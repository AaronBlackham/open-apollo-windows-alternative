/*
 * engine.c - the 1 ms copy engine (docs/PHASE2.md section 5).
 *
 * While the transport runs, a high-resolution timer (DISPATCH_LEVEL), re-armed
 * every tick on a fixed 1 ms grid (EngineRearm: a periodic timer at the clock
 * resolution folds late expirations and ran at ~650/s on the x8p host),
 * is the only reader of AX_SAMPLE_POS and the only writer of the playback
 * ring apart from the diagnostic WRITE_PLAYBACK IOCTL. Every tick it:
 *   1. reads SAMPLE_POS once and extends it to a 64-bit position (snapshot),
 *   2. zeroes the playback ring ZeroAhead frames ahead of the hardware, so a
 *      frame is always silent before any client adds into it and a stalled
 *      tick plays silence, never audio from 170 ms ago,
 *   3. lets render clients ADD (saturating) their data into the frames from
 *      the data head up to SAMPLE_POS + RenderLeadFrames,
 *   4. lets capture clients copy the record ring up to SAMPLE_POS minus
 *      CaptureGuardFrames.
 * Clients: the ASIO client (asio.c) and the ACX RT streams (stream.c).
 *
 * Locking: EngineLock (spin lock) around everything the tick touches. The
 * tick never takes DevLock or StripLock and never waits. The timer is
 * created and deleted under DevLock at PASSIVE_LEVEL, together with the
 * transport, so the ring layout cannot change under a running tick.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "apollo.h"

#define ENGINE_PERIOD_100NS         10000LL     /* 1 ms */
#define ENGINE_MIN_DUE_100NS        2500LL      /* overdue grid point: next clock tick, never back to back */
#define ENGINE_ZERO_AHEAD           APOLLO_ZERO_AHEAD_FRAMES
#define ENGINE_DEAD_LIMIT           8u          /* consecutive all-ones reads: the link is gone */
#define ENGINE_SLOW_LIMIT           2u          /* consecutive reads over 1 ms: stop touching it */
#define ENGINE_LEAD_DEFAULT         144u        /* 3 ms, includes FRAME_CFG */
#define ENGINE_LEAD_MIN             64u
#define ENGINE_LEAD_MAX             1024u
#define ENGINE_GUARD_DEFAULT        16u
#define ENGINE_GUARD_MAX            64u

static EXT_CALLBACK ApolloEngineTick;

/* ------------------------------------------------------------------ */
/* Configuration (PrepareHardware)                                     */
/* ------------------------------------------------------------------ */

/* [Twin X G0] the built-in defaults live in apollo_unit.h now; keep the two in step. */
C_ASSERT(ENGINE_LEAD_DEFAULT == APOLLO_DEF_RENDER_LEAD);
C_ASSERT(ENGINE_GUARD_DEFAULT == APOLLO_DEF_CAPTURE_GUARD);

VOID ApolloEngineReadConfig(_In_ PDEVICE_CONTEXT Ctx)
{
    /* This unit's values (unit.c: Units\<unit> -> Models\<type> -> legacy global (x8p) -> default). */
    Ctx->EngLead = ApolloUnitParam(Ctx, APOLLO_PARAM_RENDER_LEAD);
    Ctx->EngGuard = ApolloUnitParam(Ctx, APOLLO_PARAM_CAPTURE_GUARD);
    Ctx->EngStartAtConnect = ApolloUnitParam(Ctx, APOLLO_PARAM_START_AT_CONNECT);
    if (Ctx->EngLead < ENGINE_LEAD_MIN) {
        Ctx->EngLead = ENGINE_LEAD_MIN;
    }
    if (Ctx->EngLead > ENGINE_LEAD_MAX) {
        Ctx->EngLead = ENGINE_LEAD_MAX;
    }
    if (Ctx->EngGuard > ENGINE_GUARD_MAX) {
        Ctx->EngGuard = ENGINE_GUARD_MAX;
    }
    /* [Rate] the registry's values are frames at 48 kHz; the engine runs on them scaled to SampleRate */
    Ctx->EngLead48 = Ctx->EngLead;
    Ctx->EngGuard48 = Ctx->EngGuard;
    ApolloRateDerive(Ctx);
    ApolloLogU(Ctx, "engine: RenderLeadFrames=%lu CaptureGuardFrames=%lu StartTransportAtConnect=%lu",
              Ctx->EngLead48, Ctx->EngGuard48, Ctx->EngStartAtConnect);
    if (Ctx->SampleRate != APOLLO_RATE_DEFAULT_HZ) {
        ApolloLogU(Ctx, "engine: at the stored %lu Hz: lead %lu, guard %lu, FRAME_CFG %lu frames", Ctx->SampleRate,
                  Ctx->EngLead, Ctx->EngGuard, Ctx->FrameCfg);
    }
}

/* ------------------------------------------------------------------ */
/* Start / stop (DevLock held, PASSIVE_LEVEL)                          */
/* ------------------------------------------------------------------ */

BOOLEAN ApolloEngineRunning(_In_ PDEVICE_CONTEXT Ctx)
{
    return ReadPointerAcquire((PVOID volatile *)&Ctx->EngineTimer) != NULL;
}

_Use_decl_annotations_
NTSTATUS ApolloEngineStart(PDEVICE_CONTEXT Ctx)
{
    PEX_TIMER timer;
    LARGE_INTEGER freq;
    KIRQL irql;

    if (ApolloEngineRunning(Ctx)) {
        return STATUS_SUCCESS;
    }
    if (!Ctx->TransportRunning || Ctx->Play.Va == NULL || Ctx->Rec.Va == NULL ||
        Ctx->RingFrames == 0 || (Ctx->RingFrames & (Ctx->RingFrames - 1)) != 0 ||
        Ctx->PlayChannels == 0 || Ctx->RecChannels == 0 ||
        Ctx->PlayChannels > AUDIO_MAX_CHANNELS || Ctx->RecChannels > AUDIO_MAX_CHANNELS) {
        return STATUS_DEVICE_NOT_READY;
    }

    timer = ExAllocateTimer(ApolloEngineTick, Ctx, EX_TIMER_HIGH_RESOLUTION);
    if (timer == NULL) {
        ApolloLogU(Ctx, "engine: ExAllocateTimer failed; the 10 ms service pass keeps the ring");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    (VOID)KeQueryPerformanceCounter(&freq);
    {
        ULONG64 qpc;

        /* Before ExSetTimer: the first tick re-arms from here (EngineRearm). */
        Ctx->EngNextDue = KeQueryInterruptTimePrecise(&qpc) + ENGINE_PERIOD_100NS;
    }
    KeAcquireSpinLock(&Ctx->EngineLock, &irql);
    /* ApolloTransportStart just seeded LastRawPos/ExtPos/LastPosQpc. */
    Ctx->EngRaw = Ctx->LastRawPos;
    Ctx->EngPos = Ctx->ExtPos;
    Ctx->EngQpc = Ctx->LastPosQpc;
    Ctx->EngQpcFreq = freq.QuadPart;
    Ctx->EngZeroHead = Ctx->EngPos;
    Ctx->EngDataHead = Ctx->EngPos + Ctx->EngLead;
    Ctx->EngLastTickQpc = 0;
    Ctx->EngDeadRun = 0;
    Ctx->EngSlowRun = 0;
    InterlockedExchange(&Ctx->EngLost, 0);  /* a PASSIVE restart is the recovery from a lost link */
    Ctx->EngStarts++;
    ApolloAsioEngineStarted(Ctx);
    ApolloStreamsEngineStarted(Ctx);    /* [ACX endpoints] resync running streams */
    KeReleaseSpinLock(&Ctx->EngineLock, irql);

    InterlockedExchangePointer((PVOID volatile *)&Ctx->EngineTimer, timer);
    (VOID)ExSetTimer(timer, -ENGINE_PERIOD_100NS, 0, NULL);    /* one-shot; each tick re-arms */
    ApolloLogU(Ctx, "engine started at frame %llu (lead %lu, guard %lu)", Ctx->EngPos, Ctx->EngLead,
              Ctx->EngGuard);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID ApolloEngineStop(PDEVICE_CONTEXT Ctx)
{
    PEX_TIMER timer = (PEX_TIMER)InterlockedExchangePointer((PVOID volatile *)&Ctx->EngineTimer,
                                                            NULL);

    if (timer == NULL) {
        return;
    }
    /*
     * Cancel and wait for a callback in flight; the timer is gone afterwards.
     * ExDeleteTimer disables the object first, so a tick that re-arms meanwhile
     * gets FALSE from ExSetTimer and sets nothing ("Deleting a System-Allocated
     * Timer Object", Microsoft Learn).
     */
    (VOID)ExDeleteTimer(timer, TRUE, TRUE, NULL);
    ApolloLogU(Ctx, "engine stopped at frame %llu after %llu ticks", Ctx->EngPos, Ctx->EngTicks);
}

/*
 * TRANSPORT_START on a lost engine (DevLock held, PASSIVE). Deletes the dead
 * timer, checks that the device answers again, then restarts the engine from
 * its last good snapshot if the transport still runs, or restarts the
 * transport (which restarts the engine) if it does not. EngLost stays set,
 * and the tick stays silent, if the device still does not answer.
 */
_Use_decl_annotations_
NTSTATUS ApolloEngineRecover(PDEVICE_CONTEXT Ctx)
{
    ULONG raw, ctrl;
    NTSTATUS status;
    KIRQL irql;

    if (!ApolloEngineLost(Ctx)) {
        return STATUS_SUCCESS;
    }
    ApolloEngineStop(Ctx);
    raw = ApolloRead(Ctx, REG_AX_SAMPLE_POS);
    ctrl = ApolloRead(Ctx, REG_AX_CONTROL);
    if (raw == 0xFFFFFFFF || ctrl == 0xFFFFFFFF) {
        ApolloLogU(Ctx, "engine recover: the device still does not answer (SAMPLE_POS 0x%08x)", raw);
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    if (Ctx->TransportRunning && (ctrl & AX_CTRL_START) == AX_CTRL_START) {
        /* Same transport: continue the 64-bit position from the last good tick. */
        KeAcquireSpinLock(&Ctx->EngineLock, &irql);
        Ctx->ExtPos = Ctx->EngPos;
        Ctx->LastRawPos = Ctx->EngRaw;
        Ctx->LastPosQpc = Ctx->EngQpc;
        KeReleaseSpinLock(&Ctx->EngineLock, irql);
        ApolloPositionUpdate(Ctx, NULL);        /* adds the wraps since, from the QPC */
        RtlZeroMemory(Ctx->Play.Va, (SIZE_T)Ctx->RingFrames * Ctx->PlayChannels * 4);
        status = ApolloEngineStart(Ctx);        /* clears EngLost; clients resync */
        ApolloLogU(Ctx, "engine recover: engine restarted on the running transport (0x%08x)", status);
    } else {
        ApolloTransportStop(Ctx);
        status = ApolloTransportStart(Ctx);     /* ends with ApolloEngineStart */
        ApolloLogU(Ctx, "engine recover: transport restarted (0x%08x), AX_CONTROL was 0x%03x", status, ctrl);
    }
    return status;
}

/* ------------------------------------------------------------------ */
/* Snapshot readers                                                    */
/* ------------------------------------------------------------------ */

/*
 * DevLock held. Without the engine (transport stopped, or no timer) the old path
 * runs. Lost (also after a failed ApolloEngineRecover deleted the timer): the
 * last good snapshot, no register read.
 */
VOID ApolloPositionGet(_In_ PDEVICE_CONTEXT Ctx, _Out_ PULONGLONG Pos, _Out_ PLONGLONG Qpc,
                       _Out_ PULONG Raw)
{
    KIRQL irql;

    if (ApolloEngineRunning(Ctx) || ApolloEngineLost(Ctx)) {
        KeAcquireSpinLock(&Ctx->EngineLock, &irql);
        *Pos = Ctx->EngPos;
        *Qpc = Ctx->EngQpc;
        *Raw = Ctx->EngRaw;
        KeReleaseSpinLock(&Ctx->EngineLock, irql);
        return;
    }
    ApolloPositionUpdate(Ctx, NULL);
    *Pos = Ctx->ExtPos;
    *Qpc = Ctx->LastPosQpc;
    *Raw = Ctx->LastRawPos;
}

static ULONG QpcToUs(_In_ LONGLONG Ticks, _In_ LONGLONG Freq)
{
    LONGLONG us;

    if (Freq <= 0 || Ticks <= 0) {
        return 0;
    }
    us = Ticks * 1000000LL / Freq;
    return us > MAXULONG ? MAXULONG : (ULONG)us;
}

VOID ApolloEngineFillStats(_In_ PDEVICE_CONTEXT Ctx, _Out_ PAPOLLO_ENGINE_STATS S,
                           _In_ BOOLEAN Reset)
{
    PAPOLLO_ASIO a = &Ctx->Asio;
    KIRQL irql;

    RtlZeroMemory(S, sizeof(*S));
    S->Version = APOLLO_ENGINE_STATS_VERSION;
    S->Running = ApolloEngineRunning(Ctx) ? 1 : 0;
    S->RenderLeadFrames = Ctx->EngLead;
    S->CaptureGuardFrames = Ctx->EngGuard;
    S->ZeroAheadFrames = ENGINE_ZERO_AHEAD;
    S->EndOfBufferCount = (ULONG)Ctx->EndOfBufferCount;

    KeAcquireSpinLock(&Ctx->EngineLock, &irql);
    S->Starts = Ctx->EngStarts;
    S->Ticks = Ctx->EngTicks;
    S->Position = Ctx->EngPos;
    S->Qpc = Ctx->EngQpc;
    S->QpcFrequency = Ctx->EngQpcFreq;
    S->MaxTickIntervalUs = QpcToUs(Ctx->EngMaxIntervalQpc, Ctx->EngQpcFreq);
    S->MaxTickDurationUs = QpcToUs(Ctx->EngMaxDurationQpc, Ctx->EngQpcFreq);
    S->IntervalsOver2ms = Ctx->EngOver2ms;
    S->IntervalsOver4ms = Ctx->EngOver4ms;
    S->LateTicks = Ctx->EngLate;
    S->BusStalls = Ctx->EngBusStalls;
    S->DeadReads = Ctx->EngDeadReads;
    S->WrapFixes = Ctx->EngWrapFixes;
    S->EngineLost = ApolloEngineLost(Ctx) ? 1 : 0;
    S->AsioState = !a->Open ? APOLLO_ASIO_STATE_CLOSED :
                   (a->Active ? APOLLO_ASIO_STATE_RUNNING : APOLLO_ASIO_STATE_OPEN);
    if (a->Open) {
        /* Client-written words: informational only, the engine keeps its own state. */
        LONG d0 = ReadNoFence(&a->Shared->ClientDone[0]), d1 = ReadNoFence(&a->Shared->ClientDone[1]);

        S->AsioBufferFrames = a->N;
        S->AsioInputs = a->InCount;
        S->AsioOutputs = a->OutCount;
        S->AsioClientDone = (ULONG)((LONG)(d0 - d1) >= 0 ? d0 : d1);
        S->AsioClientSkipped = (ULONG)ReadNoFence(&a->Shared->ClientSkipped);
    }
    S->AsioSwitches = (ULONG)a->Switches;
    S->AsioUnderruns = (ULONG)a->Underruns;
    S->AsioOverruns = (ULONG)a->Overruns;
    S->AsioResyncs = a->Resyncs;
    if (Reset) {
        Ctx->EngMaxIntervalQpc = 0;
        Ctx->EngMaxDurationQpc = 0;
        Ctx->EngOver2ms = Ctx->EngOver4ms = 0;
        Ctx->EngLate = Ctx->EngBusStalls = Ctx->EngDeadReads = Ctx->EngWrapFixes = 0;
    }
    KeReleaseSpinLock(&Ctx->EngineLock, irql);
}

/* ------------------------------------------------------------------ */
/* The tick (DISPATCH_LEVEL)                                           */
/* ------------------------------------------------------------------ */

/* Zero whole playback frames [From, To), skipping frames a diagnostic writer claimed. */
static VOID EngineZero(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONGLONG From, _In_ ULONGLONG To)
{
    ULONG stride = Ctx->PlayChannels * 4, mask = Ctx->RingFrames - 1;
    ULONGLONG claim = ApolloPlayValidUntil(Ctx), f = From;

    if (f < claim) {
        f = min(claim, To);
    }
    while (f < To) {
        ULONG slot = (ULONG)(f & mask);
        ULONG n = (ULONG)min(To - f, (ULONGLONG)(Ctx->RingFrames - slot));

        RtlZeroMemory(Ctx->Play.Va + (SIZE_T)slot * stride, (SIZE_T)n * stride);
        f += n;
    }
}

/*
 * Next expiration on the fixed grid EngNextDue + k * 1 ms. A high-resolution
 * timer never fires early but may fire up to one clock tick (1 ms here) late;
 * a grid point that is already a little overdue is kept and fired at the next
 * clock tick, so no period is lost. Only a stall longer than a whole period
 * starts a new grid. Only the tick and ApolloEngineStart (before the timer is
 * set) touch EngNextDue, and a one-shot re-armed at the end of the tick cannot
 * overlap itself.
 */
_IRQL_requires_(DISPATCH_LEVEL)
static VOID EngineRearm(_In_ PDEVICE_CONTEXT Ctx, _In_ PEX_TIMER Timer)
{
    ULONG64 qpc;
    ULONG64 now = KeQueryInterruptTimePrecise(&qpc);
    ULONG64 next = Ctx->EngNextDue + ENGINE_PERIOD_100NS;
    LONGLONG rel;

    if ((LONGLONG)(now - next) > ENGINE_PERIOD_100NS) {
        next = now + ENGINE_PERIOD_100NS;
    }
    rel = (LONGLONG)(next - now);
    if (rel < ENGINE_MIN_DUE_100NS) {
        rel = ENGINE_MIN_DUE_100NS;
    }
    Ctx->EngNextDue = next;
    (VOID)ExSetTimer(Timer, -rel, 0, NULL);     /* FALSE and a no-op once ExDeleteTimer ran */
}

_IRQL_requires_(DISPATCH_LEVEL)
static BOOLEAN EngineTickBody(_In_ PDEVICE_CONTEXT ctx);

_Function_class_(EXT_CALLBACK)
_IRQL_requires_(DISPATCH_LEVEL)
_IRQL_requires_same_
static VOID ApolloEngineTick(_In_ PEX_TIMER Timer, _In_opt_ PVOID Context)
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;

    if (ctx != NULL && EngineTickBody(ctx)) {
        EngineRearm(ctx, Timer);
    }
}

/* One tick. FALSE: the device was declared lost, do not re-arm (ApolloEngineStop deletes the timer). */
_IRQL_requires_(DISPATCH_LEVEL)
static BOOLEAN EngineTickBody(_In_ PDEVICE_CONTEXT ctx)
{
    LARGE_INTEGER q0, q1, q2;
    ULONG raw, delta, frames, notes;
    ULONGLONG target, head;
    BOOLEAN slow;
    APOLLO_NOTIFY note[APOLLO_NOTIFY_MAX];  /* [ACX endpoints] sent after the lock */

    /* Lost: no MMIO at all from here until a PASSIVE restart (ApolloEngineStart) or PnP. */
    if (ApolloEngineLost(ctx)) {
        return FALSE;
    }
    /* Gate closed or transport stopping: idle ticks, as the periodic timer had (ApolloEngineStop ends them). */
    if (ApolloGateClosed(ctx) || !ctx->TransportRunning) {
        return TRUE;
    }
    frames = ctx->RingFrames;

    q0 = KeQueryPerformanceCounter(NULL);
    raw = ApolloRead(ctx, REG_AX_SAMPLE_POS);
    q1 = KeQueryPerformanceCounter(NULL);

    KeAcquireSpinLockAtDpcLevel(&ctx->EngineLock);
    ctx->EngTicks++;
    if (ctx->EngLastTickQpc != 0) {
        LONGLONG gap = q0.QuadPart - ctx->EngLastTickQpc;

        if (gap > ctx->EngMaxIntervalQpc) {
            ctx->EngMaxIntervalQpc = gap;
        }
        if (gap > ctx->EngQpcFreq / 500) {
            ctx->EngOver2ms++;
        }
        if (gap > ctx->EngQpcFreq / 250) {
            ctx->EngOver4ms++;
        }
    }
    ctx->EngLastTickQpc = q0.QuadPart;

    /*
     * A device that stops answering without a surprise removal makes every read
     * wait for the PCIe completion timeout, and an overdue periodic timer refires
     * back to back: at DISPATCH that is a hang (0x133). Give up on the device
     * after one slow all-ones read (a completion timeout), two slow reads in a
     * row, or a run of all-ones reads. One slow read with data is tolerated: an
     * SMI or a long ISR on this CPU looks the same.
     */
    slow = q1.QuadPart - q0.QuadPart > ctx->EngQpcFreq / 1000;
    if (slow) {
        ctx->EngBusStalls++;
        ctx->EngSlowRun++;
    } else {
        ctx->EngSlowRun = 0;
    }
    if (raw == 0xFFFFFFFF) {
        ctx->EngDeadReads++;
        ctx->EngDeadRun++;
    } else {
        ctx->EngDeadRun = 0;
    }
    if ((slow && raw == 0xFFFFFFFF) || ctx->EngSlowRun >= ENGINE_SLOW_LIMIT ||
        ctx->EngDeadRun >= ENGINE_DEAD_LIMIT) {
        ULONG us = QpcToUs(q1.QuadPart - q0.QuadPart, ctx->EngQpcFreq);
        ULONG dead = ctx->EngDeadRun, slowRun = ctx->EngSlowRun;

        InterlockedExchange(&ctx->EngLost, 1);
        /*
         * Nothing zeroes ahead of the hardware from now on. If it is in fact still
         * running (a false positive), it would loop the last 170 ms forever: silence
         * the whole ring once (host memory, no MMIO).
         */
        RtlZeroMemory(ctx->Play.Va, (SIZE_T)frames * ctx->PlayChannels * 4);
        KeReleaseSpinLockFromDpcLevel(&ctx->EngineLock);
        ApolloLogU(ctx, "engine: device not answering (SAMPLE_POS 0x%08x in %lu us, %lu dead, %lu slow "
                  "in a row): stopped all MMIO from the tick until a restart",
                  raw, us, dead, slowRun);
        return FALSE;                       /* no more wakeups; ApolloEngineStop deletes the timer */
    }
    if (raw == 0xFFFFFFFF) {
        /* Usually a surprise removal on its way. Nothing moves meanwhile. */
        KeReleaseSpinLockFromDpcLevel(&ctx->EngineLock);
        return TRUE;
    }

    /* 1. Position: extend the 8192-frame counter, adding wraps a long stall hid. */
    raw &= frames - 1;
    delta = (raw - ctx->EngRaw) & (frames - 1);
    if (ctx->EngQpcFreq > 0 && q1.QuadPart > ctx->EngQpc) {
        ULONGLONG expected = (ULONGLONG)(q1.QuadPart - ctx->EngQpc) * ctx->SampleRate /
                             (ULONGLONG)ctx->EngQpcFreq;

        if (expected > (ULONGLONG)delta + frames / 2) {
            ctx->EngPos += ((expected - delta + frames / 2) / frames) * frames;
            ctx->EngWrapFixes++;
        }
    }
    ctx->EngPos += delta;
    ctx->EngRaw = raw;
    ctx->EngQpc = q1.QuadPart;

    /* 2. Zero horizon. */
    target = ctx->EngPos + ENGINE_ZERO_AHEAD;
    if (ctx->EngZeroHead < ctx->EngPos) {
        ctx->EngZeroHead = ctx->EngPos;     /* only after a stall longer than the horizon */
    }
    if (target > ctx->EngZeroHead) {
        EngineZero(ctx, ctx->EngZeroHead, target);
        ctx->EngZeroHead = target;
    }

    /* 3. Render: frames the hardware already fetched are lost, not written late. [Rate] FRAME_CFG of the rate */
    if (ctx->EngDataHead < ctx->EngPos + ctx->FrameCfg) {
        ctx->EngLate++;
        ctx->EngDataHead = ctx->EngPos + ctx->FrameCfg;
    }
    target = ctx->EngPos + ctx->EngLead;
    if (target > ctx->EngDataHead) {
        ApolloAsioRender(ctx, ctx->EngDataHead, target);
        ApolloStreamsRender(ctx, ctx->EngDataHead, target);    /* [ACX endpoints] */
        ctx->EngDataHead = target;
    }

    /* 4. Capture: frames before SAMPLE_POS are complete; keep a small guard. */
    head = ctx->EngPos > ctx->EngGuard ? ctx->EngPos - ctx->EngGuard : 0;
    ApolloAsioCapture(ctx, head);
    ApolloStreamsCapture(ctx, head);                            /* [ACX endpoints] */
    notes = ApolloStreamsCollect(ctx, note, APOLLO_NOTIFY_MAX);

    q2 = KeQueryPerformanceCounter(NULL);
    if (q2.QuadPart - q0.QuadPart > ctx->EngMaxDurationQpc) {
        ctx->EngMaxDurationQpc = q2.QuadPart - q0.QuadPart;
    }
    KeReleaseSpinLockFromDpcLevel(&ctx->EngineLock);

    /* [ACX endpoints] packet completions, outside EngineLock (rundown protection held). */
    if (notes != 0) {
        ApolloStreamsNotify(note, notes, q1.QuadPart);
    }
    /* [G6] the combined device runs on the timeline unit's tick (agg.c; a no-op when standalone) */
    ApolloAggAfterTick(ctx);
    /* [Corr] speaker correction (corr.c): every host source is in this unit's ring by now; off = returns at once */
    ApolloCorrAfterTick(ctx);
    return TRUE;
}
