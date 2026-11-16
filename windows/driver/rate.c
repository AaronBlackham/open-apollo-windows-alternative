/*
 * rate.c - [Rate] the sample rate of one unit: GET_RATE / SET_RATE (include\apollo_rate.h), the rate
 * change, the stored rate at bring-up. The rates themselves are data (rate_plan.h); the combined
 * device's change (both units, master first) is agg.c ApolloAggRateSet, which runs RateChange below
 * for each unit.
 *
 * A change, in the kext's order (oa:driver/ua_audio.c:5343-5453, ua_audio_rate_change_kext), with
 * no lock held at first:
 *   1. RateBegin: no other change, no ASIO host on the unit (or the request comes from that host's
 *      own handle, with its client stopped: ASIO's SetSampleRate); ASIO_OPEN / ASIO_START refuse
 *      from here until RateEnd (AsioLock, never held with the locks below).
 *   2. Plug-in inserts off (48 kHz only), input strips torn down (they are rebuilt at the new rate),
 *      both with no lock held, as at D0 exit.
 *   3. StripLock -> DevLock: engine and transport stopped; the Hi-band dwell; a 48 kHz session if a
 *      Hi-band rate follows a Hi-band clock; the clock written with the transport stopped and its
 *      acknowledgement awaited (one retry); the firmware's rate polled; every DSP told the rate;
 *      1.5 s hold (the notification block serviced meanwhile, as the service thread would); the IO
 *      descriptors re-read (the Twin X runs on them); the transport and the engine started again,
 *      with the clock written after the start as always (audio.c 2.5); the lock checked. A clock
 *      that neither acknowledged nor moved puts the old rate back the same way.
 *   4. Locks released: the Windows endpoints advertise the new rate (ACX format list, format change),
 *      the strips re-armed, the rate stored (Units\<key>\SampleRate), RateEnd.
 * DevLock is held about 2-3 s: IOCTLs that need it wait (or answer busy after 3 s); GET_RATE,
 * GET_INFO, the engine stats and ASIO do not take it.
 *
 * SET_RATE runs in the caller's thread (asio.c ApolloEvtIoInCallerContext sends it here), so the
 * unit's sequential IOCTL queue keeps answering while a change runs.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "apollo.h"
#include <ntstrsafe.h>

static const UNICODE_STRING g_RateValueName = RTL_CONSTANT_STRING(L"SampleRate");

/* ------------------------------------------------------------------ */
/* Derived values                                                      */
/* ------------------------------------------------------------------ */

/* Everything that follows SampleRate (PASSIVE; the engine is stopped, or not started yet). */
VOID ApolloRateDerive(_In_ PDEVICE_CONTEXT Ctx)
{
    const APOLLO_BRINGUP_PLAN *plan = ApolloPlanOf(Ctx);
    ULONG idx = ApolloRateIndex(Ctx->SampleRate);

    if (idx == 0) {
        Ctx->SampleRate = APOLLO_RATE_DEFAULT_HZ;
        idx = APOLLO_RATE_IDX_48K;
    }
    Ctx->RateIndex = idx;
    Ctx->FrameCfg = ApolloRateFrameCfg(plan != NULL ? plan->FrameCfg48k : X8P_SAFETY_FRAMES_48K, idx);
    Ctx->EngLead = ApolloRateLead(Ctx->EngLead48, Ctx->SampleRate);
    Ctx->EngGuard = ApolloRateGuard(Ctx->EngGuard48, Ctx->SampleRate);
    Ctx->AdcFrames = ApolloRateAdc(plan != NULL ? plan->AdcFrames : APOLLO_X8P_ADC_FRAMES, idx);
    Ctx->DacFrames = ApolloRateDac(plan != NULL ? plan->DacCenti : NULL, idx);     /* [Lat] */
}

/* The rate the unit's key asks for (Units\<key>\SampleRate), or 48 kHz. Before or after ApolloUnitResolve. */
ULONG ApolloRateStoredHz(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONG hz = ApolloUnitParam(Ctx, APOLLO_PARAM_SAMPLE_RATE);

    return ApolloRateIndex(hz) != 0 ? hz : APOLLO_RATE_DEFAULT_HZ;
}

/* Every write of the clock word: the Hi-band dwell starts when the clock first goes to 176.4/192 kHz. */
VOID ApolloRateClockWritten(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Index)
{
    if (!ApolloRateIsHi(Index)) {
        Ctx->HiClockIdx = 0;
    } else if (Ctx->HiClockIdx != Index) {
        Ctx->HiClockIdx = Index;
        Ctx->HiClockSince = KeQueryInterruptTime();
    }
}

static VOID RateSetWhy(_Inout_ PDEVICE_CONTEXT Ctx, _In_z_ _Printf_format_string_ PCSTR Fmt, ...)
{
    va_list ap;

    va_start(ap, Fmt);
    (VOID)RtlStringCbVPrintfA(Ctx->RateWhy, sizeof(Ctx->RateWhy), Fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* The steps (DevLock held, transport stopped)                         */
/* ------------------------------------------------------------------ */

/* Ms of service passes, 10 ms apart: what the blocked service thread would do (mixer.c). */
static NTSTATUS RateServiceWait(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Ms)
{
    ULONG waited;

    for (waited = 0; waited < Ms; waited += 10) {
        if (ReadAcquire(&Ctx->StopRequested)) {
            return STATUS_CANCELLED;
        }
        if (ApolloGateClosed(Ctx) || ApolloRead(Ctx, REG_FPGA_REV) == 0xFFFFFFFF) {
            return STATUS_DEVICE_NOT_CONNECTED;
        }
        ApolloServiceStep(Ctx);
        ApolloSleepMs(10);
    }
    return STATUS_SUCCESS;
}

/* The Hi-band dwell before a clock write to Index (upstream hi_dwell_ms; the x4 wedged without it). */
static NTSTATUS RateDwell(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Index)
{
    ULONGLONG elapsed = (KeQueryInterruptTime() - Ctx->HiClockSince) / 10000ULL;
    ULONG wait = ApolloRateDwellMs(Ctx->HiClockIdx, elapsed > 0xFFFFFFFFull ? 0xFFFFFFFFul : (ULONG)elapsed, Index);

    if (wait == 0) {
        return STATUS_SUCCESS;
    }
    ApolloLogU(Ctx, "rate: the clock went to %s %llu ms ago: waiting %lu ms before changing it (Hi-band dwell)",
              ApolloRateText(ApolloRateHz(Ctx->HiClockIdx)), elapsed, wait);
    return RateServiceWait(Ctx, wait);
}

/*
 * The clock change with the transport stopped (ua_audio_clock_change_locked): the word, the ack bits
 * of 0xC030 cleared (a plain register: write the rest back), doorbell 4, up to 2 s for bit 5, 22 or 4;
 * one retry. STATUS_IO_TIMEOUT: no acknowledgement (the caller looks at the firmware's rate anyway).
 */
static NTSTATUS RateClockChange(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Index, _Out_ PULONG AckMs, _Out_ PULONG AckBits)
{
    ULONG attempt, waited, s = 0;
    NTSTATUS status;

    *AckMs = 0;
    *AckBits = 0;
    status = RateDwell(Ctx, Index);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    for (attempt = 0; attempt < 2; attempt++) {
        ApolloWrite(Ctx, REG_NOTIF_CLOCK_CFG, ApolloPlanClockWord(Index, CLOCK_SOURCE_INTERNAL));
        ApolloRateClockWritten(Ctx, Index);
        s = ApolloRead(Ctx, REG_NOTIF_STATUS);
        if (s == 0xFFFFFFFF) {
            return STATUS_DEVICE_NOT_CONNECTED;
        }
        if ((s & APOLLO_RATE_ACK_BITS) != 0) {
            ApolloWrite(Ctx, REG_NOTIF_STATUS, s & ~APOLLO_RATE_ACK_BITS);  /* a stale ack must not count */
        }
        ApolloWrite(Ctx, REG_AX_CONNECT, AX_CONNECT_CLOCK);
        for (waited = 0; waited < APOLLO_RATE_CLOCK_ACK_MS; waited += 10) {
            s = ApolloRead(Ctx, REG_NOTIF_STATUS);
            if (s == 0xFFFFFFFF) {
                return STATUS_DEVICE_NOT_CONNECTED;
            }
            if ((s & APOLLO_RATE_ACK_BITS) != 0) {
                Ctx->NotifStatusSeen |= s;
                *AckMs = waited;
                *AckBits = s;
                return STATUS_SUCCESS;
            }
            if (ReadAcquire(&Ctx->StopRequested)) {
                return STATUS_CANCELLED;
            }
            ApolloSleepMs(10);
        }
        ApolloLogU(Ctx, "rate: clock change to index %lu not acknowledged in %lu ms (0xC030 0x%08lx)%s", Index,
                  APOLLO_RATE_CLOCK_ACK_MS, s, attempt == 0 ? ": once more" : "");
    }
    return (ApolloRead(Ctx, REG_FPGA_REV) == 0xFFFFFFFF) ? STATUS_DEVICE_NOT_CONNECTED : STATUS_IO_TIMEOUT;
}

/* The firmware's rate (0xC07C [15:8]) at Index: 25 ms polls up to MaxMs, notifications serviced meanwhile. */
static NTSTATUS RateFirmwareWait(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Index, _In_ ULONG MaxMs, _Out_ PULONG Ms)
{
    ULONG waited, v = 0;

    for (waited = 0; waited < MaxMs; waited += APOLLO_RATE_FW_POLL_MS) {
        ApolloServiceStep(Ctx);         /* acks 0xC030: the firmware posts the new rate only after that */
        v = ApolloRead(Ctx, REG_NOTIF_RATE_INFO);
        if (v == 0xFFFFFFFF) {
            return STATUS_DEVICE_NOT_CONNECTED;
        }
        Ctx->NotifRateInfo = v;
        if (((v >> 8) & 0xFF) == Index) {
            *Ms = waited;
            return STATUS_SUCCESS;
        }
        if (ReadAcquire(&Ctx->StopRequested)) {
            return STATUS_CANCELLED;
        }
        ApolloSleepMs(APOLLO_RATE_FW_POLL_MS);
    }
    *Ms = waited;
    return STATUS_IO_TIMEOUT;
}

/* After the transport start: 0xC07C locked at Index (up to 2 s, serviced). */
static BOOLEAN RateLockWait(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Index, _Out_ PULONG Ms)
{
    ULONG waited, v;

    for (waited = 0; waited < APOLLO_RATE_LOCK_CHECK_MS; waited += 10) {
        v = ApolloRead(Ctx, REG_NOTIF_RATE_INFO);
        if (v == 0xFFFFFFFF) {
            break;
        }
        Ctx->NotifRateInfo = v;
        if (ApolloRateInfoLocked(v, Index)) {
            *Ms = waited;
            return TRUE;
        }
        ApolloServiceStep(Ctx);
        ApolloSleepMs(10);
    }
    *Ms = waited;
    return FALSE;
}

/*
 * A Hi-band start from a Hi-band clock (upstream ua_audio_hi_from_hi, x4 2026-10-10): run the
 * transport at 48 kHz for HoldMs first (no copy engine: nothing is streamed), then stop it. Without
 * it a 176.4/192 kHz start from 176.4/192 kHz records silence (or now and then, at 176.4).
 */
static NTSTATUS RateLoMidSession(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG HoldMs)
{
    ULONG keepHz = Ctx->SampleRate;
    NTSTATUS status;

    ApolloLogU(Ctx, "rate: %s from a %s clock: running 48 kHz for %lu ms first (Hi-band start rule)",
              ApolloRateText(keepHz), ApolloRateText(ApolloRateHz((Ctx->NotifRateInfo >> 8) & 0xFF)), HoldMs);
    status = RateDwell(Ctx, APOLLO_RATE_IDX_48K);
    if (NT_SUCCESS(status)) {
        Ctx->SampleRate = APOLLO_RATE_DEFAULT_HZ;
        ApolloRateDerive(Ctx);
        status = ApolloTransportStartEx(Ctx, FALSE);    /* clock 48 kHz after the start, the DSPs told */
        if (NT_SUCCESS(status)) {
            status = RateServiceWait(Ctx, HoldMs);
        }
        ApolloTransportStop(Ctx);
    }
    Ctx->SampleRate = keepHz;
    ApolloRateDerive(Ctx);
    ApolloLogU(Ctx, "rate: 48 kHz session done (0x%08x)", status);
    return status;
}

/*
 * Index from whatever the clock runs at now, transport stopped (DevLock held): dwell, the 48 kHz
 * session if needed, the clock change, the firmware's rate, the rate ack, the hold. SampleRate and
 * the derived values follow Index once the clock moved (or the firmware says it is there). The
 * transport stays stopped. *Moved: the firmware reports Index.
 */
static NTSTATUS RateMoveClock(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Index, _Out_ PBOOLEAN Moved)
{
    ULONG ackMs = 0, ackBits = 0, fwMs = 0, fw, hold;
    NTSTATUS status, cs, fs;

    *Moved = FALSE;
    fw = (ApolloRead(Ctx, REG_NOTIF_RATE_INFO) >> 8) & 0xFF;
    hold = ApolloRateLoMidHoldMs(fw, Index);
    if (hold != 0) {
        status = RateLoMidSession(Ctx, hold);
        if (status == STATUS_DEVICE_NOT_CONNECTED || status == STATUS_CANCELLED) {
            return status;
        }
    }
    cs = RateClockChange(Ctx, Index, &ackMs, &ackBits);
    if (cs == STATUS_DEVICE_NOT_CONNECTED || cs == STATUS_CANCELLED) {
        return cs;
    }
    /* the kext's 4 s after an acknowledgement; without one only a look (the caller has another way) */
    fs = RateFirmwareWait(Ctx, Index, NT_SUCCESS(cs) ? APOLLO_RATE_FW_WAIT_MS : 500, &fwMs);
    if (fs == STATUS_DEVICE_NOT_CONNECTED || fs == STATUS_CANCELLED) {
        return fs;
    }
    ApolloLogU(Ctx, "rate: clock word 0x%03lx: %s (0xC030 0x%08lx after %lu ms); firmware %s (0xC07C 0x%08lx, %lu ms)",
              ApolloPlanClockWord(Index, CLOCK_SOURCE_INTERNAL), NT_SUCCESS(cs) ? "acknowledged" : "NOT acknowledged",
              ackBits, ackMs, NT_SUCCESS(fs) ? "there" : "NOT there", Ctx->NotifRateInfo, fwMs);
    if (!NT_SUCCESS(cs) && !NT_SUCCESS(fs)) {
        return STATUS_IO_TIMEOUT;       /* nothing moved: the caller puts the old rate back */
    }
    *Moved = NT_SUCCESS(fs) ? TRUE : FALSE;
    Ctx->SampleRate = ApolloRateHz(Index);
    ApolloRateDerive(Ctx);
    status = ApolloDspRateAck(Ctx, Ctx->SampleRate);
    ApolloLogU(Ctx, "rate: %lu Hz acknowledged to every DSP (0x%08x); holding %lu ms with the transport stopped",
              Ctx->SampleRate, status, APOLLO_RATE_ACK_HOLD_MS);
    return RateServiceWait(Ctx, APOLLO_RATE_ACK_HOLD_MS);
}

/* ------------------------------------------------------------------ */
/* Bring-up (device.c, DevLock held, before the transport start)       */
/* ------------------------------------------------------------------ */

/*
 * The stored rate at bring-up. 48 kHz runs exactly the path every release ran (the clock is written
 * after the transport start). Any other rate: offered by the unit (else 48 kHz, logged), then the
 * clock moved with the transport stopped as a runtime change does, unless the firmware is there
 * already; the caller starts the transport as always.
 */
VOID ApolloRateBringup(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONG offered = ApolloRateOffered(Ctx->NotifClockSrcs), want = Ctx->SampleRate, fw;
    BOOLEAN moved = FALSE;
    NTSTATUS status;

    if ((offered & (1ul << ApolloRateIndex(want))) == 0) {
        ApolloLogU(Ctx, "rate: the stored %lu Hz is not offered (clock sources 0x%08lx): 48 kHz", want, Ctx->NotifClockSrcs);
        RateSetWhy(Ctx, "the stored %s is not offered by this unit: 48 kHz", ApolloRateText(want));
        Ctx->SampleRate = APOLLO_RATE_DEFAULT_HZ;
    }
    ApolloRateDerive(Ctx);
    if (Ctx->RateIndex == APOLLO_RATE_IDX_48K) {
        return;
    }
    fw = (Ctx->NotifRateInfo >> 8) & 0xFF;
    if (fw == Ctx->RateIndex && ApolloRateLoMidHoldMs(fw, Ctx->RateIndex) == 0) {
        ApolloLogU(Ctx, "rate: the unit runs at the stored %s already", ApolloRateText(Ctx->SampleRate));
        return;
    }
    ApolloLogU(Ctx, "rate: bring-up at the stored %s (firmware at index %lu)", ApolloRateText(Ctx->SampleRate), fw);
    status = RateMoveClock(Ctx, Ctx->RateIndex, &moved);
    if (status == STATUS_IO_TIMEOUT) {
        /* nothing acknowledged with the transport stopped: the start writes it after, as at 48 kHz */
        ApolloLogU(Ctx, "rate: no clock acknowledgement with the transport stopped: %s is written after the "
                  "transport start", ApolloRateText(Ctx->SampleRate));
    } else if (!NT_SUCCESS(status) && status != STATUS_DEVICE_NOT_CONNECTED && status != STATUS_CANCELLED) {
        ApolloLogU(Ctx, "rate: the clock did not move to %lu Hz (0x%08x): 48 kHz", want, status);
        RateSetWhy(Ctx, "the clock did not move to %s at bring-up: 48 kHz", ApolloRateText(want));
        Ctx->SampleRate = APOLLO_RATE_DEFAULT_HZ;
        ApolloRateDerive(Ctx);
    }
}

/* ------------------------------------------------------------------ */
/* Storage                                                             */
/* ------------------------------------------------------------------ */

/* Units\<Key>\SampleRate = Hz (PASSIVE, no lock). Key: the unit key name (16 hex digits or SN<serial>). */
_Use_decl_annotations_
NTSTATUS ApolloRateStoreKey(PCSTR Key, ULONG Hz)
{
    WDFKEY params = NULL, units = NULL, unit = NULL;
    UNICODE_STRING n;
    WCHAR name[40];
    NTSTATUS status;

    if (Key == NULL || Key[0] == 0) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    status = WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ | KEY_WRITE, WDF_NO_OBJECT_ATTRIBUTES, &params);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    RtlInitUnicodeString(&n, L"Units");
    status = WdfRegistryCreateKey(params, &n, KEY_READ | KEY_WRITE, REG_OPTION_NON_VOLATILE, NULL,
                                  WDF_NO_OBJECT_ATTRIBUTES, &units);
    if (NT_SUCCESS(status)) {
        status = RtlStringCbPrintfW(name, sizeof(name), L"%hs", Key);
        if (NT_SUCCESS(status)) {
            RtlInitUnicodeString(&n, name);
            status = WdfRegistryCreateKey(units, &n, KEY_READ | KEY_WRITE, REG_OPTION_NON_VOLATILE, NULL,
                                          WDF_NO_OBJECT_ATTRIBUTES, &unit);
        }
        WdfRegistryClose(units);
    }
    if (NT_SUCCESS(status)) {
        status = WdfRegistryAssignULong(unit, &g_RateValueName, Hz);
        WdfRegistryClose(unit);
    }
    WdfRegistryClose(params);
    return status;
}

/* This unit's stored rate, and the value Ctx->Unit holds for it (PASSIVE, no lock). */
_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS RateStore(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Hz)
{
    NTSTATUS status;

    if (Ctx->Ident.UnitKeyKind == APOLLO_UNIT_KEY_NONE) {
        return STATUS_OBJECT_NAME_NOT_FOUND;            /* no stable identity: nothing is kept for it */
    }
    status = ApolloRateStoreKey(Ctx->Ident.UnitKey, Hz);
    if (NT_SUCCESS(status)) {
        Ctx->Unit.Value[APOLLO_PARAM_SAMPLE_RATE] = Hz;
        Ctx->Unit.Source[APOLLO_PARAM_SAMPLE_RATE] = APOLLO_SRC_UNIT;
        Ctx->RateStoredFlag = TRUE;
    }
    ApolloLogU(Ctx, "rate: %lu Hz stored under Units\\%s (0x%08x)", Hz, Ctx->Ident.UnitKey, status);
    return status;
}

/* ------------------------------------------------------------------ */
/* The change                                                          */
/* ------------------------------------------------------------------ */

/*
 * No other change, no ASIO client in the way (AsioLock alone). An ASIO client is allowed only when
 * File is its own handle and it is stopped: ASIO's SetSampleRate from the host that has the unit.
 */
static ULONG RateBegin(_In_ PDEVICE_CONTEXT Ctx, _In_opt_ WDFFILEOBJECT File)
{
    ULONG err = APOLLO_RATE_ERR_NONE;

    WdfWaitLockAcquire(Ctx->AsioLock, NULL);
    if (ReadAcquire(&Ctx->RateChanging) != 0 || ReadAcquire(&Ctx->ClockChanging) != 0) {
        err = APOLLO_RATE_ERR_BUSY;             /* [Clock] a clock change holds the unit too */
    } else if (Ctx->Asio.Open && (File == NULL || Ctx->Asio.OwnerFile != File || Ctx->Asio.Active)) {
        err = APOLLO_RATE_ERR_ASIO;
    } else {
        InterlockedExchange(&Ctx->RateChanging, 1);     /* ASIO_OPEN / ASIO_START refuse from here */
    }
    WdfWaitLockRelease(Ctx->AsioLock);
    return err;
}

static VOID RateEnd(_In_ PDEVICE_CONTEXT Ctx)
{
    WdfWaitLockAcquire(Ctx->AsioLock, NULL);
    ApolloAsioRateChanged(Ctx);                         /* the open client's shared block: rate, latencies */
    InterlockedExchange(&Ctx->RateChanging, 0);
    WdfWaitLockRelease(Ctx->AsioLock);
}

/* Is the unit able to change at all? APOLLO_RATE_ERR_NONE or the reason (Why set). */
static ULONG RateUnitReady(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Hz)
{
    ULONG idx = ApolloRateIndex(Hz);
    ULONG srcs = Ctx->NotifClockSrcsLive != 0 ? Ctx->NotifClockSrcsLive : Ctx->NotifClockSrcs;

    if (idx == 0) {
        RateSetWhy(Ctx, "%lu Hz is not a rate the Apollo runs at (44.1, 48, 88.2, 96, 176.4 or 192 kHz)", Hz);
        return APOLLO_RATE_ERR_RATE;
    }
    if (Ctx->Stage == ApolloStageGone || ApolloGateClosed(Ctx) || !Ctx->Unit.Active ||
        Ctx->Unit.Value[APOLLO_PARAM_MODE] < APOLLO_MODE_FULL) {
        RateSetWhy(Ctx, "the unit is not running in Mode 2 (observe, or bring-up only)");
        return APOLLO_RATE_ERR_NOT_READY;
    }
    if (!Ctx->Connected || !Ctx->RingsLive) {
        RateSetWhy(Ctx, "the unit is not connected (still starting, or its bring-up failed)");
        return APOLLO_RATE_ERR_NOT_READY;
    }
    if (ApolloEngineLost(Ctx)) {
        RateSetWhy(Ctx, "the unit stopped answering: replug or power-cycle it");
        return APOLLO_RATE_ERR_HARDWARE;
    }
    if ((ApolloRateAllowed(srcs, ApolloClockWantedOf(Ctx)) & (1ul << idx)) == 0) {
        /* [Clock] on a S/PDIF or ADAT clock: 44.1-96 kHz (clock_plan.h) */
        RateSetWhy(Ctx, "this unit does not offer %s on its %s clock (clock sources 0x%08lx)", ApolloRateText(Hz),
                   ApolloClockName(ApolloClockWantedOf(Ctx)), srcs);
        return APOLLO_RATE_ERR_UNSUPPORTED;
    }
    return APOLLO_RATE_ERR_NONE;
}

/*
 * One unit to Hz. InAgg: agg.c runs it for the combined device (Agg.Mutex held, the aggregate's tick
 * paused); otherwise the unit must not run in a combined device with another unit. Fills Ctx's
 * RateResult / RateStatus / RateWhy and returns RateResult.
 */
_Use_decl_annotations_
ULONG ApolloRateChange(PDEVICE_CONTEXT Ctx, ULONG Hz, ULONG Flags, WDFFILEOBJECT File, BOOLEAN InAgg)
{
    ULONGLONG t0 = KeQueryInterruptTime();
    ULONG err, oldHz, idx = ApolloRateIndex(Hz), lockMs = 0;
    BOOLEAN wasRunning, moved = FALSE, locked = FALSE, layoutChanged = FALSE, stripsBack = FALSE, postStart = FALSE;
    NTSTATUS status = STATUS_SUCCESS;

    err = RateUnitReady(Ctx, Hz);
    if (err == APOLLO_RATE_ERR_NONE && !InAgg && ApolloAggRateMember(Ctx)) {
        RateSetWhy(Ctx, "this unit runs in the combined device with another unit: set the combined rate");
        err = APOLLO_RATE_ERR_COMBINED;
    }
    if (err != APOLLO_RATE_ERR_NONE) {
        Ctx->RateResult = err;
        Ctx->RateStatus = STATUS_SUCCESS;
        return err;
    }
    if (Hz == Ctx->SampleRate && ApolloRateInfoLocked(ApolloRead(Ctx, REG_NOTIF_RATE_INFO), idx)) {
        RateSetWhy(Ctx, "already at %s", ApolloRateText(Hz));
        Ctx->RateResult = APOLLO_RATE_ERR_NONE;
        Ctx->RateStatus = STATUS_SUCCESS;
        if ((Flags & APOLLO_RATE_SET_NO_STORE) == 0 && ApolloRateStoredHz(Ctx) != Hz) {
            (VOID)RateStore(Ctx, Hz);
        }
        return APOLLO_RATE_ERR_NONE;
    }
    err = RateBegin(Ctx, File);
    if (err != APOLLO_RATE_ERR_NONE) {
        RateSetWhy(Ctx, err == APOLLO_RATE_ERR_BUSY ? "another rate or clock change is under way on this unit" :
                   "an ASIO host (a DAW) has this unit: change the rate there, or close its audio device");
        Ctx->RateResult = err;
        Ctx->RateStatus = STATUS_SUCCESS;
        return err;
    }
    oldHz = Ctx->SampleRate;
    ApolloLogU(Ctx, "rate: change %s -> %s%s", ApolloRateText(oldHz), ApolloRateText(Hz),
              InAgg ? " (combined device)" : "");

    /* 2. Plug-in inserts and input strips off, no lock held (their own StripLock -> DevLock). */
    ApolloPluginQuiesce(Ctx, "rate change", FALSE);
    ApolloStripsQuiesce(Ctx, "rate change");

    /* 3. The clock. */
    WdfWaitLockAcquire(Ctx->StripLock, NULL);
    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    wasRunning = Ctx->TransportRunning;
    ApolloTransportStop(Ctx);                   /* the engine first, then AX_CONTROL */
    status = RateMoveClock(Ctx, idx, &moved);
    if (status == STATUS_DEVICE_NOT_CONNECTED || status == STATUS_CANCELLED) {
        err = APOLLO_RATE_ERR_HARDWARE;
        RateSetWhy(Ctx, "the unit stopped answering during the change (0x%08x)", status);
    } else if (status == STATUS_IO_TIMEOUT && Ctx->SampleRate == oldHz) {
        /*
         * Nothing acknowledged and the firmware did not move with the transport stopped: move it the way
         * every bring-up does, the clock written right after the transport start (audio.c 2.5), and
         * put the old rate back the same way if that does not lock either.
         */
        ApolloLogU(Ctx, "rate: no clock acknowledgement with the transport stopped: %s written after the transport "
                  "start instead", ApolloRateText(Hz));
        postStart = TRUE;
        Ctx->SampleRate = Hz;
        ApolloRateDerive(Ctx);
    }
    if (err == APOLLO_RATE_ERR_NONE) {
        /* the firmware republishes its descriptors when the band changes: the Twin X runs on them */
        layoutChanged = ApolloAudioDescriptorsReread(Ctx);
        if (layoutChanged) {
            ApolloStreamsLayoutChanged(Ctx);    /* running streams find their slots again at the engine start */
        }
        if (wasRunning || postStart) {
            status = ApolloTransportStart(Ctx); /* the clock again after the start; the engine; the DSPs if needed */
            if (!NT_SUCCESS(status)) {
                err = APOLLO_RATE_ERR_HARDWARE;
                RateSetWhy(Ctx, "the transport did not start again at %s (0x%08x)", ApolloRateText(Ctx->SampleRate),
                           status);
            }
        }
        if (err == APOLLO_RATE_ERR_NONE) {
            locked = RateLockWait(Ctx, Ctx->RateIndex, &lockMs);
            if (!locked && postStart) {
                ApolloLogU(Ctx, "rate: %s did not lock after the start either (0xC07C 0x%08lx): back to %s",
                          ApolloRateText(Hz), Ctx->NotifRateInfo, ApolloRateText(oldHz));
                ApolloTransportStop(Ctx);
                Ctx->SampleRate = oldHz;
                ApolloRateDerive(Ctx);
                status = ApolloTransportStart(Ctx);
                (VOID)RateLockWait(Ctx, Ctx->RateIndex, &lockMs);
                err = APOLLO_RATE_ERR_LOCK;
                RateSetWhy(Ctx, "the clock did not take %s: still %s", ApolloRateText(Hz), ApolloRateText(oldHz));
            } else if (!locked) {
                err = APOLLO_RATE_ERR_LOCK;
                RateSetWhy(Ctx, "%s set, but the clock does not report a lock (0xC07C 0x%08lx)",
                           ApolloRateText(Ctx->SampleRate), Ctx->NotifRateInfo);
            }
        }
        if (postStart && !wasRunning && Ctx->TransportRunning) {
            ApolloTransportStop(Ctx);           /* it was stopped before the change: as it was */
        }
    }
    stripsBack = ApolloStripsRateRearm(Ctx);    /* armed again: the pass builds them at this rate */
    Ctx->RateStripsRebuilt = stripsBack;
    Ctx->RateChanges++;
    WdfWaitLockRelease(Ctx->DevLock);
    WdfWaitLockRelease(Ctx->StripLock);

    /* 4. Windows, storage, the combined device. Not into ACX while a D0 exit waits (StopRequested). */
    if (ReadAcquire(&Ctx->StopRequested) == 0) {
        ApolloAcxSetRate(Ctx);
    }
    if (!InAgg) {
        ApolloAggRateUnitChanged(Ctx);          /* a unit of a combined device that runs alone now */
    }
    ApolloEventPost(Ctx, APOLLO_EVENT_RATE, Ctx->SampleRate, err);            /* [Events] */
    if (err == APOLLO_RATE_ERR_NONE) {
        RateSetWhy(Ctx, "%s, locked in %lu ms%s%s", ApolloRateText(Ctx->SampleRate), lockMs,
                   layoutChanged ? "; channel layout changed" : "",
                   stripsBack ? "; input strips rebuilding" : "");
        if ((Flags & APOLLO_RATE_SET_NO_STORE) == 0) {
            (VOID)RateStore(Ctx, Ctx->SampleRate);
        }
    }
    RateEnd(Ctx);
    Ctx->RateLastMs = (ULONG)((KeQueryInterruptTime() - t0) / 10000ULL);
    Ctx->RateResult = err;
    Ctx->RateStatus = status;
    ApolloLogU(Ctx, "rate: %s -> %s %s in %lu ms (%s)", ApolloRateText(oldHz), ApolloRateText(Ctx->SampleRate),
              err == APOLLO_RATE_ERR_NONE ? "done" : "FAILED", Ctx->RateLastMs, Ctx->RateWhy);
    return err;
}

/* ------------------------------------------------------------------ */
/* IOCTLs                                                              */
/* ------------------------------------------------------------------ */

VOID ApolloRateFillInfo(_In_ PDEVICE_CONTEXT Ctx, _Out_ PAPOLLO_RATE_INFO I)
{
    ULONG srcs = Ctx->NotifClockSrcsLive != 0 ? Ctx->NotifClockSrcsLive : Ctx->NotifClockSrcs;

    RtlZeroMemory(I, sizeof(*I));
    I->Version = APOLLO_RATE_VERSION;
    I->Bytes = sizeof(*I);
    I->Rate = Ctx->SampleRate;
    I->RateIndex = Ctx->RateIndex;
    I->RateInfo = Ctx->NotifRateInfo;
    I->Offered = ApolloRateOffered(srcs);
    I->Allowed = ApolloRateAllowed(srcs, ApolloClockWantedOf(Ctx));     /* [Clock] the source asked for */
    I->ClockSource = ApolloClockSourceOf(Ctx);                          /* [Clock] the one it runs on now */
    I->Stored = ApolloRateStoredHz(Ctx);
    I->Result = Ctx->RateResult;
    I->Status = Ctx->RateStatus;
    I->LastMs = Ctx->RateLastMs;
    I->Changes = Ctx->RateChanges;
    I->FrameCfg = Ctx->FrameCfg;
    I->LeadFrames = Ctx->EngLead;
    I->GuardFrames = Ctx->EngGuard;
    I->AdcFrames = Ctx->AdcFrames;
    I->InPathFrames = ApolloLatencyIn(Ctx) - Ctx->EngGuard - Ctx->AdcFrames;   /* [Lat] the device side, now */
    I->DacFrames = Ctx->DacFrames;
    if (ApolloRateInfoLocked(Ctx->NotifRateInfo, Ctx->RateIndex)) {
        I->Flags |= APOLLO_RATE_F_LOCKED;
    }
    if (ReadNoFence(&Ctx->RateChanging) != 0) {
        I->Flags |= APOLLO_RATE_F_CHANGING;
    }
    if (Ctx->Asio.Open) {
        I->Flags |= APOLLO_RATE_F_ASIO_OPEN;
    }
    if (ApolloStreamsPrepared(Ctx)) {
        I->Flags |= APOLLO_RATE_F_STREAMS;
    }
    if (Ctx->RateStripsRebuilt) {
        I->Flags |= APOLLO_RATE_F_STRIPS_REBUILT;
    }
    if (Ctx->SampleRate != APOLLO_RATE_DEFAULT_HZ && Ctx->Ident.Model == APOLLO_MODEL_X8P) {
        I->Flags |= APOLLO_RATE_F_PLUGINS_48K;
    }
    if (ApolloCorrRateMismatch(Ctx)) {
        I->Flags |= APOLLO_RATE_F_CORR_MISMATCH;
    }
    if (Ctx->RateStoredFlag || Ctx->Unit.Source[APOLLO_PARAM_SAMPLE_RATE] == APOLLO_SRC_UNIT) {
        I->Flags |= APOLLO_RATE_F_STORED;
    }
    (VOID)RtlStringCbCopyA(I->Why, sizeof(I->Why), Ctx->RateWhy);
    ApolloAggRateFill(Ctx, I);                  /* Combined*, F_COMBINED, F_COMBINED_ASIO, F_COMBINED_MISMATCH */
}

BOOLEAN ApolloRateIsIoctl(_In_ ULONG Code)
{
    return Code == IOCTL_APOLLO_SET_RATE || Code == IOCTL_APOLLO_SET_CLOCK;     /* [Clock] SET_CLOCK too: clock.c */
}

/*
 * SET_RATE in the caller's thread (PASSIVE; asio.c ApolloEvtIoInCallerContext). The outcome is in
 * the returned APOLLO_RATE_INFO (Result, Why), the request completes with success.
 */
_Use_decl_annotations_
VOID ApolloRateDispatch(PDEVICE_CONTEXT Ctx, WDFREQUEST Request)
{
    WDFFILEOBJECT file = WdfRequestGetFileObject(Request);
    PAPOLLO_FILE f = (file != NULL) ? ApolloGetFile(file) : NULL;
    PVOID in = NULL, out = NULL;
    size_t inLen = 0, outLen = 0;
    APOLLO_RATE_SET req;
    APOLLO_RATE_INFO info;
    ULONG err;
    WDF_REQUEST_PARAMETERS p;

    WDF_REQUEST_PARAMETERS_INIT(&p);
    WdfRequestGetParameters(Request, &p);
    if (p.Parameters.DeviceIoControl.IoControlCode == IOCTL_APOLLO_SET_CLOCK) {
        ApolloClockDispatch(Ctx, Request);      /* [Clock] the clock source and digital formats (clock.c) */
        return;
    }
    if (!NT_SUCCESS(WdfRequestRetrieveInputBuffer(Request, sizeof(req), &in, &inLen)) ||
        !NT_SUCCESS(WdfRequestRetrieveOutputBuffer(Request, sizeof(info), &out, &outLen))) {
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return;
    }
    req = *(PAPOLLO_RATE_SET)in;                /* METHOD_BUFFERED: before the output overwrites it */
    if (req.Version != APOLLO_RATE_VERSION) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }
    if (f != NULL && f->Creator != PsGetCurrentProcess()) {
        file = NULL;                            /* an inherited handle is never an ASIO host's own */
    }
    if (req.Flags & APOLLO_RATE_SET_COMBINED) {
        CHAR why[APOLLO_RATE_WHY_CHARS];

        why[0] = 0;
        err = ApolloAggRateSet(Ctx, req.Rate, req.Flags, file, why, sizeof(why));
        ApolloRateFillInfo(Ctx, &info);
        info.Result = err;
        if (why[0] != 0) {
            (VOID)RtlStringCbCopyA(info.Why, sizeof(info.Why), why);   /* the combined device's outcome */
        }
    } else {
        err = ApolloRateChange(Ctx, req.Rate, req.Flags, file, FALSE);
        ApolloRateFillInfo(Ctx, &info);
        info.Result = err;
    }
    if (outLen < sizeof(info)) {
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);     /* not reached: retrieved with that minimum */
        return;
    }
    RtlCopyMemory(out, &info, sizeof(info));
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(info));
}
