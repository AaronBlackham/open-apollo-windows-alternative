/*
 * rate_plan.h - [Rate] the sample rates as data: the six rate indexes, what each one changes (the
 * FRAME_CFG band, frames per 1 ms tick, the copy engine's lead and guard, the ADC delay), which rates
 * a unit offers, and the clock-change constants. Plain C with no OS calls: apollo.sys (rate.c,
 * audio.c, engine.c, stream.c, asio.c, agg.c) and the offline check tools\rate-test build the same
 * code. The includer defines nothing; every value is unsigned long.
 *
 * Hardware facts, all from open-apollo (GPL-2.0):
 *   rate index 1..6 = 44.1, 48, 88.2, 96, 176.4, 192 kHz            oa:driver/ua_audio.c:1928-1935
 *   rate band 0 = 44.1/48, 1 = 88.2/96, 2 = 176.4/192               :1912-1925
 *   AX_FRAME_CFG (the safety offset) 8 / 16 / 32 frames per band    :1937-1950 (the kext's values)
 *   clock change: 0xC074 = (index << 8) | source, doorbell 4, ack   :4400-4468 (bits 5, 22, 4; 2 s)
 *   firmware rate polled 25 ms apart up to 4 s, rate ack, 1.5 s     :5343-5453 (the kext's order)
 *   leaving 176.4/192 kHz before 3 s wedged the firmware (x4)       :4331-4343 (hi_dwell_ms)
 *   a Hi-band start from a Hi-band clock needs a 48 kHz session     :5228-5300 (400 / 1200 ms)
 *   ADC delay: the x8p 10 frames, 11 at 176.4/192; the Twin X 5, 6  oa:driver/ua_models_gen.h adc_mframes
 *   0xC08C: [31:16] clock sources, [7:0] rates (bit n = index n)   spec-twinx 6.3.7 (x8p 0x7E, Twin X 0x7E)
 *   S/PDIF carries 44.1-96 kHz only (a S/PDIF clock limits the unit to 96 kHz)
 *
 * The copy engine's numbers are frames at 48 kHz in the registry (RenderLeadFrames 144 = 3 ms,
 * CaptureGuardFrames 16); at another rate they are scaled to the same time, rounded up. At 48 kHz
 * every function here returns exactly what the driver used before rates could change.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#define APOLLO_RATE_IDX_MIN             1ul
#define APOLLO_RATE_IDX_MAX             6ul
#define APOLLO_RATE_IDX_48K             2ul
#define APOLLO_RATE_MASK_ALL            0x7Eul      /* bit n = rate index n: the six rates */
#define APOLLO_RATE_MASK_SPDIF          0x1Eul      /* 44.1-96 kHz: what a S/PDIF clock carries */
#define APOLLO_RATE_DEFAULT_HZ          48000ul
#define APOLLO_RATE_SOURCE_SPDIF        0x00ul      /* clock source 0 (regs.h CLOCK_SOURCE_INTERNAL = 0x0C) */
#define APOLLO_RATE_SOURCE_ADAT         0x05ul      /* [Clock] ADAT: 44.1-96 kHz too (88.2/96 with S/MUX; clock_plan.h) */

/* The clock change, as the kext (oa:driver/ua_audio.c:4383-4468, 5343-5453) */
#define APOLLO_RATE_ACK_BITS            0x00400030ul    /* 0xC030 bit 5 (the kext's), 22 and 4 */
#define APOLLO_RATE_CLOCK_ACK_MS        2000ul      /* per attempt; one retry */
#define APOLLO_RATE_FW_POLL_MS          25ul
#define APOLLO_RATE_FW_WAIT_MS          4000ul      /* a timeout is only logged, as the kext */
#define APOLLO_RATE_ACK_HOLD_MS         1500ul      /* transport stopped after the DSP rate ack */
#define APOLLO_RATE_HI_DWELL_MS         3000ul      /* at 176.4/192 kHz at least this long */
#define APOLLO_RATE_LOMID_FROM_192K_MS  400ul       /* the 48 kHz session before a Hi start */
#define APOLLO_RATE_LOMID_FROM_176K4_MS 1200ul
#define APOLLO_RATE_LOCK_CHECK_MS       2000ul      /* after the transport start: 0xC07C locked */

/* The copy engine (engine.c): the most the scaled lead may be, under the zero horizon (4096 frames) */
#define APOLLO_RATE_LEAD_MAX            2048ul
#define APOLLO_RATE_GUARD_MAX48         64ul        /* CaptureGuardFrames limit, frames at 48 kHz */

/* Hz of a rate index, 0 for anything else. */
static __inline unsigned long ApolloRateHz(unsigned long Index)
{
    static const unsigned long hz[7] = { 0, 44100, 48000, 88200, 96000, 176400, 192000 };

    return Index <= APOLLO_RATE_IDX_MAX ? hz[Index] : 0;
}

/* Rate index of Hz, 0 for a rate the Apollo does not run at. */
static __inline unsigned long ApolloRateIndex(unsigned long Hz)
{
    unsigned long i;

    for (i = APOLLO_RATE_IDX_MIN; i <= APOLLO_RATE_IDX_MAX; i++) {
        if (ApolloRateHz(i) == Hz) {
            return i;
        }
    }
    return 0;
}

/* Rate band: 0 = 44.1/48, 1 = 88.2/96, 2 = 176.4/192 kHz (0 for an unknown index, as upstream). */
static __inline unsigned long ApolloRateBand(unsigned long Index)
{
    return (Index == 3 || Index == 4) ? 1ul : ((Index == 5 || Index == 6) ? 2ul : 0ul);
}

static __inline int ApolloRateIsHi(unsigned long Index)
{
    return ApolloRateBand(Index) == 2;
}

/* 1 = the 48 kHz family (48, 96, 192), 0 = the 44.1 kHz family; the units' crystals per family. */
static __inline int ApolloRateFamily48(unsigned long Hz)
{
    return Hz == 48000 || Hz == 96000 || Hz == 192000;
}

/* AX_FRAME_CFG at Index from the plan's 44.1/48 kHz value: 8 -> 16 -> 32 (the kext's band values). */
static __inline unsigned long ApolloRateFrameCfg(unsigned long FrameCfg48k, unsigned long Index)
{
    return FrameCfg48k << ApolloRateBand(Index);
}

/* Frames48 (frames at 48 kHz) as frames at Hz for the same time, rounded up; Frames48 at 48 kHz. */
static __inline unsigned long ApolloRateScale(unsigned long Frames48, unsigned long Hz)
{
    if (Hz == 0 || Hz == 48000) {
        return Frames48;
    }
    return (unsigned long)(((unsigned long long)Frames48 * Hz + 47999ull) / 48000ull);
}

/* Frames one 1 ms engine tick covers at Hz (48 at 48 kHz). */
static __inline unsigned long ApolloRateTickFrames(unsigned long Hz)
{
    return Hz == 0 ? 48ul : (Hz + 999ul) / 1000ul;
}

/* The engine's render lead at Hz from RenderLeadFrames (frames at 48 kHz), within the zero horizon. */
static __inline unsigned long ApolloRateLead(unsigned long Lead48, unsigned long Hz)
{
    unsigned long l = ApolloRateScale(Lead48, Hz);

    return l > APOLLO_RATE_LEAD_MAX ? APOLLO_RATE_LEAD_MAX : l;
}

/* The capture guard at Hz from CaptureGuardFrames (frames at 48 kHz). */
static __inline unsigned long ApolloRateGuard(unsigned long Guard48, unsigned long Hz)
{
    return ApolloRateScale(Guard48 > APOLLO_RATE_GUARD_MAX48 ? APOLLO_RATE_GUARD_MAX48 : Guard48, Hz);
}

/* The ADC delay at Index from the model's 44.1-96 kHz value: one frame more at 176.4/192 kHz. */
static __inline unsigned long ApolloRateAdc(unsigned long Adc48, unsigned long Index)
{
    return Adc48 + (ApolloRateIsHi(Index) ? 1ul : 0ul);
}

/*
 * The rates a unit offers, from its live 0xC08C word: bits 1-6 of [7:0]. A word that offers none of
 * the six (not read yet, or a dead read) offers 48 kHz only: the rate every bring-up was run at.
 */
static __inline unsigned long ApolloRateOffered(unsigned long ClockSrcs)
{
    unsigned long m = ClockSrcs & APOLLO_RATE_MASK_ALL;

    if (ClockSrcs == 0xFFFFFFFFul || m == 0) {
        return 1ul << APOLLO_RATE_IDX_48K;
    }
    return m;
}

/* What the driver lets a unit switch to: the offered rates, 96 kHz at most on a S/PDIF or [Clock] ADAT clock. */
static __inline unsigned long ApolloRateAllowed(unsigned long ClockSrcs, unsigned long Source)
{
    unsigned long m = ApolloRateOffered(ClockSrcs);

    return (Source == APOLLO_RATE_SOURCE_SPDIF || Source == APOLLO_RATE_SOURCE_ADAT) ? (m & APOLLO_RATE_MASK_SPDIF) : m;
}

/*
 * The 48 kHz session a Hi-band start needs when the clock already runs in the Hi band (upstream
 * ua_audio_hi_from_hi): its length in ms, 0 = none needed. FwIndex = the firmware's rate now (0xC07C).
 */
static __inline unsigned long ApolloRateLoMidHoldMs(unsigned long FwIndex, unsigned long TargetIndex)
{
    if (!ApolloRateIsHi(TargetIndex)) {
        return 0;
    }
    if (FwIndex == 6) {
        return APOLLO_RATE_LOMID_FROM_192K_MS;
    }
    return FwIndex == 5 ? APOLLO_RATE_LOMID_FROM_176K4_MS : 0;
}

/*
 * The Hi-band dwell (upstream hi_dwell_ms): how long to wait before a clock write to NewIndex when
 * the clock was written at HiIndex (5 or 6; 0 = not in the Hi band) ElapsedMs ago.
 */
static __inline unsigned long ApolloRateDwellMs(unsigned long HiIndex, unsigned long ElapsedMs, unsigned long NewIndex)
{
    if (!ApolloRateIsHi(HiIndex) || NewIndex == HiIndex || ElapsedMs >= APOLLO_RATE_HI_DWELL_MS) {
        return 0;
    }
    return APOLLO_RATE_HI_DWELL_MS - ElapsedMs;
}

/* NOTIF_RATE_INFO (0xC07C) says Index, clock locked ([1:0] == 1: the Mixer Engine's GetLockedState). */
static __inline int ApolloRateInfoLocked(unsigned long RateInfo, unsigned long Index)
{
    return RateInfo != 0xFFFFFFFFul && ((RateInfo >> 8) & 0xFFul) == Index && (RateInfo & 3ul) == 1ul;
}

/*
 * [Lat] What a unit adds to the copy engine's numbers in the latencies it reports (ASIO GetLatencies, the Windows
 * endpoints; docs/ASIO.md, Latency). UA's own engine reports, per unit: input = ADC + the DSP processing block +
 * the delay-compensation budget + 8, output = DAC + 4 (oa:mixer-engine/model_features.py latency_figures, from
 * MixerTypeBase::GetAsioLatencyForDelayComp). Here the host side (UA's +8 / +4) is this driver's own copy engine
 * (capture guard, render lead, FRAME_CFG), so only the device side is taken from UA:
 *   - the strip DSP's processing block: 64 frames, 128 above 100 kHz (= twice the strips' block size, 32 / 64:
 *     strip_plan.h g_StripRamp), only while the input strips run (no strip, no DSP on the record path);
 *   - the plain Mic strip's record delay: 200 samples at every rate (strip_plan.h m0 word 16 = 0xc8, UA's "Medium"
 *     budget; measured upstream: a record delay of 1000 instead recorded exactly 800 samples later);
 *   - a routed chain's record path over those 200 (an over-budget chain): the worst one, while it runs;
 *   - the DAC: the model's table (bringup_plan.h DacCenti), rounded to whole frames.
 */
#define APOLLO_LAT_STRIP_RECORD_DELAY   200ul       /* = plugin_host.h kPlainRecordDelay, kDelayBudget */
#define APOLLO_LAT_IN_PATH_MAX          4096ul      /* sanity cap of the reported input path */

/* The strip DSP's processing block at Index: 64 frames, 128 at 176.4/192 kHz. */
static __inline unsigned long ApolloRateDspBlock(unsigned long Index)
{
    return ApolloRateIsHi(Index) ? 128ul : 64ul;
}

/* The input path beyond the engine and the ADC: StripsLive = the unit's strips run; Excess = the worst chain's. */
static __inline unsigned long ApolloLatencyInPath(int StripsLive, unsigned long Index, unsigned long Excess)
{
    unsigned long p;

    if (!StripsLive) {
        return 0;
    }
    p = ApolloRateDspBlock(Index) + APOLLO_LAT_STRIP_RECORD_DELAY + Excess;
    return p > APOLLO_LAT_IN_PATH_MAX ? APOLLO_LAT_IN_PATH_MAX : p;
}

/* A chain's record path (record delay + the plug-in latency through its tap) over the plain strip's 200. */
static __inline unsigned long ApolloLatencyChainExcess(unsigned long RecordDelay, unsigned long RecordLatency)
{
    unsigned long path = RecordDelay + RecordLatency;

    return path > APOLLO_LAT_STRIP_RECORD_DELAY ? path - APOLLO_LAT_STRIP_RECORD_DELAY : 0ul;
}

/* The DAC delay at Index from a plan's DacCenti (1/100 frame per band), rounded: 0 without a table. */
static __inline unsigned long ApolloRateDac(const unsigned long DacCenti[3], unsigned long Index)
{
    return DacCenti != 0 ? (DacCenti[ApolloRateBand(Index)] + 50ul) / 100ul : 0ul;
}

/* "44.1 kHz" etc. for log lines and messages ("?" for anything else). */
static __inline const char *ApolloRateText(unsigned long Hz)
{
    switch (Hz) {
    case 44100:  return "44.1 kHz";
    case 48000:  return "48 kHz";
    case 88200:  return "88.2 kHz";
    case 96000:  return "96 kHz";
    case 176400: return "176.4 kHz";
    case 192000: return "192 kHz";
    default:     return "?";
    }
}
