/*
 * apollo_agg.h - the combined device's pure logic (gate G6, docs/spec-twinx.md 6.3-6.6): the
 * follower clock loop, the adaptive resampler's filter, the configuration checks and the logical
 * channel layout. Plain C with no kernel or CRT calls (no sin, sqrt or memset), so the driver
 * (agg.c, DISPATCH_LEVEL for the per-tick parts) and the offline test (tools/agg-test/asrc-test.c)
 * run exactly the same code. Floating point: SSE2 doubles and floats only, which x64 kernel code
 * may use without saving state (spec 6.3.3); no x87, no AVX.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include "..\include\apollo_agg.h"
#include "bringup_plan.h"       /* ApolloChanName, ApolloIoDescCount/Id, ADC and FRAME_CFG values */
#include "rate_plan.h"          /* [Rate] the frame constants below at other rates than 48 kHz */
#include "clock_plan.h"         /* [Clock] which digital slots carry audio at a rate and format (ApolloAggSlotShownAt) */

/* ------------------------------------------------------------------ */
/* Numbers                                                             */
/* ------------------------------------------------------------------ */

#define APOLLO_AGG_PI           3.14159265358979323846
#define APOLLO_AGG_RATE         48000.0
#define APOLLO_AGG_MARGIN       16          /* M: jitter margin, frames (spec 6.3.3) */
/*
 * [G7 review] The most capture frames one aggregate tick catches up (ASIO client and each combined
 * stream); a bigger lag re-originates instead. Was 4096: after a stall that was up to 4096 frames
 * of resampling in one DPC under Agg.Lock and the members' EngineLocks (tens of ms at h 32 with 128
 * inputs). 1024 frames = 21 ms of backlog; asrc-test bounds the worst case it can cost.
 */
#define APOLLO_AGG_MAX_LAG      1024u

/*
 * [Rate] The frame numbers of this file are at 48 kHz. At another rate every one that stands for a
 * time (the jitter margin M, a tick, the clock loop's thresholds) is the same time in that rate's
 * frames; the resampler's filter is normalised to Nyquist and needs nothing. Rate 0 = 48 kHz: every
 * function returns exactly what it returned before rates could change (asrc-test, aggep-test).
 */
static __inline unsigned long ApolloAggMarginAt(unsigned long Rate)
{
    return (Rate == 0) ? APOLLO_AGG_MARGIN : ApolloRateScale(APOLLO_AGG_MARGIN, Rate);
}

/* Frames of one 1 ms tick at Rate (48 at 48 kHz). */
static __inline unsigned long ApolloAggTickAt(unsigned long Rate)
{
    return (Rate == 0) ? 48ul : ApolloRateTickFrames(Rate);
}

static __inline double ApolloAggAbs(double x)
{
    return x < 0 ? -x : x;
}

/* floor() for |x| < 2^62 */
static __inline long long ApolloAggFloor(double x)
{
    long long i = (long long)x;

    return ((double)i > x) ? i - 1 : i;
}

/* sin(x): range reduction to [-pi, pi] and 14 Taylor terms (error < 1e-16 there). */
static __inline double ApolloAggSin(double x)
{
    double t, s, x2;
    int k;

    if (x > APOLLO_AGG_PI || x < -APOLLO_AGG_PI) {
        double n = x / (2.0 * APOLLO_AGG_PI);
        long long q = (long long)(n >= 0 ? n + 0.5 : n - 0.5);

        x -= (double)q * 2.0 * APOLLO_AGG_PI;
    }
    t = x;
    s = x;
    x2 = x * x;
    for (k = 1; k < 15; k++) {
        t *= -x2 / ((2.0 * k) * (2.0 * k + 1.0));
        s += t;
    }
    return s;
}

static __inline double ApolloAggSqrt(double v)
{
    double x;
    int i;

    if (v <= 0) {
        return 0;
    }
    x = v > 1 ? v : 1;
    for (i = 0; i < 80; i++) {
        double n = 0.5 * (x + v / x);

        if (n == x) {
            break;
        }
        x = n;
    }
    return x;
}

/* Modified Bessel function of the first kind, order 0 (the Kaiser window). */
static __inline double ApolloAggI0(double x)
{
    double s = 1.0, t = 1.0, q = x * x / 4.0;
    int k;

    for (k = 1; k < 80; k++) {
        t *= q / ((double)k * (double)k);
        s += t;
        if (t < s * 1e-17) {
            break;
        }
    }
    return s;
}

/* ------------------------------------------------------------------ */
/* The resampler's filter (spec 6.3.3)                                 */
/* ------------------------------------------------------------------ */

/*
 * Polyphase windowed sinc: 2h taps, 1024 phases plus the end row, linear interpolation between
 * adjacent rows. Row p holds the weights for an output at fractional position p/1024 past input
 * frame n: tap j multiplies input frame n - h + 1 + j. Each row is normalised to a DC gain of 1.
 * The ratio stays within 500 ppm of 1, so the filter only acts as a slowly moving fractional
 * delay: the sinc runs at the full band (cutoff Fc of Nyquist) and the Kaiser window trades
 * flatness near 20 kHz against the ripple (tools/agg-test/asrc-test.c measures both).
 */
#define APOLLO_ASRC_PHASES      1024        /* 256 left a -94 dB row-interpolation error (asrc-test) */
#define APOLLO_ASRC_MAX_HALF    32
/*
 * Kaiser beta and sinc cutoff (fraction of Nyquist) per size, from asrc-test --sweep (24-bit
 * sines through a fractional delay drifting at 4.74 ppm; error = everything but the exact sine):
 *   h 16, beta 10, cutoff 1.00: flat within 0.0002 dB to 18 kHz, 0.026 dB at 20 kHz;
 *                               error 1 kHz -106 dB, 10 kHz -104 dB, 18 kHz -111 dB, 20 kHz -52 dB
 *   h 32, beta 10, cutoff 0.97: flat within 0.0001 dB to 20 kHz; error -108 / -111 / -103 / -105 dB
 *   h  8, beta  8, cutoff 1.00: the low-CPU size (asrc-test prints its figures)
 */
static __inline void ApolloAsrcParams(unsigned long Half, double *Beta, double *Fc)
{
    *Beta = (Half == 8) ? 8.0 : 10.0;
    *Fc = (Half == 32) ? 0.97 : 1.0;
}

typedef struct _APOLLO_ASRC_TABLE {
    unsigned long Half;                     /* h; 0 = not built */
    float   Row[(APOLLO_ASRC_PHASES + 1) * 2 * APOLLO_ASRC_MAX_HALF];
} APOLLO_ASRC_TABLE;

static __inline int ApolloAsrcHalfOk(unsigned long Half)
{
    return Half == 8 || Half == 16 || Half == 32;
}

/* PASSIVE only in the driver (up to 66k sin/I0 evaluations). Beta/Fc: ApolloAsrcParams, or a sweep. */
static __inline void ApolloAsrcBuildEx(APOLLO_ASRC_TABLE *T, unsigned long Half, double Beta, double Fc)
{
    unsigned long p, j, n;
    double i0b = ApolloAggI0(Beta);

    if (!ApolloAsrcHalfOk(Half)) {
        Half = 16;
    }
    n = 2 * Half;
    for (p = 0; p <= APOLLO_ASRC_PHASES; p++) {
        double frac = (double)p / APOLLO_ASRC_PHASES, sum = 0;
        float *r = T->Row + p * n;

        for (j = 0; j < n; j++) {
            double t = (double)((long)j - (long)Half + 1) - frac;      /* tap distance from the output */
            double u = t / (double)Half, w, s;

            if (u <= -1.0 || u >= 1.0) {
                w = 0;
            } else {
                w = ApolloAggI0(Beta * ApolloAggSqrt(1.0 - u * u)) / i0b;
            }
            s = (t == 0) ? Fc : ApolloAggSin(APOLLO_AGG_PI * Fc * t) / (APOLLO_AGG_PI * t);
            r[j] = (float)(s * w);
            sum += s * w;
        }
        for (j = 0; j < n && sum != 0; j++) {
            r[j] = (float)(r[j] / sum);
        }
    }
    T->Half = Half;
}

static __inline void ApolloAsrcBuild(APOLLO_ASRC_TABLE *T, unsigned long Half)
{
    double beta, fc;

    if (!ApolloAsrcHalfOk(Half)) {
        Half = 16;
    }
    ApolloAsrcParams(Half, &beta, &fc);
    ApolloAsrcBuildEx(T, Half, beta, fc);
}

/* The 2h weights for fractional position Frac in [0, 1). */
static __inline void ApolloAsrcWeights(const APOLLO_ASRC_TABLE *T, double Frac, float *W)
{
    unsigned long n = 2 * T->Half, j;
    double x = Frac * APOLLO_ASRC_PHASES;
    long p = (long)x;
    float b, a;
    const float *r0, *r1;

    if (p < 0) {
        p = 0;
    }
    if (p > APOLLO_ASRC_PHASES - 1) {
        p = APOLLO_ASRC_PHASES - 1;
    }
    b = (float)(x - (double)p);
    if (b < 0) {
        b = 0;
    }
    if (b > 1) {
        b = 1;
    }
    a = 1.0f - b;
    r0 = T->Row + (unsigned long)p * n;
    r1 = r0 + n;
    for (j = 0; j < n; j++) {
        W[j] = a * r0[j] + b * r1[j];
    }
}

/* A float accumulator back to a ring sample (S32, 24 valid bits at the top), saturated. */
static __inline long ApolloAsrcToSample(float V)
{
    if (V >= 2147483520.0f) {
        return 0x7FFFFF00L;
    }
    if (V <= -2147483648.0f) {
        return (long)0x80000000UL;
    }
    return (long)V;
}

/*
 * [polish Nov 15] One combined sample, the rule every combined client follows (spec 6.3.3, 6.4: "master
 * channels: read and written directly in the master ring"). agg.c's ASIO capture, combined Windows capture
 * and follower render all take their samples here, and tools\agg-test\aggep-test proves it offline:
 *   - the timeline unit's channels are an integer copy of its ring at the same frame: no filter, no gain,
 *     no fade, so bit-exact (ApolloAggRingSample); its render clients ADD straight into its ring;
 *   - only another unit's (a follower's) go through the ASRC (ApolloAggAsrcSample), as before.
 * Rings are Frames (a power of two) x Channels S32 samples, frame after frame.
 */
static __inline int ApolloAggResampled(unsigned long U, unsigned long Timeline)
{
    return U != Timeline;
}

static __inline long ApolloAggRingSample(const long *Ring, unsigned long Frames, unsigned long Channels,
                                         unsigned long Slot, unsigned long long X)
{
    if (Slot >= Channels) {
        return 0;
    }
    return Ring[(X & (unsigned long long)(Frames - 1)) * Channels + Slot];
}

/* A follower's sample: the Taps weights W over ring frames Base, Base + 1, ... (wrapping), times Gain. */
static __inline long ApolloAggAsrcSample(const long *Ring, unsigned long Frames, unsigned long Channels,
                                         unsigned long Slot, long long Base, const float *W, unsigned long Taps,
                                         float Gain)
{
    unsigned long j;
    float acc = 0;

    if (Slot >= Channels) {
        return 0;
    }
    for (j = 0; j < Taps; j++) {
        acc += W[j] * (float)Ring[((unsigned long long)(Base + (long long)j) & (unsigned long long)(Frames - 1)) * Channels +
                                  Slot];
    }
    return ApolloAsrcToSample(acc * Gain);
}

/* ------------------------------------------------------------------ */
/* The follower clock loop (spec 6.3.2, 6.3.4)                         */
/* ------------------------------------------------------------------ */

/*
 * Map phi(x) = F0 + R * (x - M0): timeline frame x -> follower frame. Each update measures
 * e = Pf(tm) - phi(Pm) (the follower position projected to the timeline read's QPC instant
 * minus the map), re-anchors the map at Pm (so phi stays continuous whatever R does next) and
 * steers R with a PI loop on e (a DLL as in zita-ajbridge): second order, damping 0.707,
 * natural frequency 1 Hz while acquiring (the first 2 s), 0.05 Hz while tracking. Positions are
 * compared with positions, so the PC's QPC error cancels. Raw errors beyond 2 frames are clamped
 * (a tick whose QPC stamp came late); 3 in a row beyond 12 frames (M - 4) = a position jump:
 * resync (re-anchor at the measured follower position, R kept). Lock = the averaged |e| below
 * 0.5 frame for 250 ms. R outside 1 +/- 500 ppm or 3 resyncs within 10 s: degraded (muted),
 * re-formed 10 s later.
 */
#define APOLLO_AGG_E_CLAMP          2.0
#define APOLLO_AGG_RESYNC_FRAMES    ((double)(APOLLO_AGG_MARGIN - 4))
#define APOLLO_AGG_LOCK_FRAMES      0.5
#define APOLLO_AGG_LOCK_S           0.25
#define APOLLO_AGG_ACQUIRE_S        2.0
#define APOLLO_AGG_RATIO_LIMIT      500e-6
#define APOLLO_AGG_DEGRADED_S       10.0

#define APOLLO_AGG_EV_NONE          0
#define APOLLO_AGG_EV_LOCKED        1
#define APOLLO_AGG_EV_RESYNC        2
#define APOLLO_AGG_EV_DEGRADED      3
#define APOLLO_AGG_EV_REFORM        4

typedef struct _APOLLO_AGG_CLOCK {
    int         State;          /* APOLLO_AGG_CLOCK_* */
    double      F0;             /* follower frame at timeline frame M0 */
    long long   M0;
    double      R;              /* follower frames per timeline frame, as applied */
    double      Ri;             /* integrator */
    double      ELp;            /* error fed to the loop (16-tick average) */
    double      EFilt;          /* lock detector (64-tick average) */
    double      EPeak;          /* largest |raw e| */
    double      Since;          /* seconds since formation */
    double      Good;           /* seconds |EFilt| has been below the lock threshold */
    double      Locked;         /* seconds since the lock (0 when not locked) */
    double      ResyncAt[3];    /* Since of the last three resyncs */
    unsigned long BadRun;
    unsigned long Resyncs;
    unsigned long Outliers;
    double      Rate;           /* [Rate] Hz of the timeline (frames per second); 0 = 48 kHz. Kept by a formation */
} APOLLO_AGG_CLOCK;

static __inline double ApolloAggMap(const APOLLO_AGG_CLOCK *C, long long X)
{
    return C->F0 + C->R * (double)(X - C->M0);
}

static __inline double ApolloAggUnmap(const APOLLO_AGG_CLOCK *C, double K)
{
    return (double)C->M0 + (K - C->F0) / C->R;
}

/* Formation (spec 6.3.4): the map starts on the measured positions, R from the last run (else 1). */
static __inline void ApolloAggClockForm(APOLLO_AGG_CLOCK *C, long long Pm, double PfAtTm, double R0)
{
    unsigned long resyncs = C->Resyncs, outliers = C->Outliers;
    double rate = C->Rate;              /* [Rate] the caller sets it before forming */
    unsigned long i;

    for (i = 0; i < sizeof(*C); i++) {
        ((unsigned char *)C)[i] = 0;
    }
    if (R0 < 1.0 - APOLLO_AGG_RATIO_LIMIT || R0 > 1.0 + APOLLO_AGG_RATIO_LIMIT) {
        R0 = 1.0;
    }
    C->State = APOLLO_AGG_CLOCK_ACQUIRE;
    C->F0 = PfAtTm;
    C->M0 = Pm;
    C->R = R0;
    C->Ri = R0;
    C->ResyncAt[0] = C->ResyncAt[1] = C->ResyncAt[2] = -1000.0;
    C->Resyncs = resyncs;
    C->Outliers = outliers;
    C->Rate = rate;
}

/*
 * One measurement: Pm = timeline position, PfAtTm = follower position at the same instant.
 * Returns APOLLO_AGG_EV_*. The caller mutes the follower unless State is LOCKED.
 */
static __inline int ApolloAggClockUpdate(APOLLO_AGG_CLOCK *C, long long Pm, double PfAtTm)
{
    double phi, e, ec, kp, ki, dt, wn;
    /* [Rate] frames per second, and the frame thresholds as the same time at it (1.0 at 48 kHz: exact) */
    double fs = (C->Rate > 0) ? C->Rate : APOLLO_AGG_RATE;
    double sc = (C->Rate > 0) ? C->Rate / APOLLO_AGG_RATE : 1.0;

    if (C->State == APOLLO_AGG_CLOCK_NONE) {
        return APOLLO_AGG_EV_NONE;
    }
    dt = (double)(Pm - C->M0) / fs;
    if (dt < 0 || dt > 0.25) {
        /* the timeline went backwards or stalled for long: start over from the positions */
        ApolloAggClockForm(C, Pm, PfAtTm, C->R);
        return APOLLO_AGG_EV_REFORM;
    }
    phi = ApolloAggMap(C, Pm);
    e = PfAtTm - phi;
    C->F0 = phi;                    /* re-anchor: continuous map */
    C->M0 = Pm;
    C->Since += dt;
    if (C->State == APOLLO_AGG_CLOCK_DEGRADED) {
        if (C->Since - C->ResyncAt[2] >= APOLLO_AGG_DEGRADED_S && C->Since >= APOLLO_AGG_DEGRADED_S) {
            ApolloAggClockForm(C, Pm, PfAtTm, 1.0);
            return APOLLO_AGG_EV_REFORM;
        }
        return APOLLO_AGG_EV_NONE;
    }
    if (ApolloAggAbs(e) > C->EPeak) {
        C->EPeak = ApolloAggAbs(e);
    }
    if (ApolloAggAbs(e) > APOLLO_AGG_RESYNC_FRAMES * sc) {
        if (++C->BadRun < 3) {
            return APOLLO_AGG_EV_NONE;      /* one late stamp: wait for the next ones */
        }
        C->BadRun = 0;
        C->Resyncs++;
        C->ResyncAt[0] = C->ResyncAt[1];
        C->ResyncAt[1] = C->ResyncAt[2];
        C->ResyncAt[2] = C->Since;
        C->F0 = PfAtTm;                     /* jump to the measured position, R kept */
        C->ELp = C->EFilt = 0;
        C->Good = C->Locked = 0;
        if (C->ResyncAt[2] - C->ResyncAt[0] < 10.0) {
            C->State = APOLLO_AGG_CLOCK_DEGRADED;
            return APOLLO_AGG_EV_DEGRADED;
        }
        C->State = APOLLO_AGG_CLOCK_ACQUIRE;
        return APOLLO_AGG_EV_RESYNC;
    }
    C->BadRun = 0;
    ec = e;
    if (ec > APOLLO_AGG_E_CLAMP * sc || ec < -APOLLO_AGG_E_CLAMP * sc) {
        C->Outliers++;
        ec = (ec > 0) ? APOLLO_AGG_E_CLAMP * sc : -APOLLO_AGG_E_CLAMP * sc;
    }
    /*
     * The loop's input is averaged over 16 ticks while acquiring and over 512 (a 0.3 Hz pole, six
     * times the tracking bandwidth) while tracking: the proportional path would otherwise pass the
     * read noise (0.3 frame rms per tick, measured) into R as about 1 ppm of jitter.
     */
    C->ELp += (ec - C->ELp) * ((C->Since < APOLLO_AGG_ACQUIRE_S) ? (1.0 / 16.0) : (1.0 / 512.0));
    C->EFilt += (ec - C->EFilt) * (1.0 / 64.0);
    /* omega_n^2 = Fs * Ki, 2 zeta omega_n = Fs * Kp (de/dt = Fs (r_true - R)) */
    wn = 2.0 * APOLLO_AGG_PI * ((C->Since < APOLLO_AGG_ACQUIRE_S) ? 1.0 : 0.05);
    ki = wn * wn / fs;
    kp = 2.0 * 0.7071 * wn / fs;
    C->Ri += ki * C->ELp * dt;
    C->R = C->Ri + kp * C->ELp;
    if (C->R < 1.0 - APOLLO_AGG_RATIO_LIMIT || C->R > 1.0 + APOLLO_AGG_RATIO_LIMIT) {
        C->State = APOLLO_AGG_CLOCK_DEGRADED;
        C->ResyncAt[2] = C->Since;
        C->R = C->Ri = 1.0;
        return APOLLO_AGG_EV_DEGRADED;
    }
    if (ApolloAggAbs(C->EFilt) < APOLLO_AGG_LOCK_FRAMES * sc) {
        C->Good += dt;
    } else {
        C->Good = 0;
    }
    if (C->State == APOLLO_AGG_CLOCK_LOCKED) {
        C->Locked += dt;
        if (ApolloAggAbs(C->EFilt) > 2.0 * sc) {
            C->State = APOLLO_AGG_CLOCK_ACQUIRE;    /* lost it slowly: mute until it settles */
            C->Locked = 0;
        }
    } else if (C->Good >= APOLLO_AGG_LOCK_S) {
        C->State = APOLLO_AGG_CLOCK_LOCKED;
        C->Locked = 0;
        return APOLLO_AGG_EV_LOCKED;
    }
    return APOLLO_AGG_EV_NONE;
}

/* ------------------------------------------------------------------ */
/* Configuration and layout (spec 6.6, 6.8, 6.9)                       */
/* ------------------------------------------------------------------ */

static __inline unsigned long ApolloAggModelAdc(unsigned long Model)
{
    return Model == APOLLO_INFO_MODEL_TWINX ? APOLLO_TWX_ADC_FRAMES : APOLLO_X8P_ADC_FRAMES;
}

static __inline const char *ApolloAggModelTag(unsigned long Model)
{
    return Model == APOLLO_INFO_MODEL_X8P ? "x8p" : (Model == APOLLO_INFO_MODEL_TWINX ? "TwinX" : "Unit");
}

/*
 * Simple mode lists every descriptor slot except slots with no jack: the Twin X Gen 1's frame is
 * the firmware-wide one (spec 6.9.3): no MIC/LINE 3-4 or LINE 5-8 inputs, no LINE 5-8, ADAT or
 * S/PDIF outputs, no RESERVED anywhere. The x8p shows every slot, as its ASIO driver always did.
 */
static __inline int ApolloAggSlotShown(unsigned long Model, int Record, unsigned short Id)
{
    unsigned long type = (unsigned long)Id >> 8, num = (unsigned long)Id & 0xFFul;

    if (Id == 0 || type == 0x0C) {
        return 0;
    }
    if (Model == APOLLO_INFO_MODEL_TWINX) {
        if (type == 0x03 && num >= 5) {
            return 0;
        }
        if (Record && (Id == 0x0103 || Id == 0x0104)) {
            return 0;
        }
        if (!Record && (type == 0x04 || type == 0x05)) {
            return 0;
        }
    }
    return 1;
}

/*
 * [Clock] Simple mode at the unit's rate and digital formats: a slot with a jack, and (once a format was chosen with
 * SET_CLOCK, APOLLO_DIG_KNOWN) only the ADAT and S/PDIF slots that carry audio then: ADAT in its format and up to the
 * S/MUX count of the rate (Twin X ADAT 5-8 not at 88.2/96 kHz, no ADAT at 176.4/192), S/PDIF in its format and up to
 * 96 kHz (clock_plan.h ApolloDigSlotLive). Dig 0 (nothing chosen) = ApolloAggSlotShown exactly, at every rate.
 */
static __inline int ApolloAggSlotShownAt(unsigned long Model, int Record, unsigned short Id, unsigned long Rate,
                                         unsigned long Dig)
{
    return ApolloAggSlotShown(Model, Record, Id) && ApolloDigSlotLive((int)Model, Record, Id, Rate, Dig);
}

static __inline unsigned long ApolloAggValidate(const APOLLO_AGG_CONFIG *C)
{
    unsigned long i, k;

    if (C->Version != APOLLO_AGG_VERSION || C->Bytes != sizeof(APOLLO_AGG_CONFIG)) {
        return APOLLO_AGG_ERR_VERSION;
    }
    if (C->UnitCount == 0 || C->UnitCount > APOLLO_AGG_MAX_UNITS) {
        return APOLLO_AGG_ERR_UNITS;
    }
    for (i = 0; i < C->UnitCount; i++) {
        if (C->Unit[i].Uid == 0 || C->Unit[i].InCount > APOLLO_AGG_UNIT_SLOTS ||
            C->Unit[i].OutCount > APOLLO_AGG_UNIT_SLOTS) {
            return APOLLO_AGG_ERR_UNITS;
        }
        for (k = 0; k < i; k++) {
            if (C->Unit[k].Uid == C->Unit[i].Uid) {
                return APOLLO_AGG_ERR_DUPLICATE;
            }
        }
    }
    if (C->Master >= C->UnitCount) {
        return APOLLO_AGG_ERR_MASTER;
    }
    if (!ApolloAsrcHalfOk(C->AsrcHalf)) {
        return APOLLO_AGG_ERR_ASRC;
    }
    if (C->InCount > APOLLO_AGG_MAX_CH || C->OutCount > APOLLO_AGG_MAX_CH) {
        return APOLLO_AGG_ERR_CHANNELS;
    }
    for (i = 0; i < C->InCount; i++) {
        if (C->In[i].Unit >= C->UnitCount || C->In[i].Slot >= APOLLO_AGG_UNIT_SLOTS) {
            return APOLLO_AGG_ERR_CHANNELS;
        }
    }
    for (i = 0; i < C->OutCount; i++) {
        if (C->Out[i].Unit >= C->UnitCount || C->Out[i].Slot >= APOLLO_AGG_UNIT_SLOTS) {
            return APOLLO_AGG_ERR_CHANNELS;
        }
    }
    if (C->RouteCount > APOLLO_AGG_MAX_ROUTES) {
        return APOLLO_AGG_ERR_ROUTES;
    }
    for (i = 0; i < C->RouteCount; i++) {
        const APOLLO_AGG_ROUTE_CFG *r = &C->Route[i];

        if (r->SrcKind < APOLLO_AGG_SRC_DAW || r->SrcKind > APOLLO_AGG_SRC_PORT || r->DstUnit >= C->UnitCount ||
            (r->SrcKind == APOLLO_AGG_SRC_PORT && r->SrcUnit >= C->UnitCount)) {
            return APOLLO_AGG_ERR_ROUTES;
        }
    }
    if (C->JsonBytes > APOLLO_AGG_JSON_MAX) {
        return APOLLO_AGG_ERR_JSON;
    }
    return 0;
}

/* What the layout needs to know about each configured unit right now. */
typedef struct _APOLLO_AGG_UNIT_RT {
    int             Present;        /* joined: bring-up done, transport and engine started */
    unsigned long   Guard, Lead, Adc, FrameCfg;
    long            CapTrim, RenTrim;   /* [G7] CaptureTrimFrames / RenderTrimFrames, -64..64 (0 = as G6) */
    unsigned long   Rate;               /* [Rate] Hz the unit runs at (its Guard, Lead, Adc, FrameCfg are at it); 0 = 48 kHz */
    unsigned long   Dig;                /* [Clock] the unit's digital formats (clock_plan.h APOLLO_DIG_*); 0 = none chosen */
} APOLLO_AGG_UNIT_RT;

/* ------------------------------------------------------------------ */
/* [G7] Alignment, trims (spec 6.3.5)                                  */
/* ------------------------------------------------------------------ */

#define APOLLO_AGG_TRIM_MAX         64

/* A registry DWORD read as signed frames, clamped to +/-64. */
static __inline long ApolloAggTrimOf(unsigned long Raw)
{
    long v = (long)Raw;

    if (v > APOLLO_AGG_TRIM_MAX) {
        return APOLLO_AGG_TRIM_MAX;
    }
    if (v < -APOLLO_AGG_TRIM_MAX) {
        return -APOLLO_AGG_TRIM_MAX;
    }
    return v;
}

/*
 * Where a follower's capture channels read, in its own frames relative to phi(x) for timeline frame x
 * (spec 6.4). Aligned (APOLLO_AGG_CFG_ALIGN): the ADC difference to the timeline, so both units'
 * samples of one analog instant land on one frame, minus the trim difference (a positive
 * CaptureTrim on a unit delays its channels). With every trim 0 this is G6's DAdc exactly
 * (tools/agg-test/aggep-test.c). Unaligned ("lowest latency"): as fresh as the resampler's
 * look-ahead and the margin allow, h + M behind; trims do not apply.
 * Only the difference to the timeline's trim counts: the timeline's own channels never move.
 */
static __inline long ApolloAggCapOffset(const APOLLO_AGG_CONFIG *C, const APOLLO_AGG_UNIT_RT *Rt,
                                        unsigned long Tl, unsigned long U)
{
    if ((C->Flags & APOLLO_AGG_CFG_ALIGN) == 0) {
        return -(long)(C->AsrcHalf + ApolloAggMarginAt(Rt[Tl].Rate));
    }
    return ((long)Rt[U].Adc - (long)Rt[Tl].Adc) - (Rt[U].CapTrim - Rt[Tl].CapTrim);
}

/*
 * [G7 review] How much earlier than the timeline a follower may play (a positive render offset reads
 * staging ahead of the frames it plays). The staging is written up to Pm + the timeline's lead; the
 * follower's ring frames must be written before Pf + FRAME_CFG + 4 is fetched, one tick (48 frames)
 * later at the latest, with the resampler's look-ahead h and the jitter margin M on top. More than
 * that and every tick finds its frames already fetched (RenderLate, silence). Defaults: 52 frames at
 * h 16, 36 at h 32, 60 at h 8.
 */
#define APOLLO_AGG_TICK_FRAMES      48
/* [Rate] At Rate (0 = 48 kHz): M and the tick as that rate's frames. */
static __inline long ApolloAggRenOffsetMaxAt(unsigned long TlLead, unsigned long Half, unsigned long FollowerFrameCfg,
                                             unsigned long Rate)
{
    long m = (long)TlLead - (long)Half - (long)ApolloAggMarginAt(Rate) - (long)FollowerFrameCfg - 4 -
             (long)ApolloAggTickAt(Rate);

    return m > 0 ? m : 0;
}

static __inline long ApolloAggRenOffsetMax(unsigned long TlLead, unsigned long Half, unsigned long FollowerFrameCfg)
{
    return ApolloAggRenOffsetMaxAt(TlLead, Half, FollowerFrameCfg, 0);
}

/*
 * A follower's playback: its ring frame k plays the staging (timeline) frame Unmap(k) + this, so a
 * positive RenderTrim plays that unit later. 0 with equal trims (G6). Not affected by ALIGN: output
 * latency is the same on every unit by construction (6.3.3).
 * [G7 review] "Earlier" (positive) is clamped to ApolloAggRenOffsetMax; "later" needs only older staging
 * (the ring keeps 4096 frames), so -128 is fine.
 */
static __inline long ApolloAggRenOffsetRaw(const APOLLO_AGG_UNIT_RT *Rt, unsigned long Tl, unsigned long U)
{
    return -(Rt[U].RenTrim - Rt[Tl].RenTrim);
}

static __inline long ApolloAggRenOffset(const APOLLO_AGG_UNIT_RT *Rt, unsigned long Tl, unsigned long U,
                                        unsigned long Half)
{
    long raw = ApolloAggRenOffsetRaw(Rt, Tl, U);
    long max = ApolloAggRenOffsetMaxAt(Rt[Tl].Lead, Half, Rt[U].FrameCfg, Rt[Tl].Rate);   /* [Rate] */

    return raw > max ? max : raw;
}

typedef struct _APOLLO_AGG_DEST {
    unsigned char   Unit;
    unsigned char   Pad;
    unsigned short  Slot;
} APOLLO_AGG_DEST;

typedef struct _APOLLO_AGG_COMPILED {
    unsigned long   InCount, OutCount;
    unsigned long   InLatency, OutLatency;  /* aligned, without the buffer */
    unsigned long   Pad;                    /* capture frames every channel is delayed by (alignment) */
    APOLLO_AGG_LCHAN In[APOLLO_AGG_MAX_CH];
    APOLLO_AGG_LCHAN Out[APOLLO_AGG_MAX_CH];
    unsigned char   DestCount[APOLLO_AGG_MAX_CH];
    APOLLO_AGG_DEST Dest[APOLLO_AGG_MAX_CH][APOLLO_AGG_MAX_DESTS];
} APOLLO_AGG_COMPILED;

static __inline void ApolloAggName(char *Out, unsigned long Len, unsigned long Model, int Record, unsigned short Id,
                                   unsigned long Slot)
{
    unsigned long n = 0, type = (unsigned long)Id >> 8, num = (unsigned long)Id & 0xFFul;

    ApolloStrPut(Out, Len, &n, ApolloAggModelTag(Model));
    ApolloStrPut(Out, Len, &n, " ");
    if (Model == APOLLO_INFO_MODEL_X8P && Record && num >= 1 && num <= 8 && (type == 0x00 || type == 0x01 || type == 0x03)) {
        /* the firmware-wide frame names the first 8 inputs MIC/LINE/HIZ, MIC/LINE, LINE: all 8 are preamps on the x8p */
        ApolloStrPut(Out, Len, &n, "MIC/LINE ");
        ApolloStrPutU(Out, Len, &n, num);
    } else if (Id != 0) {
        n += ApolloChanName(Id, Out + n, Len - n);
    } else {
        ApolloStrPut(Out, Len, &n, "slot ");
        ApolloStrPutU(Out, Len, &n, Slot + 1);
    }
}

/* The playback slot carrying Id on unit U (from its cached descriptor), or -1. */
static __inline long ApolloAggOutSlot(const APOLLO_AGG_CONFIG *C, unsigned long U, unsigned short Id)
{
    unsigned long i;

    for (i = 0; i < C->Unit[U].OutCount && i < APOLLO_AGG_UNIT_SLOTS; i++) {
        if (C->Unit[U].OutIds[i] == Id) {
            return (long)i;
        }
    }
    return -1;
}

/*
 * The aligned latencies (spec 6.3.5): capture delays every channel to the slowest unit:
 * pad = h + M + the largest ADC excess of another unit over the timeline's. Only when the
 * configuration has more than one unit (present or not), so a unit coming and going does not
 * move the reported latency.
 * [G7] The excess is the largest follower read offset (ApolloAggCapOffset: ADC difference minus the
 * trim difference; with every trim 0 exactly G6's). Unaligned (ALIGN clear): no pad at all, the
 * reported latency is the timeline's own and followers read h + M behind (their latency is longer,
 * which ASIO cannot express: Apollo Control says so).
 */
static __inline unsigned long ApolloAggPad(const APOLLO_AGG_CONFIG *C, const APOLLO_AGG_UNIT_RT *Rt,
                                           unsigned long Timeline)
{
    unsigned long u, tl = (Timeline < C->UnitCount) ? Timeline : C->Master;
    long extra = 0;

    if (C->UnitCount < 2 || (C->Flags & APOLLO_AGG_CFG_ALIGN) == 0) {
        return 0;
    }
    for (u = 0; u < C->UnitCount; u++) {
        long need;

        if (u == tl) {
            continue;
        }
        need = ApolloAggCapOffset(C, Rt, tl, u);
        if (need > extra) {
            extra = need;
        }
    }
    return C->AsrcHalf + ApolloAggMarginAt(Rt[tl].Rate) + (unsigned long)extra;     /* [Rate] M at the rate */
}

/*
 * The logical channel lists: whole blocks in Unit[] order (simple), or In[]/Out[] (explicit);
 * then the routes: each source pair's outputs go to every destination a route names for it,
 * instead of their own slots ("every other pair keeps its own output"). Destinations on a unit
 * whose playback descriptor is not known yet are dropped.
 */
static __inline void ApolloAggCompile(const APOLLO_AGG_CONFIG *C, const APOLLO_AGG_UNIT_RT *Rt,
                                      unsigned long Timeline, APOLLO_AGG_COMPILED *O)
{
    unsigned long u, s, i, k, tl = (Timeline < C->UnitCount) ? Timeline : C->Master;
    unsigned char routed[APOLLO_AGG_MAX_CH];

    for (i = 0; i < sizeof(*O); i++) {
        ((unsigned char *)O)[i] = 0;
    }
    for (i = 0; i < APOLLO_AGG_MAX_CH; i++) {
        routed[i] = 0;
    }
    O->Pad = ApolloAggPad(C, Rt, Timeline);
    for (k = 0; k < 2; k++) {
        int record = (k == 0);
        APOLLO_AGG_LCHAN *L = record ? O->In : O->Out;
        unsigned long *count = record ? &O->InCount : &O->OutCount;

        if (C->Flags & APOLLO_AGG_CFG_EXPLICIT) {
            const APOLLO_AGG_CHAN_CFG *E = record ? C->In : C->Out;
            unsigned long n = record ? C->InCount : C->OutCount;

            for (i = 0; i < n && *count < APOLLO_AGG_MAX_CH; i++) {
                const APOLLO_AGG_UNIT_CFG *uc;
                unsigned long cached;

                if (E[i].Unit >= C->UnitCount || (E[i].Flags & APOLLO_AGG_CHAN_HIDDEN) != 0) {
                    continue;
                }
                uc = &C->Unit[E[i].Unit];
                cached = record ? uc->InCount : uc->OutCount;
                if (cached != 0 && E[i].Slot >= cached) {
                    continue;
                }
                L[*count].Unit = E[i].Unit;
                L[*count].Slot = E[i].Slot;
                L[*count].Id = (cached != 0) ? (record ? uc->InIds[E[i].Slot] : uc->OutIds[E[i].Slot]) : 0;
                (*count)++;
            }
        } else {
            for (u = 0; u < C->UnitCount; u++) {
                const APOLLO_AGG_UNIT_CFG *uc = &C->Unit[u];
                unsigned long cached = record ? uc->InCount : uc->OutCount;

                for (s = 0; s < cached && s < APOLLO_AGG_UNIT_SLOTS && *count < APOLLO_AGG_MAX_CH; s++) {
                    unsigned short id = record ? uc->InIds[s] : uc->OutIds[s];

                    if (!ApolloAggSlotShownAt(uc->Model, record, id, Rt[u].Rate, Rt[u].Dig)) {   /* [Clock] */
                        continue;
                    }
                    L[*count].Unit = (unsigned char)u;
                    L[*count].Slot = (unsigned short)s;
                    L[*count].Id = id;
                    (*count)++;
                }
            }
        }
        for (i = 0; i < *count; i++) {
            unsigned long un = L[i].Unit;
            const APOLLO_AGG_UNIT_RT *r = &Rt[un];
            int follower = ApolloAggResampled(un, tl);

            L[i].Flags = (unsigned char)((r->Present ? APOLLO_AGG_CH_PRESENT : 0) |
                                         (follower ? APOLLO_AGG_CH_RESAMPLED : APOLLO_AGG_CH_TIMELINE));
            L[i].LatencyFrames = (unsigned short)(record ? r->Adc + r->Guard +
                                                  (follower ? C->AsrcHalf + ApolloAggMarginAt(Rt[tl].Rate) : 0) :
                                                  r->Lead + r->FrameCfg);
            ApolloAggName(L[i].Name, APOLLO_AGG_NAME_CHARS, C->Unit[un].Model, record, L[i].Id, L[i].Slot);
        }
    }
    /* Routes */
    for (k = 0; k < C->RouteCount; k++) {
        const APOLLO_AGG_ROUTE_CFG *r = &C->Route[k];
        long src[2] = { -1, -1 }, dst[2];
        unsigned long c;

        if (r->SrcKind == APOLLO_AGG_SRC_DAW) {
            if (O->OutCount >= 2) {
                src[0] = 0;
                src[1] = 1;
            }
        } else if (r->SrcKind == APOLLO_AGG_SRC_PORT) {
            for (i = 0; i < O->OutCount; i++) {
                for (c = 0; c < 2; c++) {
                    if (src[c] < 0 && O->Out[i].Unit == r->SrcUnit && O->Out[i].Id == r->SrcId[c] && r->SrcId[c] != 0) {
                        src[c] = (long)i;
                    }
                }
            }
        }
        if (r->DstUnit >= C->UnitCount) {
            continue;
        }
        dst[0] = ApolloAggOutSlot(C, r->DstUnit, r->DstId[0]);
        dst[1] = ApolloAggOutSlot(C, r->DstUnit, r->DstId[1]);
        for (c = 0; c < 2; c++) {
            unsigned long o;

            if (src[c] < 0) {
                continue;
            }
            o = (unsigned long)src[c];
            routed[o] = 1;          /* named by a route: its own slot is no longer implied */
            if (dst[c] < 0 || O->DestCount[o] >= APOLLO_AGG_MAX_DESTS) {
                continue;
            }
            O->Dest[o][O->DestCount[o]].Unit = r->DstUnit;
            O->Dest[o][O->DestCount[o]].Slot = (unsigned short)dst[c];
            O->DestCount[o]++;
        }
    }
    for (i = 0; i < O->OutCount; i++) {
        if (routed[i]) {
            unsigned long n = 0, d;

            O->Out[i].Flags |= APOLLO_AGG_CH_ROUTED;
            /* say where it plays: "x8p MON L > TwinX" (Studio), "x8p MON L > TwinX+x8p" (Mirror), "... > none" */
            while (n < APOLLO_AGG_NAME_CHARS && O->Out[i].Name[n] != 0) {
                n++;
            }
            ApolloStrPut(O->Out[i].Name, APOLLO_AGG_NAME_CHARS, &n, " >");
            for (d = 0; d < O->DestCount[i] && d < APOLLO_AGG_MAX_DESTS; d++) {
                unsigned long du = O->Dest[i][d].Unit;

                ApolloStrPut(O->Out[i].Name, APOLLO_AGG_NAME_CHARS, &n, d ? "+" : " ");
                ApolloStrPut(O->Out[i].Name, APOLLO_AGG_NAME_CHARS, &n,
                             du < APOLLO_AGG_MAX_UNITS ? ApolloAggModelTag(C->Unit[du].Model) : "?");
            }
            if (O->DestCount[i] == 0) {
                ApolloStrPut(O->Out[i].Name, APOLLO_AGG_NAME_CHARS, &n, " none");
            }
        } else {
            O->Dest[i][0].Unit = O->Out[i].Unit;
            O->Dest[i][0].Slot = O->Out[i].Slot;
            O->DestCount[i] = 1;
        }
    }
    O->InLatency = Rt[tl].Guard + O->Pad + Rt[tl].Adc;
    O->OutLatency = Rt[tl].Lead + Rt[tl].FrameCfg;
}

/* Do two compiled layouts list the same channels (what ASIO clients see)? Flags may differ. */
static __inline int ApolloAggSameLists(const APOLLO_AGG_COMPILED *A, const APOLLO_AGG_COMPILED *B)
{
    unsigned long i;

    if (A->InCount != B->InCount || A->OutCount != B->OutCount || A->InLatency != B->InLatency ||
        A->OutLatency != B->OutLatency) {
        return 0;
    }
    for (i = 0; i < A->InCount; i++) {
        if (A->In[i].Unit != B->In[i].Unit || A->In[i].Slot != B->In[i].Slot || A->In[i].Id != B->In[i].Id) {
            return 0;
        }
    }
    for (i = 0; i < A->OutCount; i++) {
        if (A->Out[i].Unit != B->Out[i].Unit || A->Out[i].Slot != B->Out[i].Slot || A->Out[i].Id != B->Out[i].Id) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* [G7] The combined Windows endpoints (spec 6.7, 6.10)                */
/* ------------------------------------------------------------------ */

/*
 * One table, created on the host unit (the x8p, upstream of the Twin X: spec 6.2) when
 * Parameters\Aggregate\EndpointSet is 1; every other unit then creates no circuits at all. Each row
 * is an ACX circuit whose streams run on the aggregate timeline (agg.c), through its routing and
 * resampler like the combined ASIO client:
 *   MONITOR   render: every destination of the "windows" routes (Studio: the Twin X MON = the
 *             speakers), else the configured master's MON L/R. Windows audio ADDs into those slots
 *             with saturation, so it sums with the DAW on the same outputs.
 *   LOOPBACK  capture: the monitor mix (record IDs 0x0901/0x0902) of the first "windows" route's
 *             unit, else the master's. Never a default device (INF NeverSetAsDefault).
 *   INPUT     capture: one unit's input by model and channel IDs (the first configured unit of that
 *             model); silent while that unit is absent or its descriptor does not carry the IDs.
 *   OUTPUT    [Endpoints] render: one unit's output jack by model and channel IDs, the same way (a
 *             follower's through the staging rings and the resampler, like ASIO's outputs).
 * The list is the single source for endpoints.c (ACX rows), this header's pure table (offline test)
 * and, by test, apollo.inf (aggep-test parses the INF and requires the same reference strings).
 * [Endpoints] Nov 15: every output jack of both units (endpoint_plan.h's standalone rows, the pin names
 * prefixed with the unit), the inputs of both units by default, VIRTUAL and S/PDIF inputs opt-in; new
 * reference strings ("Ap2C...") so no endpoint inherits a name Windows cached for the G7 rows.
 *
 * X(sym, reference string, pin name, kind, render, channels, bridge, NeverSetAsDefault, in the default
 *   set, model, id0, id1, fallback slot 0, fallback slot 1)
 * bridge: LINE_CONNECTOR (Windows then keeps the pin name: a Speaker form factor would be renamed
 * "Speakers"), HEADPHONES (the HP jacks) or MICROPHONE (what conferencing apps look for). Mono rows
 * repeat channel 0.
 */
#define APOLLO_AGG_EP_MONITOR       0
#define APOLLO_AGG_EP_LOOPBACK      1
#define APOLLO_AGG_EP_INPUT         2
#define APOLLO_AGG_EP_OUTPUT        3   /* [Endpoints] */

#define APOLLO_AGG_EP_LIST(X) \
    X(AggMon,       "Ap2CMon",        "Monitor",            APOLLO_AGG_EP_MONITOR,  1, 2, LINE_CONNECTOR, 0, 1, APOLLO_INFO_MODEL_NONE,  0x0901, 0x0902,  0,  1) \
    X(AggX8pMon,    "Ap2CX8pMon",     "x8p MON L/R",        APOLLO_AGG_EP_OUTPUT,   1, 2, LINE_CONNECTOR, 0, 1, APOLLO_INFO_MODEL_X8P,   0x0901, 0x0902,  0,  1) \
    X(AggX8pLine12, "Ap2CX8pLine12",  "x8p LINE 1-2",       APOLLO_AGG_EP_OUTPUT,   1, 2, LINE_CONNECTOR, 0, 1, APOLLO_INFO_MODEL_X8P,   0x0301, 0x0302,  2,  3) \
    X(AggX8pLine34, "Ap2CX8pLine34",  "x8p LINE 3-4",       APOLLO_AGG_EP_OUTPUT,   1, 2, LINE_CONNECTOR, 0, 1, APOLLO_INFO_MODEL_X8P,   0x0303, 0x0304,  4,  5) \
    X(AggX8pLine56, "Ap2CX8pLine56",  "x8p LINE 5-6",       APOLLO_AGG_EP_OUTPUT,   1, 2, LINE_CONNECTOR, 0, 1, APOLLO_INFO_MODEL_X8P,   0x0305, 0x0306,  6,  7) \
    X(AggX8pLine78, "Ap2CX8pLine78",  "x8p LINE 7-8",       APOLLO_AGG_EP_OUTPUT,   1, 2, LINE_CONNECTOR, 0, 1, APOLLO_INFO_MODEL_X8P,   0x0307, 0x0308,  8,  9) \
    X(AggX8pHp1,    "Ap2CX8pHp1",     "x8p HP 1",           APOLLO_AGG_EP_OUTPUT,   1, 2, HEADPHONES,     0, 1, APOLLO_INFO_MODEL_X8P,   0x0A01, 0x0A02, 28, 29) \
    X(AggX8pHp2,    "Ap2CX8pHp2",     "x8p HP 2",           APOLLO_AGG_EP_OUTPUT,   1, 2, HEADPHONES,     0, 1, APOLLO_INFO_MODEL_X8P,   0x0A03, 0x0A04, 30, 31) \
    X(AggX8pSpdif,  "Ap2CX8pSpdif",   "x8p S/PDIF",         APOLLO_AGG_EP_OUTPUT,   1, 2, LINE_CONNECTOR, 0, 1, APOLLO_INFO_MODEL_X8P,   0x0501, 0x0502, 18, 19) \
    X(AggTwxMon,    "Ap2CTwxMon",     "Twin X MON L/R",     APOLLO_AGG_EP_OUTPUT,   1, 2, LINE_CONNECTOR, 0, 1, APOLLO_INFO_MODEL_TWINX, 0x0901, 0x0902,  0,  1) \
    X(AggTwxLine34, "Ap2CTwxLine34",  "Twin X LINE 3-4",    APOLLO_AGG_EP_OUTPUT,   1, 2, LINE_CONNECTOR, 0, 1, APOLLO_INFO_MODEL_TWINX, 0x0A03, 0x0A04, 30, 31) \
    X(AggTwxHp,     "Ap2CTwxHp",      "Twin X HP",          APOLLO_AGG_EP_OUTPUT,   1, 2, HEADPHONES,     0, 1, APOLLO_INFO_MODEL_TWINX, 0x0A01, 0x0A02, 28, 29) \
    X(AggMonIn,     "Ap2CMix",        "Monitor mix",        APOLLO_AGG_EP_LOOPBACK, 0, 2, LINE_CONNECTOR, 1, 1, APOLLO_INFO_MODEL_NONE,  0x0901, 0x0902, 26, 27) \
    X(AggX8pIn1,    "Ap2CX8pIn1",     "x8p MIC/LINE 1",     APOLLO_AGG_EP_INPUT,    0, 1, MICROPHONE,     0, 1, APOLLO_INFO_MODEL_X8P,   0x0001, 0x0001,  0,  0) \
    X(AggX8pIn2,    "Ap2CX8pIn2",     "x8p MIC/LINE 2",     APOLLO_AGG_EP_INPUT,    0, 1, MICROPHONE,     0, 1, APOLLO_INFO_MODEL_X8P,   0x0002, 0x0002,  1,  1) \
    X(AggX8pIn3,    "Ap2CX8pIn3",     "x8p MIC/LINE 3",     APOLLO_AGG_EP_INPUT,    0, 1, MICROPHONE,     0, 1, APOLLO_INFO_MODEL_X8P,   0x0103, 0x0103,  2,  2) \
    X(AggX8pIn4,    "Ap2CX8pIn4",     "x8p MIC/LINE 4",     APOLLO_AGG_EP_INPUT,    0, 1, MICROPHONE,     0, 1, APOLLO_INFO_MODEL_X8P,   0x0104, 0x0104,  3,  3) \
    X(AggX8pIn5,    "Ap2CX8pIn5",     "x8p MIC/LINE 5",     APOLLO_AGG_EP_INPUT,    0, 1, MICROPHONE,     0, 1, APOLLO_INFO_MODEL_X8P,   0x0305, 0x0305,  4,  4) \
    X(AggX8pIn6,    "Ap2CX8pIn6",     "x8p MIC/LINE 6",     APOLLO_AGG_EP_INPUT,    0, 1, MICROPHONE,     0, 1, APOLLO_INFO_MODEL_X8P,   0x0306, 0x0306,  5,  5) \
    X(AggX8pIn7,    "Ap2CX8pIn7",     "x8p MIC/LINE 7",     APOLLO_AGG_EP_INPUT,    0, 1, MICROPHONE,     0, 1, APOLLO_INFO_MODEL_X8P,   0x0307, 0x0307,  6,  6) \
    X(AggX8pIn8,    "Ap2CX8pIn8",     "x8p MIC/LINE 8",     APOLLO_AGG_EP_INPUT,    0, 1, MICROPHONE,     0, 1, APOLLO_INFO_MODEL_X8P,   0x0308, 0x0308,  7,  7) \
    X(AggTwxIn1,    "Ap2CTwxIn1",     "Twin X MIC/LINE 1",  APOLLO_AGG_EP_INPUT,    0, 1, MICROPHONE,     0, 1, APOLLO_INFO_MODEL_TWINX, 0x0001, 0x0001,  0,  0) \
    X(AggTwxIn2,    "Ap2CTwxIn2",     "Twin X MIC/LINE 2",  APOLLO_AGG_EP_INPUT,    0, 1, MICROPHONE,     0, 1, APOLLO_INFO_MODEL_TWINX, 0x0002, 0x0002,  1,  1) \
    X(AggX8pVirt12, "Ap2CX8pVirt12",  "x8p VIRTUAL 1-2",    APOLLO_AGG_EP_INPUT,    0, 2, LINE_CONNECTOR, 0, 0, APOLLO_INFO_MODEL_X8P,   0x0701, 0x0702, 18, 19) \
    X(AggX8pVirt34, "Ap2CX8pVirt34",  "x8p VIRTUAL 3-4",    APOLLO_AGG_EP_INPUT,    0, 2, LINE_CONNECTOR, 0, 0, APOLLO_INFO_MODEL_X8P,   0x0703, 0x0704, 20, 21) \
    X(AggX8pVirt56, "Ap2CX8pVirt56",  "x8p VIRTUAL 5-6",    APOLLO_AGG_EP_INPUT,    0, 2, LINE_CONNECTOR, 0, 0, APOLLO_INFO_MODEL_X8P,   0x0705, 0x0706, 22, 23) \
    X(AggX8pVirt78, "Ap2CX8pVirt78",  "x8p VIRTUAL 7-8",    APOLLO_AGG_EP_INPUT,    0, 2, LINE_CONNECTOR, 0, 0, APOLLO_INFO_MODEL_X8P,   0x0707, 0x0708, 24, 25) \
    X(AggX8pSpdifIn, "Ap2CX8pSpdifIn", "x8p S/PDIF",         APOLLO_AGG_EP_INPUT,    0, 2, LINE_CONNECTOR, 0, 0, APOLLO_INFO_MODEL_X8P,   0x0501, 0x0502, 16, 17) \
    X(AggTwxVirt12, "Ap2CTwxVirt12",  "Twin X VIRTUAL 1-2", APOLLO_AGG_EP_INPUT,    0, 2, LINE_CONNECTOR, 0, 0, APOLLO_INFO_MODEL_TWINX, 0x0701, 0x0702, 18, 19) \
    X(AggTwxVirt34, "Ap2CTwxVirt34",  "Twin X VIRTUAL 3-4", APOLLO_AGG_EP_INPUT,    0, 2, LINE_CONNECTOR, 0, 0, APOLLO_INFO_MODEL_TWINX, 0x0703, 0x0704, 20, 21) \
    X(AggTwxSpdifIn, "Ap2CTwxSpdifIn", "Twin X S/PDIF",      APOLLO_AGG_EP_INPUT,    0, 2, LINE_CONNECTOR, 0, 0, APOLLO_INFO_MODEL_TWINX, 0x0501, 0x0502, 16, 17)

#define APOLLO_AGG_BRIDGE_LINE_CONNECTOR    0
#define APOLLO_AGG_BRIDGE_MICROPHONE        1
#define APOLLO_AGG_BRIDGE_HEADPHONES        2   /* [Endpoints] */

typedef struct _APOLLO_AGG_EP_ROW {
    const char     *Ref;            /* circuit name = INF reference string */
    const char     *Pin;            /* pin name: Windows shows "<Pin> (<device>)" */
    const wchar_t  *RefW;
    const wchar_t  *PinW;
    unsigned char   Kind;           /* APOLLO_AGG_EP_* */
    unsigned char   Render;
    unsigned char   Channels;
    unsigned char   Bridge;         /* [Endpoints] APOLLO_AGG_BRIDGE_* (was Mic: 1 = MICROPHONE) */
    unsigned char   NeverDefault;   /* INF Apollo_NeverDefault_AddReg */
    unsigned char   Default;        /* [Endpoints] in the default set (APOLLO_AGG_EP_DEFAULT) */
    unsigned long   Model;          /* INPUT / OUTPUT rows: APOLLO_INFO_MODEL_* */
    unsigned short  Id[2];          /* descriptor channel IDs */
    unsigned char   Fallback[2];    /* slots while the unit's descriptor was never seen */
} APOLLO_AGG_EP_ROW;

#define APOLLO_AGG_EP_ROW_INIT(sym, ref, pin, kind, render, ch, bridge, never, def, model, i0, i1, f0, f1) \
    { ref, pin, L##ref, L##pin, kind, render, ch, APOLLO_AGG_BRIDGE_##bridge, never, def, model, { i0, i1 }, { f0, f1 } },

/* Each row's index as a constant: ApolloAggEpIdx_AggMon = 0, ... (endpoints.c AggRow1). */
#define APOLLO_AGG_EP_ENUM(sym, ref, pin, kind, render, ch, bridge, never, def, model, i0, i1, f0, f1) ApolloAggEpIdx_##sym,
enum { APOLLO_AGG_EP_LIST(APOLLO_AGG_EP_ENUM) ApolloAggEpIdxEnd };

/* Rows in the list, as a constant (endpoints.c and the test compare it with APOLLO_AGG_EP_ROWS). */
#define APOLLO_AGG_EP_COUNT_ONE(sym, ref, pin, kind, render, ch, bridge, never, def, model, i0, i1, f0, f1) + 1
#define APOLLO_AGG_EP_LIST_COUNT    (0 APOLLO_AGG_EP_LIST(APOLLO_AGG_EP_COUNT_ONE))
/* [Endpoints] the default set computed from the list (endpoints.c asserts it is APOLLO_AGG_EP_DEFAULT) */
#define APOLLO_AGG_EP_DEFBIT(sym, ref, pin, kind, render, ch, bridge, never, def, model, i0, i1, f0, f1) \
    | ((def) ? (1ul << ApolloAggEpIdx_##sym) : 0ul)
#define APOLLO_AGG_EP_LIST_DEFAULT  (0ul APOLLO_AGG_EP_LIST(APOLLO_AGG_EP_DEFBIT))

static __inline const APOLLO_AGG_EP_ROW *ApolloAggEpRow(unsigned long I)
{
    static const APOLLO_AGG_EP_ROW t[] = { APOLLO_AGG_EP_LIST(APOLLO_AGG_EP_ROW_INIT) };

    return (I < sizeof(t) / sizeof(t[0])) ? &t[I] : 0;
}

/* Device add: which table a unit gets. AddModel: apollo_unit.h ApolloAddTimeModel (APOLLO_MODEL_*). */
#define APOLLO_AGG_EPTABLE_NONE         0   /* no circuits at all (the other unit hosts the combined set) */
#define APOLLO_AGG_EPTABLE_STANDALONE   1   /* its own model table, exactly as before G7 */
#define APOLLO_AGG_EPTABLE_COMBINED     2   /* the combined table above */

static __inline int ApolloAggEpTableFor(unsigned long EndpointSet, int AddModel)
{
    if (EndpointSet != APOLLO_AGG_EPSET_COMBINED) {
        return APOLLO_AGG_EPTABLE_STANDALONE;       /* 0, absent, or a value this build does not know */
    }
    return (AddModel == APOLLO_MODEL_X8P) ? APOLLO_AGG_EPTABLE_COMBINED : APOLLO_AGG_EPTABLE_NONE;
}

/*
 * Where a combined stream's channels come from (capture) or go to (render): per channel up to
 * APOLLO_AGG_MAX_DESTS (unit index into the config, ring slot) pairs. Capture uses only [c][0].
 */
typedef struct _APOLLO_AGG_SREF {
    unsigned char   Count[2];
    unsigned char   Unit[2][APOLLO_AGG_MAX_DESTS];
    unsigned short  Slot[2][APOLLO_AGG_MAX_DESTS];
} APOLLO_AGG_SREF;

/* The record slot carrying Id on unit U (from its cached descriptor), or -1. */
static __inline long ApolloAggInSlot(const APOLLO_AGG_CONFIG *C, unsigned long U, unsigned short Id)
{
    unsigned long i;

    for (i = 0; i < C->Unit[U].InCount && i < APOLLO_AGG_UNIT_SLOTS; i++) {
        if (C->Unit[U].InIds[i] == Id) {
            return (long)i;
        }
    }
    return -1;
}

static __inline void ApolloAggSrefAdd(APOLLO_AGG_SREF *O, unsigned long Ch, unsigned long U, long Slot)
{
    unsigned long d;

    if (Slot < 0 || Slot >= APOLLO_AGG_UNIT_SLOTS || Ch > 1 || O->Count[Ch] >= APOLLO_AGG_MAX_DESTS) {
        return;
    }
    for (d = 0; d < O->Count[Ch]; d++) {
        if (O->Unit[Ch][d] == U && O->Slot[Ch][d] == (unsigned short)Slot) {
            return;                 /* named twice: once is enough (ADDing twice would double it) */
        }
    }
    O->Unit[Ch][O->Count[Ch]] = (unsigned char)U;
    O->Slot[Ch][O->Count[Ch]] = (unsigned short)Slot;
    O->Count[Ch]++;
}

/*
 * Slot of Id on unit U, Record or playback; a unit whose descriptor was never seen (count 0) takes
 * the row's fallback when the row names that very ID (the MON pairs and inputs are where the G3/G4
 * descriptors put them on both models); anything else that is not in the descriptor: -1.
 */
static __inline long ApolloAggEpSlot(const APOLLO_AGG_CONFIG *C, unsigned long U, int Record, unsigned short Id,
                                     const APOLLO_AGG_EP_ROW *R, unsigned long Ch)
{
    unsigned long cached = Record ? C->Unit[U].InCount : C->Unit[U].OutCount;
    long s = Record ? ApolloAggInSlot(C, U, Id) : ApolloAggOutSlot(C, U, Id);

    if (s < 0 && cached == 0 && Id == R->Id[Ch]) {
        s = R->Fallback[Ch];
    }
    return s;
}

/* The unit the Monitor endpoints use: the first "windows" route's destination, else the master. */
static __inline unsigned long ApolloAggWindowsUnit(const APOLLO_AGG_CONFIG *C)
{
    unsigned long k;

    for (k = 0; k < C->RouteCount && k < APOLLO_AGG_MAX_ROUTES; k++) {
        if (C->Route[k].SrcKind == APOLLO_AGG_SRC_WINDOWS && C->Route[k].DstUnit < C->UnitCount) {
            return C->Route[k].DstUnit;
        }
    }
    return C->Master;
}

static __inline void ApolloAggEpResolve(const APOLLO_AGG_CONFIG *C, unsigned long Row, APOLLO_AGG_SREF *O)
{
    const APOLLO_AGG_EP_ROW *r = ApolloAggEpRow(Row);
    unsigned long i, c, k, u;

    for (i = 0; i < sizeof(*O); i++) {
        ((unsigned char *)O)[i] = 0;
    }
    if (r == 0 || C->UnitCount == 0 || C->UnitCount > APOLLO_AGG_MAX_UNITS || C->Master >= C->UnitCount) {
        return;
    }
    switch (r->Kind) {
    case APOLLO_AGG_EP_MONITOR: {
        int any = 0;

        for (k = 0; k < C->RouteCount && k < APOLLO_AGG_MAX_ROUTES; k++) {
            const APOLLO_AGG_ROUTE_CFG *rt = &C->Route[k];

            if (rt->SrcKind != APOLLO_AGG_SRC_WINDOWS || rt->DstUnit >= C->UnitCount) {
                continue;
            }
            any = 1;
            for (c = 0; c < 2 && c < r->Channels; c++) {
                ApolloAggSrefAdd(O, c, rt->DstUnit, ApolloAggEpSlot(C, rt->DstUnit, 0, rt->DstId[c], r, c));
            }
        }
        if (!any) {
            for (c = 0; c < 2 && c < r->Channels; c++) {
                ApolloAggSrefAdd(O, c, C->Master, ApolloAggEpSlot(C, C->Master, 0, r->Id[c], r, c));
            }
        }
        break;
    }
    case APOLLO_AGG_EP_LOOPBACK:
        u = ApolloAggWindowsUnit(C);
        for (c = 0; c < 2 && c < r->Channels; c++) {
            ApolloAggSrefAdd(O, c, u, ApolloAggEpSlot(C, u, 1, r->Id[c], r, c));
        }
        break;
    case APOLLO_AGG_EP_INPUT:
    case APOLLO_AGG_EP_OUTPUT:      /* [Endpoints] the same, on the unit's playback descriptor */
        for (u = 0; u < C->UnitCount; u++) {
            if (C->Unit[u].Model == r->Model) {
                break;
            }
        }
        if (u == C->UnitCount) {
            break;                  /* no unit of that model configured: silence */
        }
        for (c = 0; c < 2 && c < r->Channels; c++) {
            ApolloAggSrefAdd(O, c, u, ApolloAggEpSlot(C, u, r->Kind == APOLLO_AGG_EP_INPUT, r->Id[c], r, c));
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* [G7] The tick's windows and the timeline takeover (agg.c)           */
/* ------------------------------------------------------------------ */

/*
 * Staging is zeroed from the last head to To before any writer (the ASIO client, the combined
 * render streams) ADDs; nothing staged yet, a head from another run or more than half the ring
 * behind: the last half ring.
 */
static __inline unsigned long long ApolloAggStageZeroFrom(unsigned long long RenHead, unsigned long long To,
                                                          unsigned long long StageFrames)
{
    if (RenHead == 0 || RenHead > To || To - RenHead > StageFrames / 2) {
        return (To > StageFrames / 2) ? To - StageFrames / 2 : 0;
    }
    return RenHead;
}

/*
 * Where the combined render streams start writing this tick (timeline frames): the last head; on the
 * first tick of a run nothing (To: they start at the lead, as the unit engine does); after a late tick
 * not before what the hardware has not fetched yet (Pm + FRAME_CFG; the frames between are lost).
 */
static __inline unsigned long long ApolloAggStreamFrom(unsigned long long RenHead, unsigned long long Pm,
                                                       unsigned long FrameCfg, unsigned long long To)
{
    if (RenHead == 0 || RenHead > To) {
        return To;                  /* [G7 review] a head beyond To is another run's: start over at the lead */
    }
    if (RenHead < Pm + FrameCfg) {
        return Pm + FrameCfg;
    }
    return RenHead;
}

/*
 * A member's tick while another unit holds the timeline (spec 6.3.4): 1 = the timeline stopped, the
 * caller becomes acting master; 2 = [G7 review] the caller is the configured master and has run for
 * ReclaimTicks of its own ticks (an engine recovered, a transport started by hand): it takes its
 * timeline back; 0 = nothing. *Reclaim counts the master's ticks and restarts at 0 whenever the
 * caller is not the master or does not run, and after a takeover.
 */
/*
 * [G7 review] The timeline unit started a new run under the aggregate without a takeover (its own
 * transport restarted, e.g. ApolloEngineRecover's transport-restart branch, while no other unit ran):
 * its 64-bit position went back (a transport start seeds it from the 13-bit counter), or its engine
 * was started again (EngStarts moved). Every frame number the clients hold is then another run's,
 * so the aggregate starts a new epoch (as on a takeover). Same: Unit is the timeline unit the last
 * record was taken from; a record of another unit only records (the takeover bumped the epoch).
 */
static __inline int ApolloAggTimelineRestarted(int Valid, unsigned long LastUnit, unsigned long Unit,
                                               unsigned long long LastPm, unsigned long long Pm,
                                               unsigned long LastStarts, unsigned long Starts)
{
    if (!Valid || LastUnit != Unit) {
        return 0;
    }
    return Pm < LastPm || Starts != LastStarts;
}

static __inline unsigned long ApolloAggTakeOverWhy(int TimelineRuns, int CallerIsMaster, int CallerRuns,
                                                   unsigned long ReclaimTicks, unsigned long *Reclaim)
{
    if (TimelineRuns) {
        if (!CallerIsMaster || !CallerRuns) {
            *Reclaim = 0;
            return 0;
        }
        if (++*Reclaim < ReclaimTicks) {
            return 0;
        }
        *Reclaim = 0;
        return 2;
    }
    *Reclaim = 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* [G7] Persisted start ratios (spec 6.3.2 "Persisted")                */
/* ------------------------------------------------------------------ */

/*
 * Parameters\Aggregate\Ratios (REG_BINARY, up to 4 entries): the last tracked ratio per (follower,
 * timeline) pair, the next formation's start value. Kept when the follower has been locked for 30 s
 * and its ratio moved 20 ppb or more from the kept one (temperature moves it a few ppm at most, so a
 * write is rare); a full table drops its oldest entry. The reverse pair, inverted, is the fallback.
 */
#define APOLLO_AGG_RATIOS           4
#define APOLLO_AGG_RATIO_SAVE_S     30.0
#define APOLLO_AGG_RATIO_SAVE_PPB   20L

typedef struct _APOLLO_AGG_RATIO {
    unsigned long long  Follower;   /* HW_UID; 0 = empty */
    unsigned long long  Timeline;
    long                Ppb;        /* (r - 1) * 1e9 */
    unsigned long       Seq;        /* when kept (bigger = newer) */
} APOLLO_AGG_RATIO;

static __inline long ApolloAggPpbOf(double R)
{
    double p = (R - 1.0) * 1e9, lim = APOLLO_AGG_RATIO_LIMIT * 1e9;

    if (p > lim) {
        p = lim;
    }
    if (p < -lim) {
        p = -lim;
    }
    return (long)(p >= 0 ? p + 0.5 : p - 0.5);
}

static __inline int ApolloAggRatioUsable(const APOLLO_AGG_RATIO *E)
{
    return E->Follower != 0 && E->Timeline != 0 && E->Follower != E->Timeline &&
           E->Ppb <= (long)(APOLLO_AGG_RATIO_LIMIT * 1e9) && E->Ppb >= -(long)(APOLLO_AGG_RATIO_LIMIT * 1e9);
}

/* The start ratio for Follower against Timeline: 1 = the pair, 2 = the reverse pair inverted, 0 = none (R untouched). */
static __inline int ApolloAggRatioFind(const APOLLO_AGG_RATIO *T, unsigned long N, unsigned long long Follower,
                                       unsigned long long Timeline, double *R)
{
    unsigned long i;

    for (i = 0; i < N && i < APOLLO_AGG_RATIOS; i++) {
        if (ApolloAggRatioUsable(&T[i]) && T[i].Follower == Follower && T[i].Timeline == Timeline) {
            *R = 1.0 + (double)T[i].Ppb * 1e-9;
            return 1;
        }
    }
    for (i = 0; i < N && i < APOLLO_AGG_RATIOS; i++) {
        if (ApolloAggRatioUsable(&T[i]) && T[i].Follower == Timeline && T[i].Timeline == Follower) {
            *R = 1.0 / (1.0 + (double)T[i].Ppb * 1e-9);
            return 2;
        }
    }
    return 0;
}

/* Keep R for the pair. Returns 1 when the table changed (worth writing), 0 when within 20 ppb of the kept value. */
static __inline int ApolloAggRatioPut(APOLLO_AGG_RATIO *T, unsigned long N, unsigned long long Follower,
                                      unsigned long long Timeline, double R, unsigned long Seq)
{
    long ppb = ApolloAggPpbOf(R);
    unsigned long i, pick = N;

    if (Follower == 0 || Timeline == 0 || Follower == Timeline || N == 0 || N > APOLLO_AGG_RATIOS) {
        return 0;
    }
    for (i = 0; i < N; i++) {
        if (T[i].Follower == Follower && T[i].Timeline == Timeline) {
            long d = T[i].Ppb - ppb;

            if (ApolloAggRatioUsable(&T[i]) && d < APOLLO_AGG_RATIO_SAVE_PPB && d > -APOLLO_AGG_RATIO_SAVE_PPB) {
                return 0;
            }
            pick = i;
            break;
        }
    }
    if (pick == N) {
        for (i = 0; i < N; i++) {
            if (!ApolloAggRatioUsable(&T[i])) {
                pick = i;
                break;
            }
        }
    }
    if (pick == N) {
        pick = 0;
        for (i = 1; i < N; i++) {
            if (T[i].Seq < T[pick].Seq) {
                pick = i;           /* full: the oldest goes */
            }
        }
    }
    /* the reverse pair is older news now: the direct one is what Find takes first anyway */
    T[pick].Follower = Follower;
    T[pick].Timeline = Timeline;
    T[pick].Ppb = ppb;
    T[pick].Seq = Seq;
    return 1;
}
