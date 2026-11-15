# Phase 2 design: Windows audio endpoints (ACX)

Status: design, Nov 14 2026. Steps 0, 1, 2 (`MON L/R` playback) and 3 (`MON L/R` recording), the
kernel half of ASIO, the fixes from the review of steps 0/1/ASIO (F1-F8) and from the third review
of the combined build (G1-G8) are implemented and build clean (DriverVer 13.16.57.164), not yet run
on the hardware: see §9 (status, deviations, review fixes, risks) and §10
(test procedure). Hardware facts are from the Nov 14
run (PLAN.md, spec-streaming.md §1.6 note). This document is self-contained: an engineer should be
able to write the code from it without re-reading the references.

## 0. Conventions and evidence

| Tag | Meaning |
|---|---|
| `acxdevice.h:138` | ACX **1.1** header, `D:\Program Files\Windows Kits\10\Include\10.0.28000.0\km\acx\km\1.1\` (acxdevice.h and acxdriver.h are identical in 1.0) |
| `sample Device.cpp:165` | `reference/acx-sample/AudioCodec/Driver/` or `Common/` (MS-PL: read for API usage only, never copy) |
| `driver.c:91` | our driver, `apollo-win/driver/`, line numbers as of Nov 14 late morning. The input-strip work was editing `device.c`/`apollo.h`/`hw.c` at the same time, so prefer the function names given next to the numbers. |
| **[RE]** | read from `C:\Windows\System32\drivers\Acx01000.sys` 10.0.26100.9974 disassembled with the EWDK `cdb -z` and Microsoft public symbols. Function names given so it can be re-checked: `cdb -z <copy of Acx01000.sys> -y srv*<cache>*https://msdl.microsoft.com/download/symbols -c "uf Acx01000!<name>; q"` |
| **[WDF]** | KMDF source, github.com/microsoft/Windows-Driver-Frameworks `src/framework/shared/irphandlers/general/fxpkggeneral.cpp` |
| **[DOC]** | learn.microsoft.com ACX pages (acx-irps, acx-streaming, acx-device-enumeration, the EVT_ACX_STREAM_* reference pages, friendly-names-for-audio-endpoint-devices) |
| **[BUILD]** | verified Nov 14 with a throwaway probe driver (C, KMDF 1.33, ACX 1.1, /W4 /WX, Code Analysis) built by msbuild in the EWDK; not kept |

**ACX version: 1.1.** The installed runtime exports the 1.1 table: `Acx01000!AcxVersion` holds a
function count of 221 **[RE]**, which is `AcxFunctionTableNumEntries` in 1.1 (`acxfuncenum.h:441`;
1.0 has 210). Nothing in this design needs a 1.1-only call, so 1.0 would also work; 1.1 matches the
sample. Log `AcxDriverIsVersionAvailable` (`acxdriver.h:147`) for 1.1 at start.

## Decisions at a glance

| # | Decision |
|---|---|
| D1 | `apollo.sys` becomes an ACX driver in place: same FDO, same INF, same IOCTL queue. No second device object. |
| D2 | Private interface and IOCTL queue keep working unchanged (§2). Two required additions: LocalService gets access in the device SDDL, and admin IOCTLs check the opener's token in the driver. |
| D3 | One circuit per endpoint, all on the one FDO. v1 = 14 circuits: render `MON L/R`, capture MIC/LINE 1-8 mono, pairs 1-2/3-4/5-6/7-8, capture `MON L/R`. |
| D4 | Format: 48 kHz, PCM, 32-bit container, 24 valid bits, 1 or 2 channels. No float, no other rates. |
| D5 | No volume/mute/jack elements in v1 (Windows software volume). |
| D6 | Copy engine on a 1 ms `EX_TIMER_HIGH_RESOLUTION` timer at DISPATCH, own spin lock, never touches `DevLock`. It is the only reader of `AX_SAMPLE_POS` while the transport runs, and it zeroes every playback frame before render clients **add** (saturating) into it, so Windows audio and ASIO can share slots. The diagnostic WRITE_PLAYBACK IOCTL still writes directly. |
| D7 | Transport starts at connect (end of bring-up, `StartTransportAtConnect` = 1, coordinator override, see §5.6) and runs until D0 exit; the engine starts and stops with it. Streams and ASIO never start or stop it. |
| D8 | Packet streaming: event-driven (2 packets) advertised in the INF; timer-driven (1 packet) also handled. |

---

## 1. Init sequence (Q1)

ACX hooks itself into WDF as a class extension. `AcxDeviceInitInitialize` registers, on our
`WDFDEVICE_INIT` **[RE: AfxCxDeviceInitInitialize]**: class-extension PnP/power callbacks (post
PrepareHardware, pre/post ReleaseHardware, post D0Entry, pre D0Exit, post SelfManagedIoInit, pre
SelfManagedIoSuspend, post SelfManagedIoFlush); a class-extension file object config; and two WDM
IRP preprocess callbacks (IRP_MJ_DEVICE_CONTROL, and IRP_MJ_PNP / IRP_MN_QUERY_DEVICE_RELATIONS).
It does **not** set the I/O type, a default queue, the device type or security. `AcxDeviceInitialize`
then adds a dynamic IRP dispatch callback for IRP_MJ_DEVICE_CONTROL and one **non-default**
sequential queue **[RE: Acx::AfxDevice::Initialize]**.

| Where | Today | Add |
|---|---|---|
| `DriverEntry` (`driver.c:18-27`) | `WdfDriverCreate(..., WDF_NO_HANDLE)` | Receive the `WDFDRIVER` handle. Right after `WdfDriverCreate`: `ACX_DRIVER_CONFIG_INIT(&cfg)` then `AcxDriverInitialize(driver, &cfg)` (PASSIVE, `acxdriver.h:53-72`, `:116`; order as `sample Driver.cpp:114-126`). `ACX_DRIVER_CONFIG.Flags` is reserved, 0. No `DriverInitFlags`, no unload callback needed. |
| `EvtDeviceAdd`, first statement | | `ACX_DEVICEINIT_CONFIG_INIT(&ic)`; `AcxDeviceInitInitialize(DeviceInit, &ic)`. Must precede `WdfDeviceCreate` (`acxdevice.h:138`; `sample Device.cpp:128-129`). Keep the defaults: `Flags` 0 (not `AcxDeviceInitConfigRawDevice`), `SynchronizationScope` None, `ExecutionLevel` Passive (`acxdevice.h:65-90`). |
| `EvtDeviceAdd`, before create | pnp callbacks, `FILE_DEVICE_SECURE_OPEN`, `WdfDeviceIoBuffered` (`driver.c:68-88`) | Keep all. Add our file object config (§2.3). |
| `EvtDeviceAdd`, after `WdfDeviceCreate` (`driver.c:91`) | | `ACX_DEVICE_CONFIG_INIT(&dc)`; `AcxDeviceInitialize(device, &dc)` immediately (`acxdevice.h:94-114`, `:169`; `sample Device.cpp:164-165`). Then the existing interrupt, DMA enabler, work item, default queue and `GUID_DEVINTERFACE_APOLLO` (unchanged). Then: init `EngineLock`, create `TransportWork` work item, read registry tunables, **create all circuits** (§3) and keep their handles in the device context. Circuit creation needs no hardware (as `sample Device.cpp:181-183`). |
| `ApolloEvtPrepareHardware` (`device.c:36-117`) | | Only on the full success path (after `Stage = ApolloStageMapped`, `device.c:115`): `AcxDeviceAddCircuit(Device, c)` for each circuit, guarded by a `CircuitsAdded` flag (`acxdevice.h:200`; `sample Device.cpp:243-255`). The "stay started but idle" path (`device.c:101-105`) adds none, so an unsupported unit shows no endpoints but `apollo-diag` still works. Circuits become visible once the device is in D0. |
| `ApolloEvtReleaseHardware` (`device.c:119-138`) | | First: `AcxDeviceRemoveCircuit` for each added circuit, clear the flag (`acxdevice.h:231`; `sample Device.cpp:297-298`). ACX's own pre-ReleaseHardware hook has already torn its streams down. |
| `D0Entry` / `D0EntryPostInterruptsEnabled` | bring-up work item | Nothing for ACX: ACX powers circuits up and restores stream states itself after D0Entry **[DOC acx-device-enumeration]**. The bring-up work item, at its end, starts the transport if streams are prepared (§5.6). |
| `D0ExitPreInterruptsDisabled` (`device.c:190-214`) | quiesce under the gate check (`device.c:200`) | `ApolloEngineStop()` **unconditionally, before** the gate check (after a surprise removal the gate is closed and the quiesce is skipped, but the timer must still go). |
| Power policy | none | Keep it that way: **never** call `WdfDeviceAssignS0IdleSettings` (the sample does, `sample Device.cpp:395-403`; idling would quiesce the unit). |

ACX refuses query-remove and query-stop while any stream is in RUN **[DOC acx-device-enumeration]**.
Driver updates (`install.ps1`, `revert.ps1`) therefore need audio apps closed or `audiosrv`
restarted first.

## 2. Private interface and IOCTL queue under ACX (Q2: answer: they coexist)

### 2.1 What the headers offer

Nothing at device level: `ACX_DEVICEINIT_CONFIG` has only Flags, sync scope, execution level and a
context array (`acxdevice.h:65-71`); `ACX_DEVICE_CONFIG` only Flags and context (`:94-98`). The
preprocess hooks in the headers (`AcxCircuitInitAssignAcxRequestPreprocessCallback`,
`acxcircuit.h:869`; the stream variant `acxstreams.h:511`) see only KS property/method/event
requests on ACX objects. The routing lives inside Acx01000.sys, so it was read there **[RE]** and
checked against the KMDF source **[WDF]**.

### 2.2 How ACX routes requests (found in Acx01000.sys)

**IRP_MJ_CREATE.** ACX registers a class-extension create callback,
`Acx::AfxFile::EvtCxDeviceFileCreate` (file object class `WdfFileObjectWdfCanUseFsContext`,
AutoForwardCleanupClose = default). WDF calls it before any client handling. It decides "is this
ours" with `Acx::AfxDevice::IsAcxCreate`: the file object's name (`WdfFileObjectGetFileName`) must be
exactly `\` + a registered circuit/factory name (length equal, case-insensitive `_wcsnicmp`), or the
create must be relative to a file object that is already ACX (pins and streams are opened relative
to the circuit handle). If ACX, it handles the create and returns TRUE (claimed). If not, it
returns **FALSE**. On FALSE, WDF continues with the client: a driver-configured create queue if
any, else the client's `EvtDeviceFileCreate` if any, else it completes the create with
STATUS_SUCCESS (FDO default) **[WDF FxPkgGeneral::OnCreate, lines 1194-1331]**. A create on
`GUID_DEVINTERFACE_APOLLO` has no reference string (`driver.c:140`), so its file name is empty and
can never match: it always reaches us.

**IRP_MJ_DEVICE_CONTROL**, two ACX layers:

1. WDM preprocess `AfxEvtIrpPreprocessDeviceIoCtrl`: for the KS codes IOCTL_KS_PROPERTY,
   ENABLE_EVENT, DISABLE_EVENT, METHOD, RESET_STATE (0x2F0003/07/0B/0F/1B) it probes and locks the
   caller's buffers in caller context. For **any other code** it only copies the stack location
   and passes the IRP on to WDF (`WdfDeviceWdmDispatchIrp`). Our codes (device type 0x8A70,
   `apollo_ioctl.h:34-53`) are untouched.
2. Dynamic dispatch `Acx::AfxDevice::EvtDeviceWdmIrpDispatch` (registered for 0x0E only): for the
   KS codes above plus IOCTL_KS_READ_STREAM / WRITE_STREAM (0x2F4017 / 0x2F8013) it looks up the
   ACX file object and queues the IRP to that object's ACX queue, or fails it with
   STATUS_INVALID_DEVICE_REQUEST if the handle is not ACX. **Every other code goes to
   `WdfDeviceWdmDispatchIrp`**, i.e. to the client's normal dispatch: our default queue.

ACX's device-level queue is created with `DefaultQueue` FALSE (EvtIoDefault =
`AfxDevice::EvtQueueCreateHandler`), so our `WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE` call
(`driver.c:131-135`) does not collide. ACX does not call `WdfDeviceInitSetIoType`; KS IOCTLs are
METHOD_NEITHER and ACX locks them itself, so `WdfDeviceIoBuffered` stays. The MEDIA class upper
filter `ksthunk` (class key UpperFilters) is already in our stack today and already passes our
IOCTLs (apollo-diag works), so nothing changes there.

### 2.3 What we must do

| # | Change | Why |
|---|---|---|
| 1 | Keep `WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_APOLLO, NULL)`: **NULL reference string**, and never give a circuit an empty name. | The NULL reference string is what keeps our creates out of ACX. |
| 2 | Keep the default sequential PASSIVE queue and `ApolloEvtIoDeviceControl` as they are. Do **not** call `WdfDeviceConfigureRequestDispatching(..., WdfRequestTypeCreate)` and do not register our own WDM preprocess for CREATE/DEVICE_CONTROL. | Not needed; they would change the order relative to ACX's hooks. |
| 3 | Device SDDL in the INF becomes `D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;LS)(A;;GR;;;IU)`. | Device security covers every open on the FDO, including the circuit opens (`FILE_DEVICE_SECURE_OPEN`, `apollo.inf:53-54`). `audiosrv` and `audiodg` run as LocalService (`audiosrv` StartName on this PC; SID type UNRESTRICTED), and today's SDDL gives LocalService nothing: endpoints would appear but fail to open. AudioEndpointBuilder runs as LocalSystem (SY, already covered). |
| 4 | Admin gate in the driver. Add a client file object config: `WDF_FILEOBJECT_CONFIG_INIT(&fo, ApolloEvtFileCreate, NULL, NULL)`, `fo.FileObjectClass = WdfFileObjectWdfCanUseFsContext` (same as ACX's), attributes with context `APOLLO_FILE { BOOLEAN IsAdmin; }` and SynchronizationScope None (KMDF rejects a cx/client mismatch on scope and downgrades a class mismatch, **[WDF ConfigureConstraints 242-430, ConfigureFileObjectClass 452-570]**). `ApolloEvtFileCreate`: `WdfRequestGetParameters` → `Parameters.Create.SecurityContext->AccessState->SubjectSecurityContext`, `SeQuerySubjectContextToken`, `SeTokenIsAdmin` → store, complete STATUS_SUCCESS. In `ApolloEvtIoDeviceControl`, any code whose access bits include FILE_WRITE_ACCESS (`(code >> 14) & 2`) also requires `IsAdmin` on `WdfRequestGetFileObject(Request)`, else STATUS_ACCESS_DENIED. | Change 3 gives LocalService FILE_WRITE_ACCESS, which today is what gates raw register and ring IOCTLs (`apollo_ioctl.h:5-13`). The token check restores "elevated admin only". It uses the create's captured security context, so it is right whichever thread WDF calls it on. ACX-claimed handles never reach our create callback, so their context stays FALSE (deny). `ntifs.h` is needed for `SeTokenIsAdmin` **[BUILD]**. |
| 5 | Unchanged clients: `apollo-control.exe` (GENERIC_READ) and elevated `apollo-diag` (READ\|WRITE). | Verified by step 0 of §8. |

WDM-KS applications that open audio filters directly from a user process (PortAudio WDM-KS, some
DAWs) would need IU write access. Not needed for Zoom/Discord (they go through `audiodg`). Decision:
leave IU at GR; revisit only if a WDM-KS app is wanted.

### 2.4 Fallbacks if a future ACX build stops passing our requests through

| Option | How | Trade-offs |
|---|---|---|
| A. Control device object | `WdfControlDeviceInitAllocate(driver, &SDDL)`, `WdfDeviceInitAssignName(L"\\Device\\ApolloCtl0")`, `WdfDeviceCreateSymbolicLink`, its own sequential PASSIVE queue that calls the existing handler with the FDO context (global pointer set in DeviceAdd, cleared in cleanup); `WdfControlFinishInitializing`. | Own SDDL independent of the audio stack (strict admin/user split without the token check). But non-PnP: apps must open by name, get no interface arrival/removal notifications (the app's query-remove handling breaks), must be deleted by hand when the FDO goes, one name per unit. |
| B. Raw child PDO | Child PDO via a separate WDF child list (ACX keeps the default one: **[DOC acx-summary-of-objects]** "Creating non ACX WDF objects"), `WdfPdoInitAssignRawDevice`, `WdfPdoInitAllowForwardingRequestToParent`, its own interface; forward with `WdfRequestForwardToParentDeviceIoQueue`. | PnP-correct and notification-friendly, but a second device node in Device Manager, a raw device class GUID, more code, and forwarding semantics for each IOCTL. |

Prefer A if it is ever needed (smallest change for the app: swap the interface lookup for a name).

---

## 3. Circuits for v1 and scaling (Q3)

### 3.1 v1 endpoint table (one row = one circuit = one Windows endpoint)

Ring slots are the Nov 14 verified descriptor order with AX_REC_CH = 32, AX_PLAY_CH = 34. The
engine resolves slots from `Ctx->IoDescIn/IoDescOut` by channel ID at stream prepare and falls back
to the listed slot if the descriptor is unusable or the slot is ≥ RecChannels/PlayChannels.

| Circuit name = INF reference string | Type | Ch | Channel IDs | Slots | Bridge pin category | Pin name (shown) | Component ID |
|---|---|---|---|---|---|---|---|
| `ApolloMon` | Render | 2 | 0x0901, 0x0902 | play 0, 1 | KSNODETYPE_LINE_CONNECTOR (was SPEAKER: Windows named it "Speakers", ignoring the pin name, Nov 14) | `MON L/R` | {4A946843-D455-4B91-A89B-9395DD1D6D8B} |
| `ApolloIn1` .. `ApolloIn8` | Capture | 1 | 0x0001, 0x0002, 0x0103, 0x0104, 0x0305, 0x0306, 0x0307, 0x0308 | rec 0 .. 7 | KSNODETYPE_MICROPHONE | `MIC/LINE 1` .. `MIC/LINE 8` | In1 {3CC7D750-F2E0-450C-99F5-1F94F8995152}, In2 {95CD0DD6-B8CC-4F84-AA97-83BBCE6FFE80}, In3 {C2ABD96E-1C00-4BB1-8FA5-A78D24EC26AA}, In4 {F62841FF-E8A7-4CDC-AEAF-D6539E4A074F}, In5 {A5B37A86-8EFF-48FD-966C-31B3B26762C2}, In6 {528EDCF5-1DCA-4754-AC26-26DA5AAF4437}, In7 {0BDF8C4A-792B-4931-A533-CD2C7338E495}, In8 {0562CC8D-4E46-432E-9D57-5B55417F8C0E} |
| `ApolloIn12`, `ApolloIn34`, `ApolloIn56`, `ApolloIn78` | Capture | 2 | pairs of the above | rec 0-1, 2-3, 4-5, 6-7 | KSNODETYPE_MICROPHONE | `MIC/LINE 1-2` .. `MIC/LINE 7-8` | {4B40AF1C-A310-4FC1-8460-527FA48D581D}, {3FF2BED2-9BF5-43B9-B999-49CBA77DA320}, {3BA1AD47-57E9-4BB1-8BC1-D93E455377B1}, {410D997B-C1A3-4473-ACC1-E18D356A58E2} |
| `ApolloMonIn` | Capture | 2 | 0x0901, 0x0902 | rec 26, 27 | KSNODETYPE_LINE_CONNECTOR | `MON L/R` | {6AF701EC-2C61-406B-AAA9-7A58F15377FE} |

Mic inputs carry digital zero until input strips are loaded (separate work); the endpoints still
exist and stream whatever the ring holds. `ApolloMonIn` is the digital loopback used for testing.

Keep this table in one source (`endpoints.c`, an `APOLLO_ENDPOINT_DESC` array: `RefString`,
`PinName`, `ComponentId`, `Type`, `BridgeCategory`, `Channels`, `ChannelId[2]`,
`FallbackSlot[2]`) and generate the INF interface lines from it with a script, because circuit names
must match the INF reference strings exactly (`sample DriverSettings.h:47-54`).

### 3.2 Building one circuit (in EvtDeviceAdd, PASSIVE)

1. `AcxCircuitInitAllocate(device)` (`acxcircuit.h:524`); on any later failure before create,
   `AcxCircuitInitFree` (`:549`).
2. `AcxCircuitInitSetComponentId(init, &guid)` (`:576`), `AcxCircuitInitAssignName(init, &name)`
   (`:638`, the reference string), `AcxCircuitInitSetCircuitType(init, AcxCircuitTypeRender or
   AcxCircuitTypeCapture)` (`:702`, enum `:235-240`). Do not call `AcxCircuitInitAssignCategories`
   (`:671`): ACX's default set follows the circuit type **[DOC AcxCircuitInitAssignCategories]**,
   which is what the INF lines mirror (§6).
3. `AcxCircuitInitAssignAcxCreateStreamCallback(init, ApolloEvtCircuitCreateStream)` (`:908`;
   callback type `:207-230`). No circuit PnP/power callbacks needed (`ACX_CIRCUIT_PNPPOWER_CALLBACKS`,
   `:121-127`, optional; the circuit owns no hardware).
4. `AcxCircuitCreate(device, &attrs, &init, &circuit)` (`:968`). Pass real attributes with a
   circuit context (endpoint descriptor pointer): `Attributes` is `_In_`, `WDF_NO_OBJECT_ATTRIBUTES`
   triggers C6387 **[BUILD]**.
5. Two pins, `ACX_PIN_CONFIG_INIT` (`acxpin.h:313`), struct `:268-287`, `AcxPinCreate`
   (`acxpin.h:623`, parent = circuit):

| Pin | Render circuit | Capture circuit |
|---|---|---|
| Host (streaming), index 0 | Type `AcxPinTypeSink`, Communication `AcxPinCommunicationSink`, Category `KSCATEGORY_AUDIO`, `MaxStreams = 1` | Type `AcxPinTypeSource`, Communication `AcxPinCommunicationSink`, Category `KSCATEGORY_AUDIO`, `MaxStreams = 1` |
| Bridge, index 1 | Type `AcxPinTypeSource`, Communication `AcxPinCommunicationNone`, Category from the table | Type `AcxPinTypeSink`, Communication `AcxPinCommunicationNone`, Category from the table |

   (Enums `acxpin.h:57-69`; same roles as `sample RenderCircuit.cpp:534-579`,
   `CaptureCircuit.cpp:521-571`.) `MaxStreams = 1` on render keeps one owner per playback slot.
   On the bridge pin set `ACX_PIN_CALLBACKS.EvtAcxPinRetrieveName` (`acxpin.h:197-206`, type
   `:95-108`).
6. Format: build one `KSDATAFORMAT_WAVEFORMATEXTENSIBLE` (§4.6), `ACX_DATAFORMAT_CONFIG_INIT_KS`
   (`acxdataformat.h:88-99`), `AcxDataFormatCreate(device, &attrs(parent=circuit), &cfg, &fmt)`
   (`:179`; ACX copies the format: the sample passes the address of a by-value copy,
   `sample CircuitHelper.cpp:35-57`). `list = AcxPinGetRawDataFormatList(hostPin)` (`acxpin.h:823`),
   `AcxDataFormatListAddDataFormat(list, fmt)` (`acxdataformat.h:753`),
   `AcxDataFormatListAssignDefaultDataFormat(list, fmt)` (`:811`). RAW mode only (as the sample).
7. `AcxCircuitAddPins(circuit, pins, 2)` (`acxcircuit.h:1203`). No `AcxCircuitAddElements`.

### 3.3 Names in Windows Sound settings

Endpoint name = `<bridge pin name> (<device interface FriendlyName>)`, for example
`MIC/LINE 3 (Apollo x8p)`. The pin part comes from KSPROPERTY_PIN_NAME
**[DOC friendly-names]**; ACX answers it by calling `EvtAcxPinRetrieveName` and, only if that
returns STATUS_NOT_SUPPORTED, by looking up the pin Name GUID and then the Category GUID under
MediaCategories **[RE: Acx::AfxPin::RetrievePinName]**. ACX passes a zeroed buffer it owns:
`Length` 0, `MaximumLength` 0x208 (260 WCHARs) **[RE: PinNameGetNotification]**; write into it with
`RtlUnicodeStringCopyString` (ntstrsafe.h). The header annotates `Name` as `_Out_`, so Code Analysis
reports C6001 on that write **[BUILD]**: suppress 6001 on that one line with this justification.
The parenthetical part is the interface `FriendlyName` from the INF, the same `Apollo x8p` for all.
Users can rename endpoints; that is stored by Windows, not by us.

### 3.4 Elements, jacks, form factors

- **Volume/mute: none in v1.** The ACX docs say a circuit "may" aggregate elements
  **[DOC acx-summary-of-objects]**; with none, Windows uses software endpoint volume, and the
  monitor knob stays the hardware control (spec-streaming §5.4 [WIN]). If the endpoint builder
  rejects an element-less circuit (risk R3), add an `ACXVOLUME` + `ACXMUTE` pair (`acxelements.h:1091`,
  `:1029`) backed by a gain applied in the copy engine.
- **Jacks: none in v1.** Without KSPROPERTY_JACK_DESCRIPTION the endpoint counts as present.
  Later: `ACX_JACK_CONFIG` (`acxpin.h:501-530`) with `AcxConnTypeQuarter`/`AcxConnTypeXlrProfessional`,
  `AcxGenLocSeparate`, `AcxPortConnJack`, `AcxPinAddJacks` (`:695`); icons only.
- **Form factor = bridge category** (`ksmedia.h` 10.0.28000: MICROPHONE `:191`, SPEAKER `:235`,
  LINE_CONNECTOR `:338`, SPDIF_INTERFACE `:348`): SPEAKER → Speakers, MICROPHONE → Microphone
  (what Zoom/Discord pick first), LINE_CONNECTOR → Line. Scale-out: LINE/VIRTUAL/ADAT/CUE/AUX →
  LINE_CONNECTOR, S/PDIF → SPDIF_INTERFACE.

### 3.5 Scaling to every channel (PLAN decisions 3 and 4)

| | Count (x8p, descriptor, REC_CH 34 / PLAY_CH 36) |
|---|---|
| Capture mono | 34 (MIC/LINE 1-8, ADAT 1-8, S/PDIF L/R, VIRTUAL 1-8, MON L/R, AUX1/2 L/R, TALKBACK 1-2) |
| Capture stereo pairs | 17 |
| Render stereo pairs | 18 (MON, LINE 1-2 .. 7-8, ADAT 1-2 .. 7-8, S/PDIF, VIRTUAL 1-2 .. 7-8, CUE1 .. CUE4) |
| Total | 69 circuits, 207 AddInterface lines |

No circuit limit is documented or visible in the headers; each circuit is one KS filter, three
interfaces and one ACX queue, so cost is linear. Plan: all 69 rows in the endpoint table and in
the INF, plus two REG_DWORD bitmaps (or a REG_BINARY) under the device `Parameters` key,
`EnabledCapture` / `EnabledRender`, read in EvtDeviceAdd; only enabled rows get a circuit. Defaults
= the v1 set. The control app writes them (admin) and restarts the device. INF interfaces of
circuits that are not created are never enabled (ACX enables only its circuits' interfaces). The
talkback rows need AX_REC_CH = 34 (§8 step 6).

---

## 4. ACX RT streaming (Q4)

### 4.1 Stream creation: `ApolloEvtCircuitCreateStream` (PASSIVE, `acxcircuit.h:207-230`)

1. Validate the format with the `AcxDataFormatGet*` accessors (all ≤ DISPATCH): sample rate
   48000 (`acxdataformat.h:460`), channels = endpoint channels (`:285`), bits 32 (`:360`), valid
   bits 24 (`:385`), else STATUS_NOT_SUPPORTED. Mode GUID: accept RAW (only list we register).
2. `ACX_STREAM_CALLBACKS_INIT` / `AcxStreamInitAssignAcxStreamCallbacks` (`acxstreams.h:329-346`,
   `:550`): PrepareHardware, ReleaseHardware, Run, Pause.
3. `ACX_RT_STREAM_CALLBACKS_INIT` / `AcxStreamInitAssignAcxRtStreamCallbacks` (`:348-367`, `:581`):
   GetHwLatency, AllocateRtPackets, FreeRtPackets, GetCurrentPacket, GetPresentationPosition, and
   SetRenderPacket (render) or GetCapturePacket (capture).
4. `AcxStreamInitSetAcxRtStreamSupportsNotifications(StreamInit)` (`:608`, returns VOID): we signal
   packet completion.
5. `AcxRtStreamCreate(device, circuit, &attrs, &StreamInit, &stream)` (`:985`) with an
   `APOLLO_STREAM` context and an EvtCleanupCallback (§5.7). Fill the context: endpoint descriptor,
   Render flag, Channels, BlockAlign = 4 × Channels.

### 4.2 Callbacks: semantics, IRQL, our behaviour

All stream callbacks are PASSIVE (`acxstreams.h:129-319`). ACX serialises them per stream through
the stream's queue.

| Callback (typedef line) | Called | Our behaviour |
|---|---|---|
| AllocateRtPackets (`:135`) | Before the first PrepareHardware **[DOC]**. PacketCount 1 (timer-driven) or 2 (event-driven); PacketSize in bytes. | §4.3. Fail STATUS_INVALID_PARAMETER if PacketCount ∉ {1,2} or PacketSize % BlockAlign ≠ 0. |
| FreeRtPackets (`:154`) | After the last ReleaseHardware (order not guaranteed **[DOC]**) | Free MDL, buffer, the packet array. If the stream is still in the engine, remove it first (§5.7). |
| PrepareHardware (`:275`) | Stop → Pause | Resolve slots; reset counters (Frames, CompletedPackets, WrittenPacket = none, EOS = none, LastPosition = 0); `ExInitializeRundownProtection`; `InterlockedIncrement(&PreparedStreams)`; `WdfWorkItemEnqueue(TransportWork)`. Never blocks on `DevLock`. |
| Run (`:301`) | Pause → Run | Under EngineLock: `NeedSync = TRUE`, insert into `ActiveStreams`. |
| Pause (`:314`) | Run → Pause | Under EngineLock: remove from `ActiveStreams`. Then `ExWaitForRundownProtectionRelease`, `ExReInitializeRundownProtection`. Counters are kept (position continues on the next Run). |
| ReleaseHardware (`:288`) | Pause → Stop | Reset counters (packet numbers restart at 0 after this **[DOC GetCapturePacket]**); `InterlockedDecrement(&PreparedStreams)`. Transport keeps running. |
| SetRenderPacket (`:188`) | Client released packet `Packet` (0-based, monotonic) | §4.4 |
| GetCurrentPacket (`:207`) | Any time | `(ULONG)(Frames / FramesPerPacket)` under EngineLock: render = packet being read, capture = packet being filled. |
| GetCapturePacket (`:222`) | After our notification | `LastCapturePacket = (ULONG)(CompletedPackets - 1)`, `QPCPacketStart` = stored start QPC of that packet, `MoreData = FALSE`. Record `LastReadPacket` (the OS has read everything before it). Before the first completion this yields 0xFFFFFFFF, as the sample does (`sample StreamEngine.cpp:974-979`). |
| GetPresentationPosition (`:241`) | Any time | §4.5 |
| GetHwLatency (`:171`) | Stream creation | `FifoSize` in **bytes**, `Delay` in **100 ns** units **[DOC]**. Render: FifoSize = RenderLeadFrames × BlockAlign (default 144 frames, includes the 8-frame FRAME_CFG), Delay 0 (DAC latency unknown, spec-streaming §3.6). Capture: FifoSize = CaptureGuardFrames × BlockAlign (16), Delay = 2083 (10 frames x8p ADC latency at 48 kHz). |
| AssignDrmContentId (`:258`) | | Not registered. |

`AcxRtStreamNotifyPacketComplete(stream, completedPacket, qpc)` (`acxstreams.h:1024`) is callable
at ≤ DISPATCH and is `_Must_inspect_result_`. We call it from the engine timer (§5), outside
EngineLock, with the 0-based monotonic packet number that just completed and the QPC of that tick.

### 4.3 Packet memory (driver allocates)

`ACX_RTPACKET` = { Size, `WDF_MEMORY_DESCRIPTOR RtPacketBuffer`, RtPacketOffset, RtPacketSize }
(`acxstreams.h:88-103`). ACX accepts only MDL descriptors, page-aligned with a page-multiple byte
count **[DOC ALLOCATE_RTPACKETS]**; it maps the memory into the client.

- **PacketCount 2:** one buffer for both. Length = `ROUND_TO_PAGES(2 × PacketSize)`,
  `ExAllocatePool2(POOL_FLAG_NON_PAGED, len, tag)` (page-aligned at this size, zeroed),
  `IoAllocateMdl` + `MmBuildMdlForNonPagedPool`. Packet 0: MDL descriptor
  (`WDF_MEMORY_DESCRIPTOR_INIT_MDL`), offset 0, size PacketSize. Packet 1: `ACX_RTPACKET_INIT`,
  `RtPacketBuffer.Type = WdfMemoryDescriptorTypeInvalid` (shares packet 0's buffer), offset
  PacketSize, size PacketSize **[DOC]**. The engine sees one cyclic buffer of 2 × PacketSize bytes.
- **PacketCount 1:** ACX maps the buffer twice back to back, so offset 0 and the length must be a
  page multiple and a block-align multiple **[DOC acx-streaming]**: length = smallest multiple of
  lcm(PAGE_SIZE, BlockAlign) ≥ PacketSize; report that length as RtPacketSize. Cyclic buffer = that
  length.
- The packet array itself: `ExAllocatePool2` of 2 × `sizeof(ACX_RTPACKET)`, freed in FreeRtPackets.
- Derived: `BufferFrames = bufferBytes / BlockAlign`, `FramesPerPacket = RtPacketSize / BlockAlign`.

(Our probe built this exact layout clean **[BUILD]**.)

### 4.4 Render packet rules (`SetRenderPacket`)

Let `cur = Frames / FramesPerPacket`, `into = Frames % FramesPerPacket`.

| Case | Action | Return |
|---|---|---|
| EOS already received | | STATUS_INVALID_DEVICE_STATE |
| Flags other than 0 / 0x200 (KSSTREAM_HEADER_OPTIONSF_ENDOFSTREAM) | | STATUS_INVALID_PARAMETER |
| `Packet < cur`, or `Packet == cur && into > 0` | still record it (the part not yet copied is used) | STATUS_DATA_LATE_ERROR |
| `Packet > cur + 1` | ignore | STATUS_DATA_OVERRUN |
| otherwise | `WrittenPacket = Packet`, `UsesWritePacket = TRUE`; EOS: `EosFrame = Packet × FramesPerPacket + EosPacketLength / BlockAlign` | STATUS_SUCCESS |

Before the first Run, `Frames == 0`, so the client's preroll `SetRenderPacket(0)` succeeds. The
engine zero-fills a frame instead of reading the buffer when `UsesWritePacket` and its packet is
`> WrittenPacket` (underrun, counted once per packet), or when `frame ≥ EosFrame`. Docs require the
packet counter and notifications to keep advancing at real-time rate regardless **[DOC
SET_RENDER_PACKET]**: the engine does.

### 4.5 Position and QPC

`PositionInBlocks` is frames from stream start; `QPCPosition` is the KeQueryPerformanceCounter
value at which that position was valid **[DOC GET_PRESENTATION_POSITION]**. Return the engine
snapshot pair (`HwPos`, `HwQpc`, both from the same tick), never a fresh QPC with an old position:

- Render: `p = Active ? clamp(HwPos - HwBase, 0, Frames) : Frames` (frames the hardware has
  fetched of this stream).
- Capture: `p = Active ? max(0, HwPos - HwBase) : Frames`.
- Never return less than the last value returned (`LastPosition`): after a Pause/Run resync the raw
  formula can step back by up to the lead.

No hardware position register is exposed (ACX does not support it; spec-streaming §3.8 item 2).
Snapshot age ≤ 1 tick (≤ ~1 ms), which the docs call adequate for timer-driven clients.

### 4.6 Data format to advertise

One format per host pin, also the default (`AudioFormats.h:117-142` shows the same shape):
`KSDATAFORMAT` FormatSize = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE), MajorFormat
KSDATAFORMAT_TYPE_AUDIO, SubFormat KSDATAFORMAT_SUBTYPE_PCM, Specifier
KSDATAFORMAT_SPECIFIER_WAVEFORMATEX; `WAVEFORMATEXTENSIBLE`: wFormatTag 0xFFFE, nChannels 1|2,
nSamplesPerSec 48000, wBitsPerSample 32, nBlockAlign 4 × ch, nAvgBytesPerSec 192000 × ch, cbSize 22,
wValidBitsPerSample 24, dwChannelMask KSAUDIO_SPEAKER_MONO (0x4) | KSAUDIO_SPEAKER_STEREO (0x3)
(`ksmedia.h:1806-1808`), SubFormat PCM. This is the hardware word (spec-streaming §1.4), so the copy
is a plain 32-bit move with no conversion. The shared-mode engine converts from its float mix.
Float is not needed and is not offered (no FPU state in the DPC). 16-bit PCM is the only likely
later addition (exclusive-mode apps), as a shift in the copy.

---

## 5. Copy engine (Q5)

### 5.1 Why a 1 ms high-resolution timer

Default engine period is 10 ms (480 frames per packet). The 10 ms service thread
(`mixer.c:277-296`) would complete packets up to a whole period late and add its own wait jitter;
the end-of-buffer interrupt fires only every 170 ms (spec-streaming §3.3). So: `ExAllocateTimer(
ApolloEngineTick, ctx, EX_TIMER_HIGH_RESOLUTION)` and a one-shot `ExSetTimer(t, -rel, 0, NULL)`
re-armed at the end of every tick on a fixed 1 ms grid (`EngineRearm`). The callback
(`EXT_CALLBACK`) runs at DISPATCH_LEVEL.

The first build used a 1 ms periodic timer (`ExSetTimer(t, -10000, 10000, NULL)`). On the x8p
host (Nov 14), with the clock held at 1.0 ms by `ExSetTimerResolution`, it ran at 654 ticks/s:
a high-resolution timer "can expire as late as about a millisecond after the specified
expiration time but it never expires early" (Microsoft Learn, High-Resolution Timers). A periodic
timer at the clock resolution folds a late expiration into the next one. The grid keeps a slightly
overdue point and fires it at the next clock tick (minimum relative due 0.25 ms, so it never
fires back to back). Only a stall longer than a whole period starts a new grid. A one-shot
re-armed after the work cannot overlap itself.

Stop with `ExDeleteTimer(t, TRUE, TRUE, NULL)` (cancel and wait for a running callback) at PASSIVE,
never while holding EngineLock. It disables the object first, so a tick re-arming meanwhile gets
FALSE from `ExSetTimer` and sets nothing. A deleted timer is gone, so allocate on every engine
start. (A WDF timer with `UseHighResolutionTimer = WdfTrue` and `WdfTimerStop(Wait)` is an
equivalent choice.)
The service thread stays at 10 ms for notifications, readback and mixer flush.

### 5.2 State (device context, `apollo.h`)

| Field | Type | Notes |
|---|---|---|
| `EngineLock` | KSPIN_LOCK | everything below except where noted |
| `EngineTimer` | PEX_TIMER | non-NULL ⇔ engine running |
| `ActiveStreams` | LIST_ENTRY of APOLLO_STREAM | streams in Run |
| `EngRaw`, `HwPos`, `HwQpc` | ULONG, ULONGLONG, LONGLONG | position snapshot; replaces `LastRawPos/ExtPos/LastPosQpc` (`apollo.h:124-126`) while the engine runs |
| `PlayDataHead` | ULONGLONG | first hardware frame not yet given stream data |
| `PlayZeroHead` | ULONGLONG | first hardware frame not yet zeroed |
| `PlayValidUntil` | volatile LONG64 | diag WRITE_PLAYBACK claim (exists, `apollo.h:128`); interlocked |
| `PreparedStreams` | volatile LONG | interlocked |
| `TransportWork` | WDFWORKITEM | starts the transport under DevLock |
| `RenderLeadFrames` 144, `CaptureGuardFrames` 16, `ZeroAheadFrames` 4096 | ULONG | from `HKR\Parameters`, clamped 64-1024 / 0-64 / fixed |
| stats | ULONG/ULONGLONG | ticks, max tick interval µs, max tick duration µs, late ticks, bus stalls (read > 1 ms), all-ones reads |

`APOLLO_STREAM` (ACXSTREAM context): `Dev`, `Ep`, `Stream`, `Render`, `Channels`, `BlockAlign`,
`Slot[2]`, `Buffer`, `Mdl`, `BufferFrames`, `FramesPerPacket`, `PacketCount`; under EngineLock:
`ActiveLink`, `Active`, `NeedSync`, `HwBase` (hardware frame of stream frame 0), `Frames`
(render: consumed; capture: produced), `CompletedPackets`, `PacketStartQpc` (capture),
`WrittenPacket`, `UsesWritePacket`, `EosFrame`, `LastPosition`, `LastReadPacket`, counters
(underruns, overruns, dropped); plus `EX_RUNDOWN_REF Rundown`.

### 5.3 Locking

| Data | Written by (IRQL) | Read by | Protection |
|---|---|---|---|
| Position snapshot, heads | tick (DISPATCH) | stream callbacks, IOCTLs (PASSIVE) | EngineLock |
| Active list, per-stream engine fields | tick; Run/Pause/Prepare/Release/SetRenderPacket (PASSIVE) | tick, Get* callbacks | EngineLock |
| `PlayValidUntil` | WRITE_PLAYBACK (PASSIVE, under DevLock) | tick | `InterlockedCompareExchange64` read, interlocked max write |
| Ring layout (`PlayChannels`, `RecChannels`, `RingFrames`), `TransportRunning` | DevLock holders | tick | constant while the engine runs: engine stopped (timer deleted, waited) before any change, started after |
| `PreparedStreams` | stream Prepare/Release | TransportWork, TRANSPORT_STOP IOCTL | interlocked |
| A stream's buffers between unlock and notify | | tick | per-stream `EX_RUNDOWN_REF` (acquire under the lock, release after the ACX call) |

Rules: lock order DevLock → EngineLock, never the reverse; the tick never takes DevLock and never
waits; no ACX call while holding EngineLock; `KeAcquireSpinLock` at PASSIVE callers,
`KeAcquireSpinLockAtDpcLevel` in the tick; every function that takes EngineLock is non-paged (no
`alloc_text(PAGE)`, unlike the sample's PAGED_CODE callbacks). Hold time is the copy of ~48 frames
per active stream: microseconds.

### 5.4 Tick (DISPATCH, every 1 ms)

1. If `ApolloGateClosed(ctx)` or not `TransportRunning`: return.
2. `q0 = KeQueryPerformanceCounter`; `raw = ApolloRead(AX_SAMPLE_POS)`; `q1 = KeQueryPerformanceCounter`.
   All-ones: set `EngineLost`, count, return (PnP surprise removal follows; spec-streaming §3.8
   item 7). `q1 - q0 > 1 ms`: count a bus stall. One MMIO read per tick, so a dead Thunderbolt
   link cannot hold the DPC for long (P7).
3. Lock. Extend: `delta = (raw - EngRaw) & 8191`; if the QPC-expected advance exceeds
   `delta + 4096`, add the missed wraps (same rule as `audio.c:344-358`). `HwPos += delta`,
   `EngRaw = raw`, `HwQpc = q1`.
4. **Zero horizon** (replaces `ApolloPlaybackZeroAhead`, `audio.c:528-549`): for hardware frames
   `[PlayZeroHead, HwPos + ZeroAheadFrames)`: zero the whole playback frame (PlayChannels × 4
   bytes) unless the frame is `< PlayValidUntil` (diag data). Advance `PlayZeroHead`. A stalled
   timer therefore plays silence, never 170 ms-old audio (D8).
5. **Render.** `target = HwPos + RenderLeadFrames`. If `PlayDataHead < HwPos + 8` (fell behind):
   count a late tick, `PlayDataHead = HwPos + 8`. For each active render stream:
   - `NeedSync`: `HwBase = PlayDataHead - Frames`, clear it (stream frame `Frames` lands on the
     next unwritten hardware frame; start latency ≈ lead).
   - `h = HwBase + Frames`; if `h < PlayDataHead`, drop `PlayDataHead - h` stream frames
     (`Frames += …`, counted).
   - For each hardware frame `h` up to `target`: stream frame `s = h - HwBase`; source = zeros if
     underrun/EOS (§4.4, skip: the frame is already zero), else `Buffer + (s % BufferFrames) ×
     BlockAlign`; **add (saturating to the LONG range)** channel c into ring byte
     `(h % RingFrames) × PlayChannels × 4 + Slot[c] × 4`, so several clients (ACX, ASIO) can share a
     slot. `Frames = target - HwBase`.
   - While `Frames ≥ (CompletedPackets + 1) × FramesPerPacket`: queue notify
     `(stream, CompletedPackets, q1)`, `CompletedPackets++`.
   Then `PlayDataHead = target`.
6. **Capture.** `head = HwPos - CaptureGuardFrames`. For each active capture stream:
   - `NeedSync`: `HwBase = head - Frames`.
   - `h = HwBase + Frames`; if `head - h > 4096` (stall longer than half the ring), write zeros to
     the stream for the excess (keeps time), count a ring overrun.
   - For each frame `h` up to `head`: read slot `Slot[c]` of ring frame `(h % RingFrames)` (stride
     RecChannels × 4) into `Buffer + ((h - HwBase) % BufferFrames) × BlockAlign + 4c`.
     **Fan-out is free:** every stream reads the ring independently, so `ApolloIn1` and
     `ApolloIn12` both read slot 0.
   - `Frames = head - HwBase`. For each completed packet p: `PacketStartQpc = QpcAt(HwBase + p ×
     FramesPerPacket)` where `QpcAt(f) = HwQpc - (HwPos - f) × QpcFreq / 48000`; if
     `p - LastReadPacket ≥ PacketCount` count an overrun (we overwrite the oldest, as the docs
     allow); queue notify `(stream, p, q1)`; `CompletedPackets++`.
7. For each queued notify: `ExAcquireRundownProtection(&s->Rundown)` (skip if it fails). Unlock.
   Call `AcxRtStreamNotifyPacketComplete`, then `ExReleaseRundownProtection`. Update stats.

### 5.5 Timing numbers

| Quantity | Value | Basis |
|---|---|---|
| FRAME_CFG (hardware safety) | 8 frames at 44.1/48k | already programmed, `audio.c:249`, `:270`; spec-streaming §3.6 |
| RenderLeadFrames | 144 (3.0 ms) default, registry 64-1024 | must exceed 8 + 48 (one tick) + worst DPC jitter; tune in the glitch test |
| Render start latency | ≈ lead + up to 1 packet | |
| CaptureGuardFrames | 16 default, registry 0-64 | matches `CAPTURE_GUARD_FRAMES` (`audio.c:396`); Linux uses 0 (spec-streaming §3.2) |
| Reported capture delay | 10 frames = 2083 × 100 ns | x8p ADC latency (spec-streaming §3.6) |
| Stall tolerance | render: lead − 8 frames before silence; capture: ~4096 frames before loss | |

### 5.6 Transport ownership

> **Override (main session, Nov 14):** driver 10.47.35.903 already starts the transport at the end
> of `ApolloEvtBringupWork`, and it ran clean twice on Aaron's laptop (install, then an x8p power
> cycle). His Thunderbolt is the CPU-integrated controller (`Intel TypeC PCIe 466E`), not the
> discrete Maple Ridge part where X2 (start at probe killed the link) was seen. ASIO also wants the
> clock running before any client opens. So: **start at connect is the default**
> (`StartTransportAtConnect` = 1); the lazy start below is the fallback for machines that show X2.

- **Implemented (Nov 14):** started at the end of `ApolloEvtBringupWork` when the registry value
  `StartTransportAtConnect` (`HKLM\SYSTEM\CurrentControlSet\Services\apollo\Parameters`, default 1)
  is non-zero; with 0 the transport waits for IOCTL_APOLLO_TRANSPORT_START.
- Planned with the ACX streams (step 2+), for machines that show X2: a lazy start, where
  `TransportWork` (PASSIVE work item) takes DevLock and, if `Connected`, `PreparedStreams > 0` and
  not running, calls `ApolloTransportStart`; the bring-up work item does the same check at its end,
  which covers resume, where ACX re-prepares streams before our bring-up finishes.
- `ApolloTransportStart` ends with `ApolloEngineStart` (allocate and set the timer, seed the
  snapshot from `ExtPos`, `ZeroHead = HwPos`, `DataHead = HwPos + lead`).
  `ApolloTransportStop` and `ApolloHwQuiesce` begin with `ApolloEngineStop` (delete the timer with
  wait). Both run under DevLock at PASSIVE. `ApolloEvtD0ExitPreInterruptsDisabled` stops the engine
  unconditionally (also after a surprise removal), `ReleaseHardware` and device cleanup again.
- Streams never start or stop the transport; Run/Pause only join or leave `ActiveStreams`.
- IOCTL_APOLLO_TRANSPORT_STOP returns STATUS_DEVICE_BUSY while an ASIO client is open (and, with
  ACX streams, while `PreparedStreams > 0`).
- Stopped only by D0 exit, surprise removal and the stop IOCTL. If it stops under running streams
  (power-down order between ACX's hooks and our `D0ExitPreInterruptsDisabled` is ACX's choice),
  positions freeze and notifications stop until it restarts; streams resync with `NeedSync` on the
  next Run.

### 5.7 Changes to existing code paths

| Code | Change |
|---|---|
| `ApolloServicePass` (`mixer.c:265-268`) | Calls the position update and zero-ahead only when the engine is not running (fallback if `ExAllocateTimer` failed): the engine owns both while it runs. |
| GET_POSITION, READ_CAPTURE, GET_LEVELS, WRITE_PLAYBACK (`ioctl.c:189-226`, `:263-271`; `audio.c:420`, `:459`, `:502`) | Read the engine snapshot under EngineLock instead of calling `ApolloPositionUpdate`; never mutate it. GET_POSITION returns the snapshot's `EngRaw`/`HwPos`/`HwQpc` (no second SAMPLE_POS reader) and still reads FRAME_CTR / AX_CONTROL fresh. When the transport is stopped the engine is stopped too and these IOCTLs behave as today. |
| WRITE_PLAYBACK | Not refused (deviation: with ADD semantics there is no slot owner). It sets `PlayValidUntil` (interlocked) **before** copying, so the zero horizon skips those frames; engine clients still add on top when the frames reach the data window. Diagnostic data and a client on the same slots therefore mix. |
| ISR/DPC (`hw.c:330-372`) | Unchanged; the end-of-buffer count is a wrap witness for diagnostics only. |
| Stream cleanup | EvtCleanupCallback: if still active, remove under lock and wait for rundown; if packets are still allocated, free them. (Step 2.) |
| New IOCTL_APOLLO_GET_ENGINE_STATS (**0x0F**, APOLLO_USER; 0x0E is GET_STRIPS) | Tick timing, late ticks, stalls, ASIO client counters. `APOLLO_IOCTL_VERSION` was 4 with it. `apollo-diag engine [seconds] [--reset]` prints it. |
| New IOCTL_APOLLO_GET_ENDPOINTS (**0x10**, APOLLO_USER) | One row per endpoint-table entry: enabled/added/stream/prepared/running flags, slots, packet layout, packets, frames, underruns, overruns, dropped frames, late packets, notify failures. `APOLLO_IOCTL_VERSION` is **5**. `apollo-diag endpoints [seconds]` prints it and a last line `ACX streams: prepared N, running M` that install.ps1/revert.ps1 parse. |

New files: `engine.c` (§5, non-paged), `asio.c` (docs/ASIO.md), `apollo_acx.h` (§7), and with steps
2-3 `endpoints.c` (table), `circuit.c` (§3.2, name callback, add/remove), `stream.c` (§4, engine
hooks), `apollo_stream.h` (their types).

---

## 6. INF changes (Q6)

| Section | Directive | Value / notes |
|---|---|---|
| `[Version]` | unchanged | Class MEDIA and its GUID are already right (`apollo.inf:12-13`). |
| `[Apollo_Install.NT.Wdf]` | unchanged | `KmdfService`, `KmdfLibraryVersion = $KMDFVERSION$` (`apollo.inf:66-70`). There is **no ACX directive**: ACX is a WDF class extension bound at load through acxstub; `Acx01000` is an in-box demand-start service in WdfLoadGroup (checked in the registry). The sample INF has none either (`AudioCodec.inf:63-66`). |
| `[Apollo_Install.NT.Interfaces]` (new) | per render circuit: `AddInterface = <KSCATEGORY_AUDIO>, <ref>, <sect>`; `… <KSCATEGORY_RENDER> …`; `… <KSCATEGORY_REALTIME> …`. Per capture circuit: the same with `<KSCATEGORY_CAPTURE>` | `<ref>` = circuit name from §3.1, exactly. GUIDs: AUDIO {6994AD04-93EF-11D0-A3CC-00A0C9223196} (`ksmedia.h:594`), RENDER {65E8773E-8F56-11D0-A3B9-00A0C9223196} (`ks.h:819`), CAPTURE {65E8773D-8F56-11D0-A3B9-00A0C9223196} (`ks.h:794`), REALTIME {EB115FFC-10C8-4964-831D-6DCB02E6F23F} (`ksmedia.h:606`). Same pattern as `AudioCodec.inf:95-104`. |
| interface AddReg (one section can serve all) | `HKR,,CLSID,,{17CCA71B-ECD7-11D0-B908-00A0C9223196}` | KS proxy CLSID (`ksproxy.h:713`) |
| | `HKR,,FriendlyName,,%Apollo.EndpointName%` | `"Apollo x8p"`: the parenthetical in Sound settings |
| | `HKR,EP\0,"{1DA5D803-D492-4EDD-8C23-E0C0FFEE7F0E},2",,"{00000000-0000-0000-0000-000000000000}"` | PKEY_AudioEndpoint_Association = KSNODETYPE_ANY (`mmdeviceapi.h:164`) |
| | `HKR,EP\0,"{1DA5D803-D492-4EDD-8C23-E0C0FFEE7F0E},7",0x00010001,1` | PKEY_AudioEndpoint_Supports_EventDriven_Mode (`mmdeviceapi.h:171`): selects 2-packet streaming |
| `[Apollo_HW_AddReg]` | `HKR,,Security,,"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;LS)(A;;GR;;;IU)S:(ML;;NRNWNX;;;ME)"` | §2.3 item 3; replaces `apollo.inf:53`. The `S:` part (review fix F8) is a Medium mandatory label with no-read/write/execute-up: Low-integrity and AppContainer processes cannot open the device; users (Medium), elevated admins (High) and services (System) can. Round-trips through `ConvertStringSecurityDescriptorToSecurityDescriptorW`; whether PnP applies the SACL part is checked on hardware (§10.6). |
| `[Apollo_MonIn_Interface]` (implemented) | `HKR,EP\0,"{F3E80BEF-1723-4FF2-BCC4-7F83DC5E46D4},3",0x00010001,0x305` | PKEY_AudioDevice_NeverSetAsDefaultEndpoint, all flows and roles: Windows never makes `MON L/R` recording a default. Not on `MON L/R` playback (§9.4). |
| | keep | MSI lines, `DmaRemappingCompatible`, `DeviceCharacteristics 0x100` (`apollo.inf:44-54`) |
| | (not added) | Deviation: the engine tunables `RenderLeadFrames` (144), `CaptureGuardFrames` (16) and `StartTransportAtConnect` (1) are read from the service key `HKLM\SYSTEM\CurrentControlSet\Services\apollo\Parameters` with `WdfDriverOpenParametersRegistryKey`, like the strip settings, and have code defaults, so the INF carries none. |
| `[Strings]` | add the category GUIDs, proxy CLSID, PKEY strings, `Apollo.EndpointName` | |

Not needed: `Include=ks.inf` / `Needs=KS.Registration` (the ACX sample has neither) and
MediaCategories entries (pin names come from `EvtAcxPinRetrieveName`). `infverif /w` must stay
clean; generate the AddInterface block from the endpoint table (§3.1).

## 7. Build changes (Q7), all verified **[BUILD]**

| Item | Change |
|---|---|
| vcxproj `PropertyGroup Label="Configuration"` | `<ACX_VERSION_MAJOR>1</ACX_VERSION_MAJOR>`, `<ACX_VERSION_MINOR>1</ACX_VERSION_MINOR>`. Plain user macros: the WDK msbuild files have no ACX support (searched `build\10.0.28000.0`). KMDF stays 1.33. |
| `ClCompile/AdditionalIncludeDirectories` | prepend `$(DDK_INC_PATH)\acx\km\$(ACX_VERSION_MAJOR).$(ACX_VERSION_MINOR)` (as `sample AudioCodec.vcxproj:110`) |
| `ClCompile/PreprocessorDefinitions` | add `ACX_VERSION_MAJOR=$(ACX_VERSION_MAJOR);ACX_VERSION_MINOR=$(ACX_VERSION_MINOR)`. Required: `acxfuncenum.h:111-122` `#error`s without them. Leave `ACX_MINIMUM_VERSION_REQUIRED` undefined (defaults to the minor, `:127-129`; defining it lower switches structure sizes to runtime lookups, `:198-214`). |
| `Link/AdditionalDependencies` | add `$(DDK_LIB_PATH)acx\km\$(ACX_VERSION_MAJOR).$(ACX_VERSION_MINOR)\acxstub.lib` and `$(DDK_LIB_PATH)ksguid.lib` (KS GUIDs such as KSCATEGORY_AUDIO, KSNODETYPE_*, KSDATAFORMAT_* are unresolved in C without it, even with initguid.h). Keep `wdmsec.lib`. |
| `<ntifs.h>` | Deviation: included only by `ioctl.c` (`SeTokenIsAdmin`, `SeQuerySubjectContextToken`) and `asio.c` (`KeStackAttachProcess`, `PsGetProcessExitStatus`), ahead of `apollo.h`; `apollo.h` keeps `<ntddk.h>`. `<ntstrsafe.h>` comes with the pin-name callback in step 2. |
| `apollo_acx.h` (new) | after `apollo.h`: `<windef.h>`, `<mmsystem.h>`, `<ks.h>`, `<ksmedia.h>`, `<acx.h>`, in that order. `ks.h` fails to compile in C without `windef.h` (CDECL errors). ACX headers compile as C at /W4 /WX. |
| Our GUIDs | component IDs with `DEFINE_GUID` in `endpoints.c` (`apollo_ioctl.h:26` already includes `initguid.h`; its GUIDs are selectany, so no duplicates). |
| Code Analysis | Pass real attributes to `AcxCircuitCreate` / `AcxRtStreamCreate` (C6387 otherwise); suppress C6001 once in the pin-name callback (§3.3); check or deliberately discard the status of `AcxRtStreamNotifyPacketComplete`. |
| `build.cmd`, signing, inf2cat | unchanged |

---

## 8. Risks, unknowns, step order (Q8)

### 8.1 Risks and unknowns

| # | Risk | Mitigation / check |
|---|---|---|
| R1 | Q2 rests on disassembly of one ACX build (10.0.26100.9974). A future build could route differently. | Step 0 proves it on hardware; re-run `apollo-diag info` after OS updates; fallback §2.4 A. |
| R2 | LocalService access assumed sufficient (and needed) for audiosrv/audiodg opens. | Step 2: endpoint opens and plays. If opens fail, check with Process Monitor; add the failing account. |
| R3 | Element-less circuits might be rejected by the endpoint builder. | Step 2 shows it at once; fallback §3.4 (volume+mute). |
| R4 | Timer jitter versus a 144-frame lead. | Engine stats (max tick interval) in step 1; raise RenderLeadFrames. |
| R5 | MMIO read at DISPATCH on a dying Thunderbolt link (P7). | One read per tick, gate check first, all-ones stops the engine; Driver Verifier with DPC checks. |
| R6 | Transport start hazard X2 (Linux AER on Maple Ridge at connect). | Start at connect ran clean twice on this laptop's CPU-integrated Thunderbolt; `StartTransportAtConnect` = 0 is the switch for a machine that shows X2 (lazy start planned, §5.6). Never at probe. |
| R7 | Windows may make the new `MON L/R` render endpoint the default output: system audio would leave the user's monitors (they are on another interface). | Found: `PKEY_AudioDevice_NeverSetAsDefaultEndpoint` in the INF stops it, but also forbids choosing the endpoint by hand; used on `MON L/R` recording only. For playback, `install.ps1` saves the defaults and puts them back (§9.4). |
| R8 | ACX vetoes query-remove/stop with streams in RUN, blocking driver updates and the cable-pull flow. | `install.ps1`/`revert.ps1` check processes, `apollo-diag endpoints`/`engine` and Apollo audio sessions first, wait up to 60 s, then ask (Enter / restart audiosrv / quit); surprise removal is unaffected. |
| R9 | Mic endpoints carry digital zero until input strips exist. | Expected; loopback tests use `ApolloMonIn`. |
| R10 | REC_CH 32 versus the 34-slot descriptor; talkback needs 34. | Keep 32 for v1 (proven); step 6 changes it with GET_LEVELS checks. Changing stride requires zeroing the ring (D7) and the engine stopped. |
| R11 | Exact meaning of SAMPLE_POS (fetch point or converter) is unknown (spec-streaming §8), so presentation position may be off by the FIFO. | Affects A/V sync by < 1 ms; ignore in v1. |
| R12 | 69 endpoints at scale: enumeration time and Sound settings clutter. | Enable masks (§3.5). |
| R13 | Timer-driven (1 packet) path is less exercised than event-driven. | Test with an exclusive-mode WASAPI tool in step 4. |

### 8.2 Step order (each step gates the next)

| Step | Build | Test on hardware |
|---|---|---|
| 0 | ACX init only (§1), file-create admin gate and SDDL (§2.3), **no circuits**. vcxproj/INF changes. | Driver loads (Acx01000 binds), bring-up reaches Connected; `apollo-diag info`, `levels`, `log` from a normal prompt; `start`/`tone` elevated; control app works; elevated-only IOCTLs fail from a normal prompt. **This proves Q2.** |
| 1 | Copy engine + timer, tied to transport start/stop; service thread and IOCTLs switched to the snapshot (§5.7); GET_ENGINE_STATS. | `start`, `pos 30`: rate 48 kHz, no backwards steps; stats: tick interval max, no late ticks; `tone 5` still audible on the front-panel meters and on record slots 26-27 via `levels`. |
| 2 | Render circuit `ApolloMon` (§3, §4). | Endpoint `MON L/R (Apollo x8p)` appears. Play a 1 kHz −20 dBFS WAV from a WASAPI shared-mode player: `apollo-diag levels` shows slots 26-27 at ≈ 0x7FFFFF / 10 (−20 dBFS) and nothing else; front-panel meters move. Sound settings test tone works. Stop/start playback 50 times: no clicks of old audio (D8), transport never restarts (log). |
| 3 | Capture circuit `ApolloMonIn`. | Record it while playing into `ApolloMon`: the recording is the played file. This digital loop is the glitch rig: 1 kHz sine for 30 min, analyse for discontinuities (PLAN phase 2 gate), under Driver Verifier. |
| 4 | Remaining capture circuits (`ApolloIn1..8`, pairs). | All endpoints open and stream (silence until strips). Two apps on `ApolloIn1` and `ApolloIn12` at once (fan-out). Exclusive-mode test for the 1-packet path. |
| 5 | Soak and apps. | 30-minute Zoom and Discord calls (output to `MON L/R`, input any MIC/LINE once strips exist, else `MON L/R` capture); sleep/resume with a stream open; cable pull during playback: no bugcheck. |
| 6 | AX_REC_CH 34, talkback rows; then the full table with enable masks (§3.5). | GET_LEVELS shows talkback on 32-33 when talkback is on; existing endpoints unchanged. |

---

## 9. Implementation status (Nov 14 2026)

Built clean in the live tree with `tools\build-all.cmd` (Nov 14 13:17, after steps 2-3, review
fixes F1-F8 and G1-G8, and the strips agent's latest `strips.c`/`mixer.c`/`strip_plan.h`;
**DriverVer 11/14/2026,13.16.57.164**): `driver\build.cmd Release` at /W4 /WX with Code Analysis
(all fourteen `x64\Release\*.nativecodeanalysis.xml` are 59 bytes), `infverif /w` passes,
`apollo.sys` imports `WdfVersionBindClass` (the ACX bind) next to `WdfVersionBind`; apollo-diag,
loopback-test, wasapi-loop, avium-asio.dll, asio-test.exe, dsp (45 checks pass) and the control app
build, and `tools\bin\tools-manifest.txt` (SHA-256 of the tools, G8) is written.
`wasapi-loop --list` and the read-only parts of `tools\phase1\audio-helpers.ps1` were run on this PC
(no Apollo endpoints yet: the x8p is on the earlier build). **Nothing has run on the hardware.**

| Step | Done | Where |
|---|---|---|
| 0 | ACX 1.1: `AcxDriverInitialize` after `WdfDriverCreate`, `AcxDeviceInitInitialize` first in DeviceAdd, `AcxDeviceInitialize` right after `WdfDeviceCreate`; **no circuits**. Client file object config (CanUseFsContext, scope None) with `ApolloEvtDeviceFileCreate` recording `SeTokenIsAdmin` per handle; every IOCTL whose code carries FILE_WRITE_ACCESS also needs that flag. INF SDDL adds `(A;;GRGW;;;LS)`. vcxproj: ACX include path, defines, `acxstub.lib`, `ksguid.lib`. Private interface (NULL reference string), default sequential queue and buffered I/O unchanged. | `driver.c`, `ioctl.c`, `apollo_acx.h`, `apollo.inf`, `apollo.vcxproj` |
| 1 | Copy engine: `ExAllocateTimer(EX_TIMER_HIGH_RESOLUTION)`, 1 ms periodic, EngineLock spin lock, one SAMPLE_POS read per tick, 64-bit snapshot with QPC wrap repair, zero horizon 4096 frames ahead (skips diag-claimed frames), render ADD window to SAMPLE_POS + 144, capture to SAMPLE_POS − 16, stats. Starts/stops with the transport; stopped in D0ExitPreInterruptsDisabled (always), ReleaseHardware and device cleanup. GET_POSITION / READ_CAPTURE / WRITE_PLAYBACK / GET_LEVELS use the snapshot; the 10 ms service pass keeps the old position and zero-ahead code only as a fallback when no timer runs. IOCTL_APOLLO_GET_ENGINE_STATS (0x0F) and `apollo-diag engine`. | `engine.c`, `audio.c`, `mixer.c`, `ioctl.c`, `device.c`, `apollo_ioctl.h` (version 4), `tools\apollo-diag` |
| ASIO kernel half | IOCTLs 0x20-0x23 handled in `EvtIoInCallerContext` (all other requests are re-queued unchanged), only from the process that opened the handle. One client. Shared block = a pagefile-backed section: pages locked by an MDL and mapped non-pageable for the engine, one `ZwMapViewOfSection` view (read/write, not inherited) in the caller (review fix F1, §9.3); event referenced with `UserMode` access checks. Engine fills one half while the client owns the other; publishes HalfReady / SwitchCount / SamplePosition / Qpc under the seqlock and sets the event; render plays half h only if `ClientDone[h]` equals the switch that published it, else silence and `Underruns`++; `Overruns` when a capture half is overwritten before that (F3). `InputLatencyFrames` = guard + 10 (26), `OutputLatencyFrames` = lead + FRAME_CFG (152), `SampleRate` 48000. Released at ASIO_CLOSE, EvtFileCleanup, EvtFileClose (backstop) and device cleanup, from any process. | `asio.c`, `apollo.h`, `include\apollo_asio.h` (version 2), `asio\avium-asio.cpp` |
| 2 | Render circuit `ApolloMon` = Windows endpoint **MON L/R (Apollo x8p)**, playback slots 0-1 (resolved from the descriptor by channel ID 0x0901/0x0902, fallback 0/1). Circuits are created in EvtDeviceAdd for the rows enabled in `EnabledEndpoints` (default 0x3 = `ApolloMon` + `ApolloMonIn`), added in PrepareHardware after `Stage = Mapped`, removed in ReleaseHardware; a failing circuit costs only its endpoint (logged `acx: circuit …`). Two pins (host sink, bridge source `KSNODETYPE_SPEAKER` named by `EvtAcxPinRetrieveName`), one format (48 kHz, 32/24, stereo), raw list + default, no elements. RT stream: AllocateRtPackets (1 or 2 packets, one page-rounded non-paged buffer, packet 1 = Invalid descriptor at offset PacketSize), Prepare/Release/Run/Pause, GetHwLatency, GetCurrentPacket, GetPresentationPosition (snapshot, clamped, monotonic), SetRenderPacket (§4.4), cleanup with rundown. Tick: resync at Run, drop after a late tick, per-packet underrun decision for SetRenderPacket clients, EOS, saturating ADD into the ring, packet completion queued and notified after EngineLock. | `endpoints.c`, `circuit.c`, `stream.c`, `apollo_stream.h`; hooks marked `[ACX endpoints]` in `driver.c`, `device.c`, `engine.c`, `ioctl.c`, `apollo.h`; `apollo.inf`, `apollo.vcxproj` |
| 3 | Capture circuit `ApolloMonIn` = **MON L/R (Apollo x8p)** in the recording list, record slots 26-27 (channel IDs 0x0901/0x0902), bridge `KSNODETYPE_LINE_CONNECTOR`, `NeverSetAsDefaultEndpoint`. Tick: copy up to SAMPLE_POS − 16, packet start QPC per packet, overrun when a packet replaces one the OS never fetched, resync after a stall over 4096 frames. GetCapturePacket returns the last completed packet and its QPC. Mic circuits (`ApolloIn1..8`, pairs) are not built (step 4). | same files |
| 4 (mono part, build only) | Capture circuits `ApolloIn1..8` = **MIC/LINE 1 .. MIC/LINE 8**, mono (48 kHz, 32/24, `KSAUDIO_SPEAKER_MONO`, block align 4), record slots 0-7 resolved by channel IDs 0x0001, 0x0002, 0x0103, 0x0104, 0x0305-0x0308 (fallback 0-7), bridge `KSNODETYPE_MICROPHONE` (§3.1 GUIDs), no NeverSetAsDefault. Rows 2-9 of the table; `APOLLO_EP_DEFAULT_MASK` 0x3FF. GET_ENDPOINTS layout unchanged (10 of 16 rows). A machine whose `EnabledEndpoints` is already 3 keeps only the MON rows until it is set to 0x3FF (1023) or deleted. Stereo pairs not built. `wasapi-loop --meter TEXT` prints one recording endpoint's peak every 250 ms; `--exclusive` now opens mono endpoints with 1 channel. Not run on hardware. | `endpoints.c`, `stream.c`, `apollo_stream.h`, `apollo.inf`, `tools\wasapi-loop` |
| Tools | `apollo-diag endpoints [seconds]` (GET_ENDPOINTS 0x10, interface version 5). `tools\wasapi-loop` (new): plays a 997 Hz −12 dBFS sine into MON L/R through WASAPI and checks every sample that comes back on MON L/R recording against the sine recurrence; `--tone N` plays only; `--list`; `--exclusive` for the 2-packet exclusive path; prints driver counters every 10 s. `tools\phase1\audio-helpers.ps1` (new) + `install.ps1`/`revert.ps1`: in-use check before PnP, default-device save/restore. | `tools\` |

Timing of the ASIO client (frames, N = buffer size): input frame `a` is the hardware frame
`Origin + a`; period `P` is published once `Origin + (P+1)N ≤ SAMPLE_POS − 16`; output frame `a`
plays at `Origin + a + 2N + 16 + 144`, so the host has exactly one period after `bufferSwitch(P)`
before the engine needs that half. Round trip ≈ 2N + 178 frames (2N + 3.7 ms), what getLatencies
reports. A re-origination (engine restart, a capture lag over 4096 frames) keeps SamplePosition
monotonic and counts `Resyncs`.

### 9.1 Deviations from sections 1-8

| # | Deviation | Why |
|---|---|---|
| V1 | Transport and engine start at connect by default (§5.6); the lazy start is not built. | Coordinator override: it ran clean twice on this laptop, and ASIO needs the clock running before a host opens. `StartTransportAtConnect` = 0 restores a manual start. |
| V2 | Render clients add (saturating) instead of writing their slots; WRITE_PLAYBACK is no longer refused while a render client runs. | ASIO and Windows streams may share MON L/R (ASIO.md). |
| V3 | Engine stats IOCTL is 0x0F, interface version 4. | 0x0E went to GET_STRIPS. |
| V4 | Engine tunables live in the service `Parameters` key, not in the INF. | Same place as the strip settings; code defaults. |
| V5 | `ntifs.h` only in `ioctl.c` and `asio.c`. | Smallest change to the shared header. |
| V6 | An extra lock, `AsioLock` (wait lock), serialises ASIO open/start/stop/close/cleanup; order AsioLock → EngineLock, never with DevLock or StripLock. | ASIO requests run in caller context, outside the sequential queue. |
| V7 | TRANSPORT_STOP is refused while an ASIO client is open. | Stopping under a host only produces a reset request. |
| V8 | `build-all.cmd` now also builds `tools\loopback-test` (and `tools\wasapi-loop`). | It was missing. |
| V9 | One registry bitmap, `EnabledEndpoints` (REG_DWORD, bit i = row i of the `endpoints.c` table, default 0x3), instead of `EnabledCapture` / `EnabledRender` (§3.5). | The table has two rows today; one value is enough until the full table lands. |
| V10 | Endpoint state lives in a WDF context `APOLLO_ACX` on the device (rows, the active-stream list, the prepared count), not in `DEVICE_CONTEXT` fields (§5.2); `DEVICE_CONTEXT` only gains a pointer to it. | Keeps the hooks in the reviewed files to a few marked lines. |
| V11 | (withdrawn by fix G3) The first build notified once per stream per tick; now every completed packet is notified, in order, as §5.4 says, at most 16 per tick (the rest on the next tick). | |
| V12 | A capture stall over 4096 frames resyncs the stream to now (counted as an overrun) instead of writing zeros to keep time (§5.4 step 6). | Same rule as ASIO; the position stays monotonic. |
| V13 | Both endpoints' pin name is `MON L/R` (§3.1 already), so Windows shows **MON L/R (Apollo x8p)** in both the playback and the recording list. | The descriptor's name for both slot pairs. |
| V14 | `PKEY_AudioDevice_NeverSetAsDefaultEndpoint` on `MON L/R` recording (INF); playback relies on `install.ps1` restoring the defaults (§9.4). | R7. |
| V15 | Engine gives up on a silent device (`EngLost`, review fix F2): after one slow (> 1 ms) all-ones read, two slow reads in a row, or 8 all-ones reads in a row; the reviewer suggested any single slow read. | A single slow read with valid data is what an SMI or a long ISR on the timer's CPU looks like; giving up then would silence a healthy unit until a restart. |
| V16 | WRITE_PLAYBACK window is now `[pos + RenderLeadFrames + 48, pos + 4032)` (192..4032 by default) and at most 3072 frames per call (review fixes F5, G5); refused while the engine is lost. loopback-test and the apollo-diag tone writer keep 256 frames of safety. | Writes only into frames the zero horizon has passed and the engine's clients have not yet added into; one tick of margin for the snapshot's age. |
| V17 | Found on hardware Nov 14: the first engine builds' saturating add (`AsioSatAdd`, `StreamSatAdd`) compared with `MINLONG`, which `ntdef.h` defines as `0x80000000`, an unsigned int. The comparison was therefore against +2^31 and always true, so every rendered sample became `0x80000000`: full-scale negative DC on the client's outputs. ASIO showed MON loopback stuck at -FS whatever it played. Both now use `ApolloSatAdd32` (`driver\apollo_sat.h`, explicit signed limits), checked by `tools\sat-test`, which runs in `build-all.cmd` and fails 7 of 12 on the old code. `asio-test --run` now fails (exit 3) unless the MON loopback matches the played level. | Never compare signed values with `MINLONG`/`MINSHORT`/`MINCHAR`; /W4 and Code Analysis do not flag it. |

### 9.3 Review fixes F1-F8 (independent review of steps 0/1/ASIO, Nov 14)

| # | Finding | Fix |
|---|---|---|
| F1 | `AsioRelease` trusted `PsGetProcessExitStatus == STATUS_PENDING` as "owner alive" and unmapped by attaching (bugcheck if the owner exited with code 259, or a view of freed pages if cleanup ran during the owner's teardown). | The shared block is a pagefile-backed section (`ZwCreateSection`), locked by an MDL and mapped non-pageable for the engine (`MmMapViewInSystemSpace` + `MmProbeAndLockPages` + `MmGetSystemAddressForMdlSafe`); the client gets a `ZwMapViewOfSection` view. The view holds its own section reference, so pages are never freed under a live view, and unmapping goes through `ZwUnmapViewOfSection` on a kernel process handle (`ObOpenObjectByPointer`), which fails harmlessly if the view or the process is gone. No attach, no exit-status test, no process-notify routine. `APOLLO_FILE.Creator` (referenced, from `IoGetRequestorProcess` at create) must equal the caller for every ASIO IOCTL, so inherited or duplicated handles cannot map or drive ASIO. |
| F2 | Every tick read MMIO at DISPATCH with no back-off: a device that stops answering without a surprise removal could stall the timer DPC repeatedly (0x133). | `EngLost`: set by the tick (V15), which then cancels its own periodic timer and never touches MMIO again; GET_POSITION stops reading FRAME_CTR/AX_CONTROL too. Cleared only by a PASSIVE engine restart (transport stop/start, D0 exit/entry, PnP). Exposed as `APOLLO_FLAG_ENGINE_LOST` (GET_INFO), `EngineLost` (GET_ENGINE_STATS, `apollo-diag engine` shows `LOST`), and the ASIO DLL asks the host to reset. |
| F3 | ASIO `ClientDone` was a high-water mark: a skipped period could play stale output from two periods back, a skipped capture was lost uncounted, and `outputReady()` stored the live SwitchCount. | `ClientDone[2]` per half, `APOLLO_ASIO_VERSION` 2: the DLL stores the switch it processed in `ClientDone[half]` (also from `outputReady()`, which now uses the switch being processed); the engine plays half h only on an exact match and counts an overrun when it overwrites a half without one. The DLL counts skipped switches in `ClientSkipped`, shown by `apollo-diag engine` and `asio-test --run`. DLL driver version 2. |
| F4 | An overlapped ASIO_OPEN racing the last close could succeed after cleanup and stay open until device removal. | Cleanup sets `APOLLO_FILE.CleanedUp` under AsioLock and ASIO_OPEN refuses such a handle (STATUS_FILE_CLOSED); `ApolloEvtFileClose` releases a client still owned by the closing file (backstop) and drops the creator reference. |
| F5 | WRITE_PLAYBACK accepted writes up to a ring ahead while the zero horizon reached only 4096 frames: a race with the tick and stale ring audio in the gap. | V16. |
| F6 | `apollo-diag pos N` ended on the driver's QPC, which freezes on dead reads: it could loop forever. | The tool ends on its own clock. `APOLLO_POSITION.Reserved` became `Flags` (`APOLLO_POS_FLAG_DEAD_READ`, `APOLLO_POS_FLAG_ENGINE_LOST`), printed by `pos`. |
| F7 | `ApolloGetFile(fo)` was used without a NULL check. | Every user checks for NULL; no context = not admin, no ASIO. |
| F8 | Low-integrity processes could open the device and use the user IOCTLs, ASIO included (every input, past mic privacy). | Medium mandatory label in the INF SDDL (§6). |

### 9.3b Review fixes G1-G8 (third independent review, combined build 12.36.20.8)

The review found no bugcheck in the normal paths. Fixed in the live tree:

| # | Finding | Fix |
|---|---|---|
| G1 | `StreamCleanup` freed the packet MDL and buffer when FreeRtPackets had not run, though ACX may still hold that MDL and have the buffer mapped into audiodg; its DISPATCH branch skipped the rundown wait. | Cleanup only takes the stream off the engine (waiting for a notification in flight) and never frees packet memory; only FreeRtPackets does (as the ACX sample). If the buffer is still there it logs `deleted with its packets still allocated` and leaks it. Cleanup asserts PASSIVE (ACX deletes streams at PASSIVE); the unexpected non-PASSIVE path still unlinks the stream. |
| G2 | A lost engine (F2) left the transport running with no zeroing, so on a false positive the x8p looped its last 170 ms; and nothing could recover it (STOP refused with streams/ASIO open, START a no-op while running, EngineStart a no-op while the dead timer existed). | On lost the tick zeroes the whole playback ring once (host memory, no MMIO). TRANSPORT_START on a lost engine now calls `ApolloEngineRecover` (DevLock): delete the dead timer, read SAMPLE_POS and AX_CONTROL once at PASSIVE; if the device answers and the transport still runs, continue the 64-bit position from the last good tick and restart the engine (clients resync); if the transport stopped, restart the transport; if it still does not answer, stay lost and return STATUS_DEVICE_NOT_CONNECTED. Works with streams and ASIO open. |
| G3 | One notification per tick and a GetCapturePacket that offered only the newest packet: a second capture packet completed in the same tick was never offered, and on render one event for two used packets caused an underrun. | Every completed packet is notified, in order (`Notified` counter, up to 16 per tick). GetCapturePacket hands out packets oldest-unread first (`ReadNext`) with that packet's own start QPC and `MoreData` while more are waiting; if the OS fell more than the buffer behind it skips to the oldest packet still intact. V11 withdrawn. |
| G4 | The capture overrun count assumed the OS calls GetCapturePacket; in timer mode or with position-based reads every packet counted. | Overruns are counted only after the first GetCapturePacket call of the stream. |
| G5 | WRITE_PLAYBACK accepted from pos + 88 while the engine's clients add up to pos + 144: diagnostic writes overwrote ACX/ASIO data. | Window starts at pos + RenderLeadFrames + 48 (V16); tools keep 256 frames of safety. |
| G6 | `AcxCircuitInitFree(init)` after a failed `AcxCircuitCreate`, which may have consumed it. | Freed only if `init` is still non-NULL. |
| G7 | A new stream was refused while `ep->Stream` was still set (e.g. before the old stream's cleanup). | Not refused: MaxStreams = 1 on the host pin is the real limit; the new stream takes over the statistics row, and cleanup clears it only if it is still its own. |
| G8 | The elevated scripts ran devcon.exe and apollo-diag.exe from the user profile and compiled the audio helpers into the user's %TEMP%, so a normal-integrity process running as the user could swap them and gain admin rights (relevant: the Nov 9 malware, offline scan still open). install.ps1 also killed every "Universal Audio" process, which includes the Twin X's software, on every run; revert.ps1 matched packages by file name only. | New `tools\phase1\safe-tools.ps1`: before anything runs elevated, `%ProgramFiles%\Avium\ApolloTools` is made admin-only (owner Administrators, protected DACL SYSTEM + Administrators full control; users cannot create anything under Program Files, so nobody else can have prepared it), the needed binaries are copied there, and each copy's SHA-256 must match `tools\bin\tools-manifest.txt`, which `tools\build-all.cmd` now writes (`tools\write-manifest.ps1`). Only the verified copies run. TEMP/TMP of the elevated session point at an admin-only `tmp` inside it before Add-Type; `audio-helpers.ps1` refuses to compile elevated otherwise. pnputil and verifier run by full System32 path. UA processes are closed only when the x8p is still on UA's driver (first install). Packages are ours only if Original Name is `apollo.inf` **and** Provider is `Avium` (install and revert). revert.ps1 still runs if the tool check fails (without apollo-diag's in-use check), because it is the way back. |

Residual risk (G8): the `.ps1` scripts and the manifest themselves still live in the user profile, so a
process running as the user could edit them before the user starts them elevated. The verified copies
close the swap-while-running window, not that one. **TODO:** the planned single-exe installer installs
the driver package, tools and scripts under `%ProgramFiles%\Avium` (admin-only writable) and runs only
from there; signing with a real certificate also removes the dependency on the user's test-signing
certificate store.

### 9.4 Default output device (R7, the user's monitors are on the Twin X)

- **INF knob found:** `PKEY_AudioDevice_NeverSetAsDefaultEndpoint` (`{F3E80BEF-1723-4FF2-BCC4-7F83DC5E46D4},3`,
  REG_DWORD mask: 0x1 console, 0x2 multimedia, 0x4 communications, 0x100 render, 0x200 capture)
  under the interface's `EP\0` key. Windows then never makes the endpoint a default, **and the user
  cannot choose it as default by hand either** (the Sound settings option is greyed out). It is
  reliable but heavy-handed, so it is set (0x305, everything) only on `MON L/R` recording, which is
  a loopback nobody wants as their microphone.
- **`MON L/R` playback stays selectable.** Windows may make it the default output when it first
  appears. `install.ps1` reads all six defaults (playback/recording × console/multimedia/
  communications) before the install and, after bring-up (twice, 4 s apart), puts back any that
  moved, through `IPolicyConfig::SetDefaultEndpoint` (undocumented, what Sound settings itself
  uses, stable Windows 7 to 11). `revert.ps1` does the same after removing the driver.
- **On this PC today** the defaults are: playback console/multimedia = *Speakers (Realtek(R)
  Audio)*, playback communications = *Speakers (Universal Audio Thunderbolt WDM)*; recording =
  *AI Noise-cancelling Input (ASUS Utility)* / *Microphone Array (Realtek(R) Audio)*.
- **Manual fix** if the script cannot (or after a later re-plug): `Win+R`, `mmsys.cpl`, Playback tab,
  right-click the speakers you use (e.g. *Speakers (Universal Audio Thunderbolt WDM)* for the Twin X),
  **Set as Default Device**, and again **Set as Default Communication Device**. To forbid MON L/R
  playback as default for good, add `Apollo_NeverDefault_AddReg` to `[Apollo_Mon_Interface]` in
  `apollo.inf` (comment there) and reinstall.

### 9.5 Risks to know before installing

- **ACX on real hardware is unproven.** If `AcxDriverInitialize` / `AcxDeviceInitInitialize`
  fail, the driver does not load (Device Manager code 10 or 31) and `apollo-diag` cannot connect.
  Recovery is `tools\phase1\revert.ps1` or installing the frozen
  `builds\2026-11-14-t3-strip-uploads\package` with `install.ps1 -Package`.
- **ExDeleteTimer waits under DevLock** in transport stop and D0 exit. The tick never takes DevLock,
  so it cannot deadlock, but a tick stuck in one MMIO read on a dying link delays power-down by that
  read.
- **The engine is always on** while the transport runs (a 1 ms DISPATCH timer, ~1000 wakeups/s).
  Fine for a plugged-in studio laptop; worth knowing on battery.
- **ASIO at 64 frames** has 1.33 ms periods against a 1 ms tick: two periods can complete in one
  late tick, and the host then loses one (an underrun plus an overrun). 128 and up have two or more
  ticks per period. The hardware period-timer interrupt is the later fix (ASIO.md).
- **Handles held across install:** an open ASIO host or loopback-test keeps a handle, and a
  Windows audio stream in RUN makes ACX veto the stop, so PnP refuses the driver update (reboot
  prompt, problem code 14/38). `install.ps1`/`revert.ps1` now check and wait first; a reboot always
  finishes the update. Apollo Control releases its handle on query-remove.
- **Endpoints on real hardware are unproven.** Element-less circuits (R3), the `RetrieveName`
  callback and the 2-packet layout match the ACX sample's shapes but have not met the Windows
  endpoint builder. If `MON L/R` does not appear: `apollo-diag log` (`acx: circuit …`, `acx: add …`),
  `apollo-diag endpoints`; the private interface and ASIO keep working without circuits, and
  `EnabledEndpoints` = 0 turns them off (`reg add HKLM\SYSTEM\CurrentControlSet\Services\apollo\Parameters /v EnabledEndpoints /t REG_DWORD /d 0`, then disable/enable the x8p).
- **Re-adding circuits after a rebalance** (ReleaseHardware then PrepareHardware without
  DeviceAdd) re-adds the same circuit objects; `AcxDeviceRemoveCircuit` does not delete them
  (disassembly), but this path has not run.
- **`IPolicyConfig` is undocumented.** If a Windows update breaks it, `install.ps1` prints the
  manual fix (§9.4) instead of restoring.
- **Mandatory label (F8):** if PnP ignored the `S:` part, low-IL processes keep read access (no
  worse than before); if it applied it differently than expected, normal users could lose access,
  which test 0.4 shows at once. Recovery: revert.
- **ASIO version bump:** `apollo_asio.h` version 2. An old `avium-asio.dll` against the new driver
  (or the reverse) is refused at createBuffers ("does not match this DLL"); rebuild and re-register
  together (build-all does both builds; registration path unchanged).

## 10. Hardware test procedure (steps 0-3, ASIO, review fixes)

One install of the current build covers all of it; run the tables in order. Before each install:
close Ableton, `asio-test`, `loopback-test`, `wasapi-loop` and any `apollo-diag pos/levels` loop
(`install.ps1` now checks for them, and for Windows audio on the Apollo endpoints, and waits). Driver Verifier stays on (`verifier /standard /driver apollo.sys`, includes DDI compliance).
"Normal" = a non-elevated prompt, "admin" = Run as administrator. Paths are under
`C:\Users\aaron\Desktop\Avium\apollo-win`.

### 10.1 Step 0: ACX loads, the private interface still works

| # | Do | Pass |
|---|---|---|
| 0.1 | admin: `powershell -ExecutionPolicy Bypass -File tools\phase1\install.ps1` | x8p status OK, the script's polling reaches Stage Connected within 120 s, no bugcheck |
| 0.2 | normal: `tools\apollo-diag\apollo-diag.exe log` | contains `ACX 1.1 available`, `acx: circuit ApolloMon: 0x00000000`, `acx: circuit ApolloMonIn: 0x00000000`, `acx: add ApolloMon: 0x00000000`, `acx: add ApolloMonIn: 0x00000000`, `engine: RenderLeadFrames=144 CaptureGuardFrames=16 StartTransportAtConnect=1`, `transport auto-start: 0x00000000`, `engine started at frame …`; no `AcxDriverInitialize failed` / `AcxDeviceInit… failed` |
| 0.3 | normal: `apollo-diag info` | interface version 5 (no version warning), Stage Connected, flags include TRANSPORT and not ENGINE_LOST |
| 0.4 | normal: `apollo-diag levels 2`, `apollo-diag readback`, `apollo-diag strips` | all answer as before the ACX build |
| 0.5 | normal: `apollo-diag start` | refused: "TRANSPORT_START needs administrator rights" (the read-only handle) |
| 0.6 | admin: `apollo-diag read 0x2218` | prints the FPGA revision (0xA241C5AC): the token check lets an elevated handle through |
| 0.7 | admin: `apollo-diag tone 5` while normal: `apollo-diag levels 5` | tone on the front-panel meters; levels shows record slots 26-27 (MON L/R) near −20 dBFS |
| 0.8 | Apollo Control (already open or start it, no admin) | meters move; change input 1 gain and the monitor level; the unit follows |
| 0.9 | admin PowerShell: `$id = (Get-PnpDevice -PresentOnly -FriendlyName 'Apollo x8p*').InstanceId; Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_Security` | the descriptor is present (the INF's SDDL, now with LocalService); informational |

If 0.1 fails with code 10/31 or 0.2 shows an ACX failure: revert (risk list above) and send the
`apollo-diag log` output; nothing else in this build depends on ACX yet.

### 10.2 Step 1: the copy engine

| # | Do | Pass |
|---|---|---|
| 1.1 | normal: `apollo-diag engine` | `RUNNING (1 ms tick)`, ticks rising between two runs, dead reads 0 |
| 1.2 | normal: `apollo-diag engine 30 --reset` | about 1000 ticks/s, 47,999-48,001 frames/s, `+0 late`; record max interval (expect < 2000 us) and intervals > 2 ms / > 4 ms; exit code 0 |
| 1.3 | normal: `apollo-diag pos 30` | rate 47,999.x frames/s, 0 stalls, 0 backwards jumps (now served from the engine snapshot) |
| 1.4 | admin: `tools\loopback-test\loopback-test.exe 600` | `VERDICT: CLEAN`; then `apollo-diag engine` shows `late ticks 0`. Then the 1800 s run for the phase-2 gate |
| 1.5 | admin: `apollo-diag stop`, `apollo-diag engine`, `apollo-diag start`, `apollo-diag engine` | engine `stopped` after stop; `RUNNING` with starts +1 after start; `pos 3` rate normal |
| 1.6 | admin: `apollo-diag tone 30` and, during it, normal: `apollo-diag engine 10` | tone stays clean on the meters (diag writes and the engine's zero horizon coexist); `+0 late` |

### 10.3 ASIO

| # | Do | Pass |
|---|---|---|
| A.1 | normal: `asio\bin\asio-test.exe` | `driver: …, version 2`; `init` ok; 32 inputs, 34 outputs with the descriptor names; buffer 64..1024 pref 128; latency 154 in, 280 out (128 + 26, 128 + 152); `createBuffers(128 frames): ok` |
| A.2 | normal: `asio\bin\asio-test.exe --run 10 128`; meanwhile in a second normal prompt `apollo-diag engine` | `start: 0`; ≈ 3750 switches = 375/s; `input MON L peak` ≈ −20.0 dBFS (the tone came back through the monitor mix); MIC/LINE 1 zero unless strips are live; the new line `driver: N switches, U underruns, O overruns, R resyncs, S skipped by the DLL` shows U at most 1-2 (start-up), O 0, S 0. During the run: ASIO `running, 128 frames, 2 in, 2 out`, `skipped by the DLL 0`. After the run: ASIO `closed` |
| A.3 | repeat A.2 with 256, 1024, then 64 | 187.5/s, 46.9/s, 750/s; record underruns at 64 (see risk) |
| A.4 | two `asio-test --run 20 128` at once | the second prints "Another program is already using the Apollo x8p's ASIO driver" |
| A.5 | `asio-test --run 60 128`, end it from Task Manager after 10 s; then `apollo-diag engine` | no bugcheck; ASIO `closed` (EvtFileCleanup unmapped in the dying process) |
| A.6 | admin: `regsvr32 C:\Users\aaron\Desktop\Avium\apollo-win\asio\bin\avium-asio.dll` | success dialog; `HKLM\SOFTWARE\ASIO\Apollo x8p (Avium)` exists |
| A.7 | Ableton: Preferences → Audio: Driver Type ASIO, Audio Device "Apollo x8p (Avium)", 48000 Hz, buffer 128. Enable outputs MON L/R and input MON L/R in Input/Output Config | Ableton shows input ≈ 3.2 ms / output ≈ 5.8 ms latency; a clip plays to the monitors (front-panel meters); recording the MON L/R input captures what plays |
| A.8 | Ableton playing 30 min at 128; then `apollo-diag engine` | no audible dropouts; ASIO underruns/overruns 0 after start-up, late ticks 0 |
| A.9 | while Ableton plays: admin `apollo-diag tone 5` | both are heard together on MON (the ADD path) |

Do not run `loopback-test` while an ASIO host plays to MON L/R: both reach the same slots and the
loopback check will see the mix as discontinuities. The same holds for `wasapi-loop` (below) next to
ASIO, loopback-test, `apollo-diag tone`, or any other app or system sound playing to MON L/R.

### 10.4 Step 2: MON L/R playback (render circuit)

| # | Do | Pass |
|---|---|---|
| 2.1 | the end of `install.ps1` (0.1) | the log has `apollo-diag endpoints` with both rows `idle`; `Apollo endpoints in Windows:` lists `playback MON L/R (Apollo x8p)` and `recording MON L/R (Apollo x8p)`; then `Default audio devices unchanged.` or `put back '…'` lines. Any `could not put back` line: manual fix, §9.4 |
| 2.2 | normal: `tools\apollo-diag\apollo-diag.exe endpoints` | `EnabledEndpoints 0x3, 0 stream(s) prepared`; row 0 `out ApolloMon MON L/R 0-1 idle`, row 1 `in ApolloMonIn MON L/R 26-27 idle`; last line `ACX streams: prepared 0, running 0` |
| 2.3 | `Win+R` `mmsys.cpl` | Playback tab: **MON L/R**, *Apollo x8p*, Ready; your speakers keep the green tick. Recording tab: **MON L/R**, *Apollo x8p*; "Set Default" is greyed out for it (NeverSetAsDefault) |
| 2.4 | Playback → MON L/R → Properties → Advanced: the only format is *24 bit, 48000 Hz*. Press **Test** | the test chime plays on the monitors (front-panel MON meters move). This is the Windows shared engine path, like any app |
| 2.5 | normal: `tools\wasapi-loop\wasapi-loop.exe --tone 20`; during it, second normal prompt: `apollo-diag endpoints 10`, then `apollo-diag engine 10` | a clean 997 Hz tone on the monitors (−12 dBFS before the monitor knob). Endpoints row 0 `RUNNING 2x3840 B` (two 10 ms packets; another size is fine if Windows picked another period), packets and frames rising, underruns/overruns/dropped/late/notify-fail 0, `ACX streams: prepared 1, running 1`. Engine `+0 late`. wasapi-loop ends with `Played 20.0 s. Playback fed late: 0.` and a driver line of zeros |
| 2.6 | during another `wasapi-loop --tone 20`: normal `apollo-diag levels 5` | record slots 26-27 (MON L/R) show the tone, about −12 dBFS at unity monitor level; the other record slots unchanged |
| 2.7 | only if you make MON L/R the default output for a moment (mmsys.cpl → Set Default, put your speakers back afterwards): `powershell -c "(New-Object Media.SoundPlayer 'C:\Windows\Media\Ring05.wav').PlaySync()"` | heard on the monitors. SoundPlayer always uses the default output, hence the detour; 2.4 and 2.5 do not need it |
| 2.8 | cmd (normal): `for /l %i in (1,1,50) do tools\wasapi-loop\wasapi-loop.exe --tone 1` | 50 short tones, no click of old audio at any start (D8); `apollo-diag log` shows no transport restart; `apollo-diag endpoints` row 0 `streams` grew by 50 |

### 10.5 Step 3: MON L/R recording (capture circuit) and the 30-minute glitch run

| # | Do | Pass |
|---|---|---|
| 3.1 | `mmsys.cpl` → Recording tab open while `wasapi-loop --tone 30` plays | the level bar next to MON L/R moves with the tone (the tab opens a capture stream); `apollo-diag endpoints` row 1 `RUNNING`, overruns 0 |
| 3.2 | Windows Sound Recorder app: microphone = MON L/R (Apollo x8p), record 10 s during `wasapi-loop --tone 30`, play it back on your speakers | the recording is the tone, no gaps |
| 3.3 | normal: `tools\wasapi-loop\wasapi-loop.exe 60` | `The sine came back after …`; a progress line at 10 s steps with level L/R at the tone level, worst error about 1e-6 to 1e-5, `glitches 0`, `Windows discontinuities 0`, `position gaps 0`, `playback fed late 0`; driver line all `+0`; `VERDICT: CLEAN`, exit code 0 |
| 3.4 | normal: `wasapi-loop 60 --exclusive` | `VERDICT: CLEAN`. Both endpoints in exclusive event mode (the 2-packet path without the Windows mixer); `apollo-diag endpoints` during it shows the device-period packet size |
| 3.5 | **30-minute glitch run** (the PLAN phase 2 gate), Driver Verifier on: close other audio apps, turn on Do Not Disturb (Windows sounds would mix into MON L/R if it were the default), leave the monitor knob alone. Normal PowerShell: `& tools\wasapi-loop\wasapi-loop.exe 1800 \| Tee-Object tools\wasapi-loop\wasapi-loop-30min.log`. In a second normal prompt, now and then: `apollo-diag engine` and `apollo-diag endpoints` | `VERDICT: CLEAN` after 1800 s of checked audio; final driver line: render underruns +0, capture overruns +0, dropped frames +0, late packets +0, notify failures +0, engine late ticks +0; note the longest tick gap. Then `apollo-diag engine`: late ticks 0, not LOST |
| 3.6 | if 3.5 glitches: read the counters next to each `GLITCH` line | `playback fed late` = wasapi-loop's own scheduling (rerun); render underruns = the Windows engine released a packet late; capture overruns = it read a packet late; dropped / engine late ticks = the 1 ms timer ran later than the lead (try `RenderLeadFrames` 288); a glitch with every counter at 0 is on the x8p or ring side: compare with `loopback-test 1800` (admin, its own path) |
| 3.7 | in-use check: start `wasapi-loop --tone 300`, then admin `install.ps1` | the script lists `process wasapi-loop (PID …)`, `1 Windows audio stream(s) RUNNING …` and the session, then waits; closing wasapi-loop lets it go on within 2 s (`The x8p is free.`). With nothing closed it asks after 60 s: Enter / R / Q (Q = nothing changed) |

### 10.6 Review fixes

| # | Do | Pass |
|---|---|---|
| F.1 | A.5 (end `asio-test --run 60 128` from Task Manager after 10 s), then `apollo-diag engine` and `apollo-diag log` | no bugcheck; ASIO `closed`; the log may say `asio: view not unmapped (…); it goes with the process`, which is fine |
| F.2 | **before any power-cycle test on this build**: play `wasapi-loop --tone 120`, switch the x8p off for 10 s, on again | no hang, no bugcheck; the tone stops; after bring-up `apollo-diag info` Connected, `apollo-diag engine` RUNNING (not LOST). If `apollo-diag info` shows ENGINE_LOST (or `apollo-diag engine` says `LOST`) while the unit is on and connected, the guard fired: the monitors stay silent (the ring was zeroed, G2). Recover with admin `apollo-diag start` (no stop needed, works with Windows audio or ASIO open): the log says `engine recover: engine restarted on the running transport` or `… transport restarted`; `apollo-diag engine` RUNNING again, and wasapi-loop/ASIO resume after a resync. If it answers `STATUS_DEVICE_NOT_CONNECTED`, the unit really is gone: replug |
| G.1 | install.ps1 and revert.ps1 (admin) after `tools\build-all.cmd` | they print no tool-check error and run devcon/apollo-diag from `C:\Program Files\Avium\ApolloTools`. `icacls "C:\Program Files\Avium\ApolloTools"` shows only `NT AUTHORITY\SYSTEM` and `BUILTIN\Administrators` (full, inherited to `(OI)(CI)`). Rebuild apollo-diag without build-all, then run install.ps1: it stops with `does not match tools-manifest.txt` before touching anything |
| G.2 | x8p already on apollo.sys, Twin X console open, admin `install.ps1` | the UA console (Twin X) keeps running; only on a first install from UA's driver are UA's apps closed |
| G.3 | `wasapi-loop 60` and `wasapi-loop 60 --exclusive` | still `VERDICT: CLEAN`, `Windows discontinuities 0`, driver capture overruns +0 (now counted only for clients that fetch packets) |
| F.3 | `asio-test --run 10 64` | the driver line's underruns, overruns and `skipped by the DLL` move together (each skipped switch costs one of each); at 128 all three stay 0 after start-up |
| F.6 | normal: `apollo-diag pos 5` | ends after 5 s on its own clock; no `DEAD` line on a healthy unit |
| F.8 | normal cmd: `copy tools\apollo-diag\apollo-diag.exe %TEMP%\diag-low.exe`, `icacls %TEMP%\diag-low.exe /setintegritylevel low`, `%TEMP%\diag-low.exe info` | the low-integrity copy cannot open the device (access denied, error 5); the normal `apollo-diag info` still works and Windows audio (2.4) still plays, so LocalService and users kept access. If the low copy still opens it, PnP did not apply the label: no harm, report it |
