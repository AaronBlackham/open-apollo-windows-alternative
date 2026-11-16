/*
 * plugin-plan-test.c - offline checks of driver\plugin_plan.h (docs/PLUGINS.md gate P0): the
 * transaction paging, the pool ledger (the strip room pre-booked exactly where strip_plan.h puts
 * the 8 strips, plug-in placement beside it, release back to baseline, random churn against
 * invariants), module record checks, licence decode and date, request validation (rows, WRITEs,
 * chain directory, CREATE flags) and the words of every plug-in transaction for a synthetic recipe
 * built the way open-apollo's native carrier builds one (oa:mixer-engine/native_carrier.py:393-416);
 * staged startup: the page cut against upstream's chunks() (its own test cases and reference cuts
 * computed with the vendored 2.1.0 code), and a multi-page startup through stage, SERVICE, START;
 * stereo on a linked pair: the Synch rule (PlugSynchCheck), the overlap rule, and the start,
 * stage, START and teardown words on a pair (outside both digests); [P7] hosting per model: the
 * x8p's row = the pre-P7 constants, both rows = strip_plan.h's, the Twin X's ledger and inputs.
 * No UA bytes: every record and recipe here is synthetic. Exit code 0 = all passed.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "..\..\driver\strip_plan.h"
#include "..\..\driver\plugin_plan.h"
#include "..\..\driver\unison_plan.h"   /* [Ins] the Unison lease */

static int g_fail, g_pass;

#define CHECK(cond, ...) do { if (cond) { g_pass++; } else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- FNV-1a over words (the encodings' golden; the staged startup's has its own) ---- */
static ULONG g_fnv = 2166136261u, g_fnvStage;
static void Fnv(ULONG v)
{
    int i;
    for (i = 0; i < 4; i++) {
        g_fnv ^= (v >> (8 * i)) & 0xFFu;
        g_fnv *= 16777619u;
    }
}
static void FnvXl(const PLUG_XL *x, const PLUG_PAY *p)
{
    ULONG k, j;

    Fnv(x->Err); Fnv(x->N); Fnv(x->Len); Fnv(x->NRx); Fnv(x->RxLen); Fnv(x->RxFill);
    for (k = 0; k < x->N; k++)
        for (j = 0; j < 4; j++)
            Fnv(x->Ent[k][j]);
    for (k = 0; k < x->NRx; k++) {
        Fnv(x->RxWords[k]);
        Fnv(x->RxOff[k]);
    }
    if (p != NULL)
        for (k = 0; k < x->Len; k += 4) {
            ULONG w;
            memcpy(&w, p->Page[k / PLUG_PAGE] + k % PLUG_PAGE, 4);
            Fnv(w);
        }
}

/* ---- payload pages ---- */
static UCHAR g_pages[PLUG_PAY_PAGES_MAX][PLUG_PAGE];
static void PayInit(PLUG_PAY *p, ULONG pages)
{
    ULONG i;
    memset(g_pages, 0, sizeof(g_pages));
    p->NPages = pages;
    for (i = 0; i < PLUG_PAY_PAGES_MAX; i++)
        p->Page[i] = (i < pages) ? g_pages[i] : NULL;
}
static ULONG PayWord(const PLUG_PAY *p, ULONG off)
{
    ULONG w;
    memcpy(&w, p->Page[off / PLUG_PAGE] + off % PLUG_PAGE, 4);
    return w;
}

/* Every DMA_REF stays inside one 4 KiB page, offsets ascending and contiguous per command. */
static int RefsOk(const PLUG_XL *x)
{
    ULONG k;
    for (k = 0; k < x->N; k++) {
        if (x->Ent[k][0] & PLUG_RING_DMA_REF) {
            ULONG bytes = (x->Ent[k][0] & 0xFFFFu) * 4, at = x->Ent[k][2];
            if (bytes == 0 || bytes > PLUG_PAGE || at / PLUG_PAGE != (at + bytes - 1) / PLUG_PAGE)
                return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */

static void TestBill(void)
{
    UCHAR rec[400];
    PLUG_BILL b;
    ULONG w[5];

    memset(rec, 0x5A, sizeof(rec));
    w[0] = PLUG_BILL_MAGIC; w[1] = 0x301; w[2] = 0x02000000; w[3] = sizeof(rec) - 20; w[4] = 81;
    memcpy(rec, w, sizeof(w));
    CHECK(PlugBillCheck(rec, sizeof(rec), &b) == PLUG_OK && b.Kind == 0 && b.Arch == 2 && b.Pool == 0 &&
          b.Words == 54 && b.Id == 0x301, "code record");
    CHECK(PlugUploadHeader(&b, sizeof(rec)) == (0x00010000u | (400 / 4 + 2)), "code upload header");
    w[4] = 80; memcpy(rec, w, sizeof(w));      /* 2*80/3 = 53: odd code words */
    CHECK(PlugBillCheck(rec, sizeof(rec), &b) == PLUG_EINVAL, "odd code words refused");
    w[2] = 0x00000001; w[4] = 50; memcpy(rec, w, sizeof(w));
    CHECK(PlugBillCheck(rec, sizeof(rec), &b) == PLUG_OK && b.Pool == 1 && b.Words == 50, "data kind 1");
    CHECK(PlugUploadHeader(&b, sizeof(rec)) == (0x00040000u | 102), "data upload header");
    w[2] = 0x00000004; memcpy(rec, w, sizeof(w));
    CHECK(PlugBillCheck(rec, sizeof(rec), &b) == PLUG_OK && b.Pool == 1, "kind 4 -> pool 1");
    w[2] = 0x00000003; memcpy(rec, w, sizeof(w));
    CHECK(PlugBillCheck(rec, sizeof(rec), &b) == PLUG_OK && b.Pool == 3, "kind 3 -> pool 3");
    w[2] = 0x00000100; memcpy(rec, w, sizeof(w));
    CHECK(PlugBillCheck(rec, sizeof(rec), &b) == PLUG_EINVAL, "code with flags refused");
    w[2] = 0x03000000; memcpy(rec, w, sizeof(w));
    CHECK(PlugBillCheck(rec, sizeof(rec), &b) == PLUG_EINVAL, "arch 3 refused");
    w[2] = 0x00000005; memcpy(rec, w, sizeof(w));
    CHECK(PlugBillCheck(rec, sizeof(rec), &b) == PLUG_EINVAL, "kind 5 refused");
    w[2] = 0x02000000; w[3] = 1; memcpy(rec, w, sizeof(w));
    CHECK(PlugBillCheck(rec, sizeof(rec), &b) == PLUG_EINVAL, "payload length mismatch refused");
    w[3] = sizeof(rec) - 20; w[4] = sizeof(rec) / 4 - 12; memcpy(rec, w, sizeof(w));
    CHECK(PlugBillCheck(rec, sizeof(rec), &b) == PLUG_EINVAL, "w4 too large refused");
    w[0] = 0x6C6C6943; w[4] = 81; memcpy(rec, w, sizeof(w));
    CHECK(PlugBillCheck(rec, sizeof(rec), &b) == PLUG_EINVAL, "bad magic refused");
    CHECK(PlugBillCheck(rec, 398, &b) == PLUG_EINVAL, "unaligned length refused");
    CHECK(PlugBillCheck(rec, PLUG_BILL_MAX + 4, &b) == PLUG_EINVAL, "too long refused");

    {
        PLUG_BILL s[3] = { { 1, 0, 0, 0, 2, 0, 0, 0 }, { 2, 0, 1, 0, 0, 0, 0, 1 }, { 1, 0, 0, 0, 0, 0, 0, 0 } };
        CHECK(PlugCheckSet(s, 3) == PLUG_OK, "set: same id, other arch is fine");
        s[2].Arch = 2;
        CHECK(PlugCheckSet(s, 3) == PLUG_EEXIST, "set: same id and arch twice refused");
        s[2].Arch = 1;
        CHECK(PlugCheckSet(s, 3) == PLUG_EINVAL, "set: arch 1 refused");
        s[2].Arch = 0; s[2].Kind = 3; s[2].Flags = 1;
        CHECK(PlugCheckSet(s, 3) == PLUG_ENOTSUP, "set: kind 3 with flags refused");
    }
    {
        PLUG_BILL u = { 0x301, 0x02000000, 0, 0, 2, 0, 0, 0 };
        ULONG r[4] = { PLUG_REPLY_UPLOAD, 0, 0x301, 0x12345 };
        CHECK(PlugUploadReplyOk(r, &u), "upload reply: plain id");
        r[2] = 0x02000301;
        CHECK(PlugUploadReplyOk(r, &u), "upload reply: id with arch byte");
        r[2] = 0x302;
        CHECK(!PlugUploadReplyOk(r, &u), "upload reply: other id refused");
        r[2] = 0x301; r[1] = 1;
        CHECK(!PlugUploadReplyOk(r, &u), "upload reply: status 1 refused");
    }
}

/* Days from 1970-01-01 to y-m-d (proleptic Gregorian). */
static long long DaysFromCivil(long long y, unsigned m, unsigned d)
{
    long long era;
    unsigned yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned)(y - era * 400);
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

static ULONGLONG FileTimeOf(int y, int mo, int d, int h, int mi)
{
    long long days = DaysFromCivil(y, (unsigned)mo, (unsigned)d);
    long long secs = days * 86400 + h * 3600 + mi * 60;
    return (ULONGLONG)(secs + 11644473600LL) * 10000000ULL;
}

static void TestLicence(void)
{
    ULONG date = 7;
    long long d2026 = DaysFromCivil(2026, 11, 14) - DaysFromCivil(2007, 1, 1);

    CHECK(FileTimeOf(2007, 1, 1, 0, 0) == PLUG_FILETIME_2007, "FILETIME of 2007-01-01");
    CHECK(PlugDate(FileTimeOf(2007, 1, 1, 0, 0), &date) == PLUG_OK && date == 0, "date 2007-01-01 00:00 = 0");
    CHECK(PlugDate(FileTimeOf(2007, 1, 2, 0, 1), &date) == PLUG_OK && date == (1u << 12) + 1, "date 2007-01-02 00:01");
    CHECK(PlugDate(FileTimeOf(2026, 11, 14, 22, 37), &date) == PLUG_OK &&
          date == ((ULONG)d2026 << 12) + 22 * 60 + 37, "date 2026-11-14 22:37 (day %lld)", d2026);
    CHECK(PlugDate(FileTimeOf(2006, 12, 31, 23, 59), &date) == PLUG_ERANGE && date == 0, "date before 2007 refused");

    CHECK(PlugLicenseState(0x81000000u) == PLUG_LIC_AUTHORIZED && PlugLicenseState(0x82000000u) == PLUG_LIC_AUTHORIZED,
          "authorized words");
    CHECK(PlugLicenseState(0x80000000u) == PLUG_LIC_NOT_STARTED, "demo not started");
    CHECK(PlugLicenseState(0) == PLUG_LIC_EXPIRED, "expired");
    CHECK(PlugLicenseState(0x83000000u) == PLUG_LIC_UNAVAILABLE, "unavailable / prefill");
    CHECK(PlugLicenseState(0x0000001Eu) == PLUG_LIC_DEMO && PlugLicenseState(0x80000001u) == PLUG_LIC_DEMO,
          "anything else: a running demo");
    CHECK(PlugLicenseAdmits(PLUG_LIC_AUTHORIZED) && PlugLicenseAdmits(PLUG_LIC_DEMO) &&
          !PlugLicenseAdmits(PLUG_LIC_NOT_STARTED) && !PlugLicenseAdmits(PLUG_LIC_EXPIRED) &&
          !PlugLicenseAdmits(PLUG_LIC_UNAVAILABLE) && !PlugLicenseAdmits(PLUG_LIC_UNKNOWN), "only authorized and demo admit");

    {
        static PLUG_XL x;
        PlugXlInit(&x, 0);
        CHECK(PlugBuildBoardQuery(&x, 0x12345678, 0xC00C1E) == PLUG_OK && x.N == 2 && x.NRx == 1 &&
              x.RxWords[0] == 770 && x.RxOff[0] == 0 && x.RxFill == 0x83000000u &&
              x.Ent[0][0] == 0x00100002u && x.Ent[0][1] == 0x12345678 && x.Ent[1][0] == 0x00110002u &&
              x.Ent[1][1] == 0xC00C1E && PlugXlRxPages(&x) == 1 && x.Len == 0, "board query transaction");
        FnvXl(&x, NULL);
    }
}

static void TestPaging(void)
{
    static PLUG_XL x;
    static ULONG words[65536];
    PLUG_PAY p;
    ULONG off, k, sum, i;

    /* small then large-but-one-page: the second starts on the next page rather than straddle */
    PayInit(&p, 2);
    PlugXlInit(&x, 2 * PLUG_PAGE);
    off = PlugXlCmd(&x, 10);
    CHECK(off == 0 && x.N == 1 && x.Ent[0][0] == (PLUG_RING_DMA_REF | 10) && x.Ent[0][2] == 0, "first command at 0");
    off = PlugXlCmd(&x, 1020);
    CHECK(off == PLUG_PAGE && x.N == 2 && x.Ent[1][0] == (PLUG_RING_DMA_REF | 1020) && x.Ent[1][2] == PLUG_PAGE,
          "a <=4 KiB command never straddles: next page (off %lu)", off);
    off = PlugXlCmd(&x, 2);
    CHECK(off == PLUG_PAGE + 4080 && x.N == 3, "packs after it");
    off = PlugXlCmd(&x, 2);
    CHECK(off == PLUG_PAGE + 4088 && x.N == 4 && x.Len == 2 * PLUG_PAGE, "fills the page exactly");
    off = PlugXlCmd(&x, 1);
    CHECK(off == PLUG_NONE && x.Err == PLUG_ENOSPC && x.N == 4, "no room: ENOSPC (sticky)");
    PlugXlInline(&x, 1, 2, 3, 4);
    CHECK(x.N == 4, "sticky error: nothing added after it");

    /* the largest module on this PC: 246,068 bytes, split at page boundaries only */
    PlugXlInit(&x, PLUG_PAY_PAGES_MAX * PLUG_PAGE);
    PayInit(&p, PLUG_PAY_PAGES_MAX);
    off = PlugXlCmd(&x, 246068 / 4 + 2);
    for (k = 0, sum = 0; k < x.N; k++)
        sum += (x.Ent[k][0] & 0xFFFFu) * 4;
    CHECK(off == 0 && x.N == 61 && sum == 246076 && RefsOk(&x) && x.Len == 246076 && PlugXlPayPages(&x) == 61,
          "246,068-byte record: 61 page pieces (N %lu, sum %lu)", x.N, sum);
    for (k = 1; k < x.N; k++)
        CHECK(x.Ent[k][2] == x.Ent[k - 1][2] + (x.Ent[k - 1][0] & 0xFFFFu) * 4, "pieces contiguous at %lu", k);
    /* the payload copy across pages, and back */
    for (i = 0; i < 61519; i++)
        words[i] = i * 2654435761u;
    CHECK(PlugPayCopy(&p, off, words, 61519 * 4) == PLUG_OK, "paged copy");
    for (i = 0, sum = 0; i < 61519; i++)
        sum += PayWord(&p, i * 4) != words[i];
    CHECK(sum == 0, "paged copy reads back (%lu differ)", sum);
    CHECK(PlugPayCopy(&p, PLUG_PAY_PAGES_MAX * PLUG_PAGE - 2, words, 4) == PLUG_E2BIG, "copy past the pages refused");

    /* 256 KiB is the cap; more is ENOSPC */
    PlugXlInit(&x, PLUG_PAY_PAGES_MAX * PLUG_PAGE);
    CHECK(PlugXlCmd(&x, 0xFFFF) != PLUG_NONE && PlugXlCmd(&x, 0x1) != PLUG_NONE, "64K dwords + 1 fit");
    PlugXlInit(&x, PLUG_PAY_PAGES_MAX * PLUG_PAGE);
    (void)PlugXlCmd(&x, 0xFFFF);
    CHECK(PlugXlCmd(&x, 0x10) == PLUG_NONE && x.Err == PLUG_ENOSPC, "past 256 KiB: ENOSPC");
    PlugXlInit(&x, PLUG_PAY_PAGES_MAX * PLUG_PAGE + 1);
    CHECK(x.Err == PLUG_EINVAL, "capacity above 256 KiB refused");

    /* 128 entries at most */
    PlugXlInit(&x, 0);
    for (k = 0; k < 128; k++)
        PlugXlInline(&x, 0x00080004, k, 0, 2);
    CHECK(x.N == 128 && x.Err == PLUG_OK, "128 inline entries");
    PlugXlInline(&x, 0x00080004, 0, 0, 2);
    CHECK(x.Err == PLUG_E2BIG && x.N == 128, "entry 129: E2BIG");
    PlugXlInit(&x, 0);
    PlugXlInline(&x, 0x80000004, 0, 0, 0);
    CHECK(x.Err == PLUG_EINVAL, "an inline word with bit 31 refused");

    /* replies: 16-byte slots, never across a page, 8 at most, 1024 words at most */
    PlugXlInit(&x, 0);
    CHECK(PlugXlRx(&x, 770) == 0 && x.RxOff[0] == 0 && x.RxLen == 3088, "770-word reply");
    CHECK(PlugXlRx(&x, 1024) == 1 && x.RxOff[1] == PLUG_PAGE && x.RxLen == 2 * PLUG_PAGE, "1024 words: next page");
    CHECK(PlugXlRx(&x, 3) == 2 && x.RxOff[2] == 2 * PLUG_PAGE && x.RxLen == 2 * PLUG_PAGE + 16, "3 words: 16-byte slot");
    CHECK(PlugXlRx(&x, 4) == 3 && x.RxOff[3] == 2 * PLUG_PAGE + 16, "packed after it");
    CHECK(PlugXlRxPages(&x) == 3, "3 reply pages");
    for (k = 4; k < 8; k++)
        (void)PlugXlRx(&x, 4);
    CHECK(x.NRx == 8 && x.Err == PLUG_OK, "8 replies");
    CHECK(PlugXlRx(&x, 4) == PLUG_NONE && x.Err == PLUG_E2BIG, "reply 9: E2BIG");
    PlugXlInit(&x, 0);
    CHECK(PlugXlRx(&x, 1025) == PLUG_NONE && x.Err == PLUG_EINVAL, "1025-word reply refused");
    PlugXlInit(&x, 0);
    for (k = 0; k < 8; k++)
        CHECK(PlugXlRx(&x, 1024) == k, "1024-word reply %lu", k);
    CHECK(x.RxLen == 8 * PLUG_PAGE && PlugXlRxPages(&x) == 8, "8 x 1024 words = 8 pages");
}

/* ------------------------------------------------------------------ */
/* Ledger                                                              */
/* ------------------------------------------------------------------ */

/* The x8p's DSP 1, measured Nov 14 (docs/STRIPS.md 10.x log line): size, base, scratch. */
static const ULONG kSize[4] = { 0x00004000, 0x0000a9b9, 0x00001fff, 0x00ff516a };
static const ULONG kBase[4] = { 0x000e0000, 0x00092800, 0x000c5000, 0x08008e96 };
static const ULONG kScr[4]  = { 0, 0x00000080, 0x00000080, 0x00008000 };
static const ULONG kPer[4]  = { 0, STRIP_POOL1_PER_STRIP, STRIP_POOL2_PER_STRIP, STRIP_POOL3_PER_STRIP };

static int LedgerInvariant(const PLUG_LEDGER *L)
{
    ULONG i, k;

    for (i = 0; i < PLUG_POOLS; i++) {
        const PLUG_POOL *p = &L->Pool[i];
        ULONG lo = L->Base[i] + L->Scratch[i], hi = L->Base[i] + L->Words[i], total = 0;

        if (p->N == 0 || !p->E[p->N - 1].Terminal || p->E[p->N - 1].Cookie != 0)
            return 0;
        for (k = 0; k < p->N; k++) {
            const PLUG_EXTENT *e = &p->E[k];
            if (e->Words == 0 || (e->Words & 1) || (e->Base & 1) || e->Base < lo || e->Base + e->Words > hi)
                return 0;
            if (k + 1 < p->N) {
                const PLUG_EXTENT *n = &p->E[k + 1];
                if (e->Cookie == 0 && n->Cookie == 0)
                    return 0;                       /* two free neighbours: not merged */
                if (i == 0 ? (e->Base + e->Words != n->Base) : (n->Base + n->Words != e->Base))
                    return 0;                       /* not memory neighbours in list order */
                if (e->Terminal)
                    return 0;
            }
            total += e->Words;
        }
        if (total != hi - lo)
            return 0;
    }
    return 1;
}

static void TestLedgerStrips(void)
{
    static PLUG_LEDGER L;
    PLUG_LEASE s[4];
    PLUG_USAGE u[4];
    ULONG in, j, top[4], ok = 1;

    CHECK(PlugLedgerInit(&L, kBase, kSize, kScr) == PLUG_OK && LedgerInvariant(&L), "x8p DSP 1 geometry");
    for (j = 1; j < 4; j++)
        top[j] = L.Base[j] + L.Words[j];
    CHECK(top[1] == STRIP_TOP1 && top[2] == STRIP_TOP2 && top[3] == STRIP_TOP3, "rounded tops = the recipe's");
    CHECK(PlugLedgerBookStrips(&L, STRIP_CODE_WORDS, kPer, 8, s) == PLUG_OK, "strip room booked");
    CHECK(s[0].Base == 0xE0000 && s[0].Words == STRIP_CODE_WORDS, "strip code at 0xE0000, 994 words");
    for (j = 1; j < 4; j++)
        CHECK(s[j].Base == top[j] - 8 * kPer[j] && s[j].Words == 8 * kPer[j], "pool %lu: top %lu strips", j, 8ul);
    /* every row of every strip (strip_plan.h) lies inside the booked room, and they tile it */
    {
        ULONG used[4] = { 0, 0, 0, 0 };
        for (in = 0; in < STRIP_PLAN_INPUTS; in++) {
            ULONG a[STRIP_PLAN_ROWS];
            StripPlanAddrs(in, a);
            for (j = 0; j < STRIP_PLAN_ROWS; j++) {
                ULONG p = g_StripRows[j].Pool;
                if (a[j] < s[p].Base || a[j] + g_StripRows[j].Dwords > s[p].Base + s[p].Words)
                    ok = 0;
                used[p] += g_StripRows[j].Dwords;
            }
        }
        CHECK(ok, "StripPlanAddrs(0..7): every row inside the booked room");
        for (j = 1; j < 4; j++)
            CHECK(used[j] == s[j].Words, "pool %lu: the 8 strips use exactly the booked words", j);
    }
    /* the strip modules' pinned addresses are inside the code booking */
    CHECK(0x000E03A2u + 0x40u == 0xE0000u + STRIP_CODE_WORDS, "pins end at 0xE0000 + 994");
    PlugLedgerUsage(&L, u);
    CHECK(u[0].Allocations == 1 && u[1].Allocations == 1 && u[2].Allocations == 1 && u[3].Allocations == 1,
          "one booking per pool");
    /* review P0 finding 1: the Mic strip's scratch (0x1C0 / 0x80 / 0) over the registers' (0x80 / 0x80 / 0x8000) */
    CHECK(u[1].Scratch == 0x1C0 && u[2].Scratch == 0x80 && u[3].Scratch == 0x8000,
          "strip DSP scratch = max(register, Mic strip): 0x1C0 / 0x80 / 0x8000 (got 0x%lx 0x%lx 0x%lx)",
          u[1].Scratch, u[2].Scratch, u[3].Scratch);
    CHECK(L.ScratchMin[1] == 0x1C0 && L.ScratchMin[2] == 0x80 && L.ScratchMin[3] == 0x8000, "that is the ledger's floor");
    CHECK(L.Pool[1].E[L.Pool[1].N - 1].Base == kBase[1] + 0x1C0, "pool 1's lowest free word is base + 0x1C0, not + 0x80");
    CHECK(u[1].Free == top[1] - 8 * kPer[1] - (kBase[1] + 0x1C0), "pool 1 free = strip room bottom - (base + 0x1C0)");
    {
        PLUG_LEASE a;
        ULONG last = 0;
        CHECK(PlugLedgerReserve(&L, 1, u[1].Largest, 0, 0, &a) == PLUG_OK && a.Base >= kBase[1] + 0x1C0,
              "the largest pool-1 request never reaches below base + 0x1C0");
        CHECK(PlugLedgerRelease(&L, &a, &last) == PLUG_OK && last, "and back");
    }
    CHECK(PlugLedgerSetScratch(&L, 1, 0) == PLUG_OK && L.Scratch[1] == 0x1C0 && PlugLedgerSetScratch(&L, 2, 0) == PLUG_OK &&
          L.Scratch[2] == 0x80, "a scratch restore (a chain going away) never drops below the strips' floor");
    {
        ULONG scr[4] = { 0, STRIP_TOP1 - kBase[1] - 8 * kPer[1] + 2, 0x80, 0x8000 };
        CHECK(StripPlanLayoutCheck(kSize, kBase, kScr, 8, STRIP_CODE_WORDS) == 0, "the strips' layout check passes on DSP 1");
        CHECK(StripPlanLayoutCheck(kSize, kBase, scr, 8, STRIP_CODE_WORDS) != 0 && scr[1] > 0x1C0,
              "and refuses a pool-1 scratch that reaches into the strips' room");
    }
    {
        static PLUG_LEDGER L2;
        PLUG_LEASE s2[4];
        ULONG big[4] = { 0, 0x200, 0x80, 0x8000 };
        CHECK(PlugLedgerInit(&L2, kBase, kSize, big) == PLUG_OK && PlugLedgerBookStrips(&L2, STRIP_CODE_WORDS, kPer, 8, s2) == PLUG_OK &&
              L2.Scratch[1] == 0x200 && L2.ScratchMin[1] == 0x200, "a register scratch above the strip's (0x200) is kept");
    }
    CHECK(u[0].Free == 0x4000 - 994 && u[0].Largest == 0x4000 - 994 - 2, "code: 15390 free, 15388 largest");
    CHECK(PlugLedgerIdle(&L, PLUG_ID_STRIPS) && !PlugLedgerIdle(&L, 0), "idle but for the strips");
    CHECK(LedgerInvariant(&L), "invariant after booking");

    /*
     * [Virtual] docs/STRIPS.md 13: plugin.c books StripPlanRoom (the Mic strips plus StripVirtualMask's Input Strips) through
     * PlugLedgerBookStripRoom. Without Virtual strips that is PlugLedgerBookStrips' booking exactly; with them one block per
     * pool still ends at the top and holds every Input Strip block, so no plug-in reservation can land on one.
     */
    {
        static PLUG_LEDGER L3, L4;
        PLUG_LEASE s3[4], s4[4], a;
        ULONG room[4], vm, v, ok2 = 1, a2[STRIP_PLAN_ROWS];
        PLUG_USAGE u3[4];

        StripPlanRoom(8, 0, room);
        CHECK(PlugLedgerInit(&L3, kBase, kSize, kScr) == PLUG_OK && PlugLedgerBookStripRoom(&L3, STRIP_CODE_WORDS, room, s3) == PLUG_OK &&
              memcmp(s3, s, sizeof(s3)) == 0, "virtual: no Virtual strip: the same booking as PlugLedgerBookStrips (8 Mic strips)");
        for (j = 1; j < 4; j++)
            CHECK(s3[j].Base == s[j].Base && s3[j].Words == s[j].Words, "virtual: pool %lu booked as before", j);
        for (vm = 0x01; vm <= 0xFF && ok2; vm = (vm << 1) | 1) {
            StripPlanRoom(8, vm, room);
            ok2 = PlugLedgerInit(&L4, kBase, kSize, kScr) == PLUG_OK &&
                  PlugLedgerBookStripRoom(&L4, STRIP_CODE_WORDS, room, s4) == PLUG_OK && LedgerInvariant(&L4);
            for (j = 1; ok2 && j < 4; j++)
                ok2 = s4[j].Base + s4[j].Words == top[j] && s4[j].Words == room[j];
            for (v = 0; ok2 && v < 8; v++) {
                if (!(vm & (1u << v)))
                    continue;
                ok2 = StripPlanVirtualAddrs(8, v, a2) == 0;
                for (j = 0; ok2 && j < STRIP_PLAN_VROWS; j++) {
                    ULONG p = g_StripVRows[j].Pool;
                    ok2 = a2[j] >= s4[p].Base && a2[j] + g_StripVRows[j].Dwords <= s4[p].Base + s4[p].Words;
                }
            }
            /* the largest plug-in reservation of each pool stays below the booked room */
            PlugLedgerUsage(&L4, u3);
            for (j = 1; ok2 && j < 4; j++) {
                ok2 = PlugLedgerReserve(&L4, j, u3[j].Largest, 0, 0, &a) == PLUG_OK && a.Base + a.Words <= s4[j].Base;
                (void)PlugLedgerRelease(&L4, &a, NULL);
            }
        }
        CHECK(ok2, "virtual: x8p with VIRTUAL 1, 1-2, ..., 1-8: one block per pool from the top, every Input Strip block inside "
                   "it, the largest plug-in reservation below it");
    }
}

/*
 * A chain as native_carrier.py lays one out: the head and the Mic strip rows (head_rows(), open-apollo),
 * then a synthetic effect of four rows. No plug-in's own descriptor: the effect's sizes are invented.
 */
static const PLUG_ROW kChainRows[] = {
    { 1, 10, 0xFFFFFFFFu, 0 }, { 1, 56, 6, 0 }, { 2, 16, 0x01000036, 2 }, { 1, 16, 0x01000037, 2 },
    { 3, 16, 0x0100001B, 0 }, { 1, 4, 0x0100001C, 0 }, { 3, 2048, 0x0100000E, 2 }, { 3, 2048, 0x0100000F, 2 },
    { 3, 2048, 0x01000006, 2 }, { 3, 2048, 0x01000007, 2 },
    { 1, 6001, 8, 0 }, { 1, 25, 0x0A000030, 0 }, { 1, 20, 0x0A000031, 2 }, { 2, 20, 0x0A000032, 2 },
};
#define NCHAIN (sizeof(kChainRows) / sizeof(kChainRows[0]))

static void TestLedgerChain(void)
{
    static PLUG_LEDGER L, base;
    PLUG_LEASE s[4], lease[3 + NCHAIN], lease2[3];
    PLUG_REQ rq[3 + NCHAIN];
    PLUG_USAGE u0[4], u1[4];
    ULONG i, j, last, ok;

    (void)PlugLedgerInit(&L, kBase, kSize, kScr);
    (void)PlugLedgerBookStrips(&L, STRIP_CODE_WORDS, kPer, 8, s);
    base = L;
    PlugLedgerUsage(&L, u0);

    /* modules (synthetic): code 3000 words (shared), data kind 2 1000 words, kind 1 1500 words */
    rq[0].Kind = 0; rq[0].Words = 3000; rq[0].Identity = 101; rq[0].Shared = 1;
    rq[1].Kind = 2; rq[1].Words = 1000; rq[1].Identity = 102; rq[1].Shared = 1;
    rq[2].Kind = 1; rq[2].Words = 1500; rq[2].Identity = 103; rq[2].Shared = 1;
    for (i = 0; i < NCHAIN; i++) {
        rq[3 + i].Kind = kChainRows[i].Pool;
        rq[3 + i].Words = kChainRows[i].Dwords;
        rq[3 + i].Identity = 0;
        rq[3 + i].Shared = 0;
    }
    CHECK(PlugLedgerSetScratch(&L, 1, 448) == PLUG_OK && PlugLedgerSetScratch(&L, 2, 192) == PLUG_OK &&
          PlugLedgerSetScratch(&L, 3, 0) == PLUG_OK, "chain scratch raised (448, 192, 0)");
    CHECK(L.Scratch[1] == 448 && L.Scratch[2] == 192 && L.Scratch[3] == 0x8000, "scratch never below the register's");
    CHECK(PlugLedgerReserveBatch(&L, rq, 3 + NCHAIN, lease) == PLUG_OK && LedgerInvariant(&L), "chain placed");
    CHECK(lease[0].Base == 0xE0000 + STRIP_CODE_WORDS && lease[0].Words == 3000, "plug-in code right after the strips' (0x%lx)",
          lease[0].Base);
    ok = 1;
    for (i = 0; i < 3 + NCHAIN; i++) {
        ULONG p = lease[i].Kind;
        if (p != 0 && lease[i].Base + lease[i].Words > s[p].Base)
            ok = 0;                         /* data below the strips' room */
        if (p != 0 && lease[i].Base < L.Base[p] + L.Scratch[p])
            ok = 0;                         /* above the scratch */
        for (j = 0; j < i; j++)
            if (lease[j].Kind == p && lease[i].Base < lease[j].Base + lease[j].Words &&
                lease[j].Base < lease[i].Base + lease[i].Words)
                ok = 0;
        if (lease[i].Base & 1)
            ok = 0;
    }
    CHECK(ok, "no overlap with each other, the strips or the scratch; all even");
    CHECK(lease[2].Base == s[1].Base - 1500 && lease[3].Base == lease[2].Base - 10,
          "pool 1: the module right below the strips, then the chain head (0x%lx)", lease[3].Base);
    CHECK(lease[3 + 10].Words == 6002, "6001-word row rounded to 6002");

    /* a second instance shares the modules (holds), not their words */
    CHECK(PlugLedgerReserveBatch(&L, rq, 3, lease2) == PLUG_OK && lease2[0].Base == lease[0].Base &&
          lease2[1].Base == lease[1].Base && lease2[2].Base == lease[2].Base, "shared modules: same extents, new holds");
    PlugLedgerUsage(&L, u1);
    CHECK(u1[0].Holds == 3 && u1[0].Allocations == 2, "pool 0: strips + one shared module with 2 holds");
    {
        PLUG_REQ bad = { 0, 3002, 101, 1 };
        PLUG_LEASE x;
        CHECK(PlugLedgerReserve(&L, bad.Kind, bad.Words, bad.Identity, bad.Shared, &x) == PLUG_EEXIST,
              "same identity, other size: EEXIST");
    }
    CHECK(lease2[0].Cookie != lease[0].Cookie, "each holder has its own ticket");
    for (i = 0; i < 3; i++)
        CHECK(PlugLedgerRelease(&L, &lease2[i], &last) == PLUG_OK && !last, "second holder released: extent stays");
    CHECK(PlugLedgerRelease(&L, &lease2[0], &last) == PLUG_EINVAL && !last,
          "a repeated release is refused: it cannot drop the other holder's hold");
    CHECK(PlugLedgerReserve(&L, 0, 3000, 101, 1, &lease2[0]) == PLUG_OK, "hold taken again");

    /* everything back, scratch back: equal to the post-booking baseline */
    for (i = 3 + NCHAIN; i > 0; i--) {
        CHECK(PlugLedgerRelease(&L, &lease[i - 1], &last) == PLUG_OK, "release %lu", i - 1);
        if (i - 1 >= 3)
            CHECK(last, "row %lu: last hold", i - 4);
    }
    CHECK(PlugLedgerRelease(&L, &lease2[0], &last) == PLUG_OK && last, "module: last hold");
    CHECK(PlugLedgerSetScratch(&L, 1, 0) == PLUG_OK && PlugLedgerSetScratch(&L, 2, 0) == PLUG_OK, "scratch back");
    PlugLedgerUsage(&L, u1);
    CHECK(memcmp(u0, u1, sizeof(u0)) == 0 && LedgerInvariant(&L), "ledger back to baseline");
    CHECK(L.Pool[0].N == base.Pool[0].N && L.Pool[1].N == base.Pool[1].N && L.Pool[2].N == base.Pool[2].N &&
          L.Pool[3].N == base.Pool[3].N, "extent lists back to two per pool");
    CHECK(PlugLedgerRelease(&L, &lease[0], &last) == PLUG_EINVAL, "double release refused");

    /* all or nothing */
    {
        PLUG_REQ big[2] = { { 1, 100, 0, 0 }, { 0, 0x4000, 7, 1 } };
        PLUG_LEASE out[2];
        CHECK(PlugLedgerReserveBatch(&L, big, 2, out) == PLUG_ENOSPC && out[0].Cookie == 0, "batch: ENOSPC, nothing kept");
        PlugLedgerUsage(&L, u1);
        CHECK(memcmp(u0, u1, sizeof(u0)) == 0, "batch rollback leaves the baseline");
    }
    /* the terminal extent keeps 2 words: a request of exactly what is left does not fit */
    {
        PLUG_LEASE a;
        PlugLedgerUsage(&L, u1);
        CHECK(PlugLedgerReserve(&L, 2, u1[2].Free, 0, 0, &a) == PLUG_ENOSPC, "terminal extent never taken whole");
        CHECK(PlugLedgerReserve(&L, 2, u1[2].Largest, 0, 0, &a) == PLUG_OK, "largest fits");
        CHECK(PlugLedgerSetScratch(&L, 2, 0x200) == PLUG_ENOSPC, "scratch cannot grow into an allocation's room");
        CHECK(PlugLedgerRelease(&L, &a, &last) == PLUG_OK && last, "and back");
    }
    CHECK(PlugLedgerReserve(&L, 1, 4, 0, 1, &lease2[0]) == PLUG_EINVAL, "shared without identity refused");
    {
        static PLUG_LEDGER bad;
        ULONG sz[4] = { 0x4000, 0x81, 0x10, 0x10 }, bs[4] = { 0xE0000, 0, 0, 0 }, sc[4] = { 0, 0x80, 0, 0 };
        CHECK(PlugLedgerInit(&bad, bs, sz, sc) == PLUG_EINVAL, "scratch + 2 > size refused");
        sc[1] = 0; sc[0] = 2;
        CHECK(PlugLedgerInit(&bad, bs, sz, sc) == PLUG_EINVAL, "code scratch refused");
        sc[0] = 0; sz[3] = 0xFFFFFFFF;
        CHECK(PlugLedgerInit(&bad, bs, sz, sc) == PLUG_EINVAL, "all-ones register refused");
    }
}

/* Random churn: invariants hold, words are conserved, leases never overlap. */
/* Review P0 finding 5: 127 rows fill the privates transaction exactly (127 zero-fills + the memspec). */
static void TestPrivMax(void)
{
    static PLUG_XL x;
    static PLUG_ROW rows[PLUG_PRIV_MAX + 1];
    static ULONG addr[PLUG_PRIV_MAX + 1], w2[2 * (PLUG_PRIV_MAX + 1)], w1[PLUG_PRIV_MAX + 1];
    PLUG_PAY p;
    ULONG i;

    CHECK(PLUG_PRIV_MAX == 127 && PLUG_PRIV_MAX + 1 == PLUG_XACT_ENTS_MAX, "row limit 127: one entry left for the memspec");
    rows[0].Pool = 1; rows[0].Dwords = 256; rows[0].Link = 0xFFFFFFFFu; rows[0].Flags = 0;
    addr[0] = 0x10000;
    for (i = 1; i <= PLUG_PRIV_MAX; i++) {
        rows[i].Pool = 1 + i % 3;
        rows[i].Dwords = 2;
        rows[i].Link = i - 1;                   /* row 0 word i - 1 */
        rows[i].Flags = 0;
        addr[i] = 0x10000 + 0x200 * i;
    }
    CHECK(PlugCheckRows(rows, PLUG_PRIV_MAX, w2, w1) == PLUG_OK, "127 rows: a valid descriptor");
    CHECK(PlugCheckRows(rows, PLUG_PRIV_MAX + 1, w2, w1) == PLUG_EINVAL, "128 rows: refused before anything is placed");
    PayInit(&p, 1);
    PlugXlInit(&x, PLUG_PAGE);
    CHECK(PlugBuildPrivates(&x, &p, rows, PLUG_PRIV_MAX, addr, w2) == PLUG_OK && x.N == PLUG_XACT_ENTS_MAX &&
          x.Ent[127][0] == (PLUG_RING_DMA_REF | 253) && PayWord(&p, 0) == (PLUG_CMD_MEMSPEC | 253),
          "127 rows: 127 zero-fills + a 253-word memspec = 128 entries");
    PlugXlInit(&x, PLUG_PAGE);
    CHECK(PlugBuildPrivates(&x, &p, rows, PLUG_PRIV_MAX + 1, addr, w2) != PLUG_OK, "128 rows: no transaction");
}

static void TestLedgerChurn(void)
{
    static PLUG_LEDGER L;
    static PLUG_LEASE held[400];
    ULONG n = 0, step, i, j, bad = 0, seed = 12345;
    PLUG_LEASE s[4];
    PLUG_USAGE u0[4], u1[4];

    (void)PlugLedgerInit(&L, kBase, kSize, kScr);
    (void)PlugLedgerBookStrips(&L, STRIP_CODE_WORDS, kPer, 8, s);
    PlugLedgerUsage(&L, u0);
    for (step = 0; step < 20000; step++) {
        seed = seed * 1103515245u + 12345u;
        if (n < 300 && ((seed >> 16) % 3) != 0) {
            ULONG kind = (seed >> 8) % 4, words = 1 + (seed >> 18) % (kind == 3 ? 5000 : 300);
            PLUG_LEASE a;
            ULONG e = PlugLedgerReserve(&L, kind, words, 0, 0, &a);
            if (e == PLUG_OK)
                held[n++] = a;
            else if (e != PLUG_ENOSPC)
                bad++;
        } else if (n > 0) {
            ULONG k = (seed >> 12) % n, last = 0;
            if (PlugLedgerRelease(&L, &held[k], &last) != PLUG_OK || !last)
                bad++;
            held[k] = held[--n];
        }
        if ((step % 97) == 0 && !LedgerInvariant(&L))
            bad++;
    }
    for (i = 0; i < n; i++)
        for (j = 0; j < i; j++)
            if (held[i].Kind == held[j].Kind && held[i].Base < held[j].Base + held[j].Words &&
                held[j].Base < held[i].Base + held[i].Words)
                bad++;
    CHECK(bad == 0 && LedgerInvariant(&L), "20000 random reserve/release steps (%lu held at the end)", n);
    while (n > 0) {
        ULONG last;
        (void)PlugLedgerRelease(&L, &held[--n], &last);
    }
    PlugLedgerUsage(&L, u1);
    CHECK(memcmp(u0, u1, sizeof(u0)) == 0 && LedgerInvariant(&L), "churn: back to the baseline");
}

/* ------------------------------------------------------------------ */
/* A synthetic recipe, built as native_carrier.py builds one           */
/* ------------------------------------------------------------------ */

static ULONG g_cmd[2048], g_nw;

/* native_carrier.write(): one masked WRITE of {word: value}, span <= 33, appended to Buf at *NW */
static void WriteTo(ULONG *Buf, ULONG *NW, ULONG row, const ULONG *pos, const ULONG *val, ULONG n)
{
    ULONG first = pos[0], last = pos[n - 1], mask = 0, p, k, have;

    for (p = first + 1; p <= last; p++) {
        for (k = 0, have = 0; k < n; k++)
            if (pos[k] == p)
                have = 1;
        if (!have)
            mask |= 1u << (p - first - 1);
    }
    Buf[(*NW)++] = (PLUG_CMD_WRITE << 16) | (3 + n);
    Buf[(*NW)++] = (row << 24) | first;
    Buf[(*NW)++] = mask;
    for (k = 0; k < n; k++)
        Buf[(*NW)++] = val[k];
}

static void Write(ULONG row, const ULONG *pos, const ULONG *val, ULONG n)
{
    WriteTo(g_cmd, &g_nw, row, pos, val, n);
}

#define RENDER  0x9A5       /* synthetic */
/* plug-in rows (descriptor-local links), then the chain rows as the carrier links them */
static const PLUG_ROW kFx[3] = { { 1, 40, 0xFFFFFFFFu, 0 }, { 2, 16, 0x00000014, 2 }, { 3, 64, 0x00000015, 0 } };
static PLUG_ROW g_rows[13];

static void BuildRecipe(ULONG renderId, ULONG members)
{
    static const ULONG hp[7] = { 0, 1, 2, 4, 5, 7, 9 };
    ULONG hv[7] = { 0x40000000, 1, 1, 0x22, 0, 0x846, 0 };
    /* MIC_STRIP blocks (native_carrier.py:22-30), rows 1 + offset */
    static const ULONG m0v[15] = { 0x40000000, 1, 1, 0x800, 0x3a888889, 0xc0000000, 0xc0000000, 0x10, 0x4b,
                                   0x41000000, 0x3ada740e, 0xFFFFFFFF, 1, 0xFFFFFFFF, 1 };
    ULONG i, k;

    hv[4] = members;
    hv[6] = renderId;
    g_nw = 0;
    Write(0, hp, hv, 7);
    /* {0x0014LLLL, row << 24 | word, mask, values} as the carrier emits them (raw, precomputed mask) */
    g_cmd[g_nw++] = 0x00140000u | (15 + 3); g_cmd[g_nw++] = 0x01000000u; g_cmd[g_nw++] = 0x0f1fffec;
    for (k = 0; k < 15; k++)
        g_cmd[g_nw++] = m0v[k];
    {
        ULONG p16 = 16, v = 22;
        Write(1, &p16, &v, 1);                  /* record delay */
    }
    for (i = 0; i < 10; i++)
        g_rows[i] = kChainRows[i];
    for (i = 0; i < 3; i++) {
        g_rows[10 + i] = kFx[i];
        g_rows[10 + i].Link = (i == 0) ? 8 : ((10 + (kFx[i].Link >> 24)) << 24) | (kFx[i].Link & 0xFFFFFFu);
    }
    {
        /* the effect's startup words: row 10 words 0,1,2,4 (header, host-owned), 10..12; row 12 words 0..3 */
        ULONG p10[7] = { 0, 1, 2, 4, 10, 11, 12 }, v10[7] = { 0x40000000, 1, 1, 4, 0x3f800000, 0x3f000000, 7 };
        ULONG p12[4] = { 0, 1, 2, 3 }, v12[4] = { 11, 22, 33, 44 };
        Write(10, p10, v10, 7);
        Write(12, p12, v12, 4);
    }
}

static void TestRecipe(void)
{
    static PLUG_XL x;
    static PLUG_LEDGER L;
    PLUG_PAY p;
    ULONG s2[256], s1[128], ncmds = 0, ids[5], nids = 0, carrier = 0, addr[13], reloc[2048], mw[256], i, k;
    PLUG_LEASE strips[4], lease[13];
    PLUG_REQ rq[13];

    BuildRecipe(RENDER, 2);
    CHECK(PlugCheckRows(g_rows, 13, s2, s1) == PLUG_OK, "chain rows are one descriptor");
    CHECK(PlugCheckCmds(g_rows, 13, g_cmd, g_nw, &ncmds) == PLUG_OK && ncmds == 5, "5 WRITE blocks (%lu)", ncmds);
    CHECK(PlugChainIds(g_rows, 13, g_cmd, g_nw, 1, RENDER, ids, &nids, &carrier) == PLUG_OK && nids == 1 &&
          ids[0] == RENDER && carrier == PLUG_MIC_CARRIER, "directory: Mic carrier + render 0x%x", RENDER);
    CHECK(PlugChainIds(g_rows, 13, g_cmd, g_nw, 1, RENDER + 1, ids, &nids, &carrier) == PLUG_EINVAL,
          "directory: render id must be the first effect");
    CHECK(PlugChainIds(g_rows, 13, g_cmd, g_nw, 0, RENDER, ids, &nids, &carrier) == PLUG_OK, "N=2 without CHAIN: fine");
    CHECK(PlugWritesDirectory(g_cmd, g_nw), "the startup touches the directory");
    {
        ULONG w[5] = { 0x00140005, 0x0A00000A, 0, 1, 2 };
        CHECK(!PlugWritesDirectory(w, 5) && PlugCheckCmds(g_rows, 13, w, 5, &ncmds) == PLUG_OK,
              "an effect-row WRITE is not a directory write");
    }
    BuildRecipe(0x7FF, 2);
    CHECK(PlugChainIds(g_rows, 13, g_cmd, g_nw, 1, 0x7FF, ids, &nids, &carrier) == PLUG_EACCES, "id below the table: EACCES");
    BuildRecipe(RENDER, 3);
    CHECK(PlugChainIds(g_rows, 13, g_cmd, g_nw, 0, RENDER, ids, &nids, &carrier) == PLUG_ENOTSUP, "N=3 without CHAIN refused");
    CHECK(PlugChainIds(g_rows, 13, g_cmd, g_nw, 1, RENDER, ids, &nids, &carrier) == PLUG_EINVAL,
          "N=3 with a 10-word head refused");
    BuildRecipe(RENDER, 2);
    /* a value on a linked word (head word 6 = the Mic m0 pointer) */
    {
        ULONG w[4] = { 0x00140004, 0x00000006, 0, 1 };
        CHECK(PlugCheckCmds(g_rows, 13, w, 4, &ncmds) == PLUG_EINVAL, "WRITE onto a linked word refused");
        w[1] = 0x00000005; w[2] = 0;
        CHECK(PlugCheckCmds(g_rows, 13, w, 4, &ncmds) == PLUG_OK, "word 5 is not linked");
        w[1] = 0x00000005; w[2] = 1;          /* skip word 6: the value lands on word 5 */
        CHECK(PlugCheckCmds(g_rows, 13, w, 4, &ncmds) == PLUG_OK, "masked: one value on word 5");
    }
    {
        ULONG w[6] = { 0x00140006, 0x00000004, 0, 1, 2, 3 };    /* words 4, 5, 6: 6 is linked */
        CHECK(PlugCheckCmds(g_rows, 13, w, 6, &ncmds) == PLUG_EINVAL, "a run over a linked word refused");
        w[2] = 2;                                               /* skip word 6: 4, 5, 7 */
        CHECK(PlugCheckCmds(g_rows, 13, w, 6, &ncmds) == PLUG_OK, "the skip mask steps over it");
    }
    {
        ULONG w[5] = { 0x00140005, 0x0500000 | 3, 0, 1, 2 };  /* row 5 (4 dwords): words 3, 4 */
        w[1] = (5u << 24) | 3;
        CHECK(PlugCheckCmds(g_rows, 13, w, 5, &ncmds) == PLUG_EINVAL, "WRITE past its row refused");
        w[1] = (13u << 24) | 0;
        CHECK(PlugCheckCmds(g_rows, 13, w, 5, &ncmds) == PLUG_EINVAL, "WRITE to a row that does not exist refused");
        w[0] = 0x00150005; w[1] = (5u << 24) | 0;
        CHECK(PlugCheckCmds(g_rows, 13, w, 5, &ncmds) == PLUG_EINVAL, "not a WRITE opcode refused");
        w[0] = 0x00140009;
        CHECK(PlugCheckCmds(g_rows, 13, w, 5, &ncmds) == PLUG_EINVAL, "length past the buffer refused");
    }
    {
        PLUG_ROW r[2] = { { 1, 8, 0xFFFFFFFFu, 0 }, { 2, 4, 0x00000008, 0 } };
        CHECK(PlugCheckRows(r, 2, s2, s1) == PLUG_EINVAL, "link past the target row refused");
        r[1].Link = 0x02000000;
        CHECK(PlugCheckRows(r, 2, s2, s1) == PLUG_EINVAL, "link to a row that does not exist refused");
        r[1].Link = 7; r[1].Flags = 1;
        CHECK(PlugCheckRows(r, 2, s2, s1) == PLUG_EINVAL, "flags 1 refused");
        r[1].Flags = 2; r[0].Link = 0;
        CHECK(PlugCheckRows(r, 2, s2, s1) == PLUG_EINVAL, "row 0 must have no link");
        r[0].Link = 0xFFFFFFFFu; r[1].Pool = 4;
        CHECK(PlugCheckRows(r, 2, s2, s1) == PLUG_EINVAL, "pool 4 refused");
        CHECK(PlugCheckRows(r, 1, s2, s1) == PLUG_OK, "one row");
    }
    CHECK(PlugInputsOverlap(0x00200000, 0x00200000) && !PlugInputsOverlap(0x00200000, 0x00200001) &&
          PlugInputsOverlap(0x00010000, 0x00200001) && !PlugInputsOverlap(0x00200002, 0x00200003),
          "input overlap (0x20 = mono)");

    /* place it on the x8p's DSP 1 beside the strips */
    (void)PlugLedgerInit(&L, kBase, kSize, kScr);
    (void)PlugLedgerBookStrips(&L, STRIP_CODE_WORDS, kPer, 8, strips);
    for (i = 0; i < 13; i++) {
        rq[i].Kind = g_rows[i].Pool;
        rq[i].Words = g_rows[i].Dwords;
        rq[i].Identity = 0;
        rq[i].Shared = 0;
    }
    CHECK(PlugLedgerReserveBatch(&L, rq, 13, lease) == PLUG_OK, "13 rows placed");
    for (i = 0; i < 13; i++)
        addr[i] = lease[i].Base;

    /* privates: 13 zero-fills (even), one memspec of 25 words */
    PayInit(&p, 1);
    PlugXlInit(&x, PLUG_PAGE);
    CHECK(PlugBuildPrivates(&x, &p, g_rows, 13, addr, mw) == PLUG_OK && x.N == 14 && x.NRx == 0, "privates: 14 entries");
    for (i = 0, k = 1; i < 13; i++)
        k &= x.Ent[i][0] == PLUG_CMD_ZERO && x.Ent[i][1] == addr[i] && x.Ent[i][2] == 0 &&
             x.Ent[i][3] == g_rows[i].Dwords + (g_rows[i].Dwords & 1);
    CHECK(k, "zero-fills: address and even length per row");
    CHECK(x.Ent[13][0] == (PLUG_RING_DMA_REF | 25) && PayWord(&p, 0) == (PLUG_CMD_MEMSPEC | 25), "memspec 0x00150019");
    CHECK(PayWord(&p, 4) == addr[0] + 6 && PayWord(&p, 8) == addr[1], "memspec: Mic m0 -> head[6]");
    CHECK(PayWord(&p, 4 + 8 * 9) == addr[0] + 8 && PayWord(&p, 8 + 8 * 9) == addr[10], "memspec: effect root -> head[8]");
    CHECK(PayWord(&p, 4 + 8 * 10) == addr[10] + 0x14 && PayWord(&p, 8 + 8 * 10) == addr[11],
          "memspec: effect row 1 -> root[0x14]");
    FnvXl(&x, &p);

    /* START: stop the plain strip, the WRITEs relocated, SYNCH the head, span, span, route */
    PayInit(&p, 1);
    PlugXlInit(&x, PLUG_PAGE);
    CHECK(PlugBuildStart(&x, &p, g_cmd, g_nw, addr, 13, 0x00200000, reloc) == PLUG_OK && x.N == 6, "start: 6 entries");
    CHECK(x.Ent[0][0] == PLUG_CMD_SYNCH && x.Ent[0][1] == 0x00200000 && x.Ent[0][2] == 0, "stop input 1's strip");
    CHECK(x.Ent[1][0] == (PLUG_RING_DMA_REF | g_nw) && x.Ent[1][2] == 0, "WRITEs: one DMA command of %lu words", g_nw);
    CHECK(x.Ent[2][0] == PLUG_CMD_SYNCH && x.Ent[2][1] == 0x00200000 && x.Ent[2][2] == addr[0], "SYNCH the chain head");
    CHECK(x.Ent[3][0] == PLUG_CMD_INPUT && x.Ent[3][2] == PLUG_INPUT_SPAN && x.Ent[3][3] == PLUG_SPAN_NONE &&
          x.Ent[4][2] == PLUG_INPUT_SPAN && x.Ent[5][2] == PLUG_INPUT_ROUTE && x.Ent[5][3] == PLUG_ROUTE_NONE,
          "span none twice, route 0x1FFFF");
    for (i = 0, k = 1; i < g_nw; i += g_cmd[i] & 0xFFFF) {
        ULONG row = g_cmd[i + 1] >> 24, j;
        k &= PayWord(&p, (i + 1) * 4) == addr[row] + (g_cmd[i + 1] & 0xFFFFFF);
        for (j = 0; j < (g_cmd[i] & 0xFFFF); j++)
            if (j != 1)
                k &= PayWord(&p, (i + j) * 4) == g_cmd[i + j];
    }
    CHECK(k, "every WRITE: word 1 relocated to its row's address, the rest unchanged");
    FnvXl(&x, &p);
    {
        ULONG bad[4] = { 0x00140004, 0x0D000000, 0, 1 };
        PlugXlInit(&x, PLUG_PAGE);
        CHECK(PlugBuildStart(&x, &p, bad, 4, addr, 13, 0x00200000, reloc) == PLUG_EINVAL, "relocation of row 13 refused");
    }

    /* teardown: stop the chain, restore input 1's plain strip at its header, then releases */
    {
        ULONG hdr[2], sa[STRIP_PLAN_ROWS];
        PLUG_BILL code = { 0x301, 0x02000000, 0, 0, 2, 0, 3000, 0 }, data = { 0x302, 2, 2, 0, 0, 1000, 1000, 2 },
                  k4 = { 0x303, 4, 4, 0, 0, 10, 10, 1 };
        StripPlanAddrs(0, sa);
        hdr[0] = sa[0];
        hdr[1] = 0;
        PlugXlInit(&x, 0);
        CHECK(PlugBuildStop(&x, 0x00200000, 1, 1, hdr) == PLUG_OK && x.N == 5, "stop + restore: 5 entries");
        PlugBuildRelease(&x, &code);
        PlugBuildRelease(&x, &data);
        PlugBuildRelease(&x, &k4);
        CHECK(x.Ent[0][0] == PLUG_CMD_SYNCH && x.Ent[0][1] == 0x00200000 && x.Ent[0][2] == 0, "chain stopped (head 0)");
        CHECK(x.Ent[1][0] == PLUG_CMD_SYNCH && x.Ent[1][1] == 0x00200000 && x.Ent[1][2] == 0x9D1B0, "strip 1 back at 0x9D1B0");
        CHECK(x.Ent[4][2] == PLUG_INPUT_ROUTE && x.Ent[4][3] == PLUG_ROUTE_NONE, "its route word");
        CHECK(x.Ent[5][0] == PLUG_CMD_RELEASE_CODE && x.Ent[5][1] == 0x301 && x.Ent[6][0] == PLUG_CMD_RELEASE_DATA &&
              x.Ent[6][2] == 2 && x.Ent[7][2] == 1, "releases: code {0x00030002}, data {0x00060003, kind (4 -> 1)}");
        FnvXl(&x, NULL);
        PlugXlInit(&x, 0);
        CHECK(PlugBuildStop(&x, 0x00200003, 1, 0, hdr) == PLUG_OK && x.N == 1 && x.Ent[0][1] == 0x00200003,
              "D0 exit: stop only");
    }

    /* upload: the record copied whole, bytes 10-11 cleared in the copy only */
    {
        static UCHAR rec[8192];
        PLUG_BILL b;
        ULONG w[5] = { PLUG_BILL_MAGIC, 0x301, 0x02000000, sizeof(rec) - 20, 1998 }, off;
        memset(rec, 0xA5, sizeof(rec));
        memcpy(rec, w, sizeof(w));
        CHECK(PlugBillCheck(rec, sizeof(rec), &b) == PLUG_OK && b.Words == 1332, "synthetic 8 KiB code record");
        PayInit(&p, 3);
        PlugXlInit(&x, 3 * PLUG_PAGE);
        CHECK(PlugBuildUpload(&x, &p, &b, rec, sizeof(rec), 0xE03E2) == PLUG_OK && x.NRx == 1 && x.RxWords[0] == 4 &&
              x.N == 3 && RefsOk(&x), "upload: 1 reply, 3 page pieces");
        off = x.Ent[0][2];
        CHECK(PayWord(&p, off) == (PLUG_CMD_UPLOAD_CODE | (8192 / 4 + 2)) && PayWord(&p, off + 4) == 0xE03E2,
              "upload header and destination");
        CHECK(PayWord(&p, off + 8) == PLUG_BILL_MAGIC && PayWord(&p, off + 8 + 8) == 0 && PayWord(&p, off + 12 + 8) == w[3],
              "record word 2 cleared (0x02000000 -> 0)");
        CHECK(rec[10] == 0 && rec[11] == 2, "the source record is untouched");
        CHECK(PayWord(&p, off + 8 + 8188) == 0xA5A5A5A5, "last record word copied");
        FnvXl(&x, &p);
    }

    /* SERVICE read */
    PlugXlInit(&x, 0);
    CHECK(PlugBuildRead(&x, addr[12] + 1, 3, 1) == PLUG_OK && x.NRx == 1 && x.RxWords[0] == 5 &&
          x.RxFill == PLUG_READ_FILL && x.Ent[0][0] == PLUG_CMD_READ && x.Ent[0][1] == addr[12] + 1 &&
          x.Ent[0][2] == 3 && x.Ent[0][3] == 1, "read: {0x000C0004, addr, n, word}, reply n + 2");
    {
        ULONG r[5] = { 0x80010005, 1, 22, 33, 44 };
        CHECK(PlugReadReplyOk(r, 3, 1), "read reply accepted");
        r[1] = 2;
        CHECK(!PlugReadReplyOk(r, 3, 1), "another word's reply refused");
        r[1] = 1; r[0] = 0x80010004;
        CHECK(!PlugReadReplyOk(r, 3, 1), "another length refused");
    }
    FnvXl(&x, NULL);
    for (i = 0; i < 13; i++)
        Fnv(addr[i]);
    for (i = 0; i < g_nw; i++)
        Fnv(g_cmd[i]);
}

/* ------------------------------------------------------------------ */
/* Staged startup (a startup over one 1016-word page): CREATE flags,    */
/* the page cut against upstream's chunks(), stage / SERVICE / START    */
/* ------------------------------------------------------------------ */

static void TestCreateFlags(void)
{
    ULONG f, p0, now;

    /* without STAGE the rule is P0's: CHAIN needs START, START needs CHAIN, CHECK alone (finding 2) */
    for (f = 0; f < 16; f++) {
        if (f & PLUG_F_STAGE)
            continue;
        p0 = !(((f & PLUG_F_CHAIN) && !(f & PLUG_F_START)) || ((f & PLUG_F_START) && !(f & PLUG_F_CHAIN)) ||
               ((f & PLUG_F_CHECK) && (f & (PLUG_F_START | PLUG_F_CHAIN))));
        now = PlugCreateFlagsOk(f) == PLUG_OK;
        CHECK(p0 == now, "flags 0x%lx: %s as in P0", f, p0 ? "accepted" : "refused");
    }
    CHECK(PlugCreateFlagsOk(PLUG_F_STAGE | PLUG_F_CHAIN) == PLUG_OK, "STAGE | CHAIN accepted");
    CHECK(PlugCreateFlagsOk(PLUG_F_STAGE) == PLUG_EINVAL, "STAGE without CHAIN refused");
    CHECK(PlugCreateFlagsOk(PLUG_F_STAGE | PLUG_F_START | PLUG_F_CHAIN) == PLUG_EINVAL &&
          PlugCreateFlagsOk(PLUG_F_STAGE | PLUG_F_START) == PLUG_EINVAL, "STAGE with START refused");
    CHECK(PlugCreateFlagsOk(PLUG_F_STAGE | PLUG_F_CHECK) == PLUG_EINVAL &&
          PlugCreateFlagsOk(PLUG_F_STAGE | PLUG_F_CHAIN | PLUG_F_CHECK) == PLUG_EINVAL, "STAGE with CHECK refused");
    CHECK(PlugCreateFlagsOk(0x10) == PLUG_EINVAL && PlugCreateFlagsOk(0x80000000u | PLUG_F_START | PLUG_F_CHAIN) == PLUG_EINVAL,
          "unknown bits refused");
}

static ULONG g_seq[8192], g_nseq;

/* Blocks of these lengths at row 10 word k, values 0.. (ref_chunks: block(10, k, L - 3)) */
static void Seq(const ULONG *Lengths, ULONG N)
{
    ULONG k, j;

    g_nseq = 0;
    for (k = 0; k < N; k++) {
        g_seq[g_nseq++] = (PLUG_CMD_WRITE << 16) | Lengths[k];
        g_seq[g_nseq++] = (10u << 24) | k;
        g_seq[g_nseq++] = 0;
        for (j = 0; j + 3 < Lengths[k]; j++)
            g_seq[g_nseq++] = j;
    }
}

/* The page lengths PlugChunkNext cuts from W: their count, or -1 (refused, or not the whole of W). */
static int Chunks(const ULONG *W, ULONG NW, ULONG *Out, ULONG Max)
{
    ULONG at = 0, n = 0, len;

    while ((len = PlugChunkNext(W, NW, at, PLUG_CMD_WORDS_MAX)) != 0) {
        if (len == PLUG_NONE || n >= Max)
            return -1;
        Out[n++] = len;
        at += len;
    }
    return at == NW ? (int)n : -1;
}

/* Every page starts on a block header and holds whole blocks (upstream test_long_plan_splits_...). */
static int WholeBlocks(const ULONG *W, ULONG NW)
{
    ULONG i = 0;

    while (i < NW) {
        if ((W[i] >> 16) != PLUG_CMD_WRITE || (W[i] & 0xFFFFu) < 4)
            return 0;
        i += W[i] & 0xFFFFu;
    }
    return i == NW;
}

static int SameLengths(const ULONG *A, int N, const ULONG *B, int M)
{
    int i;

    if (N != M)
        return 0;
    for (i = 0; i < N; i++)
        if (A[i] != B[i])
            return 0;
    return 1;
}

static void TestChunks(void)
{
    /* upstream's references: native_carrier.chunks(blocks(w)) on these sequences (and plugin_native._write_chunks,
       the same cut), run on this PC with the vendored open-apollo 2.1.0 code, Nov 15 2026 */
    static const ULONG refA[] = { 1000, 1000, 500 };
    static const ULONG refB[] = { 905, 1005, 988, 892, 995, 980, 565 };
    static const ULONG c1[] = { 1016 }, c2[] = { 1012, 4 }, c3[] = { 1013, 4 }, d[] = { 1017 };
    static const ULONG r1016[] = { 1016 }, r1013[] = { 1013, 4 }, r10164[] = { 1016, 4 };
    ULONG len[300], out[64], k, at;
    int n;

    for (k = 0; k < 25; k++)
        len[k] = 100;
    Seq(len, 25);
    n = Chunks(g_seq, g_nseq, out, 64);
    CHECK(SameLengths(out, n, refA, 3), "upstream test_long_plan_splits_without_cutting_a_block: 1000, 1000, 500");
    for (k = 0, at = 0; n > 0 && k < (ULONG)n; at += out[k], k++)
        CHECK(out[k] <= PLUG_CMD_WORDS_MAX && WholeBlocks(g_seq + at, out[k]), "page %lu: whole blocks, at most 1016", k);
    for (k = 0; k < 60; k++)
        len[k] = 4 + ((k * 37) % 200);
    Seq(len, 60);
    n = Chunks(g_seq, g_nseq, out, 64);
    CHECK(g_nseq == 6330 && SameLengths(out, n, refB, 7), "60 blocks of 4..203 words: upstream's 7 pages");
    Seq(c1, 1);
    n = Chunks(g_seq, g_nseq, out, 64);
    CHECK(SameLengths(out, n, r1016, 1), "one 1016-word block: one page");
    Seq(c2, 2);
    n = Chunks(g_seq, g_nseq, out, 64);
    CHECK(SameLengths(out, n, r1016, 1), "1012 + 4: one page of exactly 1016");
    Seq(c3, 2);
    n = Chunks(g_seq, g_nseq, out, 64);
    CHECK(SameLengths(out, n, r1013, 2), "1013 + 4: two pages");
    for (k = 0; k < 255; k++)
        len[k] = 4;
    Seq(len, 254);
    n = Chunks(g_seq, g_nseq, out, 64);
    CHECK(SameLengths(out, n, r1016, 1), "254 four-word blocks: one page");
    Seq(len, 255);
    n = Chunks(g_seq, g_nseq, out, 64);
    CHECK(SameLengths(out, n, r10164, 2), "255 four-word blocks: 1016 + 4");
    Seq(d, 1);
    CHECK(Chunks(g_seq, g_nseq, out, 64) == -1, "a 1017-word block refused (upstream: exceeds SERVICE ABI)");
    {
        /* upstream test_short_plan_is_one_chunk and test_malformed_long_plan_refuses */
        ULONG w[6] = { 0x00140006, 0x0A000000, 0, 1, 2, 3 };
        CHECK(Chunks(w, 6, out, 64) == 1 && out[0] == 6, "a short plan is one page");
        for (k = 0; k < 300; k++) {
            g_seq[4 * k] = 0x00150004;
            g_seq[4 * k + 1] = g_seq[4 * k + 2] = g_seq[4 * k + 3] = 0;
        }
        CHECK(Chunks(g_seq, 1200, out, 64) == -1, "a malformed long plan refused");
        w[0] = 0x00140009;
        CHECK(PlugChunkNext(w, 6, 0, PLUG_CMD_WORDS_MAX) == PLUG_NONE, "a block past the end refused");
        CHECK(PlugChunkNext(w, 6, 6, PLUG_CMD_WORDS_MAX) == 0, "nothing after the end");
    }
}

/*
 * A synthetic chain whose startup needs pages: the carrier's head, Mic m0 and record-delay blocks
 * (as BuildRecipe), then an effect of a 3000-word root and a 2048-word row written whole in
 * 33-word runs, the root's linked word 20 skipped by the mask (compact()'s shape). No UA data.
 */
static const PLUG_ROW kStageFx[2] = { { 1, 3000, 8, 0 }, { 3, 2048, 0x0A000014, 2 } };
static ULONG g_stage[8192], g_nstage, g_relocAll[8192], g_relocPaged[8192];

static ULONG BuildStaged(PLUG_ROW *Rows)
{
    static const ULONG hp[7] = { 0, 1, 2, 4, 5, 7, 9 };
    static const ULONG m0v[15] = { 0x40000000, 1, 1, 0x800, 0x3a888889, 0xc0000000, 0xc0000000, 0x10, 0x4b,
                                   0x41000000, 0x3ada740e, 0xFFFFFFFF, 1, 0xFFFFFFFF, 1 };
    ULONG hv[7] = { 0x40000000, 1, 1, 0x22, 2, 0x846, RENDER }, pos[33], val[33], i, k, n, first, p16 = 16, v = 200;
    ULONG h4[4] = { 0, 1, 2, 4 }, hv4[4] = { 0x40000000, 1, 1, 4 };

    for (i = 0; i < 10; i++)
        Rows[i] = kChainRows[i];
    Rows[10] = kStageFx[0];
    Rows[11] = kStageFx[1];
    g_nstage = 0;
    WriteTo(g_stage, &g_nstage, 0, hp, hv, 7);
    g_stage[g_nstage++] = 0x00140000u | (15 + 3);
    g_stage[g_nstage++] = 0x01000000u;
    g_stage[g_nstage++] = 0x0f1fffec;
    for (k = 0; k < 15; k++)
        g_stage[g_nstage++] = m0v[k];
    WriteTo(g_stage, &g_nstage, 1, &p16, &v, 1);
    WriteTo(g_stage, &g_nstage, 10, h4, hv4, 4);
    for (first = 10; first < 3000; first += 33) {
        for (n = 0, k = first; k < first + 33 && k < 3000; k++)
            if (k != 20) {
                pos[n] = k;
                val[n++] = 0x3F000000u | k;
            }
        WriteTo(g_stage, &g_nstage, 10, pos, val, n);
    }
    for (first = 0; first < 2048; first += 33) {
        for (n = 0, k = first; k < first + 33 && k < 2048; k++) {
            pos[n] = k;
            val[n++] = 0x40000000u | (k * 7);
        }
        WriteTo(g_stage, &g_nstage, 11, pos, val, n);
    }
    return 12;
}

static void TestStaged(void)
{
    static PLUG_XL x;
    static PLUG_LEDGER L;
    static ULONG reloc[PLUG_CMD_WORDS_MAX];
    PLUG_ROW rows[12];
    PLUG_LEASE strips[4], lease[12];
    PLUG_REQ rq[12];
    PLUG_PAY p;
    ULONG nr, pages[64], s2[256], s1[128], addr[12], ncmds = 0, ids[5], nids = 0, carrier = 0, i, k, at, ok, start6[6][4];
    ULONG saved = g_fnv, hdr[2] = { 0, 0 };
    int n;

    g_fnv = 2166136261u;
    nr = BuildStaged(rows);
    CHECK(PlugCheckRows(rows, nr, s2, s1) == PLUG_OK, "staged chain: rows are one descriptor");
    CHECK(PlugCheckCmds(rows, nr, g_stage, g_nstage, &ncmds) == PLUG_OK && g_nstage > PLUG_CMD_WORDS_MAX,
          "staged startup: %lu words in %lu blocks, over one page", g_nstage, ncmds);
    n = Chunks(g_stage, g_nstage, pages, 64);
    CHECK(n >= 2, "cut into %d pages", n);
    for (k = 0, at = 0, ok = 1; n > 0 && k < (ULONG)n; at += pages[k], k++) {
        ULONG c = 0, j;
        ok &= pages[k] <= PLUG_CMD_WORDS_MAX && PlugCheckCmds(rows, nr, g_stage + at, pages[k], &c) == PLUG_OK;
        if (k == 0) {
            CHECK(PlugChainIds(rows, nr, g_stage, pages[0], 1, RENDER, ids, &nids, &carrier) == PLUG_OK && nids == 1 &&
                  ids[0] == RENDER && carrier == PLUG_MIC_CARRIER, "page 0 holds the chain directory (the CREATE's licence check)");
            continue;
        }
        ok &= !PlugWritesDirectory(g_stage + at, pages[k]);            /* SERVICE refuses a directory write */
        for (j = 0; j < pages[k]; j += g_stage[at + j] & 0xFFFF)
            ok &= (g_stage[at + j + 1] >> 24) >= 10;                     /* the carrier's rows 0-9 all in page 0 */
    }
    CHECK(ok, "every page: whole blocks inside their rows; pages 1.. clear of the directory and the carrier rows");

    /* placed on the x8p's DSP 1 beside the strips */
    (void)PlugLedgerInit(&L, kBase, kSize, kScr);
    (void)PlugLedgerBookStrips(&L, STRIP_CODE_WORDS, kPer, 8, strips);
    CHECK(PlugLedgerSetScratch(&L, 1, 0x800) == PLUG_OK, "chain scratch raised");
    for (i = 0; i < nr; i++) {
        rq[i].Kind = rows[i].Pool;
        rq[i].Words = rows[i].Dwords;
        rq[i].Identity = 0;
        rq[i].Shared = 0;
    }
    CHECK(PlugLedgerReserveBatch(&L, rq, nr, lease) == PLUG_OK, "12 rows placed");
    for (i = 0; i < nr; i++)
        addr[i] = lease[i].Base;
    CHECK(PlugRelocate(g_relocAll, g_stage, g_nstage, addr, nr) == PLUG_OK, "the whole startup relocates");

    /* CREATE F_STAGE: page 0 as one transaction of WRITEs: nothing stopped, SYNCH'd or routed */
    PayInit(&p, 1);
    PlugXlInit(&x, PLUG_PAGE);
    CHECK(PlugBuildStartEx(&x, &p, g_stage, pages[0], addr, nr, 0x00200000, reloc, 0) == PLUG_OK && x.N == 1 &&
          x.NRx == 0 && x.Ent[0][0] == (PLUG_RING_DMA_REF | pages[0]) && RefsOk(&x),
          "stage: one DMA command of %lu words, no SYNCH, no span or route", pages[0]);
    for (i = 0; i < pages[0]; i++)
        g_relocPaged[i] = PayWord(&p, x.Ent[0][2] + 4 * i);
    FnvXl(&x, &p);
    /* the other pages: one SERVICE write each (one DMA command, one page) */
    for (k = 1, at = pages[0], ok = 1; n > 0 && k < (ULONG)n; at += pages[k], k++) {
        PayInit(&p, 1);
        PlugXlInit(&x, PLUG_PAGE);
        ok &= PlugRelocate(reloc, g_stage + at, pages[k], addr, nr) == PLUG_OK;
        ok &= PlugXlCmdWords(&x, &p, reloc, pages[k]) == PLUG_OK && x.N == 1 && x.NRx == 0 && RefsOk(&x) &&
              x.Ent[0][0] == (PLUG_RING_DMA_REF | pages[k]);
        for (i = 0; i < pages[k]; i++)
            g_relocPaged[at + i] = PayWord(&p, x.Ent[0][2] + 4 * i);
        FnvXl(&x, &p);
    }
    CHECK(ok, "SERVICE pages: one DMA command each, inside one 4 KiB page");
    CHECK(memcmp(g_relocPaged, g_relocAll, g_nstage * 4) == 0,
          "the pages put exactly the words of the whole startup on the DSP, in order");

    /* op START: P0's start without the WRITEs (stop strip, SYNCH head, span, span, route) */
    PayInit(&p, 1);
    PlugXlInit(&x, PLUG_PAGE);
    CHECK(PlugBuildStart(&x, &p, g_stage, pages[0], addr, nr, 0x00200000, reloc) == PLUG_OK && x.N == 6, "P0 start: 6 entries");
    memcpy(start6, x.Ent, sizeof(start6));
    PlugXlInit(&x, 0);
    CHECK(PlugBuildStartEx(&x, &p, NULL, 0, addr, nr, 0x00200000, reloc, 1) == PLUG_OK && x.N == 5 && x.Len == 0,
          "START: 5 inline entries, no payload");
    CHECK(memcmp(x.Ent[0], start6[0], 16) == 0 && memcmp(x.Ent[1], start6[2], 4 * 16) == 0,
          "START = P0's start less its WRITE command");
    CHECK(x.Ent[0][0] == PLUG_CMD_SYNCH && x.Ent[0][1] == 0x00200000 && x.Ent[0][2] == 0 &&
          x.Ent[1][0] == PLUG_CMD_SYNCH && x.Ent[1][2] == addr[0] && x.Ent[4][2] == PLUG_INPUT_ROUTE,
          "stop input 1's strip, SYNCH the staged head, route");
    FnvXl(&x, NULL);
    PlugXlInit(&x, 0);
    CHECK(PlugBuildStartEx(&x, &p, NULL, 0, addr, nr, 0x00200000, reloc, 0) == PLUG_EINVAL, "a stage with nothing to write refused");
    /* a staged chain that goes before START: nothing to stop or restore, releases only */
    PlugXlInit(&x, 0);
    CHECK(PlugBuildStop(&x, 0x00200000, 0, 0, hdr) == PLUG_OK && x.N == 0, "never routed: its teardown sends no SYNCH");
    for (i = 0; i < nr; i++)
        Fnv(addr[i]);
    g_fnvStage = g_fnv;
    g_fnv = saved;
}

/* ------------------------------------------------------------------ */
/* Stereo on a linked pair (FEAT_STEREO): the Synch rule, the overlap   */
/* rule, and the words of start / stage / START / teardown on a pair.   */
/* Not in either digest: P0's and the staged goldens stay as recorded.  */
/* ------------------------------------------------------------------ */

#define SY(in1, in2)    ((ULONG)(in1) | ((ULONG)(in2) << 16))

/* The rule, stated independently of PlugSynchCheck: the x8p's four pairs listed, not computed. */
static ULONG SynchExpect(ULONG In1, ULONG In2, ULONG NumInputs)
{
    static const ULONG pairs[4][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 } };
    ULONG k;

    if (In1 >= 0x20 || In2 > 0x20 || In1 == In2)
        return PLUG_EINVAL;
    if (In2 == 0x20)
        return PLUG_OK;
    for (k = 0; k < 4; k++)
        if (pairs[k][0] == In1 && pairs[k][1] == In2 && pairs[k][1] < NumInputs)
            return PLUG_OK;
    return PLUG_ENOTSUP;
}

static void TestStereo(void)
{
    static PLUG_XL x;
    static ULONG reloc[2048];
    PLUG_PAY p;
    ULONG addr[13], hdr[2], sa[STRIP_PLAN_ROWS], mono6[6][4], i, k, ok, nok, n;
    static const ULONG nin[4] = { 8, 6, 2, 0 };

    /* the Synch rule: mono as in P0, the x8p's four linked pairs, everything else refused */
    for (i = 0; i < 8; i++)
        CHECK(PlugSynchCheck(SY(i, 0x20), 8) == PLUG_OK, "mono input %lu accepted", i + 1);
    CHECK(PlugSynchCheck(SY(8, 0x20), 8) == PLUG_OK && PlugSynchCheck(SY(0x1F, 0x20), 8) == PLUG_OK &&
          PlugSynchCheck(SY(0, 0x20), 0) == PLUG_OK,
          "mono below 0x20 accepted as in P0 (the strip target answers whether the input has a strip)");
    CHECK(PlugSynchCheck(SY(0, 1), 8) == PLUG_OK && PlugSynchCheck(SY(2, 3), 8) == PLUG_OK &&
          PlugSynchCheck(SY(4, 5), 8) == PLUG_OK && PlugSynchCheck(SY(6, 7), 8) == PLUG_OK,
          "pairs 1/2, 3/4, 5/6, 7/8 accepted");
    CHECK(PlugSynchCheck(SY(1, 2), 8) == PLUG_ENOTSUP && PlugSynchCheck(SY(3, 4), 8) == PLUG_ENOTSUP &&
          PlugSynchCheck(SY(5, 6), 8) == PLUG_ENOTSUP && PlugSynchCheck(SY(7, 8), 8) == PLUG_ENOTSUP,
          "odd first input (2/3, 4/5, 6/7, 8/9): ENOTSUP");
    CHECK(PlugSynchCheck(SY(0, 2), 8) == PLUG_ENOTSUP && PlugSynchCheck(SY(0, 3), 8) == PLUG_ENOTSUP &&
          PlugSynchCheck(SY(2, 1), 8) == PLUG_ENOTSUP && PlugSynchCheck(SY(1, 0), 8) == PLUG_ENOTSUP &&
          PlugSynchCheck(SY(4, 7), 8) == PLUG_ENOTSUP && PlugSynchCheck(SY(0, 0x1F), 8) == PLUG_ENOTSUP,
          "in2 != in1 + 1 (also reversed): ENOTSUP");
    CHECK(PlugSynchCheck(SY(8, 9), 8) == PLUG_ENOTSUP && PlugSynchCheck(SY(0x1E, 0x1F), 8) == PLUG_ENOTSUP &&
          PlugSynchCheck(SY(6, 7), 7) == PLUG_ENOTSUP, "in2 >= the unit's inputs: ENOTSUP");
    CHECK(PlugSynchCheck(SY(0, 1), 2) == PLUG_OK && PlugSynchCheck(SY(2, 3), 2) == PLUG_ENOTSUP &&
          PlugSynchCheck(SY(0, 1), 0) == PLUG_ENOTSUP, "a 2-input unit: 1/2 only; no inputs known: no pairs");
    CHECK(PlugSynchCheck(SY(0, 0), 8) == PLUG_EINVAL && PlugSynchCheck(SY(3, 3), 8) == PLUG_EINVAL &&
          PlugSynchCheck(0, 8) == PLUG_EINVAL, "in1 == in2 (Synch 0 too): EINVAL");
    CHECK(PlugSynchCheck(SY(0x20, 0x20), 8) == PLUG_EINVAL && PlugSynchCheck(SY(0x20, 1), 8) == PLUG_EINVAL &&
          PlugSynchCheck(SY(0x20, 0), 8) == PLUG_EINVAL && PlugSynchCheck(SY(0x21, 0x20), 8) == PLUG_EINVAL &&
          PlugSynchCheck(0x0000FFFFu, 8) == PLUG_EINVAL, "0x20 or above first: EINVAL");
    CHECK(PlugSynchCheck(SY(0, 0x21), 8) == PLUG_EINVAL && PlugSynchCheck(SY(1, 0x21), 8) == PLUG_EINVAL &&
          PlugSynchCheck(0xFFFF0000u, 8) == PLUG_EINVAL && PlugSynchCheck(0xFFFFFFFFu, 8) == PLUG_EINVAL &&
          PlugSynchCheck(0x00210000u | 0x10000000u, 8) == PLUG_EINVAL, "in2 above 0x20 (high bits): EINVAL");
    for (k = 0, ok = 1, nok = 0; k < 4; k++) {
        for (i = 0; i < 0x23 * 0x23; i++) {
            ULONG in1 = i % 0x23, in2 = i / 0x23, got = PlugSynchCheck(SY(in1, in2), nin[k]);
            ok &= got == SynchExpect(in1, in2, nin[k]);
            nok += (got == PLUG_OK && in2 != 0x20);
        }
    }
    CHECK(ok && nok == 4 + 3 + 1 + 0, "sweep in1, in2 0..0x22 at 8/6/2/0 inputs: as listed (%lu pairs accepted)", nok);

    /* one routed owner per input: a pair overlaps a mono chain on either input, and a pair sharing one */
    CHECK(PlugInputsOverlap(SY(0, 1), SY(0, 0x20)) && PlugInputsOverlap(SY(0, 0x20), SY(0, 1)) &&
          PlugInputsOverlap(SY(0, 1), SY(1, 0x20)) && PlugInputsOverlap(SY(1, 0x20), SY(0, 1)),
          "pair 1/2 against mono 1 and mono 2: overlap (both ways)");
    CHECK(!PlugInputsOverlap(SY(0, 1), SY(2, 0x20)) && !PlugInputsOverlap(SY(2, 0x20), SY(0, 1)) &&
          !PlugInputsOverlap(SY(0, 1), SY(2, 3)) && !PlugInputsOverlap(SY(6, 7), SY(4, 5)) &&
          !PlugInputsOverlap(SY(6, 7), SY(5, 0x20)), "pair 1/2 against mono 3, pair 3/4; 7/8 against 5/6, mono 6: none");
    CHECK(PlugInputsOverlap(SY(2, 3), SY(2, 3)) && PlugInputsOverlap(SY(6, 7), SY(7, 0x20)) &&
          PlugInputsOverlap(SY(7, 0x20), SY(6, 7)), "the same pair; pair 7/8 against mono 8");
    for (i = 0, ok = 1; i < 8; i++)
        for (k = 0; k < 8; k++)
            ok &= PlugInputsOverlap(SY(i & ~1u, (i & ~1u) + 1), SY(k, 0x20)) == (ULONG)((k >> 1) == (i >> 1)) &&
                  PlugInputsOverlap(SY(i & ~1u, (i & ~1u) + 1), SY(k & ~1u, (k & ~1u) + 1)) == (ULONG)((k >> 1) == (i >> 1));
    CHECK(ok, "every pair against every mono input and every pair: overlap exactly on a shared input");

    /* F_START on pair 3/4 (in1 2, in2 3): stop both plain strips, the WRITEs, SYNCH the head on the
       pair, span none twice and route per input (ua_inst_start, ua_plugin_instance.c:689-739) */
    BuildRecipe(RENDER, 2);
    for (i = 0; i < 13; i++)
        addr[i] = 0x90000u + 0x200u * i;
    PayInit(&p, 1);
    PlugXlInit(&x, PLUG_PAGE);
    CHECK(PlugBuildStart(&x, &p, g_cmd, g_nw, addr, 13, 0x00200002, reloc) == PLUG_OK && x.N == 6, "mono input 3: 6 entries");
    memcpy(mono6, x.Ent, sizeof(mono6));
    PayInit(&p, 1);
    PlugXlInit(&x, PLUG_PAGE);
    CHECK(PlugBuildStart(&x, &p, g_cmd, g_nw, addr, 13, SY(2, 3), reloc) == PLUG_OK && x.N == 10 && x.NRx == 0 && RefsOk(&x),
          "pair 3/4: 10 entries");
    CHECK(x.Ent[0][0] == PLUG_CMD_SYNCH && x.Ent[0][1] == 0x00200002 && x.Ent[0][2] == 0 &&
          x.Ent[1][0] == PLUG_CMD_SYNCH && x.Ent[1][1] == 0x00200003 && x.Ent[1][2] == 0, "both plain strips stopped (head 0)");
    CHECK(x.Ent[2][0] == (PLUG_RING_DMA_REF | g_nw) && memcmp(x.Ent[2], mono6[1], 16) == 0, "the WRITEs: as mono");
    CHECK(x.Ent[3][0] == PLUG_CMD_SYNCH && x.Ent[3][1] == SY(2, 3) && x.Ent[3][2] == addr[0] && x.Ent[3][3] == 0,
          "the head SYNCH'd on the pair (in2 << 16 | in1)");
    CHECK(memcmp(x.Ent[4], mono6[3], 3 * 16) == 0, "input 3: span none twice, route 0x1FFFF (as mono)");
    CHECK(x.Ent[7][0] == PLUG_CMD_INPUT && x.Ent[7][1] == 3 && x.Ent[7][2] == PLUG_INPUT_SPAN && x.Ent[7][3] == PLUG_SPAN_NONE &&
          x.Ent[8][1] == 3 && x.Ent[8][2] == PLUG_INPUT_SPAN && x.Ent[9][0] == PLUG_CMD_INPUT && x.Ent[9][1] == 3 &&
          x.Ent[9][2] == PLUG_INPUT_ROUTE && x.Ent[9][3] == PLUG_ROUTE_NONE, "input 4: span none twice, route 0x1FFFF");
    for (i = 0, ok = 1; i < g_nw; i += g_cmd[i] & 0xFFFF)
        ok &= PayWord(&p, x.Ent[2][2] + (i + 1) * 4) == addr[g_cmd[i + 1] >> 24] + (g_cmd[i + 1] & 0xFFFFFF);
    CHECK(ok, "the pair's WRITEs relocated as mono's");

    /* F_STAGE on a pair: the WRITEs only (nothing stopped or SYNCH'd), the same as on one input */
    PayInit(&p, 1);
    PlugXlInit(&x, PLUG_PAGE);
    CHECK(PlugBuildStartEx(&x, &p, g_cmd, g_nw, addr, 13, SY(6, 7), reloc, 0) == PLUG_OK && x.N == 1 &&
          x.Ent[0][0] == (PLUG_RING_DMA_REF | g_nw), "stage on pair 7/8: one DMA command, no SYNCH, no route");

    /* op START on a staged pair: the F_START words less the WRITE command */
    PlugXlInit(&x, 0);
    CHECK(PlugBuildStartEx(&x, &p, NULL, 0, addr, 13, SY(2, 3), reloc, 1) == PLUG_OK && x.N == 9 && x.Len == 0,
          "START on pair 3/4: 9 inline entries");
    CHECK(x.Ent[0][1] == 0x00200002 && x.Ent[1][1] == 0x00200003 && x.Ent[2][0] == PLUG_CMD_SYNCH &&
          x.Ent[2][1] == SY(2, 3) && x.Ent[2][2] == addr[0] && x.Ent[5][2] == PLUG_INPUT_ROUTE && x.Ent[5][1] == 2 &&
          x.Ent[8][2] == PLUG_INPUT_ROUTE && x.Ent[8][1] == 3, "stop both, SYNCH the staged head on the pair, route both");

    /* teardown: the chain stopped once on the pair, then both retained plain strips at their headers */
    StripPlanAddrs(2, sa);
    hdr[0] = sa[0];
    StripPlanAddrs(3, sa);
    hdr[1] = sa[0];
    PlugXlInit(&x, 0);
    CHECK(hdr[0] != 0 && hdr[1] != 0 && hdr[0] != hdr[1] && PlugBuildStop(&x, SY(2, 3), 1, 1, hdr) == PLUG_OK && x.N == 9,
          "stop + restore of pair 3/4: 9 entries");
    CHECK(x.Ent[0][0] == PLUG_CMD_SYNCH && x.Ent[0][1] == SY(2, 3) && x.Ent[0][2] == 0, "the chain stopped on the pair (head 0)");
    CHECK(x.Ent[1][0] == PLUG_CMD_SYNCH && x.Ent[1][1] == 0x00200002 && x.Ent[1][2] == hdr[0] &&
          x.Ent[2][1] == 2 && x.Ent[3][1] == 2 && x.Ent[4][1] == 2 && x.Ent[4][2] == PLUG_INPUT_ROUTE,
          "input 3's plain strip back at its header, span, route");
    CHECK(x.Ent[5][0] == PLUG_CMD_SYNCH && x.Ent[5][1] == 0x00200003 && x.Ent[5][2] == hdr[1] &&
          x.Ent[6][1] == 3 && x.Ent[7][1] == 3 && x.Ent[8][1] == 3 && x.Ent[8][2] == PLUG_INPUT_ROUTE &&
          x.Ent[8][3] == PLUG_ROUTE_NONE, "input 4's plain strip back at its header, span, route");
    PlugXlInit(&x, 0);
    CHECK(PlugBuildStop(&x, SY(2, 3), 1, 0, hdr) == PLUG_OK && x.N == 1 && x.Ent[0][1] == SY(2, 3) && x.Ent[0][2] == 0,
          "D0 exit on a pair: one stop on the pair, no restore");
    PlugXlInit(&x, 0);
    CHECK(PlugBuildStop(&x, SY(2, 3), 0, 0, hdr) == PLUG_OK && x.N == 0, "a staged pair never routed: no SYNCH at all");
    for (i = 0, n = 0; i < 8; i += 2) {
        PlugXlInit(&x, 0);
        hdr[0] = 0x1000u + i;
        hdr[1] = 0x2000u + i;
        n += PlugBuildStop(&x, SY(i, i + 1), 1, 1, hdr) == PLUG_OK && x.N == 9 && x.Ent[1][1] == ((0x20u << 16) | i) &&
             x.Ent[1][2] == hdr[0] && x.Ent[5][1] == ((0x20u << 16) | (i + 1)) && x.Ent[5][2] == hdr[1];
    }
    CHECK(n == 4, "every x8p pair restores both of its strips, each at its own header");
}

/* ---- the caller rule's pure parts (review findings 1 and 2, Nov 15): the stored DOS path, its match ---- */
/* ---- against SeLocateProcessImageName's NT path once \GLOBAL??\X: is resolved, the SID strings      ---- */

#define WL(s) (s), (ULONG)(sizeof(s) / sizeof(WCHAR) - 1)

static void TestCaller(void)
{
    static const WCHAR img[] = L"C:\\Program Files\\Avium\\Apollo Control\\apollo-control.exe";
    static const WCHAR vol3[] = L"\\Device\\HarddiskVolume3";
    static const WCHAR loc3[] = L"\\Device\\HarddiskVolume3\\Program Files\\Avium\\Apollo Control\\apollo-control.exe";
    static const WCHAR locCase[] = L"\\DEVICE\\harddiskvolume3\\PROGRAM FILES\\avium\\apollo control\\APOLLO-CONTROL.EXE";
    static const WCHAR loc4[] = L"\\Device\\HarddiskVolume4\\Program Files\\Avium\\Apollo Control\\apollo-control.exe";
    static const WCHAR locOther[] = L"\\Device\\HarddiskVolume3\\Program Files\\Avium\\Apollo Control\\apollo-control.ex_";
    static const WCHAR locLonger[] = L"\\Device\\HarddiskVolume3\\Program Files\\Avium\\Apollo Control\\apollo-control.exe.exe";
    static const WCHAR locShort[] = L"\\Device\\HarddiskVolume3\\Program Files\\Avium\\Apollo Control\\apollo-control.ex";
    static const WCHAR locNonAscii[] = L"\\Device\\HarddiskVolume3\\Program Files\\Avium\\Apollo Control\\apollo-contr\x00D6l.exe";
    static const WCHAR imgNonAscii[] = L"C:\\Program Files\\Avium\\Apollo Control\\apollo-contr\x00F6l.exe";
    static const WCHAR vol3Slash[] = L"\\Device\\HarddiskVolume3\\";
    static const WCHAR sid[] = L"S-1-5-21-1004336348-1177238915-682003330-1001";
    static const WCHAR sidLower[] = L"s-1-5-21-1004336348-1177238915-682003330-1001";
    static const WCHAR sidOther[] = L"S-1-5-21-1004336348-1177238915-682003330-1002";
    static const WCHAR sidPrefix[] = L"S-1-5-21-1004336348-1177238915-682003330-100";
    static const WCHAR sidHex[] = L"S-1-0x000000000100-5";
    static const WCHAR sidAad[] = L"S-1-12-1-1234567890-1234567890-1234567890-1234567890";
    static const WCHAR *badSid[] = { L"", L"S-1-5", L"S-1-5-", L"S-1--5-18", L"S-2-5-18", L"X-1-5-18", L"S-1-5-18-", L"-S-1-5-18",
                                     L"S-1-5-18 ", L"S-1-5-12345678901", L"S-1-0x12-5", L"S-1-5-1-2-3-4-5-6-7-8-9-10-11-12-13-14-15-16",
                                     L"S-1-5-1a" };
    static const WCHAR *badImg[] = { L"", L"C:", L"C:\\", L"\\Device\\HarddiskVolume3\\x.exe", L"\\\\?\\C:\\x.exe", L"\\\\server\\share\\x.exe",
                                     L"1:\\x.exe", L"C:x.exe", L"C:\\x.exe:stream", L"C:\\dir/x.exe", L"C/\\x.exe" };
    WCHAR emb[] = L"C:\\x.exe";
    ULONG i;

    CHECK(PlugImageDrive(WL(img)) == L'C' && PlugImageDrive(WL(L"d:\\Apps\\x.exe")) == L'D', "image: C:\\... and d:\\... name drives C and D");
    for (i = 0; i < sizeof(badImg) / sizeof(badImg[0]); i++) {
        CHECK(PlugImageDrive(badImg[i], (ULONG)wcslen(badImg[i])) == 0, "image: \"%ls\" names no program", badImg[i]);
    }
    emb[3] = 0;
    CHECK(PlugImageDrive(emb, 8) == 0, "image: a NUL inside is refused");
    CHECK(PlugImageMatch(WL(loc3), WL(vol3), WL(img)), "match: C: -> HarddiskVolume3, the same path");
    CHECK(PlugImageMatch(WL(locCase), WL(vol3), WL(img)), "match: ASCII letters in either case");
    CHECK(PlugImageMatch(WL(loc3), WL(vol3Slash), WL(img)), "match: a target with a trailing backslash");
    CHECK(!PlugImageMatch(WL(loc4), WL(vol3), WL(img)), "match: C: now on another volume (a USB stick enumerated first) is not it");
    CHECK(!PlugImageMatch(WL(loc3), WL(L"\\Device\\HarddiskVolume4"), WL(img)), "match: the program on the volume C: no longer names");
    CHECK(!PlugImageMatch(WL(locOther), WL(vol3), WL(img)) && !PlugImageMatch(WL(locLonger), WL(vol3), WL(img)) &&
          !PlugImageMatch(WL(locShort), WL(vol3), WL(img)), "match: another file, a longer or a shorter name: no");
    CHECK(!PlugImageMatch(WL(locNonAscii), WL(vol3), WL(imgNonAscii)), "match: non-ASCII letters differing in case: no (fail closed)");
    CHECK(PlugImageMatch(WL(L"\\Device\\HarddiskVolume3\\x\x00F6.exe"), WL(vol3), WL(L"C:\\x\x00F6.exe")), "match: non-ASCII letters, the same: yes");
    CHECK(!PlugImageMatch(WL(loc3), WL(L"Device\\HarddiskVolume3"), WL(img)) && !PlugImageMatch(WL(loc3), L"", 0, WL(img)) &&
          !PlugImageMatch(WL(loc3), WL(vol3), WL(L"\\Device\\HarddiskVolume3\\Program Files\\Avium\\Apollo Control\\apollo-control.exe")),
          "match: a relative or empty target, or a stored device path: no");
    CHECK(!PlugImageMatch(NULL, 0, WL(vol3), WL(img)) && !PlugImageMatch(WL(loc3), NULL, 0, WL(img)) && !PlugImageMatch(WL(loc3), WL(vol3), NULL, 0),
          "match: NULLs: no");
    CHECK(PlugSidTextOk(WL(sid)) && PlugSidTextOk(WL(L"S-1-5-18")) && PlugSidTextOk(WL(L"S-1-1-0")) && PlugSidTextOk(WL(sidHex)) &&
          PlugSidTextOk(WL(sidAad)) && PlugSidTextOk(WL(L"S-1-5-1-2-3-4-5-6-7-8-9-10-11-12-13-14-15")),
          "SID text: a local account, SYSTEM, Everyone, a hex authority, an Entra ID account, 15 sub-authorities");
    for (i = 0; i < sizeof(badSid) / sizeof(badSid[0]); i++) {
        CHECK(!PlugSidTextOk(badSid[i], (ULONG)wcslen(badSid[i])), "SID text: \"%ls\" refused", badSid[i]);
    }
    CHECK(PlugSidTextEqual(WL(sid), WL(sid)) && PlugSidTextEqual(WL(sid), WL(sidLower)), "SID: the same user, S in either case");
    CHECK(!PlugSidTextEqual(WL(sid), WL(sidOther)) && !PlugSidTextEqual(WL(sid), WL(sidPrefix)) && !PlugSidTextEqual(WL(sidPrefix), WL(sid)),
          "SID: another account, or a prefix of it: no");
    CHECK(!PlugSidTextEqual(WL(L"S-1-5-18x"), WL(L"S-1-5-18x")) && !PlugSidTextEqual(NULL, 0, NULL, 0), "SID: malformed or NULL never equal");
}

/* ---- [P7] hosting per model: the x8p's row is what the driver used before (8 inputs, 0xFF, 8 strips, DSP 1, ---- */
/* ---- its measured pools), both rows agree with strip_plan.h, the Twin X's ledger and inputs              ---- */

static void TestModels(void)
{
    static PLUG_LEDGER L;
    const PLUG_MODEL *x = PlugModelFor(PLUG_MODEL_X8P), *t = PlugModelFor(PLUG_MODEL_TWINX), *m;
    PLUG_LEASE s[4], lease[3 + NCHAIN];
    PLUG_REQ rq[3 + NCHAIN];
    PLUG_USAGE u0[4], u1[4];
    ULONG k, j, in, last, ok, size[4], base[4], scr[4];

    CHECK(x != NULL && t != NULL && PlugModelFor(0) == NULL && PlugModelFor(3) == NULL, "models: x8p and Twin X host, nothing else");
    if (x == NULL || t == NULL)
        return;
    CHECK(x->Inputs == 8 && x->InputMask == 0xFF && x->Strips == 8 && x->StripDsp == 1 && x->BoardDsp == 0 && x->FirstSynch == 0 &&
          memcmp(x->StripDspRegs, kSize, sizeof(kSize)) == 0 && memcmp(x->StripDspRegs + 4, kBase, sizeof(kBase)) == 0 &&
          x->StripDspRegs[8] == kScr[1] && x->StripDspRegs[9] == kScr[2] && x->StripDspRegs[10] == kScr[3],
          "x8p row = the pre-P7 constants (8 inputs, mask 0xFF, 8 strips' room, DSP 1, board query DSP 0, measured pools)");
    for (k = 0; k < 2; k++) {
        const STRIP_MODEL *sm;
        m = k ? t : x;
        sm = StripModelFor(m->Model);
        CHECK(sm != NULL && sm->Inputs == m->Inputs && sm->MaskAllowed == m->InputMask && sm->LayoutStrips == m->Strips &&
              sm->DspDefault == m->StripDsp && sm->FirstSynch == m->FirstSynch && m->InputMask == (1u << m->Inputs) - 1 &&
              StripRouteDsps(m->Dsps, m->StripDsp) == m->RouteDsps && m->BoardDsp == 0 && m->StripDsp != m->BoardDsp,
              "%s: the strip plan's inputs, mask, strips and DSP; routes to DSP mask 0x%02lx; licence on DSP 0", m->Name, m->RouteDsps);
    }
    CHECK(t->RouteDsps == 0x01 && t->Dsps == 2 && t->HizMask == 0x01 && t->Talkback == 10, "Twin X: routes to DSP 0 only, Hi-Z = input 1, talkback 10");

    /* inputs: the x8p's rule is the driver's PlugSynchCheck over its 8 inputs (unchanged); the Twin X takes 1-2 and the pair */
    ok = 1;
    for (in = 0; in < 0x21; in++) {
        ULONG mono = in | (PLUG_SYNCH_MONO << 16), pair = in | ((in + 1) << 16);
        ULONG oldMono = (in < 8) ? PLUG_OK : PLUG_ENOTSUP;
        if (in >= PLUG_SYNCH_MONO) oldMono = PLUG_EINVAL;
        if (PlugModelSynch(x, mono) != oldMono) ok = 0;
        if ((PlugModelSynch(x, pair) == PLUG_OK) != (in < 8 && (in & 1) == 0 && PlugSynchCheck(pair, 8) == PLUG_OK)) ok = 0;
    }
    CHECK(ok, "x8p: mono inputs 1-8 and the pairs 1/2 .. 7/8, as before");
    CHECK(PlugModelSynch(t, 0 | (PLUG_SYNCH_MONO << 16)) == PLUG_OK && PlugModelSynch(t, 1 | (PLUG_SYNCH_MONO << 16)) == PLUG_OK &&
          PlugModelSynch(t, 0x00010000u) == PLUG_OK, "Twin X: input 1 (and its Hi-Z jack), input 2, the pair 1/2");
    CHECK(PlugModelSynch(t, 2 | (PLUG_SYNCH_MONO << 16)) == PLUG_ENOTSUP && PlugModelSynch(t, 10 | (PLUG_SYNCH_MONO << 16)) == PLUG_ENOTSUP &&
          PlugModelSynch(t, 0x00030002u) == PLUG_ENOTSUP && PlugModelSynch(t, 0x00020001u) == PLUG_ENOTSUP &&
          PlugModelSynch(t, 24 | (PLUG_SYNCH_MONO << 16)) == PLUG_ENOTSUP && PlugModelSynch(NULL, 0x00200000u) == PLUG_ENOTSUP,
          "Twin X refuses input 3, the talkback (10), the pairs 3/4 and 2/3, VIRTUAL 1 (24)");

    /* the Twin X's DSP 1 ledger as plugin.c builds it: its 2 strips' room only */
    for (k = 0; k < 4; k++) {
        size[k] = t->StripDspRegs[k];
        base[k] = t->StripDspRegs[4 + k];
        scr[k] = k ? t->StripDspRegs[7 + k] : 0;
    }
    CHECK(StripPlanLayoutCheck(size, base, scr, t->Strips, STRIP_CODE_WORDS) == 0, "Twin X DSP 1 pools pass the strips' layout check for 2");
    CHECK(PlugLedgerInit(&L, base, size, scr) == PLUG_OK && PlugLedgerBookStrips(&L, STRIP_CODE_WORDS, kPer, t->Strips, s) == PLUG_OK &&
          LedgerInvariant(&L), "Twin X DSP 1 ledger built, its 2 strips booked");
    CHECK(s[0].Base == 0xE0000 && s[0].Words == STRIP_CODE_WORDS && s[1].Base + s[1].Words == STRIP_TOP1 && s[1].Words == 2 * kPer[1] &&
          s[2].Base + s[2].Words == STRIP_TOP2 && s[2].Words == 2 * kPer[2] && s[3].Base + s[3].Words == STRIP_TOP3 &&
          s[3].Words == 2 * kPer[3], "Twin X: code 0xE0000+994, 2 strips' room at the pool tops");
    ok = 1;
    for (in = 0; in < t->Inputs; in++) {
        ULONG a[STRIP_PLAN_ROWS];
        StripPlanAddrs(in, a);
        for (j = 0; j < STRIP_PLAN_ROWS; j++) {
            ULONG p = g_StripRows[j].Pool;
            if (a[j] < s[p].Base || a[j] + g_StripRows[j].Dwords > s[p].Base + s[p].Words)
                ok = 0;
        }
        if (a[0] != (in ? 0x9D15Cu : 0x9D1B0u))
            ok = 0;
    }
    CHECK(ok, "Twin X strips 1-2 (headers 0x9D1B0 / 0x9D15C) inside the booked room");
    PlugLedgerUsage(&L, u0);
    printf("Twin X DSP 1 room after its 2 strips: code %lu of %lu (largest %lu), pool 1 %lu, pool 2 %lu, pool 3 %lu words\n",
           u0[0].Free, u0[0].Total, u0[0].Largest, u0[1].Free, u0[2].Free, u0[3].Free);
    {
        static PLUG_LEDGER X8;
        PLUG_LEASE s8[4];
        PLUG_USAGE u8[4];
        (void)PlugLedgerInit(&X8, kBase, kSize, kScr);
        (void)PlugLedgerBookStrips(&X8, STRIP_CODE_WORDS, kPer, 8, s8);
        PlugLedgerUsage(&X8, u8);
        CHECK(u0[0].Free == u8[0].Free && u0[1].Free == u8[1].Free + 6 * kPer[1] && u0[2].Free == u8[2].Free + 6 * kPer[2],
              "Twin X: the x8p's code room, and 6 strips' more data room than the x8p (pool 1 +%lu, pool 2 +%lu)", 6 * kPer[1], 6 * kPer[2]);
    }
    /* a chain placed beside them and taken off again (the ledger back to its baseline) */
    rq[0].Kind = 0; rq[0].Words = 3000; rq[0].Identity = 101; rq[0].Shared = 1;
    rq[1].Kind = 2; rq[1].Words = 1000; rq[1].Identity = 102; rq[1].Shared = 1;
    rq[2].Kind = 1; rq[2].Words = 1500; rq[2].Identity = 103; rq[2].Shared = 1;
    for (k = 0; k < NCHAIN; k++) {
        rq[3 + k].Kind = kChainRows[k].Pool;
        rq[3 + k].Words = kChainRows[k].Dwords;
        rq[3 + k].Identity = 0;
        rq[3 + k].Shared = 0;
    }
    ok = PlugLedgerSetScratch(&L, 1, 448) == PLUG_OK && PlugLedgerSetScratch(&L, 2, 192) == PLUG_OK &&
         PlugLedgerReserveBatch(&L, rq, 3 + NCHAIN, lease) == PLUG_OK && LedgerInvariant(&L) && lease[0].Base == 0xE0000 + STRIP_CODE_WORDS;
    for (k = 0; k < 3 + NCHAIN; k++)
        if (lease[k].Kind != 0 && lease[k].Base + lease[k].Words > s[lease[k].Kind].Base)
            ok = 0;
    CHECK(ok, "Twin X: a chain placed on DSP 1 below its strips' room");
    for (k = 3 + NCHAIN; k > 0; k--)
        (void)PlugLedgerRelease(&L, &lease[k - 1], &last);
    (void)PlugLedgerSetScratch(&L, 1, 0);
    (void)PlugLedgerSetScratch(&L, 2, 0);
    PlugLedgerUsage(&L, u1);
    CHECK(memcmp(u0, u1, sizeof(u0)) == 0 && LedgerInvariant(&L), "Twin X: taken off, the ledger back to its baseline");
}

/* ------------------------------------------------------------------ */
/* [Ins] Unison and aux: CREATE flags, the directory with a Unison     */
/* header, the aux return pairs, the analog lease (unison_plan.h)      */
/* ------------------------------------------------------------------ */

static void TestIns(void)
{
    PLUG_ROW rows[6];
    ULONG cmd[64], nw = 0, ids[5], nids = 0, carrier = 0, s2[32], s1[16], k;
    ULONG hp[7] = { 0, 1, 2, 4, 5, 7, 9 }, hv[7] = { 0x40000000, 1, 1, 0x22, 2, RENDER, RENDER + 1 };
    const APOLLO_PREAMP_MODEL *x8p = ApolloPreampModelFor(APOLLO_MODEL_X8P), *twx = ApolloPreampModelFor(APOLLO_MODEL_TWINX);
    APOLLO_UNISON_IMAGE im, next;
    ULONG cache[APOLLO_UNISON_SETTINGS], rb[APOLLO_UNISON_RB_WORDS], taken[APOLLO_UNISON_SETTINGS], req[APOLLO_UNISON_SETTINGS];
    ULONG before[APOLLO_UNISON_SETTINGS], gain[APOLLO_UNISON_SETTINGS], idx, mask, enc, w[6];
    int r;

    /* flags: UNISON only as STAGE | CHAIN | UNISON */
    CHECK(PlugCreateFlagsOk(PLUG_F_STAGE | PLUG_F_CHAIN | PLUG_F_UNISON) == PLUG_OK, "STAGE | CHAIN | UNISON accepted");
    CHECK(PlugCreateFlagsOk(PLUG_F_START | PLUG_F_CHAIN | PLUG_F_UNISON) == PLUG_EINVAL &&
          PlugCreateFlagsOk(PLUG_F_STAGE | PLUG_F_UNISON) == PLUG_EINVAL && PlugCreateFlagsOk(PLUG_F_UNISON) == PLUG_EINVAL &&
          PlugCreateFlagsOk(PLUG_F_CHECK | PLUG_F_UNISON) == PLUG_EINVAL, "UNISON with START, without CHAIN or with CHECK refused");

    /* aux return pairs */
    CHECK(PlugAuxSynch(0) == 0x00110010u && PlugAuxSynch(1) == 0x00130012u && PlugAuxSynch(2) == PLUG_NONE &&
          PlugAuxOf(0x00110010u) == 0 && PlugAuxOf(0x00130012u) == 1 && PlugAuxOf(0x00010000u) == PLUG_NONE &&
          PlugAuxOf(0x00200010u) == PLUG_NONE, "aux returns: 0x10/0x11 and 0x12/0x13 (ua_apollo.h UA_BUS_AUX1/2_RETURN)");
    CHECK(PlugSynchCheck(PlugAuxSynch(0), 8) == PLUG_ENOTSUP, "an aux pair is not an analog pair (the parser takes it apart)");

    /* the directory with a Unison header (member 0 at word 7, linked at 6) and one insert (word 9, linked at 8) */
    rows[0].Pool = 1; rows[0].Dwords = 10; rows[0].Link = 0xFFFFFFFFu; rows[0].Flags = 0;
    rows[1].Pool = 1; rows[1].Dwords = 20; rows[1].Link = 6; rows[1].Flags = 0;          /* the header's root */
    rows[2].Pool = 3; rows[2].Dwords = 64; rows[2].Link = (1u << 24) | 3; rows[2].Flags = 2;
    rows[3].Pool = 1; rows[3].Dwords = 16; rows[3].Link = 8; rows[3].Flags = 0;          /* the insert's root */
    WriteTo(cmd, &nw, 0, hp, hv, 7);
    CHECK(PlugCheckRows(rows, 4, s2, s1) == PLUG_OK && PlugCheckCmds(rows, 4, cmd, nw, &k) == PLUG_OK, "Unison chain rows and head");
    CHECK(PlugChainIdsEx(rows, 4, cmd, nw, 1, 1, RENDER, ids, &nids, &carrier) == PLUG_OK && nids == 2 && ids[0] == RENDER &&
          ids[1] == RENDER + 1 && carrier == RENDER, "Unison directory: header and insert both listed for the licence, header first");
    CHECK(PlugChainIdsEx(rows, 4, cmd, nw, 1, 0, RENDER, ids, &nids, &carrier) == PLUG_EINVAL,
          "the same head without UNISON: word 7 is not a carrier, refused");
    CHECK(PlugChainIdsEx(rows, 4, cmd, nw, 1, 1, RENDER + 1, ids, &nids, &carrier) == PLUG_EINVAL, "Unison: render id must be the header");
    CHECK(PlugChainIdsEx(rows, 4, cmd, nw, 0, 1, RENDER, ids, &nids, &carrier) == PLUG_ENOTSUP, "Unison without CHAIN refused");
    hv[5] = 0x846;
    nw = 0;
    WriteTo(cmd, &nw, 0, hp, hv, 7);
    CHECK(PlugChainIdsEx(rows, 4, cmd, nw, 1, 1, 0x846, ids, &nids, &carrier) == PLUG_EINVAL,
          "Unison: the Mic carrier is never a Unison header");
    /* N = 1: the header alone */
    {
        ULONG p1[6] = { 0, 1, 2, 4, 5, 7 }, v1[6] = { 0x40000000, 1, 1, 0x22, 1, RENDER };
        rows[0].Dwords = 8;
        nw = 0;
        WriteTo(cmd, &nw, 0, p1, v1, 6);
        CHECK(PlugChainIdsEx(rows, 3, cmd, nw, 1, 1, RENDER, ids, &nids, &carrier) == PLUG_OK && nids == 1,
              "Unison header alone (N = 1)");
        CHECK(PlugChainIdsEx(rows, 3, cmd, nw, 1, 0, RENDER, ids, &nids, &carrier) == PLUG_EINVAL, "N = 1 is a Unison form only");
    }

    /* the full preamp field map (preamp_plan.h ApolloPreampField) agrees with the ordinary encoder on what both offer */
    {
        APOLLO_PREAMP_WRITE ow[APOLLO_PRE_WRITES_MAX];
        unsigned long on = 0, ch, p, bad = 0;

        for (ch = 0; ch < 8; ch++) {
            for (p = 0; p <= 5; p++) {
                if (p == APOLLO_PRE_LINK && (ch & 1)) continue;
                if (ApolloPreampEncode(x8p, ch, p, 1, ow, &on) != APOLLO_PRE_OK || on != 1) { bad++; continue; }
                bad += ApolloPreampField(x8p, ch, p, 1, &idx, &mask, &enc) != APOLLO_PRE_OK || idx != ow[0].Setting ||
                       mask != ow[0].Mask || enc != ow[0].Value;
            }
            if (ApolloPreampEncode(x8p, ch, APOLLO_PRE_GAIN_DB, 40, ow, &on) != APOLLO_PRE_OK || on != 3) { bad++; continue; }
            bad += ApolloPreampField(x8p, ch, APOLLO_PRE_F_GAIN_A, 30, &idx, &mask, &enc) != APOLLO_PRE_OK || idx != ow[0].Setting ||
                   mask != ow[0].Mask || enc != ow[0].Value;
            bad += ApolloPreampField(x8p, ch, APOLLO_PRE_F_GAIN_B, 30, &idx, &mask, &enc) != APOLLO_PRE_OK || idx != ow[1].Setting ||
                   mask != ow[1].Mask || enc != ow[1].Value;
            bad += ApolloPreampField(x8p, ch, APOLLO_PRE_F_GAIN_C, 31, &idx, &mask, &enc) != APOLLO_PRE_OK || idx != ow[2].Setting ||
                   mask != ow[2].Mask || enc != ow[2].Value;
        }
        CHECK(bad == 0, "field map = SET_INPUT_PARAM's writes for Mic/Line, PAD, Link, 48V, LowCut, Phase and the gains (x8p, 8 preamps)");
        CHECK(ApolloPreampField(x8p, 5, APOLLO_PRE_F_CUSTOM, 1, &idx, &mask, &enc) == APOLLO_PRE_OK && idx == 11 && mask == (1ul << 13) &&
              ApolloPreampField(x8p, 6, APOLLO_PRE_F_MIC_IMP, 3, &idx, &mask, &enc) == APOLLO_PRE_OK && idx == 13 && enc == (3ul << 20) &&
              ApolloPreampField(x8p, 4, 0x0D, 0xAB, &idx, &mask, &enc) == APOLLO_PRE_OK && idx == 0x0D + 7 + 8 && enc == 0xAB &&
              ApolloPreampField(x8p, 4, APOLLO_PRE_F_HIZ, 1, &idx, &mask, &enc) == APOLLO_PRE_NOT_HERE &&
              ApolloPreampField(x8p, 1, APOLLO_PRE_F_HIZ, 1, &idx, &mask, &enc) == APOLLO_PRE_OK && mask == (1ul << 25) &&
              ApolloPreampField(x8p, 0, APOLLO_PRE_F_ROUTE, 1, &idx, &mask, &enc) == APOLLO_PRE_NOT_HERE &&
              ApolloPreampField(x8p, 6, APOLLO_PRE_F_PGA_LINE, 1, &idx, &mask, &enc) == APOLLO_PRE_OK && idx == 15 &&
              mask == (1ul << 30) && enc == mask &&
              ApolloPreampField(twx, 0, APOLLO_PRE_F_PGA_LINE, 1, &idx, &mask, &enc) == APOLLO_PRE_NOT_HERE &&
              ApolloPreampField(twx, 2, APOLLO_PRE_F_GAIN_C, 1, &idx, &mask, &enc) == APOLLO_PRE_BAD_VALUE &&
              ApolloPreampField(twx, 1, APOLLO_PRE_F_HIZ_IMP, 1, &idx, &mask, &enc) == APOLLO_PRE_NOT_HERE,
              "CUSTOM S11 bit 8+ch, mic impedance S13 (hi bank), custom lanes S16+/S24+, Hi-Z low bank only, Hi-Z impedance on the Hi-Z inputs, ring params refused, PGA line S15 bit 24+ch on the x8p only");
    }

    /* the analog lease on x8p input 3 (preamp 2): capture, PGA 0, restore plan */
    memset(cache, 0, sizeof(cache));
    memset(rb, 0, sizeof(rb));
    memset(taken, 0, sizeof(taken));
    rb[3] = 31ul << 16;                         /* GainC of preamp 2 = 31 (40 dB) at S3 shift 16 */
    rb[0] = 1ul << (12 + 3);                    /* its 48V on */
    cache[0] = 1ul << (12 + 3);
    r = ApolloUnisonCapture(x8p, 2 | (0x20u << 16), cache, rb, 0, 0, taken, &im);
    CHECK(r == APOLLO_UNISON_AGAIN && !im.Valid, "capture: no settled mailbox, no baseline");
    rb[38] = rb[39] = 0xFFFFFFFFul;
    CHECK(ApolloUnisonCapture(x8p, 2 | (0x20u << 16), cache, rb, 1, 0, taken, &im) == APOLLO_UNISON_AGAIN, "capture: readback words 38/39 all ones");
    rb[38] = rb[39] = 0;
    r = ApolloUnisonCapture(x8p, 2 | (0x20u << 16), cache, rb, 1, 0, taken, &im);
    CHECK(r == APOLLO_UNISON_OK && im.Valid && (im.Mask[3] & (0x3Ful << 16)) && (im.Mask[11] & (1ul << 10)) &&
          (im.Mask[1] & (0xFul << 20)) && (im.Mask[0] & (0x3Ful << 12)) == (0x3Ful << 12) && (im.Mask[0] & (0x104ul << 12)) != (0x104ul << 12) &&
          (im.Value[17] >> 16 & 0xFF) == 30 && (im.Value[16] >> 16 & 0xFF) == 30 && (im.Value[3] >> 16 & 0x3F) == 31,
          "capture: preamp 3's fields owned (flags, GainC, CUSTOM, impedance, lanes), not the pair's Link; GainA/B follow GainC (40 dB)");
    CHECK(im.Mask[3] == (0x3Ful << 16) && im.Mask[0] == ((0x3Ful << 12) | (1ul << 26)) && (im.Mask[5] & ~(1ul << 18)) == 0,
          "capture: nothing of preamps 1, 2, 4 owned");
    cache[11] = 1ul << 10;
    CHECK(ApolloUnisonCapture(x8p, 2 | (0x20u << 16), cache, rb, 1, 0, taken, &next) == APOLLO_UNISON_BUSY, "capture: CUSTOM already on: busy");
    cache[11] = 0;
    CHECK(ApolloUnisonCapture(x8p, 2 | (0x20u << 16), cache, rb, 1, 1ul << 2, taken, &next) == APOLLO_UNISON_BUSY, "capture: owned by another lease: busy");
    taken[3] = 1ul << 16;
    CHECK(ApolloUnisonCapture(x8p, 2 | (0x20u << 16), cache, rb, 1, 0, taken, &next) == APOLLO_UNISON_BUSY, "capture: a blocked field: busy");
    taken[3] = 0;
    CHECK(ApolloUnisonCapture(twx, 2 | (0x20u << 16), cache, rb, 1, 0, taken, &next) == APOLLO_UNISON_INVAL, "capture: the Twin X has no preamp 3");

    k = ApolloUnisonPga0(2 | (0x20u << 16), w);
    CHECK(k == 3 && w[0] == 2 && w[1] == APOLLO_PRE_F_GAIN_C && w[2] == 0, "PGA 0: GainC code 0 on the lease's input");
    CHECK(ApolloUnisonPatch(x8p, &im, w, k, &next, req) == APOLLO_UNISON_OK && req[3] == (0x3Ful << 16) && (next.Value[3] & (0x3Ful << 16)) == 0,
          "patch: GainC 0 written only to S3 [21:16]");
    {
        ULONG other[3] = { 3, APOLLO_PRE_F_GAIN_C, 0 }, link[3] = { 2, APOLLO_PRE_LINK, 1 }, two[6] = { 2, 6, 10, 2, 6, 20 };
        CHECK(ApolloUnisonPatch(x8p, &im, other, 3, &next, req) == APOLLO_UNISON_PERM, "patch: an input outside the lease: refused");
        CHECK(ApolloUnisonPatch(x8p, &im, link, 3, &next, req) == APOLLO_UNISON_INVAL, "patch: Link (pair-wide): refused");
        CHECK(ApolloUnisonPatch(x8p, &im, link, 2, &next, req) == APOLLO_UNISON_INVAL, "patch: not whole triples: refused");
        CHECK(ApolloUnisonPatch(x8p, &im, two, 6, &next, req) == APOLLO_UNISON_OK && ((next.Value[3] >> 16) & 0x3F) == 20,
              "patch: duplicates coalesce, the last wins");
    }
    /* the cache after PGA 0 and CUSTOM 1; the restore plan puts CUSTOM off before, the gains after */
    {
        ULONG cw[6] = { 2, APOLLO_PRE_F_GAIN_C, 0, 2, APOLLO_PRE_F_CUSTOM, 1 };
        CHECK(ApolloUnisonPatch(x8p, &im, cw, 6, &next, req) == APOLLO_UNISON_OK, "patch: PGA 0 and CUSTOM 1");
        for (k = 0; k < APOLLO_UNISON_SETTINGS; k++) cache[k] = next.Value[k];
    }
    ApolloUnisonRestorePlan(x8p, &im, cache, before, gain);
    CHECK((before[11] & (1ul << 10)) && (before[3] & (0x3Ful << 16)) == 0 && gain[3] == (0x3Ful << 16) &&
          gain[17] == (0xFFul << 16) && gain[16] == (0xFFul << 16), "restore plan: CUSTOM off first; GainC, GainA, GainB after the 80 ms");
    rb[3] = 31ul << 16;
    CHECK(ApolloUnisonGainRestored(&im, rb), "restore: GainC read back = the baseline's");
    rb[3] = 30ul << 16;
    CHECK(!ApolloUnisonGainRestored(&im, rb), "restore: another GainC is a failed restore");
    CHECK(ApolloUnisonPreamps(0x00010000u) == 3 && ApolloUnisonPreamps(0x00200005u) == 0x20, "lease inputs: a pair, a mono input");
}

int main(void)
{
    /* the encodings' digest, recorded at P0 (Nov 14 2026): a change here is a change on the device */
    const ULONG kGolden = 0xC7523218u;
    /* staged startup (stage page, SERVICE pages, START), recorded Nov 15 2026 */
    const ULONG kGoldenStage = 0x249D3C5Cu;

    TestBill();
    TestLicence();
    TestPaging();
    TestLedgerStrips();
    TestLedgerChain();
    TestLedgerChurn();
    TestRecipe();
    TestPrivMax();
    TestCreateFlags();
    TestChunks();
    TestStaged();
    TestStereo();
    TestCaller();
    TestModels();
    TestIns();
    printf("encodings digest 0x%08lx (golden 0x%08lx)\n", g_fnv, kGolden);
    CHECK(g_fnv == kGolden, "encodings digest");
    printf("staged encodings digest 0x%08lx (golden 0x%08lx)\n", g_fnvStage, kGoldenStage);
    CHECK(g_fnvStage == kGoldenStage, "staged encodings digest");
    printf("plugin-plan-test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
