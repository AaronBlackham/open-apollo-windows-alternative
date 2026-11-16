/*
 * The few apollo.sys IOCTLs apollo-helper reads (idle-wait, check), copied from
 * include\apollo_ioctl.h (IOCTL version 5 on). ioctl_check.cpp compiles against the payload's own
 * apollo_ioctl.h and fails the build if any code, size or offset used here no longer matches the
 * driver being packaged. All are read-only USER IOCTLs: a normal or elevated GENERIC_READ handle
 * may send them, and a Twin X in observe mode answers them too.
 */
#pragma once
#include <windows.h>
#include <winioctl.h>
#include <stddef.h>

/* {4004c57a-2df0-4638-90d1-cd87d4e8ee6a} = GUID_DEVINTERFACE_APOLLO */
static const GUID HELPER_GUID_DEVINTERFACE_APOLLO =
    { 0x4004c57a, 0x2df0, 0x4638, { 0x90, 0xd1, 0xcd, 0x87, 0xd4, 0xe8, 0xee, 0x6a } };

#define HELPER_FILE_DEVICE_APOLLO   0x8A70u
#define HELPER_APOLLO_CTL(fn) \
    CTL_CODE(HELPER_FILE_DEVICE_APOLLO, 0x800 + (fn), METHOD_BUFFERED, FILE_READ_ACCESS)
#define HELPER_IOCTL_GET_INFO           HELPER_APOLLO_CTL(0x00)
#define HELPER_IOCTL_GET_ENGINE_STATS   HELPER_APOLLO_CTL(0x0F)
#define HELPER_IOCTL_GET_ENDPOINTS      HELPER_APOLLO_CTL(0x10)

/* GET_INFO (APOLLO_INFO). A version 5 driver fills only up to Reserved[3]
   (HELPER_INFO_V5_BYTES); the unit fields are valid when the driver returned
   sizeof(HELPER_INFO) bytes and Version >= 6 (include\apollo_select.h). */
#define HELPER_TYPE_X8P_ID          0x22u
#define HELPER_TYPE_TWINX_ID        0x99u
#define HELPER_INFO_MODE_UNKNOWN    0xFFFFFFFFu
#define HELPER_UNIT_FLAG_RESOLVED   0x01
#define HELPER_UNIT_FLAG_ACTIVE     0x02
#define HELPER_UNIT_FLAG_OBSERVE    0x04
#define HELPER_UNIT_FLAG_REFUSED    0x08

typedef struct _HELPER_INFO {
    ULONG   Version;
    ULONG   Stage;
    ULONG   FailedStage;
    LONG    LastStatus;
    ULONG   Flags;
    ULONG   Bar0Length;
    ULONG   FpgaRev;
    ULONG   ExtCaps;
    ULONG   FwRelease;
    ULONG   SubsystemId;
    ULONG   DeviceType;
    ULONG   PublicType;
    ULONG   NumDsps;
    ULONG   PlayChannels;
    ULONG   RecChannels;
    ULONG   RingFrames;
    ULONG   SampleRate;
    ULONG   DmaCtrl;
    ULONG   NotifRateInfo;
    ULONG   ServicePasses;
    ULONG   InterruptCount;
    ULONG   EndOfBufferCount;
    ULONG   NotifStatusSeen;
    ULONG   NumPreamps;
    CHAR    Serial[17];
    CHAR    Reserved[3];
    /* version 6 */
    ULONG   HwUidLo;
    ULONG   HwUidHi;
    ULONG   Model;
    ULONG   Mode;
    ULONG   UnitFlags;
    ULONG   AggRole;
    ULONG   AggId;
    ULONG   HwidSubsystem;
    CHAR    ModelName[24];
    CHAR    UnitKey[20];
    CHAR    LogTag[8];
    ULONG   OwnerRateInfo;
    ULONG   OwnerClockSrcs;
    ULONG   OwnerDescInVersion;
    ULONG   OwnerDescInCount;
    ULONG   OwnerDescOutVersion;
    ULONG   OwnerDescOutCount;
    ULONG   Reserved6[4];       /* version 7: ClockSrcsLive, BringupSteps, BringupFailStep, BringupApplies */
} HELPER_INFO;

#define HELPER_INFO_V5_BYTES        ((ULONG)offsetof(HELPER_INFO, HwUidLo))

#define HELPER_ASIO_STATE_CLOSED    0
#define HELPER_ASIO_STATE_OPEN      1
#define HELPER_ASIO_STATE_RUNNING   2

typedef struct _HELPER_ENGINE_STATS {
    ULONG     Version;
    ULONG     Running;
    ULONG     RenderLeadFrames;
    ULONG     CaptureGuardFrames;
    ULONG     ZeroAheadFrames;
    ULONG     Starts;
    ULONGLONG Ticks;
    ULONGLONG Position;
    LONGLONG  Qpc;
    LONGLONG  QpcFrequency;
    ULONG     MaxTickIntervalUs;
    ULONG     MaxTickDurationUs;
    ULONG     IntervalsOver2ms;
    ULONG     IntervalsOver4ms;
    ULONG     LateTicks;
    ULONG     BusStalls;
    ULONG     DeadReads;
    ULONG     WrapFixes;
    ULONG     EndOfBufferCount;
    ULONG     AsioState;
    ULONG     AsioBufferFrames;
    ULONG     AsioInputs;
    ULONG     AsioOutputs;
    ULONG     AsioSwitches;
    ULONG     AsioUnderruns;
    ULONG     AsioOverruns;
    ULONG     AsioResyncs;
    ULONG     AsioClientDone;
    ULONG     EngineLost;
    ULONG     AsioClientSkipped;
    ULONG     Reserved[4];
} HELPER_ENGINE_STATS;

#define HELPER_ENDPOINTS_MAX        16
#define HELPER_ENDPOINT_NAME_CHARS  24
#define HELPER_EP_FLAG_PREPARED     0x08
#define HELPER_EP_FLAG_RUNNING      0x10

typedef struct _HELPER_ENDPOINT_INFO {
    CHAR      Name[HELPER_ENDPOINT_NAME_CHARS];
    CHAR      Circuit[HELPER_ENDPOINT_NAME_CHARS];
    ULONG     Render;
    ULONG     Channels;
    ULONG     Slot[2];
    ULONG     Flags;
    ULONG     PacketCount;
    ULONG     PacketBytes;
    ULONG     Streams;
    ULONGLONG Frames;
    ULONGLONG Packets;
    ULONG     Underruns;
    ULONG     Overruns;
    ULONG     Dropped;
    ULONG     LatePackets;
    ULONG     NotifyFailures;
    ULONG     Reserved;
} HELPER_ENDPOINT_INFO;

typedef struct _HELPER_ENDPOINTS {
    ULONG     Version;
    ULONG     Count;
    ULONG     EnabledMask;
    ULONG     Prepared;
    HELPER_ENDPOINT_INFO Endpoint[HELPER_ENDPOINTS_MAX];
} HELPER_ENDPOINTS;
