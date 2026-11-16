/*
 * ioctl.c - diagnostic control interface (include/apollo_ioctl.h).
 *
 * METHOD_BUFFERED: input and output share one system buffer, so every
 * handler copies what it needs from the input before writing output.
 * The queue is sequential and runs at PASSIVE_LEVEL. ASIO IOCTLs never get
 * here: ApolloEvtIoInCallerContext (asio.c) handles them in the caller's thread.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <ntifs.h>
#include "apollo.h"
#include "preamp_plan.h"        /* [Twin X G5] GET_INFO NumPreamps per model */
#include <ntstrsafe.h>

#define IOCTL_LOCK_TIMEOUT_MS   3000

/* [G4] bringup_plan.h keeps its own copies of the monitor parameter codes (plain C, no Windows types) */
C_ASSERT(APOLLO_PLAN_MON_LEVEL == APOLLO_MON_LEVEL && APOLLO_PLAN_MON_HP1_LEVEL == APOLLO_MON_HP1_LEVEL &&
         APOLLO_PLAN_MON_MUTE == APOLLO_MON_MUTE && APOLLO_PLAN_MON_SOURCE == APOLLO_MON_SOURCE &&
         APOLLO_PLAN_MON_MONO == APOLLO_MON_MONO && APOLLO_PLAN_MON_HP1_SOURCE == APOLLO_MON_HP1_SOURCE &&
         APOLLO_PLAN_MON_HP2_SOURCE == APOLLO_MON_HP2_SOURCE && APOLLO_PLAN_MON_DIM_LEVEL == APOLLO_MON_DIM_LEVEL &&
         APOLLO_PLAN_MON_DIM == APOLLO_MON_DIM && APOLLO_PLAN_MON_TALKBACK == APOLLO_MON_TALKBACK);

/*
 * Every open of the device that ACX does not claim (our interface has no
 * reference string, so all of them) lands here. The opener's token decides,
 * once, whether this handle may use the admin IOCTLs: device security now
 * gives LocalService write access for the audio stack, so FILE_WRITE_ACCESS
 * alone no longer means "administrator".
 */
VOID ApolloEvtDeviceFileCreate(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request,
                               _In_ WDFFILEOBJECT FileObject)
{
    PAPOLLO_FILE f = ApolloGetFile(FileObject);
    WDF_REQUEST_PARAMETERS p;
    PIO_SECURITY_CONTEXT sc;
    PEPROCESS creator;

    UNREFERENCED_PARAMETER(Device);
    if (f == NULL) {
        /* No context: the handle works as a plain user handle (no admin, no ASIO). */
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;
    }
    f->IsAdmin = FALSE;
    f->CleanedUp = FALSE;
    /* The process that opened the handle; ASIO refuses every other (asio.c). */
    creator = IoGetRequestorProcess(WdfRequestWdmGetIrp(Request));
    if (creator == NULL) {
        creator = PsGetCurrentProcess();
    }
    ObReferenceObject(creator);
    f->Creator = creator;               /* released in ApolloEvtFileClose */
    WDF_REQUEST_PARAMETERS_INIT(&p);
    WdfRequestGetParameters(Request, &p);
    sc = p.Parameters.Create.SecurityContext;
    if (sc != NULL && sc->AccessState != NULL) {
        PACCESS_TOKEN token = SeQuerySubjectContextToken(&sc->AccessState->SubjectSecurityContext);

        if (token != NULL) {
            f->IsAdmin = SeTokenIsAdmin(token);
        }
    }
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

/* Codes with FILE_WRITE_ACCESS in their access bits are the admin set (apollo_ioctl.h). */
static BOOLEAN ApolloNeedsAdmin(_In_ ULONG Code)
{
    return ((Code >> 14) & FILE_WRITE_ACCESS) != 0;
}

static BOOLEAN ApolloRequestIsAdmin(_In_ WDFREQUEST Request)
{
    WDFFILEOBJECT fo = WdfRequestGetFileObject(Request);
    PAPOLLO_FILE f = (fo != NULL) ? ApolloGetFile(fo) : NULL;

    return f != NULL && f->IsAdmin;     /* no context (an ACX handle): not admin */
}

static NTSTATUS ApolloLockForIoctl(_In_ PDEVICE_CONTEXT Ctx)
{
    LONGLONG t = -10000LL * IOCTL_LOCK_TIMEOUT_MS;
    NTSTATUS status;

    status = WdfWaitLockAcquire(Ctx->DevLock, &t);
    if (status == STATUS_TIMEOUT) {
        return STATUS_DEVICE_BUSY;       /* bring-up holds it (ACEFACE, DSP ready) */
    }
    return STATUS_SUCCESS;
}

static BOOLEAN ApolloRegOffsetOk(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Offset)
{
    return (Offset & 3) == 0 && Offset < Ctx->Bar0Length && Ctx->Bar0 != NULL;
}

static VOID ApolloFillInfo(_In_ PDEVICE_CONTEXT Ctx, _Out_ PAPOLLO_INFO I)
{
    RtlZeroMemory(I, sizeof(*I));
    I->Version = APOLLO_IOCTL_VERSION;
    I->Stage = (ULONG)Ctx->Stage;
    I->FailedStage = (ULONG)Ctx->FailedStage;
    I->LastStatus = Ctx->LastStatus;
    I->Bar0Length = Ctx->Bar0Length;
    I->FpgaRev = Ctx->FpgaRev;
    I->ExtCaps = Ctx->ExtCaps;
    I->FwRelease = Ctx->FwRelease;
    I->SubsystemId = Ctx->SubsystemId;
    I->DeviceType = Ctx->DeviceType;
    I->PublicType = Ctx->PublicType;
    I->NumDsps = Ctx->NumDsps;
    I->PlayChannels = Ctx->PlayChannels;
    I->RecChannels = Ctx->RecChannels;
    I->RingFrames = Ctx->RingFrames;
    I->SampleRate = Ctx->SampleRate;
    I->DmaCtrl = Ctx->DmaCtrlCache;
    I->NotifRateInfo = Ctx->NotifRateInfo;
    I->ServicePasses = Ctx->ServicePasses;
    I->InterruptCount = (ULONG)Ctx->InterruptCount;
    I->EndOfBufferCount = (ULONG)Ctx->EndOfBufferCount;
    I->NotifStatusSeen = Ctx->NotifStatusSeen;
    /* [Twin X G0] an accepted x8p only; [G5] the model's preamp plan: x8p 8, Twin X 2 (spec-twinx 4.1). */
    I->NumPreamps = ApolloPreampCount(Ctx->Ident.Model);
    RtlCopyMemory(I->Serial, Ctx->Serial, sizeof(I->Serial));

    if (Ctx->FwV2)                  I->Flags |= APOLLO_FLAG_FW_V2;
    if (Ctx->Irq64)                 I->Flags |= APOLLO_FLAG_IRQ64;
    if (Ctx->WarmStart)             I->Flags |= APOLLO_FLAG_WARM_START;
    if (Ctx->RingsLive)             I->Flags |= APOLLO_FLAG_RINGS_LIVE;
    if (Ctx->DspsConnected)         I->Flags |= APOLLO_FLAG_DSPS_CONNECTED;
    if (Ctx->AcefaceDone)           I->Flags |= APOLLO_FLAG_ACEFACE_DONE;
    if (Ctx->Connected)             I->Flags |= APOLLO_FLAG_CONNECTED;
    if (Ctx->MixerReady)            I->Flags |= APOLLO_FLAG_MIXER_READY;
    if (Ctx->TransportRunning)      I->Flags |= APOLLO_FLAG_TRANSPORT;
    if (Ctx->ServiceRunning)        I->Flags |= APOLLO_FLAG_SERVICE_RUNNING;
    if (Ctx->ReadbackValid)         I->Flags |= APOLLO_FLAG_READBACK_VALID;
    if (Ctx->Msi)                   I->Flags |= APOLLO_FLAG_MSI;
    if (Ctx->FwUntested)            I->Flags |= APOLLO_FLAG_FW_UNTESTED;
    if (ApolloEngineLost(Ctx))      I->Flags |= APOLLO_FLAG_ENGINE_LOST;

    /* [Twin X G2] version 6: which unit this is (apollo_ioctl.h) */
    I->HwUidLo = Ctx->HwUidLo;
    I->HwUidHi = Ctx->HwUidHi;
    I->Model = (ULONG)Ctx->Ident.Model;
    I->Mode = Ctx->Unit.Resolved ? Ctx->Unit.Value[APOLLO_PARAM_MODE] : APOLLO_INFO_MODE_UNKNOWN;
    I->UnitFlags = ApolloUnitInfoFlags(Ctx);
    I->AggRole = ApolloAggRoleOf(Ctx);      /* [G6] standalone / master / follower / acting master */
    I->AggId = 0;
    I->HwidSubsystem = Ctx->HwidSubsysKnown ? Ctx->HwidSubsystem : 0;
    if (Ctx->Ident.Name != NULL) {
        (VOID)RtlStringCbCopyA(I->ModelName, sizeof(I->ModelName), Ctx->Ident.Name);
    }
    (VOID)RtlStringCbCopyA(I->UnitKey, sizeof(I->UnitKey), Ctx->Ident.UnitKey);
    (VOID)RtlStringCbCopyA(I->LogTag, sizeof(I->LogTag), Ctx->LogTag);
    if (Ctx->OwnerRead) {
        I->OwnerRateInfo = Ctx->OwnerRateInfo;
        I->OwnerClockSrcs = Ctx->OwnerClockSrcs;
        I->OwnerDescInVersion = Ctx->OwnerDescInVersion;
        I->OwnerDescInCount = Ctx->OwnerDescInCount;
        I->OwnerDescOutVersion = Ctx->OwnerDescOutVersion;
        I->OwnerDescOutCount = Ctx->OwnerDescOutCount;
    }
    /* [Twin X G3] version 7: the AudioExtension bring-up (0 on the x8p path) */
    I->ClockSrcsLive = Ctx->NotifClockSrcsLive;
    I->BringupSteps = Ctx->BringupSteps;
    I->BringupFailStep = Ctx->BringupFailStep;
    I->BringupApplies = Ctx->BringupApplies;            /* [G4] the former Reserved7 */
}

/*
 * [Twin X G3] Every IOCTL that reaches the queue has a class (bringup_plan.h); 0 = none (an
 * unknown code, answered "invalid request" as before).
 */
static ULONG ApolloIoctlClass(_In_ ULONG Code)
{
    switch (Code) {
    case IOCTL_APOLLO_READ_REG:
    case IOCTL_APOLLO_GET_POSITION:
    case IOCTL_APOLLO_GET_READBACK:
    case IOCTL_APOLLO_GET_LEVELS:
    case IOCTL_APOLLO_GET_STRIPS:
        return APOLLO_IOCTL_CLASS_READ;
    case IOCTL_APOLLO_READ_CAPTURE:
    case IOCTL_APOLLO_WRITE_PLAYBACK:
        return APOLLO_IOCTL_CLASS_AUDIO_IO;
    case IOCTL_APOLLO_TRANSPORT_START:
    case IOCTL_APOLLO_TRANSPORT_STOP:
        return APOLLO_IOCTL_CLASS_TRANSPORT;
    case IOCTL_APOLLO_SET_INPUT_PARAM:
    case IOCTL_APOLLO_SET_MONITOR_PARAM:
    case IOCTL_APOLLO_SET_MIXER_SETTING:
        return APOLLO_IOCTL_CLASS_SETTINGS;
    case IOCTL_APOLLO_WRITE_REG:
        return APOLLO_IOCTL_CLASS_RAW_WRITE;
    default:
        return 0;
    }
}

/*
 * [Twin X G2] A unit the driver does not run (observe: Mode 0 or no bring-up path; refused) is
 * read-only. Besides the lock-free set (info, log, engine stats, endpoints) only these reads
 * reach it (READ_REG among them, for administrators); every write, the transport and audio I/O
 * are refused, whoever asks. ASIO is refused in asio.c.
 */
static BOOLEAN ApolloObserveAllows(_In_ ULONG Code)
{
    return ApolloIoctlClass(Code) == APOLLO_IOCTL_CLASS_READ;
}

/*
 * [Twin X G3] A unit the driver runs answers what its plan allows in its Mode: the x8p
 * everything (as before); a Twin X in Mode 1 the reads plus capture and playback (apollo-diag
 * capture, tone): no settings, no raw writes, no transport start/stop.
 * [G4] A Twin X in Mode 2 also takes the SETTINGS class, narrowed per parameter in the switch
 * below (bringup_plan.h): SET_MONITOR_PARAM level, HP1 level, mute, mono, dim only; no
 * SET_INPUT_PARAM (its preamps wait for G5) and no raw SET_MIXER_SETTING. STATUS_NOT_SUPPORTED.
 * [G5] SET_INPUT_PARAM on its 2 preamps (preamp_plan.h) and SET_MONITOR_PARAM talkback too.
 */
static BOOLEAN ApolloActiveAllows(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Code)
{
    return ApolloPlanIoctlAllowed(ApolloPlanOf(Ctx), Ctx->Unit.Value[APOLLO_PARAM_MODE],
                                  ApolloIoctlClass(Code)) ? TRUE : FALSE;
}

VOID ApolloEvtIoDeviceControl(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request,
                              _In_ size_t OutputBufferLength, _In_ size_t InputBufferLength,
                              _In_ ULONG IoControlCode)
{
    PDEVICE_CONTEXT ctx = ApolloGetContext(WdfIoQueueGetDevice(Queue));
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info = 0;
    PVOID in = NULL, out = NULL;
    size_t inLen = 0, outLen = 0;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    if (InputBufferLength > 0) {
        (VOID)WdfRequestRetrieveInputBuffer(Request, 1, &in, &inLen);
    }
    if (OutputBufferLength > 0) {
        (VOID)WdfRequestRetrieveOutputBuffer(Request, 1, &out, &outLen);
    }

    if (ApolloNeedsAdmin(IoControlCode) && !ApolloRequestIsAdmin(Request)) {
        WdfRequestComplete(Request, STATUS_ACCESS_DENIED);
        return;
    }

    if (IoControlCode == IOCTL_APOLLO_GET_ENGINE_STATS) {
        /* Lock-free like GET_INFO: EngineLock only. */
        BOOLEAN reset = (in != NULL && inLen >= sizeof(ULONG) &&
                         (*(PULONG)in & APOLLO_ENGINE_RESET) != 0);

        if (out == NULL || outLen < sizeof(APOLLO_ENGINE_STATS)) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else {
            ApolloEngineFillStats(ctx, (PAPOLLO_ENGINE_STATS)out, reset);
            info = sizeof(APOLLO_ENGINE_STATS);
            status = STATUS_SUCCESS;
        }
        WdfRequestCompleteWithInformation(Request, status, info);
        return;
    }

    if (ApolloAggIsIoctl(IoControlCode)) {
        /*
         * [G6] The combined device (agg.c): driver-global, the same from any unit's handle, its own
         * locks (the clock probe takes this unit's DevLock itself). The admin check above covers
         * AGG_CLOCK_PROBE; the rest are user level by design (spec 6.8: no elevation for Apply).
         */
        if (ctx->Stage == ApolloStageGone || ApolloGateClosed(ctx)) {
            WdfRequestComplete(Request, STATUS_NO_SUCH_DEVICE);
            return;
        }
        status = ApolloAggIoctl(ctx, IoControlCode, in, inLen, out, outLen, &info);
        WdfRequestCompleteWithInformation(Request, status, info);
        return;
    }

    if (IoControlCode == IOCTL_APOLLO_GET_ENDPOINTS) {
        /*
         * [ACX endpoints] lock-free like GET_INFO: EngineLock only (stream.c). [Endpoints] v2: an optional
         * APOLLO_ENDPOINTS_QUERY names the first row; read it before the output is written (METHOD_BUFFERED:
         * one system buffer for both). A larger output buffer gets more than 16 rows.
         */
        ULONG first = 0;

        if (in != NULL && inLen >= sizeof(APOLLO_ENDPOINTS_QUERY)) {
            first = ((PAPOLLO_ENDPOINTS_QUERY)in)->FirstRow;
        }
        if (out == NULL || outLen < sizeof(APOLLO_ENDPOINTS)) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else {
            info = ApolloEndpointsFillInfo(ctx, first, out, outLen);
            status = STATUS_SUCCESS;
        }
        WdfRequestCompleteWithInformation(Request, status, info);
        return;
    }

    if (IoControlCode == IOCTL_APOLLO_GET_INFO) {
        /*
         * Lock-free so it answers during a long bring-up. [Twin X G2] Version 6 grew the
         * structure: a client built for version 5 (APOLLO_INFO_V5_BYTES) gets exactly the part
         * it knows, a newer one the whole of it.
         */
        if (out == NULL || outLen < APOLLO_INFO_V5_BYTES) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else {
            APOLLO_INFO full;

            ApolloFillInfo(ctx, &full);
            info = min(outLen, sizeof(full));
            RtlCopyMemory(out, &full, info);
            status = STATUS_SUCCESS;
        }
        WdfRequestCompleteWithInformation(Request, status, info);
        return;
    }

    if (IoControlCode == IOCTL_APOLLO_GET_RATE) {
        /* [Rate] lock-free like GET_INFO: it answers while a change holds DevLock (rate.c) */
        if (out == NULL || outLen < sizeof(APOLLO_RATE_INFO)) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else {
            ApolloRateFillInfo(ctx, (PAPOLLO_RATE_INFO)out);
            info = sizeof(APOLLO_RATE_INFO);
            status = STATUS_SUCCESS;
        }
        WdfRequestCompleteWithInformation(Request, status, info);
        return;
    }

    if (IoControlCode == IOCTL_APOLLO_GET_CLOCK) {
        /* [Clock] lock-free like GET_RATE: it answers while a clock change holds DevLock (clock.c) */
        if (out == NULL || outLen < sizeof(APOLLO_CLOCK_INFO)) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else {
            ApolloClockFillInfo(ctx, (PAPOLLO_CLOCK_INFO)out);
            info = sizeof(APOLLO_CLOCK_INFO);
            status = STATUS_SUCCESS;
        }
        WdfRequestCompleteWithInformation(Request, status, info);
        return;
    }

    if (IoControlCode == IOCTL_APOLLO_GET_LOG) {
        /* Lock-free and available even after the device is gone. */
        PAPOLLO_LOG_HEADER hdr = (PAPOLLO_LOG_HEADER)out;

        if (out == NULL || outLen < sizeof(*hdr)) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else {
            ULONG room = (ULONG)min(outLen - sizeof(*hdr), (size_t)APOLLO_LOG_BYTES);

            hdr->Length = ApolloLogSnapshot((PCHAR)(hdr + 1), room, &hdr->TotalWritten);
            info = sizeof(*hdr) + hdr->Length;
            status = STATUS_SUCCESS;
        }
        WdfRequestCompleteWithInformation(Request, status, info);
        return;
    }

    if (ApolloEventsIsIoctl(IoControlCode)) {
        /* [Events] answered now or parked until the next event (events.c); a gone unit answers NO_SUCH_DEVICE */
        ApolloEventsIoctl(ctx, Request);
        return;
    }
    if (ApolloParamsIsIoctl(IoControlCode)) {
        /* [Params] lock-free: the values this start resolved, and Units\<key> in the registry (params.c);
           SET_PARAM is administrators only (the admin check above) and applies at the next start */
        status = ApolloParamsIoctl(ctx, IoControlCode, in, inLen, out, outLen, &info);
        WdfRequestCompleteWithInformation(Request, status, info);
        return;
    }

    if (ctx->Stage == ApolloStageGone || ApolloGateClosed(ctx)) {
        WdfRequestComplete(Request, STATUS_NO_SUCH_DEVICE);
        return;
    }
    if (ApolloCorrIsIoctl(IoControlCode)) {
        /* [Corr] speaker correction (corr.c): its own lock, then EngineLock; refuses observe units itself */
        status = ApolloCorrIoctl(ctx, IoControlCode, in, inLen, out, outLen, &info);
        WdfRequestCompleteWithInformation(Request, status, info);
        return;
    }
    if (!ctx->Unit.Active && !ApolloObserveAllows(IoControlCode)) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE);   /* [Twin X G2] observe: read-only */
        return;
    }
    if (ctx->Unit.Active && !ApolloActiveAllows(ctx, IoControlCode)) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE);   /* [Twin X G3] Mode 1 Twin X: test set only */
        return;
    }
    if (ApolloPluginIsIoctl(IoControlCode)) {
        /* [P0] plug-in hosting (plugin.c): it takes StripLock, then DevLock, and waits for the DSP off-lock */
        status = ApolloPluginIoctl(ctx, Request, IoControlCode, in, inLen, out, outLen, &info);
        WdfRequestCompleteWithInformation(Request, status, info);
        return;
    }
    status = ApolloLockForIoctl(ctx);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    switch (IoControlCode) {
    case IOCTL_APOLLO_READ_REG: {
        APOLLO_REG_IO r;

        if (in == NULL || inLen < sizeof(r) || out == NULL || outLen < sizeof(r)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        r = *(PAPOLLO_REG_IO)in;
        if (!ApolloRegOffsetOk(ctx, r.Offset)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        r.Value = ApolloRead(ctx, r.Offset);
        *(PAPOLLO_REG_IO)out = r;
        info = sizeof(r);
        status = STATUS_SUCCESS;
        break;
    }
    case IOCTL_APOLLO_WRITE_REG: {
        APOLLO_REG_IO r;

        if (in == NULL || inLen < sizeof(r)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        r = *(PAPOLLO_REG_IO)in;
        if (!ApolloRegOffsetOk(ctx, r.Offset)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        ApolloLogU(ctx, "diag write 0x%04x = 0x%08x", r.Offset, r.Value);
        ApolloStripsTaint(ctx, r.Offset);       /* outside the mailbox: strips off until a power cycle */
        ApolloWrite(ctx, r.Offset, r.Value);
        status = STATUS_SUCCESS;
        break;
    }
    case IOCTL_APOLLO_TRANSPORT_START:
        if (!ctx->RingsLive) {
            status = STATUS_DEVICE_NOT_READY;
            break;
        }
        status = STATUS_SUCCESS;
        if (!ctx->Connected) {
            status = ApolloAudioConnect(ctx);   /* retry a failed connect */
        }
        if (NT_SUCCESS(status)) {
            if (ctx->TransportRunning && ApolloEngineLost(ctx)) {
                /* The recovery from a lost engine: also with streams or ASIO open. */
                status = ApolloEngineRecover(ctx);
            } else {
                status = ApolloTransportStart(ctx);
            }
        }
        break;

    case IOCTL_APOLLO_TRANSPORT_STOP:
        if (ctx->Asio.Open || ApolloStreamsPrepared(ctx)) {
            status = STATUS_DEVICE_BUSY;    /* an ASIO host or a Windows stream is open: close it */
            break;
        }
        ApolloTransportStop(ctx);
        status = STATUS_SUCCESS;
        break;

    case IOCTL_APOLLO_GET_POSITION:
        if (out == NULL || outLen < sizeof(APOLLO_POSITION)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        if (ApolloEngineRunning(ctx) || ApolloEngineLost(ctx)) {
            /* The engine is the only SAMPLE_POS reader: hand out its snapshot (the last good one if lost). */
            PAPOLLO_POSITION p = (PAPOLLO_POSITION)out;
            ULONGLONG pos;
            LONGLONG qpc;
            ULONG raw;
            LARGE_INTEGER freq;

            ApolloPositionGet(ctx, &pos, &qpc, &raw);
            (VOID)KeQueryPerformanceCounter(&freq);
            p->RawSamplePos = raw;
            if (ApolloEngineLost(ctx)) {
                /* The device stopped answering: no more register reads from here either. */
                p->FrameCtr = 0xFFFFFFFF;
                p->AxControl = 0xFFFFFFFF;
                p->Flags = APOLLO_POS_FLAG_ENGINE_LOST | APOLLO_POS_FLAG_DEAD_READ;
            } else {
                p->FrameCtr = ApolloRead(ctx, REG_AX_FRAME_CTR);
                p->AxControl = ApolloRead(ctx, REG_AX_CONTROL);
                p->Flags = (ctx->EngDeadRun != 0) ? APOLLO_POS_FLAG_DEAD_READ : 0;
            }
            p->Position = pos;
            p->QpcTimestamp = qpc;
            p->QpcFrequency = freq.QuadPart;
        } else {
            ApolloPositionUpdate(ctx, (PAPOLLO_POSITION)out);
        }
        info = sizeof(APOLLO_POSITION);
        status = STATUS_SUCCESS;
        break;

    case IOCTL_APOLLO_READ_CAPTURE: {
        APOLLO_CAPTURE_REQUEST req;
        SIZE_T written = 0;

        if (in == NULL || inLen < sizeof(req) || out == NULL) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        req = *(PAPOLLO_CAPTURE_REQUEST)in;     /* before the output overwrites it */
        status = ApolloCaptureCopy(ctx, &req, out, outLen, &written);
        info = written;
        break;
    }
    case IOCTL_APOLLO_WRITE_PLAYBACK: {
        APOLLO_PLAYBACK_RESULT res;

        if (in == NULL || out == NULL || outLen < sizeof(res)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        /* The ring copy reads the input in full before the result is written. */
        status = ApolloPlaybackCopy(ctx, in, inLen, &res);
        if (NT_SUCCESS(status)) {
            *(PAPOLLO_PLAYBACK_RESULT)out = res;
            info = sizeof(res);
        }
        break;
    }
    case IOCTL_APOLLO_SET_INPUT_PARAM: {
        APOLLO_INPUT_PARAM p;

        if (in == NULL || inLen < sizeof(p)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        p = *(PAPOLLO_INPUT_PARAM)in;
        if (ApolloPlanOf(ctx) == NULL || !ApolloPlanOf(ctx)->InputParams) {
            status = STATUS_NOT_SUPPORTED;          /* [G4] a plan without preamp settings */
            break;
        }
        /* [G5] mixer.c refuses what the model's preamp plan does not have (preamp_plan.h) */
        status = ApolloSetInputParam(ctx, &p);
        break;
    }
    case IOCTL_APOLLO_GET_READBACK: {
        PAPOLLO_READBACK rb = (PAPOLLO_READBACK)out;

        if (out == NULL || outLen < sizeof(*rb)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        rb->Valid = ctx->ReadbackValid ? 1 : 0;
        rb->Sequence = ctx->ReadbackSeq;
        RtlCopyMemory(rb->Words, ctx->Readback, sizeof(rb->Words));
        info = sizeof(*rb);
        status = STATUS_SUCCESS;
        break;
    }
    case IOCTL_APOLLO_SET_MONITOR_PARAM: {
        APOLLO_MONITOR_PARAM p;

        if (in == NULL || inLen < sizeof(p)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        p = *(PAPOLLO_MONITOR_PARAM)in;
        if (!ApolloPlanMonitorParamAllowed(ApolloPlanOf(ctx), p.Param)) {
            status = STATUS_NOT_SUPPORTED;          /* [G4] e.g. HP2 on the Twin X (one headphone) */
            break;
        }
        status = ApolloSetMonitorParam(ctx, &p);
        break;
    }
    case IOCTL_APOLLO_GET_LEVELS:
        if (out == NULL || outLen < sizeof(APOLLO_LEVELS)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        ApolloLevelsRead(ctx, (PAPOLLO_LEVELS)out);
        info = sizeof(APOLLO_LEVELS);
        status = STATUS_SUCCESS;
        break;

    case IOCTL_APOLLO_SET_MIXER_SETTING: {
        APOLLO_MIXER_SETTING s;

        if (in == NULL || inLen < sizeof(s)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        s = *(PAPOLLO_MIXER_SETTING)in;
        if (ApolloPlanOf(ctx) == NULL || !ApolloPlanOf(ctx)->RawMixerSettings) {
            status = STATUS_NOT_SUPPORTED;          /* [G4] raw settings: the x8p only */
            break;
        }
        status = ApolloMixerWriteSetting(ctx, s.Index, s.Value, s.Mask);
        break;
    }
    case IOCTL_APOLLO_GET_PANEL:
    case IOCTL_APOLLO_SET_PANEL:
        /* [Clock] the front panel and the monitor link (panel.c); a read-only unit refuses SET_PANEL there */
        status = ApolloPanelIoctl(ctx, IoControlCode, in, inLen, out, outLen, &info);
        break;

    case IOCTL_APOLLO_GET_STRIPS:
        /* Status only: no IOCTL carries strip module bytes (docs/STRIPS.md 4.1). */
        if (out == NULL || outLen < APOLLO_STRIPS_INFO_V2_SIZE) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        if (outLen >= sizeof(APOLLO_STRIPS_INFO)) {
            ApolloStripsFillInfo(ctx, (PAPOLLO_STRIPS_INFO)out);
            info = sizeof(APOLLO_STRIPS_INFO);
        } else {
            /* [Warm] a version 2 caller (older apollo-diag): the fields it knows */
            PAPOLLO_STRIPS_INFO tmp = (PAPOLLO_STRIPS_INFO)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*tmp), APOLLO_POOL_TAG);

            if (tmp == NULL) {
                status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
            ApolloStripsFillInfo(ctx, tmp);
            RtlCopyMemory(out, tmp, APOLLO_STRIPS_INFO_V2_SIZE);
            ExFreePoolWithTag(tmp, APOLLO_POOL_TAG);
            info = APOLLO_STRIPS_INFO_V2_SIZE;
        }
        status = STATUS_SUCCESS;
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfWaitLockRelease(ctx->DevLock);
    /*
     * [G7 review] A transport started (or a lost engine recovered) by hand: a unit that was not in the
     * combined device joins it now (it joined only from the bring-up work item before). DevLock is
     * released first (Agg.Mutex -> DevLock). A member that is already in only needs the tick: the
     * configured master takes its timeline back after 1 s of running (agg.c AggTakeOver).
     */
    if (IoControlCode == IOCTL_APOLLO_TRANSPORT_START && NT_SUCCESS(status)) {
        ApolloAggJoin(ctx);
    }
    WdfRequestCompleteWithInformation(Request, status, info);
}
