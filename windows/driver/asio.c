/*
 * asio.c - the kernel half of ASIO (docs/ASIO.md, include/apollo_asio.h).
 *
 * One client at a time. ASIO_OPEN runs in the caller's context
 * (EvtIoInCallerContext): it creates a pagefile-backed section, locks its pages
 * for the engine, maps a view into the calling process and references the
 * client's auto-reset event. The 1 ms engine (engine.c) then, under EngineLock:
 *   capture  copies record-ring slots into In[half][ch][i] as the hardware
 *            records them; when a half is full it publishes it in the shared
 *            header (seqlock: HalfReady, SwitchCount, SamplePosition, Qpc)
 *            and sets the event;
 *   render   adds (saturating) Out[half][ch][i] into the playback ring for
 *            the frames from the data head up to SAMPLE_POS + lead. Half h is
 *            played only if the client stored the switch that published it in
 *            ClientDone[h] before the half was due; otherwise silence (underrun).
 *
 * Timing (all in frames, N = BufferFrames, ASIO frame a):
 *   capture: a is recorded at hardware frame Origin + a; period P = a / N is
 *            published once Origin + (P+1)N <= SAMPLE_POS - guard.
 *   render:  a plays at Origin + a + Delay, Delay = 2N + guard + lead, so the
 *            client has exactly one period between the switch for P and the
 *            moment the engine needs Out[P & 1]. Output latency beyond the
 *            buffer = lead + FRAME_CFG; input latency = guard + ADC.
 *            [Lat] What the shared block reports adds the device side beyond
 *            the engine (ApolloLatencyIn / Out): the strip path while the
 *            strips run (DSP block, record delay, an over-budget chain) and the
 *            DAC; the 10 ms service pass keeps it current (ApolloLatencyRefresh).
 * The shared page is user-writable: the engine reads only ClientDone[] from it
 * and keeps its own copy of everything else.
 *
 * Memory: a section, not MDL pages mapped into the process. The user view
 * holds its own reference to the section, so whatever order things go away in
 * (owner exits, a duplicated handle closes last in another process, the device
 * is removed), the process never keeps a view of freed pages, and unmapping the
 * view is a ZwUnmapViewOfSection that fails harmlessly if the view or the
 * process is already gone. The engine only touches SysVa, a non-pageable
 * system mapping of the locked pages.
 *
 * Handles: only the process that opened the handle may use ASIO on it
 * (APOLLO_FILE.Creator). Cleanup marks the handle so a racing ASIO_OPEN fails,
 * and close releases a client that slipped through anyway.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <ntifs.h>
#include "apollo.h"

#define ASIO_NO_PERIOD          (~0ULL)
#define ASIO_MAX_LAG            4096u       /* capture lag that forces a re-origination */

#ifndef PROCESS_VM_OPERATION
#define PROCESS_VM_OPERATION    0x0008
#endif

/* ------------------------------------------------------------------ */
/* Engine side (EngineLock held, DISPATCH_LEVEL)                       */
/* ------------------------------------------------------------------ */

/* Re-originate the client at the current capture head (start, engine restart, long stall). */
static VOID AsioSync(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONGLONG Head)
{
    PAPOLLO_ASIO a = &Ctx->Asio;

    a->Origin = Head;
    a->Delay = 2ULL * a->N + Ctx->EngGuard + Ctx->EngLead;
    a->CapNext = 0;
    a->RenderKnown = FALSE;
    a->RenderPlay = FALSE;
    a->PubPeriod[0] = a->PubPeriod[1] = ASIO_NO_PERIOD;
    a->SampleOffset = a->NextSample;     /* SamplePosition never goes backwards */
    a->NeedSync = FALSE;
}

VOID ApolloAsioEngineStarted(_In_ PDEVICE_CONTEXT Ctx)
{
    if (Ctx->Asio.Open && Ctx->Asio.Active) {
        Ctx->Asio.NeedSync = TRUE;
        Ctx->Asio.Resyncs++;
    }
}

/* The client finished exactly the switch that published half Half (not an older or newer one). */
static BOOLEAN AsioClientDid(_In_ PAPOLLO_ASIO A, _In_ ULONG Half)
{
    return ReadNoFence(&A->Shared->ClientDone[Half]) == A->PubSwitch[Half];
}

VOID ApolloAsioRender(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONGLONG From, _In_ ULONGLONG To)
{
    PAPOLLO_ASIO a = &Ctx->Asio;
    ULONG stride = Ctx->PlayChannels * 4, mask = Ctx->RingFrames - 1, nmask, c;
    ULONGLONG h, start;

    if (!a->Open || !a->Active || a->NeedSync || a->OutCount == 0) {
        return;
    }
    nmask = a->N - 1;
    start = a->Origin + a->Delay;
    h = max(From, start);
    for (; h < To; h++) {
        ULONGLONG f = h - start;                /* ASIO render frame */
        ULONGLONG period = f >> a->Shift;
        ULONG i = (ULONG)(f & nmask), half = (ULONG)(period & 1);
        LONG *frame;
        const LONG *src;

        if (!a->RenderKnown || period != a->RenderPeriod) {
            /* Decide once per period, at its first frame. */
            a->RenderKnown = TRUE;
            a->RenderPeriod = period;
            a->RenderPlay = a->PubPeriod[half] == period && AsioClientDid(a, half);
            if (!a->RenderPlay) {
                a->Underruns++;
                WriteNoFence(&a->Shared->Underruns, a->Underruns);
            }
        }
        if (!a->RenderPlay) {
            continue;                           /* the zero horizon already silenced it */
        }
        frame = (LONG *)(Ctx->Play.Va + (SIZE_T)(h & mask) * stride);
        src = a->Out + (SIZE_T)half * a->OutCount * a->N + i;
        for (c = 0; c < a->OutCount; c++) {
            LONG *dst = &frame[a->OutSlots[c]];

            *dst = ApolloSatAdd32(*dst, src[(SIZE_T)c * a->N]);
        }
    }
}

static LONGLONG AsioQpcAt(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONGLONG Frame)
{
    LONGLONG back = (LONGLONG)(Ctx->EngPos - Frame);

    if (Ctx->SampleRate == 0) {
        return Ctx->EngQpc;
    }
    return Ctx->EngQpc - back * Ctx->EngQpcFreq / (LONGLONG)Ctx->SampleRate;
}

VOID ApolloAsioCapture(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONGLONG Head)
{
    PAPOLLO_ASIO a = &Ctx->Asio;
    PAPOLLO_ASIO_SHARED sh;
    ULONG stride = Ctx->RecChannels * 4, mask = Ctx->RingFrames - 1, nmask, c;
    BOOLEAN signal = FALSE;
    ULONGLONG end;

    if (!a->Open || !a->Active) {
        return;
    }
    sh = a->Shared;
    if (a->NeedSync || Head < a->Origin || Head - a->Origin - a->CapNext > ASIO_MAX_LAG) {
        if (!a->NeedSync) {
            a->Resyncs++;                       /* a stall longer than half the ring */
        }
        AsioSync(Ctx, Head);
        return;
    }
    nmask = a->N - 1;
    end = Head - a->Origin;
    while (a->CapNext < end) {
        ULONGLONG f = a->CapNext;
        ULONGLONG period = f >> a->Shift;
        ULONG i = (ULONG)(f & nmask), half = (ULONG)(period & 1);
        const LONG *frame = (const LONG *)(Ctx->Rec.Va + (SIZE_T)((a->Origin + f) & mask) * stride);
        LONG *dst = a->In + (SIZE_T)half * a->InCount * a->N + i;

        if (i == 0 && a->PubPeriod[half] != ASIO_NO_PERIOD && !AsioClientDid(a, half)) {
            /* This half still holds a period the client never finished (or skipped). */
            a->Overruns++;
            WriteNoFence(&sh->Overruns, a->Overruns);
        }
        for (c = 0; c < a->InCount; c++) {
            dst[(SIZE_T)c * a->N] = frame[a->InSlots[c]];
        }
        a->CapNext = f + 1;
        if (i == nmask) {
            /* Period complete: publish it. */
            LONG s = ++a->Switches;

            a->PubPeriod[half] = period;
            a->PubSwitch[half] = s;
            InterlockedIncrement(&sh->Sequence);        /* odd: being written */
            WriteNoFence(&sh->HalfReady, (LONG)half);
            WriteNoFence(&sh->SwitchCount, s);
            WriteNoFence64(&sh->SamplePosition, (LONG64)(a->SampleOffset + (period << a->Shift)));
            WriteNoFence64(&sh->Qpc, AsioQpcAt(Ctx, a->Origin + (period << a->Shift)));
            InterlockedIncrement(&sh->Sequence);        /* even: stable */
            a->NextSample = a->SampleOffset + ((period + 1) << a->Shift);
            signal = TRUE;
        }
    }
    if (signal) {
        (VOID)KeSetEvent(a->Event, EVENT_INCREMENT, FALSE);
    }
}

/* ------------------------------------------------------------------ */
/* IOCTLs (caller context, PASSIVE_LEVEL, AsioLock held)               */
/* ------------------------------------------------------------------ */

static ULONG AsioLog2(_In_ ULONG V)
{
    ULONG s = 0;

    while ((1u << s) < V) {
        s++;
    }
    return s;
}

/*
 * Remove the view from the owner process, from any context. Through a kernel
 * process handle this is safe even if the owner has exited or is exiting (the
 * call then fails), or if the client already unmapped the view itself.
 */
_IRQL_requires_(PASSIVE_LEVEL)
static VOID AsioUnmapUser(_In_ PAPOLLO_ASIO A)
{
    HANDLE process = NULL;
    NTSTATUS status;

    if (PsGetCurrentProcess() == A->OwnerProcess) {
        status = ZwUnmapViewOfSection(ZwCurrentProcess(), A->UserVa);
    } else {
        status = ObOpenObjectByPointer(A->OwnerProcess, OBJ_KERNEL_HANDLE, NULL, PROCESS_VM_OPERATION,
                                       *PsProcessType, KernelMode, &process);
        if (NT_SUCCESS(status)) {
            status = ZwUnmapViewOfSection(process, A->UserVa);
            ZwClose(process);
        }
    }
    if (!NT_SUCCESS(status)) {
        /* Gone already, or the process is exiting: its teardown removes the view. */
        ApolloLogU(CONTAINING_RECORD(A, DEVICE_CONTEXT, Asio), "asio: view not unmapped (0x%08x); it goes with the process", status);
    }
}

/* Unmap, unlock, dereference. EngineLock is taken here; AsioLock is held by the caller. */
_IRQL_requires_(PASSIVE_LEVEL)
static VOID AsioRelease(_In_ PDEVICE_CONTEXT Ctx)
{
    PAPOLLO_ASIO a = &Ctx->Asio;
    KIRQL irql;

    if (!a->Open) {
        return;
    }
    /* After this the tick no longer touches the pages or the event. */
    KeAcquireSpinLock(&Ctx->EngineLock, &irql);
    a->Active = FALSE;
    a->Open = FALSE;
    KeReleaseSpinLock(&Ctx->EngineLock, irql);

    if (a->UserVa != NULL) {
        AsioUnmapUser(a);
    }
    if (a->Mdl != NULL) {
        if (a->SysVa != NULL) {
            MmUnmapLockedPages(a->SysVa, a->Mdl);
        }
        MmUnlockPages(a->Mdl);
        IoFreeMdl(a->Mdl);
    }
    if (a->SysView != NULL) {
        (VOID)MmUnmapViewInSystemSpace(a->SysView);
    }
    if (a->Section != NULL) {
        ObDereferenceObject(a->Section);    /* the pages live on while a user view exists */
    }
    if (a->Event != NULL) {
        ObDereferenceObject(a->Event);
    }
    if (a->OwnerProcess != NULL) {
        ObDereferenceObject(a->OwnerProcess);
    }
    ApolloLogU(Ctx, "asio: closed (%ld switches, %ld underruns, %ld overruns, %lu resyncs)",
              a->Switches, a->Underruns, a->Overruns, a->Resyncs);
    a->OwnerFile = NULL;
    a->OwnerProcess = NULL;
    a->Section = NULL;
    a->SysView = NULL;
    a->Mdl = NULL;
    a->SysVa = NULL;
    a->UserVa = NULL;
    a->Event = NULL;
    a->Shared = NULL;
    a->In = a->Out = NULL;
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS AsioOpen(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFREQUEST Request, _In_ WDFFILEOBJECT File,
                         _Out_ ULONG_PTR *Info)
{
    PAPOLLO_ASIO a = &Ctx->Asio;
    APOLLO_ASIO_OPEN_IN in;
    PAPOLLO_ASIO_OPEN_IN pin = NULL;
    PAPOLLO_ASIO_OPEN_OUT pout = NULL;
    APOLLO_ASIO_OPEN_OUT out;
    OBJECT_ATTRIBUTES oa;
    LARGE_INTEGER maxSize;
    HANDLE sectionHandle = NULL;
    PVOID section = NULL, sysView = NULL, user = NULL;
    SIZE_T viewSize;
    PMDL mdl = NULL;
    BOOLEAN locked = FALSE;
    PUCHAR sys = NULL;
    PKEVENT event = NULL;
    PAPOLLO_ASIO_SHARED sh;
    ULONG i, n, half[2], inBytes, outBytes, total;
    size_t len = 0;
    NTSTATUS status;
    KIRQL irql;

    *Info = 0;
    if (WdfRequestGetRequestorMode(Request) != UserMode) {
        return STATUS_INVALID_DEVICE_REQUEST;   /* the mapping is for a user process */
    }
    status = WdfRequestRetrieveInputBuffer(Request, sizeof(in), (PVOID *)&pin, &len);
    if (!NT_SUCCESS(status)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    in = *pin;                                  /* METHOD_BUFFERED: copy before any output */
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(out), (PVOID *)&pout, &len);
    if (!NT_SUCCESS(status)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    n = in.BufferFrames;
    if (in.Version != APOLLO_ASIO_VERSION || n < APOLLO_ASIO_MIN_FRAMES ||
        n > APOLLO_ASIO_MAX_FRAMES || (n & (n - 1)) != 0 ||
        in.InCount > APOLLO_ASIO_MAX_IN || in.OutCount > APOLLO_ASIO_MAX_OUT ||
        in.InCount + in.OutCount == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Ctx->Stage == ApolloStageGone || ApolloGateClosed(Ctx)) {
        return STATUS_NO_SUCH_DEVICE;
    }
    if (!Ctx->Unit.Active) {
        return STATUS_DEVICE_NOT_READY;         /* an observed or refused unit: no audio, ever */
    }
    {
        /* ASIO stays the x8p's (a Mode 1 Twin X: none) */
        const APOLLO_BRINGUP_PLAN *plan = ApolloPlanOf(Ctx);

        if (plan == NULL || !plan->Asio ||
            !ApolloPlanIoctlAllowed(plan, Ctx->Unit.Value[APOLLO_PARAM_MODE], APOLLO_IOCTL_CLASS_ASIO)) {
            return STATUS_DEVICE_NOT_READY;
        }
    }
    if (Ctx->Rec.Va == NULL || Ctx->Play.Va == NULL || Ctx->RecChannels == 0 || Ctx->PlayChannels == 0) {
        return STATUS_DEVICE_NOT_READY;
    }
    for (i = 0; i < in.InCount; i++) {
        if (in.InSlots[i] >= Ctx->RecChannels) {
            return STATUS_INVALID_PARAMETER;
        }
    }
    for (i = 0; i < in.OutCount; i++) {
        if (in.OutSlots[i] >= Ctx->PlayChannels) {
            return STATUS_INVALID_PARAMETER;
        }
    }
    if (a->Open) {
        return STATUS_DEVICE_BUSY;              /* one client at a time */
    }
    if (ReadAcquire(&Ctx->RateChanging) != 0) {
        return STATUS_DEVICE_BUSY;              /* [Rate] a rate change is under way (rate.c RateBegin) */
    }

    /* Layout: header page, then In[2][InCount][N], then Out[2][OutCount][N], page-aligned. */
    half[0] = in.InCount * n * 4;
    half[1] = in.OutCount * n * 4;
    inBytes = ROUND_TO_PAGES(2 * half[0]);
    outBytes = ROUND_TO_PAGES(2 * half[1]);
    total = PAGE_SIZE + inBytes + outBytes;

    /* A pagefile-backed section (pages arrive zeroed), locked for the engine. */
    maxSize.QuadPart = total;
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    status = ZwCreateSection(&sectionHandle, SECTION_MAP_READ | SECTION_MAP_WRITE, &oa, &maxSize,
                             PAGE_READWRITE, SEC_COMMIT, NULL);
    if (!NT_SUCCESS(status)) {
        sectionHandle = NULL;
        goto fail;
    }
    status = ObReferenceObjectByHandle(sectionHandle, SECTION_MAP_READ | SECTION_MAP_WRITE, NULL,
                                       KernelMode, &section, NULL);
    if (!NT_SUCCESS(status)) {
        section = NULL;
        goto fail;
    }
    viewSize = 0;
    status = MmMapViewInSystemSpace(section, &sysView, &viewSize);
    if (!NT_SUCCESS(status)) {
        sysView = NULL;
        goto fail;
    }
    mdl = IoAllocateMdl(sysView, total, FALSE, FALSE, NULL);
    if (mdl == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto fail;
    }
    __try {
        MmProbeAndLockPages(mdl, KernelMode, IoWriteAccess);
        locked = TRUE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        status = STATUS_INSUFFICIENT_RESOURCES;
    }
    if (!locked) {
        goto fail;
    }
    sys = (PUCHAR)MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority | MdlMappingNoExecute);
    if (sys == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto fail;
    }
    status = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)in.EventHandle, EVENT_MODIFY_STATE,
                                       *ExEventObjectType, UserMode, (PVOID *)&event, NULL);
    if (!NT_SUCCESS(status)) {
        event = NULL;
        goto fail;
    }
    /* The caller's view: read/write, never executable, not inherited by child processes. */
    viewSize = total;
    status = ZwMapViewOfSection(sectionHandle, ZwCurrentProcess(), &user, 0, 0, NULL, &viewSize,
                                ViewUnmap, 0, PAGE_READWRITE);
    if (!NT_SUCCESS(status)) {
        user = NULL;
        goto fail;
    }
    ZwClose(sectionHandle);                     /* the object reference and the views remain */
    sectionHandle = NULL;

    /* Fill the header through the kernel view. */
    sh = (PAPOLLO_ASIO_SHARED)sys;
    sh->Version = APOLLO_ASIO_VERSION;
    sh->BufferFrames = n;
    sh->InCount = in.InCount;
    sh->OutCount = in.OutCount;
    sh->QpcFrequency = Ctx->EngQpcFreq;
    if (sh->QpcFrequency == 0) {
        LARGE_INTEGER f;

        (VOID)KeQueryPerformanceCounter(&f);
        sh->QpcFrequency = f.QuadPart;
    }
    /* [Rate] at this rate: the ADC delay and FRAME_CFG of it (10 and 8 at 48 kHz); [Lat] + the device side */
    sh->InputLatencyFrames = ApolloLatencyIn(Ctx);
    sh->OutputLatencyFrames = ApolloLatencyOut(Ctx);
    sh->SampleRate = Ctx->SampleRate;

    RtlZeroMemory(&out, sizeof(out));
    out.SharedAddress = (ULONGLONG)(ULONG_PTR)user;
    out.SharedBytes = total;
    out.InOffset[0] = PAGE_SIZE;
    out.InOffset[1] = PAGE_SIZE + half[0];
    out.OutOffset[0] = PAGE_SIZE + inBytes;
    out.OutOffset[1] = PAGE_SIZE + inBytes + half[1];
    out.ChannelStride = n * 4;

    KeAcquireSpinLock(&Ctx->EngineLock, &irql);
    a->OwnerFile = File;
    a->OwnerProcess = PsGetCurrentProcess();
    ObReferenceObject(a->OwnerProcess);
    a->Section = section;
    a->SysView = sysView;
    a->Mdl = mdl;
    a->SysVa = sys;
    a->UserVa = user;
    a->Bytes = total;
    a->Event = event;
    a->Shared = sh;
    a->In = (PLONG)(sys + out.InOffset[0]);
    a->Out = (PLONG)(sys + out.OutOffset[0]);
    a->N = n;
    a->Shift = AsioLog2(n);
    a->InCount = in.InCount;
    a->OutCount = in.OutCount;
    RtlCopyMemory(a->InSlots, in.InSlots, sizeof(a->InSlots));
    RtlCopyMemory(a->OutSlots, in.OutSlots, sizeof(a->OutSlots));
    a->Active = FALSE;
    a->NeedSync = TRUE;
    a->Switches = a->Underruns = a->Overruns = 0;
    a->Resyncs = 0;
    a->NextSample = 0;
    a->SampleOffset = 0;
    a->Open = TRUE;
    KeReleaseSpinLock(&Ctx->EngineLock, irql);

    *pout = out;
    *Info = sizeof(out);
    ApolloLogU(Ctx, "asio: open, %lu frames, %lu in, %lu out, %lu bytes", n, in.InCount, in.OutCount, total);
    return STATUS_SUCCESS;

fail:
    if (event != NULL) {
        ObDereferenceObject(event);
    }
    if (mdl != NULL) {
        if (sys != NULL) {
            MmUnmapLockedPages(sys, mdl);
        }
        if (locked) {
            MmUnlockPages(mdl);
        }
        IoFreeMdl(mdl);
    }
    if (sysView != NULL) {
        (VOID)MmUnmapViewInSystemSpace(sysView);
    }
    if (section != NULL) {
        ObDereferenceObject(section);
    }
    if (sectionHandle != NULL) {
        ZwClose(sectionHandle);
    }
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS AsioStartStop(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFFILEOBJECT File, _In_ BOOLEAN Start)
{
    PAPOLLO_ASIO a = &Ctx->Asio;
    KIRQL irql;

    if (!a->Open || a->OwnerFile != File) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (Start && (!Ctx->TransportRunning || !ApolloEngineRunning(Ctx) || ApolloEngineLost(Ctx))) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (Start && ReadAcquire(&Ctx->RateChanging) != 0) {
        return STATUS_DEVICE_BUSY;              /* [Rate] its own SetSampleRate is still running */
    }
    KeAcquireSpinLock(&Ctx->EngineLock, &irql);
    if (Start && !a->Active) {
        a->Active = TRUE;
        a->NeedSync = TRUE;
    } else if (!Start) {
        a->Active = FALSE;
    }
    KeReleaseSpinLock(&Ctx->EngineLock, irql);
    return STATUS_SUCCESS;
}

/*
 * [Rate] AsioLock held, at the end of a rate change (rate.c RateEnd): an open client (the host whose
 * SetSampleRate asked for it; any other blocks the change) gets the new rate and latencies in its
 * shared block. The DLL reads them after its SET_RATE returned.
 */
VOID ApolloAsioRateChanged(_In_ PDEVICE_CONTEXT Ctx)
{
    PAPOLLO_ASIO a = &Ctx->Asio;
    KIRQL irql;

    if (!a->Open || a->Shared == NULL) {
        return;
    }
    KeAcquireSpinLock(&Ctx->EngineLock, &irql);
    a->Shared->SampleRate = Ctx->SampleRate;
    a->Shared->InputLatencyFrames = ApolloLatencyIn(Ctx);      /* [Lat] the strips come back at the new rate: the */
    a->Shared->OutputLatencyFrames = ApolloLatencyOut(Ctx);    /* service pass publishes their path when they do */
    a->NeedSync = TRUE;                         /* re-originated on the new run at the next tick */
    KeReleaseSpinLock(&Ctx->EngineLock, irql);
}

/* ------------------------------------------------------------------ */
/* [Lat] Reported latencies (docs/ASIO.md, Latency; rate_plan.h)        */
/* ------------------------------------------------------------------ */

/*
 * The input path beyond the engine and the ADC now: the strip DSP's block and the strips' record delay while
 * the unit's strips run (a strip input records through them), plus the worst routed chain's excess of this
 * strips' epoch (plugin.c). Reads only; a value the service pass or a plug-in call changes a moment later is
 * published again by the next pass.
 */
static ULONG LatInPathNow(_In_ PDEVICE_CONTEXT Ctx)
{
    const APOLLO_STRIPS *s = &Ctx->Strips;
    BOOLEAN live = FALSE;
    LONG excess = 0;
    ULONG i;

    if (s->Enable == 1 && s->State == APOLLO_STRIPS_STATE_LIVE && !s->Quarantined && !s->Tainted) {
        for (i = 0; i < APOLLO_STRIP_INPUTS; i++) {
            if (s->StripStarted[i]) {
                live = TRUE;
            }
        }
    }
    if (ReadNoFence(&Ctx->LatChainEpoch) == (LONG)s->Epoch) {
        excess = ReadNoFence(&Ctx->LatChainExcess);
    }
    return ApolloLatencyInPath(live, Ctx->RateIndex != 0 ? Ctx->RateIndex : APOLLO_RATE_IDX_48K,
                               excess > 0 ? (ULONG)excess : 0);
}

_Use_decl_annotations_
ULONG ApolloLatencyIn(PDEVICE_CONTEXT Ctx)
{
    return Ctx->EngGuard + Ctx->AdcFrames + LatInPathNow(Ctx);
}

_Use_decl_annotations_
ULONG ApolloLatencyOut(PDEVICE_CONTEXT Ctx)
{
    return Ctx->EngLead + Ctx->FrameCfg + Ctx->DacFrames;
}

/*
 * The 10 ms service pass, DevLock held, PASSIVE: publish the reported latencies when the device side moved
 * (the strips went Live or away, a chain's excess, a rate): the unit's ASIO block (EngineLock; the DLL then
 * sends kAsioLatenciesChanged) and the combined device (Agg.Lock, after EngineLock is released: lock order).
 * Not while a rate change runs (its end publishes the client's block, ApolloAsioRateChanged).
 */
_Use_decl_annotations_
VOID ApolloLatencyRefresh(PDEVICE_CONTEXT Ctx)
{
    PAPOLLO_ASIO a = &Ctx->Asio;
    ULONG in, out;
    KIRQL irql;

    if (ReadNoFence(&Ctx->RateChanging) != 0 || ApolloGateClosed(Ctx)) {
        return;
    }
    in = LatInPathNow(Ctx);
    out = Ctx->DacFrames;
    Ctx->LatInPath = in;
    if (Ctx->LatPublished && in == Ctx->LatPublishedIn && out == Ctx->LatPublishedOut) {
        return;
    }
    Ctx->LatPublished = TRUE;
    Ctx->LatPublishedIn = in;
    Ctx->LatPublishedOut = out;
    KeAcquireSpinLock(&Ctx->EngineLock, &irql);
    if (a->Open && a->Shared != NULL) {
        a->Shared->InputLatencyFrames = Ctx->EngGuard + Ctx->AdcFrames + in;
        a->Shared->OutputLatencyFrames = Ctx->EngLead + Ctx->FrameCfg + out;
    }
    KeReleaseSpinLock(&Ctx->EngineLock, irql);
    ApolloAggLatencyNote(Ctx, in, out);
    ApolloLogU(Ctx, "latency: reported input %lu + buffer (guard %lu, ADC %lu, strip path %lu%s), output %lu + buffer "
               "(lead %lu, FRAME_CFG %lu, DAC %lu) at %lu Hz", Ctx->EngGuard + Ctx->AdcFrames + in, Ctx->EngGuard,
               Ctx->AdcFrames, in, in == 0 ? ": no strips running" : "", Ctx->EngLead + Ctx->FrameCfg + out,
               Ctx->EngLead, Ctx->FrameCfg, out, Ctx->SampleRate);
}

static BOOLEAN ApolloIsAsioIoctl(_In_ ULONG Code)
{
    return Code == IOCTL_APOLLO_ASIO_OPEN || Code == IOCTL_APOLLO_ASIO_START ||
           Code == IOCTL_APOLLO_ASIO_STOP || Code == IOCTL_APOLLO_ASIO_CLOSE;
}

_IRQL_requires_(PASSIVE_LEVEL)
static VOID AsioDispatch(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFREQUEST Request, _In_ ULONG Code)
{
    WDFFILEOBJECT file = WdfRequestGetFileObject(Request);
    PAPOLLO_FILE f = (file != NULL) ? ApolloGetFile(file) : NULL;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info = 0;

    if (f == NULL) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }
    if (f->Creator != PsGetCurrentProcess()) {
        /* An inherited or duplicated handle: only the opener may map and drive ASIO. */
        WdfRequestComplete(Request, STATUS_ACCESS_DENIED);
        return;
    }
    if (ApolloAggAsioClaims(Request, Code, file)) {
        /* the combined client (version 3 open, or this handle owns it): agg.c, its own lock */
        ApolloAggAsioDispatch(Request, Code, file, f->CleanedUp);
        return;
    }
    WdfWaitLockAcquire(Ctx->AsioLock, NULL);
    switch (Code) {
    case IOCTL_APOLLO_ASIO_OPEN:
        if (f->CleanedUp) {
            status = STATUS_FILE_CLOSED;        /* raced the last CloseHandle */
        } else {
            status = AsioOpen(Ctx, Request, file, &info);
        }
        break;
    case IOCTL_APOLLO_ASIO_START:
        status = AsioStartStop(Ctx, file, TRUE);
        break;
    case IOCTL_APOLLO_ASIO_STOP:
        status = AsioStartStop(Ctx, file, FALSE);
        break;
    case IOCTL_APOLLO_ASIO_CLOSE:
        if (Ctx->Asio.Open && Ctx->Asio.OwnerFile == file) {
            AsioRelease(Ctx);
            status = STATUS_SUCCESS;
        } else {
            status = STATUS_INVALID_DEVICE_STATE;
        }
        break;
    default:
        break;
    }
    WdfWaitLockRelease(Ctx->AsioLock);
    WdfRequestCompleteWithInformation(Request, status, info);
}

/*
 * Every request passes here first, in the caller's thread. ASIO requests are
 * handled (and completed) here because ASIO_OPEN must map memory into the
 * caller's process; everything else goes on to the default queue unchanged.
 */
VOID ApolloEvtIoInCallerContext(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request)
{
    WDF_REQUEST_PARAMETERS p;
    NTSTATUS status;

    WDF_REQUEST_PARAMETERS_INIT(&p);
    WdfRequestGetParameters(Request, &p);
    if (p.Type == WdfRequestTypeDeviceControl &&
        ApolloRateIsIoctl(p.Parameters.DeviceIoControl.IoControlCode)) {
        /* [Rate] SET_RATE runs in the caller's thread: the unit's queue keeps answering meanwhile */
        if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
            WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
            return;
        }
        ApolloRateDispatch(ApolloGetContext(Device), Request);
        return;
    }
    if (p.Type == WdfRequestTypeDeviceControl &&
        ApolloIsAsioIoctl(p.Parameters.DeviceIoControl.IoControlCode)) {
        if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
            WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
            return;
        }
        AsioDispatch(ApolloGetContext(Device), Request, p.Parameters.DeviceIoControl.IoControlCode);
        return;
    }
    status = WdfDeviceEnqueueRequest(Device, Request);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
    }
}

/*
 * Last handle closed (in whichever process closed it last). Mark the handle so
 * an ASIO_OPEN still in flight on it fails, then release its client.
 */
VOID ApolloEvtFileCleanup(_In_ WDFFILEOBJECT FileObject)
{
    PDEVICE_CONTEXT ctx = ApolloGetContext(WdfFileObjectGetDevice(FileObject));
    PAPOLLO_FILE f = ApolloGetFile(FileObject);

    if (ctx->AsioLock == NULL) {
        return;
    }
    WdfWaitLockAcquire(ctx->AsioLock, NULL);
    if (f != NULL) {
        f->CleanedUp = TRUE;
    }
    if (ctx->Asio.Open && ctx->Asio.OwnerFile == FileObject) {
        AsioRelease(ctx);
    }
    WdfWaitLockRelease(ctx->AsioLock);
    ApolloAggAsioFileGone(FileObject);      /* the combined client, if this handle had it */
    /* the handle's plug-in instances: destroyed, plain strips restored (StripLock, never with AsioLock) */
    ApolloPluginFileCleanup(ctx, FileObject);
}

/* The file object goes away: backstop for a client that raced the cleanup; drop the creator. */
VOID ApolloEvtFileClose(_In_ WDFFILEOBJECT FileObject)
{
    PDEVICE_CONTEXT ctx = ApolloGetContext(WdfFileObjectGetDevice(FileObject));
    PAPOLLO_FILE f = ApolloGetFile(FileObject);

    if (ctx->AsioLock != NULL) {
        WdfWaitLockAcquire(ctx->AsioLock, NULL);
        if (ctx->Asio.Open && ctx->Asio.OwnerFile == FileObject) {
            ApolloLogU(ctx, "asio: client released at close (an open raced the cleanup)");
            AsioRelease(ctx);
        }
        WdfWaitLockRelease(ctx->AsioLock);
    }
    ApolloAggAsioFileGone(FileObject);      /* backstop, as above */
    ApolloPluginFileCleanup(ctx, FileObject);   /* backstop: a CREATE that raced the cleanup */
    if (f != NULL && f->Creator != NULL) {
        ObDereferenceObject(f->Creator);
        f->Creator = NULL;
    }
}

/* Device cleanup: a client still open here would outlive the device. */
_Use_decl_annotations_
VOID ApolloAsioCloseAny(PDEVICE_CONTEXT Ctx)
{
    if (Ctx->AsioLock == NULL) {
        return;
    }
    WdfWaitLockAcquire(Ctx->AsioLock, NULL);
    AsioRelease(Ctx);
    WdfWaitLockRelease(Ctx->AsioLock);
}
