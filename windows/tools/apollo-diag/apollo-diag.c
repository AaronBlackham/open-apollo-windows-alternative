/*
 * apollo-diag.c - console diagnostics for apollo.sys (plan phases 0 and 1).
 *
 * Talks to the driver through include/apollo_ioctl.h. The device interface
 * is admin-only, so run this from an elevated prompt.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <windows.h>
#include <cfgmgr32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>
#include <errno.h>
#include <bcrypt.h>
#include <sddl.h>
#include <aclapi.h>

#include "..\..\include\apollo_ioctl.h"
#include "..\..\include\apollo_select.h"   /* [Twin X G2] which Apollo: --unit */
#include "..\..\include\apollo_strip_pins.h"
#include "..\..\include\apollo_rate.h"     /* [Rate] rate, rate <Hz> */
#include "..\..\include\apollo_clock.h"    /* [Clock] clock, digital, panel, talkback */
#include "..\..\include\apollo_params.h"   /* [Params] params, param */
#include "..\..\include\apollo_events.h"   /* [Events] events */
#include "..\..\driver\apollo_model.h"     /* model names, the driver's own table */
#include "..\..\driver\bringup_plan.h"     /* [Twin X G3] step names, IO descriptor decoding */
#include "..\..\driver\preamp_plan.h"      /* [Twin X G5] preamps per model: readback decoding, Hi-Z codes */
#include "..\..\driver\clock_plan.h"       /* [Clock] source names, the front panel fields */
#include <devpkey.h>                        /* DEVPKEY_Device_InstanceId (apollo_ioctl.h has initguid.h) */

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#define DIAG_PI             3.14159265358979323846
#define DIAG_MAX_ARGS       64
#define DIAG_MAX_SLOTS      64u
#define DIAG_CHUNK_FRAMES   4096u
#define DIAG_TONE_LEAD_MIN  1024u
#define DIAG_TONE_LEAD_MAX  2048u
#define DIAG_SAFETY_FRAMES  256u    /* the driver answers LATE below lead + 48 = 192 (apollo_ioctl.h) */
#define DIAG_TONE_BUF_MAX   4032u   /* [Rate] the most one write may reach ahead (zero horizon 4096 - 64) */
#define DIAG_FULL_SCALE     2147483648.0
#define DIAG_MAX_24BIT      2147483392.0    /* 0x7FFFFF00 */
#define DIAG_DEFAULT_RATE   48000u
#define DIAG_DEFAULT_RING   8192u
#define DIAG_WAV_HEADER     68u
#define DIAG_WAV_MAX_DATA   0xFFFFFF00ull
#define DIAG_IO_DESC_IN     0xC1A4u /* firmware IO descriptors, 72 words each (driver regs.h) */
#define DIAG_IO_DESC_OUT    0xC2C4u
#define DIAG_MODE1_TONE_MAX 10.0    /* [Twin X G3] seconds of tone on a Mode 1 unit (its speakers) */

static HANDLE g_dev = INVALID_HANDLE_VALUE;
static HANDLE g_timer = NULL;
static volatile LONG g_stop = 0;
static BOOL g_readOnly = FALSE;         /* opened without admin: control IOCTLs only */
/* [Twin X G2] the unit this run talks to (OpenDevice), and whether --unit/--index named it */
static APOLLO_UNIT_ENTRY g_units[APOLLO_SELECT_MAX];
static int g_unitCount, g_unitIndex = -1;
static BOOL g_unitNamed = FALSE;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static BOOL WINAPI CtrlHandler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        InterlockedExchange(&g_stop, 1);
        return TRUE;
    }
    return FALSE;
}

static void PrintLastError(const char *what)
{
    DWORD err = GetLastError();
    char msg[256];
    DWORD len;

    len = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                         NULL, err, 0, msg, (DWORD)sizeof(msg), NULL);
    if (len == 0) {
        msg[0] = 0;
    } else {
        while (len > 0 && (msg[len - 1] == '\r' || msg[len - 1] == '\n' || msg[len - 1] == ' ')) {
            len--;
            msg[len] = 0;
        }
    }
    fprintf(stderr, "%s failed: error %lu (%s)\n", what, err, msg);
}

static void SleepMs(DWORD ms)
{
    if (g_timer != NULL) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)ms * 10000;
        if (SetWaitableTimer(g_timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(g_timer, INFINITE);
            return;
        }
    }
    Sleep(ms);
}

static LONGLONG QpcNow(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static LONGLONG QpcFreq(void)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}

/* Hex with a 0x prefix, otherwise decimal. Negative numbers are rejected. */
static BOOL ParseU32(const char *s, ULONG *out)
{
    char *end = NULL;
    unsigned long v;
    int base = 10;

    if (s == NULL || *s == 0 || *s == '-')
        return FALSE;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        base = 16;
    errno = 0;
    v = strtoul(s, &end, base);
    if (errno == ERANGE || end == s || *end != 0)
        return FALSE;
    *out = (ULONG)v;
    return TRUE;
}

static BOOL ParseDouble(const char *s, double *out)
{
    char *end = NULL;
    double v;

    if (s == NULL || *s == 0)
        return FALSE;
    errno = 0;
    v = strtod(s, &end);
    if (errno == ERANGE || end == s || *end != 0)
        return FALSE;
    *out = v;
    return TRUE;
}

/* "a,b,c" -> out[]. */
static BOOL ParseList(const char *s, ULONG *out, ULONG maxCount, ULONG *count)
{
    char tok[32];
    size_t len = 0;
    ULONG c = 0;
    const char *p = s;

    if (s == NULL)
        return FALSE;
    for (;;) {
        char ch = *p;
        if (ch == ',' || ch == 0) {
            ULONG v = 0;
            if (len == 0)
                return FALSE;
            tok[len] = 0;
            if (!ParseU32(tok, &v) || c >= maxCount)
                return FALSE;
            out[c] = v;
            c++;
            len = 0;
            if (ch == 0)
                break;
        } else {
            if (len + 1 >= sizeof(tok))
                return FALSE;
            tok[len] = ch;
            len++;
        }
        p++;
    }
    *count = c;
    return c > 0;
}

static const char *OptValue(int n, char **a, int from, const char *name)
{
    int i;
    for (i = from; i + 1 < n; i++) {
        if (strcmp(a[i], name) == 0)
            return a[i + 1];
    }
    return NULL;
}

static BOOL HasFlag(int n, char **a, int from, const char *name)
{
    int i;
    for (i = from; i < n; i++) {
        if (strcmp(a[i], name) == 0)
            return TRUE;
    }
    return FALSE;
}

static void PrintDb(double v)
{
    if (v <= 0.0)
        printf("   -inf");
    else
        printf("%7.1f", 20.0 * log10(v / DIAG_FULL_SCALE));
}

/* AX_CONTROL: 0xF = DMA_EN | PLAY_EN | REC_EN | IRQ_EN. */
static BOOL AxRunning(ULONG ax)
{
    return ax != 0xFFFFFFFFul && (ax & 0xFul) == 0xFul;
}

/* ------------------------------------------------------------------ */
/* device access                                                       */
/* ------------------------------------------------------------------ */

/* [Twin X G2] --unit x8p|twinx|<index>, --index <index> */
static const char *UnitWantText(int want)
{
    static char buf[32];

    switch (want) {
    case APOLLO_PICK_DEFAULT: return "the default unit (the x8p, else the first)";
    case APOLLO_PICK_X8P:     return "--unit x8p";
    case APOLLO_PICK_TWINX:   return "--unit twinx";
    default:
        sprintf_s(buf, sizeof(buf), "--unit %d", want);
        return buf;
    }
}

static BOOL ParseUnit(const char *s, int *want)
{
    ULONG v = 0;

    if (_stricmp(s, "x8p") == 0) {
        *want = APOLLO_PICK_X8P;
    } else if (_stricmp(s, "twinx") == 0 || _stricmp(s, "twx") == 0) {
        *want = APOLLO_PICK_TWINX;
    } else if (ParseU32(s, &v) && v < APOLLO_SELECT_MAX) {
        *want = (int)v;
    } else {
        return FALSE;
    }
    return TRUE;
}

/* Lists every Apollo interface once (g_units); FALSE after saying why if there is none. */
static BOOL ListUnits(void)
{
    g_unitCount = ApolloListUnits(g_units, APOLLO_SELECT_MAX, NULL, 0);
    if (g_unitCount < 0) {
        fprintf(stderr, "Could not list the Apollo device interfaces (CM_Get_Device_Interface_List).\n");
        return FALSE;
    }
    if (g_unitCount == 0) {
        fprintf(stderr, "No Apollo found: apollo.sys is not loaded on any device.\n");
        return FALSE;
    }
    return TRUE;
}

/*
 * Default: the x8p (DeviceType 0x22), else the first interface. Every interface is opened
 * read-only for a moment to ask which unit it is; only the chosen one stays open.
 */
static BOOL OpenDevice(int want)
{
    const WCHAR *p;
    int i;

    if (!ListUnits())
        return FALSE;
    g_unitIndex = ApolloPickUnit(g_units, g_unitCount, want);
    if (g_unitIndex < 0) {
        fprintf(stderr, "No Apollo matches %s. Units on apollo.sys (apollo-diag units):\n", UnitWantText(want));
        for (i = 0; i < g_unitCount; i++) {
            const APOLLO_UNIT_ENTRY *e = &g_units[i];
            fprintf(stderr, "  %d  %s%s\n", i,
                    e->Error ? "(did not answer)" : ApolloInfoModelText(&e->Info, e->InfoBytes),
                    e->Error ? "" : (ApolloInfoActive(&e->Info, e->InfoBytes) ? "" : " (observe)"));
        }
        return FALSE;
    }
    p = g_units[g_unitIndex].Path;
    g_dev = CreateFileW(p, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_dev == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED) {
        /* Not elevated: the control set (info, gain, mon, levels ...) still works. */
        g_dev = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_dev != INVALID_HANDLE_VALUE) {
            g_readOnly = TRUE;
        }
    }
    if (g_dev == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            fprintf(stderr, "Access denied: run this from an elevated (Run as "
                            "administrator) prompt.\n");
        } else {
            SetLastError(err);
            PrintLastError("Opening the Apollo device");
        }
        return FALSE;
    }
    /* Which unit, once there is a choice (one x8p alone prints exactly what it always did). */
    if (g_unitCount > 1 || g_unitNamed) {
        const APOLLO_UNIT_ENTRY *e = &g_units[g_unitIndex];
        printf("Unit             %d of %d: %s%s\n", g_unitIndex, g_unitCount,
               e->Error ? "?" : ApolloInfoModelText(&e->Info, e->InfoBytes),
               (e->Error == 0 && !ApolloInfoActive(&e->Info, e->InfoBytes)) ? ", OBSERVE (read-only)" : "");
    }
    return TRUE;
}

static const char *StageName(ULONG s);

/* PnP instance ID of an interface (for matching with Device Manager / Get-PnpDevice). */
static void InterfaceInstanceId(const WCHAR *path, WCHAR *out, ULONG cch)
{
    DEVPROPTYPE type = 0;
    ULONG size = cch * sizeof(WCHAR);

    out[0] = 0;
    if (CM_Get_Device_Interface_PropertyW(path, &DEVPKEY_Device_InstanceId, &type, (PBYTE)out, &size, 0) !=
            CR_SUCCESS || type != DEVPROP_TYPE_STRING) {
        out[0] = 0;
    }
    out[cch - 1] = 0;
}

static const char *ModeText(ULONG mode)
{
    switch (mode) {
    case 0:  return "0 observe";
    case 1:  return "1 transport";
    case 2:  return "2 full";
    case APOLLO_INFO_MODE_UNKNOWN: return "- (not accepted)";
    default: return "?";
    }
}

/* apollo-diag units: every Apollo on apollo.sys, no handle kept. */
static int CmdUnits(void)
{
    int i, pick;

    if (!ListUnits())
        return 2;
    pick = ApolloPickUnit(g_units, g_unitCount, APOLLO_PICK_DEFAULT);
    printf("%-3s %-24s %-16s %-17s %-6s %s\n", "#", "Model", "Serial", "Mode", "Tag", "State");
    for (i = 0; i < g_unitCount; i++) {
        const APOLLO_UNIT_ENTRY *e = &g_units[i];
        const APOLLO_INFO *in = &e->Info;
        WCHAR inst[256];
        char serial[sizeof(in->Serial) + 1];
        BOOL v6 = ApolloInfoHasUnit(in, e->InfoBytes);

        InterfaceInstanceId(e->Path, inst, (ULONG)(sizeof(inst) / sizeof(inst[0])));
        if (e->Error != 0) {
            printf("%-3d (did not answer: error %lu)\n", i, e->Error);
        } else {
            size_t k;

            memcpy(serial, in->Serial, sizeof(in->Serial));
            serial[sizeof(in->Serial)] = 0;
            for (k = 0; serial[k] != 0; k++) {
                if ((unsigned char)serial[k] < 0x20 || (unsigned char)serial[k] > 0x7E)
                    serial[k] = '.';
            }
            printf("%-3d %-24s %-16s %-17s %-6s %s%s\n", i, ApolloInfoModelText(in, e->InfoBytes),
                   serial[0] ? serial : "-", v6 ? ModeText(in->Mode) : "(driver v5)",
                   v6 && in->LogTag[0] && in->LogTag[sizeof(in->LogTag) - 1] == 0 ? in->LogTag : "-",
                   StageName(in->Stage),
                   ApolloInfoActive(in, e->InfoBytes) ? "" : "; OBSERVE: read-only, nothing runs");
            printf("    type 0x%02lX public 0x%02lX subsystem 0x%04lX", in->DeviceType, in->PublicType,
                   in->SubsystemId);
            if (v6)
                printf(" HW_UID 0x%08lX%08lX unit key %s", in->HwUidHi, in->HwUidLo,
                       in->UnitKey[0] && in->UnitKey[sizeof(in->UnitKey) - 1] == 0 ? in->UnitKey : "(none)");
            printf("%s\n", i == pick ? "   <- default" : "");
        }
        printf("    %ls\n", inst[0] ? inst : e->Path);
    }
    return 0;
}

static BOOL Ioctl(DWORD code, const char *name, void *in, DWORD inLen, void *out, DWORD outLen,
                  DWORD *returned)
{
    DWORD r = 0;
    if (!DeviceIoControl(g_dev, code, in, inLen, out, outLen, &r, NULL)) {
        if (GetLastError() == ERROR_ACCESS_DENIED && g_readOnly) {
            fprintf(stderr, "%s needs administrator rights: run this from an elevated prompt.\n",
                    name);
            return FALSE;
        }
        if (GetLastError() == ERROR_BAD_COMMAND) {
            /* STATUS_INVALID_DEVICE_STATE: the unit's Mode does not offer this (driver ioctl.c) */
            fprintf(stderr, "%s: this unit does not offer that in its Mode (observe = read-only; a Twin X in "
                            "Mode 1 answers info, log, engine, pos, levels, readback, capture and tone only; "
                            "in Mode 2 also endpoints and the monitor settings).\n",
                    name);
            return FALSE;
        }
        PrintLastError(name);
        return FALSE;
    }
    if (returned != NULL)
        *returned = r;
    return TRUE;
}

static DWORD g_infoBytes;               /* what the last GET_INFO returned (v5: APOLLO_INFO_V5_BYTES) */

static BOOL GetInfo(APOLLO_INFO *info)
{
    DWORD ret = 0;
    ZeroMemory(info, sizeof(*info));
    if (!Ioctl(IOCTL_APOLLO_GET_INFO, "GET_INFO", NULL, 0, info, (DWORD)sizeof(*info), &ret))
        return FALSE;
    /* [Twin X G2] a version 5 driver returns the shorter structure: fine, the unit fields stay 0 */
    if (ret < APOLLO_INFO_V5_BYTES) {
        fprintf(stderr, "GET_INFO returned %lu bytes, expected at least %lu: driver and tool are out of step.\n",
                ret, (unsigned long)APOLLO_INFO_V5_BYTES);
        return FALSE;
    }
    g_infoBytes = ret;
    /* Versions 5 (release7), 6 (G2) and 7 (G3-G6; 8 added only the plug-in block) are fully supported. */
    if (info->Version != APOLLO_IOCTL_VERSION && info->Version != 5 && info->Version != 6 && info->Version != 7) {
        fprintf(stderr, "warning: driver interface version %lu, this tool expects %d\n",
                info->Version, APOLLO_IOCTL_VERSION);
    }
    return TRUE;
}

static BOOL GetPosition(APOLLO_POSITION *p)
{
    DWORD ret = 0;
    ZeroMemory(p, sizeof(*p));
    if (!Ioctl(IOCTL_APOLLO_GET_POSITION, "GET_POSITION", NULL, 0, p, (DWORD)sizeof(*p), &ret))
        return FALSE;
    if (ret < sizeof(*p)) {
        fprintf(stderr, "GET_POSITION returned %lu bytes, expected %lu\n", ret,
                (unsigned long)sizeof(*p));
        return FALSE;
    }
    return TRUE;
}

/* Quiet: leaves GetLastError for the caller. */
static BOOL ReadReg(ULONG off, ULONG *val)
{
    APOLLO_REG_IO io;
    DWORD ret = 0;

    ZeroMemory(&io, sizeof(io));
    io.Offset = off;
    if (!DeviceIoControl(g_dev, IOCTL_APOLLO_READ_REG, &io, (DWORD)sizeof(io), &io,
                         (DWORD)sizeof(io), &ret, NULL))
        return FALSE;
    if (ret < sizeof(io)) {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    *val = io.Value;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* decoding                                                            */
/* ------------------------------------------------------------------ */

static const char *StageName(ULONG s)
{
    switch (s) {
    case ApolloStageNone:           return "None";
    case ApolloStageMapped:         return "Mapped (BAR0 mapped, quiesced, identified)";
    case ApolloStageRingsReady:     return "RingsReady (DSP rings and SG tables programmed)";
    case ApolloStageRingConnected:  return "RingConnected (DSP ring connect sent)";
    case ApolloStageAcefaceDone:    return "AcefaceDone (audio extension handshake answered)";
    case ApolloStageConnected:      return "Connected (rate ack and mixer init done)";
    case ApolloStageFailed:         return "FAILED";
    case ApolloStageGone:           return "GONE (device removed)";
    default:                        return "unknown";
    }
}

static const char *ModelName(ULONG t)
{
    return ApolloModelName(t);      /* driver\apollo_model.h: the names the driver logs */
}

static const struct {
    ULONG bit;
    const char *name;
} kFlags[] = {
    { APOLLO_FLAG_FW_V2,           "FW_V2" },
    { APOLLO_FLAG_IRQ64,           "IRQ64" },
    { APOLLO_FLAG_WARM_START,      "WARM_START" },
    { APOLLO_FLAG_RINGS_LIVE,      "RINGS_LIVE" },
    { APOLLO_FLAG_DSPS_CONNECTED,  "DSPS_CONNECTED" },
    { APOLLO_FLAG_ACEFACE_DONE,    "ACEFACE_DONE" },
    { APOLLO_FLAG_CONNECTED,       "CONNECTED" },
    { APOLLO_FLAG_MIXER_READY,     "MIXER_READY" },
    { APOLLO_FLAG_TRANSPORT,       "TRANSPORT" },
    { APOLLO_FLAG_SERVICE_RUNNING, "SERVICE_RUNNING" },
    { APOLLO_FLAG_READBACK_VALID,  "READBACK_VALID" },
    { APOLLO_FLAG_MSI,             "MSI" },
    { APOLLO_FLAG_FW_UNTESTED,     "FW_UNTESTED" },
    { APOLLO_FLAG_ENGINE_LOST,     "ENGINE_LOST" },
};

static void PrintSafeString(const CHAR *s, size_t max)
{
    size_t i;
    for (i = 0; i < max && s[i] != 0; i++) {
        unsigned char c = (unsigned char)s[i];
        putchar((c >= 0x20 && c <= 0x7E) ? (int)c : '.');
    }
}

/* ------------------------------------------------------------------ */
/* commands                                                            */
/* ------------------------------------------------------------------ */

static int CmdInfo(void)
{
    APOLLO_INFO in;
    size_t i;
    ULONG known = 0;

    if (!GetInfo(&in))
        return 1;

    printf("Version          %lu\n", in.Version);
    printf("Stage            %lu  %s\n", in.Stage, StageName(in.Stage));
    printf("FailedStage      %lu  %s\n", in.FailedStage, StageName(in.FailedStage));
    printf("LastStatus       0x%08lX\n", (ULONG)in.LastStatus);
    printf("Flags            0x%08lX ", in.Flags);
    for (i = 0; i < sizeof(kFlags) / sizeof(kFlags[0]); i++) {
        known |= kFlags[i].bit;
        if (in.Flags & kFlags[i].bit)
            printf(" %s", kFlags[i].name);
    }
    if (in.Flags & ~known)
        printf(" (unknown bits 0x%08lX)", in.Flags & ~known);
    printf("\n");
    printf("Bar0Length       0x%lX (%lu bytes)\n", in.Bar0Length, in.Bar0Length);
    printf("FpgaRev          0x%08lX\n", in.FpgaRev);
    printf("ExtCaps          0x%08lX  (audio ext %lu, family %lu, DSPs %lu, ready cap %lu)\n",
           in.ExtCaps, (in.ExtCaps >> 31) & 1ul, (in.ExtCaps >> 20) & 0x3Ful,
           (in.ExtCaps >> 8) & 0xFFul, (in.ExtCaps >> 4) & 0xFul);
    printf("FwRelease        0x%08lX  (%lu.%lu.%lu)\n", in.FwRelease, (in.FwRelease >> 24) & 0xFFul,
           (in.FwRelease >> 16) & 0xFFul, in.FwRelease & 0xFFFFul);
    printf("SubsystemId      0x%04lX\n", in.SubsystemId);
    printf("DeviceType       0x%02lX  %s\n", in.DeviceType, ModelName(in.DeviceType));
    printf("PublicType       0x%02lX\n", in.PublicType);
    printf("NumDsps          %lu\n", in.NumDsps);
    printf("PlayChannels     %lu\n", in.PlayChannels);
    printf("RecChannels      %lu\n", in.RecChannels);
    printf("RingFrames       %lu\n", in.RingFrames);
    printf("SampleRate       %lu\n", in.SampleRate);
    printf("DmaCtrl          0x%08lX\n", in.DmaCtrl);
    printf("NotifRateInfo    0x%08lX  (rate index %lu, lock %lu)\n", in.NotifRateInfo,
           (in.NotifRateInfo >> 8) & 0xFFul, in.NotifRateInfo & 0x3ul);
    printf("ServicePasses    %lu\n", in.ServicePasses);
    printf("InterruptCount   %lu\n", in.InterruptCount);
    printf("EndOfBufferCount %lu\n", in.EndOfBufferCount);
    printf("NotifStatusSeen  0x%08lX\n", in.NotifStatusSeen);
    printf("NumPreamps       %lu\n", in.NumPreamps);
    printf("Serial           \"");
    PrintSafeString(in.Serial, 16);
    printf("\"\n");

    /* [Twin X G2] version 6: which unit, its Mode, and what the previous owner left */
    if (!ApolloInfoHasUnit(&in, g_infoBytes)) {
        printf("(driver interface version %lu: no unit fields; DeviceType above says which unit)\n", in.Version);
        return 0;
    }
    printf("Model            %lu  %s\n", in.Model, in.ModelName[sizeof(in.ModelName) - 1] == 0 ? in.ModelName : "?");
    printf("Mode             %s\n", ModeText(in.Mode));
    printf("UnitFlags        0x%02lX %s%s%s%s%s%s%s\n", in.UnitFlags,
           (in.UnitFlags & APOLLO_UNIT_FLAG_RESOLVED) ? " RESOLVED" : "",
           (in.UnitFlags & APOLLO_UNIT_FLAG_ACTIVE) ? " ACTIVE" : "",
           (in.UnitFlags & APOLLO_UNIT_FLAG_OBSERVE) ? " OBSERVE (read-only: nothing runs on this unit)" : "",
           (in.UnitFlags & APOLLO_UNIT_FLAG_REFUSED) ? " REFUSED" : "",
           (in.UnitFlags & APOLLO_UNIT_FLAG_OWNER_READ) ? " OWNER_READ" : "",
           (in.UnitFlags & APOLLO_UNIT_FLAG_UNIT_KEY) ? " UNIT_KEY" : "",
           (in.UnitFlags & APOLLO_UNIT_FLAG_MODEL_KEY) ? " MODEL_KEY" : "");
    printf("HwUid            0x%08lX%08lX\n", in.HwUidHi, in.HwUidLo);
    printf("UnitKey          %s\n", (in.UnitKey[0] && in.UnitKey[sizeof(in.UnitKey) - 1] == 0) ?
                                    in.UnitKey : "(none: Models and built-in values only)");
    printf("LogTag           %s\n", in.LogTag[sizeof(in.LogTag) - 1] == 0 ? in.LogTag : "?");
    printf("HwidSubsystem    0x%04lX  (from the PnP hardware ID at device add)\n", in.HwidSubsystem);
    printf("AggRole          %lu  (standalone)\n", in.AggRole);
    if (in.UnitFlags & APOLLO_UNIT_FLAG_OWNER_READ) {
        printf("--- STALE: as the previous owner (UA's driver) left them; nothing of ours runs on this unit ---\n");
        printf("OwnerRateInfo    0x%08lX  (rate index %lu, lock %lu)\n", in.OwnerRateInfo,
               (in.OwnerRateInfo >> 8) & 0xFFul, in.OwnerRateInfo & 0x3ul);
        printf("OwnerClockSrcs   0x%08lX  (sources 0x%04lX, rates 0x%04lX)\n", in.OwnerClockSrcs,
               in.OwnerClockSrcs >> 16, in.OwnerClockSrcs & 0xFFFFul);
        printf("OwnerDescIn      v%lu, %lu record channels\n", in.OwnerDescInVersion, in.OwnerDescInCount);
        printf("OwnerDescOut     v%lu, %lu playback channels\n", in.OwnerDescOutVersion, in.OwnerDescOutCount);
        printf("(every descriptor word: apollo-diag log, lines \"STALE desc\")\n");
    }
    /* [Twin X G3] version 7: the AudioExtension bring-up (all zero on the x8p and observe units) */
    if (in.Version >= 7 && (in.BringupSteps != 0 || in.BringupFailStep != 0 || in.ClockSrcsLive != 0)) {
        ULONG k;

        printf("--- G3 bring-up (Twin X Gen 1 path, docs/spec-twinx.md 6.9.3) ---\n");
        printf("ClockSrcsLive    0x%08lX  (sources 0x%04lX:%s%s%s%s%s; rates 0x%02lX)\n", in.ClockSrcsLive,
               in.ClockSrcsLive >> 16,
               ApolloClockOffers(in.ClockSrcsLive, APOLLO_CLKSRC_SPDIF) ? " S/PDIF" : "",
               ApolloClockOffers(in.ClockSrcsLive, APOLLO_CLKSRC_ADAT) ? " ADAT" : "",
               ApolloClockOffers(in.ClockSrcsLive, APOLLO_CLKSRC_WORDCLOCK) ? " WordClock" : "",
               ApolloClockOffers(in.ClockSrcsLive, APOLLO_CLKSRC_PEER) ? " Peer(0x0A)" : "",
               ApolloClockOffers(in.ClockSrcsLive, APOLLO_CLKSRC_INTERNAL) ? " Internal" : "",
               in.ClockSrcsLive & 0xFFul);
        printf("BringupSteps     0x%05lX\n", in.BringupSteps);
        /* [G4] BringupApplies (the former Reserved7): steps outside this start's path are n/a; 0 = a G3 driver */
        if (in.BringupApplies != 0) {
            printf("BringupApplies   0x%05lX  (the steps this start's path runs; the others are n/a)\n",
                   in.BringupApplies);
        }
        for (k = 1; k < APOLLO_BSTEP_COUNT; k++) {
            const char *st = (in.BringupSteps & (1ul << k)) ? "ok" : "--";

            if (in.BringupApplies != 0 && (in.BringupApplies & (1ul << k)) == 0 && (in.BringupSteps & (1ul << k)) == 0) {
                st = "n/a";
            }
            printf("  step %2lu  %-4s %s\n", k, st, ApolloBstepName(k));
        }
        printf("BringupFailStep  %lu  %s\n", in.BringupFailStep,
               in.BringupFailStep ? ApolloBstepName(in.BringupFailStep) : "(none)");
    }
    return 0;
}

static int CmdRead(int n, char **a)
{
    ULONG off = 0, v = 0;
    if (n < 2 || !ParseU32(a[1], &off)) {
        fprintf(stderr, "usage: read <offset>\n");
        return 1;
    }
    if (!ReadReg(off, &v)) {
        PrintLastError("READ_REG");
        return 1;
    }
    printf("0x%04lX = 0x%08lX (%lu)\n", off, v, v);
    return 0;
}

static int CmdWrite(int n, char **a)
{
    APOLLO_REG_IO io;
    ULONG off = 0, v = 0;

    if (n < 3 || !ParseU32(a[1], &off) || !ParseU32(a[2], &v)) {
        fprintf(stderr, "usage: write <offset> <value> --yes\n");
        return 1;
    }
    if (!HasFlag(n, a, 3, "--yes")) {
        fprintf(stderr, "Raw register writes can wedge the Apollo until a power cycle.\n"
                        "Repeat with --yes to write 0x%08lX to 0x%04lX.\n", v, off);
        return 1;
    }
    ZeroMemory(&io, sizeof(io));
    io.Offset = off;
    io.Value = v;
    if (!Ioctl(IOCTL_APOLLO_WRITE_REG, "WRITE_REG", &io, (DWORD)sizeof(io), NULL, 0, NULL))
        return 1;
    printf("wrote 0x%08lX to 0x%04lX\n", v, off);
    return 0;
}

static void DumpReg(ULONG off, const char *name)
{
    ULONG v = 0;
    if (ReadReg(off, &v))
        printf("  0x%04lX  %-18s 0x%08lX\n", off, name, v);
    else
        printf("  0x%04lX  %-18s <read failed: error %lu>\n", off, name, GetLastError());
}

static const struct {
    ULONG off;
    const char *name;
} kDumpRegs[] = {
    { 0x0000, "DSP0_STATUS" },
    { 0x0004, "ID_WORD" },
    { 0x0008, "FW_RELEASE" },
    { 0x2200, "DMA_CTRL" },
    { 0x2204, "IMR" },
    { 0x2208, "ISR" },
    { 0x2218, "FPGA_REV" },
    { 0x2220, "TRANSPORT" },
    { 0x2234, "EXT_CAPS" },
    { 0x2238, "SERIAL_REG" },
    { 0x2240, "AX_BUF_SIZE" },
    { 0x2244, "AX_SAMPLE_POS" },
    { 0x2248, "AX_CONTROL" },
    { 0x224C, "AX_P2P_ROUTE" },
    { 0x2250, "AX_PLAY_CH" },
    { 0x2254, "AX_FRAME_CTR" },
    { 0x2258, "AX_FRAME_CFG" },
    { 0x225C, "AX_REC_CH" },
    { 0x2260, "AX_CONNECT" },
    { 0x2264, "ISR_HI" },
    { 0x2268, "IMR_HI" },
    { 0x226C, "AX_DMA_BLKSZ" },
    { 0x2270, "AX_TIMER_CFG" },
    { 0x22C0, "AX_STATUS" },
    { 0x3808, "MIXER_SEQ_WR" },
    { 0x380C, "MIXER_SEQ_RD" },
    { 0x3810, "MIXER_RB_STATUS" },
    { 0xC030, "NOTIF_STATUS" },
    { 0xC07C, "NOTIF_RATE_INFO" },
    { 0xC080, "NOTIF_XPORT_INFO" },
    { 0xC084, "NOTIF_CLOCK_INFO" },
    { 0xC08C, "NOTIF_CLOCK_SRCS" },
};

static int CmdDump(void)
{
    APOLLO_INFO info;
    ULONG serial[4];
    char text[17];
    ULONG i, d, dsps;
    size_t r;
    BOOL serialOk = TRUE;

    if (!GetInfo(&info))
        return 1;

    printf("Registers:\n");
    for (r = 0; r < sizeof(kDumpRegs) / sizeof(kDumpRegs[0]); r++) {
        DumpReg(kDumpRegs[r].off, kDumpRegs[r].name);
        if (kDumpRegs[r].off == 0x0008) {
            for (i = 0; i < 4; i++) {
                serial[i] = 0;
                if (!ReadReg(0x20 + i * 4, &serial[i]))
                    serialOk = FALSE;
            }
            for (i = 0; i < 16; i++) {
                unsigned char c = (unsigned char)((serial[i / 4] >> ((i % 4) * 8)) & 0xFF);
                text[i] = (char)((c >= 0x20 && c <= 0x7E) ? c : '.');
            }
            text[16] = 0;
            printf("  0x0020  %-18s %08lX %08lX %08lX %08lX  \"%s\"%s\n", "SERIAL (ASCII)",
                   serial[0], serial[1], serial[2], serial[3], text,
                   serialOk ? "" : "  <some reads failed>");
        }
    }

    dsps = info.NumDsps > 8 ? 8 : info.NumDsps;
    printf("\nDSP rings (%lu DSPs):\n", dsps);
    printf("  DSP  ring  base    WRITE_PTR  DOORBELL   POSITION\n");
    for (d = 0; d < dsps; d++) {
        ULONG bank = (d < 4) ? (0x2000 + d * 0x80) : (0x5E00 + d * 0x80);
        for (i = 0; i < 2; i++) {
            ULONG base = bank + i * 0x40;
            ULONG wp = 0, db = 0, ps = 0;
            BOOL ok = ReadReg(base + 0x20, &wp) && ReadReg(base + 0x24, &db) &&
                      ReadReg(base + 0x28, &ps);
            if (ok)
                printf("  %3lu  %-4s  0x%04lX  0x%08lX 0x%08lX 0x%08lX\n", d, i ? "resp" : "cmd",
                       base, wp, db, ps);
            else
                printf("  %3lu  %-4s  0x%04lX  <read failed: error %lu>\n", d, i ? "resp" : "cmd",
                       base, GetLastError());
        }
    }

    printf("\nDSP windows:\n");
    printf("  DSP  window  +0x000 status  +0x1A4 boot/ready\n");
    for (d = 0; d < dsps; d++) {
        ULONG win = d * 0x800 + (d > 3 ? 0x2000 : 0);
        ULONG st = 0, rd = 0;
        if (ReadReg(win, &st) && ReadReg(win + 0x1A4, &rd))
            printf("  %3lu  0x%04lX  0x%08lX     0x%08lX%s\n", d, win, st, rd,
                   (rd == 0 || rd == 0xA8CAED0Ful) ? "  (not started)" : "");
        else
            printf("  %3lu  0x%04lX  <read failed: error %lu>\n", d, win, GetLastError());
    }
    return 0;
}

static int CmdTransport(BOOL start)
{
    DWORD code = start ? IOCTL_APOLLO_TRANSPORT_START : IOCTL_APOLLO_TRANSPORT_STOP;
    if (!Ioctl(code, start ? "TRANSPORT_START" : "TRANSPORT_STOP", NULL, 0, NULL, 0, NULL))
        return 1;
    printf("transport %s\n", start ? "started" : "stopped");
    return 0;
}

static int CmdPos(int n, char **a)
{
    APOLLO_POSITION first, prev, cur;
    double seconds = 3.0;
    LONGLONG freq = 1, endQpc = 0, nextReport = 0, wallStart = 0;
    ULONG polls = 0, same = 0, stalls = 0, backs = 0;
    BOOL inStall = FALSE;
    double elapsed = 0.0, rate = 0.0;

    if (n >= 2 && (!ParseDouble(a[1], &seconds) || seconds <= 0.0)) {
        fprintf(stderr, "usage: pos [seconds]\n");
        return 1;
    }
    if (!GetPosition(&first))
        return 1;
    freq = first.QpcFrequency > 0 ? first.QpcFrequency : QpcFreq();
    printf("start: pos %llu  raw %lu  frame_ctr %lu  ax_control 0x%03lX\n", first.Position,
           first.RawSamplePos, first.FrameCtr, first.AxControl);
    if (!AxRunning(first.AxControl))
        printf("note: the transport is not running (AX_CONTROL 0x%03lX); the position will not "
               "move. Run 'start' first.\n", first.AxControl);

    if (first.Flags & APOLLO_POS_FLAG_ENGINE_LOST)
        printf("note: the driver's engine gave up on the device (not answering); the position is "
               "frozen. Restart the transport (stop, start) or replug the x8p.\n");
    prev = first;
    /* The loop ends on this program's clock: a dead device freezes the driver's timestamp. */
    wallStart = QpcNow();
    endQpc = wallStart + (LONGLONG)(seconds * (double)QpcFreq());
    nextReport = first.QpcTimestamp + freq / 2;
    while (!g_stop) {
        SleepMs(10);
        if (!GetPosition(&cur))
            return 1;
        polls++;
        if ((cur.Flags & (APOLLO_POS_FLAG_DEAD_READ | APOLLO_POS_FLAG_ENGINE_LOST)) &&
            !(prev.Flags & (APOLLO_POS_FLAG_DEAD_READ | APOLLO_POS_FLAG_ENGINE_LOST)))
            printf("DEAD: SAMPLE_POS reads all ones%s\n",
                   (cur.Flags & APOLLO_POS_FLAG_ENGINE_LOST) ? " and the engine stopped touching the device"
                                                             : "");
        if (cur.Position < prev.Position) {
            backs++;
            printf("BACKWARDS: %llu -> %llu (raw %lu -> %lu)\n", prev.Position, cur.Position,
                   prev.RawSamplePos, cur.RawSamplePos);
        }
        if (cur.Position == prev.Position && AxRunning(cur.AxControl)) {
            same++;
            if (same >= 2 && !inStall) {
                stalls++;
                inStall = TRUE;
                printf("STALL: position %llu unchanged for 3 polls (raw %lu, frame_ctr %lu, "
                       "ax_control 0x%03lX)\n", cur.Position, cur.RawSamplePos, cur.FrameCtr,
                       cur.AxControl);
            }
        } else {
            same = 0;
            inStall = FALSE;
        }
        if (cur.QpcTimestamp >= nextReport && cur.QpcTimestamp > first.QpcTimestamp) {
            elapsed = (double)(cur.QpcTimestamp - first.QpcTimestamp) / (double)freq;
            rate = (double)(LONGLONG)(cur.Position - first.Position) / elapsed;
            printf("t=%6.2fs  pos %llu  raw %4lu  rate %9.2f frames/s  ax_control 0x%03lX\n",
                   elapsed, cur.Position, cur.RawSamplePos, rate, cur.AxControl);
            nextReport += freq / 2;
        }
        prev = cur;
        if (QpcNow() >= endQpc)
            break;
    }

    if (prev.QpcTimestamp > first.QpcTimestamp) {
        elapsed = (double)(prev.QpcTimestamp - first.QpcTimestamp) / (double)freq;
        rate = (double)(LONGLONG)(prev.Position - first.Position) / elapsed;
        printf("summary: %lu polls over %.2f s, %lld frames, measured rate %.2f frames/s, "
               "%lu stall(s), %lu backwards jump(s)\n", polls, elapsed,
               (LONGLONG)(prev.Position - first.Position), rate, stalls, backs);
    }
    return (stalls || backs) ? 1 : 0;
}

static void Put16(unsigned char *b, size_t *o, unsigned v)
{
    b[*o] = (unsigned char)(v & 0xFF);
    b[*o + 1] = (unsigned char)((v >> 8) & 0xFF);
    *o += 2;
}

static void Put32(unsigned char *b, size_t *o, ULONG v)
{
    b[*o] = (unsigned char)(v & 0xFF);
    b[*o + 1] = (unsigned char)((v >> 8) & 0xFF);
    b[*o + 2] = (unsigned char)((v >> 16) & 0xFF);
    b[*o + 3] = (unsigned char)((v >> 24) & 0xFF);
    *o += 4;
}

static void PutTag(unsigned char *b, size_t *o, const char *tag)
{
    memcpy(b + *o, tag, 4);
    *o += 4;
}

/* WAVE_FORMAT_EXTENSIBLE, 32-bit container, 24 valid bits, PCM subformat. */
static BOOL WavWriteHeader(FILE *f, ULONG channels, ULONG rate, ULONG dataBytes)
{
    static const unsigned char kPcmGuid[16] = {
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
        0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71
    };
    unsigned char h[DIAG_WAV_HEADER];
    size_t o = 0;

    PutTag(h, &o, "RIFF");
    Put32(h, &o, (ULONG)(DIAG_WAV_HEADER - 8u) + dataBytes);
    PutTag(h, &o, "WAVE");
    PutTag(h, &o, "fmt ");
    Put32(h, &o, 40);
    Put16(h, &o, 0xFFFE);                       /* WAVE_FORMAT_EXTENSIBLE */
    Put16(h, &o, (unsigned)channels);
    Put32(h, &o, rate);
    Put32(h, &o, rate * channels * 4);          /* nAvgBytesPerSec */
    Put16(h, &o, (unsigned)(channels * 4));     /* nBlockAlign */
    Put16(h, &o, 32);                           /* wBitsPerSample */
    Put16(h, &o, 22);                           /* cbSize */
    Put16(h, &o, 24);                           /* wValidBitsPerSample */
    Put32(h, &o, 0);                            /* dwChannelMask: no speaker mapping */
    memcpy(h + o, kPcmGuid, sizeof(kPcmGuid));  /* KSDATAFORMAT_SUBTYPE_PCM */
    o += sizeof(kPcmGuid);
    PutTag(h, &o, "data");
    Put32(h, &o, dataBytes);

    if (fseek(f, 0, SEEK_SET) != 0)
        return FALSE;
    return fwrite(h, 1, o, f) == o;
}

static int CmdCapture(int n, char **a)
{
    APOLLO_INFO info;
    APOLLO_POSITION pos;
    APOLLO_CAPTURE_REQUEST req;
    PAPOLLO_CAPTURE_HEADER hdr = NULL;
    double seconds = 0.0;
    const char *path;
    const char *chanOpt;
    ULONG C, rate, nsel = 0, k, overruns = 0;
    ULONG *sel = NULL;
    double *peak = NULL, *sumsq = NULL;
    unsigned char *buf = NULL;
    LONG *data = NULL, *ob = NULL;
    FILE *f = NULL;
    ULONGLONG next = 0, wanted = 0, maxFrames = 0, captured = 0, dropped = 0, dataBytes = 0;
    LONGLONG freq = 1, lastProgress = 0;
    int rc = 1;

    if (n < 3 || !ParseDouble(a[1], &seconds) || seconds <= 0.0) {
        fprintf(stderr, "usage: capture <seconds> <out.wav> [--channels a,b,c]\n");
        return 1;
    }
    path = a[2];
    if (!GetInfo(&info))
        return 1;
    C = info.RecChannels;
    rate = info.SampleRate ? info.SampleRate : DIAG_DEFAULT_RATE;
    if (C == 0) {
        fprintf(stderr, "The driver reports 0 record channels (stage: %s).\n", StageName(info.Stage));
        return 1;
    }
    if (!(info.Flags & APOLLO_FLAG_TRANSPORT)) {
        fprintf(stderr, "The transport is not running. Run 'apollo-diag start' first.\n");
        return 1;
    }

    sel = (ULONG *)calloc(C, sizeof(ULONG));
    if (sel == NULL)
        goto oom;
    chanOpt = OptValue(n, a, 3, "--channels");
    if (chanOpt != NULL) {
        if (!ParseList(chanOpt, sel, C, &nsel)) {
            fprintf(stderr, "--channels: expected a comma list of up to %lu slot numbers\n", C);
            goto out;
        }
        for (k = 0; k < nsel; k++) {
            if (sel[k] >= C) {
                fprintf(stderr, "slot %lu is out of range: the record ring has %lu slots (0..%lu)\n",
                        sel[k], C, C - 1);
                goto out;
            }
        }
    } else {
        for (k = 0; k < C; k++)
            sel[k] = k;
        nsel = C;
    }

    maxFrames = (DIAG_WAV_MAX_DATA - DIAG_WAV_HEADER) / ((ULONGLONG)nsel * 4u);
    wanted = (ULONGLONG)(seconds * (double)rate + 0.5);
    if (wanted == 0)
        wanted = 1;
    if (wanted > maxFrames) {
        printf("note: a WAV file holds at most %llu frames of %lu slots; capturing that much.\n",
               maxFrames, nsel);
        wanted = maxFrames;
    }

    peak = (double *)calloc(nsel, sizeof(double));
    sumsq = (double *)calloc(nsel, sizeof(double));
    buf = (unsigned char *)malloc(sizeof(APOLLO_CAPTURE_HEADER) +
                                  (size_t)DIAG_CHUNK_FRAMES * C * sizeof(LONG));
    ob = (LONG *)malloc((size_t)DIAG_CHUNK_FRAMES * nsel * sizeof(LONG));
    if (peak == NULL || sumsq == NULL || buf == NULL || ob == NULL)
        goto oom;
    hdr = (PAPOLLO_CAPTURE_HEADER)buf;
    data = (LONG *)(buf + sizeof(APOLLO_CAPTURE_HEADER));

    if (fopen_s(&f, path, "wb") != 0 || f == NULL) {
        fprintf(stderr, "Cannot create %s\n", path);
        f = NULL;
        goto out;
    }
    if (!WavWriteHeader(f, nsel, rate, 0)) {
        fprintf(stderr, "Cannot write %s\n", path);
        goto out;
    }

    if (!GetPosition(&pos))
        goto out;
    next = pos.Position;
    freq = pos.QpcFrequency > 0 ? pos.QpcFrequency : QpcFreq();
    lastProgress = QpcNow();
    printf("Capturing %llu frames (%.2f s) of %lu slot(s) at %lu Hz from position %llu. "
           "Ctrl+C stops early.\n", wanted, (double)wanted / (double)rate, nsel, rate, next);

    while (captured < wanted && !g_stop) {
        DWORD ret = 0, outLen;
        ULONGLONG left = wanted - captured;
        ULONG frames, i;

        ZeroMemory(&req, sizeof(req));
        req.StartPosition = next;
        req.MaxFrames = (ULONG)(left < DIAG_CHUNK_FRAMES ? left : DIAG_CHUNK_FRAMES);
        outLen = (DWORD)(sizeof(APOLLO_CAPTURE_HEADER) + (size_t)req.MaxFrames * C * sizeof(LONG));
        if (!DeviceIoControl(g_dev, IOCTL_APOLLO_READ_CAPTURE, &req, (DWORD)sizeof(req), buf,
                             outLen, &ret, NULL)) {
            PrintLastError("READ_CAPTURE");
            break;
        }
        if (ret < sizeof(APOLLO_CAPTURE_HEADER)) {
            fprintf(stderr, "READ_CAPTURE returned %lu bytes, less than its header\n", ret);
            break;
        }
        if (hdr->Channels != C) {
            fprintf(stderr, "READ_CAPTURE reports %lu channels, the driver info said %lu: layout "
                            "changed, stopping\n", hdr->Channels, C);
            break;
        }
        frames = hdr->Frames;
        if (frames > req.MaxFrames ||
            ret < sizeof(APOLLO_CAPTURE_HEADER) + (size_t)frames * C * sizeof(LONG)) {
            fprintf(stderr, "READ_CAPTURE returned %lu frames in %lu bytes: inconsistent, "
                            "stopping\n", frames, ret);
            break;
        }

        if (hdr->Flags & APOLLO_CAPTURE_OVERRUN) {
            overruns++;
            if (frames == 0) {
                ULONGLONG lost = hdr->HwPosition > next ? hdr->HwPosition - next : 0;
                printf("OVERRUN at frame %llu: the ring moved on, %llu frames lost; continuing "
                       "from %llu\n", next, lost, hdr->HwPosition);
                dropped += lost;
                next = hdr->HwPosition;
                if (lost == 0)
                    SleepMs(20);
                continue;
            }
            printf("OVERRUN at frame %llu: continuing from %llu\n", next, hdr->StartPosition);
        }

        if (frames > 0) {
            if (hdr->StartPosition > next)
                dropped += hdr->StartPosition - next;
            for (i = 0; i < frames; i++) {
                const LONG *src = data + (size_t)i * C;
                LONG *dst = ob + (size_t)i * nsel;
                for (k = 0; k < nsel; k++) {
                    LONG s = src[sel[k]];
                    double d = fabs((double)s);
                    dst[k] = s;
                    if (d > peak[k])
                        peak[k] = d;
                    sumsq[k] += d * d;
                }
            }
            if (fwrite(ob, sizeof(LONG), (size_t)frames * nsel, f) != (size_t)frames * nsel) {
                fprintf(stderr, "Write to %s failed (disk full?)\n", path);
                break;
            }
            dataBytes += (ULONGLONG)frames * nsel * sizeof(LONG);
            captured += frames;
            next = hdr->StartPosition + frames;
            lastProgress = QpcNow();
        }

        if (frames < req.MaxFrames) {
            if (QpcNow() - lastProgress > 2 * freq) {
                fprintf(stderr, "No new frames for 2 s: the transport looks stalled (try 'pos').\n");
                break;
            }
            SleepMs(20);
        }
    }

    if (!WavWriteHeader(f, nsel, rate, (ULONG)dataBytes))
        fprintf(stderr, "warning: could not finalise the WAV header of %s\n", path);
    fclose(f);
    f = NULL;

    printf("\nSaved %s: %llu frames (%.2f s), %lu slot(s), %lu Hz, 32-bit container, 24 valid bits\n",
           path, captured, (double)captured / (double)rate, nsel, rate);
    if (overruns || dropped)
        printf("Overruns: %lu, frames lost: %llu\n", overruns, dropped);
    printf("slot   peak dBFS   rms dBFS\n");
    for (k = 0; k < nsel; k++) {
        double rms = captured ? sqrt(sumsq[k] / (double)captured) : 0.0;
        printf("%4lu    ", sel[k]);
        PrintDb(peak[k]);
        printf("    ");
        PrintDb(rms);
        if (peak[k] == 0.0)
            printf("    silent (every sample is 0)");
        printf("\n");
    }
    printf("(x8p legacy layout: slot N is physical input N+1)\n");
    rc = (captured > 0) ? 0 : 1;
    goto out;

oom:
    fprintf(stderr, "Out of memory\n");
out:
    if (f != NULL)
        fclose(f);
    free(ob);
    free(buf);
    free(sumsq);
    free(peak);
    free(sel);
    return rc;
}

/*
 * [Twin X G3] MON L/R from the unit's own playback IO descriptor (READ_REG, administrators):
 * the FPGA lays the frame out in descriptor order, so these are the speaker slots.
 */
static BOOL DescMonSlots(LONG *L, LONG *R, ULONG *Count)
{
    ULONG w[APOLLO_IODESC_WORDS], i;

    for (i = 0; i < APOLLO_IODESC_WORDS; i++) {
        if (!ReadReg(DIAG_IO_DESC_OUT + i * 4, &w[i])) {
            if (GetLastError() == ERROR_ACCESS_DENIED)
                fprintf(stderr, "Reading the IO descriptor needs administrator rights: run this elevated.\n");
            else
                PrintLastError("READ_REG (playback IO descriptor)");
            return FALSE;
        }
    }
    *Count = ApolloIoDescCount(w, 0);
    if (*Count == 0) {
        fprintf(stderr, "The playback IO descriptor is not usable (header %08lX %08lX %08lX %08lX %08lX %08lX): "
                        "give --slots by hand.\n", w[0], w[1], w[2], w[3], w[4], w[5]);
        return FALSE;
    }
    *L = ApolloIoDescFind(w, 0, APOLLO_CHAN_MON_L);
    *R = ApolloIoDescFind(w, 0, APOLLO_CHAN_MON_R);
    if (*L < 0 || *R < 0) {
        fprintf(stderr, "No MON L/R in the playback IO descriptor (%lu slots): give --slots by hand "
                        "(apollo-diag log, lines 'G5: play').\n", *Count);
        return FALSE;
    }
    return TRUE;
}

static int CmdTone(int n, char **a)
{
    APOLLO_INFO info;
    APOLLO_POSITION pos;
    PAPOLLO_PLAYBACK_HEADER hdr;
    double seconds = 0.0, freqHz = 1000.0, level = -20.0, amp, w;
    const char *opt;
    ULONG C, rate, ring, nslots = 0, k, underruns = 0;
    APOLLO_PLAYBACK_RESULT pres;
    ULONG slots[DIAG_MAX_SLOTS];
    unsigned char *buf = NULL;
    LONG *data = NULL;
    ULONGLONG startFrame = 0, written = 0, toneEnd = 0, silenceEnd = 0;
    BOOL stopping = FALSE, monSlots = FALSE, twinx;
    ULONG leadMin = DIAG_TONE_LEAD_MIN, leadMax = DIAG_TONE_LEAD_MAX, safety = DIAG_SAFETY_FRAMES, sleepMs = 10;
    int rc = 1;

    if (n < 2 || !ParseDouble(a[1], &seconds) || seconds <= 0.0) {
        fprintf(stderr, "usage: tone <seconds> [--slots 0,1|mon] [--freq 1000] [--level -20]\n");
        return 1;
    }
    opt = OptValue(n, a, 2, "--freq");
    if (opt != NULL && !ParseDouble(opt, &freqHz)) {
        fprintf(stderr, "--freq: expected a number in Hz\n");
        return 1;
    }
    opt = OptValue(n, a, 2, "--level");
    if (opt != NULL && (!ParseDouble(opt, &level) || level > 0.0 || level < -120.0)) {
        fprintf(stderr, "--level: expected dBFS between -120 and 0\n");
        return 1;
    }
    opt = OptValue(n, a, 2, "--slots");
    if (opt != NULL && _stricmp(opt, "mon") == 0) {
        monSlots = TRUE;
    } else if (opt != NULL) {
        if (!ParseList(opt, slots, DIAG_MAX_SLOTS, &nslots)) {
            fprintf(stderr, "--slots: expected a comma list of slot numbers, or mon\n");
            return 1;
        }
    } else {
        slots[0] = 0;
        slots[1] = 1;
        nslots = 2;
    }

    if (!GetInfo(&info))
        return 1;
    /* [Twin X G3] the Twin X: MON L/R from its live descriptor unless --slots names them; short tones */
    twinx = ApolloInfoHasUnit(&info, g_infoBytes) && info.Model == APOLLO_INFO_MODEL_TWINX;
    if (twinx && opt == NULL)
        monSlots = TRUE;
    if (monSlots) {
        LONG l = -1, r = -1;
        ULONG count = 0;

        if (!DescMonSlots(&l, &r, &count))
            return 1;
        if (twinx && info.PlayChannels != 0 && count != info.PlayChannels) {
            fprintf(stderr, "The playback descriptor now has %lu slots, the transport runs %lu: the layout changed "
                            "since the connect. Not playing; send Claude apollo-diag --unit twinx log.\n",
                    count, info.PlayChannels);
            return 1;
        }
        slots[0] = (ULONG)l;
        slots[1] = (ULONG)r;
        nslots = 2;
        printf("MON L/R = playback slots %ld/%ld (this unit's live IO descriptor, %lu slots).\n", l, r, count);
    }
    if (twinx) {
        if (ApolloInfoHasUnit(&info, g_infoBytes) && info.Mode == 1 && seconds > DIAG_MODE1_TONE_MAX) {
            printf("Mode 1 unit: the tone is limited to %.0f s.\n", DIAG_MODE1_TONE_MAX);
            seconds = DIAG_MODE1_TONE_MAX;
        }
        printf("Twin X: this plays on its MONITOR outputs (your speakers): keep them low.\n");
    }
    C = info.PlayChannels;
    rate = info.SampleRate ? info.SampleRate : DIAG_DEFAULT_RATE;
    ring = info.RingFrames ? info.RingFrames : DIAG_DEFAULT_RING;
    if (C == 0) {
        fprintf(stderr, "The driver reports 0 playback channels (stage: %s).\n", StageName(info.Stage));
        return 1;
    }
    if (!(info.Flags & APOLLO_FLAG_TRANSPORT)) {
        if (twinx)
            fprintf(stderr, "The transport is not running on this unit: see apollo-diag --unit twinx info and log "
                            "(the G3 bring-up lines).\n");
        else
            fprintf(stderr, "The transport is not running. Run 'apollo-diag start' first.\n");
        return 1;
    }
    for (k = 0; k < nslots; k++) {
        if (slots[k] >= C) {
            fprintf(stderr, "slot %lu is out of range: the playback ring has %lu slots (0..%lu)\n",
                    slots[k], C, C - 1);
            return 1;
        }
    }
    if (freqHz < 1.0 || freqHz >= (double)rate / 2.0) {
        fprintf(stderr, "--freq must be between 1 Hz and %lu Hz\n", rate / 2);
        return 1;
    }

    amp = pow(10.0, level / 20.0) * DIAG_MAX_24BIT;
    w = 2.0 * DIAG_PI * freqHz / (double)rate;
    if (rate > DIAG_DEFAULT_RATE) {
        /*
         * [Rate] Above 48 kHz the hardware moves more frames per poll and the driver's LATE line (its
         * lead + a tick) is further out: a safety margin of the same time, the writer further ahead
         * (within the zero horizon), polls more often. 44.1/48 kHz: the values above, as before.
         */
        safety = (ULONG)((ULONGLONG)DIAG_SAFETY_FRAMES * rate / DIAG_DEFAULT_RATE);
        leadMin = max(DIAG_TONE_LEAD_MIN, 2 * safety);
        leadMax = min(DIAG_TONE_BUF_MAX, leadMin + (ULONG)((ULONGLONG)DIAG_TONE_LEAD_MIN * rate / DIAG_DEFAULT_RATE));
        sleepMs = max(2ul, (ULONG)(10ull * DIAG_DEFAULT_RATE / rate));
    }
    buf = (unsigned char *)malloc(sizeof(APOLLO_PLAYBACK_HEADER) +
                                  (size_t)leadMax * C * sizeof(LONG));
    if (buf == NULL) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }
    hdr = (PAPOLLO_PLAYBACK_HEADER)buf;
    data = (LONG *)(buf + sizeof(APOLLO_PLAYBACK_HEADER));

    if (!GetPosition(&pos))
        goto out;
    startFrame = pos.Position + leadMin;
    written = startFrame;
    toneEnd = startFrame + (ULONGLONG)(seconds * (double)rate + 0.5);
    silenceEnd = toneEnd + ring;
    printf("Playing %.0f Hz at %.1f dBFS on %lu slot(s) for %.2f s. Ctrl+C stops early.\n",
           freqHz, level, nslots, seconds);

    while (written < silenceEnd) {
        ULONGLONG target;
        ULONG frames, i;
        DWORD ret = 0, inLen;

        if (g_stop && !stopping) {
            stopping = TRUE;
            if (written < toneEnd) {
                toneEnd = written;
                silenceEnd = toneEnd + ring;
            }
            printf("Stopping: writing silence over the playback ring...\n");
        }
        if (!GetPosition(&pos)) {
            fprintf(stderr, "Lost the device: the ring may still hold tone until the transport stops.\n");
            goto out;
        }
        if (written < pos.Position + safety) {
            underruns++;
            printf("UNDERRUN: the hardware at %llu caught up with the writer at %llu\n",
                   pos.Position, written);
            written = pos.Position + leadMin;
            if (written >= toneEnd)
                silenceEnd = written + ring;
        }

        target = pos.Position + leadMax;
        if (target > silenceEnd)
            target = silenceEnd;
        if (written < target) {
            frames = (ULONG)(target - written);
            if (frames > leadMax)
                frames = leadMax;
            ZeroMemory(data, (size_t)frames * C * sizeof(LONG));
            for (i = 0; i < frames; i++) {
                ULONGLONG fr = written + i;
                if (fr < toneEnd) {
                    LONG s = (LONG)(amp * sin(w * (double)(fr - startFrame)));
                    s = (LONG)((ULONG)s & 0xFFFFFF00ul);
                    for (k = 0; k < nslots; k++)
                        data[(size_t)i * C + slots[k]] = s;
                }
            }
            hdr->StartPosition = written;
            hdr->Frames = frames;
            hdr->Channels = C;
            inLen = (DWORD)(sizeof(APOLLO_PLAYBACK_HEADER) + (size_t)frames * C * sizeof(LONG));
            ZeroMemory(&pres, sizeof(pres));
            if (!DeviceIoControl(g_dev, IOCTL_APOLLO_WRITE_PLAYBACK, buf, inLen,
                                 &pres, (DWORD)sizeof(pres), &ret, NULL)) {
                PrintLastError("WRITE_PLAYBACK");
                goto out;
            }
            if (pres.Result == APOLLO_PLAYBACK_LATE) {
                underruns++;
                printf("UNDERRUN: the driver refused a write at %llu (hardware at %llu); "
                       "resyncing\n", written, pres.HwPosition);
                written = pres.HwPosition + leadMin;
                if (written >= toneEnd)
                    silenceEnd = written + ring;
                continue;
            }
            if (pres.Result == APOLLO_PLAYBACK_TOO_EARLY) {
                SleepMs(sleepMs);
                continue;
            }
            written += frames;
        }
        SleepMs(sleepMs);
    }

    printf("Done: %llu frames of tone, %lu underrun(s). The playback ring is silent again.\n",
           toneEnd > startFrame ? toneEnd - startFrame : 0, underruns);
    rc = 0;

out:
    free(buf);
    return rc;
}

static int CmdInputParam(int n, char **a, ULONG param, const char *name)
{
    APOLLO_INPUT_PARAM p;
    ULONG preamp = 0, v = 0;

    if (n < 3 || !ParseU32(a[1], &preamp) || preamp < 1 || !ParseU32(a[2], &v)) {
        if (param == APOLLO_PARAM_GAIN_DB)
            fprintf(stderr, "usage: gain <preamp 1..8 (Twin X 1..2)> <dB 10..65>\n");
        else
            fprintf(stderr, "usage: %s <preamp 1..8 (Twin X 1..2)> 0|1\n", a[0]);
        return 1;
    }
    if (param == APOLLO_PARAM_GAIN_DB) {
        if (v < 10 || v > 65) {
            fprintf(stderr, "gain must be 10..65 dB\n");
            return 1;
        }
    } else if (param == APOLLO_PARAM_LINK && (preamp % 2) == 0) {
        fprintf(stderr, "link takes the odd preamp of a pair: 1, 3, 5 or 7\n");
        return 1;
    } else if (v > 1) {
        fprintf(stderr, "%s takes 0 or 1\n", a[0]);
        return 1;
    }
    ZeroMemory(&p, sizeof(p));
    p.Preamp = preamp - 1;
    p.Param = param;
    p.Value = (LONG)v;
    if (!Ioctl(IOCTL_APOLLO_SET_INPUT_PARAM, "SET_INPUT_PARAM", &p, (DWORD)sizeof(p), NULL, 0, NULL))
        return 1;
    printf("preamp %lu: %s = %lu queued (the service loop commits it on its next pass)\n", preamp,
           name, v);
    return 0;
}

/* [Twin X G5] hiz <preamp> <0..7 | 2.2M 1M 453k 232k 100k 70k 47k 21k>: the Hi-Z jack's impedance. */
static int CmdHiz(int n, char **a)
{
    APOLLO_INPUT_PARAM p;
    ULONG preamp = 0, code = APOLLO_PRE_HIZ_CODES, k;

    if (n >= 3 && !ParseU32(a[2], &code)) {
        for (k = 0; k < APOLLO_PRE_HIZ_CODES; k++) {
            if (_stricmp(a[2], ApolloPreampHizName(k)) == 0)
                code = k;
        }
    }
    if (n < 3 || !ParseU32(a[1], &preamp) || preamp < 1 || code >= APOLLO_PRE_HIZ_CODES) {
        fprintf(stderr, "usage: hiz <preamp> <0..7 | 2.2M 1M 453k 232k 100k 70k 47k 21k>\n"
                        "       (the Twin X: preamp 1, its front Hi-Z jack; no readback: the unit does not report it)\n");
        return 1;
    }
    ZeroMemory(&p, sizeof(p));
    p.Preamp = preamp - 1;
    p.Param = APOLLO_PARAM_HIZ_IMP;
    p.Value = (LONG)code;
    if (!Ioctl(IOCTL_APOLLO_SET_INPUT_PARAM, "SET_INPUT_PARAM", &p, (DWORD)sizeof(p), NULL, 0, NULL))
        return 1;
    printf("preamp %lu: Hi-Z impedance %s (code %lu) queued (the service loop commits it on its next pass)\n",
           preamp, ApolloPreampHizName(code), code);
    return 0;
}

typedef struct _MON_NAME {
    const char *Name;
    ULONG       Param;
    int         Kind;       /* 0 switch, 1 level dB, 2 monitor source, 3 cue source, 4 dim level,
                               [Monitor] 5 speakers, 6 ALT trim dB, 7 cue MIX source */
} MON_NAME;

static const MON_NAME g_MonNames[] = {
    { "level", APOLLO_MON_LEVEL, 1 },       { "hp1", APOLLO_MON_HP1_LEVEL, 1 },
    { "mute", APOLLO_MON_MUTE, 0 },         { "mono", APOLLO_MON_MONO, 0 },
    { "dim", APOLLO_MON_DIM, 0 },           { "dimlevel", APOLLO_MON_DIM_LEVEL, 4 },
    { "source", APOLLO_MON_SOURCE, 2 },     { "hp1src", APOLLO_MON_HP1_SOURCE, 3 },
    { "hp2src", APOLLO_MON_HP2_SOURCE, 3 }, { "talkback", APOLLO_MON_TALKBACK, 0 },
    /* [Monitor] driver\monitor_plan.h */
    { "alt", APOLLO_MON_ALT_SELECT, 5 },    { "alt1", APOLLO_MON_ALT1_ENABLE, 0 },
    { "alt2", APOLLO_MON_ALT2_ENABLE, 0 },  { "alt1trim", APOLLO_MON_ALT1_TRIM, 6 },
    { "alt2trim", APOLLO_MON_ALT2_TRIM, 6 },
    { "cue1mix", APOLLO_MON_CUE1_MIX, 7 },  { "cue2mix", APOLLO_MON_CUE2_MIX, 7 },
    { "cue3mix", APOLLO_MON_CUE3_MIX, 7 },  { "cue4mix", APOLLO_MON_CUE4_MIX, 7 },
    { "cue1mono", APOLLO_MON_CUE1_MONO, 0 }, { "cue2mono", APOLLO_MON_CUE2_MONO, 0 },
    { "cue3mono", APOLLO_MON_CUE3_MONO, 0 }, { "cue4mono", APOLLO_MON_CUE4_MONO, 0 },
    /* [Out] output and front-panel settings (driver\monitor_plan.h); kind 8 = a raw value the driver checks */
    { "aux1mono", APOLLO_MON_AUX1_MONO, 0 },   { "aux2mono", APOLLO_MON_AUX2_MONO, 0 },
    { "linepad1", APOLLO_MON_LINE_PAD(0), 0 }, { "linepad2", APOLLO_MON_LINE_PAD(1), 0 },
    { "linepad3", APOLLO_MON_LINE_PAD(2), 0 }, { "linepad4", APOLLO_MON_LINE_PAD(3), 0 },
    { "monref", APOLLO_MON_MON_REF, 0 },       { "digmirror", APOLLO_MON_DIG_MIRROR, 0 },
    { "fcn", APOLLO_MON_FUNC_SWITCH, 8 },      { "cliphold", APOLLO_MON_CLIP_HOLD, 8 },
    { "peakhold", APOLLO_MON_PEAK_HOLD, 8 },   { "identify", APOLLO_MON_IDENTIFY, 8 },
};

/* [Monitor] mix|cue1..cue4 (and the Twin X's names hp = cue1, line34 = cue2): 0..4, or -1. */
static long MonSourceValue(const char *s)
{
    if (_stricmp(s, "mix") == 0) return 0;
    if (_stricmp(s, "cue1") == 0 || _stricmp(s, "hp") == 0) return 1;
    if (_stricmp(s, "cue2") == 0 || _stricmp(s, "line34") == 0) return 2;
    if (_stricmp(s, "cue3") == 0) return 3;
    if (_stricmp(s, "cue4") == 0) return 4;
    return -1;
}

static int CmdMon(int n, char **a)
{
    APOLLO_MONITOR_PARAM p;
    const MON_NAME *m = NULL;
    char *end = NULL;
    long v = 0;
    ULONG i;

    if (n >= 2) {
        for (i = 0; i < RTL_NUMBER_OF(g_MonNames); i++) {
            if (_stricmp(a[1], g_MonNames[i].Name) == 0)
                m = &g_MonNames[i];
        }
    }
    if (m == NULL || n < 3) {
        fprintf(stderr,
                "usage: mon level <dB -96..0> | hp1 <dB -96..0> | mute|mono|dim|talkback 0|1\n"
                "       mon dimlevel 1..7 (-9 -17 -26 -34 -43 -51 -60 dB)\n"
                "       mon source mix|cue1..cue4 | hp1src cue1|cue2 | hp2src cue1|cue2\n"
                "           (Twin X: hp = cue1, line34 = cue2; source cue3/cue4 on the x8p only)\n"
                "       mon alt main|alt1|alt2      speakers (MOVES AUDIO between speaker sets)\n"
                "       mon alt1|alt2 0|1           ALT output on (x8p: Line 1-2 / 3-4; Twin X: alt1 = Line 3/4)\n"
                "       mon alt1trim|alt2trim <dB -30..30, 0.5 dB steps>\n"
                "       mon cue1mix..cue4mix own|mon   a cue's source: its own mix or the monitor mix\n"
                "       mon cue1mono..cue4mono 0|1  (Twin X: cue1 = HP, cue2 = Line 3/4)\n"
                "       mon linepad1..linepad4 0|1  x8p Line 1-2..7-8 reference: 0 +4 dBu, 1 -10 dBV; monref 0|1 the monitor's\n"
                "       mon digmirror 0|1 (x8p, monitor to S/PDIF) | aux1mono|aux2mono 0|1\n"
                "       mon fcn 0|2|3|4 (x8p: none, mono, dim, talkback) | cliphold 0..5 | peakhold 0..4 | identify 0..7\n");
        return 1;
    }
    if (m->Kind == 2 || m->Kind == 3) {
        v = MonSourceValue(a[2]);
        if (m->Kind == 3) {
            v = (v >= 1) ? v - 1 : -1;      /* headphones: CUE n = n - 1 (the driver takes CUE 1 / CUE 2 only) */
        }
        if (v < 0) {
            fprintf(stderr, "%s: unknown source '%s'\n", m->Name, a[2]);
            return 1;
        }
    } else if (m->Kind == 5) {
        if (_stricmp(a[2], "main") == 0 || strcmp(a[2], "0") == 0)        v = 0;
        else if (_stricmp(a[2], "alt1") == 0 || strcmp(a[2], "1") == 0)   v = 1;
        else if (_stricmp(a[2], "alt2") == 0 || strcmp(a[2], "2") == 0)   v = 2;
        else {
            fprintf(stderr, "alt: main, alt1 or alt2\n");
            return 1;
        }
    } else if (m->Kind == 7) {
        if (_stricmp(a[2], "own") == 0 || strcmp(a[2], "0") == 0)         v = 0;
        else if (_stricmp(a[2], "mon") == 0 || strcmp(a[2], "1") == 0)    v = 1;
        else {
            fprintf(stderr, "%s: own (the cue's own mix) or mon (the monitor mix)\n", m->Name);
            return 1;
        }
    } else {
        double d = strtod(a[2], &end);
        if (end == a[2] || *end != '\0') {
            fprintf(stderr, "%s: '%s' is not a number\n", m->Name, a[2]);
            return 1;
        }
        if (m->Kind == 1) {
            if (d < -96.0 || d > 0.0) {
                fprintf(stderr, "%s: level must be -96..0 dB\n", m->Name);
                return 1;
            }
            v = (long)floor(192.0 + 2.0 * d + 0.5);
        } else if (m->Kind == 4) {
            v = (long)d;
            if (v < 1 || v > 7) {
                fprintf(stderr, "dimlevel takes 1..7\n");
                return 1;
            }
        } else if (m->Kind == 6) {
            if (d < -30.0 || d > 30.0) {
                fprintf(stderr, "%s: -30..+30 dB\n", m->Name);
                return 1;
            }
            v = (long)floor(APOLLO_MP_ALT_TRIM_UNITY + 2.0 * d + 0.5);     /* raw = 60 + 2*dB */
        } else if (m->Kind == 8) {
            v = (long)d;                    /* [Out] the driver says what it takes (STATUS_INVALID_PARAMETER) */
        } else {
            v = (long)d;
            if (v != 0 && v != 1) {
                fprintf(stderr, "%s takes 0 or 1\n", m->Name);
                return 1;
            }
        }
    }
    ZeroMemory(&p, sizeof(p));
    p.Param = m->Param;
    p.Value = v;
    if (!Ioctl(IOCTL_APOLLO_SET_MONITOR_PARAM, "SET_MONITOR_PARAM", &p, (DWORD)sizeof(p), NULL, 0,
               NULL))
        return 1;
    printf("monitor %s = %ld queued (the service loop commits it on its next pass)\n", m->Name, v);
    return 0;
}

/* [Monitor] One SET_MONITOR_PARAM; prints what went in. */
static BOOL SendMon(ULONG Param, LONG Value, const char *What)
{
    APOLLO_MONITOR_PARAM p;

    ZeroMemory(&p, sizeof(p));
    p.Param = Param;
    p.Value = Value;
    if (!Ioctl(IOCTL_APOLLO_SET_MONITOR_PARAM, "SET_MONITOR_PARAM", &p, (DWORD)sizeof(p), NULL, 0, NULL))
        return FALSE;
    printf("%s (code 0x%04lX = %ld) sent\n", What, Param, Value);
    return TRUE;
}

static int CueUsage(void)
{
    fprintf(stderr,
            "usage: cue <1-4> send <input 1-8|tb> <dB -60..12|off> [pan -100..100]\n"
            "       cue <1-4> pan <input 1-8|tb> <-100 left..100 right>\n"
            "       cue <1-4> mute 0|1\n"
            "  A cue's send of an input (or the talkback mic, tb): DSP bus coefficients, no readback\n"
            "  (the driver keeps them and sends them again after a reconnect). Twin X: cue 1 = HP,\n"
            "  cue 2 = Line 3/4, inputs 1-2; the x8p: cues 1-4, inputs 1-8.\n");
    return 1;
}

/* [Monitor] cue <n> send|pan|mute ... (driver\monitor_plan.h APOLLO_MON_CUE_*) */
static int CmdCue(int n, char **a)
{
    ULONG cue = 0, in = 0, bus;
    char *end = NULL, what[96];
    long pan = 0, level;
    BOOL hasPan = FALSE;

    if (n < 4 || !ParseU32(a[1], &cue) || cue < 1 || cue > 4)
        return CueUsage();
    if (_stricmp(a[2], "mute") == 0) {
        if (strcmp(a[3], "0") != 0 && strcmp(a[3], "1") != 0)
            return CueUsage();
        sprintf_s(what, sizeof(what), "cue %lu mute %s", cue, a[3]);
        return SendMon(APOLLO_MON_CUE_MUTE(cue - 1), a[3][0] == '1' ? 1 : 0, what) ? 0 : 1;
    }
    if (_stricmp(a[3], "tb") == 0 || _stricmp(a[3], "talkback") == 0)
        bus = APOLLO_MON_BUS_TALKBACK;
    else if (ParseU32(a[3], &in) && in >= 1 && in <= 8)
        bus = in - 1;
    else
        return CueUsage();
    if (n < 5)
        return CueUsage();
    if (_stricmp(a[2], "pan") == 0 || (_stricmp(a[2], "send") == 0 && n >= 6)) {
        const char *ps = _stricmp(a[2], "pan") == 0 ? a[4] : a[5];
        pan = strtol(ps, &end, 10);
        if (end == ps || *end != '\0' || pan < -100 || pan > 100) {
            fprintf(stderr, "pan: -100 (left) .. 100 (right)\n");
            return 1;
        }
        hasPan = TRUE;
    }
    if (hasPan) {
        sprintf_s(what, sizeof(what), "cue %lu pan of %s%s %ld", cue, bus == APOLLO_MON_BUS_TALKBACK ? "talkback" : "input ",
                  bus == APOLLO_MON_BUS_TALKBACK ? "" : a[3], pan);
        if (!SendMon(APOLLO_MON_CUE_PAN(cue - 1, bus), pan, what))
            return 1;
    }
    if (_stricmp(a[2], "pan") == 0)
        return 0;
    if (_stricmp(a[2], "send") != 0)
        return CueUsage();
    if (_stricmp(a[4], "off") == 0) {
        level = APOLLO_MON_SEND_OFF;
    } else {
        double d = strtod(a[4], &end);
        if (end == a[4] || *end != '\0' || d < -60.0 || d > 12.0) {
            fprintf(stderr, "send level: -60..+12 dB (0.5 dB steps) or off\n");
            return 1;
        }
        level = (long)floor(2.0 * d + 0.5);
    }
    sprintf_s(what, sizeof(what), "cue %lu send of %s%s %s dB", cue, bus == APOLLO_MON_BUS_TALKBACK ? "talkback" : "input ",
              bus == APOLLO_MON_BUS_TALKBACK ? "" : a[3], a[4]);
    return SendMon(APOLLO_MON_CUE_SEND(cue - 1, bus), level, what) ? 0 : 1;
}

static int MixUsage(void)
{
    fprintf(stderr,
            "usage: mix <input 1-8|tb> level <dB -60..12|off>\n"
            "       mix <input 1-8> pan <-100 left..100 right>\n"
            "       mix <input 1-8|tb> mute 0|1\n"
            "       mix <input 1-8|tb> solo 0|1\n"
            "       mix all mute 0|1\n"
            "  The monitor mix, what the monitor outputs play (DSP bus coefficients, param 0, no readback: the\n"
            "  driver keeps them and sends them again after a reconnect). Nothing is sent for an input until it\n"
            "  is set here; the unit keeps its own default (inputs on at 0 dB). Solo: every other input goes\n"
            "  out as 0.0 while any input is soloed. Twin X: inputs 1-2; the x8p: inputs 1-8; tb = talkback mic.\n");
    return 1;
}

/* [Mix] mix <input|tb|all> level|pan|mute|solo ... (driver\monitor_plan.h APOLLO_MON_MIX_*) */
static int CmdMix(int n, char **a)
{
    ULONG in = 0, bus;
    char *end = NULL, what[96], who[24];
    long v;

    if (n < 4)
        return MixUsage();
    if (_stricmp(a[1], "all") == 0) {
        if (_stricmp(a[2], "mute") != 0 || (strcmp(a[3], "0") != 0 && strcmp(a[3], "1") != 0))
            return MixUsage();
        sprintf_s(what, sizeof(what), "monitor mix: all inputs %s", a[3][0] == '1' ? "muted" : "unmuted");
        return SendMon(APOLLO_MON_MIX_MUTE_ALL, a[3][0] == '1' ? 1 : 0, what) ? 0 : 1;
    }
    if (_stricmp(a[1], "tb") == 0 || _stricmp(a[1], "talkback") == 0) {
        bus = APOLLO_MON_BUS_TALKBACK;
        strcpy_s(who, sizeof(who), "talkback");
    } else if (ParseU32(a[1], &in) && in >= 1 && in <= 8) {
        bus = in - 1;
        sprintf_s(who, sizeof(who), "input %lu", in);
    } else {
        return MixUsage();
    }
    if (_stricmp(a[2], "level") == 0) {
        if (_stricmp(a[3], "off") == 0) {
            v = APOLLO_MON_SEND_OFF;
        } else {
            double d = strtod(a[3], &end);
            if (end == a[3] || *end != '\0' || d < -60.0 || d > 12.0) {
                fprintf(stderr, "mix level: -60..+12 dB (0.5 dB steps) or off\n");
                return 1;
            }
            v = (long)floor(2.0 * d + 0.5);
        }
        sprintf_s(what, sizeof(what), "monitor mix: %s level %s dB", who, a[3]);
        return SendMon(APOLLO_MON_MIX_LEVEL(bus), v, what) ? 0 : 1;
    }
    if (_stricmp(a[2], "pan") == 0) {
        v = strtol(a[3], &end, 10);
        if (end == a[3] || *end != '\0' || v < -100 || v > 100) {
            fprintf(stderr, "mix pan: -100 (left) .. 100 (right)\n");
            return 1;
        }
        sprintf_s(what, sizeof(what), "monitor mix: %s pan %ld", who, v);
        return SendMon(APOLLO_MON_MIX_PAN(bus), v, what) ? 0 : 1;
    }
    if (_stricmp(a[2], "mute") == 0 || _stricmp(a[2], "solo") == 0) {
        BOOL mute = _stricmp(a[2], "mute") == 0;

        if (strcmp(a[3], "0") != 0 && strcmp(a[3], "1") != 0)
            return MixUsage();
        sprintf_s(what, sizeof(what), "monitor mix: %s %s %s", who, mute ? "mute" : "solo", a[3]);
        return SendMon(mute ? APOLLO_MON_MIX_MUTE(bus) : APOLLO_MON_MIX_SOLO(bus), a[3][0] == '1' ? 1 : 0, what) ? 0 : 1;
    }
    return MixUsage();
}

static int CmdLevels(int n, char **a)
{
    ULONG secs = 3, i;
    ULONGLONG until;

    if (n >= 2 && !ParseU32(a[1], &secs)) {
        fprintf(stderr, "usage: levels [seconds]\n");
        return 1;
    }
    until = GetTickCount64() + (ULONGLONG)secs * 1000;
    while (!g_stop) {
        APOLLO_LEVELS lv;
        DWORD ret = 0;

        ZeroMemory(&lv, sizeof(lv));
        if (!Ioctl(IOCTL_APOLLO_GET_LEVELS, "GET_LEVELS", NULL, 0, &lv, (DWORD)sizeof(lv), &ret))
            return 1;
        if (!lv.Valid) {
            printf("No levels: the transport is stopped (apollo-diag start, elevated).\n");
            return 1;
        }
        printf("pos %llu |", lv.Position);
        for (i = 0; i < lv.Channels && i < APOLLO_LEVEL_CHANNELS; i++) {
            /* 1 LSB of 24 bits is -138.5 dBFS; "zero" means exact digital silence. */
            if (lv.Peak[i] == 0)
                printf(" %lu:zero", i);
            else
                printf(" %lu:%.1f", i, 20.0 * log10((double)lv.Peak[i] / 8388607.0));
        }
        printf("\n");
        if (GetTickCount64() >= until)
            break;
        SleepMs(250);
    }
    return 0;
}

static int CmdSetting(int n, char **a)
{
    APOLLO_MIXER_SETTING s;
    ULONG idx = 0, v = 0, m = 0;

    if (n < 4 || !ParseU32(a[1], &idx) || !ParseU32(a[2], &v) || !ParseU32(a[3], &m) || idx > 37) {
        fprintf(stderr, "usage: setting <index 0..37> <value> <mask>\n");
        return 1;
    }
    ZeroMemory(&s, sizeof(s));
    s.Index = idx;
    s.Value = v;
    s.Mask = m;
    if (!Ioctl(IOCTL_APOLLO_SET_MIXER_SETTING, "SET_MIXER_SETTING", &s, (DWORD)sizeof(s), NULL, 0, NULL))
        return 1;
    printf("setting %lu: value 0x%08lX mask 0x%08lX queued\n", idx, v, m);
    return 0;
}

static void DecodePreampFlags(ULONG w, ULONG first)
{
    ULONG p;
    for (p = 0; p < 4; p++) {
        ULONG b = 6 * p;
        printf("  preamp %lu: %-4s PAD %lu  Link %lu  48V %lu  LowCut %lu  Phase %lu\n", first + p,
               ((w >> b) & 1ul) ? "Line" : "Mic", (w >> (b + 1)) & 1ul, (w >> (b + 2)) & 1ul,
               (w >> (b + 3)) & 1ul, (w >> (b + 4)) & 1ul, (w >> (b + 5)) & 1ul);
    }
}

static void DecodePreampGains(ULONG w, ULONG first)
{
    static const ULONG shifts[4] = { 0, 6, 16, 22 };
    ULONG p;
    for (p = 0; p < 4; p++) {
        ULONG gc = (w >> shifts[p]) & 0x3Ful;
        ULONG db = gc + 9;
        if (db > 65)
            db = 65;
        printf("  preamp %lu: GainC %2lu -> %lu dB\n", first + p, gc, db);
    }
}

static int CmdReadback(void)
{
    APOLLO_READBACK rb;
    DWORD ret = 0;
    ULONG i, w2;

    ZeroMemory(&rb, sizeof(rb));
    if (!Ioctl(IOCTL_APOLLO_GET_READBACK, "GET_READBACK", NULL, 0, &rb, (DWORD)sizeof(rb), &ret))
        return 1;
    if (ret < sizeof(rb)) {
        fprintf(stderr, "GET_READBACK returned %lu bytes, expected %lu\n", ret,
                (unsigned long)sizeof(rb));
        return 1;
    }
    printf("Valid %lu  Sequence %lu\n", rb.Valid, rb.Sequence);
    if (!rb.Valid) {
        printf("No readback drained yet: the mixer is not ready or the DSP is not answering.\n");
        return 1;
    }
    for (i = 0; i < APOLLO_READBACK_WORDS; i += 4) {
        printf("  [%2lu] %08lX %08lX %08lX %08lX\n", i, rb.Words[i], rb.Words[i + 1],
               rb.Words[i + 2], rb.Words[i + 3]);
    }

    /* [Twin X G5] decoded per model (driver\preamp_plan.h): the Twin X has 2 preamps, Hi-Z on 1 */
    {
        APOLLO_INFO info;
        const APOLLO_PREAMP_MODEL *pm = NULL;

        ZeroMemory(&info, sizeof(info));
        if (GetInfo(&info) && ApolloInfoHasUnit(&info, g_infoBytes) && info.Model == APOLLO_INFO_MODEL_TWINX)
            pm = ApolloPreampModelFor(APOLLO_MODEL_TWINX);
        if (pm != NULL) {
            APOLLO_PREAMP_STATE st;
            ULONG ch;

            printf("\n%s: preamps 1-%lu (flags word 0, GainC word 3; Hi-Z jack on input 1 = word 0 bit 24):\n",
                   pm->Name, pm->Preamps);
            for (ch = 0; ch < pm->Preamps; ch++) {
                if (!ApolloPreampDecode(pm, rb.Words, APOLLO_READBACK_WORDS, ch, &st))
                    continue;
                printf("  preamp %lu: %-4s PAD %lu  Link %lu  48V %lu  LowCut %lu  Phase %lu  GainC %2lu -> %lu dB%s%s\n",
                       ch + 1, st.Line ? "Line" : "Mic", st.Pad, st.Link, st.Phantom, st.LowCut, st.Phase, st.GainC,
                       st.GainDb, st.HasHiz ? "  Hi-Z jack: " : "", st.HasHiz ? (st.HizIn ? "cable in" : "empty") : "");
            }
            printf("  (Hi-Z impedance has no readback field; preamps 3-8 do not exist on this model)\n");
        } else {
            printf("\nPreamps 1-4 (word 0):\n");
            DecodePreampFlags(rb.Words[0], 1);
            printf("  Hi-Z jack: input 1 %lu, input 2 %lu\n", (rb.Words[0] >> 24) & 1ul,
                   (rb.Words[0] >> 25) & 1ul);
            printf("Gains 1-4 (word 3):\n");
            DecodePreampGains(rb.Words[3], 1);
            printf("Preamps 5-8 (word 5):\n");
            DecodePreampFlags(rb.Words[5], 5);
            printf("Gains 5-8 (word 7):\n");
            DecodePreampGains(rb.Words[7], 5);
        }
        printf("Talkback (word 6 bit 8): %s; clock (word 6 bits 31/30): %s\n",
               ApolloRbTalkback(rb.Words) ? "ON" : "off",
               ApolloRbClockLock(rb.Words) < 0 ? "not reported" :
               ApolloRbClockLock(rb.Words) ? "locked" : "NOT locked");
    }

    w2 = rb.Words[2];
    printf("Monitor (word 2): level raw %lu (%.1f dB if encoded as 192 + 2*dB), HP1 raw %lu, "
           "mute %lu, mono %lu, dim %lu\n", w2 & 0xFFul, ((double)(w2 & 0xFFul) - 192.0) / 2.0,
           (w2 >> 8) & 0xFFul, (w2 >> 16) & 1ul, (w2 >> 17) & 1ul, (w2 >> 31) & 1ul);
    /* [Monitor] driver\monitor_plan.h: the ALT selection is documented; the rest may or may not be echoed */
    {
        static const char *const spk[4] = { "MAIN", "ALT 1", "ALT 2", "3?" };
        static const char *const src[8] = { "MIX", "CUE 1", "CUE 2", "CUE 3", "CUE 4", "5?", "6?", "7?" };
        const unsigned long *w = (const unsigned long *)rb.Words;

        printf("Speakers (word 6 [29:28]): %s\n", spk[ApolloRbAltSelect(w)]);
        printf("If echoed (no source documents these; tools\\phase1\\step-mon.ps1 checks whether they follow):\n"
               "  word 2: source %s, CUE 1 %s%s, CUE 2 %s%s\n"
               "  word 5: HP 1 source CUE %lu, HP 2 source CUE %lu\n",
               src[ApolloRbEchoSource(w) & 7ul], ApolloRbEchoCueMix(w, 0) ? "monitor mix" : "own mix",
               ApolloRbEchoCueMono(w, 0) ? " mono" : "", ApolloRbEchoCueMix(w, 1) ? "monitor mix" : "own mix",
               ApolloRbEchoCueMono(w, 1) ? " mono" : "", ApolloRbEchoHpSource(w, 0) + 1, ApolloRbEchoHpSource(w, 1) + 1);
    }
    if (rb.Words[38] == 0xFFFFFFFFul || rb.Words[39] == 0xFFFFFFFFul)
        printf("warning: word 38 or 39 reads 0xFFFFFFFF; the Mixer Engine drops such snapshots.\n");
    return 0;
}

/*
 * [Twin X G2] The log is one ring for every unit; a unit's lines carry "[tag] " after the time
 * (apollo_ioctl.h GET_LOG). Does this line belong to tag? Lines without a tag (driver-wide) do.
 */
static BOOL LogLineForTag(const char *line, size_t len, const char *tag)
{
    const char *close = (len > 0 && line[0] == '[') ? memchr(line, ']', len) : NULL;
    size_t at, t = strlen(tag);

    if (close == NULL)
        return TRUE;
    at = (size_t)(close - line) + 1;
    if (at + 1 >= len || line[at] != ' ' || line[at + 1] != '[')
        return TRUE;                            /* "[time] text": driver-wide */
    return at + 2 + t < len && memcmp(line + at + 2, tag, t) == 0 && line[at + 2 + t] == ']';
}

static int CmdLog(int n, char **a)
{
    DWORD size = (DWORD)(sizeof(APOLLO_LOG_HEADER) + APOLLO_LOG_BYTES);
    unsigned char *buf = (unsigned char *)calloc(1, size);
    PAPOLLO_LOG_HEADER hdr = (PAPOLLO_LOG_HEADER)buf;
    DWORD ret = 0;
    char tag[sizeof(((APOLLO_INFO *)0)->LogTag) + 1] = "";
    BOOL all = HasFlag(n, a, 1, "--all");

    if (buf == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    /* With --unit (or --index): that unit's lines and the driver-wide ones; --all: everything. */
    if (g_unitNamed && !all && g_unitIndex >= 0) {
        const APOLLO_UNIT_ENTRY *e = &g_units[g_unitIndex];
        if (e->Error == 0 && ApolloInfoHasUnit(&e->Info, e->InfoBytes) &&
            e->Info.LogTag[sizeof(e->Info.LogTag) - 1] == 0) {
            memcpy(tag, e->Info.LogTag, sizeof(e->Info.LogTag));
            tag[sizeof(tag) - 1] = 0;
        }
    }
    if (!Ioctl(IOCTL_APOLLO_GET_LOG, "GET_LOG", NULL, 0, buf, size, &ret)) {
        free(buf);
        return 1;
    }
    if (ret < sizeof(*hdr) || hdr->Length > ret - sizeof(*hdr)) {
        fprintf(stderr, "GET_LOG returned a malformed buffer (%lu bytes)\n", ret);
        free(buf);
        return 1;
    }
    if (hdr->TotalWritten > hdr->Length)
        printf("(older log text overwritten: showing the last %lu of %lu bytes)\n",
               hdr->Length, hdr->TotalWritten);
    if (tag[0] == 0) {
        fwrite(buf + sizeof(*hdr), 1, hdr->Length, stdout);
    } else {
        const char *p = (const char *)(buf + sizeof(*hdr)), *end = p + hdr->Length;

        printf("(lines of unit [%s] and driver-wide lines; apollo-diag log --all shows every unit)\n", tag);
        while (p < end) {
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            size_t len = nl ? (size_t)(nl - p) + 1 : (size_t)(end - p);

            if (LogLineForTag(p, len, tag))
                fwrite(p, 1, len, stdout);
            p += len;
        }
    }
    free(buf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* input strips (docs/STRIPS.md section 4)                             */
/* ------------------------------------------------------------------ */

#define STRIP_FILE_MAX      (512u * 1024u * 1024u)  /* UAD2SDK.dll is large; refuse anything absurd */
/*
 * Admin-only, no Users ACE: a user who could open a .bill file (with share mode 0 or a batch
 * oplock) could stall apollo.sys's read of it, and through it sleep and device removal.
 */
#define STRIP_SDDL          L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)"     /* the folder */
#define STRIP_FILE_SDDL     L"D:P(A;;FA;;;SY)(A;;FA;;;BA)"             /* each file */

typedef struct _STRIP_FOUND {
    BOOL  Found;
    WCHAR From[MAX_PATH];
    DWORD Offset;
    UCHAR Rec[APOLLO_STRIP_MODULE_MAX];
} STRIP_FOUND;

static STRIP_FOUND g_strip[APOLLO_STRIP_MODULES];
static BCRYPT_ALG_HANDLE g_sha = NULL;

static BOOL Sha256(const UCHAR *data, ULONG len, UCHAR out[32])
{
    if (g_sha == NULL &&
        !BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&g_sha, BCRYPT_SHA256_ALGORITHM, NULL, 0))) {
        g_sha = NULL;
        return FALSE;
    }
    return BCRYPT_SUCCESS(BCryptHash(g_sha, NULL, 0, (PUCHAR)data, len, out, 32));
}

static ULONG Le32(const UCHAR *p)
{
    return (ULONG)p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24);
}

/* Exactly the pinned record: length, header, w4 and SHA-256 (what apollo.sys checks again). */
static BOOL StripRecordMatches(const APOLLO_STRIP_PIN *pin, const UCHAR *rec, size_t len)
{
    UCHAR sha[32];

    if (len != pin->Length || len < APOLLO_STRIP_BILL_HEADER)
        return FALSE;
    if (Le32(rec) != APOLLO_STRIP_BILL_MAGIC || Le32(rec + 4) != pin->Id ||
        Le32(rec + 8) != APOLLO_STRIP_BILL_W2 ||
        Le32(rec + 12) != (ULONG)pin->Length - APOLLO_STRIP_BILL_HEADER || Le32(rec + 16) != pin->W4)
        return FALSE;
    if (!Sha256(rec, pin->Length, sha))
        return FALSE;
    return memcmp(sha, pin->Sha256, sizeof(sha)) == 0;
}

static UCHAR *ReadWholeFile(const WCHAR *path, DWORD *size)
{
    HANDLE h;
    LARGE_INTEGER sz;
    UCHAR *buf;
    DWORD got = 0, r;

    *size = 0;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > STRIP_FILE_MAX) {
        CloseHandle(h);
        return NULL;
    }
    buf = (UCHAR *)malloc((size_t)sz.QuadPart);
    if (buf == NULL) {
        CloseHandle(h);
        return NULL;
    }
    while (got < (DWORD)sz.QuadPart) {
        if (!ReadFile(h, buf + got, (DWORD)sz.QuadPart - got, &r, NULL) || r == 0)
            break;
        got += r;
    }
    CloseHandle(h);
    if (got != (DWORD)sz.QuadPart) {
        free(buf);
        return NULL;
    }
    *size = got;
    return buf;
}

static BOOL StripAllFound(void)
{
    ULONG j;

    for (j = 0; j < APOLLO_STRIP_MODULES; j++) {
        if (!g_strip[j].Found)
            return FALSE;
    }
    return TRUE;
}

/*
 * The raw scan open-apollo's oa-assets does (firmware.rs:200-228): "Bill" at every 4-byte
 * offset, id top byte 0, word 2 = 0x02000000, the record inside the file. A record is kept only
 * if it is exactly a pinned module. Nothing in the DLL is executed or parsed beyond this.
 */
static void StripScanFile(const WCHAR *path)
{
    DWORD size = 0, off;
    UCHAR *data = ReadWholeFile(path, &size);
    ULONG j;

    if (data == NULL)
        return;
    for (off = 0; size >= APOLLO_STRIP_BILL_HEADER && off <= size - APOLLO_STRIP_BILL_HEADER; off += 4) {
        ULONG id, payload;

        if (memcmp(data + off, "Bill", 4) != 0)
            continue;
        id = Le32(data + off + 4);
        if ((id >> 24) != 0 || Le32(data + off + 8) != APOLLO_STRIP_BILL_W2)
            continue;
        payload = Le32(data + off + 12);
        if (payload > size - off - APOLLO_STRIP_BILL_HEADER)
            continue;
        for (j = 0; j < APOLLO_STRIP_MODULES; j++) {
            const APOLLO_STRIP_PIN *pin = &g_ApolloStripPins[j];

            if (g_strip[j].Found || pin->Id != id)
                continue;
            if (StripRecordMatches(pin, data + off, APOLLO_STRIP_BILL_HEADER + (size_t)payload)) {
                memcpy(g_strip[j].Rec, data + off, pin->Length);
                g_strip[j].Found = TRUE;
                g_strip[j].Offset = off;
                wcsncpy_s(g_strip[j].From, MAX_PATH, path, _TRUNCATE);
            }
        }
    }
    free(data);
}

static BOOL EndsWithI(const WCHAR *s, const WCHAR *tail)
{
    size_t ls = wcslen(s), lt = wcslen(tail);

    return ls >= lt && _wcsicmp(s + ls - lt, tail) == 0;
}

/* "*Strip Header.dll" up to Depth folders below Dir (UAD-2 Powered Plugins sits 1 below). */
static void StripScanTree(const WCHAR *dir, int depth)
{
    WCHAR pattern[MAX_PATH], full[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    if (swprintf_s(pattern, MAX_PATH, L"%s\\*", dir) < 0)
        return;
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        if (swprintf_s(full, MAX_PATH, L"%s\\%s", dir, fd.cFileName) < 0)
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth > 0 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                StripScanTree(full, depth - 1);
        } else if (EndsWithI(fd.cFileName, L"Strip Header.dll")) {
            StripScanFile(full);
        }
    } while (!StripAllFound() && FindNextFileW(h, &fd));
    FindClose(h);
}

/* Every *.dll directly in Dir (e.g. apollo-win\ua-files). */
static void StripScanDlls(const WCHAR *dir)
{
    WCHAR pattern[MAX_PATH], full[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    if (swprintf_s(pattern, MAX_PATH, L"%s\\*.dll", dir) < 0)
        return;
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "No .dll files in %ls\n", dir);
        return;
    }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (swprintf_s(full, MAX_PATH, L"%s\\%s", dir, fd.cFileName) >= 0)
            StripScanFile(full);
    } while (!StripAllFound() && FindNextFileW(h, &fd));
    FindClose(h);
}

/*
 * Find the five pinned records: in DirArg's DLLs if given, else UAD's strip-header plug-ins
 * under Program Files, then System32\UAD2SDK.dll (oa-assets firmware.rs:236-259).
 */
static BOOL StripFind(const char *dirArg)
{
    static const WCHAR *roots[] = { L"ProgramFiles", L"ProgramW6432", L"ProgramFiles(x86)" };
    WCHAR base[MAX_PATH], path[MAX_PATH];
    ULONG j, i;

    ZeroMemory(g_strip, sizeof(g_strip));
    if (dirArg != NULL) {
        if (MultiByteToWideChar(CP_ACP, 0, dirArg, -1, path, MAX_PATH) == 0) {
            fprintf(stderr, "bad folder name\n");
            return FALSE;
        }
        printf("Searching the DLLs in %ls\n", path);
        StripScanDlls(path);
    } else {
        for (i = 0; i < sizeof(roots) / sizeof(roots[0]) && !StripAllFound(); i++) {
            DWORD n = GetEnvironmentVariableW(roots[i], base, MAX_PATH);

            if (n == 0 || n >= MAX_PATH)
                continue;
            if (swprintf_s(path, MAX_PATH, L"%s\\Universal Audio\\Powered Plugins", base) < 0)
                continue;
            printf("Searching %ls\n", path);
            StripScanTree(path, 3);
        }
        if (!StripAllFound()) {
            UINT n = GetSystemDirectoryW(base, MAX_PATH);

            if (n > 0 && n < MAX_PATH &&
                swprintf_s(path, MAX_PATH, L"%s\\UAD2SDK.dll", base) >= 0) {
                printf("Searching %ls\n", path);
                StripScanFile(path);
            }
        }
    }
    for (j = 0; j < APOLLO_STRIP_MODULES; j++) {
        const APOLLO_STRIP_PIN *pin = &g_ApolloStripPins[j];

        if (g_strip[j].Found)
            printf("  0x%03x  %4u bytes  pinned SHA-256 matches  (%ls @ 0x%lx)\n", pin->Id,
                   pin->Length, g_strip[j].From, g_strip[j].Offset);
        else
            printf("  0x%03x  NOT FOUND with the pinned bytes\n", pin->Id);
    }
    return StripAllFound();
}

static BOOL StripInstallDir(WCHAR *dir, size_t cch)
{
    WCHAR win[MAX_PATH];
    UINT n = GetSystemWindowsDirectoryW(win, MAX_PATH);

    if (n == 0 || n >= MAX_PATH)
        return FALSE;
    return swprintf_s(dir, cch, L"%s\\%s", win, APOLLO_STRIP_DIR_UNDER_ROOT) >= 0;
}

/*
 * What apollo.sys will find: each installed file checked against its pin. The folder is
 * admin-only, so without elevation the files cannot be read: *Denied is set then.
 */
static BOOL StripCheckInstalled(BOOL *denied)
{
    WCHAR dir[MAX_PATH], path[MAX_PATH];
    BOOL all = TRUE;
    ULONG j;

    *denied = FALSE;
    if (!StripInstallDir(dir, MAX_PATH))
        return FALSE;
    printf("Installed in %ls:\n", dir);
    for (j = 0; j < APOLLO_STRIP_MODULES; j++) {
        const APOLLO_STRIP_PIN *pin = &g_ApolloStripPins[j];
        DWORD size = 0;
        UCHAR *data;

        if (swprintf_s(path, MAX_PATH, L"%s\\%s", dir, pin->FileName) < 0)
            return FALSE;
        data = ReadWholeFile(path, &size);
        if (data == NULL) {
            if (GetLastError() == ERROR_ACCESS_DENIED) {
                printf("  %ls  cannot be read without elevation (admin-only folder)\n",
                       pin->FileName);
                *denied = TRUE;
            } else {
                printf("  %ls  missing\n", pin->FileName);
            }
            all = FALSE;
            continue;
        }
        if (StripRecordMatches(pin, data, size)) {
            printf("  %ls  ok\n", pin->FileName);
        } else {
            printf("  %ls  NOT the pinned bytes (%lu bytes)\n", pin->FileName, size);
            all = FALSE;
        }
        free(data);
    }
    return all;
}

static int CmdStripsVerify(const char *dirArg)
{
    BOOL found = StripFind(dirArg);
    BOOL installed, denied = FALSE;

    printf("\n");
    installed = StripCheckInstalled(&denied);
    printf("\n%s\n", found ? "Source: all five pinned modules found." :
                             "Source: not all five pinned modules found.");
    if (installed)
        printf("Installed: all five present and pinned; apollo.sys can use them.\n");
    else if (denied)
        printf("Installed: not checked; the folder is admin-only, so run 'apollo-diag strips verify'\n"
               "from an elevated prompt to check the installed copies.\n");
    else
        printf("Installed: incomplete; run 'apollo-diag strips install' elevated.\n");
    return (found || installed) ? 0 : 1;
}

/* A protected DACL from SDDL onto a file or folder. ERROR_SUCCESS or the error. */
static DWORD StripSetDacl(WCHAR *path, const WCHAR *sddl)
{
    PSECURITY_DESCRIPTOR sd = NULL;
    PACL dacl = NULL;
    BOOL present = FALSE, defaulted = FALSE;
    DWORD err;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, NULL))
        return GetLastError();
    if (!GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) || !present) {
        err = GetLastError() != ERROR_SUCCESS ? GetLastError() : ERROR_INVALID_SECURITY_DESCR;
        LocalFree(sd);
        return err;
    }
    err = SetNamedSecurityInfoW(path, SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                NULL, NULL, dacl, NULL);
    LocalFree(sd);
    return err;
}

/*
 * Elevated: write the five records, byte for byte, to %SystemRoot%\System32\drivers\ApolloStrips,
 * folder and files admin-only (SYSTEM and Administrators full, no one else). Files left by an
 * earlier install get the same DACL again. UA's bytes stay on this machine: never commit or
 * share the directory's contents.
 */
static int CmdStripsInstall(const char *dirArg)
{
    WCHAR dir[MAX_PATH], path[MAX_PATH];
    BOOL denied = FALSE;
    DWORD err;
    ULONG j;

    if (!StripFind(dirArg)) {
        fprintf(stderr, "\nNot all five pinned strip modules were found: nothing installed.\n"
                        "Pass the folder with UA's DLLs (for example apollo-win\\ua-files).\n");
        return 1;
    }
    if (!StripInstallDir(dir, MAX_PATH)) {
        fprintf(stderr, "Cannot work out the Windows folder\n");
        return 1;
    }
    if (!CreateDirectoryW(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        if (GetLastError() == ERROR_ACCESS_DENIED)
            fprintf(stderr, "Access denied: run this from an elevated (Run as administrator) prompt.\n");
        else
            PrintLastError("Creating the strip module folder");
        return 1;
    }
    err = StripSetDacl(dir, STRIP_SDDL);
    if (err != ERROR_SUCCESS) {
        SetLastError(err);
        if (err == ERROR_ACCESS_DENIED)
            fprintf(stderr, "Access denied: run this from an elevated (Run as administrator) prompt.\n");
        else
            PrintLastError("Setting the folder's access control");
        return 1;
    }
    for (j = 0; j < APOLLO_STRIP_MODULES; j++) {
        const APOLLO_STRIP_PIN *pin = &g_ApolloStripPins[j];
        HANDLE h;
        DWORD wrote = 0;

        if (swprintf_s(path, MAX_PATH, L"%s\\%s", dir, pin->FileName) < 0)
            return 1;
        /* An existing file keeps its own DACL through CREATE_ALWAYS: tighten it first. */
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
            err = StripSetDacl(path, STRIP_FILE_SDDL);
            if (err != ERROR_SUCCESS) {
                SetLastError(err);
                PrintLastError("Setting an existing module file's access control");
                return 1;
            }
        }
        h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_SHARING_VIOLATION)
                fprintf(stderr, "%ls is open in another program: close it (or reboot) and retry.\n",
                        path);
            else
                PrintLastError("Creating a module file");
            return 1;
        }
        if (!WriteFile(h, g_strip[j].Rec, pin->Length, &wrote, NULL) || wrote != pin->Length) {
            PrintLastError("Writing a module file");
            CloseHandle(h);
            return 1;
        }
        CloseHandle(h);
        err = StripSetDacl(path, STRIP_FILE_SDDL);
        if (err != ERROR_SUCCESS) {
            SetLastError(err);
            PrintLastError("Setting a module file's access control");
            return 1;
        }
    }
    printf("\n");
    if (!StripCheckInstalled(&denied)) {
        fprintf(stderr, "The written files do not read back as the pinned bytes.\n");
        return 1;
    }
    printf("\nInstalled. apollo.sys reads these at its next strip pass (after a cold start).\n");
    return 0;
}

static const char *StripsStateName(ULONG s)
{
    switch (s) {
    case APOLLO_STRIPS_STATE_OFF:      return "Off";
    case APOLLO_STRIPS_STATE_IDLE:     return "Idle";
    case APOLLO_STRIPS_STATE_ARMED:    return "Armed";
    case APOLLO_STRIPS_STATE_BUILDING: return "Building";
    case APOLLO_STRIPS_STATE_LIVE:     return "Live (pass complete)";
    case APOLLO_STRIPS_STATE_FAILED:   return "FAILED";
    default:                           return "unknown";
    }
}

static const char *StripsLayoutText(ULONG code)
{
    switch (code) {
    case 0:           return "matches the recipe (room for the model's strips: x8p 8, Twin X 2)";
    case 1:           return "a pool register reads all-ones";
    case 2:           return "the code pool does not start at 0xe0000";
    case 3:           return "the code pool is too small";
    case 5:           return "pool 1 does not end at 0x9d1b8";
    case 6:           return "pool 2 does not end at 0xc6ffe";
    case 7:           return "pool 3 does not end at 0x08ffe000";
    case 9:           return "pool 1 has no room for the strips";
    case 10:          return "pool 2 has no room for the strips";
    case 11:          return "pool 3 has no room for the strips";
    case 0xFFFFFFFFu: return "not read yet";
    default:          return "does not match";
    }
}

static const char *StripFileText(LONG st)
{
    switch ((ULONG)st) {
    case 0x00000000u: return "verified";
    case 0x00000103u: return "not read yet";
    case 0xC0000034u:
    case 0xC000003Au: return "MISSING (apollo-diag strips install)";
    case 0xC0000004u: return "length is not the pinned one";
    case 0xC000007Bu: return "header is not the pinned one";
    case 0xC0000428u: return "SHA-256 is not the pinned one";
    case 0xC0000043u: return "open elsewhere (oplock or share)";
    case 0xC0000022u: return "access denied";
    default:          return "read error";
    }
}

static const char *StripModText(ULONG s)
{
    switch (s) {
    case APOLLO_STRIP_MOD_NONE:      return "-";
    case APOLLO_STRIP_MOD_UPLOADING: return "UNPROVEN";
    case APOLLO_STRIP_MOD_RESIDENT:  return "resident";
    case APOLLO_STRIP_MOD_REFUSED:   return "REFUSED";
    default:                         return "?";
    }
}

static const char *StripStageText(ULONG st)
{
    switch (st) {
    case 0: return "nothing yet";
    case 1: return "uploads";
    case 2: return "+ zero-fills";
    case 3: return "+ memspec";
    case 4: return "+ header";
    case 5: return "+ parameters";
    case 6: return "+ SYNCH (started)";
    case 7: return "+ span/route on the strip DSP";
    case 8: return "+ bus owner, routes on other DSPs";
    case 9: return "+ 48V resync (everything)";
    default: return "?";
    }
}

static const char *StripPhantomText(ULONG st)
{
    switch (st) {
    case APOLLO_STRIPS_PHANTOM_NOT_RUN:       return "not run";
    case APOLLO_STRIPS_PHANTOM_DONE:          return "done";
    case APOLLO_STRIPS_PHANTOM_NONE_ON:       return "nothing to do: no preamp in StripMask has 48V on";
    case APOLLO_STRIPS_PHANTOM_NO_READBACK:   return "skipped: no hardware readback";
    case APOLLO_STRIPS_PHANTOM_NOT_COMMITTED: return "written, but a mailbox batch was not taken in time";
    default:                                  return "?";
    }
}

/* [Warm] GET_STRIPS version 3 (docs/STRIPS.md 12) */
static const char *StripAdoptName(ULONG v)
{
    switch (v) {
    case APOLLO_STRIPS_ADOPT_NONE:          return "- (cold start, or strips off)";
    case APOLLO_STRIPS_ADOPT_ADOPTED:       return "ADOPTED: this driver's own strips were running, nothing sent";
    case APOLLO_STRIPS_ADOPT_RESTORE:       return "ADOPTED, plain strips restored (a plug-in may have been on them)";
    case APOLLO_STRIPS_ADOPT_REBUILD:       return "REBUILT at a warm start (only this driver used the DSPs since power-up)";
    case APOLLO_STRIPS_ADOPT_NOT_ANCHORED:  return "not adopted: not proven this driver's (see Anchor)";
    case APOLLO_STRIPS_ADOPT_NOT_RUNNING:   return "not adopted: none of this driver's strips run there (StripWarmRebuild 0)";
    case APOLLO_STRIPS_ADOPT_UNKNOWN_STATE: return "not adopted: the last session ended mid-pass (StripWarmRebuild 0)";
    case APOLLO_STRIPS_ADOPT_TORN_DOWN:     return "not adopted: the last exit tore them down (StripWarmRebuild 0)";
    case APOLLO_STRIPS_ADOPT_PLAN_DIFFERS:  return "not adopted: the running strips are not this start's plan (StripWarmRebuild 0)";
    case APOLLO_STRIPS_ADOPT_POOLS_DIFFER:  return "not adopted: the strip DSP's pools moved";
    case APOLLO_STRIPS_ADOPT_DISABLED:      return "not adopted: StripAdopt is 0";
    case APOLLO_STRIPS_ADOPT_DROPPED:       return "not rebuilt: the warm ring connect dropped them (StripWarmRebuild 0)";
    case APOLLO_STRIPS_ADOPT_LAYOUT:        return "not adopted: the pools do not fit the recipe";
    default:                                return "?";
    }
}

static const char *StripAnchorName(ULONG a)
{
    switch (a) {
    case APOLLO_STRIPS_ANCHOR_OK:          return "only this driver used the DSPs since the last power-up";
    case APOLLO_STRIPS_ANCHOR_COLD_USED:   return "cold start, but a host used a DSP since power-up";
    case APOLLO_STRIPS_ANCHOR_NO_RECORD:   return "no record (unsigned: built by a driver without adoption records)";
    case APOLLO_STRIPS_ANCHOR_BAD_RECORD:  return "the record is damaged or from another version";
    case APOLLO_STRIPS_ANCHOR_OTHER_UNIT:  return "the record is another unit's or another firmware's";
    case APOLLO_STRIPS_ANCHOR_LOST_BEFORE: return "an earlier start lost track of the DSPs";
    case APOLLO_STRIPS_ANCHOR_OTHER_HOST:  return "the boot words changed: another host connected since";
    case APOLLO_STRIPS_ANCHOR_NO_ADVANCE:  return "this start's connect did not move the boot words";
    case APOLLO_STRIPS_ANCHOR_NOT_READ:    return "not read (no unit key, or the strips are not run on this unit)";
    default:                               return "?";
    }
}

/* (days since 2007-01-01 << 12) + minute of day, UTC */
static void StripDateText(ULONG d, char *out, size_t cap)
{
    ULARGE_INTEGER t;
    FILETIME ft;
    SYSTEMTIME st;

    if (d == 0) {
        sprintf_s(out, cap, "-");
        return;
    }
    t.QuadPart = 128120832000000000ULL + ((ULONGLONG)(d >> 12) * 86400ULL + (ULONGLONG)(d & 0xFFF) * 60ULL) * 10000000ULL;
    ft.dwLowDateTime = t.LowPart;
    ft.dwHighDateTime = t.HighPart;
    if (!FileTimeToSystemTime(&ft, &st)) {
        sprintf_s(out, cap, "0x%08lx", d);
        return;
    }
    sprintf_s(out, cap, "%04u-%02u-%02u %02u:%02u UTC", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
}

static int CmdStrips(void)
{
    static const struct { ULONG bit; const char *name; } flags[] = {
        { APOLLO_STRIPS_FLAG_OWNED,       "OWNED" },
        { APOLLO_STRIPS_FLAG_QUARANTINED, "QUARANTINED" },
        { APOLLO_STRIPS_FLAG_TAINTED,     "TAINTED" },
        { APOLLO_STRIPS_FLAG_MODULES_OK,  "MODULES_OK" },
        { APOLLO_STRIPS_FLAG_BOOT_READ,   "BOOT_READ" },
        { APOLLO_STRIPS_FLAG_LAYOUT_READ, "LAYOUT_READ" },
        { APOLLO_STRIPS_FLAG_POOL_READY,  "POOL_READY" },
    };
    APOLLO_STRIPS_INFO s;
    DWORD ret = 0;
    ULONG i;

    ZeroMemory(&s, sizeof(s));
    if (!DeviceIoControl(g_dev, IOCTL_APOLLO_GET_STRIPS, NULL, 0, &s, (DWORD)sizeof(s), &ret, NULL)) {
        if (GetLastError() == ERROR_INVALID_FUNCTION)
            fprintf(stderr, "This apollo.sys has no input strips (GET_STRIPS): install the newer driver.\n");
        else
            PrintLastError("GET_STRIPS");
        return 1;
    }
    /* A version-1 driver (the T3 build) returns the shorter struct: show what it has. */
    if (ret < (DWORD)FIELD_OFFSET(APOLLO_STRIPS_INFO, StepsLimit)) {
        fprintf(stderr, "GET_STRIPS returned %lu bytes, expected %lu: driver and tool are out of step.\n",
                ret, (unsigned long)sizeof(s));
        return 1;
    }
    if (ret < sizeof(s))
        s.Version = (ret >= APOLLO_STRIPS_INFO_V2_SIZE) ? 2 : 1;   /* [Warm] a version 2 driver: its part */
    s.Why[sizeof(s.Why) - 1] = 0;
    printf("State        %lu  %s\n", s.State, StripsStateName(s.State));
    printf("Why          ");
    PrintSafeString(s.Why, sizeof(s.Why));
    printf("\n");
    printf("Registry     StripsEnable=%lu StripSteps=%lu StripMask=0x%02lX StripDsp=%lu\n",
           s.Enable, s.Steps, s.Mask, s.Dsp);
    printf("Flags        0x%02lX ", s.Flags);
    for (i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
        if (s.Flags & flags[i].bit)
            printf(" %s", flags[i].name);
    }
    printf("\n");
    printf("Boot words  ");
    for (i = 0; i < 8; i++)
        printf(" %08lx", s.BootWord[i]);
    printf("   (owned when (word & ~0xff00) == 3)\n");
    printf("DSP %lu pools  size %08lx %08lx %08lx %08lx base %08lx %08lx %08lx %08lx scratch %08lx %08lx %08lx\n",
           s.Dsp, s.Geometry[0], s.Geometry[1], s.Geometry[2], s.Geometry[3], s.Geometry[4],
           s.Geometry[5], s.Geometry[6], s.Geometry[7], s.Geometry[8], s.Geometry[9], s.Geometry[10]);
    printf("Layout       %s (code %lu)\n", StripsLayoutText(s.LayoutWhy), s.LayoutWhy);
    printf("Module  dest      file                                  on DSP %lu\n", s.Dsp);
    for (i = 0; i < APOLLO_STRIP_MODULE_SLOTS; i++) {
        const APOLLO_STRIP_MODULE_INFO *m = &s.Module[i];

        printf("0x%03lx   0x%05lx   %-36s  %s", m->Id, m->Dest, StripFileText(m->FileStatus),
               StripModText(m->DspState));
        if (m->DspState == APOLLO_STRIP_MOD_RESIDENT || m->DspState == APOLLO_STRIP_MOD_REFUSED)
            printf("  (reply %08lx %08lx %08lx %08lx)", m->Reply[0], m->Reply[1], m->Reply[2],
                   m->Reply[3]);
        if (m->FileStatus != 0 && m->FileStatus != 0x103)
            printf("  [file 0x%08lX]", (ULONG)m->FileStatus);
        printf("\n");
    }
    printf("Counters     passes %lu, uploads %lu (replies ok %lu, bad %lu), timeouts %lu, teardowns %lu\n",
           s.Passes, s.Uploads, s.RepliesOk, s.RepliesBad, s.Timeouts, s.Teardowns);
    if (s.Timeouts)
        printf("Last unproven transaction: cmd end %lu pos %lu, resp end %lu pos %lu\n",
               s.LastCmdEnd, s.LastCmdPos, s.LastRespEnd, s.LastRespPos);
    if (s.Version < 2)
        return 0;

    printf("Stages       asked %lu (%s), done %lu (%s)\n", s.StepsLimit,
           StripStageText(s.StepsLimit), s.StepsDone, StripStageText(s.StepsDone));
    printf("Mixer batch  %lu (%s)\n", s.MixerInitBatch,
           s.MixerInitBatch == APOLLO_MIXER_BATCH_NONE ? "none" :
           s.MixerInitBatch == APOLLO_MIXER_BATCH_X8P_SAFE ? "model-safe (x8p: preamp fields skipped; Twin X: "
           "spec-twinx 4.3)" : "legacy full");
    printf("48V resync   %s", StripPhantomText(s.PhantomState));
    if (s.PhantomState == APOLLO_STRIPS_PHANTOM_DONE ||
        s.PhantomState == APOLLO_STRIPS_PHANTOM_NOT_COMMITTED)
        printf(" (preamp mask 0x%02lx)", s.PhantomMask);
    printf("\n");
    printf("Input  strip                 SYNCH  header    taken   bus on DSP\n");
    for (i = 0; i < APOLLO_STRIP_INPUTS; i++) {
        if (!(s.Mask & (1ul << i)) && s.StripState[i] == APOLLO_STRIP_NONE)
            continue;
        printf("%-5lu  %-20s  %-5s  0x%05lx   %4lu ms  %lu\n", i + 1,
               s.StripState[i] == APOLLO_STRIP_TAKEN ? "taken" :
               s.StripState[i] == APOLLO_STRIP_QUEUED ? "QUEUED (unproven)" : "-",
               s.StripStarted[i] ? "sent" : "-", s.StripHeader[i], s.StripAckMs[i], s.BusOwner[i]);
    }
    printf("Routes       %lu route word(s) sent to the other DSPs\n", s.RoutesSent);
    if (s.Version >= 3) {
        static const struct { ULONG bit; const char *name; } af[] = {
            { APOLLO_STRIPS_AF_ADOPTED,      "ADOPTED" },
            { APOLLO_STRIPS_AF_REBUILT_WARM, "REBUILT_WARM" },
            { APOLLO_STRIPS_AF_RECORD,       "RECORD" },
            { APOLLO_STRIPS_AF_KEEP,         "KEEP_AT_EXIT" },
            { APOLLO_STRIPS_AF_WARM_REBUILD, "WARM_REBUILD" },
            { APOLLO_STRIPS_AF_PLUGINS_OK,   "PLUGINS_OK" },
            { APOLLO_STRIPS_AF_ANCHORED,     "ANCHORED" },
            { APOLLO_STRIPS_AF_WARM_PLUGINS, "WARM_PLUGINS" },
            { APOLLO_STRIPS_AF_UPLOADS_PROVEN, "UPLOADS_PROVEN" },
        };
        char built[40];

        StripDateText(s.RecordBuiltDate, built, sizeof(built));
        printf("Warm start   %s\n", StripAdoptName(s.Adopt));
        printf("Anchor       %lu: %s\n", s.Anchor, StripAnchorName(s.Anchor));
        printf("Adoption     0x%02lX", s.AdoptFlags);
        for (i = 0; i < sizeof(af) / sizeof(af[0]); i++) {
            if (s.AdoptFlags & af[i].bit)
                printf(" %s", af[i].name);
        }
        printf("\n");
        if (s.RecordState == APOLLO_STRIPS_REC_ABSENT) {
            printf("Record       none kept (status 0x%08lX)\n", (ULONG)s.RecordStatus);
        } else {
            printf("Record       strips %s, built %s, adopted %lu time(s), %lu plug-in(s) on the strip DSP%s\n",
                   s.RecordState == APOLLO_STRIPS_REC_RUNNING ? "RUNNING" :
                   s.RecordState == APOLLO_STRIPS_REC_NONE ? "none" : "unknown", built, s.RecordAdoptions,
                   s.RecordPlugins & 0x7FFFFFFFul,
                   (s.RecordPlugins & 0x80000000ul) ? " (STALE: a restored chain's modules may remain: plug-ins after a "
                   "power cycle)" : "");
            printf("Record boot ");
            for (i = 0; i < 8; i++)
                printf(" %08lx", s.RecordBootWord[i]);
            printf("   (after this driver's last ring connect)\n");
        }
    }
    printf("(input 1 = record slot 0: watch it with 'apollo-diag levels')\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* engine: the 1 ms copy engine's counters (GET_ENGINE_STATS)          */
/* ------------------------------------------------------------------ */

static BOOL GetEngineStats(APOLLO_ENGINE_STATS *s, BOOL reset)
{
    ULONG flags = reset ? APOLLO_ENGINE_RESET : 0;
    DWORD ret = 0;

    ZeroMemory(s, sizeof(*s));
    if (!Ioctl(IOCTL_APOLLO_GET_ENGINE_STATS, "GET_ENGINE_STATS", &flags, (DWORD)sizeof(flags), s,
               (DWORD)sizeof(*s), &ret))
        return FALSE;
    if (ret < sizeof(*s)) {
        fprintf(stderr, "GET_ENGINE_STATS returned %lu bytes, expected %lu: driver and tool are out "
                        "of step.\n", ret, (unsigned long)sizeof(*s));
        return FALSE;
    }
    return TRUE;
}

static const char *AsioStateName(ULONG s)
{
    switch (s) {
    case APOLLO_ASIO_STATE_CLOSED:  return "closed";
    case APOLLO_ASIO_STATE_OPEN:    return "open (buffers mapped, not streaming)";
    case APOLLO_ASIO_STATE_RUNNING: return "running";
    default:                        return "?";
    }
}

static void PrintEngineStats(const APOLLO_ENGINE_STATS *s)
{
    printf("Engine       %s, %lu start(s), lead %lu frames, capture guard %lu, zero-ahead %lu\n",
           s->EngineLost ? "LOST (device not answering; restart the transport or replug)" :
           s->Running ? "RUNNING (1 ms tick)" : "stopped", s->Starts, s->RenderLeadFrames,
           s->CaptureGuardFrames, s->ZeroAheadFrames);
    printf("Ticks        %llu, position %llu\n", s->Ticks, s->Position);
    printf("Tick timing  max interval %lu us, max duration %lu us, intervals > 2 ms: %lu, > 4 ms: %lu\n",
           s->MaxTickIntervalUs, s->MaxTickDurationUs, s->IntervalsOver2ms, s->IntervalsOver4ms);
    printf("Problems     late ticks %lu (render frames lost), bus stalls %lu, dead reads %lu, "
           "wrap fixes %lu\n", s->LateTicks, s->BusStalls, s->DeadReads, s->WrapFixes);
    printf("Wraps (EOB)  %lu end-of-buffer interrupts\n", s->EndOfBufferCount);
    printf("ASIO         %s", AsioStateName(s->AsioState));
    if (s->AsioState != APOLLO_ASIO_STATE_CLOSED)
        printf(", %lu frames, %lu in, %lu out", s->AsioBufferFrames, s->AsioInputs, s->AsioOutputs);
    printf("\n             switches %lu, underruns %lu, overruns %lu, resyncs %lu, client done %lu, "
           "skipped by the DLL %lu\n", s->AsioSwitches, s->AsioUnderruns, s->AsioOverruns, s->AsioResyncs,
           s->AsioClientDone, s->AsioClientSkipped);
}

static int CmdEngine(int n, char **a)
{
    APOLLO_ENGINE_STATS s0, s1;
    double seconds = 0.0, el;
    BOOL reset = HasFlag(n, a, 1, "--reset");
    int i;

    for (i = 1; i < n; i++) {
        if (a[i][0] != '-' && (!ParseDouble(a[i], &seconds) || seconds < 0.0)) {
            fprintf(stderr, "usage: engine [seconds] [--reset]\n");
            return 1;
        }
    }
    if (!GetEngineStats(&s0, reset && seconds <= 0.0))
        return 1;
    if (s0.Version != APOLLO_ENGINE_STATS_VERSION)
        fprintf(stderr, "warning: engine stats version %lu, this tool expects %d\n", s0.Version,
                APOLLO_ENGINE_STATS_VERSION);
    if (seconds <= 0.0) {
        PrintEngineStats(&s0);
        if (reset)
            printf("(maxima and event counters reset)\n");
        return 0;
    }
    if (reset) {
        if (!GetEngineStats(&s0, TRUE) || !GetEngineStats(&s0, FALSE))
            return 1;
        printf("(maxima and event counters reset; watching %.1f s)\n", seconds);
    }
    SleepMs((DWORD)(seconds * 1000.0));
    if (!GetEngineStats(&s1, FALSE))
        return 1;
    PrintEngineStats(&s1);
    if (s1.QpcFrequency > 0 && s1.Qpc > s0.Qpc) {
        el = (double)(s1.Qpc - s0.Qpc) / (double)s1.QpcFrequency;
        printf("Over %.2f s: %.1f ticks/s, %.2f frames/s, ASIO %.1f switches/s, +%lu late, "
               "+%lu underruns, +%lu overruns\n", el, (double)(s1.Ticks - s0.Ticks) / el,
               (double)(LONGLONG)(s1.Position - s0.Position) / el,
               (double)(s1.AsioSwitches - s0.AsioSwitches) / el, s1.LateTicks - s0.LateTicks,
               s1.AsioUnderruns - s0.AsioUnderruns, s1.AsioOverruns - s0.AsioOverruns);
    }
    return (s1.Running && s1.LateTicks == s0.LateTicks && s1.DeadReads == s0.DeadReads) ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* [Rate] rate: the unit's sample rate (GET_RATE / SET_RATE)           */
/* ------------------------------------------------------------------ */

static const char *RateErrText(ULONG e)
{
    switch (e) {
    case APOLLO_RATE_ERR_NONE:          return "done";
    case APOLLO_RATE_ERR_RATE:          return "not one of the six rates";
    case APOLLO_RATE_ERR_UNSUPPORTED:   return "not offered by the unit";
    case APOLLO_RATE_ERR_NOT_READY:     return "the unit is not running";
    case APOLLO_RATE_ERR_ASIO:          return "an ASIO host has the unit";
    case APOLLO_RATE_ERR_COMBINED:      return "the unit runs in the combined device (use --combined)";
    case APOLLO_RATE_ERR_COMBINED_ASIO: return "an ASIO host has the combined device";
    case APOLLO_RATE_ERR_BUSY:          return "another change is under way";
    case APOLLO_RATE_ERR_LOCK:          return "the clock did not lock";
    case APOLLO_RATE_ERR_HARDWARE:      return "the unit stopped answering";
    case APOLLO_RATE_ERR_NO_COMBINED:   return "no combined device runs";
    case APOLLO_RATE_ERR_PARTIAL:       return "only part of the combined device changed";
    default:                            return "?";
    }
}

static void RateMaskText(ULONG Mask, char *Out, size_t Len)
{
    static const char *names[7] = { "", "44.1", "48", "88.2", "96", "176.4", "192" };
    ULONG i;

    Out[0] = 0;
    for (i = 1; i < 7; i++) {
        if (Mask & (1ul << i)) {
            strcat_s(Out, Len, Out[0] ? " " : "");
            strcat_s(Out, Len, names[i]);
        }
    }
    if (Out[0] == 0)
        strcpy_s(Out, Len, "none");
}

static void PrintRate(const APOLLO_RATE_INFO *r)
{
    char offered[64], allowed[64], comb[64];

    RateMaskText(r->Offered, offered, sizeof(offered));
    RateMaskText(r->Allowed, allowed, sizeof(allowed));
    RateMaskText(r->CombinedAllowed, comb, sizeof(comb));
    printf("Rate             %lu Hz (index %lu)%s%s\n", r->Rate, r->RateIndex,
           (r->Flags & APOLLO_RATE_F_LOCKED) ? ", locked" : ", NOT locked",
           (r->Flags & APOLLO_RATE_F_CHANGING) ? ", changing" : "");
    printf("RateInfo         0x%08lX (0xC07C: index %lu, lock %lu)\n", r->RateInfo, (r->RateInfo >> 8) & 0xFF,
           r->RateInfo & 3);
    printf("Offered          %s kHz\n", offered);
    printf("Allowed          %s kHz (clock source 0x%02lX)\n", allowed, r->ClockSource);
    printf("Stored           %lu Hz%s (the rate the unit comes up at)\n", r->Stored,
           (r->Flags & APOLLO_RATE_F_STORED) ? " (in its unit key)" : " (default)");
    printf("At this rate     FRAME_CFG %lu, lead %lu, guard %lu, ADC %lu frames\n", r->FrameCfg, r->LeadFrames,
           r->GuardFrames, r->AdcFrames);
    /* [Lat] what ASIO and the Windows endpoints report beyond the buffer (docs/ASIO.md, Latency) */
    printf("Reported         input %lu (guard + ADC + strip path %lu), output %lu (lead + FRAME_CFG + DAC %lu) frames + buffer\n",
           r->GuardFrames + r->AdcFrames + r->InPathFrames, r->InPathFrames, r->LeadFrames + r->FrameCfg + r->DacFrames,
           r->DacFrames);
    printf("Last change     %s (%lu), %lu ms, %lu change(s) since load%s%s\n", RateErrText(r->Result), r->Result,
           r->LastMs, r->Changes, r->Status ? ", status " : "", r->Status ? "set" : "");
    printf("Why              %.*s\n", (int)strnlen(r->Why, sizeof(r->Why)), r->Why);
    printf("Flags            0x%04lX%s%s%s%s%s%s%s%s%s\n", r->Flags,
           (r->Flags & APOLLO_RATE_F_COMBINED) ? " combined" : "",
           (r->Flags & APOLLO_RATE_F_ASIO_OPEN) ? " asio-open" : "",
           (r->Flags & APOLLO_RATE_F_STREAMS) ? " windows-streams" : "",
           (r->Flags & APOLLO_RATE_F_STRIPS_REBUILT) ? " strips-rebuilt" : "",
           (r->Flags & APOLLO_RATE_F_PLUGINS_48K) ? " plugins-48k-only" : "",
           (r->Flags & APOLLO_RATE_F_CORR_MISMATCH) ? " correction-rate-mismatch" : "",
           (r->Flags & APOLLO_RATE_F_COMBINED_ASIO) ? " combined-asio-open" : "",
           (r->Flags & APOLLO_RATE_F_COMBINED_MISMATCH) ? " combined-rate-mismatch" : "",
           (r->Flags & APOLLO_RATE_F_STORED) ? " stored" : "");
    if (r->CombinedRate != 0 || r->CombinedUnits != 0)
        printf("Combined         %lu Hz on %lu unit(s); every unit allows %s kHz; stored %lu Hz\n", r->CombinedRate,
               r->CombinedUnits, comb, r->CombinedStored);
}

/* rate | rate <Hz> [--combined] [--no-store] */
static int CmdRate(int n, char **a)
{
    APOLLO_RATE_INFO r;
    APOLLO_RATE_SET q;
    ULONG hz = 0;
    DWORD ret = 0;
    LARGE_INTEGER f, t0, t1;

    if (n >= 2 && a[1][0] != '-') {
        if (!ParseU32(a[1], &hz)) {
            fprintf(stderr, "usage: rate [<Hz: 44100 48000 88200 96000 176400 192000> [--combined] [--no-store]]\n");
            return 1;
        }
        ZeroMemory(&q, sizeof(q));
        q.Version = APOLLO_RATE_VERSION;
        q.Rate = hz;
        q.Flags = (HasFlag(n, a, 2, "--combined") ? APOLLO_RATE_SET_COMBINED : 0) |
                  (HasFlag(n, a, 2, "--no-store") ? APOLLO_RATE_SET_NO_STORE : 0);
        printf("Setting %lu Hz%s (a few seconds per unit)...\n", hz, (q.Flags & APOLLO_RATE_SET_COMBINED) ?
               " on the combined device" : "");
        ZeroMemory(&r, sizeof(r));
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        if (!Ioctl(IOCTL_APOLLO_SET_RATE, "SET_RATE", &q, (DWORD)sizeof(q), &r, (DWORD)sizeof(r), &ret))
            return 1;
        QueryPerformanceCounter(&t1);
        printf("SET_RATE answered in %.0f ms: %s\n\n", (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart,
               RateErrText(r.Result));
        PrintRate(&r);
        return r.Result == APOLLO_RATE_ERR_NONE ? 0 : 1;
    }
    ZeroMemory(&r, sizeof(r));
    if (!Ioctl(IOCTL_APOLLO_GET_RATE, "GET_RATE", NULL, 0, &r, (DWORD)sizeof(r), &ret))
        return 1;
    if (ret < sizeof(r) || r.Version != APOLLO_RATE_VERSION) {
        fprintf(stderr, "GET_RATE returned %lu bytes (version %lu): driver and tool are out of step.\n", ret,
                r.Version);
        return 1;
    }
    PrintRate(&r);
    return 0;
}

/* ------------------------------------------------------------------ */
/* endpoints: the Windows audio endpoints (GET_ENDPOINTS)              */
/* ------------------------------------------------------------------ */

/*
 * [Endpoints] GET_ENDPOINTS v2: one call with room for APOLLO_ENDPOINTS_ROWS_MAX rows (a v1 driver fills
 * 16 and says sizeof(APOLLO_ENDPOINTS)). Rows() = how many came back.
 */
typedef struct _DIAG_ENDPOINTS {
    APOLLO_ENDPOINTS        Head;           /* header + rows 0-15 */
    APOLLO_ENDPOINT_INFO    More[APOLLO_ENDPOINTS_ROWS_MAX - APOLLO_ENDPOINTS_MAX];
    ULONG                   Bytes;          /* what the driver returned */
} DIAG_ENDPOINTS;

static const APOLLO_ENDPOINT_INFO *EpRow(const DIAG_ENDPOINTS *e, ULONG i)
{
    return (const APOLLO_ENDPOINT_INFO *)((const BYTE *)&e->Head + FIELD_OFFSET(APOLLO_ENDPOINTS, Endpoint) +
                                          (SIZE_T)i * sizeof(APOLLO_ENDPOINT_INFO));
}

static ULONG EpRows(const DIAG_ENDPOINTS *e)
{
    ULONG got = (ULONG)((e->Bytes - FIELD_OFFSET(APOLLO_ENDPOINTS, Endpoint)) / sizeof(APOLLO_ENDPOINT_INFO));
    ULONG n = e->Head.Count < got ? e->Head.Count : got;

    return n < APOLLO_ENDPOINTS_ROWS_MAX ? n : APOLLO_ENDPOINTS_ROWS_MAX;
}

static BOOL GetEndpoints(DIAG_ENDPOINTS *e)
{
    DWORD ret = 0;
    APOLLO_ENDPOINTS_QUERY q = { 0 };

    ZeroMemory(e, sizeof(*e));
    if (!Ioctl(IOCTL_APOLLO_GET_ENDPOINTS, "GET_ENDPOINTS", &q, (DWORD)sizeof(q), &e->Head,
               (DWORD)(sizeof(APOLLO_ENDPOINTS) + sizeof(e->More)), &ret))
        return FALSE;
    if (ret < sizeof(APOLLO_ENDPOINTS)) {
        fprintf(stderr, "GET_ENDPOINTS returned %lu bytes, expected at least %lu: driver and tool are out "
                        "of step.\n", ret, (unsigned long)sizeof(APOLLO_ENDPOINTS));
        return FALSE;
    }
    e->Bytes = ret;
    return TRUE;
}

static const char *EpModel(ULONG m)
{
    return m == APOLLO_INFO_MODEL_X8P ? "x8p" : (m == APOLLO_INFO_MODEL_TWINX ? "twx" : "-");
}

static void EpSlot(char *Out, size_t Len, ULONG s)
{
    if (s == APOLLO_EP_SLOT_NONE)
        strcpy_s(Out, Len, "--");
    else
        sprintf_s(Out, Len, "%lu", s);
}

static void PrintEndpoints(const DIAG_ENDPOINTS *e)
{
    ULONG i, running = 0, rows = EpRows(e);

    /* the "EnabledEndpoints 0x.." text stays: the step scripts parse this line */
    printf("Endpoints    %lu in the driver's table, EnabledEndpoints 0x%lX, %lu stream(s) prepared%s\n",
           e->Head.Count, e->Head.EnabledMask, e->Head.Prepared,
           (rows > 0 && (EpRow(e, 0)->Flags & APOLLO_EP_FLAG_COMBINED)) ? " (combined set)" : "");
    printf("   # dir unit circuit         name                slots  set state                     packets  frames"
           "        underruns overruns dropped late notify-fail streams\n");
    for (i = 0; i < rows; i++) {
        const APOLLO_ENDPOINT_INFO *p = EpRow(e, i);
        char state[64], s0[12], s1[12];

        if (p->Flags & APOLLO_EP_FLAG_RUNNING)
            running++;
        if (!(p->Flags & APOLLO_EP_FLAG_ENABLED))
            strcpy_s(state, sizeof(state), "disabled");
        else if (!(p->Flags & APOLLO_EP_FLAG_ADDED))
            strcpy_s(state, sizeof(state), "enabled, not added");
        else if (p->Flags & APOLLO_EP_FLAG_RUNNING)
            sprintf_s(state, sizeof(state), "RUNNING %lux%lu B%s", p->PacketCount, p->PacketBytes,
                      (p->Flags & APOLLO_EP_FLAG_NO_SLOT) ? " NO SLOT" : "");
        else if (p->Flags & APOLLO_EP_FLAG_PREPARED)
            sprintf_s(state, sizeof(state), "paused %lux%lu B", p->PacketCount, p->PacketBytes);
        else if (p->Flags & APOLLO_EP_FLAG_STREAM)
            strcpy_s(state, sizeof(state), "stream open");
        else
            strcpy_s(state, sizeof(state), "idle");
        EpSlot(s0, sizeof(s0), p->Slot[0]);
        EpSlot(s1, sizeof(s1), p->Channels > 1 ? p->Slot[1] : p->Slot[0]);
        printf("  %2lu %-3s %-4s %-15.23s %-19.23s %2s-%-2s  %-3s %-24s %8llu %13llu %9lu %8lu %7lu %4lu %11lu %7lu\n",
               i, p->Render ? "out" : "in", EpModel(p->Model), p->Circuit, p->Name, s0, s1,
               (p->Flags & APOLLO_EP_FLAG_DEFAULT) ? "def" : "opt", state, p->Packets, p->Frames, p->Underruns,
               p->Overruns, p->Dropped, p->LatePackets, p->NotifyFailures, p->Streams);
    }
    /* install.ps1 / revert.ps1 parse this line. */
    printf("ACX streams: prepared %lu, running %lu\n", e->Head.Prepared, running);
}

static int CmdEndpoints(int n, char **a)
{
    static DIAG_ENDPOINTS e;
    double seconds = 0.0;
    int i;

    for (i = 1; i < n; i++) {
        if (!ParseDouble(a[i], &seconds) || seconds < 0.0) {
            fprintf(stderr, "usage: endpoints [seconds]\n");
            return 1;
        }
    }
    if (!GetEndpoints(&e))
        return 1;
    if (e.Head.Version != APOLLO_ENDPOINTS_VERSION && e.Head.Version != 1)
        fprintf(stderr, "warning: endpoints version %lu, this tool expects %d (or 1)\n", e.Head.Version,
                APOLLO_ENDPOINTS_VERSION);
    PrintEndpoints(&e);
    while (seconds > 0.0 && !g_stop) {
        double step = seconds > 2.0 ? 2.0 : seconds;

        SleepMs((DWORD)(step * 1000.0));
        seconds -= step;
        if (!GetEndpoints(&e))
            return 1;
        printf("\n");
        PrintEndpoints(&e);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* [Clock] clock, digital, panel, talkback (include\apollo_clock.h)    */
/* ------------------------------------------------------------------ */

static const char *ClockErrText(ULONG e)
{
    switch (e) {
    case APOLLO_CLOCK_ERR_NONE:         return "done (0)";
    case APOLLO_CLOCK_ERR_INVALID:      return "invalid (1)";
    case APOLLO_CLOCK_ERR_NOT_HERE:     return "not on this model (2)";
    case APOLLO_CLOCK_ERR_NOT_OFFERED:  return "not offered by the unit (3)";
    case APOLLO_CLOCK_ERR_UNPROVEN:     return "not proven on these units: add --try (4)";
    case APOLLO_CLOCK_ERR_RATE:         return "not at this rate (5)";
    case APOLLO_CLOCK_ERR_PORT:         return "optical port in the other format, or the clock runs from it (6)";
    case APOLLO_CLOCK_ERR_SRC:          return "SR Convert and the S/PDIF clock exclude each other (7)";
    case APOLLO_CLOCK_ERR_NOT_READY:    return "unit not running (8)";
    case APOLLO_CLOCK_ERR_BUSY:         return "busy (9)";
    case APOLLO_CLOCK_ERR_LOCK:         return "did not lock: back on Internal (10)";
    case APOLLO_CLOCK_ERR_HARDWARE:     return "HARDWARE (11)";
    default:                            return "?";
    }
}

/* " Internal S/PDIF ..." for a source mask (bit n = source n). */
static void ClockSourceList(char *Out, size_t Len, ULONG Mask)
{
    static const ULONG order[5] = { APOLLO_CK_INTERNAL, APOLLO_CK_SPDIF, APOLLO_CK_ADAT, APOLLO_CK_WORDCLOCK,
                                    APOLLO_CK_PEER };
    ULONG i;

    Out[0] = 0;
    for (i = 0; i < 5; i++) {
        if (Mask & APOLLO_CK_BIT(order[i])) {
            strcat_s(Out, Len, " ");
            strcat_s(Out, Len, ApolloClockName(order[i]));
        }
    }
    if (Out[0] == 0) {
        strcpy_s(Out, Len, " none");
    }
}

static void RateList(char *Out, size_t Len, ULONG Mask)
{
    static const char *const r[7] = { "", "44.1", "48", "88.2", "96", "176.4", "192" };
    ULONG i;

    Out[0] = 0;
    for (i = 1; i < 7; i++) {
        if (Mask & (1ul << i)) {
            strcat_s(Out, Len, " ");
            strcat_s(Out, Len, r[i]);
        }
    }
    strcat_s(Out, Len, Out[0] ? " kHz" : " none");
}

static void PrintClock(const APOLLO_CLOCK_INFO *c)
{
    char a[96], b[96], d[96], e[96];
    BOOL known = (c->Flags & APOLLO_CLOCK_F_DIG_KNOWN) != 0;

    printf("Clock source     %s%s%s%s\n", ApolloClockName(c->Source),
           (c->Flags & APOLLO_CLOCK_F_LOCKED) ? ", locked" : ", NOT locked",
           c->Wanted != c->Source ? " (wanted: " : "", c->Wanted != c->Source ? ApolloClockName(c->Wanted) : "");
    if (c->Wanted != c->Source) {
        printf("                 %s)\n", (c->Flags & APOLLO_CLOCK_F_FELL_BACK) ? "it lost lock or did not come back: on Internal" :
               (c->Flags & APOLLO_CLOCK_F_REAPPLY) ? "goes back on after the transport restart" : "pending");
    }
    ClockSourceList(a, sizeof(a), c->Sources);
    ClockSourceList(b, sizeof(b), c->Offered);
    ClockSourceList(d, sizeof(d), c->Usable);
    ClockSourceList(e, sizeof(e), c->ProvenSources);
    printf("Sources          jacks:%s\n                 the unit offers (0xC08C [31:16] 0x%04lX):%s\n"
           "                 usable now (with --try):%s\n                 proven on these units:%s\n", a, c->Offered, b, d, e);
    RateList(a, sizeof(a), c->RatesOnSource);
    printf("Rate             %lu Hz; on the %s clock:%s\n", c->Rate, ApolloClockName(c->Wanted), a);
    printf("Registers        0xC074 0x%08lX (index %lu, source 0x%02lX %s)  0xC07C 0x%08lX (index %lu, lock %lu)\n"
           "                 0xC084 0x%08lX (%lu; meaning not established: G6 read 48000)\n",
           c->ClockCfg, (c->ClockCfg >> 8) & 0xFF, c->ClockCfg & 0xFF, ApolloClockName(c->ClockCfg & 0xFF), c->RateInfo,
           (c->RateInfo >> 8) & 0xFF, c->RateInfo & 3, c->ClockInfo, c->ClockInfo);
    printf("Readback lock    word 6: %s\n", !(c->Flags & APOLLO_CLOCK_F_RB_LOCK_VALID) ? "not reported (bit 30 clear)" :
           (c->Flags & APOLLO_CLOCK_F_RB_LOCKED) ? "locked (bits 31 and 30)" : "NOT locked (bit 30 set, bit 31 clear)");
    if (c->MeasuredMilliHz != 0)
        printf("Measured         %lu.%03lu Hz (the unit's position against the PC clock over the last second)\n",
               c->MeasuredMilliHz / 1000, c->MeasuredMilliHz % 1000);
    else
        printf("Measured         - (transport or engine not running yet)\n");
    printf("Last change      %s in %lu ms; changes %lu, fallbacks %lu, lock losses %lu%s%s\n", ClockErrText(c->Result),
           c->LastMs, c->Changes, c->Fallbacks, c->LockLosses, (c->Flags & APOLLO_CLOCK_F_HOLD) ? ", hold" : "",
           (c->Flags & APOLLO_CLOCK_F_CHANGING) ? ", CHANGING" : "");
    printf("Why              %.*s\n", (int)sizeof(c->Why), c->Why);
    if (c->Flags & APOLLO_CLOCK_F_COMBINED)
        printf("Combined         in the combined device; %s\n", (c->Flags & APOLLO_CLOCK_F_TIMELINE) ?
               "this unit's clock IS the timeline (the other unit is resampled to it)" :
               "this unit is resampled to the timeline unit (any clock it runs on, within +/-500 ppm)");
    printf("Digital          optical in: %s; optical out: %s; coax S/PDIF: %s; SR Convert: %s\n",
           !(c->Digital & APOLLO_CLOCK_DIG_OPT_IN) ? "none" : !known ? "S/PDIF (the connect batch; not set here)" :
           (c->Flags & APOLLO_CLOCK_F_IN_ADAT) ? "ADAT" : "S/PDIF",
           !(c->Digital & APOLLO_CLOCK_DIG_OPT_OUT) ? "none" : !known ? "S/PDIF (the connect batch; not set here)" :
           (c->Flags & APOLLO_CLOCK_F_OUT_ADAT) ? "ADAT" : "S/PDIF",
           (c->Digital & APOLLO_CLOCK_DIG_COAX) ? "yes" : "no",
           !(c->Digital & APOLLO_CLOCK_DIG_SRC) ? "none" : (c->Flags & APOLLO_CLOCK_F_SRC) ? "on" : "off (as far as set here)");
    printf("                 carrying audio now: ADAT in %lu, out %lu; S/PDIF in %lu, out %lu%s\n", c->AdatIn, c->AdatOut,
           c->SpdifIn, c->SpdifOut, known ? "" : " (no format set here: the layouts show every slot, as before)");
    printf("                 format writes proven on these units: %s\n",
           c->ProvenDigital ? "some" : "none (every one needs --try)");
}

static BOOL GetClock(APOLLO_CLOCK_INFO *c)
{
    DWORD ret = 0;

    ZeroMemory(c, sizeof(*c));
    if (!Ioctl(IOCTL_APOLLO_GET_CLOCK, "GET_CLOCK", NULL, 0, c, (DWORD)sizeof(*c), &ret))
        return FALSE;
    if (ret < sizeof(*c) || c->Version != APOLLO_CLOCK_VERSION) {
        fprintf(stderr, "GET_CLOCK returned %lu bytes (version %lu): driver and tool are out of step.\n", ret, c->Version);
        return FALSE;
    }
    return TRUE;
}

static int ClockSet(ULONG What, ULONG Value, ULONG Flags, const char *Text)
{
    APOLLO_CLOCK_SET q;
    APOLLO_CLOCK_INFO c;
    DWORD ret = 0;
    LONGLONG t0 = QpcNow();

    ZeroMemory(&q, sizeof(q));
    q.Version = APOLLO_CLOCK_VERSION;
    q.What = What;
    q.Value = Value;
    q.Flags = Flags;
    printf("Setting %s%s%s...\n", Text, (Flags & APOLLO_CLOCK_SET_TRY) ? " (--try: not proven on these units)" : "",
           (Flags & APOLLO_CLOCK_SET_HOLD) ? " (--hold)" : "");
    ZeroMemory(&c, sizeof(c));
    if (!Ioctl(IOCTL_APOLLO_SET_CLOCK, "SET_CLOCK", &q, (DWORD)sizeof(q), &c, (DWORD)sizeof(c), &ret))
        return 1;
    printf("SET_CLOCK answered in %.0f ms: %s\n\n", (double)(QpcNow() - t0) * 1000.0 / (double)QpcFreq(),
           ClockErrText(c.Result));
    PrintClock(&c);
    return c.Result == APOLLO_CLOCK_ERR_NONE ? 0 : 1;
}


/* ---------------------------------------------------------------- [Events] events [seconds] */

static const char *EventKindName(ULONG k)
{
    switch (k) {
    case APOLLO_EVENT_STAGE:       return "stage";
    case APOLLO_EVENT_LEAVING:     return "leaving";
    case APOLLO_EVENT_RATE:        return "rate";
    case APOLLO_EVENT_CLOCK:       return "clock";
    case APOLLO_EVENT_PANEL:       return "panel";
    case APOLLO_EVENT_DSP_FAULT:   return "DSP FAULT";
    case APOLLO_EVENT_ENGINE_LOST: return "engine lost";
    case APOLLO_EVENT_PARAMS:      return "params";
    default:                       return "?";
    }
}

/* Every event of this unit as it happens, for <seconds> (default 60; 0 = the ring now, no waiting). Its own overlapped
   handle, so the wait ends on time; the driver parks the request between events. */
static int CmdEvents(int n, char **a)
{
    double secs = (n >= 1) ? atof(a[0]) : 60.0;
    HANDLE h = CreateFileW(g_units[g_unitIndex].Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, NULL);
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    ULONGLONG end = GetTickCount64() + (ULONGLONG)(secs * 1000.0);
    ULONG after = 0, shown = 0;
    int rc = 0;

    if (h == INVALID_HANDLE_VALUE || ev == NULL) {
        fprintf(stderr, "could not open the unit for events: %lu\n", GetLastError());
        return 1;
    }
    printf("Events of this unit%s (oldest first; Ctrl+C stops):\n", secs > 0 ? "" : " (the ring now)");
    for (;;) {
        APOLLO_EVENTS_WAIT w;
        APOLLO_EVENTS o;
        OVERLAPPED ov;
        DWORD ret = 0, left;
        ULONG i;
        BOOL ok;

        ZeroMemory(&w, sizeof(w));
        ZeroMemory(&o, sizeof(o));
        ZeroMemory(&ov, sizeof(ov));
        w.Version = APOLLO_EVENTS_VERSION;
        w.AfterSeq = after;
        w.Flags = (secs > 0) ? 0 : APOLLO_EVENTS_WAIT_NOWAIT;
        ResetEvent(ev);
        ov.hEvent = ev;
        ok = DeviceIoControl(h, IOCTL_APOLLO_WAIT_EVENTS, &w, sizeof(w), &o, sizeof(o), &ret, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            ULONGLONG t = GetTickCount64();
            left = (secs > 0 && end > t) ? (DWORD)(end - t) : 0;
            if (WaitForSingleObject(ev, left) != WAIT_OBJECT_0) {
                CancelIoEx(h, &ov);
                (void)GetOverlappedResult(h, &ov, &ret, TRUE);
                break;                                  /* time is up */
            }
            ok = GetOverlappedResult(h, &ov, &ret, FALSE);
        }
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_INVALID_FUNCTION) fprintf(stderr, "this apollo.sys has no event channel (an older build)\n");
            else if (err != ERROR_OPERATION_ABORTED) fprintf(stderr, "WAIT_EVENTS failed: %lu\n", err);
            rc = (err == ERROR_OPERATION_ABORTED) ? 0 : 1;
            break;
        }
        if (o.Flags & APOLLO_EVENTS_F_RESET) printf("  (the driver restarted: sequences begin again)\n");
        if (o.Lost) printf("  (%lu older event(s) no longer in the ring)\n", o.Lost);
        for (i = 0; i < o.Count && i < APOLLO_EVENTS_MAX; i++) {
            const APOLLO_EVENT_ENTRY *e = &o.Entry[i];
            FILETIME ft, lt;
            SYSTEMTIME st;

            ft.dwLowDateTime = (DWORD)e->Time;
            ft.dwHighDateTime = (DWORD)(e->Time >> 32);
            FileTimeToLocalFileTime(&ft, &lt);
            FileTimeToSystemTime(&lt, &st);
            printf("  #%-5lu %02u:%02u:%02u.%03u  %-12s A=%lu (0x%lX)  B=%lu (0x%08lX)\n", e->Seq, st.wHour, st.wMinute,
                   st.wSecond, st.wMilliseconds, EventKindName(e->Kind), e->A, e->A, e->B, e->B);
            shown++;
        }
        after = o.Seq;
        if (secs <= 0 || GetTickCount64() >= end) break;
    }
    printf("%lu event(s) shown; the unit's newest is #%lu.\n", shown, after);
    CloseHandle(ev);
    CloseHandle(h);
    return rc;
}

/* ---------------------------------------------------------------- [Params] params, param */

static void PrintParams(const APOLLO_PARAMS_INFO *I)
{
    static const char src[] = "DUMGX";
    ULONG i, pending = 0;

    printf("Driver Parameters of this unit (Units\\%s%s)%s\n", I->UnitKey[0] ? I->UnitKey : "-",
           I->UnitKey[0] ? "" : ": no unit key", I->Resolved ? "" : "  [not identified: the safe defaults]");
    printf("  %-22s %12s %3s %12s %12s\n", "name", "in force", "src", "default", "Units\\key");
    for (i = 0; i < I->Count && i < APOLLO_PARAMS_MAX; i++) {
        const APOLLO_PARAM_ENTRY *e = &I->Entry[i];
        char v[16], d[16], st[24];
        const char *fmt = (e->Flags & APOLLO_PARAM_F_HEX) ? "0x%lX" : "%lu";

        sprintf_s(v, sizeof(v), fmt, e->Value);
        sprintf_s(d, sizeof(d), fmt, e->Default);
        if (e->Flags & APOLLO_PARAM_F_STORED) sprintf_s(st, sizeof(st), fmt, e->Stored);
        else strcpy_s(st, sizeof(st), "-");
        printf("  %-22s %12s  %c  %12s %12s%s\n", e->Name, v, e->Source < 5 ? src[e->Source] : '?', d, st,
               (e->Flags & APOLLO_PARAM_F_PENDING) ? "  <- next start" : "");
        pending += (e->Flags & APOLLO_PARAM_F_PENDING) ? 1 : 0;
    }
    printf("  src: D built-in default, U Units\\key, M Models\\TT, G legacy global (x8p), X forced by Mode\n");
    if (pending)
        printf("  %lu value(s) apply at the unit's next start (disable/enable, driver update, reboot or power cycle).\n", pending);
}

static int CmdParams(void)
{
    APOLLO_PARAMS_INFO I;
    DWORD ret = 0;

    ZeroMemory(&I, sizeof(I));
    if (!Ioctl(IOCTL_APOLLO_GET_PARAMS, "GET_PARAMS", NULL, 0, &I, (DWORD)sizeof(I), &ret))
        return 1;
    if (ret < sizeof(I) || I.Version != APOLLO_PARAMS_VERSION) {
        fprintf(stderr, "GET_PARAMS returned %lu bytes (version %lu): driver and tool are out of step.\n", ret, I.Version);
        return 1;
    }
    PrintParams(&I);
    return 0;
}

/* param <name> <value> | param <name> --remove   (elevated) */
static int CmdParam(int n, char **a)
{
    APOLLO_PARAM_SET q;
    APOLLO_PARAMS_INFO I;
    DWORD ret = 0;
    char *end = NULL;

    if (n < 2) {
        fprintf(stderr, "usage: param <name> <value>|--remove   (names: 'params'; applies at the unit's next start)\n");
        return 1;
    }
    ZeroMemory(&q, sizeof(q));
    q.Version = APOLLO_PARAMS_VERSION;
    strncpy_s(q.Name, sizeof(q.Name), a[0], _TRUNCATE);
    if (_stricmp(a[1], "--remove") == 0) {
        q.Flags = APOLLO_PARAM_SET_DELETE;
    } else {
        q.Value = strtoul(a[1], &end, 0);
        if (end == a[1] || *end != '\0') {
            fprintf(stderr, "'%s' is not a number (decimal, or 0x... hex)\n", a[1]);
            return 1;
        }
    }
    ZeroMemory(&I, sizeof(I));
    if (!Ioctl(IOCTL_APOLLO_SET_PARAM, "SET_PARAM", &q, (DWORD)sizeof(q), &I, (DWORD)sizeof(I), &ret))
        return 1;
    if (I.Result != 0) {
        fprintf(stderr, "the registry write failed: 0x%08lX\n", I.Result);
    }
    PrintParams(&I);
    return I.Result == 0 ? 0 : 1;
}

static int CmdClock(int n, char **a)
{
    static const struct { const char *Name; ULONG Source; } src[] = {
        { "internal", APOLLO_CLOCK_SRC_INTERNAL }, { "spdif", APOLLO_CLOCK_SRC_SPDIF }, { "adat", APOLLO_CLOCK_SRC_ADAT },
        { "wordclock", APOLLO_CLOCK_SRC_WORDCLOCK }, { "wc", APOLLO_CLOCK_SRC_WORDCLOCK },
    };
    APOLLO_CLOCK_INFO c;
    ULONG i, flags;

    if (n >= 2 && a[1][0] != '-') {
        flags = (HasFlag(n, a, 2, "--try") ? APOLLO_CLOCK_SET_TRY : 0) | (HasFlag(n, a, 2, "--hold") ? APOLLO_CLOCK_SET_HOLD : 0);
        for (i = 0; i < sizeof(src) / sizeof(src[0]); i++) {
            if (_stricmp(a[1], src[i].Name) == 0)
                return ClockSet(APOLLO_CLOCK_WHAT_SOURCE, src[i].Source, flags, ApolloClockName(src[i].Source));
        }
        fprintf(stderr, "usage: clock [internal|spdif|adat|wordclock [--try] [--hold]]\n");
        return 1;
    }
    if (!GetClock(&c))
        return 1;
    PrintClock(&c);
    return 0;
}

static int CmdDigital(int n, char **a)
{
    APOLLO_CLOCK_INFO c;
    ULONG flags, v;

    if (n >= 3) {
        flags = HasFlag(n, a, 3, "--try") ? APOLLO_CLOCK_SET_TRY : 0;
        if (_stricmp(a[1], "optin") == 0 || _stricmp(a[1], "optout") == 0) {
            BOOL in = _stricmp(a[1], "optin") == 0;

            if (_stricmp(a[2], "adat") != 0 && _stricmp(a[2], "spdif") != 0) {
                fprintf(stderr, "%s takes adat or spdif\n", a[1]);
                return 1;
            }
            v = _stricmp(a[2], "adat") == 0 ? 1 : 0;
            return ClockSet(in ? APOLLO_CLOCK_WHAT_OPT_IN : APOLLO_CLOCK_WHAT_OPT_OUT, v, flags,
                            in ? (v ? "the optical input to ADAT" : "the optical input to S/PDIF") :
                                 (v ? "the optical output to ADAT" : "the optical output to S/PDIF"));
        }
        if (_stricmp(a[1], "src") == 0 && ParseU32(a[2], &v) && v <= 1)
            return ClockSet(APOLLO_CLOCK_WHAT_SRC, v, flags, v ? "SR Convert on" : "SR Convert off");
        fprintf(stderr, "usage: digital [optin adat|spdif | optout adat|spdif | src 0|1] [--try]\n");
        return 1;
    }
    if (n == 2) {
        fprintf(stderr, "usage: digital [optin adat|spdif | optout adat|spdif | src 0|1] [--try]\n");
        return 1;
    }
    if (!GetClock(&c))
        return 1;
    PrintClock(&c);
    return 0;
}

static void PanelValueText(char *Out, size_t Len, ULONG Field, LONG Value)
{
    static const char *const knob[3] = { "PREAMP", "MONITOR", "HP" };
    static const char *const alt[4] = { "MAIN", "ALT 1", "ALT 2", "?" };

    switch (Field) {
    case APOLLO_PANEL_F_LEVEL:
    case APOLLO_PANEL_F_HP:
        sprintf_s(Out, Len, "%ld (%.1f dB)", Value, ((double)Value - 192.0) / 2.0);
        break;
    case APOLLO_PANEL_F_KNOB:
        sprintf_s(Out, Len, "%s", (Value >= 0 && Value < 3) ? knob[Value] : "?");
        break;
    case APOLLO_PANEL_F_ALT:
        sprintf_s(Out, Len, "%s", alt[Value & 3]);
        break;
    case APOLLO_PANEL_F_PREAMP:
        sprintf_s(Out, Len, "preamp %ld", Value + 1);
        break;
    case APOLLO_PANEL_F_LOCK:
        sprintf_s(Out, Len, "%s", Value < 0 ? "not reported" : Value ? "locked" : "NOT locked");
        break;
    default:
        sprintf_s(Out, Len, "%s", Value ? "on" : "off");
        break;
    }
}

static const char *PanelOrigin(ULONG o)
{
    static const char *const n[4] = { "panel (the unit's own knob/button)", "host (a setting from Windows)",
                                      "link (mirrored from another unit, or the talkback dim)",
                                      "unit (its own reaction: dim on talkback)" };

    return o < 4 ? n[o] : "?";
}

static void PrintPanelEvent(const APOLLO_PANEL_EVENT *e)
{
    char v[48];

    PanelValueText(v, sizeof(v), e->Field, e->Value);
    printf("  #%-5lu +%8.3f s  %-9s %-18s %s\n", e->Seq, e->TimeMs / 1000.0, ApolloPanelFieldName(e->Field), v,
           PanelOrigin(e->Origin));
}

static void PrintPanel(const APOLLO_PANEL_INFO *p, BOOL Events)
{
    char v[48], h[48];
    ULONG i;

    if (!(p->Flags & APOLLO_PANEL_INFO_VALID)) {
        printf("Front panel      no readback decoded since the connect (not running, or the DSP posts none)\n");
    } else {
        PanelValueText(v, sizeof(v), APOLLO_PANEL_F_LEVEL, (LONG)p->Level);
        PanelValueText(h, sizeof(h), APOLLO_PANEL_F_HP, (LONG)p->HpLevel);
        printf("Front panel      knob %s%s (the MONITOR/PREAMP button: read-only)\n",
               p->Knob == 0 ? "PREAMP" : p->Knob == 1 ? "MONITOR" : p->Knob == 2 ? "HP" : "?", "");
        if (p->Knob == 0)
            printf("                 the knob edits preamp %lu (word 3 [30:28])\n", p->Preamp + 1);
        printf("                 monitor level %s, HP %s\n", v, h);
        PanelValueText(v, sizeof(v), APOLLO_PANEL_F_LOCK, p->Lock);
        printf("                 mute %lu, dim %lu, mono %lu, speakers %s, talkback %s, clock (readback) %s\n", p->Mute,
               p->Dim, p->Mono, p->Alt == 0 ? "MAIN" : p->Alt == 1 ? "ALT 1" : "ALT 2", p->Talk ? "ON" : "off", v);
    }
    if (p->LinkFlags == 0) {
        printf("Monitor link     off (apollo-diag panel link x8p|twinx: that unit's level follows the other unit's knob)\n");
    } else {
        printf("Monitor link     on:%s%s%s%s%s; monitor unit HW_UID 0x%016llX; this unit is %s\n",
               (p->LinkFlags & APOLLO_PANEL_LINK_LEVEL) ? " level" : "", (p->LinkFlags & APOLLO_PANEL_LINK_MUTE) ? " mute" : "",
               (p->LinkFlags & APOLLO_PANEL_LINK_DIM) ? " dim" : "", (p->LinkFlags & APOLLO_PANEL_LINK_MONO) ? " mono" : "",
               (p->LinkFlags & APOLLO_PANEL_LINK_TALK_DIM) ? " talkdim" : "", p->LinkMonitorUid,
               (p->Flags & APOLLO_PANEL_INFO_MONITOR_UNIT) ? "the MONITOR unit (the speakers)" :
               "a KNOB unit (its knob and buttons drive the monitor unit)");
        printf("                 applied %lu, monitor unit busy %lu (retried), dropped %lu (monitor unit not running)%s\n",
               p->LinkApplied, p->LinkBusy, p->LinkDropped, (p->Flags & APOLLO_PANEL_INFO_LINK_PENDING) ? ", PENDING" : "");
    }
    if (Events) {
        printf("Changes          %lu since the driver loaded, last %lu (oldest first):\n", p->Seq, p->EventCount);
        for (i = 0; i < p->EventCount && i < APOLLO_PANEL_EVENTS; i++)
            PrintPanelEvent(&p->Event[i]);
    }
}

static BOOL GetPanel(APOLLO_PANEL_INFO *p)
{
    DWORD ret = 0;

    ZeroMemory(p, sizeof(*p));
    if (!Ioctl(IOCTL_APOLLO_GET_PANEL, "GET_PANEL", NULL, 0, p, (DWORD)sizeof(*p), &ret))
        return FALSE;
    if (ret < sizeof(*p) || p->Version != APOLLO_CLOCK_VERSION) {
        fprintf(stderr, "GET_PANEL returned %lu bytes (version %lu): driver and tool are out of step.\n", ret, p->Version);
        return FALSE;
    }
    return TRUE;
}

static int CmdPanel(int n, char **a)
{
    static APOLLO_PANEL_INFO p;
    ULONG secs = 0, last, i;
    LONGLONG t0, freq;

    if (n >= 3 && _stricmp(a[1], "link") == 0) {
        APOLLO_PANEL_LINK l;
        DWORD ret = 0;
        int want = 0, idx;

        ZeroMemory(&l, sizeof(l));
        l.Version = APOLLO_CLOCK_VERSION;
        if (_stricmp(a[2], "off") != 0) {
            if (!ParseUnit(a[2], &want) || (idx = ApolloPickUnit(g_units, g_unitCount, want)) < 0 ||
                g_units[idx].Error != 0 || !ApolloInfoHasUnit(&g_units[idx].Info, g_units[idx].InfoBytes)) {
                fprintf(stderr, "panel link: '%s' is not a unit on apollo.sys (x8p, twinx or an index)\n", a[2]);
                return 1;
            }
            l.MonitorUid = ((ULONGLONG)g_units[idx].Info.HwUidHi << 32) | g_units[idx].Info.HwUidLo;
            l.Flags = APOLLO_PANEL_LINK_LEVEL | APOLLO_PANEL_LINK_MUTE | APOLLO_PANEL_LINK_DIM | APOLLO_PANEL_LINK_TALK_DIM;
            if (n >= 4) {
                static const struct { const char *Name; ULONG Flag; } fl[] = {
                    { "level", APOLLO_PANEL_LINK_LEVEL }, { "mute", APOLLO_PANEL_LINK_MUTE },
                    { "dim", APOLLO_PANEL_LINK_DIM }, { "mono", APOLLO_PANEL_LINK_MONO },
                    { "talkdim", APOLLO_PANEL_LINK_TALK_DIM }, { "all", 0x1F },
                };
                char buf[96], *ctx = NULL, *tok;
                ULONG k;

                l.Flags = 0;
                strncpy_s(buf, sizeof(buf), a[3], _TRUNCATE);
                for (tok = strtok_s(buf, ",", &ctx); tok != NULL; tok = strtok_s(NULL, ",", &ctx)) {
                    for (k = 0; k < sizeof(fl) / sizeof(fl[0]) && _stricmp(tok, fl[k].Name) != 0; k++) {
                    }
                    if (k == sizeof(fl) / sizeof(fl[0])) {
                        fprintf(stderr, "panel link: '%s' is not a flag (level,mute,dim,mono,talkdim or all)\n", tok);
                        return 1;
                    }
                    l.Flags |= fl[k].Flag;
                }
                if (l.Flags == 0) {
                    fprintf(stderr, "panel link: flags are level,mute,dim,mono,talkdim or all\n");
                    return 1;
                }
            }
        }
        if (!Ioctl(IOCTL_APOLLO_SET_PANEL, "SET_PANEL", &l, (DWORD)sizeof(l), &p, (DWORD)sizeof(p), &ret))
            return 1;
        PrintPanel(&p, FALSE);
        return 0;
    }
    if (n >= 2 && !ParseU32(a[1], &secs)) {
        fprintf(stderr, "usage: panel [seconds] | panel link x8p|twinx|off [level,mute,dim,mono,talkdim|all]\n");
        return 1;
    }
    if (!GetPanel(&p))
        return 1;
    PrintPanel(&p, secs == 0);
    if (secs == 0)
        return 0;
    printf("Watching for %lu s: turn the knob, press MONITOR/PREAMP, mute, dim, ALT, TALK (Ctrl+C stops)...\n", secs);
    last = p.Seq;
    freq = QpcFreq();
    t0 = QpcNow();
    while (!g_stop && (QpcNow() - t0) < (LONGLONG)secs * freq) {
        SleepMs(20);
        if (!GetPanel(&p))
            return 1;
        for (i = 0; i < p.EventCount && i < APOLLO_PANEL_EVENTS; i++) {
            if (p.Event[i].Seq > last)
                PrintPanelEvent(&p.Event[i]);
        }
        if (p.Seq - last > APOLLO_PANEL_EVENTS)
            printf("  (%lu changes came faster than this watch reads: the oldest are not shown)\n", p.Seq - last - APOLLO_PANEL_EVENTS);
        last = p.Seq;
    }
    PrintPanel(&p, FALSE);
    return 0;
}

static int CmdTalkback(void)
{
    APOLLO_PANEL_INFO p;

    if (!GetPanel(&p))
        return 1;
    printf("Talkback         %s (readback word 6 bit 8; set it with: mon talkback 0|1)\n",
           !(p.Flags & APOLLO_PANEL_INFO_VALID) ? "unknown (no readback yet)" : p.Talk ? "ON" : "off");
    if (p.TalkSlot[0] != APOLLO_PANEL_NO_SLOT)
        printf("Record channel   AX_REC_CH %lu: TALKBACK 1/2 at record slots %lu/%lu (capture --channels %lu,%lu)\n",
               p.TalkRecChannels, p.TalkSlot[0], p.TalkSlot[1], p.TalkSlot[0], p.TalkSlot[1]);
    else
        printf("Record channel   AX_REC_CH %lu: TALKBACK 1/2 (0x0B01/0x0B02) are not in this unit's frame\n", p.TalkRecChannels);
    printf("Mic              %s\n", p.TalkStrip ? "talkback input strip running" :
           "no talkback input strip (module 0xC9, no recipe known): the mic carries silence into the cues and the record slots");
    printf("Into the cues    the cue sends of bus 10 (cue <n> send tb <dB>); the unit gates them with TALK (UA's engine writes "
           "only 0x46: unverified until the mic is live)\n");
    printf("Dim on talkback  %s%s\n", (p.LinkFlags & APOLLO_PANEL_LINK_TALK_DIM) ? "the monitor link dims the monitor unit" :
           "off in the driver (panel link <unit> talkdim; Apollo Control's own setting dims from the host)",
           (p.Flags & APOLLO_PANEL_INFO_UNIT_DIMS) ? "; seen: this unit dims itself on talkback" : "");
    if (p.Flags & APOLLO_PANEL_INFO_TALK_DIMMED)
        printf("                 the link dimmed this unit for talkback: it undims it when talkback goes off\n");
    return 0;
}

static void Usage(void)
{
    printf(
"apollo-diag: diagnostics for the apollo.sys Apollo driver\n"
"(info, gain/pad/48v/line/lowcut/phase/link, mon, cue, levels, engine, endpoints, readback, log\n"
"work without elevation; everything else needs an elevated prompt)\n"
"\n"
"usage: apollo-diag [--unit x8p|twinx|N] <command> [args]\n"
"\n"
"  --unit x8p|twinx|N            which Apollo when apollo.sys runs more than one (N = the\n"
"                                interface index, as --index N). Default: the x8p, else the\n"
"                                first. A unit in OBSERVE mode is read-only.\n"
"  units                         every Apollo on apollo.sys: model, serial, Mode, log tag\n"
"  info                          driver state, identity and counters\n"
"  read <off>                    read a BAR0 register (0x... hex or decimal)\n"
"  write <off> <val> --yes       write a BAR0 register (raw: can wedge the unit)\n"
"  dump                          identity, transport, mixer and DSP ring registers\n"
"  start                         start the audio transport\n"
"  stop                          stop the audio transport\n"
"  pos [seconds=3]               watch the hardware position: rate, stalls, jumps\n"
"  capture <sec> <out.wav> [--channels a,b,c]\n"
"                                record capture-ring slots (0-based) to a WAV and\n"
"                                print each slot's peak and RMS in dBFS\n"
"  tone <sec> [--slots 0,1|mon] [--freq 1000] [--level -20]\n"
"                                play a sine on playback slots (0,1 = MON L/R); mon = the\n"
"                                MON L/R slots of the unit's IO descriptor (the Twin X's\n"
"                                default; on a Mode 1 Twin X at most 10 s)\n"
"  gain <preamp> <dB 10..65>     preamp gain (preamps are numbered from 1)\n"
"  pad <preamp> 0|1              pad off/on\n"
"  48v <preamp> 0|1              phantom power off/on\n"
"  line <preamp> 0|1             0 = Mic, 1 = Line\n"
"  lowcut <preamp> 0|1           low-cut filter off/on\n"
"  phase <preamp> 0|1            polarity normal/inverted\n"
"  link <1|3|5|7> 0|1            stereo-link a preamp pair (odd preamp of the pair; Twin X: 1)\n"
"  hiz 1 <0..7|2.2M..21k>        Twin X: impedance of the front Hi-Z jack (input 1):\n"
"                                0-7 = 2.2M 1M 453k 232k 100k 70k 47k 21k\n"
"  mon <param> <value>           monitor: level/hp1 <dB>, mute/mono/dim/talkback 0|1,\n"
"                                dimlevel 1..7, source mix|cue1..cue4, hp1src/hp2src cue1|cue2,\n"
"                                alt main|alt1|alt2, alt1/alt2 0|1, alt1trim/alt2trim <dB>,\n"
"                                cue1mix..cue4mix own|mon, cue1mono..cue4mono 0|1 ('mon' alone: all)\n"
"  cue <n> send|pan|mute ...     a cue mix: send <input|tb> <dB|off> [pan], pan <input|tb> <p>,\n"
"                                mute 0|1 ('cue' alone: the forms)\n"
"  mix <input|tb|all> ...        the monitor mix (what the speakers play): level <dB|off>, pan <p>,\n"
"                                mute 0|1, solo 0|1; 'mix all mute 0|1' ('mix' alone: the forms)\n"
"  levels [seconds=3]            peak dBFS per capture slot (transport must run)\n"
"  engine [seconds] [--reset]    1 ms copy engine: tick timing, late ticks, ASIO client\n"
"                                (with seconds: rates over that window)\n"
"  endpoints [seconds]           Windows audio endpoints (ACX): state, packets, underruns\n"
"  rate                          the unit's sample rate: offered, allowed, stored, the combined device\n"
"  rate <Hz> [--combined] [--no-store]\n"
"                                change it: 44100 48000 88200 96000 176400 192000 (no elevation;\n"
"                                --combined: every unit of the combined device, master first)\n"
"                                (with seconds: refresh every 2 s for that long)\n"
"  clock                         [Clock] the clock source: lock, sources (jacks, offered, usable, proven),\n"
"                                0xC074/0xC07C/0xC084, the measured rate, the digital formats (no elevation)\n"
"  clock internal|spdif|adat|wordclock [--try] [--hold]\n"
"                                switch it (transport running; 2-4 s; no lock in 1 s -> back to Internal;\n"
"                                everything but internal is unproven: --try; --hold keeps it when unlocked)\n"
"  digital [optin adat|spdif | optout adat|spdif | src 0|1] [--try]\n"
"                                optical port formats (Twin X: in; x8p: in and out), SR Convert on the\n"
"                                S/PDIF input; all unproven: --try ( 'digital' alone: as clock)\n"
"  panel [seconds]               the front panel from the readback: knob mode (MONITOR/PREAMP), monitor\n"
"                                level, mute, dim, mono, ALT, talkback, and the last changes with who made\n"
"                                them; with seconds: print each change as it happens\n"
"  panel link x8p|twinx|off [level,mute,dim,mono,talkdim|all]\n"
"                                the monitor link: the named unit (the speakers) follows the other unit's\n"
"                                knob and buttons (default level,mute,dim,talkdim; not stored)\n"
"  talkback                      the talkback: button, record slots (AX_REC_CH), strip, dim\n"
"  setting <index> <val> <mask>  raw mixer mailbox setting (index 0..37)\n"
"  readback                      the unit's 40-word hardware readback, decoded\n"
"  events [seconds=60]           the unit's events as they happen (rate, clock, front panel, DSP faults;\n"
"                                0 = what the ring holds now)\n"
"  params                        the unit's driver Parameters: in force, source, default, stored\n"
"  param <name> <value>|--remove (elevated) store one in Units\\<key> for the unit's next start\n"
"  log [--all]                   the driver's own log (bring-up steps, errors); with --unit,\n"
"                                that unit's lines (tag [x8p], [twx]) and driver-wide ones\n"
"  strips                        input-strip state: ownership, pools, modules, replies\n"
"  strips verify [dir]           find the five pinned strip modules (UAD install, or the\n"
"                                DLLs in dir) and check the installed copies; no device\n"
"  strips install [dir]          elevated: copy them to System32\\drivers\\ApolloStrips\n"
"\n"
"On the x8p legacy layout, capture slot N is physical input N+1 and playback\n"
"slots 0-1 are MON L/R. The Twin X (Mode 1, gate G3) runs its firmware IO\n"
"descriptor layout: 'log' lists every slot (lines 'G5: rec' / 'G5: play'); MON L/R\n"
"play on slots 0-1 and loop back on record slots 26-27 (descriptor read at gate G2);\n"
"its inputs 1-2 (MIC/LINE/HIZ) record on slots 0-1 once its input strips run (G5).\n"
"An empty jack reads exact zero ('zero' in levels): plug something in first.\n");
}

int main(int argc, char **argv)
{
    char *args[DIAG_MAX_ARGS];
    int n = 0, i, rc = 1;
    int want = APOLLO_PICK_DEFAULT;
    const char *cmd;

    for (i = 1; i < argc; i++) {
        /* [Twin X G2] --unit x8p|twinx|<index>; --index <index> as before */
        if (strcmp(argv[i], "--index") == 0 || strcmp(argv[i], "--unit") == 0) {
            ULONG index = 0;
            BOOL ok = FALSE;

            if (i + 1 < argc) {
                if (strcmp(argv[i], "--index") == 0) {
                    ok = ParseU32(argv[i + 1], &index) && index < APOLLO_SELECT_MAX;
                    if (ok)
                        want = (int)index;
                } else {
                    ok = ParseUnit(argv[i + 1], &want);
                }
            }
            if (!ok) {
                fprintf(stderr, "%s needs x8p, twinx or an index 0..%d\n", argv[i], APOLLO_SELECT_MAX - 1);
                return 1;
            }
            g_unitNamed = TRUE;
            i++;
            continue;
        }
        if (n < DIAG_MAX_ARGS) {
            args[n] = argv[i];
            n++;
        }
    }
    if (n == 0) {
        Usage();
        return 1;
    }
    cmd = args[0];
    if (_stricmp(cmd, "--help") == 0 || _stricmp(cmd, "-h") == 0 || _stricmp(cmd, "help") == 0 ||
        _stricmp(cmd, "/?") == 0) {
        Usage();
        return 0;
    }

    /* Module extraction works without the driver (and before it is installed). */
    if (_stricmp(cmd, "strips") == 0 && n >= 2) {
        const char *dirArg = (n >= 3) ? args[2] : NULL;

        if (_stricmp(args[1], "verify") == 0)
            rc = CmdStripsVerify(dirArg);
        else if (_stricmp(args[1], "install") == 0)
            rc = CmdStripsInstall(dirArg);
        else {
            fprintf(stderr, "usage: strips | strips verify [dir] | strips install [dir]\n");
            rc = 1;
        }
        if (g_sha != NULL)
            BCryptCloseAlgorithmProvider(g_sha, 0);
        return rc;
    }

    /* [Twin X G2] every unit on the driver, without keeping any open */
    if (_stricmp(cmd, "units") == 0)
        return CmdUnits();

    g_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                     TIMER_ALL_ACCESS);
    if (g_timer == NULL)
        g_timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    if (!OpenDevice(want)) {
        if (g_timer != NULL)
            CloseHandle(g_timer);
        return 2;
    }

    if (_stricmp(cmd, "info") == 0)
        rc = CmdInfo();
    else if (_stricmp(cmd, "read") == 0)
        rc = CmdRead(n, args);
    else if (_stricmp(cmd, "write") == 0)
        rc = CmdWrite(n, args);
    else if (_stricmp(cmd, "dump") == 0)
        rc = CmdDump();
    else if (_stricmp(cmd, "start") == 0)
        rc = CmdTransport(TRUE);
    else if (_stricmp(cmd, "stop") == 0)
        rc = CmdTransport(FALSE);
    else if (_stricmp(cmd, "pos") == 0)
        rc = CmdPos(n, args);
    else if (_stricmp(cmd, "capture") == 0)
        rc = CmdCapture(n, args);
    else if (_stricmp(cmd, "tone") == 0)
        rc = CmdTone(n, args);
    else if (_stricmp(cmd, "gain") == 0)
        rc = CmdInputParam(n, args, APOLLO_PARAM_GAIN_DB, "gain dB");
    else if (_stricmp(cmd, "pad") == 0)
        rc = CmdInputParam(n, args, APOLLO_PARAM_PAD, "PAD");
    else if (_stricmp(cmd, "48v") == 0)
        rc = CmdInputParam(n, args, APOLLO_PARAM_48V, "48V");
    else if (_stricmp(cmd, "line") == 0)
        rc = CmdInputParam(n, args, APOLLO_PARAM_MIC_LINE, "Line");
    else if (_stricmp(cmd, "lowcut") == 0)
        rc = CmdInputParam(n, args, APOLLO_PARAM_LOWCUT, "LowCut");
    else if (_stricmp(cmd, "phase") == 0)
        rc = CmdInputParam(n, args, APOLLO_PARAM_PHASE, "Phase");
    else if (_stricmp(cmd, "link") == 0)
        rc = CmdInputParam(n, args, APOLLO_PARAM_LINK, "Link");
    else if (_stricmp(cmd, "hiz") == 0)
        rc = CmdHiz(n, args);
    else if (_stricmp(cmd, "mon") == 0)
        rc = CmdMon(n, args);
    else if (_stricmp(cmd, "cue") == 0)
        rc = CmdCue(n, args);
    else if (_stricmp(cmd, "mix") == 0)
        rc = CmdMix(n, args);
    else if (_stricmp(cmd, "levels") == 0)
        rc = CmdLevels(n, args);
    else if (_stricmp(cmd, "engine") == 0)
        rc = CmdEngine(n, args);
    else if (_stricmp(cmd, "endpoints") == 0)
        rc = CmdEndpoints(n, args);
    else if (_stricmp(cmd, "rate") == 0)
        rc = CmdRate(n, args);
    else if (_stricmp(cmd, "clock") == 0)
        rc = CmdClock(n, args);
    else if (_stricmp(cmd, "params") == 0)
        rc = CmdParams();
    else if (_stricmp(cmd, "events") == 0)
        rc = CmdEvents(n, args);
    else if (_stricmp(cmd, "param") == 0)
        rc = CmdParam(n, args);
    else if (_stricmp(cmd, "digital") == 0)
        rc = CmdDigital(n, args);
    else if (_stricmp(cmd, "panel") == 0)
        rc = CmdPanel(n, args);
    else if (_stricmp(cmd, "talkback") == 0)
        rc = CmdTalkback();
    else if (_stricmp(cmd, "setting") == 0)
        rc = CmdSetting(n, args);
    else if (_stricmp(cmd, "readback") == 0)
        rc = CmdReadback();
    else if (_stricmp(cmd, "log") == 0)
        rc = CmdLog(n, args);
    else if (_stricmp(cmd, "strips") == 0)
        rc = CmdStrips();
    else {
        fprintf(stderr, "unknown command '%s'\n\n", cmd);
        Usage();
        rc = 1;
    }

    CloseHandle(g_dev);
    if (g_timer != NULL)
        CloseHandle(g_timer);
    return rc;
}
