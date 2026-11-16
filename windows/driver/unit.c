/*
 * unit.c - per-unit driver Parameters (docs/spec-twinx.md 6.9, "As built in G0-G1").
 *
 * ApolloUnitResolve runs once per start in PrepareHardware, right after ApolloIdentify and the
 * quiesce, with no lock held (PASSIVE_LEVEL). It picks every value through
 *     Units\<unit key> -> Models\<TT> -> legacy global (x8p only) -> built-in default
 * (apollo_unit.h), applies the Mode rules and logs where each value came from. The values stay
 * fixed until the next start. ApolloUnitParam is the one call the strip, engine and endpoint
 * code use to read them.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "apollo.h"
#include <ntstrsafe.h>

C_ASSERT(APOLLO_DEF_BATCH_X8P == APOLLO_MIXER_BATCH_X8P_SAFE);

/* Log letter per APOLLO_SRC_*: D default, U unit, M model, G legacy global, X forced by Mode */
static const CHAR g_UnitSrcLetter[] = "DUMGX";

static BOOLEAN UnitQuery(_In_opt_ WDFKEY Key, _In_ int Id, _Out_ PULONG Value)
{
    UNICODE_STRING n;

    *Value = 0;
    if (Key == NULL) {
        return FALSE;
    }
    RtlInitUnicodeString(&n, ApolloParamDef(Id)->NameW);
    return NT_SUCCESS(WdfRegistryQueryULong(Key, &n, Value));
}

static WDFKEY UnitOpenSub(_In_ WDFKEY Params, _In_z_ PCWSTR Path)
{
    UNICODE_STRING n;
    WDFKEY k = NULL;

    RtlInitUnicodeString(&n, Path);
    if (!NT_SUCCESS(WdfRegistryOpenKey(Params, &n, KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &k))) {
        k = NULL;
    }
    return k;
}

static VOID UnitLogValues(_In_ PDEVICE_CONTEXT Ctx)
{
    const APOLLO_UNIT *U = &Ctx->Unit;
    CHAR line[200];
    size_t len = 0;
    int i;

    line[0] = '\0';
    for (i = 0; i < APOLLO_PARAM_COUNT; i++) {
        const APOLLO_PARAM_DEF *d = ApolloParamDef(i);
        UCHAR src = U->Source[i];
        CHAR letter = (src < sizeof(g_UnitSrcLetter) - 1) ? g_UnitSrcLetter[src] : '?';

        (VOID)RtlStringCbLengthA(line, sizeof(line), &len);
        (VOID)RtlStringCbPrintfA(line + len, sizeof(line) - len, d->Hex ? " %s=0x%lX/%c" : " %s=%lu/%c",
                                 d->Name, U->Value[i], letter);
        (VOID)RtlStringCbLengthA(line, sizeof(line), &len);
        if (len > 120 || i == APOLLO_PARAM_COUNT - 1) {
            ApolloLogU(Ctx, "unit values:%s", line);
            line[0] = '\0';
        }
    }
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID ApolloUnitResolve(_In_ PDEVICE_CONTEXT Ctx)
{
    APOLLO_UNIT *u = &Ctx->Unit;
    const APOLLO_IDENT *id = &Ctx->Ident;
    BOOLEAN x8p = (id->Model == APOLLO_MODEL_X8P);
    WDFKEY params = NULL, unitKey = NULL, modelKey = NULL;
    WCHAR path[40];
    int i;

    RtlZeroMemory(u, sizeof(*u));
    if (id->Verdict != APOLLO_IDENT_ACCEPTED) {
        return;                         /* refused or not identified: no values, never active */
    }

    if (!NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ,
                                                       WDF_NO_OBJECT_ATTRIBUTES, &params))) {
        params = NULL;
    }
    if (params != NULL && id->UnitKeyKind != APOLLO_UNIT_KEY_NONE &&
        NT_SUCCESS(RtlStringCbPrintfW(path, sizeof(path), L"Units\\%hs", id->UnitKey))) {
        unitKey = UnitOpenSub(params, path);
    }
    if (params != NULL &&
        NT_SUCCESS(RtlStringCbPrintfW(path, sizeof(path), L"Models\\%02lX", id->DeviceType))) {
        modelKey = UnitOpenSub(params, path);
    }
    u->KeyPresent = (unitKey != NULL);
    u->ModelKeyPresent = (modelKey != NULL);

    for (i = 0; i < APOLLO_PARAM_COUNT; i++) {
        APOLLO_PARAM_LEVELS l;
        ULONG v;

        RtlZeroMemory(&l, sizeof(l));
        l.HaveUnit = UnitQuery(unitKey, i, &v);
        l.Unit = v;
        l.HaveModel = UnitQuery(modelKey, i, &v);
        l.Model = v;
        /* The legacy globals are the x8p's: no other model reads them. */
        l.HaveGlobal = x8p && UnitQuery(params, i, &v);
        l.Global = l.HaveGlobal ? v : 0;
        u->Value[i] = ApolloParamPick(id->Model, i, &l, &u->Source[i]);
    }
    ApolloUnitFinish(id->Model, u);

    if (modelKey != NULL) {
        WdfRegistryClose(modelKey);
    }
    if (unitKey != NULL) {
        WdfRegistryClose(unitKey);
    }
    if (params != NULL) {
        WdfRegistryClose(params);
    }

    if (id->UnitKeyKind == APOLLO_UNIT_KEY_NONE) {
        ApolloLogU(Ctx, "unit: no unit key (HW_UID reads 0 or all ones, serial not usable); Models\\%02lX %s; "
                  "legacy globals %s", id->DeviceType, u->ModelKeyPresent ? "present" : "absent",
                  x8p ? "apply (x8p)" : "ignored (not an x8p)");
    } else {
        ApolloLogU(Ctx, "unit: Units\\%s (%s) %s; Models\\%02lX %s; legacy globals %s", id->UnitKey,
                  id->UnitKeyKind == APOLLO_UNIT_KEY_HWUID ? "HW_UID" : "serial",
                  u->KeyPresent ? "present" : "absent", id->DeviceType,
                  u->ModelKeyPresent ? "present" : "absent",
                  x8p ? "apply (x8p)" : "ignored (not an x8p)");
    }
    UnitLogValues(Ctx);

    /* A Mode above what this build runs for the model came down */
    if (u->Source[APOLLO_PARAM_MODE] == APOLLO_SRC_MODE && u->ModeAsked <= APOLLO_MODE_FULL &&
        u->ModeAsked > u->Value[APOLLO_PARAM_MODE]) {
        ApolloLogU(Ctx, "unit Mode %lu asked: this build runs the %s in Mode %lu at most", u->ModeAsked,
                  id->Name, u->Value[APOLLO_PARAM_MODE]);
    }
    switch (u->Value[APOLLO_PARAM_MODE]) {
    case APOLLO_MODE_OBSERVE:
        ApolloLogU(Ctx, "unit Mode 0: observe (identified and quiesced, nothing else)");
        break;
    case APOLLO_MODE_TRANSPORT:
        ApolloLogU(Ctx, "unit Mode 1: bring-up and transport; strips and Windows endpoints off");
        if (!x8p && u->Active) {
            ApolloLogU(Ctx, "unit Mode 1 (gate G3): no ASIO and no settings either; this unit answers info, log, "
                      "engine, position, levels, readback, capture and tone only");
        }
        break;
    default:
        ApolloLogU(Ctx, "unit Mode 2: full");
        if (!x8p && u->Active) {
            /* What Mode 2 means on this model (bringup_plan.h) */
            const APOLLO_BRINGUP_PLAN *plan = ApolloBringupPlanFor(id->Model);

            ApolloLogU(Ctx, "unit Mode 2 (gates G4-G5): Windows endpoints %s (Endpoints 0x%lX), monitor settings; "
                      "ASIO %s, strips %s (StripsEnable %lu), preamp settings %s",
                      (plan != NULL && plan->Endpoints) ? "yes" : "no",
                      u->Value[APOLLO_PARAM_ENABLED_ENDPOINTS], (plan != NULL && plan->Asio) ? "yes" : "no",
                      (plan != NULL && plan->Strips) ? "yes" : "no", u->Value[APOLLO_PARAM_STRIPS_ENABLE],
                      (plan != NULL && plan->InputParams) ? "yes" : "no");
        }
        break;
    }
    if (!u->Active && u->Value[APOLLO_PARAM_MODE] != APOLLO_MODE_OBSERVE) {
        ApolloLogU(Ctx, "the %s bring-up (docs/spec-twinx.md section 2, gate G3) is not in this build: "
                  "observe only", id->Name);
    }
}

ULONG ApolloUnitParam(_In_ PDEVICE_CONTEXT Ctx, _In_ int Id)
{
    if (Id < 0 || Id >= APOLLO_PARAM_COUNT) {
        return 0;
    }
    if (!Ctx->Unit.Resolved) {
        return ApolloParamDef(Id)->DefaultOther;    /* not identified: the safe values */
    }
    return Ctx->Unit.Value[Id];
}

_IRQL_requires_(PASSIVE_LEVEL)
ULONG ApolloUnitParamBeforeIdentify(_In_ PDEVICE_CONTEXT Ctx, _In_ int Id)
{
    WDFKEY params = NULL;
    ULONG v = 0;
    BOOLEAN have = FALSE;

    if (Id < 0 || Id >= APOLLO_PARAM_COUNT) {
        return 0;
    }
    /*
     * Endpoints: every row of the slot's table, whatever the registry holds (apollo_unit.h
     * ApolloAddTimeEndpoints ignores the legacy global; the unit's value narrows it later).
     */
    if (Id == APOLLO_PARAM_ENABLED_ENDPOINTS) {
        return ApolloAddTimeEndpoints(Ctx->HwidSubsysKnown, Ctx->HwidSubsystem, FALSE, 0);
    }
    /* Not the x8p's INF line: the safe model default, no registry (apollo_unit.h). */
    if (!ApolloAddTimeX8pRules(Ctx->HwidSubsysKnown, Ctx->HwidSubsystem)) {
        return ApolloParamDef(Id)->DefaultOther;
    }
    if (NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ,
                                                      WDF_NO_OBJECT_ATTRIBUTES, &params))) {
        have = UnitQuery(params, Id, &v);
        WdfRegistryClose(params);
    }
    return have ? v : ApolloParamDef(Id)->DefaultX8p;
}

/* GET_INFO version 6: APOLLO_UNIT_FLAG_* (apollo_ioctl.h). */
ULONG ApolloUnitInfoFlags(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONG f = 0;

    if (Ctx->Unit.Resolved) {
        f |= APOLLO_UNIT_FLAG_RESOLVED;
    }
    if (Ctx->Unit.Active) {
        f |= APOLLO_UNIT_FLAG_ACTIVE;
    } else {
        f |= APOLLO_UNIT_FLAG_OBSERVE;
    }
    if (Ctx->Ident.Verdict == APOLLO_IDENT_REFUSED || Ctx->Ident.Verdict == APOLLO_IDENT_NOT_APOLLO) {
        f |= APOLLO_UNIT_FLAG_REFUSED;
    }
    if (Ctx->OwnerRead) {
        f |= APOLLO_UNIT_FLAG_OWNER_READ;
    }
    if (Ctx->Unit.KeyPresent) {
        f |= APOLLO_UNIT_FLAG_UNIT_KEY;
    }
    if (Ctx->Unit.ModelKeyPresent) {
        f |= APOLLO_UNIT_FLAG_MODEL_KEY;
    }
    return f;
}
