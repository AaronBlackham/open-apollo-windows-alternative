/*
 * corr_dsp.h - speaker correction's pure logic (include\apollo_corr.h): the biquad engine, the
 * enable / bypass / swap crossfades, the per-tick pump and the driver's own check of a profile.
 * Plain C (also compiles as C++) with no kernel or CRT calls, so the driver (corr.c, the pump at
 * DISPATCH_LEVEL) and the offline tests (tools\corr-test) run exactly the same code. Floating
 * point: SSE2 doubles only, which x64 kernel code may use without saving state; no x87, no AVX.
 *
 * Signal path per channel (Avium Measure docs\correction-profile.md):
 *     out = filters( delay( in * Preamp * Gain ) )
 * in 64-bit floating point, each filter one transposed direct form II section (scipy's sosfilt does
 * the same arithmetic in the same order). The ring holds 32-bit integers (full scale 2^31); the
 * corrected value is rounded and saturated back into the same frame and slot.
 *
 * Voices. A program is one profile ready to run (CORR_PROG: sections, gains, delays, and its own
 * filter states and input history). Up to two run at once: Cur, and during a swap Next, which takes
 * over through a crossfade; a third load while a swap runs waits in Pending. Wet (W) is the
 * dry <-> corrected fade. Both fades are smoothstep curves over APOLLO_CORR_FADE_MS.
 *
 *   enabled, not bypassed   W -> 1: the ring gets the corrected signal
 *   enabled, bypassed (A/B) W -> 0 and the filters keep running ("warm"), so going back is seamless
 *   disabled or cleared     W -> 0, then nothing runs at all and the ring is never written
 *
 * With W at 0 the ring is not written, so the bypass is bit-identical, and with no program or the
 * correction off the pump returns before touching anything (tools\corr-test proves both).
 *
 * In-place processing and the writers. The pump corrects frames [Head, bound) of the unit's ring,
 * where bound is the frame every host writer has reached (the engine's data head; for a follower of
 * the combined device also the head the aggregate wrote it to). Each writer only ever writes at or
 * beyond its own head, and a writer that (re)starts takes CorrWriteFloor so it never adds into a
 * frame already corrected. Frames below SAMPLE_POS + CORR_SAFETY are being fetched: the pump skips
 * past them (LateFrames).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include "..\include\apollo_corr.h"

#define CORR_HIST           2048u       /* input history per channel, frames (10 ms at 192 kHz fits) */
#define CORR_HIST_MASK      (CORR_HIST - 1u)
#define CORR_BLOCK          128u        /* frames per processing block (one 1 ms tick is 48) */
#define CORR_PROGS          4           /* program slots: Cur, Next, Pending and one being filled */
#define CORR_MAX_RUN        1024u       /* most frames one pump corrects; an older backlog plays uncorrected */
#define CORR_SAFETY         8u          /* the hardware fetches up to SAMPLE_POS + FRAME_CFG (8 at 48 kHz) */

/* [Rate] FRAME_CFG at Rate: 8 frames at 44.1/48 kHz, 16 at 88.2/96, 32 at 176.4/192 (driver\rate_plan.h) */
static __inline unsigned CorrSafety(unsigned long Rate)
{
    return Rate > 100000ul ? 4u * CORR_SAFETY : (Rate > 50000ul ? 2u * CORR_SAFETY : CORR_SAFETY);
}
#define CORR_AGG_STALE      8u          /* a bound the aggregate published this many own ticks ago is ignored */
#define CORR_TINY           1e-12       /* filter states below this are 0 (the sample LSB is 1): no denormals */
#define CORR_RHO            (1.0 - 1e-9)    /* pole radius limit: validate()'s |root| < 1 - 1e-9 */
#define CORR_PI             3.14159265358979323846
#define CORR_GRID_RATIO     1.0036166659754628  /* 2^(1/192): validate()'s grid, 192 points per octave */
#define CORR_GRID_FMIN      5.0
#define CORR_CLIP_LIN2      1.0118124058555886  /* 10^(0.051/10): +0.05 dB (the format) + 0.001 dB of rounding room */
#define CORR_DC_LIN2        4.027170343254591   /* 10^(6.05/10): below 5 Hz and above 0.999 Nyquist, outside validate()'s grid */
#define CORR_POLE_Q_MAX     282.0               /* the format's narrowest resonance: a q 50 peak at +30 dB, 50 * 10^(30/40) = 281.2 */
#define CORR_SUB5_RATIO     1.0145453349375237  /* 2^(1/48): the check's grid below 5 Hz, down to CORR_SUB5_FMIN */
#define CORR_SUB5_FMIN      0.01
#define CORR_REFINE_STEPS   40                  /* golden-section steps around each section's own extrema */
#define CORR_GAIN_MIN       0.000999            /* -60 dB, the format's floor (10^-3, with rounding room) */

typedef struct CORR_SEC {
    double          b0, b1, b2, a1, a2;
} CORR_SEC;

/* One channel of a program. Params are set at PASSIVE (CorrProgramFill); Z and Hist belong to the tick. */
typedef struct CORR_PCH {
    unsigned        Slot;
    unsigned        Delay;              /* frames */
    unsigned        NSec;               /* enabled sections, in the profile's order */
    unsigned        Pad;
    double          Gain;               /* Preamp * Gain */
    CORR_SEC        Sec[APOLLO_CORR_MAX_FILTERS];
    double          Z[APOLLO_CORR_MAX_FILTERS][2];
    long            Hist[CORR_HIST];    /* raw input, indexed by the engine's HistPos */
} CORR_PCH;

typedef struct CORR_PROG {
    unsigned        NCh;
    unsigned        Rate;
    CORR_PCH        Ch[APOLLO_CORR_MAX_CHANNELS];
} CORR_PROG;

typedef struct CORR_ENGINE {
    CORR_PROG       Prog[CORR_PROGS];
    int             Cur, Next, Pending, Reserved;   /* program indices, -1 = none */
    int             CurLive, NextLive;  /* that voice's states and history are running */
    int             Enabled, Bypass, Unload;
    unsigned        FadeLen;            /* frames */
    unsigned        W;                  /* dry 0 .. FadeLen corrected */
    unsigned        X;                  /* swap Cur 0 .. FadeLen Next */
    unsigned        HistPos;
    unsigned        SlotSkipped;        /* a channel's slot is beyond the ring's channels */
    unsigned        RateMismatch;
    /* the pump */
    int             HeadValid;
    unsigned long long Head;
    unsigned long long Frames, Late;
    unsigned long   Runs;
    unsigned long   Starts;             /* the engine's start count the head belongs to */
    int             AggBounded;
    unsigned long long AggBound;        /* the aggregate wrote this follower's ring up to here */
    unsigned long long AggTick;         /* the unit's tick count when it said so */
    /* scratch, one block */
    double          Xc[APOLLO_CORR_MAX_CHANNELS][CORR_BLOCK];
    double          Yc[APOLLO_CORR_MAX_CHANNELS][CORR_BLOCK];
    double          Xn[APOLLO_CORR_MAX_CHANNELS][CORR_BLOCK];
    double          Yn[APOLLO_CORR_MAX_CHANNELS][CORR_BLOCK];
    double          Gw[CORR_BLOCK];
    double          Gx[CORR_BLOCK];
} CORR_ENGINE;

/* ------------------------------------------------------------------ */
/* Numbers                                                             */
/* ------------------------------------------------------------------ */

static __inline int CorrFinite(double v)
{
    union { double d; unsigned long long u; } b;

    b.d = v;
    return ((b.u >> 52) & 0x7FFu) != 0x7FFu;
}

/* Round half away from zero and saturate to a 32-bit sample. Not MINLONG (apollo_sat.h). */
static __inline long CorrToS32(double v)
{
    if (!(v == v)) {
        return 0;
    }
    if (v >= 2147483647.0) {
        return 2147483647L;
    }
    if (v <= -2147483648.0) {
        return -2147483647L - 1;
    }
    return (long)(v >= 0.0 ? v + 0.5 : v - 0.5);
}

/* 3t^2 - 2t^3 on [0, 1]: 0 and 1 exactly at the ends, no corner where a fade starts or stops. */
static __inline double CorrSmooth(double t)
{
    return t * t * (3.0 - 2.0 * t);
}

/* sin(x) for 0 <= x <= pi/2: 12 Taylor terms, relative precision kept for small x. */
static __inline double CorrSin(double x)
{
    double s = x, t = x, y2 = x * x;
    int k;

    for (k = 1; k <= 12; k++) {
        t *= -y2 / ((2.0 * k) * (2.0 * k + 1.0));
        s += t;
    }
    return s;
}

/*
 * |c0 + c1 e^-jw + c2 e^-2jw|^2 written around z = 1 (Side 0, S = 1 - cos w = 2 sin^2(w/2)) or
 * around z = -1 (Side 1, S = 1 + cos w = 2 cos^2(w/2)):
 *     P^2 - 2 S P (c0 + c2) + 2 S (c0 - c2)^2 + 4 c0 c2 S^2,   P = c0 + c1 + c2 (Side 1: c0 - c1 + c2)
 * The textbook expansion in cos w and cos 2w cancels to nothing for poles or zeros near DC (a 20 Hz
 * room-mode cut at 192 kHz: |A|^2 ~ 1e-13 from terms ~4); every term here keeps its precision.
 */
static __inline double CorrPoly2(double C0, double C1, double C2, double S, int Side)
{
    double p = Side ? (C0 - C1 + C2) : (C0 + C1 + C2), q = C0 - C2;

    return p * p - 2.0 * S * p * (C0 + C2) + 2.0 * S * q * q + 4.0 * C0 * C2 * S * S;
}

/* |H|^2 of one section (b0 b1 b2 a1 a2) at the point S / Side above. */
static __inline double CorrMag2(const double *C, double S, int Side)
{
    double nb = CorrPoly2(C[0], C[1], C[2], S, Side), da = CorrPoly2(1.0, C[3], C[4], S, Side);

    return da > 0.0 ? nb / da : 1e300;
}

/* The point for frequency F at Rate (0 <= F <= Rate / 2): the side nearer to it, and S. */
static __inline void CorrPoint(double F, double Rate, double *S, int *Side)
{
    double h = CORR_PI * F / Rate, sh, ch;

    if (h <= CORR_PI / 4) {
        sh = CorrSin(h);
        *S = 2.0 * sh * sh;
        *Side = 0;
    } else {
        ch = CorrSin(CORR_PI / 2 - h);
        *S = 2.0 * ch * ch;
        *Side = 1;
    }
}

/* |H|^2 of one section at frequency F. */
static __inline double CorrMag2At(const double *C, double F, double Rate)
{
    double s;
    int side;

    CorrPoint(F, Rate, &s, &side);
    return CorrMag2(C, s, side);
}

/* Both poles of z^2 + a1 z + a2 strictly inside radius CORR_RHO (Jury's test, scaled to that radius). */
static __inline int CorrStable(double A1, double A2)
{
    const double r = CORR_RHO;

    if (!CorrFinite(A1) || !CorrFinite(A2)) {
        return 0;
    }
    if (!(A2 < r * r && A2 > -r * r)) {
        return 0;
    }
    return (A1 < r + A2 / r && -A1 < r + A2 / r) ? 1 : 0;
}

/*
 * No resonance narrower than the format can describe. A section's poles are the bilinear transform of
 * an analog pair whose Q is sqrt((1 + a1 + a2)(1 - a1 + a2)) / (2 (1 - a2)), whatever the prewarping:
 * for the format's designs that is q (shelves, pass, notch, all-pass, band-pass) or q * 10^(gain/40)
 * (peaking), at most 50 * 10^(30/40) = 281.2. Real poles give at most 0.5 and always pass. Compared
 * squared, so no square root. Call after CorrStable (1 - a2 > 0).
 */
static __inline int CorrPoleQOk(double A1, double A2)
{
    double w = (1.0 + A1 + A2) * (1.0 - A1 + A2), d = 1.0 - A2;

    return w <= 4.0 * CORR_POLE_Q_MAX * CORR_POLE_Q_MAX * d * d ? 1 : 0;
}

static __inline int CorrRateKnown(unsigned long Rate)
{
    return Rate == 44100 || Rate == 48000 || Rate == 88200 || Rate == 96000 || Rate == 176400 || Rate == 192000;
}

static __inline int CorrTextOk(const char *S, unsigned Cap)
{
    unsigned i;

    for (i = 0; i < Cap; i++) {
        if (S[i] == 0) {
            return 1;
        }
    }
    return 0;
}

/* Delay in frames at Rate, rounded to the nearest frame. */
static __inline unsigned CorrDelayFrames(unsigned long DelayUs, unsigned long Rate)
{
    return (unsigned)(((unsigned long long)DelayUs * Rate + 500000ull) / 1000000ull);
}

/* ------------------------------------------------------------------ */
/* The driver's check of a profile (never trusts the host)            */
/* ------------------------------------------------------------------ */

/* sqrt for x >= 0: the exponent halved, then Newton (no CRT in the kernel). Below 1e-300: 0. */
static __inline double CorrSqrt(double X)
{
    union { double d; unsigned long long u; } b;
    double y;
    int i;

    if (!(X > 1e-300) || !CorrFinite(X)) {
        return (X > 1e-300) ? X : 0.0;
    }
    b.d = X;
    b.u = (b.u >> 1) + 0x1FF8000000000000ull;   /* within 6 % of the root */
    y = b.d;
    for (i = 0; i < 6; i++) {
        y = 0.5 * (y + X / y);
    }
    return y;
}

/* CorrPoly2 as a polynomial in S: N[0] + N[1] S + N[2] S^2 */
static __inline void CorrPolyS(double C0, double C1, double C2, int Side, double *N)
{
    double p = Side ? (C0 - C1 + C2) : (C0 + C1 + C2), q = C0 - C2;

    N[0] = p * p;
    N[1] = 2.0 * q * q - 2.0 * p * (C0 + C2);
    N[2] = 4.0 * C0 * C2;
}

/*
 * The S (on Side, 0 <= S <= 1) where one section's |H|^2 has a maximum or minimum: |H|^2 = N(S) / D(S)
 * with N and D quadratics, so N'D - ND' = 0 is a quadratic too (its cubic terms cancel). Up to 2 roots.
 */
static __inline unsigned CorrSectionExtrema(const double *C, int Side, double *Out)
{
    double n[3], d[3], a2, a1, a0, disc, sq, qq, r[2];
    unsigned k, cnt = 0, nr = 0;

    CorrPolyS(C[0], C[1], C[2], Side, n);
    CorrPolyS(1.0, C[3], C[4], Side, d);
    a2 = n[2] * d[1] - n[1] * d[2];
    a1 = 2.0 * (n[2] * d[0] - n[0] * d[2]);
    a0 = n[1] * d[0] - n[0] * d[1];
    if (a2 == 0.0) {
        if (a1 != 0.0) {
            r[nr++] = -a0 / a1;
        }
    } else {
        disc = a1 * a1 - 4.0 * a2 * a0;
        if (disc >= 0.0) {
            sq = CorrSqrt(disc);
            qq = -0.5 * (a1 + (a1 >= 0.0 ? sq : -sq));
            r[nr++] = qq / a2;
            if (qq != 0.0) {
                r[nr++] = a0 / qq;
            }
        }
    }
    for (k = 0; k < nr; k++) {
        if (CorrFinite(r[k]) && r[k] >= 0.0 && r[k] <= 1.0) {
            Out[cnt++] = r[k];
        }
    }
    return cnt;
}

/* Where the format's headroom rule applies (in band) and where only the +6 dB limit does. */
typedef struct CORR_PEAK {
    double          In;             /* largest |H|^2, 5 Hz .. 0.999 Nyquist */
    double          Out;            /* largest |H|^2, below 5 Hz and above 0.999 Nyquist (DC, Nyquist included) */
    double          S5;             /* Side 0: S of 5 Hz */
    double          Sny;            /* Side 1: S of 0.999 Nyquist */
} CORR_PEAK;

static __inline double CorrCascade2(const APOLLO_CORR_CHANNEL *Ch, double S, int Side)
{
    double m = 1.0;
    unsigned k;

    for (k = 0; k < Ch->FilterCount && k < APOLLO_CORR_MAX_FILTERS; k++) {
        if (Ch->Filter[k].Enabled) {
            m *= CorrMag2(Ch->Filter[k].Coef, S, Side);
        }
    }
    return m;
}

static __inline double CorrPeakAt(const APOLLO_CORR_CHANNEL *Ch, double S, int Side, CORR_PEAK *Pk)
{
    double m = CorrCascade2(Ch, S, Side);
    int in = Side ? (S >= Pk->Sny) : (S >= Pk->S5);

    if (in) {
        if (m > Pk->In) {
            Pk->In = m;
        }
    } else if (m > Pk->Out) {
        Pk->Out = m;
    }
    return m;
}

/*
 * The cascade's maximum near S (+-2 % in S, about +-1 % in frequency): golden-section search, every
 * point it tries counted. Catches a peak the cascade makes between two sections' own peaks.
 */
static __inline void CorrRefine(const APOLLO_CORR_CHANNEL *Ch, double S, int Side, CORR_PEAK *Pk)
{
    const double g = 0.6180339887498949;
    double lo = S * 0.98, hi = S * 1.02, x1, x2, f1, f2;
    int i;

    if (hi > 1.0) {
        hi = 1.0;
    }
    if (!(hi > lo)) {
        return;
    }
    x1 = hi - g * (hi - lo);
    x2 = lo + g * (hi - lo);
    f1 = CorrPeakAt(Ch, x1, Side, Pk);
    f2 = CorrPeakAt(Ch, x2, Side, Pk);
    for (i = 0; i < CORR_REFINE_STEPS; i++) {
        if (f1 < f2) {
            lo = x1;
            x1 = x2;
            f1 = f2;
            x2 = lo + g * (hi - lo);
            f2 = CorrPeakAt(Ch, x2, Side, Pk);
        } else {
            hi = x2;
            x2 = x1;
            f2 = f1;
            x1 = hi - g * (hi - lo);
            f1 = CorrPeakAt(Ch, x1, Side, Pk);
        }
    }
}

/*
 * A channel's largest |H|^2 (its enabled sections) at Rate, in band and out of band (CORR_PEAK):
 *   validate()'s grid, 5 Hz to 0.999 Nyquist at 192 points per octave (the same points it measures);
 *   below 5 Hz a 48-per-octave grid down to 0.01 Hz, DC, and 32 points from 0.999 Nyquist to Nyquist;
 *   every section's own maxima and minima (CorrSectionExtrema, on both sides of the circle), each
 *   refined into the cascade's maximum around it (CorrRefine).
 * A peak narrower than the grid spacing is a section's resonance, so it is measured where it is.
 */
static __inline void CorrChannelPeaks(const APOLLO_CORR_CHANNEL *Ch, unsigned long Rate, CORR_PEAK *Pk)
{
    double fmax = (double)Rate / 2.0 * 0.999, f, s, e[2], h;
    unsigned k, n, i;
    int side;

    Pk->In = 0.0;
    Pk->Out = 0.0;
    h = CorrSin(CORR_PI * CORR_GRID_FMIN / (double)Rate);
    Pk->S5 = 2.0 * h * h;
    h = CorrSin(CORR_PI / 2.0 * 0.001);
    Pk->Sny = 2.0 * h * h;
    /* DC and Nyquist */
    (void)CorrPeakAt(Ch, 0.0, 0, Pk);
    (void)CorrPeakAt(Ch, 0.0, 1, Pk);
    /* validate()'s grid */
    for (f = CORR_GRID_FMIN; f <= fmax; f *= CORR_GRID_RATIO) {
        CorrPoint(f, (double)Rate, &s, &side);
        (void)CorrPeakAt(Ch, s, side, Pk);
    }
    /* below 5 Hz and above 0.999 Nyquist */
    for (f = CORR_GRID_FMIN / CORR_SUB5_RATIO; f >= CORR_SUB5_FMIN; f /= CORR_SUB5_RATIO) {
        CorrPoint(f, (double)Rate, &s, &side);
        (void)CorrPeakAt(Ch, s, side, Pk);
    }
    for (i = 0; i < 32; i++) {
        (void)CorrPeakAt(Ch, Pk->Sny * (double)i / 32.0, 1, Pk);
    }
    /* each section's own extrema, and the cascade's maximum around each */
    for (k = 0; k < Ch->FilterCount && k < APOLLO_CORR_MAX_FILTERS; k++) {
        if (!Ch->Filter[k].Enabled) {
            continue;
        }
        for (side = 0; side < 2; side++) {
            n = CorrSectionExtrema(Ch->Filter[k].Coef, side, e);
            for (i = 0; i < n; i++) {
                (void)CorrPeakAt(Ch, e[i], side, Pk);
                CorrRefine(Ch, e[i], side, Pk);
            }
        }
    }
}

/* The in-band peak (as validate() defines the band); *Out the out-of-band one. */
static __inline double CorrChannelPeak2(const APOLLO_CORR_CHANNEL *Ch, unsigned long Rate, double *Out)
{
    CORR_PEAK pk;

    CorrChannelPeaks(Ch, Rate, &pk);
    *Out = pk.Out;
    return pk.In;
}

/*
 * APOLLO_CORR_ERR_NONE, or why the profile must not run; *Ch / *Flt say where. PlayChannels 0 = the
 * unit's slot count is not known yet (only the format's limit applies; the tick skips a slot
 * beyond the ring).
 */
static __inline unsigned long CorrValidate(const APOLLO_CORR_PROFILE *P, unsigned long PlayChannels, unsigned long *Ch,
                                           unsigned long *Flt)
{
    unsigned long c, k, j;

    *Ch = 0;
    *Flt = 0;
    if (P->Size != sizeof(APOLLO_CORR_PROFILE) || P->Version != APOLLO_CORR_VERSION) {
        return APOLLO_CORR_ERR_SIZE;
    }
    if (!CorrTextOk(P->Name, APOLLO_CORR_NAME_CHARS) || !CorrTextOk(P->Id, APOLLO_CORR_ID_CHARS) ||
        !CorrTextOk(P->Created, APOLLO_CORR_CREATED_CHARS) || !CorrTextOk(P->Summary, APOLLO_CORR_SUMMARY_CHARS)) {
        return APOLLO_CORR_ERR_TEXT;
    }
    if (!CorrRateKnown(P->Rate)) {
        return APOLLO_CORR_ERR_RATE;
    }
    if (P->ChannelCount < 1 || P->ChannelCount > APOLLO_CORR_MAX_CHANNELS) {
        return APOLLO_CORR_ERR_CHANNELS;
    }
    if (!CorrFinite(P->Preamp) || !CorrFinite(P->PreampDb)) {
        return APOLLO_CORR_ERR_NOT_FINITE;
    }
    if (!(P->Preamp >= CORR_GAIN_MIN && P->Preamp <= 1.0)) {
        return APOLLO_CORR_ERR_GAIN;
    }
    for (c = 0; c < P->ChannelCount; c++) {
        const APOLLO_CORR_CHANNEL *h = &P->Channel[c];

        *Ch = c;
        if (!CorrTextOk(h->Id, APOLLO_CORR_CH_ID_CHARS)) {
            return APOLLO_CORR_ERR_TEXT;
        }
        if (h->Slot > APOLLO_CORR_MAX_SLOT || (PlayChannels != 0 && h->Slot >= PlayChannels)) {
            return APOLLO_CORR_ERR_SLOT;
        }
        for (j = 0; j < c; j++) {
            if (P->Channel[j].Slot == h->Slot) {
                return APOLLO_CORR_ERR_SLOT;
            }
        }
        if (!CorrFinite(h->Gain) || !CorrFinite(h->GainDb)) {
            return APOLLO_CORR_ERR_NOT_FINITE;
        }
        if (!(h->Gain >= CORR_GAIN_MIN && h->Gain <= 1.0)) {
            return APOLLO_CORR_ERR_GAIN;
        }
        if (h->DelayUs > APOLLO_CORR_MAX_DELAY_US || CorrDelayFrames(h->DelayUs, P->Rate) >= CORR_HIST) {
            return APOLLO_CORR_ERR_DELAY;
        }
        if (h->FilterCount > APOLLO_CORR_MAX_FILTERS) {
            return APOLLO_CORR_ERR_FILTERS;
        }
        for (k = 0; k < h->FilterCount; k++) {
            const APOLLO_CORR_FILTER *f = &h->Filter[k];

            *Flt = k;
            for (j = 0; j < 5; j++) {
                if (!CorrFinite(f->Coef[j])) {
                    return APOLLO_CORR_ERR_NOT_FINITE;
                }
            }
            if (!CorrFinite(f->FreqHz) || !CorrFinite(f->GainDb) || !CorrFinite(f->Q)) {
                return APOLLO_CORR_ERR_NOT_FINITE;
            }
            /* every listed filter, enabled or not, as validate() does */
            if (!CorrStable(f->Coef[3], f->Coef[4])) {
                return APOLLO_CORR_ERR_UNSTABLE;
            }
            if (!CorrPoleQOk(f->Coef[3], f->Coef[4])) {
                return APOLLO_CORR_ERR_NARROW;
            }
        }
        *Flt = 0;
    }
    for (c = 0; c < P->ChannelCount; c++) {
        const APOLLO_CORR_CHANNEL *h = &P->Channel[c];
        double g2 = (P->Preamp * h->Gain) * (P->Preamp * h->Gain), dc = 1.0;
        double peak = CorrChannelPeak2(h, P->Rate, &dc);

        *Ch = c;
        if (peak * g2 > CORR_CLIP_LIN2) {
            return APOLLO_CORR_ERR_CLIP;
        }
        if (dc * g2 > CORR_DC_LIN2) {
            return APOLLO_CORR_ERR_DC;
        }
    }
    *Ch = 0;
    return APOLLO_CORR_ERR_NONE;
}

/* ------------------------------------------------------------------ */
/* Programs (PASSIVE: a reserved slot the tick does not touch)        */
/* ------------------------------------------------------------------ */

/* A validated profile -> program parameters. The voice's states and history are set when it starts. */
static __inline void CorrProgramFill(CORR_PROG *G, const APOLLO_CORR_PROFILE *P)
{
    unsigned c, k;

    G->Rate = (unsigned)P->Rate;
    G->NCh = (unsigned)P->ChannelCount;
    for (c = 0; c < G->NCh && c < APOLLO_CORR_MAX_CHANNELS; c++) {
        const APOLLO_CORR_CHANNEL *h = &P->Channel[c];
        CORR_PCH *q = &G->Ch[c];

        q->Slot = (unsigned)h->Slot;
        q->Delay = CorrDelayFrames(h->DelayUs, P->Rate);
        q->Gain = P->Preamp * h->Gain;
        q->NSec = 0;
        for (k = 0; k < h->FilterCount && k < APOLLO_CORR_MAX_FILTERS; k++) {
            const APOLLO_CORR_FILTER *f = &h->Filter[k];

            if (f->Enabled) {
                CORR_SEC *s = &q->Sec[q->NSec++];

                s->b0 = f->Coef[0];
                s->b1 = f->Coef[1];
                s->b2 = f->Coef[2];
                s->a1 = f->Coef[3];
                s->a2 = f->Coef[4];
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* The state machine (EngineLock in the driver)                        */
/* ------------------------------------------------------------------ */

static __inline void CorrEngineInit(CORR_ENGINE *E)
{
    E->Cur = E->Next = E->Pending = E->Reserved = -1;
    E->CurLive = E->NextLive = 0;
    E->Enabled = E->Bypass = E->Unload = 0;
    E->FadeLen = 960;
    E->W = E->X = 0;
    E->HistPos = 0;
    E->SlotSkipped = E->RateMismatch = 0;
    E->HeadValid = 0;
    E->Head = 0;
    E->Frames = E->Late = 0;
    E->Runs = 0;
    E->Starts = 0;
    E->AggBounded = 0;
    E->AggBound = E->AggTick = 0;
}

static __inline int CorrWantWet(const CORR_ENGINE *E)
{
    return E->Cur >= 0 && E->Enabled && !E->Bypass && !E->Unload;
}

static __inline int CorrWantWarm(const CORR_ENGINE *E)
{
    return E->Cur >= 0 && E->Enabled && E->Bypass && !E->Unload;
}

/* Anything to run at all: with this 0 the pump returns before it touches a frame. */
static __inline int CorrRunning(const CORR_ENGINE *E)
{
    return E->Cur >= 0 && (E->W > 0 || CorrWantWet(E) || CorrWantWarm(E));
}

/* A program slot no voice uses, reserved for the caller to fill (-1: none, cannot happen with 4). */
static __inline int CorrReserve(CORR_ENGINE *E)
{
    int i;

    for (i = 0; i < CORR_PROGS; i++) {
        if (i != E->Cur && i != E->Next && i != E->Pending && i != E->Reserved) {
            E->Reserved = i;
            return i;
        }
    }
    return -1;
}

static __inline void CorrSetFade(CORR_ENGINE *E, unsigned Rate)
{
    unsigned n = Rate / 1000u * APOLLO_CORR_FADE_MS + (Rate % 1000u) * APOLLO_CORR_FADE_MS / 1000u;

    E->FadeLen = n != 0 ? n : 1;
}

/*
 * The reserved program (filled) goes in: the first or a cold one becomes Cur; while voices run it
 * becomes Next (a crossfade), or Pending while a swap is still in progress (a newer load replaces an
 * older pending one). Enable: the load also enables.
 */
static __inline void CorrInstall(CORR_ENGINE *E, int Idx, int Enable)
{
    if (E->Reserved == Idx) {
        E->Reserved = -1;
    }
    if (Enable) {
        E->Enabled = 1;
    }
    E->Unload = 0;
    if (E->Cur < 0 || !CorrRunning(E) || (!E->CurLive && E->W == 0)) {
        /* nothing audible from the old one yet: no crossfade needed */
        E->Cur = Idx;
        E->CurLive = 0;
        E->Next = E->Pending = -1;
        E->NextLive = 0;
        E->X = 0;
        E->W = 0;
        CorrSetFade(E, E->Prog[Idx].Rate);
        return;
    }
    if (E->Next < 0) {
        E->Next = Idx;
        E->NextLive = 0;
        E->X = 0;
        return;
    }
    E->Pending = Idx;
}

/* 0 = done; APOLLO_CORR_ERR_NO_PROFILE to enable with none, or one being cleared (only a load brings one back). */
static __inline unsigned long CorrSetEnabled(CORR_ENGINE *E, int On)
{
    if (On && (E->Cur < 0 || E->Unload)) {
        return APOLLO_CORR_ERR_NO_PROFILE;
    }
    E->Enabled = On ? 1 : 0;
    return APOLLO_CORR_ERR_NONE;
}

static __inline void CorrSetBypass(CORR_ENGINE *E, int On)
{
    E->Bypass = On ? 1 : 0;
}

/*
 * Forget the profile: at once when nothing runs, else after the fade out. Disabled from here: neither
 * an enable nor a load without APOLLO_CORR_LOAD_ENABLE during the fade brings correction back.
 */
static __inline void CorrClear(CORR_ENGINE *E)
{
    E->Enabled = 0;
    if (!CorrRunning(E)) {
        E->Cur = E->Next = E->Pending = -1;
        E->CurLive = E->NextLive = 0;
        E->Unload = 0;
        E->W = E->X = 0;
        return;
    }
    E->Unload = 1;
}

/* ------------------------------------------------------------------ */
/* The tick (DISPATCH_LEVEL, EngineLock)                               */
/* ------------------------------------------------------------------ */

/* Nothing left to run: the voices go cold, a swap in progress resolves to the newest program. */
static __inline void CorrStop(CORR_ENGINE *E)
{
    E->CurLive = E->NextLive = 0;
    if (E->Next >= 0) {
        E->Cur = E->Pending >= 0 ? E->Pending : E->Next;
    }
    E->Next = E->Pending = -1;
    E->X = 0;
    E->W = 0;
    if (E->Unload) {
        E->Cur = -1;
        E->Unload = 0;
        E->Enabled = 0;
    }
    E->HeadValid = 0;
}

/*
 * A voice starts: filter states 0; its input history from the running voice's channel on the same
 * slot (a swap), else from the ring's frames just before F0, which nothing corrected (a cold start,
 * or a slot the running voice does not touch), so the delayed signal starts without a jump.
 */
static __inline void CorrVoiceStart(CORR_ENGINE *E, CORR_PROG *G, const CORR_PROG *From, const long *Ring, unsigned Mask,
                                    unsigned Stride, unsigned long long F0)
{
    unsigned c, k, j;

    for (c = 0; c < G->NCh && c < APOLLO_CORR_MAX_CHANNELS; c++) {
        CORR_PCH *q = &G->Ch[c];
        const CORR_PCH *src = 0;

        for (k = 0; k < APOLLO_CORR_MAX_FILTERS; k++) {
            q->Z[k][0] = q->Z[k][1] = 0.0;
        }
        for (j = 0; From != 0 && j < From->NCh && j < APOLLO_CORR_MAX_CHANNELS; j++) {
            if (From->Ch[j].Slot == q->Slot) {
                src = &From->Ch[j];
            }
        }
        if (src != 0) {
            for (k = 0; k < CORR_HIST; k++) {
                q->Hist[k] = src->Hist[k];
            }
        } else {
            for (k = 0; k < CORR_HIST; k++) {
                q->Hist[k] = 0;
            }
            if (q->Slot < Stride) {
                for (k = 1; k <= q->Delay && k < CORR_HIST; k++) {
                    q->Hist[(E->HistPos - k) & CORR_HIST_MASK] = Ring[(size_t)((F0 - k) & Mask) * Stride + q->Slot];
                }
            }
        }
    }
}

/*
 * One voice channel over N frames from F0: gather the slot into X (as doubles), push the raw
 * samples into the history, then Y = the sections over (delayed input * gain). Exposed for the
 * engine reference test.
 */
static __inline void CorrChannelRun(CORR_PCH *Q, const long *Ring, unsigned Mask, unsigned Stride, unsigned long long F0,
                                    unsigned N, unsigned HistPos, double *X, double *Y)
{
    unsigned i, s;

    for (i = 0; i < N; i++) {
        long v = Ring[(size_t)((F0 + i) & Mask) * Stride + Q->Slot];

        X[i] = (double)v;
        Q->Hist[(HistPos + i) & CORR_HIST_MASK] = v;
    }
    for (i = 0; i < N; i++) {
        Y[i] = (double)Q->Hist[(HistPos + i - Q->Delay) & CORR_HIST_MASK] * Q->Gain;
    }
    for (s = 0; s < Q->NSec && s < APOLLO_CORR_MAX_FILTERS; s++) {
        const CORR_SEC *q = &Q->Sec[s];
        double z1 = Q->Z[s][0], z2 = Q->Z[s][1];

        for (i = 0; i < N; i++) {
            double v = Y[i], o = q->b0 * v + z1;

            z1 = q->b1 * v - q->a1 * o + z2;
            z2 = q->b2 * v - q->a2 * o;
            Y[i] = o;
        }
        Q->Z[s][0] = (z1 < CORR_TINY && z1 > -CORR_TINY) ? 0.0 : z1;
        Q->Z[s][1] = (z2 < CORR_TINY && z2 > -CORR_TINY) ? 0.0 : z2;
    }
}

/* The running voice's channel on Slot, -1 if it has none. */
static __inline int CorrFindSlot(const CORR_PROG *G, unsigned Slot)
{
    unsigned c;

    for (c = 0; c < G->NCh && c < APOLLO_CORR_MAX_CHANNELS; c++) {
        if (G->Ch[c].Slot == Slot) {
            return (int)c;
        }
    }
    return -1;
}

/* One block of N (<= CORR_BLOCK) frames from F0. */
static __inline void CorrBlock(CORR_ENGINE *E, long *Ring, unsigned Mask, unsigned Stride, unsigned long long F0, unsigned N)
{
    CORR_PROG *cur = &E->Prog[E->Cur], *nxt = 0;
    unsigned c, i, w0 = E->W;
    int wet = CorrWantWet(E), write, steady;
    double inv = 1.0 / (double)E->FadeLen;

    if (!E->CurLive) {
        CorrVoiceStart(E, cur, 0, Ring, Mask, Stride, F0);
        E->CurLive = 1;
    }
    if (E->Next >= 0) {
        nxt = &E->Prog[E->Next];
        if (!E->NextLive) {
            CorrVoiceStart(E, nxt, cur, Ring, Mask, Stride, F0);
            E->NextLive = 1;
            E->X = 0;
        }
    }
    for (i = 0; i < N; i++) {
        if (wet) {
            if (E->W < E->FadeLen) {
                E->W++;
            }
        } else if (E->W > 0) {
            E->W--;
        }
        E->Gw[i] = CorrSmooth((double)E->W * inv);
        if (nxt != 0) {
            if (E->X < E->FadeLen) {
                E->X++;
            }
            E->Gx[i] = CorrSmooth((double)E->X * inv);
        }
    }
    write = (w0 > 0 || E->W > 0);
    steady = (nxt == 0 && w0 == E->FadeLen && E->W == E->FadeLen);

    for (c = 0; c < cur->NCh && c < APOLLO_CORR_MAX_CHANNELS; c++) {
        if (cur->Ch[c].Slot < Stride) {
            CorrChannelRun(&cur->Ch[c], Ring, Mask, Stride, F0, N, E->HistPos, E->Xc[c], E->Yc[c]);
        } else {
            E->SlotSkipped = 1;
        }
    }
    for (c = 0; nxt != 0 && c < nxt->NCh && c < APOLLO_CORR_MAX_CHANNELS; c++) {
        if (nxt->Ch[c].Slot < Stride) {
            CorrChannelRun(&nxt->Ch[c], Ring, Mask, Stride, F0, N, E->HistPos, E->Xn[c], E->Yn[c]);
        } else {
            E->SlotSkipped = 1;
        }
    }

    if (write) {
        for (c = 0; c < cur->NCh && c < APOLLO_CORR_MAX_CHANNELS; c++) {
            unsigned s = cur->Ch[c].Slot;
            int k = nxt != 0 ? CorrFindSlot(nxt, s) : -1;
            const double *x = E->Xc[c], *yc = E->Yc[c];

            if (s >= Stride) {
                continue;
            }
            for (i = 0; i < N; i++) {
                double out;

                if (steady) {
                    out = yc[i];
                } else {
                    double w = yc[i];

                    if (nxt != 0) {
                        double yn = k >= 0 ? E->Yn[k][i] : x[i];   /* Next leaves this slot dry */

                        w = (1.0 - E->Gx[i]) * yc[i] + E->Gx[i] * yn;
                    }
                    out = (1.0 - E->Gw[i]) * x[i] + E->Gw[i] * w;
                }
                Ring[(size_t)((F0 + i) & Mask) * Stride + s] = CorrToS32(out);
            }
        }
        for (c = 0; nxt != 0 && c < nxt->NCh && c < APOLLO_CORR_MAX_CHANNELS; c++) {
            unsigned s = nxt->Ch[c].Slot;
            const double *x = E->Xn[c], *yn = E->Yn[c];

            if (s >= Stride || CorrFindSlot(cur, s) >= 0) {
                continue;                       /* done above */
            }
            for (i = 0; i < N; i++) {
                double w = (1.0 - E->Gx[i]) * x[i] + E->Gx[i] * yn[i];   /* Cur left this slot dry */

                Ring[(size_t)((F0 + i) & Mask) * Stride + s] = CorrToS32((1.0 - E->Gw[i]) * x[i] + E->Gw[i] * w);
            }
        }
    }
    E->HistPos += N;

    if (nxt != 0 && E->X >= E->FadeLen) {
        /* the swap is complete: Next is the running voice, a pending load starts its own swap next block */
        E->Cur = E->Next;
        E->CurLive = 1;
        E->Next = E->Pending;
        E->Pending = -1;
        E->NextLive = 0;
        E->X = 0;
    }
    if (E->W == 0 && !CorrWantWet(E) && !CorrWantWarm(E)) {
        CorrStop(E);
    }
}

/* Frames [From, To) of the ring (Frames a power of two, Stride = its channels per frame). */
static __inline void CorrProcess(CORR_ENGINE *E, long *Ring, unsigned Frames, unsigned Stride, unsigned long long From,
                                 unsigned long long To)
{
    while (From < To && E->Cur >= 0 && CorrRunning(E)) {
        unsigned n = (To - From > CORR_BLOCK) ? CORR_BLOCK : (unsigned)(To - From);

        CorrBlock(E, Ring, Frames - 1u, Stride, From, n);
        From += n;
    }
}

/*
 * Never write below this (DISPATCH, EngineLock): a writer that (re)starts at Head takes the larger
 * of Head and the frames already corrected. Returns Head unchanged while nothing runs.
 */
static __inline unsigned long long CorrWriteFloor(const CORR_ENGINE *E, unsigned long long Head)
{
    if (E->Cur < 0 || !E->HeadValid || !CorrRunning(E)) {
        return Head;
    }
    return E->Head > Head ? E->Head : Head;
}

/*
 * The per-tick pump (DISPATCH, EngineLock): correct what every writer finished since the last call.
 *   Pos       the engine's 64-bit SAMPLE_POS snapshot (EngPos)
 *   DataHead  the engine's data head: its own clients wrote up to here (EngDataHead)
 *   Starts    the engine's start count (a new run renumbers frames)
 *   Ticks     the unit's tick count (for the age of an aggregate bound)
 * Returns the frames corrected. Off (no program, or nothing to run): returns 0 before it reads or
 * writes a frame.
 */
static __inline unsigned CorrPump(CORR_ENGINE *E, long *Ring, unsigned Frames, unsigned Stride, unsigned Rate,
                                  unsigned long long Pos, unsigned long long DataHead, unsigned long Starts,
                                  unsigned long long Ticks)
{
    unsigned long long bound, floor, n;

    if (E->Cur < 0 || !CorrRunning(E)) {
        E->HeadValid = 0;
        return 0;
    }
    if (Ring == 0 || Frames == 0 || (Frames & (Frames - 1u)) != 0 || Stride == 0) {
        return 0;
    }
    if (Starts != E->Starts) {
        /* a new run of the engine (bring-up, recovery, transport restart): the ring starts over, so do
           the voices, and the corrected signal fades in again */
        E->Starts = Starts;
        E->HeadValid = 0;
        E->CurLive = E->NextLive = 0;
        E->W = E->X = 0;
    }
    if (Rate != E->Prog[E->Cur].Rate || (E->Next >= 0 && Rate != E->Prog[E->Next].Rate)) {
        /* nothing runs at another rate; stop here so a disable, a clear or a swap still completes
           (CorrStop: voices cold, the newest program current, a clear done) and a later match fades in */
        E->RateMismatch = 1;
        CorrStop(E);
        return 0;
    }
    E->RateMismatch = 0;
    bound = DataHead;
    if (E->AggBounded && Ticks - E->AggTick <= CORR_AGG_STALE && E->AggBound < bound) {
        bound = E->AggBound;
    }
    floor = Pos + CorrSafety(Rate);
    if (!E->HeadValid || E->Head > DataHead + Frames / 2) {
        /* start (or a new run): every frame up to the bound is already written, correct from there */
        E->Head = bound > floor ? bound : floor;
        E->HeadValid = 1;
        return 0;
    }
    if (E->Head < floor) {
        E->Late += floor - E->Head;     /* fetched before they were corrected (a late tick) */
        E->Head = floor;
    }
    if (bound <= E->Head) {
        return 0;
    }
    n = bound - E->Head;
    if (n > CORR_MAX_RUN) {
        E->Late += n - CORR_MAX_RUN;
        E->Head = bound - CORR_MAX_RUN;
        n = CORR_MAX_RUN;
    }
    CorrProcess(E, Ring, Frames, Stride, E->Head, bound);
    E->Head = bound;
    E->Frames += n;
    E->Runs++;
    return (unsigned)n;
}

/* The aggregate says how far it wrote this follower's ring (0 = it does not write it). */
static __inline void CorrAggBound(CORR_ENGINE *E, unsigned long long Head, unsigned long long Ticks)
{
    E->AggBounded = Head != 0;
    E->AggBound = Head;
    E->AggTick = Ticks;
}
