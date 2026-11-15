/*
 * preamp-plan-test.c - host-side check of driver\preamp_plan.h, the preamp controls per model
 * (docs/spec-twinx.md 4.1 and 6.9.5, gate G5). Builds the exact code mixer.c runs and checks:
 *   1. the x8p: every SET_INPUT_PARAM (preamps 0-11, every code 0-0x1FF and a few more, values
 *      -3..70 and the extremes) gives the same result and the same mailbox writes, in the same
 *      order, as mixer.c ApolloSetInputParam before G5 (copied below as written there);
 *   2. the Twin X Gen 1 against spec-twinx 4.1: preamps 1-2 only, the S0 flag bits 0,1,3,4,5 and
 *      6,7,9,10,11, link from input 1 only (mask 0x104), gain in S17/S16 lanes 0-1 and S3 bits
 *      0-5 / 6-11, Hi-Z impedance codes 0-7 in S13 [3:0] on input 1 only, and nothing else: never
 *      S12, S14, S24, S25 (preamps 5-8 / HP level / dim on the Twin X), S11 (custom preamp), S1
 *      (mic impedance);
 *   3. the readback decoding: synthetic words, and the round trip (what the encoder writes into
 *      S0/S3 decodes back from words 0/3 to the same values);
 *   4. the 48V bit the strips' resync writes (strips.c: S0 bit 6*ch+3) is the encoder's.
 * Exit code 0 = all pass.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "..\..\driver\preamp_plan.h"

static int g_fail = 0, g_pass = 0;

static void Check(int ok, const char *what)
{
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        printf("FAIL  %s\n", what);
    }
}

/* ---- mixer.c ApolloSetInputParam before G5 (builds\base-pre-g56), writes recorded ---- */

#define OLD_OK              0
#define OLD_INVALID         1   /* STATUS_INVALID_PARAMETER */
#define OLD_NOT_SUPPORTED   2   /* STATUS_NOT_SUPPORTED */

typedef struct _OLD_OUT {
    unsigned long N;
    APOLLO_PREAMP_WRITE W[3];
} OLD_OUT;

static int OldWrite(OLD_OUT *O, ULONG Index, ULONG Value, ULONG Mask)
{
    if (O->N < 3) {
        O->W[O->N].Setting = Index;
        O->W[O->N].Value = Value;
        O->W[O->N].Mask = Mask;
        O->N++;
    }
    return OLD_OK;
}

static int OldSetInputParam(ULONG Preamp, ULONG Param, LONG Value, OLD_OUT *O)
{
    ULONG ch = Preamp;
    ULONG q = ch & 3;
    int status;

    O->N = 0;
    if (ch >= 8) {
        return OLD_INVALID;
    }
    switch (Param) {
    case 0x00:
    case 0x01:
    case 0x03:
    case 0x04:
    case 0x05: {
        ULONG setting = (ch < 4) ? 0 : 12;
        ULONG bit = 1u << (6 * q + Param);

        if (Value != 0 && Value != 1) {
            return OLD_INVALID;
        }
        return OldWrite(O, setting, Value ? bit : 0, bit);
    }
    case 0x02: {
        ULONG setting = (ch < 4) ? 0 : 12;
        ULONG mask = 0x104u << (12 * (q >> 1));

        if ((ch & 1) || (Value != 0 && Value != 1)) {
            return OLD_INVALID;
        }
        return OldWrite(O, setting, Value ? mask : 0, mask);
    }
    case 0x100: {
        LONG db = Value;
        ULONG gainAB, gainC, shiftC;

        if (db < 10 || db > 65) {
            return OLD_INVALID;
        }
        gainAB = (ULONG)min(db - 10, 54);
        gainC = (ULONG)min(db - 9, 56);
        shiftC = 6 * q + ((q & 2) ? 4 : 0);
        status = OldWrite(O, (ch < 4) ? 17 : 25, gainAB << (8 * q), 0xFFu << (8 * q));
        if (status == OLD_OK) {
            status = OldWrite(O, (ch < 4) ? 16 : 24, gainAB << (8 * q), 0xFFu << (8 * q));
        }
        if (status == OLD_OK) {
            status = OldWrite(O, (ch < 4) ? 3 : 14, gainC << shiftC, 0x3Fu << shiftC);
        }
        return status;
    }
    default:
        return OLD_NOT_SUPPORTED;
    }
}

static int NewClass(int R)
{
    return R == APOLLO_PRE_OK ? OLD_OK : R == APOLLO_PRE_BAD_VALUE ? OLD_INVALID : OLD_NOT_SUPPORTED;
}

static void CheckX8p(void)
{
    static const LONG values[] = { -2147483647L - 1, -65, -3, -2, -1, 0, 1, 2, 3, 7, 8, 9, 10, 11, 40, 41,
                                   54, 55, 56, 63, 64, 65, 66, 70, 255, 256, 2147483647L };
    const APOLLO_PREAMP_MODEL *m = ApolloPreampModelFor(APOLLO_MODEL_X8P);
    APOLLO_PREAMP_WRITE w[APOLLO_PRE_WRITES_MAX];
    unsigned long n, ch, param, cases = 0, same = 0, k, v;
    char what[160];
    OLD_OUT o;

    Check(m != NULL && m->Preamps == 8 && ApolloPreampCount(APOLLO_MODEL_X8P) == 8, "x8p: 8 preamps (GET_INFO NumPreamps 8)");
    if (m == NULL)
        return;
    for (ch = 0; ch < 12; ch++) {
        for (param = 0; param <= 0x1FF + 4; param++) {
            ULONG p = (param <= 0x1FF) ? param : (param == 0x200 ? 0xFFFFFFFFu : param == 0x201 ? 0x10000100u :
                                                 param == 0x202 ? 0x80000000u : 0x111u);

            for (v = 0; v < sizeof(values) / sizeof(values[0]); v++) {
                int r = ApolloPreampEncode(m, ch, p, values[v], w, &n);
                int ro = OldSetInputParam(ch, p, values[v], &o);
                int eq = (NewClass(r) == ro) && (r != APOLLO_PRE_OK || n == o.N);

                for (k = 0; eq && r == APOLLO_PRE_OK && k < n; k++) {
                    eq = w[k].Setting == o.W[k].Setting && w[k].Value == o.W[k].Value && w[k].Mask == o.W[k].Mask;
                }
                cases++;
                if (eq) {
                    same++;
                } else if (cases - same <= 5) {
                    sprintf_s(what, sizeof(what), "x8p ch %lu param 0x%lX value %ld: new %d/%lu, old %d/%lu", ch, p,
                              values[v], r, n, ro, o.N);
                    Check(0, what);
                }
            }
        }
    }
    sprintf_s(what, sizeof(what), "x8p: %lu of %lu SET_INPUT_PARAM inputs give mixer.c's pre-G5 result and writes",
              same, cases);
    Check(same == cases, what);
    Check(ApolloPreampEncode(m, 0, APOLLO_PRE_HIZ_IMP, 0, w, &n) == APOLLO_PRE_NOT_HERE && n == 0,
          "x8p: Hi-Z impedance not offered (STATUS_NOT_SUPPORTED, as before)");
}

/* ---- the Twin X Gen 1, spec-twinx 4.1 ---- */

static void CheckTwinX(void)
{
    const APOLLO_PREAMP_MODEL *m = ApolloPreampModelFor(APOLLO_MODEL_TWINX);
    static const unsigned long flagBits[2][6] = { { 0, 1, 2, 3, 4, 5 }, { 6, 7, 8, 9, 10, 11 } };
    APOLLO_PREAMP_WRITE w[APOLLO_PRE_WRITES_MAX];
    unsigned long n, ch, code, k, badSetting = 0, okCount = 0;
    long v, db;
    char what[160];

    Check(m != NULL && m->Preamps == 2 && ApolloPreampCount(APOLLO_MODEL_TWINX) == 2,
          "Twin X: 2 preamps (GET_INFO NumPreamps 2)");
    Check(ApolloPreampCount(APOLLO_MODEL_NONE) == 0 && ApolloPreampModelFor(7) == NULL, "other models: no preamps");
    if (m == NULL)
        return;

    /* Flags: S0 bit 6*ch + code, 0/1 only */
    for (ch = 0; ch < 2; ch++) {
        static const unsigned long codes[5] = { APOLLO_PRE_MIC_LINE, APOLLO_PRE_PAD, APOLLO_PRE_48V, APOLLO_PRE_LOWCUT,
                                                APOLLO_PRE_PHASE };

        for (k = 0; k < 5; k++) {
            unsigned long bit = 1ul << flagBits[ch][codes[k]];

            sprintf_s(what, sizeof(what), "Twin X preamp %lu code %lu: S0 bit %lu", ch + 1, codes[k], flagBits[ch][codes[k]]);
            Check(ApolloPreampEncode(m, ch, codes[k], 1, w, &n) == APOLLO_PRE_OK && n == 1 && w[0].Setting == 0 &&
                  w[0].Mask == bit && w[0].Value == bit, what);
            Check(ApolloPreampEncode(m, ch, codes[k], 0, w, &n) == APOLLO_PRE_OK && n == 1 && w[0].Setting == 0 &&
                  w[0].Mask == bit && w[0].Value == 0, "Twin X flag off: same bit, value 0");
            Check(ApolloPreampEncode(m, ch, codes[k], 2, w, &n) == APOLLO_PRE_BAD_VALUE && n == 0,
                  "Twin X flag 2: invalid");
        }
    }
    /* spec 4.1: S0 bits 0,1,3,4,5 (ch 0) and 6,7,9,10,11 (ch 1) */
    Check(ApolloPreampEncode(m, 1, APOLLO_PRE_48V, 1, w, &n) == APOLLO_PRE_OK && w[0].Mask == 0x200,
          "Twin X 48V input 2 = S0 bit 9 (the strips' 48V resync bit, strips.c 6*ch+3)");
    Check(ApolloPreampEncode(m, 0, APOLLO_PRE_48V, 1, w, &n) == APOLLO_PRE_OK && w[0].Mask == 0x8,
          "Twin X 48V input 1 = S0 bit 3");

    /* Link: input 1 only, both pair bits 0x104 */
    Check(ApolloPreampEncode(m, 0, APOLLO_PRE_LINK, 1, w, &n) == APOLLO_PRE_OK && n == 1 && w[0].Setting == 0 &&
          w[0].Mask == 0x104 && w[0].Value == 0x104, "Twin X link: from input 1, S0 mask 0x104");
    Check(ApolloPreampEncode(m, 1, APOLLO_PRE_LINK, 1, w, &n) == APOLLO_PRE_BAD_VALUE,
          "Twin X link from input 2: invalid (odd preamp, as the x8p)");

    /* Gain 10-65 dB: GainA S17, GainB S16 byte lane ch, GainC S3 6 bits at 6*ch */
    for (ch = 0; ch < 2; ch++) {
        for (db = 10; db <= 65; db++) {
            unsigned long ab = (unsigned long)(db - 10 > 54 ? 54 : db - 10), c = (unsigned long)(db - 9);

            if (ApolloPreampEncode(m, ch, APOLLO_PRE_GAIN_DB, db, w, &n) == APOLLO_PRE_OK && n == 3 &&
                w[0].Setting == 17 && w[0].Mask == (0xFFul << (8 * ch)) && w[0].Value == (ab << (8 * ch)) &&
                w[1].Setting == 16 && w[1].Mask == (0xFFul << (8 * ch)) && w[1].Value == (ab << (8 * ch)) &&
                w[2].Setting == 3 && w[2].Mask == (0x3Ful << (6 * ch)) && w[2].Value == (c << (6 * ch)))
                okCount++;
        }
    }
    Check(okCount == 112, "Twin X gain 10-65 dB: S17/S16 lane 0-1, S3 bits 0-5 / 6-11 (112 of 112)");
    Check(ApolloPreampEncode(m, 0, APOLLO_PRE_GAIN_DB, 9, w, &n) == APOLLO_PRE_BAD_VALUE &&
          ApolloPreampEncode(m, 1, APOLLO_PRE_GAIN_DB, 66, w, &n) == APOLLO_PRE_BAD_VALUE,
          "Twin X gain 9 / 66 dB: invalid");

    /* Hi-Z impedance: input 1 only, S13 [3:0], codes 0-7 */
    for (code = 0; code < 8; code++) {
        sprintf_s(what, sizeof(what), "Twin X Hi-Z impedance code %lu (%s): S13 [3:0]", code, ApolloPreampHizName(code));
        Check(ApolloPreampEncode(m, 0, APOLLO_PRE_HIZ_IMP, (long)code, w, &n) == APOLLO_PRE_OK && n == 1 &&
              w[0].Setting == 13 && w[0].Mask == 0xF && w[0].Value == code, what);
    }
    Check(ApolloPreampEncode(m, 0, APOLLO_PRE_HIZ_IMP, 8, w, &n) == APOLLO_PRE_BAD_VALUE &&
          ApolloPreampEncode(m, 0, APOLLO_PRE_HIZ_IMP, -1, w, &n) == APOLLO_PRE_BAD_VALUE,
          "Twin X Hi-Z impedance 8 / -1: invalid");
    Check(ApolloPreampEncode(m, 1, APOLLO_PRE_HIZ_IMP, 0, w, &n) == APOLLO_PRE_NOT_HERE,
          "Twin X Hi-Z impedance on input 2: not supported (no Hi-Z jack there)");
    Check(strcmp(ApolloPreampHizName(0), "2.2M") == 0 && strcmp(ApolloPreampHizName(2), "453k") == 0 &&
          strcmp(ApolloPreampHizName(7), "21k") == 0 && strcmp(ApolloPreampHizName(8), "?") == 0,
          "Hi-Z codes 0-7 = 2.2M 1M 453k 232k 100k 70k 47k 21k (oa:driver/ua_audio.c:7286-7288)");

    /* Refused: preamps 3-8, mic impedance 0x07, custom preamp 0x08, every other code */
    Check(ApolloPreampEncode(m, 2, APOLLO_PRE_GAIN_DB, 40, w, &n) == APOLLO_PRE_BAD_VALUE &&
          ApolloPreampEncode(m, 7, APOLLO_PRE_48V, 1, w, &n) == APOLLO_PRE_BAD_VALUE,
          "Twin X preamps 3-8: invalid (they do not exist)");
    Check(ApolloPreampEncode(m, 0, 0x07, 1, w, &n) == APOLLO_PRE_NOT_HERE &&
          ApolloPreampEncode(m, 0, 0x08, 0, w, &n) == APOLLO_PRE_NOT_HERE &&
          ApolloPreampEncode(m, 0, 0x14, 1, w, &n) == APOLLO_PRE_NOT_HERE,
          "Twin X: mic impedance, custom (Unison) preamp, Hi-Z host write not offered");
    /* Every accepted input of every code: only S0, S3, S13, S16, S17 (nothing for preamps 5-8, HP, dim, S11, S1) */
    for (ch = 0; ch < 4; ch++) {
        unsigned long p;

        for (p = 0; p <= 0x1FF; p++) {
            for (v = -1; v <= 70; v++) {
                if (ApolloPreampEncode(m, ch, p, v, w, &n) != APOLLO_PRE_OK)
                    continue;
                for (k = 0; k < n; k++) {
                    unsigned long s = w[k].Setting;

                    if (s != 0 && s != 3 && s != 13 && s != 16 && s != 17)
                        badSetting++;
                    if (s == 0 && (w[k].Mask & ~0xFFFul) != 0)
                        badSetting++;               /* only the flag bits of preamps 1-2 */
                    if (s == 3 && (w[k].Mask & ~0xFFFul) != 0)
                        badSetting++;
                    if (s == 13 && w[k].Mask != 0xFul)
                        badSetting++;               /* never talkback (bit 8) or ALT (29:28) from here */
                    if ((s == 16 || s == 17) && (w[k].Mask & ~0xFFFFul) != 0)
                        badSetting++;
                }
            }
        }
    }
    Check(badSetting == 0, "Twin X: only S0 bits 0-11, S3 bits 0-11, S13 [3:0], S16/S17 lanes 0-1 are ever written");
}

/* ---- readback ---- */

static void CheckReadback(void)
{
    const APOLLO_PREAMP_MODEL *t = ApolloPreampModelFor(APOLLO_MODEL_TWINX), *x = ApolloPreampModelFor(APOLLO_MODEL_X8P);
    unsigned long words[40], set[38];
    APOLLO_PREAMP_STATE s;
    APOLLO_PREAMP_WRITE w[APOLLO_PRE_WRITES_MAX];
    unsigned long n, k, ch, ok;
    long db;

    memset(words, 0, sizeof(words));
    /* input 1: Line, link, Hi-Z cable in, GainC 32 (41 dB); input 2: 48V, phase, GainC 56 (65 dB) */
    words[0] = (1ul << 0) | (1ul << 2) | (1ul << 8) | (1ul << 9) | (1ul << 11) | (1ul << 24);
    words[3] = 32ul | (56ul << 6);
    Check(ApolloPreampDecode(t, words, 40, 0, &s) && s.Line == 1 && s.Pad == 0 && s.Link == 1 && s.Phantom == 0 &&
          s.LowCut == 0 && s.Phase == 0 && s.GainC == 32 && s.GainDb == 41 && s.HasHiz && s.HizIn,
          "Twin X readback input 1: Line, link, 41 dB, Hi-Z cable in");
    Check(ApolloPreampDecode(t, words, 40, 1, &s) && s.Line == 0 && s.Link == 1 && s.Phantom == 1 && s.Phase == 1 &&
          s.GainDb == 65 && !s.HasHiz && !s.HizIn,
          "Twin X readback input 2: 48V, phase, 65 dB, no Hi-Z jack (bit 25 not read)");
    words[0] |= 1ul << 25;
    Check(ApolloPreampDecode(t, words, 40, 1, &s) && !s.HasHiz && !s.HizIn, "Twin X: bit 25 is not an input 2 Hi-Z jack");
    Check(!ApolloPreampDecode(t, words, 40, 2, &s) && !ApolloPreampDecode(t, words, 4, 0, &s),
          "Twin X readback: no input 3; short readback refused");
    Check(ApolloPreampDecode(x, words, 40, 1, &s) && s.HasHiz && s.HizIn, "x8p readback: input 2 Hi-Z jack (bit 25)");
    words[5] = 1ul << (6 * 1 + 3);
    words[7] = 20ul << 6;
    Check(ApolloPreampDecode(x, words, 40, 5, &s) && s.Phantom == 1 && s.GainDb == 29 && !s.HasHiz,
          "x8p readback input 6: flags word 5, GainC word 7");
    words[6] = (1ul << 8) | (1ul << 30) | (1ul << 31);
    Check(ApolloRbTalkback(words) == 1 && ApolloRbClockLock(words) == 1, "word 6: talkback on, clock locked");
    words[6] = 1ul << 31;
    Check(ApolloRbTalkback(words) == 0 && ApolloRbClockLock(words) == -1, "word 6: lock bit without bit 30 = not reported");

    /* Round trip on the Twin X: encode into a settings image; the readback words 0/3 mirror S0/S3 */
    ok = 1;
    for (ch = 0; ch < 2; ch++) {
        for (db = 10; db <= 65; db++) {
            memset(set, 0, sizeof(set));
            if (ApolloPreampEncode(t, ch, APOLLO_PRE_GAIN_DB, db, w, &n) != APOLLO_PRE_OK)
                ok = 0;
            for (k = 0; k < n; k++)
                set[w[k].Setting] = (set[w[k].Setting] & ~w[k].Mask) | (w[k].Value & w[k].Mask);
            if (ApolloPreampEncode(t, ch, APOLLO_PRE_48V, 1, w, &n) != APOLLO_PRE_OK)
                ok = 0;
            set[w[0].Setting] = (set[w[0].Setting] & ~w[0].Mask) | (w[0].Value & w[0].Mask);
            memset(words, 0, sizeof(words));
            words[0] = set[0];
            words[3] = set[3];
            if (!ApolloPreampDecode(t, words, 40, ch, &s) || s.GainDb != (unsigned long)db || s.Phantom != 1)
                ok = 0;
            if (!ApolloPreampDecode(t, words, 40, ch ^ 1, &s) || s.Phantom != 0 || s.GainC != 0)
                ok = 0;                     /* the other preamp untouched */
        }
    }
    Check(ok, "Twin X round trip: gain 10-65 dB and 48V written to S0/S3 read back from words 0/3, the other input untouched");
}

int main(void)
{
    CheckX8p();
    CheckTwinX();
    CheckReadback();
    printf("preamp-plan-test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
