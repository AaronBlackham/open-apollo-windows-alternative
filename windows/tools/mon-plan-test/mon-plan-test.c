/*
 * mon-plan-test.c - host-side check of driver\monitor_plan.h, the monitor features per model
 * (docs/console-parity.md "Monitor"): runs the exact code mixer.c and cues.c run and checks
 *   1. the x8p: every SET_MONITOR_PARAM code 0..0x4FFF with values from -2e9 to 2e9 gives the same result
 *      and the same mailbox write as mixer.c ApolloSetMonitorParam before this change (copied below as
 *      it was) for the ten codes it had, except the monitor source's new CUE 3 and CUE 4; every other
 *      code is either refused as before or one of the new codes;
 *   2. every mailbox write of the new codes against open-apollo's kext mapping (oa:driver/ua_core.c
 *      ua_monitor_set_param, copied below), on both models, and that no monitor write ever reaches a
 *      preamp field (S0, S1, S3, S11, S16, S17, S24, S25; S12 [23:0], S13 [27:9], S14 [27:0]);
 *   3. the Twin X through its bring-up plan filter (bringup_plan.h) and the encoder: what it takes
 *      (sources MIX/HP/LINE 3/4, HP1 source, CUE 1-2, ALT 1 on LINE 3/4, sends from inputs 1-2 and the
 *      talkback) and what it refuses, with which status (HP 2, dim level, CUE 3/4, ALT 2, other buses);
 *   4. the cue send coefficients: both tables against libm, IOKit-captured values, the pan law, mute,
 *      off; the cue state (store, mute, the zeroed state is "nothing set");
 *   5. the readback decoders (ALT selection word 6 [29:28], the "if echoed" fields) and the plan bits;
 *   6. [polish Nov 15] cues.c's ring waits (ApolloCuePutStep, one APOLLO_CUE_PASS per loop): simulated DSP
 *      command rings and a simulated KeQueryInterruptTime run cues.c's loops (replay, mute, bus move) with a
 *      hung DSP, two hung DSPs, a slow DSP, a healthy DSP under a burst, at 1 ms and 15.6 ms sleep
 *      granularity, against the old per-cell wait (50 sleeps per cell, copied below).
 *   7. [Mix] the monitor mix: the codes per model (through the plan filter), the weights against the engine's
 *      encode_mix_coeff / _r and the talkback law (libm, IOKit captures), the zeroed state (nothing set, nothing
 *      replayed: the x8p unchanged), cues.c's MixApply / replay / bus move run on the same pure functions, solo
 *      (host logic: silences the others, mute wins, the mutes kept apart, ending it restores what was set),
 *      mute-all, a linked pair as upstream writes it, the -12 dB step step-mix checks, and the replay of mix
 *      cells through the ring-wait pass with a hung DSP.
 *   8. [Clock] the front panel (driver\clock_plan.h): the live readback words of both units decoded (knob mode,
 *      level, lock), a knob turn / press / talkback as changes, a host write's echo told from the panel, and the
 *      monitor link's rules (only the knob unit's own changes, the level as clamped steps, talkback dim).
 *   9. [Virtual] the Virtual buses (VIRTUAL n = 0x18 + n - 1) of the Input Strips a unit builds: ApolloMonEncodeEx with
 *      no Virtual bus is ApolloMonEncode for every code and value (so 1. holds for mixer.c's path); with VIRTUAL n's bit
 *      every mix, cue and aux family takes that bus as it takes ANALOG 1, nothing else moves, mute-all and solo cover it.
 * Exit code 0 = all pass.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "..\..\driver\bringup_plan.h"     /* includes monitor_plan.h */
#include "..\..\driver\clock_plan.h"       /* [Clock] the front panel and the monitor link (8) */

static int g_fail = 0, g_pass = 0;

static void Check(int ok, const char *what)
{
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        printf("FAIL  %s\n", what);
    }
}

#define R_OK        0
#define R_INVALID   1   /* STATUS_INVALID_PARAMETER */
#define R_NOTSUP    2   /* STATUS_NOT_SUPPORTED */

typedef struct _MON_OUT {
    ULONG Setting, Value, Mask;
} MON_OUT;

/* ---- mixer.c ApolloSetMonitorParam before [Monitor] (builds\base-pre-mon), the write recorded ---- */

static BOOLEAN OldInRange(LONG V, LONG Lo, LONG Hi)
{
    return V >= Lo && V <= Hi;
}

static int OldSetMonitorParam(ULONG Param, LONG Value, MON_OUT *O)
{
    ULONG setting, val, mask;
    LONG v = Value;

    switch (Param) {
    case 0x01:
        if (!OldInRange(v, 0, 192)) return R_INVALID;
        setting = 2; mask = 0x000000FF; val = (ULONG)v;
        break;
    case 0x02:
        if (!OldInRange(v, 0, 192)) return R_INVALID;
        setting = 2; mask = 0x0000FF00; val = (ULONG)v << 8;
        break;
    case 0x03:
        if (!OldInRange(v, 0, 1)) return R_INVALID;
        setting = 2; mask = 1u << 16; val = v ? mask : 0;
        break;
    case 0x10:
        if (!OldInRange(v, 0, 1)) return R_INVALID;
        setting = 2; mask = 1u << 17; val = v ? mask : 0;
        break;
    case 0x04:
        if (!OldInRange(v, 0, 2)) return R_INVALID;
        setting = 2; mask = 0x200C0000; val = ((ULONG)v & 3) << 18;
        break;
    case 0x44:
        if (!OldInRange(v, 0, 1)) return R_INVALID;
        setting = 2; mask = 1u << 31; val = v ? mask : 0;
        break;
    case 0x3F:
        if (!OldInRange(v, 0, 1)) return R_INVALID;
        setting = 12; mask = 0x07000000; val = (ULONG)v << 24;
        break;
    case 0x40:
        if (!OldInRange(v, 0, 1)) return R_INVALID;
        setting = 12; mask = 0x38000000; val = (ULONG)v << 27;
        break;
    case 0x43:
        if (!OldInRange(v, 1, 7)) return R_INVALID;
        setting = 14; mask = 0x70000000; val = (ULONG)v << 28;
        break;
    case 0x46:
        if (!OldInRange(v, 0, 1)) return R_INVALID;
        setting = 13; mask = 1u << 8; val = v ? mask : 0;
        break;
    default:
        return R_NOTSUP;
    }
    O->Setting = setting;
    O->Value = val;
    O->Mask = mask;
    return R_OK;
}

/* ---- open-apollo's kext mapping (oa:driver/ua_core.c:1973-2340), kext values ---- */

static ULONG OaSwap2(ULONG value, unsigned int lo)
{
    return ((value & 2) ? (1ul << lo) : 0) | ((value & 1) ? (1ul << (lo + 1)) : 0);
}

static int OaMonitorSetParam(ULONG Param, ULONG v, MON_OUT *O)
{
    switch (Param) {
    case 0x01: O->Setting = 2; O->Value = v & 0xFF; O->Mask = 0x000000FF; break;
    case 0x02: O->Setting = 2; O->Value = (v & 0xFF) << 8; O->Mask = 0x0000FF00; break;
    case 0x04: O->Setting = 2; O->Value = ((v & 3) << 18) | (((v >> 2) & 1) << 29); O->Mask = 0x200C0000; break;
    case 0x05: O->Setting = 2; O->Value = (v & 3) << 24; O->Mask = 0x03000000; break;
    case 0x06: O->Setting = 2; O->Value = OaSwap2(v, 20); O->Mask = 0x00300000; break;
    case 0x07: O->Setting = 2; O->Value = (v & 3) << 26; O->Mask = 0x0C000000; break;
    case 0x08: O->Setting = 2; O->Value = OaSwap2(v, 22); O->Mask = 0x00C00000; break;
    case 0x22: O->Setting = 8; O->Value = (v & 3) << 28; O->Mask = 0x30000000; break;
    case 0x23: O->Setting = 8; O->Value = OaSwap2(v, 24); O->Mask = 0x03000000; break;
    case 0x24: O->Setting = 8; O->Value = (v & 3) << 30; O->Mask = 0xC0000000; break;
    case 0x25: O->Setting = 8; O->Value = OaSwap2(v, 26); O->Mask = 0x0C000000; break;
    case 0x38: O->Setting = 15; O->Value = v & 0x7F; O->Mask = 0x0000007F; break;
    case 0x39: O->Setting = 15; O->Value = (v & 0x7F) << 16; O->Mask = 0x007F0000; break;
    case 0x3A: O->Setting = 13; O->Value = (v & 3) << 28; O->Mask = 0x30000000; break;
    case 0x3B: O->Setting = 15; O->Value = v ? (1ul << 8) : 0; O->Mask = 1ul << 8; break;
    case 0x3C: O->Setting = 15; O->Value = v ? (1ul << 12) : 0; O->Mask = 1ul << 12; break;
    case 0x3F: O->Setting = 12; O->Value = (v & 7) << 24; O->Mask = 0x07000000; break;
    case 0x40: O->Setting = 12; O->Value = (v & 7) << 27; O->Mask = 0x38000000; break;
    case 0x43: O->Setting = 14; O->Value = (v & 7) << 28; O->Mask = 0x70000000; break;
    case 0x44: O->Setting = 2; O->Value = v ? (1ul << 31) : 0; O->Mask = 1ul << 31; break;
    case 0x46: O->Setting = 13; O->Value = v ? (1ul << 8) : 0; O->Mask = 1ul << 8; break;
    /* [Out] */
    case 0x0A: O->Setting = 6; O->Value = OaSwap2(v, 8); O->Mask = 0x00000300; break;
    case 0x0F: O->Setting = 7; O->Value = OaSwap2(v, 8); O->Mask = 0x00000300; break;
    case 0x13: O->Setting = 1; O->Value = (v & 7) << 28; O->Mask = 0x70000000; break;
    case 0x14: O->Setting = 1; O->Value = (v & 7) << 13; O->Mask = 0x0000E000; break;
    case 0x19: case 0x1A: case 0x1B: case 0x1C:
        O->Setting = 0; O->Value = v ? (1ul << (28 + Param - 0x19)) : 0; O->Mask = 1ul << (28 + Param - 0x19); break;
    case 0x1D: O->Setting = 6; O->Value = (v & 7) << 11; O->Mask = 0x0000F800; break;
    case 0x1E: O->Setting = 6; O->Value = v ? (1ul << 10) : 0; O->Mask = 1ul << 10; break;
    case 0x32: O->Setting = 11; O->Value = v ? (1ul << 4) : 0; O->Mask = 1ul << 4; break;
    case 0x42: O->Setting = 13; O->Value = (v & 7) << 13; O->Mask = 0x0000E000; break;
    default: return R_NOTSUP;
    }
    return R_OK;
}

/* Our value -> the kext's: the cue MIX source is 0 / 2 there, everything else the same. */
static ULONG KextValue(ULONG Param, LONG V)
{
    if (Param == 0x05 || Param == 0x07 || Param == 0x22 || Param == 0x24) {
        return V ? 2ul : 0ul;
    }
    return (ULONG)V;
}

static int NewResult(int r)
{
    return r == APOLLO_MON_OK ? R_OK : r == APOLLO_MON_BAD_VALUE ? R_INVALID : R_NOTSUP;
}

/* A unit's whole path: the plan filter (ioctl.c), then the encoder (mixer.c). */
static int Path(int Model, ULONG Param, LONG Value, APOLLO_MON_WRITE *W)
{
    if (!ApolloPlanMonitorParamAllowed(ApolloBringupPlanFor(Model), Param)) {
        memset(W, 0, sizeof(*W));
        return R_NOTSUP;
    }
    return NewResult(ApolloMonEncode(ApolloMonModelFor(Model), Param, Value, W));
}

static const LONG g_values[] = {
    -2000000000L, -70000L, -1000L, -256L, -200L, -129L, -128L, -122L, -121L, -120L, -119L, -101L, -100L, -99L,
    -60L, -2L, -1L,
};

static const ULONG g_oldCodes[] = { 0x01, 0x02, 0x03, 0x04, 0x10, 0x3F, 0x40, 0x43, 0x44, 0x46 };
static const ULONG g_newCodes[] = { 0x05, 0x06, 0x07, 0x08, 0x22, 0x23, 0x24, 0x25, 0x38, 0x39, 0x3A, 0x3B, 0x3C };

static int IsOldCode(ULONG p)
{
    ULONG k;
    for (k = 0; k < RTL_NUMBER_OF(g_oldCodes); k++) {
        if (g_oldCodes[k] == p) return 1;
    }
    return 0;
}

static int IsNewCode(ULONG p)
{
    ULONG k;
    for (k = 0; k < RTL_NUMBER_OF(g_newCodes); k++) {
        if (g_newCodes[k] == p) return 1;
    }
    return (p >= 0x1000 && p <= 0x3FFF) || (p >= APOLLO_MP_MIX_FIRST && p <= APOLLO_MP_MIX_LAST) ||    /* [Mix] */
           ApolloMonOutBit(p) != 0;                                                                     /* [Out] */
}

/* No monitor write may touch a preamp field (spec-dsp-mixer 5.2, spec-twinx 4.1). */
static int ClearOfPreamps(const APOLLO_MON_WRITE *W)
{
    static const struct { ULONG Setting, Forbidden; } f[] = {
        { 0, 0xFFFFFFFF }, { 1, 0xFFFFFFFF }, { 3, 0xFFFFFFFF }, { 11, 0xFFFFFFFF }, { 16, 0xFFFFFFFF },
        { 17, 0xFFFFFFFF }, { 24, 0xFFFFFFFF }, { 25, 0xFFFFFFFF }, { 12, 0x00FFFFFF }, { 13, 0x0FFFFE00 },
        { 14, 0x0FFFFFFF }, { 8, 0x000000FF },     /* S8 [7:0]: the device-mode word (0x21) */
    };
    ULONG k;

    if (W->Kind != APOLLO_MONW_SETTING) {
        return 1;
    }
    for (k = 0; k < RTL_NUMBER_OF(f); k++) {
        if (f[k].Setting == W->Setting && (W->Mask & f[k].Forbidden) != 0) {
            return 0;
        }
    }
    return W->Setting == 2 || W->Setting == 8 || W->Setting == 12 || W->Setting == 13 || W->Setting == 14 ||
           W->Setting == 15;
}

/* 1. The x8p: unchanged for every code it had */
static void CheckX8pUnchanged(void)
{
    ULONG p, n = 0, newOk = 0, bad = 0;
    LONG v;
    unsigned int k;
    char what[200];

    for (p = 0; p < 0x5000; p++) {
        for (k = 0; k < RTL_NUMBER_OF(g_values) + 260 + 3; k++) {
            MON_OUT o;
            APOLLO_MON_WRITE w;
            int ro, rn;

            if (k < RTL_NUMBER_OF(g_values)) v = g_values[k];
            else if (k < RTL_NUMBER_OF(g_values) + 260) v = (LONG)(k - RTL_NUMBER_OF(g_values));    /* 0..259 */
            else v = (k == RTL_NUMBER_OF(g_values) + 260) ? 1000L : (k == RTL_NUMBER_OF(g_values) + 261) ? 70000L : 2000000000L;
            memset(&o, 0, sizeof(o));
            ro = OldSetMonitorParam(p, v, &o);
            rn = Path(APOLLO_MODEL_X8P, p, v, &w);
            n++;
            if (IsOldCode(p)) {
                if (p == 0x04 && (v == 3 || v == 4)) {
                    if (!(ro == R_INVALID && rn == R_OK)) bad++;
                    continue;
                }
                if (ro != rn || (ro == R_OK && (w.Kind != APOLLO_MONW_SETTING || w.Setting != o.Setting ||
                                                w.Value != o.Value || w.Mask != o.Mask))) {
                    if (bad++ < 5) {
                        sprintf_s(what, sizeof(what), "x8p code 0x%lX value %ld: old %d (S%lu 0x%08lX/0x%08lX) new %d "
                                  "(S%lu 0x%08lX/0x%08lX)", p, v, ro, o.Setting, o.Value, o.Mask, rn, w.Setting, w.Value,
                                  w.Mask);
                        Check(0, what);
                    }
                }
            } else if (rn != R_NOTSUP) {
                if (!IsNewCode(p)) {
                    if (bad++ < 5) {
                        sprintf_s(what, sizeof(what), "x8p code 0x%lX value %ld: refused before, now %d, not a new code",
                                  p, v, rn);
                        Check(0, what);
                    }
                } else if (rn == R_OK) {
                    newOk++;
                }
            }
        }
    }
    sprintf_s(what, sizeof(what), "x8p: %lu (code, value) pairs, every old monitor write and error unchanged "
              "(source 3/4 now CUE 3/4); %lu accepted new ones", n, newOk);
    Check(bad == 0, what);
}

/* 2. New mailbox writes = open-apollo's kext mapping; never a preamp field */
static void CheckKextMapping(void)
{
    static const int models[2] = { APOLLO_MODEL_X8P, APOLLO_MODEL_TWINX };
    ULONG k, p, mismatches = 0, writes = 0, preamp = 0;
    LONG v;
    int m;
    char what[200];

    for (m = 0; m < 2; m++) {
        const APOLLO_MON_MODEL *M = ApolloMonModelFor(models[m]);

        for (k = 0; k < RTL_NUMBER_OF(g_newCodes) + RTL_NUMBER_OF(g_oldCodes); k++) {
            p = k < RTL_NUMBER_OF(g_newCodes) ? g_newCodes[k] : g_oldCodes[k - RTL_NUMBER_OF(g_newCodes)];
            for (v = -3; v <= 200; v++) {
                APOLLO_MON_WRITE w;
                MON_OUT o;

                if (ApolloMonEncode(M, p, v, &w) != APOLLO_MON_OK) {
                    continue;
                }
                writes++;
                if (!ClearOfPreamps(&w)) {
                    preamp++;
                }
                if (p == 0x03 || p == 0x10) {
                    continue;               /* our mute / mono split the kext's 0x03 into two switches */
                }
                if (OaMonitorSetParam(p, KextValue(p, v), &o) != R_OK || o.Setting != w.Setting ||
                    o.Value != w.Value || o.Mask != w.Mask) {
                    if (mismatches++ < 5) {
                        sprintf_s(what, sizeof(what), "%s code 0x%lX value %ld: S%lu 0x%08lX/0x%08lX, kext S%lu 0x%08lX/0x%08lX",
                                  M->Name, p, v, w.Setting, w.Value, w.Mask, o.Setting, o.Value, o.Mask);
                        Check(0, what);
                    }
                }
            }
        }
    }
    sprintf_s(what, sizeof(what), "%lu mailbox writes on both models match open-apollo's kext mapping", writes);
    Check(mismatches == 0 && writes > 100, what);
    Check(preamp == 0, "no monitor write touches a preamp field (S0/S1/S3/S11/S16/S17/S24/S25, S12 [23:0], "
                       "S13 [27:9], S14 [27:0], S8 [7:0])");
}

/* Expected values, both models */
static void Expect(int Model, ULONG Param, LONG Value, int Want, ULONG S, ULONG Val, ULONG Mask, const char *Name)
{
    APOLLO_MON_WRITE w;
    int r = Path(Model, Param, Value, &w);
    char what[200];

    sprintf_s(what, sizeof(what), "%s: %s (code 0x%lX value %ld) -> %s%s", Model == APOLLO_MODEL_X8P ? "x8p" : "Twin X",
              Name, Param, Value, Want == R_OK ? "written" : Want == R_INVALID ? "INVALID_PARAMETER" : "NOT_SUPPORTED",
              (r == Want) ? "" : " (got something else)");
    if (Want == R_OK && r == R_OK && w.Kind == APOLLO_MONW_SETTING) {
        Check(w.Setting == S && w.Value == Val && w.Mask == Mask, what);
    } else {
        Check(r == Want, what);
    }
}

#define X APOLLO_MODEL_X8P
#define T APOLLO_MODEL_TWINX

static void CheckFeatures(void)
{
    /* 1. ALT speakers: x8p ALT 1 (Line 1-2), ALT 2 (Line 3-4); Twin X ALT 1 (Line 3/4) only */
    Expect(X, 0x3B, 1, R_OK, 15, 0x100, 0x100, "ALT 1 on");
    Expect(X, 0x3C, 1, R_OK, 15, 0x1000, 0x1000, "ALT 2 on");
    Expect(X, 0x38, 60, R_OK, 15, 60, 0x7F, "ALT 1 trim 0 dB");
    Expect(X, 0x39, 120, R_OK, 15, 120ul << 16, 0x7F0000, "ALT 2 trim +30 dB");
    Expect(X, 0x39, 0, R_OK, 15, 0, 0x7F0000, "ALT 2 trim -30 dB");
    Expect(X, 0x38, 121, R_INVALID, 0, 0, 0, "ALT 1 trim above +30 dB");
    Expect(X, 0x38, -1, R_INVALID, 0, 0, 0, "ALT 1 trim below -30 dB");
    Expect(X, 0x3A, 0, R_OK, 13, 0, 0x30000000, "speakers MAIN");
    Expect(X, 0x3A, 2, R_OK, 13, 0x20000000, 0x30000000, "speakers ALT 2");
    Expect(X, 0x3A, 3, R_INVALID, 0, 0, 0, "speakers 3");
    Expect(T, 0x3B, 1, R_OK, 15, 0x100, 0x100, "ALT 1 (LINE 3/4) on");
    Expect(T, 0x38, 40, R_OK, 15, 40, 0x7F, "ALT 1 trim -10 dB");
    Expect(T, 0x3A, 1, R_OK, 13, 0x10000000, 0x30000000, "speakers ALT 1");
    Expect(T, 0x3A, 2, R_NOTSUP, 0, 0, 0, "speakers ALT 2 (one ALT)");
    Expect(T, 0x3C, 1, R_NOTSUP, 0, 0, 0, "ALT 2 on");
    Expect(T, 0x39, 60, R_NOTSUP, 0, 0, 0, "ALT 2 trim");

    /* 2. Monitor source and headphone sources */
    Expect(X, 0x04, 3, R_OK, 2, 0x000C0000, 0x200C0000, "source CUE 3");
    Expect(X, 0x04, 4, R_OK, 2, 0x20000000, 0x200C0000, "source CUE 4 (bit 29)");
    Expect(X, 0x04, 5, R_INVALID, 0, 0, 0, "source 5");
    Expect(T, 0x04, 0, R_OK, 2, 0, 0x200C0000, "source MIX");
    Expect(T, 0x04, 1, R_OK, 2, 0x00040000, 0x200C0000, "source HP (CUE 1)");
    Expect(T, 0x04, 2, R_OK, 2, 0x00080000, 0x200C0000, "source LINE 3/4 (CUE 2)");
    Expect(T, 0x04, 3, R_NOTSUP, 0, 0, 0, "source CUE 3");
    Expect(T, 0x04, 4, R_NOTSUP, 0, 0, 0, "source CUE 4");
    Expect(X, 0x3F, 1, R_OK, 12, 0x01000000, 0x07000000, "HP 1 source CUE 2");
    Expect(X, 0x40, 1, R_OK, 12, 0x08000000, 0x38000000, "HP 2 source CUE 2");
    Expect(X, 0x40, 2, R_INVALID, 0, 0, 0, "HP 2 source CUE 3 (no known encoding)");
    Expect(T, 0x3F, 1, R_OK, 12, 0x01000000, 0x07000000, "HP source LINE 3/4 (CUE 2)");
    Expect(T, 0x3F, 0, R_OK, 12, 0, 0x07000000, "HP source HP (CUE 1)");
    Expect(T, 0x40, 0, R_NOTSUP, 0, 0, 0, "HP 2 source (one headphone)");
    Expect(T, 0x43, 3, R_NOTSUP, 0, 0, 0, "dim level (unverified on the Twin X, as before)");
    Expect(T, 0x02, 140, R_OK, 2, 140ul << 8, 0xFF00, "HP level (as G4)");
    Expect(T, 0x46, 1, R_OK, 13, 0x100, 0x100, "talkback (as G5)");

    /* 3. Cue outputs: MIX source, mono */
    Expect(X, 0x05, 1, R_OK, 2, 0x02000000, 0x03000000, "CUE 1 follows the monitor mix");
    Expect(X, 0x05, 0, R_OK, 2, 0, 0x03000000, "CUE 1 its own mix");
    Expect(X, 0x06, 1, R_OK, 2, 0x00200000, 0x00300000, "CUE 1 mono");
    Expect(X, 0x08, 1, R_OK, 2, 0x00800000, 0x00C00000, "CUE 2 mono");
    Expect(X, 0x22, 1, R_OK, 8, 0x20000000, 0x30000000, "CUE 3 follows the monitor mix");
    Expect(X, 0x25, 1, R_OK, 8, 0x08000000, 0x0C000000, "CUE 4 mono");
    Expect(X, 0x05, 2, R_INVALID, 0, 0, 0, "CUE 1 MIX value 2");
    Expect(T, 0x07, 1, R_OK, 2, 0x08000000, 0x0C000000, "LINE 3/4 cue follows the monitor mix");
    Expect(T, 0x06, 1, R_OK, 2, 0x00200000, 0x00300000, "HP cue mono");
    Expect(T, 0x22, 0, R_NOTSUP, 0, 0, 0, "CUE 3 MIX");
    Expect(T, 0x25, 1, R_NOTSUP, 0, 0, 0, "CUE 4 mono");

    /* 4. Sends, pans, mutes (cues.c) */
    Expect(X, APOLLO_MP_CUE_SEND(3, 7), 0, R_OK, 0, 0, 0, "CUE 4 send of input 8");
    Expect(X, APOLLO_MP_CUE_SEND(0, 0x0A), -12, R_OK, 0, 0, 0, "CUE 1 send of the talkback");
    Expect(X, APOLLO_MP_CUE_SEND(0, 8), 0, R_INVALID, 0, 0, 0, "send of bus 8 (S/PDIF L: no input strip)");
    Expect(X, APOLLO_MP_CUE_SEND(0, 0x18), 0, R_INVALID, 0, 0, 0, "send of Virtual 1");
    Expect(X, APOLLO_MP_CUE_SEND(4, 0), 0, R_NOTSUP, 0, 0, 0, "CUE 5");
    Expect(X, APOLLO_MP_CUE_SEND(1, 0), 25, R_INVALID, 0, 0, 0, "send +12.5 dB");
    Expect(X, APOLLO_MP_CUE_SEND(1, 0), -122, R_INVALID, 0, 0, 0, "send below off");
    Expect(X, APOLLO_MP_CUE_SEND(1, 0), -121, R_OK, 0, 0, 0, "send off");
    Expect(X, APOLLO_MP_CUE_PAN(1, 0), 101, R_INVALID, 0, 0, 0, "pan 101");
    Expect(X, APOLLO_MP_CUE_PAN(1, 0), -100, R_OK, 0, 0, 0, "pan hard left");
    Expect(X, APOLLO_MP_CUE_MUTE(3), 1, R_OK, 0, 0, 0, "CUE 4 mute");
    Expect(X, APOLLO_MP_CUE_MUTE(3) | 1, 1, R_NOTSUP, 0, 0, 0, "mute with a bus");
    Expect(T, APOLLO_MP_CUE_SEND(1, 1), 0, R_OK, 0, 0, 0, "LINE 3/4 cue send of input 2");
    Expect(T, APOLLO_MP_CUE_SEND(0, 0x0A), 0, R_OK, 0, 0, 0, "HP cue send of the talkback");
    Expect(T, APOLLO_MP_CUE_SEND(0, 2), 0, R_INVALID, 0, 0, 0, "send of input 3 (two preamps)");
    Expect(T, APOLLO_MP_CUE_SEND(2, 0), 0, R_NOTSUP, 0, 0, 0, "CUE 3 send");
    Expect(T, APOLLO_MP_CUE_PAN(3, 0), 0, R_NOTSUP, 0, 0, 0, "CUE 4 pan");
    Expect(T, APOLLO_MP_CUE_MUTE(1), 1, R_OK, 0, 0, 0, "LINE 3/4 cue mute");
    Expect(T, APOLLO_MP_CUE_MUTE(2), 1, R_NOTSUP, 0, 0, 0, "CUE 3 mute");
    Expect(T, 0x4500, 0, R_NOTSUP, 0, 0, 0, "code 0x4500 (past the [Mix] family)");
    Expect(T, 0x5000, 0, R_NOTSUP, 0, 0, 0, "code 0x5000");

    /* No model, no monitor control */
    {
        APOLLO_MON_WRITE w;
        Check(ApolloMonEncode(0, APOLLO_MP_LEVEL, 100, &w) == APOLLO_MON_NOT_HERE &&
              ApolloMonModelFor(APOLLO_MODEL_NONE) == 0, "no model: nothing encoded");
    }
    {
        const APOLLO_MON_MODEL *x = ApolloMonModelFor(X), *t = ApolloMonModelFor(T);
        Check(x->Cues == 4 && x->Alts == 2 && x->Hps == 2 && x->SourceMax == 4 && x->AnalogBuses == 8 &&
              t->Cues == 2 && t->Alts == 1 && t->Hps == 1 && t->SourceMax == 2 && t->AnalogBuses == 2 &&
              x->TalkbackBus == 0x0A && t->TalkbackBus == 0x0A, "models: x8p 4 cues 2 ALT 2 HP 8 inputs; Twin X 2/1/1/2");
        Check(strcmp(t->CueName[0], "HP") == 0 && strcmp(t->CueName[1], "LINE 3/4") == 0 &&
              strcmp(t->AltOut[0], "Line 3/4") == 0 && strcmp(x->AltOut[1], "Line 3-4") == 0, "names per model");
    }
    Check(ApolloCueSub(0) == 1 && ApolloCueSub(1) == 2 && ApolloCueSub(2) == 5 && ApolloCueSub(3) == 6 &&
          ApolloCueSub(4) == 0, "cue bus params 1, 2, 5, 6 (ua_apollo.h UA_BUS_SUB_CUE1..4)");
    Check(APOLLO_DSP_CMD_BUS_PARAM == 0x001D0004ul, "bus coefficient command 0x001D0004");
}

/* 4. Coefficients */
static ULONG F32(double d)
{
    union { float f; ULONG u; } x;
    x.f = (float)d;
    return x.u;
}

static double D(ULONG u)
{
    union { float f; ULONG u; } x;
    x.u = u;
    return (double)x.f;
}

static void CheckCoefficients(void)
{
    LONG h, p;
    ULONG bad = 0, l, r;
    const double s2 = sqrt(2.0), pi = 3.14159265358979323846;
    char what[160];

    for (h = APOLLO_CUE_SEND_MIN; h <= APOLLO_CUE_SEND_MAX; h++) {
        if (ApolloMonLevelBits(h) != F32(pow(10.0, (h / 2.0) / 20.0) / s2)) bad++;
    }
    Check(bad == 0, "level table: 10^(dB/20)/sqrt2 for -60..+12 dB in 0.5 dB steps, every entry");
    Check(ApolloMonLevelBits(0) == 0x3F3504F3ul && ApolloMonLevelBits(-20) == 0x3E64F92Eul &&
          ApolloMonLevelBits(-80) == 0x3BE7B46Aul, "IOKit-captured values: 0 dB 0x3F3504F3, -10 dB 0x3E64F92E, -40 dB 0x3BE7B46A");
    Check(ApolloMonLevelBits(APOLLO_CUE_SEND_OFF) == 0 && ApolloMonLevelBits(25) == 0, "off and out of range: 0");
    bad = 0;
    for (p = -100; p < 100; p++) {
        if (ApolloMonPanBits(p) != F32(s2 * cos((p / 100.0 + 1.0) * pi / 4.0))) bad++;
    }
    Check(bad == 0 && ApolloMonPanBits(100) == 0 && ApolloMonPanBits(0) == 0x3F800000ul &&
          ApolloMonPanBits(-100) == F32(s2), "pan table: sqrt2*cos((p+1)pi/4), centre 1.0, hard right exactly 0");
    bad = 0;
    for (h = APOLLO_CUE_SEND_MIN; h <= APOLLO_CUE_SEND_MAX; h += 3) {
        for (p = -100; p <= 100; p += 7) {
            double lin = pow(10.0, (h / 2.0) / 20.0) / s2, th = (p / 100.0 + 1.0) * pi / 4.0;
            double el = lin * s2 * cos(th), er = lin * s2 * sin(th);

            ApolloMonSendCoef(h, p, 0, &l, &r);
            if (fabs(D(l) - el) > 1e-6 * (lin + 1e-30) * 4.0 || fabs(D(r) - er) > 1e-6 * (lin + 1e-30) * 4.0) {
                bad++;
            }
        }
    }
    sprintf_s(what, sizeof(what), "send = level x pan law (the engine's _write_send), every 1.5 dB and 7 %% of pan: %lu off", bad);
    Check(bad == 0, what);
    ApolloMonSendCoef(0, 0, 0, &l, &r);
    Check(l == 0x3F3504F3ul && r == 0x3F3504F3ul, "0 dB centre: both sides 0x3F3504F3 (the engine's send without pan)");
    ApolloMonSendCoef(0, -100, 0, &l, &r);
    /* the engine rounds once from double (1.0); the tables are singles, so 1 ulp below is fine (-5e-7 dB) */
    Check((l == F32(1.0) || l == F32(1.0) - 1) && r == 0, "0 dB hard left: left 1.0 (within 1 ulp), right 0");
    ApolloMonSendCoef(-12, 100, 0, &l, &r);
    Check(l == 0 && r != 0, "hard right: left exactly 0");
    ApolloMonSendCoef(0, 0, 1, &l, &r);
    Check(l == 0 && r == 0, "muted: 0.0");
    ApolloMonSendCoef(APOLLO_CUE_SEND_OFF, 0, 0, &l, &r);
    Check(l == 0 && r == 0, "off: 0.0");
}

static void CheckCueState(void)
{
    APOLLO_CUE_STATE s;
    APOLLO_MON_WRITE w;
    ULONG l, r, c, b, any = 0;

    memset(&s, 0, sizeof(s));
    for (c = 0; c < APOLLO_MON_CUES_MAX; c++) {
        for (b = 0; b < APOLLO_MON_BUSES; b++) {
            ApolloCueCoef(&s, c, b, &l, &r);
            any |= l | r;
            if (ApolloCueLevel(&s, c, b) != APOLLO_CUE_SEND_OFF) any |= 1;
        }
    }
    Check(any == 0 && s.Muted == 0, "a zeroed device context: every send off, centre, no cue muted (no init needed)");

    Check(ApolloMonEncode(ApolloMonModelFor(X), APOLLO_MP_CUE_SEND(2, 3), -12, &w) == APOLLO_MON_OK, "encode CUE 3 send");
    ApolloCueStore(&s, &w);
    Check(ApolloCueLevel(&s, 2, 3) == -12, "stored -6 dB");
    ApolloCueCoef(&s, 2, 3, &l, &r);
    Check(l == ApolloMonLevelBits(-12) && l == r, "its coefficient, centre");
    Check(ApolloMonEncode(ApolloMonModelFor(X), APOLLO_MP_CUE_PAN(2, 3), 50, &w) == APOLLO_MON_OK, "encode its pan");
    ApolloCueStore(&s, &w);
    ApolloCueCoef(&s, 2, 3, &l, &r);
    Check(D(l) < D(r) && l != 0, "pan right: right weight above left");
    Check(ApolloMonEncode(ApolloMonModelFor(X), APOLLO_MP_CUE_MUTE(2), 1, &w) == APOLLO_MON_OK, "encode CUE 3 mute");
    ApolloCueStore(&s, &w);
    ApolloCueCoef(&s, 2, 3, &l, &r);
    Check(l == 0 && r == 0 && ApolloCueLevel(&s, 2, 3) == -12, "muted: 0.0 out, the level kept");
    w.Arg = 0;
    ApolloCueStore(&s, &w);
    ApolloCueCoef(&s, 2, 3, &l, &r);
    Check(l != 0 && r != 0, "unmuted: back");
    Check(ApolloMonEncode(ApolloMonModelFor(X), APOLLO_MP_CUE_SEND(2, 3), APOLLO_CUE_SEND_MAX, &w) == APOLLO_MON_OK, "+12 dB");
    ApolloCueStore(&s, &w);
    Check(ApolloCueLevel(&s, 2, 3) == APOLLO_CUE_SEND_MAX && ApolloCueLevel(&s, 2, 4) == APOLLO_CUE_SEND_OFF, "+12 dB kept, neighbours off");
    Check(ApolloMonEncode(ApolloMonModelFor(X), APOLLO_MP_CUE_SEND(2, 3), APOLLO_CUE_SEND_OFF, &w) == APOLLO_MON_OK, "off");
    ApolloCueStore(&s, &w);
    ApolloCueCoef(&s, 2, 3, &l, &r);
    Check(ApolloCueLevel(&s, 2, 3) == APOLLO_CUE_SEND_OFF && l == 0 && r == 0, "off again: 0.0");
}

/* 5. Readback and plan bits */
static void CheckReadback(void)
{
    unsigned long w[40];
    APOLLO_MON_WRITE e;
    LONG v;
    ULONG c, ok = 1;

    memset(w, 0, sizeof(w));
    w[6] = 0xC0000020ul;                            /* the x8p, Nov 15: locked, MAIN */
    Check(ApolloRbAltSelect(w) == 0, "word 6 0xC0000020: MAIN");
    w[6] = 0xE0000111ul;
    Check(ApolloRbAltSelect(w) == 2, "word 6 [29:28] = 2: ALT 2");
    /* What the encoder writes into S2 / S12 reads back through the "if echoed" decoders */
    for (v = 0; v <= 4; v++) {
        ApolloMonEncode(ApolloMonModelFor(X), APOLLO_MP_SOURCE, v, &e);
        w[2] = e.Value;
        ok &= ApolloRbEchoSource(w) == (ULONG)v;
    }
    for (c = 0; c < 2; c++) {
        ApolloMonEncode(ApolloMonModelFor(X), c ? APOLLO_MP_CUE2_MIX : APOLLO_MP_CUE1_MIX, 1, &e);
        w[2] = e.Value;
        ok &= ApolloRbEchoCueMix(w, c) == 1;
        ApolloMonEncode(ApolloMonModelFor(X), c ? APOLLO_MP_CUE2_MONO : APOLLO_MP_CUE1_MONO, 1, &e);
        w[2] = e.Value;
        ok &= ApolloRbEchoCueMono(w, c) == 1 && ApolloRbEchoCueMix(w, c) == 0;
        ApolloMonEncode(ApolloMonModelFor(X), c ? APOLLO_MP_HP2_SOURCE : APOLLO_MP_HP1_SOURCE, 1, &e);
        w[5] = e.Value;
        ok &= ApolloRbEchoHpSource(w, c) == 1 && ApolloRbEchoHpSource(w, c ^ 1) == 0;
    }
    Check(ok != 0, "source, CUE 1-2 MIX/mono and HP 1-2 source: the encoder's bits decode back (if the unit echoes them)");
    w[5] = 0x08000020ul;                            /* the Twin X, Nov 15 (UA's driver ran it last) */
    Check(ApolloRbEchoHpSource(w, 0) == 0 && ApolloRbEchoHpSource(w, 1) == 1,
          "Twin X word 5 0x08000020: HP 1 CUE 1, HP 2 CUE 2 if word 5 echoes S12 (UA Console's defaults)");
}

static void CheckPlanBits(void)
{
    ULONG k, all = 0, ok = 1;

    for (k = 0; k < RTL_NUMBER_OF(g_newCodes); k++) {
        ULONG b = ApolloMonExtBit(g_newCodes[k]);
        ok &= b != 0 && (b & (b - 1)) == 0 && (all & b) == 0 && b >= 0x400;
        all |= b;
    }
    ok &= ApolloMonExtBit(0x1000) == APOLLO_MON_BIT_CUE_SEND && ApolloMonExtBit(0x2FFF) == APOLLO_MON_BIT_CUE_PAN &&
          ApolloMonExtBit(0x3300) == APOLLO_MON_BIT_CUE_MUTE && ApolloMonExtBit(0x4500) == 0 && ApolloMonExtBit(0x5000) == 0 &&
          ApolloMonExtBit(0x45) == 0 && ApolloMonExtBit(0xFFFFFFFFul) == 0;
    /* [Mix] five more families, each its own bit, clear of every other */
    {
        static const ULONG mixCodes[5] = { 0x4000, 0x4100, 0x4200, 0x4300, 0x4400 };
        ULONG mixAll = 0;

        for (k = 0; k < 5; k++) {
            ULONG b = ApolloMonExtBit(mixCodes[k]);
            ok &= b != 0 && (b & (b - 1)) == 0 && (all & b) == 0 && (mixAll & b) == 0 && b >= 0x400 &&
                  (b & (APOLLO_MON_BIT_CUE_SEND | APOLLO_MON_BIT_CUE_PAN | APOLLO_MON_BIT_CUE_MUTE)) == 0 &&
                  ApolloMonExtBit(mixCodes[k] | 0x0A) == b;
            mixAll |= b;
        }
        ok &= mixAll == APOLLO_MON_EXT_MIX && (APOLLO_MON_EXT_MIX & APOLLO_MON_EXT_TWINX) == 0 && mixAll != 0xFFFFFFFFul;
    }
    Check(ok != 0, "plan bits: one per new code, the three cue families and the five [Mix] families, nothing else");
    Check((APOLLO_MON_EXT_TWINX & (APOLLO_MON_BIT_CUE3_MIX | APOLLO_MON_BIT_CUE3_MONO | APOLLO_MON_BIT_CUE4_MIX |
                                   APOLLO_MON_BIT_CUE4_MONO | APOLLO_MON_BIT_ALT2_TRIM | APOLLO_MON_BIT_ALT2_ENABLE)) == 0,
          "Twin X plan: no CUE 3/4, no ALT 2");
    Check(ApolloPlanMonitorParamAllowed(ApolloBringupPlanFor(X), 0x3C) &&
          ApolloPlanMonitorParamAllowed(ApolloBringupPlanFor(T), 0x3A) &&
          !ApolloPlanMonitorParamAllowed(ApolloBringupPlanFor(T), 0x40) &&
          !ApolloPlanMonitorParamAllowed(ApolloBringupPlanFor(T), 0x43), "plan filter: x8p any, Twin X ALT select yes, HP 2 / dim level no");
}

/* ---- 6. cues.c ring waits: one APOLLO_CUE_PASS per loop ---- */

#define MS100NS(ms)     ((unsigned long long)(ms) * 10000ull)

static unsigned long long g_now;        /* the simulated KeQueryInterruptTime (100 ns) */

/* One DSP command ring: Cap entries (one always unused, as the driver's rings), Per = 100 ns per entry consumed, 0 = hung. */
typedef struct _SIM_DSP {
    unsigned long       Cap, Pending;
    unsigned long long  Per, Last;
    unsigned long       Sent, Busy;
} SIM_DSP;

static void SimUpdate(SIM_DSP *D)
{
    unsigned long long n;

    if (D->Per == 0 || D->Pending == 0) {
        D->Last = g_now;
        return;
    }
    n = (g_now - D->Last) / D->Per;
    if (n >= D->Pending) {
        D->Pending = 0;
        D->Last = g_now;
    } else {
        D->Pending -= (unsigned long)n;
        D->Last += n * D->Per;
    }
}

static unsigned long SimFree(const SIM_DSP *D)
{
    return D->Cap - 1 - D->Pending;
}

/* cues.c CuesPut's loop with the simulated ring and clock: a look, ApolloCuePutStep, a sleep of Granule. */
static int SimPut(SIM_DSP *D, unsigned long Dsp, APOLLO_CUE_PASS *P, unsigned long long Granule)
{
    unsigned long long t0 = 0, elapsed = 0;
    int waiting = 0, step;

    for (;;) {
        g_now += 20;                    /* the ring refresh: an MMIO read, about 2 us */
        SimUpdate(&D[Dsp]);
        if (waiting) {
            elapsed = g_now - t0;
        }
        step = ApolloCuePutStep(P, Dsp, SimFree(&D[Dsp]) >= 2, elapsed);
        if (step == APOLLO_CUE_PUT_SEND) {
            D[Dsp].Pending += 2;
            D[Dsp].Sent++;
            return step;
        }
        if (step == APOLLO_CUE_PUT_BUSY) {
            D[Dsp].Busy++;
            return step;
        }
        if (!waiting) {
            t0 = g_now;
            waiting = 1;
        }
        g_now += Granule;
    }
}

/* cues.c before [polish]: Wait = up to 50 sleeps of "1 ms" per send, whatever happened to the cells before. */
static int OldPut(SIM_DSP *D, unsigned long Dsp, int Wait, unsigned long long Granule)
{
    unsigned long tries = 0;

    for (;;) {
        g_now += 20;
        SimUpdate(&D[Dsp]);
        if (SimFree(&D[Dsp]) >= 2) {
            D[Dsp].Pending += 2;
            D[Dsp].Sent++;
            return APOLLO_CUE_PUT_SEND;
        }
        if (!Wait || tries >= 50) {
            D[Dsp].Busy++;
            return APOLLO_CUE_PUT_BUSY;
        }
        g_now += Granule;
        tries++;
    }
}

/* The DSP mixing bus b: the x8p's strip DSP 1 for inputs 1-8 (and a second strip DSP for 9-16 in the two-DSP case). */
static unsigned long SimOwner(unsigned long Bus, unsigned long Second)
{
    if (Bus < 8) {
        return 1;
    }
    return (Second != 0 && Bus < 16) ? Second : 0;
}

/* cues.c ApolloCuesReplay: every set cell (here all 4 x 32) to its owner and DSP 0. Old = the code before. */
static unsigned long long SimReplay(SIM_DSP *D, unsigned long Second, unsigned long long Granule, int Old, APOLLO_CUE_PASS *Out)
{
    APOLLO_CUE_PASS pass = { 0, 0 };
    unsigned long long start = g_now;
    unsigned long c, b;

    for (c = 0; c < APOLLO_MON_CUES_MAX; c++) {
        for (b = 0; b < APOLLO_MON_BUSES; b++) {
            unsigned long owner = SimOwner(b, Second);

            if (Old) {
                (void)OldPut(D, owner, 1, Granule);
                if (owner != 0) {
                    (void)OldPut(D, 0, 1, Granule);
                }
            } else {
                (void)SimPut(D, owner, &pass, Granule);
                if (owner != 0) {
                    (void)SimPut(D, 0, &pass, Granule);
                }
            }
        }
    }
    if (Out != 0) {
        *Out = pass;
    }
    return g_now - start;
}

static void SimReset(SIM_DSP *D, unsigned long N, unsigned long Cap, unsigned long long Per)
{
    unsigned long i;

    for (i = 0; i < N; i++) {
        D[i].Cap = Cap;
        D[i].Pending = 0;
        D[i].Per = Per;
        D[i].Last = g_now;
        D[i].Sent = D[i].Busy = 0;
    }
}

static void CheckCuePass(void)
{
    SIM_DSP d[6];
    APOLLO_CUE_PASS p;
    unsigned long long t, told, g;
    char msg[320];
    int gi;

    /* the pure pieces */
    p.Busy = 0;
    p.Waited = 0;
    Check(ApolloCuePassWaitLimit(0, 0) == 0 && ApolloCuePassWaitLimit(&p, 0) == MS100NS(50) &&
          ApolloCuePassWaitLimit(&p, APOLLO_CUE_PASS_DSPS) == 0, "wait limit: no pass 0, a fresh pass 50 ms, a DSP index past the mask 0");
    Check(ApolloCuePutStep(0, 0, 1, 0) == APOLLO_CUE_PUT_SEND && ApolloCuePutStep(0, 0, 0, 0) == APOLLO_CUE_PUT_BUSY,
          "no pass (a single send): room = send, full = busy at once, never a wait");
    Check(ApolloCuePutStep(&p, 3, 0, 0) == APOLLO_CUE_PUT_WAIT && ApolloCuePutStep(&p, 3, 0, MS100NS(49)) == APOLLO_CUE_PUT_WAIT &&
          p.Busy == 0 && p.Waited == 0, "a full ring: wait while under 50 ms, nothing booked yet");
    Check(ApolloCuePutStep(&p, 3, 0, MS100NS(50)) == APOLLO_CUE_PUT_BUSY && p.Busy == (1ul << 3) && p.Waited == MS100NS(50),
          "50 ms without room: busy, DSP 3 marked, the wait booked");
    Check(ApolloCuePassWaitLimit(&p, 3) == 0 && ApolloCuePutStep(&p, 3, 0, 0) == APOLLO_CUE_PUT_BUSY &&
          ApolloCuePutStep(&p, 3, 1, 0) == APOLLO_CUE_PUT_SEND && ApolloCuePassWaitLimit(&p, 2) == MS100NS(50),
          "a marked DSP: no more waits (still sent when it has room); another DSP keeps its 50 ms");
    p.Waited = MS100NS(180);
    Check(ApolloCuePassWaitLimit(&p, 2) == MS100NS(20), "the loop's 200 ms budget: 20 ms left after 180 ms of waits");
    p.Waited = MS100NS(200);
    Check(ApolloCuePassWaitLimit(&p, 2) == 0, "budget spent: no waits for any DSP");
    p.Busy = 0;
    p.Waited = 0;
    Check(ApolloCuePutStep(&p, 0, 1, MS100NS(3)) == APOLLO_CUE_PUT_SEND && p.Waited == MS100NS(3) && p.Busy == 0,
          "room after 3 ms: sent, 3 ms booked, nothing marked");

    for (gi = 0; gi < 2; gi++) {
        const char *gn = gi ? "15.6 ms sleeps" : "1 ms sleeps";

        g = gi ? 156250ull : MS100NS(1);
        /* A. the strip DSP 1 hung (full, never consumes), every other DSP healthy (1 us per entry) */
        g_now = 1000000ull;
        SimReset(d, 6, 64, 10);
        d[1].Pending = 63;
        d[1].Per = 0;
        t = SimReplay(d, 0, g, 0, &p);
        sprintf_s(msg, sizeof(msg), "%s, DSP 1 hung: the replay waits %.1f ms (one wait, <= 50 ms + one sleep), busy 0x%lx, "
                  "DSP 0 took all 128 cells (%lu), DSP 1 none (%lu turned away)", gn, (double)t / 1e4, p.Busy, d[0].Sent, d[1].Busy);
        Check(t <= MS100NS(51) + g && p.Busy == 0x2 && d[0].Sent == 128 && d[1].Sent == 0 && d[1].Busy == 32, msg);
        g_now = 1000000ull;
        SimReset(d, 6, 64, 10);
        d[1].Pending = 63;
        d[1].Per = 0;
        told = SimReplay(d, 0, g, 1, 0);
        sprintf_s(msg, sizeof(msg), "%s, DSP 1 hung: the old code waited %.0f ms (32 cells x 50 sleeps), now %.1f ms", gn,
                  (double)told / 1e4, (double)t / 1e4);
        Check(told >= 32 * 50 * g && t * 10 < told, msg);

        /* B. two strip DSPs hung (1 for inputs 1-8, 2 for 9-16): one wait each */
        g_now = 1000000ull;
        SimReset(d, 6, 64, 10);
        d[1].Pending = d[2].Pending = 63;
        d[1].Per = d[2].Per = 0;
        t = SimReplay(d, 2, g, 0, &p);
        sprintf_s(msg, sizeof(msg), "%s, DSPs 1 and 2 hung: %.1f ms (<= 2 x 50 ms + two sleeps), busy 0x%lx, DSP 0 took all 128",
                  gn, (double)t / 1e4, p.Busy);
        Check(t <= 2 * (MS100NS(50) + g) + MS100NS(1) && p.Busy == 0x6 && d[0].Sent == 128, msg);

        /* C. DSP 1 slow: two entries every 45 ms (just inside each 50 ms wait): the 200 ms budget ends the waiting */
        g_now = 1000000ull;
        SimReset(d, 6, 64, 10);
        d[1].Pending = 63;
        d[1].Per = MS100NS(45) / 2;
        t = SimReplay(d, 0, g, 0, &p);
        sprintf_s(msg, sizeof(msg), "%s, DSP 1 slow (2 entries / 45 ms): the replay waits %.1f ms in all (budget 200 ms + one wait "
                  "+ one sleep), DSP 0 took all 128", gn, (double)t / 1e4);
        Check(t <= MS100NS(201) + MS100NS(50) + g && d[0].Sent == 128 && p.Waited >= MS100NS(150), msg);

        /* D. healthy DSPs whose 1024-entry rings are nearly full (a module upload just queued), 50 entries/ms: short
              waits, every cell goes out, nobody marked */
        g_now = 1000000ull;
        SimReset(d, 6, 1024, MS100NS(1) / 50);
        d[0].Pending = d[1].Pending = 1020;
        t = SimReplay(d, 0, g, 0, &p);
        sprintf_s(msg, sizeof(msg), "%s, healthy DSPs, rings nearly full, 50 entries/ms: all 160 sends out (%lu + %lu), busy 0x%lx, "
                  "%.1f ms", gn, d[0].Sent, d[1].Sent, p.Busy, (double)t / 1e4);
        Check(d[0].Sent == 128 && d[1].Sent == 32 && d[0].Busy == 0 && d[1].Busy == 0 && p.Busy == 0, msg);
    }

    /* E. a cue's mute loop (32 buses to owner + DSP 0) and a bus move (4 cues to the owner only): one wait for the hung DSP */
    g_now = 1000000ull;
    SimReset(d, 6, 64, 10);
    d[1].Pending = 63;
    d[1].Per = 0;
    {
        APOLLO_CUE_PASS pass = { 0, 0 };
        unsigned long b, c;
        unsigned long long t0 = g_now;

        for (b = 0; b < APOLLO_MON_BUSES; b++) {
            (void)SimPut(d, SimOwner(b, 0), &pass, MS100NS(1));
            if (SimOwner(b, 0) != 0) {
                (void)SimPut(d, 0, &pass, MS100NS(1));
            }
        }
        Check(g_now - t0 <= MS100NS(51) && pass.Busy == 0x2 && d[0].Sent == 32, "mute loop, DSP 1 hung: one 50 ms wait, DSP 0 took all 32");
        pass.Busy = 0;
        pass.Waited = 0;
        t0 = g_now;
        for (c = 0; c < APOLLO_MON_CUES_MAX; c++) {
            (void)SimPut(d, 1, &pass, MS100NS(1));
        }
        Check(g_now - t0 <= MS100NS(51) && d[1].Sent == 0, "bus move to the hung DSP 1: one 50 ms wait for its 4 cells");
    }
}

/* ---- 7. [Mix] the monitor mix ---- */

#define MIX_DEF_L       0x3F3504F3ul    /* 0 dB centre, both sides: UA's default as the zero state assumes it */
#define MIX_TB_DEF      0x407E7C91ul    /* the talkback at 0 dB: 3.976353 */

/* cues.c MixApply with the DSPs replaced by L/R (what they hold now): store, targets, Set |= targets, send each. */
typedef struct _MIX_SIM {
    APOLLO_MIX_STATE    S;
    ULONG               L[32], R[32];
    ULONG               Sent;           /* the buses the last write sent */
    ULONG               Sends;          /* sends in all */
} MIX_SIM;

static int SimMix(MIX_SIM *Sim, int Model, ULONG Param, LONG Value)
{
    APOLLO_MON_WRITE w;
    APOLLO_MIX_STATE before;
    ULONG t, b;
    int r = Path(Model, Param, Value, &w);

    Sim->Sent = 0;
    if (r != R_OK) {
        return r;
    }
    if (!ApolloMonIsMix(&w)) {
        return -1;
    }
    before = Sim->S;
    ApolloMixStore(&Sim->S, &w);
    t = ApolloMixTargets(&before, &Sim->S, &w);
    Sim->S.Set |= t;
    for (b = 0; b < 32; b++) {
        if (t & (1ul << b)) {
            ApolloMixCoef(&Sim->S, b, &Sim->L[b], &Sim->R[b]);
            Sim->Sends++;
        }
    }
    Sim->Sent = t;
    return R_OK;
}

/*
 * What the unit plays equals what the state says, for every bus the model mixes: a bus the driver sent holds the
 * weights it was sent; a bus it never sent keeps the unit's own default, which must then still be what the state
 * says (0 dB centre). This is the property ApolloMixTargets exists for.
 */
static int MixConsistent(const MIX_SIM *Sim, int Model)
{
    ULONG mask = ApolloMonBusMask(ApolloMonModelFor(Model)), b, l, r;

    for (b = 0; b < 32; b++) {
        if (((mask >> b) & 1ul) == 0) {
            continue;
        }
        ApolloMixCoef(&Sim->S, b, &l, &r);
        if ((Sim->S.Set >> b) & 1ul) {
            if (Sim->L[b] != l || Sim->R[b] != r) return 0;
        } else if (b == APOLLO_MON_TALKBACK_BUS ? (l != MIX_TB_DEF || r != MIX_TB_DEF) : (l != MIX_DEF_L || r != MIX_DEF_L)) {
            return 0;
        }
    }
    return (Sim->S.Set & ~mask) == 0;
}

static void CheckMixCodes(void)
{
    APOLLO_MON_WRITE w;
    const APOLLO_MON_MODEL *x = ApolloMonModelFor(X), *t = ApolloMonModelFor(T);

    Check(ApolloMonBusMask(x) == 0x4FFul && ApolloMonBusMask(t) == 0x403ul && ApolloMonBusMask(0) == 0,
          "mixed buses: x8p ANALOG 1-8 + TALKBACK (0x4FF), Twin X ANALOG 1-2 + TALKBACK (0x403)");
    Check(APOLLO_MP_MIX_LEVEL(3) == 0x4003 && APOLLO_MP_MIX_PAN(3) == 0x4103 && APOLLO_MP_MIX_MUTE(10) == 0x420A &&
          APOLLO_MP_MIX_SOLO(0) == 0x4300 && APOLLO_MP_MIX_MUTE_ALL == 0x4400 && APOLLO_MIX_SUB == 0,
          "codes: 0x4000 level, 0x4100 pan, 0x4200 mute, 0x4300 solo, 0x4400 mute-all; bus coefficient param 0 (UA_BUS_SUB_MAIN)");

    Expect(X, APOLLO_MP_MIX_LEVEL(0), 0, R_OK, 0, 0, 0, "mix level of input 1 at 0 dB");
    Expect(X, APOLLO_MP_MIX_LEVEL(7), APOLLO_MIX_LEVEL_MAX, R_OK, 0, 0, 0, "mix level of input 8 at +12 dB");
    Expect(X, APOLLO_MP_MIX_LEVEL(7), APOLLO_MIX_LEVEL_MAX + 1, R_INVALID, 0, 0, 0, "mix level +12.5 dB");
    Expect(X, APOLLO_MP_MIX_LEVEL(2), APOLLO_MIX_LEVEL_OFF, R_OK, 0, 0, 0, "mix level off (-inf)");
    Expect(X, APOLLO_MP_MIX_LEVEL(2), APOLLO_MIX_LEVEL_OFF - 1, R_INVALID, 0, 0, 0, "mix level below off");
    Expect(X, APOLLO_MP_MIX_LEVEL(0x0A), -12, R_OK, 0, 0, 0, "mix level of the talkback");
    Expect(X, APOLLO_MP_MIX_LEVEL(8), 0, R_INVALID, 0, 0, 0, "mix level of bus 8 (S/PDIF L: no input strip)");
    Expect(X, APOLLO_MP_MIX_LEVEL(0x18), 0, R_INVALID, 0, 0, 0, "mix level of Virtual 1");
    Expect(X, APOLLO_MP_MIX_LEVEL(0xFF), 0, R_INVALID, 0, 0, 0, "mix level of bus 0xFF");
    Expect(X, APOLLO_MP_MIX_PAN(3), -100, R_OK, 0, 0, 0, "mix pan hard left");
    Expect(X, APOLLO_MP_MIX_PAN(3), 101, R_INVALID, 0, 0, 0, "mix pan 101");
    Expect(X, APOLLO_MP_MIX_PAN(0x0A), 0, R_NOTSUP, 0, 0, 0, "mix pan of the talkback (one weight, both sides)");
    Expect(X, APOLLO_MP_MIX_MUTE(4), 1, R_OK, 0, 0, 0, "mix mute of input 5");
    Expect(X, APOLLO_MP_MIX_MUTE(4), 2, R_INVALID, 0, 0, 0, "mix mute 2");
    Expect(X, APOLLO_MP_MIX_SOLO(0x0A), 1, R_OK, 0, 0, 0, "mix solo of the talkback");
    Expect(X, APOLLO_MP_MIX_SOLO(1), -1, R_INVALID, 0, 0, 0, "mix solo -1");
    Expect(X, APOLLO_MP_MIX_MUTE_ALL, 1, R_OK, 0, 0, 0, "mute all");
    Expect(X, APOLLO_MP_MIX_MUTE_ALL, 2, R_INVALID, 0, 0, 0, "mute all 2");
    Expect(X, APOLLO_MP_MIX_MUTE_ALL | 1, 1, R_NOTSUP, 0, 0, 0, "mute all with a bus");
    Expect(X, 0x44FF, 0, R_NOTSUP, 0, 0, 0, "code 0x44FF");
    Expect(T, APOLLO_MP_MIX_LEVEL(1), -24, R_OK, 0, 0, 0, "mix level of input 2");
    Expect(T, APOLLO_MP_MIX_LEVEL(2), 0, R_INVALID, 0, 0, 0, "mix level of input 3 (two preamps)");
    Expect(T, APOLLO_MP_MIX_LEVEL(0x0A), 0, R_OK, 0, 0, 0, "mix level of its talkback mic");
    Expect(T, APOLLO_MP_MIX_PAN(0), 50, R_OK, 0, 0, 0, "mix pan of input 1");
    Expect(T, APOLLO_MP_MIX_PAN(0x0A), 0, R_NOTSUP, 0, 0, 0, "mix pan of the talkback");
    Expect(T, APOLLO_MP_MIX_MUTE(0), 1, R_OK, 0, 0, 0, "mix mute of input 1 (step-mix)");
    Expect(T, APOLLO_MP_MIX_SOLO(1), 1, R_OK, 0, 0, 0, "mix solo of input 2");
    Expect(T, APOLLO_MP_MIX_MUTE_ALL, 1, R_OK, 0, 0, 0, "mute all");

    Check(ApolloMonEncode(x, APOLLO_MP_MIX_PAN(5), -40, &w) == APOLLO_MON_OK && w.Kind == APOLLO_MONW_MIX_PAN && w.Bus == 5 &&
          w.Arg == -40 && w.Buses == 0x4FF && w.Cue == 0 && ApolloMonIsMix(&w), "an x8p mix pan: kind, bus, value, the model's buses");
    Check(ApolloMonEncode(t, APOLLO_MP_MIX_MUTE_ALL, 1, &w) == APOLLO_MON_OK && w.Kind == APOLLO_MONW_MIX_MUTE_ALL &&
          w.Bus == 0 && w.Buses == 0x403 && w.Arg == 1, "Twin X mute-all: its three buses");
    Check(ApolloMonEncode(x, APOLLO_MP_CUE_SEND(0, 0), 0, &w) == APOLLO_MON_OK && !ApolloMonIsMix(&w) && w.Buses == 0,
          "a cue send is not a mix write");
    Check(ApolloPlanMonitorParamAllowed(ApolloBringupPlanFor(T), APOLLO_MP_MIX_LEVEL(0)) &&
          ApolloPlanMonitorParamAllowed(ApolloBringupPlanFor(T), APOLLO_MP_MIX_MUTE_ALL) &&
          ApolloPlanMonitorParamAllowed(ApolloBringupPlanFor(T), APOLLO_MP_MIX_SOLO(0x0A)) &&
          !ApolloPlanMonitorParamAllowed(ApolloBringupPlanFor(T), 0x4500) &&
          ApolloPlanMonitorParamAllowed(ApolloBringupPlanFor(X), APOLLO_MP_MIX_PAN(7)), "plan filter: both units take the mix families");
}

static void CheckMixWeights(void)
{
    APOLLO_MIX_STATE s;
    LONG h, p;
    ULONG bad = 0, l, r, l0, r0, b;
    const double pi = 3.14159265358979323846;
    char what[200];

    for (h = APOLLO_CUE_SEND_MIN; h <= APOLLO_CUE_SEND_MAX; h++) {
        if (ApolloMixTalkbackBits(h) != F32(pow(10.0, (h / 2.0) / 20.0) * 3.976353)) bad++;
    }
    Check(bad == 0 && ApolloMixTalkbackBits(0) == MIX_TB_DEF && ApolloMixTalkbackBits(APOLLO_MIX_LEVEL_OFF) == 0 &&
          ApolloMixTalkbackBits(25) == 0, "talkback table: 10^(dB/20) x 3.976353 (TALKBACK_UNITY) every half dB, 0 dB 3.976353, off 0");

    /* every level and pan: ApolloMixCoef = the engine's encode_mix_coeff / encode_mix_coeff_r (hardware.py:1041-1068) */
    memset(&s, 0, sizeof(s));
    bad = 0;
    for (h = APOLLO_MIX_LEVEL_OFF + 1; h <= APOLLO_MIX_LEVEL_MAX; h++) {
        for (p = -100; p <= 100; p++) {
            double db = h / 2.0, th = (p / 100.0 + 1.0) * pi / 4.0;
            double el = pow(10.0, db / 20.0) * cos(th), er = pow(10.0, db / 20.0) * sin(th), tol = 4e-7 * pow(10.0, db / 20.0);

            s.Level[3] = (signed char)h;
            s.Pan[3] = (signed char)p;
            ApolloMixCoef(&s, 3, &l, &r);
            if (fabs(D(l) - el) > tol || fabs(D(r) - er) > tol) bad++;
        }
    }
    sprintf_s(what, sizeof(what), "mix weights = 10^(dB/20) cos/sin((p+1)pi/4) (encode_mix_coeff/_r) for all %ld levels x 201 pans: %lu off",
              (long)(APOLLO_MIX_LEVEL_MAX - APOLLO_MIX_LEVEL_OFF), bad);
    Check(bad == 0, what);
    memset(&s, 0, sizeof(s));
    ApolloMixCoef(&s, 0, &l, &r);
    Check(l == 0x3F3504F3ul && r == 0x3F3504F3ul, "0 dB centre: 0x3F3504F3 both sides (IOKit-captured, the engine's 0 dB fader)");
    s.Level[0] = -20;
    ApolloMixCoef(&s, 0, &l, &r);
    s.Level[0] = -80;
    ApolloMixCoef(&s, 0, &l0, &r0);
    Check(l == 0x3E64F92Eul && r == l && l0 == 0x3BE7B46Aul, "-10 dB 0x3E64F92E and -40 dB 0x3BE7B46A (IOKit-captured)");
    s.Level[0] = -24;
    ApolloMixCoef(&s, 0, &l, &r);
    s.Level[0] = 0;
    ApolloMixCoef(&s, 0, &l0, &r0);
    sprintf_s(what, sizeof(what), "a -12 dB fader drops the input's weight by %.4f dB on both sides (step-mix checks about 12)",
              20.0 * log10(D(l) / D(l0)));
    Check(fabs(20.0 * log10(D(l) / D(l0)) + 12.0) < 0.001 && fabs(20.0 * log10(D(r) / D(r0)) + 12.0) < 0.001, what);
    s.Level[0] = APOLLO_MIX_LEVEL_OFF;
    ApolloMixCoef(&s, 0, &l, &r);
    Check(l == 0 && r == 0, "level off: 0.0");
    s.Level[0] = 0;
    s.Pan[0] = 100;
    ApolloMixCoef(&s, 0, &l, &r);
    Check(l == 0 && (r == F32(1.0) || r == F32(1.0) - 1), "0 dB hard right: left exactly 0, right 1.0 (within 1 ulp)");
    s.Pan[0] = 0;
    s.Muted = 1;
    ApolloMixCoef(&s, 0, &l, &r);
    Check(l == 0 && r == 0, "muted: both weights 0.0 (hardware.py Mute: set_bus_fader(bus, 0, 0, 0))");
    /* the talkback: its own law, L = R, a stored pan ignored */
    memset(&s, 0, sizeof(s));
    s.Pan[APOLLO_MON_TALKBACK_BUS] = -100;
    s.Level[APOLLO_MON_TALKBACK_BUS] = -12;
    ApolloMixCoef(&s, APOLLO_MON_TALKBACK_BUS, &l, &r);
    Check(l == r && l == ApolloMixTalkbackBits(-12) && fabs(D(l) - pow(10.0, -6.0 / 20.0) * 3.976353) < 2e-6,
          "talkback -6 dB: 10^(-6/20) x 3.976353 on both sides, no pan");
    /* the zeroed state: UA's default everywhere, nothing set */
    memset(&s, 0, sizeof(s));
    bad = 0;
    for (b = 0; b < 32; b++) {
        ApolloMixCoef(&s, b, &l, &r);
        if (ApolloMixSilent(&s, b) || ApolloMixLevel(&s, b) != 0 ||
            (b == APOLLO_MON_TALKBACK_BUS ? (l != MIX_TB_DEF || r != MIX_TB_DEF) : (l != MIX_DEF_L || r != MIX_DEF_L))) bad++;
    }
    Check(bad == 0 && s.Set == 0, "a zeroed device context: every bus on at 0 dB, centre, nothing muted or soloed, nothing set");
    {
        APOLLO_CUE_STATE c;
        memset(&c, 0, sizeof(c));
        Check(c.Mix.Set == 0 && c.Mix.Muted == 0 && c.Mix.Soloed == 0 && sizeof(c.Mix.Level) == 32 && sizeof(c.Mix.Pan) == 32,
              "the mix rides in APOLLO_CUE_STATE (Ctx->Cues.Mix), zero = nothing to replay");
    }
}

static void CheckMixLogic(void)
{
    MIX_SIM m;
    ULONG l, r;
    char what[200];
    unsigned long seed = 12345, n, bad = 0;

    /* x8p unchanged: nothing set, so the connect replay and a strip's bus move send no mix cell */
    memset(&m, 0, sizeof(m));
    Check(m.S.Set == 0 && MixConsistent(&m, X), "x8p, nothing sent: no mix cell to replay or move; the unit keeps its default");

    /* a fader: just that bus, even at the value the unit had (from then on the driver's value is the unit's) */
    Check(SimMix(&m, X, APOLLO_MP_MIX_LEVEL(0), 0) == R_OK && m.Sent == 0x1 && m.L[0] == MIX_DEF_L && MixConsistent(&m, X),
          "input 1 set to 0 dB: bus 0 sent (0x3F3504F3), nothing else");
    Check(SimMix(&m, X, APOLLO_MP_MIX_LEVEL(2), -12) == R_OK && m.Sent == 0x4 && MixConsistent(&m, X), "input 3 at -6 dB: bus 2 only");
    Check(SimMix(&m, X, APOLLO_MP_MIX_PAN(2), 0) == R_OK && m.Sent == 0x4, "a pan write sends its bus even when nothing changed");

    /* solo: host logic. The others go out as 0.0; the soloed bus itself is not sent (its weights do not change). */
    Check(SimMix(&m, X, APOLLO_MP_MIX_SOLO(1), 1) == R_OK && m.Sent == (0x4FFul & ~0x2ul) && MixConsistent(&m, X) &&
          m.L[0] == 0 && m.L[2] == 0 && m.L[10] == 0 && !(m.S.Set & 0x2), "solo input 2: every other bus (and the talkback) 0.0, input 2 untouched");
    Check(SimMix(&m, X, APOLLO_MP_MIX_LEVEL(2), -24) == R_OK && m.Sent == 0x4 && m.L[2] == 0 && m.S.Level[2] == -24,
          "a fader on a solo-silenced input: kept, still 0.0");
    Check(SimMix(&m, X, APOLLO_MP_MIX_SOLO(3), 1) == R_OK && m.Sent == 0x8 && m.L[3] == MIX_DEF_L && MixConsistent(&m, X),
          "solo input 4 as well: it comes back, nothing else moves");
    Check(SimMix(&m, X, APOLLO_MP_MIX_MUTE(3), 1) == R_OK && m.Sent == 0x8 && m.L[3] == 0 && MixConsistent(&m, X),
          "mute wins over solo: input 4 soloed and muted is 0.0");
    Check(SimMix(&m, X, APOLLO_MP_MIX_SOLO(1), 0) == R_OK && m.Sent == 0x2 && m.L[1] == 0 && MixConsistent(&m, X),
          "unsolo input 2 while input 4 stays soloed: input 2 now silenced");
    Check(SimMix(&m, X, APOLLO_MP_MIX_SOLO(3), 0) == R_OK && m.Sent == (0x4FFul & ~0x8ul) && MixConsistent(&m, X) &&
          m.L[3] == 0 && m.L[2] == ApolloMonLevelBits(-24) && m.L[0] == MIX_DEF_L && m.L[10] == MIX_TB_DEF,
          "the last solo ends: every bus back to what was set (input 3 at -12 dB kept through the solo), input 4 still muted");
    Check(SimMix(&m, X, APOLLO_MP_MIX_MUTE(3), 0) == R_OK && m.Sent == 0x8 && m.L[3] == MIX_DEF_L && m.S.Muted == 0,
          "unmute input 4: back at 0 dB");

    /* mute-all: every bus 0.0 at once, re-sent even where already 0.0; unmute-all brings back what was set */
    Check(SimMix(&m, X, APOLLO_MP_MIX_MUTE(5), 1) == R_OK && m.S.Muted == 0x20, "input 6 muted on its own");
    Check(SimMix(&m, X, APOLLO_MP_MIX_MUTE_ALL, 1) == R_OK && m.Sent == 0x4FF && m.S.Muted == 0x4FF && MixConsistent(&m, X) &&
          m.L[0] == 0 && m.R[7] == 0 && m.L[10] == 0, "mute all (x8p): all nine buses sent 0.0, input 6's 0.0 again too");
    Check(SimMix(&m, X, APOLLO_MP_MIX_LEVEL(0), 6) == R_OK && m.L[0] == 0 && m.S.Level[0] == 6, "a fader while all are muted: kept, 0.0");
    Check(SimMix(&m, X, APOLLO_MP_MIX_MUTE_ALL, 0) == R_OK && m.Sent == 0x4FF && m.S.Muted == 0 && MixConsistent(&m, X) &&
          m.L[0] == ApolloMonLevelBits(6) && m.L[2] == ApolloMonLevelBits(-24),
          "unmute all: every bus back with its level (+3 dB, -12 dB kept); input 6's own mute goes too");

    /* Twin X: its three buses; a solo of input 1 silences input 2 and the talkback mic */
    memset(&m, 0, sizeof(m));
    Check(SimMix(&m, T, APOLLO_MP_MIX_SOLO(0), 1) == R_OK && m.Sent == 0x402 && m.L[1] == 0 && m.L[10] == 0 && MixConsistent(&m, T),
          "Twin X solo input 1: input 2 and the talkback 0.0, input 1 not sent");
    Check(SimMix(&m, T, APOLLO_MP_MIX_SOLO(0), 0) == R_OK && m.Sent == 0x402 && m.L[1] == MIX_DEF_L && m.L[10] == MIX_TB_DEF,
          "unsolo: both back at 0 dB (now set, so replayed after a connect)");
    Check(SimMix(&m, T, APOLLO_MP_MIX_MUTE_ALL, 1) == R_OK && m.Sent == 0x403 && m.S.Muted == 0x403, "Twin X mute all: three buses");
    Check(SimMix(&m, T, APOLLO_MP_MIX_MUTE(2), 1) == R_INVALID && m.Sent == 0, "Twin X input 3: refused, nothing sent");

    /* step-mix on the Twin X: mute input 1, unmute, then -12 dB */
    memset(&m, 0, sizeof(m));
    Check(SimMix(&m, T, APOLLO_MP_MIX_MUTE(0), 1) == R_OK && m.Sent == 0x1 && m.L[0] == 0 && m.R[0] == 0 && !(m.S.Set & 0x2),
          "step-mix: mute input 1 sends 0.0 for bus 0 only (input 2 keeps the unit's default)");
    Check(SimMix(&m, T, APOLLO_MP_MIX_MUTE(0), 0) == R_OK && m.L[0] == MIX_DEF_L && m.R[0] == MIX_DEF_L, "step-mix: unmute, 0 dB centre");
    Check(SimMix(&m, T, APOLLO_MP_MIX_LEVEL(0), -24) == R_OK && m.L[0] == ApolloMonLevelBits(-24) &&
          fabs(20.0 * log10(D(m.L[0]) / D(MIX_DEF_L)) + 12.0) < 0.001, "step-mix: fader -12 dB, the weight 12.00 dB down");

    /* a linked pair as upstream writes it: the leader's level on both buses, Pan -1 / Pan2 +1 (link_tree) */
    memset(&m, 0, sizeof(m));
    (void)SimMix(&m, X, APOLLO_MP_MIX_PAN(0), -100);
    (void)SimMix(&m, X, APOLLO_MP_MIX_PAN(1), 100);
    (void)SimMix(&m, X, APOLLO_MP_MIX_LEVEL(0), -12);
    (void)SimMix(&m, X, APOLLO_MP_MIX_LEVEL(1), -12);
    l = m.L[0];
    r = m.R[1];
    sprintf_s(what, sizeof(what), "linked 1-2 at -6 dB: bus 0 left %.7f right 0, bus 1 left 0 right %.7f = 10^(-6/20) "
              "(_write_partner_bus with Pan -1 / Pan2 +1)", D(l), D(r));
    Check(m.R[0] == 0 && m.L[1] == 0 && fabs(D(l) - pow(10.0, -6.0 / 20.0)) < 2e-7 && l == r && MixConsistent(&m, X), what);

    /* 20000 random writes on both models: the unit always plays what the state says, nothing beyond the model's buses */
    {
        MIX_SIM sims[2];
        ULONG accepted = 0;

        for (n = 0; n < 20000; n++) {
            int model = (n & 1) ? X : T;
            MIX_SIM *sim = &sims[n & 1];
            ULONG kind, bus, code;
            LONG v;

            if ((n % 4000) < 2) {
                memset(sim, 0, sizeof(*sim));
            }
            seed = seed * 1103515245ul + 12345ul;
            kind = (seed >> 16) % 5;
            seed = seed * 1103515245ul + 12345ul;
            bus = (seed >> 16) % 12;
            seed = seed * 1103515245ul + 12345ul;
            v = (LONG)((seed >> 16) % 300) - 150;
            code = kind == 4 ? APOLLO_MP_MIX_MUTE_ALL : (0x4000ul | (kind << 8) | bus);
            if (kind >= 2) {
                v = (v & 1) ? 1 : 0;
            }
            if (SimMix(sim, model, code, v) == R_OK) {
                accepted++;
            }
            if (!MixConsistent(sim, model)) bad++;
        }
        sprintf_s(what, sizeof(what), "20000 random mix writes (%lu accepted: levels, pans, mutes, solos, mute-all) on both models: "
                  "the unit plays the state on every bus after each one (%lu not)", accepted, bad);
        Check(bad == 0 && accepted > 5000, what);
    }
}

/* cues.c ApolloCuesReplay with the mix: the set mix buses first (here all nine of the x8p), then every cue cell. */
static unsigned long long SimReplayMix(SIM_DSP *D, unsigned long long Granule, APOLLO_CUE_PASS *Out, unsigned long *MixOut)
{
    APOLLO_CUE_PASS pass = { 0, 0 };
    unsigned long long start = g_now;
    unsigned long c, b, mix = 0;

    for (b = 0; b < APOLLO_MON_BUSES; b++) {
        unsigned long owner;

        if ((0x4FFul & (1ul << b)) == 0) {
            continue;
        }
        owner = SimOwner(b, 0);
        if (SimPut(D, owner, &pass, Granule) == APOLLO_CUE_PUT_SEND && owner == 0) {
            mix++;
        }
        if (owner != 0 && SimPut(D, 0, &pass, Granule) == APOLLO_CUE_PUT_SEND) {
            mix++;
        }
    }
    for (c = 0; c < APOLLO_MON_CUES_MAX; c++) {
        for (b = 0; b < APOLLO_MON_BUSES; b++) {
            unsigned long owner = SimOwner(b, 0);

            (void)SimPut(D, owner, &pass, Granule);
            if (owner != 0) {
                (void)SimPut(D, 0, &pass, Granule);
            }
        }
    }
    *Out = pass;
    *MixOut = mix;
    return g_now - start;
}

static void CheckMixPass(void)
{
    SIM_DSP d[6];
    APOLLO_CUE_PASS p;
    unsigned long long t;
    unsigned long mix = 0;
    char msg[240];

    g_now = 1000000ull;
    SimReset(d, 6, 64, 10);
    d[1].Pending = 63;
    d[1].Per = 0;
    t = SimReplayMix(d, MS100NS(1), &p, &mix);
    sprintf_s(msg, sizeof(msg), "replay of 9 mix + 128 cue cells, the strip DSP 1 hung: one wait (%.1f ms), DSP 0 took all 9 mix "
              "cells (%lu) and 128 cue cells", (double)t / 1e4, mix);
    Check(t <= MS100NS(51) + MS100NS(1) && p.Busy == 0x2 && mix == 9 && d[0].Sent == 9 + 128 && d[1].Sent == 0, msg);
}

/*
 * 8. [Aux] The aux buses: the codes per model through the plan filter, the weights against upstream's laws (libm):
 * pre-fader send = encode_input_fader(send) with the send pan, post-fader = encode_input_fader(send + fader) and 0.0
 * when the input is muted, solo-silenced or its fader is off, the return = encode_aux_fader (10^(dB/20), no sqrt2),
 * aux to cue = encode_aux_fader with the pan law and the cue's mute; the zero state sends nothing.
 */
static int Near(ULONG Got, double Want)
{
    double g = D(Got);

    if (fabs(Want) < 1e-12) {
        return Got == 0 || fabs(g) < 1e-12;
    }
    return fabs(g - Want) <= fabs(Want) * 4e-7;      /* two rounding steps of a float */
}

static void CheckAux(void)
{
    APOLLO_MON_WRITE w;
    APOLLO_CUE_STATE st;
    ULONG l, r, bad = 0, a, b;
    LONG h, f, p;
    const double s2 = sqrt(2.0), pi = 3.14159265358979323846;

    /* codes: both models, every input bus the model mixes and the talkback; cues per model; malformed refused */
    for (a = 0; a < 2; a++) {
        Check(Path(X, APOLLO_MP_AUX_SEND(a, 7), -20, &w) == R_OK && w.Kind == APOLLO_MONW_AUX_SEND && w.Aux == a && w.Bus == 7,
              "aux: x8p input 8's send to the aux");
        Check(Path(T, APOLLO_MP_AUX_SEND(a, 1), 0, &w) == R_OK && Path(T, APOLLO_MP_AUX_SEND(a, 0x0A), 0, &w) == R_OK &&
              Path(T, APOLLO_MP_AUX_SEND(a, 2), 0, &w) == R_INVALID, "aux: Twin X sends from inputs 1-2 and the talkback only");
        Check(Path(X, APOLLO_MP_AUX_PAN(a, 0), 100, &w) == R_OK && Path(X, APOLLO_MP_AUX_PAN(a, 0), 101, &w) == R_INVALID &&
              Path(X, APOLLO_MP_AUX_POST(a, 0), 1, &w) == R_OK && Path(X, APOLLO_MP_AUX_POST(a, 0), 2, &w) == R_INVALID,
              "aux: pan and post ranges");
        Check(Path(X, APOLLO_MP_AUX_RETURN(a), -121, &w) == R_OK && Path(X, APOLLO_MP_AUX_RETURN(a), 25, &w) == R_INVALID &&
              Path(T, APOLLO_MP_AUX_MUTE(a), 1, &w) == R_OK && Path(X, APOLLO_MP_AUX_RETURN(a) | 1, 0, &w) == R_NOTSUP,
              "aux: return level and mute");
        Check(Path(X, APOLLO_MP_AUX_CUE(a, 3), 0, &w) == R_OK && Path(T, APOLLO_MP_AUX_CUE(a, 1), 0, &w) == R_OK &&
              Path(T, APOLLO_MP_AUX_CUE(a, 2), 0, &w) == R_NOTSUP && Path(X, APOLLO_MP_AUX_CUE_PAN(a, 0), -100, &w) == R_OK,
              "aux: returns into the cues the model has");
    }
    Check(Path(X, 0x6040, 0, &w) == R_NOTSUP && Path(X, 0x6700, 0, &w) == R_NOTSUP && Path(T, 0x6080, 0, &w) == R_NOTSUP &&
          ApolloMonExtBit(0x6000) == APOLLO_MON_BIT_AUX && ApolloMonExtBit(0x66FF) == APOLLO_MON_BIT_AUX &&
          ApolloMonExtBit(0x6700) == 0 && (APOLLO_MON_BIT_AUX & (APOLLO_MON_EXT_TWINX | APOLLO_MON_EXT_MIX)) == 0,
          "aux: bits 6-7 and kinds 7+ refused, one plan bit of its own");
    Check(APOLLO_AUX_SUB(0) == 3 && APOLLO_AUX_SUB(1) == 4 && APOLLO_MON_AUX_RETURN_BUS(0) == 0x10 &&
          APOLLO_MON_AUX_RETURN_BUS(1) == 0x12, "aux: params 3/4 (UA_BUS_SUB_AUX1/2), return buses 0x10/0x12");

    /* weights */
    memset(&st, 0, sizeof(st));
    for (h = APOLLO_CUE_SEND_MIN; h <= APOLLO_CUE_SEND_MAX; h += 7) {
        for (p = -100; p <= 100; p += 25) {
            double lin = pow(10.0, h / 40.0) / s2, th = (p / 100.0 + 1.0) * pi / 4.0;

            st.Aux.Level[1][2] = (unsigned char)(h + 122);
            st.Aux.Pan[1][2] = (signed char)p;
            st.Aux.Post[1] = 0;
            ApolloAuxSendCoef(&st, 1, 2, &l, &r);
            bad += !Near(l, lin * s2 * cos(th)) || !Near(r, lin * s2 * sin(th));
            for (f = -40; f <= 24; f += 16) {
                double post = pow(10.0, (h + f) / 40.0) / s2;

                st.Mix.Level[2] = (signed char)f;
                st.Aux.Post[1] = 1ul << 2;
                ApolloAuxSendCoef(&st, 1, 2, &l, &r);
                bad += !Near(l, post * s2 * cos(th)) || !Near(r, post * s2 * sin(th));
            }
            st.Mix.Level[2] = 0;
        }
    }
    Check(bad == 0, "aux: pre-fader = encode_input_fader(send) x pan, post-fader = encode_input_fader(send + fader) x pan");
    st.Aux.Level[0][1] = 122;           /* 0 dB */
    st.Aux.Post[0] = 1ul << 1;
    ApolloAuxSendCoef(&st, 0, 1, &l, &r);
    Check(l == 0x3F3504F3ul && r == 0x3F3504F3ul, "aux: post-fader 0 dB send at a 0 dB fader, centre = 0x3F3504F3 (as a cue send)");
    st.Mix.Muted = 1ul << 1;
    ApolloAuxSendCoef(&st, 0, 1, &l, &r);
    Check(l == 0 && r == 0, "aux: a muted input's post-fader send is 0.0");
    st.Aux.Post[0] = 0;
    ApolloAuxSendCoef(&st, 0, 1, &l, &r);
    Check(l == 0x3F3504F3ul, "aux: a pre-fader send ignores the mute");
    st.Mix.Muted = 0;
    st.Mix.Soloed = 1ul << 3;
    st.Aux.Post[0] = 1ul << 1;
    ApolloAuxSendCoef(&st, 0, 1, &l, &r);
    Check(l == 0 && r == 0, "aux: a solo elsewhere silences a post-fader send");
    st.Mix.Soloed = 0;
    st.Mix.Level[1] = (signed char)APOLLO_MIX_LEVEL_OFF;
    ApolloAuxSendCoef(&st, 0, 1, &l, &r);
    Check(l == 0, "aux: a fader at -inf silences a post-fader send");
    st.Mix.Level[1] = 0;
    st.Aux.Level[0][1] = 0;
    ApolloAuxSendCoef(&st, 0, 1, &l, &r);
    Check(l == 0 && r == 0, "aux: a send that is off is 0.0");

    for (bad = 0, h = APOLLO_CUE_SEND_MIN; h <= APOLLO_CUE_SEND_MAX; h++) {
        st.Aux.RetLevel[1] = (signed char)h;
        bad += !Near(ApolloAuxReturnBits(&st.Aux, 1), pow(10.0, h / 40.0));
    }
    Check(bad == 0, "aux: the return = encode_aux_fader (10^(dB/20), no sqrt2), every half-dB");
    st.Aux.RetLevel[1] = 0;
    Check(Near(ApolloAuxReturnBits(&st.Aux, 1), 1.0) && ApolloAuxReturnBits(&st.Aux, 1) == F32(1.0),
          "aux: the zero state's return is 0 dB = 1.0 (IOKit: -10 dB 0.316228)");
    st.Aux.RetLevel[0] = -20;
    Check(fabs(D(ApolloAuxReturnBits(&st.Aux, 0)) - 0.316228) < 1e-6, "aux: return -10 dB = 0.316228 (hardware.py IOKit capture)");
    st.Aux.RetMuted = 1;
    Check(ApolloAuxReturnBits(&st.Aux, 0) == 0, "aux: a muted return is 0.0");
    st.Aux.RetLevel[0] = (signed char)APOLLO_CUE_SEND_OFF;
    st.Aux.RetMuted = 0;
    Check(ApolloAuxReturnBits(&st.Aux, 0) == 0, "aux: a return at -inf is 0.0");

    for (bad = 0, h = APOLLO_CUE_SEND_MIN; h <= APOLLO_CUE_SEND_MAX; h += 5) {
        for (p = -100; p <= 100; p += 10) {
            double th = (p / 100.0 + 1.0) * pi / 4.0, lin = pow(10.0, h / 40.0);

            st.Aux.CueLevel[0][1] = (unsigned char)(h + 122);
            st.Aux.CuePan[0][1] = (signed char)p;
            ApolloAuxCueCoef(&st, 0, 1, &l, &r);
            bad += !Near(l, lin * s2 * cos(th)) || !Near(r, lin * s2 * sin(th));
        }
    }
    Check(bad == 0, "aux: return into a cue = encode_aux_fader x the pan law (_write_send)");
    st.Muted = 1ul << 1;
    ApolloAuxCueCoef(&st, 0, 1, &l, &r);
    Check(l == 0 && r == 0, "aux: a muted cue silences the aux return into it");

    /* store and the zero state */
    memset(&st, 0, sizeof(st));
    for (a = 0, bad = 0; a < 2; a++) {
        for (b = 0; b < 32; b++) {
            ApolloAuxSendCoef(&st, a, b, &l, &r);
            bad += l != 0 || r != 0;
        }
        bad += st.Aux.Set[a] != 0 || st.Aux.CueSet[a] != 0 || st.Aux.Owner[a] != 0;
    }
    Check(bad == 0 && st.Aux.RetSet == 0, "aux: the zeroed state sends nothing (every send off and pre-fader, nothing set)");
    Check(ApolloMonEncode(ApolloMonModelFor(X), APOLLO_MP_AUX_SEND(1, 4), -12, &w) == APOLLO_MON_OK, "aux: encode a send");
    ApolloAuxStore(&st.Aux, &w);
    Check(ApolloAuxLevel(&st.Aux, 1, 4) == -12 && ApolloAuxLevel(&st.Aux, 0, 4) == APOLLO_CUE_SEND_OFF, "aux: store a send level");
    (void)ApolloMonEncode(ApolloMonModelFor(X), APOLLO_MP_AUX_POST(1, 4), 1, &w);
    ApolloAuxStore(&st.Aux, &w);
    (void)ApolloMonEncode(ApolloMonModelFor(X), APOLLO_MP_AUX_MUTE(0), 1, &w);
    ApolloAuxStore(&st.Aux, &w);
    (void)ApolloMonEncode(ApolloMonModelFor(X), APOLLO_MP_AUX_CUE_PAN(1, 2), -40, &w);
    ApolloAuxStore(&st.Aux, &w);
    Check(st.Aux.Post[1] == (1ul << 4) && st.Aux.RetMuted == 1 && st.Aux.CuePan[1][2] == -40, "aux: store post, mute, cue pan");

    /*
     * [AuxStrips] The cells cues.c sends (docs/STRIPS.md 14). Without the return's strips: exactly the one write of every
     * earlier build (the hardware sees what it saw). With them: a stereo pair, both return inputs, the weight on the L
     * input's left entry and the R input's right entry, 0.0 on the other two, 0.0 everywhere while muted or never set.
     */
    {
        APOLLO_AUX_CELL c[2];
        ULONG cue, set;
        LONG mute;

        memset(&st, 0, sizeof(st));
        Check(st.Aux.Strips == 0, "aux strips: the zero state has none (every return cell the single write of before)");
        for (bad = 0, a = 0; a < 2; a++) {
            for (h = APOLLO_CUE_SEND_OFF; h <= APOLLO_CUE_SEND_MAX; h++) {
                for (mute = 0; mute <= 1; mute++) {
                    for (set = 0; set <= 1; set++) {
                        st.Aux.RetLevel[a] = (signed char)h;
                        st.Aux.RetMuted = (ULONG)mute << a;
                        st.Aux.RetSet = set << a;
                        bad += ApolloAuxReturnCells(&st.Aux, a, c) != 1 || c[0].Bus != 0x10ul + 2 * a || c[0].Sub != 0 ||
                               c[0].Left != ApolloAuxReturnBits(&st.Aux, a) || c[0].Right != 0 || !c[0].LeftOnly;
                    }
                }
            }
        }
        Check(bad == 0, "aux strips off: the return is the single left-entry write of before (bus 0x10/0x12, param 0, 10^(dB/20)), "
                        "every level, mute and set state");
        for (bad = 0, a = 0; a < 2; a++) {
            for (cue = 0; cue < 4; cue++) {
                for (h = APOLLO_CUE_SEND_MIN; h <= APOLLO_CUE_SEND_MAX; h += 9) {
                    for (p = -100; p <= 100; p += 50) {
                        for (mute = 0; mute <= 1; mute++) {
                            st.Aux.CueLevel[a][cue] = (unsigned char)(h + 122);
                            st.Aux.CuePan[a][cue] = (signed char)p;
                            st.Aux.CueSet[a] = (h & 1) ? (1ul << cue) : 0;
                            st.Muted = (ULONG)mute << cue;
                            ApolloAuxCueCoef(&st, a, cue, &l, &r);
                            bad += ApolloAuxCueCells(&st, a, cue, c) != 1 || c[0].Bus != 0x10ul + 2 * a ||
                                   c[0].Sub != ApolloCueSub(cue) || c[0].Left != l || c[0].Right != r || c[0].LeftOnly;
                        }
                    }
                }
            }
        }
        Check(bad == 0, "aux strips off: a return-to-cue send is the one {0x10/0x12, cue param, L, R} write of before");
        Check(ApolloAuxReturnCells(&st.Aux, 2, c) == 0 && ApolloAuxCueCells(&st, 2, 0, c) == 0 && ApolloAuxCueCells(&st, 0, 4, c) == 0,
              "aux strips: a third aux or a fifth cue: no write");

        /* AUX 1's strips run, AUX 2's do not */
        memset(&st, 0, sizeof(st));
        st.Aux.Strips = 1;
        st.Aux.RetSet = 3;
        st.Aux.RetLevel[0] = -20;                       /* -10 dB */
        Check(ApolloAuxStereo(&st.Aux, 0) && !ApolloAuxStereo(&st.Aux, 1) && !ApolloAuxStereo(&st.Aux, 2), "aux strips: per return");
        /* Nov 16 2026: strips on the L returns only (a mono aux bus): one cell {L input, 0, g, g}, both monitor sides */
        Check(ApolloAuxReturnCells(&st.Aux, 0, c) == 1 && c[0].Bus == 0x10 && c[0].Sub == 0 && !c[0].LeftOnly &&
              c[0].Left == ApolloAuxReturnBits(&st.Aux, 0) && fabs(D(c[0].Left) - 0.316228) < 1e-6 && c[0].Right == c[0].Left,
              "aux strips on: the return is one cell on the L input, both sides: 0x10 {L 0.316228 (-10 dB), R 0.316228}");
        Check(ApolloAuxReturnCells(&st.Aux, 1, c) == 1 && c[0].Bus == 0x12 && c[0].LeftOnly && c[0].Left == F32(1.0),
              "aux strips on AUX 1 only: AUX 2's return stays the single left-entry write");
        st.Aux.Strips = 2;
        st.Aux.RetLevel[1] = 0;
        Check(ApolloAuxReturnCells(&st.Aux, 1, c) == 1 && c[0].Bus == 0x12 && c[0].Left == F32(1.0) && c[0].Right == F32(1.0),
              "aux strips on AUX 2: 0x12, 0 dB = 1.0 on both sides");
        st.Aux.RetMuted = 2;
        Check(ApolloAuxReturnCells(&st.Aux, 1, c) == 1 && c[0].Left == 0 && c[0].Right == 0, "aux strips on: a muted return is 0.0");
        st.Aux.RetMuted = 0;
        st.Aux.RetSet = 1;                              /* AUX 2's return never set: its 0 dB zero state is not sent */
        Check(ApolloAuxReturnCells(&st.Aux, 1, c) == 1 && c[0].Left == 0 && c[0].Right == 0,
              "aux strips on: a return the host never set is 0.0 (not the zero state's 0 dB, not the DSP default)");

        /* into a cue: the one {L input, cue param, L, R} write with the pan law; 0.0 while muted or never set */
        st.Aux.Strips = 1;
        st.Aux.CueSet[0] = 1ul << 1;
        for (bad = 0, h = APOLLO_CUE_SEND_MIN; h <= APOLLO_CUE_SEND_MAX; h += 5) {
            for (p = -100; p <= 100; p += 10) {
                st.Aux.CueLevel[0][1] = (unsigned char)(h + 122);
                st.Aux.CuePan[0][1] = (signed char)p;
                ApolloAuxCueCoef(&st, 0, 1, &l, &r);
                bad += ApolloAuxCueCells(&st, 0, 1, c) != 1 || c[0].Bus != 0x10 || c[0].Sub != 2 || c[0].Left != l ||
                       c[0].Right != r || c[0].LeftOnly;
            }
        }
        Check(bad == 0, "aux strips on: AUX 1 into CUE 2 = 0x10 {pan-law L, pan-law R}, every level and pan");
        st.Muted = 1ul << 1;
        Check(ApolloAuxCueCells(&st, 0, 1, c) == 1 && c[0].Left == 0 && c[0].Right == 0, "aux strips on: a muted cue: 0.0");
        st.Muted = 0;
        st.Aux.CuePan[0][1] = 0;
        Check(ApolloAuxCueCells(&st, 0, 2, c) == 1 && c[0].Sub == 5 && c[0].Left == 0 && c[0].Right == 0,
              "aux strips on: a cue send the host never set (CUE 3): 0.0");

        /* a send into a strip-backed (mono) aux: cues.c writes the left entry only, at the centre value, whatever the pan */
        memset(&st, 0, sizeof(st));
        st.Aux.Level[0][3] = (unsigned char)(0 + 122);   /* input 4 into AUX 1 at 0 dB, pre-fader */
        for (bad = 0, p = -100; p <= 100; p += 25) {
            unsigned long l0, r0;

            st.Aux.Pan[0][3] = (signed char)p;
            ApolloAuxSendCoefPan(&st, 0, 3, 0, &l, &r);
            st.Aux.Pan[0][3] = 0;
            ApolloAuxSendCoef(&st, 0, 3, &l0, &r0);
            bad += l != l0 || l == 0;
        }
        Check(bad == 0, "aux send into a mono aux: the centre value whatever the send's pan (never dropped by a hard pan)");
    }
}

/*
 * [Clock] 8. The front panel (driver\clock_plan.h, panel.c): the decode of the live readback words read on Aaron's
 * units (Nov 15), what a knob turn, a button and a host write's echo become, and the monitor link's arithmetic: only
 * the knob unit's own changes travel, the level as steps, never a host or mirrored value (no loops), talkback dims
 * only what was not dimmed and undims only what it dimmed.
 */
static void CheckPanel(void)
{
    /* [HW] Twin X: word 0 0x0800030C (knob on an output, not the headphones), word 2 0x00007094 (level 148),
       word 3 0x000077DF, word 6 0xC0000011 (lock reported, locked; talkback off) */
    unsigned long tw[40] = { 0x0800030Cul, 0xA0004182ul, 0x00007094ul, 0x000077DFul, 0, 0x08000020ul, 0xC0000011ul, 0xA0000000ul };
    /* [HW] x8p: word 0 0x00000208 (bit 27 clear: on a preamp), word 3 [30:28] 0 */
    unsigned long xw[40] = { 0x00000208ul, 0xA0004182ul, 0x00007F7Ful, 0x034DC555ul, 0, 0x00000920ul, 0xC0000020ul, 0x20417820ul };
    APOLLO_PANEL_STATE a, b;
    APOLLO_PANEL_EXPECT e[APOLLO_PANEL_FIELDS];
    unsigned long m;
    int dimmed;

    Check(ApolloPanelDecode(tw, 40, &a) == 1 && a.Focus == APOLLO_PANEL_FOCUS_MONITOR && a.Level == 148 && a.HpLevel == 112 &&
          a.Mute == 0 && a.Dim == 0 && a.Mono == 0 && a.Talk == 0 && a.Alt == 0 && a.Lock == 1,
          "Twin X live readback: knob MONITOR, level 148 (-22 dB), HP 112, lock reported locked, talkback off");
    Check(ApolloPanelDecode(xw, 40, &b) == 1 && b.Focus == APOLLO_PANEL_FOCUS_PREAMP && b.Preamp == 0 && b.Level == 127,
          "x8p live readback: knob on preamp 1 (word 0 bit 27 clear), level 127");
    Check(ApolloPanelDecode(tw, 7, &b) == 0 && b.Lock == -1 && b.Level == 0, "fewer than 8 words: nothing decoded");
    tw[0] |= 1ul << 26;
    ApolloPanelDecode(tw, 40, &b);
    Check(b.Focus == APOLLO_PANEL_FOCUS_HP, "word 0 bits 27+26: the knob is on the headphones");
    tw[0] &= ~(3ul << 26);
    tw[3] = (tw[3] & 0x0FFFFFFFul) | (1ul << 28);
    ApolloPanelDecode(tw, 40, &b);
    Check(b.Focus == APOLLO_PANEL_FOCUS_PREAMP && b.Preamp == 1, "word 0 bit 27 clear: PREAMP mode on word 3 [30:28] (preamp 2)");
    /* a knob turn, a press, talkback: the changed fields */
    tw[0] = 0x0800030Cul;
    tw[3] = 0x000077DFul;
    ApolloPanelDecode(tw, 40, &a);
    tw[2] = (tw[2] & ~0xFFul) | 150ul;
    ApolloPanelDecode(tw, 40, &b);
    Check(ApolloPanelDiff(&a, &b) == (1ul << APOLLO_PANEL_LEVEL), "knob +2 steps: the level alone changed");
    tw[2] |= 1ul << 16;
    tw[6] |= 1ul << 8;
    ApolloPanelDecode(tw, 40, &b);
    m = ApolloPanelDiff(&a, &b);
    Check(m == ((1ul << APOLLO_PANEL_LEVEL) | (1ul << APOLLO_PANEL_MUTE) | (1ul << APOLLO_PANEL_TALK)),
          "level, mute (the knob press) and talkback at once");
    tw[3] = (tw[3] & 0x0FFFFFFFul) | (1ul << 28);
    ApolloPanelDecode(tw, 40, &b);
    Check((ApolloPanelDiff(&a, &b) & (1ul << APOLLO_PANEL_PREAMP)) == 0,
          "word 3 [30:28] moving while the knob is on MONITOR is not a change");
    /* who: a host write's echo vs the panel */
    memset(e, 0, sizeof(e));
    ApolloPanelExpect(e, APOLLO_PANEL_LEVEL, 100, APOLLO_PANEL_ORIGIN_HOST, 1000);
    Check(ApolloPanelClassify(e, APOLLO_PANEL_LEVEL, 99, 1100) == APOLLO_PANEL_ORIGIN_PANEL &&
          ApolloPanelClassify(e, APOLLO_PANEL_LEVEL, 100, 1200) == APOLLO_PANEL_ORIGIN_HOST &&
          ApolloPanelClassify(e, APOLLO_PANEL_LEVEL, 100, 1300) == APOLLO_PANEL_ORIGIN_PANEL,
          "a host write: its value within 1.5 s is the host's, once; anything else is the panel's");
    ApolloPanelExpect(e, APOLLO_PANEL_DIM, 1, APOLLO_PANEL_ORIGIN_LINK, 0xFFFFFF00ul);
    Check(ApolloPanelClassify(e, APOLLO_PANEL_DIM, 1, 0x00000200ul) == APOLLO_PANEL_ORIGIN_LINK,
          "an expectation across the ms counter's wrap still counts");
    ApolloPanelExpect(e, APOLLO_PANEL_MUTE, 1, APOLLO_PANEL_ORIGIN_HOST, 0);
    Check(ApolloPanelClassify(e, APOLLO_PANEL_MUTE, 1, 1501) == APOLLO_PANEL_ORIGIN_PANEL, "stale after 1.5 s: the panel's");
    Check(ApolloPanelFieldOfMon(APOLLO_MP_LEVEL) == APOLLO_PANEL_LEVEL && ApolloPanelFieldOfMon(APOLLO_MP_MUTE) == APOLLO_PANEL_MUTE &&
          ApolloPanelFieldOfMon(APOLLO_MP_MONO) == APOLLO_PANEL_MONO && ApolloPanelFieldOfMon(APOLLO_MP_DIM) == APOLLO_PANEL_DIM &&
          ApolloPanelFieldOfMon(APOLLO_MP_TALKBACK) == APOLLO_PANEL_TALK && ApolloPanelFieldOfMon(APOLLO_MP_ALT_SELECT) == APOLLO_PANEL_ALT &&
          ApolloPanelFieldOfMon(APOLLO_MP_HP1_LEVEL) == APOLLO_PANEL_HP && ApolloPanelFieldOfMon(APOLLO_MP_SOURCE) == 0,
          "the monitor codes whose echo the panel recognises (monitor_plan.h codes)");
    /* the link: what travels */
    Check(ApolloLinkCarries(APOLLO_LINK_ALL, APOLLO_PANEL_LEVEL, APOLLO_PANEL_ORIGIN_PANEL, APOLLO_PANEL_FOCUS_MONITOR) ==
          APOLLO_LINK_LEVEL && ApolloLinkCarries(APOLLO_LINK_ALL, APOLLO_PANEL_LEVEL, APOLLO_PANEL_ORIGIN_PANEL,
                                                  APOLLO_PANEL_FOCUS_PREAMP) == 0 &&
          ApolloLinkCarries(APOLLO_LINK_ALL, APOLLO_PANEL_LEVEL, APOLLO_PANEL_ORIGIN_PANEL, APOLLO_PANEL_FOCUS_HP) == 0,
          "the knob's level travels in MONITOR mode only (PREAMP: a gain; HP: the headphones)");
    Check(ApolloPanelLinkFocus(APOLLO_MODEL_TWINX, APOLLO_PANEL_FOCUS_PREAMP) == APOLLO_PANEL_FOCUS_PREAMP &&
          ApolloPanelLinkFocus(APOLLO_MODEL_X8P, APOLLO_PANEL_FOCUS_PREAMP) == APOLLO_PANEL_FOCUS_MONITOR,
          "the Twin X's one knob has modes; the x8p's monitor knob is its own (its preamp knob's focus does not count)");
    Check(ApolloLinkCarries(APOLLO_LINK_ALL, APOLLO_PANEL_LEVEL, APOLLO_PANEL_ORIGIN_HOST, APOLLO_PANEL_FOCUS_MONITOR) == 0 &&
          ApolloLinkCarries(APOLLO_LINK_ALL, APOLLO_PANEL_MUTE, APOLLO_PANEL_ORIGIN_LINK, APOLLO_PANEL_FOCUS_MONITOR) == 0 &&
          ApolloLinkCarries(APOLLO_LINK_ALL, APOLLO_PANEL_DIM, APOLLO_PANEL_ORIGIN_UNIT, APOLLO_PANEL_FOCUS_MONITOR) == 0,
          "a host write, a mirrored value, the unit's own talkback dim: never sent on (no loops)");
    Check(ApolloLinkCarries(APOLLO_LINK_TALK_DIM, APOLLO_PANEL_TALK, APOLLO_PANEL_ORIGIN_HOST, 0) == APOLLO_LINK_TALK_DIM &&
          ApolloLinkCarries(APOLLO_LINK_LEVEL, APOLLO_PANEL_MUTE, APOLLO_PANEL_ORIGIN_PANEL, 1) == 0 &&
          ApolloLinkCarries(APOLLO_LINK_ALL, APOLLO_PANEL_ALT, APOLLO_PANEL_ORIGIN_PANEL, 1) == 0,
          "talkback from the host dims too; each kind only with its flag; ALT never travels");
    Check(ApolloLinkLevel(148, 2) == 150 && ApolloLinkLevel(191, 5) == 192 && ApolloLinkLevel(3, -10) == 0 &&
          ApolloLinkLevel(250, -1) == 191, "level steps: added, clamped to 0..192");
    dimmed = 0;
    Check(ApolloLinkTalkDim(1, 0, &dimmed) == 1 && dimmed == 1 && ApolloLinkTalkDim(0, 1, &dimmed) == 0 && dimmed == 0,
          "talkback on dims an undimmed monitor and off undims it");
    Check(ApolloLinkTalkDim(1, 1, &dimmed) == -1 && dimmed == 0 && ApolloLinkTalkDim(0, 1, &dimmed) == -1,
          "a dim the user had stays: talkback neither sets nor clears it");
}

/* [Out] output and front-panel settings: open-apollo's kext mapping, per model, clear of every preamp field */
static void CheckOutputs(void)
{
    static const ULONG codes[] = { 0x0A, 0x0F, 0x13, 0x14, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x32, 0x42 };
    static const int models[2] = { APOLLO_MODEL_X8P, APOLLO_MODEL_TWINX };
    /* preamp and Unison fields (preamp_plan.h, spec-twinx 4.1/4.4): S0 [25:0], S11 [15:8], S12 [23:0], S13 [7:0], S15 [31:24] */
    static const struct { ULONG Setting, Mask; } pre[] = {
        { 0, 0x03FFFFFF }, { 11, 0x0000FF00 }, { 12, 0x00FFFFFF }, { 13, 0x000000FF }, { 15, 0xFF000000 },
        { 3, 0xFFFFFFFF }, { 14, 0x0FFFFFFF }, { 16, 0xFFFFFFFF }, { 17, 0xFFFFFFFF }, { 24, 0xFFFFFFFF }, { 25, 0xFFFFFFFF },
    };
    ULONG k, j, writes = 0, bad = 0, clash = 0, bits = 0;
    LONG v;
    int m;
    char what[200];

    for (m = 0; m < 2; m++) {
        for (k = 0; k < RTL_NUMBER_OF(codes); k++) {
            for (v = -3; v <= 10; v++) {
                APOLLO_MON_WRITE w;
                MON_OUT o;

                if (Path(models[m], codes[k], v, &w) != R_OK) {
                    continue;
                }
                writes++;
                if (OaMonitorSetParam(codes[k], (ULONG)v, &o) != R_OK || o.Setting != w.Setting || o.Value != w.Value ||
                    o.Mask != w.Mask) {
                    if (bad++ < 5) {
                        sprintf_s(what, sizeof(what), "[Out] model %d code 0x%lX value %ld: S%lu 0x%08lX/0x%08lX", models[m],
                                  codes[k], v, w.Setting, w.Value, w.Mask);
                        Check(0, what);
                    }
                }
                for (j = 0; j < RTL_NUMBER_OF(pre); j++) {
                    if (pre[j].Setting == w.Setting && (w.Mask & pre[j].Mask) != 0) clash++;
                }
            }
        }
    }
    sprintf_s(what, sizeof(what), "[Out] %lu writes on both models = open-apollo's kext mapping, none on a preamp field", writes);
    Check(writes > 60 && bad == 0 && clash == 0, what);
    for (k = 0; k < RTL_NUMBER_OF(codes); k++) {
        ULONG b = ApolloMonOutBit(codes[k]);
        bits |= b;
        Check(b != 0 && ApolloMonExtBit(codes[k]) == 0 && ApolloPlanMonBit(codes[k]) == 0, "[Out] its own plan word, not MonitorParams");
    }
    Check(bits == APOLLO_MON_OUT_ALL, "[Out] every plan bit used");
    Expect(X, 0x1C, 1, R_OK, 0, 0x80000000ul, 0x80000000ul, "Line 7-8 -10 dBV");
    Expect(X, 0x19, 0, R_OK, 0, 0, 0x10000000ul, "Line 1-2 +4 dBu");
    Expect(X, 0x19, 2, R_INVALID, 0, 0, 0, "line pad 2");
    Expect(T, 0x19, 1, R_NOTSUP, 0, 0, 0, "Twin X has no line reference");
    Expect(X, 0x32, 1, R_OK, 11, 0x10, 0x10, "monitor -10 dBV");
    Expect(T, 0x32, 1, R_OK, 11, 0x10, 0x10, "monitor -10 dBV");
    Expect(X, 0x1E, 1, R_OK, 6, 0x400, 0x400, "monitor to S/PDIF");
    Expect(T, 0x1E, 1, R_NOTSUP, 0, 0, 0, "Twin X has no digital mirror");
    Expect(X, 0x42, 4, R_OK, 13, 0x8000, 0xE000, "FCN talkback");
    Expect(X, 0x42, 1, R_INVALID, 0, 0, 0, "FCN alt 2 (not the x8p's)");
    Expect(T, 0x42, 2, R_NOTSUP, 0, 0, 0, "Twin X has no function switch");
    Expect(X, 0x13, 5, R_OK, 1, 0x50000000ul, 0x70000000ul, "clip hold infinite");
    Expect(X, 0x13, 6, R_INVALID, 0, 0, 0, "clip hold 6");
    Expect(T, 0x14, 4, R_OK, 1, 0x8000, 0xE000, "peak hold 10 s");
    Expect(X, 0x14, 5, R_INVALID, 0, 0, 0, "peak hold 5");
    Expect(T, 0x0A, 1, R_OK, 6, 0x200, 0x300, "AUX 1 mono");
    Expect(X, 0x0F, 0, R_OK, 7, 0, 0x300, "AUX 2 stereo");
    Expect(T, 0x1D, 2, R_OK, 6, 0x1000, 0xF800, "identify 2");
    Expect(X, 0x1D, 0, R_OK, 6, 0, 0xF800, "identify off");
    Expect(X, 0x1D, 8, R_INVALID, 0, 0, 0, "identify 8");
}

/*
 * 9. [Virtual] The Virtual buses (VIRTUAL n = bus 0x18 + n - 1) of the Input Strips the unit builds (strips.c
 * ApolloStripsVirtualBuses -> ApolloMonEncodeEx's Extra): with Extra 0 every code and every write is
 * ApolloMonEncode's (so the x8p-unchanged proof above stands for the driver's path); with VIRTUAL n's bit the mix,
 * cue and aux codes of that bus are taken exactly like an ANALOG bus's, every other bus and code as before, and
 * mute-all and solo cover it; bits outside 0x18-0x1F are ignored.
 */
static int SameWrite(const APOLLO_MON_WRITE *A, const APOLLO_MON_WRITE *B, unsigned long BusesExtra)
{
    return A->Kind == B->Kind && A->Setting == B->Setting && A->Value == B->Value && A->Mask == B->Mask &&
           A->Cue == B->Cue && A->Bus == B->Bus && A->Arg == B->Arg && A->Aux == B->Aux &&
           A->Buses == (B->Buses | (ApolloMonIsMix(B) ? BusesExtra : 0ul));
}

/* The bus a mix, cue or aux code names (0xFFFFFFFF: none). */
static unsigned long CodeBus(unsigned long P)
{
    if (P >= APOLLO_MP_AUX_FIRST && P <= APOLLO_MP_AUX_LAST) {
        return (APOLLO_MP_AUX_KIND(P) <= 2ul && (P & 0xC0ul) == 0) ? (P & 0x1Ful) : 0xFFFFFFFFul;
    }
    if (P >= APOLLO_MP_MIX_FIRST && P <= APOLLO_MP_MIX_LAST) {
        return APOLLO_MP_MIX_KIND(P) <= 3ul ? (P & 0xFFul) : 0xFFFFFFFFul;
    }
    if (P >= 0x1000ul && P <= 0x2FFFul) {
        return P & 0xFFul;
    }
    return 0xFFFFFFFFul;
}

static void CheckVirtualBuses(void)
{
    static const LONG vals[] = { -2000000000L, -122L, -121L, -120L, -101L, -100L, -12L, -1L, 0L, 1L, 2L, 3L, 4L, 24L,
                                 25L, 100L, 101L, 192L, 193L };
    static const int models[3] = { X, T, 0 };
    const unsigned long v1 = 1ul << 0x18, all = APOLLO_MON_VIRTUAL_BUSES, twx4 = 0x0F000000ul;
    unsigned long p, bad = 0, n = 0, took = 0, m;
    APOLLO_MON_WRITE w0, w1, w2;
    APOLLO_MIX_STATE before, after;
    unsigned int k;
    int r0, r1, r2;
    char what[220];

    Check(APOLLO_MON_VIRTUAL_BUS(1) == 0x18ul && APOLLO_MON_VIRTUAL_BUS(8) == 0x1Ful && APOLLO_MON_VIRTUAL_BUSES == 0xFF000000ul,
          "virtual: VIRTUAL 1-8 = buses 0x18-0x1F (synch_in 24-31), one mask bit each");
    Check(APOLLO_MP_AUX_SEND(1, 0x18) == 0x6038ul && APOLLO_MP_AUX_POST(0, 0x1F) == 0x621Ful &&
          APOLLO_MP_CUE_SEND(3, 0x18) == 0x1318ul && APOLLO_MP_MIX_SOLO(0x1F) == 0x431Ful,
          "virtual: the codes carry buses 0x18-0x1F (aux: 5-bit index, bits 6-7 clear)");

    /* Extra 0, and Extra outside the Virtual buses: exactly ApolloMonEncode, on both models and no model */
    for (m = 0; m < 3; m++) {
        const APOLLO_MON_MODEL *M = ApolloMonModelFor(models[m]);

        for (p = 0; p < 0x7000; p++) {
            for (k = 0; k < RTL_NUMBER_OF(vals); k++) {
                r0 = ApolloMonEncode(M, p, vals[k], &w0);
                r1 = ApolloMonEncodeEx(M, 0, p, vals[k], &w1);
                r2 = ApolloMonEncodeEx(M, 0x00FFFFFFul, p, vals[k], &w2);
                n++;
                if (r0 != r1 || r0 != r2 || (r0 == APOLLO_MON_OK && (!SameWrite(&w1, &w0, 0) || !SameWrite(&w2, &w0, 0)))) {
                    if (bad++ < 5) {
                        sprintf_s(what, sizeof(what), "virtual: model %d code 0x%lX value %ld: Encode %d, EncodeEx(0) %d, "
                                  "EncodeEx(0x00ffffff) %d", models[m], p, vals[k], r0, r1, r2);
                        Check(0, what);
                    }
                }
            }
        }
    }
    sprintf_s(what, sizeof(what), "virtual: Extra 0 (and bits outside 0x18-0x1F) = ApolloMonEncode for %lu (model, code, value) "
              "triples: the x8p-unchanged proof holds for mixer.c's path", n);
    Check(bad == 0, what);

    /* x8p with VIRTUAL 1 (StripVirtualMask 0x01): bus 0x18 taken by every mix, cue and aux family; nothing else moves */
    bad = 0;
    n = 0;
    for (m = 0; m < 2; m++) {
        const APOLLO_MON_MODEL *M = ApolloMonModelFor(models[m]);
        unsigned long extra = (m == 0) ? v1 : twx4;

        for (p = 0; p < 0x7000; p++) {
            unsigned long b = CodeBus(p);

            for (k = 0; k < RTL_NUMBER_OF(vals); k++) {
                r0 = ApolloMonEncode(M, p, vals[k], &w0);
                r1 = ApolloMonEncodeEx(M, extra, p, vals[k], &w1);
                n++;
                if (b < 32 && ((extra >> b) & 1ul) != 0 && ((p >> 12) != 3ul)) {
                    /* the Virtual bus: refused before (no strip), now what an ANALOG bus gets with this value */
                    unsigned long q = (p & ~0x1Ful & ~0xE0ul) | ((p >= APOLLO_MP_AUX_FIRST) ? (p & 0x20ul) : 0ul);
                    APOLLO_MON_WRITE wa;
                    int ra;

                    if (p < APOLLO_MP_AUX_FIRST) {
                        q = p & ~0xFFul;        /* the same code on bus 0 (ANALOG 1) */
                    }
                    ra = ApolloMonEncode(M, q, vals[k], &wa);
                    /* before: refused for the bus (a cue the model lacks: not here, as for any bus) */
                    if (r0 != ((ra == APOLLO_MON_NOT_HERE) ? APOLLO_MON_NOT_HERE : APOLLO_MON_BAD_VALUE) || r1 != ra ||
                        (r1 == APOLLO_MON_OK && (w1.Kind != wa.Kind || w1.Bus != b || w1.Arg != wa.Arg || w1.Cue != wa.Cue ||
                                                 w1.Aux != wa.Aux))) {
                        if (bad++ < 5) {
                            sprintf_s(what, sizeof(what), "virtual: model %d code 0x%lX (bus 0x%lX) value %ld: before %d, now %d, "
                                      "ANALOG 1 gets %d", models[m], p, b, vals[k], r0, r1, ra);
                            Check(0, what);
                        }
                    } else if (r1 == APOLLO_MON_OK) {
                        took++;
                    }
                } else if (r0 != r1 || (r0 == APOLLO_MON_OK && !SameWrite(&w1, &w0, extra))) {
                    if (bad++ < 5) {
                        sprintf_s(what, sizeof(what), "virtual: model %d code 0x%lX value %ld: %d before, %d with the Virtual "
                                  "buses (another bus or code must not change)", models[m], p, vals[k], r0, r1);
                        Check(0, what);
                    }
                }
            }
        }
    }
    sprintf_s(what, sizeof(what), "virtual: x8p + VIRTUAL 1, Twin X + VIRTUAL 1-4: their buses take every mix/cue/aux family as "
              "ANALOG 1 does (%lu writes), every other code and bus as before (%lu triples)", took, n);
    Check(bad == 0 && took > 600, what);

    /* by name, the ones Apollo Control will send */
    Check(ApolloMonEncodeEx(ApolloMonModelFor(X), v1, APOLLO_MP_MIX_LEVEL(0x18), -12, &w1) == APOLLO_MON_OK &&
          w1.Kind == APOLLO_MONW_MIX_LEVEL && w1.Bus == 0x18 && w1.Arg == -12 && w1.Buses == (0x4FFul | v1),
          "virtual: x8p mix level of VIRTUAL 1 (-6 dB): bus 0x18; the mixed buses 0x010004FF");
    Check(ApolloMonEncodeEx(ApolloMonModelFor(X), v1, APOLLO_MP_MIX_PAN(0x18), -100, &w1) == APOLLO_MON_OK &&
          ApolloMonEncodeEx(ApolloMonModelFor(X), v1, APOLLO_MP_MIX_MUTE(0x18), 1, &w1) == APOLLO_MON_OK &&
          ApolloMonEncodeEx(ApolloMonModelFor(X), v1, APOLLO_MP_MIX_SOLO(0x18), 1, &w1) == APOLLO_MON_OK,
          "virtual: VIRTUAL 1 pan (not the talkback's rule), mute, solo");
    Check(ApolloMonEncodeEx(ApolloMonModelFor(X), v1, APOLLO_MP_CUE_SEND(0, 0x18), 0, &w1) == APOLLO_MON_OK &&
          w1.Kind == APOLLO_MONW_SEND && w1.Cue == 0 && w1.Bus == 0x18 &&
          ApolloMonEncodeEx(ApolloMonModelFor(X), v1, APOLLO_MP_CUE_PAN(3, 0x18), 30, &w1) == APOLLO_MON_OK &&
          w1.Kind == APOLLO_MONW_PAN && w1.Cue == 3,
          "virtual: VIRTUAL 1's sends to CUE 1-4 (level, pan)");
    Check(ApolloMonEncodeEx(ApolloMonModelFor(X), v1, APOLLO_MP_AUX_SEND(1, 0x18), -6, &w1) == APOLLO_MON_OK &&
          w1.Kind == APOLLO_MONW_AUX_SEND && w1.Aux == 1 && w1.Bus == 0x18 &&
          ApolloMonEncodeEx(ApolloMonModelFor(X), v1, APOLLO_MP_AUX_POST(0, 0x18), 1, &w1) == APOLLO_MON_OK &&
          w1.Kind == APOLLO_MONW_AUX_POST,
          "virtual: VIRTUAL 1's aux sends (AUX 2 level, AUX 1 post-fader)");
    Check(ApolloMonEncodeEx(ApolloMonModelFor(X), v1, APOLLO_MP_MIX_LEVEL(0x19), 0, &w1) == APOLLO_MON_BAD_VALUE &&
          ApolloMonEncodeEx(ApolloMonModelFor(X), v1, APOLLO_MP_CUE_SEND(0, 0x19), 0, &w1) == APOLLO_MON_BAD_VALUE &&
          ApolloMonEncodeEx(ApolloMonModelFor(X), v1, APOLLO_MP_AUX_SEND(0, 0x19), 0, &w1) == APOLLO_MON_BAD_VALUE &&
          ApolloMonEncode(ApolloMonModelFor(X), APOLLO_MP_MIX_LEVEL(0x18), 0, &w1) == APOLLO_MON_BAD_VALUE,
          "virtual: VIRTUAL 2 without its strip, and VIRTUAL 1 on a unit without one: refused (invalid parameter)");
    Check(ApolloMonEncodeEx(ApolloMonModelFor(T), twx4, APOLLO_MP_CUE_SEND(1, 0x1B), 0, &w1) == APOLLO_MON_OK &&
          ApolloMonEncodeEx(ApolloMonModelFor(T), twx4, APOLLO_MP_CUE_SEND(2, 0x18), 0, &w1) == APOLLO_MON_NOT_HERE,
          "virtual: Twin X VIRTUAL 4 to LINE 3/4 (CUE 2) taken; CUE 3 still not on a Twin X");
    Check(ApolloMonEncodeEx(0, all, APOLLO_MP_MIX_LEVEL(0x18), 0, &w1) == APOLLO_MON_NOT_HERE,
          "virtual: no monitor plan, no Virtual bus");
    Check(ApolloPlanMonitorParamAllowed(ApolloBringupPlanFor(X), APOLLO_MP_MIX_LEVEL(0x18)) &&
          ApolloPlanMonitorParamAllowed(ApolloBringupPlanFor(X), APOLLO_MP_CUE_SEND(0, 0x18)) &&
          ApolloPlanMonitorParamAllowed(ApolloBringupPlanFor(X), APOLLO_MP_AUX_SEND(0, 0x18)) &&
          ApolloPlanMonitorParamAllowed(ApolloBringupPlanFor(T), APOLLO_MP_MIX_MUTE(0x18)),
          "virtual: the plan filter passes the families whatever the bus (the encoder decides the bus)");

    /* mute-all and solo cover the Virtual buses the unit mixes */
    memset(&before, 0, sizeof(before));
    Check(ApolloMonEncodeEx(ApolloMonModelFor(X), v1, APOLLO_MP_MIX_MUTE_ALL, 1, &w1) == APOLLO_MON_OK &&
          w1.Buses == (0x4FFul | v1), "virtual: mute-all covers VIRTUAL 1");
    after = before;
    ApolloMixStore(&after, &w1);
    Check((after.Muted & v1) != 0 && (ApolloMixTargets(&before, &after, &w1) & v1) != 0,
          "virtual: mute-all mutes and re-sends VIRTUAL 1 (0.0)");
    Check(ApolloMonEncodeEx(ApolloMonModelFor(X), v1, APOLLO_MP_MIX_SOLO(0), 1, &w1) == APOLLO_MON_OK, "solo input 1");
    after = before;
    ApolloMixStore(&after, &w1);
    {
        unsigned long l, r, t = ApolloMixTargets(&before, &after, &w1);

        ApolloMixCoef(&after, 0x18, &l, &r);
        Check((t & v1) != 0 && l == 0 && r == 0, "virtual: soloing input 1 silences VIRTUAL 1 (sent as 0.0)");
        ApolloMixCoef(&after, 0, &l, &r);
        Check(l == 0x3F3504F3ul && r == 0x3F3504F3ul && (t & 1ul) == 0, "virtual: ... and leaves the soloed input alone");
    }
}

int main(void)
{
    CheckX8pUnchanged();
    CheckKextMapping();
    CheckFeatures();
    CheckCoefficients();
    CheckCueState();
    CheckReadback();
    CheckPlanBits();
    CheckCuePass();
    CheckMixCodes();
    CheckMixWeights();
    CheckMixLogic();
    CheckMixPass();
    CheckAux();
    CheckPanel();
    CheckOutputs();
    CheckVirtualBuses();    /* [Virtual] */
    printf("mon-plan-test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
