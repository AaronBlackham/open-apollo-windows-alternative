/*
 * agg-test.c - the combined device's IOCTLs from the command line (gate G6, docs/spec-twinx.md 6.9).
 *
 *   agg-test state [--reset]          AGG_GET_STATE: members, roles, clock loop, latency, ASIO
 *   agg-test watch <seconds>          the clock loop once a second
 *   agg-test layout                   AGG_GET_LAYOUT: the logical channel table
 *   agg-test config [--save file]     AGG_GET_CONFIG: what is stored (or the default); [G7] --save
 *                                     writes the whole binary configuration (JSON included) to a file
 *   agg-test set [--order x8p,twx] [--master x8p|twx] [--routes studio|mirror|phones|none]
 *                [--asrc 8|16|32] [--off|--on] [--align 0|1] | --load file
 *                                     AGG_SET_CONFIG from the current configuration, changed as
 *                                     asked (user level: no elevation needed). [G7] Routes set the
 *                                     DAW pair and the Windows "Monitor" endpoint together (spec 6.8
 *                                     presets; none = DAW unrouted, Windows on the master's MON);
 *                                     --load stores a file --save wrote, exactly (step-g7.ps1 puts
 *                                     Aaron's configuration back with it)
 *   agg-test probe --unit x8p|twinx --source 0x0A|0x0C [--ax 0|0xF|0x20F] [--wait ms]
 *                                     AGG_CLOCK_PROBE (administrator): the peer-clock experiment
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "..\..\include\apollo_ioctl.h"
#include "..\..\include\apollo_asio.h"
#include "..\..\include\apollo_agg.h"
#include "..\..\include\apollo_select.h"

#ifndef MAXULONG
#define MAXULONG 0xFFFFFFFFul
#endif

static APOLLO_UNIT_ENTRY g_units[APOLLO_SELECT_MAX];
static int g_n;

static HANDLE OpenUnit(int Want, BOOL Write)
{
    int pick = ApolloPickUnit(g_units, g_n, Want);
    HANDLE h;

    if (pick < 0) {
        printf("no such unit on apollo.sys\n");
        return INVALID_HANDLE_VALUE;
    }
    h = CreateFileW(g_units[pick].Path, GENERIC_READ | (Write ? GENERIC_WRITE : 0), FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("cannot open the unit (%lu)\n", GetLastError());
    }
    return h;
}

static BOOL Ioctl(HANDLE h, DWORD code, void *in, DWORD inLen, void *out, DWORD outLen, DWORD *ret)
{
    DWORD r = 0;
    BOOL ok = DeviceIoControl(h, code, in, inLen, out, outLen, &r, NULL);

    if (ret) *ret = r;
    if (!ok) printf("IOCTL 0x%08lx failed: %lu\n", code, GetLastError());
    return ok;
}

static const char *ModelTag(ULONG m)
{
    return m == APOLLO_INFO_MODEL_X8P ? "x8p" : (m == APOLLO_INFO_MODEL_TWINX ? "TwinX" : "unit");
}

static const char *ClockName(ULONG s)
{
    switch (s) {
    case APOLLO_AGG_CLOCK_NONE:     return "-";
    case APOLLO_AGG_CLOCK_ACQUIRE:  return "ACQUIRING";
    case APOLLO_AGG_CLOCK_LOCKED:   return "LOCKED";
    case APOLLO_AGG_CLOCK_DEGRADED: return "DEGRADED";
    default:                        return "?";
    }
}

static void PrintState(const APOLLO_AGG_STATE *s)
{
    ULONG u;

    printf("combined device: %s%s%s, layout v%lu, timeline epoch %lu, %lu unit(s), master %lu, timeline %ld, "
           "ASRC h %lu\n", (s->Flags & APOLLO_AGG_STATE_ENABLED) ? "enabled" : "OFF",
           (s->Flags & APOLLO_AGG_STATE_ACTIVE) ? ", active" : ", not active",
           (s->Flags & APOLLO_AGG_STATE_STORED) ? ", stored config" : ", default config", s->LayoutVersion,
           s->TimelineEpoch, s->UnitCount, s->Master, s->Timeline == MAXULONG ? -1L : (long)s->Timeline, s->AsrcHalf);
    printf("latency in %lu / out %lu frames (without the buffer); ticks %llu, aggregate work last %lu us, max %lu us\n",
           s->InLatency, s->OutLatency, s->Ticks, s->AsrcLastUs, s->AsrcMaxUs);
    printf("ASIO: state %lu, %lu frames, %lu in, %lu out, %lu switches, %lu underruns, %lu overruns, %lu resyncs\n",
           s->AsioState, s->AsioBufferFrames, s->AsioInputs, s->AsioOutputs, s->AsioSwitches, s->AsioUnderruns,
           s->AsioOverruns, s->AsioResyncs);
    /* [G7] (a G6 driver leaves these 0: generation 0 says so) */
    if (s->ConfigGeneration == 0) {
        printf("endpoint set: (apollo.sys before G7: no combined Windows endpoints); config generation 0\n");
    } else {
        printf("endpoint set: %s, %lu combined Windows stream(s) running; %s; start ratios %s; config generation %lu\n",
               s->EndpointSet == APOLLO_AGG_EPSET_COMBINED ? "COMBINED" : "standalone", s->EndpointStreams,
               (s->Flags & APOLLO_AGG_STATE_ALIGNED) ? "aligned" : "UNALIGNED", (s->Flags & APOLLO_AGG_STATE_RATIOS) ?
               "loaded" : "none kept", s->ConfigGeneration);
    }
    for (u = 0; u < s->UnitCount && u < APOLLO_AGG_MAX_UNITS; u++) {
        const APOLLO_AGG_UNIT_STATE *x = &s->Unit[u];

        printf("  [%lu] %-5s %016llx %s%s%s%s clock %-9s ratio %+9.3f ppm  e %+7.3f (peak %.3f) frames  resyncs %lu "
               "outliers %lu locked %.1f s gain %.3f  pos %llu  render late %lu  capture short %lu  latency %lu/%lu\n",
               u, ModelTag(x->Model), x->Uid, (x->Flags & APOLLO_AGG_UNIT_PRESENT) ? "present" : "ABSENT",
               (x->Flags & APOLLO_AGG_UNIT_MASTER) ? " master" : "", (x->Flags & APOLLO_AGG_UNIT_TIMELINE) ? " timeline" : "",
               (x->Flags & APOLLO_AGG_UNIT_USABLE) ? " running" : "", ClockName(x->ClockState), x->RatioPpb / 1000.0,
               x->ErrorMilli / 1000.0, x->ErrorPeakMilli / 1000.0, x->Resyncs, x->Outliers, x->LockedMs / 1000.0,
               x->GainMilli / 1000.0, x->Position, x->RenderLate, x->CaptureShort, x->InLatency, x->OutLatency);
    }
}

static BOOL GetState(HANDLE h, APOLLO_AGG_STATE *s, BOOL reset)
{
    ULONG flags = reset ? APOLLO_AGG_RESET : 0;
    return Ioctl(h, IOCTL_APOLLO_AGG_GET_STATE, &flags, sizeof(flags), s, sizeof(*s), NULL);
}

static int CmdLayout(HANDLE h)
{
    static APOLLO_AGG_LAYOUT l;
    ULONG i;

    if (!Ioctl(h, IOCTL_APOLLO_AGG_GET_LAYOUT, NULL, 0, &l, sizeof(l), NULL)) return 1;
    printf("layout v%lu (%s): %lu in, %lu out, latency in %lu out %lu, master %lu, timeline %ld (epoch %lu)\n",
           l.LayoutVersion, (l.Flags & APOLLO_AGG_LAYOUT_ACTIVE) ? "active" : "not active", l.InCount, l.OutCount,
           l.InLatency, l.OutLatency, l.Master, l.Timeline == MAXULONG ? -1L : (long)l.Timeline, l.TimelineEpoch);
    for (i = 0; i < l.UnitCount && i < APOLLO_AGG_MAX_UNITS; i++) {
        printf("  unit %lu: %s %016llx %s\n", i, ModelTag(l.Model[i]), l.Uid[i], l.Present[i] ? "present" : "absent");
    }
    for (i = 0; i < l.InCount; i++) {
        printf("  in  %3lu  %-24s unit %u slot %2u id %04x lat %3u %s%s\n", i + 1, l.In[i].Name, l.In[i].Unit,
               l.In[i].Slot, l.In[i].Id, l.In[i].LatencyFrames, (l.In[i].Flags & APOLLO_AGG_CH_RESAMPLED) ? "resampled" : "",
               (l.In[i].Flags & APOLLO_AGG_CH_PRESENT) ? "" : " ABSENT");
    }
    for (i = 0; i < l.OutCount; i++) {
        printf("  out %3lu  %-24s unit %u slot %2u id %04x lat %3u %s%s%s\n", i + 1, l.Out[i].Name, l.Out[i].Unit,
               l.Out[i].Slot, l.Out[i].Id, l.Out[i].LatencyFrames,
               (l.Out[i].Flags & APOLLO_AGG_CH_RESAMPLED) ? "resampled" : "",
               (l.Out[i].Flags & APOLLO_AGG_CH_ROUTED) ? " ROUTED" : "", (l.Out[i].Flags & APOLLO_AGG_CH_PRESENT) ? "" : " ABSENT");
    }
    return 0;
}

static APOLLO_AGG_CONFIG g_cfg;

static int CmdConfig(HANDLE h)
{
    ULONG i;

    if (!Ioctl(h, IOCTL_APOLLO_AGG_GET_CONFIG, NULL, 0, &g_cfg, sizeof(g_cfg), NULL)) return 1;
    printf("config%s: flags 0x%lx, %lu unit(s), master %lu, ASRC h %lu, %s order (%lu in / %lu out), %lu route(s), "
           "JSON %lu bytes\n", (g_cfg.Flags & APOLLO_AGG_CFG_DEFAULT) ? " (DEFAULT, nothing stored)" : "", g_cfg.Flags,
           g_cfg.UnitCount, g_cfg.Master, g_cfg.AsrcHalf, (g_cfg.Flags & APOLLO_AGG_CFG_EXPLICIT) ? "explicit" : "simple",
           g_cfg.InCount, g_cfg.OutCount, g_cfg.RouteCount, g_cfg.JsonBytes);
    for (i = 0; i < g_cfg.UnitCount && i < APOLLO_AGG_MAX_UNITS; i++) {
        printf("  unit %lu: %s %016llx, cached descriptor %u in / %u out, trims capture %d render %d\n", i,
               ModelTag(g_cfg.Unit[i].Model), g_cfg.Unit[i].Uid, g_cfg.Unit[i].InCount, g_cfg.Unit[i].OutCount,
               g_cfg.Unit[i].CaptureTrim, g_cfg.Unit[i].RenderTrim);
    }
    for (i = 0; i < g_cfg.RouteCount && i < APOLLO_AGG_MAX_ROUTES; i++) {
        const APOLLO_AGG_ROUTE_CFG *r = &g_cfg.Route[i];
        printf("  route %lu: src kind %u unit %u ids %04x/%04x -> unit %u ids %04x/%04x\n", i, r->SrcKind, r->SrcUnit,
               r->SrcId[0], r->SrcId[1], r->DstUnit, r->DstId[0], r->DstId[1]);
    }
    if (g_cfg.JsonBytes) printf("  json: %.*s\n", (int)g_cfg.JsonBytes, g_cfg.Json);
    printf("  %s\n", (g_cfg.Flags & APOLLO_AGG_CFG_ALIGN) ? "aligned" : "UNALIGNED (lowest latency)");
    return 0;
}

/* [G7] agg-test config --save file: the binary configuration as the driver holds it. */
static int SaveConfig(const char *Path)
{
    FILE *f = NULL;

    if (fopen_s(&f, Path, "wb") != 0 || f == NULL) {
        printf("cannot write %s\n", Path);
        return 1;
    }
    if (fwrite(&g_cfg, sizeof(g_cfg), 1, f) != 1) {
        fclose(f);
        printf("cannot write %s\n", Path);
        return 1;
    }
    fclose(f);
    printf("saved to %s (%lu bytes%s)\n", Path, (unsigned long)sizeof(g_cfg),
           (g_cfg.Flags & APOLLO_AGG_CFG_DEFAULT) ? ", the driver's DEFAULT: --load stores it as a configuration" : "");
    return 0;
}

static int LoadConfig(const char *Path)
{
    FILE *f = NULL;
    size_t got;

    if (fopen_s(&f, Path, "rb") != 0 || f == NULL) {
        printf("cannot read %s\n", Path);
        return 1;
    }
    got = fread(&g_cfg, 1, sizeof(g_cfg), f);
    fclose(f);
    if (got != sizeof(g_cfg) || g_cfg.Version != APOLLO_AGG_VERSION || g_cfg.Bytes != sizeof(g_cfg)) {
        printf("%s is not a configuration this build saved (%lu bytes)\n", Path, (unsigned long)got);
        return 1;
    }
    return 0;
}

static int UnitIndexByModel(ULONG model)
{
    ULONG i;
    for (i = 0; i < g_cfg.UnitCount; i++) if (g_cfg.Unit[i].Model == model) return (int)i;
    return -1;
}

static ULONG ModelOf(const char *s)
{
    return (_stricmp(s, "x8p") == 0) ? APOLLO_INFO_MODEL_X8P : APOLLO_INFO_MODEL_TWINX;
}

static int CmdSet(HANDLE h, int argc, char **argv)
{
    APOLLO_AGG_SET_RESULT res;
    int i, loaded = 0;
    DWORD r = 0;

    memset(&res, 0, sizeof(res));
    for (i = 0; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--load") == 0) {
            if (LoadConfig(argv[i + 1]) != 0) return 1;
            loaded = 1;
        }
    }
    if (!loaded && !Ioctl(h, IOCTL_APOLLO_AGG_GET_CONFIG, NULL, 0, &g_cfg, sizeof(g_cfg), NULL)) return 1;
    g_cfg.Flags &= ~APOLLO_AGG_CFG_DEFAULT;
    for (i = 0; i < argc && !loaded; i++) {
        if (strcmp(argv[i], "--order") == 0 && i + 1 < argc) {
            char buf[64], *ctx = NULL, *tok;
            APOLLO_AGG_UNIT_CFG u[APOLLO_AGG_MAX_UNITS];
            ULONG n = 0, oldMaster = g_cfg.Unit[g_cfg.Master].Model, k;
            strcpy_s(buf, sizeof(buf), argv[++i]);
            for (tok = strtok_s(buf, ",", &ctx); tok && n < APOLLO_AGG_MAX_UNITS; tok = strtok_s(NULL, ",", &ctx)) {
                int idx = UnitIndexByModel(ModelOf(tok));
                if (idx >= 0) u[n++] = g_cfg.Unit[idx];
            }
            if (n == g_cfg.UnitCount) {
                ULONG dstModel[APOLLO_AGG_MAX_ROUTES], srcModel[APOLLO_AGG_MAX_ROUTES];
                for (k = 0; k < g_cfg.RouteCount && k < APOLLO_AGG_MAX_ROUTES; k++) {
                    dstModel[k] = g_cfg.Unit[g_cfg.Route[k].DstUnit].Model;
                    srcModel[k] = g_cfg.Unit[g_cfg.Route[k].SrcUnit].Model;
                }
                memcpy(g_cfg.Unit, u, sizeof(u[0]) * n);
                for (k = 0; k < n; k++) if (g_cfg.Unit[k].Model == oldMaster) g_cfg.Master = k;
                /* routes refer to unit indexes: re-point them to the same units */
                for (k = 0; k < g_cfg.RouteCount && k < APOLLO_AGG_MAX_ROUTES; k++) {
                    g_cfg.Route[k].DstUnit = (UCHAR)max(0, UnitIndexByModel(dstModel[k]));
                    g_cfg.Route[k].SrcUnit = (UCHAR)max(0, UnitIndexByModel(srcModel[k]));
                }
            }
        } else if (strcmp(argv[i], "--master") == 0 && i + 1 < argc) {
            int idx = UnitIndexByModel(ModelOf(argv[++i]));
            if (idx >= 0) g_cfg.Master = (ULONG)idx;
        } else if (strcmp(argv[i], "--routes") == 0 && i + 1 < argc) {
            const char *p = argv[++i];
            int twx = UnitIndexByModel(APOLLO_INFO_MODEL_TWINX), x8p = UnitIndexByModel(APOLLO_INFO_MODEL_X8P);
            ULONG k;
            memset(g_cfg.Route, 0, sizeof(g_cfg.Route));
            g_cfg.RouteCount = 0;
            if ((strcmp(p, "studio") == 0 || strcmp(p, "mirror") == 0 || strcmp(p, "phones") == 0) && twx >= 0) {
                /* [G7] each preset routes the DAW pair and the Windows Monitor endpoint (spec 6.8) */
                g_cfg.Route[0].SrcKind = APOLLO_AGG_SRC_DAW;
                g_cfg.Route[0].DstUnit = (UCHAR)twx;
                g_cfg.Route[0].DstId[0] = 0x0901;
                g_cfg.Route[0].DstId[1] = 0x0902;
                g_cfg.Route[1] = g_cfg.Route[0];
                g_cfg.Route[1].SrcKind = APOLLO_AGG_SRC_WINDOWS;
                g_cfg.RouteCount = 2;
                if (strcmp(p, "phones") == 0) {
                    g_cfg.Route[1].DstId[0] = 0x0A01;   /* the Twin X CUE 1 = its headphones */
                    g_cfg.Route[1].DstId[1] = 0x0A02;
                }
                if (strcmp(p, "mirror") == 0 && x8p >= 0) {
                    for (k = 0; k < 2; k++) {
                        g_cfg.Route[2 + k] = g_cfg.Route[k];
                        g_cfg.Route[2 + k].DstUnit = (UCHAR)x8p;
                    }
                    g_cfg.RouteCount = 4;
                }
            }
        } else if (strcmp(argv[i], "--align") == 0 && i + 1 < argc) {
            if (atoi(argv[++i]) != 0) g_cfg.Flags |= APOLLO_AGG_CFG_ALIGN;
            else g_cfg.Flags &= ~APOLLO_AGG_CFG_ALIGN;
        } else if (strcmp(argv[i], "--asrc") == 0 && i + 1 < argc) {
            g_cfg.AsrcHalf = (ULONG)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--off") == 0) {
            g_cfg.Flags &= ~APOLLO_AGG_CFG_ENABLED;
        } else if (strcmp(argv[i], "--on") == 0) {
            g_cfg.Flags |= APOLLO_AGG_CFG_ENABLED;
        }
    }
    if (!loaded) {
        g_cfg.JsonBytes = 0;        /* Apollo Control rebuilds its view from the binary */
    }                               /* --load: exactly as saved, Apollo Control's JSON included */
    if (!Ioctl(h, IOCTL_APOLLO_AGG_SET_CONFIG, &g_cfg, sizeof(g_cfg), &res, sizeof(res), &r)) {
        return 1;
    }
    /* [G7 review] a refused configuration completes with success and the reason in Error */
    if (r >= sizeof(res) && res.Error != 0) {
        printf("refused by the driver: check %lu (nothing stored)\n", res.Error);
        return 1;
    }
    printf("stored: layout v%lu\n", res.LayoutVersion);
    return CmdConfig(h);
}

static int CmdProbe(int argc, char **argv)
{
    APOLLO_AGG_PROBE_IN in = { APOLLO_AGG_VERSION, 0x0A, 0, 2000 };
    APOLLO_AGG_PROBE_OUT out;
    int want = APOLLO_PICK_TWINX, i;
    HANDLE h;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--unit") == 0 && i + 1 < argc) want = (_stricmp(argv[++i], "x8p") == 0) ? APOLLO_PICK_X8P : APOLLO_PICK_TWINX;
        else if (strcmp(argv[i], "--source") == 0 && i + 1 < argc) in.Source = strtoul(argv[++i], NULL, 0);
        else if (strcmp(argv[i], "--ax") == 0 && i + 1 < argc) in.AxControl = strtoul(argv[++i], NULL, 0);
        else if (strcmp(argv[i], "--wait") == 0 && i + 1 < argc) in.WaitMs = strtoul(argv[++i], NULL, 0);
    }
    h = OpenUnit(want, TRUE);
    if (h == INVALID_HANDLE_VALUE) return 2;
    memset(&out, 0, sizeof(out));
    if (!Ioctl(h, IOCTL_APOLLO_AGG_CLOCK_PROBE, &in, sizeof(in), &out, sizeof(out), NULL)) {
        CloseHandle(h);
        return 1;
    }
    printf("probe source 0x%02lx ax 0x%03lx: ack %s (%lu ms); 0xC074 0x%08lx -> 0x%08lx; 0xC07C 0x%08lx -> 0x%08lx; "
           "0xC084 0x%08lx; 0xC08C 0x%08lx; AX_CONTROL 0x%03lx -> 0x%03lx; notifications 0x%08lx; P2P route 0x%08lx "
           "enable 0x%08lx slots %08lx %08lx %08lx %08lx; peer latency %08lx %08lx %08lx; SAMPLE_POS +%lu in 100 ms\n",
           in.Source, in.AxControl, out.AckMs == MAXULONG ? "NO" : "yes", out.AckMs == MAXULONG ? 0 : out.AckMs,
           out.ClockCfgBefore, out.ClockCfgAfter, out.RateInfoBefore, out.RateInfoAfter, out.ClockInfo, out.ClockSrcs,
           out.AxControlBefore, out.AxControlAfter, out.NotifSeen, out.P2pRoute, out.P2pEnable, out.P2pSlot[0],
           out.P2pSlot[1], out.P2pSlot[2], out.P2pSlot[3], out.PeerLatency[0], out.PeerLatency[1], out.PeerLatency[2],
           out.FramesIn100ms);
    CloseHandle(h);
    return 0;
}

int main(int argc, char **argv)
{
    static APOLLO_AGG_STATE s;
    HANDLE h;
    int rc = 0;

    if (argc < 2) {
        printf("usage: agg-test state|watch <s>|layout|config|set ...|probe ...\n");
        return 2;
    }
    g_n = ApolloListUnits(g_units, APOLLO_SELECT_MAX, NULL, 0);
    if (g_n <= 0) {
        printf("no Apollo on apollo.sys\n");
        return 2;
    }
    if (strcmp(argv[1], "probe") == 0) return CmdProbe(argc - 2, argv + 2);
    h = OpenUnit(APOLLO_PICK_DEFAULT, FALSE);
    if (h == INVALID_HANDLE_VALUE) return 2;
    if (strcmp(argv[1], "state") == 0) {
        rc = GetState(h, &s, argc > 2 && strcmp(argv[2], "--reset") == 0) ? (PrintState(&s), 0) : 1;
    } else if (strcmp(argv[1], "watch") == 0) {
        int secs = argc > 2 ? atoi(argv[2]) : 10, t;
        for (t = 0; t < secs && rc == 0; t++) {
            if (!GetState(h, &s, FALSE)) { rc = 1; break; }
            if (s.UnitCount >= 2) {
                ULONG u;
                printf("t=%3d", t);
                for (u = 0; u < s.UnitCount && u < APOLLO_AGG_MAX_UNITS; u++) {
                    const APOLLO_AGG_UNIT_STATE *x = &s.Unit[u];
                    printf("  %s %s%s ratio %+8.3f ppm e %+6.3f peak %.2f rs %lu", ModelTag(x->Model),
                           (x->Flags & APOLLO_AGG_UNIT_TIMELINE) ? "TL " : "", ClockName(x->ClockState),
                           x->RatioPpb / 1000.0, x->ErrorMilli / 1000.0, x->ErrorPeakMilli / 1000.0, x->Resyncs);
                }
                printf("  work %lu/%lu us\n", s.AsrcLastUs, s.AsrcMaxUs);
            }
            Sleep(1000);
        }
    } else if (strcmp(argv[1], "layout") == 0) {
        rc = CmdLayout(h);
    } else if (strcmp(argv[1], "config") == 0) {
        rc = CmdConfig(h);
        if (rc == 0 && argc > 3 && strcmp(argv[2], "--save") == 0) rc = SaveConfig(argv[3]);
    } else if (strcmp(argv[1], "set") == 0) {
        rc = CmdSet(h, argc - 2, argv + 2);
    } else {
        printf("unknown command\n");
        rc = 2;
    }
    CloseHandle(h);
    return rc;
}
