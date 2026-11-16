/*
 * endpoints_host.h - [Endpoints] Apollo Control's host commands for the Windows endpoint set: which of a
 * unit's endpoints exist in Windows (include\apollo_ioctl.h GET_ENDPOINTS v2, driver\endpoint_plan.h). The
 * UI contract is docs\apollo-control-units.md 1.7. The page posts {"cmd":"endpoints-...", ...} as a string
 * (like the rate-* and corr-* commands) and gets JSON back. main.cpp routes "endpoints-*" here and
 * runs the elevated step.
 *
 *   endpoints-get {unit?}                    -> one {"type":"endpoints",...} per unit (every unit on
 *                                               apollo.sys when "unit" is left out)
 *   endpoints-set {unit, rows:[i..]|mask:n|default:true}
 *                                            -> {"type":"endpoints-result",...,"pending":true} at once; the
 *                                               registry write runs elevated (one UAC prompt) on a thread
 *                                               of its own, then {"type":"endpoints-result",...,"pending":
 *                                               false,"ok"} and a fresh "endpoints" for that unit
 *
 * The set is a registry value (admin): Parameters\Units\<unit key>\Endpoints for a unit's own set,
 * Parameters\Aggregate\Endpoints for the combined set (the x8p hosts it when Aggregate\EndpointSet is 1).
 * The driver reads it when the unit starts, so a change shows in Windows after the unit's next start (power
 * cycle, driver update, or disable/enable): every reply says so ("restartNeeded").
 * The elevated step is this exe again: apollo-control.exe --endpoints-admin <unit key|combined>
 * <hex mask|default>; it checks both arguments itself and writes (or deletes) that one value.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <atomic>
#include <cwctype>
#include <string>
#include <thread>
#include <vector>

namespace ephost {

using Post = void (*)(const std::string &Utf8Json);

static std::atomic<bool> g_busy{ false };

static const wchar_t *const kParams = L"SYSTEM\\CurrentControlSet\\Services\\apollo\\Parameters";
static const wchar_t *const kValue = L"Endpoints";

static inline bool Owns(const std::wstring &Cmd)
{
    return Cmd.rfind(L"endpoints-", 0) == 0;
}

using corrhost::Q;
using corrhost::Bool;
using corrhost::Text;

static inline const char *ModelName(ULONG m)
{
    return m == APOLLO_INFO_MODEL_X8P ? "x8p" : (m == APOLLO_INFO_MODEL_TWINX ? "twinx" : "");
}

/* A unit key the driver uses for Parameters\Units\<key>: 16 hex digits, or SN + 1..30 letters/digits. */
static inline bool KeyOk(const std::wstring &k)
{
    if (k.size() == 16) {
        for (wchar_t c : k) {
            if (!iswxdigit(c)) return false;
        }
        return true;
    }
    if (k.size() < 3 || k.size() > 32 || k[0] != L'S' || k[1] != L'N') {
        return false;
    }
    for (size_t i = 2; i < k.size(); i++) {
        if (!iswalnum(k[i])) return false;
    }
    return true;
}

/* A DWORD value under Parameters\<sub> (HKLM is readable by users); false = absent. */
static inline bool ReadDword(const std::wstring &Sub, const wchar_t *Name, DWORD &Out)
{
    std::wstring path = std::wstring(kParams) + (Sub.empty() ? L"" : L"\\" + Sub);
    DWORD type = 0, size = sizeof(Out);
    return RegGetValueW(HKEY_LOCAL_MACHINE, path.c_str(), Name, RRF_RT_REG_DWORD, &type, &Out, &size) == ERROR_SUCCESS;
}

/* GET_ENDPOINTS v2 on a unit: every row (a v1 driver: up to 16). */
struct Rows {
    APOLLO_ENDPOINTS head{};
    std::vector<APOLLO_ENDPOINT_INFO> row;
};

static inline bool Query(HANDLE h, Rows &R, DWORD &Err)
{
    std::vector<BYTE> buf(FIELD_OFFSET(APOLLO_ENDPOINTS, Endpoint) + APOLLO_ENDPOINTS_ROWS_MAX * sizeof(APOLLO_ENDPOINT_INFO));
    APOLLO_ENDPOINTS_QUERY q{};
    DWORD ret = 0;

    if (buf.size() < sizeof(APOLLO_ENDPOINTS)) buf.resize(sizeof(APOLLO_ENDPOINTS));
    if (!DeviceIoControl(h, IOCTL_APOLLO_GET_ENDPOINTS, &q, sizeof(q), buf.data(), (DWORD)buf.size(), &ret, nullptr)) {
        Err = GetLastError();
        return false;
    }
    if (ret < sizeof(APOLLO_ENDPOINTS)) {
        Err = ERROR_INVALID_DATA;
        return false;
    }
    memcpy(&R.head, buf.data(), sizeof(R.head));
    size_t got = (ret - FIELD_OFFSET(APOLLO_ENDPOINTS, Endpoint)) / sizeof(APOLLO_ENDPOINT_INFO);
    size_t n = std::min<size_t>({ got, (size_t)R.head.Count, (size_t)APOLLO_ENDPOINTS_ROWS_MAX });
    R.row.resize(n);
    if (n) memcpy(R.row.data(), buf.data() + FIELD_OFFSET(APOLLO_ENDPOINTS, Endpoint), n * sizeof(APOLLO_ENDPOINT_INFO));
    Err = 0;
    return true;
}

/*
 * {"type":"endpoints",...} for one unit: its rows (what Windows has), the mask in force, the default set,
 * and what the registry holds now (the next start's set).
 */
static inline std::string StateMsg(const std::string &Id)
{
    APOLLO_INFO info{};
    DWORD err = 0;
    HANDLE h = corrhost::OpenUnit(Id, info, err);
    Rows r;

    if (h == INVALID_HANDLE_VALUE) {
        err = err ? err : ERROR_FILE_NOT_FOUND;
        return "{\"type\":\"endpoints\",\"unit\":" + Q(Id) + ",\"present\":" + Bool(err != ERROR_FILE_NOT_FOUND) +
               ",\"error\":" + std::to_string(err) + ",\"detail\":" + Q(corrhost::ErrText(err)) + "}";
    }
    bool ok = Query(h, r, err);
    CloseHandle(h);
    if (!ok) {
        return "{\"type\":\"endpoints\",\"unit\":" + Q(Id) + ",\"present\":true,\"error\":" + std::to_string(err) +
               ",\"detail\":" + Q(corrhost::ErrText(err)) + "}";
    }
    bool combined = !r.row.empty() && (r.row[0].Flags & APOLLO_EP_FLAG_COMBINED) != 0;
    DWORD epset = 0;
    bool combinedOn = ReadDword(L"Aggregate", L"EndpointSet", epset) && epset == 1;
    std::string set = combined ? "combined" : (r.row.empty() ? "none" : "standalone");
    ULONG def = 0;
    for (size_t i = 0; i < r.row.size() && i < 32; i++) {
        if (r.row[i].Flags & APOLLO_EP_FLAG_DEFAULT) def |= 1ul << i;
    }
    /* where the next start reads this set from (circuit.c): the aggregate key, or the unit's chain */
    DWORD v = 0;
    std::string src = "default";
    bool have = false;
    std::wstring wid(Id.begin(), Id.end());
    if (combined) {
        have = ReadDword(L"Aggregate", kValue, v);
        if (have) src = "Aggregate";
    } else if (KeyOk(wid)) {
        if ((have = ReadDword(L"Units\\" + wid, kValue, v))) {
            src = "unit";
        } else if ((have = ReadDword(info.Model == APOLLO_INFO_MODEL_X8P ? L"Models\\22" : L"Models\\99", kValue, v))) {
            src = "model";
        } else if (info.Model == APOLLO_INFO_MODEL_X8P && (have = ReadDword(L"", kValue, v))) {
            src = "global";
        }
    }
    ULONG next = have ? (ULONG)v : def;
    std::string j = "{\"type\":\"endpoints\",\"unit\":" + Q(Id) + ",\"present\":true,\"model\":" +
                    Q(ModelName(info.Model)) + ",\"set\":" + Q(set) + ",\"combinedOn\":" + Bool(combinedOn) +
                    ",\"count\":" + std::to_string(r.head.Count) + ",\"mask\":" + std::to_string(r.head.EnabledMask) +
                    ",\"defaultMask\":" + std::to_string(def) + ",\"stored\":" + (have ? std::to_string(v) : "null") +
                    ",\"storedIn\":" + Q(src) + ",\"next\":" + std::to_string(next) +
                    ",\"restartNeeded\":" + Bool(!r.row.empty() && next != r.head.EnabledMask) +
                    ",\"version\":" + std::to_string(r.head.Version) + ",\"rows\":[";
    for (size_t i = 0; i < r.row.size(); i++) {
        const APOLLO_ENDPOINT_INFO &e = r.row[i];
        ULONG f = e.Flags;
        if (i) j += ",";
        j += "{\"row\":" + std::to_string(i) + ",\"name\":" + Q(Text(e.Name, sizeof(e.Name))) + ",\"circuit\":" +
             Q(Text(e.Circuit, sizeof(e.Circuit))) + ",\"render\":" + Bool(e.Render != 0) + ",\"channels\":" +
             std::to_string(e.Channels) + ",\"model\":" + (r.head.Version >= 2 && e.Model ? Q(ModelName(e.Model)) : "null") +
             ",\"enabled\":" + Bool((f & APOLLO_EP_FLAG_ENABLED) != 0) + ",\"added\":" + Bool((f & APOLLO_EP_FLAG_ADDED) != 0) +
             ",\"default\":" + Bool((f & APOLLO_EP_FLAG_DEFAULT) != 0) + ",\"next\":" + Bool(i < 32 && ((next >> i) & 1) != 0) +
             ",\"open\":" + Bool((f & APOLLO_EP_FLAG_STREAM) != 0) + ",\"running\":" + Bool((f & APOLLO_EP_FLAG_RUNNING) != 0) +
             ",\"noSlot\":" + Bool((f & APOLLO_EP_FLAG_NO_SLOT) != 0) + "}";
    }
    return j + "]}";
}

static inline std::string Result(const std::string &Id, bool Pending, bool Ok, const std::string &Code,
                                 const std::string &Detail)
{
    return "{\"type\":\"endpoints-result\",\"unit\":" + Q(Id) + ",\"pending\":" + Bool(Pending) +
           (Pending ? "" : ",\"ok\":" + Bool(Ok)) + ",\"code\":" + Q(Code) + ",\"detail\":" + Q(Detail) +
           ",\"restartNeeded\":true}";
}

/* The elevated write (worker thread): UAC, then wait for the step (up to 3 minutes for the prompt). */
static inline void SetWork(std::string Id, std::wstring Scope, std::wstring Arg, Post post)
{
    wchar_t exe[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, exe, _countof(exe));
    std::wstring params = L"--endpoints-admin " + Scope + L" " + Arg;
    SHELLEXECUTEINFOW sei{ sizeof(sei) };

    (void)CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = n ? exe : nullptr;
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || sei.hProcess == nullptr) {
        DWORD e = GetLastError();
        post(Result(Id, false, false, e == ERROR_CANCELLED ? "cancelled" : "elevation",
                    e == ERROR_CANCELLED ? "cancelled at the administrator prompt: nothing changed"
                                         : "the elevated step did not start: " + corrhost::ErrText(e)));
    } else {
        DWORD code = 1;
        if (WaitForSingleObject(sei.hProcess, 180000) == WAIT_OBJECT_0) {
            GetExitCodeProcess(sei.hProcess, &code);
        }
        CloseHandle(sei.hProcess);
        post(Result(Id, false, code == 0, code == 0 ? "" : "registry",
                    code == 0 ? "stored; Windows shows the new set after the unit's next start (power cycle, driver "
                                "update, or disable/enable)"
                              : "the registry value could not be written: " + corrhost::ErrText(code)));
    }
    post(StateMsg(Id));
    CoUninitialize();
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
    if (cmd == "endpoints-get") {
        if (!id.empty()) {
            post(StateMsg(id));
        } else {
            for (const std::string &u : ratehost::UnitIds()) post(StateMsg(u));
        }
        return;
    }
    if (cmd != "endpoints-set") {
        return;
    }
    if (id.empty()) {
        post(Result(id, false, false, "unit", "endpoints-set needs a unit"));
        return;
    }
    /* the set this unit hosts now decides the value: combined -> Aggregate, else its own unit key */
    APOLLO_INFO info{};
    DWORD err = 0;
    Rows r;
    HANDLE h = corrhost::OpenUnit(id, info, err);
    if (h == INVALID_HANDLE_VALUE) {
        post(Result(id, false, false, "absent", corrhost::ErrText(err ? err : ERROR_FILE_NOT_FOUND)));
        return;
    }
    bool ok = Query(h, r, err);
    CloseHandle(h);
    if (!ok || r.row.empty()) {
        post(Result(id, false, false, "none", ok ? "this unit has no Windows endpoints of its own now (the combined set "
                                                   "lives on the x8p, or the unit is below Mode 2)"
                                                 : corrhost::ErrText(err)));
        return;
    }
    bool combined = (r.row[0].Flags & APOLLO_EP_FLAG_COMBINED) != 0;
    std::wstring wid(id.begin(), id.end());
    if (!combined && !KeyOk(wid)) {
        post(Result(id, false, false, "unit", "not a unit key"));
        return;
    }
    ULONG all = r.row.size() >= 32 ? 0xFFFFFFFFul : ((1ul << r.row.size()) - 1);
    std::wstring arg;
    const aggjson::Value *dv = m.Get("default"), *rows = m.Get("rows"), *mv = m.Get("mask");
    if (dv && dv->kind == aggjson::Value::Bool && dv->b) {
        arg = L"default";
    } else {
        ULONG mask = 0;
        if (rows && rows->kind == aggjson::Value::Array) {
            for (const aggjson::Value &v : rows->a) {
                if (v.kind == aggjson::Value::Number && v.n >= 0 && v.n < (double)r.row.size()) mask |= 1ul << (ULONG)v.n;
            }
        } else if (mv && mv->kind == aggjson::Value::Number && mv->n >= 0 && mv->n <= 4294967295.0) {
            mask = (ULONG)mv->n;
        } else {
            post(Result(id, false, false, "mask", "endpoints-set needs rows, mask or default"));
            return;
        }
        mask &= all;
        wchar_t b[16];
        swprintf_s(b, L"%lX", mask);
        arg = b;
    }
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) {
        post(Result(id, false, false, "busy", "another endpoint change is waiting for the administrator prompt"));
        return;
    }
    post(Result(id, true, false, "", "asking for administrator rights to store the endpoint set"));
    std::thread(SetWork, id, combined ? std::wstring(L"combined") : wid, arg, post).detach();
}

/*
 * apollo-control.exe --endpoints-admin <unit key|combined> <hex mask|default>: run elevated by SetWork (UAC).
 * Writes Parameters\Units\<key>\Endpoints (or Parameters\Aggregate\Endpoints) as a REG_DWORD, or deletes it
 * for "default". Exit code 0 = done, else the Win32 error.
 */
static inline int AdminMain(const std::wstring &Scope, const std::wstring &Arg)
{
    std::wstring sub;
    if (Scope == L"combined") {
        sub = L"Aggregate";
    } else if (KeyOk(Scope)) {
        sub = L"Units\\" + Scope;
    } else {
        return ERROR_INVALID_PARAMETER;
    }
    bool del = (Arg == L"default");
    DWORD mask = 0;
    if (!del) {
        if (Arg.empty() || Arg.size() > 8) return ERROR_INVALID_PARAMETER;
        for (wchar_t c : Arg) {
            if (!iswxdigit(c)) return ERROR_INVALID_PARAMETER;
        }
        mask = (DWORD)wcstoul(Arg.c_str(), nullptr, 16);
    }
    std::wstring path = std::wstring(kParams) + L"\\" + sub;
    HKEY k = nullptr;
    LSTATUS e = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr);
    if (e != ERROR_SUCCESS) return (int)e;
    if (del) {
        e = RegDeleteValueW(k, kValue);
        if (e == ERROR_FILE_NOT_FOUND) e = ERROR_SUCCESS;
    } else {
        e = RegSetValueExW(k, kValue, 0, REG_DWORD, (const BYTE *)&mask, sizeof(mask));
    }
    RegCloseKey(k);
    return (int)e;
}

} /* namespace ephost */
