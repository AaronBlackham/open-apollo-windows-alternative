/*
 * session_host.h - [Sess] Apollo Control's sessions on disk (docs/apollo-control-units.md 1.11), included by main.cpp
 * after its helpers (Widen, Narrow, KnownFolder) and routed from HandleUiJson ("session-*").
 *
 *   session-list                      -> {"type":"sessions","dir":..,"list":[{name,file,saved,version,units,inserts,bad}],
 *                                         "last":{file,saved}|null}
 *   session-save {name, session}      -> writes sessions\<name>.json (atomically), {"type":"session-result","op":"save",..}
 *                                         and a fresh "sessions"
 *   session-load {name | file}        -> {"type":"session","op":"load","name","file","session":{..}} (the file as it is)
 *   session-load {last:true}          -> the same with op "last" (sessions\last-state.json), or a result "none"
 *   session-delete {name | file}      -> the file goes to the Recycle Bin; a result and a fresh "sessions"
 *   session-autosave {session}        -> sessions\last-state.json, {"type":"session-result","op":"autosave",..}
 *   session-export {name, session} | {file}   -> a Save As dialog, then the file written (or copied)
 *   session-import                    -> an Open dialog, then {"type":"session","op":"import","file","session"}
 *   session-recent {names}            -> the tray menu's recent sessions (the page keeps the list)
 *
 * The page owns a session's content (what is captured and how it is applied); this file lists, reads, writes and
 * deletes files, nothing else. A file is a UTF-8 JSON object with "format":"apollo-control-session" and an integer
 * "version"; whatever is not that is listed as unreadable and never sent as a session. The file dialogs run from the
 * window procedure (main.cpp WM_APP_SESSDLG), outside the WebView2 message handler. Nothing here goes on the network.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <shobjidl.h>
#include <string>
#include <vector>
#include <algorithm>

namespace sesshost {

using Post = void (*)(const std::string &Utf8Json);

static const char    *kFormat = "apollo-control-session";
static const wchar_t *kLastFile = L"last-state.json";
static const ULONGLONG kMaxBytes = 16ull << 20;

static std::vector<std::wstring> g_recent;      /* UI thread: the tray's recent sessions, as the page listed them */
static std::string g_dlgJson;                   /* UI thread: an export / import waiting for the window procedure */

static inline bool Owns(const std::wstring &cmd) { return cmd.rfind(L"session-", 0) == 0; }

static inline std::string Q(const std::string &s)
{
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

static inline std::wstring Dir()
{
    std::wstring d = KnownFolder(FOLDERID_RoamingAppData) + L"\\Avium\\ApolloControl\\sessions";
    SHCreateDirectoryExW(nullptr, d.c_str(), nullptr);
    return d;
}

/* A session name as a file name: no path characters, no device names, not the autosave's, at most 80 characters. */
static inline std::wstring FileOf(const std::string &nameUtf8)
{
    std::wstring w = Widen(nameUtf8.data(), nameUtf8.size()), o;
    for (wchar_t c : w) o += (c < 0x20 || wcschr(L"<>:\"/\\|?*", c)) ? L'_' : c;
    while (!o.empty() && (o.back() == L' ' || o.back() == L'.')) o.pop_back();
    while (!o.empty() && (o.front() == L' ' || o.front() == L'.')) o.erase(0, 1);
    if (o.size() > 80) o.resize(80);
    if (o.empty()) o = L"session";
    std::wstring up = o;
    for (wchar_t &c : up) c = (wchar_t)towupper(c);
    static const wchar_t *dev[] = { L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6",
                                    L"COM7", L"COM8", L"COM9", L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7",
                                    L"LPT8", L"LPT9" };
    for (const wchar_t *d : dev) {
        if (up == d || up.rfind(std::wstring(d) + L".", 0) == 0) { o = L"_" + o; break; }
    }
    if (up == L"LAST-STATE") o += L" (session)";
    return o + L".json";
}

/* A file name the page names (from a "sessions" list): a plain *.json in the folder, never a path. */
static inline bool PlainFile(const std::wstring &f)
{
    return !f.empty() && f.size() < 200 && f.find_first_of(L"\\/:*?\"<>|") == std::wstring::npos && f != L"." && f != L".." &&
           f.size() > 5 && _wcsicmp(f.c_str() + f.size() - 5, L".json") == 0;
}

static inline bool ReadUtf8(const std::wstring &path, std::string &out)
{
    out.clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && (ULONGLONG)size.QuadPart <= kMaxBytes;
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = ReadFile(f, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size();
    }
    CloseHandle(f);
    if (ok && out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF) {
        out.erase(0, 3);                                /* a BOM from an editor */
    }
    if (!ok) out.clear();
    return ok;
}

static inline DWORD WriteUtf8(const std::wstring &path, const std::string &data)
{
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD put = 0, err = 0;
    if (!WriteFile(f, data.data(), (DWORD)data.size(), &put, nullptr) || put != data.size() || !FlushFileBuffers(f)) err = GetLastError();
    CloseHandle(f);
    if (err == 0 && !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) err = GetLastError();
    if (err != 0) DeleteFileW(tmp.c_str());
    return err;
}

/* The text is one session object: the format word and an integer version. */
static inline bool IsSession(const aggjson::Value &v)
{
    const aggjson::Value *ver = v.Get("version");
    return v.kind == aggjson::Value::Object && v.Str("format") == kFormat && ver && ver->kind == aggjson::Value::Number &&
           ver->n >= 1 && ver->n == (double)(long)ver->n;
}

static inline std::string Err(DWORD e)
{
    return Narrow(ErrorText(e));
}

static inline std::string Result(const char *op, bool ok, const std::string &name, const std::wstring &file, const std::string &why,
                                 bool cancelled = false)
{
    std::wstring f = file;
    return std::string("{\"type\":\"session-result\",\"op\":\"") + op + "\",\"ok\":" + (ok ? "true" : "false") +
           ",\"cancelled\":" + (cancelled ? "true" : "false") + ",\"name\":" + Q(name) + ",\"file\":" + Q(Narrow(f)) +
           ",\"why\":" + Q(why) + "}";
}

/* One list entry: what the page shows without opening the session (its name, when, which units, how many inserts). */
static inline std::string Entry(const std::wstring &file, const std::string &text, const FILETIME &mt)
{
    aggjson::Value v;
    aggjson::Parser p(text);
    std::string fileU = Narrow(file), base = fileU.substr(0, fileU.size() - 5);
    SYSTEMTIME st{};
    FileTimeToSystemTime(&mt, &st);
    char iso[40];
    snprintf(iso, sizeof(iso), "%04u-%02u-%02uT%02u:%02u:%02u.000Z", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::string j = "{\"file\":" + Q(fileU) + ",\"modified\":" + Q(iso);
    if (!p.Parse(v) || !IsSession(v)) {
        return j + ",\"name\":" + Q(base) + ",\"saved\":" + Q(iso) + ",\"version\":0,\"units\":[],\"inserts\":0,\"bad\":\"not an Apollo Control session file\"}";
    }
    std::string name = v.Str("name");
    j += ",\"name\":" + Q(name.empty() ? base : name) + ",\"saved\":" + Q(v.Str("saved", iso)) + ",\"version\":" +
         std::to_string((long)v.Num("version")) + ",\"units\":[";
    size_t inserts = 0;
    bool first = true;
    if (const aggjson::Value *us = v.Get("units")) {
        for (const aggjson::Value &u : us->a) {
            if (u.kind != aggjson::Value::Object) continue;
            j += first ? "{" : ",{";
            first = false;
            j += "\"id\":" + Q(u.Str("id")) + ",\"model\":" + Q(u.Str("model")) + ",\"deviceType\":" + std::to_string((long)u.Num("deviceType")) +
                 ",\"rate\":" + std::to_string((long)u.Num("rate")) + "}";
            if (const aggjson::Value *pl = u.Get("plugins")) {
                if (const aggjson::Value *ch = pl->Get("chains")) {
                    for (const aggjson::Value &c : ch->a) {
                        if (const aggjson::Value *sl = c.Get("slots")) inserts += sl->a.size();
                    }
                }
            }
        }
    }
    return j + "],\"inserts\":" + std::to_string(inserts) + ",\"bad\":\"\"}";
}

static inline std::string ListJson()
{
    std::wstring dir = Dir();
    struct Item { std::string saved, json; };
    std::vector<Item> items;
    std::string last = "null";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.json").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring f = fd.cFileName;
            if (!PlainFile(f) || items.size() >= 500) continue;
            std::string text;
            ReadUtf8(dir + L"\\" + f, text);
            std::string e = Entry(f, text, fd.ftLastWriteTime);
            if (_wcsicmp(f.c_str(), kLastFile) == 0) { last = e; continue; }
            aggjson::Value v;
            aggjson::Parser p(e);
            std::string saved = p.Parse(v) ? v.Str("saved") : std::string();
            items.push_back({ saved, e });
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.saved > b.saved; });
    std::string j = "{\"type\":\"sessions\",\"dir\":" + Q(Narrow(dir)) + ",\"list\":[";
    for (size_t i = 0; i < items.size(); i++) j += (i ? "," : "") + items[i].json;
    return j + "],\"last\":" + last + "}";
}

/* The file a command names: its "file" (from a list) when it is a plain name in the folder, else its name's. */
static inline std::wstring Target(const aggjson::Value &v)
{
    std::string f = v.Str("file");
    std::wstring w = Widen(f.data(), f.size());
    return PlainFile(w) ? w : FileOf(v.Str("name"));
}

/* {"type":"session",...,"session":<text>} for a file that is a session, else a failed result. */
static inline std::string SessionMsg(const char *op, const std::wstring &path, const std::wstring &shown, const std::string &name)
{
    std::string text;
    if (!ReadUtf8(path, text)) {
        DWORD e = GetLastError();
        return Result(op, false, name, shown, e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? "none" : Err(e));
    }
    aggjson::Value v;
    aggjson::Parser p(text);
    if (!p.Parse(v) || !IsSession(v)) return Result(op, false, name, shown, "not an Apollo Control session file");
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return std::string("{\"type\":\"session\",\"op\":\"") + op + "\",\"name\":" + Q(v.Str("name", name.c_str())) + ",\"file\":" +
           Q(Narrow(shown)) + ",\"session\":" + text + "}";
}

static inline void Handle(const std::string &json, HWND hwnd, UINT dlgMsg, Post post)
{
    aggjson::Value v;
    aggjson::Parser p(json);
    if (!p.Parse(v) || v.kind != aggjson::Value::Object) return;
    std::string cmd = v.Str("cmd"), name = v.Str("name");
    const aggjson::Value *s = v.Get("session");
    if (cmd == "session-list") {
        post(ListJson());
    } else if (cmd == "session-save" || cmd == "session-autosave") {
        bool autosave = cmd == "session-autosave";
        const char *op = autosave ? "autosave" : "save";
        std::wstring file = autosave ? std::wstring(kLastFile) : FileOf(name);
        if (!autosave && name.empty()) { post(Result(op, false, name, file, "a session needs a name")); return; }
        if (!s || !IsSession(*s)) { post(Result(op, false, name, file, "not a session")); return; }
        DWORD e = WriteUtf8(Dir() + L"\\" + file, json.substr(s->begin, s->end - s->begin) + "\n");
        post(Result(op, e == 0, name, file, e ? Err(e) : std::string()));
        if (!autosave) post(ListJson());
    } else if (cmd == "session-load") {
        const aggjson::Value *last = v.Get("last");
        bool isLast = last && last->kind == aggjson::Value::Bool && last->b;
        std::wstring file = isLast ? std::wstring(kLastFile) : Target(v);
        post(SessionMsg(isLast ? "last" : "load", Dir() + L"\\" + file, file, name));
    } else if (cmd == "session-delete") {
        std::wstring file = Target(v);
        if (_wcsicmp(file.c_str(), kLastFile) == 0) { post(Result("delete", false, name, file, "the last state is kept by Apollo Control")); return; }
        std::wstring path = Dir() + L"\\" + file;
        path.push_back(L'\0');                          /* SHFileOperation: a double-null-terminated list */
        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = path.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NO_UI;
        int r = GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES ? ERROR_FILE_NOT_FOUND : SHFileOperationW(&op);
        post(Result("delete", r == 0 && !op.fAnyOperationsAborted, name, file,
                    r == 0 ? std::string() : r == ERROR_FILE_NOT_FOUND ? "the file is not there any more" : "Windows did not delete it (" + std::to_string(r) + ")"));
        post(ListJson());
    } else if (cmd == "session-export" || cmd == "session-import") {
        g_dlgJson = json;                               /* the window procedure shows the dialog (not inside the WebView2 handler) */
        PostMessageW(hwnd, dlgMsg, 0, 0);
    } else if (cmd == "session-recent") {
        g_recent.clear();
        if (const aggjson::Value *n = v.Get("names")) {
            for (const aggjson::Value &x : n->a) {
                if (x.kind == aggjson::Value::String && !x.s.empty() && g_recent.size() < 8) g_recent.push_back(Widen(x.s.data(), x.s.size()));
            }
        }
    }
}

/* The Save As / Open dialog of an export or import (UI thread, from the window procedure). */
static inline void RunDialog(HWND owner, Post post)
{
    std::string json;
    json.swap(g_dlgJson);
    aggjson::Value v;
    aggjson::Parser p(json);
    if (json.empty() || !p.Parse(v)) return;
    bool exp = v.Str("cmd") == "session-export";
    const char *op = exp ? "export" : "import";
    std::string name = v.Str("name");
    static const COMDLG_FILTERSPEC types[] = { { L"Apollo Control session (*.json)", L"*.json" }, { L"All files", L"*.*" } };
    Microsoft::WRL::ComPtr<IFileDialog> d;
    HRESULT hr = CoCreateInstance(exp ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d));
    if (FAILED(hr)) { post(Result(op, false, name, L"", "the file dialog did not open")); return; }
    d->SetFileTypes(2, types);
    d->SetDefaultExtension(L"json");
    FILEOPENDIALOGOPTIONS o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_FORCEFILESYSTEM | (exp ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));
    d->SetTitle(exp ? L"Export the session" : L"Import a session");
    if (exp) {
        std::wstring f = FileOf(name.empty() ? std::string("session") : name);
        d->SetFileName(f.c_str());
    }
    hr = d->Show(owner);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) { post(Result(op, false, name, L"", "", true)); return; }
    Microsoft::WRL::ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(hr) || FAILED(d->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || path == nullptr) {
        post(Result(op, false, name, L"", "no file was chosen"));
        return;
    }
    std::wstring file = path;
    CoTaskMemFree(path);
    if (!exp) {
        post(SessionMsg("import", file, file, name));
        return;
    }
    std::string text;
    const aggjson::Value *s = v.Get("session");
    if (s && IsSession(*s)) {
        text = json.substr(s->begin, s->end - s->begin) + "\n";
    } else {
        std::wstring src = Dir() + L"\\" + Target(v);
        aggjson::Value sv;
        if (!ReadUtf8(src, text)) { post(Result(op, false, name, file, "the session file could not be read")); return; }
        aggjson::Parser sp(text);
        if (!sp.Parse(sv) || !IsSession(sv)) { post(Result(op, false, name, file, "not an Apollo Control session file")); return; }
    }
    DWORD e = WriteUtf8(file, text);
    post(Result(op, e == 0, name, file, e ? Err(e) : std::string()));
}

}   /* namespace sesshost */
