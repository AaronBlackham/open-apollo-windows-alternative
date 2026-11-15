/*
 * ring-map-test.c - [G4b] offline checks of driver\ring_map.h, the page math of the audio rings.
 * No device, no Windows headers: runs in tools\build-all.cmd.
 *
 *   1. A contiguous ring (one 4 MiB chunk): every SG entry and both SG words are what the pre-G4b
 *      ApolloAudioProgramSg wrote (base + i * 4 KiB), so the x8p's table is unchanged.
 *   2. The page path (1024 chunks at scattered addresses), and a middle size (64 KiB): SG entry i
 *      and page i of the linear CPU view are the same memory, for every page and for byte offsets
 *      inside frames that straddle a page (x8p and Twin X strides).
 *   3. The chunk checks (alignment, the 4 GiB window), the chunk sizes, the log summary.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <string.h>
#include "..\..\driver\regs.h"
#include "..\..\driver\ring_map.h"

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

static unsigned long long g_chunkLa[AUDIO_SG_ENTRIES];
static unsigned long long g_pageLa[AUDIO_SG_ENTRIES];

/* A fixed shuffle of page addresses below 4 GiB: no two equal, few neighbours (xorshift). */
static void ScatterPages(unsigned long long *La, unsigned long N)
{
    unsigned long i, x = 0x2545F491ul;

    for (i = 0; i < N; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        /* page frame (x % 0xF0000) spread over 0x1000_0000..0xFFFF_F000, i keeps them unique */
        La[i] = 0x10000000ull + ((unsigned long long)((x % 0xEF000ul) / AUDIO_SG_ENTRIES * AUDIO_SG_ENTRIES + i)
                                 * AUDIO_SG_PAGE);
    }
}

/*
 * The invariant: the memory the FPGA reaches through SG entry i (PageLa[i]) is the memory the CPU
 * reaches at page i of the linear view (ring_map.h ApolloRingChunkOfPage / ApolloRingPageInChunk,
 * exactly what audio.c ApolloRingMapView puts into the MDL). Found by searching the chunks for
 * PageLa[i], independently of ring_map.h's own formula.
 */
static void CheckSameMemory(unsigned long ChunkBytes, const char *What)
{
    unsigned long n = ApolloRingChunkCount(ChunkBytes), i, c, bad = 0;

    for (i = 0; i < AUDIO_SG_ENTRIES; i++) {
        unsigned long long la = g_pageLa[i];
        unsigned long found = n;

        for (c = 0; c < n; c++) {
            if (la >= g_chunkLa[c] && la < g_chunkLa[c] + ChunkBytes) {
                found = c;
                break;
            }
        }
        if (found == n || found != ApolloRingChunkOfPage(i, ChunkBytes) ||
            la - g_chunkLa[found] != (unsigned long long)ApolloRingPageInChunk(i, ChunkBytes) * AUDIO_SG_PAGE) {
            if (bad++ < 4) {
                printf("  %s: page %lu -> LA 0x%llx (chunk %lu), CPU view chunk %lu page %lu\n", What, i, la,
                       found, ApolloRingChunkOfPage(i, ChunkBytes), ApolloRingPageInChunk(i, ChunkBytes));
            }
        }
    }
    CHECK(bad == 0, "%s: SG entry i and view page i are the same memory (%lu pages differ)", What, bad);
}

/* Frames of Stride bytes: every byte's device address is its page's LA + its offset in the page. */
static void CheckFrames(unsigned long Stride, const char *What)
{
    unsigned long f, b, bad = 0, straddle = 0;

    for (f = 0; f < AUDIO_RING_FRAMES && (unsigned long long)(f + 1) * Stride <= AUDIO_RING_BYTES; f++) {
        unsigned long long off = (unsigned long long)f * Stride;

        if (ApolloRingPageOf(off) != ApolloRingPageOf(off + Stride - 1)) {
            straddle++;
        }
        for (b = 0; b < Stride; b += 4) {
            unsigned long long o = off + b;
            unsigned long long want = g_pageLa[o / AUDIO_SG_PAGE] + o % AUDIO_SG_PAGE;

            if (ApolloRingDeviceAddr(g_pageLa, o) != want) {
                bad++;
            }
        }
    }
    /* a stride that divides 4 KiB (x8p record, 128 bytes) never straddles; 136 and 184 do */
    CHECK(bad == 0 && (straddle > 0) == (AUDIO_SG_PAGE % Stride != 0),
          "%s: every sample's device address (%lu wrong, %lu frames straddle a page)", What, bad, straddle);
}

static void TestContiguous(void)
{
    static const unsigned long long bases[] = { 0x2EA40000ull, 0x1AECC000ull, 0x1000ull, 0xFFC00000ull };
    unsigned long k, i;

    CHECK(ApolloRingChunkCount(AUDIO_RING_BYTES) == 1 && ApolloRingPagesPerChunk(AUDIO_RING_BYTES) == 1024,
          "contiguous: one chunk of 1024 pages");
    for (k = 0; k < sizeof(bases) / sizeof(bases[0]); k++) {
        unsigned long bad = 0;
        APOLLO_RING_SPAN s;

        memset(g_chunkLa, 0, sizeof(g_chunkLa));
        g_chunkLa[0] = bases[k];
        CHECK(ApolloRingChunkOk(bases[k], 0xFFFFC00012340000ull, AUDIO_RING_BYTES),
              "contiguous 4 MiB at 0x%llx is usable", bases[k]);
        ApolloRingFillPageLa(g_chunkLa, AUDIO_RING_BYTES, g_pageLa);
        for (i = 0; i < AUDIO_SG_ENTRIES; i++) {
            /* audio.c before G4b: p = Ctx->Play.La + (ULONGLONG)i * AUDIO_SG_PAGE; (ULONG)p; (ULONG)(p >> 32) */
            unsigned long long p = bases[k] + (unsigned long long)i * AUDIO_SG_PAGE;

            if (g_pageLa[i] != p || ApolloSgLo(g_pageLa[i]) != (unsigned long)p ||
                ApolloSgHi(g_pageLa[i]) != (unsigned long)(p >> 32)) {
                bad++;
            }
        }
        CHECK(bad == 0, "contiguous at 0x%llx: SG table = the pre-G4b one (%lu entries differ)", bases[k], bad);
        CHECK(g_pageLa[0] == bases[k], "contiguous: page 0 is the base (the 'SG tables programmed' log line)");
        ApolloRingSpan(g_pageLa, &s);
        CHECK(s.Lowest == bases[k] && s.Highest == bases[k] + 0x3FF000ull && s.Runs == 1,
              "contiguous span: 0x%llx..0x%llx, %lu runs", s.Lowest, s.Highest, s.Runs);
        CheckSameMemory(AUDIO_RING_BYTES, "contiguous");
    }
    CheckFrames(34 * 4, "contiguous, x8p playback (34 slots)");
}

static void TestPages(void)
{
    unsigned long i, bad = 0, minI = 0, maxI = 0, runs = 1;
    APOLLO_RING_SPAN s;

    CHECK(ApolloRingChunkCount(AUDIO_SG_PAGE) == AUDIO_SG_ENTRIES && ApolloRingPagesPerChunk(AUDIO_SG_PAGE) == 1,
          "pages: 1024 chunks of one page");
    ScatterPages(g_chunkLa, AUDIO_SG_ENTRIES);
    for (i = 0; i < AUDIO_SG_ENTRIES; i++) {
        CHECK(ApolloRingChunkOk(g_chunkLa[i], 0xFFFFC00000000000ull + (unsigned long long)i * 0x3000, AUDIO_SG_PAGE),
              "scattered page %lu at 0x%llx is usable", i, g_chunkLa[i]);
        if (g_chunkLa[i] < g_chunkLa[minI]) {
            minI = i;
        }
        if (g_chunkLa[i] > g_chunkLa[maxI]) {
            maxI = i;
        }
        if (i > 0 && g_chunkLa[i] != g_chunkLa[i - 1] + AUDIO_SG_PAGE) {
            runs++;
        }
    }
    ApolloRingFillPageLa(g_chunkLa, AUDIO_SG_PAGE, g_pageLa);
    for (i = 0; i < AUDIO_SG_ENTRIES; i++) {
        if (g_pageLa[i] != g_chunkLa[i] || ApolloSgHi(g_pageLa[i]) != 0) {
            bad++;
        }
    }
    CHECK(bad == 0, "pages: SG entry i = common buffer i (%lu differ)", bad);
    ApolloRingSpan(g_pageLa, &s);
    CHECK(s.Lowest == g_chunkLa[minI] && s.Highest == g_chunkLa[maxI] && s.Runs == runs && runs > 1,
          "pages span: 0x%llx..0x%llx, %lu runs (want 0x%llx..0x%llx, %lu)", s.Lowest, s.Highest, s.Runs,
          g_chunkLa[minI], g_chunkLa[maxI], runs);
    CheckSameMemory(AUDIO_SG_PAGE, "pages");
    CheckFrames(34 * 4, "pages, x8p playback (34 slots)");
    CheckFrames(32 * 4, "pages, x8p record (32 slots)");
    CheckFrames(46 * 4, "pages, Twin X record (46 slots)");

    /* Pages that happen to be consecutive give one run, like a contiguous ring */
    for (i = 0; i < AUDIO_SG_ENTRIES; i++) {
        g_chunkLa[i] = 0x40000000ull + (unsigned long long)i * AUDIO_SG_PAGE;
    }
    ApolloRingFillPageLa(g_chunkLa, AUDIO_SG_PAGE, g_pageLa);
    ApolloRingSpan(g_pageLa, &s);
    CHECK(s.Runs == 1 && s.Lowest == 0x40000000ull && s.Highest == 0x403FF000ull, "consecutive pages: one run");

    /* Reversed pages: 1024 runs, still the same memory page for page */
    for (i = 0; i < AUDIO_SG_ENTRIES; i++) {
        g_chunkLa[i] = 0x50000000ull + (unsigned long long)(AUDIO_SG_ENTRIES - 1 - i) * AUDIO_SG_PAGE;
    }
    ApolloRingFillPageLa(g_chunkLa, AUDIO_SG_PAGE, g_pageLa);
    ApolloRingSpan(g_pageLa, &s);
    CHECK(s.Runs == AUDIO_SG_ENTRIES && s.Lowest == 0x50000000ull && s.Highest == 0x503FF000ull,
          "reversed pages: %lu runs", s.Runs);
    CheckSameMemory(AUDIO_SG_PAGE, "reversed pages");
}

static void TestMiddle(void)
{
    const unsigned long cb = 0x10000;   /* 64 KiB: 64 chunks of 16 pages */
    unsigned long c;

    CHECK(ApolloRingChunkCount(cb) == 64 && ApolloRingPagesPerChunk(cb) == 16, "64 KiB: 64 chunks of 16 pages");
    for (c = 0; c < 64; c++) {
        g_chunkLa[c] = 0x80000000ull - (unsigned long long)(c + 1) * 0x30000;    /* gaps, descending */
    }
    ApolloRingFillPageLa(g_chunkLa, cb, g_pageLa);
    CHECK(g_pageLa[0] == g_chunkLa[0] && g_pageLa[15] == g_chunkLa[0] + 15 * AUDIO_SG_PAGE &&
          g_pageLa[16] == g_chunkLa[1] && g_pageLa[1023] == g_chunkLa[63] + 15 * AUDIO_SG_PAGE,
          "64 KiB chunks: page boundaries");
    CheckSameMemory(cb, "64 KiB chunks");
    CheckFrames(34 * 4, "64 KiB chunks, x8p playback");
}

static void TestChecks(void)
{
    /* the 4 GiB window: the last page that fits, and one byte beyond */
    CHECK(ApolloRingChunkOk(0xFFFFF000ull, 0, AUDIO_SG_PAGE), "page ending exactly at 4 GiB fits");
    CHECK(!ApolloRingChunkOk(0xFFFFF000ull, 0, 2 * AUDIO_SG_PAGE), "8 KiB from 0xFFFFF000 crosses 4 GiB");
    CHECK(!ApolloRingChunkOk(0x100000000ull, 0, AUDIO_SG_PAGE), "page at 4 GiB: out of the window");
    CHECK(!ApolloRingChunkOk(0xFFFFFFFFFFFFF000ull, 0, AUDIO_SG_PAGE), "no wrap-around at 2^64");
    CHECK(ApolloRingChunkOk(0xFFC00000ull, 0, AUDIO_RING_BYTES), "4 MiB ending at 4 GiB fits");
    CHECK(!ApolloRingChunkOk(0xFFC01000ull, 0, AUDIO_RING_BYTES), "4 MiB from 0xFFC01000 crosses 4 GiB");
    /* alignment, both sides */
    CHECK(!ApolloRingChunkOk(0x10000800ull, 0, AUDIO_SG_PAGE), "logical address not page aligned");
    CHECK(!ApolloRingChunkOk(0x10000000ull, 0xFFFF800000000040ull, AUDIO_SG_PAGE), "kernel address not page aligned");
    CHECK(!ApolloRingChunkOk(0x10000000ull, 0, 0), "zero bytes");
    /* chunk sizes */
    CHECK(ApolloRingChunkBytesOk(AUDIO_SG_PAGE) && ApolloRingChunkBytesOk(AUDIO_RING_BYTES) &&
          ApolloRingChunkBytesOk(0x10000), "page, 64 KiB and 4 MiB chunks");
    CHECK(!ApolloRingChunkBytesOk(0) && !ApolloRingChunkBytesOk(0x800) && !ApolloRingChunkBytesOk(0x3000) &&
          !ApolloRingChunkBytesOk(2 * AUDIO_RING_BYTES), "no 0, half-page, 12 KiB or 8 MiB chunks");
    CHECK(ApolloRingChunkCount(0x3000) == 0 && ApolloRingPagesPerChunk(0) == 0 &&
          ApolloRingChunkOfPage(5, 0) == 0 && ApolloRingPageInChunk(5, 0) == 0, "bad sizes give 0, never divide by 0");
    /* linear view: byte offset -> page */
    CHECK(ApolloRingPageOf(0) == 0 && ApolloRingPageOf(0xFFF) == 0 && ApolloRingPageOf(0x1000) == 1 &&
          ApolloRingPageOf(AUDIO_RING_BYTES - 1) == AUDIO_SG_ENTRIES - 1, "byte offset -> page");
    CHECK(ApolloRingOffsetInPage(0x1234) == 0x234 && ApolloRingOffsetInPage(AUDIO_RING_BYTES - 1) == 0xFFF,
          "byte offset -> offset in page");
    /* the SG words of an address above 4 GiB (never programmed: the check refuses it) */
    CHECK(ApolloSgLo(0x123456789000ull) == 0x56789000ul && ApolloSgHi(0x123456789000ull) == 0x1234ul,
          "SG words lo/hi");
    /* the geometry the rings are used with fits the linear view */
    CHECK((unsigned long long)AUDIO_RING_FRAMES * AUDIO_MAX_CHANNELS * 4 <= AUDIO_RING_BYTES,
          "8192 frames x 64 slots fit the 4 MiB view");
}

int main(void)
{
    TestContiguous();
    TestPages();
    TestMiddle();
    TestChecks();
    printf("ring-map-test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
