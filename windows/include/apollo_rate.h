/*
 * apollo_rate.h - [Rate] the sample rate of a unit and of the combined device: apollo.sys
 * (driver\rate.c, agg.c), apollo-diag (rate), the ASIO driver (avium-asio.dll) and Apollo Control
 * (app\src\rate_host.h; the UI contract is docs\apollo-control-units.md 1.6).
 *
 * Both IOCTLs are user level (FILE_READ_ACCESS), like the preamp and monitor settings: no elevation.
 *
 *   GET_RATE  out APOLLO_RATE_INFO: this handle's unit, and the combined device it may be part of.
 *   SET_RATE  in APOLLO_RATE_SET, out APOLLO_RATE_INFO (the state after the change). Completes
 *             with success whenever the request was understood: the outcome is Info.Result
 *             (APOLLO_RATE_ERR_*; 0 = done) with Info.Why, because a METHOD_BUFFERED failure would
 *             bring nothing back (the agg SET_CONFIG rule). STATUS_INVALID_PARAMETER only for a
 *             malformed request (size, version).
 *
 * A change takes about 2-3 s per unit (the kext's order: transport stopped, clock written and
 * acknowledged, the firmware's rate polled, every DSP told, 1.5 s hold, transport and engine started
 * again, lock checked). SET_RATE runs it in the caller's thread (the request never waits in the
 * unit's IOCTL queue), so GET_RATE, GET_INFO and the meters still answer meanwhile;
 * APOLLO_RATE_F_CHANGING says one is under way.
 *
 * What follows a change (driver side): Windows endpoints advertise the new rate only (ACX format
 * list + format change); a stream opened at the old rate goes silent until Windows reopens it; the
 * input strips are torn down and rebuilt at the new rate; plug-in inserts come off (48 kHz only in
 * this build); speaker correction stops until a profile for the new rate is loaded; the combined
 * device re-forms its followers. The rate is stored per unit (Units\<key>\SampleRate) and applied
 * at the next bring-up.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include "apollo_ioctl.h"

#define IOCTL_APOLLO_GET_RATE           APOLLO_CTL(0x11, APOLLO_USER)
#define IOCTL_APOLLO_SET_RATE           APOLLO_CTL(0x12, APOLLO_USER)

#define APOLLO_RATE_VERSION             1

/* APOLLO_RATE_SET.Flags */
#define APOLLO_RATE_SET_COMBINED        0x1     /* every unit of the combined device: master first, then followers */
#define APOLLO_RATE_SET_NO_STORE        0x2     /* do not keep it for the next bring-up (a test) */

typedef struct _APOLLO_RATE_SET {
    ULONG   Version;            /* APOLLO_RATE_VERSION */
    ULONG   Rate;               /* Hz: 44100, 48000, 88200, 96000, 176400, 192000 */
    ULONG   Flags;              /* APOLLO_RATE_SET_* */
    ULONG   Reserved;
} APOLLO_RATE_SET, *PAPOLLO_RATE_SET;

/* APOLLO_RATE_INFO.Result: why a SET_RATE did not happen (or only partly) */
#define APOLLO_RATE_ERR_NONE            0
#define APOLLO_RATE_ERR_RATE            1   /* not one of the six rates */
#define APOLLO_RATE_ERR_UNSUPPORTED     2   /* the unit does not offer it (Allowed) */
#define APOLLO_RATE_ERR_NOT_READY       3   /* the unit is not running (observe, Mode < 2, not connected, gone) */
#define APOLLO_RATE_ERR_ASIO            4   /* an ASIO host has this unit: change it there, or close the DAW's device */
#define APOLLO_RATE_ERR_COMBINED        5   /* this unit runs in the combined device: set the combined rate */
#define APOLLO_RATE_ERR_COMBINED_ASIO   6   /* the combined ASIO driver ("Apollo (Avium)") is open in a host */
#define APOLLO_RATE_ERR_BUSY            7   /* another change is under way */
#define APOLLO_RATE_ERR_LOCK            8   /* the clock did not lock at the new rate: back at the old one */
#define APOLLO_RATE_ERR_HARDWARE        9   /* the unit stopped answering, or the transport did not start again */
#define APOLLO_RATE_ERR_NO_COMBINED     10  /* COMBINED asked, but no combined device runs */
#define APOLLO_RATE_ERR_PARTIAL         11  /* combined: a later unit failed; see Why (the master changed) */

/* APOLLO_RATE_INFO.Flags */
#define APOLLO_RATE_F_LOCKED            0x0001  /* 0xC07C: Rate, clock locked */
#define APOLLO_RATE_F_CHANGING          0x0002  /* a change is under way on this unit (or the combined device) */
#define APOLLO_RATE_F_COMBINED          0x0004  /* this unit runs in the combined device (set the combined rate) */
#define APOLLO_RATE_F_ASIO_OPEN         0x0008  /* an ASIO host has this unit (SET_RATE refused unless from it) */
#define APOLLO_RATE_F_STREAMS           0x0010  /* Windows streams are open on it: they are reopened by Windows */
#define APOLLO_RATE_F_STRIPS_REBUILT    0x0020  /* the last change rebuilt the input strips (or will, once armed) */
#define APOLLO_RATE_F_PLUGINS_48K       0x0040  /* plug-in inserts run at 48 kHz only: off at this rate */
#define APOLLO_RATE_F_CORR_MISMATCH     0x0080  /* speaker correction holds a profile for another rate: stopped */
#define APOLLO_RATE_F_COMBINED_ASIO     0x0100  /* the combined ASIO client is open */
#define APOLLO_RATE_F_COMBINED_MISMATCH 0x0200  /* the combined device's units run at different rates (muted) */
#define APOLLO_RATE_F_STORED            0x0400  /* Stored was read from or written to this unit's key */

#define APOLLO_RATE_WHY_CHARS           112

typedef struct _APOLLO_RATE_INFO {
    ULONG   Version;            /* APOLLO_RATE_VERSION */
    ULONG   Bytes;              /* sizeof(APOLLO_RATE_INFO) */
    ULONG   Rate;               /* Hz the unit runs at now (its transport, its endpoints) */
    ULONG   RateIndex;          /* 1..6 */
    ULONG   RateInfo;           /* 0xC07C, last read: [15:8] index, [1:0] lock */
    ULONG   Offered;            /* bit n = rate index n the unit offers (0xC08C [7:0]) */
    ULONG   Allowed;            /* bit n = index n a SET_RATE may ask for now (Offered; 96 kHz at most on a S/PDIF or ADAT clock) */
    ULONG   ClockSource;        /* [Clock] the source it runs on now (0x0C Internal; apollo_clock.h, clock.c) */
    ULONG   Stored;             /* Hz kept for the next bring-up (Units\<key>\SampleRate), 48000 = default */
    ULONG   Flags;              /* APOLLO_RATE_F_* */
    ULONG   Result;             /* SET_RATE: APOLLO_RATE_ERR_*; GET_RATE: the last change's */
    LONG    Status;             /* SET_RATE: NTSTATUS of the step that failed (0 = none) */
    ULONG   LastMs;             /* how long the last change took */
    ULONG   Changes;            /* changes done on this unit since the driver loaded */
    ULONG   FrameCfg;           /* AX_FRAME_CFG at this rate (8 / 16 / 32) */
    ULONG   LeadFrames;         /* the engine's render lead at this rate */
    ULONG   GuardFrames;        /* the engine's capture guard at this rate */
    ULONG   AdcFrames;          /* the ADC delay at this rate */
    /* the combined device (spec-twinx 6), when this unit is one of its units */
    ULONG   CombinedRate;       /* Hz of the timeline, 0 = no combined device runs */
    ULONG   CombinedAllowed;    /* bit n = index n every unit of the combined device allows */
    ULONG   CombinedUnits;      /* units joined */
    ULONG   CombinedStored;     /* Parameters\Aggregate\SampleRate, 0 = none */
    /*
     * [Lat] (Nov 15 2026; 0 from an older driver) what the unit adds to the copy engine's numbers in the latencies it
     * reports (ASIO GetLatencies, the Windows endpoints; docs/ASIO.md, Latency). Input, without the buffer: GuardFrames
     * + AdcFrames + InPathFrames. Output: LeadFrames + FrameCfg + DacFrames.
     */
    ULONG   InPathFrames;       /* strips Live: the strip DSP's processing block (64, 128 above 100 kHz) + the plain strips'
                                   record delay (200) + the worst routed chain's record path over 200; strips not Live: 0 */
    ULONG   DacFrames;          /* the model's DAC delay at this rate (UA's Mixer Engine table, rounded): x8p 43/15/16 */
    ULONG   Reserved[2];
    CHAR    Why[APOLLO_RATE_WHY_CHARS];     /* the last change's outcome, or what limits the rate now */
} APOLLO_RATE_INFO, *PAPOLLO_RATE_INFO;
