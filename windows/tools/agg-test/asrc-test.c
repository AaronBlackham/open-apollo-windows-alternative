/*
 * asrc-test.c - offline checks of driver/apollo_agg.h (gate G6, docs/spec-twinx.md 6.3-6.6): the
 * driver's own code, compiled in user mode.
 *
 *   1. the CRT-free math (sin, sqrt, I0) against the CRT;
 *   2. the resampler's filter: passband flatness per fractional position for h = 8/16/32, and a
 *      time-domain THD+N of sines through a drifting fractional delay (ratio 1 + 4.74 ppm, the
 *      measured x8p/Twin X drift), on 24-bit samples the way the tick processes them;
 *   3. the clock loop: synthetic traces (ppm offsets, read noise, late stamps, a position jump,
 *      an out-of-range ratio) and, given a CSV from agg-drift (--trace file), the real one;
 *   4. configuration checks and the layout compile (x8p + Twin X descriptors, routes, latency).
 *
 *   asrc-test [--trace drift.csv] [--qpcfreq 10000000]
 * Exit code 0 = every check passed.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "..\..\include\apollo_ioctl.h"
#include "..\..\driver\apollo_agg.h"

static int g_fail, g_pass;

static void Check(int ok, const char *what)
{
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        printf("FAIL: %s\n", what);
    }
}

/* deterministic gaussian noise */
static unsigned long long g_rng = 0x9E3779B97F4A7C15ull;
static double Uniform(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return ((g_rng >> 11) + 0.5) / 9007199254740992.0;
}
static double Gauss(void)
{
    double u = Uniform(), v = Uniform();
    return sqrt(-2.0 * log(u)) * cos(2.0 * 3.14159265358979323846 * v);
}

/* ---------------------------------------------------------------- 1 */
static void TestMath(void)
{
    double x, worst = 0;
    for (x = -50.0; x <= 50.0; x += 0.001) {
        double e = fabs(ApolloAggSin(x) - sin(x));
        if (e > worst) worst = e;
    }
    printf("math: sin max error %.3g over [-50, 50]\n", worst);
    Check(worst < 1e-12, "ApolloAggSin matches sin");
    worst = 0;
    for (x = 0; x <= 4.0; x += 0.0001) {
        double e = fabs(ApolloAggSqrt(x) - sqrt(x));
        if (e > worst) worst = e;
    }
    Check(worst < 1e-14, "ApolloAggSqrt matches sqrt");
    /* I0(1) = 1.2660658777520082, I0(8) = 427.56411572180474 */
    Check(fabs(ApolloAggI0(1.0) - 1.2660658777520082) < 1e-13, "I0(1)");
    Check(fabs(ApolloAggI0(8.0) / 427.56411572180474 - 1) < 1e-13, "I0(8)");
    Check(ApolloAggFloor(-0.5) == -1 && ApolloAggFloor(2.0) == 2 && ApolloAggFloor(2.9) == 2, "floor");
}

/* ---------------------------------------------------------------- 2 */
static APOLLO_ASRC_TABLE g_tab;

/* worst |H(f) - ideal delay| in dB over 0..fmax for every fractional position (row-interpolated) */
static double Flatness(double FmaxHz, double *WorstFrac)
{
    double worst = 0;
    int k;
    unsigned long h = g_tab.Half, j;
    float w[2 * APOLLO_ASRC_MAX_HALF];

    for (k = 0; k < 512; k++) {
        double frac = (k + 0.37) / 512.0, f;
        ApolloAsrcWeights(&g_tab, frac, w);
        for (f = 0; f <= FmaxHz; f += 250.0) {
            double om = 2 * 3.14159265358979323846 * f / 48000.0, re = 0, im = 0, mag;
            for (j = 0; j < 2 * h; j++) {
                double t = (double)((long)j - (long)h + 1) - frac;   /* tap position relative to the output */
                re += w[j] * cos(om * t);
                im += w[j] * sin(om * t);
            }
            mag = sqrt(re * re + im * im);
            if (fabs(20 * log10(mag)) > worst) {
                worst = fabs(20 * log10(mag));
                *WorstFrac = frac;
            }
        }
    }
    return worst;
}

/*
 * A sine through the moving fractional delay: input x[n] = A sin(w n) (24-bit samples), output at
 * positions z(m) = z0 + r m. Error against the exact A sin(w z(m)), as a level below the tone.
 */
static double SineThdN(double FreqHz, double Ratio, double Amp)
{
    static long in[200000];
    const double w = 2 * 3.14159265358979323846 * FreqHz / 48000.0;
    unsigned long h = g_tab.Half, j;
    long n;
    double err = 0, sig = 0;
    int m, count = 0;
    float wt[2 * APOLLO_ASRC_MAX_HALF];

    for (n = 0; n < 200000; n++) {
        double v = Amp * sin(w * n);
        in[n] = (long)floor(v * 8388607.0 + 0.5) * 256;     /* 24 bits at the top, as the ring */
    }
    for (m = 0; m < 150000; m++) {
        double z = 1000.123 + Ratio * m;
        long long k = ApolloAggFloor(z);
        float acc = 0;
        double ideal, got;

        ApolloAsrcWeights(&g_tab, z - (double)k, wt);
        for (j = 0; j < 2 * h; j++) {
            acc += wt[j] * (float)in[k - (long long)h + 1 + (long long)j];
        }
        got = (double)(ApolloAsrcToSample(acc) & ~0xFFL) / 2147483648.0;  /* the DAC keeps 24 bits */
        ideal = Amp * sin(w * z);
        err += (got - ideal) * (got - ideal);
        sig += ideal * ideal;
        count++;
    }
    return 10 * log10(err / sig);
}

static void TestFilter(void)
{
    static const unsigned long halves[3] = { 8, 16, 32 };
    int i;

    for (i = 0; i < 3; i++) {
        double wf = 0, flat20, flat18, flat10, t1, t10, t18, t20, beta, fc;

        ApolloAsrcParams(halves[i], &beta, &fc);
        ApolloAsrcBuild(&g_tab, halves[i]);
        Check(g_tab.Half == halves[i], "table built");
        flat20 = Flatness(20000, &wf);
        flat18 = Flatness(18000, &wf);
        flat10 = Flatness(10000, &wf);
        t1 = SineThdN(1000, 1 + 4.74e-6, 0.5);
        t10 = SineThdN(10000, 1 + 4.74e-6, 0.5);
        t18 = SineThdN(18000, 1 + 4.74e-6, 0.5);
        t20 = SineThdN(20000, 1 + 4.74e-6, 0.5);
        printf("asrc h=%2lu (beta %.1f, cutoff %.2f, %d phases): flat within %.4f dB to 10 kHz, %.4f dB to 18 kHz, "
               "%.4f dB to 20 kHz; error vs exact (-6 dBFS sine): 1 kHz %.1f dB, 10 kHz %.1f dB, 18 kHz %.1f dB, "
               "20 kHz %.1f dB\n", halves[i], beta, fc, APOLLO_ASRC_PHASES, flat10, flat18, flat20, t1, t10, t18, t20);
        if (halves[i] == 16) {
            Check(flat18 < 0.01 && flat20 < 0.05, "h=16 flat within 0.01 dB to 18 kHz, 0.05 dB to 20 kHz");
            Check(t1 < -100.0 && t10 < -100.0, "h=16: 1 and 10 kHz at least 100 dB clean");
        }
        if (halves[i] == 32) {
            Check(flat20 < 0.01, "h=32 flat within 0.01 dB to 20 kHz");
            Check(t1 < -105.0 && t20 < -100.0, "h=32: at least 105 dB clean at 1 kHz, 100 dB at 20 kHz");
        }
        Check(t1 < -80.0, "1 kHz at least 80 dB clean");
    }
    /* DC gain of every row is 1 */
    ApolloAsrcBuild(&g_tab, 16);
    {
        int k;
        double worst = 0;
        float w[32];
        unsigned long j;
        for (k = 0; k < 1000; k++) {
            double s = 0;
            ApolloAsrcWeights(&g_tab, k / 1000.0, w);
            for (j = 0; j < 32; j++) s += w[j];
            if (fabs(s - 1) > worst) worst = fabs(s - 1);
        }
        Check(worst < 1e-5, "DC gain 1 at every position");
    }
    /* saturation */
    Check(ApolloAsrcToSample(3e9f) == 0x7FFFFF00L && ApolloAsrcToSample(-3e9f) == (long)0x80000000UL, "saturates");
}

/* --sweep: the Kaiser beta and cutoff choice (not part of the checks) */
static void Sweep(void)
{
    static const unsigned long halves[2] = { 16, 32 };
    static const double betas[5] = { 6, 7, 8, 9, 10 }, fcs[4] = { 0.90, 0.94, 0.97, 1.0 };
    int a, b, c;
    double wf;

    for (a = 0; a < 2; a++) {
        for (b = 0; b < 5; b++) {
            for (c = 0; c < 4; c++) {
                ApolloAsrcBuildEx(&g_tab, halves[a], betas[b], fcs[c]);
                printf("h=%2lu beta %4.1f fc %.2f: flat 10k %.4f 18k %.4f 20k %.4f dB | err 1k %6.1f 10k %6.1f 18k %6.1f 20k %6.1f\n",
                       halves[a], betas[b], fcs[c], Flatness(10000, &wf), Flatness(18000, &wf), Flatness(20000, &wf),
                       SineThdN(1000, 1 + 4.74e-6, 0.5), SineThdN(10000, 1 + 4.74e-6, 0.5),
                       SineThdN(18000, 1 + 4.74e-6, 0.5), SineThdN(20000, 1 + 4.74e-6, 0.5));
            }
        }
    }
}

/* ---------------------------------------------------------------- 3 */
typedef struct {
    double lockS;           /* first lock, seconds; -1 = never */
    double maxAbsE;         /* largest |EFilt| after the lock */
    double ppmErr;          /* (R - 1) - true, ppm, at the end */
    unsigned long resyncs, outliers;
    int finalState;
} LOOPRES;

/* Simulate: true ratio, read noise (frames rms), late stamps (probability, size), a jump at JumpS */
static LOOPRES Simulate(double TruePpm, double Noise, double LateP, double LateFrames, double JumpS, double JumpFrames,
                        double Seconds)
{
    APOLLO_AGG_CLOCK c;
    LOOPRES r;
    long long pm = 1000000;
    double pf = 5555555.25, ratio = 1 + TruePpm * 1e-6;
    int t, ticks = (int)(Seconds * 1000), jumped = 0;

    memset(&c, 0, sizeof(c));
    memset(&r, 0, sizeof(r));
    r.lockS = -1;
    ApolloAggClockForm(&c, pm, pf, 1.0);
    for (t = 0; t < ticks; t++) {
        double meas;
        int ev;
        pm += 48;
        pf += 48 * ratio;
        if (!jumped && JumpS > 0 && t >= JumpS * 1000) {
            pf += JumpFrames;
            jumped = 1;
        }
        meas = pf + Noise * Gauss();
        if (Uniform() < LateP) meas += LateFrames;
        ev = ApolloAggClockUpdate(&c, pm, meas);
        if (ev == APOLLO_AGG_EV_LOCKED && r.lockS < 0) r.lockS = t / 1000.0;
        if (c.State == APOLLO_AGG_CLOCK_LOCKED && t > 5000 && fabs(c.EFilt) > r.maxAbsE) r.maxAbsE = fabs(c.EFilt);
    }
    r.ppmErr = (c.Ri - 1) * 1e6 - TruePpm;     /* the integrator: R adds the (filtered) proportional term */
    r.resyncs = c.Resyncs;
    r.outliers = c.Outliers;
    r.finalState = c.State;
    return r;
}

static void TestLoop(void)
{
    LOOPRES r;

    r = Simulate(4.74, 0.3, 0.0, 0, 0, 0, 60);
    printf("loop +4.74 ppm, 0.3 frame noise: lock %.2f s, |e| after 5 s <= %.3f, ratio error %.4f ppm, %lu resyncs\n",
           r.lockS, r.maxAbsE, r.ppmErr, r.resyncs);
    Check(r.lockS >= 0 && r.lockS < 3.0, "locks within 3 s");
    Check(r.maxAbsE < 0.5, "stays within 0.5 frame");
    Check(fabs(r.ppmErr) < 0.1, "ratio within 0.1 ppm");
    Check(r.resyncs == 0 && r.finalState == APOLLO_AGG_CLOCK_LOCKED, "no resync, locked");

    r = Simulate(-50, 0.5, 0.002, 8.0, 0, 0, 60);
    printf("loop -50 ppm, 0.5 noise, 0.2%% stamps 8 frames late: lock %.2f s, |e| <= %.3f, ratio error %.4f ppm, "
           "%lu outliers, %lu resyncs\n", r.lockS, r.maxAbsE, r.ppmErr, r.outliers, r.resyncs);
    Check(r.lockS >= 0 && r.lockS < 3.0 && r.maxAbsE < 0.5 && fabs(r.ppmErr) < 0.2 && r.resyncs == 0,
          "late stamps are absorbed");

    r = Simulate(4.74, 0.3, 0, 0, 20.0, 100.0, 40);
    printf("loop with a 100-frame jump at 20 s: %lu resync(s), final state %d, ratio error %.4f ppm\n", r.resyncs,
           r.finalState, r.ppmErr);
    Check(r.resyncs == 1 && r.finalState == APOLLO_AGG_CLOCK_LOCKED && fabs(r.ppmErr) < 0.2, "a jump resyncs once, relocks");

    r = Simulate(900, 0.3, 0, 0, 0, 0, 20);
    printf("loop +900 ppm: final state %d (3 = degraded)\n", r.finalState);
    Check(r.finalState == APOLLO_AGG_CLOCK_DEGRADED, "a ratio outside 500 ppm is degraded");
}

/* The real trace: agg-drift --csv (unit 0 = x8p, 1 = Twin X), replayed as the driver does it. */
static void TestTrace(const char *Path, double QpcFreq)
{
    FILE *f = NULL;
    char line[256];
    static long long q[2][200000], p[2][200000];
    int n[2] = { 0, 0 }, i, j = -1, tl = 1, fo = 0, locks = 0;
    APOLLO_AGG_CLOCK c;
    double lockS = -1, maxE = 0, sumE2 = 0;
    int cnt = 0;

    if (fopen_s(&f, Path, "r") != 0 || f == NULL) {
        printf("trace: cannot read %s\n", Path);
        g_fail++;
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        int u;
        long long qq, pp;
        if (sscanf_s(line, "%d,%lld,%lld", &u, &qq, &pp) == 3 && u >= 0 && u < 2 && n[u] < 200000) {
            q[u][n[u]] = qq;
            p[u][n[u]] = pp;
            n[u]++;
        }
    }
    fclose(f);
    memset(&c, 0, sizeof(c));
    /* timeline = Twin X (the configured master), follower = x8p */
    for (i = 0; i < n[tl]; i++) {
        long long tm = q[tl][i], pm = p[tl][i];
        double pf;
        int ev;
        while (j + 1 < n[fo] && q[fo][j + 1] <= tm) j++;
        if (j < 0) continue;
        pf = (double)p[fo][j] + (double)(tm - q[fo][j]) * 48000.0 / QpcFreq;
        if (cnt == 0) {
            ApolloAggClockForm(&c, pm, pf, 1.0);
            cnt = 1;
            continue;
        }
        ev = ApolloAggClockUpdate(&c, pm, pf);
        if (ev == APOLLO_AGG_EV_LOCKED) {
            locks++;
            if (lockS < 0) lockS = c.Since;
        }
        if (c.State == APOLLO_AGG_CLOCK_LOCKED && c.Since > 5) {
            if (fabs(c.EFilt) > maxE) maxE = fabs(c.EFilt);
            sumE2 += c.EFilt * c.EFilt;
            cnt++;
        }
    }
    printf("trace %s: %d timeline ticks, lock at %.2f s, ratio x8p/TwinX - 1 = %+.3f ppm, |e| after 5 s max %.3f "
           "rms %.3f frames, %lu resyncs, %lu outliers, final state %d\n", Path, n[tl], lockS, (c.R - 1) * 1e6, maxE,
           cnt > 1 ? sqrt(sumE2 / (cnt - 1)) : 0.0, c.Resyncs, c.Outliers, c.State);
    Check(lockS >= 0 && lockS < 3.0, "real trace: locks within 3 s");
    Check(c.Resyncs == 0 && c.State == APOLLO_AGG_CLOCK_LOCKED, "real trace: no resync");
    Check(maxE < 0.5, "real trace: phase within 0.5 frame");
    Check(fabs((c.R - 1) * 1e6 - 4.74) < 1.0, "real trace: ratio near the measured +4.74 ppm");
}

/* ---------------------------------------------------------------- 4 */
static const unsigned short kX8pIn[32] = {
    0x0001, 0x0002, 0x0103, 0x0104, 0x0305, 0x0306, 0x0307, 0x0308, 0x0401, 0x0402, 0x0403, 0x0404, 0x0405, 0x0406,
    0x0407, 0x0408, 0x0501, 0x0502, 0x0701, 0x0702, 0x0703, 0x0704, 0x0705, 0x0706, 0x0707, 0x0708, 0x0901, 0x0902,
    0x0201, 0x0202, 0x0203, 0x0204 };
static const unsigned short kX8pOut[34] = {
    0x0901, 0x0902, 0x0301, 0x0302, 0x0303, 0x0304, 0x0305, 0x0306, 0x0307, 0x0308, 0x0401, 0x0402, 0x0403, 0x0404,
    0x0405, 0x0406, 0x0407, 0x0408, 0x0501, 0x0502, 0x0701, 0x0702, 0x0703, 0x0704, 0x0705, 0x0706, 0x0707, 0x0708,
    0x0A01, 0x0A02, 0x0A03, 0x0A04, 0x0A05, 0x0A06 };
/* the Twin X Gen 1 descriptors (docs/spec-twinx.md 6.9.3) */
static const unsigned short kTwxIn[34] = {
    0x0001, 0x0002, 0x0103, 0x0104, 0x0305, 0x0306, 0x0307, 0x0308, 0x0401, 0x0402, 0x0403, 0x0404, 0x0405, 0x0406,
    0x0407, 0x0408, 0x0501, 0x0502, 0x0701, 0x0702, 0x0703, 0x0704, 0x0705, 0x0706, 0x0707, 0x0708, 0x0901, 0x0902,
    0x0201, 0x0202, 0x0203, 0x0204, 0x0B01, 0x0B02 };
static const unsigned short kTwxOut[46] = {
    0x0901, 0x0902, 0x0301, 0x0302, 0x0303, 0x0304, 0x0305, 0x0306, 0x0307, 0x0308, 0x0401, 0x0402, 0x0403, 0x0404,
    0x0405, 0x0406, 0x0407, 0x0408, 0x0501, 0x0502, 0x0701, 0x0702, 0x0703, 0x0704, 0x0705, 0x0706, 0x0707, 0x0708,
    0x0A01, 0x0A02, 0x0A03, 0x0A04, 0x0A05, 0x0A06, 0x0A07, 0x0A08, 0x0C01, 0x0C02, 0x0C03, 0x0C04, 0x0C05, 0x0C06,
    0x0C07, 0x0C08, 0x0C09, 0x0C0A };

static APOLLO_AGG_CONFIG g_cfg;
static APOLLO_AGG_COMPILED g_lay;

static void BaseConfig(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.Version = APOLLO_AGG_VERSION;
    g_cfg.Bytes = sizeof(g_cfg);
    g_cfg.Flags = APOLLO_AGG_CFG_ENABLED | APOLLO_AGG_CFG_ALIGN;
    g_cfg.UnitCount = 2;
    g_cfg.Master = 1;
    g_cfg.AsrcHalf = 16;
    g_cfg.Unit[0].Uid = 0x3A00C0DE;
    g_cfg.Unit[0].Model = APOLLO_INFO_MODEL_X8P;
    g_cfg.Unit[0].InCount = 32;
    g_cfg.Unit[0].OutCount = 34;
    memcpy(g_cfg.Unit[0].InIds, kX8pIn, sizeof(kX8pIn));
    memcpy(g_cfg.Unit[0].OutIds, kX8pOut, sizeof(kX8pOut));
    g_cfg.Unit[1].Uid = 0x2F00BEEF;
    g_cfg.Unit[1].Model = APOLLO_INFO_MODEL_TWINX;
    g_cfg.Unit[1].InCount = 34;
    g_cfg.Unit[1].OutCount = 46;
    memcpy(g_cfg.Unit[1].InIds, kTwxIn, sizeof(kTwxIn));
    memcpy(g_cfg.Unit[1].OutIds, kTwxOut, sizeof(kTwxOut));
}

static void TestLayout(void)
{
    APOLLO_AGG_UNIT_RT rt[4];
    unsigned long i;

    memset(rt, 0, sizeof(rt));
    rt[0].Present = rt[1].Present = 1;
    rt[0].Guard = rt[1].Guard = 16;
    rt[0].Lead = rt[1].Lead = 144;
    rt[0].FrameCfg = rt[1].FrameCfg = 8;
    rt[0].Adc = 10;
    rt[1].Adc = 5;

    BaseConfig();
    Check(ApolloAggValidate(&g_cfg) == 0, "valid config");
    ApolloAggCompile(&g_cfg, rt, 1, &g_lay);
    printf("layout x8p + TwinX (simple, timeline TwinX): %lu in, %lu out, latency in %lu out %lu (pad %lu); "
           "in[0] \"%s\", in[32] \"%s\", out[34] \"%s\", out[%lu] \"%s\"\n", g_lay.InCount, g_lay.OutCount,
           g_lay.InLatency, g_lay.OutLatency, g_lay.Pad, g_lay.In[0].Name, g_lay.In[32].Name, g_lay.Out[34].Name,
           g_lay.OutCount - 1, g_lay.Out[g_lay.OutCount - 1].Name);
    Check(g_lay.InCount == 32 + 28 && g_lay.OutCount == 34 + 22, "block counts (Twin X non-jacks hidden)");
    Check(g_lay.Pad == 16 + 16 + 5 && g_lay.InLatency == 58 && g_lay.OutLatency == 152, "aligned latency 58 / 152");
    Check(strcmp(g_lay.In[0].Name, "x8p MIC/LINE 1") == 0 && strcmp(g_lay.In[7].Name, "x8p MIC/LINE 8") == 0 && strcmp(g_lay.In[32].Name, "TwinX MIC/LINE/HIZ 1") == 0,
          "names");
    Check(strcmp(g_lay.Out[34].Name, "TwinX MON L") == 0, "Twin X outputs start with MON L");
    Check((g_lay.In[0].Flags & APOLLO_AGG_CH_RESAMPLED) && (g_lay.In[32].Flags & APOLLO_AGG_CH_TIMELINE), "flags");
    Check(g_lay.In[0].LatencyFrames == 10 + 16 + 32 && g_lay.In[32].LatencyFrames == 5 + 16, "per-channel latency");
    for (i = 0; i < g_lay.OutCount; i++) {
        Check(g_lay.DestCount[i] == 1 && g_lay.Dest[i][0].Unit == g_lay.Out[i].Unit &&
              g_lay.Dest[i][0].Slot == g_lay.Out[i].Slot, "native destinations without routes");
    }

    /* Studio: DAW (outs 1-2) to the Twin X MON */
    g_cfg.RouteCount = 1;
    g_cfg.Route[0].SrcKind = APOLLO_AGG_SRC_DAW;
    g_cfg.Route[0].DstUnit = 1;
    g_cfg.Route[0].DstId[0] = 0x0901;
    g_cfg.Route[0].DstId[1] = 0x0902;
    ApolloAggCompile(&g_cfg, rt, 1, &g_lay);
    Check(g_lay.DestCount[0] == 1 && g_lay.Dest[0][0].Unit == 1 && g_lay.Dest[0][0].Slot == 0 &&
          g_lay.Dest[1][0].Unit == 1 && g_lay.Dest[1][0].Slot == 1 && (g_lay.Out[0].Flags & APOLLO_AGG_CH_ROUTED),
          "Studio: DAW 1-2 go to Twin X MON 0/1 instead of x8p MON");
    Check(strcmp(g_lay.Out[0].Name, "x8p MON L > TwinX") == 0, "Studio: the routed name says where it plays");
    Check(g_lay.DestCount[2] == 1 && g_lay.Dest[2][0].Unit == 0 && g_lay.Dest[2][0].Slot == 2, "other pairs keep theirs");
    /* Mirror: also to the x8p MON */
    g_cfg.RouteCount = 2;
    g_cfg.Route[1] = g_cfg.Route[0];
    g_cfg.Route[1].DstUnit = 0;
    ApolloAggCompile(&g_cfg, rt, 1, &g_lay);
    Check(g_lay.DestCount[0] == 2 && g_lay.Dest[0][1].Unit == 0 && g_lay.Dest[0][1].Slot == 0, "Mirror: two destinations");
    Check(strcmp(g_lay.Out[0].Name, "x8p MON L > TwinX+x8p") == 0, "Mirror: the name says both");
    /* a port route: Twin X MON (logical 34/35) to the Twin X headphones (CUE 1) */
    g_cfg.RouteCount = 1;
    g_cfg.Route[0].SrcKind = APOLLO_AGG_SRC_PORT;
    g_cfg.Route[0].SrcUnit = 1;
    g_cfg.Route[0].SrcId[0] = 0x0901;
    g_cfg.Route[0].SrcId[1] = 0x0902;
    g_cfg.Route[0].DstId[0] = 0x0A01;
    g_cfg.Route[0].DstId[1] = 0x0A02;
    ApolloAggCompile(&g_cfg, rt, 1, &g_lay);
    Check(g_lay.Dest[34][0].Unit == 1 && g_lay.Dest[34][0].Slot == 28 && g_lay.DestCount[0] == 1 &&
          g_lay.Dest[0][0].Unit == 0, "port route by channel ID");

    /* x8p master (timeline x8p): the Twin X is the follower; pad has no ADC excess */
    BaseConfig();
    g_cfg.Master = 0;
    ApolloAggCompile(&g_cfg, rt, 0, &g_lay);
    Check(g_lay.Pad == 32 && g_lay.InLatency == 16 + 32 + 10, "x8p timeline: pad 32, latency 58");
    /* Twin X absent: channel list unchanged, flags say so */
    rt[1].Present = 0;
    {
        APOLLO_AGG_COMPILED a;
        ApolloAggCompile(&g_cfg, rt, 0, &a);
        Check(ApolloAggSameLists(&a, &g_lay) && !(a.In[40].Flags & APOLLO_AGG_CH_PRESENT), "absent unit keeps its channels");
    }
    rt[1].Present = 1;
    /* explicit order with a hidden channel */
    BaseConfig();
    g_cfg.Flags |= APOLLO_AGG_CFG_EXPLICIT;
    g_cfg.InCount = 3;
    g_cfg.In[0].Unit = 1; g_cfg.In[0].Slot = 0;
    g_cfg.In[1].Unit = 0; g_cfg.In[1].Slot = 0; g_cfg.In[1].Flags = APOLLO_AGG_CHAN_HIDDEN;
    g_cfg.In[2].Unit = 0; g_cfg.In[2].Slot = 26;
    g_cfg.OutCount = 2;
    g_cfg.Out[0].Unit = 1; g_cfg.Out[0].Slot = 0;
    g_cfg.Out[1].Unit = 1; g_cfg.Out[1].Slot = 1;
    Check(ApolloAggValidate(&g_cfg) == 0, "explicit config valid");
    ApolloAggCompile(&g_cfg, rt, 1, &g_lay);
    Check(g_lay.InCount == 2 && strcmp(g_lay.In[0].Name, "TwinX MIC/LINE/HIZ 1") == 0 &&
          strcmp(g_lay.In[1].Name, "x8p MON L") == 0 && g_lay.OutCount == 2, "explicit order, hidden left out");

    /* validation */
    BaseConfig();
    g_cfg.Master = 2;
    Check(ApolloAggValidate(&g_cfg) == APOLLO_AGG_ERR_MASTER, "master out of range refused");
    BaseConfig();
    g_cfg.AsrcHalf = 12;
    Check(ApolloAggValidate(&g_cfg) == APOLLO_AGG_ERR_ASRC, "bad ASRC size refused");
    BaseConfig();
    g_cfg.Unit[1].Uid = g_cfg.Unit[0].Uid;
    Check(ApolloAggValidate(&g_cfg) == APOLLO_AGG_ERR_DUPLICATE, "duplicate unit refused");
    BaseConfig();
    g_cfg.Bytes = 12;
    Check(ApolloAggValidate(&g_cfg) == APOLLO_AGG_ERR_VERSION, "wrong size refused");
    BaseConfig();
    g_cfg.RouteCount = 1;
    g_cfg.Route[0].SrcKind = 9;
    Check(ApolloAggValidate(&g_cfg) == APOLLO_AGG_ERR_ROUTES, "bad route refused");
    BaseConfig();
    g_cfg.Flags |= APOLLO_AGG_CFG_EXPLICIT;
    g_cfg.InCount = 1;
    g_cfg.In[0].Unit = 3;
    Check(ApolloAggValidate(&g_cfg) == APOLLO_AGG_ERR_CHANNELS, "channel on a missing unit refused");
}

/*
 * [G7 review] The capture catch-up bound (apollo_agg.h APOLLO_AGG_MAX_LAG): after a stall, one tick
 * resamples at most this many frames under Agg.Lock and the members' EngineLocks, else it
 * re-originates. Worst case: h 32, all 128 logical inputs on a follower (weights once per frame,
 * then 2h MACs per channel). At 4096 that was ~34 M MACs in one DPC; it must stay a few ms at the
 * scalar rate, and still ride out an ordinary late tick (several ms) without a resync.
 */
static void TestLagBound(void)
{
    double macs = (double)APOLLO_AGG_MAX_LAG * (2.0 * APOLLO_ASRC_MAX_HALF + (double)APOLLO_AGG_MAX_CH * 2.0 * APOLLO_ASRC_MAX_HALF);
    double old = 4096.0 * (2.0 * APOLLO_ASRC_MAX_HALF + (double)APOLLO_AGG_MAX_CH * 2.0 * APOLLO_ASRC_MAX_HALF);

    printf("capture catch-up bound: %u frames, worst case %.1f M MACs per tick (was %.1f M at 4096)\n", APOLLO_AGG_MAX_LAG,
           macs / 1e6, old / 1e6);
    Check(APOLLO_AGG_MAX_LAG <= 1024, "catch-up bounded to 1024 frames (21 ms) per tick");
    Check(macs <= 9e6, "worst-case catch-up under 9 M MACs (h 32, 128 follower inputs)");
    Check(APOLLO_AGG_MAX_LAG >= 480, "an ordinary late tick (10 ms) still catches up instead of resyncing");
    Check(APOLLO_AGG_MAX_LAG <= 8192 / 4, "the bound stays well inside a unit's 8192-frame record ring");
}

int main(int argc, char **argv)
{
    const char *trace = NULL;
    double qf = 0;
    int sweep = 0;
    LARGE_INTEGER f;
    int i;

    QueryPerformanceFrequency(&f);
    qf = (double)f.QuadPart;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) trace = argv[++i];
        else if (strcmp(argv[i], "--qpcfreq") == 0 && i + 1 < argc) qf = atof(argv[++i]);
        else if (strcmp(argv[i], "--sweep") == 0) sweep = 1;
    }
    TestMath();
    TestFilter();
    TestLoop();
    if (sweep) {
        Sweep();
        return 0;
    }
    if (trace) TestTrace(trace, qf);
    TestLayout();
    TestLagBound();
    printf("asrc-test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
