/*
 * diag_host.h - [Diag] "Save diagnostics": one folder with what a fault report needs, written on this PC only and
 * never sent anywhere (the open-apollo parity row "collect a diagnostics bundle"). The page posts {"cmd":"diag-save"};
 * the folder is %LOCALAPPDATA%\Avium\ApolloControl\diagnostics\<date-time>\ and Explorer opens on it.
 *
 *   driver-log.txt         the driver's log ring (GET_LOG, every unit's lines, tagged)
 *   unit-<id>.txt          per unit: identity and state (GET_INFO), the readback words, the rate, clock, front panel and
 *                          driver Parameters as Apollo Control reads them, and the unit's event ring (WAIT_EVENTS, NOWAIT)
 *   app\config.json, app\plugins.json, app\plughelper.log, app\apollo-control.log    Apollo Control's own files
 *   scripts\*.log          the install and test scripts' logs (tools\phase1), when this is a source-tree build
 *
 * Reply: {"type":"diag-result","ok":true|false,"dir":"...","why":"..."}.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <string>
#include <vector>
#include "..\..\include\apollo_events.h"

namespace diaghost {

using Post = void (*)(const std::string &Utf8Json);

static inline bool Owns(const std::wstring &Cmd)
{
    return Cmd == L"diag-save";
}

static inline std::wstring Folder(REFKNOWNFOLDERID id)
{
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) s = p;
    CoTaskMemFree(p);
    return s;
}

static inline bool WriteText(const std::wstring &Path, const std::string &Text)
{
    HANDLE f = CreateFileW(Path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    BOOL ok = WriteFile(f, Text.data(), (DWORD)Text.size(), &w, nullptr);
    CloseHandle(f);
    return ok && w == Text.size();
}

static inline std::string Fmt(const char *f, ...)
{
    char b[512];
    va_list a;
    va_start(a, f);
    vsnprintf(b, sizeof(b), f, a);
    va_end(a);
    return b;
}

static inline std::string Narrow8(const std::wstring &w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static inline const char *EventName(ULONG k)
{
    switch (k) {
    case APOLLO_EVENT_STAGE: return "stage";           case APOLLO_EVENT_LEAVING: return "leaving";
    case APOLLO_EVENT_RATE: return "rate";             case APOLLO_EVENT_CLOCK: return "clock";
    case APOLLO_EVENT_PANEL: return "panel";           case APOLLO_EVENT_DSP_FAULT: return "DSP fault";
    case APOLLO_EVENT_ENGINE_LOST: return "engine lost"; case APOLLO_EVENT_PARAMS: return "params";
    default: return "?";
    }
}

static inline std::string UnitReport(const std::string &Id)
{
    APOLLO_INFO i{};
    DWORD err = 0, ret = 0;
    HANDLE h = corrhost::OpenUnit(Id, i, err);
    std::string t = "Unit " + Id + "\r\n";

    if (h == INVALID_HANDLE_VALUE) {
        return t + Fmt("not opened: error %lu\r\n", err);
    }
    DWORD bytes = 0;
    if (ApolloQueryInfo(h, &i, &bytes) == 0) {
        t += Fmt("serial %.16s  device type 0x%02lX public 0x%02lX  model %lu  mode %lu\r\n", i.Serial, i.DeviceType,
                 i.PublicType, i.Model, i.Mode);
        t += Fmt("stage %lu (failed at %lu, last status 0x%08lX)  flags 0x%08lX  unit flags 0x%08lX\r\n", i.Stage,
                 i.FailedStage, i.LastStatus, i.Flags, i.UnitFlags);
        t += Fmt("firmware %lu.%lu.%lu  FPGA 0x%08lX  ext caps 0x%08lX  subsystem 0x%04lX  DSPs %lu  preamps %lu\r\n",
                 i.FwRelease >> 24, (i.FwRelease >> 16) & 0xFF, i.FwRelease & 0xFFFF, i.FpgaRev, i.ExtCaps, i.SubsystemId,
                 i.NumDsps, i.NumPreamps);
        t += Fmt("rate %lu Hz  slots rec %lu play %lu  ring %lu  service passes %lu  interrupts %lu  notif seen 0x%08lX  "
                 "interface v%lu\r\n", i.SampleRate, i.RecChannels, i.PlayChannels, i.RingFrames, i.ServicePasses,
                 i.InterruptCount, i.NotifStatusSeen, i.Version);
    }
    APOLLO_READBACK rb{};
    if (DeviceIoControl(h, IOCTL_APOLLO_GET_READBACK, nullptr, 0, &rb, sizeof(rb), &ret, nullptr)) {
        t += Fmt("\r\nreadback valid %lu sequence %lu\r\n", rb.Valid, rb.Sequence);
        for (ULONG w = 0; w < APOLLO_READBACK_WORDS; w++) {
            t += Fmt("%s%08lX", (w % 8 == 0) ? Fmt("%s  [%2lu] ", w ? "\r\n" : "", w).c_str() : " ", rb.Words[w]);
        }
        t += "\r\n";
    }
    APOLLO_EVENTS_WAIT q{ APOLLO_EVENTS_VERSION, 0, APOLLO_EVENTS_WAIT_NOWAIT, 0 };
    APOLLO_EVENTS ev{};
    if (DeviceIoControl(h, IOCTL_APOLLO_WAIT_EVENTS, &q, sizeof(q), &ev, sizeof(ev), &ret, nullptr) && ret >= sizeof(ev)) {
        t += Fmt("\r\nevents (newest #%lu, %lu older ones no longer held)\r\n", ev.Seq, ev.Lost);
        for (ULONG k = 0; k < ev.Count && k < APOLLO_EVENTS_MAX; k++) {
            const APOLLO_EVENT_ENTRY &e = ev.Entry[k];
            FILETIME ft{ (DWORD)e.Time, (DWORD)(e.Time >> 32) }, lt;
            SYSTEMTIME st;
            FileTimeToLocalFileTime(&ft, &lt);
            FileTimeToSystemTime(&lt, &st);
            t += Fmt("  #%-5lu %02u:%02u:%02u.%03u %-12s A=%lu (0x%lX) B=0x%08lX\r\n", e.Seq, st.wHour, st.wMinute,
                     st.wSecond, st.wMilliseconds, EventName(e.Kind), e.A, e.A, e.B);
        }
    } else {
        t += "\r\nevents: not available (an older driver)\r\n";
    }
    CloseHandle(h);
    t += "\r\nrate:   " + ratehost::StateMsg(Id) + "\r\n";
    t += "clock:  " + clockhost::ClockMsg(Id) + "\r\n";
    t += "panel:  " + clockhost::PanelMsg(Id) + "\r\n";
    t += "params: " + paramshost::StateMsg(Id) + "\r\n";
    return t;
}

static inline void CopyIf(const std::wstring &From, const std::wstring &To, int &copied)
{
    if (GetFileAttributesW(From.c_str()) != INVALID_FILE_ATTRIBUTES && CopyFileW(From.c_str(), To.c_str(), FALSE)) copied++;
}

static inline void Save(Post post, const std::wstring &AppVersion)
{
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t stamp[32];
    swprintf_s(stamp, L"%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    std::wstring dir = Folder(FOLDERID_LocalAppData) + L"\\Avium\\ApolloControl\\diagnostics\\" + stamp;
    if (SHCreateDirectoryExW(nullptr, (dir + L"\\app").c_str(), nullptr) != ERROR_SUCCESS) {
        post("{\"type\":\"diag-result\",\"ok\":false,\"why\":\"could not create the folder\"}");
        return;
    }
    int files = 0;

    /* the driver's log, from any unit's handle (one ring for all) */
    std::vector<std::string> ids = ratehost::UnitIds();
    std::string summary = "Apollo Control " + Narrow8(AppVersion) + " diagnostics, " + Narrow8(stamp) + "\r\n";
    summary += Fmt("%zu unit(s) on apollo.sys\r\n", ids.size());
    for (const std::string &id : ids) {
        APOLLO_INFO i{};
        DWORD err = 0;
        HANDLE h = corrhost::OpenUnit(id, i, err);
        if (h == INVALID_HANDLE_VALUE) continue;
        std::vector<BYTE> buf(sizeof(APOLLO_LOG_HEADER) + APOLLO_LOG_BYTES);
        DWORD ret = 0;
        if (DeviceIoControl(h, IOCTL_APOLLO_GET_LOG, nullptr, 0, buf.data(), (DWORD)buf.size(), &ret, nullptr) &&
            ret >= sizeof(APOLLO_LOG_HEADER)) {
            auto *hdr = (PAPOLLO_LOG_HEADER)buf.data();
            ULONG len = (ULONG)std::min<size_t>(hdr->Length, ret - sizeof(*hdr));
            files += WriteText(dir + L"\\driver-log.txt", std::string((const char *)(hdr + 1), len)) ? 1 : 0;
        }
        CloseHandle(h);
        break;
    }
    for (const std::string &id : ids) {
        files += WriteText(dir + L"\\unit-" + std::wstring(id.begin(), id.end()) + L".txt", UnitReport(id)) ? 1 : 0;
    }
    /* Apollo Control's own files */
    std::wstring roam = Folder(FOLDERID_RoamingAppData) + L"\\Avium\\ApolloControl", local = Folder(FOLDERID_LocalAppData) +
                        L"\\Avium\\ApolloControl";
    CopyIf(roam + L"\\config.json", dir + L"\\app\\config.json", files);
    CopyIf(roam + L"\\plugins.json", dir + L"\\app\\plugins.json", files);
    CopyIf(local + L"\\plughelper.log", dir + L"\\app\\plughelper.log", files);
    CopyIf(local + L"\\apollo-control.log", dir + L"\\app\\apollo-control.log", files);     /* [Log] */
    CopyIf(local + L"\\apollo-control.log.1", dir + L"\\app\\apollo-control.log.1", files);
    {   /* [Crash] the newest minidump of Apollo Control, if any */
        WIN32_FIND_DATAW fd;
        std::wstring newest;
        HANDLE f = FindFirstFileW((local + L"\\crashes\\apollo-control-*.dmp").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do { if (newest.empty() || _wcsicmp(fd.cFileName, newest.c_str()) > 0) newest = fd.cFileName; } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        if (!newest.empty()) CopyIf(local + L"\\crashes\\" + newest, dir + L"\\app\\" + newest, files);
    }
    /* a source-tree build: the scripts' logs (app\bin\apollo-control.exe -> tools\phase1) */
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        std::wstring p1 = exe;
        for (int up = 0; up < 3; up++) p1 = p1.substr(0, p1.find_last_of(L'\\'));
        p1 += L"\\tools\\phase1";
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((p1 + L"\\*.log").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            SHCreateDirectoryExW(nullptr, (dir + L"\\scripts").c_str(), nullptr);
            do {
                CopyIf(p1 + L"\\" + fd.cFileName, dir + L"\\scripts\\" + fd.cFileName, files);
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
    }
    OSVERSIONINFOEXW os{ sizeof(os) };
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOEXW *);
    auto rgv = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (rgv && rgv(&os) == 0) summary += Fmt("Windows %lu.%lu build %lu\r\n", os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber);
    summary += Fmt("%d file(s) written\r\n", files + 1);
    WriteText(dir + L"\\README.txt", summary);
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    post("{\"type\":\"diag-result\",\"ok\":true,\"files\":" + std::to_string(files + 1) + ",\"dir\":" + corrhost::Q(Narrow8(dir)) + "}");
}

}   /* namespace diaghost */
