/*
 * apollo.h - device context and internal interfaces of apollo.sys.
 *
 * Locking:
 *   DevLock (WDFWAITLOCK, PASSIVE_LEVEL) serialises every multi-register
 *   sequence: bring-up, transport, ring sends, mixer flush, IOCTLs, and each
 *   service-thread pass. Nothing that holds it may run above PASSIVE_LEVEL.
 *   The interrupt lock protects ImrLo/ImrHi and the IMR/ISR registers.
 *   Shutdown is a lock-free gate checked by every MMIO access.
 *   EngineLock (KSPIN_LOCK) protects the copy engine's position snapshot,
 *   heads, stats and the ASIO client's engine fields (engine.c, asio.c). The
 *   1 ms engine tick runs at DISPATCH_LEVEL and never takes DevLock or
 *   StripLock. Order: StripLock -> DevLock -> EngineLock; AsioLock ->
 *   EngineLock. AsioLock is never held together with DevLock or StripLock.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <ntddk.h>
#include <wdf.h>
#include "regs.h"
#include "..\include\apollo_ioctl.h"
#include "..\include\apollo_asio.h"
#include "apollo_sat.h"         /* ApolloSatAdd32: ring clients ADD with saturation */
#include "apollo_model.h"       /* model identification (spec-twinx 1) */
#include "apollo_unit.h"        /* per-unit Parameters (spec-twinx 6.9) */
#include "ring_map.h"           /* audio ring pages: SG entry i <-> linear view page i */
#include "monitor_plan.h"       /* [Monitor] ALT, sources, cue mixes per model; the cue send cache */
#include "rate_plan.h"          /* [Rate] the six rates: FRAME_CFG band, lead/guard scaling, clock-change constants */
#include "..\include\apollo_rate.h"   /* [Rate] GET_RATE / SET_RATE */
#include "strip_record.h"       /* [Warm] the warm-start adoption record (strip_adopt.h, docs/STRIPS.md 12) */
#include "clock_plan.h"         /* [Clock] clock sources, digital formats, the front panel per model */
#include "..\include\apollo_clock.h"  /* [Clock] GET/SET_CLOCK, GET/SET_PANEL */
#include "..\include\apollo_events.h" /* [Events] WAIT_EVENTS and the event kinds */
#include "dsp_status.h"                /* [DSP] reply status words by name, for the log */

#define APOLLO_POOL_TAG     'lopA'
#define APOLLO_MAX_DSPS     8

/*
 * log.c: DbgPrintEx plus a 256 KiB in-memory ring that apollo-diag reads. One ring for every
 * device on the driver: ApolloLogU(Ctx, ...) tags the line with that unit's LogTag ("[x8p]",
 * "[twx]", apollo_ioctl.h GET_LOG); ApolloLog(...) is for lines about the driver as a whole.
 */
VOID  ApolloLogInit(VOID);
VOID  ApolloLogPrint(_In_opt_z_ PCSTR Tag, _In_z_ _Printf_format_string_ PCSTR Fmt, ...);
ULONG ApolloLogSnapshot(_Out_writes_bytes_(OutLen) PCHAR Out, _In_ ULONG OutLen,
                        _Out_ PULONG TotalWritten);
#define ApolloLog(...)          ApolloLogPrint(NULL, __VA_ARGS__)
#define ApolloLogU(C, ...)      ApolloLogPrint((C)->LogTag, __VA_ARGS__)

/*
 * [Clock] One unit's front panel as panel.c tracks it from the readback (DevLock: the service pass writes it, GET_PANEL
 * reads it). Pend*: what the monitor link still has to bring to the monitor unit, written by the pass and used up by
 * the same service thread after it (panel.c ApolloPanelServiceAfter); LinkLevel: as the monitor unit, the level the
 * link last wrote, better than the readback until LinkLevelUntil (the readback lags a write by a pass or two).
 */
typedef struct _APOLLO_PANEL_RT {
    BOOLEAN             Valid;              /* State holds a decoded readback of this connection */
    BOOLEAN             TalkDimmed;         /* the link dimmed this unit for talkback */
    BOOLEAN             UnitDims;           /* seen: the unit dims itself when talkback goes on */
    ULONG               RbSeq;              /* ReadbackSeq last decoded */
    APOLLO_PANEL_STATE  State;
    APOLLO_PANEL_EXPECT Expect[APOLLO_PANEL_FIELDS];
    ULONG               NextOrigin;         /* origin of the write under way (panel.c), 0 = the host */
    ULONG               Seq;                /* events since the driver loaded */
    ULONG               Count;              /* events in Ring (at most APOLLO_PANEL_EVENTS) */
    ULONG               Head;               /* Ring index of the next event */
    APOLLO_PANEL_EVENT  Ring[APOLLO_PANEL_EVENTS];
    ULONGLONG           T0;                 /* KeQueryInterruptTime at the connect */
    ULONG               TalkOnMs;           /* ms (since T0) talkback last went on or off, 0 = not yet */
    LONG                PendLevel;          /* knob steps for the monitor unit */
    ULONG               PendMask;           /* 1 << APOLLO_PANEL_* to bring over */
    ULONG               PendMute, PendDim, PendMono, PendTalk;
    ULONG               LinkLevel;
    ULONGLONG           LinkLevelUntil;
} APOLLO_PANEL_RT;

typedef struct _APOLLO_RING {
    WDFCOMMONBUFFER Buffer;
    PUCHAR          Va;             /* 4 consecutive 4 KiB pages */
    ULONGLONG       La;             /* device-visible (logical) address */
    ULONG           Base;           /* BAR0 offset of the ring registers */
    ULONG           WritePtr;       /* host producer index */
    ULONG           Position;       /* last valid FPGA consumer index */
    ULONG           Gen;            /* bumped by every resync: queued entries may be dropped */
    BOOLEAN         Allocated;
    BOOLEAN         Live;
} APOLLO_RING, *PAPOLLO_RING;

/*
 * Input strips (strips.c, docs/STRIPS.md section 3): one transaction = command-ring entries,
 * their DMA payload (page 0) and reply descriptors (page 1) in one preallocated common buffer.
 * Slots are touched only under DevLock; waits happen with DevLock released.
 */
/* [Virtual] strips.c: 0-7 Mic, 8-15 VIRTUAL 1-8; [AuxStrips] 16-19 the aux returns 0x10-0x13; [TalkStrip] 20 the talkback 0x0A */
#define APOLLO_STRIP_SLOTS  (APOLLO_STRIP_INPUTS + APOLLO_STRIP_VIRTUALS + APOLLO_STRIP_AUXES + APOLLO_STRIP_TALKBACKS)
#define XACT_SLOTS          25      /* [TalkStrip] a pass holds 21 PREPAREs (8 Mic + 8 Virtual + 4 aux + talkback) and its
                                       START, with 3 spare as before */
#define XACT_PAGE           0x1000u
#define XACT_BYTES          (2 * XACT_PAGE)
#define XACT_ENTS_MAX       84      /* [TalkStrip] the START of 8 Mic + 13 Input Strips: 4 entries each */
#define XACT_RX_MAX         2

typedef enum _XACT_STATE {
    XactFree = 0,
    XactBuilding,           /* ours to fill; the device never saw it */
    XactPublished,          /* doorbelled: the device may read/write it */
    XactRetired,            /* both rings provably past it: replies valid */
    XactParked,             /* published, outcome unproven: never reused before ReleaseHardware */
} XACT_STATE;

typedef struct _XACT_REF {
    BOOLEAN         Pending;
    ULONG           Gen;
    ULONG           End;            /* producer index just past our entries */
} XACT_REF;

typedef struct _APOLLO_XACT {
    WDFCOMMONBUFFER Buffer;
    PUCHAR          Va;
    ULONGLONG       La;
    XACT_STATE      State;
    ULONG           Dsp;
    ULONG           Epoch;
    NTSTATUS        Err;            /* sticky builder error */
    ULONG           N;
    ULONG           Ent[XACT_ENTS_MAX][4];  /* DMA refs: Ent[k][2] = payload offset */
    ULONG           Len;            /* payload bytes used in page 0 */
    ULONG           NRx;
    ULONG           RxWords[XACT_RX_MAX];
    ULONG           RxOff[XACT_RX_MAX];     /* bytes into page 1 */
    XACT_REF        Ref[2];         /* [0] command ring, [1] response ring */
    ULONGLONG       PublishedAt;    /* KeQueryInterruptTime */
} APOLLO_XACT, *PAPOLLO_XACT;

/* [Warm] APOLLO_STRIPS.WarmUploads: uploads to the strip DSP at a warm start (docs/STRIPS.md 12.5) */
#define STRIPS_WARM_UPLOADS_NA          0u  /* a cold start, or no plug-ins at this warm start */
#define STRIPS_WARM_UPLOADS_UNPROVEN    1u  /* adopted without a ring connect: no upload acknowledged yet in this session */
#define STRIPS_WARM_UPLOADS_PROVEN      2u  /* the strip DSP acknowledged an upload at this warm start */
#define STRIPS_WARM_UPLOADS_FAILED      3u  /* the first one was not acknowledged: quarantined until a power cycle */

typedef struct _APOLLO_STRIPS {
    /* Registry (PrepareHardware) */
    ULONG           Enable;         /* StripsEnable: 0 off, 1 build, 2 dry run */
    ULONG           Steps;          /* StripSteps: last stage of a pass (strip_plan.h STRIP_STEP_*) */
    ULONG           Mask;           /* StripMask */
    ULONG           Dsp;            /* StripDsp, 0..3 */
    ULONG           MixerBatch;     /* MixerInitBatch: APOLLO_MIXER_BATCH_* */
    ULONG           Probe;          /* StripProbe: strip_plan.h STRIP_PROBE_OFF/MEASURE_ONLY/REPAIR */
    ULONG           VMask;          /* [Virtual] StripVirtualMask as the model and the pools take it (bit n-1 = VIRTUAL n) */
    ULONG           VMaskAsked;     /* [Virtual] the registry value */
    ULONG           VLayoutWhy;     /* [Virtual] 0, or the pool check's code that refused VMaskAsked (VMask is 0 then) */
    ULONG           AMask;          /* [AuxStrips] StripAuxMask as the model and the pools take it (bit k = DSP input 0x10+k) */
    ULONG           AMaskAsked;     /* [AuxStrips] the registry value */
    ULONG           ALayoutWhy;     /* [AuxStrips] 0, or the pool check's code that refused AMaskAsked (AMask is 0 then) */
    ULONG           TMask;          /* [TalkStrip] StripTalkback as the model and the pools take it (1: the strip on DSP input 0x0A) */
    ULONG           TMaskAsked;     /* [TalkStrip] the registry value */
    ULONG           TLayoutWhy;     /* [TalkStrip] 0, or the pool check's code that refused TMaskAsked (TMask is 0 then) */

    /* State (DevLock) */
    ULONG           State;          /* APOLLO_STRIPS_STATE_* */
    CHAR            Why[APOLLO_STRIPS_WHY_CHARS];
    BOOLEAN         Owned;          /* strip DSP: (boot & ~0xFF00) == 3 at a cold bring-up */
    BOOLEAN         Quarantined;
    BOOLEAN         Tainted;
    BOOLEAN         BootWordsRead;
    BOOLEAN         LayoutRead;
    BOOLEAN         ModulesOk;
    BOOLEAN         PoolReady;
    BOOLEAN         ReplyWaiting;   /* a published reply descriptor is outstanding */
    BOOLEAN         WorkQueued;
    BOOLEAN         RateWaitLogged;
    ULONG           Epoch;          /* bumped at every bring-up: older transactions are stale */
    ULONG           Gen;            /* bumped by every teardown or taint: a running pass stops */
    ULONGLONG       ArmedAt;        /* KeQueryInterruptTime */
    ULONG           BootWord[APOLLO_MAX_DSPS];
    ULONG           Geom[11];       /* size 0-3, base 0-3, scratch 1-3 */
    ULONG           LayoutWhy;
    UCHAR           ModState[APOLLO_STRIP_MODULE_SLOTS];    /* APOLLO_STRIP_MOD_* */
    NTSTATUS        ModFile[APOLLO_STRIP_MODULE_SLOTS];
    ULONG           ModReply[APOLLO_STRIP_MODULE_SLOTS][4];
    ULONG           ModOffset[APOLLO_STRIP_MODULE_SLOTS];   /* into Blob */
    ULONG           Passes, Uploads, RepliesOk, RepliesBad, Timeouts, Teardowns;
    ULONG           LastCmdEnd, LastCmdPos, LastRespEnd, LastRespPos;
    /* The strip stages (strips.c, StripSteps >= 2) */
    ULONG           StepsDone;      /* last stage the last pass completed */
    BOOLEAN         PhantomPending; /* cold start: 48V not re-applied yet */
    ULONG           PhantomState;   /* APOLLO_STRIPS_PHANTOM_* */
    ULONG           PhantomMask;
    ULONG           RoutesSent;
    /*
     * Per strip slot (strip_plan.h): 0-7 the Mic strips (preamp i, bus i), [Virtual] 8-15 the Input Strips on
     * VIRTUAL 1-8 (bus 0x18-0x1F), [AuxStrips] 16-19 the Input Strips on the aux returns (bus 0x10-0x13), [TalkStrip] 20
     * the Input Strip on the talkback mic (bus 0x0A). GET_STRIPS reports 0-7 as before, 8-15 in its version 4 part, 16-19
     * in its version 5 part, 20 in its version 6 part.
     */
    UCHAR           StripState[APOLLO_STRIP_SLOTS];     /* APOLLO_STRIP_NONE/QUEUED/TAKEN */
    BOOLEAN         StripStarted[APOLLO_STRIP_SLOTS];   /* its SYNCH went out: stop it at D0 exit */
    ULONG           StripHeader[APOLLO_STRIP_SLOTS];
    ULONG           StripAckMs[APOLLO_STRIP_SLOTS];
    ULONG           BusOwner[APOLLO_STRIP_SLOTS];       /* DSP that mixes slot k's bus (0 = default) */
    /* The live probe (strips.c StripsProbe, docs/STRIPS.md 10.9) */
    ULONG           ProbeDead;      /* strip inputs whose record slot read exact zero, last measurement */
    ULONG           ProbeMeasured;  /* measurements made since the bring-up */
    ULONG           ProbeRepairs;   /* repairs sent since the bring-up */
    /* [Warm] warm starts: adopting this driver's own strips (strip_adopt.h, docs/STRIPS.md 12) */
    ULONG           AdoptParam;     /* StripAdopt: keep Live strips at the D0 exit, adopt them warm */
    ULONG           WarmRebuild;    /* StripWarmRebuild: build again at a warm start that proves the DSP ours */
    BOOLEAN         AdoptPending;   /* warm start: the pass decides (adopt, rebuild or off) */
    BOOLEAN         Adopted;        /* these strips were adopted at this start: nothing was built */
    BOOLEAN         RebuiltWarm;    /* these strips were built at a warm start */
    BOOLEAN         TeardownFirst;  /* the rebuild stops the recorded strips before it writes memory */
    BOOLEAN         PlugAdoptOk;    /* the plug-in host may place on the adopted strip DSP */
    ULONG           WarmPlugins;    /* StripWarmPlugins: plug-ins on adopted (or warm-rebuilt) strips */
    ULONG           WarmUploads;    /* STRIPS_WARM_UPLOADS_*: what this warm start knows of uploads to its strip DSP */
    BOOLEAN         KeptAtExit;     /* the last D0 exit left the strips running */
    BOOLEAN         RecFound;       /* Rec = the record found at this start (valid, this unit) */
    BOOLEAN         RecHave;        /* Cur = what the registry holds (this start wrote or found it) */
    ULONG           RecWhy;         /* strip_adopt.h STRIP_ANCHOR_* of the record found at this start */
    ULONG           Anchor;         /* STRIP_ANCHOR_* after this start's bring-up */
    ULONG           AdoptVerdict;   /* STRIP_ADOPT_* */
    ULONG           AdoptWaits;     /* settle periods the warm decision waited for a valid readback */
    NTSTATUS        RecStatus;      /* the last record read or write */
    ULONG           PostBoot[APOLLO_MAX_DSPS];      /* boot words after this start's bring-up */
    STRIP_RECORD    Rec;            /* found at this start */
    STRIP_RECORD    Cur;            /* kept by this start */
    ULONG           PlanScratch[700];                 /* strip_plan.h STRIP_PLAN (84 entries: 697 words), under StripLock */
    UCHAR           Blob[6336];     /* verified module records (APOLLO_STRIP_MODULES_BYTES) */
    APOLLO_XACT     Xact[XACT_SLOTS];
} APOLLO_STRIPS;

/*
 * One audio ring (audio.c ApolloAudioRingAlloc, ring_map.h): AUDIO_RING_BYTES the FPGA reads
 * through its SG table. The memory is common buffers of the DMA enabler's adapter: one contiguous
 * 4 MiB, or one per SG page when no contiguous 4 MiB below 4 GiB is left. Every consumer sees the
 * same two things either way: Va, one linear kernel view of all 4 MiB, and PageLa[i], the device
 * (logical) address of SG page i. Alloc owns the memory, the view and PageLa; it is a WDF object
 * under the DMA enabler, so its cleanup frees all of it exactly once, before the enabler lets its
 * adapter go (ApolloAudioFree deletes it at ReleaseHardware).
 */
typedef struct _APOLLO_AUDIO_RING {
    WDFOBJECT       Alloc;          /* audio.c APOLLO_RING_ALLOC; NULL = no ring */
    PUCHAR          Va;             /* linear 4 MiB kernel view */
    const ULONGLONG *PageLa;        /* [AUDIO_SG_ENTRIES] device (logical) address of SG page i */
} APOLLO_AUDIO_RING;

/*
 * One ASIO client (asio.c, docs/ASIO.md). The PASSIVE fields change only under
 * AsioLock; Open/Active and the engine fields only under EngineLock as well,
 * so the tick sees a consistent client.
 */
typedef struct _APOLLO_ASIO {
    /* AsioLock (and EngineLock for Open) */
    BOOLEAN         Open;
    WDFFILEOBJECT   OwnerFile;
    PEPROCESS       OwnerProcess;   /* referenced */
    PVOID           Section;        /* referenced section object (pagefile-backed) */
    PVOID           SysView;        /* MmMapViewInSystemSpace view of it */
    PMDL            Mdl;            /* locks SysView's pages */
    PUCHAR          SysVa;          /* non-pageable system mapping of Mdl: all the engine touches */
    PVOID           UserVa;         /* view in OwnerProcess (holds its own section reference) */
    ULONG           Bytes;
    PKEVENT         Event;          /* referenced */
    PAPOLLO_ASIO_SHARED Shared;     /* = SysVa (first page) */
    PLONG           In;             /* In[2][InCount][N] */
    PLONG           Out;            /* Out[2][OutCount][N] */
    ULONG           N;              /* frames per half (power of two) */
    ULONG           Shift;          /* log2(N) */
    ULONG           InCount;
    ULONG           OutCount;
    UCHAR           InSlots[APOLLO_ASIO_MAX_IN];
    UCHAR           OutSlots[APOLLO_ASIO_MAX_OUT];

    /* EngineLock */
    BOOLEAN         Active;         /* ASIO_START .. ASIO_STOP */
    BOOLEAN         NeedSync;       /* re-originate at the next tick */
    BOOLEAN         RenderKnown;    /* RenderPeriod/RenderPlay valid */
    BOOLEAN         RenderPlay;     /* the client wrote RenderPeriod's half in time */
    ULONGLONG       Origin;         /* hardware frame of ASIO frame 0 (capture side) */
    ULONGLONG       Delay;          /* render frame a plays at Origin + a + Delay */
    ULONGLONG       CapNext;        /* next ASIO frame to capture */
    ULONGLONG       RenderPeriod;
    ULONGLONG       PubPeriod[2];   /* period last published in half h, ~0 = none */
    LONG            PubSwitch[2];   /* its SwitchCount */
    ULONGLONG       SampleOffset;   /* keeps SamplePosition monotonic across re-originations */
    ULONGLONG       NextSample;     /* SamplePosition of the next period */
    LONG            Switches;
    LONG            Underruns;
    LONG            Overruns;
    ULONG           Resyncs;
} APOLLO_ASIO, *PAPOLLO_ASIO;

/*
 * Per-handle context (ioctl.c). Only handles opened through our own interface
 * have one: ACX-claimed handles may not, so every user checks for NULL.
 */
typedef struct _APOLLO_FILE {
    BOOLEAN         IsAdmin;        /* the opener's token decides the admin IOCTLs */
    BOOLEAN         CleanedUp;      /* AsioLock: EvtFileCleanup ran, no new ASIO client */
    PEPROCESS       Creator;        /* referenced; the only process that may use ASIO here */
} APOLLO_FILE, *PAPOLLO_FILE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(APOLLO_FILE, ApolloGetFile)

typedef struct _DEVICE_CONTEXT {
    WDFDEVICE       Device;
    /*
     * Set once at device add from the PCI hardware ID (driver.c), before any log line
     * of this unit: ApolloLogU reads LogTag without a lock. Every other field below is this
     * device's own: no device ever reaches another device's context (two units = two contexts,
     * two engines, two ASIO clients; only log.c's ring is shared, under its spin lock).
     */
    CHAR            LogTag[8];
    BOOLEAN         HwidSubsysKnown;
    ULONG           HwidSubsystem;
    WDFINTERRUPT    Interrupt;
    WDFDMAENABLER   DmaEnabler;
    WDFWAITLOCK     DevLock;
    WDFWORKITEM     BringupWork;
    WDFWAITLOCK     StripLock;      /* strip passes and their quiesce; taken BEFORE DevLock */
    WDFWORKITEM     StripWork;      /* the strip pass (strips.c) */
    BUS_INTERFACE_STANDARD BusInterface;
    BOOLEAN         HaveBusInterface;

    /* BAR0 */
    PUCHAR          Bar0;
    ULONG           Bar0Length;
    volatile LONG   Shutdown;       /* MMIO gate: 1 = no bus access */
    volatile LONG   StopRequested;  /* abort long bring-up waits (D0Exit, removal) */
    BOOLEAN         Msi;

    /* Identity */
    ULONG           FpgaRev;
    ULONG           ExtCaps;
    ULONG           FwRelease;
    ULONG           SubsystemId;
    ULONG           DeviceType;
    ULONG           PublicType;
    ULONG           NumDsps;
    BOOLEAN         FwV2;
    BOOLEAN         Irq64;
    BOOLEAN         FwUntested;     /* FwRelease/FpgaRev are not a tested pair for the model */

    CHAR            Serial[17];     /* printable part only (apollo_model.h ApolloSerialClean) */

    /* model and unit (hw.c ApolloIdentify, unit.c ApolloUnitResolve; PrepareHardware) */
    ULONG           HwUidLo;        /* BAR0 0x30 */
    ULONG           HwUidHi;        /* BAR0 0x34 */
    APOLLO_IDENT    Ident;          /* verdict, model, unit key */
    APOLLO_UNIT     Unit;           /* this start's Parameters: read them with ApolloUnitParam */

    /* What the previous owner left (hw.c ApolloReadOwnerState; observe/refused only) */
    BOOLEAN         OwnerRead;
    ULONG           OwnerRateInfo;
    ULONG           OwnerClockSrcs;
    ULONG           OwnerDescInVersion;
    ULONG           OwnerDescInCount;
    ULONG           OwnerDescOutVersion;
    ULONG           OwnerDescOutCount;

    /* Interrupt mask caches (interrupt lock) */
    ULONG           ImrLo;
    ULONG           ImrHi;
    volatile LONG   InterruptConnected; /* EvtInterruptEnable .. Disable */
    volatile LONG   InterruptCount;
    volatile LONG   EndOfBufferCount;

    /* DSP command / response rings */
    APOLLO_RING     Cmd[APOLLO_MAX_DSPS];
    APOLLO_RING     Resp[APOLLO_MAX_DSPS];
    ULONG           DmaCtrlCache;

    /* Audio rings */
    APOLLO_AUDIO_RING Play;
    APOLLO_AUDIO_RING Rec;
    ULONG           PlayChannels;
    ULONG           RecChannels;
    ULONG           RingFrames;
    ULONG           SampleRate;
    ULONG           RateIndex;
    ULONG           DspToldRate;    /* last rate ack sent to the DSPs */
    /*
     * [Rate] What follows SampleRate (rate.c ApolloRateDerive; DevLock, and only while the engine is
     * stopped), the Hi-band dwell, and the last change for GET_RATE. RateChanging: set and cleared
     * under AsioLock (ASIO_OPEN / ASIO_START refuse while it is set), read anywhere.
     */
    ULONG           FrameCfg;       /* AX_FRAME_CFG at SampleRate: 8 / 16 / 32 */
    ULONG           AdcFrames;      /* the ADC delay at SampleRate */
    ULONG           DacFrames;      /* [Lat] the DAC delay at SampleRate (plan DacCenti, rounded) */
    /*
     * [Lat] The reported latencies beyond the copy engine (rate_plan.h ApolloLatencyInPath; docs/ASIO.md, Latency):
     * LatInPath is recomputed by the 10 ms service pass (DevLock) and published to the unit's ASIO block and the
     * combined device when it changes. LatChainExcess: the worst routed chain's record path over the plain strip's,
     * written by plugin.c (StripLock and DevLock) for the strips' epoch LatChainEpoch.
     */
    ULONG           LatInPath;
    ULONG           LatPublishedIn, LatPublishedOut;    /* what the last publication used (input path, DAC) */
    BOOLEAN         LatPublished;
    volatile LONG   LatChainExcess;
    volatile LONG   LatChainEpoch;
    ULONG           HiClockIdx;     /* the clock word was last written at 176.4/192 kHz (5/6), 0 = not */
    ULONGLONG       HiClockSince;   /* KeQueryInterruptTime of the first such write */
    volatile LONG   RateChanging;
    ULONG           RateChanges;
    ULONG           RateLastMs;
    ULONG           RateResult;     /* APOLLO_RATE_ERR_* of the last change */
    NTSTATUS        RateStatus;
    BOOLEAN         RateStripsRebuilt;
    BOOLEAN         RateStoredFlag;
    CHAR            RateWhy[APOLLO_RATE_WHY_CHARS];

    /* State */
    APOLLO_STAGE    Stage;
    APOLLO_STAGE    FailedStage;
    NTSTATUS        LastStatus;
    BOOLEAN         WarmStart;
    BOOLEAN         RingsLive;
    BOOLEAN         DspsConnected;
    BOOLEAN         AcefaceDone;
    BOOLEAN         Connected;
    BOOLEAN         TransportRunning;
    BOOLEAN         TimerResolutionSet;

    /* Connect snapshot (diagnostics) */
    ULONG           IoDescIn[IO_DESC_WORDS];
    ULONG           IoDescOut[IO_DESC_WORDS];
    ULONG           NotifRateInfo;
    ULONG           NotifClockSrcs;
    ULONG           NotifStatusSeen;

    /*
     * The AudioExtension bring-up (bringup_plan.h, docs/spec-twinx.md 6.9.3), DevLock.
     * Zero on the x8p path, which does not record steps. GET_INFO version 7.
     */
    ULONG           NotifClockSrcsLive; /* 0xC08C with our transport running */
    ULONG           BringupSteps;       /* bit APOLLO_BSTEP_* = passed at the last bring-up */
    ULONG           BringupFailStep;    /* APOLLO_BSTEP_* that failed, 0 = none */
    ULONG           BringupApplies;     /* steps this start's path runs (bringup_plan.h ApolloPlanBstepApplies) */
    APOLLO_STAGE    FailAt;             /* stage a failure is reported at, None = Stage + 1 (x8p) */
    ULONG           DspReadyTimeouts;   /* E3: DSPs that never reported ready at the last bring-up */

    /* Position (DevLock) */
    ULONG           LastRawPos;
    ULONGLONG       ExtPos;
    LONGLONG        LastPosQpc;
    LONGLONG        QpcFrequency;
    ULONGLONG       PlayValidUntil; /* client data written up to here */
    ULONGLONG       PlayZeroedUntil;

    /* Mixer mailbox (DevLock) */
    BOOLEAN         MixerReady;
    BOOLEAN         MixerDirty;
    ULONG           MixerSeqWr;
    ULONG           MixerVal[MIXER_NUM_SETTINGS];
    ULONG           MixerMask[MIXER_NUM_SETTINGS];
    ULONGLONG       MixerZeroPending;
    ULONG           Readback[APOLLO_READBACK_WORDS];
    BOOLEAN         ReadbackValid;
    ULONG           ReadbackSeq;

    /* Service thread */
    PKTHREAD        ServiceThread;
    KEVENT          ServiceStop;
    volatile LONG   ServiceRunning;
    ULONG           ServicePasses;

    /* Input strips (strips.c): DevLock unless noted there */
    APOLLO_STRIPS   Strips;
    /* UAD plug-in hosting (plugin.c, docs/PLUGINS.md): StripLock, then DevLock. NULL = none */
    struct _APOLLO_PLUGIN_HOST *Plugin;
    /* [Monitor] the cue sends the host set (cues.c), DevLock; zero = none. [Mix] Cues.Mix: the monitor mix, same rules */
    APOLLO_CUE_STATE Cues;
    /* [Corr] speaker correction (corr.c): set at the first PrepareHardware, freed at device cleanup. NULL = none */
    struct _APOLLO_CORR *Corr;
    /*
     * [Clock] The clock source, the digital formats and the front panel (clock.c, panel.c; driver\clock_plan.h).
     * DevLock for writes (SET_CLOCK, the service pass, the service thread after it); GET_CLOCK reads them lock-free.
     * Zero = what every start had before this code: Internal, nothing chosen, nothing pending.
     */
    BOOLEAN         ClockExt;           /* runs on ClockExtSource; FALSE = Internal (what every transport start writes) */
    BOOLEAN         ClockWantExt;       /* the source asked for is ClockWantSource (external) */
    BOOLEAN         ClockHold;          /* stay on it when it loses lock */
    BOOLEAN         ClockFellBack;      /* it lost lock, or did not come back after a restart: on Internal */
    UCHAR           ClockExtSource;
    UCHAR           ClockWantSource;
    volatile LONG   ClockChanging;      /* a clock write sequence holds DevLock (SET_CLOCK, the service thread) */
    volatile LONG   ClockReapply;       /* a transport start wrote Internal: the service thread puts the source back */
    volatile LONG   ClockFallbackDue;   /* the external source has been unlocked APOLLO_CK_LOSS_MS */
    ULONGLONG       ClockUnlockedSince; /* KeQueryInterruptTime of the first unlocked pass on it, 0 = locked */
    ULONG           ClockPasses;
    ULONG           ClockCfgSeen;       /* 0xC074, every 10th pass */
    ULONG           ClockInfoSeen;      /* 0xC084, every 10th pass */
    ULONG           ClockMilliHz;       /* position rate over the last second, mHz */
    ULONGLONG       ClockMeasPos;
    LONGLONG        ClockMeasQpc;
    ULONG           ClockResult;        /* APOLLO_CLOCK_ERR_* of the last change */
    NTSTATUS        ClockStatus;
    ULONG           ClockLastMs;
    ULONG           ClockChanges;
    ULONG           ClockFallbacks;
    ULONG           ClockLockLosses;
    CHAR            ClockWhy[APOLLO_CLOCK_WHY_CHARS];
    ULONG           DigState;           /* APOLLO_DIG_*: the formats this driver wrote since the connect */
    APOLLO_PANEL_RT Panel;

    /* Copy engine (engine.c). EngineTimer changes under DevLock; the rest is EngineLock. */
    KSPIN_LOCK      EngineLock;
    PEX_TIMER volatile EngineTimer; /* non-NULL while the 1 ms tick is armed */
    ULONG           EngLead;        /* [Rate] RenderLeadFrames at SampleRate (ApolloRateDerive) */
    ULONG           EngGuard;       /* [Rate] CaptureGuardFrames at SampleRate */
    ULONG           EngLead48;      /* [Rate] RenderLeadFrames (registry, frames at 48 kHz) */
    ULONG           EngGuard48;     /* [Rate] CaptureGuardFrames (registry, frames at 48 kHz) */
    ULONG           EngStartAtConnect;  /* StartTransportAtConnect (registry, default 1) */
    ULONG           EngRaw;         /* last AX_SAMPLE_POS read by the tick */
    ULONGLONG       EngPos;         /* 64-bit hardware frame position */
    LONGLONG        EngQpc;         /* QPC right after that read */
    LONGLONG        EngQpcFreq;
    ULONGLONG       EngZeroHead;    /* first playback frame not yet zeroed */
    ULONGLONG       EngDataHead;    /* first playback frame not yet given client data */
    ULONGLONG       EngTicks;
    ULONG64         EngNextDue;         /* interrupt time of the current grid point (EngineRearm) */
    LONGLONG        EngLastTickQpc;
    LONGLONG        EngMaxIntervalQpc;
    LONGLONG        EngMaxDurationQpc;
    ULONG           EngOver2ms;
    ULONG           EngOver4ms;
    ULONG           EngLate;        /* data head fell behind the hardware */
    ULONG           EngBusStalls;   /* SAMPLE_POS read took > 1 ms */
    ULONG           EngDeadReads;   /* SAMPLE_POS read all ones */
    ULONG           EngWrapFixes;   /* wraps added from the QPC estimate */
    ULONG           EngStarts;
    ULONG           EngDeadRun;     /* consecutive all-ones SAMPLE_POS reads */
    ULONG           EngSlowRun;     /* consecutive SAMPLE_POS reads over 1 ms */
    volatile LONG   EngLost;        /* the tick stopped all MMIO (dead link); cleared by a restart */

    /* ASIO (asio.c) */
    WDFWAITLOCK     AsioLock;
    APOLLO_ASIO     Asio;

    /* ACX endpoints (circuit.c, stream.c; WDF context on the device, see apollo_stream.h) */
    struct _APOLLO_ACX *Acx;
    BOOLEAN         AcxRemovedAfterFail;    /* a failed Twin X bring-up removed its circuits */

    /* [Events] events.c: the unit's last events and the parked WAIT_EVENTS (EventLock is the innermost lock) */
    WDFSPINLOCK     EventLock;
    WDFQUEUE        EventQueue;
    ULONG           EventSeq;
    APOLLO_EVENT_ENTRY EventRing[APOLLO_EVENT_RING];
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, ApolloGetContext)

/* The unit's bring-up plan (bringup_plan.h), NULL for a model this driver does not run. */
FORCEINLINE const APOLLO_BRINGUP_PLAN *ApolloPlanOf(_In_ PDEVICE_CONTEXT Ctx)
{
    return ApolloBringupPlanFor(Ctx->Ident.Model);
}

/* The AudioExtension path (Twin X Gen 1): the plan decides; FALSE for the x8p. */
FORCEINLINE BOOLEAN ApolloIsAudioExt(_In_ PDEVICE_CONTEXT Ctx)
{
    const APOLLO_BRINGUP_PLAN *p = ApolloPlanOf(Ctx);

    return (p != NULL && p->AudioExtension) ? TRUE : FALSE;
}

FORCEINLINE VOID ApolloStepPassed(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Step)
{
    if (Step < 32) {
        Ctx->BringupSteps |= 1u << Step;
    }
}

/* ---- hw.c: MMIO, timing, identity, interrupts ---- */

FORCEINLINE BOOLEAN ApolloGateClosed(_In_ PDEVICE_CONTEXT Ctx)
{
    return ReadAcquire(&Ctx->Shutdown) != 0;
}

FORCEINLINE ULONG ApolloRead(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Offset)
{
    if (ApolloGateClosed(Ctx) || Ctx->Bar0 == NULL || Offset + 4 > Ctx->Bar0Length) {
        return 0xFFFFFFFF;
    }
    return READ_REGISTER_ULONG((volatile ULONG *)(Ctx->Bar0 + Offset));
}

FORCEINLINE VOID ApolloWrite(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Offset, _In_ ULONG Value)
{
    if (ApolloGateClosed(Ctx) || Ctx->Bar0 == NULL || Offset + 4 > Ctx->Bar0Length) {
        return;
    }
    WRITE_REGISTER_ULONG((volatile ULONG *)(Ctx->Bar0 + Offset), Value);
}

FORCEINLINE VOID ApolloCloseGate(_In_ PDEVICE_CONTEXT Ctx)
{
    InterlockedExchange(&Ctx->Shutdown, 1);
}

VOID     ApolloSleepMs(_In_ ULONG Ms);
NTSTATUS ApolloIdentify(_In_ PDEVICE_CONTEXT Ctx);
VOID     ApolloReadOwnerState(_In_ PDEVICE_CONTEXT Ctx);  /* reads only, observe units */
VOID     ApolloStopTransportAndIrqs(_In_ PDEVICE_CONTEXT Ctx);
VOID     ApolloVectorEnable(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG ImrHiBit, _In_ BOOLEAN ClearPending);
VOID     ApolloVectorDisable(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG ImrHiBit);
VOID     ApolloMaskAll(_In_ PDEVICE_CONTEXT Ctx, _In_ BOOLEAN ClearPending);
VOID     ApolloClearStalePending(_In_ PDEVICE_CONTEXT Ctx);
VOID     ApolloCheckDsps(_In_ PDEVICE_CONTEXT Ctx, _In_ PCSTR When);
ULONG    ApolloDspWindow(_In_ ULONG Dsp);
ULONG    ApolloDmaEnableBits(_In_ PDEVICE_CONTEXT Ctx);
ULONG    ApolloDmaCtrlClean(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Value);
VOID     ApolloResetDmaAllEngines(_In_ PDEVICE_CONTEXT Ctx);
BOOLEAN  ApolloMixerDspAlive(_In_ PDEVICE_CONTEXT Ctx);
NTSTATUS ApolloWaitDspsReady(_In_ PDEVICE_CONTEXT Ctx);
VOID     ApolloObserveDmaOff(_In_ PDEVICE_CONTEXT Ctx);     /* observe units, plan ObserveDmaOff */
EVT_WDF_INTERRUPT_ISR ApolloEvtInterruptIsr;
EVT_WDF_INTERRUPT_DPC ApolloEvtInterruptDpc;

/* ---- dsp.c: command rings ---- */

NTSTATUS ApolloRingsAllocate(_In_ PDEVICE_CONTEXT Ctx);
VOID     ApolloRingsFree(_In_ PDEVICE_CONTEXT Ctx);
NTSTATUS ApolloRingsNeutralizeAll(_In_ PDEVICE_CONTEXT Ctx);
NTSTATUS ApolloRingsInit(_In_ PDEVICE_CONTEXT Ctx);
NTSTATUS ApolloRingsQuiesce(_In_ PDEVICE_CONTEXT Ctx);
NTSTATUS ApolloDspConnectAll(_In_ PDEVICE_CONTEXT Ctx);
/* AudioExtension path: page addresses again (E7), and the ring connect that must be consumed (G5) */
NTSTATUS ApolloRingsReprogram(_In_ PDEVICE_CONTEXT Ctx);
NTSTATUS ApolloDspConnectChecked(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG ConsumeMs);
NTSTATUS ApolloDspRateAck(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG RateHz);
NTSTATUS ApolloDspSendDateOnly(_In_ PDEVICE_CONTEXT Ctx);
/* Ring primitives for strips.c (DevLock held) */
NTSTATUS ApolloRingRefresh(_In_ PDEVICE_CONTEXT Ctx, _Inout_ PAPOLLO_RING Ring);
ULONG    ApolloRingFreeSlots(_In_ const APOLLO_RING *Ring);
ULONG    ApolloRingPending(_In_ const APOLLO_RING *Ring);
VOID     ApolloRingWrite(_Inout_ PAPOLLO_RING Ring, _In_ ULONG W0, _In_ ULONG W1, _In_ ULONG W2,
                         _In_ ULONG W3);
VOID     ApolloRingDoorbell(_In_ PDEVICE_CONTEXT Ctx, _In_ PAPOLLO_RING Ring);
NTSTATUS ApolloRingResyncNow(_In_ PDEVICE_CONTEXT Ctx, _Inout_ PAPOLLO_RING Ring);

/* ---- strips.c: Console input strips (docs/STRIPS.md) ---- */

VOID     ApolloStripsPrepareHardware(_In_ PDEVICE_CONTEXT Ctx);   /* registry, buffers */
VOID     ApolloStripsReleaseHardware(_In_ PDEVICE_CONTEXT Ctx);
VOID     ApolloStripsProbeBoot(_In_ PDEVICE_CONTEXT Ctx);         /* DevLock, before E11 */
VOID     ApolloStripsProbeLayout(_In_ PDEVICE_CONTEXT Ctx);       /* DevLock, after bring-up */
VOID     ApolloStripsArm(_In_ PDEVICE_CONTEXT Ctx);               /* DevLock */
VOID     ApolloStripsService(_In_ PDEVICE_CONTEXT Ctx);           /* DevLock, 10 ms pass */
VOID     ApolloStripsTaint(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Offset); /* DevLock */
VOID     ApolloStripsFillInfo(_In_ PDEVICE_CONTEXT Ctx, _Out_ PAPOLLO_STRIPS_INFO I); /* DevLock */
VOID     ApolloStripsQuiesce(_In_ PDEVICE_CONTEXT Ctx, _In_z_ PCSTR Why); /* no locks held */
/* [Warm] D0 exit: may the strips stay running for the next start (no locks; before the plug-in quiesce)? */
_IRQL_requires_(PASSIVE_LEVEL)
BOOLEAN  ApolloStripsExitKeep(_In_ PDEVICE_CONTEXT Ctx);
/* [Warm] D0 exit, after the plug-in quiesce: leave them running (Keep, re-checked) or tear them down (no locks) */
_IRQL_requires_(PASSIVE_LEVEL)
VOID     ApolloStripsExit(_In_ PDEVICE_CONTEXT Ctx, _In_ BOOLEAN Keep);
/* [Warm] plugin.c, DevLock held: plug-in instances now placed on the strip DSP (the record keeps the count) */
_IRQL_requires_(PASSIVE_LEVEL)
VOID     ApolloStripsRecordPlugins(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Placed);
/* [Warm] plugin.c, DevLock held: the strip DSP's state is unknown from here (quarantine): no adoption until a power cycle */
_IRQL_requires_(PASSIVE_LEVEL)
VOID     ApolloStripsRecordLost(_In_ PDEVICE_CONTEXT Ctx, _In_z_ PCSTR Why);
/*
 * [Warm] plugin.c, DevLock held: the strip DSP acknowledged an upload. At a warm start this proves its session takes
 * uploads (docs/STRIPS.md 12.5); logged once.
 */
_IRQL_requires_(PASSIVE_LEVEL)
VOID     ApolloStripsUploadProven(_In_ PDEVICE_CONTEXT Ctx);
VOID     ApolloStripsMixerInitBatch(_In_ PDEVICE_CONTEXT Ctx);    /* DevLock, ApolloMixerInit */
BOOLEAN  ApolloStripsRateRearm(_In_ PDEVICE_CONTEXT Ctx);         /* [Rate] DevLock, after a rate change */
ULONG    ApolloStripsVirtualBuses(_In_ PDEVICE_CONTEXT Ctx);      /* [Virtual] mixer buses of the Virtual strips (bit 0x18+n-1) */
ULONG    ApolloStripsBusSlot(_In_ ULONG Bus);                     /* [Virtual] cues.c: the strip slot that mixes Bus, or 21 */
ULONG    ApolloStripsAuxReturns(_In_ PDEVICE_CONTEXT Ctx);        /* [AuxStrips] plugin.c: bit a = AUX a+1's return has strips built */
BOOLEAN  ApolloStripsTalkbackLive(_In_ PDEVICE_CONTEXT Ctx);      /* [TalkStrip] panel.c (DevLock): the talkback strip runs */
EVT_WDF_WORKITEM                         ApolloEvtStripWork;

/* ---- plugin.c: UAD plug-in hosting, phase P0 (docs/PLUGINS.md 2.1; include\apollo_plugin.h) ---- */

VOID     ApolloPluginPrepareHardware(_In_ PDEVICE_CONTEXT Ctx);   /* the host struct */
VOID     ApolloPluginReleaseHardware(_In_ PDEVICE_CONTEXT Ctx);   /* gate closed: pages, instances forgotten */
/* no locks; before the strips' quiesce or exit. [Warm] RestoreStrips: SYNCH each retained plain strip again */
VOID     ApolloPluginQuiesce(_In_ PDEVICE_CONTEXT Ctx, _In_z_ PCSTR Why, _In_ BOOLEAN RestoreStrips);
VOID     ApolloPluginFileCleanup(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFFILEOBJECT File); /* no locks */
VOID     ApolloPluginFree(_In_ PDEVICE_CONTEXT Ctx);              /* device cleanup */
BOOLEAN  ApolloPluginIsIoctl(_In_ ULONG Code);
BOOLEAN  ApolloPluginAnalogConflict(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Index, _In_ ULONG Mask); /* [Ins] DevLock: a Unison lease owns it */
NTSTATUS ApolloPluginIoctl(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFREQUEST Request, _In_ ULONG Code,
                           _In_reads_bytes_opt_(InLen) PVOID In, _In_ size_t InLen,
                           _Out_writes_bytes_opt_(OutLen) PVOID Out, _In_ size_t OutLen,
                           _Out_ ULONG_PTR *Info);                  /* no locks held */

/*
 * ---- corr.c: [Corr] speaker correction on the playback ring (include\apollo_corr.h, corr_dsp.h) ----
 * Its PASSIVE lock (Corr->Lock) comes before EngineLock and is never held with DevLock, StripLock or
 * the aggregate's locks. The tick side takes only the unit's EngineLock (or runs under the
 * aggregate's hold of it); with no profile, or the correction off, it returns before touching a frame.
 */
_IRQL_requires_(PASSIVE_LEVEL)
VOID     ApolloCorrPrepareHardware(_In_ PDEVICE_CONTEXT Ctx);  /* after ApolloUnitResolve: state, stored profile */
VOID     ApolloCorrFree(_In_ PDEVICE_CONTEXT Ctx);              /* device cleanup */
_IRQL_requires_(DISPATCH_LEVEL)
VOID     ApolloCorrAfterTick(_In_ PDEVICE_CONTEXT Ctx);         /* engine.c: end of the tick, no lock held */
_IRQL_requires_(DISPATCH_LEVEL)
VOID     ApolloCorrAggFollower(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONGLONG RenderHead); /* agg.c: Ctx's EngineLock held */
_IRQL_requires_(DISPATCH_LEVEL)
ULONGLONG ApolloCorrWriteFloor(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONGLONG Head);       /* agg.c: Ctx's EngineLock held */
BOOLEAN  ApolloCorrIsIoctl(_In_ ULONG Code);
BOOLEAN  ApolloCorrRateMismatch(_In_ PDEVICE_CONTEXT Ctx);        /* [Rate] a profile for another rate is loaded */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS ApolloCorrIoctl(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Code, _In_reads_bytes_opt_(InLen) PVOID In, _In_ size_t InLen,
                         _Out_writes_bytes_opt_(OutLen) PVOID Out, _In_ size_t OutLen, _Out_ ULONG_PTR *Info);

/* ---- audio.c: SG, connect, transport, position, data copy ---- */

NTSTATUS ApolloAudioAllocate(_In_ PDEVICE_CONTEXT Ctx);
VOID     ApolloAudioFree(_In_ PDEVICE_CONTEXT Ctx);
VOID     ApolloAudioProgramSg(_In_ PDEVICE_CONTEXT Ctx);
NTSTATUS ApolloAudioConnect(_In_ PDEVICE_CONTEXT Ctx);
/* AudioExtension path (docs/spec-twinx.md 2): E7-E13, and the way every failure ends */
NTSTATUS ApolloBootTransport(_In_ PDEVICE_CONTEXT Ctx);
NTSTATUS ApolloAudioExtAbort(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Step, _In_ APOLLO_STAGE At,
                             _In_ NTSTATUS Status, _In_z_ PCSTR Hint);
NTSTATUS ApolloTransportStart(_In_ PDEVICE_CONTEXT Ctx);
NTSTATUS ApolloTransportStartEx(_In_ PDEVICE_CONTEXT Ctx, _In_ BOOLEAN Engine);   /* [Rate] FALSE: no copy engine */
VOID     ApolloTransportStop(_In_ PDEVICE_CONTEXT Ctx);
BOOLEAN  ApolloAudioDescriptorsReread(_In_ PDEVICE_CONTEXT Ctx);   /* [Rate] DevLock; TRUE = the layout changed */
VOID     ApolloPositionUpdate(_In_ PDEVICE_CONTEXT Ctx, _Out_opt_ PAPOLLO_POSITION Out);
VOID     ApolloPlaybackZeroAhead(_In_ PDEVICE_CONTEXT Ctx);
VOID     ApolloLevelsRead(_In_ PDEVICE_CONTEXT Ctx, _Out_ PAPOLLO_LEVELS L);
NTSTATUS ApolloCaptureCopy(_In_ PDEVICE_CONTEXT Ctx, _In_ PAPOLLO_CAPTURE_REQUEST Req,
                           _Out_writes_bytes_(OutLen) PVOID Out, _In_ SIZE_T OutLen,
                           _Out_ SIZE_T *Written);
NTSTATUS ApolloPlaybackCopy(_In_ PDEVICE_CONTEXT Ctx, _In_reads_bytes_(InLen) PVOID In,
                            _In_ SIZE_T InLen, _Out_ PAPOLLO_PLAYBACK_RESULT Result);
VOID     ApolloHwQuiesce(_In_ PDEVICE_CONTEXT Ctx);

/* ---- engine.c: 1 ms copy engine (docs/PHASE2.md section 5) ---- */

/* The tick keeps the playback ring zeroed this far ahead of SAMPLE_POS (half the ring). */
#define APOLLO_ZERO_AHEAD_FRAMES    4096u

VOID     ApolloEngineReadConfig(_In_ PDEVICE_CONTEXT Ctx);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS ApolloEngineStart(_In_ PDEVICE_CONTEXT Ctx);      /* DevLock, transport just started */
_IRQL_requires_(PASSIVE_LEVEL)
VOID     ApolloEngineStop(_In_ PDEVICE_CONTEXT Ctx);       /* never with EngineLock held */
BOOLEAN  ApolloEngineRunning(_In_ PDEVICE_CONTEXT Ctx);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS ApolloEngineRecover(_In_ PDEVICE_CONTEXT Ctx);    /* DevLock; TRANSPORT_START when lost */
FORCEINLINE BOOLEAN ApolloEngineLost(_In_ PDEVICE_CONTEXT Ctx)
{
    return ReadNoFence(&Ctx->EngLost) != 0;
}
VOID     ApolloPositionGet(_In_ PDEVICE_CONTEXT Ctx, _Out_ PULONGLONG Pos, _Out_ PLONGLONG Qpc,
                           _Out_ PULONG Raw);             /* DevLock */
VOID     ApolloEngineFillStats(_In_ PDEVICE_CONTEXT Ctx, _Out_ PAPOLLO_ENGINE_STATS S,
                               _In_ BOOLEAN Reset);
FORCEINLINE ULONGLONG ApolloPlayValidUntil(_In_ PDEVICE_CONTEXT Ctx)
{
    return (ULONGLONG)ReadNoFence64((volatile LONG64 *)&Ctx->PlayValidUntil);
}

/* ---- asio.c: the kernel half of ASIO (docs/ASIO.md) ---- */

EVT_WDF_IO_IN_CALLER_CONTEXT             ApolloEvtIoInCallerContext;
EVT_WDF_FILE_CLEANUP                     ApolloEvtFileCleanup;
EVT_WDF_FILE_CLOSE                       ApolloEvtFileClose;
_IRQL_requires_(PASSIVE_LEVEL)
VOID     ApolloAsioCloseAny(_In_ PDEVICE_CONTEXT Ctx);     /* device cleanup */
/* Engine hooks, EngineLock held, DISPATCH_LEVEL */
VOID     ApolloAsioEngineStarted(_In_ PDEVICE_CONTEXT Ctx);
VOID     ApolloAsioRender(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONGLONG From, _In_ ULONGLONG To);
VOID     ApolloAsioCapture(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONGLONG Head);
VOID     ApolloAsioRateChanged(_In_ PDEVICE_CONTEXT Ctx);         /* [Rate] AsioLock held: the client's rate, latencies */
/* [Lat] DevLock held (the 10 ms service pass): the reported latencies again, published when they changed */
_IRQL_requires_(PASSIVE_LEVEL)
VOID     ApolloLatencyRefresh(_In_ PDEVICE_CONTEXT Ctx);
/* [Lat] the unit's reported input / output latency without the buffer (engine + ADC + path; engine + FRAME_CFG + DAC) */
ULONG    ApolloLatencyIn(_In_ PDEVICE_CONTEXT Ctx);
ULONG    ApolloLatencyOut(_In_ PDEVICE_CONTEXT Ctx);

/* ---- circuit.c / stream.c / endpoints.c: ACX endpoints (docs/PHASE2.md steps 2-3) ---- */

/* A packet-complete notification collected under EngineLock, sent after it is released. */
typedef struct _APOLLO_NOTIFY {
    PVOID           Stream;         /* PAPOLLO_STREAM, rundown protection held */
    ULONGLONG       Packet;
} APOLLO_NOTIFY, *PAPOLLO_NOTIFY;
#define APOLLO_NOTIFY_MAX           16

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS ApolloAcxDeviceInit(_In_ PDEVICE_CONTEXT Ctx);    /* EvtDeviceAdd, after AcxDeviceInitialize */
_IRQL_requires_(PASSIVE_LEVEL)
VOID     ApolloAcxPrepareHardware(_In_ PDEVICE_CONTEXT Ctx);   /* adds the circuits */
_IRQL_requires_(PASSIVE_LEVEL)
VOID     ApolloAcxReleaseHardware(_In_ PDEVICE_CONTEXT Ctx);   /* removes them */
BOOLEAN  ApolloStreamsPrepared(_In_ PDEVICE_CONTEXT Ctx);
/* [Endpoints] GET_ENDPOINTS v2: rows FirstRow.. as many as fit; returns the bytes to report (stream.c) */
ULONG    ApolloEndpointsFillInfo(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG FirstRow,
                                 _Out_writes_bytes_(OutBytes) PVOID OutBuf, _In_ SIZE_T OutBytes);
_IRQL_requires_(PASSIVE_LEVEL)
VOID     ApolloAcxSetRate(_In_ PDEVICE_CONTEXT Ctx);   /* [Rate] the circuits' format = SampleRate; no unit lock held */
VOID     ApolloStreamsLayoutChanged(_In_ PDEVICE_CONTEXT Ctx);    /* [Rate] running streams find their slots again */
/* Engine hooks, EngineLock held, DISPATCH_LEVEL */
VOID     ApolloStreamsEngineStarted(_In_ PDEVICE_CONTEXT Ctx);
VOID     ApolloStreamsRender(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONGLONG From, _In_ ULONGLONG To);
VOID     ApolloStreamsCapture(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONGLONG Head);
ULONG    ApolloStreamsCollect(_In_ PDEVICE_CONTEXT Ctx, _Out_writes_to_(Max, return) PAPOLLO_NOTIFY List,
                              _In_ ULONG Max);
/* After EngineLock is released */
VOID     ApolloStreamsNotify(_In_reads_(Count) PAPOLLO_NOTIFY List, _In_ ULONG Count, _In_ LONGLONG Qpc);

/* ---- mixer.c: mailbox, readback, preamps, service thread ---- */

VOID     ApolloMixerInit(_In_ PDEVICE_CONTEXT Ctx);
NTSTATUS ApolloMixerWriteSetting(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Index,
                                 _In_ ULONG Value, _In_ ULONG Mask);
VOID     ApolloMixerFlush(_In_ PDEVICE_CONTEXT Ctx);
NTSTATUS ApolloSetInputParam(_In_ PDEVICE_CONTEXT Ctx, _In_ PAPOLLO_INPUT_PARAM P);
NTSTATUS ApolloSetMonitorParam(_In_ PDEVICE_CONTEXT Ctx, _In_ PAPOLLO_MONITOR_PARAM P);
NTSTATUS ApolloServiceStart(_In_ PDEVICE_CONTEXT Ctx);
VOID     ApolloServiceStop(_In_ PDEVICE_CONTEXT Ctx);
VOID     ApolloServiceStep(_In_ PDEVICE_CONTEXT Ctx);   /* [Rate] one service pass, DevLock held (long waits under it) */

/* ---- cues.c: [Monitor] cue sends as DSP bus coefficients (monitor_plan.h), DevLock held ---- */

NTSTATUS ApolloCuesApply(_In_ PDEVICE_CONTEXT Ctx, _In_ const APOLLO_MON_WRITE *W);
ULONG    ApolloCuesBusMoved(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Bus);    /* strips.c: BusOwner[Bus] changed */
VOID     ApolloCuesReplay(_In_ PDEVICE_CONTEXT Ctx);                      /* ApolloMixerInit, every connect */
ULONG    ApolloCuesAuxOwner(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Aux, _In_ ULONG Dsp); /* [Ins] plugin.c: an aux insert chain (DevLock) */
ULONG    ApolloCuesAuxStrips(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Aux, _In_ BOOLEAN Live, _In_ ULONG Dsp); /* [AuxStrips] strips.c */

/* ---- device.c / ioctl.c ---- */

EVT_WDF_DEVICE_PREPARE_HARDWARE          ApolloEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE          ApolloEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY                  ApolloEvtD0Entry;
EVT_WDF_DEVICE_D0_ENTRY_POST_INTERRUPTS_ENABLED ApolloEvtD0EntryPostInterruptsEnabled;
EVT_WDF_DEVICE_D0_EXIT_PRE_INTERRUPTS_DISABLED ApolloEvtD0ExitPreInterruptsDisabled;
EVT_WDF_DEVICE_D0_EXIT                   ApolloEvtD0Exit;
EVT_WDF_DEVICE_SURPRISE_REMOVAL          ApolloEvtSurpriseRemoval;
EVT_WDF_WORKITEM                         ApolloEvtBringupWork;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL       ApolloEvtIoDeviceControl;
EVT_WDF_DEVICE_FILE_CREATE               ApolloEvtDeviceFileCreate;

VOID ApolloFail(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_STAGE Attempted, _In_ NTSTATUS Status);

/*
 * ---- rate.c: [Rate] the sample rate (include\apollo_rate.h, rate_plan.h) ----
 * ApolloRateChange runs with no lock held and takes AsioLock alone, then StripLock -> DevLock; the
 * combined device calls it with Agg.Mutex held (Agg.Mutex -> AsioLock is the one new edge).
 */
VOID     ApolloRateDerive(_In_ PDEVICE_CONTEXT Ctx);
ULONG    ApolloRateStoredHz(_In_ PDEVICE_CONTEXT Ctx);
VOID     ApolloRateClockWritten(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Index);
VOID     ApolloRateBringup(_In_ PDEVICE_CONTEXT Ctx);                  /* DevLock, before the first transport start */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS ApolloRateStoreKey(_In_z_ PCSTR Key, _In_ ULONG Hz);
_IRQL_requires_(PASSIVE_LEVEL)
ULONG    ApolloRateChange(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Hz, _In_ ULONG Flags, _In_opt_ WDFFILEOBJECT File,
                          _In_ BOOLEAN InAgg);
VOID     ApolloRateFillInfo(_In_ PDEVICE_CONTEXT Ctx, _Out_ PAPOLLO_RATE_INFO I);
BOOLEAN  ApolloRateIsIoctl(_In_ ULONG Code);
_IRQL_requires_(PASSIVE_LEVEL)
VOID     ApolloRateDispatch(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFREQUEST Request);

/*
 * ---- clock.c, panel.c: [Clock] clock source, digital formats, the front panel, the monitor link ----
 * (include\apollo_clock.h, driver\clock_plan.h). The clock write sequence takes DevLock alone (never StripLock);
 * the service thread runs ApolloClockServiceAfter and ApolloPanelServiceAfter with no lock held after each pass:
 * the first takes its own DevLock, the second another unit's DevLock without waiting (a member rundown held).
 */
ULONG    ApolloClockSourceOf(_In_ PDEVICE_CONTEXT Ctx);          /* the source it runs on now */
ULONG    ApolloClockWantedOf(_In_ PDEVICE_CONTEXT Ctx);          /* the source asked for (rates follow it) */
VOID     ApolloClockConnected(_In_ PDEVICE_CONTEXT Ctx);         /* DevLock: every connect (mixer init) */
VOID     ApolloClockTransportStarted(_In_ PDEVICE_CONTEXT Ctx);  /* DevLock: a start wrote the Internal clock word */
VOID     ApolloClockServicePass(_In_ PDEVICE_CONTEXT Ctx);       /* DevLock: every service pass */
_IRQL_requires_(PASSIVE_LEVEL)
VOID     ApolloClockServiceAfter(_In_ PDEVICE_CONTEXT Ctx);      /* service thread, no lock held */
VOID     ApolloClockFillInfo(_In_ PDEVICE_CONTEXT Ctx, _Out_ PAPOLLO_CLOCK_INFO I);  /* lock-free */
_IRQL_requires_(PASSIVE_LEVEL)
VOID     ApolloClockDispatch(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFREQUEST Request);    /* SET_CLOCK, caller's thread */
VOID     ApolloPanelConnected(_In_ PDEVICE_CONTEXT Ctx);         /* DevLock: every connect */
VOID     ApolloPanelNoteWrite(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Param, _In_ LONG Value);   /* DevLock: SET_MONITOR_PARAM */
VOID     ApolloPanelServicePass(_In_ PDEVICE_CONTEXT Ctx);       /* DevLock: after the readback read */
_IRQL_requires_(PASSIVE_LEVEL)
VOID     ApolloPanelServiceAfter(_In_ PDEVICE_CONTEXT Ctx);      /* service thread, no lock held */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS ApolloPanelIoctl(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Code, _In_opt_ PVOID In, _In_ size_t InLen,
                          _Out_writes_bytes_opt_(OutLen) PVOID Out, _In_ size_t OutLen, _Out_ ULONG_PTR *Info);  /* DevLock */

/* [Events] events.c (include\apollo_events.h) */
NTSTATUS ApolloEventsInit(_In_ PDEVICE_CONTEXT Ctx);
BOOLEAN  ApolloEventsIsIoctl(_In_ ULONG Code);
VOID     ApolloEventsIoctl(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFREQUEST Request);       /* completes or parks Request */
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID     ApolloEventPost(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Kind, _In_ ULONG A, _In_ ULONG B);

/* [Params] params.c: the per-unit Parameters at run time (include\apollo_params.h) */
BOOLEAN  ApolloParamsIsIoctl(_In_ ULONG Code);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS ApolloParamsIoctl(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Code, _In_opt_ PVOID In, _In_ size_t InLen,
                           _Out_opt_ PVOID Out, _In_ size_t OutLen, _Out_ PULONG_PTR Info);

/* ---- unit.c: per-unit Parameters (docs/spec-twinx.md 6.9; apollo_unit.h) ---- */

_IRQL_requires_(PASSIVE_LEVEL)
VOID     ApolloUnitResolve(_In_ PDEVICE_CONTEXT Ctx);     /* PrepareHardware, after ApolloIdentify */
/* The value for this unit (APOLLO_PARAM_*); before ApolloUnitResolve: the safe default. */
ULONG    ApolloUnitParam(_In_ PDEVICE_CONTEXT Ctx, _In_ int Id);
/*
 * Device add only (no BAR0 yet), after driver.c read the hardware ID. Endpoints: every row of the
 * slot's table (apollo_unit.h ApolloAddTimeEndpoints; the unit's value narrows it at PrepareHardware).
 * Other values: on the x8p's subsystem (or an unread hardware ID) the legacy global or the x8p
 * default; on any other subsystem the safe default of a model that is not the x8p.
 */
_IRQL_requires_(PASSIVE_LEVEL)
ULONG    ApolloUnitParamBeforeIdentify(_In_ PDEVICE_CONTEXT Ctx, _In_ int Id);
/* GET_INFO version 6 unit fields (ioctl.c) */
ULONG    ApolloUnitInfoFlags(_In_ PDEVICE_CONTEXT Ctx);

/*
 * ---- agg.c: the combined device (docs/spec-twinx.md 6.2-6.9, gate G6) ----
 * Lock order across devices: Agg.Mutex -> StripLock -> DevLock -> Agg.Lock -> EngineLocks of
 * several units only in ascending HW_UID order. The engine tick calls ApolloAggAfterTick after it
 * released its own EngineLock; Join/Leave run with no unit lock held.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS ApolloAggInit(_In_ WDFDRIVER Driver);          /* DriverEntry */
VOID     ApolloAggUnload(VOID);                         /* EvtDriverUnload */
_IRQL_requires_(PASSIVE_LEVEL)
VOID     ApolloAggJoin(_In_ PDEVICE_CONTEXT Ctx);       /* bring-up done, transport and engine running */
_IRQL_requires_(PASSIVE_LEVEL)
VOID     ApolloAggLeave(_In_ PDEVICE_CONTEXT Ctx);      /* D0 exit, before the rings can go */
_IRQL_requires_(DISPATCH_LEVEL)
VOID     ApolloAggAfterTick(_In_ PDEVICE_CONTEXT Ctx);  /* end of every engine tick, no lock held */
/* [Lat] the unit's device-side latencies changed (asio.c ApolloLatencyRefresh: DevLock held, no EngineLock) */
_IRQL_requires_(PASSIVE_LEVEL)
VOID     ApolloAggLatencyNote(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG InPath, _In_ ULONG Dac);
ULONG    ApolloAggRoleOf(_In_ PDEVICE_CONTEXT Ctx);     /* GET_INFO AggRole */
BOOLEAN  ApolloAggIsIoctl(_In_ ULONG Code);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS ApolloAggIoctl(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Code, _In_opt_ PVOID In, _In_ size_t InLen,
                        _Inout_opt_ PVOID Out, _In_ size_t OutLen, _Out_ ULONG_PTR *Info);
_IRQL_requires_(PASSIVE_LEVEL)
BOOLEAN  ApolloAggAsioClaims(_In_ WDFREQUEST Request, _In_ ULONG Code, _In_opt_ WDFFILEOBJECT File);
_IRQL_requires_(PASSIVE_LEVEL)
VOID     ApolloAggAsioDispatch(_In_ WDFREQUEST Request, _In_ ULONG Code, _In_ WDFFILEOBJECT File,
                               _In_ BOOLEAN CleanedUp);
_IRQL_requires_(PASSIVE_LEVEL)
VOID     ApolloAggAsioFileGone(_In_ WDFFILEOBJECT File);
/*
 * The combined Windows endpoints (spec 6.7): the endpoint set read at device add (circuit.c),
 * the host announcing its combined circuits, and the service-thread hook that writes kept start
 * ratios (mixer.c, after DevLock is released). The stream calls are in apollo_stream.h.
 */
_IRQL_requires_(PASSIVE_LEVEL)
ULONG    ApolloAggReadEndpointSet(_Out_ PULONG Mask);   /* APOLLO_AGG_EPSET_*; Mask = EndpointMask */
_IRQL_requires_(PASSIVE_LEVEL)
VOID     ApolloAggEndpointsHosted(_In_ PDEVICE_CONTEXT Ctx, _In_ BOOLEAN On);   /* no unit lock held */
_IRQL_requires_(PASSIVE_LEVEL)
VOID     ApolloAggServicePoll(VOID);                    /* no lock held; never waits for Agg.Mutex */
/* [Rate] the combined device's rate (agg.c): both units, master first; what GET_RATE reports of it */
_IRQL_requires_(PASSIVE_LEVEL)
ULONG    ApolloAggRateSet(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Hz, _In_ ULONG Flags, _In_opt_ WDFFILEOBJECT File,
                          _Out_writes_(WhyLen) PCHAR Why, _In_ size_t WhyLen);
BOOLEAN  ApolloAggRateMember(_In_ PDEVICE_CONTEXT Ctx);    /* runs in an active combined device with another unit */
_IRQL_requires_(PASSIVE_LEVEL)
VOID     ApolloAggRateUnitChanged(_In_ PDEVICE_CONTEXT Ctx);   /* no unit lock held: its latencies/layout again */
VOID     ApolloAggRateFill(_In_ PDEVICE_CONTEXT Ctx, _Inout_ PAPOLLO_RATE_INFO I);
/*
 * [Clock] A running member by HW_UID with its member rundown held (Ref), so its context stays valid while the caller
 * (panel.c, the service thread of another unit, no lock held) uses it; NULL = not a running member. Unref drops it.
 * Also whether Ctx is a member with another unit, and whether its clock is the timeline (GET_CLOCK flags).
 */
PDEVICE_CONTEXT ApolloAggUnitRef(_In_ ULONGLONG Uid, _Out_ PVOID *Ref);
VOID     ApolloAggUnitUnref(_In_opt_ PVOID Ref);
BOOLEAN  ApolloAggIsTimeline(_In_ PDEVICE_CONTEXT Ctx);
