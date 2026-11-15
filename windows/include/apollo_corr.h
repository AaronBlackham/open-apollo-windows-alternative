/*
 * apollo_corr.h - speaker correction: the IOCTLs apollo.sys (driver\corr.c), Apollo Control
 * (app\src\corr_host.h) and the tests (tools\corr-test) share.
 *
 * What it is. An "Avium correction profile" (Avium Measure, Correction mode, Export; format in
 * Avium Measure docs\correction-profile.md) holds per speaker channel a list of IIR biquads, a
 * trim and a delay, plus one preamp. The driver runs it on chosen playback slots of one unit, in
 * place, at the end of that unit's 1 ms engine tick, once every host source has been summed into
 * the playback ring: Windows endpoints (standalone or the combined "Monitor"), the per-unit ASIO
 * client and the combined "Apollo (Avium)" ASIO client all get it. The Apollo's own input
 * monitoring (inputs into the Console mix on the unit's DSP) never passes through the host and is
 * not corrected. Host playback on MON L/R enters the unit's monitor mix, so ALT speakers and
 * headphones that follow that mix hear the correction too.
 *
 * Per channel: out = filters( delay( in * Preamp * Gain ) ), 64-bit floating point, transposed
 * direct form II, at most APOLLO_CORR_MAX_FILTERS sections, delay at most APOLLO_CORR_MAX_DELAY_US.
 * Enabling, bypassing (A/B) and swapping profiles crossfade over APOLLO_CORR_FADE_MS.
 *
 * The host does the full check of the file (validate(), all six rates, coefficients against the
 * filter parameters) and sends the driver the coefficients for the unit's current rate. The driver
 * trusts none of it: it checks what it will run (driver\corr_dsp.h CorrValidate): sizes and counts,
 * every number finite, both poles of every section strictly inside the unit circle and no resonance
 * narrower than the format can describe (pole Q at most 282: a q 50 peak at +30 dB), Preamp and each
 * Gain in (0, 1] (0 dB or less), delay within its ring, slots on the unit and distinct, and the
 * headroom rule of the format (each channel's cascade times Preamp times Gain at most +0.05 dB from
 * 5 Hz to 0.999 Nyquist), measured on validate()'s grid AND at every section's own peak frequencies,
 * so no narrow peak hides between grid points; below 5 Hz and above 0.999 Nyquist at most +6 dB
 * (sub-sonic boosts). A profile that fails is refused whole and nothing changes.
 *
 * Every IOCTL here is APOLLO_USER (interactive users, like the monitor controls and the combined
 * device's configuration): a refusal completes with success and the reason in Error (a
 * METHOD_BUFFERED error status would bring nothing back). Persisted by the driver per unit:
 * Parameters\Units\<unit key>\Correction, values Profile (REG_BINARY APOLLO_CORR_PROFILE) and
 * Enabled (DWORD), read again at every start.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#ifndef APOLLO_CTL
#include "apollo_ioctl.h"
#endif

#define IOCTL_APOLLO_CORR_GET_STATE     APOLLO_CTL(0x50, APOLLO_USER)
#define IOCTL_APOLLO_CORR_LOAD          APOLLO_CTL(0x51, APOLLO_USER)
#define IOCTL_APOLLO_CORR_SET           APOLLO_CTL(0x52, APOLLO_USER)
/* 0x53-0x5F reserved for speaker correction */

#define APOLLO_CORR_VERSION             1
#define APOLLO_CORR_MAX_CHANNELS        4       /* L, R (future C, SW) */
#define APOLLO_CORR_MAX_FILTERS         20      /* the format's limit */
#define APOLLO_CORR_MAX_DELAY_US        10000   /* 10 ms: alignment of near-field speakers, never a wall echo */
#define APOLLO_CORR_FADE_MS             20      /* every enable, bypass and swap */
#define APOLLO_CORR_NAME_CHARS          64
#define APOLLO_CORR_ID_CHARS            40
#define APOLLO_CORR_CH_ID_CHARS         8
#define APOLLO_CORR_CREATED_CHARS       24
#define APOLLO_CORR_SUMMARY_CHARS       200
#define APOLLO_CORR_MAX_SLOT            63      /* playback slots 0..63 (AUDIO_MAX_CHANNELS) */

/* APOLLO_CORR_FILTER.Type: the profile's type names, display only (the driver runs Coef) */
#define APOLLO_CORR_TYPE_PEAKING        1
#define APOLLO_CORR_TYPE_LOW_SHELF      2
#define APOLLO_CORR_TYPE_HIGH_SHELF     3
#define APOLLO_CORR_TYPE_LOWPASS        4
#define APOLLO_CORR_TYPE_HIGHPASS       5
#define APOLLO_CORR_TYPE_LOWPASS_1      6
#define APOLLO_CORR_TYPE_HIGHPASS_1     7
#define APOLLO_CORR_TYPE_LOW_SHELF_1    8
#define APOLLO_CORR_TYPE_HIGH_SHELF_1   9
#define APOLLO_CORR_TYPE_NOTCH          10
#define APOLLO_CORR_TYPE_ALLPASS        11
#define APOLLO_CORR_TYPE_BANDPASS       12

#pragma pack(push, 8)

typedef struct _APOLLO_CORR_FILTER {
    double          Coef[5];        /* b0 b1 b2 a1 a2 at APOLLO_CORR_PROFILE.Rate, a0 = 1 */
    unsigned long   Type;           /* APOLLO_CORR_TYPE_* (display) */
    unsigned long   Enabled;        /* 0: listed (display) but not run */
    double          FreqHz;         /* display */
    double          GainDb;         /* display */
    double          Q;              /* display */
} APOLLO_CORR_FILTER;

typedef struct _APOLLO_CORR_CHANNEL {
    char            Id[APOLLO_CORR_CH_ID_CHARS];   /* "L", "R": the speaker's role, NUL-terminated */
    unsigned long   Slot;           /* playback slot on this unit (Twin X / x8p MON L/R = 0/1) */
    unsigned long   Output;         /* the profile's "output" (where it was measured; display) */
    double          Gain;           /* linear trim, (0, 1] */
    double          GainDb;         /* display */
    unsigned long   DelayUs;        /* 0 .. APOLLO_CORR_MAX_DELAY_US, rounded to a frame at Rate */
    unsigned long   FilterCount;    /* 0 .. APOLLO_CORR_MAX_FILTERS */
    APOLLO_CORR_FILTER Filter[APOLLO_CORR_MAX_FILTERS];
} APOLLO_CORR_CHANNEL;

/* IOCTL_APOLLO_CORR_LOAD input; the value the driver stores; GET_STATE returns the one in force. */
typedef struct _APOLLO_CORR_PROFILE {
    unsigned long   Size;           /* sizeof(APOLLO_CORR_PROFILE) */
    unsigned long   Version;        /* APOLLO_CORR_VERSION */
    unsigned long   Rate;           /* Hz the coefficients are for: must be the unit's rate to run */
    unsigned long   ChannelCount;   /* 1 .. APOLLO_CORR_MAX_CHANNELS */
    unsigned long   Flags;          /* APOLLO_CORR_LOAD_* */
    unsigned long   Reserved;
    double          Preamp;         /* linear, (0, 1] */
    double          PreampDb;       /* display */
    char            Name[APOLLO_CORR_NAME_CHARS];  /* the profile's name, UTF-8, NUL-terminated */
    char            Id[APOLLO_CORR_ID_CHARS];      /* the profile's id (32 hex), NUL-terminated */
    char            Created[APOLLO_CORR_CREATED_CHARS];    /* the profile's "created" (UTC, display) */
    char            Summary[APOLLO_CORR_SUMMARY_CHARS];    /* how it was measured and its target, one line (display) */
    APOLLO_CORR_CHANNEL Channel[APOLLO_CORR_MAX_CHANNELS];
} APOLLO_CORR_PROFILE;

#define APOLLO_CORR_LOAD_ENABLE         0x00000001  /* LOAD: enable it too (else the enable state stays) */
#define APOLLO_CORR_LOAD_NO_STORE       0x00000002  /* LOAD: do not persist (tests) */

/* IOCTL_APOLLO_CORR_SET input */
typedef struct _APOLLO_CORR_SET {
    unsigned long   Op;             /* APOLLO_CORR_OP_* */
    unsigned long   Value;          /* ENABLE / BYPASS: 0 or 1 */
} APOLLO_CORR_SET;

#define APOLLO_CORR_OP_ENABLE           1   /* persisted: off fades out and stops all processing */
#define APOLLO_CORR_OP_BYPASS           2   /* A/B, not persisted: fades to the uncorrected signal, filters keep running */
#define APOLLO_CORR_OP_CLEAR            3   /* fade out, forget the profile, delete the stored one */

/* LOAD and SET output: Error 0 = done */
typedef struct _APOLLO_CORR_RESULT {
    unsigned long   Error;          /* APOLLO_CORR_ERR_* */
    unsigned long   Channel;        /* where (when it applies), 0-based */
    unsigned long   Filter;
    unsigned long   Stored;         /* LOAD/SET: 1 = written to the registry */
    long            StoreStatus;    /* NTSTATUS of that write */
    unsigned long   Reserved;
} APOLLO_CORR_RESULT;

#define APOLLO_CORR_ERR_NONE            0
#define APOLLO_CORR_ERR_SIZE            1   /* Size / buffer length / Version */
#define APOLLO_CORR_ERR_RATE            2   /* not 44.1/48/88.2/96/176.4/192 kHz */
#define APOLLO_CORR_ERR_CHANNELS        3   /* ChannelCount */
#define APOLLO_CORR_ERR_FILTERS         4   /* FilterCount */
#define APOLLO_CORR_ERR_NOT_FINITE      5   /* a coefficient, gain or display number is not finite */
#define APOLLO_CORR_ERR_UNSTABLE        6   /* a pole on or outside the unit circle */
#define APOLLO_CORR_ERR_GAIN            7   /* Preamp or a Gain outside (0.001, 1] (-60..0 dB) */
#define APOLLO_CORR_ERR_DELAY           8   /* DelayUs too long */
#define APOLLO_CORR_ERR_SLOT            9   /* slot beyond the unit's playback slots, or used twice */
#define APOLLO_CORR_ERR_CLIP            10  /* a channel could clip after the preamp */
#define APOLLO_CORR_ERR_RATE_MISMATCH   11  /* coefficients for another rate than the unit runs */
#define APOLLO_CORR_ERR_OP              12  /* SET: unknown op */
#define APOLLO_CORR_ERR_NO_PROFILE      13  /* SET enable without a profile */
#define APOLLO_CORR_ERR_UNIT            14  /* this unit does not run audio (observe, or no engine) */
#define APOLLO_CORR_ERR_BUSY            15  /* a load is still waiting for the previous fade: try again */
#define APOLLO_CORR_ERR_TEXT            16  /* Name / Id / Created / Summary / channel Id not NUL-terminated */
#define APOLLO_CORR_ERR_DC              17  /* more than 6 dB of gain below 5 Hz or above 0.999 Nyquist */
#define APOLLO_CORR_ERR_NARROW          18  /* a resonance narrower than the format can describe (pole Q over 282) */

/* IOCTL_APOLLO_CORR_GET_STATE input (optional): */
#define APOLLO_CORR_STATE_RESET_COST    0x1     /* zero MaxNs after reading it */

typedef struct _APOLLO_CORR_STATE {
    unsigned long   Version;        /* APOLLO_CORR_VERSION */
    unsigned long   Flags;          /* APOLLO_CORR_F_* */
    unsigned long   UnitRate;       /* the unit's sample rate now (0 = transport never ran) */
    unsigned long   WetPermille;    /* 0 = uncorrected .. 1000 = corrected, the fade position */
    unsigned long   SwapPermille;   /* a profile swap in progress: 0..1000; 1000 = none */
    unsigned long   Runs;           /* ticks that processed frames */
    unsigned long long Frames;      /* frames processed since the start */
    unsigned long long LateFrames;  /* frames the hardware fetched before they were corrected */
    unsigned long long Head;        /* next frame to correct (hardware frame numbers) */
    unsigned long   LastNs;         /* cost of the last tick that processed, nanoseconds */
    unsigned long   MaxNs;          /* the largest since the start or the last reset */
    long            StoreStatus;    /* NTSTATUS of the last registry write (0 = none or ok) */
    unsigned long   LoadedFrom;     /* APOLLO_CORR_FROM_* */
    unsigned long   Reserved[4];
    APOLLO_CORR_PROFILE Profile;    /* the one in force (Size 0 = none) */
} APOLLO_CORR_STATE;

#define APOLLO_CORR_F_LOADED            0x0001  /* a profile is in force */
#define APOLLO_CORR_F_ENABLED           0x0002  /* enabled (persisted) */
#define APOLLO_CORR_F_BYPASS            0x0004  /* A/B: listening to the uncorrected signal */
#define APOLLO_CORR_F_RUNNING           0x0008  /* the filters run (enabled; wet, fading or warm for A/B) */
#define APOLLO_CORR_F_RATE_MISMATCH     0x0010  /* the profile's rate is not the unit's: nothing runs */
#define APOLLO_CORR_F_STORED            0x0020  /* the registry holds this profile */
#define APOLLO_CORR_F_SLOT_RANGE        0x0040  /* a slot is beyond the unit's playback slots: that channel is skipped */
#define APOLLO_CORR_F_UNIT_IDLE         0x0080  /* the unit runs no engine (observe or not started) */

#define APOLLO_CORR_FROM_NONE           0
#define APOLLO_CORR_FROM_REGISTRY       1   /* loaded at the start */
#define APOLLO_CORR_FROM_IOCTL          2

#pragma pack(pop)
