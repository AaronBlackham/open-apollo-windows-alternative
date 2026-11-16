/*
 * strip_record.h - the warm-start adoption record's layout (strip_adopt.h has the logic,
 * docs/STRIPS.md 12). Types only, so apollo.h can hold records without strip_plan.h.
 * The includer defines ULONG.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#define STRIP_REC_MAGIC         0x52537641u     /* "AvSR" */
#define STRIP_REC_VERSION       1u
/*
 * [Virtual] (docs/STRIPS.md 13) The same 50 words; Mask holds the plan mask (StripMask | StripVirtualMask
 * << 8). A record whose Mask names Virtual strips is sealed as version 2, so a build without Virtual strips (which
 * trusts version 1 only) never adopts, rebuilds over or keeps strips it does not know: it asks for a power cycle.
 * Every other record is version 1, byte for byte what the builds before wrote.
 */
#define STRIP_REC_VERSION_VIRTUAL 2u
/*
 * [AuxStrips] (docs/STRIPS.md 14) The same 50 words; Mask bits 16-19 name the aux-return strips (StripAuxMask << 16,
 * DSP inputs 0x10-0x13), with or without Virtual strips. Such a record is sealed as version 3: a build that knows
 * versions 1 and 2 only (and refuses any Mask bit past 15) never adopts, rebuilds over or keeps strips it does not
 * know. Versions 1 and 2 keep their meaning and are read as before.
 */
#define STRIP_REC_VERSION_AUX   3u
#define STRIP_REC_DSPS          8u              /* = APOLLO_MAX_DSPS */
#define STRIP_REC_INPUTS        8u              /* = STRIP_PLAN_INPUTS */

/* What the strip DSP holds of this driver's */
#define STRIP_REC_NONE          0u  /* nothing: cold start, a proven teardown, or dropped by a connect */
#define STRIP_REC_RUNNING       1u  /* the plan below runs there */
#define STRIP_REC_UNKNOWN       2u  /* a pass or a teardown was under way: unknown until proven */

/*
 * [Warm] Plugins: this bit (with any count) = a warm start put the plain strips back over a chain (RESTORE) that no
 * teardown took off, so its modules may still be resident on the strip DSP: adopted again from then on, but plug-ins
 * wait for the power cycle that clears it (a cold start writes 0). docs/STRIPS.md 12.5.
 */
#define STRIP_REC_PLUGINS_STALE 0x80000000u

/* Registry: Parameters\Units\<unit key>\StripRecord, REG_BINARY, exactly sizeof(STRIP_RECORD). */
#define STRIP_REC_VALUE_NAME    L"StripRecord"

typedef struct _STRIP_RECORD {
    ULONG   Magic;
    ULONG   Version;
    ULONG   Size;
    ULONG   Anchored;               /* 1: only this driver used the DSPs since the last power-up */
    ULONG   HwUidLo, HwUidHi;
    ULONG   DeviceType;
    ULONG   FpgaRev;
    ULONG   FwRelease;
    ULONG   NumDsps;
    ULONG   BootWord[STRIP_REC_DSPS];   /* right after this driver's last ring connect */
    ULONG   State;                  /* STRIP_REC_* */
    ULONG   Keep;                   /* RUNNING strips are left running at the D0 exit */
    ULONG   Dsp, Mask, Rate, Limit; /* the plan: strip DSP, StripMask [| StripVirtualMask << 8] [| StripAuxMask << 16], Hz, stage */
    ULONG   ModuleHash;             /* the pins the strips were built from (strips.c StripsModulesHash) */
    ULONG   PlanHash;               /* strip_adopt.h StripPlanHash (StripPlanHashPm: the Virtual strips' words too) */
    ULONG   Header[STRIP_REC_INPUTS];   /* the Mic strips' (the Virtual headers follow from the plan, which the hash covers) */
    ULONG   Geom[11];               /* strip DSP pool registers: size 0-3, base 0-3, scratch 1-3 */
    ULONG   Plugins;                /* plug-in instances placed on the strip DSP */
    ULONG   BuiltDate;              /* (days since 2007-01-01 << 12) + minute of day */
    ULONG   Adoptions;              /* warm starts that adopted these strips */
    ULONG   Writes;                 /* every write of this record */
    ULONG   Check;                  /* FNV-1a of everything above */
} STRIP_RECORD;
