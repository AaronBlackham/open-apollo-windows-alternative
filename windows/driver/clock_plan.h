/*
 * clock_plan.h - [Clock] clock source, digital I/O formats and the front panel of each unit, as data:
 * which sources and digital ports a model has, what each one carries at each rate, the mailbox fields
 * that switch them, the readback words the front panel shows up in, and the monitor link's arithmetic.
 * Plain C like rate_plan.h and monitor_plan.h: no kernel or Windows headers. apollo.sys (clock.c, panel.c,
 * agg.c, stream.c), apollo-diag, Apollo Control's host (app\src\clock_host.h) and the offline checks
 * (tools\rate-test, tools\mon-plan-test, tools\agg-test) build the same code.
 *
 * Evidence (oa: = apollo-win\reference\open-apollo, read only; [HW] = read on Aaron's units):
 *   clock word          0xC074 = (rate index << 8) | source, doorbell 4, ack 0xC030 bit 5/22/4 (oa:driver/ua_audio.c:4400-4468)
 *   source ids          S/PDIF 0x00, ADAT 0x05, Word Clock 0x07, Peer 0x0A, Internal 0x0C (oa:driver/ua_apollo.h:917-928);
 *                       the unit lists its own in 0xC08C [31:16], bit n = source n. [HW] x8p 0x14A1 (S/PDIF, ADAT,
 *                       Word Clock, Peer, Internal), Twin X 0x1421 (S/PDIF, ADAT, Peer, Internal), live reads
 *   lock                0xC07C [1:0] == 1 at the rate index (the Mixer Engine's GetLockedState); readback word 6
 *                       bit 31, valid when bit 30 is set (oa:driver/ua_apollo.h:882-883). [HW] both units 0xC0000000
 *                       set at 48 kHz internal
 *   source change, run  [HW] G6 (spec-twinx 6.9.6): source 0x0A written with the transport running on both units,
 *                       acked in 10-42 ms, 0xC074 read back 0x20A, 0xC084 read 48000, then the lock bit DROPPED
 *                       (0x201 -> 0x200) and the unit free-ran; 0x0C written back: locked, no glitch. So a source
 *                       that acks is not yet a source that locks: the lock must hold for APOLLO_CK_SETTLE_MS.
 *   0xC084              "clock source info": 48000 on that probe. Hz of what, unknown: shown, never trusted.
 *   S/PDIF, ADAT rates  44.1-96 kHz (S/PDIF by standard; ADAT 88.2/96 with S/MUX); docs/console-parity.md
 *                       "Sample rate 44.1-192 kHz (S/PDIF <= 96)"
 *   device-mode word    other param 0x21 -> S8 [7:0]: 0x08 TOSLink out = ADAT, 0x10 TOSLink in = ADAT, 0x20/0x28/0x30
 *                       converter mode, [2:0] multi-unit role, [7:6] order (oa:driver/ua_apollo.h:1922-1929, the
 *                       kext's PushDeviceMode). The input bit is UNVERIFIED upstream (oa:driver/ua_audio.c:1198-1201,
 *                       toslink_in_bit off by default: "needs an optical source to verify"); the output bit is the
 *                       Mixer Engine's "DigitalOutputMode" 0x21 = 0/8 (oa:mixer-engine/hardware.py:542). Our connect
 *                       batch writes S8 = 0x20/0xFF on both units (spec-twinx 4.3): TOSLink in and out = S/PDIF.
 *                       Writes here are MASKED to bit 0x08 or 0x10 alone, so the converter and role bits stay.
 *   SR Convert          other param 0x1F -> S1 bit 31 (oa:driver/ua_core.c:2016-2020, kext SetOtherParam);
 *                       "sample rate converter on/off (S/PDIF inputs)" (oa:mixer-engine/hardware.py:543)
 *   ports per model     Twin X Gen 1: one optical INPUT, ADAT or S/PDIF, no digital output, no word clock
 *                       (spec-twinx 0.1, oa:devices/apollo-twin-x.json); ADAT 5-8 hidden at 88.2/96 kHz (S/MUX,
 *                       oa:driver/ua_models_gen.h adat_mid_rate_max 4). x8p Gen 1: word clock in/out, optical in
 *                       and out (format per direction, oa:ua_models_gen.h UA_TOSLINK_PER_DIR), S/PDIF coax (Aaron's
 *                       hardware list; NOTE open-apollo's route lists and UA's x8p device map offer S/PDIF only in the
 *                       optical port's S/PDIF format, so this flag is a hardware question: see Coax below), ADAT 1-8
 *                       at 44.1-96 kHz (88.2/96 over both ports, S/MUX, docs/console-parity.md). Nothing ADAT at
 *                       176.4/192 kHz on either unit (assumed: UA documents S/MUX II only).
 *   front panel         readback word 2 [7:0] monitor level (raw = 192 + 2 dB), [15:8] HP 1 level, bit 16 mute,
 *                       bit 17 mono, bit 31 dim; word 6 bit 8 talkback, [29:28] ALT; word 0 bit 27 = the knob is on
 *                       an output (bit 26: the headphones, else the monitor), bit 27 clear = on the preamp in word 3
 *                       [30:28] (oa:mixer-engine/hardware.py:5225-5345, ua_mixer_daemon.py:1944-2140; x4-verified
 *                       upstream). [HW] Twin X word 0 0x0800030C: bit 27 set, bit 26 clear = MONITOR. No notification
 *                       bit announces a knob turn: the unit changes its own monitor level and the readback follows,
 *                       which the 10 ms service pass reads (mixer.c). "Talkback also sets the dim bit"
 *                       (oa:mixer-engine/ua_mixer_daemon.py:3055, x4).
 *
 * Nothing here has run against an external clock or an optical cable on these units: every source but Internal
 * and every digital-format write is UNPROVEN (APOLLO_CLOCK_MODEL.Proven*), refused unless the caller says TRY.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once
#include "apollo_model.h"
#include "rate_plan.h"

/* ------------------------------------------------------------------ */
/* Clock sources                                                       */
/* ------------------------------------------------------------------ */

#define APOLLO_CK_SPDIF             0x00ul
#define APOLLO_CK_ADAT              0x05ul
#define APOLLO_CK_WORDCLOCK         0x07ul
#define APOLLO_CK_PEER              0x0Aul      /* Thunderbolt peer: never offered here (no P2P path, spec-twinx 6.3.7) */
#define APOLLO_CK_INTERNAL          0x0Cul
#define APOLLO_CK_BIT(s)            (1ul << ((s) & 0x1Ful))

#define APOLLO_CK_ACK_BITS          0x00400030ul    /* 0xC030 bit 5 (the kext's), 22, 4: rate_plan.h APOLLO_RATE_ACK_BITS */
#define APOLLO_CK_EVENT_BIT         0x00000020ul    /* bit 5: the clock-change event */
#define APOLLO_CK_ACK_MS            2000ul      /* per write, as the rate change */
#define APOLLO_CK_SETTLE_MS         1000ul      /* the lock must hold this long after the ack (G6: acked, then dropped) */
#define APOLLO_CK_LOSS_MS           1000ul      /* an external source unlocked this long: back to Internal (unless HOLD) */
#define APOLLO_CK_POLL_MS           10ul

/* The clock word (0xC074). */
static __inline unsigned long ApolloClockWord(unsigned long RateIndex, unsigned long Source)
{
    return ((RateIndex & 0xFFul) << 8) | (Source & 0xFFul);
}

/* Does the unit's 0xC08C word offer Source ([31:16], bit n = source n)? A dead or unread word offers Internal only. */
static __inline int ApolloClockOffered(unsigned long ClockSrcs, unsigned long Source)
{
    if (Source == APOLLO_CK_INTERNAL) {
        return 1;
    }
    if (ClockSrcs == 0 || ClockSrcs == 0xFFFFFFFFul || Source > 15) {
        return 0;
    }
    return ((ClockSrcs >> (16 + Source)) & 1ul) != 0;
}

static __inline const char *ApolloClockName(unsigned long Source)
{
    switch (Source) {
    case APOLLO_CK_INTERNAL:  return "Internal";
    case APOLLO_CK_SPDIF:     return "S/PDIF";
    case APOLLO_CK_ADAT:      return "ADAT";
    case APOLLO_CK_WORDCLOCK: return "Word Clock";
    case APOLLO_CK_PEER:      return "Peer (Thunderbolt)";
    default:                  return "?";
    }
}

/* The rates a source carries (bit n = rate index n): S/PDIF and ADAT 44.1-96 kHz, the rest all six. */
static __inline unsigned long ApolloClockRatesFor(unsigned long Source)
{
    return (Source == APOLLO_CK_SPDIF || Source == APOLLO_CK_ADAT) ? APOLLO_RATE_MASK_SPDIF : APOLLO_RATE_MASK_ALL;
}

/* ------------------------------------------------------------------ */
/* Digital I/O                                                         */
/* ------------------------------------------------------------------ */

/* The digital state of a unit (driver Ctx->DigState, the combined device's APOLLO_AGG_UNIT_RT.Dig). */
#define APOLLO_DIG_KNOWN            0x01ul      /* the formats were set by this driver since the connect */
#define APOLLO_DIG_IN_ADAT          0x02ul      /* optical input: ADAT (else S/PDIF) */
#define APOLLO_DIG_OUT_ADAT         0x04ul      /* optical output: ADAT (else S/PDIF) */
#define APOLLO_DIG_SRC              0x08ul      /* SR Convert on the S/PDIF input */

/* What a model has (APOLLO_CLOCK_MODEL.Digital, ProvenDigital) */
#define APOLLO_DIG_HAS_OPT_IN       0x01ul      /* optical input with a format choice */
#define APOLLO_DIG_HAS_OPT_OUT      0x02ul      /* optical output with a format choice */
#define APOLLO_DIG_HAS_COAX         0x04ul      /* coaxial S/PDIF in and out, whatever the optical format */
#define APOLLO_DIG_HAS_SRC          0x08ul      /* SR Convert on the S/PDIF input */

/* Mailbox fields (masked writes; mixer.c ApolloMixerWriteSetting) */
#define APOLLO_DIG_SETTING_MODE     8ul         /* S8: the device-mode word [7:0] */
#define APOLLO_DIG_MODE_OUT_ADAT    0x08ul
#define APOLLO_DIG_MODE_IN_ADAT     0x10ul
#define APOLLO_DIG_SETTING_SRC      1ul         /* S1 bit 31 */
#define APOLLO_DIG_SRC_BIT          0x80000000ul

/* Descriptor channel types (IO descriptor id = type << 8 | number) */
#define APOLLO_CHTYPE_ADAT          0x04ul
#define APOLLO_CHTYPE_SPDIF         0x05ul
#define APOLLO_CHTYPE_TALKBACK      0x0Bul
#define APOLLO_CHAN_TALKBACK_1      0x0B01ul
#define APOLLO_CHAN_TALKBACK_2      0x0B02ul

typedef struct _APOLLO_CLOCK_MODEL {
    int             Model;              /* APOLLO_MODEL_* */
    const char     *Name;
    unsigned long   Sources;            /* APOLLO_CK_BIT of every source the model has a jack for */
    unsigned long   Digital;            /* APOLLO_DIG_HAS_* */
    unsigned long   AdatCh[3];          /* ADAT channels at 44.1/48, 88.2/96 (S/MUX), 176.4/192 kHz */
    unsigned long   ProvenSources;      /* APOLLO_CK_BIT of the sources that ran on Aaron's units */
    unsigned long   ProvenDigital;      /* APOLLO_DIG_HAS_* bits whose writes ran on Aaron's units */
} APOLLO_CLOCK_MODEL;

/* The clock and digital plan of a model, or 0. */
static __inline const APOLLO_CLOCK_MODEL *ApolloClockModelFor(int Model)
{
    static const APOLLO_CLOCK_MODEL t[2] = {
        { APOLLO_MODEL_X8P, "Apollo x8p",
          APOLLO_CK_BIT(APOLLO_CK_INTERNAL) | APOLLO_CK_BIT(APOLLO_CK_WORDCLOCK) | APOLLO_CK_BIT(APOLLO_CK_ADAT) |
          APOLLO_CK_BIT(APOLLO_CK_SPDIF),
          APOLLO_DIG_HAS_OPT_IN | APOLLO_DIG_HAS_OPT_OUT | APOLLO_DIG_HAS_COAX | APOLLO_DIG_HAS_SRC,
          { 8ul, 8ul, 0ul },
          APOLLO_CK_BIT(APOLLO_CK_INTERNAL), 0ul },
        { APOLLO_MODEL_TWINX, "Apollo Twin X (Gen 1)",
          APOLLO_CK_BIT(APOLLO_CK_INTERNAL) | APOLLO_CK_BIT(APOLLO_CK_ADAT) | APOLLO_CK_BIT(APOLLO_CK_SPDIF),
          APOLLO_DIG_HAS_OPT_IN | APOLLO_DIG_HAS_SRC,
          { 8ul, 4ul, 0ul },
          APOLLO_CK_BIT(APOLLO_CK_INTERNAL), 0ul },
    };

    if (Model == APOLLO_MODEL_X8P) {
        return &t[0];
    }
    if (Model == APOLLO_MODEL_TWINX) {
        return &t[1];
    }
    return 0;
}

/* Why a source cannot be used now (ApolloClockCheck); 0 = it can. Same numbers as apollo_clock.h APOLLO_CLOCK_ERR_*. */
#define APOLLO_CKCHK_OK             0
#define APOLLO_CKCHK_INVALID        1   /* not a source id this driver knows (Peer is never selectable) */
#define APOLLO_CKCHK_NOT_HERE       2   /* the model has no jack for it */
#define APOLLO_CKCHK_NOT_OFFERED    3   /* the unit's own 0xC08C list does not offer it */
#define APOLLO_CKCHK_UNPROVEN       4   /* never run on these units: the caller must say TRY */
#define APOLLO_CKCHK_RATE           5   /* it does not carry the unit's rate */
#define APOLLO_CKCHK_PORT           6   /* the optical port is in the other format */
#define APOLLO_CKCHK_SRC            7   /* SR Convert is on: the S/PDIF input is not a clock */

/* Can model M run on Source at rate index Idx with digital state Dig, given the unit's 0xC08C word? */
static __inline int ApolloClockCheck(const APOLLO_CLOCK_MODEL *M, unsigned long Source, unsigned long ClockSrcs,
                                     unsigned long Idx, unsigned long Dig, int Try)
{
    if (Source != APOLLO_CK_INTERNAL && Source != APOLLO_CK_SPDIF && Source != APOLLO_CK_ADAT &&
        Source != APOLLO_CK_WORDCLOCK) {
        return APOLLO_CKCHK_INVALID;
    }
    if (M == 0 || (M->Sources & APOLLO_CK_BIT(Source)) == 0) {
        return APOLLO_CKCHK_NOT_HERE;
    }
    if (!ApolloClockOffered(ClockSrcs, Source)) {
        return APOLLO_CKCHK_NOT_OFFERED;
    }
    if ((ApolloClockRatesFor(Source) & (1ul << (Idx & 31ul))) == 0) {
        return APOLLO_CKCHK_RATE;
    }
    /* A source on the optical port needs the port in that format: ADAT only once set to ADAT here (the connect
       batch leaves S/PDIF); S/PDIF over optical unless the port was set to ADAT (or the model has coax). */
    if (Source == APOLLO_CK_ADAT && ((Dig & APOLLO_DIG_KNOWN) == 0 || (Dig & APOLLO_DIG_IN_ADAT) == 0)) {
        return APOLLO_CKCHK_PORT;
    }
    if (Source == APOLLO_CK_SPDIF && (M->Digital & APOLLO_DIG_HAS_COAX) == 0 && (Dig & APOLLO_DIG_KNOWN) != 0 &&
        (Dig & APOLLO_DIG_IN_ADAT) != 0) {
        return APOLLO_CKCHK_PORT;
    }
    if (Source == APOLLO_CK_SPDIF && (Dig & APOLLO_DIG_SRC) != 0) {
        return APOLLO_CKCHK_SRC;
    }
    if ((M->ProvenSources & APOLLO_CK_BIT(Source)) == 0 && !Try) {
        return APOLLO_CKCHK_UNPROVEN;
    }
    return APOLLO_CKCHK_OK;
}

/* Every source model M could switch to now with TRY (bit n = source n): the menu Apollo Control shows. */
static __inline unsigned long ApolloClockUsable(const APOLLO_CLOCK_MODEL *M, unsigned long ClockSrcs, unsigned long Idx,
                                                unsigned long Dig)
{
    static const unsigned long s[4] = { APOLLO_CK_INTERNAL, APOLLO_CK_SPDIF, APOLLO_CK_ADAT, APOLLO_CK_WORDCLOCK };
    unsigned long m = 0, i;

    for (i = 0; i < 4; i++) {
        if (ApolloClockCheck(M, s[i], ClockSrcs, Idx, Dig, 1) == APOLLO_CKCHK_OK) {
            m |= APOLLO_CK_BIT(s[i]);
        }
    }
    return m;
}

/* The rates a unit may run at on Source (bit n = rate index n): what it offers, and what the source carries. */
static __inline unsigned long ApolloClockRatesAllowed(unsigned long ClockSrcs, unsigned long Source)
{
    return ApolloRateOffered(ClockSrcs) & ApolloClockRatesFor(Source);
}

/* The digital controls (apollo_clock.h APOLLO_CLOCK_WHAT_*): which APOLLO_DIG_HAS_* bit each needs. */
#define APOLLO_DIGW_OPT_IN          1ul
#define APOLLO_DIGW_OPT_OUT         2ul
#define APOLLO_DIGW_SRC             3ul

static __inline unsigned long ApolloDigNeeds(unsigned long What)
{
    return What == APOLLO_DIGW_OPT_IN ? APOLLO_DIG_HAS_OPT_IN : What == APOLLO_DIGW_OPT_OUT ? APOLLO_DIG_HAS_OPT_OUT :
           What == APOLLO_DIGW_SRC ? APOLLO_DIG_HAS_SRC : 0ul;
}

/* The mailbox write of one digital control: setting, value, mask. 0 = not a control. */
static __inline int ApolloDigEncode(unsigned long What, unsigned long On, unsigned long *Setting, unsigned long *Value,
                                    unsigned long *Mask)
{
    switch (What) {
    case APOLLO_DIGW_OPT_IN:
        *Setting = APOLLO_DIG_SETTING_MODE;
        *Mask = APOLLO_DIG_MODE_IN_ADAT;
        break;
    case APOLLO_DIGW_OPT_OUT:
        *Setting = APOLLO_DIG_SETTING_MODE;
        *Mask = APOLLO_DIG_MODE_OUT_ADAT;
        break;
    case APOLLO_DIGW_SRC:
        *Setting = APOLLO_DIG_SETTING_SRC;
        *Mask = APOLLO_DIG_SRC_BIT;
        break;
    default:
        *Setting = *Value = *Mask = 0;
        return 0;
    }
    *Value = On ? *Mask : 0ul;
    return 1;
}

/* The state bit a control sets. */
static __inline unsigned long ApolloDigStateBit(unsigned long What)
{
    return What == APOLLO_DIGW_OPT_IN ? APOLLO_DIG_IN_ADAT : What == APOLLO_DIGW_OPT_OUT ? APOLLO_DIG_OUT_ADAT :
           What == APOLLO_DIGW_SRC ? APOLLO_DIG_SRC : 0ul;
}

/*
 * Why a digital control cannot change now (0 = it can): APOLLO_CKCHK_NOT_HERE (the model lacks it),
 * APOLLO_CKCHK_PORT (the clock runs from the port whose format would change), APOLLO_CKCHK_SRC (SR Convert while
 * the clock is S/PDIF), APOLLO_CKCHK_UNPROVEN (needs TRY).
 */
static __inline int ApolloDigCheck(const APOLLO_CLOCK_MODEL *M, unsigned long What, unsigned long On,
                                   unsigned long ClockSource, int Try)
{
    unsigned long need = ApolloDigNeeds(What);

    if (M == 0 || need == 0 || (M->Digital & need) == 0) {
        return APOLLO_CKCHK_NOT_HERE;
    }
    if (What == APOLLO_DIGW_OPT_IN && (ClockSource == APOLLO_CK_ADAT ||
                                       (ClockSource == APOLLO_CK_SPDIF && (M->Digital & APOLLO_DIG_HAS_COAX) == 0))) {
        return APOLLO_CKCHK_PORT;
    }
    if (What == APOLLO_DIGW_SRC && On && ClockSource == APOLLO_CK_SPDIF) {
        return APOLLO_CKCHK_SRC;
    }
    if ((M->ProvenDigital & need) == 0 && !Try) {
        return APOLLO_CKCHK_UNPROVEN;
    }
    return APOLLO_CKCHK_OK;
}

/* ADAT channels that carry audio in one direction at rate index Idx with state Dig (0 = the port is S/PDIF). */
static __inline unsigned long ApolloDigAdatChannels(const APOLLO_CLOCK_MODEL *M, int Record, unsigned long Idx,
                                                    unsigned long Dig)
{
    unsigned long port = Record ? APOLLO_DIG_HAS_OPT_IN : APOLLO_DIG_HAS_OPT_OUT;
    unsigned long adat = Record ? APOLLO_DIG_IN_ADAT : APOLLO_DIG_OUT_ADAT;

    if (M == 0 || (M->Digital & port) == 0 || ((Dig & APOLLO_DIG_KNOWN) != 0 && (Dig & adat) == 0)) {
        return 0;
    }
    return M->AdatCh[ApolloRateBand(Idx) > 2 ? 2 : ApolloRateBand(Idx)];
}

/* S/PDIF channels (0 or 2) that carry audio in one direction at rate index Idx with state Dig. */
static __inline unsigned long ApolloDigSpdifChannels(const APOLLO_CLOCK_MODEL *M, int Record, unsigned long Idx,
                                                     unsigned long Dig)
{
    unsigned long port = Record ? APOLLO_DIG_HAS_OPT_IN : APOLLO_DIG_HAS_OPT_OUT;
    unsigned long adat = Record ? APOLLO_DIG_IN_ADAT : APOLLO_DIG_OUT_ADAT;

    if (M == 0 || ApolloRateBand(Idx) == 2) {
        return 0;                       /* S/PDIF carries 44.1-96 kHz */
    }
    if ((M->Digital & APOLLO_DIG_HAS_COAX) != 0) {
        return 2;
    }
    if ((M->Digital & port) == 0 || ((Dig & APOLLO_DIG_KNOWN) != 0 && (Dig & adat) != 0)) {
        return 0;
    }
    return 2;
}

/*
 * Does the slot with descriptor id Id carry audio on Model at Hz with state Dig? 1 = yes, or nothing chosen here
 * (APOLLO_DIG_KNOWN clear: the layout stays the descriptor's, as before this code); 0 = silent in this format or at
 * this rate. Only ADAT and S/PDIF ids are ever 0. The combined layout (apollo_agg.h ApolloAggSlotShownAt) and the
 * endpoint rows (APOLLO_EP_FLAG_SILENT) follow it.
 */
static __inline int ApolloDigSlotLive(int Model, int Record, unsigned long Id, unsigned long Hz, unsigned long Dig)
{
    const APOLLO_CLOCK_MODEL *M = ApolloClockModelFor(Model);
    unsigned long type = (Id >> 8) & 0xFFul, num = Id & 0xFFul;
    unsigned long idx = ApolloRateIndex(Hz != 0 ? Hz : APOLLO_RATE_DEFAULT_HZ);

    if (M == 0 || (Dig & APOLLO_DIG_KNOWN) == 0 || idx == 0) {
        return 1;
    }
    if (type == APOLLO_CHTYPE_ADAT) {
        return num >= 1 && num <= ApolloDigAdatChannels(M, Record, idx, Dig);
    }
    if (type == APOLLO_CHTYPE_SPDIF) {
        return num >= 1 && num <= ApolloDigSpdifChannels(M, Record, idx, Dig);
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Front panel (readback)                                              */
/* ------------------------------------------------------------------ */

/* Fields (event ids; bit (1 << f) in a change mask) */
#define APOLLO_PANEL_LEVEL          1ul     /* monitor level, raw 0..192 = 192 + 2 dB */
#define APOLLO_PANEL_HP             2ul     /* HP 1 level, raw */
#define APOLLO_PANEL_MUTE           3ul
#define APOLLO_PANEL_MONO           4ul
#define APOLLO_PANEL_DIM            5ul
#define APOLLO_PANEL_ALT            6ul     /* 0 MAIN, 1 ALT 1, 2 ALT 2 */
#define APOLLO_PANEL_TALK           7ul     /* the talkback button */
#define APOLLO_PANEL_FOCUS          8ul     /* what the big knob drives: APOLLO_PANEL_FOCUS_* */
#define APOLLO_PANEL_PREAMP         9ul     /* the preamp the knob edits (0-based) while FOCUS is PREAMP */
#define APOLLO_PANEL_LOCK           10ul    /* readback clock lock: -1 not reported, 0, 1 */
#define APOLLO_PANEL_FIELDS         11ul

#define APOLLO_PANEL_FOCUS_PREAMP   0ul     /* PREAMP mode: the knob is a preamp gain */
#define APOLLO_PANEL_FOCUS_MONITOR  1ul     /* MONITOR mode: the knob is the monitor level */
#define APOLLO_PANEL_FOCUS_HP       2ul     /* the knob is the headphone level */

/* Who changed a field (events) */
#define APOLLO_PANEL_ORIGIN_PANEL   0ul     /* the unit itself: its knob or a button */
#define APOLLO_PANEL_ORIGIN_HOST    1ul     /* a write from Windows (Apollo Control, apollo-diag) seen coming back */
#define APOLLO_PANEL_ORIGIN_LINK    2ul     /* the monitor link mirrored another unit's knob, or dimmed for talkback */
#define APOLLO_PANEL_ORIGIN_UNIT    3ul     /* the unit's own reaction (its dim on talkback) */

#define APOLLO_PANEL_EXPECT_MS      1500ul  /* a host write's value is recognised this long (open-apollo: 2 s) */
#define APOLLO_PANEL_FW_DIM_MS      300ul   /* a dim change this soon after talkback went on or off is the unit's own */

typedef struct _APOLLO_PANEL_STATE {
    unsigned long   Level, HpLevel, Mute, Mono, Dim, Alt, Talk, Focus, Preamp;
    long            Lock;
} APOLLO_PANEL_STATE;

/* The front panel from the readback words (at least 8). 0 when there are none. */
static __inline int ApolloPanelDecode(const unsigned long *W, unsigned long N, APOLLO_PANEL_STATE *S)
{
    S->Level = S->HpLevel = S->Mute = S->Mono = S->Dim = S->Alt = S->Talk = S->Focus = S->Preamp = 0;
    S->Lock = -1;
    if (W == 0 || N < 8) {
        return 0;
    }
    S->Level = W[2] & 0xFFul;
    S->HpLevel = (W[2] >> 8) & 0xFFul;
    S->Mute = (W[2] >> 16) & 1ul;
    S->Mono = (W[2] >> 17) & 1ul;
    S->Dim = (W[2] >> 31) & 1ul;
    S->Alt = (W[6] >> 28) & 3ul;
    S->Talk = (W[6] >> 8) & 1ul;
    S->Lock = ((W[6] >> 30) & 1ul) ? (long)((W[6] >> 31) & 1ul) : -1L;
    if ((W[0] >> 27) & 1ul) {
        S->Focus = ((W[0] >> 26) & 1ul) ? APOLLO_PANEL_FOCUS_HP : APOLLO_PANEL_FOCUS_MONITOR;
        S->Preamp = 0;
    } else {
        S->Focus = APOLLO_PANEL_FOCUS_PREAMP;
        S->Preamp = (W[3] >> 28) & 7ul;
    }
    return 1;
}

static __inline long ApolloPanelValue(const APOLLO_PANEL_STATE *S, unsigned long Field)
{
    switch (Field) {
    case APOLLO_PANEL_LEVEL:  return (long)S->Level;
    case APOLLO_PANEL_HP:     return (long)S->HpLevel;
    case APOLLO_PANEL_MUTE:   return (long)S->Mute;
    case APOLLO_PANEL_MONO:   return (long)S->Mono;
    case APOLLO_PANEL_DIM:    return (long)S->Dim;
    case APOLLO_PANEL_ALT:    return (long)S->Alt;
    case APOLLO_PANEL_TALK:   return (long)S->Talk;
    case APOLLO_PANEL_FOCUS:  return (long)S->Focus;
    case APOLLO_PANEL_PREAMP: return (long)S->Preamp;
    case APOLLO_PANEL_LOCK:   return S->Lock;
    default:                  return 0;
    }
}

/* The fields that differ between A and B (bit 1 << field). The preamp counts only while the knob is on a preamp. */
static __inline unsigned long ApolloPanelDiff(const APOLLO_PANEL_STATE *A, const APOLLO_PANEL_STATE *B)
{
    unsigned long m = 0, f;

    for (f = 1; f < APOLLO_PANEL_FIELDS; f++) {
        if (f == APOLLO_PANEL_PREAMP && B->Focus != APOLLO_PANEL_FOCUS_PREAMP) {
            continue;
        }
        if (ApolloPanelValue(A, f) != ApolloPanelValue(B, f)) {
            m |= 1ul << f;
        }
    }
    return m;
}

static __inline const char *ApolloPanelFieldName(unsigned long Field)
{
    static const char *const n[APOLLO_PANEL_FIELDS] = {
        "?", "level", "hp", "mute", "mono", "dim", "alt", "talkback", "knob", "preamp", "lock",
    };

    return Field < APOLLO_PANEL_FIELDS ? n[Field] : "?";
}

/* The panel field a SET_MONITOR_PARAM code writes (apollo_ioctl.h APOLLO_MON_*), 0 = none. */
static __inline unsigned long ApolloPanelFieldOfMon(unsigned long Param)
{
    switch (Param) {
    case 0x01ul: return APOLLO_PANEL_LEVEL;     /* APOLLO_MON_LEVEL */
    case 0x02ul: return APOLLO_PANEL_HP;        /* APOLLO_MON_HP1_LEVEL */
    case 0x03ul: return APOLLO_PANEL_MUTE;
    case 0x10ul: return APOLLO_PANEL_MONO;
    case 0x44ul: return APOLLO_PANEL_DIM;
    case 0x3Aul: return APOLLO_PANEL_ALT;       /* APOLLO_MON_ALT_SELECT */
    case 0x46ul: return APOLLO_PANEL_TALK;      /* APOLLO_MON_TALKBACK */
    default:     return 0;
    }
}

/* A value a write from the host (or the link) is expected to bring back, and from whom. */
typedef struct _APOLLO_PANEL_EXPECT {
    long            Value;
    unsigned long   Until;              /* ms (the caller's clock), wraps */
    unsigned char   Origin;             /* APOLLO_PANEL_ORIGIN_* */
    unsigned char   Active;
} APOLLO_PANEL_EXPECT;

/* Who changed Field to Value at NowMs: the expected writer if it matches in time (and the expectation is used up). */
static __inline unsigned long ApolloPanelClassify(APOLLO_PANEL_EXPECT *E, unsigned long Field, long Value,
                                                  unsigned long NowMs)
{
    if (Field >= APOLLO_PANEL_FIELDS || !E[Field].Active) {
        return APOLLO_PANEL_ORIGIN_PANEL;
    }
    if ((long)(E[Field].Until - NowMs) < 0) {
        E[Field].Active = 0;            /* stale: the unit never showed it */
        return APOLLO_PANEL_ORIGIN_PANEL;
    }
    if (E[Field].Value != Value) {
        return APOLLO_PANEL_ORIGIN_PANEL;
    }
    E[Field].Active = 0;
    return (unsigned long)E[Field].Origin;
}

static __inline void ApolloPanelExpect(APOLLO_PANEL_EXPECT *E, unsigned long Field, long Value, unsigned long Origin,
                                       unsigned long NowMs)
{
    if (Field == 0 || Field >= APOLLO_PANEL_FIELDS) {
        return;
    }
    E[Field].Value = Value;
    E[Field].Until = NowMs + APOLLO_PANEL_EXPECT_MS;
    E[Field].Origin = (unsigned char)Origin;
    E[Field].Active = 1;
}

/* ------------------------------------------------------------------ */
/* The monitor link (panel.c): one unit's knob drives another's monitor */
/* ------------------------------------------------------------------ */

#define APOLLO_LINK_LEVEL           0x01ul  /* the knob's turns move the monitor unit's level by the same steps */
#define APOLLO_LINK_MUTE            0x02ul  /* its mute (the knob press) is the monitor unit's mute */
#define APOLLO_LINK_DIM             0x04ul
#define APOLLO_LINK_MONO            0x08ul
#define APOLLO_LINK_TALK_DIM        0x10ul  /* talkback on any unit dims the monitor unit (and undims what it dimmed) */
#define APOLLO_LINK_ALL             0x1Ful

/*
 * What one change on a knob unit (not the monitor unit) asks of the monitor unit: the link flag that carries it, 0 =
 * nothing. Only the unit's own changes travel (a host write or a mirrored value never comes back: no loops); the
 * level only while the knob is in MONITOR mode (in PREAMP mode it is a gain, in HP mode the headphones).
 */
static __inline unsigned long ApolloLinkCarries(unsigned long Flags, unsigned long Field, unsigned long Origin,
                                                unsigned long Focus)
{
    unsigned long need;

    if (Origin != APOLLO_PANEL_ORIGIN_PANEL && !(Field == APOLLO_PANEL_TALK && Origin == APOLLO_PANEL_ORIGIN_HOST)) {
        return 0;
    }
    switch (Field) {
    case APOLLO_PANEL_LEVEL:
        need = (Focus == APOLLO_PANEL_FOCUS_MONITOR) ? APOLLO_LINK_LEVEL : 0ul;
        break;
    case APOLLO_PANEL_MUTE: need = APOLLO_LINK_MUTE; break;
    case APOLLO_PANEL_DIM:  need = APOLLO_LINK_DIM; break;
    case APOLLO_PANEL_MONO: need = APOLLO_LINK_MONO; break;
    case APOLLO_PANEL_TALK: need = APOLLO_LINK_TALK_DIM; break;
    default:                need = 0; break;
    }
    return (Flags & need);
}

/*
 * The knob mode that counts for the link: the Twin X has one knob for preamps, monitor and headphones (its mode is
 * word 0 bit 27/26), the x8p a monitor knob of its own beside its preamp knob, so its monitor level changes are
 * always the monitor knob's whatever word 0 says (bit 27 clear on the live read: the preamp knob's focus).
 */
static __inline unsigned long ApolloPanelLinkFocus(int Model, unsigned long Focus)
{
    return Model == APOLLO_MODEL_TWINX ? Focus : APOLLO_PANEL_FOCUS_MONITOR;
}

/* The monitor unit's new level for a knob turn of Delta raw steps (0.5 dB each) from Current: 0..192. */
static __inline unsigned long ApolloLinkLevel(unsigned long Current, long Delta)
{
    long v = (long)(Current > 192ul ? 192ul : Current) + Delta;

    return (unsigned long)(v < 0 ? 0 : (v > 192 ? 192 : v));
}

/*
 * Talkback dim on the monitor unit: Talk is the talkback state that just changed, Dim the monitor unit's dim now,
 * Dimmed whether the link dimmed it. Returns the dim to write (0/1) or -1 for nothing, and updates *Dimmed: on with
 * talkback only where it was off (a dim the user had stays the user's), off again only what the link turned on.
 */
static __inline int ApolloLinkTalkDim(unsigned long Talk, unsigned long Dim, int *Dimmed)
{
    if (Talk) {
        if (!Dim && !*Dimmed) {
            *Dimmed = 1;
            return 1;
        }
        return -1;
    }
    if (*Dimmed) {
        *Dimmed = 0;
        return 0;
    }
    return -1;
}
