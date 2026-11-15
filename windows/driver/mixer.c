/*
 * mixer.c - mixer-settings mailbox, hardware readback, preamp parameters,
 * and the 10 ms DSP service thread.
 *
 * Follows docs/spec-dsp-mixer.md sections 3 and 5 and open-apollo's
 * ua_mixer_flush_settings() (mixer_mailbox, the default): a setting is
 * written once with its mask, then once as zeros on the next flush, and
 * SEQ_WR is bumped once per batch. Persistent masks left PAD, 48V and
 * Mic/Line dead on Linux.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "apollo.h"
#include "preamp_plan.h"        /* [Twin X G5] the preamp controls per model */

/* [Twin X G5] preamp_plan.h keeps its own copies of the SET_INPUT_PARAM codes (plain C) */
C_ASSERT(APOLLO_PRE_MIC_LINE == APOLLO_PARAM_MIC_LINE && APOLLO_PRE_PAD == APOLLO_PARAM_PAD &&
         APOLLO_PRE_LINK == APOLLO_PARAM_LINK && APOLLO_PRE_48V == APOLLO_PARAM_48V &&
         APOLLO_PRE_LOWCUT == APOLLO_PARAM_LOWCUT && APOLLO_PRE_PHASE == APOLLO_PARAM_PHASE &&
         APOLLO_PRE_HIZ_IMP == APOLLO_PARAM_HIZ_IMP && APOLLO_PRE_GAIN_DB == APOLLO_PARAM_GAIN_DB);
C_ASSERT(APOLLO_RB_WORDS_MIN <= APOLLO_READBACK_WORDS);

/*
 * strips.c: a host write to a 48V bit while the strips' 48V resync holds it off means "leave it
 * as the host set it" (docs/STRIPS.md 10.7). Declared here rather than in apollo.h to keep this
 * hook out of the shared header; DevLock held.
 */
VOID ApolloStripsNoteSettingWrite(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Index, _In_ ULONG Mask);

static ULONG ApolloMixerSettingReg(_In_ ULONG Index)
{
    if (Index <= 15) {
        return REG_MIXER_BASE + 0xB4 + Index * MIXER_SETTING_STRIDE;
    }
    if (Index <= 31) {
        return REG_MIXER_BASE + 0xBC + Index * MIXER_SETTING_STRIDE;
    }
    return REG_MIXER_BASE + 0xC0 + Index * MIXER_SETTING_STRIDE;
}

/* Connect-time init (ua_audio.c:3237-3308). The x8p never inherits its DSP. */
VOID ApolloMixerInit(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONG seqRd = ApolloRead(Ctx, REG_MIXER_SEQ_RD);

    Ctx->MixerSeqWr = seqRd;
    ApolloWrite(Ctx, REG_MIXER_SEQ_WR, seqRd);
    RtlZeroMemory(Ctx->MixerVal, sizeof(Ctx->MixerVal));
    RtlZeroMemory(Ctx->MixerMask, sizeof(Ctx->MixerMask));
    Ctx->MixerZeroPending = 0;
    Ctx->ReadbackValid = FALSE;
    Ctx->MixerReady = TRUE;

    /*
     * "capture routing requires settings 1-37 = 0x20/0xFF (Windows BAR0
     * capture) plus setting[35] = 0x05". Setting 2 (monitor core) is
     * skipped. Purpose partly unproven. On the x8p several of these low
     * bytes are preamp fields, so the batch is chosen by the MixerInitBatch
     * registry value (strips.c, docs/STRIPS.md section 6): absent = legacy,
     * the full batch every earlier build sent.
     */
    ApolloStripsMixerInitBatch(Ctx);
    ApolloLogU(Ctx, "mixer init: SEQ %lu, capture-routing batch queued", seqRd);
    ApolloCuesReplay(Ctx);              /* [Monitor] the cue sends set earlier, if any (cues.c) */
    ApolloClockConnected(Ctx);          /* [Clock] the batch rewrote S8: formats back to the unit's, clock Internal */
    ApolloPanelConnected(Ctx);          /* [Clock] the front panel is decoded afresh from the next readback */
}

NTSTATUS ApolloMixerWriteSetting(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Index,
                                 _In_ ULONG Value, _In_ ULONG Mask)
{
    if (Index >= MIXER_NUM_SETTINGS) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!Ctx->MixerReady) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (ApolloPluginAnalogConflict(Ctx, Index, Mask)) {
        return STATUS_DEVICE_BUSY;      /* [Ins] a Unison lease owns (or a failed one blocked) a field of this write */
    }
    ApolloStripsNoteSettingWrite(Ctx, Index, Mask);     /* the 48V resync yields to the host */
    Ctx->MixerVal[Index] = (Ctx->MixerVal[Index] & ~Mask) | (Value & Mask);
    Ctx->MixerMask[Index] |= Mask;
    Ctx->MixerDirty = TRUE;
    return STATUS_SUCCESS;
}

VOID ApolloMixerFlush(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONG i, seqRd, reg, wordA, wordB;
    ULONGLONG zeroNext = 0;

    if (!Ctx->MixerReady || !Ctx->MixerDirty) {
        return;
    }
    seqRd = ApolloRead(Ctx, REG_MIXER_SEQ_RD);
    if (seqRd != Ctx->MixerSeqWr) {
        return;                         /* DSP still on the previous batch */
    }
    for (i = 0; i < MIXER_NUM_SETTINGS; i++) {
        reg = ApolloMixerSettingReg(i);
        if (Ctx->MixerMask[i] == 0) {
            if (Ctx->MixerZeroPending & (1ULL << i)) {
                ApolloWrite(Ctx, reg, 0);       /* the one zero pass after a write */
                ApolloWrite(Ctx, reg + 4, 0);
            }
            continue;                           /* mask 0 keeps firmware defaults */
        }
        wordA = ((Ctx->MixerMask[i] & 0xFFFF) << 16) | (Ctx->MixerVal[i] & 0xFFFF);
        wordB = (((Ctx->MixerMask[i] >> 16) & 0xFFFF) << 16) | ((Ctx->MixerVal[i] >> 16) & 0xFFFF);
        ApolloWrite(Ctx, reg, wordA);
        ApolloWrite(Ctx, reg + 4, wordB);
        Ctx->MixerMask[i] = 0;
        zeroNext |= 1ULL << i;
    }
    Ctx->MixerSeqWr++;
    ApolloWrite(Ctx, REG_MIXER_SEQ_WR, Ctx->MixerSeqWr);
    Ctx->MixerZeroPending = zeroNext;
    Ctx->MixerDirty = (zeroNext != 0);
}

/* ------------------------------------------------------------------ */
/* Preamp parameters (spec-dsp-mixer 5.2, 5.3)                         */
/* ------------------------------------------------------------------ */

/*
 * [Twin X G5] Per model, from preamp_plan.h: the x8p exactly as before (8 preamps, the same
 * writes and errors, preamp-plan-test compares every input with the pre-G5 code); the Twin X Gen 1
 * its 2 preamps (S0 flags, link from input 1, GainA/B/C lanes 0-1) and the Hi-Z impedance of
 * input 1 (S13 nibble 0). A preamp the model does not have: STATUS_INVALID_PARAMETER, as before;
 * a control it does not have (Hi-Z on input 2, any other code): STATUS_NOT_SUPPORTED.
 */
NTSTATUS ApolloSetInputParam(_In_ PDEVICE_CONTEXT Ctx, _In_ PAPOLLO_INPUT_PARAM P)
{
    APOLLO_PREAMP_WRITE w[APOLLO_PRE_WRITES_MAX];
    unsigned long n = 0, k;
    NTSTATUS status = STATUS_SUCCESS;
    int r = ApolloPreampEncode(ApolloPreampModelFor(Ctx->Ident.Model), P->Preamp, P->Param, P->Value, w, &n);

    if (r == APOLLO_PRE_BAD_VALUE) {
        return STATUS_INVALID_PARAMETER;
    }
    if (r != APOLLO_PRE_OK || n == 0) {
        return STATUS_NOT_SUPPORTED;
    }
    /* Gain: A, B, C under one lock, so they share one mailbox commit. */
    for (k = 0; k < n && k < APOLLO_PRE_WRITES_MAX && NT_SUCCESS(status); k++) {
        status = ApolloMixerWriteSetting(Ctx, w[k].Setting, w[k].Value, w[k].Mask);
    }
    return status;
}

/* ------------------------------------------------------------------ */
/* Monitor section (spec-dsp-mixer 6.1 C9-C12)                         */
/* ------------------------------------------------------------------ */

/*
 * [Monitor] Per model, from monitor_plan.h (mon-plan-test proves the x8p's writes and errors are what this
 * function produced before, for every code it had; the x8p's source also takes CUE 3 and 4). ALT speakers,
 * cue MIX/mono and the sources are mailbox writes; the cue sends and [Mix] the monitor mix go to cues.c (DSP bus
 * coefficients, param 1/2/5/6 and 0).
 * The plan filter (ioctl.c, bringup_plan.h) has already refused codes the unit's plan does not take.
 */
NTSTATUS ApolloSetMonitorParam(_In_ PDEVICE_CONTEXT Ctx, _In_ PAPOLLO_MONITOR_PARAM P)
{
    APOLLO_MON_WRITE w;
    int r = ApolloMonEncode(ApolloMonModelFor(Ctx->Ident.Model), P->Param, P->Value, &w);

    if (r == APOLLO_MON_BAD_VALUE) {
        return STATUS_INVALID_PARAMETER;
    }
    if (r != APOLLO_MON_OK) {
        return STATUS_NOT_SUPPORTED;
    }
    ApolloPanelNoteWrite(Ctx, P->Param, P->Value);  /* [Clock] its echo in the readback is the host's, not the panel's */
    if (w.Kind == APOLLO_MONW_SETTING) {
        return ApolloMixerWriteSetting(Ctx, w.Setting, w.Value, w.Mask);
    }
    return ApolloCuesApply(Ctx, &w);
}

/* ------------------------------------------------------------------ */
/* Service thread (spec-bringup 3.7 G6)                                */
/* ------------------------------------------------------------------ */

static VOID ApolloServicePass(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONG s, i;

    if (!Ctx->Connected) {
        return;
    }
    s = ApolloRead(Ctx, REG_NOTIF_STATUS);
    if (s == 0xFFFFFFFF) {
        ApolloLogU(Ctx, "service: notification status all ones, device gone");
        Ctx->Stage = ApolloStageGone;
        Ctx->Connected = FALSE;
        return;
    }
    Ctx->NotifStatusSeen |= s;
    Ctx->NotifRateInfo = ApolloRead(Ctx, REG_NOTIF_RATE_INFO);

    if (ApolloRead(Ctx, REG_MIXER_RB_STATUS) == 1) {
        for (i = 0; i < APOLLO_READBACK_WORDS; i++) {
            Ctx->Readback[i] = ApolloRead(Ctx, REG_MIXER_RB_DATA + i * 4);
        }
        ApolloWrite(Ctx, REG_MIXER_RB_STATUS, 0);
        Ctx->ReadbackValid = TRUE;
        Ctx->ReadbackSeq++;
    }
    ApolloPanelServicePass(Ctx);                /* [Clock] knob, buttons, talkback: what changed and who changed it */
    ApolloClockServicePass(Ctx);                /* [Clock] an external source's lock, 0xC074 / 0xC084, the measured rate */
    if (Ctx->ServicePasses >= 5) {
        ApolloMixerFlush(Ctx);
    }
    /*
     * The 1 ms engine owns SAMPLE_POS and the playback ring; this is the fallback
     * without it. Not while lost: a failed recovery deleted the timer, and the
     * device is not to be read until a restart succeeds.
     */
    if (Ctx->TransportRunning && !ApolloEngineRunning(Ctx) && !ApolloEngineLost(Ctx)) {
        ApolloPositionUpdate(Ctx, NULL);
        ApolloPlaybackZeroAhead(Ctx);
    }
    if (s != 0) {
        ApolloWrite(Ctx, REG_NOTIF_STATUS, 0);  /* last: write 0, not W1C */
    }
    ApolloStripsService(Ctx);                   /* queues the strip pass once armed + settled */
    ApolloLatencyRefresh(Ctx);                  /* [Lat] the reported latencies, published when they moved */
    Ctx->ServicePasses++;
}

/*
 * [Rate] One pass from a caller that holds DevLock through a long wait (rate.c: the clock change and
 * its hold), so the notification block, the readback and the mixer batch are serviced meanwhile as the
 * blocked service thread would.
 */
VOID ApolloServiceStep(_In_ PDEVICE_CONTEXT Ctx)
{
    ApolloServicePass(Ctx);
}

static KSTART_ROUTINE ApolloServiceThread;

static VOID ApolloServiceThread(_In_ PVOID Context)
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;
    LARGE_INTEGER period;

    period.QuadPart = -10LL * 10000;    /* 10 ms */
    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);

    for (;;) {
        NTSTATUS w = KeWaitForSingleObject(&ctx->ServiceStop, Executive, KernelMode, FALSE, &period);

        if (w == STATUS_SUCCESS || ApolloGateClosed(ctx)) {
            break;
        }
        WdfWaitLockAcquire(ctx->DevLock, NULL);
        ApolloServicePass(ctx);
        WdfWaitLockRelease(ctx->DevLock);
        /* [G7] a start ratio the combined device kept: written here (PASSIVE, DevLock released, try-lock only) */
        ApolloAggServicePoll();
        /* [Clock] no lock held: an external clock that fell back or comes back on (its own DevLock), and what the
           monitor link brings to the monitor unit (that unit's DevLock, never waited for) */
        ApolloClockServiceAfter(ctx);
        ApolloPanelServiceAfter(ctx);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS ApolloServiceStart(_In_ PDEVICE_CONTEXT Ctx)
{
    HANDLE h;
    NTSTATUS status;
    OBJECT_ATTRIBUTES oa;

    if (InterlockedCompareExchange(&Ctx->ServiceRunning, 1, 0) != 0) {
        return STATUS_SUCCESS;
    }
    KeClearEvent(&Ctx->ServiceStop);
    Ctx->ServicePasses = 0;
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    status = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, &oa, NULL, NULL, ApolloServiceThread, Ctx);
    if (!NT_SUCCESS(status)) {
        InterlockedExchange(&Ctx->ServiceRunning, 0);
        return status;
    }
    status = ObReferenceObjectByHandle(h, SYNCHRONIZE, *PsThreadType, KernelMode,
                                       (PVOID *)&Ctx->ServiceThread, NULL);
    ZwClose(h);
    if (!NT_SUCCESS(status)) {
        /* Cannot wait for it without a reference: stop it and hope. */
        KeSetEvent(&Ctx->ServiceStop, IO_NO_INCREMENT, FALSE);
        Ctx->ServiceThread = NULL;
        InterlockedExchange(&Ctx->ServiceRunning, 0);
    }
    return status;
}

/* Must not be called with DevLock held: the thread takes it. */
VOID ApolloServiceStop(_In_ PDEVICE_CONTEXT Ctx)
{
    if (InterlockedCompareExchange(&Ctx->ServiceRunning, 0, 1) != 1) {
        return;
    }
    KeSetEvent(&Ctx->ServiceStop, IO_NO_INCREMENT, FALSE);
    if (Ctx->ServiceThread != NULL) {
        KeWaitForSingleObject(Ctx->ServiceThread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Ctx->ServiceThread);
        Ctx->ServiceThread = NULL;
    }
}
