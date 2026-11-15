/*
 * strips.c - the Console Mic Input Strips on the x8p (docs/STRIPS.md), as far as test T3:
 *
 *   - ownership: the strip DSP's boot word, read before our ring connect (STRIPS.md 2.3);
 *   - the pool-layout check against the recipe (2.4);
 *   - the module loader: five files from an admin-only directory, each re-verified against
 *     the pins in apollo_strip_pins.h (length, header, w4, SHA-256). No IOCTL carries module
 *     bytes, and no other file or id is ever read (4.3);
 *   - the transaction engine on the DSP rings (section 3);
 *   - the five code-module uploads, each acknowledged {0x80070004, 0, id, x} (2.5);
 *   - the strips of StripMask (2.6, 2.7, 10.8; words from strip_plan.h): one PREPARE
 *     transaction per preamp (its memory), then one START transaction that starts them all,
 *     then the bus-owner table and the route words to the other DSPs (2.8), the 48V resync
 *     (5.6), each stage gated by StripSteps so a hardware run can be bisected (STRIP_STEP_*);
 *   - at every D0 exit: SYNCH stops for the strips that were started, then the release
 *     {0x00030002, id, 0, 0} of every module we sent (2.9);
 *   - the mixer-init batch choice (MixerInitBatch, section 6).
 *
 * [Twin X G5] The same strips on the Apollo Twin X Gen 1 (docs/spec-twinx.md 5.2, 6.9.5): its plan
 * row (strip_plan.h StripModelFor: inputs 1-2, DSP 1, room for 2 strips, the same five modules
 * and words) and the bring-up plan's Strips flag decide; the x8p row is the pre-G5 behaviour.
 * StripsEnable=2 logs the whole pass the unit would get, per input, and sends nothing.
 *
 * [Warm] Warm starts (docs/STRIPS.md 12, strip_adopt.h). A D0 exit on a model whose warm start
 * keeps the DSPs' state (the x8p) leaves Live strips running (StripAdopt), and the next warm start
 * adopts them without sending anything when this unit's record proves them this driver's: same
 * unit and firmware, only this driver on the DSPs since the last power-up (anchored), boot words
 * unchanged since its last connect, the same pools, and the same plan word for word. A record
 * that cannot prove that leaves the old rule: a power cycle. The Twin X's warm start ring-
 * connects, which drops the strips, so with the same proof it builds them again
 * (StripWarmRebuild). The record lives under Parameters\Units\<unit key>\StripRecord.
 * Adopted or rebuilt strips whose record saw no plug-in left take plug-ins again (StripWarmPlugins,
 * docs/STRIPS.md 12.5); the first acknowledged upload proves the warm session (WarmUploads).
 *
 * Ported from open-apollo (GPL-2.0): ua_dsp_xact.c, ua_plugin_upload.c, ua_plugin_place.c,
 * ua_strips.c, ua_plugin_host.c. Citations in docs/STRIPS.md.
 *
 * Locking: StripLock serialises passes and the quiesce; it is taken BEFORE DevLock and never
 * while DevLock is held. DevLock covers every transaction slot, ring index and strip field, in
 * short sections only. Nothing waits for the DSP while holding DevLock: the 10 ms service
 * thread, the bring-up and the IOCTLs need it.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "apollo.h"
#include <bcrypt.h>
#include <ntstrsafe.h>
#include <stdarg.h>
#include "apollo_strip_pins.h"
#include "strip_plan.h"
#include "strip_adopt.h"

/* [Warm] the record and the GET_STRIPS version 3 codes are the same numbers (strip_adopt.h, apollo_ioctl.h) */
C_ASSERT(STRIP_REC_DSPS == APOLLO_MAX_DSPS && STRIP_REC_INPUTS == APOLLO_STRIP_INPUTS &&
         STRIP_REC_MODULES == APOLLO_STRIP_MODULES);
C_ASSERT(STRIP_REC_NONE == APOLLO_STRIPS_REC_NONE && STRIP_REC_RUNNING == APOLLO_STRIPS_REC_RUNNING &&
         STRIP_REC_UNKNOWN == APOLLO_STRIPS_REC_UNKNOWN);
C_ASSERT(STRIP_ADOPT_NONE == APOLLO_STRIPS_ADOPT_NONE && STRIP_ADOPT_ADOPTED == APOLLO_STRIPS_ADOPT_ADOPTED &&
         STRIP_ADOPT_RESTORE == APOLLO_STRIPS_ADOPT_RESTORE && STRIP_ADOPT_REBUILD == APOLLO_STRIPS_ADOPT_REBUILD &&
         STRIP_ADOPT_NOT_ANCHORED == APOLLO_STRIPS_ADOPT_NOT_ANCHORED &&
         STRIP_ADOPT_NOT_RUNNING == APOLLO_STRIPS_ADOPT_NOT_RUNNING &&
         STRIP_ADOPT_UNKNOWN_STATE == APOLLO_STRIPS_ADOPT_UNKNOWN_STATE &&
         STRIP_ADOPT_TORN_DOWN == APOLLO_STRIPS_ADOPT_TORN_DOWN &&
         STRIP_ADOPT_PLAN_DIFFERS == APOLLO_STRIPS_ADOPT_PLAN_DIFFERS &&
         STRIP_ADOPT_POOLS_DIFFER == APOLLO_STRIPS_ADOPT_POOLS_DIFFER &&
         STRIP_ADOPT_DISABLED == APOLLO_STRIPS_ADOPT_DISABLED && STRIP_ADOPT_DROPPED == APOLLO_STRIPS_ADOPT_DROPPED &&
         STRIP_ADOPT_LAYOUT == APOLLO_STRIPS_ADOPT_LAYOUT && STRIP_ADOPT_COUNT == 13);
C_ASSERT(STRIP_ANCHOR_OK == APOLLO_STRIPS_ANCHOR_OK && STRIP_ANCHOR_COLD_USED == APOLLO_STRIPS_ANCHOR_COLD_USED &&
         STRIP_ANCHOR_NO_RECORD == APOLLO_STRIPS_ANCHOR_NO_RECORD &&
         STRIP_ANCHOR_BAD_RECORD == APOLLO_STRIPS_ANCHOR_BAD_RECORD &&
         STRIP_ANCHOR_OTHER_UNIT == APOLLO_STRIPS_ANCHOR_OTHER_UNIT &&
         STRIP_ANCHOR_LOST_BEFORE == APOLLO_STRIPS_ANCHOR_LOST_BEFORE &&
         STRIP_ANCHOR_OTHER_HOST == APOLLO_STRIPS_ANCHOR_OTHER_HOST &&
         STRIP_ANCHOR_NO_ADVANCE == APOLLO_STRIPS_ANCHOR_NO_ADVANCE &&
         STRIP_ANCHOR_NOT_READ == APOLLO_STRIPS_ANCHOR_NOT_READ);
C_ASSERT(sizeof(STRIP_RECORD) == 50 * sizeof(ULONG));

C_ASSERT(RTL_FIELD_SIZE(APOLLO_STRIPS, Blob) == APOLLO_STRIP_MODULES_BYTES);
C_ASSERT(APOLLO_STRIP_MODULES == APOLLO_STRIP_MODULE_SLOTS);
C_ASSERT(sizeof(STRIP_PLAN) <= RTL_FIELD_SIZE(APOLLO_STRIPS, PlanScratch));
C_ASSERT(STRIP_PLAN_INPUTS == APOLLO_STRIP_INPUTS);
C_ASSERT(STRIP_PLAN_ENTS_MAX <= XACT_ENTS_MAX);
C_ASSERT(STRIP_PLAN_PREPARE_ENTS <= STRIP_PLAN_ENTS_MAX);
C_ASSERT(STRIP_PLAN_PAYLOAD_MAX * 4 <= XACT_PAGE);
/* A pass holds every PREPARE buffer until the START is taken, plus the START itself. */
C_ASSERT(STRIP_PLAN_INPUTS + 1 < XACT_SLOTS);

/*
 * The layout check's constants: the values the recipe was validated on (x4 A6.1, and the
 * x8p's DSP 1 on Nov 14). The planner (strip_plan.h) builds addresses from its own macros;
 * these asserts keep the two from drifting apart. [Twin X G5] The check itself is
 * strip_plan.h StripPlanLayoutCheck now (the same code, run offline by strip-plan-test).
 */
#define LAYOUT_TOP1             0x0009D1B8u
#define LAYOUT_TOP2             0x000C6FFEu
#define LAYOUT_TOP3             0x08FFE000u
#define LAYOUT_PER_STRIP1       0x54u
#define LAYOUT_PER_STRIP2       0x10u
#define LAYOUT_PER_STRIP3       0x2010u
C_ASSERT(LAYOUT_TOP1 == STRIP_TOP1 && LAYOUT_TOP2 == STRIP_TOP2 && LAYOUT_TOP3 == STRIP_TOP3);
C_ASSERT(LAYOUT_PER_STRIP1 == STRIP_POOL1_PER_STRIP);
C_ASSERT(LAYOUT_PER_STRIP2 == STRIP_POOL2_PER_STRIP);
C_ASSERT(LAYOUT_PER_STRIP3 == STRIP_POOL3_PER_STRIP);
/* [Twin X G5] the per-model plan (strip_plan.h) names the models and limits the driver's */
C_ASSERT(STRIP_MODEL_X8P == APOLLO_MODEL_X8P && STRIP_MODEL_TWINX == APOLLO_MODEL_TWINX);
C_ASSERT(STRIP_MODULE_IDS == APOLLO_STRIP_MODULES);
C_ASSERT(STRIP_DSP_ROUTE_MAX == APOLLO_MAX_DSPS);
C_ASSERT(APOLLO_STRIP_CODE_BASE == 0x000E0000u);

#define STRIP_PHANTOM_OFF_MS    200u            /* ua_core.c:2459 */
#define STRIP_OFF_COMMIT_MS     500u            /* mailbox commit wait for the 48V off */
#define STRIP_ON_COMMIT_MS      2000u           /* ... and for the on: keep flushing this long */
#define STRIP_READBACK_WAIT_MS  500u            /* for a second, matching readback snapshot */
#define STRIP_PHANTOM_RUNNING   0x80000000u     /* internal PhantomState: off written, on pending */

#define STRIP_CMD_RELEASE_CODE  0x00030002u     /* {cmd, id, 0, 0}, no reply */
#define STRIP_CMD_UPLOAD_CODE   0x00010000u     /* | (record dwords + 2), carried by DMA */
#define STRIP_REPLY_UPLOAD      0x80070004u
#define STRIP_WAIT_MS           1000u           /* ua_plugin_host.h:133 */
#define STRIP_PUBLISH_MS        50u             /* ua_plugin_host.h:134 */
#define STRIP_SETTLE_MS         500u            /* ua_strips.c:760 */
#define STRIP_RATE_HZ           48000u     /* [Rate] the captured rate; the gate takes any rate StripRampOf knows */
#define STRIP_MAX_DSP           3u              /* open-apollo clamps input_strips_dsp below 4: the x8p
                                                   plan's DspAllowed 0x0F (strip_plan.h, [Twin X G5]) */
#define STRIP_LAYOUT_UNREAD     0xFFFFFFFFu
#define STRIP_RX_WORDS_MAX      64u

#define MS_TO_100NS(ms)         ((ULONGLONG)(ms) * 10000ULL)

#define POLL_DONE               1
#define POLL_PENDING            0
#define POLL_GONE               (-1)
#define POLL_STALE              (-2)

/* ------------------------------------------------------------------ */
/* State helpers (DevLock held)                                        */
/* ------------------------------------------------------------------ */

static VOID StripsSetWhy(_Inout_ APOLLO_STRIPS *S, _In_z_ _Printf_format_string_ PCSTR Fmt, ...)
{
    va_list ap;

    va_start(ap, Fmt);
    (VOID)RtlStringCbVPrintfA(S->Why, sizeof(S->Why), Fmt, ap);
    va_end(ap);
}

static VOID StripsOff(_Inout_ APOLLO_STRIPS *S, _In_z_ PCSTR Why)
{
    S->State = APOLLO_STRIPS_STATE_OFF;
    StripsSetWhy(S, "%s", Why);
    ApolloLogU(CONTAINING_RECORD(S, DEVICE_CONTEXT, Strips), "strips: off: %s", Why);
}

_IRQL_requires_(PASSIVE_LEVEL)
static VOID StripsRecLost(_In_ PDEVICE_CONTEXT Ctx, _In_z_ PCSTR Why);     /* [Warm] below */

_IRQL_requires_(PASSIVE_LEVEL)
static VOID StripsQuarantine(_In_ PDEVICE_CONTEXT Ctx, _In_z_ PCSTR What, _In_ NTSTATUS Status)
{
    APOLLO_STRIPS *s = &Ctx->Strips;

    if (!s->Quarantined) {
        ApolloLogU(Ctx, "strips: DSP %lu quarantined until the Apollo is power-cycled: %s (0x%08x)",
                  s->Dsp, What, Status);
        StripsRecLost(Ctx, "quarantine");      /* [Warm] no adoption or warm rebuild until a power cycle */
    }
    s->Quarantined = TRUE;
    s->State = APOLLO_STRIPS_STATE_FAILED;
    StripsSetWhy(s, "DSP %lu quarantined: %s (0x%08x); power-cycle the Apollo", s->Dsp, What,
                 Status);
}

static VOID StripsForgetModules(_Inout_ APOLLO_STRIPS *S)
{
    RtlZeroMemory(S->ModState, sizeof(S->ModState));
}

/* Nothing of ours runs any more (or it is not ours to stop): strip state and bus owners back. */
static VOID StripsForgetStrips(_Inout_ APOLLO_STRIPS *S)
{
    RtlZeroMemory(S->StripState, sizeof(S->StripState));
    RtlZeroMemory(S->StripStarted, sizeof(S->StripStarted));
    RtlZeroMemory(S->BusOwner, sizeof(S->BusOwner));    /* every bus back on DSP 0 */
}

static ULONG StripsStarted(_In_ const APOLLO_STRIPS *S)
{
    ULONG i, n = 0;

    for (i = 0; i < APOLLO_STRIP_INPUTS; i++) {
        if (S->StripStarted[i]) {
            n++;
        }
    }
    return n;
}

static PCSTR StripsStepName(_In_ ULONG Steps)
{
    switch (Steps) {
    case STRIP_STEP_UPLOADS: return "uploads only";
    case STRIP_STEP_ZERO:    return "+ zero-fills";
    case STRIP_STEP_MEMSPEC: return "+ memspec";
    case STRIP_STEP_HEADER:  return "+ header";
    case STRIP_STEP_PARAMS:  return "+ parameters";
    case STRIP_STEP_SYNCH:   return "+ SYNCH (strip started)";
    case STRIP_STEP_INPUT:   return "+ span/route words on the strip DSP";
    case STRIP_STEP_OTHERS:  return "+ bus owner and routes on the other DSPs";
    case STRIP_STEP_PHANTOM: return "+ 48V resync (everything)";
    default:                 return "?";
    }
}

/* Registry StripSteps -> the stage this pass goes to (0 = everything). */
static ULONG StripsStepLimit(_In_ ULONG Steps)
{
    return (Steps == 0 || Steps > STRIP_STEP_ALL) ? STRIP_STEP_ALL : Steps;
}

static ULONG StripsModulesSent(_In_ const APOLLO_STRIPS *S)
{
    ULONG j, n = 0;

    for (j = 0; j < APOLLO_STRIP_MODULES; j++) {
        if (S->ModState[j] != APOLLO_STRIP_MOD_NONE) {
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Registry and transaction buffers (PrepareHardware/ReleaseHardware)  */
/* ------------------------------------------------------------------ */

/* [Twin X G5] This unit's strip plan (strip_plan.h), NULL for a model without one. */
static const STRIP_MODEL *StripsModel(_In_ PDEVICE_CONTEXT Ctx)
{
    return (Ctx->Ident.Model > 0) ? StripModelFor((ULONG)Ctx->Ident.Model) : NULL;
}

/* [Twin X G5] The modules the model's plan names are exactly the pinned ones, in load order. */
static BOOLEAN StripsModulesArePinned(_In_ const STRIP_MODEL *Model)
{
    ULONG j;

    for (j = 0; j < APOLLO_STRIP_MODULES; j++) {
        if (Model->ModuleId[j] != g_ApolloStripPins[j].Id) {
            return FALSE;
        }
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* [Warm] The adoption record (strip_adopt.h, docs/STRIPS.md 12)       */
/* ------------------------------------------------------------------ */

#define STRIP_FILETIME_2007     128120832000000000ULL   /* 2007-01-01 00:00 UTC, FILETIME units */
#define STRIP_ADOPT_WAITS       20u     /* settle periods (500 ms each) a warm start waits for a valid readback */

static const UNICODE_STRING g_StripRecName = RTL_CONSTANT_STRING(STRIP_REC_VALUE_NAME);

/* The model's warm start ring-connects (the Twin X Gen 1): whatever ran there is dropped, nothing to adopt. */
static BOOLEAN StripsConnectsOnWarm(_In_ PDEVICE_CONTEXT Ctx)
{
    const APOLLO_BRINGUP_PLAN *plan = ApolloPlanOf(Ctx);

    return (plan != NULL && plan->AudioExtension && plan->RingConnectAtConnect) ? TRUE : FALSE;
}

/* A unit that runs (Mode 1-2), with a strip plan and a unit key to keep the record under. */
static BOOLEAN StripsRecordsKept(_In_ PDEVICE_CONTEXT Ctx)
{
    return Ctx->Unit.Active && StripsModel(Ctx) != NULL && Ctx->Ident.UnitKeyKind != APOLLO_UNIT_KEY_NONE &&
           Ctx->Ident.UnitKey[0] != 0;
}

static VOID StripsUnitId(_In_ PDEVICE_CONTEXT Ctx, _Out_ STRIP_UNIT_ID *U)
{
    U->HwUidLo = Ctx->HwUidLo;
    U->HwUidHi = Ctx->HwUidHi;
    U->DeviceType = Ctx->Ident.DeviceType;
    U->FpgaRev = Ctx->FpgaRev;
    U->FwRelease = Ctx->FwRelease;
    U->NumDsps = min(Ctx->NumDsps, (ULONG)APOLLO_MAX_DSPS);
}

/* The five pins (ids, places, lengths, SHA-256): strips built from other module bytes are another plan. */
static ULONG StripsModulesHash(VOID)
{
    ULONG h = STRIP_FNV_INIT, j, k;

    for (j = 0; j < APOLLO_STRIP_MODULES; j++) {
        const APOLLO_STRIP_PIN *p = &g_ApolloStripPins[j];

        h = StripFnv(h, p->Id);
        h = StripFnv(h, p->Length);
        h = StripFnv(h, p->W4);
        h = StripFnv(h, p->PoolWords);
        h = StripFnv(h, p->Dest);
        for (k = 0; k + 3 < sizeof(p->Sha256); k += 4) {
            h = StripFnv(h, (ULONG)p->Sha256[k] | ((ULONG)p->Sha256[k + 1] << 8) |
                            ((ULONG)p->Sha256[k + 2] << 16) | ((ULONG)p->Sha256[k + 3] << 24));
        }
    }
    return h;
}

/* (days since 2007-01-01 << 12) + minute of day, UTC, as the connect date; 0 before 2007. */
static ULONG StripsDateNow(VOID)
{
    LARGE_INTEGER now;
    ULONGLONG secs;

    KeQuerySystemTimePrecise(&now);
    if ((ULONGLONG)now.QuadPart < STRIP_FILETIME_2007) {
        return 0;
    }
    secs = ((ULONGLONG)now.QuadPart - STRIP_FILETIME_2007) / 10000000ULL;
    return (ULONG)(((secs / 86400ULL) << 12) + ((secs % 86400ULL) / 60ULL));
}

/* A record date as text, "2026-11-15 09:41 UTC". */
static VOID StripsDateText(_In_ ULONG Date, _Out_writes_z_(Chars) PCHAR Out, _In_ size_t Chars)
{
    LARGE_INTEGER t;
    TIME_FIELDS f;

    if (Date == 0) {
        (VOID)RtlStringCchCopyA(Out, Chars, "an unknown time");
        return;
    }
    t.QuadPart = (LONGLONG)(STRIP_FILETIME_2007 +
                            ((ULONGLONG)(Date >> 12) * 86400ULL + (ULONGLONG)(Date & 0xFFFu) * 60ULL) * 10000000ULL);
    RtlTimeToTimeFields(&t, &f);
    (VOID)RtlStringCchPrintfA(Out, Chars, "%04lu-%02lu-%02lu %02lu:%02lu UTC", (ULONG)f.Year, (ULONG)f.Month,
                              (ULONG)f.Day, (ULONG)f.Hour, (ULONG)f.Minute);
}

static PCSTR StripsRecStateText(_In_ ULONG State)
{
    return State == STRIP_REC_RUNNING ? "running" : State == STRIP_REC_NONE ? "none" : "unknown";
}

/* This unit's Units\<key>, opened to read, or created to write. PASSIVE; DevLock may be held. */
_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS StripsRecKey(_In_ PDEVICE_CONTEXT Ctx, _In_ BOOLEAN Write, _Out_ WDFKEY *Key)
{
    WDFKEY params = NULL, units = NULL;
    UNICODE_STRING n;
    WCHAR name[48];
    ACCESS_MASK access = Write ? (KEY_READ | KEY_WRITE) : KEY_READ;
    NTSTATUS status;

    *Key = NULL;
    status = RtlStringCbPrintfW(name, sizeof(name), L"%hs", Ctx->Ident.UnitKey);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = WdfDriverOpenParametersRegistryKey(WdfGetDriver(), access, WDF_NO_OBJECT_ATTRIBUTES, &params);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    RtlInitUnicodeString(&n, L"Units");
    if (Write) {
        status = WdfRegistryCreateKey(params, &n, access, REG_OPTION_NON_VOLATILE, NULL, WDF_NO_OBJECT_ATTRIBUTES,
                                      &units);
    } else {
        status = WdfRegistryOpenKey(params, &n, access, WDF_NO_OBJECT_ATTRIBUTES, &units);
    }
    if (NT_SUCCESS(status)) {
        RtlInitUnicodeString(&n, name);
        if (Write) {
            status = WdfRegistryCreateKey(units, &n, access, REG_OPTION_NON_VOLATILE, NULL, WDF_NO_OBJECT_ATTRIBUTES,
                                          Key);
        } else {
            status = WdfRegistryOpenKey(units, &n, access, WDF_NO_OBJECT_ATTRIBUTES, Key);
        }
        WdfRegistryClose(units);
    }
    WdfRegistryClose(params);
    if (!NT_SUCCESS(status)) {
        *Key = NULL;
    }
    return status;
}

/*
 * Bring-up, DevLock held, PASSIVE: the record this unit's last start left, into s->Rec. RecFound =
 * valid and this unit's (the same HW_UID, type, FPGA, firmware and DSP count); RecWhy says why not.
 */
_IRQL_requires_(PASSIVE_LEVEL)
static VOID StripsRecLoad(_In_ PDEVICE_CONTEXT Ctx)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    STRIP_UNIT_ID u;
    WDFKEY key = NULL;
    ULONG got = 0, type = 0;
    NTSTATUS status;

    s->RecFound = FALSE;
    s->RecHave = FALSE;
    RtlZeroMemory(&s->Rec, sizeof(s->Rec));
    RtlZeroMemory(&s->Cur, sizeof(s->Cur));
    if (!StripsRecordsKept(Ctx)) {
        s->RecWhy = STRIP_ANCHOR_NOT_READ;
        s->RecStatus = STATUS_NOT_SUPPORTED;
        return;
    }
    status = StripsRecKey(Ctx, FALSE, &key);
    if (NT_SUCCESS(status)) {
        status = WdfRegistryQueryValue(key, &g_StripRecName, sizeof(s->Rec), &s->Rec, &got, &type);
        WdfRegistryClose(key);
    }
    s->RecStatus = status;
    StripsUnitId(Ctx, &u);
    if (status == STATUS_OBJECT_NAME_NOT_FOUND || status == STATUS_OBJECT_PATH_NOT_FOUND) {
        s->RecWhy = STRIP_ANCHOR_NO_RECORD;
    } else if (!NT_SUCCESS(status) || type != REG_BINARY || !StripRecordValid(&s->Rec, got)) {
        s->RecWhy = STRIP_ANCHOR_BAD_RECORD;
    } else if (!StripRecordSameUnit(&s->Rec, &u)) {
        s->RecWhy = STRIP_ANCHOR_OTHER_UNIT;
    } else {
        s->RecWhy = STRIP_ANCHOR_OK;
        s->RecFound = TRUE;
        s->RecHave = TRUE;
        s->Cur = s->Rec;
    }
    if (!s->RecFound) {
        RtlZeroMemory(&s->Rec, sizeof(s->Rec));
    }
}

/*
 * DevLock held, PASSIVE: s->Cur, with this unit's identity, sealed into the registry. RecHave = the
 * registry holds Cur. A D0 exit keeps strips only while it does (ApolloStripsExitKeep).
 */
_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS StripsRecWrite(_In_ PDEVICE_CONTEXT Ctx, _In_z_ PCSTR Why)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    STRIP_UNIT_ID u;
    WDFKEY key = NULL;
    NTSTATUS status;

    if (!StripsRecordsKept(Ctx)) {
        return STATUS_NOT_SUPPORTED;
    }
    StripsUnitId(Ctx, &u);
    s->Cur.HwUidLo = u.HwUidLo;
    s->Cur.HwUidHi = u.HwUidHi;
    s->Cur.DeviceType = u.DeviceType;
    s->Cur.FpgaRev = u.FpgaRev;
    s->Cur.FwRelease = u.FwRelease;
    s->Cur.NumDsps = u.NumDsps;
    s->Cur.Writes++;
    StripRecordSeal(&s->Cur);
    status = StripsRecKey(Ctx, TRUE, &key);
    if (NT_SUCCESS(status)) {
        status = WdfRegistryAssignValue(key, &g_StripRecName, REG_BINARY, sizeof(s->Cur), &s->Cur);
        WdfRegistryClose(key);
    }
    s->RecStatus = status;
    s->RecHave = NT_SUCCESS(status) ? TRUE : FALSE;
    if (NT_SUCCESS(status)) {
        ApolloLogU(Ctx, "strips: record (%s): %s, strips %s%s, StripMask 0x%02lx, %lu plug-in(s)%s, write %lu",
                  Why, s->Cur.Anchored ? "anchored" : "NOT anchored", StripsRecStateText(s->Cur.State),
                  (s->Cur.State == STRIP_REC_RUNNING && s->Cur.Keep) ? " (kept at exit)" : "", s->Cur.Mask,
                  s->Cur.Plugins & ~STRIP_REC_PLUGINS_STALE,
                  (s->Cur.Plugins & STRIP_REC_PLUGINS_STALE) ? " (stale: plug-ins after a power cycle)" : "", s->Cur.Writes);
    } else {
        ApolloLogU(Ctx, "strips: record NOT written (%s): 0x%08x; the strips are torn down at the next D0 exit",
                  Why, status);
    }
    return status;
}

/* DevLock held: from here nothing is known about what the strip DSP holds (taint, quarantine, unproven step). */
_IRQL_requires_(PASSIVE_LEVEL)
static VOID StripsRecLost(_In_ PDEVICE_CONTEXT Ctx, _In_z_ PCSTR Why)
{
    APOLLO_STRIPS *s = &Ctx->Strips;

    if (!StripsRecordsKept(Ctx) || (s->RecHave && !s->Cur.Anchored && s->Cur.State == STRIP_REC_UNKNOWN)) {
        return;
    }
    s->Cur.Anchored = 0;
    s->Cur.State = STRIP_REC_UNKNOWN;
    s->Cur.Keep = 0;
    (VOID)StripsRecWrite(Ctx, Why);
}

/* DevLock held: Cur's strips part = State, with the plan of Mask / Limit at this rate on this DSP, these pools. */
static VOID StripsRecSetPlan(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG State, _In_ ULONG Mask, _In_ ULONG Limit,
                             _In_ ULONG PlanHash)
{
    APOLLO_STRIPS *s = &Ctx->Strips;

    s->Cur.State = State;
    s->Cur.Dsp = s->Dsp;
    s->Cur.Mask = Mask;
    s->Cur.Rate = Ctx->SampleRate;
    s->Cur.Limit = Limit;
    s->Cur.ModuleHash = StripsModulesHash();
    s->Cur.PlanHash = PlanHash;
    StripPlanHeaders(Mask, s->Cur.Header);
    RtlCopyMemory(s->Cur.Geom, s->Geom, sizeof(s->Cur.Geom));
}

/*
 * Bring-up complete, DevLock held, PASSIVE: this start's anchor (strip_adopt.h StripAnchor) and the
 * record's boot words, read now, after any ring connect this start sent (x8p cold, every Twin X
 * start). Written only when something changed: an x8p warm start that finds its record writes nothing.
 */
_IRQL_requires_(PASSIVE_LEVEL)
static VOID StripsRecRefresh(_In_ PDEVICE_CONTEXT Ctx)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    ULONG n = min(Ctx->NumDsps, (ULONG)APOLLO_MAX_DSPS), d;
    BOOLEAN cold = Ctx->WarmStart ? FALSE : TRUE, connected = Ctx->DspsConnected;
    STRIP_RECORD next;

    RtlZeroMemory(s->PostBoot, sizeof(s->PostBoot));
    if (!StripsRecordsKept(Ctx) || !s->BootWordsRead) {
        s->Anchor = STRIP_ANCHOR_NOT_READ;
        return;
    }
    for (d = 0; d < n; d++) {
        s->PostBoot[d] = ApolloRead(Ctx, ApolloDspWindow(d) + DSP_WIN_READY);
    }
    s->Anchor = StripAnchor(cold, connected, s->BootWord, s->PostBoot, n, s->RecFound ? &s->Rec : NULL,
                            s->RecWhy);
    if (s->Anchor == STRIP_ANCHOR_OK && (s->Tainted || s->Quarantined)) {
        s->Anchor = STRIP_ANCHOR_LOST_BEFORE;
    }

    /* cold: nothing of ours; warm without a record: unknown; the Twin X's warm connect dropped them */
    StripRecordAfterBringup(cold, connected, s->RecFound ? &s->Rec : NULL, s->Anchor, s->PostBoot, n, &next);
    ApolloLogU(Ctx, "strips: %s start, %s: %s; boot words now %08x %08x %08x %08x %08x %08x%s", cold ? "cold" : "warm",
              s->Anchor == STRIP_ANCHOR_OK ? "anchored" : "NOT anchored", StripAnchorText(s->Anchor),
              s->PostBoot[0], s->PostBoot[1], s->PostBoot[2], s->PostBoot[3], s->PostBoot[4], s->PostBoot[5],
              connected ? " (after this start's ring connect)" : " (no ring connect at this start)");
    if (!cold && s->RecWhy != STRIP_ANCHOR_NO_RECORD && s->RecWhy != STRIP_ANCHOR_NOT_READ) {
        ApolloLogU(Ctx, "strips: the record found had boot words %08x %08x %08x %08x %08x %08x, strips %s (%s)",
                  s->Rec.BootWord[0], s->Rec.BootWord[1], s->Rec.BootWord[2], s->Rec.BootWord[3],
                  s->Rec.BootWord[4], s->Rec.BootWord[5], StripsRecStateText(s->Rec.State),
                  s->RecFound ? "valid, this unit" : StripAnchorText(s->RecWhy));
    }
    if (s->RecFound && StripRecordSameContent(&next, &s->Rec)) {
        s->Cur = s->Rec;                                    /* nothing changed: nothing written */
        s->RecHave = TRUE;
        return;
    }
    s->Cur = next;
    (VOID)StripsRecWrite(Ctx, cold ? "cold start" : "warm start");
}

/*
 * Plugins placed on the strip DSP (plugin.c, DevLock held): the record keeps the count, so a start that
 * finds a chain may have been left on a strip puts the plain strip back before adopting it.
 */
_Use_decl_annotations_
VOID ApolloStripsRecordPlugins(PDEVICE_CONTEXT Ctx, ULONG Placed)
{
    APOLLO_STRIPS *s = &Ctx->Strips;

    Placed |= s->Cur.Plugins & STRIP_REC_PLUGINS_STALE;    /* [Warm] only a power cycle clears the stale mark */
    if (!StripsRecordsKept(Ctx) || !s->RecHave || s->Cur.Plugins == Placed) {
        return;
    }
    s->Cur.Plugins = Placed;
    (VOID)StripsRecWrite(Ctx, Placed ? "a plug-in placed on the strip DSP" : "no plug-in left on the strip DSP");
}

/* plugin.c, DevLock held: a plug-in quarantine of the strip DSP. */
_Use_decl_annotations_
VOID ApolloStripsRecordLost(PDEVICE_CONTEXT Ctx, PCSTR Why)
{
    StripsRecLost(Ctx, Why);
}

/*
 * [Warm] plugin.c, DevLock held: the strip DSP acknowledged an upload. At a warm start that adopted the strips without
 * a ring connect (the x8p) this is the proof that its session takes uploads with replies (docs/STRIPS.md 12.5).
 */
_Use_decl_annotations_
VOID ApolloStripsUploadProven(PDEVICE_CONTEXT Ctx)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    ULONG d = s->Dsp < APOLLO_MAX_DSPS ? s->Dsp : 0;

    if (s->WarmUploads != STRIPS_WARM_UPLOADS_UNPROVEN) {
        return;
    }
    s->WarmUploads = STRIPS_WARM_UPLOADS_PROVEN;
    ApolloLogU(Ctx, "strips: DSP %lu acknowledged the first upload of this warm start (boot word %08x: no ring connect since "
               "this driver's last session): plug-ins on the adopted strips as after a cold start", d, s->PostBoot[d]);
}

VOID ApolloStripsPrepareHardware(_In_ PDEVICE_CONTEXT Ctx)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    const STRIP_MODEL *model = StripsModel(Ctx);
    NTSTATUS status = STATUS_SUCCESS;
    ULONG i, off = 0, askedMask, askedDsp;

    /*
     * [Twin X G0] This unit's values, read once per start (unit.c: Units\<unit> -> Models\<type>
     * -> the legacy Services\apollo\Parameters values, x8p only -> built-in defaults; Mode < 2
     * forces StripsEnable 0). MixerInitBatch absent on the x8p = x8p-safe (1): the legacy batch's
     * non-preamp settings unchanged, the preamp fields skipped, so the unit keeps its own gains,
     * polarity and 48V (legacy forced ~41 dB on 1 and 5, inverted 5 and cleared its 48V).
     * Section 10.8.
     */
    s->Enable = ApolloUnitParam(Ctx, APOLLO_PARAM_STRIPS_ENABLE);
    s->Steps = ApolloUnitParam(Ctx, APOLLO_PARAM_STRIP_STEPS);
    /* [Twin X G5] mask and DSP as the model's strip plan takes them (x8p: & 0xFF, DSP 0-3, as before) */
    askedMask = ApolloUnitParam(Ctx, APOLLO_PARAM_STRIP_MASK);
    askedDsp = ApolloUnitParam(Ctx, APOLLO_PARAM_STRIP_DSP);
    s->Mask = StripModelMask(model, askedMask);
    s->Dsp = StripModelDsp(model, askedDsp, Ctx->NumDsps);
    s->MixerBatch = ApolloUnitParam(Ctx, APOLLO_PARAM_MIXER_INIT_BATCH);
    /*
     * StripProbe, diagnostics only (docs/STRIPS.md 10.9): 2 = measure + repair ladder, 1 = measure
     * and log. Default 0 (off): on the x8p an input with nothing plugged in reads exact zero by
     * design (the unit mutes empty jacks; confirmed Nov 14 by moving a mic from 3-4 to 1-2 live).
     */
    s->Probe = ApolloUnitParam(Ctx, APOLLO_PARAM_STRIP_PROBE);
    /* [Warm] docs/STRIPS.md 12: keep Live strips at the D0 exit and adopt them warm; build warm when proven ours */
    s->AdoptParam = ApolloUnitParam(Ctx, APOLLO_PARAM_STRIP_ADOPT) ? 1u : 0u;
    s->WarmRebuild = ApolloUnitParam(Ctx, APOLLO_PARAM_STRIP_WARM_REBUILD) ? 1u : 0u;
    s->WarmPlugins = ApolloUnitParam(Ctx, APOLLO_PARAM_STRIP_WARM_PLUGINS) ? 1u : 0u;     /* [Warm] 12.5 */
    if (s->Enable > 2) {
        s->Enable = 0;
    }
    {
        /*
         * [Twin X G3] strips only on a model whose plan has them; Mode < 2 forced 0 already.
         * [Twin X G5] the x8p and the Twin X Gen 1, each with its strip plan row (strip_plan.h).
         */
        const APOLLO_BRINGUP_PLAN *plan = ApolloPlanOf(Ctx);

        if (plan == NULL || !plan->Strips || model == NULL) {
            s->Enable = 0;
        } else if (s->Enable != 0 && !StripsModulesArePinned(model)) {
            ApolloLogU(Ctx, "strips: the %s plan names other modules than the pinned ones: strips off", model->Name);
            s->Enable = 0;
        }
    }
    if (s->Enable != 0 && model != NULL) {
        if ((askedMask & 0xFF) != s->Mask) {
            ApolloLogU(Ctx, "strips: StripMask 0x%02lx names inputs the %s does not have: 0x%02lx is used",
                      askedMask & 0xFF, model->Name, s->Mask);
        }
        if (askedDsp != s->Dsp) {
            ApolloLogU(Ctx, "strips: StripDsp %lu is not a strip DSP of the %s (%lu DSPs): DSP %lu is used",
                      askedDsp, model->Name, Ctx->NumDsps, s->Dsp);
        }
    }
    if (s->Probe > STRIP_PROBE_REPAIR) {
        s->Probe = STRIP_PROBE_REPAIR;
    }
    if (s->MixerBatch > APOLLO_MIXER_BATCH_LEGACY) {
        s->MixerBatch = APOLLO_MIXER_BATCH_LEGACY;
    }

    for (i = 0; i < APOLLO_STRIP_MODULES; i++) {
        s->ModOffset[i] = off;
        off += g_ApolloStripPins[i].Length;
        if (!s->ModulesOk) {
            s->ModFile[i] = STATUS_PENDING;
        }
    }
    s->LayoutWhy = STRIP_LAYOUT_UNREAD;
    s->State = APOLLO_STRIPS_STATE_OFF;
    StripsSetWhy(s, "%s", s->Enable ? "waiting for a cold bring-up" : "StripsEnable is 0");

    /* Transaction buffers: only for a build that may send (StripsEnable=1). */
    if (s->Enable == 1) {
        for (i = 0; i < XACT_SLOTS; i++) {
            PAPOLLO_XACT x = &s->Xact[i];
            PHYSICAL_ADDRESS la;

            if (x->Buffer != NULL) {
                continue;
            }
            status = WdfCommonBufferCreate(Ctx->DmaEnabler, XACT_BYTES, WDF_NO_OBJECT_ATTRIBUTES,
                                           &x->Buffer);
            if (!NT_SUCCESS(status)) {
                x->Buffer = NULL;
                break;
            }
            x->Va = (PUCHAR)WdfCommonBufferGetAlignedVirtualAddress(x->Buffer);
            la = WdfCommonBufferGetAlignedLogicalAddress(x->Buffer);
            x->La = (ULONGLONG)la.QuadPart;
            if ((x->La & (XACT_PAGE - 1)) != 0 || x->La + XACT_BYTES > 0x100000000ULL) {
                WdfObjectDelete(x->Buffer);
                x->Buffer = NULL;
                x->Va = NULL;
                status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
            RtlZeroMemory(x->Va, XACT_BYTES);
            x->State = XactFree;
        }
        s->PoolReady = NT_SUCCESS(status);
        if (!s->PoolReady) {
            ApolloLogU(Ctx, "strips: transaction buffers not allocated (0x%08x)", status);
        }
    }
    ApolloLogU(Ctx, "strips: StripsEnable=%lu StripSteps=%lu StripMask=0x%02lx StripDsp=%lu "
              "MixerInitBatch=%lu StripProbe=%lu%s", s->Enable, s->Steps, s->Mask, s->Dsp,
              s->MixerBatch, s->Probe,
              s->Enable == 0 ? " (off)" : s->Enable == 2 ? " (dry run: nothing is sent)" : "");
    if (s->Enable == 1) {
        ApolloLogU(Ctx, "strips: stage %lu: %s", StripsStepLimit(s->Steps),
                  StripsStepName(StripsStepLimit(s->Steps)));
    }
    if (s->Enable != 0) {
        ApolloLogU(Ctx, "strips: warm starts: StripAdopt=%lu (%s), StripWarmRebuild=%lu (%s), StripWarmPlugins=%lu (%s)%s",
                  s->AdoptParam,
                  StripsConnectsOnWarm(Ctx) ? "nothing to adopt: this model's warm start ring-connects" :
                  s->AdoptParam ? "a D0 exit leaves Live strips running, a warm start adopts its own" :
                  "torn down at every D0 exit", s->WarmRebuild,
                  s->WarmRebuild ? "built again when proven this driver's" : "a power cycle instead", s->WarmPlugins,
                  s->WarmPlugins ? "plug-ins on adopted or rebuilt strips" : "plug-ins after a power cycle only",
                  StripsRecordsKept(Ctx) ? "" : "; no unit key: no record, so no adoption");
    }
}

/*
 * ApolloMixerInit's capture-routing batch (docs/STRIPS.md section 6), DevLock held. Value 1 is
 * "model-safe": the unit's plan (bringup_plan.h BatchSkip) lists the settings whose low byte is
 * not routing on that model, and leaves them alone. On the x8p (the list every release had):
 *   S3, S14            GainC (preamps 1-4, 5-8)
 *   S12                preamp 5-8 flags (polarity, 48V, PAD, Mic/Line, ...)
 *   S13                Hi-Z / mic impedance
 *   S16-S22, S24-S30   byte lane per preamp of params 0x09-0x0f (ua_core.c:2610-2616)
 *   S23, S31           knob steps (ua_core.c:2617-2622)
 * [Twin X G3] On the Twin X Gen 1 (docs/spec-twinx.md 4.3): S3 GainC 1-2, S11 output reference /
 * mirror, S13 Hi-Z impedance, S14 HP level, S15 ALT trim / remote, S16/S17 GainB/A, S23 knob steps.
 * S2 (monitor) is never written. Legacy = every build before release5.
 */
VOID ApolloStripsMixerInitBatch(_In_ PDEVICE_CONTEXT Ctx)
{
    const APOLLO_BRINGUP_PLAN *plan = ApolloPlanOf(Ctx);
    ULONG mode = Ctx->Strips.MixerBatch, s, n = 0;

    if (mode == APOLLO_MIXER_BATCH_NONE) {
        ApolloLogU(Ctx, "mixer init batch: none (MixerInitBatch=0): firmware defaults kept");
        return;
    }
    for (s = 1; s < MIXER_NUM_SETTINGS; s++) {
        if (!ApolloPlanBatchWrites(plan, mode, s)) {
            continue;                           /* S2 never; S35 gets its own value below */
        }
        Ctx->MixerVal[s] = 0x20;
        Ctx->MixerMask[s] = 0xFF;
        n++;
    }
    Ctx->MixerVal[35] = 0x05;
    Ctx->MixerMask[35] = 0x1F;
    Ctx->MixerDirty = TRUE;
    ApolloLogU(Ctx, "mixer init batch: %s (MixerInitBatch=%lu): %lu settings at 0x20/0xFF, S35 0x05/0x1F",
              mode == APOLLO_MIXER_BATCH_X8P_SAFE ? (plan != NULL ? plan->BatchName : "model-safe, no model: nothing") :
              "legacy full", mode, n);
}

/*
 * After the D0 exit (gate already closed). A slot that was published but never proven taken
 * (Published/Parked) may still be fetched or written by the FPGA: the D0 exit only resyncs
 * the rings, nothing resets DSP DMA. Freeing it under a live engine is an IOMMU violation
 * (bugcheck 0xE6 under Verifier DMA checks). So such a slot is freed only when the device
 * is gone; otherwise its common buffer is left to the DMA enabler's deletion with the device,
 * after PCI has turned bus mastering off (open-apollo: ua_dsp_xact.c:803-806, ua_core.c:5095).
 */
VOID ApolloStripsReleaseHardware(_In_ PDEVICE_CONTEXT Ctx)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    BOOLEAN gone = (Ctx->Stage == ApolloStageGone);
    ULONG i, leaked = 0;

    if (!gone && Ctx->Bar0 != NULL && Ctx->Bar0Length >= REG_FPGA_REV + 4) {
        /* One raw read: the MMIO gate is closed already, the mapping is not. */
        gone = READ_REGISTER_ULONG((volatile ULONG *)(Ctx->Bar0 + REG_FPGA_REV)) == 0xFFFFFFFFu;
    }
    for (i = 0; i < XACT_SLOTS; i++) {
        PAPOLLO_XACT x = &s->Xact[i];

        if (x->Buffer != NULL && !gone &&
            (x->State == XactParked || x->State == XactPublished)) {
            /* Keep the handle and the state: the slot is never reused, nothing frees it here. */
            x->State = XactParked;
            leaked++;
            continue;
        }
        if (x->Buffer != NULL) {
            WdfObjectDelete(x->Buffer);
        }
        RtlZeroMemory(x, sizeof(*x));
    }
    if (leaked) {
        ApolloLogU(Ctx, "strips: %lu unproven transaction buffer(s) left to the device's deletion: "
                  "DSP %lu may still reference them", leaked, s->Dsp);
    }
    s->PoolReady = FALSE;
    s->ReplyWaiting = FALSE;
}

/* ------------------------------------------------------------------ */
/* Ownership and layout (docs/STRIPS.md 2.3, 2.4)                      */
/* ------------------------------------------------------------------ */

/*
 * Port of open-apollo ua_strips.c:440-483 (GPL-2.0): 0 = fits, else a reason code.
 * [Twin X G5] The check is strip_plan.h StripPlanLayoutCheck (same code, tested offline); the
 * room it reserves is the model's (x8p 8 strips, as before; Twin X its 2).
 */
static ULONG ApolloStripLayoutCheck(_In_ PDEVICE_CONTEXT Ctx, _In_reads_(4) const ULONG *Size,
                                    _In_reads_(4) const ULONG *Base, _In_reads_(4) const ULONG *Scr)
{
    const STRIP_MODEL *model = StripsModel(Ctx);
    ULONG p, codeWords = 0;

    for (p = 0; p < APOLLO_STRIP_MODULES; p++) {
        codeWords += g_ApolloStripPins[p].PoolWords;    /* 994 */
    }
    return StripPlanLayoutCheck(Size, Base, Scr, (model != NULL) ? model->LayoutStrips : APOLLO_STRIP_INPUTS,
                                codeWords);
}

/* The strips the layout check reserves room for on this unit (log lines). */
static ULONG StripsLayoutStrips(_In_ PDEVICE_CONTEXT Ctx)
{
    const STRIP_MODEL *model = StripsModel(Ctx);

    return (model != NULL) ? model->LayoutStrips : APOLLO_STRIP_INPUTS;
}

/* The strip DSP's 11 pool registers (reads only) and the check for the model's strips (x8p: 8). */
static VOID StripsReadLayout(_In_ PDEVICE_CONTEXT Ctx)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    ULONG w = ApolloDspWindow(s->Dsp), size[4], base[4], scr[4], p;

    for (p = 0; p < 4; p++) {
        size[p] = ApolloRead(Ctx, w + 0x010 + 4 * p);
        base[p] = ApolloRead(Ctx, w + 0x184 + 4 * p);
        scr[p] = p ? ApolloRead(Ctx, w + 0x190 + 4 * p) : 0;
        s->Geom[p] = size[p];
        s->Geom[4 + p] = base[p];
        if (p) {
            s->Geom[7 + p] = scr[p];
        }
    }
    s->LayoutWhy = ApolloStripLayoutCheck(Ctx, size, base, scr);
    s->LayoutRead = TRUE;
}

/*
 * Bring-up, DevLock held, after E3 and BEFORE E11: our ring connect turns the boot word into
 * 0x0B + a connect count. 3 in the low byte = no host since power-up (ua_plugin_place.c:95-136).
 * Every bring-up is a new epoch: the rings are reprogrammed next, so nothing queued survives.
 */
VOID ApolloStripsProbeBoot(_In_ PDEVICE_CONTEXT Ctx)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    ULONG i, n = min(Ctx->NumDsps, (ULONG)APOLLO_MAX_DSPS);

    RtlZeroMemory(s->BootWord, sizeof(s->BootWord));
    for (i = 0; i < n; i++) {
        s->BootWord[i] = ApolloRead(Ctx, ApolloDspWindow(i) + DSP_WIN_READY);
    }
    s->BootWordsRead = TRUE;
    s->Epoch++;
    s->Gen++;
    s->ReplyWaiting = FALSE;
    s->WorkQueued = FALSE;
    s->RateWaitLogged = FALSE;
    s->LayoutRead = FALSE;
    s->LayoutWhy = STRIP_LAYOUT_UNREAD;
    for (i = 0; i < XACT_SLOTS; i++) {
        if (s->Xact[i].State == XactPublished) {
            s->Xact[i].State = XactParked;      /* its ring is about to be reprogrammed */
        }
    }
    StripsForgetModules(s);                     /* fresh DSPs, or not ours: nothing to release */
    StripsForgetStrips(s);
    RtlZeroMemory(s->StripHeader, sizeof(s->StripHeader));
    RtlZeroMemory(s->StripAckMs, sizeof(s->StripAckMs));
    s->StepsDone = 0;
    s->RoutesSent = 0;
    s->ProbeDead = 0;
    s->ProbeMeasured = 0;
    s->ProbeRepairs = 0;
    /* [Warm] nothing adopted or rebuilt yet at this start; the record this unit's last start left */
    s->AdoptPending = FALSE;
    s->Adopted = FALSE;
    s->RebuiltWarm = FALSE;
    s->TeardownFirst = FALSE;
    s->PlugAdoptOk = FALSE;
    s->WarmUploads = STRIPS_WARM_UPLOADS_NA;
    s->KeptAtExit = FALSE;
    s->AdoptWaits = 0;
    s->AdoptVerdict = STRIP_ADOPT_NONE;
    s->Anchor = STRIP_ANCHOR_NOT_READ;
    StripsRecLoad(Ctx);

    s->Owned = !Ctx->WarmStart && n > s->Dsp && StripOwned(s->BootWord[s->Dsp]);
    s->PhantomPending = FALSE;
    if (s->Owned) {
        s->Quarantined = FALSE;                 /* a power cycle ends both */
        s->Tainted = FALSE;
        s->PhantomPending = TRUE;               /* 48V comes up only on a written 0 -> 1 edge */
        s->PhantomState = APOLLO_STRIPS_PHANTOM_NOT_RUN;
        s->PhantomMask = 0;
    }
    ApolloLogU(Ctx, "strips: boot words %08x %08x %08x %08x %08x %08x (DSP %lu %s)",
              s->BootWord[0], s->BootWord[1], s->BootWord[2], s->BootWord[3],
              s->BootWord[4], s->BootWord[5], s->Dsp,
              s->Owned ? "untouched since power-up" : "already used by a host");

    if (s->Enable == 0) {
        s->State = APOLLO_STRIPS_STATE_OFF;
        StripsSetWhy(s, "StripsEnable is 0");
    } else if (!s->Owned && Ctx->WarmStart && StripsRecordsKept(Ctx) && n > s->Dsp &&
               (s->Enable == 2 || s->PoolReady)) {
        /*
         * [Warm] The decision waits for the end of the bring-up (the anchor needs the boot words after
         * any ring connect, the plan needs the rate) and is made by the first pass (StripsAdoptDecide).
         */
        s->AdoptPending = TRUE;
        s->State = APOLLO_STRIPS_STATE_OFF;
        StripsSetWhy(s, "warm start: looking for this driver's own strips (record %s)",
                     s->RecFound ? "found" : StripAnchorText(s->RecWhy));
        ApolloLogU(Ctx, "strips: warm start: the record %s; the decision follows the bring-up",
                  s->RecFound ? "of this unit was found" : StripAnchorText(s->RecWhy));
    } else if (!s->Owned) {
        StripsOff(s, Ctx->WarmStart ?
                  "warm start: power-cycle the Apollo for input strips" :
                  "the strip DSP was used by a host since power-up: power-cycle the Apollo");
    } else if (s->Enable == 1 && !s->PoolReady) {
        StripsOff(s, "no transaction buffers");
    } else {
        s->State = APOLLO_STRIPS_STATE_IDLE;
        StripsSetWhy(s, "cold start: DSP %lu owned", s->Dsp);
    }
}

/* Bring-up complete, DevLock held: the pool registers, logged (reads only). */
VOID ApolloStripsProbeLayout(_In_ PDEVICE_CONTEXT Ctx)
{
    APOLLO_STRIPS *s = &Ctx->Strips;

    if (Ctx->NumDsps <= s->Dsp) {
        return;
    }
    StripsReadLayout(Ctx);
    ApolloLogU(Ctx, "strips: DSP %lu pools size %08x %08x %08x %08x base %08x %08x %08x %08x "
              "scratch %08x %08x %08x", s->Dsp, s->Geom[0], s->Geom[1], s->Geom[2], s->Geom[3],
              s->Geom[4], s->Geom[5], s->Geom[6], s->Geom[7], s->Geom[8], s->Geom[9], s->Geom[10]);
    ApolloLogU(Ctx, "strips: layout %s (code %lu) for %lu strips; %s",
              s->LayoutWhy == 0 ? "matches the recipe" : "does NOT match", s->LayoutWhy,
              StripsLayoutStrips(Ctx), s->Enable == 1 ? "uploads may follow" : "nothing sent");
    StripsRecRefresh(Ctx);                      /* [Warm] this start's anchor, the record's boot words */
}

/* Bring-up complete, DevLock held: start the settle time of the first pass. */
VOID ApolloStripsArm(_In_ PDEVICE_CONTEXT Ctx)
{
    APOLLO_STRIPS *s = &Ctx->Strips;

    if (s->AdoptPending) {
        /*
         * [Warm] The first pass decides (StripsAdoptDecide), after the same settle time and rate gate a
         * build gets: adopt (nothing sent), restore, rebuild, or off with the reason.
         */
        if (s->State != APOLLO_STRIPS_STATE_OFF || !s->LayoutRead) {
            s->AdoptPending = FALSE;
            StripsOff(s, "warm start: the bring-up did not read the strip DSP's pools: power-cycle the Apollo");
            return;
        }
        s->State = APOLLO_STRIPS_STATE_ARMED;
        s->ArmedAt = KeQueryInterruptTime();
        s->RateWaitLogged = FALSE;
        StripsSetWhy(s, "warm start: checking the strip DSP against this unit's record");
        ApolloLogU(Ctx, "strips: warm start: the check runs in %u ms once the DSPs run at %s", STRIP_SETTLE_MS,
                  ApolloRateText(Ctx->SampleRate));
        return;
    }
    if (s->State != APOLLO_STRIPS_STATE_IDLE || !s->Owned) {
        return;
    }
    if (!s->LayoutRead || s->LayoutWhy != 0) {
        ApolloLogU(Ctx, "strips: DSP %lu pools do not match the recipe (code %lu): nothing sent",
                  s->Dsp, s->LayoutWhy);
        StripsOff(s, "the strip DSP's pools do not match the recipe: nothing sent");
        return;
    }
    s->State = APOLLO_STRIPS_STATE_ARMED;
    s->ArmedAt = KeQueryInterruptTime();
    s->RateWaitLogged = FALSE;
    StripsSetWhy(s, "armed: waiting %u ms and for %s locked", STRIP_SETTLE_MS, ApolloRateText(Ctx->SampleRate));
    ApolloLogU(Ctx, "strips: armed (%s): the pass starts in %u ms once the DSPs run at %s",
              s->Enable == 2 ? "dry run" : "uploads", STRIP_SETTLE_MS, ApolloRateText(Ctx->SampleRate));
}

/* The 10 ms service pass, DevLock held: queue the pass after the settle time. */
VOID ApolloStripsService(_In_ PDEVICE_CONTEXT Ctx)
{
    APOLLO_STRIPS *s = &Ctx->Strips;

    if (s->State != APOLLO_STRIPS_STATE_ARMED || s->WorkQueued ||
        ReadAcquire(&Ctx->StopRequested) || Ctx->StripWork == NULL) {
        return;
    }
    if (KeQueryInterruptTime() - s->ArmedAt < MS_TO_100NS(STRIP_SETTLE_MS)) {
        return;
    }
    s->WorkQueued = TRUE;
    WdfWorkItemEnqueue(Ctx->StripWork);
}

/*
 * IOCTL_APOLLO_WRITE_REG, DevLock held: a raw write may change what the DSPs hold behind our
 * back (ua_plugin_place.c:147-160). The mixer mailbox window is exempt. Nothing is sent.
 */
VOID ApolloStripsTaint(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Offset)
{
    APOLLO_STRIPS *s = &Ctx->Strips;

    if (Offset >= REG_MIXER_BASE && Offset < REG_MIXER_BASE + 0x100) {
        return;
    }
    if (s->Tainted) {
        return;
    }
    s->Tainted = TRUE;
    s->Owned = FALSE;
    s->Gen++;                                   /* a running pass stops */
    s->Epoch++;                                 /* nothing built before may publish (ua_plugin_place.c:158) */
    StripsForgetModules(s);
    StripsForgetStrips(s);                      /* not ours to stop any more */
    s->PhantomPending = FALSE;
    s->AdoptPending = FALSE;
    s->PlugAdoptOk = FALSE;
    StripsRecLost(Ctx, "raw register write");  /* [Warm] nothing about the DSPs is known until a power cycle */
    if (s->Enable != 0) {
        ApolloLogU(Ctx, "strips: raw register write 0x%04lx: strips off until the Apollo is power-cycled",
                  Offset);
        s->State = APOLLO_STRIPS_STATE_OFF;
        StripsSetWhy(s, "raw register write 0x%04lx: power-cycle the Apollo", Offset);
    }
}

/*
 * [Rate] After a rate change (rate.c, DevLock held; ApolloStripsQuiesce tore the strips down before
 * the clock moved): armed again, so the next pass builds them at the new rate (StripsRateReady,
 * StripPlanPrepareRate), with no power cycle: the strip DSP is still ours (Owned) and its pools are
 * empty again. A teardown that was not proven leaves nothing known about the DSP: strips off, named.
 * Returns TRUE when they are armed (they come back by themselves).
 */
BOOLEAN ApolloStripsRateRearm(_In_ PDEVICE_CONTEXT Ctx)
{
    APOLLO_STRIPS *s = &Ctx->Strips;

    if (s->Enable == 0 || !s->Owned || s->Quarantined || s->Tainted) {
        return FALSE;                           /* off, warm start, or not ours: as before the change */
    }
    if (s->State == APOLLO_STRIPS_STATE_LIVE) {
        StripsOff(s, "the strips' teardown before the rate change was not proven: power-cycle the Apollo");
        ApolloLogU(Ctx, "strips: %s", s->Why);
        return FALSE;
    }
    if (s->State != APOLLO_STRIPS_STATE_IDLE) {
        return FALSE;
    }
    if (Ctx->WarmStart && !s->WarmRebuild && !StripsConnectsOnWarm(Ctx)) {
        /*
         * [Warm] Adopted at a warm start: building again means uploads to a DSP this driver never connected
         * in this power session, which only StripWarmRebuild allows (docs/STRIPS.md 12.4).
         */
        StripsOff(s, "the rate changed after the strips were adopted at a warm start: power-cycle the Apollo "
                     "for strips at the new rate (or set StripWarmRebuild=1)");
        ApolloLogU(Ctx, "strips: %s", s->Why);
        return FALSE;
    }
    ApolloStripsArm(Ctx);
    return s->State == APOLLO_STRIPS_STATE_ARMED ? TRUE : FALSE;
}

/* IOCTL_APOLLO_GET_STRIPS, DevLock held. */
VOID ApolloStripsFillInfo(_In_ PDEVICE_CONTEXT Ctx, _Out_ PAPOLLO_STRIPS_INFO I)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    ULONG j;

    RtlZeroMemory(I, sizeof(*I));
    I->Version = 2;
    I->State = s->State;
    if (s->Owned)        I->Flags |= APOLLO_STRIPS_FLAG_OWNED;
    if (s->Quarantined)  I->Flags |= APOLLO_STRIPS_FLAG_QUARANTINED;
    if (s->Tainted)      I->Flags |= APOLLO_STRIPS_FLAG_TAINTED;
    if (s->ModulesOk)    I->Flags |= APOLLO_STRIPS_FLAG_MODULES_OK;
    if (s->BootWordsRead) I->Flags |= APOLLO_STRIPS_FLAG_BOOT_READ;
    if (s->LayoutRead)   I->Flags |= APOLLO_STRIPS_FLAG_LAYOUT_READ;
    if (s->PoolReady)    I->Flags |= APOLLO_STRIPS_FLAG_POOL_READY;
    I->Enable = s->Enable;
    I->Steps = s->Steps;
    I->Mask = s->Mask;
    I->Dsp = s->Dsp;
    I->LayoutWhy = s->LayoutWhy;
    RtlCopyMemory(I->BootWord, s->BootWord, sizeof(I->BootWord));
    RtlCopyMemory(I->Geometry, s->Geom, sizeof(I->Geometry));
    for (j = 0; j < APOLLO_STRIP_MODULES; j++) {
        I->Module[j].Id = g_ApolloStripPins[j].Id;
        I->Module[j].Dest = g_ApolloStripPins[j].Dest;
        I->Module[j].FileStatus = s->ModFile[j];
        I->Module[j].DspState = s->ModState[j];
        RtlCopyMemory(I->Module[j].Reply, s->ModReply[j], sizeof(I->Module[j].Reply));
    }
    I->Passes = s->Passes;
    I->Uploads = s->Uploads;
    I->RepliesOk = s->RepliesOk;
    I->RepliesBad = s->RepliesBad;
    I->Timeouts = s->Timeouts;
    I->Teardowns = s->Teardowns;
    I->LastCmdEnd = s->LastCmdEnd;
    I->LastCmdPos = s->LastCmdPos;
    I->LastRespEnd = s->LastRespEnd;
    I->LastRespPos = s->LastRespPos;
    RtlCopyMemory(I->Why, s->Why, sizeof(I->Why));
    I->Why[sizeof(I->Why) - 1] = '\0';

    /* Version 2: the strip stages */
    I->StepsLimit = StripsStepLimit(s->Steps);
    I->StepsDone = s->StepsDone;
    I->MixerInitBatch = s->MixerBatch;
    I->PhantomState = (s->PhantomState == STRIP_PHANTOM_RUNNING) ? APOLLO_STRIPS_PHANTOM_NOT_RUN :
                      s->PhantomState;
    I->PhantomMask = s->PhantomMask;
    I->RoutesSent = s->RoutesSent;
    for (j = 0; j < APOLLO_STRIP_INPUTS; j++) {
        I->StripState[j] = s->StripState[j];
        I->StripStarted[j] = s->StripStarted[j] ? 1 : 0;
        I->StripHeader[j] = s->StripHeader[j];
        I->StripAckMs[j] = s->StripAckMs[j];
        I->BusOwner[j] = s->BusOwner[j];
    }

    /* Version 3 [Warm]: warm starts */
    I->Version = 3;
    I->Adopt = s->AdoptVerdict;
    I->Anchor = s->Anchor;
    if (s->Adopted)      I->AdoptFlags |= APOLLO_STRIPS_AF_ADOPTED;
    if (s->RebuiltWarm)  I->AdoptFlags |= APOLLO_STRIPS_AF_REBUILT_WARM;
    if (s->RecHave)      I->AdoptFlags |= APOLLO_STRIPS_AF_RECORD;
    if (s->AdoptParam && !StripsConnectsOnWarm(Ctx)) I->AdoptFlags |= APOLLO_STRIPS_AF_KEEP;
    if (s->WarmRebuild)  I->AdoptFlags |= APOLLO_STRIPS_AF_WARM_REBUILD;
    if (s->PlugAdoptOk)  I->AdoptFlags |= APOLLO_STRIPS_AF_PLUGINS_OK;
    if (s->RecHave && s->Cur.Anchored) I->AdoptFlags |= APOLLO_STRIPS_AF_ANCHORED;
    if (s->WarmPlugins)  I->AdoptFlags |= APOLLO_STRIPS_AF_WARM_PLUGINS;
    if (s->WarmUploads == STRIPS_WARM_UPLOADS_PROVEN) I->AdoptFlags |= APOLLO_STRIPS_AF_UPLOADS_PROVEN;
    I->RecordState = s->RecHave ? s->Cur.State : APOLLO_STRIPS_REC_ABSENT;
    RtlCopyMemory(I->RecordBootWord, s->Cur.BootWord, sizeof(I->RecordBootWord));
    I->RecordAdoptions = s->Cur.Adoptions;
    I->RecordBuiltDate = s->Cur.BuiltDate;
    I->RecordPlugins = s->Cur.Plugins;
    I->RecordStatus = s->RecStatus;
}

/* ------------------------------------------------------------------ */
/* Module files (docs/STRIPS.md 4.3), PASSIVE, no locks held           */
/* ------------------------------------------------------------------ */

static NTSTATUS StripsReadFile(_In_ const APOLLO_STRIP_PIN *Pin,
                               _Out_writes_bytes_(Cap) PUCHAR Out, _In_ ULONG Cap)
{
    WCHAR pathBuf[96];
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    FILE_STANDARD_INFORMATION fsi;
    LARGE_INTEGER offset;
    HANDLE h = NULL;
    NTSTATUS status;

    RtlZeroMemory(Out, Cap);
    if (Pin->Length > Cap) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    status = RtlStringCbPrintfW(pathBuf, sizeof(pathBuf), L"%ws%ws", APOLLO_STRIP_DIR_NT,
                                Pin->FileName);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    RtlInitUnicodeString(&path, pathBuf);
    InitializeObjectAttributes(&oa, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    /*
     * FILE_COMPLETE_IF_OPLOCKED: never wait for someone else's oplock break. The D0 exit
     * flushes this work item unconditionally, so a blocked open would hang sleep or removal.
     */
    status = ZwCreateFile(&h, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ, FILE_OPEN,
                          FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
                          FILE_COMPLETE_IF_OPLOCKED, NULL, 0);
    if (status == STATUS_OPLOCK_BREAK_IN_PROGRESS) {
        ZwClose(h);                             /* a success code: someone holds an oplock */
        return STATUS_SHARING_VIOLATION;
    }
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = ZwQueryInformationFile(h, &iosb, &fsi, sizeof(fsi), FileStandardInformation);
    if (NT_SUCCESS(status) && fsi.EndOfFile.QuadPart != (LONGLONG)Pin->Length) {
        status = STATUS_INFO_LENGTH_MISMATCH;
    }
    if (NT_SUCCESS(status)) {
        offset.QuadPart = 0;
        status = ZwReadFile(h, NULL, NULL, NULL, &iosb, Out, Pin->Length, &offset, NULL);
        if (NT_SUCCESS(status) && iosb.Information != Pin->Length) {
            status = STATUS_END_OF_FILE;
        }
    }
    ZwClose(h);
    return status;
}

/* ua_strips.c:256-274 plus the SHA-256 pin. */
static NTSTATUS StripsCheckRecord(_In_ const APOLLO_STRIP_PIN *Pin,
                                  _In_reads_bytes_(Pin->Length) const UCHAR *Rec,
                                  _In_ BCRYPT_ALG_HANDLE Alg)
{
    ULONG w[5];
    UCHAR sha[32];
    NTSTATUS status;

    if ((Pin->Length & 3) != 0 || Pin->Length < sizeof(w)) {
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    RtlCopyMemory(w, Rec, sizeof(w));
    if (w[0] != APOLLO_STRIP_BILL_MAGIC || w[1] != Pin->Id || w[2] != APOLLO_STRIP_BILL_W2 ||
        w[3] != (ULONG)Pin->Length - APOLLO_STRIP_BILL_HEADER || w[4] != Pin->W4 ||
        (w[4] * 2) / 3 != Pin->PoolWords) {
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    status = BCryptHash(Alg, NULL, 0, (PUCHAR)Rec, Pin->Length, sha, sizeof(sha));
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (RtlCompareMemory(sha, Pin->Sha256, sizeof(sha)) != sizeof(sha)) {
        return STATUS_INVALID_IMAGE_HASH;
    }
    return STATUS_SUCCESS;
}

/* Reads and verifies all five, once per device start. Only the pinned ids, only these files. */
static VOID StripsLoadModules(_In_ PDEVICE_CONTEXT Ctx)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    BCRYPT_ALG_HANDLE alg = NULL;
    NTSTATUS st[APOLLO_STRIP_MODULES], status;
    BOOLEAN ok = TRUE;
    ULONG j;

    status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    for (j = 0; j < APOLLO_STRIP_MODULES; j++) {
        const APOLLO_STRIP_PIN *pin = &g_ApolloStripPins[j];
        ULONG off = s->ModOffset[j];

        if (!NT_SUCCESS(status)) {
            st[j] = status;
        } else if (off > sizeof(s->Blob) || pin->Length > sizeof(s->Blob) - off) {
            st[j] = STATUS_BUFFER_TOO_SMALL;
        } else {
            st[j] = StripsReadFile(pin, s->Blob + off, pin->Length);
            if (NT_SUCCESS(st[j])) {
                st[j] = StripsCheckRecord(pin, s->Blob + off, alg);
            }
        }
        if (!NT_SUCCESS(st[j])) {
            ok = FALSE;
        }
    }
    if (alg != NULL) {
        (VOID)BCryptCloseAlgorithmProvider(alg, 0);
    }

    for (j = 0; j < APOLLO_STRIP_MODULES; j++) {
        const APOLLO_STRIP_PIN *pin = &g_ApolloStripPins[j];

        if (NT_SUCCESS(st[j])) {
            ApolloLogU(Ctx, "strips: module 0x%03x verified (%u bytes, w4 %u, sha256 %02x%02x%02x%02x..)",
                      pin->Id, pin->Length, pin->W4, pin->Sha256[0], pin->Sha256[1],
                      pin->Sha256[2], pin->Sha256[3]);
        } else {
            ApolloLogU(Ctx, "strips: module 0x%03x refused: %s (0x%08x)", pin->Id,
                      st[j] == STATUS_OBJECT_NAME_NOT_FOUND || st[j] == STATUS_OBJECT_PATH_NOT_FOUND ?
                      "file missing (apollo-diag strips install)" :
                      st[j] == STATUS_INVALID_IMAGE_HASH ? "SHA-256 is not the pinned one" :
                      st[j] == STATUS_INFO_LENGTH_MISMATCH ? "length is not the pinned one" :
                      st[j] == STATUS_INVALID_IMAGE_FORMAT ? "header is not the pinned one" :
                      st[j] == STATUS_SHARING_VIOLATION ? "file is open elsewhere (oplock or share)" :
                      "could not be read", st[j]);
        }
    }

    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    RtlCopyMemory(s->ModFile, st, sizeof(s->ModFile));
    s->ModulesOk = ok;
    WdfWaitLockRelease(Ctx->DevLock);
}

/* ------------------------------------------------------------------ */
/* Transactions (docs/STRIPS.md section 3; ua_dsp_xact.c)              */
/* ------------------------------------------------------------------ */

/* DevLock held. A free slot, zeroed, bound to this epoch; NULL when none is free. */
static PAPOLLO_XACT XactAlloc(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Dsp)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    ULONG i;

    if (!s->PoolReady) {
        return NULL;
    }
    for (i = 0; i < XACT_SLOTS; i++) {
        PAPOLLO_XACT x = &s->Xact[i];

        if (x->Buffer == NULL || x->Va == NULL || x->State != XactFree) {
            continue;
        }
        x->State = XactBuilding;
        x->Dsp = Dsp;
        x->Epoch = s->Epoch;
        x->Err = STATUS_SUCCESS;
        x->N = 0;
        x->Len = 0;
        x->NRx = 0;
        RtlZeroMemory(x->Ref, sizeof(x->Ref));
        x->PublishedAt = 0;
        RtlZeroMemory(x->Va, XACT_BYTES);
        return x;
    }
    return NULL;
}

static VOID XactAdd(_Inout_ PAPOLLO_XACT X, _In_ ULONG W0, _In_ ULONG W1, _In_ ULONG W2,
                    _In_ ULONG W3)
{
    if (!NT_SUCCESS(X->Err)) {
        return;
    }
    if (X->State != XactBuilding || X->N >= XACT_ENTS_MAX) {
        X->Err = STATUS_BUFFER_OVERFLOW;
        return;
    }
    X->Ent[X->N][0] = W0;
    X->Ent[X->N][1] = W1;
    X->Ent[X->N][2] = W2;
    X->Ent[X->N][3] = W3;
    X->N++;
}

/* An inline command: bit 31 of W0 clear, never a DMA reference (ua_dsp_xact.c:100-108). */
static VOID XactInline(_Inout_ PAPOLLO_XACT X, _In_ ULONG W0, _In_ ULONG W1, _In_ ULONG W2,
                       _In_ ULONG W3)
{
    if (W0 & RING_DMA_REF) {
        if (NT_SUCCESS(X->Err)) {
            X->Err = STATUS_INVALID_PARAMETER;
        }
        return;
    }
    XactAdd(X, W0, W1, W2, W3);
}

/*
 * Room for one command carried by DMA (header words included), as one DMA_REF that never
 * crosses the payload page (1 KiB pieces took the x4 off the bus: ua_dsp_xact.c:110-121).
 */
static PULONG XactCmd(_Inout_ PAPOLLO_XACT X, _In_ ULONG Dwords)
{
    ULONG off = X->Len;

    if (!NT_SUCCESS(X->Err)) {
        return NULL;
    }
    if (Dwords == 0 || Dwords > XACT_PAGE / 4 || off > XACT_PAGE || Dwords * 4 > XACT_PAGE - off) {
        X->Err = STATUS_BUFFER_OVERFLOW;
        return NULL;
    }
    XactAdd(X, RING_DMA_REF | Dwords, 0, off, 0);
    if (!NT_SUCCESS(X->Err)) {
        return NULL;
    }
    X->Len = off + Dwords * 4;
    return (PULONG)(X->Va + off);
}

/* A reply descriptor of Dwords on page 1, 16-byte aligned (ua_dsp_xact.c:157-177). */
static VOID XactRx(_Inout_ PAPOLLO_XACT X, _In_ ULONG Dwords)
{
    ULONG off = 0, k = X->NRx;

    if (!NT_SUCCESS(X->Err)) {
        return;
    }
    if (Dwords == 0 || Dwords > STRIP_RX_WORDS_MAX || k >= XACT_RX_MAX) {
        X->Err = STATUS_BUFFER_OVERFLOW;
        return;
    }
    if (k > 0) {
        off = X->RxOff[k - 1] + ((X->RxWords[k - 1] * 4 + 15) & ~15u);
    }
    if (off + Dwords * 4 > XACT_PAGE) {
        X->Err = STATUS_BUFFER_OVERFLOW;
        return;
    }
    X->RxOff[k] = off;
    X->RxWords[k] = Dwords;
    X->NRx = k + 1;
}

/* Reply k of a retired transaction. */
static VOID XactReply(_In_ const APOLLO_XACT *X, _In_ ULONG K, _Out_writes_(4) PULONG Out)
{
    ULONG i;

    RtlZeroMemory(Out, 4 * sizeof(ULONG));
    if (K >= X->NRx || K >= XACT_RX_MAX || X->Va == NULL) {
        return;
    }
    for (i = 0; i < 4 && i < X->RxWords[K]; i++) {
        Out[i] = ((volatile const ULONG *)(X->Va + XACT_PAGE + X->RxOff[K]))[i];
    }
}

/*
 * DevLock held. All or nothing; replies first, then the command (ua_dsp_xact.c:249-365).
 * STATUS_RETRY: no room yet, or a reply transaction found the command ring busy.
 */
static NTSTATUS XactSubmit(_In_ PDEVICE_CONTEXT Ctx, _Inout_ PAPOLLO_XACT X)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    PAPOLLO_RING cmd, resp;
    ULONG k;

    if (X->State != XactBuilding) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (!NT_SUCCESS(X->Err)) {
        return X->Err;
    }
    if (ApolloGateClosed(Ctx)) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    if (X->Epoch != s->Epoch) {
        return STATUS_CANCELLED;
    }
    if (X->Dsp >= Ctx->NumDsps || X->Dsp >= APOLLO_MAX_DSPS || X->N == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    cmd = &Ctx->Cmd[X->Dsp];
    resp = &Ctx->Resp[X->Dsp];
    if (!cmd->Allocated || !cmd->Live || (X->NRx && (!resp->Allocated || !resp->Live))) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (!NT_SUCCESS(ApolloRingRefresh(Ctx, cmd))) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    if (ApolloRingFreeSlots(cmd) < X->N) {
        return STATUS_RETRY;
    }
    if (X->NRx) {
        if (s->ReplyWaiting) {
            return STATUS_DEVICE_BUSY;
        }
        if (ApolloRingPending(cmd) != 0) {
            return STATUS_RETRY;                /* nothing ahead may answer into our slot */
        }
        if (!NT_SUCCESS(ApolloRingRefresh(Ctx, resp))) {
            return STATUS_DEVICE_NOT_CONNECTED;
        }
        if (ApolloRingPending(resp) != 0) {
            /* someone's unanswered descriptor: abandon it, or it takes our reply */
            ApolloLogU(Ctx, "strips: DSP %lu response ring had %lu stale descriptor(s): resynced",
                      X->Dsp, ApolloRingPending(resp));
            (VOID)ApolloRingResyncNow(Ctx, resp);
            if (ApolloRingPending(resp) != 0) {
                return STATUS_RETRY;
            }
        }
        if (ApolloRingFreeSlots(resp) < X->NRx) {
            return STATUS_RETRY;
        }
    }

    for (k = 0; k < X->NRx && k < XACT_RX_MAX; k++) {
        ULONGLONG a = X->La + XACT_PAGE + X->RxOff[k];

        ApolloRingWrite(resp, RING_DMA_REF | X->RxWords[k], 0, (ULONG)a, (ULONG)(a >> 32));
    }
    for (k = 0; k < X->N && k < XACT_ENTS_MAX; k++) {
        ULONG w2 = X->Ent[k][2], w3 = X->Ent[k][3];

        if (X->Ent[k][0] & RING_DMA_REF) {
            ULONGLONG a = X->La + w2;

            w2 = (ULONG)a;
            w3 = (ULONG)(a >> 32);
        }
        ApolloRingWrite(cmd, X->Ent[k][0], X->Ent[k][1], w2, w3);
    }
    /* ApolloRingDoorbell's barrier orders the payload and the slots before the MMIO writes. */
    if (X->NRx) {
        ApolloRingDoorbell(Ctx, resp);
        X->Ref[1].Pending = TRUE;
        X->Ref[1].Gen = resp->Gen;
        X->Ref[1].End = resp->WritePtr;
        s->ReplyWaiting = TRUE;
    }
    ApolloRingDoorbell(Ctx, cmd);
    X->Ref[0].Pending = TRUE;
    X->Ref[0].Gen = cmd->Gen;
    X->Ref[0].End = cmd->WritePtr;
    X->State = XactPublished;
    X->PublishedAt = KeQueryInterruptTime();
    return STATUS_SUCCESS;
}

/* DevLock held. POLL_DONE (replies readable), POLL_PENDING, POLL_GONE or POLL_STALE. */
static LONG XactPoll(_In_ PDEVICE_CONTEXT Ctx, _Inout_ PAPOLLO_XACT X)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    ULONG k;

    if (X->State == XactRetired) {
        return POLL_DONE;
    }
    if (X->State != XactPublished || X->Dsp >= APOLLO_MAX_DSPS) {
        return POLL_STALE;
    }
    if (ApolloGateClosed(Ctx)) {
        return POLL_GONE;
    }
    if (X->Epoch != s->Epoch) {
        return POLL_STALE;
    }
    for (k = 0; k < 2; k++) {
        XACT_REF *r = &X->Ref[k];
        PAPOLLO_RING ring = k ? &Ctx->Resp[X->Dsp] : &Ctx->Cmd[X->Dsp];
        ULONG dEnd;

        if (!r->Pending) {
            continue;
        }
        if (!ring->Live || ring->Gen != r->Gen) {
            return POLL_STALE;                  /* a resync dropped what was queued */
        }
        if (!NT_SUCCESS(ApolloRingRefresh(Ctx, ring))) {
            return POLL_GONE;
        }
        /* ua_dsp_xact.c:367-384: past our last entry, or the ring is idle at it */
        dEnd = (r->End - ring->Position) & RING_INDEX_MASK;
        if (dEnd == 0 || dEnd > ApolloRingPending(ring)) {
            r->Pending = FALSE;
        }
    }
    if (X->Ref[0].Pending || X->Ref[1].Pending) {
        return POLL_PENDING;
    }
    KeMemoryBarrier();                          /* POSITION moved: the reply words are in */
    if (X->NRx) {
        s->ReplyWaiting = FALSE;
    }
    X->State = XactRetired;
    return POLL_DONE;
}

/* DevLock held. Never published or retired: free. Published, unproven: parked for good. */
static VOID XactRelease(_In_ PDEVICE_CONTEXT Ctx, _Inout_ PAPOLLO_XACT X)
{
    UNREFERENCED_PARAMETER(Ctx);
    if (X->State == XactPublished) {
        X->State = XactParked;
    } else if (X->State == XactBuilding || X->State == XactRetired) {
        X->State = XactFree;
    }
}

/* No lock held. Publish, retrying a full or busy ring for 50 ms with DevLock released. */
static NTSTATUS XactPublish(_In_ PDEVICE_CONTEXT Ctx, _Inout_ PAPOLLO_XACT X)
{
    ULONGLONG until = KeQueryInterruptTime() + MS_TO_100NS(STRIP_PUBLISH_MS);
    NTSTATUS status;

    for (;;) {
        WdfWaitLockAcquire(Ctx->DevLock, NULL);
        status = XactSubmit(Ctx, X);
        WdfWaitLockRelease(Ctx->DevLock);
        if (status != STATUS_RETRY || KeQueryInterruptTime() > until) {
            break;
        }
        ApolloSleepMs(1);
    }
    return status == STATUS_RETRY ? STATUS_DEVICE_BUSY : status;
}

/*
 * No lock held. DevLock is taken for one poll at a time. The poll comes before the deadline
 * check: if the bring-up held DevLock for seconds, a transaction the DSP finished meanwhile
 * still counts as done.
 */
static NTSTATUS XactWait(_In_ PDEVICE_CONTEXT Ctx, _Inout_ PAPOLLO_XACT X, _In_ ULONG TimeoutMs)
{
    ULONGLONG until = X->PublishedAt + MS_TO_100NS(TimeoutMs);
    LONG r;

    for (;;) {
        WdfWaitLockAcquire(Ctx->DevLock, NULL);
        r = XactPoll(Ctx, X);
        WdfWaitLockRelease(Ctx->DevLock);
        if (r == POLL_DONE) {
            return STATUS_SUCCESS;
        }
        if (r == POLL_GONE) {
            return STATUS_DEVICE_NOT_CONNECTED;
        }
        if (r == POLL_STALE) {
            return STATUS_CANCELLED;
        }
        if (KeQueryInterruptTime() > until) {
            return STATUS_IO_TIMEOUT;
        }
        ApolloSleepMs(1);
    }
}

/* DevLock held: what an unproven transaction looked like (ua_dsp_xact.c:504-517). */
static VOID XactReport(_In_ PDEVICE_CONTEXT Ctx, _In_ const APOLLO_XACT *X, _In_ NTSTATUS Status)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    ULONG r[4], dsp = X->Dsp < APOLLO_MAX_DSPS ? X->Dsp : 0;

    RtlZeroMemory(r, sizeof(r));
    if (X->NRx && X->Va != NULL) {
        r[0] = *(volatile const ULONG *)(X->Va + XACT_PAGE + X->RxOff[0]);
    }
    s->LastCmdEnd = X->Ref[0].End;
    s->LastCmdPos = Ctx->Cmd[dsp].Position;
    s->LastRespEnd = X->Ref[1].End;
    s->LastRespPos = Ctx->Resp[dsp].Position;
    ApolloLogU(Ctx, "strips: DSP %lu transaction (%lu entries, %lu replies) ended 0x%08x after %lu ms: "
              "cmd end %lu pos %lu, resp end %lu pos %lu, reply word 0x%08x",
              dsp, X->N, X->NRx, Status,
              (ULONG)((KeQueryInterruptTime() - X->PublishedAt) / 10000ULL),
              s->LastCmdEnd, s->LastCmdPos, s->LastRespEnd, s->LastRespPos, r[0]);
}

/*
 * StripLock held, DevLock not held (ua_dsp_xact.c:531-552). An error before publication
 * changed nothing on the DSP. After publication, an outcome that cannot be proven quarantines
 * the DSP until the next power cycle. The caller releases X under DevLock.
 */
static NTSTATUS XactRun(_In_ PDEVICE_CONTEXT Ctx, _Inout_ PAPOLLO_XACT X)
{
    NTSTATUS status = XactPublish(Ctx, X);

    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = XactWait(Ctx, X, STRIP_WAIT_MS);
    if (!NT_SUCCESS(status) && status != STATUS_DEVICE_NOT_CONNECTED) {
        WdfWaitLockAcquire(Ctx->DevLock, NULL);
        XactReport(Ctx, X, status);
        if (X->Epoch == Ctx->Strips.Epoch) {
            Ctx->Strips.Timeouts++;
            StripsQuarantine(Ctx, "a transaction ended unproven", status);
        } else {
            /* A taint or a new bring-up disowned the DSP meanwhile: nothing more goes there. */
            ApolloLogU(Ctx, "strips: the DSP was disowned while that transaction was out (no quarantine)");
        }
        WdfWaitLockRelease(Ctx->DevLock);
    }
    return status;
}

/* ------------------------------------------------------------------ */
/* The pass (StripWork)                                                */
/* ------------------------------------------------------------------ */

/*
 * The unit's rate acked to the DSPs and locked by the firmware (ua_audio.c:5168-5180,
 * ua_audio_plugin_rate_ready). DevLock held. [Rate] Any of the six rates now (strip_plan.h
 * StripRateWords has the strip's words for each); before rates could change this was 48 kHz only.
 */
static BOOLEAN StripsRateReady(_In_ PDEVICE_CONTEXT Ctx, _Out_ PULONG Info)
{
    ULONG info = ApolloRead(Ctx, REG_NOTIF_RATE_INFO);

    *Info = info;
    return Ctx->Connected && StripRampOf(Ctx->SampleRate) != 0 &&
           Ctx->DspToldRate == Ctx->SampleRate && info != 0xFFFFFFFFu && (info & 3u) == 1u &&
           ((info >> 8) & 0xFFu) == Ctx->RateIndex;
}

/*
 * One upload (ua_plugin_upload.c:24-95, 185-238): {0x0001nnnn, dest, record} by DMA, the
 * record's bytes 10-11 cleared (the SDK form, input_strips_form=1), one 4-dword reply that
 * must read {0x80070004, 0, id, x}: in the SDK form the id goes out without the architecture
 * byte, so only the plain id is accepted (ua_plugin_upload.c:84-93). TRUE = resident and the
 * pass goes on. After the wait, state, counters and quarantine change only while Gen is still
 * the pass's: a taint meanwhile forgot the modules and must not see one come back.
 */
static BOOLEAN StripsUploadOne(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG J, _In_ ULONG Gen)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    const APOLLO_STRIP_PIN *pin = &g_ApolloStripPins[J];
    ULONG dwords = pin->Length / 4 + 2, r[4];
    PAPOLLO_XACT x;
    PULONG cmd;
    NTSTATUS status;
    BOOLEAN ok = FALSE;

    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (s->Gen != Gen || s->State != APOLLO_STRIPS_STATE_BUILDING ||
        ReadAcquire(&Ctx->StopRequested) || J >= APOLLO_STRIP_MODULES) {
        WdfWaitLockRelease(Ctx->DevLock);
        return FALSE;
    }
    x = XactAlloc(Ctx, s->Dsp);
    if (x == NULL) {
        s->State = APOLLO_STRIPS_STATE_FAILED;
        StripsSetWhy(s, "no free transaction buffer for module 0x%03x", pin->Id);
        ApolloLogU(Ctx, "strips: %s", s->Why);
        WdfWaitLockRelease(Ctx->DevLock);
        return FALSE;
    }
    XactRx(x, 4);
    cmd = XactCmd(x, dwords);
    if (cmd != NULL && s->ModOffset[J] <= sizeof(s->Blob) &&
        pin->Length <= sizeof(s->Blob) - s->ModOffset[J]) {
        PUCHAR rec = (PUCHAR)(cmd + 2);

        cmd[0] = STRIP_CMD_UPLOAD_CODE | dwords;
        cmd[1] = pin->Dest;
        RtlCopyMemory(rec, s->Blob + s->ModOffset[J], pin->Length);
        rec[10] = 0;                            /* word 2 = 0x02000000 -> 0: the SDK form */
        rec[11] = 0;
    } else if (NT_SUCCESS(x->Err)) {
        x->Err = STATUS_BUFFER_OVERFLOW;
    }
    s->ModState[J] = APOLLO_STRIP_MOD_UPLOADING;
    RtlZeroMemory(s->ModReply[J], sizeof(s->ModReply[J]));
    WdfWaitLockRelease(Ctx->DevLock);

    status = XactRun(Ctx, x);

    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (s->Gen != Gen) {
        /* Tainted (or quiesced) meanwhile: the modules were forgotten; leave it so. */
        ApolloLogU(Ctx, "strips: module 0x%03x: outcome 0x%08x ignored, the strips were turned off meanwhile",
                  pin->Id, status);
    } else if (x->State == XactBuilding) {
        /* never published: the DSP saw nothing */
        s->ModState[J] = APOLLO_STRIP_MOD_NONE;
        if (s->State == APOLLO_STRIPS_STATE_BUILDING) {
            s->State = APOLLO_STRIPS_STATE_FAILED;
            StripsSetWhy(s, "module 0x%03x not sent (0x%08x)", pin->Id, status);
            ApolloLogU(Ctx, "strips: %s", s->Why);
        }
    } else {
        s->Uploads++;
        if (NT_SUCCESS(status)) {
            XactReply(x, 0, r);
            RtlCopyMemory(s->ModReply[J], r, sizeof(r));
            if (r[0] == STRIP_REPLY_UPLOAD && r[1] == 0 && r[2] == pin->Id) {
                s->ModState[J] = APOLLO_STRIP_MOD_RESIDENT;
                s->RepliesOk++;
                ok = TRUE;
                ApolloLogU(Ctx, "strips: DSP %lu accepted module 0x%03x at 0x%05lx (reply %08x %08x %08x %08x)",
                          s->Dsp, pin->Id, pin->Dest, r[0], r[1], r[2], r[3]);
            } else {
                s->ModState[J] = APOLLO_STRIP_MOD_REFUSED;
                s->RepliesBad++;
                ApolloLogU(Ctx, "strips: DSP %lu refused module 0x%03x: reply %08x %08x %08x %08x",
                          s->Dsp, pin->Id, r[0], r[1], r[2], r[3]);
                StripsQuarantine(Ctx, "an upload was not accepted", STATUS_REQUEST_NOT_ACCEPTED);
            }
        } else if (!s->Quarantined && s->State == APOLLO_STRIPS_STATE_BUILDING) {
            /* the device went away mid-wait: nothing more to send */
            s->State = APOLLO_STRIPS_STATE_FAILED;
            StripsSetWhy(s, "module 0x%03x: device gone (0x%08x)", pin->Id, status);
        }
        /* An unproven upload keeps UPLOADING, so the D0-exit release still covers it. */
    }
    XactRelease(Ctx, x);
    if (s->Gen != Gen) {
        ok = FALSE;
    }
    WdfWaitLockRelease(Ctx->DevLock);
    return ok;
}

/*
 * DevLock held. One inline command to DSP Dsp, fire and forget (ua_dsp_ring_send_raw,
 * ua_dsp.c:4035-4065): the route words to the DSPs that do not host the strip get no reply.
 */
static NTSTATUS StripsSendInline(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Dsp, _In_ ULONG W0,
                                 _In_ ULONG W1, _In_ ULONG W2, _In_ ULONG W3)
{
    PAPOLLO_RING cmd;

    if (W0 & RING_DMA_REF) {
        return STATUS_INVALID_PARAMETER;
    }
    if (ApolloGateClosed(Ctx)) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    if (Dsp >= Ctx->NumDsps || Dsp >= APOLLO_MAX_DSPS) {
        return STATUS_INVALID_PARAMETER;
    }
    cmd = &Ctx->Cmd[Dsp];
    if (!cmd->Allocated || !cmd->Live) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (!NT_SUCCESS(ApolloRingRefresh(Ctx, cmd))) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    if (ApolloRingFreeSlots(cmd) < 1) {
        return STATUS_DEVICE_BUSY;
    }
    ApolloRingWrite(cmd, W0, W1, W2, W3);
    ApolloRingDoorbell(Ctx, cmd);
    return STATUS_SUCCESS;
}

/* DevLock held. A plan's entries into transaction X: inline words, or DMA refs and their payload. */
static VOID StripsXactFromPlan(_Inout_ PAPOLLO_XACT X, _In_ const STRIP_PLAN *Plan)
{
    ULONG k;

    for (k = 0; k < Plan->N && k < STRIP_PLAN_ENTS_MAX; k++) {
        const STRIP_PLAN_ENT *e = &Plan->Ent[k];

        if (!e->Dma) {
            XactInline(X, e->W[0], e->W[1], e->W[2], e->W[3]);
        } else {
            PULONG w = XactCmd(X, e->Dwords);

            if (w != NULL && e->Off <= STRIP_PLAN_PAYLOAD_MAX &&
                e->Dwords <= STRIP_PLAN_PAYLOAD_MAX - e->Off) {
                RtlCopyMemory(w, &Plan->Payload[e->Off], e->Dwords * sizeof(ULONG));
            } else if (NT_SUCCESS(X->Err)) {
                X->Err = STATUS_BUFFER_OVERFLOW;
            }
        }
    }
}

/*
 * StripLock held, DevLock not held. The PREPARE transaction for preamp Input (DSP input =
 * Input): stages 2-5 up to Limit, the strip's memory only, nothing started (strip_plan.h),
 * waited for by command-ring consumption (ua_strips.c:1689-1729). TRUE = taken and the pass
 * goes on. Then *Held is the retired transaction: the pass keeps it (XactAlloc never hands it
 * out again) until the START is taken, so no later strip's payload is ever written into a
 * buffer an earlier strip's DMA refs pointed at while the pass runs (docs/STRIPS.md 10.8).
 */
static BOOLEAN StripsPrepareOne(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Input, _In_ ULONG Limit,
                                _In_ ULONG Gen, _Out_ PAPOLLO_XACT *Held)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    STRIP_PLAN *plan = (STRIP_PLAN *)s->PlanScratch;   /* StripLock serialises its users */
    PAPOLLO_XACT x;
    NTSTATUS status;
    ULONG header;
    BOOLEAN ok = FALSE;

    *Held = NULL;
    if (Input >= APOLLO_STRIP_INPUTS) {
        return FALSE;
    }
    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (s->Gen != Gen || s->State != APOLLO_STRIPS_STATE_BUILDING ||
        ReadAcquire(&Ctx->StopRequested)) {
        WdfWaitLockRelease(Ctx->DevLock);
        return FALSE;
    }
    if (StripPlanPrepareRate(Input, Limit, Ctx->SampleRate, plan) != 0 || plan->N == 0 ||   /* [Rate] the unit's rate */
        plan->N > STRIP_PLAN_PREPARE_ENTS) {
        s->State = APOLLO_STRIPS_STATE_FAILED;
        StripsSetWhy(s, "planner error for input %lu: nothing sent", Input + 1);
        ApolloLogU(Ctx, "strips: %s", s->Why);
        WdfWaitLockRelease(Ctx->DevLock);
        return FALSE;
    }
    x = XactAlloc(Ctx, s->Dsp);
    if (x == NULL) {
        s->State = APOLLO_STRIPS_STATE_FAILED;
        StripsSetWhy(s, "no free transaction buffer for input %lu", Input + 1);
        ApolloLogU(Ctx, "strips: %s", s->Why);
        WdfWaitLockRelease(Ctx->DevLock);
        return FALSE;
    }
    StripsXactFromPlan(x, plan);
    header = plan->Addr[0];
    s->StripState[Input] = APOLLO_STRIP_QUEUED;
    s->StripHeader[Input] = header;
    ApolloLogU(Ctx, "strips: input %lu: prepare transaction, %lu entries (stage %lu: %s), header 0x%05lx, DSP %lu",
              Input + 1, x->N, Limit, StripsStepName(Limit), header, s->Dsp);
    WdfWaitLockRelease(Ctx->DevLock);

    status = XactRun(Ctx, x);

    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (s->Gen != Gen) {
        ApolloLogU(Ctx, "strips: input %lu: outcome 0x%08x ignored, the strips were turned off meanwhile",
                  Input + 1, status);
    } else if (x->State == XactBuilding) {
        /* never published: the DSP saw nothing */
        s->StripState[Input] = APOLLO_STRIP_NONE;
        if (s->State == APOLLO_STRIPS_STATE_BUILDING) {
            s->State = APOLLO_STRIPS_STATE_FAILED;
            StripsSetWhy(s, "strip for input %lu not sent (0x%08x)", Input + 1, status);
            ApolloLogU(Ctx, "strips: %s", s->Why);
        }
    } else if (NT_SUCCESS(status)) {
        s->StripAckMs[Input] = (ULONG)((KeQueryInterruptTime() - x->PublishedAt) / 10000ULL);
        if (Limit < STRIP_STEP_SYNCH) {
            s->StripState[Input] = APOLLO_STRIP_TAKEN;     /* nothing more for it this pass */
        }
        ok = TRUE;
        ApolloLogU(Ctx, "strips: input %lu: Mic Input Strip prepared at 0x%05lx on DSP %lu (taken after %lu ms)%s",
                  Input + 1, header, s->Dsp, s->StripAckMs[Input],
                  Limit >= STRIP_STEP_SYNCH ? "; starts with the others" : ", not started");
    } else if (!s->Quarantined && s->State == APOLLO_STRIPS_STATE_BUILDING) {
        s->State = APOLLO_STRIPS_STATE_FAILED;
        StripsSetWhy(s, "input %lu: device gone (0x%08x)", Input + 1, status);
    }
    /* An unproven transaction already quarantined the DSP (XactRun). */
    if (ok && s->Gen == Gen && x->State == XactRetired) {
        *Held = x;                  /* the pass releases it after the START */
    } else {
        ok = FALSE;
        XactRelease(Ctx, x);
    }
    WdfWaitLockRelease(Ctx->DevLock);
    return ok;
}

/*
 * StripLock held, DevLock not held. The START transaction, sent once every PREPARE of the pass
 * was taken: per preamp in Mask (input 1 first) SYNCH with its header, and from stage 7 span,
 * span and the route word (strip_plan.h StripPlanStart). No strip runs while any strip's
 * memory is written: UA's own order (oa:tools/uausb/soloinit.py:569-620). Waited for by
 * command-ring consumption; then, from STRIP_STEP_OTHERS on, per input the bus owner and the
 * route word on every other DSP (ua_strips.c:1731-1753). TRUE = taken and the pass goes on.
 */
static BOOLEAN StripsStartAll(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Mask, _In_ ULONG Limit,
                              _In_ ULONG Gen)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    STRIP_PLAN *plan = (STRIP_PLAN *)s->PlanScratch;   /* StripLock serialises its users */
    PAPOLLO_XACT x;
    NTSTATUS status;
    ULONG i, d, sent, cached, n = 0, ms;
    BOOLEAN ok = FALSE;

    for (i = 0; i < APOLLO_STRIP_INPUTS; i++) {
        if (Mask & (1u << i)) {
            n++;
        }
    }
    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (s->Gen != Gen || s->State != APOLLO_STRIPS_STATE_BUILDING ||
        ReadAcquire(&Ctx->StopRequested)) {
        WdfWaitLockRelease(Ctx->DevLock);
        return FALSE;
    }
    if (n == 0 || StripPlanStart(Mask, Limit, plan) != 0 || plan->N == 0 ||
        plan->N > STRIP_PLAN_ENTS_MAX) {
        s->State = APOLLO_STRIPS_STATE_FAILED;
        StripsSetWhy(s, "planner error for the start of StripMask 0x%02lx: nothing started", Mask);
        ApolloLogU(Ctx, "strips: %s", s->Why);
        WdfWaitLockRelease(Ctx->DevLock);
        return FALSE;
    }
    x = XactAlloc(Ctx, s->Dsp);
    if (x == NULL) {
        s->State = APOLLO_STRIPS_STATE_FAILED;
        StripsSetWhy(s, "no free transaction buffer for the start: nothing started");
        ApolloLogU(Ctx, "strips: %s", s->Why);
        WdfWaitLockRelease(Ctx->DevLock);
        return FALSE;
    }
    StripsXactFromPlan(x, plan);
    ApolloLogU(Ctx, "strips: start transaction, %lu entries for %lu strip(s) (stage %lu: %s), DSP %lu",
              x->N, n, Limit, StripsStepName(Limit), s->Dsp);
    WdfWaitLockRelease(Ctx->DevLock);

    status = XactRun(Ctx, x);

    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (s->Gen != Gen) {
        ApolloLogU(Ctx, "strips: start: outcome 0x%08x ignored, the strips were turned off meanwhile",
                  status);
    } else if (x->State == XactBuilding) {
        /* never published: nothing started */
        if (s->State == APOLLO_STRIPS_STATE_BUILDING) {
            s->State = APOLLO_STRIPS_STATE_FAILED;
            StripsSetWhy(s, "start transaction not sent (0x%08x): nothing started", status);
            ApolloLogU(Ctx, "strips: %s", s->Why);
        }
    } else {
        /* Published: from here every strip in Mask is stopped at D0 exit, proven or not. */
        for (i = 0; i < APOLLO_STRIP_INPUTS; i++) {
            if (Mask & (1u << i)) {
                s->StripStarted[i] = TRUE;
            }
        }
        if (NT_SUCCESS(status)) {
            ok = TRUE;
            ms = (ULONG)((KeQueryInterruptTime() - x->PublishedAt) / 10000ULL);
            ApolloLogU(Ctx, "strips: %lu strip(s) started together on DSP %lu (start taken after %lu ms)",
                      n, s->Dsp, ms);
            for (i = 0; i < APOLLO_STRIP_INPUTS; i++) {
                if (!(Mask & (1u << i))) {
                    continue;
                }
                s->StripState[i] = APOLLO_STRIP_TAKEN;
                ApolloLogU(Ctx, "strips: input %lu: Mic Input Strip started at 0x%05lx on DSP %lu",
                          i + 1, s->StripHeader[i], s->Dsp);
                if (Limit < STRIP_STEP_OTHERS) {
                    continue;
                }
                /* The strip's DSP mixes the input now (ua_dsp.c:3866-3903): [Monitor] it gets the bus's
                   cached cue sends and [Mix] monitor-mix weights (cues.c; DSP 0 has them already). */
                s->BusOwner[i] = s->Dsp;
                cached = ApolloCuesBusMoved(Ctx, i);
                sent = 0;
                for (d = 0; d < Ctx->NumDsps && d < APOLLO_MAX_DSPS; d++) {
                    NTSTATUS st;

                    /* [Twin X G5] every DSP but the strip DSP (x8p 0, 2-5; Twin X Duo 0), as before */
                    if ((StripRouteDsps(Ctx->NumDsps, s->Dsp) & (1u << d)) == 0 ||
                        !Ctx->Cmd[d].Allocated || !Ctx->Cmd[d].Live) {
                        continue;
                    }
                    st = StripsSendInline(Ctx, d, STRIP_CMD_INPUT, i, STRIP_INPUT_ROUTE,
                                          STRIP_ROUTE_NONE);
                    if (NT_SUCCESS(st)) {
                        sent++;
                    } else {
                        ApolloLogU(Ctx, "strips: input %lu: route word to DSP %lu not queued (0x%08x)",
                                  i + 1, d, st);
                    }
                }
                s->RoutesSent += sent;
                ApolloLogU(Ctx, "strips: input %lu: bus %lu now mixed on DSP %lu (%lu cached mix/cue send(s) "
                          "re-sent); route 0x1ffff sent to %lu other DSP(s)", i + 1, i, s->Dsp, cached,
                          sent);
            }
        } else if (!s->Quarantined && s->State == APOLLO_STRIPS_STATE_BUILDING) {
            s->State = APOLLO_STRIPS_STATE_FAILED;
            StripsSetWhy(s, "start: device gone (0x%08x)", status);
        }
        /* An unproven transaction already quarantined the DSP (XactRun). */
    }
    XactRelease(Ctx, x);
    if (s->Gen != Gen) {
        ok = FALSE;
    }
    WdfWaitLockRelease(Ctx->DevLock);
    return ok;
}

/*
 * DevLock held: the 48V write went out in a batch after Seq and the DSP took that batch.
 * Flushes itself rather than wait for the service thread (a D0 exit stops that first).
 */
static BOOLEAN StripsMailboxCommitted(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Seq)
{
    ApolloMixerFlush(Ctx);
    return Ctx->MixerSeqWr != Seq && ApolloRead(Ctx, REG_MIXER_SEQ_RD) == Ctx->MixerSeqWr;
}

static BOOLEAN StripsMailboxWait(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Seq, _In_ ULONG TimeoutMs)
{
    ULONG waited;
    BOOLEAN done = FALSE;

    for (waited = 0; !done && waited < TimeoutMs; waited += 10) {
        ApolloSleepMs(10);
        WdfWaitLockAcquire(Ctx->DevLock, NULL);
        done = StripsMailboxCommitted(Ctx, Seq);
        WdfWaitLockRelease(Ctx->DevLock);
    }
    return done;
}

/* The 48V bit of preamp Ch (0-based): setting S0 for 1-4, S12 for 5-8, bit 6*(ch&3)+3. */
static ULONG StripsPhantomSetting(_In_ ULONG Ch)
{
    return (Ch < 4) ? 0 : 12;
}

static ULONG StripsPhantomBit(_In_ ULONG Ch)
{
    return 1u << (6 * (Ch & 3) + 3);
}

/*
 * DevLock held, MixerReady. The resync's own 48V write, straight into the mailbox cache as
 * ApolloMixerWriteSetting would do it (mixer.c), but without that function's host-write hook:
 * ApolloStripsNoteSettingWrite must see only other writers.
 */
static VOID StripsWritePhantom(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Ch, _In_ BOOLEAN On)
{
    ULONG idx = StripsPhantomSetting(Ch), bit = StripsPhantomBit(Ch);

    Ctx->MixerVal[idx] = (Ctx->MixerVal[idx] & ~bit) | (On ? bit : 0);
    Ctx->MixerMask[idx] |= bit;
    Ctx->MixerDirty = TRUE;
}

/*
 * mixer.c ApolloMixerWriteSetting, DevLock held: a host write (SET_INPUT_PARAM, a raw setting)
 * to a 48V bit while the resync has it off. The resync then leaves that preamp as the host set
 * it instead of switching it back on (docs/STRIPS.md 10.7).
 */
VOID ApolloStripsNoteSettingWrite(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Index, _In_ ULONG Mask)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    ULONG ch;

    if (s->PhantomState != STRIP_PHANTOM_RUNNING) {
        return;
    }
    for (ch = 0; ch < APOLLO_STRIP_INPUTS; ch++) {
        if ((s->PhantomMask & (1u << ch)) && Index == StripsPhantomSetting(ch) &&
            (Mask & StripsPhantomBit(ch))) {
            s->PhantomMask &= ~(1u << ch);
            ApolloLogU(Ctx, "strips: 48V of preamp %lu set by the host during the resync: left as set",
                      ch + 1);
        }
    }
}

/*
 * DevLock held. One hardware readback, only if it is a real one: UA's Mixer Engine and
 * open-apollo drop a snapshot whose word 38 or 39 reads all-ones (hardware.py:5265-5269,
 * ua_core.c:2716-2718); the flag words in use (0 for preamps 1-4, 5 for 5-8) must not either.
 * *On = the preamps in Mask the snapshot reports with 48V on.
 */
static BOOLEAN StripsPhantomSnapshot(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Mask, _Out_ PULONG On,
                                     _Out_ PULONG Seq)
{
    ULONG ch, on = 0;

    *On = 0;
    *Seq = Ctx->ReadbackSeq;
    if (!Ctx->MixerReady || !Ctx->ReadbackValid ||
        Ctx->Readback[38] == 0xFFFFFFFFu || Ctx->Readback[39] == 0xFFFFFFFFu ||
        ((Mask & 0x0F) && Ctx->Readback[0] == 0xFFFFFFFFu) ||
        ((Mask & 0xF0) && Ctx->Readback[5] == 0xFFFFFFFFu)) {
        return FALSE;
    }
    for (ch = 0; ch < APOLLO_STRIP_INPUTS; ch++) {
        ULONG word = (ch < 4) ? Ctx->Readback[0] : Ctx->Readback[5];

        if ((Mask & (1u << ch)) && (word & StripsPhantomBit(ch))) {
            on |= 1u << ch;
        }
    }
    *On = on;
    return TRUE;
}

static VOID StripsPhantomEnd(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Gen, _In_ ULONG State)
{
    APOLLO_STRIPS *s = &Ctx->Strips;

    s->PhantomState = State;
    if (s->Gen == Gen) {
        s->StepsDone = STRIP_STEP_PHANTOM;
    }
}

/*
 * StripLock held, DevLock not. Port of ua_preamp_phantom_resync (ua_core.c:2475-2499),
 * extended to preamps 5-8: after a cold start the phantom supply comes up only on a written
 * 0 -> 1 edge. Guards (docs/STRIPS.md 5.6, 10.7):
 *   - only preamps in Mask that TWO consecutive valid readbacks both report with 48V on;
 *     phantom is never switched on where it was off;
 *   - skipped when a D0 exit is under way;
 *   - once the off is written the on is always written (unless the host changed that preamp's
 *     48V meanwhile), and flushed for up to 2 s.
 */
static VOID StripsPhantomResync(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Mask, _In_ ULONG Gen)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    ULONG ch, on = 0, on2 = 0, seq = 0, seq2 = 0, onSet, waited;
    BOOLEAN ok, ok2 = FALSE, offDone, onDone;

    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (s->Gen != Gen) {
        WdfWaitLockRelease(Ctx->DevLock);
        return;
    }
    if (ReadAcquire(&Ctx->StopRequested)) {
        StripsPhantomEnd(Ctx, Gen, APOLLO_STRIPS_PHANTOM_NOT_RUN);
        ApolloLogU(Ctx, "strips: 48V resync skipped: the device is stopping");
        WdfWaitLockRelease(Ctx->DevLock);
        return;
    }
    ok = StripsPhantomSnapshot(Ctx, Mask, &on, &seq);
    WdfWaitLockRelease(Ctx->DevLock);

    /* A second snapshot, newer than the first, must say the same. */
    for (waited = 0; ok && waited < STRIP_READBACK_WAIT_MS && !ReadAcquire(&Ctx->StopRequested);
         waited += 10) {
        ApolloSleepMs(10);
        WdfWaitLockAcquire(Ctx->DevLock, NULL);
        if (Ctx->ReadbackSeq != seq) {
            ok2 = StripsPhantomSnapshot(Ctx, Mask, &on2, &seq2);
            WdfWaitLockRelease(Ctx->DevLock);
            break;
        }
        WdfWaitLockRelease(Ctx->DevLock);
    }

    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (!ok || !ok2 || on != on2) {
        StripsPhantomEnd(Ctx, Gen, APOLLO_STRIPS_PHANTOM_NO_READBACK);
        ApolloLogU(Ctx, "strips: 48V resync skipped: no two matching valid readbacks (first %s, second %s, "
                  "48V 0x%02lx / 0x%02lx); nothing written", ok ? "valid" : "invalid",
                  ok2 ? "valid" : "invalid or missing", on, on2);
        WdfWaitLockRelease(Ctx->DevLock);
        return;
    }
    if (on == 0) {
        StripsPhantomEnd(Ctx, Gen, APOLLO_STRIPS_PHANTOM_NONE_ON);
        ApolloLogU(Ctx, "strips: 48V resync: no preamp in StripMask 0x%02lx reports 48V on: nothing written",
                  Mask);
        WdfWaitLockRelease(Ctx->DevLock);
        return;
    }
    if (ReadAcquire(&Ctx->StopRequested) || !Ctx->MixerReady) {
        StripsPhantomEnd(Ctx, Gen, APOLLO_STRIPS_PHANTOM_NOT_RUN);
        ApolloLogU(Ctx, "strips: 48V resync skipped: the device is stopping");
        WdfWaitLockRelease(Ctx->DevLock);
        return;
    }
    /* A host 48V write still queued in the mailbox is newer than any readback: leave that preamp. */
    for (ch = 0; ch < APOLLO_STRIP_INPUTS; ch++) {
        if ((on & (1u << ch)) &&
            (Ctx->MixerMask[StripsPhantomSetting(ch)] & StripsPhantomBit(ch))) {
            on &= ~(1u << ch);
            ApolloLogU(Ctx, "strips: 48V of preamp %lu has a host write queued: left as set", ch + 1);
        }
    }
    if (on == 0) {
        StripsPhantomEnd(Ctx, Gen, APOLLO_STRIPS_PHANTOM_NONE_ON);
        WdfWaitLockRelease(Ctx->DevLock);
        return;
    }
    /* Off. From here the on is owed, whatever happens. */
    s->PhantomState = STRIP_PHANTOM_RUNNING;
    s->PhantomMask = on;
    for (ch = 0; ch < APOLLO_STRIP_INPUTS; ch++) {
        if (on & (1u << ch)) {
            StripsWritePhantom(Ctx, ch, FALSE);
        }
    }
    seq = Ctx->MixerSeqWr;
    ApolloLogU(Ctx, "strips: 48V resync: 48V off on preamp mask 0x%02lx", on);
    WdfWaitLockRelease(Ctx->DevLock);

    offDone = StripsMailboxWait(Ctx, seq, STRIP_OFF_COMMIT_MS);
    ApolloSleepMs(STRIP_PHANTOM_OFF_MS);

    /* On, except where the host set that preamp's 48V meanwhile (PhantomMask lost its bit). */
    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    onSet = s->PhantomMask;
    for (ch = 0; ch < APOLLO_STRIP_INPUTS; ch++) {
        if (onSet & (1u << ch)) {
            StripsWritePhantom(Ctx, ch, TRUE);
        }
    }
    seq = Ctx->MixerSeqWr;
    WdfWaitLockRelease(Ctx->DevLock);

    onDone = onSet ? StripsMailboxWait(Ctx, seq, STRIP_ON_COMMIT_MS) : TRUE;

    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    s->PhantomMask = onSet;
    StripsPhantomEnd(Ctx, Gen, (offDone && onDone) ? APOLLO_STRIPS_PHANTOM_DONE :
                                                     APOLLO_STRIPS_PHANTOM_NOT_COMMITTED);
    if (!onDone) {
        ApolloLogU(Ctx, "strips: 48V ON for preamp mask 0x%02lx was not taken by the mailbox within %u ms: "
                  "it stays queued; check 'apollo-diag readback'", onSet, STRIP_ON_COMMIT_MS);
    }
    ApolloLogU(Ctx, "strips: 48V re-applied after the cold start (preamp mask 0x%02lx%s)%s", onSet,
              (onSet != on) ? ", others left as the host set them" : "",
              offDone ? "" : "; the off was not taken in time, so there may have been no edge");
    WdfWaitLockRelease(Ctx->DevLock);
}

/* ------------------------------------------------------------------ */
/* The live probe and its repair ladder (docs/STRIPS.md 10.9)           */
/* ------------------------------------------------------------------ */

#define STRIP_PROBE_READS       8u      /* level windows per measurement (~8k frames in all) */
#define STRIP_PROBE_GAP_MS      25u
#define STRIP_PROBE_SLICE_MS    50u

/* DevLock held: the pass this probe belongs to is still the live one. */
static BOOLEAN StripsProbeCurrent(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Gen)
{
    APOLLO_STRIPS *s = &Ctx->Strips;

    /* Connected/Stage too: the service pass can see an all-ones link before the gate closes. */
    return s->Gen == Gen && s->State == APOLLO_STRIPS_STATE_LIVE && !s->Quarantined &&
           !ReadAcquire(&Ctx->StopRequested) && !ApolloGateClosed(Ctx) &&
           Ctx->Connected && Ctx->Stage != ApolloStageGone;
}

/* StripLock held, DevLock not. Sleeps Ms in slices; FALSE as soon as the pass is not current. */
static BOOLEAN StripsProbeSleep(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Gen, _In_ ULONG Ms)
{
    ULONG waited = 0, step;
    BOOLEAN ok = TRUE;

    while (ok && waited < Ms) {
        step = min(STRIP_PROBE_SLICE_MS, Ms - waited);
        ApolloSleepMs(step);
        waited += step;
        WdfWaitLockAcquire(Ctx->DevLock, NULL);
        ok = StripsProbeCurrent(Ctx, Gen);
        WdfWaitLockRelease(Ctx->DevLock);
    }
    return ok;
}

/*
 * StripLock held, DevLock not. After SettleMs, reads the capture ring STRIP_PROBE_READS times
 * (ApolloLevelsRead with DevLock held, as GET_LEVELS does: StripLock -> DevLock -> EngineLock)
 * and keeps each record slot's largest peak. Strip input i records into slot i (x8p legacy
 * layout; [Twin X G5] the Twin X's descriptor too: 0x0001/0x0002 at slots 0/1, strip_plan.h
 * RecId). An open input carries noise, so a live strip never reads exactly zero over ~8k
 * frames. TRUE = measured; *Dead = the inputs in Mask whose slot never left zero.
 */
static BOOLEAN StripsProbeMeasure(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Mask, _In_ ULONG Gen,
                                  _In_ ULONG SettleMs, _In_z_ PCSTR After, _Out_ PULONG Dead)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    APOLLO_LEVELS lv;
    ULONG peak[STRIP_PLAN_INPUTS], meter[4], r, i;
    BOOLEAN valid = FALSE, rb = FALSE;

    *Dead = 0;
    RtlZeroMemory(peak, sizeof(peak));
    RtlZeroMemory(meter, sizeof(meter));
    if (!StripsProbeSleep(Ctx, Gen, SettleMs)) {
        return FALSE;
    }
    for (r = 0; r < STRIP_PROBE_READS; r++) {
        if (r > 0 && !StripsProbeSleep(Ctx, Gen, STRIP_PROBE_GAP_MS)) {
            return FALSE;
        }
        WdfWaitLockAcquire(Ctx->DevLock, NULL);
        if (!StripsProbeCurrent(Ctx, Gen)) {
            WdfWaitLockRelease(Ctx->DevLock);
            return FALSE;
        }
        ApolloLevelsRead(Ctx, &lv);
        if (lv.Valid && lv.Channels >= STRIP_PLAN_INPUTS) {
            valid = TRUE;
            for (i = 0; i < STRIP_PLAN_INPUTS; i++) {
                peak[i] = max(peak[i], lv.Peak[i]);
            }
        }
        if (Ctx->ReadbackValid) {
            rb = TRUE;
            for (i = 0; i < 4; i++) {
                meter[i] = Ctx->Readback[8 + i];    /* the DSP's input meters, inputs 1-8 */
            }
        }
        WdfWaitLockRelease(Ctx->DevLock);
    }

    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (!valid) {
        ApolloLogU(Ctx, "strips: probe: no capture data (transport not running): nothing measured");
        WdfWaitLockRelease(Ctx->DevLock);
        return FALSE;
    }
    *Dead = StripProbeDead(Mask, peak);
    s->ProbeDead = *Dead;
    s->ProbeMeasured++;
    ApolloLogU(Ctx, "strips: probe %lu (%s): StripMask 0x%02lx live 0x%02lx, exact zero 0x%02lx",
              s->ProbeMeasured, After, Mask, Mask & ~*Dead, *Dead);
    ApolloLogU(Ctx, "strips: probe %lu: slot peaks %06lx %06lx %06lx %06lx %06lx %06lx %06lx %06lx; "
              "meters %08lx %08lx %08lx %08lx%s", s->ProbeMeasured, peak[0], peak[1], peak[2],
              peak[3], peak[4], peak[5], peak[6], peak[7], meter[0], meter[1], meter[2], meter[3],
              rb ? "" : " (no readback)");
    WdfWaitLockRelease(Ctx->DevLock);
    return TRUE;
}

/*
 * StripLock held, DevLock not. One inline-only transaction on the strip DSP from a plan,
 * waited for by command-ring consumption; an unproven one quarantines the DSP (XactRun).
 * Kind 0 = STOP (StripPlanStop), 1 = START (StripPlanStart), 2 = SPANS (StripPlanSpans).
 */
static BOOLEAN StripsInlineXact(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Kind, _In_ ULONG Mask,
                                _In_ ULONG Limit, _In_ ULONG Gen, _In_z_ PCSTR Tag)
{
    static const PCSTR kName[3] = { "stop", "start", "span" };
    APOLLO_STRIPS *s = &Ctx->Strips;
    STRIP_PLAN *plan = (STRIP_PLAN *)s->PlanScratch;   /* StripLock serialises its users */
    PAPOLLO_XACT x;
    NTSTATUS status;
    ULONG err, ms;
    BOOLEAN ok = FALSE;

    if (Kind > 2) {
        return FALSE;
    }
    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (!StripsProbeCurrent(Ctx, Gen)) {
        WdfWaitLockRelease(Ctx->DevLock);
        return FALSE;
    }
    err = (Kind == 0) ? StripPlanStop(Mask, plan) :
          (Kind == 1) ? StripPlanStart(Mask, Limit, plan) : StripPlanSpans(plan);
    if (err != 0 || plan->N == 0 || plan->N > STRIP_PLAN_ENTS_MAX || plan->Len != 0) {
        ApolloLogU(Ctx, "strips: %s: planner error for the %s transaction: nothing sent", Tag, kName[Kind]);
        WdfWaitLockRelease(Ctx->DevLock);
        return FALSE;
    }
    x = XactAlloc(Ctx, s->Dsp);
    if (x == NULL) {
        ApolloLogU(Ctx, "strips: %s: no free transaction buffer for the %s transaction: nothing sent",
                  Tag, kName[Kind]);
        WdfWaitLockRelease(Ctx->DevLock);
        return FALSE;
    }
    StripsXactFromPlan(x, plan);
    s->ProbeRepairs++;
    ApolloLogU(Ctx, "strips: %s: %s transaction, %lu entries, StripMask 0x%02lx, DSP %lu",
              Tag, kName[Kind], x->N, Mask, s->Dsp);
    WdfWaitLockRelease(Ctx->DevLock);

    status = XactRun(Ctx, x);

    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (x->State == XactRetired && NT_SUCCESS(status)) {
        ms = (ULONG)((KeQueryInterruptTime() - x->PublishedAt) / 10000ULL);
        ApolloLogU(Ctx, "strips: %s: %s transaction taken after %lu ms", Tag, kName[Kind], ms);
        ok = s->Gen == Gen;
    } else if (x->State == XactBuilding) {
        ApolloLogU(Ctx, "strips: %s: %s transaction not sent (0x%08x)", Tag, kName[Kind], status);
    }
    /* An unproven transaction already quarantined the DSP (XactRun). */
    XactRelease(Ctx, x);
    WdfWaitLockRelease(Ctx->DevLock);
    return ok;
}

static BOOLEAN StripsProbeXact(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Kind, _In_ ULONG Mask,
                               _In_ ULONG Limit, _In_ ULONG Gen)
{
    return StripsInlineXact(Ctx, Kind, Mask, Limit, Gen, "probe");
}

/*
 * StripLock held, DevLock not. Span none for analog inputs 1-8 on every live DSP: on the strip
 * DSP as a waited transaction, on the others fire and forget like the stage-8 route words
 * (ua_dsp.c:4035-4065). TRUE = the strip DSP took its part (the others cannot be proven).
 */
static BOOLEAN StripsProbeSpans(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Gen)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    ULONG d, i, sent = 0, failed = 0;

    if (!StripsProbeXact(Ctx, 2, 0, 0, Gen)) {
        return FALSE;
    }
    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (!StripsProbeCurrent(Ctx, Gen)) {
        WdfWaitLockRelease(Ctx->DevLock);
        return FALSE;
    }
    for (d = 0; d < Ctx->NumDsps && d < APOLLO_MAX_DSPS; d++) {
        if (d == s->Dsp || !Ctx->Cmd[d].Allocated || !Ctx->Cmd[d].Live) {
            continue;
        }
        for (i = 0; i < STRIP_PLAN_INPUTS; i++) {
            if (NT_SUCCESS(StripsSendInline(Ctx, d, STRIP_CMD_INPUT, i, STRIP_INPUT_SPAN,
                                            STRIP_SPAN_NONE))) {
                sent++;
            } else {
                failed++;
            }
        }
    }
    ApolloLogU(Ctx, "strips: probe: span none for inputs 1-8 sent to %lu other DSP word(s)%s", sent,
              failed ? " (some not queued)" : "");
    WdfWaitLockRelease(Ctx->DevLock);
    return TRUE;
}

/*
 * StripLock held, DevLock not. After a pass that started strips: measure, and in StripProbe 2
 * walk g_StripProbe while a strip input reads exact zero, measuring after every repair. The
 * repairs are UA's own words (strip_plan.h) and run only after the full chain (stage 9), on
 * strips this pass started. Ends at the first measurement with every strip input live.
 */
static VOID StripsProbe(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Mask, _In_ ULONG Limit, _In_ ULONG Gen)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    const STRIP_PROBE_STEP *step;
    ULONG mode, k, dead = 0, first = 0, resynchs = 0, stuck = 0;
    BOOLEAN spans = FALSE, measured = FALSE;
    PCSTR after = "as built";

    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    mode = s->Probe;
    if (mode == STRIP_PROBE_REPAIR && Limit < STRIP_STEP_ALL) {
        mode = STRIP_PROBE_MEASURE_ONLY;        /* repairs only after the full chain */
    }
    if (mode == STRIP_PROBE_OFF || Limit < STRIP_STEP_SYNCH || Mask == 0 ||
        !StripsProbeCurrent(Ctx, Gen)) {
        WdfWaitLockRelease(Ctx->DevLock);
        return;
    }
    ApolloLogU(Ctx, "strips: probe: measuring StripMask 0x%02lx in %u ms (StripProbe=%lu: %s)", Mask,
              STRIP_PROBE_SETTLE_MS, mode,
              mode == STRIP_PROBE_REPAIR ? "repairs while an input reads zero" : "measure only");
    WdfWaitLockRelease(Ctx->DevLock);

    for (k = 0; k < STRIP_PROBE_STEPS; k++) {
        step = StripProbeStep(mode, k);
        if (step->Kind == STRIP_PROBE_MEASURE) {
            if (!StripsProbeMeasure(Ctx, Mask, Gen, step->Arg, after, &dead)) {
                return;
            }
            if (!measured) {
                first = dead;
                measured = TRUE;
            }
            if (dead == 0) {
                break;
            }
            if (resynchs == 0 && !spans) {
                after = "as built, later";
            }
        } else if (step->Kind == STRIP_PROBE_RESYNCH) {
            /*
             * Stop + start only the dead inputs: the live ones keep running (no dropout,
             * and they stay live controls), as UA's per-strip restore (ua_strips.c:1101-1132).
             */
            ULONG redo = dead & Mask;

            if (redo == 0 || !StripsProbeXact(Ctx, 0, redo, Limit, Gen)) {
                break;
            }
            if (!StripsProbeXact(Ctx, 1, redo, Limit, Gen)) {
                stuck = redo;                   /* stopped, not restarted: say so below */
                break;
            }
            resynchs++;
            after = spans ? "after spans on every DSP, then stop + start" : "after stop + start";
        } else if (step->Kind == STRIP_PROBE_SPANS) {
            if (!StripsProbeSpans(Ctx, Gen)) {
                break;
            }
            spans = TRUE;
            after = "after span none on every DSP";
        } else {
            break;
        }
    }

    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (stuck != 0 && s->Gen == Gen) {
        ApolloLogU(Ctx, "strips: probe: inputs 0x%02lx were stopped and NOT restarted (the start was not "
                  "taken); power-cycle the Apollo", stuck);
        if (s->State == APOLLO_STRIPS_STATE_LIVE) {
            StripsSetWhy(s, "probe: inputs 0x%02lx stopped and not restarted; power-cycle the Apollo",
                         stuck);
        }
    } else if (measured && s->Gen == Gen) {
        if (dead == 0) {
            ApolloLogU(Ctx, "strips: probe: every strip input live (%s); at the first measurement "
                      "0x%02lx read exact zero", after, first);
            if (s->State == APOLLO_STRIPS_STATE_LIVE) {
                StripsSetWhy(s, "live on StripMask 0x%02lx, DSP %lu; probe: every input live (%s)",
                             Mask, s->Dsp, after);
            }
        } else {
            ApolloLogU(Ctx, "strips: probe: inputs 0x%02lx still read exact zero (%s, %lu repair "
                      "transaction(s))%s", dead, after, s->ProbeRepairs,
                      s->Quarantined ? "; DSP quarantined" : "");
            if (s->State == APOLLO_STRIPS_STATE_LIVE) {
                StripsSetWhy(s, "live on StripMask 0x%02lx, DSP %lu; probe: 0x%02lx exact zero (%s)",
                             Mask, s->Dsp, dead, after);
            }
        }
    }
    WdfWaitLockRelease(Ctx->DevLock);
}

/*
 * [Twin X G5] StripsEnable=2 (dry run), StripLock and DevLock held, every gate passed: the pass
 * this unit would get, built by the same planner and logged, NOT sent. Per input: its DSP input,
 * the record slot of its channel in the live IO descriptor, the instance header and blocks, the
 * PREPARE size, the START words and the DSPs that would get its route word; then the 48V state
 * the resync would act on. Reads only (the readback snapshot the service thread drained).
 */
static VOID StripsLogDryRun(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Mask, _In_ ULONG Limit)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    const STRIP_MODEL *model = StripsModel(Ctx);
    STRIP_PLAN *plan = (STRIP_PLAN *)s->PlanScratch;   /* StripLock serialises its users */
    ULONG i, k, in, prepares = 0, starts = 0, on = 0, seq = 0;
    ULONG routes = StripRouteDsps(Ctx->NumDsps, s->Dsp);
    CHAR name[24];

    if (model == NULL) {
        return;
    }
    for (k = 0; k <= APOLLO_STRIP_INPUTS; k++) {
        ULONG kind = StripPassStep(Mask, Limit, k, &in);

        if (kind == STRIP_XACT_PREPARE) {
            prepares++;
        } else if (kind == STRIP_XACT_START) {
            starts++;
        } else {
            break;
        }
    }
    ApolloLogU(Ctx, "strips: dry run plan (%s): StripMask 0x%02lx (inputs it has 0x%02lx), DSP %lu boot word 0x%08lx, "
              "pools checked for %lu strip(s); %lu PREPARE + %lu START transaction(s) to stage %lu",
              model->Name, Mask, model->MaskAllowed, s->Dsp, s->BootWord[s->Dsp & (APOLLO_MAX_DSPS - 1)],
              model->LayoutStrips, prepares, starts, Limit);
    ApolloLogU(Ctx, "strips: dry run plan: modules 0x%03x@0x%05lx 0x%03x@0x%05lx 0x%03x@0x%05lx 0x%03x@0x%05lx "
              "0x%03x@0x%05lx to DSP %lu, each waits for {80070004 0 id x}",
              g_ApolloStripPins[0].Id, g_ApolloStripPins[0].Dest, g_ApolloStripPins[1].Id, g_ApolloStripPins[1].Dest,
              g_ApolloStripPins[2].Id, g_ApolloStripPins[2].Dest, g_ApolloStripPins[3].Id, g_ApolloStripPins[3].Dest,
              g_ApolloStripPins[4].Id, g_ApolloStripPins[4].Dest, s->Dsp);
    for (i = 0; i < APOLLO_STRIP_INPUTS; i++) {
        USHORT id = model->RecId[i];
        LONG slot;

        if (!(Mask & (1u << i))) {
            continue;
        }
        slot = (id != 0) ? ApolloIoDescFind(Ctx->IoDescIn, 1, id) : -1;
        name[0] = '\0';
        (VOID)ApolloChanName(id, name, sizeof(name));
        if (StripPlanPrepareRate(i, Limit, Ctx->SampleRate, plan) != 0) {     /* [Rate] */
            ApolloLogU(Ctx, "strips: dry run plan: input %lu: planner error", i + 1);
            continue;
        }
        ApolloLogU(Ctx, "strips: dry run plan: input %lu = DSP input %lu -> record slot %ld (%04lx %s); header 0x%05lx, "
                  "m0 0x%05lx, m1 0x%05lx, c0 0x%08lx; PREPARE %lu entries, %lu payload dwords", i + 1, i, slot,
                  (ULONG)id, name, plan->Addr[0], plan->Addr[1], plan->Addr[2], plan->Addr[6], plan->N, plan->Len);
        if (Limit >= STRIP_STEP_SYNCH) {
            ApolloLogU(Ctx, "strips: dry run plan: input %lu START {%08x %08lx %08lx 0}%s; route 0x1ffff to DSP mask "
                      "0x%02lx", i + 1, STRIP_CMD_SYNCH, (ULONG)((STRIP_SYNCH_MONO << 16) | i), plan->Addr[0],
                      Limit >= STRIP_STEP_INPUT ? " + span, span, route 0x1ffff" : "",
                      Limit >= STRIP_STEP_OTHERS ? routes : 0);
        }
    }
    if (Limit < STRIP_STEP_PHANTOM) {
        ApolloLogU(Ctx, "strips: dry run plan: no 48V resync below stage %u", STRIP_STEP_PHANTOM);
    } else if (StripsPhantomSnapshot(Ctx, Mask, &on, &seq)) {
        ApolloLogU(Ctx, "strips: dry run plan: 48V resync (off 200 ms, on) for the preamps that read 48V on: 0x%02lx now",
                  on);
    } else {
        ApolloLogU(Ctx, "strips: dry run plan: 48V resync: no valid readback yet (it needs two matching ones)");
    }
}

/* ------------------------------------------------------------------ */
/* [Warm] The warm-start decision (docs/STRIPS.md 12, strip_adopt.h)   */
/* ------------------------------------------------------------------ */

_IRQL_requires_(PASSIVE_LEVEL)
static BOOLEAN StripsTeardownXact(_In_ PDEVICE_CONTEXT Ctx, _In_z_ PCSTR Why, _In_ BOOLEAN Record);  /* below */

#define WARM_DONE       0u  /* decided: off, adopted or a dry run; the pass ends here */
#define WARM_BUILD      1u  /* a rebuild: the pass goes on and builds */
#define WARM_RESTORE    2u  /* adopted; the plain strips are SYNCHed again next (DevLock released) */
#define WARM_WAIT       3u  /* no valid readback yet: armed again */

/*
 * StripLock and DevLock held: this start's side of StripAdoptDecide (the pools read fresh, the plan hashed
 * in PlanScratch, which StripLock serialises). *Teardown: the recorded strips must be stopped first.
 */
static ULONG StripsAdoptDecideNow(_In_ PDEVICE_CONTEXT Ctx, _Out_ int *Teardown, _Out_ STRIP_ADOPT_NOW *Now)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    STRIP_PLAN *plan = (STRIP_PLAN *)s->PlanScratch;

    RtlZeroMemory(Now, sizeof(*Now));
    StripsReadLayout(Ctx);
    Now->Anchor = s->Anchor;
    Now->ConnectsOnWarm = StripsConnectsOnWarm(Ctx) ? 1u : 0u;
    Now->Adopt = s->AdoptParam;
    Now->WarmRebuild = s->WarmRebuild;
    Now->Dsp = s->Dsp;
    Now->Mask = s->Mask & ((1u << APOLLO_STRIP_INPUTS) - 1);
    Now->Rate = Ctx->SampleRate;
    Now->Limit = StripsStepLimit(s->Steps);
    Now->ModuleHash = StripsModulesHash();
    Now->PlanHash = StripPlanHash(Now->Mask, Now->Limit, Now->Rate, Now->Dsp, Ctx->NumDsps, plan);
    Now->LayoutOk = (s->LayoutRead && s->LayoutWhy == 0) ? 1u : 0u;
    RtlCopyMemory(Now->Geom, s->Geom, sizeof(Now->Geom));
    return StripAdoptDecide(s->RecFound ? &s->Rec : NULL, Now, Teardown);
}

/* The Why line (96 chars) for a warm start that adopts nothing. */
static PCSTR StripsWarmWhy(_In_ ULONG Verdict, _In_ ULONG Anchor)
{
    if (Verdict == STRIP_ADOPT_NOT_ANCHORED) {
        switch (Anchor) {
        case STRIP_ANCHOR_NO_RECORD:   return "warm start: unsigned strips (no record): power-cycle once; later restarts keep them";
        case STRIP_ANCHOR_OTHER_HOST:  return "warm start: another host used the DSPs since: power-cycle the Apollo";
        case STRIP_ANCHOR_LOST_BEFORE: return "warm start: DSP state unknown since an earlier start: power-cycle the Apollo";
        case STRIP_ANCHOR_BAD_RECORD:  return "warm start: the record is unreadable: power-cycle the Apollo";
        case STRIP_ANCHOR_OTHER_UNIT:  return "warm start: the record is another unit's or firmware's: power-cycle";
        case STRIP_ANCHOR_NO_ADVANCE:  return "warm start: the connect counter stopped counting: power-cycle the Apollo";
        default:                       return "warm start: no record kept for this unit: power-cycle the Apollo";
        }
    }
    switch (Verdict) {
    case STRIP_ADOPT_NOT_RUNNING:   return "warm start: none of this driver's strips run: power-cycle (or StripWarmRebuild=1)";
    case STRIP_ADOPT_UNKNOWN_STATE: return "warm start: the last session ended mid-pass: power-cycle (or StripWarmRebuild=1)";
    case STRIP_ADOPT_TORN_DOWN:     return "warm start: the last exit tore them down: power-cycle (or StripWarmRebuild=1)";
    case STRIP_ADOPT_PLAN_DIFFERS:  return "warm start: the running strips are not this plan: power-cycle (or StripWarmRebuild=1)";
    case STRIP_ADOPT_POOLS_DIFFER:  return "warm start: the strip DSP's pools moved: power-cycle the Apollo";
    case STRIP_ADOPT_DISABLED:      return "warm start: StripAdopt is 0: power-cycle the Apollo for input strips";
    case STRIP_ADOPT_DROPPED:       return "warm start: the warm connect dropped them: power-cycle (or StripWarmRebuild=1)";
    case STRIP_ADOPT_LAYOUT:        return "warm start: the strip DSP's pools do not fit the recipe: nothing sent";
    default:                        return "warm start: power-cycle the Apollo for input strips";
    }
}

/*
 * DevLock held (StripLock too): the strips the record names are this driver's and run as planned. They
 * become Live exactly as if this start had built them: modules resident, every strip of the mask taken
 * and started at its header, the buses on the strip DSP (their cached mix and cue sends follow them, as
 * after a build: cues.c ApolloCuesBusMoved; a fresh start has none cached), no 48V resync (that fixes a
 * power-up, and there was none). Nothing goes to the DSP. RESTORE: the caller SYNCHes them again next.
 */
static VOID StripsAdoptApply(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Verdict)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    const STRIP_RECORD *r = &s->Rec;
    ULONG mask = r->Mask & ((1u << APOLLO_STRIP_INPUTS) - 1), limit = r->Limit, i, j, n = 0, cached = 0;
    CHAR built[32];

    s->AdoptPending = FALSE;
    s->Owned = TRUE;
    s->Adopted = TRUE;
    s->PhantomPending = FALSE;
    s->PhantomState = APOLLO_STRIPS_PHANTOM_NOT_RUN;
    s->PhantomMask = 0;
    s->RoutesSent = 0;
    for (j = 0; j < APOLLO_STRIP_MODULES; j++) {
        s->ModState[j] = (limit >= STRIP_STEP_UPLOADS) ? APOLLO_STRIP_MOD_RESIDENT : APOLLO_STRIP_MOD_NONE;
        RtlZeroMemory(s->ModReply[j], sizeof(s->ModReply[j]));
    }
    for (i = 0; i < APOLLO_STRIP_INPUTS; i++) {
        s->StripAckMs[i] = 0;
        s->StripHeader[i] = (mask & (1u << i)) ? r->Header[i] : 0;
        if (mask & (1u << i)) {
            s->StripState[i] = APOLLO_STRIP_TAKEN;
            s->StripStarted[i] = (limit >= STRIP_STEP_SYNCH) ? TRUE : FALSE;
            n++;
        }
    }
    s->StepsDone = min(limit, (ULONG)STRIP_STEP_OTHERS);
    s->State = APOLLO_STRIPS_STATE_LIVE;
    if (limit >= STRIP_STEP_OTHERS) {
        for (i = 0; i < APOLLO_STRIP_INPUTS; i++) {
            if (mask & (1u << i)) {
                s->BusOwner[i] = s->Dsp;
                cached += ApolloCuesBusMoved(Ctx, i);
            }
        }
    }
    /*
     * Plug-ins on the adopted strip DSP (docs/STRIPS.md 12.5): the record proves only this driver used the DSPs since
     * the last power-up (the verdict needs the anchor) and saw no plug-in left there, so the room beside the strips is
     * empty, as after a cold start; StripWarmPlugins allows it. No ring connect at this start (it would drop the
     * strips): the DSPs' session byte reads 0x07, as our last D0 exit left it. The licence query on DSP 0 is answered
     * in that state (hardware, Nov 15 2026); the first plug-in upload to the strip DSP proves uploads there
     * (ApolloStripsUploadProven), and an unanswered one quarantines it as any unproven transaction does.
     */
    s->PlugAdoptOk = (Verdict == STRIP_ADOPT_ADOPTED && StripWarmPluginsOk(Verdict, r->Plugins, s->WarmPlugins)) ? TRUE : FALSE;
    s->WarmUploads = s->PlugAdoptOk ? (Ctx->DspsConnected ? STRIPS_WARM_UPLOADS_PROVEN : STRIPS_WARM_UPLOADS_UNPROVEN) :
                     STRIPS_WARM_UPLOADS_NA;
    StripsDateText(r->BuiltDate, built, sizeof(built));
    StripsSetWhy(s, "warm start: adopted %lu strip(s) built %s; nothing sent", n, built);
    ApolloLogU(Ctx, "strips: adopted %lu strip(s) this driver left running (StripMask 0x%02lx, DSP %lu, %lu Hz, stage %lu, "
              "built %s, adoption %lu): nothing sent", n, mask, s->Dsp, r->Rate, limit, built, r->Adoptions + 1);
    for (i = 0; i < APOLLO_STRIP_INPUTS; i++) {
        if (mask & (1u << i)) {
            ApolloLogU(Ctx, "strips: input %lu: Mic Input Strip at 0x%05lx on DSP %lu, adopted", i + 1,
                      s->StripHeader[i], s->Dsp);
        }
    }
    if (limit >= STRIP_STEP_OTHERS) {
        ApolloLogU(Ctx, "strips: buses of StripMask 0x%02lx mixed on DSP %lu (%lu cached mix/cue send(s) re-sent)", mask,
                  s->Dsp, cached);
    }
    ApolloLogU(Ctx, "strips: plug-ins on DSP %lu after this adoption: %s", s->Dsp,
              s->PlugAdoptOk ? "allowed (the record proves the DSPs ours since power-up and the room beside the strips "
                               "empty; the first upload proves the warm session takes uploads)" :
              r->Plugins != 0 ? "a power cycle first (a plug-in was on this DSP when the record was last written)" :
              !s->WarmPlugins ? "a power cycle first (StripWarmPlugins=0)" : "a power cycle first");
    s->Cur.State = STRIP_REC_RUNNING;
    s->Cur.Keep = (s->AdoptParam && !StripsConnectsOnWarm(Ctx)) ? 1u : 0u;
    s->Cur.Adoptions = r->Adoptions + 1;
    (VOID)StripsRecWrite(Ctx, Verdict == STRIP_ADOPT_RESTORE ? "adopted, plain strips to restore" : "adopted");
}

/*
 * StripLock and DevLock held, a warm start's first pass after the settle and the rate gate: adopt,
 * rebuild, or off, with the reason. Readback sanity first: the unit must give a real hardware readback
 * (words 38/39 not all ones), waited for up to STRIP_ADOPT_WAITS settle periods.
 */
static ULONG StripsWarmDecide(_In_ PDEVICE_CONTEXT Ctx)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    STRIP_ADOPT_NOW now;
    ULONG v, dsp = s->Dsp, i, j;
    int teardown = 0;
    CHAR built[32];

    if (!Ctx->ReadbackValid || Ctx->Readback[38] == 0xFFFFFFFFu || Ctx->Readback[39] == 0xFFFFFFFFu) {
        if (++s->AdoptWaits < STRIP_ADOPT_WAITS) {
            s->ArmedAt = KeQueryInterruptTime();
            return WARM_WAIT;
        }
        s->AdoptPending = FALSE;
        StripsOff(s, "warm start: no valid hardware readback within 10 s: power-cycle the Apollo for strips");
        return WARM_DONE;
    }
    if (s->Quarantined || s->Tainted || !Ctx->Connected || dsp >= Ctx->NumDsps || dsp >= APOLLO_MAX_DSPS ||
        !Ctx->Cmd[dsp].Live || !Ctx->Resp[dsp].Live) {
        s->AdoptPending = FALSE;
        StripsOff(s, "warm start: the strip DSP is quarantined, tainted or not ring-ready: power-cycle the Apollo");
        return WARM_DONE;
    }
    v = StripsAdoptDecideNow(Ctx, &teardown, &now);
    s->AdoptVerdict = v;
    StripsDateText(s->Rec.BuiltDate, built, sizeof(built));
    ApolloLogU(Ctx, "strips: warm start check: anchor %lu (%s); pools %s", s->Anchor, StripAnchorText(s->Anchor),
              now.LayoutOk ? "fit the recipe" : "do NOT fit the recipe");
    ApolloLogU(Ctx, "strips: warm start check: this start StripMask 0x%02lx, %lu Hz, stage %lu, DSP %lu, plan %08lx, "
              "modules %08lx; StripAdopt %lu, StripWarmRebuild %lu", now.Mask, now.Rate, now.Limit, now.Dsp, now.PlanHash,
              now.ModuleHash, now.Adopt, now.WarmRebuild);
    if (s->RecFound) {
        ApolloLogU(Ctx, "strips: warm start check: record strips %s%s, StripMask 0x%02lx, %lu Hz, stage %lu, DSP %lu, "
                  "plan %08lx, modules %08lx, pools %s, %lu plug-in(s), built %s, adopted %lu time(s)",
                  StripsRecStateText(s->Rec.State), s->Rec.Keep ? " (kept at exit)" : "", s->Rec.Mask, s->Rec.Rate,
                  s->Rec.Limit, s->Rec.Dsp, s->Rec.PlanHash, s->Rec.ModuleHash,
                  StripRecordGeomSame(&s->Rec, &now) ? "the same" : "DIFFERENT", s->Rec.Plugins, built,
                  s->Rec.Adoptions);
    }
    ApolloLogU(Ctx, "strips: warm start: %s%s", StripAdoptText(v),
              (v == STRIP_ADOPT_REBUILD && teardown) ? " (the recorded strips are stopped first)" : "");
    if (s->Enable == 2) {
        s->AdoptPending = FALSE;
        s->State = APOLLO_STRIPS_STATE_IDLE;
        StripsSetWhy(s, "dry run: warm start would be: %s; nothing sent", StripAdoptText(v));
        ApolloLogU(Ctx, "strips: dry run: nothing sent");
        return WARM_DONE;
    }
    switch (v) {
    case STRIP_ADOPT_ADOPTED:
    case STRIP_ADOPT_RESTORE:
        StripsAdoptApply(Ctx, v);
        return v == STRIP_ADOPT_RESTORE ? WARM_RESTORE : WARM_DONE;
    case STRIP_ADOPT_REBUILD:
        s->AdoptPending = FALSE;
        s->Owned = TRUE;
        s->RebuiltWarm = TRUE;
        s->PhantomPending = FALSE;      /* no power-up, no 48V edge to make */
        s->TeardownFirst = FALSE;
        if (teardown && s->RecFound) {
            /* what the record says may still run: its strips stopped, its modules released, as a D0 exit would */
            for (i = 0; i < APOLLO_STRIP_INPUTS; i++) {
                s->StripStarted[i] = ((s->Rec.Mask & (1u << i)) && s->Rec.Limit >= STRIP_STEP_SYNCH) ? TRUE : FALSE;
            }
            for (j = 0; j < APOLLO_STRIP_MODULES; j++) {
                s->ModState[j] = (s->Rec.Limit >= STRIP_STEP_UPLOADS) ? APOLLO_STRIP_MOD_UPLOADING : APOLLO_STRIP_MOD_NONE;
            }
            s->TeardownFirst = TRUE;
        }
        StripsSetWhy(s, "warm start: rebuilding (only this driver used the DSPs since power-up)");
        return WARM_BUILD;
    default:
        s->AdoptPending = FALSE;
        StripsOff(s, StripsWarmWhy(v, s->Anchor));
        return WARM_DONE;
    }
}

/*
 * StripLock held, DevLock not: RESTORE. The record saw a plug-in on the strip DSP, so a chain may still
 * override a strip: each adopted input is stopped and started again at its own header (the probe's
 * stop + start, UA's own words, inline only), which puts the plain strip back whatever ran there.
 */
static VOID StripsAdoptRestore(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Mask, _In_ ULONG Limit, _In_ ULONG Gen)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    BOOLEAN stopped, started = FALSE;

    stopped = StripsInlineXact(Ctx, 0, Mask, Limit, Gen, "warm start restore");
    if (stopped) {
        started = StripsInlineXact(Ctx, 1, Mask, Limit, Gen, "warm start restore");
    }
    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (s->Gen == Gen) {
        if (stopped && started) {
            StripsSetWhy(s, "warm start: adopted StripMask 0x%02lx, plain strips restored", Mask);
            ApolloLogU(Ctx, "strips: warm start: plain strips restored on StripMask 0x%02lx (stop + start taken); a chain's "
                      "modules may still be on DSP %lu: plug-ins after a power cycle", Mask, s->Dsp);
            /* [Warm] 12.5: adopted as plain strips from now on, plug-ins wait for a power cycle (the stale mark) */
            s->Cur.Plugins = STRIP_REC_PLUGINS_STALE;
            (VOID)StripsRecWrite(Ctx, "plain strips restored");
        } else if (!s->Quarantined) {
            s->State = APOLLO_STRIPS_STATE_FAILED;
            StripsSetWhy(s, "warm start: restoring the plain strips was not taken: power-cycle the Apollo");
            ApolloLogU(Ctx, "strips: %s (stop %s)", s->Why, stopped ? "taken, start not" : "not taken");
            StripsRecLost(Ctx, "restore not taken");
        }
    }
    WdfWaitLockRelease(Ctx->DevLock);
}

/* DevLock held: these strips are Live now (built or rebuilt): the record says so, with their plan. */
static VOID StripsRecLive(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Mask, _In_ ULONG Limit)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    STRIP_PLAN *plan = (STRIP_PLAN *)s->PlanScratch;   /* StripLock held by the pass */

    StripsRecSetPlan(Ctx, STRIP_REC_RUNNING, Mask, Limit,
                     StripPlanHash(Mask, Limit, Ctx->SampleRate, s->Dsp, Ctx->NumDsps, plan));
    s->Cur.Keep = (s->AdoptParam && !StripsConnectsOnWarm(Ctx)) ? 1u : 0u;
    s->Cur.BuiltDate = StripsDateNow();
    s->Cur.Adoptions = 0;
    if (s->RebuiltWarm) {
        /* [Warm] the pass's own uploads were acknowledged on this warm start: uploads there are proven (12.5) */
        s->PlugAdoptOk = (s->RecFound && s->Cur.Plugins == 0 &&
                          StripWarmPluginsOk(STRIP_ADOPT_REBUILD, s->Rec.Plugins, s->WarmPlugins)) ? TRUE : FALSE;
        s->WarmUploads = STRIPS_WARM_UPLOADS_PROVEN;
    }
    (VOID)StripsRecWrite(Ctx, s->RebuiltWarm ? "built at a warm start" : "built");
}

/* StripLock held, DevLock not held. Port of ua_strips_run (ua_strips.c:1626-1786). */
static VOID StripsPass(_In_ PDEVICE_CONTEXT Ctx)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    ULONG j, gen, info = 0, dsp, limit, mask, warm;
    BOOLEAN phantom = FALSE, teardown;

    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    s->WorkQueued = FALSE;
    if (s->State != APOLLO_STRIPS_STATE_ARMED || ReadAcquire(&Ctx->StopRequested) ||
        ApolloGateClosed(Ctx)) {
        goto unlock;
    }
    if (!s->ModulesOk && !s->AdoptPending) {    /* [Warm] adopting uploads nothing: the files may wait */
        StripsOff(s, "strip module files missing or not the pinned bytes: run apollo-diag "
                     "strips install, then power-cycle the Apollo");
        goto unlock;
    }
    if (!StripsRateReady(Ctx, &info)) {
        if (!s->RateWaitLogged) {
            ApolloLogU(Ctx, "strips: waiting for %s (rate info 0x%08x, DSPs told %lu Hz, rate %lu Hz)",
                      ApolloRateText(Ctx->SampleRate), info, Ctx->DspToldRate, Ctx->SampleRate);
            s->RateWaitLogged = TRUE;
        }
        s->ArmedAt = KeQueryInterruptTime();    /* the service pass retries in 500 ms */
        goto unlock;
    }
    dsp = s->Dsp;
    if (s->AdoptPending) {
        /* [Warm] a warm start: adopt, restore, rebuild, or off (docs/STRIPS.md 12) */
        warm = StripsWarmDecide(Ctx);
        if (warm == WARM_WAIT) {
            goto unlock;                        /* armed again: the service pass retries in 500 ms */
        }
        if (warm == WARM_DONE || warm == WARM_RESTORE) {
            BOOLEAN adopted = s->Adopted && s->State == APOLLO_STRIPS_STATE_LIVE;
            ULONG am = s->Rec.Mask & ((1u << APOLLO_STRIP_INPUTS) - 1), al = s->Rec.Limit, ag = s->Gen;

            WdfWaitLockRelease(Ctx->DevLock);
            if (adopted && warm == WARM_RESTORE) {
                StripsAdoptRestore(Ctx, am, al, ag);
            }
            if (adopted) {
                StripsProbe(Ctx, am, al, ag);   /* StripProbe 1: measure the adopted inputs (default off) */
            }
            return;
        }
        if (!s->ModulesOk) {                    /* WARM_BUILD */
            StripsOff(s, "warm start: strip module files missing or not the pinned bytes: run apollo-diag strips "
                         "install, then power-cycle the Apollo");
            goto unlock;
        }
    }
    if (!s->Owned || s->Quarantined || s->Tainted) {
        StripsOff(s, "the strip DSP is not ours any more: power-cycle the Apollo");
        goto unlock;
    }
    /*
     * [Warm] A warm start that sent no ring connect (the x8p) builds only when StripWarmRebuild allows uploads
     * to its warm DSP (a warm rebuild, or a rate change after an adoption); its rings are live (F8).
     */
    if ((!Ctx->DspsConnected && !(Ctx->WarmStart && s->WarmRebuild)) || dsp >= Ctx->NumDsps || dsp >= APOLLO_MAX_DSPS ||
        !Ctx->Cmd[dsp].Live || !Ctx->Resp[dsp].Live) {
        StripsOff(s, "the strip DSP is not ring-connected");
        goto unlock;
    }
    StripsReadLayout(Ctx);                      /* fresh, right before anything is sent */
    if (s->LayoutWhy != 0) {
        ApolloLogU(Ctx, "strips: DSP %lu pools changed: size %08x %08x %08x %08x base %08x %08x %08x %08x "
                  "scratch %08x %08x %08x (code %lu)", dsp, s->Geom[0], s->Geom[1], s->Geom[2],
                  s->Geom[3], s->Geom[4], s->Geom[5], s->Geom[6], s->Geom[7], s->Geom[8],
                  s->Geom[9], s->Geom[10], s->LayoutWhy);
        StripsOff(s, "the strip DSP's pools do not match the recipe: nothing sent");
        goto unlock;
    }
    limit = StripsStepLimit(s->Steps);
    mask = s->Mask & ((1u << APOLLO_STRIP_INPUTS) - 1);
    if (s->Enable == 2) {
        ApolloLogU(Ctx, "strips: dry run: every gate passed (modules verified, DSP %lu owned, layout ok, "
                  "%s locked); would upload 0x0eb 0x0da 0x0a5 0x0c2 0x12b to 0xe0000..0xe03e1; "
                  "nothing sent", dsp, ApolloRateText(Ctx->SampleRate));
        ApolloLogU(Ctx, "strips: dry run: would go to stage %lu (%s) on StripMask 0x%02lx", limit,
                  StripsStepName(limit), mask);
        StripsLogDryRun(Ctx, mask, limit);      /* [Twin X G5] the whole pass, per input, not sent */
        s->State = APOLLO_STRIPS_STATE_IDLE;
        StripsSetWhy(s, "dry run complete: every gate passed, nothing sent");
        goto unlock;
    }
    if (!s->PoolReady) {
        StripsOff(s, "no transaction buffers");
        goto unlock;
    }
    s->State = APOLLO_STRIPS_STATE_BUILDING;
    s->Passes++;
    s->StepsDone = 0;
    gen = s->Gen;
    StripsSetWhy(s, "uploading the 5 modules to DSP %lu", dsp);
    ApolloLogU(Ctx, "strips: uploading 5 Mic Input Strip modules to DSP %lu (StripSteps=%lu: stage %lu, %s)",
              dsp, s->Steps, limit, StripsStepName(limit));
    /* [Warm] the record: a pass is under way, so what the DSP holds is unknown until it ends */
    StripsRecSetPlan(Ctx, STRIP_REC_UNKNOWN, mask, limit, 0);
    s->Cur.Keep = 0;
    (VOID)StripsRecWrite(Ctx, s->RebuiltWarm ? "a warm rebuild starts" : "a pass starts");
    teardown = s->TeardownFirst;
    s->TeardownFirst = FALSE;
    WdfWaitLockRelease(Ctx->DevLock);

    /* [Warm] a rebuild over strips the record says may still run: stopped and released first */
    if (teardown && !StripsTeardownXact(Ctx, "warm rebuild", FALSE)) {
        WdfWaitLockAcquire(Ctx->DevLock, NULL);
        if (s->Gen == gen && s->State == APOLLO_STRIPS_STATE_BUILDING) {
            s->State = APOLLO_STRIPS_STATE_FAILED;
            StripsSetWhy(s, "warm rebuild: the old strips' teardown was not proven: power-cycle the Apollo");
            ApolloLogU(Ctx, "strips: %s", s->Why);
        }
        WdfWaitLockRelease(Ctx->DevLock);
        return;
    }

    /* Stage 1: the code modules (T3). */
    for (j = 0; j < APOLLO_STRIP_MODULES; j++) {
        if (!StripsUploadOne(Ctx, j, gen)) {
            return;
        }
    }

    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (s->Gen != gen || s->State != APOLLO_STRIPS_STATE_BUILDING) {
        goto unlock;
    }
    s->StepsDone = STRIP_STEP_UPLOADS;
    if (limit == STRIP_STEP_UPLOADS) {
        s->State = APOLLO_STRIPS_STATE_LIVE;
        StripsSetWhy(s, "uploads only: 5 modules resident on DSP %lu, no strip started", dsp);
        ApolloLogU(Ctx, "strips: 5 modules resident on DSP %lu (0xe0000..0xe03e1); stopping here "
                  "(StripSteps=1), no strip is started", dsp);
        StripsRecLive(Ctx, mask, limit);        /* [Warm] */
        goto unlock;
    }
    if (mask == 0) {
        s->State = APOLLO_STRIPS_STATE_LIVE;
        StripsSetWhy(s, "5 modules resident on DSP %lu; StripMask selects no input", dsp);
        ApolloLogU(Ctx, "strips: 5 modules resident; StripMask is 0: no strip");
        StripsRecLive(Ctx, mask, limit);        /* [Warm] */
        goto unlock;
    }
    WdfWaitLockRelease(Ctx->DevLock);

    /*
     * Stages 2-8 (docs/STRIPS.md 10.8): a PREPARE transaction per preamp in StripMask (input 1
     * first), then ONE START transaction for all of them, in strip_plan.h's order. Every PREPARE
     * buffer stays held until the START is taken.
     */
    {
        PAPOLLO_XACT held[APOLLO_STRIP_INPUTS];
        BOOLEAN ok = TRUE;
        ULONG k, kind, in;

        RtlZeroMemory(held, sizeof(held));
        for (k = 0; ok && k <= APOLLO_STRIP_INPUTS; k++) {
            kind = StripPassStep(mask, limit, k, &in);
            if (kind == STRIP_XACT_PREPARE && in < APOLLO_STRIP_INPUTS) {
                ok = StripsPrepareOne(Ctx, in, limit, gen, &held[in]);
            } else if (kind == STRIP_XACT_START) {
                ok = StripsStartAll(Ctx, mask, limit, gen);
            } else {
                break;
            }
        }
        WdfWaitLockAcquire(Ctx->DevLock, NULL);
        for (j = 0; j < APOLLO_STRIP_INPUTS; j++) {
            if (held[j] != NULL) {
                XactRelease(Ctx, held[j]);
            }
        }
        WdfWaitLockRelease(Ctx->DevLock);
        if (!ok) {
            return;
        }
    }

    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (s->Gen == gen && s->State == APOLLO_STRIPS_STATE_BUILDING) {
        s->State = APOLLO_STRIPS_STATE_LIVE;
        s->StepsDone = min(limit, (ULONG)STRIP_STEP_OTHERS);
        StripsSetWhy(s, "stage %lu (%s) done on StripMask 0x%02lx, DSP %lu%s",
                     s->StepsDone, StripsStepName(s->StepsDone), mask, dsp, s->RebuiltWarm ? " (warm rebuild)" : "");
        ApolloLogU(Ctx, "strips: %s", s->Why);
        StripsRecLive(Ctx, mask, limit);        /* [Warm] the record: these strips run, this plan */
        /* Stage 9: 48V only after a cold start (a rebuild keeps the supply up). */
        if (limit >= STRIP_STEP_PHANTOM && s->PhantomPending) {
            s->PhantomPending = FALSE;
            phantom = TRUE;
        }
    }
    WdfWaitLockRelease(Ctx->DevLock);
    if (phantom) {
        StripsPhantomResync(Ctx, mask, gen);
    }
    /* Measure every strip input's record slot; repair while one reads exact zero (10.9). */
    StripsProbe(Ctx, mask, limit, gen);
    return;
unlock:
    WdfWaitLockRelease(Ctx->DevLock);
}

VOID ApolloEvtStripWork(_In_ WDFWORKITEM WorkItem)
{
    PDEVICE_CONTEXT ctx = ApolloGetContext((WDFDEVICE)WdfWorkItemGetParentObject(WorkItem));
    BOOLEAN load;

    /* File I/O and hashing first, with no lock held (the work item never runs twice at once). */
    WdfWaitLockAcquire(ctx->DevLock, NULL);
    load = ctx->Strips.State == APOLLO_STRIPS_STATE_ARMED && !ctx->Strips.ModulesOk &&
           !ReadAcquire(&ctx->StopRequested);
    WdfWaitLockRelease(ctx->DevLock);
    if (load) {
        StripsLoadModules(ctx);
    }

    WdfWaitLockAcquire(ctx->StripLock, NULL);
    StripsPass(ctx);
    WdfWaitLockRelease(ctx->StripLock);
}

/* ------------------------------------------------------------------ */
/* D0 exit: release what we uploaded (docs/STRIPS.md 2.9, 5.4)         */
/* ------------------------------------------------------------------ */

/*
 * StripLock held, DevLock not; Gen is the caller's business. One transaction: SYNCH with header 0
 * for every strip whose SYNCH went out, then {0x00030002, id, 0, 0} for every module we sent
 * (resident, refused or unproven); waited up to 1 s. Bus owners go back to DSP 0. TRUE = proven, or
 * nothing to send. [Warm] Record: the record says "unknown" before anything is sent and "none" once
 * proven (an unproven one loses the anchor: StripsQuarantine, StripsRecLost); a device that is gone
 * changes nothing (a unit that stays powered keeps its strips, and its record still names them).
 */
_IRQL_requires_(PASSIVE_LEVEL)
static BOOLEAN StripsTeardownXact(_In_ PDEVICE_CONTEXT Ctx, _In_z_ PCSTR Why, _In_ BOOLEAN Record)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    PAPOLLO_XACT x = NULL;
    NTSTATUS status;
    ULONG j, n, ns;
    BOOLEAN ok = FALSE;

    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    n = StripsModulesSent(s);
    ns = StripsStarted(s);
    if (n == 0 && ns == 0) {
        StripsForgetStrips(s);                  /* prepared, never started: nothing to stop */
        WdfWaitLockRelease(Ctx->DevLock);
        return TRUE;
    }
    if (ApolloGateClosed(Ctx) || s->Dsp >= APOLLO_MAX_DSPS || !Ctx->Cmd[s->Dsp].Live) {
        ApolloLogU(Ctx, "strips: %lu strip(s), %lu module(s) forgotten (%s): the device is gone", ns, n,
                  Why);
        StripsForgetModules(s);
        StripsForgetStrips(s);
        WdfWaitLockRelease(Ctx->DevLock);
        return FALSE;
    }
    x = XactAlloc(Ctx, s->Dsp);
    if (x == NULL) {
        ApolloLogU(Ctx, "strips: no buffer for the teardown of %lu strip(s), %lu module(s) (%s)", ns, n, Why);
        StripsForgetModules(s);
        StripsForgetStrips(s);
        StripsQuarantine(Ctx, "the teardown could not be sent", STATUS_INSUFFICIENT_RESOURCES);
        WdfWaitLockRelease(Ctx->DevLock);
        return FALSE;
    }
    /* SYNCH with header 0 stops each strip whose SYNCH went out, then the releases (A6.9). */
    for (j = 0; j < APOLLO_STRIP_INPUTS; j++) {
        if (s->StripStarted[j]) {
            XactInline(x, STRIP_CMD_SYNCH, (STRIP_SYNCH_MONO << 16) | j, 0, 0);
        }
    }
    for (j = 0; j < APOLLO_STRIP_MODULES; j++) {
        if (s->ModState[j] != APOLLO_STRIP_MOD_NONE) {
            XactInline(x, STRIP_CMD_RELEASE_CODE, g_ApolloStripPins[j].Id, 0, 0);
        }
    }
    StripsForgetModules(s);
    StripsForgetStrips(s);
    s->Teardowns++;
    if (Record && s->RecHave && s->Cur.State != STRIP_REC_UNKNOWN) {
        s->Cur.State = STRIP_REC_UNKNOWN;       /* [Warm] from here until it is proven */
        s->Cur.Keep = 0;
        (VOID)StripsRecWrite(Ctx, "a teardown starts");
    }
    WdfWaitLockRelease(Ctx->DevLock);

    status = XactRun(Ctx, x);

    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (NT_SUCCESS(status)) {
        ok = TRUE;
        ApolloLogU(Ctx, "strips: %lu strip(s) stopped, %lu module(s) released on DSP %lu (%s)", ns, n,
                  s->Dsp, Why);
        if (Record && StripsRecordsKept(Ctx)) {
            StripRecordClearStrips(&s->Cur, STRIP_REC_NONE);  /* [Warm] proven: nothing of ours there */
            (VOID)StripsRecWrite(Ctx, "teardown proven");
        }
    } else if (x->State == XactBuilding) {
        ApolloLogU(Ctx, "strips: the teardown (%lu strip(s), %lu module(s)) was not sent (0x%08x, %s)", ns, n, status, Why);
        StripsQuarantine(Ctx, "the teardown could not be sent", status);
    } else {
        ApolloLogU(Ctx, "strips: the teardown (%lu strip(s), %lu module(s)) was not proven (0x%08x, %s)", ns, n, status, Why);
        StripsRecLost(Ctx, "teardown not proven");     /* [Warm] quarantined already when it timed out */
    }
    XactRelease(Ctx, x);
    WdfWaitLockRelease(Ctx->DevLock);
    return ok;
}

/*
 * No lock held; the service thread is already stopped. The teardown (StripsTeardownXact), BEFORE
 * ApolloRingsQuiesce resyncs the rings and sends the date-only disconnect. A pass in flight stops
 * (Gen). Rate changes, and every D0 exit that does not keep the strips (ApolloStripsExit).
 */
VOID ApolloStripsQuiesce(_In_ PDEVICE_CONTEXT Ctx, _In_z_ PCSTR Why)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    BOOLEAN ok;

    if (Ctx->StripWork != NULL) {
        WdfWorkItemFlush(Ctx->StripWork);
    }
    if (Ctx->StripLock == NULL) {
        return;
    }
    WdfWaitLockAcquire(Ctx->StripLock, NULL);
    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    s->WorkQueued = FALSE;
    s->Gen++;
    s->AdoptPending = FALSE;
    s->PlugAdoptOk = FALSE;
    if (s->State == APOLLO_STRIPS_STATE_ARMED || s->State == APOLLO_STRIPS_STATE_BUILDING) {
        s->State = APOLLO_STRIPS_STATE_IDLE;
        StripsSetWhy(s, "stopped (%s)", Why);
    }
    WdfWaitLockRelease(Ctx->DevLock);

    ok = StripsTeardownXact(Ctx, Why, TRUE);

    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (ok && s->State == APOLLO_STRIPS_STATE_LIVE) {
        s->State = APOLLO_STRIPS_STATE_IDLE;
        StripsSetWhy(s, "strips stopped, modules released (%s)", Why);
    }
    WdfWaitLockRelease(Ctx->DevLock);
    WdfWaitLockRelease(Ctx->StripLock);
}

/*
 * [Warm] D0 exit, no lock held, before the plug-in quiesce (which then puts the plain strips back on any
 * input a chain overrides): may the strips stay running for the next start? Only Live strips this driver
 * started, on a model whose warm start keeps the DSPs' state, with StripAdopt, nothing quarantined or
 * tainted, and an anchored record that names them as running and kept (the registry holds it).
 */
_Use_decl_annotations_
BOOLEAN ApolloStripsExitKeep(PDEVICE_CONTEXT Ctx)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    BOOLEAN keep;

    if (Ctx->StripLock == NULL) {
        return FALSE;
    }
    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    keep = s->Enable == 1 && s->AdoptParam != 0 && !StripsConnectsOnWarm(Ctx) &&
           s->State == APOLLO_STRIPS_STATE_LIVE && s->Owned && !s->Quarantined && !s->Tainted &&
           s->StepsDone >= STRIP_STEP_SYNCH && StripsStarted(s) != 0 && StripsRecordsKept(Ctx) && s->RecHave &&
           s->Cur.State == STRIP_REC_RUNNING && s->Cur.Keep && s->Cur.Anchored &&
           !ApolloGateClosed(Ctx) && Ctx->Stage != ApolloStageGone;
    WdfWaitLockRelease(Ctx->DevLock);
    return keep;
}

/*
 * [Warm] D0 exit, no lock held, after the plug-in quiesce: Keep (re-checked: the plug-in teardown may have
 * quarantined the DSP) leaves the strips running and sends nothing, so the next warm start adopts them;
 * otherwise the teardown, as every D0 exit did before.
 */
_Use_decl_annotations_
VOID ApolloStripsExit(PDEVICE_CONTEXT Ctx, BOOLEAN Keep)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    ULONG mask = 0, i;
    BOOLEAN still;

    if (!Keep || Ctx->StripLock == NULL) {
        ApolloStripsQuiesce(Ctx, "D0 exit");
        return;
    }
    if (Ctx->StripWork != NULL) {
        WdfWorkItemFlush(Ctx->StripWork);       /* a probe after the pass, or a 48V resync, ends first */
    }
    WdfWaitLockAcquire(Ctx->StripLock, NULL);
    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    still = s->State == APOLLO_STRIPS_STATE_LIVE && s->Owned && !s->Quarantined && !s->Tainted && s->RecHave &&
            s->Cur.State == STRIP_REC_RUNNING && s->Cur.Keep && s->Cur.Anchored && !ApolloGateClosed(Ctx);
    if (still) {
        s->WorkQueued = FALSE;
        s->Gen++;                               /* nothing more is sent for these strips by this start */
        s->KeptAtExit = TRUE;
        s->PlugAdoptOk = FALSE;
        for (i = 0; i < APOLLO_STRIP_INPUTS; i++) {
            if (s->StripStarted[i]) {
                mask |= 1u << i;
            }
        }
        StripsSetWhy(s, "left running at the D0 exit for the next start (StripMask 0x%02lx)", mask);
        ApolloLogU(Ctx, "strips: D0 exit: StripMask 0x%02lx left running on DSP %lu for the next start (StripAdopt=1; "
                  "the record names them, %lu plug-in(s) on the DSP): nothing sent", mask, s->Dsp, s->Cur.Plugins);
    }
    WdfWaitLockRelease(Ctx->DevLock);
    WdfWaitLockRelease(Ctx->StripLock);
    if (!still) {
        ApolloLogU(Ctx, "strips: D0 exit: the strips can no longer be kept (quarantine, taint or the record): torn down");
        ApolloStripsQuiesce(Ctx, "D0 exit");
    }
}
