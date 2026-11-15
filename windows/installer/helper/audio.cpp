/*
 * Windows audio: the default endpoints (saved before PnP touches the x8p and put back afterwards,
 * because Windows may make a new Apollo endpoint the default), and everything that holds the x8p
 * (ACX streams and the ASIO client on the x8p's own interface, audio sessions on "(Apollo x8p"
 * endpoints, our own tools). A Twin X never makes the x8p busy: it has no endpoints on our driver
 * and its own device interface is skipped. Ported from tools\phase1\audio-helpers.ps1.
 * Also the read-only GET_INFO query 'check' uses for each unit on our driver.
 */
#include "common.h"
#include "apollo_ioctl_min.h"
#include <cfgmgr32.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <propidl.h>
#include <tlhelp32.h>
#include <stddef.h>
#include <string.h>
#include <wctype.h>

/* PKEY_Device_FriendlyName, defined here so no other header needs INITGUID. */
static const PROPERTYKEY kFriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

/* Undocumented but long-stable (Windows 7..11): the interface Sound settings uses to set the
   default device. Only SetDefaultEndpoint is called; the slots before it keep the vtable order. */
struct __declspec(uuid("F8679F50-850A-41CF-9C72-430F290290C8")) IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, void *, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, INT, const PROPERTYKEY &, PROPVARIANT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, INT, const PROPERTYKEY &, PROPVARIANT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
static const CLSID kPolicyConfigClient =
    { 0x870af99c, 0x171d, 0x4f9e, { 0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9 } };

static const wchar_t *kFlowNames[] = { L"playback", L"recording" };
static const wchar_t *kRoleNames[] = { L"Console", L"Multimedia", L"Communications" };

template <class T> static void Release(T *&p)
{
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}

struct Endpoint {
    int          Flow;
    std::wstring Id;
    std::wstring Name;
};

static IMMDeviceEnumerator *Enumerator()
{
    IMMDeviceEnumerator *en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                (void **)&en)))
        return nullptr;
    return en;
}

static std::wstring DeviceName(IMMDevice *d)
{
    IPropertyStore *ps = nullptr;
    std::wstring s;
    if (FAILED(d->OpenPropertyStore(STGM_READ, &ps)) || ps == nullptr)
        return s;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(ps->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal != nullptr)
        s = v.pwszVal;
    PropVariantClear(&v);
    ps->Release();
    return s;
}

static std::wstring DeviceId(IMMDevice *d)
{
    LPWSTR id = nullptr;
    std::wstring s;
    if (SUCCEEDED(d->GetId(&id)) && id != nullptr)
        s = id;
    if (id != nullptr)
        CoTaskMemFree(id);
    return s;
}

/* Active playback and recording endpoints. */
static std::vector<Endpoint> ActiveEndpoints(IMMDeviceEnumerator *en)
{
    std::vector<Endpoint> r;
    for (int flow = 0; flow < 2; flow++) {
        IMMDeviceCollection *col = nullptr;
        UINT n = 0;
        if (FAILED(en->EnumAudioEndpoints(flow == 0 ? eRender : eCapture, DEVICE_STATE_ACTIVE, &col)) || col == nullptr)
            continue;
        if (SUCCEEDED(col->GetCount(&n))) {
            for (UINT i = 0; i < n; i++) {
                IMMDevice *d = nullptr;
                if (SUCCEEDED(col->Item(i, &d)) && d != nullptr) {
                    r.push_back({ flow, DeviceId(d), DeviceName(d) });
                    d->Release();
                }
            }
        }
        col->Release();
    }
    return r;
}

static std::wstring DefaultId(IMMDeviceEnumerator *en, int flow, int role)
{
    IMMDevice *d = nullptr;
    std::wstring id;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(flow == 0 ? eRender : eCapture, (ERole)role, &d)) && d != nullptr) {
        id = DeviceId(d);
        d->Release();
    }
    return id;
}

static std::wstring NameOf(const std::vector<Endpoint> &eps, const std::wstring &id)
{
    for (auto &e : eps)
        if (e.Id == id)
            return e.Name;
    return std::wstring();
}

/* ------------------------------------------------------------------ */
/* defaults save / restore                                             */
/* ------------------------------------------------------------------ */

int CmdDefaultsSave(const std::wstring &file)
{
    IMMDeviceEnumerator *en = Enumerator();
    if (en == nullptr) {
        Err(L"Windows audio is not available (MMDeviceEnumerator).");
        return EXIT_ERROR;
    }
    std::vector<Endpoint> eps = ActiveEndpoints(en);
    std::wstring text = L"apollo-helper audio defaults v1\n";
    Out(L"Default audio devices now:");
    for (int flow = 0; flow < 2; flow++) {
        for (int role = 0; role < 3; role++) {
            std::wstring id = DefaultId(en, flow, role);
            std::wstring name = NameOf(eps, id);
            text += Fmt(L"%d\t%d\t%s\t%s\n", flow, role, id.c_str(), name.c_str());
            Out(L"  %-9s %-14s %s", kFlowNames[flow], kRoleNames[role], id.empty() ? L"(none)" : name.c_str());
        }
    }
    en->Release();

    std::string u = ToUtf8(text);
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD done = 0;
    if (h == INVALID_HANDLE_VALUE) {
        Err(L"Cannot write %s: %s", file.c_str(), ErrText(GetLastError()).c_str());
        return EXIT_ERROR;
    }
    BOOL ok = WriteFile(h, u.data(), (DWORD)u.size(), &done, nullptr);
    CloseHandle(h);
    if (!ok || done != u.size()) {
        Err(L"Cannot write %s", file.c_str());
        return EXIT_ERROR;
    }
    return EXIT_OK;
}

struct Saved {
    int          Flow;
    int          Role;
    std::wstring Id;
    std::wstring Name;
};

static bool LoadSaved(const std::wstring &file, std::vector<Saved> &out)
{
    std::vector<BYTE> data;
    if (!ReadFileBytes(file, data))
        return false;
    std::wstring text = FromUtf8(std::string(data.begin(), data.end()));
    size_t pos = 0;
    bool header = false;
    while (pos < text.size()) {
        size_t eol = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, eol == std::wstring::npos ? std::wstring::npos : eol - pos);
        pos = eol == std::wstring::npos ? text.size() : eol + 1;
        if (!line.empty() && line.back() == L'\r')
            line.pop_back();
        if (!header) {
            header = line == L"apollo-helper audio defaults v1";
            if (!header)
                return false;
            continue;
        }
        std::vector<std::wstring> f;
        size_t s = 0;
        for (;;) {
            size_t t = line.find(L'\t', s);
            if (t == std::wstring::npos || f.size() == 3) {
                f.push_back(line.substr(s));
                break;
            }
            f.push_back(line.substr(s, t - s));
            s = t + 1;
        }
        if (f.size() < 3)
            continue;
        Saved v;
        v.Flow = _wtoi(f[0].c_str());
        v.Role = _wtoi(f[1].c_str());
        v.Id = f[2];
        v.Name = f.size() > 3 ? f[3] : std::wstring();
        if (v.Flow < 0 || v.Flow > 1 || v.Role < 0 || v.Role > 2)
            continue;
        out.push_back(v);
    }
    return header;
}

int CmdDefaultsRestore(const std::wstring &file, int passes)
{
    std::vector<Saved> saved;
    if (!LoadSaved(file, saved)) {
        Err(L"No saved audio defaults in %s", file.c_str());
        return EXIT_ERROR;
    }
    IPolicyConfig *pc = nullptr;
    if (FAILED(CoCreateInstance(kPolicyConfigClient, nullptr, CLSCTX_ALL, __uuidof(IPolicyConfig), (void **)&pc)) ||
        pc == nullptr) {
        Err(L"Cannot reach Windows' default-device setting (IPolicyConfig). Fix by hand if needed: mmsys.cpl,");
        Err(L"right-click your speakers, Set as Default Device.");
        return EXIT_ERROR;
    }
    int moved = 0, failed = 0;
    for (int pass = 0; pass < passes; pass++) {
        if (pass > 0)
            Sleep(4000);
        IMMDeviceEnumerator *en = Enumerator();
        if (en == nullptr)
            break;
        std::vector<Endpoint> eps = ActiveEndpoints(en);
        for (auto &s : saved) {
            if (s.Id.empty())
                continue;
            std::wstring now = DefaultId(en, s.Flow, s.Role);
            if (now == s.Id)
                continue;
            std::wstring nowName = now.empty() ? L"(none)" : NameOf(eps, now);
            bool active = false;
            for (auto &e : eps)
                active = active || e.Id == s.Id;
            if (!active) {
                if (pass == passes - 1)
                    Out(L"  %s %s: was '%s', which is gone; now '%s'. Pick one in Sound settings if needed.",
                        kFlowNames[s.Flow], kRoleNames[s.Role], s.Name.c_str(), nowName.c_str());
                continue;
            }
            HRESULT hr = pc->SetDefaultEndpoint(s.Id.c_str(), (ERole)s.Role);
            if (SUCCEEDED(hr)) {
                Out(L"  %s %s: Windows had moved it to '%s'; put back '%s'.", kFlowNames[s.Flow], kRoleNames[s.Role],
                    nowName.c_str(), s.Name.c_str());
                moved++;
            } else {
                Out(L"  %s %s: could not put back '%s' (0x%08lX). Fix by hand: mmsys.cpl, right-click it, "
                    L"Set as Default Device.", kFlowNames[s.Flow], kRoleNames[s.Role], s.Name.c_str(),
                    (unsigned long)hr);
                failed++;
            }
        }
        en->Release();
    }
    pc->Release();
    if (moved == 0 && failed == 0)
        Out(L"Default audio devices unchanged.");
    return failed ? EXIT_ERROR : EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* idle-wait                                                           */
/* ------------------------------------------------------------------ */

static std::wstring ProcessName(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr)
        return L"?";
    wchar_t buf[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    std::wstring r = L"?";
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
        r.assign(buf, n);
        size_t s = r.find_last_of(L'\\');
        if (s != std::wstring::npos)
            r = r.substr(s + 1);
    }
    CloseHandle(h);
    return r;
}

/* Our tools open the device directly (control and ASIO IOCTLs, raw ring access). Which unit a
   process holds is not visible from outside, so these count whatever unit they opened. */
static void ToolReasons(std::vector<std::wstring> &why)
{
    static const wchar_t *tools[] = { L"apollo-control.exe", L"asio-test.exe", L"loopback-test.exe",
                                      L"wasapi-loop.exe", L"apollo-diag.exe" };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        for (const wchar_t *t : tools)
            if (EqualsI(pe.szExeFile, t))
                why.push_back(Fmt(L"%s (PID %lu) may have the x8p open", pe.szExeFile, pe.th32ProcessID));
    }
    CloseHandle(snap);
}

/* GET_INFO on an open handle (read-only; any driver from IOCTL version 5 on). */
static bool QueryInfo(HANDLE h, HELPER_INFO &info, DWORD &bytes)
{
    DWORD ret = 0;
    memset(&info, 0, sizeof(info));
    bytes = 0;
    if (!DeviceIoControl(h, HELPER_IOCTL_GET_INFO, nullptr, 0, &info, sizeof(info), &ret, nullptr) ||
        ret < HELPER_INFO_V5_BYTES)
        return false;
    bytes = ret;
    return true;
}

static std::wstring CharField(const CHAR *s, size_t max)
{
    std::wstring r;
    for (size_t i = 0; i < max && s[i] != '\0'; i++)
        r += (s[i] >= 0x20 && s[i] < 0x7F) ? (wchar_t)s[i] : L'?';
    return r;
}

/* Which unit an Apollo interface belongs to, from its device's hardware ID; false if the
   interface's device is not among the present UA devices (then ask the driver). */
static bool InterfaceUnitByDevice(const ApolloInterface &a, const std::vector<Device> &present, UnitKind &kind)
{
    for (auto &d : present) {
        if (!a.InstanceId.empty() && EqualsI(d.InstanceId, a.InstanceId)) {
            kind = d.Kind;
            return true;
        }
    }
    return false;
}

/* The same from the driver's own GET_INFO DeviceType; an x8p if even that cannot tell (counted,
   as before there was a second unit). */
static UnitKind InterfaceUnitByDriver(HANDLE h)
{
    HELPER_INFO info;
    DWORD bytes = 0;
    if (!QueryInfo(h, info, bytes))
        return UNIT_X8P;
    if (info.DeviceType == HELPER_TYPE_X8P_ID)
        return UNIT_X8P;
    if (info.DeviceType == HELPER_TYPE_TWINX_ID || info.SubsystemId == 0x0019)
        return UNIT_TWINX;
    return UNIT_OTHER;
}

DriverUnitInfo QueryDriverUnit(const std::wstring &instanceId)
{
    DriverUnitInfo r;
    for (auto &a : FindApolloInterfaces()) {
        if (a.InstanceId.empty() || !EqualsI(a.InstanceId, instanceId))
            continue;
        HANDLE h = CreateFileW(a.Path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            r.Error = GetLastError();
            return r;
        }
        HELPER_INFO info;
        DWORD bytes = 0;
        if (!QueryInfo(h, info, bytes)) {
            r.Error = GetLastError() ? GetLastError() : ERROR_INVALID_DATA;
            CloseHandle(h);
            return r;
        }
        CloseHandle(h);
        r.Error = 0;
        r.Version = info.Version;
        r.DeviceType = info.DeviceType;
        r.HasUnit = bytes >= sizeof(HELPER_INFO) && info.Version >= 6;
        if (r.HasUnit) {
            r.Mode = info.Mode;
            r.UnitFlags = info.UnitFlags;
            r.ModelName = CharField(info.ModelName, sizeof(info.ModelName));
            r.UnitKey = CharField(info.UnitKey, sizeof(info.UnitKey));
        }
        return r;
    }
    return r;
}

/* ACX streams and the ASIO client, from the driver itself (USER IOCTLs). The x8p's interface
   only: a Twin X on apollo.sys has its own interface, no endpoints and no ASIO, and nothing on it
   stops the x8p's driver from being swapped (audio-helpers.ps1 [G2]: --unit x8p). */
static void DriverReasons(std::vector<std::wstring> &why)
{
    std::vector<ApolloInterface> ifs = FindApolloInterfaces();
    if (ifs.empty())
        return;
    std::vector<Device> present = FindUaDevices(false);
    for (auto &a : ifs) {
        UnitKind kind = UNIT_OTHER;
        bool known = InterfaceUnitByDevice(a, present, kind);
        if (known && kind != UNIT_X8P)
            continue;                   /* a Twin X (or other unit): not opened at all */
        const wchar_t *path = a.Path.c_str();
        HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        if (!known && InterfaceUnitByDriver(h) != UNIT_X8P) {
            CloseHandle(h);
            continue;
        }
        HELPER_ENDPOINTS *e = (HELPER_ENDPOINTS *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(HELPER_ENDPOINTS));
        DWORD ret = 0;
        if (e != nullptr &&
            DeviceIoControl(h, HELPER_IOCTL_GET_ENDPOINTS, nullptr, 0, e, sizeof(*e), &ret, nullptr) &&
            ret >= offsetof(HELPER_ENDPOINTS, Endpoint)) {
            ULONG rows = e->Count < HELPER_ENDPOINTS_MAX ? e->Count : HELPER_ENDPOINTS_MAX;
            ULONG running = 0;
            for (ULONG i = 0; i < rows; i++) {
                if (ret >= offsetof(HELPER_ENDPOINTS, Endpoint) + (i + 1) * sizeof(HELPER_ENDPOINT_INFO) &&
                    (e->Endpoint[i].Flags & HELPER_EP_FLAG_RUNNING))
                    running++;
            }
            if (running > 0)
                why.push_back(Fmt(L"%lu Windows audio stream(s) RUNNING on the x8p (Windows refuses to remove the "
                                  L"driver under them)", running));
            else if (e->Prepared > 0)
                why.push_back(Fmt(L"%lu Windows audio stream(s) open (paused) on the x8p", e->Prepared));
        }
        if (e != nullptr)
            HeapFree(GetProcessHeap(), 0, e);

        HELPER_ENGINE_STATS s = {};
        ret = 0;
        if (DeviceIoControl(h, HELPER_IOCTL_GET_ENGINE_STATS, nullptr, 0, &s, sizeof(s), &ret, nullptr) &&
            ret >= offsetof(HELPER_ENGINE_STATS, AsioState) + sizeof(ULONG)) {
            if (s.AsioState == HELPER_ASIO_STATE_RUNNING)
                why.push_back(L"an ASIO host is streaming through the Apollo x8p (Avium) ASIO driver");
            else if (s.AsioState == HELPER_ASIO_STATE_OPEN)
                why.push_back(L"an ASIO host has the Apollo x8p (Avium) ASIO driver open");
        }
        CloseHandle(h);
    }
}

/* Active audio sessions on the x8p's endpoints ("... (Apollo x8p (open driver, test build))"). */
static void SessionReasons(std::vector<std::wstring> &why)
{
    IMMDeviceEnumerator *en = Enumerator();
    if (en == nullptr)
        return;
    for (int flow = 0; flow < 2; flow++) {
        IMMDeviceCollection *col = nullptr;
        UINT n = 0;
        if (FAILED(en->EnumAudioEndpoints(flow == 0 ? eRender : eCapture, DEVICE_STATE_ACTIVE, &col)) || col == nullptr)
            continue;
        if (SUCCEEDED(col->GetCount(&n))) {
            for (UINT i = 0; i < n; i++) {
                IMMDevice *d = nullptr;
                if (FAILED(col->Item(i, &d)) || d == nullptr)
                    continue;
                std::wstring name = DeviceName(d);
                IAudioSessionManager2 *mgr = nullptr;
                IAudioSessionEnumerator *se = nullptr;
                if (ContainsI(name, ENDPOINT_TAG) &&
                    SUCCEEDED(d->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void **)&mgr)) &&
                    mgr != nullptr && SUCCEEDED(mgr->GetSessionEnumerator(&se)) && se != nullptr) {
                    int count = 0;
                    se->GetCount(&count);
                    for (int k = 0; k < count; k++) {
                        IAudioSessionControl *c = nullptr;
                        IAudioSessionControl2 *c2 = nullptr;
                        AudioSessionState st = AudioSessionStateInactive;
                        if (SUCCEEDED(se->GetSession(k, &c)) && c != nullptr && SUCCEEDED(c->GetState(&st)) &&
                            st == AudioSessionStateActive &&
                            SUCCEEDED(c->QueryInterface(__uuidof(IAudioSessionControl2), (void **)&c2)) && c2 != nullptr) {
                            DWORD pid = 0;
                            c2->GetProcessId(&pid);
                            std::wstring who = c2->IsSystemSoundsSession() == S_OK ? std::wstring(L"Windows system sounds")
                                                                                   : ProcessName(pid);
                            why.push_back(Fmt(L"%s (PID %lu) is using %s for %s", who.c_str(), pid, name.c_str(),
                                              kFlowNames[flow]));
                        }
                        Release(c2);
                        Release(c);
                    }
                }
                Release(se);
                Release(mgr);
                d->Release();
            }
        }
        col->Release();
    }
    en->Release();
}

std::vector<std::wstring> ApolloBusyReasons()
{
    std::vector<std::wstring> why;
    ToolReasons(why);
    DriverReasons(why);
    SessionReasons(why);
    return why;
}

static void PrintReasons(const std::vector<std::wstring> &why)
{
    for (auto &w : why)
        Out(L"  - %s", w.c_str());
}

static bool RestartAudioService()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm == nullptr)
        return false;
    SC_HANDLE svc = OpenServiceW(scm, L"audiosrv", SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS);
    bool ok = false;
    if (svc != nullptr) {
        SERVICE_STATUS st = {};
        ControlService(svc, SERVICE_CONTROL_STOP, &st);
        for (int i = 0; i < 50 && QueryServiceStatus(svc, &st) && st.dwCurrentState != SERVICE_STOPPED; i++)
            Sleep(200);
        ok = StartServiceW(svc, 0, nullptr) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return ok;
}

static wchar_t AskKey()
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    wchar_t buf[32] = {};
    DWORD n = 0;
    if (in == nullptr || in == INVALID_HANDLE_VALUE || !ReadConsoleW(in, buf, 31, &n, nullptr))
        return L'q';
    for (DWORD i = 0; i < n; i++) {
        if (buf[i] == L'\r' || buf[i] == L'\n')
            return L'\n';
        if (buf[i] != L' ')
            return (wchar_t)towlower(buf[i]);
    }
    return L'\n';
}

/* Exit 0 when nothing uses the x8p (or the user chose to go ahead), 3 when it is still in use. */
int CmdIdleWait(int seconds, bool interactive)
{
    std::vector<std::wstring> why = ApolloBusyReasons();
    if (why.empty()) {
        Out(L"The x8p is free.");
        return EXIT_OK;
    }
    Out(L"The x8p is in use:");
    PrintReasons(why);
    if (seconds > 0) {
        Out(L"Close those programs, or stop their playback/recording (Ableton: Preferences, Audio, Driver Type");
        Out(L"away from ASIO). Waiting up to %d s for the x8p to be free...", seconds);
        ULONGLONG deadline = GetTickCount64() + (ULONGLONG)seconds * 1000;
        while (GetTickCount64() < deadline) {
            Sleep(2000);
            why = ApolloBusyReasons();
            if (why.empty()) {
                Out(L"The x8p is free.");
                return EXIT_OK;
            }
        }
        Out(L"Still in use after %d s:", seconds);
        PrintReasons(why);
    }
    if (!interactive)
        return EXIT_BUSY;
    for (;;) {
        Out(L"[Enter] go ahead anyway (Windows may then ask for a restart), [R] restart the Windows audio service");
        Out(L"(all sound stops for a few seconds), [Q] quit");
        wchar_t k = AskKey();
        if (k == L'\n' || k == L'g')
            return EXIT_OK;
        if (k == L'q')
            return EXIT_BUSY;
        if (k == L'r') {
            Out(L"Restarting the Windows audio service...");
            if (!RestartAudioService())
                Out(L"Could not restart it: %s", ErrText(GetLastError()).c_str());
            Sleep(3000);
            why = ApolloBusyReasons();
            if (why.empty()) {
                Out(L"The x8p is free.");
                return EXIT_OK;
            }
            PrintReasons(why);
        }
    }
}
