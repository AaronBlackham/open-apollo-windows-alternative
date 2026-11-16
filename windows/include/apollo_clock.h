/*
 * apollo_clock.h - [Clock] clock source, digital I/O formats, the front panel and the monitor link: apollo.sys
 * (driver\clock.c, panel.c), apollo-diag (clock, digital, panel, talkback) and Apollo Control
 * (app\src\clock_host.h; the UI contract is docs\apollo-control-units.md 1.10). The model data and the evidence
 * are in driver\clock_plan.h.
 *
 * Every IOCTL here is user level (FILE_READ_ACCESS), like SET_RATE and the monitor settings: no elevation.
 *
 *   GET_CLOCK  out APOLLO_CLOCK_INFO: this handle's unit. Lock-free like GET_RATE (answers during a change).
 *   SET_CLOCK  in APOLLO_CLOCK_SET, out APOLLO_CLOCK_INFO (the state after it). What = SOURCE (the clock),
 *              OPT_IN / OPT_OUT (an optical port's format) or SRC (SR Convert on the S/PDIF input). Completes
 *              with success whenever the request was understood: the outcome is Info.Result (APOLLO_CLOCK_ERR_*,
 *              0 = done) and Info.Why, as SET_RATE. Runs in the caller's thread (up to about 4 s for a source
 *              that does not lock: the write, 2 s for the acknowledgement, 1 s of lock, Internal back).
 *   GET_PANEL  out APOLLO_PANEL_INFO: the unit's front panel decoded from its readback (knob mode, monitor level,
 *              mute, dim, mono, ALT, talkback), the last 16 changes with who made them, the talkback channel, and
 *              the monitor link (driver-wide).
 *   SET_PANEL  in APOLLO_PANEL_LINK, out APOLLO_PANEL_INFO: the monitor link (driver-wide, from any unit's handle;
 *              not stored: off at every driver load until a host sets it).
 *
 * Function codes 0x53-0x56 (0x50-0x52 are speaker correction's, apollo_corr.h).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include "apollo_ioctl.h"

#define IOCTL_APOLLO_GET_CLOCK          APOLLO_CTL(0x53, APOLLO_USER)
#define IOCTL_APOLLO_SET_CLOCK          APOLLO_CTL(0x54, APOLLO_USER)
#define IOCTL_APOLLO_GET_PANEL          APOLLO_CTL(0x55, APOLLO_USER)
#define IOCTL_APOLLO_SET_PANEL          APOLLO_CTL(0x56, APOLLO_USER)

#define APOLLO_CLOCK_VERSION            1

/* Clock source ids (0xC074 low byte; = driver\clock_plan.h APOLLO_CK_*) */
#define APOLLO_CLOCK_SRC_SPDIF          0x00
#define APOLLO_CLOCK_SRC_ADAT           0x05
#define APOLLO_CLOCK_SRC_WORDCLOCK      0x07
#define APOLLO_CLOCK_SRC_INTERNAL       0x0C

/* APOLLO_CLOCK_SET.What */
#define APOLLO_CLOCK_WHAT_SOURCE        0   /* Value: APOLLO_CLOCK_SRC_* */
#define APOLLO_CLOCK_WHAT_OPT_IN        1   /* Value: 0 S/PDIF, 1 ADAT (Twin X, x8p) */
#define APOLLO_CLOCK_WHAT_OPT_OUT       2   /* Value: 0 S/PDIF, 1 ADAT (x8p) */
#define APOLLO_CLOCK_WHAT_SRC           3   /* Value: 0 off, 1 on: SR Convert on the S/PDIF input */

/* APOLLO_CLOCK_SET.Flags */
#define APOLLO_CLOCK_SET_TRY            0x1 /* allow what has not run on these units yet (Info.ProvenSources/ProvenDigital) */
#define APOLLO_CLOCK_SET_HOLD           0x2 /* SOURCE: stay on the external source when it loses lock (default: Internal after 1 s) */

typedef struct _APOLLO_CLOCK_SET {
    ULONG   Version;            /* APOLLO_CLOCK_VERSION */
    ULONG   What;               /* APOLLO_CLOCK_WHAT_* */
    ULONG   Value;
    ULONG   Flags;              /* APOLLO_CLOCK_SET_* */
} APOLLO_CLOCK_SET, *PAPOLLO_CLOCK_SET;

/* APOLLO_CLOCK_INFO.Result (= clock_plan.h APOLLO_CKCHK_* for 1-7) */
#define APOLLO_CLOCK_ERR_NONE           0
#define APOLLO_CLOCK_ERR_INVALID        1   /* not a source or a value this driver knows, or What unknown */
#define APOLLO_CLOCK_ERR_NOT_HERE       2   /* the model has no such jack or control */
#define APOLLO_CLOCK_ERR_NOT_OFFERED    3   /* the unit's own clock-source list (0xC08C) does not offer it */
#define APOLLO_CLOCK_ERR_UNPROVEN       4   /* never run on these units: refused without APOLLO_CLOCK_SET_TRY */
#define APOLLO_CLOCK_ERR_RATE           5   /* the source does not carry the unit's rate (S/PDIF, ADAT: 44.1-96 kHz) */
#define APOLLO_CLOCK_ERR_PORT           6   /* the optical port is in the other format, or the clock runs from the port */
#define APOLLO_CLOCK_ERR_SRC            7   /* SR Convert and the S/PDIF clock exclude each other */
#define APOLLO_CLOCK_ERR_NOT_READY      8   /* the unit does not run in Mode 2 with its transport */
#define APOLLO_CLOCK_ERR_BUSY           9   /* a rate or clock change is under way */
#define APOLLO_CLOCK_ERR_LOCK           10  /* the source did not lock (no signal, other rate): back on Internal */
#define APOLLO_CLOCK_ERR_HARDWARE       11  /* the unit stopped answering, or Internal did not lock again */

/* APOLLO_CLOCK_INFO.Flags */
#define APOLLO_CLOCK_F_LOCKED           0x0001  /* 0xC07C: the unit's rate, locked */
#define APOLLO_CLOCK_F_CHANGING         0x0002  /* a clock change is under way */
#define APOLLO_CLOCK_F_FELL_BACK        0x0004  /* Wanted lost lock (or did not come back after a restart): on Internal */
#define APOLLO_CLOCK_F_HOLD             0x0008  /* stays on Wanted when it loses lock */
#define APOLLO_CLOCK_F_REAPPLY          0x0010  /* Wanted goes on again after a transport restart (pending) */
#define APOLLO_CLOCK_F_DIG_KNOWN        0x0020  /* the formats below were set by this driver since the connect */
#define APOLLO_CLOCK_F_IN_ADAT          0x0040  /* optical input ADAT (else S/PDIF; the connect batch leaves S/PDIF) */
#define APOLLO_CLOCK_F_OUT_ADAT         0x0080  /* optical output ADAT (x8p) */
#define APOLLO_CLOCK_F_SRC              0x0100  /* SR Convert on */
#define APOLLO_CLOCK_F_RB_LOCK_VALID    0x0200  /* readback word 6 bit 30: the unit reports its lock there too */
#define APOLLO_CLOCK_F_RB_LOCKED        0x0400  /* readback word 6 bit 31 */
#define APOLLO_CLOCK_F_COMBINED         0x0800  /* the unit runs in the combined device with another unit */
#define APOLLO_CLOCK_F_TIMELINE         0x1000  /* ... and its clock is the combined device's timeline */

/* APOLLO_CLOCK_INFO.Digital / ProvenDigital (= clock_plan.h APOLLO_DIG_HAS_*) */
#define APOLLO_CLOCK_DIG_OPT_IN         0x01    /* optical input with a format choice */
#define APOLLO_CLOCK_DIG_OPT_OUT        0x02    /* optical output with a format choice */
#define APOLLO_CLOCK_DIG_COAX           0x04    /* coaxial S/PDIF in and out */
#define APOLLO_CLOCK_DIG_SRC            0x08    /* SR Convert */

#define APOLLO_CLOCK_WHY_CHARS          112

typedef struct _APOLLO_CLOCK_INFO {
    ULONG   Version;            /* APOLLO_CLOCK_VERSION */
    ULONG   Bytes;              /* sizeof(APOLLO_CLOCK_INFO) */
    ULONG   Model;              /* APOLLO_INFO_MODEL_* */
    ULONG   Source;             /* APOLLO_CLOCK_SRC_* the unit runs on now */
    ULONG   Wanted;             /* the last source asked for (= Source unless it fell back or waits to go on again) */
    ULONG   Sources;            /* bit n = source n the model has a jack for */
    ULONG   Offered;            /* 0xC08C [31:16]: the unit's own list, bit n = source n */
    ULONG   Usable;             /* bit n = source n a SET_CLOCK (with TRY) may ask for now: jack, offered, rate, port */
    ULONG   ProvenSources;      /* bit n = source n has run on these units (the others need TRY) */
    ULONG   Rate;               /* Hz the unit is set to */
    ULONG   RatesOnSource;      /* bit n = rate index n the unit may run at on Source (S/PDIF, ADAT: 44.1-96 kHz) */
    ULONG   RateInfo;           /* 0xC07C, last read: [15:8] rate index, [1:0] lock (1 = locked) */
    ULONG   ClockCfg;           /* 0xC074 as the unit holds it (index << 8 | source), sampled every 100 ms */
    ULONG   ClockInfo;          /* 0xC084, sampled every 100 ms: read 48000 at G6; what it reports for an external
                                   source is not known yet (the detected rate, or the set one) */
    ULONG   MeasuredMilliHz;    /* frames per second the unit's position moved over the last second, x1000 (the
                                   rate its clock really runs at against the PC's clock); 0 = transport stopped */
    ULONG   Flags;              /* APOLLO_CLOCK_F_* */
    ULONG   Result;             /* SET_CLOCK: APOLLO_CLOCK_ERR_*; GET_CLOCK: the last change's */
    LONG    Status;             /* NTSTATUS of the step that failed (0 = none) */
    ULONG   LastMs;             /* how long the last change took */
    ULONG   Changes;            /* source changes done since the driver loaded */
    ULONG   Fallbacks;          /* times an external source lost lock and the unit went back to Internal */
    ULONG   LockLosses;         /* times 0xC07C dropped its lock bit while on an external source */
    ULONG   Digital;            /* APOLLO_CLOCK_DIG_*: what the model has */
    ULONG   ProvenDigital;      /* APOLLO_CLOCK_DIG_* whose writes have run on these units (the others need TRY) */
    ULONG   AdatIn, AdatOut;    /* ADAT channels carrying audio at this rate and format (unknown format: as ADAT) */
    ULONG   SpdifIn, SpdifOut;  /* S/PDIF channels carrying audio (0 or 2) */
    ULONG   Reserved[6];
    CHAR    Why[APOLLO_CLOCK_WHY_CHARS];    /* the last change's outcome, or what limits the clock now */
} APOLLO_CLOCK_INFO, *PAPOLLO_CLOCK_INFO;

/* ------------------------------------------------------------------ */
/* Front panel and the monitor link                                    */
/* ------------------------------------------------------------------ */

/* Fields (APOLLO_PANEL_EVENT.Field; = clock_plan.h APOLLO_PANEL_*) */
#define APOLLO_PANEL_F_LEVEL            1   /* monitor level, raw 0..192 = 192 + 2 dB */
#define APOLLO_PANEL_F_HP               2   /* HP 1 level, raw */
#define APOLLO_PANEL_F_MUTE             3
#define APOLLO_PANEL_F_MONO             4
#define APOLLO_PANEL_F_DIM              5
#define APOLLO_PANEL_F_ALT              6   /* 0 MAIN, 1 ALT 1, 2 ALT 2 */
#define APOLLO_PANEL_F_TALK             7   /* talkback */
#define APOLLO_PANEL_F_KNOB             8   /* knob mode: 0 PREAMP, 1 MONITOR, 2 HP */
#define APOLLO_PANEL_F_PREAMP           9   /* the preamp the knob edits in PREAMP mode, 0-based */
#define APOLLO_PANEL_F_LOCK             10  /* readback clock lock: -1 not reported, 0, 1 */

/* APOLLO_PANEL_EVENT.Origin */
#define APOLLO_PANEL_FROM_PANEL         0   /* the unit's own knob or button */
#define APOLLO_PANEL_FROM_HOST          1   /* a setting from Windows (Apollo Control, apollo-diag) coming back */
#define APOLLO_PANEL_FROM_LINK          2   /* the monitor link (another unit's knob, or the talkback dim) */
#define APOLLO_PANEL_FROM_UNIT          3   /* the unit's own reaction (it dims itself on talkback) */

/* APOLLO_PANEL_INFO.Flags */
#define APOLLO_PANEL_INFO_VALID         0x0001  /* a readback has been decoded since the connect */
#define APOLLO_PANEL_INFO_MONITOR_UNIT  0x0002  /* this unit is the link's monitor unit */
#define APOLLO_PANEL_INFO_KNOB_UNIT     0x0004  /* the link mirrors this unit's knob and buttons to the monitor unit */
#define APOLLO_PANEL_INFO_TALK_DIMMED   0x0008  /* the link dimmed this unit for talkback (it undims it after) */
#define APOLLO_PANEL_INFO_UNIT_DIMS     0x0010  /* seen: the unit dims itself when talkback goes on */
#define APOLLO_PANEL_INFO_LINK_PENDING  0x0020  /* a knob change waits for the monitor unit (busy) */

/* SET_PANEL Flags (= clock_plan.h APOLLO_LINK_*) */
#define APOLLO_PANEL_LINK_LEVEL         0x01    /* knob turns move the monitor unit's level by the same steps */
#define APOLLO_PANEL_LINK_MUTE          0x02
#define APOLLO_PANEL_LINK_DIM           0x04
#define APOLLO_PANEL_LINK_MONO          0x08
#define APOLLO_PANEL_LINK_TALK_DIM      0x10    /* talkback on any unit dims the monitor unit */

#define APOLLO_PANEL_EVENTS             16
#define APOLLO_PANEL_NO_SLOT            0xFFFFFFFFul

typedef struct _APOLLO_PANEL_EVENT {
    ULONG   Seq;                /* 1, 2, ... per unit since the driver loaded */
    USHORT  Field;              /* APOLLO_PANEL_F_* */
    USHORT  Origin;             /* APOLLO_PANEL_FROM_* */
    LONG    Value;              /* the new value */
    ULONG   TimeMs;             /* ms since the unit connected */
} APOLLO_PANEL_EVENT;

typedef struct _APOLLO_PANEL_INFO {
    ULONG   Version;            /* APOLLO_CLOCK_VERSION */
    ULONG   Bytes;              /* sizeof(APOLLO_PANEL_INFO) */
    ULONG   Model;              /* APOLLO_INFO_MODEL_* */
    ULONG   Flags;              /* APOLLO_PANEL_INFO_* */
    ULONG   ReadbackSeq;        /* the readback this state was decoded from */
    ULONG   Seq;                /* the newest event's Seq (0 = none yet): poll and compare */
    /* the state */
    ULONG   Level;              /* raw 0..192 (dB = (raw - 192) / 2) */
    ULONG   HpLevel;            /* raw */
    ULONG   Mute, Mono, Dim, Alt, Talk;
    ULONG   Knob;               /* 0 PREAMP, 1 MONITOR, 2 HP (the MONITOR/PREAMP button; read-only) */
    ULONG   Preamp;             /* in PREAMP mode: the preamp the knob edits, 0-based */
    LONG    Lock;               /* -1 not reported, 0, 1 */
    /* talkback */
    ULONG   TalkRecChannels;    /* AX_REC_CH now (the Twin X: 34, its TALKBACK 1/2 at record slots 32/33) */
    ULONG   TalkSlot[2];        /* record slots of TALKBACK 1/2 (0x0B01/0x0B02), APOLLO_PANEL_NO_SLOT = not in the frame */
    ULONG   TalkStrip;          /* 1: the talkback mic's Input Strip runs (StripTalkback, GET_STRIPS version 6, docs/STRIPS.md
                                   15); 0: none: the mic carries silence */
    /* the monitor link (driver-wide) */
    ULONG   LinkFlags;          /* APOLLO_PANEL_LINK_* (0 = off) */
    ULONG   LinkApplied;        /* changes mirrored to the monitor unit */
    ULONG   LinkBusy;           /* times the monitor unit was busy (retried 10 ms later) */
    ULONG   LinkDropped;        /* changes dropped: the monitor unit is not running */
    ULONGLONG LinkMonitorUid;   /* the monitor unit's HW_UID (0 = none) */
    ULONG   EventCount;         /* valid entries in Event[], oldest first */
    ULONG   Reserved[3];
    APOLLO_PANEL_EVENT Event[APOLLO_PANEL_EVENTS];
} APOLLO_PANEL_INFO, *PAPOLLO_PANEL_INFO;

typedef struct _APOLLO_PANEL_LINK {
    ULONG   Version;            /* APOLLO_CLOCK_VERSION */
    ULONG   Flags;              /* APOLLO_PANEL_LINK_*; 0 = off */
    ULONGLONG MonitorUid;       /* the unit with the speakers (HW_UID); every other running unit is a knob unit */
} APOLLO_PANEL_LINK, *PAPOLLO_PANEL_LINK;
