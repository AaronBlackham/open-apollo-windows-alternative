/*
 * params.c - [Params] the per-unit driver Parameters at run time (include\apollo_params.h): GET_PARAMS lists what this
 * start resolved (unit.c ApolloUnitResolve) next to what Units\<unit key> holds now; SET_PARAM writes or removes a
 * value there for the next start. Nothing here changes a value a running start uses: the strip, engine, endpoint and
 * rate code read them once at start (spec-twinx 6.9), and changing one under them is not what they are built for.
 *
 * PASSIVE_LEVEL, no device lock: Ctx->Unit is written once in PrepareHardware before any IOCTL is queued, and the
 * registry calls take their own locks.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "apollo.h"
#include "..\include\apollo_params.h"
#include <ntstrsafe.h>

C_ASSERT(APOLLO_PARAM_COUNT <= APOLLO_PARAMS_MAX);
C_ASSERT(APOLLO_PARAM_SRC_UNIT == APOLLO_SRC_UNIT && APOLLO_PARAM_SRC_MODEL == APOLLO_SRC_MODEL &&
         APOLLO_PARAM_SRC_GLOBAL == APOLLO_SRC_GLOBAL && APOLLO_PARAM_SRC_MODE == APOLLO_SRC_MODE &&
         APOLLO_PARAM_SRC_DEFAULT == APOLLO_SRC_DEFAULT);
C_ASSERT(sizeof(((APOLLO_PARAMS_INFO *)0)->UnitKey) >= APOLLO_UNIT_KEY_CHARS);

BOOLEAN ApolloParamsIsIoctl(_In_ ULONG Code)
{
    return Code == IOCTL_APOLLO_GET_PARAMS || Code == IOCTL_APOLLO_SET_PARAM;
}

/* Units\<key> of this unit, opened for Access; NULL if the unit has no key or it does not exist (and !Create). */
_IRQL_requires_(PASSIVE_LEVEL)
static WDFKEY ParamsOpenUnitKey(_In_ PDEVICE_CONTEXT Ctx, _In_ ACCESS_MASK Access, _In_ BOOLEAN Create)
{
    WDFKEY params = NULL, k = NULL;
    WCHAR path[40];
    UNICODE_STRING n;

    if (Ctx->Ident.UnitKeyKind == APOLLO_UNIT_KEY_NONE ||
        !NT_SUCCESS(RtlStringCbPrintfW(path, sizeof(path), L"Units\\%hs", Ctx->Ident.UnitKey))) {
        return NULL;
    }
    if (!NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), Create ? (KEY_READ | KEY_WRITE) : KEY_READ,
                                                       WDF_NO_OBJECT_ATTRIBUTES, &params))) {
        return NULL;
    }
    RtlInitUnicodeString(&n, path);
    if (Create) {
        if (!NT_SUCCESS(WdfRegistryCreateKey(params, &n, Access, REG_OPTION_NON_VOLATILE, NULL,
                                             WDF_NO_OBJECT_ATTRIBUTES, &k))) {
            k = NULL;
        }
    } else if (!NT_SUCCESS(WdfRegistryOpenKey(params, &n, Access, WDF_NO_OBJECT_ATTRIBUTES, &k))) {
        k = NULL;
    }
    WdfRegistryClose(params);
    return k;
}

_IRQL_requires_(PASSIVE_LEVEL)
static VOID ParamsFill(_In_ PDEVICE_CONTEXT Ctx, _Out_ PAPOLLO_PARAMS_INFO I)
{
    WDFKEY unitKey = ParamsOpenUnitKey(Ctx, KEY_READ, FALSE);
    int i;

    RtlZeroMemory(I, sizeof(*I));
    I->Version = APOLLO_PARAMS_VERSION;
    I->Count = APOLLO_PARAM_COUNT;
    I->Resolved = Ctx->Unit.Resolved ? 1u : 0u;
    if (Ctx->Ident.UnitKeyKind != APOLLO_UNIT_KEY_NONE) {
        (VOID)RtlStringCbCopyA(I->UnitKey, sizeof(I->UnitKey), Ctx->Ident.UnitKey);
    }
    for (i = 0; i < APOLLO_PARAM_COUNT; i++) {
        const APOLLO_PARAM_DEF *d = ApolloParamDef(i);
        PAPOLLO_PARAM_ENTRY e = &I->Entry[i];
        UNICODE_STRING n;
        ULONG v = 0;

        (VOID)RtlStringCbCopyA(e->Name, sizeof(e->Name), d->Name);
        e->Value = ApolloUnitParam(Ctx, i);
        e->Default = Ctx->Unit.Resolved ? ApolloParamDefault(Ctx->Ident.Model, i) : d->DefaultOther;
        e->Source = Ctx->Unit.Resolved ? Ctx->Unit.Source[i] : (UCHAR)APOLLO_SRC_DEFAULT;
        e->Flags = d->Hex ? APOLLO_PARAM_F_HEX : 0;
        if (unitKey != NULL) {
            RtlInitUnicodeString(&n, d->NameW);
            if (NT_SUCCESS(WdfRegistryQueryULong(unitKey, &n, &v))) {
                e->Stored = v;
                e->Flags |= APOLLO_PARAM_F_STORED;
            }
        }
        /* pending: Units\<key> now says something this start does not run (a new value, or one removed) */
        if (((e->Flags & APOLLO_PARAM_F_STORED) && (e->Source != APOLLO_SRC_UNIT || e->Stored != e->Value) &&
             e->Source != APOLLO_SRC_MODE) ||
            (!(e->Flags & APOLLO_PARAM_F_STORED) && e->Source == APOLLO_SRC_UNIT)) {
            e->Flags |= APOLLO_PARAM_F_PENDING;
        }
    }
    if (unitKey != NULL) {
        WdfRegistryClose(unitKey);
    }
}

/* The Parameter called Name (case-insensitive), or -1. */
static int ParamsFind(_In_reads_(Len) const CHAR *Name, _In_ size_t Len)
{
    CHAR n[APOLLO_PARAM_NAME_LEN];
    int i;

    RtlZeroMemory(n, sizeof(n));
    RtlCopyMemory(n, Name, min(Len, sizeof(n) - 1));
    for (i = 0; i < APOLLO_PARAM_COUNT; i++) {
        if (_stricmp(n, ApolloParamDef(i)->Name) == 0) {
            return i;
        }
    }
    return -1;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS ApolloParamsIoctl(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Code, _In_opt_ PVOID In, _In_ size_t InLen,
                           _Out_opt_ PVOID Out, _In_ size_t OutLen, _Out_ PULONG_PTR Info)
{
    PAPOLLO_PARAMS_INFO I = (PAPOLLO_PARAMS_INFO)Out;
    NTSTATUS result = STATUS_SUCCESS;

    *Info = 0;
    if (I == NULL || OutLen < sizeof(*I)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (Code == IOCTL_APOLLO_SET_PARAM) {
        /* METHOD_BUFFERED: In and Out are one buffer; read the request before the answer is written */
        APOLLO_PARAM_SET s;
        WDFKEY k;
        UNICODE_STRING n;
        int id;

        if (In == NULL || InLen < sizeof(s)) {
            return STATUS_INVALID_PARAMETER;
        }
        RtlCopyMemory(&s, In, sizeof(s));
        if (s.Version != APOLLO_PARAMS_VERSION) {
            return STATUS_REVISION_MISMATCH;
        }
        id = ParamsFind(s.Name, sizeof(s.Name));
        if (id < 0) {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        if (Ctx->Ident.UnitKeyKind == APOLLO_UNIT_KEY_NONE) {
            return STATUS_NOT_SUPPORTED;            /* no Units\<key> to write: the model key is the place (by hand) */
        }
        k = ParamsOpenUnitKey(Ctx, KEY_READ | KEY_WRITE, (s.Flags & APOLLO_PARAM_SET_DELETE) == 0);
        RtlInitUnicodeString(&n, ApolloParamDef(id)->NameW);
        if (s.Flags & APOLLO_PARAM_SET_DELETE) {
            result = (k == NULL) ? STATUS_SUCCESS : WdfRegistryRemoveValue(k, &n);
            if (result == STATUS_OBJECT_NAME_NOT_FOUND) {
                result = STATUS_SUCCESS;            /* already gone */
            }
        } else {
            result = (k == NULL) ? STATUS_UNSUCCESSFUL : WdfRegistryAssignULong(k, &n, s.Value);
        }
        if (k != NULL) {
            WdfRegistryClose(k);
        }
        if (s.Flags & APOLLO_PARAM_SET_DELETE) {
            ApolloLogU(Ctx, "params: Units\\%s\\%s removed by a client (0x%08lX): the model value or default from the next start",
                       Ctx->Ident.UnitKey, ApolloParamDef(id)->Name, (ULONG)result);
        } else {
            ApolloLogU(Ctx, "params: Units\\%s\\%s = %lu (0x%lX) by a client (0x%08lX): applies at the next start",
                       Ctx->Ident.UnitKey, ApolloParamDef(id)->Name, s.Value, s.Value, (ULONG)result);
        }
        if (NT_SUCCESS(result)) {
            ApolloEventPost(Ctx, APOLLO_EVENT_PARAMS, (ULONG)id, s.Value);         /* [Events] */
        }
    } else if (Code != IOCTL_APOLLO_GET_PARAMS) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    ParamsFill(Ctx, I);
    I->Result = (ULONG)result;
    *Info = sizeof(*I);
    return STATUS_SUCCESS;
}
