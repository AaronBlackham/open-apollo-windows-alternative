/*
 * avdsp.h - Avium voice DSP: our own processors, written from scratch.
 *
 * Portable C11, 32-bit float, no allocation, no I/O, no OS calls. The same
 * code is meant to run in three places:
 *   - the offline tools (dsp-test, dsp-render) on the PC,
 *   - the Windows audio effect (APO) on the x8p's inputs (after phase 2),
 *   - the Apollo's SHARC DSPs, if the on-chip route proves possible
 *     (SHARC is native float32; see docs/DSP.md).
 *
 * Chain per channel (mono, in place):
 *   low cut -> gate/expander -> 4-band EQ -> compressor -> de-esser
 *           -> output gain -> lookahead limiter
 * Every stage crossfades over 10 ms when switched on or off, parameter
 * moves are smoothed, and the limiter's lookahead delay is always present,
 * so the latency (av_voice_latency) never changes while running.
 *
 * Threading: av_voice_set() and av_voice_process() must not run at the same
 * time on one instance. The APO wrapper hands parameters over between blocks.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Biquad (RBJ cookbook, transposed direct form II)                    */
/* ------------------------------------------------------------------ */

enum {
    AV_BQ_LOWPASS = 0,
    AV_BQ_HIGHPASS,
    AV_BQ_BANDPASS,         /* 0 dB peak gain */
    AV_BQ_PEAK,
    AV_BQ_LOWSHELF,
    AV_BQ_HIGHSHELF,
};

typedef struct av_biquad {
    float b0, b1, b2, a1, a2;
    float z1, z2;
} av_biquad;

void  av_biquad_design(av_biquad *f, int type, float fs, float hz, float q, float gain_db);
void  av_biquad_reset(av_biquad *f);
float av_biquad_response_db(const av_biquad *f, float fs, float hz);

static inline float av_biquad_tick(av_biquad *f, float x)
{
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

/* ------------------------------------------------------------------ */
/* Voice chain                                                         */
/* ------------------------------------------------------------------ */

#define AV_EQ_BANDS         4
#define AV_MAX_LOOKAHEAD    512     /* samples; 1.5 ms needs 288 at 192 kHz */

/* Every field is a float so one table (av_voice_params_info) can describe,
   clamp and name them all. Switches are 0/1. Units: dB, Hz, ms. */
typedef struct av_voice_params {
    float hpf_on, hpf_hz, hpf_slope;                    /* slope: 12 or 24 dB/oct */
    float gate_on, gate_thresh, gate_range, gate_ratio, gate_attack, gate_hold, gate_release, gate_hyst;
    float eq_on;
    float eq_type[AV_EQ_BANDS];                         /* AV_BQ_PEAK / LOWSHELF / HIGHSHELF */
    float eq_hz[AV_EQ_BANDS], eq_gain[AV_EQ_BANDS], eq_q[AV_EQ_BANDS];
    float comp_on, comp_thresh, comp_ratio, comp_knee, comp_attack, comp_release, comp_makeup, comp_rms;
    float deess_on, deess_hz, deess_thresh, deess_range;
    float lim_on, lim_ceiling, lim_release;
    float out_gain;
} av_voice_params;

/* Largest gain changes since the last av_voice_meters_take (all dB, <= 0
   for reductions; peaks are dBFS). */
typedef struct av_voice_meters {
    float in_peak, out_peak;
    float gate_gain;        /* lowest gate gain */
    float comp_gr, deess_gr, lim_gr;
} av_voice_meters;

typedef struct av_voice {
    float fs;
    av_voice_params p;              /* targets, already clamped */
    int dirty;
    int started;                    /* first block: stages start in place, no fade */

    /* stage crossfades: current mix 0..1 */
    float mix_hpf, mix_gate, mix_eq, mix_comp, mix_deess, mix_lim;
    float fade_step;                /* mix change per sub-block */

    /* smoothed parameters */
    float hpf_hz_s;
    float eq_hz_s[AV_EQ_BANDS], eq_gain_s[AV_EQ_BANDS], eq_q_s[AV_EQ_BANDS];
    float makeup_lin, out_lin;      /* smoothed per sample */
    float k_param, k_gain;          /* smoothing coefficients (per block, per sample) */

    av_biquad hpf[2];
    av_biquad eq[AV_EQ_BANDS];
    int eq_type_cur[AV_EQ_BANDS];
    av_biquad ds_side;              /* de-esser detector: high-pass at deess_hz */
    av_biquad ds_shelf;             /* de-esser cut: high shelf at deess_hz, gain = ds_g */
    float ds_g_designed;

    /* dynamics coefficients */
    float gate_att, gate_rel, gate_env_rel;
    int   gate_hold_n;
    float comp_att, comp_rel, comp_rms_k;
    float ds_att, ds_rel;
    float lim_rel;

    /* dynamics state */
    float gate_env, gate_g;         /* gate_g in dB */
    int   gate_open, gate_hold_left;
    float comp_g, comp_ms;          /* comp_g in dB */
    float ds_g;                     /* dB */

    /* limiter */
    int   lim_len;                  /* lookahead L, also the chain latency */
    int   lim_pos;
    float lim_delay[AV_MAX_LOOKAHEAD];
    float lim_avg[AV_MAX_LOOKAHEAD];
    float lim_sum;
    unsigned lim_n;
    unsigned dq_idx[AV_MAX_LOOKAHEAD + 2];
    float dq_val[AV_MAX_LOOKAHEAD + 2];
    int   dq_head, dq_count;
    float lim_g;

    av_voice_meters m;
} av_voice;

void  av_voice_init(av_voice *v, float fs);
void  av_voice_set(av_voice *v, const av_voice_params *p);
void  av_voice_process(av_voice *v, float *buf, int n);
int   av_voice_latency(const av_voice *v);
void  av_voice_meters_take(av_voice *v, av_voice_meters *out);

/* ------------------------------------------------------------------ */
/* Parameters by name ("comp.ratio", "eq2.gain" ...) and presets       */
/* ------------------------------------------------------------------ */

typedef struct av_param_info {
    const char *key;
    size_t      offset;             /* into av_voice_params */
    float       min, max, def;
    const char *unit;
    const char *label;
} av_param_info;

extern const av_param_info av_voice_params_info[];
extern const int av_voice_params_count;

void   av_voice_defaults(av_voice_params *p);          /* everything off, sane values */
int    av_voice_param_find(const char *key);           /* index, or -1 */
float *av_voice_param_ptr(av_voice_params *p, int index);
void   av_voice_param_clamp(av_voice_params *p);

/* "clean", "voice", "podcast", "broadcast": returns 0 if unknown. */
int    av_voice_preset(av_voice_params *p, const char *name);
extern const char *const av_voice_preset_names[];

#ifdef __cplusplus
}
#endif
