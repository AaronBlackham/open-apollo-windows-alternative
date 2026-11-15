/*
 * monitor_cmds.h - [Monitor] Apollo Control host side of the monitor features (ALT speakers, sources, cue
 * mixes, talkback to the cues; driver\monitor_plan.h): the names the UI sends as
 * "monitor\t<name>\t<value>[\t<unit>]" beyond G4's ten, and what the host itself refuses per model.
 *
 * Not included by main.cpp in this work copy (main.cpp belongs to the G7 change). The merge is three
 * hunks, in app\src\main-monitor.patch:
 *   1. #include "monitor_cmds.h" after "agg_json.h";
 *   2. MonitorParam(): `return MonFeatureParam(n, p);` instead of its final `return false;`;
 *   3. SettingAllowed(): the x8p block asks MonHostRefusalX8p() (the HP level stays refused: analog knobs;
 *      the HP1/HP2 source refusal goes, the driver's per-model plan decides).
 * Every command still goes out as SET_MONITOR_PARAM with the unit's slot; the driver refuses what the
 * unit lacks (STATUS_NOT_SUPPORTED / STATUS_INVALID_PARAMETER), which the UI shows inline.
 * tools\mon-plan-test compiles this file and checks every name against driver\monitor_plan.h.
 *
 * Names (values as apollo_ioctl.h):
 *   altsel 0|1|2              speakers: MAIN, ALT 1, ALT 2          alt1, alt2 0|1    ALT output on/off
 *   alt1trim, alt2trim 0..120 raw = 60 + 2*dB
 *   cue<n>mix 0|1             CUE n source: 0 its own mix, 1 the monitor mix (n = 1..4)
 *   cue<n>mono 0|1, cue<n>mute 0|1
 *   send:<n>:<bus> h          CUE n send of bus (ANALOG k = k-1, TALKBACK = 10): half-dB -120..24, -121 off
 *   pan:<n>:<bus> p           -100..100
 *   [Mix] the monitor mix of a bus (same numbering):
 *   mix:<bus> h               level, half-dB -120..24, -121 off
 *   mixpan:<bus> p            -100..100 (not the talkback: the driver refuses it)
 *   mixmute:<bus>, mixsolo:<bus> 0|1
 *   mixmuteall 0|1            every input and the talkback at once
 *   [Aux] the two stereo aux buses (a = 1 or 2; bus numbered as above; c = the cue 1..4), driver\monitor_plan.h:
 *   auxsend:<a>:<bus> h       the bus's send to AUX a, half-dB -120..24, -121 off
 *   auxpan:<a>:<bus> p        its pan, -100..100
 *   auxpost:<a>:<bus> 0|1     0 pre-fader (default), 1 post-fader (follows the bus's monitor-mix fader, mute, solo)
 *   auxret:<a> h              AUX a's return into the monitor mix, half-dB, -121 off
 *   auxmute:<a> 0|1           its mute
 *   auxcue:<a>:<c> h          AUX a's return into CUE c, half-dB, -121 off (the driver refuses a cue the unit lacks)
 *   auxcuepan:<a>:<c> p       its pan
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once
#include <string>
#include <cwchar>

/* "<n>:<bus>" after a prefix: n 1..4, bus 0..31. */
static bool MonCueBusName(const std::wstring &n, size_t at, ULONG &cue, ULONG &bus)
{
    const wchar_t *s = n.c_str() + at;
    wchar_t *end = nullptr;

    if (at >= n.size() || *s < L'1' || *s > L'4' || s[1] != L':' || s[2] < L'0' || s[2] > L'9') {
        return false;
    }
    cue = (ULONG)(*s - L'1');
    unsigned long b = wcstoul(s + 2, &end, 10);
    if (end == nullptr || *end != L'\0' || b > 31 || end - (s + 2) > 2) {
        return false;
    }
    bus = (ULONG)b;
    return true;
}

/* [Mix] "<bus>" after a prefix: 0..31, one or two digits. */
static bool MonBusName(const std::wstring &n, size_t at, ULONG &bus)
{
    const wchar_t *s = n.c_str() + at;
    wchar_t *end = nullptr;

    if (at >= n.size() || *s < L'0' || *s > L'9') {
        return false;
    }
    unsigned long b = wcstoul(s, &end, 10);
    if (end == nullptr || *end != L'\0' || b > 31 || end - s > 2) {
        return false;
    }
    bus = (ULONG)b;
    return true;
}

/* [Aux] "<a>" or "<a>:<k>" after a prefix: a 1..2, k 0..31 (Max), one or two digits. */
static bool MonAuxName(const std::wstring &n, size_t at, bool second, ULONG max, ULONG &aux, ULONG &k)
{
    const wchar_t *s = n.c_str() + at;
    wchar_t *end = nullptr;

    if (at >= n.size() || (*s != L'1' && *s != L'2')) {
        return false;
    }
    aux = (ULONG)(*s - L'1');
    k = 0;
    if (!second) {
        return s[1] == L'\0';
    }
    if (s[1] != L':' || s[2] < L'0' || s[2] > L'9') {
        return false;
    }
    unsigned long v = wcstoul(s + 2, &end, 10);
    if (end == nullptr || *end != L'\0' || v > max || end - (s + 2) > 2) {
        return false;
    }
    k = (ULONG)v;
    return true;
}

static bool MonFeatureParam(const std::wstring &n, ULONG &p)
{
    static const struct { const wchar_t *n; ULONG p; } map[] = {
        { L"altsel", APOLLO_MON_ALT_SELECT },     { L"alt1", APOLLO_MON_ALT1_ENABLE },
        { L"alt2", APOLLO_MON_ALT2_ENABLE },      { L"alt1trim", APOLLO_MON_ALT1_TRIM },
        { L"alt2trim", APOLLO_MON_ALT2_TRIM },
        { L"cue1mix", APOLLO_MON_CUE1_MIX },      { L"cue1mono", APOLLO_MON_CUE1_MONO },
        { L"cue2mix", APOLLO_MON_CUE2_MIX },      { L"cue2mono", APOLLO_MON_CUE2_MONO },
        { L"cue3mix", APOLLO_MON_CUE3_MIX },      { L"cue3mono", APOLLO_MON_CUE3_MONO },
        { L"cue4mix", APOLLO_MON_CUE4_MIX },      { L"cue4mono", APOLLO_MON_CUE4_MONO },
        { L"cue1mute", APOLLO_MON_CUE_MUTE(0) },  { L"cue2mute", APOLLO_MON_CUE_MUTE(1) },
        { L"cue3mute", APOLLO_MON_CUE_MUTE(2) },  { L"cue4mute", APOLLO_MON_CUE_MUTE(3) },
    };
    ULONG cue = 0, bus = 0;

    for (auto &m : map) {
        if (n == m.n) {
            p = m.p;
            return true;
        }
    }
    if (n.compare(0, 5, L"send:") == 0 && MonCueBusName(n, 5, cue, bus)) {
        p = APOLLO_MON_CUE_SEND(cue, bus);
        return true;
    }
    if (n.compare(0, 4, L"pan:") == 0 && MonCueBusName(n, 4, cue, bus)) {
        p = APOLLO_MON_CUE_PAN(cue, bus);
        return true;
    }
    /* [Mix] the monitor mix */
    if (n == L"mixmuteall") {
        p = APOLLO_MON_MIX_MUTE_ALL;
        return true;
    }
    static const struct { const wchar_t *prefix; size_t len; ULONG base; } mix[] = {
        { L"mix:", 4, APOLLO_MON_MIX_LEVEL(0) },      { L"mixpan:", 7, APOLLO_MON_MIX_PAN(0) },
        { L"mixmute:", 8, APOLLO_MON_MIX_MUTE(0) },   { L"mixsolo:", 8, APOLLO_MON_MIX_SOLO(0) },
    };
    for (auto &m : mix) {
        if (n.compare(0, m.len, m.prefix) == 0 && MonBusName(n, m.len, bus)) {
            p = m.base | bus;
            return true;
        }
    }
    /* [Aux] the aux buses; longest prefixes first ("auxcuepan:" before "auxcue:") */
    static const struct { const wchar_t *prefix; size_t len; int kind; bool second; } aux[] = {
        { L"auxcuepan:", 10, 6, true }, { L"auxcue:", 7, 5, true }, { L"auxsend:", 8, 0, true },
        { L"auxpan:", 7, 1, true },     { L"auxpost:", 8, 2, true }, { L"auxret:", 7, 3, false },
        { L"auxmute:", 8, 4, false },
    };
    for (auto &m : aux) {
        ULONG a = 0, k = 0;
        if (n.compare(0, m.len, m.prefix) == 0 && MonAuxName(n, m.len, m.second, m.kind >= 5 ? 4 : 31, a, k)) {
            if (m.kind >= 5) {
                if (k < 1) return false;
                k -= 1;                         /* cues are 1-based in names, 0-based in codes */
            }
            switch (m.kind) {
            case 0: p = APOLLO_MON_AUX_SEND(a, k); break;
            case 1: p = APOLLO_MON_AUX_PAN(a, k); break;
            case 2: p = APOLLO_MON_AUX_POST(a, k); break;
            case 3: p = APOLLO_MON_AUX_RETURN(a); break;
            case 4: p = APOLLO_MON_AUX_MUTE(a); break;
            case 5: p = APOLLO_MON_AUX_CUE(a, k); break;
            default: p = APOLLO_MON_AUX_CUE_PAN(a, k); break;
            }
            return true;
        }
    }
    return false;
}

/* What the host refuses on the x8p before the driver sees it, or nullptr. */
static const wchar_t *MonHostRefusalX8p(ULONG param)
{
    if (param == APOLLO_MON_HP1_LEVEL) {
        return L"Not available on the Apollo x8p: its headphone levels are analog knobs";
    }
    return nullptr;
}
