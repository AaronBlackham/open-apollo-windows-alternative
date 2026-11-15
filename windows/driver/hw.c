/*
 * hw.c - timing, identity, interrupt vectors, DMA_CTRL, liveness checks.
 *
 * Sequences follow docs/spec-bringup.md (sections 1.2, 2.4, 3.2, 3.4, 3.5).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "apollo.h"

VOID ApolloSleepMs(_In_ ULONG Ms)
{
    LARGE_INTEGER t;

    t.QuadPart = -10000LL * (LONGLONG)Ms;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

VOID ApolloDelayUs(_In_ ULONG Us)
{
    if (Us <= 100) {
        KeStallExecutionProcessor(Us);
    } else {
        ApolloSleepMs((Us + 999) / 1000);
    }
}

ULONG ApolloDspWindow(_In_ ULONG Dsp)
{
    return Dsp * 0x800 + (Dsp > 3 ? 0x2000 : 0);
}

/* ------------------------------------------------------------------ */
/* Identity (spec-bringup 1.2, spec-twinx 1; the logic is apollo_model.h) */
/* ------------------------------------------------------------------ */

C_ASSERT(APOLLO_X8P_TESTED_FW == APOLLO_TESTED_FW_RELEASE);
C_ASSERT(APOLLO_X8P_TESTED_FPGA == APOLLO_TESTED_FPGA_REV);
C_ASSERT(APOLLO_DEVTYPE_X8P == APOLLO_TYPE_X8P);

static ULONG ApolloReadSubsystemId(_In_ PDEVICE_CONTEXT Ctx)
{
    USHORT sub = 0;

    if (Ctx->HaveBusInterface && Ctx->BusInterface.GetBusData != NULL) {
        Ctx->BusInterface.GetBusData(Ctx->BusInterface.Context, PCI_WHICHSPACE_CONFIG,
                                     &sub, 0x2E, sizeof(sub));
    }
    return sub;
}

/*
 * Reads only. Returns STATUS_DEVICE_NOT_CONNECTED (all ones), STATUS_NOT_SUPPORTED (v1
 * firmware, more than 8 DSPs: not an Apollo we can even quiesce by DSP), else STATUS_SUCCESS
 * with Ctx->Ident saying whether the model is one this driver runs (APOLLO_IDENT_ACCEPTED) or
 * one it refuses: a refused unit is still quiesced, then stays started and idle.
 */
NTSTATUS ApolloIdentify(_In_ PDEVICE_CONTEXT Ctx)
{
    APOLLO_IDENT_IN in;
    const APOLLO_IDENT *id = &Ctx->Ident;
    BOOLEAN accepted;
    ULONG i;

    RtlZeroMemory(&in, sizeof(in));
    Ctx->FpgaRev = ApolloRead(Ctx, REG_FPGA_REV);
    Ctx->SubsystemId = ApolloReadSubsystemId(Ctx);
    in.FpgaRev = Ctx->FpgaRev;
    in.SubsystemId = Ctx->SubsystemId;

    /* ext_caps only on v2 firmware; the rest only once the DSP count is sane (as before). */
    if (Ctx->FpgaRev != 0xFFFFFFFF && (Ctx->FpgaRev & 0x80000000u) != 0) {
        Ctx->ExtCaps = ApolloRead(Ctx, REG_EXT_CAPS);
        in.ExtCaps = Ctx->ExtCaps;
        if (Ctx->ExtCaps != 0xFFFFFFFF && ((Ctx->ExtCaps >> 8) & 0xFF) <= APOLLO_MAX_DSPS) {
            for (i = 0; i < 4; i++) {
                ULONG w = ApolloRead(Ctx, REG_SERIAL0 + i * 4);

                RtlCopyMemory(&in.SerialRaw[i * 4], &w, sizeof(w));
            }
            Ctx->HwUidLo = ApolloRead(Ctx, REG_HW_UID_LO);
            Ctx->HwUidHi = ApolloRead(Ctx, REG_HW_UID_HI);
            Ctx->FwRelease = ApolloRead(Ctx, REG_FW_RELEASE);
            in.HwUidLo = Ctx->HwUidLo;
            in.HwUidHi = Ctx->HwUidHi;
            in.FwRelease = Ctx->FwRelease;
        }
    }

    ApolloIdentResolve(&in, &Ctx->Ident);
    Ctx->FwV2 = id->FwV2 ? TRUE : FALSE;
    Ctx->NumDsps = id->NumDsps;
    Ctx->DeviceType = id->DeviceType;
    Ctx->PublicType = id->PublicType;
    Ctx->FwUntested = id->FwUntested ? TRUE : FALSE;
    RtlCopyMemory(Ctx->Serial, id->Serial, sizeof(Ctx->Serial));

    if (id->Verdict == APOLLO_IDENT_OFFLINE) {
        ApolloLogU(Ctx, "%s", id->Why);
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    if (id->Verdict == APOLLO_IDENT_NOT_APOLLO) {
        ApolloLogU(Ctx, "FPGA_REV 0x%08x ext_caps 0x%08x (%lu DSPs): %s", Ctx->FpgaRev, Ctx->ExtCaps,
                  Ctx->NumDsps, id->Why);
        return STATUS_NOT_SUPPORTED;
    }
    Ctx->Irq64 = id->Irq64 ? TRUE : FALSE;
    accepted = (id->Verdict == APOLLO_IDENT_ACCEPTED);

    ApolloLogU(Ctx, "FPGA 0x%08x subsys 0x%04x serial '%s' type 0x%02x public 0x%02x DSPs %lu "
              "fw %lu.%lu.%lu irq64 %d",
              Ctx->FpgaRev, Ctx->SubsystemId, Ctx->Serial, Ctx->DeviceType, Ctx->PublicType,
              Ctx->NumDsps, Ctx->FwRelease >> 24, (Ctx->FwRelease >> 16) & 0xFF,
              Ctx->FwRelease & 0xFFFF, Ctx->Irq64);
    ApolloLogU(Ctx, "previous owner left 0x01A4=0x%08x 0x01AC=0x%08x",
              ApolloRead(Ctx, REG_DEV_INFO_1A4), ApolloRead(Ctx, REG_DEV_INFO_1AC));

    if (!Ctx->Irq64) {
        ApolloLogU(Ctx, "ext_caps 0x%08x: no audio extension vectors", Ctx->ExtCaps);
    }
    ApolloLogU(Ctx, "model: %s (type 0x%02x, public 0x%02x, family %lu, %lu DSPs): %s%s", id->Name,
              Ctx->DeviceType, Ctx->PublicType, id->Family, Ctx->NumDsps,
              accepted ? "supported" : "REFUSED: ", id->Why);
    ApolloLogU(Ctx, "serial '%s': %lu characters, digits 5-8 %s; HW_UID 0x%08x%08x", Ctx->Serial,
              id->SerialLen, id->SerialDigitsOk ? "ok" : "NOT DIGITS", Ctx->HwUidHi, Ctx->HwUidLo);
    if (accepted && Ctx->FwUntested) {
        ApolloLogU(Ctx, "WARNING: firmware 0x%08x / FPGA 0x%08x are not a tested pair for the %s "
                  "(flagged, not refused; keep UA firmware updates off)",
                  Ctx->FwRelease, Ctx->FpgaRev, id->Name);
    }
    return STATUS_SUCCESS;
}

/*
 * [Twin X G2] A unit the driver does not bring up (observe, refused), right after its quiesce:
 * what the previous owner (UA's driver) left in the notification block, so gate G2 can compare
 * it with spec-twinx Appendix A. Reads only. These values are STALE: nothing of ours runs on the
 * unit, so they are UA's last rate, clock-source mask and IO descriptors, not the current state.
 * GET_INFO carries the summary (Owner*); the log carries every descriptor word.
 */
VOID ApolloReadOwnerState(_In_ PDEVICE_CONTEXT Ctx)
{
    static const struct {
        PCSTR Name;
        ULONG Base;
    } desc[2] = { { "in ", REG_IO_DESC_IN }, { "out", REG_IO_DESC_OUT } };
    ULONG d, i, k, w[8];

    if (ApolloGateClosed(Ctx) || Ctx->Bar0 == NULL || Ctx->Bar0Length < APOLLO_BAR0_MIN) {
        return;
    }
    /*
     * [G2 review] A unit that stops answering reads all ones, and each read on a dead link may
     * wait for the PCIe completion timeout: stop at the first sign of it instead of 150 reads.
     */
    Ctx->OwnerRateInfo = ApolloRead(Ctx, REG_NOTIF_RATE_INFO);
    if (Ctx->OwnerRateInfo == 0xFFFFFFFF && ApolloRead(Ctx, REG_FPGA_REV) == 0xFFFFFFFF) {
        ApolloLogU(Ctx, "STALE state not read: the unit reads all ones (gone?)");
        return;
    }
    Ctx->OwnerClockSrcs = ApolloRead(Ctx, REG_NOTIF_CLOCK_SRCS);
    Ctx->OwnerDescInVersion = ApolloRead(Ctx, REG_IO_DESC_IN);
    Ctx->OwnerDescInCount = ApolloRead(Ctx, REG_IO_DESC_IN + 3 * 4);
    Ctx->OwnerDescOutVersion = ApolloRead(Ctx, REG_IO_DESC_OUT);
    Ctx->OwnerDescOutCount = ApolloRead(Ctx, REG_IO_DESC_OUT + 4 * 4);
    Ctx->OwnerRead = TRUE;

    ApolloLogU(Ctx, "STALE (as the previous owner left them; nothing of ours runs on this unit): "
               "rate info 0x%08x (index %lu, lock %lu), clock sources 0x%08x (sources 0x%04x, rates 0x%04x)",
               Ctx->OwnerRateInfo, (Ctx->OwnerRateInfo >> 8) & 0xFF, Ctx->OwnerRateInfo & 0x3,
               Ctx->OwnerClockSrcs, Ctx->OwnerClockSrcs >> 16, Ctx->OwnerClockSrcs & 0xFFFF);
    ApolloLogU(Ctx, "STALE IO descriptors: in v%lu count %lu, out v%lu count %lu",
               Ctx->OwnerDescInVersion, Ctx->OwnerDescInCount, Ctx->OwnerDescOutVersion,
               Ctx->OwnerDescOutCount);
    for (d = 0; d < 2; d++) {
        for (i = 0; i < IO_DESC_WORDS; i += 8) {
            BOOLEAN ones = FALSE;

            for (k = 0; k < 8; k++) {
                w[k] = (i + k < IO_DESC_WORDS) ? ApolloRead(Ctx, desc[d].Base + (i + k) * 4) : 0;
                ones = ones || (w[k] == 0xFFFFFFFF && i + k != 2);  /* word 2 is all ones by design */
            }
            if (ones && ApolloRead(Ctx, REG_FPGA_REV) == 0xFFFFFFFF) {
                ApolloLogU(Ctx, "STALE desc %s: the unit stopped answering at word %lu", desc[d].Name, i);
                return;
            }
            ApolloLogU(Ctx, "STALE desc %s %2lu: %08x %08x %08x %08x %08x %08x %08x %08x", desc[d].Name, i,
                       w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
        }
    }
}

/*
 * [Twin X G3] Observe, right after the quiesce (plan ObserveDmaOff: the Twin X): UA's driver
 * left the per-DSP ring DMA engines enabled with ring pages in memory that is no longer anyone's
 * (its driver unloaded). The quiesce already pulled every ring's target back to its position, so
 * nothing is fetched; this also takes the engines' enables off, so a stray DSP or FPGA ring access
 * cannot reach that memory while the unit sits in observe for hours (an IOMMU / DMA-verifier
 * fault, bugcheck 0xE6, otherwise). Only enable bits [8:1] and strobes [16:9] change: the global
 * bit and everything else stay as they are, no strobe is pulsed, nothing else is written. A later
 * bring-up re-enables per DSP with its own strobe pulse (E1 + E4 cold, F8 warm), as always.
 */
VOID ApolloObserveDmaOff(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONG d = ApolloRead(Ctx, REG_DMA_CTRL);

    if (d == 0xFFFFFFFF) {
        return;
    }
    if ((d & (DMA_CTRL_ENABLES | DMA_CTRL_STROBES)) == 0) {
        ApolloLogU(Ctx, "observe: DMA_CTRL 0x%08x: no ring DMA engine enabled", d);
        return;
    }
    ApolloWrite(Ctx, REG_DMA_CTRL, d & ~(DMA_CTRL_ENABLES | DMA_CTRL_STROBES));
    Ctx->DmaCtrlCache = ApolloRead(Ctx, REG_DMA_CTRL);
    ApolloLogU(Ctx, "observe: DMA_CTRL 0x%08x -> 0x%08x: the previous owner's ring DMA engines switched off "
              "(their ring pages are not ours)", d, Ctx->DmaCtrlCache);
}

/* ------------------------------------------------------------------ */
/* Interrupt vectors (spec-bringup 2.4)                                */
/* ------------------------------------------------------------------ */

/*
 * The interrupt lock only matters once the ISR can run. Before KMDF
 * connects the interrupt (PrepareHardware, D0Entry) nothing races us.
 * Returns whether the lock was taken so the release matches even if the
 * connected flag flips in between.
 */
static BOOLEAN ApolloIntLock(_In_ PDEVICE_CONTEXT Ctx)
{
    if (ReadAcquire(&Ctx->InterruptConnected) && Ctx->Interrupt != NULL) {
        WdfInterruptAcquireLock(Ctx->Interrupt);
        return TRUE;
    }
    return FALSE;
}

static VOID ApolloIntUnlock(_In_ PDEVICE_CONTEXT Ctx, _In_ BOOLEAN Taken)
{
    if (Taken) {
        WdfInterruptReleaseLock(Ctx->Interrupt);
    }
}

VOID ApolloMaskAll(_In_ PDEVICE_CONTEXT Ctx, _In_ BOOLEAN ClearPending)
{
    BOOLEAN taken = ApolloIntLock(Ctx);

    Ctx->ImrLo = 0;
    Ctx->ImrHi = 0;
    ApolloWrite(Ctx, REG_IMR, 0);
    if (Ctx->Irq64) {
        ApolloWrite(Ctx, REG_IMR_HI, 0);
    }
    if (ClearPending) {
        ApolloWrite(Ctx, REG_ISR, 0xFFFFFFFF);
        if (Ctx->Irq64) {
            ApolloWrite(Ctx, REG_ISR_HI, 0xFFFFFFFF);
        }
    }
    ApolloIntUnlock(Ctx, taken);
}

VOID ApolloVectorEnable(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG ImrHiBit, _In_ BOOLEAN ClearPending)
{
    BOOLEAN taken;

    if (!Ctx->Irq64) {
        return;
    }
    taken = ApolloIntLock(Ctx);
    Ctx->ImrHi |= ImrHiBit;
    if (ClearPending) {
        ApolloWrite(Ctx, REG_ISR_HI, ImrHiBit);
    }
    ApolloWrite(Ctx, REG_IMR_HI, Ctx->ImrHi);
    if (ClearPending) {
        (VOID)ApolloRead(Ctx, REG_FPGA_REV);   /* posted-write flush */
    }
    ApolloIntUnlock(Ctx, taken);
}

VOID ApolloVectorDisable(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG ImrHiBit)
{
    BOOLEAN taken;

    if (!Ctx->Irq64) {
        return;
    }
    taken = ApolloIntLock(Ctx);
    Ctx->ImrHi &= ~ImrHiBit;
    ApolloWrite(Ctx, REG_IMR_HI, Ctx->ImrHi);
    ApolloIntUnlock(Ctx, taken);
}

/* Transport start (spec-streaming 2.4 step 3): ack whatever is already pending. */
VOID ApolloClearStalePending(_In_ PDEVICE_CONTEXT Ctx)
{
    BOOLEAN taken = ApolloIntLock(Ctx);
    ULONG v = ApolloRead(Ctx, REG_ISR);

    if (v != 0 && v != 0xFFFFFFFF) {
        ApolloWrite(Ctx, REG_ISR, v);
    }
    if (Ctx->Irq64) {
        v = ApolloRead(Ctx, REG_ISR_HI);
        if (v != 0 && v != 0xFFFFFFFF) {
            ApolloWrite(Ctx, REG_ISR_HI, v);
        }
    }
    ApolloIntUnlock(Ctx, taken);
}

/* Phase B1: stop the transport and every interrupt the previous owner left. */
VOID ApolloStopTransportAndIrqs(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONG ax = ApolloRead(Ctx, REG_AX_CONTROL);

    if (ax != 0 && ax != 0xFFFFFFFF) {
        ApolloLogU(Ctx, "previous session left the transport running (AX_CONTROL 0x%08x)", ax);
    }
    ApolloWrite(Ctx, REG_AX_CONTROL, AX_CTRL_STOP);
    (VOID)ApolloRead(Ctx, REG_AX_CONTROL);
    ApolloMaskAll(Ctx, TRUE);
}

BOOLEAN ApolloEvtInterruptIsr(_In_ WDFINTERRUPT Interrupt, _In_ ULONG MessageID)
{
    PDEVICE_CONTEXT ctx = ApolloGetContext(WdfInterruptGetDevice(Interrupt));
    ULONG lo, hi = 0;

    UNREFERENCED_PARAMETER(MessageID);

    if (ApolloGateClosed(ctx)) {
        return FALSE;
    }
    lo = ApolloRead(ctx, REG_ISR);
    if (lo == 0xFFFFFFFF) {
        return FALSE;                           /* device gone */
    }
    lo &= ctx->ImrLo;
    if (ctx->Irq64) {
        hi = ApolloRead(ctx, REG_ISR_HI);
        hi = (hi == 0xFFFFFFFF) ? 0 : (hi & ctx->ImrHi);
    }
    if (lo == 0 && hi == 0) {
        return FALSE;
    }
    /* Ack only the bits we handle: unhandled DSP bits re-set and storm. */
    if (lo != 0) {
        ApolloWrite(ctx, REG_ISR, lo);
    }
    if (hi != 0) {
        ApolloWrite(ctx, REG_ISR_HI, hi);
    }
    InterlockedIncrement(&ctx->InterruptCount);
    if (hi & IMR_HI_END_OF_BUFFER) {
        InterlockedIncrement(&ctx->EndOfBufferCount);
    }
    WdfInterruptQueueDpcForIsr(Interrupt);
    return TRUE;
}

VOID ApolloEvtInterruptDpc(_In_ WDFINTERRUPT Interrupt, _In_ WDFOBJECT AssociatedObject)
{
    UNREFERENCED_PARAMETER(Interrupt);
    UNREFERENCED_PARAMETER(AssociatedObject);
    /* Phase 0: counters only. Phase 2 drives the copy engine from here. */
}

/* ------------------------------------------------------------------ */
/* DMA_CTRL (spec-bringup 3.5 E1, E4, E6; issue #46 rule)              */
/* ------------------------------------------------------------------ */

ULONG ApolloDmaEnableBits(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONG i, en = 0;

    for (i = 0; i < Ctx->NumDsps; i++) {
        if (Ctx->Cmd[i].Allocated) {
            en |= DMA_CTRL_ENABLE_DSP(i);
        }
    }
    return en;
}

ULONG ApolloDmaCtrlClean(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Value)
{
    return (Value & ~(DMA_CTRL_STROBES | DMA_CTRL_ENABLES)) | DMA_CTRL_GLOBAL |
           ApolloDmaEnableBits(Ctx);
}

/* E1 ua_reset_dma: all strobes pulsed once, global enable forced on. */
VOID ApolloResetDmaAllEngines(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONG v = ApolloRead(Ctx, REG_DMA_CTRL);
    BOOLEAN taken;

    v &= ~(DMA_CTRL_STROBES | DMA_CTRL_ENABLES);
    v |= DMA_CTRL_GLOBAL;
    ApolloWrite(Ctx, REG_DMA_CTRL, v | DMA_CTRL_STROBES);
    ApolloWrite(Ctx, REG_DMA_CTRL, v);
    (VOID)ApolloRead(Ctx, REG_DMA_CTRL);
    Ctx->DmaCtrlCache = v;

    taken = ApolloIntLock(Ctx);
    ApolloWrite(Ctx, REG_ISR, 0xFFFFFFFF);
    if (Ctx->Irq64) {
        ApolloWrite(Ctx, REG_ISR_HI, 0xFFFFFFFF);
    }
    ApolloIntUnlock(Ctx, taken);
}

VOID ApolloCheckDsps(_In_ PDEVICE_CONTEXT Ctx, _In_ PCSTR When)
{
    ULONG i;

    for (i = 0; i < Ctx->NumDsps; i++) {
        ULONG s = ApolloRead(Ctx, ApolloDspWindow(i) + DSP_WIN_STATUS);

        ApolloLogU(Ctx, "[%s] DSP %lu status 0x%08x fault bits 0x%08x", When, i, s,
                  s & DSP_STATUS_FAULT_MASK);
    }
}

/* ------------------------------------------------------------------ */
/* Phase D: is a mixer DSP left running by the previous owner?        */
/* ------------------------------------------------------------------ */

BOOLEAN ApolloMixerDspAlive(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONG seqWr = ApolloRead(Ctx, REG_MIXER_SEQ_WR);
    ULONG seqRd = ApolloRead(Ctx, REG_MIXER_SEQ_RD);
    ULONG i;

    ApolloLogU(Ctx, "liveness: SEQ_WR %lu SEQ_RD %lu MIXER 0x%08x", seqWr, seqRd,
              ApolloRead(Ctx, REG_MIXER_BASE));
    if (seqRd == 0xFFFFFFFF || !(seqRd > 0 && seqRd == seqWr)) {
        return FALSE;
    }
    ApolloWrite(Ctx, REG_MIXER_RB_STATUS, 0);
    for (i = 0; i < 30; i++) {
        ApolloSleepMs(11);
        if (ApolloRead(Ctx, REG_MIXER_RB_STATUS) == 1) {
            return TRUE;
        }
    }
    ApolloLogU(Ctx, "SEQ counters look alive but readback is dead: treating as cold");
    return FALSE;
}

/* E3: DSP ready wait, only when ext_caps[7:4] == 1. A timeout is not fatal. */
NTSTATUS ApolloWaitDspsReady(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONG i, tries, v;

    Ctx->DspReadyTimeouts = 0;          /* [Twin X G3] counted for the step record; the x8p ignores it */
    if (!Ctx->FwV2 || ((ApolloRead(Ctx, REG_EXT_CAPS) >> 4) & 0xF) != 1) {
        return STATUS_SUCCESS;
    }
    for (i = 0; i < Ctx->NumDsps; i++) {
        for (tries = (i == 0) ? 100 : 10; tries > 0; tries--) {
            if (ApolloGateClosed(Ctx) || ApolloRead(Ctx, REG_FPGA_REV) == 0xFFFFFFFF) {
                return STATUS_DEVICE_NOT_CONNECTED;
            }
            if (ReadAcquire(&Ctx->StopRequested)) {
                return STATUS_CANCELLED;
            }
            v = ApolloRead(Ctx, ApolloDspWindow(i) + DSP_WIN_READY);
            if (v != 0 && v != DSP_READY_NOT_STARTED) {
                break;
            }
            ApolloSleepMs(DSP_READY_POLL_MS);
        }
        if (tries == 0) {
            ApolloLogU(Ctx, "DSP %lu never reported ready (continuing)", i);
            Ctx->DspReadyTimeouts++;
        }
    }
    return STATUS_SUCCESS;
}
