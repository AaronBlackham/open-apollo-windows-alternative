/*
 * apollo-helper: install-time helper for the Apollo open driver. See installer\README.md.
 * It switches the Apollo x8p and the Apollo Twin X Gen 1 (the two units apollo.inf lists), either
 * or both (--units, default both); every other Universal Audio device is left alone.
 *
 *   apollo-helper check                         system state as JSON (no changes)
 *   apollo-helper bind <inf> [--units U] [--stage-only]
 *                                               stage apollo.inf, force it onto the named units
 *   apollo-helper unbind                        units other than the x8p on our driver back to UA's
 *                                               first, then remove every Avium apollo.inf package
 *   apollo-helper unbind --unit x8p|twinx       that unit back to UA's driver; the package stays
 *   apollo-helper defaults save <file>          the 6 default audio endpoints
 *   apollo-helper defaults restore <file> [--passes N]
 *   apollo-helper idle-wait <seconds> [--ask] [--units U]
 *                                               wait until nothing uses the named units
 *   apollo-helper quit-ua [--units U]           close UA's apps, only while a named unit is on UA's
 *                                               driver and no unit that stays is
 *   apollo-helper regdefaults <defaults.json>   driver registry values (Services\apollo\Parameters)
 *   apollo-helper cert remove-local <local-cert.txt>
 *                                               the per-PC certificates sign-local.ps1 made
 *   apollo-helper cert add|remove <cer> [--expect <sha1>] [--record <file>]   (older installs)
 *   apollo-helper connect                       Start-menu "Connect Apollo units": elevates, asks
 *                                               per unit, then the above
 * U: x8p, twinx, x8p,twinx, all, none.
 */
#include "common.h"
#include "apollo_ioctl_min.h"
#include <objbase.h>
#include <shellapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <io.h>
#include <fcntl.h>

/* ------------------------------------------------------------------ */
/* output                                                              */
/* ------------------------------------------------------------------ */

std::string ToUtf8(const std::wstring &s)
{
    if (s.empty())
        return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string r((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &r[0], n, nullptr, nullptr);
    return r;
}

std::wstring FromUtf8(const std::string &s)
{
    if (s.empty())
        return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring r((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &r[0], n);
    return r;
}

static void WriteTo(DWORD which, const std::wstring &s)
{
    HANDLE h = GetStdHandle(which);
    DWORD mode = 0, done = 0;

    if (h == nullptr || h == INVALID_HANDLE_VALUE || s.empty())
        return;
    if (GetConsoleMode(h, &mode)) {
        WriteConsoleW(h, s.c_str(), (DWORD)s.size(), &done, nullptr);
    } else {
        std::string u = ToUtf8(s);
        WriteFile(h, u.data(), (DWORD)u.size(), &done, nullptr);
    }
}

static std::wstring VFmt(const wchar_t *fmt, va_list ap)
{
    std::vector<wchar_t> buf(1024);
    for (;;) {
        va_list cp;
        va_copy(cp, ap);
        int n = _vsnwprintf_s(buf.data(), buf.size(), _TRUNCATE, fmt, cp);
        va_end(cp);
        if (n >= 0)
            return std::wstring(buf.data(), (size_t)n);
        if (buf.size() >= 1u << 20)
            return std::wstring(buf.data());
        buf.resize(buf.size() * 4);
    }
}

std::wstring Fmt(const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::wstring s = VFmt(fmt, ap);
    va_end(ap);
    return s;
}

void Out(const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::wstring s = VFmt(fmt, ap);
    va_end(ap);
    WriteTo(STD_OUTPUT_HANDLE, s + L"\r\n");
}

void Err(const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::wstring s = VFmt(fmt, ap);
    va_end(ap);
    WriteTo(STD_ERROR_HANDLE, s + L"\r\n");
}

void OutRaw(const std::wstring &s)
{
    WriteTo(STD_OUTPUT_HANDLE, s);
}

std::wstring ErrText(DWORD err)
{
    wchar_t *msg = nullptr;
    std::wstring r;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                             FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0, (LPWSTR)&msg, 0, nullptr);
    if (n > 0 && msg != nullptr) {
        r.assign(msg, n);
        while (!r.empty() && (r.back() == L'\r' || r.back() == L'\n' || r.back() == L' ' || r.back() == L'.'))
            r.pop_back();
    }
    if (msg != nullptr)
        LocalFree(msg);
    if (r.empty())
        r = L"error";
    return Fmt(L"%s (0x%08lX)", r.c_str(), err);
}

std::wstring Lower(std::wstring s)
{
    if (!s.empty())
        CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}

bool ContainsI(const std::wstring &hay, const std::wstring &needle)
{
    return Lower(hay).find(Lower(needle)) != std::wstring::npos;
}

bool EqualsI(const std::wstring &a, const std::wstring &b)
{
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

std::wstring JsonStr(const std::wstring &s)
{
    std::wstring r = L"\"";
    for (wchar_t c : s) {
        switch (c) {
        case L'"':  r += L"\\\""; break;
        case L'\\': r += L"\\\\"; break;
        case L'\n': r += L"\\n"; break;
        case L'\r': r += L"\\r"; break;
        case L'\t': r += L"\\t"; break;
        default:
            if (c < 0x20 || c > 0x7E)
                r += Fmt(L"\\u%04x", (unsigned)c);
            else
                r += c;
        }
    }
    return r + L"\"";
}

bool ReadFileBytes(const std::wstring &path, std::vector<BYTE> &data)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER size;
    DWORD got = 0;

    data.clear();
    if (h == INVALID_HANDLE_VALUE)
        return false;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(h);
        SetLastError(ERROR_FILE_TOO_LARGE);
        return false;
    }
    data.resize((size_t)size.QuadPart);
    BOOL ok = data.empty() || ReadFile(h, data.data(), (DWORD)data.size(), &got, nullptr);
    DWORD err = GetLastError();
    CloseHandle(h);
    if (!ok || got != data.size()) {
        SetLastError(ok ? ERROR_READ_FAULT : err);
        return false;
    }
    return true;
}

bool IsElevated()
{
    HANDLE tok = nullptr;
    TOKEN_ELEVATION e = {};
    DWORD n = 0;
    bool r = false;

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        if (GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &n))
            r = e.TokenIsElevated != 0;
        CloseHandle(tok);
    }
    return r;
}

bool RequireElevated(const wchar_t *what)
{
    if (IsElevated())
        return true;
    Err(L"'%s' changes the system: run it from an elevated (Run as administrator) prompt.", what);
    return false;
}

std::wstring ExeDir()
{
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n == 0)
            return L".";
        if (n < buf.size()) {
            std::wstring p(buf.data(), n);
            size_t s = p.find_last_of(L'\\');
            return s == std::wstring::npos ? p : p.substr(0, s);
        }
        buf.resize(buf.size() * 2);
    }
}

/* ------------------------------------------------------------------ */
/* check                                                               */
/* ------------------------------------------------------------------ */

typedef LONG (WINAPI *RtlGetVersionFn)(PRTL_OSVERSIONINFOW);

static std::wstring RegString(HKEY root, const wchar_t *key, const wchar_t *value, REGSAM view)
{
    wchar_t buf[512];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | view, nullptr, buf, &size) == ERROR_SUCCESS)
        return buf;
    return std::wstring();
}

static DWORD RegDword(HKEY root, const wchar_t *key, const wchar_t *value, DWORD def)
{
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(root, key, value, RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY, nullptr, &v, &size) == ERROR_SUCCESS)
        return v;
    return def;
}

static bool DirExists(const std::wstring &p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool FileExists(const std::wstring &p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring EnvPath(const wchar_t *name)
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
    return (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : std::wstring();
}

/* UAD: where apollo-diag looks for the strip modules (Powered Plugins, System32\UAD2SDK.dll). */
static std::vector<std::wstring> FindUad()
{
    std::vector<std::wstring> found;
    const wchar_t *roots[] = { L"ProgramW6432", L"ProgramFiles", L"ProgramFiles(x86)" };
    for (const wchar_t *r : roots) {
        std::wstring base = EnvPath(r);
        if (base.empty())
            continue;
        std::wstring p = base + L"\\Universal Audio\\Powered Plugins";
        if (DirExists(p)) {
            bool dup = false;
            for (auto &f : found)
                dup = dup || EqualsI(f, p);
            if (!dup)
                found.push_back(p);
        }
    }
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::wstring p = std::wstring(sys) + L"\\UAD2SDK.dll";
        if (FileExists(p))
            found.push_back(p);
    }
    return found;
}

/* WebView2 Evergreen runtime: the registry keys Microsoft documents for detecting it. */
static std::wstring WebView2Version()
{
    const wchar_t *client = L"{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
    std::wstring k1 = std::wstring(L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\") + client;
    std::wstring v = RegString(HKEY_LOCAL_MACHINE, k1.c_str(), L"pv", RRF_SUBKEY_WOW6432KEY);
    if (v.empty() || v == L"0.0.0.0")
        v = RegString(HKEY_CURRENT_USER, k1.c_str(), L"pv", 0);
    if (v == L"0.0.0.0")
        v.clear();
    return v;
}

/* What the driver itself says about a connected unit on our driver (read-only GET_INFO): the
   Mode in effect (0 observe, 1 bring-up + transport, 2 full) from IOCTL version 6 on. */
static std::wstring DriverInfoJson(const Device &d)
{
    if (!d.Present || !d.OnApollo())
        return L"null";
    DriverUnitInfo i = QueryDriverUnit(d.InstanceId);
    if (i.Error != 0)
        return Fmt(L"{ \"error\": %s }", JsonStr(ErrText(i.Error)).c_str());
    std::wstring s = Fmt(L"{ \"ioctl_version\": %lu, \"device_type\": \"0x%02lX\"", i.Version, i.DeviceType);
    if (i.HasUnit) {
        s += Fmt(L", \"model\": %s, \"mode\": %s, \"active\": %s, \"observe\": %s, \"refused\": %s, \"unit_key\": %s",
                 JsonStr(i.ModelName).c_str(),
                 i.Mode == HELPER_INFO_MODE_UNKNOWN ? L"null" : Fmt(L"%lu", i.Mode).c_str(),
                 (i.UnitFlags & HELPER_UNIT_FLAG_ACTIVE) ? L"true" : L"false",
                 (i.UnitFlags & HELPER_UNIT_FLAG_OBSERVE) ? L"true" : L"false",
                 (i.UnitFlags & HELPER_UNIT_FLAG_REFUSED) ? L"true" : L"false", JsonStr(i.UnitKey).c_str());
    } else {
        s += L", \"mode\": null";       /* a version 5 driver: no unit fields */
    }
    return s + L" }";
}

static std::wstring DeviceJson(const Device &d, const wchar_t *indent, const std::vector<Package> &ua)
{
    Package p;
    bool haveUa = FindUaPackageFor(ua, d.Hwid, p);
    std::wstring s = L"{\r\n";
    s += Fmt(L"%s  \"kind\": \"%s\",\r\n", indent, UnitKindKey(d.Kind));
    s += Fmt(L"%s  \"supported\": %s,\r\n", indent, IsSupportedKind(d.Kind) ? L"true" : L"false");
    s += Fmt(L"%s  \"hardware_id\": %s,\r\n", indent, JsonStr(d.Hwid).c_str());
    s += Fmt(L"%s  \"instance_id\": %s,\r\n", indent, JsonStr(d.InstanceId).c_str());
    s += Fmt(L"%s  \"description\": %s,\r\n", indent, JsonStr(d.Description).c_str());
    s += Fmt(L"%s  \"connected\": %s,\r\n", indent, d.Present ? L"true" : L"false");
    s += Fmt(L"%s  \"driver\": \"%s\",\r\n", indent, OwnerKey(d.Owner()));
    s += Fmt(L"%s  \"on_apollo_driver\": %s,\r\n", indent, d.OnApollo() ? L"true" : L"false");
    s += Fmt(L"%s  \"service\": %s,\r\n", indent, JsonStr(d.Service).c_str());
    s += Fmt(L"%s  \"provider\": %s,\r\n", indent, JsonStr(d.Provider).c_str());
    s += Fmt(L"%s  \"inf\": %s,\r\n", indent, JsonStr(d.InfPath).c_str());
    s += Fmt(L"%s  \"driver_version\": %s,\r\n", indent, JsonStr(d.DriverVersion).c_str());
    s += Fmt(L"%s  \"problem_code\": %lu,\r\n", indent, d.HasProblem ? d.Problem : 0ul);
    s += Fmt(L"%s  \"apollo_driver\": %s,\r\n", indent, DriverInfoJson(d).c_str());
    s += Fmt(L"%s  \"ua_driver_package\": %s\r\n", indent, haveUa ? JsonStr(p.Published).c_str() : L"null");
    s += Fmt(L"%s}", indent);
    return s;
}

/* Parameters\<sub>\<key>\Mode for each subkey: Models\<type hex> (99 = Twin X Gen 1) and
   Units\<HW_UID or SN serial>. The driver reads these for every unit (the x8p also reads the
   legacy service-wide values; a Twin X without them is in Mode 0, observe; docs/spec-twinx.md
   6.9.1). Setup writes Models\99 from defaults.json, never Units. Read-only here. */
static std::wstring ModeKeysJson(const wchar_t *sub)
{
    std::wstring path = std::wstring(APOLLO_PARAMS_KEY) + L"\\" + sub;
    std::wstring s = L"[";
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS)
        return s + L"]";
    bool first = true;
    for (DWORD i = 0; i < 256; i++) {
        wchar_t name[256];
        DWORD n = 256;
        LSTATUS st = RegEnumKeyExW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr);
        if (st == ERROR_NO_MORE_ITEMS)
            break;
        if (st != ERROR_SUCCESS)
            continue;
        DWORD v = 0, size = sizeof(v);
        bool has = RegGetValueW(k, name, L"Mode", RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS;
        s += Fmt(L"%s{ \"key\": %s, \"mode\": %s }", first ? L"" : L", ", JsonStr(name).c_str(),
                 has ? Fmt(L"%lu", v).c_str() : L"null");
        first = false;
    }
    RegCloseKey(k);
    return s + L"]";
}

static int CmdCheck()
{
    RTL_OSVERSIONINFOW vi = {};
    vi.dwOSVersionInfoSize = sizeof(vi);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionFn rtlGetVersion = ntdll ? (RtlGetVersionFn)(void *)GetProcAddress(ntdll, "RtlGetVersion") : nullptr;
    if (rtlGetVersion != nullptr)
        rtlGetVersion(&vi);
    const wchar_t *cv = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    DWORD ubr = RegDword(HKEY_LOCAL_MACHINE, cv, L"UBR", 0);
    std::wstring display = RegString(HKEY_LOCAL_MACHINE, cv, L"DisplayVersion", RRF_SUBKEY_WOW6464KEY);
    USHORT procMachine = 0, nativeMachine = 0;
    IsWow64Process2(GetCurrentProcess(), &procMachine, &nativeMachine);
    const wchar_t *arch = nativeMachine == IMAGE_FILE_MACHINE_AMD64 ? L"x64" :
                          nativeMachine == IMAGE_FILE_MACHINE_ARM64 ? L"arm64" : L"other";
    bool osOk = vi.dwMajorVersion == 10 && vi.dwBuildNumber >= 22621 && nativeMachine == IMAGE_FILE_MACHINE_AMD64;

    std::wstring startOptions;
    int ciTestSign = -1;
    bool tsActive = TestSigningActive(startOptions, ciTestSign);
    int tsNext = TestSigningNextBoot();
    std::wstring blDrive, blDetail;
    int bl = BitLockerProtection(blDrive, blDetail);

    /* Every Universal Audio PCI device Windows knows: connected ones, and remembered ones (not
       connected now) that are still set up with our driver, since uninstall moves those too. */
    std::vector<Device> known = FindUaDevices(true);
    std::vector<Device> devs, remembered;
    for (auto &d : known) {
        if (d.Present)
            devs.push_back(d);
        else if (d.OnApollo())
            remembered.push_back(d);
    }
    std::vector<Package> pkgs = FindApolloPackages();
    std::vector<Package> uaPkgs = FindUaPackages();
    std::vector<std::wstring> uad = FindUad();
    std::wstring wv2 = WebView2Version();
    std::vector<std::wstring> unitJson;
    const Device *x8p = nullptr;
    size_t x8pIndex = 0;
    for (size_t i = 0; i < devs.size(); i++) {
        unitJson.push_back(DeviceJson(devs[i], L"    ", uaPkgs));
        if (devs[i].IsX8p() && x8p == nullptr) {
            x8p = &devs[i];
            x8pIndex = i;
        }
    }

    std::wstring s = L"{\r\n";
    s += Fmt(L"  \"helper\": %s,\r\n", JsonStr(HELPER_VERSION).c_str());
    s += Fmt(L"  \"elevated\": %s,\r\n", IsElevated() ? L"true" : L"false");
    s += Fmt(L"  \"os\": { \"version\": \"%lu.%lu.%lu.%lu\", \"display\": %s, \"build\": %lu, \"arch\": \"%s\", "
             L"\"supported\": %s },\r\n", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber, ubr,
             JsonStr(display).c_str(), vi.dwBuildNumber, arch, osOk ? L"true" : L"false");
    s += Fmt(L"  \"secure_boot\": \"%s\",\r\n", SecureBootState().c_str());
    s += Fmt(L"  \"test_signing\": { \"active\": %s, \"code_integrity_testsign\": %s, \"next_boot\": %s, "
             L"\"start_options\": %s },\r\n", tsActive ? L"true" : L"false",
             ciTestSign < 0 ? L"null" : (ciTestSign ? L"true" : L"false"),
             tsNext < 0 ? L"null" : (tsNext ? L"true" : L"false"), JsonStr(startOptions).c_str());
    s += Fmt(L"  \"bitlocker\": { \"drive\": %s, \"protection\": \"%s\", \"detail\": %s },\r\n",
             JsonStr(blDrive).c_str(), bl < 0 ? L"unknown" : (bl ? L"on" : L"off"), JsonStr(blDetail).c_str());
    /* "x8p" stays for older readers; "units" lists every connected Apollo, the x8p included. */
    if (x8p != nullptr)
        s += Fmt(L"  \"x8p\": { \"present\": true, \"device\": %s },\r\n", unitJson[x8pIndex].c_str());
    else
        s += L"  \"x8p\": { \"present\": false },\r\n";
    s += L"  \"units\": [";
    bool first = true;
    for (auto &u : unitJson) {
        s += first ? L"\r\n    " : L",\r\n    ";
        s += u;
        first = false;
    }
    s += first ? L"],\r\n" : L"\r\n  ],\r\n";
    s += L"  \"remembered_on_apollo\": [";
    first = true;
    for (auto &d : remembered) {
        s += first ? L"\r\n    " : L",\r\n    ";
        s += DeviceJson(d, L"    ", uaPkgs);
        first = false;
    }
    s += first ? L"],\r\n" : L"\r\n  ],\r\n";
    s += Fmt(L"  \"unit_settings\": { \"models\": %s, \"units\": %s },\r\n", ModeKeysJson(L"Models").c_str(),
             ModeKeysJson(L"Units").c_str());
    std::wstring loaded = LoadedApolloImage();
    s += Fmt(L"  \"apollo_loaded_image\": %s,\r\n", loaded.empty() ? L"null" : JsonStr(loaded).c_str());
    s += L"  \"apollo_packages\": [";
    first = true;
    for (auto &p : pkgs) {
        s += first ? L"\r\n    " : L",\r\n    ";
        s += Fmt(L"{ \"published\": %s, \"driver_ver\": %s, \"in_use\": %s }", JsonStr(p.Published).c_str(),
                 JsonStr(p.DriverVer).c_str(), PackageInUse(known, p.Published) ? L"true" : L"false");
        first = false;
    }
    s += first ? L"],\r\n" : L"\r\n  ],\r\n";
    s += L"  \"ua_packages\": [";
    first = true;
    for (auto &p : uaPkgs) {
        s += first ? L"\r\n    " : L",\r\n    ";
        s += Fmt(L"{ \"published\": %s, \"driver_ver\": %s, \"signer\": %s }", JsonStr(p.Published).c_str(),
                 JsonStr(p.DriverVer).c_str(), JsonStr(p.Signer).c_str());
        first = false;
    }
    s += first ? L"],\r\n" : L"\r\n  ],\r\n";
    s += Fmt(L"  \"uad\": { \"installed\": %s, \"found\": [", uad.empty() ? L"false" : L"true");
    for (size_t i = 0; i < uad.size(); i++)
        s += (i ? L", " : L"") + JsonStr(uad[i]);
    s += L"] },\r\n";
    s += Fmt(L"  \"webview2\": { \"installed\": %s, \"version\": %s }\r\n", wv2.empty() ? L"false" : L"true",
             JsonStr(wv2).c_str());
    s += L"}\r\n";
    OutRaw(s);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* connect: the Start-menu shortcut                                    */
/* ------------------------------------------------------------------ */

static void Pause()
{
    Out(L"");
    Out(L"Press Enter to close this window.");
    wchar_t buf[16];
    DWORD n = 0;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (in != nullptr && in != INVALID_HANDLE_VALUE)
        ReadConsoleW(in, buf, 15, &n, nullptr);
}

static int Relaunch(const wchar_t *args)
{
    std::vector<wchar_t> exe(MAX_PATH * 2);
    DWORD n = GetModuleFileNameW(nullptr, exe.data(), (DWORD)exe.size());
    if (n == 0 || n >= exe.size()) {
        Err(L"Cannot find this program's own path.");
        return EXIT_ERROR;
    }
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.data();
    sei.lpParameters = args;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        DWORD e = GetLastError();
        if (e == ERROR_CANCELLED)
            Err(L"Cancelled: switching a unit to the Apollo driver needs administrator rights.");
        else
            Err(L"Could not start elevated: %s", ErrText(e).c_str());
        return EXIT_NOT_ELEVATED;
    }
    return EXIT_OK;
}

/* [Y/n] on the console; Enter = yes. */
static bool AskYes()
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    wchar_t buf[32] = {};
    DWORD n = 0;
    if (in == nullptr || in == INVALID_HANDLE_VALUE || !ReadConsoleW(in, buf, 31, &n, nullptr))
        return false;
    for (DWORD i = 0; i < n; i++) {
        if (buf[i] == L' ')
            continue;
        return !(buf[i] == L'n' || buf[i] == L'N' || buf[i] == L'q' || buf[i] == L'Q');
    }
    return true;
}

static int ConnectElevated()
{
    std::wstring dir = ExeDir();
    std::wstring inf = dir + L"\\driver\\apollo.inf";
    std::wstring state = dir + L"\\state";
    std::wstring saved = state + L"\\connect-audio-defaults.txt";

    Out(L"Connect Apollo units to the Apollo open driver (test build)");
    Out(L"==========================================================");
    Out(L"");

    std::wstring opts;
    int ci = -1;
    if (!TestSigningActive(opts, ci)) {
        Out(L"Test signing is not active, so Windows cannot load the Apollo driver yet.");
        Out(L"If you just installed it, restart Windows first. Otherwise run Apollo setup again.");
        return EXIT_ERROR;
    }
    std::vector<Device> present = FindUaDevices(false);
    std::vector<UnitKind> chosen;
    bool anySupported = false;
    for (UnitKind k : { UNIT_X8P, UNIT_TWINX }) {
        std::vector<size_t> same = UnitsWithHwid(present, UnitHwid(k));
        if (same.empty())
            continue;
        anySupported = true;
        const Device &d = present[same[0]];
        Out(L"%s: on %s%s", UnitKindName(k), d.OnApollo() ? L"the Apollo driver" :
            (d.Owner() == OWNER_UA ? L"Universal Audio's driver" : (d.Service.empty() ? L"no driver" : d.Service.c_str())),
            d.HasProblem ? Fmt(L" (problem %lu)", d.Problem).c_str() : L"");
        if (same.size() > 1) {
            Out(L"  %zu units of this model are connected: Windows would switch all of them. Connect one at a time.",
                same.size());
            continue;
        }
        if (!NeedsSwitch(d)) {
            Out(L"  already running on the Apollo driver.");
            continue;
        }
        Out(L"  Switch the %s to the Apollo driver? [Y/n]", UnitKindName(k));
        if (AskYes())
            chosen.push_back(k);
        else
            Out(L"  left as it is.");
    }
    for (auto &d : present)
        if (!IsSupportedKind(d.Kind))
            Out(L"Universal Audio device %s: not supported by this driver, left on its own driver.", d.Hwid.c_str());
    if (!anySupported) {
        Out(L"No Apollo x8p or Apollo Twin X Gen 1 found. Connect it over Thunderbolt, switch it on, wait about");
        Out(L"15 seconds, then run \"Connect Apollo units\" again.");
        return EXIT_NO_DEVICE;
    }
    if (chosen.empty()) {
        Out(L"Nothing to switch. Nothing changed.");
        return EXIT_OK;
    }
    Out(L"");

    int rc = CmdIdleWait(60, true, chosen);
    if (rc != EXIT_OK) {
        Out(L"Stopped before changing anything.");
        return rc;
    }
    CreateDirectoryW(state.c_str(), nullptr);
    bool haveDefaults = CmdDefaultsSave(saved) == EXIT_OK;
    Out(L"");
    CmdQuitUa(chosen);
    Out(L"");
    rc = CmdBind(inf, chosen, false);
    if (haveDefaults) {
        Out(L"");
        CmdDefaultsRestore(saved, 3);
        DeleteFileW(saved.c_str());
    }
    Out(L"");
    if (rc == EXIT_OK)
        Out(L"Done. Apollo Control (Start menu) shows the units once they have connected (up to a minute).");
    else if (rc == EXIT_REBOOT)
        Out(L"Restart Windows to finish (see above why).");
    return rc;
}

static int CmdConnect(bool elevatedChild)
{
    if (!IsElevated()) {
        if (elevatedChild) {
            Err(L"Still not elevated after the UAC prompt.");
            return EXIT_NOT_ELEVATED;
        }
        return Relaunch(L"connect --elevated");
    }
    int rc = ConnectElevated();
    Pause();
    return rc;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void Usage()
{
    Out(L"apollo-helper %s: installer helper for the Apollo open driver (test build)", HELPER_VERSION);
    Out(L"");
    Out(L"  check                                   system state as JSON (changes nothing): every Apollo unit,");
    Out(L"                                          its driver and, on the Apollo driver, its Mode");
    Out(L"  bind <inf> [--units U] [--stage-only]   admin: stage the package and force it onto the named units");
    Out(L"  unbind                                  admin: move units other than the x8p on the Apollo driver back to");
    Out(L"                                          UA's driver, then remove every Avium apollo.inf package, rescan");
    Out(L"  unbind --unit x8p|twinx                 admin: that unit only back to UA's driver; the package stays");
    Out(L"                                          (run idle-wait --units <it> first)");
    Out(L"  defaults save <file>                    save the 6 default playback/recording devices");
    Out(L"  defaults restore <file> [--passes N]    put them back where Windows moved them");
    Out(L"  idle-wait <seconds> [--ask] [--units U] wait until no stream, ASIO host or app uses the named units");
    Out(L"  quit-ua [--units U]                     admin: close UA's apps while a named unit is on UA's driver and");
    Out(L"                                          no unit that stays still is");
    Out(L"  regdefaults <defaults.json>             admin: write Services\\apollo\\Parameters values (and subkeys)");
    Out(L"  cert remove-local <local-cert.txt>      admin: remove the per-PC certificates sign-local.ps1 made");
    Out(L"  cert add <cer> [--expect <sha1>] [--record <file>]");
    Out(L"  cert remove <cer> [--record <file>]     admin: LocalMachine Root and TrustedPublisher (older installs)");
    Out(L"  connect                                 Start-menu shortcut: elevate, ask per unit, free, bind, restore");
    Out(L"");
    Out(L"U: x8p, twinx, x8p,twinx, all (the default) or none. Only the Apollo x8p and the Apollo Twin X Gen 1 are");
    Out(L"ever switched; every other Universal Audio device stays on its own driver.");
    Out(L"Exit codes: 0 done, 1 error, 2 no named unit connected (staged only) / unit not connected, 3 still in use,");
    Out(L"4 not elevated, 5 stopped before changing anything (a unit would lose its driver or share another image),");
    Out(L"3010 restart");
}

static const wchar_t *Opt(int argc, wchar_t **argv, const wchar_t *name)
{
    for (int i = 1; i + 1 < argc; i++)
        if (EqualsI(argv[i], name))
            return argv[i + 1];
    return nullptr;
}

static bool Flag(int argc, wchar_t **argv, const wchar_t *name)
{
    for (int i = 1; i < argc; i++)
        if (EqualsI(argv[i], name))
            return true;
    return false;
}

/* --units U (default: all). False, with a message, on a bad list: nothing must run then. */
static bool UnitsOpt(int argc, wchar_t **argv, std::vector<UnitKind> &units)
{
    const wchar_t *u = Opt(argc, argv, L"--units");
    bool given = Flag(argc, argv, L"--units");
    if (!given)
        return ParseUnitList(L"all", units);
    if (u == nullptr || !ParseUnitList(u, units)) {
        Err(L"--units takes x8p, twinx, x8p,twinx, all or none. Nothing changed.");
        return false;
    }
    return true;
}

int wmain(int argc, wchar_t **argv)
{
    /* Never load DLLs from the current directory; system32 and the exe's folder only. */
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
    SetDllDirectoryW(L"");

    if (argc < 2 || EqualsI(argv[1], L"help") || EqualsI(argv[1], L"--help") || EqualsI(argv[1], L"/?")) {
        Usage();
        return argc < 2 ? EXIT_ERROR : EXIT_OK;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        Err(L"COM initialisation failed: 0x%08lX", (unsigned long)hr);
        return EXIT_ERROR;
    }
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                         nullptr, EOAC_NONE, nullptr);

    std::wstring cmd = argv[1];
    int rc = EXIT_ERROR;
    if (EqualsI(cmd, L"check")) {
        rc = CmdCheck();
    } else if (EqualsI(cmd, L"bind") && argc >= 3) {
        std::vector<UnitKind> units;
        rc = UnitsOpt(argc, argv, units) ? CmdBind(argv[2], units, Flag(argc, argv, L"--stage-only")) : EXIT_ERROR;
    } else if (EqualsI(cmd, L"unbind")) {
        /* Strict: a mistyped "--unit" must never fall through to unbinding every unit. */
        const wchar_t *unit = Opt(argc, argv, L"--unit");
        if (argc == 2) {
            rc = CmdUnbind(L"");
        } else if (argc == 4 && EqualsI(argv[2], L"--unit") && unit != nullptr && unit[0] != L'\0') {
            rc = CmdUnbind(unit);
        } else {
            Err(L"unbind takes no arguments (every unit), or --unit x8p|twinx (one unit). Nothing changed.");
            rc = EXIT_ERROR;
        }
    } else if (EqualsI(cmd, L"defaults") && argc >= 4 && EqualsI(argv[2], L"save")) {
        rc = CmdDefaultsSave(argv[3]);
    } else if (EqualsI(cmd, L"defaults") && argc >= 4 && EqualsI(argv[2], L"restore")) {
        const wchar_t *p = Opt(argc, argv, L"--passes");
        int passes = p ? _wtoi(p) : 1;
        rc = CmdDefaultsRestore(argv[3], passes < 1 ? 1 : (passes > 10 ? 10 : passes));
    } else if (EqualsI(cmd, L"idle-wait") && argc >= 3) {
        int secs = _wtoi(argv[2]);
        std::vector<UnitKind> units;
        rc = UnitsOpt(argc, argv, units) ?
             CmdIdleWait(secs < 0 ? 0 : (secs > 3600 ? 3600 : secs), Flag(argc, argv, L"--ask"), units) : EXIT_ERROR;
    } else if (EqualsI(cmd, L"quit-ua")) {
        std::vector<UnitKind> units;
        rc = UnitsOpt(argc, argv, units) ? CmdQuitUa(units) : EXIT_ERROR;
    } else if (EqualsI(cmd, L"regdefaults") && argc >= 3) {
        rc = CmdRegDefaults(argv[2]);
    } else if (EqualsI(cmd, L"cert") && argc == 4 && EqualsI(argv[2], L"remove-local")) {
        rc = CmdCertRemoveLocal(argv[3]);
    } else if (EqualsI(cmd, L"cert") && argc >= 4 && (EqualsI(argv[2], L"add") || EqualsI(argv[2], L"remove"))) {
        const wchar_t *expect = Opt(argc, argv, L"--expect");
        const wchar_t *record = Opt(argc, argv, L"--record");
        rc = CmdCert(EqualsI(argv[2], L"add"), argv[3], expect ? expect : L"", record ? record : L"");
    } else if (EqualsI(cmd, L"connect")) {
        rc = CmdConnect(Flag(argc, argv, L"--elevated"));
    } else {
        Err(L"Unknown command or missing argument: %s", cmd.c_str());
        Usage();
        rc = EXIT_ERROR;
    }
    CoUninitialize();
    return rc;
}
