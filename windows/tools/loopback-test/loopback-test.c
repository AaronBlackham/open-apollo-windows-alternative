/*
 * loopback-test.c - crackle test of the x8p's own data path (run elevated).
 *
 * Plays a continuous 997 Hz sine on playback slots 0-1 (MON L/R) and records
 * record slots 26-27 (the x8p's monitor-mix capture, descriptor order). Every
 * captured sample is checked against the sine recurrence
 *     x[n] = 2 cos(w) x[n-1] - x[n-2]
 * whose error is ~1e-6 of full scale on a clean path. A click, dropout,
 * repeated or skipped block shows up as an error orders of magnitude larger.
 * Events are tagged when the feeder (this program) was late, so the test never
 * blames the hardware for its own scheduling.
 *
 *   loopback-test [seconds=1800] [--threshold 1e-4]
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <windows.h>
#include <avrt.h>
#include <cfgmgr32.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "..\..\include\apollo_ioctl.h"
#include "..\..\include\apollo_select.h"   /* [Twin X G2] the x8p among several units */

#define PI              3.14159265358979323846
#define RATE            48000.0
#define FREQ            997.0
#define LEVEL_DB        (-12.0)
#define LEAD_MIN        2048u           /* 43 ms */
#define LEAD_MAX        3072u           /* 64 ms */
#define SAFETY          256u            /* the driver answers LATE below lead + 48 = 192 (apollo_ioctl.h) */
#define PLAY_SLOT_L     0
#define PLAY_SLOT_R     1
#define REC_SLOT_L      26
#define REC_SLOT_R      27
#define SETTLE_SECONDS  0.5
#define MAX_PRINTED     40

static HANDLE g_dev = INVALID_HANDLE_VALUE;
static volatile LONG g_stop;

static BOOL WINAPI OnCtrl(DWORD t) { (void)t; InterlockedExchange(&g_stop, 1); return TRUE; }

/* [Twin X G2] The x8p only (GET_INFO DeviceType 0x22), never another Apollo on the driver. */
static BOOL OpenDevice(void)
{
    static APOLLO_UNIT_ENTRY units[APOLLO_SELECT_MAX];
    int n = ApolloListUnits(units, APOLLO_SELECT_MAX, NULL, 0);
    int pick = (n > 0) ? ApolloPickUnit(units, n, APOLLO_PICK_X8P) : -1;
    BOOL ok;

    if (n <= 0) {
        fprintf(stderr, "No Apollo on apollo.sys.\n");
        return FALSE;
    }
    if (pick < 0) {
        fprintf(stderr, "No Apollo x8p on apollo.sys (%d other unit(s): this test runs on the x8p only).\n", n);
        return FALSE;
    }
    g_dev = CreateFileW(units[pick].Path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    ok = g_dev != INVALID_HANDLE_VALUE;
    if (!ok) fprintf(stderr, "Opening the Apollo failed (%lu): run this from an elevated prompt.\n", GetLastError());
    return ok;
}

static BOOL Ioctl(DWORD code, void *in, DWORD inLen, void *out, DWORD outLen, DWORD *ret)
{
    DWORD r = 0;
    BOOL ok = DeviceIoControl(g_dev, code, in, inLen, out, outLen, &r, NULL);
    if (ret) *ret = r;
    return ok;
}

typedef struct {
    double c2;                  /* 2 cos(w) */
    double x1, x2;              /* previous two samples */
    ULONGLONG n;                /* samples seen since the sine started */
    double worst;
    double sumSq;
    ULONG sumN;
} CHECKER;

int main(int argc, char **argv)
{
    double seconds = 1800.0, threshold = 1e-4;
    APOLLO_INFO info;
    APOLLO_POSITION pos;
    ULONG C, R, i;
    unsigned char *play, *rec;
    PAPOLLO_PLAYBACK_HEADER ph;
    PAPOLLO_CAPTURE_HEADER chd;
    LONG *pdata;
    double amp, w;
    ULONGLONG start, written, readNext, end, toneStart, settleEnd, lastReport;
    ULONG feederLate = 0, capOverruns = 0, events = 0, printed = 0;
    ULONGLONG lastLateFrame = 0;
    CHECKER ck[2];
    LARGE_INTEGER qf, q0, qn, lastIter;
    double maxGapMs = 0;
    ULONG stalls = 0;

    for (i = 1; i < (ULONG)argc; i++) {
        if (strcmp(argv[i], "--threshold") == 0 && i + 1 < (ULONG)argc) threshold = atof(argv[++i]);
        else seconds = atof(argv[i]);
    }
    if (seconds < 5) seconds = 5;
    SetConsoleCtrlHandler(OnCtrl, TRUE);

    /*
     * Selecting text in a console with QuickEdit on blocks every write to it, which froze this
     * loop for ~200 ms in the first run (feeder late + capture overrun at 273 s). Turn QuickEdit
     * off for the run, and run the loop at MMCSS "Pro Audio" priority like an audio app.
     */
    {
        HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;
        if (GetConsoleMode(hin, &mode)) {
            SetConsoleMode(hin, (mode & ~ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS);
        }
    }
    {
        DWORD task = 0;
        HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
        if (mm) AvSetMmThreadPriority(mm, AVRT_PRIORITY_HIGH);
        else printf("(could not join MMCSS Pro Audio: %lu)\n", GetLastError());
    }
    if (!OpenDevice()) return 2;
    if (!Ioctl(IOCTL_APOLLO_GET_INFO, NULL, 0, &info, sizeof(info), NULL) || info.Stage != ApolloStageConnected) {
        fprintf(stderr, "The x8p is not connected.\n");
        return 2;
    }
    if (!(info.Flags & APOLLO_FLAG_TRANSPORT)) {
        printf("Starting the transport.\n");
        if (!Ioctl(IOCTL_APOLLO_TRANSPORT_START, NULL, 0, NULL, 0, NULL)) { fprintf(stderr, "TRANSPORT_START failed\n"); return 2; }
        Ioctl(IOCTL_APOLLO_GET_INFO, NULL, 0, &info, sizeof(info), NULL);
    }
    C = info.PlayChannels;
    R = info.RecChannels;
    if (C <= PLAY_SLOT_R || R <= REC_SLOT_R || info.SampleRate != 48000) {
        fprintf(stderr, "Unexpected layout: %lu play, %lu rec slots at %lu Hz.\n", C, R, info.SampleRate);
        return 2;
    }
    play = (unsigned char *)malloc(sizeof(APOLLO_PLAYBACK_HEADER) + (size_t)LEAD_MAX * C * 4);
    rec = (unsigned char *)malloc(sizeof(APOLLO_CAPTURE_HEADER) + (size_t)4096 * R * 4);     /* one READ_CAPTURE */
    if (!play || !rec) return 2;
    ph = (PAPOLLO_PLAYBACK_HEADER)play;
    pdata = (LONG *)(play + sizeof(*ph));
    chd = (PAPOLLO_CAPTURE_HEADER)rec;

    amp = pow(10.0, LEVEL_DB / 20.0) * 8388607.0 * 256.0;      /* S32, 24 bits at the top */
    w = 2.0 * PI * FREQ / RATE;
    memset(ck, 0, sizeof(ck));
    ck[0].c2 = ck[1].c2 = 2.0 * cos(w);

    if (!Ioctl(IOCTL_APOLLO_GET_POSITION, NULL, 0, &pos, sizeof(pos), NULL)) return 2;
    start = pos.Position + LEAD_MIN;
    written = start;
    toneStart = start;
    settleEnd = toneStart + (ULONGLONG)(SETTLE_SECONDS * RATE);
    end = start + (ULONGLONG)(seconds * RATE);
    readNext = pos.Position > 64 ? pos.Position - 64 : 0;
    lastReport = start;
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&q0);
    printf("Loopback: %.0f Hz at %.0f dBFS on MON L/R, checking record slots %d-%d for %.0f s "
           "(threshold %.0e of full scale). Ctrl+C stops early.\n", FREQ, LEVEL_DB, REC_SLOT_L + 1, REC_SLOT_R + 1,
           seconds, threshold);

    lastIter = q0;
    while (!g_stop) {
        ULONGLONG target;
        DWORD ret;
        LARGE_INTEGER now;
        double gapMs;

        QueryPerformanceCounter(&now);
        gapMs = (double)(now.QuadPart - lastIter.QuadPart) * 1000.0 / (double)qf.QuadPart;
        lastIter = now;
        if (gapMs > maxGapMs) maxGapMs = gapMs;
        if (gapMs > 40.0) {
            stalls++;
            printf("  this program was paused for %.0f ms (loop period is 5 ms)\n", gapMs);
        }
        if (!Ioctl(IOCTL_APOLLO_GET_POSITION, NULL, 0, &pos, sizeof(pos), NULL)) {
            fprintf(stderr, "Lost the device.\n");
            break;
        }
        /* --- feed --- */
        if (written < end) {
            if (written < pos.Position + SAFETY) {
                feederLate++;
                lastLateFrame = pos.Position;
                printf("  feeder late at hardware frame %llu (this program, not the x8p); resyncing\n", pos.Position);
                written = pos.Position + LEAD_MIN;
            }
            target = pos.Position + LEAD_MAX;
            if (target > end) target = end;
            if (written < target) {
                ULONG frames = (ULONG)(target - written), f;
                APOLLO_PLAYBACK_RESULT pr;

                memset(pdata, 0, (size_t)frames * C * 4);
                for (f = 0; f < frames; f++) {
                    double ph0 = w * (double)(written + f - toneStart);
                    LONG s = (LONG)(amp * sin(ph0));
                    s = (LONG)((ULONG)s & 0xFFFFFF00ul);
                    pdata[(size_t)f * C + PLAY_SLOT_L] = s;
                    pdata[(size_t)f * C + PLAY_SLOT_R] = s;
                }
                ph->StartPosition = written;
                ph->Frames = frames;
                ph->Channels = C;
                memset(&pr, 0, sizeof(pr));
                if (!Ioctl(IOCTL_APOLLO_WRITE_PLAYBACK, play, (DWORD)(sizeof(*ph) + (size_t)frames * C * 4),
                           &pr, sizeof(pr), &ret)) {
                    fprintf(stderr, "WRITE_PLAYBACK failed (%lu)\n", GetLastError());
                    break;
                }
                if (pr.Result == APOLLO_PLAYBACK_LATE) {
                    feederLate++;
                    lastLateFrame = pr.HwPosition;
                    written = pr.HwPosition + LEAD_MIN;
                } else if (pr.Result == APOLLO_PLAYBACK_WRITTEN) {
                    written += frames;
                }
            }
        }
        /* --- capture and check --- */
        for (;;) {
            APOLLO_CAPTURE_REQUEST cr;
            ULONG frames, f;

            cr.StartPosition = readNext;
            cr.MaxFrames = 4096;
            cr.Reserved = 0;
            if (!Ioctl(IOCTL_APOLLO_READ_CAPTURE, &cr, sizeof(cr), rec,
                       (DWORD)(sizeof(*chd) + (size_t)4096 * R * 4), &ret)) {
                fprintf(stderr, "READ_CAPTURE failed (%lu)\n", GetLastError());
                g_stop = 1;
                break;
            }
            if (chd->Flags & APOLLO_CAPTURE_OVERRUN) {
                capOverruns++;
                printf("  capture overrun at %llu: reader fell a ring behind (this program); skipping ahead\n", readNext);
                readNext = chd->HwPosition > 4096 ? chd->HwPosition - 4096 : 0;
                ck[0].n = ck[1].n = 0;          /* restart the recurrence after the gap */
                continue;
            }
            frames = chd->Frames;
            if (frames == 0) break;
            for (f = 0; f < frames; f++) {
                ULONGLONG fr = chd->StartPosition + f;
                const LONG *row = (const LONG *)(rec + sizeof(*chd)) + (size_t)f * chd->Channels;
                int c;

                for (c = 0; c < 2; c++) {
                    double x = (double)row[c ? REC_SLOT_R : REC_SLOT_L] / 2147483648.0;
                    CHECKER *k = &ck[c];

                    if (fr < settleEnd || fr >= end) {
                        k->n = 0;               /* onset, end, and the time before the sine reaches capture */
                        k->x2 = k->x1;
                        k->x1 = x;
                        continue;
                    }
                    if (k->n >= 2) {
                        double r = fabs(x - k->c2 * k->x1 + k->x2);
                        if (r > k->worst) k->worst = r;
                        if (r > threshold) {
                            int feeder = lastLateFrame && fr + 4096 > lastLateFrame && fr < lastLateFrame + 8192;
                            events++;
                            if (printed < MAX_PRINTED) {
                                printed++;
                                printf("  DISCONTINUITY %s at %.4f s (frame %llu): error %.2e (%.1f dBFS)%s\n",
                                       c ? "R" : "L", (double)(fr - toneStart) / RATE, fr, r, 20.0 * log10(r),
                                       feeder ? "  <- near a feeder resync (not the x8p)" : "");
                            }
                        }
                    }
                    k->sumSq += x * x;
                    k->sumN++;
                    k->x2 = k->x1;
                    k->x1 = x;
                    k->n++;
                }
            }
            readNext = chd->StartPosition + frames;
            if (frames < 4096) break;
        }
        /* --- progress every 10 s --- */
        if (readNext >= lastReport + 480000) {
            double el = (double)(readNext - toneStart) / RATE;
            double rmsL = ck[0].sumN ? sqrt(ck[0].sumSq / ck[0].sumN) : 0, rmsR = ck[1].sumN ? sqrt(ck[1].sumSq / ck[1].sumN) : 0;
            printf("%7.0f s | level L %.2f R %.2f dBFS | worst error %.1e / %.1e | discontinuities %lu | feeder late %lu | capture overruns %lu\n",
                   el, rmsL > 0 ? 20 * log10(rmsL * sqrt(2.0)) : -144.0, rmsR > 0 ? 20 * log10(rmsR * sqrt(2.0)) : -144.0,
                   ck[0].worst, ck[1].worst, events, feederLate, capOverruns);
            ck[0].sumSq = ck[1].sumSq = 0;
            ck[0].sumN = ck[1].sumN = 0;
            lastReport = readNext;
        }
        if (readNext >= end + 4800) break;
        Sleep(5);
    }

    QueryPerformanceCounter(&qn);
    printf("\nRan %.1f s of audio (%.1f s wall). Discontinuities: %lu. Worst error L %.1e, R %.1e. "
           "Feeder late: %lu. Capture overruns: %lu. Program pauses over 40 ms: %lu (longest %.0f ms).\n",
           (double)(min(readNext, end) - toneStart) / RATE, (double)(qn.QuadPart - q0.QuadPart) / qf.QuadPart,
           events, ck[0].worst, ck[1].worst, feederLate, capOverruns, stalls, maxGapMs);
    if (ck[0].worst == 0 && ck[1].worst == 0)
        printf("VERDICT: NO SIGNAL - the sine never reached record slots %d-%d.\n", REC_SLOT_L + 1, REC_SLOT_R + 1);
    else if (events == 0)
        printf("VERDICT: CLEAN - the x8p's playback -> DSP monitor -> capture path did not glitch once.\n");
    else
        printf("VERDICT: %lu glitch(es). Ones not marked as feeder resyncs are on the x8p side.\n", events);
    return events ? 1 : 0;
}
