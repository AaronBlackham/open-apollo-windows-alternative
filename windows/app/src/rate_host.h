/*
 * rate_host.h - [Rate] Apollo Control's host commands for the sample rate (include\apollo_rate.h; the
 * UI contract is docs\apollo-control-units.md 1.6). The page posts {"cmd":"rate-...", ...} as a string
 * (like the agg-* and corr-* commands) and gets JSON back. Kept out of main.cpp, which only routes
 * "rate-*" here and joins the worker at exit (two marked hunks), so the UI rework merges without
 * touching it.
 *
 *   rate-get {unit?}                  -> one {"type":"rate",...} per unit (every unit on apollo.sys
 *                                        when "unit" is left out)
 *   rate-set {unit, rate, combined?}  -> {"type":"rate-result",...,"pending":true} at once; the change
 *                                        runs on a thread of its own (2-3 s per unit, the driver's
 *                                        SET_RATE), then {"type":"rate-result",...,"pending":false,"ok"}
 *                                        and a fresh "rate" for every unit
 *
 * Each command opens a short-lived read-only handle on the unit (GET_RATE and SET_RATE are user level,
 * like the monitor controls), so the worker's slots are never involved. The unit id is the "units"
 * message's (the driver's unit key: HW_UID as 16 hex digits, or SN<serial>; corr_host.h OpenUnit).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include "..\..\include\apollo_rate.h"

namespace ratehost {

using Post = void (*)(const std::string &Utf8Json);

static std::thread       g_worker;          /* the rate-set under way (UI thread starts and joins it) */
static std::atomic<bool> g_busy{ false };

static inline bool Owns(const std::wstring &Cmd)
{
    return Cmd.rfind(L"rate-", 0) == 0;
}

static inline std::string Q(const std::string &s)
{
    return corrhost::Q(s);
}

static inline std::string Rates(ULONG Mask)
{
    static const unsigned hz[7] = { 0, 44100, 48000, 88200, 96000, 176400, 192000 };
    std::string j = "[";
    for (int i = 1; i < 7; i++) {
        if (Mask & (1ul << i)) {
            if (j.size() > 1) j += ",";
            j += std::to_string(hz[i]);
        }
    }
    return j + "]";
}

/* APOLLO_RATE_ERR_* as the short code the UI keys its wording on (docs 1.6). */
static inline const char *Code(ULONG e)
{
    switch (e) {
    case APOLLO_RATE_ERR_NONE:          return "";
    case APOLLO_RATE_ERR_RATE:          return "rate";
    case APOLLO_RATE_ERR_UNSUPPORTED:   return "unsupported";
    case APOLLO_RATE_ERR_NOT_READY:     return "not-ready";
    case APOLLO_RATE_ERR_ASIO:          return "asio";
    case APOLLO_RATE_ERR_COMBINED:      return "combined";
    case APOLLO_RATE_ERR_COMBINED_ASIO: return "combined-asio";
    case APOLLO_RATE_ERR_BUSY:          return "busy";
    case APOLLO_RATE_ERR_LOCK:          return "lock";
    case APOLLO_RATE_ERR_HARDWARE:      return "hardware";
    case APOLLO_RATE_ERR_NO_COMBINED:   return "no-combined";
    case APOLLO_RATE_ERR_PARTIAL:       return "partial";
    default:                            return "driver";
    }
}

static inline std::string Bool(bool b)
{
    return b ? "true" : "false";
}

static inline std::string WhyOf(const APOLLO_RATE_INFO &r)
{
    return std::string(r.Why, strnlen(r.Why, sizeof(r.Why)));
}

/* {"type":"rate",...} from a GET_RATE / SET_RATE answer. */
static inline std::string InfoJson(const std::string &Id, const APOLLO_RATE_INFO &r)
{
    std::string j = "{\"type\":\"rate\",\"unit\":" + Q(Id) + ",\"present\":true";
    j += ",\"rate\":" + std::to_string(r.Rate);
    j += ",\"locked\":" + Bool((r.Flags & APOLLO_RATE_F_LOCKED) != 0);
    j += ",\"changing\":" + Bool((r.Flags & APOLLO_RATE_F_CHANGING) != 0);
    j += ",\"offered\":" + Rates(r.Offered);
    j += ",\"allowed\":" + Rates(r.Allowed);
    j += ",\"stored\":" + std::to_string(r.Stored);
    j += ",\"asio\":" + Bool((r.Flags & APOLLO_RATE_F_ASIO_OPEN) != 0);
    j += ",\"streams\":" + Bool((r.Flags & APOLLO_RATE_F_STREAMS) != 0);
    j += ",\"plugins48k\":" + Bool((r.Flags & APOLLO_RATE_F_PLUGINS_48K) != 0);
    j += ",\"correctionMismatch\":" + Bool((r.Flags & APOLLO_RATE_F_CORR_MISMATCH) != 0);
    j += ",\"stripsRebuilt\":" + Bool((r.Flags & APOLLO_RATE_F_STRIPS_REBUILT) != 0);
    j += ",\"combined\":{\"member\":" + Bool((r.Flags & APOLLO_RATE_F_COMBINED) != 0) +
         ",\"rate\":" + std::to_string(r.CombinedRate) + ",\"allowed\":" + Rates(r.CombinedAllowed) +
         ",\"units\":" + std::to_string(r.CombinedUnits) + ",\"stored\":" + std::to_string(r.CombinedStored) +
         ",\"asio\":" + Bool((r.Flags & APOLLO_RATE_F_COMBINED_ASIO) != 0) +
         ",\"mismatch\":" + Bool((r.Flags & APOLLO_RATE_F_COMBINED_MISMATCH) != 0) + "}";
    j += ",\"last\":{\"code\":" + Q(Code(r.Result)) + ",\"why\":" + Q(WhyOf(r)) + ",\"ms\":" + std::to_string(r.LastMs) +
         ",\"changes\":" + std::to_string(r.Changes) + "}";
    j += ",\"frames\":{\"frameCfg\":" + std::to_string(r.FrameCfg) + ",\"lead\":" + std::to_string(r.LeadFrames) +
         ",\"guard\":" + std::to_string(r.GuardFrames) + ",\"adc\":" + std::to_string(r.AdcFrames) +
         /* [Lat] the device side of the reported latencies (0 from an older driver) */
         ",\"inPath\":" + std::to_string(r.InPathFrames) + ",\"dac\":" + std::to_string(r.DacFrames) + "}";
    return j + "}";
}

static inline std::string Absent(const std::string &Id, DWORD Err)
{
    std::string why = Err == ERROR_INVALID_FUNCTION ? "this apollo.sys runs at 48 kHz only (no rate change in this build)"
                                                    : corrhost::ErrText(Err);
    return "{\"type\":\"rate\",\"unit\":" + Q(Id) + ",\"present\":" + Bool(Err != ERROR_FILE_NOT_FOUND) +
           ",\"error\":" + std::to_string(Err) + ",\"detail\":" + Q(why) + "}";
}

static inline std::string StateMsg(const std::string &Id)
{
    APOLLO_INFO info{};
    APOLLO_RATE_INFO r{};
    DWORD err = 0, ret = 0;
    HANDLE h = corrhost::OpenUnit(Id, info, err);

    if (h == INVALID_HANDLE_VALUE) {
        return Absent(Id, err ? err : ERROR_FILE_NOT_FOUND);
    }
    BOOL ok = DeviceIoControl(h, IOCTL_APOLLO_GET_RATE, nullptr, 0, &r, sizeof(r), &ret, nullptr);
    err = ok ? 0 : GetLastError();
    CloseHandle(h);
    if (!ok || ret < sizeof(r) || r.Version != APOLLO_RATE_VERSION) {
        return Absent(Id, ok ? ERROR_INVALID_DATA : err);
    }
    return InfoJson(Id, r);
}

/* Every unit's id on apollo.sys now (the "units" message's ids). */
static inline std::vector<std::string> UnitIds()
{
    std::vector<APOLLO_UNIT_ENTRY> units(APOLLO_SELECT_MAX);
    std::vector<std::string> ids;
    int n = ApolloListUnits(units.data(), APOLLO_SELECT_MAX, nullptr, 0);

    for (int i = 0; i < n; i++) {
        const APOLLO_UNIT_ENTRY &e = units[(size_t)i];
        if (e.Error != 0) continue;
        if (ApolloInfoHasUnit(&e.Info, e.InfoBytes) && e.Info.UnitKey[0] != 0 &&
            e.Info.UnitKey[sizeof(e.Info.UnitKey) - 1] == 0) {
            ids.push_back(e.Info.UnitKey);
        } else {
            ids.push_back("SN" + std::string(e.Info.Serial, strnlen(e.Info.Serial, sizeof(e.Info.Serial))));
        }
    }
    return ids;
}

static inline std::string Result(const std::string &Id, ULONG Rate, bool Combined, bool Pending, bool Ok,
                                 const std::string &Code, const std::string &Why, ULONG Ms)
{
    return "{\"type\":\"rate-result\",\"unit\":" + Q(Id) + ",\"rate\":" + std::to_string(Rate) + ",\"combined\":" +
           Bool(Combined) + ",\"pending\":" + Bool(Pending) + (Pending ? "" : ",\"ok\":" + Bool(Ok)) + ",\"code\":" +
           Q(Code) + ",\"why\":" + Q(Why) + ",\"ms\":" + std::to_string(Ms) + "}";
}

/* The change itself (worker thread). */
static inline void SetWork(std::string Id, ULONG Rate, bool Combined, Post post)
{
    APOLLO_INFO info{};
    APOLLO_RATE_SET q{};
    APOLLO_RATE_INFO r{};
    DWORD err = 0, ret = 0;
    ULONGLONG t0 = GetTickCount64();
    HANDLE h = corrhost::OpenUnit(Id, info, err);

    if (h == INVALID_HANDLE_VALUE) {
        post(Result(Id, Rate, Combined, false, false, "absent", corrhost::ErrText(err ? err : ERROR_FILE_NOT_FOUND), 0));
    } else {
        q.Version = APOLLO_RATE_VERSION;
        q.Rate = Rate;
        q.Flags = Combined ? APOLLO_RATE_SET_COMBINED : 0;
        BOOL ok = DeviceIoControl(h, IOCTL_APOLLO_SET_RATE, &q, sizeof(q), &r, sizeof(r), &ret, nullptr);
        err = ok ? 0 : GetLastError();
        CloseHandle(h);
        ULONG ms = (ULONG)(GetTickCount64() - t0);
        if (!ok || ret < sizeof(r)) {
            post(Result(Id, Rate, Combined, false, false, err == ERROR_INVALID_FUNCTION ? "driver" : "error",
                        err == ERROR_INVALID_FUNCTION ? "this apollo.sys runs at 48 kHz only (no rate change in this build)"
                                                      : corrhost::ErrText(err), ms));
        } else {
            post(Result(Id, Rate, Combined, false, r.Result == APOLLO_RATE_ERR_NONE, Code(r.Result), WhyOf(r), ms));
        }
    }
    for (const std::string &u : UnitIds()) {
        post(StateMsg(u));                      /* every unit: a combined change moved them all */
    }
    g_busy = false;
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
    if (cmd == "rate-get") {
        if (!id.empty()) {
            post(StateMsg(id));
        } else {
            for (const std::string &u : UnitIds()) post(StateMsg(u));
        }
    } else if (cmd == "rate-set") {
        ULONG rate = (ULONG)m.Num("rate", 0);
        const aggjson::Value *c = m.Get("combined");
        bool combined = c && c->kind == aggjson::Value::Bool && c->b;
        bool expected = false;

        if (id.empty() || rate == 0) {
            post(Result(id, rate, combined, false, false, "rate", "rate-set needs a unit and a rate", 0));
            return;
        }
        if (!g_busy.compare_exchange_strong(expected, true)) {
            post(Result(id, rate, combined, false, false, "busy", "another rate change is under way", 0));
            return;
        }
        if (g_worker.joinable()) {
            g_worker.join();                    /* the last one has finished (g_busy was false) */
        }
        char d[64];
        snprintf(d, sizeof(d), "Changing to %.1f kHz", rate / 1000.0);
        post(Result(id, rate, combined, true, false, "", d, 0));
        g_worker = std::thread(SetWork, id, rate, combined, post);
    }
}

/* App exit: wait for a change under way (the driver finishes it either way). */
static inline void Stop()
{
    if (g_worker.joinable()) {
        g_worker.join();
    }
}

} /* namespace ratehost */
