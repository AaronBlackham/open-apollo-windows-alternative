/*
 * mon-cmds-test.cpp - host-side check of app\src\monitor_cmds.h, Apollo Control's names for the monitor
 * features: every name the UI sends maps to the SET_MONITOR_PARAM code driver\monitor_plan.h encodes,
 * malformed names are refused, and the host refuses on the x8p only its analog headphone level.
 * Exit code 0 = all pass.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <windows.h>
#include <stdio.h>
#include "..\..\include\apollo_ioctl.h"
#include "..\..\driver\monitor_plan.h"
#include "..\..\app\src\monitor_cmds.h"

static int g_fail = 0, g_pass = 0;

static void Check(bool ok, const char *what)
{
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        printf("FAIL  %s\n", what);
    }
}

static bool Is(const wchar_t *name, ULONG want)
{
    ULONG p = 0xDEADBEEF;
    return MonFeatureParam(name, p) && p == want;
}

static bool No(const wchar_t *name)
{
    ULONG p = 0;
    return !MonFeatureParam(name, p);
}

int main()
{
    Check(Is(L"altsel", APOLLO_MP_ALT_SELECT) && Is(L"alt1", APOLLO_MP_ALT1_ENABLE) && Is(L"alt2", APOLLO_MP_ALT2_ENABLE) &&
          Is(L"alt1trim", APOLLO_MP_ALT1_TRIM) && Is(L"alt2trim", APOLLO_MP_ALT2_TRIM), "ALT names");
    Check(Is(L"cue1mix", APOLLO_MP_CUE1_MIX) && Is(L"cue2mix", APOLLO_MP_CUE2_MIX) && Is(L"cue3mix", APOLLO_MP_CUE3_MIX) &&
          Is(L"cue4mix", APOLLO_MP_CUE4_MIX), "cue MIX names");
    Check(Is(L"cue1mono", APOLLO_MP_CUE1_MONO) && Is(L"cue2mono", APOLLO_MP_CUE2_MONO) && Is(L"cue3mono", APOLLO_MP_CUE3_MONO) &&
          Is(L"cue4mono", APOLLO_MP_CUE4_MONO), "cue mono names");
    Check(Is(L"cue1mute", APOLLO_MP_CUE_MUTE(0)) && Is(L"cue4mute", APOLLO_MP_CUE_MUTE(3)), "cue mute names");
    Check(Is(L"send:1:0", APOLLO_MP_CUE_SEND(0, 0)) && Is(L"send:4:7", APOLLO_MP_CUE_SEND(3, 7)) &&
          Is(L"send:2:10", APOLLO_MP_CUE_SEND(1, 10)) && Is(L"send:3:31", APOLLO_MP_CUE_SEND(2, 31)), "send names");
    Check(Is(L"pan:1:1", APOLLO_MP_CUE_PAN(0, 1)) && Is(L"pan:4:10", APOLLO_MP_CUE_PAN(3, 10)), "pan names");
    Check(No(L"send:0:0") && No(L"send:5:0") && No(L"send:1:32") && No(L"send:1:") && No(L"send:1") &&
          No(L"send:1:-1") && No(L"send:1:010") && No(L"send:1:1x") && No(L"pan:x:1") && No(L"cue5mix") &&
          No(L"cue0mute") && No(L"send") && No(L"level") && No(L"") && No(L"alt3"), "malformed names refused");
    /* [Mix] the monitor mix */
    Check(Is(L"mix:0", APOLLO_MP_MIX_LEVEL(0)) && Is(L"mix:7", APOLLO_MP_MIX_LEVEL(7)) && Is(L"mix:10", APOLLO_MP_MIX_LEVEL(10)) &&
          Is(L"mixpan:1", APOLLO_MP_MIX_PAN(1)) && Is(L"mixmute:10", APOLLO_MP_MIX_MUTE(10)) &&
          Is(L"mixsolo:31", APOLLO_MP_MIX_SOLO(31)) && Is(L"mixmuteall", APOLLO_MP_MIX_MUTE_ALL), "monitor mix names");
    Check(No(L"mix:") && No(L"mix:32") && No(L"mix:-1") && No(L"mix:100") && No(L"mix:1x") && No(L"mix") &&
          No(L"mixpan:") && No(L"mixmute:a") && No(L"mixsolo") && No(L"mixmuteall1") && No(L"mix:1:2"),
          "malformed monitor mix names refused");
    /* [Aux] the aux buses */
    Check(Is(L"auxsend:1:0", APOLLO_MP_AUX_SEND(0, 0)) && Is(L"auxsend:2:10", APOLLO_MP_AUX_SEND(1, 10)) &&
          Is(L"auxpan:1:7", APOLLO_MP_AUX_PAN(0, 7)) && Is(L"auxpost:2:3", APOLLO_MP_AUX_POST(1, 3)) &&
          Is(L"auxret:1", APOLLO_MP_AUX_RETURN(0)) && Is(L"auxret:2", APOLLO_MP_AUX_RETURN(1)) &&
          Is(L"auxmute:2", APOLLO_MP_AUX_MUTE(1)) && Is(L"auxcue:1:1", APOLLO_MP_AUX_CUE(0, 0)) &&
          Is(L"auxcue:2:4", APOLLO_MP_AUX_CUE(1, 3)) && Is(L"auxcuepan:1:2", APOLLO_MP_AUX_CUE_PAN(0, 1)), "aux names");
    Check(No(L"auxsend:0:0") && No(L"auxsend:3:0") && No(L"auxsend:1:32") && No(L"auxsend:1") && No(L"auxret:3") &&
          No(L"auxret:1:0") && No(L"auxcue:1:0") && No(L"auxcue:1:5") && No(L"auxmute:") && No(L"aux") &&
          No(L"auxcuepan:2:9") && No(L"auxpost:1:x"), "malformed aux names refused");
    {
        static const wchar_t *names[] = { L"auxsend:1:0", L"auxpan:2:10", L"auxpost:1:7", L"auxret:2", L"auxmute:1",
                                          L"auxcue:2:4", L"auxcuepan:1:1" };
        bool ok = true;
        for (auto n : names) {
            ULONG p = 0;
            APOLLO_MON_WRITE w;
            ok = ok && MonFeatureParam(n, p) && ApolloMonEncode(ApolloMonModelFor(APOLLO_MODEL_X8P), p, 0, &w) == APOLLO_MON_OK &&
                 ApolloMonIsAux(&w);
        }
        Check(ok, "every aux name is a code the x8p's plan encodes as an aux write");
    }
    /* The names reach codes the driver encodes for the x8p */
    {
        static const wchar_t *names[] = { L"altsel", L"alt1", L"alt2", L"alt1trim", L"alt2trim", L"cue1mix", L"cue4mono",
                                          L"cue3mute", L"send:4:10", L"pan:2:7", L"mix:3", L"mixpan:7", L"mixmute:10",
                                          L"mixsolo:0", L"mixmuteall" };
        bool ok = true;
        for (auto n : names) {
            ULONG p = 0;
            APOLLO_MON_WRITE w;
            ok = ok && MonFeatureParam(n, p) && ApolloMonEncode(ApolloMonModelFor(APOLLO_MODEL_X8P), p, 1, &w) == APOLLO_MON_OK;
        }
        Check(ok, "every feature name is a code the x8p's plan encodes");
    }
    Check(MonHostRefusalX8p(APOLLO_MON_HP1_LEVEL) != nullptr && MonHostRefusalX8p(APOLLO_MON_HP1_SOURCE) == nullptr &&
          MonHostRefusalX8p(APOLLO_MON_HP2_SOURCE) == nullptr && MonHostRefusalX8p(APOLLO_MON_ALT_SELECT) == nullptr,
          "host on the x8p: HP level refused (analog knobs), HP sources and the rest to the driver");
    printf("mon-cmds-test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
