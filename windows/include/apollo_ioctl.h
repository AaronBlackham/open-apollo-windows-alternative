/*
 * apollo_ioctl.h - control interface shared by apollo.sys, apollo-diag and
 * the Apollo Control app.
 *
 * Access (v4). The device grants SYSTEM and Administrators full access,
 * LocalService read/write (the Windows audio service and audiodg open the
 * ACX endpoints on this device) and interactive users read access (SDDL in
 * apollo.inf). The I/O manager checks each IOCTL's access bits against the
 * handle, and the driver additionally requires an elevated administrator
 * token, captured when the handle was opened, for every IOCTL that carries
 * FILE_WRITE_ACCESS:
 *   APOLLO_USER  (FILE_READ_ACCESS)  status, position, levels, readback, log,
 *                                    preamp and monitor parameters, engine
 *                                    stats, ASIO streaming (apollo_asio.h)
 *   APOLLO_ADMIN (+ FILE_WRITE_ACCESS) raw registers, raw mixer settings,
 *                                    transport start/stop, audio ring I/O
 * Open with GENERIC_READ for the control set; GENERIC_READ | GENERIC_WRITE
 * (elevated) for everything. Every IOCTL is METHOD_BUFFERED.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 * Hardware behaviour ported from open-apollo (GPL-2.0); see docs/spec-*.md.
 */
#pragma once

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <windows.h>
#include <winioctl.h>
#endif
#include <initguid.h>

/* {4004c57a-2df0-4638-90d1-cd87d4e8ee6a} */
DEFINE_GUID(GUID_DEVINTERFACE_APOLLO,
    0x4004c57a, 0x2df0, 0x4638, 0x90, 0xd1, 0xcd, 0x87, 0xd4, 0xe8, 0xee, 0x6a);

#define APOLLO_IOCTL_VERSION        8   /* 8: UAD plug-in hosting P0, block 0x40-0x43 (apollo_plugin.h,
                                           docs/PLUGINS.md 2.1); every layout below unchanged.
                                           3: GET_STRIPS; 4: GET_ENGINE_STATS, ASIO (0x20-0x23);
                                           5: GET_ENDPOINTS (ACX endpoints);
                                           6: GET_INFO unit fields (HW_UID, model, Mode, log tag,
                                              what the previous owner left), log lines tagged per
                                              unit, 256 KiB log (docs/spec-twinx.md 6.9, gate G2);
                                           7: the Twin X Gen 1 bring-up (gate G3, spec-twinx 6.9.3):
                                              GET_INFO ClockSrcsLive, BringupSteps, BringupFailStep
                                              in the former Reserved6 (same size); a Mode 1 Twin X
                                              answers reads, capture and playback only.
                                              Still 7 at G4 (same layout): GET_INFO BringupApplies
                                              in the former Reserved7; a Mode 2 Twin X also takes
                                              SET_MONITOR_PARAM (level, HP1 level, mute, mono, dim).
                                              Still 7 at G5 (same layout): a Mode 2 Twin X also takes
                                              SET_INPUT_PARAM on preamps 1-2 (+ APOLLO_PARAM_HIZ_IMP on
                                              input 1) and SET_MONITOR_PARAM talkback; GET_INFO
                                              NumPreamps 2; GET_STRIPS reports its input strips */
/* [Clock] still 8 (every layout here unchanged; GET_ENDPOINTS rows may carry the new APOLLO_EP_FLAG_SILENT):
   clock source, digital formats, front panel and monitor link are apollo_clock.h, codes 0x53-0x56, own version */
/* [Virtual] still 8: GET_STRIPS grows at its end (its own Version 4, the Virtual Input Strips; a shorter buffer gets
   version 3 or 2 as before), and SET_MONITOR_PARAM's mix, cue and aux codes take the buses 0x18-0x1F (VIRTUAL 1-8)
   on a unit whose StripVirtualMask builds their strips */
/* [AuxStrips] still 8: GET_STRIPS grows at its end again (Version 5, the aux returns' Input Strips; a buffer of the
   version 4 size gets version 4, then 3 or 2 as before); every SET_MONITOR_PARAM code unchanged */

#define FILE_DEVICE_APOLLO          0x8A70u     /* unsigned: << 16 overflows int */
#define APOLLO_CTL(fn, access) \
    CTL_CODE(FILE_DEVICE_APOLLO, 0x800 + (fn), METHOD_BUFFERED, (access))
#define APOLLO_USER                 FILE_READ_ACCESS
#define APOLLO_ADMIN                (FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#define IOCTL_APOLLO_GET_INFO           APOLLO_CTL(0x00, APOLLO_USER)
#define IOCTL_APOLLO_READ_REG           APOLLO_CTL(0x01, APOLLO_ADMIN)
#define IOCTL_APOLLO_WRITE_REG          APOLLO_CTL(0x02, APOLLO_ADMIN)
#define IOCTL_APOLLO_TRANSPORT_START    APOLLO_CTL(0x03, APOLLO_ADMIN)
#define IOCTL_APOLLO_TRANSPORT_STOP     APOLLO_CTL(0x04, APOLLO_ADMIN)
#define IOCTL_APOLLO_GET_POSITION       APOLLO_CTL(0x05, APOLLO_USER)
#define IOCTL_APOLLO_READ_CAPTURE       APOLLO_CTL(0x06, APOLLO_ADMIN)
#define IOCTL_APOLLO_WRITE_PLAYBACK     APOLLO_CTL(0x07, APOLLO_ADMIN)
#define IOCTL_APOLLO_SET_INPUT_PARAM    APOLLO_CTL(0x08, APOLLO_USER)
#define IOCTL_APOLLO_GET_READBACK       APOLLO_CTL(0x09, APOLLO_USER)
#define IOCTL_APOLLO_SET_MIXER_SETTING  APOLLO_CTL(0x0A, APOLLO_ADMIN)
#define IOCTL_APOLLO_GET_LOG            APOLLO_CTL(0x0B, APOLLO_USER)
#define IOCTL_APOLLO_SET_MONITOR_PARAM  APOLLO_CTL(0x0C, APOLLO_USER)
#define IOCTL_APOLLO_GET_LEVELS         APOLLO_CTL(0x0D, APOLLO_USER)
#define IOCTL_APOLLO_GET_STRIPS         APOLLO_CTL(0x0E, APOLLO_USER)
#define IOCTL_APOLLO_GET_ENGINE_STATS   APOLLO_CTL(0x0F, APOLLO_USER)
#define IOCTL_APOLLO_GET_ENDPOINTS      APOLLO_CTL(0x10, APOLLO_USER)
/* 0x20-0x2F: ASIO, see apollo_asio.h */

/*
 * GET_LOG: output APOLLO_LOG_HEADER followed by Length bytes of text, the
 * most recent part of the driver's in-memory log (oldest first, lines end
 * in '\n'). TotalWritten counts every byte ever logged, so a caller can
 * tell whether older text was overwritten.
 *
 * Version 6: one log for every Apollo on the driver. A line about one unit
 * carries that unit's tag after the time, "[  12.345] [twx] text" (tag =
 * APOLLO_INFO.LogTag: "x8p", "twx", or "s" + the PCI subsystem in hex); a
 * line without a tag is about the driver as a whole. The ring grew from
 * 64 KiB to 256 KiB; a smaller output buffer gets the newest part.
 */
#define APOLLO_LOG_BYTES    262144

typedef struct _APOLLO_LOG_HEADER {
    ULONG   TotalWritten;
    ULONG   Length;
} APOLLO_LOG_HEADER, *PAPOLLO_LOG_HEADER;

/* Bring-up stage reached (APOLLO_INFO.Stage). Ordered: higher = further. */
typedef enum _APOLLO_STAGE {
    ApolloStageNone = 0,
    ApolloStageMapped,          /* BAR0 mapped, quiesced, identified */
    ApolloStageRingsReady,      /* DSP rings + SG tables programmed */
    ApolloStageRingConnected,   /* DSP ring connect sent (x8p cold) */
    ApolloStageAcefaceDone,     /* audio extension handshake answered */
    ApolloStageConnected,       /* post-handshake reads, rate ack, mixer init */
    ApolloStageFailed = 0x100,  /* see LastStatus / FailedStage */
    ApolloStageGone   = 0x200,  /* device removed (all-ones reads) */
} APOLLO_STAGE;

/* APOLLO_INFO.Flags */
#define APOLLO_FLAG_FW_V2           0x00000001
#define APOLLO_FLAG_IRQ64           0x00000002
#define APOLLO_FLAG_WARM_START      0x00000004  /* mixer DSP was alive at start */
#define APOLLO_FLAG_RINGS_LIVE      0x00000008
#define APOLLO_FLAG_DSPS_CONNECTED  0x00000010
#define APOLLO_FLAG_ACEFACE_DONE    0x00000020
#define APOLLO_FLAG_CONNECTED       0x00000040
#define APOLLO_FLAG_MIXER_READY     0x00000080
#define APOLLO_FLAG_TRANSPORT       0x00000100
#define APOLLO_FLAG_SERVICE_RUNNING 0x00000200
#define APOLLO_FLAG_READBACK_VALID  0x00000400
#define APOLLO_FLAG_MSI             0x00000800
#define APOLLO_FLAG_FW_UNTESTED     0x00001000  /* firmware/FPGA differ from the tested unit */
#define APOLLO_FLAG_ENGINE_LOST     0x00002000  /* the 1 ms engine gave up on a device that stopped
                                                   answering; restart the transport or replug */

/* The firmware this driver was tested on (Aaron's x8p, Nov 14 2026). UA firmware updates can
   change registers underneath the driver: keep the unit on these. */
#define APOLLO_TESTED_FW_RELEASE    0x0B0FD613u /* 11.15.54803 */
#define APOLLO_TESTED_FPGA_REV      0xA241C5ACu

typedef struct _APOLLO_INFO {
    ULONG   Version;            /* APOLLO_IOCTL_VERSION */
    ULONG   Stage;              /* APOLLO_STAGE */
    ULONG   FailedStage;        /* stage being attempted when it failed */
    LONG    LastStatus;         /* NTSTATUS of the failure, or 0 */
    ULONG   Flags;
    ULONG   Bar0Length;
    ULONG   FpgaRev;            /* 0x2218 */
    ULONG   ExtCaps;            /* 0x2234 */
    ULONG   FwRelease;          /* 0x0008 */
    ULONG   SubsystemId;        /* PCI config 0x2E */
    ULONG   DeviceType;         /* internal type, 0x22 = x8p */
    ULONG   PublicType;         /* kext UAD2DeviceType */
    ULONG   NumDsps;
    ULONG   PlayChannels;       /* AX_PLAY_CH slots per frame */
    ULONG   RecChannels;        /* AX_REC_CH slots per frame */
    ULONG   RingFrames;         /* 8192 */
    ULONG   SampleRate;
    ULONG   DmaCtrl;            /* last read of 0x2200 */
    ULONG   NotifRateInfo;      /* last read of 0xC07C */
    ULONG   ServicePasses;
    ULONG   InterruptCount;
    ULONG   EndOfBufferCount;
    ULONG   NotifStatusSeen;    /* OR of every 0xC030 value the service loop saw */
    ULONG   NumPreamps;
    CHAR    Serial[17];         /* BAR0 0x20-0x2F, NUL terminated */
    CHAR    Reserved[3];

    /*
     * Version 6 (docs/spec-twinx.md 6.9): which unit this handle is, for clients that must pick
     * one when two Apollos run on the driver. A driver before version 6 returns only the fields
     * above (APOLLO_INFO_V5_BYTES): read these only when GET_INFO returned sizeof(APOLLO_INFO)
     * bytes and Version >= 6. The driver accepts any output buffer of at least
     * APOLLO_INFO_V5_BYTES and fills as much as fits, so older clients keep working.
     */
    ULONG   HwUidLo;            /* BAR0 0x30 */
    ULONG   HwUidHi;            /* BAR0 0x34 */
    ULONG   Model;              /* APOLLO_INFO_MODEL_*: the model the driver accepted, 0 = none */
    ULONG   Mode;               /* unit Mode in effect (0 observe, 1 transport, 2 full), or
                                   APOLLO_INFO_MODE_UNKNOWN when the unit was not accepted */
    ULONG   UnitFlags;          /* APOLLO_UNIT_FLAG_* */
    ULONG   AggRole;            /* APOLLO_AGG_ROLE_*: always standalone in this build */
    ULONG   AggId;              /* 0 in this build */
    ULONG   HwidSubsystem;      /* SUBSYS_xxxx of the PCI hardware ID, read at device add, 0 = unread */
    CHAR    ModelName[24];      /* e.g. "Apollo x8p", "Apollo Twin X (Gen 1)", NUL terminated */
    CHAR    UnitKey[20];        /* Parameters\Units\<UnitKey>, "" = no unit key, NUL terminated */
    CHAR    LogTag[8];          /* the tag of this unit's GET_LOG lines, NUL terminated */
    /*
     * The notification block as the previous owner (UA's driver) left it, read once at start-up
     * before this driver wrote anything to it. Only for units the driver does not bring up
     * (observe and refused): UnitFlags OWNER_READ. Stale by definition: nothing of ours runs
     * on that unit, so these are UA's last values, not the unit's current state.
     */
    ULONG   OwnerRateInfo;      /* 0xC07C: [15:8] rate index, [1:0] lock */
    ULONG   OwnerClockSrcs;     /* 0xC08C: [31:16] clock sources, [15:0] rates */
    ULONG   OwnerDescInVersion; /* IO descriptor in (0xC1A4) word 0 */
    ULONG   OwnerDescInCount;   /* word 3: record channels */
    ULONG   OwnerDescOutVersion;/* IO descriptor out (0xC2C4) word 0 */
    ULONG   OwnerDescOutCount;  /* word 4: playback channels */
    /*
     * Version 7 (gate G3, docs/spec-twinx.md 6.9.3): the Twin X Gen 1 (AudioExtension) bring-up.
     * All zero on the x8p, which records no steps, and on observe units. A version 6 driver
     * returns zeros here (it was Reserved6).
     */
    ULONG   ClockSrcsLive;      /* 0xC08C read with our transport running: [31:16] sources, [7:0] rates */
    ULONG   BringupSteps;       /* bit n = step n passed at the last bring-up (driver\bringup_plan.h
                                   APOLLO_BSTEP_*: E1 ... copy engine) */
    ULONG   BringupFailStep;    /* the APOLLO_BSTEP_* that stopped the unit, 0 = none */
    ULONG   BringupApplies;     /* [G4] (the former Reserved7) bit n = step n is part of this start's
                                   path (cold/warm, boot transport or not, E10 only when E8 failed);
                                   a step outside it is "n/a", not "not passed". 0 from a G3 driver */
} APOLLO_INFO, *PAPOLLO_INFO;

/* GET_INFO before version 6 returned this many bytes (the fields up to Reserved[3]). */
#define APOLLO_INFO_V5_BYTES        ((ULONG)FIELD_OFFSET(APOLLO_INFO, HwUidLo))

/* APOLLO_INFO.Model */
#define APOLLO_INFO_MODEL_NONE      0   /* not accepted (refused, not identified) */
#define APOLLO_INFO_MODEL_X8P       1   /* = apollo_model.h APOLLO_MODEL_X8P */
#define APOLLO_INFO_MODEL_TWINX     2   /* = APOLLO_MODEL_TWINX: Twin X Gen 1 */

#define APOLLO_INFO_MODE_UNKNOWN    0xFFFFFFFFu

/* APOLLO_INFO.UnitFlags */
#define APOLLO_UNIT_FLAG_RESOLVED   0x01    /* accepted and its Parameters read: Mode is valid */
#define APOLLO_UNIT_FLAG_ACTIVE     0x02    /* the driver runs this unit (bring-up, transport,
                                               mixer, endpoints as its Mode allows) */
#define APOLLO_UNIT_FLAG_OBSERVE    0x04    /* the driver does NOT run it: identified and quiesced
                                               only (Mode 0, a model without a bring-up path in
                                               this build, or refused). Read-only: settings,
                                               transport, ASIO and audio I/O are refused. */
#define APOLLO_UNIT_FLAG_REFUSED    0x08    /* a model this driver does not run (see the log) */
#define APOLLO_UNIT_FLAG_OWNER_READ 0x10    /* Owner* hold what the previous owner left (stale) */
#define APOLLO_UNIT_FLAG_UNIT_KEY   0x20    /* Parameters\Units\<UnitKey> exists */
#define APOLLO_UNIT_FLAG_MODEL_KEY  0x40    /* Parameters\Models\<DeviceType hex> exists */

/* APOLLO_INFO.AggRole */
#define APOLLO_AGG_ROLE_STANDALONE  0

/* Device types clients choose by (apollo_model.h holds the full table). */
#define APOLLO_TYPE_X8P_ID          0x22u
#define APOLLO_TYPE_TWINX_ID        0x99u

typedef struct _APOLLO_REG_IO {
    ULONG   Offset;             /* BAR0 byte offset, 4-byte aligned */
    ULONG   Value;
} APOLLO_REG_IO, *PAPOLLO_REG_IO;

#define APOLLO_POS_FLAG_DEAD_READ   0x1     /* the last SAMPLE_POS read returned all ones */
#define APOLLO_POS_FLAG_ENGINE_LOST 0x2     /* the engine gave up on the device: Position and
                                               QpcTimestamp are frozen, registers not read */

typedef struct _APOLLO_POSITION {
    ULONG     RawSamplePos;     /* AX_SAMPLE_POS as read */
    ULONG     FrameCtr;         /* AX_FRAME_CTR */
    ULONG     AxControl;        /* AX_CONTROL */
    ULONG     Flags;            /* APOLLO_POS_FLAG_* */
    ULONGLONG Position;         /* 64-bit extended frame position */
    LONGLONG  QpcTimestamp;     /* KeQueryPerformanceCounter at the read */
    LONGLONG  QpcFrequency;
} APOLLO_POSITION, *PAPOLLO_POSITION;

/*
 * READ_CAPTURE: input APOLLO_CAPTURE_REQUEST; output APOLLO_CAPTURE_HEADER
 * followed by Frames * Channels LONGs (S32_LE, 24 valid bits MSB-aligned),
 * interleaved exactly as the hardware ring holds them.
 * StartPosition must lie within the last (RingFrames - margin) frames.
 */
typedef struct _APOLLO_CAPTURE_REQUEST {
    ULONGLONG StartPosition;
    ULONG     MaxFrames;
    ULONG     Reserved;
} APOLLO_CAPTURE_REQUEST, *PAPOLLO_CAPTURE_REQUEST;

#define APOLLO_CAPTURE_OVERRUN      0x1     /* StartPosition already overwritten */

typedef struct _APOLLO_CAPTURE_HEADER {
    ULONGLONG StartPosition;    /* first frame returned */
    ULONGLONG HwPosition;       /* hardware position at the copy */
    ULONG     Frames;
    ULONG     Channels;
    ULONG     Flags;
    ULONG     Reserved;
} APOLLO_CAPTURE_HEADER, *PAPOLLO_CAPTURE_HEADER;

/*
 * WRITE_PLAYBACK: input APOLLO_PLAYBACK_HEADER followed by
 * Frames * Channels LONGs, output APOLLO_PLAYBACK_RESULT.
 * Channels must equal APOLLO_INFO.PlayChannels; Frames at most 3072.
 * Frames land at StartPosition, which must be at least RenderLeadFrames + 48
 * frames ahead of the hardware (192 by default: past the frames the engine's
 * clients already wrote, plus one engine tick) and end less than 4032 frames
 * ahead (inside the engine's zero horizon). Outside that window the result is
 * LATE or TOO_EARLY. Refused (STATUS_DEVICE_NOT_READY) while ENGINE_LOST.
 */
typedef struct _APOLLO_PLAYBACK_HEADER {
    ULONGLONG StartPosition;
    ULONG     Frames;
    ULONG     Channels;
} APOLLO_PLAYBACK_HEADER, *PAPOLLO_PLAYBACK_HEADER;

/*
 * WRITE_PLAYBACK output. Timing outcomes are results, not errors: the
 * IOCTL fails only for malformed requests or a device that is not running.
 */
#define APOLLO_PLAYBACK_WRITTEN     0
#define APOLLO_PLAYBACK_LATE        1   /* too close to the hardware: resync */
#define APOLLO_PLAYBACK_TOO_EARLY   2   /* beyond the zero horizon: wait */

typedef struct _APOLLO_PLAYBACK_RESULT {
    ULONG     Result;
    ULONG     Reserved;
    ULONGLONG HwPosition;
} APOLLO_PLAYBACK_RESULT, *PAPOLLO_PLAYBACK_RESULT;

/*
 * SET_INPUT_PARAM: preamp parameters through the mixer mailbox
 * (spec-dsp-mixer 5.2). Switches take 0/1. Settings S0 (preamps 1-4) and
 * S12 (preamps 5-8), 6 flag bits per preamp. Per model (driver\preamp_plan.h):
 * the x8p preamps 0-7; the Twin X Gen 1 preamps 0-1, Link on 0 only, and the
 * Hi-Z impedance of preamp 0. A preamp the model lacks: STATUS_INVALID_PARAMETER;
 * a control it lacks: STATUS_NOT_SUPPORTED.
 */
#define APOLLO_PARAM_MIC_LINE   0x00    /* 1 = Line, 0 = Mic */
#define APOLLO_PARAM_PAD        0x01
#define APOLLO_PARAM_LINK       0x02    /* even preamp (0-based) only: links it with the next */
#define APOLLO_PARAM_48V        0x03
#define APOLLO_PARAM_LOWCUT     0x04
#define APOLLO_PARAM_PHASE      0x05    /* 1 = polarity inverted */
#define APOLLO_PARAM_HIZ_IMP    0x11    /* Twin X preamp 0 (the Hi-Z jack): 0-7 = 2.2M 1M 453k
                                           232k 100k 70k 47k 21k; S13 [3:0]; no readback field */
#define APOLLO_PARAM_GAIN_DB    0x100   /* driver-side: writes GainA, GainB, GainC */

typedef struct _APOLLO_INPUT_PARAM {
    ULONG   Preamp;             /* 0-based */
    ULONG   Param;              /* APOLLO_PARAM_* */
    LONG    Value;
} APOLLO_INPUT_PARAM, *PAPOLLO_INPUT_PARAM;

typedef struct _APOLLO_MIXER_SETTING {
    ULONG   Index;              /* 0..37 */
    ULONG   Value;
    ULONG   Mask;
} APOLLO_MIXER_SETTING, *PAPOLLO_MIXER_SETTING;

/*
 * SET_MONITOR_PARAM: monitor section through the mixer mailbox
 * (spec-dsp-mixer 6.1 C9-C12; open-apollo ua_monitor_set_param). IDs follow
 * the kext's monitor property IDs except APOLLO_MON_MONO, which the kext sends
 * inside 0x03. Levels are raw: 192 + 2 * dB, so 192 = 0 dB, 0 = -96 dB.
 *
 * Not offered: headphone 2 level (kext 0x66). It lands in S14 bits 7:0,
 * which on the x8p hold preamp 5-6 GainC; where the x8p keeps it is unknown.
 */
#define APOLLO_MON_LEVEL        0x01    /* raw 0..192            S2 [7:0]           */
#define APOLLO_MON_HP1_LEVEL    0x02    /* raw 0..192            S2 [15:8]          */
#define APOLLO_MON_MUTE         0x03    /* 0/1                   S2 bit 16          */
#define APOLLO_MON_SOURCE       0x04    /* 0 MIX, 1-4 CUE 1-4    S2 [19:18], bit 29 */
#define APOLLO_MON_MONO         0x10    /* 0/1                   S2 bit 17          */
#define APOLLO_MON_HP1_SOURCE   0x3F    /* 0 CUE 1, 1 CUE 2      S12 [26:24]        */
#define APOLLO_MON_HP2_SOURCE   0x40    /* 0 CUE 1, 1 CUE 2      S12 [29:27]        */
#define APOLLO_MON_DIM_LEVEL    0x43    /* 1..7: -9 -17 -26 -34 -43 -51 -60 dB, S14 [30:28] */
#define APOLLO_MON_DIM          0x44    /* 0/1                   S2 bit 31          */
#define APOLLO_MON_TALKBACK     0x46    /* 0/1                   S13 bit 8          */
#define APOLLO_MON_LEVEL_MAX    192
/*
 * [Monitor] Same IOCTL and layout (still version 8); per model in driver\monitor_plan.h, which also cites
 * the evidence. A code, cue, ALT or headphone the model lacks: STATUS_NOT_SUPPORTED; a value out of range
 * or a bus the model does not mix: STATUS_INVALID_PARAMETER. APOLLO_MON_SOURCE takes 3 = CUE 3 and 4 = CUE 4
 * on the x8p (bit 29); HP1/HP2 source take CUE 1 / CUE 2 on both units (the Twin X has HP 1 only).
 *   x8p: CUE 1-4, ALT 1 (Line 1-2) and ALT 2 (Line 3-4), HP 1-2, sends from ANALOG 1-8 and TALKBACK.
 *   Twin X: CUE 1 "HP" and CUE 2 "LINE 3/4", ALT 1 (Line 3/4), HP 1, sends from ANALOG 1-2 and TALKBACK.
 *   [Mix] Both: the monitor mix of the same buses (APOLLO_MON_MIX_*, below).
 */
#define APOLLO_MON_CUE1_MIX     0x05    /* 0 own mix, 1 monitor mix  S2 [25:24] = 0/2 */
#define APOLLO_MON_CUE1_MONO    0x06    /* 0/1                   S2 [21:20], mono = bit 21 */
#define APOLLO_MON_CUE2_MIX     0x07    /* as CUE 1              S2 [27:26] */
#define APOLLO_MON_CUE2_MONO    0x08    /*                       S2 [23:22], mono = bit 23 */
#define APOLLO_MON_CUE3_MIX     0x22    /* x8p                   S8 [29:28] */
#define APOLLO_MON_CUE3_MONO    0x23    /* x8p                   S8 [25:24], mono = bit 25 */
#define APOLLO_MON_CUE4_MIX     0x24    /* x8p                   S8 [31:30] */
#define APOLLO_MON_CUE4_MONO    0x25    /* x8p                   S8 [27:26], mono = bit 27 */
#define APOLLO_MON_ALT1_TRIM    0x38    /* raw 0..120 = 60 + 2*dB  S15 [6:0]    */
#define APOLLO_MON_ALT2_TRIM    0x39    /* x8p                   S15 [22:16]        */
#define APOLLO_MON_ALT_SELECT   0x3A    /* 0 MAIN, 1 ALT 1, 2 ALT 2  S13 [29:28]; readback word 6 [29:28] */
#define APOLLO_MON_ALT1_ENABLE  0x3B    /* 0/1                   S15 bit 8          */
#define APOLLO_MON_ALT2_ENABLE  0x3C    /* x8p                   S15 bit 12         */
/*
 * [Out] Output and front-panel settings (driver\monitor_plan.h has the evidence). Nothing reads back: the host keeps
 * what it set and sends it again when the unit connects.
 */
#define APOLLO_MON_AUX1_MONO    0x0A    /* 0/1                   S6 [9:8], mono = bit 9 */
#define APOLLO_MON_AUX2_MONO    0x0F    /* 0/1                   S7 [9:8], mono = bit 9 */
#define APOLLO_MON_CLIP_HOLD    0x13    /* 0 none, 1/3/5/10 s = 1-4, 5 infinite   S1 [30:28] */
#define APOLLO_MON_PEAK_HOLD    0x14    /* 0 none, 1/3/5/10 s = 1-4               S1 [15:13] */
#define APOLLO_MON_LINE_PAD(n)  (0x19 + (n))    /* x8p line pair n 0-3: 0 +4 dBu, 1 -10 dBV  S0 bit 28+n */
#define APOLLO_MON_IDENTIFY     0x1D    /* 0 normal, 1-7 identify states          S6 [13:11] */
#define APOLLO_MON_DIG_MIRROR   0x1E    /* x8p: monitor mirrored to S/PDIF 0/1    S6 bit 10 */
#define APOLLO_MON_MON_REF      0x32    /* monitor outputs: 0 +4 dBu, 1 -10 dBV   S11 bit 4 */
#define APOLLO_MON_FUNC_SWITCH  0x42    /* x8p: 0 none, 2 mono, 3 dim, 4 talkback S13 [15:13] */
/* Cue sends (driver-side codes: DSP ring bus coefficients, cached and re-sent by the driver).
   c = cue 0-3, b = mixer bus: ANALOG n = n-1, TALKBACK = 0x0A. */
#define APOLLO_MON_CUE_SEND(c, b)   (0x1000u | ((c) << 8) | (b))    /* half-dB -120..24 (-60..+12 dB), -121 = off */
#define APOLLO_MON_CUE_PAN(c, b)    (0x2000u | ((c) << 8) | (b))    /* -100 left .. 100 right */
#define APOLLO_MON_CUE_MUTE(c)      (0x3000u | ((c) << 8))          /* 0/1: the cue's sends go out as 0.0 */
#define APOLLO_MON_SEND_OFF     (-121)
#define APOLLO_MON_BUS_TALKBACK 0x0A
/* [Virtual] VIRTUAL n (1-8) = bus 0x18 + n - 1, for the mix, cue and aux codes, on a unit whose registry StripVirtualMask
   has bit n-1 (the driver builds its Input Strip; GET_STRIPS version 4 VirtualMask); refused (invalid parameter) elsewhere */
#define APOLLO_MON_BUS_VIRTUAL(n)   (0x18u + (ULONG)(n) - 1u)
/*
 * [Mix] The monitor mix (what the monitor outputs play): per bus, param 0 of the same DSP bus coefficients,
 * cached and re-sent by the driver like the sends. Until the host sets a bus the unit keeps its own default
 * (UA: on at 0 dB); the driver sends nothing for it. Solo is the driver's (host logic): while any bus is
 * soloed every other bus goes out as 0.0, the mutes kept apart. The talkback (0x0A) has no pan. A linked
 * pair is the host's: the same level/mute/solo to both buses, a pan each (UA: -100 / +100 on link).
 */
#define APOLLO_MON_MIX_LEVEL(b)     (0x4000u | (b))     /* half-dB -120..24 (-60..+12 dB), -121 = off (-inf) */
#define APOLLO_MON_MIX_PAN(b)       (0x4100u | (b))     /* -100 left .. 100 right */
#define APOLLO_MON_MIX_MUTE(b)      (0x4200u | (b))     /* 0/1 */
#define APOLLO_MON_MIX_SOLO(b)      (0x4300u | (b))     /* 0/1 */
#define APOLLO_MON_MIX_MUTE_ALL     0x4400u             /* 1 mutes, 0 unmutes, every bus the model mixes */
/*
 * [Aux] The two stereo aux buses, both models (driver\monitor_plan.h has the encoding and the evidence). a = 0 (AUX 1)
 * or 1 (AUX 2); b = the input's bus as for the cue sends; c = the cue (0-3, the model's cues). Nothing reads back:
 * the host keeps what it set (the driver replays it after every connect). [AuxStrips] An aux bus carries audio (its
 * record slots, its return) only where the unit's StripAuxMask builds the Input Strips of its return (GET_STRIPS
 * version 5 AuxMask); while they run, the return and its cue sends are written as a stereo pair, the pan as a balance.
 */
#define APOLLO_MON_AUX_SEND(a, b)   (0x6000u | ((a) << 5) | (b))    /* input b's send to aux a: half-dB -120..24, -121 off */
#define APOLLO_MON_AUX_PAN(a, b)    (0x6100u | ((a) << 5) | (b))    /* -100 left .. 100 right */
#define APOLLO_MON_AUX_POST(a, b)   (0x6200u | ((a) << 5) | (b))    /* 0 pre-fader (default), 1 post-fader */
#define APOLLO_MON_AUX_RETURN(a)    (0x6300u | ((a) << 5))          /* aux a into the monitor mix: half-dB, -121 off */
#define APOLLO_MON_AUX_MUTE(a)      (0x6400u | ((a) << 5))          /* 0/1 */
#define APOLLO_MON_AUX_CUE(a, c)    (0x6500u | ((a) << 5) | (c))    /* aux a into cue c: half-dB, -121 off */
#define APOLLO_MON_AUX_CUE_PAN(a, c) (0x6600u | ((a) << 5) | (c))   /* -100 left .. 100 right */

typedef struct _APOLLO_MONITOR_PARAM {
    ULONG   Param;              /* APOLLO_MON_* */
    LONG    Value;
} APOLLO_MONITOR_PARAM, *PAPOLLO_MONITOR_PARAM;

/*
 * GET_LEVELS: per record slot, the largest |sample| over the newest
 * Frames frames of the capture ring (24-bit full scale = 0x7FFFFF).
 * Valid is 0 while the transport is stopped.
 */
#define APOLLO_LEVEL_CHANNELS   64

typedef struct _APOLLO_LEVELS {
    ULONG     Valid;
    ULONG     Channels;         /* slots filled in Peak[] */
    ULONG     Frames;           /* window length */
    ULONG     Reserved;
    ULONGLONG Position;         /* hardware frame at the end of the window */
    ULONG     Peak[APOLLO_LEVEL_CHANNELS];
} APOLLO_LEVELS, *PAPOLLO_LEVELS;

/*
 * GET_ENGINE_STATS: the 1 ms copy engine (docs/PHASE2.md section 5). Optional
 * input: one ULONG, bit 0 = reset the maxima and event counters after this
 * read. While the engine runs, GET_POSITION, READ_CAPTURE, WRITE_PLAYBACK
 * and GET_LEVELS all use the engine's position snapshot (one SAMPLE_POS reader).
 */
#define APOLLO_ENGINE_STATS_VERSION 1
#define APOLLO_ENGINE_RESET         0x1

#define APOLLO_ASIO_STATE_CLOSED    0
#define APOLLO_ASIO_STATE_OPEN      1   /* buffers mapped, not streaming */
#define APOLLO_ASIO_STATE_RUNNING   2

typedef struct _APOLLO_ENGINE_STATS {
    ULONG     Version;              /* APOLLO_ENGINE_STATS_VERSION */
    ULONG     Running;              /* 1 while the 1 ms tick is armed */
    ULONG     RenderLeadFrames;     /* client data written this far ahead of SAMPLE_POS */
    ULONG     CaptureGuardFrames;   /* capture read up to SAMPLE_POS minus this */
    ULONG     ZeroAheadFrames;      /* playback frames zeroed this far ahead */
    ULONG     Starts;               /* engine starts since the driver loaded */
    ULONGLONG Ticks;
    ULONGLONG Position;             /* snapshot: 64-bit hardware frame */
    LONGLONG  Qpc;                  /* snapshot: QPC of that read */
    LONGLONG  QpcFrequency;
    ULONG     MaxTickIntervalUs;    /* longest gap between two ticks */
    ULONG     MaxTickDurationUs;    /* longest tick */
    ULONG     IntervalsOver2ms;
    ULONG     IntervalsOver4ms;
    ULONG     LateTicks;            /* render head fell behind the hardware (frames lost) */
    ULONG     BusStalls;            /* one SAMPLE_POS read took over 1 ms */
    ULONG     DeadReads;            /* SAMPLE_POS read all ones */
    ULONG     WrapFixes;            /* ring wraps added from the QPC estimate */
    ULONG     EndOfBufferCount;     /* interrupts: one per 8192-frame wrap (witness) */
    ULONG     AsioState;            /* APOLLO_ASIO_STATE_* */
    ULONG     AsioBufferFrames;
    ULONG     AsioInputs;
    ULONG     AsioOutputs;
    ULONG     AsioSwitches;         /* halves published */
    ULONG     AsioUnderruns;        /* render halves zeroed: the client was late */
    ULONG     AsioOverruns;         /* capture halves overwritten before the client finished */
    ULONG     AsioResyncs;          /* re-originations (engine restart, long stall) */
    ULONG     AsioClientDone;       /* last SwitchCount the client reported done */
    ULONG     EngineLost;           /* 1: the tick stopped all MMIO (device not answering) */
    ULONG     AsioClientSkipped;    /* switches the ASIO DLL never processed (it woke too late) */
    ULONG     Reserved[4];
} APOLLO_ENGINE_STATS, *PAPOLLO_ENGINE_STATS;

/*
 * GET_ENDPOINTS: the Windows audio endpoints (ACX circuits, docs/PHASE2.md section 3) and the
 * stream on each. Counters accumulate since the driver loaded. Which rows exist is set by the
 * unit's registry value Endpoints (bit i = row i; older builds read EnabledEndpoints),
 * or Parameters\Aggregate\Endpoints for the combined set.
 * Rows (driver\endpoint_plan.h / apollo_agg.h): every output jack pair, the
 * monitor-mix loopback, every mic/line input (mono), VIRTUAL and S/PDIF inputs (opt-in).
 *
 * Version 2 ([Endpoints]): a table may have up to APOLLO_ENDPOINTS_ROWS_MAX rows, more than the
 * 16 of APOLLO_ENDPOINTS. Without breaking that layout (installer helper ioctl_check.cpp asserts
 * it):
 *   - the output buffer must still hold an APOLLO_ENDPOINTS (16 rows); a larger one gets as many
 *     rows as fit, and the call returns FIELD_OFFSET(Endpoint) + rows * sizeof(row), never less
 *     than sizeof(APOLLO_ENDPOINTS). A version-1 client (exactly sizeof(APOLLO_ENDPOINTS)) gets
 *     rows 0-15 and that size, as before;
 *   - an optional input APOLLO_ENDPOINTS_QUERY names the first row (paging with the fixed struct);
 *     Endpoint[0] is then row FirstRow, and EnabledMask holds bits FirstRow..FirstRow+31;
 *   - Count stays the total number of rows in the table;
 *   - the row's former Reserved is Model (APOLLO_INFO_MODEL_* of the unit whose jack the row is,
 *     0 = the combined Monitor rows, which follow the routing); new flags DEFAULT, COMBINED, NO_SLOT.
 * EnabledMask and the DWORD registry values cover rows 0-31 (APOLLO_ENDPOINTS_ROWS_MAX).
 */
#define APOLLO_ENDPOINTS_VERSION    2
#define APOLLO_ENDPOINTS_MAX        16      /* rows in APOLLO_ENDPOINTS (one page; unchanged since v1) */
#define APOLLO_ENDPOINTS_ROWS_MAX   32      /* [Endpoints] rows a driver table may have (one DWORD mask) */
#define APOLLO_ENDPOINT_NAME_CHARS  24

#define APOLLO_EP_FLAG_ENABLED      0x01    /* in the unit's Endpoints mask in force (v1: the circuit was created) */
#define APOLLO_EP_FLAG_ADDED        0x02    /* added to the device: visible to Windows */
#define APOLLO_EP_FLAG_STREAM       0x04    /* a stream exists (an app opened the endpoint) */
#define APOLLO_EP_FLAG_PREPARED     0x08    /* the stream holds packets and is paused or running */
#define APOLLO_EP_FLAG_RUNNING      0x10    /* the stream is in RUN: Windows will veto removal */
#define APOLLO_EP_FLAG_DEFAULT      0x20    /* [v2] in the table's default set (absent registry value) */
#define APOLLO_EP_FLAG_COMBINED     0x40    /* [v2] a row of the combined set (Aggregate\EndpointSet 1) */
#define APOLLO_EP_FLAG_NO_SLOT      0x80    /* [v2] prepared, but a channel's ID is not on the unit now: silent */
#define APOLLO_EP_FLAG_SILENT       0x100   /* [Clock] the jack carries nothing at this rate and digital format (S/PDIF
                                           above 96 kHz, ADAT in S/PDIF format, ADAT 5-8 at 88.2/96 on the Twin X:
                                           driver\clock_plan.h ApolloDigSlotLive; only once a format was set) */

#define APOLLO_EP_SLOT_NONE         0xFFFFFFFFul    /* [v2] Slot[] of a channel without a slot (NO_SLOT) */

typedef struct _APOLLO_ENDPOINT_INFO {
    CHAR      Name[APOLLO_ENDPOINT_NAME_CHARS];     /* pin name, e.g. "LINE 1-2" */
    CHAR      Circuit[APOLLO_ENDPOINT_NAME_CHARS];  /* circuit name = INF reference string */
    ULONG     Render;               /* 1 render (playback), 0 capture */
    ULONG     Channels;
    ULONG     Slot[2];              /* ring slots the stream uses (resolved at prepare) */
    ULONG     Flags;                /* APOLLO_EP_FLAG_* */
    ULONG     PacketCount;          /* 2 = event-driven, 1 = timer-driven */
    ULONG     PacketBytes;
    ULONG     Streams;              /* streams created on this endpoint */
    ULONGLONG Frames;               /* current stream: frames moved */
    ULONGLONG Packets;              /* all streams: packets completed */
    ULONG     Underruns;            /* render packets played as silence: the app was late */
    ULONG     Overruns;             /* capture packets overwritten before the app read them */
    ULONG     Dropped;              /* frames skipped after a late engine tick */
    ULONG     LatePackets;          /* SetRenderPacket for a packet already playing */
    ULONG     NotifyFailures;       /* AcxRtStreamNotifyPacketComplete errors */
    ULONG     Model;                /* [v2] APOLLO_INFO_MODEL_* of the row's unit (v1: Reserved, 0) */
} APOLLO_ENDPOINT_INFO, *PAPOLLO_ENDPOINT_INFO;

typedef struct _APOLLO_ENDPOINTS {
    ULONG     Version;              /* APOLLO_ENDPOINTS_VERSION */
    ULONG     Count;                /* rows in the driver's table */
    ULONG     EnabledMask;          /* the Endpoints mask in force (v2: bits FirstRow..FirstRow+31) */
    ULONG     Prepared;             /* streams holding packets */
    APOLLO_ENDPOINT_INFO Endpoint[APOLLO_ENDPOINTS_MAX];   /* v2: as many as the buffer holds */
} APOLLO_ENDPOINTS, *PAPOLLO_ENDPOINTS;

/* [v2] GET_ENDPOINTS input (optional): rows from FirstRow on. */
typedef struct _APOLLO_ENDPOINTS_QUERY {
    ULONG     FirstRow;
} APOLLO_ENDPOINTS_QUERY, *PAPOLLO_ENDPOINTS_QUERY;

#define APOLLO_READBACK_WORDS   40

typedef struct _APOLLO_READBACK {
    ULONG   Valid;              /* 1 once a readback has been drained */
    ULONG   Sequence;           /* increments per drained readback */
    ULONG   Words[APOLLO_READBACK_WORDS];
} APOLLO_READBACK, *PAPOLLO_READBACK;

/*
 * GET_STRIPS: the Console input strips (docs/STRIPS.md). Status only. The module bytes come
 * from files apollo-diag writes to an admin-only directory, and the driver re-verifies them
 * against the pins in apollo_strip_pins.h: no IOCTL carries module bytes.
 */
#define APOLLO_STRIPS_STATE_OFF         0   /* disabled, see Why */
#define APOLLO_STRIPS_STATE_IDLE        1   /* nothing of ours on the strip DSP */
#define APOLLO_STRIPS_STATE_ARMED       2   /* waiting for the settle time and 48 kHz */
#define APOLLO_STRIPS_STATE_BUILDING    3   /* a pass is running */
#define APOLLO_STRIPS_STATE_LIVE        4   /* the pass completed (this build: uploads only) */
#define APOLLO_STRIPS_STATE_FAILED      5   /* a step failed or the DSP is quarantined, see Why */

#define APOLLO_STRIPS_FLAG_OWNED        0x01    /* strip DSP's boot word said: no host since power-up */
#define APOLLO_STRIPS_FLAG_QUARANTINED  0x02    /* an outcome could not be proven: power-cycle */
#define APOLLO_STRIPS_FLAG_TAINTED      0x04    /* a raw register write: power-cycle */
#define APOLLO_STRIPS_FLAG_MODULES_OK   0x08    /* all five module files read and verified */
#define APOLLO_STRIPS_FLAG_BOOT_READ    0x10    /* BootWord[] valid */
#define APOLLO_STRIPS_FLAG_LAYOUT_READ  0x20    /* Geometry[] and LayoutWhy valid */
#define APOLLO_STRIPS_FLAG_POOL_READY   0x40    /* transaction buffers allocated */

#define APOLLO_STRIP_MOD_NONE           0
#define APOLLO_STRIP_MOD_UPLOADING      1   /* published, the reply not proven */
#define APOLLO_STRIP_MOD_RESIDENT       2   /* the DSP accepted it: {0x80070004, 0, id, x} */
#define APOLLO_STRIP_MOD_REFUSED        3   /* the DSP answered something else */

#define APOLLO_STRIP_MODULE_SLOTS       5   /* EB DA A5 C2 12B */
#define APOLLO_STRIPS_WHY_CHARS         96
#define APOLLO_STRIP_INPUTS             8   /* preamps 1-8 */
#define APOLLO_STRIP_VIRTUALS           8   /* [Virtual] VIRTUAL 1-8 (Input Strips, GET_STRIPS version 4) */
#define APOLLO_STRIP_VIRTUAL_BUS0       0x18    /* DSP input = mixer bus of VIRTUAL 1 (VIRTUAL n = 0x18 + n - 1) */
#define APOLLO_STRIP_AUXES              4   /* [AuxStrips] the aux returns AUX 1 L/R, AUX 2 L/R (GET_STRIPS version 5) */
#define APOLLO_STRIP_AUX_BUS0           0x10    /* DSP input = bus of AUX 1 L (aux return k = 0x10 + k) */
#define APOLLO_STRIP_AUX_RETURNS        2   /* AUX 1, AUX 2: return k/2 */

#define APOLLO_STRIP_NONE               0   /* per input: nothing sent */
#define APOLLO_STRIP_QUEUED             1   /* its transaction published, not proven taken */
#define APOLLO_STRIP_TAKEN              2   /* the DSP took the whole transaction */

#define APOLLO_STRIPS_PHANTOM_NOT_RUN       0
#define APOLLO_STRIPS_PHANTOM_DONE          1   /* PhantomMask: 48V written 0, then 1 */
#define APOLLO_STRIPS_PHANTOM_NONE_ON       2   /* no preamp in StripMask reports 48V on */
#define APOLLO_STRIPS_PHANTOM_NO_READBACK   3   /* no hardware readback yet: skipped */
#define APOLLO_STRIPS_PHANTOM_NOT_COMMITTED 4   /* written, but a mailbox batch was not taken in time */

/* Registry MixerInitBatch (docs/STRIPS.md section 6); absent = X8P_SAFE since release5 (was LEGACY) */
#define APOLLO_MIXER_BATCH_NONE         0   /* nothing: firmware defaults */
#define APOLLO_MIXER_BATCH_X8P_SAFE     1   /* 0x20/0xFF without the x8p's preamp settings */
#define APOLLO_MIXER_BATCH_LEGACY       2   /* S1..S37 but S2 at 0x20/0xFF, S35 0x05/0x1F */

typedef struct _APOLLO_STRIP_MODULE_INFO {
    ULONG   Id;
    ULONG   Dest;               /* code-pool address on the strip DSP */
    LONG    FileStatus;         /* NTSTATUS of reading and verifying <id>.bill; 0x103 = not read yet */
    ULONG   DspState;           /* APOLLO_STRIP_MOD_* */
    ULONG   Reply[4];           /* the DSP's reply to the upload */
} APOLLO_STRIP_MODULE_INFO, *PAPOLLO_STRIP_MODULE_INFO;

typedef struct _APOLLO_STRIPS_INFO {
    ULONG   Version;            /* 1 */
    ULONG   State;              /* APOLLO_STRIPS_STATE_* */
    ULONG   Flags;              /* APOLLO_STRIPS_FLAG_* */
    ULONG   Enable;             /* registry StripsEnable: 0 off, 1 build, 2 dry run */
    ULONG   Steps;              /* registry StripSteps */
    ULONG   Mask;               /* registry StripMask (bit i = input i+1; used from the strip step on) */
    ULONG   Dsp;                /* registry StripDsp */
    ULONG   LayoutWhy;          /* 0 = the pools fit the recipe for 8 strips */
    ULONG   BootWord[8];        /* window +0x1A4 per DSP, read before the ring connect */
    ULONG   Geometry[11];       /* strip DSP: size 0-3, base 0-3, scratch 1-3 */
    APOLLO_STRIP_MODULE_INFO Module[APOLLO_STRIP_MODULE_SLOTS];
    ULONG   Passes;
    ULONG   Uploads;
    ULONG   RepliesOk;
    ULONG   RepliesBad;
    ULONG   Timeouts;
    ULONG   Teardowns;
    ULONG   LastCmdEnd;         /* the last transaction that ended unproven: ring indexes */
    ULONG   LastCmdPos;
    ULONG   LastRespEnd;
    ULONG   LastRespPos;
    CHAR    Why[APOLLO_STRIPS_WHY_CHARS];
    /* Version 2: the strip stages (StripSteps 2-9) */
    ULONG   StepsLimit;         /* the stage StripSteps asks for (0 maps to 9) */
    ULONG   StepsDone;          /* the last stage the last pass completed (1 = uploads) */
    ULONG   MixerInitBatch;     /* APOLLO_MIXER_BATCH_* in use */
    ULONG   PhantomState;       /* APOLLO_STRIPS_PHANTOM_* */
    ULONG   PhantomMask;        /* preamps whose 48V was written off, then on */
    ULONG   RoutesSent;         /* route words queued to the DSPs that do not host a strip */
    ULONG   StripState[APOLLO_STRIP_INPUTS];    /* APOLLO_STRIP_NONE/QUEUED/TAKEN */
    ULONG   StripStarted[APOLLO_STRIP_INPUTS];  /* 1 = its SYNCH went out */
    ULONG   StripHeader[APOLLO_STRIP_INPUTS];   /* instance header address on the strip DSP */
    ULONG   StripAckMs[APOLLO_STRIP_INPUTS];    /* publish to taken */
    ULONG   BusOwner[APOLLO_STRIP_INPUTS];      /* DSP that mixes input i's bus */
    /*
     * Version 3 [Warm]: warm starts (docs/STRIPS.md 12). A driver fills as much of the structure as the
     * caller's buffer holds, down to the version 2 size (APOLLO_STRIPS_INFO_V2_SIZE).
     */
    ULONG   Adopt;              /* APOLLO_STRIPS_ADOPT_*: what this start decided about strips it found */
    ULONG   Anchor;             /* APOLLO_STRIPS_ANCHOR_*: may this start trust the DSPs' state? */
    ULONG   AdoptFlags;         /* APOLLO_STRIPS_AF_* */
    ULONG   RecordState;        /* the record kept now: APOLLO_STRIPS_REC_* */
    ULONG   RecordBootWord[8];  /* the boot words it holds: right after this driver's last ring connect */
    ULONG   RecordAdoptions;    /* warm starts that adopted the running strips */
    ULONG   RecordBuiltDate;    /* when they were built: (days since 2007-01-01 << 12) + minute of day, UTC */
    ULONG   RecordPlugins;      /* plug-in instances on the strip DSP at the last write */
    LONG    RecordStatus;       /* NTSTATUS of the last record read or write */
    /*
     * Version 4 [Virtual]: the Input Strips on the Virtual inputs (docs/STRIPS.md 13). A driver fills as much of the
     * structure as the caller's buffer holds, down to the version 3 size (APOLLO_STRIPS_INFO_V3_SIZE), then 2.
     * VIRTUAL n is index n-1 below; its DSP input and mixer bus is VirtualBus0 + n - 1.
     */
    ULONG   VirtualMask;        /* the Virtual strips this start builds or adopted (bit n-1 = VIRTUAL n) */
    ULONG   VirtualAsked;       /* registry StripVirtualMask */
    ULONG   VirtualLayoutWhy;   /* 0, or the pool check's code (8 + pool: no room) that refused VirtualAsked */
    ULONG   VirtualBus0;        /* APOLLO_STRIP_VIRTUAL_BUS0 */
    ULONG   VStripState[APOLLO_STRIP_VIRTUALS];     /* APOLLO_STRIP_NONE/QUEUED/TAKEN */
    ULONG   VStripStarted[APOLLO_STRIP_VIRTUALS];   /* 1 = its SYNCH went out */
    ULONG   VStripHeader[APOLLO_STRIP_VIRTUALS];    /* instance header address on the strip DSP */
    ULONG   VStripAckMs[APOLLO_STRIP_VIRTUALS];     /* its PREPARE: publish to taken */
    ULONG   VBusOwner[APOLLO_STRIP_VIRTUALS];       /* DSP that mixes VIRTUAL n's bus (0 = default) */
    /*
     * Version 5 [AuxStrips]: the Input Strips on the aux returns (docs/STRIPS.md 14). A driver fills as much of the
     * structure as the caller's buffer holds, down to the version 4 size (APOLLO_STRIPS_INFO_V4_SIZE), then 3, then 2.
     * Index k below is DSP input AuxBus0 + k: 0 AUX 1 L, 1 AUX 1 R, 2 AUX 2 L, 3 AUX 2 R.
     */
    ULONG   AuxMask;            /* the aux-return strips this start builds or adopted (bit k = DSP input 0x10 + k) */
    ULONG   AuxAsked;           /* registry StripAuxMask */
    ULONG   AuxLayoutWhy;       /* 0, or the pool check's code (8 + pool: no room) that refused AuxAsked */
    ULONG   AuxBus0;            /* APOLLO_STRIP_AUX_BUS0 */
    ULONG   AStripState[APOLLO_STRIP_AUXES];        /* APOLLO_STRIP_NONE/QUEUED/TAKEN */
    ULONG   AStripStarted[APOLLO_STRIP_AUXES];      /* 1 = its SYNCH went out */
    ULONG   AStripHeader[APOLLO_STRIP_AUXES];       /* instance header address on the strip DSP */
    ULONG   AStripAckMs[APOLLO_STRIP_AUXES];        /* its PREPARE: publish to taken */
    ULONG   ABusOwner[APOLLO_STRIP_AUXES];          /* DSP that mixes that return input's bus (0 = default) */
    ULONG   AuxReturnOwner[APOLLO_STRIP_AUX_RETURNS];   /* DSP the return's coefficients go to besides DSP 0 (0 = DSP 0 only):
                                                       the strip DSP while its strips run, an aux insert chain's DSP */
    ULONG   AuxReturnStereo;    /* bit a: AUX a+1's return and its cue sends go out as a stereo pair (L input left entry,
                                   R input right entry), because its Input Strips run; 0 = the single left-entry write */
} APOLLO_STRIPS_INFO, *PAPOLLO_STRIPS_INFO;

#define APOLLO_STRIPS_INFO_V2_SIZE      ((ULONG)FIELD_OFFSET(APOLLO_STRIPS_INFO, Adopt))
#define APOLLO_STRIPS_INFO_V3_SIZE      ((ULONG)FIELD_OFFSET(APOLLO_STRIPS_INFO, VirtualMask))
#define APOLLO_STRIPS_INFO_V4_SIZE      ((ULONG)FIELD_OFFSET(APOLLO_STRIPS_INFO, AuxMask))

/* Version 3: Adopt (= driver\strip_adopt.h STRIP_ADOPT_*) */
#define APOLLO_STRIPS_ADOPT_NONE            0   /* not a warm start, or strips off */
#define APOLLO_STRIPS_ADOPT_ADOPTED         1   /* this driver's strips were running: adopted, nothing sent */
#define APOLLO_STRIPS_ADOPT_RESTORE         2   /* ... and re-SYNCHed, a plug-in chain may have sat on them */
#define APOLLO_STRIPS_ADOPT_REBUILD         3   /* built again at a warm start (proven: only this driver since power-up) */
#define APOLLO_STRIPS_ADOPT_NOT_ANCHORED    4   /* not proven this driver's: see Anchor; power-cycle */
#define APOLLO_STRIPS_ADOPT_NOT_RUNNING     5   /* none of ours run there; StripWarmRebuild 0 */
#define APOLLO_STRIPS_ADOPT_UNKNOWN_STATE   6   /* the last session ended mid-pass; StripWarmRebuild 0 */
#define APOLLO_STRIPS_ADOPT_TORN_DOWN       7   /* the last exit tore them down; StripWarmRebuild 0 */
#define APOLLO_STRIPS_ADOPT_PLAN_DIFFERS    8   /* running, but not this start's plan; StripWarmRebuild 0 */
#define APOLLO_STRIPS_ADOPT_POOLS_DIFFER    9   /* the strip DSP's pool registers moved */
#define APOLLO_STRIPS_ADOPT_DISABLED        10  /* StripAdopt 0 */
#define APOLLO_STRIPS_ADOPT_DROPPED         11  /* the model's warm connect dropped them; StripWarmRebuild 0 */
#define APOLLO_STRIPS_ADOPT_LAYOUT          12  /* the pools do not fit the recipe */

/* Version 3: Anchor (= driver\strip_adopt.h STRIP_ANCHOR_*) */
#define APOLLO_STRIPS_ANCHOR_OK             0   /* only this driver used the DSPs since the last power-up */
#define APOLLO_STRIPS_ANCHOR_COLD_USED      1
#define APOLLO_STRIPS_ANCHOR_NO_RECORD      2   /* "unsigned": strips built by a driver without records */
#define APOLLO_STRIPS_ANCHOR_BAD_RECORD     3
#define APOLLO_STRIPS_ANCHOR_OTHER_UNIT     4
#define APOLLO_STRIPS_ANCHOR_LOST_BEFORE    5
#define APOLLO_STRIPS_ANCHOR_OTHER_HOST     6   /* the boot words changed since this driver's last connect */
#define APOLLO_STRIPS_ANCHOR_NO_ADVANCE     7
#define APOLLO_STRIPS_ANCHOR_NOT_READ       8

#define APOLLO_STRIPS_AF_ADOPTED            0x01    /* the Live strips were adopted at this start */
#define APOLLO_STRIPS_AF_REBUILT_WARM       0x02    /* the Live strips were built at a warm start */
#define APOLLO_STRIPS_AF_RECORD             0x04    /* a record is kept for this unit */
#define APOLLO_STRIPS_AF_KEEP               0x08    /* StripAdopt: a D0 exit leaves Live strips running */
#define APOLLO_STRIPS_AF_WARM_REBUILD       0x10    /* StripWarmRebuild */
#define APOLLO_STRIPS_AF_PLUGINS_OK         0x20    /* plug-ins may be placed on the adopted strip DSP */
#define APOLLO_STRIPS_AF_ANCHORED           0x40    /* the record kept now is anchored */
#define APOLLO_STRIPS_AF_WARM_PLUGINS       0x80    /* StripWarmPlugins: plug-ins on the strip DSP after a warm adoption or rebuild */
#define APOLLO_STRIPS_AF_UPLOADS_PROVEN     0x100   /* this warm start's strip DSP acknowledged an upload (a plug-in module,
                                                       or the warm rebuild's own) */

#define APOLLO_STRIPS_REC_NONE              0       /* (= strip_record.h STRIP_REC_*) */
#define APOLLO_STRIPS_REC_RUNNING           1
#define APOLLO_STRIPS_REC_UNKNOWN           2
#define APOLLO_STRIPS_REC_ABSENT            0xFFFFFFFFu     /* no record kept for this unit */

/* Version 8: UAD plug-in hosting, block 0x40-0x43 (docs/PLUGINS.md gate P0). */
#include "apollo_plugin.h"
