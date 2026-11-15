/*
 * rate-test.c - [Rate] offline checks of the sample-rate change (driver\rate_plan.h and what follows
 * the rate elsewhere): the driver's own headers, compiled in user mode.
 *
 *   1. the rate table: index <-> Hz, band, FRAME_CFG, the 48 kHz family, offered / allowed masks
 *      (S/PDIF at most 96 kHz), the Hi-band dwell and the 48 kHz session before a Hi-band start;
 *   2. the buffer math at every rate: the copy engine's lead and guard (the same time as at 48 kHz),
 *      ticks, the zero horizon, the diagnostic playback window, the meters, speaker correction's
 *      fetch-ahead (= FRAME_CFG), the ADC delay; and 48 kHz exactly as before; [Lat] the reported
 *      latencies' device side (strip DSP block, record delay, a chain's excess, the DAC per model);
 *   3. the Mic strip's words per rate (strip_plan.h StripRateWords): 48 kHz = the captured words,
 *      every other rate the captured layout with open-apollo's ramp / DC blocker / LowCut tables;
 *   4. the combined device: the margin, the tick and the render offset limit at every rate, Rate 0
 *      = 48 kHz bit for bit (Compile, clock loop), the follower render lead at every rate and ASRC
 *      size;
 *   5. the clock loop at every rate: lock time, phase error, ratio error, late stamps, a jump and an
 *      out-of-range ratio, with the read noise the same time at every rate;
 *   6. the resampler at every rate: its filter is normalised to Nyquist (no table per rate); error of
 *      1, 10, 18 and 20 kHz sines through the drifting fractional delay at each rate;
 *   7. [Clock] driver\clock_plan.h: the clock sources per model against the units' live 0xC08C words, every
 *      refusal (unproven, rate, port, SR Convert), the digital channels per format and rate (S/MUX), the masked
 *      format writes, and the combined layout following a format (no format = the layout as before).
 *
 * Exit code 0 = every check passed.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "..\..\include\apollo_ioctl.h"
#include "..\..\driver\apollo_agg.h"        /* includes bringup_plan.h and rate_plan.h */
#include "..\..\driver\strip_plan.h"
#include "..\..\driver\corr_dsp.h"
#include "..\..\driver\regs.h"            /* X8P_SAFETY_FRAMES_48K */
#include "..\..\driver\apollo_unit.h"     /* APOLLO_DEF_RENDER_LEAD, APOLLO_DEF_CAPTURE_GUARD */

static int g_fail, g_pass;

static void Check(int ok, const char *what)
{
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        printf("FAIL: %s\n", what);
    }
}

#define APOLLO_ZERO_AHEAD_FRAMES_TEST 4096ul  /* driver\apollo.h APOLLO_ZERO_AHEAD_FRAMES (kernel header) */

#define CHECKF(ok, ...) do { char m_[256]; _snprintf_s(m_, sizeof(m_), _TRUNCATE, __VA_ARGS__); Check((ok), m_); } while (0)

static const unsigned long kHz[6] = { 44100, 48000, 88200, 96000, 176400, 192000 };

/* deterministic gaussian noise (asrc-test's) */
static unsigned long long g_rng = 0x9E3779B97F4A7C15ull;
static double Uniform(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return ((g_rng >> 11) + 0.5) / 9007199254740992.0;
}
static double Gauss(void)
{
    double u = Uniform(), v = Uniform();
    return sqrt(-2.0 * log(u)) * cos(2.0 * 3.14159265358979323846 * v);
}

/* ---------------------------------------------------------------- 1 */
static void TestTable(void)
{
    unsigned long i;

    for (i = 0; i < 6; i++) {
        unsigned long idx = i + 1, hz = kHz[i];

        CHECKF(ApolloRateHz(idx) == hz && ApolloRateIndex(hz) == idx, "index %lu <-> %lu Hz", idx, hz);
        CHECKF(ApolloRateBand(idx) == i / 2, "%lu Hz: band %lu", hz, i / 2);
        CHECKF(ApolloRateFrameCfg(8, idx) == (8ul << (i / 2)), "%lu Hz: FRAME_CFG %lu (the kext's 8/16/32)", hz, 8ul << (i / 2));
        CHECKF(ApolloRateIsHi(idx) == (i >= 4), "%lu Hz: Hi band %d", hz, i >= 4);
        CHECKF(ApolloRateFamily48(hz) == (hz % 1000 == 0), "%lu Hz: family", hz);
        CHECKF(strcmp(ApolloRateText(hz), "?") != 0, "%lu Hz has a name", hz);
    }
    Check(ApolloRateHz(0) == 0 && ApolloRateHz(7) == 0 && ApolloRateHz(0xFFFFFFFFul) == 0, "no Hz for index 0, 7, ~0");
    Check(ApolloRateIndex(0) == 0 && ApolloRateIndex(32000) == 0 && ApolloRateIndex(48001) == 0 &&
          ApolloRateIndex(384000) == 0, "no index for 0, 32k, 48001, 384k");
    Check(ApolloRateBand(0) == 0 && ApolloRateBand(9) == 0, "unknown index: band 0 (upstream's default)");
    Check(ApolloPlanClockWord(2, 0x0C) == 0x20C && ApolloPlanClockWord(6, 0x0C) == 0x60C, "clock words 0x20C, 0x60C");
    /* offered / allowed */
    Check(ApolloRateOffered(0x0000007Eul) == 0x7E, "x8p 0xC08C 0x007E: all six");
    Check(ApolloRateOffered(0x1421007Eul) == 0x7E, "Twin X 0xC08C 0x1421007E: all six");
    Check(ApolloRateOffered(0) == 0x4 && ApolloRateOffered(0xFFFFFFFFul) == 0x4 && ApolloRateOffered(0x14210000ul) == 0x4,
          "unknown / dead / no rate bits: 48 kHz only");
    Check(ApolloRateOffered(0x00000016ul) == 0x16, "a unit offering 44.1, 88.2, 96 only");
    Check(ApolloRateAllowed(0x7E, 0x0C) == 0x7E, "internal clock: every offered rate");
    Check(ApolloRateAllowed(0x7E, APOLLO_RATE_SOURCE_SPDIF) == 0x1E, "S/PDIF clock: 44.1-96 kHz only");
    /* the Hi band rules */
    Check(ApolloRateLoMidHoldMs(6, 6) == 400 && ApolloRateLoMidHoldMs(6, 5) == 400 && ApolloRateLoMidHoldMs(5, 6) == 1200 &&
          ApolloRateLoMidHoldMs(5, 5) == 1200, "Hi start from a Hi clock: 48 kHz for 400 ms (from 192) / 1200 ms (176.4)");
    Check(ApolloRateLoMidHoldMs(2, 6) == 0 && ApolloRateLoMidHoldMs(4, 5) == 0 && ApolloRateLoMidHoldMs(6, 2) == 0 &&
          ApolloRateLoMidHoldMs(0, 6) == 0, "no session from 44.1-96 kHz, or to a Lo/Mid rate");
    Check(ApolloRateDwellMs(6, 0, 2) == 3000 && ApolloRateDwellMs(6, 1000, 1) == 2000 && ApolloRateDwellMs(5, 2999, 4) == 1 &&
          ApolloRateDwellMs(6, 3000, 2) == 0 && ApolloRateDwellMs(6, 100, 6) == 0 && ApolloRateDwellMs(0, 0, 2) == 0 &&
          ApolloRateDwellMs(4, 0, 2) == 0, "Hi-band dwell: 3 s at 176.4/192 before any other rate");
    Check(ApolloRateInfoLocked(0x201, 2) && !ApolloRateInfoLocked(0x200, 2) && !ApolloRateInfoLocked(0x101, 2) &&
          !ApolloRateInfoLocked(0xFFFFFFFFul, 2) && ApolloRateInfoLocked(0x601, 6) && !ApolloRateInfoLocked(0x603, 6),
          "0xC07C locked = index and [1:0] == 1");
    Check(ApolloPlanRateLocked(0x201, 2) && ApolloPlanRateLocked(0x203, 2), "the bring-up's own check is unchanged");
}

/* ---------------------------------------------------------------- 2 */
static void TestBuffers(void)
{
    unsigned long i;

    /* 48 kHz: exactly what the driver used before rates could change */
    Check(ApolloRateScale(144, 48000) == 144 && ApolloRateScale(16, 48000) == 16 && ApolloRateScale(1024, 48000) == 1024,
          "48 kHz: lead 144, guard 16, meter window 1024 unchanged");
    Check(ApolloRateLead(144, 48000) == 144 && ApolloRateGuard(16, 48000) == 16 && ApolloRateTickFrames(48000) == 48,
          "48 kHz: engine lead, guard, tick unchanged");
    Check(ApolloRateLead(1024, 48000) == 1024 && ApolloRateLead(64, 48000) == 64 && ApolloRateGuard(64, 48000) == 64,
          "48 kHz: the registry limits unchanged");
    Check(ApolloRateFrameCfg(X8P_SAFETY_FRAMES_48K, 2) == X8P_SAFETY_FRAMES_48K && ApolloRateAdc(10, 2) == 10 &&
          ApolloRateAdc(5, 2) == 5, "48 kHz: FRAME_CFG 8, ADC 10 / 5 unchanged");
    Check(ApolloRateScale(0, 192000) == 0 && ApolloRateScale(144, 0) == 144, "scale of 0, rate 0 = 48 kHz");
    for (i = 0; i < 6; i++) {
        unsigned long hz = kHz[i], idx = i + 1;
        unsigned long lead = ApolloRateLead(APOLLO_DEF_RENDER_LEAD, hz), guard = ApolloRateGuard(APOLLO_DEF_CAPTURE_GUARD, hz);
        unsigned long tick = ApolloRateTickFrames(hz), fc = ApolloRateFrameCfg(8, idx);
        unsigned long leadMax = ApolloRateLead(1024, hz), win = ApolloRateScale(1024, hz);
        double leadMs = lead * 1000.0 / hz, guardMs = guard * 1000.0 / hz;

        printf("%6lu Hz: lead %4lu (%.3f ms), guard %3lu (%.3f ms), tick %3lu, FRAME_CFG %2lu, ADC x8p %lu / Twin X %lu, "
               "max lead %4lu, meters %4lu frames\n", hz, lead, leadMs, guard, guardMs, tick, fc, ApolloRateAdc(10, idx),
               ApolloRateAdc(5, idx), leadMax, win > 4096 ? 4096 : win);
        CHECKF(leadMs >= 3.0 && leadMs < 3.03, "%lu Hz: the lead is 3 ms as at 48 kHz (%.3f)", hz, leadMs);
        CHECKF(guardMs >= 1.0 / 3.0 && guardMs < 0.36, "%lu Hz: the guard is 1/3 ms as at 48 kHz (%.3f)", hz, guardMs);
        CHECKF(tick * 1000ul >= hz && (tick - 1) * 1000ul < hz, "%lu Hz: a tick is %lu frames", hz, tick);
        /* the tick writes up to pos + lead; the hardware fetches up to pos + FRAME_CFG and moves a tick per tick */
        CHECKF(lead >= fc + tick + 32, "%lu Hz: lead %lu covers FRAME_CFG %lu + a tick %lu with room", hz, lead, fc, tick);
        CHECKF(leadMax + tick < APOLLO_ZERO_AHEAD_FRAMES_TEST, "%lu Hz: even the longest lead stays under the zero horizon", hz);
        CHECKF(lead + tick < 4096 - 64 - 256, "%lu Hz: the diagnostic playback window is open (lead + tick < horizon - 64)", hz);
        CHECKF(2 * 4096 <= 8192 && 4096 + lead < 8192, "%lu Hz: zero horizon + lead inside the 8192-frame ring", hz);
        CHECKF(CorrSafety(hz) == fc, "%lu Hz: speaker correction skips FRAME_CFG %lu frames", hz, fc);
        CHECKF((unsigned long long)CORR_HIST * 1000ull / hz >= 10, "%lu Hz: correction history >= 10 ms", hz);
        CHECKF(ApolloRateAdc(10, idx) == (i >= 4 ? 11ul : 10ul) && ApolloRateAdc(5, idx) == (i >= 4 ? 6ul : 5ul),
               "%lu Hz: ADC x8p %lu, Twin X %lu (oa adc_mframes)", hz, i >= 4 ? 11ul : 10ul, i >= 4 ? 6ul : 5ul);
        /* ASIO: Delay = 2N + guard + lead must stay under the ring for the largest buffer (1024) */
        CHECKF(2ul * 1024 + guard + lead + tick < 8192 - 256, "%lu Hz: the largest ASIO buffer fits the ring", hz);
    }
    Check(ApolloRateLead(1024, 192000) == APOLLO_RATE_LEAD_MAX, "the scaled lead is capped at 2048");
}

/* ---------------------------------------------------------------- 2b [Lat] reported latencies (docs/ASIO.md, Latency) */
static void TestLatency(void)
{
    static const unsigned long dacX8p[6] = { 43, 43, 15, 15, 16, 16 }, dacTwx[6] = { 44, 44, 16, 16, 17, 17 };
    const APOLLO_BRINGUP_PLAN *x8p = ApolloBringupPlanFor(APOLLO_MODEL_X8P), *twx = ApolloBringupPlanFor(APOLLO_MODEL_TWINX);
    unsigned long i;

    Check(x8p != 0 && twx != 0, "latency: both plans");
    if (x8p == 0 || twx == 0) {
        return;
    }
    /* the strip path: the strip DSP's block (2 x the strips' block size of strip_plan.h) + the record delay 0xc8 */
    Check(APOLLO_LAT_STRIP_RECORD_DELAY == 200 && g_StripM0A[6] == APOLLO_LAT_STRIP_RECORD_DELAY,
          "latency: the plain strip's record delay is the strip recipe's m0 word (0xc8 = 200)");
    for (i = 0; i < 6; i++) {
        unsigned long hz = kHz[i], idx = i + 1, bs = (hz > 100000) ? 64ul : 32ul;
        const STRIP_RAMP *r = StripRampOf(hz);

        CHECKF(r != 0 && (unsigned long)(*(const float *)&r->F) * 4ul == bs && ApolloRateDspBlock(idx) == 2 * bs,
               "%lu Hz: DSP block %lu = 2 x the strips' block size %lu", hz, ApolloRateDspBlock(idx), bs);
        CHECKF(ApolloLatencyInPath(1, idx, 0) == ApolloRateDspBlock(idx) + 200 && ApolloLatencyInPath(0, idx, 500) == 0,
               "%lu Hz: strip path %lu with strips running, 0 without", hz, ApolloLatencyInPath(1, idx, 0));
        CHECKF(ApolloRateDac(x8p->DacCenti, idx) == dacX8p[i] && ApolloRateDac(twx->DacCenti, idx) == dacTwx[i],
               "%lu Hz: DAC x8p %lu, Twin X %lu (UA output_delay_comp, rounded)", hz, dacX8p[i], dacTwx[i]);
    }
    /* 48 kHz on the x8p at 128 frames: input 128 + 16 + 10 + 264, output 128 + 144 + 8 + 43 */
    Check(16 + ApolloRateAdc(10, 2) + ApolloLatencyInPath(1, 2, 0) == 290 && 144 + ApolloRateFrameCfg(8, 2) + ApolloRateDac(x8p->DacCenti, 2) == 195,
          "latency: x8p at 48 kHz reports input 290 + N and output 195 + N with the strips running");
    /* a chain's record path over the plain strip's 200 */
    Check(ApolloLatencyChainExcess(200, 0) == 0 && ApolloLatencyChainExcess(136, 64) == 0 && ApolloLatencyChainExcess(0, 1000) == 800 &&
          ApolloLatencyChainExcess(200, 55) == 55 && ApolloLatencyChainExcess(0, 0) == 0,
          "latency: chain excess: within the budget 0, Long-latency chain 800, a fixed 200 + 55 = 55");
    Check(ApolloLatencyInPath(1, 2, 800) == 64 + 200 + 800 && ApolloLatencyInPath(1, 2, 100000) == APOLLO_LAT_IN_PATH_MAX,
          "latency: an over-budget chain adds its excess; the path is capped");
    Check(ApolloRateDac(0, 2) == 0, "latency: no DAC table, no DAC");
}

/* ---------------------------------------------------------------- 3 */
static void TestStrips(void)
{
    ULONG a[18], b[16], m[15], j, i, band;
    STRIP_PLAN p48, pr;
    int same;

    Check(StripRateWords(48000, a, b, m) == 0, "48 kHz has strip words");
    same = 1;
    for (j = 0; j < 18; j++) same &= a[j] == g_StripM0A[j];
    for (j = 0; j < 16; j++) same &= b[j] == g_StripM0B[j];
    for (j = 0; j < 15; j++) same &= m[j] == g_StripM3W[j];
    Check(same, "48 kHz: the captured words verbatim");
    Check(g_StripM0A[11] == g_StripRamp[1].K && g_StripM0A[12] == g_StripRamp[1].F && g_StripM0A[13] == g_StripRamp[1].Inv,
          "the captured m0[11..13] are the 48 kHz ramp (k 75, f 8.0, 1/600)");
    Check(memcmp(&g_StripM3W[8], &g_StripRateLowcut[0][1], 7 * sizeof(ULONG)) == 0, "the captured LowCut is band 0's");
    Check(StripRateWords(32000, a, b, m) != 0 && StripRateWords(0, a, b, m) != 0, "no words for a rate the Apollo lacks");
    for (i = 0; i < 6; i++) {
        unsigned long hz = kHz[i];
        float f, inv, k;

        CHECKF(StripRateWords(hz, a, b, m) == 0, "%lu Hz has strip words", hz);
        if (hz == 48000) {
            continue;
        }
        band = StripRateBand(hz);
        CHECKF(band == i / 2, "%lu Hz: SDK band %lu", hz, i / 2);
        memcpy(&f, &a[12], 4);
        memcpy(&inv, &a[13], 4);
        k = (float)a[11];
        CHECKF(fabs(inv * f * k - 1.0) < 1e-6, "%lu Hz: 1/(f*k) (k %lu, f %.0f)", hz, a[11], f);
        CHECKF(a[11] == (ULONG)floor(hz / (hz > 100000 ? 64.0 : 32.0) * 0.05 + 0.5), "%lu Hz: k = SR / BS * 0.05 + 0.5", hz);
        CHECKF(f == (hz > 100000 ? 16.0f : 8.0f), "%lu Hz: f = BS / 4 (BS %d)", hz, hz > 100000 ? 64 : 32);
        same = 1;
        for (j = 0; j < 18; j++) same &= (j >= 11 && j <= 13) || a[j] == g_StripM0A[j];
        CHECKF(same, "%lu Hz: m0[0..17] is the captured block but the ramp", hz);
        CHECKF(b[0] == g_StripM0B[0] && b[15] == g_StripM0B[15], "%lu Hz: m0[38] and m0[53] kept", hz);
        same = 1;
        for (j = 1; j < 15; j++) same &= b[j] == g_StripRateDc[band][j < 8 ? j : j - 7];
        CHECKF(same, "%lu Hz: m0[39..52] = the band's DC blocker twice", hz);
        same = m[0] == g_StripM3W[0];
        for (j = 1; j < 15; j++) same &= m[j] == (j < 8 ? g_StripRateDc[band][j] : g_StripRateLowcut[band][j - 7]);
        CHECKF(same, "%lu Hz: m3[1..15] = the count, the band's DC blocker and LowCut", hz);
        /* the plan: only stage 13's payload differs from 48 kHz */
        Check(StripPlanPrepare(3, 0, &p48) == 0, "48 kHz plan builds");
        CHECKF(StripPlanPrepareRate(3, 0, hz, &pr) == 0 && pr.N == p48.N && pr.Len == p48.Len, "%lu Hz plan builds alike", hz);
        CHECKF(memcmp(pr.Payload, p48.Payload, (p48.Len - 55) * sizeof(ULONG)) == 0, "%lu Hz: memspec and header as 48 kHz", hz);
        CHECKF(memcmp(pr.Ent, p48.Ent, p48.N * sizeof(p48.Ent[0])) == 0 && memcmp(pr.Addr, p48.Addr, sizeof(p48.Addr)) == 0,
               "%lu Hz: same entries and addresses", hz);
    }
    Check(StripPlanPrepareRate(0, 0, 32000, &pr) != 0, "a plan at 32 kHz is refused");
}

/* ---------------------------------------------------------------- 4 */
static void TestAggConstants(void)
{
    unsigned long i, hz, h;
    static const unsigned long halves[3] = { 8, 16, 32 };

    Check(ApolloAggMarginAt(0) == APOLLO_AGG_MARGIN && ApolloAggMarginAt(48000) == APOLLO_AGG_MARGIN &&
          ApolloAggTickAt(0) == APOLLO_AGG_TICK_FRAMES && ApolloAggTickAt(48000) == APOLLO_AGG_TICK_FRAMES,
          "48 kHz (Rate 0 or 48000): M 16, tick 48 as before");
    for (h = 0; h < 3; h++) {
        CHECKF(ApolloAggRenOffsetMaxAt(144, halves[h], 8, 0) == ApolloAggRenOffsetMax(144, halves[h], 8) &&
               ApolloAggRenOffsetMaxAt(144, halves[h], 8, 48000) == ApolloAggRenOffsetMax(144, halves[h], 8),
               "48 kHz render offset limit unchanged at h %lu", halves[h]);
    }
    for (i = 0; i < 6; i++) {
        unsigned long idx = i + 1, lead, fc, m, tick;

        hz = kHz[i];
        lead = ApolloRateLead(APOLLO_DEF_RENDER_LEAD, hz);
        fc = ApolloRateFrameCfg(8, idx);
        m = ApolloAggMarginAt(hz);
        tick = ApolloAggTickAt(hz);
        CHECKF(fabs(m * 1000.0 / hz - 16000.0 / 48000.0) < 0.03, "%lu Hz: M is a third of a ms (%lu frames)", hz, m);
        CHECKF(tick == ApolloRateTickFrames(hz), "%lu Hz: the aggregate's tick is the engine's", hz);
        for (h = 0; h < 3; h++) {
            long lim = ApolloAggRenOffsetMaxAt(lead, halves[h], fc, hz);
            long flead = (long)lead - (long)halves[h] - (long)m - 2;

            if (flead < (long)fc + 16) {
                flead = (long)fc + 16;
            }
            CHECKF(lim > 0, "%lu Hz h %lu: a follower may play earlier (limit %ld)", hz, halves[h], lim);
            /* the follower render: written to pf + lead each tick, fetched to pf + FRAME_CFG + 4, a tick apart */
            CHECKF(flead - (long)(fc + 4) >= (long)tick, "%lu Hz h %lu: follower render lead %ld covers a tick", hz,
                   halves[h], flead);
        }
    }
}

/* Rate 0 and 48000 in the layout: bit for bit the same */
static void TestAggIdentity(void)
{
    static APOLLO_AGG_CONFIG c;
    static APOLLO_AGG_COMPILED a, b;
    APOLLO_AGG_UNIT_RT rt[APOLLO_AGG_MAX_UNITS];
    unsigned long i;

    memset(&c, 0, sizeof(c));
    c.Version = APOLLO_AGG_VERSION;
    c.Bytes = sizeof(c);
    c.Flags = APOLLO_AGG_CFG_ENABLED | APOLLO_AGG_CFG_ALIGN;
    c.AsrcHalf = 16;
    c.UnitCount = 2;
    c.Master = 1;
    c.Unit[0].Model = APOLLO_INFO_MODEL_X8P;
    c.Unit[0].InCount = 32;
    c.Unit[0].OutCount = 34;
    c.Unit[1].Model = APOLLO_INFO_MODEL_TWINX;
    c.Unit[1].InCount = 34;
    c.Unit[1].OutCount = 46;
    for (i = 0; i < 32; i++) c.Unit[0].InIds[i] = (unsigned short)(0x0101 + i);
    for (i = 0; i < 34; i++) c.Unit[0].OutIds[i] = (unsigned short)(0x0201 + i);
    for (i = 0; i < 34; i++) c.Unit[1].InIds[i] = (unsigned short)(0x0301 + i);
    for (i = 0; i < 46; i++) c.Unit[1].OutIds[i] = (unsigned short)(0x0401 + i);
    memset(rt, 0, sizeof(rt));
    rt[0].Present = rt[1].Present = 1;
    rt[0].Guard = rt[1].Guard = 16;
    rt[0].Lead = rt[1].Lead = 144;
    rt[0].FrameCfg = rt[1].FrameCfg = 8;
    rt[0].Adc = 10;
    rt[1].Adc = 5;
    ApolloAggCompile(&c, rt, 1, &a);
    rt[0].Rate = rt[1].Rate = 48000;
    ApolloAggCompile(&c, rt, 1, &b);
    Check(memcmp(&a, &b, sizeof(a)) == 0, "layout: Rate 48000 = Rate 0 (G7 at 48 kHz), byte for byte");
    printf("layout at 48 kHz: latency in %lu out %lu (pad %lu)\n", a.InLatency, a.OutLatency, a.Pad);
    /* the same layout at 192 kHz: the same times */
    for (i = 0; i < 2; i++) {
        rt[i].Rate = 192000;
        rt[i].Guard = ApolloRateGuard(16, 192000);
        rt[i].Lead = ApolloRateLead(144, 192000);
        rt[i].FrameCfg = ApolloRateFrameCfg(8, 6);
        rt[i].Adc = ApolloRateAdc(i == 0 ? 10 : 5, 6);
    }
    ApolloAggCompile(&c, rt, 1, &b);
    printf("layout at 192 kHz: latency in %lu out %lu (pad %lu) = %.3f / %.3f ms (48 kHz %.3f / %.3f ms)\n", b.InLatency,
           b.OutLatency, b.Pad, b.InLatency / 192.0, b.OutLatency / 192.0, a.InLatency / 48.0, a.OutLatency / 48.0);
    Check(b.InCount == a.InCount && b.OutCount == a.OutCount, "192 kHz: the same channels");
    Check(fabs(b.OutLatency / 192.0 - a.OutLatency / 48.0) < 0.25 && b.Pad > a.Pad, "192 kHz: about the same time, M scaled");
}

/* ---------------------------------------------------------------- 5 */
typedef struct {
    double lockS, maxAbsE, ppmErr;
    unsigned long resyncs, outliers;
    int finalState;
} LOOPRES;

/*
 * The loop at Hz: positions advance Hz/1000 frames a tick (44.1 and 88.2 kHz: fractional, the reads
 * are whole frames), read noise Noise48 frames at 48 kHz = the same time at Hz. RateField 0 = the
 * clock's Rate left 0 (48 kHz).
 */
static LOOPRES Simulate(double Hz, double RateField, double TruePpm, double Noise48, double LateP, double Late48,
                        double JumpS, double Jump48, double Seconds)
{
    APOLLO_AGG_CLOCK c;
    LOOPRES r;
    double sc = Hz / 48000.0, tpos = 1000000.0, pf = 5555555.25, ratio = 1 + TruePpm * 1e-6;
    int t, ticks = (int)(Seconds * 1000), jumped = 0;

    memset(&c, 0, sizeof(c));
    memset(&r, 0, sizeof(r));
    r.lockS = -1;
    c.Rate = RateField;
    ApolloAggClockForm(&c, (long long)tpos, pf, 1.0);
    for (t = 0; t < ticks; t++) {
        double meas;
        int ev;
        tpos += Hz / 1000.0;
        pf += Hz / 1000.0 * ratio;
        if (!jumped && JumpS > 0 && t >= JumpS * 1000) {
            pf += Jump48 * sc;
            jumped = 1;
        }
        meas = pf + Noise48 * sc * Gauss();
        if (Uniform() < LateP) meas += Late48 * sc;
        ev = ApolloAggClockUpdate(&c, (long long)floor(tpos), meas - (tpos - floor(tpos)) * ratio);
        if (ev == APOLLO_AGG_EV_LOCKED && r.lockS < 0) r.lockS = t / 1000.0;
        if (c.State == APOLLO_AGG_CLOCK_LOCKED && t > 5000 && fabs(c.EFilt) > r.maxAbsE) r.maxAbsE = fabs(c.EFilt);
    }
    r.ppmErr = (c.Ri - 1) * 1e6 - TruePpm;
    r.resyncs = c.Resyncs;
    r.outliers = c.Outliers;
    r.finalState = c.State;
    return r;
}

static void TestLoop(void)
{
    unsigned long i;
    LOOPRES a, b;

    /* 48 kHz: the Rate field 0 and 48000 run the same loop, bit for bit */
    g_rng = 0x1234567ull;
    a = Simulate(48000, 0, 4.74, 0.3, 0.002, 8.0, 0, 0, 30);
    g_rng = 0x1234567ull;
    b = Simulate(48000, 48000, 4.74, 0.3, 0.002, 8.0, 0, 0, 30);
    Check(memcmp(&a, &b, sizeof(a)) == 0, "clock loop: Rate 48000 = Rate 0, bit for bit");
    for (i = 0; i < 6; i++) {
        double hz = (double)kHz[i], sc = hz / 48000.0;

        g_rng = 0x9E3779B97F4A7C15ull + i;
        a = Simulate(hz, hz, 4.74, 0.3, 0.0, 0, 0, 0, 60);
        printf("loop %6.0f Hz +4.74 ppm: lock %.2f s, |e| after 5 s <= %.3f frames (%.2f us), ratio error %.4f ppm, %lu resyncs\n",
               hz, a.lockS, a.maxAbsE, a.maxAbsE / hz * 1e6, a.ppmErr, a.resyncs);
        CHECKF(a.lockS >= 0 && a.lockS < 3.0, "%.0f Hz: locks within 3 s", hz);
        CHECKF(a.maxAbsE < 0.5 * sc, "%.0f Hz: stays within the lock window (the same time as 0.5 frame at 48 kHz)", hz);
        CHECKF(fabs(a.ppmErr) < 0.1, "%.0f Hz: ratio within 0.1 ppm", hz);
        CHECKF(a.resyncs == 0 && a.finalState == APOLLO_AGG_CLOCK_LOCKED, "%.0f Hz: no resync, locked", hz);
        b = Simulate(hz, hz, -50, 0.5, 0.002, 8.0, 0, 0, 60);
        CHECKF(b.lockS >= 0 && b.lockS < 3.0 && b.maxAbsE < 0.5 * sc && fabs(b.ppmErr) < 0.2 && b.resyncs == 0,
               "%.0f Hz: -50 ppm with late stamps absorbed", hz);
        b = Simulate(hz, hz, 4.74, 0.3, 0, 0, 20.0, 100.0, 40);
        CHECKF(b.resyncs == 1 && b.finalState == APOLLO_AGG_CLOCK_LOCKED && fabs(b.ppmErr) < 0.2,
               "%.0f Hz: a jump resyncs once and relocks", hz);
        b = Simulate(hz, hz, 900, 0.3, 0, 0, 0, 0, 20);
        CHECKF(b.finalState == APOLLO_AGG_CLOCK_DEGRADED, "%.0f Hz: 900 ppm is degraded", hz);
        /* the loop without its rate (as before rates could change) would mis-scale everything above 48 kHz */
        if (kHz[i] == 192000) {
            b = Simulate(hz, 0, 4.74, 0.3, 0, 0, 0, 0, 30);
            printf("loop 192000 Hz run as if 48 kHz (no Rate): lock %.2f s, %lu resyncs, final state %d\n", b.lockS,
                   b.resyncs, b.finalState);
        }
    }
}

/* ---------------------------------------------------------------- 6 */
static APOLLO_ASRC_TABLE g_tab;

static double SineErr(double FreqHz, double Hz, double Ratio, double Amp)
{
    static long in[200000];
    const double w = 2 * 3.14159265358979323846 * FreqHz / Hz;
    unsigned long h = g_tab.Half, j;
    long n;
    double err = 0, sig = 0;
    int m;
    float wt[2 * APOLLO_ASRC_MAX_HALF];

    for (n = 0; n < 200000; n++) {
        in[n] = (long)floor(Amp * sin(w * n) * 8388607.0 + 0.5) * 256;
    }
    for (m = 0; m < 150000; m++) {
        double z = 1000.123 + Ratio * m, got, ideal;
        long long k = ApolloAggFloor(z);
        float acc = 0;

        ApolloAsrcWeights(&g_tab, z - (double)k, wt);
        for (j = 0; j < 2 * h; j++) {
            acc += wt[j] * (float)in[k - (long long)h + 1 + (long long)j];
        }
        got = (double)(ApolloAsrcToSample(acc) & ~0xFFL) / 2147483648.0;
        ideal = Amp * sin(w * z);
        err += (got - ideal) * (got - ideal);
        sig += ideal * ideal;
    }
    return 10 * log10(err / sig);
}

static void TestAsrc(void)
{
    unsigned long i;

    ApolloAsrcBuild(&g_tab, 16);
    for (i = 0; i < 6; i++) {
        double hz = (double)kHz[i];
        double e1 = SineErr(1000, hz, 1 + 4.74e-6, 0.5), e10 = SineErr(10000, hz, 1 + 4.74e-6, 0.5);
        double e18 = SineErr(18000, hz, 1 + 4.74e-6, 0.5), e20 = SineErr(20000, hz, 1 + 4.74e-6, 0.5);

        printf("asrc h=16 at %6.0f Hz: error vs exact (-6 dBFS sine) 1 kHz %.1f dB, 10 kHz %.1f dB, 18 kHz %.1f dB, "
               "20 kHz %.1f dB\n", hz, e1, e10, e18, e20);
        CHECKF(e1 < -100.0 && e10 < -95.0, "%.0f Hz: 1 and 10 kHz at least 95-100 dB clean", hz);
        if (hz >= 88200) {
            CHECKF(e18 < -100.0 && e20 < -100.0, "%.0f Hz: the whole audio band at least 100 dB clean", hz);
        }
        if (hz == 48000) {
            CHECKF(e1 < -100.0 && e10 < -100.0, "48 kHz: as asrc-test (1 and 10 kHz at least 100 dB clean)");
        }
        if (hz == 44100) {
            /* 44.1 kHz: 20 kHz is 0.907 of Nyquist, in the filter's transition band; where it is clean */
            double e14 = SineErr(14000, hz, 1 + 4.74e-6, 0.5), e16 = SineErr(16000, hz, 1 + 4.74e-6, 0.5);

            printf("asrc h=16 at  44100 Hz: 14 kHz %.1f dB, 16 kHz %.1f dB\n", e14, e16);
            CHECKF(e14 < -95.0, "44.1 kHz h 16: clean to 14 kHz (%.1f dB)", e14);
        }
    }
    ApolloAsrcBuild(&g_tab, 32);
    {
        double e16 = SineErr(16000, 44100, 1 + 4.74e-6, 0.5), e18 = SineErr(18000, 44100, 1 + 4.74e-6, 0.5);
        double e20 = SineErr(20000, 44100, 1 + 4.74e-6, 0.5);

        printf("asrc h=32 at  44100 Hz: 16 kHz %.1f dB, 18 kHz %.1f dB, 20 kHz %.1f dB\n", e16, e18, e20);
        CHECKF(e16 < -95.0, "44.1 kHz h 32: clean to 16 kHz (%.1f dB)", e16);
    }
}

/* ---------------------------------------------------------------- 7 */
/*
 * [Clock] driver\clock_plan.h: the clock sources per model against the units' live 0xC08C words, the refusals, the
 * rates a source carries, the digital channels per format and rate (S/MUX), the masked format writes, and the combined
 * layout following a format (apollo_agg.h ApolloAggSlotShownAt): Dig 0 = the layout before this code at every rate.
 */
static void TestClock(void)
{
    const APOLLO_CLOCK_MODEL *x = ApolloClockModelFor(APOLLO_MODEL_X8P), *t = ApolloClockModelFor(APOLLO_MODEL_TWINX);
    const unsigned long x8pSrcs = 0x14A1007Eul, twxSrcs = 0x1421007Eul;     /* [HW] live reads */
    const unsigned long known = APOLLO_DIG_KNOWN, inAdat = APOLLO_DIG_KNOWN | APOLLO_DIG_IN_ADAT;
    unsigned long i, s, v, m, ok, idx, id;
    int r;

    Check(x != 0 && t != 0 && ApolloClockModelFor(0) == 0, "clock plans: x8p, Twin X, nothing else");
    Check(ApolloClockWord(2, APOLLO_CK_INTERNAL) == 0x20C && ApolloClockWord(6, APOLLO_CK_ADAT) == 0x605,
          "clock word: (index << 8) | source (0x20C = 48 kHz Internal, as every start writes)");
    Check(ApolloClockOffered(x8pSrcs, APOLLO_CK_WORDCLOCK) && ApolloClockOffered(x8pSrcs, APOLLO_CK_SPDIF) &&
          ApolloClockOffered(x8pSrcs, APOLLO_CK_ADAT) && ApolloClockOffered(x8pSrcs, APOLLO_CK_PEER),
          "x8p 0x14A1007E offers Word Clock, S/PDIF, ADAT and Peer");
    Check(!ApolloClockOffered(twxSrcs, APOLLO_CK_WORDCLOCK) && ApolloClockOffered(twxSrcs, APOLLO_CK_SPDIF) &&
          ApolloClockOffered(twxSrcs, APOLLO_CK_ADAT), "Twin X 0x1421007E: S/PDIF and ADAT, no Word Clock");
    Check(ApolloClockOffered(0, APOLLO_CK_INTERNAL) && !ApolloClockOffered(0xFFFFFFFFul, APOLLO_CK_SPDIF),
          "an unread or dead 0xC08C offers Internal only");
    /* the refusals, in order */
    Check(ApolloClockCheck(x, APOLLO_CK_INTERNAL, x8pSrcs, 2, 0, 0) == APOLLO_CKCHK_OK, "Internal: always, no try");
    Check(ApolloClockCheck(x, APOLLO_CK_PEER, x8pSrcs, 2, 0, 1) == APOLLO_CKCHK_INVALID, "Peer 0x0A: never selectable");
    Check(ApolloClockCheck(x, 0x03, x8pSrcs, 2, 0, 1) == APOLLO_CKCHK_INVALID, "an unknown source id: invalid");
    Check(ApolloClockCheck(t, APOLLO_CK_WORDCLOCK, 0xFFFF007Eul, 2, 0, 1) == APOLLO_CKCHK_NOT_HERE,
          "Twin X Word Clock: no jack, whatever the unit's word says");
    Check(ApolloClockCheck(x, APOLLO_CK_WORDCLOCK, 0x1021007Eul, 2, 0, 1) == APOLLO_CKCHK_NOT_OFFERED,
          "x8p Word Clock not in 0xC08C: not offered");
    Check(ApolloClockCheck(x, APOLLO_CK_WORDCLOCK, x8pSrcs, 2, 0, 0) == APOLLO_CKCHK_UNPROVEN &&
          ApolloClockCheck(x, APOLLO_CK_WORDCLOCK, x8pSrcs, 2, 0, 1) == APOLLO_CKCHK_OK,
          "x8p Word Clock: unproven without try, fine with it");
    Check(ApolloClockCheck(x, APOLLO_CK_WORDCLOCK, x8pSrcs, 6, 0, 1) == APOLLO_CKCHK_OK,
          "Word Clock carries 192 kHz");
    Check(ApolloClockCheck(t, APOLLO_CK_SPDIF, twxSrcs, 5, 0, 1) == APOLLO_CKCHK_RATE &&
          ApolloClockCheck(t, APOLLO_CK_SPDIF, twxSrcs, 4, 0, 1) == APOLLO_CKCHK_OK,
          "S/PDIF: 44.1-96 kHz (176.4 refused, 96 fine)");
    Check(ApolloClockCheck(t, APOLLO_CK_ADAT, twxSrcs, 2, 0, 1) == APOLLO_CKCHK_PORT &&
          ApolloClockCheck(t, APOLLO_CK_ADAT, twxSrcs, 2, known, 1) == APOLLO_CKCHK_PORT &&
          ApolloClockCheck(t, APOLLO_CK_ADAT, twxSrcs, 2, inAdat, 1) == APOLLO_CKCHK_OK,
          "ADAT clock: only once the optical input was set to ADAT here");
    Check(ApolloClockCheck(t, APOLLO_CK_ADAT, twxSrcs, 6, inAdat, 1) == APOLLO_CKCHK_RATE, "ADAT: not at 192 kHz");
    Check(ApolloClockCheck(t, APOLLO_CK_SPDIF, twxSrcs, 2, inAdat, 1) == APOLLO_CKCHK_PORT &&
          ApolloClockCheck(x, APOLLO_CK_SPDIF, x8pSrcs, 2, inAdat, 1) == APOLLO_CKCHK_OK,
          "S/PDIF clock with the optical input on ADAT: refused on the Twin X (optical only), fine on the x8p (coax)");
    Check(ApolloClockCheck(t, APOLLO_CK_SPDIF, twxSrcs, 2, APOLLO_DIG_KNOWN | APOLLO_DIG_SRC, 1) == APOLLO_CKCHK_SRC,
          "S/PDIF clock with SR Convert on: refused");
    m = ApolloClockUsable(t, twxSrcs, 2, 0);
    Check(m == (APOLLO_CK_BIT(APOLLO_CK_INTERNAL) | APOLLO_CK_BIT(APOLLO_CK_SPDIF)),
          "Twin X usable at 48 kHz, nothing set: Internal, S/PDIF (ADAT needs the format)");
    m = ApolloClockUsable(x, x8pSrcs, 2, inAdat);
    Check(m == (APOLLO_CK_BIT(APOLLO_CK_INTERNAL) | APOLLO_CK_BIT(APOLLO_CK_SPDIF) | APOLLO_CK_BIT(APOLLO_CK_ADAT) |
                APOLLO_CK_BIT(APOLLO_CK_WORDCLOCK)), "x8p usable with ADAT in: all four");
    m = ApolloClockUsable(x, x8pSrcs, 6, inAdat);
    Check(m == (APOLLO_CK_BIT(APOLLO_CK_INTERNAL) | APOLLO_CK_BIT(APOLLO_CK_WORDCLOCK)), "x8p usable at 192 kHz: Internal, Word Clock");
    Check(x->ProvenSources == APOLLO_CK_BIT(APOLLO_CK_INTERNAL) && t->ProvenSources == APOLLO_CK_BIT(APOLLO_CK_INTERNAL) &&
          x->ProvenDigital == 0 && t->ProvenDigital == 0, "proven on these units: Internal only, no format write");
    /* rates on a source; rate_plan.h's allowance follows the source asked for */
    Check(ApolloClockRatesAllowed(x8pSrcs, APOLLO_CK_INTERNAL) == 0x7E && ApolloClockRatesAllowed(x8pSrcs, APOLLO_CK_ADAT) == 0x1E &&
          ApolloClockRatesAllowed(x8pSrcs, APOLLO_CK_SPDIF) == 0x1E && ApolloClockRatesAllowed(x8pSrcs, APOLLO_CK_WORDCLOCK) == 0x7E,
          "rates on a source: S/PDIF and ADAT 44.1-96, Internal and Word Clock all six");
    Check(ApolloRateAllowed(x8pSrcs, APOLLO_RATE_SOURCE_ADAT) == 0x1E && ApolloRateAllowed(x8pSrcs, CLOCK_SOURCE_INTERNAL) == 0x7E &&
          ApolloRateAllowed(x8pSrcs, APOLLO_RATE_SOURCE_SPDIF) == 0x1E, "ApolloRateAllowed: ADAT now 96 kHz at most too");
    /* digital controls: masked mailbox writes */
    ok = 1;
    for (i = 1; i <= 3; i++) {
        unsigned long st, val, msk;

        for (v = 0; v <= 1; v++) {
            ok &= ApolloDigEncode(i, v, &st, &val, &msk) == 1 && val == (v ? msk : 0ul) &&
                  ((i == APOLLO_DIGW_OPT_IN && st == 8 && msk == 0x10) || (i == APOLLO_DIGW_OPT_OUT && st == 8 && msk == 0x08) ||
                   (i == APOLLO_DIGW_SRC && st == 1 && msk == 0x80000000ul));
        }
    }
    Check(ok, "format writes: S8 bit 0x10 (in ADAT), S8 bit 0x08 (out ADAT), S1 bit 31 (SR Convert), masked alone");
    Check(ApolloDigCheck(t, APOLLO_DIGW_OPT_OUT, 1, APOLLO_CK_INTERNAL, 1) == APOLLO_CKCHK_NOT_HERE &&
          ApolloDigCheck(x, APOLLO_DIGW_OPT_OUT, 1, APOLLO_CK_INTERNAL, 1) == APOLLO_CKCHK_OK,
          "optical output format: the x8p's, not the Twin X's");
    Check(ApolloDigCheck(t, APOLLO_DIGW_OPT_IN, 1, APOLLO_CK_INTERNAL, 0) == APOLLO_CKCHK_UNPROVEN,
          "format writes are unproven: try needed");
    Check(ApolloDigCheck(t, APOLLO_DIGW_OPT_IN, 0, APOLLO_CK_ADAT, 1) == APOLLO_CKCHK_PORT &&
          ApolloDigCheck(t, APOLLO_DIGW_OPT_IN, 1, APOLLO_CK_SPDIF, 1) == APOLLO_CKCHK_PORT &&
          ApolloDigCheck(x, APOLLO_DIGW_OPT_IN, 1, APOLLO_CK_SPDIF, 1) == APOLLO_CKCHK_OK,
          "the optical input's format stays while the clock runs from it (x8p S/PDIF: coax)");
    Check(ApolloDigCheck(t, APOLLO_DIGW_SRC, 1, APOLLO_CK_SPDIF, 1) == APOLLO_CKCHK_SRC &&
          ApolloDigCheck(t, APOLLO_DIGW_SRC, 0, APOLLO_CK_SPDIF, 1) == APOLLO_CKCHK_OK, "SR Convert on refused on a S/PDIF clock");
    /* channels per format and rate */
    Check(ApolloDigAdatChannels(t, 1, 2, inAdat) == 8 && ApolloDigAdatChannels(t, 1, 4, inAdat) == 4 &&
          ApolloDigAdatChannels(t, 1, 6, inAdat) == 0 && ApolloDigAdatChannels(t, 1, 2, known) == 0 &&
          ApolloDigAdatChannels(t, 0, 2, inAdat) == 0, "Twin X ADAT in: 8 / 4 (S/MUX) / 0; none in S/PDIF format; no output");
    Check(ApolloDigAdatChannels(x, 1, 4, inAdat) == 8 && ApolloDigAdatChannels(x, 0, 3, APOLLO_DIG_KNOWN | APOLLO_DIG_OUT_ADAT) == 8 &&
          ApolloDigAdatChannels(x, 0, 2, inAdat) == 0, "x8p ADAT 8 at 88.2/96 (two ports, S/MUX); output by its own format");
    Check(ApolloDigSpdifChannels(t, 1, 2, known) == 2 && ApolloDigSpdifChannels(t, 1, 2, inAdat) == 0 &&
          ApolloDigSpdifChannels(t, 1, 5, known) == 0 && ApolloDigSpdifChannels(x, 1, 2, inAdat) == 2 &&
          ApolloDigSpdifChannels(x, 0, 6, known) == 0, "S/PDIF: by format on the Twin X, coax on the x8p, never above 96 kHz");
    /* the slot rule: Dig 0 = everything live at every rate (the layout as before) */
    ok = 1;
    for (i = 0; i < 6; i++) {
        for (id = 0; id < 0x1000; id++) {
            ok &= ApolloDigSlotLive(APOLLO_MODEL_X8P, 1, id, kHz[i], 0) == 1 && ApolloDigSlotLive(APOLLO_MODEL_TWINX, 0, id, kHz[i], 0) == 1;
        }
    }
    Check(ok, "nothing set: every slot live at every rate, both models (layouts unchanged)");
    idx = 0;
    for (s = 1; s <= 8; s++) {
        idx += (unsigned long)ApolloDigSlotLive(APOLLO_MODEL_TWINX, 1, 0x0400 | s, 96000, inAdat);
    }
    Check(idx == 4 && ApolloDigSlotLive(APOLLO_MODEL_TWINX, 1, 0x0501, 96000, inAdat) == 0 &&
          ApolloDigSlotLive(APOLLO_MODEL_TWINX, 1, 0x0001, 192000, inAdat) == 1,
          "Twin X ADAT in at 96 kHz: ADAT 1-4 live, S/PDIF silent, analog untouched");
    r = ApolloDigSlotLive(APOLLO_MODEL_X8P, 0, 0x0501, 176400, known);
    Check(r == 0 && ApolloDigSlotLive(APOLLO_MODEL_X8P, 0, 0x0408, 48000, known) == 0 &&
          ApolloDigSlotLive(APOLLO_MODEL_X8P, 0, 0x0408, 48000, APOLLO_DIG_KNOWN | APOLLO_DIG_OUT_ADAT) == 1,
          "x8p out: S/PDIF silent at 176.4, ADAT silent in S/PDIF format, live in ADAT");

    /* the combined layout: Dig 0 = the old filter bit for bit; a format hides the slots that carry nothing */
    {
        static APOLLO_AGG_CONFIG c;
        static APOLLO_AGG_COMPILED a, b;
        APOLLO_AGG_UNIT_RT rt[APOLLO_AGG_MAX_UNITS];
        unsigned long adatIn = 0, spdifIn = 0;

        ok = 1;
        for (id = 0; id < 0x1000; id++) {
            for (s = 1; s <= 2; s++) {
                for (i = 0; i < 6; i++) {
                    ok &= ApolloAggSlotShownAt(s, 1, (unsigned short)id, kHz[i], 0) == ApolloAggSlotShown(s, 1, (unsigned short)id) &&
                          ApolloAggSlotShownAt(s, 0, (unsigned short)id, kHz[i], 0) == ApolloAggSlotShown(s, 0, (unsigned short)id);
                }
            }
        }
        Check(ok, "ApolloAggSlotShownAt with no format = ApolloAggSlotShown, every id, both models, every rate");
        memset(&c, 0, sizeof(c));
        memset(rt, 0, sizeof(rt));
        c.Version = APOLLO_AGG_VERSION;
        c.Bytes = sizeof(c);
        c.Flags = APOLLO_AGG_CFG_ENABLED | APOLLO_AGG_CFG_ALIGN;
        c.AsrcHalf = 16;
        c.UnitCount = 1;
        c.Unit[0].Uid = 0x2F00BEEFull;
        c.Unit[0].Model = APOLLO_INFO_MODEL_TWINX;
        c.Unit[0].InCount = 18;
        for (i = 0; i < 8; i++) {
            c.Unit[0].InIds[i] = (unsigned short)(0x0401 + i);      /* ADAT 1-8 */
        }
        c.Unit[0].InIds[8] = 0x0501;
        c.Unit[0].InIds[9] = 0x0502;
        for (i = 10; i < 18; i++) {
            c.Unit[0].InIds[i] = (unsigned short)(0x0001 + (i & 1));
        }
        rt[0].Present = 1;
        rt[0].Guard = 16;
        rt[0].Lead = 144;
        rt[0].FrameCfg = 8;
        rt[0].Adc = 5;
        ApolloAggCompile(&c, rt, 0, &a);
        rt[0].Rate = 96000;
        rt[0].Dig = inAdat;
        ApolloAggCompile(&c, rt, 0, &b);
        for (i = 0; i < b.InCount; i++) {
            adatIn += (b.In[i].Id >> 8) == 0x04;
            spdifIn += (b.In[i].Id >> 8) == 0x05;
        }
        CHECKF(a.InCount == 18 && b.InCount == 12 && adatIn == 4 && spdifIn == 0,
               "combined layout follows the format: Twin X 18 inputs as before, ADAT at 96 kHz 12 (ADAT 1-4, no S/PDIF; got %lu, %lu, %lu)",
               b.InCount, adatIn, spdifIn);
    }
}

int main(void)
{
    TestTable();
    TestBuffers();
    TestLatency();
    TestStrips();
    TestAggConstants();
    TestAggIdentity();
    TestLoop();
    TestAsrc();
    TestClock();
    printf("rate-test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
