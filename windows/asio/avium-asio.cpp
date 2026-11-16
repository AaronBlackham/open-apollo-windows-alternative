/*
 * avium-asio.cpp - ASIO driver for the Apollo x8p on apollo.sys (docs/ASIO.md).
 *
 * A COM in-proc server implementing IASIO. All audio moves through a block
 * the kernel driver maps into this process: the driver's 1 ms engine fills
 * one half of every buffer while the host works on the other, publishes the
 * half in the shared header (seqlock) and sets our event; a Pro Audio thread
 * then calls bufferSwitch. Samples are ASIOSTInt32LSB, exactly as the
 * hardware ring holds them, so this side only hands out pointers.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 * Uses the Steinberg ASIO SDK under its GPLv3 option (third_party/asiosdk/LICENSE.txt).
 */
#include <windows.h>
#include <ole2.h>
#include <cfgmgr32.h>
#include <avrt.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

#include "asiosys.h"
#include "combase.h"
#include "iasiodrv.h"
#include "..\include\apollo_asio.h"
#include "..\include\apollo_agg.h"     /* [G6] the combined device */
#include "..\include\apollo_select.h"  /* [Twin X G2] the x8p among several units */
#include "..\include\apollo_rate.h"    /* [Rate] Can/Get/SetSampleRate: GET_RATE / SET_RATE */
#include <new>

#define DRIVER_NAME     "Apollo x8p (Avium)"
#define AGG_DRIVER_NAME "Apollo (Avium)"   /* [G6] the combined device: every unit, one timeline */
#define DLL_NAME        "avium-asio.dll"
#define DRIVER_VERSION  2       /* 2: per-half ClientDone handshake (apollo_asio.h version 2) */
#define AGG_DRIVER_VERSION 3    /* [G6] the combined client (apollo_agg.h, shared block version 3) */
#define SAMPLE_RATE     48000   /* [Rate] the default only: the driver says what the unit runs at (GET_RATE) */
#define PREF_FRAMES     128
#define EST_IN_LATENCY  26      /* ADC 10 + capture guard 16: only when the driver says nothing (no GET_RATE) */
#define EST_OUT_LATENCY 152     /* render lead 144 + FRAME_CFG 8 */
#define MAX_CH          APOLLO_AGG_MAX_CH   /* channel arrays sized for the combined device */

/* {edb055a9-283d-4679-97e2-7713928bd01b} */
CLSID CLSID_AviumApolloAsio = { 0xedb055a9, 0x283d, 0x4679, { 0x97, 0xe2, 0x77, 0x13, 0x92, 0x8b, 0xd0, 0x1b } };
/* [G6] {b6a1f3c2-5d4e-4a7b-9c21-3e8f0a6d7c55}: "Apollo (Avium)", the combined device */
CLSID CLSID_AviumApolloAggAsio = { 0xb6a1f3c2, 0x5d4e, 0x4a7b, { 0x9c, 0x21, 0x3e, 0x8f, 0x0a, 0x6d, 0x7c, 0x55 } };

/* Slot names in the x8p firmware descriptor order (confirmed on hardware Nov 14 2026). */
static const char *const kInNames[] = {
    "MIC/LINE 1", "MIC/LINE 2", "MIC/LINE 3", "MIC/LINE 4", "MIC/LINE 5", "MIC/LINE 6", "MIC/LINE 7", "MIC/LINE 8",
    "ADAT 1", "ADAT 2", "ADAT 3", "ADAT 4", "ADAT 5", "ADAT 6", "ADAT 7", "ADAT 8",
    "S/PDIF L", "S/PDIF R",
    "VIRTUAL 1", "VIRTUAL 2", "VIRTUAL 3", "VIRTUAL 4", "VIRTUAL 5", "VIRTUAL 6", "VIRTUAL 7", "VIRTUAL 8",
    "MON L", "MON R",
    "AUX 1 L", "AUX 1 R", "AUX 2 L", "AUX 2 R",
    "TALKBACK 1", "TALKBACK 2",
};
static const char *const kOutNames[] = {
    "MON L", "MON R",
    "LINE 1", "LINE 2", "LINE 3", "LINE 4", "LINE 5", "LINE 6", "LINE 7", "LINE 8",
    "ADAT 1", "ADAT 2", "ADAT 3", "ADAT 4", "ADAT 5", "ADAT 6", "ADAT 7", "ADAT 8",
    "S/PDIF L", "S/PDIF R",
    "VIRTUAL 1", "VIRTUAL 2", "VIRTUAL 3", "VIRTUAL 4", "VIRTUAL 5", "VIRTUAL 6", "VIRTUAL 7", "VIRTUAL 8",
    "CUE 1 L", "CUE 1 R", "CUE 2 L", "CUE 2 R", "CUE 3 L", "CUE 3 R", "CUE 4 L", "CUE 4 R",
};
#define N_IN_NAMES  ((long)(sizeof(kInNames) / sizeof(kInNames[0])))
#define N_OUT_NAMES ((long)(sizeof(kOutNames) / sizeof(kOutNames[0])))

static void SetTime64(unsigned long *hi, unsigned long *lo, unsigned long long v)
{
    *hi = (unsigned long)(v >> 32);
    *lo = (unsigned long)(v & 0xFFFFFFFFull);
}

class ApolloAsio : public IASIO, public CUnknown
{
public:
    ApolloAsio(LPUNKNOWN pUnk, HRESULT *phr, bool agg = false)
        : CUnknown(agg ? L"AviumApolloAggAsio" : L"AviumApolloAsio", pUnk, phr), agg_(agg) {}
    ~ApolloAsio();

    DECLARE_IUNKNOWN
    static CUnknown *CreateInstance(LPUNKNOWN pUnk, HRESULT *phr) { return new ApolloAsio(pUnk, phr); }
    static CUnknown *CreateAggInstance(LPUNKNOWN pUnk, HRESULT *phr) { return new ApolloAsio(pUnk, phr, true); }
    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void **ppv) override
    {
        if (riid == (agg_ ? CLSID_AviumApolloAggAsio : CLSID_AviumApolloAsio)) {
            return GetInterface(static_cast<IASIO *>(this), ppv);
        }
        return CUnknown::NonDelegatingQueryInterface(riid, ppv);
    }

    ASIOBool init(void *sysHandle) override;
    void getDriverName(char *name) override { strcpy_s(name, 32, agg_ ? AGG_DRIVER_NAME : DRIVER_NAME); }
    long getDriverVersion() override { return agg_ ? AGG_DRIVER_VERSION : DRIVER_VERSION; }
    void getErrorMessage(char *string) override { strcpy_s(string, 124, err_); }
    ASIOError start() override;
    ASIOError stop() override;
    ASIOError getChannels(long *numInputChannels, long *numOutputChannels) override;
    ASIOError getLatencies(long *inputLatency, long *outputLatency) override;
    ASIOError getBufferSize(long *minSize, long *maxSize, long *preferredSize, long *granularity) override;
    ASIOError canSampleRate(ASIOSampleRate sampleRate) override;
    ASIOError getSampleRate(ASIOSampleRate *sampleRate) override;
    ASIOError setSampleRate(ASIOSampleRate sampleRate) override;
    ASIOError getClockSources(ASIOClockSource *clocks, long *numSources) override;
    ASIOError setClockSource(long reference) override;
    ASIOError getSamplePosition(ASIOSamples *sPos, ASIOTimeStamp *tStamp) override;
    ASIOError getChannelInfo(ASIOChannelInfo *info) override;
    ASIOError createBuffers(ASIOBufferInfo *bufferInfos, long numChannels, long bufferSize,
                            ASIOCallbacks *callbacks) override;
    ASIOError disposeBuffers() override;
    ASIOError controlPanel() override;
    ASIOError future(long selector, void *opt) override;
    ASIOError outputReady() override;

private:
    struct Snapshot {
        LONG half, switches;
        LONGLONG position, qpc;
    };

    bool OpenDevice();
    bool ReadLayout();                  /* [G6] AGG_GET_LAYOUT into layout_ */
    ASIOError CreateAggBuffers(ASIOBufferInfo *bufferInfos, long numChannels, long bufferSize);
    bool CheckShared(ULONG version, long bufferSize);
    void MapBuffers(ASIOBufferInfo *bufferInfos, long numChannels, long bufferSize);
    ASIOError SetCallbacks(ASIOCallbacks *callbacks);
    void Message(long selector)
    {
        if (callbacks_ && callbacks_->asioMessage) callbacks_->asioMessage(selector, 0, nullptr, nullptr);
    }
    bool Ioctl(DWORD code, void *in, DWORD inLen, void *out, DWORD outLen, DWORD *err);
    bool ReadSnapshot(Snapshot *s);
    bool ReadRate(APOLLO_RATE_INFO *r);     /* [Rate] GET_RATE on this handle's unit */
    double CurrentRate();                   /* [Rate] Hz the unit (or the combined device) runs at */
    unsigned long long QpcToNs(LONGLONG qpc) const;
    void SetError(const char *msg) { strcpy_s(err_, sizeof(err_), msg); }
    static DWORD WINAPI ThreadProc(void *p) { static_cast<ApolloAsio *>(p)->Run(); return 0; }
    void Run();

    bool agg_ = false;                  /* [G6] the combined device's class */
    APOLLO_AGG_LAYOUT *layout_ = nullptr;
    ULONG openLayout_ = 0, openEpoch_ = 0, openInLat_ = 0, openOutLat_ = 0;
    HANDLE dev_ = INVALID_HANDLE_VALUE;
    long numIn_ = 0, numOut_ = 0;
    long blockFrames_ = 0;
    bool inActive_[MAX_CH] = {};
    bool outActive_[MAX_CH] = {};
    ASIOCallbacks *callbacks_ = nullptr;
    bool timeInfoMode_ = false;
    bool buffersCreated_ = false;
    bool started_ = false;
    HANDLE event_ = nullptr;
    HANDLE thread_ = nullptr;
    volatile LONG stopThread_ = 0;
    PAPOLLO_ASIO_SHARED shared_ = nullptr;
    volatile LONG curHalf_ = 0;         /* the switch bufferSwitch is processing now */
    volatile LONG curSwitch_ = 0;
    LONG skipped_ = 0;                  /* switches this thread never processed (woke too late) */
    APOLLO_ASIO_OPEN_OUT map_ = {};
    LONGLONG qpcFreq_ = 0;
    ASIOTime asioTime_ = {};
    char err_[124] = "";
};

/* COM factory table for the SDK's dllentry.cpp. */
CFactoryTemplate g_Templates[2] = {
    { L"AviumApolloAsio", &CLSID_AviumApolloAsio, ApolloAsio::CreateInstance },
    { L"AviumApolloAggAsio", &CLSID_AviumApolloAggAsio, ApolloAsio::CreateAggInstance },   /* [G6] */
};
int g_cTemplates = 2;

extern LONG RegisterAsioDriver(CLSID, char *, char *, char *, char *);
extern LONG UnregisterAsioDriver(CLSID, char *, char *);

/* [G6] Two ASIO drivers from one DLL: the x8p alone (unchanged) and the combined device. */
HRESULT _stdcall DllRegisterServer()
{
    LONG rc = RegisterAsioDriver(CLSID_AviumApolloAsio, (char *)DLL_NAME, (char *)DRIVER_NAME,
                                 (char *)DRIVER_NAME, (char *)"Apartment");
    LONG rc2 = RegisterAsioDriver(CLSID_AviumApolloAggAsio, (char *)DLL_NAME, (char *)AGG_DRIVER_NAME,
                                  (char *)AGG_DRIVER_NAME, (char *)"Apartment");
    return (rc || rc2) ? E_FAIL : S_OK;
}

HRESULT _stdcall DllUnregisterServer()
{
    LONG rc = UnregisterAsioDriver(CLSID_AviumApolloAsio, (char *)DLL_NAME, (char *)DRIVER_NAME);
    LONG rc2 = UnregisterAsioDriver(CLSID_AviumApolloAggAsio, (char *)DLL_NAME, (char *)AGG_DRIVER_NAME);
    return (rc || rc2) ? E_FAIL : S_OK;
}

/* ------------------------------------------------------------------ */

ApolloAsio::~ApolloAsio()
{
    if (started_) stop();
    if (buffersCreated_) disposeBuffers();
    if (dev_ != INVALID_HANDLE_VALUE) CloseHandle(dev_);
    delete layout_;
}

bool ApolloAsio::ReadLayout()
{
    DWORD err = 0;
    if (!layout_) layout_ = new (std::nothrow) APOLLO_AGG_LAYOUT;
    if (!layout_) return false;
    memset(layout_, 0, sizeof(*layout_));
    if (!Ioctl(IOCTL_APOLLO_AGG_GET_LAYOUT, nullptr, 0, layout_, sizeof(*layout_), &err)) {
        SetError(err == ERROR_INVALID_FUNCTION ? "This apollo.sys has no combined device (gate G6 or later needed)."
                                               : "The Apollo driver did not give the combined channel list.");
        return false;
    }
    return true;
}

/*
 * [Twin X G2] The x8p only: every Apollo interface is asked GET_INFO through a short-lived
 * read-only handle and the one with DeviceType 0x22 is opened (apollo_select.h). Another unit
 * on the driver (the Twin X in observe mode) is never opened for audio, whatever the order.
 * [G6] The combined device: any unit's handle reaches the driver-global aggregate. The x8p is
 * preferred (it is upstream on the Thunderbolt chain and outlives the Twin X, spec 6.2), else the
 * first unit the driver runs.
 */
bool ApolloAsio::OpenDevice()
{
    APOLLO_UNIT_ENTRY *units = new (std::nothrow) APOLLO_UNIT_ENTRY[APOLLO_SELECT_MAX];
    if (units == nullptr) {
        SetError("Out of memory.");
        return false;
    }
    int n = ApolloListUnits(units, APOLLO_SELECT_MAX, nullptr, 0);
    int pick = n > 0 ? ApolloPickUnit(units, n, APOLLO_PICK_X8P) : -1;
    for (int i = 0; agg_ && pick < 0 && i < n; i++) {
        if (units[i].Error == 0 && ApolloInfoActive(&units[i].Info, units[i].InfoBytes)) pick = i;
    }
    bool ok = false;
    if (pick < 0) {
        SetError(agg_ ? "No Apollo runs on the open driver (apollo.sys)."
                 : n <= 0 ? "No Apollo x8p on the open driver (apollo.sys is not running)."
                 : "No Apollo x8p on the open driver (another Apollo is there, but this ASIO driver runs the x8p only).");
    } else {
        dev_ = CreateFileW(units[pick].Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        ok = dev_ != INVALID_HANDLE_VALUE;
        if (!ok) SetError("The Apollo driver refused the connection.");
    }
    delete[] units;
    return ok;
}

bool ApolloAsio::Ioctl(DWORD code, void *in, DWORD inLen, void *out, DWORD outLen, DWORD *err)
{
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(dev_, code, in, inLen, out, outLen, &ret, nullptr);
    if (err) *err = ok ? 0 : GetLastError();
    return ok != FALSE;
}

ASIOBool ApolloAsio::init(void *)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpcFreq_ = f.QuadPart;
    if (agg_) {
        /* [G6] the combined device: its channel list is the driver's layout (spec 6.6) */
        if (dev_ == INVALID_HANDLE_VALUE && !OpenDevice()) return ASIOFalse;
        if (!ReadLayout()) return ASIOFalse;
        if (!(layout_->Flags & APOLLO_AGG_LAYOUT_ACTIVE)) {
            SetError("The combined Apollo device is off, or no Apollo runs (Apollo Control, Units & routing).");
            return ASIOFalse;
        }
        numIn_ = (long)min(layout_->InCount, (ULONG)MAX_CH);
        numOut_ = (long)min(layout_->OutCount, (ULONG)MAX_CH);
        SetError("");
        return ASIOTrue;
    }
    if (dev_ == INVALID_HANDLE_VALUE && !OpenDevice()) return ASIOFalse;
    APOLLO_INFO info = {};
    DWORD infoBytes = 0;
    if (ApolloQueryInfo(dev_, &info, &infoBytes) != 0) {
        SetError("The Apollo driver did not answer.");
        return ASIOFalse;
    }
    /* [Twin X G2] the handle must still be the x8p, and one the driver runs (not observe). */
    if (!ApolloInfoIsX8p(&info, infoBytes) || !ApolloInfoActive(&info, infoBytes)) {
        SetError("The Apollo x8p is not running on the open driver (observe mode or another unit).");
        return ASIOFalse;
    }
    if (info.Version < 2 || info.Stage != ApolloStageConnected || !(info.Flags & APOLLO_FLAG_CONNECTED)) {
        SetError("The Apollo x8p is not ready (still starting up, or bring-up failed: see Apollo Control).");
        return ASIOFalse;
    }
    /* [Rate] any of the six rates (the host asks which with Get/CanSampleRate) */
    if (info.SampleRate == 0 || !(info.Flags & APOLLO_FLAG_TRANSPORT)) {
        SetError("The Apollo's audio clock is not running.");
        return ASIOFalse;
    }
    numIn_ = (long)info.RecChannels < N_IN_NAMES ? (long)info.RecChannels : N_IN_NAMES;
    numOut_ = (long)info.PlayChannels < N_OUT_NAMES ? (long)info.PlayChannels : N_OUT_NAMES;
    SetError("");
    return ASIOTrue;
}

ASIOError ApolloAsio::getChannels(long *numInputChannels, long *numOutputChannels)
{
    if (dev_ == INVALID_HANDLE_VALUE) return ASE_NotPresent;
    *numInputChannels = numIn_;
    *numOutputChannels = numOut_;
    return ASE_OK;
}

/*
 * [Lat] What the driver reports (docs/ASIO.md, Latency): the copy engine (guard, lead, FRAME_CFG), the converters
 * (ADC, DAC) and, while the input strips run, the strip DSP's block, the strips' record delay and an over-budget
 * plug-in chain. The shared block once the buffers exist (the driver keeps it current; Run() sends
 * kAsioLatenciesChanged when it moves), before that the driver's GET_RATE (or the combined layout).
 */
ASIOError ApolloAsio::getLatencies(long *inputLatency, long *outputLatency)
{
    long n = blockFrames_ ? blockFrames_ : PREF_FRAMES;
    APOLLO_RATE_INFO r;
    if (shared_) {
        *inputLatency = n + (long)shared_->InputLatencyFrames;
        *outputLatency = n + (long)shared_->OutputLatencyFrames;
    } else if (agg_ && layout_) {
        /* [G6] aligned figures (spec 6.3.5): true for every channel of every unit; [Lat] with the units' device side */
        *inputLatency = n + (long)layout_->InLatency;
        *outputLatency = n + (long)layout_->OutLatency;
    } else if (!agg_ && ReadRate(&r) && r.LeadFrames != 0) {
        *inputLatency = n + (long)(r.GuardFrames + r.AdcFrames + r.InPathFrames);
        *outputLatency = n + (long)(r.LeadFrames + r.FrameCfg + r.DacFrames);
    } else {
        *inputLatency = n + EST_IN_LATENCY;
        *outputLatency = n + EST_OUT_LATENCY;
    }
    return ASE_OK;
}

ASIOError ApolloAsio::getBufferSize(long *minSize, long *maxSize, long *preferredSize, long *granularity)
{
    *minSize = APOLLO_ASIO_MIN_FRAMES;
    *maxSize = APOLLO_ASIO_MAX_FRAMES;
    *preferredSize = PREF_FRAMES;
    *granularity = -1;                  /* powers of two */
    return ASE_OK;
}

/*
 * [Rate] The rates come from the driver (GET_RATE): what the unit offers (Allowed), or for the combined
 * device what every unit offers (CombinedAllowed), and what it runs at now. A driver before the rate
 * change (no GET_RATE) runs 48 kHz only, as this DLL assumed before.
 */
bool ApolloAsio::ReadRate(APOLLO_RATE_INFO *r)
{
    DWORD err = 0;
    memset(r, 0, sizeof(*r));
    return dev_ != INVALID_HANDLE_VALUE && Ioctl(IOCTL_APOLLO_GET_RATE, nullptr, 0, r, sizeof(*r), &err) &&
           r->Version == APOLLO_RATE_VERSION;
}

double ApolloAsio::CurrentRate()
{
    APOLLO_RATE_INFO r;
    if (!ReadRate(&r)) return (double)SAMPLE_RATE;
    if (agg_ && r.CombinedRate != 0) return (double)r.CombinedRate;
    return r.Rate != 0 ? (double)r.Rate : (double)SAMPLE_RATE;
}

static ULONG RateIndexOf(ASIOSampleRate sr)
{
    static const double hz[7] = { 0, 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
    for (ULONG i = 1; i < 7; i++) {
        if (sr == hz[i]) return i;
    }
    return 0;
}

ASIOError ApolloAsio::canSampleRate(ASIOSampleRate sampleRate)
{
    APOLLO_RATE_INFO r;
    ULONG idx = RateIndexOf(sampleRate);

    if (dev_ == INVALID_HANDLE_VALUE) return ASE_NotPresent;
    if (idx == 0) return ASE_NoClock;
    if (!ReadRate(&r)) return sampleRate == (double)SAMPLE_RATE ? ASE_OK : ASE_NoClock;
    ULONG mask = agg_ ? r.CombinedAllowed : r.Allowed;
    return (mask & (1ul << idx)) ? ASE_OK : ASE_NoClock;
}

ASIOError ApolloAsio::getSampleRate(ASIOSampleRate *sampleRate)
{
    if (dev_ == INVALID_HANDLE_VALUE) return ASE_NotPresent;
    *sampleRate = CurrentRate();
    return ASE_OK;
}

/*
 * [Rate] The host asks for a rate: the driver changes the unit (or both units of the combined device,
 * the master first) with SET_RATE on this handle, so a client this host opened (buffers created,
 * stopped) does not block it. 2-3 s per unit. While running the host has to stop first (ASE_InvalidMode).
 */
ASIOError ApolloAsio::setSampleRate(ASIOSampleRate sampleRate)
{
    APOLLO_RATE_SET req = {};
    APOLLO_RATE_INFO r = {};
    DWORD err = 0;
    ULONG idx = RateIndexOf(sampleRate);

    if (dev_ == INVALID_HANDLE_VALUE) return ASE_NotPresent;
    if (sampleRate == CurrentRate()) return ASE_OK;
    if (idx == 0 || canSampleRate(sampleRate) != ASE_OK) {
        SetError("The Apollo does not run at that sample rate.");
        return ASE_NoClock;
    }
    if (started_) {
        SetError("Stop the audio before changing the sample rate.");
        return ASE_InvalidMode;
    }
    req.Version = APOLLO_RATE_VERSION;
    req.Rate = (ULONG)sampleRate;
    req.Flags = agg_ ? APOLLO_RATE_SET_COMBINED : 0;
    if (!Ioctl(IOCTL_APOLLO_SET_RATE, &req, sizeof(req), &r, sizeof(r), &err)) {
        SetError(err == ERROR_INVALID_FUNCTION ? "This apollo.sys runs at 48 kHz only (no rate change)."
                                               : "The Apollo driver did not change the sample rate.");
        return err == ERROR_INVALID_FUNCTION ? ASE_NoClock : ASE_HWMalfunction;
    }
    if (r.Result != APOLLO_RATE_ERR_NONE) {
        char msg[124];
        sprintf_s(msg, sizeof(msg), "%.*s", (int)strnlen(r.Why, sizeof(r.Why)), r.Why);
        SetError(msg[0] ? msg : "The Apollo driver did not change the sample rate.");
        switch (r.Result) {
        case APOLLO_RATE_ERR_RATE:
        case APOLLO_RATE_ERR_UNSUPPORTED:
            return ASE_NoClock;
        case APOLLO_RATE_ERR_ASIO:
        case APOLLO_RATE_ERR_COMBINED:
        case APOLLO_RATE_ERR_COMBINED_ASIO:
        case APOLLO_RATE_ERR_BUSY:
            return ASE_InvalidMode;
        default:
            return ASE_HWMalfunction;
        }
    }
    if (agg_) ReadLayout();                         /* latencies at the new rate (the x8p's channels stay) */
    /* the buffers stay, the latencies moved; the combined device's layout version moved too */
    if (buffersCreated_) Message(agg_ ? kAsioResetRequest : kAsioLatenciesChanged);
    SetError("");
    return ASE_OK;
}

ASIOError ApolloAsio::getClockSources(ASIOClockSource *clocks, long *numSources)
{
    if (*numSources < 1) return ASE_InvalidParameter;
    if (agg_ && layout_ && layout_->UnitCount > 0) {
        /* [G6] one entry per unit: the timeline unit's clock, the others drift-corrected to it (6.6) */
        long n = 0;
        for (ULONG u = 0; u < layout_->UnitCount && u < APOLLO_AGG_MAX_UNITS && n < *numSources; u++, n++) {
            const char *m = layout_->Model[u] == APOLLO_INFO_MODEL_X8P ? "x8p" :
                            (layout_->Model[u] == APOLLO_INFO_MODEL_TWINX ? "Twin X" : "Apollo");
            memset(&clocks[n], 0, sizeof(clocks[n]));
            clocks[n].index = (long)u;
            clocks[n].associatedChannel = -1;
            clocks[n].associatedGroup = -1;
            clocks[n].isCurrentSource = (layout_->Timeline == u) ? ASIOTrue : ASIOFalse;
            sprintf_s(clocks[n].name, sizeof(clocks[n].name), "%s clock%s", m,
                      layout_->Master == u ? " (master)" : " (follower)");
        }
        *numSources = n;
        return ASE_OK;
    }
    memset(clocks, 0, sizeof(*clocks));
    clocks[0].index = 0;
    clocks[0].associatedChannel = -1;
    clocks[0].associatedGroup = -1;
    clocks[0].isCurrentSource = ASIOTrue;
    strcpy_s(clocks[0].name, sizeof(clocks[0].name), "Internal");
    *numSources = 1;
    return ASE_OK;
}

ASIOError ApolloAsio::setClockSource(long reference)
{
    if (agg_ && layout_) {
        /* [G6] the master is part of the combined configuration: change it there (user level) */
        if (reference < 0 || (ULONG)reference >= layout_->UnitCount) return ASE_InvalidParameter;
        if ((ULONG)reference == layout_->Master) return ASE_OK;
        APOLLO_AGG_CONFIG *c = new (std::nothrow) APOLLO_AGG_CONFIG;
        APOLLO_AGG_SET_RESULT r = {};
        bool ok = false;
        if (c && Ioctl(IOCTL_APOLLO_AGG_GET_CONFIG, nullptr, 0, c, sizeof(*c), nullptr)) {
            c->Flags &= ~APOLLO_AGG_CFG_DEFAULT;
            c->Master = (ULONG)reference;
            c->JsonBytes = 0;           /* Apollo Control rebuilds its view from the driver's */
            /* [G7 review] a refused configuration completes with success and the reason in r.Error */
            ok = Ioctl(IOCTL_APOLLO_AGG_SET_CONFIG, c, sizeof(*c), &r, sizeof(r), nullptr) && r.Error == 0;
        }
        delete c;
        if (!ok) return ASE_HWMalfunction;
        ReadLayout();
        Message(kAsioResetRequest);     /* the clock domain changed (the driver re-originates its client too) */
        return ASE_OK;
    }
    return reference == 0 ? ASE_OK : ASE_InvalidParameter;
}

ASIOError ApolloAsio::getChannelInfo(ASIOChannelInfo *info)
{
    long n = info->isInput ? numIn_ : numOut_;
    if (info->channel < 0 || info->channel >= n) return ASE_InvalidParameter;
    info->type = ASIOSTInt32LSB;
    info->channelGroup = 0;
    info->isActive = (info->isInput ? inActive_[info->channel] : outActive_[info->channel]) ? ASIOTrue : ASIOFalse;
    if (agg_ && layout_) {
        /* [G6] "x8p MIC/LINE 1", "TwinX MON L": the unit is in the name (spec 6.6) */
        const APOLLO_AGG_LCHAN *l = info->isInput ? &layout_->In[info->channel] : &layout_->Out[info->channel];
        sprintf_s(info->name, sizeof(info->name), "%.*s", (int)strnlen(l->Name, APOLLO_AGG_NAME_CHARS), l->Name);
        info->channelGroup = l->Unit;
        return ASE_OK;
    }
    strcpy_s(info->name, sizeof(info->name), info->isInput ? kInNames[info->channel] : kOutNames[info->channel]);
    return ASE_OK;
}

ASIOError ApolloAsio::createBuffers(ASIOBufferInfo *bufferInfos, long numChannels, long bufferSize,
                                    ASIOCallbacks *callbacks)
{
    APOLLO_ASIO_OPEN_IN req = {};
    long i, nIn = 0, nOut = 0;
    DWORD err = 0;

    if (dev_ == INVALID_HANDLE_VALUE) return ASE_NotPresent;
    if (buffersCreated_) return ASE_InvalidMode;
    if (agg_) {
        if (!callbacks) return ASE_InvalidParameter;
        ASIOError e = CreateAggBuffers(bufferInfos, numChannels, bufferSize);
        return e == ASE_OK ? SetCallbacks(callbacks) : e;
    }
    if (!callbacks || bufferSize < APOLLO_ASIO_MIN_FRAMES || bufferSize > APOLLO_ASIO_MAX_FRAMES ||
        (bufferSize & (bufferSize - 1)) != 0) {
        SetError("Buffer size must be a power of two from 64 to 1024 samples.");
        return ASE_InvalidParameter;
    }
    memset(inActive_, 0, sizeof(inActive_));
    memset(outActive_, 0, sizeof(outActive_));
    for (i = 0; i < numChannels; i++) {
        long ch = bufferInfos[i].channelNum;
        if (bufferInfos[i].isInput) {
            if (ch < 0 || ch >= numIn_ || nIn >= APOLLO_ASIO_MAX_IN || inActive_[ch]) return ASE_InvalidParameter;
            inActive_[ch] = true;
            req.InSlots[nIn++] = (UCHAR)ch;     /* ASIO channel n = ring slot n */
        } else {
            if (ch < 0 || ch >= numOut_ || nOut >= APOLLO_ASIO_MAX_OUT || outActive_[ch]) return ASE_InvalidParameter;
            outActive_[ch] = true;
            req.OutSlots[nOut++] = (UCHAR)ch;
        }
    }

    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) return ASE_NoMemory;
    req.Version = APOLLO_ASIO_VERSION;
    req.BufferFrames = (ULONG)bufferSize;
    req.InCount = (ULONG)nIn;
    req.OutCount = (ULONG)nOut;
    req.EventHandle = (ULONGLONG)(ULONG_PTR)event_;
    memset(&map_, 0, sizeof(map_));
    if (!Ioctl(IOCTL_APOLLO_ASIO_OPEN, &req, sizeof(req), &map_, sizeof(map_), &err)) {
        CloseHandle(event_);
        event_ = nullptr;
        if (err == ERROR_INVALID_FUNCTION || err == ERROR_NOT_SUPPORTED) {
            SetError("This apollo.sys build has no ASIO support yet (it arrives with the phase 2 engine).");
            return ASE_NotPresent;
        }
        if (err == ERROR_BUSY) {
            SetError("Another program is already using the Apollo x8p's ASIO driver.");
            return ASE_HWMalfunction;
        }
        SetError("The Apollo driver refused the ASIO buffers.");
        return ASE_HWMalfunction;
    }
    if (!CheckShared(APOLLO_ASIO_VERSION, bufferSize)) return ASE_HWMalfunction;
    MapBuffers(bufferInfos, numChannels, bufferSize);
    return SetCallbacks(callbacks);
}

/* The block the driver mapped must be this DLL's version and size; else the client is closed again. */
bool ApolloAsio::CheckShared(ULONG version, long bufferSize)
{
    shared_ = (PAPOLLO_ASIO_SHARED)(ULONG_PTR)map_.SharedAddress;
    if (shared_ && shared_->Version == version && shared_->BufferFrames == (ULONG)bufferSize) return true;
    Ioctl(IOCTL_APOLLO_ASIO_CLOSE, nullptr, 0, nullptr, 0, nullptr);
    CloseHandle(event_);
    event_ = nullptr;
    shared_ = nullptr;
    SetError("The Apollo driver's ASIO block does not match this DLL; update both.");
    return false;
}

/* Each channel's two halves, in the order the driver was given them; the switch state starts over. */
void ApolloAsio::MapBuffers(ASIOBufferInfo *bufferInfos, long numChannels, long bufferSize)
{
    BYTE *base = (BYTE *)shared_;
    long in = 0, out = 0;
    for (long i = 0; i < numChannels; i++) {
        for (int h = 0; h < 2; h++) {
            ULONG off = bufferInfos[i].isInput ? map_.InOffset[h] + (ULONG)in * map_.ChannelStride
                                               : map_.OutOffset[h] + (ULONG)out * map_.ChannelStride;
            bufferInfos[i].buffers[h] = base + off;
        }
        if (bufferInfos[i].isInput) in++; else out++;
    }
    curHalf_ = 0;
    curSwitch_ = 0;
    skipped_ = 0;
    blockFrames_ = bufferSize;
    openInLat_ = shared_->InputLatencyFrames;      /* [Lat] Run() tells the host when the driver moves them */
    openOutLat_ = shared_->OutputLatencyFrames;
}

ASIOError ApolloAsio::SetCallbacks(ASIOCallbacks *callbacks)
{
    callbacks_ = callbacks;
    timeInfoMode_ = callbacks->asioMessage &&
                    callbacks->asioMessage(kAsioSelectorSupported, kAsioSupportsTimeInfo, nullptr, nullptr) == 1 &&
                    callbacks->asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr) == 1;
    buffersCreated_ = true;
    return ASE_OK;
}

/*
 * [G6] The combined client: ASIO channel n = logical channel n of the layout read at init. The
 * driver refuses a layout that changed since (ERROR_REVISION_MISMATCH): read it again and say so.
 */
ASIOError ApolloAsio::CreateAggBuffers(ASIOBufferInfo *bufferInfos, long numChannels, long bufferSize)
{
    APOLLO_ASIO_OPEN_AGG_IN req = {};
    long i, nIn = 0, nOut = 0;
    DWORD err = 0;

    if (bufferSize < APOLLO_ASIO_MIN_FRAMES || bufferSize > APOLLO_ASIO_MAX_FRAMES || (bufferSize & (bufferSize - 1)) != 0) {
        SetError("Buffer size must be a power of two from 64 to 1024 samples.");
        return ASE_InvalidParameter;
    }
    memset(inActive_, 0, sizeof(inActive_));
    memset(outActive_, 0, sizeof(outActive_));
    for (i = 0; i < numChannels; i++) {
        long ch = bufferInfos[i].channelNum;
        if (bufferInfos[i].isInput) {
            if (ch < 0 || ch >= numIn_ || nIn >= MAX_CH || inActive_[ch]) return ASE_InvalidParameter;
            inActive_[ch] = true;
            req.In[nIn++] = (USHORT)ch;
        } else {
            if (ch < 0 || ch >= numOut_ || nOut >= MAX_CH || outActive_[ch]) return ASE_InvalidParameter;
            outActive_[ch] = true;
            req.Out[nOut++] = (USHORT)ch;
        }
    }
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) return ASE_NoMemory;
    req.Version = APOLLO_ASIO_VERSION_AGG;
    req.BufferFrames = (ULONG)bufferSize;
    req.InCount = (ULONG)nIn;
    req.OutCount = (ULONG)nOut;
    req.LayoutVersion = layout_ ? layout_->LayoutVersion : 0;
    req.EventHandle = (ULONGLONG)(ULONG_PTR)event_;
    memset(&map_, 0, sizeof(map_));
    if (!Ioctl(IOCTL_APOLLO_ASIO_OPEN, &req, sizeof(req), &map_, sizeof(map_), &err)) {
        CloseHandle(event_);
        event_ = nullptr;
        if (err == ERROR_REVISION_MISMATCH) {
            ReadLayout();
            SetError("The combined device's channels changed: reopen the ASIO driver.");
            Message(kAsioResetRequest);
            return ASE_HWMalfunction;
        }
        SetError(err == ERROR_BUSY ? "Another program is already using the combined Apollo device." :
                 (err == ERROR_NOT_READY ? "The combined Apollo device is off or no Apollo runs." :
                  "The Apollo driver refused the ASIO buffers."));
        return ASE_HWMalfunction;
    }
    if (!CheckShared(APOLLO_ASIO_VERSION_AGG, bufferSize)) return ASE_HWMalfunction;
    openLayout_ = shared_->Reserved[0];
    openEpoch_ = shared_->Reserved[1];
    MapBuffers(bufferInfos, numChannels, bufferSize);
    return ASE_OK;
}

ASIOError ApolloAsio::disposeBuffers()
{
    if (started_) stop();
    if (buffersCreated_) Ioctl(IOCTL_APOLLO_ASIO_CLOSE, nullptr, 0, nullptr, 0, nullptr);
    if (event_) CloseHandle(event_);
    event_ = nullptr;
    shared_ = nullptr;
    callbacks_ = nullptr;
    blockFrames_ = 0;
    buffersCreated_ = false;
    memset(inActive_, 0, sizeof(inActive_));
    memset(outActive_, 0, sizeof(outActive_));
    return ASE_OK;
}

ASIOError ApolloAsio::start()
{
    if (!buffersCreated_) return ASE_NotPresent;
    if (started_) return ASE_OK;
    stopThread_ = 0;
    thread_ = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    if (!thread_) return ASE_HWMalfunction;
    if (!Ioctl(IOCTL_APOLLO_ASIO_START, nullptr, 0, nullptr, 0, nullptr)) {
        InterlockedExchange(&stopThread_, 1);
        SetEvent(event_);
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
        thread_ = nullptr;
        SetError("The Apollo driver would not start the ASIO stream.");
        return ASE_HWMalfunction;
    }
    started_ = true;
    return ASE_OK;
}

ASIOError ApolloAsio::stop()
{
    if (!started_) return ASE_OK;
    Ioctl(IOCTL_APOLLO_ASIO_STOP, nullptr, 0, nullptr, 0, nullptr);
    InterlockedExchange(&stopThread_, 1);
    SetEvent(event_);
    if (thread_) {
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    started_ = false;
    return ASE_OK;
}

bool ApolloAsio::ReadSnapshot(Snapshot *s)
{
    for (int i = 0; i < 1000; i++) {
        LONG a = shared_->Sequence;
        if (a & 1) {
            YieldProcessor();
            continue;
        }
        MemoryBarrier();
        s->half = shared_->HalfReady;
        s->switches = shared_->SwitchCount;
        s->position = shared_->SamplePosition;
        s->qpc = shared_->Qpc;
        MemoryBarrier();
        if (shared_->Sequence == a) return true;
    }
    return false;
}

unsigned long long ApolloAsio::QpcToNs(LONGLONG qpc) const
{
    unsigned long long q = (unsigned long long)qpc, f = (unsigned long long)qpcFreq_;
    return (q / f) * 1000000000ull + (q % f) * 1000000000ull / f;
}

void ApolloAsio::Run()
{
    DWORD task = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    LONG last = shared_ ? shared_->SwitchCount : 0;
    bool resetSent = false;

    if (mm) AvSetMmThreadPriority(mm, AVRT_PRIORITY_CRITICAL);
    while (!stopThread_) {
        Snapshot s;
        DWORD w = WaitForSingleObject(event_, 500);

        if (stopThread_) break;
        if (w == WAIT_TIMEOUT) {
            /*
             * No half in 500 ms: the unit is gone or the engine stopped ([G6] the combined device: no unit
             * is the clock any more, all gone or Combined off). Ask the host to reset once.
             */
            APOLLO_INFO i = {};
            DWORD ib = 0;
            if (!resetSent && (agg_ ? !ReadLayout() || !(layout_->Flags & APOLLO_AGG_LAYOUT_ACTIVE)
                                    : ApolloQueryInfo(dev_, &i, &ib) != 0 || !(i.Flags & APOLLO_FLAG_TRANSPORT) ||
                                      (i.Flags & APOLLO_FLAG_ENGINE_LOST))) {
                resetSent = true;
                Message(kAsioResetRequest);
            }
            continue;
        }
        if (callbacks_ && callbacks_->asioMessage && (!agg_ || !resetSent)) {
            if (agg_ && (shared_->Reserved[0] != openLayout_ || shared_->Reserved[1] != openEpoch_ ||
                         (shared_->Reserved[2] & APOLLO_AGG_SHARED_STALE) != 0)) {
                /*
                 * [G6] The timeline unit changed (master lost or back: another clock domain) or the
                 * channel list changed (Apply in Apollo Control): the host must reset (spec 6.3.4, 6.6).
                 */
                resetSent = true;
                callbacks_->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
            } else if (shared_->InputLatencyFrames != openInLat_ || shared_->OutputLatencyFrames != openOutLat_) {
                /*
                 * [Lat] The driver moved the reported latencies (the input strips went live or away, a plug-in
                 * chain over its record-delay budget came or went; [G6] the ASRC size): the host asks
                 * getLatencies again.
                 */
                openInLat_ = shared_->InputLatencyFrames;
                openOutLat_ = shared_->OutputLatencyFrames;
                callbacks_->asioMessage(kAsioLatenciesChanged, 0, nullptr, nullptr);
            }
        }
        if (!ReadSnapshot(&s) || s.switches == last) continue;
        if ((LONG)(s.switches - last) > 1) {
            /* Woke too late: the switches in between are gone. Their halves play silence
               (the driver compares ClientDone[h] with the exact switch) and count there. */
            skipped_ += (LONG)(s.switches - last) - 1;
            InterlockedExchange(&shared_->ClientSkipped, skipped_);
        }
        last = s.switches;
        curHalf_ = s.half & 1;
        curSwitch_ = s.switches;
        if (timeInfoMode_) {
            memset(&asioTime_, 0, sizeof(asioTime_));
            asioTime_.timeInfo.speed = 1.0;
            asioTime_.timeInfo.sampleRate = shared_->SampleRate ? (double)shared_->SampleRate : (double)SAMPLE_RATE;
            SetTime64(&asioTime_.timeInfo.systemTime.hi, &asioTime_.timeInfo.systemTime.lo, QpcToNs(s.qpc));
            SetTime64(&asioTime_.timeInfo.samplePosition.hi, &asioTime_.timeInfo.samplePosition.lo,
                      (unsigned long long)s.position);
            asioTime_.timeInfo.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;
            callbacks_->bufferSwitchTimeInfo(&asioTime_, s.half, ASIOTrue);
        } else {
            callbacks_->bufferSwitch(s.half, ASIOTrue);
        }
        /* This exact switch, in its own half: never a newer SwitchCount the driver published meanwhile. */
        InterlockedExchange(&shared_->ClientDone[s.half & 1], s.switches);
    }
    if (mm) AvRevertMmThreadCharacteristics(mm);
}

ASIOError ApolloAsio::getSamplePosition(ASIOSamples *sPos, ASIOTimeStamp *tStamp)
{
    Snapshot s;
    if (!shared_ || !started_) return ASE_SPNotAdvancing;
    if (!ReadSnapshot(&s)) return ASE_SPNotAdvancing;
    SetTime64(&sPos->hi, &sPos->lo, (unsigned long long)s.position);
    SetTime64(&tStamp->hi, &tStamp->lo, QpcToNs(s.qpc));
    return ASE_OK;
}

ASIOError ApolloAsio::outputReady()
{
    /* Called by the host from inside bufferSwitch: the switch being processed, not the live count. */
    if (shared_ && curSwitch_ != 0) InterlockedExchange(&shared_->ClientDone[curHalf_ & 1], curSwitch_);
    return ASE_OK;
}

ASIOError ApolloAsio::controlPanel()
{
    HWND w = FindWindowW(L"AviumApolloControlWnd", nullptr);
    if (w) {
        PostMessageW(w, WM_APP + 3, 0, 0);      /* the app's "show yourself" message */
        return ASE_OK;
    }
    HMODULE self = nullptr;
    WCHAR path[MAX_PATH * 2];
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&CLSID_AviumApolloAsio, &self);
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH * 2);
    if (n == 0) return ASE_NotPresent;
    WCHAR *slash = wcsrchr(path, L'\\');
    if (slash) *slash = 0;
    wcscat_s(path, L"\\..\\..\\app\\bin\\apollo-control.exe");     /* apollo-win\asio\bin -> app\bin */
    return (INT_PTR)ShellExecuteW(nullptr, L"open", path, nullptr, nullptr, SW_SHOWNORMAL) > 32 ? ASE_OK : ASE_NotPresent;
}

ASIOError ApolloAsio::future(long selector, void *)
{
    switch (selector) {
    case kAsioCanTimeInfo:
        return ASE_SUCCESS;
    default:
        return ASE_NotPresent;
    }
}
