/*
 * corr_host.h - Apollo Control's host commands for speaker correction (include\apollo_corr.h): the
 * Speaker correction page (ui\correction.js) posts {"cmd":"corr-...", "unit":"<unit id>", ...} and
 * gets JSON replies. Each command opens a short-lived read-only handle on that unit (the correction
 * IOCTLs are APOLLO_USER, like the monitor controls), so it never disturbs the worker's slots.
 *
 *   corr-check {unit, text}                       the file through the port of validate()
 *                                                 -> {"type":"corr-check","unit","seq","ok","problems":[...],
 *                                                     "unitRate","designed"}
 *   corr-load  {unit, text, slots:{"L":0,...}, enable}
 *                                                 validate again, compile at the unit's rate, LOAD
 *                                                 -> {"type":"corr-result","op":"load",...} then a "corr" state
 *   corr-set   {unit, op:"enable"|"bypass"|"clear", value}
 *                                                 -> {"type":"corr-result","op":...} then a "corr" state
 *   corr-get   {unit}                             -> {"type":"corr","unit","present","state":{...}|null,
 *                                                     "error","detail"}
 *
 * The unit id is the one the "units" message carries: the driver's unit key (HW_UID as 16 hex digits,
 * or SN<serial>). Kept out of main.cpp so the plug-in UI work and this one merge without touching
 * each other; main.cpp only routes "corr-*" here (one marked hunk).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <memory>
#include <string>
#include <vector>
#include "corr_profile.h"

namespace corrhost {

static inline bool Owns(const std::wstring &Cmd)
{
    return Cmd.rfind(L"corr-", 0) == 0;
}

static inline std::string Q(const std::string &s)
{
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

/* The unit with this id: a read-only handle and its GET_INFO, or INVALID_HANDLE_VALUE + a Win32 error. */
static inline HANDLE OpenUnit(const std::string &Id, APOLLO_INFO &Info, DWORD &Err)
{
    std::vector<APOLLO_UNIT_ENTRY> units(APOLLO_SELECT_MAX);
    int n = ApolloListUnits(units.data(), APOLLO_SELECT_MAX, nullptr, 0);

    Err = ERROR_FILE_NOT_FOUND;
    for (int i = 0; i < n; i++) {
        const APOLLO_UNIT_ENTRY &e = units[(size_t)i];
        std::string key;
        if (e.Error != 0) {
            continue;
        }
        if (ApolloInfoHasUnit(&e.Info, e.InfoBytes) && e.Info.UnitKey[0] != 0 &&
            e.Info.UnitKey[sizeof(e.Info.UnitKey) - 1] == 0) {
            key = e.Info.UnitKey;
        } else {
            key = "SN" + std::string(e.Info.Serial, strnlen(e.Info.Serial, sizeof(e.Info.Serial)));
        }
        if (key != Id) {
            continue;
        }
        HANDLE h = CreateFileW(e.Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            Err = GetLastError();
            return h;
        }
        Info = e.Info;
        Err = 0;
        return h;
    }
    return INVALID_HANDLE_VALUE;
}

static inline std::string ErrText(DWORD e)
{
    switch (e) {
    case ERROR_FILE_NOT_FOUND:      return "that unit is not on apollo.sys right now";
    case ERROR_INVALID_FUNCTION:    return "this driver build has no speaker correction (install a newer apollo.sys)";
    case ERROR_BUSY:                return "the driver is busy, try again";
    case ERROR_ACCESS_DENIED:       return "access denied";
    default: {
        char b[48];
        snprintf(b, sizeof(b), "Windows error %lu", e);
        return b;
    }
    }
}

/* {"type":"corr",...} for one unit */
static inline std::string StateMsg(const std::string &Id)
{
    APOLLO_INFO info{};
    DWORD err = 0, ret = 0;
    HANDLE h = OpenUnit(Id, info, err);
    std::string j = "{\"type\":\"corr\",\"unit\":" + Q(Id);

    if (h == INVALID_HANDLE_VALUE) {
        return j + ",\"present\":false,\"state\":null,\"error\":" + std::to_string(err) + ",\"detail\":" + Q(ErrText(err)) + "}";
    }
    auto st = std::make_unique<APOLLO_CORR_STATE>();
    ULONG flags = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_APOLLO_CORR_GET_STATE, &flags, sizeof(flags), st.get(), sizeof(*st), &ret, nullptr);
    err = ok ? 0 : GetLastError();
    CloseHandle(h);
    if (!ok || ret < sizeof(*st) || st->Version != APOLLO_CORR_VERSION) {
        if (ok) err = ERROR_INVALID_DATA;
        return j + ",\"present\":true,\"state\":null,\"error\":" + std::to_string(err) + ",\"detail\":" + Q(ErrText(err)) + "}";
    }
    return j + ",\"present\":true,\"state\":" + corrprof::StateJson(*st) + ",\"error\":0,\"detail\":\"\"}";
}

static inline std::string Result(const std::string &Id, const char *Op, unsigned long Error, const std::string &Detail)
{
    return "{\"type\":\"corr-result\",\"unit\":" + Q(Id) + ",\"op\":" + Q(Op) + ",\"ok\":" + (Error == 0 ? "true" : "false") +
           ",\"error\":" + std::to_string(Error) + ",\"detail\":" + Q(Detail) + "}";
}

/* Every reply to one command, in order (UTF-8 JSON). Message = the UI's JSON command. */
static inline std::vector<std::string> Handle(const std::string &Message)
{
    using namespace corrprof;
    J m;
    std::string perr;
    if (!ParseJson(Message, m, perr) || m.kind != J::Obj) {
        return {};
    }
    const J *cmdv = m.Get("cmd"), *unitv = m.Get("unit"), *seqv = m.Get("seq");
    std::string cmd = cmdv && cmdv->kind == J::Str ? cmdv->s : "", id = unitv && unitv->kind == J::Str ? unitv->s : "";
    std::string seq = seqv && seqv->IsNum() ? std::to_string((long long)seqv->n) : "0";

    if (cmd == "corr-get") {
        return { StateMsg(id) };
    }
    if (cmd == "corr-check" || cmd == "corr-load") {
        const J *tv = m.Get("text");
        J d;
        std::vector<std::string> problems;
        if (tv == nullptr || tv->kind != J::Str) {
            problems.push_back("no file text");
        } else if (!ParseJson(tv->s, d, perr)) {
            problems.push_back("Not JSON: " + perr);
        } else {
            problems = Validate(d);
        }
        APOLLO_INFO info{};
        DWORD err = 0;
        HANDLE h = OpenUnit(id, info, err);
        ULONG rate = (h != INVALID_HANDLE_VALUE && info.SampleRate != 0) ? info.SampleRate : 48000;
        bool designed = false;
        if (problems.empty()) {
            std::string key = std::to_string(rate);
            for (const J &c : d.Get("channels")->a) {
                for (const J &b : c.Get("filters")->a) designed = designed || b.Get("coefficients")->Get(key.c_str()) == nullptr;
            }
        }
        if (cmd == "corr-check") {
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            std::string j = "{\"type\":\"corr-check\",\"unit\":" + Q(id) + ",\"seq\":" + seq + ",\"ok\":" +
                            (problems.empty() ? "true" : "false") + ",\"problems\":[";
            for (size_t i = 0; i < problems.size(); i++) j += (i ? "," : "") + Q(problems[i]);
            j += "],\"unitRate\":" + std::to_string(rate) + ",\"designed\":" + (designed ? "true" : "false") +
                 ",\"summary\":" + Q(problems.empty() ? Summary(d) : std::string()) + "}";
            return { j };
        }
        /* corr-load */
        if (!problems.empty()) {
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            return { Result(id, "load", 1000, problems[0]) };
        }
        if (h == INVALID_HANDLE_VALUE) {
            return { Result(id, "load", 2000 + err, ErrText(err)) };
        }
        std::vector<SlotPick> picks;
        const J *sv = m.Get("slots");
        for (size_t i = 0; sv && sv->kind == J::Obj && i < sv->o.size(); i++) {
            const J &v = sv->o[i].second;
            picks.push_back({ sv->o[i].first, v.IsNum() ? (long)v.n : -1 });
        }
        const J *ev = m.Get("enable");
        auto p = std::make_unique<APOLLO_CORR_PROFILE>();
        std::string cerr;
        if (!Compile(d, rate, picks, ev && ev->kind == J::Bool && ev->b, *p, cerr)) {
            CloseHandle(h);
            return { Result(id, "load", 1001, cerr) };
        }
        APOLLO_CORR_RESULT res{};
        DWORD ret = 0;
        BOOL ok = DeviceIoControl(h, IOCTL_APOLLO_CORR_LOAD, p.get(), sizeof(*p), &res, sizeof(res), &ret, nullptr);
        err = ok ? 0 : GetLastError();
        CloseHandle(h);
        if (!ok) {
            return { Result(id, "load", 2000 + err, ErrText(err)), StateMsg(id) };
        }
        if (res.Error != 0) {
            return { Result(id, "load", res.Error, RefusalText(res.Error, res.Channel, res.Filter)), StateMsg(id) };
        }
        std::string note = res.Stored ? "" : "loaded, but not stored for the next start (no unit key)";
        return { Result(id, "load", 0, note), StateMsg(id) };
    }
    if (cmd == "corr-set") {
        const J *opv = m.Get("op"), *vv = m.Get("value");
        std::string op = opv && opv->kind == J::Str ? opv->s : "";
        APOLLO_CORR_SET s{};
        s.Op = op == "enable" ? APOLLO_CORR_OP_ENABLE : op == "bypass" ? APOLLO_CORR_OP_BYPASS : op == "clear" ? APOLLO_CORR_OP_CLEAR : 0;
        s.Value = Truth(vv, false) ? 1 : 0;
        if (s.Op == 0) {
            return { Result(id, "set", APOLLO_CORR_ERR_OP, "unknown request") };
        }
        APOLLO_INFO info{};
        DWORD err = 0, ret = 0;
        HANDLE h = OpenUnit(id, info, err);
        if (h == INVALID_HANDLE_VALUE) {
            return { Result(id, op.c_str(), 2000 + err, ErrText(err)) };
        }
        APOLLO_CORR_RESULT res{};
        BOOL ok = DeviceIoControl(h, IOCTL_APOLLO_CORR_SET, &s, sizeof(s), &res, sizeof(res), &ret, nullptr);
        err = ok ? 0 : GetLastError();
        CloseHandle(h);
        if (!ok) {
            return { Result(id, op.c_str(), 2000 + err, ErrText(err)), StateMsg(id) };
        }
        return { Result(id, op.c_str(), res.Error, res.Error ? RefusalText(res.Error, 0, 0) : std::string()), StateMsg(id) };
    }
    return {};
}

}   /* namespace corrhost */
