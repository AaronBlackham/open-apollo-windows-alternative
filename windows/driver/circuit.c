/*
 * circuit.c - ACX circuits: one per enabled endpoint row (docs/PHASE2.md 3.2).
 *
 * Created in EvtDeviceAdd (they need no hardware), added to the device at
 * the end of a successful PrepareHardware, removed in ReleaseHardware, as
 * the Microsoft sample does. AcxDeviceRemoveCircuit unregisters the circuit
 * but does not delete the object (Acx01000.sys, AfxDevice::RemoveCircuit), so
 * a later PrepareHardware can add it again.
 *
 * Each circuit: host pin (streaming, KSCATEGORY_AUDIO, MaxStreams 1) and a
 * bridge pin whose category is the endpoint's form factor; no elements (no
 * hardware volume: Windows uses software volume); one format, 48 kHz PCM
 * 32-bit container with 24 valid bits, the hardware's own word.
 * [Rate] One format at a time, the unit's rate: the six are made at device add, the host pin's list
 * holds the one the unit runs at (ApolloAcxSetRate). A change swaps it and tells Windows
 * (AcxPinNotifyDataFormatChange: KSEVENT_PINCAPS_FORMATCHANGE), which re-opens its streams at the new
 * format; a stream still open at the old one is silent meanwhile (stream.c).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "apollo_stream.h"
#include <devpkey.h>            /* [Endpoints] DEVPKEY_Device_FriendlyName (initguid.h came with apollo_ioctl.h) */

static EVT_ACX_PIN_RETRIEVE_NAME ApolloEvtPinRetrieveName;

/* ACX passes its own zeroed buffer (Length 0, MaximumLength 0x208); fill it. */
_Use_decl_annotations_
static NTSTATUS ApolloEvtPinRetrieveName(ACXPIN Pin, PUNICODE_STRING Name)
{
    PAPOLLO_CIRCUIT c = ApolloGetCircuit(AcxPinGetCircuit(Pin));

    if (c == NULL || c->Ep == NULL || c->Ep->Desc == NULL) {
        return STATUS_NOT_SUPPORTED;            /* ACX then falls back to the category name */
    }
    /* The header says _Out_, but the buffer is the caller's (docs/PHASE2.md 3.3). */
#pragma warning(suppress: 6001)
    return RtlUnicodeStringCopyString(Name, c->Ep->Desc->PinName);
}

/* One format object: PCM, 32-bit container, 24 valid bits, Channels at Hz. */
static NTSTATUS ApolloCircuitFormatOne(_In_ WDFDEVICE Device, _In_ ACXCIRCUIT Circuit, _In_ ULONG Channels,
                                       _In_ ULONG Hz, _Out_ ACXDATAFORMAT *Fmt)
{
    KSDATAFORMAT_WAVEFORMATEXTENSIBLE wf;
    ACX_DATAFORMAT_CONFIG cfg;
    WDF_OBJECT_ATTRIBUTES attrs;

    *Fmt = NULL;
    RtlZeroMemory(&wf, sizeof(wf));
    wf.DataFormat.FormatSize = sizeof(wf);
    wf.DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    wf.DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    wf.DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    wf.WaveFormatExt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf.WaveFormatExt.Format.nChannels = (WORD)Channels;
    wf.WaveFormatExt.Format.nSamplesPerSec = Hz;
    wf.WaveFormatExt.Format.wBitsPerSample = 32;
    wf.WaveFormatExt.Format.nBlockAlign = (WORD)(4 * Channels);
    wf.WaveFormatExt.Format.nAvgBytesPerSec = Hz * 4 * Channels;
    wf.WaveFormatExt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wf.WaveFormatExt.Samples.wValidBitsPerSample = 24;
    wf.WaveFormatExt.dwChannelMask = (Channels == 1) ? KSAUDIO_SPEAKER_MONO : KSAUDIO_SPEAKER_STEREO;
    wf.WaveFormatExt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

    ACX_DATAFORMAT_CONFIG_INIT_KS(&cfg, &wf);       /* ACX copies the format */
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ParentObject = Circuit;
    return AcxDataFormatCreate(Device, &attrs, &cfg, Fmt);
}

/*
 * [Rate] The six formats of an endpoint (rate index i at Fmt[i - 1]); the host pin's list gets the
 * 48 kHz one, as before. PrepareHardware (ApolloAcxPrepareHardware) puts the unit's stored rate in
 * before the circuit is added; a runtime change swaps it (ApolloAcxSetRate).
 */
static NTSTATUS ApolloCircuitFormat(_In_ WDFDEVICE Device, _In_ ACXCIRCUIT Circuit, _In_ ACXPIN HostPin,
                                    _In_ ULONG Channels, _Inout_ PAPOLLO_EP Ep)
{
    ACXDATAFORMATLIST list;
    NTSTATUS status;
    ULONG i;

    for (i = APOLLO_RATE_IDX_MIN; i <= APOLLO_RATE_IDX_MAX; i++) {
        status = ApolloCircuitFormatOne(Device, Circuit, Channels, ApolloRateHz(i), &Ep->Fmt[i - 1]);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }
    list = AcxPinGetRawDataFormatList(HostPin);
    if (list == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    status = AcxDataFormatListAddDataFormat(list, Ep->Fmt[APOLLO_RATE_IDX_48K - 1]);
    if (NT_SUCCESS(status)) {
        status = AcxDataFormatListAssignDefaultDataFormat(list, Ep->Fmt[APOLLO_RATE_IDX_48K - 1]);
    }
    if (NT_SUCCESS(status)) {
        Ep->HostPin = HostPin;
        Ep->FmtRate = APOLLO_RATE_DEFAULT_HZ;
    }
    return status;
}

static NTSTATUS ApolloCircuitCreate(_In_ PDEVICE_CONTEXT Ctx, _Inout_ PAPOLLO_EP Ep)
{
    const APOLLO_ENDPOINT_DESC *d = Ep->Desc;
    BOOLEAN render = (d->Type == AcxCircuitTypeRender);
    PACXCIRCUIT_INIT init;
    WDF_OBJECT_ATTRIBUTES attrs;
    ACX_PIN_CONFIG pc;
    ACX_PIN_CALLBACKS cb;
    ACXCIRCUIT circuit;
    ACXPIN pins[2];
    UNICODE_STRING name;
    NTSTATUS status;

    init = AcxCircuitInitAllocate(Ctx->Device);
    if (init == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    AcxCircuitInitSetComponentId(init, d->ComponentId);
    RtlInitUnicodeString(&name, d->RefString);
    status = AcxCircuitInitAssignName(init, &name);
    if (NT_SUCCESS(status)) {
        AcxCircuitInitSetCircuitType(init, d->Type);
        status = AcxCircuitInitAssignAcxCreateStreamCallback(init, ApolloEvtCircuitCreateStream);
    }
    if (!NT_SUCCESS(status)) {
        AcxCircuitInitFree(init);
        return status;
    }
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, APOLLO_CIRCUIT);
    status = AcxCircuitCreate(Ctx->Device, &attrs, &init, &circuit);
    if (!NT_SUCCESS(status)) {
        /* AcxCircuitCreate may have consumed (and NULLed) init on failure; free it only if not. */
        if (init != NULL) {
            AcxCircuitInitFree(init);
        }
        return status;
    }
    ApolloGetCircuit(circuit)->Ep = Ep;

    /* Host (streaming) pin: one stream, so each ring slot has one ACX writer. */
    ACX_PIN_CONFIG_INIT(&pc);
    pc.Type = render ? AcxPinTypeSink : AcxPinTypeSource;
    pc.Communication = AcxPinCommunicationSink;
    pc.Category = &KSCATEGORY_AUDIO;
    pc.MaxStreams = 1;
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ParentObject = circuit;
    status = AcxPinCreate(circuit, &attrs, &pc, &pins[0]);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Bridge pin: the endpoint itself; its name is what Sound settings shows. */
    ACX_PIN_CALLBACKS_INIT(&cb);
    cb.EvtAcxPinRetrieveName = ApolloEvtPinRetrieveName;
    ACX_PIN_CONFIG_INIT(&pc);
    pc.Type = render ? AcxPinTypeSource : AcxPinTypeSink;
    pc.Communication = AcxPinCommunicationNone;
    pc.Category = d->BridgeCategory;
    pc.PinCallbacks = &cb;
    status = AcxPinCreate(circuit, &attrs, &pc, &pins[1]);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = ApolloCircuitFormat(Ctx->Device, circuit, pins[0], d->Channels, Ep);
    if (NT_SUCCESS(status)) {
        status = AcxCircuitAddPins(circuit, pins, 2);
    }
    if (NT_SUCCESS(status)) {
        Ep->Circuit = circuit;
    }
    return status;
}

/*
 * [Twin X G0] Which circuit objects to create at device add, before the unit is identified:
 * the legacy global EnabledEndpoints (default 0x3FF), as before. They stay invisible until
 * ApolloAcxPrepareHardware adds the ones this unit's own value allows (unit.c), so a unit key
 * can narrow this set, and a unit that is not an x8p adds none unless its key says so.
 * [Twin X G2] Only on the x8p's hardware ID (or one that could not be read): a device that
 * came in by any other INF line (the Twin X) creates no circuit objects at all (unit.c,
 * apollo_unit.h ApolloAddTimeEndpoints), so nothing of it can ever reach Windows.
 * [G4] The Twin X's hardware ID creates the circuit objects of its own table (endpoints.c,
 * MON L/R out and in); PrepareHardware adds them only in Mode 2 (its unit's EnabledEndpoints,
 * default 0x3) once identification says it is the Twin X. Any other hardware ID: none.
 */
C_ASSERT(APOLLO_EP_DEFAULT_MASK == APOLLO_DEF_ENDPOINTS_X8P);

static ULONG ApolloAcxRegMask(_In_ PDEVICE_CONTEXT Ctx)
{
    return ApolloUnitParamBeforeIdentify(Ctx, APOLLO_PARAM_ENABLED_ENDPOINTS);
}

/*
 * [Endpoints] The device's friendly name, which Windows puts in parentheses after every endpoint name
 * ("LINE 1-2 (Apollo x8p)"): seen on this machine, the endpoints took the devnode's name, not the KS
 * interfaces' FriendlyName (the INF sets "Apollo" on the combined interfaces, yet they showed
 * "(Apollo x8p (open driver, test build))"). So the x8p hosting the combined set is called "Apollo", a
 * standalone unit "Apollo x8p" / "Apollo Twin X" (= its INF DeviceDesc). Set at device add, before
 * PrepareHardware adds any circuit, so Windows creates the endpoints under that name. A refusal costs only
 * the name (logged).
 */
static VOID ApolloAcxAssignFriendlyName(_In_ PDEVICE_CONTEXT Ctx, _In_ BOOLEAN Combined, _In_ int Model)
{
    WDF_DEVICE_PROPERTY_DATA pd;
    PCWSTR name;
    NTSTATUS status;

    if (Combined) {
        name = L"Apollo";
    } else if (Model == APOLLO_MODEL_X8P) {
        name = L"Apollo x8p";
    } else if (Model == APOLLO_MODEL_TWINX) {
        name = L"Apollo Twin X";
    } else {
        return;
    }
    WDF_DEVICE_PROPERTY_DATA_INIT(&pd, &DEVPKEY_Device_FriendlyName);
    pd.Lcid = LOCALE_NEUTRAL;
    pd.Flags = PLUGPLAY_PROPERTY_PERSISTENT;
    status = WdfDeviceAssignProperty(Ctx->Device, &pd, DEVPROP_TYPE_STRING,
                                     (ULONG)((wcslen(name) + 1) * sizeof(WCHAR)), (PVOID)name);
    ApolloLogU(Ctx, "acx: device friendly name \"%ws\": 0x%08x", name, status);
}

_Use_decl_annotations_
NTSTATUS ApolloAcxDeviceInit(PDEVICE_CONTEXT Ctx)
{
    WDF_OBJECT_ATTRIBUTES attrs;
    PAPOLLO_ACX acx = NULL;
    NTSTATUS status;
    ULONG i;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, APOLLO_ACX);
    attrs.EvtDestroyCallback = ApolloAcxContextDestroy;     /* frees parked packet memory */
    status = WdfObjectAllocateContext(Ctx->Device, &attrs, (PVOID *)&acx);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    InitializeListHead(&acx->Active);
    KeInitializeSpinLock(&acx->ParkLock);
    InitializeListHead(&acx->Parked);
    {
        /*
         * [G7] Parameters\Aggregate\EndpointSet (apollo_agg.h ApolloAggEpTableFor): 0 / absent = each unit
         * its own table, exactly the pre-G7 code below; 1 = the host (the x8p's hardware ID) creates the
         * combined table and every other unit none, so Windows never sees one channel twice.
         */
        ULONG epMask = APOLLO_AGG_EP_DEFAULT;
        int model = ApolloAddTimeModel(Ctx->HwidSubsysKnown, Ctx->HwidSubsystem);
        int which = ApolloAggEpTableFor(ApolloAggReadEndpointSet(&epMask), model);

        if (which == APOLLO_AGG_EPTABLE_COMBINED) {
            acx->Table = &g_ApolloAggEndpointTable;
            acx->Combined = TRUE;
            acx->Count = min(acx->Table->Count, (ULONG)APOLLO_ENDPOINTS_ROWS_MAX);
            acx->Mask = epMask & ApolloEpAllMask(acx->Count);
            ApolloLogU(Ctx, "acx: EndpointSet 1: this unit hosts the combined Windows endpoints (Aggregate\\Endpoints 0x%lX)",
                      acx->Mask);
        } else if (which == APOLLO_AGG_EPTABLE_NONE) {
            acx->Table = NULL;
            acx->Count = 0;
            acx->Mask = 0;
            ApolloLogU(Ctx, "acx: EndpointSet 1: the host unit has the combined endpoints, none on this unit");
        } else {
            /* [G4] the model's table by the hardware ID: the x8p's as before, the Twin X's own, else none */
            acx->Table = ApolloEndpointTableFor(model);
            acx->Count = (acx->Table != NULL) ? min(acx->Table->Count, (ULONG)APOLLO_ENDPOINTS_ROWS_MAX) : 0;
            acx->Mask = (acx->Count != 0) ? (ApolloAcxRegMask(Ctx) & ApolloEpAllMask(acx->Count)) : 0;
        }
        /* [Endpoints] the name Windows shows in parentheses: "Apollo" for the combined set, else the model's */
        ApolloAcxAssignFriendlyName(Ctx, acx->Combined, model);
    }
    Ctx->Acx = acx;
    if (acx->Mask == 0) {
        ApolloLogU(Ctx, "acx: no circuits at device add (Endpoints for this unit: 0)");
    }

    for (i = 0; i < acx->Count; i++) {
        acx->Ep[i].Desc = &acx->Table->Rows[i];
        if ((acx->Mask & (1u << i)) == 0) {
            continue;
        }
        status = ApolloCircuitCreate(Ctx, &acx->Ep[i]);
        ApolloLogU(Ctx, "acx: circuit %ws: 0x%08x", acx->Table->Rows[i].RefString, status);
        /* A circuit that fails costs its endpoint only; the device and diagnostics stay. */
    }
    return STATUS_SUCCESS;
}

/*
 * [Rate] The host pin's list = Hz's format alone (the new one added and made the default before the
 * old one goes). FALSE: nothing changed (already Hz, no circuit, or ACX refused; logged).
 */
_IRQL_requires_(PASSIVE_LEVEL)
static BOOLEAN ApolloAcxFormatSwap(_In_ PDEVICE_CONTEXT Ctx, _Inout_ PAPOLLO_EP Ep, _In_ ULONG Hz)
{
    ULONG idx = ApolloRateIndex(Hz), oldIdx = ApolloRateIndex(Ep->FmtRate);
    ACXDATAFORMATLIST list;
    NTSTATUS status;

    if (Ep->Circuit == NULL || Ep->HostPin == NULL || idx == 0 || oldIdx == 0 || Hz == Ep->FmtRate) {
        return FALSE;
    }
    list = AcxPinGetRawDataFormatList(Ep->HostPin);
    if (list == NULL) {
        return FALSE;
    }
    status = AcxDataFormatListAddDataFormat(list, Ep->Fmt[idx - 1]);
    if (NT_SUCCESS(status)) {
        status = AcxDataFormatListAssignDefaultDataFormat(list, Ep->Fmt[idx - 1]);
    }
    if (NT_SUCCESS(status)) {
        status = AcxDataFormatListRemoveDataFormat(list, Ep->Fmt[oldIdx - 1]);
    }
    if (!NT_SUCCESS(status)) {
        ApolloLogU(Ctx, "acx: %ws: format %lu Hz -> %lu Hz refused by ACX 0x%08x", Ep->Desc->RefString, Ep->FmtRate,
                  Hz, status);
        return FALSE;
    }
    Ep->FmtRate = Hz;
    return TRUE;
}

/*
 * [Rate] Every circuit of the unit advertises SampleRate (PASSIVE, no unit lock: rate.c after a change,
 * the bring-up work item after the stored rate was applied). For an added circuit Windows is told
 * (KSEVENT_PINCAPS_FORMATCHANGE through ACX): the audio engine takes the new format and its clients'
 * streams are invalidated and opened again. A no-op while the formats are the unit's.
 */
_Use_decl_annotations_
VOID ApolloAcxSetRate(PDEVICE_CONTEXT Ctx)
{
    PAPOLLO_ACX acx = Ctx->Acx;
    ULONG i, n = 0, hz = Ctx->SampleRate;
    NTSTATUS status;

    if (acx == NULL || ApolloRateIndex(hz) == 0) {
        return;
    }
    for (i = 0; i < acx->Count && i < APOLLO_ENDPOINTS_ROWS_MAX; i++) {
        PAPOLLO_EP ep = &acx->Ep[i];

        if (!ApolloAcxFormatSwap(Ctx, ep, hz)) {
            continue;
        }
        n++;
        if (ep->Added) {
            status = AcxPinNotifyDataFormatChange(ep->HostPin);
            if (!NT_SUCCESS(status)) {
                ApolloLogU(Ctx, "acx: %ws: format change notification 0x%08x", ep->Desc->RefString, status);
            }
        }
    }
    if (n != 0) {
        ApolloLogU(Ctx, "acx: %lu endpoint(s) advertise %lu Hz now; Windows re-opens their streams", n, hz);
    }
}

_Use_decl_annotations_
VOID ApolloAcxPrepareHardware(PDEVICE_CONTEXT Ctx)
{
    PAPOLLO_ACX acx = Ctx->Acx;
    NTSTATUS status;
    ULONG i, unitMask;

    if (acx == NULL) {
        return;
    }
    /* [Twin X G0] this unit's Endpoints value (unit.c; 0 below Mode 2 and for a new model). */
    unitMask = ApolloUnitParam(Ctx, APOLLO_PARAM_ENABLED_ENDPOINTS);
    if (acx->Combined) {
        /*
         * [G7] The combined table: Aggregate\Endpoints (read at device add), in Mode 2 only, and (below)
         * only on the model whose plan has endpoints and only if identification says it is the x8p.
         * The unit's own Endpoints value stays its standalone value, untouched for the way back.
         */
        unitMask = (ApolloUnitParam(Ctx, APOLLO_PARAM_MODE) >= APOLLO_MODE_FULL) ? acx->Mask : 0;
    }
    {
        /*
         * [Twin X G3] and only on a model whose plan has Windows endpoints (x8p; the Twin X since G4).
         * [G4] and only from the table of the model identified: a device whose hardware ID chose
         * another model's table adds nothing.
         */
        const APOLLO_BRINGUP_PLAN *plan = ApolloPlanOf(Ctx);

        if (plan == NULL || !plan->Endpoints) {
            unitMask = 0;
        }
        if (unitMask != 0 && (acx->Table == NULL || acx->Table->Model != Ctx->Ident.Model)) {
            ApolloLogU(Ctx, "acx: the endpoint table chosen at device add is not this unit's model (%d): none added",
                      Ctx->Ident.Model);
            unitMask = 0;
        }
    }
    unitMask &= ApolloEpAllMask(acx->Count);
    acx->InForce = unitMask;            /* [Endpoints] GET_ENDPOINTS: what this start added from */
    ApolloLogU(Ctx, "acx: %s endpoint set, mask 0x%lX of %lu rows", acx->Combined ? "combined" : "standalone",
              unitMask, acx->Count);
    for (i = 0; i < acx->Count; i++) {
        PAPOLLO_EP ep = &acx->Ep[i];

        if (ep->Circuit == NULL || ep->Added) {
            continue;
        }
        if ((unitMask & (1ul << i)) == 0) {
            continue;                   /* [Endpoints] not in the mask (logged above, once) */
        }
        /* [Rate] the unit's stored rate before Windows sees the circuit (48 kHz: unchanged) */
        (VOID)ApolloAcxFormatSwap(Ctx, ep, Ctx->SampleRate);
        status = AcxDeviceAddCircuit(Ctx->Device, ep->Circuit);
        ep->Added = NT_SUCCESS(status);
        ApolloLogU(Ctx, "acx: add %ws: 0x%08x", ep->Desc->RefString, status);
    }
    if (acx->Combined && !acx->Hosted) {
        for (i = 0; i < acx->Count; i++) {
            if (acx->Ep[i].Added) {
                /* [G7] the aggregate runs a timeline for these from now on (agg.c AggRebuild) */
                acx->Hosted = TRUE;
                ApolloAggEndpointsHosted(Ctx, TRUE);
                break;
            }
        }
    }
}

_Use_decl_annotations_
VOID ApolloAcxReleaseHardware(PDEVICE_CONTEXT Ctx)
{
    PAPOLLO_ACX acx = Ctx->Acx;
    NTSTATUS status;
    ULONG i;

    if (acx == NULL) {
        return;
    }
    for (i = 0; i < acx->Count; i++) {
        PAPOLLO_EP ep = &acx->Ep[i];

        if (!ep->Added) {
            continue;
        }
        status = AcxDeviceRemoveCircuit(Ctx->Device, ep->Circuit);
        ep->Added = FALSE;
        ApolloLogU(Ctx, "acx: remove %ws: 0x%08x", ep->Desc->RefString, status);
    }
    if (acx->Hosted) {
        acx->Hosted = FALSE;
        ApolloAggEndpointsHosted(Ctx, FALSE);  /* [G7] */
    }
}
