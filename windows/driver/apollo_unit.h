/*
 * apollo_unit.h - per-unit driver Parameters: names, built-in defaults, lookup order and the
 * Mode rules (docs/spec-twinx.md 6.9, "As built in G0-G1"). Plain C like apollo_model.h:
 * driver\unit.c reads the registry and calls these, tools\ident-test checks them.
 *
 *   HKLM\SYSTEM\CurrentControlSet\Services\apollo\Parameters
 *       <value>                 legacy global: applies to the Apollo x8p ONLY
 *       Units\<unit key>\<value>    one unit (apollo_model.h ApolloUnitKeyName)
 *       Models\<TT>\<value>         one model, TT = device type in hex: 22 x8p, 99 Twin X Gen 1
 *
 * Each value: Units\<key> -> Models\<TT> -> legacy global (x8p only) -> built-in default of the
 * model. A unit that is not an x8p therefore never inherits the x8p's values, and with no key of
 * its own it starts in Mode 0 (observe: identify and quiesce only).
 * [Twin X G2] Device add, before identification, goes by the PCI hardware ID instead (below).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once
#ifndef _WCHAR_T_DEFINED
#include <stddef.h>             /* wchar_t (the kernel build has it from ntddk.h) */
#endif
#include "apollo_model.h"
#include "bringup_plan.h"       /* [Twin X G3] which models have a bring-up path, and up to which Mode */

/* Mode: what the driver does with the unit */
#define APOLLO_MODE_OBSERVE         0   /* identify + quiesce, nothing else */
#define APOLLO_MODE_TRANSPORT       1   /* bring-up + transport; no strips, no Windows endpoints */
#define APOLLO_MODE_FULL            2   /* everything the values and the model's plan allow (Twin X, G4:
                                           endpoints + monitor settings; no ASIO, strips, preamps;
                                           G5: + strips (StripsEnable), preamps, MIC/LINE 1-2 rows) */

/* The x8p's built-in defaults, as the driver had them before per-unit keys */
#define APOLLO_DEF_RENDER_LEAD      144u
#define APOLLO_DEF_CAPTURE_GUARD    16u
#define APOLLO_DEF_ENDPOINTS_X8P    0x1FFFFu    /* [Endpoints] = APOLLO_X8P_EP_DEFAULT (circuit.c asserts): every
                                                   output, the loopback, MIC/LINE 1-8; VIRTUAL/S/PDIF in opt-in */
#define APOLLO_DEF_ENDPOINTS_TWINX  0x3Fu       /* [Endpoints] = APOLLO_TWX_EP_DEFAULT: MON L/R, LINE 3-4, HP out,
                                                   the loopback, MIC/LINE 1-2 */
#define APOLLO_DEF_BATCH_X8P        1u      /* = APOLLO_MIXER_BATCH_X8P_SAFE (unit.c asserts) */

/* Parameter ids */
#define APOLLO_PARAM_MODE                   0
#define APOLLO_PARAM_STRIPS_ENABLE          1
#define APOLLO_PARAM_STRIP_STEPS            2
#define APOLLO_PARAM_STRIP_MASK             3
#define APOLLO_PARAM_STRIP_DSP              4
#define APOLLO_PARAM_MIXER_INIT_BATCH       5
#define APOLLO_PARAM_RENDER_LEAD            6
#define APOLLO_PARAM_CAPTURE_GUARD          7
#define APOLLO_PARAM_START_AT_CONNECT       8
#define APOLLO_PARAM_ENABLED_ENDPOINTS      9
#define APOLLO_PARAM_STRIP_PROBE            10  /* live strips.c (StripProbe, STRIPS.md 10.9) reads it */
#define APOLLO_PARAM_WARM_BOOT_TRANSPORT    11  /* [Twin X G3] device.c: boot transport on a warm start too */
#define APOLLO_PARAM_AUDIO_RING_PAGES       12  /* [G4b] audio.c: 1 = audio rings always one common buffer per page */
#define APOLLO_PARAM_CAPTURE_TRIM           13  /* [G7] agg.c: combined capture alignment trim, signed frames */
#define APOLLO_PARAM_RENDER_TRIM            14  /* [G7] agg.c: combined playback alignment trim, signed frames */
#define APOLLO_PARAM_SAMPLE_RATE            15  /* [Rate] rate.c: the rate this unit comes up at (Hz) */
#define APOLLO_PARAM_STRIP_ADOPT            16  /* [Warm] strips.c: keep Live strips at the D0 exit, adopt them warm */
#define APOLLO_PARAM_STRIP_WARM_REBUILD     17  /* [Warm] strips.c: build again at a warm start proven ours */
#define APOLLO_PARAM_STRIP_WARM_PLUGINS     18  /* [Warm] strips.c/plugin.c: plug-ins on adopted or rebuilt warm strips */
#define APOLLO_PARAM_COUNT                  19

/* Where a value came from (APOLLO_UNIT.Source, the driver log letter) */
#define APOLLO_SRC_DEFAULT          0   /* D: built-in default of the model */
#define APOLLO_SRC_UNIT             1   /* U: Units\<key> */
#define APOLLO_SRC_MODEL            2   /* M: Models\<TT> */
#define APOLLO_SRC_GLOBAL           3   /* G: legacy global (x8p only) */
#define APOLLO_SRC_MODE             4   /* X: forced by Mode (or an invalid Mode forced to 0) */

typedef struct _APOLLO_PARAM_DEF {
    const char     *Name;
    const wchar_t  *NameW;          /* registry value name */
    unsigned long   DefaultX8p;
    unsigned long   DefaultOther;   /* any other model: safe values (also before identification) */
    int             Hex;            /* log in hex */
    unsigned long   DefaultTwinX;   /* [Twin X G3] the Twin X Gen 1's built-in default */
} APOLLO_PARAM_DEF;

static __inline const APOLLO_PARAM_DEF *ApolloParamDef(int Id)
{
    static const APOLLO_PARAM_DEF t[APOLLO_PARAM_COUNT] = {
        { "Mode",                    L"Mode",                    APOLLO_MODE_FULL, APOLLO_MODE_OBSERVE, 0, APOLLO_MODE_OBSERVE },
        /* [G5] The Twin X: strips off by default (the G5 test turns them on); when on, the whole
           chain (StripSteps 0) on both inputs (StripMask 0x3) on DSP 1 (strip_plan.h) */
        { "StripsEnable",            L"StripsEnable",            0, 0, 0, 0 },
        { "StripSteps",              L"StripSteps",              1, 1, 0, 0 },
        { "StripMask",               L"StripMask",               0x01, 0x01, 1, 0x03 },
        { "StripDsp",                L"StripDsp",                1, 1, 0, 1 },
        /* Other models: 0 (nothing). The Twin X: 1 = its model-safe batch (bringup_plan.h, spec-twinx 4.3). */
        { "MixerInitBatch",          L"MixerInitBatch",          APOLLO_DEF_BATCH_X8P, 0, 0, 1 },
        { "RenderLeadFrames",        L"RenderLeadFrames",        APOLLO_DEF_RENDER_LEAD, APOLLO_DEF_RENDER_LEAD, 0, APOLLO_DEF_RENDER_LEAD },
        { "CaptureGuardFrames",      L"CaptureGuardFrames",      APOLLO_DEF_CAPTURE_GUARD, APOLLO_DEF_CAPTURE_GUARD, 0, APOLLO_DEF_CAPTURE_GUARD },
        { "StartTransportAtConnect", L"StartTransportAtConnect", 1, 1, 0, 1 },
        /* Other models: no Windows endpoints. [G4] The Twin X: its own table, MON L/R out + in (0x3),
           used in Mode 2 only (forced 0 below).
           [Endpoints] Nov 15: value name "Endpoints" (bit i = row i of endpoint_plan.h's table), no longer
           "EnabledEndpoints": the rows were renumbered, and a mask stored for the old 10-row x8p / 4-row Twin X
           tables (0x3FF, 0xF) would hide every new output. The old value stays where it is (an older build
           still reads it) and this one ignores it; absent = the model's default set. */
        { "Endpoints",               L"Endpoints",               APOLLO_DEF_ENDPOINTS_X8P, 0, 1, APOLLO_DEF_ENDPOINTS_TWINX },
        /* Strip diagnostics (strip_plan.h STRIP_PROBE_OFF = 0) */
        { "StripProbe",              L"StripProbe",              0, 0, 0, 0 },
        /* [Twin X G3] 1 = run the boot transport + early ACEFACE on a warm start too (spec-twinx 2
           notes: the fallback if a warm ACEFACE with the transport stopped times out). The x8p
           has no boot transport and ignores it. */
        { "WarmBootTransport",       L"WarmBootTransport",       0, 0, 0, 0 },
        /* [G4b] 0 = each audio ring one contiguous 4 MiB when there is one below 4 GiB, else one
           common buffer per SG page (ring_map.h); 1 = always per page (to run that path at will). */
        { "AudioRingPages",          L"AudioRingPages",          0, 0, 0, 0 },
        /* [G7] the combined device's per-unit alignment (spec 6.3.5): a REG_DWORD read as a signed number of
           frames, -64..64 (agg.c ApolloAggTrimOf), e.g. 0xFFFFFFFB = -5. Positive delays that unit's capture /
           playback in the combined device. 0 = G6 behaviour. Read when the unit joins (its next start). */
        { "CaptureTrimFrames",       L"CaptureTrimFrames",       0, 0, 1, 0 },
        { "RenderTrimFrames",        L"RenderTrimFrames",        0, 0, 1, 0 },
        /* [Rate] the sample rate the unit comes up at, Hz (rate_plan.h; anything but the six rates = 48000).
           SET_RATE writes it under Units\<key> (apollo_rate.h); read at every start. */
        { "SampleRate",              L"SampleRate",              48000, 48000, 0, 48000 },
        /* [Warm] docs/STRIPS.md 12. StripAdopt 1: a D0 exit leaves Live strips running (plug-in chains put back
           on the plain strips first) and the next warm start adopts them when the record proves them this
           driver's; 0 = the old rule (torn down at every D0 exit, a power cycle for strips). The Twin X's warm
           start ring-connects, which drops them, so it keeps the teardown whatever this says.
           StripWarmRebuild 1: a warm start that proves only this driver used the DSPs since the last power-up
           builds the strips again instead of asking for a power cycle. The Twin X needs it after every warm
           start (on); the x8p adopts instead and builds warm only when told to (off: its warm DSP was never
           sent an upload by this driver, docs/STRIPS.md 12.4). */
        { "StripAdopt",              L"StripAdopt",              1, 0, 0, 1 },
        { "StripWarmRebuild",        L"StripWarmRebuild",        0, 0, 0, 1 },
        /* [Warm] docs/STRIPS.md 12.5 (Nov 15 2026). StripWarmPlugins 1: after a warm start whose strips were adopted
           (the x8p: no ring connect, the DSPs' session byte 0x07) or rebuilt (the Twin X) from an anchored record
           that saw no plug-in left on the strip DSP, that DSP takes plug-ins as after a cold start; its first
           acknowledged upload proves the warm session takes uploads, an unanswered one quarantines it (power cycle).
           0 = plug-ins wait for a power cycle after every warm start (the rule before this value). */
        { "StripWarmPlugins",        L"StripWarmPlugins",        1, 0, 0, 1 },
    };

    return (Id >= 0 && Id < APOLLO_PARAM_COUNT) ? &t[Id] : &t[APOLLO_PARAM_MODE];
}

/* The built-in default of a value for a model. */
static __inline unsigned long ApolloParamDefault(int Model, int Id)
{
    const APOLLO_PARAM_DEF *d = ApolloParamDef(Id);

    if (Model == APOLLO_MODEL_X8P) {
        return d->DefaultX8p;
    }
    return (Model == APOLLO_MODEL_TWINX) ? d->DefaultTwinX : d->DefaultOther;
}

/* What the registry holds for one value at each level (Have* = the value exists there). */
typedef struct _APOLLO_PARAM_LEVELS {
    int             HaveUnit, HaveModel, HaveGlobal;
    unsigned long   Unit, Model, Global;
} APOLLO_PARAM_LEVELS;

/* Units\<key> -> Models\<TT> -> legacy global (x8p only) -> built-in default. */
static __inline unsigned long ApolloParamPick(int Model, int Id, const APOLLO_PARAM_LEVELS *L,
                                              unsigned char *Source)
{
    if (L->HaveUnit) {
        *Source = APOLLO_SRC_UNIT;
        return L->Unit;
    }
    if (L->HaveModel) {
        *Source = APOLLO_SRC_MODEL;
        return L->Model;
    }
    if (Model == APOLLO_MODEL_X8P && L->HaveGlobal) {
        *Source = APOLLO_SRC_GLOBAL;
        return L->Global;
    }
    *Source = APOLLO_SRC_DEFAULT;
    return ApolloParamDefault(Model, Id);
}

typedef struct _APOLLO_UNIT {
    int             Resolved;       /* Value[] valid for this start */
    int             Active;         /* bring-up allowed (device.c ApolloSupported) */
    int             KeyPresent;     /* Units\<key> exists */
    int             ModelKeyPresent;    /* Models\<TT> exists */
    unsigned long   Value[APOLLO_PARAM_COUNT];
    unsigned char   Source[APOLLO_PARAM_COUNT];     /* APOLLO_SRC_* */
    unsigned long   ModeAsked;      /* [Twin X G3] the Mode the registry asked for, before the rules */
} APOLLO_UNIT;

/* Bring-up paths this build has (bringup_plan.h): the x8p, and since G3 the Twin X Gen 1. */
static __inline int ApolloModelHasBringup(int Model)
{
    return ApolloBringupPlanFor(Model) != 0;
}

/* The highest Mode this build runs a model in: x8p 2, Twin X Gen 1 2 (G4; 1 at G3), anything else 0. */
static __inline unsigned long ApolloModelMaxMode(int Model)
{
    const APOLLO_BRINGUP_PLAN *p = ApolloBringupPlanFor(Model);

    return p != 0 ? p->MaxMode : APOLLO_MODE_OBSERVE;
}

/*
 * After every Value[] is picked: an unknown Mode is observe; a Mode above what this build runs
 * for the model comes down to that (none since G4); below FULL, strips and endpoints are off
 * whatever their values say; Active = an accepted model with a bring-up path and Mode >= 1.
 */
static __inline void ApolloUnitFinish(int Model, APOLLO_UNIT *U)
{
    U->ModeAsked = U->Value[APOLLO_PARAM_MODE];
    if (U->Value[APOLLO_PARAM_MODE] > APOLLO_MODE_FULL) {
        U->Value[APOLLO_PARAM_MODE] = APOLLO_MODE_OBSERVE;
        U->Source[APOLLO_PARAM_MODE] = APOLLO_SRC_MODE;
    }
    if (ApolloModelHasBringup(Model) && U->Value[APOLLO_PARAM_MODE] > ApolloModelMaxMode(Model)) {
        U->Value[APOLLO_PARAM_MODE] = ApolloModelMaxMode(Model);
        U->Source[APOLLO_PARAM_MODE] = APOLLO_SRC_MODE;
    }
    if (U->Value[APOLLO_PARAM_MODE] < APOLLO_MODE_FULL) {
        if (U->Value[APOLLO_PARAM_STRIPS_ENABLE] != 0) {
            U->Value[APOLLO_PARAM_STRIPS_ENABLE] = 0;
            U->Source[APOLLO_PARAM_STRIPS_ENABLE] = APOLLO_SRC_MODE;
        }
        if (U->Value[APOLLO_PARAM_ENABLED_ENDPOINTS] != 0) {
            U->Value[APOLLO_PARAM_ENABLED_ENDPOINTS] = 0;
            U->Source[APOLLO_PARAM_ENABLED_ENDPOINTS] = APOLLO_SRC_MODE;
        }
    }
    U->Active = (Model != APOLLO_MODEL_NONE && ApolloModelHasBringup(Model) &&
                 U->Value[APOLLO_PARAM_MODE] >= APOLLO_MODE_TRANSPORT);
    U->Resolved = 1;
}

/* ------------------------------------------------------------------ */
/* [Twin X G2] Device add: the unit before identification             */
/* ------------------------------------------------------------------ */

/*
 * At device add there is no BAR0 yet, only the PnP hardware ID. apollo.inf matches exactly two:
 *   PCI\VEN_1A00&DEV_0002&SUBSYS_00141A00   Apollo x8p
 *   PCI\VEN_1A00&DEV_0002&SUBSYS_00191A00   Apollo Twin X Gen 1 (Mode 0 observe by default; Mode 1 =
 *                                            the G3 bring-up + transport; Mode 2 (G4) = + its own
 *                                            Windows endpoints, MON L/R out and in)
 * The subsystem does not prove the model (spec-twinx 1.1: identification does that later), but
 * it is enough to decide what device add may create: the ACX circuits (Windows endpoints) and
 * the unit's log tag. Rule: the x8p's subsystem, or a hardware ID that could not be read, keeps
 * the behaviour every release before this one had (the x8p's table, circuits from the legacy
 * EnabledEndpoints). [G4] The Twin X's subsystem: the circuit objects of the Twin X's own table
 * (both rows); they stay unregistered until PrepareHardware adds the ones its unit value allows,
 * which is none below Mode 2 and none unless the identified model is the Twin X. Any other
 * subsystem: no circuits at all.
 */
#define APOLLO_SUBSYS_X8P           0x0014u
#define APOLLO_SUBSYS_TWINX         0x0019u

static __inline int ApolloHexDigit(wchar_t C)
{
    if (C >= L'0' && C <= L'9') {
        return (int)(C - L'0');
    }
    if (C >= L'A' && C <= L'F') {
        return (int)(C - L'A') + 10;
    }
    if (C >= L'a' && C <= L'f') {
        return (int)(C - L'a') + 10;
    }
    return -1;
}

/*
 * The subsystem ID (the first four hex digits after "SUBSYS_") of the first string of a
 * hardware ID list, e.g. PCI\VEN_1A00&DEV_0002&SUBSYS_00191A00&REV_00 -> 0x0019. Chars bounds
 * the scan. Returns 1 and sets *Subsys, or 0 if there is no well-formed SUBSYS_ field (eight hex
 * digits) before the string ends.
 */
static __inline int ApolloHwidSubsystem(const wchar_t *Hwid, unsigned long Chars, unsigned long *Subsys)
{
    static const wchar_t key[] = L"SUBSYS_";
    unsigned long i, k, v = 0;

    *Subsys = 0;
    if (Hwid == 0) {
        return 0;
    }
    for (i = 0; i < Chars && Hwid[i] != 0; i++) {
        for (k = 0; k < 7 && i + k < Chars && Hwid[i + k] == key[k]; k++) {
        }
        if (k < 7) {
            continue;
        }
        for (k = 0; k < 8; k++) {
            int d = (i + 7 + k < Chars) ? ApolloHexDigit(Hwid[i + 7 + k]) : -1;

            if (d < 0) {
                return 0;
            }
            if (k < 4) {
                v = (v << 4) | (unsigned long)d;
            }
        }
        *Subsys = v;
        return 1;
    }
    return 0;
}

/* Device add: is this the x8p's slot (or unknown), so today's x8p behaviour applies? */
static __inline int ApolloAddTimeX8pRules(int SubsysKnown, unsigned long Subsys)
{
    return !SubsysKnown || Subsys == APOLLO_SUBSYS_X8P;
}

/* [G4] Device add: whose endpoint table this device gets (APOLLO_MODEL_*, NONE = no table). */
static __inline int ApolloAddTimeModel(int SubsysKnown, unsigned long Subsys)
{
    if (ApolloAddTimeX8pRules(SubsysKnown, Subsys)) {
        return APOLLO_MODEL_X8P;
    }
    return (Subsys == APOLLO_SUBSYS_TWINX) ? APOLLO_MODEL_TWINX : APOLLO_MODEL_NONE;
}

/*
 * Device add: which endpoint rows get an ACX circuit. HaveGlobal/Global: the legacy
 * Parameters\EnabledEndpoints value (the x8p's only). [G4] The Twin X's subsystem: every row of
 * its table (its unit value narrows that at PrepareHardware); another subsystem: none.
 * [Endpoints] The x8p too: every row (HaveGlobal/Global unused; kept for the callers and tests).
 */
static __inline unsigned long ApolloAddTimeEndpoints(int SubsysKnown, unsigned long Subsys, int HaveGlobal,
                                                     unsigned long Global)
{
    int model = ApolloAddTimeModel(SubsysKnown, Subsys);

    (void)HaveGlobal;
    (void)Global;
    if (model == APOLLO_MODEL_TWINX) {
        /* [G5] every row (MON out/in + MIC/LINE 1-2): its unit value (default 0x3) picks the ones added */
        return APOLLO_TWX_EP_ALL;
    }
    if (model != APOLLO_MODEL_X8P) {
        return ApolloParamDef(APOLLO_PARAM_ENABLED_ENDPOINTS)->DefaultOther;
    }
    /*
     * [Endpoints] the x8p like the Twin X: every row's circuit object (they cost nothing until added); its
     * unit's Endpoints value (Units\<key> -> legacy global -> default) picks the ones PrepareHardware adds,
     * so a unit key can also widen the set, not only narrow it.
     */
    return APOLLO_X8P_EP_ALL;
}

/* The unit's log tag (APOLLO_INFO.LogTag, 8 bytes): "x8p", "twx", "s" + 4 hex digits, "unit". */
static __inline void ApolloLogTagFor(int SubsysKnown, unsigned long Subsys, char *Out)
{
    static const char hex[] = "0123456789abcdef";
    int i;

    for (i = 0; i < 8; i++) {
        Out[i] = 0;
    }
    if (!SubsysKnown) {
        Out[0] = 'u'; Out[1] = 'n'; Out[2] = 'i'; Out[3] = 't';
    } else if (Subsys == APOLLO_SUBSYS_X8P) {
        Out[0] = 'x'; Out[1] = '8'; Out[2] = 'p';
    } else if (Subsys == APOLLO_SUBSYS_TWINX) {
        Out[0] = 't'; Out[1] = 'w'; Out[2] = 'x';
    } else {
        Out[0] = 's';
        for (i = 0; i < 4; i++) {
            Out[1 + i] = hex[(Subsys >> (12 - 4 * i)) & 0xF];
        }
    }
}
