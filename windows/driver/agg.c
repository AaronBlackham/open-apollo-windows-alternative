/*
 * agg.c - the combined device (docs/spec-twinx.md 6.2-6.9).
 *
 * One aggregate for the whole driver over every unit that runs in Mode 2. Each unit keeps
 * everything it had: its bring-up, transport, 1 ms engine (engine.c), its own Windows endpoints and
 * its own per-unit ASIO client. The aggregate adds a timeline and one combined ASIO client:
 *
 *   Timeline  the configured master's frames (default the Twin X, the monitor unit); if it is
 *             absent, the first running unit in block order (acting master).
 *   Followers every other running member. Their clock is measured against the timeline on every
 *             tick (apollo_agg.h ApolloAggClockUpdate, a PI loop on positions) and their channels
 *             pass through the adaptive resampler (spec 6.3.3): capture reads the follower's
 *             record ring at the mapped fractional position; render goes through a staging ring
 *             on the timeline, from which the resampler writes the follower's playback ring.
 *
 * The aggregate work runs at the end of the timeline unit's own engine tick (ApolloAggAfterTick,
 * DISPATCH_LEVEL), after that tick released its EngineLock. It reads the members' position
 * snapshots (the engines stay the only SAMPLE_POS readers) and touches only host memory: rings,
 * staging, the ASIO block. No MMIO from here.
 *
 * Locking (spec 6.2, cross-device order):
 *   Agg.Mutex (WDFWAITLOCK, PASSIVE) -> [StripLock -> DevLock of a unit] -> Agg.Lock (spin) ->
 *   member EngineLocks, several at once only in ascending HW_UID order.
 *   The engine tick holds only its own EngineLock and calls the aggregate after releasing it.
 *   AsioLock (per unit) is never taken by the aggregate; the combined client is under Agg.Mutex.
 *   Member contexts are protected by a rundown reference per member: Leave (D0 exit) unlinks the
 *   member under Agg.Lock and waits for the tick to drop the rundown before the rings can go.
 *
 * The combined Windows endpoints (spec 6.7): ACX circuits on the host unit (the x8p) whose
 * streams run here, on the timeline, like the combined ASIO client: render streams ADD into the
 * destinations of the "windows" routes (timeline slots directly, follower slots through the staging
 * rings and the resampler), capture streams read timeline slots directly and follower slots through
 * the resampler. Their state is under Agg.Lock (stream.c takes it instead of the host's EngineLock
 * for them); packet notifications are sent after every lock is dropped, under the stream's rundown
 * protection, exactly as engine.c does for the standalone streams. The staging rings are shared by
 * the ASIO client and these streams (several writers ADD into one ring); they are zeroed once per
 * tick, ahead of every writer (AggStageUpdate), and mapped incrementally so a client starting or
 * stopping never disturbs another's follower outputs.
 * Also: Align=0, Render/CaptureTrim, start ratios kept in Parameters\Aggregate\Ratios, and a
 * configuration generation Apollo Control polls.
 *
 * [Rate] Every unit runs at the timeline's rate or it is not used (muted, AGG_GET_STATE Reserved[1]
 * names it): the clock loop, the margin, the tick and the fades run on that rate's frames
 * (apollo_agg.h ApolloAggMarginAt / TickAt, APOLLO_AGG_CLOCK.Rate); the resampler's filter is
 * normalised and needs nothing. ApolloAggRateSet changes the rate of every unit, the timeline unit
 * first, with the tick paused (RateChanging) and a new epoch after. Start ratios are kept and used in
 * the 48 kHz family only (the 44.1 kHz family runs on other crystals): LastR only at its own rate.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <ntifs.h>
#include "apollo_stream.h"      /* APOLLO_STREAM (combined streams), includes apollo.h */
#include "apollo_agg.h"
#include <ntstrsafe.h>

#define AGG_STAGE_FRAMES        4096u
#define AGG_STAGE_MASK          (AGG_STAGE_FRAMES - 1)
#define AGG_STAGE_CH            32u         /* follower destination channels with a staging ring */
#define AGG_FADE_FRAMES         480.0       /* 10 ms fade-in after a lock (spec 6.3.4) */
#define AGG_STALE_MS            20          /* a follower snapshot older than this is not used */
#define AGG_MAX_LAG             APOLLO_AGG_MAX_LAG  /* capture lag that forces a re-origination (apollo_agg.h) */
#define AGG_NO_PERIOD           (~0ULL)
#define AGG_LOCK_TIMEOUT_MS     3000
#define AGG_TAG                 "agg"
#define AGG_STAGE_NONE          0xFF        /* StageUnit[] of a free staging ring */
#define AGG_RECLAIM_TICKS       1000u       /* the configured master runs this long, then takes the timeline back */
#define AGG_RATIO_CHECK_TICKS   600000ull   /* a kept ratio is re-checked every 10 minutes at most */
#define AGG_FADE_S              0.010       /* combined capture: fade-in after a lock, seconds */

static const UNICODE_STRING g_AggRateName = RTL_CONSTANT_STRING(L"SampleRate");             /* [Rate] */

static const UNICODE_STRING g_AggKeyName = RTL_CONSTANT_STRING(L"Aggregate");
static const UNICODE_STRING g_AggValueName = RTL_CONSTANT_STRING(L"Config");
static const UNICODE_STRING g_AggRatiosName = RTL_CONSTANT_STRING(L"Ratios");
static const UNICODE_STRING g_AggEpSetName = RTL_CONSTANT_STRING(L"EndpointSet");
static const UNICODE_STRING g_AggEpMaskName = RTL_CONSTANT_STRING(L"Endpoints");

typedef struct _AGG_JOINED {
    PDEVICE_CONTEXT Ctx;                    /* NULL = free slot */
    EX_RUNDOWN_REF  Rundown;
    ULONGLONG       Uid;
    ULONG           Model;
    APOLLO_AGG_UNIT_RT Rt;
    ULONG           InCount, OutCount;
    USHORT          InIds[APOLLO_AGG_UNIT_SLOTS];
    USHORT          OutIds[APOLLO_AGG_UNIT_SLOTS];
    ULONG           LatInPath, LatDac;      /* [Lat] Agg.Lock: the unit's device-side latencies (asio.c) */
} AGG_JOINED;

/* Per configured unit, runtime (Agg.Lock) */
typedef struct _AGG_UNIT {
    LONG            Joined;                 /* index into Joined[], -1 = absent */
    BOOLEAN         Usable;                 /* this tick */
    BOOLEAN         Formed;                 /* the clock map exists (follower) */
    APOLLO_AGG_CLOCK Clock;
    double          LastR;                  /* ratio of the last run, the next formation's start */
    double          GainCap, GainRen;       /* fades 0..1 */
    ULONGLONG       Pos;
    LONGLONG        Qpc;
    double          PfAtTm;
    LONGLONG        RenderHead;             /* follower frames written up to here (exclusive) */
    ULONG           RenderLate, CaptureShort;
    SHORT           StageOf[APOLLO_AGG_UNIT_SLOTS];
    LONG            CapOff;                 /* capture read offset (ApolloAggCapOffset) */
    LONG            RenOff;                 /* playback offset in timeline frames (ApolloAggRenOffset) */
    BOOLEAN         LastRValid;             /* LastR was measured in this load ... */
    ULONGLONG       LastRTl;                /* ... against this timeline unit (HW_UID) */
    ULONGLONG       RatioCheckTick;         /* g_Agg.Ticks of the last keep check, 0 = none yet */
    ULONG           Reclaim;                /* own ticks run as configured master off the timeline */
    ULONG           LastRRate;              /* [Rate] the rate LastR was measured at */
} AGG_UNIT;

typedef struct _AGG_MAP {
    PEPROCESS       OwnerProcess;           /* referenced */
    PVOID           Section;
    PVOID           SysView;
    PMDL            Mdl;
    PUCHAR          SysVa;
    PVOID           UserVa;
    PKEVENT         Event;                  /* referenced */
} AGG_MAP;

typedef struct _AGG_IREF {
    UCHAR           Valid;
    UCHAR           Unit;
    USHORT          Slot;
} AGG_IREF;

typedef struct _AGG_OREF {
    UCHAR           Count;
    UCHAR           Unit[APOLLO_AGG_MAX_DESTS];
    USHORT          Slot[APOLLO_AGG_MAX_DESTS];
} AGG_OREF;

typedef struct _AGG_ASIO {
    /* Agg.Mutex (and Agg.Lock for Open) */
    BOOLEAN         Open;
    WDFFILEOBJECT   OwnerFile;
    AGG_MAP         Map;
    PAPOLLO_ASIO_SHARED Shared;
    PLONG           In;
    PLONG           Out;
    ULONG           N, Shift, InCount, OutCount;
    AGG_IREF        InRef[APOLLO_AGG_MAX_CH];
    AGG_OREF        OutRef[APOLLO_AGG_MAX_CH];
    USHORT          OutL[APOLLO_AGG_MAX_CH];    /* the client's logical outputs (routing re-resolved from them) */
    /* Agg.Lock */
    BOOLEAN         Active;
    BOOLEAN         NeedSync;
    BOOLEAN         Stale;
    BOOLEAN         RenderKnown;
    BOOLEAN         RenderPlay;
    ULONGLONG       Origin, Delay, CapNext, RenderPeriod, PubPeriod[2];
    LONG            PubSwitch[2];
    ULONGLONG       SampleOffset, NextSample, DataHead;
    LONG            Switches, Underruns, Overruns;
    ULONG           Resyncs;
    ULONG           Epoch;
    ULONG           Pad;
} AGG_ASIO;

typedef struct _APOLLO_AGG {
    BOOLEAN         Ready;
    BOOLEAN         Loaded;                 /* the registry was read once */
    BOOLEAN         Stored;                 /* Cfg is a stored configuration (else the default) */
    WDFWAITLOCK     Mutex;
    KSPIN_LOCK      Lock;
    volatile LONG   Active;                 /* enabled and a timeline unit exists */
    PDEVICE_CONTEXT Timeline;               /* Agg.Lock */
    LONG            TimelineUnit;           /* config index, -1 = none */
    ULONG           TimelineEpoch;
    ULONG           LayoutVersion;
    ULONG           LockOrder[APOLLO_AGG_MAX_UNITS];    /* config units, ascending HW_UID */
    ULONG           LockCount;
    AGG_JOINED      Joined[APOLLO_AGG_MAX_UNITS];
    ULONG           SeenCount;              /* units seen since load, for the default config */
    ULONGLONG       SeenUid[APOLLO_AGG_MAX_UNITS];
    ULONG           SeenModel[APOLLO_AGG_MAX_UNITS];
    APOLLO_AGG_CONFIG Cfg;                  /* in force; changes under Mutex + Lock */
    APOLLO_AGG_COMPILED Lay;
    APOLLO_AGG_UNIT_RT Rt[APOLLO_AGG_MAX_UNITS];
    AGG_UNIT        U[APOLLO_AGG_MAX_UNITS];
    APOLLO_ASRC_TABLE *Asrc;                /* in use (Agg.Lock) */
    APOLLO_ASRC_TABLE *AsrcSpare;
    PLONG           Stage;                  /* AGG_STAGE_CH rings of AGG_STAGE_FRAMES */
    ULONG           StageCount;             /* rings 0..StageCount-1 may be in use (StageUnit AGG_STAGE_NONE = free) */
    UCHAR           StageUnit[AGG_STAGE_CH];
    USHORT          StageSlot[AGG_STAGE_CH];
    UCHAR           StageNeed[APOLLO_AGG_MAX_UNITS][APOLLO_AGG_UNIT_SLOTS];    /* AggStageMap scratch */
    ULONG           StageRings[APOLLO_AGG_MAX_UNITS];   /* rings per unit (follower render skips units with none) */
    BOOLEAN         StageValid;             /* the map and RenHead belong to StageEpoch */
    BOOLEAN         StageDirty;             /* a client started/stopped/re-routed: map again (not fresh) */
    ULONG           StageEpoch;
    ULONGLONG       RenHead;                /* timeline frames staged (zeroed, then written) up to here */
    ULONGLONG       Ticks;
    ULONG           LastUs, MaxUs;
    AGG_ASIO        Asio;
    /* combined Windows streams (Agg.Lock) */
    LIST_ENTRY      Streams;                /* APOLLO_STREAM.Link of the running combined streams */
    ULONG           StreamCount;
    BOOLEAN         TlValid;                /* the timeline snapshot below is from a tick that ran */
    BOOLEAN         TlRunValid;             /* TlRunUnit/TlLastPm/TlLastStarts: the last tick's record */
    ULONG           TlRunUnit;
    ULONG           TlLastStarts;           /* the timeline unit's EngStarts at the last tick */
    ULONGLONG       TlLastPm;               /* its position at the last tick */
    ULONGLONG       TlPos;
    LONGLONG        TlQpc, TlFreq;
    /* the host unit's combined circuits are added (AGG_GET_STATE.EndpointSet) */
    BOOLEAN         EpHosted;
    /* configuration generation (AGG_GET_STATE.ConfigGeneration) */
    volatile LONG   Generation;
    /* start ratios (Agg.Lock; written to the registry under Mutex) */
    APOLLO_AGG_RATIO Ratio[APOLLO_AGG_RATIOS];
    ULONG           RatioSeq;
    BOOLEAN         RatiosLoaded;
    volatile LONG   RatioDirty;
    /* [Rate] Agg.Lock (RateChanging: set under Mutex + Lock) */
    BOOLEAN         RateChanging;           /* ApolloAggRateSet runs: the tick does nothing */
    ULONG           TlRate;                 /* the timeline's rate at the last tick */
    ULONG           RateMismatch;           /* bit u = config unit u joined but at another rate (muted) */
    ULONG           StoredRate;             /* Parameters\Aggregate\SampleRate, 0 = none (Mutex) */
    /*
     * [Lat] Agg.Lock: what the combined device reports beyond the aligned layout (Lay.InLatency / OutLatency): the
     * largest strip path and DAC of the joined units (docs/ASIO.md, Latency). The alignment works on the ADC and
     * the engine figures (Pad), so a unit's strip path adds on top; equal on both units while both run strips.
     */
    ULONG           InExtra, OutExtra;
} APOLLO_AGG;

static APOLLO_AGG g_Agg;

/*
 * The compiler references _fltused from any object that uses floating point; the CRT defines it,
 * and a kernel driver links no CRT. x64 kernel code may use SSE2 without saving state (spec 6.3.3).
 */
int _fltused = 0;

static VOID AggStageMap(_In_ BOOLEAN Fresh);   /* Agg.Lock */
static VOID AggAsioResolveOutputs(VOID);    /* Agg.Lock */
static VOID AggStreamsResolve(VOID);        /* Agg.Lock */
static VOID AggNewEpoch(VOID);              /* Agg.Lock */

/* [Lat] Agg.Lock held: InExtra / OutExtra from the joined units; into the combined ASIO block when they moved. */
static VOID AggLatencyExtras(VOID)
{
    ULONG j, in = 0, out = 0;

    for (j = 0; j < APOLLO_AGG_MAX_UNITS; j++) {
        if (g_Agg.Joined[j].Ctx != NULL) {
            in = max(in, g_Agg.Joined[j].LatInPath);
            out = max(out, g_Agg.Joined[j].LatDac);
        }
    }
    if (in == g_Agg.InExtra && out == g_Agg.OutExtra) {
        return;
    }
    g_Agg.InExtra = in;
    g_Agg.OutExtra = out;
    if (g_Agg.Asio.Open && g_Agg.Asio.Shared != NULL) {
        g_Agg.Asio.Shared->InputLatencyFrames = g_Agg.Lay.InLatency + in;     /* the DLL: kAsioLatenciesChanged */
        g_Agg.Asio.Shared->OutputLatencyFrames = g_Agg.Lay.OutLatency + out;
    }
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static ULONGLONG AggUidOf(_In_ PDEVICE_CONTEXT Ctx)
{
    return ((ULONGLONG)Ctx->HwUidHi << 32) | Ctx->HwUidLo;
}

/* The member's engine is running on a live device: its snapshot and rings may be used. */
static BOOLEAN AggCtxRunning(_In_ PDEVICE_CONTEXT Ctx)
{
    /* [Rate] at any of the six rates; the tick also wants the timeline's rate */
    return !ApolloGateClosed(Ctx) && Ctx->TransportRunning && ApolloEngineRunning(Ctx) &&
           !ApolloEngineLost(Ctx) && Ctx->Play.Va != NULL && Ctx->Rec.Va != NULL && Ctx->RingFrames != 0 &&
           (Ctx->RingFrames & (Ctx->RingFrames - 1)) == 0 && Ctx->EngQpcFreq > 0 &&
           ApolloRateIndex(Ctx->SampleRate) != 0;
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS AggMutexAcquire(VOID)
{
    LONGLONG t = -10000LL * AGG_LOCK_TIMEOUT_MS;

    if (WdfWaitLockAcquire(g_Agg.Mutex, &t) == STATUS_TIMEOUT) {
        return STATUS_DEVICE_BUSY;
    }
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

_Use_decl_annotations_
NTSTATUS ApolloAggInit(WDFDRIVER Driver)
{
    WDF_OBJECT_ATTRIBUTES attrs;
    NTSTATUS status;
    ULONG i;

    RtlZeroMemory(&g_Agg, sizeof(g_Agg));
    KeInitializeSpinLock(&g_Agg.Lock);
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ParentObject = Driver;
    status = WdfWaitLockCreate(&attrs, &g_Agg.Mutex);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    g_Agg.Asrc = (APOLLO_ASRC_TABLE *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(APOLLO_ASRC_TABLE),
                                                       APOLLO_POOL_TAG);
    g_Agg.AsrcSpare = (APOLLO_ASRC_TABLE *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(APOLLO_ASRC_TABLE),
                                                            APOLLO_POOL_TAG);
    g_Agg.Stage = (PLONG)ExAllocatePool2(POOL_FLAG_NON_PAGED, (SIZE_T)AGG_STAGE_CH * AGG_STAGE_FRAMES * sizeof(LONG),
                                         APOLLO_POOL_TAG);
    if (g_Agg.Asrc == NULL || g_Agg.AsrcSpare == NULL || g_Agg.Stage == NULL) {
        ApolloAggUnload();
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    ApolloAsrcBuild(g_Agg.Asrc, 16);
    g_Agg.TimelineUnit = -1;
    for (i = 0; i < APOLLO_AGG_MAX_UNITS; i++) {
        ULONG s;

        g_Agg.U[i].Joined = -1;
        g_Agg.U[i].LastR = 1.0;
        for (s = 0; s < APOLLO_AGG_UNIT_SLOTS; s++) {
            g_Agg.U[i].StageOf[s] = -1;     /* the staging map is incremental: nothing mapped yet */
        }
    }
    for (i = 0; i < AGG_STAGE_CH; i++) {
        g_Agg.StageUnit[i] = AGG_STAGE_NONE;
    }
    InitializeListHead(&g_Agg.Streams);
    /* different after every load, so a host that polls it notices a reload too */
    g_Agg.Generation = (LONG)((KeQueryInterruptTime() / 10000ULL) & 0x3FFFFFFF) | 1;
    g_Agg.Ready = TRUE;
    ApolloLogPrint(AGG_TAG, "combined device ready (ASRC h 16, %d phases; spec-twinx 6.9.7: G7 combined Windows "
                   "endpoints, Align, trims, start ratios)", APOLLO_ASRC_PHASES);
    return STATUS_SUCCESS;
}

VOID ApolloAggUnload(VOID)
{
    g_Agg.Ready = FALSE;
    if (g_Agg.Asrc != NULL) {
        ExFreePoolWithTag(g_Agg.Asrc, APOLLO_POOL_TAG);
        g_Agg.Asrc = NULL;
    }
    if (g_Agg.AsrcSpare != NULL) {
        ExFreePoolWithTag(g_Agg.AsrcSpare, APOLLO_POOL_TAG);
        g_Agg.AsrcSpare = NULL;
    }
    if (g_Agg.Stage != NULL) {
        ExFreePoolWithTag(g_Agg.Stage, APOLLO_POOL_TAG);
        g_Agg.Stage = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Registry (PASSIVE, Mutex)                                           */
/* ------------------------------------------------------------------ */

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS AggOpenKey(_In_ ACCESS_MASK Access, _Out_ WDFKEY *Key)
{
    WDFKEY params = NULL;
    NTSTATUS status;

    *Key = NULL;
    status = WdfDriverOpenParametersRegistryKey(WdfGetDriver(), Access, WDF_NO_OBJECT_ATTRIBUTES, &params);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = WdfRegistryCreateKey(params, &g_AggKeyName, Access, REG_OPTION_NON_VOLATILE, NULL,
                                  WDF_NO_OBJECT_ATTRIBUTES, Key);
    WdfRegistryClose(params);
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
static VOID AggLoad(VOID)
{
    APOLLO_AGG_CONFIG *c;
    WDFKEY key = NULL;
    ULONG got = 0, type = 0, err;
    NTSTATUS status;

    g_Agg.Loaded = TRUE;
    c = (APOLLO_AGG_CONFIG *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*c), APOLLO_POOL_TAG);
    if (c == NULL) {
        return;
    }
    status = AggOpenKey(KEY_READ, &key);
    if (NT_SUCCESS(status)) {
        APOLLO_AGG_RATIO r[APOLLO_AGG_RATIOS];
        ULONG rgot = 0, rtype = 0, i, n = 0;

        /* the start ratios (spec 6.3.2): only usable entries, the newest Seq continues */
        RtlZeroMemory(r, sizeof(r));
        if (NT_SUCCESS(WdfRegistryQueryValue(key, &g_AggRatiosName, sizeof(r), r, &rgot, &rtype)) &&
            rtype == REG_BINARY && rgot == sizeof(r)) {
            for (i = 0; i < APOLLO_AGG_RATIOS; i++) {
                if (ApolloAggRatioUsable(&r[i])) {
                    g_Agg.Ratio[n++] = r[i];
                    if (r[i].Seq > g_Agg.RatioSeq) {
                        g_Agg.RatioSeq = r[i].Seq;
                    }
                    ApolloLogPrint(AGG_TAG, "start ratio kept: follower %016llx against %016llx %+ld ppb", r[i].Follower,
                                   r[i].Timeline, r[i].Ppb);
                }
            }
            g_Agg.RatiosLoaded = (n != 0);
        }
        {
            ULONG hz = 0;

            /* [Rate] the combined device's rate, as the last ApolloAggRateSet stored it (GET_RATE) */
            if (NT_SUCCESS(WdfRegistryQueryULong(key, &g_AggRateName, &hz)) && ApolloRateIndex(hz) != 0) {
                g_Agg.StoredRate = hz;
            }
        }
        status = WdfRegistryQueryValue(key, &g_AggValueName, sizeof(*c), c, &got, &type);
        WdfRegistryClose(key);
    }
    if (!NT_SUCCESS(status)) {
        ApolloLogPrint(AGG_TAG, "no stored configuration (0x%08x): the default applies", status);
    } else if (type != REG_BINARY || got != sizeof(*c) || (err = ApolloAggValidate(c)) != 0) {
        ApolloLogPrint(AGG_TAG, "stored configuration ignored (type %lu, %lu bytes, check %lu): the default applies",
                       type, got, (type == REG_BINARY && got == sizeof(*c)) ? ApolloAggValidate(c) : 0);
    } else {
        RtlCopyMemory(&g_Agg.Cfg, c, sizeof(*c));
        g_Agg.Stored = TRUE;
        ApolloLogPrint(AGG_TAG, "stored configuration loaded: %lu unit(s), master %lu, ASRC h %lu, flags 0x%lx, "
                       "%lu route(s)", c->UnitCount, c->Master, c->AsrcHalf, c->Flags, c->RouteCount);
    }
    ExFreePoolWithTag(c, APOLLO_POOL_TAG);
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS AggSave(VOID)
{
    WDFKEY key = NULL;
    NTSTATUS status;

    status = AggOpenKey(KEY_READ | KEY_WRITE, &key);
    if (NT_SUCCESS(status)) {
        status = WdfRegistryAssignValue(key, &g_AggValueName, REG_BINARY, sizeof(g_Agg.Cfg), &g_Agg.Cfg);
        WdfRegistryClose(key);
    }
    ApolloLogPrint(AGG_TAG, "configuration stored under Parameters\\Aggregate: 0x%08x", status);
    return status;
}

/* Parameters\Aggregate\Ratios from the table in memory (Mutex held). */
_IRQL_requires_(PASSIVE_LEVEL)
static VOID AggSaveRatios(VOID)
{
    APOLLO_AGG_RATIO r[APOLLO_AGG_RATIOS];
    WDFKEY key = NULL;
    NTSTATUS status;
    KIRQL irql;
    ULONG i;

    KeAcquireSpinLock(&g_Agg.Lock, &irql);
    RtlCopyMemory(r, g_Agg.Ratio, sizeof(r));
    KeReleaseSpinLock(&g_Agg.Lock, irql);
    status = AggOpenKey(KEY_READ | KEY_WRITE, &key);
    if (NT_SUCCESS(status)) {
        status = WdfRegistryAssignValue(key, &g_AggRatiosName, REG_BINARY, sizeof(r), r);
        WdfRegistryClose(key);
    }
    for (i = 0; i < APOLLO_AGG_RATIOS; i++) {
        if (ApolloAggRatioUsable(&r[i])) {
            ApolloLogPrint(AGG_TAG, "start ratio stored: follower %016llx against %016llx %+ld ppb (0x%08x)", r[i].Follower,
                           r[i].Timeline, r[i].Ppb, status);
        }
    }
}

/*
 * The endpoint set (device add, PASSIVE, no lock): Parameters\Aggregate\EndpointSet and Endpoints
 * (an old EndpointMask value, over a 12-row table, is ignored). Absent, unreadable or unknown set =
 * standalone.
 */
_Use_decl_annotations_
ULONG ApolloAggReadEndpointSet(PULONG Mask)
{
    WDFKEY params = NULL, key = NULL;
    ULONG set = APOLLO_AGG_EPSET_STANDALONE, v = 0;

    *Mask = APOLLO_AGG_EP_DEFAULT;
    if (!NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &params))) {
        return set;
    }
    if (NT_SUCCESS(WdfRegistryOpenKey(params, &g_AggKeyName, KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        if (NT_SUCCESS(WdfRegistryQueryULong(key, &g_AggEpSetName, &v)) && v == APOLLO_AGG_EPSET_COMBINED) {
            set = APOLLO_AGG_EPSET_COMBINED;
        }
        if (NT_SUCCESS(WdfRegistryQueryULong(key, &g_AggEpMaskName, &v))) {
            *Mask = v;
        }
        WdfRegistryClose(key);
    }
    WdfRegistryClose(params);
    return set;
}

/* Try-lock from a unit's service thread (PASSIVE, no lock held): write the ratios when they changed. */
_Use_decl_annotations_
VOID ApolloAggServicePoll(VOID)
{
    LONGLONG none = 0;

    if (!g_Agg.Ready || ReadNoFence(&g_Agg.RatioDirty) == 0) {
        return;
    }
    if (WdfWaitLockAcquire(g_Agg.Mutex, &none) != STATUS_SUCCESS) {
        return;                             /* busy: the next pass (10 ms later) tries again */
    }
    if (InterlockedExchange(&g_Agg.RatioDirty, 0) != 0) {
        AggSaveRatios();
    }
    WdfWaitLockRelease(g_Agg.Mutex);
}

/* ------------------------------------------------------------------ */
/* Rebuild: membership, timeline, layout (PASSIVE, Mutex held)         */
/* ------------------------------------------------------------------ */

static LONG AggJoinedByUid(_In_ ULONGLONG Uid)
{
    ULONG j;

    for (j = 0; j < APOLLO_AGG_MAX_UNITS; j++) {
        if (g_Agg.Joined[j].Ctx != NULL && g_Agg.Joined[j].Uid == Uid) {
            return (LONG)j;
        }
    }
    return -1;
}

/* The default (nothing stored): every unit seen since load, the x8p first, the Twin X master, Studio. */
static VOID AggDefaultConfig(_Out_ APOLLO_AGG_CONFIG *C)
{
    ULONG i, k, n = 0, twx = MAXULONG;
    ULONG order[APOLLO_AGG_MAX_UNITS];

    RtlZeroMemory(C, sizeof(*C));
    C->Version = APOLLO_AGG_VERSION;
    C->Bytes = sizeof(*C);
    C->Flags = APOLLO_AGG_CFG_ENABLED | APOLLO_AGG_CFG_ALIGN;
    C->AsrcHalf = 16;
    /* x8p first, then the Twin X, then anything else, each group by HW_UID */
    for (k = 0; k < 3; k++) {
        for (i = 0; i < g_Agg.SeenCount; i++) {
            ULONG m = g_Agg.SeenModel[i];
            ULONG grp = (m == APOLLO_INFO_MODEL_X8P) ? 0 : (m == APOLLO_INFO_MODEL_TWINX ? 1 : 2);

            if (grp == k) {
                order[n++] = i;
            }
        }
    }
    for (i = 0; i < n; i++) {
        C->Unit[i].Uid = g_Agg.SeenUid[order[i]];
        C->Unit[i].Model = g_Agg.SeenModel[order[i]];
        if (C->Unit[i].Model == APOLLO_INFO_MODEL_TWINX && twx == MAXULONG) {
            twx = i;
        }
    }
    C->UnitCount = n;
    C->Master = (twx != MAXULONG) ? twx : 0;
    if (twx != MAXULONG && n > 1) {
        /* Studio: DAW mains to the Twin X monitor outputs (the speakers) */
        C->RouteCount = 1;
        C->Route[0].SrcKind = APOLLO_AGG_SRC_DAW;
        C->Route[0].DstUnit = (UCHAR)twx;
        C->Route[0].DstId[0] = APOLLO_CHAN_MON_L;
        C->Route[0].DstId[1] = APOLLO_CHAN_MON_R;
    }
}

/* The unit caches in C from the joined units (and the old config's caches for absent ones). */
static BOOLEAN AggFillCaches(_Inout_ APOLLO_AGG_CONFIG *C, _In_opt_ const APOLLO_AGG_CONFIG *Old)
{
    ULONG u, k;
    BOOLEAN changed = FALSE;

    for (u = 0; u < C->UnitCount && u < APOLLO_AGG_MAX_UNITS; u++) {
        APOLLO_AGG_UNIT_CFG *uc = &C->Unit[u];
        LONG j = AggJoinedByUid(uc->Uid);

        if (j >= 0) {
            const AGG_JOINED *m = &g_Agg.Joined[j];

            if (uc->Model != m->Model || uc->InCount != m->InCount || uc->OutCount != m->OutCount ||
                uc->CaptureTrim != (CHAR)m->Rt.CapTrim || uc->RenderTrim != (CHAR)m->Rt.RenTrim ||
                RtlCompareMemory(uc->InIds, m->InIds, sizeof(uc->InIds)) != sizeof(uc->InIds) ||
                RtlCompareMemory(uc->OutIds, m->OutIds, sizeof(uc->OutIds)) != sizeof(uc->OutIds)) {
                changed = TRUE;
            }
            uc->Model = m->Model;
            uc->InCount = (UCHAR)m->InCount;
            uc->OutCount = (UCHAR)m->OutCount;
            uc->CaptureTrim = (CHAR)m->Rt.CapTrim;  /* cached like the descriptor (-64..64 fits) */
            uc->RenderTrim = (CHAR)m->Rt.RenTrim;
            RtlCopyMemory(uc->InIds, m->InIds, sizeof(uc->InIds));
            RtlCopyMemory(uc->OutIds, m->OutIds, sizeof(uc->OutIds));
        } else if (Old != NULL && uc->InCount == 0 && uc->OutCount == 0) {
            for (k = 0; k < Old->UnitCount && k < APOLLO_AGG_MAX_UNITS; k++) {
                if (Old->Unit[k].Uid == uc->Uid) {
                    ULONG model = uc->Model ? uc->Model : Old->Unit[k].Model;

                    *uc = Old->Unit[k];
                    uc->Model = model;
                    break;
                }
            }
        }
    }
    return changed;
}

/* Runtime parameters per configured unit (joined: its own; absent: the model's defaults [Rate] at Rate). */
static VOID AggFillRt(_In_ const APOLLO_AGG_CONFIG *C, _Out_writes_(APOLLO_AGG_MAX_UNITS) APOLLO_AGG_UNIT_RT *Rt,
                      _In_ ULONG Rate)
{
    ULONG u, idx = ApolloRateIndex(Rate) != 0 ? ApolloRateIndex(Rate) : APOLLO_RATE_IDX_48K;

    RtlZeroMemory(Rt, sizeof(APOLLO_AGG_UNIT_RT) * APOLLO_AGG_MAX_UNITS);
    for (u = 0; u < APOLLO_AGG_MAX_UNITS; u++) {
        LONG j = (u < C->UnitCount) ? AggJoinedByUid(C->Unit[u].Uid) : -1;

        if (j >= 0) {
            Rt[u] = g_Agg.Joined[j].Rt;
            Rt[u].Present = 1;
        } else {
            Rt[u].Present = 0;
            Rt[u].Guard = ApolloRateGuard(APOLLO_DEF_CAPTURE_GUARD, ApolloRateHz(idx));
            Rt[u].Lead = ApolloRateLead(APOLLO_DEF_RENDER_LEAD, ApolloRateHz(idx));
            Rt[u].FrameCfg = ApolloRateFrameCfg(X8P_SAFETY_FRAMES_48K, idx);
            Rt[u].Adc = ApolloRateAdc((u < C->UnitCount) ? ApolloAggModelAdc(C->Unit[u].Model) : APOLLO_X8P_ADC_FRAMES, idx);
            Rt[u].Rate = (Rate == 0 || Rate == APOLLO_RATE_DEFAULT_HZ) ? 0 : Rate;   /* 0: 48 kHz */
            /* an absent unit keeps its cached trims, so it coming and going does not move the latency */
            Rt[u].CapTrim = (u < C->UnitCount) ? ApolloAggTrimOf((ULONG)(LONG)C->Unit[u].CaptureTrim) : 0;
            Rt[u].RenTrim = (u < C->UnitCount) ? ApolloAggTrimOf((ULONG)(LONG)C->Unit[u].RenderTrim) : 0;
        }
    }
}

/*
 * Apply the configuration in g_Agg.Cfg (Mutex held): unit caches, timeline, layout, then the
 * runtime state under Agg.Lock. Returns TRUE when the stored caches changed (the caller saves).
 */
_IRQL_requires_(PASSIVE_LEVEL)
static BOOLEAN AggRebuild(_In_z_ PCSTR Why)
{
    APOLLO_AGG_COMPILED *lay;
    APOLLO_AGG_CONFIG *nc;
    APOLLO_AGG_UNIT_RT rt[APOLLO_AGG_MAX_UNITS];
    LONG joined[APOLLO_AGG_MAX_UNITS], tlUnit = -1;
    ULONG u, k, order[APOLLO_AGG_MAX_UNITS] = { 0 }, lockCount = 0;
    BOOLEAN cachesChanged, sameLists, enabled;
    PDEVICE_CONTEXT tl = NULL;
    KIRQL irql;

    /* Both are copied under the spin lock below: non-paged. */
    lay = (APOLLO_AGG_COMPILED *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*lay), APOLLO_POOL_TAG);
    nc = (APOLLO_AGG_CONFIG *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*nc), APOLLO_POOL_TAG);
    if (lay == NULL || nc == NULL) {
        ApolloLogPrint(AGG_TAG, "rebuild (%s): out of memory, the combined device keeps its last layout", Why);
        if (lay != NULL) {
            ExFreePoolWithTag(lay, APOLLO_POOL_TAG);
        }
        if (nc != NULL) {
            ExFreePoolWithTag(nc, APOLLO_POOL_TAG);
        }
        return FALSE;
    }
    if (!g_Agg.Stored) {
        AggDefaultConfig(nc);
        (VOID)AggFillCaches(nc, &g_Agg.Cfg);
        cachesChanged = FALSE;
    } else {
        RtlCopyMemory(nc, &g_Agg.Cfg, sizeof(*nc));
        cachesChanged = AggFillCaches(nc, NULL);
    }
    /*
     * The combined Windows endpoints run on the timeline too: while the host has them added, a
     * timeline runs even with Combined switched off in the configuration (they are then the only
     * Windows endpoints the units have). The combined ASIO driver still needs ENABLED (AggAsioOpen).
     */
    enabled = ((nc->Flags & APOLLO_AGG_CFG_ENABLED) != 0 || g_Agg.EpHosted) && nc->UnitCount != 0;

    for (u = 0; u < APOLLO_AGG_MAX_UNITS; u++) {
        joined[u] = (u < nc->UnitCount) ? AggJoinedByUid(nc->Unit[u].Uid) : -1;
    }
    /* timeline: the configured master if it runs, else the first running unit in block order */
    if (enabled) {
        if (nc->Master < nc->UnitCount && joined[nc->Master] >= 0) {
            tlUnit = (LONG)nc->Master;
        } else {
            for (u = 0; u < nc->UnitCount; u++) {
                if (joined[u] >= 0) {
                    tlUnit = (LONG)u;
                    break;
                }
            }
        }
    }
    if (tlUnit >= 0) {
        tl = g_Agg.Joined[joined[tlUnit]].Ctx;
    }
    AggFillRt(nc, rt, tl != NULL ? tl->SampleRate : APOLLO_RATE_DEFAULT_HZ);     /* [Rate] */
    ApolloAggCompile(nc, rt, tlUnit >= 0 ? (ULONG)tlUnit : MAXULONG, lay);

    /* lock order: joined config units by ascending HW_UID */
    for (u = 0; u < nc->UnitCount && u < APOLLO_AGG_MAX_UNITS; u++) {
        if (joined[u] >= 0) {
            order[lockCount++] = u;
        }
    }
    for (u = 1; u < lockCount; u++) {
        for (k = u; k > 0 && nc->Unit[order[k - 1]].Uid > nc->Unit[order[k]].Uid; k--) {
            ULONG t = order[k];

            order[k] = order[k - 1];
            order[k - 1] = t;
        }
    }
    /* the ASRC table for the configured size, built here at PASSIVE into the spare */
    if (g_Agg.AsrcSpare != NULL && g_Agg.Asrc != NULL && g_Agg.Asrc->Half != nc->AsrcHalf) {
        ApolloAsrcBuild(g_Agg.AsrcSpare, nc->AsrcHalf);
    }

    KeAcquireSpinLock(&g_Agg.Lock, &irql);
    RtlCopyMemory(&g_Agg.Cfg, nc, sizeof(*nc));    /* the tick reads UnitCount and Master */
    sameLists = ApolloAggSameLists(&g_Agg.Lay, lay) ? TRUE : FALSE;
    RtlCopyMemory(&g_Agg.Lay, lay, sizeof(*lay));
    RtlCopyMemory(g_Agg.Rt, rt, sizeof(rt));
    g_Agg.InExtra = g_Agg.OutExtra = MAXULONG;  /* [Lat] the layout or the joined units changed: published again */
    AggLatencyExtras();
    if (!sameLists) {
        g_Agg.LayoutVersion++;
        if (g_Agg.Asio.Open) {
            g_Agg.Asio.Stale = TRUE;        /* its channel numbers mean something else now: silence */
            g_Agg.Asio.Shared->Reserved[2] |= APOLLO_AGG_SHARED_STALE;
            g_Agg.Asio.Shared->Reserved[0] = g_Agg.LayoutVersion;
        }
    } else if (g_Agg.Asio.Open && !g_Agg.Asio.Stale) {
        /* the same channels: a routing change applies from the next tick, no reset (apollo-control-units 3) */
        AggAsioResolveOutputs();
    }
    if (g_Agg.AsrcSpare != NULL && g_Agg.Asrc != NULL && g_Agg.AsrcSpare->Half == g_Agg.Cfg.AsrcHalf &&
        g_Agg.Asrc->Half != g_Agg.Cfg.AsrcHalf) {
        APOLLO_ASRC_TABLE *t = g_Agg.Asrc;

        g_Agg.Asrc = g_Agg.AsrcSpare;
        g_Agg.AsrcSpare = t;
        for (u = 0; u < APOLLO_AGG_MAX_UNITS; u++) {
            g_Agg.U[u].Formed = FALSE;      /* the look-ahead changed: re-form */
        }
        g_Agg.Asio.NeedSync = TRUE;
    }
    for (u = 0; u < APOLLO_AGG_MAX_UNITS; u++) {
        if (g_Agg.U[u].Joined != joined[u]) {
            g_Agg.U[u].Formed = FALSE;
            g_Agg.U[u].Usable = FALSE;
        }
        g_Agg.U[u].Joined = joined[u];
    }
    RtlCopyMemory(g_Agg.LockOrder, order, sizeof(order));
    g_Agg.LockCount = lockCount;
    if (tl == NULL) {
        g_Agg.TlValid = FALSE;              /* no timeline: combined stream positions stand still */
    }
    if (g_Agg.Timeline != tl || g_Agg.TimelineUnit != tlUnit) {
        g_Agg.Timeline = tl;
        g_Agg.TimelineUnit = tlUnit;
        AggNewEpoch();
    }
    /* the combined Windows streams follow the configuration (routes, master) from the next tick */
    AggStreamsResolve();
    g_Agg.StageDirty = TRUE;
    InterlockedExchange(&g_Agg.Active, (enabled && tl != NULL) ? 1 : 0);
    KeReleaseSpinLock(&g_Agg.Lock, irql);

    /* a follower told to play earlier than the lead allows plays at the most it allows */
    for (u = 0; tlUnit >= 0 && u < nc->UnitCount && u < APOLLO_AGG_MAX_UNITS; u++) {
        LONG raw, max;

        if ((LONG)u == tlUnit) {
            continue;
        }
        raw = ApolloAggRenOffsetRaw(rt, (ULONG)tlUnit, u);
        max = ApolloAggRenOffsetMaxAt(rt[tlUnit].Lead, nc->AsrcHalf, rt[u].FrameCfg, rt[tlUnit].Rate);   /* [Rate] */
        if (raw > max) {
            ApolloLogPrint(AGG_TAG, "unit %lu: RenderTrimFrames differ from the timeline's by %ld frames (earlier); the lead "
                           "allows %ld at ASRC h %lu: clamped", u, raw, max, nc->AsrcHalf);
        }
    }

    ApolloLogPrint(AGG_TAG, "rebuild (%s): %s%s, %lu unit(s), timeline %ld (%s), layout v%lu %lu in / %lu out, "
                   "latency in %lu out %lu (pad %lu, %s)", Why,
                   !enabled ? "OFF" : ((g_Agg.Cfg.Flags & APOLLO_AGG_CFG_ENABLED) ? "enabled" :
                                       "Combined off, running for the combined Windows endpoints"),
                   g_Agg.Stored ? "" : " (default config)", g_Agg.Cfg.UnitCount, tlUnit,
                   tlUnit < 0 ? "none" : ApolloAggModelTag(g_Agg.Cfg.Unit[tlUnit].Model), g_Agg.LayoutVersion,
                   lay->InCount, lay->OutCount, lay->InLatency, lay->OutLatency, lay->Pad,
                   (g_Agg.Cfg.Flags & APOLLO_AGG_CFG_ALIGN) ? "aligned" : "UNALIGNED");
    ExFreePoolWithTag(lay, APOLLO_POOL_TAG);
    ExFreePoolWithTag(nc, APOLLO_POOL_TAG);
    return cachesChanged;
}

/* ------------------------------------------------------------------ */
/* Join / leave (PASSIVE, no unit lock held)                           */
/* ------------------------------------------------------------------ */

/* A member's runtime values and descriptor caches from its context (Mutex held): join, rate change. */
static VOID AggMemberFill(_Inout_ AGG_JOINED *M, _In_ PDEVICE_CONTEXT Ctx)
{
    ULONG i, cin = ApolloIoDescCount(Ctx->IoDescIn, 1), cout = ApolloIoDescCount(Ctx->IoDescOut, 0);

    /* [Rate] at the unit's rate (plan->AdcFrames and FrameCfg48k at 48 kHz) */
    M->Rt.Guard = Ctx->EngGuard;
    M->Rt.Lead = Ctx->EngLead;
    M->Rt.Adc = Ctx->AdcFrames;
    M->Rt.FrameCfg = Ctx->FrameCfg;
    M->Rt.Rate = (Ctx->SampleRate == APOLLO_RATE_DEFAULT_HZ) ? 0 : Ctx->SampleRate;
    M->Rt.Dig = Ctx->DigState;              /* [Clock] the digital formats chosen (0 = none); a change re-compiles the layout */
    M->InCount = min(cin != 0 ? cin : Ctx->RecChannels, min(Ctx->RecChannels, (ULONG)APOLLO_AGG_UNIT_SLOTS));
    M->OutCount = min(cout != 0 ? cout : Ctx->PlayChannels, min(Ctx->PlayChannels, (ULONG)APOLLO_AGG_UNIT_SLOTS));
    RtlZeroMemory(M->InIds, sizeof(M->InIds));
    RtlZeroMemory(M->OutIds, sizeof(M->OutIds));
    for (i = 0; i < M->InCount; i++) {
        M->InIds[i] = (cin != 0) ? ApolloIoDescId(Ctx->IoDescIn, i) : 0;
    }
    for (i = 0; i < M->OutCount; i++) {
        M->OutIds[i] = (cout != 0) ? ApolloIoDescId(Ctx->IoDescOut, i) : 0;
    }
}

_Use_decl_annotations_
VOID ApolloAggJoin(PDEVICE_CONTEXT Ctx)
{
    const APOLLO_BRINGUP_PLAN *plan = ApolloPlanOf(Ctx);
    ULONGLONG uid = AggUidOf(Ctx);
    ULONG j, i, free = MAXULONG;
    AGG_JOINED *m;

    if (!g_Agg.Ready || plan == NULL || !Ctx->Unit.Active || Ctx->Unit.Value[APOLLO_PARAM_MODE] < 2 ||
        uid == 0 || !Ctx->TransportRunning || !ApolloEngineRunning(Ctx)) {
        return;
    }
    if (!NT_SUCCESS(AggMutexAcquire())) {
        ApolloLogU(Ctx, "agg: join skipped (busy)");
        return;
    }
    if (!g_Agg.Loaded) {
        AggLoad();
    }
    for (j = 0; j < APOLLO_AGG_MAX_UNITS; j++) {
        if (g_Agg.Joined[j].Ctx == Ctx) {
            WdfWaitLockRelease(g_Agg.Mutex);
            return;                         /* already a member */
        }
        if (g_Agg.Joined[j].Ctx == NULL && free == MAXULONG) {
            free = j;
        }
    }
    if (free == MAXULONG) {
        WdfWaitLockRelease(g_Agg.Mutex);
        ApolloLogU(Ctx, "agg: no free member slot");
        return;
    }
    m = &g_Agg.Joined[free];
    RtlZeroMemory(m, sizeof(*m));
    ExInitializeRundownProtection(&m->Rundown);
    m->Uid = uid;
    m->Model = (ULONG)Ctx->Ident.Model;
    m->Rt.Present = 1;
    AggMemberFill(m, Ctx);
    /* Units\<key>\CaptureTrimFrames / RenderTrimFrames (signed DWORDs, spec 6.3.5) */
    m->Rt.CapTrim = ApolloAggTrimOf(ApolloUnitParam(Ctx, APOLLO_PARAM_CAPTURE_TRIM));
    m->Rt.RenTrim = ApolloAggTrimOf(ApolloUnitParam(Ctx, APOLLO_PARAM_RENDER_TRIM));
    m->LatInPath = Ctx->LatInPath;          /* [Lat] the service pass keeps them current from here */
    m->LatDac = Ctx->DacFrames;
    m->Ctx = Ctx;                           /* last: AggJoinedByUid finds it from here on */
    for (i = 0; i < g_Agg.SeenCount; i++) {
        if (g_Agg.SeenUid[i] == uid) {
            break;
        }
    }
    if (i == g_Agg.SeenCount && g_Agg.SeenCount < APOLLO_AGG_MAX_UNITS) {
        g_Agg.SeenUid[g_Agg.SeenCount] = uid;
        g_Agg.SeenModel[g_Agg.SeenCount] = m->Model;
        g_Agg.SeenCount++;
    }
    ApolloLogU(Ctx, "agg: joined as member %lu (HW_UID 0x%016llx, record %lu / playback %lu slots, ADC %lu, "
              "guard %lu, lead %lu, capture trim %ld, render trim %ld)", free, uid, m->InCount, m->OutCount, m->Rt.Adc,
              m->Rt.Guard, m->Rt.Lead, m->Rt.CapTrim, m->Rt.RenTrim);
    if (AggRebuild("unit joined") && g_Agg.Stored) {
        (VOID)AggSave();                    /* its descriptor is now cached in the stored config */
    }
    WdfWaitLockRelease(g_Agg.Mutex);
}

_Use_decl_annotations_
VOID ApolloAggLeave(PDEVICE_CONTEXT Ctx)
{
    ULONG j, u;
    KIRQL irql;
    AGG_JOINED *m = NULL;

    if (!g_Agg.Ready) {
        return;
    }
    WdfWaitLockAcquire(g_Agg.Mutex, NULL);  /* no timeout: the rings must not go before this */
    for (j = 0; j < APOLLO_AGG_MAX_UNITS; j++) {
        if (g_Agg.Joined[j].Ctx == Ctx) {
            m = &g_Agg.Joined[j];
            break;
        }
    }
    if (m == NULL) {
        WdfWaitLockRelease(g_Agg.Mutex);
        return;
    }
    /* Unlink under the spin lock: no tick starts on it from here (nor drives the aggregate from it). */
    KeAcquireSpinLock(&g_Agg.Lock, &irql);
    for (u = 0; u < APOLLO_AGG_MAX_UNITS; u++) {
        if (g_Agg.U[u].Joined == (LONG)j) {
            g_Agg.U[u].Joined = -1;
            g_Agg.U[u].Formed = FALSE;
            g_Agg.U[u].Usable = FALSE;
        }
    }
    g_Agg.LockCount = 0;                    /* the rebuild below sets the order of the units left */
    if (g_Agg.Timeline == Ctx) {
        g_Agg.Timeline = NULL;
        g_Agg.TimelineUnit = -1;
        g_Agg.TlValid = FALSE;              /* its snapshot is not a timeline any more */
        InterlockedExchange(&g_Agg.Active, 0);
    }
    KeReleaseSpinLock(&g_Agg.Lock, irql);
    /* A tick that took the rundown before the unlink finishes first. */
    ExWaitForRundownProtectionRelease(&m->Rundown);
    m->Ctx = NULL;
    ApolloLogU(Ctx, "agg: left (member %lu)", j);
    (VOID)AggRebuild("unit left");
    /* a start ratio the tick kept since the last service pass: write it now (sleep, shutdown) */
    if (InterlockedExchange(&g_Agg.RatioDirty, 0) != 0) {
        AggSaveRatios();
    }
    WdfWaitLockRelease(g_Agg.Mutex);
}

/*
 * The host unit added (On) or removed its combined circuits (circuit.c, PASSIVE, no unit lock
 * held). While they are added the aggregate runs a timeline even with Combined off (AggRebuild).
 */
_Use_decl_annotations_
VOID ApolloAggEndpointsHosted(PDEVICE_CONTEXT Ctx, BOOLEAN On)
{
    if (!g_Agg.Ready) {
        return;
    }
    WdfWaitLockAcquire(g_Agg.Mutex, NULL);
    if (g_Agg.EpHosted != On) {
        g_Agg.EpHosted = On;
        ApolloLogU(Ctx, "agg: combined Windows endpoints %s on this unit", On ? "added" : "removed");
        if (g_Agg.Loaded) {
            (VOID)AggRebuild(On ? "combined endpoints added" : "combined endpoints removed");
        }
    }
    WdfWaitLockRelease(g_Agg.Mutex);
}

/* GET_INFO AggRole (lock-free: a snapshot) */
_Use_decl_annotations_
ULONG ApolloAggRoleOf(PDEVICE_CONTEXT Ctx)
{
    LONG tlu = g_Agg.TimelineUnit;
    ULONG u;

    if (!g_Agg.Ready || ReadNoFence(&g_Agg.Active) == 0) {
        return APOLLO_AGG_ROLE_STANDALONE;
    }
    for (u = 0; u < g_Agg.Cfg.UnitCount && u < APOLLO_AGG_MAX_UNITS; u++) {
        LONG j = g_Agg.U[u].Joined;

        if (j < 0 || j >= APOLLO_AGG_MAX_UNITS || g_Agg.Joined[j].Ctx != Ctx) {
            continue;
        }
        if ((LONG)u == tlu) {
            return (u == g_Agg.Cfg.Master) ? APOLLO_AGG_ROLE_MASTER : APOLLO_AGG_ROLE_ACTING_MASTER;
        }
        return APOLLO_AGG_ROLE_FOLLOWER;
    }
    return APOLLO_AGG_ROLE_STANDALONE;
}

/* ------------------------------------------------------------------ */
/* The tick (DISPATCH_LEVEL, Agg.Lock and the members' EngineLocks)    */
/* ------------------------------------------------------------------ */

static PDEVICE_CONTEXT AggCtx(_In_ ULONG U)
{
    LONG j = g_Agg.U[U].Joined;

    return (j >= 0 && j < APOLLO_AGG_MAX_UNITS) ? g_Agg.Joined[j].Ctx : NULL;
}

static VOID AggStageNeed(_In_ ULONG U, _In_ ULONG S, _In_ LONG T)
{
    if (U < APOLLO_AGG_MAX_UNITS && (LONG)U != T && S < APOLLO_AGG_UNIT_SLOTS) {
        g_Agg.StageNeed[U][S] = 1;
    }
}

/*
 * The staging rings for follower destinations (Agg.Lock): every destination of the combined ASIO
 * client's outputs and of the running combined render streams that is not on the timeline.
 * Fresh (the timeline changed: the content is another clock's): every ring zeroed and mapped anew.
 * Otherwise incremental: a destination keeps its ring and content, a new one takes a free zeroed
 * ring, a dropped one frees its ring; so one client starting or stopping never touches another's
 * follower outputs. More destinations than rings: the extra ones stay silent.
 */
static VOID AggStageMap(_In_ BOOLEAN Fresh)
{
    AGG_ASIO *a = &g_Agg.Asio;
    ULONG u, c, d, s, i;
    LONG t = g_Agg.TimelineUnit;
    PLIST_ENTRY e;

    RtlZeroMemory(g_Agg.StageNeed, sizeof(g_Agg.StageNeed));
    if (a->Open) {
        for (c = 0; c < a->OutCount && c < APOLLO_AGG_MAX_CH; c++) {
            const AGG_OREF *r = &a->OutRef[c];

            for (d = 0; d < r->Count && d < APOLLO_AGG_MAX_DESTS; d++) {
                AggStageNeed(r->Unit[d], r->Slot[d], t);
            }
        }
    }
    for (e = g_Agg.Streams.Flink; e != &g_Agg.Streams; e = e->Flink) {
        PAPOLLO_STREAM st = CONTAINING_RECORD(e, APOLLO_STREAM, Link);

        if (!st->Render) {
            continue;
        }
        for (c = 0; c < 2; c++) {
            for (d = 0; d < st->AggRef.Count[c] && d < APOLLO_AGG_MAX_DESTS; d++) {
                AggStageNeed(st->AggRef.Unit[c][d], st->AggRef.Slot[c][d], t);
            }
        }
    }
    for (u = 0; u < APOLLO_AGG_MAX_UNITS; u++) {
        for (s = 0; s < APOLLO_AGG_UNIT_SLOTS; s++) {
            SHORT r = g_Agg.U[u].StageOf[s];

            if (r >= 0 && (Fresh || !g_Agg.StageNeed[u][s] || (ULONG)r >= AGG_STAGE_CH)) {
                if ((ULONG)r < AGG_STAGE_CH) {
                    g_Agg.StageUnit[r] = AGG_STAGE_NONE;
                }
                g_Agg.U[u].StageOf[s] = -1;
            }
        }
    }
    if (Fresh) {
        for (i = 0; i < AGG_STAGE_CH; i++) {
            g_Agg.StageUnit[i] = AGG_STAGE_NONE;
        }
    }
    for (u = 0; u < APOLLO_AGG_MAX_UNITS; u++) {
        for (s = 0; s < APOLLO_AGG_UNIT_SLOTS; s++) {
            if (!g_Agg.StageNeed[u][s] || g_Agg.U[u].StageOf[s] >= 0) {
                continue;
            }
            for (i = 0; i < AGG_STAGE_CH && g_Agg.StageUnit[i] != AGG_STAGE_NONE; i++) {
            }
            if (i == AGG_STAGE_CH) {
                continue;                   /* no ring left: this destination stays silent */
            }
            g_Agg.U[u].StageOf[s] = (SHORT)i;
            g_Agg.StageUnit[i] = (UCHAR)u;
            g_Agg.StageSlot[i] = (USHORT)s;
            RtlZeroMemory(g_Agg.Stage + (SIZE_T)i * AGG_STAGE_FRAMES, AGG_STAGE_FRAMES * sizeof(LONG));
        }
    }
    g_Agg.StageCount = 0;
    RtlZeroMemory(g_Agg.StageRings, sizeof(g_Agg.StageRings));
    for (i = 0; i < AGG_STAGE_CH; i++) {
        if (g_Agg.StageUnit[i] != AGG_STAGE_NONE) {
            g_Agg.StageCount = i + 1;
            if (g_Agg.StageUnit[i] < APOLLO_AGG_MAX_UNITS) {
                g_Agg.StageRings[g_Agg.StageUnit[i]]++;
            }
        }
    }
}

/*
 * The client's output destinations from the layout's routes (Agg.Lock): at open, and again when a
 * configuration keeps the channel lists but changes the routes, so routing switches at the next
 * tick without a reset. The staging rings follow (a few ms of a moved follower destination are lost).
 */
static VOID AggAsioResolveOutputs(VOID)
{
    AGG_ASIO *a = &g_Agg.Asio;
    ULONG i, d;

    for (i = 0; i < a->OutCount && i < APOLLO_AGG_MAX_CH; i++) {
        ULONG lo = a->OutL[i];

        a->OutRef[i].Count = 0;
        if (lo >= g_Agg.Lay.OutCount || lo >= APOLLO_AGG_MAX_CH) {
            continue;                       /* cannot happen with the same lists; silence if it did */
        }
        a->OutRef[i].Count = (UCHAR)min(g_Agg.Lay.DestCount[lo], (UCHAR)APOLLO_AGG_MAX_DESTS);
        for (d = 0; d < a->OutRef[i].Count; d++) {
            a->OutRef[i].Unit[d] = g_Agg.Lay.Dest[lo][d].Unit;
            a->OutRef[i].Slot[d] = g_Agg.Lay.Dest[lo][d].Slot;
        }
    }
    g_Agg.StageDirty = TRUE;                /* mapped at the next tick, before any writer */
}

static LONGLONG AggQpcAt(_In_ PDEVICE_CONTEXT Tl, _In_ ULONGLONG Frame)
{
    LONGLONG back = (LONGLONG)(Tl->EngPos - Frame);

    return Tl->EngQpc - back * Tl->EngQpcFreq / (LONGLONG)(Tl->SampleRate != 0 ? Tl->SampleRate : 48000);   /* [Rate] */
}

static BOOLEAN AggClientDid(_In_ AGG_ASIO *A, _In_ ULONG Half)
{
    return ReadNoFence(&A->Shared->ClientDone[Half]) == A->PubSwitch[Half];
}

static VOID AggAsioSync(_In_ PDEVICE_CONTEXT Tl, _In_ ULONG T, _In_ ULONGLONG Head)
{
    AGG_ASIO *a = &g_Agg.Asio;
    const APOLLO_AGG_UNIT_RT *rt = &g_Agg.Rt[T];

    a->Pad = g_Agg.Lay.Pad;
    a->Origin = Head;
    a->Delay = 2ULL * a->N + rt->Guard + a->Pad + rt->Lead;
    a->CapNext = 0;
    a->RenderKnown = FALSE;
    a->RenderPlay = FALSE;
    a->PubPeriod[0] = a->PubPeriod[1] = AGG_NO_PERIOD;
    a->SampleOffset = a->NextSample;
    a->DataHead = Tl->EngPos + rt->Lead;
    a->Epoch = g_Agg.TimelineEpoch;
    a->NeedSync = FALSE;
    a->Shared->InputLatencyFrames = g_Agg.Lay.InLatency + g_Agg.InExtra;      /* [Lat] */
    a->Shared->OutputLatencyFrames = g_Agg.Lay.OutLatency + g_Agg.OutExtra;
    a->Shared->Reserved[1] = g_Agg.TimelineEpoch;
    /*
     * Incremental, at the next tick: the combined Windows streams may share the staging rings,
     * and the timeline is the same (an epoch change maps fresh in AggStageUpdate).
     */
    g_Agg.StageDirty = TRUE;
}

/*
 * Capture up to Head (timeline frames). Timeline channels are copied from its ring; follower
 * channels are the resampler's value at the mapped position (spec 6.4).
 */
static BOOLEAN AggAsioCapture(_In_ PDEVICE_CONTEXT Tl, _In_ ULONG T, _In_ ULONGLONG Head)
{
    AGG_ASIO *a = &g_Agg.Asio;
    PAPOLLO_ASIO_SHARED sh = a->Shared;
    const APOLLO_ASRC_TABLE *tab = g_Agg.Asrc;
    ULONG h = tab->Half, nmask, c, u;
    float w[APOLLO_AGG_MAX_UNITS][2 * APOLLO_ASRC_MAX_HALF];
    LONGLONG base[APOLLO_AGG_MAX_UNITS] = { 0 };
    BOOLEAN live[APOLLO_AGG_MAX_UNITS], signal = FALSE;
    PDEVICE_CONTEXT fc[APOLLO_AGG_MAX_UNITS];
    ULONGLONG end, done = 0;

    if (a->NeedSync || Head < a->Origin || Head - a->Origin - a->CapNext > AGG_MAX_LAG) {
        if (!a->NeedSync) {
            a->Resyncs++;
        }
        AggAsioSync(Tl, T, Head);
        return FALSE;
    }
    for (u = 0; u < APOLLO_AGG_MAX_UNITS; u++) {
        fc[u] = (g_Agg.U[u].Usable && ApolloAggResampled(u, T)) ? AggCtx(u) : NULL;
    }
    nmask = a->N - 1;
    end = Head - a->Origin;
    while (a->CapNext < end && done < AGG_MAX_LAG) {
        ULONGLONG f = a->CapNext, x = a->Origin + f;
        ULONGLONG period = f >> a->Shift;
        ULONG i = (ULONG)(f & nmask), half = (ULONG)(period & 1);
        LONG *dst = a->In + (SIZE_T)half * a->InCount * a->N + i;

        /* per follower: the weights for this frame, once */
        for (u = 0; u < APOLLO_AGG_MAX_UNITS; u++) {
            AGG_UNIT *U = &g_Agg.U[u];
            PDEVICE_CONTEXT f8 = fc[u];

            live[u] = FALSE;
            if (f8 == NULL || !U->Formed || U->Clock.State != APOLLO_AGG_CLOCK_LOCKED) {
                U->GainCap = 0;
                continue;
            }
            {
                double z = ApolloAggMap(&U->Clock, (LONGLONG)x) + (double)U->CapOff;
                LONGLONG n = ApolloAggFloor(z);
                LONGLONG pf = ApolloAggFloor(U->PfAtTm);

                if (n + (LONGLONG)h > pf - (LONGLONG)g_Agg.Rt[u].Guard ||
                    n - (LONGLONG)h + 1 < pf - (LONGLONG)f8->RingFrames + 64) {
                    U->CaptureShort++;
                    continue;
                }
                ApolloAsrcWeights(tab, z - (double)n, w[u]);
                base[u] = n - (LONGLONG)h + 1;
                live[u] = TRUE;
                if (U->GainCap < 1.0) {
                    U->GainCap += 1.0 / (double)ApolloRateScale((ULONG)AGG_FADE_FRAMES, Tl->SampleRate);  /* [Rate] 10 ms */
                    if (U->GainCap > 1.0) {
                        U->GainCap = 1.0;
                    }
                }
            }
        }
        if (i == 0 && a->PubPeriod[half] != AGG_NO_PERIOD && !AggClientDid(a, half)) {
            a->Overruns++;
            WriteNoFence(&sh->Overruns, a->Overruns);
        }
        for (c = 0; c < a->InCount; c++) {
            const AGG_IREF *r = &a->InRef[c];
            LONG v = 0;

            if (r->Valid && !a->Stale) {
                u = r->Unit;
                if (!ApolloAggResampled(u, T)) {
                    /* the timeline's channels: an integer copy (apollo_agg.h ApolloAggRingSample) */
                    v = ApolloAggRingSample((const LONG *)Tl->Rec.Va, Tl->RingFrames, Tl->RecChannels, r->Slot, x);
                } else if (u < APOLLO_AGG_MAX_UNITS && live[u] && r->Slot < fc[u]->RecChannels) {
                    v = ApolloAggAsrcSample((const LONG *)fc[u]->Rec.Va, fc[u]->RingFrames, fc[u]->RecChannels, r->Slot,
                                            base[u], w[u], 2 * h, (float)g_Agg.U[u].GainCap);
                }
            }
            dst[(SIZE_T)c * a->N] = v;
        }
        a->CapNext = f + 1;
        done++;
        if (i == nmask) {
            LONG s = ++a->Switches;

            a->PubPeriod[half] = period;
            a->PubSwitch[half] = s;
            InterlockedIncrement(&sh->Sequence);
            WriteNoFence(&sh->HalfReady, (LONG)half);
            WriteNoFence(&sh->SwitchCount, s);
            WriteNoFence64(&sh->SamplePosition, (LONG64)(a->SampleOffset + (period << a->Shift)));
            WriteNoFence64(&sh->Qpc, AggQpcAt(Tl, a->Origin + (period << a->Shift)));
            InterlockedIncrement(&sh->Sequence);
            a->NextSample = a->SampleOffset + ((period + 1) << a->Shift);
            signal = TRUE;
        }
    }
    return signal;
}

/* Render the client's outputs for timeline frames [DataHead, To): timeline slots and staging. */
static VOID AggAsioRender(_In_ PDEVICE_CONTEXT Tl, _In_ ULONG T, _In_ ULONGLONG To)
{
    AGG_ASIO *a = &g_Agg.Asio;
    ULONG tmask = Tl->RingFrames - 1, tstride = Tl->PlayChannels * 4, nmask = a->N - 1, c, d;
    ULONGLONG from = a->DataHead, start = a->Origin + a->Delay, h;

    if (from < Tl->EngPos + g_Agg.Rt[T].FrameCfg) {
        from = Tl->EngPos + g_Agg.Rt[T].FrameCfg;   /* late: those frames are gone */
    }
    if (To <= from) {
        return;
    }
    if (To - from > AGG_STAGE_FRAMES / 2) {
        from = To - AGG_STAGE_FRAMES / 2;
    }
    /* staging was zeroed up to To by AggStageUpdate, before any writer: the client's samples ADD */
    for (h = max(from, start); h < To; h++) {
        ULONGLONG f = h - start;
        ULONGLONG period = f >> a->Shift;
        ULONG i = (ULONG)(f & nmask), half = (ULONG)(period & 1);
        LONG *tframe;
        const LONG *src;

        if (!a->RenderKnown || period != a->RenderPeriod) {
            a->RenderKnown = TRUE;
            a->RenderPeriod = period;
            a->RenderPlay = a->PubPeriod[half] == period && AggClientDid(a, half) && !a->Stale;
            if (!a->RenderPlay) {
                a->Underruns++;
                WriteNoFence(&a->Shared->Underruns, a->Underruns);
            }
        }
        if (!a->RenderPlay) {
            continue;
        }
        tframe = (LONG *)(Tl->Play.Va + (SIZE_T)(h & tmask) * tstride);
        src = a->Out + (SIZE_T)half * a->OutCount * a->N + i;
        for (c = 0; c < a->OutCount; c++) {
            const AGG_OREF *r = &a->OutRef[c];
            LONG v = src[(SIZE_T)c * a->N];

            for (d = 0; d < r->Count && d < APOLLO_AGG_MAX_DESTS; d++) {
                ULONG u = r->Unit[d], slot = r->Slot[d];

                if (!ApolloAggResampled(u, T)) {
                    if (slot < Tl->PlayChannels) {
                        tframe[slot] = ApolloSatAdd32(tframe[slot], v);     /* the timeline's ring, directly */
                    }
                } else if (u < APOLLO_AGG_MAX_UNITS && slot < APOLLO_AGG_UNIT_SLOTS && g_Agg.U[u].StageOf[slot] >= 0) {
                    PLONG p = g_Agg.Stage + (SIZE_T)g_Agg.U[u].StageOf[slot] * AGG_STAGE_FRAMES + (h & AGG_STAGE_MASK);

                    *p = ApolloSatAdd32(*p, v);
                }
            }
        }
    }
    a->DataHead = To;
}

/* Follower u's playback ring from the staging rings, through the resampler (spec 6.4). */
static VOID AggFollowerRender(_In_ ULONG U)
{
    AGG_UNIT *un = &g_Agg.U[U];
    PDEVICE_CONTEXT f8 = AggCtx(U);
    const APOLLO_ASRC_TABLE *tab = g_Agg.Asrc;
    const APOLLO_AGG_UNIT_RT *rt = &g_Agg.Rt[U];
    ULONG h = tab->Half, fmask, fstride, s;
    LONGLONG pf, target, k, oldest, newest = (LONGLONG)g_Agg.RenHead;   /* every writer's frames, not ASIO's only */
    LONG lead;
    float w[2 * APOLLO_ASRC_MAX_HALF];

    if (f8 == NULL) {
        return;
    }
    fmask = f8->RingFrames - 1;
    fstride = f8->PlayChannels * 4;
    pf = ApolloAggFloor(un->PfAtTm);
    lead = (LONG)rt->Lead - (LONG)h - (LONG)ApolloAggMarginAt(rt->Rate) - 2;     /* [Rate] M at the rate */
    if (lead < (LONG)rt->FrameCfg + 16) {
        lead = (LONG)rt->FrameCfg + 16;
    }
    target = pf + lead;
    if (un->RenderHead < pf + (LONGLONG)rt->FrameCfg + 4) {
        if (un->RenderHead != 0) {
            un->RenderLate += (ULONG)min(pf + (LONGLONG)rt->FrameCfg + 4 - un->RenderHead, (LONGLONG)MAXULONG);
        }
        un->RenderHead = pf + (LONGLONG)rt->FrameCfg + 4;
    }
    if (target - un->RenderHead > (LONGLONG)AGG_STAGE_FRAMES / 2) {
        un->RenderHead = target - (LONGLONG)AGG_STAGE_FRAMES / 2;
    }
    /* [Corr] never into frames speaker correction already processed on this ring (corr.c; unchanged while off) */
    un->RenderHead = (LONGLONG)ApolloCorrWriteFloor(f8, (ULONGLONG)un->RenderHead);
    oldest = newest - (LONGLONG)AGG_STAGE_FRAMES + 64;
    for (k = un->RenderHead; k < target; k++) {
        double y = ApolloAggUnmap(&un->Clock, (double)k) + (double)un->RenOff;     /* RenderTrim */
        LONGLONG n = ApolloAggFloor(y), b = n - (LONGLONG)h + 1;
        LONG *frame;

        if (n + (LONGLONG)h >= newest) {
            break;                          /* the client's staging is not that far yet: next tick */
        }
        if (un->Clock.State != APOLLO_AGG_CLOCK_LOCKED || b < oldest) {
            un->GainRen = 0;
            continue;                       /* muted (acquiring) or too old: silence */
        }
        if (un->GainRen < 1.0) {
            un->GainRen += 1.0 / (double)ApolloRateScale((ULONG)AGG_FADE_FRAMES, f8->SampleRate);   /* [Rate] 10 ms */
            if (un->GainRen > 1.0) {
                un->GainRen = 1.0;
            }
        }
        ApolloAsrcWeights(tab, y - (double)n, w);
        frame = (LONG *)(f8->Play.Va + (SIZE_T)((ULONGLONG)k & fmask) * fstride);
        for (s = 0; s < g_Agg.StageCount; s++) {
            ULONG slot;

            if (g_Agg.StageUnit[s] != U) {
                continue;
            }
            slot = g_Agg.StageSlot[s];
            if (slot >= f8->PlayChannels) {
                continue;
            }
            /* one staging ring = one channel of AGG_STAGE_FRAMES frames */
            frame[slot] = ApolloSatAdd32(frame[slot], ApolloAggAsrcSample(g_Agg.Stage + (SIZE_T)s * AGG_STAGE_FRAMES,
                                                                          AGG_STAGE_FRAMES, 1, 0, b, w, 2 * h,
                                                                          (float)un->GainRen));
        }
    }
    un->RenderHead = k;
}

/*
 * A new timeline epoch (Agg.Lock): every follower re-forms, the ASIO client re-originates and its
 * host is asked to reset (the DLL watches Reserved[1]); AggStageUpdate maps staging fresh and
 * re-anchors the combined streams (their positions continue). On a takeover, a new timeline from a
 * rebuild, the timeline unit's own restart and a rate change.
 */
static VOID AggNewEpoch(VOID)
{
    ULONG k;

    g_Agg.TimelineEpoch++;
    g_Agg.TlRunValid = FALSE;               /* the next tick only records the (new) timeline's run */
    for (k = 0; k < APOLLO_AGG_MAX_UNITS; k++) {
        g_Agg.U[k].Formed = FALSE;
    }
    g_Agg.Asio.NeedSync = TRUE;
    if (g_Agg.Asio.Open) {
        g_Agg.Asio.Shared->Reserved[1] = g_Agg.TimelineEpoch;
    }
}

/*
 * The calling unit (a member) takes the timeline (Agg.Lock, DISPATCH). Returns:
 *   1  the timeline stopped running: the caller becomes acting master (spec 6.3.4);
 *   2  the caller is the configured master, it runs again (an engine recovered with
 *      TRANSPORT_START, a transport started by hand) and has done so for AGG_RECLAIM_TICKS of its own
 *      ticks while another unit held the timeline: it takes its timeline back;
 *   0  nothing changes.
 * 1 and 2 bump the epoch: the ASIO host gets kAsioResetRequest (the DLL watches Reserved[1]), the
 * combined Windows streams continue with their positions rebased (AggStageUpdate).
 */
static ULONG AggTakeOver(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONG u, why;
    PDEVICE_CONTEXT tl = g_Agg.Timeline;
    BOOLEAN tlRuns;

    for (u = 0; u < g_Agg.Cfg.UnitCount && u < APOLLO_AGG_MAX_UNITS; u++) {
        if (AggCtx(u) == Ctx) {
            break;
        }
    }
    if (u >= g_Agg.Cfg.UnitCount || u >= APOLLO_AGG_MAX_UNITS) {
        return 0;
    }
    tlRuns = (tl != NULL && AggCtxRunning(tl)) ? TRUE : FALSE;
    why = ApolloAggTakeOverWhy(tlRuns, u == g_Agg.Cfg.Master, tlRuns ? AggCtxRunning(Ctx) : FALSE, AGG_RECLAIM_TICKS,
                               &g_Agg.U[u].Reclaim);
    if (why == 0) {
        return 0;
    }
    g_Agg.Timeline = Ctx;
    g_Agg.TimelineUnit = (LONG)u;
    AggNewEpoch();
    return why;
}

/* ------------------------------------------------------------------ */
/* Combined Windows streams in the tick (Agg.Lock + EngineLocks)      */
/* ------------------------------------------------------------------ */

/*
 * The staging rings for this tick, before any writer: a fresh map when the timeline changed (or
 * nothing ran last tick), an incremental one when a client came, went or was re-routed; then zero
 * every ring in use from the last head to To, so each writer only ADDs.
 */
static VOID AggStageUpdate(_In_ ULONGLONG To)
{
    ULONGLONG from, f;
    ULONG s;

    /* a head beyond To is another run's frames (the epoch normally says so first) */
    if (!g_Agg.StageValid || g_Agg.StageEpoch != g_Agg.TimelineEpoch || g_Agg.RenHead > To) {
        PLIST_ENTRY e;

        AggStageMap(TRUE);
        g_Agg.StageValid = TRUE;
        g_Agg.StageEpoch = g_Agg.TimelineEpoch;
        g_Agg.StageDirty = FALSE;
        g_Agg.RenHead = 0;
        /* another clock's frames from here: every combined stream re-anchors, its position continues */
        for (e = g_Agg.Streams.Flink; e != &g_Agg.Streams; e = e->Flink) {
            CONTAINING_RECORD(e, APOLLO_STREAM, Link)->NeedSync = TRUE;
        }
    } else if (g_Agg.StageDirty) {
        AggStageMap(FALSE);
        g_Agg.StageDirty = FALSE;
    }
    from = ApolloAggStageZeroFrom(g_Agg.RenHead, To, AGG_STAGE_FRAMES);
    for (s = 0; s < g_Agg.StageCount && s < AGG_STAGE_CH; s++) {
        PLONG ring = g_Agg.Stage + (SIZE_T)s * AGG_STAGE_FRAMES;

        if (g_Agg.StageUnit[s] == AGG_STAGE_NONE) {
            continue;
        }
        for (f = from; f < To; f++) {
            ring[f & AGG_STAGE_MASK] = 0;
        }
    }
}

/*
 * One combined render stream for timeline frames [From, To): stream.c StreamRenderOne with the
 * routing: each channel ADDs (saturating) into every destination of its windows route, the
 * timeline's ring directly, a follower's through its staging ring and the resampler.
 */
static VOID AggStreamRender(_In_ PDEVICE_CONTEXT Tl, _In_ ULONG T, _Inout_ PAPOLLO_STREAM S, _In_ ULONGLONG From,
                            _In_ ULONGLONG To)
{
    ULONG tmask = Tl->RingFrames - 1, tstride = Tl->PlayChannels * 4, idx, c, d;
    LONGLONG next;
    ULONGLONG h, frame, packetEnd = 0;      /* the next packet's first frame: one division per packet */
    BOOLEAN play = FALSE;
    BOOLEAN stale = (S->Rate != Tl->SampleRate) ? TRUE : FALSE;    /* [Rate] another rate: silent until reopened */

    if (S->BufferFrames == 0 || S->FramesPerPacket == 0) {
        return;
    }
    if (S->NeedSync) {
        S->HwBase = (LONGLONG)From - (LONGLONG)S->Frames;  /* stream frame Frames lands on From */
        S->NeedSync = FALSE;
    }
    next = S->HwBase + (LONGLONG)S->Frames;
    if (next < (LONGLONG)From) {
        /* a late tick: those frames are gone for this stream too */
        S->Ep->Dropped += (ULONG)min((ULONGLONG)((LONGLONG)From - next), (ULONGLONG)MAXULONG);
        S->Frames = (ULONGLONG)((LONGLONG)From - S->HwBase);
        next = (LONGLONG)From;
    }
    if ((LONGLONG)To <= next) {
        return;
    }
    frame = S->Frames;
    idx = (ULONG)(frame % S->BufferFrames);
    for (h = (ULONGLONG)next; h < To; h++, frame++) {
        if (frame >= packetEnd) {
            ULONGLONG q = frame / S->FramesPerPacket;

            packetEnd = (q + 1) * S->FramesPerPacket;
            play = !S->UsesWritePacket || (LONGLONG)q <= S->WrittenPacket;
            if (!play && S->UnderrunPacket != q + 1) {
                S->UnderrunPacket = q + 1;      /* count each late packet once */
                S->Ep->Underruns++;
            }
        }
        if (play && !stale && frame < S->EosFrame) {
            const LONG *src = (const LONG *)(S->Buffer + (SIZE_T)idx * S->BlockAlign);
            LONG *tframe = (LONG *)(Tl->Play.Va + (SIZE_T)(h & tmask) * tstride);

            for (c = 0; c < S->Channels && c < 2; c++) {
                for (d = 0; d < S->AggRef.Count[c] && d < APOLLO_AGG_MAX_DESTS; d++) {
                    ULONG u = S->AggRef.Unit[c][d], slot = S->AggRef.Slot[c][d];

                    if (!ApolloAggResampled(u, T)) {
                        if (slot < Tl->PlayChannels) {
                            tframe[slot] = ApolloSatAdd32(tframe[slot], src[c]);    /* the timeline's ring, directly */
                        }
                    } else if (u < APOLLO_AGG_MAX_UNITS && slot < APOLLO_AGG_UNIT_SLOTS && g_Agg.U[u].StageOf[slot] >= 0) {
                        PLONG p = g_Agg.Stage + (SIZE_T)g_Agg.U[u].StageOf[slot] * AGG_STAGE_FRAMES + (h & AGG_STAGE_MASK);

                        *p = ApolloSatAdd32(*p, src[c]);
                    }
                }
            }
        }
        if (++idx == S->BufferFrames) {
            idx = 0;
        }
    }
    S->Frames = frame;
    ApolloStreamComplete(S);                    /* apollo_stream.h */
}

/* The resampler's weights for one (unit, timeline frame), shared by the channels of a frame. */
typedef struct _AGG_WCACHE {
    LONG        Unit;               /* -1: nothing cached */
    ULONGLONG   X;
    BOOLEAN     Live;
    LONGLONG    Base;
    float       Gain;
    float       W[2 * APOLLO_ASRC_MAX_HALF];
} AGG_WCACHE;

/*
 * One captured sample of (config unit U, record slot Slot) for timeline frame X: the timeline's ring
 * directly, a locked follower through the resampler at its mapped position (AggAsioCapture's rule,
 * spec 6.4), anything else (absent, acquiring, not recorded yet) zero. A follower fades in over
 * AGG_FADE_S after its lock (from the loop's lock time: no per-client state).
 */
static LONG AggSampleAt(_In_ PDEVICE_CONTEXT Tl, _In_ ULONG T, _In_ ULONG U, _In_ ULONG Slot, _In_ ULONGLONG X,
                        _Inout_ AGG_WCACHE *Wc)
{
    const APOLLO_ASRC_TABLE *tab = g_Agg.Asrc;
    PDEVICE_CONTEXT f8;
    ULONG h;

    if (!ApolloAggResampled(U, T)) {
        /* the timeline's channels: an integer copy (apollo_agg.h ApolloAggRingSample) */
        return ApolloAggRingSample((const LONG *)Tl->Rec.Va, Tl->RingFrames, Tl->RecChannels, Slot, X);
    }
    if (U >= APOLLO_AGG_MAX_UNITS || !g_Agg.U[U].Usable) {
        return 0;
    }
    f8 = AggCtx(U);
    if (f8 == NULL || Slot >= f8->RecChannels) {
        return 0;
    }
    h = tab->Half;
    if (Wc->Unit != (LONG)U || Wc->X != X) {
        AGG_UNIT *un = &g_Agg.U[U];

        Wc->Unit = (LONG)U;
        Wc->X = X;
        Wc->Live = FALSE;
        if (un->Formed && un->Clock.State == APOLLO_AGG_CLOCK_LOCKED) {
            double z = ApolloAggMap(&un->Clock, (LONGLONG)X) + (double)un->CapOff;
            LONGLONG n = ApolloAggFloor(z), pf = ApolloAggFloor(un->PfAtTm);

            if (n + (LONGLONG)h > pf - (LONGLONG)g_Agg.Rt[U].Guard || n - (LONGLONG)h + 1 < pf - (LONGLONG)f8->RingFrames + 64) {
                un->CaptureShort++;
            } else {
                double g = un->Clock.Locked / AGG_FADE_S;

                ApolloAsrcWeights(tab, z - (double)n, Wc->W);
                Wc->Base = n - (LONGLONG)h + 1;
                Wc->Gain = (float)(g > 1.0 ? 1.0 : g);
                Wc->Live = TRUE;
            }
        }
    }
    if (!Wc->Live) {
        return 0;
    }
    return ApolloAggAsrcSample((const LONG *)f8->Rec.Va, f8->RingFrames, f8->RecChannels, Slot, Wc->Base, Wc->W, 2 * h,
                               Wc->Gain);
}

/* One combined capture stream up to Head (timeline frames, already aligned like the ASIO client's). */
static VOID AggStreamCapture(_In_ PDEVICE_CONTEXT Tl, _In_ ULONG T, _Inout_ PAPOLLO_STREAM S, _In_ ULONGLONG Head)
{
    ULONG idx, c;
    LONGLONG next;
    ULONGLONG x, frame;
    AGG_WCACHE wc;
    BOOLEAN stale = (S->Rate != Tl->SampleRate) ? TRUE : FALSE;    /* [Rate] another rate: records silence */

    if (S->BufferFrames == 0 || S->FramesPerPacket == 0 || S->PacketCount == 0) {
        return;
    }
    if (S->NeedSync) {
        S->HwBase = (LONGLONG)Head - (LONGLONG)S->Frames;  /* capture starts now */
        S->NeedSync = FALSE;
        return;
    }
    next = S->HwBase + (LONGLONG)S->Frames;
    if ((LONGLONG)Head < next || (ULONGLONG)((LONGLONG)Head - next) > AGG_MAX_LAG) {
        /* a stall longer than APOLLO_AGG_MAX_LAG: skip to now, never a long catch-up here */
        S->Ep->Overruns++;
        S->HwBase = (LONGLONG)Head - (LONGLONG)S->Frames;
        return;
    }
    wc.Unit = -1;
    wc.X = 0;
    wc.Live = FALSE;
    frame = S->Frames;
    idx = (ULONG)(frame % S->BufferFrames);
    for (x = (ULONGLONG)next; x < Head; x++, frame++) {
        LONG *dst = (LONG *)(S->Buffer + (SIZE_T)idx * S->BlockAlign);

        for (c = 0; c < S->Channels && c < 2; c++) {
            dst[c] = (S->AggRef.Count[c] != 0 && !stale) ?
                     AggSampleAt(Tl, T, S->AggRef.Unit[c][0], S->AggRef.Slot[c][0], x, &wc) : 0;
        }
        if (++idx == S->BufferFrames) {
            idx = 0;
        }
    }
    S->Frames = frame;
    while (S->Frames >= (S->Completed + 1) * S->FramesPerPacket) {
        ULONGLONG p = S->Completed;

        S->PacketQpc[p % S->PacketCount] = AggQpcAt(Tl, (ULONGLONG)(S->HwBase + (LONGLONG)(p * S->FramesPerPacket)));
        S->Completed++;
        S->Ep->Packets++;
    }
    if (S->ReadStarted && S->PacketCount > 1) {
        ULONGLONG oldest = ApolloStreamOldestIntact(S);    /* apollo_stream.h */

        if (S->ReadNext < oldest) {
            S->Ep->Overruns += (ULONG)min(oldest - S->ReadNext, (ULONGLONG)MAXULONG);
            S->ReadNext = oldest;
            if (S->Notified < S->ReadNext) {
                S->Notified = S->ReadNext;
            }
        }
    }
}

/* Completed packets to notify, in order, each under the stream's rundown (engine.c's rule). */
static ULONG AggStreamsCollect(_Out_writes_to_(Max, return) PAPOLLO_NOTIFY List, _In_ ULONG Max)
{
    PLIST_ENTRY e;
    ULONG n = 0;

    for (e = g_Agg.Streams.Flink; e != &g_Agg.Streams && n < Max; e = e->Flink) {
        PAPOLLO_STREAM s = CONTAINING_RECORD(e, APOLLO_STREAM, Link);

        while (n < Max && s->Notified < s->Completed && ExAcquireRundownProtection(&s->Rundown)) {
            List[n].Stream = s;
            List[n].Packet = s->Notified++;
            n++;
        }
    }
    return n;
}

/* The start ratio of follower U against timeline T: this run's lock, else the kept one, else 1. */
static double AggStartRatio(_In_ ULONG U, _In_ ULONG T, _In_ ULONG Rate, _Out_ PULONG Src)
{
    AGG_UNIT *un = &g_Agg.U[U];
    ULONGLONG tl = g_Agg.Cfg.Unit[T].Uid;
    double r = 1.0;

    /* [Rate] this run's ratio at the same rate only; the kept ones are the 48 kHz family's */
    if (un->LastRValid && un->LastRTl == tl && un->LastRRate == Rate) {
        *Src = 1;
        return un->LastR;
    }
    if (ApolloRateFamily48(Rate) &&
        ApolloAggRatioFind(g_Agg.Ratio, APOLLO_AGG_RATIOS, g_Agg.Cfg.Unit[U].Uid, tl, &r) != 0) {
        *Src = 2;
        return r;
    }
    *Src = 0;
    return 1.0;
}

_Use_decl_annotations_
VOID ApolloAggAfterTick(PDEVICE_CONTEXT Ctx)
{
    LARGE_INTEGER q0, q1;
    BOOLEAN held[APOLLO_AGG_MAX_UNITS], signal = FALSE;
    ULONG i, u, t, take = 0, notes = 0, formedSrc = 0;
    LONG formedU = -1, formedPpb = 0, takeU = -1;
    BOOLEAN restarted = FALSE;              /* the timeline unit's own new run: new epoch */
    PDEVICE_CONTEXT tl;
    LONGLONG freq, nqpc = 0;
    PKEVENT event = NULL;
    APOLLO_NOTIFY note[APOLLO_NOTIFY_MAX];  /* combined stream completions, sent after the locks */

    if (!g_Agg.Ready || ReadNoFence(&g_Agg.Active) == 0) {
        return;
    }
    KeAcquireSpinLockAtDpcLevel(&g_Agg.Lock);
    if (g_Agg.Active == 0 || g_Agg.RateChanging) {
        /* [Rate] ApolloAggRateSet: the units stop and start one after the other; nothing runs meanwhile */
        KeReleaseSpinLockFromDpcLevel(&g_Agg.Lock);
        return;
    }
    if (g_Agg.Timeline != Ctx) {
        take = AggTakeOver(Ctx);
        if (take == 0) {
            KeReleaseSpinLockFromDpcLevel(&g_Agg.Lock);
            return;
        }
        takeU = g_Agg.TimelineUnit;
    }
    if (g_Agg.TimelineUnit < 0) {
        KeReleaseSpinLockFromDpcLevel(&g_Agg.Lock);
        return;
    }
    q0 = KeQueryPerformanceCounter(NULL);
    t = (ULONG)g_Agg.TimelineUnit;
    tl = Ctx;
    RtlZeroMemory(held, sizeof(held));
    for (i = 0; i < g_Agg.LockCount && i < APOLLO_AGG_MAX_UNITS; i++) {
        LONG j;

        u = g_Agg.LockOrder[i];
        if (u >= APOLLO_AGG_MAX_UNITS) {
            continue;
        }
        j = g_Agg.U[u].Joined;
        if (j < 0 || j >= APOLLO_AGG_MAX_UNITS || g_Agg.Joined[j].Ctx == NULL) {
            continue;
        }
        if (ExAcquireRundownProtection(&g_Agg.Joined[j].Rundown)) {
            held[u] = TRUE;
            KeAcquireSpinLockAtDpcLevel(&g_Agg.Joined[j].Ctx->EngineLock);
        }
    }
    g_Agg.Ticks++;
    freq = tl->EngQpcFreq;
    g_Agg.TlRate = tl->SampleRate;
    g_Agg.RateMismatch = 0;
    for (u = 0; u < g_Agg.Cfg.UnitCount && u < APOLLO_AGG_MAX_UNITS; u++) {
        PDEVICE_CONTEXT m = held[u] ? AggCtx(u) : NULL;

        g_Agg.U[u].Usable = (m != NULL && AggCtxRunning(m)) ? TRUE : FALSE;
        if (g_Agg.U[u].Usable && m->SampleRate != tl->SampleRate) {
            /* [Rate] another rate than the timeline's: not used (its channels silent) until the rates agree */
            g_Agg.U[u].Usable = FALSE;
            g_Agg.RateMismatch |= 1u << u;
        }
        if (g_Agg.U[u].Usable) {
            g_Agg.U[u].Pos = m->EngPos;
            g_Agg.U[u].Qpc = m->EngQpc;
        }
    }
    if (held[t] && g_Agg.U[t].Usable && freq > 0) {
        LONGLONG tm = tl->EngQpc, pm = (LONGLONG)tl->EngPos;
        const APOLLO_AGG_UNIT_RT *trt = &g_Agg.Rt[t];

        /*
         * The timeline unit began a new run without a takeover (its transport restarted while no
         * other unit ran): every frame number the clients hold is another run's. A new epoch, as on a
         * takeover: fresh staging, combined streams re-anchored, followers re-formed, ASIO reset request.
         */
        if (ApolloAggTimelineRestarted(g_Agg.TlRunValid, g_Agg.TlRunUnit, t, g_Agg.TlLastPm, (ULONGLONG)pm,
                                       g_Agg.TlLastStarts, tl->EngStarts)) {
            AggNewEpoch();
            restarted = TRUE;
        }
        g_Agg.TlRunValid = TRUE;
        g_Agg.TlRunUnit = t;
        g_Agg.TlLastPm = (ULONGLONG)pm;
        g_Agg.TlLastStarts = tl->EngStarts;
        /* the timeline as the combined streams' position queries see it (stream.c, Agg.Lock) */
        g_Agg.TlValid = TRUE;
        g_Agg.TlPos = (ULONGLONG)pm;
        g_Agg.TlQpc = tm;
        g_Agg.TlFreq = freq;
        nqpc = tm;
        /* followers: the clock map at this instant */
        for (u = 0; u < g_Agg.Cfg.UnitCount && u < APOLLO_AGG_MAX_UNITS; u++) {
            AGG_UNIT *un = &g_Agg.U[u];
            LONGLONG age;

            if (u == t || !un->Usable) {
                if (u != t && un->Formed) {
                    un->Formed = FALSE;     /* gone or stopped: re-form when it runs again */
                    un->GainCap = un->GainRen = 0;
                }
                continue;
            }
            age = tm - un->Qpc;
            if (age > freq * AGG_STALE_MS / 1000 || -age > freq * AGG_STALE_MS / 1000) {
                un->Usable = FALSE;         /* its engine missed ticks: skip it this time */
                continue;
            }
            un->PfAtTm = (double)un->Pos + (double)age * (double)tl->SampleRate / (double)freq;   /* [Rate] */
            /* Align / trims (apollo_agg.h ApolloAggCapOffset, ApolloAggRenOffset) */
            un->CapOff = ApolloAggCapOffset(&g_Agg.Cfg, g_Agg.Rt, t, u);
            un->RenOff = ApolloAggRenOffset(g_Agg.Rt, t, u, g_Agg.Cfg.AsrcHalf);  /* "earlier" clamped */
            if (!un->Formed) {
                ULONG src = 0;
                double r0 = AggStartRatio(u, t, tl->SampleRate, &src);

                un->Clock.Rate = (double)tl->SampleRate;     /* [Rate] the loop runs on the timeline's frames */
                ApolloAggClockForm(&un->Clock, pm, un->PfAtTm, r0);
                un->Formed = TRUE;
                un->GainCap = un->GainRen = 0;
                un->RenderHead = 0;
                if (formedU < 0) {
                    formedU = (LONG)u;
                    formedPpb = ApolloAggPpbOf(r0);
                    formedSrc = src;
                }
            } else {
                int ev = ApolloAggClockUpdate(&un->Clock, pm, un->PfAtTm);

                if (ev == APOLLO_AGG_EV_RESYNC || ev == APOLLO_AGG_EV_DEGRADED || ev == APOLLO_AGG_EV_REFORM) {
                    un->GainCap = un->GainRen = 0;
                }
                if (un->Clock.State == APOLLO_AGG_CLOCK_LOCKED) {
                    un->LastR = un->Clock.R;
                    un->LastRValid = TRUE;
                    un->LastRTl = g_Agg.Cfg.Unit[t].Uid;
                    un->LastRRate = tl->SampleRate;
                    /* keep it for the next load once it has held for 30 s (written by a service pass) */
                    /* [Rate] in the 48 kHz family only: the kept ratios are that family's */
                    if (un->Clock.Locked >= APOLLO_AGG_RATIO_SAVE_S && ApolloRateFamily48(tl->SampleRate) &&
                        (un->RatioCheckTick == 0 || g_Agg.Ticks - un->RatioCheckTick >= AGG_RATIO_CHECK_TICKS)) {
                        un->RatioCheckTick = g_Agg.Ticks;
                        if (ApolloAggRatioPut(g_Agg.Ratio, APOLLO_AGG_RATIOS, g_Agg.Cfg.Unit[u].Uid,
                                              g_Agg.Cfg.Unit[t].Uid, un->Clock.R, g_Agg.RatioSeq + 1) != 0) {
                            g_Agg.RatioSeq++;
                            InterlockedExchange(&g_Agg.RatioDirty, 1);
                        }
                    }
                }
            }
        }
        /* the clients: the combined ASIO client and the combined Windows streams */
        if ((g_Agg.Asio.Open && g_Agg.Asio.Active) || g_Agg.StreamCount != 0) {
            ULONGLONG to = (ULONGLONG)pm + trt->Lead, from;
            ULONGLONG head = (ULONGLONG)pm > trt->Guard + g_Agg.Lay.Pad ? (ULONGLONG)pm - trt->Guard - g_Agg.Lay.Pad : 0;
            PLIST_ENTRY e;

            AggStageUpdate(to);
            /* the combined streams render from the last head (or, late, from what the hardware has not fetched) */
            from = ApolloAggStreamFrom(g_Agg.RenHead, (ULONGLONG)pm, trt->FrameCfg, to);
            if (g_Agg.Asio.Open && g_Agg.Asio.Active) {
                if (g_Agg.Asio.Epoch != g_Agg.TimelineEpoch) {
                    g_Agg.Asio.NeedSync = TRUE;
                }
                if (!g_Agg.Asio.NeedSync) {
                    AggAsioRender(tl, t, to);
                }
                signal = AggAsioCapture(tl, t, head);
                if (signal) {
                    event = g_Agg.Asio.Map.Event;
                }
            }
            for (e = g_Agg.Streams.Flink; e != &g_Agg.Streams; e = e->Flink) {
                PAPOLLO_STREAM s = CONTAINING_RECORD(e, APOLLO_STREAM, Link);

                if (s->Render) {
                    AggStreamRender(tl, t, s, from, to);
                } else {
                    AggStreamCapture(tl, t, s, head);
                }
            }
            notes = AggStreamsCollect(note, APOLLO_NOTIFY_MAX);
            g_Agg.RenHead = to;             /* the followers read staging up to here */
            for (u = 0; u < g_Agg.Cfg.UnitCount && u < APOLLO_AGG_MAX_UNITS; u++) {
                if (u == t || !g_Agg.U[u].Usable || !g_Agg.U[u].Formed) {
                    continue;
                }
                if (g_Agg.StageRings[u] != 0) {
                    AggFollowerRender(u);
                } else {
                    g_Agg.U[u].RenderHead = 0;  /* nothing staged for it: start fresh when something is */
                }
            }
        } else {
            for (u = 0; u < APOLLO_AGG_MAX_UNITS; u++) {
                g_Agg.U[u].RenderHead = 0;
            }
            g_Agg.RenHead = 0;
            g_Agg.StageValid = FALSE;       /* the next client maps fresh */
        }
        /*
         * [Corr] speaker correction (corr.c) on each member's ring, under its EngineLock (held): a follower right
         * after this tick rendered it, bounded by how far that went (0 = nothing writes it from here); the timeline
         * unit unbounded (its own tick corrects it right after this returns, when every writer is done).
         */
        for (u = 0; u < g_Agg.Cfg.UnitCount && u < APOLLO_AGG_MAX_UNITS; u++) {
            PDEVICE_CONTEXT cm = held[u] ? AggCtx(u) : NULL;

            if (cm != NULL) {
                ApolloCorrAggFollower(cm, (u != t && g_Agg.U[u].Formed && g_Agg.StageRings[u] != 0) ?
                                          (ULONGLONG)g_Agg.U[u].RenderHead : 0);
            }
        }
    } else {
        g_Agg.TlValid = FALSE;
    }
    for (i = g_Agg.LockCount; i > 0; i--) {
        LONG j;

        u = g_Agg.LockOrder[i - 1];
        if (u >= APOLLO_AGG_MAX_UNITS || !held[u]) {
            continue;
        }
        j = g_Agg.U[u].Joined;
        KeReleaseSpinLockFromDpcLevel(&g_Agg.Joined[j].Ctx->EngineLock);
        ExReleaseRundownProtection(&g_Agg.Joined[j].Rundown);
    }
    q1 = KeQueryPerformanceCounter(NULL);
    if (freq > 0) {
        LONGLONG us = (q1.QuadPart - q0.QuadPart) * 1000000LL / freq;

        g_Agg.LastUs = (ULONG)min(us, (LONGLONG)MAXULONG);
        if (g_Agg.LastUs > g_Agg.MaxUs) {
            g_Agg.MaxUs = g_Agg.LastUs;
        }
    }
    if (event != NULL) {
        (VOID)KeSetEvent(event, EVENT_INCREMENT, FALSE);    /* still under Agg.Lock: the client cannot close */
    }
    KeReleaseSpinLockFromDpcLevel(&g_Agg.Lock);

    /* combined stream completions, every lock dropped (rundown protection held per entry) */
    if (notes != 0) {
        ApolloStreamsNotify(note, notes, nqpc);
    }
    if (take != 0) {
        ApolloLogU(Ctx, "agg: %s the timeline (unit %ld)", take == 2 ? "configured master running again: took back" :
                   "timeline unit stopped: acting master, took", takeU);
    }
    if (restarted) {
        ApolloLogU(Ctx, "agg: the timeline unit started a new run (its position went back or its engine restarted): "
                   "new epoch, clients re-anchored");
    }
    if (formedU >= 0) {
        ApolloLogPrint(AGG_TAG, "unit %ld formed against the timeline (unit %lu), start ratio %+ld ppb (%s)", formedU, t,
                       formedPpb, formedSrc == 1 ? "this run" : (formedSrc == 2 ? "kept in Parameters\\Aggregate\\Ratios" :
                       "none kept: 0"));
    }
}

/* ------------------------------------------------------------------ */
/* Combined Windows streams: the calls stream.c makes                 */
/* ------------------------------------------------------------------ */

/* The row of the combined table a stream's circuit is (endpoints.c sets AggRow1 = row + 1). */
static ULONG AggStreamRow(_In_ PAPOLLO_STREAM S)
{
    return (S->Ep != NULL && S->Ep->Desc != NULL && S->Ep->Desc->AggRow1 != 0) ? (ULONG)S->Ep->Desc->AggRow1 - 1 : MAXULONG;
}

static VOID AggStreamResolveOne(_Inout_ PAPOLLO_STREAM S)
{
    ULONG c;

    ApolloAggEpResolve(&g_Agg.Cfg, AggStreamRow(S), &S->AggRef);
    for (c = 0; c < 2; c++) {
        /* GET_ENDPOINTS shows the first source/destination slot (diagnostics only) */
        S->Slot[c] = S->AggRef.Count[c] != 0 ? S->AggRef.Slot[c][0] : 0;
    }
}

/* Every running combined stream after a configuration change (Agg.Lock). */
static VOID AggStreamsResolve(VOID)
{
    PLIST_ENTRY e;

    for (e = g_Agg.Streams.Flink; e != NULL && e != &g_Agg.Streams; e = e->Flink) {
        AggStreamResolveOne(CONTAINING_RECORD(e, APOLLO_STREAM, Link));
    }
}

/* The lock a combined stream's engine state is under (instead of the host's EngineLock). */
PKSPIN_LOCK ApolloAggStreamLock(VOID)
{
    return &g_Agg.Lock;
}

BOOLEAN ApolloAggStreamsReady(VOID)
{
    return g_Agg.Ready;
}

/* Run: Agg.Lock held. Its sources/destinations from the configuration, into the tick's list. */
_Use_decl_annotations_
VOID ApolloAggStreamAttach(PAPOLLO_STREAM S)
{
    AggStreamResolveOne(S);
    InsertTailList(&g_Agg.Streams, &S->Link);
    g_Agg.StreamCount++;
    if (S->Render) {
        g_Agg.StageDirty = TRUE;            /* its follower destinations get staging rings at the next tick */
    }
}

/* Pause / cleanup: Agg.Lock held, S in the list. The caller then waits for its rundown. */
_Use_decl_annotations_
VOID ApolloAggStreamDetach(PAPOLLO_STREAM S)
{
    RemoveEntryList(&S->Link);
    InitializeListHead(&S->Link);
    if (g_Agg.StreamCount != 0) {
        g_Agg.StreamCount--;
    }
    if (S->Render) {
        g_Agg.StageDirty = TRUE;
    }
}

/* Agg.Lock held: the timeline snapshot of the last tick that ran (FALSE: none). */
_Use_decl_annotations_
BOOLEAN ApolloAggStreamClock(PULONGLONG Pos, PLONGLONG Qpc, PLONGLONG Freq)
{
    *Pos = g_Agg.TlPos;
    *Qpc = g_Agg.TlQpc;
    *Freq = g_Agg.TlFreq;
    return g_Agg.TlValid && g_Agg.TlFreq > 0 && ReadNoFence(&g_Agg.Active) != 0;
}

/* PASSIVE: what a combined stream reports to Windows (StreamGetHwLatency). */
_Use_decl_annotations_
VOID ApolloAggStreamLatency(PULONG LeadFrames, PULONG GuardFrames, PULONG AdcFrames, PULONG InPathFrames,
                            PULONG DacFrames)
{
    KIRQL irql;
    LONG t;

    KeAcquireSpinLock(&g_Agg.Lock, &irql);
    t = g_Agg.TimelineUnit;
    if (t >= 0 && t < APOLLO_AGG_MAX_UNITS) {
        *LeadFrames = g_Agg.Rt[t].Lead;
        *GuardFrames = g_Agg.Rt[t].Guard + g_Agg.Lay.Pad;   /* the capture head trails by guard + pad */
        *AdcFrames = g_Agg.Rt[t].Adc;
        *InPathFrames = g_Agg.InExtra;                      /* [Lat] */
        *DacFrames = g_Agg.OutExtra;
    } else {
        *LeadFrames = APOLLO_DEF_RENDER_LEAD;
        *GuardFrames = APOLLO_DEF_CAPTURE_GUARD;
        *AdcFrames = APOLLO_X8P_ADC_FRAMES;
        *InPathFrames = 0;
        *DacFrames = 0;
    }
    KeReleaseSpinLock(&g_Agg.Lock, irql);
}

/*
 * [Lat] asio.c ApolloLatencyRefresh (DevLock of Ctx held, no EngineLock; lock order DevLock -> Agg.Lock): the unit's
 * strip path and DAC, into its member and the combined figures.
 */
_Use_decl_annotations_
VOID ApolloAggLatencyNote(PDEVICE_CONTEXT Ctx, ULONG InPath, ULONG Dac)
{
    KIRQL irql;
    ULONG j;

    if (!g_Agg.Ready) {
        return;
    }
    KeAcquireSpinLock(&g_Agg.Lock, &irql);
    for (j = 0; j < APOLLO_AGG_MAX_UNITS; j++) {
        if (g_Agg.Joined[j].Ctx == Ctx) {
            g_Agg.Joined[j].LatInPath = InPath;
            g_Agg.Joined[j].LatDac = Dac;
        }
    }
    AggLatencyExtras();
    KeReleaseSpinLock(&g_Agg.Lock, irql);
}

/* ------------------------------------------------------------------ */
/* The combined ASIO client (caller context, PASSIVE, Agg.Mutex)       */
/* ------------------------------------------------------------------ */

_IRQL_requires_(PASSIVE_LEVEL)
static VOID AggMapDestroy(_Inout_ AGG_MAP *M)
{
    if (M->UserVa != NULL && M->OwnerProcess != NULL) {
        HANDLE process = NULL;
        NTSTATUS status;

        if (PsGetCurrentProcess() == M->OwnerProcess) {
            status = ZwUnmapViewOfSection(ZwCurrentProcess(), M->UserVa);
        } else {
            status = ObOpenObjectByPointer(M->OwnerProcess, OBJ_KERNEL_HANDLE, NULL, 0x0008 /* PROCESS_VM_OPERATION */,
                                           *PsProcessType, KernelMode, &process);
            if (NT_SUCCESS(status)) {
                status = ZwUnmapViewOfSection(process, M->UserVa);
                ZwClose(process);
            }
        }
        if (!NT_SUCCESS(status)) {
            ApolloLogPrint(AGG_TAG, "asio: view not unmapped (0x%08x); it goes with the process", status);
        }
    }
    if (M->Mdl != NULL) {
        if (M->SysVa != NULL) {
            MmUnmapLockedPages(M->SysVa, M->Mdl);
        }
        MmUnlockPages(M->Mdl);
        IoFreeMdl(M->Mdl);
    }
    if (M->SysView != NULL) {
        (VOID)MmUnmapViewInSystemSpace(M->SysView);
    }
    if (M->Section != NULL) {
        ObDereferenceObject(M->Section);
    }
    if (M->Event != NULL) {
        ObDereferenceObject(M->Event);
    }
    if (M->OwnerProcess != NULL) {
        ObDereferenceObject(M->OwnerProcess);
    }
    RtlZeroMemory(M, sizeof(*M));
}

/* The shared block, as asio.c builds it: a pagefile-backed section locked for the tick, viewed by the caller. */
_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS AggMapCreate(_In_ ULONG Total, _In_ ULONGLONG EventHandle, _Out_ AGG_MAP *M)
{
    OBJECT_ATTRIBUTES oa;
    LARGE_INTEGER maxSize;
    HANDLE sectionHandle = NULL;
    SIZE_T viewSize;
    BOOLEAN locked = FALSE;
    NTSTATUS status;

    RtlZeroMemory(M, sizeof(*M));
    maxSize.QuadPart = Total;
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    status = ZwCreateSection(&sectionHandle, SECTION_MAP_READ | SECTION_MAP_WRITE, &oa, &maxSize, PAGE_READWRITE,
                             SEC_COMMIT, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = ObReferenceObjectByHandle(sectionHandle, SECTION_MAP_READ | SECTION_MAP_WRITE, NULL, KernelMode,
                                       &M->Section, NULL);
    if (!NT_SUCCESS(status)) {
        M->Section = NULL;
        goto fail;
    }
    viewSize = 0;
    status = MmMapViewInSystemSpace(M->Section, &M->SysView, &viewSize);
    if (!NT_SUCCESS(status)) {
        M->SysView = NULL;
        goto fail;
    }
    M->Mdl = IoAllocateMdl(M->SysView, Total, FALSE, FALSE, NULL);
    if (M->Mdl == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto fail;
    }
    __try {
        MmProbeAndLockPages(M->Mdl, KernelMode, IoWriteAccess);
        locked = TRUE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        status = STATUS_INSUFFICIENT_RESOURCES;
    }
    if (!locked) {
        IoFreeMdl(M->Mdl);
        M->Mdl = NULL;
        goto fail;
    }
    M->SysVa = (PUCHAR)MmGetSystemAddressForMdlSafe(M->Mdl, NormalPagePriority | MdlMappingNoExecute);
    if (M->SysVa == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto fail;
    }
    status = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)EventHandle, EVENT_MODIFY_STATE, *ExEventObjectType,
                                       UserMode, (PVOID *)&M->Event, NULL);
    if (!NT_SUCCESS(status)) {
        M->Event = NULL;
        goto fail;
    }
    viewSize = Total;
    status = ZwMapViewOfSection(sectionHandle, ZwCurrentProcess(), &M->UserVa, 0, 0, NULL, &viewSize, ViewUnmap, 0,
                                PAGE_READWRITE);
    if (!NT_SUCCESS(status)) {
        M->UserVa = NULL;
        goto fail;
    }
    ZwClose(sectionHandle);
    M->OwnerProcess = PsGetCurrentProcess();
    ObReferenceObject(M->OwnerProcess);
    return STATUS_SUCCESS;

fail:
    ZwClose(sectionHandle);
    AggMapDestroy(M);
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
static VOID AggAsioRelease(VOID)
{
    AGG_ASIO *a = &g_Agg.Asio;
    KIRQL irql;

    if (!a->Open) {
        return;
    }
    KeAcquireSpinLock(&g_Agg.Lock, &irql);
    a->Active = FALSE;
    a->Open = FALSE;
    g_Agg.StageDirty = TRUE;                /* its staging rings go back (the streams keep theirs) */
    KeReleaseSpinLock(&g_Agg.Lock, irql);
    ApolloLogPrint(AGG_TAG, "asio: closed (%ld switches, %ld underruns, %ld overruns, %lu resyncs)", a->Switches,
                   a->Underruns, a->Overruns, a->Resyncs);
    AggMapDestroy(&a->Map);
    a->OwnerFile = NULL;
    a->Shared = NULL;
    a->In = a->Out = NULL;
}

static ULONG AggLog2(_In_ ULONG V)
{
    ULONG s = 0;

    while (s < 31 && (1u << s) < V) {
        s++;
    }
    return s;
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS AggAsioOpen(_In_ WDFREQUEST Request, _In_ WDFFILEOBJECT File, _Out_ ULONG_PTR *Info)
{
    AGG_ASIO *a = &g_Agg.Asio;
    APOLLO_ASIO_OPEN_AGG_IN *in = NULL;
    PAPOLLO_ASIO_OPEN_AGG_IN pin = NULL;
    PAPOLLO_ASIO_OPEN_OUT pout = NULL;
    APOLLO_ASIO_OPEN_OUT out;
    AGG_MAP map;
    PAPOLLO_ASIO_SHARED sh;
    ULONG i, n, half[2], inBytes, outBytes, total;
    size_t len = 0;
    NTSTATUS status;
    KIRQL irql;

    *Info = 0;
    if (WdfRequestGetRequestorMode(Request) != UserMode) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*pin), (PVOID *)&pin, &len);
    if (!NT_SUCCESS(status)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    in = (APOLLO_ASIO_OPEN_AGG_IN *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*in), APOLLO_POOL_TAG); /* read under the spin lock */
    if (in == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    *in = *pin;                             /* METHOD_BUFFERED: copy before any output */
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(out), (PVOID *)&pout, &len);
    if (!NT_SUCCESS(status)) {
        status = STATUS_BUFFER_TOO_SMALL;
        goto done;
    }
    n = in->BufferFrames;
    if (in->Version != APOLLO_ASIO_VERSION_AGG || n < APOLLO_ASIO_MIN_FRAMES || n > APOLLO_ASIO_MAX_FRAMES ||
        (n & (n - 1)) != 0 || in->InCount > APOLLO_AGG_MAX_CH || in->OutCount > APOLLO_AGG_MAX_CH ||
        in->InCount + in->OutCount == 0) {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }
    if (ReadNoFence(&g_Agg.Active) == 0 || (g_Agg.Cfg.Flags & APOLLO_AGG_CFG_ENABLED) == 0) {
        /* off, or no unit runs; a timeline kept only for the combined Windows endpoints is not ASIO */
        status = STATUS_DEVICE_NOT_READY;
        goto done;
    }
    if (a->Open || g_Agg.RateChanging) {
        status = STATUS_DEVICE_BUSY;        /* [Rate] or a rate change is under way */
        goto done;
    }
    if (in->LayoutVersion != g_Agg.LayoutVersion) {
        status = STATUS_REVISION_MISMATCH;  /* the client read an older layout: read it again */
        goto done;
    }
    for (i = 0; i < in->InCount; i++) {
        if (in->In[i] >= g_Agg.Lay.InCount) {
            status = STATUS_INVALID_PARAMETER;
            goto done;
        }
    }
    for (i = 0; i < in->OutCount; i++) {
        if (in->Out[i] >= g_Agg.Lay.OutCount) {
            status = STATUS_INVALID_PARAMETER;
            goto done;
        }
    }
    half[0] = in->InCount * n * 4;
    half[1] = in->OutCount * n * 4;
    inBytes = ROUND_TO_PAGES(2 * half[0]);
    outBytes = ROUND_TO_PAGES(2 * half[1]);
    total = PAGE_SIZE + inBytes + outBytes;
    status = AggMapCreate(total, in->EventHandle, &map);
    if (!NT_SUCCESS(status)) {
        goto done;
    }
    sh = (PAPOLLO_ASIO_SHARED)map.SysVa;
    sh->Version = APOLLO_ASIO_VERSION_AGG;
    sh->BufferFrames = n;
    sh->InCount = in->InCount;
    sh->OutCount = in->OutCount;
    {
        LARGE_INTEGER f;

        (VOID)KeQueryPerformanceCounter(&f);
        sh->QpcFrequency = f.QuadPart;
    }
    sh->InputLatencyFrames = g_Agg.Lay.InLatency + g_Agg.InExtra;     /* [Lat] the units' device side */
    sh->OutputLatencyFrames = g_Agg.Lay.OutLatency + g_Agg.OutExtra;
    sh->SampleRate = (g_Agg.Timeline != NULL) ? g_Agg.Timeline->SampleRate : 48000;   /* [Rate] the timeline's */
    sh->Reserved[0] = g_Agg.LayoutVersion;
    sh->Reserved[1] = g_Agg.TimelineEpoch;

    RtlZeroMemory(&out, sizeof(out));
    out.SharedAddress = (ULONGLONG)(ULONG_PTR)map.UserVa;
    out.SharedBytes = total;
    out.InOffset[0] = PAGE_SIZE;
    out.InOffset[1] = PAGE_SIZE + half[0];
    out.OutOffset[0] = PAGE_SIZE + inBytes;
    out.OutOffset[1] = PAGE_SIZE + inBytes + half[1];
    out.ChannelStride = n * 4;

    KeAcquireSpinLock(&g_Agg.Lock, &irql);
    a->Map = map;
    a->OwnerFile = File;
    a->Shared = sh;
    a->In = (PLONG)(map.SysVa + out.InOffset[0]);
    a->Out = (PLONG)(map.SysVa + out.OutOffset[0]);
    a->N = n;
    a->Shift = AggLog2(n);
    a->InCount = in->InCount;
    a->OutCount = in->OutCount;
    for (i = 0; i < in->InCount; i++) {
        const APOLLO_AGG_LCHAN *l = &g_Agg.Lay.In[in->In[i]];

        a->InRef[i].Valid = 1;
        a->InRef[i].Unit = l->Unit;
        a->InRef[i].Slot = l->Slot;
    }
    for (i = 0; i < in->OutCount; i++) {
        a->OutL[i] = in->Out[i];
    }
    AggAsioResolveOutputs();
    a->Active = FALSE;
    a->NeedSync = TRUE;
    a->Stale = FALSE;
    a->Switches = a->Underruns = a->Overruns = 0;
    a->Resyncs = 0;
    a->NextSample = 0;
    a->SampleOffset = 0;
    a->Epoch = g_Agg.TimelineEpoch;
    a->Open = TRUE;
    KeReleaseSpinLock(&g_Agg.Lock, irql);

    *pout = out;
    *Info = sizeof(out);
    ApolloLogPrint(AGG_TAG, "asio: combined client open, %lu frames, %lu in, %lu out, layout v%lu, %lu bytes", n,
                   in->InCount, in->OutCount, g_Agg.LayoutVersion, total);
    status = STATUS_SUCCESS;
done:
    ExFreePoolWithTag(in, APOLLO_POOL_TAG);
    return status;
}

/* Does this ASIO request belong to the combined client? (caller context, PASSIVE) */
_Use_decl_annotations_
BOOLEAN ApolloAggAsioClaims(WDFREQUEST Request, ULONG Code, WDFFILEOBJECT File)
{
    if (!g_Agg.Ready) {
        return FALSE;
    }
    if (Code == IOCTL_APOLLO_ASIO_OPEN) {
        PULONG v = NULL;
        size_t len = 0;

        return NT_SUCCESS(WdfRequestRetrieveInputBuffer(Request, sizeof(ULONG), (PVOID *)&v, &len)) &&
               *v == APOLLO_ASIO_VERSION_AGG;
    }
    return g_Agg.Asio.Open && g_Agg.Asio.OwnerFile == File;
}

_Use_decl_annotations_
VOID ApolloAggAsioDispatch(WDFREQUEST Request, ULONG Code, WDFFILEOBJECT File, BOOLEAN CleanedUp)
{
    AGG_ASIO *a = &g_Agg.Asio;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info = 0;
    KIRQL irql;

    status = AggMutexAcquire();
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }
    switch (Code) {
    case IOCTL_APOLLO_ASIO_OPEN:
        status = CleanedUp ? STATUS_FILE_CLOSED : AggAsioOpen(Request, File, &info);
        break;
    case IOCTL_APOLLO_ASIO_START:
    case IOCTL_APOLLO_ASIO_STOP:
        if (!a->Open || a->OwnerFile != File) {
            status = STATUS_INVALID_DEVICE_STATE;
            break;
        }
        if (Code == IOCTL_APOLLO_ASIO_START &&
            (ReadNoFence(&g_Agg.Active) == 0 || (g_Agg.Cfg.Flags & APOLLO_AGG_CFG_ENABLED) == 0)) {
            status = STATUS_DEVICE_NOT_READY;
            break;
        }
        KeAcquireSpinLock(&g_Agg.Lock, &irql);
        if (Code == IOCTL_APOLLO_ASIO_START && !a->Active) {
            a->Active = TRUE;
            a->NeedSync = TRUE;
        } else if (Code == IOCTL_APOLLO_ASIO_STOP) {
            a->Active = FALSE;
        }
        KeReleaseSpinLock(&g_Agg.Lock, irql);
        status = STATUS_SUCCESS;
        break;
    case IOCTL_APOLLO_ASIO_CLOSE:
        if (a->Open && a->OwnerFile == File) {
            AggAsioRelease();
            status = STATUS_SUCCESS;
        } else {
            status = STATUS_INVALID_DEVICE_STATE;
        }
        break;
    default:
        break;
    }
    WdfWaitLockRelease(g_Agg.Mutex);
    WdfRequestCompleteWithInformation(Request, status, info);
}

/* File cleanup / close / device cleanup: the combined client of this handle goes. */
_Use_decl_annotations_
VOID ApolloAggAsioFileGone(WDFFILEOBJECT File)
{
    if (!g_Agg.Ready || !g_Agg.Asio.Open || g_Agg.Asio.OwnerFile != File) {
        return;
    }
    WdfWaitLockAcquire(g_Agg.Mutex, NULL);
    if (g_Agg.Asio.Open && g_Agg.Asio.OwnerFile == File) {
        AggAsioRelease();
    }
    WdfWaitLockRelease(g_Agg.Mutex);
}

/* ------------------------------------------------------------------ */
/* IOCTLs (default queue, PASSIVE)                                     */
/* ------------------------------------------------------------------ */

_Use_decl_annotations_
BOOLEAN ApolloAggIsIoctl(ULONG Code)
{
    return Code == IOCTL_APOLLO_AGG_GET_STATE || Code == IOCTL_APOLLO_AGG_GET_LAYOUT ||
           Code == IOCTL_APOLLO_AGG_SET_CONFIG || Code == IOCTL_APOLLO_AGG_CLOCK_PROBE ||
           Code == IOCTL_APOLLO_AGG_GET_CONFIG;
}

static VOID AggFillState(_Out_ APOLLO_AGG_STATE *S, _In_ BOOLEAN Reset)
{
    ULONG u;
    KIRQL irql;
    AGG_ASIO *a = &g_Agg.Asio;

    RtlZeroMemory(S, sizeof(*S));
    S->Version = APOLLO_AGG_VERSION;
    KeAcquireSpinLock(&g_Agg.Lock, &irql);
    S->Flags = ((g_Agg.Cfg.Flags & APOLLO_AGG_CFG_ENABLED) ? APOLLO_AGG_STATE_ENABLED : 0) |
               (g_Agg.Active ? APOLLO_AGG_STATE_ACTIVE : 0) | (g_Agg.Stored ? APOLLO_AGG_STATE_STORED : 0) |
               ((g_Agg.Cfg.Flags & APOLLO_AGG_CFG_ALIGN) ? APOLLO_AGG_STATE_ALIGNED : 0) |
               (g_Agg.RatiosLoaded ? APOLLO_AGG_STATE_RATIOS : 0);
    S->ConfigGeneration = (ULONG)ReadNoFence(&g_Agg.Generation);
    S->EndpointSet = g_Agg.EpHosted ? APOLLO_AGG_EPSET_COMBINED : APOLLO_AGG_EPSET_STANDALONE;
    S->EndpointStreams = g_Agg.StreamCount;
    S->Reserved[0] = g_Agg.TlValid ? g_Agg.TlRate : 0;      /* [Rate] the timeline's rate, 0 = none runs */
    S->Reserved[1] = g_Agg.RateMismatch;                    /* [Rate] bit u = unit u muted: another rate */
    S->LayoutVersion = g_Agg.LayoutVersion;
    S->TimelineEpoch = g_Agg.TimelineEpoch;
    S->UnitCount = g_Agg.Cfg.UnitCount;
    S->Master = g_Agg.Cfg.Master;
    S->Timeline = g_Agg.TimelineUnit < 0 ? MAXULONG : (ULONG)g_Agg.TimelineUnit;
    S->AsrcHalf = g_Agg.Asrc != NULL ? g_Agg.Asrc->Half : 0;
    S->InLatency = g_Agg.Lay.InLatency + g_Agg.InExtra;     /* [Lat] what ASIO reports */
    S->OutLatency = g_Agg.Lay.OutLatency + g_Agg.OutExtra;
    S->Ticks = g_Agg.Ticks;
    S->AsrcLastUs = g_Agg.LastUs;
    S->AsrcMaxUs = g_Agg.MaxUs;
    S->AsioState = !a->Open ? APOLLO_ASIO_STATE_CLOSED : (a->Active ? APOLLO_ASIO_STATE_RUNNING : APOLLO_ASIO_STATE_OPEN);
    S->AsioBufferFrames = a->Open ? a->N : 0;
    S->AsioInputs = a->Open ? a->InCount : 0;
    S->AsioOutputs = a->Open ? a->OutCount : 0;
    S->AsioSwitches = (ULONG)a->Switches;
    S->AsioUnderruns = (ULONG)a->Underruns;
    S->AsioOverruns = (ULONG)a->Overruns;
    S->AsioResyncs = a->Resyncs;
    for (u = 0; u < g_Agg.Cfg.UnitCount && u < APOLLO_AGG_MAX_UNITS; u++) {
        APOLLO_AGG_UNIT_STATE *o = &S->Unit[u];
        AGG_UNIT *un = &g_Agg.U[u];
        double ppb = (un->Clock.R - 1.0) * 1e9, em = un->Clock.EFilt * 1000.0, ep = un->Clock.EPeak * 1000.0;

        o->Uid = g_Agg.Cfg.Unit[u].Uid;
        o->Model = g_Agg.Cfg.Unit[u].Model;
        o->Flags = (un->Joined >= 0 ? APOLLO_AGG_UNIT_PRESENT : 0) | (u == g_Agg.Cfg.Master ? APOLLO_AGG_UNIT_MASTER : 0) |
                   ((LONG)u == g_Agg.TimelineUnit ? APOLLO_AGG_UNIT_TIMELINE : 0) | (un->Usable ? APOLLO_AGG_UNIT_USABLE : 0);
        if (un->Formed && (LONG)u != g_Agg.TimelineUnit) {
            o->ClockState = (ULONG)un->Clock.State;
            o->RatioPpb = (LONG)(ppb > 2e9 ? 2e9 : (ppb < -2e9 ? -2e9 : ppb));
            o->ErrorMilli = (LONG)(em > 2e9 ? 2e9 : (em < -2e9 ? -2e9 : em));
            o->ErrorPeakMilli = (LONG)(ep > 2e9 ? 2e9 : ep);
            o->LockedMs = (ULONG)(un->Clock.Locked * 1000.0 > 4e9 ? 4e9 : un->Clock.Locked * 1000.0);
            o->GainMilli = (ULONG)(un->GainRen * 1000.0);
        }
        o->Resyncs = un->Clock.Resyncs;
        o->Outliers = un->Clock.Outliers;
        o->Position = un->Pos;
        o->RenderLate = un->RenderLate;
        o->CaptureShort = un->CaptureShort;
        o->InLatency = g_Agg.Rt[u].Adc + g_Agg.Rt[u].Guard + ((LONG)u != g_Agg.TimelineUnit ?
                                                              g_Agg.Cfg.AsrcHalf + ApolloAggMarginAt(g_Agg.Rt[u].Rate) : 0);
        o->OutLatency = g_Agg.Rt[u].Lead + g_Agg.Rt[u].FrameCfg;
        if (un->Joined >= 0 && un->Joined < APOLLO_AGG_MAX_UNITS) {
            o->InLatency += g_Agg.Joined[un->Joined].LatInPath;    /* [Lat] its strip path and DAC */
            o->OutLatency += g_Agg.Joined[un->Joined].LatDac;
        }
        if (Reset) {
            un->Clock.EPeak = 0;
            un->Clock.Outliers = 0;
            un->RenderLate = un->CaptureShort = 0;
        }
    }
    if (Reset) {
        g_Agg.MaxUs = 0;
    }
    KeReleaseSpinLock(&g_Agg.Lock, irql);
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS AggSetConfig(_In_ const APOLLO_AGG_CONFIG *In, _Out_ APOLLO_AGG_SET_RESULT *Res)
{
    APOLLO_AGG_CONFIG *c, *old;
    NTSTATUS status;
    ULONG err;

    RtlZeroMemory(Res, sizeof(*Res));
    err = ApolloAggValidate(In);
    if (err != 0) {
        Res->Error = err;
        ApolloLogPrint(AGG_TAG, "configuration refused (check %lu)", err);
        return STATUS_INVALID_PARAMETER;
    }
    c = (APOLLO_AGG_CONFIG *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*c), APOLLO_POOL_TAG);  /* copied under the spin lock */
    old = (APOLLO_AGG_CONFIG *)ExAllocatePool2(POOL_FLAG_PAGED, sizeof(*old), APOLLO_POOL_TAG);
    if (c == NULL || old == NULL) {
        if (c != NULL) {
            ExFreePoolWithTag(c, APOLLO_POOL_TAG);
        }
        if (old != NULL) {
            ExFreePoolWithTag(old, APOLLO_POOL_TAG);
        }
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(c, In, sizeof(*c));
    status = AggMutexAcquire();
    if (NT_SUCCESS(status)) {
        if (!g_Agg.Loaded) {
            AggLoad();
        }
        RtlCopyMemory(old, &g_Agg.Cfg, sizeof(*old));
        (VOID)AggFillCaches(c, old);        /* the driver's descriptor caches, not the caller's */
        {
            KIRQL irql;

            /* Cfg is read by the tick (UnitCount, Master): replace it under the spin lock */
            KeAcquireSpinLock(&g_Agg.Lock, &irql);
            RtlCopyMemory(&g_Agg.Cfg, c, sizeof(*c));
            KeReleaseSpinLock(&g_Agg.Lock, irql);
        }
        g_Agg.Stored = TRUE;
        (VOID)AggRebuild("configuration set");
        status = AggSave();
        Res->LayoutVersion = g_Agg.LayoutVersion;
        /* whoever set it (Apollo Control, agg-test, the ASIO clock menu): pollers re-read the config */
        InterlockedIncrement(&g_Agg.Generation);
        WdfWaitLockRelease(g_Agg.Mutex);
    }
    ExFreePoolWithTag(c, APOLLO_POOL_TAG);
    ExFreePoolWithTag(old, APOLLO_POOL_TAG);
    return status;
}

/* AGG_CLOCK_PROBE: the peer-clock experiment on Ctx (spec 6.3.7), DevLock, every write logged. */
_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS AggClockProbe(_In_ PDEVICE_CONTEXT Ctx, _In_ const APOLLO_AGG_PROBE_IN *In, _Out_ APOLLO_AGG_PROBE_OUT *Out)
{
    LONGLONG t = -10000LL * AGG_LOCK_TIMEOUT_MS;
    ULONGLONG t0;
    ULONG i, word, polls, s, v, p0, p1;
    NTSTATUS status;

    RtlZeroMemory(Out, sizeof(*Out));
    Out->Version = APOLLO_AGG_VERSION;
    if (In->Version != APOLLO_AGG_VERSION || (In->Source != APOLLO_CLKSRC_PEER && In->Source != APOLLO_CLKSRC_INTERNAL) ||
        (In->AxControl != 0 && In->AxControl != 0xF && In->AxControl != 0x20F) || In->WaitMs > 3000) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!Ctx->Unit.Active || !Ctx->Connected || !Ctx->TransportRunning || Ctx->Bar0 == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    status = WdfWaitLockAcquire(Ctx->DevLock, &t);
    if (status == STATUS_TIMEOUT) {
        return STATUS_DEVICE_BUSY;
    }
    Out->ClockCfgBefore = ApolloRead(Ctx, REG_NOTIF_CLOCK_CFG);
    Out->RateInfoBefore = ApolloRead(Ctx, REG_NOTIF_RATE_INFO);
    Out->AxControlBefore = ApolloRead(Ctx, REG_AX_CONTROL);
    Out->P2pRoute = ApolloRead(Ctx, REG_AX_P2P_ROUTE);
    Out->P2pEnable = ApolloRead(Ctx, 0x22C4);
    for (i = 0; i < 4; i++) {
        Out->P2pSlot[i] = ApolloRead(Ctx, 0x2280 + i * 4);
    }
    ApolloLogU(Ctx, "clock probe: before: 0xC074 0x%08lx, 0xC07C 0x%08lx, AX_CONTROL 0x%03lx, P2P route 0x%08lx, "
              "P2P enable 0x%08lx, slots 0x%08lx 0x%08lx 0x%08lx 0x%08lx", Out->ClockCfgBefore, Out->RateInfoBefore,
              Out->AxControlBefore, Out->P2pRoute, Out->P2pEnable, Out->P2pSlot[0], Out->P2pSlot[1], Out->P2pSlot[2],
              Out->P2pSlot[3]);
    if (In->AxControl != 0) {
        ApolloLogU(Ctx, "clock probe: write AX_CONTROL (0x%04x) = 0x%03lx", REG_AX_CONTROL, In->AxControl);
        ApolloWrite(Ctx, REG_AX_CONTROL, In->AxControl);
    }
    word = ApolloPlanClockWord(Ctx->RateIndex, In->Source);
    ApolloLogU(Ctx, "clock probe: write 0x%04x = 0x%03lx (rate index %lu, source 0x%02lx), then AX_CONNECT (0x%04x) = 0x%x",
              REG_NOTIF_CLOCK_CFG, word, Ctx->RateIndex, In->Source, REG_AX_CONNECT, AX_CONNECT_CLOCK);
    ApolloWrite(Ctx, REG_NOTIF_CLOCK_CFG, word);
    ApolloWrite(Ctx, REG_AX_CONNECT, AX_CONNECT_CLOCK);
    t0 = KeQueryInterruptTime();
    Out->AckMs = MAXULONG;
    polls = (In->WaitMs + 9) / 10;
    for (i = 0; i <= polls; i++) {
        s = ApolloRead(Ctx, REG_NOTIF_STATUS);
        if (s == 0xFFFFFFFF) {
            break;
        }
        if (s != 0) {
            Out->NotifSeen |= s;
            Ctx->NotifStatusSeen |= s;
            ApolloWrite(Ctx, REG_NOTIF_STATUS, 0);      /* acknowledge, as the service pass does */
        }
        v = ApolloRead(Ctx, REG_NOTIF_RATE_INFO);
        if (i > 0 && ApolloPlanRateLocked(v, Ctx->RateIndex) && (Out->NotifSeen & 0x20) != 0) {
            Out->AckMs = (ULONG)((KeQueryInterruptTime() - t0) / 10000ULL);
            break;
        }
        ApolloSleepMs(10);
    }
    Out->ClockCfgAfter = ApolloRead(Ctx, REG_NOTIF_CLOCK_CFG);
    Out->RateInfoAfter = ApolloRead(Ctx, REG_NOTIF_RATE_INFO);
    Out->ClockInfo = ApolloRead(Ctx, REG_NOTIF_CLOCK_INFO);
    Out->ClockSrcs = ApolloRead(Ctx, REG_NOTIF_CLOCK_SRCS);
    Out->AxControlAfter = ApolloRead(Ctx, REG_AX_CONTROL);
    for (i = 0; i < 3; i++) {
        Out->PeerLatency[i] = ApolloRead(Ctx, 0x22C8 + i * 4);
    }
    p0 = ApolloRead(Ctx, REG_AX_SAMPLE_POS);
    ApolloSleepMs(100);
    p1 = ApolloRead(Ctx, REG_AX_SAMPLE_POS);
    Out->FramesIn100ms = (Ctx->RingFrames != 0) ? ((p1 - p0) & (Ctx->RingFrames - 1)) : 0;
    ApolloLogU(Ctx, "clock probe: after %s: 0xC074 0x%08lx, 0xC07C 0x%08lx, 0xC084 0x%08lx, 0xC08C 0x%08lx, AX_CONTROL "
              "0x%03lx, notifications 0x%08lx, peer latency 0x%08lx 0x%08lx 0x%08lx, SAMPLE_POS +%lu in 100 ms",
              Out->AckMs == MAXULONG ? "(no rate-locked ack)" : "(acked)", Out->ClockCfgAfter, Out->RateInfoAfter,
              Out->ClockInfo, Out->ClockSrcs, Out->AxControlAfter, Out->NotifSeen, Out->PeerLatency[0],
              Out->PeerLatency[1], Out->PeerLatency[2], Out->FramesIn100ms);
    WdfWaitLockRelease(Ctx->DevLock);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS ApolloAggIoctl(PDEVICE_CONTEXT Ctx, ULONG Code, PVOID In, size_t InLen, PVOID Out, size_t OutLen,
                        ULONG_PTR *Info)
{
    NTSTATUS status;

    *Info = 0;
    if (!g_Agg.Ready) {
        return STATUS_DEVICE_NOT_READY;
    }
    switch (Code) {
    case IOCTL_APOLLO_AGG_GET_STATE: {
        BOOLEAN reset = (In != NULL && InLen >= sizeof(ULONG) && (*(PULONG)In & APOLLO_AGG_RESET) != 0);

        if (Out == NULL || OutLen < sizeof(APOLLO_AGG_STATE)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        AggFillState((APOLLO_AGG_STATE *)Out, reset);
        *Info = sizeof(APOLLO_AGG_STATE);
        return STATUS_SUCCESS;
    }
    case IOCTL_APOLLO_AGG_GET_LAYOUT: {
        APOLLO_AGG_LAYOUT *l = (APOLLO_AGG_LAYOUT *)Out;
        ULONG u;
        KIRQL irql;

        if (Out == NULL || OutLen < sizeof(APOLLO_AGG_LAYOUT)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        RtlZeroMemory(l, sizeof(*l));
        l->Version = APOLLO_AGG_VERSION;
        KeAcquireSpinLock(&g_Agg.Lock, &irql);
        l->LayoutVersion = g_Agg.LayoutVersion;
        /* "the combined ASIO device exists": not for a timeline kept only for the Windows endpoints */
        l->Flags = (g_Agg.Active && (g_Agg.Cfg.Flags & APOLLO_AGG_CFG_ENABLED)) ? APOLLO_AGG_LAYOUT_ACTIVE : 0;
        l->InCount = g_Agg.Lay.InCount;
        l->OutCount = g_Agg.Lay.OutCount;
        l->InLatency = g_Agg.Lay.InLatency + g_Agg.InExtra;     /* [Lat] what ASIO reports */
        l->OutLatency = g_Agg.Lay.OutLatency + g_Agg.OutExtra;
        l->UnitCount = g_Agg.Cfg.UnitCount;
        l->Master = g_Agg.Cfg.Master;
        l->Timeline = g_Agg.TimelineUnit < 0 ? MAXULONG : (ULONG)g_Agg.TimelineUnit;
        l->TimelineEpoch = g_Agg.TimelineEpoch;
        for (u = 0; u < g_Agg.Cfg.UnitCount && u < APOLLO_AGG_MAX_UNITS; u++) {
            l->Uid[u] = g_Agg.Cfg.Unit[u].Uid;
            l->Model[u] = g_Agg.Cfg.Unit[u].Model;
            l->Present[u] = g_Agg.U[u].Joined >= 0 ? 1 : 0;
        }
        RtlCopyMemory(l->In, g_Agg.Lay.In, sizeof(l->In));
        RtlCopyMemory(l->Out, g_Agg.Lay.Out, sizeof(l->Out));
        KeReleaseSpinLock(&g_Agg.Lock, irql);
        *Info = sizeof(*l);
        return STATUS_SUCCESS;
    }
    case IOCTL_APOLLO_AGG_GET_CONFIG: {
        APOLLO_AGG_CONFIG *c = (APOLLO_AGG_CONFIG *)Out;

        if (Out == NULL || OutLen < sizeof(APOLLO_AGG_CONFIG)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        status = AggMutexAcquire();
        if (!NT_SUCCESS(status)) {
            return status;
        }
        if (!g_Agg.Loaded) {
            AggLoad();
            (VOID)AggRebuild("configuration read");
        }
        RtlCopyMemory(c, &g_Agg.Cfg, sizeof(*c));
        if (!g_Agg.Stored) {
            c->Flags |= APOLLO_AGG_CFG_DEFAULT;
        }
        WdfWaitLockRelease(g_Agg.Mutex);
        *Info = sizeof(*c);
        return STATUS_SUCCESS;
    }
    case IOCTL_APOLLO_AGG_SET_CONFIG: {
        APOLLO_AGG_SET_RESULT res;

        if (In == NULL || InLen < sizeof(APOLLO_AGG_CONFIG)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        if (Out == NULL || OutLen < sizeof(res)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        status = AggSetConfig((const APOLLO_AGG_CONFIG *)In, &res);   /* copies the input first */
        if (status == STATUS_INVALID_PARAMETER && res.Error != 0) {
            /*
             * METHOD_BUFFERED copies nothing back under an error status, so the reason never
             * reached the caller ("check 0"). A refused configuration completes with success and the
             * reason in Error (nothing was stored, LayoutVersion is the one in force); callers test
             * Error (apollo_agg.h APOLLO_AGG_SET_RESULT).
             */
            res.LayoutVersion = g_Agg.LayoutVersion;
            status = STATUS_SUCCESS;
        }
        RtlCopyMemory(Out, &res, sizeof(res));
        *Info = sizeof(res);
        return status;
    }
    case IOCTL_APOLLO_AGG_CLOCK_PROBE: {
        APOLLO_AGG_PROBE_IN pin;
        APOLLO_AGG_PROBE_OUT pout;

        if (In == NULL || InLen < sizeof(pin) || Out == NULL || OutLen < sizeof(pout)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        pin = *(const APOLLO_AGG_PROBE_IN *)In;
        status = AggClockProbe(Ctx, &pin, &pout);
        if (NT_SUCCESS(status)) {
            RtlCopyMemory(Out, &pout, sizeof(pout));
            *Info = sizeof(pout);
        }
        return status;
    }
    default:
        return STATUS_INVALID_DEVICE_REQUEST;
    }
}

/* ------------------------------------------------------------------ */
/* [Rate] The combined device's rate (include\apollo_rate.h)           */
/* ------------------------------------------------------------------ */

/* A joined member's runtime values and descriptor caches from its context again (Mutex held). */
static VOID AggMemberRefresh(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONG j;

    for (j = 0; j < APOLLO_AGG_MAX_UNITS; j++) {
        if (g_Agg.Joined[j].Ctx == Ctx) {
            AggMemberFill(&g_Agg.Joined[j], Ctx);
            return;
        }
    }
}

/* The joined members in change order, the timeline unit (else the configured master) first (Mutex held). */
static ULONG AggRateMembers(_Out_writes_to_(APOLLO_AGG_MAX_UNITS, return) PDEVICE_CONTEXT *M)
{
    ULONG n = 0, u, k;
    LONG first = (g_Agg.TimelineUnit >= 0) ? g_Agg.TimelineUnit : (LONG)g_Agg.Cfg.Master;

    for (k = 0; k < 2; k++) {
        for (u = 0; u < g_Agg.Cfg.UnitCount && u < APOLLO_AGG_MAX_UNITS; u++) {
            PDEVICE_CONTEXT c = AggCtx(u);

            if (c == NULL || ((LONG)u == first) != (k == 0) || n >= APOLLO_AGG_MAX_UNITS) {
                continue;
            }
            M[n++] = c;
        }
    }
    return n;
}

/* Joined config units now, and whether Ctx is one of them (a snapshot; Agg.Lock or Mutex). */
static ULONG AggJoinedCount(_In_opt_ PDEVICE_CONTEXT Ctx, _Out_ PBOOLEAN Found)
{
    ULONG u, n = 0;

    *Found = FALSE;
    for (u = 0; u < g_Agg.Cfg.UnitCount && u < APOLLO_AGG_MAX_UNITS; u++) {
        PDEVICE_CONTEXT c = AggCtx(u);

        if (c != NULL) {
            n++;
            if (c == Ctx) {
                *Found = TRUE;
            }
        }
    }
    return n;
}

static ULONG AggUnitAllowed(_In_ PDEVICE_CONTEXT Ctx)
{
    /* [Clock] on the clock source asked for: a unit on S/PDIF or ADAT allows 44.1-96 kHz only */
    return ApolloRateAllowed(Ctx->NotifClockSrcsLive != 0 ? Ctx->NotifClockSrcsLive : Ctx->NotifClockSrcs,
                             ApolloClockWantedOf(Ctx));
}

_Use_decl_annotations_
BOOLEAN ApolloAggRateMember(PDEVICE_CONTEXT Ctx)
{
    BOOLEAN found = FALSE;
    ULONG n;
    KIRQL irql;

    if (!g_Agg.Ready || ReadNoFence(&g_Agg.Active) == 0) {
        return FALSE;
    }
    KeAcquireSpinLock(&g_Agg.Lock, &irql);
    n = AggJoinedCount(Ctx, &found);
    KeReleaseSpinLock(&g_Agg.Lock, irql);
    return (found && n >= 2) ? TRUE : FALSE;
}

/*
 * Every joined unit to Hz, the timeline unit first (the master when it runs), then the followers, each
 * the way a unit changes alone (rate.c ApolloRateChange, InAgg). The aggregate's tick is paused for the
 * whole of it; afterwards the layout is built again at the new rate (latencies, M) and a new epoch
 * re-forms the followers and re-anchors the combined streams. Refused when a unit does not allow Hz,
 * when an ASIO host has the combined device (unless File is that host's handle, stopped) or a unit.
 * Absent configured units get Hz stored too (Units\<HW_UID>\SampleRate), so they come up at it.
 */
_Use_decl_annotations_
ULONG ApolloAggRateSet(PDEVICE_CONTEXT Ctx, ULONG Hz, ULONG Flags, WDFFILEOBJECT File, PCHAR Why, size_t WhyLen)
{
    PDEVICE_CONTEXT m[APOLLO_AGG_MAX_UNITS];
    ULONG n, i, u, err = APOLLO_RATE_ERR_NONE, idx = ApolloRateIndex(Hz), allowed = APOLLO_RATE_MASK_ALL;
    BOOLEAN found = FALSE, cfgChanged;
    KIRQL irql;

    Why[0] = 0;
    if (idx == 0) {
        (VOID)RtlStringCbPrintfA(Why, WhyLen, "%lu Hz is not a rate the Apollo runs at", Hz);
        return APOLLO_RATE_ERR_RATE;
    }
    if (!g_Agg.Ready || !NT_SUCCESS(AggMutexAcquire())) {
        (VOID)RtlStringCbCopyA(Why, WhyLen, "the combined device is busy: try again");
        return APOLLO_RATE_ERR_BUSY;
    }
    if (!g_Agg.Loaded) {
        AggLoad();
    }
    n = AggRateMembers(m);
    for (i = 0; i < n; i++) {
        found = found || m[i] == Ctx;
        allowed &= AggUnitAllowed(m[i]);
    }
    if (ReadNoFence(&g_Agg.Active) == 0 || n == 0) {
        (VOID)RtlStringCbCopyA(Why, WhyLen, "no combined device runs (Units & routing)");
        err = APOLLO_RATE_ERR_NO_COMBINED;
    } else if (!found) {
        (VOID)RtlStringCbCopyA(Why, WhyLen, "this unit is not in the combined device");
        err = APOLLO_RATE_ERR_NO_COMBINED;
    } else if ((allowed & (1ul << idx)) == 0) {
        (VOID)RtlStringCbPrintfA(Why, WhyLen, "not every unit of the combined device offers %s", ApolloRateText(Hz));
        err = APOLLO_RATE_ERR_UNSUPPORTED;
    } else {
        KeAcquireSpinLock(&g_Agg.Lock, &irql);
        if (g_Agg.Asio.Open && (File == NULL || g_Agg.Asio.OwnerFile != File || g_Agg.Asio.Active)) {
            err = APOLLO_RATE_ERR_COMBINED_ASIO;
        }
        KeReleaseSpinLock(&g_Agg.Lock, irql);
        if (err != APOLLO_RATE_ERR_NONE) {
            (VOID)RtlStringCbCopyA(Why, WhyLen, "an ASIO host has the combined device (Apollo (Avium)): change the "
                                   "rate there, or close its audio device");
        }
        for (i = 0; err == APOLLO_RATE_ERR_NONE && i < n; i++) {
            if (m[i]->Asio.Open) {
                err = APOLLO_RATE_ERR_ASIO;
                (VOID)RtlStringCbPrintfA(Why, WhyLen, "an ASIO host has the %s on its own driver: close its audio device",
                                         m[i]->Ident.Name != NULL ? m[i]->Ident.Name : "unit");
            }
        }
    }
    if (err != APOLLO_RATE_ERR_NONE) {
        WdfWaitLockRelease(g_Agg.Mutex);
        return err;
    }

    ApolloLogPrint(AGG_TAG, "rate: the combined device to %s: %lu unit(s), the timeline unit first", ApolloRateText(Hz), n);
    KeAcquireSpinLock(&g_Agg.Lock, &irql);
    g_Agg.RateChanging = TRUE;              /* the tick does nothing until the end */
    KeReleaseSpinLock(&g_Agg.Lock, irql);

    for (i = 0; i < n; i++) {
        ULONG e = ApolloRateChange(m[i], Hz, Flags, File, TRUE);

        if (e != APOLLO_RATE_ERR_NONE) {
            if (i == 0) {
                err = e;
                (VOID)RtlStringCbCopyA(Why, WhyLen, m[i]->RateWhy);
            } else {
                err = APOLLO_RATE_ERR_PARTIAL;
                (VOID)RtlStringCbPrintfA(Why, WhyLen, "the timeline unit runs at %s, the %s did not follow: %s",
                                         ApolloRateText(m[0]->SampleRate),
                                         m[i]->Ident.Name != NULL ? m[i]->Ident.Name : "other unit", m[i]->RateWhy);
            }
            break;
        }
    }
    for (i = 0; i < n; i++) {
        AggMemberRefresh(m[i]);
    }
    cfgChanged = AggRebuild("rate change");
    if (cfgChanged && g_Agg.Stored) {
        (VOID)AggSave();
    }
    if (err == APOLLO_RATE_ERR_NONE && (Flags & APOLLO_RATE_SET_NO_STORE) == 0) {
        WDFKEY key = NULL;
        NTSTATUS st = AggOpenKey(KEY_READ | KEY_WRITE, &key);

        if (NT_SUCCESS(st)) {
            st = WdfRegistryAssignULong(key, &g_AggRateName, Hz);
            WdfRegistryClose(key);
        }
        if (NT_SUCCESS(st)) {
            g_Agg.StoredRate = Hz;
        }
        /* configured units that are not here now come up at it too */
        for (u = 0; u < g_Agg.Cfg.UnitCount && u < APOLLO_AGG_MAX_UNITS; u++) {
            CHAR key16[20];

            if (AggCtx(u) != NULL || g_Agg.Cfg.Unit[u].Uid == 0) {
                continue;
            }
            if (NT_SUCCESS(RtlStringCbPrintfA(key16, sizeof(key16), "%016llX", g_Agg.Cfg.Unit[u].Uid))) {
                ApolloLogPrint(AGG_TAG, "rate: unit %lu (absent) stored at %lu Hz: 0x%08x", u, Hz,
                               ApolloRateStoreKey(key16, Hz));
            }
        }
        ApolloLogPrint(AGG_TAG, "rate: %lu Hz stored under Parameters\\Aggregate (0x%08x)", Hz, st);
    }
    KeAcquireSpinLock(&g_Agg.Lock, &irql);
    g_Agg.RateChanging = FALSE;
    AggNewEpoch();                          /* followers re-form at the new rate, clients re-anchor */
    if (g_Agg.Asio.Open && g_Agg.Asio.Shared != NULL && n != 0) {
        g_Agg.Asio.Shared->SampleRate = m[0]->SampleRate;
    }
    KeReleaseSpinLock(&g_Agg.Lock, irql);
    WdfWaitLockRelease(g_Agg.Mutex);
    if (err == APOLLO_RATE_ERR_NONE) {
        (VOID)RtlStringCbPrintfA(Why, WhyLen, "%s on %lu unit(s), the timeline unit first", ApolloRateText(Hz), n);
    }
    ApolloLogPrint(AGG_TAG, "rate: the combined device %s (%s)", err == APOLLO_RATE_ERR_NONE ? "changed" : "FAILED", Why);
    return err;
}

/*
 * A unit changed its rate alone (rate.c, no unit lock held): a joined unit (the only one present, or
 * the combined device is off) gets its runtime values and layout again and a new epoch.
 */
_Use_decl_annotations_
VOID ApolloAggRateUnitChanged(PDEVICE_CONTEXT Ctx)
{
    KIRQL irql;
    BOOLEAN found = FALSE;

    if (!g_Agg.Ready || !NT_SUCCESS(AggMutexAcquire())) {
        return;
    }
    (VOID)AggJoinedCount(Ctx, &found);
    if (found) {
        AggMemberRefresh(Ctx);
        if (AggRebuild("unit rate change") && g_Agg.Stored) {
            (VOID)AggSave();
        }
        KeAcquireSpinLock(&g_Agg.Lock, &irql);
        AggNewEpoch();
        KeReleaseSpinLock(&g_Agg.Lock, irql);
    }
    WdfWaitLockRelease(g_Agg.Mutex);
}

/*
 * [Clock] A running member by HW_UID, its rundown held: the tick's own rule (U[].Joined under Agg.Lock, which Leave
 * unlinks under the same lock before it waits for the rundown), so the context stays valid until the Unref.
 */
_Use_decl_annotations_
PDEVICE_CONTEXT ApolloAggUnitRef(ULONGLONG Uid, PVOID *Ref)
{
    PDEVICE_CONTEXT c = NULL;
    ULONG u;
    LONG j;
    KIRQL irql;

    *Ref = NULL;
    if (!g_Agg.Ready || Uid == 0) {
        return NULL;
    }
    KeAcquireSpinLock(&g_Agg.Lock, &irql);
    for (u = 0; u < g_Agg.Cfg.UnitCount && u < APOLLO_AGG_MAX_UNITS && c == NULL; u++) {
        j = g_Agg.U[u].Joined;
        if (j < 0 || j >= APOLLO_AGG_MAX_UNITS || g_Agg.Joined[j].Ctx == NULL || g_Agg.Joined[j].Uid != Uid) {
            continue;
        }
        if (ExAcquireRundownProtection(&g_Agg.Joined[j].Rundown)) {
            c = g_Agg.Joined[j].Ctx;
            *Ref = &g_Agg.Joined[j];
        }
    }
    KeReleaseSpinLock(&g_Agg.Lock, irql);
    return c;
}

_Use_decl_annotations_
VOID ApolloAggUnitUnref(PVOID Ref)
{
    if (Ref != NULL) {
        ExReleaseRundownProtection(&((AGG_JOINED *)Ref)->Rundown);
    }
}

/* [Clock] Is Ctx's clock the combined device's timeline now (a snapshot)? */
_Use_decl_annotations_
BOOLEAN ApolloAggIsTimeline(PDEVICE_CONTEXT Ctx)
{
    return (g_Agg.Ready && ReadNoFence(&g_Agg.Active) != 0 && g_Agg.Timeline == Ctx) ? TRUE : FALSE;
}

/* GET_RATE: the combined device as this unit sees it (PASSIVE; Agg.Lock for the snapshot). */
_Use_decl_annotations_
VOID ApolloAggRateFill(PDEVICE_CONTEXT Ctx, PAPOLLO_RATE_INFO I)
{
    ULONG u, n = 0, allowed = APOLLO_RATE_MASK_ALL;
    BOOLEAN found = FALSE;
    KIRQL irql;

    if (!g_Agg.Ready) {
        return;
    }
    KeAcquireSpinLock(&g_Agg.Lock, &irql);
    I->CombinedStored = g_Agg.StoredRate;
    if (g_Agg.Active != 0) {
        for (u = 0; u < g_Agg.Cfg.UnitCount && u < APOLLO_AGG_MAX_UNITS; u++) {
            PDEVICE_CONTEXT c = AggCtx(u);

            if (c == NULL) {
                continue;
            }
            n++;
            found = found || c == Ctx;
            allowed &= AggUnitAllowed(c);
        }
        I->CombinedUnits = n;
        if (found) {
            I->CombinedRate = (g_Agg.Timeline != NULL) ? g_Agg.Timeline->SampleRate : 0;
            I->CombinedAllowed = allowed;
            if (n >= 2) {
                I->Flags |= APOLLO_RATE_F_COMBINED;
            }
            if (g_Agg.Asio.Open) {
                I->Flags |= APOLLO_RATE_F_COMBINED_ASIO;
            }
            if (g_Agg.RateMismatch != 0) {
                I->Flags |= APOLLO_RATE_F_COMBINED_MISMATCH;
            }
            if (g_Agg.RateChanging) {
                I->Flags |= APOLLO_RATE_F_CHANGING;
            }
        }
    }
    KeReleaseSpinLock(&g_Agg.Lock, irql);
}
