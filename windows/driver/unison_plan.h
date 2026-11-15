/*
 * unison_plan.h - [Ins] the Unison analog lease as data: the preamp settings image a Unison chain owns
 * while it runs, how it is captured, patched by the plug-in's own {input, parameter, value} triples and
 * put back (docs/PLUGINS.md 1.9 and P8, docs/spec-twinx.md 4.4, docs/apollo-control-units.md 1.7).
 *
 * Port of open-apollo 2.1.0 (GPL-2.0), oa:driver/ua_core.c:2692-2955 (ua_unison_set_owned_locked,
 * ua_unison_capture_locked, ua_unison_changed_masks, ua_unison_patch_locked, ua_unison_restore's two
 * phases) and oa:driver/ua_plugin_instance.c:73-152 (the analog conflict, the PGA0 bootstrap words).
 * Plain C like preamp_plan.h: no kernel or Windows headers, no allocation, every function static __inline.
 * plugin.c runs it on the device's mixer cache (Ctx->MixerVal) and hardware readback (Ctx->Readback);
 * tools\plugin-plan-test runs it on synthetic arrays.
 *
 * The sequence upstream's driver enforces (ua_ioctl.h:1189-1214), and apollo.sys with it:
 *   CREATE STAGE | CHAIN | UNISON   the chain initialised, nothing routed, no analog field touched;
 *   BEGIN_UNISON                    capture the ordinary baseline (readback flags and GainC supersede the
 *                                   write cache), take logical ownership of the lane(s), write PGA 0
 *                                   (GainC code 0), wait for the mailbox to consume it plus a fresh
 *                                   readback, then 80 ms; nothing routed;
 *   UNISON_PARAMS (any time after)  the plug-in's own triples, validated whole, applied in setting order;
 *   ACTIVATE_UNISON                 route the staged chain (as op START), then the same fence;
 *   (the caller journals CUSTOM 1 and its 80 ms wait with UNISON_PARAMS, as UA's worker does);
 *   DESTROY / handle close          stop the chain and restore its plain strip, then the baseline:
 *                                   CUSTOM off plus every other changed field EXCEPT the gains, fence,
 *                                   80 ms, the gains (GainC, GainA, GainB), fence, GainC read back.
 * While owned, every other write that touches an owned field (SET_INPUT_PARAM, raw settings, the strips'
 * 48V resync) is refused with STATUS_DEVICE_BUSY; an unproven completion keeps the fields blocked until
 * the device is started again (upstream: "unknown completion blocks that input's settings until driver
 * reprobe").
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once
#include "preamp_plan.h"

#define APOLLO_UNISON_SETTINGS      38ul        /* the mailbox's settings (regs.h MIXER_NUM_SETTINGS) */
#define APOLLO_UNISON_WORDS_MAX     192ul       /* triples per UNISON_PARAMS (ua_ioctl.h UA_PLUGIN_UNISON_WORDS_MAX) */
#define APOLLO_UNISON_RB_WORDS      40ul        /* readback words read (apollo_ioctl.h APOLLO_READBACK_WORDS) */
#define APOLLO_UNISON_SETTLE_MS     80ul        /* "original CUSTOM-off settling" and the PGA0 wait */
#define APOLLO_UNISON_NONE          0x20ul      /* the mono half of a Synch */

#define APOLLO_UNISON_OK            0
#define APOLLO_UNISON_BUSY          1           /* custom on, owned already, or another lease holds a field */
#define APOLLO_UNISON_AGAIN         2           /* the mailbox is not settled: no trustworthy baseline now */
#define APOLLO_UNISON_INVAL         3           /* a preamp the model lacks, a malformed triple */
#define APOLLO_UNISON_PERM          4           /* an input not in the lease, a field the lease does not own */

typedef struct _APOLLO_UNISON_IMAGE {
    unsigned long   Synch;                              /* in1 | in2 << 16, 0x20 = none */
    unsigned long   Value[APOLLO_UNISON_SETTINGS];
    unsigned long   Mask[APOLLO_UNISON_SETTINGS];       /* the fields this lease owns */
    int             Valid;
} APOLLO_UNISON_IMAGE;

static __inline unsigned long ApolloUnisonInput(unsigned long Synch, unsigned long K)
{
    return K == 0 ? (Synch & 0xFFFFul) : (Synch >> 16);
}

/* The preamps a Synch names, as a mask (bit = preamp). */
static __inline unsigned long ApolloUnisonPreamps(unsigned long Synch)
{
    unsigned long k, ch, m = 0;

    for (k = 0; k < 2; k++) {
        ch = ApolloUnisonInput(Synch, k);
        if (ch != APOLLO_UNISON_NONE && ch < 32ul) {
            m |= 1ul << ch;
        }
    }
    return m;
}

/*
 * ua_unison_capture_locked. Cache = the mailbox's value cache (38 settings), Rb = the hardware readback
 * (40 words) and RbReady = the mailbox is settled and that readback is newer than the last write (the
 * caller's check: no dirty cache, SEQ_RD = SEQ_WR, a readback seen, words 38/39 not all ones). Custom
 * (bit = preamp) = the CUSTOM bits the cache holds; Owned = preamps another lease owns; Taken = every
 * field another lease (or a blocked failure) holds. The ordinary GainA/B follow the physical GainC.
 */
static __inline int ApolloUnisonCapture(const APOLLO_PREAMP_MODEL *M, unsigned long Synch, const unsigned long *Cache,
                                        const unsigned long *Rb, int RbReady, unsigned long Owned, const unsigned long *Taken,
                                        APOLLO_UNISON_IMAGE *Im)
{
    unsigned long k, ch, p, i, mask, val, idx, enc, sh, q;

    for (i = 0; i < APOLLO_UNISON_SETTINGS; i++) {
        Im->Value[i] = Cache[i];
        Im->Mask[i] = 0;
    }
    Im->Synch = Synch;
    Im->Valid = 0;
    if (!RbReady || Rb[38] == 0xFFFFFFFFul || Rb[39] == 0xFFFFFFFFul) {
        return APOLLO_UNISON_AGAIN;
    }
    if (ApolloUnisonPreamps(Synch) == 0) {
        return APOLLO_UNISON_INVAL;
    }
    for (k = 0; k < 2; k++) {
        ch = ApolloUnisonInput(Synch, k);
        if (ch == APOLLO_UNISON_NONE) {
            continue;
        }
        if (M == 0 || ch >= M->Preamps || ch >= 8ul) {
            return APOLLO_UNISON_INVAL;
        }
        if (((Cache[11] >> (8ul + ch)) & 1ul) != 0 || ((Owned >> ch) & 1ul) != 0) {
            return APOLLO_UNISON_BUSY;
        }
        for (p = 0; p <= APOLLO_PRE_F_RAW_REC; p++) {
            if (p == APOLLO_PRE_LINK) {
                continue;               /* pair-wide writes are forbidden */
            }
            if (ApolloPreampField(M, ch, p, 0, &idx, &mask, &enc) == APOLLO_PRE_OK && idx < APOLLO_UNISON_SETTINGS) {
                Im->Mask[idx] |= mask;
            }
        }
        q = ch & 3ul;
        i = ch < 4 ? 0ul : 12ul;
        mask = 0x3Ful << (6ul * q);
        Im->Mask[i] |= mask;
        Im->Value[i] = (Im->Value[i] & ~mask) | (Rb[ch < 4 ? 0 : 5] & mask);
        i = ch < 4 ? 3ul : 14ul;
        sh = 6ul * q + ((q & 2ul) ? 4ul : 0ul);
        mask = 0x3Ful << sh;
        Im->Value[i] = (Im->Value[i] & ~mask) | (Rb[ch < 4 ? 3 : 7] & mask);
        if (ch < 2 && ch < ApolloPreampHizInputs(M)) {
            mask = 1ul << (24ul + ch);
            Im->Value[0] = (Im->Value[0] & ~mask) | (Rb[0] & mask);
        }
        /* ordinary GainB then GainA follow the physical GainC, not a stale write cache */
        val = ApolloPreampGainDbFromCode((Im->Value[i] >> sh) & 63ul);
        val = val >= 10 ? val - 10 : 0;
        for (p = APOLLO_PRE_F_GAIN_B; p <= APOLLO_PRE_F_GAIN_A; p++) {
            if (ApolloPreampField(M, ch, p, val, &idx, &mask, &enc) == APOLLO_PRE_OK && idx < APOLLO_UNISON_SETTINGS) {
                Im->Value[idx] = (Im->Value[idx] & ~mask) | enc;
            }
            val = ApolloPreampGainDbFromCode(ApolloRbGainC(Rb, ch));
            val = val >= 10 ? val - 10 : 0;
        }
    }
    for (i = 0; i < APOLLO_UNISON_SETTINGS; i++) {
        if (Taken != 0 && (Taken[i] & Im->Mask[i]) != 0) {
            return APOLLO_UNISON_BUSY;
        }
    }
    Im->Valid = 1;
    return APOLLO_UNISON_OK;
}

/*
 * ua_unison_patch_locked: Words = NW / 3 triples {input, parameter, value}, the whole batch validated before
 * anything changes; Next = the image with every triple applied (duplicates coalesce, the last wins), Requested =
 * the fields the batch names (the masks the caller writes, in setting order). Inputs outside the lease and
 * fields it does not own: PERM; Link, a 0x20 input, a malformed count: INVAL.
 */
static __inline int ApolloUnisonPatch(const APOLLO_PREAMP_MODEL *M, const APOLLO_UNISON_IMAGE *Im, const unsigned long *Words,
                                      unsigned long NW, APOLLO_UNISON_IMAGE *Next, unsigned long *Requested)
{
    unsigned long n, i, ch, param, value, idx, mask, enc;

    for (i = 0; i < APOLLO_UNISON_SETTINGS; i++) {
        Requested[i] = 0;
    }
    *Next = *Im;
    if (!Im->Valid || NW % 3ul != 0 || NW > APOLLO_UNISON_WORDS_MAX) {
        return APOLLO_UNISON_INVAL;
    }
    for (n = 0; n < NW; n += 3) {
        ch = Words[n];
        param = Words[n + 1];
        value = Words[n + 2];
        if (ch == APOLLO_UNISON_NONE || param == APOLLO_PRE_LINK) {
            return APOLLO_UNISON_INVAL;
        }
        if (ch != ApolloUnisonInput(Im->Synch, 0) && ch != ApolloUnisonInput(Im->Synch, 1)) {
            return APOLLO_UNISON_PERM;
        }
        if (ApolloPreampField(M, ch, param, value, &idx, &mask, &enc) != APOLLO_PRE_OK || idx >= APOLLO_UNISON_SETTINGS) {
            return APOLLO_UNISON_INVAL;
        }
        if ((mask & Im->Mask[idx]) != mask) {
            return APOLLO_UNISON_PERM;
        }
        Requested[idx] |= mask;
        Next->Value[idx] = (Next->Value[idx] & ~mask) | enc;
    }
    return APOLLO_UNISON_OK;
}

/* ua_unison_changed_masks: the owned fields whose image value differs from the cache, compared field by field. */
static __inline void ApolloUnisonChanged(const APOLLO_PREAMP_MODEL *M, const APOLLO_UNISON_IMAGE *Im, const unsigned long *Cache,
                                         unsigned long *Masks)
{
    unsigned long k, ch, p, idx, mask, enc;

    for (k = 0; k < APOLLO_UNISON_SETTINGS; k++) {
        Masks[k] = 0;
    }
    for (k = 0; k < 2; k++) {
        ch = ApolloUnisonInput(Im->Synch, k);
        if (ch == APOLLO_UNISON_NONE) {
            continue;
        }
        for (p = 0; p <= APOLLO_PRE_F_RAW_REC; p++) {
            if (p == APOLLO_PRE_LINK || ApolloPreampField(M, ch, p, 0, &idx, &mask, &enc) != APOLLO_PRE_OK ||
                idx >= APOLLO_UNISON_SETTINGS) {
                continue;
            }
            if ((mask & Im->Mask[idx]) == mask && (mask & (Im->Value[idx] ^ Cache[idx])) != 0) {
                Masks[idx] |= mask;
            }
        }
    }
}

/*
 * ua_unison_restore's two writes of the baseline Base over the cache: BeforeGain = CUSTOM off and every
 * changed field except GainC/A/B (ascending setting order: GainC must not go before CUSTOM 0 in one batch),
 * Gain = the three gains, written after the fence and the 80 ms.
 */
static __inline void ApolloUnisonRestorePlan(const APOLLO_PREAMP_MODEL *M, const APOLLO_UNISON_IMAGE *Base, const unsigned long *Cache,
                                             unsigned long *BeforeGain, unsigned long *Gain)
{
    static const unsigned long gains[3] = { APOLLO_PRE_F_GAIN_C, APOLLO_PRE_F_GAIN_A, APOLLO_PRE_F_GAIN_B };
    unsigned long k, ch, p, idx, mask, enc;

    ApolloUnisonChanged(M, Base, Cache, BeforeGain);
    for (k = 0; k < APOLLO_UNISON_SETTINGS; k++) {
        Gain[k] = 0;
    }
    for (k = 0; k < 2; k++) {
        ch = ApolloUnisonInput(Base->Synch, k);
        if (ch == APOLLO_UNISON_NONE) {
            continue;
        }
        if (ApolloPreampField(M, ch, APOLLO_PRE_F_CUSTOM, 0, &idx, &mask, &enc) == APOLLO_PRE_OK && idx < APOLLO_UNISON_SETTINGS) {
            BeforeGain[idx] |= mask;
        }
        for (p = 0; p < 3; p++) {
            if (ApolloPreampField(M, ch, gains[p], 0, &idx, &mask, &enc) == APOLLO_PRE_OK && idx < APOLLO_UNISON_SETTINGS) {
                BeforeGain[idx] &= ~mask;
                Gain[idx] |= mask;
            }
        }
    }
}

/* The PGA0 bootstrap triples (ua_inst_analog_bootstrap): GainC code 0 on every input of the lease. Returns the word count. */
static __inline unsigned long ApolloUnisonPga0(unsigned long Synch, unsigned long Words[6])
{
    unsigned long k, ch, n = 0;

    for (k = 0; k < 2; k++) {
        ch = ApolloUnisonInput(Synch, k);
        if (ch == APOLLO_UNISON_NONE) {
            continue;
        }
        Words[n++] = ch;
        Words[n++] = APOLLO_PRE_F_GAIN_C;
        Words[n++] = 0;
    }
    return n;
}

/* Is the restored GainC of every input the baseline's? (the last check of ua_unison_restore) */
static __inline int ApolloUnisonGainRestored(const APOLLO_UNISON_IMAGE *Base, const unsigned long *Rb)
{
    unsigned long k, ch, q, sh;

    for (k = 0; k < 2; k++) {
        ch = ApolloUnisonInput(Base->Synch, k);
        if (ch == APOLLO_UNISON_NONE || ch >= 8ul) {
            continue;
        }
        q = ch & 3ul;
        sh = 6ul * q + ((q & 2ul) ? 4ul : 0ul);
        if (ApolloRbGainC(Rb, ch) != ((Base->Value[ch < 4 ? 3 : 14] >> sh) & 0x3Ful)) {
            return 0;
        }
    }
    return 1;
}
