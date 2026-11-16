/*
 * params_host.h - [Params] Apollo Control's read of the per-unit driver Parameters (include\apollo_params.h): what each
 * unit's start resolved, where each value came from, the model default and what Units\<key> holds now. Read-only here:
 * SET_PARAM is administrators only (apollo-diag param, elevated), and Apollo Control runs unelevated.
 *
 *   params-get {unit?}   -> {"type":"params","unit":id,"present":true,"key":"...","resolved":true,
 *                            "entries":[{"name","value","default","stored"|null,"src":"D|U|M|G|X","hex","pending"}]}
 *                           per unit (every unit without "unit"); an older driver: "present":true,"error":1
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <memory>
#include <string>
#include "..\..\include\apollo_params.h"

namespace paramshost {

using Post = void (*)(const std::string &Utf8Json);
using corrhost::Q;

static inline bool Owns(const std::wstring &Cmd)
{
    return Cmd.rfind(L"params-", 0) == 0;
}

static inline std::string StateMsg(const std::string &Id)
{
    APOLLO_INFO info{};
    auto p = std::make_unique<APOLLO_PARAMS_INFO>();
    DWORD err = 0, ret = 0;
    HANDLE h = corrhost::OpenUnit(Id, info, err);
    std::string j = "{\"type\":\"params\",\"unit\":" + Q(Id);

    if (h == INVALID_HANDLE_VALUE) {
        return j + ",\"present\":false,\"error\":" + std::to_string(err ? err : ERROR_FILE_NOT_FOUND) + "}";
    }
    BOOL ok = DeviceIoControl(h, IOCTL_APOLLO_GET_PARAMS, nullptr, 0, p.get(), sizeof(*p), &ret, nullptr);
    err = ok ? 0 : GetLastError();
    CloseHandle(h);
    if (!ok || ret < sizeof(*p) || p->Version != APOLLO_PARAMS_VERSION) {
        return j + ",\"present\":true,\"error\":" + std::to_string(ok ? ERROR_INVALID_DATA : err) +
               ",\"detail\":" + Q(ok ? "driver and app are out of step" : corrhost::ErrText(err)) + "}";
    }
    static const char src[] = "DUMGX";
    j += ",\"present\":true,\"key\":" + Q(corrhost::Text(p->UnitKey, sizeof(p->UnitKey))) + ",\"resolved\":" +
         (p->Resolved ? "true" : "false") + ",\"entries\":[";
    for (ULONG i = 0; i < p->Count && i < APOLLO_PARAMS_MAX; i++) {
        const APOLLO_PARAM_ENTRY &e = p->Entry[i];
        if (i) j += ",";
        j += "{\"name\":" + Q(corrhost::Text(e.Name, APOLLO_PARAM_NAME_LEN)) + ",\"value\":" + std::to_string(e.Value) +
             ",\"default\":" + std::to_string(e.Default) +
             ",\"stored\":" + ((e.Flags & APOLLO_PARAM_F_STORED) ? std::to_string(e.Stored) : std::string("null")) +
             ",\"src\":\"" + std::string(1, e.Source < 5 ? src[e.Source] : '?') + "\"" +
             ",\"hex\":" + ((e.Flags & APOLLO_PARAM_F_HEX) ? "true" : "false") +
             ",\"pending\":" + ((e.Flags & APOLLO_PARAM_F_PENDING) ? "true" : "false") + "}";
    }
    return j + "]}";
}

static inline void Handle(const std::string &Message, Post post)
{
    aggjson::Value m;
    aggjson::Parser p(Message);
    if (!p.Parse(m) || m.kind != aggjson::Value::Object) {
        return;
    }
    std::string cmd = m.Str("cmd"), id = m.Str("unit");
    if (cmd == "params-get") {
        if (!id.empty()) {
            post(StateMsg(id));
        } else {
            for (const std::string &u : ratehost::UnitIds()) post(StateMsg(u));
        }
    }
}

}   /* namespace paramshost */
