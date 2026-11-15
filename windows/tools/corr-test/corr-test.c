/*
 * corr-test.c - offline checks of driver\corr_dsp.h, the speaker correction the driver runs
 * (include\apollo_corr.h). The same header the driver compiles, run here on the CPU:
 *
 *   1. engine vs Avium Measure: every channel of ref\studio.json and ref\max20.json at 48 and 192 kHz,
 *      through avium_measure.dsp.biquad.apply (scipy sosfilt, float64; ref\make-ref.py) and through
 *      the engine (delay, gain, sections), then the same through the full in-place path (rounded,
 *      saturated 32-bit samples); analytic responses (peak gain at f0, notch depth, first order at DC
 *      and Nyquist) and the driver's headroom measure against validate()'s peak_gain_db (ref\peaks.txt)
 *   2. off is bit-identical: no profile, a profile disabled, bypassed (A/B) after its fade, disabled
 *      after running, cleared; other slots and frames outside the window never touched, even when on;
 *      the writers' floor unchanged while off
 *   3. crossfades: enable, bypass, back, swap, a swap during a swap, disable: no step larger than the
 *      signals' own (a hard switch, checked too, is caught by the same measure); fade length 20 ms
 *   4. the driver's check refuses what must not run (sizes, counts, rates, non-finite numbers,
 *      unstable poles, gains above 0 dB, delay, slots, clipping, sub-5 Hz boosts, unterminated text),
 *      including the review's crafted profiles (F1: a narrow peak between grid points, sub-5 Hz peaks,
 *      resonances narrower than the format), and accepts the format's own narrowest boost
 *   5b. a clear during its fade stays cleared (F2); another sample rate stops cleanly (F3)
 *   5. the pump: start, late ticks, the aggregate's bound and its age, a new run, the run cap
 *   6. cost: 20 filters x 2 channels x 48 frames per tick (steady, while fading, during a swap)
 *
 * Usage: corr-test.exe [ref folder]   (default: ref next to the exe). Exit 0 = every check passed.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "..\..\driver\corr_dsp.h"
#include "..\..\driver\apollo_sat.h"     /* the writers' saturating ADD, as the engine's clients do */

static long ApolloSatAdd32Test(long A, long B)
{
    return ApolloSatAdd32(A, B);
}

static int g_fail, g_pass;
static char g_ref[MAX_PATH];

static void Check(int ok, const char *what)
{
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        printf("FAIL  %s\n", what);
    }
}

#define CHECKF(ok, ...) do { char m_[256]; _snprintf_s(m_, sizeof(m_), _TRUNCATE, __VA_ARGS__); Check((ok), m_); } while (0)

/* ------------------------------------------------------------------ */
/* Reference data                                                      */
/* ------------------------------------------------------------------ */

typedef struct REC {
    unsigned rate, delay, k;
    double gain;
    double sos[APOLLO_CORR_MAX_FILTERS][5];
    double *y;
} REC;

static unsigned g_n, g_nrec;
static double *g_x;
static REC g_rec[16];

static FILE *OpenRef(const char *name, const char *mode)
{
    char p[MAX_PATH];
    FILE *f = NULL;

    _snprintf_s(p, sizeof(p), _TRUNCATE, "%s\\%s", g_ref, name);
    if (fopen_s(&f, p, mode) != 0) {
        f = NULL;
    }
    return f;
}

static int LoadRef(void)
{
    FILE *f = OpenRef("engine-ref.bin", "rb");
    char magic[4];
    unsigned r;

    if (f == NULL) {
        printf("FAIL  cannot open %s\\engine-ref.bin\n", g_ref);
        return 0;
    }
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "CRF1", 4) != 0 || fread(&g_n, 4, 1, f) != 1 ||
        fread(&g_nrec, 4, 1, f) != 1 || g_nrec > 16 || g_n == 0 || g_n > (1u << 20)) {
        fclose(f);
        printf("FAIL  engine-ref.bin header\n");
        return 0;
    }
    g_x = (double *)malloc(sizeof(double) * g_n);
    if (g_x == NULL || fread(g_x, sizeof(double), g_n, f) != g_n) {
        fclose(f);
        return 0;
    }
    for (r = 0; r < g_nrec; r++) {
        REC *q = &g_rec[r];
        unsigned char h[20];
        unsigned i;

        if (fread(h, 1, 20, f) != 20) {
            fclose(f);
            return 0;
        }
        memcpy(&q->rate, h, 4);
        memcpy(&q->gain, h + 4, 8);
        memcpy(&q->delay, h + 12, 4);
        memcpy(&q->k, h + 16, 4);
        if (q->k > APOLLO_CORR_MAX_FILTERS) {
            fclose(f);
            return 0;
        }
        for (i = 0; i < q->k; i++) {
            if (fread(q->sos[i], sizeof(double), 5, f) != 5) {
                fclose(f);
                return 0;
            }
        }
        q->y = (double *)malloc(sizeof(double) * g_n);
        if (q->y == NULL || fread(q->y, sizeof(double), g_n, f) != g_n) {
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    return 1;
}

/* A program channel from a reference record (slot S). */
static void RecToChannel(const REC *q, CORR_PCH *c, unsigned Slot)
{
    unsigned i;

    memset(c, 0, sizeof(*c));
    c->Slot = Slot;
    c->Delay = q->delay;
    c->Gain = q->gain;
    c->NSec = q->k;
    for (i = 0; i < q->k; i++) {
        c->Sec[i].b0 = q->sos[i][0];
        c->Sec[i].b1 = q->sos[i][1];
        c->Sec[i].b2 = q->sos[i][2];
        c->Sec[i].a1 = q->sos[i][3];
        c->Sec[i].a2 = q->sos[i][4];
    }
}

/* A profile from two reference records (L on slot SL, R on slot SR), preamp/trim split as in studio.json. */
static void RecsToProfile(APOLLO_CORR_PROFILE *P, const REC *L, const REC *R, unsigned SL, unsigned SR, double PreampDb)
{
    const REC *r[2] = { L, R };
    unsigned c, i;

    memset(P, 0, sizeof(*P));
    P->Size = sizeof(*P);
    P->Version = APOLLO_CORR_VERSION;
    P->Rate = L->rate;
    P->ChannelCount = 2;
    P->PreampDb = PreampDb;
    P->Preamp = pow(10.0, PreampDb / 20.0);
    strcpy_s(P->Name, sizeof(P->Name), "test");
    strcpy_s(P->Id, sizeof(P->Id), "0123");
    for (c = 0; c < 2; c++) {
        APOLLO_CORR_CHANNEL *h = &P->Channel[c];

        strcpy_s(h->Id, sizeof(h->Id), c == 0 ? "L" : "R");
        h->Slot = c == 0 ? SL : SR;
        h->Output = c;
        h->Gain = r[c]->gain / P->Preamp;   /* the record's gain is preamp * trim */
        if (h->Gain > 1.0) {
            h->Gain = 1.0;
        }
        h->GainDb = 20.0 * log10(h->Gain);
        h->DelayUs = (unsigned long)floor((double)r[c]->delay * 1e6 / (double)r[c]->rate + 0.5);
        h->FilterCount = r[c]->k;
        for (i = 0; i < r[c]->k; i++) {
            memcpy(h->Filter[i].Coef, r[c]->sos[i], sizeof(h->Filter[i].Coef));
            h->Filter[i].Enabled = 1;
            h->Filter[i].Type = APOLLO_CORR_TYPE_PEAKING;
            h->Filter[i].FreqHz = 100.0;
            h->Filter[i].Q = 1.0;
        }
    }
}

static double Nan(void)
{
    union { double d; unsigned long long u; } b;

    b.u = 0x7FF8000000000000ull;
    return b.d;
}

/* One channel (L on slot 0), one section C, preamp Pre (linear), trim 0 dB, at 48 kHz. */
static void OneFilter(APOLLO_CORR_PROFILE *P, const double *C, double Pre);

/* RBJ designs (for the analytic and the sub-5 Hz checks), b0 b1 b2 a1 a2 */
static void DesignPeaking(double *C, double F0, double G, double Q, double Fs)
{
    double w0 = 2 * CORR_PI * F0 / Fs, A = pow(10.0, G / 40.0), al = sin(w0) / (2 * Q), cs = cos(w0), a0 = 1 + al / A;

    C[0] = (1 + al * A) / a0;
    C[1] = -2 * cs / a0;
    C[2] = (1 - al * A) / a0;
    C[3] = -2 * cs / a0;
    C[4] = (1 - al / A) / a0;
}

static void DesignLowShelf(double *C, double F0, double G, double Q, double Fs)
{
    double w0 = 2 * CORR_PI * F0 / Fs, A = pow(10.0, G / 40.0), al = sin(w0) / (2 * Q), cs = cos(w0), r = 2 * sqrt(A) * al;
    double a0 = (A + 1) + (A - 1) * cs + r;

    C[0] = A * ((A + 1) - (A - 1) * cs + r) / a0;
    C[1] = 2 * A * ((A - 1) - (A + 1) * cs) / a0;
    C[2] = A * ((A + 1) - (A - 1) * cs - r) / a0;
    C[3] = -2 * ((A - 1) + (A + 1) * cs) / a0;
    C[4] = ((A + 1) + (A - 1) * cs - r) / a0;
}

static void DesignNotch(double *C, double F0, double Q, double Fs)
{
    double w0 = 2 * CORR_PI * F0 / Fs, al = sin(w0) / (2 * Q), cs = cos(w0), a0 = 1 + al;

    C[0] = 1 / a0;
    C[1] = -2 * cs / a0;
    C[2] = 1 / a0;
    C[3] = -2 * cs / a0;
    C[4] = (1 - al) / a0;
}

/* ------------------------------------------------------------------ */
/* Engine helpers                                                      */
/* ------------------------------------------------------------------ */

static CORR_ENGINE *NewEngine(void)
{
    CORR_ENGINE *e = (CORR_ENGINE *)calloc(1, sizeof(CORR_ENGINE));

    if (e != NULL) {
        CorrEngineInit(e);
    }
    return e;
}

/* Load a profile the way corr.c does (reserve, fill, install). */
static void Load(CORR_ENGINE *E, const APOLLO_CORR_PROFILE *P, int Enable)
{
    int idx = CorrReserve(E);

    CorrProgramFill(&E->Prog[idx], P);
    CorrInstall(E, idx, Enable);
}

#define T_FRAMES    8192u
#define T_STRIDE    34u
#define T_LEAD      144u
#define T_TICK      48u

typedef struct SIM {
    long *ring;                 /* T_FRAMES x T_STRIDE */
    unsigned long long pos;     /* hardware position */
    unsigned long long head;    /* writers' data head */
    unsigned long starts;
    unsigned long long ticks;
} SIM;

/* A test signal per slot and frame: tones plus noise, deterministic, ~-10 dBFS. */
static long Source(unsigned Slot, unsigned long long F)
{
    double t = (double)F / 48000.0;
    unsigned long long h = (F * 0x9E3779B97F4A7C15ull) ^ ((unsigned long long)Slot * 0xBF58476D1CE4E5B9ull);
    double noise = ((double)((h >> 11) & 0xFFFFF) / 1048576.0 - 0.5) * 0.02;
    double v = 0.18 * sin(2 * CORR_PI * (55.0 + 3.0 * Slot) * t) + 0.09 * sin(2 * CORR_PI * 440.0 * t) +
               0.04 * sin(2 * CORR_PI * 3150.0 * t + Slot) + noise;

    return (long)floor(v * 2147483647.0 + 0.5);
}

/* One engine tick: the writers fill [head, pos + lead) (the zero-ahead + ADD collapse to "the source" here),
   then the pump. Out (optional) gets slot S's final value of every frame the hardware is about to fetch. */
static unsigned SimTick(SIM *S, CORR_ENGINE *E, unsigned Rate)
{
    unsigned long long to = S->pos + T_LEAD, f;
    unsigned c, n;

    for (f = S->head; f < to; f++) {
        for (c = 0; c < T_STRIDE; c++) {
            S->ring[(size_t)(f & (T_FRAMES - 1)) * T_STRIDE + c] = Source(c, f);
        }
    }
    S->head = to;
    S->ticks++;
    n = CorrPump(E, S->ring, T_FRAMES, T_STRIDE, Rate, S->pos, S->head, S->starts, S->ticks);
    S->pos += T_TICK;
    return n;
}

static long RingAt(const SIM *S, unsigned Slot, unsigned long long F)
{
    return S->ring[(size_t)(F & (T_FRAMES - 1)) * T_STRIDE + Slot];
}

static void SimInit(SIM *S)
{
    S->ring = (long *)calloc((size_t)T_FRAMES * T_STRIDE, sizeof(long));
    S->pos = 100000;
    S->head = S->pos;
    S->starts = 1;
    S->ticks = 0;
}

/* ------------------------------------------------------------------ */
/* 1. Engine against Avium Measure and analytic responses              */
/* ------------------------------------------------------------------ */

static void TestReference(void)
{
    static const char *names[] = { "studio 48k L", "studio 48k R", "studio 192k L", "studio 192k R",
                                   "max20 48k L", "max20 48k R", "max20 192k L", "max20 192k R" };
    unsigned r, i;
    long *ring = (long *)calloc(g_n, sizeof(long));
    double *xs = (double *)malloc(sizeof(double) * CORR_BLOCK), *ys = (double *)malloc(sizeof(double) * g_n);
    CORR_PCH *ch = (CORR_PCH *)calloc(1, sizeof(CORR_PCH));

    for (i = 0; i < g_n; i++) {
        ring[i] = (long)g_x[i];
    }
    for (r = 0; r < g_nrec; r++) {
        const REC *q = &g_rec[r];
        double peak = 0, err = 0;
        unsigned off;

        RecToChannel(q, ch, 0);
        for (off = 0; off < g_n; off += T_TICK) {
            unsigned n = (g_n - off < T_TICK) ? g_n - off : T_TICK;

            CorrChannelRun(ch, ring, (unsigned)(g_n - 1), 1, off, n, off, xs, ys + off);
        }
        for (i = 0; i < g_n; i++) {
            double d = fabs(ys[i] - q->y[i]);

            if (fabs(q->y[i]) > peak) {
                peak = fabs(q->y[i]);
            }
            if (d > err) {
                err = d;
            }
        }
        CHECKF(err <= 1e-12 * peak, "engine = Avium Measure (scipy sosfilt), %s: worst error %.3g of peak %.3g (%.1f dB down)",
               r < 8 ? names[r] : "?", err, peak, err > 0 ? 20 * log10(err / peak) : -400.0);
        if (r == 0 || r == 6) {
            printf("      %s: %u sections, delay %u, worst difference to scipy %.2e of full scale\n", names[r], q->k, q->delay,
                   err / 2147483648.0);
        }
    }

    /* The full in-place path, steady corrected: rounded and saturated, within one LSB of the reference */
    for (r = 0; r < g_nrec; r++) {
        const REC *q = &g_rec[r];
        CORR_ENGINE *e = NewEngine();
        unsigned long long f;
        long worst = 0;
        int sat = 1;
        long *ring2 = (long *)calloc(2 * (size_t)g_n, sizeof(long));   /* frames n..2n-1 = the zeros before frame 0 */

        for (i = 0; i < g_n; i++) {
            ring2[i] = (long)g_x[i];
        }
        e->Cur = 0;
        e->Prog[0].NCh = 1;
        e->Prog[0].Rate = q->rate;
        RecToChannel(q, &e->Prog[0].Ch[0], 0);
        e->Enabled = 1;
        CorrSetFade(e, q->rate);
        e->W = e->FadeLen;              /* steady corrected */
        for (f = 0; f < g_n; f += T_TICK) {
            CorrProcess(e, ring2, 2 * g_n, 1, f, f + T_TICK);
        }
        for (i = 0; i < g_n; i++) {
            long want = CorrToS32(q->y[i]), d = ring2[i] - want;

            if (d < 0) {
                d = -d;
            }
            if (d > worst) {
                worst = d;
            }
            if ((q->y[i] > 2147483647.0 && ring2[i] != 2147483647L) || (q->y[i] < -2147483648.0 && ring2[i] != -2147483647L - 1)) {
                sat = 0;
            }
        }
        free(ring2);
        CHECKF(worst <= 1, "in-place path %s: rounded output within %ld LSB of the reference", r < 8 ? names[r] : "?", worst);
        CHECKF(sat, "in-place path %s: saturates at the 32-bit limits (not MINLONG)", r < 8 ? names[r] : "?");
        free(e);
    }

    /* Analytic: a +6 dB peak at 1 kHz is 6 dB there; a notch is deep; 6 dB/oct filters at DC and Nyquist */
    {
        double c[5], mag;
        CORR_ENGINE *e = NewEngine();
        unsigned n = 48000;
        long *rg = (long *)calloc(n, sizeof(long));
        double in = 0, outp = 0;

        DesignPeaking(c, 1000.0, 6.0, 2.0, 48000.0);
        mag = 10 * log10(CorrMag2At(c, 1000.0, 48000.0));
        CHECKF(fabs(mag - 6.0) < 1e-9, "|H| of a +6 dB peak at its f0 = %.12f dB", mag);
        for (i = 0; i < n; i++) {
            rg[i] = (long)floor(0.25 * 2147483647.0 * sin(2 * CORR_PI * 1000.0 * i / 48000.0) + 0.5);
        }
        e->Cur = 0;
        e->Prog[0].NCh = 1;
        e->Prog[0].Rate = 48000;
        e->Prog[0].Ch[0].Gain = 1.0;
        e->Prog[0].Ch[0].NSec = 1;
        e->Prog[0].Ch[0].Sec[0].b0 = c[0];
        e->Prog[0].Ch[0].Sec[0].b1 = c[1];
        e->Prog[0].Ch[0].Sec[0].b2 = c[2];
        e->Prog[0].Ch[0].Sec[0].a1 = c[3];
        e->Prog[0].Ch[0].Sec[0].a2 = c[4];
        e->Enabled = 1;
        e->W = e->FadeLen;
        for (i = 0; i < 24000; i++) {
            in += (double)rg[i + 24000] * rg[i + 24000];
        }
        CorrProcess(e, rg, 32768, 1, 0, 32768);     /* the first 32768 frames (wraps nothing) */
        for (i = 24000; i < 32640; i++) {           /* 180 whole periods */
            outp += (double)rg[i] * rg[i];
        }
        mag = 10 * log10((outp / (32640 - 24000)) / (in / 24000));
        CHECKF(fabs(mag - 6.0) < 0.001, "a 1 kHz sine through the +6 dB peak comes out %.4f dB louder", mag);
        DesignNotch(c, 2000.0, 5.0, 48000.0);
        CHECKF(CorrMag2At(c, 2000.0, 48000.0) < 1e-16, "a notch at its f0: |H|^2 = %.3g (below -160 dB)",
               CorrMag2At(c, 2000.0, 48000.0));
        free(rg);
        free(e);
    }
    {
        /* first order (from studio's highpass_1 at 18 Hz, a section with b2 = a2 = 0): 0 at DC, 1 at Nyquist */
        const REC *q = &g_rec[0];
        unsigned k;
        int found = 0;

        for (k = 0; k < q->k; k++) {
            if (q->sos[k][2] == 0.0 && q->sos[k][4] == 0.0) {
                double dc = CorrMag2(q->sos[k], 0.0, 0), ny = CorrMag2(q->sos[k], 0.0, 1);

                CHECKF(dc < 1e-24 && fabs(ny - 1.0) < 1e-12, "highpass_1: |H|^2 at DC %.2g, at Nyquist %.15f", dc, ny);
                found = 1;
            }
        }
        Check(found, "studio L has its first-order high-pass");
    }
    {
        /* CorrSin against the CRT, and |H|^2 against a direct complex evaluation (every reference section) */
        double worst = 0, x;
        unsigned r2, k2;

        for (x = 1e-9; x <= CORR_PI / 2; x *= 1.001) {
            double d = fabs(CorrSin(x) - sin(x)) / sin(x);

            if (d > worst) {
                worst = d;
            }
        }
        CHECKF(worst < 1e-15, "CorrSin within %.2g (relative) of sin() on [1e-9, pi/2]", worst);
        worst = 0;
        for (r2 = 0; r2 < g_nrec; r2++) {
            for (k2 = 0; k2 < g_rec[r2].k; k2++) {
                const double *c = g_rec[r2].sos[k2];
                double fs = (double)g_rec[r2].rate, fr;

                for (fr = 200.0; fr < fs / 2 * 0.95; fr *= 1.1) {
                    double w = 2 * CORR_PI * fr / fs;
                    double br = c[0] + c[1] * cos(w) + c[2] * cos(2 * w), bi = -(c[1] * sin(w) + c[2] * sin(2 * w));
                    double ar = 1 + c[3] * cos(w) + c[4] * cos(2 * w), ai = -(c[3] * sin(w) + c[4] * sin(2 * w));
                    double want = (br * br + bi * bi) / (ar * ar + ai * ai), got = CorrMag2At(c, fr, fs);
                    double d = fabs(got - want) / want;

                    if (d > worst) {
                        worst = d;
                    }
                }
            }
        }
        CHECKF(worst < 1e-9, "|H|^2 of every reference section, 200 Hz up: within %.2g of a direct complex evaluation", worst);
        x = 1.0;
        for (i = 0; i < 192; i++) {
            x *= CORR_GRID_RATIO;
        }
        CHECKF(fabs(x - 2.0) < 1e-13, "the grid ratio is 2^(1/192): 192 steps = %.15f", x);
    }

    /* The driver's headroom measure = validate()'s peak_gain_db (studio, max20 at 48 kHz; ref\peaks.txt) */
    {
        FILE *f = OpenRef("peaks.txt", "r");
        char name[32], id[8];
        double peak, trim;
        int matched = 0;

        Check(f != NULL, "ref\\peaks.txt opens");
        while (f != NULL && fscanf_s(f, "%31s %7s", name, (unsigned)sizeof(name), id, (unsigned)sizeof(id)) == 2) {
            if (strcmp(id, "preamp") == 0) {
                (void)fscanf_s(f, "%lf", &peak);
                continue;
            }
            if (fscanf_s(f, "%lf %lf", &peak, &trim) != 2) {
                break;
            }
            {
                int ri = -1;
                APOLLO_CORR_CHANNEL *h = (APOLLO_CORR_CHANNEL *)calloc(1, sizeof(APOLLO_CORR_CHANNEL));
                double dc, mine;
                unsigned k;

                unsigned rate = 48000;

                if (strcmp(name, "studio") == 0) {
                    ri = strcmp(id, "L") == 0 ? 0 : 1;
                } else if (strcmp(name, "max20") == 0) {
                    ri = strcmp(id, "L") == 0 ? 4 : 5;
                } else if (strcmp(name, "studio192") == 0) {
                    ri = strcmp(id, "L") == 0 ? 2 : 3;
                    rate = 192000;
                } else if (strcmp(name, "max20192") == 0) {
                    ri = strcmp(id, "L") == 0 ? 6 : 7;
                    rate = 192000;
                }
                if (ri < 0 || h == NULL) {
                    free(h);
                    continue;
                }
                h->FilterCount = g_rec[ri].k;
                for (k = 0; k < g_rec[ri].k; k++) {
                    memcpy(h->Filter[k].Coef, g_rec[ri].sos[k], sizeof(h->Filter[k].Coef));
                    h->Filter[k].Enabled = 1;
                }
                mine = 10 * log10(CorrChannelPeak2(h, rate, &dc)) + trim;
                CHECKF(fabs(mine - peak) < 1e-6, "headroom %s %s at %u Hz: driver %+.9f dB, Avium Measure %+.9f dB", name, id,
                       rate, mine, peak);
                matched++;
                free(h);
            }
        }
        if (f != NULL) {
            fclose(f);
        }
        Check(matched == 8, "eight channels compared with Avium Measure's peaks (48 and 192 kHz)");
    }
    free(ring);
    free(xs);
    free(ys);
    free(ch);
}

/* ------------------------------------------------------------------ */
/* 2. Off is bit-identical                                             */
/* ------------------------------------------------------------------ */

/* Runs Ticks ticks; Same = every frame the hardware fetched equals the source, on every slot. */
static int RunCompare(SIM *S, CORR_ENGINE *E, unsigned Ticks, unsigned SlotsFrom, int *Same)
{
    unsigned t, c;
    unsigned long long f;

    *Same = 1;
    for (t = 0; t < Ticks; t++) {
        unsigned long long p0 = S->pos;

        SimTick(S, E, 48000);
        /* the frames that the hardware fetches before the next tick are final now */
        for (f = p0 + CORR_SAFETY; f < S->pos + CORR_SAFETY; f++) {
            for (c = SlotsFrom; c < T_STRIDE; c++) {
                if (RingAt(S, c, f) != Source(c, f)) {
                    *Same = 0;
                }
            }
        }
    }
    return 1;
}

static APOLLO_CORR_PROFILE g_studio48, g_max2048, g_studio192;

static void TestBypass(void)
{
    SIM s;
    CORR_ENGINE *e = NewEngine();
    int same;
    unsigned n = 0, t;
    long *copy = (long *)malloc(sizeof(long) * T_FRAMES * T_STRIDE);

    SimInit(&s);
    /* no profile */
    RunCompare(&s, e, 200, 0, &same);
    Check(same, "off (no profile): every slot of every frame is the writers' sum, bit for bit");
    memcpy(copy, s.ring, sizeof(long) * T_FRAMES * T_STRIDE);
    n = CorrPump(e, s.ring, T_FRAMES, T_STRIDE, 48000, s.pos, s.head + 4000, s.starts, s.ticks);
    Check(n == 0 && memcmp(copy, s.ring, sizeof(long) * T_FRAMES * T_STRIDE) == 0,
          "off: the pump returns 0 and the ring's bytes are unchanged (memcmp of all 8192 x 34)");
    Check(CorrWriteFloor(e, 12345) == 12345, "off: the writers' floor is their own head (agg.c unchanged)");
    Check(!CorrRunning(e), "off: nothing runs (corr.c's tick fast path returns before EngineLock)");

    /* loaded, not enabled */
    Load(e, &g_studio48, 0);
    RunCompare(&s, e, 200, 0, &same);
    Check(same && !CorrRunning(e), "loaded but disabled: bit-identical, nothing runs");
    Check(CorrWriteFloor(e, 777) == 777, "loaded but disabled: floor unchanged");

    /* enabled: slots 0/1 corrected, all others untouched */
    CorrSetEnabled(e, 1);
    RunCompare(&s, e, 200, 2, &same);
    Check(same, "enabled: slots 2-33 stay bit-identical (only the profile's slots are written)");
    RunCompare(&s, e, 4, 0, &same);
    Check(!same, "enabled: slots 0/1 are corrected (they differ from the source)");
    /* bypass (A/B): after the fade the output is the source again, bit for bit, with the filters warm */
    CorrSetBypass(e, 1);
    RunCompare(&s, e, 30, 0, &same);       /* the fade */
    RunCompare(&s, e, 300, 0, &same);
    Check(same, "bypassed (A/B) after its 20 ms fade: bit-identical on every slot");
    Check(CorrRunning(e) && e->W == 0 && e->CurLive, "bypassed: the filters keep running (warm) for a seamless return");
    memcpy(copy, s.ring, sizeof(long) * T_FRAMES * T_STRIDE);
    /* back on, then disabled: after the fade out bit-identical and nothing runs */
    CorrSetBypass(e, 0);
    RunCompare(&s, e, 100, 0, &same);
    Check(!same, "A/B back: corrected again");
    CorrSetEnabled(e, 0);
    RunCompare(&s, e, 30, 0, &same);
    RunCompare(&s, e, 300, 0, &same);
    Check(same && !CorrRunning(e), "disabled after running: bit-identical after the fade, nothing runs");
    Check(CorrWriteFloor(e, 99) == 99, "disabled: floor unchanged");
    /* cleared */
    CorrSetEnabled(e, 1);
    RunCompare(&s, e, 60, 0, &same);
    CorrClear(e);
    for (t = 0; t < 40; t++) {
        SimTick(&s, e, 48000);
    }
    Check(e->Cur < 0 && !CorrRunning(e), "cleared: faded out, then no profile");
    RunCompare(&s, e, 100, 0, &same);
    Check(same, "cleared: bit-identical");
    /* rate mismatch: a 192 kHz profile on a 48 kHz unit never runs */
    Load(e, &g_studio192, 1);
    RunCompare(&s, e, 100, 0, &same);
    Check(same && e->RateMismatch, "a profile for another rate does not run (flagged, bit-identical)");
    CorrClear(e);
    CorrStop(e);
    free(copy);
    free(s.ring);
    free(e);
}

/*
 * The render path with correction off is the one without it, byte for byte: two rings in lockstep,
 * the same writers (ADDs, as engine.c / agg.c do), late ticks and a new engine run, one of them with
 * the pump after every tick (engine.c's ApolloCorrAfterTick and agg.c's ApolloCorrAggFollower) and the
 * aggregate's floor applied to a writer that restarts; memcmp of the whole ring after every tick.
 */
static int Lockstep(CORR_ENGINE *E, unsigned Ticks, unsigned long long *Pos, unsigned long *Starts)
{
    long *a = (long *)calloc((size_t)T_FRAMES * T_STRIDE, sizeof(long));
    long *b = (long *)calloc((size_t)T_FRAMES * T_STRIDE, sizeof(long));
    unsigned long long head = *Pos, fhead = 0;
    unsigned t, c;
    int same = 1;

    for (t = 0; t < Ticks && same; t++) {
        unsigned long long to = *Pos + T_LEAD, f, from;
        /* the engine: zero ahead, then two clients ADD (saturating) */
        for (f = *Pos + T_LEAD; f < *Pos + T_LEAD + 512; f++) {
            for (c = 0; c < T_STRIDE; c++) {
                a[(size_t)(f & (T_FRAMES - 1)) * T_STRIDE + c] = 0;
                b[(size_t)(f & (T_FRAMES - 1)) * T_STRIDE + c] = 0;
            }
        }
        if (head < *Pos + CORR_SAFETY) {
            head = *Pos + CORR_SAFETY;          /* a late tick */
        }
        for (f = head; f < to; f++) {
            for (c = 0; c < T_STRIDE; c++) {
                size_t i = (size_t)(f & (T_FRAMES - 1)) * T_STRIDE + c;
                long v1 = Source(c, f), v2 = Source(c + 7, f * 3) / 4;

                a[i] = ApolloSatAdd32Test(ApolloSatAdd32Test(a[i], v1), v2);
                b[i] = ApolloSatAdd32Test(ApolloSatAdd32Test(b[i], v1), v2);
            }
        }
        head = to;
        /* the aggregate: a writer that (re)starts every 37 ticks takes the floor (agg.c AggFollowerRender) */
        from = (t % 37 == 0 || fhead < *Pos + CORR_SAFETY) ? *Pos + CORR_SAFETY : fhead;
        from = CorrWriteFloor(E, from);
        for (f = from; f < to - 20; f++) {
            size_t i = (size_t)(f & (T_FRAMES - 1)) * T_STRIDE + 1;

            a[i] = ApolloSatAdd32Test(a[i], 12345);
            b[i] = ApolloSatAdd32Test(b[i], 12345);
        }
        fhead = to - 20;
        CorrAggBound(E, fhead, (unsigned long long)t);
        (void)CorrPump(E, b, T_FRAMES, T_STRIDE, 48000, *Pos, head, *Starts, (unsigned long long)t);
        (void)CorrPump(E, b, T_FRAMES, T_STRIDE, 48000, *Pos, head, *Starts, (unsigned long long)t);
        if (memcmp(a, b, sizeof(long) * T_FRAMES * T_STRIDE) != 0) {
            same = 0;
        }
        *Pos += (t % 101 == 50) ? 700 : T_TICK;     /* now and then a stall */
        if (t % 997 == 500) {
            (*Starts)++;                             /* a new engine run */
            *Pos += 100000;
            head = *Pos;
        }
    }
    free(a);
    free(b);
    return same;
}

static void TestLockstep(void)
{
    CORR_ENGINE *e = NewEngine();
    unsigned long long pos = 1000;
    unsigned long starts = 1;
    unsigned t;

    Check(Lockstep(e, 3000, &pos, &starts), "off (no profile): the whole ring is byte-identical to the path without correction, "
          "3000 ticks with late ticks, a new run and a restarting writer");
    Load(e, &g_studio48, 0);
    Check(Lockstep(e, 3000, &pos, &starts), "loaded, disabled: byte-identical");
    CorrSetEnabled(e, 1);
    CorrSetBypass(e, 1);
    Check(Lockstep(e, 3000, &pos, &starts) && CorrRunning(e) && e->CurLive, "enabled and bypassed from the start (filters warm): byte-identical");
    CorrSetBypass(e, 0);
    Check(!Lockstep(e, 200, &pos, &starts), "on: the ring differs (the same harness sees correction)");
    CorrSetEnabled(e, 0);
    for (t = 0; t < 4 && CorrRunning(e); t++) {
        (void)Lockstep(e, 30, &pos, &starts);
    }
    Check(!CorrRunning(e) && Lockstep(e, 3000, &pos, &starts), "turned off after running: byte-identical again once faded out");
    free(e);
}

/* ------------------------------------------------------------------ */
/* 3. Crossfades                                                       */
/* ------------------------------------------------------------------ */

/*
 * The largest first difference of slot 0's output in [From, To) against what the signals themselves
 * allow there: max(|step of the dry source|, |step of the steady corrected output|) plus the fade's
 * own slope. A click is a step beyond that.
 */
typedef struct TRACE {
    double *y;          /* what the hardware played on slot 0 */
    unsigned long long f0;
    unsigned n;
} TRACE;

static void Record(SIM *S, CORR_ENGINE *E, TRACE *T, unsigned Ticks)
{
    unsigned t;
    unsigned long long f;

    for (t = 0; t < Ticks; t++) {
        unsigned long long p0 = S->pos;

        SimTick(S, E, 48000);
        for (f = p0 + CORR_SAFETY; f < S->pos + CORR_SAFETY; f++) {
            if (T->n == 0) {
                T->f0 = f;
            }
            T->y[T->n++] = (double)RingAt(S, 0, f) / 2147483648.0;
        }
    }
}

static double MaxStep(const double *Y, unsigned From, unsigned To)
{
    double m = 0;
    unsigned i;

    for (i = From + 1; i < To; i++) {
        double d = fabs(Y[i] - Y[i - 1]);

        if (d > m) {
            m = d;
        }
    }
    return m;
}

/* A steady reference over the same frames: P NULL = the source itself, else P corrected from the start. */
static void RefTrace(const APOLLO_CORR_PROFILE *P, TRACE *T, unsigned Ticks)
{
    SIM s;
    CORR_ENGINE *e = NewEngine();

    SimInit(&s);
    if (P != NULL) {
        Load(e, P, 1);
        e->W = e->FadeLen;              /* corrected from the first frame: no fade */
        e->Starts = s.starts;           /* (the same engine run: no fade in for a new one) */
    }
    T->n = 0;
    Record(&s, e, T, Ticks);
    free(s.ring);
    free(e);
}

static void TestCrossfades(void)
{
    SIM s;
    CORR_ENGINE *e = NewEngine();
    TRACE tr, dry, wa, wb;
    unsigned i, fade, ticks = 0, k, kmax = 0;
    double base = 0, stepFade, allow, dmax = 0;
    unsigned mark[10], nm = 0;
    double *hard;
    static const char *what[] = { "enable (fade in)", "A/B to the source", "A/B back to corrected (warm filters)",
                                  "swap to another profile", "swap, then a queued swap", "disable (fade out)" };

    tr.y = (double *)malloc(sizeof(double) * 400000);
    dry.y = (double *)malloc(sizeof(double) * 400000);
    wa.y = (double *)malloc(sizeof(double) * 400000);
    wb.y = (double *)malloc(sizeof(double) * 400000);
    tr.n = 0;
    SimInit(&s);
    Load(e, &g_studio48, 0);
    Record(&s, e, &tr, 100);                    /* the source, a profile loaded but off */
    ticks += 100;
    mark[nm++] = tr.n;
    CorrSetEnabled(e, 1);                       /* fade in */
    {
        /* the fade is exactly 20 ms of frames from the first one corrected */
        unsigned long long frames = 0;
        unsigned t;

        for (t = 0; t < 200 && e->W < e->FadeLen; t++) {
            unsigned long long p0 = s.pos, f;

            frames += SimTick(&s, e, 48000);
            for (f = p0 + CORR_SAFETY; f < s.pos + CORR_SAFETY; f++) {
                tr.y[tr.n++] = (double)RingAt(&s, 0, f) / 2147483648.0;
            }
            ticks++;
        }
        fade = e->FadeLen;
        CHECKF(fade == 960 && frames == fade, "the fade in takes 20 ms: %llu frames corrected when it completes (960)",
               frames);
    }
    Record(&s, e, &tr, 150);
    ticks += 150;
    Check(e->W == e->FadeLen, "enabled: fully corrected after the fade");
    mark[nm++] = tr.n;
    CorrSetBypass(e, 1);                        /* A/B to the source */
    Record(&s, e, &tr, 100);
    ticks += 100;
    Check(e->W == 0, "A/B: back to the source after the fade");
    mark[nm++] = tr.n;
    CorrSetBypass(e, 0);                        /* A/B back (the filters stayed warm) */
    Record(&s, e, &tr, 100);
    ticks += 100;
    mark[nm++] = tr.n;
    Load(e, &g_max2048, 1);                     /* swap */
    Check(e->Next >= 0, "a load while running starts a swap");
    Record(&s, e, &tr, 150);
    ticks += 150;
    Check(e->Next < 0 && e->Prog[e->Cur].Ch[0].NSec == 20, "swap done: max20 runs");
    mark[nm++] = tr.n;
    Load(e, &g_studio48, 1);                    /* swap ... */
    Record(&s, e, &tr, 4);
    Load(e, &g_max2048, 1);                     /* ... and another load during it: queued */
    Check(e->Pending >= 0, "a load during a swap waits (Pending)");
    Record(&s, e, &tr, 200);
    ticks += 204;
    Check(e->Next < 0 && e->Pending < 0 && e->Prog[e->Cur].Ch[0].NSec == 20,
          "both swaps done in turn: the last load (max20) runs");
    mark[nm++] = tr.n;
    CorrSetEnabled(e, 0);                       /* fade out */
    Record(&s, e, &tr, 100);
    ticks += 100;
    Check(!CorrRunning(e), "disabled: stopped after the fade");
    mark[nm++] = tr.n;

    /* What the signals allow: the steps of the source and of each profile's steady output, same frames */
    RefTrace(NULL, &dry, ticks);
    RefTrace(&g_studio48, &wa, ticks);
    RefTrace(&g_max2048, &wb, ticks);
    Check(dry.n == tr.n && wa.n == tr.n && wb.n == tr.n && dry.f0 == tr.f0, "the references cover the same frames");
    for (i = 2400; i + 1 < tr.n && i + 1 < dry.n && i + 1 < wa.n && i + 1 < wb.n; i++) {
        double a = fabs(dry.y[i + 1] - dry.y[i]), b = fabs(wa.y[i + 1] - wa.y[i]), c = fabs(wb.y[i + 1] - wb.y[i]);

        if (a > base) base = a;
        if (b > base) base = b;
        if (c > base) base = c;
    }
    stepFade = 1.5 / (double)fade * 1.2;        /* smoothstep's steepest slope x |difference| (<= 1.2 of full scale) */
    allow = base * 1.02 + stepFade + 4.0 / 2147483648.0;
    printf("      largest step of the source / corrected signals %.5f; allowed in a transition %.5f (of full scale)\n",
           base, allow);
    for (i = 0; i + 1 < nm; i++) {
        double m = MaxStep(tr.y, mark[i] > 0 ? mark[i] - 1 : 0, mark[i + 1]);

        CHECKF(m <= allow, "no click: %s, largest step %.5f <= %.5f", what[i], m, allow);
    }
    /* the measure catches a click: the source switched to the corrected signal with no fade */
    for (k = mark[0] + 1000; k < mark[0] + 3000 && k < tr.n; k++) {
        if (fabs(wa.y[k] - dry.y[k]) > dmax) {
            dmax = fabs(wa.y[k] - dry.y[k]);
            kmax = k;
        }
    }
    hard = (double *)malloc(sizeof(double) * tr.n);
    for (k = 0; k < tr.n; k++) {
        hard[k] = k < kmax ? dry.y[k] : wa.y[k];
    }
    CHECKF(MaxStep(hard, kmax - 2, kmax + 2) > allow, "the same measure sees a hard switch as a click (%.5f > %.5f)",
           MaxStep(hard, kmax - 2, kmax + 2), allow);
    free(hard);

    /* a cold start with a delay: the delayed path starts from the ring's history, no jump */
    {
        SIM s3;
        CORR_ENGINE *e3 = NewEngine();
        TRACE t3;

        t3.y = (double *)malloc(sizeof(double) * 100000);
        t3.n = 0;
        SimInit(&s3);
        Load(e3, &g_studio48, 0);
        e3->Prog[e3->Cur].Ch[0].Delay = 300;    /* 6.25 ms on slot 0 */
        Record(&s3, e3, &t3, 50);
        k = t3.n;
        CorrSetEnabled(e3, 1);
        Record(&s3, e3, &t3, 100);
        CHECKF(MaxStep(t3.y, k - 1, t3.n) <= allow,
               "enable with a 6.25 ms delay: no jump when the delayed signal comes in (%.5f <= %.5f)",
               MaxStep(t3.y, k - 1, t3.n), allow);
        free(t3.y);
        free(s3.ring);
        free(e3);
    }
    free(tr.y);
    free(dry.y);
    free(wa.y);
    free(wb.y);
    free(s.ring);
    free(e);
}

/* ------------------------------------------------------------------ */
/* 4. The driver's check                                               */
/* ------------------------------------------------------------------ */

static void TestValidate(void)
{
    APOLLO_CORR_PROFILE *p = (APOLLO_CORR_PROFILE *)malloc(sizeof(*p));
    unsigned long ch, fl, err;

#define MUT(stmt, want, what) do { *p = g_studio48; stmt; err = CorrValidate(p, 34, &ch, &fl); \
        CHECKF(err == (want), "refused: %s (got %lu, want %lu)", what, err, (unsigned long)(want)); } while (0)

    err = CorrValidate(&g_studio48, 34, &ch, &fl);
    CHECKF(err == 0, "the studio profile (preamp -2.5 dB) passes the driver's check (got %lu)", err);
    err = CorrValidate(&g_max2048, 34, &ch, &fl);
    CHECKF(err == 0, "twenty filters a channel pass (got %lu)", err);
    err = CorrValidate(&g_studio192, 0, &ch, &fl);
    CHECKF(err == 0, "the 192 kHz set passes (got %lu)", err);
    MUT(p->Size = 100, APOLLO_CORR_ERR_SIZE, "size");
    MUT(p->Version = 2, APOLLO_CORR_ERR_SIZE, "version 2");
    MUT(p->Rate = 12345, APOLLO_CORR_ERR_RATE, "12345 Hz");
    MUT(p->ChannelCount = 0, APOLLO_CORR_ERR_CHANNELS, "no channels");
    MUT(p->ChannelCount = 5, APOLLO_CORR_ERR_CHANNELS, "five channels");
    MUT(p->Channel[1].FilterCount = 21, APOLLO_CORR_ERR_FILTERS, "21 filters");
    MUT(p->Channel[0].Filter[2].Coef[1] = Nan(), APOLLO_CORR_ERR_NOT_FINITE, "a NaN coefficient");
    MUT(p->Channel[0].Filter[0].Coef[0] = HUGE_VAL, APOLLO_CORR_ERR_NOT_FINITE, "an infinite coefficient");
    MUT(p->Channel[1].Gain = HUGE_VAL, APOLLO_CORR_ERR_NOT_FINITE, "an infinite trim");
    MUT(p->Channel[1].Filter[0].Q = -HUGE_VAL, APOLLO_CORR_ERR_NOT_FINITE, "a non-finite display number");
    MUT((p->Channel[0].Filter[0].Coef[3] = -2.5, p->Channel[0].Filter[0].Coef[4] = 1.6), APOLLO_CORR_ERR_UNSTABLE,
        "poles outside the unit circle");
    MUT((p->Channel[0].Filter[1].Coef[3] = -1.9, p->Channel[0].Filter[1].Coef[4] = 1.0), APOLLO_CORR_ERR_UNSTABLE,
        "complex poles on the unit circle");
    MUT((p->Channel[0].Filter[1].Coef[3] = -1.9, p->Channel[0].Filter[1].Coef[4] = 1.0 - 1e-10), APOLLO_CORR_ERR_UNSTABLE,
        "poles at radius 1 - 5e-11 (inside 1 - 1e-9 only)");
    MUT((p->Channel[0].Filter[1].Coef[3] = -0.9999999995, p->Channel[0].Filter[1].Coef[4] = 0.0), APOLLO_CORR_ERR_UNSTABLE,
        "a real pole at 0.9999999995");
    MUT((p->Channel[1].Filter[0].Coef[3] = 0.5, p->Channel[1].Filter[0].Coef[4] = -1.6), APOLLO_CORR_ERR_UNSTABLE,
        "a real pole beyond -1");
    MUT((p->Channel[1].Filter[0].Enabled = 0, p->Channel[1].Filter[0].Coef[4] = 1.2), APOLLO_CORR_ERR_UNSTABLE,
        "an unstable disabled filter (validate() checks every listed filter)");
    MUT(p->Preamp = 1.1, APOLLO_CORR_ERR_GAIN, "preamp above 0 dB");
    MUT(p->Preamp = 0.0, APOLLO_CORR_ERR_GAIN, "preamp of zero");
    MUT(p->Preamp = 0.0005, APOLLO_CORR_ERR_GAIN, "preamp below -60 dB");
    MUT(p->Channel[1].Gain = 1.0001, APOLLO_CORR_ERR_GAIN, "a trim above 0 dB");
    MUT(p->Channel[0].Gain = -0.5, APOLLO_CORR_ERR_GAIN, "a negative trim");
    MUT(p->Channel[1].DelayUs = APOLLO_CORR_MAX_DELAY_US + 1, APOLLO_CORR_ERR_DELAY, "delay over 10 ms");
    MUT(p->Channel[1].Slot = 64, APOLLO_CORR_ERR_SLOT, "slot 64");
    MUT(p->Channel[1].Slot = 34, APOLLO_CORR_ERR_SLOT, "slot 34 on a unit with 34 playback slots");
    MUT(p->Channel[1].Slot = p->Channel[0].Slot, APOLLO_CORR_ERR_SLOT, "two channels on one slot");
    MUT(p->Preamp = 1.0, APOLLO_CORR_ERR_CLIP, "studio without its -2.5 dB preamp (+2.5 dB peak would clip)");
    MUT(p->Preamp = pow(10.0, -2.4 / 20.0), APOLLO_CORR_ERR_CLIP, "preamp -2.4 dB for a +2.5 dB peak");
    MUT(memset(p->Name, 'x', sizeof(p->Name)), APOLLO_CORR_ERR_TEXT, "a name with no terminator");
    MUT(memset(p->Channel[1].Id, 'R', sizeof(p->Channel[1].Id)), APOLLO_CORR_ERR_TEXT, "a channel id with no terminator");
    {
        /* +12 dB below 5 Hz: a low shelf at 1 Hz, 1 dB of preamp covers the grid but not DC */
        double c[5];

        DesignLowShelf(c, 1.0, 12.0, 0.7071, 48000.0);
        MUT(memcpy(p->Channel[1].Filter[0].Coef, c, sizeof(c)), APOLLO_CORR_ERR_DC,
            "a +12 dB shelf at 1 Hz on R (0.03 dB at 5 Hz, inside validate()'s grid; +7 dB at DC after preamp and trim)");
    }
    /* F1 (review): what the grid alone let through, and the format's narrowest boost that must pass */
    {
        double fg = CORR_GRID_FMIN, prev = fg, f0, c[5];
        unsigned long err2;

        while (fg <= 10000.0) {
            prev = fg;
            fg *= CORR_GRID_RATIO;
        }
        f0 = sqrt(prev * fg);                   /* geometric middle of two grid points near 10 kHz */
        {
            static const struct { double q, g; unsigned long want; const char *what; } gap[] = {
                { 1000.0, 40.0, APOLLO_CORR_ERR_NARROW, "Q 1000, +40 dB (pole Q 10000)" },
                { 3000.0, 50.0, APOLLO_CORR_ERR_NARROW, "Q 3000, +50 dB (rings for about 2 s)" },
                { 50.0, 30.0, APOLLO_CORR_ERR_CLIP, "q 50, +30 dB: the format's narrowest (pole Q 281), only the peak measure catches it" },
            };
            unsigned gi;

            for (gi = 0; gi < sizeof(gap) / sizeof(gap[0]); gi++) {
                double grid = 0, fr, pre, truth;

                DesignPeaking(c, f0, gap[gi].g, gap[gi].q, 48000.0);
                for (fr = CORR_GRID_FMIN; fr <= 24000.0 * 0.999; fr *= CORR_GRID_RATIO) {
                    double m = CorrMag2At(c, fr, 48000.0);

                    if (m > grid) {
                        grid = m;
                    }
                }
                pre = sqrt(CORR_CLIP_LIN2 / grid);          /* the largest preamp the grid alone accepted */
                if (pre > 1.0) {
                    pre = 1.0;
                }
                truth = 10 * log10(CorrMag2At(c, f0, 48000.0) * pre * pre);
                OneFilter(p, c, pre);
                err2 = CorrValidate(p, 34, &ch, &fl);
                CHECKF(err2 == gap[gi].want, "F1 refused: peak at %.2f Hz between grid points, %s, preamp %+.2f dB "
                       "(the grid saw %+.2f dB, true peak after preamp %+.2f dB): got %lu, want %lu", f0, gap[gi].what,
                       20 * log10(pre), 10 * log10(grid * pre * pre), truth, err2, gap[gi].want);
            }
        }
        {
            static const double sub[] = { 1.0, 2.5 };
            unsigned si;

            for (si = 0; si < 2; si++) {
                DesignPeaking(c, sub[si], 30.0, 50.0, 48000.0);
                OneFilter(p, c, 1.0);
                err2 = CorrValidate(p, 34, &ch, &fl);
                CHECKF(err2 == APOLLO_CORR_ERR_DC, "F1 refused: +30 dB at %.1f Hz, q 50, preamp 0 dB (a sub-sonic boost; "
                       "the grid starts at 5 Hz): got %lu", sub[si], err2);
            }
        }
        DesignPeaking(c, 1000.0, 30.0, 50.0, 48000.0);
        OneFilter(p, c, pow(10.0, -30.0 / 20.0));
        err2 = CorrValidate(p, 34, &ch, &fl);
        CHECKF(err2 == 0, "accepted: the format's narrowest boost (q 50, +30 dB, 1 kHz) with a -30 dB preamp (got %lu)", err2);
        DesignPeaking(c, 0.49 * 48000.0, 30.0, 50.0, 48000.0);
        OneFilter(p, c, pow(10.0, -30.0 / 20.0));
        err2 = CorrValidate(p, 34, &ch, &fl);
        CHECKF(err2 == 0, "accepted: the same at 0.49 fs, where its digital poles are narrowest (got %lu)", err2);
        DesignPeaking(c, 1000.0, 30.0, 50.0, 48000.0);
        OneFilter(p, c, pow(10.0, -29.9 / 20.0));
        err2 = CorrValidate(p, 34, &ch, &fl);
        CHECKF(err2 == APOLLO_CORR_ERR_CLIP, "refused: the same with a -29.9 dB preamp (+0.1 dB) (got %lu)", err2);
        DesignPeaking(c, 1000.0, 30.2, 50.0, 48000.0);
        OneFilter(p, c, pow(10.0, -31.0 / 20.0));
        err2 = CorrValidate(p, 34, &ch, &fl);
        CHECKF(err2 == APOLLO_CORR_ERR_NARROW, "refused: q 50 at +30.2 dB (pole Q 284, past the format) (got %lu)", err2);
        {
            /* a real pole close to 1: a 6 dB/oct low-pass at 0.02 Hz is not a resonance and passes */
            double K = 1.0 / tan(CORR_PI * 0.02 / 48000.0), lp[5];

            lp[0] = lp[1] = 1.0 / (K + 1.0);
            lp[2] = 0.0;
            lp[3] = (1.0 - K) / (K + 1.0);
            lp[4] = 0.0;
            OneFilter(p, lp, 1.0);
            err2 = CorrValidate(p, 34, &ch, &fl);
            CHECKF(err2 == 0, "accepted: a first-order low-pass at 0.02 Hz (a real pole, no Q limit) (got %lu)", err2);
        }
    }
    /* the slot count may be unknown at the start (PrepareHardware): only the format's limit applies */
    *p = g_studio48;
    p->Channel[1].Slot = 40;
    CHECKF(CorrValidate(p, 0, &ch, &fl) == 0, "slot 40 passes while the unit's slot count is unknown");
    free(p);
#undef MUT
}

static void OneFilter(APOLLO_CORR_PROFILE *P, const double *C, double Pre)
{
    *P = g_studio48;
    P->ChannelCount = 1;
    P->Preamp = Pre;
    P->PreampDb = 20.0 * log10(Pre);
    P->Channel[0].Gain = 1.0;
    P->Channel[0].GainDb = 0.0;
    P->Channel[0].DelayUs = 0;
    P->Channel[0].FilterCount = 1;
    memcpy(P->Channel[0].Filter[0].Coef, C, sizeof(P->Channel[0].Filter[0].Coef));
    P->Channel[0].Filter[0].Enabled = 1;
}

/* ------------------------------------------------------------------ */
/* 5b. A clear stays cleared (F2); another rate stops cleanly (F3)     */
/* ------------------------------------------------------------------ */

/* Ticks at Rate; returns 1 if W never rose (a fade out that is not undone). */
static int RunNoRise(SIM *S, CORR_ENGINE *E, unsigned Ticks, unsigned Rate)
{
    unsigned t, w = E->W;
    int ok = 1;

    for (t = 0; t < Ticks; t++) {
        SimTick(S, E, Rate);
        if (E->W > w) {
            ok = 0;
        }
        w = E->W;
    }
    return ok;
}

static void TestClearAndRate(void)
{
    SIM s;
    CORR_ENGINE *e = NewEngine();
    unsigned t;

    SimInit(&s);
    /* F2: clear, then enable during the fade out */
    Load(e, &g_studio48, 1);
    for (t = 0; t < 40; t++) SimTick(&s, e, 48000);
    Check(e->W == e->FadeLen, "F2 setup: corrected");
    CorrClear(e);
    Check(!e->Enabled && e->Unload && CorrRunning(e), "F2: a clear disables at once and fades out (running until W is 0)");
    Check(CorrSetEnabled(e, 1) == APOLLO_CORR_ERR_NO_PROFILE && !e->Enabled, "F2: an enable during the clear's fade is refused");
    Check(RunNoRise(&s, e, 40, 48000) && e->Cur < 0 && !CorrRunning(e), "F2: ... the fade out completes and the profile is gone");
    Check(CorrSetEnabled(e, 1) == APOLLO_CORR_ERR_NO_PROFILE, "F2: an enable after the clear is refused");
    /* F2: clear, then a load without enable during the fade out: loaded, not corrected */
    Load(e, &g_studio48, 1);
    for (t = 0; t < 40; t++) SimTick(&s, e, 48000);
    CorrClear(e);
    for (t = 0; t < 3; t++) SimTick(&s, e, 48000);
    Load(e, &g_max2048, 0);
    Check(RunNoRise(&s, e, 40, 48000), "F2: a load without enable during the clear's fade does not bring correction back");
    Check(e->Cur >= 0 && e->Prog[e->Cur].Ch[0].NSec == 20 && !e->Enabled && !CorrRunning(e) && e->W == 0,
          "F2: ... the new profile is loaded, off; the cleared one is gone");
    /* F2: clear, then a load with enable: the new one fades in */
    CorrSetEnabled(e, 1);
    for (t = 0; t < 40; t++) SimTick(&s, e, 48000);
    CorrClear(e);
    for (t = 0; t < 3; t++) SimTick(&s, e, 48000);
    Load(e, &g_studio48, 1);
    for (t = 0; t < 60; t++) SimTick(&s, e, 48000);
    Check(e->Enabled && e->W == e->FadeLen && e->Next < 0 && e->Prog[e->Cur].Ch[0].NSec == g_rec[0].k,
          "F2: a load with enable during the clear's fade: that profile, corrected");

    /* F3: the unit changes rate while corrected: everything stops cleanly, a disable completes */
    Check(SimTick(&s, e, 96000) == 0 && e->RateMismatch && e->W == 0 && !e->CurLive,
          "F3: another rate: flagged, stopped (no frame touched, voices cold)");
    CorrSetEnabled(e, 0);
    SimTick(&s, e, 96000);
    Check(!CorrRunning(e), "F3: a disable at the other rate completes (the fade machine does not freeze)");
    /* a swap and a clear at the other rate complete too */
    CorrSetEnabled(e, 1);
    Load(e, &g_max2048, 1);
    SimTick(&s, e, 96000);
    Check(e->Next < 0 && e->Pending < 0 && e->Prog[e->Cur].Ch[0].NSec == 20, "F3: a load at the other rate takes over");
    CorrClear(e);
    SimTick(&s, e, 96000);
    Check(e->Cur < 0 && !CorrRunning(e), "F3: a clear at the other rate completes");
    /* back at the profile's rate: a fresh fade in */
    Load(e, &g_studio48, 1);
    SimTick(&s, e, 96000);
    SimTick(&s, e, 48000);
    SimTick(&s, e, 48000);
    Check(!e->RateMismatch && e->W > 0 && e->W < e->FadeLen, "F3: the rate matches again: it fades in from the start");
    free(s.ring);
    free(e);
}

/* ------------------------------------------------------------------ */
/* 5. The pump                                                         */
/* ------------------------------------------------------------------ */

static void TestPump(void)
{
    SIM s;
    CORR_ENGINE *e = NewEngine();
    unsigned n, t;
    unsigned long long h0;

    SimInit(&s);
    Load(e, &g_studio48, 1);
    n = SimTick(&s, e, 48000);
    Check(n == 0 && e->HeadValid && e->Head == s.head, "start: the first pump takes the writers' head and corrects from there");
    n = SimTick(&s, e, 48000);
    Check(n == T_TICK && e->Head == s.head, "next tick: one tick of frames (48) corrected, up to the head");
    /* a late tick: the hardware moved 600 frames */
    s.pos += 600;
    n = SimTick(&s, e, 48000);
    Check(e->Late > 0 && e->Head == s.head, "a late tick: frames the hardware already fetched are counted late and skipped");
    /* the aggregate's bound for a follower */
    for (t = 0; t < 3; t++) {
        SimTick(&s, e, 48000);
    }
    h0 = e->Head;
    CorrAggBound(e, h0 + 20, s.ticks);
    n = SimTick(&s, e, 48000);
    Check(n == 20 && e->Head == h0 + 20, "a follower: corrected only up to where the aggregate wrote it");
    CorrAggBound(e, e->Head + 10, s.ticks);
    CHECKF(CorrWriteFloor(e, e->Head - 30) == e->Head, "the aggregate's (re)start floor is the corrected head");
    for (t = 0; t < CORR_AGG_STALE + 2; t++) {
        SimTick(&s, e, 48000);
    }
    Check(e->Head == s.head, "an aggregate bound older than 8 ticks is ignored");
    CorrAggBound(e, 0, s.ticks);
    /* a new run of the engine (EngStarts) renumbers frames: start over */
    s.starts++;
    s.pos = 5000;
    s.head = s.pos + T_LEAD;
    n = SimTick(&s, e, 48000);
    Check(n == 0 && e->Head == s.head && e->W == 0 && !e->CurLive, "a new engine run: the head starts over, the voices too (fade in)");
    n = SimTick(&s, e, 48000);
    Check(n == T_TICK, "... and continues");
    /* the run cap */
    s.head += 3000;
    n = CorrPump(e, s.ring, T_FRAMES, T_STRIDE, 48000, s.pos, s.head, s.starts, s.ticks);
    Check(n == CORR_MAX_RUN && e->Head == s.head, "a backlog beyond 1024 frames: the newest 1024 are corrected, no more");
    free(s.ring);
    free(e);
}

/* ------------------------------------------------------------------ */
/* 6. Cost                                                             */
/* ------------------------------------------------------------------ */

static int CmpU(const void *a, const void *b)
{
    unsigned x = *(const unsigned *)a, y = *(const unsigned *)b;

    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Swaps: keep a swap running and time only the ticks with two voices (Cur and Next) */
static void Bench(const char *What, CORR_ENGINE *E, SIM *S, unsigned Ticks, int Swaps, double *Mean, unsigned *P99)
{
    LARGE_INTEGER f, a, b;
    unsigned *ns = (unsigned *)malloc(sizeof(unsigned) * Ticks), t, sum = 0, k = 0;
    double tot = 0;

    QueryPerformanceFrequency(&f);
    for (t = 0; t < Ticks; t++) {
        unsigned long long to = S->pos + T_LEAD, fr;
        unsigned c;

        for (fr = S->head; fr < to; fr++) {
            for (c = 0; c < 2; c++) {
                S->ring[(size_t)(fr & (T_FRAMES - 1)) * T_STRIDE + c] = Source(c, fr);
            }
        }
        S->head = to;
        S->ticks++;
        if (Swaps && E->Next < 0 && E->Pending < 0) {
            Load(E, &g_max2048, 1);    /* swap again as soon as one ends: two 20-filter voices */
        }
        {
            int two = E->Next >= 0 && E->NextLive;
            unsigned d;

            QueryPerformanceCounter(&a);
            sum += CorrPump(E, S->ring, T_FRAMES, T_STRIDE, 48000, S->pos, S->head, S->starts, S->ticks);
            QueryPerformanceCounter(&b);
            d = (unsigned)((b.QuadPart - a.QuadPart) * 1000000000LL / f.QuadPart);
            if (!Swaps || two) {
                ns[k++] = d;
                tot += d;
            }
        }
        S->pos += T_TICK;
    }
    if (k == 0) {
        k = 1;
        ns[0] = 0;
    }
    qsort(ns, k, sizeof(unsigned), CmpU);
    *Mean = tot / k;
    *P99 = ns[k * 99 / 100];
    printf("      %-46s mean %6.2f us  median %6.2f  p99 %6.2f  max %7.2f  (%u ticks timed)\n", What, *Mean / 1000.0,
           ns[k / 2] / 1000.0, *P99 / 1000.0, ns[k - 1] / 1000.0, k);
    (void)sum;
    free(ns);
}

static void TestCost(void)
{
    SIM s;
    CORR_ENGINE *e = NewEngine();
    double mean;
    unsigned p99;

    printf("      cost per 1 ms tick, 20 filters x 2 channels x 48 frames, 64-bit TDF2 (this CPU, user mode):\n");
    SimInit(&s);
    Load(e, &g_max2048, 1);
    Bench("fading in, then corrected (steady)", e, &s, 20000, 0, &mean, &p99);
    CHECKF(mean < 50000.0, "steady cost %.2f us per tick (bound 50 us)", mean / 1000.0);
    CorrSetBypass(e, 1);
    Bench("bypassed (A/B: filters warm, ring untouched)", e, &s, 5000, 0, &mean, &p99);
    CorrSetBypass(e, 0);
    Bench("during a profile swap (two voices, 4 channels)", e, &s, 20000, 1, &mean, &p99);
    CHECKF(mean < 100000.0, "swap cost %.2f us per tick (bound 100 us)", mean / 1000.0);
    CorrSetEnabled(e, 0);
    Bench("disabled (faded out: nothing runs)", e, &s, 5000, 0, &mean, &p99);
    free(s.ring);
    free(e);
}

int main(int argc, char **argv)
{
    char exe[MAX_PATH], *slash;

    if (argc > 1) {
        strcpy_s(g_ref, sizeof(g_ref), argv[1]);
    } else {
        GetModuleFileNameA(NULL, exe, sizeof(exe));
        slash = strrchr(exe, '\\');
        if (slash != NULL) {
            *slash = 0;
        }
        _snprintf_s(g_ref, sizeof(g_ref), _TRUNCATE, "%s\\ref", exe);
    }
    if (!LoadRef() || g_nrec != 8) {
        printf("FAIL  reference data (%u records)\n", g_nrec);
        return 1;
    }
    /* studio at 48 kHz on slots 0/1: preamp -2.5 dB, R trim -0.8 dB, R delay 0.42 ms (studio.json) */
    RecsToProfile(&g_studio48, &g_rec[0], &g_rec[1], 0, 1, -2.5);
    RecsToProfile(&g_max2048, &g_rec[4], &g_rec[5], 0, 1, 0.0);
    RecsToProfile(&g_studio192, &g_rec[2], &g_rec[3], 0, 1, -2.5);

    printf("== engine against Avium Measure, analytic responses, headroom\n");
    TestReference();
    printf("== off and bypass are bit-identical\n");
    TestBypass();
    TestLockstep();
    printf("== crossfades\n");
    TestCrossfades();
    printf("== the driver's check\n");
    TestValidate();
    printf("== the pump\n");
    TestPump();
    printf("== clear and rate (review F2, F3)\n");
    TestClearAndRate();
    printf("== cost\n");
    TestCost();
    printf("corr-test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
