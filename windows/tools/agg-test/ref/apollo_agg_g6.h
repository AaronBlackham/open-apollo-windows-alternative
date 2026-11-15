/*
 * apollo_agg_g6.h - FROZEN copy of driver/apollo_agg.h as gate G6 left it (builds/base-pre-g7, Nov 15 2026),
 * only the include paths changed. tools/agg-test/ref-g6.c compiles it on its own so aggep-test can prove the G7
 * header computes the same layout, pad and capture offsets when ALIGN is set and every trim is 0.
 * Never edit: it is the reference.
 */
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

#include "..\..\..\include\apollo_agg.h"
#include "..\..\..\driver\bringup_plan.h"       /* ApolloChanName, ApolloIoDescCount/Id, ADC and FRAME_CFG values */

/* ------------------------------------------------------------------ */
/* Numbers                                                             */
/* ------------------------------------------------------------------ */

#define APOLLO_AGG_PI           3.14159265358979323846
#define APOLLO_AGG_RATE         48000.0
#define APOLLO_AGG_MARGIN       16          /* M: jitter margin, frames (spec 6.3.3) */

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
}

/*
 * One measurement: Pm = timeline position, PfAtTm = follower position at the same instant.
 * Returns APOLLO_AGG_EV_*. The caller mutes the follower unless State is LOCKED.
 */
static __inline int ApolloAggClockUpdate(APOLLO_AGG_CLOCK *C, long long Pm, double PfAtTm)
{
    double phi, e, ec, kp, ki, dt, wn;

    if (C->State == APOLLO_AGG_CLOCK_NONE) {
        return APOLLO_AGG_EV_NONE;
    }
    dt = (double)(Pm - C->M0) / APOLLO_AGG_RATE;
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
    if (ApolloAggAbs(e) > APOLLO_AGG_RESYNC_FRAMES) {
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
    if (ec > APOLLO_AGG_E_CLAMP || ec < -APOLLO_AGG_E_CLAMP) {
        C->Outliers++;
        ec = (ec > 0) ? APOLLO_AGG_E_CLAMP : -APOLLO_AGG_E_CLAMP;
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
    ki = wn * wn / APOLLO_AGG_RATE;
    kp = 2.0 * 0.7071 * wn / APOLLO_AGG_RATE;
    C->Ri += ki * C->ELp * dt;
    C->R = C->Ri + kp * C->ELp;
    if (C->R < 1.0 - APOLLO_AGG_RATIO_LIMIT || C->R > 1.0 + APOLLO_AGG_RATIO_LIMIT) {
        C->State = APOLLO_AGG_CLOCK_DEGRADED;
        C->ResyncAt[2] = C->Since;
        C->R = C->Ri = 1.0;
        return APOLLO_AGG_EV_DEGRADED;
    }
    if (ApolloAggAbs(C->EFilt) < APOLLO_AGG_LOCK_FRAMES) {
        C->Good += dt;
    } else {
        C->Good = 0;
    }
    if (C->State == APOLLO_AGG_CLOCK_LOCKED) {
        C->Locked += dt;
        if (ApolloAggAbs(C->EFilt) > 2.0) {
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
} APOLLO_AGG_UNIT_RT;

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
 */
static __inline unsigned long ApolloAggPad(const APOLLO_AGG_CONFIG *C, const APOLLO_AGG_UNIT_RT *Rt,
                                           unsigned long Timeline)
{
    unsigned long u, extra = 0, tl = (Timeline < C->UnitCount) ? Timeline : C->Master;

    if (C->UnitCount < 2) {
        return 0;
    }
    for (u = 0; u < C->UnitCount; u++) {
        if (u != tl && Rt[u].Adc > Rt[tl].Adc && Rt[u].Adc - Rt[tl].Adc > extra) {
            extra = Rt[u].Adc - Rt[tl].Adc;
        }
    }
    return C->AsrcHalf + APOLLO_AGG_MARGIN + extra;
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

                    if (!ApolloAggSlotShown(uc->Model, record, id)) {
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
            int follower = (un != tl);

            L[i].Flags = (unsigned char)((r->Present ? APOLLO_AGG_CH_PRESENT : 0) |
                                         (follower ? APOLLO_AGG_CH_RESAMPLED : APOLLO_AGG_CH_TIMELINE));
            L[i].LatencyFrames = (unsigned short)(record ? r->Adc + r->Guard +
                                                  (follower ? C->AsrcHalf + APOLLO_AGG_MARGIN : 0) :
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
