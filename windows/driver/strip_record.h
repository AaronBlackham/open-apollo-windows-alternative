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
    ULONG   Dsp, Mask, Rate, Limit; /* the plan: strip DSP, StripMask, Hz, stage (STRIP_STEP_*) */
    ULONG   ModuleHash;             /* the pins the strips were built from (strips.c StripsModulesHash) */
    ULONG   PlanHash;               /* strip_adopt.h StripPlanHash */
    ULONG   Header[STRIP_REC_INPUTS];
    ULONG   Geom[11];               /* strip DSP pool registers: size 0-3, base 0-3, scratch 1-3 */
    ULONG   Plugins;                /* plug-in instances placed on the strip DSP */
    ULONG   BuiltDate;              /* (days since 2007-01-01 << 12) + minute of day */
    ULONG   Adoptions;              /* warm starts that adopted these strips */
    ULONG   Writes;                 /* every write of this record */
    ULONG   Check;                  /* FNV-1a of everything above */
} STRIP_RECORD;
