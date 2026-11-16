/*
 * apollo_agg.h - the combined device (gate G6, docs/spec-twinx.md 6.2-6.9): one timeline over every
 * Apollo apollo.sys runs, one ASIO client for all of them.
 *
 * Any unit's handle reaches the aggregate (it is driver-global). Codes 0x30-0x3F:
 *   AGG_GET_STATE   user   members, roles, clock loop per follower, latencies, counters
 *   AGG_GET_LAYOUT  user   the logical channel table the combined ASIO driver lists
 *   AGG_SET_CONFIG  user   members and order, master, ASRC size, channel order, routes, plus the
 *                          caller's JSON text (kept verbatim for Apollo Control). The driver
 *                          validates it and stores it itself under Parameters\Aggregate, so no
 *                          elevation is needed (docs/apollo-control-units.md, "Elevation, decided")
 *   AGG_GET_CONFIG  user   the stored configuration (or the driver's default, flagged)
 *   AGG_CLOCK_PROBE admin  the Thunderbolt peer-clock experiment (spec 6.3.7) on the handle's unit
 * The combined ASIO client opens with IOCTL_APOLLO_ASIO_OPEN and APOLLO_ASIO_OPEN_AGG_IN (Version 3)
 * on any unit's handle; START/STOP/CLOSE on that handle then drive it (apollo_asio.h).
 * [G7] The combined Windows endpoints (EndpointSet below) run on the same timeline and routing; the
 * structures keep their G6 sizes (new fields took Reserved space), APOLLO_AGG_VERSION stays 1.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once
#include "apollo_ioctl.h"

#define IOCTL_APOLLO_AGG_GET_STATE      APOLLO_CTL(0x30, APOLLO_USER)
#define IOCTL_APOLLO_AGG_GET_LAYOUT     APOLLO_CTL(0x31, APOLLO_USER)
#define IOCTL_APOLLO_AGG_SET_CONFIG     APOLLO_CTL(0x32, APOLLO_USER)
#define IOCTL_APOLLO_AGG_CLOCK_PROBE    APOLLO_CTL(0x33, APOLLO_ADMIN)
#define IOCTL_APOLLO_AGG_GET_CONFIG     APOLLO_CTL(0x34, APOLLO_USER)

#define APOLLO_AGG_VERSION          1
#define APOLLO_AGG_MAX_UNITS        4
#define APOLLO_AGG_MAX_CH           128     /* logical inputs, and logical outputs */
#define APOLLO_AGG_MAX_DESTS        4       /* destinations per logical output (mirroring) */
#define APOLLO_AGG_MAX_ROUTES       16
#define APOLLO_AGG_UNIT_SLOTS       64      /* descriptor slots cached per unit and direction */
#define APOLLO_AGG_JSON_MAX         16384
#define APOLLO_AGG_NAME_CHARS       24

/* APOLLO_AGG_CONFIG.Flags */
#define APOLLO_AGG_CFG_ENABLED      0x0001  /* the combined device exists (ASIO "Apollo (Avium)") */
#define APOLLO_AGG_CFG_ALIGN        0x0002  /* every channel delayed to the slowest unit (6.3.5); clear =
                                               "lowest latency, unaligned": no capture padding, followers read
                                               as fresh as the resampler allows, latency = the timeline's */
#define APOLLO_AGG_CFG_EXPLICIT     0x0004  /* In[]/Out[] give the logical order ("advanced"); else whole blocks */
#define APOLLO_AGG_CFG_DEFAULT      0x0100  /* GET_CONFIG: nothing stored, this is the driver's default */

/*
 * One member, in block order. Uid and Model come from the caller; the rest the driver fills.
 * [G7] CaptureTrim / RenderTrim: the unit's Units\<key>\CaptureTrimFrames / RenderTrimFrames (signed,
 * -64..64, spec 6.3.5), cached here by the driver like the descriptor so an absent unit keeps them.
 * They were one USHORT Reserved (always 0) before G7: the layout and size are unchanged.
 */
typedef struct _APOLLO_AGG_UNIT_CFG {
    ULONGLONG   Uid;                /* HW_UID (BAR0 0x34:0x30), APOLLO_INFO HwUidHi:HwUidLo */
    ULONG       Model;              /* APOLLO_INFO_MODEL_*, 0 = not known yet */
    UCHAR       InCount;            /* driver: the unit's record descriptor as last seen (0 = never) */
    UCHAR       OutCount;           /* driver: its playback descriptor as last seen */
    CHAR        CaptureTrim;        /* driver: its capture channels later (+) or earlier (-), frames */
    CHAR        RenderTrim;         /* driver: its playback later (+) or earlier (-), frames */
    USHORT      InIds[APOLLO_AGG_UNIT_SLOTS];   /* driver: channel IDs in slot order */
    USHORT      OutIds[APOLLO_AGG_UNIT_SLOTS];
} APOLLO_AGG_UNIT_CFG;

/* One logical channel of an explicit order. */
#define APOLLO_AGG_CHAN_HIDDEN      0x01
typedef struct _APOLLO_AGG_CHAN_CFG {
    UCHAR       Unit;               /* index into Unit[] */
    UCHAR       Flags;              /* APOLLO_AGG_CHAN_HIDDEN: left out of the ASIO list */
    USHORT      Slot;               /* ring slot on that unit */
} APOLLO_AGG_CHAN_CFG;

/* One stereo route (spec 6.8): a source pair to a destination pair. */
#define APOLLO_AGG_SRC_DAW          1       /* logical outputs 1-2 */
#define APOLLO_AGG_SRC_WINDOWS      2       /* the combined Windows "Monitor" endpoint (none: the master's MON) */
#define APOLLO_AGG_SRC_PORT         3       /* the logical outputs carrying SrcUnit's SrcId[] */
typedef struct _APOLLO_AGG_ROUTE_CFG {
    UCHAR       SrcKind;            /* APOLLO_AGG_SRC_* */
    UCHAR       SrcUnit;
    UCHAR       DstUnit;
    UCHAR       Reserved;
    USHORT      SrcId[2];
    USHORT      DstId[2];           /* playback channel IDs on DstUnit (0x0901/0x0902 = MON L/R) */
} APOLLO_AGG_ROUTE_CFG;

typedef struct _APOLLO_AGG_CONFIG {
    ULONG       Version;            /* APOLLO_AGG_VERSION */
    ULONG       Bytes;              /* sizeof(APOLLO_AGG_CONFIG) */
    ULONG       Flags;              /* APOLLO_AGG_CFG_* */
    ULONG       UnitCount;          /* 1..APOLLO_AGG_MAX_UNITS */
    ULONG       Master;             /* configured clock master: index into Unit[] */
    ULONG       AsrcHalf;           /* resampler half-length h: 8 (low CPU), 16 (default), 32 (high) */
    ULONG       InCount;            /* explicit order only */
    ULONG       OutCount;
    ULONG       RouteCount;
    ULONG       JsonBytes;          /* Json[] text, not NUL terminated; 0 = none */
    ULONG       Reserved[6];
    APOLLO_AGG_UNIT_CFG  Unit[APOLLO_AGG_MAX_UNITS];
    APOLLO_AGG_CHAN_CFG  In[APOLLO_AGG_MAX_CH];
    APOLLO_AGG_CHAN_CFG  Out[APOLLO_AGG_MAX_CH];
    APOLLO_AGG_ROUTE_CFG Route[APOLLO_AGG_MAX_ROUTES];
    CHAR        Json[APOLLO_AGG_JSON_MAX];  /* Apollo Control's config object, verbatim */
} APOLLO_AGG_CONFIG;

/*
 * AGG_SET_CONFIG output. A configuration the driver refuses completes with SUCCESS and
 * Error = APOLLO_AGG_ERR_* (nothing stored; LayoutVersion = the one in force): with METHOD_BUFFERED an
 * error status would bring no output back, so the reason never reached the caller. Callers must test
 * Error. A short input or output buffer still fails (STATUS_BUFFER_TOO_SMALL).
 */
typedef struct _APOLLO_AGG_SET_RESULT {
    ULONG       LayoutVersion;
    ULONG       Error;              /* 0 = stored; APOLLO_AGG_ERR_* = refused, nothing changed */
} APOLLO_AGG_SET_RESULT;

#define APOLLO_AGG_ERR_VERSION      1
#define APOLLO_AGG_ERR_UNITS        2
#define APOLLO_AGG_ERR_MASTER       3
#define APOLLO_AGG_ERR_ASRC         4
#define APOLLO_AGG_ERR_CHANNELS     5
#define APOLLO_AGG_ERR_ROUTES       6
#define APOLLO_AGG_ERR_JSON         7
#define APOLLO_AGG_ERR_DUPLICATE    8

/* AGG_GET_LAYOUT: one logical channel */
#define APOLLO_AGG_CH_PRESENT       0x01    /* its unit runs now (else it streams silence) */
#define APOLLO_AGG_CH_RESAMPLED     0x02    /* its unit follows the timeline through the ASRC */
#define APOLLO_AGG_CH_ROUTED        0x04    /* output: a route sends it somewhere other than its own slot */
#define APOLLO_AGG_CH_TIMELINE      0x08    /* its unit is the timeline (master or acting master) */
typedef struct _APOLLO_AGG_LCHAN {
    UCHAR       Unit;               /* index into the config's Unit[] */
    UCHAR       Flags;              /* APOLLO_AGG_CH_* */
    USHORT      Slot;
    USHORT      Id;                 /* descriptor channel ID */
    USHORT      LatencyFrames;      /* this channel's own latency without the buffer (unaligned) */
    CHAR        Name[APOLLO_AGG_NAME_CHARS];    /* "x8p MIC/LINE 1", "TwinX MON L" */
} APOLLO_AGG_LCHAN;

#define APOLLO_AGG_LAYOUT_ACTIVE    0x01    /* enabled and at least one member runs */
typedef struct _APOLLO_AGG_LAYOUT {
    ULONG       Version;            /* APOLLO_AGG_VERSION */
    ULONG       LayoutVersion;      /* bumped by every change of the lists (ASIO: reset request) */
    ULONG       Flags;              /* APOLLO_AGG_LAYOUT_* */
    ULONG       InCount;
    ULONG       OutCount;
    ULONG       InLatency;          /* aligned input latency without the buffer (what ASIO reports) */
    ULONG       OutLatency;
    ULONG       UnitCount;
    ULONG       Master;             /* configured */
    ULONG       Timeline;           /* the unit whose frames clients count; ~0 = none */
    ULONG       TimelineEpoch;      /* bumped whenever the timeline unit changes */
    ULONG       Reserved[5];
    ULONGLONG   Uid[APOLLO_AGG_MAX_UNITS];
    ULONG       Model[APOLLO_AGG_MAX_UNITS];
    ULONG       Present[APOLLO_AGG_MAX_UNITS];
    APOLLO_AGG_LCHAN In[APOLLO_AGG_MAX_CH];
    APOLLO_AGG_LCHAN Out[APOLLO_AGG_MAX_CH];
} APOLLO_AGG_LAYOUT;

/* AGG_GET_STATE */
#define APOLLO_AGG_CLOCK_NONE       0       /* not a follower (timeline, absent, or alone) */
#define APOLLO_AGG_CLOCK_ACQUIRE    1       /* loop acquiring: its channels muted */
#define APOLLO_AGG_CLOCK_LOCKED     2       /* |e| < 0.5 frame: channels live */
#define APOLLO_AGG_CLOCK_DEGRADED   3       /* ratio out of range or resyncing too often: muted, retried */

#define APOLLO_AGG_UNIT_PRESENT     0x01
#define APOLLO_AGG_UNIT_MASTER      0x02    /* configured master */
#define APOLLO_AGG_UNIT_TIMELINE    0x04
#define APOLLO_AGG_UNIT_USABLE      0x08    /* engine running at the last tick */

typedef struct _APOLLO_AGG_UNIT_STATE {
    ULONGLONG   Uid;
    ULONG       Model;
    ULONG       Flags;              /* APOLLO_AGG_UNIT_* */
    ULONG       ClockState;         /* APOLLO_AGG_CLOCK_* */
    LONG        RatioPpb;           /* (r - 1) * 1e9: follower frames per timeline frame */
    LONG        ErrorMilli;         /* filtered phase error, 1/1000 frame */
    LONG        ErrorPeakMilli;     /* largest |raw e| since the last reset */
    ULONG       Resyncs;
    ULONG       Outliers;           /* raw errors beyond 2 frames (clamped before the loop) */
    ULONG       LockedMs;           /* time since the last lock */
    ULONG       GainMilli;          /* fade 0..1000 */
    ULONGLONG   Position;           /* the unit's last 64-bit SAMPLE_POS */
    ULONG       RenderLate;         /* follower render frames skipped (written too late) */
    ULONG       CaptureShort;       /* capture frames the follower had not recorded yet (zeros) */
    ULONG       InLatency;          /* own, without the buffer */
    ULONG       OutLatency;
} APOLLO_AGG_UNIT_STATE;

#define APOLLO_AGG_STATE_ENABLED    0x01
#define APOLLO_AGG_STATE_ACTIVE     0x02    /* a timeline runs (enabled, or the combined endpoints need it) */
#define APOLLO_AGG_STATE_STORED     0x04    /* a configuration is stored (else the default) */
#define APOLLO_AGG_STATE_ALIGNED    0x08    /* APOLLO_AGG_CFG_ALIGN in force */
#define APOLLO_AGG_STATE_RATIOS     0x10    /* start ratios were loaded from Parameters\Aggregate\Ratios */

/*
 * The Windows endpoint set (spec 6.7). Admin registry values under Parameters\Aggregate, read at
 * device add (a change needs both units restarted):
 *   EndpointSet   REG_DWORD  0 (absent) = standalone: each unit its own table, exactly as before G7;
 *                            1 = combined: the host unit (the x8p) creates the combined table below and
 *                            every other unit creates none
 *   Endpoints     REG_DWORD  [Endpoints] which rows of the combined table exist (default APOLLO_AGG_EP_DEFAULT).
 *                            Older builds read EndpointMask instead, over a 12-row table; the rows were
 *                            renumbered, so this build ignores EndpointMask (an older build still reads it).
 * AGG_GET_STATE.EndpointSet reports what runs: APOLLO_AGG_EPSET_*.
 */
#define APOLLO_AGG_EPSET_STANDALONE 0
#define APOLLO_AGG_EPSET_COMBINED   1
/*
 * [Endpoints] Rows (driver\apollo_agg.h APOLLO_AGG_EP_LIST): 0 Monitor, 1-8 x8p MON L/R, LINE 1-2..7-8,
 * HP 1, HP 2, S/PDIF, 9-11 Twin X MON L/R, LINE 3-4, HP (outputs); 12 Monitor mix, 13-20 x8p MIC/LINE 1-8,
 * 21-22 Twin X MIC/LINE 1-2 (inputs); 23-30 VIRTUAL and S/PDIF inputs of both units (opt-in).
 */
#define APOLLO_AGG_EP_ROWS          31
#define APOLLO_AGG_EP_DEFAULT       0x7FFFFFul  /* rows 0-22: every output, the loopback, every mic/line input */

typedef struct _APOLLO_AGG_STATE {
    ULONG       Version;
    ULONG       Flags;              /* APOLLO_AGG_STATE_* */
    ULONG       LayoutVersion;
    ULONG       TimelineEpoch;
    ULONG       UnitCount;
    ULONG       Master;
    ULONG       Timeline;           /* ~0 = none */
    ULONG       AsrcHalf;
    ULONG       InLatency;
    ULONG       OutLatency;
    ULONGLONG   Ticks;              /* aggregate ticks */
    ULONG       AsrcLastUs;         /* the aggregate part of the last tick */
    ULONG       AsrcMaxUs;
    ULONG       AsioState;          /* APOLLO_ASIO_STATE_* of the combined client */
    ULONG       AsioBufferFrames;
    ULONG       AsioInputs;
    ULONG       AsioOutputs;
    ULONG       AsioSwitches;
    ULONG       AsioUnderruns;
    ULONG       AsioOverruns;
    ULONG       AsioResyncs;
    /*
     * [G7] (were Reserved[0..2], always 0). ConfigGeneration: bumped by every AGG_SET_CONFIG, whoever
     * sent it, and different after a driver reload; Apollo Control polls it and re-reads the config
     * when it moved for another reason (docs/apollo-control-units.md 1.4).
     */
    ULONG       ConfigGeneration;
    ULONG       EndpointSet;        /* APOLLO_AGG_EPSET_* the host unit created */
    ULONG       EndpointStreams;    /* combined Windows streams running on the timeline */
    ULONG       Reserved[3];
    APOLLO_AGG_UNIT_STATE Unit[APOLLO_AGG_MAX_UNITS];
} APOLLO_AGG_STATE;

/* AGG_GET_STATE input (optional): bit 0 = reset the peaks and counters after the read */
#define APOLLO_AGG_RESET            0x1

/*
 * AGG_CLOCK_PROBE (admin): the Thunderbolt peer-clock experiment (spec 6.3.7) on the handle's own
 * unit. Writes AX_CONTROL (if AxControl is not 0), then the clock-change primitive 0xC074 =
 * (rate index << 8) | Source with doorbell AX_CONNECT 4, then polls 0xC07C up to WaitMs while
 * acknowledging the notification status. Source 0x0A (peer) or 0x0C (internal) only; AxControl 0,
 * 0xF or 0x20F only. No P2P register is written. Every write is logged.
 */
typedef struct _APOLLO_AGG_PROBE_IN {
    ULONG       Version;            /* APOLLO_AGG_VERSION */
    ULONG       Source;             /* 0x0A or 0x0C */
    ULONG       AxControl;          /* 0 = leave, 0xF, 0x20F */
    ULONG       WaitMs;             /* <= 3000 */
} APOLLO_AGG_PROBE_IN;

typedef struct _APOLLO_AGG_PROBE_OUT {
    ULONG       Version;
    ULONG       AckMs;              /* ~0 = 0xC07C never showed the rate locked */
    ULONG       ClockCfgBefore, ClockCfgAfter;      /* 0xC074 */
    ULONG       RateInfoBefore, RateInfoAfter;      /* 0xC07C */
    ULONG       ClockInfo;          /* 0xC084 */
    ULONG       ClockSrcs;          /* 0xC08C */
    ULONG       AxControlBefore, AxControlAfter;
    ULONG       NotifSeen;          /* OR of 0xC030 while polling */
    ULONG       P2pRoute;           /* 0x224C */
    ULONG       P2pEnable;          /* 0x22C4 */
    ULONG       P2pSlot[4];         /* 0x2280..0x228C */
    ULONG       PeerLatency[3];     /* 0x22C8..0x22D0 */
    ULONG       FramesIn100ms;      /* SAMPLE_POS advance over 100 ms after the poll */
    ULONG       Reserved[4];
} APOLLO_AGG_PROBE_OUT;

/*
 * Combined ASIO client (apollo_asio.h codes). ASIO_OPEN with this input (Version 3) opens the
 * aggregate's client on any member's handle; In[]/Out[] are logical channel numbers of the layout
 * whose LayoutVersion the caller read. The shared block is APOLLO_ASIO_SHARED with Version 3;
 * Reserved[0] = LayoutVersion, Reserved[1] = TimelineEpoch (the client asks its host for a reset
 * when either changes), Reserved[2] = APOLLO_AGG_SHARED_* flags.
 */
#define APOLLO_ASIO_VERSION_AGG     3
typedef struct _APOLLO_ASIO_OPEN_AGG_IN {
    ULONG       Version;            /* APOLLO_ASIO_VERSION_AGG */
    ULONG       BufferFrames;
    ULONG       InCount;
    ULONG       OutCount;
    ULONG       LayoutVersion;
    ULONG       Reserved;
    USHORT      In[APOLLO_AGG_MAX_CH];
    USHORT      Out[APOLLO_AGG_MAX_CH];
    ULONGLONG   EventHandle;
} APOLLO_ASIO_OPEN_AGG_IN, *PAPOLLO_ASIO_OPEN_AGG_IN;

#define APOLLO_AGG_SHARED_STALE     0x1     /* the layout changed under the client: silence until reset */

/* APOLLO_INFO.AggRole (APOLLO_AGG_ROLE_STANDALONE = 0 is in apollo_ioctl.h) */
#define APOLLO_AGG_ROLE_MASTER          1   /* configured master and the timeline */
#define APOLLO_AGG_ROLE_FOLLOWER        2   /* resampled to the timeline */
#define APOLLO_AGG_ROLE_ACTING_MASTER   3   /* the timeline while the configured master is absent */
