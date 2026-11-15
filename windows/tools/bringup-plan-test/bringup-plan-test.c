/*
 * bringup-plan-test.c - offline checks of driver\bringup_plan.h (gate G3, docs/spec-twinx.md 2,
 * 3, 4.3, 6.9.3). No device, no Windows headers: runs in tools\build-all.cmd.
 *
 *   1. The x8p plan is the path every release before G3 ran: each value against the constant or
 *      the code it replaces (regs.h, the pre-G3 strips.c batch list, audio.c's P2P write).
 *   2. The Twin X Gen 1 plan matches the spec's step table, value for value.
 *   3. The firmware IO descriptors of Aaron's Twin X as UA left them (gate G2, step-g2.log
 *      Nov 14 2026, "STALE desc" lines): counts, MON slots, every slot's name.
 *   4. Channel names as the kext spells them; 5. step names; 6. clock-source masks; 7. the
 *      per-unit Mode rules with the plans.
 *   8. [G4] The Twin X in Mode 2: its endpoints, the settings it takes, the steps each start
 *      runs (GET_INFO BringupApplies), the rate confirmation; and none of it reaches the x8p.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <string.h>
#include "..\..\driver\regs.h"
#include "..\..\driver\bringup_plan.h"
#include "..\..\driver\apollo_unit.h"

static int g_checks, g_fail;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        g_checks++;                                             \
        if (!(cond)) {                                          \
            g_fail++;                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

/* ------------------------------------------------------------------ */
/* 1. The x8p: unchanged                                               */
/* ------------------------------------------------------------------ */

/* strips.c ApolloStripsMixerInitBatch before G3 (builds\base-pre-g3), as it was written. */
static int PreG3X8pBatchWrites(unsigned long Mode, unsigned long S)
{
    static const unsigned char kX8pSkip[] = { 2, 3, 12, 13, 14, 16, 17, 18, 19, 20, 21, 22, 23,
                                              24, 25, 26, 27, 28, 29, 30, 31 };
    unsigned int k;
    int skip;

    if (Mode == 0 || S < 1 || S >= MIXER_NUM_SETTINGS) {
        return 0;
    }
    skip = (S == 2 || S == 35);
    if (Mode == 1) {
        for (k = 0; k < sizeof(kX8pSkip); k++) {
            skip = skip || (S == kX8pSkip[k]);
        }
    }
    return !skip;
}

static void TestX8p(void)
{
    const APOLLO_BRINGUP_PLAN *p = ApolloBringupPlanFor(APOLLO_MODEL_X8P);
    unsigned long s, m, c, n1 = 0, n2 = 0;

    CHECK(p != 0, "x8p has a plan");
    if (p == 0) {
        return;
    }
    CHECK(p->Model == APOLLO_MODEL_X8P && p->AudioExtension == 0 && p->MaxMode == 2,
          "x8p: ring-connect path, Mode up to 2");
    /* device.c ApolloBringup (x8p body unchanged): no boot transport, E11 at the cold probe only */
    CHECK(p->BootTransport == 0 && p->RingConnectAtProbe == 1 && p->WarmBootTransport == 0,
          "x8p: no boot transport, ring connect at the cold probe, WarmBootTransport ignored");
    CHECK(p->RingConnectAtConnect == 0 && p->ConnectConsumeMs == 0,
          "x8p: no ring connect in the audio connect, no consumption check (as before)");
    /* audio.c ApolloAceface: ACEFACE_RETRIES x ACEFACE_POLLS x ACEFACE_POLL_MS */
    CHECK(p->AcefaceTries == ACEFACE_RETRIES && p->AcefacePolls == ACEFACE_POLLS &&
          p->AcefacePollMs == ACEFACE_POLL_MS, "x8p: ACEFACE 20 x 10 x 100 ms");
    /* audio.c ApolloAudioAllocate / ApolloTransportStart */
    CHECK(p->CountsFromDescriptor == 0 && p->LegacyPlay == X8P_PLAY_CHANNELS && p->LegacyRec == X8P_REC_CHANNELS &&
          p->LegacyPlay == 34 && p->LegacyRec == 32, "x8p: legacy layout 34 / 32");
    CHECK(p->P2pRouteAtStart == 1, "x8p: AX_P2P_ROUTE written at the transport start (step 13, as before)");
    CHECK(p->AxStart == AX_CTRL_START && p->AxStart == 0xF, "x8p: AX_CONTROL 0xF");
    CHECK(p->FrameCfg48k == X8P_SAFETY_FRAMES_48K && p->FrameCfg48k == 8, "x8p: FRAME_CFG 8 at 48 kHz");
    CHECK(p->ConfirmRate == 0, "x8p: no rate confirmation poll (as before)");
    CHECK(ApolloPlanBufFrames(p->LegacyPlay, p->LegacyRec) == AUDIO_RING_FRAMES, "x8p: 8192-frame ring");
    CHECK(ApolloPlanDmaEnables(6) == 0x7Eul && (DMA_CTRL_GLOBAL | ApolloPlanDmaEnables(6)) == 0x7Ful,
          "x8p: 6 DSP enables 0x7E, DMA_CTRL 0x7F (measured Nov 14)");

    /* The batch: exactly what strips.c wrote before G3, in every mode */
    for (m = 0; m <= 2; m++) {
        for (s = 0; s < 64; s++) {
            CHECK(ApolloPlanBatchWrites(p, m, s) == PreG3X8pBatchWrites(m, s),
                  "x8p batch mode %lu setting %lu: plan %d, pre-G3 %d", m, s, ApolloPlanBatchWrites(p, m, s),
                  PreG3X8pBatchWrites(m, s));
        }
    }
    for (s = 0; s < MIXER_NUM_SETTINGS; s++) {
        n1 += (unsigned long)ApolloPlanBatchWrites(p, 1, s);
        n2 += (unsigned long)ApolloPlanBatchWrites(p, 2, s);
    }
    CHECK(n1 == 15 && n2 == 35, "x8p batch: x8p-safe 15 settings, legacy 35 (got %lu, %lu)", n1, n2);
    CHECK(strcmp(p->BatchName, "x8p-safe, preamp fields skipped") == 0, "x8p batch log text unchanged");

    /* Everything else runs, every IOCTL as before */
    CHECK(p->Endpoints && p->Asio && p->Strips, "x8p: endpoints, ASIO, strips");
    CHECK(p->IoctlMode1 == APOLLO_IOCTL_CLASS_ALL && p->IoctlMode2 == APOLLO_IOCTL_CLASS_ALL, "x8p: all IOCTLs");
    for (m = 1; m <= 2; m++) {
        for (c = 0; c <= APOLLO_IOCTL_CLASS_ALL; c = c ? c << 1 : 1) {
            CHECK(ApolloPlanIoctlAllowed(p, m, c), "x8p Mode %lu: class 0x%lX allowed", m, c);
        }
    }
    CHECK(p->ObserveDmaOff == 0 && p->ServiceAfterFailure == 1 && p->NamesFailStep == 0,
          "x8p: observe untouched, service after a failure (TRANSPORT_START retries), no step names");

    /* [G4] nothing new reaches the x8p */
    CHECK(p->RateConfirmMs == 0, "x8p: no rate confirmation wait (ConfirmRate 0, as before)");
    CHECK(p->StepsCold == 0 && p->StepsWarm == 0 && p->StepsBoot == 0 && p->StepsConnect == 0,
          "x8p: records no steps");
    for (m = 0; m < 2; m++) {
        for (c = 0; c < 2; c++) {
            CHECK(ApolloPlanBstepApplies(p, (int)m, (int)c, 0) == 0 && ApolloPlanBstepApplies(p, (int)m, (int)c, 1) == 0,
                  "x8p: no steps apply (warm %lu boot %lu)", m, c);
        }
    }
    CHECK(p->MonitorParams == APOLLO_PLAN_MON_ANY && p->InputParams == 1 && p->RawMixerSettings == 1,
          "x8p: every setting as before");
    for (s = 0; s < 0x200; s++) {
        CHECK(ApolloPlanMonitorParamAllowed(p, s), "x8p: monitor param 0x%lX goes on to mixer.c (as before)", s);
    }
    /* [Endpoints] Nov 15: endpoint_plan.h's table (every output, loopback, MIC/LINE 1-8; VIRTUAL/S/PDIF in opt-in) */
    CHECK(p->EndpointRows == 22 && p->EndpointDefault == 0x1FFFF && p->EndpointDefault == APOLLO_DEF_ENDPOINTS_X8P &&
          p->EndpointRows == APOLLO_X8P_EP_LIST_COUNT && p->EndpointDefault == APOLLO_X8P_EP_LIST_DEFAULT &&
          p->AdcFrames == 10, "x8p: 22 endpoint rows, default 0x1FFFF (= the list), ADC 10 frames");
    CHECK(ApolloParamDefault(APOLLO_MODEL_X8P, APOLLO_PARAM_ENABLED_ENDPOINTS) == 0x1FFFF, "x8p Endpoints default 0x1FFFF");
    CHECK(ApolloAddTimeEndpoints(1, 0x0014, 0, 0) == APOLLO_X8P_EP_ALL && ApolloAddTimeEndpoints(1, 0x0014, 1, 0x3FF) ==
          APOLLO_X8P_EP_ALL && ApolloAddTimeModel(1, 0x0014) == APOLLO_MODEL_X8P,
          "x8p: device add creates every row's circuit object (its value adds them)");
}

/* ------------------------------------------------------------------ */
/* 2. The Twin X Gen 1: docs/spec-twinx.md section 2                   */
/* ------------------------------------------------------------------ */

static void TestTwinX(void)
{
    const APOLLO_BRINGUP_PLAN *p = ApolloBringupPlanFor(APOLLO_MODEL_TWINX);
    static const unsigned char skip[] = { 2, 3, 11, 13, 14, 15, 16, 17, 23 };   /* spec-twinx 4.3 */
    unsigned long s, m, frames, n = 0;
    unsigned int k;

    CHECK(p != 0, "Twin X has a plan");
    if (p == 0) {
        return;
    }
    CHECK(p->Model == APOLLO_MODEL_TWINX && p->AudioExtension == 1, "Twin X: AudioExtension path (0x99 >= 64)");
    CHECK(p->MaxMode == 2, "Twin X: Mode 2 in this build (G4)");

    /* E7 boot transport */
    CHECK(p->BootTransport == 1, "E7: boot transport on a cold start");
    CHECK(p->BootPlay == 3 && p->BootRec == 2, "E7: legacy fallback counts 3 / 2 (UA_DEFAULT_PLAY_CH + 1, UA_DEFAULT_REC_CH)");
    frames = ApolloPlanBufFrames(p->BootPlay, p->BootRec);
    CHECK(frames == 8192, "E7: frames %lu (8192)", frames);
    CHECK(frames - 1 == 0x1FFF, "E7: AX_BUF_SIZE 0x1FFF");
    CHECK(((frames - 1) * p->BootPlay) >> 10 == 23, "E7: AX_DMA_BLKSZ 23");
    CHECK(p->BootP2pRoute == 1 && (0x100 | (p->BootPlay - 1)) == 0x102, "E7: AX_P2P_ROUTE 0x102 in the boot transport");
    CHECK(p->BootFrameCfg == 0x10, "E7: FRAME_CFG 0x10");
    CHECK(p->BootAxStart == 0x20F, "E7: AX_CONTROL 0x20F (start + extended mode)");
    CHECK(p->BootStartPolls == 10, "E7: FRAME_CTR polled up to 10 times");
    /* E8 early ACEFACE, about 1 s */
    CHECK(p->EarlyAcefaceTries == 2 && p->EarlyAcefacePolls == 10 && p->EarlyAcefacePollMs == 50,
          "E8: 2 tries x 10 polls x 50 ms");
    CHECK(p->EarlyAcefaceTries * p->EarlyAcefacePolls * p->EarlyAcefacePollMs == 1000, "E8: about 1 s");
    CHECK(p->PulseSettleMs == 50, "E7: 50 ms after the DMA reset pulse");
    /* E11 not here; F: WarmBootTransport honoured */
    CHECK(p->RingConnectAtProbe == 0, "E11: no ring connect at the probe (deferred to G5)");
    CHECK(p->WarmBootTransport == 1, "warm: WarmBootTransport=1 runs E7 on a warm start (spec notes)");
    /* G5 */
    CHECK(p->AcefaceTries == 20 && p->AcefacePolls == 10 && p->AcefacePollMs == 100,
          "G5: full ACEFACE 20 x 10 x 100 ms when the early one did not answer (warm)");
    CHECK(p->RingConnectAtConnect == 1, "G5: ring connect + dated finalize on every connect");
    CHECK(p->ConnectConsumeMs == 50, "G5: each DSP's command POSITION reaches WRITE_PTR within 50 ms");
    /* Transport */
    CHECK(p->CountsFromDescriptor == 1 && p->LegacyPlay == 0 && p->LegacyRec == 0,
          "transport: descriptor counts, no legacy layout");
    CHECK(p->RateConfirmMs == 2000, "[G4] rate confirmation: up to 2 s, notifications acknowledged meanwhile");
    CHECK(p->P2pRouteAtStart == 0, "transport: no AX_P2P_ROUTE write (io_map layout, kext rule)");
    CHECK(p->AxStart == AX_CTRL_START && p->AxStart == 0xF, "transport: AX_CONTROL 0xF");
    CHECK(p->FrameCfg48k == X8P_SAFETY_FRAMES_48K, "transport: FRAME_CFG 8 at 48 kHz");
    CHECK(p->ConfirmRate == 1, "transport: 48 kHz confirmed, SAMPLE_POS checked, live clock mask");
    CHECK(ApolloPlanClockWord(RATE_IDX_48000, CLOCK_SOURCE_INTERNAL) == 0x20C, "clock write 0xC074 = 0x20C");
    CHECK(ApolloPlanRateLocked(0x201, RATE_IDX_48000), "0x201 = 48 kHz locked");
    CHECK(!ApolloPlanRateLocked(0x101, RATE_IDX_48000), "0x101 (UA left 44.1 kHz) is not 48 kHz");
    CHECK(!ApolloPlanRateLocked(0x200, RATE_IDX_48000), "0x200 is not locked");
    CHECK(!ApolloPlanRateLocked(0xFFFFFFFFul, RATE_IDX_48000), "all ones is not locked");
    /* E4: enables */
    CHECK(ApolloPlanDmaEnables(2) == 0x6ul && (DMA_CTRL_GLOBAL | ApolloPlanDmaEnables(2)) == 0x7ul,
          "E4: Duo enables 0x6, DMA_CTRL 0x7 after bring-up (Appendix A)");
    CHECK(ApolloPlanDmaEnables(4) == 0x1Eul && (DMA_CTRL_GLOBAL | ApolloPlanDmaEnables(4)) == 0x1Ful,
          "E4: Quad enables 0x1E, DMA_CTRL 0x1F");
    CHECK(ApolloPlanDmaEnables(0) == 0 && ApolloPlanDmaEnables(9) == 0 && ApolloPlanDmaEnables(8) == 0x1FEul,
          "enables: bounds");
    CHECK((ApolloPlanDmaEnables(8) & DMA_CTRL_STROBES) == 0, "enables never touch a strobe");

    /* 4.3 batch */
    for (s = 0; s < 64; s++) {
        int want = 0;

        for (k = 0; k < sizeof(skip); k++) {
            want = want || (s == skip[k]);
        }
        CHECK(((p->BatchSkip >> s) & 1ull) == (unsigned long long)want, "Twin X skip list setting %lu", s);
    }
    for (s = 0; s < MIXER_NUM_SETTINGS; s++) {
        int w = ApolloPlanBatchWrites(p, 1, s);

        n += (unsigned long)w;
        CHECK(w == (s >= 1 && s != 2 && s != 35 && !((p->BatchSkip >> s) & 1ull)), "Twin X batch 1 setting %lu", s);
        CHECK(ApolloPlanBatchWrites(p, 2, s) == (s >= 1 && s != 2 && s != 35), "Twin X batch 2 (legacy) setting %lu", s);
        CHECK(ApolloPlanBatchWrites(p, 0, s) == 0, "batch 0 writes nothing (setting %lu)", s);
    }
    CHECK(n == 27, "Twin X-safe batch: 27 settings at 0x20/0xFF (got %lu)", n);
    CHECK(!ApolloPlanBatchWrites(p, 1, 2) && !ApolloPlanBatchWrites(p, 2, 2), "S2 (monitor level) never written (4.2)");
    CHECK(ApolloParamDefault(APOLLO_MODEL_TWINX, APOLLO_PARAM_MIXER_INIT_BATCH) == 1, "Twin X default batch 1");
    CHECK(!ApolloPlanBatchWrites(0, 1, 5), "model-safe batch without a plan writes nothing");

    /* What runs: [G4] endpoints (Mode 2); ASIO stays the x8p's; [G5] strips (StripsEnable decides) */
    CHECK(p->Endpoints && !p->Asio && p->Strips, "Twin X: endpoints, no ASIO, strips (G5)");
    CHECK(p->IoctlMode1 == (APOLLO_IOCTL_CLASS_READ | APOLLO_IOCTL_CLASS_AUDIO_IO), "Twin X Mode 1: reads + capture/tone");
    CHECK(p->IoctlMode2 == (APOLLO_IOCTL_CLASS_READ | APOLLO_IOCTL_CLASS_AUDIO_IO | APOLLO_IOCTL_CLASS_SETTINGS),
          "Twin X Mode 2: reads + capture/tone + settings");
    for (m = 1; m <= 2; m++) {
        CHECK(ApolloPlanIoctlAllowed(p, m, APOLLO_IOCTL_CLASS_READ), "Twin X Mode %lu: reads", m);
        CHECK(ApolloPlanIoctlAllowed(p, m, APOLLO_IOCTL_CLASS_AUDIO_IO), "Twin X Mode %lu: capture, tone", m);
        CHECK(!ApolloPlanIoctlAllowed(p, m, APOLLO_IOCTL_CLASS_TRANSPORT), "Twin X Mode %lu: no transport IOCTLs", m);
        CHECK(ApolloPlanIoctlAllowed(p, m, APOLLO_IOCTL_CLASS_SETTINGS) == (m == 2),
              "Twin X Mode %lu: settings only in Mode 2", m);
        CHECK(!ApolloPlanIoctlAllowed(p, m, APOLLO_IOCTL_CLASS_RAW_WRITE), "Twin X Mode %lu: no raw writes", m);
        CHECK(!ApolloPlanIoctlAllowed(p, m, APOLLO_IOCTL_CLASS_ASIO), "Twin X Mode %lu: no ASIO", m);
        CHECK(ApolloPlanIoctlAllowed(p, m, 0), "unknown codes go on to 'invalid request'");
    }
    CHECK(p->ObserveDmaOff == 1, "Twin X observe: UA's ring DMA enables switched off");
    CHECK(p->ServiceAfterFailure == 0 && p->NamesFailStep == 1, "Twin X: a failure stays stopped and is named");
    CHECK(!ApolloPlanIoctlAllowed(0, 2, APOLLO_IOCTL_CLASS_READ) && ApolloPlanIoctlAllowed(0, 2, 0),
          "no plan: nothing but unknown codes");
    CHECK(ApolloBringupPlanFor(APOLLO_MODEL_NONE) == 0 && ApolloBringupPlanFor(7) == 0, "other models: no plan");
}

/* ------------------------------------------------------------------ */
/* 3. Aaron's Twin X descriptors (gate G2, STALE: as UA's driver left them) */
/* ------------------------------------------------------------------ */

static void FillDesc(unsigned long *W, const unsigned long *Head, unsigned int N)
{
    unsigned int i;

    for (i = 0; i < APOLLO_IODESC_WORDS; i++) {
        W[i] = (i < N) ? Head[i] : (i < 70 ? 0x00FF00FFul : 0);
    }
}

static const char *const kRecNames[34] = {
    "MIC/LINE/HIZ 1", "MIC/LINE/HIZ 2", "MIC/LINE 3", "MIC/LINE 4", "LINE 5", "LINE 6", "LINE 7", "LINE 8",
    "ADAT 1", "ADAT 2", "ADAT 3", "ADAT 4", "ADAT 5", "ADAT 6", "ADAT 7", "ADAT 8",
    "S/PDIF L", "S/PDIF R",
    "VIRTUAL 1", "VIRTUAL 2", "VIRTUAL 3", "VIRTUAL 4", "VIRTUAL 5", "VIRTUAL 6", "VIRTUAL 7", "VIRTUAL 8",
    "MON L", "MON R", "AUX1 L", "AUX1 R", "AUX2 L", "AUX2 R", "TALKBACK 1", "TALKBACK 2",
};

static const char *const kPlayNames[46] = {
    "MON L", "MON R", "LINE 1", "LINE 2", "LINE 3", "LINE 4", "LINE 5", "LINE 6", "LINE 7", "LINE 8",
    "ADAT 1", "ADAT 2", "ADAT 3", "ADAT 4", "ADAT 5", "ADAT 6", "ADAT 7", "ADAT 8",
    "S/PDIF L", "S/PDIF R",
    "VIRTUAL 1", "VIRTUAL 2", "VIRTUAL 3", "VIRTUAL 4", "VIRTUAL 5", "VIRTUAL 6", "VIRTUAL 7", "VIRTUAL 8",
    "CUE1 L", "CUE1 R", "CUE2 L", "CUE2 R", "CUE3 L", "CUE3 R", "CUE4 L", "CUE4 R",
    "RESERVED 1", "RESERVED 2", "RESERVED 3", "RESERVED 4", "RESERVED 5", "RESERVED 6", "RESERVED 7",
    "RESERVED 8", "RESERVED 9", "RESERVED 10",
};

static void TestDescriptors(void)
{
    /* apollo-diag --unit twinx log, "STALE desc in/out" lines, words 0-23 / 0-28; 0x00FF00FF to word 69, then 0 */
    static const unsigned long inHead[23] = {
        0x00000001, 0x00000046, 0xffffffff, 0x00000022, 0x00000000, 0x00000002, 0x00020001, 0x01040103,
        0x03060305, 0x03080307, 0x04020401, 0x04040403, 0x04060405, 0x04080407, 0x05020501, 0x07020701,
        0x07040703, 0x07060705, 0x07080707, 0x09020901, 0x02020201, 0x02040203, 0x0b020b01,
    };
    static const unsigned long outHead[29] = {
        0x00000001, 0x00000046, 0xffffffff, 0x00000000, 0x0000002e, 0x00000000, 0x09020901, 0x03020301,
        0x03040303, 0x03060305, 0x03080307, 0x04020401, 0x04040403, 0x04060405, 0x04080407, 0x05020501,
        0x07020701, 0x07040703, 0x07060705, 0x07080707, 0x0a020a01, 0x0a040a03, 0x0a060a05, 0x0a080a07,
        0x0c020c01, 0x0c040c03, 0x0c060c05, 0x0c080c07, 0x0c0a0c09,
    };
    unsigned long in[APOLLO_IODESC_WORDS], out[APOLLO_IODESC_WORDS], bad[APOLLO_IODESC_WORDS];
    unsigned long rec, play, i;
    char name[32];

    FillDesc(in, inHead, 23);
    FillDesc(out, outHead, 29);
    rec = ApolloIoDescCount(in, 1);
    play = ApolloIoDescCount(out, 0);
    CHECK(rec == 34 && play == 46, "Twin X descriptor counts: record %lu (34), playback %lu (46)", rec, play);
    CHECK(ApolloIoDescCount(in, 0) == 0 && ApolloIoDescCount(out, 1) == 0,
          "the other direction's count word is 0 in each area (not usable)");
    CHECK(ApolloPlanBufFrames(play, rec) == AUDIO_RING_FRAMES, "Twin X descriptor counts fit the 8192-frame ring");
    CHECK((unsigned long long)AUDIO_RING_FRAMES * play * 4 <= AUDIO_RING_BYTES, "46 slots fit the 4 MiB ring");
    CHECK(ApolloIoDescFind(out, 0, APOLLO_CHAN_MON_L) == 0 && ApolloIoDescFind(out, 0, APOLLO_CHAN_MON_R) == 1,
          "tone slots: MON L/R = playback 0/1");
    CHECK(ApolloIoDescFind(in, 1, APOLLO_CHAN_MON_L) == 26 && ApolloIoDescFind(in, 1, APOLLO_CHAN_MON_R) == 27,
          "monitor loopback: MON L/R = record 26/27");
    CHECK(ApolloIoDescFind(in, 1, 0x0A01) == -1, "no CUE in the record descriptor");
    /* [G4] the Twin X endpoint table's fallback slots are this descriptor's MON slots */
    CHECK(ApolloIoDescFind(out, 0, APOLLO_CHAN_MON_L) == APOLLO_TWX_SLOT_PLAY_MON_L &&
          ApolloIoDescFind(out, 0, APOLLO_CHAN_MON_R) == APOLLO_TWX_SLOT_PLAY_MON_R &&
          ApolloIoDescFind(in, 1, APOLLO_CHAN_MON_L) == APOLLO_TWX_SLOT_REC_MON_L &&
          ApolloIoDescFind(in, 1, APOLLO_CHAN_MON_R) == APOLLO_TWX_SLOT_REC_MON_R,
          "Twin X endpoint fallbacks = the G2/G3 descriptor: play 0/1, record 26/27");
    CHECK(APOLLO_TWX_SLOT_REC_MON_R < rec && APOLLO_TWX_SLOT_PLAY_MON_R < play, "fallback slots inside the counts");
    /* [G5] and its MIC/LINE 1-2 rows' fallbacks are this descriptor's 0x0001/0x0002 slots */
    CHECK(ApolloIoDescFind(in, 1, APOLLO_CHAN_IN1) == APOLLO_TWX_SLOT_REC_IN1 &&
          ApolloIoDescFind(in, 1, APOLLO_CHAN_IN2) == APOLLO_TWX_SLOT_REC_IN2,
          "Twin X MIC/LINE 1-2 fallbacks = the G2/G3 descriptor: record 0/1");
    CHECK(ApolloIoDescFind(out, 0, 0x0001) == -1, "no input channel in the playback descriptor");
    for (i = 0; i < rec; i++) {
        ApolloChanName(ApolloIoDescId(in, i), name, sizeof(name));
        CHECK(strcmp(name, kRecNames[i]) == 0, "record slot %lu: %04x '%s', want '%s'", i,
              ApolloIoDescId(in, i), name, kRecNames[i]);
    }
    for (i = 0; i < play; i++) {
        ApolloChanName(ApolloIoDescId(out, i), name, sizeof(name));
        CHECK(strcmp(name, kPlayNames[i]) == 0, "playback slot %lu: %04x '%s', want '%s'", i,
              ApolloIoDescId(out, i), name, kPlayNames[i]);
    }
    CHECK(ApolloIoDescId(in, rec) == 0x00FF, "past the count: padding 0x00FF");
    CHECK(ApolloIoDescId(in, 200) == 0, "beyond the 72 words: 0, no read past the area");

    /* Headers that must not be used */
    memcpy(bad, in, sizeof(bad));
    bad[0] = 2;
    CHECK(ApolloIoDescCount(bad, 1) == 0, "version 2: not usable");
    memcpy(bad, in, sizeof(bad));
    bad[1] = 0x45;
    CHECK(ApolloIoDescCount(bad, 1) == 0, "length 0x45: not usable");
    memcpy(bad, in, sizeof(bad));
    bad[3] = 0;
    CHECK(ApolloIoDescCount(bad, 1) == 0, "count 0: not usable");
    bad[3] = 65;
    CHECK(ApolloIoDescCount(bad, 1) == 0, "count 65: not usable (more than the ring takes)");
    bad[3] = 64;
    CHECK(ApolloIoDescCount(bad, 1) == 64 && ApolloPlanBufFrames(64, 64) == 8192, "count 64: usable, 8192 frames");
    CHECK(ApolloPlanBufFrames(65, 2) == 4096, "65 slots would need a smaller ring (refused by the driver)");
    for (i = 0; i < APOLLO_IODESC_WORDS; i++) {
        bad[i] = 0xFFFFFFFFul;
    }
    CHECK(ApolloIoDescCount(bad, 1) == 0 && ApolloIoDescCount(bad, 0) == 0, "all ones (link gone): not usable");
}

/* ------------------------------------------------------------------ */
/* 4-6. Names, steps, clock masks                                      */
/* ------------------------------------------------------------------ */

static void TestNames(void)
{
    static const struct {
        unsigned short Id;
        const char    *Name;
    } t[] = {
        { 0x0001, "MIC/LINE/HIZ 1" }, { 0x0101, "MIC/LINE 1" }, { 0x0108, "MIC/LINE 8" }, { 0x0201, "AUX1 L" },
        { 0x0204, "AUX2 R" }, { 0x0301, "LINE 1" }, { 0x0408, "ADAT 8" }, { 0x0501, "S/PDIF L" },
        { 0x0502, "S/PDIF R" }, { 0x0601, "AES/EBU L" }, { 0x0710, "VIRTUAL 16" }, { 0x0801, "MADI 1" },
        { 0x0901, "MON L" }, { 0x0902, "MON R" }, { 0x0A03, "CUE2 L" }, { 0x0A08, "CUE4 R" },
        { 0x0B01, "TALKBACK 1" }, { 0x0C0A, "RESERVED 10" }, { 0x0D01, "DANTE 1" }, { 0x0E01, "UNKNOWN 1" },
        { 0xFFFF, "UNKNOWN 255" },
    };
    char b[32], small[4];
    unsigned int i;
    unsigned long k;

    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        unsigned long n = ApolloChanName(t[i].Id, b, sizeof(b));

        CHECK(strcmp(b, t[i].Name) == 0 && n == strlen(t[i].Name), "name %04x: '%s', want '%s'", t[i].Id, b, t[i].Name);
    }
    CHECK(ApolloChanName(0x0001, small, sizeof(small)) == 3 && strcmp(small, "MIC") == 0, "truncated, terminated");
    CHECK(ApolloChanName(0x0001, small, 0) == 0, "zero-length buffer");

    for (k = 1; k < APOLLO_BSTEP_COUNT; k++) {
        unsigned long j;

        CHECK(ApolloBstepName(k)[0] != 0 && strcmp(ApolloBstepName(k), "?") != 0, "step %lu has a name", k);
        for (j = 1; j < k; j++) {
            CHECK(strcmp(ApolloBstepName(j), ApolloBstepName(k)) != 0, "steps %lu and %lu named alike", j, k);
        }
    }
    CHECK(strcmp(ApolloBstepName(APOLLO_BSTEP_COUNT), "?") == 0, "no step past the table");
    CHECK(APOLLO_BSTEP_COUNT <= 32, "steps fit GET_INFO BringupSteps");

    /* G2: the Twin X offered S/PDIF, ADAT, Peer and Internal (stale); the x8p adds Word Clock */
    CHECK(ApolloClockOffers(0x1421007Eul, APOLLO_CLKSRC_SPDIF) && ApolloClockOffers(0x1421007Eul, APOLLO_CLKSRC_ADAT) &&
          !ApolloClockOffers(0x1421007Eul, APOLLO_CLKSRC_WORDCLOCK) && ApolloClockOffers(0x1421007Eul, APOLLO_CLKSRC_PEER) &&
          ApolloClockOffers(0x1421007Eul, APOLLO_CLKSRC_INTERNAL), "Twin X stale mask 0x1421: S/PDIF ADAT Peer Internal");
    CHECK(ApolloClockOffers(0x14A1007Eul, APOLLO_CLKSRC_WORDCLOCK) && ApolloClockOffers(0x14A1007Eul, APOLLO_CLKSRC_PEER),
          "x8p mask 0x14A1: Word Clock and Peer");
    CHECK(!ApolloClockOffers(0x1421007Eul, 16), "source ids above 15: no");
}

/* ------------------------------------------------------------------ */
/* 7. Mode rules with the plans (apollo_unit.h)                        */
/* ------------------------------------------------------------------ */

static void Finish(int Model, unsigned long Mode, APOLLO_UNIT *U)
{
    int i;

    memset(U, 0, sizeof(*U));
    for (i = 0; i < APOLLO_PARAM_COUNT; i++) {
        U->Value[i] = ApolloParamDefault(Model, i);
    }
    U->Value[APOLLO_PARAM_MODE] = Mode;
    U->Value[APOLLO_PARAM_STRIPS_ENABLE] = 1;
    U->Value[APOLLO_PARAM_ENABLED_ENDPOINTS] = 0x3FF;
    ApolloUnitFinish(Model, U);
}

static void TestModes(void)
{
    APOLLO_UNIT u;

    Finish(APOLLO_MODEL_X8P, 2, &u);
    CHECK(u.Active && u.Value[APOLLO_PARAM_MODE] == 2 && u.Value[APOLLO_PARAM_STRIPS_ENABLE] == 1 &&
          u.Value[APOLLO_PARAM_ENABLED_ENDPOINTS] == 0x3FF, "x8p Mode 2: everything, as before");
    Finish(APOLLO_MODEL_X8P, 1, &u);
    CHECK(u.Active && u.Value[APOLLO_PARAM_MODE] == 1 && u.Value[APOLLO_PARAM_STRIPS_ENABLE] == 0,
          "x8p Mode 1: as before");
    Finish(APOLLO_MODEL_TWINX, 1, &u);
    CHECK(u.Active && u.Value[APOLLO_PARAM_MODE] == 1 && u.Value[APOLLO_PARAM_STRIPS_ENABLE] == 0 &&
          u.Value[APOLLO_PARAM_ENABLED_ENDPOINTS] == 0, "Twin X Mode 1: active, nothing else");
    /* [G4] Mode 2 runs as Mode 2; StripsEnable stays a value, strips.c refuses it by the plan */
    Finish(APOLLO_MODEL_TWINX, 2, &u);
    CHECK(u.Active && u.Value[APOLLO_PARAM_MODE] == 2 && u.Source[APOLLO_PARAM_MODE] != APOLLO_SRC_MODE &&
          u.ModeAsked == 2 && u.Value[APOLLO_PARAM_ENABLED_ENDPOINTS] == 0x3FF,
          "Twin X Mode 2 stays 2 (endpoints value kept; the table has 4 rows since G5)");
    CHECK(ApolloBringupPlanFor(APOLLO_MODEL_TWINX)->Strips && u.Value[APOLLO_PARAM_STRIPS_ENABLE] == 1,
          "Twin X Mode 2 (G5): the plan has strips, StripsEnable stays the value asked");
    Finish(APOLLO_MODEL_TWINX, 0, &u);
    CHECK(!u.Active, "Twin X Mode 0: observe");
    Finish(APOLLO_MODEL_NONE, 2, &u);
    CHECK(!u.Active, "no model: never active");
    CHECK(ApolloParamDefault(APOLLO_MODEL_TWINX, APOLLO_PARAM_MODE) == APOLLO_MODE_OBSERVE &&
          ApolloParamDefault(APOLLO_MODEL_X8P, APOLLO_PARAM_MODE) == APOLLO_MODE_FULL,
          "defaults: Twin X observe, x8p full");
    CHECK(ApolloParamDefault(APOLLO_MODEL_X8P, APOLLO_PARAM_WARM_BOOT_TRANSPORT) == 0 &&
          ApolloParamDefault(APOLLO_MODEL_TWINX, APOLLO_PARAM_WARM_BOOT_TRANSPORT) == 0,
          "WarmBootTransport default 0");
    CHECK(ApolloParamDefault(APOLLO_MODEL_X8P, APOLLO_PARAM_MIXER_INIT_BATCH) == APOLLO_DEF_BATCH_X8P,
          "x8p default batch unchanged");
}

/* ------------------------------------------------------------------ */
/* 8. [G4] The Twin X in Mode 2                                        */
/* ------------------------------------------------------------------ */

static void TestG4(void)
{
    const APOLLO_BRINGUP_PLAN *p = ApolloBringupPlanFor(APOLLO_MODEL_TWINX);
    /*
     * [G5] + talkback (S13 bit 8, its built-in mic). [Monitor] + monitor source, HP1 source, CUE 1-2 MIX/mono
     * (0x05-0x08), ALT 1 trim/select/enable (0x38, 0x3A, 0x3B), the cue send families (monitor_plan.h; the
     * cue and bus within a family are refused per model by ApolloMonEncode, mon-plan-test).
     * [Mix] + the monitor-mix families (0x4000 level, 0x4100 pan, 0x4200 mute, 0x4300 solo, 0x4400 mute-all).
     */
    static const unsigned long yes[] = { APOLLO_PLAN_MON_LEVEL, APOLLO_PLAN_MON_HP1_LEVEL, APOLLO_PLAN_MON_MUTE,
                                         APOLLO_PLAN_MON_MONO, APOLLO_PLAN_MON_DIM, APOLLO_PLAN_MON_TALKBACK,
                                         APOLLO_PLAN_MON_SOURCE, APOLLO_PLAN_MON_HP1_SOURCE, 0x05, 0x06, 0x07, 0x08,
                                         0x38, 0x3A, 0x3B, 0x1000, 0x110A, 0x2001, 0x3100,
                                         0x4000, 0x400A, 0x4101, 0x4200, 0x4301, 0x4400 };
    /* one headphone, no dim level (unverified on the Twin X), no CUE 3/4 (0x22-0x25), no ALT 2 (0x39, 0x3C) */
    static const unsigned long no[] = { APOLLO_PLAN_MON_HP2_SOURCE, APOLLO_PLAN_MON_DIM_LEVEL, 0, 0x22, 0x23, 0x24,
                                        0x25, 0x39, 0x3C, 0x45, 0x99, 0x4500, 0x4FFF, 0x5000, 0xFFFFFFFFul };
    /* G4's ten codes keep bits 0x001-0x200; the [Monitor] codes take their own bits above them */
    static const unsigned long g4[] = { 0x01, 0x02, 0x03, 0x04, 0x10, 0x3F, 0x40, 0x43, 0x44, 0x46 };
    static const unsigned long mon[] = { 0x05, 0x06, 0x07, 0x08, 0x22, 0x23, 0x24, 0x25, 0x38, 0x39, 0x3A, 0x3B, 0x3C,
                                         0x1000, 0x2000, 0x3000, 0x4000, 0x4100, 0x4200, 0x4300, 0x4400 };
    unsigned int k;
    unsigned long bits = 0, monBits = 0;
    APOLLO_UNIT u;

    if (p == 0) {
        CHECK(0, "Twin X plan");
        return;
    }
    /* Settings: exactly the monitor controls spec-twinx 4.1 shares with the x8p, one headphone */
    for (k = 0; k < sizeof(yes) / sizeof(yes[0]); k++) {
        CHECK(ApolloPlanMonitorParamAllowed(p, yes[k]), "Twin X Mode 2 takes monitor param 0x%lX", yes[k]);
    }
    for (k = 0; k < sizeof(no) / sizeof(no[0]); k++) {
        CHECK(!ApolloPlanMonitorParamAllowed(p, no[k]), "Twin X refuses monitor param 0x%lX", no[k]);
    }
    CHECK(!ApolloPlanMonitorParamAllowed(0, APOLLO_PLAN_MON_LEVEL), "no plan: no monitor params");
    CHECK(p->InputParams == 1 && p->RawMixerSettings == 0, "Twin X: preamp settings (G5, preamp_plan.h), no raw mixer settings");
    for (k = 0; k < sizeof(g4) / sizeof(g4[0]); k++) {
        bits |= ApolloPlanMonBit(g4[k]);
    }
    CHECK(bits == 0x3FFul, "ten monitor codes, ten distinct bits (0x%lX)", bits);
    for (k = 0; k < sizeof(mon) / sizeof(mon[0]); k++) {
        unsigned long b = ApolloPlanMonBit(mon[k]);

        CHECK(b != 0 && (b & (b - 1)) == 0 && (b & 0x3FFul) == 0 && (monBits & b) == 0,
              "[Monitor] code 0x%lX: its own bit 0x%lX above G4's", mon[k], b);
        monBits |= b;
    }
    /*
     * Its endpoints: MON L/R out + in by default; [G5] MIC/LINE 1-2 (rows 2-3) opt-in, every circuit object at
     * device add; Mode 2 only
     */
    CHECK(p->EndpointRows == 9 && p->EndpointDefault == 0x3F && APOLLO_DEF_ENDPOINTS_TWINX == APOLLO_TWX_EP_DEFAULT &&
          p->EndpointDefault == APOLLO_TWX_EP_DEFAULT && APOLLO_TWX_EP_ALL == 0x1FF && p->AdcFrames == 5 &&
          p->EndpointRows == APOLLO_TWX_EP_LIST_COUNT && p->EndpointDefault == APOLLO_TWX_EP_LIST_DEFAULT,
          "[Endpoints] Twin X endpoints: 9 rows, default 0x3F (MON, LINE 3-4, HP, loopback, MIC/LINE 1-2), ADC 5 frames");
    CHECK(ApolloParamDefault(APOLLO_MODEL_TWINX, APOLLO_PARAM_ENABLED_ENDPOINTS) == 0x3F, "Twin X Endpoints default 0x3F");
    CHECK(ApolloAddTimeEndpoints(1, 0x0019, 1, 0x3FF) == 0x1FF && ApolloAddTimeModel(1, 0x0019) == APOLLO_MODEL_TWINX,
          "Twin X device add: its own circuit objects, whatever the x8p's global says");
    CHECK(APOLLO_TWX_SLOT_REC_IN1 == 0 && APOLLO_TWX_SLOT_REC_IN2 == 1 && APOLLO_CHAN_IN1 == 0x0001 &&
          APOLLO_CHAN_IN2 == 0x0002, "Twin X MIC/LINE 1-2: 0x0001/0x0002, fallback record slots 0/1");
    CHECK(ApolloAddTimeEndpoints(1, 0x000F, 1, 0x3FF) == 0 && ApolloAddTimeModel(1, 0x000F) == APOLLO_MODEL_NONE,
          "other subsystem: no table");
    Finish(APOLLO_MODEL_TWINX, 1, &u);
    CHECK(u.Value[APOLLO_PARAM_ENABLED_ENDPOINTS] == 0, "Twin X Mode 1: endpoints forced off (G3 unchanged)");
    Finish(APOLLO_MODEL_TWINX, 0, &u);
    CHECK(!u.Active && u.Value[APOLLO_PARAM_ENABLED_ENDPOINTS] == 0, "Twin X Mode 0: none");

    /* Steps per path (GET_INFO BringupApplies): the G3 hardware runs, Nov 14 */
    CHECK(ApolloPlanBstepApplies(p, 0, 1, 1) == 0xFFEFEul, "cold, E8 answered: 0xFFEFE (0x%05lX)",
          ApolloPlanBstepApplies(p, 0, 1, 1));
    CHECK(ApolloPlanBstepApplies(p, 0, 1, 0) == 0xFFFFEul, "cold, E8 not answered: E10 applies too");
    CHECK(ApolloPlanBstepApplies(p, 1, 0, 0) == 0xFF818ul, "warm, no boot transport: 0xFF818 (0x%05lX)",
          ApolloPlanBstepApplies(p, 1, 0, 0));
    CHECK(ApolloPlanBstepApplies(p, 1, 1, 1) == 0xFFEF8ul, "warm, WarmBootTransport=1, E8 answered: 0xFFEF8");
    CHECK(ApolloPlanBstepApplies(0, 0, 1, 1) == 0, "no plan: no steps");
    /* step-g3.log: the warm start passed 0xFF818 = every step its path runs; the cold one missed only 17 */
    CHECK((ApolloPlanBstepApplies(p, 1, 0, 0) & ~0xFF818ul) == 0, "G3 warm start: 0 not passed");
    CHECK((ApolloPlanBstepApplies(p, 0, 1, 1) & ~0xDFEFEul) == (1ul << APOLLO_BSTEP_RATE_48K),
          "G3 cold start: only step 17 (48 kHz) missing");
    CHECK((ApolloPlanBstepApplies(p, 0, 1, 1) & 1ul) == 0 && (ApolloPlanBstepApplies(p, 1, 0, 0) & 1ul) == 0,
          "bit 0 never used");
    CHECK(APOLLO_BSTEPS_CONNECT == 0xFF800ul && APOLLO_BSTEPS_BOOT == 0x7E0ul && APOLLO_BSTEPS_COLD == 0x1Eul &&
          APOLLO_BSTEPS_WARM == 0x18ul, "step ranges");
    CHECK(strcmp(ApolloBstepName(APOLLO_BSTEP_RATE_48K), "48 kHz locked + acked") == 0, "step 17 name");
}

int main(void)
{
    TestX8p();
    TestTwinX();
    TestDescriptors();
    TestNames();
    TestModes();
    TestG4();
    printf("bringup-plan-test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
