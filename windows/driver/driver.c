/*
 * driver.c - DriverEntry and EvtDriverDeviceAdd for apollo.sys.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "apollo.h"
#include "apollo_acx.h"

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD ApolloEvtDeviceAdd;
static EVT_WDF_INTERRUPT_ENABLE ApolloEvtInterruptEnable;
static EVT_WDF_INTERRUPT_DISABLE ApolloEvtInterruptDisable;
static EVT_WDF_OBJECT_CONTEXT_CLEANUP ApolloEvtDeviceCleanup;
static EVT_WDF_DRIVER_UNLOAD ApolloEvtDriverUnload;

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, ApolloEvtDeviceAdd)
#endif

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    ACX_DRIVER_CONFIG acx;
    ACX_DRIVER_VERSION_AVAILABLE_PARAMS ver;
    WDFDRIVER driver;
    NTSTATUS status;

    ApolloLogInit();
    ApolloLog("apollo.sys loading (phase 2: ACX, copy engine, ASIO; interface v%d: Twin X Gen 1 bring-up G3, "
              "Twin X Mode 2 endpoints G4, combined device G6)", APOLLO_IOCTL_VERSION);
    WDF_DRIVER_CONFIG_INIT(&config, ApolloEvtDeviceAdd);
    config.EvtDriverUnload = ApolloEvtDriverUnload;     /* [G6] the aggregate's pool */
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config,
                             &driver);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* ACX: right after the WDF driver object (docs/PHASE2.md section 1). Flags are reserved. */
    ACX_DRIVER_CONFIG_INIT(&acx);
    status = AcxDriverInitialize(driver, &acx);
    if (!NT_SUCCESS(status)) {
        ApolloLog("AcxDriverInitialize failed 0x%08x", status);
        return status;
    }
    ACX_DRIVER_VERSION_AVAILABLE_PARAMS_INIT(&ver, 1, 1);
    ApolloLog("ACX 1.1 %s", AcxDriverIsVersionAvailable(driver, &ver) ? "available" : "NOT available");
    /* [G6] the combined device; without it every unit still runs standalone */
    status = ApolloAggInit(driver);
    if (!NT_SUCCESS(status)) {
        ApolloLog("combined device not available (0x%08x): units run standalone", status);
    }
    return STATUS_SUCCESS;
}

/* [G6] Unload: the aggregate's tables and staging (every device is gone by now). */
static VOID ApolloEvtDriverUnload(_In_ WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    ApolloAggUnload();
}

/*
 * [Twin X G2] Which slot of apollo.inf this device came in by, before anything else is known:
 * the subsystem of its PCI hardware ID (apollo_unit.h ApolloHwidSubsystem). It names the unit
 * in the log and decides whether device add creates the x8p's ACX circuits (circuit.c).
 */
_IRQL_requires_(PASSIVE_LEVEL)
static VOID ApolloReadHardwareId(_In_ PDEVICE_CONTEXT Ctx)
{
    WDFMEMORY mem = NULL;
    size_t bytes = 0;
    const WCHAR *hwid = L"";
    ULONG subsys = 0;
    NTSTATUS status;

    status = WdfDeviceAllocAndQueryProperty(Ctx->Device, DevicePropertyHardwareID, NonPagedPoolNx,
                                            WDF_NO_OBJECT_ATTRIBUTES, &mem);
    if (NT_SUCCESS(status)) {
        hwid = (const WCHAR *)WdfMemoryGetBuffer(mem, &bytes);
        Ctx->HwidSubsysKnown = ApolloHwidSubsystem(hwid, (ULONG)(bytes / sizeof(WCHAR)), &subsys) ? TRUE : FALSE;
        Ctx->HwidSubsystem = subsys;
    }
    ApolloLogTagFor(Ctx->HwidSubsysKnown, Ctx->HwidSubsystem, Ctx->LogTag);
    if (Ctx->HwidSubsysKnown) {
        ApolloLogU(Ctx, "device add: %ws, PCI subsystem 0x%04lx (%s)", hwid, Ctx->HwidSubsystem,
                   ApolloAddTimeX8pRules(TRUE, Ctx->HwidSubsystem) ? "the x8p's" :
                   (ApolloAddTimeModel(TRUE, Ctx->HwidSubsystem) == APOLLO_MODEL_TWINX ?
                    "the Twin X's: its own endpoint table, added in Mode 2 only" :
                    "not an Apollo this driver runs: no Windows endpoints"));
    } else {
        ApolloLogU(Ctx, "device add: hardware ID not read (0x%08x): x8p rules for the endpoints, as before", status);
    }
    if (mem != NULL) {
        WdfObjectDelete(mem);
    }
}

/* Device object teardown: an ASIO client still open would outlive its pages. */
static VOID ApolloEvtDeviceCleanup(_In_ WDFOBJECT Object)
{
    PDEVICE_CONTEXT ctx = ApolloGetContext((WDFDEVICE)Object);

    ApolloAggLeave(ctx);                /* [G6] backstop: D0 exit normally left already */
    ApolloEngineStop(ctx);
    ApolloAsioCloseAny(ctx);
    ApolloPluginFree(ctx);              /* [P0] */
    ApolloCorrFree(ctx);                /* [Corr] after the engine stopped and the unit left the combined device */
}

static NTSTATUS ApolloEvtInterruptEnable(_In_ WDFINTERRUPT Interrupt, _In_ WDFDEVICE Device)
{
    UNREFERENCED_PARAMETER(Interrupt);
    /* IMR stays 0 here; bring-up enables vectors once SG tables are valid. */
    InterlockedExchange(&ApolloGetContext(Device)->InterruptConnected, 1);
    return STATUS_SUCCESS;
}

static NTSTATUS ApolloEvtInterruptDisable(_In_ WDFINTERRUPT Interrupt, _In_ WDFDEVICE Device)
{
    PDEVICE_CONTEXT ctx = ApolloGetContext(Device);

    UNREFERENCED_PARAMETER(Interrupt);
    /* Called with the interrupt lock held: plain register writes only. */
    ctx->ImrLo = 0;
    ctx->ImrHi = 0;
    ApolloWrite(ctx, REG_IMR, 0);
    if (ctx->Irq64) {
        ApolloWrite(ctx, REG_IMR_HI, 0);
    }
    InterlockedExchange(&ctx->InterruptConnected, 0);
    return STATUS_SUCCESS;
}

static NTSTATUS ApolloEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_INTERRUPT_CONFIG icfg;
    WDF_DMA_ENABLER_CONFIG dcfg;
    WDF_WORKITEM_CONFIG wcfg;
    WDF_IO_QUEUE_CONFIG qcfg;
    WDF_FILEOBJECT_CONFIG fcfg;
    ACX_DEVICEINIT_CONFIG acxInit;
    ACX_DEVICE_CONFIG acxDev;
    WDFDEVICE device;
    PDEVICE_CONTEXT ctx;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();

    /*
     * ACX first, before any other WDFDEVICE_INIT call. It adds class-extension
     * hooks only: a create callback that claims opens of its circuit names
     * (ours has no reference string, so it reaches ApolloEvtDeviceFileCreate),
     * and IOCTL dispatch that takes the KS codes and passes every other code
     * to our default queue (docs/PHASE2.md section 2).
     */
    ACX_DEVICEINIT_CONFIG_INIT(&acxInit);
    status = AcxDeviceInitInitialize(DeviceInit, &acxInit);
    if (!NT_SUCCESS(status)) {
        ApolloLog("AcxDeviceInitInitialize failed 0x%08x", status);
        return status;
    }

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = ApolloEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = ApolloEvtReleaseHardware;
    pnp.EvtDeviceD0Entry = ApolloEvtD0Entry;
    pnp.EvtDeviceD0EntryPostInterruptsEnabled = ApolloEvtD0EntryPostInterruptsEnabled;
    pnp.EvtDeviceD0ExitPreInterruptsDisabled = ApolloEvtD0ExitPreInterruptsDisabled;
    pnp.EvtDeviceD0Exit = ApolloEvtD0Exit;
    pnp.EvtDeviceSurpriseRemoval = ApolloEvtSurpriseRemoval;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    /*
     * Security comes only from apollo.inf (HKR,,Security), which PnP applies
     * to every device object in the stack: SYSTEM and Administrators
     * everything, LocalService read/write (audiosrv and audiodg open the ACX
     * endpoints), interactive users read access (= the control IOCTLs, see
     * apollo_ioctl.h). The admin IOCTLs additionally need an elevated token
     * at open time (ApolloEvtDeviceFileCreate). Do not call
     * WdfDeviceInitAssignSDDLString here: this FDO is unnamed, and KMDF cannot
     * put a security descriptor on an unnamed device object. WdfDeviceCreate
     * then fails with STATUS_INVALID_SECURITY_DESCR (code 31 on the x8p, Nov 14 2026).
     */
    WdfDeviceInitSetCharacteristics(DeviceInit, FILE_DEVICE_SECURE_OPEN, TRUE);
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);

    /*
     * Per-handle context: same file object class as ACX's, no synchronization scope.
     * Close is the ASIO backstop (an ASIO_OPEN that raced the cleanup) and drops the
     * creator reference (asio.c).
     */
    WDF_FILEOBJECT_CONFIG_INIT(&fcfg, ApolloEvtDeviceFileCreate, ApolloEvtFileClose,
                               ApolloEvtFileCleanup);
    fcfg.FileObjectClass = WdfFileObjectWdfCanUseFsContext;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, APOLLO_FILE);
    attrs.SynchronizationScope = WdfSynchronizationScopeNone;
    attrs.ExecutionLevel = WdfExecutionLevelPassive;
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &fcfg, &attrs);

    /* ASIO_OPEN maps memory into the caller: it must run in the caller's context. */
    WdfDeviceInitSetIoInCallerContextCallback(DeviceInit, ApolloEvtIoInCallerContext);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, DEVICE_CONTEXT);
    attrs.EvtCleanupCallback = ApolloEvtDeviceCleanup;
    status = WdfDeviceCreate(&DeviceInit, &attrs, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    ACX_DEVICE_CONFIG_INIT(&acxDev);
    status = AcxDeviceInitialize(device, &acxDev);
    if (!NT_SUCCESS(status)) {
        ApolloLog("AcxDeviceInitialize failed 0x%08x", status);
        return status;
    }

    ctx = ApolloGetContext(device);
    ctx->Device = device;
    ctx->Stage = ApolloStageNone;
    ctx->Shutdown = 1;                  /* gate stays closed until BAR0 is mapped */
    KeInitializeEvent(&ctx->ServiceStop, NotificationEvent, FALSE);
    KeInitializeSpinLock(&ctx->EngineLock);
    /* [Twin X G2] the unit's log tag and add-time rules, before its first log line */
    ApolloReadHardwareId(ctx);

    status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &ctx->DevLock);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &ctx->AsioLock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_INTERRUPT_CONFIG_INIT(&icfg, ApolloEvtInterruptIsr, ApolloEvtInterruptDpc);
    icfg.EvtInterruptEnable = ApolloEvtInterruptEnable;
    icfg.EvtInterruptDisable = ApolloEvtInterruptDisable;
    status = WdfInterruptCreate(device, &icfg, WDF_NO_OBJECT_ATTRIBUTES, &ctx->Interrupt);
    if (!NT_SUCCESS(status)) {
        ApolloLogU(ctx, "WdfInterruptCreate failed 0x%08x", status);
        return status;
    }

    /* 32-bit profile: "the FPGA may only use the low 32 bits internally". */
    WDF_DMA_ENABLER_CONFIG_INIT(&dcfg, WdfDmaProfileScatterGather, AUDIO_RING_BYTES);
    status = WdfDmaEnablerCreate(device, &dcfg, WDF_NO_OBJECT_ATTRIBUTES, &ctx->DmaEnabler);
    if (!NT_SUCCESS(status)) {
        ApolloLogU(ctx, "WdfDmaEnablerCreate failed 0x%08x", status);
        return status;
    }

    WDF_WORKITEM_CONFIG_INIT(&wcfg, ApolloEvtBringupWork);
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ParentObject = device;
    status = WdfWorkItemCreate(&wcfg, &attrs, &ctx->BringupWork);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Input strips (strips.c): StripLock is always taken before DevLock. */
    status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &ctx->StripLock);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    WDF_WORKITEM_CONFIG_INIT(&wcfg, ApolloEvtStripWork);
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ParentObject = device;
    status = WdfWorkItemCreate(&wcfg, &attrs, &ctx->StripWork);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* [ACX endpoints, PHASE2.md steps 2-3] circuits per EnabledEndpoints (circuit.c). */
    status = ApolloAcxDeviceInit(ctx);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* [Events] the event ring and the queue parked WAIT_EVENTS wait in (events.c) */
    status = ApolloEventsInit(ctx);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qcfg, WdfIoQueueDispatchSequential);
    qcfg.EvtIoDeviceControl = ApolloEvtIoDeviceControl;
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfIoQueueCreate(device, &qcfg, &attrs, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_APOLLO, NULL);
}
