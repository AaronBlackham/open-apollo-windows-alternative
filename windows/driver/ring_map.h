/*
 * ring_map.h - page math of the two audio rings (docs/spec-streaming.md 1.1-1.3).
 *
 * Plain C like bringup_plan.h: no kernel or Windows headers, every function static __inline.
 * driver\audio.c allocates the rings with it; tools\ring-map-test checks it offline.
 *
 * A ring is AUDIO_RING_BYTES (4 MiB) that the FPGA reads through its SG table of AUDIO_SG_ENTRIES
 * (1024) entries, one AUDIO_SG_PAGE (4 KiB) page each (regs.h). The FPGA never needs the pages to
 * be contiguous. The driver holds a ring as "chunks": common buffers of ChunkBytes each, in ring
 * order, every one SG-page aligned and below 4 GiB. Two chunk sizes are used (audio.c):
 *   AUDIO_RING_BYTES   one contiguous common buffer (the first choice); SG entry i = base + i * 4 KiB;
 *   AUDIO_SG_PAGE      one common buffer per SG page (1024 chunks), when no contiguous 4 MiB
 *                      below 4 GiB is left (0xc000009a seen on the Twin X after hours of uptime),
 *                      or when the unit's AudioRingPages value asks for it.
 * Whatever the chunk size, the consumers see the same two things: PageLa[i], the device (logical)
 * address of SG page i, and one linear CPU view in which ring byte Off is page Off / AUDIO_SG_PAGE.
 * SG entry i and page i of the linear view are the same memory: chunk i / PagesPerChunk, page
 * i % PagesPerChunk of it (ring-map-test checks exactly that).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once
#include "regs.h"

/* The FPGA's DMA window: the enabler's 32-bit profile (driver.c), "may only use the low 32 bits". */
#define APOLLO_RING_DMA_LIMIT       0x100000000ULL

/* A chunk size the ring can be split into: whole SG pages that divide the ring. */
static __inline int ApolloRingChunkBytesOk(unsigned long ChunkBytes)
{
    return ChunkBytes >= AUDIO_SG_PAGE && ChunkBytes <= AUDIO_RING_BYTES &&
           (ChunkBytes % AUDIO_SG_PAGE) == 0 && (AUDIO_RING_BYTES % ChunkBytes) == 0;
}

static __inline unsigned long ApolloRingChunkCount(unsigned long ChunkBytes)
{
    return ApolloRingChunkBytesOk(ChunkBytes) ? AUDIO_RING_BYTES / ChunkBytes : 0;
}

static __inline unsigned long ApolloRingPagesPerChunk(unsigned long ChunkBytes)
{
    return ApolloRingChunkBytesOk(ChunkBytes) ? ChunkBytes / AUDIO_SG_PAGE : 0;
}

/*
 * Is one common buffer usable as ring memory: its logical address and its kernel address both
 * SG-page aligned (the CPU side for the linear view of the page path), and the whole of it inside
 * the FPGA's 32-bit window.
 */
static __inline int ApolloRingChunkOk(unsigned long long La, unsigned long long Va, unsigned long Bytes)
{
    return Bytes != 0 && (La & (AUDIO_SG_PAGE - 1)) == 0 && (Va & (AUDIO_SG_PAGE - 1)) == 0 &&
           (unsigned long long)Bytes <= APOLLO_RING_DMA_LIMIT && La <= APOLLO_RING_DMA_LIMIT - Bytes;
}

/* Ring page -> the chunk holding it, and its page number inside that chunk. */
static __inline unsigned long ApolloRingChunkOfPage(unsigned long Page, unsigned long ChunkBytes)
{
    unsigned long ppc = ApolloRingPagesPerChunk(ChunkBytes);

    return ppc != 0 ? Page / ppc : 0;
}

static __inline unsigned long ApolloRingPageInChunk(unsigned long Page, unsigned long ChunkBytes)
{
    unsigned long ppc = ApolloRingPagesPerChunk(ChunkBytes);

    return ppc != 0 ? Page % ppc : 0;
}

/* Ring byte offset (linear view) -> SG page index, and the byte inside that page. */
static __inline unsigned long ApolloRingPageOf(unsigned long long Off)
{
    return (unsigned long)(Off / AUDIO_SG_PAGE);
}

static __inline unsigned long ApolloRingOffsetInPage(unsigned long long Off)
{
    return (unsigned long)(Off % AUDIO_SG_PAGE);
}

/*
 * PageLa[AUDIO_SG_ENTRIES] from the chunks' logical addresses (ApolloRingChunkCount(ChunkBytes) of
 * them, in ring order): SG page i is page i % ppc of chunk i / ppc.
 */
static __inline void ApolloRingFillPageLa(const unsigned long long *ChunkLa, unsigned long ChunkBytes,
                                          unsigned long long *PageLa)
{
    unsigned long i;

    for (i = 0; i < AUDIO_SG_ENTRIES; i++) {
        PageLa[i] = ChunkLa[ApolloRingChunkOfPage(i, ChunkBytes)] +
                    (unsigned long long)ApolloRingPageInChunk(i, ChunkBytes) * AUDIO_SG_PAGE;
    }
}

/* The device address of ring byte Off: where the FPGA reaches it through SG entry Off / 4 KiB. */
static __inline unsigned long long ApolloRingDeviceAddr(const unsigned long long *PageLa, unsigned long long Off)
{
    return PageLa[ApolloRingPageOf(Off)] + ApolloRingOffsetInPage(Off);
}

/* SG entry i is {u32 lo, u32 hi} at REG_PLAY_SG / REG_REC_SG + i * 8 (spec-streaming 1.2). */
static __inline unsigned long ApolloSgLo(unsigned long long La)
{
    return (unsigned long)(La & 0xFFFFFFFFull);
}

static __inline unsigned long ApolloSgHi(unsigned long long La)
{
    return (unsigned long)(La >> 32);
}

/* For the log line: the lowest and highest page address, and how many runs of consecutive pages. */
typedef struct _APOLLO_RING_SPAN {
    unsigned long long  Lowest;
    unsigned long long  Highest;    /* page base (the ring's last byte there is + 0xFFF) */
    unsigned long       Runs;       /* 1 = one contiguous range */
} APOLLO_RING_SPAN;

static __inline void ApolloRingSpan(const unsigned long long *PageLa, APOLLO_RING_SPAN *S)
{
    unsigned long i;

    S->Lowest = PageLa[0];
    S->Highest = PageLa[0];
    S->Runs = 1;
    for (i = 1; i < AUDIO_SG_ENTRIES; i++) {
        if (PageLa[i] < S->Lowest) {
            S->Lowest = PageLa[i];
        }
        if (PageLa[i] > S->Highest) {
            S->Highest = PageLa[i];
        }
        if (PageLa[i] != PageLa[i - 1] + AUDIO_SG_PAGE) {
            S->Runs++;
        }
    }
}
