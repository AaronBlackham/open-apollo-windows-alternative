/*
 * apollo-control.exe - control panel for apollo.sys (Apollo x8p open driver).
 *
 * A Win32 window hosting a WebView2 UI. The UI lives in ui\ next to the exe
 * and is served as https://apollo.ui/. A worker thread owns the device
 * handle: it polls status, readback and levels, and once a second the live
 * status (strips, engine, endpoints), runs the UI's commands, and
 * closes the handle as soon as PnP asks to remove the device, so a driver
 * install, update or revert never waits on this app.
 *
 * The device is opened with GENERIC_READ only: that is the control set in
 * apollo_ioctl.h (status, levels, readback, log, strips, engine stats,
 * endpoints, preamp and monitor parameters). Raw registers and the transport
 * stay with apollo-diag.
 *
 * [Twin X G2] Every Apollo on apollo.sys is tracked live, one slot each (the
 * x8p in slot 0); "units" lists them and every per-unit message carries
 * "unit": <slot>. Settings go to the slot the UI names (the x8p's 0 when it names none), never
 * to an observe-mode unit.
 *
 * [Plug-ins] UAD plug-in inserts on the x8p (plugin_engine.h, docs/PLUGINS.md 5): a
 * thread of their own talks to the plug-in IOCTLs (0x40-0x42) through the x8p slot's
 * handle, so its instances live and die with it; --plugin-admin <file> is the
 * elevated step the app runs itself for (hosting on/off, the module store).
 *
 * UI -> host: strings, fields separated by tabs (HandleUiMessage); a string
 * or object that is JSON ({"cmd": ...}) for the combined-device and plug-in commands.
 * Host -> UI: JSON objects with a "type" field.
 *
 * [Sess] Sessions (session_host.h: the files in %APPDATA%\Avium\ApolloControl\sessions, export / import dialogs),
 * Start with Windows (HKCU Run, written only when the page says the user turned it on: autostart-set), the tray
 * menu (open, monitor volume up / down, mute, dim, recent sessions, quit) and one instance (a second start focuses
 * the first). docs/apollo-control-units.md 1.11.
 *
 * Flags: --minimized (tray only), --demo (UI simulates the unit),
 *        --view <control|status|units|channels|device|settings>, --capture <file.png>
 *        (render, save a screenshot, exit; settings are not saved), --dev (DevTools).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <cfgmgr32.h>
#include <wrl.h>
#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cstdarg>
#include <cstddef>
#include <cwchar>

#include "WebView2.h"
#include "..\..\include\apollo_ioctl.h"
#include "..\..\include\apollo_select.h"   /* [Twin X G2] which Apollo is which */
#include "..\..\include\apollo_agg.h"      /* [G6] the combined device */
#include <map>
#include <memory>
#include "agg_json.h"                       /* [G6] Units & routing JSON <-> AGG config */
#include "monitor_cmds.h"                   /* [Monitor] ALT, sources, cue mixes: names, host refusals */
#include "corr_host.h"                      /* [Corr] speaker correction: the page's corr-* commands */
#include "rate_host.h"                      /* [Rate] the sample rate: the page's rate-* commands (docs 1.6) */
#include "endpoints_host.h"                 /* [Endpoints] the Windows endpoint set: endpoints-* commands (docs 1.7) */
#include "clock_host.h"                     /* [Clock] clock source, digital formats, front panel: clock-*, digital-*,
                                               panel-* commands and the panel/clock messages (docs 1.10) */
#include "params_host.h"                    /* [Params] the units' driver Parameters: params-get (read-only) */
#include "diag_host.h"                      /* [Diag] Save diagnostics: diag-save */
#include "..\..\include\apollo_events.h"  /* [Events] WAIT_EVENTS: the event pump below */
#include "..\res\resource.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

#define APP_VERSION      L"1.0.0"
#define WND_CLASS        L"AviumApolloControlWnd"
#define UI_HOST          L"apollo.ui"
#define UI_ORIGIN        L"https://apollo.ui/"
#define REG_KEY          L"Software\\Avium\\ApolloControl"
#define RUN_KEY          L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE        L"Avium Apollo Control"

#define WM_APP_JSON      (WM_APP + 1)   /* lParam: std::wstring* from the worker */
#define WM_APP_TRAY      (WM_APP + 2)
#define WM_APP_SHOW      (WM_APP + 3)   /* a second instance asks us to show */
#define WM_APP_SESSDLG   (WM_APP + 4)   /* [Sess] an export / import dialog, shown outside the WebView2 handler */

#define ID_TRAY_OPEN     40001
#define ID_TRAY_MUTE     40002
#define ID_TRAY_DIM      40003
#define ID_TRAY_QUIT     40004
#define ID_TRAY_VOLUP    40005          /* [Sess] monitor volume up / down (the focused unit, 2 dB) */
#define ID_TRAY_VOLDOWN  40006
#define ID_TRAY_RECENT   40100          /* [Sess] + n: the page's recent sessions, n < 8 */
#define TIMER_CAPTURE    1

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

static HINSTANCE g_inst;
static HWND      g_hwnd;
static HICON     g_iconBig, g_iconSmall;
static UINT      g_msgTaskbarCreated;

static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2>           g_webview;
static bool g_uiReady;

static bool g_optMinimized, g_optDemo, g_optDev;
static std::wstring g_optView, g_optCapture;

static bool g_closeToTray = true;
static bool g_reallyQuit;
static bool g_trayMute, g_trayDim;
static std::wstring g_trayTip = L"Apollo Control";

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void Appendf(std::wstring &out, _Printf_format_string_ const wchar_t *fmt, ...)
{
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n > 0) {
        out.append(buf, (size_t)n);
    }
}

static void JsonString(std::wstring &out, const std::wstring &s)
{
    out += L'"';
    for (wchar_t c : s) {
        switch (c) {
        case L'"':  out += L"\\\""; break;
        case L'\\': out += L"\\\\"; break;
        case L'\n': out += L"\\n"; break;
        case L'\r': out += L"\\r"; break;
        case L'\t': out += L"\\t"; break;
        default:
            if (c < 0x20 || c == 0x2028 || c == 0x2029) {
                Appendf(out, L"\\u%04x", (unsigned)c);
            } else {
                out += c;
            }
        }
    }
    out += L'"';
}

static std::wstring Widen(const char *s, size_t len)
{
    if (len == 0) {
        return {};
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s, (int)len, nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, (int)len, w.data(), n);
    return w;
}

static std::string Narrow(const std::wstring &w)
{
    if (w.empty()) {
        return {};
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring ExeDir()
{
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, _countof(buf));
    std::wstring p(buf, n);
    size_t slash = p.find_last_of(L'\\');
    return slash == std::wstring::npos ? p : p.substr(0, slash);
}

static std::wstring KnownFolder(REFKNOWNFOLDERID id)
{
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) {
        s = p;
    }
    CoTaskMemFree(p);
    return s;
}

static std::wstring ConfigPath()
{
    std::wstring dir = KnownFolder(FOLDERID_RoamingAppData) + L"\\Avium\\ApolloControl";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir + L"\\config.json";
}

static std::wstring ErrorText(DWORD err)
{
    wchar_t *buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                             FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring s = n ? std::wstring(buf, n) : L"error " + std::to_wstring(err);
    LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'.')) {
        s.pop_back();
    }
    return s;
}

static std::vector<std::wstring> Split(const std::wstring &s, size_t maxParts)
{
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (parts.size() + 1 < maxParts) {
        size_t tab = s.find(L'\t', start);
        if (tab == std::wstring::npos) {
            break;
        }
        parts.push_back(s.substr(start, tab - start));
        start = tab + 1;
    }
    parts.push_back(s.substr(start));
    return parts;
}

static bool ParseLong(const std::wstring &s, LONG &out)
{
    wchar_t *end = nullptr;
    long v = wcstol(s.c_str(), &end, 10);
    if (s.empty() || end == nullptr || *end != L'\0') {
        return false;
    }
    out = v;
    return true;
}

static void PostJson(std::wstring json)
{
    auto *p = new std::wstring(std::move(json));
    if (g_hwnd == nullptr || !PostMessageW(g_hwnd, WM_APP_JSON, 0, (LPARAM)p)) {
        delete p;
    }
}

/* ------------------------------------------------------------------ */
/* Config file (UTF-8 JSON owned by the UI) and registry bits          */
/* ------------------------------------------------------------------ */

static std::wstring ReadConfig()
{
    HANDLE f = CreateFileW(ConfigPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return {};
    }
    LARGE_INTEGER size{};
    std::string data;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < 4 * 1024 * 1024) {
        data.resize((size_t)size.QuadPart);
        DWORD got = 0;
        if (!ReadFile(f, data.data(), (DWORD)data.size(), &got, nullptr)) {
            got = 0;
        }
        data.resize(got);
    }
    CloseHandle(f);
    return Widen(data.data(), data.size());
}

static bool WriteConfig(const std::wstring &json)
{
    std::wstring path = ConfigPath();
    std::wstring tmp = path + L".tmp";
    std::string data = Narrow(json);
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD put = 0;
    BOOL ok = WriteFile(f, data.data(), (DWORD)data.size(), &put, nullptr) && put == data.size();
    ok = ok && FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

/*
 * [Sess] Start with Windows: HKCU\...\Run "Avium Apollo Control" = "<this exe>" --minimized (per user, no
 * administrator). Written or deleted only by autostart-set, which the page sends when the user flips the setting;
 * autostart-get only reads it, so the page shows what Windows will really do at the next sign-in.
 */
static std::wstring AutostartCommand()
{
    wchar_t exe[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, exe, _countof(exe));
    return L"\"" + std::wstring(exe, n) + L"\" --minimized";
}

static DWORD SetStartWithWindows(bool on)
{
    HKEY k;
    LSTATUS e = RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &k, nullptr);
    if (e != ERROR_SUCCESS) {
        return (DWORD)e;
    }
    if (on) {
        std::wstring cmd = AutostartCommand();
        e = RegSetValueExW(k, RUN_VALUE, 0, REG_SZ, (const BYTE *)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        e = RegDeleteValueW(k, RUN_VALUE);
        if (e == ERROR_FILE_NOT_FOUND) {
            e = ERROR_SUCCESS;                  /* already off */
        }
    }
    RegCloseKey(k);
    return (DWORD)e;
}

/* The Run value as it is now ('' = none). */
static std::wstring RunValue()
{
    wchar_t buf[MAX_PATH * 4 + 64];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) {
        return {};
    }
    return buf;
}

static void SavePlacement()
{
    WINDOWPLACEMENT wp{ sizeof(wp) };
    if (g_hwnd && GetWindowPlacement(g_hwnd, &wp)) {
        RegSetKeyValueW(HKEY_CURRENT_USER, REG_KEY, L"Placement", REG_BINARY, &wp, sizeof(wp));
    }
}

static bool RestorePlacement()
{
    WINDOWPLACEMENT wp{};
    DWORD size = sizeof(wp);
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"Placement", RRF_RT_REG_BINARY, nullptr, &wp, &size) !=
            ERROR_SUCCESS || size != sizeof(wp) || wp.length != sizeof(wp)) {
        return false;
    }
    if (MonitorFromRect(&wp.rcNormalPosition, MONITOR_DEFAULTTONULL) == nullptr) {
        return false;                           /* that monitor is gone */
    }
    wp.showCmd = (wp.showCmd == SW_SHOWMAXIMIZED) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    if (g_optMinimized) {
        wp.showCmd = SW_HIDE;
    }
    return SetWindowPlacement(g_hwnd, &wp) != FALSE;
}

/* ------------------------------------------------------------------ */
/* Device worker: every Apollo on apollo.sys (docs/spec-twinx.md 6.9)  */
/* ------------------------------------------------------------------ */

/*
 * [Twin X G2] Units are sensed live. Each Apollo interface the driver publishes gets a slot:
 * slot 0 is the x8p (the unit the controls talk to), any other unit takes the lowest free slot
 * 1..3 and keeps it while present. The slot number is the "unit" of every per-unit message to
 * the UI and the "index" in "units". A unit that leaves disappears at once (no stale entries).
 *
 * Each slot keeps one GENERIC_READ handle with its own PnP handle notification. On a removal
 * notice (query-remove, remove-pending/complete, or an IOCTL saying the device is gone) the
 * handle is closed at once and the unit's interface path is HELD OFF: it is not opened again,
 * not even for a moment, until its interface arrives again, PnP says the query-remove failed,
 * or HOLD_OFF_MS pass. The interface is still listed while PnP removes or restarts the device,
 * and a handle opened in that window keeps the old device object, and so the old apollo.sys
 * image, alive (a driver update then needs a reboot, and a power-cycled x8p comes back on the
 * old code; seen Nov 14). A hold is per path: one unit's arrival never releases another's.
 *
 * Settings go only to a unit the driver runs in Mode 2 (an observe unit is read-only), and on
 * the x8p never to its headphone level or headphone sources (analog knobs; unverified fields).
 */

enum class CmdKind { Input, Monitor, GetLog, Resend };

struct Command {
    CmdKind      kind;
    ULONG        a = 0;
    ULONG        b = 0;
    LONG         v = 0;
    std::wstring key;
    ULONG        unit = 0;              /* slot; settings default to the x8p's slot 0 */
};

#define MAX_UNITS        4
static const ULONGLONG HOLD_OFF_MS = 30000;
static const ULONGLONG RESCAN_MS = 2000;   /* fallback scan; arrivals and removals scan at once */
static const DWORD     ERR_READ_ONLY = ERROR_INVALID_OPERATION;    /* refused by this app, not the driver */

/* Rates from two engine snapshots of the same run (per unit). */
struct EngineSample {
    bool      valid = false;
    ULONG     starts = 0;
    ULONGLONG ticks = 0, position = 0;
    LONGLONG  qpc = 0;
    double    ticksPerSec = -1.0, framesPerSec = -1.0;
};

struct Slot {
    /* Shared with the PnP callback (thread pool) */
    SRWLOCK            lock = SRWLOCK_INIT;     /* exclusive to close, shared for I/O */
    HANDLE             h = INVALID_HANDLE_VALUE;
    std::atomic<bool>  removing{ false };       /* closed by a removal notice: drop the unit, hold its path */
    std::atomic<bool>  queryFailed{ false };    /* the query-remove failed: release the hold, reopen */
    /* Worker thread only */
    HCMNOTIFICATION    notif = nullptr;         /* never unregistered from its own callback */
    bool               used = false;
    std::wstring       path;                    /* interface path (kept after a drop for queryFailed) */
    APOLLO_INFO        info{};
    DWORD              infoBytes = 0;
    EngineSample       engine;
    std::wstring       lastStatus;
    ULONGLONG          lastStatusPost = 0, lastLivePost = 0;
    ULONG              lastSeq = 0xFFFFFFFF;
    bool               lastLevelsValid = false;
    std::atomic<ULONG> openGen{ 0 };            /* [Plug-ins] +1 per handle opened: a reopened handle lost its inserts */
    /*
     * [Events] A second, overlapped handle on the unit: the event pump parks WAIT_EVENTS on it. Opened and closed with h
     * (under lock), and on every removal notice cancelled and closed with it, so it never holds a unit up.
     */
    HANDLE             evh = INVALID_HANDLE_VALUE;
    /* Event pump thread only */
    OVERLAPPED         evOv{};
    APOLLO_EVENTS_WAIT evIn{};
    APOLLO_EVENTS      evOut{};
    bool               evPending = false;
    ULONG              evAfter = 0;
    ULONGLONG          evRetryAt = 0;
    ULONG              evGen = 0xFFFFFFFF;      /* the openGen whose handle is primed (its ring read, nothing posted) */
    ULONG              evIssueGen = 0;
    bool               evPriming = false;
};

static Slot g_slots[MAX_UNITS];

struct Hold {
    std::wstring path;
    ULONGLONG    since;
};
static std::vector<Hold> g_holds;               /* worker thread only */

/* Interface arrivals and removals (thread pool) for the worker. */
static std::mutex                g_ifaceLock;
static std::vector<std::wstring> g_ifaceArrived, g_ifaceRemoved;
static HCMNOTIFICATION           g_ifaceNotif = nullptr;
static std::atomic<bool>         g_rescan{ true };

static std::mutex          g_cmdLock;
static std::deque<Command> g_cmds;
static HANDLE              g_wake;
static HANDLE              g_evWake = nullptr;      /* [Events] the pump's wake (a unit opened, the app quits) */
static std::thread         g_evPump;
static std::atomic<bool>   g_quit{ false };
static std::thread         g_worker;

static void QueueCommand(Command c)
{
    {
        std::lock_guard<std::mutex> g(g_cmdLock);
        g_cmds.push_back(std::move(c));
    }
    SetEvent(g_wake);
}

static bool SamePath(const std::wstring &a, const std::wstring &b)
{
    return !a.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

/* ---------- hold-off (worker thread) ---------- */

static void HoldExpire(ULONGLONG now)
{
    g_holds.erase(std::remove_if(g_holds.begin(), g_holds.end(),
                                 [now](const Hold &h) { return now - h.since >= HOLD_OFF_MS; }),
                  g_holds.end());
}

static void HoldPath(const std::wstring &p, ULONGLONG now)
{
    if (p.empty()) {
        return;
    }
    for (Hold &h : g_holds) {
        if (SamePath(h.path, p)) {
            h.since = now;
            return;
        }
    }
    g_holds.push_back({ p, now });
}

static void Unhold(const std::wstring &p)
{
    g_holds.erase(std::remove_if(g_holds.begin(), g_holds.end(), [&p](const Hold &h) { return SamePath(h.path, p); }),
                  g_holds.end());
}

/* [Events] Slot lock held exclusive: the event handle's parked wait cancelled, the handle closed. */
static void EventHandleClose(Slot &s)
{
    if (s.evh != INVALID_HANDLE_VALUE) {
        CancelIoEx(s.evh, nullptr);
        CloseHandle(s.evh);
        s.evh = INVALID_HANDLE_VALUE;
    }
}

/* ---------- PnP callbacks (thread pool). Never unregister from here: that deadlocks. ---------- */

static DWORD CALLBACK DevNotify(HCMNOTIFICATION, PVOID ctx, CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA, DWORD)
{
    Slot *s = static_cast<Slot *>(ctx);
    if (s == nullptr) {
        return ERROR_SUCCESS;
    }
    switch (action) {
    case CM_NOTIFY_ACTION_DEVICEQUERYREMOVE:
    case CM_NOTIFY_ACTION_DEVICEREMOVEPENDING:
    case CM_NOTIFY_ACTION_DEVICEREMOVECOMPLETE:
        AcquireSRWLockExclusive(&s->lock);
        if (s->h != INVALID_HANDLE_VALUE) {
            CloseHandle(s->h);
            s->h = INVALID_HANDLE_VALUE;
        }
        EventHandleClose(*s);                   /* [Events] */
        ReleaseSRWLockExclusive(&s->lock);
        s->removing = true;
        SetEvent(g_wake);
        break;
    case CM_NOTIFY_ACTION_DEVICEQUERYREMOVEFAILED:
        s->queryFailed = true;
        SetEvent(g_wake);
        break;
    default:
        break;
    }
    return ERROR_SUCCESS;
}

/* An Apollo interface arrived (a unit plugged in, powered on, or back from a driver update) or left. */
static DWORD CALLBACK IfaceNotify(HCMNOTIFICATION, PVOID, CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA data, DWORD)
{
    if ((action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL || action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) &&
        data != nullptr && data->FilterType == CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE) {
        std::wstring p = data->u.DeviceInterface.SymbolicLink;
        {
            std::lock_guard<std::mutex> g(g_ifaceLock);
            (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL ? g_ifaceArrived : g_ifaceRemoved).push_back(p);
        }
        g_rescan = true;
        SetEvent(g_wake);
    }
    return ERROR_SUCCESS;
}

static void IfaceWatchStart()
{
    CM_NOTIFY_FILTER filter{};
    filter.cbSize = sizeof(filter);
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    filter.u.DeviceInterface.ClassGuid = GUID_DEVINTERFACE_APOLLO;
    if (CM_Register_Notification(&filter, nullptr, IfaceNotify, &g_ifaceNotif) != CR_SUCCESS) {
        g_ifaceNotif = nullptr;                 /* the RESCAN_MS scan and HOLD_OFF_MS timeout still work */
    }
}

static void IfaceWatchStop()
{
    if (g_ifaceNotif != nullptr) {
        CM_Unregister_Notification(g_ifaceNotif);
        g_ifaceNotif = nullptr;
    }
}

/* ---------- one slot (worker thread) ---------- */

static bool SlotIoctl(Slot &s, DWORD code, void *in, DWORD inLen, void *out, DWORD outLen, DWORD *ret, DWORD *err)
{
    bool ok = false;
    DWORD r = 0;
    *err = ERROR_INVALID_HANDLE;
    AcquireSRWLockShared(&s.lock);
    if (s.h != INVALID_HANDLE_VALUE) {
        ok = DeviceIoControl(s.h, code, in, inLen, out, outLen, &r, nullptr) != FALSE;
        *err = ok ? 0 : GetLastError();
    }
    ReleaseSRWLockShared(&s.lock);
    if (ret) {
        *ret = r;
    }
    return ok;
}

static DWORD SlotInfo(Slot &s, APOLLO_INFO &out, DWORD &bytes)
{
    DWORD err = ERROR_INVALID_HANDLE;
    AcquireSRWLockShared(&s.lock);
    if (s.h != INVALID_HANDLE_VALUE) {
        err = ApolloQueryInfo(s.h, &out, &bytes);
    }
    ReleaseSRWLockShared(&s.lock);
    return err;
}

/* 0 = opened (handle and removal notification), else the Win32 error. */
static DWORD SlotOpen(Slot &s)
{
    HANDLE h = CreateFileW(s.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        return e ? e : ERROR_GEN_FAILURE;
    }
    s.removing = false;
    s.queryFailed = false;
    /* [Events] the pump's overlapped handle (none = no events for this unit; everything else works as before) */
    HANDLE eh = CreateFileW(s.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED, nullptr);
    /* The handle first, then the notification: a removal notice that fires at once finds it and closes it. */
    AcquireSRWLockExclusive(&s.lock);
    s.h = h;
    s.evh = eh;
    ReleaseSRWLockExclusive(&s.lock);
    if (g_evWake != nullptr) {
        SetEvent(g_evWake);
    }
    s.openGen++;
    CM_NOTIFY_FILTER filter{};
    filter.cbSize = sizeof(filter);
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEHANDLE;
    filter.u.DeviceHandle.hTarget = h;
    HCMNOTIFICATION n = nullptr;
    if (CM_Register_Notification(&filter, &s, DevNotify, &n) != CR_SUCCESS) {
        n = nullptr;                            /* still usable; removal just waits for our poll */
    }
    s.notif = n;
    return 0;
}

static void SlotClose(Slot &s)
{
    AcquireSRWLockExclusive(&s.lock);
    if (s.h != INVALID_HANDLE_VALUE) {
        CloseHandle(s.h);
        s.h = INVALID_HANDLE_VALUE;
    }
    EventHandleClose(s);                        /* [Events] */
    ReleaseSRWLockExclusive(&s.lock);
    if (s.notif != nullptr) {
        CM_Unregister_Notification(s.notif);    /* worker thread: never from the callback */
        s.notif = nullptr;
    }
}

static bool IsGone(DWORD err)
{
    switch (err) {
    case ERROR_FILE_NOT_FOUND: case ERROR_INVALID_HANDLE: case ERROR_DEV_NOT_EXIST:
    case ERROR_DEVICE_NOT_CONNECTED: case ERROR_DEVICE_REMOVED: case ERROR_NO_SUCH_DEVICE:
    case ERROR_BAD_UNIT: case ERROR_GEN_FAILURE:
        return true;
    default:
        return false;
    }
}

/* ---------- JSON pieces ---------- */

static void JsonAscii(std::wstring &out, const char *s, size_t cap)
{
    std::wstring w;
    for (size_t i = 0; i < cap && s[i] != 0; i++) {
        unsigned char c = (unsigned char)s[i];
        w += (c >= 0x20 && c < 0x7F) ? (wchar_t)c : L'?';
    }
    JsonString(out, w);
}

static std::string SerialText(const APOLLO_INFO &i)
{
    std::string s;
    for (size_t k = 0; k < sizeof(i.Serial) && i.Serial[k] != 0; k++) {
        unsigned char c = (unsigned char)i.Serial[k];
        s += (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    return s;
}

static std::wstring FwText(ULONG fw)
{
    wchar_t b[32];
    swprintf_s(b, L"%lu.%lu.%lu", fw >> 24, (fw >> 16) & 0xFF, fw & 0xFFFF);
    return b;
}

/* The unit's Mode, or -1 (unknown: a unit the driver did not accept, or a v5 driver's non-x8p). */
static long UnitMode(const APOLLO_INFO &i, DWORD bytes)
{
    if (ApolloInfoHasUnit(&i, bytes)) {
        return i.Mode == APOLLO_INFO_MODE_UNKNOWN ? -1 : (long)i.Mode;
    }
    return ApolloInfoIsX8p(&i, bytes) ? 2 : -1;     /* version 5 ran only an x8p, always in full */
}

/* The driver's per-unit key (Parameters\Units\<id>): HW_UID as 16 hex digits, or SN<serial>. */
static std::wstring UnitId(const Slot &s, int idx)
{
    const APOLLO_INFO &i = s.info;
    if (ApolloInfoHasUnit(&i, s.infoBytes) && i.UnitKey[0] != 0 && i.UnitKey[sizeof(i.UnitKey) - 1] == 0) {
        return Widen(i.UnitKey, strlen(i.UnitKey));
    }
    std::string sn = SerialText(i);
    bool alnum = sn.size() >= 8 && sn.size() <= 16;
    for (char c : sn) {
        alnum = alnum && ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
    }
    if (alnum) {
        return L"SN" + Widen(sn.data(), sn.size());
    }
    return L"if" + std::to_wstring(idx);
}

static std::wstring StatusJson(int unit, const APOLLO_INFO &i, DWORD bytes)
{
    std::wstring j = L"{\"type\":\"status\",";
    Appendf(j, L"\"unit\":%d,\"present\":true,\"info\":{", unit);
    Appendf(j, L"\"version\":%lu,\"stage\":%lu,\"failedStage\":%lu,\"lastStatus\":%lu,\"flags\":%lu,",
            i.Version, i.Stage, i.FailedStage, (ULONG)i.LastStatus, i.Flags);
    Appendf(j, L"\"fpgaRev\":%lu,\"extCaps\":%lu,\"fwRelease\":%lu,\"subsystemId\":%lu,\"deviceType\":%lu,",
            i.FpgaRev, i.ExtCaps, i.FwRelease, i.SubsystemId, i.DeviceType);
    Appendf(j, L"\"publicType\":%lu,\"numDsps\":%lu,\"playChannels\":%lu,\"recChannels\":%lu,",
            i.PublicType, i.NumDsps, i.PlayChannels, i.RecChannels);
    Appendf(j, L"\"ringFrames\":%lu,\"sampleRate\":%lu,\"servicePasses\":%lu,\"interruptCount\":%lu,",
            i.RingFrames, i.SampleRate, i.ServicePasses, i.InterruptCount);
    Appendf(j, L"\"notifStatusSeen\":%lu,\"numPreamps\":%lu,", i.NotifStatusSeen, i.NumPreamps);
    long mode = UnitMode(i, bytes);
    Appendf(j, mode < 0 ? L"\"mode\":null," : L"\"mode\":%ld,", mode);
    Appendf(j, L"\"observe\":%s,\"serial\":", ApolloInfoActive(&i, bytes) ? L"false" : L"true");
    std::string serial = SerialText(i);
    JsonString(j, Widen(serial.data(), serial.size()));
    if (ApolloInfoHasUnit(&i, bytes)) {
        Appendf(j, L",\"unitFlags\":%lu,\"hwUid\":\"%08lX%08lX\",\"hwidSubsystem\":%lu,\"aggRole\":%lu,\"unitKey\":",
                i.UnitFlags, i.HwUidHi, i.HwUidLo, i.HwidSubsystem, i.AggRole);
        JsonAscii(j, i.UnitKey, sizeof(i.UnitKey));
        j += L",\"logTag\":";
        JsonAscii(j, i.LogTag, sizeof(i.LogTag));
        j += L",\"modelName\":";
        JsonAscii(j, i.ModelName, sizeof(i.ModelName));
        if (i.UnitFlags & APOLLO_UNIT_FLAG_OWNER_READ) {
            /* What UA's driver left: stale by definition, nothing of ours runs on that unit. */
            Appendf(j, L",\"owner\":{\"rateInfo\":%lu,\"clockSrcs\":%lu,\"descInVersion\":%lu,\"descInCount\":%lu,"
                       L"\"descOutVersion\":%lu,\"descOutCount\":%lu,\"stale\":true}",
                    i.OwnerRateInfo, i.OwnerClockSrcs, i.OwnerDescInVersion, i.OwnerDescInCount,
                    i.OwnerDescOutVersion, i.OwnerDescOutCount);
        }
    }
    j += L"}}";
    return j;
}

static void PostAbsent(DWORD err)
{
    std::wstring j = L"{\"type\":\"status\",\"unit\":0,\"present\":false,\"error\":";
    Appendf(j, L"%lu,\"detail\":", err);
    JsonString(j, err == ERROR_FILE_NOT_FOUND ? L"No Apollo x8p on apollo.sys" : ErrorText(err));
    j += L"}";
    PostJson(std::move(j));
}

static void PostUnitGone(int unit)
{
    std::wstring j = L"{\"type\":\"status\",";
    Appendf(j, L"\"unit\":%d,\"present\":false}", unit);
    PostJson(std::move(j));
}

/* {"type":"units","units":[{id,index,model,deviceType,serial,fw,mode,present,active,observe,stage}]} */
static std::wstring UnitsJson()
{
    std::wstring j = L"{\"type\":\"units\",\"units\":[";
    bool first = true;
    for (int idx = 0; idx < MAX_UNITS; idx++) {
        const Slot &s = g_slots[idx];
        if (!s.used || s.infoBytes < APOLLO_INFO_V5_BYTES) {
            continue;
        }
        const APOLLO_INFO &i = s.info;
        long mode = UnitMode(i, s.infoBytes);
        bool active = i.Stage == ApolloStageConnected && mode == 2;
        j += first ? L"{\"id\":" : L",{\"id\":";
        first = false;
        JsonString(j, UnitId(s, idx));
        Appendf(j, L",\"index\":%d,\"model\":", idx);
        const char *model = ApolloInfoModelText(&i, s.infoBytes);
        JsonString(j, Widen(model, strlen(model)));
        Appendf(j, L",\"deviceType\":%lu,\"serial\":", i.DeviceType);
        std::string serial = SerialText(i);
        JsonString(j, Widen(serial.data(), serial.size()));
        j += L",\"fw\":";
        JsonString(j, FwText(i.FwRelease));
        Appendf(j, mode < 0 ? L",\"mode\":null" : L",\"mode\":%ld", mode);
        Appendf(j, L",\"present\":true,\"active\":%s,\"observe\":%s,\"stage\":%lu", active ? L"true" : L"false",
                ApolloInfoActive(&i, s.infoBytes) ? L"false" : L"true", i.Stage);
        std::string clk = clockhost::UnitsExtra(idx);  /* [Clock] "clock" and "panel" summaries (clock_host.h) */
        j += Widen(clk.data(), clk.size()) + L"}";
    }
    j += L"]}";
    return j;
}

/* ------------------------------------------------------------------ */
/* Live status (about 1 s per unit): strips, engine, endpoints. Read-only IOCTLs */
/* ------------------------------------------------------------------ */

/*
 * One "live" message per present unit: {"type":"live","unit":N,"engine":{..}|null,"strips":{..}|null,
 * "endpoints":{..}|null}, with "<part>Err" (a Win32 code) beside each null. N is the unit's slot.
 */
static void LiveEngine(std::wstring &j, Slot &u)
{
    EngineSample &prev = u.engine;
    /* No input buffer: bit 0 of the input would reset the driver's counters. Slack for a newer,
       longer struct; a shorter (older) one reads as zeros past what it returned. */
    std::vector<BYTE> buf(sizeof(APOLLO_ENGINE_STATS) + 256);
    DWORD ret = 0, err = 0;
    if (!SlotIoctl(u, IOCTL_APOLLO_GET_ENGINE_STATS, nullptr, 0, buf.data(), (DWORD)buf.size(), &ret, &err) ||
        ret < offsetof(APOLLO_ENGINE_STATS, EndOfBufferCount)) {
        prev = EngineSample{};
        Appendf(j, L"\"engine\":null,\"engineErr\":%lu", err ? err : (DWORD)ERROR_INVALID_DATA);
        return;
    }
    APOLLO_ENGINE_STATS s;
    memcpy(&s, buf.data(), sizeof(s));

    /* Rates from two snapshots of the same engine run: ticks/s and frames/s (the real rate). */
    bool same = prev.valid && s.Running && !s.EngineLost && s.Starts == prev.starts && s.QpcFrequency > 0 &&
                s.Qpc > prev.qpc && s.Ticks >= prev.ticks && s.Position >= prev.position;
    double el = same ? (double)(s.Qpc - prev.qpc) / (double)s.QpcFrequency : 0.0;
    if (!same || el >= 0.5) {
        double tps = same ? (double)(s.Ticks - prev.ticks) / el : -1.0;
        double fps = same ? (double)(s.Position - prev.position) / el : -1.0;
        prev.valid = s.Running && !s.EngineLost && s.QpcFrequency > 0;
        prev.starts = s.Starts;
        prev.ticks = s.Ticks;
        prev.position = s.Position;
        prev.qpc = s.Qpc;
        prev.ticksPerSec = tps;
        prev.framesPerSec = fps;
    }                                   /* else: too short a window, keep the older sample */

    Appendf(j, L"\"engine\":{\"version\":%lu,\"running\":%lu,\"lost\":%lu,\"starts\":%lu,\"ticks\":%llu,",
            s.Version, s.Running, s.EngineLost, s.Starts, s.Ticks);
    Appendf(j, L"\"lateTicks\":%lu,\"busStalls\":%lu,\"deadReads\":%lu,\"maxIntervalUs\":%lu,",
            s.LateTicks, s.BusStalls, s.DeadReads, s.MaxTickIntervalUs);
    Appendf(j, L"\"asioState\":%lu,\"asioFrames\":%lu,\"asioIn\":%lu,\"asioOut\":%lu,", s.AsioState,
            s.AsioBufferFrames, s.AsioInputs, s.AsioOutputs);
    Appendf(j, L"\"asioUnderruns\":%lu,\"asioOverruns\":%lu,\"asioSkipped\":%lu,", s.AsioUnderruns,
            s.AsioOverruns, s.AsioClientSkipped);
    if (same && prev.ticksPerSec >= 0.0) {
        Appendf(j, L"\"ticksPerSec\":%.2f,\"framesPerSec\":%.2f}", prev.ticksPerSec, prev.framesPerSec);
    } else {
        j += L"\"ticksPerSec\":null,\"framesPerSec\":null}";
    }
}

static void LiveStrips(std::wstring &j, Slot &u)
{
    /* GET_STRIPS waits for DevLock, which bring-up holds for seconds: ask once it is done. */
    if (u.info.Stage != ApolloStageConnected) {
        Appendf(j, L",\"strips\":null,\"stripsErr\":%lu", (DWORD)ERROR_NOT_READY);
        return;
    }
    std::vector<BYTE> buf(sizeof(APOLLO_STRIPS_INFO) + 256);
    DWORD ret = 0, err = 0;
    if (!SlotIoctl(u, IOCTL_APOLLO_GET_STRIPS, nullptr, 0, buf.data(), (DWORD)buf.size(), &ret, &err) ||
        ret < offsetof(APOLLO_STRIPS_INFO, StepsLimit)) {
        Appendf(j, L",\"strips\":null,\"stripsErr\":%lu", err ? err : (DWORD)ERROR_INVALID_DATA);
        return;
    }
    APOLLO_STRIPS_INFO s;
    memcpy(&s, buf.data(), sizeof(s));
    bool perInput = ret >= sizeof(APOLLO_STRIPS_INFO);  /* version 2: the per-input arrays */

    Appendf(j, L",\"strips\":{\"version\":%lu,\"state\":%lu,\"flags\":%lu,\"enable\":%lu,\"mask\":%lu,",
            perInput ? s.Version : 1ul, s.State, s.Flags, s.Enable, s.Mask);
    Appendf(j, L"\"stepsLimit\":%lu,\"stepsDone\":%lu,\"phantomState\":%lu,\"phantomMask\":%lu,\"why\":",
            perInput ? s.StepsLimit : 0ul, perInput ? s.StepsDone : 0ul, perInput ? s.PhantomState : 0ul,
            perInput ? s.PhantomMask : 0ul);
    JsonAscii(j, s.Why, sizeof(s.Why));
    Appendf(j, L",\"perInput\":%s,\"taken\":[", perInput ? L"true" : L"false");
    for (ULONG i = 0; i < APOLLO_STRIP_INPUTS; i++) {
        Appendf(j, i ? L",%lu" : L"%lu", perInput ? s.StripState[i] : 0ul);
    }
    j += L"],\"started\":[";
    for (ULONG i = 0; i < APOLLO_STRIP_INPUTS; i++) {
        Appendf(j, i ? L",%lu" : L"%lu", perInput && s.StripStarted[i] ? 1ul : 0ul);
    }
    j += L"]}";
}

static void LiveEndpoints(std::wstring &j, Slot &u)
{
    /* The driver refuses a buffer smaller than its own struct, which may grow (more endpoint
       rows): offer plenty and take the row count from what came back. */
    const size_t hdr = offsetof(APOLLO_ENDPOINTS, Endpoint);
    const size_t row = sizeof(APOLLO_ENDPOINT_INFO);
    std::vector<BYTE> buf(std::max<size_t>(sizeof(APOLLO_ENDPOINTS) * 4, 16384));
    DWORD ret = 0, err = 0;
    if (!SlotIoctl(u, IOCTL_APOLLO_GET_ENDPOINTS, nullptr, 0, buf.data(), (DWORD)buf.size(), &ret, &err) ||
        ret < hdr) {
        Appendf(j, L",\"endpoints\":null,\"endpointsErr\":%lu", err ? err : (DWORD)ERROR_INVALID_DATA);
        return;
    }
    const APOLLO_ENDPOINTS *e = (const APOLLO_ENDPOINTS *)buf.data();
    /* A row size this build does not know (header changed, app not rebuilt) leaves a remainder. */
    bool layoutOk = ((ret - hdr) % row) == 0;
    ULONG rows = layoutOk ? (ULONG)std::min<size_t>(e->Count, (ret - hdr) / row) : 0;

    Appendf(j, L",\"endpoints\":{\"version\":%lu,\"count\":%lu,\"enabledMask\":%lu,\"prepared\":%lu,"
               L"\"layoutOk\":%s,\"rows\":[", e->Version, e->Count, e->EnabledMask, e->Prepared,
            layoutOk ? L"true" : L"false");
    for (ULONG i = 0; i < rows; i++) {
        const APOLLO_ENDPOINT_INFO *p = (const APOLLO_ENDPOINT_INFO *)(buf.data() + hdr + i * row);
        j += i ? L",{\"name\":" : L"{\"name\":";
        JsonAscii(j, p->Name, sizeof(p->Name));
        j += L",\"circuit\":";
        JsonAscii(j, p->Circuit, sizeof(p->Circuit));
        Appendf(j, L",\"render\":%lu,\"channels\":%lu,\"slots\":[%lu,%lu],\"flags\":%lu,", p->Render,
                p->Channels, p->Slot[0], p->Slot[1], p->Flags);
        Appendf(j, L"\"packetCount\":%lu,\"packetBytes\":%lu,\"streams\":%lu,\"underruns\":%lu,"
                   L"\"overruns\":%lu,\"dropped\":%lu}", p->PacketCount, p->PacketBytes, p->Streams,
                p->Underruns, p->Overruns, p->Dropped);
    }
    j += L"]}";
}

static void PostLive(int unit, Slot &u)
{
    std::wstring j = L"{\"type\":\"live\",";
    Appendf(j, L"\"unit\":%d,", unit);
    LiveEngine(j, u);
    LiveStrips(j, u);
    LiveEndpoints(j, u);
    j += L"}";
    PostJson(std::move(j));
}

/* ------------------------------------------------------------------ */
/* Units: discovery, drop, commands (worker thread)                   */
/* ------------------------------------------------------------------ */

static DWORD g_absentErr = 0xFFFFFFFF;          /* slot 0's last "present":false error (dedupe) */
static std::wstring g_lastUnits;

static void ResetSlotState(Slot &s)
{
    s.info = APOLLO_INFO{};
    s.infoBytes = 0;
    s.engine = EngineSample{};
    s.lastStatus.clear();
    s.lastStatusPost = 0;
    s.lastLivePost = 0;
    s.lastSeq = 0xFFFFFFFF;
    s.lastLevelsValid = false;
}

/* The unit in slot idx left (or is being removed): close, hold its path, tell the UI at once. */
static void DropSlot(int idx, bool hold, ULONGLONG now)
{
    Slot &s = g_slots[idx];
    SlotClose(s);
    if (hold) {
        HoldPath(s.path, now);
    }
    bool was = s.used;
    s.used = false;
    ResetSlotState(s);
    clockhost::Forget(idx);                     /* [Clock] the next unit in this slot is posted afresh */
    if (was) {
        if (idx == 0) {
            g_absentErr = ERROR_DEVICE_REMOVED;
            PostAbsent(ERROR_DEVICE_REMOVED);
        } else {
            PostUnitGone(idx);
        }
    }
    g_rescan = true;
}

/*
 * List the interfaces that are neither open in a slot nor held off, ask each for GET_INFO
 * (ApolloListUnits: a short read-only open), and give every new unit a slot: the x8p slot 0,
 * any other the lowest free of 1..3. Then slot 0's "absent" status if no x8p is there.
 */
static void Rescan(ULONGLONG now)
{
    static APOLLO_UNIT_ENTRY e[APOLLO_SELECT_MAX];
    std::vector<const WCHAR *> skip;

    HoldExpire(now);
    for (const Slot &s : g_slots) {
        if (s.used) {
            skip.push_back(s.path.c_str());
        }
    }
    for (const Hold &h : g_holds) {
        skip.push_back(h.path.c_str());
    }
    int n = ApolloListUnits(e, APOLLO_SELECT_MAX, skip.data(), (int)skip.size());
    DWORD firstErr = 0;
    for (int k = 0; k < n; k++) {
        if (e[k].Error != 0) {
            if (e[k].Error != ERROR_BUSY && firstErr == 0) {
                firstErr = e[k].Error;
            }
            continue;
        }
        int idx = -1;
        if (ApolloInfoIsX8p(&e[k].Info, e[k].InfoBytes) && !g_slots[0].used) {
            idx = 0;
        } else {
            for (int m = 1; m < MAX_UNITS && idx < 0; m++) {
                if (!g_slots[m].used) {
                    idx = m;
                }
            }
        }
        if (idx < 0) {
            continue;                           /* more units than slots: shown once one leaves */
        }
        Slot &s = g_slots[idx];
        ResetSlotState(s);
        s.path = e[k].Path;
        DWORD err = SlotOpen(s);
        if (err != 0) {
            if (firstErr == 0) {
                firstErr = err;
            }
            continue;
        }
        s.used = true;
        s.info = e[k].Info;
        s.infoBytes = e[k].InfoBytes;
    }
    if (!g_slots[0].used) {
        DWORD err = (n < 0) ? (DWORD)ERROR_GEN_FAILURE : (firstErr != 0 ? firstErr : (DWORD)ERROR_FILE_NOT_FOUND);
        if (err != g_absentErr) {
            g_absentErr = err;
            PostAbsent(err);
        }
    } else {
        g_absentErr = 0xFFFFFFFF;
    }
}

static void PostError(const wchar_t *op, const Command &c, DWORD code, const std::wstring &detail)
{
    std::wstring j = L"{\"type\":\"error\",\"op\":";
    JsonString(j, op);
    j += L",\"key\":";
    JsonString(j, c.key);
    Appendf(j, L",\"index\":%lu,\"unit\":%lu,\"code\":%lu,\"detail\":", c.a, c.unit, code);
    JsonString(j, detail);
    j += L"}";
    PostJson(std::move(j));
}

/*
 * Settings: only to a present unit the driver runs in Mode 2. Per model: the x8p's headphone
 * levels are analog knobs (S2 [15:8] does nothing useful there), so they are never sent to an x8p.
 * [Monitor] Its headphone sources go to the driver, whose per-model plan decides (monitor_cmds.h).
 */
static bool SettingAllowed(const Command &c, const wchar_t *op)
{
    if (c.unit >= MAX_UNITS || !g_slots[c.unit].used) {
        PostError(op, c, ERROR_FILE_NOT_FOUND, L"No Apollo in that unit slot: nothing was sent");
        return false;
    }
    const Slot &s = g_slots[c.unit];
    if (!ApolloInfoActive(&s.info, s.infoBytes) || UnitMode(s.info, s.infoBytes) < 2) {
        PostError(op, c, ERR_READ_ONLY, L"Read-only unit (observe mode): settings are never sent to it");
        return false;
    }
    if (c.kind == CmdKind::Monitor && ApolloInfoIsX8p(&s.info, s.infoBytes)) {
        if (const wchar_t *why = MonHostRefusalX8p(c.a)) {
            PostError(op, c, ERR_READ_ONLY, why);
            return false;
        }
    }
    return true;
}

static void RunCommand(const Command &c, ULONGLONG now)
{
    DWORD err = 0;
    bool ok = false;
    const wchar_t *op = L"";
    int idx = (int)c.unit;

    if (c.kind == CmdKind::Input) {
        op = L"input";
        if (!SettingAllowed(c, op)) {
            return;
        }
        APOLLO_INPUT_PARAM p{ c.a, c.b, c.v };
        ok = SlotIoctl(g_slots[idx], IOCTL_APOLLO_SET_INPUT_PARAM, &p, sizeof(p), nullptr, 0, nullptr, &err);
    } else if (c.kind == CmdKind::Monitor) {
        op = L"monitor";
        if (!SettingAllowed(c, op)) {
            return;
        }
        APOLLO_MONITOR_PARAM p{ c.a, c.v };
        ok = SlotIoctl(g_slots[idx], IOCTL_APOLLO_SET_MONITOR_PARAM, &p, sizeof(p), nullptr, 0, nullptr, &err);
    } else if (c.kind == CmdKind::GetLog) {
        /* One ring for every unit (lines tagged [x8p], [twx]): any present unit's handle reads it. */
        op = L"log";
        idx = -1;
        for (int k = 0; k < MAX_UNITS && idx < 0; k++) {
            if (g_slots[k].used) {
                idx = k;
            }
        }
        if (idx < 0) {
            PostError(op, c, ERROR_FILE_NOT_FOUND, L"No Apollo is connected to the open driver");
            return;
        }
        std::vector<BYTE> buf(sizeof(APOLLO_LOG_HEADER) + APOLLO_LOG_BYTES);
        DWORD ret = 0;
        ok = SlotIoctl(g_slots[idx], IOCTL_APOLLO_GET_LOG, nullptr, 0, buf.data(), (DWORD)buf.size(), &ret, &err);
        if (ok && ret >= sizeof(APOLLO_LOG_HEADER)) {
            auto *hdr = (PAPOLLO_LOG_HEADER)buf.data();
            ULONG len = std::min<ULONG>(hdr->Length, ret - (ULONG)sizeof(*hdr));
            std::wstring j = L"{\"type\":\"log\",\"total\":";
            Appendf(j, L"%lu,\"text\":", hdr->TotalWritten);
            JsonString(j, Widen((const char *)(hdr + 1), len));
            j += L"}";
            PostJson(std::move(j));
            return;
        }
    } else {
        return;
    }
    if (!ok) {
        PostError(op, c, err, ErrorText(err));
        if (IsGone(err) && idx >= 0 && idx < MAX_UNITS && g_slots[idx].used) {
            DropSlot(idx, true, now);
        }
    }
}

/* Per present unit, every pass: status (on change or each second), readback and levels for a unit
   the driver runs, live once a second. Returns false if the unit went away. */
static bool PollSlot(int idx, ULONGLONG now)
{
    Slot &s = g_slots[idx];
    APOLLO_INFO info{};
    DWORD bytes = 0, ret = 0, err = SlotInfo(s, info, bytes);

    if (err != 0) {
        if (IsGone(err)) {
            DropSlot(idx, true, now);           /* wait for its interface to arrive again */
            return false;
        }
        return true;
    }
    s.info = info;
    s.infoBytes = bytes;
    std::wstring status = StatusJson(idx, info, bytes);
    if (status != s.lastStatus || now - s.lastStatusPost >= 1000) {
        s.lastStatus = status;
        s.lastStatusPost = now;
        PostJson(status);
    }
    bool active = ApolloInfoActive(&info, bytes) != FALSE;

    if (active && info.Stage == ApolloStageConnected && (info.Flags & APOLLO_FLAG_MIXER_READY)) {
        APOLLO_READBACK rb{};
        if (SlotIoctl(s, IOCTL_APOLLO_GET_READBACK, nullptr, 0, &rb, sizeof(rb), &ret, &err) &&
            ret >= sizeof(rb) && rb.Sequence != s.lastSeq) {
            s.lastSeq = rb.Sequence;
            std::wstring j = L"{\"type\":\"readback\",";
            Appendf(j, L"\"unit\":%d,\"valid\":%s,\"seq\":%lu,\"words\":[", idx, rb.Valid ? L"true" : L"false",
                    rb.Sequence);
            for (ULONG w = 0; w < APOLLO_READBACK_WORDS; w++) {
                Appendf(j, w ? L",%lu" : L"%lu", rb.Words[w]);
            }
            j += L"]}";
            PostJson(std::move(j));
        }
    }

    if (active && (info.Flags & APOLLO_FLAG_TRANSPORT)) {
        APOLLO_LEVELS lv{};
        if (SlotIoctl(s, IOCTL_APOLLO_GET_LEVELS, nullptr, 0, &lv, sizeof(lv), &ret, &err) && ret >= sizeof(lv)) {
            std::wstring j = L"{\"type\":\"levels\",";
            Appendf(j, L"\"unit\":%d,\"valid\":%s,\"peaks\":[", idx, lv.Valid ? L"true" : L"false");
            ULONG n = std::min<ULONG>(lv.Channels, APOLLO_LEVEL_CHANNELS);
            for (ULONG c = 0; c < n; c++) {
                Appendf(j, c ? L",%lu" : L"%lu", lv.Peak[c]);
            }
            j += L"]}";
            PostJson(std::move(j));
            s.lastLevelsValid = lv.Valid != 0;
        }
    } else if (s.lastLevelsValid) {
        s.lastLevelsValid = false;
        std::wstring j = L"{\"type\":\"levels\",";
        Appendf(j, L"\"unit\":%d,\"valid\":false,\"peaks\":[]}", idx);
        PostJson(std::move(j));
    }

    if (now - s.lastLivePost >= 1000) {
        s.lastLivePost = now;
        PostLive(idx, s);
    }

    /* [Clock] the front panel (knob, buttons, talkback) and the clock source: "panel" / "clock" when they change */
    if (active && info.Stage == ApolloStageConnected) {
        clockhost::PollUnit(idx, info, bytes, [&s](DWORD code, void *out, DWORD outLen, DWORD *r) {
            DWORD e = 0;
            return SlotIoctl(s, code, nullptr, 0, out, outLen, r, &e);
        }, +[](const std::string &u) { PostJson(Widen(u.data(), u.size())); }, now);
    }
    return true;
}

static void PostUnitsIfChanged()
{
    std::wstring units = UnitsJson();
    if (units != g_lastUnits) {
        g_lastUnits = units;
        PostJson(std::move(units));
    }
}

/*
 * [G7] The combined configuration changed from another tool (agg-test, the ASIO clock menu, another
 * Apollo Control): AGG_GET_STATE.ConfigGeneration moves on every AGG_SET_CONFIG, and after a driver
 * reload. Polled once a second on a unit the worker holds; a change that is not our own agg-set is
 * answered with an unsolicited "agg" (docs/apollo-control-units.md 1.4). A G6 driver reports 0: nothing.
 */
static std::atomic<ULONG> g_aggSeenGen{ 0 };    /* last generation seen (0 = none yet) */
static std::atomic<ULONG> g_aggOwnGen{ 0 };     /* the generation our own agg-set produced */
static std::atomic<bool>  g_aggSetBusy{ false }; /* an agg-set is under way on the UI thread */
static void AggGet();

static void AggPollGeneration()
{
    for (int idx = 0; idx < MAX_UNITS; idx++) {
        if (!g_slots[idx].used) {
            continue;
        }
        APOLLO_AGG_STATE st{};
        ULONG flags = 0;
        DWORD ret = 0, err = 0;
        if (!SlotIoctl(g_slots[idx], IOCTL_APOLLO_AGG_GET_STATE, &flags, sizeof(flags), &st, sizeof(st), &ret, &err) ||
            ret < sizeof(st) || st.ConfigGeneration == 0) {
            return;                             /* no combined device, or a driver before G7 */
        }
        ULONG seen = g_aggSeenGen.load();
        if (seen == 0 || g_aggSetBusy.load()) {
            if (seen == 0) {
                g_aggSeenGen = st.ConfigGeneration;     /* the page asks agg-get itself on its first list */
            }
            return;
        }
        if (st.ConfigGeneration != seen) {
            g_aggSeenGen = st.ConfigGeneration;
            if (st.ConfigGeneration != g_aggOwnGen.load()) {
                AggGet();                       /* unsolicited: the stored config changed for another reason */
            }
        }
        return;
    }
}

/* ------------------------------------------------------------------ */
/* [Events] The event pump: one thread, one parked WAIT_EVENTS per unit */
/* ------------------------------------------------------------------ */

static const wchar_t *EventKindText(ULONG k)
{
    switch (k) {
    case APOLLO_EVENT_STAGE:       return L"stage";
    case APOLLO_EVENT_LEAVING:     return L"leaving";
    case APOLLO_EVENT_RATE:        return L"rate";
    case APOLLO_EVENT_CLOCK:       return L"clock";
    case APOLLO_EVENT_PANEL:       return L"panel";
    case APOLLO_EVENT_DSP_FAULT:   return L"dsp-fault";
    case APOLLO_EVENT_ENGINE_LOST: return L"engine-lost";
    case APOLLO_EVENT_PARAMS:      return L"params";
    default:                       return L"other";
    }
}

/*
 * Each unit with an event handle has one WAIT_EVENTS parked in the driver; when it completes, its events go to the page
 * as {"type":"event","unit":<slot>,"seq","kind","a","b"} (panel events are left to the panel poll: they come in runs)
 * and the next wait goes out with the newest Seq. Completion is read from the OVERLAPPED itself, so a handle a removal
 * notice closed (EventHandleClose: CancelIoEx, then CloseHandle) is never touched again. An error waits before the
 * next try (2 s; an older driver without WAIT_EVENTS: a minute).
 */
static void EventPumpMain()
{
    for (Slot &s : g_slots) {
        s.evOv.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    while (!g_quit) {
        ULONGLONG now = GetTickCount64();
        HANDLE waits[MAX_UNITS + 1];
        DWORD n = 0;

        for (int idx = 0; idx < MAX_UNITS; idx++) {
            Slot &s = g_slots[idx];
            if (s.evPending || s.evOv.hEvent == nullptr || now < s.evRetryAt) {
                continue;
            }
            AcquireSRWLockShared(&s.lock);
            if (s.evh != INVALID_HANDLE_VALUE) {
                HANDLE ev = s.evOv.hEvent;
                ZeroMemory(&s.evOv, sizeof(s.evOv));
                s.evOv.hEvent = ev;
                ResetEvent(ev);
                /* a handle not read yet: one NOWAIT read primes it (what the ring holds is old news, never posted) */
                s.evIssueGen = s.openGen.load();
                s.evPriming = (s.evGen != s.evIssueGen);
                s.evIn = APOLLO_EVENTS_WAIT{ APOLLO_EVENTS_VERSION, s.evPriming ? 0ul : s.evAfter,
                                             s.evPriming ? (ULONG)APOLLO_EVENTS_WAIT_NOWAIT : 0ul, 0 };
                BOOL ok = DeviceIoControl(s.evh, IOCTL_APOLLO_WAIT_EVENTS, &s.evIn, sizeof(s.evIn), &s.evOut, sizeof(s.evOut),
                                          nullptr, &s.evOv);
                DWORD err = ok ? 0 : GetLastError();
                if (ok || err == ERROR_IO_PENDING) {
                    s.evPending = true;
                } else {
                    s.evRetryAt = now + (err == ERROR_INVALID_FUNCTION ? 60000 : 2000);
                }
            }
            ReleaseSRWLockShared(&s.lock);
        }
        waits[n++] = g_evWake;
        for (Slot &s : g_slots) {
            if (s.evPending) {
                waits[n++] = s.evOv.hEvent;
            }
        }
        WaitForMultipleObjects(n, waits, FALSE, 1000);
        now = GetTickCount64();
        for (int idx = 0; idx < MAX_UNITS; idx++) {
            Slot &s = g_slots[idx];
            if (!s.evPending || !HasOverlappedIoCompleted(&s.evOv)) {
                continue;
            }
            s.evPending = false;
            LONG st = (LONG)s.evOv.Internal;
            if (st != 0 || s.evOv.InternalHigh < sizeof(APOLLO_EVENTS) || s.evOut.Version != APOLLO_EVENTS_VERSION) {
                /* cancelled (a removal, the app closing), gone, or an older driver: try again later */
                s.evRetryAt = now + ((ULONG)st == 0xC0000010ul ? 60000 : 2000);   /* STATUS_INVALID_DEVICE_REQUEST */
                continue;
            }
            if (s.evPriming) {
                s.evGen = s.evIssueGen;
                s.evAfter = s.evOut.Seq;
                continue;
            }
            for (ULONG i = 0; i < s.evOut.Count && i < APOLLO_EVENTS_MAX; i++) {
                const APOLLO_EVENT_ENTRY &e = s.evOut.Entry[i];
                if (e.Kind == APOLLO_EVENT_PANEL) {
                    continue;
                }
                std::wstring j = L"{\"type\":\"event\",";
                Appendf(j, L"\"unit\":%d,\"seq\":%lu,\"kind\":\"%s\",\"a\":%lu,\"b\":%lu,\"reset\":%s}", idx, e.Seq,
                        EventKindText(e.Kind), e.A, e.B, (s.evOut.Flags & APOLLO_EVENTS_F_RESET) ? L"true" : L"false");
                PostJson(std::move(j));
            }
            s.evAfter = s.evOut.Seq;
            SetEvent(g_wake);                   /* the worker posts the units' status now */
        }
    }
    /* quitting: cancel what is parked and wait for it (the buffers are the slots') */
    for (Slot &s : g_slots) {
        AcquireSRWLockShared(&s.lock);
        if (s.evPending && s.evh != INVALID_HANDLE_VALUE) {
            CancelIoEx(s.evh, &s.evOv);
        }
        ReleaseSRWLockShared(&s.lock);
        if (s.evPending) {
            WaitForSingleObject(s.evOv.hEvent, 2000);
            s.evPending = false;
        }
    }
}

static void WorkerMain()
{
    IfaceWatchStart();
    ULONGLONG lastRescan = 0, lastAggPoll = 0;

    while (!g_quit) {
        WaitForSingleObject(g_wake, 33);
        if (g_quit) {
            break;
        }
        ULONGLONG now = GetTickCount64();

        /* PnP: removal notices and failed query-removes per slot, then interface arrivals/removals. */
        for (int idx = 0; idx < MAX_UNITS; idx++) {
            Slot &s = g_slots[idx];
            if (s.removing.exchange(false)) {
                if (s.used) {
                    DropSlot(idx, true, now);
                } else {
                    SlotClose(s);
                }
            }
            if (s.queryFailed.exchange(false)) {
                Unhold(s.path);                 /* the device stays: reopen it */
                g_rescan = true;
            }
        }
        std::vector<std::wstring> arrived, removed;
        {
            std::lock_guard<std::mutex> g(g_ifaceLock);
            arrived.swap(g_ifaceArrived);
            removed.swap(g_ifaceRemoved);
        }
        for (const std::wstring &p : arrived) {
            Unhold(p);                          /* this path only: other held units stay held */
        }
        for (const std::wstring &p : removed) {
            for (int idx = 0; idx < MAX_UNITS; idx++) {
                if (g_slots[idx].used && SamePath(g_slots[idx].path, p)) {
                    DropSlot(idx, true, now);
                }
            }
        }

        std::deque<Command> cmds;
        {
            std::lock_guard<std::mutex> g(g_cmdLock);
            cmds.swap(g_cmds);
        }
        bool resend = false;
        for (const Command &c : cmds) {
            if (c.kind == CmdKind::Resend) {
                resend = true;
            }
        }
        if (resend) {
            for (Slot &s : g_slots) {
                s.lastStatus.clear();
                s.lastSeq = 0xFFFFFFFF;
                s.lastLivePost = 0;
            }
            g_absentErr = 0xFFFFFFFF;
            g_lastUnits.clear();
            g_rescan = true;
        }

        if (g_rescan.exchange(false) || now - lastRescan >= RESCAN_MS) {
            lastRescan = now;
            Rescan(now);
            PostUnitsIfChanged();               /* a new unit is listed before its first status */
        }

        for (const Command &c : cmds) {
            if (c.kind != CmdKind::Resend) {
                RunCommand(c, now);
            }
        }

        for (int idx = 0; idx < MAX_UNITS; idx++) {
            if (g_slots[idx].used) {
                (void)PollSlot(idx, now);
            }
        }

        PostUnitsIfChanged();                   /* Mode, stage or active changed in this pass */
        if (now - lastAggPoll >= 1000) {
            lastAggPoll = now;
            AggPollGeneration();                /* [G7] */
        }
    }
    for (Slot &s : g_slots) {
        SlotClose(s);
        s.used = false;
    }
    IfaceWatchStop();
}

/* ------------------------------------------------------------------ */
/* [Plug-ins] UAD plug-in inserts (plugin_engine.h, docs/PLUGINS.md 5)  */
/* ------------------------------------------------------------------ */

#include "plugin_engine.h"

/* ------------------------------------------------------------------ */
/* UI messages                                                         */
/* ------------------------------------------------------------------ */

static bool InputParam(const std::wstring &n, ULONG &p)
{
    static const struct { const wchar_t *n; ULONG p; } map[] = {
        { L"line", APOLLO_PARAM_MIC_LINE }, { L"pad", APOLLO_PARAM_PAD },
        { L"link", APOLLO_PARAM_LINK },     { L"48v", APOLLO_PARAM_48V },
        { L"lowcut", APOLLO_PARAM_LOWCUT }, { L"phase", APOLLO_PARAM_PHASE },
        { L"gain", APOLLO_PARAM_GAIN_DB },
        { L"hizimp", APOLLO_PARAM_HIZ_IMP },   /* [G5] Twin X input 1 Hi-Z impedance, codes 0-7 */
    };
    for (auto &m : map) {
        if (n == m.n) {
            p = m.p;
            return true;
        }
    }
    return false;
}

static bool MonitorParam(const std::wstring &n, ULONG &p)
{
    static const struct { const wchar_t *n; ULONG p; } map[] = {
        { L"level", APOLLO_MON_LEVEL },         { L"hp1", APOLLO_MON_HP1_LEVEL },
        { L"mute", APOLLO_MON_MUTE },           { L"mono", APOLLO_MON_MONO },
        { L"source", APOLLO_MON_SOURCE },       { L"hp1src", APOLLO_MON_HP1_SOURCE },
        { L"hp2src", APOLLO_MON_HP2_SOURCE },   { L"dimlevel", APOLLO_MON_DIM_LEVEL },
        { L"dim", APOLLO_MON_DIM },             { L"talkback", APOLLO_MON_TALKBACK },
    };
    for (auto &m : map) {
        if (n == m.n) {
            p = m.p;
            return true;
        }
    }
    return MonFeatureParam(n, p);               /* [Monitor] ALT, cue MIX/mono/mute, sends (monitor_cmds.h) */
}

static COLORREF ParseHexColor(const std::wstring &s, COLORREF fallback)
{
    if (s.size() != 7 || s[0] != L'#') {
        return fallback;
    }
    unsigned long v = wcstoul(s.c_str() + 1, nullptr, 16);
    return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

static void ApplyTheme(bool dark, COLORREF bg, COLORREF fg)
{
    BOOL d = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &d, sizeof(d));
    DwmSetWindowAttribute(g_hwnd, DWMWA_CAPTION_COLOR, &bg, sizeof(bg));
    DwmSetWindowAttribute(g_hwnd, DWMWA_TEXT_COLOR, &fg, sizeof(fg));
    ComPtr<ICoreWebView2Controller2> c2;
    if (g_controller && SUCCEEDED(g_controller.As(&c2))) {
        COREWEBVIEW2_COLOR c{ 255, GetRValue(bg), GetGValue(bg), GetBValue(bg) };
        c2->put_DefaultBackgroundColor(c);
    }
}

static void UpdateTrayTip();
static void ShowMain();

static void SendHello()
{
    std::wstring j = L"{\"type\":\"hello\",\"app\":";
    JsonString(j, APP_VERSION);
    j += L",\"config\":";
    JsonString(j, ReadConfig());
    Appendf(j, L",\"demo\":%s,\"minimized\":%s,\"capture\":%s,\"view\":", g_optDemo ? L"true" : L"false",
            g_optMinimized ? L"true" : L"false", g_optCapture.empty() ? L"false" : L"true");
    JsonString(j, g_optView);
    j += L"}";
    g_webview->PostWebMessageAsJson(j.c_str());
}

/*
 * [Twin X G2] JSON commands ({"cmd":"agg-get"} / {"cmd":"agg-set","config":{...}}, Units & routing,
 * docs/apollo-control-units.md 1.3-1.4). [G6] Answered from the driver's AGG_* (AggGet/AggSet
 * below); a driver before G6 answers ERROR_INVALID_FUNCTION, sent as error code 1 (preview).
 * AGG_SET_CONFIG is a user-level IOCTL and the driver stores the config itself (spec 6.8), so
 * Apply needs no elevation: needsAdmin is false.
 */
static std::wstring JsonCmd(const std::wstring &json)
{
    size_t k = json.find(L"\"cmd\"");
    if (k == std::wstring::npos) {
        return {};
    }
    k = json.find(L':', k + 5);
    if (k == std::wstring::npos) {
        return {};
    }
    k = json.find(L'"', k + 1);
    if (k == std::wstring::npos) {
        return {};
    }
    size_t e = json.find(L'"', k + 1);
    return e == std::wstring::npos ? std::wstring() : json.substr(k + 1, e - k - 1);
}

/*
 * [G6] The combined device (driver AGG_*, include/apollo_agg.h). The aggregate is driver-global, so a
 * short-lived read-only handle on any unit reaches it, apart from the slots the worker keeps.
 * agg-get answers what the driver holds: the JSON it stored for us, or (nothing stored, or a config
 * set by another tool) a description of its binary config. agg-set compiles the page's JSON
 * (app/src/agg_json.h) into AGG_SET_CONFIG, a user-level IOCTL: the driver validates and stores
 * it under Parameters\Aggregate itself, so needsAdmin stays false (docs/apollo-control-units.md 1.4).
 */
static void AggReply(const std::string &configUtf8, bool pending, DWORD code, const std::string &detail)
{
    std::wstring j = L"{\"type\":\"agg\"";
    if (!pending || !configUtf8.empty()) {
        j += L",\"config\":";
        j += configUtf8.empty() ? std::wstring(L"null") : Widen(configUtf8.data(), configUtf8.size());
    }
    j += pending ? L",\"pending\":true" : L",\"pending\":false";
    if (code != 0) {
        Appendf(j, L",\"error\":{\"code\":%lu,\"detail\":", code);
        JsonString(j, Widen(detail.data(), detail.size()));
        j += L'}';
    } else {
        j += L",\"error\":null";
    }
    j += L",\"detail\":";
    JsonString(j, pending ? Widen(detail.data(), detail.size()) : std::wstring());
    j += L",\"needsAdmin\":false}";
    PostJson(std::move(j));
}

/* A read-only handle on the first unit that answers, plus what the units say about themselves. */
static HANDLE AggOpen(DWORD &err, std::map<std::string, aggjson::UnitText> *known)
{
    std::vector<APOLLO_UNIT_ENTRY> units(APOLLO_SELECT_MAX);
    int n = ApolloListUnits(units.data(), APOLLO_SELECT_MAX, nullptr, 0);
    HANDLE h = INVALID_HANDLE_VALUE;

    err = n <= 0 ? ERROR_FILE_NOT_FOUND : ERROR_NOT_READY;
    for (int i = 0; i < n; i++) {
        const APOLLO_UNIT_ENTRY &e = units[(size_t)i];
        if (e.Error != 0) {
            continue;
        }
        if (known && ApolloInfoHasUnit(&e.Info, e.InfoBytes) && e.Info.UnitKey[0] != 0 &&
            e.Info.UnitKey[sizeof(e.Info.UnitKey) - 1] == 0) {
            aggjson::UnitText t;
            t.model = ApolloInfoModelText(&e.Info, e.InfoBytes);
            t.serial.assign(e.Info.Serial, strnlen(e.Info.Serial, sizeof(e.Info.Serial)));
            (*known)[e.Info.UnitKey] = t;
        }
        if (h == INVALID_HANDLE_VALUE) {
            h = CreateFileW(e.Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                err = GetLastError();
            }
        }
    }
    return h;
}

static std::string AggErrorText(DWORD e)
{
    switch (e) {
    case ERROR_INVALID_FUNCTION:
        return "not available in this build (apollo.sys before G6)";
    case ERROR_FILE_NOT_FOUND:
        return "no Apollo on apollo.sys";
    case ERROR_BUSY:
        return "the driver is busy, try again";
    default:
        return Narrow(ErrorText(e));
    }
}

static void AggGet()
{
    std::map<std::string, aggjson::UnitText> known;
    DWORD err = 0, ret = 0;
    HANDLE h = AggOpen(err, &known);
    if (h == INVALID_HANDLE_VALUE) {
        AggReply({}, false, err == ERROR_FILE_NOT_FOUND ? 50 : err, AggErrorText(err));
        return;
    }
    auto c = std::make_unique<APOLLO_AGG_CONFIG>();
    BOOL ok = DeviceIoControl(h, IOCTL_APOLLO_AGG_GET_CONFIG, nullptr, 0, c.get(), sizeof(*c), &ret, nullptr);
    err = ok ? 0 : GetLastError();
    CloseHandle(h);
    if (!ok || ret < sizeof(*c)) {
        AggReply({}, false, err ? err : ERROR_INVALID_DATA, AggErrorText(err ? err : ERROR_INVALID_DATA));
        return;
    }
    std::string text;
    if (c->JsonBytes != 0 && c->JsonBytes <= APOLLO_AGG_JSON_MAX && !(c->Flags & APOLLO_AGG_CFG_DEFAULT)) {
        text.assign(c->Json, c->JsonBytes);
    } else {
        text = aggjson::AggDescribe(*c, known);    /* the driver's default, or set by another tool */
    }
    AggReply(text, false, 0, {});
}

/* [G7 review] Why the driver refused a configuration (APOLLO_AGG_SET_RESULT.Error, APOLLO_AGG_ERR_*). */
static std::string AggRefusalText(ULONG code)
{
    switch (code) {
    case APOLLO_AGG_ERR_VERSION:   return "Apollo Control and the driver are from different builds (configuration size)";
    case APOLLO_AGG_ERR_UNITS:     return "the unit list is empty, too long, or has a unit without an ID";
    case APOLLO_AGG_ERR_MASTER:    return "the clock master is not one of the units";
    case APOLLO_AGG_ERR_ASRC:      return "the resampler quality must be low, default or high";
    case APOLLO_AGG_ERR_CHANNELS:  return "a channel names a unit or slot that does not exist";
    case APOLLO_AGG_ERR_ROUTES:    return "a route names a unit or source that does not exist";
    case APOLLO_AGG_ERR_JSON:      return "the configuration text is too long";
    case APOLLO_AGG_ERR_DUPLICATE: return "the same unit is listed twice";
    default:                       return "the driver refused the configuration (check " + std::to_string(code) + ")";
    }
}

static void AggSet(const std::wstring &json)
{
    std::string text = aggjson::ConfigTextOf(Narrow(json)), err;
    auto c = std::make_unique<APOLLO_AGG_CONFIG>();
    APOLLO_AGG_SET_RESULT res{};
    DWORD e = 0, ret = 0;

    if (text.empty() || !aggjson::AggCompile(text, *c, err)) {
        AggReply({}, false, ERROR_INVALID_PARAMETER, err.empty() ? std::string("no config object") : err);
        return;
    }
    AggReply({}, true, 0, "Storing the configuration in the driver");
    HANDLE h = AggOpen(e, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        AggReply({}, false, e == ERROR_FILE_NOT_FOUND ? 50 : e, AggErrorText(e));
        return;
    }
    g_aggSetBusy = true;                        /* [G7] the generation poll leaves this change to us */
    BOOL ok = DeviceIoControl(h, IOCTL_APOLLO_AGG_SET_CONFIG, c.get(), sizeof(*c), &res, sizeof(res), &ret, nullptr);
    e = ok ? 0 : GetLastError();
    if (ok) {
        APOLLO_AGG_STATE st{};
        ULONG flags = 0;
        DWORD sret = 0;
        if (DeviceIoControl(h, IOCTL_APOLLO_AGG_GET_STATE, &flags, sizeof(flags), &st, sizeof(st), &sret, nullptr) &&
            sret >= sizeof(st)) {
            g_aggOwnGen = st.ConfigGeneration;
        }
    }
    g_aggSetBusy = false;
    CloseHandle(h);
    if (!ok) {
        std::string d = AggErrorText(e);
        if (e == ERROR_INVALID_PARAMETER && ret >= sizeof(res) && res.Error != 0) {
            d = AggRefusalText(res.Error);      /* a G6 driver (the error status brings nothing back, usually) */
        }
        AggReply({}, false, e, d);
        return;
    }
    if (ret >= sizeof(res) && res.Error != 0) {
        /* [G7 review] refused: the driver completes with success and the reason in Error (nothing stored) */
        AggReply({}, false, ERROR_INVALID_PARAMETER, AggRefusalText(res.Error));
        return;
    }
    AggReply(text, false, 0, {});               /* as the driver stored it */
}

/* [Rate] rate_host.h replies from its own thread too: PostJson is safe from any thread. */
static void RatePost(const std::string &utf8)
{
    PostJson(Widen(utf8.data(), utf8.size()));
}

/*
 * [Themes] Lab's Avium Measure hand-off: {"cmd":"measure-open","routing":{...}} is written as it came to
 * %APPDATA%\Avium\ApolloControl\measure-handoff.json, then Avium Measure is started through its App Paths
 * registration ("AviumMeasure.exe") with --apollo-routing <file>. Nothing is installed or looked up on the
 * network; when Avium Measure is not registered the file stays for it to open. Reply: {"type":"measure-result",
 * "ok":<written>,"launched":<started>,"file":"<path>","why":"<text>"}.
 */
static std::wstring JsonText(const std::wstring &s)
{
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'"' || c == L'\\') { o += L'\\'; o += c; }
        else if (c < 0x20) { o += L' '; }
        else { o += c; }
    }
    return o;
}

static void MeasureOpen(const std::wstring &json)
{
    std::wstring path = ConfigPath();
    path = path.substr(0, path.find_last_of(L'\\')) + L"\\measure-handoff.json";
    std::string data = Narrow(json);
    bool ok = false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD put = 0;
        ok = WriteFile(f, data.data(), (DWORD)data.size(), &put, nullptr) && put == data.size();
        CloseHandle(f);
    }
    std::wstring why;
    bool launched = false;
    if (!ok) {
        why = L"the file could not be written (" + ErrorText(GetLastError()) + L")";
    } else {
        std::wstring args = L"--apollo-routing \"" + path + L"\"";
        SHELLEXECUTEINFOW sei{ sizeof(sei) };
        sei.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
        sei.hwnd = g_hwnd;
        sei.lpVerb = L"open";
        sei.lpFile = L"AviumMeasure.exe";
        sei.lpParameters = args.c_str();
        sei.nShow = SW_SHOWNORMAL;
        launched = ShellExecuteExW(&sei) != FALSE;
        if (!launched) {
            why = L"Avium Measure is not installed here (no App Paths entry for AviumMeasure.exe): open the file from it";
        }
    }
    std::wstring r = L"{\"type\":\"measure-result\",\"ok\":" + std::wstring(ok ? L"true" : L"false") + L",\"launched\":" +
                     std::wstring(launched ? L"true" : L"false") + L",\"file\":\"" + JsonText(path) + L"\",\"why\":\"" +
                     JsonText(why) + L"\"}";
    PostJson(r);
}

/* [Sess] the session files (session_host.h) */
#include "session_host.h"

/* [Sess] {"type":"autostart","on":<the Run value exists>,"current":<it starts this exe>,"command":"...","error":<Win32>,"detail":"..."} */
static void PostAutostart(DWORD err)
{
    std::wstring v = RunValue();
    std::wstring j = L"{\"type\":\"autostart\",\"on\":";
    j += v.empty() ? L"false" : L"true";
    j += L",\"current\":";
    j += !v.empty() && _wcsicmp(v.c_str(), AutostartCommand().c_str()) == 0 ? L"true" : L"false";
    j += L",\"command\":";
    JsonString(j, v);
    Appendf(j, L",\"error\":%lu,\"detail\":", err);
    JsonString(j, err ? ErrorText(err) : std::wstring());
    j += L"}";
    PostJson(std::move(j));
}

static void HandleUiJson(const std::wstring &json)
{
    std::wstring cmd = JsonCmd(json);
    if (cmd == L"measure-open") {
        MeasureOpen(json);                      /* [Themes] Lab's Avium Measure hand-off */
        return;
    }
    if (cmd == L"session-plugins") {
        /* [Sess] the engine's chains for a session or the last state, read only: answered as "plugin-session" by the
           plug-in thread (plug-session-get); a page's own plug-* commands stay the user's actions */
        if (!g_optDemo && g_optCapture.empty()) {
            pe::FromUi("{\"cmd\":\"plug-session-get\"}");
        }
        return;
    }
    if (sesshost::Owns(cmd)) {
        sesshost::Handle(Narrow(json), g_hwnd, WM_APP_SESSDLG, RatePost);   /* [Sess] session-* */
        return;
    }
    if (cmd == L"autostart-get" || cmd == L"autostart-set") {
        /* [Sess] read always; written only on autostart-set (the user flipped "Start Apollo Control when I sign in") */
        DWORD err = 0;
        if (cmd == L"autostart-set") {
            aggjson::Value v;
            std::string u = Narrow(json);
            aggjson::Parser p(u);
            const aggjson::Value *on = p.Parse(v) ? v.Get("on") : nullptr;
            err = on && on->kind == aggjson::Value::Bool ? SetStartWithWindows(on->b) : (DWORD)ERROR_INVALID_PARAMETER;
        }
        PostAutostart(err);
        return;
    }
    if (ratehost::Owns(cmd)) {
        ratehost::Handle(Narrow(json), RatePost);   /* [Rate] rate-get / rate-set (rate_host.h) */
        return;
    }
    if (ephost::Owns(cmd)) {
        ephost::Handle(Narrow(json), RatePost);     /* [Endpoints] endpoints-get / endpoints-set (endpoints_host.h) */
        return;
    }
    if (clockhost::Owns(cmd)) {
        clockhost::Handle(Narrow(json), RatePost);  /* [Clock] clock-*, digital-*, panel-* (clock_host.h) */
        return;
    }
    if (paramshost::Owns(cmd)) {
        paramshost::Handle(Narrow(json), RatePost); /* [Params] params-get (params_host.h) */
        return;
    }
    if (diaghost::Owns(cmd)) {
        diaghost::Save(RatePost, APP_VERSION);      /* [Diag] one folder for a fault report (diag_host.h) */
        return;
    }
    if (cmd == L"agg-get") {
        AggGet();
    } else if (cmd == L"agg-set") {
        AggSet(json);
    } else if (corrhost::Owns(cmd)) {
        /* [Corr] speaker correction (corr_host.h): check / load / set / get, replies straight to the page */
        for (const std::string &r : corrhost::Handle(Narrow(json))) {
            PostJson(Widen(r.data(), r.size()));
        }
    } else if (cmd.compare(0, 5, L"plug-") == 0) {
        pe::FromUi(Narrow(json));               /* [Plug-ins] the plug-in thread runs it */
    }                                           /* anything else: ignored */
}

/* The optional unit field of a setting command ("...\t<unit>"); 0 = the x8p's slot. */
static bool ParseUnitField(const std::vector<std::wstring> &f, size_t at, ULONG &unit)
{
    LONG u = 0;
    unit = 0;
    if (f.size() <= at || f[at].empty()) {
        return true;
    }
    if (!ParseLong(f[at], u) || u < 0) {
        return false;
    }
    unit = (ULONG)u;
    return true;
}

static void HandleUiMessage(const std::wstring &msg)
{
    if (!msg.empty() && msg[0] == L'{') {
        HandleUiJson(msg);
        return;
    }
    std::vector<std::wstring> f = Split(msg, 8);
    const std::wstring &cmd = f[0];

    if (cmd == L"hello") {
        g_uiReady = true;
        SendHello();
        QueueCommand({ CmdKind::Resend });
    } else if (cmd == L"input" && f.size() >= 4) {
        /* input \t <preamp> \t <param> \t <value> [\t <unit>] */
        LONG ch = 0, v = 0;
        ULONG p = 0, unit = 0;
        if (ParseLong(f[1], ch) && ch >= 0 && InputParam(f[2], p) && ParseLong(f[3], v) &&
            ParseUnitField(f, 4, unit)) {
            QueueCommand({ CmdKind::Input, (ULONG)ch, p, v, f[2], unit });
        }
    } else if (cmd == L"monitor" && f.size() >= 3) {
        /* monitor \t <param> \t <value> [\t <unit>] */
        LONG v = 0;
        ULONG p = 0, unit = 0;
        if (MonitorParam(f[1], p) && ParseLong(f[2], v) && ParseUnitField(f, 3, unit)) {
            QueueCommand({ CmdKind::Monitor, p, 0, v, f[1], unit });
        }
    } else if (cmd == L"getLog") {
        QueueCommand({ CmdKind::GetLog });
    } else if (cmd == L"resend") {
        QueueCommand({ CmdKind::Resend });      /* the UI left demo mode: send fresh state */
    } else if (cmd == L"saveConfig") {
        std::vector<std::wstring> g = Split(msg, 2);
        if (g.size() == 2 && !WriteConfig(g[1])) {
            g_webview->PostWebMessageAsJson(
                L"{\"type\":\"error\",\"op\":\"config\",\"key\":\"\",\"code\":0,"
                L"\"detail\":\"Settings could not be saved to %APPDATA%\\\\Avium\\\\ApolloControl\"}");
        }
    } else if (cmd == L"prefs" && f.size() >= 2) {
        /* prefs \t <close to the tray> [\t ...]: [Sess] the Run value is autostart-set's alone, never written here */
        g_closeToTray = f[1] == L"1";
    } else if (cmd == L"show") {
        ShowMain();                             /* [Sess] the page needs the user (a 48V question for a session load) */
    } else if (cmd == L"theme" && f.size() >= 4) {
        ApplyTheme(f[1] == L"dark", ParseHexColor(f[2], RGB(22, 24, 27)), ParseHexColor(f[3], RGB(230, 230, 230)));
    } else if (cmd == L"zoom" && f.size() >= 2) {
        double z = _wtof(f[1].c_str());
        if (z >= 0.75 && z <= 1.5 && g_controller) {
            g_controller->put_ZoomFactor(z);
        }
    } else if (cmd == L"trayState" && f.size() >= 4) {
        g_trayMute = f[1] == L"1";
        g_trayDim = f[2] == L"1";
        g_trayTip = f[3].substr(0, 120);
        UpdateTrayTip();
    } else if (cmd == L"quit") {
        g_reallyQuit = true;
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    }
}

/* ------------------------------------------------------------------ */
/* Tray icon                                                           */
/* ------------------------------------------------------------------ */

static void TrayAdd()
{
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g_iconSmall;
    wcsncpy_s(nid.szTip, g_trayTip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

static void TrayRemove()
{
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void UpdateTrayTip()
{
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    wcsncpy_s(nid.szTip, g_trayTip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void ShowMain()
{
    if (IsIconic(g_hwnd)) {
        ShowWindow(g_hwnd, SW_RESTORE);
    } else {
        ShowWindow(g_hwnd, SW_SHOW);
    }
    SetForegroundWindow(g_hwnd);
}

/*
 * [Sess] Open; monitor volume up / down, mute, dim (the focused unit, as M and D); the recent sessions (the page's
 * list, session-recent; a pick loads it in the page, which asks first if it would turn 48V on); Quit. Mute and dim
 * carry a check mark from the page's trayState, the tooltip the level and the session.
 */
static void TrayMenu(int x, int y)
{
    HMENU m = CreatePopupMenu();
    HMENU recent = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_TRAY_OPEN, L"Open Apollo Control");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    UINT live = g_uiReady ? 0 : MF_GRAYED;
    AppendMenuW(m, MF_STRING | live, ID_TRAY_VOLUP, L"Monitor volume up");
    AppendMenuW(m, MF_STRING | live, ID_TRAY_VOLDOWN, L"Monitor volume down");
    AppendMenuW(m, MF_STRING | live | (g_trayMute ? MF_CHECKED : 0), ID_TRAY_MUTE, L"Mute monitor");
    AppendMenuW(m, MF_STRING | live | (g_trayDim ? MF_CHECKED : 0), ID_TRAY_DIM, L"Dim monitor");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < sesshost::g_recent.size() && i < 8; i++) {
        std::wstring label = sesshost::g_recent[i];
        std::wstring amp;
        for (wchar_t c : label) {
            amp += c;
            if (c == L'&') amp += L'&';         /* a literal & in a menu item */
        }
        AppendMenuW(recent, MF_STRING | live, ID_TRAY_RECENT + (UINT)i, amp.c_str());
    }
    if (sesshost::g_recent.empty()) {
        AppendMenuW(recent, MF_STRING | MF_GRAYED, 0, L"No sessions yet");
    }
    AppendMenuW(m, MF_POPUP | (sesshost::g_recent.empty() ? MF_GRAYED : 0), (UINT_PTR)recent, L"Recent sessions");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_TRAY_QUIT, L"Quit");
    SetMenuDefaultItem(m, ID_TRAY_OPEN, FALSE);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenuEx(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, x, y, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);                             /* and its submenu */
}

/* [Sess] A tray command for the page: {"type":"tray","action":"<action>"[,"name":"<session>"]}. */
static void TrayToPage(const wchar_t *action, const std::wstring *name = nullptr)
{
    if (!g_webview || !g_uiReady) {
        return;
    }
    std::wstring j = L"{\"type\":\"tray\",\"action\":";
    JsonString(j, action);
    if (name) {
        j += L",\"name\":";
        JsonString(j, *name);
    }
    j += L"}";
    g_webview->PostWebMessageAsJson(j.c_str());
}

/* ------------------------------------------------------------------ */
/* WebView2                                                            */
/* ------------------------------------------------------------------ */

static void ResizeWebView()
{
    if (g_controller) {
        RECT rc;
        GetClientRect(g_hwnd, &rc);
        g_controller->put_Bounds(rc);
    }
}

static void Capture()
{
    ComPtr<IStream> stream;
    if (FAILED(SHCreateStreamOnFileEx(g_optCapture.c_str(), STGM_CREATE | STGM_WRITE, FILE_ATTRIBUTE_NORMAL,
                                      TRUE, nullptr, &stream))) {
        PostQuitMessage(3);
        return;
    }
    g_webview->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream.Get(),
        Callback<ICoreWebView2CapturePreviewCompletedHandler>([stream](HRESULT hr) -> HRESULT {
            stream->Commit(STGC_DEFAULT);
            g_reallyQuit = true;
            PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
            return hr;
        }).Get());
}

static void Fatal(const wchar_t *what, HRESULT hr)
{
    std::wstring m = std::wstring(what) + L"\n\n" + ErrorText((DWORD)hr);
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
        m += L"\n\nThe Microsoft Edge WebView2 Runtime is missing. Install it from "
             L"https://developer.microsoft.com/microsoft-edge/webview2/ and start Apollo Control again.";
    }
    MessageBoxW(g_hwnd, m.c_str(), L"Apollo Control", MB_OK | MB_ICONERROR);
    g_reallyQuit = true;
    PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

static HRESULT OnController(HRESULT hr, ICoreWebView2Controller *ctl)
{
    if (FAILED(hr) || ctl == nullptr) {
        Fatal(L"Apollo Control could not start its window contents (WebView2).", hr);
        return S_OK;
    }
    g_controller = ctl;
    g_controller->get_CoreWebView2(&g_webview);

    ComPtr<ICoreWebView2Controller2> c2;
    if (SUCCEEDED(g_controller.As(&c2))) {
        c2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{ 255, 22, 24, 27 });
    }

    ComPtr<ICoreWebView2Settings> s;
    g_webview->get_Settings(&s);
    s->put_AreDevToolsEnabled(g_optDev);
    s->put_AreDefaultContextMenusEnabled(g_optDev);
    s->put_IsStatusBarEnabled(FALSE);
    s->put_IsZoomControlEnabled(FALSE);
    s->put_AreHostObjectsAllowed(FALSE);
    s->put_AreDefaultScriptDialogsEnabled(TRUE);
    ComPtr<ICoreWebView2Settings3> s3;
    if (SUCCEEDED(s.As(&s3))) {
        s3->put_AreBrowserAcceleratorKeysEnabled(g_optDev);
    }
    ComPtr<ICoreWebView2Settings4> s4;
    if (SUCCEEDED(s.As(&s4))) {
        s4->put_IsPasswordAutosaveEnabled(FALSE);
        s4->put_IsGeneralAutofillEnabled(FALSE);
    }

    std::wstring uiDir = ExeDir() + L"\\ui";
    ComPtr<ICoreWebView2_3> wv3;
    if (FAILED(g_webview.As(&wv3)) ||
        FAILED(wv3->SetVirtualHostNameToFolderMapping(UI_HOST, uiDir.c_str(),
                                                      COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS))) {
        Fatal(L"This WebView2 Runtime is too old for Apollo Control.", E_NOINTERFACE);
        return S_OK;
    }
    /* [Plug-ins] UA's plug-in icons from the local UAD install, read in place for <img> in the Plug-ins list
       (DENY_CORS: no fetch or canvas reads). Optional: without the install or the mapping the list has no art. */
    std::wstring artDir = pe::UadArtDir();
    if (!artDir.empty()) {
        (void)wv3->SetVirtualHostNameToFolderMapping(pe::UAD_ART_HOST, artDir.c_str(),
                                                     COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
    }

    EventRegistrationToken tok;
    g_webview->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
        [](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *a) -> HRESULT {
            LPWSTR uri = nullptr;
            a->get_Uri(&uri);
            if (uri == nullptr || _wcsnicmp(uri, UI_ORIGIN, wcslen(UI_ORIGIN)) != 0) {
                a->put_Cancel(TRUE);            /* the UI never leaves its own folder */
            }
            CoTaskMemFree(uri);
            return S_OK;
        }).Get(), &tok);
    g_webview->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
        [](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *a) -> HRESULT {
            a->put_Handled(TRUE);
            return S_OK;
        }).Get(), &tok);
    g_webview->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
        [](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *a) -> HRESULT {
            LPWSTR src = nullptr, msg = nullptr;
            a->get_Source(&src);
            bool trusted = src != nullptr && _wcsnicmp(src, UI_ORIGIN, wcslen(UI_ORIGIN)) == 0;
            CoTaskMemFree(src);
            if (trusted && SUCCEEDED(a->TryGetWebMessageAsString(&msg)) && msg != nullptr) {
                HandleUiMessage(msg);
            } else if (trusted) {
                /* [Twin X G2] an object posted by the UI (postMessage({cmd: ...})) */
                LPWSTR json = nullptr;
                if (SUCCEEDED(a->get_WebMessageAsJson(&json)) && json != nullptr && json[0] == L'{') {
                    HandleUiJson(json);
                }
                CoTaskMemFree(json);
            }
            CoTaskMemFree(msg);
            return S_OK;
        }).Get(), &tok);
    g_webview->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>(
        [](ICoreWebView2 *, ICoreWebView2ProcessFailedEventArgs *a) -> HRESULT {
            COREWEBVIEW2_PROCESS_FAILED_KIND kind;
            a->get_ProcessFailedKind(&kind);
            if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) {
                Fatal(L"The WebView2 browser process stopped.", E_FAIL);
            } else if (g_webview) {
                g_webview->Reload();
            }
            return S_OK;
        }).Get(), &tok);
    if (!g_optCapture.empty()) {
        g_webview->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *) -> HRESULT {
                SetTimer(g_hwnd, TIMER_CAPTURE, 2500, nullptr);
                return S_OK;
            }).Get(), &tok);
    }

    ResizeWebView();
    g_webview->Navigate(UI_ORIGIN L"index.html");
    return S_OK;
}

static void InitWebView()
{
    std::wstring data = KnownFolder(FOLDERID_LocalAppData) + L"\\Avium\\ApolloControl\\WebView2";
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, data.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT hr, ICoreWebView2Environment *env) -> HRESULT {
                if (FAILED(hr) || env == nullptr) {
                    Fatal(L"Apollo Control could not start WebView2.", hr);
                    return S_OK;
                }
                return env->CreateCoreWebView2Controller(g_hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(OnController).Get());
            }).Get());
    if (FAILED(hr)) {
        Fatal(L"Apollo Control could not start WebView2.", hr);
    }
}

/* ------------------------------------------------------------------ */
/* Window                                                              */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_msgTaskbarCreated && msg != 0) {
        TrayAdd();                              /* Explorer restarted */
        return 0;
    }
    switch (msg) {
    case WM_SIZE:
        ResizeWebView();
        return 0;
    case WM_MOVE:
    case WM_MOVING:
        if (g_controller) {
            g_controller->NotifyParentWindowPositionChanged();
        }
        break;
    case WM_SETFOCUS:
        if (g_controller) {
            g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        UINT dpi = GetDpiForWindow(hwnd);
        auto *mmi = (MINMAXINFO *)lp;
        mmi->ptMinTrackSize.x = MulDiv(980, (int)dpi, 96);
        mmi->ptMinTrackSize.y = MulDiv(640, (int)dpi, 96);
        return 0;
    }
    case WM_DPICHANGED: {
        auto *r = (RECT *)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_CAPTURE) {
            KillTimer(hwnd, TIMER_CAPTURE);
            Capture();
        }
        return 0;
    case WM_APP_JSON: {
        auto *s = (std::wstring *)lp;
        if (g_webview && g_uiReady) {
            g_webview->PostWebMessageAsJson(s->c_str());
        }
        delete s;
        return 0;
    }
    case WM_APP_SHOW:
        ShowMain();
        return 0;
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            if (IsWindowVisible(hwnd) && !IsIconic(hwnd) && GetForegroundWindow() == hwnd) {
                ShowWindow(hwnd, SW_HIDE);
            } else {
                ShowMain();
            }
            break;
        case WM_CONTEXTMENU:
            TrayMenu(GET_X_LPARAM(wp), GET_Y_LPARAM(wp));
            break;
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_TRAY_OPEN:
            ShowMain();
            break;
        case ID_TRAY_MUTE:
            TrayToPage(L"mute");
            break;
        case ID_TRAY_DIM:
            TrayToPage(L"dim");
            break;
        case ID_TRAY_VOLUP:
            TrayToPage(L"volup");
            break;
        case ID_TRAY_VOLDOWN:
            TrayToPage(L"voldown");
            break;
        case ID_TRAY_QUIT:
            g_reallyQuit = true;
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            break;
        default:
            if (LOWORD(wp) >= ID_TRAY_RECENT && LOWORD(wp) < ID_TRAY_RECENT + 8 &&
                (size_t)(LOWORD(wp) - ID_TRAY_RECENT) < sesshost::g_recent.size()) {
                std::wstring name = sesshost::g_recent[LOWORD(wp) - ID_TRAY_RECENT];
                TrayToPage(L"session", &name);
            }
            break;
        }
        return 0;
    case WM_APP_SESSDLG:
        sesshost::RunDialog(hwnd, RatePost);    /* [Sess] export / import: the file dialog, outside the WebView2 handler */
        return 0;
    case WM_CLOSE:
        SavePlacement();
        if (g_closeToTray && !g_reallyQuit) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_QUERYENDSESSION:
        SavePlacement();
        return TRUE;
    case WM_DESTROY:
        TrayRemove();
        if (g_controller) {
            g_controller->Close();
            g_controller.Reset();
            g_webview.Reset();
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int APIENTRY wWinMain(_In_ HINSTANCE inst, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int)
{
    g_inst = inst;

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc == 3 && wcscmp(argv[1], L"--plugin-admin") == 0) {
        /* [Plug-ins] the elevated step Apollo Control runs itself for (UAC): no window, no second-instance check */
        std::wstring req = argv[2];
        LocalFree(argv);
        return pe::AdminMain(req);
    }
    if (argv && argc == 4 && wcscmp(argv[1], L"--endpoints-admin") == 0) {
        /* [Endpoints] the elevated registry write endpoints-set runs (UAC): no window (endpoints_host.h) */
        std::wstring scope = argv[2], arg = argv[3];
        LocalFree(argv);
        return ephost::AdminMain(scope, arg);
    }
    for (int i = 1; argv && i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--minimized") g_optMinimized = true;
        else if (a == L"--demo") g_optDemo = true;
        else if (a == L"--dev") g_optDev = true;
        else if (a == L"--view" && i + 1 < argc) g_optView = argv[++i];
        else if (a == L"--capture" && i + 1 < argc) g_optCapture = argv[++i];
    }
    LocalFree(argv);
    g_closeToTray = !RunValue().empty();        /* [Sess] until the page says: close to the tray by default while it starts with Windows */

    HANDLE single = nullptr;
    if (g_optCapture.empty()) {
        single = CreateMutexW(nullptr, TRUE, L"Local\\AviumApolloControl");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            /* [Sess] one instance: a second start focuses the first (it may take the foreground: this start has it);
               a second --minimized start (the sign-in entry while it already runs) leaves it in the tray */
            HWND other = FindWindowW(WND_CLASS, nullptr);
            if (other && !g_optMinimized) {
                DWORD pid = 0;
                GetWindowThreadProcessId(other, &pid);
                AllowSetForegroundWindow(pid);
                PostMessageW(other, WM_APP_SHOW, 0, 0);
            }
            if (single) {
                CloseHandle(single);
            }
            return 0;
        }
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        return 1;
    }
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_iconBig = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                  GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    g_iconSmall = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                    LR_DEFAULTCOLOR);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = g_iconBig;
    wc.hIconSm = g_iconSmall;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(22, 24, 27));
    wc.lpszClassName = WND_CLASS;
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    g_hwnd = CreateWindowExW(0, WND_CLASS, L"Apollo Control", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             MulDiv(1240, (int)dpi, 96), MulDiv(800, (int)dpi, 96), nullptr, nullptr, inst,
                             nullptr);
    if (g_hwnd == nullptr) {
        return 1;
    }
    ApplyTheme(true, RGB(22, 24, 27), RGB(230, 230, 230));

    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_evWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_worker = std::thread(WorkerMain);
    if (g_optCapture.empty()) {
        g_evPump = std::thread(EventPumpMain);  /* [Events] */
    }
    if (!g_optDemo && g_optCapture.empty()) {
        pe::Start();                            /* [Plug-ins] saved inserts go back once the x8p's strips are live */
    }

    if (!RestorePlacement() && !g_optMinimized) {
        ShowWindow(g_hwnd, SW_SHOWNORMAL);
    }
    if (!g_optCapture.empty()) {
        ShowWindow(g_hwnd, SW_SHOWNORMAL);
    }
    UpdateWindow(g_hwnd);
    if (g_optCapture.empty()) {
        TrayAdd();
    }
    InitWebView();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    pe::Stop();                                 /* [Plug-ins] before the worker closes the slot handles */
    ratehost::Stop();                           /* [Rate] a rate-set under way finishes first */
    clockhost::Stop();                          /* [Clock] a clock-set under way finishes first */
    g_quit = true;
    SetEvent(g_wake);
    if (g_evWake != nullptr) {
        SetEvent(g_evWake);                     /* [Events] the pump cancels its waits and ends */
    }
    if (g_evPump.joinable()) {
        g_evPump.join();
    }
    if (g_worker.joinable()) {
        g_worker.join();
    }
    CloseHandle(g_wake);
    CoUninitialize();
    if (single) {
        ReleaseMutex(single);
        CloseHandle(single);
    }
    return (int)m.wParam;
}
