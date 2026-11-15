/*
 * apollo_asio.h - kernel interface for the ASIO driver (docs/ASIO.md).
 *
 * The ASIO DLL opens the Apollo device interface (GENERIC_READ), asks the
 * driver to map a shared block into its process, and gives it an auto-reset
 * event. The phase 2 copy engine fills one half of each buffer while the
 * client works on the other, then publishes the half and sets the event.
 *
 * Function codes 0x20-0x2F are reserved for ASIO.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once
#include "apollo_ioctl.h"

#define IOCTL_APOLLO_ASIO_OPEN      APOLLO_CTL(0x20, APOLLO_USER)
#define IOCTL_APOLLO_ASIO_START     APOLLO_CTL(0x21, APOLLO_USER)
#define IOCTL_APOLLO_ASIO_STOP      APOLLO_CTL(0x22, APOLLO_USER)
#define IOCTL_APOLLO_ASIO_CLOSE     APOLLO_CTL(0x23, APOLLO_USER)

#define APOLLO_ASIO_VERSION         2   /* 2: ClientDone per half, ClientSkipped */
#define APOLLO_ASIO_MAX_IN          64
#define APOLLO_ASIO_MAX_OUT         64
#define APOLLO_ASIO_MIN_FRAMES      64
#define APOLLO_ASIO_MAX_FRAMES      1024

/* ASIO_OPEN input. One client at a time: a second open gets STATUS_DEVICE_BUSY. */
typedef struct _APOLLO_ASIO_OPEN_IN {
    ULONG       Version;            /* APOLLO_ASIO_VERSION */
    ULONG       BufferFrames;       /* power of two, MIN..MAX */
    ULONG       InCount;            /* active ASIO inputs */
    ULONG       OutCount;           /* active ASIO outputs */
    UCHAR       InSlots[APOLLO_ASIO_MAX_IN];    /* record ring slot for each input */
    UCHAR       OutSlots[APOLLO_ASIO_MAX_OUT];  /* playback ring slot for each output */
    ULONGLONG   EventHandle;        /* auto-reset event in the caller's process */
} APOLLO_ASIO_OPEN_IN, *PAPOLLO_ASIO_OPEN_IN;

/* ASIO_OPEN output: where the shared block landed in the caller's address space. */
typedef struct _APOLLO_ASIO_OPEN_OUT {
    ULONGLONG   SharedAddress;
    ULONG       SharedBytes;
    ULONG       Reserved;
    ULONG       InOffset[2];        /* byte offset of half h's first input channel */
    ULONG       OutOffset[2];       /* byte offset of half h's first output channel */
    ULONG       ChannelStride;      /* bytes between channels = BufferFrames * 4 */
    ULONG       Reserved2;
} APOLLO_ASIO_OPEN_OUT, *PAPOLLO_ASIO_OPEN_OUT;

/*
 * First page of the shared block. The engine writes, the client reads, except
 * ClientDone[] and ClientSkipped. Seqlock: read Sequence (even = stable), the
 * fields, Sequence again; retry if it changed or was odd.
 * Samples are LONG (ASIOSTInt32LSB): 24 valid bits at the top, as the ring holds them.
 *
 * Per-half handshake: after processing the switch S that published half h
 * (HalfReady = h, SwitchCount = S), the client stores S in ClientDone[h]. The
 * engine plays half h only if ClientDone[h] equals the SwitchCount it published
 * there, else silence (Underruns). It counts Overruns when it overwrites a captured
 * half whose ClientDone[h] never reached that switch. A client that skips a switch
 * (woke up too late) therefore gets silence for it, never the half's stale output.
 */
typedef struct _APOLLO_ASIO_SHARED {
    ULONG           Version;
    ULONG           BufferFrames;
    ULONG           InCount;
    ULONG           OutCount;
    volatile LONG   Sequence;
    volatile LONG   HalfReady;      /* the half the client must process now: 0 or 1 */
    volatile LONG   SwitchCount;    /* +1 per published half */
    volatile LONG   ClientSkipped;  /* client: switches it never processed (statistics only) */
    volatile LONGLONG SamplePosition;   /* frames at the start of HalfReady */
    volatile LONGLONG Qpc;              /* KeQueryPerformanceCounter at that frame */
    LONGLONG        QpcFrequency;
    volatile LONG   Overruns;       /* capture half overwritten before the client processed it */
    volatile LONG   Underruns;      /* render half due before the client processed its switch */
    ULONG           InputLatencyFrames;     /* excluding the buffer itself */
    ULONG           OutputLatencyFrames;
    ULONG           SampleRate;
    volatile LONG   ClientDone[2];  /* client: SwitchCount it finished in half h */
    ULONG           Reserved[5];
} APOLLO_ASIO_SHARED, *PAPOLLO_ASIO_SHARED;
