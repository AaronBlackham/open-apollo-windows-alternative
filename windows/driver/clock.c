/*
 * clock.c - [Clock] the clock source of one unit and its digital I/O formats: GET_CLOCK / SET_CLOCK
 * (include\apollo_clock.h), the lock watch in the service pass, and the source going back on after a transport
 * restart. The models, the rules and the evidence are data in clock_plan.h.
 *
 * A source change (SET_CLOCK SOURCE), in the caller's thread (asio.c ApolloEvtIoInCallerContext -> rate.c), with
 * the transport RUNNING, as the G6 peer-clock probe did on both units (spec-twinx 6.9.6) and as open-apollo's Clock
 * Source control does (oa:driver/ua_audio.c:4517-4615):
 *   1. checks (clock_plan.h ApolloClockCheck): a jack on the model, offered by the unit's 0xC08C list, the rate
 *      carried (S/PDIF and ADAT 44.1-96 kHz), the optical port in that format, SR Convert off for S/PDIF; anything
 *      but Internal is UNPROVEN on these units and refused without APOLLO_CLOCK_SET_TRY;
 *   2. DevLock (3 s, else busy): 0xC074 = (rate index << 8) | source, stale acks cleared, doorbell 4; up to 2 s for
 *      the clock event (0xC030 bits 5/22/4), every notification acknowledged meanwhile as the service pass would;
 *   3. the lock (0xC07C [1:0] == 1 at the unit's rate) must then HOLD for APOLLO_CK_SETTLE_MS (1 s; the service
 *      pass runs meanwhile): G6 saw source 0x0A acked and the lock drop afterwards, so an ack alone proves nothing;
 *   4. no ack or no lasting lock: Internal is written back the same way (proven: G6, and every transport start).
 *      A source that never locked is not kept: Wanted goes back to Internal too.
 * DevLock is held up to about 4 s (the failing case); GET_CLOCK, GET_RATE, GET_INFO and the meters do not take it.
 *
 * While on an external source the service pass watches 0xC07C: unlocked for APOLLO_CK_LOSS_MS (1 s) and the service
 * thread writes Internal back (FELL_BACK; Wanted keeps the choice), unless HOLD was asked for. Every transport start
 * writes the Internal clock word (audio.c, unchanged, proven): a start while on an external source (a rate change,
 * an engine recovery) leaves the unit on Internal and the service thread puts the source back once the rate change
 * is over, with the same checks and the same fallback. A connect (bring-up, resume) forgets an external choice: the
 * unit comes up on Internal, as every start before this code did, and nothing unproven runs by itself.
 *
 * Digital formats (SET_CLOCK OPT_IN / OPT_OUT / SRC): one masked mailbox write each (S8 bit 0x10 / 0x08, S1 bit 31),
 * flushed by the next service pass; UNPROVEN, refused without TRY. The connect batch writes S8 = 0x20/0xFF at every
 * connect (both formats S/PDIF), so DigState starts unknown at each connect and the layouts stay what the firmware
 * descriptor says; once a format is set here, the combined layout follows it (apollo_agg.h ApolloAggSlotShownAt).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "apollo.h"
#include <ntstrsafe.h>

/* clock_plan.h and apollo_clock.h keep their own copies (plain C / Windows types) */
C_ASSERT(APOLLO_CK_SPDIF == APOLLO_CLOCK_SRC_SPDIF && APOLLO_CK_ADAT == APOLLO_CLOCK_SRC_ADAT &&
         APOLLO_CK_WORDCLOCK == APOLLO_CLOCK_SRC_WORDCLOCK && APOLLO_CK_INTERNAL == APOLLO_CLOCK_SRC_INTERNAL &&
         APOLLO_CK_INTERNAL == CLOCK_SOURCE_INTERNAL && APOLLO_CK_ACK_BITS == APOLLO_RATE_ACK_BITS);
C_ASSERT(APOLLO_CKCHK_INVALID == APOLLO_CLOCK_ERR_INVALID && APOLLO_CKCHK_NOT_HERE == APOLLO_CLOCK_ERR_NOT_HERE &&
         APOLLO_CKCHK_NOT_OFFERED == APOLLO_CLOCK_ERR_NOT_OFFERED && APOLLO_CKCHK_UNPROVEN == APOLLO_CLOCK_ERR_UNPROVEN &&
         APOLLO_CKCHK_RATE == APOLLO_CLOCK_ERR_RATE && APOLLO_CKCHK_PORT == APOLLO_CLOCK_ERR_PORT &&
         APOLLO_CKCHK_SRC == APOLLO_CLOCK_ERR_SRC);
C_ASSERT(APOLLO_DIGW_OPT_IN == APOLLO_CLOCK_WHAT_OPT_IN && APOLLO_DIGW_OPT_OUT == APOLLO_CLOCK_WHAT_OPT_OUT &&
         APOLLO_DIGW_SRC == APOLLO_CLOCK_WHAT_SRC);
C_ASSERT(APOLLO_DIG_HAS_OPT_IN == APOLLO_CLOCK_DIG_OPT_IN && APOLLO_DIG_HAS_OPT_OUT == APOLLO_CLOCK_DIG_OPT_OUT &&
         APOLLO_DIG_HAS_COAX == APOLLO_CLOCK_DIG_COAX && APOLLO_DIG_HAS_SRC == APOLLO_CLOCK_DIG_SRC);

#define CLOCK_LOCK_TIMEOUT_MS   3000
#define CLOCK_INTERNAL_SETTLE   200ul       /* Internal back: proven, a short look at the lock is enough */

_Use_decl_annotations_
ULONG ApolloClockSourceOf(PDEVICE_CONTEXT Ctx)
{
    return Ctx->ClockExt ? (ULONG)Ctx->ClockExtSource : APOLLO_CK_INTERNAL;
}

_Use_decl_annotations_
ULONG ApolloClockWantedOf(PDEVICE_CONTEXT Ctx)
{
    return Ctx->ClockWantExt ? (ULONG)Ctx->ClockWantSource : APOLLO_CK_INTERNAL;
}

static ULONG ClockSrcs(_In_ PDEVICE_CONTEXT Ctx)
{
    return Ctx->NotifClockSrcsLive != 0 ? Ctx->NotifClockSrcsLive : Ctx->NotifClockSrcs;
}

static VOID ClockSetWhy(_Inout_ PDEVICE_CONTEXT Ctx, _In_z_ _Printf_format_string_ PCSTR Fmt, ...)
{
    va_list ap;

    va_start(ap, Fmt);
    (VOID)RtlStringCbVPrintfA(Ctx->ClockWhy, sizeof(Ctx->ClockWhy), Fmt, ap);
    va_end(ap);
}

static ULONG ClockMsSince(_In_ ULONGLONG T0)
{
    ULONGLONG ms = (KeQueryInterruptTime() - T0) / 10000ULL;

    return ms > 0xFFFFFFFFull ? 0xFFFFFFFFul : (ULONG)ms;
}

/* The wording of a refusal from clock_plan.h's checks. */
static VOID ClockWhyCheck(_Inout_ PDEVICE_CONTEXT Ctx, _In_ int Chk, _In_z_ PCSTR What, _In_ ULONG Source)
{
    switch (Chk) {
    case APOLLO_CKCHK_INVALID:
        ClockSetWhy(Ctx, "%s is not a clock source or value this driver knows", What);
        break;
    case APOLLO_CKCHK_NOT_HERE:
        ClockSetWhy(Ctx, "the %s has no %s", Ctx->Ident.Name != NULL ? Ctx->Ident.Name : "unit", What);
        break;
    case APOLLO_CKCHK_NOT_OFFERED:
        ClockSetWhy(Ctx, "the unit does not offer %s now (its clock sources 0x%04lx)", What, ClockSrcs(Ctx) >> 16);
        break;
    case APOLLO_CKCHK_UNPROVEN:
        ClockSetWhy(Ctx, "%s has not run on these units yet: ask again with try (apollo-diag ... --try)", What);
        break;
    case APOLLO_CKCHK_RATE:
        ClockSetWhy(Ctx, "%s carries 44.1-96 kHz: the unit runs at %s", What, ApolloRateText(Ctx->SampleRate));
        break;
    case APOLLO_CKCHK_PORT:
        ClockSetWhy(Ctx, Source == APOLLO_CK_ADAT || Source == APOLLO_CK_SPDIF ?
                    "the optical input is in the other format: set it to %s first" :
                    "the clock runs from the optical input (%s): set the clock to Internal first",
                    Source == APOLLO_CK_ADAT || Source == APOLLO_CK_SPDIF ? What :
                    ApolloClockName(ApolloClockWantedOf(Ctx)));
        break;
    case APOLLO_CKCHK_SRC:
        ClockSetWhy(Ctx, "SR Convert resamples the S/PDIF input: it cannot also be the clock (turn one off)");
        break;
    default:
        ClockSetWhy(Ctx, "%s refused (%d)", What, Chk);
        break;
    }
}

/* Mode 2, connected, the transport running: APOLLO_CLOCK_ERR_NONE or the reason (Why set). */
static ULONG ClockUnitReady(_In_ PDEVICE_CONTEXT Ctx, _In_ BOOLEAN NeedTransport)
{
    if (Ctx->Stage == ApolloStageGone || ApolloGateClosed(Ctx) || !Ctx->Unit.Active ||
        Ctx->Unit.Value[APOLLO_PARAM_MODE] < APOLLO_MODE_FULL) {
        ClockSetWhy(Ctx, "the unit is not running in Mode 2 (observe, or bring-up only)");
        return APOLLO_CLOCK_ERR_NOT_READY;
    }
    if (!Ctx->Connected || !Ctx->MixerReady || (NeedTransport && !Ctx->TransportRunning)) {
        ClockSetWhy(Ctx, "the unit is not connected with its transport running (still starting, or stopped)");
        return APOLLO_CLOCK_ERR_NOT_READY;
    }
    if (ApolloEngineLost(Ctx)) {
        ClockSetWhy(Ctx, "the unit stopped answering: replug or power-cycle it");
        return APOLLO_CLOCK_ERR_HARDWARE;
    }
    return APOLLO_CLOCK_ERR_NONE;
}

/*
 * The clock word for Source with the transport running (DevLock held), then up to APOLLO_CK_ACK_MS for the clock
 * event, every notification acknowledged as the service pass does (agg.c AggClockProbe, run on both units at G6);
 * after the event the service pass runs every 10 ms and the lock must hold SettleMs without a break within
 * APOLLO_CK_ACK_MS + SettleMs. STATUS_SUCCESS locked; STATUS_IO_TIMEOUT no event; STATUS_TIMEOUT the event came, a
 * lasting lock did not; STATUS_DEVICE_NOT_CONNECTED; STATUS_CANCELLED (D0 exit).
 */
static NTSTATUS ClockWrite(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Source, _In_ ULONG SettleMs, _Out_ PULONG AckMs,
                           _Out_ PULONG Seen)
{
    ULONG word = ApolloClockWord(Ctx->RateIndex, Source), s, v, waited, held = 0, ackAt = 0;
    BOOLEAN acked = FALSE;

    *AckMs = 0;
    *Seen = 0;
    s = ApolloRead(Ctx, REG_NOTIF_STATUS);
    if (s == 0xFFFFFFFF) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    if ((s & APOLLO_CK_ACK_BITS) != 0) {
        ApolloWrite(Ctx, REG_NOTIF_STATUS, s & ~APOLLO_CK_ACK_BITS);   /* a stale ack must not count (rate.c) */
    }
    ApolloLogU(Ctx, "clock: write 0x%04x = 0x%03lx (%s at %s), doorbell 4", REG_NOTIF_CLOCK_CFG, word,
              ApolloClockName(Source), ApolloRateText(Ctx->SampleRate));
    ApolloWrite(Ctx, REG_NOTIF_CLOCK_CFG, word);
    ApolloRateClockWritten(Ctx, Ctx->RateIndex);
    ApolloWrite(Ctx, REG_AX_CONNECT, AX_CONNECT_CLOCK);
    for (waited = 0;; waited += APOLLO_CK_POLL_MS) {
        if (ReadAcquire(&Ctx->StopRequested)) {
            return STATUS_CANCELLED;
        }
        if (!acked) {
            s = ApolloRead(Ctx, REG_NOTIF_STATUS);
            if (s == 0xFFFFFFFF) {
                return STATUS_DEVICE_NOT_CONNECTED;
            }
            if (s != 0) {
                *Seen |= s;
                Ctx->NotifStatusSeen |= s;
                ApolloWrite(Ctx, REG_NOTIF_STATUS, 0);     /* acknowledged, as the service pass does */
                if ((s & APOLLO_CK_ACK_BITS) != 0) {
                    acked = TRUE;
                    ackAt = waited;
                    *AckMs = waited;
                }
            }
        } else {
            ApolloServiceStep(Ctx);                     /* readback, mixer flush, 0xC07C, notifications */
        }
        v = ApolloRead(Ctx, REG_NOTIF_RATE_INFO);
        if (v == 0xFFFFFFFF || ApolloRead(Ctx, REG_FPGA_REV) == 0xFFFFFFFF) {
            return STATUS_DEVICE_NOT_CONNECTED;
        }
        Ctx->NotifRateInfo = v;
        /*
         * An external source counts from its clock event on (before it, 0xC07C still shows the old lock); Internal
         * from the start: going back to it, a lasting lock is all that matters, event or not.
         */
        if (acked || Source == APOLLO_CK_INTERNAL) {
            held = ApolloRateInfoLocked(v, Ctx->RateIndex) ? held + APOLLO_CK_POLL_MS : 0;
            if (held >= SettleMs) {
                return STATUS_SUCCESS;
            }
        }
        if (!acked && waited >= APOLLO_CK_ACK_MS + (Source == APOLLO_CK_INTERNAL ? SettleMs : 0ul)) {
            return STATUS_IO_TIMEOUT;
        }
        if (acked && waited - ackAt >= APOLLO_CK_ACK_MS + SettleMs) {
            return STATUS_TIMEOUT;
        }
        ApolloSleepMs(APOLLO_CK_POLL_MS);
    }
}

/* Internal back after a source that did not hold (DevLock held). */
static NTSTATUS ClockBackToInternal(_In_ PDEVICE_CONTEXT Ctx, _In_z_ PCSTR Why)
{
    ULONG ackMs = 0, seen = 0;
    NTSTATUS st = ClockWrite(Ctx, APOLLO_CK_INTERNAL, CLOCK_INTERNAL_SETTLE, &ackMs, &seen);

    Ctx->ClockExt = FALSE;
    ApolloLogU(Ctx, "clock: back on Internal (%s): 0x%08x, event after %lu ms (0xC030 0x%08lx), 0xC07C 0x%08lx", Why, st,
              ackMs, seen, Ctx->NotifRateInfo);
    return st;
}

/* ------------------------------------------------------------------ */
/* SET_CLOCK                                                           */
/* ------------------------------------------------------------------ */

static ULONG ClockBegin(_In_ PDEVICE_CONTEXT Ctx)
{
    if (InterlockedCompareExchange(&Ctx->ClockChanging, 1, 0) != 0) {
        ClockSetWhy(Ctx, "another clock change is under way on this unit");
        return APOLLO_CLOCK_ERR_BUSY;
    }
    if (ReadAcquire(&Ctx->RateChanging) != 0) {
        InterlockedExchange(&Ctx->ClockChanging, 0);
        ClockSetWhy(Ctx, "a sample-rate change is under way on this unit");
        return APOLLO_CLOCK_ERR_BUSY;
    }
    return APOLLO_CLOCK_ERR_NONE;
}

_IRQL_requires_(PASSIVE_LEVEL)
static ULONG ClockSetSource(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Source, _In_ ULONG Flags)
{
    const APOLLO_CLOCK_MODEL *m = ApolloClockModelFor(Ctx->Ident.Model);
    LONGLONG t = -10000LL * CLOCK_LOCK_TIMEOUT_MS;
    ULONGLONG t0 = KeQueryInterruptTime();
    ULONG err, ackMs = 0, seen = 0, oldSrc;
    NTSTATUS st;
    int chk;

    Ctx->ClockStatus = STATUS_SUCCESS;
    err = ClockUnitReady(Ctx, TRUE);
    if (err != APOLLO_CLOCK_ERR_NONE) {
        return err;
    }
    chk = ApolloClockCheck(m, Source, ClockSrcs(Ctx), Ctx->RateIndex, Ctx->DigState, (Flags & APOLLO_CLOCK_SET_TRY) != 0);
    if (chk != APOLLO_CKCHK_OK) {
        ClockWhyCheck(Ctx, chk, ApolloClockName(Source), Source);
        return (ULONG)chk;
    }
    err = ClockBegin(Ctx);
    if (err != APOLLO_CLOCK_ERR_NONE) {
        return err;
    }
    if (WdfWaitLockAcquire(Ctx->DevLock, &t) == STATUS_TIMEOUT) {
        InterlockedExchange(&Ctx->ClockChanging, 0);
        ClockSetWhy(Ctx, "the unit is busy (its lock was held for 3 s): try again");
        return APOLLO_CLOCK_ERR_BUSY;
    }
    oldSrc = ApolloClockSourceOf(Ctx);
    if (!Ctx->TransportRunning || !Ctx->Connected) {
        err = APOLLO_CLOCK_ERR_NOT_READY;
        ClockSetWhy(Ctx, "the transport stopped meanwhile");
    } else if (Source == oldSrc && ApolloRateInfoLocked(Ctx->NotifRateInfo, Ctx->RateIndex) &&
               ReadNoFence(&Ctx->ClockReapply) == 0) {
        ClockSetWhy(Ctx, "already on %s, locked", ApolloClockName(Source));
    } else {
        ApolloLogU(Ctx, "clock: %s -> %s at %s%s%s", ApolloClockName(oldSrc), ApolloClockName(Source),
                  ApolloRateText(Ctx->SampleRate), (Flags & APOLLO_CLOCK_SET_TRY) ? " (try: not proven on these units)" : "",
                  (Flags & APOLLO_CLOCK_SET_HOLD) ? ", hold" : "");
        st = ClockWrite(Ctx, Source, Source == APOLLO_CK_INTERNAL ? CLOCK_INTERNAL_SETTLE : APOLLO_CK_SETTLE_MS, &ackMs,
                        &seen);
        Ctx->ClockStatus = st;
        if (NT_SUCCESS(st)) {
            Ctx->ClockExt = (Source != APOLLO_CK_INTERNAL);
            Ctx->ClockExtSource = (UCHAR)Source;
            ClockSetWhy(Ctx, "%s, event after %lu ms, locked for %lu ms", ApolloClockName(Source), ackMs,
                        Source == APOLLO_CK_INTERNAL ? CLOCK_INTERNAL_SETTLE : APOLLO_CK_SETTLE_MS);
            Ctx->ClockChanges++;
        } else if (st == STATUS_DEVICE_NOT_CONNECTED || st == STATUS_CANCELLED) {
            err = APOLLO_CLOCK_ERR_HARDWARE;
            ClockSetWhy(Ctx, "the unit stopped answering during the change (0x%08x)", st);
        } else if (Source == APOLLO_CK_INTERNAL) {
            err = APOLLO_CLOCK_ERR_HARDWARE;
            ClockSetWhy(Ctx, "Internal did not %s (0xC07C 0x%08lx): power-cycle the unit",
                        st == STATUS_IO_TIMEOUT ? "acknowledge" : "lock", Ctx->NotifRateInfo);
        } else {
            NTSTATUS back = ClockBackToInternal(Ctx, "the new source did not hold");

            err = NT_SUCCESS(back) ? APOLLO_CLOCK_ERR_LOCK : APOLLO_CLOCK_ERR_HARDWARE;
            if (NT_SUCCESS(back)) {
                ClockSetWhy(Ctx, st == STATUS_IO_TIMEOUT ?
                            "%s was not acknowledged in 2 s (no signal on the input?): back on Internal" :
                            "%s acknowledged but did not stay locked for 1 s (no signal, or not at %s): back on Internal",
                            ApolloClockName(Source), ApolloRateText(Ctx->SampleRate));
            } else {
                ClockSetWhy(Ctx, "%s did not lock, and Internal did not lock again (0xC07C 0x%08lx): power-cycle the unit",
                            ApolloClockName(Source), Ctx->NotifRateInfo);
            }
        }
    }
    if (err == APOLLO_CLOCK_ERR_NONE) {
        Ctx->ClockWantExt = (Source != APOLLO_CK_INTERNAL);
        Ctx->ClockWantSource = (UCHAR)Source;
        Ctx->ClockHold = (Flags & APOLLO_CLOCK_SET_HOLD) ? TRUE : FALSE;
        Ctx->ClockFellBack = FALSE;
        InterlockedExchange(&Ctx->ClockReapply, 0);
        InterlockedExchange(&Ctx->ClockFallbackDue, 0);
        Ctx->ClockUnlockedSince = 0;
    } else if (err == APOLLO_CLOCK_ERR_LOCK) {
        Ctx->ClockWantExt = FALSE;              /* a source that never held is not kept */
        Ctx->ClockFellBack = FALSE;
        InterlockedExchange(&Ctx->ClockReapply, 0);
    }
    WdfWaitLockRelease(Ctx->DevLock);
    InterlockedExchange(&Ctx->ClockChanging, 0);
    Ctx->ClockLastMs = ClockMsSince(t0);
    ApolloLogU(Ctx, "clock: %s %s in %lu ms (%s)", ApolloClockName(Source), err == APOLLO_CLOCK_ERR_NONE ? "done" : "FAILED",
              Ctx->ClockLastMs, Ctx->ClockWhy);
    return err;
}

_IRQL_requires_(PASSIVE_LEVEL)
static ULONG ClockSetDigital(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG What, _In_ ULONG Value, _In_ ULONG Flags)
{
    static const char *const names[4] = { "", "optical input format", "optical output format", "SR Convert" };
    const APOLLO_CLOCK_MODEL *m = ApolloClockModelFor(Ctx->Ident.Model);
    LONGLONG t = -10000LL * CLOCK_LOCK_TIMEOUT_MS;
    ULONG err, setting = 0, value = 0, mask = 0, bit = ApolloDigStateBit(What), was;
    NTSTATUS st;
    int chk;
    PCSTR name = (What < 4) ? names[What] : "?";

    Ctx->ClockStatus = STATUS_SUCCESS;
    if (Value > 1 || bit == 0) {
        ClockSetWhy(Ctx, "%s takes 0 or 1", name);
        return APOLLO_CLOCK_ERR_INVALID;
    }
    err = ClockUnitReady(Ctx, FALSE);
    if (err != APOLLO_CLOCK_ERR_NONE) {
        return err;
    }
    /* both the source it runs on and the one it goes back to (a restart under way) must agree */
    chk = ApolloDigCheck(m, What, Value, ApolloClockSourceOf(Ctx), (Flags & APOLLO_CLOCK_SET_TRY) != 0);
    if (chk == APOLLO_CKCHK_OK) {
        chk = ApolloDigCheck(m, What, Value, ApolloClockWantedOf(Ctx), (Flags & APOLLO_CLOCK_SET_TRY) != 0);
    }
    if (chk != APOLLO_CKCHK_OK) {
        ClockWhyCheck(Ctx, chk, name, APOLLO_CK_INTERNAL);
        return (ULONG)chk;
    }
    err = ClockBegin(Ctx);
    if (err != APOLLO_CLOCK_ERR_NONE) {
        return err;
    }
    if (WdfWaitLockAcquire(Ctx->DevLock, &t) == STATUS_TIMEOUT) {
        InterlockedExchange(&Ctx->ClockChanging, 0);
        ClockSetWhy(Ctx, "the unit is busy (its lock was held for 3 s): try again");
        return APOLLO_CLOCK_ERR_BUSY;
    }
    (VOID)ApolloDigEncode(What, Value, &setting, &value, &mask);
    st = ApolloMixerWriteSetting(Ctx, setting, value, mask);
    Ctx->ClockStatus = st;
    was = Ctx->DigState;
    if (NT_SUCCESS(st)) {
        /* first choice since the connect: the rest is what the connect batch left (S/PDIF both ways, SRC off) */
        Ctx->DigState = (was & APOLLO_DIG_KNOWN) ? was : APOLLO_DIG_KNOWN;
        Ctx->DigState = Value ? (Ctx->DigState | bit) : (Ctx->DigState & ~bit);
        ClockSetWhy(Ctx, "%s: %s (S%lu 0x%08lx/0x%08lx queued; %s)", name,
                    What == APOLLO_DIGW_SRC ? (Value ? "on" : "off") : (Value ? "ADAT" : "S/PDIF"), setting, value, mask,
                    (Flags & APOLLO_CLOCK_SET_TRY) ? "not proven on these units" : "proven");
        ApolloLogU(Ctx, "clock: %s", Ctx->ClockWhy);
    } else {
        err = (st == STATUS_DEVICE_BUSY) ? APOLLO_CLOCK_ERR_BUSY : APOLLO_CLOCK_ERR_NOT_READY;
        ClockSetWhy(Ctx, "%s: the mailbox write was refused (0x%08x)", name, st);
    }
    WdfWaitLockRelease(Ctx->DevLock);
    InterlockedExchange(&Ctx->ClockChanging, 0);
    if (err == APOLLO_CLOCK_ERR_NONE && Ctx->DigState != was && What != APOLLO_DIGW_SRC) {
        ApolloAggRateUnitChanged(Ctx);          /* the combined layout follows the format (no unit lock held) */
    }
    return err;
}

_Use_decl_annotations_
VOID ApolloClockDispatch(PDEVICE_CONTEXT Ctx, WDFREQUEST Request)
{
    PVOID in = NULL, out = NULL;
    size_t inLen = 0, outLen = 0;
    APOLLO_CLOCK_SET req;
    APOLLO_CLOCK_INFO info;
    ULONG err;

    if (!NT_SUCCESS(WdfRequestRetrieveInputBuffer(Request, sizeof(req), &in, &inLen)) ||
        !NT_SUCCESS(WdfRequestRetrieveOutputBuffer(Request, sizeof(info), &out, &outLen))) {
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return;
    }
    req = *(PAPOLLO_CLOCK_SET)in;               /* METHOD_BUFFERED: before the output overwrites it */
    if (req.Version != APOLLO_CLOCK_VERSION) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }
    if (req.What == APOLLO_CLOCK_WHAT_SOURCE) {
        err = ClockSetSource(Ctx, req.Value, req.Flags);
    } else if (req.What <= APOLLO_CLOCK_WHAT_SRC) {
        err = ClockSetDigital(Ctx, req.What, req.Value, req.Flags);
    } else {
        ClockSetWhy(Ctx, "SET_CLOCK What %lu is not known", req.What);
        err = APOLLO_CLOCK_ERR_INVALID;
    }
    Ctx->ClockResult = err;
    ApolloClockFillInfo(Ctx, &info);
    if (outLen < sizeof(info)) {
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);     /* not reached: retrieved with that minimum */
        return;
    }
    RtlCopyMemory(out, &info, sizeof(info));
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(info));
}

/* ------------------------------------------------------------------ */
/* Connect, transport start, the service pass                         */
/* ------------------------------------------------------------------ */

_Use_decl_annotations_
VOID ApolloClockConnected(PDEVICE_CONTEXT Ctx)
{
    if (Ctx->ClockWantExt || Ctx->DigState != 0) {
        ApolloLogU(Ctx, "clock: connected: on Internal with the formats the connect batch wrote (%s and the formats "
                  "chosen before are not put back: set them again)", ApolloClockName(ApolloClockWantedOf(Ctx)));
    }
    Ctx->ClockExt = FALSE;
    Ctx->ClockWantExt = FALSE;
    Ctx->ClockFellBack = FALSE;
    Ctx->ClockHold = FALSE;
    InterlockedExchange(&Ctx->ClockReapply, 0);
    InterlockedExchange(&Ctx->ClockFallbackDue, 0);
    Ctx->ClockUnlockedSince = 0;
    Ctx->ClockMilliHz = 0;
    Ctx->ClockMeasQpc = 0;
    Ctx->DigState = 0;
}

_Use_decl_annotations_
VOID ApolloClockTransportStarted(PDEVICE_CONTEXT Ctx)
{
    if (Ctx->ClockExt) {
        Ctx->ClockExt = FALSE;
    }
    if (Ctx->ClockWantExt && !Ctx->ClockFellBack) {
        InterlockedExchange(&Ctx->ClockReapply, 1);
        ApolloLogU(Ctx, "clock: the transport start wrote Internal: %s goes back on once the start has settled",
                  ApolloClockName(Ctx->ClockWantSource));
    }
    Ctx->ClockUnlockedSince = 0;
    Ctx->ClockMeasQpc = 0;
}

_Use_decl_annotations_
VOID ApolloClockServicePass(PDEVICE_CONTEXT Ctx)
{
    ULONGLONG now;

    if (ReadNoFence(&Ctx->ClockChanging) != 0) {
        return;                                 /* our own write sequence runs this pass (ApolloServiceStep) */
    }
    Ctx->ClockPasses++;
    if ((Ctx->ClockPasses % 10) == 0) {
        Ctx->ClockCfgSeen = ApolloRead(Ctx, REG_NOTIF_CLOCK_CFG);
        Ctx->ClockInfoSeen = ApolloRead(Ctx, REG_NOTIF_CLOCK_INFO);
    }
    if ((Ctx->ClockPasses % 100) == 0) {
        if (ApolloEngineRunning(Ctx) && Ctx->TransportRunning) {
            ULONGLONG pos;
            LONGLONG qpc;
            ULONG raw;
            LARGE_INTEGER freq;

            ApolloPositionGet(Ctx, &pos, &qpc, &raw);
            (VOID)KeQueryPerformanceCounter(&freq);
            if (Ctx->ClockMeasQpc != 0 && qpc > Ctx->ClockMeasQpc && pos >= Ctx->ClockMeasPos && freq.QuadPart > 0) {
                ULONGLONG mhz = (pos - Ctx->ClockMeasPos) * 1000ull * (ULONGLONG)freq.QuadPart /
                                (ULONGLONG)(qpc - Ctx->ClockMeasQpc);

                Ctx->ClockMilliHz = mhz > 0xFFFFFFFFull ? 0xFFFFFFFFul : (ULONG)mhz;
            }
            Ctx->ClockMeasPos = pos;
            Ctx->ClockMeasQpc = qpc;
        } else {
            Ctx->ClockMilliHz = 0;
            Ctx->ClockMeasQpc = 0;
        }
    }
    if (!Ctx->ClockExt || ReadNoFence(&Ctx->RateChanging) != 0) {
        Ctx->ClockUnlockedSince = 0;
        return;
    }
    now = KeQueryInterruptTime();
    if (!ApolloRateInfoLocked(Ctx->NotifRateInfo, Ctx->RateIndex)) {
        if (Ctx->ClockUnlockedSince == 0) {
            Ctx->ClockUnlockedSince = now;
            Ctx->ClockLockLosses++;
            ApolloLogU(Ctx, "clock: %s lost lock (0xC07C 0x%08lx)%s", ApolloClockName(Ctx->ClockExtSource),
                      Ctx->NotifRateInfo, Ctx->ClockHold ? ": hold, staying on it" : "");
        } else if (!Ctx->ClockHold && now - Ctx->ClockUnlockedSince >= APOLLO_CK_LOSS_MS * 10000ull) {
            InterlockedExchange(&Ctx->ClockFallbackDue, 1);
        }
    } else if (Ctx->ClockUnlockedSince != 0) {
        ApolloLogU(Ctx, "clock: %s locked again after %lu ms", ApolloClockName(Ctx->ClockExtSource),
                  ClockMsSince(Ctx->ClockUnlockedSince));
        Ctx->ClockUnlockedSince = 0;
    }
}

/*
 * The service thread after a pass, no lock held: Internal back for a source that stayed unlocked, or the source back
 * on after a transport start. Its own DevLock, waited for (this is the unit's own service thread); never during a
 * rate change or another clock change (the next pass tries again).
 */
_Use_decl_annotations_
VOID ApolloClockServiceAfter(PDEVICE_CONTEXT Ctx)
{
    const APOLLO_CLOCK_MODEL *m;
    ULONG ackMs = 0, seen = 0, want;
    NTSTATUS st;
    int chk;

    if ((ReadNoFence(&Ctx->ClockFallbackDue) == 0 && ReadNoFence(&Ctx->ClockReapply) == 0) ||
        ReadNoFence(&Ctx->RateChanging) != 0 || ReadAcquire(&Ctx->StopRequested) != 0) {
        return;
    }
    if (InterlockedCompareExchange(&Ctx->ClockChanging, 1, 0) != 0) {
        return;
    }
    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (!Ctx->Connected || !Ctx->TransportRunning || ApolloGateClosed(Ctx)) {
        /* nothing to do on a stopped unit: the next start asks again */
    } else if (InterlockedExchange(&Ctx->ClockFallbackDue, 0) != 0) {
        if (Ctx->ClockExt && !ApolloRateInfoLocked(Ctx->NotifRateInfo, Ctx->RateIndex)) {
            want = Ctx->ClockExtSource;
            st = ClockBackToInternal(Ctx, "lock lost for 1 s");
            Ctx->ClockFellBack = TRUE;
            Ctx->ClockFallbacks++;
            ClockSetWhy(Ctx, "%s lost lock for 1 s: back on Internal%s (set the clock again once the signal is back)",
                        ApolloClockName(want), NT_SUCCESS(st) ? "" : ", which did not lock either: power-cycle the unit");
        }
    } else if (InterlockedExchange(&Ctx->ClockReapply, 0) != 0 && Ctx->ClockWantExt && !Ctx->ClockExt) {
        want = Ctx->ClockWantSource;
        m = ApolloClockModelFor(Ctx->Ident.Model);
        chk = ApolloClockCheck(m, want, ClockSrcs(Ctx), Ctx->RateIndex, Ctx->DigState, 1);   /* asked with try before */
        if (chk != APOLLO_CKCHK_OK) {
            Ctx->ClockFellBack = TRUE;
            ClockWhyCheck(Ctx, chk, ApolloClockName(want), want);
            ApolloLogU(Ctx, "clock: %s not put back after the restart: %s", ApolloClockName(want), Ctx->ClockWhy);
        } else {
            st = ClockWrite(Ctx, want, APOLLO_CK_SETTLE_MS, &ackMs, &seen);
            if (NT_SUCCESS(st)) {
                Ctx->ClockExt = TRUE;
                Ctx->ClockExtSource = (UCHAR)want;
                ClockSetWhy(Ctx, "%s back on after the transport restart, locked", ApolloClockName(want));
                ApolloLogU(Ctx, "clock: %s", Ctx->ClockWhy);
            } else if (st != STATUS_CANCELLED && st != STATUS_DEVICE_NOT_CONNECTED) {
                (VOID)ClockBackToInternal(Ctx, "not back after the restart");
                Ctx->ClockFellBack = TRUE;
                Ctx->ClockFallbacks++;
                ClockSetWhy(Ctx, "%s did not lock again after the transport restart: on Internal", ApolloClockName(want));
            }
        }
    }
    WdfWaitLockRelease(Ctx->DevLock);
    InterlockedExchange(&Ctx->ClockChanging, 0);
}

/* ------------------------------------------------------------------ */
/* GET_CLOCK (lock-free)                                               */
/* ------------------------------------------------------------------ */

_Use_decl_annotations_
VOID ApolloClockFillInfo(PDEVICE_CONTEXT Ctx, PAPOLLO_CLOCK_INFO I)
{
    const APOLLO_CLOCK_MODEL *m = ApolloClockModelFor(Ctx->Ident.Model);
    ULONG srcs = ClockSrcs(Ctx), dig = Ctx->DigState, idx = Ctx->RateIndex, rb6 = Ctx->Readback[6];

    RtlZeroMemory(I, sizeof(*I));
    I->Version = APOLLO_CLOCK_VERSION;
    I->Bytes = sizeof(*I);
    I->Model = (ULONG)Ctx->Ident.Model;
    I->Source = ApolloClockSourceOf(Ctx);
    I->Wanted = ApolloClockWantedOf(Ctx);
    I->Sources = (m != NULL) ? m->Sources : APOLLO_CK_BIT(APOLLO_CK_INTERNAL);
    I->Offered = (srcs != 0xFFFFFFFF) ? (srcs >> 16) : 0;
    I->Usable = ApolloClockUsable(m, srcs, idx, dig);
    I->ProvenSources = (m != NULL) ? m->ProvenSources : APOLLO_CK_BIT(APOLLO_CK_INTERNAL);
    I->Rate = Ctx->SampleRate;
    I->RatesOnSource = ApolloClockRatesAllowed(srcs, I->Wanted);
    I->RateInfo = Ctx->NotifRateInfo;
    I->ClockCfg = Ctx->ClockCfgSeen;
    I->ClockInfo = Ctx->ClockInfoSeen;
    I->MeasuredMilliHz = Ctx->ClockMilliHz;
    I->Result = Ctx->ClockResult;
    I->Status = Ctx->ClockStatus;
    I->LastMs = Ctx->ClockLastMs;
    I->Changes = Ctx->ClockChanges;
    I->Fallbacks = Ctx->ClockFallbacks;
    I->LockLosses = Ctx->ClockLockLosses;
    I->Digital = (m != NULL) ? m->Digital : 0;
    I->ProvenDigital = (m != NULL) ? m->ProvenDigital : 0;
    I->AdatIn = ApolloDigAdatChannels(m, 1, idx, dig);
    I->AdatOut = ApolloDigAdatChannels(m, 0, idx, dig);
    I->SpdifIn = ApolloDigSpdifChannels(m, 1, idx, dig);
    I->SpdifOut = ApolloDigSpdifChannels(m, 0, idx, dig);
    if (ApolloRateInfoLocked(Ctx->NotifRateInfo, idx)) {
        I->Flags |= APOLLO_CLOCK_F_LOCKED;
    }
    if (ReadNoFence(&Ctx->ClockChanging) != 0) {
        I->Flags |= APOLLO_CLOCK_F_CHANGING;
    }
    if (Ctx->ClockFellBack) {
        I->Flags |= APOLLO_CLOCK_F_FELL_BACK;
    }
    if (Ctx->ClockHold) {
        I->Flags |= APOLLO_CLOCK_F_HOLD;
    }
    if (ReadNoFence(&Ctx->ClockReapply) != 0) {
        I->Flags |= APOLLO_CLOCK_F_REAPPLY;
    }
    if (dig & APOLLO_DIG_KNOWN) {
        I->Flags |= APOLLO_CLOCK_F_DIG_KNOWN;
    }
    if (dig & APOLLO_DIG_IN_ADAT) {
        I->Flags |= APOLLO_CLOCK_F_IN_ADAT;
    }
    if (dig & APOLLO_DIG_OUT_ADAT) {
        I->Flags |= APOLLO_CLOCK_F_OUT_ADAT;
    }
    if (dig & APOLLO_DIG_SRC) {
        I->Flags |= APOLLO_CLOCK_F_SRC;
    }
    if (Ctx->ReadbackValid && ((rb6 >> 30) & 1u) != 0) {
        I->Flags |= APOLLO_CLOCK_F_RB_LOCK_VALID;
        if ((rb6 >> 31) & 1u) {
            I->Flags |= APOLLO_CLOCK_F_RB_LOCKED;
        }
    }
    if (ApolloAggRateMember(Ctx)) {
        I->Flags |= APOLLO_CLOCK_F_COMBINED;
        if (ApolloAggIsTimeline(Ctx)) {
            I->Flags |= APOLLO_CLOCK_F_TIMELINE;
        }
    }
    (VOID)RtlStringCbCopyA(I->Why, sizeof(I->Why), Ctx->ClockWhy);
}
