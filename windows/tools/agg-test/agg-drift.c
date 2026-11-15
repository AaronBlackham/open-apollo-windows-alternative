/*
 * agg-drift.c - measures the clock drift between the two Apollos (docs/spec-twinx.md 6.3.2, gate G6).
 *
 *   agg-drift [seconds] [--csv file]
 *
 * Opens every unit apollo.sys runs (read-only handles) and polls GET_ENGINE_STATS on each back to
 * back every 2 ms. Each answer is the engine tick's own snapshot: a 64-bit SAMPLE_POS and the QPC
 * instant of that read. Per unit: least-squares frames/s against QPC and the residual. Between the
 * units: the x8p and Twin X positions at the same QPC instants (each projected to a common instant
 * with its own fitted rate), their difference over time and its slope in ppm. QPC errors cancel in
 * the relative figure; only the two position counters matter.
 *
 * Also reports the SAMPLE_POS granularity (the gcd of position steps between ticks) and how far
 * the follower's position wanders around a straight line (the jitter the clock loop must absorb).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "..\..\include\apollo_ioctl.h"
#include "..\..\include\apollo_asio.h"
#include "..\..\include\apollo_select.h"

#define MAXS 200000

typedef struct {
    double t;           /* seconds since the first sample (QPC) */
    double pos;         /* frames since the first sample */
} SAMPLE;

static SAMPLE g_s[2][MAXS];
static int g_n[2];

static BOOL Stats(HANDLE h, APOLLO_ENGINE_STATS *s)
{
    ULONG flags = 0;
    DWORD r = 0;

    memset(s, 0, sizeof(*s));
    return DeviceIoControl(h, IOCTL_APOLLO_GET_ENGINE_STATS, &flags, sizeof(flags), s, sizeof(*s), &r, NULL) &&
           r >= sizeof(*s);
}

static void Fit(const SAMPLE *s, int n, double *a, double *b, double *rms, double *maxdev)
{
    double st = 0, sp = 0, stt = 0, stp = 0, d, e = 0, m = 0;
    int i;

    for (i = 0; i < n; i++) {
        st += s[i].t;
        sp += s[i].pos;
        stt += s[i].t * s[i].t;
        stp += s[i].t * s[i].pos;
    }
    d = n * stt - st * st;
    *b = d != 0 ? (n * stp - st * sp) / d : 0;
    *a = (sp - *b * st) / n;
    for (i = 0; i < n; i++) {
        double r = s[i].pos - (*a + *b * s[i].t);
        e += r * r;
        if (fabs(r) > m) m = fabs(r);
    }
    *rms = n ? sqrt(e / n) : 0;
    *maxdev = m;
}

static ULONGLONG Gcd(ULONGLONG a, ULONGLONG b)
{
    while (b) {
        ULONGLONG t = a % b;
        a = b;
        b = t;
    }
    return a;
}

int main(int argc, char **argv)
{
    static APOLLO_UNIT_ENTRY units[APOLLO_SELECT_MAX];
    HANDLE h[2] = { INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE };
    const char *name[2] = { "x8p", "twx" };
    double secs = 60.0;
    const char *csv = NULL;
    int n, i, u, pick[2];
    LONGLONG q0 = 0, lastQ[2] = { 0, 0 };
    ULONGLONG p0[2] = { 0, 0 }, lastP[2] = { 0, 0 }, g[2] = { 0, 0 };
    LARGE_INTEGER f, t0, now;
    FILE *out = NULL;
    double qf;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc) csv = argv[++i];
        else secs = atof(argv[i]);
    }
    if (secs < 2) secs = 2;
    n = ApolloListUnits(units, APOLLO_SELECT_MAX, NULL, 0);
    pick[0] = ApolloPickUnit(units, n, APOLLO_PICK_X8P);
    pick[1] = ApolloPickUnit(units, n, APOLLO_PICK_TWINX);
    if (pick[0] < 0 || pick[1] < 0) {
        for (i = 0; i < n; i++) {
            printf("unit %d: error %lu, %lu bytes, type 0x%lx\n", i, units[i].Error, units[i].InfoBytes,
                   units[i].Info.DeviceType);
        }
        printf("need both units on apollo.sys (x8p %d, Twin X %d, %d listed)\n", pick[0], pick[1], n);
        return 2;
    }
    for (u = 0; u < 2; u++) {
        h[u] = CreateFileW(units[pick[u]].Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
        if (h[u] == INVALID_HANDLE_VALUE) {
            printf("cannot open the %s (%lu)\n", name[u], GetLastError());
            return 2;
        }
    }
    if (csv && fopen_s(&out, csv, "w") != 0) out = NULL;
    if (out) fprintf(out, "unit,qpc,position\n");
    QueryPerformanceFrequency(&f);
    qf = (double)f.QuadPart;
    timeBeginPeriod(1);
    QueryPerformanceCounter(&t0);
    for (;;) {
        for (u = 0; u < 2; u++) {
            APOLLO_ENGINE_STATS s;

            if (!Stats(h[u], &s) || !s.Running || s.Qpc == 0) continue;
            if (s.Qpc == lastQ[u]) continue;         /* same tick as last time */
            if (q0 == 0) q0 = s.Qpc;
            if (g_n[u] == 0) p0[u] = s.Position;
            else if (s.Position > lastP[u]) g[u] = Gcd(g[u], s.Position - lastP[u]);
            lastQ[u] = s.Qpc;
            lastP[u] = s.Position;
            if (out) fprintf(out, "%d,%lld,%llu\n", u, s.Qpc, s.Position);
            if (g_n[u] < MAXS) {
                g_s[u][g_n[u]].t = (double)(s.Qpc - q0) / qf;
                g_s[u][g_n[u]].pos = (double)(LONGLONG)(s.Position - p0[u]);
                g_n[u]++;
            }
        }
        QueryPerformanceCounter(&now);
        if ((double)(now.QuadPart - t0.QuadPart) / qf >= secs) break;
        Sleep(2);
    }
    timeEndPeriod(1);
    if (out) fclose(out);
    {
        double a[2], b[2], rms[2], mx[2];

        for (u = 0; u < 2; u++) {
            Fit(g_s[u], g_n[u], &a[u], &b[u], &rms[u], &mx[u]);
            printf("%s: %d ticks sampled over %.1f s, %.3f frames/s against QPC (%.2f ppm vs 48000), "
                   "residual %.2f frames rms, max %.2f; position step gcd %llu\n",
                   name[u], g_n[u], g_n[u] ? g_s[u][g_n[u] - 1].t : 0.0, b[u], (b[u] / 48000.0 - 1) * 1e6,
                   rms[u], mx[u], g[u]);
        }
        printf("relative: Twin X / x8p - 1 = %+.3f ppm (fitted rates)\n", (b[1] / b[0] - 1) * 1e6);
        /*
         * Direct: for each Twin X sample, the x8p position at that instant (interpolated between its
         * neighbouring samples), then the slope of (twx - x8p) against x8p frames.
         */
        {
            static SAMPLE d[MAXS];
            int k = 0, j = 0;
            double a2, b2, r2, m2;

            for (i = 0; i < g_n[1]; i++) {
                double t = g_s[1][i].t;
                while (j + 1 < g_n[0] && g_s[0][j + 1].t < t) j++;
                if (j + 1 >= g_n[0] || g_s[0][j].t > t) continue;
                {
                    double w = (t - g_s[0][j].t) / (g_s[0][j + 1].t - g_s[0][j].t);
                    double px = g_s[0][j].pos + w * (g_s[0][j + 1].pos - g_s[0][j].pos);
                    d[k].t = px;                         /* x8p frames */
                    d[k].pos = g_s[1][i].pos - px;       /* Twin X minus x8p */
                    k++;
                }
            }
            Fit(d, k, &a2, &b2, &r2, &m2);
            printf("direct: (twx - x8p) slope %+.3f ppm over %d pairs, wander around the line %.2f frames rms, "
                   "max %.2f; offset moved %+.1f frames in the run\n", b2 * 1e6, k, r2, m2,
                   k ? d[k - 1].pos - d[0].pos : 0.0);
        }
    }
    CloseHandle(h[0]);
    CloseHandle(h[1]);
    return 0;
}
