/*
 * plugin.c - UAD plug-in hosting on the Apollo's DSPs, phase P0 (docs/PLUGINS.md 2.1, gate P1):
 *
 *   - transaction engine v2: payloads up to 256 KiB as separate 4 KiB common-buffer pages (a
 *     DMA_REF never crosses a page), 128 entries, up to 8 reply descriptors of up to 1024 dwords
 *     with a per-transaction prefill; one transaction at a time (StripLock), published under
 *     DevLock, waited for with DevLock released, 1 s bound (2 s for the board query);
 *   - the board query on DSP 0 (the 770-dword reply; it also sends the device date, as UA's
 *     driver does) and the admit decision, fresh for every CREATE, never cached, counts-only log;
 *   - the pool ledger per owned DSP (plugin_plan.h), the strip DSP's strip room pre-booked;
 *   - module upload by SHA-256 from the admin-only store System32\drivers\ApolloModules (the
 *     pinned strip modules also from ApolloStrips), re-hashed, Bill-checked, each acknowledged;
 *   - PLUGIN_INSTANCE: CREATE (CHECK, unrouted, START + CHAIN on a live plain strip, STAGE + CHAIN
 *     for a startup over one page), DESTROY, SERVICE (WRITEs + one read), START (route a staged
 *     chain); instances belong to the file handle: closing it destroys them
 *     and restores the retained plain strip; a D0 exit stops them before the strips' teardown;
 *   - stereo (FEAT_STEREO): a chain routed on a linked pair (in1 even, in2 = in1 + 1) stops,
 *     overrides and restores both plain strips; every Synch path (strip target, overlap, start,
 *     stop, restore) takes both inputs, as upstream's;
 *   - staged startup (ua_plugin_instance.c:842-906, :1277-1332): the first page with the CREATE
 *     (WRITEs only, the plain strip keeps running), the rest as SERVICE writes, one bounded
 *     transaction each, then START routes it; a staged chain that goes before START was never
 *     routed, so its teardown releases and changes no routing;
 *   - the caller (FEAT_CALLER): BOARD_QUERY and PLUGIN_INSTANCE only from an elevated
 *     administrator's handle or the PluginHostImage program's own handle (PlugCallerAllowed),
 *     refused before any lock or word;
 *   - per unit (P7, FEAT_UNITS): the x8p and the Twin X Gen 1 Duo (plugin_plan.h PlugModelFor).
 *     Everything above is per device already: each unit's strip DSP (DSP 1), its ledger with only
 *     its own strips' room (x8p 8, Twin X 2), its own board query on its DSP 0, its inputs and pair
 *     (Twin X 1-2, the pair 1/2; Hi-Z is input 1's strip; the talkback has none: refused);
 *   - quarantine: a transaction whose outcome cannot be proven (no reply in time, a refused
 *     upload, a resync that dropped it) ends all placement on that DSP until a power cycle; on
 *     the strip DSP it also quarantines the strips (strips.c sends nothing more either).
 *
 *   - [Ins] chains of up to four effects are the host's (the directory check took N = 2..5 since P0); Unison (F_UNISON,
 *     STAGE only; BEGIN_UNISON / ACTIVATE_UNISON / UNISON_PARAMS, ops 6/7/5 as upstream): the analog lease of
 *     unison_plan.h on the mixer cache and readback, every other write to an owned field refused (mixer.c asks
 *     ApolloPluginAnalogConflict), the baseline put back after the route is retired, an unproven completion blocks
 *     the fields until the hardware is released; aux return chains (Synch 0x10/0x11, 0x12/0x13, Input Strip carrier,
 *     Parameters\AuxInserts = 1): no retained strip, stop only, the return's coefficients follow the chain's DSP
 *     (cues.c ApolloCuesAuxOwner);
 *   - [Warm] (FEAT_WARM, docs/STRIPS.md 12.5) the strip DSP of strips a warm start adopted or rebuilt from an anchored
 *     record with no plug-in left there is owned as after a cold start (StripWarmPlugins); its first acknowledged
 *     upload proves the warm session, an unproven first step quarantines it; caps say why a DSP is not owned;
 *   - [Lat] (FEAT_LATENCY) op LATENCY: a routed chain's record path, so an over-budget chain is in the reported
 *     input latency (asio.c ApolloLatencyRefresh) while its route lasts;
 *
 * Every path is bounded (1-2 s per transaction, 50 ms to publish, a bounded StripLock wait for
 * the IOCTLs) and anything placed is taken down again when a later step fails.
 *
 * Ported from open-apollo (GPL-2.0): ua_dsp_xact.c, ua_plugin_license.c, ua_plugin_catalog.c,
 * ua_plugin_place.c, ua_plugin_upload.c, ua_plugin_instance.c, ua_plugin_host.c, ua_strips.c
 * (override/restore). Citations in docs/PLUGINS.md.
 *
 * Locking: StripLock serialises every plug-in operation with the strip passes (open-apollo's
 * host->mutex); it is taken BEFORE DevLock. DevLock covers ring indexes, the strips' fields and
 * the ledgers, in short sections; nothing waits for the DSP while holding it. The host struct,
 * its pages, catalog and instances change only with StripLock held.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <ntifs.h>              /* IoGetRequestorProcess, SeLocateProcessImageName, tokens, symbolic links: the caller rule */
#include "apollo.h"
#include <bcrypt.h>
#include <ntstrsafe.h>
#include "apollo_strip_pins.h"
#include "strip_plan.h"
#include "plugin_plan.h"
#include "unison_plan.h"        /* [Ins] the Unison analog lease (preamp_plan.h's full field map) */

#define PLUG_TAG                'gulP'
#define PLUG_PAGES_MAX          96u
#define PLUG_CATALOG_MAX        96u
#define PLUG_WAIT_MS            1000u           /* ua_plugin_host.h:133 */
#define PLUG_PUBLISH_MS         50u             /* ua_plugin_host.h:134 */
#define PLUG_BQ_WAIT_MS         2000u           /* ua_plugin_host.h:375 */
#define PLUG_LOCK_MS            10000u          /* an IOCTL waits this long for a strip pass */
#define PLUG_CAPS_LOCK_MS       2000u
#define PLUG_RESYNC_RX          4u              /* ua_plugin_host.h:135 */
#define PLUG_DENIED_LOG_MS      10000u          /* a caller that is not the host: one log line per 10 s at most */

#define MS_TO_100NS(ms)         ((ULONGLONG)(ms) * 10000ULL)

C_ASSERT(APOLLO_PLUGIN_RES_MAX == PLUG_RES_MAX && APOLLO_PLUGIN_PRIV_MAX == PLUG_PRIV_MAX);
C_ASSERT(APOLLO_PLUGIN_CMD_WORDS_MAX == PLUG_CMD_WORDS_MAX && APOLLO_PLUGIN_READ_WORDS_MAX == PLUG_READ_WORDS_MAX);
C_ASSERT(APOLLO_BOARD_QUERY_WORDS == PLUG_BQ_WORDS && APOLLO_BOARD_QUERY_BASE == PLUG_BQ_BASE);
C_ASSERT(APOLLO_PLUGIN_LICENSE_AUTHORIZED == PLUG_LIC_AUTHORIZED && APOLLO_PLUGIN_LICENSE_DEMO == PLUG_LIC_DEMO &&
         APOLLO_PLUGIN_LICENSE_NOT_STARTED == PLUG_LIC_NOT_STARTED && APOLLO_PLUGIN_LICENSE_EXPIRED == PLUG_LIC_EXPIRED &&
         APOLLO_PLUGIN_LICENSE_UNAVAILABLE == PLUG_LIC_UNAVAILABLE && APOLLO_PLUGIN_LICENSE_UNKNOWN == PLUG_LIC_UNKNOWN);
C_ASSERT(sizeof(APOLLO_PLUGIN_ROW) == sizeof(PLUG_ROW));
C_ASSERT(APOLLO_PLUGIN_DSPS == APOLLO_MAX_DSPS);
C_ASSERT(PLUG_RING_DMA_REF == RING_DMA_REF);
C_ASSERT(PLUG_FILETIME_2007 == FILETIME_2007);
C_ASSERT(PLUG_PAGE == XACT_PAGE);
C_ASSERT(STRIP_CODE_WORDS == 0x6C + 0x1F0 + 0x130 + 0x16 + 0x40);
C_ASSERT(APOLLO_MODULE_BYTES_MAX == PLUG_BILL_MAX);
C_ASSERT(PLUG_PAY_PAGES_MAX + PLUG_RX_PAGES_MAX <= PLUG_PAGES_MAX);
C_ASSERT(PLUG_PRIV_MAX + 1 <= PLUG_XACT_ENTS_MAX);     /* the privates transaction: a zero-fill per row + the memspec */
C_ASSERT(APOLLO_PLUGIN_F_CHECK == PLUG_F_CHECK && APOLLO_PLUGIN_F_START == PLUG_F_START &&
         APOLLO_PLUGIN_F_STAGE == PLUG_F_STAGE && APOLLO_PLUGIN_F_CHAIN == PLUG_F_CHAIN);
C_ASSERT(PLUG_CMD_WORDS_MAX * 4 <= PLUG_PAGE);         /* one startup page or SERVICE write: one DMA page */
C_ASSERT(APOLLO_PLUGIN_SYNCH_MONO == PLUG_SYNCH_MONO);
C_ASSERT(APOLLO_PLUGIN_IMAGE_CCH_MAX >= 1 && APOLLO_PLUGIN_IMAGE_CCH_MAX < 0x7FFF);    /* a UNICODE_STRING's bytes */
C_ASSERT(APOLLO_PLUGIN_ENABLED_HOST == 1);                                              /* version 8's Enabled = 1 */
C_ASSERT(PLUG_MODEL_X8P == APOLLO_MODEL_X8P && PLUG_MODEL_X8P == STRIP_MODEL_X8P);         /* [P7] one model id everywhere */
C_ASSERT(PLUG_MODEL_TWINX == APOLLO_MODEL_TWINX && PLUG_MODEL_TWINX == STRIP_MODEL_TWINX);
/* [Ins] Unison and aux inserts */
C_ASSERT(APOLLO_PLUGIN_F_UNISON == PLUG_F_UNISON && APOLLO_PLUGIN_UNISON_WORDS_MAX == APOLLO_UNISON_WORDS_MAX);
C_ASSERT(APOLLO_UNISON_SETTINGS == MIXER_NUM_SETTINGS && APOLLO_UNISON_RB_WORDS == APOLLO_READBACK_WORDS);
C_ASSERT(APOLLO_PLUGIN_UNISON_WORDS_MAX <= PLUG_CMD_WORDS_MAX);
C_ASSERT(APOLLO_PLUGIN_AUX_BUSES == PLUG_AUX_BUSES);

/* ------------------------------------------------------------------ */
/* Host state                                                          */
/* ------------------------------------------------------------------ */

#define PAGE_NONE               0u
#define PAGE_FREE               1u
#define PAGE_USED               2u
#define PAGE_PARKED             3u      /* published, never proven: never reused, freed only when the device is gone */

typedef struct _PLUG_PAGEBUF {
    WDFCOMMONBUFFER Buffer;
    PUCHAR          Va;
    ULONGLONG       La;
    ULONG           State;
} PLUG_PAGEBUF;

#define PX_FREE                 0u
#define PX_BUILDING             1u
#define PX_PUBLISHED            2u
#define PX_RETIRED              3u

typedef struct _PLUG_XACT {
    ULONG           State;
    ULONG           Dsp;
    ULONG           Epoch;
    BOOLEAN         NoQuarantine;   /* the board query: a timeout is not a reason to stop using DSP 0 */
    PLUG_XL         L;
    PLUG_PAY        Pay;
    UCHAR           PayIdx[PLUG_PAY_PAGES_MAX];
    ULONG           NPay;
    UCHAR           RxIdx[PLUG_RX_PAGES_MAX];
    ULONG           NRxPages;
    XACT_REF        Ref[2];         /* [0] command ring, [1] response ring */
    ULONGLONG       PublishedAt;
} PLUG_XACT;

#define RES_NONE                0u
#define RES_RESIDENT            1u
#define RES_UNKNOWN             2u

typedef struct _PLUG_CAT {
    ULONG           Users;          /* instances holding it; 0 = a free slot */
    UCHAR           Sha[32];
    PLUG_BILL       Bill;
    ULONG           Len;
    ULONG           Serial;         /* ledger identity, never reused */
    LONG            StripPin;       /* index into g_ApolloStripPins, -1 = none */
    PUCHAR          Blob;           /* the record (paged pool), only while a CREATE needs it */
    struct {
        ULONG       Epoch;
        ULONG       State;          /* RES_* */
        ULONG       Base;
    } Res[APOLLO_MAX_DSPS];
} PLUG_CAT;

typedef struct _PLUG_DSP {
    ULONG           Epoch;          /* the epoch the fields below belong to */
    BOOLEAN         Quarantined;
    BOOLEAN         LedgerValid;
    ULONG           Regs[11];
    PLUG_LEDGER    *Ledger;         /* allocated on first use */
    PLUG_LEASE      StripBook[4];
    ULONG           Xacts, Uploads, RepliesOk, RepliesBad, ZeroFills, Memspecs, Writes, Stops, Releases, Timeouts;
} PLUG_DSP;

typedef struct _PLUG_INST {
    WDFFILEOBJECT   Owner;
    ULONG           Handle;
    ULONG           RenderId;
    ULONG           Epoch;
    ULONG           Dsp;
    ULONG           Synch;
    ULONG           StripGen;
    BOOLEAN         Placed;
    BOOLEAN         Loaded;
    BOOLEAN         Lost;
    BOOLEAN         Started;
    BOOLEAN         StripOverride;
    BOOLEAN         Staged;         /* F_STAGE, not routed yet (op START clears it) */
    BOOLEAN         ScratchClaim;
    ULONG           RecExcess;      /* [Lat] op LATENCY: its record path over the plain strip's 200 (frames) */
    /* [Ins] an aux insert chain (1 + the aux bus; 0 = an input's chain), Unison and its analog lease */
    ULONG           AuxBus;         /* runs on the aux return pair: no retained strip, stop only at teardown */
    BOOLEAN         Unison;         /* CREATE F_UNISON: member 0 is an integrated preamp header */
    BOOLEAN         UnisonBegun;    /* BEGIN_UNISON done, ACTIVATE_UNISON not yet */
    BOOLEAN         AnalogOwned;    /* the lease holds AnalogBase.Mask (BEGIN .. restore) */
    APOLLO_UNISON_IMAGE AnalogBase; /* the ordinary settings captured at BEGIN: what the restore puts back */
    APOLLO_UNISON_IMAGE AnalogCur;  /* the accepted image now (PGA0, the plug-in's triples) */
    ULONG           ScratchBase[4];
    ULONG           ScratchNeed[4];
    ULONG           NRes;
    ULONG           NPriv;
    ULONG           Cat[PLUG_RES_MAX];
    UCHAR           StripShared[PLUG_RES_MAX];  /* a strip module resident with the strips: no lease */
    PLUG_LEASE      Lease[PLUG_RES_MAX + PLUG_PRIV_MAX];
    PLUG_ROW        Rows[PLUG_PRIV_MAX];
} PLUG_INST;

typedef struct _APOLLO_PLUGIN_HOST {
    volatile LONG   Live;           /* instances in Inst[]: a handle close without any takes no lock */
    ULONG           NextHandle;
    ULONG           CatSerial;
    ULONG           CookieSeq;
    PLUG_PAGEBUF    Page[PLUG_PAGES_MAX];
    PLUG_XACT       X;
    PLUG_DSP        Dsp[APOLLO_MAX_DSPS];
    PLUG_CAT        Cat[PLUG_CATALOG_MAX];
    PLUG_INST      *Inst[APOLLO_PLUGIN_INSTANCES_MAX];
    ULONG           BoardQueries, BoardQueryFailures, Admitted, Refused;
    NTSTATUS        LastStatus;
    ULONG           LastStep;
    BOOLEAN         LoggedDisabled;
    /* refusals of a caller that is not the host (the sequential queue serialises these, no lock) */
    ULONG           Denied;         /* since the host was made */
    ULONG           DeniedUnlogged; /* since the last log line */
    ULONGLONG       DeniedLogAt;    /* interrupt time of the next line allowed */
    /* scratch for one operation (StripLock) */
    ULONG           Words2N[2 * PLUG_PRIV_MAX];
    ULONG           WordsN[PLUG_PRIV_MAX];
    ULONG           Reloc[PLUG_CMD_WORDS_MAX];
    ULONG           Addr[PLUG_PRIV_MAX];
    PLUG_REQ        Req[PLUG_RES_MAX + PLUG_PRIV_MAX];
    PLUG_LEASE      Got[PLUG_RES_MAX + PLUG_PRIV_MAX];
    PLUG_BILL       Bills[PLUG_RES_MAX];
    ULONG           Bq[PLUG_BQ_WORDS];
    CHAR            Why[96];
    /*
     * [Ins] The Unison analog lease (unison_plan.h), DevLock: AnalogMask = every field a lease owns or a
     * failed completion blocked (ApolloPluginAnalogConflict reads it from mixer.c), AnalogBlock = the
     * blocked part (kept until the hardware is released), AnalogWrite = the lease's own write is going out.
     */
    ULONG           AnalogMask[APOLLO_UNISON_SETTINGS];
    ULONG           AnalogBlock[APOLLO_UNISON_SETTINGS];
    BOOLEAN         AnalogWrite;
} APOLLO_PLUGIN_HOST;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static NTSTATUS PlugErr(_In_ ULONG E)
{
    switch (E) {
    case PLUG_OK:      return STATUS_SUCCESS;
    case PLUG_E2BIG:   return STATUS_BUFFER_OVERFLOW;
    case PLUG_ENOSPC:  return STATUS_INSUFFICIENT_RESOURCES;
    case PLUG_EEXIST:  return STATUS_OBJECT_NAME_COLLISION;
    case PLUG_EACCES:  return STATUS_ACCESS_DENIED;
    case PLUG_ENOTSUP: return STATUS_NOT_SUPPORTED;
    case PLUG_ERANGE:  return STATUS_INVALID_DEVICE_STATE;
    default:           return STATUS_INVALID_PARAMETER;
    }
}

static ULONG PlugMsSince(_In_ ULONGLONG T0)
{
    return (ULONG)((KeQueryInterruptTime() - T0) / 10000ULL);
}

static const STRIP_MODEL *PlugStripModel(_In_ PDEVICE_CONTEXT Ctx)
{
    return (Ctx->Ident.Model > 0) ? StripModelFor((ULONG)Ctx->Ident.Model) : NULL;
}

/*
 * The analog inputs a stereo pair may span (PlugSynchCheck): the model's preamp inputs when its
 * input 1 is DSP input 0, so DSP inputs 2k / 2k + 1 are the linked pair 2k+1/2k+2 (the x8p: 8);
 * else 0, no pairs (fail closed on a numbering this host does not know).
 */
static ULONG PlugPairInputs(_In_ PDEVICE_CONTEXT Ctx)
{
    const STRIP_MODEL *model = PlugStripModel(Ctx);

    if (model == NULL || model->FirstSynch != 0) {
        return 0;
    }
    return (model->Inputs < (ULONG)APOLLO_STRIP_INPUTS) ? model->Inputs : (ULONG)APOLLO_STRIP_INPUTS;
}

/* A DWORD of Services\apollo\Parameters, read now; 0 when absent. */
static ULONG PlugRegULong(_In_z_ PCWSTR Value)
{
    WDFKEY key = NULL;
    UNICODE_STRING name;
    ULONG v = 0;

    if (NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        RtlInitUnicodeString(&name, Value);
        if (!NT_SUCCESS(WdfRegistryQueryULong(key, &name, &v))) {
            v = 0;
        }
        WdfRegistryClose(key);
    }
    return v;
}

/* Services\apollo\Parameters\PluginHost, read now (an admin turns hosting on and off without a restart). */
static ULONG PlugRegEnabled(VOID)
{
    return PlugRegULong(APOLLO_PLUGIN_REG_ENABLE);
}

/* [Ins] Parameters\AuxInserts: chains on the aux returns (experimental, never run on hardware), default 0. */
static BOOLEAN PlugAuxEnabled(VOID)
{
    return PlugRegULong(APOLLO_PLUGIN_REG_AUX) != 0;
}

/* [P7] The unit's hosting plan (plugin_plan.h): the x8p and the Twin X Gen 1, else NULL (no hosting). */
static const PLUG_MODEL *PlugModel(_In_ PDEVICE_CONTEXT Ctx)
{
    return (Ctx->Ident.Model > 0) ? PlugModelFor((ULONG)Ctx->Ident.Model) : NULL;
}

/*
 * Hosting is on for this unit: PluginHost != 0, a model that hosts (the x8p, and since P7 the Twin X
 * Gen 1: its own strip DSP, ledger and licence answer, every one per device), in Mode 2.
 */
static BOOLEAN PlugEnabled(_In_ PDEVICE_CONTEXT Ctx, _Out_writes_bytes_(WhyLen) PCHAR Why, _In_ size_t WhyLen)
{
    (VOID)RtlStringCbCopyA(Why, WhyLen, "enabled");
    if (Ctx->Plugin == NULL) {
        (VOID)RtlStringCbCopyA(Why, WhyLen, "no plug-in host on this unit (not started, or no memory)");
        return FALSE;
    }
    if (PlugModel(Ctx) == NULL || PlugStripModel(Ctx) == NULL) {
        (VOID)RtlStringCbCopyA(Why, WhyLen, "plug-in hosting is built for the x8p and the Twin X (Gen 1) only");
        return FALSE;
    }
    if (!Ctx->Unit.Active || Ctx->Unit.Value[APOLLO_PARAM_MODE] < APOLLO_MODE_FULL) {
        (VOID)RtlStringCbCopyA(Why, WhyLen, "the unit is not in Mode 2");
        return FALSE;
    }
    if (PlugRegEnabled() == 0) {
        (VOID)RtlStringCbCopyA(Why, WhyLen, "PluginHost is 0 (Services\\apollo\\Parameters)");
        return FALSE;
    }
    if (Ctx->SampleRate != APOLLO_RATE_DEFAULT_HZ) {
        /* [Rate] the recipes and the host's carrier were mapped at 48 kHz (app plugin_host.h "mono48k") */
        (VOID)RtlStringCbPrintfA(Why, WhyLen, "the unit runs at %s: plug-ins run at 48 kHz only in this build",
                                 ApolloRateText(Ctx->SampleRate));
        return FALSE;
    }
    return TRUE;
}

/*
 * A REG_SZ value of Services\apollo\Parameters, read now like PluginHost (an administrator sets or
 * clears it without a restart). Returns the buffer (paged pool, PLUG_TAG; the caller frees it) with
 * *Out pointing into it, the terminator not counted; NULL = absent: no value, not REG_SZ, empty,
 * longer than MaxCch characters, a NUL inside, or any error (fail closed).
 */
_IRQL_requires_(PASSIVE_LEVEL)
static PWCH PlugRegString(_In_ PCWSTR Name, _In_ ULONG MaxCch, _Out_ PUNICODE_STRING Out)
{
    /* one size for every value (the longest, PluginHostImage): a constant the analysis can follow */
    const ULONG bytes = (APOLLO_PLUGIN_IMAGE_CCH_MAX + 1) * sizeof(WCHAR);
    WDFKEY key = NULL;
    UNICODE_STRING name;
    PWCH buf;
    ULONG got = 0, type = REG_NONE, n, i;
    NTSTATUS status;

    RtlZeroMemory(Out, sizeof(*Out));
    if (MaxCch == 0 || MaxCch > APOLLO_PLUGIN_IMAGE_CCH_MAX) {
        return NULL;
    }
    buf = (PWCH)ExAllocatePool2(POOL_FLAG_PAGED, bytes, PLUG_TAG);
    if (buf == NULL) {
        return NULL;
    }
    status = WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &key);
    if (NT_SUCCESS(status)) {
        RtlInitUnicodeString(&name, Name);
        /* too long for the buffer: STATUS_BUFFER_OVERFLOW, absent */
        status = WdfRegistryQueryValue(key, &name, bytes, buf, &got, &type);
        WdfRegistryClose(key);
    }
    if (!NT_SUCCESS(status) || type != REG_SZ || got > bytes || (got & 1) != 0) {
        ExFreePoolWithTag(buf, PLUG_TAG);
        return NULL;
    }
    n = got / sizeof(WCHAR);
    while (n > 0 && buf[n - 1] == L'\0') {
        n--;                                    /* the terminator (REG_SZ usually has one) */
    }
    for (i = 0; i < n && buf[i] != L'\0'; i++) {
    }
    if (n == 0 || n > MaxCch || i != n) {
        ExFreePoolWithTag(buf, PLUG_TAG);
        return NULL;
    }
    Out->Buffer = buf;
    Out->Length = (USHORT)(n * sizeof(WCHAR));
    Out->MaximumLength = (USHORT)bytes;
    return buf;
}

#define PLUG_TARGET_BYTES       1024u           /* what \GLOBAL??\X: links to ("\Device\HarddiskVolume3") */
C_ASSERT(APOLLO_PLUGIN_USER_CCH_MAX <= APOLLO_PLUGIN_IMAGE_CCH_MAX);     /* PlugRegString's one buffer size */

/*
 * The device a drive letter names NOW: \GLOBAL??\X: (the global namespace only, never the
 * caller's \?? or a session's mapped drives), opened and read as a symbolic link. Returns the
 * buffer (paged pool, PLUG_TAG; the caller frees it) with *Target in it, or NULL (fail closed).
 */
_IRQL_requires_(PASSIVE_LEVEL)
static PWCH PlugDriveTarget(_In_ WCHAR Drive, _Out_ PUNICODE_STRING Target)
{
    WCHAR linkName[] = L"\\GLOBAL\x3F\x3F\\X:";        /* "\GLOBAL??\X:", no trigraph in the source */
    UNICODE_STRING link;
    OBJECT_ATTRIBUTES oa;
    HANDLE h = NULL;
    ULONG ret = 0;
    PWCH buf;
    NTSTATUS status;

    RtlZeroMemory(Target, sizeof(*Target));
    if (Drive < L'A' || Drive > L'Z') {
        return NULL;
    }
    linkName[10] = Drive;
    RtlInitUnicodeString(&link, linkName);
    InitializeObjectAttributes(&oa, &link, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    status = ZwOpenSymbolicLinkObject(&h, SYMBOLIC_LINK_QUERY, &oa);
    if (!NT_SUCCESS(status)) {
        return NULL;
    }
    buf = (PWCH)ExAllocatePool2(POOL_FLAG_PAGED, PLUG_TARGET_BYTES, PLUG_TAG);
    if (buf == NULL) {
        ZwClose(h);
        return NULL;
    }
    Target->Buffer = buf;
    Target->Length = 0;
    Target->MaximumLength = (USHORT)PLUG_TARGET_BYTES;
    status = ZwQuerySymbolicLinkObject(h, Target, &ret);
    ZwClose(h);
    if (!NT_SUCCESS(status) || Target->Length == 0 || Target->Length > Target->MaximumLength || (Target->Length & 1) != 0) {
        ExFreePoolWithTag(buf, PLUG_TAG);
        RtlZeroMemory(Target, sizeof(*Target));
        return NULL;
    }
    return buf;
}

/*
 * Is the primary token of Proc (not a thread's impersonation) that of the user PluginHostUser
 * names (a SID string, PlugSidTextEqual)? Everything taken is released; any error: no.
 */
_IRQL_requires_(PASSIVE_LEVEL)
static BOOLEAN PlugUserIs(_In_ PEPROCESS Proc, _In_ PCUNICODE_STRING Want)
{
    PACCESS_TOKEN tok = PsReferencePrimaryToken(Proc);
    PTOKEN_USER tu = NULL;
    UNICODE_STRING sid;
    BOOLEAN ok = FALSE;

    if (tok == NULL) {
        return FALSE;
    }
    if (NT_SUCCESS(SeQueryInformationToken(tok, TokenUser, (PVOID *)&tu)) && tu != NULL) {
        RtlZeroMemory(&sid, sizeof(sid));
        if (tu->User.Sid != NULL && RtlValidSid(tu->User.Sid) && NT_SUCCESS(RtlConvertSidToUnicodeString(&sid, tu->User.Sid, TRUE))) {
            ok = PlugSidTextEqual(sid.Buffer, sid.Length / sizeof(WCHAR), Want->Buffer, Want->Length / sizeof(WCHAR)) ? TRUE : FALSE;
            RtlFreeUnicodeString(&sid);
        }
        ExFreePool(tu);                         /* SeQueryInformationToken's buffer: ExFreePool, as its docs say */
    }
    PsDereferencePrimaryToken(tok);
    return ok;
}

/*
 * May this request make the plug-in calls (BOARD_QUERY, PLUGIN_INSTANCE)? PASSIVE (the queue is
 * sequential at PASSIVE); no lock held, nothing sent. Yes when
 *   (a) the handle was opened by an elevated administrator (APOLLO_FILE.IsAdmin: the opener's
 *       token, as for the admin IOCTLs; apollo-plug and step-p1.ps1 run elevated), or
 *   (b) all of: the process asking (the IRP's, whatever thread runs the queue) is the one that
 *       opened the handle (APOLLO_FILE.Creator, referenced: a handle inherited by or duplicated
 *       into another process does not count); its primary token's user is PluginHostUser (a SID
 *       string, review finding 1: another account launching the same program does not pass); and
 *       its image, as SeLocateProcessImageName names it (the NT path of the image section), is
 *       PluginHostImage, a DOS path whose drive letter is resolved now through \GLOBAL??\X:
 *       (review finding 2: a stored \Device\HarddiskVolumeN could name another volume after a
 *       reboot).
 * No file context (an ACX handle), no requestor, a value absent or malformed, any error: no.
 */
_IRQL_requires_(PASSIVE_LEVEL)
static BOOLEAN PlugCallerAllowed(_In_ WDFREQUEST Request)
{
    WDFFILEOBJECT fo = WdfRequestGetFileObject(Request);
    PAPOLLO_FILE f = (fo != NULL) ? ApolloGetFile(fo) : NULL;
    PIRP irp = WdfRequestWdmGetIrp(Request);
    PEPROCESS proc;
    PUNICODE_STRING located = NULL;
    UNICODE_STRING image, user, target;
    PWCH imageBuf = NULL, userBuf = NULL, targetBuf = NULL;
    WCHAR drive;
    BOOLEAN ok = FALSE;

    if (f == NULL) {
        return FALSE;
    }
    if (f->IsAdmin) {
        return TRUE;                            /* (a) */
    }
    proc = (irp != NULL) ? IoGetRequestorProcess(irp) : NULL;
    if (proc == NULL || f->Creator == NULL || proc != f->Creator) {
        return FALSE;                           /* not the opener: an inherited or duplicated handle */
    }
    imageBuf = PlugRegString(APOLLO_PLUGIN_REG_IMAGE, APOLLO_PLUGIN_IMAGE_CCH_MAX, &image);
    userBuf = PlugRegString(APOLLO_PLUGIN_REG_USER, APOLLO_PLUGIN_USER_CCH_MAX, &user);
    drive = (imageBuf != NULL) ? PlugImageDrive(image.Buffer, image.Length / sizeof(WCHAR)) : 0;
    /* f->Creator holds a reference: the process object cannot go while we look at it */
    if (drive != 0 && userBuf != NULL && PlugSidTextOk(user.Buffer, user.Length / sizeof(WCHAR)) && PlugUserIs(proc, &user)) {
        targetBuf = PlugDriveTarget(drive, &target);
        if (targetBuf != NULL && NT_SUCCESS(SeLocateProcessImageName(proc, &located)) && located != NULL) {
            ok = (located->Buffer != NULL &&
                  PlugImageMatch(located->Buffer, located->Length / sizeof(WCHAR), target.Buffer, target.Length / sizeof(WCHAR),
                                 image.Buffer, image.Length / sizeof(WCHAR))) ? TRUE : FALSE;
        }
    }
    if (located != NULL) {
        ExFreePool(located);                    /* SeLocateProcessImageName's buffer: ExFreePool, as its docs say */
    }
    if (targetBuf != NULL) {
        ExFreePoolWithTag(targetBuf, PLUG_TAG);
    }
    if (userBuf != NULL) {
        ExFreePoolWithTag(userBuf, PLUG_TAG);
    }
    if (imageBuf != NULL) {
        ExFreePoolWithTag(imageBuf, PLUG_TAG);
    }
    return ok;
}

/* A refused caller: counts only, at most one line per PLUG_DENIED_LOG_MS (the queue serialises the fields). */
static VOID PlugLogDenied(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _In_ ULONG Code)
{
    ULONGLONG now = KeQueryInterruptTime();

    H->Denied++;
    H->DeniedUnlogged++;
    if (now < H->DeniedLogAt) {
        return;
    }
    ApolloLogU(Ctx, "plugins: refused %lu plug-in call(s) (last: %s) from a process that is neither an administrator's "
               "handle nor PluginHostImage run by PluginHostUser (%lu since the host started): nothing locked or sent", H->DeniedUnlogged,
               Code == IOCTL_APOLLO_BOARD_QUERY ? "BOARD_QUERY" : "PLUGIN_INSTANCE", H->Denied);
    H->DeniedUnlogged = 0;
    H->DeniedLogAt = now + MS_TO_100NS(PLUG_DENIED_LOG_MS);
}

/* ------------------------------------------------------------------ */
/* Per-DSP state, ownership, quarantine (DevLock held)                 */
/* ------------------------------------------------------------------ */

static PLUG_DSP *PlugSyncDsp(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _In_ ULONG D)
{
    PLUG_DSP *d = &H->Dsp[D & (APOLLO_MAX_DSPS - 1)];

    if (d->Epoch != Ctx->Strips.Epoch) {
        /* A new bring-up or a taint: what this host knew about the DSP is over. Ownership needs a
           fresh power-up (boot word 3), so an old quarantine cannot carry over to a reused DSP. */
        d->Epoch = Ctx->Strips.Epoch;
        d->Quarantined = FALSE;
        d->LedgerValid = FALSE;
        RtlZeroMemory(d->StripBook, sizeof(d->StripBook));
    }
    return d;
}

/* The DSP was found freshly powered at this cold start and nothing raw touched it since (ua_plugin_place.c:95-160). */
static BOOLEAN PlugDspOwned(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG D)
{
    const APOLLO_STRIPS *s = &Ctx->Strips;

    if (D >= Ctx->NumDsps || D >= APOLLO_MAX_DSPS || !s->BootWordsRead || s->Tainted) {
        return FALSE;
    }
    if (Ctx->WarmStart) {
        /*
         * [Warm] docs/STRIPS.md 12.5: at a warm start only the strip DSP whose strips this start adopted or
         * rebuilt from a record that proves only this driver used it since the last power-up and saw no
         * plug-in left on it, with StripWarmPlugins on (strips.c PlugAdoptOk). The x8p's adopted DSP was not
         * ring-connected at this start (session byte 0x07): its first acknowledged upload proves the session
         * (ApolloStripsUploadProven), an unanswered one quarantines it (PlugQuarantine).
         */
        return (D == s->Dsp && (s->Adopted || s->RebuiltWarm) && s->PlugAdoptOk && s->Owned && !s->Quarantined) ?
               TRUE : FALSE;
    }
    if (!StripOwned(s->BootWord[D])) {
        return FALSE;
    }
    if (D == s->Dsp && (!s->Owned || s->Quarantined)) {
        return FALSE;
    }
    return TRUE;
}

static BOOLEAN PlugRingsLive(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG D)
{
    return D < Ctx->NumDsps && D < APOLLO_MAX_DSPS && Ctx->Cmd[D].Allocated && Ctx->Cmd[D].Live &&
           Ctx->Resp[D].Allocated && Ctx->Resp[D].Live;
}

/* May anything more be placed on (or sent to the placements on) DSP D? Never DSP 0, the mixer's. */
static BOOLEAN PlugDspAdmitted(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _In_ ULONG D)
{
    PLUG_DSP *d;

    if (D == 0 || D >= Ctx->NumDsps || D >= APOLLO_MAX_DSPS || !Ctx->Connected || ApolloGateClosed(Ctx)) {
        return FALSE;
    }
    d = PlugSyncDsp(Ctx, H, D);
    return PlugDspOwned(Ctx, D) && !d->Quarantined && PlugRingsLive(Ctx, D);
}

/*
 * The strip DSP takes plug-ins only once its strip pass is over: the strips Live, or in a final
 * state (Off, Failed). Idle (a pass not armed yet), Armed and Building refuse (review P0 finding 3).
 */
static BOOLEAN PlugStripsSettled(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG D)
{
    ULONG st = Ctx->Strips.State;

    return D != Ctx->Strips.Dsp || st == APOLLO_STRIPS_STATE_LIVE || st == APOLLO_STRIPS_STATE_OFF ||
           st == APOLLO_STRIPS_STATE_FAILED;
}

/*
 * An outcome nobody can prove: nothing more is placed on DSP D until it is power-cycled. On the
 * strip DSP the strips are quarantined with it (as strips.c StripsQuarantine does).
 */
static VOID PlugQuarantine(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _In_ ULONG D, _In_z_ PCSTR What,
                           _In_ NTSTATUS Status)
{
    APOLLO_STRIPS *s = &Ctx->Strips;
    PLUG_DSP *d;

    if (D >= APOLLO_MAX_DSPS) {
        return;
    }
    d = PlugSyncDsp(Ctx, H, D);
    if (!d->Quarantined) {
        ApolloLogU(Ctx, "plugins: DSP %lu quarantined until the Apollo is power-cycled: %s (0x%08x)", D, What, Status);
        ApolloEventPost(Ctx, APOLLO_EVENT_DSP_FAULT, D, (ULONG)Status);          /* [Events] */
    }
    d->Quarantined = TRUE;
    if (D == s->Dsp && s->WarmUploads == STRIPS_WARM_UPLOADS_UNPROVEN) {
        /* [Warm] 12.5: the first plug-in transaction of this warm session went unproven: no uploads there until a power cycle */
        s->WarmUploads = STRIPS_WARM_UPLOADS_FAILED;
        ApolloLogU(Ctx, "plugins: DSP %lu did not prove the first plug-in step after this warm start (adopted strips, no ring "
                   "connect, boot word %08x): power-cycle the Apollo for plug-ins; StripWarmPlugins=0 keeps them off after "
                   "warm starts", D, s->PostBoot[D]);
    }
    if (D == s->Dsp && !s->Quarantined && s->Enable != 0) {
        s->Quarantined = TRUE;
        s->State = APOLLO_STRIPS_STATE_FAILED;
        (VOID)RtlStringCbPrintfA(s->Why, sizeof(s->Why), "DSP %lu quarantined by a plug-in: %s (0x%08x); power-cycle the Apollo",
                                 D, What, Status);
        ApolloLogU(Ctx, "strips: %s", s->Why);
    }
    if (D == s->Dsp) {
        ApolloStripsRecordLost(Ctx, "plug-in quarantine");     /* [Warm] no adoption until a power cycle */
    }
}

/* [Warm] Instances placed on the strip DSP now (DevLock held): the strips' record keeps the count. */
static VOID PlugNoteStripDsp(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H)
{
    ULONG i, n = 0;

    for (i = 0; i < APOLLO_PLUGIN_INSTANCES_MAX; i++) {
        const PLUG_INST *in = H->Inst[i];

        if (in != NULL && in->Placed && !in->Lost && in->Dsp == Ctx->Strips.Dsp && in->Epoch == Ctx->Strips.Epoch) {
            n++;
        }
    }
    ApolloStripsRecordPlugins(Ctx, n);
}

/*
 * [Lat] StripLock held: the worst routed input chain's record path over the plain strip's (op LATENCY) for this strips'
 * epoch, for the reported latencies (asio.c ApolloLatencyRefresh publishes it within 10 ms).
 */
static VOID PlugLatencyNote(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H)
{
    ULONG i, worst = 0, epoch = Ctx->Strips.Epoch;

    for (i = 0; i < APOLLO_PLUGIN_INSTANCES_MAX; i++) {
        const PLUG_INST *in = H->Inst[i];

        if (in != NULL && in->Placed && in->Started && !in->Lost && in->StripOverride && in->AuxBus == 0 &&
            in->Epoch == epoch && in->Dsp == Ctx->Strips.Dsp && in->RecExcess > worst) {
            worst = in->RecExcess;
        }
    }
    InterlockedExchange(&Ctx->LatChainExcess, (LONG)worst);
    InterlockedExchange(&Ctx->LatChainEpoch, (LONG)epoch);
}

/*
 * [Warm] DevLock held: why DSP D is not this host's now (APOLLO_PLUGIN_WHY_*, GET_PLUGIN_CAPS Dsp[].Flags 15:8), so
 * Apollo Control can say what keeps the saved inserts waiting and the one action that ends it.
 */
static ULONG PlugOwnWhy(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG D)
{
    const APOLLO_STRIPS *s = &Ctx->Strips;

    if (D >= Ctx->NumDsps || D >= APOLLO_MAX_DSPS) {
        return APOLLO_PLUGIN_WHY_NONE;
    }
    if (!s->BootWordsRead) {
        return APOLLO_PLUGIN_WHY_NOT_READ;
    }
    if (s->Tainted) {
        return APOLLO_PLUGIN_WHY_TAINTED;
    }
    if (D == s->Dsp && s->WarmUploads == STRIPS_WARM_UPLOADS_FAILED) {
        return APOLLO_PLUGIN_WHY_WARM_UNANSWERED;
    }
    if (!Ctx->WarmStart) {
        if (!StripOwned(s->BootWord[D])) {
            return APOLLO_PLUGIN_WHY_HOST_USED;
        }
        return (D == s->Dsp) ? APOLLO_PLUGIN_WHY_STRIPS : APOLLO_PLUGIN_WHY_NONE;
    }
    if (D != s->Dsp) {
        return APOLLO_PLUGIN_WHY_NONE;          /* a warm start never owns another DSP */
    }
    if (s->Quarantined) {
        return APOLLO_PLUGIN_WHY_STRIPS;
    }
    if (s->AdoptPending || (s->RebuiltWarm && s->State != APOLLO_STRIPS_STATE_LIVE)) {
        return APOLLO_PLUGIN_WHY_WARM_PENDING;
    }
    if (!s->Adopted && !s->RebuiltWarm) {
        return APOLLO_PLUGIN_WHY_WARM_NOT_ADOPTED;
    }
    if (!s->WarmPlugins) {
        return APOLLO_PLUGIN_WHY_WARM_OFF;
    }
    if (s->AdoptVerdict == APOLLO_STRIPS_ADOPT_RESTORE || (s->RecFound && s->Rec.Plugins != 0)) {
        return APOLLO_PLUGIN_WHY_WARM_PLUGIN_LEFT;
    }
    return (!s->Owned || !s->PlugAdoptOk) ? APOLLO_PLUGIN_WHY_WARM_NOT_ADOPTED : APOLLO_PLUGIN_WHY_NONE;
}

/* ------------------------------------------------------------------ */
/* Transaction pages                                                   */
/* ------------------------------------------------------------------ */

/* StripLock held. A zeroed 4 KiB page: a free one, or a new common buffer below 4 GiB. */
static NTSTATUS PlugPageGet(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _Out_ UCHAR *Idx)
{
    ULONG i;

    *Idx = 0;
    for (i = 0; i < PLUG_PAGES_MAX; i++) {
        if (H->Page[i].State == PAGE_FREE) {
            RtlZeroMemory(H->Page[i].Va, PLUG_PAGE);
            H->Page[i].State = PAGE_USED;
            *Idx = (UCHAR)i;
            return STATUS_SUCCESS;
        }
    }
    for (i = 0; i < PLUG_PAGES_MAX; i++) {
        PLUG_PAGEBUF *p = &H->Page[i];
        PHYSICAL_ADDRESS la;
        NTSTATUS status;

        if (p->State != PAGE_NONE) {
            continue;
        }
        status = WdfCommonBufferCreate(Ctx->DmaEnabler, PLUG_PAGE, WDF_NO_OBJECT_ATTRIBUTES, &p->Buffer);
        if (!NT_SUCCESS(status)) {
            p->Buffer = NULL;
            return status;
        }
        p->Va = (PUCHAR)WdfCommonBufferGetAlignedVirtualAddress(p->Buffer);
        la = WdfCommonBufferGetAlignedLogicalAddress(p->Buffer);
        p->La = (ULONGLONG)la.QuadPart;
        if ((p->La & (PLUG_PAGE - 1)) != 0 || p->La + PLUG_PAGE > 0x100000000ULL || p->Va == NULL) {
            WdfObjectDelete(p->Buffer);
            p->Buffer = NULL;
            p->Va = NULL;
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(p->Va, PLUG_PAGE);
        p->State = PAGE_USED;
        *Idx = (UCHAR)i;
        return STATUS_SUCCESS;
    }
    return STATUS_INSUFFICIENT_RESOURCES;
}

static VOID PlugPagePut(_In_ APOLLO_PLUGIN_HOST *H, _In_ UCHAR Idx, _In_ BOOLEAN Park)
{
    if (Idx < PLUG_PAGES_MAX && H->Page[Idx].State == PAGE_USED) {
        H->Page[Idx].State = Park ? PAGE_PARKED : PAGE_FREE;
    }
}

/* ------------------------------------------------------------------ */
/* Transactions (ua_dsp_xact.c), one at a time: H->X                   */
/* ------------------------------------------------------------------ */

/* StripLock held: an empty transaction for DSP Dsp in Epoch with PayBytes of payload pages. */
static NTSTATUS PxBegin(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _In_ ULONG Dsp, _In_ ULONG Epoch,
                        _In_ ULONG PayBytes, _Outptr_result_maybenull_ PLUG_XACT **Out)
{
    PLUG_XACT *x = &H->X;
    ULONG pages = (PayBytes + PLUG_PAGE - 1) / PLUG_PAGE, i;
    NTSTATUS status = STATUS_SUCCESS;

    *Out = NULL;
    if (x->State != PX_FREE || pages > PLUG_PAY_PAGES_MAX || Dsp >= APOLLO_MAX_DSPS) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    RtlZeroMemory(x, FIELD_OFFSET(PLUG_XACT, L));
    x->State = PX_BUILDING;
    x->Dsp = Dsp;
    x->Epoch = Epoch;
    x->NPay = 0;
    x->NRxPages = 0;
    RtlZeroMemory(&x->Pay, sizeof(x->Pay));
    RtlZeroMemory(x->Ref, sizeof(x->Ref));
    x->PublishedAt = 0;
    for (i = 0; i < pages && NT_SUCCESS(status); i++) {
        status = PlugPageGet(Ctx, H, &x->PayIdx[i]);
        if (NT_SUCCESS(status)) {
            x->Pay.Page[i] = H->Page[x->PayIdx[i]].Va;
            x->NPay = i + 1;
        }
    }
    x->Pay.NPages = x->NPay;
    PlugXlInit(&x->L, x->NPay * PLUG_PAGE);
    if (!NT_SUCCESS(status)) {
        for (i = 0; i < x->NPay; i++) {
            PlugPagePut(H, x->PayIdx[i], FALSE);
        }
        x->State = PX_FREE;
        return status;
    }
    *Out = x;
    return STATUS_SUCCESS;
}

/* StripLock held: the reply pages, prefilled. Nothing is published yet. */
static NTSTATUS PxMap(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _Inout_ PLUG_XACT *X)
{
    ULONG n = PlugXlRxPages(&X->L), i, k;
    NTSTATUS status;

    if (X->State != PX_BUILDING) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (X->L.Err != PLUG_OK) {
        return PlugErr(X->L.Err);
    }
    if (X->L.N == 0 && X->L.NRx == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if (n > PLUG_RX_PAGES_MAX) {
        return STATUS_BUFFER_OVERFLOW;
    }
    for (i = X->NRxPages; i < n; i++) {
        status = PlugPageGet(Ctx, H, &X->RxIdx[i]);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        X->NRxPages = i + 1;
        for (k = 0; k < PLUG_PAGE / 4; k++) {
            ((PULONG)H->Page[X->RxIdx[i]].Va)[k] = X->L.RxFill;
        }
    }
    return STATUS_SUCCESS;
}

static ULONGLONG PxPayLa(_In_ APOLLO_PLUGIN_HOST *H, _In_ const PLUG_XACT *X, _In_ ULONG Off)
{
    ULONG pg = Off / PLUG_PAGE;

    return (pg < X->NPay) ? H->Page[X->PayIdx[pg]].La + (Off % PLUG_PAGE) : 0;
}

static ULONGLONG PxRxLa(_In_ APOLLO_PLUGIN_HOST *H, _In_ const PLUG_XACT *X, _In_ ULONG Off)
{
    ULONG pg = Off / PLUG_PAGE;

    return (pg < X->NRxPages) ? H->Page[X->RxIdx[pg]].La + (Off % PLUG_PAGE) : 0;
}

/* Word W of reply K as the device left it (0 when out of range). */
static ULONG PxReplyWord(_In_ APOLLO_PLUGIN_HOST *H, _In_ const PLUG_XACT *X, _In_ ULONG K, _In_ ULONG W)
{
    ULONG off, pg;

    if (K >= X->L.NRx || W >= X->L.RxWords[K]) {
        return 0;
    }
    off = X->L.RxOff[K] + W * 4;
    pg = off / PLUG_PAGE;
    if (pg >= X->NRxPages) {
        return 0;
    }
    return *(volatile const ULONG *)(H->Page[X->RxIdx[pg]].Va + (off % PLUG_PAGE));
}

/*
 * DevLock held. All or nothing; replies first, then the command (ua_dsp_xact.c:249-365).
 * STATUS_RETRY: no room yet, or a reply transaction found the command ring busy.
 */
static NTSTATUS PxSubmit(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _Inout_ PLUG_XACT *X)
{
    PAPOLLO_RING cmd, resp;
    ULONG k;

    if (X->State != PX_BUILDING) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (ApolloGateClosed(Ctx)) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    if (X->Epoch != Ctx->Strips.Epoch) {
        return STATUS_CANCELLED;
    }
    if (X->Dsp >= Ctx->NumDsps || X->Dsp >= APOLLO_MAX_DSPS || X->L.N == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    cmd = &Ctx->Cmd[X->Dsp];
    resp = &Ctx->Resp[X->Dsp];
    if (!cmd->Allocated || !cmd->Live || (X->L.NRx && (!resp->Allocated || !resp->Live))) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (X->L.N > RING_ENTRIES / 4 || X->L.NRx > RING_ENTRIES / 4) {
        return STATUS_BUFFER_OVERFLOW;
    }
    if (!NT_SUCCESS(ApolloRingRefresh(Ctx, cmd))) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    if (ApolloRingFreeSlots(cmd) < X->L.N) {
        return STATUS_RETRY;
    }
    if (X->L.NRx) {
        if (X->Dsp == Ctx->Strips.Dsp && Ctx->Strips.ReplyWaiting) {
            return STATUS_DEVICE_BUSY;          /* a strip reply is outstanding there */
        }
        if (ApolloRingPending(cmd) != 0) {
            return STATUS_RETRY;                /* nothing ahead may answer into our replies */
        }
        if (!NT_SUCCESS(ApolloRingRefresh(Ctx, resp))) {
            return STATUS_DEVICE_NOT_CONNECTED;
        }
        if (ApolloRingPending(resp) != 0) {
            /* an unanswered descriptor (ours, parked, or a resync probe's spares): abandon it */
            ApolloLogU(Ctx, "plugins: DSP %lu response ring had %lu unanswered descriptor(s): resynced", X->Dsp,
                       ApolloRingPending(resp));
            (VOID)ApolloRingResyncNow(Ctx, resp);
            if (ApolloRingPending(resp) != 0) {
                return STATUS_RETRY;
            }
        }
        if (ApolloRingFreeSlots(resp) < X->L.NRx) {
            return STATUS_RETRY;
        }
    }
    for (k = 0; k < X->L.NRx && k < PLUG_XACT_RX_MAX; k++) {
        ULONGLONG a = PxRxLa(H, X, X->L.RxOff[k]);

        if (a == 0) {
            return STATUS_INVALID_PARAMETER;
        }
    }
    for (k = 0; k < X->L.N && k < PLUG_XACT_ENTS_MAX; k++) {
        if ((X->L.Ent[k][0] & RING_DMA_REF) && PxPayLa(H, X, X->L.Ent[k][2]) == 0) {
            return STATUS_INVALID_PARAMETER;
        }
    }
    for (k = 0; k < X->L.NRx && k < PLUG_XACT_RX_MAX; k++) {
        ULONGLONG a = PxRxLa(H, X, X->L.RxOff[k]);

        ApolloRingWrite(resp, RING_DMA_REF | X->L.RxWords[k], 0, (ULONG)a, (ULONG)(a >> 32));
    }
    for (k = 0; k < X->L.N && k < PLUG_XACT_ENTS_MAX; k++) {
        ULONG w2 = X->L.Ent[k][2], w3 = X->L.Ent[k][3];

        if (X->L.Ent[k][0] & RING_DMA_REF) {
            ULONGLONG a = PxPayLa(H, X, w2);

            w2 = (ULONG)a;
            w3 = (ULONG)(a >> 32);
        }
        ApolloRingWrite(cmd, X->L.Ent[k][0], X->L.Ent[k][1], w2, w3);
    }
    /* ApolloRingDoorbell's barrier orders the payload, the reply area and the slots before the MMIO writes. */
    if (X->L.NRx) {
        ApolloRingDoorbell(Ctx, resp);
        X->Ref[1].Pending = TRUE;
        X->Ref[1].Gen = resp->Gen;
        X->Ref[1].End = resp->WritePtr;
    }
    ApolloRingDoorbell(Ctx, cmd);
    X->Ref[0].Pending = TRUE;
    X->Ref[0].Gen = cmd->Gen;
    X->Ref[0].End = cmd->WritePtr;
    X->State = PX_PUBLISHED;
    X->PublishedAt = KeQueryInterruptTime();
    PlugSyncDsp(Ctx, H, X->Dsp)->Xacts++;
    return STATUS_SUCCESS;
}

#define POLL_DONE               1
#define POLL_PENDING            0
#define POLL_GONE               (-1)
#define POLL_STALE              (-2)

/* DevLock held (ua_dsp_xact.c:367-447). */
static LONG PxPoll(_In_ PDEVICE_CONTEXT Ctx, _Inout_ PLUG_XACT *X)
{
    ULONG k;

    if (X->State == PX_RETIRED) {
        return POLL_DONE;
    }
    if (X->State != PX_PUBLISHED || X->Dsp >= APOLLO_MAX_DSPS) {
        return POLL_STALE;
    }
    if (ApolloGateClosed(Ctx)) {
        return POLL_GONE;
    }
    if (X->Epoch != Ctx->Strips.Epoch) {
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
        dEnd = (r->End - ring->Position) & RING_INDEX_MASK;
        if (dEnd == 0 || dEnd > ApolloRingPending(ring)) {
            r->Pending = FALSE;
        }
    }
    if (X->Ref[0].Pending || X->Ref[1].Pending) {
        return POLL_PENDING;
    }
    KeMemoryBarrier();                          /* POSITION moved: the reply words are in */
    X->State = PX_RETIRED;
    return POLL_DONE;
}

/* No lock held but StripLock. Publish, retrying a full or busy ring for 50 ms with DevLock released. */
static NTSTATUS PxPublish(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _Inout_ PLUG_XACT *X)
{
    ULONGLONG until = KeQueryInterruptTime() + MS_TO_100NS(PLUG_PUBLISH_MS);
    NTSTATUS status;

    for (;;) {
        WdfWaitLockAcquire(Ctx->DevLock, NULL);
        status = PxSubmit(Ctx, H, X);
        WdfWaitLockRelease(Ctx->DevLock);
        if (status != STATUS_RETRY || KeQueryInterruptTime() > until) {
            break;
        }
        ApolloSleepMs(1);
    }
    return status == STATUS_RETRY ? STATUS_DEVICE_BUSY : status;
}

/* StripLock held. DevLock is taken for one poll at a time; the poll comes before the deadline check. */
static NTSTATUS PxWait(_In_ PDEVICE_CONTEXT Ctx, _Inout_ PLUG_XACT *X, _In_ ULONG TimeoutMs)
{
    ULONGLONG until = X->PublishedAt + MS_TO_100NS(TimeoutMs);
    LONG r;

    for (;;) {
        WdfWaitLockAcquire(Ctx->DevLock, NULL);
        r = PxPoll(Ctx, X);
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
static VOID PxReport(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _In_ const PLUG_XACT *X,
                     _In_z_ PCSTR What, _In_ NTSTATUS Status)
{
    ULONG d = X->Dsp & (APOLLO_MAX_DSPS - 1);

    ApolloLogU(Ctx, "plugins: DSP %lu %s (%lu entries, %lu replies) ended 0x%08x after %lu ms: cmd end %lu pos %lu, "
               "resp end %lu pos %lu, reply word 0x%08x", d, What, X->L.N, X->L.NRx, Status,
               PlugMsSince(X->PublishedAt), X->Ref[0].End, Ctx->Cmd[d].Position, X->Ref[1].End,
               Ctx->Resp[d].Position, PxReplyWord(H, X, 0, 0));
}

/*
 * StripLock held, DevLock not (ua_dsp_xact.c:531-552): map, publish, wait. An error before
 * publication changed nothing on the DSP. After it, an outcome that cannot be proven quarantines
 * the DSP (not for a NoQuarantine transaction). The caller ends X with PxEnd.
 */
static NTSTATUS PxRun(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _Inout_ PLUG_XACT *X, _In_ ULONG TimeoutMs,
                      _In_z_ PCSTR What)
{
    NTSTATUS status = PxMap(Ctx, H, X);

    if (NT_SUCCESS(status)) {
        status = PxPublish(Ctx, H, X);
    }
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = PxWait(Ctx, X, TimeoutMs);
    if (!NT_SUCCESS(status) && status != STATUS_DEVICE_NOT_CONNECTED) {
        WdfWaitLockAcquire(Ctx->DevLock, NULL);
        PxReport(Ctx, H, X, What, status);
        if (!X->NoQuarantine && X->Epoch == Ctx->Strips.Epoch) {
            PlugSyncDsp(Ctx, H, X->Dsp)->Timeouts++;
            PlugQuarantine(Ctx, H, X->Dsp, "a transaction ended unproven", status);
        }
        WdfWaitLockRelease(Ctx->DevLock);
    }
    return status;
}

/* StripLock held. Never published or retired: pages freed. Published, unproven: pages parked for good. */
static VOID PxEnd(_In_ APOLLO_PLUGIN_HOST *H, _Inout_ PLUG_XACT *X)
{
    BOOLEAN park = (X->State == PX_PUBLISHED);
    ULONG i;

    for (i = 0; i < X->NPay; i++) {
        PlugPagePut(H, X->PayIdx[i], park);
    }
    for (i = 0; i < X->NRxPages; i++) {
        PlugPagePut(H, X->RxIdx[i], park);
    }
    X->NPay = 0;
    X->NRxPages = 0;
    X->State = PX_FREE;
}

/* ------------------------------------------------------------------ */
/* The board query and the admit decision (ua_plugin_license.c)        */
/* ------------------------------------------------------------------ */

/*
 * StripLock held. One transaction on DSP 0; Words (PLUG_BQ_WORDS) only written on success.
 * Diag (may be NULL): Reply0, Reply1, Cookie, WordsWritten, ElapsedMs. Never logs the table.
 */
static NTSTATUS PlugBoardQuery(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H,
                               _Out_writes_(PLUG_BQ_WORDS) PULONG Words, _Out_ PULONG Date, _Out_ PULONG Epoch,
                               _Out_opt_ PAPOLLO_BOARD_QUERY Diag)
{
    LARGE_INTEGER now;
    PLUG_XACT *x = NULL;
    NTSTATUS status;
    ULONG cookie, i, r0, r1, written = 0, e;

    *Date = 0;
    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    *Epoch = e = Ctx->Strips.Epoch;
    H->BoardQueries++;
    if (!Ctx->Connected || !PlugRingsLive(Ctx, 0) || ApolloGateClosed(Ctx)) {
        H->BoardQueryFailures++;
        WdfWaitLockRelease(Ctx->DevLock);
        return STATUS_DEVICE_NOT_READY;
    }
    WdfWaitLockRelease(Ctx->DevLock);

    KeQuerySystemTimePrecise(&now);
    if (PlugDate((ULONGLONG)now.QuadPart, Date) != PLUG_OK) {
        H->BoardQueryFailures++;
        ApolloLogU(Ctx, "plugins: board query not sent: the system clock is before 2007");
        return STATUS_INVALID_DEVICE_STATE;
    }
    cookie = 0x5A000000u | ((++H->CookieSeq) & 0x00FFFFFFu);
    status = PxBegin(Ctx, H, 0, e, 0, &x);
    if (!NT_SUCCESS(status) || x == NULL) {
        H->BoardQueryFailures++;
        return NT_SUCCESS(status) ? STATUS_INSUFFICIENT_RESOURCES : status;
    }
    x->NoQuarantine = TRUE;
    (VOID)PlugBuildBoardQuery(&x->L, *Date, cookie);
    status = PxRun(Ctx, H, x, PLUG_BQ_WAIT_MS, "board query");

    r0 = PxReplyWord(H, x, 0, 0);
    r1 = PxReplyWord(H, x, 0, 1);
    if (x->NRxPages != 0) {
        for (i = 0; i < PLUG_BQ_RX_WORDS; i++) {
            if (PxReplyWord(H, x, 0, i) != PLUG_BQ_SENTINEL) {
                written++;
            }
        }
    }
    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (NT_SUCCESS(status)) {
        if (r0 != PLUG_BQ_REPLY || r1 != cookie) {
            status = STATUS_DEVICE_PROTOCOL_ERROR;
        } else {
            for (i = 0; i < PLUG_BQ_WORDS; i++) {
                Words[i] = PxReplyWord(H, x, 0, 2 + i);
            }
            if (Ctx->Strips.Epoch != e || ApolloGateClosed(Ctx)) {
                status = STATUS_CANCELLED;          /* accepted only if nothing moved on meanwhile */
            }
        }
    }
    if (!NT_SUCCESS(status)) {
        H->BoardQueryFailures++;
        ApolloLogU(Ctx, "plugins: board query on DSP 0 failed 0x%08x after %lu ms: reply %08x %08x (cookie %08x), "
                   "%lu reply words written", status, x->State == PX_BUILDING ? 0 : PlugMsSince(x->PublishedAt),
                   r0, r1, cookie, written);
    }
    if (Diag != NULL) {
        Diag->Reply0 = r0;
        Diag->Reply1 = r1;
        Diag->Cookie = cookie;
        Diag->WordsWritten = written;
        Diag->ElapsedMs = (x->State == PX_BUILDING) ? 0 : PlugMsSince(x->PublishedAt);
    }
    WdfWaitLockRelease(Ctx->DevLock);
    PxEnd(H, x);
    return status;
}

/* The table's states as counts (the only thing about the table that is ever logged). */
static VOID PlugLogCounts(_In_ PDEVICE_CONTEXT Ctx, _In_reads_(PLUG_BQ_WORDS) const ULONG *Words, _In_ ULONG Date)
{
    ULONG n[5] = { 0, 0, 0, 0, 0 }, i;

    for (i = 0; i < PLUG_BQ_WORDS; i++) {
        ULONG st = PlugLicenseState(Words[i]);

        if (st < 5) {
            n[st]++;
        }
    }
    ApolloLogU(Ctx, "plugins: board table (date 0x%08x): %lu authorized, %lu demo running, %lu demo not started, "
               "%lu expired, %lu unavailable", Date, n[0], n[1], n[2], n[3], n[4]);
}

/*
 * StripLock held. A fresh answer for every id in Ids (one query): STATUS_SUCCESS only when it
 * admits them all; STATUS_ACCESS_DENIED for a refusal or an id outside the table; the query's
 * error when there is no answer. *State and *Word: the first refused id's, else Ids[0]'s.
 */
static NTSTATUS PlugAdmit(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _In_reads_(N) const ULONG *Ids,
                          _In_ ULONG N, _Out_ PULONG State, _Out_ PULONG Word, _Out_ PULONG Epoch)
{
    ULONG date = 0, i;
    NTSTATUS status;

    *State = PLUG_LIC_UNKNOWN;
    *Word = 0;
    *Epoch = 0;
    if (N == 0 || N > 5) {
        return STATUS_INVALID_PARAMETER;
    }
    for (i = 0; i < N; i++) {
        if (Ids[i] < PLUG_BQ_BASE || Ids[i] >= PLUG_BQ_BASE + PLUG_BQ_WORDS) {
            H->Refused++;
            ApolloLogU(Ctx, "plugins: plug-in 0x%03lx is outside the licence table: refused, nothing placed", Ids[i]);
            return STATUS_ACCESS_DENIED;
        }
    }
    status = PlugBoardQuery(Ctx, H, H->Bq, &date, Epoch, NULL);
    if (!NT_SUCCESS(status)) {
        H->Refused++;
        ApolloLogU(Ctx, "plugins: plug-in 0x%03lx: no licence answer (0x%08x): refused, nothing placed", Ids[0], status);
        return status;
    }
    PlugLogCounts(Ctx, H->Bq, date);
    for (i = 0; i < N; i++) {
        *Word = H->Bq[Ids[i] - PLUG_BQ_BASE];
        *State = PlugLicenseState(*Word);
        if (!PlugLicenseAdmits(*State)) {
            break;
        }
    }
    if (i < N) {
        H->Refused++;
        ApolloLogU(Ctx, "plugins: plug-in 0x%03lx refused by the device (state %lu): nothing placed", Ids[i], *State);
        return STATUS_ACCESS_DENIED;
    }
    *Word = H->Bq[Ids[0] - PLUG_BQ_BASE];
    *State = PlugLicenseState(*Word);
    H->Admitted++;
    ApolloLogU(Ctx, "plugins: plug-in 0x%03lx admitted by the device (state %lu)%s", Ids[0], *State,
               N > 1 ? " with the rest of its chain" : "");
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* The module catalog (ua_plugin_catalog.c), StripLock held            */
/* ------------------------------------------------------------------ */

/* Reads Path into a new paged buffer (at most PLUG_BILL_MAX bytes). */
static NTSTATUS PlugReadFile(_In_z_ PCWSTR Path, _Outptr_result_bytebuffer_maybenull_(*Len) PUCHAR *Out, _Out_ PULONG Len)
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    FILE_STANDARD_INFORMATION fsi;
    LARGE_INTEGER offset;
    HANDLE h = NULL;
    PUCHAR buf = NULL;
    NTSTATUS status;
    ULONG len = 0;

    *Out = NULL;
    *Len = 0;
    RtlInitUnicodeString(&path, Path);
    InitializeObjectAttributes(&oa, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    /* FILE_COMPLETE_IF_OPLOCKED: never wait for someone else's oplock break (as strips.c). */
    status = ZwCreateFile(&h, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                          FILE_OPEN, FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_COMPLETE_IF_OPLOCKED,
                          NULL, 0);
    if (status == STATUS_OPLOCK_BREAK_IN_PROGRESS) {
        ZwClose(h);
        return STATUS_SHARING_VIOLATION;
    }
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = ZwQueryInformationFile(h, &iosb, &fsi, sizeof(fsi), FileStandardInformation);
    if (NT_SUCCESS(status) && (fsi.EndOfFile.QuadPart <= 0 || fsi.EndOfFile.QuadPart > (LONGLONG)PLUG_BILL_MAX)) {
        status = STATUS_FILE_TOO_LARGE;
    }
    if (NT_SUCCESS(status)) {
        len = (ULONG)fsi.EndOfFile.QuadPart;
        buf = (PUCHAR)ExAllocatePool2(POOL_FLAG_PAGED, len, PLUG_TAG);
        if (buf == NULL) {
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    if (NT_SUCCESS(status)) {
        offset.QuadPart = 0;
        status = ZwReadFile(h, NULL, NULL, NULL, &iosb, buf, len, &offset, NULL);
        if (NT_SUCCESS(status) && iosb.Information != len) {
            status = STATUS_END_OF_FILE;
        }
    }
    ZwClose(h);
    if (!NT_SUCCESS(status)) {
        if (buf != NULL) {
            ExFreePoolWithTag(buf, PLUG_TAG);
        }
        return status;
    }
    *Out = buf;
    *Len = len;
    return STATUS_SUCCESS;
}

/*
 * The record of entry E, read and checked as UAD2System's IsValid does: from
 * ApolloModules\<sha256>.bill (a pinned strip module: ApolloStrips\<id>.bill), its SHA-256 the
 * name it was asked for, a valid Bill header, and the same record as before if it was read before.
 */
static NTSTATUS PlugCatLoad(_In_ PDEVICE_CONTEXT Ctx, _Inout_ PLUG_CAT *E)
{
    static const CHAR hex[] = "0123456789abcdef";
    WCHAR path[200], name[70];
    BCRYPT_ALG_HANDLE alg = NULL;
    UCHAR sha[32];
    PUCHAR blob = NULL;
    ULONG len = 0, i;
    PLUG_BILL b;
    NTSTATUS status;

    RtlZeroMemory(&b, sizeof(b));
    if (E->Blob != NULL) {
        return STATUS_SUCCESS;
    }
    for (i = 0; i < 32; i++) {
        name[2 * i] = (WCHAR)hex[E->Sha[i] >> 4];
        name[2 * i + 1] = (WCHAR)hex[E->Sha[i] & 15];
    }
    name[64] = L'\0';
    if (E->StripPin >= 0 && E->StripPin < APOLLO_STRIP_MODULES) {
        status = RtlStringCbPrintfW(path, sizeof(path), L"%ws%ws", APOLLO_STRIP_DIR_NT,
                                    g_ApolloStripPins[E->StripPin].FileName);
    } else {
        status = RtlStringCbPrintfW(path, sizeof(path), L"%ws%ws.bill", APOLLO_MODULE_DIR_NT, name);
    }
    if (NT_SUCCESS(status)) {
        status = PlugReadFile(path, &blob, &len);
    }
    if (NT_SUCCESS(status) && blob == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
    }
    if (NT_SUCCESS(status)) {
        status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    }
    if (NT_SUCCESS(status) && blob != NULL) {
        status = BCryptHash(alg, NULL, 0, blob, len, sha, sizeof(sha));
        (VOID)BCryptCloseAlgorithmProvider(alg, 0);
    }
    if (NT_SUCCESS(status) && RtlCompareMemory(sha, E->Sha, sizeof(sha)) != sizeof(sha)) {
        status = STATUS_INVALID_IMAGE_HASH;
    }
    if (NT_SUCCESS(status) && PlugBillCheck(blob, len, &b) != PLUG_OK) {
        status = STATUS_INVALID_IMAGE_FORMAT;
    }
    if (NT_SUCCESS(status) && E->Len != 0 && (E->Len != len || E->Bill.Id != b.Id || E->Bill.W2 != b.W2)) {
        status = STATUS_INVALID_IMAGE_FORMAT;
    }
    if (!NT_SUCCESS(status)) {
        if (blob != NULL) {
            ExFreePoolWithTag(blob, PLUG_TAG);
        }
        ApolloLogU(Ctx, "plugins: module %.16ws.. refused (0x%08x): %s", name, status,
                   status == STATUS_OBJECT_NAME_NOT_FOUND || status == STATUS_OBJECT_PATH_NOT_FOUND ?
                   "not in the module store (apollo-plug modules install)" :
                   status == STATUS_INVALID_IMAGE_HASH ? "its SHA-256 is not its name" :
                   status == STATUS_INVALID_IMAGE_FORMAT ? "not a module record UAD2System would register" :
                   status == STATUS_FILE_TOO_LARGE ? "longer than one upload" : "could not be read");
        return status;
    }
    E->Blob = blob;
    E->Len = len;
    E->Bill = b;
    return STATUS_SUCCESS;
}

static VOID PlugCatPut(_In_ APOLLO_PLUGIN_HOST *H, _In_ ULONG Idx)
{
    PLUG_CAT *e;

    if (Idx >= PLUG_CATALOG_MAX || H->Cat[Idx].Users == 0) {
        return;
    }
    e = &H->Cat[Idx];
    if (--e->Users != 0) {
        return;
    }
    if (e->Blob != NULL) {
        ExFreePoolWithTag(e->Blob, PLUG_TAG);
    }
    RtlZeroMemory(e, sizeof(*e));
}

static VOID PlugCatDropBlobs(_In_ APOLLO_PLUGIN_HOST *H)
{
    ULONG i;

    for (i = 0; i < PLUG_CATALOG_MAX; i++) {
        if (H->Cat[i].Blob != NULL) {
            ExFreePoolWithTag(H->Cat[i].Blob, PLUG_TAG);
            H->Cat[i].Blob = NULL;
        }
    }
}

/* A module by SHA-256, with a user reference taken and its record loaded. */
static NTSTATUS PlugCatGet(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _In_reads_(32) const UCHAR *Sha,
                           _Out_ PULONG Idx)
{
    ULONG i, j, freeSlot = PLUG_CATALOG_MAX;
    PLUG_CAT *e;
    NTSTATUS status;

    *Idx = PLUG_CATALOG_MAX;
    for (i = 0; i < PLUG_CATALOG_MAX; i++) {
        if (H->Cat[i].Users == 0) {
            if (freeSlot == PLUG_CATALOG_MAX) {
                freeSlot = i;
            }
            continue;
        }
        if (RtlCompareMemory(H->Cat[i].Sha, Sha, 32) == 32) {
            status = PlugCatLoad(Ctx, &H->Cat[i]);
            if (!NT_SUCCESS(status)) {
                return status;
            }
            H->Cat[i].Users++;
            *Idx = i;
            return STATUS_SUCCESS;
        }
    }
    if (freeSlot == PLUG_CATALOG_MAX) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    e = &H->Cat[freeSlot];
    RtlZeroMemory(e, sizeof(*e));
    RtlCopyMemory(e->Sha, Sha, 32);
    e->StripPin = -1;
    for (j = 0; j < APOLLO_STRIP_MODULES; j++) {
        if (RtlCompareMemory(g_ApolloStripPins[j].Sha256, Sha, 32) == 32) {
            e->StripPin = (LONG)j;
        }
    }
    status = PlugCatLoad(Ctx, e);
    if (!NT_SUCCESS(status)) {
        RtlZeroMemory(e, sizeof(*e));
        return status;
    }
    if (++H->CatSerial == 0 || H->CatSerial >= PLUG_ID_STRIPS) {
        H->CatSerial = 1;
    }
    e->Serial = H->CatSerial;
    e->Users = 1;
    *Idx = freeSlot;
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Ledgers (ua_plugin_place.c), DevLock held                            */
/* ------------------------------------------------------------------ */

/*
 * DSP D's ledger for this epoch: the pool registers read (reads only), the ledger built from
 * them; on the strip DSP the strip room booked first, where the strips' layout check proved it.
 * A later read with another map while something is placed quarantines the DSP.
 */
static NTSTATUS PlugLedgerFor(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _In_ ULONG D,
                              _Outptr_result_maybenull_ PLUG_LEDGER **Out)
{
    const STRIP_MODEL *model = PlugStripModel(Ctx);
    APOLLO_STRIPS *s = &Ctx->Strips;
    PLUG_DSP *d = PlugSyncDsp(Ctx, H, D);
    ULONG w = ApolloDspWindow(D), size[4], base[4], scr[4], regs[11], p, err;
    CHAR booked[112];

    *Out = NULL;
    booked[0] = '\0';
    for (p = 0; p < 4; p++) {
        size[p] = ApolloRead(Ctx, w + 0x010 + 4 * p);
        base[p] = ApolloRead(Ctx, w + 0x184 + 4 * p);
        scr[p] = p ? ApolloRead(Ctx, w + 0x190 + 4 * p) : 0;
        regs[p] = size[p];
        regs[4 + p] = base[p];
        if (p) {
            regs[7 + p] = scr[p];
        }
    }
    if (d->Ledger == NULL) {
        d->Ledger = (PLUG_LEDGER *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(PLUG_LEDGER), PLUG_TAG);
        if (d->Ledger == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        d->LedgerValid = FALSE;
    }
    if (d->LedgerValid) {
        if (RtlCompareMemory(regs, d->Regs, sizeof(regs)) == sizeof(regs)) {
            *Out = d->Ledger;
            return STATUS_SUCCESS;
        }
        if (!PlugLedgerIdle(d->Ledger, PLUG_ID_STRIPS)) {
            PlugQuarantine(Ctx, H, D, "its pool map changed with plug-ins placed", STATUS_DEVICE_CONFIGURATION_ERROR);
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        d->LedgerValid = FALSE;
    }
    RtlCopyMemory(d->Regs, regs, sizeof(regs));
    err = PlugLedgerInit(d->Ledger, base, size, scr);
    if (err == PLUG_OK && D == s->Dsp) {
        if (s->LayoutWhy != 0 || !s->LayoutRead) {
            err = PLUG_EINVAL;
        } else {
            /* pools 1-3 raised to max(register, the Mic strip's 0x1C0 / 0x80 / 0) and kept there as the
               ledger's floor (PlugLedgerStripScratch), then the strips' room (review P0 finding 1);
               [Virtual] the room includes this start's Virtual Input Strips, right below the Mic strips' */
            ULONG room[4];

            StripPlanRoom(model != NULL ? model->LayoutStrips : STRIP_PLAN_INPUTS, s->VMask, room);
            err = PlugLedgerBookStripRoom(d->Ledger, STRIP_CODE_WORDS, room, d->StripBook);
        }
        if (err == PLUG_OK && (d->StripBook[0].Base != APOLLO_STRIP_CODE_BASE ||
                               d->StripBook[1].Base + d->StripBook[1].Words != STRIP_TOP1 ||
                               d->StripBook[2].Base + d->StripBook[2].Words != STRIP_TOP2 ||
                               d->StripBook[3].Base + d->StripBook[3].Words != STRIP_TOP3)) {
            err = PLUG_EINVAL;
        }
    }
    if (err == PLUG_OK && D == s->Dsp) {
        /* [P7] the unit's own strips: x8p 8, Twin X 2 */
        (VOID)RtlStringCbPrintfA(booked, sizeof(booked), " (strip room booked: code 0xe0000+994, the %lu strips + Virtual 0x%02lx at the pool tops)",
                                 model != NULL ? model->LayoutStrips : (ULONG)STRIP_PLAN_INPUTS, s->VMask);
    }
    ApolloLogU(Ctx, "plugins: DSP %lu pools size %08x %08x %08x %08x base %08x %08x %08x %08x scratch %08x %08x %08x: %s%s",
               D, size[0], size[1], size[2], size[3], base[0], base[1], base[2], base[3], scr[1], scr[2], scr[3],
               err == PLUG_OK ? "ledger built" : "NOT a usable pool map", booked);
    if (err != PLUG_OK) {
        d->Ledger->Valid = 0;
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    d->LedgerValid = TRUE;
    *Out = d->Ledger;
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* The retained plain strips (ua_strips.c:1059-1132), DevLock held     */
/* ------------------------------------------------------------------ */

/*
 * Every actual input of Synch has a live plain Mic strip on DSP Dsp, in Epoch, fully started
 * (span and route words out): *Gen = the strips' generation, Header[k] = each input's header.
 * A pair needs both (ua_strips_override_target, ua_strips.c:1059-1098): one missing fails it whole.
 */
static NTSTATUS PlugStripTarget(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Dsp, _In_ ULONG Synch, _In_ ULONG Epoch,
                                _Out_ PULONG Gen, _Out_writes_(2) PULONG Header)
{
    const APOLLO_STRIPS *s = &Ctx->Strips;
    const STRIP_MODEL *model = PlugStripModel(Ctx);
    ULONG in[2], k;

    *Gen = 0;
    Header[0] = Header[1] = 0;
    in[0] = Synch & 0xFFFFu;
    in[1] = Synch >> 16;
    if (s->Enable != 1 || Dsp != s->Dsp || s->State != APOLLO_STRIPS_STATE_LIVE || !s->Owned || s->Quarantined ||
        s->Tainted || s->Epoch != Epoch || Ctx->Strips.Epoch != Epoch || s->StepsDone < STRIP_STEP_OTHERS) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (PlugSynchCheck(Synch, PlugPairInputs(Ctx)) != PLUG_OK) {
        return STATUS_INVALID_PARAMETER;        /* the parser's rule again: mono, or a linked pair */
    }
    for (k = 0; k < 2; k++) {
        if (in[k] == PLUG_SYNCH_MONO) {
            continue;
        }
        if (in[k] >= APOLLO_STRIP_INPUTS || (model != NULL && (model->MaskAllowed & (1u << in[k])) == 0)) {
            return STATUS_NOT_FOUND;
        }
        if (!s->StripStarted[in[k]] || s->StripState[in[k]] != APOLLO_STRIP_TAKEN || s->StripHeader[in[k]] == 0) {
            return STATUS_NOT_FOUND;
        }
        Header[k] = s->StripHeader[in[k]];
    }
    *Gen = s->Gen;
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* [Ins] The Unison analog lease (unison_plan.h; ua_core.c:2692-2955)  */
/* ------------------------------------------------------------------ */

/* StripLock and DevLock held. AnalogMask = the blocked fields + every owning lease's baseline fields. */
static VOID PlugAnalogRecompute(_In_ APOLLO_PLUGIN_HOST *H)
{
    ULONG i, k;

    for (k = 0; k < APOLLO_UNISON_SETTINGS; k++) {
        H->AnalogMask[k] = H->AnalogBlock[k];
    }
    for (i = 0; i < APOLLO_PLUGIN_INSTANCES_MAX; i++) {
        const PLUG_INST *in = H->Inst[i];

        if (in == NULL || !in->AnalogOwned) {
            continue;
        }
        for (k = 0; k < APOLLO_UNISON_SETTINGS; k++) {
            H->AnalogMask[k] |= in->AnalogBase.Mask[k];
        }
    }
}

/*
 * mixer.c ApolloMixerWriteSetting, DevLock held: does a write of Mask to setting Index touch a field a
 * Unison lease owns (or a failed completion blocked)? Every write but the lease's own is then refused
 * (upstream's ua_instances_analog_conflict in ua_mixer_write_setting_locked: "Generic/ALSA/raw writes
 * touching an owned analog lane return EBUSY").
 */
BOOLEAN ApolloPluginAnalogConflict(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Index, _In_ ULONG Mask)
{
    const APOLLO_PLUGIN_HOST *h = Ctx->Plugin;

    if (h == NULL || h->AnalogWrite || Index >= APOLLO_UNISON_SETTINGS) {
        return FALSE;
    }
    return (h->AnalogMask[Index] & Mask) != 0;
}

/* DevLock held. The image's Masks fields to the mailbox, past the conflict check, then one flush. */
static NTSTATUS PlugAnalogWrite(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _In_ const APOLLO_UNISON_IMAGE *Im,
                                _In_reads_(APOLLO_UNISON_SETTINGS) const ULONG *Masks)
{
    NTSTATUS status = STATUS_SUCCESS, st;
    ULONG i;

    if (!Im->Valid || !Ctx->MixerReady) {
        return STATUS_RETRY;
    }
    H->AnalogWrite = TRUE;
    for (i = 0; i < APOLLO_UNISON_SETTINGS; i++) {
        if (Masks[i] == 0) {
            continue;
        }
        st = ApolloMixerWriteSetting(Ctx, i, Im->Value[i], Masks[i]);
        if (!NT_SUCCESS(st) && NT_SUCCESS(status)) {
            status = st;
        }
    }
    H->AnalogWrite = FALSE;
    ApolloMixerFlush(Ctx);
    return status;
}

/*
 * StripLock held, DevLock not (ua_unison_drain): the mailbox consumed everything written (SEQ_RD = SEQ_WR, the
 * zero pass done) and then a fresh hardware readback; at most 100 x 5 ms. *Seq = the SEQ seen consumed. This
 * is a consumption fence plus a later readback, not a proof that the analog parts settled.
 */
static NTSTATUS PlugAnalogFence(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Epoch, _Out_opt_ PULONG Seq)
{
    ULONG n, after = 0;
    BOOLEAN drained = FALSE;
    NTSTATUS st;

    for (n = 0; n < 100; n++) {
        WdfWaitLockAcquire(Ctx->DevLock, NULL);
        st = (Ctx->Strips.Epoch != Epoch || !Ctx->MixerReady || ApolloGateClosed(Ctx)) ? STATUS_CANCELLED : STATUS_PENDING;
        if (st == STATUS_PENDING) {
            ApolloMixerFlush(Ctx);
            if (!Ctx->MixerDirty && ApolloRead(Ctx, REG_MIXER_SEQ_RD) == Ctx->MixerSeqWr) {
                if (!drained) {
                    after = Ctx->ReadbackSeq;
                    drained = TRUE;
                }
                if (Ctx->ReadbackValid && Ctx->ReadbackSeq != after && Ctx->Readback[38] != 0xFFFFFFFFu &&
                    Ctx->Readback[39] != 0xFFFFFFFFu) {
                    if (Seq != NULL) {
                        *Seq = Ctx->MixerSeqWr;
                    }
                    st = STATUS_SUCCESS;
                }
            }
        }
        WdfWaitLockRelease(Ctx->DevLock);
        if (st != STATUS_PENDING) {
            return st;
        }
        ApolloSleepMs(5);
    }
    return STATUS_IO_TIMEOUT;
}

/* DevLock held. An unproven completion: the lease's fields stay blocked until the hardware is released, the DSP quarantined. */
static VOID PlugAnalogFailed(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _Inout_ PLUG_INST *In, _In_ NTSTATUS Status)
{
    ULONG k;

    for (k = 0; k < APOLLO_UNISON_SETTINGS; k++) {
        H->AnalogBlock[k] |= In->AnalogBase.Mask[k];
    }
    In->AnalogOwned = FALSE;
    PlugAnalogRecompute(H);
    ApolloLogU(Ctx, "plugins: instance %lu: the Unison analog completion is unproven (0x%08x): synch 0x%05lx's preamp "
               "settings stay blocked until the unit is started again", In->Handle, Status, In->Synch);
    PlugQuarantine(Ctx, H, In->Dsp, "Unison analog completion unproven", Status);
}

/* DevLock held. The model's preamp plan (preamp_plan.h). */
static const APOLLO_PREAMP_MODEL *PlugPreampModel(_In_ PDEVICE_CONTEXT Ctx)
{
    return ApolloPreampModelFor(Ctx->Ident.Model);
}

/* StripLock and DevLock held. The preamps other leases own (ApolloUnisonCapture's Owned). */
static ULONG PlugAnalogOwnedPreamps(_In_ APOLLO_PLUGIN_HOST *H, _In_ const PLUG_INST *Except)
{
    ULONG i, m = 0;

    for (i = 0; i < APOLLO_PLUGIN_INSTANCES_MAX; i++) {
        const PLUG_INST *in = H->Inst[i];

        if (in != NULL && in != Except && in->AnalogOwned) {
            m |= ApolloUnisonPreamps(in->AnalogBase.Synch);
        }
    }
    return m;
}

/*
 * StripLock held (ua_inst_analog_restore + ua_unison_restore): the baseline back after the chain's route was
 * retired. CUSTOM off and every other changed field but the gains, fence, 80 ms, the gains, fence, GainC read
 * back. Success ends the lease; a failure keeps its fields blocked (PlugAnalogFailed).
 */
static NTSTATUS PlugAnalogRestore(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _Inout_ PLUG_INST *In, _In_ NTSTATUS Prior)
{
    ULONG before[APOLLO_UNISON_SETTINGS], gain[APOLLO_UNISON_SETTINGS];
    NTSTATUS status = Prior;

    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (!In->AnalogOwned) {
        WdfWaitLockRelease(Ctx->DevLock);
        return Prior;
    }
    if (NT_SUCCESS(status) && (In->Lost || In->Epoch != Ctx->Strips.Epoch)) {
        status = STATUS_CANCELLED;
    }
    if (NT_SUCCESS(status)) {
        ApolloUnisonRestorePlan(PlugPreampModel(Ctx), &In->AnalogBase, Ctx->MixerVal, before, gain);
        status = PlugAnalogWrite(Ctx, H, &In->AnalogBase, before);
    }
    WdfWaitLockRelease(Ctx->DevLock);
    if (NT_SUCCESS(status)) {
        status = PlugAnalogFence(Ctx, In->Epoch, NULL);
    }
    if (NT_SUCCESS(status)) {
        ApolloSleepMs(APOLLO_UNISON_SETTLE_MS);     /* CUSTOM-off settling precedes the ordinary gain */
        WdfWaitLockAcquire(Ctx->DevLock, NULL);
        status = (In->Epoch != Ctx->Strips.Epoch) ? STATUS_CANCELLED : PlugAnalogWrite(Ctx, H, &In->AnalogBase, gain);
        WdfWaitLockRelease(Ctx->DevLock);
    }
    if (NT_SUCCESS(status)) {
        status = PlugAnalogFence(Ctx, In->Epoch, NULL);
    }
    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (NT_SUCCESS(status) && !ApolloUnisonGainRestored(&In->AnalogBase, Ctx->Readback)) {
        status = STATUS_DEVICE_DATA_ERROR;
    }
    if (NT_SUCCESS(status)) {
        In->AnalogOwned = FALSE;
        PlugAnalogRecompute(H);
        ApolloLogU(Ctx, "plugins: instance %lu: Unison lease ended, synch 0x%05lx's ordinary preamp settings back", In->Handle,
                   In->Synch);
    } else {
        PlugAnalogFailed(Ctx, H, In, status);
    }
    WdfWaitLockRelease(Ctx->DevLock);
    return status;
}

/* ------------------------------------------------------------------ */
/* Instances (ua_plugin_instance.c)                                     */
/* ------------------------------------------------------------------ */

/* DevLock held. Claim the chain's scratch before placing rows (nothing is sent to the DSP). */
static NTSTATUS PlugScratchRaise(_In_ APOLLO_PLUGIN_HOST *H, _In_ PLUG_LEDGER *L, _Inout_ PLUG_INST *In,
                                 _In_reads_(4) const ULONG *Need)
{
    ULONG p, i, err;

    for (p = 1; p < 4; p++) {
        In->ScratchBase[p] = L->Scratch[p];
    }
    for (i = 0; i < APOLLO_PLUGIN_INSTANCES_MAX; i++) {
        PLUG_INST *o = H->Inst[i];

        if (o != NULL && o != In && o->Dsp == In->Dsp && o->ScratchClaim && o->Placed && o->Epoch == In->Epoch) {
            RtlCopyMemory(In->ScratchBase, o->ScratchBase, sizeof(In->ScratchBase));
            break;
        }
    }
    RtlCopyMemory(In->ScratchNeed, Need, sizeof(In->ScratchNeed));
    In->ScratchClaim = TRUE;
    for (p = 1; p < 4; p++) {
        if (Need[p] <= L->Scratch[p]) {
            continue;
        }
        err = PlugLedgerSetScratch(L, p, Need[p]);
        if (err != PLUG_OK) {
            return PlugErr(err);
        }
    }
    return STATUS_SUCCESS;
}

/* DevLock held. The maximum over the remaining claimants (only reductions). */
static VOID PlugScratchRestore(_In_ APOLLO_PLUGIN_HOST *H, _In_opt_ PLUG_LEDGER *L, _Inout_ PLUG_INST *In)
{
    ULONG need[4], p, i;

    if (!In->ScratchClaim) {
        return;
    }
    In->ScratchClaim = FALSE;
    if (L == NULL || !L->Valid) {
        return;
    }
    RtlCopyMemory(need, In->ScratchBase, sizeof(need));
    for (i = 0; i < APOLLO_PLUGIN_INSTANCES_MAX; i++) {
        PLUG_INST *o = H->Inst[i];

        if (o != NULL && o != In && o->Dsp == In->Dsp && o->ScratchClaim && o->Epoch == In->Epoch) {
            for (p = 1; p < 4; p++) {
                need[p] = max(need[p], o->ScratchNeed[p]);
            }
        }
    }
    for (p = 1; p < 4; p++) {
        (VOID)PlugLedgerSetScratch(L, p, need[p]);
    }
}

/*
 * StripLock held, DevLock not (ua_plugin_instance.c:242-314). Take In off its DSP: with Send, one
 * transaction stops a started chain (Restore: and SYNCHs each retained plain strip again, both of
 * a pair, with its span and route words) and releases the modules whose last hold this was; it is
 * waited for (1 s). Everything goes back to the ledger. An instance of an older epoch sends
 * nothing (its DSP state went with the epoch). A restore that is impossible (the strips are not
 * live any more, either strip of a pair) sends nothing and quarantines the DSP, as upstream does
 * (ua_strips_restore: both or none).
 */
static NTSTATUS PlugUnplaceOnce(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _Inout_ PLUG_INST *In,
                                _In_ BOOLEAN Send, _In_ BOOLEAN Restore);

/*
 * PlugUnplaceOnce, then [Warm] the strips' record learns how many instances are left on the strip DSP:
 * only after an unplace that worked on a live device (a stranded or unproven one leaves the count as it
 * was, so the next warm start puts the plain strips back and keeps plug-ins off: docs/STRIPS.md 12.5).
 */
static NTSTATUS PlugUnplace(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _Inout_ PLUG_INST *In,
                            _In_ BOOLEAN Send, _In_ BOOLEAN Restore)
{
    BOOLEAN wasPlaced = In->Placed, strip = (In->Dsp == Ctx->Strips.Dsp) ? TRUE : FALSE;
    NTSTATUS status = PlugUnplaceOnce(Ctx, H, In, Send, Restore);

    if (wasPlaced && strip) {
        WdfWaitLockAcquire(Ctx->DevLock, NULL);
        if (NT_SUCCESS(status) && Ctx->Connected && !ApolloGateClosed(Ctx) && !Ctx->Strips.Quarantined &&
            !H->Dsp[In->Dsp & (APOLLO_MAX_DSPS - 1)].Quarantined) {
            PlugNoteStripDsp(Ctx, H);
        }
        WdfWaitLockRelease(Ctx->DevLock);
    }
    return status;
}

static NTSTATUS PlugUnplaceOnce(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _Inout_ PLUG_INST *In,
                                _In_ BOOLEAN Send, _In_ BOOLEAN Restore)
{
    PLUG_XACT *x = NULL;
    PLUG_DSP *d;
    PLUG_LEDGER *L;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG header[2] = { 0, 0 }, gen = 0, i, last, rels = 0;
    BOOLEAN stop = FALSE, restore = FALSE, admitted, stranded = FALSE;

    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (!In->Placed) {
        WdfWaitLockRelease(Ctx->DevLock);
        return STATUS_SUCCESS;
    }
    In->Placed = FALSE;
    d = PlugSyncDsp(Ctx, H, In->Dsp);
    L = d->LedgerValid ? d->Ledger : NULL;
    if (In->Epoch != Ctx->Strips.Epoch || L == NULL) {
        In->Started = FALSE;
        In->RecExcess = 0;
        PlugLatencyNote(Ctx, H);                /* [Lat] */
        In->ScratchClaim = FALSE;
        RtlZeroMemory(In->Lease, sizeof(In->Lease));
        WdfWaitLockRelease(Ctx->DevLock);
        return STATUS_SUCCESS;
    }
    admitted = PlugDspAdmitted(Ctx, H, In->Dsp);
    if (Send && admitted) {
        status = PxBegin(Ctx, H, In->Dsp, In->Epoch, 0, &x);
        if (!NT_SUCCESS(status)) {
            x = NULL;
        }
        stop = In->Started && !In->Lost;
        if (NT_SUCCESS(status) && stop && Restore && In->StripOverride) {
            status = PlugStripTarget(Ctx, In->Dsp, In->Synch, In->Epoch, &gen, header);
            if (NT_SUCCESS(status) && gen != In->StripGen) {
                status = STATUS_INVALID_DEVICE_STATE;
            }
            restore = NT_SUCCESS(status);
        }
        if (NT_SUCCESS(status) && x != NULL) {
            (VOID)PlugBuildStop(&x->L, In->Synch, stop, restore, header);
        }
    } else if (In->Started && !In->Lost) {
        stranded = TRUE;                        /* nothing can be sent there any more */
        if (Restore) {
            status = STATUS_INVALID_DEVICE_STATE;
        }
    }
    In->Started = FALSE;
    In->RecExcess = 0;
    PlugLatencyNote(Ctx, H);                    /* [Lat] its record path no longer counts */
    if (In->AuxBus != 0) {
        (VOID)ApolloCuesAuxOwner(Ctx, In->AuxBus - 1, 0);     /* [Ins] the aux return is DSP 0's again */
    }
    for (i = 0; i < In->NRes && i < PLUG_RES_MAX; i++) {
        PLUG_CAT *e = (In->Cat[i] < PLUG_CATALOG_MAX) ? &H->Cat[In->Cat[i]] : NULL;

        if (In->StripShared[i] || In->Lease[i].Cookie == 0) {
            continue;
        }
        last = 0;
        (VOID)PlugLedgerRelease(L, &In->Lease[i], &last);
        RtlZeroMemory(&In->Lease[i], sizeof(In->Lease[i]));
        if (e == NULL || !last || e->Res[In->Dsp].Epoch != In->Epoch || e->Res[In->Dsp].State == RES_NONE) {
            continue;
        }
        if (x != NULL && e->Res[In->Dsp].State == RES_RESIDENT) {
            PlugBuildRelease(&x->L, &e->Bill);
            rels++;
        }
        e->Res[In->Dsp].State = RES_NONE;
    }
    for (i = 0; i < In->NPriv && i < PLUG_PRIV_MAX; i++) {
        if (In->Lease[PLUG_RES_MAX + i].Cookie != 0) {
            (VOID)PlugLedgerRelease(L, &In->Lease[PLUG_RES_MAX + i], NULL);
        }
        RtlZeroMemory(&In->Lease[PLUG_RES_MAX + i], sizeof(In->Lease[0]));
    }
    PlugScratchRestore(H, L, In);
    if (!NT_SUCCESS(status)) {
        PlugQuarantine(Ctx, H, In->Dsp, "a plug-in teardown could not restore its plain strip(s)", status);
    }
    if (stranded) {
        ApolloLogU(Ctx, "plugins: instance %lu on DSP %lu: the DSP is not this host's any more: the chain on synch 0x%05lx "
                   "keeps running until the Apollo is power-cycled", In->Handle, In->Dsp, In->Synch);
    }
    WdfWaitLockRelease(Ctx->DevLock);

    if (x == NULL) {
        return status;
    }
    if (!NT_SUCCESS(status) || x->L.N == 0) {
        PxEnd(H, x);
        return status;
    }
    status = PxRun(Ctx, H, x, PLUG_WAIT_MS, "teardown");
    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (x->State != PX_BUILDING) {
        d = PlugSyncDsp(Ctx, H, In->Dsp);
        d->Stops += stop ? 1 : 0;
        d->Releases += rels;
    }
    if (NT_SUCCESS(status)) {
        ApolloLogU(Ctx, "plugins: instance %lu taken off DSP %lu after %lu ms: %s%s%lu module(s) released", In->Handle,
                   In->Dsp, PlugMsSince(x->PublishedAt), stop ? "chain stopped, " : "",
                   restore ? "plain strip(s) restored, " : "", rels);
    }
    WdfWaitLockRelease(Ctx->DevLock);
    PxEnd(H, x);
    return status;
}

static VOID PlugInstFree(_In_ APOLLO_PLUGIN_HOST *H, _In_ PLUG_INST *In)
{
    ULONG i;

    for (i = 0; i < APOLLO_PLUGIN_INSTANCES_MAX; i++) {
        if (H->Inst[i] == In) {
            H->Inst[i] = NULL;
            InterlockedDecrement(&H->Live);
        }
    }
    for (i = 0; i < In->NRes && i < PLUG_RES_MAX; i++) {
        PlugCatPut(H, In->Cat[i]);
    }
    ExFreePoolWithTag(In, PLUG_TAG);
}

/*
 * StripLock held. Take it down (stop, restore, releases), then free it. [Ins] A Unison lease is ended only
 * after its route was retired: the baseline goes back (PlugAnalogRestore), or its fields stay blocked.
 */
static NTSTATUS PlugRetire(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _In_ PLUG_INST *In)
{
    NTSTATUS status = PlugUnplace(Ctx, H, In, !In->Lost, TRUE);

    if (In->AnalogOwned) {
        NTSTATUS a = PlugAnalogRestore(Ctx, H, In, status);

        if (NT_SUCCESS(status)) {
            status = a;
        }
    }
    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    In->AnalogOwned = FALSE;                    /* blocked (PlugAnalogFailed) or ended: AnalogMask no longer counts it */
    PlugInstFree(H, In);
    PlugAnalogRecompute(H);
    WdfWaitLockRelease(Ctx->DevLock);
    return status;
}

static PLUG_INST *PlugFind(_In_ APOLLO_PLUGIN_HOST *H, _In_ WDFFILEOBJECT File, _In_ ULONG Handle)
{
    ULONG i;

    for (i = 0; i < APOLLO_PLUGIN_INSTANCES_MAX; i++) {
        PLUG_INST *in = H->Inst[i];

        if (in != NULL && in->Owner == File && in->Handle == Handle) {
            return in;
        }
    }
    return NULL;
}

/* StripLock held. One module upload, acknowledged (ua_plugin_upload.c:185-238). Detail: index + reply. */
static NTSTATUS PlugUpload(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _In_ ULONG Dsp, _In_ ULONG Epoch,
                           _Inout_ PLUG_CAT *E, _In_ ULONG Dest, _Out_writes_(4) PULONG Reply)
{
    PLUG_XACT *x = NULL;
    NTSTATUS status;
    ULONG k;
    PLUG_DSP *d;

    RtlZeroMemory(Reply, 4 * sizeof(ULONG));
    status = PlugCatLoad(Ctx, E);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = PxBegin(Ctx, H, Dsp, Epoch, E->Len + 8, &x);
    if (!NT_SUCCESS(status) || x == NULL) {
        return NT_SUCCESS(status) ? STATUS_INSUFFICIENT_RESOURCES : status;
    }
    if (PlugBuildUpload(&x->L, &x->Pay, &E->Bill, E->Blob, E->Len, Dest) != PLUG_OK) {
        status = PlugErr(x->L.Err);
        PxEnd(H, x);
        return status;
    }
    status = PxRun(Ctx, H, x, PLUG_WAIT_MS, "upload");
    for (k = 0; k < 4; k++) {
        Reply[k] = PxReplyWord(H, x, 0, k);
    }
    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    d = PlugSyncDsp(Ctx, H, Dsp);
    if (x->State != PX_BUILDING) {
        d->Uploads++;
    }
    if (NT_SUCCESS(status)) {
        if (!PlugUploadReplyOk(Reply, &E->Bill)) {
            status = STATUS_REQUEST_NOT_ACCEPTED;
            d->RepliesBad++;
            CHAR why[64];

            ApolloLogU(Ctx, "plugins: DSP %lu refused module 0x%03lx (kind %lu arch %lu, %lu bytes) at 0x%05lx: reply %08x %08x %08x %08x %s",
                       Dsp, E->Bill.Id, E->Bill.Kind, E->Bill.Arch, E->Len, Dest, Reply[0], Reply[1], Reply[2], Reply[3],
                       ApolloDspReplyText(Reply, why, sizeof(why)));
            PlugQuarantine(Ctx, H, Dsp, "an upload was not accepted", status);
        } else {
            d->RepliesOk++;
            ApolloLogU(Ctx, "plugins: DSP %lu accepted module 0x%03lx (kind %lu arch %lu, %lu words) at 0x%05lx after %lu ms",
                       Dsp, E->Bill.Id, E->Bill.Kind, E->Bill.Arch, E->Bill.Words, Dest, PlugMsSince(x->PublishedAt));
            if (Dsp == Ctx->Strips.Dsp && Epoch == Ctx->Strips.Epoch) {
                ApolloStripsUploadProven(Ctx);  /* [Warm] the first one of a warm session proves it (12.5) */
            }
        }
    }
    if (NT_SUCCESS(status)) {
        E->Res[Dsp].Epoch = Epoch;
        E->Res[Dsp].State = RES_RESIDENT;
        E->Res[Dsp].Base = Dest;
    } else if (x->State != PX_BUILDING) {
        E->Res[Dsp].Epoch = Epoch;
        E->Res[Dsp].State = RES_UNKNOWN;
        E->Res[Dsp].Base = Dest;
    }
    WdfWaitLockRelease(Ctx->DevLock);
    PxEnd(H, x);
    return status;
}

/* StripLock held. The private rows' zero-fills and memspec, taken by the DSP (ua_plugin_instance.c:610-658). */
static NTSTATUS PlugPrivates(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _In_ PLUG_INST *In)
{
    PLUG_XACT *x = NULL;
    NTSTATUS status;
    ULONG i;
    PLUG_DSP *d;

    for (i = 0; i < In->NPriv; i++) {
        H->Addr[i] = In->Lease[PLUG_RES_MAX + i].Base;
    }
    status = PxBegin(Ctx, H, In->Dsp, In->Epoch, PLUG_PAGE, &x);
    if (!NT_SUCCESS(status) || x == NULL) {
        return NT_SUCCESS(status) ? STATUS_INSUFFICIENT_RESOURCES : status;
    }
    if (PlugBuildPrivates(&x->L, &x->Pay, In->Rows, In->NPriv, H->Addr, H->Words2N) != PLUG_OK) {
        status = PlugErr(x->L.Err);
        PxEnd(H, x);
        return status;
    }
    status = PxRun(Ctx, H, x, PLUG_WAIT_MS, "privates");
    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (x->State != PX_BUILDING) {
        d = PlugSyncDsp(Ctx, H, In->Dsp);
        d->ZeroFills += In->NPriv;
        d->Memspecs += (In->NPriv > 1) ? 1 : 0;
    }
    WdfWaitLockRelease(Ctx->DevLock);
    PxEnd(H, x);
    return status;
}

/*
 * StripLock held. ua_inst_start (ua_plugin_instance.c:689-739). Route (F_START, op START): stop
 * each input's plain strip, the WRITEs (relocated; none for op START), SYNCH the head, span/route;
 * marked started before the run, so a failure still stops it at the takedown. Without Route
 * (F_STAGE): the WRITEs only, one transaction; nothing is stopped or routed and the instance is
 * not started (its plain strip, or both of a pair, keeps running).
 */
static NTSTATUS PlugStart(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _Inout_ PLUG_INST *In,
                          _In_reads_opt_(NW) const ULONG *Cmd, _In_ ULONG NW, _In_ BOOLEAN Route)
{
    PLUG_XACT *x = NULL;
    NTSTATUS status;
    ULONG i, ncmds = 0;
    PLUG_DSP *d;

    for (i = 0; i < In->NPriv; i++) {
        H->Addr[i] = In->Lease[PLUG_RES_MAX + i].Base;
    }
    if ((NW != 0 && Cmd == NULL) || (!Route && NW == 0)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (NW != 0 && PlugCheckCmds(In->Rows, In->NPriv, Cmd, NW, &ncmds) != PLUG_OK) {
        return STATUS_INVALID_PARAMETER;        /* checked at the IOCTL already: never expected */
    }
    status = PxBegin(Ctx, H, In->Dsp, In->Epoch, NW ? PLUG_PAGE : 0, &x);
    if (!NT_SUCCESS(status) || x == NULL) {
        return NT_SUCCESS(status) ? STATUS_INSUFFICIENT_RESOURCES : status;
    }
    if (PlugBuildStartEx(&x->L, &x->Pay, Cmd, NW, H->Addr, In->NPriv, In->Synch, H->Reloc, Route ? 1 : 0) != PLUG_OK) {
        status = PlugErr(x->L.Err);
        PxEnd(H, x);
        return status;
    }
    if (Route) {
        WdfWaitLockAcquire(Ctx->DevLock, NULL);
        In->Started = TRUE;
        WdfWaitLockRelease(Ctx->DevLock);
    }
    status = PxRun(Ctx, H, x, PLUG_WAIT_MS, Route ? (NW ? "start" : "route") : "stage");
    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (x->State == PX_BUILDING) {
        if (Route) {
            In->Started = FALSE;                /* never published: nothing was stopped or started */
        }
    } else {
        d = PlugSyncDsp(Ctx, H, In->Dsp);
        d->Writes += ncmds;
    }
    if (NT_SUCCESS(status) && Route && In->AuxBus != 0) {
        /* [Ins] the chain mixes the aux return from now on: its coefficients go to this DSP too (cues.c) */
        (VOID)ApolloCuesAuxOwner(Ctx, In->AuxBus - 1, In->Dsp);
    }
    if (NT_SUCCESS(status)) {
        ApolloLogU(Ctx, "plugins: instance %lu %s on DSP %lu synch 0x%05lx (head 0x%05lx, %lu WRITE blocks, %lu words) "
                   "after %lu ms", In->Handle, !Route ? "staged (first page written, not routed)" :
                   NW ? "started" : "routed (staged startup over)", In->Dsp, In->Synch, H->Addr[0], ncmds, NW,
                   PlugMsSince(x->PublishedAt));
    }
    WdfWaitLockRelease(Ctx->DevLock);
    PxEnd(H, x);
    return status;
}

/* The operation's request, copied out of the system buffer. */
typedef struct _PLUG_REQUEST {
    APOLLO_PLUGIN_INSTANCE  Hdr;
    UCHAR                   Sha[PLUG_RES_MAX][32];
    PLUG_ROW                Rows[PLUG_PRIV_MAX];
    ULONG                   Cmd[PLUG_CMD_WORDS_MAX];
    ULONG                   Ids[5];
    ULONG                   NIds;
    ULONG                   Carrier;
    ULONG                   Values[PLUG_READ_WORDS_MAX];
    ULONG                   Aux;            /* [Ins] PLUG_NONE, or the aux bus whose return pair Synch is */
} PLUG_REQUEST;

/*
 * StripLock held. CREATE (ua_plugin_instance.c:762-961): the licence answer first and alone
 * decides whether anything else happens; then the modules, one placement in the answer's epoch,
 * uploads, private rows, and with F_START the start. Any failure after placement takes down what
 * was placed.
 */
static VOID PlugCreate(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _In_ WDFFILEOBJECT File,
                       _Inout_ PLUG_REQUEST *R)
{
    APOLLO_PLUGIN_INSTANCE *q = &R->Hdr;
    PLUG_INST *in = NULL;
    PLUG_LEDGER *L = NULL;
    PLUG_BILL *bills = H->Bills;
    NTSTATUS status;
    ULONG epoch = 0, i, j, n, slot, dsp = q->Dsp, need[4], header[2];
    BOOLEAN start = (q->Flags & APOLLO_PLUGIN_F_START) != 0;                           /* routed at CREATE */
    BOOLEAN chain = (q->Flags & (APOLLO_PLUGIN_F_START | APOLLO_PLUGIN_F_STAGE)) != 0;  /* replaces a plain strip (both of a pair) */
    ULONGLONG t0 = KeQueryInterruptTime();

    /* 1. The device's answer, fresh: nothing else unless it admits */
    q->Step = APOLLO_PLUGIN_STEP_LICENCE;
    if (R->NIds == 0) {
        R->Ids[0] = q->RenderId;
        R->NIds = 1;
    }
    status = PlugAdmit(Ctx, H, R->Ids, R->NIds, &q->LicenseState, &q->LicenseWord, &epoch);
    q->Epoch = epoch;
    if (!NT_SUCCESS(status) || (q->Flags & APOLLO_PLUGIN_F_CHECK)) {
        q->Status = status;
        if (NT_SUCCESS(status)) {
            q->Step = APOLLO_PLUGIN_STEP_DONE;
        }
        return;
    }

    /* 2. The modules, from the store by hash */
    q->Step = APOLLO_PLUGIN_STEP_MODULES;
    for (slot = 0; slot < APOLLO_PLUGIN_INSTANCES_MAX && H->Inst[slot] != NULL; slot++) {
    }
    if (slot == APOLLO_PLUGIN_INSTANCES_MAX) {
        q->Status = STATUS_INSUFFICIENT_RESOURCES;
        return;
    }
    in = (PLUG_INST *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(PLUG_INST), PLUG_TAG);
    if (in == NULL) {
        q->Status = STATUS_INSUFFICIENT_RESOURCES;
        return;
    }
    in->Owner = File;
    in->RenderId = q->RenderId;
    in->Epoch = epoch;
    in->Unison = (q->Flags & APOLLO_PLUGIN_F_UNISON) != 0;     /* [Ins] staged only (PlugCreateFlagsOk) */
    in->NPriv = q->NPrivate;
    RtlCopyMemory(in->Rows, R->Rows, q->NPrivate * sizeof(PLUG_ROW));
    for (i = 0; i < PLUG_RES_MAX; i++) {
        in->Cat[i] = PLUG_CATALOG_MAX;
    }
    status = STATUS_SUCCESS;
    for (i = 0; i < q->NResources && NT_SUCCESS(status); i++) {
        status = PlugCatGet(Ctx, H, R->Sha[i], &in->Cat[i]);
        if (NT_SUCCESS(status)) {
            in->NRes = i + 1;
            bills[i] = H->Cat[in->Cat[i]].Bill;
            for (j = 0; j < i; j++) {
                if (in->Cat[j] == in->Cat[i]) {
                    status = STATUS_INVALID_PARAMETER;      /* the same module twice */
                }
            }
        } else {
            q->Detail[0] = i;
        }
    }
    if (NT_SUCCESS(status)) {
        status = PlugErr(PlugCheckSet(bills, in->NRes));
    }
    if (!NT_SUCCESS(status)) {
        goto fail;
    }

    /* 3./4. One placement, in the answer's epoch, on an owned DSP */
    q->Step = APOLLO_PLUGIN_STEP_PLACE;
    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (Ctx->Strips.Epoch != epoch) {
        status = STATUS_CANCELLED;
    } else if (dsp == APOLLO_PLUGIN_DSP_ANY) {
        status = STATUS_NOT_FOUND;
        for (i = 1; i < Ctx->NumDsps && i < APOLLO_MAX_DSPS; i++) {
            if (PlugDspAdmitted(Ctx, H, i) && PlugStripsSettled(Ctx, i)) {
                dsp = i;
                status = STATUS_SUCCESS;
                break;
            }
        }
    } else if (!PlugDspAdmitted(Ctx, H, dsp)) {
        status = STATUS_INVALID_DEVICE_STATE;
        ApolloLogU(Ctx, "plugins: DSP %lu is not this host's to place on (owned %u, quarantined %u, rings %u): "
                   "power-cycle the Apollo for plug-ins", dsp, PlugDspOwned(Ctx, dsp),
                   dsp < APOLLO_MAX_DSPS ? H->Dsp[dsp].Quarantined : 0, PlugRingsLive(Ctx, dsp));
    } else if (!PlugStripsSettled(Ctx, dsp)) {
        status = STATUS_DEVICE_BUSY;
        ApolloLogU(Ctx, "plugins: DSP %lu: its strip pass is not over (strips state %lu): nothing placed there until it is",
                   dsp, Ctx->Strips.State);
    }
    if (NT_SUCCESS(status)) {
        in->Dsp = dsp;
        status = PlugLedgerFor(Ctx, H, dsp, &L);
    }
    /* the modules: shared with the strips there, or a conflict with a module of the same id */
    for (i = 0; NT_SUCCESS(status) && i < in->NRes; i++) {
        PLUG_CAT *e = &H->Cat[in->Cat[i]];

        in->StripShared[i] = 0;
        if (e->StripPin >= 0 && dsp == Ctx->Strips.Dsp) {
            if (Ctx->Strips.ModState[e->StripPin] != APOLLO_STRIP_MOD_RESIDENT) {
                status = STATUS_INVALID_DEVICE_STATE;   /* a second copy next to an unproven one: never */
            } else {
                in->StripShared[i] = 1;
                e->Res[dsp].Epoch = epoch;
                e->Res[dsp].State = RES_RESIDENT;
                e->Res[dsp].Base = g_ApolloStripPins[e->StripPin].Dest;
            }
            continue;
        }
        if (dsp == Ctx->Strips.Dsp) {
            for (j = 0; j < APOLLO_STRIP_MODULES; j++) {
                if (g_ApolloStripPins[j].Id == e->Bill.Id) {
                    status = STATUS_OBJECT_NAME_COLLISION;  /* the strips' ids, whatever arch or state (finding 3) */
                }
            }
        }
        for (j = 0; j < PLUG_CATALOG_MAX; j++) {
            const PLUG_CAT *o = &H->Cat[j];

            if (o != e && o->Users != 0 && o->Bill.Id == e->Bill.Id && o->Bill.Arch == e->Bill.Arch) {
                status = STATUS_OBJECT_NAME_COLLISION;  /* the DSP knows a resource by its id */
            }
        }
    }
    if (NT_SUCCESS(status) && chain && R->Aux != PLUG_NONE) {
        /* [Ins] an aux return: no plain strip there to retain; the strip DSP once its pass is over */
        if (dsp != Ctx->Strips.Dsp || !PlugAuxEnabled()) {
            status = STATUS_NOT_SUPPORTED;
        }
        for (i = 0; NT_SUCCESS(status) && start && i < APOLLO_PLUGIN_INSTANCES_MAX; i++) {
            PLUG_INST *o = H->Inst[i];

            if (o != NULL && o->Dsp == dsp && o->Placed && !o->Lost && o->Started && PlugInputsOverlap(o->Synch, q->Synch)) {
                status = STATUS_DEVICE_BUSY;
            }
        }
        if (NT_SUCCESS(status)) {
            in->AuxBus = R->Aux + 1;
            in->Synch = q->Synch;
        }
    } else if (NT_SUCCESS(status) && chain) {
        status = PlugStripTarget(Ctx, dsp, q->Synch, epoch, &in->StripGen, header);
        for (i = 0; NT_SUCCESS(status) && start && i < APOLLO_PLUGIN_INSTANCES_MAX; i++) {
            PLUG_INST *o = H->Inst[i];

            if (o != NULL && o->Dsp == dsp && o->Placed && !o->Lost && o->Started && PlugInputsOverlap(o->Synch, q->Synch)) {
                status = STATUS_DEVICE_BUSY;    /* one routed owner per input, whatever the handle */
            }
        }
        if (NT_SUCCESS(status)) {
            in->StripOverride = TRUE;           /* staged too: it keeps the strip generation for START */
            in->Synch = q->Synch;
        }
    }
    if (NT_SUCCESS(status) && chain && (q->Scratch[0] | q->Scratch[1] | q->Scratch[2]) != 0) {
        need[0] = 0;
        need[1] = q->Scratch[0];
        need[2] = q->Scratch[1];
        need[3] = q->Scratch[2];
        status = PlugScratchRaise(H, L, in, need);
        if (!NT_SUCCESS(status)) {
            PlugScratchRestore(H, L, in);
        }
    }
    if (NT_SUCCESS(status)) {
        n = 0;
        for (i = 0; i < in->NRes; i++) {
            const PLUG_CAT *e = &H->Cat[in->Cat[i]];

            if (in->StripShared[i]) {
                continue;
            }
            H->Req[n].Kind = e->Bill.Pool;
            H->Req[n].Words = e->Bill.Words;
            H->Req[n].Identity = e->Serial;
            H->Req[n].Shared = 1;
            n++;
        }
        for (i = 0; i < in->NPriv; i++, n++) {
            H->Req[n].Kind = in->Rows[i].Pool;
            H->Req[n].Words = in->Rows[i].Dwords;
            H->Req[n].Identity = 0;
            H->Req[n].Shared = 0;
        }
        {
            PLUG_LEASE *got = H->Got;
            ULONG err = PlugLedgerReserveBatch(L, H->Req, n, got), m = 0;

            status = PlugErr(err);
            if (err == PLUG_OK) {
                for (i = 0; i < in->NRes; i++) {
                    if (!in->StripShared[i]) {
                        in->Lease[i] = got[m++];
                    }
                }
                for (i = 0; i < in->NPriv; i++) {
                    in->Lease[PLUG_RES_MAX + i] = got[m++];
                }
            } else if (in->ScratchClaim) {
                PlugScratchRestore(H, L, in);
            }
        }
    }
    if (NT_SUCCESS(status)) {
        in->Placed = TRUE;
        in->Handle = H->NextHandle++;
        if (H->NextHandle == 0) {
            H->NextHandle = 1;
        }
        H->Inst[slot] = in;
        InterlockedIncrement(&H->Live);
        if (dsp == Ctx->Strips.Dsp) {
            PlugNoteStripDsp(Ctx, H);           /* [Warm] the record says a plug-in is there, before anything is sent */
        }
        ApolloLogU(Ctx, "plugins: plug-in 0x%03lx instance %lu placed on DSP %lu: %lu module(s), %lu row(s), head 0x%05lx%s",
                   q->RenderId, in->Handle, dsp, in->NRes, in->NPriv, in->Lease[PLUG_RES_MAX].Base,
                   start ? ", replaces the plain strip(s) at START" :
                   chain ? ", staged: its startup in pages, routed later by op START" : ", not routed");
    } else {
        ApolloLogU(Ctx, "plugins: plug-in 0x%03lx not placed on DSP %lu (0x%08x): nothing sent", q->RenderId, dsp, status);
    }
    WdfWaitLockRelease(Ctx->DevLock);
    if (!NT_SUCCESS(status)) {
        goto fail;
    }
    q->Dsp = dsp;

    /* 5. The modules not resident there, each acknowledged */
    q->Step = APOLLO_PLUGIN_STEP_UPLOAD;
    for (i = 0; i < in->NRes && NT_SUCCESS(status); i++) {
        PLUG_CAT *e = &H->Cat[in->Cat[i]];
        BOOLEAN resident;
        ULONG reply[4];

        if (ReadAcquire(&Ctx->StopRequested)) {
            status = STATUS_CANCELLED;
            break;
        }
        if (in->StripShared[i]) {
            q->Resident++;
            continue;
        }
        WdfWaitLockAcquire(Ctx->DevLock, NULL);
        resident = e->Res[dsp].Epoch == epoch && e->Res[dsp].State == RES_RESIDENT && e->Res[dsp].Base == in->Lease[i].Base;
        WdfWaitLockRelease(Ctx->DevLock);
        if (resident) {
            q->Resident++;
            continue;
        }
        status = PlugUpload(Ctx, H, dsp, epoch, e, in->Lease[i].Base, reply);
        if (NT_SUCCESS(status)) {
            q->Uploads++;
        } else {
            q->Detail[0] = i;
            q->Detail[1] = reply[0];
            q->Detail[2] = reply[1];
            q->Detail[3] = reply[2];
        }
    }
    if (!NT_SUCCESS(status)) {
        goto takedown;
    }

    /* 6. Private rows zero-filled and linked */
    q->Step = APOLLO_PLUGIN_STEP_PRIVATES;
    status = ReadAcquire(&Ctx->StopRequested) ? STATUS_CANCELLED : PlugPrivates(Ctx, H, in);
    if (!NT_SUCCESS(status)) {
        goto takedown;
    }

    /* 7. Parameters, then started on the DSP input (F_STAGE: the first page only, not started) */
    if (chain) {
        q->Step = APOLLO_PLUGIN_STEP_START;
        status = ReadAcquire(&Ctx->StopRequested) ? STATUS_CANCELLED : PlugStart(Ctx, H, in, R->Cmd, q->NCommands, start);
        if (!NT_SUCCESS(status)) {
            goto takedown;
        }
    }

    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (in->Lost || Ctx->Strips.Epoch != epoch) {
        status = STATUS_CANCELLED;
    } else {
        in->Loaded = TRUE;
        in->Staged = chain && !start;
        q->Handle = in->Handle;
        q->Dsp = dsp;
        ApolloLogU(Ctx, "plugins: plug-in 0x%03lx instance %lu loaded on DSP %lu in %lu ms (%lu uploaded, %lu resident)%s",
                   q->RenderId, in->Handle, dsp, PlugMsSince(t0), q->Uploads, q->Resident,
                   (in->Started && (in->Synch >> 16) != PLUG_SYNCH_MONO) ? ": running on its input pair" :
                   in->Started ? ": running on its input" :
                   in->Staged ? ": staged (the plain strip(s) still run until START)" : ": not routed");
    }
    WdfWaitLockRelease(Ctx->DevLock);
    if (!NT_SUCCESS(status)) {
        goto takedown;
    }
    q->Step = APOLLO_PLUGIN_STEP_DONE;
    q->Status = STATUS_SUCCESS;
    PlugCatDropBlobs(H);
    return;

takedown:
    ApolloLogU(Ctx, "plugins: plug-in 0x%03lx instance %lu: step %lu failed (0x%08x): taking it down", q->RenderId,
               in->Handle, q->Step, status);
    {
        NTSTATUS td;
        BOOLEAN send;

        WdfWaitLockAcquire(Ctx->DevLock, NULL);
        send = !PlugSyncDsp(Ctx, H, in->Dsp)->Quarantined;
        WdfWaitLockRelease(Ctx->DevLock);
        td = PlugUnplace(Ctx, H, in, send, TRUE);
        if (!NT_SUCCESS(td)) {
            ApolloLogU(Ctx, "plugins: the takedown of instance %lu failed too (0x%08x)", in->Handle, td);
        }
    }
    PlugInstFree(H, in);
    q->Status = status;
    q->Handle = 0;
    PlugCatDropBlobs(H);
    return;
fail:
    for (i = 0; i < in->NRes; i++) {
        PlugCatPut(H, in->Cat[i]);
    }
    ExFreePoolWithTag(in, PLUG_TAG);
    q->Status = status;
    PlugCatDropBlobs(H);
}

/* StripLock held. ua_xact_resync_replies (ua_dsp_xact.c:642-696): realign DSP Dsp's replies. */
static NTSTATUS PlugResyncReplies(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _In_ ULONG Dsp, _In_ ULONG Epoch,
                                  _In_reads_(4) const ULONG *Cmd, _In_ ULONG Hdr)
{
    ULONGLONG until;
    PLUG_XACT *x = NULL;
    NTSTATUS status;
    LONG r = POLL_PENDING, found = -1;
    ULONG i;

    status = PxBegin(Ctx, H, Dsp, Epoch, 0, &x);
    if (!NT_SUCCESS(status) || x == NULL) {
        return NT_SUCCESS(status) ? STATUS_INSUFFICIENT_RESOURCES : status;
    }
    x->L.RxFill = PLUG_READ_FILL;
    for (i = 0; i < PLUG_RESYNC_RX; i++) {
        (VOID)PlugXlRx(&x->L, Hdr & 0xFFFFu);
    }
    PlugXlInline(&x->L, Cmd[0], Cmd[1], Cmd[2], Cmd[3]);
    status = PxMap(Ctx, H, x);
    if (NT_SUCCESS(status)) {
        status = PxPublish(Ctx, H, x);
    }
    until = KeQueryInterruptTime() + MS_TO_100NS(PLUG_WAIT_MS);
    while (NT_SUCCESS(status) && found < 0) {
        WdfWaitLockAcquire(Ctx->DevLock, NULL);
        r = PxPoll(Ctx, x);
        if (r >= 0) {
            KeMemoryBarrier();
            for (i = 0; i < x->L.NRx; i++) {
                if (PxReplyWord(H, x, i, 0) == Hdr && PxReplyWord(H, x, i, 1) == Cmd[3]) {
                    found = (LONG)i;
                    break;
                }
            }
        }
        WdfWaitLockRelease(Ctx->DevLock);
        if (r < 0) {
            status = (r == POLL_GONE) ? STATUS_DEVICE_NOT_CONNECTED : STATUS_CANCELLED;
        } else if (found < 0 && r == POLL_DONE) {
            status = STATUS_DEVICE_PROTOCOL_ERROR;      /* every descriptor took a stray */
        } else if (found < 0 && KeQueryInterruptTime() > until) {
            status = STATUS_IO_TIMEOUT;
        } else if (found < 0) {
            ApolloSleepMs(1);
        }
    }
    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (found >= 0) {
        for (i = 0; i < (ULONG)found; i++) {
            ApolloLogU(Ctx, "plugins: DSP %lu stray reply %lu: %08x %08x %08x %08x", Dsp, i, PxReplyWord(H, x, i, 0),
                       PxReplyWord(H, x, i, 1), PxReplyWord(H, x, i, 2), PxReplyWord(H, x, i, 3));
        }
        ApolloLogU(Ctx, "plugins: DSP %lu replies realigned after %ld stray repl%s", Dsp, found, found == 1 ? "y" : "ies");
        status = STATUS_SUCCESS;
    } else if (status != STATUS_DEVICE_NOT_CONNECTED && Ctx->Strips.Epoch == Epoch) {
        PlugQuarantine(Ctx, H, Dsp, "its replies could not be realigned after one that was not ours", status);
    }
    WdfWaitLockRelease(Ctx->DevLock);
    PxEnd(H, x);            /* still published (spare descriptors unfilled): its pages are parked */
    return status;
}

/* StripLock held. SERVICE (ua_plugin_instance.c:1013-1118): checked WRITEs, then an optional read. */
static VOID PlugService(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _In_ WDFFILEOBJECT File,
                        _Inout_ PLUG_REQUEST *R)
{
    APOLLO_PLUGIN_INSTANCE *q = &R->Hdr;
    PLUG_INST *in;
    PLUG_XACT *x = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG nw = q->NCommands, nr = q->ReadWords, row = q->ReadRef >> 24, word = q->ReadRef & 0xFFFFFFu, ncmds = 0, i;
    ULONG probe[4], dsp = 0, epoch = 0;
    BOOLEAN resync = FALSE;

    q->Step = nw ? APOLLO_PLUGIN_STEP_SERVICE_WRITE : APOLLO_PLUGIN_STEP_SERVICE_READ;
    in = PlugFind(H, File, q->Handle);
    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (in == NULL) {
        status = STATUS_NOT_FOUND;
    } else if (in->Lost || !in->Placed || in->Epoch != Ctx->Strips.Epoch || q->Epoch != in->Epoch ||
               !PlugDspAdmitted(Ctx, H, in->Dsp)) {
        status = STATUS_CANCELLED;
    } else if (!in->Loaded || (!in->Started && !in->StripOverride && in->AuxBus == 0)) {
        status = STATUS_INVALID_DEVICE_STATE;   /* unrouted: no SERVICE (ua_plugin_instance.c:1043-1046, finding 4) */
    } else if (nw && PlugCheckCmds(in->Rows, in->NPriv, R->Cmd, nw, &ncmds) != PLUG_OK) {
        status = STATUS_INVALID_PARAMETER;
    } else if (nw && (in->StripOverride || in->AuxBus != 0) && PlugWritesDirectory(R->Cmd, nw)) {
        status = STATUS_INVALID_PARAMETER;      /* a topology change is a new admitted CREATE */
    } else if (nr && (row >= in->NPriv || word >= in->Rows[row].Dwords || nr > in->Rows[row].Dwords - word)) {
        status = STATUS_INVALID_PARAMETER;
    }
    WdfWaitLockRelease(Ctx->DevLock);
    if (!NT_SUCCESS(status) || in == NULL) {
        q->Status = status;
        return;
    }
    for (i = 0; i < in->NPriv; i++) {
        H->Addr[i] = in->Lease[PLUG_RES_MAX + i].Base;
    }
    status = PxBegin(Ctx, H, in->Dsp, in->Epoch, nw ? PLUG_PAGE : 0, &x);
    if (!NT_SUCCESS(status) || x == NULL) {
        q->Status = NT_SUCCESS(status) ? STATUS_INSUFFICIENT_RESOURCES : status;
        return;
    }
    if (nw && PlugRelocate(H->Reloc, R->Cmd, nw, H->Addr, in->NPriv) == PLUG_OK) {
        (VOID)PlugXlCmdWords(&x->L, &x->Pay, H->Reloc, nw);
    } else if (nw) {
        x->L.Err = PLUG_EINVAL;
    }
    if (nr) {
        (VOID)PlugBuildRead(&x->L, H->Addr[row] + word, nr, word);
    }
    status = PxRun(Ctx, H, x, PLUG_WAIT_MS, nr ? "service read" : "service write");
    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (x->State != PX_BUILDING) {
        PlugSyncDsp(Ctx, H, in->Dsp)->Writes += ncmds;
    }
    if (NT_SUCCESS(status) && (in->Lost || Ctx->Strips.Epoch != in->Epoch)) {
        status = STATUS_CANCELLED;
    }
    if (NT_SUCCESS(status) && nr) {
        ULONG r[2];

        r[0] = PxReplyWord(H, x, 0, 0);
        r[1] = PxReplyWord(H, x, 0, 1);
        if (!PlugReadReplyOk(r, nr, word)) {
            status = STATUS_DEVICE_PROTOCOL_ERROR;
            q->Step = APOLLO_PLUGIN_STEP_SERVICE_READ;
            q->Detail[0] = r[0];
            q->Detail[1] = r[1];
            ApolloLogU(Ctx, "plugins: DSP %lu read of %lu words at row %lu word %lu (0x%05lx) got a reply that is not its "
                       "own: %08x %08x", in->Dsp, nr, row, word, H->Addr[row] + word, r[0], r[1]);
            /* a different length than the refused read's: its late reply cannot pass for the probe's */
            probe[0] = PLUG_CMD_READ;
            probe[1] = H->Addr[row] + word;
            probe[2] = (nr == 1 && in->Rows[row].Dwords - word >= 2) ? 2 : 1;
            probe[3] = word;
            dsp = in->Dsp;
            epoch = in->Epoch;
            resync = TRUE;
        } else {
            for (i = 0; i < nr; i++) {
                R->Values[i] = PxReplyWord(H, x, 0, 2 + i);
            }
        }
    }
    WdfWaitLockRelease(Ctx->DevLock);
    PxEnd(H, x);
    if (resync) {
        (VOID)PlugResyncReplies(Ctx, H, dsp, epoch, probe, PLUG_REPLY_READ | (probe[2] + 2));
    }
    q->Status = status;
    if (NT_SUCCESS(status)) {
        q->Step = APOLLO_PLUGIN_STEP_DONE;
    }
}

/*
 * StripLock held. START (ua_plugin_start_staged, ua_plugin_instance.c:1277-1332): route a staged
 * chain of this handle on its inputs, as F_START would have. Every refusal comes before anything
 * is sent; then one transaction (1 s bound; unproven: the DSP is quarantined).
 */
static VOID PlugStartStaged(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _In_ WDFFILEOBJECT File,
                            _Inout_ PLUG_REQUEST *R)
{
    APOLLO_PLUGIN_INSTANCE *q = &R->Hdr;
    PLUG_INST *in;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG gen = 0, header[2] = { 0, 0 }, i;

    q->Step = APOLLO_PLUGIN_STEP_ROUTE;
    in = PlugFind(H, File, q->Handle);
    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (in == NULL) {
        status = STATUS_NOT_FOUND;
    } else if (!in->Staged || !in->Loaded || in->Started || (!in->StripOverride && in->AuxBus == 0) || in->Unison) {
        status = STATUS_INVALID_DEVICE_STATE;   /* not an unrouted staged chain (upstream -EPERM); Unison: ACTIVATE_UNISON */
    } else if (in->Lost || !in->Placed || in->Epoch != q->Epoch || Ctx->Strips.Epoch != q->Epoch ||
               !PlugDspAdmitted(Ctx, H, in->Dsp)) {
        status = STATUS_CANCELLED;              /* -ESTALE */
    } else {
        for (i = 0; i < APOLLO_PLUGIN_INSTANCES_MAX; i++) {
            const PLUG_INST *o = H->Inst[i];

            if (o != NULL && o != in && o->Dsp == in->Dsp && o->Placed && !o->Lost && o->Started &&
                PlugInputsOverlap(o->Synch, in->Synch)) {
                status = STATUS_DEVICE_BUSY;    /* one routed owner per input, whatever the handle */
                break;
            }
        }
    }
    if (NT_SUCCESS(status) && in != NULL && in->AuxBus == 0) {
        /* the retained plain strip (both of a pair) still live, and the same generation as at the CREATE */
        status = PlugStripTarget(Ctx, in->Dsp, in->Synch, q->Epoch, &gen, header);
        if (!NT_SUCCESS(status) || gen != in->StripGen) {
            status = STATUS_CANCELLED;
        }
    } else if (NT_SUCCESS(status) && in != NULL && !PlugAuxEnabled()) {
        status = STATUS_NOT_SUPPORTED;          /* [Ins] AuxInserts turned off since the CREATE */
    }
    if (!NT_SUCCESS(status)) {
        ApolloLogU(Ctx, "plugins: START of instance %lu refused (0x%08x): nothing sent", q->Handle, status);
    }
    WdfWaitLockRelease(Ctx->DevLock);
    if (!NT_SUCCESS(status) || in == NULL) {
        q->Status = status;
        return;
    }
    status = ReadAcquire(&Ctx->StopRequested) ? STATUS_CANCELLED : PlugStart(Ctx, H, in, NULL, 0, TRUE);
    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (in->Started) {
        in->Staged = FALSE;                     /* the route went out (proven or not): DESTROY stops and restores */
    }
    if (NT_SUCCESS(status) && (in->Lost || Ctx->Strips.Epoch != q->Epoch)) {
        status = STATUS_CANCELLED;
    }
    if (NT_SUCCESS(status)) {
        q->Dsp = in->Dsp;
        q->Step = APOLLO_PLUGIN_STEP_DONE;
    }
    WdfWaitLockRelease(Ctx->DevLock);
    q->Status = status;
}

/*
 * [Ins] StripLock held. BEGIN_UNISON / ACTIVATE_UNISON (ua_plugin_unison_lifecycle, ua_plugin_instance.c:1334-1426).
 * BEGIN: a staged, unrouted Unison chain of this handle whose plain strip is still the CREATE's generation, no
 * other routed owner on its inputs: capture the ordinary baseline, own the lane, PGA 0, fence, 80 ms; nothing
 * routed. ACTIVATE: a begun lease: route the staged chain (as op START), fence. CUSTOM stays the caller's.
 */
static VOID PlugUnisonLifecycle(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _In_ WDFFILEOBJECT File,
                                _Inout_ PLUG_REQUEST *R)
{
    APOLLO_PLUGIN_INSTANCE *q = &R->Hdr;
    BOOLEAN begin = q->Op == APOLLO_PLUGIN_INST_BEGIN_UNISON;
    PLUG_INST *in;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG gen = 0, header[2] = { 0, 0 }, i, seq = 0, words[6], nw, req[APOLLO_UNISON_SETTINGS];
    APOLLO_UNISON_IMAGE next;
    int r;

    q->Step = APOLLO_PLUGIN_STEP_ANALOG;
    in = PlugFind(H, File, q->Handle);
    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (in == NULL) {
        status = STATUS_NOT_FOUND;
    } else if (in->Lost || !in->Placed || in->Epoch != q->Epoch || Ctx->Strips.Epoch != q->Epoch ||
               !PlugDspAdmitted(Ctx, H, in->Dsp)) {
        status = STATUS_CANCELLED;
    } else if (!in->Unison || !in->Loaded || in->Started || !in->StripOverride ||
               (begin ? (in->AnalogOwned || in->UnisonBegun) : (!in->AnalogOwned || !in->UnisonBegun))) {
        status = begin && (in->AnalogOwned || in->UnisonBegun) ? STATUS_DEVICE_BUSY : STATUS_INVALID_DEVICE_STATE;
    } else {
        status = PlugStripTarget(Ctx, in->Dsp, in->Synch, q->Epoch, &gen, header);
        if (NT_SUCCESS(status) && gen != in->StripGen) {
            status = STATUS_CANCELLED;
        }
        for (i = 0; NT_SUCCESS(status) && i < APOLLO_PLUGIN_INSTANCES_MAX; i++) {
            const PLUG_INST *o = H->Inst[i];

            if (o != NULL && o != in && o->Placed && !o->Lost && (o->Started || o->UnisonBegun) &&
                PlugInputsOverlap(o->Synch, in->Synch)) {
                status = STATUS_DEVICE_BUSY;    /* another route owner on the input (upstream -EBUSY) */
            }
        }
    }
    if (NT_SUCCESS(status) && begin) {
        /* the baseline: readback flags and GainC over the write cache, only from a settled mailbox */
        r = ApolloUnisonCapture(PlugPreampModel(Ctx), in->Synch, Ctx->MixerVal, Ctx->Readback,
                                Ctx->MixerReady && Ctx->ReadbackValid && !Ctx->MixerDirty &&
                                ApolloRead(Ctx, REG_MIXER_SEQ_RD) == Ctx->MixerSeqWr,
                                PlugAnalogOwnedPreamps(H, in), H->AnalogMask, &in->AnalogBase);
        status = (r == APOLLO_UNISON_OK) ? STATUS_SUCCESS : (r == APOLLO_UNISON_AGAIN) ? STATUS_RETRY :
                 (r == APOLLO_UNISON_BUSY) ? STATUS_DEVICE_BUSY : STATUS_INVALID_PARAMETER;
        if (NT_SUCCESS(status)) {
            in->AnalogCur = in->AnalogBase;
            in->AnalogOwned = TRUE;
            in->UnisonBegun = TRUE;
            PlugAnalogRecompute(H);             /* from here every other write to these fields is refused */
            /* PGA 0 (ua_inst_analog_bootstrap), through the lease's own image */
            nw = ApolloUnisonPga0(in->Synch, words);
            r = ApolloUnisonPatch(PlugPreampModel(Ctx), &in->AnalogCur, words, nw, &next, req);
            status = (r == APOLLO_UNISON_OK) ? PlugAnalogWrite(Ctx, H, &next, req) : STATUS_INVALID_PARAMETER;
            if (NT_SUCCESS(status)) {
                in->AnalogCur = next;
            }
            ApolloLogU(Ctx, "plugins: instance %lu: Unison lease on synch 0x%05lx, baseline captured, PGA 0 (0x%08x)",
                       in->Handle, in->Synch, status);
        }
    }
    WdfWaitLockRelease(Ctx->DevLock);
    if (!NT_SUCCESS(status) || in == NULL) {
        if (in != NULL && begin && in->AnalogOwned) {
            /* the PGA 0 write itself failed: nothing proven went out, put the baseline back */
            (VOID)PlugAnalogRestore(Ctx, H, in, STATUS_SUCCESS);
            WdfWaitLockAcquire(Ctx->DevLock, NULL);
            in->UnisonBegun = FALSE;
            WdfWaitLockRelease(Ctx->DevLock);
        }
        q->Status = status;
        return;
    }
    if (begin) {
        status = PlugAnalogFence(Ctx, in->Epoch, &seq);
        if (NT_SUCCESS(status)) {
            ApolloSleepMs(APOLLO_UNISON_SETTLE_MS);
        }
    } else {
        status = ReadAcquire(&Ctx->StopRequested) ? STATUS_CANCELLED : PlugStart(Ctx, H, in, NULL, 0, TRUE);
        if (NT_SUCCESS(status)) {
            status = PlugAnalogFence(Ctx, in->Epoch, &seq);
        }
    }
    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (!begin && in->Started) {
        in->Staged = FALSE;
    }
    if (NT_SUCCESS(status) && (in->Lost || Ctx->Strips.Epoch != q->Epoch)) {
        status = STATUS_CANCELLED;
    }
    if (!NT_SUCCESS(status) && in->AnalogOwned) {
        PlugAnalogFailed(Ctx, H, in, status);
    } else if (NT_SUCCESS(status) && !begin) {
        in->UnisonBegun = FALSE;
    }
    if (NT_SUCCESS(status)) {
        q->Detail[0] = seq;
        q->Step = APOLLO_PLUGIN_STEP_DONE;
    }
    WdfWaitLockRelease(Ctx->DevLock);
    q->Status = status;
}

/*
 * [Ins] StripLock held. UNISON_PARAMS (ua_plugin_unison_params, ua_plugin_instance.c:1428-1484): the triples in the
 * command area, validated whole against the lease, written in setting order, fenced. A failed fence keeps the
 * fields blocked.
 */
static VOID PlugUnisonParams(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _In_ WDFFILEOBJECT File,
                             _Inout_ PLUG_REQUEST *R)
{
    APOLLO_PLUGIN_INSTANCE *q = &R->Hdr;
    PLUG_INST *in;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG req[APOLLO_UNISON_SETTINGS], seq = 0;
    APOLLO_UNISON_IMAGE next;
    int r;

    q->Step = APOLLO_PLUGIN_STEP_ANALOG;
    in = PlugFind(H, File, q->Handle);
    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (in == NULL) {
        status = STATUS_NOT_FOUND;
    } else if (in->Lost || in->Epoch != q->Epoch || Ctx->Strips.Epoch != q->Epoch || !PlugDspAdmitted(Ctx, H, in->Dsp)) {
        status = STATUS_CANCELLED;
    } else if (!in->Unison || !in->Loaded || !in->AnalogOwned || (!in->Started && !in->UnisonBegun)) {
        status = STATUS_INVALID_DEVICE_STATE;
    } else if (q->NCommands != 0) {
        r = ApolloUnisonPatch(PlugPreampModel(Ctx), &in->AnalogCur, R->Cmd, q->NCommands, &next, req);
        status = (r == APOLLO_UNISON_OK) ? PlugAnalogWrite(Ctx, H, &next, req) :
                 (r == APOLLO_UNISON_PERM) ? STATUS_ACCESS_DENIED : STATUS_INVALID_PARAMETER;
        if (NT_SUCCESS(status)) {
            in->AnalogCur = next;
        }
    }
    WdfWaitLockRelease(Ctx->DevLock);
    if (!NT_SUCCESS(status) || in == NULL) {
        q->Status = status;
        return;
    }
    status = PlugAnalogFence(Ctx, in->Epoch, &seq);
    if (!NT_SUCCESS(status)) {
        WdfWaitLockAcquire(Ctx->DevLock, NULL);
        if (in->AnalogOwned) {
            PlugAnalogFailed(Ctx, H, in, status);
        }
        WdfWaitLockRelease(Ctx->DevLock);
    } else {
        q->Detail[0] = seq;
        q->Step = APOLLO_PLUGIN_STEP_DONE;
    }
    q->Status = status;
}

/* ------------------------------------------------------------------ */
/* Caps                                                                 */
/* ------------------------------------------------------------------ */

/*
 * [Lat] StripLock held. Op LATENCY: a routed input chain's record path (the host knows its plug-ins' latency; the
 * driver knows only the words), kept with the instance until its route goes; nothing is sent to the device.
 */
static VOID PlugLatency(_In_ PDEVICE_CONTEXT Ctx, _In_ APOLLO_PLUGIN_HOST *H, _In_ WDFFILEOBJECT File,
                        _Inout_ PLUG_REQUEST *R)
{
    APOLLO_PLUGIN_INSTANCE *q = &R->Hdr;
    PLUG_INST *in = PlugFind(H, File, q->Handle);
    ULONG excess, was;

    if (in == NULL) {
        q->Status = STATUS_NOT_FOUND;
        return;
    }
    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    if (in->Epoch != q->Epoch || in->Epoch != Ctx->Strips.Epoch || !in->Placed || in->Lost || !in->Started ||
        !in->StripOverride || in->AuxBus != 0) {
        q->Status = STATUS_INVALID_DEVICE_STATE;
    } else {
        excess = ApolloLatencyChainExcess(q->Scratch[1], q->Scratch[0]);
        was = in->RecExcess;
        in->RecExcess = excess;
        PlugLatencyNote(Ctx, H);
        if (excess != was) {
            ApolloLogU(Ctx, "plugins: instance %lu (synch 0x%05lx): record path %lu + %lu = %lu samples, %lu over the plain "
                       "strip's %lu%s", in->Handle, in->Synch, q->Scratch[1], q->Scratch[0], q->Scratch[1] + q->Scratch[0],
                       excess, APOLLO_LAT_STRIP_RECORD_DELAY, excess ? ": added to the reported input latency" : "");
        }
        q->Detail[1] = excess;
        q->Status = STATUS_SUCCESS;
        q->Step = APOLLO_PLUGIN_STEP_DONE;
    }
    q->Detail[0] = ApolloLatencyIn(Ctx) - Ctx->EngGuard - Ctx->AdcFrames;
    WdfWaitLockRelease(Ctx->DevLock);
}

/*
 * StripLock (if Locked) and DevLock held. Caller: this handle, from the process asking, may make
 * the plug-in calls (PlugCallerAllowed); without it Operations stays 0 and Enabled says HOST only.
 */
static VOID PlugFillCaps(_In_ PDEVICE_CONTEXT Ctx, _In_opt_ APOLLO_PLUGIN_HOST *H, _In_ BOOLEAN Enabled,
                         _In_ BOOLEAN Caller, _In_ BOOLEAN AuxOn, _In_opt_ WDFFILEOBJECT File, _Out_ PAPOLLO_PLUGIN_CAPS C)
{
    const APOLLO_STRIPS *s = &Ctx->Strips;
    BOOLEAN ops = Enabled && Caller;
    ULONG d, p, i, k = 0;

    RtlZeroMemory(C, sizeof(*C));
    C->Size = sizeof(*C);
    C->Abi = APOLLO_PLUGIN_ABI;
    C->Enabled = (Enabled ? APOLLO_PLUGIN_ENABLED_HOST : 0) | (ops ? APOLLO_PLUGIN_ENABLED_CALLER : 0);
    C->Features = APOLLO_PLUGIN_FEAT_CHECK | APOLLO_PLUGIN_FEAT_PLAIN | APOLLO_PLUGIN_FEAT_START_CHAIN |
                  APOLLO_PLUGIN_FEAT_SERVICE | APOLLO_PLUGIN_FEAT_STAGE | APOLLO_PLUGIN_FEAT_STEREO |
                  APOLLO_PLUGIN_FEAT_CALLER | APOLLO_PLUGIN_FEAT_UNITS |
                  APOLLO_PLUGIN_FEAT_CHAIN4 | APOLLO_PLUGIN_FEAT_UNISON | APOLLO_PLUGIN_FEAT_AUX |    /* [Ins] */
                  APOLLO_PLUGIN_FEAT_WARM | APOLLO_PLUGIN_FEAT_LATENCY;                             /* [Warm], [Lat] */
    if (Enabled && AuxOn) {
        C->Enabled |= APOLLO_PLUGIN_ENABLED_AUX;
    }
    C->Epoch = s->Epoch;
    C->NumDsps = Ctx->NumDsps;
    C->StripDsp = s->Dsp;
    C->StripsState = s->State;
    for (i = 0; i < APOLLO_STRIP_INPUTS; i++) {
        if (s->StripStarted[i]) {
            C->StripsStarted |= 1u << i;
        }
    }
    for (d = 0; d < Ctx->NumDsps && d < APOLLO_MAX_DSPS; d++) {
        APOLLO_PLUGIN_DSP_INFO *o = &C->Dsp[d];

        o->BootWord = s->BootWord[d];
        o->Load = ApolloGateClosed(Ctx) ? 0xFFFFFFFFu : ApolloRead(Ctx, ApolloDspWindow(d) + DSP_WIN_LOAD);
        if (PlugDspOwned(Ctx, d)) {
            o->Flags |= APOLLO_PLUGIN_DSP_OWNED;
            C->OwnedMask |= 1u << d;
            if (Ctx->WarmStart) {
                o->Flags |= APOLLO_PLUGIN_DSP_WARM;                     /* [Warm] through the adoption's proof */
                if (d == s->Dsp && s->WarmUploads == STRIPS_WARM_UPLOADS_PROVEN) {
                    o->Flags |= APOLLO_PLUGIN_DSP_WARM_PROVEN;
                }
            }
        } else {
            o->Flags |= (PlugOwnWhy(Ctx, d) << APOLLO_PLUGIN_DSP_WHY_SHIFT) & APOLLO_PLUGIN_DSP_WHY_MASK;    /* [Warm] */
        }
        if (PlugRingsLive(Ctx, d)) {
            o->Flags |= APOLLO_PLUGIN_DSP_RINGS;
        }
        if (d == s->Dsp) {
            o->Flags |= APOLLO_PLUGIN_DSP_STRIPS;
        }
        if (H == NULL) {
            continue;
        }
        if (PlugDspAdmitted(Ctx, H, d) && PlugStripsSettled(Ctx, d)) {
            o->Flags |= APOLLO_PLUGIN_DSP_ADMITTED;
        }
        {
            PLUG_DSP *pd = PlugSyncDsp(Ctx, H, d);

            if (pd->Quarantined) {
                o->Flags |= APOLLO_PLUGIN_DSP_QUARANTINED;
                C->QuarantinedMask |= 1u << d;
            }
            if (pd->LedgerValid && pd->Ledger != NULL) {
                PLUG_USAGE u[4];

                o->Flags |= APOLLO_PLUGIN_DSP_LEDGER;
                RtlCopyMemory(o->Regs, pd->Regs, sizeof(o->Regs));
                PlugLedgerUsage(pd->Ledger, u);
                for (p = 0; p < 4; p++) {
                    o->Pool[p].Base = u[p].Base;
                    o->Pool[p].Total = u[p].Total;
                    o->Pool[p].Scratch = u[p].Scratch;
                    o->Pool[p].Used = u[p].Used;
                    o->Pool[p].Free = u[p].Free;
                    o->Pool[p].Largest = u[p].Largest;
                    o->Pool[p].Allocations = u[p].Allocations;
                    o->Pool[p].Holds = u[p].Holds;
                }
            }
            o->Xacts = pd->Xacts;
            o->Uploads = pd->Uploads;
            o->RepliesOk = pd->RepliesOk;
            o->RepliesBad = pd->RepliesBad;
            o->ZeroFills = pd->ZeroFills;
            o->Memspecs = pd->Memspecs;
            o->Writes = pd->Writes;
            o->Stops = pd->Stops;
            o->Releases = pd->Releases;
            o->Timeouts = pd->Timeouts;
        }
    }
    if (H == NULL) {
        return;
    }
    if (ops && Ctx->Connected && PlugRingsLive(Ctx, 0) && !ApolloGateClosed(Ctx)) {
        C->Operations |= APOLLO_PLUGIN_OP_BOARD_QUERY;
        for (d = 1; d < Ctx->NumDsps && d < APOLLO_MAX_DSPS; d++) {
            if (PlugDspAdmitted(Ctx, H, d) && PlugStripsSettled(Ctx, d)) {
                C->Operations |= APOLLO_PLUGIN_OP_CREATE | APOLLO_PLUGIN_OP_SERVICE;
            }
        }
    }
    if (ops) {
        C->Operations |= APOLLO_PLUGIN_OP_DESTROY;
    }
    for (i = 0; i < APOLLO_PLUGIN_INSTANCES_MAX; i++) {
        const PLUG_INST *in = H->Inst[i];
        APOLLO_PLUGIN_INSTANCE_INFO *o;

        if (in == NULL) {
            continue;
        }
        C->Instances++;
        o = &C->Inst[k++];
        o->Handle = in->Handle;
        o->RenderId = in->RenderId;
        o->Dsp = in->Dsp;
        o->Synch = in->Synch;
        o->Epoch = in->Epoch;
        o->Rows = in->NPriv;
        o->HeadAddress = in->Lease[PLUG_RES_MAX].Base;
        for (p = 0; p < in->NRes; p++) {
            o->Modules += in->StripShared[p] ? 0 : 1;
        }
        o->Flags = (in->Placed ? APOLLO_PLUGIN_INST_PLACED : 0) | (in->Loaded ? APOLLO_PLUGIN_INST_LOADED : 0) |
                   (in->Started ? APOLLO_PLUGIN_INST_STARTED : 0) | (in->Lost ? APOLLO_PLUGIN_INST_LOST : 0) |
                   (in->StripOverride ? APOLLO_PLUGIN_INST_OVERRIDE : 0) |
                   (in->Staged ? APOLLO_PLUGIN_INST_STAGED : 0) |
                   (in->Unison ? APOLLO_PLUGIN_INST_UNISON : 0) | (in->AnalogOwned ? APOLLO_PLUGIN_INST_ANALOG : 0) |
                   (in->AuxBus != 0 ? APOLLO_PLUGIN_INST_AUX : 0) |
                   (File != NULL && in->Owner == File ? APOLLO_PLUGIN_INST_MINE : 0);
    }
    for (i = 0; i < PLUG_PAGES_MAX; i++) {
        if (H->Page[i].State != PAGE_NONE) {
            C->PagesAllocated++;
        }
        if (H->Page[i].State == PAGE_PARKED) {
            C->PagesParked++;
        }
    }
    for (i = 0; i < PLUG_CATALOG_MAX; i++) {
        if (H->Cat[i].Users != 0) {
            C->CatalogEntries++;
        }
    }
    C->BoardQueries = H->BoardQueries;
    C->BoardQueryFailures = H->BoardQueryFailures;
    C->Admitted = H->Admitted;
    C->Refused = H->Refused;
    C->LastStatus = H->LastStatus;
    C->LastStep = H->LastStep;
}

/* ------------------------------------------------------------------ */
/* IOCTL entry (ioctl.c), PASSIVE, no lock held                          */
/* ------------------------------------------------------------------ */

BOOLEAN ApolloPluginIsIoctl(_In_ ULONG Code)
{
    return Code == IOCTL_APOLLO_GET_PLUGIN_CAPS || Code == IOCTL_APOLLO_BOARD_QUERY ||
           Code == IOCTL_APOLLO_PLUGIN_INSTANCE || Code == IOCTL_APOLLO_PLUGIN_DELAY;
}

static NTSTATUS PlugLock(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Ms)
{
    LONGLONG t = -10000LL * (LONGLONG)Ms;

    if (Ctx->StripLock == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    return (WdfWaitLockAcquire(Ctx->StripLock, &t) == STATUS_TIMEOUT) ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
}

/* The request header and arrays, checked as ua_plugin_ioctl_instance does (ua_plugin_instance.c:1523-1658). */
static NTSTATUS PlugParseInstance(_In_ PDEVICE_CONTEXT Ctx, _In_reads_bytes_(InLen) const UCHAR *In, _In_ size_t InLen,
                                  _Inout_ PLUG_REQUEST *R, _Inout_ APOLLO_PLUGIN_HOST *H)
{
    APOLLO_PLUGIN_INSTANCE *q = &R->Hdr;
    size_t need;
    ULONG f, e, ncmds = 0, i, nr = 0;

    if (InLen < sizeof(*q)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlCopyMemory(q, In, sizeof(*q));
    if (q->Size != sizeof(*q) || q->Status != 0 || q->Step != 0 || q->LicenseState != 0 || q->LicenseWord != 0 ||
        q->Uploads != 0 || q->Resident != 0) {
        return STATUS_INVALID_PARAMETER;
    }
    for (i = 0; i < 4; i++) {
        if (q->Detail[i] != 0 || (i < 3 && q->Reserved[i] != 0)) {
            return STATUS_INVALID_PARAMETER;
        }
    }
    if (q->NResources > PLUG_RES_MAX || q->NPrivate > PLUG_PRIV_MAX || q->NCommands > PLUG_CMD_WORDS_MAX) {
        return STATUS_INVALID_PARAMETER;
    }
    need = sizeof(*q) + (size_t)q->NResources * 32 + (size_t)q->NPrivate * sizeof(PLUG_ROW) +
           (size_t)q->NCommands * sizeof(ULONG);
    if (InLen < need) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    In += sizeof(*q);
    RtlCopyMemory(R->Sha, In, (size_t)q->NResources * 32);
    In += (size_t)q->NResources * 32;
    RtlCopyMemory(R->Rows, In, (size_t)q->NPrivate * sizeof(PLUG_ROW));
    In += (size_t)q->NPrivate * sizeof(PLUG_ROW);
    RtlCopyMemory(R->Cmd, In, (size_t)q->NCommands * sizeof(ULONG));
    R->NIds = 0;
    R->Carrier = PLUG_MIC_CARRIER;
    R->Aux = PLUG_NONE;
    q->Step = APOLLO_PLUGIN_STEP_VALIDATE;

    switch (q->Op) {
    case APOLLO_PLUGIN_INST_DESTROY:
        if (q->Flags || !q->Handle || q->RenderId || q->Dsp || q->NResources || q->NPrivate || q->NCommands || q->Synch ||
            q->Scratch[0] || q->Scratch[1] || q->Scratch[2] || q->ReadRef || q->ReadWords) {
            return STATUS_INVALID_PARAMETER;
        }
        return STATUS_SUCCESS;
    case APOLLO_PLUGIN_INST_SERVICE:
        if (q->Flags || !q->Handle || !q->Epoch || q->RenderId || q->Dsp || q->NResources || q->NPrivate || q->Synch ||
            q->Scratch[0] || q->Scratch[1] || q->Scratch[2] || (q->NCommands && q->NCommands < 4) ||
            q->ReadWords > PLUG_READ_WORDS_MAX || (!q->NCommands && !q->ReadWords) || (!q->ReadWords && q->ReadRef)) {
            return STATUS_INVALID_PARAMETER;
        }
        return STATUS_SUCCESS;
    case APOLLO_PLUGIN_INST_START:
    case APOLLO_PLUGIN_INST_BEGIN_UNISON:
    case APOLLO_PLUGIN_INST_ACTIVATE_UNISON:
        /* handle and epoch of a staged instance, every other field zero (ua_plugin_instance.c:1288-1291, :1342-1345) */
        if (q->Flags || !q->Handle || !q->Epoch || q->RenderId || q->Dsp || q->NResources || q->NPrivate ||
            q->NCommands || q->Synch || q->Scratch[0] || q->Scratch[1] || q->Scratch[2] || q->ReadRef || q->ReadWords) {
            return STATUS_INVALID_PARAMETER;
        }
        return STATUS_SUCCESS;
    case APOLLO_PLUGIN_INST_UNISON_PARAMS:
        /* [Ins] handle, epoch and NCommands = 3 x triples in the command area (ua_plugin_instance.c:1435-1438) */
        if (q->Flags || !q->Handle || !q->Epoch || q->RenderId || q->Dsp || q->NResources || q->NPrivate || q->Synch ||
            q->Scratch[0] || q->Scratch[1] || q->Scratch[2] || q->ReadRef || q->ReadWords || q->NCommands % 3 != 0 ||
            q->NCommands > APOLLO_PLUGIN_UNISON_WORDS_MAX) {
            return STATUS_INVALID_PARAMETER;
        }
        return STATUS_SUCCESS;
    case APOLLO_PLUGIN_INST_LATENCY:
        /* [Lat] handle, epoch, Scratch[0] latency through the record tap, Scratch[1] record delay; nothing else */
        if (q->Flags || !q->Handle || !q->Epoch || q->RenderId || q->Dsp || q->NResources || q->NPrivate || q->NCommands ||
            q->Synch || q->Scratch[2] || q->ReadRef || q->ReadWords || q->Scratch[0] > APOLLO_PLUGIN_LATENCY_MAX ||
            q->Scratch[1] > APOLLO_PLUGIN_RECORD_DELAY_MAX) {
            return STATUS_INVALID_PARAMETER;
        }
        return STATUS_SUCCESS;
    case APOLLO_PLUGIN_INST_CREATE:
        break;
    default:
        return STATUS_INVALID_PARAMETER;
    }

    f = q->Flags;
    if (q->Handle || q->Epoch || q->ReadRef || q->ReadWords || PlugCreateFlagsOk(f) != PLUG_OK ||   /* finding 2 */
        (q->Dsp != APOLLO_PLUGIN_DSP_ANY && q->Dsp >= Ctx->NumDsps)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (f & APOLLO_PLUGIN_F_CHECK) {
        if (q->NResources || q->NPrivate || q->NCommands || q->Synch || q->Scratch[0] || q->Scratch[1] || q->Scratch[2]) {
            return STATUS_INVALID_PARAMETER;
        }
        return STATUS_SUCCESS;
    }
    if (q->Dsp == 0) {
        return STATUS_NOT_SUPPORTED;            /* DSP 0 is the mixer's: never placed on in P0 */
    }
    if (q->NResources == 0 || q->NPrivate == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if (f & (APOLLO_PLUGIN_F_START | APOLLO_PLUGIN_F_STAGE)) {
        /* F_STAGE: the same arguments as F_START, NCommands = the startup's first page */
        if (q->Dsp == APOLLO_PLUGIN_DSP_ANY || q->NCommands < 4 || q->Scratch[0] > 0xFFFFu || q->Scratch[1] > 0xFFFFu) {
            return STATUS_INVALID_PARAMETER;
        }
        /* [Ins] an aux return pair (AuxInserts, chains only, never Unison) */
        R->Aux = PlugAuxOf(q->Synch);
        if (R->Aux != PLUG_NONE && (!(f & APOLLO_PLUGIN_F_CHAIN) || (f & APOLLO_PLUGIN_F_UNISON))) {
            return STATUS_INVALID_PARAMETER;
        }
        /* mono, or (FEAT_STEREO) a linked pair of the unit's inputs: malformed EINVAL, another pairing ENOTSUP */
        e = (R->Aux != PLUG_NONE) ? PLUG_OK : PlugSynchCheck(q->Synch, PlugPairInputs(Ctx));
        if (e != PLUG_OK) {
            return PlugErr(e);
        }
    } else if (q->NCommands || q->Synch || q->Scratch[0] || q->Scratch[1] || q->Scratch[2]) {
        return STATUS_INVALID_PARAMETER;
    }
    /* the rows are checked whole before the device is asked */
    if (PlugCheckRows(R->Rows, q->NPrivate, H->Words2N, H->WordsN) != PLUG_OK) {
        return STATUS_INVALID_PARAMETER;
    }
    for (i = 0; i < q->NPrivate; i++) {
        if (R->Rows[i].Dwords == 0 || R->Rows[i].Dwords >= 0x01000000u) {
            return STATUS_INVALID_PARAMETER;
        }
    }
    if (q->NCommands) {
        if (PlugCheckCmds(R->Rows, q->NPrivate, R->Cmd, q->NCommands, &ncmds) != PLUG_OK) {
            return STATUS_INVALID_PARAMETER;
        }
        f = PlugChainIdsEx(R->Rows, q->NPrivate, R->Cmd, q->NCommands, (q->Flags & APOLLO_PLUGIN_F_CHAIN) != 0,
                           (q->Flags & APOLLO_PLUGIN_F_UNISON) != 0, q->RenderId, R->Ids, &nr, &R->Carrier);
        if (f != PLUG_OK) {
            return PlugErr(f);
        }
        R->NIds = nr;
        /* [Ins] the carrier must be the one the inputs take: the Mic strip on an analog input, the Input Strip on an
           aux return (as upstream refuses a carrier that differs from the strip its inputs run), the header for Unison */
        if (!(q->Flags & APOLLO_PLUGIN_F_UNISON) &&
            R->Carrier != ((R->Aux != PLUG_NONE) ? PLUG_INPUT_CARRIER : PLUG_MIC_CARRIER)) {
            return STATUS_NOT_SUPPORTED;        /* Virtual inputs (Input Strip carrier): not built yet */
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS ApolloPluginIoctl(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFREQUEST Request, _In_ ULONG Code,
                           _In_reads_bytes_opt_(InLen) PVOID In, _In_ size_t InLen,
                           _Out_writes_bytes_opt_(OutLen) PVOID Out, _In_ size_t OutLen, _Out_ ULONG_PTR *Info)
{
    APOLLO_PLUGIN_HOST *h = Ctx->Plugin;
    WDFFILEOBJECT file = WdfRequestGetFileObject(Request);
    CHAR why[96];
    BOOLEAN enabled;
    NTSTATUS status;

    *Info = 0;
    if (Code == IOCTL_APOLLO_PLUGIN_DELAY) {
        return STATUS_NOT_SUPPORTED;            /* phase P4 */
    }
    enabled = PlugEnabled(Ctx, why, sizeof(why));

    if (Code == IOCTL_APOLLO_GET_PLUGIN_CAPS) {
        PAPOLLO_PLUGIN_CAPS c;
        BOOLEAN locked, caller, auxOn;

        if (Out == NULL || OutLen < sizeof(APOLLO_PLUGIN_CAPS)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        /* open to every handle; it says whether this one may make the calls (before any lock) */
        caller = enabled && PlugCallerAllowed(Request);
        auxOn = PlugAuxEnabled();               /* [Ins] read before any lock */
        c = (PAPOLLO_PLUGIN_CAPS)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*c), PLUG_TAG);
        if (c == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        locked = NT_SUCCESS(PlugLock(Ctx, PLUG_CAPS_LOCK_MS));
        WdfWaitLockAcquire(Ctx->DevLock, NULL);
        PlugFillCaps(Ctx, locked ? h : NULL, enabled, caller, auxOn, file, c);
        WdfWaitLockRelease(Ctx->DevLock);
        if (enabled && !caller) {
            (VOID)RtlStringCbCopyA(why, sizeof(why),
                                   "not the plug-in host: an administrator turns hosting on from Apollo Control (PluginHostImage)");
        } else if (!locked) {
            (VOID)RtlStringCbCopyA(why, sizeof(why), "busy: an operation or a strip pass holds the host (host fields left 0)");
        } else if (enabled && h != NULL && h->Why[0] != '\0') {
            (VOID)RtlStringCbCopyA(why, sizeof(why), h->Why);
        }
        RtlCopyMemory(c->Why, why, sizeof(c->Why));
        c->Why[sizeof(c->Why) - 1] = '\0';
        if (locked) {
            WdfWaitLockRelease(Ctx->StripLock);
        }
        RtlCopyMemory(Out, c, sizeof(*c));
        ExFreePoolWithTag(c, PLUG_TAG);
        *Info = sizeof(APOLLO_PLUGIN_CAPS);
        return STATUS_SUCCESS;
    }

    if (!enabled || h == NULL) {
        if (h != NULL && !h->LoggedDisabled) {
            h->LoggedDisabled = TRUE;
            ApolloLogU(Ctx, "plugins: refused: %s", why);
        }
        return STATUS_NOT_SUPPORTED;
    }
    /* FEAT_CALLER: an administrator's handle, or PluginHostImage's own; before any check, lock or word */
    if (!PlugCallerAllowed(Request)) {
        PlugLogDenied(Ctx, h, Code);
        return STATUS_ACCESS_DENIED;
    }

    if (Code == IOCTL_APOLLO_BOARD_QUERY) {
        PAPOLLO_BOARD_QUERY q;
        ULONG date = 0, epoch = 0;

        if (In == NULL || InLen < 2 * sizeof(ULONG) || Out == NULL || OutLen < sizeof(APOLLO_BOARD_QUERY)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        if (((const ULONG *)In)[0] != sizeof(APOLLO_BOARD_QUERY) || ((const ULONG *)In)[1] != 0) {
            return STATUS_INVALID_PARAMETER;
        }
        q = (PAPOLLO_BOARD_QUERY)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*q), PLUG_TAG);
        if (q == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        status = PlugLock(Ctx, PLUG_LOCK_MS);
        if (!NT_SUCCESS(status)) {
            ExFreePoolWithTag(q, PLUG_TAG);
            return status;
        }
        h->LoggedDisabled = FALSE;
        q->Size = sizeof(*q);
        q->Status = PlugBoardQuery(Ctx, h, q->Words, &date, &epoch, q);
        q->Date = date;
        q->Epoch = epoch;
        if (NT_SUCCESS(q->Status)) {
            PlugLogCounts(Ctx, q->Words, date);
        } else {
            RtlZeroMemory(q->Words, sizeof(q->Words));
        }
        h->LastStatus = q->Status;
        h->LastStep = APOLLO_PLUGIN_STEP_LICENCE;
        WdfWaitLockRelease(Ctx->StripLock);
        RtlCopyMemory(Out, q, sizeof(*q));
        RtlSecureZeroMemory(q, sizeof(*q));
        ExFreePoolWithTag(q, PLUG_TAG);
        *Info = sizeof(APOLLO_BOARD_QUERY);
        return STATUS_SUCCESS;
    }

    if (Code == IOCTL_APOLLO_PLUGIN_INSTANCE) {
        PLUG_REQUEST *r;
        ULONGLONG t0 = KeQueryInterruptTime();
        size_t outNeed;

        if (In == NULL || file == NULL) {
            return STATUS_INVALID_PARAMETER;
        }
        r = (PLUG_REQUEST *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*r), PLUG_TAG);
        if (r == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        status = PlugLock(Ctx, PLUG_LOCK_MS);
        if (!NT_SUCCESS(status)) {
            ExFreePoolWithTag(r, PLUG_TAG);
            return status;
        }
        h->LoggedDisabled = FALSE;
        status = PlugParseInstance(Ctx, (const UCHAR *)In, InLen, r, h);
        outNeed = sizeof(APOLLO_PLUGIN_INSTANCE) +
                  ((r->Hdr.Op == APOLLO_PLUGIN_INST_SERVICE) ? (size_t)r->Hdr.ReadWords * sizeof(ULONG) : 0);
        if (NT_SUCCESS(status) && (Out == NULL || OutLen < outNeed)) {
            status = STATUS_BUFFER_TOO_SMALL;
        }
        if (!NT_SUCCESS(status)) {
            WdfWaitLockRelease(Ctx->StripLock);
            ExFreePoolWithTag(r, PLUG_TAG);
            return status;                      /* malformed: nothing was done */
        }
        switch (r->Hdr.Op) {
        case APOLLO_PLUGIN_INST_CREATE:
            PlugCreate(Ctx, h, file, r);
            break;
        case APOLLO_PLUGIN_INST_DESTROY: {
            PLUG_INST *in = PlugFind(h, file, r->Hdr.Handle);

            r->Hdr.Step = APOLLO_PLUGIN_STEP_TEARDOWN;
            r->Hdr.Status = (in == NULL) ? STATUS_NOT_FOUND : PlugRetire(Ctx, h, in);
            if (NT_SUCCESS(r->Hdr.Status)) {
                r->Hdr.Step = APOLLO_PLUGIN_STEP_DONE;
            }
            break;
        }
        case APOLLO_PLUGIN_INST_START:
            PlugStartStaged(Ctx, h, file, r);
            break;
        case APOLLO_PLUGIN_INST_BEGIN_UNISON:
        case APOLLO_PLUGIN_INST_ACTIVATE_UNISON:
            PlugUnisonLifecycle(Ctx, h, file, r);       /* [Ins] */
            break;
        case APOLLO_PLUGIN_INST_UNISON_PARAMS:
            PlugUnisonParams(Ctx, h, file, r);          /* [Ins] */
            break;
        case APOLLO_PLUGIN_INST_LATENCY:
            PlugLatency(Ctx, h, file, r);               /* [Lat] */
            break;
        default:
            PlugService(Ctx, h, file, r);
            break;
        }
        r->Hdr.ElapsedMs = PlugMsSince(t0);
        h->LastStatus = r->Hdr.Status;
        h->LastStep = r->Hdr.Step;
        WdfWaitLockRelease(Ctx->StripLock);
        RtlCopyMemory(Out, &r->Hdr, sizeof(r->Hdr));
        if (r->Hdr.Op == APOLLO_PLUGIN_INST_SERVICE && r->Hdr.ReadWords != 0) {
            if (NT_SUCCESS(r->Hdr.Status)) {
                RtlCopyMemory((PUCHAR)Out + sizeof(r->Hdr), r->Values, r->Hdr.ReadWords * sizeof(ULONG));
            } else {
                RtlZeroMemory((PUCHAR)Out + sizeof(r->Hdr), r->Hdr.ReadWords * sizeof(ULONG));
            }
        }
        ExFreePoolWithTag(r, PLUG_TAG);
        *Info = outNeed;
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_DEVICE_REQUEST;
}

/* ------------------------------------------------------------------ */
/* Lifetime                                                             */
/* ------------------------------------------------------------------ */

/* PrepareHardware: the host struct, once per device (freed at device cleanup). */
VOID ApolloPluginPrepareHardware(_In_ PDEVICE_CONTEXT Ctx)
{
    if (Ctx->Plugin == NULL) {
        Ctx->Plugin = (struct _APOLLO_PLUGIN_HOST *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(APOLLO_PLUGIN_HOST),
                                                                    PLUG_TAG);
        if (Ctx->Plugin == NULL) {
            ApolloLogU(Ctx, "plugins: no memory for the plug-in host: plug-in hosting unavailable on this unit");
            return;
        }
        Ctx->Plugin->NextHandle = 1;
    }
    ApolloLogU(Ctx, "plugins: host ready (PluginHost=%lu; %s%s)", PlugRegEnabled(),
               PlugModel(Ctx) != NULL ? PlugModel(Ctx)->Name : "no plug-in hosting on this model",
               PlugModel(Ctx) != NULL ? ": inserts on its strip DSP, its own ledger and licence answer" : "");
}

/*
 * D0 exit, no lock held, before ApolloStripsQuiesce: every instance stopped (no restore: the
 * strips' own teardown stops every strip right after) and its modules released, each waited for
 * (1 s); then lost: DESTROY or close just forgets it. [Warm] RestoreStrips (a D0 exit that leaves the
 * strips running for the next start, strips.c ApolloStripsExitKeep): each chain's plain strips are
 * SYNCHed again in the same transaction, as a DESTROY does, so the next start finds plain strips.
 */
VOID ApolloPluginQuiesce(_In_ PDEVICE_CONTEXT Ctx, _In_z_ PCSTR Why, _In_ BOOLEAN RestoreStrips)
{
    APOLLO_PLUGIN_HOST *h = Ctx->Plugin;
    ULONG i, n = 0;

    if (h == NULL || Ctx->StripLock == NULL) {
        return;
    }
    WdfWaitLockAcquire(Ctx->StripLock, NULL);
    for (i = 0; i < APOLLO_PLUGIN_INSTANCES_MAX; i++) {
        PLUG_INST *in = h->Inst[i];

        if (in == NULL || in->Lost) {
            continue;
        }
        /* [Warm] RestoreStrips: plain strips back when the strips stay running (an aux chain has no strip
           override, so nothing is restored for it) */
        (VOID)PlugUnplace(Ctx, h, in, TRUE, RestoreStrips);
        if (in->AnalogOwned) {
            /* [Ins] the Unison lease's baseline back while the mailbox still runs (else its fields stay blocked) */
            (VOID)PlugAnalogRestore(Ctx, h, in, STATUS_SUCCESS);
        }
        WdfWaitLockAcquire(Ctx->DevLock, NULL);
        in->Lost = TRUE;
        in->Loaded = FALSE;
        WdfWaitLockRelease(Ctx->DevLock);
        n++;
    }
    if (n != 0) {
        ApolloLogU(Ctx, "plugins: %lu instance(s) taken down (%s%s)", n, Why,
                   RestoreStrips ? ", plain strips restored: the strips stay running" : "");
    }
    PlugLatencyNote(Ctx, h);                    /* [Lat] none of them counts any more */
    PlugCatDropBlobs(h);
    WdfWaitLockRelease(Ctx->StripLock);
}

/*
 * EvtFileCleanup and EvtFileClose (asio.c), no lock held: the handle's instances destroyed, plain
 * strips restored. Close is the backstop for a CREATE that was still in flight at the cleanup.
 * A handle that never created one (every audio-stack handle) returns without taking a lock.
 */
VOID ApolloPluginFileCleanup(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFFILEOBJECT File)
{
    APOLLO_PLUGIN_HOST *h = Ctx->Plugin;
    ULONG i;

    if (h == NULL || Ctx->StripLock == NULL || File == NULL || ReadAcquire(&h->Live) == 0) {
        return;
    }
    WdfWaitLockAcquire(Ctx->StripLock, NULL);
    for (i = 0; i < APOLLO_PLUGIN_INSTANCES_MAX; i++) {
        PLUG_INST *in = h->Inst[i];

        if (in != NULL && in->Owner == File) {
            ULONG handle = in->Handle;
            NTSTATUS st = PlugRetire(Ctx, h, in);

            ApolloLogU(Ctx, "plugins: instance %lu destroyed at handle close (0x%08x)", handle, st);
        }
    }
    WdfWaitLockRelease(Ctx->StripLock);
}

/*
 * ReleaseHardware (gate closed). Instances are forgotten (their handles answer "not found" or
 * nothing); proven pages are freed, parked ones kept for the device's deletion unless the device
 * is gone (the strips' rule, strips.c ApolloStripsReleaseHardware).
 */
VOID ApolloPluginReleaseHardware(_In_ PDEVICE_CONTEXT Ctx)
{
    APOLLO_PLUGIN_HOST *h = Ctx->Plugin;
    BOOLEAN gone = (Ctx->Stage == ApolloStageGone);
    ULONG i, kept = 0;

    if (h == NULL || Ctx->StripLock == NULL) {
        return;
    }
    if (!gone && Ctx->Bar0 != NULL && Ctx->Bar0Length >= REG_FPGA_REV + 4) {
        gone = READ_REGISTER_ULONG((volatile ULONG *)(Ctx->Bar0 + REG_FPGA_REV)) == 0xFFFFFFFFu;
    }
    WdfWaitLockAcquire(Ctx->StripLock, NULL);
    WdfWaitLockAcquire(Ctx->DevLock, NULL);
    for (i = 0; i < APOLLO_PLUGIN_INSTANCES_MAX; i++) {
        PLUG_INST *in = h->Inst[i];

        if (in != NULL) {
            in->Lost = TRUE;
            in->Loaded = FALSE;
            in->Placed = FALSE;
            in->Started = FALSE;
            in->ScratchClaim = FALSE;
            in->AnalogOwned = FALSE;
            in->UnisonBegun = FALSE;
            RtlZeroMemory(in->Lease, sizeof(in->Lease));
        }
    }
    /* [Ins] the unit is started again from here: no lease, nothing blocked (upstream: "until driver reprobe") */
    RtlZeroMemory(h->AnalogBlock, sizeof(h->AnalogBlock));
    RtlZeroMemory(h->AnalogMask, sizeof(h->AnalogMask));
    for (i = 0; i < APOLLO_MAX_DSPS; i++) {
        h->Dsp[i].LedgerValid = FALSE;
    }
    if (h->X.State == PX_PUBLISHED) {
        PxEnd(h, &h->X);
    }
    h->X.State = PX_FREE;
    for (i = 0; i < PLUG_PAGES_MAX; i++) {
        PLUG_PAGEBUF *p = &h->Page[i];

        if (p->State == PAGE_NONE) {
            continue;
        }
        if (p->State == PAGE_PARKED && !gone) {
            kept++;                             /* the DSP may still reference it */
            continue;
        }
        if (p->Buffer != NULL) {
            WdfObjectDelete(p->Buffer);
        }
        RtlZeroMemory(p, sizeof(*p));
    }
    WdfWaitLockRelease(Ctx->DevLock);
    PlugCatDropBlobs(h);
    WdfWaitLockRelease(Ctx->StripLock);
    if (kept) {
        ApolloLogU(Ctx, "plugins: %lu unproven transaction page(s) left to the device's deletion", kept);
    }
}

/* Device cleanup: nothing can reach the host any more. Common buffers go with the DMA enabler. */
VOID ApolloPluginFree(_In_ PDEVICE_CONTEXT Ctx)
{
    APOLLO_PLUGIN_HOST *h = Ctx->Plugin;
    ULONG i;

    if (h == NULL) {
        return;
    }
    for (i = 0; i < APOLLO_PLUGIN_INSTANCES_MAX; i++) {
        if (h->Inst[i] != NULL) {
            ExFreePoolWithTag(h->Inst[i], PLUG_TAG);
            h->Inst[i] = NULL;
        }
    }
    for (i = 0; i < APOLLO_MAX_DSPS; i++) {
        if (h->Dsp[i].Ledger != NULL) {
            ExFreePoolWithTag(h->Dsp[i].Ledger, PLUG_TAG);
            h->Dsp[i].Ledger = NULL;
        }
    }
    PlugCatDropBlobs(h);
    Ctx->Plugin = NULL;
    ExFreePoolWithTag(h, PLUG_TAG);
}
