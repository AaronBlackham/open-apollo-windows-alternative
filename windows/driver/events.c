/*
 * events.c - [Events] the event channel (include\apollo_events.h): a ring of the unit's last APOLLO_EVENT_RING events
 * and WAIT_EVENTS, answered at once when something newer is there and parked in a manual queue until the next event
 * otherwise. A parked request is the framework's to cancel (CancelIoEx, the handle's cleanup); the queue is not power
 * managed, so a parked wait never holds a D0 exit or a removal up, and every exit posts LEAVING first so its clients
 * hear about it and close their handles.
 *
 * ApolloEventPost runs at IRQL <= DISPATCH_LEVEL from any path (the engine's timer included) and may be called with any
 * of the driver's locks held: EventLock is the innermost lock and nothing is taken under it.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "apollo.h"

C_ASSERT(APOLLO_EVENTS_MAX <= APOLLO_EVENT_RING);

NTSTATUS ApolloEventsInit(_In_ PDEVICE_CONTEXT Ctx)
{
    WDF_IO_QUEUE_CONFIG qc;
    WDF_OBJECT_ATTRIBUTES a;
    NTSTATUS status;

    WDF_OBJECT_ATTRIBUTES_INIT(&a);
    a.ParentObject = Ctx->Device;
    status = WdfSpinLockCreate(&a, &Ctx->EventLock);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    WDF_IO_QUEUE_CONFIG_INIT(&qc, WdfIoQueueDispatchManual);
    qc.PowerManaged = WdfFalse;
    return WdfIoQueueCreate(Ctx->Device, &qc, WDF_NO_OBJECT_ATTRIBUTES, &Ctx->EventQueue);
}

BOOLEAN ApolloEventsIsIoctl(_In_ ULONG Code)
{
    return Code == IOCTL_APOLLO_WAIT_EVENTS;
}

/* EventLock held: the events after After into O. FALSE = none (and After is not ahead of the ring). */
static BOOLEAN EventsFill(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG After, _Out_ PAPOLLO_EVENTS O)
{
    ULONG seq = Ctx->EventSeq, oldest, s;

    RtlZeroMemory(O, sizeof(*O));
    O->Version = APOLLO_EVENTS_VERSION;
    O->Seq = seq;
    if (After > seq) {
        O->Flags |= APOLLO_EVENTS_F_RESET;          /* the driver restarted under this client */
        After = 0;
    }
    if (seq == After) {
        return (O->Flags & APOLLO_EVENTS_F_RESET) != 0;
    }
    oldest = (seq > APOLLO_EVENT_RING) ? seq - APOLLO_EVENT_RING + 1 : 1;
    if (After + 1 < oldest) {
        O->Lost = oldest - (After + 1);
        After = oldest - 1;
    }
    for (s = After + 1; s <= seq && O->Count < APOLLO_EVENTS_MAX; s++) {
        O->Entry[O->Count++] = Ctx->EventRing[s % APOLLO_EVENT_RING];
    }
    return TRUE;
}

/* A request's AfterSeq (METHOD_BUFFERED: read before the answer overwrites the buffer), or MAXULONG if malformed. */
static ULONG EventsAfter(_In_ WDFREQUEST Request, _Out_ PULONG Flags)
{
    PAPOLLO_EVENTS_WAIT w = NULL;
    size_t len = 0;

    *Flags = 0;
    if (!NT_SUCCESS(WdfRequestRetrieveInputBuffer(Request, sizeof(*w), (PVOID *)&w, &len)) || w == NULL ||
        w->Version != APOLLO_EVENTS_VERSION) {
        return MAXULONG;
    }
    *Flags = w->Flags;
    return w->AfterSeq;
}

/* Complete one request with what the ring holds after its AfterSeq (it has something, or the unit is gone). */
static VOID EventsAnswer(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFREQUEST Request, _In_ ULONG After)
{
    APOLLO_EVENTS o;
    PVOID out = NULL;
    size_t len = 0;
    BOOLEAN any;

    WdfSpinLockAcquire(Ctx->EventLock);
    any = EventsFill(Ctx, After, &o);
    WdfSpinLockRelease(Ctx->EventLock);
    if (!any && (Ctx->Stage == ApolloStageGone || ApolloGateClosed(Ctx))) {
        WdfRequestComplete(Request, STATUS_NO_SUCH_DEVICE);
        return;
    }
    if (!NT_SUCCESS(WdfRequestRetrieveOutputBuffer(Request, sizeof(o), &out, &len)) || out == NULL || len < sizeof(o)) {
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return;
    }
    RtlCopyMemory(out, &o, sizeof(o));
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(o));
}

/* WAIT_EVENTS from the default queue (PASSIVE_LEVEL): answered now, or parked until the next event. */
VOID ApolloEventsIoctl(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFREQUEST Request)
{
    APOLLO_EVENTS o;
    ULONG flags = 0, after = EventsAfter(Request, &flags);
    BOOLEAN any;
    NTSTATUS status;

    if (after == MAXULONG) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }
    if (Ctx->EventQueue == NULL) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE);
        return;
    }
    /*
     * Check and park under EventLock: a post either comes before (the check sees it) or after (its drain finds the
     * request in the queue). Nothing new and NOWAIT, or a unit that is gone: answered now.
     */
    WdfSpinLockAcquire(Ctx->EventLock);
    any = EventsFill(Ctx, after, &o);
    if (!any && (flags & APOLLO_EVENTS_WAIT_NOWAIT) == 0 && Ctx->Stage != ApolloStageGone && !ApolloGateClosed(Ctx) &&
        ReadAcquire(&Ctx->StopRequested) == 0) {
        status = WdfRequestForwardToIoQueue(Request, Ctx->EventQueue);
        WdfSpinLockRelease(Ctx->EventLock);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
        }
        return;
    }
    WdfSpinLockRelease(Ctx->EventLock);
    EventsAnswer(Ctx, Request, after);
}

/* Wake every parked wait (each gets the events after its own AfterSeq). */
static VOID EventsDrain(_In_ PDEVICE_CONTEXT Ctx)
{
    WDFREQUEST r;
    ULONG flags;

    while (Ctx->EventQueue != NULL && NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Ctx->EventQueue, &r))) {
        EventsAnswer(Ctx, r, EventsAfter(r, &flags));
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID ApolloEventPost(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Kind, _In_ ULONG A, _In_ ULONG B)
{
    PAPOLLO_EVENT_ENTRY e;
    LARGE_INTEGER now;

    if (Ctx == NULL || Ctx->EventLock == NULL) {
        return;
    }
    KeQuerySystemTime(&now);
    WdfSpinLockAcquire(Ctx->EventLock);
    Ctx->EventSeq++;
    if (Ctx->EventSeq == 0) {
        Ctx->EventSeq = 1;                  /* 0 means "everything" to a client: never a sequence */
    }
    e = &Ctx->EventRing[Ctx->EventSeq % APOLLO_EVENT_RING];
    e->Seq = Ctx->EventSeq;
    e->Kind = Kind;
    e->A = A;
    e->B = B;
    e->Time = now.QuadPart;
    WdfSpinLockRelease(Ctx->EventLock);
    EventsDrain(Ctx);
}
