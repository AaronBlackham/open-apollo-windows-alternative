/*
 * audio.c - audio rings, SG tables, audio-extension connect, transport,
 * position tracking, diagnostic capture/playback copies, hardware quiesce.
 *
 * Follows docs/spec-streaming.md sections 1-3 and spec-bringup.md 3.5 E6,
 * 3.7 G5 and 4.1. The x8p runs the legacy layout (34 play / 32 rec), the
 * one verified on hardware.
 *
 * [Twin X G3] The Twin X Gen 1 (AudioExtension path, bringup_plan.h) adds the boot transport
 * (E7-E13), the ring connect after ACEFACE with a consumption check, the descriptor counts and
 * the rate confirmation; every such branch is gated on the unit's plan, so the x8p runs what it
 * ran before.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "apollo.h"
#include <ntstrsafe.h>

/* ------------------------------------------------------------------ */
/* Audio rings and SG tables                                           */
/* ------------------------------------------------------------------ */

/*
 * [G4b] How a ring's memory is held (apollo.h APOLLO_AUDIO_RING, ring_map.h).
 *
 * Why plain AllocateCommonBuffer on the enabler's own adapter, one call per chunk, and an MDL for
 * the linear view:
 *  - The FPGA needs no contiguity (1024 SG entries of 4 KiB), and open-apollo moved to separate
 *    pages for the same 0xc000009a-after-hours reason (docs/spec-streaming.md 1.1). Device
 *    addresses must come from the DMA adapter: apollo.inf sets DmaRemappingCompatible=1, so under
 *    Kernel DMA Protection the logical address is an IOMMU address, not the physical one.
 *  - AllocateCommonBuffer is in every DMA_OPERATIONS version (wdm.h, 10.0.28000: the first entries
 *    of the table); it is the call WdfCommonBufferCreate made for the pre-G4b ring, through the
 *    same adapter, with the same CacheEnabled TRUE (KMDF FxDmaEnabler::AllocateCommonBuffer), so
 *    DMA remapping and Driver Verifier's DMA checks treat these pages exactly as they treated the
 *    old 4 MiB, and the memory is MmCached as before. The view below maps it MmCached too: one
 *    cache type for every mapping of a page.
 *  - Not AllocateCommonBufferVector: it is there in the same table (after
 *    AllocateCommonBufferWithBounds), but only an adapter from a DEVICE_DESCRIPTION_VERSION3
 *    exposes it, and on x64 KMDF asks for version 2 unless WDF_DMA_ENABLER_CONFIG.
 *    WdmDmaVersionOverride says otherwise (wdfdmaenabler.h; FxDmaEnabler::Initialize). Asking for
 *    version 3 would change the adapter of the x8p's DSP rings and strip buffers too (or need a
 *    second enabler), for a call that still hands out separate elements
 *    (GetCommonBufferFromVectorByIndex): the linear view would need this same MDL anyway.
 *  - The linear view: one MDL whose PFN array lists the pages in ring order, mapped once with
 *    MmMapLockedPagesSpecifyCache (MmCached, no execute). MmGetPhysicalAddress gives those PFNs:
 *    that is the CPU side only, our own locked nonpaged pages; the FPGA sees PageLa, never these.
 *    The pages are locked for as long as their common buffers exist, so MDL_PAGES_LOCKED is set by
 *    hand (no MmProbeAndLockPages: there is nothing to lock) and cleared before IoFreeMdl.
 * The chunks, the MDL and PageLa live in this WDF object's context, parented to the DMA enabler:
 * its cleanup runs exactly once (ApolloAudioFree at ReleaseHardware, or with the device at the
 * latest), and always before the enabler hands its adapter back (Verifier checks that every common
 * buffer is freed by then), the same guarantee KMDF's own common buffers have.
 */
typedef struct _APOLLO_RING_ALLOC {
    PDMA_ADAPTER    Adapter;        /* the enabler's; every chunk is freed through it */
    ULONG           ChunkBytes;     /* AUDIO_RING_BYTES or AUDIO_SG_PAGE */
    ULONG           Chunks;         /* chunks held, in ring order */
    PMDL            Mdl;            /* page path: the pages in ring order */
    PUCHAR          View;           /* page path: Mdl's system mapping; NULL = not mapped */
    PVOID           ChunkVa[AUDIO_SG_ENTRIES];
    ULONGLONG       ChunkLa[AUDIO_SG_ENTRIES];
    ULONGLONG       PageLa[AUDIO_SG_ENTRIES];
} APOLLO_RING_ALLOC, *PAPOLLO_RING_ALLOC;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(APOLLO_RING_ALLOC, ApolloRingAllocGet)

C_ASSERT(AUDIO_SG_PAGE == PAGE_SIZE);
C_ASSERT(AUDIO_RING_BYTES == AUDIO_SG_ENTRIES * AUDIO_SG_PAGE);

static EVT_WDF_OBJECT_CONTEXT_CLEANUP ApolloRingAllocCleanup;

/* Every chunk back to the adapter, last first. Nothing may map them any more (the view is gone). */
static VOID ApolloRingChunksFree(_Inout_ PAPOLLO_RING_ALLOC A)
{
    while (A->Chunks > 0) {
        PHYSICAL_ADDRESS la;

        A->Chunks--;
        la.QuadPart = (LONGLONG)A->ChunkLa[A->Chunks];
        A->Adapter->DmaOperations->FreeCommonBuffer(A->Adapter, A->ChunkBytes, la, A->ChunkVa[A->Chunks], TRUE);
        A->ChunkVa[A->Chunks] = NULL;
        A->ChunkLa[A->Chunks] = 0;
    }
}

/* Runs once: WdfObjectDelete (ApolloAudioFree, a failed allocation) or the enabler's teardown. */
static VOID ApolloRingAllocCleanup(_In_ WDFOBJECT Object)
{
    PAPOLLO_RING_ALLOC a = ApolloRingAllocGet(Object);

    /* PASSIVE_LEVEL: ReleaseHardware, PrepareHardware or device removal, like KMDF's own buffers */
    NT_ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    if (a->View != NULL) {
        MmUnmapLockedPages(a->View, a->Mdl);
        a->View = NULL;
    }
    if (a->Mdl != NULL) {
        /* ours, set in ApolloRingMapView (C28145 there says why) */
#pragma warning(suppress: 28145)
        a->Mdl->MdlFlags = (CSHORT)(a->Mdl->MdlFlags & ~MDL_PAGES_LOCKED);
        IoFreeMdl(a->Mdl);
        a->Mdl = NULL;
    }
    ApolloRingChunksFree(a);
}

/*
 * The ring as ApolloRingChunkCount(ChunkBytes) common buffers of ChunkBytes, each SG-page aligned
 * below 4 GiB (ring_map.h ApolloRingChunkOk). On any failure every chunk taken here is given back.
 */
static NTSTATUS ApolloRingChunksAlloc(_In_ PDEVICE_CONTEXT Ctx, _Inout_ PAPOLLO_RING_ALLOC A,
                                      _In_ ULONG ChunkBytes, _In_z_ PCSTR Name)
{
    ULONG n = ApolloRingChunkCount(ChunkBytes), c;
    NTSTATUS status = STATUS_SUCCESS;

    ApolloRingChunksFree(A);
    A->ChunkBytes = ChunkBytes;
    if (n == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    for (c = 0; c < n; c++) {
        PHYSICAL_ADDRESS la;
        PVOID va;

        la.QuadPart = 0;
        va = A->Adapter->DmaOperations->AllocateCommonBuffer(A->Adapter, ChunkBytes, &la, TRUE);
        if (va == NULL) {
            if (n > 1) {
                ApolloLogU(Ctx, "%s ring: common buffer %lu of %lu (%lu bytes) not allocated", Name, c, n,
                           ChunkBytes);
            }
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        A->ChunkVa[c] = va;
        A->ChunkLa[c] = (ULONGLONG)la.QuadPart;
        A->Chunks = c + 1;
        if (!ApolloRingChunkOk(A->ChunkLa[c], (ULONGLONG)(ULONG_PTR)va, ChunkBytes)) {
            ApolloLogU(Ctx, "%s ring: common buffer %lu at 0x%llx (%lu bytes) is not page aligned below 4 GiB",
                       Name, c, A->ChunkLa[c], ChunkBytes);
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
    }
    if (!NT_SUCCESS(status)) {
        ApolloRingChunksFree(A);
    }
    return status;
}

/* Page path: one linear kernel view of the chunks, page i of the view = SG page i (ring_map.h). */
static NTSTATUS ApolloRingMapView(_Inout_ PAPOLLO_RING_ALLOC A)
{
    PPFN_NUMBER pfn;
    ULONG i;

    if (A->Chunks != ApolloRingChunkCount(A->ChunkBytes)) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    /* StartVa only names the MDL (page aligned: ByteOffset 0); the PFN array is what maps. */
    A->Mdl = IoAllocateMdl(A->ChunkVa[0], AUDIO_RING_BYTES, FALSE, FALSE, NULL);
    if (A->Mdl == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    pfn = MmGetMdlPfnArray(A->Mdl);
    for (i = 0; i < AUDIO_SG_ENTRIES; i++) {
        PUCHAR va = (PUCHAR)A->ChunkVa[ApolloRingChunkOfPage(i, A->ChunkBytes)] +
                    (SIZE_T)ApolloRingPageInChunk(i, A->ChunkBytes) * AUDIO_SG_PAGE;

        pfn[i] = (PFN_NUMBER)((ULONGLONG)MmGetPhysicalAddress(va).QuadPart >> PAGE_SHIFT);
        if (pfn[i] == 0) {
            return STATUS_INSUFFICIENT_RESOURCES;   /* the cleanup frees the MDL (not mapped yet) */
        }
    }
    /*
     * C28145 (opaque MDL) suppressed on purpose, here and in the cleanup: no Mm routine builds a
     * locked MDL over pages that are not one virtual range (MmProbeAndLockPages and
     * MmBuildMdlForNonPagedPool both walk StartVa; MmAllocatePagesForMdlEx picks its own pages,
     * which would then need a scatter/gather mapping for logical addresses, and Verifier's DMA
     * checks double-buffer those: no good for a ring the FPGA reads all the time). The pages stay
     * locked by being common buffers until the cleanup, which unmaps the view, clears this flag
     * and frees the MDL before any of them goes back to the adapter.
     */
#pragma warning(suppress: 28145)
    A->Mdl->MdlFlags = (CSHORT)(A->Mdl->MdlFlags | MDL_PAGES_LOCKED);
    A->View = (PUCHAR)MmMapLockedPagesSpecifyCache(A->Mdl, KernelMode, MmCached, NULL, FALSE,
                                                   NormalPagePriority | MdlMappingNoExecute);
    return (A->View != NULL) ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

/*
 * [G4b] One ring (docs/spec-streaming.md 1.1): one contiguous 4 MiB common buffer if there is one
 * below 4 GiB (what every release before allocated, SG entry i = base + i * 4 KiB), else, or when
 * PagesOnly, 1024 common buffers of one SG page each plus one linear view of them. Both paths fill
 * the same Ring->Va / Ring->PageLa, so every consumer runs one code path. One log line per ring.
 * Any failure gives back everything this ring took (the object's cleanup) and leaves Ring zero.
 */
_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS ApolloAudioRingAlloc(_In_ PDEVICE_CONTEXT Ctx, _Inout_ APOLLO_AUDIO_RING *Ring,
                                     _In_z_ PCSTR Name, _In_ BOOLEAN PagesOnly)
{
    WDF_OBJECT_ATTRIBUTES attrs;
    WDFOBJECT obj = NULL;
    PAPOLLO_RING_ALLOC a;
    APOLLO_RING_SPAN span;
    NTSTATUS status;
    BOOLEAN contiguous = FALSE;

    RtlZeroMemory(Ring, sizeof(*Ring));
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, APOLLO_RING_ALLOC);
    attrs.ParentObject = Ctx->DmaEnabler;   /* cleaned up before the enabler lets its adapter go */
    attrs.EvtCleanupCallback = ApolloRingAllocCleanup;
    status = WdfObjectCreate(&attrs, &obj);
    if (!NT_SUCCESS(status)) {
        ApolloLogU(Ctx, "%s ring: no allocation object 0x%08x", Name, status);
        return status;
    }
    a = ApolloRingAllocGet(obj);            /* zeroed by the framework */
    a->Adapter = WdfDmaEnablerWdmGetDmaAdapter(Ctx->DmaEnabler, WdfDmaDirectionWriteToDevice);
    if (a->Adapter == NULL || a->Adapter->DmaOperations == NULL) {
        a->Adapter = NULL;
        WdfObjectDelete(obj);
        ApolloLogU(Ctx, "%s ring: the DMA enabler has no adapter", Name);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    if (!PagesOnly) {
        contiguous = NT_SUCCESS(ApolloRingChunksAlloc(Ctx, a, AUDIO_RING_BYTES, Name)) ? TRUE : FALSE;
    }
    if (contiguous) {
        Ring->Va = (PUCHAR)a->ChunkVa[0];
        status = STATUS_SUCCESS;
    } else {
        status = ApolloRingChunksAlloc(Ctx, a, AUDIO_SG_PAGE, Name);
        if (NT_SUCCESS(status)) {
            status = ApolloRingMapView(a);
            if (!NT_SUCCESS(status)) {
                ApolloLogU(Ctx, "%s ring: no linear view of its %lu pages 0x%08x", Name, a->Chunks, status);
            }
        }
        Ring->Va = a->View;
    }
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(obj);               /* the cleanup gives back every chunk and the MDL */
        RtlZeroMemory(Ring, sizeof(*Ring));
        return status;
    }

    ApolloRingFillPageLa(a->ChunkLa, a->ChunkBytes, a->PageLa);
    Ring->Alloc = obj;
    Ring->PageLa = a->PageLa;
    RtlZeroMemory(Ring->Va, AUDIO_RING_BYTES);

    ApolloRingSpan(a->PageLa, &span);
    if (contiguous) {
        ApolloLogU(Ctx, "%s ring: one contiguous 4 MiB common buffer, %lu pages, logical 0x%llx lowest, 0x%llx highest",
                   Name, (ULONG)AUDIO_SG_ENTRIES, span.Lowest, span.Highest);
    } else {
        ApolloLogU(Ctx, "%s ring: %lu page common buffers + one linear view (%s), %lu pages, logical 0x%llx lowest, "
                   "0x%llx highest, %lu run(s)", Name, a->Chunks,
                   PagesOnly ? "AudioRingPages=1" : "no contiguous 4 MiB below 4 GiB",
                   (ULONG)AUDIO_SG_ENTRIES, span.Lowest, span.Highest, span.Runs);
    }
    return STATUS_SUCCESS;
}

/*
 * Device lifetime: allocated once, before anything can start the transport. [G4b] A failure
 * frees both rings (all or nothing): nothing is left for the enabler's teardown to find.
 */
NTSTATUS ApolloAudioAllocate(_In_ PDEVICE_CONTEXT Ctx)
{
    BOOLEAN pagesOnly = (ApolloUnitParam(Ctx, APOLLO_PARAM_AUDIO_RING_PAGES) != 0) ? TRUE : FALSE;
    NTSTATUS status;

    if (Ctx->Play.Alloc == NULL) {
        status = ApolloAudioRingAlloc(Ctx, &Ctx->Play, "playback", pagesOnly);
        if (!NT_SUCCESS(status)) {
            ApolloLogU(Ctx, "playback ring allocation failed 0x%08x", status);
            ApolloAudioFree(Ctx);
            return status;
        }
    }
    if (Ctx->Rec.Alloc == NULL) {
        status = ApolloAudioRingAlloc(Ctx, &Ctx->Rec, "record", pagesOnly);
        if (!NT_SUCCESS(status)) {
            ApolloLogU(Ctx, "record ring allocation failed 0x%08x", status);
            ApolloAudioFree(Ctx);
            return status;
        }
    }
    if (ApolloIsAudioExt(Ctx)) {
        /* [Twin X G3] no legacy layout: the counts come from the IO descriptors at connect (G5) */
        Ctx->PlayChannels = 0;
        Ctx->RecChannels = 0;
    } else {
        Ctx->PlayChannels = X8P_PLAY_CHANNELS;
        Ctx->RecChannels = X8P_REC_CHANNELS;
    }
    Ctx->RingFrames = AUDIO_RING_FRAMES;
    /* [Rate] the rate the unit's key asks for (48 kHz unless SET_RATE stored another); rate.c checks it
       against what the unit offers at bring-up (ApolloRateBringup), ApolloEngineReadConfig derives the rest */
    Ctx->SampleRate = ApolloRateStoredHz(Ctx);
    Ctx->RateIndex = ApolloRateIndex(Ctx->SampleRate);
    return STATUS_SUCCESS;
}

/*
 * Only after the transport stopped and 100 ms passed (IOMMU, R6). [G4b] Deleting the allocation
 * object runs its cleanup now: view unmapped, every common buffer back to the adapter. Again is a
 * no-op (the ring is zero after the first call).
 */
VOID ApolloAudioFree(_In_ PDEVICE_CONTEXT Ctx)
{
    if (Ctx->Play.Alloc != NULL) {
        WdfObjectDelete(Ctx->Play.Alloc);
    }
    if (Ctx->Rec.Alloc != NULL) {
        WdfObjectDelete(Ctx->Rec.Alloc);
    }
    RtlZeroMemory(&Ctx->Play, sizeof(Ctx->Play));
    RtlZeroMemory(&Ctx->Rec, sizeof(Ctx->Rec));
}

/*
 * E6 / F7 ua_audio_program_sg: never under a running transport. [G4b] Entry i is the ring's SG
 * page i as the adapter mapped it (PageLa): base + i * 4 KiB for a contiguous ring, as before.
 */
VOID ApolloAudioProgramSg(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONG ax, i, d;

    if (Ctx->Play.PageLa == NULL || Ctx->Rec.PageLa == NULL) {
        /* PrepareHardware allocates both for every unit it brings up: not reached */
        ApolloLogU(Ctx, "SG tables NOT programmed: no audio rings");
        return;
    }
    ax = ApolloRead(Ctx, REG_AX_CONTROL);
    if (ax != 0 && ax != 0xFFFFFFFF) {
        ApolloLogU(Ctx, "transport armed (AX_CONTROL 0x%08x): stopping before SG rewrite", ax);
    }
    ApolloWrite(Ctx, REG_AX_CONTROL, AX_CTRL_STOP);
    (VOID)ApolloRead(Ctx, REG_AX_CONTROL);

    for (i = 0; i < AUDIO_SG_ENTRIES; i++) {
        ULONGLONG p = Ctx->Play.PageLa[i];
        ULONGLONG r = Ctx->Rec.PageLa[i];

        ApolloWrite(Ctx, REG_PLAY_SG + i * 8, ApolloSgLo(p));
        ApolloWrite(Ctx, REG_PLAY_SG + i * 8 + 4, ApolloSgHi(p));
        ApolloWrite(Ctx, REG_REC_SG + i * 8, ApolloSgLo(r));
        ApolloWrite(Ctx, REG_REC_SG + i * 8 + 4, ApolloSgHi(r));
    }

    d = ApolloRead(Ctx, REG_DMA_CTRL);
    ApolloWrite(Ctx, REG_DMA_CTRL, ApolloDmaCtrlClean(Ctx, d));
    Ctx->DmaCtrlCache = ApolloRead(Ctx, REG_DMA_CTRL);
    /* page 0 of each ring: the ring's base when contiguous, as every release logged it */
    ApolloLogU(Ctx, "SG tables programmed (play 0x%llx rec 0x%llx), DMA_CTRL 0x%08x",
              Ctx->Play.PageLa[0], Ctx->Rec.PageLa[0], Ctx->DmaCtrlCache);

    ApolloVectorEnable(Ctx, IMR_HI_NOTIFICATION, TRUE);
}

/* ------------------------------------------------------------------ */
/* Audio-extension connect (spec-bringup 3.7 G5)                       */
/* ------------------------------------------------------------------ */

/* [Rate] ApolloRateHz (rate index -> Hz) is rate_plan.h's now: the same table. */

/*
 * The ACEFACE handshake (spec-bringup 3.7 G5 / E8). Retries x Polls x PollMs; the disconnect
 * doorbell 0x10 before the second try. What: a log prefix ("" on the x8p: its lines are the
 * ones every release logged). [Twin X G3] The early handshake runs with 2 x 10 x 50 ms.
 */
static NTSTATUS ApolloAcefaceRun(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Retries, _In_ ULONG Polls,
                                 _In_ ULONG PollMs, _In_z_ PCSTR What)
{
    ULONG retry, poll, s, i;

    for (retry = 0; retry < Retries; retry++) {
        if (retry == 1) {
            /* clear_stale: a host connection the previous owner left behind */
            ApolloWrite(Ctx, REG_AX_CONNECT, AX_CONNECT_DISCONNECT);
            (VOID)ApolloRead(Ctx, REG_FPGA_REV);
            ApolloSleepMs(50);
        }
        ApolloWrite(Ctx, REG_NOTIF_ACEFACE, ACEFACE_MAGIC);
        ApolloWrite(Ctx, REG_AX_CONNECT, AX_CONNECT_CONNECT);

        for (poll = 0; poll < Polls; poll++) {
            if (ReadAcquire(&Ctx->StopRequested)) {
                return STATUS_CANCELLED;
            }
            s = ApolloRead(Ctx, REG_NOTIF_STATUS);
            if (s == 0xFFFFFFFF) {
                return STATUS_DEVICE_NOT_CONNECTED;
            }
            if (s & NOTIF_STATUS_CONNECT_ACK) {
                ApolloWrite(Ctx, REG_NOTIF_CONFIG + 0x00, NOTIF_CONNECT_WORD0);
                ApolloWrite(Ctx, REG_NOTIF_CONFIG + 0x04, NOTIF_CONNECT_WORD1);
                for (i = 0; i < 8; i++) {
                    ApolloWrite(Ctx, REG_NOTIF_CONFIG + 0x08 + i * 4, 0);
                }
                for (i = 0; i < NOTIF_READBACK_WORDS; i++) {
                    (VOID)ApolloRead(Ctx, REG_NOTIF_READBACK + i * 4);
                }
                ApolloWrite(Ctx, REG_NOTIF_STATUS, 0);  /* write 0, not W1C */
                Ctx->AcefaceDone = TRUE;
                ApolloLogU(Ctx, "%sACEFACE answered (retry %lu, poll %lu, status 0x%08x)", What, retry, poll, s);
                return STATUS_SUCCESS;
            }
            ApolloSleepMs(PollMs);
        }
    }
    ApolloLogU(Ctx, "%sACEFACE never answered: power-cycle the Apollo", What);
    return STATUS_IO_TIMEOUT;
}

static NTSTATUS ApolloAceface(_In_ PDEVICE_CONTEXT Ctx)
{
    return ApolloAcefaceRun(Ctx, ACEFACE_RETRIES, ACEFACE_POLLS, ACEFACE_POLL_MS, "");
}

/* ------------------------------------------------------------------ */
/* [Twin X G3] AudioExtension path: the end of every failure           */
/* ------------------------------------------------------------------ */

/*
 * Every failure of the Twin X path ends here (spec-twinx 2, "what safe means"): the engine and
 * the transport stopped, the end-of-buffer vector off, every ring waited on and resynced (the
 * date-only disconnect only if the ring connect went through), not connected, mixer flushes off.
 * The unit is then idle on our driver until it is power-cycled. Nothing here writes firmware,
 * DSP_RESET or a shutdown step. Returns a failure status for the caller to pass on; the bring-up
 * work item reports it at stage At, and GET_INFO names Step (BringupFailStep).
 */
NTSTATUS ApolloAudioExtAbort(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Step, _In_ APOLLO_STAGE At,
                             _In_ NTSTATUS Status, _In_z_ PCSTR Hint)
{
    ApolloEngineStop(Ctx);
    ApolloVectorDisable(Ctx, IMR_HI_END_OF_BUFFER);
    ApolloWrite(Ctx, REG_AX_CONTROL, AX_CTRL_STOP);
    (VOID)ApolloRead(Ctx, REG_AX_CONTROL);
    Ctx->TransportRunning = FALSE;
    (VOID)ApolloRingsQuiesce(Ctx);
    Ctx->Connected = FALSE;
    Ctx->MixerReady = FALSE;
    Ctx->BringupFailStep = Step;
    Ctx->FailAt = At;
    ApolloLogU(Ctx, "G3 bring-up FAILED at step %lu (%s), 0x%08x: transport stopped, rings resynced, unit idle. %s",
              Step, ApolloBstepName(Step), Status, Hint);
    return NT_SUCCESS(Status) ? STATUS_UNSUCCESSFUL : Status;
}

/* ------------------------------------------------------------------ */
/* [Twin X G3] Boot transport: E7, E8, DMA pulse, E10, E12, E13         */
/* ------------------------------------------------------------------ */

/*
 * docs/spec-twinx.md 2 (oa:driver/ua_audio.c:2702-2847, ua_core.c:4372-4554). The full
 * PrepareTransport sequence on the legacy fallback counts (3 play / 2 record), started with
 * AX_CONTROL 0x20F; the early ACEFACE (2 x 10 x 50 ms, disconnect doorbell before try 2); the DMA
 * reset pulse (all eight strobes for one write) and 50 ms; every ring's pages again; the ACEFACE
 * retry if the early one did not answer; the readback check; the boot transport stop, keeping
 * AcefaceDone. The SG tables (E6) are programmed before this, so the FPGA DMAs into our rings (the
 * playback ring is zero: silence). Only a gone device or a stop request ends it early; anything
 * else is logged and the audio connect goes on (a full ACEFACE there if no early one answered).
 * Returns with the transport stopped.
 */
NTSTATUS ApolloBootTransport(_In_ PDEVICE_CONTEXT Ctx)
{
    const APOLLO_BRINGUP_PLAN *p = ApolloPlanOf(Ctx);
    ULONG play, rec, frames, st, fc = 0, ctrl, d, i;
    BOOLEAN alive = FALSE;
    NTSTATUS status;

    if (p == NULL || !p->BootTransport) {
        return STATUS_SUCCESS;
    }
    play = p->BootPlay;
    rec = p->BootRec;
    frames = ApolloPlanBufFrames(play, rec);
    if (Ctx->Play.Va == NULL || Ctx->Rec.Va == NULL || play == 0 || rec == 0 ||
        (ULONGLONG)frames * max(play, rec) * 4 > AUDIO_RING_BYTES) {
        ApolloLogU(Ctx, "E7: no audio rings for the boot transport: skipped");
        return STATUS_SUCCESS;
    }
    RtlZeroMemory(Ctx->Play.Va, AUDIO_RING_BYTES);     /* it plays whatever the ring holds: silence */

    /* E7: PrepareTransport, in the kext's order */
    ApolloWrite(Ctx, REG_AX_BUF_SIZE, frames - 1);
    ApolloWrite(Ctx, REG_AX_DMA_BLKSZ, ((frames - 1) * play) >> 10);
    ApolloWrite(Ctx, REG_AX_SAMPLE_POS, 0);
    ApolloWrite(Ctx, REG_AX_CONTROL, AX_CTRL_STOP);
    ApolloWrite(Ctx, REG_AX_FRAME_CFG, p->BootFrameCfg);
    ApolloWrite(Ctx, REG_AX_PLAY_CH, play);
    ApolloWrite(Ctx, REG_AX_REC_CH, rec);
    if (p->BootP2pRoute) {
        ApolloWrite(Ctx, REG_AX_P2P_ROUTE, 0x100 | (play - 1));
    }
    ApolloWrite(Ctx, REG_AX_CONTROL, AX_CTRL_ARM);
    st = ApolloRead(Ctx, REG_AX_STATUS);
    ApolloSleepMs(2);                   /* back-to-back ARM and START: the START is ignored */
    ApolloWrite(Ctx, REG_AX_CONTROL, p->BootAxStart);
    for (i = 0; i < p->BootStartPolls; i++) {
        fc = ApolloRead(Ctx, REG_AX_FRAME_CTR);
        if (fc == 0xFFFFFFFF) {
            ApolloWrite(Ctx, REG_AX_CONTROL, AX_CTRL_STOP);     /* no-op if the link is really gone */
            return STATUS_DEVICE_NOT_CONNECTED;
        }
        if (fc != 0) {
            break;
        }
        ApolloSleepMs(1);
    }
    ctrl = ApolloRead(Ctx, REG_AX_CONTROL);
    if (fc != 0) {
        ApolloLogU(Ctx, "E7: boot transport running: play %lu rec %lu, %lu frames, FRAME_CFG 0x%02lx, "
                  "AX_CONTROL 0x%03lx, FRAME_CTR 0x%lx after %lu poll(s), AX_STATUS 0x%08lx",
                  play, rec, frames, p->BootFrameCfg, ctrl, fc, i + 1, st);
        ApolloStepPassed(Ctx, APOLLO_BSTEP_E7_BOOT_START);
    } else {
        ApolloLogU(Ctx, "E7: FRAME_CTR still 0 after %lu polls (AX_CONTROL 0x%03lx, AX_STATUS 0x%08lx): the "
                  "boot transport start was ignored; stopped, going on with the early ACEFACE",
                  p->BootStartPolls, ctrl, st);
        ApolloWrite(Ctx, REG_AX_CONTROL, AX_CTRL_STOP);
    }

    /* E8: the early ACEFACE, transport running */
    status = ApolloAcefaceRun(Ctx, p->EarlyAcefaceTries, p->EarlyAcefacePolls, p->EarlyAcefacePollMs,
                              "E8 early ");
    if (status == STATUS_DEVICE_NOT_CONNECTED || status == STATUS_CANCELLED) {
        ApolloWrite(Ctx, REG_AX_CONTROL, AX_CTRL_STOP);
        return status;
    }
    if (NT_SUCCESS(status)) {
        ApolloStepPassed(Ctx, APOLLO_BSTEP_E8_ACEFACE);
    } else {
        ApolloLogU(Ctx, "E8: no early ACEFACE answer in %lu x %lu x %lu ms (E10 retries once)",
                  p->EarlyAcefaceTries, p->EarlyAcefacePolls, p->EarlyAcefacePollMs);
    }

    /* The DMA reset pulse "to activate ring buffers": all eight strobes for one write, then back */
    d = ApolloRead(Ctx, REG_DMA_CTRL);
    if (d == 0xFFFFFFFF) {
        ApolloWrite(Ctx, REG_AX_CONTROL, AX_CTRL_STOP);
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    ApolloWrite(Ctx, REG_DMA_CTRL, d | DMA_CTRL_STROBES);
    ApolloWrite(Ctx, REG_DMA_CTRL, d);
    Ctx->DmaCtrlCache = ApolloRead(Ctx, REG_DMA_CTRL);
    ApolloSleepMs(p->PulseSettleMs);    /* "without this delay, FPGA ignores ring entries" */
    status = ApolloRingsReprogram(Ctx);
    if (status == STATUS_DEVICE_NOT_CONNECTED) {
        ApolloWrite(Ctx, REG_AX_CONTROL, AX_CTRL_STOP);
        return status;
    }
    ApolloLogU(Ctx, "E7: DMA reset pulse (DMA_CTRL 0x%08lx, back to 0x%08lx), %lu ms, ring pages re-programmed 0x%08x",
              d | DMA_CTRL_STROBES, Ctx->DmaCtrlCache, p->PulseSettleMs, status);
    if (NT_SUCCESS(status) && (Ctx->DmaCtrlCache & DMA_CTRL_STROBES) == 0) {
        ApolloStepPassed(Ctx, APOLLO_BSTEP_E7_PULSE);
    }

    /* E10: once more if the early ACEFACE did not answer */
    if (!Ctx->AcefaceDone) {
        status = ApolloAcefaceRun(Ctx, p->EarlyAcefaceTries, p->EarlyAcefacePolls, p->EarlyAcefacePollMs,
                                  "E10 retry: ");
        if (status == STATUS_DEVICE_NOT_CONNECTED || status == STATUS_CANCELLED) {
            ApolloWrite(Ctx, REG_AX_CONTROL, AX_CTRL_STOP);
            return status;
        }
        if (NT_SUCCESS(status)) {
            ApolloStepPassed(Ctx, APOLLO_BSTEP_E10_ACEFACE);
        }
    }

    /* E12: readback liveness, log only */
    ApolloWrite(Ctx, REG_MIXER_RB_STATUS, 0);
    for (i = 0; i < 50 && !alive; i++) {
        ApolloSleepMs(11);
        alive = (ApolloRead(Ctx, REG_MIXER_RB_STATUS) == 1);
    }
    ApolloLogU(Ctx, "E12: mixer readback %s (%lu polls)", alive ? "alive" : "silent", i);
    if (alive) {
        ApolloStepPassed(Ctx, APOLLO_BSTEP_E12_READBACK);
    }

    /* E13: boot transport stop; AcefaceDone stays, so the connect only does the post-handshake work */
    ApolloWrite(Ctx, REG_AX_CONTROL, AX_CTRL_STOP);
    ctrl = ApolloRead(Ctx, REG_AX_CONTROL);
    Ctx->Connected = FALSE;
    ApolloLogU(Ctx, "E13: boot transport stopped (AX_CONTROL 0x%03lx); ACEFACE %s", ctrl,
              Ctx->AcefaceDone ? "done (kept for the connect)" : "NOT answered (the connect runs the full one)");
    if (ctrl != 0xFFFFFFFF) {
        ApolloStepPassed(Ctx, APOLLO_BSTEP_E13_BOOT_STOP);
    }
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* [Twin X G3] The firmware IO descriptors decide the DMA frame        */
/* ------------------------------------------------------------------ */

/* One area (72 words) the torn-read-safe way (ua_io_desc_fetch): words, then the header again. */
static ULONG ApolloIoDescFetch(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Base, _In_ BOOLEAN Record,
                               _Out_writes_(IO_DESC_WORDS) PULONG W)
{
    ULONG i, count, cw = Record ? 3 : 4;

    for (i = 0; i < IO_DESC_WORDS; i++) {
        W[i] = ApolloRead(Ctx, Base + i * 4);
    }
    count = ApolloIoDescCount(W, Record);
    if (count == 0) {
        return 0;
    }
    if (ApolloRead(Ctx, Base) != W[0] || ApolloRead(Ctx, Base + 4) != W[1] ||
        ApolloRead(Ctx, Base + cw * 4) != W[cw]) {
        return 0;                       /* republished (or the link dropped) while being read */
    }
    return count;
}

/* Slot, ID and name, four slots a line. */
static VOID ApolloIoDescLog(_In_ PDEVICE_CONTEXT Ctx, _In_reads_(IO_DESC_WORDS) const ULONG *W,
                            _In_ BOOLEAN Record, _In_ ULONG Count)
{
    CHAR line[200], name[24];
    size_t len;
    ULONG i, k;

    for (i = 0; i < Count; i += 4) {
        line[0] = '\0';
        for (k = i; k < Count && k < i + 4; k++) {
            USHORT id = ApolloIoDescId(W, k);

            (VOID)ApolloChanName(id, name, sizeof(name));
            len = 0;
            (VOID)RtlStringCbLengthA(line, sizeof(line), &len);
            (VOID)RtlStringCbPrintfA(line + len, sizeof(line) - len, "%s%2lu %04x %s", k == i ? "" : " | ",
                                     k, id, name);
        }
        ApolloLogU(Ctx, "G5: %s %s", Record ? "rec " : "play", line);
    }
}

/*
 * G5 bits 0 + 1 (spec-twinx 3.1): both descriptors, usable and stable (up to 5 reads, 20 ms
 * apart); their counts become AX_PLAY_CH / AX_REC_CH and the ring stride. No usable descriptor,
 * no transport: the layout would be a guess.
 */
static NTSTATUS ApolloAudioExtDescriptors(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONG tries, in = 0, out = 0;
    LONG monL, monR, recL, recR;

    for (tries = 0; tries < 5; tries++) {
        if (tries != 0) {
            ApolloSleepMs(20);
        }
        in = ApolloIoDescFetch(Ctx, REG_IO_DESC_IN, TRUE, Ctx->IoDescIn);
        out = ApolloIoDescFetch(Ctx, REG_IO_DESC_OUT, FALSE, Ctx->IoDescOut);
        if (in != 0 && out != 0) {
            break;
        }
        if (ApolloRead(Ctx, REG_FPGA_REV) == 0xFFFFFFFF) {
            return STATUS_DEVICE_NOT_CONNECTED;
        }
    }
    if (in == 0 || out == 0) {
        ApolloLogU(Ctx, "G5: IO descriptors not usable after %lu reads: in %08x %08x %08x %08x %08x %08x, "
                  "out %08x %08x %08x %08x %08x %08x", tries,
                  Ctx->IoDescIn[0], Ctx->IoDescIn[1], Ctx->IoDescIn[2], Ctx->IoDescIn[3], Ctx->IoDescIn[4],
                  Ctx->IoDescIn[5], Ctx->IoDescOut[0], Ctx->IoDescOut[1], Ctx->IoDescOut[2],
                  Ctx->IoDescOut[3], Ctx->IoDescOut[4], Ctx->IoDescOut[5]);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (ApolloPlanBufFrames(out, in) != Ctx->RingFrames) {
        ApolloLogU(Ctx, "G5: IO descriptor counts play %lu rec %lu need a %lu-frame ring, ours is %lu",
                  out, in, ApolloPlanBufFrames(out, in), Ctx->RingFrames);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    Ctx->PlayChannels = out;
    Ctx->RecChannels = in;
    ApolloIoDescLog(Ctx, Ctx->IoDescIn, TRUE, in);
    ApolloIoDescLog(Ctx, Ctx->IoDescOut, FALSE, out);
    monL = ApolloIoDescFind(Ctx->IoDescOut, FALSE, APOLLO_CHAN_MON_L);
    monR = ApolloIoDescFind(Ctx->IoDescOut, FALSE, APOLLO_CHAN_MON_R);
    recL = ApolloIoDescFind(Ctx->IoDescIn, TRUE, APOLLO_CHAN_MON_L);
    recR = ApolloIoDescFind(Ctx->IoDescIn, TRUE, APOLLO_CHAN_MON_R);
    ApolloLogU(Ctx, "G5: IO descriptors: record %lu slots (word5 0x%x), playback %lu slots: transport counts "
              "play %lu rec %lu", in, Ctx->IoDescIn[5], out, out, in);
    ApolloLogU(Ctx, "G5: MON L/R: playback slots %ld/%ld, record (monitor loopback) slots %ld/%ld "
              "(apollo-diag --unit twinx tone 5 --slots %ld,%ld)", monL, monR, recL, recR, monL, monR);
    return STATUS_SUCCESS;
}

NTSTATUS ApolloAudioConnect(_In_ PDEVICE_CONTEXT Ctx)
{
    const APOLLO_BRINGUP_PLAN *plan = ApolloPlanOf(Ctx);
    BOOLEAN ext = (plan != NULL && plan->AudioExtension) ? TRUE : FALSE;   /* [Twin X G3] */
    NTSTATUS status;
    ULONG s, i, idx, rate;

    if (Ctx->Connected) {
        return STATUS_SUCCESS;
    }
    if (!Ctx->AcefaceDone && ext) {
        /* [Twin X G3] warm start (or no early answer): the full handshake, transport stopped */
        status = ApolloAcefaceRun(Ctx, plan->AcefaceTries, plan->AcefacePolls, plan->AcefacePollMs,
                                  "G5 (transport stopped): ");
        if (status == STATUS_DEVICE_NOT_CONNECTED || status == STATUS_CANCELLED) {
            return status;
        }
        if (!NT_SUCCESS(status)) {
            return ApolloAudioExtAbort(Ctx, APOLLO_BSTEP_G5_ACEFACE, ApolloStageAcefaceDone, status,
                                       Ctx->WarmStart ?
                                       "Warm start: power-cycle the Twin X, or set WarmBootTransport=1 in its "
                                       "unit key (docs/spec-twinx.md 2 notes) and restart it." :
                                       "Power-cycle the Twin X.");
        }
    } else if (!Ctx->AcefaceDone) {
        status = ApolloAceface(Ctx);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    } else if (ext) {
        ApolloLogU(Ctx, "G5: ACEFACE already answered by the early handshake: post-handshake work only");
    }
    if (ext) {
        ApolloStepPassed(Ctx, APOLLO_BSTEP_G5_ACEFACE);
    }
    Ctx->Stage = ApolloStageAcefaceDone;

    /* Post-handshake reads the kext does; the DSP may not route audio without them. */
    s = ApolloRead(Ctx, REG_NOTIF_STATUS);
    if (s != 0 && s != 0xFFFFFFFF) {
        ApolloWrite(Ctx, REG_NOTIF_STATUS, 0);
    }
    for (i = 0; i < IO_DESC_WORDS; i++) {
        Ctx->IoDescIn[i] = ApolloRead(Ctx, REG_IO_DESC_IN + i * 4);
        Ctx->IoDescOut[i] = ApolloRead(Ctx, REG_IO_DESC_OUT + i * 4);
    }
    ApolloLogU(Ctx, "IO descriptors: in v%lu count %lu, out v%lu count %lu",
              Ctx->IoDescIn[0], Ctx->IoDescIn[3], Ctx->IoDescOut[0], Ctx->IoDescOut[4]);
    if (ext) {
        /* [Twin X G3] the descriptor IS the layout (spec-twinx 3.1): read it properly, use its counts */
        status = ApolloAudioExtDescriptors(Ctx);
        if (status == STATUS_DEVICE_NOT_CONNECTED) {
            return status;
        }
        if (!NT_SUCCESS(status)) {
            return ApolloAudioExtAbort(Ctx, APOLLO_BSTEP_G5_DESCRIPTORS, ApolloStageConnected, status,
                                       "No usable IO descriptor, so no transport. Power-cycle the Twin X; "
                                       "if it repeats, send Claude the log.");
        }
        ApolloStepPassed(Ctx, APOLLO_BSTEP_G5_DESCRIPTORS);
    }
    Ctx->NotifRateInfo = ApolloRead(Ctx, REG_NOTIF_RATE_INFO);
    (VOID)ApolloRead(Ctx, REG_NOTIF_CLOCK_INFO);
    Ctx->NotifClockSrcs = ApolloRead(Ctx, REG_NOTIF_CLOCK_SRCS);
    for (i = 0; i < 64; i++) {
        (VOID)ApolloRead(Ctx, REG_NOTIF_CLOCK_NAMES + i * 4);
    }
    (VOID)ApolloRead(Ctx, REG_FW_LATENCY_OUT);
    (VOID)ApolloRead(Ctx, REG_FW_LATENCY_IN);
    (VOID)ApolloRead(Ctx, REG_NOTIF_XPORT_INFO);
    Ctx->Connected = TRUE;

    /* x8p: the ring connect ran at probe (cold only). No deferred connect here. */
    if (ext && plan->RingConnectAtConnect) {
        /*
         * [Twin X G3] AudioExtension: the ring connect follows ACEFACE on every connect, cold and
         * warm (ua_kext_ring_connect; skipping it "left playback silent"). Each command must be
         * consumed, else the unit stops here and nothing more goes to the DSPs.
         */
        status = ApolloDspConnectChecked(Ctx, plan->ConnectConsumeMs);
        if (status == STATUS_DEVICE_NOT_CONNECTED || status == STATUS_CANCELLED) {
            Ctx->Connected = FALSE;
            return status;
        }
        if (!NT_SUCCESS(status)) {
            return ApolloAudioExtAbort(Ctx, APOLLO_BSTEP_G5_RING_CONNECT, ApolloStageRingConnected, status,
                                       "The DSPs did not take the ring connect: power-cycle the Twin X.");
        }
        ApolloStepPassed(Ctx, APOLLO_BSTEP_G5_RING_CONNECT);
    }

    /* Rate ack: the firmware's current rate if it reports one. */
    idx = (Ctx->NotifRateInfo >> 8) & 0xFF;
    rate = ApolloRateHz(idx);
    if (rate == 0) {
        rate = Ctx->SampleRate;
    }
    status = ApolloDspRateAck(Ctx, rate);
    ApolloLogU(Ctx, "rate info 0x%08x (index %lu): rate ack %lu Hz 0x%08x, clock sources 0x%08x",
              Ctx->NotifRateInfo, idx, rate, status, Ctx->NotifClockSrcs);
    if (ext && NT_SUCCESS(status)) {
        ApolloStepPassed(Ctx, APOLLO_BSTEP_G5_RATE_ACK);
    }

    ApolloMixerInit(Ctx);
    if (ext && Ctx->Strips.MixerBatch != APOLLO_MIXER_BATCH_LEGACY) {
        ApolloStepPassed(Ctx, APOLLO_BSTEP_G5_MIXER_INIT);     /* model-safe (1) or none (0) */
    }
    Ctx->Stage = ApolloStageConnected;
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Transport (spec-streaming 2.3-2.6)                                  */
/* ------------------------------------------------------------------ */

static VOID ApolloCheckAxFaults(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Ctrl, _In_ PCSTR When)
{
    if (Ctrl == 0xFFFFFFFF) {
        return;
    }
    if (Ctrl & AX_CTRL_FAULT_BIT7) {
        ApolloLogU(Ctx, "AX_CONTROL fault bit 7 (%s): 0x%03x AX_STATUS 0x%08x", When, Ctrl,
                  ApolloRead(Ctx, REG_AX_STATUS));
    }
    if (Ctrl & AX_CTRL_FAULT_BIT8) {
        ApolloLogU(Ctx, "AX_CONTROL fault bit 8 (%s): 0x%03x", When, Ctrl);
    }
}

/*
 * [G4] After the rate change: are the IO descriptors the ones the transport was started on? The
 * firmware republishes them (notification bits 0/1) when the rate band changes; 44.1 -> 48 kHz
 * stays in one band (G3: the same 34 / 46 at 0x101 and 0x201). A change is logged, the transport
 * keeps the connect-time layout (its counts are what AX_PLAY_CH / AX_REC_CH say) and step
 * RATE_48K is not passed: the gate shows CHECK and the log says what moved. Compared: the slot
 * count and every slot's channel ID (the header's flag word and the padding may change freely).
 */
static BOOLEAN ApolloAudioExtDescSame(_In_ PDEVICE_CONTEXT Ctx, _In_ BOOLEAN Record)
{
    ULONG w[IO_DESC_WORDS];
    const ULONG *old = Record ? Ctx->IoDescIn : Ctx->IoDescOut;
    ULONG had = Record ? Ctx->RecChannels : Ctx->PlayChannels, now, i;

    now = ApolloIoDescFetch(Ctx, Record ? REG_IO_DESC_IN : REG_IO_DESC_OUT, Record, w);
    for (i = 0; now == had && i < now; i++) {
        if (ApolloIoDescId(w, i) != ApolloIoDescId(old, i)) {
            break;
        }
    }
    if (now != had || i < now) {
        ApolloLogU(Ctx, "rate: %s IO descriptor CHANGED after the rate change: %lu slots (MON L at %ld), the transport "
                  "runs on %lu (MON L at %ld)", Record ? "record" : "playback", now,
                  ApolloIoDescFind(w, Record, APOLLO_CHAN_MON_L), had, ApolloIoDescFind(old, Record, APOLLO_CHAN_MON_L));
        return FALSE;
    }
    return TRUE;
}

static BOOLEAN ApolloAudioExtLayoutSame(_In_ PDEVICE_CONTEXT Ctx)
{
    BOOLEAN rec = ApolloAudioExtDescSame(Ctx, TRUE);
    BOOLEAN play = ApolloAudioExtDescSame(Ctx, FALSE);

    if (rec && play) {
        ApolloLogU(Ctx, "rate: IO descriptors republished, unchanged (record %lu, playback %lu slots)",
                  Ctx->RecChannels, Ctx->PlayChannels);
    }
    return (rec && play) ? TRUE : FALSE;
}

/*
 * [Rate] After a rate change, transport stopped (DevLock): the firmware republishes its IO descriptors
 * when the rate band changes (oa:driver/ua_audio.c:1770-1795, ua_io_map_rate_changed). The Twin X runs
 * on its descriptor (spec-twinx 3.1): a usable new one becomes the layout (counts, slot IDs), so the
 * transport starts on it and the endpoints find MON L/R again by ID. The x8p keeps its verified legacy
 * layout (34 / 32 at every rate, as open-apollo's io_map=-1 does for it); its descriptor is logged.
 * Returns TRUE when the layout the transport runs on changed.
 */
BOOLEAN ApolloAudioDescriptorsReread(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONG win[IO_DESC_WORDS], wout[IO_DESC_WORDS], in, out, i;
    BOOLEAN same;

    in = ApolloIoDescFetch(Ctx, REG_IO_DESC_IN, TRUE, win);
    out = ApolloIoDescFetch(Ctx, REG_IO_DESC_OUT, FALSE, wout);
    if (!ApolloIsAudioExt(Ctx)) {
        ApolloLogU(Ctx, "rate: IO descriptors at %lu Hz: record %lu, playback %lu slots (the legacy layout %lu / %lu "
                  "stays)", Ctx->SampleRate, in, out, Ctx->RecChannels, Ctx->PlayChannels);
        return FALSE;
    }
    if (in == 0 || out == 0 || in > AUDIO_MAX_CHANNELS || out > AUDIO_MAX_CHANNELS ||
        ApolloPlanBufFrames(out, in) != Ctx->RingFrames) {
        ApolloLogU(Ctx, "rate: IO descriptors at %lu Hz not usable (record %lu, playback %lu): the layout stays "
                  "%lu / %lu", Ctx->SampleRate, in, out, Ctx->RecChannels, Ctx->PlayChannels);
        return FALSE;
    }
    same = (in == Ctx->RecChannels && out == Ctx->PlayChannels) ? TRUE : FALSE;
    for (i = 0; same && i < in; i++) {
        same = ApolloIoDescId(win, i) == ApolloIoDescId(Ctx->IoDescIn, i) ? TRUE : FALSE;
    }
    for (i = 0; same && i < out; i++) {
        same = ApolloIoDescId(wout, i) == ApolloIoDescId(Ctx->IoDescOut, i) ? TRUE : FALSE;
    }
    if (same) {
        ApolloLogU(Ctx, "rate: IO descriptors at %lu Hz: unchanged (record %lu, playback %lu slots)", Ctx->SampleRate,
                  in, out);
        return FALSE;
    }
    RtlCopyMemory(Ctx->IoDescIn, win, sizeof(win));
    RtlCopyMemory(Ctx->IoDescOut, wout, sizeof(wout));
    ApolloLogU(Ctx, "rate: IO descriptors at %lu Hz CHANGED: record %lu -> %lu, playback %lu -> %lu slots; the "
              "transport starts on the new layout", Ctx->SampleRate, Ctx->RecChannels, in, Ctx->PlayChannels, out);
    Ctx->RecChannels = in;
    Ctx->PlayChannels = out;
    ApolloIoDescLog(Ctx, Ctx->IoDescIn, TRUE, in);
    ApolloIoDescLog(Ctx, Ctx->IoDescOut, FALSE, out);
    return TRUE;
}

/*
 * [Twin X G3] After the transport start and the clock write: wait for 0xC07C to say the asked
 * rate, locked, and log it; check that SAMPLE_POS moves (a start the FPGA ignored fails here);
 * re-seed the 64-bit position, which nothing has read yet; log the clock-source mask on a LIVE
 * read (spec-twinx 6.3.7: is Peer 0x0A offered?). A rate that does not lock is logged for the
 * gate (step RATE_48K missing), not fatal; a frozen SAMPLE_POS is.
 * [G4] The wait (plan RateConfirmMs, 2 s) acknowledges the notification status while it polls,
 * as the kext's handler and open-apollo's service step do: the firmware posts nothing new while
 * earlier bits stay set (oa:driver/ua_audio.c:2150-2154), it publishes the new rate only after
 * that write (oa:driver/ua_audio.c:5357-5361), and this runs under DevLock before the service
 * thread exists, so nobody else clears them (G3 cold start: still 0x101 after 529 ms, 0x201 once
 * the service thread ran). Then the DSPs get the 48 kHz rate ack, after the firmware reports the
 * rate: the kext's order for a clock change (oa:driver/ua_audio.c:5387-5453, ua_audio_rate_change_kext).
 */
static NTSTATUS ApolloAudioExtConfirmRate(_In_ PDEVICE_CONTEXT Ctx, _In_ const APOLLO_BRINGUP_PLAN *Plan)
{
    ULONGLONG t0 = KeQueryInterruptTime();
    ULONG v = 0, i, polls, ms, p0, p1, delta, ax, fc, src, s, acked = 0, told;
    BOOLEAN locked = FALSE, sameLayout = TRUE;
    NTSTATUS rs;

    polls = (Plan->RateConfirmMs + 9) / 10;
    if (polls == 0) {
        polls = 1;
    }
    for (i = 0; i < polls; i++) {
        /* the service step first (ack = write 0, last), then the rate word, as ua_audio_rate_change_kext */
        s = ApolloRead(Ctx, REG_NOTIF_STATUS);
        if (s == 0xFFFFFFFF) {
            return STATUS_DEVICE_NOT_CONNECTED;
        }
        if (s != 0) {
            Ctx->NotifStatusSeen |= s;
            acked |= s;
            ApolloWrite(Ctx, REG_NOTIF_STATUS, 0);
        }
        v = ApolloRead(Ctx, REG_NOTIF_RATE_INFO);
        if (v == 0xFFFFFFFF) {
            return STATUS_DEVICE_NOT_CONNECTED;
        }
        if (ApolloPlanRateLocked(v, Ctx->RateIndex)) {
            locked = TRUE;
            break;
        }
        if (ReadAcquire(&Ctx->StopRequested)) {
            return STATUS_CANCELLED;
        }
        ApolloSleepMs(10);
    }
    ms = (ULONG)((KeQueryInterruptTime() - t0) / 10000ULL);
    Ctx->NotifRateInfo = v;
    if (locked) {
        ApolloLogU(Ctx, "rate: 0xC07C 0x%08lx after %lu ms: %lu Hz locked (clock write 0x%03lx + doorbell 4; "
                  "notifications acknowledged meanwhile: 0x%08lx)", v, ms, Ctx->SampleRate,
                  ApolloPlanClockWord(Ctx->RateIndex, CLOCK_SOURCE_INTERNAL), acked);
        if ((acked & 0x3u) != 0) {
            sameLayout = ApolloAudioExtLayoutSame(Ctx);     /* bits 0/1: descriptors republished */
        }
    } else {
        ApolloLogU(Ctx, "rate: 0xC07C 0x%08lx after %lu ms: NOT %lu Hz locked (index %lu, lock %lu; wanted index %lu; "
                  "notifications acknowledged 0x%08lx): the transport runs on, check the rate", v, ms,
                  Ctx->SampleRate, (v >> 8) & 0xFF, v & 3, Ctx->RateIndex, acked);
    }

    /* The DSPs had the firmware's rate from the connect (0x101 = 44.1 kHz after a power-up): now ours. */
    told = Ctx->DspToldRate;
    rs = STATUS_SUCCESS;
    if (told != Ctx->SampleRate) {
        rs = ApolloDspRateAck(Ctx, Ctx->SampleRate);
        ApolloLogU(Ctx, "rate ack %lu Hz to the DSPs after the clock change (they had %lu Hz from the connect): "
                  "0x%08x", Ctx->SampleRate, told, rs);
    }
    if (locked && sameLayout && NT_SUCCESS(rs) && Ctx->DspToldRate == Ctx->SampleRate) {
        ApolloStepPassed(Ctx, APOLLO_BSTEP_RATE_48K);
    }

    t0 = KeQueryInterruptTime();
    p0 = ApolloRead(Ctx, REG_AX_SAMPLE_POS);
    ApolloSleepMs(20);
    p1 = ApolloRead(Ctx, REG_AX_SAMPLE_POS);
    ms = (ULONG)((KeQueryInterruptTime() - t0) / 10000ULL);
    if (p0 == 0xFFFFFFFF || p1 == 0xFFFFFFFF) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    delta = (p1 - p0) & (Ctx->RingFrames - 1);
    ax = ApolloRead(Ctx, REG_AX_CONTROL);
    fc = ApolloRead(Ctx, REG_AX_FRAME_CTR);
    if (delta == 0) {
        ApolloLogU(Ctx, "transport: SAMPLE_POS did not move in %lu ms (%lu; AX_CONTROL 0x%03lx, FRAME_CTR %lu): "
                  "the start was ignored", ms, p1, ax, fc);
        return STATUS_IO_TIMEOUT;
    }
    ApolloLogU(Ctx, "transport: SAMPLE_POS moved %lu frames in %lu ms (about %lu Hz), AX_CONTROL 0x%03lx, FRAME_CTR %lu",
              delta, ms, ms ? delta * 1000 / ms : 0, ax, fc);
    ApolloStepPassed(Ctx, APOLLO_BSTEP_SAMPLE_POS);

    /* Up to half a second passed since the start seeded the position: seed it again (no reader yet). */
    Ctx->LastRawPos = ApolloRead(Ctx, REG_AX_SAMPLE_POS) & (Ctx->RingFrames - 1);
    Ctx->ExtPos = Ctx->LastRawPos;
    Ctx->LastPosQpc = KeQueryPerformanceCounter(NULL).QuadPart;

    src = ApolloRead(Ctx, REG_NOTIF_CLOCK_SRCS);
    Ctx->NotifClockSrcsLive = src;
    ApolloLogU(Ctx, "clock sources (LIVE, our transport running): 0x%08lx: sources 0x%04lx (S/PDIF %s, ADAT %s, "
              "Word Clock %s, Peer 0x0A %s, Internal 0x0C %s), rates 0x%02lx", src, src >> 16,
              ApolloClockOffers(src, APOLLO_CLKSRC_SPDIF) ? "yes" : "no",
              ApolloClockOffers(src, APOLLO_CLKSRC_ADAT) ? "yes" : "no",
              ApolloClockOffers(src, APOLLO_CLKSRC_WORDCLOCK) ? "yes" : "no",
              ApolloClockOffers(src, APOLLO_CLKSRC_PEER) ? "OFFERED" : "no",
              ApolloClockOffers(src, APOLLO_CLKSRC_INTERNAL) ? "yes" : "no", src & 0xFF);
    return STATUS_SUCCESS;
}

NTSTATUS ApolloTransportStart(_In_ PDEVICE_CONTEXT Ctx)
{
    return ApolloTransportStartEx(Ctx, TRUE);
}

/*
 * [Rate] Engine FALSE: the transport alone (rate.c's 48 kHz session before a Hi-band start, which
 * streams nothing). AX_FRAME_CFG is the rate's (Ctx->FrameCfg: 8 at 44.1/48 kHz as before, 16, 32).
 */
NTSTATUS ApolloTransportStartEx(_In_ PDEVICE_CONTEXT Ctx, _In_ BOOLEAN Engine)
{
    const APOLLO_BRINGUP_PLAN *plan = ApolloPlanOf(Ctx);
    BOOLEAN ext = (plan != NULL && plan->AudioExtension) ? TRUE : FALSE;   /* [Twin X G3] */
    ULONG d, clean, frames, frameCfg, fc, ctrl, i;

    if (!Ctx->Connected) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (Ctx->TransportRunning) {
        return STATUS_SUCCESS;
    }
    frames = Ctx->RingFrames;
    frameCfg = Ctx->FrameCfg;                   /* [Rate] X8P_SAFETY_FRAMES_48K at 44.1/48 kHz, as before */
    if (ext) {
        /* [Twin X G3] the IO descriptor counts (G5) or nothing: no guessed layout */
        if (Ctx->PlayChannels == 0 || Ctx->RecChannels == 0 || Ctx->PlayChannels > AUDIO_MAX_CHANNELS ||
            Ctx->RecChannels > AUDIO_MAX_CHANNELS ||
            ApolloPlanBufFrames(Ctx->PlayChannels, Ctx->RecChannels) != frames) {
            ApolloLogU(Ctx, "transport start: no usable IO descriptor counts (play %lu, record %lu): not started",
                      Ctx->PlayChannels, Ctx->RecChannels);
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        frameCfg = Ctx->FrameCfg;               /* [Rate] plan->FrameCfg48k at 44.1/48 kHz, as before */
    }

    /* A fresh start plays silence until a client writes. */
    RtlZeroMemory(Ctx->Play.Va, (SIZE_T)frames * Ctx->PlayChannels * 4);

    /* 0b: issue #46, strobes re-asserted after SG programming freeze SAMPLE_POS. */
    d = ApolloRead(Ctx, REG_DMA_CTRL);
    clean = ApolloDmaCtrlClean(Ctx, d);
    if (d != clean) {
        ApolloLogU(Ctx, "DMA_CTRL 0x%08x at prepare, writing clean 0x%08x", d, clean);
        ApolloWrite(Ctx, REG_DMA_CTRL, clean);
        (VOID)ApolloRead(Ctx, REG_DMA_CTRL);
    }

    ApolloWrite(Ctx, REG_AX_BUF_SIZE, frames - 1);                                  /* 1 */
    ApolloWrite(Ctx, REG_AX_DMA_BLKSZ, ((frames - 1) * Ctx->PlayChannels) >> 10);    /* 2 */
    ApolloWrite(Ctx, REG_AX_SAMPLE_POS, 0);                                         /* 3 */
    ApolloVectorDisable(Ctx, IMR_HI_PERIOD_TIMER);                                  /* 3b */
    ApolloWrite(Ctx, REG_AX_SAMPLE_POS, 0);                                         /* 5 */
    (VOID)ApolloRead(Ctx, REG_AX_SAMPLE_POS);                                       /* 6 */
    ApolloWrite(Ctx, REG_AX_CONTROL, AX_CTRL_STOP);                                 /* 7 */
    ApolloWrite(Ctx, REG_AX_FRAME_CFG, frameCfg);                                   /* 8 */
    ApolloWrite(Ctx, REG_AX_PLAY_CH, Ctx->PlayChannels);                            /* 9 */
    ApolloWrite(Ctx, REG_AX_REC_CH, Ctx->RecChannels);                              /* 10 */
    ApolloWrite(Ctx, REG_AX_CONTROL, AX_CTRL_ARM);                                  /* 11 */
    (VOID)ApolloRead(Ctx, REG_AX_STATUS);                                           /* 12 */
    if (plan == NULL || plan->P2pRouteAtStart) {
        /* The legacy layout's step; [Twin X G3] the descriptor layout skips it, as the kext does. */
        ApolloWrite(Ctx, REG_AX_P2P_ROUTE, 0x100 | (Ctx->PlayChannels - 1));        /* 13 */
    }
    ApolloCheckAxFaults(Ctx, ApolloRead(Ctx, REG_AX_CONTROL), "prepare");           /* 13b */

    fc = ApolloRead(Ctx, REG_AX_FRAME_CTR);                                         /* 14 */
    for (i = 0; fc != frameCfg && i < 3; i++) {
        ApolloSleepMs(1);
        fc = ApolloRead(Ctx, REG_AX_FRAME_CTR);
    }
    if (fc != frameCfg) {
        ApolloLogU(Ctx, "FRAME_CTR %lu != FRAME_CFG %lu after arm%s", fc, frameCfg,
                  fc == 0 ? " (zero: start may be ignored)" : "");
    }
    ApolloVectorEnable(Ctx, IMR_HI_END_OF_BUFFER, TRUE);                            /* 15 */

    /* ARM and START need time between them, or START is silently ignored. */
    ApolloSleepMs(2);

    ApolloClearStalePending(Ctx);
    ApolloWrite(Ctx, REG_AX_CONTROL, AX_CTRL_START);
    ctrl = ApolloRead(Ctx, REG_AX_CONTROL);
    if (ctrl == 0xFFFFFFFF) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    ApolloCheckAxFaults(Ctx, ctrl, "start");

    Ctx->TransportRunning = TRUE;
    Ctx->LastRawPos = ApolloRead(Ctx, REG_AX_SAMPLE_POS) & (frames - 1);
    Ctx->ExtPos = Ctx->LastRawPos;
    Ctx->LastPosQpc = KeQueryPerformanceCounter(NULL).QuadPart;
    Ctx->PlayValidUntil = 0;
    Ctx->PlayZeroedUntil = frames;          /* the whole ring is zero already */
    ApolloLogU(Ctx, "transport started: AX_CONTROL 0x%03x SAMPLE_POS %lu FRAME_CTR %lu AX_STATUS 0x%08x",
              ctrl, Ctx->LastRawPos, ApolloRead(Ctx, REG_AX_FRAME_CTR),
              ApolloRead(Ctx, REG_AX_STATUS));

    /* 2.5: the clock write must come AFTER the start, or capture records silence. */
    ApolloWrite(Ctx, REG_NOTIF_CLOCK_CFG, (Ctx->RateIndex << 8) | CLOCK_SOURCE_INTERNAL);
    ApolloWrite(Ctx, REG_AX_CONNECT, AX_CONNECT_CLOCK);
    ApolloRateClockWritten(Ctx, Ctx->RateIndex);    /* [Rate] the Hi-band dwell starts here */
    ApolloClockTransportStarted(Ctx);   /* [Clock] on Internal now: an external source goes back on later (clock.c) */
    if (ext) {
        NTSTATUS rs;

        ApolloStepPassed(Ctx, APOLLO_BSTEP_XPORT_START);
        if (plan->ConfirmRate) {
            /* [Twin X G3] 48 kHz explicitly: logged; SAMPLE_POS must move; the live clock mask */
            /* [G4] notifications acknowledged while it waits (2 s), then the 48 kHz rate ack */
            rs = ApolloAudioExtConfirmRate(Ctx, plan);
            if (!NT_SUCCESS(rs)) {
                ApolloTransportStop(Ctx);
                return rs;
            }
        }
    }
    if (Ctx->DspToldRate != Ctx->SampleRate) {
        (VOID)ApolloDspRateAck(Ctx, Ctx->SampleRate);
    }

    /* The 1 ms copy engine runs exactly while the transport does (engine.c). */
    if (Engine) {
        NTSTATUS es = ApolloEngineStart(Ctx);

        if (!NT_SUCCESS(es)) {
            ApolloLogU(Ctx, "engine did not start (0x%08x): the service pass keeps the ring", es);
        } else if (ext) {
            ApolloStepPassed(Ctx, APOLLO_BSTEP_ENGINE);
        }
    }
    return STATUS_SUCCESS;
}

VOID ApolloTransportStop(_In_ PDEVICE_CONTEXT Ctx)
{
    ApolloEngineStop(Ctx);              /* before the ring or the position can change */
    if (!Ctx->TransportRunning) {
        return;
    }
    ApolloVectorDisable(Ctx, IMR_HI_END_OF_BUFFER);
    ApolloWrite(Ctx, REG_AX_CONTROL, AX_CTRL_STOP);
    Ctx->TransportRunning = FALSE;
    ApolloLogU(Ctx, "transport stopped: AX_CONTROL 0x%03x SAMPLE_POS %lu",
              ApolloRead(Ctx, REG_AX_CONTROL), ApolloRead(Ctx, REG_AX_SAMPLE_POS));
}

/* ------------------------------------------------------------------ */
/* Position: extend the 8192-frame SAMPLE_POS to 64 bits               */
/* ------------------------------------------------------------------ */

VOID ApolloPositionUpdate(_In_ PDEVICE_CONTEXT Ctx, _Out_opt_ PAPOLLO_POSITION Out)
{
    LARGE_INTEGER freq, now;
    ULONG raw, delta, frames = Ctx->RingFrames ? Ctx->RingFrames : AUDIO_RING_FRAMES;

    now = KeQueryPerformanceCounter(&freq);
    Ctx->QpcFrequency = freq.QuadPart;
    raw = ApolloRead(Ctx, REG_AX_SAMPLE_POS);

    if (Ctx->TransportRunning && raw != 0xFFFFFFFF) {
        ULONGLONG expected;

        raw &= frames - 1;
        delta = (raw - Ctx->LastRawPos) & (frames - 1);
        /* If we were late by more than half a ring, add the wraps we missed. */
        expected = (ULONGLONG)(now.QuadPart - Ctx->LastPosQpc) * Ctx->SampleRate /
                   (ULONGLONG)freq.QuadPart;
        if (expected > delta + frames / 2) {
            ULONGLONG wraps = (expected - delta + frames / 2) / frames;

            ApolloLogU(Ctx, "position read late by %llu frames: adding %llu wraps", expected, wraps);
            Ctx->ExtPos += wraps * frames;
        }
        Ctx->ExtPos += delta;
        Ctx->LastRawPos = raw;
        Ctx->LastPosQpc = now.QuadPart;
    }

    if (Out != NULL) {
        Out->RawSamplePos = raw;
        Out->FrameCtr = ApolloRead(Ctx, REG_AX_FRAME_CTR);
        Out->AxControl = ApolloRead(Ctx, REG_AX_CONTROL);
        Out->Flags = (raw == 0xFFFFFFFF) ? APOLLO_POS_FLAG_DEAD_READ : 0;
        Out->Position = Ctx->ExtPos;
        Out->QpcTimestamp = now.QuadPart;
        Out->QpcFrequency = freq.QuadPart;
    }
}

/* Copy Frames frames between a linear buffer and the ring, handling the wrap. */
static VOID ApolloRingCopy(_In_ PUCHAR RingVa, _In_ ULONG RingFrames, _In_ ULONG Stride,
                           _In_ ULONGLONG Start, _In_ ULONG Frames,
                           _Inout_ PUCHAR Linear, _In_ BOOLEAN ToRing)
{
    ULONG first = (ULONG)(Start % RingFrames);
    ULONG n1 = min(Frames, RingFrames - first);
    ULONG n2 = Frames - n1;

    if (ToRing) {
        RtlCopyMemory(RingVa + (SIZE_T)first * Stride, Linear, (SIZE_T)n1 * Stride);
        if (n2) {
            RtlCopyMemory(RingVa, Linear + (SIZE_T)n1 * Stride, (SIZE_T)n2 * Stride);
        }
    } else {
        RtlCopyMemory(Linear, RingVa + (SIZE_T)first * Stride, (SIZE_T)n1 * Stride);
        if (n2) {
            RtlCopyMemory(Linear + (SIZE_T)n1 * Stride, RingVa, (SIZE_T)n2 * Stride);
        }
    }
}

#define CAPTURE_GUARD_FRAMES    16      /* frames at the head may still be in flight */
#define CAPTURE_KEEP_FRAMES     (AUDIO_RING_FRAMES - 256)

NTSTATUS ApolloCaptureCopy(_In_ PDEVICE_CONTEXT Ctx, _In_ PAPOLLO_CAPTURE_REQUEST Req,
                           _Out_writes_bytes_(OutLen) PVOID Out, _In_ SIZE_T OutLen,
                           _Out_ SIZE_T *Written)
{
    PAPOLLO_CAPTURE_HEADER hdr = (PAPOLLO_CAPTURE_HEADER)Out;
    ULONG stride = Ctx->RecChannels * 4, raw;
    ULONGLONG p, end, start = Req->StartPosition;
    LONGLONG qpc;
    SIZE_T room;
    ULONG frames;

    *Written = 0;
    if (OutLen < sizeof(*hdr)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlZeroMemory(hdr, sizeof(*hdr));
    hdr->Channels = Ctx->RecChannels;
    *Written = sizeof(*hdr);
    if (!Ctx->TransportRunning) {
        return STATUS_DEVICE_NOT_READY;
    }

    ApolloPositionGet(Ctx, &p, &qpc, &raw);
    hdr->HwPosition = p;
    hdr->StartPosition = start;
    end = (p > CAPTURE_GUARD_FRAMES) ? p - CAPTURE_GUARD_FRAMES : 0;

    if (end > CAPTURE_KEEP_FRAMES && start < end - CAPTURE_KEEP_FRAMES) {
        hdr->Flags |= APOLLO_CAPTURE_OVERRUN;
        return STATUS_SUCCESS;
    }
    if (start >= end) {
        return STATUS_SUCCESS;
    }
    room = (OutLen - sizeof(*hdr)) / stride;
    frames = (ULONG)min((ULONGLONG)(end - start), (ULONGLONG)Req->MaxFrames);
    frames = (ULONG)min((SIZE_T)frames, room);

    ApolloRingCopy(Ctx->Rec.Va, Ctx->RingFrames, stride, start, frames,
                   (PUCHAR)(hdr + 1), FALSE);
    hdr->Frames = frames;
    *Written = sizeof(*hdr) + (SIZE_T)frames * stride;
    return STATUS_SUCCESS;
}

#define LEVEL_WINDOW_FRAMES     1024    /* ~21 ms at 48 kHz; [Rate] the same time at any rate (LevelWindow) */
#define PLAYBACK_TICK_FRAMES    48      /* one 1 ms engine tick at 48 kHz: the snapshot's age ([Rate] TickFrames) */
#define PLAYBACK_MAX_FRAMES     (APOLLO_ZERO_AHEAD_FRAMES * 3 / 4)  /* fits the write window */

/* Peak per record slot over the newest LEVEL_WINDOW_FRAMES frames (meters). */
VOID ApolloLevelsRead(_In_ PDEVICE_CONTEXT Ctx, _Out_ PAPOLLO_LEVELS L)
{
    ULONG chans = Ctx->RecChannels, ringFrames = Ctx->RingFrames, n, ch, f, raw;
    ULONG window = min(ApolloRateScale(LEVEL_WINDOW_FRAMES, Ctx->SampleRate), 4096ul);   /* [Rate] */
    ULONGLONG end, start, pos;
    LONGLONG qpc;

    RtlZeroMemory(L, sizeof(*L));
    n = min(chans, (ULONG)APOLLO_LEVEL_CHANNELS);
    L->Channels = n;
    if (!Ctx->TransportRunning || Ctx->Rec.Va == NULL || n == 0 || ringFrames == 0 ||
        (ULONGLONG)ringFrames * chans * 4 > AUDIO_RING_BYTES) {
        return;
    }
    ApolloPositionGet(Ctx, &pos, &qpc, &raw);
    end = (pos > CAPTURE_GUARD_FRAMES) ? pos - CAPTURE_GUARD_FRAMES : 0;
    if (end < window) {
        return;
    }
    start = end - window;
    for (f = 0; f < window; f++) {
        const LONG *frame = (const LONG *)(Ctx->Rec.Va +
                                           (SIZE_T)((start + f) % ringFrames) * chans * 4);

        for (ch = 0; ch < n; ch++) {
            LONG s = frame[ch] / 256;           /* 24 valid bits, MSB-aligned */
            ULONG a = (ULONG)(s < 0 ? -s : s);

            if (a > L->Peak[ch]) {
                L->Peak[ch] = a;
            }
        }
    }
    L->Valid = 1;
    L->Frames = window;
    L->Position = end;
}

NTSTATUS ApolloPlaybackCopy(_In_ PDEVICE_CONTEXT Ctx, _In_reads_bytes_(InLen) PVOID In,
                            _In_ SIZE_T InLen, _Out_ PAPOLLO_PLAYBACK_RESULT Result)
{
    PAPOLLO_PLAYBACK_HEADER hdr = (PAPOLLO_PLAYBACK_HEADER)In;
    ULONG stride = Ctx->PlayChannels * 4, raw;
    ULONGLONG p, until;
    LONGLONG qpc;

    RtlZeroMemory(Result, sizeof(*Result));
    if (InLen < sizeof(*hdr)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (hdr->Channels != Ctx->PlayChannels || hdr->Frames == 0 ||
        hdr->Frames > PLAYBACK_MAX_FRAMES ||
        InLen < sizeof(*hdr) + (SIZE_T)hdr->Frames * stride) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!Ctx->TransportRunning) {
        return STATUS_DEVICE_NOT_READY;
    }
    ApolloPositionGet(Ctx, &p, &qpc, &raw);
    Result->HwPosition = p;

    /*
     * The window is [p + RenderLeadFrames + one tick, p + zero horizon - 64). The
     * engine's clients have already added their data up to p + RenderLeadFrames,
     * and a copy there would overwrite it; p is the engine's snapshot, up to one
     * 1 ms tick (48 frames) old, hence the extra tick. On the early side, only
     * frames the zero horizon has already passed may be written: beyond it the
     * tick would race this copy, and a claim past the horizon would leave stale
     * ring audio unzeroed in the gap.
     */
    if (ApolloEngineLost(Ctx)) {
        return STATUS_DEVICE_NOT_READY;         /* the position is frozen */
    }
    if (hdr->StartPosition < p + Ctx->EngLead + ApolloRateTickFrames(Ctx->SampleRate)) {
        Result->Result = APOLLO_PLAYBACK_LATE;
        return STATUS_SUCCESS;
    }
    if (hdr->StartPosition + hdr->Frames > p + APOLLO_ZERO_AHEAD_FRAMES - 64) {
        Result->Result = APOLLO_PLAYBACK_TOO_EARLY;
        return STATUS_SUCCESS;
    }
    Result->Result = APOLLO_PLAYBACK_WRITTEN;
    /*
     * Claim the frames BEFORE copying, so the engine's zero horizon (which
     * skips frames below PlayValidUntil) cannot wipe them after the copy.
     * Engine clients still ADD into these frames when they reach them.
     */
    until = hdr->StartPosition + hdr->Frames;
    if (until > ApolloPlayValidUntil(Ctx)) {
        (VOID)InterlockedExchange64((volatile LONG64 *)&Ctx->PlayValidUntil, (LONG64)until);
    }
    ApolloRingCopy(Ctx->Play.Va, Ctx->RingFrames, stride, hdr->StartPosition, hdr->Frames,
                   (PUCHAR)(hdr + 1), TRUE);
    return STATUS_SUCCESS;
}

/*
 * The transport loops the playback ring forever. Zero what is about to
 * play unless a client wrote it, so stale audio never repeats.
 */
VOID ApolloPlaybackZeroAhead(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONGLONG p, target, start, f;
    ULONG stride = Ctx->PlayChannels * 4;

    if (!Ctx->TransportRunning) {
        return;
    }
    p = Ctx->ExtPos;
    target = p + Ctx->RingFrames / 2;
    start = max(max(Ctx->PlayZeroedUntil, Ctx->PlayValidUntil), p);
    for (f = start; f < target; ) {
        ULONG slot = (ULONG)(f % Ctx->RingFrames);
        ULONG n = (ULONG)min(target - f, (ULONGLONG)(Ctx->RingFrames - slot));

        RtlZeroMemory(Ctx->Play.Va + (SIZE_T)slot * stride, (SIZE_T)n * stride);
        f += n;
    }
    if (target > Ctx->PlayZeroedUntil) {
        Ctx->PlayZeroedUntil = target;
    }
}

/* ------------------------------------------------------------------ */
/* Hardware quiesce (spec-bringup 4.1), default: keep the mixer DSP    */
/* ------------------------------------------------------------------ */

VOID ApolloHwQuiesce(_In_ PDEVICE_CONTEXT Ctx)
{
    ApolloEngineStop(Ctx);                                          /* the tick first */
    ApolloVectorDisable(Ctx, IMR_HI_END_OF_BUFFER);                 /* 3 */
    ApolloWrite(Ctx, REG_AX_CONTROL, AX_CTRL_STOP);
    Ctx->TransportRunning = FALSE;

    (VOID)ApolloRingsQuiesce(Ctx);                                  /* 3b */

    ApolloWrite(Ctx, REG_AX_CONTROL, AX_CTRL_STOP);                 /* 5 */
    (VOID)ApolloRead(Ctx, REG_AX_CONTROL);
    ApolloWrite(Ctx, REG_AX_P2P_ENABLE, 0);
    ApolloVectorDisable(Ctx, IMR_HI_NOTIFICATION);

    ApolloWrite(Ctx, REG_AX_CONNECT, AX_CONNECT_DISCONNECT);        /* 6: always */
    (VOID)ApolloRead(Ctx, REG_FPGA_REV);

    ApolloMaskAll(Ctx, TRUE);                                       /* 7 */
    Ctx->Connected = FALSE;
    Ctx->MixerReady = FALSE;
    Ctx->AcefaceDone = FALSE;
}
