/*
 * clock_host.h - [Clock] Apollo Control's host side of the clock source, the digital formats, the front panel and
 * the monitor link (include\apollo_clock.h; the UI contract is docs\apollo-control-units.md 1.10). The page posts
 * {"cmd":"clock-...", ...} as a string (like rate-*); main.cpp routes "clock-", "digital-" and "panel-" here and
 * calls PollUnit from the worker's per-unit pass.
 *
 *   clock-get {unit?}                         -> {"type":"clock",...} per unit (every unit without "unit")
 *   clock-set {unit, source, try?, hold?}     -> {"type":"clock-result",...,"pending":true} at once; the change runs on
 *                                                a thread of its own (1-4 s), then the final clock-result and a fresh
 *                                                "clock" and "rate" for that unit
 *   digital-set {unit, what, value, try?}     -> clock-result (pending, then final) and a fresh "clock"
 *   panel-get {unit?}                         -> {"type":"panel",...} per unit
 *   panel-link {monitor, level?, mute?, dim?, mono?, talkDim?}
 *                                             -> {"type":"panel-link",...} and a "panel" for every unit
 *   unsolicited (PollUnit, the worker thread): "panel" whenever a unit's front panel changed (the knob, a button, a
 *                                             host write coming back), "clock" when the clock state changed (at most
 *                                             once a second)
 *
 * Every command opens a short-lived read-only handle on the unit (all four IOCTLs are user level), as rate_host.h.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include "..\..\include\apollo_clock.h"

namespace clockhost {

using Post = void (*)(const std::string &Utf8Json);

static std::thread       g_worker;          /* the clock-set / digital-set under way (UI thread starts and joins it) */
static std::atomic<bool> g_busy{ false };

static inline bool Owns(const std::wstring &Cmd)
{
    return Cmd.rfind(L"clock-", 0) == 0 || Cmd.rfind(L"digital-", 0) == 0 || Cmd.rfind(L"panel-", 0) == 0;
}

using corrhost::Q;
using corrhost::Bool;
using corrhost::UnitIoctl;
using ratehost::Rates;

/* ------------------------------------------------------------------ */
/* Names                                                               */
/* ------------------------------------------------------------------ */

static inline const char *SourceName(ULONG s)
{
    switch (s) {
    case APOLLO_CLOCK_SRC_INTERNAL:  return "internal";
    case APOLLO_CLOCK_SRC_SPDIF:     return "spdif";
    case APOLLO_CLOCK_SRC_ADAT:      return "adat";
    case APOLLO_CLOCK_SRC_WORDCLOCK: return "wordclock";
    case 0x0A:                       return "peer";
    default:                         return "?";
    }
}

static inline bool SourceOf(const std::string &n, ULONG &s)
{
    if (n == "internal") { s = APOLLO_CLOCK_SRC_INTERNAL; return true; }
    if (n == "spdif")    { s = APOLLO_CLOCK_SRC_SPDIF; return true; }
    if (n == "adat")     { s = APOLLO_CLOCK_SRC_ADAT; return true; }
    if (n == "wordclock") { s = APOLLO_CLOCK_SRC_WORDCLOCK; return true; }
    return false;
}

static inline std::string Sources(ULONG Mask)
{
    static const ULONG order[5] = { APOLLO_CLOCK_SRC_INTERNAL, APOLLO_CLOCK_SRC_SPDIF, APOLLO_CLOCK_SRC_ADAT,
                                    APOLLO_CLOCK_SRC_WORDCLOCK, 0x0A };
    std::string j = "[";
    for (ULONG s : order) {
        if (Mask & (1ul << s)) {
            if (j.size() > 1) j += ",";
            j += Q(SourceName(s));
        }
    }
    return j + "]";
}

static inline std::string DigHas(ULONG m)
{
    std::string j = "[";
    auto add = [&](ULONG bit, const char *n) {
        if (m & bit) {
            if (j.size() > 1) j += ",";
            j += Q(n);
        }
    };
    add(APOLLO_CLOCK_DIG_OPT_IN, "optical-in");
    add(APOLLO_CLOCK_DIG_OPT_OUT, "optical-out");
    add(APOLLO_CLOCK_DIG_COAX, "coax");
    add(APOLLO_CLOCK_DIG_SRC, "src");
    return j + "]";
}

/* APOLLO_CLOCK_ERR_* as the short code the UI keys its wording on (docs 1.10). */
static inline const char *Code(ULONG e)
{
    switch (e) {
    case APOLLO_CLOCK_ERR_NONE:         return "";
    case APOLLO_CLOCK_ERR_INVALID:      return "invalid";
    case APOLLO_CLOCK_ERR_NOT_HERE:     return "not-here";
    case APOLLO_CLOCK_ERR_NOT_OFFERED:  return "not-offered";
    case APOLLO_CLOCK_ERR_UNPROVEN:     return "unproven";
    case APOLLO_CLOCK_ERR_RATE:         return "rate";
    case APOLLO_CLOCK_ERR_PORT:         return "port";
    case APOLLO_CLOCK_ERR_SRC:          return "src";
    case APOLLO_CLOCK_ERR_NOT_READY:    return "not-ready";
    case APOLLO_CLOCK_ERR_BUSY:         return "busy";
    case APOLLO_CLOCK_ERR_LOCK:         return "lock";
    case APOLLO_CLOCK_ERR_HARDWARE:     return "hardware";
    default:                            return "driver";
    }
}

static inline std::string WhyOf(const APOLLO_CLOCK_INFO &c)
{
    return corrhost::Text(c.Why, sizeof(c.Why));
}

/* ------------------------------------------------------------------ */
/* JSON                                                                */
/* ------------------------------------------------------------------ */

/* {"type":"clock",...} from a GET_CLOCK / SET_CLOCK answer. Measured goes last (PollUnit compares the rest). */
static inline std::string ClockJson(const std::string &Id, const APOLLO_CLOCK_INFO &c, bool WithMeasured = true)
{
    bool known = (c.Flags & APOLLO_CLOCK_F_DIG_KNOWN) != 0;
    auto fmt = [&](ULONG has, ULONG adat) -> std::string {
        if (!(c.Digital & has)) return "null";
        return Q((known && (c.Flags & adat)) ? "adat" : "spdif");
    };
    std::string j = "{\"type\":\"clock\",\"unit\":" + Q(Id) + ",\"present\":true";
    j += ",\"source\":" + Q(SourceName(c.Source)) + ",\"wanted\":" + Q(SourceName(c.Wanted));
    j += ",\"locked\":" + Bool((c.Flags & APOLLO_CLOCK_F_LOCKED) != 0);
    j += ",\"changing\":" + Bool((c.Flags & APOLLO_CLOCK_F_CHANGING) != 0);
    j += ",\"fellBack\":" + Bool((c.Flags & APOLLO_CLOCK_F_FELL_BACK) != 0);
    j += ",\"hold\":" + Bool((c.Flags & APOLLO_CLOCK_F_HOLD) != 0);
    j += ",\"reapply\":" + Bool((c.Flags & APOLLO_CLOCK_F_REAPPLY) != 0);
    j += ",\"sources\":" + Sources(c.Sources) + ",\"offered\":" + Sources(c.Offered) + ",\"usable\":" + Sources(c.Usable) +
         ",\"proven\":" + Sources(c.ProvenSources);
    j += ",\"rate\":" + std::to_string(c.Rate) + ",\"ratesOnSource\":" + Rates(c.RatesOnSource);
    j += ",\"registers\":{\"clockCfg\":" + std::to_string(c.ClockCfg) + ",\"rateInfo\":" + std::to_string(c.RateInfo) +
         ",\"clockInfo\":" + std::to_string(c.ClockInfo) + "}";
    j += ",\"readbackLock\":" + std::string(!(c.Flags & APOLLO_CLOCK_F_RB_LOCK_VALID) ? "null" :
                                             Bool((c.Flags & APOLLO_CLOCK_F_RB_LOCKED) != 0));
    j += ",\"combined\":{\"member\":" + Bool((c.Flags & APOLLO_CLOCK_F_COMBINED) != 0) + ",\"timeline\":" +
         Bool((c.Flags & APOLLO_CLOCK_F_TIMELINE) != 0) + "}";
    j += ",\"digital\":{\"has\":" + DigHas(c.Digital) + ",\"proven\":" + DigHas(c.ProvenDigital) + ",\"known\":" + Bool(known) +
         ",\"opticalIn\":" + fmt(APOLLO_CLOCK_DIG_OPT_IN, APOLLO_CLOCK_F_IN_ADAT) + ",\"opticalOut\":" +
         fmt(APOLLO_CLOCK_DIG_OPT_OUT, APOLLO_CLOCK_F_OUT_ADAT) + ",\"coax\":" + Bool((c.Digital & APOLLO_CLOCK_DIG_COAX) != 0) +
         ",\"src\":" + std::string(!(c.Digital & APOLLO_CLOCK_DIG_SRC) ? "null" : Bool((c.Flags & APOLLO_CLOCK_F_SRC) != 0)) +
         ",\"channels\":{\"adatIn\":" + std::to_string(c.AdatIn) + ",\"adatOut\":" + std::to_string(c.AdatOut) +
         ",\"spdifIn\":" + std::to_string(c.SpdifIn) + ",\"spdifOut\":" + std::to_string(c.SpdifOut) + "}}";
    j += ",\"last\":{\"code\":" + Q(Code(c.Result)) + ",\"why\":" + Q(WhyOf(c)) + ",\"ms\":" + std::to_string(c.LastMs) +
         ",\"changes\":" + std::to_string(c.Changes) + ",\"fallbacks\":" + std::to_string(c.Fallbacks) +
         ",\"lockLosses\":" + std::to_string(c.LockLosses) + "}";
    if (WithMeasured) {
        char m[48];
        snprintf(m, sizeof(m), "%lu.%03lu", c.MeasuredMilliHz / 1000, c.MeasuredMilliHz % 1000);
        j += ",\"measuredHz\":" + std::string(c.MeasuredMilliHz ? m : "null");
    }
    return j + "}";
}

static inline const char *FieldName(ULONG f)
{
    static const char *const n[11] = { "?", "level", "hp", "mute", "mono", "dim", "alt", "talkback", "knob", "preamp", "lock" };
    return f < 11 ? n[f] : "?";
}

static inline const char *OriginName(ULONG o)
{
    static const char *const n[4] = { "panel", "host", "link", "unit" };
    return o < 4 ? n[o] : "?";
}

static inline std::string Db(ULONG Raw)
{
    char b[24];
    snprintf(b, sizeof(b), "%.1f", ((double)Raw - 192.0) / 2.0);
    return b;
}

/* {"type":"panel",...} from a GET_PANEL / SET_PANEL answer. */
static inline std::string PanelJson(const std::string &Id, const APOLLO_PANEL_INFO &p)
{
    static const char *const knob[3] = { "preamp", "monitor", "hp" };
    bool valid = (p.Flags & APOLLO_PANEL_INFO_VALID) != 0;
    std::string j = "{\"type\":\"panel\",\"unit\":" + Q(Id) + ",\"present\":true,\"valid\":" + Bool(valid);
    j += ",\"seq\":" + std::to_string(p.Seq);
    if (valid) {
        j += ",\"knob\":" + Q(p.Knob < 3 ? knob[p.Knob] : "?") + ",\"preamp\":" + std::to_string(p.Preamp + 1);
        j += ",\"level\":" + std::to_string(p.Level) + ",\"levelDb\":" + Db(p.Level);
        j += ",\"hp\":" + std::to_string(p.HpLevel) + ",\"hpDb\":" + Db(p.HpLevel);
        j += ",\"mute\":" + Bool(p.Mute != 0) + ",\"dim\":" + Bool(p.Dim != 0) + ",\"mono\":" + Bool(p.Mono != 0);
        j += ",\"alt\":" + std::to_string(p.Alt) + ",\"talkback\":" + Bool(p.Talk != 0);
        j += ",\"lock\":" + std::string(p.Lock < 0 ? "null" : Bool(p.Lock != 0));
    }
    j += ",\"role\":" + std::string((p.Flags & APOLLO_PANEL_INFO_MONITOR_UNIT) ? "\"monitor\"" :
                                    (p.Flags & APOLLO_PANEL_INFO_KNOB_UNIT) ? "\"knob\"" : "null");
    j += ",\"talkDimmed\":" + Bool((p.Flags & APOLLO_PANEL_INFO_TALK_DIMMED) != 0) + ",\"unitDims\":" +
         Bool((p.Flags & APOLLO_PANEL_INFO_UNIT_DIMS) != 0) + ",\"pending\":" + Bool((p.Flags & APOLLO_PANEL_INFO_LINK_PENDING) != 0);
    j += ",\"talkbackChannel\":{\"recChannels\":" + std::to_string(p.TalkRecChannels) + ",\"slots\":";
    j += (p.TalkSlot[0] == APOLLO_PANEL_NO_SLOT) ? std::string("null") :
         "[" + std::to_string(p.TalkSlot[0]) + "," + std::to_string(p.TalkSlot[1]) + "]";
    j += ",\"strip\":" + Bool(p.TalkStrip != 0) + "}";
    j += ",\"link\":{\"on\":" + Bool(p.LinkFlags != 0) + ",\"monitor\":" +
         (p.LinkFlags != 0 ? Q(aggjson::UidText(p.LinkMonitorUid)) : std::string("null")) +
         ",\"level\":" + Bool((p.LinkFlags & APOLLO_PANEL_LINK_LEVEL) != 0) + ",\"mute\":" +
         Bool((p.LinkFlags & APOLLO_PANEL_LINK_MUTE) != 0) + ",\"dim\":" + Bool((p.LinkFlags & APOLLO_PANEL_LINK_DIM) != 0) +
         ",\"mono\":" + Bool((p.LinkFlags & APOLLO_PANEL_LINK_MONO) != 0) + ",\"talkDim\":" +
         Bool((p.LinkFlags & APOLLO_PANEL_LINK_TALK_DIM) != 0) + ",\"applied\":" + std::to_string(p.LinkApplied) +
         ",\"busy\":" + std::to_string(p.LinkBusy) + ",\"dropped\":" + std::to_string(p.LinkDropped) + "}";
    j += ",\"events\":[";
    for (ULONG i = 0; i < p.EventCount && i < APOLLO_PANEL_EVENTS; i++) {
        const APOLLO_PANEL_EVENT &e = p.Event[i];
        if (i) j += ",";
        j += "{\"seq\":" + std::to_string(e.Seq) + ",\"field\":" + Q(FieldName(e.Field)) + ",\"value\":" +
             std::to_string(e.Value) + ",\"origin\":" + Q(OriginName(e.Origin)) + ",\"ms\":" + std::to_string(e.TimeMs) + "}";
    }
    return j + "]}";
}

static inline std::string Absent(const char *Type, const std::string &Id, DWORD Err)
{
    std::string why = Err == ERROR_INVALID_FUNCTION ? "this apollo.sys has no clock and front-panel control (an older build)"
                                                    : corrhost::ErrText(Err);
    return std::string("{\"type\":\"") + Type + "\",\"unit\":" + Q(Id) + ",\"present\":" + Bool(Err != ERROR_FILE_NOT_FOUND) +
           ",\"error\":" + std::to_string(Err) + ",\"detail\":" + Q(why) + "}";
}

static inline std::string ClockMsg(const std::string &Id)
{
    APOLLO_CLOCK_INFO c{};
    DWORD ret = 0, err = UnitIoctl(Id, IOCTL_APOLLO_GET_CLOCK, nullptr, 0, &c, sizeof(c), ret);

    if (err != 0 || ret < sizeof(c) || c.Version != APOLLO_CLOCK_VERSION) {
        return Absent("clock", Id, err ? err : ERROR_INVALID_DATA);
    }
    return ClockJson(Id, c);
}

static inline std::string PanelMsg(const std::string &Id)
{
    APOLLO_PANEL_INFO p{};
    DWORD ret = 0, err = UnitIoctl(Id, IOCTL_APOLLO_GET_PANEL, nullptr, 0, &p, sizeof(p), ret);

    if (err != 0 || ret < sizeof(p) || p.Version != APOLLO_CLOCK_VERSION) {
        return Absent("panel", Id, err ? err : ERROR_INVALID_DATA);
    }
    return PanelJson(Id, p);
}

static inline std::string Result(const std::string &Id, const std::string &What, const std::string &Value, bool Pending,
                                 bool Ok, const std::string &Code, const std::string &Why, ULONG Ms)
{
    return "{\"type\":\"clock-result\",\"unit\":" + Q(Id) + ",\"what\":" + Q(What) + ",\"value\":" + Q(Value) +
           ",\"pending\":" + Bool(Pending) + (Pending ? "" : ",\"ok\":" + Bool(Ok)) + ",\"code\":" + Q(Code) + ",\"why\":" +
           Q(Why) + ",\"ms\":" + std::to_string(Ms) + "}";
}

/* The change itself (worker thread). */
static inline void SetWork(std::string Id, std::string What, std::string Value, APOLLO_CLOCK_SET q, Post post)
{
    APOLLO_CLOCK_INFO c{};
    DWORD ret = 0;
    ULONGLONG t0 = GetTickCount64();
    DWORD err = UnitIoctl(Id, IOCTL_APOLLO_SET_CLOCK, &q, sizeof(q), &c, sizeof(c), ret);
    ULONG ms = (ULONG)(GetTickCount64() - t0);

    if (err != 0 || ret < sizeof(c)) {
        post(Result(Id, What, Value, false, false, err == ERROR_INVALID_FUNCTION ? "driver" : "error",
                    err == ERROR_INVALID_FUNCTION ? "this apollo.sys has no clock control (an older build)"
                                                  : corrhost::ErrText(err ? err : ERROR_INVALID_DATA), ms));
    } else {
        post(Result(Id, What, Value, false, c.Result == APOLLO_CLOCK_ERR_NONE, Code(c.Result), WhyOf(c), ms));
        post(ClockJson(Id, c));
    }
    post(ratehost::StateMsg(Id));               /* the rates a S/PDIF or ADAT clock allows */
    g_busy = false;
}

static inline bool Flag(const aggjson::Value &m, const char *k)
{
    const aggjson::Value *v = m.Get(k);
    return v && v->kind == aggjson::Value::Bool && v->b;
}

/* One UI command (the UI thread). */
static inline void Handle(const std::string &Message, Post post)
{
    aggjson::Value m;
    aggjson::Parser p(Message);
    if (!p.Parse(m) || m.kind != aggjson::Value::Object) {
        return;
    }
    std::string cmd = m.Str("cmd"), id = m.Str("unit");
    if (cmd == "clock-get" || cmd == "panel-get") {
        bool clock = (cmd == "clock-get");
        if (!id.empty()) {
            post(clock ? ClockMsg(id) : PanelMsg(id));
        } else {
            for (const std::string &u : ratehost::UnitIds()) post(clock ? ClockMsg(u) : PanelMsg(u));
        }
    } else if (cmd == "clock-set" || cmd == "digital-set") {
        APOLLO_CLOCK_SET q{};
        std::string what = cmd == "clock-set" ? "source" : m.Str("what"), value;
        bool expected = false;

        q.Version = APOLLO_CLOCK_VERSION;
        q.Flags = (Flag(m, "try") ? APOLLO_CLOCK_SET_TRY : 0) | (Flag(m, "hold") ? APOLLO_CLOCK_SET_HOLD : 0);
        if (cmd == "clock-set") {
            value = m.Str("source");
            q.What = APOLLO_CLOCK_WHAT_SOURCE;
            if (!SourceOf(value, q.Value)) {
                post(Result(id, what, value, false, false, "invalid", "source is internal, spdif, adat or wordclock", 0));
                return;
            }
        } else {
            const aggjson::Value *v = m.Get("value");
            q.What = what == "optical-in" ? APOLLO_CLOCK_WHAT_OPT_IN : what == "optical-out" ? APOLLO_CLOCK_WHAT_OPT_OUT :
                     what == "src" ? APOLLO_CLOCK_WHAT_SRC : 0xFFFFFFFFul;
            if (v && v->kind == aggjson::Value::String && (v->s == "adat" || v->s == "spdif") && q.What != APOLLO_CLOCK_WHAT_SRC) {
                value = v->s;
                q.Value = v->s == "adat" ? 1 : 0;
            } else if (v && v->kind == aggjson::Value::Bool && q.What == APOLLO_CLOCK_WHAT_SRC) {
                value = v->b ? "on" : "off";
                q.Value = v->b ? 1 : 0;
            } else {
                post(Result(id, what, value, false, false, "invalid",
                            "digital-set: what optical-in / optical-out with value \"adat\" or \"spdif\", or what src with true/false", 0));
                return;
            }
        }
        if (id.empty()) {
            post(Result(id, what, value, false, false, "invalid", cmd + " needs a unit", 0));
            return;
        }
        if (!g_busy.compare_exchange_strong(expected, true)) {
            post(Result(id, what, value, false, false, "busy", "another clock change is under way", 0));
            return;
        }
        if (g_worker.joinable()) {
            g_worker.join();                    /* the last one has finished (g_busy was false) */
        }
        post(Result(id, what, value, true, false, "", q.What == APOLLO_CLOCK_WHAT_SOURCE ?
                    "Switching the clock (up to 4 s; it goes back to Internal if the source does not lock)" :
                    "Writing the format", 0));
        g_worker = std::thread(SetWork, id, what, value, q, post);
    } else if (cmd == "panel-link") {
        APOLLO_PANEL_LINK l{};
        APOLLO_PANEL_INFO info{};
        DWORD ret = 0, err;
        std::string mon = m.Str("monitor");
        std::vector<std::string> ids = ratehost::UnitIds();

        l.Version = APOLLO_CLOCK_VERSION;
        if (!mon.empty()) {
            if (mon.size() != 16 || mon.find_first_not_of("0123456789ABCDEFabcdef") != std::string::npos) {
                post("{\"type\":\"panel-link\",\"ok\":false,\"code\":\"invalid\",\"why\":" +
                     Q("monitor is a unit id with a HW_UID (16 hex digits)") + "}");
                return;
            }
            l.MonitorUid = strtoull(mon.c_str(), nullptr, 16);
            l.Flags = (Flag(m, "level") ? APOLLO_PANEL_LINK_LEVEL : 0) | (Flag(m, "mute") ? APOLLO_PANEL_LINK_MUTE : 0) |
                      (Flag(m, "dim") ? APOLLO_PANEL_LINK_DIM : 0) | (Flag(m, "mono") ? APOLLO_PANEL_LINK_MONO : 0) |
                      (Flag(m, "talkDim") ? APOLLO_PANEL_LINK_TALK_DIM : 0);
        }
        /* driver-wide: through the monitor unit's handle, else any unit's */
        std::string via = !mon.empty() ? mon : (ids.empty() ? std::string() : ids[0]);
        err = via.empty() ? ERROR_FILE_NOT_FOUND : UnitIoctl(via, IOCTL_APOLLO_SET_PANEL, &l, sizeof(l), &info, sizeof(info), ret);
        post("{\"type\":\"panel-link\",\"ok\":" + Bool(err == 0 && ret >= sizeof(info)) + ",\"code\":" +
             Q(err == 0 ? "" : err == ERROR_INVALID_FUNCTION ? "driver" : "error") + ",\"why\":" +
             Q(err == 0 ? (l.Flags ? "the monitor link is on" : "the monitor link is off") : corrhost::ErrText(err)) +
             ",\"monitor\":" + (l.Flags ? Q(mon) : std::string("null")) + "}");
        for (const std::string &u : ids) post(PanelMsg(u));
    }
}

/* ------------------------------------------------------------------ */
/* The worker's per-unit pass (main.cpp PollSlot)                     */
/* ------------------------------------------------------------------ */

static const int kSlots = 8;
static std::string g_lastPanel[kSlots], g_lastClock[kSlots];
static std::string g_summaryClock[kSlots], g_summaryPanel[kSlots];   /* for the "units" entries (UnitsExtra) */
static ULONGLONG   g_lastClockPost[kSlots];
static bool        g_noDriver[kSlots];
/* The answers the strings above were built from (PollUnit), with the fields their JSON leaves out zeroed */
static std::string       g_rawId[kSlots];
static APOLLO_CLOCK_INFO g_rawClock[kSlots];
static APOLLO_PANEL_INFO g_rawPanel[kSlots];
static bool              g_haveClock[kSlots], g_havePanel[kSlots];

/*
 * Ioctl: (code, out, outLen, &ret) -> ok, on the worker's own handle of the slot. Posts "panel" when the front panel
 * changed and "clock" when the clock state changed (the measured rate alone: at most every 5 s). A driver without
 * these IOCTLs is asked once per slot occupancy (Forget clears it). The JSON is built only when the driver's answer
 * moved (the measured rate and the panel's ReadbackSeq are not compared: the compared JSON has neither), so a steady
 * unit costs the two IOCTLs per pass and no strings.
 */
template <typename F>
static inline void PollUnit(int Slot, const APOLLO_INFO &Info, DWORD Bytes, F Ioctl, Post post, ULONGLONG Now)
{
    if (Slot < 0 || Slot >= kSlots || g_noDriver[Slot]) {
        return;
    }
    std::string id = corrhost::KeyOf(Info, Bytes);
    APOLLO_PANEL_INFO p{};
    APOLLO_CLOCK_INFO c{};
    DWORD ret = 0;

    if (!Ioctl((DWORD)IOCTL_APOLLO_GET_CLOCK, &c, (DWORD)sizeof(c), &ret) || ret < sizeof(c) ||
        c.Version != APOLLO_CLOCK_VERSION) {
        g_noDriver[Slot] = true;                /* an older apollo.sys: nothing to poll */
        return;
    }
    if (id != g_rawId[Slot]) {
        g_rawId[Slot] = id;
        g_haveClock[Slot] = g_havePanel[Slot] = false;
    }
    APOLLO_CLOCK_INFO ck = c;
    ck.MeasuredMilliHz = 0;
    bool moved = false;
    if (!g_haveClock[Slot] || memcmp(&ck, &g_rawClock[Slot], sizeof(ck)) != 0) {
        g_rawClock[Slot] = ck;
        g_haveClock[Slot] = true;
        std::string cj = ClockJson(id, c, false);
        g_summaryClock[Slot] = ",\"clock\":{\"source\":" + Q(SourceName(c.Source)) + ",\"wanted\":" + Q(SourceName(c.Wanted)) +
                               ",\"locked\":" + Bool((c.Flags & APOLLO_CLOCK_F_LOCKED) != 0) + ",\"fellBack\":" +
                               Bool((c.Flags & APOLLO_CLOCK_F_FELL_BACK) != 0) + "}";
        moved = cj != g_lastClock[Slot];
        if (moved) {
            g_lastClock[Slot] = std::move(cj);
        }
    }
    if (moved || Now - g_lastClockPost[Slot] >= 5000) {
        g_lastClockPost[Slot] = Now;
        post(ClockJson(id, c));
    }
    if (Ioctl((DWORD)IOCTL_APOLLO_GET_PANEL, &p, (DWORD)sizeof(p), &ret) && ret >= sizeof(p) &&
        p.Version == APOLLO_CLOCK_VERSION) {
        APOLLO_PANEL_INFO pk = p;
        pk.ReadbackSeq = 0;
        if (g_havePanel[Slot] && memcmp(&pk, &g_rawPanel[Slot], sizeof(pk)) == 0) {
            return;
        }
        g_rawPanel[Slot] = pk;
        g_havePanel[Slot] = true;
        static const char *const knob[3] = { "preamp", "monitor", "hp" };
        std::string pj = PanelJson(id, p);
        g_summaryPanel[Slot] = ",\"panel\":{\"knob\":" +
                               std::string((p.Flags & APOLLO_PANEL_INFO_VALID) && p.Knob < 3 ? Q(knob[p.Knob]) : "null") +
                               ",\"role\":" + std::string((p.Flags & APOLLO_PANEL_INFO_MONITOR_UNIT) ? "\"monitor\"" :
                                                           (p.Flags & APOLLO_PANEL_INFO_KNOB_UNIT) ? "\"knob\"" : "null") + "}";
        if (pj != g_lastPanel[Slot]) {
            g_lastPanel[Slot] = pj;
            post(pj);
        }
    }
}

/* A slot was dropped or reopened: its next poll posts afresh. */
static inline void Forget(int Slot)
{
    if (Slot >= 0 && Slot < kSlots) {
        g_lastPanel[Slot].clear();
        g_lastClock[Slot].clear();
        g_lastClockPost[Slot] = 0;
        g_noDriver[Slot] = false;
        g_summaryClock[Slot].clear();
        g_summaryPanel[Slot].clear();
        g_rawId[Slot].clear();
        g_haveClock[Slot] = g_havePanel[Slot] = false;
    }
}

/*
 * The "units" entry's [Clock] fields for a slot (main.cpp UnitsJson): ,"clock":{"source","wanted","locked","fellBack"}
 * and ,"panel":{"knob","role"} once the worker has polled them (nothing for a driver without GET_CLOCK). The list is
 * sent again whenever they change, as for any other field.
 */
static inline std::string UnitsExtra(int Slot)
{
    if (Slot < 0 || Slot >= kSlots) {
        return std::string();
    }
    return g_summaryClock[Slot] + g_summaryPanel[Slot];
}

/* App exit: wait for a change under way (the driver finishes it either way). */
static inline void Stop()
{
    if (g_worker.joinable()) {
        g_worker.join();
    }
}

} /* namespace clockhost */
