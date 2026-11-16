/*
 * wasapi-loop.cpp - glitch test of the Apollo x8p's Windows audio endpoints (PHASE2.md steps 2-3).
 *
 * Plays a continuous 997 Hz sine at -12 dBFS into the render endpoint "MON L/R (Apollo x8p)"
 * through WASAPI, and records the capture endpoint "MON L/R (Apollo x8p)" (the x8p's monitor-mix
 * loopback, record slots 26-27) at the same time. Every captured sample after the sine has
 * arrived and settled is checked against the sine recurrence
 *     x[n] = 2 cos(w) x[n-1] - x[n-2]
 * which does not depend on level, so the monitor knob does not matter. On a clean path the error
 * is about 1e-6 of full scale; a dropout, click, repeated or skipped packet anywhere on the way
 * (Windows audio engine, the driver's packets, the copy engine, the x8p) is orders of magnitude
 * larger. Windows' own capture discontinuity flags and device-position gaps are counted too.
 *
 *   wasapi-loop [seconds=1800] [--threshold 1e-4] [--exclusive] [--raw]
 *               [--render TEXT] [--capture TEXT]
 *   wasapi-loop --tone [seconds=10] [--render TEXT]     play the sine only (hear it, meters)
 *   wasapi-loop --meter TEXT [seconds=0] [--exclusive] [--raw]
 *                                                        peak level of ONE recording endpoint
 *                                                        every 250 ms (0 s = until Ctrl+C)
 *   wasapi-loop --list                                   active endpoints and the defaults
 *
 * TEXT picks the endpoint whose name contains it (defaults "x8p MON L/R" to play, "Monitor mix" to record).
 * --meter needs a TEXT that matches exactly one recording endpoint, e.g. "MIC/LINE 3"; it exits
 * 0 when any non-zero sample arrived, 3 when everything was digital zero, 2 on a setup error.
 * No administrator rights needed. Every 10 s it also prints the driver's own counters
 * (GET_ENDPOINTS, GET_ENGINE_STATS) from a read-only handle.
 *
 * Exit code: 0 clean, 1 glitches, 2 setup error, 3 no signal came back.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <avrt.h>
#include <cfgmgr32.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "..\..\include\apollo_ioctl.h"
#include "..\..\include\apollo_select.h"   /* [Twin X G2] the x8p among several units */

#define PI              3.14159265358979323846
#define FREQ            997.0
#define LEVEL_DB        (-12.0)
#define SETTLE_SECONDS  0.5
#define DETECT_LEVEL    1e-3            /* -60 dBFS: the sine has come back */
#define EVENT_GAP       4800            /* errors closer than 100 ms are one glitch */
#define MAX_PRINTED     40
#define SHARED_BUFFER   400000          /* 40 ms in 100 ns units: 30 ms of slack for this program */
#define REPORT_FRAMES   480000          /* progress every 10 s of checked audio */

static volatile LONG g_stop;
static LONGLONG g_lastRenderWake;                   /* render timing (QPC of the last render wake) */
static ULONG g_renderWakes, g_slowRender;
static double g_maxGapMs, g_maxFillMs;

static BOOL WINAPI OnCtrl(DWORD t)
{
    (void)t;
    InterlockedExchange(&g_stop, 1);
    return TRUE;
}

template <class T> static void SafeRelease(T **p)
{
    if (*p != NULL) {
        (*p)->Release();
        *p = NULL;
    }
}

static void Fail(const char *what, HRESULT hr)
{
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED)
        fprintf(stderr, "%s: the endpoint went away (driver removed or x8p disconnected).\n", what);
    else if (hr == AUDCLNT_E_DEVICE_IN_USE)
        fprintf(stderr, "%s: another program holds the endpoint in exclusive mode.\n", what);
    else if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT)
        fprintf(stderr, "%s: the endpoint refused the format (0x%08lX).\n", what, (unsigned long)hr);
    else
        fprintf(stderr, "%s failed: 0x%08lX\n", what, (unsigned long)hr);
}

static BOOL ContainsI(const WCHAR *hay, const WCHAR *needle)
{
    size_t n = wcslen(needle), i, j;

    if (n == 0)
        return TRUE;
    for (i = 0; hay[i] != 0; i++) {
        for (j = 0; j < n && hay[i + j] != 0; j++) {
            if (towlower(hay[i + j]) != towlower(needle[j]))
                break;
        }
        if (j == n)
            return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Endpoints                                                            */
/* ------------------------------------------------------------------ */

static void GetName(IMMDevice *d, WCHAR *out, size_t cch)
{
    IPropertyStore *ps = NULL;
    PROPVARIANT v;

    out[0] = 0;
    PropVariantInit(&v);
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)) &&
        SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
        wcsncpy_s(out, cch, v.pwszVal, _TRUNCATE);
    PropVariantClear(&v);
    SafeRelease(&ps);
}

/* Caller frees with CoTaskMemFree; NULL when there is no default. */
static WCHAR *DefaultId(IMMDeviceEnumerator *en, EDataFlow flow, ERole role)
{
    IMMDevice *d = NULL;
    WCHAR *id = NULL;

    if (SUCCEEDED(en->GetDefaultAudioEndpoint(flow, role, &d)))
        d->GetId(&id);
    SafeRelease(&d);
    return id;
}

static int ListEndpoints(IMMDeviceEnumerator *en)
{
    int flow;

    for (flow = 0; flow < 2; flow++) {
        EDataFlow df = flow ? eCapture : eRender;
        IMMDeviceCollection *col = NULL;
        WCHAR *defCon = DefaultId(en, df, eConsole);
        WCHAR *defCom = DefaultId(en, df, eCommunications);
        UINT n = 0, i;

        printf("%s endpoints (active):\n", flow ? "Capture" : "Render");
        if (SUCCEEDED(en->EnumAudioEndpoints(df, DEVICE_STATE_ACTIVE, &col)) && SUCCEEDED(col->GetCount(&n))) {
            for (i = 0; i < n; i++) {
                IMMDevice *d = NULL;
                WCHAR *id = NULL;
                WCHAR name[256];

                if (FAILED(col->Item(i, &d)))
                    continue;
                d->GetId(&id);
                GetName(d, name, ARRAYSIZE(name));
                printf("  %-9s %-6s %ls\n",
                       (id && defCon && wcscmp(id, defCon) == 0) ? "[default]" : "",
                       (id && defCom && wcscmp(id, defCom) == 0) ? "[comm]" : "", name);
                CoTaskMemFree(id);
                SafeRelease(&d);
            }
        }
        if (n == 0)
            printf("  (none)\n");
        SafeRelease(&col);
        CoTaskMemFree(defCon);
        CoTaskMemFree(defCom);
    }
    return 0;
}

/* The active endpoint of this flow whose name contains Match; among several, the MON L/R one. */
static IMMDevice *FindEndpoint(IMMDeviceEnumerator *en, EDataFlow flow, const WCHAR *match, WCHAR *name,
                               size_t cch)
{
    IMMDeviceCollection *col = NULL;
    IMMDevice *best = NULL;
    UINT n = 0, i;

    name[0] = 0;
    if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col)) || FAILED(col->GetCount(&n))) {
        SafeRelease(&col);
        return NULL;
    }
    for (i = 0; i < n; i++) {
        IMMDevice *d = NULL;
        WCHAR nm[256];

        if (FAILED(col->Item(i, &d)))
            continue;
        GetName(d, nm, ARRAYSIZE(nm));
        /* [Endpoints] the loopback recording endpoint is "Monitor mix" (older builds: "MON L/R"); playback is "MON L/R" */
        const WCHAR *prefer = (flow == eRender) ? L"MON L/R" : L"Monitor mix";
        if (ContainsI(nm, match) && (best == NULL || ContainsI(nm, prefer) || ContainsI(nm, L"MON L/R"))) {
            BOOL better = (best == NULL) || (!ContainsI(name, prefer) && !ContainsI(name, L"MON L/R"));
            if (better) {
                SafeRelease(&best);
                best = d;
                d = NULL;
                wcsncpy_s(name, cch, nm, _TRUNCATE);
            }
        }
        SafeRelease(&d);
    }
    SafeRelease(&col);
    if (best == NULL)
        fprintf(stderr, "No active %s endpoint whose name contains \"%ls\". Is apollo.sys installed with "
                        "the endpoint build and the x8p connected? (wasapi-loop --list shows them all.)\n",
                flow == eRender ? "playback" : "recording", match);
    return best;
}

/* Other programs with an active session on this endpoint mix into the test signal. */
static ULONG OtherActiveSessions(IMMDevice *dev)
{
    IAudioSessionManager2 *m = NULL;
    IAudioSessionEnumerator *e = NULL;
    int n = 0, i;
    ULONG active = 0;

    if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, NULL, (void **)&m)))
        return 0;
    if (SUCCEEDED(m->GetSessionEnumerator(&e)) && SUCCEEDED(e->GetCount(&n))) {
        for (i = 0; i < n; i++) {
            IAudioSessionControl *c = NULL;
            IAudioSessionControl2 *c2 = NULL;
            AudioSessionState st = AudioSessionStateInactive;
            DWORD pid = 0;

            if (FAILED(e->GetSession(i, &c)))
                continue;
            if (SUCCEEDED(c->GetState(&st)) && st == AudioSessionStateActive) {
                if (SUCCEEDED(c->QueryInterface(__uuidof(IAudioSessionControl2), (void **)&c2))) {
                    c2->GetProcessId(&pid);
                    SafeRelease(&c2);
                }
                if (pid != GetCurrentProcessId()) {
                    active++;
                    printf("  warning: process %lu is playing into this endpoint; its sound mixes into the "
                           "test tone and will count as glitches.\n", pid);
                }
            }
            SafeRelease(&c);
        }
    }
    SafeRelease(&e);
    SafeRelease(&m);
    return active;
}

/* ------------------------------------------------------------------ */
/* Sample formats                                                       */
/* ------------------------------------------------------------------ */

enum SampleKind { SK_FLOAT32, SK_INT16, SK_INT24, SK_INT32 };

struct Format {
    SampleKind kind;
    UINT32 channels;
    UINT32 blockAlign;
    UINT32 rate;
    UINT32 validBits;
};

static BOOL DescribeFormat(const WAVEFORMATEX *wf, Format *f)
{
    WORD tag = wf->wFormatTag;

    f->channels = wf->nChannels;
    f->blockAlign = wf->nBlockAlign;
    f->rate = wf->nSamplesPerSec;
    f->validBits = wf->wBitsPerSample;
    if (tag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
        const WAVEFORMATEXTENSIBLE *x = (const WAVEFORMATEXTENSIBLE *)wf;

        if (x->Samples.wValidBitsPerSample != 0)
            f->validBits = x->Samples.wValidBitsPerSample;
        if (IsEqualGUID(x->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
            tag = WAVE_FORMAT_IEEE_FLOAT;
        else if (IsEqualGUID(x->SubFormat, KSDATAFORMAT_SUBTYPE_PCM))
            tag = WAVE_FORMAT_PCM;
        else
            return FALSE;
    }
    if (tag == WAVE_FORMAT_IEEE_FLOAT && wf->wBitsPerSample == 32)
        f->kind = SK_FLOAT32;
    else if (tag == WAVE_FORMAT_PCM && wf->wBitsPerSample == 16)
        f->kind = SK_INT16;
    else if (tag == WAVE_FORMAT_PCM && wf->wBitsPerSample == 24)
        f->kind = SK_INT24;
    else if (tag == WAVE_FORMAT_PCM && wf->wBitsPerSample == 32)
        f->kind = SK_INT32;
    else
        return FALSE;
    return f->channels >= 1 && f->rate >= 8000;
}

static const char *KindName(const Format *f)
{
    switch (f->kind) {
    case SK_FLOAT32: return "32-bit float";
    case SK_INT16:   return "16-bit PCM";
    case SK_INT24:   return "24-bit packed PCM";
    default:         return "32-bit PCM";
    }
}

static double ReadSample(const Format *f, const BYTE *frame, UINT32 ch)
{
    switch (f->kind) {
    case SK_FLOAT32:
        return ((const float *)frame)[ch];
    case SK_INT16:
        return ((const SHORT *)frame)[ch] / 32768.0;
    case SK_INT32:
        return ((const LONG *)frame)[ch] / 2147483648.0;
    default: {
        const BYTE *p = frame + 3 * ch;
        LONG v = (LONG)(((ULONG)p[0] << 8) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 24));
        return v / 2147483648.0;
    }
    }
}

static void WriteSample(const Format *f, BYTE *frame, UINT32 ch, double v)
{
    switch (f->kind) {
    case SK_FLOAT32:
        ((float *)frame)[ch] = (float)v;
        break;
    case SK_INT16:
        ((SHORT *)frame)[ch] = (SHORT)floor(v * 32767.0 + 0.5);
        break;
    case SK_INT32: {
        LONG s = (LONG)floor(v * 2147483647.0 + 0.5);
        if (f->validBits < 32)
            s = (LONG)((ULONG)s & ~((1ul << (32 - f->validBits)) - 1));
        ((LONG *)frame)[ch] = s;
        break;
    }
    default: {
        LONG s = (LONG)floor(v * 8388607.0 + 0.5);
        BYTE *p = frame + 3 * ch;
        p[0] = (BYTE)s;
        p[1] = (BYTE)(s >> 8);
        p[2] = (BYTE)(s >> 16);
        break;
    }
    }
}

/* ------------------------------------------------------------------ */
/* WASAPI streams                                                       */
/* ------------------------------------------------------------------ */

struct Stream {
    IAudioClient *client;
    IAudioRenderClient *render;
    IAudioCaptureClient *capture;
    HANDLE event;
    WAVEFORMATEX *mix;              /* shared mode: CoTaskMem from GetMixFormat */
    WAVEFORMATEXTENSIBLE excl;      /* exclusive mode: what the driver advertises */
    Format fmt;
    UINT32 bufferFrames;
    BOOL exclusive;
};

static HRESULT ActivateClient(IMMDevice *dev, BOOL raw, IAudioClient **out)
{
    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void **)out);

    if (FAILED(hr)) {
        Fail("IMMDevice::Activate(IAudioClient)", hr);
        return hr;
    }
    if (raw) {
        IAudioClient2 *c2 = NULL;

        hr = (*out)->QueryInterface(__uuidof(IAudioClient2), (void **)&c2);
        if (SUCCEEDED(hr)) {
            AudioClientProperties p;

            ZeroMemory(&p, sizeof(p));
            p.cbSize = sizeof(p);
            p.bIsOffload = FALSE;
            p.eCategory = AudioCategory_Other;
            p.Options = AUDCLNT_STREAMOPTIONS_RAW;
            hr = c2->SetClientProperties(&p);
            SafeRelease(&c2);
        }
        if (FAILED(hr)) {
            Fail("raw stream option (IAudioClient2::SetClientProperties)", hr);
            SafeRelease(out);
        }
    }
    return hr;
}

/* Channels of the endpoint's device format (the driver's own format): 1 or 2, 2 if unknown. */
static WORD DeviceChannels(IMMDevice *dev)
{
    IPropertyStore *ps = NULL;
    PROPVARIANT v;
    WORD ch = 2;

    PropVariantInit(&v);
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)) &&
        SUCCEEDED(ps->GetValue(PKEY_AudioEngine_DeviceFormat, &v)) && v.vt == VT_BLOB &&
        v.blob.pBlobData != NULL && v.blob.cbSize >= sizeof(WAVEFORMATEX)) {
        WORD n = ((const WAVEFORMATEX *)v.blob.pBlobData)->nChannels;

        if (n == 1 || n == 2)
            ch = n;
    }
    PropVariantClear(&v);
    SafeRelease(&ps);
    return ch;
}

static HRESULT OpenStream(IMMDevice *dev, BOOL render, BOOL exclusive, BOOL raw, Stream *s)
{
    const WAVEFORMATEX *wf;
    HRESULT hr;
    int attempt;

    ZeroMemory(s, sizeof(*s));
    s->exclusive = exclusive;
    s->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (s->event == NULL)
        return E_OUTOFMEMORY;

    if (exclusive) {
        REFERENCE_TIME period = 0, minPeriod = 0;
        WAVEFORMATEXTENSIBLE *x = &s->excl;
        WORD ch = DeviceChannels(dev);              /* MIC/LINE n endpoints are mono */

        x->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        x->Format.nChannels = ch;
        x->Format.nSamplesPerSec = 48000;
        x->Format.wBitsPerSample = 32;
        x->Format.nBlockAlign = (WORD)(4 * ch);
        x->Format.nAvgBytesPerSec = 48000 * 4 * ch;
        x->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        x->Samples.wValidBitsPerSample = 24;
        x->dwChannelMask = (ch == 1) ? KSAUDIO_SPEAKER_MONO : KSAUDIO_SPEAKER_STEREO;
        x->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        wf = &x->Format;
        for (attempt = 0; attempt < 2; attempt++) {
            hr = ActivateClient(dev, raw, &s->client);
            if (FAILED(hr))
                return hr;
            if (attempt == 0) {
                hr = s->client->GetDevicePeriod(&period, &minPeriod);
                if (FAILED(hr)) {
                    Fail("IAudioClient::GetDevicePeriod", hr);
                    return hr;
                }
            }
            hr = s->client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period,
                                       period, wf, NULL);
            if (hr != AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
                break;
            {
                UINT32 frames = 0;

                s->client->GetBufferSize(&frames);
                period = (REFERENCE_TIME)(10000000.0 * frames / 48000.0 + 0.5);
                SafeRelease(&s->client);
            }
        }
    } else {
        hr = ActivateClient(dev, raw, &s->client);
        if (FAILED(hr))
            return hr;
        hr = s->client->GetMixFormat(&s->mix);
        if (FAILED(hr)) {
            Fail("IAudioClient::GetMixFormat", hr);
            return hr;
        }
        wf = s->mix;
        hr = s->client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, SHARED_BUFFER, 0,
                                   wf, NULL);
    }
    if (FAILED(hr)) {
        Fail(render ? "IAudioClient::Initialize (playback)" : "IAudioClient::Initialize (recording)", hr);
        return hr;
    }
    if (!DescribeFormat(wf, &s->fmt)) {
        fprintf(stderr, "Unsupported stream format (tag 0x%04X, %u bits).\n", wf->wFormatTag, wf->wBitsPerSample);
        return E_FAIL;
    }
    hr = s->client->SetEventHandle(s->event);
    if (SUCCEEDED(hr))
        hr = s->client->GetBufferSize(&s->bufferFrames);
    if (SUCCEEDED(hr)) {
        if (render)
            hr = s->client->GetService(__uuidof(IAudioRenderClient), (void **)&s->render);
        else
            hr = s->client->GetService(__uuidof(IAudioCaptureClient), (void **)&s->capture);
    }
    if (FAILED(hr))
        Fail("WASAPI stream setup", hr);
    return hr;
}

static void CloseStream(Stream *s)
{
    if (s->client != NULL)
        s->client->Stop();
    SafeRelease(&s->render);
    SafeRelease(&s->capture);
    SafeRelease(&s->client);
    if (s->mix != NULL)
        CoTaskMemFree(s->mix);
    s->mix = NULL;
    if (s->event != NULL)
        CloseHandle(s->event);
    s->event = NULL;
}

/* ------------------------------------------------------------------ */
/* Driver counters (read-only handle, no admin needed)                  */
/* ------------------------------------------------------------------ */

struct DriverCounters {
    BOOL valid;
    ULONG renderUnderruns, captureOverruns, dropped, latePackets, notifyFailures;
    ULONG lateTicks, maxTickUs, asioState;
};

/* [Twin X G2] The x8p's counters only (GET_INFO DeviceType 0x22, apollo_select.h), never another unit's. */
static HANDLE OpenApollo(void)
{
    static APOLLO_UNIT_ENTRY units[APOLLO_SELECT_MAX];
    int n = ApolloListUnits(units, APOLLO_SELECT_MAX, NULL, 0);
    int pick = (n > 0) ? ApolloPickUnit(units, n, APOLLO_PICK_X8P) : -1;

    if (pick < 0)
        return INVALID_HANDLE_VALUE;
    return CreateFileW(units[pick].Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
}

static void ReadDriver(HANDLE h, DriverCounters *c)
{
    /* [Endpoints] GET_ENDPOINTS v2: room for every row (a v1 driver fills 16) */
    static struct { APOLLO_ENDPOINTS head; APOLLO_ENDPOINT_INFO more[APOLLO_ENDPOINTS_ROWS_MAX - APOLLO_ENDPOINTS_MAX]; } e;
    APOLLO_ENGINE_STATS s;
    ULONG flags = 0, i, rows;
    DWORD r = 0;

    ZeroMemory(c, sizeof(*c));
    if (h == INVALID_HANDLE_VALUE)
        return;
    ZeroMemory(&e, sizeof(e));
    ZeroMemory(&s, sizeof(s));
    if (!DeviceIoControl(h, IOCTL_APOLLO_GET_ENDPOINTS, NULL, 0, &e, sizeof(e), &r, NULL) || r < sizeof(e.head))
        return;
    rows = (ULONG)((r - FIELD_OFFSET(APOLLO_ENDPOINTS, Endpoint)) / sizeof(APOLLO_ENDPOINT_INFO));
    if (!DeviceIoControl(h, IOCTL_APOLLO_GET_ENGINE_STATS, &flags, sizeof(flags), &s, sizeof(s), &r, NULL) ||
        r < sizeof(s))
        return;
    for (i = 0; i < e.head.Count && i < rows && i < APOLLO_ENDPOINTS_ROWS_MAX; i++) {
        const APOLLO_ENDPOINT_INFO *p = (const APOLLO_ENDPOINT_INFO *)((const BYTE *)&e.head +
            FIELD_OFFSET(APOLLO_ENDPOINTS, Endpoint) + (SIZE_T)i * sizeof(APOLLO_ENDPOINT_INFO));

        if (p->Render)
            c->renderUnderruns += p->Underruns;
        else
            c->captureOverruns += p->Overruns;
        c->dropped += p->Dropped;
        c->latePackets += p->LatePackets;
        c->notifyFailures += p->NotifyFailures;
    }
    c->lateTicks = s.LateTicks;
    c->maxTickUs = s.MaxTickIntervalUs;
    c->asioState = s.AsioState;
    c->valid = TRUE;
}

static void PrintDriverDelta(HANDLE h, const DriverCounters *base)
{
    DriverCounters now;

    ReadDriver(h, &now);
    if (!now.valid || !base->valid) {
        printf("          driver counters: not available (no apollo.sys interface, or an older driver)\n");
        return;
    }
    printf("          driver: render underruns +%lu, capture overruns +%lu, dropped frames +%lu, late packets +%lu, "
           "notify failures +%lu, engine late ticks +%lu, longest tick gap %lu us\n",
           now.renderUnderruns - base->renderUnderruns, now.captureOverruns - base->captureOverruns,
           now.dropped - base->dropped, now.latePackets - base->latePackets,
           now.notifyFailures - base->notifyFailures, now.lateTicks - base->lateTicks, now.maxTickUs);
}

/* ------------------------------------------------------------------ */
/* Signal                                                               */
/* ------------------------------------------------------------------ */

struct Generator {
    double amp, w;
    ULONGLONG n;
};

static double g_fillPartMs[3], g_fillPartMax[3];   /* GetBuffer, writing the samples, ReleaseBuffer */

static HRESULT Fill(Stream *s, Generator *g, UINT32 frames)
{
    static LARGE_INTEGER qf;            /* fixed at boot: read once */
    BYTE *data = NULL;
    UINT32 f, c;
    LARGE_INTEGER t[4];
    HRESULT hr;

    if (qf.QuadPart == 0)
        QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&t[0]);
    hr = s->render->GetBuffer(frames, &data);
    QueryPerformanceCounter(&t[1]);
    if (FAILED(hr))
        return hr;
    for (f = 0; f < frames; f++) {
        double v = g->amp * sin(g->w * (double)g->n++);
        BYTE *frame = data + (size_t)f * s->fmt.blockAlign;

        for (c = 0; c < s->fmt.channels; c++)
            WriteSample(&s->fmt, frame, c, c < 2 ? v : 0.0);
    }
    QueryPerformanceCounter(&t[2]);
    hr = s->render->ReleaseBuffer(frames, 0);
    QueryPerformanceCounter(&t[3]);
    for (c = 0; c < 3; c++) {
        g_fillPartMs[c] = (double)(t[c + 1].QuadPart - t[c].QuadPart) * 1000.0 / (double)qf.QuadPart;
        if (g_fillPartMs[c] > g_fillPartMax[c]) g_fillPartMax[c] = g_fillPartMs[c];
    }
    return hr;
}

struct Checker {
    double c2, x1, x2;
    ULONGLONG n;
    double worst, sumSq;
    ULONG sumN;
};

enum { ST_WAIT, ST_SETTLE, ST_CHECK, ST_DONE };

struct Analysis {
    Checker ck[2];
    UINT32 channels;
    double rate, threshold;
    int state;
    ULONGLONG frame;            /* capture frames seen */
    ULONGLONG settleEnd;
    ULONGLONG checked, target, lastReport;
    ULONGLONG lastErrFrame;
    BOOL anyErr;
    ULONG events, printed;
    ULONGLONG errSamples;
    LONGLONG lastRenderLateQpc; /* tags glitches near this program's own lateness */
};

static void Analyze(Analysis *a, const Stream *s, const BYTE *data, UINT32 frames, BOOL silent, LONGLONG nowQpc,
                    LONGLONG qpcFreq)
{
    UINT32 f, c;

    for (f = 0; f < frames && a->state != ST_DONE; f++) {
        const BYTE *frame = data + (size_t)f * s->fmt.blockAlign;
        double x[2];

        for (c = 0; c < a->channels; c++)
            x[c] = silent ? 0.0 : ReadSample(&s->fmt, frame, c);
        if (a->state == ST_WAIT) {
            for (c = 0; c < a->channels; c++) {
                if (fabs(x[c]) > DETECT_LEVEL) {
                    a->state = ST_SETTLE;
                    a->settleEnd = a->frame + (ULONGLONG)(SETTLE_SECONDS * a->rate);
                    printf("The sine came back after %.3f s of recording; checking starts in %.1f s.\n",
                           (double)a->frame / a->rate, SETTLE_SECONDS);
                    break;
                }
            }
        } else if (a->state == ST_SETTLE && a->frame >= a->settleEnd) {
            a->state = ST_CHECK;
        }
        for (c = 0; c < a->channels; c++) {
            Checker *k = &a->ck[c];

            if (a->state == ST_CHECK && k->n >= 2) {
                double r = fabs(x[c] - k->c2 * k->x1 + k->x2);

                if (r > k->worst)
                    k->worst = r;
                if (r > a->threshold) {
                    a->errSamples++;
                    if (!a->anyErr || a->frame > a->lastErrFrame + EVENT_GAP) {
                        a->events++;
                        if (a->printed < MAX_PRINTED) {
                            BOOL ours = a->lastRenderLateQpc != 0 &&
                                        nowQpc - a->lastRenderLateQpc < qpcFreq / 2;
                            a->printed++;
                            printf("  GLITCH %s at %.4f s of checked audio: error %.2e (%.1f dBFS)%s\n",
                                   c ? "R" : "L", (double)a->checked / a->rate, r, 20.0 * log10(r),
                                   ours ? "  <- right after this program fed playback late" : "");
                        }
                    }
                    a->lastErrFrame = a->frame;
                    a->anyErr = TRUE;
                }
            }
            if (a->state == ST_CHECK) {
                k->sumSq += x[c] * x[c];
                k->sumN++;
            }
            k->x2 = k->x1;
            k->x1 = x[c];
            k->n++;
        }
        if (a->state == ST_CHECK && ++a->checked >= a->target)
            a->state = ST_DONE;
        a->frame++;
    }
}

static double LevelDb(const Checker *k)
{
    double rms = k->sumN ? sqrt(k->sumSq / k->sumN) : 0.0;

    return rms > 0.0 ? 20.0 * log10(rms * sqrt(2.0)) : -144.0;
}

/* ------------------------------------------------------------------ */
/* --meter: peak level of one recording endpoint                        */
/* ------------------------------------------------------------------ */

#define METER_MAX_CH        8
#define METER_PERIOD_MS     250
#define METER_BAR           30          /* characters for -60..0 dBFS */

/* The active recording endpoint whose name contains Match; NULL (candidates listed) unless exactly one. */
static IMMDevice *FindOneCapture(IMMDeviceEnumerator *en, const WCHAR *match, WCHAR *name, size_t cch)
{
    IMMDeviceCollection *col = NULL;
    IMMDevice *found = NULL;
    UINT n = 0, i, hits = 0;

    name[0] = 0;
    if (FAILED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col)) || FAILED(col->GetCount(&n))) {
        SafeRelease(&col);
        fprintf(stderr, "Could not list the recording endpoints.\n");
        return NULL;
    }
    for (i = 0; i < n; i++) {
        IMMDevice *d = NULL;
        WCHAR nm[256];

        if (FAILED(col->Item(i, &d)))
            continue;
        GetName(d, nm, ARRAYSIZE(nm));
        if (ContainsI(nm, match)) {
            if (++hits == 1) {
                found = d;
                d = NULL;
                wcsncpy_s(name, cch, nm, _TRUNCATE);
            } else {
                if (hits == 2)
                    fprintf(stderr, "\"%ls\" matches several recording endpoints:\n  %ls\n", match, name);
                fprintf(stderr, "  %ls\n", nm);
            }
        }
        SafeRelease(&d);
    }
    SafeRelease(&col);
    if (hits == 0) {
        fprintf(stderr, "No active recording endpoint whose name contains \"%ls\" (wasapi-loop --list shows "
                        "them all).\n", match);
    } else if (hits > 1) {
        fprintf(stderr, "Give more of the name, for example --meter \"MIC/LINE 3\".\n");
        SafeRelease(&found);
    }
    return found;
}

static void PrintPeak(double peak)
{
    char bar[METER_BAR + 1];
    int i, fill = 0;

    if (peak <= 0.0) {
        printf("   -inf dBFS [%*s] digital zero", METER_BAR, "");
        return;
    }
    {
        double db = 20.0 * log10(peak);

        fill = (int)((db + 60.0) / 60.0 * METER_BAR + 0.5);
        if (fill < 0)
            fill = 0;
        if (fill > METER_BAR)
            fill = METER_BAR;
        for (i = 0; i < METER_BAR; i++)
            bar[i] = (i < fill) ? '#' : ' ';
        bar[METER_BAR] = 0;
        printf(" %6.1f dBFS [%s]%s", db, bar, peak >= 1.0 ? " CLIP" : "");
    }
}

static int RunMeter(IMMDeviceEnumerator *en, const WCHAR *match, double seconds, BOOL exclusive, BOOL raw,
                    HANDLE apollo, const DriverCounters *base)
{
    IMMDevice *dev;
    WCHAR name[256];
    Stream cs;
    double peak[METER_MAX_CH], top[METER_MAX_CH];
    ULONG discont = 0, silentPackets = 0, timeouts = 0;
    ULONGLONG packets = 0, frames = 0;
    UINT32 nch, c;
    LARGE_INTEGER qf, q0, now;
    LONGLONG period, next;
    BOOL lost = FALSE, anySignal = FALSE;
    HRESULT hr;
    int rc = 2;

    ZeroMemory(&cs, sizeof(cs));
    ZeroMemory(peak, sizeof(peak));
    ZeroMemory(top, sizeof(top));
    dev = FindOneCapture(en, match, name, ARRAYSIZE(name));
    if (dev == NULL)
        return 2;
    if (FAILED(OpenStream(dev, FALSE, exclusive, raw, &cs)))
        goto out;
    nch = cs.fmt.channels < METER_MAX_CH ? cs.fmt.channels : METER_MAX_CH;
    printf("Metering %ls: %s %s, %u ch, %u Hz%s. Peak every %u ms, %s. Ctrl+C stops.\n", name,
           exclusive ? "exclusive" : "shared", KindName(&cs.fmt), cs.fmt.channels, cs.fmt.rate,
           raw ? ", raw" : "", (unsigned)METER_PERIOD_MS, seconds > 0.0 ? "for the given time" : "until Ctrl+C");
    if (cs.fmt.channels > METER_MAX_CH)
        printf("(showing the first %u channels)\n", (unsigned)METER_MAX_CH);

    hr = cs.client->Start();
    if (FAILED(hr)) {
        Fail("IAudioClient::Start (recording)", hr);
        goto out;
    }
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&q0);
    period = qf.QuadPart * METER_PERIOD_MS / 1000;
    next = q0.QuadPart + period;

    while (!g_stop) {
        DWORD w = WaitForSingleObject(cs.event, 100);

        if (w == WAIT_TIMEOUT) {
            if (++timeouts >= 20) {
                printf("No WASAPI events for 2 s: the endpoint stopped streaming.\n");
                lost = TRUE;
                break;
            }
        } else {
            timeouts = 0;
            for (;;) {
                UINT32 size = 0, got = 0, f;
                BYTE *data = NULL;
                DWORD flags = 0;

                hr = cs.capture->GetNextPacketSize(&size);
                if (FAILED(hr)) {
                    Fail("IAudioCaptureClient::GetNextPacketSize", hr);
                    lost = TRUE;
                    break;
                }
                if (size == 0)
                    break;
                hr = cs.capture->GetBuffer(&data, &got, &flags, NULL, NULL);
                if (hr == AUDCLNT_S_BUFFER_EMPTY)
                    break;
                if (FAILED(hr)) {
                    Fail("IAudioCaptureClient::GetBuffer", hr);
                    lost = TRUE;
                    break;
                }
                if (packets > 0 && (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY))
                    discont++;
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    silentPackets++;
                } else {
                    for (f = 0; f < got; f++) {
                        const BYTE *frame = data + (size_t)f * cs.fmt.blockAlign;

                        for (c = 0; c < nch; c++) {
                            double v = fabs(ReadSample(&cs.fmt, frame, c));

                            if (v > peak[c])
                                peak[c] = v;
                        }
                    }
                }
                packets++;
                frames += got;
                hr = cs.capture->ReleaseBuffer(got);
                if (FAILED(hr)) {
                    Fail("IAudioCaptureClient::ReleaseBuffer", hr);
                    lost = TRUE;
                    break;
                }
            }
            if (lost)
                break;
        }
        QueryPerformanceCounter(&now);
        if (now.QuadPart >= next) {
            double t = (double)(now.QuadPart - q0.QuadPart) / (double)qf.QuadPart;

            printf("%8.2f s", t);
            for (c = 0; c < nch; c++) {
                if (nch > 1)
                    printf("  ch%u", c + 1);
                PrintPeak(peak[c]);
                if (peak[c] > top[c])
                    top[c] = peak[c];
                if (peak[c] > 0.0)
                    anySignal = TRUE;
                peak[c] = 0.0;
            }
            printf("\n");
            next += period;
            if (next <= now.QuadPart)
                next = now.QuadPart + period;   /* the console stalled: no burst of catch-up lines */
            if (seconds > 0.0 && t >= seconds)
                break;
        }
    }
    cs.client->Stop();

    printf("\n%llu packets, %llu frames (%.1f s of audio). Loudest peak:", packets, frames,
           cs.fmt.rate ? (double)frames / cs.fmt.rate : 0.0);
    for (c = 0; c < nch; c++) {
        if (nch > 1)
            printf("  ch%u", c + 1);
        PrintPeak(top[c]);
    }
    printf("\nWindows discontinuities %lu, silent packets %lu.\n", discont, silentPackets);
    PrintDriverDelta(apollo, base);
    if (lost)
        rc = 2;
    else if (!anySignal) {
        printf("VERDICT: NO SIGNAL - every sample was digital zero. On a MIC/LINE input that usually means its "
               "input strip is not built yet (or nothing is plugged in and the preamp is fully down).\n");
        rc = 3;
    } else {
        printf("VERDICT: SIGNAL - %ls delivered audio.\n", name);
        rc = 0;
    }
out:
    CloseStream(&cs);
    SafeRelease(&dev);
    return rc;
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

static void Usage(void)
{
    printf("usage: wasapi-loop [seconds=1800] [--threshold 1e-4] [--exclusive] [--raw]\n"
           "                   [--render TEXT] [--capture TEXT]\n"
           "       wasapi-loop --tone [seconds=10] [--render TEXT]\n"
           "       wasapi-loop --meter TEXT [seconds=0] [--exclusive] [--raw]\n"
           "       wasapi-loop --list\n"
           "Plays a 997 Hz sine at -12 dBFS into x8p MON L/R (Apollo) and checks every sample that\n"
           "comes back on the Monitor mix (Apollo) recording endpoint (the monitor-mix loopback).\n"
           "--meter prints the peak level of the one recording endpoint whose name contains TEXT\n"
           "(e.g. \"MIC/LINE 3\") every 250 ms; 0 seconds = until Ctrl+C.\n");
}

int wmain(int argc, wchar_t **argv)
{
    double seconds = -1.0, threshold = 1e-4;
    BOOL tone = FALSE, list = FALSE, exclusive = FALSE, raw = FALSE;
    /* The endpoints are "x8p MON L/R (Apollo)" (playback) and "Monitor mix (Apollo)" (its loopback recording). */
    const WCHAR *renderMatch = L"x8p MON L/R", *captureMatch = L"Monitor mix", *meterMatch = NULL;
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *rdev = NULL, *cdev = NULL;
    WCHAR rname[256], cname[256];
    Stream rs, cs;
    Generator gen;
    Analysis an;
    HANDLE apollo;
    DriverCounters base;
    LARGE_INTEGER qf, q0, qn;
    ULONG renderLate = 0, winDiscont = 0, posGaps = 0, eventTimeouts = 0, quietWaits = 0;
    ULONGLONG capturePackets = 0, nextDevPos = 0;
    BOOL haveDevPos = FALSE, lost = FALSE;
    HRESULT hr;
    int rc = 2, i;

    for (i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--tone") == 0)
            tone = TRUE;
        else if (_wcsicmp(argv[i], L"--list") == 0)
            list = TRUE;
        else if (_wcsicmp(argv[i], L"--exclusive") == 0)
            exclusive = TRUE;
        else if (_wcsicmp(argv[i], L"--raw") == 0)
            raw = TRUE;
        else if (_wcsicmp(argv[i], L"--threshold") == 0 && i + 1 < argc)
            threshold = _wtof(argv[++i]);
        else if (_wcsicmp(argv[i], L"--render") == 0 && i + 1 < argc)
            renderMatch = argv[++i];
        else if (_wcsicmp(argv[i], L"--capture") == 0 && i + 1 < argc)
            captureMatch = argv[++i];
        else if (_wcsicmp(argv[i], L"--meter") == 0 && i + 1 < argc)
            meterMatch = argv[++i];
        else if (argv[i][0] != L'-' && _wtof(argv[i]) > 0.0)
            seconds = _wtof(argv[i]);
        else {
            Usage();
            return 2;
        }
    }
    if (seconds < 0.0)
        seconds = meterMatch ? 0.0 : tone ? 10.0 : 1800.0;
    if (!tone && !meterMatch && seconds < 5.0)
        seconds = 5.0;
    if (threshold <= 0.0)
        threshold = 1e-4;
    SetConsoleCtrlHandler(OnCtrl, TRUE);
    setvbuf(stdout, NULL, _IONBF, 0);   /* progress shows at once, also through Tee-Object */

    /* Selecting text in the console (QuickEdit) blocks every printf and would stall the loop. */
    {
        HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;

        if (GetConsoleMode(hin, &mode))
            SetConsoleMode(hin, (mode & ~ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS);
    }

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        Fail("CoInitializeEx", hr);
        return 2;
    }
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                          (void **)&en);
    if (FAILED(hr)) {
        Fail("MMDeviceEnumerator", hr);
        CoUninitialize();
        return 2;
    }
    if (list) {
        rc = ListEndpoints(en);
        SafeRelease(&en);
        CoUninitialize();
        return rc;
    }
    if (meterMatch != NULL) {
        apollo = OpenApollo();
        ReadDriver(apollo, &base);
        rc = RunMeter(en, meterMatch, seconds, exclusive, raw, apollo, &base);
        if (apollo != INVALID_HANDLE_VALUE)
            CloseHandle(apollo);
        SafeRelease(&en);
        CoUninitialize();
        return rc;
    }

    ZeroMemory(&rs, sizeof(rs));
    ZeroMemory(&cs, sizeof(cs));
    ZeroMemory(&an, sizeof(an));
    apollo = OpenApollo();
    ReadDriver(apollo, &base);
    QueryPerformanceFrequency(&qf);

    rdev = FindEndpoint(en, eRender, renderMatch, rname, ARRAYSIZE(rname));
    if (rdev == NULL)
        goto done;
    if (!tone) {
        cdev = FindEndpoint(en, eCapture, captureMatch, cname, ARRAYSIZE(cname));
        if (cdev == NULL)
            goto done;
    }
    {
        WCHAR *defId = DefaultId(en, eRender, eConsole), *id = NULL;

        rdev->GetId(&id);
        if (!tone && id && defId && wcscmp(id, defId) == 0)
            printf("note: %ls is the default output, so Windows sounds play into the loop and count as "
                   "glitches. Silence notifications for the run, or make your usual speakers the default.\n",
                   rname);
        CoTaskMemFree(id);
        CoTaskMemFree(defId);
    }
    if (!tone)
        OtherActiveSessions(rdev);
    if (!tone && base.valid && base.asioState == APOLLO_ASIO_STATE_RUNNING)
        printf("warning: an ASIO host is streaming; its MON L/R output adds into the loop.\n");

    if (FAILED(OpenStream(rdev, TRUE, exclusive, raw, &rs)))
        goto done;
    if (!tone && FAILED(OpenStream(cdev, FALSE, exclusive, raw, &cs)))
        goto done;

    gen.amp = pow(10.0, LEVEL_DB / 20.0);
    gen.w = 2.0 * PI * FREQ / (double)rs.fmt.rate;
    gen.n = 0;
    printf("Playback:  %ls: %s %s, %u ch, %u Hz, buffer %u frames%s\n", rname,
           exclusive ? "exclusive" : "shared", KindName(&rs.fmt), rs.fmt.channels, rs.fmt.rate, rs.bufferFrames,
           raw ? ", raw" : "");
    if (!tone) {
        an.channels = cs.fmt.channels < 2 ? cs.fmt.channels : 2;
        an.rate = (double)cs.fmt.rate;
        an.threshold = threshold;
        an.target = (ULONGLONG)(seconds * an.rate);
        an.ck[0].c2 = an.ck[1].c2 = 2.0 * cos(2.0 * PI * FREQ / an.rate);
        an.state = ST_WAIT;
        printf("Recording: %ls: %s %s, %u ch, %u Hz, buffer %u frames%s\n", cname,
               exclusive ? "exclusive" : "shared", KindName(&cs.fmt), cs.fmt.channels, cs.fmt.rate,
               cs.bufferFrames, raw ? ", raw" : "");
        if (cs.fmt.rate != rs.fmt.rate)
            printf("warning: playback and recording run at different rates; the check assumes one clock.\n");
        printf("Checking %.0f s of the loop after the sine settles (threshold %.0e of full scale). "
               "Ctrl+C stops early.\n", seconds, threshold);
    } else {
        printf("Playing %.0f Hz at %.0f dBFS for %.0f s. Ctrl+C stops early.\n", FREQ, LEVEL_DB, seconds);
    }

    {
        DWORD task = 0;
        HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);

        if (mm != NULL)
            AvSetMmThreadPriority(mm, AVRT_PRIORITY_HIGH);
        else
            printf("(could not join MMCSS Pro Audio: %lu)\n", GetLastError());
    }

    hr = Fill(&rs, &gen, rs.bufferFrames);          /* prime the whole buffer before start */
    if (FAILED(hr)) {
        Fail("first playback buffer", hr);
        goto done;
    }
    if (!tone) {
        hr = cs.client->Start();
        if (FAILED(hr)) {
            Fail("IAudioClient::Start (recording)", hr);
            goto done;
        }
    }
    hr = rs.client->Start();
    if (FAILED(hr)) {
        Fail("IAudioClient::Start (playback)", hr);
        goto done;
    }
    QueryPerformanceCounter(&q0);

    while (!g_stop) {
        HANDLE ev[2] = { rs.event, cs.event };
        DWORD w = WaitForMultipleObjects(tone ? 1 : 2, ev, FALSE, 200);
        BOOL renderDue = (w == WAIT_OBJECT_0), captureDue = (w == WAIT_OBJECT_0 + 1);
        LARGE_INTEGER now;
        double wall;

        QueryPerformanceCounter(&now);
        wall = (double)(now.QuadPart - q0.QuadPart) / (double)qf.QuadPart;
        if (w == WAIT_TIMEOUT) {
            if (++eventTimeouts % 10 == 0)
                printf("  no WASAPI events for %lu00 ms: the endpoint stopped streaming\n", eventTimeouts);
            if (eventTimeouts >= 50) {
                lost = TRUE;
                break;
            }
            continue;
        }
        eventTimeouts = 0;
        /* An auto-reset event the wait did not report may still be set: check both. */
        if (!renderDue && WaitForSingleObject(rs.event, 0) == WAIT_OBJECT_0)
            renderDue = TRUE;
        if (!tone && !captureDue && WaitForSingleObject(cs.event, 0) == WAIT_OBJECT_0)
            captureDue = TRUE;

        if (renderDue) {
            UINT32 avail = rs.bufferFrames;

            if (!rs.exclusive) {
                UINT32 pad = 0;

                hr = rs.client->GetCurrentPadding(&pad);
                if (FAILED(hr)) {
                    Fail("IAudioClient::GetCurrentPadding", hr);
                    lost = TRUE;
                    break;
                }
                if (pad == 0) {
                    renderLate++;
                    an.lastRenderLateQpc = now.QuadPart;
                    printf("  playback buffer ran dry at %.3f s (this program was late, not the driver)\n", wall);
                }
                avail = rs.bufferFrames - pad;
            }
            if (avail != 0) {
                LARGE_INTEGER f0, f1;
                double gapMs, fillMs;

                QueryPerformanceCounter(&f0);
                hr = Fill(&rs, &gen, avail);
                QueryPerformanceCounter(&f1);
                if (FAILED(hr)) {
                    Fail("playback buffer", hr);
                    lost = TRUE;
                    break;
                }
                /* Render timing: wake-to-wake gap and how long GetBuffer..ReleaseBuffer took. */
                gapMs = g_lastRenderWake ? (double)(now.QuadPart - g_lastRenderWake) * 1000.0 / (double)qf.QuadPart : 0.0;
                fillMs = (double)(f1.QuadPart - f0.QuadPart) * 1000.0 / (double)qf.QuadPart;
                g_lastRenderWake = now.QuadPart;
                g_renderWakes++;
                if (gapMs > g_maxGapMs) g_maxGapMs = gapMs;
                if (fillMs > g_maxFillMs) g_maxFillMs = fillMs;
                if (gapMs > 13.0 || fillMs > 2.0) {
                    g_slowRender++;
                    printf("  render timing at %.4f s: woke %.2f ms after the previous wake, fill %.2f ms "
                           "(GetBuffer %.2f, samples %.2f, ReleaseBuffer %.2f)\n",
                           wall, gapMs, fillMs, g_fillPartMs[0], g_fillPartMs[1], g_fillPartMs[2]);
                }
            }
        }

        if (captureDue) {
            for (;;) {
                UINT32 packet = 0, frames = 0;
                BYTE *data = NULL;
                DWORD flags = 0;
                UINT64 devPos = 0, qpcPos = 0;

                hr = cs.capture->GetNextPacketSize(&packet);
                if (FAILED(hr)) {
                    Fail("IAudioCaptureClient::GetNextPacketSize", hr);
                    lost = TRUE;
                    break;
                }
                if (packet == 0)
                    break;
                hr = cs.capture->GetBuffer(&data, &frames, &flags, &devPos, &qpcPos);
                if (hr == AUDCLNT_S_BUFFER_EMPTY)
                    break;
                if (FAILED(hr)) {
                    Fail("IAudioCaptureClient::GetBuffer", hr);
                    lost = TRUE;
                    break;
                }
                if (capturePackets > 0 && (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)) {
                    winDiscont++;
                    an.ck[0].n = an.ck[1].n = 0;    /* counted here; restart the recurrence */
                    printf("  Windows flagged a recording discontinuity at %.3f s (device frame %llu)\n", wall,
                           devPos);
                } else if (haveDevPos && devPos != nextDevPos) {
                    posGaps++;
                    an.ck[0].n = an.ck[1].n = 0;
                    printf("  recording position jumped from %llu to %llu (%lld frames) at %.3f s\n", nextDevPos,
                           devPos, (LONGLONG)(devPos - nextDevPos), wall);
                }
                nextDevPos = devPos + frames;
                haveDevPos = TRUE;
                capturePackets++;
                Analyze(&an, &cs, data, frames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0, now.QuadPart,
                        qf.QuadPart);
                hr = cs.capture->ReleaseBuffer(frames);
                if (FAILED(hr)) {
                    Fail("IAudioCaptureClient::ReleaseBuffer", hr);
                    lost = TRUE;
                    break;
                }
            }
            if (lost)
                break;
        }

        if (tone) {
            if (wall >= seconds)
                break;
            continue;
        }
        if (an.state == ST_WAIT && wall > 10.0 + quietWaits * 10.0) {
            quietWaits++;
            printf("  %.0f s and the sine has not come back on %ls yet. Is MON L/R in the monitor mix and the "
                   "monitor level up?\n", wall, cname);
            if (quietWaits >= 3)
                break;
        }
        if (an.state == ST_CHECK && an.checked >= an.lastReport + REPORT_FRAMES) {
            printf("%7.0f s | level L %.2f R %.2f dBFS | worst error %.1e / %.1e | glitches %lu | Windows "
                   "discontinuities %lu | position gaps %lu | playback fed late %lu\n",
                   (double)an.checked / an.rate, LevelDb(&an.ck[0]), LevelDb(&an.ck[1]), an.ck[0].worst,
                   an.ck[1].worst, an.events, winDiscont, posGaps, renderLate);
            PrintDriverDelta(apollo, &base);
            an.ck[0].sumSq = an.ck[1].sumSq = 0.0;
            an.ck[0].sumN = an.ck[1].sumN = 0;
            an.lastReport = an.checked;
        }
        if (an.state == ST_DONE)
            break;
    }

    QueryPerformanceCounter(&qn);
    rs.client->Stop();
    if (cs.client != NULL)
        cs.client->Stop();

    printf("Render timing: %lu wakes, longest wake-to-wake %.2f ms, longest GetBuffer..ReleaseBuffer %.2f ms "
           "(longest GetBuffer %.2f, samples %.2f, ReleaseBuffer %.2f), %lu slow (gap > 13 ms or fill > 2 ms).\n",
           g_renderWakes, g_maxGapMs, g_maxFillMs, g_fillPartMax[0], g_fillPartMax[1], g_fillPartMax[2], g_slowRender);
    if (tone) {
        printf("Played %.1f s%s. Playback fed late: %lu.\n", (double)(qn.QuadPart - q0.QuadPart) / qf.QuadPart,
               lost ? " (the endpoint stopped)" : "", renderLate);
        PrintDriverDelta(apollo, &base);
        rc = lost ? 2 : 0;
        goto done;
    }

    {
        ULONG glitches = an.events + winDiscont + posGaps;

        printf("\nChecked %.1f s of the loop (%.1f s wall). Glitches: %lu (sample check %lu, Windows "
               "discontinuities %lu, position gaps %lu). Worst error L %.1e, R %.1e. Playback fed late: %lu.\n",
               (double)an.checked / (an.rate > 0 ? an.rate : 48000.0),
               (double)(qn.QuadPart - q0.QuadPart) / qf.QuadPart, glitches, an.events, winDiscont, posGaps,
               an.ck[0].worst, an.ck[1].worst, renderLate);
        PrintDriverDelta(apollo, &base);
        if (lost)
            printf("The endpoint stopped streaming during the run.\n");
        if (an.state == ST_WAIT) {
            printf("VERDICT: NO SIGNAL - the sine never came back on %ls.\n", cname);
            rc = 3;
        } else if (glitches == 0 && !lost) {
            printf("VERDICT: CLEAN - MON L/R out -> x8p monitor mix -> MON L/R in did not glitch once%s.\n",
                   an.state == ST_DONE ? "" : " (stopped early)");
            rc = 0;
        } else {
            printf("VERDICT: %lu glitch(es)%s. Ones marked as late playback feeding are this program's; the "
                   "driver counters above show which side lost data.\n", glitches,
                   lost ? " and the stream stopped" : "");
            rc = 1;
        }
    }

done:
    CloseStream(&rs);
    CloseStream(&cs);
    SafeRelease(&rdev);
    SafeRelease(&cdev);
    SafeRelease(&en);
    if (apollo != INVALID_HANDLE_VALUE)
        CloseHandle(apollo);
    CoUninitialize();
    return rc;
}
