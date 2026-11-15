/*
 * preamp_plan.h - the preamp controls of each model as data (docs/spec-twinx.md 4.1 and 4.4,
 * gate G5; docs/spec-dsp-mixer.md 5.2-5.3). Which SET_INPUT_PARAM codes a model takes, the
 * mixer-mailbox writes each one becomes, and how the hardware readback decodes for it.
 *
 * Plain C like bringup_plan.h: no kernel or Windows headers, every function static __inline.
 * mixer.c builds the writes from it and ioctl.c the preamp count; apollo-diag decodes readback
 * with it; tools\preamp-plan-test checks the Twin X against the spec and proves that the x8p's
 * writes and errors are what mixer.c produced before G5, for every input.
 *
 * Mapping: port of oa:driver/ua_core.c ua_preamp_setting (2536-2660), readback
 * oa:driver/ua_apollo.h 842-890 (GPL-2.0). "ch" is 0-based.
 *
 *   control                   setting, bits                                    x8p     Twin X Gen 1
 *   Mic/Line, PAD, 48V,       S0 (ch 0-3) / S12 (ch 4-7), bit 6*(ch&3) + code  0-7     0-1 (S0 bits 0,1,3,4,5
 *   LowCut, Phase (0/1)                                                                 and 6,7,9,10,11)
 *   Link (0/1)                same setting, mask 0x104 << 12*((ch&3)>>1),      0,2,4,6 0 only
 *                             from the first channel of a pair
 *   Gain 10-65 dB             GainA S17/S25 and GainB S16/S24 byte lane ch&3   0-7     0-1
 *                             (dB-10, at most 54), GainC S3/S14 6 bits at
 *                             6*(ch&3) (+4 for ch&2) (dB-9, at most 56)
 *   Hi-Z impedance 0x11       S13 nibble 4*ch, codes 0-7 = 2.2M 1M 453k 232k   -       0 only (input 1)
 *                             100k 70k 47k 21k (oa:driver/ua_audio.c:7281-7288)
 *
 * [polish Nov 15] Linked pairs stay the host's job. While a pair is linked the unit runs both preamps from
 * the first channel's fields and ignores the second channel's (Twin X Gen 1 on hardware, Nov 15: a low cut
 * write to input 2 was taken and did nothing; one to input 1 changed both, and readback then shows input 1's
 * settings on both). This driver stays a per-channel field writer, like UA's kext (ua_preamp_setting); the
 * host gangs the pair: Apollo Control (app\ui\app.js setInput / linkedPair) sends a change made on either
 * input to the first input, then the same value to the second, which is what UA's own host does
 * (oa:mixer-engine/hardware.py _gang and _link_hardware, from CMxInput::UpdateHardwareState). Not mirrored
 * here because:
 *   - the mailbox keeps no shadow (a masked write, zeroed on the next flush), so the link state would come
 *     from readback, which trails a write by up to one 10 ms service pass and is invalid after a connect:
 *     a write to channel 2 right after a link write would be mirrored or not by chance;
 *   - a client may write the second channel on purpose: its value for after an unlink (UA re-pushes the
 *     first channel's settings there at link time, and Apollo Control does the same), apollo-diag and the
 *     step scripts' round trips;
 *   - the host already owns what the user sees: one control for the pair, both inputs pending until the
 *     readback agrees.
 *
 * Not offered on any model: mic impedance 0x07 (no code-to-ohm table in any source), the custom
 * (Unison) preamp bit 0x08 (kept 0: gain is refused while it is set, spec-twinx 4.4), the byte
 * lanes 0x0B-0x0F, knob steps, the Hi-Z host write 0x14 and the rest of the kext's ids. The x8p
 * keeps exactly what it had before G5 (no Hi-Z impedance there).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once
#include "apollo_model.h"

/* SET_INPUT_PARAM codes (copies of apollo_ioctl.h APOLLO_PARAM_*, which needs Windows types; mixer.c asserts) */
#define APOLLO_PRE_MIC_LINE         0x00ul      /* 1 = Line */
#define APOLLO_PRE_PAD              0x01ul
#define APOLLO_PRE_LINK             0x02ul
#define APOLLO_PRE_48V              0x03ul
#define APOLLO_PRE_LOWCUT           0x04ul
#define APOLLO_PRE_PHASE            0x05ul      /* 1 = inverted */
#define APOLLO_PRE_HIZ_IMP          0x11ul      /* [G5] kPropHiZImpedance, code 0-7 */
#define APOLLO_PRE_GAIN_DB          0x100ul     /* driver-side: GainA, GainB, GainC */
#define APOLLO_PRE_HIZ_CODES        8ul
#define APOLLO_PRE_GAIN_MIN_DB      10L
#define APOLLO_PRE_GAIN_MAX_DB      65L

/* ApolloPreampEncode results */
#define APOLLO_PRE_OK               0   /* W[0..*N) to the mailbox, in this order */
#define APOLLO_PRE_BAD_VALUE        1   /* STATUS_INVALID_PARAMETER: a value out of range, a preamp the
                                           model does not have, Link from a channel that does not start a pair */
#define APOLLO_PRE_NOT_HERE         2   /* STATUS_NOT_SUPPORTED: a control the model (or that preamp) lacks */

#define APOLLO_PRE_WRITES_MAX       3

typedef struct _APOLLO_PREAMP_MODEL {
    int             Model;              /* APOLLO_MODEL_* */
    const char     *Name;
    unsigned long   Preamps;            /* channels 0..Preamps-1 */
    unsigned long   LinkFirst;          /* bit c: Link on channel c links c and c+1 */
    unsigned long   HizImpedance;       /* bit c: the host sets channel c's Hi-Z impedance (0x11) */
    unsigned long   HizDetect;          /* bit c: readback word 0 bit 24+c = a cable in channel c's Hi-Z jack */
} APOLLO_PREAMP_MODEL;

typedef struct _APOLLO_PREAMP_WRITE {
    unsigned long   Setting;            /* mixer setting 0..37 */
    unsigned long   Value;
    unsigned long   Mask;
} APOLLO_PREAMP_WRITE;

/* The preamp plan of a model, or 0 (no preamp control). */
static __inline const APOLLO_PREAMP_MODEL *ApolloPreampModelFor(int Model)
{
    static const APOLLO_PREAMP_MODEL t[2] = {
        /* x8p: as mixer.c had it before G5 (8 preamps, pairs 1-2 .. 7-8); its two front Hi-Z jacks
           are reported in readback, their impedance is not offered */
        { APOLLO_MODEL_X8P, "Apollo x8p", 8, 0x55ul, 0x0ul, 0x3ul },
        /* Twin X Gen 1 (spec-twinx 4.1): 2 preamps, link on input 1 only (oa:mixer-engine/model_info.py
           196-200), Hi-Z on input 1 only (oa:driver/ua_models_gen.h Twin X .hiz = 1) */
        { APOLLO_MODEL_TWINX, "Apollo Twin X (Gen 1)", 2, 0x1ul, 0x1ul, 0x1ul },
    };

    if (Model == APOLLO_MODEL_X8P) {
        return &t[0];
    }
    if (Model == APOLLO_MODEL_TWINX) {
        return &t[1];
    }
    return 0;
}

/* The preamp count a client should show (GET_INFO NumPreamps): 0 without a plan. */
static __inline unsigned long ApolloPreampCount(int Model)
{
    const APOLLO_PREAMP_MODEL *m = ApolloPreampModelFor(Model);

    return (m != 0) ? m->Preamps : 0;
}

static __inline void ApolloPreampPut(APOLLO_PREAMP_WRITE *W, unsigned long *N, unsigned long Setting,
                                     unsigned long Value, unsigned long Mask)
{
    if (*N < APOLLO_PRE_WRITES_MAX) {
        W[*N].Setting = Setting;
        W[*N].Value = Value & Mask;
        W[*N].Mask = Mask;
        (*N)++;
    }
}

/*
 * One SET_INPUT_PARAM as mailbox writes for model M. APOLLO_PRE_OK with *N writes (the gain's
 * three go out under one lock: one mailbox commit), else a refusal and *N = 0. The order of the
 * checks is mixer.c's before G5: the channel first, then the code, then the value.
 */
static __inline int ApolloPreampEncode(const APOLLO_PREAMP_MODEL *M, unsigned long Ch, unsigned long Param,
                                       long Value, APOLLO_PREAMP_WRITE W[APOLLO_PRE_WRITES_MAX], unsigned long *N)
{
    unsigned long q = Ch & 3ul;
    int hiBank = (Ch >= 4);

    *N = 0;
    if (M == 0) {
        return APOLLO_PRE_NOT_HERE;
    }
    if (Ch >= M->Preamps) {
        return APOLLO_PRE_BAD_VALUE;
    }
    switch (Param) {
    case APOLLO_PRE_MIC_LINE:
    case APOLLO_PRE_PAD:
    case APOLLO_PRE_48V:
    case APOLLO_PRE_LOWCUT:
    case APOLLO_PRE_PHASE: {
        unsigned long bit = 1ul << (6ul * q + Param);

        if (Value != 0 && Value != 1) {
            return APOLLO_PRE_BAD_VALUE;
        }
        ApolloPreampPut(W, N, hiBank ? 12ul : 0ul, Value ? bit : 0ul, bit);
        return APOLLO_PRE_OK;
    }
    case APOLLO_PRE_LINK: {
        /* The kext links a pair from its first preamp, both pair bits in one write. */
        unsigned long mask = 0x104ul << (12ul * (q >> 1));

        if (((M->LinkFirst >> Ch) & 1ul) == 0 || (Value != 0 && Value != 1)) {
            return APOLLO_PRE_BAD_VALUE;
        }
        ApolloPreampPut(W, N, hiBank ? 12ul : 0ul, Value ? mask : 0ul, mask);
        return APOLLO_PRE_OK;
    }
    case APOLLO_PRE_GAIN_DB: {
        unsigned long gainAB, gainC, shiftC;

        if (Value < APOLLO_PRE_GAIN_MIN_DB || Value > APOLLO_PRE_GAIN_MAX_DB) {
            return APOLLO_PRE_BAD_VALUE;
        }
        gainAB = (unsigned long)(Value - 10);
        if (gainAB > 54ul) {
            gainAB = 54ul;
        }
        gainC = (unsigned long)(Value - 9);
        if (gainC > 56ul) {
            gainC = 56ul;
        }
        shiftC = 6ul * q + ((q & 2ul) ? 4ul : 0ul);
        ApolloPreampPut(W, N, hiBank ? 25ul : 17ul, gainAB << (8ul * q), 0xFFul << (8ul * q));     /* GainA */
        ApolloPreampPut(W, N, hiBank ? 24ul : 16ul, gainAB << (8ul * q), 0xFFul << (8ul * q));     /* GainB */
        ApolloPreampPut(W, N, hiBank ? 14ul : 3ul, gainC << shiftC, 0x3Ful << shiftC);             /* GainC */
        return APOLLO_PRE_OK;
    }
    case APOLLO_PRE_HIZ_IMP:
        if (((M->HizImpedance >> Ch) & 1ul) == 0) {
            return APOLLO_PRE_NOT_HERE;
        }
        if (Value < 0 || Value >= (long)APOLLO_PRE_HIZ_CODES) {
            return APOLLO_PRE_BAD_VALUE;
        }
        ApolloPreampPut(W, N, 13ul, (unsigned long)Value << (4ul * Ch), 0xFul << (4ul * Ch));
        return APOLLO_PRE_OK;
    default:
        return APOLLO_PRE_NOT_HERE;
    }
}

/*
 * [Ins] Every preamp field of the kext's map, raw (port of oa:driver/ua_core.c ua_preamp_setting,
 * 2536-2660), for the Unison analog lease ONLY (driver\unison_plan.h, plugin.c): a Unison plug-in owns
 * its input's whole analog image, including the fields SET_INPUT_PARAM never offers (mic impedance 0x07,
 * CUSTOM 0x08, the custom byte lanes 0x09-0x0F, knob steps, the selected-parameter lane, Hi-Z, raw
 * record). SET_INPUT_PARAM keeps using ApolloPreampEncode above, unchanged. Value is the kext's raw value
 * for the field (GainC a 6-bit code, the lanes a byte, a switch 0/1, Phase 1 or a negative float's bits).
 * APOLLO_PRE_OK with *Index/*Mask/*Encoded, APOLLO_PRE_BAD_VALUE (a preamp the model lacks, Link from a
 * second channel), APOLLO_PRE_NOT_HERE (no such field there: the DSP-ring params 0x13/0x16, PGA line
 * 0x15 on the Twin X, Hi-Z beyond the model's Hi-Z inputs, a hi-bank field the kext refuses).
 */
#define APOLLO_PRE_F_GAIN_C         0x06ul
#define APOLLO_PRE_F_MIC_IMP        0x07ul
#define APOLLO_PRE_F_CUSTOM         0x08ul
#define APOLLO_PRE_F_GAIN_B         0x09ul
#define APOLLO_PRE_F_GAIN_A         0x0Aul
#define APOLLO_PRE_F_KNOB_STEPS     0x10ul
#define APOLLO_PRE_F_HIZ_IMP        0x11ul
#define APOLLO_PRE_F_SEL_PARAM      0x12ul
#define APOLLO_PRE_F_ROUTE          0x13ul      /* a DSP ring word, not a setting: refused here */
#define APOLLO_PRE_F_HIZ            0x14ul
#define APOLLO_PRE_F_PGA_LINE       0x15ul
#define APOLLO_PRE_F_LEVEL          0x16ul      /* a DSP ring word, not a setting: refused here */
#define APOLLO_PRE_F_RAW_REC        0x17ul
#define APOLLO_PRE_F_LAST           0x17ul

static __inline unsigned long ApolloPreampHizInputs(const APOLLO_PREAMP_MODEL *M)
{
    unsigned long n = 0, c;

    for (c = 0; M != 0 && c < 2; c++) {
        n += (M->HizDetect >> c) & 1ul;     /* "min(num_hiz, 2)": the x8p 2, the Twin X 1 */
    }
    return n;
}

static __inline int ApolloPreampField(const APOLLO_PREAMP_MODEL *M, unsigned long Ch, unsigned long Param,
                                      unsigned long Value, unsigned long *Index, unsigned long *Mask, unsigned long *Encoded)
{
    static const unsigned long impShift[4] = { 9ul, 16ul, 20ul, 24ul };
    unsigned long q = Ch & 3ul, idx, shift, mask, val;
    int hiBank = (Ch > 3);

    *Index = 0;
    *Mask = 0;
    *Encoded = 0;
    if (M == 0) {
        return APOLLO_PRE_NOT_HERE;
    }
    if (Param > APOLLO_PRE_F_LAST) {
        return APOLLO_PRE_NOT_HERE;
    }
    if (Ch >= M->Preamps) {
        return APOLLO_PRE_BAD_VALUE;
    }
    switch (Param) {
    case APOLLO_PRE_MIC_LINE:
    case APOLLO_PRE_PAD:
    case APOLLO_PRE_48V:
    case APOLLO_PRE_LOWCUT:
    case APOLLO_PRE_PHASE:
        idx = hiBank ? 12ul : 0ul;
        mask = (1ul << Param) << (q * 6ul);
        val = ((Param == APOLLO_PRE_PHASE) ? (((Value >> 31) & 1ul) != 0 || Value == 1ul) : (Value != 0)) ? mask : 0ul;
        break;
    case APOLLO_PRE_LINK:
        if (Ch & 1ul) {
            return APOLLO_PRE_BAD_VALUE;
        }
        idx = hiBank ? 12ul : 0ul;
        mask = 0x104ul << (12ul * (q >> 1));
        val = Value ? mask : 0ul;
        break;
    case APOLLO_PRE_F_GAIN_C:
        idx = hiBank ? 14ul : 3ul;
        shift = q * 6ul + ((q & 2ul) ? 4ul : 0ul);
        mask = 0x3Ful << shift;
        val = (Value & 0x3Ful) << shift;
        break;
    case APOLLO_PRE_F_MIC_IMP:
        idx = hiBank ? 13ul : 1ul;
        shift = impShift[q];
        mask = 0xFul << shift;
        val = (Value & 0xFul) << shift;
        break;
    case APOLLO_PRE_F_CUSTOM:
        idx = 11ul;
        mask = 1ul << (8ul + Ch);
        val = Value ? mask : 0ul;
        break;
    case 0x09ul: case 0x0Aul: case 0x0Bul: case 0x0Cul: case 0x0Dul: case 0x0Eul: case 0x0Ful:
        /* S16-S22 for ch 0-3, S24-S30 above: one byte lane per channel */
        idx = Param + 7ul + (hiBank ? 8ul : 0ul);
        shift = q * 8ul;
        mask = 0xFFul << shift;
        val = (Value & 0xFFul) << shift;
        break;
    case APOLLO_PRE_F_KNOB_STEPS:
        idx = hiBank ? 31ul : 23ul;
        shift = 4ul * q;
        mask = 0xFul << shift;
        val = (Value & 0xFul) << shift;
        break;
    case APOLLO_PRE_F_HIZ_IMP:
        if (Ch >= ApolloPreampHizInputs(M)) {
            return APOLLO_PRE_NOT_HERE;
        }
        idx = 13ul;
        shift = 4ul * Ch;
        mask = 0xFul << shift;
        val = (Value & 0xFul) << shift;
        break;
    case APOLLO_PRE_F_SEL_PARAM:
        idx = hiBank ? 31ul : 23ul;
        shift = 16ul + 2ul * q;
        mask = 3ul << shift;
        val = (Value & 3ul) << shift;
        break;
    case APOLLO_PRE_F_HIZ:
        if (hiBank) {
            return APOLLO_PRE_NOT_HERE;
        }
        idx = 0ul;
        mask = 1ul << (24ul + Ch);
        val = Value ? mask : 0ul;
        break;
    case APOLLO_PRE_F_RAW_REC:
        if (hiBank) {
            return APOLLO_PRE_NOT_HERE;
        }
        idx = 5ul;
        mask = 1ul << (16ul + Ch);
        val = Value ? mask : 0ul;
        break;
    case APOLLO_PRE_F_PGA_LINE:
        /* line input gain bypass (kPropPGADisabledInLineMode): oa:driver/ua_core.c:2526 S15 bit 24+ch on the
           kinds of UA_KIND_SET_IN_15, which include the x8p (devices/apollo-x8p.json line_pga_disable true);
           not on the Twin X Gen 1 (apollo-twin-x.json false). Found by the Nov 15 parity audit. */
        if (M->Model != APOLLO_MODEL_X8P) {
            return APOLLO_PRE_NOT_HERE;
        }
        idx = 15ul;
        mask = 1ul << (24ul + Ch);
        val = Value ? mask : 0ul;
        break;
    default:                /* 0x13 route, 0x16 level (DSP ring words, not settings) */
        return APOLLO_PRE_NOT_HERE;
    }
    *Index = idx;
    *Mask = mask;
    *Encoded = val;
    return APOLLO_PRE_OK;
}

/* oa:driver/ua_apollo.h ua_preamp_gain_db_from_code: the ordinary GainC decode (0 = none, 1..56 = dB - 9, above: 65). */
static __inline unsigned long ApolloPreampGainDbFromCode(unsigned long Code)
{
    Code &= 0x3Ful;
    return Code ? (Code < 57ul ? Code + 9ul : 65ul) : 0ul;
}

/* Readback: preamp Ch's GainC code (word 3 for 1-4, word 7 for 5-8; oa:driver/ua_apollo.h UA_RB_PREAMP_GAINC). */
static __inline unsigned long ApolloRbGainC(const unsigned long *Words, unsigned long Ch)
{
    unsigned long q = Ch & 3ul;

    return (Words[Ch < 4 ? 3 : 7] >> (q * 6ul + ((q & 2ul) ? 4ul : 0ul))) & 0x3Ful;
}

/* The Hi-Z impedance a code selects (oa:driver/ua_audio.c:7286-7288), or "?". */
static __inline const char *ApolloPreampHizName(unsigned long Code)
{
    static const char *const n[APOLLO_PRE_HIZ_CODES] = {
        "2.2M", "1M", "453k", "232k", "100k", "70k", "47k", "21k",
    };

    return Code < APOLLO_PRE_HIZ_CODES ? n[Code] : "?";
}

/* ------------------------------------------------------------------ */
/* Readback (GET_READBACK, 40 words)                                   */
/* ------------------------------------------------------------------ */

#define APOLLO_RB_WORDS_MIN         8ul     /* words 0..7 carry what is decoded here */

typedef struct _APOLLO_PREAMP_STATE {
    unsigned long   Line, Pad, Link, Phantom, LowCut, Phase;    /* 0/1 */
    unsigned long   GainC;              /* the raw 6-bit field */
    unsigned long   GainDb;             /* GainC + 9 (at most 65) */
    int             HasHiz;             /* the channel has a Hi-Z jack */
    int             HizIn;              /* readback reports a cable in it */
} APOLLO_PREAMP_STATE;

/*
 * One preamp from the readback words: flags in word 0 (ch 0-3) / 5 (4-7) at 6*(ch&3), GainC in
 * word 3 / 7 at 6*(ch&3) (+4 for ch&2), dB = GainC + 9; Hi-Z detect word 0 bit 24+ch. 0 when the
 * model has no such preamp. The Hi-Z impedance has no readback field (the host keeps it).
 */
static __inline int ApolloPreampDecode(const APOLLO_PREAMP_MODEL *M, const unsigned long *Words, unsigned long NWords,
                                       unsigned long Ch, APOLLO_PREAMP_STATE *S)
{
    unsigned long q = Ch & 3ul, f, g, b;

    S->Line = S->Pad = S->Link = S->Phantom = S->LowCut = S->Phase = 0;
    S->GainC = S->GainDb = 0;
    S->HasHiz = S->HizIn = 0;
    if (M == 0 || Words == 0 || NWords < APOLLO_RB_WORDS_MIN || Ch >= M->Preamps) {
        return 0;
    }
    f = Words[(Ch < 4) ? 0 : 5];
    g = Words[(Ch < 4) ? 3 : 7];
    b = 6ul * q;
    S->Line = (f >> (b + 0)) & 1ul;
    S->Pad = (f >> (b + 1)) & 1ul;
    S->Link = (f >> (b + 2)) & 1ul;
    S->Phantom = (f >> (b + 3)) & 1ul;
    S->LowCut = (f >> (b + 4)) & 1ul;
    S->Phase = (f >> (b + 5)) & 1ul;
    S->GainC = (g >> (6ul * q + ((q & 2ul) ? 4ul : 0ul))) & 0x3Ful;
    S->GainDb = (S->GainC + 9ul > 65ul) ? 65ul : S->GainC + 9ul;
    S->HasHiz = (Ch < 2 && ((M->HizDetect >> Ch) & 1ul) != 0);
    S->HizIn = S->HasHiz && ((Words[0] >> (24ul + Ch)) & 1ul) != 0;
    return 1;
}

/* Word 6: talkback bit 8; clock lock bit 31, valid only when bit 30 is set (oa:driver/ua_apollo.h:882-883). */
static __inline unsigned long ApolloRbTalkback(const unsigned long *Words)
{
    return (Words[6] >> 8) & 1ul;
}

static __inline int ApolloRbClockLock(const unsigned long *Words)
{
    if (((Words[6] >> 30) & 1ul) == 0) {
        return -1;                      /* not reported */
    }
    return (int)((Words[6] >> 31) & 1ul);
}
