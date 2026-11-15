/*
 * corr.c - speaker correction on the playback ring (include\apollo_corr.h; the engine, crossfades
 * and checks are in corr_dsp.h).
 *
 * Where it runs. Every host source reaches a unit's playback ring through ADDs on the 1 ms engine
 * tick: the unit's own tick (engine.c step 3: the per-unit ASIO client, the unit's standalone
 * Windows streams) and, when the unit is in the combined device (agg.c), the timeline unit's tick
 * (the combined ASIO client and the combined Windows endpoints write the timeline's ring directly,
 * and a follower's ring through the resampler). So the correction runs in place on the corrected
 * slots, frames [Head, bound), at the end of that summing:
 *   - ApolloCorrAfterTick, the last thing the unit's own tick does (after ApolloAggAfterTick): on
 *     the timeline unit and a standalone unit every writer has finished the frames up to the
 *     engine's data head by then;
 *   - ApolloCorrAggFollower, from the timeline's tick right after it rendered a follower's ring:
 *     a follower's frames are final only up to the aggregate's head for it, which the aggregate
 *     passes here (and its own tick then uses as the bound).
 * Off (no profile, disabled, or bypassed and faded out with the filters stopped) both return before
 * they read or write a frame: the render path is the one before speaker correction.
 *
 * Locking: the engine state (CORR_ENGINE) is under the unit's EngineLock, taken at DISPATCH by the
 * tick, or already held by the aggregate (it holds every member's EngineLock in its tick). The
 * PASSIVE side (IOCTLs, the start) serialises on Corr->Lock (a WDFWAITLOCK) and takes EngineLock
 * inside it for the few fields it changes: order Corr->Lock -> EngineLock, and Corr->Lock is never
 * held together with DevLock, StripLock or the aggregate's locks. Nothing here allocates at
 * DISPATCH: the per-unit state (programs, histories, scratch) is one nonpaged block allocated at
 * the first PrepareHardware and freed at device cleanup; a load fills a program slot no voice uses
 * at PASSIVE and then swaps it in under EngineLock.
 *
 * Persisted per unit: Parameters\Units\<unit key>\Correction, Profile (REG_BINARY) and Enabled
 * (DWORD), read and checked again at every PrepareHardware.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "apollo.h"
#include "..\include\apollo_corr.h"
#include "corr_dsp.h"
#include <ntstrsafe.h>

#define CORR_LOCK_TIMEOUT_MS    3000

typedef struct _APOLLO_CORR {
    CORR_ENGINE         E;              /* EngineLock */
    volatile LONG       Live;           /* CorrRunning(&E), written under EngineLock: the tick's fast path */
    ULONG               LastNs;         /* EngineLock */
    ULONG               MaxNs;
    LONGLONG            QpcFreq;
    WDFWAITLOCK         Lock;           /* the PASSIVE side */
    APOLLO_CORR_PROFILE Active;         /* Lock: the profile in force (Size 0 = none) */
    BOOLEAN             Stored;
    NTSTATUS            StoreStatus;
    ULONG               LoadedFrom;     /* APOLLO_CORR_FROM_* */
} APOLLO_CORR, *PAPOLLO_CORR;

static const UNICODE_STRING g_CorrProfileName = RTL_CONSTANT_STRING(L"Profile");
static const UNICODE_STRING g_CorrEnabledName = RTL_CONSTANT_STRING(L"Enabled");

/* ------------------------------------------------------------------ */
/* The tick (DISPATCH_LEVEL)                                           */
/* ------------------------------------------------------------------ */

/* EngineLock held. */
_IRQL_requires_(DISPATCH_LEVEL)
static VOID CorrPumpUnit(_In_ PDEVICE_CONTEXT Ctx, _Inout_ PAPOLLO_CORR C)
{
    LARGE_INTEGER q0, q1;
    unsigned n;

    q0 = KeQueryPerformanceCounter(NULL);
    n = CorrPump(&C->E, (long *)Ctx->Play.Va, Ctx->RingFrames, Ctx->PlayChannels, Ctx->SampleRate, Ctx->EngPos,
                 Ctx->EngDataHead, Ctx->EngStarts, Ctx->EngTicks);
    if (n != 0 && C->QpcFreq > 0) {
        LONGLONG d;

        q1 = KeQueryPerformanceCounter(NULL);
        d = (q1.QuadPart - q0.QuadPart) * 1000000000LL / C->QpcFreq;
        C->LastNs = (d < 0) ? 0 : (d > (LONGLONG)MAXULONG ? MAXULONG : (ULONG)d);
        if (C->LastNs > C->MaxNs) {
            C->MaxNs = C->LastNs;
        }
    }
    WriteNoFence(&C->Live, CorrRunning(&C->E) ? 1 : 0);
}

/* engine.c, the last step of the unit's tick: no lock held. */
_Use_decl_annotations_
VOID ApolloCorrAfterTick(PDEVICE_CONTEXT Ctx)
{
    PAPOLLO_CORR c = Ctx->Corr;

    if (c == NULL || ReadNoFence(&c->Live) == 0) {
        return;                         /* off: nothing read, nothing written */
    }
    KeAcquireSpinLockAtDpcLevel(&Ctx->EngineLock);
    CorrPumpUnit(Ctx, c);
    KeReleaseSpinLockFromDpcLevel(&Ctx->EngineLock);
}

/* agg.c, the timeline's tick: Ctx (a member) has its EngineLock held by the aggregate. */
_Use_decl_annotations_
VOID ApolloCorrAggFollower(PDEVICE_CONTEXT Ctx, ULONGLONG RenderHead)
{
    PAPOLLO_CORR c = Ctx->Corr;

    if (c == NULL) {
        return;
    }
    CorrAggBound(&c->E, RenderHead, Ctx->EngTicks);
    if (ReadNoFence(&c->Live) != 0) {
        CorrPumpUnit(Ctx, c);
    }
}

/* agg.c: a writer (re)starting on Ctx's ring at Head; Ctx's EngineLock held. Head itself while off. */
_Use_decl_annotations_
ULONGLONG ApolloCorrWriteFloor(PDEVICE_CONTEXT Ctx, ULONGLONG Head)
{
    PAPOLLO_CORR c = Ctx->Corr;

    return (c == NULL) ? Head : CorrWriteFloor(&c->E, Head);
}

/* ------------------------------------------------------------------ */
/* Registry (PASSIVE)                                                  */
/* ------------------------------------------------------------------ */

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS CorrOpenKey(_In_ PDEVICE_CONTEXT Ctx, _In_ BOOLEAN Write, _Out_ WDFKEY *Key)
{
    ACCESS_MASK access = Write ? (KEY_READ | KEY_WRITE) : KEY_READ;
    WDFKEY params = NULL, units = NULL, unit = NULL;
    UNICODE_STRING n;
    WCHAR path[64];
    NTSTATUS status;

    *Key = NULL;
    if (Ctx->Ident.UnitKeyKind == APOLLO_UNIT_KEY_NONE || Ctx->Ident.UnitKey[0] == 0) {
        return STATUS_OBJECT_NAME_NOT_FOUND;    /* no unit key: nothing is kept for this unit */
    }
    status = WdfDriverOpenParametersRegistryKey(WdfGetDriver(), access, WDF_NO_OBJECT_ATTRIBUTES, &params);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (!Write) {
        status = RtlStringCbPrintfW(path, sizeof(path), L"Units\\%hs\\Correction", Ctx->Ident.UnitKey);
        if (NT_SUCCESS(status)) {
            RtlInitUnicodeString(&n, path);
            status = WdfRegistryOpenKey(params, &n, KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, Key);
        }
        WdfRegistryClose(params);
        return status;
    }
    /* Units, Units\<key>, Units\<key>\Correction: one level at a time */
    RtlInitUnicodeString(&n, L"Units");
    status = WdfRegistryCreateKey(params, &n, access, REG_OPTION_NON_VOLATILE, NULL, WDF_NO_OBJECT_ATTRIBUTES, &units);
    if (NT_SUCCESS(status)) {
        status = RtlStringCbPrintfW(path, sizeof(path), L"%hs", Ctx->Ident.UnitKey);
        if (NT_SUCCESS(status)) {
            RtlInitUnicodeString(&n, path);
            status = WdfRegistryCreateKey(units, &n, access, REG_OPTION_NON_VOLATILE, NULL, WDF_NO_OBJECT_ATTRIBUTES,
                                          &unit);
        }
        WdfRegistryClose(units);
    }
    if (NT_SUCCESS(status)) {
        RtlInitUnicodeString(&n, L"Correction");
        status = WdfRegistryCreateKey(unit, &n, access, REG_OPTION_NON_VOLATILE, NULL, WDF_NO_OBJECT_ATTRIBUTES, Key);
        WdfRegistryClose(unit);
    }
    WdfRegistryClose(params);
    return status;
}

/* Profile (NULL: delete it) and Enabled. Corr->Lock held. */
_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS CorrStore(_In_ PDEVICE_CONTEXT Ctx, _In_opt_ const APOLLO_CORR_PROFILE *P, _In_ BOOLEAN Profile,
                          _In_ ULONG Enabled)
{
    WDFKEY key = NULL;
    NTSTATUS status = CorrOpenKey(Ctx, TRUE, &key);

    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (Profile) {
        if (P != NULL) {
            status = WdfRegistryAssignValue(key, &g_CorrProfileName, REG_BINARY, sizeof(*P), (PVOID)P);
        } else {
            status = WdfRegistryRemoveValue(key, &g_CorrProfileName);
            if (status == STATUS_OBJECT_NAME_NOT_FOUND) {
                status = STATUS_SUCCESS;
            }
        }
    }
    if (NT_SUCCESS(status)) {
        status = WdfRegistryAssignULong(key, &g_CorrEnabledName, Enabled);
    }
    WdfRegistryClose(key);
    return status;
}

/* ------------------------------------------------------------------ */
/* Install (PASSIVE, Corr->Lock held)                                  */
/* ------------------------------------------------------------------ */

/* A checked profile goes in: fill a free program at PASSIVE, swap it in under EngineLock. *Enabled: after it. */
_IRQL_requires_(PASSIVE_LEVEL)
static BOOLEAN CorrInstallProfile(_In_ PDEVICE_CONTEXT Ctx, _Inout_ PAPOLLO_CORR C, _In_ const APOLLO_CORR_PROFILE *P,
                                  _In_ BOOLEAN Enable, _Out_ PULONG Enabled)
{
    KIRQL irql;
    int idx;

    *Enabled = 0;
    KeAcquireSpinLock(&Ctx->EngineLock, &irql);
    idx = CorrReserve(&C->E);
    KeReleaseSpinLock(&Ctx->EngineLock, irql);
    if (idx < 0) {
        return FALSE;
    }
    CorrProgramFill(&C->E.Prog[idx], P);    /* no voice uses it: no lock */
    KeAcquireSpinLock(&Ctx->EngineLock, &irql);
    CorrInstall(&C->E, idx, Enable ? 1 : 0);
    *Enabled = C->E.Enabled ? 1 : 0;
    WriteNoFence(&C->Live, CorrRunning(&C->E) ? 1 : 0);
    KeReleaseSpinLock(&Ctx->EngineLock, irql);
    RtlCopyMemory(&C->Active, P, sizeof(*P));
    return TRUE;
}

static PCSTR CorrErrText(_In_ ULONG E)
{
    switch (E) {
    case APOLLO_CORR_ERR_SIZE:          return "size or version";
    case APOLLO_CORR_ERR_RATE:          return "sample rate";
    case APOLLO_CORR_ERR_CHANNELS:      return "channel count";
    case APOLLO_CORR_ERR_FILTERS:       return "filter count";
    case APOLLO_CORR_ERR_NOT_FINITE:    return "a number is not finite";
    case APOLLO_CORR_ERR_UNSTABLE:      return "unstable filter";
    case APOLLO_CORR_ERR_GAIN:          return "gain above 0 dB or below -60 dB";
    case APOLLO_CORR_ERR_DELAY:         return "delay too long";
    case APOLLO_CORR_ERR_SLOT:          return "playback slot";
    case APOLLO_CORR_ERR_CLIP:          return "could clip after the preamp";
    case APOLLO_CORR_ERR_RATE_MISMATCH: return "coefficients for another rate";
    case APOLLO_CORR_ERR_TEXT:          return "text not terminated";
    case APOLLO_CORR_ERR_DC:            return "gain below 5 Hz or at Nyquist";
    case APOLLO_CORR_ERR_NARROW:        return "a resonance narrower than the format allows";
    default:                            return "refused";
    }
}

/* The stored profile, at the start (PrepareHardware, after ApolloUnitResolve). */
_IRQL_requires_(PASSIVE_LEVEL)
static VOID CorrLoadStored(_In_ PDEVICE_CONTEXT Ctx, _Inout_ PAPOLLO_CORR C)
{
    APOLLO_CORR_PROFILE *p;
    WDFKEY key = NULL;
    ULONG got = 0, type = 0, enabled = 0, err, ch = 0, flt = 0, now = 0;
    NTSTATUS status;

    status = CorrOpenKey(Ctx, FALSE, &key);
    if (!NT_SUCCESS(status)) {
        return;                         /* nothing stored (or no unit key) */
    }
    p = (APOLLO_CORR_PROFILE *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*p), APOLLO_POOL_TAG);
    if (p == NULL) {
        WdfRegistryClose(key);
        return;
    }
    status = WdfRegistryQueryValue(key, &g_CorrProfileName, sizeof(*p), p, &got, &type);
    if (!NT_SUCCESS(WdfRegistryQueryULong(key, &g_CorrEnabledName, &enabled))) {
        enabled = 0;
    }
    WdfRegistryClose(key);
    if (!NT_SUCCESS(status) || type != REG_BINARY || got != sizeof(*p)) {
        if (NT_SUCCESS(status)) {
            ApolloLogU(Ctx, "correction: stored profile ignored (type %lu, %lu bytes)", type, got);
        }
        ExFreePoolWithTag(p, APOLLO_POOL_TAG);
        return;
    }
    err = CorrValidate(p, 0, &ch, &flt);
    if (err != APOLLO_CORR_ERR_NONE) {
        ApolloLogU(Ctx, "correction: stored profile refused: %s (check %lu, channel %lu, filter %lu)", CorrErrText(err),
                   err, ch + 1, flt + 1);
    } else if (CorrInstallProfile(Ctx, C, p, enabled != 0, &now)) {
        C->Stored = TRUE;
        C->LoadedFrom = APOLLO_CORR_FROM_REGISTRY;
        ApolloLogU(Ctx, "correction: stored profile \"%s\" loaded (%lu channel(s) at %lu Hz, %s)", p->Name,
                   p->ChannelCount, p->Rate, enabled ? "enabled" : "disabled");
    }
    ExFreePoolWithTag(p, APOLLO_POOL_TAG);
}

/* ------------------------------------------------------------------ */
/* Start and teardown (PASSIVE)                                        */
/* ------------------------------------------------------------------ */

/* device.c PrepareHardware, a unit this driver runs, no lock held: the state, then the stored profile. */
_Use_decl_annotations_
VOID ApolloCorrPrepareHardware(PDEVICE_CONTEXT Ctx)
{
    PAPOLLO_CORR c = Ctx->Corr;
    LARGE_INTEGER freq;
    KIRQL irql;

    if (c == NULL) {
        WDF_OBJECT_ATTRIBUTES attrs;

        c = (PAPOLLO_CORR)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*c), APOLLO_POOL_TAG);
        if (c == NULL) {
            ApolloLogU(Ctx, "correction: no memory for the per-unit state (%Iu bytes): off on this unit", sizeof(*c));
            return;
        }
        WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
        attrs.ParentObject = Ctx->Device;
        if (!NT_SUCCESS(WdfWaitLockCreate(&attrs, &c->Lock))) {
            ExFreePoolWithTag(c, APOLLO_POOL_TAG);
            ApolloLogU(Ctx, "correction: no lock: off on this unit");
            return;
        }
        CorrEngineInit(&c->E);
        (VOID)KeQueryPerformanceCounter(&freq);
        c->QpcFreq = freq.QuadPart;
        Ctx->Corr = c;                  /* before the engine can start (D0 entry) */
    }
    WdfWaitLockAcquire(c->Lock, NULL);
    /* a new start: what ran before is gone (the engine is not running yet) */
    KeAcquireSpinLock(&Ctx->EngineLock, &irql);
    CorrEngineInit(&c->E);
    c->LastNs = c->MaxNs = 0;
    WriteNoFence(&c->Live, 0);
    KeReleaseSpinLock(&Ctx->EngineLock, irql);
    RtlZeroMemory(&c->Active, sizeof(c->Active));
    c->Stored = FALSE;
    c->StoreStatus = STATUS_SUCCESS;
    c->LoadedFrom = APOLLO_CORR_FROM_NONE;
    CorrLoadStored(Ctx, c);
    WdfWaitLockRelease(c->Lock);
}

/* driver.c device cleanup, after the engine stopped and the unit left the combined device. */
_Use_decl_annotations_
VOID ApolloCorrFree(PDEVICE_CONTEXT Ctx)
{
    PAPOLLO_CORR c = Ctx->Corr;

    if (c != NULL) {
        Ctx->Corr = NULL;
        ExFreePoolWithTag(c, APOLLO_POOL_TAG);     /* the lock is the device's child: WDF deletes it */
    }
}

/* ------------------------------------------------------------------ */
/* IOCTLs (PASSIVE, no lock held)                                      */
/* ------------------------------------------------------------------ */

BOOLEAN ApolloCorrIsIoctl(_In_ ULONG Code)
{
    return Code == IOCTL_APOLLO_CORR_GET_STATE || Code == IOCTL_APOLLO_CORR_LOAD || Code == IOCTL_APOLLO_CORR_SET;
}

/* [Rate] rate.c GET_RATE: a profile for another rate than the unit's is loaded (it does not run). */
BOOLEAN ApolloCorrRateMismatch(_In_ PDEVICE_CONTEXT Ctx)
{
    PAPOLLO_CORR c = Ctx->Corr;
    BOOLEAN r = FALSE;
    KIRQL irql;

    if (c == NULL) {
        return FALSE;
    }
    KeAcquireSpinLock(&Ctx->EngineLock, &irql);
    if (c->E.RateMismatch || (c->E.Cur >= 0 && Ctx->SampleRate != 0 && c->E.Prog[c->E.Cur].Rate != Ctx->SampleRate)) {
        r = TRUE;
    }
    KeReleaseSpinLock(&Ctx->EngineLock, irql);
    return r;
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS CorrLock(_In_ PAPOLLO_CORR C)
{
    LONGLONG t = -10000LL * CORR_LOCK_TIMEOUT_MS;

    return (WdfWaitLockAcquire(C->Lock, &t) == STATUS_TIMEOUT) ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
static VOID CorrFillState(_In_ PDEVICE_CONTEXT Ctx, _In_opt_ PAPOLLO_CORR C, _Out_ APOLLO_CORR_STATE *S, _In_ BOOLEAN Reset)
{
    KIRQL irql;

    RtlZeroMemory(S, sizeof(*S));
    S->Version = APOLLO_CORR_VERSION;
    S->UnitRate = Ctx->SampleRate;
    S->SwapPermille = 1000;
    if (!ApolloEngineRunning(Ctx)) {
        S->Flags |= APOLLO_CORR_F_UNIT_IDLE;
    }
    if (C == NULL) {
        return;
    }
    RtlCopyMemory(&S->Profile, &C->Active, sizeof(S->Profile));
    S->StoreStatus = C->StoreStatus;
    S->LoadedFrom = C->LoadedFrom;
    if (C->Stored) {
        S->Flags |= APOLLO_CORR_F_STORED;
    }
    KeAcquireSpinLock(&Ctx->EngineLock, &irql);
    {
        const CORR_ENGINE *e = &C->E;

        if (e->Cur >= 0) {
            S->Flags |= APOLLO_CORR_F_LOADED;
        }
        if (e->Enabled) {
            S->Flags |= APOLLO_CORR_F_ENABLED;
        }
        if (e->Bypass) {
            S->Flags |= APOLLO_CORR_F_BYPASS;
        }
        if (CorrRunning(e)) {
            S->Flags |= APOLLO_CORR_F_RUNNING;
        }
        if (e->RateMismatch || (e->Cur >= 0 && Ctx->SampleRate != 0 && e->Prog[e->Cur].Rate != Ctx->SampleRate)) {
            S->Flags |= APOLLO_CORR_F_RATE_MISMATCH;
        }
        if (e->SlotSkipped) {
            S->Flags |= APOLLO_CORR_F_SLOT_RANGE;
        }
        S->WetPermille = (ULONG)((ULONGLONG)e->W * 1000u / (e->FadeLen != 0 ? e->FadeLen : 1));
        if (e->Next >= 0) {
            S->SwapPermille = (ULONG)((ULONGLONG)e->X * 1000u / (e->FadeLen != 0 ? e->FadeLen : 1));
        }
        S->Runs = e->Runs;
        S->Frames = e->Frames;
        S->LateFrames = e->Late;
        S->Head = e->Head;
    }
    S->LastNs = C->LastNs;
    S->MaxNs = C->MaxNs;
    if (Reset) {
        C->MaxNs = 0;
    }
    KeReleaseSpinLock(&Ctx->EngineLock, irql);
}

_IRQL_requires_(PASSIVE_LEVEL)
static ULONG CorrLoad(_In_ PDEVICE_CONTEXT Ctx, _Inout_ PAPOLLO_CORR C, _In_ const APOLLO_CORR_PROFILE *P,
                      _Inout_ APOLLO_CORR_RESULT *R)
{
    ULONG err, ch = 0, flt = 0, enabled = 0;

    err = CorrValidate(P, Ctx->PlayChannels, &ch, &flt);
    if (err == APOLLO_CORR_ERR_NONE && Ctx->SampleRate != 0 && P->Rate != Ctx->SampleRate) {
        err = APOLLO_CORR_ERR_RATE_MISMATCH;
    }
    R->Channel = ch;
    R->Filter = flt;
    if (err != APOLLO_CORR_ERR_NONE) {
        ApolloLogU(Ctx, "correction: profile refused: %s (check %lu, channel %lu, filter %lu)", CorrErrText(err), err,
                   ch + 1, flt + 1);
        return err;
    }
    if (!CorrInstallProfile(Ctx, C, P, (P->Flags & APOLLO_CORR_LOAD_ENABLE) != 0, &enabled)) {
        return APOLLO_CORR_ERR_BUSY;
    }
    C->LoadedFrom = APOLLO_CORR_FROM_IOCTL;
    C->Stored = FALSE;
    if ((P->Flags & APOLLO_CORR_LOAD_NO_STORE) == 0) {
        C->StoreStatus = CorrStore(Ctx, P, TRUE, enabled);
        C->Stored = NT_SUCCESS(C->StoreStatus);
        R->Stored = C->Stored ? 1 : 0;
        R->StoreStatus = C->StoreStatus;
    }
    ApolloLogU(Ctx, "correction: profile \"%s\" loaded (%lu channel(s), slots %lu/%lu, %lu Hz, %s; stored 0x%08x)",
               P->Name, P->ChannelCount, P->Channel[0].Slot, P->ChannelCount > 1 ? P->Channel[1].Slot : P->Channel[0].Slot,
               P->Rate, enabled ? "enabled" : "disabled", R->StoreStatus);
    return APOLLO_CORR_ERR_NONE;
}

_IRQL_requires_(PASSIVE_LEVEL)
static ULONG CorrSet(_In_ PDEVICE_CONTEXT Ctx, _Inout_ PAPOLLO_CORR C, _In_ const APOLLO_CORR_SET *S,
                     _Inout_ APOLLO_CORR_RESULT *R)
{
    ULONG err = APOLLO_CORR_ERR_NONE;
    BOOLEAN ticking = ApolloEngineRunning(Ctx);
    KIRQL irql;

    if (S->Op == APOLLO_CORR_OP_ENABLE && S->Value != 0 && C->Active.Size == 0) {
        return APOLLO_CORR_ERR_NO_PROFILE;  /* none loaded, or cleared: only a load brings one back */
    }
    KeAcquireSpinLock(&Ctx->EngineLock, &irql);
    switch (S->Op) {
    case APOLLO_CORR_OP_ENABLE:
        err = CorrSetEnabled(&C->E, S->Value != 0);
        break;
    case APOLLO_CORR_OP_BYPASS:
        CorrSetBypass(&C->E, S->Value != 0);
        break;
    case APOLLO_CORR_OP_CLEAR:
        CorrClear(&C->E);
        if (!ticking && C->E.Unload) {
            CorrStop(&C->E);            /* no tick to fade it out: forget it now */
        }
        break;
    default:
        err = APOLLO_CORR_ERR_OP;
        break;
    }
    WriteNoFence(&C->Live, CorrRunning(&C->E) ? 1 : 0);
    KeReleaseSpinLock(&Ctx->EngineLock, irql);
    if (err != APOLLO_CORR_ERR_NONE) {
        return err;
    }
    if (S->Op == APOLLO_CORR_OP_ENABLE && C->Active.Size != 0) {
        C->StoreStatus = CorrStore(Ctx, NULL, FALSE, S->Value != 0 ? 1 : 0);
        R->Stored = NT_SUCCESS(C->StoreStatus) ? 1 : 0;
        R->StoreStatus = C->StoreStatus;
    } else if (S->Op == APOLLO_CORR_OP_CLEAR) {
        RtlZeroMemory(&C->Active, sizeof(C->Active));
        C->Stored = FALSE;
        C->LoadedFrom = APOLLO_CORR_FROM_NONE;
        C->StoreStatus = CorrStore(Ctx, NULL, TRUE, 0);
        if (C->StoreStatus == STATUS_OBJECT_NAME_NOT_FOUND) {
            C->StoreStatus = STATUS_SUCCESS;    /* no unit key: nothing was stored */
        }
        R->StoreStatus = C->StoreStatus;
    }
    ApolloLogU(Ctx, "correction: %s %lu", S->Op == APOLLO_CORR_OP_ENABLE ? "enable" :
               (S->Op == APOLLO_CORR_OP_BYPASS ? "bypass (A/B)" : "clear"), S->Value);
    return APOLLO_CORR_ERR_NONE;
}

_Use_decl_annotations_
NTSTATUS ApolloCorrIoctl(PDEVICE_CONTEXT Ctx, ULONG Code, PVOID In, size_t InLen, PVOID Out, size_t OutLen, ULONG_PTR *Info)
{
    PAPOLLO_CORR c = Ctx->Corr;
    APOLLO_CORR_RESULT res;
    NTSTATUS status;

    *Info = 0;
    if (Code == IOCTL_APOLLO_CORR_GET_STATE) {
        APOLLO_CORR_STATE *st;
        BOOLEAN reset = (In != NULL && InLen >= sizeof(ULONG) && (*(PULONG)In & APOLLO_CORR_STATE_RESET_COST) != 0);

        if (Out == NULL || OutLen < sizeof(APOLLO_CORR_STATE)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        /* METHOD_BUFFERED: In and Out share the buffer, so the reset bit was read first; fill a copy */
        st = (APOLLO_CORR_STATE *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*st), APOLLO_POOL_TAG);
        if (st == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        if (c != NULL) {
            status = CorrLock(c);
            if (!NT_SUCCESS(status)) {
                ExFreePoolWithTag(st, APOLLO_POOL_TAG);
                return status;
            }
        }
        CorrFillState(Ctx, c, st, reset);
        if (c != NULL) {
            WdfWaitLockRelease(c->Lock);
        }
        RtlCopyMemory(Out, st, sizeof(*st));
        ExFreePoolWithTag(st, APOLLO_POOL_TAG);
        *Info = sizeof(APOLLO_CORR_STATE);
        return STATUS_SUCCESS;
    }

    if (Out == NULL || OutLen < sizeof(APOLLO_CORR_RESULT)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlZeroMemory(&res, sizeof(res));
    if (Code == IOCTL_APOLLO_CORR_LOAD) {
        APOLLO_CORR_PROFILE *p;

        if (In == NULL || InLen < sizeof(APOLLO_CORR_PROFILE)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        /* the input, copied before the result overwrites the shared buffer */
        p = (APOLLO_CORR_PROFILE *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*p), APOLLO_POOL_TAG);
        if (p == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlCopyMemory(p, In, sizeof(*p));
        if (c == NULL || !Ctx->Unit.Active) {
            res.Error = APOLLO_CORR_ERR_UNIT;
        } else {
            status = CorrLock(c);
            if (!NT_SUCCESS(status)) {
                ExFreePoolWithTag(p, APOLLO_POOL_TAG);
                return status;
            }
            res.Error = CorrLoad(Ctx, c, p, &res);
            WdfWaitLockRelease(c->Lock);
        }
        ExFreePoolWithTag(p, APOLLO_POOL_TAG);
    } else {
        APOLLO_CORR_SET s;

        if (In == NULL || InLen < sizeof(s)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        s = *(const APOLLO_CORR_SET *)In;
        if (c == NULL || !Ctx->Unit.Active) {
            res.Error = APOLLO_CORR_ERR_UNIT;
        } else {
            status = CorrLock(c);
            if (!NT_SUCCESS(status)) {
                return status;
            }
            res.Error = CorrSet(Ctx, c, &s, &res);
            WdfWaitLockRelease(c->Lock);
        }
    }
    RtlCopyMemory(Out, &res, sizeof(res));
    *Info = sizeof(res);
    return STATUS_SUCCESS;              /* a refusal is res.Error (METHOD_BUFFERED brings nothing back on an error) */
}
