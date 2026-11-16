/*
 * strip_plan.h - the Mic Input Strip planner (docs/STRIPS.md 2.6, 2.7, 10.8): private-block
 * addresses, the memspec link table, and the words of a pass's transactions, as plain lists.
 * No OS calls and no allocation: apollo.sys (strips.c) and the host-side check
 * tools\strip-plan-test build the same code. The includer defines ULONG.
 *
 * A pass is: one PREPARE transaction per preamp in StripMask (zero-fills, memspec, header,
 * parameters), then ONE START transaction that starts them all (SYNCH, span, span, route per
 * input). No strip runs while any strip's memory is written. That is UA's own order (the
 * captured Console bring-up, open-apollo tools/uausb/soloinit.py:569-620: every strip's
 * private blocks first, then a single "enable" transaction of SYNCH + span/route per strip).
 * open-apollo's driver starts each strip in the same transaction as its memory
 * (ua_strips.c:610-705, run one by one at :1689-1754); with eight strips on the x8p that left
 * five of them at exact digital zero (docs/STRIPS.md 10.8).
 *
 * Port of open-apollo (GPL-2.0): ua_strips.c:372-384 (rows), 498-520 (addresses),
 * 538-563 (header, captured 48 kHz parameters), 579-601 (start words), 610-705 (words),
 * ua_plugin_upload.c:114-160 (memspec).
 *
 * [Virtual] (docs/STRIPS.md 13) The plain Input Strip (pluginId 0x83E) on the Virtual inputs, after
 * the "Virtual Input Strips" section below: a pass is then every Mic PREPARE, every Virtual PREPARE,
 * and ONE START for all of them (the "plan mask" Pm: bits 0-7 Mic, 8-15 VIRTUAL 1-8). Every function
 * above that section is pinned by strip-plan-test's golden digests.
 *
 * [AuxStrips] (docs/STRIPS.md 14) The same Input Strip on the aux returns, DSP inputs 0x10-0x13 (AUX 1 L/R,
 * AUX 2 L/R): Pm bits 16-19, PREPAREd after every Virtual strip, started in the same single START.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#define STRIP_PLAN_ROWS         10      /* instance header, m0-m4, c0-c3 */
#define STRIP_PLAN_INPUTS       8       /* preamps 1-8 = DSP inputs (synch) 0-7 on the x8p */
#define STRIP_PLAN_PREPARE_ENTS 13      /* 10 zero-fills + 3 DMA refs */
#define STRIP_PLAN_START_ENTS   4       /* per input: SYNCH, span, span, route */
#define STRIP_PLAN_ENTS_MAX     (STRIP_PLAN_START_ENTS * STRIP_PLAN_INPUTS)    /* 32: a Mic-only transaction */
#define STRIP_PLAN_PAYLOAD_MAX  96      /* dwords: memspec 19 + header 9 + parameters 55 = 83 */
#define STRIP_PLAN_ENT_SLOTS    80      /* [Virtual] entries a plan holds: the START of 8 Mic + 8 Virtual + [AuxStrips] 4 aux Input Strips */

#define STRIP_CMD_ZERO          0x00080004u     /* {cmd, addr, 0, dwords} */
#define STRIP_CMD_SYNCH         0x001F0004u     /* {cmd, 0x20 << 16 | synch, header (0 = stop), 0} */
#define STRIP_CMD_INPUT         0x001E0004u     /* {cmd, synch, param, value} */
#define STRIP_SYNCH_MONO        0x20u           /* in2: FW_SYNCH_IN_INVALID */
#define STRIP_INPUT_ROUTE       7u              /* DirectOutputRoute */
#define STRIP_INPUT_SPAN        8u              /* SpanInfo */
/*
 * The route word: [7:0] direct-output signal (0xFF none), [15:8] destination synch-out (0xFF
 * none), bit 16 the input feeds the monitor mix, bit 17 stereo leader (open-apollo
 * mixer-engine/model_features.py:218-235). 0x1FFFF = no direct output, monitor mix on: UA's
 * ClearOutputDestination default, the same word for every input and every DSP.
 */
#define STRIP_ROUTE_NONE        0x0001FFFFu
#define STRIP_SPAN_NONE         0x20u           /* not linked */
#define STRIP_PLUGIN_MIC        0x846u          /* pluginId of the Mic Input Strip */
#define STRIP_OP(op, len)       (((ULONG)(op) << 16) | (ULONG)(len))
#define STRIP_OP_WRITE          0x14u
#define STRIP_OP_MEMSPEC        0x15u

/*
 * StripSteps (registry): each stage adds exactly one new kind of command to what the last
 * stage sent, so a hardware run can be bisected (open-apollo input_strips_steps, extended).
 * Stages 2-5 go into each input's PREPARE transaction, 6-7 into the pass's START transaction.
 */
#define STRIP_STEP_UPLOADS      1   /* the five code modules (T3) */
#define STRIP_STEP_ZERO         2   /* + 10 zero-fills of the strip's private blocks */
#define STRIP_STEP_MEMSPEC      3   /* + the memspec link table (DMA) */
#define STRIP_STEP_HEADER       4   /* + the instance header (DMA) */
#define STRIP_STEP_PARAMS       5   /* + the captured 48 kHz parameters (DMA) */
#define STRIP_STEP_SYNCH        6   /* + SYNCH: the strips start (all of them, after every PREPARE) */
#define STRIP_STEP_INPUT        7   /* + span (twice) and route words on the strip DSP */
#define STRIP_STEP_OTHERS       8   /* + bus owner, route words to every other DSP (driver side) */
#define STRIP_STEP_PHANTOM      9   /* + the 48V off/on resync (driver side) */
#define STRIP_STEP_ALL          9   /* StripSteps 0 */

typedef struct _STRIP_ROW {
    ULONG   Pool;           /* 1-3 */
    ULONG   Dwords;
    ULONG   Link;           /* (row << 24) | word, 0xFFFFFFFF = chain head */
} STRIP_ROW;

/* Row sizes (dwords) and what one strip takes per pool: strips.c's layout check is built on
   these too (C_ASSERTs there), so the check and the planner cannot drift apart. */
#define STRIP_DW_HDR            0x008u
#define STRIP_DW_M0             0x038u
#define STRIP_DW_M1             0x010u
#define STRIP_DW_M2             0x010u
#define STRIP_DW_M3             0x010u
#define STRIP_DW_M4             0x004u
#define STRIP_DW_C              0x800u      /* c0-c3, the delay buffers */
#define STRIP_POOL1_PER_STRIP   (STRIP_DW_HDR + STRIP_DW_M0 + STRIP_DW_M2 + STRIP_DW_M4)   /* 0x54 */
#define STRIP_POOL2_PER_STRIP   (STRIP_DW_M1)                                              /* 0x10 */
#define STRIP_POOL3_PER_STRIP   (STRIP_DW_M3 + 4 * STRIP_DW_C)                             /* 0x2010 */

/* Pool tops the recipe was validated on; the layout check proved the x8p's DSP 1 has them. */
#define STRIP_TOP1              0x0009D1B8u
#define STRIP_TOP2              0x000C6FFEu
#define STRIP_TOP3              0x08FFE000u

/* ua_strips.c:372-383: descriptor 0 = row 0 (head), descriptor 1 = rows 1-9 (Mic sub-plugin) */
static const STRIP_ROW g_StripRows[STRIP_PLAN_ROWS] = {
    { 1, STRIP_DW_HDR, 0xFFFFFFFFu },   /* instance header */
    { 1, STRIP_DW_M0,  0x00000006u },   /* m0 -> header[6] */
    { 2, STRIP_DW_M1,  0x00000036u },   /* m1 -> m0[0x36] */
    { 1, STRIP_DW_M2,  0x00000037u },   /* m2 -> m0[0x37] */
    { 3, STRIP_DW_M3,  0x0000001Bu },   /* m3 -> m0[0x1b] */
    { 1, STRIP_DW_M4,  0x0000001Cu },   /* m4 -> m0[0x1c] */
    { 3, STRIP_DW_C,   0x0000000Eu },   /* c0 -> m0[0x0e] */
    { 3, STRIP_DW_C,   0x0000000Fu },   /* c1 -> m0[0x0f] */
    { 3, STRIP_DW_C,   0x00000006u },   /* c2 -> m0[0x06] */
    { 3, STRIP_DW_C,   0x00000007u },   /* c3 -> m0[0x07] */
};

static const ULONG g_StripTop[4] = { 0, STRIP_TOP1, STRIP_TOP2, STRIP_TOP3 };

/* UA's captured 48 kHz layout (ua_strips.c:538-552): m0[0..17], m0[38..53], m3[1..15] */
static const ULONG g_StripM0A[18] = {
    0x0f1f7fe4, 0x40000000, 0x00000001, 0x00000001, 0x00000004, 0x00000100,
    0x000000c8, 0x3a888889, 0xc0000000, 0xc0000000, 0x00000010, 0x0000004b,
    0x41000000, 0x3ada740e, 0xffffffff, 0x00000001, 0xffffffff, 0x00000001,
};
static const ULONG g_StripM0B[16] = {
    0x00000040, 0x3cae6c74, 0xbf7ff125, 0x3f7ff892, 0x3909422f, 0x3f800000,
    0xbc2e6fd3, 0xbbc96245, 0x3cae6c74, 0xbf7ff125, 0x3f7ff892, 0x3909422f,
    0x3f800000, 0xbc2e6fd3, 0xbbc96245, 0x00000001,
};
static const ULONG g_StripM3W[15] = {
    0x00000040, 0x3cae6c74, 0xbf7ff125, 0x3f7ff892, 0x3909422f, 0x3f800000,
    0xbc2e6fd3, 0xbbc96245, 0x3e3b3f3d, 0xbf7baf04, 0x3f7dd5f1, 0x3c20d8d2,
    0x3f7ffcd7, 0xbdbc4dc9, 0xbd565d96,
};

/*
 * [Rate] The Mic strip's words that depend on the sample rate (oa:driver/ua_strips.c:277-341 tables,
 * :641-672 their use with the captured layout, input_strips_params=1). The ramp: k = SR / BS * 0.05 +
 * 0.5 blocks, f = BS / 4.0f, 1.0f / (f * k), block size BS 32 below 100 kHz and 64 above; the DC
 * blocker and the 75 Hz LowCut come from the SDK tables for three bands (< 50 kHz, 50-100, > 100).
 * At 48 kHz the captured words verbatim (above, unchanged); at any other rate the same three writes
 * with that rate's k, f, 1/(f*k) in m0[11..13], the band's DC taps in m0[39..45] and [46..52], and
 * the band's DC and LowCut taps in m3[2..15]. Verified on hardware at 48 kHz only (open-apollo's
 * other rates were "not yet verified on hardware" either): docs/STRIPS.md 11.
 */
static const ULONG g_StripRateDc[3][8] = {
    { 0x00000000, 0x3cb55044, 0xbf7fefd0, 0x3f7ff7f0, 0x391565e1, 0x3f7fffef, 0xbc3573eb, 0xbbd23ba2 },
    { 0x00000000, 0x3c76af8a, 0xbf7ff898, 0x3f7ffc44, 0x38893a86, 0x3f7ffffc, 0xbbf6b20e, 0xbb8e6863 },
    { 0x00000000, 0x3c2e7056, 0xbf7ffc44, 0x3f7ffe2a, 0x38092b1e, 0x3f7fffff, 0xbbae7117, 0xbb498cfc },
};
static const ULONG g_StripRateLowcut[3][8] = {
    { 0x00000000, 0x3e3b3f3d, 0xbf7baf04, 0x3f7dd5f1, 0x3c20d8d2, 0x3f7ffcd7, 0xbdbc4dc9, 0xbd565d96 },
    { 0x00000000, 0x3e04f704, 0xbf7dd529, 0x3f7eea30, 0x3ba0d951, 0x3f7fff36, 0xbd855748, 0xbd18e14f },
    { 0x00000000, 0x3daa574a, 0xbf7f1ceb, 0x3f7f8e5c, 0x3b20d96c, 0x3f7fffcd, 0xbd2aa2e7, 0xbcf07ac9 },
};
typedef struct _STRIP_RAMP {
    ULONG   Rate, K, F, Inv;
} STRIP_RAMP;
static const STRIP_RAMP g_StripRamp[6] = {
    {  44100,  69, 0x41000000, 0x3aed7304 },     /* BS 32: 1/552 */
    {  48000,  75, 0x41000000, 0x3ada740e },     /* 1/600 (= the captured m0[11..13]) */
    {  88200, 138, 0x41000000, 0x3a6d7304 },     /* 1/1104 */
    {  96000, 150, 0x41000000, 0x3a5a740e },     /* 1/1200 */
    { 176400, 138, 0x41800000, 0x39ed7304 },     /* BS 64: 1/2208 */
    { 192000, 150, 0x41800000, 0x39da740e },     /* 1/2400 */
};

/* The ramp row of Rate (Hz), 0 for a rate the Apollo does not run at. */
static __inline const STRIP_RAMP *StripRampOf(ULONG Rate)
{
    ULONG i;

    for (i = 0; i < 6; i++) {
        if (g_StripRamp[i].Rate == Rate) {
            return &g_StripRamp[i];
        }
    }
    return 0;
}

/* The SDK table band of Rate: 0 below 50 kHz, 1 up to 100 kHz, 2 above (ua_strip_rate_lookup). */
static __inline ULONG StripRateBand(ULONG Rate)
{
    return Rate < 50000 ? 0 : (Rate <= 100000 ? 1 : 2);
}

/*
 * The three parameter writes' words at Rate (stage 13): m0[0..17] (18), m0[38..53] (16), m3[1..15] (15).
 * Returns 0, or 1 for a rate without strip words (nothing written).
 */
static __inline ULONG StripRateWords(ULONG Rate, ULONG M0a[18], ULONG M0b[16], ULONG M3[15])
{
    const STRIP_RAMP *r = StripRampOf(Rate);
    ULONG j, band;

    if (r == 0) {
        return 1;
    }
    for (j = 0; j < 18; j++) {
        M0a[j] = g_StripM0A[j];
    }
    for (j = 0; j < 16; j++) {
        M0b[j] = g_StripM0B[j];
    }
    for (j = 0; j < 15; j++) {
        M3[j] = g_StripM3W[j];
    }
    if (Rate == 48000) {
        return 0;                                   /* the captured words verbatim */
    }
    band = StripRateBand(Rate);
    M0a[11] = r->K;
    M0a[12] = r->F;
    M0a[13] = r->Inv;
    for (j = 1; j < 15; j++) {
        M0b[j] = g_StripRateDc[band][j < 8 ? j : j - 7];   /* [0] and [15] keep the count and the 1 */
    }
    for (j = 1; j < 15; j++) {
        M3[j] = j < 8 ? g_StripRateDc[band][j] : g_StripRateLowcut[band][j - 7];
    }
    return 0;
}

typedef struct _STRIP_PLAN_ENT {
    ULONG   Dma;            /* 0 = inline W[0..3]; 1 = DMA_REF of Dwords at payload dword Off */
    ULONG   W[4];
    ULONG   Off;
    ULONG   Dwords;
} STRIP_PLAN_ENT;

typedef struct _STRIP_PLAN {
    ULONG           Addr[STRIP_PLAN_ROWS];      /* [Virtual] an Input Strip uses rows 0-5 (the rest 0) */
    ULONG           N;
    STRIP_PLAN_ENT  Ent[STRIP_PLAN_ENT_SLOTS];  /* a Mic-only plan never has more than STRIP_PLAN_ENTS_MAX */
    ULONG           Len;                        /* payload dwords used */
    ULONG           Payload[STRIP_PLAN_PAYLOAD_MAX];
    ULONG           Err;
} STRIP_PLAN;

/*
 * Slot = input index (docs/STRIPS.md D6): each pool hands out its blocks top-down, slot after
 * slot (ua_strips.c:503-520). Slot 0 = the spec 6.4 addresses; slot k is 0x54 / 0x10 / 0x2010
 * dwords lower in pools 1 / 2 / 3.
 */
static __inline void StripPlanAddrs(ULONG Input, ULONG Addr[STRIP_PLAN_ROWS])
{
    ULONG cur[4], s, j;

    cur[0] = 0;
    cur[1] = g_StripTop[1];
    cur[2] = g_StripTop[2];
    cur[3] = g_StripTop[3];
    for (j = 0; j < STRIP_PLAN_ROWS; j++) {
        Addr[j] = 0;
    }
    for (s = 0; s <= Input && s < STRIP_PLAN_INPUTS; s++) {
        for (j = 0; j < STRIP_PLAN_ROWS; j++) {
            ULONG p = g_StripRows[j].Pool & 3;

            cur[p] -= g_StripRows[j].Dwords;
            Addr[j] = cur[p];
        }
    }
}

/*
 * The memspec (ua_memspec_compile with descriptors {0, 1}) of N rows: row 0 has no link; row 1 (the
 * sub-plugin's first row) links into the head's rows, rows 2..N-1 into the sub-plugin's own rows.
 * Each pair stores a block's address at word `offset` of its target row. 2N-1 words; 0 on error.
 * [Virtual] Rows = g_StripRows (the Mic strip, 19 words) or g_StripVRows (the Input Strip, 11).
 */
static __inline ULONG StripPlanMemspecOf(const STRIP_ROW *Rows, ULONG N, const ULONG *Addr, ULONG *Words)
{
    ULONG m = 0, j;

    if (N < 2) {
        return 0;
    }
    Words[m++] = STRIP_OP(STRIP_OP_MEMSPEC, 2 * N - 1);
    for (j = 1; j < N; j++) {
        ULONG tgtFirst = (j == 1) ? 0 : 1;
        ULONG tgtCount = (j == 1) ? 1 : N - 1;
        ULONG t = Rows[j].Link >> 24, off = Rows[j].Link & 0xFFFFFF;

        if (t >= tgtCount || off >= Rows[tgtFirst + t].Dwords || (Addr[j] & 1)) {
            return 0;
        }
        Words[m++] = Addr[tgtFirst + t] + off;
        Words[m++] = Addr[j];
    }
    return m;
}

/* The Mic strip's memspec: 19 words; 0 on error. */
static __inline ULONG StripPlanMemspec(const ULONG Addr[STRIP_PLAN_ROWS],
                                       ULONG Words[2 * STRIP_PLAN_ROWS - 1])
{
    return StripPlanMemspecOf(g_StripRows, STRIP_PLAN_ROWS, Addr, Words);
}

static __inline void StripPlanInline(STRIP_PLAN *P, ULONG W0, ULONG W1, ULONG W2, ULONG W3)
{
    STRIP_PLAN_ENT *e;

    if (P->Err || P->N >= STRIP_PLAN_ENT_SLOTS || (W0 & 0x80000000u)) {
        P->Err = 1;
        return;
    }
    e = &P->Ent[P->N++];
    e->Dma = 0;
    e->W[0] = W0;
    e->W[1] = W1;
    e->W[2] = W2;
    e->W[3] = W3;
    e->Off = 0;
    e->Dwords = 0;
}

/* One DMA-carried command of Dwords, packed after the previous one (as ua_xact_cmd does). */
static __inline ULONG *StripPlanDma(STRIP_PLAN *P, ULONG Dwords)
{
    STRIP_PLAN_ENT *e;
    ULONG *w;

    if (P->Err || P->N >= STRIP_PLAN_ENT_SLOTS || Dwords == 0 ||
        Dwords > STRIP_PLAN_PAYLOAD_MAX - P->Len) {
        P->Err = 1;
        return 0;
    }
    e = &P->Ent[P->N++];
    e->Dma = 1;
    e->W[0] = e->W[1] = e->W[2] = e->W[3] = 0;
    e->Off = P->Len;
    e->Dwords = Dwords;
    w = &P->Payload[P->Len];
    P->Len += Dwords;
    return w;
}

/* StripSteps -> the last stage a pass goes to (0 or out of range = everything). */
static __inline ULONG StripPlanSteps(ULONG Steps)
{
    return (Steps == 0 || Steps > STRIP_STEP_ALL) ? STRIP_STEP_ALL : Steps;
}

static __inline void StripPlanReset(STRIP_PLAN *P)
{
    ULONG j;

    for (j = 0; j < STRIP_PLAN_ROWS; j++) {
        P->Addr[j] = 0;
    }
    P->N = 0;
    P->Len = 0;
    P->Err = 0;
}

/*
 * The PREPARE transaction for preamp Input (0-based; DSP input = Input) up to stage Steps
 * (STRIP_STEP_*; 0 = all): stages 2-5 only, the strip's memory. It never starts anything: no
 * SYNCH and no input word. Below STRIP_STEP_ZERO it is empty. Returns 0, or nonzero on a
 * planner error.
 */
static __inline ULONG StripPlanPrepareRate(ULONG Input, ULONG Steps, ULONG Rate, STRIP_PLAN *P);

/* At 48 kHz: the plan every release built (strip-plan-test's golden hashes). */
static __inline ULONG StripPlanPrepare(ULONG Input, ULONG Steps, STRIP_PLAN *P)
{
    return StripPlanPrepareRate(Input, Steps, 48000, P);
}

/* [Rate] The same at Rate (Hz): stage 13 carries that rate's words (StripRateWords). */
static __inline ULONG StripPlanPrepareRate(ULONG Input, ULONG Steps, ULONG Rate, STRIP_PLAN *P)
{
    const ULONG *a = P->Addr;
    ULONG j, *w;
    ULONG m0a[18], m0b[16], m3[15];

    StripPlanReset(P);
    if (Input >= STRIP_PLAN_INPUTS || StripRateWords(Rate, m0a, m0b, m3) != 0) {
        P->Err = 1;
        return P->Err;
    }
    Steps = StripPlanSteps(Steps);
    StripPlanAddrs(Input, P->Addr);
    if (Steps < STRIP_STEP_ZERO) {
        return 0;
    }

    /* 1-10: zero-fill the instance header and every private block */
    for (j = 0; j < STRIP_PLAN_ROWS; j++) {
        StripPlanInline(P, STRIP_CMD_ZERO, a[j], 0, g_StripRows[j].Dwords);
    }
    if (Steps < STRIP_STEP_MEMSPEC) {
        return P->Err;
    }

    /* 11: the memspec, 19 words */
    w = StripPlanDma(P, 2 * STRIP_PLAN_ROWS - 1);
    if (w != 0 && StripPlanMemspec(a, w) != 2 * STRIP_PLAN_ROWS - 1) {
        P->Err = 1;
    }
    if (Steps < STRIP_STEP_HEADER) {
        return P->Err;
    }

    /* 12: the main plugin's header: in 1, out 1, sub-plugin 0x846 (ua_strips.c:555-563) */
    w = StripPlanDma(P, 9);
    if (w != 0) {
        w[0] = STRIP_OP(STRIP_OP_WRITE, 9);
        w[1] = a[0];
        w[2] = 0x24;
        w[3] = 0x40000000u;
        w[4] = 1;
        w[5] = 1;
        w[6] = 0x22;
        w[7] = 1;
        w[8] = STRIP_PLUGIN_MIC;
    }
    if (Steps < STRIP_STEP_PARAMS) {
        return P->Err;
    }

    /* 13: the captured 48 kHz parameters ([Rate] or the rate's), three writes in one DMA command (ua_strips.c:639-672) */
    w = StripPlanDma(P, (2 + 18) + (2 + 16) + (2 + 15));
    if (w != 0) {
        ULONG n = 0;

        w[n++] = STRIP_OP(STRIP_OP_WRITE, 2 + 18);
        w[n++] = a[1];
        for (j = 0; j < 18; j++) {
            w[n++] = m0a[j];
        }
        w[n++] = STRIP_OP(STRIP_OP_WRITE, 2 + 16);
        w[n++] = a[1] + 38;
        for (j = 0; j < 16; j++) {
            w[n++] = m0b[j];
        }
        w[n++] = STRIP_OP(STRIP_OP_WRITE, 2 + 15);
        w[n++] = a[4] + 1;
        for (j = 0; j < 15; j++) {
            w[n++] = m3[j];
        }
    }
    return P->Err;
}

/*
 * The START transaction for every preamp in Mask (input 1 first), up to stage Steps: per
 * input, SYNCH with its header (stage 6), then span none twice and the route word (stage 7;
 * ua_strips.c:589-599). Inline entries only. Empty below STRIP_STEP_SYNCH. P->Addr is not
 * used. Returns 0, or nonzero on a planner error.
 */
static __inline ULONG StripPlanStart(ULONG Mask, ULONG Steps, STRIP_PLAN *P)
{
    ULONG addr[STRIP_PLAN_ROWS], i;

    StripPlanReset(P);
    Steps = StripPlanSteps(Steps);
    if (Mask & ~((1u << STRIP_PLAN_INPUTS) - 1)) {
        P->Err = 1;
        return P->Err;
    }
    if (Steps < STRIP_STEP_SYNCH) {
        return 0;
    }
    for (i = 0; i < STRIP_PLAN_INPUTS; i++) {
        if (!(Mask & (1u << i))) {
            continue;
        }
        StripPlanAddrs(i, addr);
        StripPlanInline(P, STRIP_CMD_SYNCH, (STRIP_SYNCH_MONO << 16) | i, addr[0], 0);
        if (Steps >= STRIP_STEP_INPUT) {
            StripPlanInline(P, STRIP_CMD_INPUT, i, STRIP_INPUT_SPAN, STRIP_SPAN_NONE);
            StripPlanInline(P, STRIP_CMD_INPUT, i, STRIP_INPUT_SPAN, STRIP_SPAN_NONE);
            StripPlanInline(P, STRIP_CMD_INPUT, i, STRIP_INPUT_ROUTE, STRIP_ROUTE_NONE);
        }
    }
    return P->Err;
}

/*
 * The pass's transactions on the strip DSP, in order: transaction K (0, 1, ...) of a pass over
 * Mask up to stage Steps. STRIP_XACT_PREPARE (*Input = the preamp) for every input in Mask,
 * ascending; then one STRIP_XACT_START when the pass goes to SYNCH or beyond; then
 * STRIP_XACT_END. strips.c walks exactly this list, and strip-plan-test checks it.
 */
#define STRIP_XACT_END          0u
#define STRIP_XACT_PREPARE      1u
#define STRIP_XACT_START        2u

static __inline ULONG StripPassStep(ULONG Mask, ULONG Steps, ULONG K, ULONG *Input)
{
    ULONG i, n = 0;

    *Input = 0;
    Steps = StripPlanSteps(Steps);
    Mask &= (1u << STRIP_PLAN_INPUTS) - 1;
    if (Steps < STRIP_STEP_ZERO || Mask == 0) {
        return STRIP_XACT_END;
    }
    for (i = 0; i < STRIP_PLAN_INPUTS; i++) {
        if (!(Mask & (1u << i))) {
            continue;
        }
        if (n == K) {
            *Input = i;
            return STRIP_XACT_PREPARE;
        }
        n++;
    }
    return (K == n && Steps >= STRIP_STEP_SYNCH) ? STRIP_XACT_START : STRIP_XACT_END;
}

/*
 * ---- [Virtual] Virtual Input Strips (docs/STRIPS.md 13) ----
 *
 * A DAW (or Windows) plays into VIRTUAL 1-8; on the DSP those are inputs (synch) 0x18-0x1F, and "without a
 * strip on their DSP input the Console's Virtual channels mix nothing and record silence"
 * (oa:driver/ua_strips.c:19-25). Their strip is the plain Input Strip, pluginId 0x83E, run by the Mic
 * strip's A5 and C2 modules (resident already: the same five uploads), with six private blocks.
 * Port of oa:driver/ua_strips.c:385-412 (ua_strip_input_rows, ua_strip_input_m0), 713-745
 * (ua_strip_build_input), 990-1009 (the planner: the Virtual strips after every Mic strip), 766-789
 * (first_virtual 0x18 on every model with strips).
 *
 * Placement (this driver's fixed layout, docs/STRIPS.md D6): Mic input i keeps slot i of the room the model
 * books for its Mic strips (LayoutStrips: x8p 8, Twin X 2); VIRTUAL n sits in Virtual slot n-1 directly
 * below that room, top-down per pool, so the strip DSP's ledger (plugin.c) books one contiguous block per
 * pool. With every Mic strip of the model built, that is open-apollo's packing word for word (it puts the
 * Input Strips after every Mic strip, ua_strip_priv_addrs).
 *
 * A pass is ONE ordered list of "slots": 0-7 the Mic strips (preamp i), 8-15 the Input Strips (VIRTUAL
 * slot-7). Pm, the plan mask, has bit k for slot k (= StripMask | StripVirtualMask << 8): every PREPARE
 * (Mic first, then Virtual), then ONE START (SYNCH, span, span, route for each, in slot order). No strip
 * runs while any strip's memory is written (docs/STRIPS.md 10.8). A Pm of 0..0xFF gives exactly the
 * Mic-only functions' words (strip-plan-test).
 */
#define STRIP_PLAN_VIRTUALS     8u      /* VIRTUAL 1-8 */
#define STRIP_PLAN_VROWS        6u      /* Input Strip: instance header, m0, c0-c3 */
#define STRIP_PLAN_SLOTS        20u     /* a pass's strips: 0-7 Mic, 8-15 Virtual, [AuxStrips] 16-19 the aux returns */
#define STRIP_SLOT_VIRTUAL0     8u      /* slot of VIRTUAL 1 */
#define STRIP_VIRTUAL_SYNCH0    0x18u   /* DSP input (synch) of VIRTUAL 1 = its mixer bus; every model's FirstVirtual */
#define STRIP_PLAN_VPREPARE_ENTS 9u     /* 6 zero-fills + 3 DMA refs */
#define STRIP_PLUGIN_INPUT      0x83Eu  /* pluginId of the Input Strip */
#define STRIP_DW_VM0            0x01Eu
#define STRIP_DW_VC             0x100u  /* the captured commons: the capacity m0 word 5 selects */
#define STRIP_VM0_WORDS         9u
#define STRIP_VPOOL1_PER_STRIP  (STRIP_DW_HDR + STRIP_DW_VM0)  /* 0x26 */
#define STRIP_VPOOL2_PER_STRIP  0u
#define STRIP_VPOOL3_PER_STRIP  (4u * STRIP_DW_VC)              /* 0x400 */
#define STRIP_PM_MIC            0x00FFu
#define STRIP_PM_VIRTUAL        0xFF00u

/* ua_strips.c:394-412: descriptor 0 = row 0 (head), descriptor 1 = rows 1-5 (the Input sub-plugin) */
static const STRIP_ROW g_StripVRows[STRIP_PLAN_VROWS] = {
    { 1, STRIP_DW_HDR, 0xFFFFFFFFu },   /* instance header */
    { 1, STRIP_DW_VM0, 0x00000006u },   /* m0 -> header[6] */
    { 3, STRIP_DW_VC,  0x0000000Eu },   /* c0 -> m0[0x0e] */
    { 3, STRIP_DW_VC,  0x0000000Fu },   /* c1 -> m0[0x0f] */
    { 3, STRIP_DW_VC,  0x00000006u },   /* c2 -> m0[0x06] */
    { 3, STRIP_DW_VC,  0x00000007u },   /* c3 -> m0[0x07] */
};

/*
 * ua_strips.c:405-412: the captured Input Strip parameters (oa:tools/uausb/solo-usb-init.json params.input),
 * written as one {0x0014000B, m0, ...} command: word 0 is the WRITE's skip mask (the values land on m0 words
 * 0, 1, 2, 4, 5, 16, 22, 25, never on the link words 6, 7, 0x0e, 0x0f), the last value 0.004f. 48 kHz values;
 * upstream writes them at every rate (no rate rule is known), and so does this port.
 */
static const ULONG g_StripVM0[STRIP_VM0_WORDS] = {
    0x00df7fe4, 0x40000000, 0x00000001, 0x00000001, 0x00000004, 0x00000100,
    0x000000c8, 0x3a888889, 0x3b83126f,
};

static __inline ULONG StripPlanPm(ULONG Mask, ULONG VMask)
{
    return (Mask & STRIP_PM_MIC) | ((VMask & 0xFFu) << 8);
}

/*
 * ---- [AuxStrips] The aux returns' Input Strips (docs/STRIPS.md 14) ----
 *
 * The aux returns are DSP inputs 0x10-0x13 (AUX 1 L, AUX 1 R, AUX 2 L, AUX 2 R; oa:driver/ua_apollo.h:816-817
 * UA_BUS_AUX1_RETURN 0x10 / AUX2_RETURN 0x12, the R inputs 0x11 / 0x13; plugin_plan.h PlugAuxSynch), on the x8p and the
 * Twin X alike. Like a Virtual input they mix nothing and record silence without an Input Strip: measured on the x8p
 * Nov 16 2026, the plain Input Strip (0x83E) on 0x10 carried VIRTUAL 1's tone into the AUX 1 L record slot (28) at the
 * send's level, while 0x11 without one stayed exact zero. The same strip, the same words (StripPlanPrepareInput), on
 * slots 16-19 (Pm bits 16-19 = StripAuxMask bits 0-3).
 *
 * Placement: the Input Strips are one area below the model's Mic room, Input Strip n (0-based) n places down: VIRTUAL
 * 1-8 are n = 0-7 (as before), the aux return k (0 = 0x10) is n = 8 + k, right below VIRTUAL 8's fixed place. So no
 * Virtual strip moves when aux strips come or go, every slot keeps one fixed address per model, and the plug-in ledger
 * still books one block per pool from the top (StripPlanRoomAux). x8p, 8 Mic strips: AUX 1 L's header 0x9cde0.
 */
#define STRIP_PLAN_AUXES        4u      /* DSP inputs 0x10-0x13 */
#define STRIP_SLOT_AUX0         16u     /* slot of the AUX 1 L return (DSP input 0x10) */
#define STRIP_AUX_SYNCH0        0x10u   /* DSP input (synch) of AUX 1 L = its bus (UA_BUS_AUX1_RETURN) */
#define STRIP_PM_AUX            0x000F0000u
#define STRIP_PLAN_INPUT_STRIPS (STRIP_PLAN_VIRTUALS + STRIP_PLAN_AUXES)    /* 12 Input Strip places */

/* [AuxStrips] The plan mask with the aux-return strips: StripMask | StripVirtualMask << 8 | StripAuxMask << 16. */
static __inline ULONG StripPlanPmAux(ULONG Mask, ULONG VMask, ULONG AMask)
{
    return StripPlanPm(Mask, VMask) | ((AMask & 0xFu) << 16);
}

/* The DSP input (= the mixer bus) of slot Slot: preamp i -> i, VIRTUAL n -> 0x18 + n - 1, [AuxStrips] aux k -> 0x10 + k. */
static __inline ULONG StripSlotSynch(ULONG Slot)
{
    if (Slot < STRIP_SLOT_VIRTUAL0) {
        return Slot;
    }
    if (Slot < STRIP_SLOT_AUX0) {
        return STRIP_VIRTUAL_SYNCH0 + ((Slot - STRIP_SLOT_VIRTUAL0) & 7u);
    }
    return STRIP_AUX_SYNCH0 + ((Slot - STRIP_SLOT_AUX0) & 3u);
}

/* The slot whose strip mixes bus Bus (DSP input), or STRIP_PLAN_SLOTS: none. */
static __inline ULONG StripBusSlot(ULONG Bus)
{
    if (Bus < STRIP_PLAN_INPUTS) {
        return Bus;
    }
    if (Bus >= STRIP_VIRTUAL_SYNCH0 && Bus < STRIP_VIRTUAL_SYNCH0 + STRIP_PLAN_VIRTUALS) {
        return STRIP_SLOT_VIRTUAL0 + (Bus - STRIP_VIRTUAL_SYNCH0);
    }
    if (Bus >= STRIP_AUX_SYNCH0 && Bus < STRIP_AUX_SYNCH0 + STRIP_PLAN_AUXES) {
        return STRIP_SLOT_AUX0 + (Bus - STRIP_AUX_SYNCH0);      /* [AuxStrips] */
    }
    return STRIP_PLAN_SLOTS;
}

/*
 * Input Strip N's six private addresses (Addr[6..9] = 0): below the room of MicSlots Mic strips, place after place,
 * top-down in each pool (N 0-7 = VIRTUAL N+1, [AuxStrips] 8-11 = the aux return N-8). 0, or 1 for N or MicSlots out of
 * range (Addr all 0).
 */
static __inline ULONG StripPlanInputAddrs(ULONG MicSlots, ULONG N, ULONG Addr[STRIP_PLAN_ROWS])
{
    ULONG cur[4], s, j;

    for (j = 0; j < STRIP_PLAN_ROWS; j++) {
        Addr[j] = 0;
    }
    if (N >= STRIP_PLAN_INPUT_STRIPS || MicSlots > STRIP_PLAN_INPUTS) {
        return 1;
    }
    cur[0] = 0;
    cur[1] = g_StripTop[1] - MicSlots * STRIP_POOL1_PER_STRIP;
    cur[2] = g_StripTop[2] - MicSlots * STRIP_POOL2_PER_STRIP;
    cur[3] = g_StripTop[3] - MicSlots * STRIP_POOL3_PER_STRIP;
    for (s = 0; s <= N; s++) {
        for (j = 0; j < STRIP_PLAN_VROWS; j++) {
            ULONG p = g_StripVRows[j].Pool & 3;

            cur[p] -= g_StripVRows[j].Dwords;
            Addr[j] = cur[p];
        }
    }
    return 0;
}

/*
 * VIRTUAL V+1's six private addresses (Addr[6..9] = 0): below the room of MicSlots Mic strips, slot after
 * slot, top-down in each pool. 0, or 1 for V or MicSlots out of range (Addr all 0).
 */
static __inline ULONG StripPlanVirtualAddrs(ULONG MicSlots, ULONG V, ULONG Addr[STRIP_PLAN_ROWS])
{
    if (V >= STRIP_PLAN_VIRTUALS) {
        return StripPlanInputAddrs(STRIP_PLAN_INPUTS + 1, 0, Addr);     /* out of range: Addr all 0, 1 */
    }
    return StripPlanInputAddrs(MicSlots, V, Addr);
}

/* [AuxStrips] The aux return A's six private addresses (A 0-3 = DSP input 0x10 + A): Input Strip 8 + A. 0, or 1. */
static __inline ULONG StripPlanAuxAddrs(ULONG MicSlots, ULONG A, ULONG Addr[STRIP_PLAN_ROWS])
{
    if (A >= STRIP_PLAN_AUXES) {
        return StripPlanInputAddrs(STRIP_PLAN_INPUTS + 1, 0, Addr);     /* out of range: Addr all 0, 1 */
    }
    return StripPlanInputAddrs(MicSlots, STRIP_PLAN_VIRTUALS + A, Addr);
}

/* Slot Slot's private addresses (Mic rows 0-9, or the Input Strip's 0-5). 0, or 1 for a slot out of range. */
static __inline ULONG StripPlanSlotAddrs(ULONG Slot, ULONG MicSlots, ULONG Addr[STRIP_PLAN_ROWS])
{
    ULONG j;

    if (Slot < STRIP_SLOT_VIRTUAL0) {
        StripPlanAddrs(Slot, Addr);
        return 0;
    }
    if (Slot < STRIP_PLAN_SLOTS) {
        return StripPlanInputAddrs(MicSlots, Slot - STRIP_SLOT_VIRTUAL0, Addr);    /* Virtual, then the aux returns */
    }
    for (j = 0; j < STRIP_PLAN_ROWS; j++) {
        Addr[j] = 0;
    }
    return 1;
}

/*
 * The PREPARE transaction for Input Strip N (N 0-7 = VIRTUAL N+1, [AuxStrips] 8-11 = the aux return N-8;
 * ua_strip_build_input's stages 2-5, nothing started): zero-fills of its six blocks (stage 2), the 11-word memspec (3),
 * the header {0x00140009, hdr, 0x24, 0x40000000, 1, 1, 0x22, 1, 0x83E} (4), the m0 parameters {0x0014000B, m0,
 * g_StripVM0} (5). Each DMA command is its own DMA_REF, as ua_strip_dma packs them. The words depend on the strip's
 * addresses only, never on its DSP input. Returns 0, or nonzero on a planner error.
 */
static __inline ULONG StripPlanPrepareInput(ULONG N, ULONG MicSlots, ULONG Steps, STRIP_PLAN *P)
{
    const ULONG *a = P->Addr;
    ULONG j, *w;

    StripPlanReset(P);
    if (StripPlanInputAddrs(MicSlots, N, P->Addr) != 0) {
        P->Err = 1;
        return P->Err;
    }
    Steps = StripPlanSteps(Steps);
    if (Steps < STRIP_STEP_ZERO) {
        return 0;
    }
    for (j = 0; j < STRIP_PLAN_VROWS; j++) {
        StripPlanInline(P, STRIP_CMD_ZERO, a[j], 0, g_StripVRows[j].Dwords);
    }
    if (Steps < STRIP_STEP_MEMSPEC) {
        return P->Err;
    }
    w = StripPlanDma(P, 2 * STRIP_PLAN_VROWS - 1);
    if (w != 0 && StripPlanMemspecOf(g_StripVRows, STRIP_PLAN_VROWS, a, w) != 2 * STRIP_PLAN_VROWS - 1) {
        P->Err = 1;
    }
    if (Steps < STRIP_STEP_HEADER) {
        return P->Err;
    }
    /* ua_strip_build_header(x, addr[0], UA_STRIP_PLUGIN_INPUT) */
    w = StripPlanDma(P, 9);
    if (w != 0) {
        w[0] = STRIP_OP(STRIP_OP_WRITE, 9);
        w[1] = a[0];
        w[2] = 0x24;
        w[3] = 0x40000000u;
        w[4] = 1;
        w[5] = 1;
        w[6] = 0x22;
        w[7] = 1;
        w[8] = STRIP_PLUGIN_INPUT;
    }
    if (Steps < STRIP_STEP_PARAMS) {
        return P->Err;
    }
    /* ua_strip_build_block(x, addr[1], ua_strip_input_m0, 9) */
    w = StripPlanDma(P, 2 + STRIP_VM0_WORDS);
    if (w != 0) {
        w[0] = STRIP_OP(STRIP_OP_WRITE, 2 + STRIP_VM0_WORDS);
        w[1] = a[1];
        for (j = 0; j < STRIP_VM0_WORDS; j++) {
            w[2 + j] = g_StripVM0[j];
        }
    }
    return P->Err;
}

/* The PREPARE transaction for VIRTUAL V+1 (Input Strip V): StripPlanPrepareInput. Returns 0, or nonzero on a planner error. */
static __inline ULONG StripPlanPrepareVirtual(ULONG V, ULONG MicSlots, ULONG Steps, STRIP_PLAN *P)
{
    if (V >= STRIP_PLAN_VIRTUALS) {
        StripPlanReset(P);
        P->Err = 1;
        return P->Err;
    }
    return StripPlanPrepareInput(V, MicSlots, Steps, P);
}

/*
 * Slot Slot's PREPARE at Rate (the Mic strip's rate words; the Input Strip's are the same at every rate). [AuxStrips]
 * Slots 8-19 are Input Strips 0-11: VIRTUAL 1-8, then the aux returns 0x10-0x13.
 */
static __inline ULONG StripPlanPrepareSlot(ULONG Slot, ULONG MicSlots, ULONG Steps, ULONG Rate, STRIP_PLAN *P)
{
    if (Slot < STRIP_SLOT_VIRTUAL0) {
        return StripPlanPrepareRate(Slot, Steps, Rate, P);
    }
    if (Slot >= STRIP_PLAN_SLOTS || StripRampOf(Rate) == 0) {
        StripPlanReset(P);
        P->Err = 1;
        return P->Err;
    }
    return StripPlanPrepareInput(Slot - STRIP_SLOT_VIRTUAL0, MicSlots, Steps, P);
}

/*
 * The START transaction for every slot in Pm (Mic input 1 first, then VIRTUAL 1, ..., [AuxStrips] then the aux returns
 * 0x10-0x13), up to stage Steps: per slot SYNCH {0x001F0004, 0x20 << 16 | synch, header, 0} (stage 6), then span none
 * twice and the route word (stage 7; ua_strip_build_start: the same words for every kind). Inline only, at most 80
 * entries. A Pm of 0..0xFF gives StripPlanStart's entries, a Pm of 0..0xFFFF the Virtual build's. Returns 0, or
 * nonzero on a planner error.
 */
static __inline ULONG StripPlanStartPm(ULONG Pm, ULONG MicSlots, ULONG Steps, STRIP_PLAN *P)
{
    ULONG addr[STRIP_PLAN_ROWS], k, synch;

    StripPlanReset(P);
    Steps = StripPlanSteps(Steps);
    if (Pm & ~((1u << STRIP_PLAN_SLOTS) - 1)) {
        P->Err = 1;
        return P->Err;
    }
    if (Steps < STRIP_STEP_SYNCH) {
        return 0;
    }
    for (k = 0; k < STRIP_PLAN_SLOTS; k++) {
        if (!(Pm & (1u << k))) {
            continue;
        }
        if (StripPlanSlotAddrs(k, MicSlots, addr) != 0) {
            P->Err = 1;
            return P->Err;
        }
        synch = StripSlotSynch(k);
        StripPlanInline(P, STRIP_CMD_SYNCH, (STRIP_SYNCH_MONO << 16) | synch, addr[0], 0);
        if (Steps >= STRIP_STEP_INPUT) {
            StripPlanInline(P, STRIP_CMD_INPUT, synch, STRIP_INPUT_SPAN, STRIP_SPAN_NONE);
            StripPlanInline(P, STRIP_CMD_INPUT, synch, STRIP_INPUT_SPAN, STRIP_SPAN_NONE);
            StripPlanInline(P, STRIP_CMD_INPUT, synch, STRIP_INPUT_ROUTE, STRIP_ROUTE_NONE);
        }
    }
    return P->Err;
}

/*
 * The pass over Pm: transaction K is STRIP_XACT_PREPARE (*Slot) for every slot in Pm, ascending (every Mic
 * strip, then every Virtual one, [AuxStrips] then every aux return), then one STRIP_XACT_START at SYNCH or beyond,
 * then STRIP_XACT_END. A Pm of 0..0xFF gives StripPassStep's list.
 */
static __inline ULONG StripPassStepPm(ULONG Pm, ULONG Steps, ULONG K, ULONG *Slot)
{
    ULONG k, n = 0;

    *Slot = 0;
    Steps = StripPlanSteps(Steps);
    Pm &= (1u << STRIP_PLAN_SLOTS) - 1;
    if (Steps < STRIP_STEP_ZERO || Pm == 0) {
        return STRIP_XACT_END;
    }
    for (k = 0; k < STRIP_PLAN_SLOTS; k++) {
        if (!(Pm & (1u << k))) {
            continue;
        }
        if (n == K) {
            *Slot = k;
            return STRIP_XACT_PREPARE;
        }
        n++;
    }
    return (K == n && Steps >= STRIP_STEP_SYNCH) ? STRIP_XACT_START : STRIP_XACT_END;
}

/* Virtual slots the room must hold for VMask: VIRTUAL n has its fixed place, so up to the highest one. */
static __inline ULONG StripVirtualSlots(ULONG VMask)
{
    ULONG n = 0, v;

    for (v = 0; v < STRIP_PLAN_VIRTUALS; v++) {
        if (VMask & (1u << v)) {
            n = v + 1;
        }
    }
    return n;
}

/* [AuxStrips] Aux places the room must hold for AMask (bit k = DSP input 0x10 + k): up to the highest one. */
static __inline ULONG StripAuxSlots(ULONG AMask)
{
    ULONG n = 0, k;

    for (k = 0; k < STRIP_PLAN_AUXES; k++) {
        if (AMask & (1u << k)) {
            n = k + 1;
        }
    }
    return n;
}

/*
 * [AuxStrips] Input Strip places the room must hold: the Virtual slots of VMask, or with any aux strip all eight Virtual
 * places and the aux places up to the highest (each aux return sits below VIRTUAL 8's place, StripPlanAuxAddrs).
 */
static __inline ULONG StripPlanInputSlots(ULONG VMask, ULONG AMask)
{
    ULONG a = StripAuxSlots(AMask);

    return (a != 0) ? STRIP_PLAN_VIRTUALS + a : StripVirtualSlots(VMask);
}

/*
 * [AuxStrips] Pool words (1-3) the strips need below the pool tops: MicSlots Mic strips and the Input Strip places of
 * VMask and AMask (StripPlanInputSlots). An AMask of 0 gives StripPlanRoom's words.
 */
static __inline void StripPlanRoomAux(ULONG MicSlots, ULONG VMask, ULONG AMask, ULONG Room[4])
{
    ULONG v = StripPlanInputSlots(VMask, AMask);

    Room[0] = 0;
    Room[1] = STRIP_POOL1_PER_STRIP * MicSlots + STRIP_VPOOL1_PER_STRIP * v;
    Room[2] = STRIP_POOL2_PER_STRIP * MicSlots + STRIP_VPOOL2_PER_STRIP * v;
    Room[3] = STRIP_POOL3_PER_STRIP * MicSlots + STRIP_VPOOL3_PER_STRIP * v;
}

/* Pool words (1-3) the strips need below the pool tops: MicSlots Mic strips and the Virtual slots of VMask. */
static __inline void StripPlanRoom(ULONG MicSlots, ULONG VMask, ULONG Room[4])
{
    StripPlanRoomAux(MicSlots, VMask, 0, Room);
}

/*
 * ---- The live probe and its repair ladder (docs/STRIPS.md 10.9) ----
 *
 * Eight strips on DSP 1 left inputs 1, 2, 5, 6, 7 at exact digital zero and 3, 4, 8 live;
 * two strips (inputs 1-2) left both at zero. The strips are word-for-word identical apart from
 * the SYNCH input and the slot, so what differs is per-input state on the DSPs that this driver
 * never sets and UA's own driver does:
 *   - STOP: UA stops every strip input (SYNCH with address 0, then span, span, route) before it
 *     builds anything, even on a fresh DSP (oa:tools/uausb/soloinit.py:186-190, 571-576), and
 *     an x4 input that read zero "whatever is toggled" carried its signal once its strip had
 *     been stopped and started again (oa:HARDWARE-SESSIONS.md:336-337, 406-425;
 *     oa:driver/ua_plugin_instance.c:706-726, oa:driver/ua_strips.c:1101-1132);
 *   - SPAN: UA's first words to a DSP after the connect set span "none" for every input
 *     (oa:tools/usb-dsp-init.py:202-227); this driver sent span only to the strip DSP, only
 *     for strip inputs, and only after the strip had started.
 * After the pass the driver measures each strip input's record slot (any non-zero sample in
 * ~8k frames = live), then, while a strip input reads zero, walks this list, measuring after
 * every repair. Each repair is inline words of kinds the x8p has already taken.
 */
#define STRIP_PROBE_END         0u
#define STRIP_PROBE_MEASURE     1u  /* Arg = settle ms before the measurement */
#define STRIP_PROBE_RESYNCH     2u  /* the STOP transaction, then the START transaction again */
#define STRIP_PROBE_SPANS       3u  /* span none for analog inputs 1-8 on every live DSP */

typedef struct _STRIP_PROBE_STEP {
    ULONG   Kind;
    ULONG   Arg;
} STRIP_PROBE_STEP;

#define STRIP_PROBE_SETTLE_MS   5000u   /* after the 48V resync: the 48V switch mutes the input a while */
#define STRIP_PROBE_STEPS       9u

/*
 * M0 as built; M1 the same 2 s later (does time alone change it?); R2 stop + start (M2);
 * R1 spans on every DSP, live (M3); R2 again, now with every span set before the SYNCH (M4).
 */
static const STRIP_PROBE_STEP g_StripProbe[STRIP_PROBE_STEPS] = {
    { STRIP_PROBE_MEASURE, STRIP_PROBE_SETTLE_MS },
    { STRIP_PROBE_MEASURE, 2000u },
    { STRIP_PROBE_RESYNCH, 0 },
    { STRIP_PROBE_MEASURE, 1500u },
    { STRIP_PROBE_SPANS,   0 },
    { STRIP_PROBE_MEASURE, 1000u },
    { STRIP_PROBE_RESYNCH, 0 },
    { STRIP_PROBE_MEASURE, 1500u },
    { STRIP_PROBE_END,     0 },
};

/*
 * StripProbe (registry): 0 = off; 1 = measure once, change nothing; 2 = measure, then the
 * repair ladder while a strip input reads zero (absent = 2). Step K of mode Mode: the ladder
 * ends after the first measurement in mode 1, and at the first measurement that finds every
 * strip input live (the caller checks that).
 */
#define STRIP_PROBE_OFF         0u
#define STRIP_PROBE_MEASURE_ONLY 1u
#define STRIP_PROBE_REPAIR      2u

static __inline const STRIP_PROBE_STEP *StripProbeStep(ULONG Mode, ULONG K)
{
    if (Mode == STRIP_PROBE_OFF || Mode > STRIP_PROBE_REPAIR || K >= STRIP_PROBE_STEPS ||
        (Mode == STRIP_PROBE_MEASURE_ONLY && K >= 1)) {
        return &g_StripProbe[STRIP_PROBE_STEPS - 1];
    }
    return &g_StripProbe[K];
}

/* Strip inputs (bits) whose record slot showed no non-zero sample: Peak[i] = slot i's peak. */
static __inline ULONG StripProbeDead(ULONG Mask, const ULONG *Peak)
{
    ULONG i, dead = 0;

    for (i = 0; i < STRIP_PLAN_INPUTS; i++) {
        if ((Mask & (1u << i)) && Peak[i] == 0) {
            dead |= 1u << i;
        }
    }
    return dead;
}

/*
 * The STOP transaction for every preamp in Mask (input 1 first): UA's teardown words per input,
 * SYNCH with address 0, span none twice, route none (soloinit.py:571-576, _ip_triplet 536-539).
 * Inline only. Returns 0, or nonzero on a planner error.
 */
static __inline ULONG StripPlanStop(ULONG Mask, STRIP_PLAN *P)
{
    ULONG i;

    StripPlanReset(P);
    if (Mask == 0 || (Mask & ~((1u << STRIP_PLAN_INPUTS) - 1))) {
        P->Err = 1;
        return P->Err;
    }
    for (i = 0; i < STRIP_PLAN_INPUTS; i++) {
        if (!(Mask & (1u << i))) {
            continue;
        }
        StripPlanInline(P, STRIP_CMD_SYNCH, (STRIP_SYNCH_MONO << 16) | i, 0, 0);
        StripPlanInline(P, STRIP_CMD_INPUT, i, STRIP_INPUT_SPAN, STRIP_SPAN_NONE);
        StripPlanInline(P, STRIP_CMD_INPUT, i, STRIP_INPUT_SPAN, STRIP_SPAN_NONE);
        StripPlanInline(P, STRIP_CMD_INPUT, i, STRIP_INPUT_ROUTE, STRIP_ROUTE_NONE);
    }
    return P->Err;
}

/*
 * Span none for analog inputs 1-8 (DSP inputs 0-7), the words UA's driver sends every input
 * right after the connect (usb-dsp-init.py:202-227 sends 0x00-0x1F but 0x10-0x13; only the
 * analog inputs are sent here). One list for every DSP. Inline only.
 */
static __inline ULONG StripPlanSpans(STRIP_PLAN *P)
{
    ULONG i;

    StripPlanReset(P);
    for (i = 0; i < STRIP_PLAN_INPUTS; i++) {
        StripPlanInline(P, STRIP_CMD_INPUT, i, STRIP_INPUT_SPAN, STRIP_SPAN_NONE);
    }
    return P->Err;
}

/*
 * ---- The strip plan per model (docs/spec-twinx.md 5.2) ----
 *
 * Everything above is the same for every model that builds strips: strip i sits on DSP input
 * (synch) i and in slot i of the pools, with the same rows, words and 48 kHz parameters. That is
 * the model entry {type, first_synch 0, first_virtual 0x18} of the x8p and of the Twin X alike
 * (oa:driver/ua_strips.c:785-789 for the Twin X Gen 2; oa:devices/apollo-twin-x.json:115-137,
 * ANALOG 1/2 = synch_in 0/1, VIRTUAL 1-8 = 24-31, the same tables for Gen 1). What differs is
 * data: how many inputs, which DSP may host them, how much pool room the check reserves, and
 * which record slot each strip feeds. A model whose first synch is not 0 would need the planner
 * changed, so StripModelFor never returns one (strip-plan-test checks it).
 *
 * The x8p row: StripMask & 0xFF, StripDsp 0-3 below the DSP count else 1 (open-apollo clamps
 * input_strips_dsp below 4), room for 8 strips, routes to every other DSP (strip-plan-test
 * compares it with the original x8p-only code).
 */
#define STRIP_MODEL_X8P         1u      /* = apollo_model.h APOLLO_MODEL_X8P (strips.c asserts) */
#define STRIP_MODEL_TWINX       2u      /* = APOLLO_MODEL_TWINX, Twin X Gen 1 (0x99) */
#define STRIP_MODULE_IDS        5u      /* = apollo_strip_pins.h APOLLO_STRIP_MODULES */
#define STRIP_CODE_WORDS        994u    /* code pool the five modules take: 0x6C+0x1F0+0x130+0x16+0x40 */
#define STRIP_DSP_ROUTE_MAX     8u      /* = apollo.h APOLLO_MAX_DSPS */

typedef struct _STRIP_MODEL {
    ULONG           Model;              /* STRIP_MODEL_* */
    const char     *Name;
    ULONG           Inputs;             /* preamp inputs that can get a strip: 1..Inputs */
    ULONG           MaskAllowed;        /* StripMask is ANDed with this */
    ULONG           FirstSynch;         /* DSP input of analog input 1 (0: the words above) */
    ULONG           FirstVirtual;       /* DSP input of VIRTUAL 1 (0x18: STRIP_VIRTUAL_SYNCH0, the Input Strips' words) */
    ULONG           VirtualAllowed;     /* [Virtual] StripVirtualMask is ANDed with this (bit n = VIRTUAL n+1) */
    ULONG           DspDefault;         /* StripDsp when absent or not allowed */
    ULONG           DspAllowed;         /* bit d: DSP d may host the strips (and d < the unit's DSP count) */
    ULONG           LayoutStrips;       /* the pool check reserves room for this many strips */
    ULONG           ModuleId[STRIP_MODULE_IDS];     /* uploaded in this order; apollo_strip_pins.h must agree */
    unsigned short  RecId[STRIP_PLAN_INPUTS];       /* record channel ID strip i feeds (0 = none) */
    ULONG           AuxAllowed;         /* [AuxStrips] StripAuxMask is ANDed with this (bit k = DSP input 0x10 + k) */
} STRIP_MODEL;

/* The plan of a model that builds strips, or 0. */
static __inline const STRIP_MODEL *StripModelFor(ULONG Model)
{
    static const STRIP_MODEL t[2] = {
        {
            /* docs/STRIPS.md 2.1-2.4: 8 preamps, DSP 1 (0-3 allowed), measured pools fit 8 strips;
               record slots 0-7 = MIC/LINE/HIZ 1-2, MIC/LINE 3-4, LINE 5-8 (the same combo jacks).
               [Virtual] VIRTUAL 1-8 (oa:devices/apollo-x8p.json synch_in 24-31, no virtual_max) */
            STRIP_MODEL_X8P, "Apollo x8p", 8, 0xFF, 0, 0x18, 0xFF, 1, 0x0F, 8,
            { 0x0EB, 0x0DA, 0x0A5, 0x0C2, 0x12B },
            { 0x0001, 0x0002, 0x0103, 0x0104, 0x0305, 0x0306, 0x0307, 0x0308 },
            /* [AuxStrips] the LEFT return inputs only, 0x10 (AUX 1) and 0x12 (AUX 2): the aux buses run mono. Nov 16 2026
               on this x8p a mono Input Strip on 0x11 or 0x13 put its output over input 1's record slot (slot 0 exact
               zero with either one built, live with 0x10/0x12 alone) and never reached slots 29/31; a stereo strip
               (SYNCH in2 = the R input) is unproven. AUX 1 L / AUX 2 L record on slots 28/30. */
            0x05,
        },
        {
            /*
             * docs/spec-twinx.md 5.2: 2 preamps (Hi-Z on input 1 is the same strip: it sits on the
             * DSP input, not on the jack), DSP 1 (a Duo has DSPs 0-1; 0 = record only, no monitor
             * mix, oa:HARDWARE-SESSIONS.md:327-328), the Mic Input Strip modules unchanged, room
             * for its 2 strips (its DSP 1 pools, measured, fit 8: tops 0x9D1B8 / 0xC6FFE / 0x08FFE000);
             * record slots 0/1 = 0x0001/0x0002 MIC/LINE/HIZ 1-2 (its IO descriptor). No talkback
             * strip (module 0xC9, no captured recipe): TALKBACK 1-2 stay silent.
             * [Virtual] VIRTUAL 1-4 (oa:devices/apollo-twin-x.json virtual_max 4; VIRTUAL 1-4 play and record).
             */
            STRIP_MODEL_TWINX, "Apollo Twin X (Gen 1)", 2, 0x03, 0, 0x18, 0x0F, 1, 0x03, 2,
            { 0x0EB, 0x0DA, 0x0A5, 0x0C2, 0x12B },
            { 0x0001, 0x0002, 0, 0, 0, 0, 0, 0 },
            /* [AuxStrips] the left return inputs only, as on the x8p (0x10, 0x12: mono aux buses; its record descriptor
               has AUX1 L .. AUX2 R, 0x0201-0x0204, at slots 28-31). Run on a Twin X Nov 16 2026: AUX 1/2 as on the x8p. */
            0x05,
        },
    };
    ULONG i;

    for (i = 0; i < 2; i++) {
        if (t[i].Model == Model && t[i].FirstSynch == 0 && t[i].FirstVirtual == STRIP_VIRTUAL_SYNCH0) {
            return &t[i];
        }
    }
    return 0;
}

/* StripMask as the model takes it (bits for inputs it does not have are dropped). */
static __inline ULONG StripModelMask(const STRIP_MODEL *M, ULONG Mask)
{
    return (M != 0) ? (Mask & M->MaskAllowed) : 0;
}

/* [Virtual] StripVirtualMask as the model takes it (VIRTUAL inputs it does not have are dropped). */
static __inline ULONG StripModelVirtualMask(const STRIP_MODEL *M, ULONG VMask)
{
    return (M != 0) ? (VMask & M->VirtualAllowed & 0xFFu) : 0;
}

/* [AuxStrips] StripAuxMask as the model takes it (aux return inputs it does not have are dropped). */
static __inline ULONG StripModelAuxMask(const STRIP_MODEL *M, ULONG AMask)
{
    return (M != 0) ? (AMask & M->AuxAllowed & 0xFu) : 0;
}

/* StripDsp as the model takes it: an allowed DSP the unit has, else the default. */
static __inline ULONG StripModelDsp(const STRIP_MODEL *M, ULONG Dsp, ULONG NumDsps)
{
    if (M == 0) {
        return 1;
    }
    if (Dsp < 32 && ((M->DspAllowed >> Dsp) & 1u) != 0 && Dsp < NumDsps) {
        return Dsp;
    }
    return M->DspDefault;
}

/*
 * Ownership (docs/STRIPS.md 2.3, oa:driver/ua_plugin_place.c:110-118): the strip DSP is ours only
 * if its boot word, read at a cold bring-up before anything connects its rings, is 3 apart from
 * the model byte [15:8] (no host since power-up). Twin X Gen 1 measured: 00002703 00000003 cold,
 * 0008270b 0008000b after UA's driver.
 */
static __inline int StripOwned(ULONG BootWord)
{
    return (BootWord & ~0x0000FF00u) == 3u;
}

/* The DSPs that get the route word {0x001E0004, i, 7, 0x1FFFF} after a strip starts: all but the
   strip DSP (oa:driver/ua_strips.c:1740-1753). x8p (6, DSP 1): 0x3D; Twin X Duo (2, DSP 1): 0x01. */
static __inline ULONG StripRouteDsps(ULONG NumDsps, ULONG Dsp)
{
    ULONG n = (NumDsps > STRIP_DSP_ROUTE_MAX) ? STRIP_DSP_ROUTE_MAX : NumDsps;
    ULONG all = (n >= 32) ? 0xFFFFFFFFu : ((1u << n) - 1u);

    return (Dsp < 32) ? (all & ~(1u << Dsp)) : all;
}

/*
 * The pool check (port of oa:driver/ua_strips.c:440-483, docs/STRIPS.md 2.4; strips.c
 * StripsReadLayout runs it): the strip DSP's 11 pool registers against the layout the recipe was
 * validated on, with room for Strips strips above each pool's scratch. 0 = fits; 1 gone (a read
 * all ones); 2 code pool not at 0xE0000; 3 code pool too small; 4+p pool p's top is not the
 * recipe's (or wraps); 8+p no room in pool p. Size/Base/Scr are the raw registers (Scr[0] unused).
 */
static __inline ULONG StripPlanLayoutCheckRoom(const ULONG Size[4], const ULONG Base[4], const ULONG Scr[4],
                                               const ULONG Room[4], ULONG CodeWords);

static __inline ULONG StripPlanLayoutCheck(const ULONG Size[4], const ULONG Base[4], const ULONG Scr[4],
                                           ULONG Strips, ULONG CodeWords)
{
    static const ULONG kPerStrip[4] = { 0, STRIP_POOL1_PER_STRIP, STRIP_POOL2_PER_STRIP, STRIP_POOL3_PER_STRIP };
    ULONG room[4], p;

    for (p = 0; p < 4; p++) {
        room[p] = kPerStrip[p] * Strips;
    }
    return StripPlanLayoutCheckRoom(Size, Base, Scr, room, CodeWords);
}

/*
 * [Virtual] The same check with the room each pool must hold above its scratch given in words (StripPlanRoom: the
 * model's Mic strips plus the Virtual strips of StripVirtualMask). The same codes: 8+p = no room in pool p.
 */
static __inline ULONG StripPlanLayoutCheckRoom(const ULONG Size[4], const ULONG Base[4], const ULONG Scr[4],
                                               const ULONG Room[4], ULONG CodeWords)
{
    static const ULONG kScratch[4] = { 0, 0x1C0u, 0x80u, 0 };   /* the Mic strip's AllocInfo */
    ULONG p;

    for (p = 0; p < 4; p++) {
        ULONG start, top, sc, floorAddr;

        if (Size[p] == 0xFFFFFFFFu || Base[p] == 0xFFFFFFFFu || (p && Scr[p] == 0xFFFFFFFFu)) {
            return 1;
        }
        start = Base[p] + (Base[p] & 1u);
        top = start + (Size[p] & ~1u);
        sc = 0;
        if (p) {
            sc = Scr[p] + (Scr[p] & 1u);
            if (sc < kScratch[p]) {
                sc = kScratch[p];
            }
        }
        floorAddr = start + sc;
        if (top < start || floorAddr < start) {
            return 4 + p;
        }
        if (p == 0) {
            if (start != 0x000E0000u) return 2;         /* apollo_strip_pins.h APOLLO_STRIP_CODE_BASE */
            if (top - start < CodeWords) return 3;
        } else {
            if (top != g_StripTop[p]) return 4 + p;
            if (floorAddr > top || top - floorAddr < Room[p]) return 8 + p;
        }
    }
    return 0;
}
