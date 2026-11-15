/*
 * device.c - PnP/power lifecycle and the x8p bring-up sequence.
 *
 * Callback placement follows docs/spec-bringup.md section 6.2:
 *   PrepareHardware      map BAR0, identify, quiesce FIRST (bus mastering is
 *                        already on under Windows), allocate DMA memory
 *   D0Entry              timer resolution, resume re-check + quiesce
 *   D0EntryPostIntr      queue the slow bring-up (up to ~50 s worst case)
 *   D0ExitPreIntr        stop service + bring-up, hardware quiesce, 100 ms
 *   ReleaseHardware      free DMA memory, unmap
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "apollo.h"
#include <wdmguid.h>

VOID ApolloFail(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_STAGE Attempted, _In_ NTSTATUS Status)
{
    Ctx->FailedStage = Attempted;
    Ctx->LastStatus = Status;
    if (Ctx->Stage != ApolloStageGone) {
        Ctx->Stage = (Status == STATUS_DEVICE_NOT_CONNECTED) ? ApolloStageGone : ApolloStageFailed;
    }
    ApolloLogU(Ctx, "bring-up failed at stage %d: 0x%08x", Attempted, Status);
}

/*
 * [Twin X G0] Bring-up allowed: an accepted model (apollo_model.h) whose bring-up this build
 * has, on a unit whose Mode is 1 or 2 (unit.c). False for refused models and observe units.
 */
static BOOLEAN ApolloSupported(_In_ PDEVICE_CONTEXT Ctx)
{
    return Ctx->Unit.Active ? TRUE : FALSE;
}

/* ------------------------------------------------------------------ */
/* PrepareHardware / ReleaseHardware                                   */
/* ------------------------------------------------------------------ */

NTSTATUS ApolloEvtPrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Raw,
                                  _In_ WDFCMRESLIST Translated)
{
    PDEVICE_CONTEXT ctx = ApolloGetContext(Device);
    ULONG i, count;
    NTSTATUS status;
    BOOLEAN haveBar = FALSE;

    UNREFERENCED_PARAMETER(Raw);

    count = WdfCmResourceListGetCount(Translated);
    for (i = 0; i < count; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = WdfCmResourceListGetDescriptor(Translated, i);

        if (d == NULL) {
            continue;
        }
        if (d->Type == CmResourceTypeMemory && !haveBar) {
            ctx->Bar0Length = d->u.Memory.Length;
            ctx->Bar0 = (PUCHAR)MmMapIoSpaceEx(d->u.Memory.Start, d->u.Memory.Length,
                                               PAGE_READWRITE | PAGE_NOCACHE);
            if (ctx->Bar0 == NULL) {
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            haveBar = TRUE;
            ApolloLogU(ctx, "BAR0 at 0x%llx, %lu bytes", d->u.Memory.Start.QuadPart, d->u.Memory.Length);
        } else if (d->Type == CmResourceTypeMemory) {
            ApolloLogU(ctx, "ignoring extra memory resource 0x%llx (%lu bytes)",
                      d->u.Memory.Start.QuadPart, d->u.Memory.Length);
        } else if (d->Type == CmResourceTypeInterrupt) {
            ctx->Msi = (d->Flags & CM_RESOURCE_INTERRUPT_MESSAGE) != 0;
        }
    }
    if (!haveBar) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (ctx->Bar0Length < APOLLO_BAR0_MIN) {
        ApolloLogU(ctx, "BAR0 is %lu bytes, need at least 0x%x", ctx->Bar0Length, APOLLO_BAR0_MIN);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    status = WdfFdoQueryForInterface(Device, &GUID_BUS_INTERFACE_STANDARD,
                                     (PINTERFACE)&ctx->BusInterface,
                                     sizeof(BUS_INTERFACE_STANDARD), 1, NULL);
    ctx->HaveBusInterface = NT_SUCCESS(status);

    /* Open the gate. From here, the first writes must be the quiesce. */
    InterlockedExchange(&ctx->Shutdown, 0);
    InterlockedExchange(&ctx->StopRequested, 0);
    RtlZeroMemory(&ctx->Unit, sizeof(ctx->Unit));   /* [Twin X G0] not active until resolved */
    ctx->OwnerRead = FALSE;                         /* [Twin X G2] read again below if observe */

    /* Identification is reads only; it gives the DSP count the quiesce needs. */
    status = ApolloIdentify(ctx);
    if (status == STATUS_DEVICE_NOT_CONNECTED) {
        return status;
    }

    /* Phase B: stop what the previous owner left, before any DMA can happen. */
    ApolloStopTransportAndIrqs(ctx);
    if (NT_SUCCESS(status)) {
        status = ApolloRingsNeutralizeAll(ctx);
        if (status == STATUS_DEVICE_NOT_CONNECTED) {
            return status;
        }
    }

    /*
     * [Twin X G0] this unit's Parameters and Mode (unit.c); nothing for a refused model.
     * [release7 review] Only after a clean quiesce: a failed ring neutralize leaves Unit zeroed
     * (not Active), so D0Entry neither resets the Stage nor queues a bring-up with nothing
     * allocated.
     */
    if (NT_SUCCESS(status)) {
        ApolloUnitResolve(ctx);
    }

    /*
     * [Twin X G2] A unit this driver does not bring up (observe, refused): what the previous
     * owner left in the notification block, reads only, logged as stale (GET_INFO Owner*).
     */
    if (NT_SUCCESS(status) && !ApolloSupported(ctx)) {
        const APOLLO_BRINGUP_PLAN *plan = ApolloPlanOf(ctx);

        ApolloReadOwnerState(ctx);
        /* [Twin X G3] and switch off the ring DMA engines UA's driver left enabled (hw.c) */
        if (plan != NULL && plan->ObserveDmaOff) {
            ApolloObserveDmaOff(ctx);
        }
    }

    if (!NT_SUCCESS(status) || ctx->Ident.Verdict != APOLLO_IDENT_ACCEPTED) {
        /* Refused (logged by ApolloIdentify): stay started but idle so apollo-diag can report it. */
        ApolloFail(ctx, ApolloStageMapped, NT_SUCCESS(status) ? STATUS_NOT_SUPPORTED : status);
        return STATUS_SUCCESS;
    }
    if (!ApolloSupported(ctx)) {
        /*
         * Observe (unit Mode 0, or no bring-up path for this model yet): identified, quiesced,
         * idle. No rings, no audio memory, no strips, no engine, no endpoints, no service
         * thread; user IOCTLs that would write are refused (ioctl.c).
         */
        ctx->Stage = ApolloStageMapped;
        ApolloLogU(ctx, "observe: identified and quiesced; nothing else runs on this unit");
        return STATUS_SUCCESS;
    }

    status = ApolloRingsAllocate(ctx);
    if (NT_SUCCESS(status)) {
        status = ApolloAudioAllocate(ctx);
    }
    if (!NT_SUCCESS(status)) {
        ApolloFail(ctx, ApolloStageMapped, status);
        return status;
    }
    /* Input strips: registry gates and transaction buffers (never fatal). */
    ApolloStripsPrepareHardware(ctx);
    ApolloPluginPrepareHardware(ctx);       /* [P0] plug-in host (never fatal) */
    ApolloCorrPrepareHardware(ctx);         /* [Corr] speaker correction: state + stored profile (never fatal) */
    /* Copy engine tunables (RenderLeadFrames, CaptureGuardFrames, StartTransportAtConnect). */
    ApolloEngineReadConfig(ctx);
    ctx->Stage = ApolloStageMapped;
    /* [ACX endpoints] only a supported, allocated unit gets Windows endpoints (circuit.c). */
    ApolloAcxPrepareHardware(ctx);
    return STATUS_SUCCESS;
}

NTSTATUS ApolloEvtReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Translated)
{
    PDEVICE_CONTEXT ctx = ApolloGetContext(Device);

    UNREFERENCED_PARAMETER(Translated);

    /* [ACX endpoints] unregister the circuits first (ACX already stopped their streams). */
    ApolloAcxReleaseHardware(ctx);
    /* D0Exit stopped it already; this is the guard before the rings go away. */
    ApolloEngineStop(ctx);
    ApolloCloseGate(ctx);
    /* D0Exit already stopped the transport and waited 100 ms. */
    ApolloAudioFree(ctx);
    ApolloPluginReleaseHardware(ctx);       /* [P0] */
    ApolloStripsReleaseHardware(ctx);
    ApolloRingsFree(ctx);
    if (ctx->Bar0 != NULL) {
        MmUnmapIoSpace(ctx->Bar0, ctx->Bar0Length);
        ctx->Bar0 = NULL;
    }
    if (ctx->HaveBusInterface && ctx->BusInterface.InterfaceDereference != NULL) {
        ctx->BusInterface.InterfaceDereference(ctx->BusInterface.Context);
        ctx->HaveBusInterface = FALSE;
    }
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Power                                                               */
/* ------------------------------------------------------------------ */

NTSTATUS ApolloEvtD0Entry(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PDEVICE_CONTEXT ctx = ApolloGetContext(Device);
    ULONG rev;

    /* A failed bring-up is retried on every D0 entry; FailedStage/LastStatus stay for diagnosis. */
    if (!ApolloSupported(ctx) || ctx->Bar0 == NULL) {
        return STATUS_SUCCESS;          /* [Twin X G2] observe/refused: not even the timer resolution */
    }
    /* 1 ms timer resolution while started: service thread and short sleeps. */
    if (!ctx->TimerResolutionSet) {
        ExSetTimerResolution(10000, TRUE);
        ctx->TimerResolutionSet = TRUE;
    }
    InterlockedExchange(&ctx->StopRequested, 0);

    if (PreviousState != WdfPowerDeviceD3Final) {
        /* Resume: same unit, same firmware? Then quiesce again (cheap). */
        rev = ApolloRead(ctx, REG_FPGA_REV);
        if (rev == 0xFFFFFFFF) {
            ApolloFail(ctx, ApolloStageMapped, STATUS_DEVICE_NOT_CONNECTED);
            return STATUS_SUCCESS;      /* hot-plug will remove it */
        }
        if (rev != ctx->FpgaRev) {
            ApolloFail(ctx, ApolloStageMapped, STATUS_DEVICE_HARDWARE_ERROR);
            return STATUS_SUCCESS;
        }
        ApolloStopTransportAndIrqs(ctx);
        (VOID)ApolloRingsNeutralizeAll(ctx);
    }
    ctx->Stage = ApolloStageMapped;
    return STATUS_SUCCESS;
}

NTSTATUS ApolloEvtD0EntryPostInterruptsEnabled(_In_ WDFDEVICE Device,
                                               _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PDEVICE_CONTEXT ctx = ApolloGetContext(Device);

    UNREFERENCED_PARAMETER(PreviousState);
    if (ApolloSupported(ctx) && ctx->Stage == ApolloStageMapped) {
        WdfWorkItemEnqueue(ctx->BringupWork);
    }
    return STATUS_SUCCESS;
}

NTSTATUS ApolloEvtD0ExitPreInterruptsDisabled(_In_ WDFDEVICE Device,
                                              _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    PDEVICE_CONTEXT ctx = ApolloGetContext(Device);

    InterlockedExchange(&ctx->StopRequested, 1);
    WdfWorkItemFlush(ctx->BringupWork);
    /* [G6] out of the combined device before anything stops (after the flush: a bring-up may just have joined) */
    ApolloAggLeave(ctx);
    ApolloServiceStop(ctx);
    /*
     * Release the strip modules we uploaded and wait for the DSP to take that, before the
     * hardware quiesce resyncs the rings (docs/STRIPS.md 5.4). Takes StripLock, then DevLock.
     * [Warm] Or leave Live strips running for the next warm start to adopt (docs/STRIPS.md 12):
     * the plug-in chains then come off with their plain strips SYNCHed again.
     */
    if (ApolloSupported(ctx)) {
        BOOLEAN keep = ApolloStripsExitKeep(ctx);

        ApolloPluginQuiesce(ctx, "D0 exit", keep);  /* [P0] plug-in chains stopped, their modules released first */
        ApolloStripsExit(ctx, keep);
    }

    WdfWaitLockAcquire(ctx->DevLock, NULL);
    /* Unconditionally: after a surprise removal the gate is closed and the quiesce skipped. */
    ApolloEngineStop(ctx);
    if (!ApolloGateClosed(ctx) && ctx->Bar0 != NULL && ApolloSupported(ctx)) {
        ApolloHwQuiesce(ctx);
        if (TargetState == WdfPowerDeviceD3Final) {
            ApolloWrite(ctx, REG_TRANSPORT, 0);
        }
    }
    if (ApolloSupported(ctx) && ctx->Stage != ApolloStageGone) {
        ctx->Stage = ApolloStageMapped;
    }
    WdfWaitLockRelease(ctx->DevLock);

    /* The firmware may keep DMAing briefly after the stop (IOMMU, R6). */
    ApolloSleepMs(100);
    return STATUS_SUCCESS;
}

NTSTATUS ApolloEvtD0Exit(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    PDEVICE_CONTEXT ctx = ApolloGetContext(Device);

    if (TargetState == WdfPowerDeviceD3Final) {
        ApolloCloseGate(ctx);
    }
    if (ctx->TimerResolutionSet) {
        ExSetTimerResolution(0, FALSE);
        ctx->TimerResolutionSet = FALSE;
    }
    return STATUS_SUCCESS;
}

VOID ApolloEvtSurpriseRemoval(_In_ WDFDEVICE Device)
{
    PDEVICE_CONTEXT ctx = ApolloGetContext(Device);

    /* Gate first: reads on a dead Thunderbolt link can stall. */
    ApolloCloseGate(ctx);
    InterlockedExchange(&ctx->StopRequested, 1);
    ctx->Stage = ApolloStageGone;
    ctx->Connected = FALSE;
    ctx->TransportRunning = FALSE;
    KeSetEvent(&ctx->ServiceStop, IO_NO_INCREMENT, FALSE);
    ApolloLogU(ctx, "surprise removal");
}

/* ------------------------------------------------------------------ */
/* [Twin X G3] Bring-up, AudioExtension path (docs/spec-twinx.md 2)    */
/* ------------------------------------------------------------------ */

/*
 * The Twin X Gen 1, step by step as the spec's table: D, E1-E6 (cold) or F (warm), the boot
 * transport E7-E13 (cold; warm only with WarmBootTransport=1, else F9), then the audio connect
 * (G5: ACEFACE unless the early one answered, descriptors, ring connect + finalize consumed, rate
 * ack, model-safe batch). The transport start follows in the work item. A step that fails stops
 * the unit through ApolloAudioExtAbort (transport stopped, rings resynced, idle) and names itself
 * in the log and in GET_INFO BringupFailStep. Never firmware, DSP_RESET or shutdown steps.
 */
static NTSTATUS ApolloBringupAudioExt(_In_ PDEVICE_CONTEXT Ctx, _In_ const APOLLO_BRINGUP_PLAN *Plan)
{
    NTSTATUS status;
    ULONG d;
    BOOLEAN warmBoot, boot;

    /* Phase D: did the previous owner (or our own last session) leave a running mixer DSP? */
    Ctx->WarmStart = ApolloMixerDspAlive(Ctx);
    warmBoot = Plan->WarmBootTransport && ApolloUnitParam(Ctx, APOLLO_PARAM_WARM_BOOT_TRANSPORT) != 0;
    boot = Plan->BootTransport && (!Ctx->WarmStart || warmBoot);
    /* [G4] the steps this start runs: the others are n/a (GET_INFO BringupApplies), not "not passed" */
    Ctx->BringupApplies = ApolloPlanBstepApplies(Plan, Ctx->WarmStart, boot, FALSE);
    ApolloLogU(Ctx, "%s start (%s, AudioExtension path, docs/spec-twinx.md 2): boot transport %s", Ctx->WarmStart ?
              "warm" : "cold", Plan->Name, boot ? (Ctx->WarmStart ? "yes (WarmBootTransport=1)" : "yes") :
              "no (warm start, WarmBootTransport=0: G5 runs the full ACEFACE with the transport stopped)");

    d = ApolloRead(Ctx, REG_FPGA_REV);
    if (d != Ctx->FpgaRev) {
        /* Not the unit we identified, or gone: nothing is written to it. */
        Ctx->BringupFailStep = APOLLO_BSTEP_E1_PROGRAM;
        ApolloLogU(Ctx, "G3 bring-up FAILED at step 1 (%s): FPGA_REV reads 0x%08lx, identified 0x%08lx; nothing "
                  "written", ApolloBstepName(APOLLO_BSTEP_E1_PROGRAM), d, Ctx->FpgaRev);
        return (d == 0xFFFFFFFF) ? STATUS_DEVICE_NOT_CONNECTED : STATUS_DEVICE_HARDWARE_ERROR;
    }

    if (!Ctx->WarmStart) {
        /* E1 ProgramRegisters: TRANSPORT 0, every strobe pulsed once, global enable on */
        ApolloWrite(Ctx, REG_TRANSPORT, 0);
        ApolloResetDmaAllEngines(Ctx);
        d = ApolloRead(Ctx, REG_DMA_CTRL);
        ApolloLogU(Ctx, "E1: DMA_CTRL 0x%08lx after the 8-strobe pulse (%s)", d,
                  (d & DMA_CTRL_STROBES) ? "A STROBE IS STILL SET" : "no strobe left set");
        ApolloCheckDsps(Ctx, "early");
        if (d != 0xFFFFFFFF && (d & DMA_CTRL_STROBES) == 0) {
            ApolloStepPassed(Ctx, APOLLO_BSTEP_E1_PROGRAM);
        }
        /* E2 */
        Ctx->DspsConnected = FALSE;
        Ctx->AcefaceDone = FALSE;
        /* E3: only when ext_caps[7:4] says the DSPs report ready (the Twin X: 1); a timeout is logged */
        status = ApolloWaitDspsReady(Ctx);
        if (!NT_SUCCESS(status)) {
            return status;              /* gone or stopping */
        }
        if (Ctx->DspReadyTimeouts == 0) {
            ApolloStepPassed(Ctx, APOLLO_BSTEP_E3_READY);
        }
        ApolloStripsProbeBoot(Ctx);     /* boot words, logged, before the boot transport may change them */
        /* E4: rings, then exactly one enable per DSP (Duo 0x6) with its strobe pulsed */
        status = ApolloRingsInit(Ctx);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        if ((Ctx->DmaCtrlCache & (DMA_CTRL_ENABLES | DMA_CTRL_STROBES)) == ApolloPlanDmaEnables(Ctx->NumDsps)) {
            ApolloStepPassed(Ctx, APOLLO_BSTEP_E4_RINGS);
        }
        /* E5 */
        ApolloWrite(Ctx, REG_MIXER_SEQ_WR, 0);
        /* E6: SG before ANY transport start, the boot transport included (IOMMU) */
        ApolloAudioProgramSg(Ctx);
    } else {
        ApolloStripsProbeBoot(Ctx);
        /* F4 */
        ApolloCheckDsps(Ctx, "early");
        Ctx->AcefaceDone = FALSE;       /* F5: ACEFACE does not survive an OS change */
        /* F7: the previous OS's SG entries point at memory we do not own. */
        ApolloAudioProgramSg(Ctx);
        /* F8 */
        status = ApolloRingsInit(Ctx);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        if ((Ctx->DmaCtrlCache & (DMA_CTRL_ENABLES | DMA_CTRL_STROBES)) == ApolloPlanDmaEnables(Ctx->NumDsps)) {
            ApolloStepPassed(Ctx, APOLLO_BSTEP_E4_RINGS);
        }
    }
    if ((Ctx->DmaCtrlCache & DMA_CTRL_STROBES) == 0 && (Ctx->DmaCtrlCache & DMA_CTRL_GLOBAL) != 0) {
        ApolloStepPassed(Ctx, APOLLO_BSTEP_E6_SG);
    }
    Ctx->Stage = ApolloStageRingsReady;

    if (boot) {
        /* E7, E8, DMA pulse + ring pages, E10, E12, E13 (audio.c) */
        status = ApolloBootTransport(Ctx);
        /* [G4] E10 runs only when E8 did not answer */
        Ctx->BringupApplies = ApolloPlanBstepApplies(Plan, Ctx->WarmStart, boot,
                                                     (Ctx->BringupSteps & (1u << APOLLO_BSTEP_E8_ACEFACE)) != 0);
        if (!NT_SUCCESS(status)) {
            if (status == STATUS_DEVICE_NOT_CONNECTED || status == STATUS_CANCELLED) {
                return status;
            }
            return ApolloAudioExtAbort(Ctx, APOLLO_BSTEP_E7_BOOT_START, ApolloStageAcefaceDone, status,
                                       "Power-cycle the Twin X.");
        }
    } else {
        /* F9 = E13: nothing of ours started it; the quiesce already wrote 0 */
        ApolloWrite(Ctx, REG_AX_CONTROL, AX_CTRL_STOP);
        Ctx->Connected = FALSE;
        ApolloLogU(Ctx, "F9: transport stopped (AX_CONTROL 0x%03lx); no boot transport on this warm start",
                  ApolloRead(Ctx, REG_AX_CONTROL));
    }

    ApolloCheckDsps(Ctx, "final");

    /* G5: ACEFACE (unless the early one answered), descriptors, ring connect, rate ack, batch */
    return ApolloAudioConnect(Ctx);
}

/* ------------------------------------------------------------------ */
/* Bring-up (spec-bringup 3.4-3.7, x8p path)                           */
/* ------------------------------------------------------------------ */

static NTSTATUS ApolloBringup(_In_ PDEVICE_CONTEXT Ctx)
{
    NTSTATUS status;
    ULONG i;
    BOOLEAN alive = FALSE;
    const APOLLO_BRINGUP_PLAN *plan = ApolloPlanOf(Ctx);

    /* [Twin X G3] Every bring-up starts with a clean step record (the x8p path records none). */
    Ctx->BringupSteps = 0;
    Ctx->BringupApplies = 0;
    Ctx->BringupFailStep = APOLLO_BSTEP_NONE;
    Ctx->FailAt = ApolloStageNone;
    Ctx->NotifClockSrcsLive = 0;
    if (plan != NULL && plan->AudioExtension) {
        return ApolloBringupAudioExt(Ctx, plan);    /* the Twin X Gen 1; below is the x8p, unchanged */
    }

    /* Phase D: did the previous owner leave a running mixer DSP? */
    Ctx->WarmStart = ApolloMixerDspAlive(Ctx);
    ApolloLogU(Ctx, "%s start", Ctx->WarmStart ? "warm" : "cold");

    if (ApolloRead(Ctx, REG_FPGA_REV) != Ctx->FpgaRev) {
        return STATUS_DEVICE_HARDWARE_ERROR;
    }

    if (!Ctx->WarmStart) {
        /* E1 ProgramRegisters */
        ApolloWrite(Ctx, REG_TRANSPORT, 0);
        ApolloResetDmaAllEngines(Ctx);
        ApolloCheckDsps(Ctx, "early");
        /* E2 */
        Ctx->DspsConnected = FALSE;
        Ctx->AcefaceDone = FALSE;
        /* E3 */
        status = ApolloWaitDspsReady(Ctx);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        ApolloStripsProbeBoot(Ctx);      /* before E11 turns the boot words into connect counts */
        /* E4 */
        status = ApolloRingsInit(Ctx);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        /* E5 */
        ApolloWrite(Ctx, REG_MIXER_SEQ_WR, 0);
        /* E6: SG before any transport start, or the FPGA DMAs to garbage. */
        ApolloAudioProgramSg(Ctx);
    } else {
        ApolloStripsProbeBoot(Ctx);
        /* F4 */
        ApolloCheckDsps(Ctx, "early");
        Ctx->AcefaceDone = FALSE;       /* F5: ACEFACE does not survive an OS change */
        /* F7: the previous OS's SG entries point at memory we do not own. */
        ApolloAudioProgramSg(Ctx);
        /* F8 */
        status = ApolloRingsInit(Ctx);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }
    Ctx->Stage = ApolloStageRingsReady;

    if (!Ctx->WarmStart) {
        /*
         * E11: x8p ring connect at cold probe only, before ACEFACE (the
         * verified Linux order). The warm path keeps the previous owner's
         * DSP state, including its input strips.
         */
        status = ApolloDspConnectAll(Ctx);
        if (status == STATUS_DEVICE_NOT_CONNECTED) {
            return status;
        }
        if (NT_SUCCESS(status)) {
            Ctx->Stage = ApolloStageRingConnected;
        } else {
            ApolloLogU(Ctx, "ring connect failed 0x%08x (continuing to ACEFACE)", status);
        }

        /* E12: readback liveness, not fatal either way. */
        ApolloWrite(Ctx, REG_MIXER_RB_STATUS, 0);
        for (i = 0; i < 50 && !alive; i++) {
            ApolloSleepMs(11);
            alive = (ApolloRead(Ctx, REG_MIXER_RB_STATUS) == 1);
        }
        ApolloLogU(Ctx, "mixer readback %s after ring connect", alive ? "alive" : "silent");
    }

    ApolloCheckDsps(Ctx, "final");

    /* Phase G: audio-extension connect (ACEFACE, reads, rate ack, mixer init). */
    return ApolloAudioConnect(Ctx);
}

VOID ApolloEvtBringupWork(_In_ WDFWORKITEM WorkItem)
{
    PDEVICE_CONTEXT ctx = ApolloGetContext((WDFDEVICE)WdfWorkItemGetParentObject(WorkItem));
    const APOLLO_BRINGUP_PLAN *plan = ApolloPlanOf(ctx);
    BOOLEAN ext = (plan != NULL && plan->AudioExtension) ? TRUE : FALSE;  /* [Twin X G3] */
    NTSTATUS status;
    APOLLO_STAGE reached;

    WdfWaitLockAcquire(ctx->DevLock, NULL);
    status = ApolloBringup(ctx);
    reached = ctx->Stage;
    if (!NT_SUCCESS(status)) {
        /* [Twin X G3] its failures name their stage (FailAt); the x8p's are Stage + 1, as before */
        ApolloFail(ctx, (ctx->FailAt != ApolloStageNone) ? ctx->FailAt : (APOLLO_STAGE)(reached + 1), status);
    } else {
        NTSTATUS ts;

        ApolloLogU(ctx, "bring-up complete: connected");
        ApolloStripsProbeLayout(ctx);
        /*
         * The transport starts once here and keeps running while the unit is
         * powered (PLAN.md streaming design): endpoints and ASIO open and close
         * without touching it. Verified by hand on Nov 14: 47,999.7 frames/s.
         * The copy engine starts with it (ApolloTransportStart).
         * StartTransportAtConnect=0 leaves it to the TRANSPORT_START IOCTL.
         * [Rate] The stored rate first (rate.c): at 48 kHz nothing (the path as before); at any other
         * rate the clock is moved with the transport stopped, as a runtime change does.
         */
        ApolloRateBringup(ctx);
        if (ctx->EngStartAtConnect) {
            ts = ApolloTransportStart(ctx);
            ApolloLogU(ctx, "transport auto-start: 0x%08x", ts);
            if (ext && !NT_SUCCESS(ts)) {
                /* [Twin X G3] Mode 1 is the transport: a start that fails stops the unit, named. */
                status = ApolloAudioExtAbort(ctx, (ctx->BringupSteps & (1u << APOLLO_BSTEP_XPORT_START)) ?
                                             APOLLO_BSTEP_SAMPLE_POS : APOLLO_BSTEP_XPORT_START,
                                             ApolloStageConnected, ts, "Power-cycle the Twin X.");
                ApolloFail(ctx, ctx->FailAt, status);
            }
        } else {
            ApolloLogU(ctx, "transport left stopped (StartTransportAtConnect=0)");
        }
        /* Input strips (strips.c): the service pass starts the first pass 500 ms later. */
        ApolloStripsArm(ctx);
        if (ext && NT_SUCCESS(status)) {
            ULONG k, missing = 0;

            /* [G4] only the steps this path runs count (BringupApplies); the rest are n/a */
            for (k = 1; k < APOLLO_BSTEP_COUNT; k++) {
                if ((ctx->BringupApplies & (1u << k)) != 0 && (ctx->BringupSteps & (1u << k)) == 0) {
                    missing++;
                }
            }
            ApolloLogU(ctx, "G3 bring-up done (%s start): steps 0x%05lx of 0x%05lx, %lu not passed%s; play %lu rec %lu, "
                      "rate info 0x%08lx, clock sources 0x%08lx, DMA_CTRL 0x%08lx", ctx->WarmStart ? "warm" : "cold",
                      ctx->BringupSteps, ctx->BringupApplies, missing,
                      missing ? " (apollo-diag --unit twinx info lists them)" : "",
                      ctx->PlayChannels, ctx->RecChannels, ctx->NotifRateInfo, ctx->NotifClockSrcsLive,
                      ctx->DmaCtrlCache);
        }
    }
    WdfWaitLockRelease(ctx->DevLock);

    /*
     * [G4 review] A failed Twin X bring-up stopped the unit on purpose (ApolloAudioExtAbort: no
     * transport, no service thread), but its Windows endpoints were added at PrepareHardware: with
     * MON L/R as the default device Windows would keep sending audio to a dead endpoint. Remove the
     * circuits now (ACX dynamic removal, PASSIVE, DevLock released: the stream callbacks take
     * EngineLock); the next start (PrepareHardware) adds them again. The x8p keeps its endpoints
     * after a failure, as before (its service thread still runs and TRANSPORT_START retries).
     */
    /*
     * Not while D0 exit waits on this work item (StopRequested): a cancelled bring-up must not call
     * into ACX while the WDF power state machine sits in our D0 exit callback; ReleaseHardware
     * removes the circuits on the way down anyway (G5+G6 review item 2).
     */
    if (ext && ctx->Stage == ApolloStageFailed && InterlockedCompareExchange(&ctx->StopRequested, 0, 0) == 0) {
        ApolloLogU(ctx, "bring-up failed: removing this unit's Windows endpoints until it starts again");
        ApolloAcxReleaseHardware(ctx);
        ctx->AcxRemovedAfterFail = TRUE;
    } else if (ext && ctx->AcxRemovedAfterFail && ctx->Stage == ApolloStageConnected) {
        /* a later bring-up of the same start (resume, D0 entry) succeeded: put them back */
        ctx->AcxRemovedAfterFail = FALSE;
        ApolloLogU(ctx, "bring-up done after a failure: adding this unit's Windows endpoints again");
        ApolloAcxPrepareHardware(ctx);
    }
    /* [Rate] the Windows endpoints advertise the rate the unit came up at (a no-op when it is theirs) */
    if (ctx->Stage == ApolloStageConnected && InterlockedCompareExchange(&ctx->StopRequested, 0, 0) == 0) {
        ApolloAcxSetRate(ctx);
    }
    /* [G6] a running Mode 2 unit joins the combined device (agg.c checks Mode, transport, engine) */
    if (ctx->Stage == ApolloStageConnected && ctx->TransportRunning) {
        ApolloAggJoin(ctx);
    }

    /*
     * The DSP halts without host servicing. Start the service thread once
     * the rings are up, even if ACEFACE failed (a later TRANSPORT_START
     * retries the connect, and the pass is a no-op until connected).
     * [Twin X G3] Not after a failure of the Twin X path: it stopped the unit on purpose.
     */
    if (reached >= ApolloStageRingsReady && reached <= ApolloStageConnected &&
        ctx->Stage != ApolloStageGone &&
        !(plan != NULL && !plan->ServiceAfterFailure && ctx->Stage == ApolloStageFailed)) {
        (VOID)ApolloServiceStart(ctx);
    }
}
