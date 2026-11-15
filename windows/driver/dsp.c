/*
 * dsp.c - DSP command/response rings: allocate, neutralize, program, send.
 *
 * Follows docs/spec-dsp-mixer.md section 2 and spec-bringup.md sections
 * 3.2 (B2), 3.5 (E4, E11) and 4.1 (3b).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "apollo.h"

static ULONG ApolloDspBank(_In_ ULONG Dsp)
{
    return (Dsp < 4 ? DSP_BANK_LOW : DSP_BANK_HIGH) + Dsp * DSP_BANK_STRIDE;
}

/* ------------------------------------------------------------------ */
/* Allocation                                                          */
/* ------------------------------------------------------------------ */

static NTSTATUS ApolloRingAlloc(_In_ PDEVICE_CONTEXT Ctx, _Inout_ PAPOLLO_RING Ring, _In_ ULONG Base)
{
    NTSTATUS status;
    PHYSICAL_ADDRESS la;

    status = WdfCommonBufferCreate(Ctx->DmaEnabler, RING_BYTES, WDF_NO_OBJECT_ATTRIBUTES,
                                   &Ring->Buffer);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    Ring->Va = (PUCHAR)WdfCommonBufferGetAlignedVirtualAddress(Ring->Buffer);
    la = WdfCommonBufferGetAlignedLogicalAddress(Ring->Buffer);
    Ring->La = (ULONGLONG)la.QuadPart;
    if ((Ring->La & (RING_PAGE_BYTES - 1)) != 0 || Ring->La + RING_BYTES > 0x100000000ULL) {
        ApolloLogU(Ctx, "ring buffer at 0x%llx is not page aligned below 4 GiB", Ring->La);
        WdfObjectDelete(Ring->Buffer);
        Ring->Buffer = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(Ring->Va, RING_BYTES);
    Ring->Base = Base;
    Ring->WritePtr = 0;
    Ring->Position = 0;
    Ring->Live = FALSE;
    Ring->Allocated = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS ApolloRingsAllocate(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONG i;
    NTSTATUS status;

    for (i = 0; i < Ctx->NumDsps; i++) {
        ULONG bank = ApolloDspBank(i);

        if (!Ctx->Cmd[i].Allocated) {
            status = ApolloRingAlloc(Ctx, &Ctx->Cmd[i], bank + DSP_RING_CMD);
            if (!NT_SUCCESS(status)) {
                ApolloLogU(Ctx, "DSP %lu: command ring allocation failed 0x%08x", i, status);
                return status;
            }
        }
        if (!Ctx->Resp[i].Allocated) {
            status = ApolloRingAlloc(Ctx, &Ctx->Resp[i], bank + DSP_RING_RESP);
            if (!NT_SUCCESS(status)) {
                ApolloLogU(Ctx, "DSP %lu: response ring allocation failed 0x%08x", i, status);
                return status;
            }
        }
    }
    return STATUS_SUCCESS;
}

/* Only after the quiesce (nothing may still fetch from these pages). */
VOID ApolloRingsFree(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONG i;

    for (i = 0; i < APOLLO_MAX_DSPS; i++) {
        if (Ctx->Cmd[i].Buffer != NULL) {
            WdfObjectDelete(Ctx->Cmd[i].Buffer);
        }
        if (Ctx->Resp[i].Buffer != NULL) {
            WdfObjectDelete(Ctx->Resp[i].Buffer);
        }
        RtlZeroMemory(&Ctx->Cmd[i], sizeof(Ctx->Cmd[i]));
        RtlZeroMemory(&Ctx->Resp[i], sizeof(Ctx->Resp[i]));
    }
}

/* ------------------------------------------------------------------ */
/* Register-level ring helpers                                         */
/* ------------------------------------------------------------------ */

/* POSITION, normalised: >= 0x400 is invalid (keep Last), ~0 = gone. */
static NTSTATUS ApolloRingReadPos(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Base,
                                  _In_ ULONG Last, _Out_ PULONG Pos)
{
    ULONG v = ApolloRead(Ctx, Base + RING_POSITION);

    if (v == 0xFFFFFFFF) {
        *Pos = Last;
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    *Pos = (v >= RING_ENTRIES) ? Last : v;
    return STATUS_SUCCESS;
}

/*
 * Resync (B2): pull the FPGA's target back to its position. DOORBELL is
 * written before WRITE_PTR. Register only, so it works before the ring
 * memory exists. Updates the software indices when Ring is given.
 */
static NTSTATUS ApolloRingResync(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Base, _Inout_opt_ PAPOLLO_RING Ring)
{
    ULONG pos, next, pass;
    NTSTATUS status;

    status = ApolloRingReadPos(Ctx, Base, 0, &pos);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    for (pass = 0; pass < 3; pass++) {
        ApolloWrite(Ctx, Base + RING_DOORBELL, pos);
        ApolloWrite(Ctx, Base + RING_WRITE_PTR, pos);
        status = ApolloRingReadPos(Ctx, Base, 0, &next);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        if (next == pos) {
            break;
        }
        pos = next;
    }
    if (pass == 3) {
        ApolloWrite(Ctx, Base + RING_DOORBELL, pos);
        ApolloWrite(Ctx, Base + RING_WRITE_PTR, pos);
    }
    if (Ring != NULL) {
        Ring->WritePtr = pos;
        Ring->Position = pos;
        Ring->Gen++;                    /* whatever was queued may never run (strips.c) */
    }
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Primitives for the strip transactions (strips.c), DevLock held      */
/* ------------------------------------------------------------------ */

NTSTATUS ApolloRingRefresh(_In_ PDEVICE_CONTEXT Ctx, _Inout_ PAPOLLO_RING Ring)
{
    ULONG pos;
    NTSTATUS status = ApolloRingReadPos(Ctx, Ring->Base, Ring->Position, &pos);

    if (NT_SUCCESS(status)) {
        Ring->Position = pos;
    }
    return status;
}

ULONG ApolloRingFreeSlots(_In_ const APOLLO_RING *Ring)
{
    return (Ring->Position - Ring->WritePtr - 1) & RING_INDEX_MASK;
}

ULONG ApolloRingPending(_In_ const APOLLO_RING *Ring)
{
    return (Ring->WritePtr - Ring->Position) & RING_INDEX_MASK;
}

/* No wait: the caller checked ApolloRingFreeSlots() for the whole batch. */
VOID ApolloRingWrite(_Inout_ PAPOLLO_RING Ring, _In_ ULONG W0, _In_ ULONG W1, _In_ ULONG W2,
                     _In_ ULONG W3)
{
    PULONG slot = (PULONG)(Ring->Va + (SIZE_T)(Ring->WritePtr & RING_INDEX_MASK) * RING_ENTRY_BYTES);

    slot[0] = W0;
    slot[1] = W1;
    slot[2] = W2;
    slot[3] = W3;
    Ring->WritePtr = (Ring->WritePtr + 1) & RING_INDEX_MASK;
}

/* Abandon what the ring still has queued (a reply descriptor nobody answered). */
NTSTATUS ApolloRingResyncNow(_In_ PDEVICE_CONTEXT Ctx, _Inout_ PAPOLLO_RING Ring)
{
    return ApolloRingResync(Ctx, Ring->Base, Ring);
}

NTSTATUS ApolloRingsNeutralizeAll(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONG i;
    NTSTATUS status;

    for (i = 0; i < Ctx->NumDsps; i++) {
        ULONG bank = ApolloDspBank(i);

        status = ApolloRingResync(Ctx, bank + DSP_RING_CMD, NULL);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        status = ApolloRingResync(Ctx, bank + DSP_RING_RESP, NULL);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }
    return STATUS_SUCCESS;
}

/* Wait (bounded) for the FPGA to consume everything queued. */
static VOID ApolloRingWaitIdle(_In_ PDEVICE_CONTEXT Ctx, _Inout_ PAPOLLO_RING Ring, _In_ ULONG TimeoutMs)
{
    ULONG waited = 0;
    ULONG pos;

    for (;;) {
        if (!NT_SUCCESS(ApolloRingReadPos(Ctx, Ring->Base, Ring->Position, &pos))) {
            return;
        }
        Ring->Position = pos;
        if (pos == Ring->WritePtr || waited >= TimeoutMs) {
            return;
        }
        ApolloSleepMs(1);
        waited++;
    }
}

/* __ua_dsp_ring_program: resync, page addresses, verify, zero, live. */
static NTSTATUS ApolloRingProgram(_In_ PDEVICE_CONTEXT Ctx, _Inout_ PAPOLLO_RING Ring, _In_ BOOLEAN Drain)
{
    NTSTATUS status;
    ULONG k;

    if (!Ring->Allocated) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (Drain && Ring->Live) {
        ApolloRingWaitIdle(Ctx, Ring, 20);
    }
    status = ApolloRingResync(Ctx, Ring->Base, Ring);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    for (k = 0; k < RING_PAGES; k++) {
        ULONGLONG a = Ring->La + (ULONGLONG)k * RING_PAGE_BYTES;

        ApolloWrite(Ctx, Ring->Base + RING_PAGE_LO(k), (ULONG)a);
        ApolloWrite(Ctx, Ring->Base + RING_PAGE_HI(k), (ULONG)(a >> 32));
    }
    for (k = 0; k < RING_PAGES; k++) {
        ULONGLONG a = Ring->La + (ULONGLONG)k * RING_PAGE_BYTES;
        ULONG lo = ApolloRead(Ctx, Ring->Base + RING_PAGE_LO(k));
        ULONG hi = ApolloRead(Ctx, Ring->Base + RING_PAGE_HI(k));

        if (lo != (ULONG)a || hi != (ULONG)(a >> 32)) {
            ApolloLogU(Ctx, "ring 0x%04x page %lu readback 0x%08x%08x, wrote 0x%llx",
                      Ring->Base, k, hi, lo, a);
        }
    }
    RtlZeroMemory(Ring->Va, RING_BYTES);
    Ring->Live = TRUE;
    return STATUS_SUCCESS;
}

/* E4 / F8: program every ring, then the per-DSP enable + strobe pulse. */
NTSTATUS ApolloRingsInit(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONG i, en, d, cache;
    NTSTATUS status;

    for (i = 0; i < Ctx->NumDsps; i++) {
        status = ApolloRingProgram(Ctx, &Ctx->Cmd[i], FALSE);
        if (NT_SUCCESS(status)) {
            status = ApolloRingProgram(Ctx, &Ctx->Resp[i], FALSE);
        }
        if (status == STATUS_DEVICE_NOT_CONNECTED) {
            return status;
        }
        if (!NT_SUCCESS(status)) {
            ApolloLogU(Ctx, "DSP %lu: ring program failed 0x%08x (skipped)", i, status);
        }
    }

    /*
     * Issue #46: exactly one enable per existing DSP; strobes are pulsed
     * (assert, deassert, read back) and never left set. "Reset while
     * enabled" is what starts the ring engine.
     */
    en = ApolloDmaEnableBits(Ctx);
    d = ApolloRead(Ctx, REG_DMA_CTRL);
    ApolloWrite(Ctx, REG_DMA_CTRL, (d & ~DMA_CTRL_STROBES) | en);
    cache = ApolloRead(Ctx, REG_DMA_CTRL) & ~DMA_CTRL_STROBES;
    ApolloWrite(Ctx, REG_DMA_CTRL, cache | (en << 8));
    ApolloWrite(Ctx, REG_DMA_CTRL, cache);
    Ctx->DmaCtrlCache = ApolloRead(Ctx, REG_DMA_CTRL);
    ApolloLogU(Ctx, "DMA_CTRL after per-DSP enable: 0x%08x (enables 0x%02x)", Ctx->DmaCtrlCache, en);

    /* The pulse may move POSITION. */
    for (i = 0; i < Ctx->NumDsps; i++) {
        if (Ctx->Cmd[i].Allocated) {
            (VOID)ApolloRingResync(Ctx, Ctx->Cmd[i].Base, &Ctx->Cmd[i]);
        }
        if (Ctx->Resp[i].Allocated) {
            (VOID)ApolloRingResync(Ctx, Ctx->Resp[i].Base, &Ctx->Resp[i]);
        }
    }
    Ctx->RingsLive = TRUE;
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Sending                                                             */
/* ------------------------------------------------------------------ */

static NTSTATUS ApolloRingPut(_In_ PDEVICE_CONTEXT Ctx, _Inout_ PAPOLLO_RING Ring,
                              _In_ ULONG W0, _In_ ULONG W1, _In_ ULONG W2, _In_ ULONG W3)
{
    ULONG free, waited = 0;
    PULONG slot;

    if (!Ring->Allocated || !Ring->Live) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    for (;;) {
        ULONG pos;
        NTSTATUS status = ApolloRingReadPos(Ctx, Ring->Base, Ring->Position, &pos);

        if (!NT_SUCCESS(status)) {
            return status;
        }
        Ring->Position = pos;
        free = (Ring->Position - Ring->WritePtr - 1) & RING_INDEX_MASK;
        if (free > 0) {
            break;
        }
        if (waited >= 100) {
            ApolloLogU(Ctx, "ring 0x%04x full", Ring->Base);
            return STATUS_DEVICE_BUSY;
        }
        ApolloSleepMs(1);
        waited++;
    }

    /* The 4 pages are contiguous in our common buffer: slot = index * 16. */
    slot = (PULONG)(Ring->Va + (SIZE_T)Ring->WritePtr * RING_ENTRY_BYTES);
    slot[0] = W0;
    slot[1] = W1;
    slot[2] = W2;
    slot[3] = W3;
    Ring->WritePtr = (Ring->WritePtr + 1) & RING_INDEX_MASK;
    return STATUS_SUCCESS;
}

/* DOORBELL first, WRITE_PTR second: the reverse reset the engine mid-DMA. */
VOID ApolloRingDoorbell(_In_ PDEVICE_CONTEXT Ctx, _In_ PAPOLLO_RING Ring)
{
    KeMemoryBarrier();
    ApolloWrite(Ctx, Ring->Base + RING_DOORBELL, Ring->WritePtr);
    ApolloWrite(Ctx, Ring->Base + RING_WRITE_PTR, Ring->WritePtr);
}

/* (days since 2007-01-01 UTC << 12) + minute of day; 0 before 2007. */
static ULONG ApolloConnectDate(VOID)
{
    LARGE_INTEGER now;
    ULONGLONG secs;

    KeQuerySystemTimePrecise(&now);
    if ((ULONGLONG)now.QuadPart < FILETIME_2007) {
        return 0;
    }
    secs = ((ULONGLONG)now.QuadPart - FILETIME_2007) / 10000000ULL;
    return (ULONG)(((secs / 86400ULL) << 12) + ((secs % 86400ULL) / 60ULL));
}

/* E11 ua_dsp_connect_all: connect every DSP, then the dated finalize to DSP 0. */
static NTSTATUS ApolloDspConnectOnce(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONG i;
    NTSTATUS status;

    if (Ctx->NumDsps == 0) {
        return STATUS_SUCCESS;
    }
    for (i = 0; i < Ctx->NumDsps; i++) {
        PAPOLLO_RING cmd = &Ctx->Cmd[i];

        if (!cmd->Allocated) {
            continue;
        }
        status = ApolloRingProgram(Ctx, cmd, TRUE);
        if (NT_SUCCESS(status)) {
            status = ApolloRingPut(Ctx, cmd, DSP_CMD_CONNECT, 1, 0, 0);
        }
        if (!NT_SUCCESS(status)) {
            ApolloLogU(Ctx, "DSP %lu: connect entry failed 0x%08x", i, status);
            return status;
        }
        ApolloRingDoorbell(Ctx, cmd);
        ApolloSleepMs(7);
        ApolloLogU(Ctx, "DSP %lu connect: pos %lu (expect %lu)", i,
                  ApolloRead(Ctx, cmd->Base + RING_POSITION), cmd->WritePtr);
    }
    ApolloSleepMs(2);

    if (Ctx->Cmd[0].Allocated) {
        ULONG date = ApolloConnectDate();

        status = ApolloRingPut(Ctx, &Ctx->Cmd[0], DSP_CMD_DATE, date, 0, 0);
        if (!NT_SUCCESS(status)) {
            ApolloLogU(Ctx, "DSP 0: finalize entry failed 0x%08x", status);
            return status;
        }
        ApolloRingDoorbell(Ctx, &Ctx->Cmd[0]);
        ApolloSleepMs(7);
        ApolloLogU(Ctx, "DSP 0 finalize (date 0x%08x): pos %lu (expect %lu)", date,
                  ApolloRead(Ctx, Ctx->Cmd[0].Base + RING_POSITION), Ctx->Cmd[0].WritePtr);
    }
    Ctx->DspsConnected = TRUE;
    return STATUS_SUCCESS;
}

/* ua_dsp_connect_retry: one retry after resetting DSP 0's engine. */
NTSTATUS ApolloDspConnectAll(_In_ PDEVICE_CONTEXT Ctx)
{
    NTSTATUS status = ApolloDspConnectOnce(Ctx);
    ULONG d, i;

    if (NT_SUCCESS(status) || status == STATUS_DEVICE_NOT_CONNECTED) {
        return status;
    }
    ApolloLogU(Ctx, "DSP connect failed 0x%08x: resetting engine 0 and retrying", status);

    /* ua_dma_reset_engine(0): clear enable bit 1, pulse strobe bit 9, read. */
    d = ApolloRead(Ctx, REG_DMA_CTRL) & ~DMA_CTRL_STROBES;
    d &= ~DMA_CTRL_ENABLE_DSP(0);
    ApolloWrite(Ctx, REG_DMA_CTRL, d | (DMA_CTRL_ENABLE_DSP(0) << 8));
    ApolloWrite(Ctx, REG_DMA_CTRL, d);
    (VOID)ApolloRead(Ctx, REG_DMA_CTRL);

    /* ua_dsp_rings_reprogram: page addresses again, no drain. */
    for (i = 0; i < Ctx->NumDsps; i++) {
        (VOID)ApolloRingProgram(Ctx, &Ctx->Cmd[i], FALSE);
        (VOID)ApolloRingProgram(Ctx, &Ctx->Resp[i], FALSE);
    }

    /* ua_dma_enable_engine(0): set bit 1, drop strobes. */
    d = ApolloRead(Ctx, REG_DMA_CTRL) & ~DMA_CTRL_STROBES;
    ApolloWrite(Ctx, REG_DMA_CTRL, d | DMA_CTRL_ENABLE_DSP(0));
    Ctx->DmaCtrlCache = ApolloRead(Ctx, REG_DMA_CTRL);

    return ApolloDspConnectOnce(Ctx);
}

/* ------------------------------------------------------------------ */
/* [Twin X G3] AudioExtension path (docs/spec-twinx.md 2)              */
/* ------------------------------------------------------------------ */

/*
 * E7, after the boot transport's DMA reset pulse: ua_dsp_rings_reprogram. "The DMA pulse
 * deactivates the FPGA ring engine; re-writing page addresses re-initializes it" (ua_core.c
 * 4385-4390). Every ring: resync, page addresses, read back, zero, no drain.
 */
NTSTATUS ApolloRingsReprogram(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONG i;
    NTSTATUS status, result = STATUS_SUCCESS;

    for (i = 0; i < Ctx->NumDsps; i++) {
        status = ApolloRingProgram(Ctx, &Ctx->Cmd[i], FALSE);
        if (NT_SUCCESS(status)) {
            status = ApolloRingProgram(Ctx, &Ctx->Resp[i], FALSE);
        }
        if (status == STATUS_DEVICE_NOT_CONNECTED) {
            return status;
        }
        if (!NT_SUCCESS(status)) {
            ApolloLogU(Ctx, "DSP %lu: ring re-program failed 0x%08x", i, status);
            result = status;
        }
    }
    return result;
}

/*
 * Wait up to Ms for the FPGA to take everything queued on a ring (POSITION == WRITE_PTR).
 * *TookMs: how long it took (or waited). STATUS_IO_TIMEOUT when it did not.
 */
static NTSTATUS ApolloRingWaitConsumed(_In_ PDEVICE_CONTEXT Ctx, _Inout_ PAPOLLO_RING Ring, _In_ ULONG Ms,
                                       _Out_ PULONG TookMs)
{
    ULONGLONG t0 = KeQueryInterruptTime(), dt;
    ULONG pos;
    NTSTATUS status;

    for (;;) {
        status = ApolloRingReadPos(Ctx, Ring->Base, Ring->Position, &pos);
        dt = (KeQueryInterruptTime() - t0) / 10000ULL;
        *TookMs = (ULONG)min(dt, (ULONGLONG)MAXULONG);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        Ring->Position = pos;
        if (pos == Ring->WritePtr) {
            return STATUS_SUCCESS;
        }
        if (dt >= Ms) {
            return STATUS_IO_TIMEOUT;
        }
        if (ReadAcquire(&Ctx->StopRequested)) {
            return STATUS_CANCELLED;
        }
        ApolloSleepMs(1);
    }
}

/*
 * G5, after ACEFACE, on every connect (cold and warm): the E11 sequence (connect to every DSP,
 * then the dated finalize to DSP 0), each command checked: the DSP's command POSITION must
 * reach WRITE_PTR within ConsumeMs, else STATUS_IO_TIMEOUT and nothing more is sent (the caller
 * stops the unit). Upstream sleeps 5-10 ms after each doorbell and checks nothing
 * (ua_dsp.c:2942-3059); this keeps a 7 ms pace too.
 */
NTSTATUS ApolloDspConnectChecked(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG ConsumeMs)
{
    ULONG i, ms = 0, date;
    NTSTATUS status;

    for (i = 0; i < Ctx->NumDsps; i++) {
        PAPOLLO_RING cmd = &Ctx->Cmd[i];

        if (!cmd->Allocated) {
            continue;
        }
        status = ApolloRingProgram(Ctx, cmd, TRUE);
        if (NT_SUCCESS(status)) {
            status = ApolloRingPut(Ctx, cmd, DSP_CMD_CONNECT, 1, 0, 0);
        }
        if (!NT_SUCCESS(status)) {
            ApolloLogU(Ctx, "G5: DSP %lu: connect entry not queued 0x%08x", i, status);
            return status;
        }
        ApolloRingDoorbell(Ctx, cmd);
        status = ApolloRingWaitConsumed(Ctx, cmd, ConsumeMs, &ms);
        if (!NT_SUCCESS(status)) {
            ApolloLogU(Ctx, "G5: DSP %lu did NOT take the ring connect within %lu ms (POSITION %lu, WRITE_PTR %lu): "
                      "0x%08x", i, ConsumeMs, cmd->Position, cmd->WritePtr, status);
            return status;
        }
        ApolloLogU(Ctx, "G5: DSP %lu took the ring connect in %lu ms (POSITION %lu)", i, ms, cmd->Position);
        if (ms < 7) {
            ApolloSleepMs(7 - ms);
        }
    }
    ApolloSleepMs(2);

    if (Ctx->Cmd[0].Allocated) {
        date = ApolloConnectDate();
        status = ApolloRingPut(Ctx, &Ctx->Cmd[0], DSP_CMD_DATE, date, 0, 0);
        if (!NT_SUCCESS(status)) {
            ApolloLogU(Ctx, "G5: DSP 0: finalize entry not queued 0x%08x", status);
            return status;
        }
        ApolloRingDoorbell(Ctx, &Ctx->Cmd[0]);
        status = ApolloRingWaitConsumed(Ctx, &Ctx->Cmd[0], ConsumeMs, &ms);
        if (!NT_SUCCESS(status)) {
            ApolloLogU(Ctx, "G5: DSP 0 did NOT take the dated finalize within %lu ms (POSITION %lu, WRITE_PTR %lu): "
                      "0x%08x", ConsumeMs, Ctx->Cmd[0].Position, Ctx->Cmd[0].WritePtr, status);
            return status;
        }
        ApolloLogU(Ctx, "G5: DSP 0 took the dated finalize (date 0x%08x) in %lu ms", date, ms);
        if (ms < 7) {
            ApolloSleepMs(7 - ms);
        }
    }
    Ctx->DspsConnected = TRUE;
    return STATUS_SUCCESS;
}

/* Rate ack {0x001A0002, rate} to every DSP with rings. */
NTSTATUS ApolloDspRateAck(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG RateHz)
{
    ULONG i;
    NTSTATUS status, result = STATUS_SUCCESS;

    for (i = 0; i < Ctx->NumDsps; i++) {
        if (!Ctx->Cmd[i].Allocated || !Ctx->Cmd[i].Live) {
            continue;
        }
        status = ApolloRingPut(Ctx, &Ctx->Cmd[i], DSP_CMD_RATE, RateHz, 0, 0);
        if (!NT_SUCCESS(status)) {
            ApolloLogU(Ctx, "DSP %lu: rate ack failed 0x%08x", i, status);
            result = status;
            continue;
        }
        ApolloRingDoorbell(Ctx, &Ctx->Cmd[i]);
    }
    if (NT_SUCCESS(result)) {
        Ctx->DspToldRate = RateHz;
    }
    return result;
}

/* Date-only word to DSP 0: the disconnect used by the quiesce. */
NTSTATUS ApolloDspSendDateOnly(_In_ PDEVICE_CONTEXT Ctx)
{
    NTSTATUS status;

    if (!Ctx->Cmd[0].Allocated || !Ctx->Cmd[0].Live) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    status = ApolloRingPut(Ctx, &Ctx->Cmd[0], DSP_CMD_DATE, ApolloConnectDate(), 0, 0);
    if (NT_SUCCESS(status)) {
        ApolloRingDoorbell(Ctx, &Ctx->Cmd[0]);
    }
    return status;
}

/* Quiesce 3b: disconnect word, wait for idle, resync, not live. */
NTSTATUS ApolloRingsQuiesce(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONG i;

    if (Ctx->DspsConnected) {
        (VOID)ApolloDspSendDateOnly(Ctx);
        Ctx->DspsConnected = FALSE;
    }
    for (i = 0; i < Ctx->NumDsps; i++) {
        PAPOLLO_RING r[2];
        ULONG k;

        r[0] = &Ctx->Cmd[i];
        r[1] = &Ctx->Resp[i];
        for (k = 0; k < 2; k++) {
            if (!r[k]->Allocated) {
                continue;
            }
            if (r[k]->Live) {
                ApolloRingWaitIdle(Ctx, r[k], 5);
            }
            (VOID)ApolloRingResync(Ctx, r[k]->Base, r[k]);
            r[k]->Live = FALSE;
        }
    }
    Ctx->RingsLive = FALSE;
    return STATUS_SUCCESS;
}
