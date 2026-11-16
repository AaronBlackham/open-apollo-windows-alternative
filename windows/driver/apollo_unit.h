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
 * Device add, before identification, goes by the PCI hardware ID instead (below).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once
#ifndef _WCHAR_T_DEFINED
#include <stddef.h>             /* wchar_t (the kernel build has it from ntddk.h) */
#endif
#include "apollo_model.h"
#include "bringup_plan.h"       /* which models have a bring-up path, and up to which Mode */

/* Mode: what the driver does with the unit */
#define APOLLO_MODE_OBSERVE         0   /* identify + quiesce, nothing else */
#define APOLLO_MODE_TRANSPORT       1   /* bring-up + transport; no strips, no Windows endpoints */
#define APOLLO_MODE_FULL            2   /* everything the values and the model's plan allow (Twin X:
                                           endpoints, monitor settings, strips (StripsEnable), preamps;
                                           no ASIO) */

/* The x8p's built-in defaults, as the driver had them before per-unit keys */
#define APOLLO_DEF_RENDER_LEAD      144u
#define APOLLO_DEF_CAPTURE_GUARD    16u
#define APOLLO_DEF_ENDPOINTS_X8P    0x1FFFFu    /* = APOLLO_X8P_EP_DEFAULT (circuit.c asserts): every output,
                                                   the loopback, MIC/LINE 1-8; VIRTUAL/S/PDIF in opt-in */
#define APOLLO_DEF_ENDPOINTS_TWINX  0x3Fu       /* = APOLLO_TWX_EP_DEFAULT: MON L/R, LINE 3-4, HP out, the
                                                   loopback, MIC/LINE 1-2 */
#define APOLLO_DEF_BATCH_X8P        1u      /* = APOLLO_MIXER_BATCH_X8P_SAFE (unit.c asserts) */
/*
 * [AuxStrips] The x8p's built-in StripAuxMask (docs/STRIPS.md 14): 0x5, the Input Strips on the L returns of AUX 1 and
 * AUX 2 (0x10, 0x12; the model allows no more, strip_plan.h). Nov 16 2026 on the x8p: sends reach the aux buses, the
 * return reaches both MON sides at its level, mute works, input 1 and the other inputs record as before. ident-test
 * checks the defaults against these constants; every other model stays 0.
 */
#define APOLLO_DEF_STRIP_AUX_MASK_X8P   0x5u
#define APOLLO_DEF_STRIP_AUX_MASK_TWINX 0x5u    /* the same on the Twin X Gen 1: proven there Nov 16 2026 (AUX 1/2 -43.0, return on
                                                   both MON sides, mute, no leaks) */
/*
 * [TalkStrip] The built-in StripTalkback (docs/STRIPS.md 15), one constant per model: 0 on both until the hardware run of
 * this build (a throwaway build's plain Input Strip on 0x0A carried the Twin X's room into TALKBACK 1/2 with TALK on,
 * Nov 16 2026). 1 builds the talkback mic's Input Strip. ident-test checks the defaults against these constants; every
 * other model 0.
 */
#define APOLLO_DEF_STRIP_TALKBACK_X8P   0u    /* Nov 16 2026: the strip builds, but the x8p's talkback mic never reaches DSP
                                                   input 0x0A (silent into MON and AUX 1 with TALK on): an unknown enable */
#define APOLLO_DEF_STRIP_TALKBACK_TWINX 1u    /* proven Nov 16 2026: TALK on = the room on its talkback record slots (-45.5 dBFS),
                                                   into AUX 1 through its send, kept out of the monitor mix; TALK off = silence */

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
#define APOLLO_PARAM_WARM_BOOT_TRANSPORT    11  /* device.c: boot transport on a warm start too */
#define APOLLO_PARAM_AUDIO_RING_PAGES       12  /* audio.c: 1 = audio rings always one common buffer per page */
#define APOLLO_PARAM_CAPTURE_TRIM           13  /* agg.c: combined capture alignment trim, signed frames */
#define APOLLO_PARAM_RENDER_TRIM            14  /* agg.c: combined playback alignment trim, signed frames */
#define APOLLO_PARAM_SAMPLE_RATE            15  /* rate.c: the rate this unit comes up at (Hz) */
#define APOLLO_PARAM_STRIP_ADOPT            16  /* strips.c: keep Live strips at the D0 exit, adopt them warm */
#define APOLLO_PARAM_STRIP_WARM_REBUILD     17  /* strips.c: build again at a warm start proven ours */
#define APOLLO_PARAM_STRIP_WARM_PLUGINS     18  /* strips.c/plugin.c: plug-ins on adopted or rebuilt warm strips */
#define APOLLO_PARAM_STRIP_VIRTUAL_MASK     19  /* strips.c: Input Strips on VIRTUAL 1-8 (docs/STRIPS.md 13) */
#define APOLLO_PARAM_BUS_ENABLE             20  /* cues.c: DSP output buses switched on at every connect */
#define APOLLO_PARAM_STRIP_AUX_MASK         21  /* strips.c: Input Strips on the aux returns 0x10-0x13 (docs/STRIPS.md 14) */
#define APOLLO_PARAM_STRIP_TALKBACK         22  /* strips.c: the Input Strip on the talkback mic, DSP input 0x0A (STRIPS.md 15) */
#define APOLLO_PARAM_COUNT                  23

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
    unsigned long   DefaultTwinX;   /* the Twin X Gen 1's built-in default */
} APOLLO_PARAM_DEF;

static __inline const APOLLO_PARAM_DEF *ApolloParamDef(int Id)
{
    static const APOLLO_PARAM_DEF t[APOLLO_PARAM_COUNT] = {
        { "Mode",                    L"Mode",                    APOLLO_MODE_FULL, APOLLO_MODE_OBSERVE, 0, APOLLO_MODE_OBSERVE },
        /* The Twin X: strips off by default; when on, the whole chain (StripSteps 0) on both
           inputs (StripMask 0x3) on DSP 1 (strip_plan.h) */
        { "StripsEnable",            L"StripsEnable",            0, 0, 0, 0 },
        { "StripSteps",              L"StripSteps",              1, 1, 0, 0 },
        { "StripMask",               L"StripMask",               0x01, 0x01, 1, 0x03 },
        { "StripDsp",                L"StripDsp",                1, 1, 0, 1 },
        /* Other models: 0 (nothing). The Twin X: 1 = its model-safe batch (bringup_plan.h, spec-twinx 4.3). */
        { "MixerInitBatch",          L"MixerInitBatch",          APOLLO_DEF_BATCH_X8P, 0, 0, 1 },
        { "RenderLeadFrames",        L"RenderLeadFrames",        APOLLO_DEF_RENDER_LEAD, APOLLO_DEF_RENDER_LEAD, 0, APOLLO_DEF_RENDER_LEAD },
        { "CaptureGuardFrames",      L"CaptureGuardFrames",      APOLLO_DEF_CAPTURE_GUARD, APOLLO_DEF_CAPTURE_GUARD, 0, APOLLO_DEF_CAPTURE_GUARD },
        { "StartTransportAtConnect", L"StartTransportAtConnect", 1, 1, 0, 1 },
        /* Bit i = row i of the model's endpoint_plan.h table, used in Mode 2 only (forced 0 below);
           other models: no Windows endpoints. Named "Endpoints", not the older "EnabledEndpoints": the
           rows were renumbered, and a mask stored for the old 10-row x8p / 4-row Twin X tables (0x3FF,
           0xF) would hide every new output, so an old value is left where it is and ignored; absent =
           the model's default set. */
        { "Endpoints",               L"Endpoints",               APOLLO_DEF_ENDPOINTS_X8P, 0, 1, APOLLO_DEF_ENDPOINTS_TWINX },
        /* Strip diagnostics (strip_plan.h STRIP_PROBE_OFF = 0) */
        { "StripProbe",              L"StripProbe",              0, 0, 0, 0 },
        /* 1 = run the boot transport + early ACEFACE on a warm start too (spec-twinx 2 notes: the
           fallback if a warm ACEFACE with the transport stopped times out). The x8p has no boot
           transport and ignores it. */
        { "WarmBootTransport",       L"WarmBootTransport",       0, 0, 0, 0 },
        /* 0 = each audio ring one contiguous 4 MiB when there is one below 4 GiB, else one common
           buffer per SG page (ring_map.h); 1 = always per page (to run that path at will). */
        { "AudioRingPages",          L"AudioRingPages",          0, 0, 0, 0 },
        /* The combined device's per-unit alignment (spec 6.3.5): a REG_DWORD read as a signed number of
           frames, -64..64 (agg.c ApolloAggTrimOf), e.g. 0xFFFFFFFB = -5. Positive delays that unit's capture /
           playback in the combined device. 0 = no trim. Read when the unit joins (its next start). */
        { "CaptureTrimFrames",       L"CaptureTrimFrames",       0, 0, 1, 0 },
        { "RenderTrimFrames",        L"RenderTrimFrames",        0, 0, 1, 0 },
        /* The sample rate the unit comes up at, Hz (rate_plan.h; anything but the six rates = 48000).
           SET_RATE writes it under Units\<key> (apollo_rate.h); read at every start. */
        { "SampleRate",              L"SampleRate",              48000, 48000, 0, 48000 },
        /* docs/STRIPS.md 12. StripAdopt 1: a D0 exit leaves Live strips running (plug-in chains put back
           on the plain strips first) and the next warm start adopts them when the record proves them this
           driver's; 0 = the old rule (torn down at every D0 exit, a power cycle for strips). The Twin X's warm
           start ring-connects, which drops them, so it keeps the teardown whatever this says.
           StripWarmRebuild 1: a warm start that proves only this driver used the DSPs since the last power-up
           builds the strips again instead of asking for a power cycle. The Twin X needs it after every warm
           start (on). The x8p: on since Nov 15 2026, when the first x8p warm rebuild (96 kHz, no ring connect)
           had all five module uploads accepted by DSP 1 (docs/STRIPS.md 12.4). Since Nov 16 2026 a rebuild also
           wins over adoption (strip_adopt.h StripAdoptDecide): adopted x8p strips recorded exact zero on every
           input after a warm restart, a rebuilt VIRTUAL 1 recorded. StripWarmRebuild 0 brings adoption back. */
        { "StripAdopt",              L"StripAdopt",              1, 0, 0, 1 },
        { "StripWarmRebuild",        L"StripWarmRebuild",        1, 0, 0, 1 },
        /* docs/STRIPS.md 12.5. StripWarmPlugins 1: after a warm start whose strips were adopted
           (the x8p: no ring connect, the DSPs' session byte 0x07) or rebuilt (the Twin X) from an anchored record
           that saw no plug-in left on the strip DSP, that DSP takes plug-ins as after a cold start; its first
           acknowledged upload proves the warm session takes uploads, an unanswered one quarantines it (power cycle).
           0 = plug-ins wait for a power cycle after every warm start (the rule before this value). */
        { "StripWarmPlugins",        L"StripWarmPlugins",        1, 0, 0, 1 },
        /* docs/STRIPS.md 13. Bit n-1: the plain Input Strip (0x83E) on VIRTUAL n, built with the Mic
           strips in the same pass (StripsEnable 1, its StripSteps), so the Console mixes and records what plays into that
           Virtual channel; its bus 0x18 + n - 1 then takes monitor-mix, cue and aux sends. The x8p has VIRTUAL 1-8, the
           Twin X 1-4. 0 on every model until it is proven on hardware (tools\phase1\step-virtual.ps1). */
        { "StripVirtualMask",        L"StripVirtualMask",        0, 0, 1, 0 },
        /* cues.c ApolloCuesBusEnable: {0x001C0003, bus, 1, 0} at every connect for each bus of bits 0-6 (0 monitor,
           1/2 CUE 1/2, 3/4 AUX 1/2, 5/6 CUE 3/4) on each DSP of bits 8-15 (none = every DSP). 0 = nothing sent. */
        { "BusEnable",               L"BusEnable",               0, 0, 1, 0 },
        /* docs/STRIPS.md 14. Bits 0-3: the plain Input Strip (0x83E) on DSP inputs 0x10, 0x11, 0x12, 0x13 (AUX 1 L, AUX 1 R,
           AUX 2 L, AUX 2 R, the aux returns), built with the Mic and Virtual strips in the same pass (StripsEnable 1). Without
           one an aux return mixes nothing and records silence (measured on the x8p, Nov 16 2026). Only the L inputs are built
           (the models allow 0x05: a mono strip on 0x11/0x13 covered input 1's record slot): mono aux buses, the return on
           both monitor sides, the sends' left entry. The x8p and the Twin X have AUX 1-2; every other model: none. The
           defaults: APOLLO_DEF_STRIP_AUX_MASK_X8P / _TWINX (0x5 on both). */
        { "StripAuxMask",            L"StripAuxMask",            APOLLO_DEF_STRIP_AUX_MASK_X8P, 0, 1, APOLLO_DEF_STRIP_AUX_MASK_TWINX },
        /* docs/STRIPS.md 15. Bit 0: the plain Input Strip (0x83E) on the talkback mic, DSP input 0x0A (talkback_synch_in 10 on
           the x8p and the Twin X), built with the other strips in the same pass (StripsEnable 1). Without it the mic carries
           silence; with it bus 10 is mixed on the strip DSP and the unit gates it with TALK. Only models whose monitor plan
           has a talkback bus (the x8p, the Twin X); every other model: none. The defaults: APOLLO_DEF_STRIP_TALKBACK_X8P /
           _TWINX. */
        { "StripTalkback",           L"StripTalkback",           APOLLO_DEF_STRIP_TALKBACK_X8P, 0, 1, APOLLO_DEF_STRIP_TALKBACK_TWINX },
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
    unsigned long   ModeAsked;      /* the Mode the registry asked for, before the rules */
} APOLLO_UNIT;

/* Bring-up paths this build has (bringup_plan.h): the x8p and the Twin X Gen 1. */
static __inline int ApolloModelHasBringup(int Model)
{
    return ApolloBringupPlanFor(Model) != 0;
}

/* The highest Mode this build runs a model in: x8p 2, Twin X Gen 1 2, anything else 0. */
static __inline unsigned long ApolloModelMaxMode(int Model)
{
    const APOLLO_BRINGUP_PLAN *p = ApolloBringupPlanFor(Model);

    return p != 0 ? p->MaxMode : APOLLO_MODE_OBSERVE;
}

/*
 * After every Value[] is picked: an unknown Mode is observe; a Mode above what this build runs
 * for the model comes down to that; below FULL, strips and endpoints are off
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
/* Device add: the unit before identification                          */
/* ------------------------------------------------------------------ */

/*
 * At device add there is no BAR0 yet, only the PnP hardware ID. apollo.inf matches exactly two:
 *   PCI\VEN_1A00&DEV_0002&SUBSYS_00141A00   Apollo x8p
 *   PCI\VEN_1A00&DEV_0002&SUBSYS_00191A00   Apollo Twin X Gen 1 (Mode 0 observe by default; Mode 1 =
 *                                            bring-up + transport; Mode 2 = + its own Windows endpoints)
 * The subsystem does not prove the model (spec-twinx 1.1: identification does that later), but
 * it is enough to decide what device add may create: the ACX circuits (Windows endpoints) and
 * the unit's log tag. The x8p's subsystem, or a hardware ID that could not be read, gets the
 * circuit objects of the x8p's table; the Twin X's subsystem those of the Twin X's table. They
 * stay unregistered until PrepareHardware adds the ones its unit value allows, which is none
 * below Mode 2 and none unless the identified model is that table's. Any other subsystem: no
 * circuits at all.
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

/* Device add: is this the x8p's slot (or unknown), so the x8p's rules apply? */
static __inline int ApolloAddTimeX8pRules(int SubsysKnown, unsigned long Subsys)
{
    return !SubsysKnown || Subsys == APOLLO_SUBSYS_X8P;
}

/* Device add: whose endpoint table this device gets (APOLLO_MODEL_*, NONE = no table). */
static __inline int ApolloAddTimeModel(int SubsysKnown, unsigned long Subsys)
{
    if (ApolloAddTimeX8pRules(SubsysKnown, Subsys)) {
        return APOLLO_MODEL_X8P;
    }
    return (Subsys == APOLLO_SUBSYS_TWINX) ? APOLLO_MODEL_TWINX : APOLLO_MODEL_NONE;
}

/*
 * Device add: which endpoint rows get an ACX circuit object (they cost nothing until added): every
 * row of the slot's table; another subsystem none. The unit's Endpoints value (Units\<key> -> Models
 * -> legacy global (x8p) -> default) picks the ones PrepareHardware adds, so a unit key can also
 * widen the default set, not only narrow it. HaveGlobal/Global (the legacy global) are unused, kept
 * for the callers and tests.
 */
static __inline unsigned long ApolloAddTimeEndpoints(int SubsysKnown, unsigned long Subsys, int HaveGlobal,
                                                     unsigned long Global)
{
    int model = ApolloAddTimeModel(SubsysKnown, Subsys);

    (void)HaveGlobal;
    (void)Global;
    if (model == APOLLO_MODEL_TWINX) {
        return APOLLO_TWX_EP_ALL;
    }
    if (model != APOLLO_MODEL_X8P) {
        return ApolloParamDef(APOLLO_PARAM_ENABLED_ENDPOINTS)->DefaultOther;
    }
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
