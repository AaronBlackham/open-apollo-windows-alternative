/*
 * params_host.h - [Params] Apollo Control's read of the per-unit driver Parameters (include\apollo_params.h): what each
 * unit's start resolved, where each value came from, the model default and what Units\<key> holds now. [Virtual] One value
 * is also changed from here, elevated like endpoints-set (endpoints_host.h): StripVirtualMask, which VIRTUAL inputs get an
 * Input Strip (docs/STRIPS.md 13), followed by a restart of that unit so its next start builds them.
 *
 *   params-get {unit?}   -> {"type":"params","unit":id,"present":true,"key":"...","resolved":true,
 *                            "entries":[{"name","value","default","stored"|null,"src":"D|U|M|G|X","hex","pending"}]}
 *                           per unit (every unit without "unit"); an older driver: "present":true,"error":1
 *   params-set {unit, name:"StripVirtualMask", value}
 *                        -> {"type":"params-result","unit","name","value","pending":true,"code":"","detail"} at once;
 *                           the SET_PARAM and the restart run elevated (one UAC prompt) on a thread of their own, then
 *                           {"type":"params-result",...,"pending":false,"ok","stored","restarted","code","detail"} and a
 *                           fresh "params" for that unit (once it is back, at most 15 s after the restart)
 *
 * Only the names in kSettable are taken, with the model's bits (x8p VIRTUAL 1-8 = 0xFF, Twin X VIRTUAL 1-4 = 0x0F);
 * anything else is refused here and again by the elevated step. That step is this exe again:
 * apollo-control.exe --params-admin <unit key> <name> <hex value>. It opens the unit read/write, SET_PARAMs the value
 * (the driver writes Units\<key>\<name>; administrators only), closes its handle and restarts the unit the way Device
 * Manager's disable / enable and devcon restart do (SetupAPI DIF_PROPERTYCHANGE, DICS_PROPCHANGE). Something that holds
 * the unit open and refuses the query-remove (an ASIO host, say) leaves the restart to Windows' next reboot: the value is
 * stored either way and the reply says which. The exit code: (what << 24) | Win32 error (or an NTSTATUS's low 24 bits).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <atomic>
#include <cwctype>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <setupapi.h>
#include <devpkey.h>
#include "..\..\include\apollo_params.h"

namespace paramshost {

using Post = void (*)(const std::string &Utf8Json);
using corrhost::Q;
using corrhost::Bool;

static std::atomic<bool> g_busy{ false };

static inline bool Owns(const std::wstring &Cmd)
{
    return Cmd.rfind(L"params-", 0) == 0;
}

/* [Virtual] what params-set may change, and the bits each model takes (0 = not on that model) */
struct Settable {
    const char *Name;
    ULONG       X8p;
    ULONG       TwinX;
};
static const Settable kSettable[] = {
    { "StripVirtualMask", 0xFFul, 0x0Ful },         /* bit n-1 = VIRTUAL n: x8p 1-8, Twin X 1-4 */
};

static inline const Settable *Find(const std::string &Name)
{
    for (const Settable &s : kSettable) {
        if (Name == s.Name) return &s;
    }
    return nullptr;
}

static inline ULONG BitsFor(const Settable &S, ULONG Model)
{
    return Model == APOLLO_INFO_MODEL_X8P ? S.X8p : Model == APOLLO_INFO_MODEL_TWINX ? S.TwinX : 0;
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

/* ------------------------------------------------------------------ [Virtual] params-set */

/* What the elevated step did: its exit code is (What << 24) | detail */
enum AdminWhat : DWORD {
    kDone = 0,          /* stored, the unit restarted */
    kArgs = 1,          /* an argument it does not take */
    kUnit = 2,          /* the unit was not found or would not open read/write: Win32 */
    kSet = 3,           /* SET_PARAM failed: Win32 */
    kRegistry = 4,      /* the driver's registry write failed: NTSTATUS low 24 bits */
    kRestart = 5,       /* stored; the restart failed: Win32 */
    kReboot = 6,        /* stored; Windows put the restart off until a reboot (the unit is held open) */
};

static inline int AdminCode(DWORD What, DWORD Detail)
{
    return (int)((What << 24) | (Detail & 0xFFFFFFul));
}

static inline std::string Result(const std::string &Id, const std::string &Name, ULONG Value, bool Pending, bool Ok,
                                 bool Stored, bool Restarted, const std::string &Code, const std::string &Detail)
{
    return "{\"type\":\"params-result\",\"unit\":" + Q(Id) + ",\"name\":" + Q(Name) + ",\"value\":" + std::to_string(Value) +
           ",\"pending\":" + Bool(Pending) +
           (Pending ? "" : ",\"ok\":" + Bool(Ok) + ",\"stored\":" + Bool(Stored) + ",\"restarted\":" + Bool(Restarted)) +
           ",\"code\":" + Q(Code) + ",\"detail\":" + Q(Detail) + "}";
}

/* The elevated step's exit code as the final params-result. */
static inline std::string Outcome(const std::string &Id, const std::string &Name, ULONG Value, DWORD Exit)
{
    DWORD what = Exit >> 24, d = Exit & 0xFFFFFFul;
    char ns[24];
    switch (what) {
    case kDone:
        return Result(Id, Name, Value, false, true, true, true, "",
                      "stored; the unit restarted (its audio stops for a few seconds) and builds the new set as it starts");
    case kArgs:
        return Result(Id, Name, Value, false, false, false, false, "value", "the administrator step refused the value: nothing changed");
    case kUnit:
        return Result(Id, Name, Value, false, false, false, false, "absent",
                      "the administrator step could not open the unit: " + corrhost::ErrText(d ? d : ERROR_FILE_NOT_FOUND));
    case kSet:
        return Result(Id, Name, Value, false, false, false, false, "driver",
                      d == ERROR_INVALID_FUNCTION ? "this apollo.sys cannot store driver parameters (install a newer driver)"
                      : d == ERROR_INVALID_PARAMETER ? "this apollo.sys does not know " + Name + " (install a newer driver)"
                      : "the driver did not store it: " + corrhost::ErrText(d));
    case kRegistry:
        snprintf(ns, sizeof(ns), "0x%08lX", 0xC0000000ul | d);
        return Result(Id, Name, Value, false, false, false, false, "registry",
                      std::string("the driver could not write the registry value (NTSTATUS ") + ns + ")");
    case kRestart:
        return Result(Id, Name, Value, false, false, true, false, "restart",
                      "stored, but the unit did not restart (" + corrhost::ErrText(d) + "): it applies at the unit's next "
                      "start (power cycle, or disable and enable it in Device Manager)");
    case kReboot:
        return Result(Id, Name, Value, false, false, true, false, "reboot",
                      "stored, but Windows could not restart the unit now: something holds it open (an ASIO host or an "
                      "audio app). Close it and apply again, or power-cycle the unit");
    default:
        return Result(Id, Name, Value, false, false, false, false, "elevation", "the administrator step ended without an answer");
    }
}

/* The elevated step (worker thread): UAC, then wait for it (up to 3 minutes for the prompt and the restart). */
static inline void SetWork(std::string Id, std::string Name, ULONG Value, Post post)
{
    wchar_t exe[MAX_PATH * 4], v[16];
    DWORD n = GetModuleFileNameW(nullptr, exe, _countof(exe));
    swprintf_s(v, L"%lX", Value);
    std::wstring params = L"--params-admin " + std::wstring(Id.begin(), Id.end()) + L" " +
                          std::wstring(Name.begin(), Name.end()) + L" " + v;
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    bool restarted = false;

    (void)CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = n ? exe : nullptr;
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || sei.hProcess == nullptr) {
        DWORD e = GetLastError();
        post(Result(Id, Name, Value, false, false, false, false, e == ERROR_CANCELLED ? "cancelled" : "elevation",
                    e == ERROR_CANCELLED ? "cancelled at the administrator prompt: nothing changed"
                                         : "the administrator step did not start: " + corrhost::ErrText(e)));
    } else {
        DWORD code = 0xFF000000ul;
        if (WaitForSingleObject(sei.hProcess, 180000) == WAIT_OBJECT_0) {
            GetExitCodeProcess(sei.hProcess, &code);
        }
        CloseHandle(sei.hProcess);
        restarted = code == 0;
        post(Outcome(Id, Name, Value, code));
    }
    /* the unit was restarted: its interface goes and comes back; answer once it is there again (or after 15 s) */
    for (int i = 0; restarted && i < 60; i++) {
        APOLLO_INFO info{};
        DWORD err = 0;
        HANDLE h = corrhost::OpenUnit(Id, info, err);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            break;
        }
        Sleep(250);
    }
    post(StateMsg(Id));
    CoUninitialize();
    g_busy = false;
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
        return;
    }
    if (cmd != "params-set") {
        return;
    }
    std::string name = m.Str("name");
    const aggjson::Value *vv = m.Get("value");
    ULONG value = (vv && vv->kind == aggjson::Value::Number && vv->n >= 0 && vv->n <= 4294967295.0) ? (ULONG)vv->n : 0;
    const Settable *s = Find(name);
    if (id.empty()) {
        post(Result(id, name, value, false, false, false, false, "unit", "params-set needs a unit"));
        return;
    }
    if (!s) {
        post(Result(id, name, value, false, false, false, false, "name", "Apollo Control changes only StripVirtualMask"));
        return;
    }
    if (!vv || vv->kind != aggjson::Value::Number || vv->n < 0 || vv->n > 4294967295.0 || (double)value != vv->n) {
        post(Result(id, name, value, false, false, false, false, "value", name + " needs a whole number"));
        return;
    }
    APOLLO_INFO info{};
    DWORD err = 0;
    HANDLE h = corrhost::OpenUnit(id, info, err);
    if (h == INVALID_HANDLE_VALUE) {
        post(Result(id, name, value, false, false, false, false, "absent", corrhost::ErrText(err ? err : ERROR_FILE_NOT_FOUND)));
        return;
    }
    CloseHandle(h);
    ULONG bits = BitsFor(*s, info.Model);
    std::wstring wid(id.begin(), id.end());
    if (bits == 0 || !ephost::KeyOk(wid)) {
        post(Result(id, name, value, false, false, false, false, "unit",
                    bits == 0 ? "this unit's model takes no " + name : "the unit has no key the driver stores values under"));
        return;
    }
    if (value & ~bits) {
        char b[96];
        snprintf(b, sizeof(b), "%s takes bits 0x%lX on this model: nothing changed", name.c_str(), bits);
        post(Result(id, name, value, false, false, false, false, "value", b));
        return;
    }
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) {
        post(Result(id, name, value, false, false, false, false, "busy", "another change is waiting for its administrator prompt"));
        return;
    }
    post(Result(id, name, value, true, false, false, false, "",
                "asking for administrator rights to store " + name + " and restart the unit"));
    std::thread(SetWork, id, name, value, post).detach();
}

/* ------------------------------------------------------------------ [Virtual] the elevated step */

/* Restart the device behind an Apollo interface path: DIF_PROPERTYCHANGE / DICS_PROPCHANGE (stop, then start). */
static inline int RestartUnit(const wchar_t *Path)
{
    wchar_t inst[MAX_DEVICE_ID_LEN + 1] = {};
    DEVPROPTYPE type = 0;
    ULONG size = sizeof(inst) - sizeof(wchar_t);
    CONFIGRET cr = CM_Get_Device_Interface_PropertyW(Path, &DEVPKEY_Device_InstanceId, &type, (PBYTE)inst, &size, 0);
    if (cr != CR_SUCCESS || type != DEVPROP_TYPE_STRING) {
        return AdminCode(kRestart, cr != CR_SUCCESS ? CM_MapCrToWin32Err(cr, ERROR_NOT_FOUND) : ERROR_INVALID_DATA);
    }
    HDEVINFO set = SetupDiCreateDeviceInfoList(nullptr, nullptr);
    if (set == INVALID_HANDLE_VALUE) {
        return AdminCode(kRestart, GetLastError());
    }
    SP_DEVINFO_DATA dev{};
    dev.cbSize = sizeof(dev);
    SP_PROPCHANGE_PARAMS pc{};
    pc.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    pc.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    pc.StateChange = DICS_PROPCHANGE;
    pc.Scope = DICS_FLAG_CONFIGSPECIFIC;
    pc.HwProfile = 0;
    if (!SetupDiOpenDeviceInfoW(set, inst, nullptr, 0, &dev) ||
        !SetupDiSetClassInstallParamsW(set, &dev, &pc.ClassInstallHeader, sizeof(pc)) ||
        !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &dev)) {
        DWORD e = GetLastError();
        SetupDiDestroyDeviceInfoList(set);
        return AdminCode(kRestart, e ? e : ERROR_GEN_FAILURE);
    }
    SP_DEVINSTALL_PARAMS_W ip{};
    ip.cbSize = sizeof(ip);
    bool later = SetupDiGetDeviceInstallParamsW(set, &dev, &ip) && (ip.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)) != 0;
    SetupDiDestroyDeviceInfoList(set);
    return later ? AdminCode(kReboot, 0) : AdminCode(kDone, 0);
}

/*
 * apollo-control.exe --params-admin <unit key> <name> <hex value>: run elevated by SetWork (UAC). Checks every argument
 * itself (a name of kSettable, the unit's model bits), SET_PARAMs the value on that unit and restarts it.
 */
static inline int AdminMain(const std::wstring &Key, const std::wstring &Name, const std::wstring &Arg)
{
    std::string name;
    for (wchar_t c : Name) {
        if (c < 0x20 || c > 0x7E) return AdminCode(kArgs, ERROR_INVALID_PARAMETER);
        name += (char)c;
    }
    const Settable *s = Find(name);
    if (!s || !ephost::KeyOk(Key) || Arg.empty() || Arg.size() > 8) {
        return AdminCode(kArgs, ERROR_INVALID_PARAMETER);
    }
    for (wchar_t c : Arg) {
        if (!iswxdigit(c)) return AdminCode(kArgs, ERROR_INVALID_PARAMETER);
    }
    ULONG value = wcstoul(Arg.c_str(), nullptr, 16);
    std::string key;
    for (wchar_t c : Key) key += (char)c;          /* KeyOk: hex digits, or SN + letters and digits */

    std::vector<APOLLO_UNIT_ENTRY> units(APOLLO_SELECT_MAX);
    int n = ApolloListUnits(units.data(), APOLLO_SELECT_MAX, nullptr, 0);
    const APOLLO_UNIT_ENTRY *e = nullptr;
    for (int i = 0; i < n; i++) {
        if (units[(size_t)i].Error == 0 && corrhost::KeyOf(units[(size_t)i].Info, units[(size_t)i].InfoBytes) == key) {
            e = &units[(size_t)i];
            break;
        }
    }
    if (e == nullptr) {
        return AdminCode(kUnit, ERROR_FILE_NOT_FOUND);
    }
    ULONG bits = BitsFor(*s, e->Info.Model);
    if (bits == 0 || (value & ~bits) != 0) {
        return AdminCode(kArgs, ERROR_INVALID_PARAMETER);
    }
    HANDLE h = CreateFileW(e->Path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return AdminCode(kUnit, GetLastError());
    }
    APOLLO_PARAM_SET q{};
    q.Version = APOLLO_PARAMS_VERSION;
    strncpy_s(q.Name, sizeof(q.Name), s->Name, _TRUNCATE);
    q.Value = value;
    auto info = std::make_unique<APOLLO_PARAMS_INFO>();
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_APOLLO_SET_PARAM, &q, sizeof(q), info.get(), sizeof(*info), &ret, nullptr);
    DWORD err = ok ? 0 : GetLastError();
    CloseHandle(h);                                 /* before the restart: this handle must not hold the unit */
    if (!ok) {
        return AdminCode(kSet, err ? err : ERROR_GEN_FAILURE);
    }
    if (info->Result != 0) {
        return AdminCode(kRegistry, info->Result);
    }
    return RestartUnit(e->Path);
}

}   /* namespace paramshost */
