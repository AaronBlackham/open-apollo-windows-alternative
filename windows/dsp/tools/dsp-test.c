/*
 * dsp-test.c - checks for avdsp: every processor against its own math.
 * Exit code 0 = all passed. Run: dsp\bin\dsp-test.exe
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#define _CRT_SECURE_NO_WARNINGS
#include "../avdsp.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define FS      48000.0f
#define NFS     48000

static int g_pass, g_fail;

static void check(int ok, const char *fmt, ...)
{
    va_list ap;

    printf("%s  ", ok ? "PASS" : "FAIL");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    if (ok) g_pass++; else g_fail++;
}

static unsigned g_rng = 12345u;
static float rnd(void)          /* uniform [-1, 1) */
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return (float)(g_rng >> 8) / 8388608.0f - 1.0f;
}

static float dbf(double lin) { return (float)(20.0 * log10(lin > 1e-12 ? lin : 1e-12)); }
static float amp(float db) { return powf(10.0f, db / 20.0f); }

static double rms(const float *x, int n)
{
    double s = 0;
    int i;
    for (i = 0; i < n; i++) s += (double)x[i] * x[i];
    return sqrt(s / (n > 0 ? n : 1));
}

static float peak(const float *x, int n)
{
    float p = 0;
    int i;
    for (i = 0; i < n; i++) if (fabsf(x[i]) > p) p = fabsf(x[i]);
    return p;
}

static float *alloc(int n) { return (float *)calloc((size_t)n, sizeof(float)); }

static void tone(float *x, int n, float hz, float a)
{
    int i;
    for (i = 0; i < n; i++) x[i] = a * (float)sin(2.0 * 3.14159265358979 * hz * i / NFS);
}

static void run(av_voice *v, float *x, int n)
{
    int off;
    for (off = 0; off < n; off += 480) av_voice_process(v, x + off, (n - off) < 480 ? (n - off) : 480);
}

static void voice_with(av_voice *v, const av_voice_params *p)
{
    av_voice_init(v, FS);
    av_voice_set(v, p);
}

/* ------------------------------------------------------------------ */

static void test_biquad(void)
{
    static const struct { int type; float hz, q, g; const char *name; } cases[] = {
        { AV_BQ_HIGHPASS, 100, 0.7071f, 0, "high-pass 100 Hz" },
        { AV_BQ_LOWPASS, 1000, 0.7071f, 0, "low-pass 1 kHz" },
        { AV_BQ_PEAK, 1000, 1.0f, 6, "peak 1 kHz +6 dB" },
        { AV_BQ_PEAK, 3000, 2.0f, -9, "peak 3 kHz -9 dB Q2" },
        { AV_BQ_LOWSHELF, 200, 0.7071f, 4, "low shelf 200 Hz +4 dB" },
        { AV_BQ_HIGHSHELF, 8000, 0.7071f, -6, "high shelf 8 kHz -6 dB" },
    };
    static const float freqs[] = { 50, 100, 200, 500, 1000, 2000, 5000, 10000, 16000 };
    const int n = NFS, win = 9600;          /* last 0.2 s: whole cycles at every test frequency */
    float *x = alloc(n), *y = alloc(n);
    size_t c, k;
    av_biquad f;

    for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        float worst = 0;
        av_biquad_design(&f, cases[c].type, FS, cases[c].hz, cases[c].q, cases[c].g);
        for (k = 0; k < sizeof(freqs) / sizeof(freqs[0]); k++) {
            int i;
            float meas, want, err;
            tone(x, n, freqs[k], 0.5f);
            av_biquad_reset(&f);
            for (i = 0; i < n; i++) y[i] = av_biquad_tick(&f, x[i]);
            meas = dbf(rms(y + n - win, win) / rms(x + n - win, win));
            want = av_biquad_response_db(&f, FS, freqs[k]);
            err = fabsf(meas - want);
            if (err > worst) worst = err;
        }
        check(worst < 0.05f, "%-24s measured response matches the design (worst %.4f dB)", cases[c].name, worst);
    }

    av_biquad_design(&f, AV_BQ_HIGHPASS, FS, 100, 0.7071068f, 0);
    check(fabsf(av_biquad_response_db(&f, FS, 100) + 3.0103f) < 0.05f, "Butterworth high-pass is -3.01 dB at its corner (%.3f)", av_biquad_response_db(&f, FS, 100));
    av_biquad_design(&f, AV_BQ_PEAK, FS, 1000, 1, 6);
    check(fabsf(av_biquad_response_db(&f, FS, 1000) - 6.0f) < 0.01f, "peak EQ is +6.00 dB at its centre (%.3f)", av_biquad_response_db(&f, FS, 1000));
    av_biquad_design(&f, AV_BQ_LOWSHELF, FS, 200, 0.7071f, 4);
    check(fabsf(av_biquad_response_db(&f, FS, 20) - 4.0f) < 0.25f, "low shelf reaches +4 dB at 20 Hz (%.3f)", av_biquad_response_db(&f, FS, 20));
    av_biquad_design(&f, AV_BQ_HIGHSHELF, FS, 8000, 0.7071f, -6);
    check(fabsf(av_biquad_response_db(&f, FS, 20000) + 6.0f) < 0.5f, "high shelf reaches -6 dB at 20 kHz (%.3f)", av_biquad_response_db(&f, FS, 20000));
    free(x); free(y);
}

static float comp_expect(float lvl, float T, float R, float W)
{
    float d = lvl - T;
    if (W > 0 && 2 * fabsf(d) <= W) return (1 / R - 1) * (d + W / 2) * (d + W / 2) / (2 * W);
    return d > 0 ? d * (1 / R - 1) : 0;
}

static void test_compressor(void)
{
    static const struct { float in, knee; int rms; } pts[] = {
        { -40, 0, 0 }, { -30, 0, 0 }, { -20, 0, 0 }, { -10, 0, 0 }, { -5, 0, 0 }, { 0, 0, 0 },
        { -25, 10, 0 }, { -20, 10, 0 }, { -15, 10, 0 }, { -6, 0, 1 }, { -12, 6, 1 },
    };
    const int n = (int)(1.5f * FS), win = 14400;
    float *x = alloc(n);
    size_t k;

    for (k = 0; k < sizeof(pts) / sizeof(pts[0]); k++) {
        av_voice v;
        av_voice_params p;
        float outPk, want, lvl;

        av_voice_defaults(&p);
        p.comp_on = 1; p.comp_thresh = -20; p.comp_ratio = 4; p.comp_knee = pts[k].knee;
        p.comp_attack = 1; p.comp_release = 500; p.comp_makeup = 0; p.comp_rms = (float)pts[k].rms;
        voice_with(&v, &p);
        tone(x, n, 1000, amp(pts[k].in));
        run(&v, x, n);
        outPk = dbf(peak(x + n - win, win));
        lvl = pts[k].rms ? pts[k].in - 3.0103f : pts[k].in;
        want = pts[k].in + comp_expect(lvl, -20, 4, pts[k].knee);
        check(fabsf(outPk - want) < 0.75f, "compressor 4:1 at -20 dB, knee %2.0f, %s, input %5.1f dB: out %6.2f dB, curve says %6.2f",
              pts[k].knee, pts[k].rms ? "RMS " : "peak", pts[k].in, outPk, want);
    }
    free(x);
}

/* Gain of window [a, b) seconds, output aligned by the chain latency. */
static float win_gain_db(const float *in, const float *out, int L, float a, float b)
{
    int s = (int)(a * FS), e = (int)(b * FS);
    return dbf(rms(out + s + L, e - s) / rms(in + s, e - s));
}

static void test_gate(void)
{
    const int n = (int)(1.5f * FS);
    float *in = alloc(n), *out = alloc(n);
    av_voice v;
    av_voice_params p;
    int i, L;
    float g;

    for (i = 0; i < n; i++) {
        float t = (float)i / FS;
        in[i] = 0.001f * rnd();
        if (t >= 0.5f && t < 0.8f) in[i] += 0.1f * (float)sin(2 * 3.14159265 * 1000 * (t - 0.5f));
    }
    av_voice_defaults(&p);
    p.gate_on = 1; p.gate_thresh = -40; p.gate_range = -60; p.gate_ratio = 100;
    p.gate_attack = 0.5f; p.gate_hold = 50; p.gate_release = 50; p.gate_hyst = 6;
    voice_with(&v, &p);
    L = av_voice_latency(&v);
    memcpy(out, in, sizeof(float) * (size_t)n);
    run(&v, out, n - L);
    g = win_gain_db(in, out, L, 0.3f, 0.5f);
    check(g < -50, "gate: noise at -60 dBFS below a -40 dB threshold is cut (%.1f dB, range -60)", g);
    g = win_gain_db(in, out, L, 0.505f, 0.52f);
    check(g > -0.5f, "gate: opens within 5 ms of a -20 dB tone (%.2f dB)", g);
    g = win_gain_db(in, out, L, 0.81f, 0.84f);
    check(g > -1, "gate: hold keeps it open right after the tone ends (%.2f dB)", g);
    g = win_gain_db(in, out, L, 1.2f, 1.45f);
    check(g < -50, "gate: closes again after hold and release (%.1f dB)", g);

    /* Hysteresis: a level swinging between -36 and -42 dB around a -40 dB threshold. */
    {
        int hyst;
        for (hyst = 0; hyst < 2; hyst++) {
            float worst = 0;
            int w;
            for (i = 0; i < n; i++) {
                float t = (float)i / FS;
                float a = (t < 0.2f || ((int)(t / 0.02f) % 2) == 0) ? amp(-36) : amp(-42);
                in[i] = a * (float)sin(2 * 3.14159265 * 1000 * t);
            }
            av_voice_defaults(&p);
            p.gate_on = 1; p.gate_thresh = -40; p.gate_range = -60; p.gate_ratio = 100;
            p.gate_attack = 0.5f; p.gate_hold = 0; p.gate_release = 20; p.gate_hyst = hyst ? 6.0f : 0.0f;
            voice_with(&v, &p);
            memcpy(out, in, sizeof(float) * (size_t)n);
            run(&v, out, n - L);
            for (w = 0; w < 90; w++) {
                float a = 0.25f + w * 0.01f;
                float gg = win_gain_db(in, out, L, a, a + 0.01f);
                if (gg < worst) worst = gg;
            }
            if (hyst) check(worst > -0.5f, "gate: 6 dB hysteresis stays open through -42 dB dips (worst %.2f dB)", worst);
            else check(worst < -6, "gate: without hysteresis the same signal chatters (worst %.1f dB), so the test bites", worst);
        }
    }
    free(in); free(out);
}

static void test_deesser(void)
{
    const int n = NFS, win = 14400;
    float *x = alloc(n), *ref = alloc(n);
    av_voice v;
    av_voice_params p;
    int i, L, exact = 1;
    float o;

    av_voice_defaults(&p);
    p.deess_on = 1; p.deess_hz = 6000; p.deess_thresh = -30; p.deess_range = 12;

    voice_with(&v, &p);
    tone(x, n, 8000, amp(-10));
    run(&v, x, n);
    o = dbf(peak(x + n - win, win));
    check(o < -16, "de-esser: an 8 kHz tone at -10 dB comes out at %.1f dB (at least 6 dB down)", o);

    voice_with(&v, &p);
    tone(x, n, 200, amp(-10));
    run(&v, x, n);
    o = dbf(peak(x + n - win, win));
    check(fabsf(o + 10) < 0.1f, "de-esser: a 200 Hz tone at -10 dB is untouched (%.2f dB)", o);

    voice_with(&v, &p);
    L = av_voice_latency(&v);
    tone(ref, n, 8000, amp(-45));
    memcpy(x, ref, sizeof(float) * (size_t)n);
    run(&v, x, n);
    for (i = L; i < n; i++) if (x[i] != ref[i - L]) { exact = 0; break; }
    check(exact, "de-esser: below threshold the output is bit-exact with the input");
    free(x); free(ref);
}

static void test_limiter(void)
{
    const int n = 2 * NFS;
    float *x = alloc(n);
    av_voice v;
    av_voice_params p;
    int i, L, ok = 1;
    float ceil, pk;

    av_voice_defaults(&p);
    p.lim_on = 1; p.lim_ceiling = -1; p.lim_release = 60;
    voice_with(&v, &p);
    for (i = 0; i < n; i++) x[i] = 4.0f * rnd();          /* peaks near +12 dBFS */
    run(&v, x, n);
    ceil = amp(-1);
    pk = peak(x, n);
    check(pk <= ceil, "limiter: noise peaking at +12 dBFS never passes the -1 dBFS ceiling (max %.5f dBFS)", dbf(pk));

    voice_with(&v, &p);
    tone(x, n, 1000, 2.0f);
    run(&v, x, n);
    pk = dbf(peak(x + n - 14400, 14400));
    check(pk <= -1.0f + 1e-4f && pk > -1.3f, "limiter: a +6 dBFS sine settles at the ceiling (%.3f dBFS)", pk);

    av_voice_defaults(&p);
    p.lim_on = 1; p.lim_ceiling = 0;
    voice_with(&v, &p);
    L = av_voice_latency(&v);
    memset(x, 0, sizeof(float) * (size_t)n);
    x[1000] = 0.5f;
    run(&v, x, n);
    for (i = 0; i < n; i++) if (x[i] != (i == 1000 + L ? 0.5f : 0.0f)) { ok = 0; break; }
    check(ok, "limiter: below the ceiling an impulse comes out untouched, exactly %d samples later (%.2f ms)", L, L * 1000.0f / FS);
    free(x);
}

static void test_passthrough(void)
{
    const int n = 20000;
    float *in = alloc(n), *x = alloc(n);
    av_voice v;
    av_voice_params p;
    int i, L, ok = 1;

    for (i = 0; i < n; i++) in[i] = rnd();
    av_voice_defaults(&p);
    voice_with(&v, &p);
    L = av_voice_latency(&v);
    memcpy(x, in, sizeof(float) * (size_t)n);
    run(&v, x, n);
    for (i = 0; i < n; i++) if (x[i] != (i < L ? 0.0f : in[i - L])) { ok = 0; break; }
    check(ok, "everything off: output is the input, bit-exact, delayed by the %d-sample latency", L);
    free(in); free(x);
}

static int all_ok(const float *x, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        int c = fpclassify(x[i]);
        if (c == FP_NAN || c == FP_INFINITE || c == FP_SUBNORMAL) return 0;
    }
    return 1;
}

static void test_robustness(void)
{
    const int n = NFS;
    float *x = alloc(n);
    av_voice v;
    av_voice_params p;
    int i, k, ok, zero;

    for (k = 0; av_voice_preset_names[k]; k++) {
        av_voice_preset(&p, av_voice_preset_names[k]);
        voice_with(&v, &p);
        for (i = 0; i < n; i++) x[i] = 8.0f * rnd();
        run(&v, x, n);
        ok = all_ok(x, n);
        voice_with(&v, &p);
        memset(x, 0, sizeof(float) * (size_t)n);
        run(&v, x, n);
        zero = 1;
        for (i = 0; i < n; i++) if (x[i] != 0.0f) zero = 0;
        check(ok && zero, "preset %-9s: loud noise stays finite with no denormals; silence stays exactly 0", av_voice_preset_names[k]);
    }

    for (k = 0; k < 2; k++) {
        av_voice_defaults(&p);
        for (i = 0; i < av_voice_params_count; i++) {
            *av_voice_param_ptr(&p, i) = k ? av_voice_params_info[i].max : av_voice_params_info[i].min;
        }
        p.hpf_on = p.gate_on = p.eq_on = p.comp_on = p.deess_on = p.lim_on = 1;
        voice_with(&v, &p);
        for (i = 0; i < n; i++) x[i] = (i & 1 ? 8.0f : -8.0f) * rnd();
        run(&v, x, n);
        check(all_ok(x, n), "every parameter at its %s with every stage on: output stays finite", k ? "maximum" : "minimum");
    }

    /* Fuzz: random parameters every block. */
    av_voice_init(&v, FS);
    ok = 1;
    for (k = 0; k < 3000 && ok; k++) {
        float blk[64];
        for (i = 0; i < av_voice_params_count; i++) {
            const av_param_info *pi = &av_voice_params_info[i];
            *av_voice_param_ptr(&p, i) = pi->min + (pi->max - pi->min) * 0.5f * (rnd() + 1.0f);
        }
        av_voice_set(&v, &p);
        for (i = 0; i < 64; i++) blk[i] = 2.0f * rnd();
        av_voice_process(&v, blk, 64);
        ok = all_ok(blk, 64);
    }
    check(ok, "fuzz: 3000 blocks with random parameters every block stay finite");
    free(x);
}

static float max_step(const float *x, int n)
{
    float m = 0;
    int i;
    for (i = 1; i < n; i++) if (fabsf(x[i] - x[i - 1]) > m) m = fabsf(x[i] - x[i - 1]);
    return m;
}

static void test_smoothness(void)
{
    const int n = (int)(1.5f * FS);
    float *x = alloc(n);
    av_voice v;
    av_voice_params p;
    int off;
    float s;

    /* EQ +12 dB at the tone's frequency switched on at 0.5 s and off at 1.0 s. */
    av_voice_defaults(&p);
    p.eq_type[1] = AV_BQ_PEAK; p.eq_hz[1] = 1000; p.eq_gain[1] = 12; p.eq_q[1] = 1;
    voice_with(&v, &p);
    tone(x, n, 1000, 0.25f);
    for (off = 0; off < n; off += 480) {
        p.eq_on = (off >= NFS / 2 && off < NFS) ? 1.0f : 0.0f;
        av_voice_set(&v, &p);
        av_voice_process(&v, x + off, 480);
    }
    s = max_step(x, n);
    check(s < 0.2f, "switching a +12 dB EQ on and off mid-tone: largest sample step %.3f (a hard switch would be about 0.75)", s);

    /* EQ gain jumped from 0 to +12 dB in one call. */
    av_voice_defaults(&p);
    p.eq_on = 1; p.eq_type[1] = AV_BQ_PEAK; p.eq_hz[1] = 1000; p.eq_gain[1] = 0; p.eq_q[1] = 1;
    voice_with(&v, &p);
    tone(x, n, 1000, 0.25f);
    for (off = 0; off < n; off += 480) {
        p.eq_gain[1] = off >= NFS / 2 ? 12.0f : 0.0f;
        av_voice_set(&v, &p);
        av_voice_process(&v, x + off, 480);
    }
    s = max_step(x, n);
    check(s < 0.2f, "jumping EQ gain 0 -> +12 dB mid-tone is smoothed: largest step %.3f", s);
    free(x);
}

static void test_params(void)
{
    av_voice_params p;
    int i, ok = 1;

    for (i = 0; i < av_voice_params_count; i++) {
        const av_param_info *pi = &av_voice_params_info[i];
        if (pi->def < pi->min || pi->def > pi->max || av_voice_param_find(pi->key) != i) ok = 0;
    }
    check(ok, "parameter table: %d parameters, defaults in range, names unique", av_voice_params_count);
    ok = 1;
    for (i = 0; av_voice_preset_names[i]; i++) ok &= av_voice_preset(&p, av_voice_preset_names[i]);
    check(ok && !av_voice_preset(&p, "nope"), "presets: every listed name loads, unknown names are refused");
}

static void bench(void)
{
    const int ch = 8, secs = 30, blk = 480;
    av_voice *v = (av_voice *)calloc((size_t)ch, sizeof(av_voice));
    float *x = alloc(blk);
    av_voice_params p;
    LARGE_INTEGER f, t0, t1;
    int c, b, i, blocks = secs * NFS / blk;
    double sec;

    av_voice_preset(&p, "podcast");
    for (c = 0; c < ch; c++) { av_voice_init(&v[c], FS); av_voice_set(&v[c], &p); }
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (b = 0; b < blocks; b++) {
        for (c = 0; c < ch; c++) {
            for (i = 0; i < blk; i++) x[i] = 0.3f * rnd();
            av_voice_process(&v[c], x, blk);
        }
    }
    QueryPerformanceCounter(&t1);
    sec = (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
    printf("INFO  speed: full podcast chain on %d channels x %d s at 48 kHz took %.3f s: %.0fx real time, %.2f%% of one core\n",
           ch, secs, sec, secs / sec, 100.0 * sec / secs);
    free(v); free(x);
}

int main(void)
{
    printf("avdsp checks (48 kHz)\n\n");
    test_params();
    test_biquad();
    test_compressor();
    test_gate();
    test_deesser();
    test_limiter();
    test_passthrough();
    test_smoothness();
    test_robustness();
    bench();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
