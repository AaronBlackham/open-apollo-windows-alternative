/*
 * avdsp.c - Avium voice DSP (see avdsp.h).
 *
 * Designs used, all standard:
 *   filters      RBJ "Audio EQ Cookbook" biquads, transposed direct form II
 *   compressor   feed-forward, soft-knee gain computer, branching attack/
 *                release smoothing of the gain in dB (Giannoulis, Massberg,
 *                Reiss, JAES 2012)
 *   gate         downward expander with hysteresis, hold and range
 *   de-esser     dynamic high shelf at deess_hz, gain driven by a high-pass
 *                detector (4:1 above threshold, capped at the range). At 0 dB
 *                the shelf's coefficients are an exact identity, so an idle
 *                de-esser is bit-transparent. (A subtractive split,
 *                x - hp(x)(1 - g), was tried and rejected: the high-pass's
 *                phase shift leaves an 8 kHz "ess" only ~0.6 dB down.)
 *   limiter      lookahead L: gain = moving average (L) of the sliding
 *                minimum (L+1) of min(1, ceiling/|x|), applied to x delayed
 *                by L. Every term of the average saw the delayed sample, so
 *                |out| <= ceiling holds by construction.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "avdsp.h"
#include <math.h>
#include <string.h>

#define AV_SUB              32          /* sub-block for smoothing and coefficient updates */
#define AV_FADE_MS          10.0f
#define AV_LOOKAHEAD_MS     1.5f
#define AV_LN10_OVER_20     0.115129254649702f
#define AV_DQ_CAP           (AV_MAX_LOOKAHEAD + 2)

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float db2lin(float db) { return expf(db * AV_LN10_OVER_20); }
static float lin2db(float x) { return 20.0f * log10f(x > 1e-12f ? x : 1e-12f); }
static float flushf(float v) { return (v > -1e-20f && v < 1e-20f) ? 0.0f : v; }

/* One-pole coefficient for a time constant in ms (0 ms = instant). */
static float tc(float ms, float fs)
{
    return ms <= 0.0f ? 0.0f : expf(-1000.0f / (ms * fs));
}

/* ------------------------------------------------------------------ */
/* Biquad                                                              */
/* ------------------------------------------------------------------ */

void av_biquad_design(av_biquad *f, int type, float fs, float hz, float q, float gain_db)
{
    const double pi = 3.14159265358979323846;
    double nyq = 0.49 * (double)fs;
    double f0 = hz < 1.0f ? 1.0 : ((double)hz > nyq ? nyq : (double)hz);
    double w0 = 2.0 * pi * f0 / (double)fs;
    double cw = cos(w0), sw = sin(w0);
    double alpha = sw / (2.0 * (q < 0.05f ? 0.05 : (double)q));
    double A = pow(10.0, (double)gain_db / 40.0);
    double sa = 2.0 * sqrt(A) * alpha;
    double b0, b1, b2, a0, a1, a2;

    switch (type) {
    case AV_BQ_LOWPASS:
        b0 = (1.0 - cw) / 2.0; b1 = 1.0 - cw; b2 = b0;
        a0 = 1.0 + alpha; a1 = -2.0 * cw; a2 = 1.0 - alpha;
        break;
    case AV_BQ_HIGHPASS:
        b0 = (1.0 + cw) / 2.0; b1 = -(1.0 + cw); b2 = b0;
        a0 = 1.0 + alpha; a1 = -2.0 * cw; a2 = 1.0 - alpha;
        break;
    case AV_BQ_BANDPASS:
        b0 = alpha; b1 = 0.0; b2 = -alpha;
        a0 = 1.0 + alpha; a1 = -2.0 * cw; a2 = 1.0 - alpha;
        break;
    case AV_BQ_LOWSHELF:
        b0 = A * ((A + 1.0) - (A - 1.0) * cw + sa);
        b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cw);
        b2 = A * ((A + 1.0) - (A - 1.0) * cw - sa);
        a0 = (A + 1.0) + (A - 1.0) * cw + sa;
        a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cw);
        a2 = (A + 1.0) + (A - 1.0) * cw - sa;
        break;
    case AV_BQ_HIGHSHELF:
        b0 = A * ((A + 1.0) + (A - 1.0) * cw + sa);
        b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw);
        b2 = A * ((A + 1.0) + (A - 1.0) * cw - sa);
        a0 = (A + 1.0) - (A - 1.0) * cw + sa;
        a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cw);
        a2 = (A + 1.0) - (A - 1.0) * cw - sa;
        break;
    case AV_BQ_PEAK:
    default:
        b0 = 1.0 + alpha * A; b1 = -2.0 * cw; b2 = 1.0 - alpha * A;
        a0 = 1.0 + alpha / A; a1 = -2.0 * cw; a2 = 1.0 - alpha / A;
        break;
    }
    f->b0 = (float)(b0 / a0);
    f->b1 = (float)(b1 / a0);
    f->b2 = (float)(b2 / a0);
    f->a1 = (float)(a1 / a0);
    f->a2 = (float)(a2 / a0);
}

void av_biquad_reset(av_biquad *f)
{
    f->z1 = 0.0f;
    f->z2 = 0.0f;
}

float av_biquad_response_db(const av_biquad *f, float fs, float hz)
{
    const double pi = 3.14159265358979323846;
    double w = 2.0 * pi * (double)hz / (double)fs;
    double c1 = cos(w), s1 = sin(w), c2 = cos(2.0 * w), s2 = sin(2.0 * w);
    double nr = f->b0 + f->b1 * c1 + f->b2 * c2, ni = -(f->b1 * s1 + f->b2 * s2);
    double dr = 1.0 + f->a1 * c1 + f->a2 * c2, di = -(f->a1 * s1 + f->a2 * s2);
    double m2 = (nr * nr + ni * ni) / (dr * dr + di * di);
    return (float)(10.0 * log10(m2 > 1e-30 ? m2 : 1e-30));
}

static void flush_biquad(av_biquad *f)
{
    f->z1 = flushf(f->z1);
    f->z2 = flushf(f->z2);
}

/* ------------------------------------------------------------------ */
/* Parameters                                                          */
/* ------------------------------------------------------------------ */

#define PO(field) offsetof(av_voice_params, field)

const av_param_info av_voice_params_info[] = {
    { "hpf.on",        PO(hpf_on),        0, 1, 0, "", "Low cut" },
    { "hpf.hz",        PO(hpf_hz),        20, 400, 80, "Hz", "Low cut frequency" },
    { "hpf.slope",     PO(hpf_slope),     12, 24, 12, "dB/oct", "Low cut slope" },
    { "gate.on",       PO(gate_on),       0, 1, 0, "", "Gate" },
    { "gate.thresh",   PO(gate_thresh),   -90, 0, -50, "dB", "Gate threshold" },
    { "gate.range",    PO(gate_range),    -90, 0, -30, "dB", "Gate range" },
    { "gate.ratio",    PO(gate_ratio),    1.5f, 100, 4, ":1", "Gate ratio (100 = hard gate)" },
    { "gate.attack",   PO(gate_attack),   0.1f, 50, 1, "ms", "Gate attack" },
    { "gate.hold",     PO(gate_hold),     0, 500, 50, "ms", "Gate hold" },
    { "gate.release",  PO(gate_release),  5, 2000, 150, "ms", "Gate release" },
    { "gate.hyst",     PO(gate_hyst),     0, 12, 4, "dB", "Gate hysteresis" },
    { "eq.on",         PO(eq_on),         0, 1, 0, "", "EQ" },
    { "eq1.type",      PO(eq_type[0]),    AV_BQ_PEAK, AV_BQ_HIGHSHELF, AV_BQ_LOWSHELF, "", "EQ 1 type" },
    { "eq1.hz",        PO(eq_hz[0]),      20, 20000, 120, "Hz", "EQ 1 frequency" },
    { "eq1.gain",      PO(eq_gain[0]),    -18, 18, 0, "dB", "EQ 1 gain" },
    { "eq1.q",         PO(eq_q[0]),       0.2f, 10, 0.71f, "", "EQ 1 Q" },
    { "eq2.type",      PO(eq_type[1]),    AV_BQ_PEAK, AV_BQ_HIGHSHELF, AV_BQ_PEAK, "", "EQ 2 type" },
    { "eq2.hz",        PO(eq_hz[1]),      20, 20000, 400, "Hz", "EQ 2 frequency" },
    { "eq2.gain",      PO(eq_gain[1]),    -18, 18, 0, "dB", "EQ 2 gain" },
    { "eq2.q",         PO(eq_q[1]),       0.2f, 10, 1, "", "EQ 2 Q" },
    { "eq3.type",      PO(eq_type[2]),    AV_BQ_PEAK, AV_BQ_HIGHSHELF, AV_BQ_PEAK, "", "EQ 3 type" },
    { "eq3.hz",        PO(eq_hz[2]),      20, 20000, 3000, "Hz", "EQ 3 frequency" },
    { "eq3.gain",      PO(eq_gain[2]),    -18, 18, 0, "dB", "EQ 3 gain" },
    { "eq3.q",         PO(eq_q[2]),       0.2f, 10, 1, "", "EQ 3 Q" },
    { "eq4.type",      PO(eq_type[3]),    AV_BQ_PEAK, AV_BQ_HIGHSHELF, AV_BQ_HIGHSHELF, "", "EQ 4 type" },
    { "eq4.hz",        PO(eq_hz[3]),      20, 20000, 10000, "Hz", "EQ 4 frequency" },
    { "eq4.gain",      PO(eq_gain[3]),    -18, 18, 0, "dB", "EQ 4 gain" },
    { "eq4.q",         PO(eq_q[3]),       0.2f, 10, 0.71f, "", "EQ 4 Q" },
    { "comp.on",       PO(comp_on),       0, 1, 0, "", "Compressor" },
    { "comp.thresh",   PO(comp_thresh),   -60, 0, -20, "dB", "Compressor threshold" },
    { "comp.ratio",    PO(comp_ratio),    1, 20, 3, ":1", "Compressor ratio" },
    { "comp.knee",     PO(comp_knee),     0, 24, 6, "dB", "Compressor knee" },
    { "comp.attack",   PO(comp_attack),   0.1f, 200, 5, "ms", "Compressor attack" },
    { "comp.release",  PO(comp_release),  10, 3000, 120, "ms", "Compressor release" },
    { "comp.makeup",   PO(comp_makeup),   0, 24, 0, "dB", "Compressor makeup gain" },
    { "comp.rms",      PO(comp_rms),      0, 1, 0, "", "Compressor RMS detection" },
    { "deess.on",      PO(deess_on),      0, 1, 0, "", "De-esser" },
    { "deess.hz",      PO(deess_hz),      2000, 12000, 6500, "Hz", "De-esser frequency" },
    { "deess.thresh",  PO(deess_thresh),  -60, 0, -30, "dB", "De-esser threshold" },
    { "deess.range",   PO(deess_range),   0, 24, 8, "dB", "De-esser maximum reduction" },
    { "lim.on",        PO(lim_on),        0, 1, 0, "", "Limiter" },
    { "lim.ceiling",   PO(lim_ceiling),   -24, 0, -1, "dBFS", "Limiter ceiling" },
    { "lim.release",   PO(lim_release),   5, 1000, 60, "ms", "Limiter release" },
    { "out.gain",      PO(out_gain),      -24, 24, 0, "dB", "Output gain" },
};
const int av_voice_params_count = (int)(sizeof(av_voice_params_info) / sizeof(av_voice_params_info[0]));

float *av_voice_param_ptr(av_voice_params *p, int index)
{
    return (float *)((char *)p + av_voice_params_info[index].offset);
}

void av_voice_defaults(av_voice_params *p)
{
    int i;

    memset(p, 0, sizeof(*p));
    for (i = 0; i < av_voice_params_count; i++) {
        *av_voice_param_ptr(p, i) = av_voice_params_info[i].def;
    }
}

int av_voice_param_find(const char *key)
{
    int i;

    for (i = 0; i < av_voice_params_count; i++) {
        if (strcmp(av_voice_params_info[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

void av_voice_param_clamp(av_voice_params *p)
{
    int i, b;

    for (i = 0; i < av_voice_params_count; i++) {
        const av_param_info *pi = &av_voice_params_info[i];
        float *v = av_voice_param_ptr(p, i);

        if (!(*v == *v)) {
            *v = pi->def;                       /* NaN */
        }
        *v = clampf(*v, pi->min, pi->max);
    }
    p->hpf_on = p->hpf_on >= 0.5f ? 1.0f : 0.0f;
    p->gate_on = p->gate_on >= 0.5f ? 1.0f : 0.0f;
    p->eq_on = p->eq_on >= 0.5f ? 1.0f : 0.0f;
    p->comp_on = p->comp_on >= 0.5f ? 1.0f : 0.0f;
    p->comp_rms = p->comp_rms >= 0.5f ? 1.0f : 0.0f;
    p->deess_on = p->deess_on >= 0.5f ? 1.0f : 0.0f;
    p->lim_on = p->lim_on >= 0.5f ? 1.0f : 0.0f;
    p->hpf_slope = p->hpf_slope >= 18.0f ? 24.0f : 12.0f;
    for (b = 0; b < AV_EQ_BANDS; b++) {
        p->eq_type[b] = (float)(int)(p->eq_type[b] + 0.5f);
    }
}

const char *const av_voice_preset_names[] = { "clean", "voice", "podcast", "broadcast", NULL };

int av_voice_preset(av_voice_params *p, const char *name)
{
    av_voice_defaults(p);
    if (strcmp(name, "clean") == 0) {
        p->hpf_on = 1; p->hpf_hz = 80;
        p->lim_on = 1; p->lim_ceiling = -1;
        return 1;
    }
    if (strcmp(name, "voice") == 0 || strcmp(name, "podcast") == 0 || strcmp(name, "broadcast") == 0) {
        /* Calls: keep the room and keyboard down, steady level, no harsh esses. */
        p->hpf_on = 1; p->hpf_hz = 90; p->hpf_slope = 12;
        p->gate_on = 1; p->gate_thresh = -55; p->gate_range = -20; p->gate_ratio = 3;
        p->gate_attack = 1; p->gate_hold = 80; p->gate_release = 200; p->gate_hyst = 4;
        p->eq_on = 1;
        p->eq_type[0] = AV_BQ_LOWSHELF;  p->eq_hz[0] = 120;   p->eq_gain[0] = 0;    p->eq_q[0] = 0.71f;
        p->eq_type[1] = AV_BQ_PEAK;      p->eq_hz[1] = 300;   p->eq_gain[1] = -2;   p->eq_q[1] = 1.0f;
        p->eq_type[2] = AV_BQ_PEAK;      p->eq_hz[2] = 3500;  p->eq_gain[2] = 2.5f; p->eq_q[2] = 0.9f;
        p->eq_type[3] = AV_BQ_HIGHSHELF; p->eq_hz[3] = 10000; p->eq_gain[3] = 1.5f; p->eq_q[3] = 0.71f;
        p->comp_on = 1; p->comp_thresh = -24; p->comp_ratio = 3; p->comp_knee = 6;
        p->comp_attack = 5; p->comp_release = 140; p->comp_makeup = 6; p->comp_rms = 0;
        p->deess_on = 1; p->deess_hz = 6500; p->deess_thresh = -32; p->deess_range = 8;
        p->lim_on = 1; p->lim_ceiling = -1; p->lim_release = 60;
        if (strcmp(name, "podcast") == 0) {
            /* Closer, warmer, more even. */
            p->hpf_hz = 80; p->hpf_slope = 24;
            p->gate_thresh = -50; p->gate_range = -25; p->gate_ratio = 4;
            p->eq_gain[0] = 1.5f;
            p->comp_thresh = -28; p->comp_ratio = 4; p->comp_knee = 8; p->comp_attack = 3;
            p->comp_release = 120; p->comp_makeup = 9; p->comp_rms = 1;
            p->deess_hz = 6000; p->deess_thresh = -34; p->deess_range = 10;
        } else if (strcmp(name, "broadcast") == 0) {
            /* Dense and loud. */
            p->comp_thresh = -32; p->comp_ratio = 6; p->comp_knee = 10; p->comp_attack = 2;
            p->comp_release = 100; p->comp_makeup = 12; p->comp_rms = 1;
            p->lim_release = 40;
        }
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Voice chain                                                         */
/* ------------------------------------------------------------------ */

static void lim_reset(av_voice *v)
{
    int i;

    for (i = 0; i < v->lim_len; i++) {
        v->lim_avg[i] = 1.0f;
    }
    v->lim_sum = (float)v->lim_len;
    v->dq_head = 0;
    v->dq_count = 0;
    v->lim_g = 1.0f;
}

static void design_hpf(av_voice *v)
{
    if (v->p.hpf_slope >= 18.0f) {
        /* 4th-order Butterworth as two sections */
        av_biquad_design(&v->hpf[0], AV_BQ_HIGHPASS, v->fs, v->hpf_hz_s, 0.5411961f, 0.0f);
        av_biquad_design(&v->hpf[1], AV_BQ_HIGHPASS, v->fs, v->hpf_hz_s, 1.3065630f, 0.0f);
    } else {
        av_biquad_design(&v->hpf[0], AV_BQ_HIGHPASS, v->fs, v->hpf_hz_s, 0.7071068f, 0.0f);
    }
}

static void design_eq(av_voice *v, int b)
{
    int type = (int)v->p.eq_type[b];

    av_biquad_design(&v->eq[b], type, v->fs, v->eq_hz_s[b], v->eq_q_s[b], v->eq_gain_s[b]);
    v->eq_type_cur[b] = type;
}

void av_voice_init(av_voice *v, float fs)
{
    int b, L;

    memset(v, 0, sizeof(*v));
    v->fs = fs;
    av_voice_defaults(&v->p);
    v->fade_step = (float)AV_SUB / (AV_FADE_MS * 0.001f * fs);
    v->k_param = 1.0f - expf(-(float)AV_SUB / (0.02f * fs));
    v->k_gain = 1.0f - expf(-1.0f / (0.02f * fs));

    L = (int)(AV_LOOKAHEAD_MS * 0.001f * fs + 0.5f);
    if (L < 1) L = 1;
    if (L > AV_MAX_LOOKAHEAD - 1) L = AV_MAX_LOOKAHEAD - 1;
    v->lim_len = L;
    lim_reset(v);

    v->hpf_hz_s = v->p.hpf_hz;
    for (b = 0; b < AV_EQ_BANDS; b++) {
        v->eq_hz_s[b] = v->p.eq_hz[b];
        v->eq_gain_s[b] = v->p.eq_gain[b];
        v->eq_q_s[b] = v->p.eq_q[b];
        design_eq(v, b);
    }
    design_hpf(v);
    v->makeup_lin = db2lin(v->p.comp_makeup);
    v->out_lin = db2lin(v->p.out_gain);
    v->gate_open = 1;
    v->dirty = 1;
    v->m.gate_gain = 0.0f;
}

void av_voice_set(av_voice *v, const av_voice_params *p)
{
    float oldSlope = v->p.hpf_slope;

    v->p = *p;
    av_voice_param_clamp(&v->p);
    if (v->p.hpf_slope != oldSlope) {
        av_biquad_reset(&v->hpf[1]);
        design_hpf(v);
    }
    v->dirty = 1;
}

int av_voice_latency(const av_voice *v)
{
    return v->lim_len;
}

void av_voice_meters_take(av_voice *v, av_voice_meters *out)
{
    out->in_peak = lin2db(v->m.in_peak);
    out->out_peak = lin2db(v->m.out_peak);
    out->gate_gain = v->m.gate_gain;
    out->comp_gr = v->m.comp_gr;
    out->deess_gr = v->m.deess_gr;
    out->lim_gr = v->m.lim_gr;
    memset(&v->m, 0, sizeof(v->m));
}

/* Step a stage's mix toward on/off. Returns 0 idle, 1 active, 2 just switched on. */
static int fade(float *mix, float on, float step, float *m0, float *m1)
{
    float target = on >= 0.5f ? 1.0f : 0.0f;

    *m0 = *mix;
    if (*mix < target) {
        *mix += step;
        if (*mix > target) *mix = target;
    } else if (*mix > target) {
        *mix -= step;
        if (*mix < target) *mix = target;
    }
    *m1 = *mix;
    if (*m0 == 0.0f && *m1 > 0.0f) return 2;
    return (*m0 > 0.0f || *m1 > 0.0f) ? 1 : 0;
}

static int smooth_to(float *cur, float target, float k, float snap)
{
    float d = target - *cur;

    if (d == 0.0f) return 0;
    if (fabsf(d) < snap) *cur = target;
    else *cur += d * k;
    return 1;
}

static int smooth_hz(float *cur, float target, float k)
{
    float r;

    if (*cur == target) return 0;
    r = target / *cur;
    if (fabsf(r - 1.0f) < 1e-3f) *cur = target;
    else *cur *= powf(r, k);
    return 1;
}

static float comp_gain(float lvl, float T, float R, float W)
{
    float d = lvl - T;

    if (W > 0.0f && 2.0f * fabsf(d) <= W) {
        float t = d + 0.5f * W;
        return (1.0f / R - 1.0f) * t * t / (2.0f * W);
    }
    return d > 0.0f ? d * (1.0f / R - 1.0f) : 0.0f;
}

typedef struct {
    int   act[6];
    float m0[6], m1[6];
} av_block;

enum { ST_HPF, ST_GATE, ST_EQ, ST_COMP, ST_DEESS, ST_LIM };

static void update_block(av_voice *v, av_block *bk)
{
    const av_voice_params *p = &v->p;
    float fs = v->fs;
    int b, s;
    float *mix[6] = { &v->mix_hpf, &v->mix_gate, &v->mix_eq, &v->mix_comp, &v->mix_deess, &v->mix_lim };
    float on[6] = { p->hpf_on, p->gate_on, p->eq_on, p->comp_on, p->deess_on, p->lim_on };

    if (v->dirty) {
        v->gate_att = tc(p->gate_attack, fs);
        v->gate_rel = tc(p->gate_release, fs);
        v->gate_env_rel = tc(10.0f, fs);
        v->gate_hold_n = (int)(p->gate_hold * 0.001f * fs);
        v->comp_att = tc(p->comp_attack, fs);
        v->comp_rel = tc(p->comp_release, fs);
        v->comp_rms_k = tc(10.0f, fs);
        v->ds_att = tc(1.0f, fs);
        v->ds_rel = tc(60.0f, fs);
        v->lim_rel = tc(p->lim_release, fs);
        av_biquad_design(&v->ds_side, AV_BQ_HIGHPASS, fs, p->deess_hz, 0.7071068f, 0.0f);
        av_biquad_design(&v->ds_shelf, AV_BQ_HIGHSHELF, fs, p->deess_hz, 0.7071068f, v->ds_g);
        v->ds_g_designed = v->ds_g;
        v->dirty = 0;
    }

    for (s = 0; s < 6; s++) {
        if (!v->started) {
            *mix[s] = on[s] >= 0.5f ? 1.0f : 0.0f;     /* stream start: no fade-in */
        }
        bk->act[s] = fade(mix[s], on[s], v->fade_step, &bk->m0[s], &bk->m1[s]);
        if (bk->act[s] == 2) {
            switch (s) {
            case ST_HPF: av_biquad_reset(&v->hpf[0]); av_biquad_reset(&v->hpf[1]); break;
            case ST_GATE: v->gate_env = 0.0f; v->gate_g = 0.0f; v->gate_open = 1;
                          v->gate_hold_left = v->gate_hold_n; break;
            case ST_EQ: for (b = 0; b < AV_EQ_BANDS; b++) av_biquad_reset(&v->eq[b]); break;
            case ST_COMP: v->comp_g = 0.0f; v->comp_ms = 0.0f; break;
            case ST_DEESS: av_biquad_reset(&v->ds_side); av_biquad_reset(&v->ds_shelf); v->ds_g = 0.0f; break;
            case ST_LIM: lim_reset(v); break;
            }
        }
        if (!v->started && *mix[s] > 0.0f) {
            bk->act[s] = 1;
        }
    }
    v->started = 1;

    /* De-esser shelf follows the detector once per sub-block; exactly 0 dB when idle. */
    if (v->ds_g != v->ds_g_designed &&
        (v->ds_g == 0.0f || fabsf(v->ds_g - v->ds_g_designed) > 0.05f)) {
        av_biquad_design(&v->ds_shelf, AV_BQ_HIGHSHELF, fs, p->deess_hz, 0.7071068f, v->ds_g);
        v->ds_g_designed = v->ds_g;
    }

    if (smooth_hz(&v->hpf_hz_s, p->hpf_hz, v->k_param)) {
        design_hpf(v);
    }
    for (b = 0; b < AV_EQ_BANDS; b++) {
        int changed = (int)p->eq_type[b] != v->eq_type_cur[b];

        changed |= smooth_to(&v->eq_gain_s[b], p->eq_gain[b], v->k_param, 0.01f);
        changed |= smooth_hz(&v->eq_hz_s[b], p->eq_hz[b], v->k_param);
        changed |= smooth_to(&v->eq_q_s[b], p->eq_q[b], v->k_param, 0.001f);
        if (changed) {
            design_eq(v, b);
        }
    }
}

static void flush_state(av_voice *v)
{
    int b;

    flush_biquad(&v->hpf[0]);
    flush_biquad(&v->hpf[1]);
    for (b = 0; b < AV_EQ_BANDS; b++) flush_biquad(&v->eq[b]);
    flush_biquad(&v->ds_side);
    flush_biquad(&v->ds_shelf);
    v->gate_env = flushf(v->gate_env);
    v->comp_ms = flushf(v->comp_ms);
    if (v->gate_g > -1e-6f) v->gate_g = 0.0f;
    if (v->comp_g > -1e-6f) v->comp_g = 0.0f;
    if (v->ds_g > -1e-6f) v->ds_g = 0.0f;
}

static void run_block(av_voice *v, const av_block *bk, float *buf, int n)
{
    const av_voice_params *p = &v->p;
    const int L = v->lim_len;
    const float ceil_lin = db2lin(p->lim_ceiling) * 0.99999f;  /* margin for float rounding in the average */
    const float makeup_t = db2lin(p->comp_makeup);
    const float out_t = db2lin(p->out_gain);
    const int hpf24 = p->hpf_slope >= 18.0f;
    float gate_min = v->m.gate_gain, comp_min = v->m.comp_gr, ds_min = v->m.deess_gr, lim_min = 1.0f;
    float pk_in = v->m.in_peak, pk_out = v->m.out_peak;
    int i, b;

    for (i = 0; i < n; i++) {
        float x = buf[i], y, a;
        float fr = (float)(i + 1) / (float)n;

        a = fabsf(x);
        if (a > pk_in) pk_in = a;

        if (bk->act[ST_HPF]) {
            float m = bk->m0[ST_HPF] + (bk->m1[ST_HPF] - bk->m0[ST_HPF]) * fr;
            y = av_biquad_tick(&v->hpf[0], x);
            if (hpf24) y = av_biquad_tick(&v->hpf[1], y);
            x += (y - x) * m;
        }

        if (bk->act[ST_GATE]) {
            float m = bk->m0[ST_GATE] + (bk->m1[ST_GATE] - bk->m0[ST_GATE]) * fr;
            float lvl, target = 0.0f, c;

            a = fabsf(x);
            v->gate_env = a > v->gate_env ? a : v->gate_env * v->gate_env_rel;
            lvl = lin2db(v->gate_env);
            if (lvl >= p->gate_thresh) {
                v->gate_open = 1;
                v->gate_hold_left = v->gate_hold_n;
            } else if (lvl < p->gate_thresh - p->gate_hyst) {
                if (v->gate_hold_left > 0) v->gate_hold_left--;
                else v->gate_open = 0;
            }
            if (!v->gate_open) {
                target = (lvl - p->gate_thresh) * (p->gate_ratio - 1.0f);
                if (target < p->gate_range) target = p->gate_range;
                if (target > 0.0f) target = 0.0f;
            }
            c = target > v->gate_g ? v->gate_att : v->gate_rel;
            v->gate_g = target + (v->gate_g - target) * c;
            if (v->gate_g < gate_min) gate_min = v->gate_g;
            x *= 1.0f + (db2lin(v->gate_g) - 1.0f) * m;
        }

        if (bk->act[ST_EQ]) {
            float m = bk->m0[ST_EQ] + (bk->m1[ST_EQ] - bk->m0[ST_EQ]) * fr;
            y = x;
            for (b = 0; b < AV_EQ_BANDS; b++) y = av_biquad_tick(&v->eq[b], y);
            x += (y - x) * m;
        }

        if (bk->act[ST_COMP]) {
            float m = bk->m0[ST_COMP] + (bk->m1[ST_COMP] - bk->m0[ST_COMP]) * fr;
            float lvl, gc, c;

            if (p->comp_rms >= 0.5f) {
                float x2 = x * x;
                v->comp_ms = x2 + (v->comp_ms - x2) * v->comp_rms_k;
                lvl = 10.0f * log10f(v->comp_ms > 1e-24f ? v->comp_ms : 1e-24f);
            } else {
                lvl = lin2db(fabsf(x));
            }
            gc = comp_gain(lvl, p->comp_thresh, p->comp_ratio, p->comp_knee);
            c = gc < v->comp_g ? v->comp_att : v->comp_rel;
            v->comp_g = gc + (v->comp_g - gc) * c;
            if (v->comp_g < comp_min) comp_min = v->comp_g;
            v->makeup_lin += (makeup_t - v->makeup_lin) * v->k_gain;
            x *= 1.0f + (db2lin(v->comp_g) * v->makeup_lin - 1.0f) * m;
        }

        if (bk->act[ST_DEESS]) {
            float m = bk->m0[ST_DEESS] + (bk->m1[ST_DEESS] - bk->m0[ST_DEESS]) * fr;
            float hi = av_biquad_tick(&v->ds_side, x);
            float over = lin2db(fabsf(hi)) - p->deess_thresh;
            float target = over > 0.0f ? -0.75f * over : 0.0f;     /* 4:1 above threshold */
            float c;

            if (target < -p->deess_range) target = -p->deess_range;
            c = target < v->ds_g ? v->ds_att : v->ds_rel;
            v->ds_g = target + (v->ds_g - target) * c;
            if (v->ds_g < ds_min) ds_min = v->ds_g;
            y = av_biquad_tick(&v->ds_shelf, x);
            x += (y - x) * m;
        }

        if (v->out_lin != out_t) {
            v->out_lin += (out_t - v->out_lin) * v->k_gain;
            if (fabsf(v->out_lin - out_t) < 1e-6f) v->out_lin = out_t;
        }
        x *= v->out_lin;

        /* Limiter: the lookahead delay always runs, so latency never changes. */
        y = v->lim_delay[v->lim_pos];
        v->lim_delay[v->lim_pos] = x;
        if (bk->act[ST_LIM]) {
            float m = bk->m0[ST_LIM] + (bk->m1[ST_LIM] - bk->m0[ST_LIM]) * fr;
            float t, mn, avg;
            unsigned idx = v->lim_n++;
            int back;

            a = fabsf(x);
            t = a > ceil_lin ? ceil_lin / a : 1.0f;
            while (v->dq_count > 0) {
                back = (v->dq_head + v->dq_count - 1) % AV_DQ_CAP;
                if (v->dq_val[back] < t) break;
                v->dq_count--;
            }
            back = (v->dq_head + v->dq_count) % AV_DQ_CAP;
            v->dq_idx[back] = idx;
            v->dq_val[back] = t;
            v->dq_count++;
            while ((unsigned)(idx - v->dq_idx[v->dq_head]) > (unsigned)L) {
                v->dq_head = (v->dq_head + 1) % AV_DQ_CAP;
                v->dq_count--;
            }
            mn = v->dq_val[v->dq_head];
            v->lim_sum += mn - v->lim_avg[v->lim_pos];
            v->lim_avg[v->lim_pos] = mn;
            avg = v->lim_sum / (float)L;
            if (avg > 1.0f) avg = 1.0f;
            if (avg < v->lim_g) v->lim_g = avg;
            else v->lim_g = avg + (v->lim_g - avg) * v->lim_rel;
            if (v->lim_g < lim_min) lim_min = v->lim_g;
            y *= 1.0f + (v->lim_g - 1.0f) * m;
        }
        if (++v->lim_pos >= L) {
            int k;
            float s = 0.0f;

            v->lim_pos = 0;
            for (k = 0; k < L; k++) s += v->lim_avg[k];     /* stop float drift in the running sum */
            v->lim_sum = s;
        }

        buf[i] = y;
        a = fabsf(y);
        if (a > pk_out) pk_out = a;
    }

    v->m.in_peak = pk_in;
    v->m.out_peak = pk_out;
    v->m.gate_gain = gate_min;
    v->m.comp_gr = comp_min;
    v->m.deess_gr = ds_min;
    if (lin2db(lim_min) < v->m.lim_gr) v->m.lim_gr = lin2db(lim_min);
}

void av_voice_process(av_voice *v, float *buf, int n)
{
    int off = 0;

    while (off < n) {
        av_block bk;
        int m = n - off < AV_SUB ? n - off : AV_SUB;

        update_block(v, &bk);
        run_block(v, &bk, buf + off, m);
        flush_state(v);
        off += m;
    }
}
