/*
 * stream.c - ACX RT streams on the endpoint circuits, and their part of the
 * 1 ms engine tick (docs/PHASE2.md sections 4 and 5.4).
 *
 * Each stream owns one page-aligned buffer holding its RT packets (1 or 2).
 * The engine tick, under EngineLock:
 *   render   adds (saturating) stream frames into the playback ring for the
 *            hardware frames [data head, SAMPLE_POS + lead); stream frame s
 *            lands on hardware frame HwBase + s;
 *   capture  copies record-ring frames up to SAMPLE_POS - guard into the
 *            stream buffer;
 * and marks a packet complete when the stream crosses a packet boundary. The
 * completion is sent with AcxRtStreamNotifyPacketComplete after the lock is
 * dropped, under the stream's rundown protection, so Pause can wait for it.
 * Positions and QPC come from the engine's snapshot of one SAMPLE_POS read.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "apollo_stream.h"

#define STREAM_MAX_LAG      4096u       /* capture lag that forces a resync */

static EVT_ACX_STREAM_PREPARE_HARDWARE          StreamPrepareHardware;
static EVT_ACX_STREAM_RELEASE_HARDWARE          StreamReleaseHardware;
static EVT_ACX_STREAM_RUN                       StreamRun;
static EVT_ACX_STREAM_PAUSE                     StreamPause;
static EVT_ACX_STREAM_ALLOCATE_RTPACKETS        StreamAllocateRtPackets;
static EVT_ACX_STREAM_FREE_RTPACKETS            StreamFreeRtPackets;
static EVT_ACX_STREAM_GET_HW_LATENCY            StreamGetHwLatency;
static EVT_ACX_STREAM_SET_RENDER_PACKET         StreamSetRenderPacket;
static EVT_ACX_STREAM_GET_CURRENT_PACKET        StreamGetCurrentPacket;
static EVT_ACX_STREAM_GET_CAPTURE_PACKET        StreamGetCapturePacket;
static EVT_ACX_STREAM_GET_PRESENTATION_POSITION StreamGetPresentationPosition;
static EVT_WDF_OBJECT_CONTEXT_CLEANUP           StreamCleanup;


/* QPC at which hardware frame Frame passed SAMPLE_POS (EngineLock held). */
static LONGLONG StreamQpcAt(_In_ PDEVICE_CONTEXT Ctx, _In_ LONGLONG Frame)
{
    LONGLONG back = (LONGLONG)Ctx->EngPos - Frame;

    if (Ctx->SampleRate == 0) {
        return Ctx->EngQpc;
    }
    return Ctx->EngQpc - back * Ctx->EngQpcFreq / (LONGLONG)Ctx->SampleRate;
}

/*
 * The lock a stream's engine state is under: its unit's EngineLock (standalone rows), or the
 * aggregate's lock for a combined row, whose stream runs on the timeline tick (agg.c).
 */
static PKSPIN_LOCK StreamLockOf(_In_ PAPOLLO_STREAM S)
{
    return S->Agg ? ApolloAggStreamLock() : &S->Ctx->EngineLock;
}

/* An Active stream off its tick's list (StreamLockOf held). */
static VOID StreamUnlink(_Inout_ PAPOLLO_STREAM S)
{
    if (S->Agg) {
        ApolloAggStreamDetach(S);               /* off the timeline tick's list */
    } else {
        RemoveEntryList(&S->Link);
        InitializeListHead(&S->Link);
    }
    S->Active = FALSE;
}

/* Leave the engine and wait until no notification for this stream is in flight. PASSIVE. */
_IRQL_requires_(PASSIVE_LEVEL)
static VOID StreamDeactivate(_In_ PAPOLLO_STREAM S)
{
    PKSPIN_LOCK lock = StreamLockOf(S);
    BOOLEAN was;
    KIRQL irql;

    KeAcquireSpinLock(lock, &irql);
    was = S->Active;
    if (was) {
        StreamUnlink(S);
    }
    S->Notified = S->Completed;                 /* no completions after Pause */
    KeReleaseSpinLock(lock, irql);
    if (was) {
        ExWaitForRundownProtectionRelease(&S->Rundown);
        ExReInitializeRundownProtection(&S->Rundown);
    }
}

static VOID StreamResetCounters(_Inout_ PAPOLLO_STREAM S)
{
    S->Frames = 0;
    S->Completed = 0;
    S->Notified = 0;
    S->UsesWritePacket = FALSE;
    S->EosSet = FALSE;
    S->EosFrame = MAXULONGLONG;
    S->LastPosition = 0;
    S->ReadStarted = FALSE;
    S->ReadNext = 0;
    S->UnderrunPacket = 0;
    S->WrittenPacket = -1;
    S->PacketQpc[0] = S->PacketQpc[1] = 0;
}

/* ------------------------------------------------------------------ */
/* Stream creation (PASSIVE)                                           */
/* ------------------------------------------------------------------ */

_Use_decl_annotations_
NTSTATUS ApolloEvtCircuitCreateStream(WDFDEVICE Device, ACXCIRCUIT Circuit, ACXPIN Pin,
                                      PACXSTREAM_INIT StreamInit, ACXDATAFORMAT StreamFormat,
                                      const GUID *SignalProcessingMode, ACXOBJECTBAG VarArguments)
{
    PDEVICE_CONTEXT ctx = ApolloGetContext(Device);
    PAPOLLO_CIRCUIT cc = ApolloGetCircuit(Circuit);
    PAPOLLO_EP ep = cc->Ep;
    ACX_STREAM_CALLBACKS sc;
    ACX_RT_STREAM_CALLBACKS rc;
    WDF_OBJECT_ATTRIBUTES attrs;
    ACXSTREAM stream;
    PAPOLLO_STREAM s;
    GUID sub;
    NTSTATUS status;
    KIRQL irql;

    UNREFERENCED_PARAMETER(Pin);
    UNREFERENCED_PARAMETER(SignalProcessingMode);
    UNREFERENCED_PARAMETER(VarArguments);

    if (ep == NULL || ep->Desc == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (ep->Desc->Channels < 1 || ep->Desc->Channels > RTL_NUMBER_OF_FIELD(APOLLO_STREAM, Slot)) {
        return STATUS_NOT_SUPPORTED;            /* a table row the copy loops cannot serve */
    }
    sub = AcxDataFormatGetSubFormat(StreamFormat);
    /* [Rate] the rate the endpoint advertises now (circuit.c ApolloAcxSetRate; 48 kHz unless changed) */
    if (AcxDataFormatGetSampleRate(StreamFormat) != ep->FmtRate || ep->FmtRate == 0 ||
        AcxDataFormatGetChannelsCount(StreamFormat) != ep->Desc->Channels ||
        AcxDataFormatGetBitsPerSample(StreamFormat) != 32 ||
        AcxDataFormatGetValidBitsPerSample(StreamFormat) != 24 ||
        !IsEqualGUID(&sub, &KSDATAFORMAT_SUBTYPE_PCM)) {
        return STATUS_NOT_SUPPORTED;
    }
    /*
     * ep->Stream is for statistics only. The host pin's MaxStreams = 1 is what keeps
     * one stream per endpoint; a new stream may arrive before the old one's cleanup
     * ran, so it simply takes the row over (StreamCleanup clears it only if still its own).
     */

    ACX_STREAM_CALLBACKS_INIT(&sc);
    sc.EvtAcxStreamPrepareHardware = StreamPrepareHardware;
    sc.EvtAcxStreamReleaseHardware = StreamReleaseHardware;
    sc.EvtAcxStreamRun = StreamRun;
    sc.EvtAcxStreamPause = StreamPause;
    status = AcxStreamInitAssignAcxStreamCallbacks(StreamInit, &sc);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    ACX_RT_STREAM_CALLBACKS_INIT(&rc);
    rc.EvtAcxStreamGetHwLatency = StreamGetHwLatency;
    rc.EvtAcxStreamAllocateRtPackets = StreamAllocateRtPackets;
    rc.EvtAcxStreamFreeRtPackets = StreamFreeRtPackets;
    rc.EvtAcxStreamGetCurrentPacket = StreamGetCurrentPacket;
    rc.EvtAcxStreamGetPresentationPosition = StreamGetPresentationPosition;
    if (ep->Desc->Type == AcxCircuitTypeRender) {
        rc.EvtAcxStreamSetRenderPacket = StreamSetRenderPacket;
    } else {
        rc.EvtAcxStreamGetCapturePacket = StreamGetCapturePacket;
    }
    status = AcxStreamInitAssignAcxRtStreamCallbacks(StreamInit, &rc);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    AcxStreamInitSetAcxRtStreamSupportsNotifications(StreamInit);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, APOLLO_STREAM);
    attrs.EvtCleanupCallback = StreamCleanup;
    status = AcxRtStreamCreate(Device, Circuit, &attrs, &StreamInit, &stream);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    s = ApolloGetStream(stream);
    InitializeListHead(&s->Link);
    s->Ctx = ctx;
    s->Ep = ep;
    s->Stream = stream;
    s->Agg = (ep->Desc->AggRow1 != 0) ? TRUE : FALSE;  /* a combined row: the aggregate timeline */
    s->Render = (ep->Desc->Type == AcxCircuitTypeRender);
    s->Rate = AcxDataFormatGetSampleRate(StreamFormat);    /* [Rate] */
    s->Channels = ep->Desc->Channels;
    s->BlockAlign = 4 * s->Channels;
    s->Slot[0] = ep->Desc->FallbackSlot[0];
    s->Slot[1] = ep->Desc->FallbackSlot[1];
    StreamResetCounters(s);
    ExInitializeRundownProtection(&s->Rundown);

    KeAcquireSpinLock(&ctx->EngineLock, &irql);
    ep->Stream = s;
    ep->Streams++;
    KeReleaseSpinLock(&ctx->EngineLock, irql);
    ApolloLogU(ctx, "acx: stream on %ws (%s)", ep->Desc->RefString, s->Render ? "render" : "capture");
    return STATUS_SUCCESS;
}

/*
 * The ACXSTREAM is going away: make sure the engine no longer sees it and no
 * notification is in flight. ACX deletes streams at PASSIVE_LEVEL, so the
 * rundown wait always runs. The packet memory is NOT freed here: ACX may still
 * hold the MDL in the packet array and have the buffer mapped into audiodg;
 * only FreeRtPackets releases it (as the ACX sample does).
 */
_Use_decl_annotations_
static VOID StreamCleanup(WDFOBJECT Object)
{
    PAPOLLO_STREAM s = ApolloGetStream((ACXSTREAM)Object);
    PDEVICE_CONTEXT ctx = s->Ctx;
    KIRQL irql;

    if (ctx == NULL) {
        return;                                 /* creation never finished */
    }
    NT_ASSERTMSG("ACX stream cleanup above PASSIVE_LEVEL", KeGetCurrentIrql() == PASSIVE_LEVEL);
    if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
        StreamDeactivate(s);                    /* waits for a notification in flight */
    } else {
        /* Not expected (ACX deletes at PASSIVE): at least take it off the engine's list. */
        PKSPIN_LOCK lock = StreamLockOf(s);

        KeAcquireSpinLock(lock, &irql);
        if (s->Active) {
            StreamUnlink(s);
        }
        KeReleaseSpinLock(lock, irql);
        ApolloLogU(ctx, "acx: stream cleanup at IRQL %u, no rundown wait", (ULONG)KeGetCurrentIrql());
    }
    KeAcquireSpinLock(&ctx->EngineLock, &irql);
    if (s->Ep != NULL && s->Ep->Stream == s) {
        s->Ep->Stream = NULL;
    }
    KeReleaseSpinLock(&ctx->EngineLock, irql);
    if (s->Prepared) {
        s->Prepared = FALSE;
        InterlockedDecrement(&ctx->Acx->Prepared);
    }
    if (s->Buffer != NULL) {
        /*
         * FreeRtPackets never ran: do not free the memory under a mapping ACX may
         * still hold. Park it on the device; ApolloAcxContextDestroy frees it once
         * the device object, and every ACX object under it, is gone.
         */
        PAPOLLO_PARKED pk = (ctx->Acx != NULL)
            ? (PAPOLLO_PARKED)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(APOLLO_PARKED), APOLLO_STREAM_TAG)
            : NULL;

        if (pk != NULL) {
            pk->Packets = s->Packets;
            pk->Buffer = s->Buffer;
            pk->Mdl = s->Mdl;
            KeAcquireSpinLock(&ctx->Acx->ParkLock, &irql);
            InsertTailList(&ctx->Acx->Parked, &pk->Link);
            KeReleaseSpinLock(&ctx->Acx->ParkLock, irql);
            s->Packets = NULL;
            s->Buffer = NULL;
            s->Mdl = NULL;
        }
        if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
            ApolloLogU(ctx, "acx: stream on %ws deleted before FreeRtPackets (%lu bytes): %s",
                      (s->Ep != NULL && s->Ep->Desc != NULL) ? s->Ep->Desc->RefString : L"?",
                      s->BufferBytes, (pk != NULL) ? "parked" : "LEAKED");
        } else {
            /* No %ws above PASSIVE: the Unicode conversion may touch pageable data. */
            ApolloLogU(ctx, "acx: stream deleted before FreeRtPackets at IRQL %u (%lu bytes): %s",
                      (ULONG)KeGetCurrentIrql(), s->BufferBytes, (pk != NULL) ? "parked" : "LEAKED");
        }
    }
}

/*
 * FreeRtPackets after StreamCleanup parked this stream's memory (ACX may call it
 * from its own cleanup, after ours): take the entry back off the park list and
 * free it now, ACX is done with the mapping. TRUE if Packets was parked.
 */
_IRQL_requires_(PASSIVE_LEVEL)
static BOOLEAN StreamFreeParked(_In_ PDEVICE_CONTEXT Ctx, _In_ PACX_RTPACKET Packets)
{
    PAPOLLO_PARKED found = NULL;
    PLIST_ENTRY e;
    KIRQL irql;

    if (Ctx == NULL || Ctx->Acx == NULL || Packets == NULL) {
        return FALSE;
    }
    KeAcquireSpinLock(&Ctx->Acx->ParkLock, &irql);
    for (e = Ctx->Acx->Parked.Flink; e != &Ctx->Acx->Parked; e = e->Flink) {
        PAPOLLO_PARKED pk = CONTAINING_RECORD(e, APOLLO_PARKED, Link);

        if (pk->Packets == Packets) {
            RemoveEntryList(e);
            found = pk;
            break;
        }
    }
    KeReleaseSpinLock(&Ctx->Acx->ParkLock, irql);
    if (found == NULL) {
        return FALSE;
    }
    if (found->Mdl != NULL) {
        IoFreeMdl(found->Mdl);
    }
    if (found->Buffer != NULL) {
        ExFreePoolWithTag(found->Buffer, APOLLO_STREAM_TAG);
    }
    ExFreePoolWithTag(found->Packets, APOLLO_STREAM_TAG);
    ExFreePoolWithTag(found, APOLLO_STREAM_TAG);
    ApolloLogU(Ctx, "acx: FreeRtPackets after cleanup: freed the parked packet memory");
    return TRUE;
}

/* APOLLO_ACX context destroy: the device object is going, every circuit and stream under it is gone. */
_Use_decl_annotations_
VOID ApolloAcxContextDestroy(WDFOBJECT Object)
{
    PAPOLLO_ACX acx = ApolloGetAcx(Object);
    ULONG n = 0;

    /* No lock: nothing else can reach this context any more. */
    while (acx->Parked.Flink != NULL && !IsListEmpty(&acx->Parked)) {
        PAPOLLO_PARKED pk = CONTAINING_RECORD(RemoveHeadList(&acx->Parked), APOLLO_PARKED, Link);

        if (pk->Mdl != NULL) {
            IoFreeMdl(pk->Mdl);
        }
        if (pk->Buffer != NULL) {
            ExFreePoolWithTag(pk->Buffer, APOLLO_STREAM_TAG);
        }
        if (pk->Packets != NULL) {
            ExFreePoolWithTag(pk->Packets, APOLLO_STREAM_TAG);
        }
        ExFreePoolWithTag(pk, APOLLO_STREAM_TAG);
        n++;
    }
    if (n != 0) {
        ApolloLog("acx: freed %lu parked packet buffer(s)", n);
    }
}

/* ------------------------------------------------------------------ */
/* Packets (PASSIVE): one buffer, both packets inside it               */
/* ------------------------------------------------------------------ */

_Use_decl_annotations_
static NTSTATUS StreamAllocateRtPackets(ACXSTREAM Stream, ULONG PacketCount, ULONG PacketSize,
                                        PACX_RTPACKET *Packets)
{
    PAPOLLO_STREAM s = ApolloGetStream(Stream);
    PACX_RTPACKET pk;
    PUCHAR buf;
    PMDL mdl;
    ULONG len, size;

    *Packets = NULL;
    if (PacketCount < 1 || PacketCount > 2 || PacketSize == 0 || PacketSize % s->BlockAlign != 0 ||
        PacketSize > 16u * 1024 * 1024) {
        return STATUS_INVALID_PARAMETER;
    }
    if (s->Buffer != NULL || s->Active) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (PacketCount == 2) {
        /* Event-driven: packet 1 follows packet 0 in the same buffer (Invalid descriptor). */
        size = PacketSize;
        len = (ULONG)ROUND_TO_PAGES((SIZE_T)2 * PacketSize);
    } else {
        /* Timer-driven: ACX maps the buffer twice back to back, so it must be whole pages. */
        len = (ULONG)ROUND_TO_PAGES((SIZE_T)PacketSize);
        while (len % s->BlockAlign != 0) {
            len += PAGE_SIZE;
        }
        size = len;
    }

    pk = (PACX_RTPACKET)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(ACX_RTPACKET) * 2,
                                        APOLLO_STREAM_TAG);
    buf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, len, APOLLO_STREAM_TAG);   /* zeroed, page-aligned */
    mdl = (buf != NULL) ? IoAllocateMdl(buf, len, FALSE, FALSE, NULL) : NULL;
    if (pk == NULL || buf == NULL || mdl == NULL) {
        if (mdl != NULL) {
            IoFreeMdl(mdl);
        }
        if (buf != NULL) {
            ExFreePoolWithTag(buf, APOLLO_STREAM_TAG);
        }
        if (pk != NULL) {
            ExFreePoolWithTag(pk, APOLLO_STREAM_TAG);
        }
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    MmBuildMdlForNonPagedPool(mdl);

    ACX_RTPACKET_INIT(&pk[0]);
    WDF_MEMORY_DESCRIPTOR_INIT_MDL(&pk[0].RtPacketBuffer, mdl, len);
    pk[0].RtPacketOffset = 0;
    pk[0].RtPacketSize = size;
    ACX_RTPACKET_INIT(&pk[1]);
    pk[1].RtPacketBuffer.Type = WdfMemoryDescriptorTypeInvalid;
    pk[1].RtPacketOffset = size;
    pk[1].RtPacketSize = size;

    s->Packets = pk;
    s->Buffer = buf;
    s->Mdl = mdl;
    s->BufferBytes = len;
    s->PacketCount = PacketCount;
    s->PacketBytes = size;
    s->FramesPerPacket = size / s->BlockAlign;
    s->BufferFrames = (PacketCount * size) / s->BlockAlign;
    *Packets = pk;
    ApolloLogU(s->Ctx, "acx: %ws packets %lu x %lu bytes (%lu frames each)", s->Ep->Desc->RefString,
              PacketCount, size, s->FramesPerPacket);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
static VOID StreamFreeRtPackets(ACXSTREAM Stream, PACX_RTPACKET Packets, ULONG PacketCount)
{
    PAPOLLO_STREAM s = ApolloGetStream(Stream);

    UNREFERENCED_PARAMETER(PacketCount);
    StreamDeactivate(s);                        /* normally already paused */
    if (s->Buffer == NULL && s->Packets == NULL) {
        /* Nothing of ours left on the stream: either cleanup parked it, or there is nothing. */
        if (Packets != NULL && !StreamFreeParked(s->Ctx, Packets)) {
            ApolloLogU(s->Ctx, "acx: FreeRtPackets with an array this stream does not own: left alone");
        }
        s->BufferFrames = 0;
        s->FramesPerPacket = 0;
        return;
    }
    if (Packets != NULL && Packets != s->Packets) {
        ApolloLogU(s->Ctx, "acx: FreeRtPackets array differs from the allocated one: freeing the allocated one");
    }
    if (s->Mdl != NULL) {
        IoFreeMdl(s->Mdl);
        s->Mdl = NULL;
    }
    if (s->Buffer != NULL) {
        ExFreePoolWithTag(s->Buffer, APOLLO_STREAM_TAG);
        s->Buffer = NULL;
    }
    if (s->Packets != NULL) {
        ExFreePoolWithTag(s->Packets, APOLLO_STREAM_TAG);
    }
    s->Packets = NULL;
    s->BufferFrames = 0;
    s->FramesPerPacket = 0;
}

/* ------------------------------------------------------------------ */
/* State changes (PASSIVE)                                             */
/* ------------------------------------------------------------------ */

/*
 * The stream's ring slots by channel ID (endpoints.c ApolloEndpointSlot), PASSIVE, while the
 * stream is not in the engine's Active list. A unit whose layout comes from the IO
 * descriptor at connect (the Twin X: no slot counts until G5 ran) may see a stream prepared
 * before that: its slots are marked provisional and resolved again at Run. The x8p has its
 * legacy counts from PrepareHardware on, so it never takes that path.
 */
static VOID StreamResolveSlots(_Inout_ PAPOLLO_STREAM S)
{
    PDEVICE_CONTEXT ctx = S->Ctx;
    ULONG c;

    for (c = 0; c < S->Channels && c < 2; c++) {
        S->Slot[c] = ApolloEndpointSlot(ctx, S->Ep->Desc, c);
    }
    if (S->Channels == 1) {
        S->Slot[1] = S->Slot[0];                /* mono: GET_ENDPOINTS shows one slot twice */
    }
    S->SlotsProvisional = (ctx->PlayChannels == 0 || ctx->RecChannels == 0) ? TRUE : FALSE;
}

_Use_decl_annotations_
static NTSTATUS StreamPrepareHardware(ACXSTREAM Stream)
{
    PAPOLLO_STREAM s = ApolloGetStream(Stream);
    PDEVICE_CONTEXT ctx = s->Ctx;
    KIRQL irql;

    if (s->Buffer == NULL || s->FramesPerPacket == 0) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (s->Agg) {
        /* slots come from the aggregate's configuration at Run (agg.c ApolloAggStreamAttach) */
        PKSPIN_LOCK lock = ApolloAggStreamLock();

        s->SlotsProvisional = FALSE;
        KeAcquireSpinLock(lock, &irql);
        StreamResetCounters(s);
        KeReleaseSpinLock(lock, irql);
    } else {
        StreamResolveSlots(s);
        KeAcquireSpinLock(&ctx->EngineLock, &irql);
        StreamResetCounters(s);
        KeReleaseSpinLock(&ctx->EngineLock, irql);
    }
    if (!s->Prepared) {
        s->Prepared = TRUE;
        InterlockedIncrement(&ctx->Acx->Prepared);
    }
    /* The transport runs from connect (PHASE2.md 5.6); if it does not, positions just wait. */
    ApolloLogU(ctx, "acx: %ws prepared, slots %lu/%lu", s->Ep->Desc->RefString, s->Slot[0],
              s->Channels > 1 ? s->Slot[1] : s->Slot[0]);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
static NTSTATUS StreamReleaseHardware(ACXSTREAM Stream)
{
    PAPOLLO_STREAM s = ApolloGetStream(Stream);
    PDEVICE_CONTEXT ctx = s->Ctx;
    PKSPIN_LOCK lock = StreamLockOf(s);
    KIRQL irql;

    StreamDeactivate(s);
    KeAcquireSpinLock(lock, &irql);
    StreamResetCounters(s);                     /* packet numbers restart at 0 */
    KeReleaseSpinLock(lock, irql);
    if (s->Prepared) {
        s->Prepared = FALSE;
        InterlockedDecrement(&ctx->Acx->Prepared);
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
static NTSTATUS StreamRun(ACXSTREAM Stream)
{
    PAPOLLO_STREAM s = ApolloGetStream(Stream);
    PDEVICE_CONTEXT ctx = s->Ctx;
    KIRQL irql;

    if (s->Buffer == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (s->Agg) {
        /*
         * A combined endpoint: onto the aggregate timeline's list. Its sources/destinations come
         * from the configuration (routes, master) and follow it while it runs; positions count
         * timeline frames. With no timeline (no unit running) the position stands still.
         */
        PKSPIN_LOCK lock = ApolloAggStreamLock();

        if (!ApolloAggStreamsReady()) {
            return STATUS_DEVICE_NOT_READY;
        }
        KeAcquireSpinLock(lock, &irql);
        if (!s->Active) {
            s->NeedSync = TRUE;
            s->Active = TRUE;
            ApolloAggStreamAttach(s);
        }
        KeReleaseSpinLock(lock, irql);
        ApolloLogU(ctx, "acx: %ws runs on the combined timeline, channel slots %lu/%lu (first source or destination)",
                  s->Ep->Desc->RefString, s->Slot[0], s->Slot[1]);
        return STATUS_SUCCESS;
    }
    if (s->SlotsProvisional && !s->Active) {
        /* prepared before the unit's layout was known: take the live descriptor's slots now */
        StreamResolveSlots(s);
        ApolloLogU(ctx, "acx: %ws slots resolved again at run: %lu/%lu%s", s->Ep->Desc->RefString, s->Slot[0],
                  s->Slot[1], s->SlotsProvisional ?
                  " (no layout yet: placeholder slots, resolved again when the engine starts)" : "");
    }
    KeAcquireSpinLock(&ctx->EngineLock, &irql);
    if (!s->Active) {
        s->NeedSync = TRUE;                     /* the next tick places it on the hardware clock */
        s->Active = TRUE;
        InsertTailList(&ctx->Acx->Active, &s->Link);
    }
    KeReleaseSpinLock(&ctx->EngineLock, irql);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
static NTSTATUS StreamPause(ACXSTREAM Stream)
{
    StreamDeactivate(ApolloGetStream(Stream)); /* position is kept; Run resyncs */
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Queries (PASSIVE)                                                   */
/* ------------------------------------------------------------------ */

_Use_decl_annotations_
static NTSTATUS StreamGetHwLatency(ACXSTREAM Stream, ULONG *FifoSize, ULONG *Delay)
{
    PAPOLLO_STREAM s = ApolloGetStream(Stream);
    PAPOLLO_ACX acx = s->Ctx->Acx;
    /* the model's ADC latency (x8p 10 frames, Twin X 5); [Rate] at the stream's rate */
    ULONG adc = (acx != NULL && acx->Table != NULL) ? acx->Table->AdcFrames : APOLLO_ADC_FRAMES;
    ULONG rate = (s->Rate != 0) ? s->Rate : APOLLO_RATE_DEFAULT_HZ;

    adc = ApolloRateAdc(adc, ApolloRateIndex(rate));

    /*
     * [Lat] Windows asks once, when it creates the stream: the device side beyond the FIFO is in Delay (100 ns):
     * capture the ADC plus the strip path while the strips run (DSP block, record delay, an over-budget chain),
     * render the model's DAC (docs/ASIO.md, Latency). A chain added later counts for streams created after it.
     */
    if (s->Agg) {
        /* combined: the timeline's lead; capture trails it by guard + pad (aligned), the timeline's ADC */
        ULONG lead, guard, path, dac;

        ApolloAggStreamLatency(&lead, &guard, &adc, &path, &dac);
        *FifoSize = (s->Render ? lead : guard) * s->BlockAlign;
        *Delay = (ULONG)((s->Render ? dac : adc + path) * 10000000ULL / rate);
        return STATUS_SUCCESS;
    }
    if (s->Render) {
        *FifoSize = s->Ctx->EngLead * s->BlockAlign;            /* written this far ahead */
        *Delay = (ULONG)(s->Ctx->DacFrames * 10000000ULL / rate);   /* [Lat] the DAC (was 0: unknown) */
    } else {
        *FifoSize = s->Ctx->EngGuard * s->BlockAlign;
        *Delay = (ULONG)((adc + s->Ctx->LatInPath) * 10000000ULL / rate);  /* 100 ns units; [Lat] + the strip path */
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
static NTSTATUS StreamGetCurrentPacket(ACXSTREAM Stream, PULONG CurrentPacket)
{
    PAPOLLO_STREAM s = ApolloGetStream(Stream);
    PKSPIN_LOCK lock = StreamLockOf(s);
    KIRQL irql;

    KeAcquireSpinLock(lock, &irql);
    *CurrentPacket = s->FramesPerPacket ? (ULONG)(s->Frames / s->FramesPerPacket) : 0;
    KeReleaseSpinLock(lock, irql);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
static NTSTATUS StreamGetCapturePacket(ACXSTREAM Stream, PULONG LastCapturePacket,
                                       PULONGLONG QPCPacketStart, PBOOLEAN MoreData)
{
    PAPOLLO_STREAM s = ApolloGetStream(Stream);
    PKSPIN_LOCK lock = StreamLockOf(s);
    ULONGLONG p, oldest;
    KIRQL irql;

    /*
     * Hand out completed packets in order, oldest unread first, with MoreData
     * when more are waiting. Only packets whose memory is still intact are
     * offered (ApolloStreamOldestIntact): the packet being written reuses the memory
     * of packet p - PacketCount from its first frame. Before the first call
     * nothing is counted; after it, the tick counts the packets it overwrote
     * unread as overruns and moves ReadNext past them.
     */
    KeAcquireSpinLock(lock, &irql);
    s->ReadStarted = TRUE;
    if (s->Completed == 0 || s->PacketCount == 0) {
        *LastCapturePacket = 0xFFFFFFFF;                /* nothing yet, as the sample */
        *QPCPacketStart = 0;
        *MoreData = FALSE;
    } else {
        oldest = ApolloStreamOldestIntact(s);
        if (s->ReadNext < oldest) {
            s->ReadNext = oldest;
        }
        if (s->ReadNext >= s->Completed) {
            p = s->Completed - 1;                       /* asked again without a new packet */
        } else {
            p = s->ReadNext++;
        }
        *LastCapturePacket = (ULONG)p;
        *QPCPacketStart = (ULONGLONG)s->PacketQpc[p % s->PacketCount];
        *MoreData = (s->ReadNext < s->Completed);
    }
    KeReleaseSpinLock(lock, irql);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
static NTSTATUS StreamGetPresentationPosition(ACXSTREAM Stream, PULONGLONG PositionInBlocks,
                                              PULONGLONG QPCPosition)
{
    PAPOLLO_STREAM s = ApolloGetStream(Stream);
    PDEVICE_CONTEXT ctx = s->Ctx;
    ULONGLONG pos = s->Frames;
    LONGLONG qpc;
    KIRQL irql;

    if (s->Agg) {
        /* a combined stream counts timeline frames: the aggregate's snapshot of the last tick */
        PKSPIN_LOCK lock = ApolloAggStreamLock();
        ULONGLONG tpos;
        LONGLONG tqpc, tfreq;

        KeAcquireSpinLock(lock, &irql);
        pos = s->Frames;
        if (s->Active && !s->NeedSync && ApolloAggStreamClock(&tpos, &tqpc, &tfreq)) {
            LONGLONG played = (LONGLONG)tpos - s->HwBase;

            if (s->Render) {
                pos = played <= 0 ? 0 : min((ULONGLONG)played, s->Frames);
            }
            qpc = tqpc - ((LONGLONG)tpos - (s->HwBase + (LONGLONG)pos)) * tfreq /
                  (LONGLONG)(s->Rate != 0 ? s->Rate : APOLLO_RATE_DEFAULT_HZ);     /* [Rate] the timeline's */
        } else {
            qpc = KeQueryPerformanceCounter(NULL).QuadPart;
        }
        if (pos < s->LastPosition) {
            pos = s->LastPosition;              /* after a resync (timeline change, Pause/Run) */
        }
        s->LastPosition = pos;
        KeReleaseSpinLock(lock, irql);
        *PositionInBlocks = pos;
        *QPCPosition = (ULONGLONG)qpc;
        return STATUS_SUCCESS;
    }
    KeAcquireSpinLock(&ctx->EngineLock, &irql);
    pos = s->Frames;
    if (s->Active && !s->NeedSync && ApolloEngineRunning(ctx)) {
        LONGLONG played = (LONGLONG)ctx->EngPos - s->HwBase;   /* frames the hardware fetched */

        if (s->Render) {
            pos = played <= 0 ? 0 : min((ULONGLONG)played, s->Frames);
        }
        qpc = StreamQpcAt(ctx, s->HwBase + (LONGLONG)pos);
    } else {
        qpc = KeQueryPerformanceCounter(NULL).QuadPart;
    }
    if (pos < s->LastPosition) {
        pos = s->LastPosition;                  /* after a Pause/Run resync */
    }
    s->LastPosition = pos;
    KeReleaseSpinLock(&ctx->EngineLock, irql);
    *PositionInBlocks = pos;
    *QPCPosition = (ULONGLONG)qpc;
    return STATUS_SUCCESS;
}

/*
 * The client released packet Packet (0-based, monotonic). It should be the one
 * after the packet the engine is reading. Data arriving for a packet already
 * being read is still used for the frames not yet copied (PHASE2.md 4.4).
 */
_Use_decl_annotations_
static NTSTATUS StreamSetRenderPacket(ACXSTREAM Stream, ULONG Packet, ULONG Flags,
                                      ULONG EosPacketLength)
{
    PAPOLLO_STREAM s = ApolloGetStream(Stream);
    PKSPIN_LOCK lock = StreamLockOf(s);
    NTSTATUS status = STATUS_SUCCESS;
    ULONGLONG cur, into;
    KIRQL irql;

    if ((Flags & ~(ULONG)KSSTREAM_HEADER_OPTIONSF_ENDOFSTREAM) != 0 || s->FramesPerPacket == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    KeAcquireSpinLock(lock, &irql);
    cur = s->Frames / s->FramesPerPacket;
    into = s->Frames % s->FramesPerPacket;
    if (s->EosSet) {
        status = STATUS_INVALID_DEVICE_STATE;
    } else if ((ULONGLONG)Packet > cur + 1) {
        status = STATUS_DATA_OVERRUN;
    } else {
        if ((ULONGLONG)Packet < cur || ((ULONGLONG)Packet == cur && into > 0)) {
            status = STATUS_DATA_LATE_ERROR;
            s->Ep->LatePackets++;
        }
        if ((LONGLONG)Packet > s->WrittenPacket) {
            s->WrittenPacket = (LONGLONG)Packet;
        }
        s->UsesWritePacket = TRUE;
        if (Flags & KSSTREAM_HEADER_OPTIONSF_ENDOFSTREAM) {
            s->EosSet = TRUE;
            s->EosFrame = (ULONGLONG)Packet * s->FramesPerPacket + EosPacketLength / s->BlockAlign;
        }
    }
    KeReleaseSpinLock(lock, irql);
    return status;
}

/* ------------------------------------------------------------------ */
/* Engine hooks (EngineLock held, DISPATCH_LEVEL)                      */
/* ------------------------------------------------------------------ */

static PAPOLLO_ACX StreamsOf(_In_ PDEVICE_CONTEXT Ctx)
{
    return (PAPOLLO_ACX)Ctx->Acx;
}

VOID ApolloStreamsEngineStarted(_In_ PDEVICE_CONTEXT Ctx)
{
    PAPOLLO_ACX acx = StreamsOf(Ctx);
    PLIST_ENTRY e;

    if (acx == NULL) {
        return;
    }
    for (e = acx->Active.Flink; e != &acx->Active; e = e->Flink) {
        PAPOLLO_STREAM s = CONTAINING_RECORD(e, APOLLO_STREAM, Link);

        s->NeedSync = TRUE;
        if (s->SlotsProvisional) {
            /*
             * It ran before the unit's layout was known (a Twin X stream opened while the
             * bring-up still ran): its slots are placeholders (slot 0 for both channels). The engine
             * starts only once the descriptor counts exist, so resolve them now, before its first
             * render or capture. ApolloEndpointSlot is non-paged and only reads the descriptor.
             */
            StreamResolveSlots(s);
            /* DISPATCH (EngineLock): no %ws here, the Unicode conversion may touch pageable data */
            ApolloLogU(Ctx, "acx: a provisional stream's slots resolved at engine start: %lu/%lu%s",
                      s->Slot[0], s->Slot[1], s->SlotsProvisional ? " (STILL no layout)" : "");
        }
    }
}

/*
 * [Rate] The unit's IO layout changed with its rate (audio.c ApolloAudioDescriptorsReread, DevLock,
 * engine stopped): every running stream resolves its slots again at the engine start, by channel ID.
 */
VOID ApolloStreamsLayoutChanged(_In_ PDEVICE_CONTEXT Ctx)
{
    PAPOLLO_ACX acx = StreamsOf(Ctx);
    PLIST_ENTRY e;
    KIRQL irql;

    if (acx == NULL) {
        return;
    }
    KeAcquireSpinLock(&Ctx->EngineLock, &irql);
    for (e = acx->Active.Flink; e != &acx->Active; e = e->Flink) {
        CONTAINING_RECORD(e, APOLLO_STREAM, Link)->SlotsProvisional = TRUE;
    }
    KeReleaseSpinLock(&Ctx->EngineLock, irql);
}

static VOID StreamRenderOne(_In_ PDEVICE_CONTEXT Ctx, _Inout_ PAPOLLO_STREAM S, _In_ ULONGLONG From,
                            _In_ ULONGLONG To)
{
    ULONG stride = Ctx->PlayChannels * 4, mask = Ctx->RingFrames - 1, idx, c;
    LONGLONG next;
    ULONGLONG h, frame, packetEnd = 0;          /* the next packet's first frame: one division per packet */
    BOOLEAN play = FALSE;
    /* [Rate] opened at another rate than the unit runs at now: it plays silence until Windows re-opens it */
    BOOLEAN stale = (S->Rate != Ctx->SampleRate) ? TRUE : FALSE;

    if (S->NeedSync) {
        S->HwBase = (LONGLONG)From - (LONGLONG)S->Frames;  /* stream frame Frames lands on From */
        S->NeedSync = FALSE;
    }
    next = S->HwBase + (LONGLONG)S->Frames;
    if (next < (LONGLONG)From) {
        /* The engine skipped frames after a late tick: they are gone for this stream too. */
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
        LONG *dst;
        const LONG *src;

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
            dst = (LONG *)(Ctx->Play.Va + (SIZE_T)(h & mask) * stride);
            src = (const LONG *)(S->Buffer + (SIZE_T)idx * S->BlockAlign);
            for (c = 0; c < S->Channels; c++) {
                ULONG sl = S->Slot[c];

                if (sl < Ctx->PlayChannels) {   /* [Endpoints] APOLLO_EP_SLOT_NONE: the ID is not on this unit */
                    dst[sl] = ApolloSatAdd32(dst[sl], src[c]);
                }
            }
        }
        if (++idx == S->BufferFrames) {
            idx = 0;
        }
    }
    S->Frames = frame;
    ApolloStreamComplete(S);
}

VOID ApolloStreamsRender(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONGLONG From, _In_ ULONGLONG To)
{
    PAPOLLO_ACX acx = StreamsOf(Ctx);
    PLIST_ENTRY e;

    if (acx == NULL) {
        return;
    }
    for (e = acx->Active.Flink; e != &acx->Active; e = e->Flink) {
        PAPOLLO_STREAM s = CONTAINING_RECORD(e, APOLLO_STREAM, Link);

        if (s->Render && s->BufferFrames != 0) {
            StreamRenderOne(Ctx, s, From, To);
        }
    }
}

static VOID StreamCaptureOne(_In_ PDEVICE_CONTEXT Ctx, _Inout_ PAPOLLO_STREAM S, _In_ ULONGLONG Head)
{
    ULONG stride = Ctx->RecChannels * 4, mask = Ctx->RingFrames - 1, idx, c;
    LONGLONG next;
    ULONGLONG h, frame;
    BOOLEAN stale = (S->Rate != Ctx->SampleRate) ? TRUE : FALSE;   /* [Rate] */

    if (S->NeedSync) {
        S->HwBase = (LONGLONG)Head - (LONGLONG)S->Frames;  /* capture starts now */
        S->NeedSync = FALSE;
        return;
    }
    next = S->HwBase + (LONGLONG)S->Frames;
    if ((LONGLONG)Head < next || (ULONGLONG)((LONGLONG)Head - next) > STREAM_MAX_LAG) {
        /* A stall longer than half the ring (or a transport restart): skip to now. */
        S->Ep->Overruns++;
        S->HwBase = (LONGLONG)Head - (LONGLONG)S->Frames;
        return;
    }
    frame = S->Frames;
    idx = (ULONG)(frame % S->BufferFrames);
    for (h = (ULONGLONG)next; h < Head; h++, frame++) {
        const LONG *src = (const LONG *)(Ctx->Rec.Va + (SIZE_T)(h & mask) * stride);
        LONG *dst = (LONG *)(S->Buffer + (SIZE_T)idx * S->BlockAlign);

        for (c = 0; c < S->Channels; c++) {
            ULONG sl = S->Slot[c];

            /* [Rate] another rate's stream records silence; [Endpoints] so does a channel with no slot */
            dst[c] = (stale || sl >= Ctx->RecChannels) ? 0 : src[sl];
        }
        if (++idx == S->BufferFrames) {
            idx = 0;
        }
    }
    S->Frames = frame;
    while (S->Frames >= (S->Completed + 1) * S->FramesPerPacket) {
        ULONGLONG p = S->Completed;

        S->PacketQpc[p % S->PacketCount] = StreamQpcAt(Ctx, S->HwBase + (LONGLONG)(p * S->FramesPerPacket));
        S->Completed++;
        S->Ep->Packets++;
    }
    /*
     * Packets overwritten before the OS took them. Only an OS that reads packets
     * (GetCapturePacket) can lose one; a client that reads by position (timer
     * mode) never calls it, so nothing is counted.
     */
    if (S->ReadStarted && S->PacketCount > 1) {
        ULONGLONG oldest = ApolloStreamOldestIntact(S);

        if (S->ReadNext < oldest) {
            S->Ep->Overruns += (ULONG)min(oldest - S->ReadNext, (ULONGLONG)MAXULONG);
            S->ReadNext = oldest;
            /* Overwritten packets are not notified either: one notification per packet handed out. */
            if (S->Notified < S->ReadNext) {
                S->Notified = S->ReadNext;
            }
        }
    }
}

VOID ApolloStreamsCapture(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONGLONG Head)
{
    PAPOLLO_ACX acx = StreamsOf(Ctx);
    PLIST_ENTRY e;

    if (acx == NULL) {
        return;
    }
    for (e = acx->Active.Flink; e != &acx->Active; e = e->Flink) {
        PAPOLLO_STREAM s = CONTAINING_RECORD(e, APOLLO_STREAM, Link);

        if (!s->Render && s->BufferFrames != 0) {
            StreamCaptureOne(Ctx, s, Head);
        }
    }
}

/*
 * Every completed packet gets its own notification, in order (one event per
 * packet for the OS). Up to Max per tick; the rest go out on the next tick.
 * Each entry holds the stream's rundown protection until it is sent.
 */
ULONG ApolloStreamsCollect(_In_ PDEVICE_CONTEXT Ctx, _Out_writes_to_(Max, return) PAPOLLO_NOTIFY List,
                           _In_ ULONG Max)
{
    PAPOLLO_ACX acx = StreamsOf(Ctx);
    PLIST_ENTRY e;
    ULONG n = 0;

    if (acx == NULL) {
        return 0;
    }
    for (e = acx->Active.Flink; e != &acx->Active && n < Max; e = e->Flink) {
        PAPOLLO_STREAM s = CONTAINING_RECORD(e, APOLLO_STREAM, Link);

        while (n < Max && s->Notified < s->Completed && ExAcquireRundownProtection(&s->Rundown)) {
            List[n].Stream = s;
            List[n].Packet = s->Notified++;
            n++;
        }
    }
    return n;
}

VOID ApolloStreamsNotify(_In_reads_(Count) PAPOLLO_NOTIFY List, _In_ ULONG Count, _In_ LONGLONG Qpc)
{
    ULONG i;

    for (i = 0; i < Count; i++) {
        PAPOLLO_STREAM s = (PAPOLLO_STREAM)List[i].Stream;
        NTSTATUS status = AcxRtStreamNotifyPacketComplete(s->Stream, List[i].Packet, (ULONGLONG)Qpc);

        if (!NT_SUCCESS(status)) {
            s->Ep->NotifyFailures++;            /* diagnostic counter, racy by design */
        }
        ExReleaseRundownProtection(&s->Rundown);
    }
}

/* ------------------------------------------------------------------ */
/* Status for IOCTL_APOLLO_GET_ENDPOINTS and TRANSPORT_STOP             */
/* ------------------------------------------------------------------ */

BOOLEAN ApolloStreamsPrepared(_In_ PDEVICE_CONTEXT Ctx)
{
    PAPOLLO_ACX acx = StreamsOf(Ctx);

    return acx != NULL && ReadNoFence(&acx->Prepared) > 0;
}

static VOID StreamCopyName(_Out_writes_(APOLLO_ENDPOINT_NAME_CHARS) CHAR *Dst, _In_z_ PCWSTR Src)
{
    ULONG i;

    for (i = 0; i + 1 < APOLLO_ENDPOINT_NAME_CHARS && Src[i] != 0; i++) {
        Dst[i] = (Src[i] < 0x80) ? (CHAR)Src[i] : '?';
    }
    Dst[i] = 0;
}

/*
 * [Endpoints] GET_ENDPOINTS v2 (apollo_ioctl.h): rows FirstRow.. into the caller's buffer, as many as
 * OutBytes holds (the caller checked it holds at least an APOLLO_ENDPOINTS: 16 rows). Returns the bytes
 * to report: the header and the rows written, never less than sizeof(APOLLO_ENDPOINTS) (v1 clients).
 */
ULONG ApolloEndpointsFillInfo(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG FirstRow,
                              _Out_writes_bytes_(OutBytes) PVOID OutBuf, _In_ SIZE_T OutBytes)
{
    PAPOLLO_ACX acx = StreamsOf(Ctx);
    PAPOLLO_ENDPOINTS Out = (PAPOLLO_ENDPOINTS)OutBuf;
    SIZE_T hdr = FIELD_OFFSET(APOLLO_ENDPOINTS, Endpoint), used;
    ULONG maxRows, i, n = 0;
    KIRQL irql;

    NT_ASSERT(OutBytes >= sizeof(APOLLO_ENDPOINTS));
    maxRows = (ULONG)min((OutBytes - hdr) / sizeof(APOLLO_ENDPOINT_INFO), (SIZE_T)APOLLO_ENDPOINTS_ROWS_MAX);
    used = hdr + (SIZE_T)maxRows * sizeof(APOLLO_ENDPOINT_INFO);
    RtlZeroMemory(OutBuf, max(used, sizeof(APOLLO_ENDPOINTS)));
    Out->Version = APOLLO_ENDPOINTS_VERSION;
    if (acx == NULL) {
        return sizeof(APOLLO_ENDPOINTS);
    }
    Out->Count = acx->Count;
    Out->EnabledMask = (FirstRow < 32) ? (acx->InForce >> FirstRow) : 0;
    Out->Prepared = (ULONG)ReadNoFence(&acx->Prepared);
    KeAcquireSpinLock(&Ctx->EngineLock, &irql);
    for (i = FirstRow; i < acx->Count && i < APOLLO_ENDPOINTS_ROWS_MAX && n < maxRows; i++, n++) {
        PAPOLLO_EP ep = &acx->Ep[i];
        PAPOLLO_ENDPOINT_INFO o = (PAPOLLO_ENDPOINT_INFO)((PUCHAR)OutBuf + hdr + (SIZE_T)n * sizeof(APOLLO_ENDPOINT_INFO));
        PAPOLLO_STREAM s = ep->Stream;

        StreamCopyName(o->Name, ep->Desc->PinName);
        StreamCopyName(o->Circuit, ep->Desc->RefString);
        o->Render = (ep->Desc->Type == AcxCircuitTypeRender) ? 1 : 0;
        o->Channels = ep->Desc->Channels;
        o->Slot[0] = ep->Desc->FallbackSlot[0];
        o->Slot[1] = ep->Desc->FallbackSlot[1];
        o->Model = ep->Desc->Model;
        if (acx->InForce & (1ul << i)) {
            o->Flags |= APOLLO_EP_FLAG_ENABLED;
        }
        if (acx->Table != NULL && (acx->Table->DefaultMask & (1ul << i))) {
            o->Flags |= APOLLO_EP_FLAG_DEFAULT;
        }
        if (acx->Combined) {
            o->Flags |= APOLLO_EP_FLAG_COMBINED;
        }
        if (ep->Added) {
            o->Flags |= APOLLO_EP_FLAG_ADDED;
        }
        /* [Clock] a row of this unit's own whose jack carries nothing at its rate and digital formats */
        if (ep->Desc->AggRow1 == 0 &&
            (!ApolloDigSlotLive(Ctx->Ident.Model, o->Render ? 0 : 1, ep->Desc->ChannelId[0], Ctx->SampleRate, Ctx->DigState) ||
             !ApolloDigSlotLive(Ctx->Ident.Model, o->Render ? 0 : 1, ep->Desc->ChannelId[1], Ctx->SampleRate, Ctx->DigState))) {
            o->Flags |= APOLLO_EP_FLAG_SILENT;
        }
        if (s != NULL) {
            o->Flags |= APOLLO_EP_FLAG_STREAM;
            if (s->Prepared) {
                o->Flags |= APOLLO_EP_FLAG_PREPARED;
                o->Slot[0] = s->Slot[0];
                o->Slot[1] = s->Slot[1];
                if (s->Slot[0] == APOLLO_EP_SLOT_NONE || (s->Channels > 1 && s->Slot[1] == APOLLO_EP_SLOT_NONE)) {
                    o->Flags |= APOLLO_EP_FLAG_NO_SLOT;
                }
            }
            if (s->Active) {
                o->Flags |= APOLLO_EP_FLAG_RUNNING;
            }
            o->PacketCount = s->PacketCount;
            o->PacketBytes = s->PacketBytes;
            o->Frames = s->Frames;
        }
        o->Streams = ep->Streams;
        o->Packets = ep->Packets;
        o->Underruns = ep->Underruns;
        o->Overruns = ep->Overruns;
        o->Dropped = ep->Dropped;
        o->LatePackets = ep->LatePackets;
        o->NotifyFailures = ep->NotifyFailures;
    }
    KeReleaseSpinLock(&Ctx->EngineLock, irql);
    used = hdr + (SIZE_T)n * sizeof(APOLLO_ENDPOINT_INFO);
    return (ULONG)max(used, sizeof(APOLLO_ENDPOINTS));
}
