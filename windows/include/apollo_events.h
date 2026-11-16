/*
 * apollo_events.h - [Events] the driver's event channel: what changed on a unit, as it happens, instead of polling
 * (the kext's per-client notification ring and SEL165 SetNotificationPort; open-apollo parity "event fan-out").
 * apollo.sys (driver\events.c), apollo-diag (events).
 *
 *   WAIT_EVENTS  in APOLLO_EVENTS_WAIT, out APOLLO_EVENTS. User level. Answers at once with the unit's events newer
 *                than AfterSeq (oldest first, at most APOLLO_EVENTS_MAX; Lost counts the ones the ring no longer holds);
 *                with none, it waits until the next one (or until the handle closes or is cancelled: CancelIoEx), unless
 *                Flags has NOWAIT. Pass the last answer's Seq as the next AfterSeq. An AfterSeq ahead of the unit's Seq
 *                (the driver restarted: sequences begin again at 1) is answered at once with RESET and the whole ring.
 *                A unit that is gone answers STATUS_NO_SUCH_DEVICE once nothing newer is left.
 *
 * Every unit has its own ring of APOLLO_EVENT_RING entries. Function code 0x59 (0x57-0x58 are apollo_params.h's).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include "apollo_ioctl.h"

#define IOCTL_APOLLO_WAIT_EVENTS        APOLLO_CTL(0x59, APOLLO_USER)

#define APOLLO_EVENTS_VERSION           1
#define APOLLO_EVENT_RING               32
#define APOLLO_EVENTS_MAX               16

/* APOLLO_EVENT_ENTRY.Kind and its A / B */
#define APOLLO_EVENT_STAGE              1   /* A = the new stage (APOLLO_INFO.Stage), B = the last status */
#define APOLLO_EVENT_LEAVING            2   /* D0 exit, removal or driver update: close the handle now. A = 1 surprise removal */
#define APOLLO_EVENT_RATE               3   /* a rate change finished: A = the rate in Hz now, B = APOLLO_RATE_ERR_* */
#define APOLLO_EVENT_CLOCK              4   /* a clock source change finished: A = the source (0xC074 id), B = NTSTATUS */
#define APOLLO_EVENT_PANEL              5   /* the front panel changed (knob, a button, a host write's echo): A = the panel
                                               event's Seq (GET_PANEL), B = its field */
#define APOLLO_EVENT_DSP_FAULT          6   /* a DSP stopped answering and is quarantined until a power cycle: A = DSP, B = NTSTATUS */
#define APOLLO_EVENT_ENGINE_LOST        7   /* the audio engine lost the unit (reads all ones or too slow): A dead reads, B slow */
#define APOLLO_EVENT_PARAMS             8   /* a driver Parameter was stored for the next start: A = its index (GET_PARAMS), B = value */

#define APOLLO_EVENTS_WAIT_NOWAIT       0x01

typedef struct _APOLLO_EVENTS_WAIT {
    ULONG   Version;            /* APOLLO_EVENTS_VERSION */
    ULONG   AfterSeq;           /* 0 = everything the ring holds */
    ULONG   Flags;              /* APOLLO_EVENTS_WAIT_* */
    ULONG   Reserved;
} APOLLO_EVENTS_WAIT, *PAPOLLO_EVENTS_WAIT;

typedef struct _APOLLO_EVENT_ENTRY {
    ULONG     Seq;              /* 1, 2, 3, ... per unit and driver start */
    ULONG     Kind;             /* APOLLO_EVENT_* */
    ULONG     A, B;
    LONGLONG  Time;             /* system time (UTC, 100 ns since 1601) */
} APOLLO_EVENT_ENTRY, *PAPOLLO_EVENT_ENTRY;

#define APOLLO_EVENTS_F_RESET           0x01    /* AfterSeq was ahead of Seq: the driver restarted; Entry[] = the whole ring */

typedef struct _APOLLO_EVENTS {
    ULONG   Version;            /* APOLLO_EVENTS_VERSION */
    ULONG   Seq;                /* the unit's newest sequence (pass it as the next AfterSeq) */
    ULONG   Count;              /* entries filled, oldest first */
    ULONG   Lost;               /* events after AfterSeq the ring no longer held */
    ULONG   Flags;              /* APOLLO_EVENTS_F_* */
    ULONG   Reserved;
    APOLLO_EVENT_ENTRY Entry[APOLLO_EVENTS_MAX];
} APOLLO_EVENTS, *PAPOLLO_EVENTS;
