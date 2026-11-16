/*
 * asio-test.cpp - loads avium-asio.dll the way an ASIO host does (no registration needed)
 * and exercises it.
 *
 *   asio-test                  init, channels and names, sizes, latencies, rate, clock,
 *                              then a createBuffers attempt
 *   asio-test --run 10 [128]   also start, play a -20 dBFS 1 kHz tone on MON L/R, count
 *                              buffer switches and report the rate and input peaks
 *     [--amp <dBFS>]           tone level (default -20)
 *     [--wav <file>]           write 2 s of the MON L input (32-bit mono) after a 0.5 s settle
 *     [--in-only]              inputs only (MIC/LINE 1, MON L), no output buffers
 *     [--outs a,b]             the two output channels, 0-based (default 0,1 = MON L/R)
 *     [--dc <hex>]             a constant sample (e.g. 10000000 = +0.125 FS) instead of the tone
 *   The MON L input (record slot 26, the unit's monitor bus) is also measured with a
 *   1 kHz lock-in: tone level, residual (everything that is not the tone) and DC.
 *   With the outputs on MON L/R it must come back at the played level (PASS), else
 *   exit code 3. The monitor bus is unity with the unit's inputs silent.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <windows.h>
#include <ole2.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cfgmgr32.h>
#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"
#include "..\include\apollo_asio.h"
#include "..\include\apollo_agg.h"     /* [G6] the combined device's counters */
#include "..\include\apollo_select.h"  /* [Twin X G2] the x8p among several units */

static CLSID CLSID_AviumApolloAsio = { 0xedb055a9, 0x283d, 0x4679, { 0x97, 0xe2, 0x77, 0x13, 0x92, 0x8b, 0xd0, 0x1b } };
/* [G6] "Apollo (Avium)", the combined device */
static CLSID CLSID_AviumApolloAggAsio = { 0xb6a1f3c2, 0x5d4e, 0x4a7b, { 0x9c, 0x21, 0x3e, 0x8f, 0x0a, 0x6d, 0x7c, 0x55 } };

/*
 * [G6] asio-test --agg <seconds> [frames] [--amp dB] [--also Hz]: the combined driver. Plays the tone into the
 * outputs named "TwinX MON L/R" and "x8p MON L/R" (set the routes to none first, or the DAW pair is
 * routed), records "TwinX MON L" and "x8p MON L" (each unit's monitor-bus loopback), then per second
 * the 1 kHz level and phase of both. With the clock loop working the phase between the two units
 * stays constant (no drift; 4.74 ppm would move it 1.7 degrees a second at 1 kHz).
 */
#define AGG_CAP_MAX     (48000 * 62)
static bool g_agg;
/*
 * [G7] --also <Hz>: with --agg, also measure a second tone on both loopbacks, e.g. 997 Hz from
 * "wasapi-loop --tone" playing into the combined Windows "Monitor" endpoint at the same time: Windows
 * audio and the DAW must sum on the routed outputs (spec 6.8 Studio: on the Twin X MON only). A 1 s
 * lock-in at an integer frequency 3 Hz away does not see the other tone, so both levels are exact.
 */
static double g_also;
static long g_aggIn[2] = { -1, -1 }, g_aggOut[4] = { -1, -1, -1, -1 };
static LONG *g_aggCap[2];
static long g_aggCapN;
static long g_aggSettle = 48000;
/*
 * [Rate] --rate <Hz>: setSampleRate(Hz) first (timed, the driver's answer printed), then everything
 * below runs at the rate the driver reports (getSampleRate): the tone, the analysis, the WAV.
 */
static long g_rate = 48000;
static long g_wantRate;
static bool g_skipSet;

static IASIO *g_asio;
static ASIOBufferInfo g_bufs[6];
static long g_frames;
static volatile LONG g_switches;
static double g_phase;
static LONG g_inPeak[2];
static double g_amp = 0.1;                      /* tone amplitude, of full scale */
static long CAP_SKIP = 24000;                   /* 0.5 s settle; --skip <frames> */
#define CAP_MAX     96000                       /* 2 s */
static LONG g_cap[CAP_MAX];                     /* MON L input */
static long g_capSeen, g_capN;
static bool g_inOnly;                           /* --in-only: no output buffers at all */
static long g_outCh[2] = { 0, 1 };              /* --outs a,b: output channels (default MON L/R) */
static bool g_verdictFail;                      /* MON loopback did not match what was played: exit 3 */
static bool g_dc;                               /* --dc <hex>: a constant sample instead of the tone */
static LONG g_dcValue;

/* The 1 kHz tone (or with dc, --dc's constant) into the outputs g_bufs[2..last-1]: the same samples in each. */
static void PlayTone(long index, int last, bool dc)
{
    const double step = 2.0 * 3.14159265358979 * 1000.0 / (double)g_rate;
    double ph = g_phase;
    for (long i = 0; last > 2 && i < g_frames; i++) {
        LONG v = dc ? g_dcValue : (LONG)(g_amp * sin(ph) * 2147483647.0);
        for (int c = 2; c < last; c++) ((LONG *)g_bufs[c].buffers[index])[i] = v;
        ph += step;
    }
    g_phase = fmod(g_phase + step * g_frames, 2.0 * 3.14159265358979);
}

/* [G6] buffers: 0, 1 = inputs TwinX MON L, x8p MON L; 2-5 = outputs */
static void FillAgg(long index)
{
    PlayTone(index, 6, false);
    for (long i = 0; i < g_frames; i++) {
        if (g_capSeen >= g_aggSettle && g_aggCapN < AGG_CAP_MAX) {
            g_aggCap[0][g_aggCapN] = ((LONG *)g_bufs[0].buffers[index])[i];
            g_aggCap[1][g_aggCapN] = ((LONG *)g_bufs[1].buffers[index])[i];
            g_aggCapN++;
        }
        g_capSeen++;
    }
    InterlockedIncrement(&g_switches);
}

/* [G6] Per second: level and phase of 1 kHz on both loopbacks; the phase difference over time. */
static void AnalyseAgg(void)
{
    const double w = 2.0 * 3.14159265358979 * 1000.0 / (double)g_rate, fs = 2147483648.0;
    const long R = g_rate;
    const double fpp = (double)g_rate / 1000.0;     /* frames per 1 kHz period */
    long blocks = g_aggCapN / R;
    double dmin = 1e9, dmax = -1e9, first = 0, lvl[2] = { 0, 0 }, resid[2] = { 0, 0 };
    printf("combined loopback analysis: %ld s captured after a %.1f s settle at %ld Hz\n", blocks,
           g_aggSettle / (double)g_rate, g_rate);
    for (long b = 0; b < blocks; b++) {
        double amp[2], ph[2];
        for (int c = 0; c < 2; c++) {
            double si = 0, co = 0, e2 = 0, ps, pc;
            const LONG *x = g_aggCap[c] + b * R;
            for (long i = 0; i < R; i++) {
                double v = x[i] / fs;
                long n = b * R + i;
                si += v * sin(w * n);
                co += v * cos(w * n);
            }
            ps = 2 * si / R;
            pc = 2 * co / R;
            amp[c] = sqrt(ps * ps + pc * pc);
            ph[c] = atan2(pc, ps) * 180.0 / 3.14159265358979;
            for (long i = 0; i < R; i++) {
                long n = b * R + i;
                double e = x[i] / fs - ps * sin(w * n) - pc * cos(w * n);
                e2 += e * e;
            }
            lvl[c] += amp[c];
            resid[c] += e2 / R;
        }
        double d = ph[1] - ph[0];
        while (d > 180) d -= 360;
        while (d < -180) d += 360;
        if (b == 0) first = d;
        double rel = d - first;
        while (rel > 180) rel -= 360;
        while (rel < -180) rel += 360;
        if (rel < dmin) dmin = rel;
        if (rel > dmax) dmax = rel;
        printf("  t=%3ld s  TwinX MON L %7.2f dBFS  x8p MON L %7.2f dBFS  phase x8p-TwinX %+8.3f deg (%+.3f frames)\n", b,
               amp[0] > 0 ? 20 * log10(amp[0]) : -999.0, amp[1] > 0 ? 20 * log10(amp[1]) : -999.0, d, d / 360.0 * fpp);
    }
    if (blocks > 0) {
        for (int c = 0; c < 2; c++) {
            double a = lvl[c] / blocks, r = sqrt(resid[c] / blocks);
            printf("%s MON L: tone %.2f dBFS (played %.2f), residual %.1f dB below the tone\n", c ? "x8p  " : "TwinX",
                   a > 0 ? 20 * log10(a) : -999.0, 20 * log10(g_amp), (a > 0 && r > 0) ? 20 * log10(a / (r * 1.41421356)) : 0.0);
        }
        printf("phase between the units over %ld s: moved %.3f deg peak to peak = %.4f frames (uncorrected 4.74 ppm "
               "would move %.1f deg)\n", blocks, dmax - dmin, (dmax - dmin) / 360.0 * fpp,
               4.74e-6 * 1000.0 * 360.0 * blocks);
        if (g_also > 0) {
            /* [G7] the second tone: average 1 s lock-in level per loopback (step-g7.ps1 parses this line) */
            double wa = 2.0 * 3.14159265358979 * g_also / (double)g_rate, al[2] = { 0, 0 };
            for (long b = 0; b < blocks; b++) {
                for (int c = 0; c < 2; c++) {
                    double si = 0, co = 0;
                    const LONG *x = g_aggCap[c] + b * R;
                    for (long i = 0; i < R; i++) {
                        double v = x[i] / fs;
                        long n = b * R + i;
                        si += v * sin(wa * n);
                        co += v * cos(wa * n);
                    }
                    al[c] += sqrt((2 * si / R) * (2 * si / R) + (2 * co / R) * (2 * co / R));
                }
            }
            printf("other tone %.0f Hz: TwinX MON L %.2f dBFS, x8p MON L %.2f dBFS\n", g_also,
                   al[0] > 0 ? 20 * log10(al[0] / blocks) : -999.0, al[1] > 0 ? 20 * log10(al[1] / blocks) : -999.0);
        }
        if (dmax - dmin > 3.6 || lvl[0] / blocks < 0.5 * g_amp || lvl[1] / blocks < 0.5 * g_amp) g_verdictFail = true;
        printf("combined loopback: %s\n", g_verdictFail ? "FAIL" : "PASS");
    }
}

static void Fill(long index)
{
    if (g_agg) {
        FillAgg(index);
        return;
    }
    /* outputs: g_bufs[2], g_bufs[3] (MON L/R); inputs: g_bufs[0], g_bufs[1] */
    PlayTone(index, g_inOnly ? 2 : 4, g_dc);
    for (int c = 0; c < 2; c++) {
        LONG *in = (LONG *)g_bufs[c].buffers[index];
        for (long i = 0; i < g_frames; i++) {
            LONG a = in[i] < 0 ? -(in[i] / 256) : in[i] / 256;
            if (a > g_inPeak[c]) g_inPeak[c] = a;
            if (c == 1) {
                if (g_capSeen >= CAP_SKIP && g_capN < CAP_MAX) g_cap[g_capN++] = in[i];
                g_capSeen++;
            }
        }
    }
    InterlockedIncrement(&g_switches);
}

/* 1 kHz lock-in on the captured MON L samples: tone level, residual, DC, clipped samples. */
static void Analyse(const char *wav)
{
    const double fs = 2147483648.0, w = 2.0 * 3.14159265358979 * 1000.0 / (double)g_rate;
    double sum = 0, si = 0, co = 0, res = 0;
    long clipped = 0, n = g_capN;

    if (n < 4800) {
        printf("MON L analysis: only %ld samples captured\n", n);
        return;
    }
    {
        /* Runs of equal values from the start of the capture: when does MON change? */
        long start = 0;
        int shown = 0;
        for (long i = 1; i <= n && shown < 8; i++) {
            if (i == n || g_cap[i] != g_cap[start]) {
                printf("  MON L samples %ld..%ld (%.2f ms from capture start): %08lx%s\n", start + CAP_SKIP,
                       i - 1 + CAP_SKIP, (start + CAP_SKIP) / (g_rate / 1000.0), (unsigned long)g_cap[start],
                       i - start > 1 ? "" : " (single)");
                shown++;
                start = i;
            }
        }
    }
    n -= n % ((g_rate % 1000) == 0 ? g_rate / 1000 : g_rate / 100);   /* whole 1 kHz periods (10 at 44.1k) */
    for (long i = 0; i < n; i++) {
        double x = g_cap[i] / fs;
        sum += x;
        si += x * sin(w * i);
        co += x * cos(w * i);
        if (g_cap[i] >= 0x7FFFFF00 || g_cap[i] <= (LONG)0x80000100) clipped++;
    }
    double dc = sum / n, a = 2.0 * sqrt(si * si + co * co) / n;
    double ps = 2.0 * si / n, pc = 2.0 * co / n;
    for (long i = 0; i < n; i++) {
        double e = g_cap[i] / fs - dc - ps * sin(w * i) - pc * cos(w * i);
        res += e * e;
    }
    res = sqrt(res / n);
    printf("MON L analysis (%ld samples): tone %.2f dBFS (played %.2f), residual %.1f dBFS rms, DC %.4e, "
           "clipped samples %ld, first samples %08lx %08lx %08lx %08lx\n", n, a > 0 ? 20 * log10(a) : -999.0,
           20 * log10(g_amp), res > 0 ? 20 * log10(res) : -999.0, dc, clipped,
           (unsigned long)g_cap[0], (unsigned long)g_cap[1], (unsigned long)g_cap[2], (unsigned long)g_cap[3]);
    if (!g_inOnly && g_outCh[0] == 0) {
        /* MON L out -> monitor bus -> record slot 26: unity with the unit's inputs silent. */
        bool ok;
        if (g_dc) {
            ok = fabs(dc - g_dcValue / fs) < 1e-3;
            printf("MON loopback: %s (DC %.5f, played %.5f)\n", ok ? "PASS" : "FAIL", dc, g_dcValue / fs);
        } else {
            double got = a > 0 ? 20 * log10(a) : -999.0, want = 20 * log10(g_amp);
            ok = fabs(got - want) < 0.5 && res < a / 100.0;
            printf("MON loopback: %s (tone %.2f dBFS, played %.2f, residual %.1f dB below the tone)\n",
                   ok ? "PASS" : "FAIL", got, want, (a > 0 && res > 0) ? 20 * log10(a / res) : 0.0);
        }
        if (!ok) g_verdictFail = true;
    }
    if (wav) {
        FILE *f = nullptr;
        if (fopen_s(&f, wav, "wb") != 0 || !f) { printf("cannot write %s\n", wav); return; }
        unsigned long bytes = (unsigned long)n * 4;
        unsigned long hdr[11] = { 0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16,
                                  0x00010001, (unsigned long)g_rate, (unsigned long)g_rate * 4, 0x00200004,
                                  0x61746164, bytes };
        fwrite(hdr, 4, 11, f);
        fwrite(g_cap, 4, (size_t)n, f);
        fclose(f);
        printf("wrote %s\n", wav);
    }
}

static void bufferSwitch(long index, ASIOBool) { Fill(index); }
static ASIOTime *bufferSwitchTimeInfo(ASIOTime *t, long index, ASIOBool) { Fill(index); return t; }
static void sampleRateDidChange(ASIOSampleRate) {}
static long asioMessage(long selector, long value, void *, double *)
{
    if (selector == kAsioSelectorSupported) return value == kAsioSupportsTimeInfo || value == kAsioEngineVersion;
    if (selector == kAsioSupportsTimeInfo) return 1;
    if (selector == kAsioEngineVersion) return 2;
    if (selector == kAsioResetRequest) { printf("  host got kAsioResetRequest\n"); return 1; }
    return 0;
}

/* A read-only handle on the x8p ([Twin X G2] DeviceType 0x22, never another unit), or on the first unit listed. */
static HANDLE OpenUnit(bool x8p)
{
    static APOLLO_UNIT_ENTRY units[APOLLO_SELECT_MAX];
    int n = ApolloListUnits(units, APOLLO_SELECT_MAX, nullptr, 0);
    int pick = n <= 0 ? -1 : x8p ? ApolloPickUnit(units, n, APOLLO_PICK_X8P) : 0;

    if (pick < 0) {
        if (x8p) printf("driver: no Apollo x8p on apollo.sys\n");
        return INVALID_HANDLE_VALUE;
    }
    return CreateFileW(units[pick].Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                       nullptr);
}

/* The driver's view of the ASIO client (GET_ENGINE_STATS): the x8p's counters only. */
static void PrintDriverCounters(void)
{
    ULONG flags = 0;
    APOLLO_ENGINE_STATS s = {};
    DWORD r = 0;
    HANDLE h = OpenUnit(true);
    if (h == INVALID_HANDLE_VALUE) return;
    if (DeviceIoControl(h, IOCTL_APOLLO_GET_ENGINE_STATS, &flags, sizeof(flags), &s, sizeof(s), &r, nullptr) &&
        r >= sizeof(s)) {
        printf("driver: %lu switches, %lu underruns, %lu overruns, %lu resyncs, %lu skipped by the DLL%s\n",
               s.AsioSwitches, s.AsioUnderruns, s.AsioOverruns, s.AsioResyncs, s.AsioClientSkipped,
               s.EngineLost ? " (ENGINE LOST: the device stopped answering)" : "");
    }
    CloseHandle(h);
}

/* [G6] The combined client's counters and the clock loop (AGG_GET_STATE on any unit's handle). */
static void PrintAggCounters(void)
{
    APOLLO_AGG_STATE s = {};
    ULONG flags = 0;
    DWORD r = 0;
    HANDLE h = OpenUnit(false);
    if (h == INVALID_HANDLE_VALUE) return;
    if (DeviceIoControl(h, IOCTL_APOLLO_AGG_GET_STATE, &flags, sizeof(flags), &s, sizeof(s), &r, nullptr) && r >= sizeof(s)) {
        printf("driver (combined): %lu switches, %lu underruns, %lu overruns, %lu resyncs; aggregate work max %lu us\n",
               s.AsioSwitches, s.AsioUnderruns, s.AsioOverruns, s.AsioResyncs, s.AsrcMaxUs);
        for (ULONG u = 0; u < s.UnitCount && u < APOLLO_AGG_MAX_UNITS; u++) {
            const APOLLO_AGG_UNIT_STATE *x = &s.Unit[u];
            printf("  unit %lu %s: clock state %lu, ratio %+.3f ppm, e %+.3f frames (peak %.3f), resyncs %lu, render late %lu, "
                   "capture short %lu\n", u, (x->Flags & APOLLO_AGG_UNIT_TIMELINE) ? "(timeline)" : "(follower)", x->ClockState,
                   x->RatioPpb / 1000.0, x->ErrorMilli / 1000.0, x->ErrorPeakMilli / 1000.0, x->Resyncs, x->RenderLate,
                   x->CaptureShort);
        }
    }
    CloseHandle(h);
}

int main(int argc, char **argv)
{
    int runSecs = 0;
    long frames = 128;
    char text[128] = "";
    const char *wav = nullptr;

    if (argc >= 3 && (strcmp(argv[1], "--run") == 0 || strcmp(argv[1], "--agg") == 0)) {
        g_agg = strcmp(argv[1], "--agg") == 0;   /* [G6] the combined driver */
        runSecs = atoi(argv[2]);
        int i = 3;
        if (argc > i && argv[i][0] != '-') frames = atol(argv[i++]);
        for (; i < argc; i++) {
            if (strcmp(argv[i], "--in-only") == 0) g_inOnly = true;
            else if (i + 1 < argc && strcmp(argv[i], "--skip") == 0) { CAP_SKIP = atol(argv[++i]); g_skipSet = true; }
            else if (i + 1 < argc && strcmp(argv[i], "--rate") == 0) g_wantRate = atol(argv[++i]);   /* [Rate] */
            else if (i + 1 < argc && strcmp(argv[i], "--dc") == 0) {
                g_dc = true;
                g_dcValue = (LONG)strtoul(argv[++i], nullptr, 16);
            }
            else if (i + 1 < argc && strcmp(argv[i], "--outs") == 0) {
                if (sscanf_s(argv[++i], "%ld,%ld", &g_outCh[0], &g_outCh[1]) != 2) g_outCh[0] = 0, g_outCh[1] = 1;
            }
            else if (i + 1 < argc && strcmp(argv[i], "--amp") == 0) g_amp = pow(10.0, atof(argv[++i]) / 20.0);
            else if (i + 1 < argc && strcmp(argv[i], "--wav") == 0) wav = argv[++i];
            else if (i + 1 < argc && strcmp(argv[i], "--also") == 0) g_also = atof(argv[++i]);   /* [G7] */
        }
        if (g_amp > 1.0) g_amp = 1.0;
    }
    CoInitialize(nullptr);
    HMODULE dll = LoadLibraryW(L"avium-asio.dll");
    if (!dll) { printf("cannot load avium-asio.dll (%lu)\n", GetLastError()); return 2; }
    typedef HRESULT(STDAPICALLTYPE * GetClassObjectFn)(REFCLSID, REFIID, void **);
    GetClassObjectFn get = (GetClassObjectFn)GetProcAddress(dll, "DllGetClassObject");
    IClassFactory *cf = nullptr;
    const CLSID &cls = g_agg ? CLSID_AviumApolloAggAsio : CLSID_AviumApolloAsio;
    if (!get || FAILED(get(cls, IID_IClassFactory, (void **)&cf))) { printf("no class factory\n"); return 2; }
    if (FAILED(cf->CreateInstance(nullptr, cls, (void **)&g_asio))) { printf("CreateInstance failed\n"); return 2; }
    cf->Release();

    g_asio->getDriverName(text);
    printf("driver: %s, version %ld\n", text, g_asio->getDriverVersion());
    if (!g_asio->init(GetConsoleWindow())) {
        g_asio->getErrorMessage(text);
        printf("init FAILED: %s\n", text);
        g_asio->Release();
        return 1;
    }
    long nIn = 0, nOut = 0, mn, mx, pref, gran, li, lo;
    if (g_wantRate > 0) {
        /* [Rate] the host's way: setSampleRate before createBuffers */
        LARGE_INTEGER rf, r0, r1;
        ASIOError re;
        QueryPerformanceFrequency(&rf);
        printf("canSampleRate(%ld): %s\n", g_wantRate, g_asio->canSampleRate((double)g_wantRate) == ASE_OK ? "ok" : "refused");
        QueryPerformanceCounter(&r0);
        re = g_asio->setSampleRate((double)g_wantRate);
        QueryPerformanceCounter(&r1);
        g_asio->getErrorMessage(text);
        printf("setSampleRate(%ld): %ld in %.0f ms%s%s\n", g_wantRate, re,
               (double)(r1.QuadPart - r0.QuadPart) * 1000.0 / (double)rf.QuadPart, text[0] ? ": " : "", text);
        if (re != ASE_OK) {
            g_asio->Release();
            return 1;
        }
    }
    {
        ASIOSampleRate cur = 0;
        g_asio->getSampleRate(&cur);
        if (cur > 0) g_rate = (long)cur;
        g_aggSettle = g_rate;
        if (!g_skipSet) CAP_SKIP = g_rate / 2;
    }
    g_asio->getChannels(&nIn, &nOut);
    g_asio->getBufferSize(&mn, &mx, &pref, &gran);
    g_asio->getLatencies(&li, &lo);
    ASIOSampleRate sr = 0;
    g_asio->getSampleRate(&sr);
    printf("channels: %ld in, %ld out | buffer %ld..%ld pref %ld gran %ld | latency %ld in, %ld out | %.0f Hz (44.1k %s)\n",
           nIn, nOut, mn, mx, pref, gran, li, lo, sr, g_asio->canSampleRate(44100.0) == ASE_OK ? "ok" : "refused");
    ASIOClockSource clk[4];
    long nClk = 4;
    g_asio->getClockSources(clk, &nClk);
    printf("clock: %ld source(s):", nClk);
    for (long k = 0; k < nClk; k++) printf(" \"%s\"%s", clk[k].name, clk[k].isCurrentSource ? " (current)" : "");
    printf("\n");
    for (int dir = 1; dir >= 0; dir--) {
        long n = dir ? nIn : nOut;
        printf("%s:", dir ? "inputs " : "outputs");
        for (long c = 0; c < n; c++) {
            ASIOChannelInfo ci = {};
            ci.channel = c;
            ci.isInput = dir;
            g_asio->getChannelInfo(&ci);
            printf(" [%ld %s]", c + 1, ci.name);
            /* a routed channel's name carries where it goes ("x8p MON L > x8p+TwinX"): match the name before it */
            auto is = [&](const char *want) {
                size_t k = strlen(want);
                return strncmp(ci.name, want, k) == 0 && (ci.name[k] == 0 || strncmp(ci.name + k, " > ", 3) == 0);
            };
            if (g_agg && dir == 1 && is("TwinX MON L")) g_aggIn[0] = c;
            if (g_agg && dir == 1 && is("x8p MON L")) g_aggIn[1] = c;
            if (g_agg && dir == 0 && is("TwinX MON L")) g_aggOut[0] = c;
            if (g_agg && dir == 0 && is("TwinX MON R")) g_aggOut[1] = c;
            if (g_agg && dir == 0 && is("x8p MON L")) g_aggOut[2] = c;
            if (g_agg && dir == 0 && is("x8p MON R")) g_aggOut[3] = c;
        }
        printf("\n");
    }

    ASIOCallbacks cb = { bufferSwitch, sampleRateDidChange, asioMessage, bufferSwitchTimeInfo };
    g_bufs[0] = { ASIOTrue, 0, { nullptr, nullptr } };      /* MIC/LINE 1 */
    g_bufs[1] = { ASIOTrue, 26, { nullptr, nullptr } };     /* MON L (loopback of what we play) */
    g_bufs[2] = { ASIOFalse, g_outCh[0], { nullptr, nullptr } };   /* MON L unless --outs */
    g_bufs[3] = { ASIOFalse, g_outCh[1], { nullptr, nullptr } };   /* MON R unless --outs */
    g_frames = frames;
    if (g_agg) {
        /* [G6] both units' MON loopbacks in, both units' MON pairs out */
        for (int k = 0; k < 2; k++) {
            if (g_aggIn[k] < 0) { printf("no \"%s MON L\" input in the combined list\n", k ? "x8p" : "TwinX"); return 2; }
            g_bufs[k] = { ASIOTrue, g_aggIn[k], { nullptr, nullptr } };
        }
        for (int k = 0; k < 4; k++) {
            if (g_aggOut[k] < 0) { printf("an x8p or Twin X MON output is missing from the combined list\n"); return 2; }
            g_bufs[2 + k] = { ASIOFalse, g_aggOut[k], { nullptr, nullptr } };
        }
        g_aggCap[0] = new LONG[AGG_CAP_MAX];
        g_aggCap[1] = new LONG[AGG_CAP_MAX];
        printf("combined test: inputs %ld/%ld (TwinX/x8p MON L), outputs %ld,%ld (TwinX MON) %ld,%ld (x8p MON)\n",
               g_aggIn[0] + 1, g_aggIn[1] + 1, g_aggOut[0] + 1, g_aggOut[1] + 1, g_aggOut[2] + 1, g_aggOut[3] + 1);
    }
    ASIOError e = g_asio->createBuffers(g_bufs, g_agg ? 6 : (g_inOnly ? 2 : 4), frames, &cb);
    if (e != ASE_OK) {
        g_asio->getErrorMessage(text);
        printf("createBuffers(%ld frames): error %ld: %s\n", frames, e, text);
        g_asio->Release();
        return runSecs ? 1 : 0;
    }
    printf("createBuffers(%ld frames): ok\n", frames);
    if (runSecs > 0) {
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        e = g_asio->start();
        printf("start: %ld\n", e);
        QueryPerformanceCounter(&t0);
        Sleep((DWORD)runSecs * 1000);
        QueryPerformanceCounter(&t1);
        g_asio->stop();
        if (g_agg) {
            PrintAggCounters();
        } else {
            PrintDriverCounters();              /* the client is still open: its counters are live */
        }
        double secs = (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
        printf("%ld buffer switches in %.2f s = %.1f/s (expected %.1f/s at %ld frames)\n", g_switches, secs,
               g_switches / secs, (double)g_rate / frames, frames);
        if (g_agg) {
            AnalyseAgg();
        } else {
            for (int c = 0; c < 2; c++) {
                printf("input %s peak: %s%.1f dBFS\n", c ? "MON L" : "MIC/LINE 1", g_inPeak[c] ? "" : "zero ",
                       g_inPeak[c] ? 20.0 * log10(g_inPeak[c] / 8388607.0) : -144.0);
            }
            Analyse(wav);
        }
    }
    g_asio->disposeBuffers();
    g_asio->Release();
    return g_verdictFail ? 3 : 0;
}
