/*
 * apollo_stream.h - ACX endpoints: the endpoint table (endpoints.c), circuits
 * (circuit.c) and RT streams (stream.c). docs/PHASE2.md sections 3-5.
 *
 * Locking: everything a stream shares with the engine tick is under
 * EngineLock (apollo.h). Stream callbacks run at PASSIVE_LEVEL, one at a time
 * per stream (ACX serialises them); they take EngineLock briefly. Packet
 * notifications are collected under EngineLock with the stream's rundown
 * protection held and sent after the lock is dropped, so Pause can wait for
 * them before the packets are freed.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include "apollo.h"
#include <ntstrsafe.h>
#include "apollo_acx.h"
#include "apollo_agg.h"         /* the combined endpoint table and APOLLO_AGG_SREF (pure) */

#define APOLLO_STREAM_TAG           'sopA'
#define APOLLO_EP_DEFAULT_MASK      0x1FFFF /* [Endpoints] the x8p's default set (bringup_plan.h APOLLO_X8P_EP_DEFAULT) */
#define APOLLO_ADC_FRAMES           10      /* x8p ADC latency (spec-streaming 3.6) */

/* One row = one circuit = one Windows endpoint. Static; the INF lists the same names. */
typedef struct _APOLLO_ENDPOINT_DESC {
    PCWSTR              RefString;          /* circuit name = INF reference string */
    PCWSTR              PinName;            /* what Sound settings shows */
    const GUID         *ComponentId;
    ACX_CIRCUIT_TYPE    Type;
    const GUID         *BridgeCategory;     /* KSNODETYPE_* = endpoint form factor */
    ULONG               Channels;           /* 1 or 2 */
    USHORT              ChannelId[2];       /* IO descriptor IDs (type << 8 | number); mono: [1] = [0] */
    UCHAR               FallbackSlot[2];    /* verified slot when the descriptor is unusable; mono: [1] = [0] */
    /*
     * 0 = a unit's own (standalone) row (the field is left out of those initialisers, so it is 0);
     * n = row n - 1 of the combined table (apollo_agg.h APOLLO_AGG_EP_LIST): the stream runs on the
     * aggregate timeline (agg.c), under Agg.Lock instead of EngineLock.
     */
    UCHAR               AggRow1;
    UCHAR               Model;              /* [Endpoints] APOLLO_INFO_MODEL_* of the jack's unit, 0 = follows the routing */
} APOLLO_ENDPOINT_DESC, *PAPOLLO_ENDPOINT_DESC;

extern const APOLLO_ENDPOINT_DESC g_ApolloEndpoints[];
extern const ULONG g_ApolloEndpointCount;

/*
 * One endpoint table per model (endpoints.c, rows in endpoint_plan.h): the x8p's g_ApolloEndpoints,
 * the Twin X's own rows. Device add picks it by the PCI hardware ID (apollo_unit.h
 * ApolloAddTimeModel); PrepareHardware adds circuits only when the identified model is the same.
 */
typedef struct _APOLLO_ENDPOINT_TABLE {
    int                         Model;      /* APOLLO_MODEL_* */
    const APOLLO_ENDPOINT_DESC *Rows;
    ULONG                       Count;
    ULONG                       AdcFrames;  /* capture delay reported to Windows (bringup_plan.h AdcFrames) */
    ULONG                       DefaultMask;    /* [Endpoints] rows in the default set (registry value absent) */
} APOLLO_ENDPOINT_TABLE;

const APOLLO_ENDPOINT_TABLE *ApolloEndpointTableFor(_In_ int Model);   /* NULL: no table */
/* the combined table (spec 6.7), created only on the host unit when EndpointSet is 1 */
extern const APOLLO_ENDPOINT_TABLE g_ApolloAggEndpointTable;

ULONG ApolloEndpointSlot(_In_ PDEVICE_CONTEXT Ctx, _In_ const APOLLO_ENDPOINT_DESC *Desc,
                         _In_ ULONG Channel);

typedef struct _APOLLO_STREAM APOLLO_STREAM, *PAPOLLO_STREAM;

/* Per endpoint. Counters and Stream: EngineLock. */
typedef struct _APOLLO_EP {
    const APOLLO_ENDPOINT_DESC *Desc;
    ACXCIRCUIT          Circuit;            /* NULL if disabled or creation failed */
    BOOLEAN             Added;              /* AcxDeviceAddCircuit done */
    /*
     * [Rate] The host pin and one format object per rate (index 1-6 at [0-5], parented to the circuit,
     * made at device add); the pin's list holds exactly one of them, FmtRate's (circuit.c
     * ApolloAcxSetRate, PASSIVE, no unit lock).
     */
    ACXPIN              HostPin;
    ACXDATAFORMAT       Fmt[6];
    ULONG               FmtRate;            /* Hz the host pin advertises now, 0 = none (creation failed) */
    PAPOLLO_STREAM      Stream;             /* the one stream (host pin MaxStreams = 1) */
    ULONG               Streams;
    ULONGLONG           Packets;
    ULONG               Underruns;
    ULONG               Overruns;
    ULONG               Dropped;
    ULONG               LatePackets;
    ULONG               NotifyFailures;
} APOLLO_EP, *PAPOLLO_EP;

/*
 * Packet memory of a stream ACX deleted without calling FreeRtPackets (not
 * expected: the ACX sample relies on FreeRtPackets alone). Kept until the
 * device object is destroyed, after every circuit and stream under it, then
 * freed, so a driver unload under Verifier pool tracking finds nothing left.
 */
typedef struct _APOLLO_PARKED {
    LIST_ENTRY          Link;
    PACX_RTPACKET       Packets;
    PUCHAR              Buffer;
    PMDL                Mdl;
} APOLLO_PARKED, *PAPOLLO_PARKED;

/* Device-level ACX state: a second WDF context on the WDFDEVICE (Ctx->Acx points at it). */
typedef struct _APOLLO_ACX {
    const APOLLO_ENDPOINT_TABLE *Table;     /* the model's table, NULL = none (Count 0) */
    ULONG               Mask;               /* rows with a circuit object (combined: Aggregate\Endpoints) */
    ULONG               InForce;            /* [Endpoints] the mask the last PrepareHardware added from (GET_ENDPOINTS) */
    BOOLEAN             Combined;           /* Table is the combined one (EndpointSet 1, host unit) */
    BOOLEAN             Hosted;             /* ApolloAggEndpointsHosted(TRUE) was said */
    ULONG               Count;              /* rows used (<= APOLLO_ENDPOINTS_ROWS_MAX) */
    LIST_ENTRY          Active;             /* running streams, EngineLock */
    volatile LONG       Prepared;           /* streams between Prepare and Release */
    KSPIN_LOCK          ParkLock;
    LIST_ENTRY          Parked;             /* APOLLO_PARKED, ParkLock */
    APOLLO_EP           Ep[APOLLO_ENDPOINTS_ROWS_MAX];
} APOLLO_ACX, *PAPOLLO_ACX;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(APOLLO_ACX, ApolloGetAcx)

/* stream.c: destroy callback of the APOLLO_ACX context, frees parked packet memory */
EVT_WDF_OBJECT_CONTEXT_DESTROY ApolloAcxContextDestroy;

/* Circuit context: which endpoint this circuit is. */
typedef struct _APOLLO_CIRCUIT {
    PAPOLLO_EP          Ep;
} APOLLO_CIRCUIT, *PAPOLLO_CIRCUIT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(APOLLO_CIRCUIT, ApolloGetCircuit)

/* ACXSTREAM context. */
struct _APOLLO_STREAM {
    LIST_ENTRY          Link;               /* APOLLO_ACX.Active while running */
    PDEVICE_CONTEXT     Ctx;
    PAPOLLO_EP          Ep;
    ACXSTREAM           Stream;
    BOOLEAN             Render;
    ULONG               Rate;               /* [Rate] Hz of its format: a stream of another rate than the unit's is silent */
    ULONG               Channels;
    ULONG               BlockAlign;         /* 4 * Channels: 32-bit samples, 24 valid */
    ULONG               Slot[2];
    BOOLEAN             SlotsProvisional;   /* resolved before the unit had a layout: again at Run */

    /* Packets (allocated and freed at PASSIVE, read by the tick only while Active) */
    PACX_RTPACKET       Packets;
    PUCHAR              Buffer;             /* one page-aligned buffer for all packets */
    PMDL                Mdl;
    ULONG               BufferBytes;
    ULONG               PacketCount;        /* 1 or 2 */
    ULONG               PacketBytes;        /* RtPacketSize */
    ULONG               FramesPerPacket;
    ULONG               BufferFrames;       /* frames in the cyclic buffer */
    BOOLEAN             Prepared;           /* counted in APOLLO_ACX.Prepared */

    /* Engine state, EngineLock */
    BOOLEAN             Active;             /* in APOLLO_ACX.Active */
    BOOLEAN             NeedSync;           /* place HwBase at the next tick */
    BOOLEAN             UsesWritePacket;    /* the client calls SetRenderPacket */
    BOOLEAN             EosSet;
    BOOLEAN             ReadStarted;        /* capture: the OS has called GetCapturePacket */
    LONGLONG            HwBase;             /* hardware frame of stream frame 0 */
    ULONGLONG           Frames;             /* render: consumed; capture: produced */
    ULONGLONG           Completed;          /* packets completed */
    ULONGLONG           Notified;           /* packets notified (< Completed: still to send, in order) */
    ULONGLONG           EosFrame;
    ULONGLONG           LastPosition;       /* presentation position never goes back */
    ULONGLONG           ReadNext;           /* capture: next packet GetCapturePacket hands out */
    ULONGLONG           UnderrunPacket;     /* render: last packet counted as an underrun, +1 */
    LONGLONG            WrittenPacket;      /* render: highest packet the client released, -1 */
    LONGLONG            PacketQpc[2];       /* capture: QPC of packet p's first frame, at [p % PacketCount] */

    EX_RUNDOWN_REF      Rundown;            /* held by the tick across a notification */

    /*
     * A combined endpoint's stream (Ep->Desc->AggRow1 != 0): Link is in the aggregate's list,
     * the engine fields above are under Agg.Lock (ApolloAggStreamLock) and HwBase counts timeline
     * frames; AggRef (Agg.Lock) is where its channels come from or go to, from the configuration.
     */
    BOOLEAN             Agg;
    APOLLO_AGG_SREF     AggRef;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(APOLLO_STREAM, ApolloGetStream)

/* Packet bookkeeping shared by the standalone (stream.c) and combined (agg.c) streams, under the stream's lock. */

/* Render: every packet the hardware side has consumed is complete (notified in order). */
static __inline VOID ApolloStreamComplete(_Inout_ PAPOLLO_STREAM S)
{
    while (S->Frames >= (S->Completed + 1) * S->FramesPerPacket) {
        S->Completed++;
        S->Ep->Packets++;
    }
}

/*
 * Capture: the lowest packet whose memory is still intact. Packet p lives at [p % PacketCount], so the
 * packet being written (Completed) reuses the memory of Completed - PacketCount as soon as its first
 * frame lands. With one packet nothing completed stays intact once the next one starts.
 */
static __inline ULONGLONG ApolloStreamOldestIntact(_In_ const APOLLO_STREAM *S)
{
    ULONGLONG oldest;

    if (S->Completed < S->PacketCount) {
        return 0;
    }
    oldest = S->Completed - S->PacketCount;
    if (S->Frames > S->Completed * S->FramesPerPacket) {
        oldest++;
    }
    return oldest;
}

/*
 * agg.c: combined streams. Attach/Detach/Clock with ApolloAggStreamLock() held (DISPATCH);
 * Latency at PASSIVE (it takes the lock). Lock order: nothing else is held with it here.
 */
PKSPIN_LOCK ApolloAggStreamLock(VOID);
BOOLEAN  ApolloAggStreamsReady(VOID);
VOID     ApolloAggStreamAttach(_Inout_ PAPOLLO_STREAM S);
VOID     ApolloAggStreamDetach(_Inout_ PAPOLLO_STREAM S);
BOOLEAN  ApolloAggStreamClock(_Out_ PULONGLONG Pos, _Out_ PLONGLONG Qpc, _Out_ PLONGLONG Freq);
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID     ApolloAggStreamLatency(_Out_ PULONG LeadFrames, _Out_ PULONG GuardFrames, _Out_ PULONG AdcFrames,
                                _Out_ PULONG InPathFrames, _Out_ PULONG DacFrames);     /* [Lat] + the device side */

/* stream.c: the circuits' create-stream callback */
EVT_ACX_CIRCUIT_CREATE_STREAM ApolloEvtCircuitCreateStream;
