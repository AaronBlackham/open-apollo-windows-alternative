/*
 * strip-plan-test.c - host-side check of the input-strip planner (docs/STRIPS.md T0, 10.8).
 *
 * Builds driver\strip_plan.h, the exact code apollo.sys runs, and checks:
 *   - its words against the values docs/STRIPS.md 2.6/2.7 lists (computed independently: a
 *     Python re-implementation of open-apollo's planner, Nov 14 2026);
 *   - the pass for StripMask 0x01 against the one-strip run that worked on the x8p (Test A):
 *     the same 17 entries in the same order;
 *   - multi-strip passes (0xFF, and masks with gaps): the transaction order (StripPassStep),
 *     that no strip's memory is written once any strip has started (the order that left five
 *     of eight inputs at exact zero is run through the same check and must fail it), that
 *     every PREPARE writes only its own strip's rows and never a memspec link word, that the
 *     eight strips' rows are disjoint and inside the x8p's measured pools, and that the START
 *     starts every strip once, with its own header, after every PREPARE;
 *   - the live probe's repair ladder (docs/STRIPS.md 10.9): its order per StripProbe mode, the
 *     STOP and SPANS words, a re-SYNCH that writes no memory and stops then starts each input
 *     exactly once, and the dead mask.
 *   - [Twin X G5] the per-model plan (StripModelFor, docs/spec-twinx.md 5.2): the x8p's planner
 *     words are byte-identical to the pre-G5 planner (digests of every word for every input,
 *     mask and stage, taken from builds\base-pre-g56), and its row reproduces the pre-G5 strips.c
 *     (mask, DSP clamp, route DSPs, the layout check copied verbatim); the Twin X's row against
 *     the spec and upstream (inputs 1-2, DSP 1, the five modules, routes to DSP 0 on a Duo, the
 *     boot words and DSP 1 pools its G3 cold start logged), and its passes through the same
 *     stream checks as the x8p's.
 *   - [Warm] warm-start adoption (strip_adopt.h, docs/STRIPS.md 12): the record's seal, the plan
 *     hash (pinned for the x8p), the boot-word rules with the logged words, the anchor and decision
 *     tables, and start-by-start lives of an x8p and a Twin X. "--adopt-hash" prints the hashes.
 *   - [Virtual] the Input Strips on VIRTUAL 1-8 (docs/STRIPS.md 13): their tables against upstream's, every
 *     Input Strip's words against a verbatim port of open-apollo's ua_strip_build_input at the addresses its
 *     ua_strip_priv_addrs gives (x8p with 8 Mic strips, Twin X with 2, every stage), the x8p's VIRTUAL 1 word by
 *     word, the plan-mask functions equal to the Mic-only ones without Virtual strips, the pass through the
 *     stream checks, the blocks inside the pools and the room the ledger books, the refusal of a mask that does
 *     not fit, the plan hash (the pinned x8p hash unchanged), the version 2 record and the decisions.
 * Exit code 0 = all pass. "--print [mask]" prints the pass for a mask (default 0xff);
 * "--print-probe [mask]" prints the repair transactions; "--digests" prints the planner digests;
 * "--print-pm [plan mask]" prints the x8p pass for StripMask | StripVirtualMask << 8 (default 0x1ff).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "..\..\driver\strip_plan.h"
#include "..\..\driver\strip_adopt.h"      /* [Warm] warm-start adoption (docs/STRIPS.md 12) */
#include "..\..\driver\bringup_plan.h"     /* [G5] APOLLO_MODEL_*, IO descriptor lookup */
#include "..\..\include\apollo_strip_pins.h"

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

/* docs/STRIPS.md 2.6: every input's ten private addresses */
static const ULONG kAddr[8][10] = {
    { 0x9D1B0, 0x9D178, 0xC6FEE, 0x9D168, 0x08FFDFF0, 0x9D164, 0x08FFD7F0, 0x08FFCFF0, 0x08FFC7F0, 0x08FFBFF0 },
    { 0x9D15C, 0x9D124, 0xC6FDE, 0x9D114, 0x08FFBFE0, 0x9D110, 0x08FFB7E0, 0x08FFAFE0, 0x08FFA7E0, 0x08FF9FE0 },
    { 0x9D108, 0x9D0D0, 0xC6FCE, 0x9D0C0, 0x08FF9FD0, 0x9D0BC, 0x08FF97D0, 0x08FF8FD0, 0x08FF87D0, 0x08FF7FD0 },
    { 0x9D0B4, 0x9D07C, 0xC6FBE, 0x9D06C, 0x08FF7FC0, 0x9D068, 0x08FF77C0, 0x08FF6FC0, 0x08FF67C0, 0x08FF5FC0 },
    { 0x9D060, 0x9D028, 0xC6FAE, 0x9D018, 0x08FF5FB0, 0x9D014, 0x08FF57B0, 0x08FF4FB0, 0x08FF47B0, 0x08FF3FB0 },
    { 0x9D00C, 0x9CFD4, 0xC6F9E, 0x9CFC4, 0x08FF3FA0, 0x9CFC0, 0x08FF37A0, 0x08FF2FA0, 0x08FF27A0, 0x08FF1FA0 },
    { 0x9CFB8, 0x9CF80, 0xC6F8E, 0x9CF70, 0x08FF1F90, 0x9CF6C, 0x08FF1790, 0x08FF0F90, 0x08FF0790, 0x08FEFF90 },
    { 0x9CF64, 0x9CF2C, 0xC6F7E, 0x9CF1C, 0x08FEFF80, 0x9CF18, 0x08FEF780, 0x08FEEF80, 0x08FEE780, 0x08FEDF80 },
};

/* docs/STRIPS.md 2.6: the memspec words of inputs 1 and 8 */
static const ULONG kMemspec1[19] = {
    0x00150013, 0x0009d1b6, 0x0009d178, 0x0009d1ae, 0x000c6fee, 0x0009d1af, 0x0009d168,
    0x0009d193, 0x08ffdff0, 0x0009d194, 0x0009d164, 0x0009d186, 0x08ffd7f0, 0x0009d187,
    0x08ffcff0, 0x0009d17e, 0x08ffc7f0, 0x0009d17f, 0x08ffbff0 };
static const ULONG kMemspec8[19] = {
    0x00150013, 0x0009cf6a, 0x0009cf2c, 0x0009cf62, 0x000c6f7e, 0x0009cf63, 0x0009cf1c,
    0x0009cf47, 0x08feff80, 0x0009cf48, 0x0009cf18, 0x0009cf3a, 0x08fef780, 0x0009cf3b,
    0x08feef80, 0x0009cf32, 0x08fee780, 0x0009cf33, 0x08fedf80 };

static const ULONG kZeroDwords[10] = { 0x8, 0x38, 0x10, 0x10, 0x10, 0x4, 0x800, 0x800, 0x800, 0x800 };

/* Entries per StripSteps value (index; 0 = all): PREPARE, and START per input. */
static const ULONG kPrepEntries[10] = { 13, 0, 10, 11, 12, 13, 13, 13, 13, 13 };
static const ULONG kStartEntries[10] = { 4, 0, 0, 0, 0, 0, 1, 4, 4, 4 };

/* The x8p's DSP 1 pool registers, measured Nov 14 2026 (docs/STRIPS.md 2.4) */
static const ULONG kSize[4] = { 0x00004000, 0x0000a9b9, 0x00001fff, 0x00ff516a };
static const ULONG kBase[4] = { 0x000e0000, 0x00092800, 0x000c5000, 0x08008e96 };
static const ULONG kScr[4] = { 0, 0x00000080, 0x00000080, 0x00008000 };
static const ULONG kStripScratch[4] = { 0, 0x1C0, 0x80, 0 };    /* the Mic strip's AllocInfo */

/* ---- a pass, flattened into the command stream the strip DSP sees ---- */

#define EV_MEM      1u      /* memory written (Param: SRC_*) */
#define SRC_ZERO    0u
#define SRC_MEMSPEC 1u
#define SRC_WRITE   2u
#define EV_SYNCH    2u      /* SYNCH start (Arg = synch, Addr = header) */
#define EV_INPUT    3u      /* {0x001E0004, synch, param, value} */
#define EV_OTHER    4u
#define EV_MAX      4096

typedef struct _EV {
    ULONG Kind, Xact, Owner;    /* Owner: the input whose PREPARE sent it (~0 for START) */
    ULONG Addr, Count, Arg, Param, Value;
} EV;

typedef struct _STREAM {
    ULONG N, Xacts, Overflow;
    EV Ev[EV_MAX];
} STREAM;

static void Emit(STREAM *S, ULONG Kind, ULONG Xact, ULONG Owner, ULONG Addr, ULONG Count,
                 ULONG Arg, ULONG Param, ULONG Value)
{
    EV *e;

    if (S->N >= EV_MAX) {
        S->Overflow = 1;
        return;
    }
    e = &S->Ev[S->N++];
    e->Kind = Kind;
    e->Xact = Xact;
    e->Owner = Owner;
    e->Addr = Addr;
    e->Count = Count;
    e->Arg = Arg;
    e->Param = Param;
    e->Value = Value;
}

/* One masked WRITE {0x0014nnnn, addr, skip mask, values}: value k's word (native_carrier.py:94-113) */
static void EmitWrite(STREAM *S, ULONG Xact, ULONG Owner, const ULONG *c, ULONG len)
{
    ULONG first = c[1], mask = c[2], pos = first, k;

    for (k = 0; k + 3 < len; k++) {
        while (pos > first && pos - first - 1 < 32 && ((mask >> (pos - first - 1)) & 1))
            pos++;
        Emit(S, EV_MEM, Xact, Owner, pos, 1, 0, SRC_WRITE, 0);
        pos++;
    }
}

/* Every command of a plan, in ring order, the DMA payloads expanded. */
static void EmitPlan(STREAM *S, const STRIP_PLAN *p, ULONG Owner)
{
    ULONG i, x = S->Xacts++;

    for (i = 0; i < p->N; i++) {
        const STRIP_PLAN_ENT *e = &p->Ent[i];

        if (!e->Dma) {
            if (e->W[0] == STRIP_CMD_ZERO)
                Emit(S, EV_MEM, x, Owner, e->W[1], e->W[3], 0, SRC_ZERO, 0);
            else if (e->W[0] == STRIP_CMD_SYNCH && e->W[2] != 0)
                Emit(S, EV_SYNCH, x, Owner, e->W[2], 0, e->W[1] & 0xFFFF, 0, 0);
            else if (e->W[0] == STRIP_CMD_INPUT)
                Emit(S, EV_INPUT, x, Owner, 0, 0, e->W[1], e->W[2], e->W[3]);
            else
                Emit(S, EV_OTHER, x, Owner, 0, 0, 0, 0, 0);
            continue;
        }
        {
            const ULONG *c = &p->Payload[e->Off];
            ULONG at = 0;

            while (at < e->Dwords) {
                ULONG op = c[at] >> 16, len = c[at] & 0xFFFF, k;

                if (len == 0 || at + len > e->Dwords) {
                    Emit(S, EV_OTHER, x, Owner, 0, 0, 0, 0, 0);
                    break;
                }
                if (op == STRIP_OP_MEMSPEC) {
                    for (k = 1; k + 1 < len; k += 2)
                        Emit(S, EV_MEM, x, Owner, c[at + k], 1, 0, SRC_MEMSPEC, 0);
                } else if (op == STRIP_OP_WRITE && len >= 4) {
                    EmitWrite(S, x, Owner, &c[at], len);
                } else {
                    Emit(S, EV_OTHER, x, Owner, 0, 0, 0, 0, 0);
                }
                at += len;
            }
        }
    }
}

/* The pass as apollo.sys runs it: StripPassStep's transactions, in order. */
static int BuildPass(STREAM *S, ULONG Mask, ULONG Steps)
{
    STRIP_PLAN p;
    ULONG k, in, kind;

    memset(S, 0, sizeof(*S));
    for (k = 0; k <= STRIP_PLAN_INPUTS + 1; k++) {
        kind = StripPassStep(Mask, Steps, k, &in);
        if (kind == STRIP_XACT_END)
            return 1;
        if (kind == STRIP_XACT_PREPARE) {
            if (StripPlanPrepare(in, Steps, &p) != 0)
                return 0;
            EmitPlan(S, &p, in);
        } else if (kind == STRIP_XACT_START) {
            if (StripPlanStart(Mask, Steps, &p) != 0)
                return 0;
            EmitPlan(S, &p, 0xFFFFFFFFu);
        } else {
            return 0;
        }
    }
    return 0;   /* StripPassStep never ended */
}

/* The order before Nov 14 (open-apollo's): each input's memory and its start in one
   transaction, input after input. Only here to prove the checks below catch it. */
static int BuildOldPass(STREAM *S, ULONG Mask, ULONG Steps)
{
    STRIP_PLAN p, q;
    ULONG in, i;

    memset(S, 0, sizeof(*S));
    for (in = 0; in < STRIP_PLAN_INPUTS; in++) {
        if (!(Mask & (1u << in)))
            continue;
        if (StripPlanPrepare(in, Steps, &p) != 0 || StripPlanStart(1u << in, Steps, &q) != 0)
            return 0;
        for (i = 0; i < q.N && p.N < STRIP_PLAN_ENTS_MAX; i++)
            p.Ent[p.N++] = q.Ent[i];
        EmitPlan(S, &p, in);
    }
    return 1;
}

/* Which input's rows hold Addr, and which row; 0 if none. */
static int RowOf(ULONG Addr, ULONG *Input, ULONG *Row)
{
    ULONG in, j, a[STRIP_PLAN_ROWS];

    for (in = 0; in < STRIP_PLAN_INPUTS; in++) {
        StripPlanAddrs(in, a);
        for (j = 0; j < STRIP_PLAN_ROWS; j++) {
            if (Addr >= a[j] && Addr - a[j] < g_StripRows[j].Dwords) {
                *Input = in;
                *Row = j;
                return 1;
            }
        }
    }
    return 0;
}

/* Is Addr a memspec link word (a block address the DSP follows) of some strip? */
static int IsLinkWord(ULONG Addr)
{
    ULONG in, j, a[STRIP_PLAN_ROWS];

    for (in = 0; in < STRIP_PLAN_INPUTS; in++) {
        StripPlanAddrs(in, a);
        for (j = 1; j < STRIP_PLAN_ROWS; j++) {
            ULONG tgt = (j == 1) ? a[0] : a[1];

            if (Addr == tgt + (g_StripRows[j].Link & 0xFFFFFF))
                return 1;
        }
    }
    return 0;
}

/* No memory word of any strip is written after the first SYNCH of the pass. */
static int NoWriteAfterStart(const STREAM *S)
{
    ULONG i;
    int started = 0;

    for (i = 0; i < S->N; i++) {
        if (S->Ev[i].Kind == EV_SYNCH)
            started = 1;
        else if (S->Ev[i].Kind == EV_MEM && started)
            return 0;
    }
    return 1;
}

static void CheckPass(ULONG Mask, ULONG Steps)
{
    STREAM *s = (STREAM *)calloc(1, sizeof(STREAM));
    ULONG i, n = 0, in, row, seen = 0, lastPrep = 0, firstSynch = 0xFFFFFFFFu, steps;
    int ownOk = 1, linkOk = 1, synchOk = 1, inputOk = 1;
    char what[160];

    if (s == NULL) {
        Check(0, "out of memory");
        return;
    }
    steps = StripPlanSteps(Steps);
    for (i = 0; i < STRIP_PLAN_INPUTS; i++)
        n += (Mask >> i) & 1;
    sprintf_s(what, sizeof(what), "mask 0x%02lx stage %lu: pass plans", Mask, steps);
    Check(BuildPass(s, Mask, Steps) && !s->Overflow, what);

    for (i = 0; i < s->N; i++) {
        const EV *e = &s->Ev[i];

        if (e->Kind == EV_MEM) {
            ULONG in2, row2;

            lastPrep = i;
            /* the whole range inside one row of the strip whose PREPARE sent it */
            if (e->Count == 0 || !RowOf(e->Addr, &in, &row) || in != e->Owner ||
                !RowOf(e->Addr + e->Count - 1, &in2, &row2) || in2 != in || row2 != row)
                ownOk = 0;
            /* the memspec writes exactly the link words; a WRITE value never lands on one */
            if ((e->Param == SRC_MEMSPEC && !IsLinkWord(e->Addr)) ||
                (e->Param == SRC_WRITE && IsLinkWord(e->Addr)))
                linkOk = 0;
        } else if (e->Kind == EV_SYNCH) {
            if (firstSynch == 0xFFFFFFFFu)
                firstSynch = i;
            if (e->Arg >= STRIP_PLAN_INPUTS || !(Mask & (1u << e->Arg)) || (seen & (1u << e->Arg)) ||
                e->Addr != kAddr[e->Arg][0] || e->Owner != 0xFFFFFFFFu)
                synchOk = 0;
            seen |= 1u << (e->Arg < 32 ? e->Arg : 0);
        } else if (e->Kind == EV_INPUT) {
            if (e->Arg >= STRIP_PLAN_INPUTS || !(seen & (1u << e->Arg)) ||
                !((e->Param == STRIP_INPUT_SPAN && e->Value == STRIP_SPAN_NONE) ||
                  (e->Param == STRIP_INPUT_ROUTE && e->Value == STRIP_ROUTE_NONE)))
                inputOk = 0;
        } else {
            inputOk = 0;
        }
    }

    sprintf_s(what, sizeof(what), "mask 0x%02lx stage %lu: %lu transactions", Mask, steps,
              n + (steps >= STRIP_STEP_SYNCH ? 1u : 0u));
    Check(s->Xacts == n + (steps >= STRIP_STEP_SYNCH && n ? 1u : 0u), what);
    sprintf_s(what, sizeof(what), "mask 0x%02lx stage %lu: no strip memory written after a strip started",
              Mask, steps);
    Check(NoWriteAfterStart(s) && (firstSynch == 0xFFFFFFFFu || lastPrep < firstSynch), what);
    sprintf_s(what, sizeof(what), "mask 0x%02lx stage %lu: each PREPARE writes only its own rows", Mask, steps);
    Check(ownOk, what);
    sprintf_s(what, sizeof(what), "mask 0x%02lx stage %lu: no value lands on a memspec link word", Mask, steps);
    Check(linkOk, what);
    sprintf_s(what, sizeof(what), "mask 0x%02lx stage %lu: every strip started once, own header, in the START",
              Mask, steps);
    Check(synchOk && seen == (steps >= STRIP_STEP_SYNCH ? Mask : 0u), what);
    sprintf_s(what, sizeof(what), "mask 0x%02lx stage %lu: span/route words only after their SYNCH", Mask, steps);
    Check(inputOk, what);
    free(s);
}

static void PrintPlan(const STRIP_PLAN *p)
{
    ULONG i, k;

    for (i = 0; i < p->N; i++) {
        const STRIP_PLAN_ENT *e = &p->Ent[i];

        if (!e->Dma) {
            printf("  %2lu inline  %08lx %08lx %08lx %08lx\n", i + 1, e->W[0], e->W[1], e->W[2],
                   e->W[3]);
            continue;
        }
        printf("  %2lu DMA_REF %08lx at payload +0x%03lx:", i + 1, 0x80000000ul | e->Dwords,
               e->Off * 4);
        for (k = 0; k < e->Dwords; k++)
            printf("%s%08lx", (k % 8) ? " " : "\n      ", p->Payload[e->Off + k]);
        printf("\n");
    }
}

static void PrintPass(ULONG Mask)
{
    STRIP_PLAN p;
    ULONG k, in, kind;

    for (k = 0; k <= STRIP_PLAN_INPUTS + 1; k++) {
        kind = StripPassStep(Mask, 0, k, &in);
        if (kind == STRIP_XACT_PREPARE) {
            (void)StripPlanPrepare(in, 0, &p);
            printf("Transaction %lu: PREPARE input %lu (StripSteps 0), on the strip DSP:\n", k + 1, in + 1);
            PrintPlan(&p);
        } else if (kind == STRIP_XACT_START) {
            (void)StripPlanStart(Mask, 0, &p);
            printf("Transaction %lu: START StripMask 0x%02lx, on the strip DSP:\n", k + 1, Mask);
            PrintPlan(&p);
        } else {
            break;
        }
    }
}

/* ---- the live probe's repair ladder (docs/STRIPS.md 10.9) ---- */

/* The STOP transaction for Mask: per input, input 1 first, SYNCH stop + span + span + route. */
static int StopWordsOk(ULONG Mask, const STRIP_PLAN *p)
{
    ULONG in, k = 0;

    for (in = 0; in < STRIP_PLAN_INPUTS; in++) {
        const STRIP_PLAN_ENT *e;

        if (!(Mask & (1u << in)))
            continue;
        if (k + 4 > p->N)
            return 0;
        e = &p->Ent[k];
        if (e[0].Dma || e[0].W[0] != 0x001F0004 || e[0].W[1] != (0x00200000 | in) ||
            e[0].W[2] != 0 || e[0].W[3] != 0)
            return 0;
        if (e[1].Dma || e[1].W[0] != 0x001E0004 || e[1].W[1] != in || e[1].W[2] != 8 || e[1].W[3] != 0x20 ||
            e[2].Dma || e[2].W[0] != 0x001E0004 || e[2].W[1] != in || e[2].W[2] != 8 || e[2].W[3] != 0x20 ||
            e[3].Dma || e[3].W[0] != 0x001E0004 || e[3].W[1] != in || e[3].W[2] != 7 ||
            e[3].W[3] != 0x0001FFFF)
            return 0;
        k += 4;
    }
    return k == p->N && p->Len == 0;
}

/*
 * A repair re-SYNCH (STOP then START) as the strip DSP sees it: no memory written, every strip
 * input stopped exactly once before any start, then started once at its own header; every
 * command a kind the pass already sends (SYNCH, input words 7/8).
 */
static int ResynchOk(ULONG Mask)
{
    STRIP_PLAN p;
    ULONG i, stopped = 0, started = 0;

    if (StripPlanStop(Mask, &p) != 0 || p.N > STRIP_PLAN_ENTS_MAX || p.N > 32)
        return 0;
    for (i = 0; i < p.N; i++) {
        const STRIP_PLAN_ENT *e = &p.Ent[i];

        if (e->Dma)
            return 0;
        if (e->W[0] == STRIP_CMD_SYNCH) {
            ULONG in = e->W[1] & 0xFFFF;

            if (e->W[2] != 0 || in >= STRIP_PLAN_INPUTS || (stopped & (1u << in)) ||
                (e->W[1] >> 16) != STRIP_SYNCH_MONO)
                return 0;
            stopped |= 1u << in;
        } else if (e->W[0] != STRIP_CMD_INPUT ||
                   !((e->W[2] == STRIP_INPUT_SPAN && e->W[3] == STRIP_SPAN_NONE) ||
                     (e->W[2] == STRIP_INPUT_ROUTE && e->W[3] == STRIP_ROUTE_NONE))) {
            return 0;
        }
    }
    if (stopped != Mask || StripPlanStart(Mask, 0, &p) != 0 || p.N > 32)
        return 0;
    for (i = 0; i < p.N; i++) {
        const STRIP_PLAN_ENT *e = &p.Ent[i];

        if (e->Dma)
            return 0;
        if (e->W[0] == STRIP_CMD_SYNCH) {
            ULONG in = e->W[1] & 0xFFFF;

            if (in >= STRIP_PLAN_INPUTS || e->W[2] != kAddr[in][0] || (started & (1u << in)))
                return 0;
            started |= 1u << in;
        }
    }
    return started == Mask;
}

static void CheckProbe(void)
{
    static const ULONG kKind[STRIP_PROBE_STEPS] = {
        STRIP_PROBE_MEASURE, STRIP_PROBE_MEASURE, STRIP_PROBE_RESYNCH, STRIP_PROBE_MEASURE,
        STRIP_PROBE_SPANS, STRIP_PROBE_MEASURE, STRIP_PROBE_RESYNCH, STRIP_PROBE_MEASURE,
        STRIP_PROBE_END };
    static const ULONG masks[] = { 0x01, 0x03, 0x0F, 0x80, 0xA4, 0xFF };
    STRIP_PLAN p;
    ULONG k, j, i, ok;
    char what[160];

    /* The ladder: mode 2 is the full list, mode 1 one measurement, mode 0 nothing */
    ok = 1;
    for (k = 0; k < STRIP_PROBE_STEPS; k++)
        ok = ok && StripProbeStep(STRIP_PROBE_REPAIR, k)->Kind == kKind[k];
    Check(ok, "probe ladder (StripProbe 2): M, M, stop+start, M, spans, M, stop+start, M, end");
    Check(StripProbeStep(STRIP_PROBE_REPAIR, 0)->Arg == 5000 &&
          StripProbeStep(STRIP_PROBE_REPAIR, 1)->Arg == 2000 &&
          StripProbeStep(STRIP_PROBE_REPAIR, 3)->Arg == 1500 &&
          StripProbeStep(STRIP_PROBE_REPAIR, 5)->Arg == 1000 &&
          StripProbeStep(STRIP_PROBE_REPAIR, 7)->Arg == 1500,
          "probe ladder: 5 s settle after the 48V resync, then 2 / 1.5 / 1 / 1.5 s");
    Check(StripProbeStep(STRIP_PROBE_REPAIR, STRIP_PROBE_STEPS)->Kind == STRIP_PROBE_END &&
          StripProbeStep(STRIP_PROBE_REPAIR, 1000)->Kind == STRIP_PROBE_END,
          "probe ladder: past its end, END");
    Check(StripProbeStep(STRIP_PROBE_MEASURE_ONLY, 0)->Kind == STRIP_PROBE_MEASURE &&
          StripProbeStep(STRIP_PROBE_MEASURE_ONLY, 1)->Kind == STRIP_PROBE_END,
          "StripProbe 1: one measurement, no repair");
    Check(StripProbeStep(STRIP_PROBE_OFF, 0)->Kind == STRIP_PROBE_END &&
          StripProbeStep(7, 0)->Kind == STRIP_PROBE_END, "StripProbe 0 (and junk): nothing");
    ok = 1;
    for (j = 0; j < 3; j++)
        for (k = 0; k < STRIP_PROBE_STEPS + 2; k++) {
            ULONG kd = StripProbeStep(j, k)->Kind;

            if (kd != STRIP_PROBE_MEASURE && kd != STRIP_PROBE_END && j != STRIP_PROBE_REPAIR)
                ok = 0;     /* only mode 2 ever repairs */
        }
    Check(ok, "only StripProbe 2 sends repairs");

    /* The dead mask: only strip inputs, only slots that never left zero */
    {
        ULONG peak[8] = { 0, 0, 5, 0x100, 0, 0, 0, 1 };

        Check(StripProbeDead(0xFF, peak) == 0x73, "dead mask 0xff: inputs 1 2 5 6 7 (the T6 pattern)");
        Check(StripProbeDead(0x03, peak) == 0x03, "dead mask 0x03: inputs 1 2");
        Check(StripProbeDead(0x8C, peak) == 0x00, "dead mask: live strips only");
        Check(StripProbeDead(0x00, peak) == 0x00, "dead mask: no strips, nothing dead");
    }

    /* STOP words for every mask, and the re-SYNCH as a whole */
    for (j = 0; j < sizeof(masks) / sizeof(masks[0]); j++) {
        sprintf_s(what, sizeof(what), "STOP 0x%02lx: SYNCH stop, span, span, route per input", masks[j]);
        Check(StripPlanStop(masks[j], &p) == 0 && StopWordsOk(masks[j], &p), what);
        sprintf_s(what, sizeof(what), "re-SYNCH 0x%02lx: inline only, each input stopped then started once", masks[j]);
        Check(ResynchOk(masks[j]), what);
    }
    Check(StripPlanStop(0xFF, &p) == 0 && p.N == 32 && p.N <= STRIP_PLAN_ENTS_MAX,
          "STOP 0xff fits one transaction (32 entries)");
    Check(StripPlanStop(0x00, &p) != 0, "STOP refuses an empty mask");
    Check(StripPlanStop(0x100, &p) != 0, "STOP refuses a mask beyond input 8");

    /* Spans: 8 inline words, span none for DSP inputs 0-7 */
    ok = StripPlanSpans(&p) == 0 && p.N == 8 && p.Len == 0;
    for (i = 0; ok && i < p.N; i++)
        ok = !p.Ent[i].Dma && p.Ent[i].W[0] == 0x001E0004 && p.Ent[i].W[1] == i &&
             p.Ent[i].W[2] == 8 && p.Ent[i].W[3] == 0x20;
    Check(ok, "SPANS: {0x001E0004, i, 8, 0x20} for inputs 1-8");
}

/* ------------------------------------------------------------------ */
/* [Twin X G5] the per-model plan                                      */
/* ------------------------------------------------------------------ */

/* ---- the x8p's words, byte for byte: FNV-1a digests of everything the planner can produce ---- */

typedef struct _PLAN_FNV {
    unsigned long long H;
} PLAN_FNV;

static void FnvInit(PLAN_FNV *F)
{
    F->H = 0xcbf29ce484222325ull;
}

static void FnvU32(PLAN_FNV *F, ULONG V)
{
    int i;

    for (i = 0; i < 4; i++) {
        F->H ^= (unsigned long long)((V >> (8 * i)) & 0xFFu);
        F->H *= 0x100000001b3ull;
    }
}

static void FnvPlan(PLAN_FNV *F, ULONG Rc, const STRIP_PLAN *P)
{
    ULONG i, k;

    FnvU32(F, Rc);
    FnvU32(F, P->N);
    FnvU32(F, P->Len);
    FnvU32(F, P->Err);
    for (i = 0; i < STRIP_PLAN_ROWS; i++)
        FnvU32(F, P->Addr[i]);
    for (i = 0; i < P->N && i < STRIP_PLAN_ENTS_MAX; i++) {
        const STRIP_PLAN_ENT *e = &P->Ent[i];

        FnvU32(F, e->Dma);
        for (k = 0; k < 4; k++)
            FnvU32(F, e->W[k]);
        FnvU32(F, e->Off);
        FnvU32(F, e->Dwords);
    }
    for (i = 0; i < P->Len && i < STRIP_PLAN_PAYLOAD_MAX; i++)
        FnvU32(F, P->Payload[i]);
}

#define PLAN_DIGESTS    8

/*
 * D[0] constants and tables, [1] addresses, [2] PREPARE (inputs 1-9, stages 0-10), [3] START (masks
 * 0-0x100, stages 0-10), [4] the pass order (masks 0-0x1FF), [5] STOP, [6] SPANS, [7] the probe ladder
 * and dead masks. The same code ran on builds\base-pre-g56\driver\strip_plan.h (Nov 14 2026) and
 * gave kGolden.
 */
static void PlanDigests(unsigned long long D[PLAN_DIGESTS])
{
    static STRIP_PLAN p;
    PLAN_FNV f;
    ULONG in, st, m, k, kind, j, a[STRIP_PLAN_ROWS];

    FnvInit(&f);
    FnvU32(&f, STRIP_PLAN_ROWS); FnvU32(&f, STRIP_PLAN_INPUTS); FnvU32(&f, STRIP_PLAN_PREPARE_ENTS);
    FnvU32(&f, STRIP_PLAN_START_ENTS); FnvU32(&f, STRIP_PLAN_ENTS_MAX); FnvU32(&f, STRIP_PLAN_PAYLOAD_MAX);
    FnvU32(&f, STRIP_CMD_ZERO); FnvU32(&f, STRIP_CMD_SYNCH); FnvU32(&f, STRIP_CMD_INPUT);
    FnvU32(&f, STRIP_SYNCH_MONO); FnvU32(&f, STRIP_INPUT_ROUTE); FnvU32(&f, STRIP_INPUT_SPAN);
    FnvU32(&f, STRIP_ROUTE_NONE); FnvU32(&f, STRIP_SPAN_NONE); FnvU32(&f, STRIP_PLUGIN_MIC);
    FnvU32(&f, STRIP_OP_WRITE); FnvU32(&f, STRIP_OP_MEMSPEC);
    FnvU32(&f, STRIP_STEP_UPLOADS); FnvU32(&f, STRIP_STEP_ZERO); FnvU32(&f, STRIP_STEP_MEMSPEC);
    FnvU32(&f, STRIP_STEP_HEADER); FnvU32(&f, STRIP_STEP_PARAMS); FnvU32(&f, STRIP_STEP_SYNCH);
    FnvU32(&f, STRIP_STEP_INPUT); FnvU32(&f, STRIP_STEP_OTHERS); FnvU32(&f, STRIP_STEP_PHANTOM);
    FnvU32(&f, STRIP_STEP_ALL);
    FnvU32(&f, STRIP_DW_HDR); FnvU32(&f, STRIP_DW_M0); FnvU32(&f, STRIP_DW_M1); FnvU32(&f, STRIP_DW_M2);
    FnvU32(&f, STRIP_DW_M3); FnvU32(&f, STRIP_DW_M4); FnvU32(&f, STRIP_DW_C);
    FnvU32(&f, STRIP_POOL1_PER_STRIP); FnvU32(&f, STRIP_POOL2_PER_STRIP); FnvU32(&f, STRIP_POOL3_PER_STRIP);
    FnvU32(&f, STRIP_TOP1); FnvU32(&f, STRIP_TOP2); FnvU32(&f, STRIP_TOP3);
    for (j = 0; j < STRIP_PLAN_ROWS; j++) {
        FnvU32(&f, g_StripRows[j].Pool);
        FnvU32(&f, g_StripRows[j].Dwords);
        FnvU32(&f, g_StripRows[j].Link);
    }
    for (j = 0; j < 4; j++)
        FnvU32(&f, g_StripTop[j]);
    for (j = 0; j < 18; j++)
        FnvU32(&f, g_StripM0A[j]);
    for (j = 0; j < 16; j++)
        FnvU32(&f, g_StripM0B[j]);
    for (j = 0; j < 15; j++)
        FnvU32(&f, g_StripM3W[j]);
    FnvU32(&f, STRIP_XACT_END); FnvU32(&f, STRIP_XACT_PREPARE); FnvU32(&f, STRIP_XACT_START);
    FnvU32(&f, STRIP_PROBE_END); FnvU32(&f, STRIP_PROBE_MEASURE); FnvU32(&f, STRIP_PROBE_RESYNCH);
    FnvU32(&f, STRIP_PROBE_SPANS); FnvU32(&f, STRIP_PROBE_SETTLE_MS); FnvU32(&f, STRIP_PROBE_STEPS);
    FnvU32(&f, STRIP_PROBE_OFF); FnvU32(&f, STRIP_PROBE_MEASURE_ONLY); FnvU32(&f, STRIP_PROBE_REPAIR);
    D[0] = f.H;

    FnvInit(&f);
    for (in = 0; in < STRIP_PLAN_INPUTS + 2; in++) {
        StripPlanAddrs(in, a);
        for (j = 0; j < STRIP_PLAN_ROWS; j++)
            FnvU32(&f, a[j]);
    }
    D[1] = f.H;

    FnvInit(&f);
    for (in = 0; in <= STRIP_PLAN_INPUTS; in++)
        for (st = 0; st <= STRIP_STEP_ALL + 1; st++)
            FnvPlan(&f, StripPlanPrepare(in, st, &p), &p);
    D[2] = f.H;

    FnvInit(&f);
    for (m = 0; m <= 0x100; m++)
        for (st = 0; st <= STRIP_STEP_ALL + 1; st++)
            FnvPlan(&f, StripPlanStart(m, st, &p), &p);
    D[3] = f.H;

    FnvInit(&f);
    for (m = 0; m <= 0x1FF; m++)
        for (st = 0; st <= STRIP_STEP_ALL + 1; st++)
            for (k = 0; k <= STRIP_PLAN_INPUTS + 2; k++) {
                kind = StripPassStep(m, st, k, &in);
                FnvU32(&f, kind);
                FnvU32(&f, in);
            }
    D[4] = f.H;

    FnvInit(&f);
    for (m = 0; m <= 0x100; m++)
        FnvPlan(&f, StripPlanStop(m, &p), &p);
    D[5] = f.H;

    FnvInit(&f);
    FnvPlan(&f, StripPlanSpans(&p), &p);
    D[6] = f.H;

    FnvInit(&f);
    for (m = 0; m < 4; m++)
        for (k = 0; k <= STRIP_PROBE_STEPS + 1; k++) {
            const STRIP_PROBE_STEP *s = StripProbeStep(m, k);

            FnvU32(&f, s->Kind);
            FnvU32(&f, s->Arg);
        }
    for (m = 0; m <= 0x1FF; m++) {
        ULONG peak[STRIP_PLAN_INPUTS];

        for (j = 0; j < STRIP_PLAN_INPUTS; j++)
            peak[j] = ((m * 7 + j * 13) % 5 == 0) ? 0 : (j + 1) * 0x101;
        FnvU32(&f, StripProbeDead(m, peak));
    }
    D[7] = f.H;
}

/* builds\base-pre-g56\driver\strip_plan.h through PlanDigests (scratch gen.c, Nov 14 2026) */
static const unsigned long long kGolden[PLAN_DIGESTS] = {
    0xD50E677A91CAF270ull, 0xF5AF329550078EF7ull, 0x7F252CA750FE2294ull, 0x35AF1C86C9D26DC5ull,
    0xC205EBEE9247E725ull, 0x81FA06ACA2D7D6A5ull, 0xEE8A668D17EE96CDull, 0x28F82862448F60DBull,
};

/* ---- strips.c before G5, as written there (builds\base-pre-g56), for the x8p row ---- */

/* ApolloStripLayoutCheck(Size, Base, Scr, 7) before G5 (max() spelled out). */
static ULONG OldLayoutCheck(const ULONG *Size, const ULONG *Base, const ULONG *Scr, ULONG Hi)
{
    static const ULONG kTop[4] = { 0, 0x0009D1B8u, 0x000C6FFEu, 0x08FFE000u };
    static const ULONG kScratch[4] = { 0, 0x1C0u, 0x80u, 0 };
    static const ULONG kPerStrip[4] = { 0, 0x54u, 0x10u, 0x2010u };
    ULONG p, codeWords = 0;

    for (p = 0; p < APOLLO_STRIP_MODULES; p++) {
        codeWords += g_ApolloStripPins[p].PoolWords;
    }
    for (p = 0; p < 4; p++) {
        ULONG start, top, sc, floorAddr, s1;

        if (Size[p] == 0xFFFFFFFFu || Base[p] == 0xFFFFFFFFu || (p && Scr[p] == 0xFFFFFFFFu)) {
            return 1;
        }
        start = Base[p] + (Base[p] & 1u);
        top = start + (Size[p] & ~1u);
        s1 = Scr[p] + (Scr[p] & 1u);
        sc = p ? (s1 > kScratch[p] ? s1 : kScratch[p]) : 0;
        floorAddr = start + sc;
        if (top < start || floorAddr < start) {
            return 4 + p;
        }
        if (p == 0) {
            if (start != APOLLO_STRIP_CODE_BASE) return 2;
            if (top - start < codeWords) return 3;
        } else {
            if (top != kTop[p]) return 4 + p;
            if (floorAddr > top || top - floorAddr < kPerStrip[p] * (Hi + 1)) return 8 + p;
        }
    }
    return 0;
}

/* PrepareHardware's StripDsp rule before G5: above 3 or not below the DSP count -> 1. */
static ULONG OldDsp(ULONG Dsp, ULONG NumDsps)
{
    return (Dsp > 3u || Dsp >= NumDsps) ? 1u : Dsp;
}

/* StripsStartAll's route loop before G5: every DSP below the count (and 8) but the strip DSP. */
static ULONG OldRoutes(ULONG NumDsps, ULONG Dsp)
{
    ULONG d, m = 0;

    for (d = 0; d < NumDsps && d < 8; d++) {
        if (d == Dsp)
            continue;
        m |= 1u << d;
    }
    return m;
}

/* The Twin X's DSP 1 pools as its G3 cold start logged them (tools\phase1\step-g3.log, Nov 14 2026). */
static const ULONG kTwxSize[4] = { 0x00004000, 0x0000a9b9, 0x00001fff, 0x00ff51ae };
static const ULONG kTwxBase[4] = { 0x000e0000, 0x00092800, 0x000c5000, 0x08008e52 };
static const ULONG kTwxScr[4] = { 0, 0x00000080, 0x00000080, 0x00008000 };

/* Its record descriptor at G3 (34 slots, the order the FPGA lays the frame out in). */
static const unsigned short kTwxRecIds[34] = {
    0x0001, 0x0002, 0x0103, 0x0104, 0x0305, 0x0306, 0x0307, 0x0308,
    0x0401, 0x0402, 0x0403, 0x0404, 0x0405, 0x0406, 0x0407, 0x0408,
    0x0501, 0x0502, 0x0701, 0x0702, 0x0703, 0x0704, 0x0705, 0x0706, 0x0707, 0x0708,
    0x0901, 0x0902, 0x0201, 0x0202, 0x0203, 0x0204, 0x0B01, 0x0B02,
};

static void LayoutBoth(const ULONG *Size, const ULONG *Base, const ULONG *Scr, int *Same)
{
    if (StripPlanLayoutCheck(Size, Base, Scr, 8, STRIP_CODE_WORDS) != OldLayoutCheck(Size, Base, Scr, 7))
        *Same = 0;
}

static void CheckModels(void)
{
    const STRIP_MODEL *x = StripModelFor(STRIP_MODEL_X8P), *t = StripModelFor(STRIP_MODEL_TWINX);
    unsigned long long d[PLAN_DIGESTS];
    static const char *const part[PLAN_DIGESTS] = {
        "constants and tables", "private addresses", "every PREPARE", "every START", "the pass order",
        "every STOP", "SPANS", "the probe ladder and dead masks" };
    ULONG i, j, n, m, ok, code;
    char what[200];

    /* 1. The x8p's planner words: byte for byte what base-pre-g56 produced */
    PlanDigests(d);
    for (i = 0; i < PLAN_DIGESTS; i++) {
        sprintf_s(what, sizeof(what), "x8p planner unchanged since base-pre-g56: %s (digest %016llX, golden %016llX)",
                  part[i], d[i], kGolden[i]);
        Check(d[i] == kGolden[i], what);
    }

    if (x == NULL || t == NULL) {
        Check(0, "both models have a strip plan");
        return;
    }
    Check(STRIP_MODEL_X8P == APOLLO_MODEL_X8P && STRIP_MODEL_TWINX == APOLLO_MODEL_TWINX,
          "strip plan model ids = apollo_model.h's");
    Check(StripModelFor(0) == NULL && StripModelFor(3) == NULL && StripModelFor(0x99) == NULL &&
          StripModelFor(0x22) == NULL, "no strip plan for any other model id");
    Check(x->FirstSynch == 0 && t->FirstSynch == 0, "both models: analog input 1 = DSP input 0 (the planner's words)");

    /* 2. The x8p row reproduces strips.c before G5 */
    ok = 1;
    for (m = 0; m < 0x400; m++)
        ok = ok && StripModelMask(x, m) == (m & 0xFF);
    Check(ok && StripModelMask(x, 0xFFFFFFFFu) == 0xFF, "x8p: StripMask & 0xFF, as before");
    ok = 1;
    for (n = 0; n <= 9; n++)
        for (j = 0; j <= 40; j++)
            ok = ok && StripModelDsp(x, j, n) == OldDsp(j, n);
    Check(ok && StripModelDsp(x, 0xFFFFFFFFu, 6) == 1, "x8p: StripDsp 0-3 below the DSP count, else 1, as before");
    ok = 1;
    for (n = 0; n <= 12; n++)
        for (j = 0; j <= 12; j++)
            ok = ok && StripRouteDsps(n, j) == OldRoutes(n, j);
    Check(ok, "route DSPs = the pre-G5 loop (every DSP but the strip DSP) for 0-12 DSPs");
    Check(StripRouteDsps(6, 1) == 0x3D, "x8p: route words to DSPs 0, 2-5");
    Check(x->Inputs == 8 && x->MaskAllowed == 0xFF && x->LayoutStrips == 8 && x->DspDefault == 1 &&
          x->DspAllowed == 0x0F && x->FirstVirtual == 0x18, "x8p row: 8 inputs, room for 8, DSP 1 (0-3)");
    {
        static const unsigned short want[8] = { 0x0001, 0x0002, 0x0103, 0x0104, 0x0305, 0x0306, 0x0307, 0x0308 };

        Check(memcmp(x->RecId, want, sizeof(want)) == 0, "x8p: strip i feeds MIC/LINE i+1 (endpoints.c rows 2-9)");
    }
    /* the layout check moved from strips.c to strip_plan.h: same result on every geometry tried */
    {
        /* the x8p's measured pools (kSize/kBase/kScr), the Twin X's, and the x4's recipe values (STRIPS.md 2.4) */
        static const ULONG kX4Size[4] = { 0x00004000, 0x0000A9B8, 0x00001FFE, 0x00FFE000 };
        static const ULONG kX4Base[4] = { 0x000E0000, 0x00092800, 0x000C5000, 0x08000000 };
        static const ULONG deltas[] = { 0, 1, 2, 0xFFFFFFFFu, 0xFFFFFFFEu, 0x100, 0x10000, 0x80000000u };
        const ULONG *sizes[3] = { kSize, kTwxSize, kX4Size }, *bases[3] = { kBase, kTwxBase, kX4Base };
        const ULONG *scrs[3] = { kScr, kTwxScr, kScr };
        int same = 1;
        ULONG g, r, dd, sz[4], bs[4], sc[4];

        for (g = 0; g < 3; g++) {
            LayoutBoth(sizes[g], bases[g], scrs[g], &same);
            for (r = 0; r < 11; r++)
                for (dd = 0; dd < sizeof(deltas) / sizeof(deltas[0]); dd++) {
                    memcpy(sz, sizes[g], sizeof(sz));
                    memcpy(bs, bases[g], sizeof(bs));
                    memcpy(sc, scrs[g], sizeof(sc));
                    if (r < 4)
                        sz[r] += deltas[dd];
                    else if (r < 8)
                        bs[r - 4] += deltas[dd];
                    else
                        sc[r - 7] += deltas[dd];
                    LayoutBoth(sz, bs, sc, &same);
                    sz[r & 3] = 0xFFFFFFFFu;
                    LayoutBoth(sz, bs, sc, &same);
                }
        }
        Check(same, "layout check: strip_plan.h's = strips.c's before G5 on 3 geometries x 11 registers x 8 changes");
        Check(StripPlanLayoutCheck(kSize, kBase, kScr, x->LayoutStrips, STRIP_CODE_WORDS) == 0,
              "x8p DSP 1 pools (measured Nov 14): fit 8 strips");
    }
    for (i = 0, n = 0; i < APOLLO_STRIP_MODULES; i++)
        n += g_ApolloStripPins[i].PoolWords;
    Check(n == STRIP_CODE_WORDS, "the pinned modules take 994 code words (STRIP_CODE_WORDS)");

    /* 3. The Twin X row: docs/spec-twinx.md 5.2 and upstream */
    Check(t->Inputs == 2 && t->MaskAllowed == 0x03, "Twin X: 2 preamp inputs, StripMask clamped to 0x03 (spec 5.2)");
    Check(StripModelMask(t, 0xFF) == 0x03 && StripModelMask(t, 0x01) == 0x01 && StripModelMask(t, 0x02) == 0x02 &&
          StripModelMask(t, 0xFC) == 0 && StripModelMask(NULL, 0xFF) == 0, "Twin X: StripMask 0xff -> 0x03, 0xfc -> 0");
    Check(t->FirstVirtual == 0x18, "Twin X: VIRTUAL 1 = DSP input 0x18 (devices/apollo-twin-x.json, = Gen 2's entry)");
    Check(t->DspDefault == 1 && StripModelDsp(t, 1, 2) == 1, "Twin X: strips on DSP 1 (oa:driver/ua_strips.c:129-132)");
    Check(StripModelDsp(t, 0, 2) == 0 && StripModelDsp(t, 2, 2) == 1 && StripModelDsp(t, 3, 4) == 1 &&
          StripModelDsp(t, 2, 4) == 1 && StripModelDsp(t, 7, 2) == 1,
          "Twin X: StripDsp 0 or 1 only (a Duo's DSPs), anything else DSP 1");
    Check(StripRouteDsps(2, 1) == 0x01 && StripRouteDsps(4, 1) == 0x0D && StripRouteDsps(2, 0) == 0x02,
          "Twin X: route word to DSP 0 on a Duo (0, 2, 3 on a Quad; spec 5.2)");
    Check(t->LayoutStrips == 2, "Twin X: pools checked for its 2 strips");
    {
        static const ULONG upstream[5] = { 0x0EB, 0x0DA, 0x0A5, 0x0C2, 0x12B };   /* oa:driver/ua_strips.c:27-31 */

        ok = 1;
        for (i = 0; i < 5; i++)
            ok = ok && t->ModuleId[i] == upstream[i] && x->ModuleId[i] == upstream[i] &&
                 g_ApolloStripPins[i].Id == upstream[i];
        Check(ok, "both models: modules 0xEB 0xDA 0xA5 0xC2 0x12B in load order = the pins (no 0xC9 talkback, 0x83E input)");
    }
    Check(t->RecId[0] == 0x0001 && t->RecId[1] == 0x0002 && t->RecId[2] == 0,
          "Twin X: strip 1 -> 0x0001 MIC/LINE/HIZ 1, strip 2 -> 0x0002, nothing else");
    {
        unsigned long w[APOLLO_IODESC_WORDS];

        memset(w, 0, sizeof(w));
        w[0] = APOLLO_IODESC_VERSION;
        w[1] = APOLLO_IODESC_LENGTH;
        w[2] = 0xFFFFFFFFul;
        w[3] = 34;
        w[4] = 46;
        w[5] = 2;
        for (i = 0; i < 34; i++)
            w[APOLLO_IODESC_HDR + i / 2] |= (unsigned long)kTwxRecIds[i] << ((i & 1) ? 16 : 0);
        Check(ApolloIoDescFind(w, 1, t->RecId[0]) == 0 && ApolloIoDescFind(w, 1, t->RecId[1]) == 1,
              "Twin X G3 descriptor: strips 1-2 record on slots 0/1 (as the probe and the endpoints expect)");
        Check(ApolloIoDescFind(w, 1, 0x0B01) == 32, "Twin X G3 descriptor: TALKBACK 1 on slot 32 (no strip: silent)");
    }
    /* Ownership: the boot words the hardware logged */
    Check(StripOwned(0x00000003) && StripOwned(0x00002703) && StripOwned(0x00002203),
          "owned: 3 apart from the model byte (Twin X G3 cold 00002703 00000003, x8p 00002203)");
    Check(!StripOwned(0x0008000B) && !StripOwned(0x0008270B) && !StripOwned(0x0010220B) && !StripOwned(0) &&
          !StripOwned(0xFFFFFFFFu) && !StripOwned(0x00010003),
          "not owned: after a host (Twin X 0008270b 0008000b, x8p 0010220b), 0, all ones");
    /* The Twin X's DSP 1 pools at G3: the recipe's tops, room for its 2 strips (and for 8) */
    code = StripPlanLayoutCheck(kTwxSize, kTwxBase, kTwxScr, t->LayoutStrips, STRIP_CODE_WORDS);
    sprintf_s(what, sizeof(what), "Twin X DSP 1 pools (G3 cold start): fit its 2 strips (code %lu)", code);
    Check(code == 0, what);
    Check(StripPlanLayoutCheck(kTwxSize, kTwxBase, kTwxScr, 8, STRIP_CODE_WORDS) == 0,
          "Twin X DSP 1 pools: room for 8 too (the G3 log line)");
    {
        ULONG sz[4], bs[4], sc[4];

        memcpy(sz, kTwxSize, sizeof(sz));
        memcpy(bs, kTwxBase, sizeof(bs));
        memcpy(sc, kTwxScr, sizeof(sc));
        sz[1] += 2;
        Check(StripPlanLayoutCheck(sz, bs, sc, 2, STRIP_CODE_WORDS) == 5, "Twin X: pool 1 top moved -> refused (code 5)");
        memcpy(sz, kTwxSize, sizeof(sz));
        bs[0] = 0x000E0002;
        Check(StripPlanLayoutCheck(sz, bs, sc, 2, STRIP_CODE_WORDS) != 0, "Twin X: code pool elsewhere -> refused");
        memcpy(bs, kTwxBase, sizeof(bs));
        sc[2] = 0x1FF0;         /* 14 dwords left above the scratch, 2 strips need 0x20 */
        Check(StripPlanLayoutCheck(sz, bs, sc, 2, STRIP_CODE_WORDS) == 10, "Twin X: no room in pool 2 -> refused (code 10)");
        sc[2] = 0x80;
        sz[3] = 0xFFFFFFFFu;
        Check(StripPlanLayoutCheck(sz, bs, sc, 2, STRIP_CODE_WORDS) == 1, "Twin X: a register all ones -> gone (code 1)");
    }
    /* Its 2 strips' blocks inside the Twin X's measured pools, above the scratch */
    {
        ULONG a[STRIP_PLAN_ROWS], in, row, inside = 1;

        for (in = 0; in < t->Inputs; in++) {
            StripPlanAddrs(in, a);
            for (row = 0; row < STRIP_PLAN_ROWS; row++) {
                ULONG pool = g_StripRows[row].Pool & 3, lo = a[row], hi = lo + g_StripRows[row].Dwords;
                ULONG start = kTwxBase[pool] + (kTwxBase[pool] & 1), top = start + (kTwxSize[pool] & ~1u);
                ULONG s0 = kTwxScr[pool] + (kTwxScr[pool] & 1);

                if (s0 < kStripScratch[pool])
                    s0 = kStripScratch[pool];
                if (lo < start + s0 || hi > top || (lo & 1))
                    inside = 0;
            }
        }
        Check(inside, "Twin X: both strips' blocks inside its DSP 1 pools, above the scratch");
    }
    /* Its passes: the same words as the x8p's inputs 1-2, through the same stream checks */
    {
        static const ULONG twxMasks[] = { 0x01, 0x02, 0x03 };
        STRIP_PLAN p;
        ULONG st, mm;

        for (mm = 0; mm < 3; mm++) {
            for (st = 2; st <= STRIP_STEP_ALL; st++)
                CheckPass(StripModelMask(t, twxMasks[mm]), st);
            CheckPass(StripModelMask(t, twxMasks[mm]), 0);
        }
        Check(StripPlanStart(0x03, 0, &p) == 0 && p.N == 8 && p.Ent[0].W[1] == 0x00200000 &&
              p.Ent[0].W[2] == kAddr[0][0] && p.Ent[4].W[1] == 0x00200001 && p.Ent[4].W[2] == kAddr[1][0] &&
              p.Ent[7].W[2] == STRIP_INPUT_ROUTE && p.Ent[7].W[3] == STRIP_ROUTE_NONE,
              "Twin X START 0x03: SYNCH 0x00200000 at 0x9d1b0, SYNCH 0x00200001 at 0x9d15c, span/route each");
        Check(StripPassStep(0x03, 0, 0, &mm) == STRIP_XACT_PREPARE && mm == 0 &&
              StripPassStep(0x03, 0, 1, &mm) == STRIP_XACT_PREPARE && mm == 1 &&
              StripPassStep(0x03, 0, 2, &mm) == STRIP_XACT_START && StripPassStep(0x03, 0, 3, &mm) == STRIP_XACT_END,
              "Twin X pass 0x03: PREPARE 1, PREPARE 2, START, end");
        Check(StripPlanStop(0x03, &p) == 0 && StopWordsOk(0x03, &p) && ResynchOk(0x03),
              "Twin X: the teardown / probe words for 0x03 as the x8p's");
    }
}

static void PrintProbe(ULONG Mask)
{
    STRIP_PLAN p;

    if (StripPlanStop(Mask, &p) == 0) {
        printf("Probe repair, STOP StripMask 0x%02lx, on the strip DSP:\n", Mask);
        PrintPlan(&p);
    }
    if (StripPlanStart(Mask, 0, &p) == 0) {
        printf("Probe repair, START StripMask 0x%02lx, on the strip DSP:\n", Mask);
        PrintPlan(&p);
    }
    if (StripPlanSpans(&p) == 0) {
        printf("Probe repair, SPANS, on every live DSP:\n");
        PrintPlan(&p);
    }
}

/* ------------------------------------------------------------------ */
/* [Warm] warm-start adoption (strip_adopt.h, docs/STRIPS.md 12)       */
/* ------------------------------------------------------------------ */

/* Boot words the hardware logged (Nov 14-15 2026): x8p and Twin X before and after one connect of ours. */
static const ULONG kX8pCold[8]   = { 0x00002203, 3, 3, 3, 3, 3, 0, 0 };
static const ULONG kX8pOurs[8]   = { 0x0008220b, 0x0008000b, 0x0008000b, 0x0008000b, 0x0008000b, 0x0008000b, 0, 0 };
static const ULONG kX8pUa[8]     = { 0x0010220b, 0x0010000b, 0x0010000b, 0x0010000b, 0x0010000b, 0x0010000b, 0, 0 };
static const ULONG kTwxCold[8]   = { 0x00002703, 3, 0, 0, 0, 0, 0, 0 };
static const ULONG kTwxOurs1[8]  = { 0x0008270b, 0x0008000b, 0, 0, 0, 0, 0, 0 };
static const ULONG kTwxOurs2[8]  = { 0x0010270b, 0x0010000b, 0, 0, 0, 0, 0, 0 };
static const ULONG kTwxOurs3[8]  = { 0x0018270b, 0x0018000b, 0, 0, 0, 0, 0, 0 };
static const ULONG kX8pGeom[11]  = { 0x00004000, 0x0000a9b9, 0x00001fff, 0x00ff516a,
                                     0x000e0000, 0x00092800, 0x000c5000, 0x08008e96,
                                     0x00000080, 0x00000080, 0x00008000 };

/* The x8p plan Aaron runs: StripMask 0xFF, StripSteps 9, 48 kHz, DSP 1 of 6. Pinned: a change to any word
   of the pass (or of StripPlanHash) changes it, and every record written before would stop matching. */
#define ADOPT_X8P_PLAN_HASH     0x209A8BDBu     /* strip-plan-test --adopt-hash, Nov 15 2026 */

static ULONG AdoptModulesHash(void)
{
    ULONG h = STRIP_FNV_INIT, j, k;

    /* the same as strips.c StripsModulesHash */
    for (j = 0; j < APOLLO_STRIP_MODULES; j++) {
        const APOLLO_STRIP_PIN *p = &g_ApolloStripPins[j];

        h = StripFnv(h, p->Id);
        h = StripFnv(h, p->Length);
        h = StripFnv(h, p->W4);
        h = StripFnv(h, p->PoolWords);
        h = StripFnv(h, p->Dest);
        for (k = 0; k + 3 < sizeof(p->Sha256); k += 4)
            h = StripFnv(h, (ULONG)p->Sha256[k] | ((ULONG)p->Sha256[k + 1] << 8) |
                            ((ULONG)p->Sha256[k + 2] << 16) | ((ULONG)p->Sha256[k + 3] << 24));
    }
    return h;
}

/* What strips.c does when a pass ends Live (StripsRecLive), on a copy. */
static void AdoptLive(STRIP_RECORD *R, ULONG Mask, ULONG Limit, ULONG Rate, ULONG Dsp, ULONG NumDsps, int Keep,
                      const ULONG *Geom)
{
    static STRIP_PLAN p;
    ULONG i;

    R->State = STRIP_REC_RUNNING;
    R->Dsp = Dsp;
    R->Mask = Mask;
    R->Rate = Rate;
    R->Limit = Limit;
    R->ModuleHash = AdoptModulesHash();
    R->PlanHash = StripPlanHash(Mask, Limit, Rate, Dsp, NumDsps, &p);
    StripPlanHeaders(Mask, R->Header);
    for (i = 0; i < 11; i++)
        R->Geom[i] = Geom[i];
    R->Keep = Keep ? 1u : 0u;
    R->BuiltDate = (7258u << 12) + 600u;
    R->Adoptions = 0;
}

/* This start's side of the decision, as strips.c StripsAdoptDecideNow builds it. */
static void AdoptNow(STRIP_ADOPT_NOW *N, ULONG Anchor, int ConnectsOnWarm, int Adopt, int Rebuild, ULONG Mask,
                     ULONG Limit, ULONG Rate, ULONG Dsp, ULONG NumDsps, const ULONG *Geom)
{
    static STRIP_PLAN p;
    ULONG i;

    memset(N, 0, sizeof(*N));
    N->Anchor = Anchor;
    N->ConnectsOnWarm = ConnectsOnWarm ? 1u : 0u;
    N->Adopt = Adopt ? 1u : 0u;
    N->WarmRebuild = Rebuild ? 1u : 0u;
    N->Dsp = Dsp;
    N->Mask = Mask;
    N->Rate = Rate;
    N->Limit = Limit;
    N->ModuleHash = AdoptModulesHash();
    N->PlanHash = StripPlanHash(Mask, Limit, Rate, Dsp, NumDsps, &p);
    N->LayoutOk = 1;
    for (i = 0; i < 11; i++)
        N->Geom[i] = Geom[i];
}

/* One start of a simulated unit, as strips.c runs it: the anchor, the record after the bring-up. */
static ULONG AdoptStart(int Cold, int Connected, const ULONG *Pre, const ULONG *Post, ULONG N, STRIP_RECORD *Rec,
                        int *HaveRec, int *Wrote)
{
    STRIP_RECORD next;
    ULONG anchor;
    int valid = *HaveRec && StripRecordValid(Rec, sizeof(*Rec));

    anchor = StripAnchor(Cold, Connected, Pre, Post, N, valid ? Rec : NULL,
                         !*HaveRec ? STRIP_ANCHOR_NO_RECORD : valid ? STRIP_ANCHOR_OK : STRIP_ANCHOR_BAD_RECORD);
    StripRecordAfterBringup(Cold, Connected, valid ? Rec : NULL, anchor, Post, N, &next);
    *Wrote = !(valid && StripRecordSameContent(&next, Rec));
    if (*Wrote) {
        next.Writes = valid ? Rec->Writes + 1 : 1;
        StripRecordSeal(&next);
        *Rec = next;
        *HaveRec = 1;
    }
    return anchor;
}

static void CheckAdopt(void)
{
    STRIP_RECORD r, r2;
    STRIP_ADOPT_NOW n;
    static STRIP_PLAN p;
    ULONG h, h2, a, v, hdr[8], i;
    int td, have, wrote;
    char what[200];

    /* the record: layout, seal, every way it stops being trusted */
    Check(sizeof(STRIP_RECORD) == 200, "adopt: the record is 50 words (200 bytes)");
    Check(STRIP_REC_DSPS == STRIP_DSP_ROUTE_MAX && STRIP_REC_INPUTS == STRIP_PLAN_INPUTS &&
          STRIP_REC_MODULES == APOLLO_STRIP_MODULES, "adopt: record array sizes = DSPs, inputs, modules");
    memset(&r, 0, sizeof(r));
    r.Anchored = 1;
    r.State = STRIP_REC_RUNNING;
    StripRecordSeal(&r);
    Check(StripRecordValid(&r, sizeof(r)), "adopt: a sealed record is valid");
    Check(!StripRecordValid(&r, sizeof(r) - 4) && !StripRecordValid(&r, sizeof(r) + 4),
          "adopt: a record of another size is not");
    for (i = 0; i < sizeof(r); i += 13) {
        r2 = r;
        ((unsigned char *)&r2)[i] ^= 0x20;
        if (StripRecordValid(&r2, sizeof(r2))) {
            sprintf_s(what, sizeof(what), "adopt: a record with byte %lu changed is not valid", i);
            Check(0, what);
        }
    }
    r2 = r;
    r2.Version = STRIP_REC_VERSION + 1;
    r2.Check = StripRecordSum(&r2);
    Check(!StripRecordValid(&r2, sizeof(r2)), "adopt: another record version is not trusted");
    r2 = r;
    r2.State = 3;
    r2.Check = StripRecordSum(&r2);
    Check(!StripRecordValid(&r2, sizeof(r2)), "adopt: a state beyond UNKNOWN is not trusted");
    r2 = r;
    r2.Writes++;
    Check(!StripRecordValid(&r2, sizeof(r2)) && StripRecordSameContent(&r, &r2),
          "adopt: Writes is sealed, but not content");

    /* the plan's identity */
    h = StripPlanHash(0xFF, 9, 48000, 1, 6, &p);
    Check(h != 0 && h == StripPlanHash(0xFF, 9, 48000, 1, 6, &p), "adopt: the plan hash is stable");
    Check(h != StripPlanHash(0x7F, 9, 48000, 1, 6, &p), "adopt: StripMask changes it");
    Check(h != StripPlanHash(0xFF, 7, 48000, 1, 6, &p), "adopt: the stage changes it");
    Check(h != StripPlanHash(0xFF, 9, 96000, 1, 6, &p), "adopt: the rate changes it (other parameter words)");
    Check(h != StripPlanHash(0xFF, 9, 48000, 2, 6, &p), "adopt: the strip DSP changes it");
    Check(h != StripPlanHash(0xFF, 9, 48000, 1, 2, &p), "adopt: the route DSPs change it");
    Check(StripPlanHash(0xFF, 9, 47999, 1, 6, &p) == 0, "adopt: a rate without strip words: 0 (never matches)");
    h2 = StripPlanHash(0x03, 9, 48000, 1, 2, &p);
    Check(h2 != 0 && h2 != h, "adopt: the Twin X plan (0x03 on a Duo) has its own hash");
    if (ADOPT_X8P_PLAN_HASH != 0) {
        sprintf_s(what, sizeof(what), "adopt: the x8p plan hash is the pinned %08lx (got %08lx)",
                  (ULONG)ADOPT_X8P_PLAN_HASH, h);
        Check(h == ADOPT_X8P_PLAN_HASH, what);
    }
    StripPlanHeaders(0xA5, hdr);
    Check(hdr[0] == kAddr[0][0] && hdr[1] == 0 && hdr[2] == kAddr[2][0] && hdr[5] == kAddr[5][0] &&
          hdr[7] == kAddr[7][0] && hdr[6] == 0, "adopt: headers of 0xA5 = the planner's, 0 elsewhere");

    /* boot words */
    Check(StripBootAllUntouched(kX8pCold, 6) && StripBootAllUntouched(kTwxCold, 2),
          "adopt: the logged cold boot words are untouched (x8p 6 DSPs, Twin X 2)");
    Check(!StripBootAllUntouched(kX8pOurs, 6) && !StripBootAllUntouched(kX8pUa, 6),
          "adopt: connected boot words are not");
    Check(StripBootAdvanced(kX8pCold, kX8pOurs, 6) && StripBootAdvanced(kTwxOurs1, kTwxOurs2, 2),
          "adopt: our connect moves every word (x8p cold; Twin X warm)");
    Check(!StripBootAdvanced(kTwxOurs2, kTwxOurs2, 2), "adopt: a counter that did not move is caught");
    {
        ULONG dead[8] = { 0xFFFFFFFF, 0xFFFFFFFF, 0, 0, 0, 0, 0, 0 };

        Check(!StripBootAdvanced(kTwxOurs1, dead, 2), "adopt: all-ones after the connect is not an advance");
    }

    /* the anchor */
    memset(&r, 0, sizeof(r));
    memcpy(r.BootWord, kX8pOurs, sizeof(r.BootWord));
    r.Anchored = 1;
    Check(StripAnchor(1, 1, kX8pCold, kX8pOurs, 6, NULL, STRIP_ANCHOR_NO_RECORD) == STRIP_ANCHOR_OK,
          "anchor: x8p cold start + our connect: anchored, whatever record");
    Check(StripAnchor(1, 1, kX8pOurs, kX8pUa, 6, NULL, STRIP_ANCHOR_NO_RECORD) == STRIP_ANCHOR_COLD_USED,
          "anchor: a cold start whose DSPs a host used: not");
    Check(StripAnchor(1, 0, kX8pCold, kX8pCold, 6, NULL, STRIP_ANCHOR_NO_RECORD) == STRIP_ANCHOR_OK,
          "anchor: a cold start whose connect failed (no connect): anchored, the words stay 3");
    Check(StripAnchor(1, 1, kX8pCold, kX8pCold, 6, NULL, STRIP_ANCHOR_NO_RECORD) == STRIP_ANCHOR_NO_ADVANCE,
          "anchor: a connect that left the words: not");
    Check(StripAnchor(0, 0, kX8pOurs, kX8pOurs, 6, &r, STRIP_ANCHOR_OK) == STRIP_ANCHOR_OK,
          "anchor: x8p warm, record anchored, words as our connect left them: anchored");
    Check(StripAnchor(0, 0, kX8pUa, kX8pUa, 6, &r, STRIP_ANCHOR_OK) == STRIP_ANCHOR_OTHER_HOST,
          "anchor: x8p warm after UA's driver connected (0010220b): another host");
    Check(StripAnchor(0, 0, kX8pOurs, kX8pOurs, 6, NULL, STRIP_ANCHOR_NO_RECORD) == STRIP_ANCHOR_NO_RECORD,
          "anchor: warm, no record: unsigned");
    Check(StripAnchor(0, 0, kX8pOurs, kX8pOurs, 6, NULL, STRIP_ANCHOR_BAD_RECORD) == STRIP_ANCHOR_BAD_RECORD &&
          StripAnchor(0, 0, kX8pOurs, kX8pOurs, 6, NULL, STRIP_ANCHOR_OTHER_UNIT) == STRIP_ANCHOR_OTHER_UNIT,
          "anchor: warm, a bad or foreign record: its reason");
    r.Anchored = 0;
    Check(StripAnchor(0, 0, kX8pOurs, kX8pOurs, 6, &r, STRIP_ANCHOR_OK) == STRIP_ANCHOR_LOST_BEFORE,
          "anchor: warm, a record that lost its anchor: never back until a cold start");
    Check(StripAnchor(0, 0, kX8pOurs, kX8pOurs, 0, &r, STRIP_ANCHOR_OK) == STRIP_ANCHOR_NOT_READ &&
          StripAnchor(0, 0, kX8pOurs, kX8pOurs, 9, &r, STRIP_ANCHOR_OK) == STRIP_ANCHOR_NOT_READ,
          "anchor: 0 or more than 8 DSPs: not read");
    memset(&r, 0, sizeof(r));
    memcpy(r.BootWord, kTwxOurs1, sizeof(r.BootWord));
    r.Anchored = 1;
    Check(StripAnchor(0, 1, kTwxOurs1, kTwxOurs2, 2, &r, STRIP_ANCHOR_OK) == STRIP_ANCHOR_OK,
          "anchor: Twin X warm: words as our last connect left them, and ours moved them");
    Check(StripAnchor(0, 1, kTwxOurs1, kTwxOurs1, 2, &r, STRIP_ANCHOR_OK) == STRIP_ANCHOR_NO_ADVANCE,
          "anchor: Twin X warm: a saturated counter ends the proof");
    Check(StripAnchor(0, 1, kTwxOurs2, kTwxOurs1, 2, &r, STRIP_ANCHOR_OK) == STRIP_ANCHOR_OTHER_HOST,
          "anchor: Twin X warm: another connect since ours");
    {
        /* Nov 15 2026 on hardware: our own D0 exit turns the session byte 0x0B into 0x07, the count stays */
        static const ULONG x8pExited[8] = { 0x00082207, 0x00080007, 0x00080007, 0x00080007, 0x00080007, 0x00080007, 0, 0 };
        static const ULONG x8pUaExited[8] = { 0x00102207, 0x00100007, 0x00100007, 0x00100007, 0x00100007, 0x00100007, 0, 0 };
        static const ULONG twxExited1[8] = { 0x00082707, 0x00080007, 0, 0, 0, 0, 0, 0 };
        static const ULONG twxExited3[8] = { 0x00182707, 0x00180007, 0, 0, 0, 0, 0, 0 };

        Check(StripAnchor(0, 1, twxExited1, kTwxOurs2, 2, &r, STRIP_ANCHOR_OK) == STRIP_ANCHOR_OK,
              "anchor: Twin X warm after our own D0 exit (session byte 0x07): anchored, ours moved the count");
        Check(!StripBootAdvanced(twxExited3, kTwxOurs3, 2),
              "anchor: only the session byte moving (07 -> 0b) is not a count advance");
        memset(&r, 0, sizeof(r));
        memcpy(r.BootWord, kX8pOurs, sizeof(r.BootWord));
        r.Anchored = 1;
        Check(StripAnchor(0, 0, x8pExited, x8pExited, 6, &r, STRIP_ANCHOR_OK) == STRIP_ANCHOR_OK,
              "anchor: x8p warm after our own D0 exit (00082207, the record 0008220b): anchored");
        Check(StripAnchor(0, 0, x8pUaExited, x8pUaExited, 6, &r, STRIP_ANCHOR_OK) == STRIP_ANCHOR_OTHER_HOST,
              "anchor: x8p warm after UA's driver connected and left (00102207): another host");
    }

    /* the decision table (x8p plan, record RUNNING and kept) */
    memset(&r, 0, sizeof(r));
    r.Anchored = 1;
    AdoptLive(&r, 0xFF, 9, 48000, 1, 6, 1, kX8pGeom);
    AdoptNow(&n, STRIP_ANCHOR_OK, 0, 1, 0, 0xFF, 9, 48000, 1, 6, kX8pGeom);
    Check(StripAdoptDecide(&r, &n, &td) == STRIP_ADOPT_ADOPTED && !td, "decide: the same plan, kept: ADOPTED");
    /* [Warm] 12.5: plug-ins on the adopted strip DSP with StripWarmPlugins (x8p default 1), no StripWarmRebuild needed */
    Check(n.WarmRebuild == 0 && StripWarmPluginsOk(StripAdoptDecide(&r, &n, &td), r.Plugins, 1) &&
          !StripWarmPluginsOk(STRIP_ADOPT_ADOPTED, 0, 0), "plug-ins: ADOPTED, none left, StripWarmPlugins 1: allowed (0: not)");
    r.Plugins = 1;
    Check(StripAdoptDecide(&r, &n, &td) == STRIP_ADOPT_RESTORE && !td, "decide: a plug-in was there: RESTORE");
    Check(!StripWarmPluginsOk(STRIP_ADOPT_RESTORE, 1, 1) && !StripWarmPluginsOk(STRIP_ADOPT_ADOPTED, 1, 1),
          "plug-ins: RESTORE (a chain may have sat there): a power cycle first");
    r.Plugins = STRIP_REC_PLUGINS_STALE;            /* restored at an earlier warm start, its modules perhaps still there */
    Check(StripAdoptDecide(&r, &n, &td) == STRIP_ADOPT_ADOPTED && !td && !StripWarmPluginsOk(STRIP_ADOPT_ADOPTED, r.Plugins, 1),
          "decide: the stale mark alone: ADOPTED (plain strips), plug-ins still wait for a power cycle");
    r.Plugins = STRIP_REC_PLUGINS_STALE | 1;
    Check(StripAdoptDecide(&r, &n, &td) == STRIP_ADOPT_RESTORE, "decide: the stale mark and a chain counted: RESTORE");
    Check(StripWarmPluginsOk(STRIP_ADOPT_REBUILD, 0, 1) && !StripWarmPluginsOk(STRIP_ADOPT_NOT_ANCHORED, 0, 1) &&
          !StripWarmPluginsOk(STRIP_ADOPT_TORN_DOWN, 0, 1) && !StripWarmPluginsOk(STRIP_ADOPT_PLAN_DIFFERS, 0, 1),
          "plug-ins: a warm rebuild yes; not anchored, torn down or another plan: no");
    r.Plugins = 0;
    n.Adopt = 0;
    Check(StripAdoptDecide(&r, &n, &td) == STRIP_ADOPT_DISABLED && !td, "decide: StripAdopt 0: DISABLED, nothing sent");
    /* Nov 16 2026: adopted x8p strips recorded exact zero after a warm restart; a rebuild wins whenever allowed */
    n.WarmRebuild = 1;
    Check(StripAdoptDecide(&r, &n, &td) == STRIP_ADOPT_REBUILD && td,
          "decide: StripAdopt 0 with rebuilds: REBUILD after a teardown");
    n.Adopt = 1;
    Check(StripAdoptDecide(&r, &n, &td) == STRIP_ADOPT_REBUILD && td,
          "decide: the same plan, kept, with rebuilds (the default): REBUILD after a teardown, never adopted");
    Check(StripWarmPluginsOk(STRIP_ADOPT_REBUILD, r.Plugins, 1), "plug-ins: that rebuild, none left: allowed");
    r.Plugins = 1;
    Check(StripAdoptDecide(&r, &n, &td) == STRIP_ADOPT_REBUILD && td && !StripWarmPluginsOk(STRIP_ADOPT_REBUILD, r.Plugins, 1),
          "decide: a chain was there, with rebuilds: REBUILD after a teardown, plug-ins wait for a power cycle");
    r.Plugins = 0;
    n.WarmRebuild = 0;
    r.Keep = 0;
    Check(StripAdoptDecide(&r, &n, &td) == STRIP_ADOPT_TORN_DOWN && !td, "decide: not kept at exit: TORN_DOWN");
    n.WarmRebuild = 1;
    Check(StripAdoptDecide(&r, &n, &td) == STRIP_ADOPT_REBUILD && td, "decide: ... with rebuilds: REBUILD after a teardown");
    n.WarmRebuild = 0;
    r.Keep = 1;
    AdoptNow(&n, STRIP_ANCHOR_OK, 0, 1, 0, 0x03, 9, 48000, 1, 6, kX8pGeom);
    Check(StripAdoptDecide(&r, &n, &td) == STRIP_ADOPT_PLAN_DIFFERS, "decide: StripMask changed: PLAN_DIFFERS");
    n.WarmRebuild = 1;
    Check(StripAdoptDecide(&r, &n, &td) == STRIP_ADOPT_REBUILD && td, "decide: ... with rebuilds: REBUILD after a teardown");
    AdoptNow(&n, STRIP_ANCHOR_OK, 0, 1, 0, 0xFF, 9, 96000, 1, 6, kX8pGeom);
    Check(StripAdoptDecide(&r, &n, &td) == STRIP_ADOPT_PLAN_DIFFERS, "decide: another rate: PLAN_DIFFERS");
    AdoptNow(&n, STRIP_ANCHOR_OK, 0, 1, 0, 0xFF, 7, 48000, 1, 6, kX8pGeom);
    Check(StripAdoptDecide(&r, &n, &td) == STRIP_ADOPT_PLAN_DIFFERS, "decide: another stage: PLAN_DIFFERS");
    AdoptNow(&n, STRIP_ANCHOR_OK, 0, 1, 0, 0xFF, 9, 48000, 1, 6, kX8pGeom);
    n.ModuleHash ^= 1;
    Check(StripAdoptDecide(&r, &n, &td) == STRIP_ADOPT_PLAN_DIFFERS, "decide: other module pins: PLAN_DIFFERS");
    AdoptNow(&n, STRIP_ANCHOR_OK, 0, 1, 1, 0xFF, 9, 48000, 1, 6, kX8pGeom);
    n.Geom[7] += 2;
    Check(StripAdoptDecide(&r, &n, &td) == STRIP_ADOPT_POOLS_DIFFER && !td,
          "decide: pools moved: POOLS_DIFFER, even with rebuilds");
    AdoptNow(&n, STRIP_ANCHOR_OK, 0, 1, 1, 0xFF, 9, 48000, 1, 6, kX8pGeom);
    n.LayoutOk = 0;
    Check(StripAdoptDecide(&r, &n, &td) == STRIP_ADOPT_LAYOUT && !td, "decide: pools off the recipe: LAYOUT first");
    for (a = STRIP_ANCHOR_COLD_USED; a <= STRIP_ANCHOR_NOT_READ; a++) {
        AdoptNow(&n, a, 0, 1, 1, 0xFF, 9, 48000, 1, 6, kX8pGeom);
        v = StripAdoptDecide(&r, &n, &td);
        sprintf_s(what, sizeof(what), "decide: anchor %lu (%s): NOT_ANCHORED even with rebuilds (got %lu)", a,
                  StripAnchorText(a), v);
        Check(v == STRIP_ADOPT_NOT_ANCHORED && !td, what);
    }
    AdoptNow(&n, STRIP_ANCHOR_OK, 0, 1, 1, 0xFF, 9, 48000, 1, 6, kX8pGeom);
    Check(StripAdoptDecide(NULL, &n, &td) == STRIP_ADOPT_NOT_ANCHORED, "decide: no record: NOT_ANCHORED");
    r.State = STRIP_REC_NONE;
    n.WarmRebuild = 0;
    Check(StripAdoptDecide(&r, &n, &td) == STRIP_ADOPT_NOT_RUNNING, "decide: nothing of ours runs: NOT_RUNNING");
    n.WarmRebuild = 1;
    Check(StripAdoptDecide(&r, &n, &td) == STRIP_ADOPT_REBUILD && !td, "decide: ... with rebuilds: REBUILD, no teardown");
    r.State = STRIP_REC_UNKNOWN;
    n.WarmRebuild = 0;
    Check(StripAdoptDecide(&r, &n, &td) == STRIP_ADOPT_UNKNOWN_STATE, "decide: ended mid-pass: UNKNOWN_STATE");
    n.WarmRebuild = 1;
    Check(StripAdoptDecide(&r, &n, &td) == STRIP_ADOPT_REBUILD && td, "decide: ... with rebuilds: REBUILD after a teardown");
    r.State = STRIP_REC_RUNNING;
    AdoptNow(&n, STRIP_ANCHOR_OK, 1, 1, 0, 0x03, 9, 48000, 1, 2, kX8pGeom);
    Check(StripAdoptDecide(&r, &n, &td) == STRIP_ADOPT_DROPPED, "decide: Twin X (connects warm), no rebuilds: DROPPED");
    n.WarmRebuild = 1;
    Check(StripAdoptDecide(&r, &n, &td) == STRIP_ADOPT_REBUILD && !td,
          "decide: Twin X with rebuilds: REBUILD, no teardown (its connect dropped them)");
    for (v = 0; v <= STRIP_ADOPT_COUNT; v++)
        Check(StripAdoptText(v)[0] != 0 && StripAnchorText(v)[0] != 0, "adopt: every verdict and anchor has a text");

    /* ---- lifecycles, start by start, as strips.c runs them ---- */

    /* x8p: power-up + cold build, then driver updates / PC restarts / disable-enable (no connect, warm) */
    memset(&r, 0, sizeof(r));
    have = 0;
    a = AdoptStart(1, 1, kX8pCold, kX8pOurs, 6, &r, &have, &wrote);
    Check(a == STRIP_ANCHOR_OK && wrote && r.Anchored && r.State == STRIP_REC_NONE &&
          memcmp(r.BootWord, kX8pOurs, sizeof(kX8pOurs)) == 0, "x8p life: cold start: anchored, nothing yet, our words");
    AdoptLive(&r, 0xFF, 9, 48000, 1, 6, 1, kX8pGeom);
    r.Writes++;
    StripRecordSeal(&r);
    for (i = 0; i < 3; i++) {
        a = AdoptStart(0, 0, kX8pOurs, kX8pOurs, 6, &r, &have, &wrote);
        AdoptNow(&n, a, 0, 1, 0, 0xFF, 9, 48000, 1, 6, kX8pGeom);
        v = StripAdoptDecide(&r, &n, &td);
        sprintf_s(what, sizeof(what), "x8p life: warm start %lu: anchored, nothing written, ADOPTED (anchor %lu, wrote %d, %lu)",
                  i + 1, a, wrote, v);
        Check(a == STRIP_ANCHOR_OK && !wrote && v == STRIP_ADOPT_ADOPTED, what);
        r.Adoptions++;
        r.Writes++;
        StripRecordSeal(&r);
    }
    /* UA's driver ran in between (it connects: 0010220b): refused now, and on every later warm start */
    a = AdoptStart(0, 0, kX8pUa, kX8pUa, 6, &r, &have, &wrote);
    AdoptNow(&n, a, 0, 1, 1, 0xFF, 9, 48000, 1, 6, kX8pGeom);
    Check(a == STRIP_ANCHOR_OTHER_HOST && wrote && !r.Anchored && StripAdoptDecide(&r, &n, &td) == STRIP_ADOPT_NOT_ANCHORED,
          "x8p life: after UA's driver: another host, the anchor is gone (even with rebuilds)");
    a = AdoptStart(0, 0, kX8pUa, kX8pUa, 6, &r, &have, &wrote);
    Check(a == STRIP_ANCHOR_LOST_BEFORE, "x8p life: ... and stays gone at the next warm start");
    a = AdoptStart(1, 1, kX8pCold, kX8pOurs, 6, &r, &have, &wrote);
    Check(a == STRIP_ANCHOR_OK && r.Anchored && r.State == STRIP_REC_NONE, "x8p life: a power cycle anchors it again");

    /* x8p: the first install of this build over strips an older build left (no record): unsigned */
    memset(&r, 0, sizeof(r));
    have = 0;
    a = AdoptStart(0, 0, kX8pOurs, kX8pOurs, 6, &r, &have, &wrote);
    AdoptNow(&n, a, 0, 1, 1, 0xFF, 9, 48000, 1, 6, kX8pGeom);
    Check(a == STRIP_ANCHOR_NO_RECORD && wrote && !r.Anchored && r.State == STRIP_REC_UNKNOWN &&
          StripAdoptDecide(have ? &r : NULL, &n, &td) == STRIP_ADOPT_NOT_ANCHORED,
          "x8p life: first install over an older build's strips: unsigned, a power cycle once");

    /* Twin X: every start connects; a cold build, then warm rebuilds while our connects keep counting */
    memset(&r, 0, sizeof(r));
    have = 0;
    a = AdoptStart(1, 1, kTwxCold, kTwxOurs1, 2, &r, &have, &wrote);
    Check(a == STRIP_ANCHOR_OK && r.Anchored, "Twin X life: cold start anchored");
    AdoptLive(&r, 0x03, 9, 48000, 1, 2, 0, kX8pGeom);
    r.Writes++;
    StripRecordSeal(&r);
    a = AdoptStart(0, 1, kTwxOurs1, kTwxOurs2, 2, &r, &have, &wrote);
    AdoptNow(&n, a, 1, 1, 1, 0x03, 9, 48000, 1, 2, kX8pGeom);
    v = StripAdoptDecide(&r, &n, &td);
    Check(a == STRIP_ANCHOR_OK && wrote && r.State == STRIP_REC_NONE && v == STRIP_ADOPT_REBUILD && !td &&
          memcmp(r.BootWord, kTwxOurs2, sizeof(kTwxOurs2)) == 0,
          "Twin X life: warm start: anchored, the connect dropped them, REBUILD; the record has the new words");
    a = AdoptStart(0, 1, kTwxOurs2, kTwxOurs2, 2, &r, &have, &wrote);
    Check(a == STRIP_ANCHOR_NO_ADVANCE && !r.Anchored,
          "Twin X life: if its counter ever stops moving, the proof ends there (a power cycle)");
    memset(&r, 0, sizeof(r));
    memcpy(r.BootWord, kTwxOurs1, sizeof(r.BootWord));
    r.Anchored = 1;
    r.NumDsps = 2;
    StripRecordSeal(&r);
    have = 1;
    a = AdoptStart(0, 1, kTwxOurs2, kTwxOurs3, 2, &r, &have, &wrote);
    Check(a == STRIP_ANCHOR_OTHER_HOST, "Twin X life: a connect by someone else between ours: refused");
}

/* ------------------------------------------------------------------ */
/* [Virtual] Virtual Input Strips (strip_plan.h, docs/STRIPS.md 13)    */
/* ------------------------------------------------------------------ */

/*
 * open-apollo's builder as written there, recording what it puts into a transaction, so the planner's words
 * can be compared with upstream's word for word: oa:driver/ua_strips.c:372-383 and 394-412 (rows, m0),
 * 503-520 (ua_strip_priv_addrs), 555-601 (header, block, start), 713-745 (ua_strip_build_input), 990-1009
 * (the pass's kinds: every Mic strip, then the Input Strips), oa:driver/ua_plugin_upload.c ua_memspec_compile,
 * oa:driver/ua_plugin_host.h:319-324 (UA_CMD_ZERO, UA_CMD_MEMSPEC, UA_MEMSPEC_ROWS_MAX).
 */
#define OA_KIND_MIC             0
#define OA_KIND_INPUT           1
#define OA_CMD_ZERO             0x00080004u
#define OA_CMD_MEMSPEC          0x00150000u
#define OA_MEMSPEC_ROWS_MAX     128u
#define OA_XENTS                128u

typedef struct _OA_ROW {
    ULONG pool, dwords, link, flags;
} OA_ROW;

static const OA_ROW oa_strip_rows[10] = {
    { 1, 0x008, 0xffffffff, 0 }, { 1, 0x038, 0x00000006, 0 }, { 2, 0x010, 0x00000036, 0 },
    { 1, 0x010, 0x00000037, 0 }, { 3, 0x010, 0x0000001b, 0 }, { 1, 0x004, 0x0000001c, 0 },
    { 3, 0x800, 0x0000000e, 0 }, { 3, 0x800, 0x0000000f, 0 }, { 3, 0x800, 0x00000006, 0 },
    { 3, 0x800, 0x00000007, 0 },
};
static const OA_ROW oa_strip_input_rows[6] = {
    { 1, 0x008, 0xffffffff, 0 },        /* instance header */
    { 1, 0x01e, 0x00000006, 0 },        /* m0 -> header[6] */
    { 3, 0x100, 0x0000000e, 0 },        /* c0 -> m0[0x0e] */
    { 3, 0x100, 0x0000000f, 0 },        /* c1 -> m0[0x0f] */
    { 3, 0x100, 0x00000006, 0 },        /* c2 -> m0[0x06] */
    { 3, 0x100, 0x00000007, 0 },        /* c3 -> m0[0x07] */
};
static const unsigned int oa_strip_desc[2] = { 0, 1 };
static const ULONG oa_strip_input_m0[9] = {
    0x00df7fe4, 0x40000000, 0x00000001, 0x00000001, 0x00000004, 0x00000100,
    0x000000c8, 0x3a888889, 0x3b83126f,
};
static const ULONG oa_strip_tops[4] = { 0, 0x0009d1b8, 0x000c6ffe, 0x08ffe000 };

/* What a transaction got: inline words, or a DMA command's words (Len of them at Words[Off]). */
typedef struct _OA_XACT {
    ULONG N, Err, Used;
    ULONG Dma[OA_XENTS], W[OA_XENTS][4], Off[OA_XENTS], Len[OA_XENTS];
    ULONG Words[2048];
} OA_XACT;

static void oa_xact_inline(OA_XACT *x, ULONG a, ULONG b, ULONG c, ULONG d)
{
    if (x->N >= OA_XENTS) {
        x->Err = 1;
        return;
    }
    x->Dma[x->N] = 0;
    x->W[x->N][0] = a;
    x->W[x->N][1] = b;
    x->W[x->N][2] = c;
    x->W[x->N][3] = d;
    x->N++;
}

static void oa_strip_dma(OA_XACT *x, const ULONG *w, unsigned int n)
{
    if (x->N >= OA_XENTS || x->Used + n > 2048) {
        x->Err = 1;
        return;
    }
    x->Dma[x->N] = 1;
    x->Off[x->N] = x->Used;
    x->Len[x->N] = n;
    memcpy(&x->Words[x->Used], w, n * sizeof(ULONG));
    x->Used += n;
    x->N++;
}

static int oa_memspec_compile(const OA_ROW *rows, unsigned int n, const unsigned int *desc, unsigned int n_desc,
                              const ULONG *addr, ULONG *words)
{
    unsigned int c, j, m = 0, first, last, tgt_first, tgt_count, t;
    ULONG link, off;

    if (!rows || !desc || !addr || !words || !n || n > OA_MEMSPEC_ROWS_MAX || !n_desc || n_desc > n || desc[0])
        return -1;
    for (j = 0; j < n; j++)
        if (rows[j].pool < 1 || rows[j].pool > 3 || !rows[j].dwords || (rows[j].flags != 0 && rows[j].flags != 2) ||
            addr[j] == 0xffffffff || (addr[j] & 1) || (unsigned long long)addr[j] + rows[j].dwords > 0xffffffffULL)
            return -1;
    for (c = 1; c < n_desc; c++)
        if (desc[c] <= desc[c - 1] || desc[c] >= n)
            return -1;
    if (rows[0].link != 0xffffffff)
        return -1;
    words[m++] = OA_CMD_MEMSPEC | (2 * n - 1);
    for (c = 0; c < n_desc; c++) {
        first = desc[c];
        last = c + 1 < n_desc ? desc[c + 1] : n;
        for (j = first; j < last; j++) {
            link = rows[j].link;
            if (link == 0xffffffff) {
                if (j)
                    return -1;
                continue;
            }
            tgt_first = j == first ? 0 : first;
            tgt_count = j == first ? (n_desc > 1 ? desc[1] : n) : last - first;
            t = link >> 24;
            off = link & 0xffffff;
            if (t >= tgt_count || off >= rows[tgt_first + t].dwords)
                return -1;
            words[m++] = addr[tgt_first + t] + off;
            words[m++] = addr[j];
        }
    }
    return m == 2 * n - 1 ? (int)m : -1;
}

/* ua_strip_priv_addrs on the recipe's empty pools: strip after strip, each its kind's rows, top-down */
static void oa_strip_priv_addrs(const unsigned char *kinds, unsigned int k, ULONG addr[10])
{
    const OA_ROW *rows;
    ULONG cur[4];
    unsigned int p, j, s, n;

    for (p = 0; p < 4; p++)
        cur[p] = oa_strip_tops[p];
    for (s = 0; s <= k; s++) {
        rows = kinds[s] == OA_KIND_INPUT ? oa_strip_input_rows : oa_strip_rows;
        n = kinds[s] == OA_KIND_INPUT ? 6 : 10;
        for (j = 0; j < n; j++) {
            cur[rows[j].pool] -= rows[j].dwords;
            addr[j] = cur[rows[j].pool];
        }
    }
}

static void oa_strip_build_header(OA_XACT *x, ULONG hdr, ULONG plugin)
{
    const ULONG w[9] = { (0x14u << 16) | 9u, hdr, 0x24, 0x40000000, 1, 1, 0x22, 1, plugin };

    oa_strip_dma(x, w, 9);
}

static void oa_strip_build_block(OA_XACT *x, ULONG addr, const ULONG *v, unsigned int n)
{
    ULONG w[64];
    unsigned int j;

    w[0] = (0x14u << 16) | (2 + n);
    w[1] = addr;
    for (j = 0; j < n; j++)
        w[2 + j] = v[j];
    oa_strip_dma(x, w, 2 + n);
}

static int oa_strip_build_start(OA_XACT *x, ULONG hdr, ULONG synch, unsigned int steps)
{
    if (steps && steps <= 5)
        return (int)x->Err;
    oa_xact_inline(x, 0x001F0004, 0x20u << 16 | synch, hdr, 0);
    if (steps && steps <= 6)
        return (int)x->Err;
    oa_xact_inline(x, 0x001E0004, synch, 8, 0x20);
    oa_xact_inline(x, 0x001E0004, synch, 8, 0x20);
    oa_xact_inline(x, 0x001E0004, synch, 7, 0x1ffff);   /* ua_strip_route, input_strips_route default */
    return (int)x->Err;
}

static int oa_strip_build_input(OA_XACT *x, const ULONG addr[6], ULONG synch, unsigned int steps)
{
    ULONG w[2 * 6];
    unsigned int j;
    int ret;

    if (steps && steps <= 1)
        return (int)x->Err;
    for (j = 0; j < 6; j++)
        oa_xact_inline(x, OA_CMD_ZERO, addr[j], 0, oa_strip_input_rows[j].dwords);
    if (steps && steps <= 2)
        return (int)x->Err;
    ret = oa_memspec_compile(oa_strip_input_rows, 6, oa_strip_desc, 2, addr, w);
    if (ret < 0)
        return ret;
    oa_strip_dma(x, w, (unsigned int)ret);
    if (steps && steps <= 3)
        return (int)x->Err;
    oa_strip_build_header(x, addr[0], 0x83e);
    if (steps && steps <= 4)
        return (int)x->Err;
    oa_strip_build_block(x, addr[1], oa_strip_input_m0, 9);
    return oa_strip_build_start(x, addr[0], synch, steps);
}

/* Two plans say the same: error, addresses, entries 0..N-1 and payload 0..Len-1 (what lies past them is stale). */
static int PlanSame(const STRIP_PLAN *A, const STRIP_PLAN *B)
{
    return A->Err == B->Err && A->N == B->N && A->Len == B->Len && A->N <= STRIP_PLAN_ENT_SLOTS &&
           A->Len <= STRIP_PLAN_PAYLOAD_MAX && memcmp(A->Addr, B->Addr, sizeof(A->Addr)) == 0 &&
           memcmp(A->Ent, B->Ent, A->N * sizeof(A->Ent[0])) == 0 &&
           memcmp(A->Payload, B->Payload, A->Len * sizeof(A->Payload[0])) == 0;
}

/* Our StripSteps (1-9, 0 = all) as upstream's input_strips_steps: 1-6 the same, 7 and up everything (0). */
static unsigned int OaSteps(ULONG Steps)
{
    ULONG s = StripPlanSteps(Steps);

    return s >= STRIP_STEP_INPUT ? 0u : (unsigned int)s;
}

/* Does our plan p (a PREPARE) followed by START entries q[from..from+n) say exactly what x got? */
static int SameAsUpstream(const STRIP_PLAN *p, const STRIP_PLAN *q, ULONG From, ULONG N, const OA_XACT *x)
{
    ULONG i, k = 0;

    if (x->Err)
        return 0;
    for (i = 0; i < p->N; i++, k++) {
        const STRIP_PLAN_ENT *e = &p->Ent[i];

        if (k >= x->N || e->Dma != x->Dma[k])
            return 0;
        if (!e->Dma) {
            if (memcmp(e->W, x->W[k], sizeof(e->W)) != 0)
                return 0;
        } else if (e->Dwords != x->Len[k] || memcmp(&p->Payload[e->Off], &x->Words[x->Off[k]], e->Dwords * sizeof(ULONG)) != 0) {
            return 0;
        }
    }
    for (i = 0; i < N; i++, k++) {
        if (From + i >= q->N || k >= x->N || x->Dma[k] || q->Ent[From + i].Dma ||
            memcmp(q->Ent[From + i].W, x->W[k], sizeof(q->Ent[0].W)) != 0)
            return 0;
    }
    return k == x->N;
}

/*
 * The Input Strip of VIRTUAL V+1 in a pass of every Mic strip of a model with MicSlots preamps and the Virtual strips
 * of VMask (VIRTUAL 1..n contiguous from 1, where the fixed layout and upstream's packing agree): our PREPARE plus its
 * START entries against ua_strip_build_input at the address ua_strip_priv_addrs gives, at stage Steps.
 */
static int VirtualMatchesUpstream(ULONG MicSlots, ULONG VMask, ULONG V, ULONG Steps)
{
    static OA_XACT x;
    static STRIP_PLAN p, q;
    unsigned char kinds[16];
    ULONG addr[10], i, k = 0, slot = STRIP_SLOT_VIRTUAL0 + V, pm, from = 0, n;

    if (!(VMask & (1u << V)))
        return 0;
    for (i = 0; i < MicSlots; i++)
        kinds[k++] = OA_KIND_MIC;
    for (i = 0; i <= V; i++)
        if (VMask & (1u << i))
            kinds[k++] = OA_KIND_INPUT;
    oa_strip_priv_addrs(kinds, k - 1, addr);
    memset(&x, 0, sizeof(x));
    if (oa_strip_build_input(&x, addr, STRIP_VIRTUAL_SYNCH0 + V, OaSteps(Steps)) != 0)
        return 0;
    pm = StripPlanPm((1u << MicSlots) - 1, VMask);
    if (StripPlanPrepareSlot(slot, MicSlots, Steps, 48000, &p) != 0 || StripPlanStartPm(pm, MicSlots, Steps, &q) != 0)
        return 0;
    for (i = 0; i < 6; i++)
        if (StripPlanSteps(Steps) >= STRIP_STEP_ZERO && p.Addr[i] != addr[i])
            return 0;
    /* this strip's entries in the START: after those of every slot before it */
    n = (StripPlanSteps(Steps) >= STRIP_STEP_INPUT) ? 4 : (StripPlanSteps(Steps) >= STRIP_STEP_SYNCH) ? 1 : 0;
    for (i = 0; i < slot; i++)
        if (pm & (1u << i))
            from += n;
    return SameAsUpstream(&p, &q, from, n, &x);
}

/* The blocks of slot K (Mic rows 0-9 or the Input Strip's 0-5): row J's pool and size. */
static void SlotRow(ULONG K, ULONG J, ULONG *Pool, ULONG *Dwords)
{
    const STRIP_ROW *r = (K < STRIP_SLOT_VIRTUAL0) ? &g_StripRows[J] : &g_StripVRows[J];

    *Pool = r->Pool & 3;
    *Dwords = r->Dwords;
}

static ULONG SlotRows(ULONG K)
{
    return (K < STRIP_SLOT_VIRTUAL0) ? STRIP_PLAN_ROWS : STRIP_PLAN_VROWS;
}

/* Which strip of Pm holds Addr, and which row; 0 if none. */
static int RowOfPm(ULONG Pm, ULONG MicSlots, ULONG Addr, ULONG *Slot, ULONG *Row)
{
    ULONG k, j, a[STRIP_PLAN_ROWS], pool, dw;

    for (k = 0; k < STRIP_PLAN_SLOTS; k++) {
        if (!(Pm & (1u << k)) || StripPlanSlotAddrs(k, MicSlots, a) != 0)
            continue;
        for (j = 0; j < SlotRows(k); j++) {
            SlotRow(k, j, &pool, &dw);
            if (Addr >= a[j] && Addr - a[j] < dw) {
                *Slot = k;
                *Row = j;
                return 1;
            }
        }
    }
    return 0;
}

/* Is Addr a memspec link word of a strip of Pm (a block address the DSP follows)? */
static int IsLinkWordPm(ULONG Pm, ULONG MicSlots, ULONG Addr)
{
    ULONG k, j, a[STRIP_PLAN_ROWS];

    for (k = 0; k < STRIP_PLAN_SLOTS; k++) {
        const STRIP_ROW *rows = (k < STRIP_SLOT_VIRTUAL0) ? g_StripRows : g_StripVRows;

        if (!(Pm & (1u << k)) || StripPlanSlotAddrs(k, MicSlots, a) != 0)
            continue;
        for (j = 1; j < SlotRows(k); j++) {
            ULONG tgt = (j == 1) ? a[0] : a[1];

            if (Addr == tgt + (rows[j].Link & 0xFFFFFF))
                return 1;
        }
    }
    return 0;
}

/* The pass over Pm as apollo.sys runs it (StripPassStepPm), flattened into the stream the strip DSP sees. */
static int BuildPassPm(STREAM *S, ULONG Pm, ULONG MicSlots, ULONG Steps)
{
    static STRIP_PLAN p;
    ULONG k, in, kind;

    memset(S, 0, sizeof(*S));
    for (k = 0; k <= STRIP_PLAN_SLOTS + 1; k++) {
        kind = StripPassStepPm(Pm, Steps, k, &in);
        if (kind == STRIP_XACT_END)
            return 1;
        if (kind == STRIP_XACT_PREPARE) {
            if (StripPlanPrepareSlot(in, MicSlots, Steps, 48000, &p) != 0)
                return 0;
            EmitPlan(S, &p, in);
        } else if (kind == STRIP_XACT_START) {
            if (StripPlanStartPm(Pm, MicSlots, Steps, &p) != 0)
                return 0;
            EmitPlan(S, &p, 0xFFFFFFFFu);
        } else {
            return 0;
        }
    }
    return 0;
}

/* CheckPass's stream checks for a plan mask with Virtual strips (the SYNCH input is the slot's synch). */
static void CheckPassPm(ULONG Pm, ULONG MicSlots, ULONG Steps, const char *Who)
{
    STREAM *s = (STREAM *)calloc(1, sizeof(STREAM));
    ULONG i, n = 0, in, row, seen = 0, lastPrep = 0, firstSynch = 0xFFFFFFFFu, steps, a[STRIP_PLAN_ROWS];
    int ownOk = 1, linkOk = 1, synchOk = 1, inputOk = 1;
    char what[200];

    if (s == NULL) {
        Check(0, "out of memory");
        return;
    }
    steps = StripPlanSteps(Steps);
    for (i = 0; i < STRIP_PLAN_SLOTS; i++)
        n += (Pm >> i) & 1;
    sprintf_s(what, sizeof(what), "%s Pm 0x%04lx stage %lu: pass plans", Who, Pm, steps);
    Check(BuildPassPm(s, Pm, MicSlots, Steps) && !s->Overflow, what);
    for (i = 0; i < s->N; i++) {
        const EV *e = &s->Ev[i];

        if (e->Kind == EV_MEM) {
            ULONG in2, row2;

            lastPrep = i;
            if (e->Count == 0 || !RowOfPm(Pm, MicSlots, e->Addr, &in, &row) || in != e->Owner ||
                !RowOfPm(Pm, MicSlots, e->Addr + e->Count - 1, &in2, &row2) || in2 != in || row2 != row)
                ownOk = 0;
            if ((e->Param == SRC_MEMSPEC && !IsLinkWordPm(Pm, MicSlots, e->Addr)) ||
                (e->Param == SRC_WRITE && IsLinkWordPm(Pm, MicSlots, e->Addr)))
                linkOk = 0;
        } else if (e->Kind == EV_SYNCH) {
            ULONG k = StripBusSlot(e->Arg);

            if (firstSynch == 0xFFFFFFFFu)
                firstSynch = i;
            if (k >= STRIP_PLAN_SLOTS || !(Pm & (1u << k)) || (seen & (1u << k)) || e->Owner != 0xFFFFFFFFu ||
                StripPlanSlotAddrs(k, MicSlots, a) != 0 || e->Addr != a[0])
                synchOk = 0;
            seen |= 1u << (k < 32 ? k : 0);
        } else if (e->Kind == EV_INPUT) {
            ULONG k = StripBusSlot(e->Arg);

            if (k >= STRIP_PLAN_SLOTS || !(seen & (1u << k)) ||
                !((e->Param == STRIP_INPUT_SPAN && e->Value == STRIP_SPAN_NONE) ||
                  (e->Param == STRIP_INPUT_ROUTE && e->Value == STRIP_ROUTE_NONE)))
                inputOk = 0;
        } else {
            inputOk = 0;
        }
    }
    sprintf_s(what, sizeof(what), "%s Pm 0x%04lx stage %lu: %lu PREPAREs + one START", Who, Pm, steps, n);
    Check(s->Xacts == n + (steps >= STRIP_STEP_SYNCH && n ? 1u : 0u), what);
    sprintf_s(what, sizeof(what), "%s Pm 0x%04lx stage %lu: no strip memory written after a strip started", Who, Pm, steps);
    Check(NoWriteAfterStart(s) && (firstSynch == 0xFFFFFFFFu || lastPrep < firstSynch), what);
    sprintf_s(what, sizeof(what), "%s Pm 0x%04lx stage %lu: each PREPARE writes only its own rows", Who, Pm, steps);
    Check(ownOk, what);
    sprintf_s(what, sizeof(what), "%s Pm 0x%04lx stage %lu: memspec on link words only, no value on one", Who, Pm, steps);
    Check(linkOk, what);
    sprintf_s(what, sizeof(what), "%s Pm 0x%04lx stage %lu: every strip started once, own header and synch, in the START",
              Who, Pm, steps);
    Check(synchOk && seen == (steps >= STRIP_STEP_SYNCH ? Pm : 0u), what);
    sprintf_s(what, sizeof(what), "%s Pm 0x%04lx stage %lu: span/route words only after their SYNCH", Who, Pm, steps);
    Check(inputOk, what);
    free(s);
}

/* Every block of the strips of Pm: pairwise disjoint, inside the pools above the scratch, inside StripPlanRoom. */
static int BlocksOk(ULONG Pm, ULONG MicSlots, const ULONG *Size, const ULONG *Base, const ULONG *Scr, int *Inside, int *InRoom)
{
    ULONG a[STRIP_PLAN_SLOTS][STRIP_PLAN_ROWS], k, j, k2, j2, room[4], pool, dw, pool2, dw2;
    int ok = 1;

    *Inside = 1;
    *InRoom = 1;
    StripPlanRoom(MicSlots, (Pm & STRIP_PM_VIRTUAL) >> 8, room);
    for (k = 0; k < STRIP_PLAN_SLOTS; k++)
        if ((Pm & (1u << k)) && StripPlanSlotAddrs(k, MicSlots, a[k]) != 0)
            return 0;
    for (k = 0; k < STRIP_PLAN_SLOTS; k++) {
        if (!(Pm & (1u << k)))
            continue;
        for (j = 0; j < SlotRows(k); j++) {
            ULONG lo = a[k][j], hi, start, top, sc;

            SlotRow(k, j, &pool, &dw);
            hi = lo + dw;
            start = Base[pool] + (Base[pool] & 1);
            top = start + (Size[pool] & ~1u);
            sc = Scr[pool] + (Scr[pool] & 1);
            if (sc < kStripScratch[pool])
                sc = kStripScratch[pool];
            if (lo < start + sc || hi > top || (lo & 1))
                *Inside = 0;
            if (lo < g_StripTop[pool] - room[pool] || hi > g_StripTop[pool])
                *InRoom = 0;
            for (k2 = 0; k2 < STRIP_PLAN_SLOTS; k2++) {
                if (!(Pm & (1u << k2)))
                    continue;
                for (j2 = 0; j2 < SlotRows(k2); j2++) {
                    ULONG lo2 = a[k2][j2];

                    SlotRow(k2, j2, &pool2, &dw2);
                    if ((k2 != k || j2 != j) && lo < lo2 + dw2 && lo2 < hi)
                        ok = 0;
                }
            }
        }
    }
    return ok;
}

/* strip_adopt.h's validator before [Virtual] (version 1 only): what an older build does with a record. */
static int OldRecordValid(const STRIP_RECORD *R, ULONG Bytes)
{
    return Bytes == sizeof(*R) && R->Magic == STRIP_REC_MAGIC && R->Version == 1 &&
           R->Size == sizeof(*R) && R->Check == StripRecordSum(R) && R->NumDsps <= STRIP_REC_DSPS &&
           R->State <= STRIP_REC_UNKNOWN;
}

/* AdoptLive / AdoptNow (above) for a plan mask with Virtual strips, on the model's Mic room. */
static void AdoptLivePm(STRIP_RECORD *R, ULONG Pm, ULONG MicSlots, ULONG Limit, ULONG Rate, ULONG Dsp, ULONG NumDsps,
                        int Keep, const ULONG *Geom)
{
    static STRIP_PLAN p;

    AdoptLive(R, Pm & STRIP_PM_MIC, Limit, Rate, Dsp, NumDsps, Keep, Geom);
    R->Mask = Pm;
    R->PlanHash = StripPlanHashPm(Pm, MicSlots, Limit, Rate, Dsp, NumDsps, &p);
    StripPlanHeaders(Pm, R->Header);
}

static void AdoptNowPm(STRIP_ADOPT_NOW *N, ULONG Anchor, int Adopt, int Rebuild, ULONG Pm, ULONG MicSlots, ULONG Limit,
                       ULONG Rate, ULONG Dsp, ULONG NumDsps, const ULONG *Geom)
{
    static STRIP_PLAN p;

    AdoptNow(N, Anchor, 0, Adopt, Rebuild, Pm & STRIP_PM_MIC, Limit, Rate, Dsp, NumDsps, Geom);
    N->Mask = Pm;
    N->PlanHash = StripPlanHashPm(Pm, MicSlots, Limit, Rate, Dsp, NumDsps, &p);
}

/* VIRTUAL 1 on the x8p's layout (8 Mic strips), every word: docs/STRIPS.md 13.2 lists these. */
static const ULONG kV1Addr[6] = { 0x0009CF10, 0x0009CEF2, 0x08FEDE80, 0x08FEDD80, 0x08FEDC80, 0x08FEDB80 };
static const ULONG kV1Memspec[11] = {
    0x0015000B, 0x0009CF16, 0x0009CEF2, 0x0009CF00, 0x08FEDE80, 0x0009CF01, 0x08FEDD80,
    0x0009CEF8, 0x08FEDC80, 0x0009CEF9, 0x08FEDB80 };
static const ULONG kV1Header[9] = { 0x00140009, 0x0009CF10, 0x24, 0x40000000, 1, 1, 0x22, 1, 0x83E };
static const ULONG kV1Params[11] = {
    0x0014000B, 0x0009CEF2, 0x00df7fe4, 0x40000000, 1, 1, 4, 0x100, 0xc8, 0x3a888889, 0x3b83126f };
static const ULONG kV1Start[4][4] = {
    { 0x001F0004, 0x00200018, 0x0009CF10, 0 }, { 0x001E0004, 0x18, 8, 0x20 }, { 0x001E0004, 0x18, 8, 0x20 },
    { 0x001E0004, 0x18, 7, 0x0001FFFF } };

static void CheckVirtual(void)
{
    static STRIP_PLAN p, q;
    const STRIP_MODEL *x = StripModelFor(STRIP_MODEL_X8P), *t = StripModelFor(STRIP_MODEL_TWINX);
    ULONG i, j, k, m, st, sum[4] = { 0, 0, 0, 0 }, room[4], h, kind, kind2, in, in2, ok;
    int inside, inRoom, td;
    char what[200];
    STRIP_RECORD r, r2;
    STRIP_ADOPT_NOW n;

    /* the Input Strip's tables: upstream's, and the per-pool sums the room and the ledger use */
    for (j = 0; j < STRIP_PLAN_VROWS; j++) {
        sum[g_StripVRows[j].Pool & 3] += g_StripVRows[j].Dwords;
        ok = g_StripVRows[j].Pool == oa_strip_input_rows[j].pool && g_StripVRows[j].Dwords == oa_strip_input_rows[j].dwords &&
             g_StripVRows[j].Link == oa_strip_input_rows[j].link;
        sprintf_s(what, sizeof(what), "virtual: Input Strip row %lu = ua_strip_input_rows[%lu]", j, j);
        Check(ok, what);
    }
    Check(sum[1] == STRIP_VPOOL1_PER_STRIP && sum[2] == STRIP_VPOOL2_PER_STRIP && sum[3] == STRIP_VPOOL3_PER_STRIP &&
          STRIP_VPOOL1_PER_STRIP == 0x26 && STRIP_VPOOL3_PER_STRIP == 0x400,
          "virtual: rows per pool add up to 0x26 / 0 / 0x400 per Input Strip");
    Check(memcmp(g_StripVM0, oa_strip_input_m0, sizeof(oa_strip_input_m0)) == 0 && STRIP_VM0_WORDS == 9,
          "virtual: m0 words = ua_strip_input_m0 (9 words, 0.004f last)");
    Check(STRIP_PLUGIN_INPUT == 0x83E && STRIP_VIRTUAL_SYNCH0 == 0x18 && STRIP_PLAN_SLOTS == 16 &&
          STRIP_SLOT_VIRTUAL0 == STRIP_PLAN_INPUTS && STRIP_PLAN_ENT_SLOTS == 64,
          "virtual: pluginId 0x83E, VIRTUAL 1 = DSP input 0x18, 16 slots, 64 START entries");
    for (i = 0; i < 16; i++)
        if (StripBusSlot(StripSlotSynch(i)) != i)
            Check(0, "virtual: slot -> synch -> slot round trip");
    Check(StripSlotSynch(0) == 0 && StripSlotSynch(7) == 7 && StripSlotSynch(8) == 0x18 && StripSlotSynch(15) == 0x1F &&
          StripBusSlot(8) == STRIP_PLAN_SLOTS && StripBusSlot(0x0A) == STRIP_PLAN_SLOTS && StripBusSlot(0x17) == STRIP_PLAN_SLOTS &&
          StripBusSlot(0x20) == STRIP_PLAN_SLOTS && StripBusSlot(0x18) == 8 && StripBusSlot(0x1F) == 15,
          "virtual: synch of slot k (0-7 Mic, 8-15 VIRTUAL 1-8 = 0x18-0x1F); buses 8-0x17 and the talkback have no strip");
    Check(StripPlanPm(0xFF, 0x01) == 0x1FF && StripPlanPm(0x1FF, 0x1FF) == 0xFFFF && StripPlanPm(0x03, 0) == 0x03,
          "virtual: plan mask = StripMask | StripVirtualMask << 8 (8 bits each)");

    /* models */
    Check(x != NULL && t != NULL && x->VirtualAllowed == 0xFF && t->VirtualAllowed == 0x0F &&
          x->FirstVirtual == STRIP_VIRTUAL_SYNCH0 && t->FirstVirtual == STRIP_VIRTUAL_SYNCH0,
          "virtual: x8p VIRTUAL 1-8, Twin X 1-4 (virtual_max 4), both at DSP input 0x18");
    Check(StripModelVirtualMask(x, 0xFFFFFFFFu) == 0xFF && StripModelVirtualMask(t, 0xFF) == 0x0F &&
          StripModelVirtualMask(t, 0x30) == 0 && StripModelVirtualMask(NULL, 0x01) == 0 && StripModelVirtualMask(x, 0) == 0,
          "virtual: StripVirtualMask as each model takes it");

    /* the x8p's VIRTUAL 1, every word (8 Mic strips: the pass the x8p runs with StripMask 0xFF) */
    Check(StripPlanVirtualAddrs(8, 0, p.Addr) == 0 && memcmp(p.Addr, kV1Addr, sizeof(kV1Addr)) == 0 &&
          p.Addr[6] == 0 && p.Addr[9] == 0, "virtual: x8p VIRTUAL 1 at 0x9cf10 / 0x9cef2 / 0x08fede80..0x08fedb80");
    Check(kV1Addr[0] + 8 == kAddr[7][5] && kV1Addr[2] + 0x100 == kAddr[7][9],
          "virtual: VIRTUAL 1's blocks start right below Mic input 8's (pool 1 m4, pool 3 c3)");
    ok = StripPlanPrepareSlot(8, 8, 0, 48000, &p) == 0 && p.N == STRIP_PLAN_VPREPARE_ENTS && p.Len == 11 + 9 + 11;
    for (j = 0; ok && j < 6; j++)
        ok = !p.Ent[j].Dma && p.Ent[j].W[0] == 0x00080004 && p.Ent[j].W[1] == kV1Addr[j] && p.Ent[j].W[2] == 0 &&
             p.Ent[j].W[3] == oa_strip_input_rows[j].dwords;
    ok = ok && p.Ent[6].Dma && p.Ent[6].Dwords == 11 && memcmp(&p.Payload[p.Ent[6].Off], kV1Memspec, sizeof(kV1Memspec)) == 0 &&
         p.Ent[7].Dma && p.Ent[7].Dwords == 9 && memcmp(&p.Payload[p.Ent[7].Off], kV1Header, sizeof(kV1Header)) == 0 &&
         p.Ent[8].Dma && p.Ent[8].Dwords == 11 && memcmp(&p.Payload[p.Ent[8].Off], kV1Params, sizeof(kV1Params)) == 0;
    Check(ok, "virtual: x8p VIRTUAL 1 PREPARE: 6 zero-fills, memspec(11), header 0x83E(9), m0(11), word for word");
    ok = StripPlanStartPm(0x1FF, 8, 0, &q) == 0 && q.N == 36;
    for (j = 0; ok && j < 4; j++)
        ok = !q.Ent[32 + j].Dma && memcmp(q.Ent[32 + j].W, kV1Start[j], sizeof(kV1Start[j])) == 0;
    Check(ok, "virtual: x8p START 0x1ff: the 8 Mic strips' 32 entries, then VIRTUAL 1's SYNCH 0x00200018, span, span, route");
    {
        STRIP_PLAN q8;

        ok = StripPlanStart(0xFF, 0, &q8) == 0 && q8.N == 32;
        for (j = 0; ok && j < 32; j++)
            ok = memcmp(&q.Ent[j], &q8.Ent[j], sizeof(q8.Ent[j])) == 0;
        Check(ok, "virtual: ... and its first 32 entries are exactly the Mic-only START 0xff's");
    }
    for (j = 0; j < 6; j++)
        if (IsLinkWordPm(0x100, 8, kV1Addr[1] + j) && (j == 0 || j == 1 || j == 2 || j == 4 || j == 5))
            Check(0, "virtual: an m0 value word is a link word");

    /* word for word against upstream's ua_strip_build_input, every stage, the layouts where both agree */
    for (st = 0; st <= STRIP_STEP_ALL; st++) {
        static const ULONG vmasks[] = { 0x01, 0x03, 0x0F, 0xFF };

        for (m = 0; m < 4; m++) {
            ok = 1;
            for (i = 0; i < 8; i++)
                if (vmasks[m] & (1u << i))
                    ok = ok && VirtualMatchesUpstream(8, vmasks[m], i, st);
            sprintf_s(what, sizeof(what), "virtual: x8p (8 Mic) + Virtual 0x%02lx stage %lu: every Input Strip = "
                      "ua_strip_build_input's words at ua_strip_priv_addrs' addresses", vmasks[m], st);
            Check(ok, what);
        }
        ok = 1;
        for (i = 0; i < 4; i++)
            ok = ok && VirtualMatchesUpstream(2, 0x0F, i, st);
        sprintf_s(what, sizeof(what), "virtual: Twin X (2 Mic) + Virtual 0x0f (upstream's default) stage %lu: = upstream", st);
        Check(ok, what);
    }
    {
        /* the comparison can fail: VIRTUAL 1's plan against upstream's VIRTUAL 2, and against a Mic-strip header */
        static OA_XACT ox;
        unsigned char kinds[10] = { 0, 0, 0, 0, 0, 0, 0, 0, OA_KIND_INPUT, OA_KIND_INPUT };
        ULONG a2[10];

        oa_strip_priv_addrs(kinds, 9, a2);
        memset(&ox, 0, sizeof(ox));
        (void)oa_strip_build_input(&ox, a2, 0x19, 0);
        (void)StripPlanPrepareSlot(8, 8, 0, 48000, &p);
        (void)StripPlanStartPm(0x3FF, 8, 0, &q);
        Check(!SameAsUpstream(&p, &q, 32, 4, &ox) && SameAsUpstream(&p, &q, 32, 4, &ox) == 0,
              "virtual: the upstream comparison tells VIRTUAL 1 from VIRTUAL 2 (it is not vacuous)");
        (void)StripPlanPrepareSlot(9, 8, 0, 48000, &p);
        Check(SameAsUpstream(&p, &q, 36, 4, &ox) && a2[0] == 0x0009CEEA,
              "virtual: ... and VIRTUAL 2 matches it (header 0x9ceea, below VIRTUAL 1's 0x26 words)");
    }

    /* the Pm functions with no Virtual strip are the Mic-only functions, entry for entry */
    ok = 1;
    for (m = 0; m <= 0x100 && ok; m++)
        for (st = 0; st <= STRIP_STEP_ALL + 1 && ok; st++) {
            ULONG e1 = StripPlanStart(m, st, &p), e2 = StripPlanStartPm(m, 8, st, &q);

            ok = (m == 0x100) ? (e1 != 0 && e2 == 0) : (e1 == e2 && PlanSame(&p, &q));
            for (k = 0; ok && k <= STRIP_PLAN_SLOTS + 1; k++) {
                kind = StripPassStep(m & 0xFF, st, k, &in);
                kind2 = StripPassStepPm(m & 0xFF, st, k, &in2);
                ok = kind == kind2 && in == in2;
            }
        }
    Check(ok, "virtual: StripPlanStartPm / StripPassStepPm on masks 0-0xff = StripPlanStart / StripPassStep");
    ok = 1;
    for (i = 0; i <= STRIP_PLAN_INPUTS && ok; i++)
        for (st = 0; st <= STRIP_STEP_ALL + 1 && ok; st++) {
            ULONG e1 = StripPlanPrepareRate(i, st, 96000, &p), e2 = StripPlanPrepareSlot(i, 8, st, 96000, &q);

            ok = (i == STRIP_PLAN_INPUTS) ? (e1 != 0) : (e1 == e2 && PlanSame(&p, &q));
        }
    Check(ok, "virtual: StripPlanPrepareSlot of a Mic slot = StripPlanPrepareRate (96 kHz)");
    Check(StripPlanPrepareSlot(16, 8, 0, 48000, &p) != 0 && StripPlanPrepareSlot(8, 9, 0, 48000, &p) != 0 &&
          StripPlanPrepareSlot(8, 8, 0, 32000, &p) != 0 && StripPlanStartPm(0x10000, 8, 0, &p) != 0,
          "virtual: refused: slot 16, more than 8 Mic slots, a rate without strip words, a plan mask past VIRTUAL 8");
    ok = 1;
    for (st = 0; st <= STRIP_STEP_ALL; st++)
        ok = ok && StripPlanPrepareSlot(9, 8, st, 44100, &p) == 0 && StripPlanPrepareSlot(9, 8, st, 48000, &q) == 0 &&
             PlanSame(&p, &q);
    Check(ok, "virtual: the Input Strip's words are the same at every rate (upstream has no rate rule for them)");
    {
        static const ULONG wantEnts[10] = { 9, 0, 6, 7, 8, 9, 9, 9, 9, 9 };

        ok = 1;
        for (st = 0; st <= STRIP_STEP_ALL; st++)
            ok = ok && StripPlanPrepareSlot(8, 8, st, 48000, &p) == 0 && p.N == wantEnts[st];
        Check(ok, "virtual: PREPARE entries per stage 0-9: 9, 0, 6, 7, 8, 9, 9, 9, 9, 9");
    }

    /* the pass order and the stream checks with Virtual strips */
    Check(StripPassStepPm(0x1FF, 0, 7, &in) == STRIP_XACT_PREPARE && in == 7 &&
          StripPassStepPm(0x1FF, 0, 8, &in) == STRIP_XACT_PREPARE && in == 8 &&
          StripPassStepPm(0x1FF, 0, 9, &in) == STRIP_XACT_START && StripPassStepPm(0x1FF, 0, 10, &in) == STRIP_XACT_END,
          "virtual: pass 0x1ff: PREPARE inputs 1-8, PREPARE VIRTUAL 1, ONE START, end");
    Check(StripPassStepPm(0x300, 0, 0, &in) == STRIP_XACT_PREPARE && in == 8 &&
          StripPassStepPm(0x300, 0, 1, &in) == STRIP_XACT_PREPARE && in == 9 &&
          StripPassStepPm(0x300, 0, 2, &in) == STRIP_XACT_START && StripPassStepPm(0x300, 5, 2, &in) == STRIP_XACT_END,
          "virtual: Virtual strips alone (StripMask 0): PREPAREs then START; stage 5: no START");
    {
        static const ULONG pms[] = { 0x1FF, 0x101, 0x100, 0x8001, 0xA5A5, 0xFFFF };

        for (j = 0; j < sizeof(pms) / sizeof(pms[0]); j++) {
            for (st = 2; st <= STRIP_STEP_ALL; st++)
                CheckPassPm(pms[j], 8, st, "x8p");
            CheckPassPm(pms[j], 8, 0, "x8p");
        }
        CheckPassPm(0xF03, 2, 0, "Twin X");
        CheckPassPm(0x103, 2, 7, "Twin X");
    }
    Check(StripPlanStartPm(0xFFFF, 8, 0, &q) == 0 && q.N == 64, "virtual: START of all 16 strips: 64 entries (one transaction)");

    /* the room: where the blocks sit, the pool check, the refusal */
    StripPlanRoom(8, 0, room);
    Check(room[0] == 0 && room[1] == 8 * 0x54 && room[2] == 8 * 0x10 && room[3] == 8 * 0x2010,
          "room: no Virtual strip = the 8 Mic strips' (the ledger as before)");
    StripPlanRoom(8, 0x01, room);
    Check(room[1] == 8 * 0x54 + 0x26 && room[2] == 8 * 0x10 && room[3] == 8 * 0x2010 + 0x400, "room: + VIRTUAL 1");
    StripPlanRoom(8, 0x80, room);
    Check(room[1] == 8 * 0x54 + 8 * 0x26 && room[3] == 8 * 0x2010 + 8 * 0x400 && StripVirtualSlots(0x80) == 8 &&
          StripVirtualSlots(0x05) == 3 && StripVirtualSlots(0) == 0,
          "room: VIRTUAL 8 alone needs 8 Virtual slots (each VIRTUAL n has its fixed place)");
    Check(BlocksOk(0xFFFF, 8, kSize, kBase, kScr, &inside, &inRoom) && inside && inRoom,
          "x8p: all 16 strips' 128 blocks disjoint, inside DSP 1's measured pools above the scratch, inside the booked room");
    Check(BlocksOk(0xF03, 2, kTwxSize, kTwxBase, kTwxScr, &inside, &inRoom) && inside && inRoom,
          "Twin X: Mic 1-2 + VIRTUAL 1-4 disjoint, inside its DSP 1 pools and the booked room");
    StripPlanRoom(8, 0xFF, room);
    Check(StripPlanLayoutCheckRoom(kSize, kBase, kScr, room, STRIP_CODE_WORDS) == 0,
          "room: the x8p's DSP 1 pools fit 8 Mic + 8 Virtual strips");
    StripPlanRoom(2, 0x0F, room);
    Check(StripPlanLayoutCheckRoom(kTwxSize, kTwxBase, kTwxScr, room, STRIP_CODE_WORDS) == 0,
          "room: the Twin X's DSP 1 pools fit 2 Mic + 4 Virtual strips");
    {
        ULONG sc[4], room8[4];

        memcpy(sc, kScr, sizeof(sc));
        StripPlanRoom(8, 0, room8);
        sc[1] = 0x9D1B8 - 0x92800 - 8 * 0x54;                 /* pool 1: exactly the 8 Mic strips' room left */
        StripPlanRoom(8, 0x01, room);
        Check(StripPlanLayoutCheckRoom(kSize, kBase, sc, room8, STRIP_CODE_WORDS) == 0 &&
              StripPlanLayoutCheckRoom(kSize, kBase, sc, room, STRIP_CODE_WORDS) == 9,
              "room: pool 1 with room for the Mic strips only: Mic fit, StripVirtualMask 0x01 refused (code 9, pool 1)");
        memcpy(sc, kScr, sizeof(sc));
        sc[3] = 0x08FFE000 - 0x08008E96 - 8 * 0x2010 - 0x400 * 3;  /* pool 3: room for 3 Virtual strips */
        StripPlanRoom(8, 0x07, room);
        Check(StripPlanLayoutCheckRoom(kSize, kBase, sc, room, STRIP_CODE_WORDS) == 0, "room: pool 3 fits VIRTUAL 1-3 exactly");
        StripPlanRoom(8, 0x08, room);
        Check(StripPlanLayoutCheckRoom(kSize, kBase, sc, room, STRIP_CODE_WORDS) == 11,
              "room: ... VIRTUAL 4 needs a 4th slot: refused (code 11, pool 3)");
        Check(StripPlanLayoutCheck(kSize, kBase, kScr, 8, STRIP_CODE_WORDS) ==
              StripPlanLayoutCheckRoom(kSize, kBase, kScr, room8, STRIP_CODE_WORDS),
              "room: StripPlanLayoutCheck(8 strips) = the room check with the 8 Mic strips' room");
    }

    /* the plan hash: a plan mask without Virtual strips hashes as before (records of earlier builds match) */
    h = StripPlanHashPm(0xFF, 8, 9, 48000, 1, 6, &p);
    sprintf_s(what, sizeof(what), "adopt: StripPlanHashPm(0xff) = the pinned x8p plan hash %08lx (got %08lx)",
              (ULONG)ADOPT_X8P_PLAN_HASH, h);
    Check(h == ADOPT_X8P_PLAN_HASH, what);
    ok = 1;
    for (m = 0; m <= 0xFF && ok; m += 0x11)
        for (st = 1; st <= STRIP_STEP_ALL && ok; st++)
            ok = StripPlanHashPm(m, 8, st, 48000, 1, 6, &p) == StripPlanHash(m, st, 48000, 1, 6, &q) &&
                 StripPlanHashPm(m, 2, st, 96000, 1, 2, &p) == StripPlanHash(m, st, 96000, 1, 2, &q);
    Check(ok, "adopt: StripPlanHashPm = StripPlanHash for every StripMask without Virtual strips (masks, stages, rates)");
    h = StripPlanHashPm(0x1FF, 8, 9, 48000, 1, 6, &p);
    Check(h != 0 && h != ADOPT_X8P_PLAN_HASH && h != StripPlanHashPm(0x3FF, 8, 9, 48000, 1, 6, &p) &&
          h != StripPlanHashPm(0x1FF, 2, 9, 48000, 1, 6, &p) && h != StripPlanHashPm(0x1FF, 8, 6, 48000, 1, 6, &p),
          "adopt: a Virtual strip changes the hash (which ones, where they sit, the stage)");

    /* the record: version 1 as before without Virtual strips, version 2 with them; an older build refuses version 2 */
    memset(&r, 0, sizeof(r));
    r.Anchored = 1;
    AdoptLivePm(&r, 0xFF, 8, 9, 48000, 1, 6, 1, kX8pGeom);
    StripRecordSeal(&r);
    Check(r.Version == 1 && StripRecordValid(&r, sizeof(r)) && OldRecordValid(&r, sizeof(r)),
          "record: StripMask 0xff, no Virtual strip: version 1, valid here and for an older build");
    r2 = r;
    AdoptLivePm(&r2, 0x1FF, 8, 9, 48000, 1, 6, 1, kX8pGeom);
    StripRecordSeal(&r2);
    Check(r2.Version == STRIP_REC_VERSION_VIRTUAL && StripRecordValid(&r2, sizeof(r2)) && !OldRecordValid(&r2, sizeof(r2)),
          "record: + VIRTUAL 1: version 2, valid here, refused by an older build (it asks for a power cycle)");
    r2.Version = 1;
    r2.Check = StripRecordSum(&r2);
    Check(!StripRecordValid(&r2, sizeof(r2)), "record: Virtual strips under version 1: not trusted");
    r2 = r;
    r2.Version = 2;
    r2.Check = StripRecordSum(&r2);
    Check(!StripRecordValid(&r2, sizeof(r2)), "record: version 2 without Virtual strips: not trusted");
    r2 = r;
    r2.Mask = 0x10000;
    StripRecordSeal(&r2);
    Check(!StripRecordValid(&r2, sizeof(r2)), "record: a plan mask past VIRTUAL 8: not trusted");
    r2 = r;
    StripRecordClearStrips(&r2, STRIP_REC_NONE);
    StripRecordSeal(&r2);
    Check(r2.Version == 1 && OldRecordValid(&r2, sizeof(r2)), "record: torn down (nothing of ours runs): version 1 again");

    /* the decision with Virtual strips, x8p (record RUNNING, kept, anchored) */
    AdoptNowPm(&n, STRIP_ANCHOR_OK, 1, 0, 0xFF, 8, 9, 48000, 1, 6, kX8pGeom);
    Check(StripAdoptDecide(&r, &n, &td) == STRIP_ADOPT_ADOPTED && !td,
          "decide: a record of a build without Virtual strips, this start without them: ADOPTED (as before)");
    AdoptNowPm(&n, STRIP_ANCHOR_OK, 1, 0, 0x1FF, 8, 9, 48000, 1, 6, kX8pGeom);
    Check(StripAdoptDecide(&r, &n, &td) == STRIP_ADOPT_PLAN_DIFFERS && !td,
          "decide: ... this start with VIRTUAL 1: PLAN_DIFFERS (nothing adopted that the record does not cover)");
    n.WarmRebuild = 1;
    Check(StripAdoptDecide(&r, &n, &td) == STRIP_ADOPT_REBUILD && td,
          "decide: ... with StripWarmRebuild (x8p default 1): REBUILD, the recorded Mic strips stopped first");
    r2 = r;
    AdoptLivePm(&r2, 0x1FF, 8, 9, 48000, 1, 6, 1, kX8pGeom);
    StripRecordSeal(&r2);
    AdoptNowPm(&n, STRIP_ANCHOR_OK, 1, 0, 0x1FF, 8, 9, 48000, 1, 6, kX8pGeom);
    Check(StripAdoptDecide(&r2, &n, &td) == STRIP_ADOPT_ADOPTED && !td,
          "decide: a record with VIRTUAL 1, this start the same: ADOPTED (the record covers the Input Strip)");
    AdoptNowPm(&n, STRIP_ANCHOR_OK, 1, 0, 0xFF, 8, 9, 48000, 1, 6, kX8pGeom);
    Check(StripAdoptDecide(&r2, &n, &td) == STRIP_ADOPT_PLAN_DIFFERS,
          "decide: a record with VIRTUAL 1, this start without it (StripVirtualMask 0 or refused): PLAN_DIFFERS");
    AdoptNowPm(&n, STRIP_ANCHOR_OK, 1, 0, 0x3FF, 8, 9, 48000, 1, 6, kX8pGeom);
    Check(StripAdoptDecide(&r2, &n, &td) == STRIP_ADOPT_PLAN_DIFFERS, "decide: another Virtual mask: PLAN_DIFFERS");
    {
        /* the rebuild's teardown (strips.c StripsWarmDecide) stops every slot the record's plan mask names */
        ULONG started = 0;

        for (i = 0; i < STRIP_PLAN_SLOTS; i++)
            if ((r2.Mask & (1u << i)) && r2.Limit >= STRIP_STEP_SYNCH)
                started |= 1u << i;
        Check(started == 0x1FF && StripSlotSynch(8) == 0x18,
              "decide: a rebuild over a version 2 record stops inputs 1-8 and VIRTUAL 1 (SYNCH 0 at 0x18)");
    }
}

/* --print-pm <plan mask>: the pass for the x8p (8 Mic slots), every transaction. */
static void PrintPassPm(ULONG Pm)
{
    STRIP_PLAN p;
    ULONG k, in, kind;
    char name[16];

    for (k = 0; k <= STRIP_PLAN_SLOTS + 1; k++) {
        kind = StripPassStepPm(Pm, 0, k, &in);
        if (kind == STRIP_XACT_PREPARE) {
            (void)StripPlanPrepareSlot(in, 8, 0, 48000, &p);
            if (in < STRIP_SLOT_VIRTUAL0)
                sprintf_s(name, sizeof(name), "input %lu", in + 1);
            else
                sprintf_s(name, sizeof(name), "VIRTUAL %lu", in - STRIP_SLOT_VIRTUAL0 + 1);
            printf("Transaction %lu: PREPARE %s (StripSteps 0), on the strip DSP:\n", k + 1, name);
            PrintPlan(&p);
        } else if (kind == STRIP_XACT_START) {
            (void)StripPlanStartPm(Pm, 8, 0, &p);
            printf("Transaction %lu: START plan mask 0x%04lx, on the strip DSP:\n", k + 1, Pm);
            PrintPlan(&p);
        } else {
            break;
        }
    }
}

int main(int argc, char **argv)
{
    static STREAM old;
    STRIP_PLAN p, q;
    ULONG in, st, j, k, kind;
    char what[160];

    {
        /* strips.c's layout check asserts these macros at compile time; the rows must add up. */
        ULONG sum[4] = { 0, 0, 0, 0 };

        for (j = 0; j < STRIP_PLAN_ROWS; j++)
            sum[g_StripRows[j].Pool & 3] += g_StripRows[j].Dwords;
        Check(sum[1] == STRIP_POOL1_PER_STRIP && sum[2] == STRIP_POOL2_PER_STRIP &&
              sum[3] == STRIP_POOL3_PER_STRIP, "rows per pool add up to the per-strip needs");
        Check(STRIP_POOL1_PER_STRIP == 0x54 && STRIP_POOL2_PER_STRIP == 0x10 &&
              STRIP_POOL3_PER_STRIP == 0x2010, "per-strip needs are the recipe's 0x54/0x10/0x2010");
        Check(g_StripTop[1] == 0x0009D1B8 && g_StripTop[2] == 0x000C6FFE &&
              g_StripTop[3] == 0x08FFE000, "pool tops are the recipe's");
    }

    for (in = 0; in < 8; in++) {
        StripPlanAddrs(in, p.Addr);
        sprintf_s(what, sizeof(what), "input %lu: private addresses", in + 1);
        Check(memcmp(p.Addr, kAddr[in], sizeof(kAddr[in])) == 0, what);
    }

    /* The eight strips' 80 rows: pairwise disjoint, and inside the x8p's pools above scratch */
    {
        ULONG a[8][STRIP_PLAN_ROWS], i2, j2, ok = 1, inside = 1, pool;

        for (in = 0; in < 8; in++)
            StripPlanAddrs(in, a[in]);
        for (in = 0; in < 8; in++) {
            for (j = 0; j < STRIP_PLAN_ROWS; j++) {
                ULONG lo = a[in][j], hi = lo + g_StripRows[j].Dwords;
                ULONG start, top, sc;

                pool = g_StripRows[j].Pool & 3;
                start = kBase[pool] + (kBase[pool] & 1);
                top = start + (kSize[pool] & ~1u);
                sc = kScr[pool] + (kScr[pool] & 1);
                if (sc < kStripScratch[pool])
                    sc = kStripScratch[pool];
                if (lo < start + sc || hi > top || (lo & 1))
                    inside = 0;
                for (i2 = 0; i2 < 8; i2++) {
                    for (j2 = 0; j2 < STRIP_PLAN_ROWS; j2++) {
                        ULONG lo2 = a[i2][j2], hi2 = lo2 + g_StripRows[j2].Dwords;

                        if ((i2 != in || j2 != j) && lo < hi2 && lo2 < hi)
                            ok = 0;
                    }
                }
            }
        }
        Check(ok, "eight strips: no two private blocks overlap");
        Check(inside, "eight strips: every block inside the x8p's measured pools, above the scratch");
    }

    /* PREPARE and START entry counts per stage */
    for (st = 0; st <= STRIP_STEP_ALL; st++) {
        Check(StripPlanPrepare(0, st, &p) == 0 && p.Err == 0, "prepare plan builds");
        sprintf_s(what, sizeof(what), "StripSteps %lu: PREPARE %lu entries (got %lu)", st, kPrepEntries[st], p.N);
        Check(p.N == kPrepEntries[st], what);
        Check(StripPlanStart(0x01, st, &q) == 0 && q.Err == 0, "start plan builds (0x01)");
        sprintf_s(what, sizeof(what), "StripSteps %lu: START 0x01 %lu entries (got %lu)", st, kStartEntries[st], q.N);
        Check(q.N == kStartEntries[st], what);
        Check(StripPlanStart(0xFF, st, &q) == 0 && q.Err == 0, "start plan builds (0xff)");
        sprintf_s(what, sizeof(what), "StripSteps %lu: START 0xff %lu entries (got %lu)", st, 8 * kStartEntries[st], q.N);
        Check(q.N == 8 * kStartEntries[st], what);
        for (j = 0; j < p.N; j++)
            Check(!(p.Ent[j].W[0] == STRIP_CMD_SYNCH || p.Ent[j].W[0] == STRIP_CMD_INPUT),
                  "PREPARE never carries a SYNCH or an input word");
    }
    Check(StripPlanStart(0x100, 0, &q) != 0, "START refuses a mask beyond input 8");
    Check(StripPlanPrepare(8, 0, &p) != 0, "PREPARE refuses input 9");

    /* Every input's PREPARE, word for word (docs/STRIPS.md 2.6, 2.7) */
    for (in = 0; in < 8; in++) {
        Check(StripPlanPrepare(in, 0, &p) == 0 && p.N == STRIP_PLAN_PREPARE_ENTS, "full PREPARE builds");
        for (j = 0; j < 10; j++) {
            sprintf_s(what, sizeof(what), "input %lu: zero-fill %lu", in + 1, j + 1);
            Check(!p.Ent[j].Dma && p.Ent[j].W[0] == 0x00080004 && p.Ent[j].W[1] == kAddr[in][j] &&
                  p.Ent[j].W[2] == 0 && p.Ent[j].W[3] == kZeroDwords[j], what);
        }
        /* DMA entries and their packing (memspec +0x000, header +0x04C, parameters +0x070) */
        Check(p.Ent[10].Dma && p.Ent[10].Off * 4 == 0x000 && p.Ent[10].Dwords == 19, "memspec ref");
        Check(p.Ent[11].Dma && p.Ent[11].Off * 4 == 0x04C && p.Ent[11].Dwords == 9, "header ref");
        Check(p.Ent[12].Dma && p.Ent[12].Off * 4 == 0x070 && p.Ent[12].Dwords == 55, "params ref");
        Check(p.Len * 4 == 0x14C, "payload ends at +0x14c");
        if (in == 0)
            Check(memcmp(&p.Payload[0], kMemspec1, sizeof(kMemspec1)) == 0, "input 1 memspec");
        if (in == 7)
            Check(memcmp(&p.Payload[0], kMemspec8, sizeof(kMemspec8)) == 0, "input 8 memspec");
        {
            const ULONG *h = &p.Payload[19];
            const ULONG want[9] = { 0x00140009, kAddr[in][0], 0x24, 0x40000000, 1, 1, 0x22, 1, 0x846 };

            sprintf_s(what, sizeof(what), "input %lu: header", in + 1);
            Check(memcmp(h, want, sizeof(want)) == 0, what);
        }
        {
            const ULONG *w = &p.Payload[28];

            sprintf_s(what, sizeof(what), "input %lu: parameter write headers", in + 1);
            Check(w[0] == 0x00140014 && w[1] == kAddr[in][1] &&
                  w[20] == 0x00140012 && w[21] == kAddr[in][1] + 38 &&
                  w[38] == 0x00140011 && w[39] == kAddr[in][4] + 1, what);
            Check(w[2] == 0x0f1f7fe4 && w[13] == 0x0000004b && w[14] == 0x41000000 &&
                  w[15] == 0x3ada740e && w[22] == 0x00000040 && w[37] == 0x00000001 &&
                  w[40] == 0x00000040 && w[54] == 0xbd565d96, "captured 48 kHz words in place");
        }
    }

    /* The START for 0xFF: per input SYNCH (own header), span, span, route; input 1 first */
    Check(StripPlanStart(0xFF, 0, &q) == 0 && q.N == 32, "START 0xff: 32 inline entries");
    for (in = 0; in < 8; in++) {
        const STRIP_PLAN_ENT *e = &q.Ent[4 * in];

        sprintf_s(what, sizeof(what), "START 0xff: input %lu SYNCH and input words", in + 1);
        Check(!e[0].Dma && e[0].W[0] == 0x001F0004 && e[0].W[1] == (0x00200000 | in) &&
              e[0].W[2] == kAddr[in][0] && e[0].W[3] == 0 &&
              e[1].W[0] == 0x001E0004 && e[1].W[1] == in && e[1].W[2] == 8 && e[1].W[3] == 0x20 &&
              e[2].W[0] == 0x001E0004 && e[2].W[1] == in && e[2].W[2] == 8 && e[2].W[3] == 0x20 &&
              e[3].W[0] == 0x001E0004 && e[3].W[1] == in && e[3].W[2] == 7 &&
              e[3].W[3] == 0x0001FFFF, what);
    }
    Check(StripPlanStart(0xA4, 0, &q) == 0 && q.N == 12 && q.Ent[0].W[1] == 0x00200002 &&
          q.Ent[4].W[1] == 0x00200005 && q.Ent[8].W[1] == 0x00200007 &&
          q.Ent[8].W[2] == kAddr[7][0], "START 0xa4: inputs 3, 6, 8 at their own headers");

    /* Mask 0x01: the same 17 entries, in the same order, as the one-strip run that worked (Test A) */
    {
        ULONG kind0, kind1, kind2, in0, in1, in2, ok;

        kind0 = StripPassStep(0x01, 0, 0, &in0);
        kind1 = StripPassStep(0x01, 0, 1, &in1);
        kind2 = StripPassStep(0x01, 0, 2, &in2);
        Check(kind0 == STRIP_XACT_PREPARE && in0 == 0 && kind1 == STRIP_XACT_START &&
              kind2 == STRIP_XACT_END, "mask 0x01: PREPARE input 1, START, end");
        (void)StripPlanPrepare(0, 0, &p);
        (void)StripPlanStart(0x01, 0, &q);
        ok = (p.N + q.N == 17);
        for (j = 0; ok && j < 10; j++)
            ok = p.Ent[j].W[0] == 0x00080004 && p.Ent[j].W[1] == kAddr[0][j];
        ok = ok && p.Ent[10].Dma && p.Ent[11].Dma && p.Ent[12].Dma &&
             memcmp(&p.Payload[0], kMemspec1, sizeof(kMemspec1)) == 0 &&
             q.Ent[0].W[0] == 0x001F0004 && q.Ent[0].W[1] == 0x00200000 && q.Ent[0].W[2] == 0x9D1B0 &&
             q.Ent[1].W[2] == 8 && q.Ent[2].W[2] == 8 && q.Ent[3].W[2] == 7 && q.Ent[3].W[3] == 0x1FFFF;
        Check(ok, "mask 0x01: the Test A command stream, entry for entry");
    }

    /* The transaction order for 0xFF at every stage */
    for (st = 0; st <= STRIP_STEP_ALL; st++) {
        ULONG want = (StripPlanSteps(st) < STRIP_STEP_ZERO) ? 0 :
                     8 + (StripPlanSteps(st) >= STRIP_STEP_SYNCH ? 1 : 0), got = 0, ok = 1;

        for (k = 0; k <= 10; k++) {
            kind = StripPassStep(0xFF, st, k, &in);
            if (kind == STRIP_XACT_END)
                break;
            got++;
            if (k < 8 && (kind != STRIP_XACT_PREPARE || in != k))
                ok = 0;
            if (k == 8 && kind != STRIP_XACT_START)
                ok = 0;
        }
        sprintf_s(what, sizeof(what), "mask 0xff StripSteps %lu: %lu transactions, PREPAREs then START (got %lu)",
                  st, want, got);
        Check(ok && got == want, what);
    }
    Check(StripPassStep(0x00, 0, 0, &in) == STRIP_XACT_END, "mask 0: nothing");

    /* Whole passes through the stream checks */
    {
        static const ULONG masks[] = { 0x01, 0x03, 0x80, 0xA4, 0xFF };

        for (j = 0; j < sizeof(masks) / sizeof(masks[0]); j++)
            for (st = 2; st <= STRIP_STEP_ALL; st++)
                CheckPass(masks[j], st);
        CheckPass(0xFF, 0);
    }

    /* The old order (each strip started in its own transaction, before the next strip's memory
       is written) must be caught: it is what left inputs 1, 2, 5, 6, 7 at exact zero. */
    Check(BuildOldPass(&old, 0xFF, 0) && !NoWriteAfterStart(&old),
          "the old per-strip order (open-apollo's) fails the no-write-after-start check");
    Check(BuildOldPass(&old, 0x01, 0) && NoWriteAfterStart(&old),
          "with one strip the old order passes it (Test A worked)");

    CheckProbe();
    CheckModels();          /* [Twin X G5] */
    CheckAdopt();           /* [Warm] */
    CheckVirtual();         /* [Virtual] */

    if (argc > 1 && strcmp(argv[1], "--adopt-hash") == 0)
        printf("x8p plan hash (0xFF, stage 9, 48 kHz, DSP 1 of 6): 0x%08lXu; modules 0x%08lXu\n",
               StripPlanHash(0xFF, 9, 48000, 1, 6, &p), AdoptModulesHash());

    if (argc > 1 && strcmp(argv[1], "--print") == 0)
        PrintPass(argc > 2 ? strtoul(argv[2], NULL, 0) & 0xFF : 0xFF);
    if (argc > 1 && strcmp(argv[1], "--print-probe") == 0)
        PrintProbe(argc > 2 ? strtoul(argv[2], NULL, 0) & 0xFF : 0xFF);
    if (argc > 1 && strcmp(argv[1], "--print-pm") == 0)
        PrintPassPm(argc > 2 ? strtoul(argv[2], NULL, 0) & 0xFFFF : 0x1FF);
    if (argc > 1 && strcmp(argv[1], "--digests") == 0) {
        unsigned long long d[PLAN_DIGESTS];

        PlanDigests(d);
        for (j = 0; j < PLAN_DIGESTS; j++)
            printf("0x%016llXull,\n", d[j]);
    }
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
