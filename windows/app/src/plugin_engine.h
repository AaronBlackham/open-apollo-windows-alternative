/*
 * plugin_engine.h - UAD plug-in inserts in Apollo Control (docs/PLUGINS.md 5), included by main.cpp
 * after the device worker (it uses g_slots, SlotIoctl, SlotInfo, PostJson). Per unit (P7): the x8p
 * (slot 0) and a Twin X Gen 1 (its slot), as the driver hosts them (each unit's strip DSP, DSP 1).
 *
 * One thread owns every plug-in operation, so the device worker's polls and meters never wait on
 * a CREATE: it talks to apollo.sys through each unit's own slot handle (IOCTLs 0x40-0x42; instances
 * belong to that handle, so closing it, a driver update or quitting the app restores the plain
 * strips), reads each unit's own licence answer (BOARD_QUERY, kept in memory only, never logged or saved),
 * builds recipes in C++ (plugin_host.h: no Python at start-up), and starts the parameter planner
 * (tools/plugin-host/plughelper.py, the plug-in's own setter in open-apollo's offline emulator)
 * only when a parameter changes, quitting it again after 5 idle minutes.
 *
 * Chains are saved per unit and input in %APPDATA%\Avium\ApolloControl\plugins.json (version 2: REC/MON, the
 * slots with plug-in, bundle, its SHA-256, the parameter values and texts, the accepted control words) and put back
 * whenever that unit's strips are Live and the driver owns its strip DSP: a cold start, or this app starting, and
 * [Warm] a warm start whose strips the driver adopted or rebuilt (FEAT_WARM, docs/STRIPS.md 12.5). Otherwise they
 * wait with the driver's reason (caps Dsp[].Flags APOLLO_PLUGIN_DSP_WHY) and the one action that ends it, a power
 * cycle. Nothing here contacts Universal Audio or loads a UA program.
 *
 * [Ins] Chains (docs/apollo-control-units.md 1.9): per unit and input (or linked pair, or aux bus) up to four slots,
 * composed into one apollo.sys chain in slot order (plugin_host.h BuildChain, upstream's compose()); add, remove, move,
 * bypass and replace rebuild it (staged next to the running one when DSP 1 has room for both, so the plain strip is heard
 * only between the old chain's DESTROY and the new one's START); per chain UAD REC / MON (the record tap: a live SERVICE
 * flip, or a rebuild when the record delay moves); DSP room (code pool) and a cycle budget (DSP 1's load word) per chain;
 * the Unison slot (CREATE F_UNISON, the analog lease: BEGIN, ACTIVATE, CUSTOM, gain and impedance triples; no Unison map
 * exists yet, so it waits with code "unison"); inserts on the aux returns (experimental, Parameters\AuxInserts).
 *
 * Plug-in hosting on/off and the module store need an administrator: Apollo Control runs itself
 * elevated once (--plugin-admin <request file>, UAC), which writes Parameters\PluginHost and
 * PluginHostImage (this exe's own NT path: the driver then takes the plug-in calls from this
 * program only, or from elevated tools) and copies verified module records into
 * System32\drivers\ApolloModules.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include "plugin_host.h"
#include <sddl.h>
#include <aclapi.h>
#include <list>

namespace pe {

using plughost::Words;
using plughost::RW;

/* ------------------------------------------------------------------ state */

/*
 * [Ins] One slot of a chain: what is saved (plug-in, bundle, bypass, values, texts, accepted words) and what it is now.
 * ChainSlot 99 is the Unison preamp slot's key (Chain::unison).
 */
struct ChainSlot : plughost::SavedSlot {
    std::vector<plughost::Param> params;
    ULONG latency = 0, codeWords = 0, pages = 0, base = 0;  /* base: its root row in the running chain, 0 = not in it */
    std::string state = "waiting", code, why;               /* running, bypassed, waiting, failed */
    bool helper = false;
};

static const ULONG kUnisonSlot = 99;
/* [Ins] preamp fields of the Unison lease (driver\preamp_plan.h APOLLO_PRE_F_*) and its settling time (unison_plan.h) */
static const ULONG kPreGainC = 0x06, kPreMicImp = 0x07, kPreCustom = 0x08, kPreGainB = 0x09, kPreGainA = 0x0A;
static const DWORD kUnisonSettleMs = 80;

/*
 * [Ins] One input's chain (or a linked pair's, or an aux bus's): up to four slots, UAD REC / MON, the Unison slot. One
 * apollo.sys instance runs the active slots in slot order (plugin_host.h BuildChain); a change rebuilds it.
 */
struct Chain {
    std::string unit;
    ULONG input = 0;
    LONG aux = -1;                          /* 0/1: aux bus (stereo); -1: an analog input */
    bool stereo = false, mon = false;
    std::vector<ChainSlot> slots;                /* by slot number, ascending */
    bool hasUnison = false;
    ChainSlot unison;
    /* live (plug-in thread) */
    int slot = -1;                          /* the g_slots index whose handle holds it */
    ULONG handle = 0, epoch = 0, gen = 0, triedEpoch = 0, missing = 0;
    std::string state = "idle", code, why, busy;
    plughost::ChainRecipe run;              /* the running chain's layout (bases, taps, delays); its big vectors cleared */
    std::vector<ULONG> running;             /* the slots in it, in order (kUnisonSlot first when a Unison header leads) */
    bool runMon = false, analog = false;    /* its REC/MON as built; it holds the Unison analog lease */
    std::set<RW> owned;                     /* DSP-owned words: the recipe's and those seen moving */
    std::map<std::string, ULONG> mods;      /* its modules (SHA-256 -> code words), for the room check */
    ULONG loadBefore = 0, loadAfter = 0;    /* DSP load word (1/100 %) before and after its last build */
    std::string key() const { return unit + "|" + (aux >= 0 ? "aux" + std::to_string(aux) : std::to_string(input)); }
    std::string slotKey(ULONG s) const { return key() + "|" + (s == kUnisonSlot ? std::string("unison") : std::to_string(s)); }
    bool covers(ULONG in) const { return aux < 0 && (input == in || (stereo && input + 1 == in)); }
    bool active() const
    {
        for (const ChainSlot &s : slots) if (!s.bypass) return true;
        return hasUnison && !unison.bypass;
    }
};

struct Cmd {
    std::string op, name, dir;
    ULONG unit = 0, input = 0, index = 0, value = 0;  /* unit: the slot ("unit" of the messages), 0 = the x8p */
    bool on = false, stereo = false;
    /* [Ins] */
    LONG aux = -1, db = 0;
    ULONG slot = 0, from = 0, to = 0, code = 0;
    bool hasSlot = false, mon = false;
    std::vector<ULONG> words;
    std::string text, unitId;               /* [Sess] session-set: the message (its "chains") and the unit id it names */
};

struct CatEntry {
    std::string file, layout;
    ULONGLONG size = 0, mtime = 0;
    plughost::CatalogItem item;
    std::vector<std::string> modules;       /* the plug-in's own module SHA-256s (architectures 0 and 2) */
};

static std::mutex           g_cmdLock;
static std::deque<Cmd>      g_cmds;
static HANDLE               g_wake;
static std::thread          g_thread;
static std::atomic<bool>    g_quit{ false };
static std::atomic<bool>    g_watch{ false };       /* the Plug-ins view is open: refresh each second */
/* [P7] the page reads "unit" on plug-in messages (it said so: "units":true on a plug-* command); until then only
   slot 0's go out, so a page from before P7 never takes a Twin X snapshot for the x8p's */
static std::atomic<bool>    g_multi{ false };

/* plug-in thread only */
static std::list<Chain>     g_chains;               /* [Ins] every unit's chains */
static std::string          g_mapsDir;              /* configured (plugins.json), '' = find it */
static std::wstring         g_plugins;              /* resolved apollo-win\plugins, '' = none */
static std::string          g_mapsWhy;

/*
 * [P7] Per unit: what the driver says about it and its own licence answer. Each unit that hosts (the
 * x8p in slot 0, a Twin X in the slot the worker gave it) is worked on through its own slot handle;
 * g_u is the one being worked on now (plug-in thread only), and the single-unit names below read it,
 * so everything else in this file reads as it did for the x8p alone. #undef'd at the end.
 */
struct UnitState {
    int                 slot = 0;               /* g_slots index = the "unit" tag of the plug-in messages */
    const PLUG_MODEL   *model = nullptr;        /* its hosting plan (plugin_plan.h); null = this slot holds no unit that hosts */
    APOLLO_PLUGIN_CAPS  caps{};
    bool                capsOk = false;
    DWORD               capsErr = 0;
    APOLLO_INFO         info{};
    DWORD               infoBytes = 0;
    bool                present = false;
    std::string         unitId;
    ULONG               gen = 0;
    std::vector<ULONG>  lic;                    /* its board table, in memory only */
    std::string         licState = "unknown", licWhy;
    ULONG               licEpoch = 0;
    ULONGLONG           licAt = 0;
    std::string         lastState, lastList;
};
static UnitState            g_unitState[MAX_UNITS];
static UnitState           *g_u = &g_unitState[0];
#define g_caps      (g_u->caps)
#define g_capsOk    (g_u->capsOk)
#define g_capsErr   (g_u->capsErr)
#define g_info      (g_u->info)
#define g_infoBytes (g_u->infoBytes)
#define g_present   (g_u->present)
#define g_unitId    (g_u->unitId)
#define g_gen       (g_u->gen)
#define g_lic       (g_u->lic)
#define g_licState  (g_u->licState)
#define g_licWhy    (g_u->licWhy)
#define g_licEpoch  (g_u->licEpoch)
#define g_licAt     (g_u->licAt)
#define g_lastState (g_u->lastState)
#define g_lastList  (g_u->lastList)

/* The unit a message names (its slot), else the x8p's slot 0. */
static UnitState &UnitAt(ULONG slot) { return g_unitState[slot < (ULONG)MAX_UNITS ? slot : 0]; }

/* "x8p", "Twin X": the unit being worked on, in the UI's words */
static std::string Nm() { return g_u->model ? g_u->model->Name : (g_u->slot == 0 ? "x8p" : "unit"); }

static std::vector<plughost::IndexEntry> g_index;
static std::map<std::string, CatEntry>   g_cat;
static size_t               g_catNext;
static bool                 g_catBuilding, g_catDirty, g_listWanted;
static std::string          g_admin;                /* an elevated step under way */
static HANDLE               g_helperProc, g_helperIn, g_helperOut, g_helperJob;
static ULONGLONG            g_helperUsed;
static ULONG                g_helperId;

static const ULONGLONG HELPER_IDLE_MS = 5 * 60 * 1000;
static const DWORD HELPER_REPLY_MS = 90000;         /* the emulator starts a heavy plug-in in seconds */

/* ------------------------------------------------------------------ small helpers */

static std::string U8(const std::wstring &w) { return Narrow(w); }
static std::wstring W(const std::string &s) { return Widen(s.data(), s.size()); }
static std::string Q(const std::string &s) { return plughost::JsonEsc(s); }

static bool ReadAll(const std::wstring &path, std::string &out, ULONGLONG maxBytes = 64ull << 20)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    out.clear();
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(f, &sz) && sz.QuadPart >= 0 && (ULONGLONG)sz.QuadPart <= maxBytes;
    if (ok) {
        out.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        ok = sz.QuadPart == 0 || (ReadFile(f, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size());
    }
    CloseHandle(f);
    if (!ok) out.clear();
    return ok;
}

static bool WriteAtomic(const std::wstring &path, const std::string &data)
{
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    BOOL ok = WriteFile(f, data.data(), (DWORD)data.size(), &put, nullptr) && put == data.size() && FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

static bool FileStamp(const std::wstring &path, ULONGLONG &size, ULONGLONG &mtime)
{
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return false;
    size = ((ULONGLONG)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    mtime = ((ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
    return true;
}

static bool Exists(const std::wstring &p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

static std::wstring AppDataDir(REFKNOWNFOLDERID id)
{
    std::wstring dir = KnownFolder(id) + L"\\Avium\\ApolloControl";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir;
}

static std::wstring SavePath() { return AppDataDir(FOLDERID_RoamingAppData) + L"\\plugins.json"; }
static std::wstring CachePath() { return AppDataDir(FOLDERID_LocalAppData) + L"\\plugin-catalog.json"; }

/* The plug-in maps: the configured folder, %LOCALAPPDATA%\Avium\ApolloPlugins, or apollo-win\plugins above the exe. */
static std::wstring FindPlugins(std::string &why)
{
    auto ok = [](const std::wstring &d) { return !d.empty() && Exists(d + L"\\bundles\\index.json") && Exists(d + L"\\store\\modules"); };
    if (!g_mapsDir.empty()) {
        std::wstring d = W(g_mapsDir);
        if (ok(d)) return d;
        why = "the plug-in maps folder set in Settings has no bundles\\index.json and store\\modules";
        return {};
    }
    std::wstring d = KnownFolder(FOLDERID_LocalAppData) + L"\\Avium\\ApolloPlugins";
    if (ok(d)) return d;
    std::wstring up = ExeDir();
    for (int i = 0; i < 7 && !up.empty(); i++) {
        if (ok(up + L"\\plugins")) return up + L"\\plugins";
        size_t s = up.find_last_of(L'\\');
        if (s == std::wstring::npos) break;
        up = up.substr(0, s);
    }
    why = "no plug-in maps on this PC (apollo-win\\plugins: the mapper writes them; set the folder in Settings)";
    return {};
}

static std::string UnitIdOf(const APOLLO_INFO &i, DWORD bytes)
{
    if (ApolloInfoHasUnit(&i, bytes) && i.UnitKey[0] != 0 && i.UnitKey[sizeof(i.UnitKey) - 1] == 0) return i.UnitKey;
    std::string sn = SerialText(i);
    return sn.empty() ? std::string(ApolloInfoIsTwinX(&i, bytes) ? "twinx" : "x8p") : "SN" + sn;
}

/* ------------------------------------------------------------------ device calls (the unit's own slot handle) */

static bool Ioctl(DWORD code, void *in, DWORD inLen, void *out, DWORD outLen, DWORD *ret, DWORD *err)
{
    return SlotIoctl(g_slots[g_u->slot], code, in, inLen, out, outLen, ret, err);
}

/* One PLUGIN_INSTANCE call; false when the IOCTL itself failed (*err: the Win32 code). */
static bool Instance(APOLLO_PLUGIN_INSTANCE &q, const std::vector<plughost::Sha> *res, const std::vector<plughost::Row> *rows,
                     const ULONG *cmd, ULONG ncmd, ULONG *values, DWORD *err)
{
    size_t inLen = sizeof(q) + (size_t)q.NResources * 32 + (size_t)q.NPrivate * sizeof(APOLLO_PLUGIN_ROW) + (size_t)q.NCommands * 4;
    size_t outLen = sizeof(q) + (size_t)q.ReadWords * 4;
    std::vector<UCHAR> buf(std::max(inLen, outLen));
    q.Size = sizeof(q);
    memcpy(buf.data(), &q, sizeof(q));
    UCHAR *p = buf.data() + sizeof(q);
    for (ULONG i = 0; res && i < q.NResources; i++, p += 32) memcpy(p, (*res)[i].data(), 32);
    for (ULONG i = 0; rows && i < q.NPrivate; i++, p += sizeof(APOLLO_PLUGIN_ROW)) {
        APOLLO_PLUGIN_ROW r{ (*rows)[i].pool, (*rows)[i].dwords, (*rows)[i].link, (*rows)[i].flags };
        memcpy(p, &r, sizeof(r));
    }
    if (q.NCommands && cmd) memcpy(p, cmd, (size_t)std::min(q.NCommands, ncmd) * 4);
    DWORD ret = 0, e = 0;
    if (!Ioctl(IOCTL_APOLLO_PLUGIN_INSTANCE, buf.data(), (DWORD)inLen, buf.data(), (DWORD)outLen, &ret, &e) || ret < sizeof(q)) {
        if (err) *err = e ? e : ERROR_INVALID_DATA;
        return false;
    }
    memcpy(&q, buf.data(), sizeof(q));
    if (values && q.ReadWords) memcpy(values, buf.data() + sizeof(q), (size_t)q.ReadWords * 4);
    if (err) *err = 0;
    return true;
}

static void Destroy(ULONG handle)
{
    if (handle == 0) return;
    APOLLO_PLUGIN_INSTANCE q{};
    q.Op = APOLLO_PLUGIN_INST_DESTROY;
    q.Handle = handle;
    DWORD e = 0;
    (void)Instance(q, nullptr, nullptr, nullptr, 0, nullptr, &e);
}

/*
 * [Lat] A routed chain's record path for the reported latencies (op LATENCY, FEAT_LATENCY): the plug-in latency through
 * its record tap and the record delay its CREATE wrote. Only what goes over the plain strip's 200 samples changes the
 * unit's ASIO / Windows input latency (docs/ASIO.md, Latency); an older driver without the op is simply not told.
 */
static void ReportLatency(ULONG handle, ULONG epoch, ULONG recordLatency, ULONG recordDelay)
{
    if (handle == 0 || !(g_caps.Features & APOLLO_PLUGIN_FEAT_LATENCY)) return;
    APOLLO_PLUGIN_INSTANCE q{};
    q.Op = APOLLO_PLUGIN_INST_LATENCY;
    q.Handle = handle;
    q.Epoch = epoch;
    q.Scratch[0] = std::min<ULONG>(recordLatency, APOLLO_PLUGIN_LATENCY_MAX);
    q.Scratch[1] = std::min<ULONG>(recordDelay, APOLLO_PLUGIN_RECORD_DELAY_MAX);
    DWORD e = 0;
    (void)Instance(q, nullptr, nullptr, nullptr, 0, nullptr, &e);
}

static bool ServiceRead(ULONG handle, ULONG epoch, ULONG row, ULONG word, ULONG n, ULONG *v, LONG &status)
{
    APOLLO_PLUGIN_INSTANCE q{};
    q.Op = APOLLO_PLUGIN_INST_SERVICE;
    q.Handle = handle;
    q.Epoch = epoch;
    q.ReadRef = (row << 24) | word;
    q.ReadWords = n;
    DWORD e = 0;
    bool ok = Instance(q, nullptr, nullptr, nullptr, 0, v, &e);
    status = ok ? q.Status : (LONG)0xC0000001;
    return ok && q.Status == 0;
}

static bool ServiceWrite(ULONG handle, ULONG epoch, const ULONG *cmd, ULONG n, LONG &status, DWORD &err)
{
    APOLLO_PLUGIN_INSTANCE q{};
    q.Op = APOLLO_PLUGIN_INST_SERVICE;
    q.Handle = handle;
    q.Epoch = epoch;
    q.NCommands = n;
    bool ok = Instance(q, nullptr, nullptr, cmd, n, nullptr, &err);
    status = ok ? q.Status : (LONG)0xC0000001;
    return ok && q.Status == 0;
}

/* [Ins] A Unison op (BEGIN, ACTIVATE, PARAMS with its triples), apollo_plugin.h. */
static bool UnisonOp(ULONG handle, ULONG epoch, ULONG op, const std::vector<ULONG> &words, LONG &status)
{
    APOLLO_PLUGIN_INSTANCE q{};
    q.Op = op;
    q.Handle = handle;
    q.Epoch = epoch;
    q.NCommands = (ULONG)words.size();
    DWORD e = 0;
    bool ok = Instance(q, nullptr, nullptr, words.empty() ? nullptr : words.data(), (ULONG)words.size(), nullptr, &e);
    status = ok ? q.Status : (LONG)0xC0000001;
    return ok && q.Status == 0;
}

/* The readback of observation runs: words that differ (owned skipped, moving ones learned into owned). */
static bool Readback(ULONG handle, ULONG epoch, std::set<RW> &owned, const std::vector<plughost::Obs> &obs, ULONG &bad, std::string &why)
{
    bad = 0;
    size_t i = 0;
    while (i < obs.size()) {
        size_t j = plughost::ReadGroupEnd(obs, i);
        ULONG span = plughost::ReadGroupSpan(obs, i, j), v[PLUG_READ_WORDS_MAX], v2[PLUG_READ_WORDS_MAX];
        LONG st = 0;
        if (!ServiceRead(handle, epoch, obs[i].row, obs[i].word, span, v, st)) {
            char b[128];
            snprintf(b, sizeof(b), "a readback was refused (row %lu word %lu: 0x%08lx)", obs[i].row, obs[i].word, (ULONG)st);
            why = b;
            return false;
        }
        std::vector<RW> diffs = plughost::GroupDiffs(obs, i, j, v, nullptr, owned, nullptr);
        if (!diffs.empty()) {
            std::set<RW> moving;
            if (ServiceRead(handle, epoch, obs[i].row, obs[i].word, span, v2, st)) {
                diffs = plughost::GroupDiffs(obs, i, j, v, v2, owned, &moving);
                owned.insert(moving.begin(), moving.end());
            }
            bad += (ULONG)diffs.size();
            if (!diffs.empty() && why.empty()) {
                why = "row " + std::to_string(diffs[0].first) + " word " + std::to_string(diffs[0].second) + " is not as written";
            }
        }
        i = j;
    }
    return true;
}

/* Row 0 (the chain head) as page 0 wrote it (apollo-plug HeadCheck). */
static bool HeadCheck(ULONG handle, ULONG epoch, const plughost::ChainRecipe &r)
{
    ULONG n = std::min<ULONG>(r.rows[0].dwords, PLUG_READ_WORDS_MAX), want[PLUG_READ_WORDS_MAX]{}, v[PLUG_READ_WORDS_MAX];
    bool have[PLUG_READ_WORDS_MAX]{};
    ULONG page0 = r.pages[0].second;
    for (ULONG i = 0, len = 0; i + 3 < page0; i += len) {
        len = r.cmd[i] & 0xFFFF;
        if (len < 4) return false;
        if ((r.cmd[i + 1] >> 24) != 0) continue;
        ULONG w0 = r.cmd[i + 1] & 0xFFFFFF;
        for (ULONG k = 0, pos = w0; k < len - 3; k++, pos++) {
            pos = PlugWriteSkip(w0, r.cmd[i + 2], pos);
            if (pos < n) { want[pos] = r.cmd[i + 3 + k]; have[pos] = true; }
        }
    }
    LONG st = 0;
    if (!ServiceRead(handle, epoch, 0, 0, n, v, st)) return false;
    for (ULONG p = 0; p < n; p++) {
        if (have[p] && v[p] != want[p]) return false;
    }
    return true;
}

/* ------------------------------------------------------------------ what the driver says */

static bool CapsOk() { return g_present && g_capsOk; }
static bool HostingOn() { return CapsOk() && (g_caps.Enabled & APOLLO_PLUGIN_ENABLED_HOST); }
static bool CallerOk()
{
    if (!HostingOn()) return false;
    return (g_caps.Features & APOLLO_PLUGIN_FEAT_CALLER) ? (g_caps.Enabled & APOLLO_PLUGIN_ENABLED_CALLER) != 0 : true;
}
static ULONG StripDsp() { return (CapsOk() && g_caps.StripDsp < APOLLO_PLUGIN_DSPS) ? g_caps.StripDsp : 1; }
static const APOLLO_PLUGIN_DSP_INFO &DspInfo() { return g_caps.Dsp[StripDsp()]; }
static bool Warm() { return g_present && (g_info.Flags & APOLLO_FLAG_WARM_START); }
static bool StripsLive() { return CapsOk() && g_caps.StripsState == APOLLO_STRIPS_STATE_LIVE; }
static bool Quarantined() { return CapsOk() && (g_caps.QuarantinedMask & (1u << StripDsp())); }

/*
 * [Warm] The strip DSP is not this host's: the driver's reason (caps Dsp[].Flags 15:8, FEAT_WARM) in the UI's words, with
 * the one action that ends it. Code "warm" (waiting: a power cycle of the unit brings the saved inserts back), or "strips"
 * while the driver's warm-start check has not run yet (it ends by itself within a second).
 */
static std::string OwnBlock(std::string &why)
{
    const std::string unit = g_u->model && g_u->model->Model == PLUG_MODEL_TWINX ? "Twin X" : "x8p";
    const std::string pc = "power-cycle the " + unit + " (off, 10 s, on): its saved inserts come back after it";
    ULONG w = (g_caps.Features & APOLLO_PLUGIN_FEAT_WARM) ? APOLLO_PLUGIN_DSP_WHY(DspInfo().Flags) : APOLLO_PLUGIN_WHY_NONE;
    switch (w) {
    case APOLLO_PLUGIN_WHY_WARM_PENDING:
    case APOLLO_PLUGIN_WHY_NOT_READ:
        why = "the " + Nm() + " is starting: the driver checks its input strips first";
        return "strips";
    case APOLLO_PLUGIN_WHY_HOST_USED:
        why = "another driver (UA's) used DSP 1 since the " + unit + " was switched on: " + pc;
        return "warm";
    case APOLLO_PLUGIN_WHY_TAINTED:
        why = "a raw register write reached the " + unit + " since it started: " + pc;
        return "warm";
    case APOLLO_PLUGIN_WHY_WARM_NOT_ADOPTED:
        why = "this warm start (PC restart or driver update with the " + unit + " on) could not take over the input strips "
              "the driver left running: " + pc;
        return "warm";
    case APOLLO_PLUGIN_WHY_WARM_PLUGIN_LEFT:
        why = "a plug-in was still on DSP 1 when the driver last stopped, so its room is unknown: " + pc;
        return "warm";
    case APOLLO_PLUGIN_WHY_WARM_OFF:
        why = "plug-ins after a warm start are off for this unit (StripWarmPlugins = 0): " + pc;
        return "warm";
    case APOLLO_PLUGIN_WHY_WARM_UNANSWERED:
        why = "DSP 1 did not answer the first plug-in step after this warm start: " + pc;
        return "warm";
    case APOLLO_PLUGIN_WHY_STRIPS:
        why = "the " + unit + "'s input strips are not the driver's (or were quarantined): " + pc;
        return "warm";
    default:
        why = "needs " + std::string(unit == "Twin X" ? "a Twin X" : "an x8p") + " power cycle: after a warm start (driver "
              "update, PC restart with the " + Nm() + " on) no DSP is ours to place on";
        return "warm";
    }
}

/* Why an insert cannot go in now: code and words for the UI ('' = it can). */
static std::string Blocked(const Chain &ch, std::string &why)
{
    if (!g_present) { why = "the " + Nm() + " is not connected"; return "absent"; }
    if (!g_capsOk) {
        why = g_capsErr == ERROR_INVALID_FUNCTION ? "the installed apollo.sys has no plug-in hosting (P0 or later is needed)"
                                                  : "the driver did not answer the plug-in host query";
        return "driver";
    }
    if (g_u->model && g_u->model->Model != PLUG_MODEL_X8P && !(g_caps.Features & APOLLO_PLUGIN_FEAT_UNITS)) {
        why = "the installed apollo.sys hosts plug-ins on the x8p only: install the build with Twin X hosting";
        return "unit";                      /* [P7] waits: a driver update (and a power cycle) brings it */
    }
    if (g_infoBytes >= offsetof(APOLLO_INFO, SampleRate) + sizeof(ULONG) && g_info.SampleRate != 0 && g_info.SampleRate != 48000) {
        /* [Rate] the recipes were mapped at 48 kHz: they wait for 48 kHz (the driver refuses them too) */
        char b[96];
        snprintf(b, sizeof(b), "plug-ins run at 48 kHz only: the %s runs at %.1f kHz", std::string(Nm()).c_str(), g_info.SampleRate / 1000.0);
        why = b;
        return "rate";
    }
    if (!HostingOn()) { why = "plug-in hosting is off (Settings, Plug-in hosting)"; return "hosting"; }
    if (!CallerOk()) { why = "the driver takes plug-in calls from another program: turn hosting on again from this Apollo Control"; return "caller"; }
    if (Quarantined()) {
        if ((g_caps.Features & APOLLO_PLUGIN_FEAT_WARM) && APOLLO_PLUGIN_DSP_WHY(DspInfo().Flags) == APOLLO_PLUGIN_WHY_WARM_UNANSWERED) {
            (void)OwnBlock(why);            /* [Warm] the first upload after a warm start went unanswered */
            return "quarantine";
        }
        why = "DSP 1 is quarantined after an unproven plug-in step: power-cycle the " + Nm();
        return "quarantine";
    }
    if (StripDsp() == 0) { why = "the " + Nm() + "'s strips run on DSP 0 (StripDsp 0): inserts need them on DSP 1"; return "strips"; }
    /*
     * [Warm] The driver decides whether the strip DSP is ours (OwnedMask): after a cold start, and (FEAT_WARM) after a warm
     * start that adopted or rebuilt this driver's strips. A driver without FEAT_WARM owns nothing at a warm start.
     */
    if (!(g_caps.OwnedMask & (1u << StripDsp())) || (Warm() && !(g_caps.Features & APOLLO_PLUGIN_FEAT_WARM))) {
        return OwnBlock(why);
    }
    if (!StripsLive()) { why = "the " + Nm() + "'s input strips are not live yet"; return "strips"; }
    if (ch.aux >= 0) {
        /* [Ins] aux inserts: experimental, a driver that has them and the registry switch */
        if (!(g_caps.Features & APOLLO_PLUGIN_FEAT_AUX)) { why = "the installed apollo.sys has no aux inserts"; return "driver"; }
        if (!(g_caps.Enabled & APOLLO_PLUGIN_ENABLED_AUX)) {
            why = "aux inserts are off: they are experimental (Services\\apollo\\Parameters\\AuxInserts = 1 turns them on)";
            return "aux";
        }
    } else {
        ULONG mask = ch.stereo ? (3u << ch.input) : (1u << ch.input);
        if ((g_caps.StripsStarted & mask) != mask) { why = "the plain strip of this input is not running"; return "strips"; }
    }
    if (!(DspInfo().Flags & APOLLO_PLUGIN_DSP_ADMITTED)) { why = "DSP 1 does not take plug-ins right now (its strip pass is running)"; return "busy"; }
    if (ch.stereo && ch.aux < 0 && !(g_caps.Features & APOLLO_PLUGIN_FEAT_STEREO)) { why = "the installed apollo.sys starts plug-ins mono only (no stereo pairs)"; return "stereo"; }
    if (ch.hasUnison && !ch.unison.bypass && !(g_caps.Features & APOLLO_PLUGIN_FEAT_UNISON)) { why = "the installed apollo.sys has no Unison lease"; return "driver"; }
    return {};
}

/* An NTSTATUS from a CREATE / START / SERVICE, in the UI's words. */
static std::string Refusal(const APOLLO_PLUGIN_INSTANCE &q, std::string &why)
{
    char b[160];
    switch ((ULONG)q.Status) {
    case 0xC0000022u:
        if (q.Step == APOLLO_PLUGIN_STEP_LICENCE || q.Step == APOLLO_PLUGIN_STEP_VALIDATE) {
            static const char *st[] = { "authorized", "a running demo", "a demo not started", "expired", "unavailable" };
            snprintf(b, sizeof(b), "not licensed on this %s (%s): new licences come through UA's own software", Nm().c_str(),
                     q.LicenseState < 5 ? st[q.LicenseState] : "no answer");
            why = b;
            return "licence";
        }
        break;
    case 0xC000009Au: why = "no room on DSP 1 for this plug-in next to the strips and the other inserts"; return "room";
    case 0xC0000034u: why = "its modules are not in the driver's module store: install them (needs administrator)"; return "modules";
    case 0xC0000428u: case 0xC000007Bu: why = "a module in the driver's store is damaged: install the modules again"; return "modules";
    case 0x80000011u: why = "DSP 1 is busy (a strip pass or another operation): try again"; return "busy";
    case 0xC00000B5u: case 0xC000038Eu: case 0xC00000D0u:
        why = "the DSP did not prove a step: DSP 1 is quarantined until the " + Nm() + " is power-cycled";
        return "quarantine";
    case 0xC0000120u: why = "the " + Nm() + " restarted under the operation"; return "warm";
    case 0xC000022Du: why = "the mixer mailbox was not settled: try again"; return "busy";      /* [Ins] STATUS_RETRY (Unison BEGIN) */
    case 0xC0000184u: case 0xC0000225u: why = "the input's plain strip is not live (power-cycle the " + Nm() + " for the strips)"; return "strips";
    default: break;
    }
    snprintf(b, sizeof(b), "apollo.sys refused it (status 0x%08lX at step %lu)", (ULONG)q.Status, q.Step);
    why = b;
    return "driver";
}

/* ------------------------------------------------------------------ persistence */

static std::vector<plughost::SavedChain> SavedList()
{
    std::vector<plughost::SavedChain> list;
    for (const Chain &ch : g_chains) {
        plughost::SavedChain c;
        c.unit = ch.unit;
        c.input = ch.input;
        c.aux = ch.aux;
        c.stereo = ch.stereo;
        c.mon = ch.mon;
        for (const ChainSlot &x : ch.slots) c.slots.push_back(x);
        c.hasUnison = ch.hasUnison;
        if (ch.hasUnison) c.unison = ch.unison;
        if (!c.slots.empty() || c.hasUnison || c.mon) list.push_back(std::move(c));
    }
    return list;
}

static void Save()
{
    (void)WriteAtomic(SavePath(), plughost::ChainsToJson(SavedList(), g_mapsDir));
}

/* A saved chain as the engine keeps it (nothing running yet: Restore() builds it once the unit's strips are Live). */
static Chain FromSaved(plughost::SavedChain &sc)
{
    Chain ch;
    ch.unit = sc.unit;
    ch.input = sc.input;
    ch.aux = sc.aux;
    ch.stereo = sc.stereo;
    ch.mon = sc.mon;
    for (plughost::SavedSlot &x : sc.slots) {
        ChainSlot sl;
        static_cast<plughost::SavedSlot &>(sl) = std::move(x);
        sl.state = sl.bypass ? "bypassed" : "waiting";
        ch.slots.push_back(std::move(sl));
    }
    ch.hasUnison = sc.hasUnison;
    if (sc.hasUnison) {
        static_cast<plughost::SavedSlot &>(ch.unison) = std::move(sc.unison);
        ch.unison.slot = kUnisonSlot;
    }
    ch.state = ch.active() ? "waiting" : "idle";
    return ch;
}

/* [Ins] version 2 (chains); a version 1 file (one insert per input) reads as chains with slot 0 */
static void Load()
{
    std::string t;
    std::vector<plughost::SavedChain> list;
    g_chains.clear();
    if (!ReadAll(SavePath(), t, 16ull << 20) || !plughost::ChainsFromJson(t, list, g_mapsDir)) return;
    for (plughost::SavedChain &sc : list) g_chains.push_back(FromSaved(sc));
}

/* ------------------------------------------------------------------ UAD catalogue facts: category, tags, art
 *
 * The Plug-ins list shows UA's own category, tags and icon for each plug-in, read from the user's UAD install
 * (Powered Plugins\Manifest\<name>\manifest.json, UA.xml, Plug-In Icons\*.png): plain files, read in place,
 * offline, never copied, cached or logged. No UA program or DLL is started or loaded. The page shows the icons
 * through the uad-art.apollo virtual host (main.cpp maps it to Plug-In Icons with DENY_CORS: <img> only, no
 * fetch reads). Older hosts send no such fields; the page treats them as empty. */

static const wchar_t UAD_ART_HOST[] = L"uad-art.apollo";
static const char    UAD_ART_ORIGIN[] = "https://uad-art.apollo/";

static bool IsDir(const std::wstring &p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool IsFile(const std::wstring &p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* The UAD install (Program Files (x86), else Program Files; it must have a Manifest folder), '' = none.
   Any thread: main.cpp maps the icons with it, the plug-in thread reads the manifests. */
static std::wstring UadInstallDir()
{
    const KNOWNFOLDERID *ids[] = { &FOLDERID_ProgramFilesX86, &FOLDERID_ProgramFiles };
    for (const KNOWNFOLDERID *id : ids) {
        std::wstring base = KnownFolder(*id);
        if (base.empty()) continue;
        std::wstring d = base + L"\\Universal Audio\\Powered Plugins";
        if (IsDir(d + L"\\Manifest")) return d;
    }
    return {};
}

/* Its Plug-In Icons folder, '' = none (main.cpp: the uad-art.apollo mapping). */
static std::wstring UadArtDir()
{
    std::wstring d = UadInstallDir();
    if (d.empty() || !IsDir(d + L"\\Plug-In Icons")) return {};
    return d + L"\\Plug-In Icons";
}

/* plug-in thread only */
static bool                                g_uadLooked, g_uadScanned, g_uadXmlRead;
static std::wstring                        g_uadDir;
static std::map<std::string, std::string>  g_uadJson;      /* plug-in name -> its ,"category",... fields */
static std::map<std::string, std::wstring> g_uadByName;    /* lower-case manifest "name" -> manifest.json */
static std::map<std::string, std::string>  g_uadXml;       /* lower-case UA.xml plugin title -> its category */

/* Read again when the catalogue is rebuilt (a UAD update may have changed them). */
static void UadReset()
{
    g_uadLooked = g_uadScanned = g_uadXmlRead = false;
    g_uadDir.clear();
    g_uadJson.clear();
    g_uadByName.clear();
    g_uadXml.clear();
}

/* One path component, nothing that leaves its folder or that Windows treats specially. */
static bool UadLeaf(const std::string &s)
{
    if (s.empty() || s == "." || s == "..") return false;
    for (unsigned char c : s) {
        if (c < 0x20 || strchr("\\/:*?\"<>|", (char)c)) return false;
    }
    return true;
}

static bool UadManifest(const std::wstring &path, aggjson::Value &m)
{
    std::string t;
    if (!ReadAll(path, t, 1u << 20)) return false;
    if (t.size() >= 3 && (unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB && (unsigned char)t[2] == 0xBF) t.erase(0, 3);
    aggjson::Parser p(t);
    return p.Parse(m) && m.kind == aggjson::Value::Object;
}

/* Every manifest by its "name" (once): for a plug-in whose folder is named differently. */
static void UadScan()
{
    g_uadScanned = true;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((g_uadDir + L"\\Manifest\\*").c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    int n = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        if (++n > 4000) break;
        std::wstring path = g_uadDir + L"\\Manifest\\" + fd.cFileName + L"\\manifest.json";
        aggjson::Value m;
        if (!UadManifest(path, m)) continue;
        std::string name = plughost::Lower(m.Str("name"));
        if (!name.empty()) g_uadByName.emplace(name, path);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static std::string XmlText(const std::string &s)
{
    static const char *const ent[][2] = { { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" }, { "&apos;", "'" } };
    std::string o;
    for (size_t i = 0; i < s.size();) {
        bool hit = false;
        if (s[i] == '&') {
            for (const auto &e : ent) {
                size_t n = strlen(e[0]);
                if (s.compare(i, n, e[0]) == 0) { o += e[1]; i += n; hit = true; break; }
            }
        }
        if (!hit) o += s[i++];
    }
    return o;
}

/* name="value" (or 'value') inside the tag t[from, to), decoded. */
static bool XmlAttr(const std::string &t, size_t from, size_t to, const char *name, std::string &out)
{
    std::string key = std::string(name) + "=";
    for (size_t k = from; (k = t.find(key, k)) != std::string::npos && k < to; k++) {
        char pre = k ? t[k - 1] : '<';
        if (pre != ' ' && pre != '\t' && pre != '\r' && pre != '\n') continue;
        size_t q = k + key.size();
        if (q >= to || (t[q] != '"' && t[q] != '\'')) return false;
        size_t e = t.find(t[q], q + 1);
        if (e == std::string::npos || e > to) return false;
        out = XmlText(t.substr(q + 1, e - q - 1));
        return true;
    }
    return false;
}

/* A tag name ends at a space, '/' or '>' ("<category" is not "<categories"). */
static bool XmlTagEnds(const std::string &t, size_t at)
{
    char c = at < t.size() ? t[at] : '>';
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '/' || c == '>';
}

/* UA.xml <categories>: each plug-in's first category (not Console), for plug-ins without primary_category. */
static void UadXml()
{
    g_uadXmlRead = true;
    std::string t;
    if (!ReadAll(g_uadDir + L"\\UA.xml", t, 8u << 20)) return;
    const size_t npos = std::string::npos;
    size_t a = t.find("<categories"), b = a == npos ? npos : t.find("</categories>", a);
    if (b == npos) return;
    for (size_t p = a;;) {
        size_t c = t.find("<category", p);
        if (c == npos || c >= b) break;
        size_t ce = t.find('>', c);
        if (ce == npos || ce > b) break;
        p = ce;
        if (!XmlTagEnds(t, c + 9) || t[ce - 1] == '/') continue;  /* <categories>, or an empty <category/> */
        std::string title;
        XmlAttr(t, c, ce, "title", title);
        size_t end = t.find("</category>", ce);
        if (end == npos || end > b) end = b;
        p = end;
        if (title.empty() || plughost::Lower(title) == "console") continue;
        for (size_t q = ce;;) {
            size_t g = t.find("<plugin", q);
            if (g == npos || g >= end) break;
            size_t ge = t.find('>', g);
            if (ge == npos || ge > end) break;
            q = ge;
            std::string pt;
            if (XmlTagEnds(t, g + 7) && XmlAttr(t, g, ge, "title", pt) && !pt.empty()) g_uadXml.emplace(plughost::Lower(pt), title);
        }
    }
}

/* A URL path segment: A-Z a-z 0-9 - . _ ~ as they are, every other UTF-8 byte as %XX. */
static std::string PctEnc(const std::string &s)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') {
            o += (char)c;
        } else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

/* ,"category":"...","tags":[...],"art":"..." for one plug-in (PostList), read once and kept. */
static const std::string &UadJson(const std::string &name)
{
    auto it = g_uadJson.find(name);
    if (it != g_uadJson.end()) return it->second;
    if (!g_uadLooked) {
        g_uadLooked = true;
        g_uadDir = UadInstallDir();
    }
    std::string cat, tags, art;
    if (!g_uadDir.empty()) {
        aggjson::Value m;
        bool have = UadLeaf(name) && UadManifest(g_uadDir + L"\\Manifest\\" + W(name) + L"\\manifest.json", m);
        if (!have) {
            if (!g_uadScanned) UadScan();
            auto f = g_uadByName.find(plughost::Lower(name));
            have = f != g_uadByName.end() && UadManifest(f->second, m);
        }
        std::string thumb;
        if (have) {
            cat = m.Str("primary_category");
            if (const aggjson::Value *c = m.Get("categories")) {
                for (const aggjson::Value &x : c->a) {
                    if (x.kind == aggjson::Value::String && !x.s.empty()) tags += (tags.empty() ? "" : ",") + Q(x.s);
                }
            }
            thumb = m.Str("thumbnail");
        }
        if (cat.empty()) {
            if (!g_uadXmlRead) UadXml();
            auto x = g_uadXml.find(plughost::Lower(name));
            if (x != g_uadXml.end()) cat = x->second;
        }
        /* the manifest's thumbnail, else <name>.png (a few manifests name none, or a file that is not there) */
        std::wstring icons = g_uadDir + L"\\Plug-In Icons\\";
        for (const std::string &f : { thumb, name + ".png" }) {
            if (UadLeaf(f) && IsFile(icons + W(f))) {
                art = UAD_ART_ORIGIN + PctEnc(f);
                break;
            }
        }
    }
    std::string j = ",\"category\":" + Q(cat) + ",\"tags\":[" + tags + "],\"art\":" + Q(art);
    return g_uadJson.emplace(name, std::move(j)).first->second;
}

/* ------------------------------------------------------------------ the maps: index and catalogue */

static bool LoadIndex()
{
    g_index.clear();
    g_mapsWhy.clear();
    g_plugins = FindPlugins(g_mapsWhy);
    if (g_plugins.empty()) return false;
    std::string t, err;
    if (!ReadAll(g_plugins + L"\\bundles\\index.json", t) || !plughost::ParseIndex(t, g_index, err)) {
        g_mapsWhy = err.empty() ? "bundles\\index.json could not be read" : err;
        return false;
    }
    return true;
}

static const plughost::IndexEntry *IndexFor(const std::string &name, const char *layout)
{
    for (const auto &e : g_index) {
        if (e.name == name && e.layout == layout) return &e;
    }
    return nullptr;
}

static void CacheLoad()
{
    std::string t;
    g_cat.clear();
    if (!ReadAll(CachePath(), t)) return;
    aggjson::Value root;
    aggjson::Parser p(t);
    if (!p.Parse(root) || root.Num("version") != 2) return;
    if (const aggjson::Value *a = root.Get("entries")) {
        for (const aggjson::Value &e : a->a) {
            CatEntry c;
            c.file = e.Str("file");
            c.layout = e.Str("layout");
            c.size = (ULONGLONG)e.Num("size");
            c.mtime = (ULONGLONG)e.Num("mtime");
            c.item.name = e.Str("name");
            c.item.renderId = (ULONG)e.Num("renderId");
            c.item.latency = (ULONG)e.Num("latency");
            c.item.codeWords = (ULONG)e.Num("code");
            c.item.dataWords = (ULONG)e.Num("data");
            c.item.startupWords = (ULONG)e.Num("startup");
            c.item.pages = (ULONG)e.Num("pages");
            c.item.params = (ULONG)e.Num("params");
            const aggjson::Value *ok = e.Get("ok");
            c.item.ok = ok && ok->kind == aggjson::Value::Bool && ok->b;
            c.item.why = e.Str("why");
            if (const aggjson::Value *m = e.Get("modules")) for (const aggjson::Value &x : m->a) c.modules.push_back(x.s);
            if (!c.file.empty()) g_cat[c.file] = std::move(c);
        }
    }
}

static void CacheSave()
{
    std::string j = "{\"version\":2,\"entries\":[";
    bool first = true;
    char b[64];
    for (const auto &kv : g_cat) {
        const CatEntry &c = kv.second;
        j += first ? "{" : ",{";
        first = false;
        snprintf(b, sizeof(b), "%llu", c.size);
        j += "\"file\":" + Q(c.file) + ",\"layout\":" + Q(c.layout) + ",\"size\":" + b;
        snprintf(b, sizeof(b), "%llu", c.mtime);
        j += std::string(",\"mtime\":") + b + ",\"name\":" + Q(c.item.name) + ",\"renderId\":" + std::to_string(c.item.renderId) +
             ",\"latency\":" + std::to_string(c.item.latency) + ",\"code\":" + std::to_string(c.item.codeWords) +
             ",\"data\":" + std::to_string(c.item.dataWords) + ",\"startup\":" + std::to_string(c.item.startupWords) +
             ",\"pages\":" + std::to_string(c.item.pages) + ",\"params\":" + std::to_string(c.item.params) +
             ",\"ok\":" + (c.item.ok ? "true" : "false") + ",\"why\":" + Q(c.item.why) + ",\"modules\":[";
        for (size_t k = 0; k < c.modules.size(); k++) j += (k ? "," : "") + Q(c.modules[k]);
        j += "]}";
    }
    j += "]}\n";
    (void)WriteAtomic(CachePath(), j);
}

/* A slice of the catalogue (about 150 ms): bundles not in the cache or changed are parsed. */
static void CatalogStep()
{
    if (!g_catBuilding) return;
    ULONGLONG t0 = GetTickCount64();
    while (g_catNext < g_index.size() && GetTickCount64() - t0 < 150) {
        const plughost::IndexEntry &e = g_index[g_catNext++];
        std::wstring path = g_plugins + L"\\bundles\\" + W(e.file);
        ULONGLONG size = 0, mtime = 0;
        if (!FileStamp(path, size, mtime)) continue;
        auto it = g_cat.find(e.file);
        if (it != g_cat.end() && it->second.size == size && it->second.mtime == mtime) continue;
        CatEntry c;
        c.file = e.file;
        c.layout = e.layout;
        c.size = size;
        c.mtime = mtime;
        std::string t, err;
        plughost::Bundle b;
        if (ReadAll(path, t) && plughost::ParseBundle(t, e.file, b, err)) {
            plughost::CatalogFrom(b, c.item);
            for (const auto &r : b.resources) {
                if (r.arch == 0 || r.arch == 2) c.modules.push_back(r.sha);
            }
        } else {
            c.item.name = e.name;
            c.item.ok = false;
            c.item.why = err.empty() ? "unreadable" : err;
        }
        g_cat[e.file] = std::move(c);
        g_catDirty = true;
    }
    if (g_catNext >= g_index.size()) {
        g_catBuilding = false;
        if (g_catDirty) CacheSave();
        g_catDirty = false;
    }
}

static void CatalogStart()
{
    if (g_catBuilding) return;
    UadReset();                             /* UA's categories and icons: read again with the catalogue */
    if (!LoadIndex()) return;
    if (g_cat.empty()) CacheLoad();
    g_catNext = 0;
    g_catBuilding = true;
}

/* ------------------------------------------------------------------ licence (BOARD_QUERY) */

static void Licence(bool force)
{
    if (!CallerOk()) {
        g_lic.clear();
        g_licState = "unknown";
        g_licWhy = HostingOn() ? "this program is not the plug-in host" : "plug-in hosting is off";
        return;
    }
    if (!force && g_licState == "ok" && g_licEpoch == g_caps.Epoch && GetTickCount64() - g_licAt < 60000) return;
    auto q = std::make_unique<APOLLO_BOARD_QUERY>();
    memset(q.get(), 0, sizeof(*q));
    q->Size = sizeof(*q);
    DWORD ret = 0, err = 0;
    g_licAt = GetTickCount64();
    if (!Ioctl(IOCTL_APOLLO_BOARD_QUERY, q.get(), sizeof(*q), q.get(), sizeof(*q), &ret, &err) || ret < sizeof(*q) || q->Status != 0) {
        g_lic.clear();
        g_licState = "failed";
        g_licWhy = (ret >= sizeof(*q) && q->WordsWritten == 0) ? "DSP 0 did not answer the licence query (a warm start: power-cycle the " + Nm() + ")"
                                                              : std::string("the licence query failed");
        SecureZeroMemory(q.get(), sizeof(*q));
        return;
    }
    g_lic.assign(q->Words, q->Words + APOLLO_BOARD_QUERY_WORDS);
    SecureZeroMemory(q.get(), sizeof(*q));
    g_licState = "ok";
    g_licWhy.clear();
    g_licEpoch = g_caps.Epoch;
}

/* 1 authorized (or a running demo), 0 not, -1 unknown */
static int Licensed(ULONG renderId)
{
    if (g_licState != "ok" || renderId < APOLLO_BOARD_QUERY_BASE || renderId >= APOLLO_BOARD_QUERY_BASE + APOLLO_BOARD_QUERY_WORDS) {
        return g_licState == "ok" ? 0 : -1;
    }
    return PlugLicenseAdmits(PlugLicenseState(g_lic[renderId - APOLLO_BOARD_QUERY_BASE])) ? 1 : 0;
}

/* ------------------------------------------------------------------ the parameter planner (plughelper.py) */

static void HelperStop()
{
    if (g_helperIn) {
        static const char quit[] = "{\"op\":\"quit\"}\n";
        DWORD put = 0;
        WriteFile(g_helperIn, quit, (DWORD)strlen(quit), &put, nullptr);
        CloseHandle(g_helperIn);
        g_helperIn = nullptr;
    }
    if (g_helperProc) {
        if (WaitForSingleObject(g_helperProc, 2000) != WAIT_OBJECT_0) TerminateProcess(g_helperProc, 1);
        CloseHandle(g_helperProc);
        g_helperProc = nullptr;
    }
    if (g_helperOut) { CloseHandle(g_helperOut); g_helperOut = nullptr; }
    if (g_helperJob) { CloseHandle(g_helperJob); g_helperJob = nullptr; }
    for (Chain &ch : g_chains) {
        for (ChainSlot &x : ch.slots) x.helper = false;
        ch.unison.helper = false;
    }
}

static bool HelperLine(std::string &line, DWORD timeoutMs)
{
    static std::string pending;
    ULONGLONG t0 = GetTickCount64();
    line.clear();
    for (;;) {
        size_t nl = pending.find('\n');
        if (nl != std::string::npos) {
            line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        DWORD avail = 0;
        if (!PeekNamedPipe(g_helperOut, nullptr, 0, nullptr, &avail, nullptr)) { pending.clear(); return false; }
        if (avail) {
            std::string b(std::min<DWORD>(avail, 1 << 20), '\0');
            DWORD got = 0;
            if (!ReadFile(g_helperOut, b.data(), (DWORD)b.size(), &got, nullptr)) { pending.clear(); return false; }
            pending.append(b.data(), got);
            continue;
        }
        if (g_quit || GetTickCount64() - t0 > timeoutMs) { pending.clear(); return false; }
        Sleep(5);
    }
}

static bool HelperStart(std::string &why)
{
    if (g_helperProc && WaitForSingleObject(g_helperProc, 0) == WAIT_TIMEOUT) return true;
    HelperStop();
    if (g_plugins.empty() && !LoadIndex()) { why = g_mapsWhy; return false; }
    std::wstring py = g_plugins + L"\\.venv\\Scripts\\python.exe", script;
    std::wstring up = ExeDir();
    if (Exists(up + L"\\plughelper.py")) script = up + L"\\plughelper.py";
    for (int i = 0; i < 5 && script.empty() && !up.empty(); i++) {
        if (Exists(up + L"\\tools\\plugin-host\\plughelper.py")) script = up + L"\\tools\\plugin-host\\plughelper.py";
        size_t s = up.find_last_of(L'\\');
        if (s == std::wstring::npos) break;
        up = up.substr(0, s);
    }
    if (!Exists(py)) { why = "the plug-in maps' Python (plugins\\.venv) is missing: parameters need it"; return false; }
    if (script.empty()) { why = "plughelper.py is not next to Apollo Control"; return false; }
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
    if (!CreatePipe(&inR, &inW, &sa, 0) || !CreatePipe(&outR, &outW, &sa, 1 << 16)) { why = "no pipe for the planner"; return false; }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    std::wstring logPath = AppDataDir(FOLDERID_LocalAppData) + L"\\plughelper.log";
    HANDLE log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR;
    si.hStdOutput = outW;
    si.hStdError = log != INVALID_HANDLE_VALUE ? log : outW;
    std::wstring cmd = L"\"" + py + L"\" -I -X utf8 \"" + script + L"\" --plugins \"" + g_plugins + L"\"";
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(py.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                             g_plugins.c_str(), &si, &pi);
    CloseHandle(inR);
    CloseHandle(outW);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok) {
        why = "the planner did not start: " + U8(ErrorText(GetLastError()));
        CloseHandle(inW);
        CloseHandle(outR);
        return false;
    }
    /* the planner (and the venv launcher's child) ends with this app */
    g_helperJob = CreateJobObjectW(nullptr, nullptr);
    if (g_helperJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_helperJob, JobObjectExtendedLimitInformation, &li, sizeof(li));
        AssignProcessToJobObject(g_helperJob, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    g_helperProc = pi.hProcess;
    g_helperIn = inW;
    g_helperOut = outR;
    std::string line;
    if (!HelperLine(line, 60000) || line.find("\"ready\"") == std::string::npos) {
        why = "the planner did not come up (see %LOCALAPPDATA%\\Avium\\ApolloControl\\plughelper.log)";
        HelperStop();
        return false;
    }
    g_helperUsed = GetTickCount64();
    return true;
}

/* One request, one reply; false and *why when the planner failed or refused. */
static bool HelperCall(std::string req, aggjson::Value &reply, std::string &why)
{
    if (!HelperStart(why)) return false;
    ULONG id = ++g_helperId;
    req.insert(1, "\"id\":" + std::to_string(id) + ",");
    req += "\n";
    DWORD put = 0;
    if (!WriteFile(g_helperIn, req.data(), (DWORD)req.size(), &put, nullptr) || put != req.size()) {
        why = "the planner stopped";
        HelperStop();
        return false;
    }
    std::string line;
    for (;;) {
        if (!HelperLine(line, HELPER_REPLY_MS)) {
            why = "the planner did not answer";
            HelperStop();
            return false;
        }
        aggjson::Parser p(line);
        reply = aggjson::Value{};
        if (p.Parse(reply) && reply.Num("id", 0) == (double)id) break;
    }
    g_helperUsed = GetTickCount64();
    const aggjson::Value *ok = reply.Get("ok");
    if (!ok || ok->kind != aggjson::Value::Bool || !ok->b) {
        why = "the planner refused: " + reply.Str("error", "no reason given");
        return false;
    }
    return true;
}

static bool Words32(const aggjson::Value *a, Words &out)
{
    out.clear();
    if (!a || a->kind != aggjson::Value::Array) return false;
    for (const aggjson::Value &x : a->a) {
        ULONG v = 0;
        if (!plughost::U32(&x, v)) return false;
        out.push_back(v);
    }
    return true;
}

static void TakeState(ChainSlot &in, const aggjson::Value &r)
{
    Words v;
    if (Words32(r.Get("values"), v) && v.size() == in.params.size()) in.values = v;
    if (const aggjson::Value *t = r.Get("texts")) {
        std::vector<std::string> ts;
        for (const aggjson::Value &x : t->a) ts.push_back(x.s);
        if (ts.size() == in.params.size()) in.texts = ts;
    }
    if (r.Get("diff")) plughost::DiffFromJson(r.Get("diff"), in.diff);
}

static bool HelperOpen(const Chain &ch, ChainSlot &in, ULONG sn, std::string &why)
{
    if (in.helper && g_helperProc) return true;
    std::string req = "{\"op\":\"open\",\"key\":" + Q(ch.slotKey(sn)) + ",\"bundle\":" + Q(in.file) + ",\"values\":[";
    for (size_t k = 0; k < in.values.size(); k++) req += (k ? "," : "") + std::to_string(in.values[k]);
    req += "],\"diff\":" + plughost::DiffToJson(in.diff) + "}";
    aggjson::Value r;
    if (!HelperCall(req, r, why)) return false;
    in.helper = true;
    TakeState(in, r);
    return true;
}

/* ------------------------------------------------------------------ inserts: chains */

static void PostResult(const char *op, const Chain *ch, ULONG input, LONG slot, bool ok, const std::string &code, const std::string &detail)
{
    if (g_u->slot != 0 && !g_multi) return;
    std::string j = std::string("{\"type\":\"plugin-result\",\"unit\":") + std::to_string(g_u->slot) + ",\"op\":\"" + op + "\",\"input\":" +
                    std::to_string(ch ? ch->input : input) + ",\"aux\":" + (ch && ch->aux >= 0 ? std::to_string(ch->aux) : std::string("null")) +
                    ",\"slot\":" + (slot < 0 ? std::string("null") : std::to_string(slot)) + ",\"ok\":" + (ok ? "true" : "false") +
                    ",\"code\":" + Q(code) + ",\"detail\":" + Q(detail) + "}";
    PostJson(W(j));
}

static void PostState(bool force = false);

static ChainSlot *SlotOf(Chain &ch, ULONG s)
{
    if (s == kUnisonSlot) return ch.hasUnison ? &ch.unison : nullptr;
    for (ChainSlot &x : ch.slots) if (x.slot == s) return &x;
    return nullptr;
}

/* The bundle of a slot, checked against its saved SHA-256 (a remapped plug-in loses its saved words). */
static bool LoadSlotBundle(ChainSlot &s, bool stereo, plughost::Bundle &b, std::string &why)
{
    if (g_plugins.empty() && !LoadIndex()) { why = g_mapsWhy; return false; }
    std::string t, err;
    if (!ReadAll(g_plugins + L"\\bundles\\" + W(s.file), t)) { why = "its map (" + s.file + ") is gone: map the plug-in again"; return false; }
    plughost::Sha h{};
    plughost::Sha256(t.data(), t.size(), h);
    std::string sha = plughost::ShaHex(h);
    if (!plughost::ParseBundle(t, s.file, b, err)) { why = "its map cannot be read: " + err; return false; }
    if (b.name != s.name || b.layout != (stereo ? "stereo" : "mono")) { why = "its map is for another plug-in or layout"; return false; }
    if (!s.sha.empty() && s.sha != sha) {
        s.diff.clear();                     /* mapped again: the saved words may not fit the new map */
        s.values.clear();
        s.texts.clear();
    }
    s.sha = sha;
    s.params = b.params;
    s.latency = b.latency;
    if (s.values.size() != b.params.size()) { s.values = b.values; s.texts = b.texts; s.diff.clear(); }
    if (s.texts.size() != b.params.size()) s.texts = b.texts;
    return true;
}

static plughost::ModuleReader StoreReader()
{
    std::wstring store = g_plugins + L"\\store\\modules\\";
    return [store](const std::string &sha, std::vector<UCHAR> &bytes) {
        std::string t;
        if (!ReadAll(store + W(sha) + L".bill", t, 1u << 20)) return false;
        bytes.assign(t.begin(), t.end());
        return true;
    };
}

/* The recipe of the chain's active slots (and its Unison header): slot order, saved words replayed after the startup. */
static bool BuildFor(Chain &ch, plughost::ChainRecipe &r, std::vector<ULONG> &order, std::string &code, std::string &why)
{
    std::vector<plughost::Bundle> bs;
    std::vector<ChainSlot *> act;
    order.clear();
    for (ChainSlot &s : ch.slots) {
        if (!s.bypass) act.push_back(&s);
    }
    bs.resize(act.size());
    for (size_t k = 0; k < act.size(); k++) {
        if (!LoadSlotBundle(*act[k], ch.stereo, bs[k], why)) {
            act[k]->state = "failed";
            act[k]->code = code = "maps";
            act[k]->why = why;
            return false;
        }
    }
    std::vector<plughost::ChainMember> m;
    for (size_t k = 0; k < act.size(); k++) {
        m.push_back({ &bs[k], &act[k]->diff, act[k]->slot });
        order.push_back(act[k]->slot);
    }
    plughost::ChainOptions o;
    o.unit = g_u->model;
    o.input = ch.input;
    o.stereo = ch.stereo;
    o.aux = ch.aux;
    o.mon = ch.mon;
    o.specsDir = U8(g_plugins + L"\\specs");
    o.ownedList = U8(g_plugins + L"\\p1\\dsp-owned.txt");
    bool ok;
    if (ch.hasUnison && !ch.unison.bypass) {
        plughost::Bundle ub;
        if (!LoadSlotBundle(ch.unison, ch.stereo, ub, why)) { code = "maps"; return false; }
        if (!ub.unison) {
            code = "unison";
            why = "no Unison map of " + plughost::Label(ch.unison.name) + ": the plug-in mapper maps only the insert form of a plug-in "
                  "(its Unison preamp form, the integrated preamp header, is not mapped by open-apollo 2.1.0 either)";
            return false;
        }
        order.insert(order.begin(), kUnisonSlot);
        ok = plughost::BuildUnisonChain({ &ub, &ch.unison.diff, 0 }, m, StoreReader(), o, r, why);
    } else {
        ok = plughost::BuildChain(m, StoreReader(), o, r, why);
    }
    if (!ok) {
        code = "maps";
        why = "the chain cannot be built: " + why;
    }
    return ok;
}

/* The code pool of the strip DSP that is free now (the ledger's largest extent, or the estimate before the first CREATE). */
static ULONG CodeLargest()
{
    const APOLLO_PLUGIN_DSP_INFO &d = DspInfo();
    if (CapsOk() && (d.Flags & APOLLO_PLUGIN_DSP_LEDGER)) return d.Pool[0].Largest;
    return 16384 - 994;
}

/*
 * [Ins] DSP room per chain: the code words of the modules no other running chain of this unit holds, against the code pool's
 * largest free extent. StageFirst = the new chain fits next to the old one (built while the old still runs: the shortest
 * gap); else it must fit once the old chain's own modules are released (the old goes first: a longer gap). The driver's
 * ledger stays the judge; this only refuses what cannot fit either way and picks the order.
 */
static bool RoomCheck(const Chain &ch, const plughost::ChainRecipe &r, bool &stageFirst, std::string &why)
{
    std::set<std::string> others;
    for (const Chain &o : g_chains) {
        if (&o == &ch || !o.handle || o.slot != g_u->slot) continue;
        for (const auto &kv : o.mods) others.insert(kv.first);
    }
    std::map<std::string, ULONG> mine;
    for (const plughost::ChainEffect &e : r.effects) {
        for (const plughost::Module &m : e.modules) {
            if (m.pool == 0) mine[m.sha] = m.words;
        }
    }
    ULONG needStage = 0, needAfter = 0, freed = 0, largest = CodeLargest();
    for (const auto &kv : mine) {
        if (others.count(kv.first)) continue;
        needAfter += kv.second;
        if (!ch.mods.count(kv.first) || !ch.handle) needStage += kv.second;
    }
    for (const auto &kv : ch.mods) {
        if (ch.handle && !others.count(kv.first) && !mine.count(kv.first)) freed += kv.second;
    }
    stageFirst = needStage <= largest;
    if (!stageFirst && needAfter > largest + freed) {
        char b[200];
        snprintf(b, sizeof(b), "no room on DSP %lu: the chain needs %lu code words, %lu are free", StripDsp(), needAfter, largest + freed);
        why = b;
        return false;
    }
    return true;
}

/*
 * [Ins] The cycle budget: the strip DSP's own load word (window +0x0C, 1/100 %; UA's allocator stops at 10000 = 100 %,
 * oa:driver/ua_dsp.c ua_dsp_alloc_load). A chain that adds plug-ins is refused while the DSP is at kLoadRefuse or its
 * learned estimate would pass kLoadMax; after a build the load is sampled and a chain over kLoadMax is taken back.
 * What a plug-in costs is learned per name from the builds that added only it (memory only).
 */
static const ULONG kLoadRefuse = 8500, kLoadMax = 9500;
static std::map<std::string, ULONG> g_loadEst;

static ULONG ReadLoad()
{
    APOLLO_PLUGIN_CAPS c{};
    DWORD ret = 0, err = 0;
    if (!Ioctl(IOCTL_APOLLO_GET_PLUGIN_CAPS, nullptr, 0, &c, sizeof(c), &ret, &err) || ret < sizeof(c)) return 0;
    ULONG d = c.StripDsp < APOLLO_PLUGIN_DSPS ? c.StripDsp : 1;
    return c.Dsp[d].Load == 0xFFFFFFFFu ? 0 : c.Dsp[d].Load;
}

static bool CycleCheck(const Chain &ch, const std::vector<ULONG> &order, std::vector<std::string> &added, std::string &why)
{
    added.clear();
    for (ULONG s : order) {
        if (std::find(ch.running.begin(), ch.running.end(), s) != ch.running.end() && ch.handle) continue;
        const ChainSlot *x = (s == kUnisonSlot) ? &ch.unison : nullptr;
        for (const ChainSlot &y : ch.slots) if (y.slot == s) x = &y;
        if (x) added.push_back(x->name);
    }
    ULONG now = ReadLoad(), est = now;
    for (const std::string &n : added) {
        auto f = g_loadEst.find(n);
        if (f != g_loadEst.end()) est += f->second;
    }
    if (!added.empty() && now != 0 && (now >= kLoadRefuse || est > kLoadMax)) {
        char b[200];
        snprintf(b, sizeof(b), "DSP %lu is at %.1f %% load%s: no cycles left for another plug-in", StripDsp(), now / 100.0,
                 est > now ? (" (" + std::to_string((est + 50) / 100) + " % with it)").c_str() : "");
        why = b;
        return false;
    }
    return true;
}

/* One CREATE of the recipe (staged or started), its pages and readback; 0 and code/why on a refusal. */
static ULONG CreateChain(Chain &ch, const plughost::ChainRecipe &r, bool stage, std::string &code, std::string &why)
{
    APOLLO_PLUGIN_INSTANCE q{};
    q.Op = APOLLO_PLUGIN_INST_CREATE;
    q.Flags = (stage ? APOLLO_PLUGIN_F_STAGE : APOLLO_PLUGIN_F_START) | APOLLO_PLUGIN_F_CHAIN | (r.unison ? APOLLO_PLUGIN_F_UNISON : 0);
    q.RenderId = r.effects[0].renderId;
    q.Dsp = StripDsp();
    q.NResources = (ULONG)r.res.size();
    q.NPrivate = (ULONG)r.rows.size();
    q.NCommands = r.pages[0].second;
    q.Synch = r.in1 | (r.in2 << 16);
    q.Scratch[0] = r.scratch[0];
    q.Scratch[1] = r.scratch[1];
    q.Scratch[2] = r.scratch[2];
    DWORD err = 0;
    if (!Instance(q, &r.res, &r.rows, r.cmd.data(), (ULONG)r.cmd.size(), nullptr, &err)) {
        code = err == ERROR_NOT_SUPPORTED ? "hosting" : err == ERROR_ACCESS_DENIED ? "caller" : err == ERROR_BUSY ? "busy" : "driver";
        why = err == ERROR_NOT_SUPPORTED ? "plug-in hosting is off or not on this unit (or this chain kind is off)" :
              err == ERROR_ACCESS_DENIED ? "the driver takes plug-in calls from another program: turn hosting on again here" :
              err == ERROR_BUSY ? "the plug-in host is busy (a strip pass?): try again" : "the CREATE call failed: " + U8(ErrorText(err));
        return 0;
    }
    if (q.Status != 0) {
        code = Refusal(q, why);
        return 0;
    }
    ULONG handle = q.Handle, epoch = q.Epoch, bad = 0;
    ch.epoch = epoch;
    if (stage) {
        for (size_t k = 1; k < r.pages.size(); k++) {
            LONG st = 0;
            if (!ServiceWrite(handle, epoch, r.cmd.data() + r.pages[k].first, r.pages[k].second, st, err)) {
                APOLLO_PLUGIN_INSTANCE s{};
                s.Status = st;
                s.Step = APOLLO_PLUGIN_STEP_SERVICE_WRITE;
                code = Refusal(s, why);
                why = "startup page " + std::to_string(k + 1) + ": " + why;
                Destroy(handle);
                return 0;
            }
        }
        if (!Readback(handle, epoch, ch.owned, r.obs, bad, why) || bad) {
            code = "readback";
            why = "its startup did not read back as written (" + why + "): nothing was routed";
            Destroy(handle);
            return 0;
        }
        if (!HeadCheck(handle, epoch, r)) {
            code = "readback";
            why = "the chain head did not read back as written: nothing was routed";
            Destroy(handle);
            return 0;
        }
    } else if (!Readback(handle, epoch, ch.owned, r.obs, bad, why) || bad) {
        code = "readback";
        why = "its startup did not read back as written (" + why + "): the plain strip is back";
        Destroy(handle);
        return 0;
    }
    return handle;
}

/* Route a staged chain: op START, or for a Unison chain BEGIN, ACTIVATE, CUSTOM 1 and its 80 ms (the worker's journal). */
static bool RouteChain(Chain &ch, ULONG handle, const plughost::ChainRecipe &r, std::string &code, std::string &why)
{
    LONG st = 0;
    if (!r.unison) {
        APOLLO_PLUGIN_INSTANCE s{};
        DWORD err = 0;
        s.Op = APOLLO_PLUGIN_INST_START;
        s.Handle = handle;
        s.Epoch = ch.epoch;
        if (!Instance(s, nullptr, nullptr, nullptr, 0, nullptr, &err) || s.Status != 0) {
            code = Refusal(s, why);
            why = "START: " + why;
            return false;
        }
        return true;
    }
    auto refuse = [&](const char *step) {
        APOLLO_PLUGIN_INSTANCE s{};
        s.Status = st;
        code = (ULONG)st == 0xC000022Du ? "busy" : Refusal(s, why);
        why = std::string(step) + ": " + why;
        return false;
    };
    if (!UnisonOp(handle, ch.epoch, APOLLO_PLUGIN_INST_BEGIN_UNISON, {}, st)) return refuse("the Unison lease (BEGIN)");
    ch.analog = true;
    if (!UnisonOp(handle, ch.epoch, APOLLO_PLUGIN_INST_ACTIVATE_UNISON, {}, st)) return refuse("ACTIVATE_UNISON");
    std::vector<ULONG> custom;
    for (ULONG in : { r.in1, r.in2 }) {
        if (in == plughost::kSynchMono) continue;
        custom.insert(custom.end(), { in, kPreCustom, 1 });
    }
    if (!UnisonOp(handle, ch.epoch, APOLLO_PLUGIN_INST_UNISON_PARAMS, custom, st)) return refuse("CUSTOM on");
    Sleep(kUnisonSettleMs);
    return true;
}

static void SetSlotStates(Chain &ch)
{
    auto one = [&](ChainSlot &s) {
        s.base = 0;
        for (const plughost::ChainEffect &e : ch.run.effects) {
            if (ch.handle && e.slot == s.slot && !(&s == &ch.unison) == !(e.slot == kUnisonSlot)) s.base = e.base;
        }
        if (s.bypass) { s.state = "bypassed"; s.code.clear(); s.why.clear(); }
        else if (ch.handle && s.base) { s.state = "running"; s.code.clear(); s.why.clear(); }
        else { s.state = ch.state == "failed" ? "failed" : "waiting"; s.code = ch.code; s.why = ch.why; }
    };
    for (ChainSlot &s : ch.slots) one(s);
    if (ch.hasUnison) one(ch.unison);
}

static void ForgetRun(Chain &ch)
{
    ch.handle = 0;
    ch.running.clear();
    ch.run = plughost::ChainRecipe{};
    ch.mods.clear();
    ch.analog = false;
}

/* DESTROY through the handle of the slot that holds the chain, whichever unit is being worked on. */
static void DestroyOn(Chain &ch)
{
    if (ch.handle && ch.slot >= 0 && ch.slot < MAX_UNITS && ch.gen == g_slots[ch.slot].openGen.load()) {
        UnitState *keep = g_u;
        g_u = &g_unitState[ch.slot];
        Destroy(ch.handle);
        g_u = keep;
    }
    ForgetRun(ch);
}

/*
 * [Ins] Bring the chain's DSP state to its configuration: the active slots (slot order) composed into one chain, built
 * next to the running one when it fits (stage, pages, readback, then the old one destroyed and the new one routed: the
 * gap is two transactions), else after the old one went; no active slot: the plain strip. True when the configured chain
 * runs (or nothing should). On false the old chain is gone only if ch.handle is 0 (the caller may rebuild the previous).
 */
static bool DoChain(Chain &ch)
{
    std::string why, code = Blocked(ch, why);
    ch.code.clear();
    ch.why.clear();
    auto fail = [&](const std::string &c, const std::string &w, bool wait) {
        ch.state = wait ? "waiting" : "failed";
        ch.code = c;
        ch.why = w;
        ch.busy.clear();
        SetSlotStates(ch);
        return false;
    };
    if (!ch.active()) {
        if (ch.handle) DestroyOn(ch);
        ch.state = "idle";
        ch.busy.clear();
        SetSlotStates(ch);
        return true;
    }
    if (!code.empty()) {
        return fail(code, why, code == "warm" || code == "absent" || code == "strips" || code == "hosting" || code == "caller" ||
                               code == "unit" || code == "rate" || code == "aux");
    }
    plughost::ChainRecipe r;
    std::vector<ULONG> order;
    if (!BuildFor(ch, r, order, code, why)) return fail(code, why, false);
    if ((r.staged() || r.unison) && !(g_caps.Features & APOLLO_PLUGIN_FEAT_STAGE)) {
        return fail("staged", "its startup is over one page and the installed apollo.sys cannot stage one", false);
    }
    if (r.unison && !(g_caps.Features & APOLLO_PLUGIN_FEAT_UNISON)) return fail("driver", "the installed apollo.sys has no Unison lease", false);
    bool stageFirst = true;
    if (!RoomCheck(ch, r, stageFirst, why)) return fail("room", why, false);
    std::vector<std::string> added;
    if (!CycleCheck(ch, order, added, why)) return fail("cycles", why, false);
    const bool canStage = (g_caps.Features & APOLLO_PLUGIN_FEAT_STAGE) != 0;
    ch.state = "loading";
    ch.busy = r.staged() ? "Loading (" + std::to_string(r.pages.size()) + " startup pages)" : "Loading";
    PostState();
    ch.loadBefore = ReadLoad();
    ULONG gen = g_slots[g_u->slot].openGen.load();
    bool oldLive = ch.handle && ch.slot == g_u->slot && ch.gen == gen;
    if (!oldLive && ch.handle) ForgetRun(ch);       /* a handle of a closed slot: the driver destroyed it */
    ch.owned.clear();
    for (const plughost::Owned &w : r.owned) ch.owned.insert({ w.row, w.word });
    ULONG h = 0;
    if (oldLive && canStage && stageFirst) {
        h = CreateChain(ch, r, true, code, why);
        if (h == 0 && code != "room") return fail(code, why, false);    /* the old chain still runs */
    }
    if (h == 0) {
        if (oldLive) DestroyOn(ch);                  /* the old chain goes first (no room for both, or no staging) */
        h = CreateChain(ch, r, canStage, code, why);
        if (h == 0) return fail(code, why, false);
    } else if (oldLive) {
        DestroyOn(ch);                               /* the new chain is staged and checked: the old one goes now */
    }
    ch.handle = h;
    ch.slot = g_u->slot;
    ch.gen = gen;
    if (canStage && !RouteChain(ch, h, r, code, why)) {
        Destroy(h);
        ForgetRun(ch);
        return fail(code, why, false);
    }
    /* the cycle budget, measured */
    ULONG peak = 0;
    for (int k = 0; k < 3; k++) {
        Sleep(100);
        peak = std::max(peak, ReadLoad());
    }
    ch.loadAfter = peak;
    if (peak > kLoadMax) {
        Destroy(h);
        ForgetRun(ch);
        char b[160];
        snprintf(b, sizeof(b), "DSP %lu ran at %.1f %% with this chain (over %.0f %%): taken off again", StripDsp(), peak / 100.0, kLoadMax / 100.0);
        if (added.size() == 1 && peak > ch.loadBefore) g_loadEst[added[0]] = peak - ch.loadBefore;
        return fail("cycles", b, false);
    }
    if (added.size() == 1 && peak > ch.loadBefore) g_loadEst[added[0]] = peak - ch.loadBefore;
    r.cmd.clear();
    r.cmd.shrink_to_fit();
    r.obs.clear();
    r.pages.clear();
    ch.run = std::move(r);
    if (ch.run.unison && !ch.run.effects.empty()) ch.run.effects[0].slot = kUnisonSlot;
    ch.running = order;
    ch.runMon = ch.mon;
    ch.mods.clear();
    for (const plughost::ChainEffect &e : ch.run.effects) {
        for (const plughost::Module &m : e.modules) if (m.pool == 0) ch.mods[m.sha] = m.words;
    }
    for (const plughost::ChainEffect &e : ch.run.effects) {
        if (ChainSlot *s = SlotOf(ch, e.slot)) {
            s->codeWords = e.codeWords;
            s->pages = e.pages;
        }
    }
    ch.state = "running";
    ch.busy.clear();
    ch.triedEpoch = ch.epoch;
    SetSlotStates(ch);
    if (ch.aux < 0) ReportLatency(ch.handle, ch.epoch, ch.run.recordLatency, ch.run.recordDelay);   /* [Lat] */
    return true;
}

/* A user's change: on a refusal the previous configuration comes back (rebuilt if the attempt had taken it down). */
static bool ApplyChange(Chain &ch, const Chain &prev)
{
    if (DoChain(ch)) return true;
    std::string c = ch.code, w = ch.why;
    bool waiting = ch.state == "waiting";
    ch.slots = prev.slots;
    ch.mon = prev.mon;
    ch.stereo = prev.stereo;
    ch.hasUnison = prev.hasUnison;
    ch.unison = prev.unison;
    if (!waiting && prev.handle && !ch.handle) {
        (void)DoChain(ch);                  /* the old chain had gone: build it again */
    } else {
        ch.state = ch.handle ? "running" : (ch.active() ? (waiting ? "waiting" : "failed") : "idle");
        ch.busy.clear();
        SetSlotStates(ch);                  /* the old chain still runs as it was */
    }
    ch.code = c;                            /* the refusal stays readable on the chain until its next change */
    ch.why = w;
    return false;
}

/* The chain of this unit on an analog input (either input of a pair), or on an aux bus. */
static Chain *FindChain(ULONG input, LONG aux)
{
    for (Chain &ch : g_chains) {
        if (ch.unit != g_unitId) continue;
        if (aux >= 0 ? ch.aux == aux : ch.covers(input)) return &ch;
    }
    return nullptr;
}

static void CloseHelpers(Chain &ch)
{
    auto one = [&](ChainSlot &s, ULONG k) {
        if (s.helper && g_helperProc) {
            aggjson::Value r;
            std::string why;
            HelperCall("{\"op\":\"close\",\"key\":" + Q(ch.slotKey(k)) + "}", r, why);
        }
        s.helper = false;
    };
    for (ChainSlot &s : ch.slots) one(s, s.slot);
    if (ch.hasUnison) one(ch.unison, kUnisonSlot);
}

static void RemoveChain(Chain &ch)
{
    DestroyOn(ch);
    CloseHelpers(ch);
    for (auto it = g_chains.begin(); it != g_chains.end(); ++it) {
        if (&*it == &ch) { g_chains.erase(it); return; }
    }
}

/* The chain a new insert goes into: this target's, a new one, or (layout change) the one converted / the overlapping ones dropped. */
static Chain *ChainFor(ULONG input, LONG aux, bool stereo, std::string &code, std::string &why)
{
    if (aux >= 0) {
        if (Chain *ch = FindChain(0, aux)) return ch;
        Chain n;
        n.unit = g_unitId;
        n.aux = aux;
        n.stereo = true;
        g_chains.push_back(n);
        return &g_chains.back();
    }
    Chain *same = nullptr;
    for (Chain &ch : g_chains) {
        if (ch.unit == g_unitId && ch.aux < 0 && ch.input == input && ch.stereo == stereo) same = &ch;
    }
    if (same) return same;
    /* another layout here: its other slots must exist in the new layout, else it is refused */
    for (Chain &ch : g_chains) {
        if (ch.unit != g_unitId || ch.aux >= 0) continue;
        bool overlap = ch.covers(input) || (stereo && ch.covers(input + 1));
        if (!overlap) continue;
        if (ch.input == input && !ch.slots.empty()) {
            for (ChainSlot &s : ch.slots) {
                if (!IndexFor(s.name, stereo ? "stereo" : "mono")) {
                    code = "layout";
                    why = plughost::Label(s.name) + " has no " + (stereo ? "stereo" : "mono") + " map: remove it before " +
                          (stereo ? "linking" : "unlinking") + " this input's inserts";
                    return nullptr;
                }
            }
        }
    }
    Chain *conv = nullptr;
    for (auto it = g_chains.begin(); it != g_chains.end();) {
        Chain &ch = *it;
        bool overlap = ch.unit == g_unitId && ch.aux < 0 && (ch.covers(input) || (stereo && ch.covers(input + 1)));
        if (overlap && ch.input == input && conv == nullptr) {
            /* convert: same input, the other layout's maps */
            DestroyOn(ch);
            CloseHelpers(ch);
            for (ChainSlot &s : ch.slots) {
                const plughost::IndexEntry *e = IndexFor(s.name, stereo ? "stereo" : "mono");
                if (e) s.file = e->file;
                s.sha.clear();
                s.diff.clear();
                s.values.clear();
                s.texts.clear();
            }
            if (ch.hasUnison) ch.hasUnison = false;
            ch.stereo = stereo;
            ch.triedEpoch = 0;              /* rebuilt in its new layout by the next restore pass if the new insert fails */
            ch.state = ch.active() ? "waiting" : "idle";
            conv = &ch;
            ++it;
        } else if (overlap) {
            DestroyOn(ch);
            CloseHelpers(ch);
            it = g_chains.erase(it);
        } else {
            ++it;
        }
    }
    if (conv) return conv;
    Chain n;
    n.unit = g_unitId;
    n.input = input;
    n.stereo = stereo;
    g_chains.push_back(n);
    return &g_chains.back();
}

/* ------------------------------------------------------------------ elevation (Settings: hosting, modules) */

/* The user this process runs as, as a SID string ("S-1-5-21-..."), or '' (never guessed). */
static std::wstring OwnUserSid()
{
    HANDLE tok = nullptr;
    std::wstring out;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return out;
    DWORD need = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &need);
    std::vector<BYTE> buf(need ? need : 1);
    LPWSTR text = nullptr;
    if (need && GetTokenInformation(tok, TokenUser, buf.data(), need, &need) &&
        ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buf.data())->User.Sid, &text) && text) {
        out = text;
        LocalFree(text);
    }
    CloseHandle(tok);
    return out;
}

/* A SID string the driver takes (plugin_plan.h PlugSidTextOk) that Windows also reads as a valid SID. */
static bool SidTextOk(const std::wstring &t)
{
    PSID sid = nullptr;
    bool ok = PlugSidTextOk(t.c_str(), (ULONG)t.size()) && ConvertStringSidToSidW(t.c_str(), &sid) && IsValidSid(sid);
    if (sid) LocalFree(sid);
    return ok;
}

static bool Admin(const std::string &hosting, bool modules, std::string &why)
{
    std::wstring req = AppDataDir(FOLDERID_LocalAppData) + L"\\plugin-admin.json", res = req + L".result";
    /* the user this (unelevated) Apollo Control runs as: the elevated step may run as another administrator */
    std::string j = "{\"hosting\":" + Q(hosting) + ",\"user\":" + Q(U8(OwnUserSid()));
    if (modules) {
        std::set<std::string> list;
        for (const auto &kv : g_cat) {
            if (!kv.second.item.ok) continue;
            for (const std::string &m : kv.second.modules) {
                plughost::Sha s{};
                bool pin = false;
                if (plughost::ParseShaHex(m, s)) {
                    for (int k = 0; k < APOLLO_STRIP_MODULES; k++) pin |= memcmp(g_ApolloStripPins[k].Sha256, s.data(), 32) == 0;
                }
                if (!pin) list.insert(m);
            }
        }
        j += ",\"from\":" + Q(U8(g_plugins + L"\\store\\modules")) + ",\"modules\":[";
        bool first = true;
        for (const std::string &m : list) { j += (first ? "" : ",") + Q(m); first = false; }
        j += "]";
    }
    j += "}";
    DeleteFileW(res.c_str());
    if (!WriteAtomic(req, j)) { why = "the request could not be written"; return false; }
    wchar_t exe[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, exe, _countof(exe));
    std::wstring params = L"--plugin-admin \"" + req + L"\"";
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = n ? exe : nullptr;
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || sei.hProcess == nullptr) {
        DWORD e = GetLastError();
        why = e == ERROR_CANCELLED ? "cancelled at the administrator prompt: nothing changed" : "the elevated step did not start: " + U8(ErrorText(e));
        return false;
    }
    for (int k = 0; k < 900 && !g_quit && WaitForSingleObject(sei.hProcess, 200) == WAIT_TIMEOUT; k++) {
        /* up to 3 minutes for the administrator prompt; quitting the app does not wait for it */
    }
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    std::string t;
    ReadAll(res, t);
    aggjson::Value r;
    aggjson::Parser p(t);
    if (!p.Parse(r)) { why = "the elevated step left no result"; return false; }
    why = r.Str("detail");
    const aggjson::Value *ok = r.Get("ok");
    return code == 0 && ok && ok->kind == aggjson::Value::Bool && ok->b;
}

/* ------------------------------------------------------------------ state for the UI */

static std::string ParamsJson(const ChainSlot &s)
{
    std::string j = "[";
    for (size_t k = 0; k < s.params.size(); k++) {
        const plughost::Param &p = s.params[k];
        j += (k ? ",{" : "{") + std::string("\"name\":") + Q(p.name) + ",\"max\":" + std::to_string(p.max) +
             ",\"value\":" + std::to_string(k < s.values.size() ? s.values[k] : 0) +
             ",\"text\":" + Q(k < s.texts.size() ? s.texts[k] : std::string()) + ",\"steps\":";
        if (p.steps.empty()) j += "null";
        else {
            j += "[";
            for (size_t t = 0; t < p.steps.size(); t++) j += (t ? "," : "") + Q(p.steps[t]);
            j += "]";
        }
        j += "}";
    }
    return j + "]";
}

static std::string SlotJson(const ChainSlot &s, bool unison)
{
    return "{\"slot\":" + (unison ? std::string("\"unison\"") : std::to_string(s.slot)) + ",\"name\":" + Q(s.name) +
           ",\"label\":" + Q(plughost::Label(s.name)) + ",\"bypass\":" + (s.bypass ? "true" : "false") + ",\"state\":" + Q(s.state) +
           ",\"code\":" + Q(s.code) + ",\"why\":" + Q(s.why) + ",\"latency\":" + std::to_string(s.latency) +
           ",\"codeWords\":" + std::to_string(s.codeWords) + ",\"pages\":" + std::to_string(s.pages) + ",\"params\":" + ParamsJson(s) + "}";
}

/* [Ins] One chain for the UI (docs/apollo-control-units.md 1.9). */
static std::string ChainJson(const Chain &ch)
{
    char b[256];
    std::string j = "{\"input\":" + std::to_string(ch.input) + ",\"aux\":" + (ch.aux >= 0 ? std::to_string(ch.aux) : std::string("null")) +
                    ",\"stereo\":" + (ch.stereo ? "true" : "false") + ",\"mon\":" + (ch.mon ? "true" : "false") +
                    ",\"state\":" + Q(ch.state) + ",\"code\":" + Q(ch.code) + ",\"why\":" + Q(ch.why) + ",\"busy\":" + Q(ch.busy);
    ULONG lat = 0;
    for (const ChainSlot &s : ch.slots) lat += s.bypass ? 0 : s.latency;
    if (ch.hasUnison && !ch.unison.bypass) lat += ch.unison.latency;
    snprintf(b, sizeof(b), ",\"latency\":%lu,\"recordDelay\":%lu,\"overloaded\":%s,\"running\":%s,\"load\":{\"before\":%lu,\"after\":%lu}",
             lat, ch.handle ? ch.run.recordDelay : 0, ch.handle && ch.run.overloaded ? "true" : "false", ch.handle ? "true" : "false",
             ch.loadBefore, ch.loadAfter);
    j += b;
    j += ",\"slots\":[";
    for (size_t k = 0; k < ch.slots.size(); k++) j += (k ? "," : "") + SlotJson(ch.slots[k], false);
    j += "],\"unison\":" + (ch.hasUnison ? SlotJson(ch.unison, true) : std::string("null"));
    j += std::string(",\"analog\":") + (ch.analog ? "true" : "false") + "}";
    return j;
}

/* The pre-[Ins] "inserts" row of a chain (its first slot that is not bypassed, else its first): a page from before chains. */
static std::string LegacyInsertJson(const Chain &ch)
{
    const ChainSlot *s = nullptr;
    for (const ChainSlot &x : ch.slots) if (!x.bypass) { s = &x; break; }
    if (!s && !ch.slots.empty()) s = &ch.slots[0];
    if (!s || ch.aux >= 0) return {};
    return "{\"input\":" + std::to_string(ch.input) + ",\"stereo\":" + (ch.stereo ? "true" : "false") +
           ",\"bypass\":" + (s->bypass ? "true" : "false") + ",\"name\":" + Q(s->name) + ",\"label\":" + Q(plughost::Label(s->name)) +
           ",\"state\":" + Q(s->state) + ",\"code\":" + Q(s->code.empty() ? ch.code : s->code) + ",\"why\":" + Q(s->why.empty() ? ch.why : s->why) +
           ",\"busy\":" + Q(ch.busy) + ",\"latency\":" + std::to_string(s->latency) + ",\"codeWords\":" + std::to_string(s->codeWords) +
           ",\"pages\":" + std::to_string(s->pages) + ",\"params\":" + ParamsJson(*s) + "}";
}

/* [P7] Is a chain this unit's (the one being worked on)? A unit never seen yet in slot 0 shows the chains no other unit claims. */
static bool Mine(const Chain &ch)
{
    if (!g_unitId.empty()) return ch.unit == g_unitId;
    if (g_u->slot != 0) return false;
    for (const UnitState &u : g_unitState) {
        if (&u != g_u && !u.unitId.empty() && u.unitId == ch.unit) return false;
    }
    return true;
}

/* [P7] What the unit is to the UI: its model, inputs, pairs, Hi-Z, whether this driver hosts there. */
static std::string UnitJson()
{
    const PLUG_MODEL *m = g_u->model ? g_u->model : (g_u->slot == 0 ? PlugModelFor(PLUG_MODEL_X8P) : nullptr);
    if (!m) return ",\"model\":null,\"inputs\":0,\"pairs\":[],\"hiz\":[],\"supported\":false";
    std::string j = std::string(",\"model\":") + (m->Model == PLUG_MODEL_TWINX ? "\"twinx\"" : "\"x8p\"") +
                    ",\"inputs\":" + std::to_string(m->Inputs) + ",\"pairs\":[";
    for (ULONG k = 0; k + 1 < m->Inputs; k += 2) j += (k ? "," : "") + std::to_string(k);
    j += "],\"hiz\":[";
    for (ULONG k = 0, n = 0; k < m->Inputs; k++) {
        if (m->HizMask & (1u << (m->FirstSynch + k))) j += (n++ ? "," : "") + std::to_string(k);
    }
    /* the x8p always (any P0 driver); another model only with a driver that hosts there (FEAT_UNITS) */
    bool supported = m->Model == PLUG_MODEL_X8P || !CapsOk() || (g_caps.Features & APOLLO_PLUGIN_FEAT_UNITS);
    /* [Ins] what this unit's driver does with chains: 4 slots, REC/MON, Unison (lease built, no map), aux buses */
    j += "],\"supported\":" + std::string(supported ? "true" : "false") + ",\"slots\":4,\"recmon\":true,\"auxBuses\":2";
    j += std::string(",\"unison\":") + ((CapsOk() && (g_caps.Features & APOLLO_PLUGIN_FEAT_UNISON)) ? "true" : "false");
    j += std::string(",\"auxInserts\":") + ((CapsOk() && (g_caps.Features & APOLLO_PLUGIN_FEAT_AUX) && (g_caps.Enabled & APOLLO_PLUGIN_ENABLED_AUX)) ? "true" : "false");
    return j;
}

static void PostState(bool force)
{
    if (g_u->slot != 0 && !g_multi) return;
    const APOLLO_PLUGIN_DSP_INFO &d = DspInfo();
    bool ledger = CapsOk() && (d.Flags & APOLLO_PLUGIN_DSP_LEDGER);
    /* before the first CREATE builds the ledger: the measured layout, 16384 code words less the strips' 994
       (the same on the x8p and the Twin X: the strips' code does not depend on their number) */
    ULONG total = ledger ? d.Pool[0].Total : 16384, room = ledger ? d.Pool[0].Free : 16384 - 994, largest = ledger ? d.Pool[0].Largest : room;
    char b[512];
    std::string j = "{\"type\":\"plugins\",\"unit\":" + std::to_string(g_u->slot) + ",\"present\":";
    j += g_present ? "true" : "false";
    j += ",\"unitId\":" + Q(g_unitId) + UnitJson();
    snprintf(b, sizeof(b), ",\"hosting\":{\"driver\":%s,\"on\":%s,\"caller\":%s,\"stereo\":%s,\"staged\":%s,\"callerRule\":%s,\"why\":",
             CapsOk() ? "true" : "false", HostingOn() ? "true" : "false", CallerOk() ? "true" : "false",
             (CapsOk() && (g_caps.Features & APOLLO_PLUGIN_FEAT_STEREO)) ? "true" : "false",
             (CapsOk() && (g_caps.Features & APOLLO_PLUGIN_FEAT_STAGE)) ? "true" : "false",
             (CapsOk() && (g_caps.Features & APOLLO_PLUGIN_FEAT_CALLER)) ? "true" : "false");
    j += b;
    j += Q(CapsOk() ? std::string(g_caps.Why, strnlen(g_caps.Why, sizeof(g_caps.Why))) : std::string()) + ",\"admin\":" + Q(g_admin) + "}";
    snprintf(b, sizeof(b), ",\"dsp\":{\"index\":%lu,\"room\":%lu,\"total\":%lu,\"largest\":%lu,\"estimate\":%s,\"quarantined\":%s,"
                           "\"owned\":%s,\"admitted\":%s,\"load\":%lu",
             StripDsp(), room, total, largest, ledger ? "false" : "true", Quarantined() ? "true" : "false",
             (CapsOk() && (g_caps.OwnedMask & (1u << StripDsp()))) ? "true" : "false",
             (CapsOk() && (d.Flags & APOLLO_PLUGIN_DSP_ADMITTED)) ? "true" : "false", CapsOk() ? d.Load : 0);
    j += b;
    {
        /* [Warm] owned through a warm adoption (and proven by an acknowledged upload); else why not, with the action */
        std::string ow;
        bool owned = CapsOk() && (g_caps.OwnedMask & (1u << StripDsp())) &&
                     !(Warm() && !(g_caps.Features & APOLLO_PLUGIN_FEAT_WARM));
        if (CapsOk() && !owned) (void)OwnBlock(ow);
        j += std::string(",\"warm\":") + (CapsOk() && (d.Flags & APOLLO_PLUGIN_DSP_WARM) ? "true" : "false") +
             ",\"proven\":" + (CapsOk() && (d.Flags & APOLLO_PLUGIN_DSP_WARM_PROVEN) ? "true" : "false") +
             ",\"ownWhy\":" + Q(ow);
    }
    /* the four pools from the ledger; [] before it is valid (the page then estimates pool 0 from room/total) */
    j += ",\"pools\":[";
    for (size_t k = 0; ledger && k < _countof(d.Pool); k++) {
        const APOLLO_PLUGIN_POOL_USAGE &u = d.Pool[k];
        snprintf(b, sizeof(b), "%s{\"total\":%lu,\"used\":%lu,\"free\":%lu,\"largest\":%lu,\"scratch\":%lu}", k ? "," : "",
                 u.Total, u.Used, u.Free, u.Largest, u.Scratch);
        j += b;
    }
    j += "]}";
    snprintf(b, sizeof(b), ",\"strips\":{\"live\":%s,\"started\":%lu,\"warm\":%s,\"state\":%lu}", StripsLive() ? "true" : "false",
             CapsOk() ? g_caps.StripsStarted : 0, Warm() ? "true" : "false", CapsOk() ? g_caps.StripsState : 0);
    j += b;
    j += ",\"maps\":{\"ok\":" + std::string(g_plugins.empty() ? "false" : "true") + ",\"dir\":" + Q(U8(g_plugins)) +
         ",\"configured\":" + Q(g_mapsDir) + ",\"why\":" + Q(g_mapsWhy) + ",\"python\":" +
         (Exists(g_plugins + L"\\.venv\\Scripts\\python.exe") && !g_plugins.empty() ? "true" : "false") +
         ",\"catalog\":" + Q(g_catBuilding ? "building" : (g_cat.empty() ? "idle" : "ready")) +
         ",\"progress\":[" + std::to_string(g_catNext) + "," + std::to_string(g_index.size()) + "]}";
    j += ",\"inserts\":[";
    bool first = true;
    for (const Chain &ch : g_chains) {
        if (!Mine(ch)) continue;
        std::string row = LegacyInsertJson(ch);
        if (row.empty()) continue;
        j += (first ? "" : ",") + row;
        first = false;
    }
    j += "],\"chains\":[";                  /* [Ins] every chain: slots, REC/MON, Unison, aux */
    first = true;
    for (const Chain &ch : g_chains) {
        if (!Mine(ch)) continue;
        j += (first ? "" : ",") + ChainJson(ch);
        first = false;
    }
    j += "]}";
    if (force || j != g_lastState) {
        g_lastState = j;
        PostJson(W(j));
    }
}

static void PostList(bool force)
{
    if (g_u->slot != 0 && !g_multi) return;
    std::map<std::string, std::pair<const CatEntry *, const CatEntry *>> byName;     /* mono, stereo */
    for (const auto &kv : g_cat) {
        const CatEntry &c = kv.second;
        if (!c.item.ok || c.item.name.empty()) continue;
        auto &p = byName[c.item.name];
        (c.layout == "stereo" ? p.second : p.first) = &c;
    }
    ULONG authorized = 0;
    std::string items;
    for (const auto &kv : byName) {
        const CatEntry *m = kv.second.first, *s = kv.second.second, *a = m ? m : s;
        int lic = Licensed(a->item.renderId);
        if (lic > 0) authorized++;
        items += items.empty() ? "{" : ",{";
        items += "\"name\":" + Q(kv.first) + ",\"label\":" + Q(plughost::Label(kv.first)) + ",\"mono\":" + (m ? "true" : "false") +
                 ",\"stereo\":" + (s ? "true" : "false") + ",\"latency\":" + std::to_string(a->item.latency) +
                 ",\"codeWords\":" + std::to_string(a->item.codeWords) + ",\"dataWords\":" + std::to_string(a->item.dataWords) +
                 ",\"renderId\":" + std::to_string(a->item.renderId) + ",\"pages\":" + std::to_string(a->item.pages) +
                 ",\"params\":" + std::to_string(a->item.params) + ",\"licensed\":" + (lic < 0 ? "null" : lic ? "true" : "false") +
                 UadJson(kv.first) + "}";       /* category, tags, art (UA's own, from the local install) */
    }
    std::string j = "{\"type\":\"plugin-list\",\"unit\":" + std::to_string(g_u->slot) + ",\"state\":" +
                    Q(g_catBuilding ? "loading" : g_plugins.empty() ? "failed" : "ok") +
                    ",\"why\":" + Q(g_mapsWhy) + ",\"licence\":" + Q(g_licState) + ",\"licenceWhy\":" + Q(g_licWhy) +
                    ",\"mapped\":" + std::to_string(byName.size()) + ",\"authorized\":" + std::to_string(authorized) + ",\"items\":[" + items + "]}";
    if (force || j != g_lastList) {
        g_lastList = j;
        PostJson(W(j));
    }
}

/* ------------------------------------------------------------------ the thread */

/* One unit (g_u): its info, and with withCaps what its driver says about hosting there. */
static void RefreshUnit(bool withCaps)
{
    APOLLO_INFO info{};
    DWORD bytes = 0;
    Slot &slot = g_slots[g_u->slot];
    ULONG gen = slot.openGen.load();
    bool got = SlotInfo(slot, info, bytes) == 0;
    /* [P7] the models that host: the x8p and the Twin X Gen 1 (plugin_plan.h) */
    const PLUG_MODEL *model = !got ? nullptr : ApolloInfoIsX8p(&info, bytes) ? PlugModelFor(PLUG_MODEL_X8P) :
                              ApolloInfoIsTwinX(&info, bytes) ? PlugModelFor(PLUG_MODEL_TWINX) : nullptr;
    bool present = model != nullptr && info.Stage == ApolloStageConnected && UnitMode(info, bytes) == 2;
    g_present = present;
    if (present) {
        std::string id = UnitIdOf(info, bytes);
        if (id != g_unitId) {
            /* another unit in this slot: its own licence answer, nothing of the last one's */
            g_lic.clear();
            g_licState = "unknown";
            g_licWhy.clear();
            g_licEpoch = 0;
        }
        g_u->model = model;
        g_info = info;
        g_infoBytes = bytes;
        g_unitId = id;
    }
    {
        /* [Rate] a rate change took the inserts down (driver rate.c): put them back once allowed again */
        static ULONG lastRate = 0;
        if (present && g_info.SampleRate != 0 && g_info.SampleRate != lastRate) {
            if (lastRate != 0) {
                for (Chain &ch : g_chains) ch.triedEpoch = 0;
            }
            lastRate = g_info.SampleRate;
        }
    }
    if (gen != g_gen) {
        /* the slot's handle was closed and opened again: the driver destroyed its instances */
        g_gen = gen;
        for (Chain &ch : g_chains) {
            if (ch.handle && ch.slot == g_u->slot) {
                ForgetRun(ch);
                ch.state = ch.active() ? "waiting" : "idle";
                ch.triedEpoch = 0;
                SetSlotStates(ch);
            }
        }
    }
    g_capsOk = false;
    if (!present || !withCaps) return;
    DWORD ret = 0;
    memset(&g_caps, 0, sizeof(g_caps));
    g_capsOk = Ioctl(IOCTL_APOLLO_GET_PLUGIN_CAPS, nullptr, 0, &g_caps, sizeof(g_caps), &ret, &g_capsErr) && ret >= sizeof(g_caps);
    if (!g_capsOk) return;
    /* A busy host fills no instance list ("busy: ..."): nothing can be concluded from it. */
    if (strncmp(g_caps.Why, "busy", 4) == 0) return;
    for (Chain &ch : g_chains) {
        if (!ch.handle || ch.slot != g_u->slot) continue;
        bool alive = false;
        for (ULONG k = 0; k < g_caps.Instances && k < APOLLO_PLUGIN_INSTANCES_MAX; k++) {
            const APOLLO_PLUGIN_INSTANCE_INFO &o = g_caps.Inst[k];
            if (o.Handle == ch.handle && (o.Flags & APOLLO_PLUGIN_INST_MINE) && !(o.Flags & APOLLO_PLUGIN_INST_LOST) && o.Epoch == g_caps.Epoch) alive = true;
        }
        ch.missing = alive ? 0 : ch.missing + 1;
        if (ch.missing >= 2 && g_caps.Instances <= APOLLO_PLUGIN_INSTANCES_MAX) {
            ch.missing = 0;
            Destroy(ch.handle);             /* lost twice in a row: DESTROY only forgets it */
            ForgetRun(ch);
            ch.state = Quarantined() ? "failed" : "waiting";
            ch.code = Quarantined() ? "quarantine" : "";
            ch.why = Quarantined() ? "DSP 1 is quarantined: power-cycle the " + Nm() : "";
            SetSlotStates(ch);
        }
    }
}

/* [P7] Every slot; g_u back on the x8p's slot 0 after. */
static void Refresh(bool withCaps)
{
    for (UnitState &u : g_unitState) {
        g_u = &u;
        RefreshUnit(withCaps);
    }
    g_u = &g_unitState[0];
}

/* Saved chains back on the DSP once the strips are live (once per epoch; a user's retry clears it). */
static void RestoreUnit()
{
    for (Chain &ch : g_chains) {
        if (!g_present || ch.unit != g_unitId || !ch.active() || ch.handle) continue;
        std::string why, code = Blocked(ch, why);
        if (!code.empty()) {
            ch.state = (code == "warm" || code == "absent" || code == "strips" || code == "hosting" || code == "caller" ||
                        code == "rate" || code == "unit" || code == "aux") ? "waiting" : "failed";
            ch.code = code;
            ch.why = why;
            SetSlotStates(ch);
            continue;
        }
        if (ch.triedEpoch == g_caps.Epoch) continue;
        ch.triedEpoch = g_caps.Epoch;
        DoChain(ch);
        PostState();
    }
}

/* [P7] each present unit's own saved inserts */
static void Restore()
{
    for (UnitState &u : g_unitState) {
        g_u = &u;
        if (g_present) RestoreUnit();
    }
    g_u = &g_unitState[0];
}

/* [P7] each unit's snapshot (slot 0 always, as before; another slot once it held a unit that hosts) */
static void PostStates(bool force = false)
{
    for (UnitState &u : g_unitState) {
        g_u = &u;
        if (u.slot == 0 || u.model != nullptr) PostState(force);
    }
    g_u = &g_unitState[0];
}

static void PostLists(bool force)
{
    for (UnitState &u : g_unitState) {
        g_u = &u;
        if (u.slot == 0 || u.model != nullptr) PostList(force);
    }
    g_u = &g_unitState[0];
}

/* [Ins] A parameter of one slot: planned in its own rows by the plug-in's setter, moved to the slot's rows in the chain. */
static void DoParam(const Cmd &c)
{
    Chain *ch = FindChain(c.input, c.aux);
    ULONG sn = c.hasSlot ? c.slot : (ch && !ch->slots.empty() ? ch->slots[0].slot : 0);
    ChainSlot *in = ch ? SlotOf(*ch, sn) : nullptr;
    if (!in || c.index >= in->params.size() || c.value > in->params[c.index].max) {
        PostResult("param", ch, c.input, (LONG)sn, false, "driver", "no such insert or parameter");
        return;
    }
    std::string why, key = ch->slotKey(sn);
    ch->busy = "Planning the change";
    PostState();
    aggjson::Value r;
    if (!HelperOpen(*ch, *in, sn, why) ||
        !HelperCall("{\"op\":\"plan\",\"key\":" + Q(key) + ",\"index\":" + std::to_string(c.index) + ",\"value\":" + std::to_string(c.value) + "}",
                    r, why)) {
        ch->busy.clear();
        PostResult("param", ch, c.input, (LONG)sn, false, "engine", why);
        PostState();
        return;
    }
    const aggjson::Value *prop = r.Get("proposal"), *rest = r.Get("restore");
    bool running = ch->handle && ch->state == "running" && in->base != 0 && ch->slot == g_u->slot &&
                   ch->gen == g_slots[g_u->slot].openGen.load();
    auto apply = [&](const aggjson::Value *plan, std::string &w) -> bool {
        if (!plan) { w = "no plan"; return false; }
        if (const aggjson::Value *chs = plan->Get("chunks")) {
            for (const aggjson::Value &chunk : chs->a) {
                Words cmd, moved;
                LONG st = 0;
                DWORD err = 0;
                if (!Words32(&chunk, cmd) || cmd.empty()) continue;
                if (!plughost::RebaseCmd(cmd, in->base, moved)) { w = "the plan writes outside the plug-in's rows"; return false; }
                if (!ServiceWrite(ch->handle, ch->epoch, moved.data(), (ULONG)moved.size(), st, err)) {
                    APOLLO_PLUGIN_INSTANCE s{};
                    s.Status = st;
                    Refusal(s, w);
                    return false;
                }
            }
        }
        std::vector<plughost::Obs> obs;
        if (const aggjson::Value *ob = plan->Get("observe")) {
            for (const aggjson::Value &x : ob->a) {
                plughost::Obs o;
                if (x.a.size() == 3 && plughost::U32(&x.a[0], o.row) && plughost::U32(&x.a[1], o.word) && Words32(&x.a[2], o.v) && !o.v.empty() &&
                    o.row >= 10) {
                    o.row = in->base + o.row - 10;      /* ComposedEffect._observations: the carrier's words are the chain's */
                    obs.push_back(o);
                }
            }
        }
        ULONG bad = 0;
        if (!Readback(ch->handle, ch->epoch, ch->owned, obs, bad, w)) return false;
        return bad == 0;
    };
    bool ok = true;
    std::string code;
    if (running) {
        ch->busy = "Sending the change";
        ok = apply(prop, why);
        if (!ok) {
            code = Quarantined() ? "quarantine" : "readback";
            std::string w2;
            apply(rest, w2);                /* put the prior words back (best effort) */
            aggjson::Value a2;
            HelperCall("{\"op\":\"accept\",\"key\":" + Q(key) + ",\"which\":\"restore\"}", a2, w2);
            TakeState(*in, a2);
        }
    }
    if (ok) {
        aggjson::Value a;
        if (HelperCall("{\"op\":\"accept\",\"key\":" + Q(key) + ",\"which\":\"proposal\"}", a, why)) {
            TakeState(*in, a);
            Save();
        } else {
            ok = false;
            code = "engine";
        }
    }
    ch->busy.clear();
    PostResult("param", ch, c.input, (LONG)sn, ok, code, ok ? std::string() : "not changed: " + why);
    PostState();
}

/*
 * [Ins] UAD REC / MON of a chain: stored (a chain built later is composed with it); on a running chain whose record delay
 * stays the same (no plug-in latency) one SERVICE writes the tap words (record_tap_plan) and reads them back, else the
 * chain is rebuilt with the other record delay (upstream's rule, RECORD-PRE-EFFECTS.md step 5).
 */
static bool DoRecMon(Chain &ch, bool mon, std::string &code, std::string &why)
{
    if (ch.aux >= 0) { code = "driver"; why = "an aux bus is always printed (no REC/MON)"; return false; }
    if (ch.mon == mon) return true;
    Chain prev = ch;
    ch.mon = mon;
    bool running = ch.handle && ch.state == "running" && ch.slot == g_u->slot && ch.gen == g_slots[g_u->slot].openGen.load();
    if (!running || ch.run.effects.empty()) return true;
    ULONG rd = 0, md = 0, cap = 0;
    bool over = false;
    plughost::ChainDelays(ch.run.latency, mon, rd, md, cap, over);
    if (ch.run.unison || rd != ch.run.recordDelay) {
        if (ApplyChange(ch, prev)) return true;
        code = ch.code;
        why = ch.why;
        return false;
    }
    Words cmd;
    std::vector<plughost::Obs> obs;
    plughost::TapPlan(ch.run, mon, cmd, obs);
    LONG st = 0;
    DWORD err = 0;
    ULONG bad = 0;
    if (!ServiceWrite(ch.handle, ch.epoch, cmd.data(), (ULONG)cmd.size(), st, err)) {
        APOLLO_PLUGIN_INSTANCE s{};
        s.Status = st;
        code = Refusal(s, why);
        ch.mon = prev.mon;
        return false;
    }
    if (!Readback(ch.handle, ch.epoch, ch.owned, obs, bad, why) || bad) {
        code = "readback";
        why = "the record tap did not read back as written: " + why;
        Words back;
        plughost::TapPlan(ch.run, prev.mon, back, obs);
        (void)ServiceWrite(ch.handle, ch.epoch, back.data(), (ULONG)back.size(), st, err);
        ch.mon = prev.mon;
        return false;
    }
    ch.runMon = mon;
    return true;
}

/* [Ins] Unison analog triples on the running Unison chain: gain (GainC dB-9, GainA/B dB-10), mic impedance, or raw. */
static bool DoUnisonAnalog(Chain &ch, const std::vector<ULONG> &words, std::string &code, std::string &why)
{
    if (!ch.handle || !ch.analog || !ch.run.unison) {
        code = "unison";
        why = "no Unison preamp runs on this input";
        return false;
    }
    LONG st = 0;
    if (!UnisonOp(ch.handle, ch.epoch, APOLLO_PLUGIN_INST_UNISON_PARAMS, words, st)) {
        APOLLO_PLUGIN_INSTANCE s{};
        s.Status = st;
        code = (ULONG)st == 0xC0000022u ? "unison" : Refusal(s, why);
        if ((ULONG)st == 0xC0000022u) why = "a field outside the Unison lease";
        return false;
    }
    return true;
}

static std::vector<ULONG> UnisonGainWords(const Chain &ch, LONG db)
{
    std::vector<ULONG> w;
    LONG c = std::clamp<LONG>(db - 9, 1, 56), ab = std::clamp<LONG>(db - 10, 0, 54);
    for (ULONG in : { ch.run.in1, ch.run.in2 }) {
        if (in == plughost::kSynchMono) continue;
        w.insert(w.end(), { in, kPreGainC, (ULONG)c, in, kPreGainA, (ULONG)ab, in, kPreGainB, (ULONG)ab });
    }
    return w;
}

static void Run(const Cmd &c)
{
    std::string why;
    g_u = &UnitAt(c.unit);                  /* [P7] the unit the message names (slot 0 = the x8p) */
    if (c.op == "state") {
        for (UnitState &u : g_unitState) u.lastState.clear();   /* one fresh snapshot of each */
        if (g_index.empty()) LoadIndex();
    } else if (c.op == "watch") {
        g_watch = true;                     /* the Plug-ins view is open: each second */
    } else if (c.op == "unwatch") {
        g_watch = false;
    } else if (c.op == "list") {
        g_listWanted = true;
        for (UnitState &u : g_unitState) u.lastList.clear();
        CatalogStart();
        if (g_present) Licence(true);       /* the unit's own answer: its picker lists what it is licensed for */
    } else if (c.op == "insert") {
        /* [Ins] {input | aux, slot (absent = 0: the old one-slot insert), name, stereo} */
        LONG sn = c.hasSlot ? (LONG)c.slot : 0;
        if (!g_present) { PostResult("insert", nullptr, c.input, sn, false, "absent", "the " + Nm() + " is not connected"); return; }
        bool stereo = c.aux >= 0 ? true : c.stereo;
        if (c.aux >= 0 ? (c.aux > 1 || !g_u->model) :
            (c.input >= plughost::kInputs || !g_u->model ||
             PlugModelSynch(g_u->model, (g_u->model->FirstSynch + c.input) |
                                        ((stereo ? g_u->model->FirstSynch + c.input + 1 : plughost::kSynchMono) << 16)) != PLUG_OK)) {
            PostResult("insert", nullptr, c.input, sn, false, "driver", "no such input, pair or aux bus on the " + Nm());
            return;
        }
        if (c.slot > 3) { PostResult("insert", nullptr, c.input, sn, false, "driver", "slots are 0-3"); return; }
        if (g_index.empty()) LoadIndex();
        const plughost::IndexEntry *e = IndexFor(c.name, stereo ? "stereo" : "mono");
        if (!e) { PostResult("insert", nullptr, c.input, sn, false, "maps", "no " + std::string(stereo ? "stereo" : "mono") + " map of " + c.name); return; }
        std::string code;
        why.clear();
        Chain *ch = ChainFor(c.input, c.aux, stereo, code, why);
        if (!ch) { PostResult("insert", nullptr, c.input, sn, false, code, why); return; }
        Chain prev = *ch;
        ChainSlot ns;
        ns.slot = (ULONG)sn;
        ns.name = c.name;
        ns.file = e->file;
        bool replaced = false;
        for (ChainSlot &s : ch->slots) {
            if (s.slot == ns.slot) {
                if (s.helper && g_helperProc) {
                    aggjson::Value hr;
                    std::string w;
                    HelperCall("{\"op\":\"close\",\"key\":" + Q(ch->slotKey(s.slot)) + "}", hr, w);
                }
                s = ns;
                replaced = true;
            }
        }
        if (!replaced) {
            ch->slots.push_back(ns);
            std::sort(ch->slots.begin(), ch->slots.end(), [](const ChainSlot &x, const ChainSlot &y) { return x.slot < y.slot; });
        }
        PostState();
        if (ApplyChange(*ch, prev)) {
            Save();
            PostResult("insert", ch, c.input, sn, true, "", "");
        } else {
            std::string co = ch->code, w = ch->why;
            if (!ch->active() && !ch->hasUnison && !ch->mon && ch->slots.empty()) RemoveChain(*ch);
            PostResult("insert", nullptr, c.input, sn, false, co, w);
        }
    } else if (c.op == "remove") {
        /* [Ins] {input | aux, slot}: that slot; no slot: the whole chain (its REC/MON setting stays) */
        if (Chain *ch = FindChain(c.input, c.aux)) {
            Chain prev = *ch;
            if (c.hasSlot) {
                for (auto it = ch->slots.begin(); it != ch->slots.end(); ++it) {
                    if (it->slot != c.slot) continue;
                    if (it->helper && g_helperProc) {
                        aggjson::Value hr;
                        std::string w;
                        HelperCall("{\"op\":\"close\",\"key\":" + Q(ch->slotKey(it->slot)) + "}", hr, w);
                    }
                    ch->slots.erase(it);
                    break;
                }
            } else {
                CloseHelpers(*ch);
                ch->slots.clear();
                ch->hasUnison = false;
            }
            bool ok = ApplyChange(*ch, prev);
            std::string co = ch->code, w = ch->why;
            if (ok && ch->slots.empty() && !ch->hasUnison && !ch->mon) RemoveChain(*ch);
            Save();
            PostResult("remove", ok ? nullptr : ch, c.input, c.hasSlot ? (LONG)c.slot : -1, ok, co, w);
        }
    } else if (c.op == "move") {
        /* [Ins] {input | aux, from, to}: to an empty slot it moves, onto a used one the two swap; the chain is rebuilt */
        Chain *ch = FindChain(c.input, c.aux);
        ChainSlot *a = ch ? SlotOf(*ch, c.from) : nullptr;
        if (!a || c.to > 3 || c.from == c.to) { PostResult("move", ch, c.input, (LONG)c.from, false, "driver", "no such slot"); return; }
        Chain prev = *ch;
        ChainSlot *b = SlotOf(*ch, c.to);
        CloseHelpers(*ch);
        if (b) {
            b->slot = c.from;
        }
        a->slot = c.to;
        std::sort(ch->slots.begin(), ch->slots.end(), [](const ChainSlot &x, const ChainSlot &y) { return x.slot < y.slot; });
        bool ok = ApplyChange(*ch, prev);
        if (ok) Save();
        PostResult("move", ch, c.input, (LONG)c.to, ok, ch->code, ch->why);
    } else if (c.op == "bypass") {
        /* [Ins] {input | aux, slot, on}; no slot: every slot (the old one-slot bypass) */
        Chain *ch = FindChain(c.input, c.aux);
        if (!ch) return;
        Chain prev = *ch;
        for (ChainSlot &s : ch->slots) {
            if (!c.hasSlot || s.slot == c.slot) s.bypass = c.on;
        }
        if (c.hasSlot && c.slot == kUnisonSlot && ch->hasUnison) ch->unison.bypass = c.on;
        bool ok = ApplyChange(*ch, prev);
        Save();
        PostResult("bypass", ch, c.input, c.hasSlot ? (LONG)c.slot : -1, ok, ch->code, ch->why);
    } else if (c.op == "retry") {
        if (Chain *ch = FindChain(c.input, c.aux)) {
            if (!c.hasSlot) for (ChainSlot &s : ch->slots) s.bypass = false;
            if (ch->handle) DestroyOn(*ch);
            bool ok = DoChain(*ch);
            Save();
            PostResult("retry", ch, c.input, -1, ok, ch->code, ch->why);
        }
    } else if (c.op == "param") {
        DoParam(c);
    } else if (c.op == "recmon") {
        /* [Ins] {input, mon}: UAD MON (true) records the dry input, the inserts are heard in the monitor mix only */
        if (c.aux >= 0 || c.input >= plughost::kInputs) { PostResult("recmon", nullptr, c.input, -1, false, "driver", "REC/MON is per analog input"); return; }
        Chain *ch = FindChain(c.input, -1);
        if (!ch) {
            if (!c.mon) { PostResult("recmon", nullptr, c.input, -1, true, "", ""); return; }
            Chain n;
            n.unit = g_unitId;
            n.input = c.input;
            n.stereo = false;
            g_chains.push_back(n);
            ch = &g_chains.back();
        }
        std::string code;
        why.clear();
        bool ok = DoRecMon(*ch, c.mon, code, why);
        if (ok && !ch->mon && ch->slots.empty() && !ch->hasUnison) { RemoveChain(*ch); ch = nullptr; }
        Save();
        PostResult("recmon", ch, c.input, -1, ok, code, why);
    } else if (c.op == "unison") {
        /* [Ins] {input, name}: the Unison preamp slot (its map must be a Unison form: none maps yet) */
        if (!g_present || c.input >= plughost::kInputs) { PostResult("unison", nullptr, c.input, -1, false, "absent", "the " + Nm() + " is not connected"); return; }
        Chain *cur = FindChain(c.input, -1);
        bool stereo = cur ? cur->stereo : false;
        const plughost::IndexEntry *e = IndexFor(c.name, stereo ? "stereo" : "mono");
        if (!e) { PostResult("unison", nullptr, c.input, -1, false, "maps", "no " + std::string(stereo ? "stereo" : "mono") + " map of " + c.name); return; }
        std::string code;
        why.clear();
        Chain *ch = ChainFor(c.input, -1, stereo, code, why);
        if (!ch) { PostResult("unison", nullptr, c.input, -1, false, code, why); return; }
        Chain prev = *ch;
        ch->hasUnison = true;
        ch->unison = ChainSlot{};
        ch->unison.slot = kUnisonSlot;
        ch->unison.name = c.name;
        ch->unison.file = e->file;
        bool ok = ApplyChange(*ch, prev);
        std::string co = ch->code, w = ch->why;
        if (!ok && !ch->active() && !ch->mon && ch->slots.empty() && !ch->hasUnison) { RemoveChain(*ch); ch = nullptr; }
        if (ok) Save();
        PostResult("unison", ch, c.input, -1, ok, co, w);
    } else if (c.op == "unison-remove") {
        if (Chain *ch = FindChain(c.input, -1)) {
            Chain prev = *ch;
            ch->hasUnison = false;
            bool ok = ApplyChange(*ch, prev);
            if (ok && ch->slots.empty() && !ch->mon) { RemoveChain(*ch); ch = nullptr; }
            Save();
            PostResult("unison-remove", ch, c.input, -1, ok, ch ? ch->code : "", ch ? ch->why : "");
        }
    } else if (c.op == "unison-gain" || c.op == "unison-impedance" || c.op == "unison-params") {
        /* [Ins] the analog lease: the real preamp's gain (dB 10-65), impedance code, or the plug-in's own triples */
        Chain *ch = FindChain(c.input, -1);
        std::string code = "unison";
        why = "no Unison preamp runs on this input";
        bool ok = false;
        if (ch) {
            std::vector<ULONG> w = c.op == "unison-gain" ? UnisonGainWords(*ch, c.db) : c.words;
            if (c.op == "unison-impedance") {
                w.clear();
                for (ULONG in : { ch->run.in1, ch->run.in2 }) {
                    if (in != plughost::kSynchMono) w.insert(w.end(), { in, kPreMicImp, c.code & 0xF });
                }
            }
            ok = DoUnisonAnalog(*ch, w, code, why);
        }
        PostResult(c.op.c_str(), ch, c.input, -1, ok, ok ? "" : code, ok ? "" : why);
    } else if (c.op == "hosting") {
        if (!c.on) {
            for (Chain &ch : g_chains) {
                if (ch.handle) { DestroyOn(ch); ch.state = "waiting"; SetSlotStates(ch); }     /* every unit's */
            }
        } else {
            if (!LoadIndex()) { PostResult("hosting", nullptr, 0, -1, false, "maps", g_mapsWhy); return; }
            if (g_cat.empty()) CacheLoad();
            g_catNext = 0;
            g_catBuilding = true;
            g_admin = "Reading the plug-in maps";
            PostState();
            while (g_catBuilding && !g_quit) CatalogStep();
        }
        g_admin = c.on ? "Waiting for the administrator prompt: hosting on and the module store" : "Waiting for the administrator prompt: hosting off";
        PostState();
        bool ok = Admin(c.on ? "on" : "off", c.on, why);
        g_admin.clear();
        for (Chain &ch : g_chains) ch.triedEpoch = 0;
        PostResult("hosting", nullptr, 0, -1, ok, ok ? "" : "admin", why);
    } else if (c.op == "modules") {
        if (!LoadIndex()) { PostResult("modules", nullptr, 0, -1, false, "maps", g_mapsWhy); return; }
        if (g_cat.empty()) CacheLoad();
        g_catNext = 0;
        g_catBuilding = true;
        while (g_catBuilding && !g_quit) CatalogStep();
        g_admin = "Waiting for the administrator prompt: the module store";
        PostState();
        bool ok = Admin("", true, why);
        g_admin.clear();
        for (Chain &ch : g_chains) ch.triedEpoch = 0;
        PostResult("modules", nullptr, 0, -1, ok, ok ? "" : "admin", why);
    } else if (c.op == "session-get") {
        /* [Sess] every unit's chains as plugins.json version 2 holds them: a session keeps each unit's part (docs 1.11) */
        std::string doc = plughost::ChainsToJson(SavedList(), g_mapsDir);
        while (!doc.empty() && (doc.back() == '\n' || doc.back() == '\r')) doc.pop_back();
        PostJson(W("{\"type\":\"plugin-session\",\"op\":\"get\",\"doc\":" + doc + "}"));
    } else if (c.op == "session-set") {
        /* [Sess] a session's chains for one unit: the unit's chains are replaced (running ones destroyed, the plain strips
           back), saved, and built by Restore() once its strips are Live, as after a restart; one that cannot go in waits
           or fails with its reason like any saved chain. The id is the unit's own while it is here, else the page's. */
        std::string id = g_present && !g_unitId.empty() ? g_unitId : c.unitId;
        std::vector<plughost::SavedChain> list;
        std::string md;
        if (id.empty() || !plughost::ChainsFromJson(c.text, list, md)) {
            PostResult("session", nullptr, 0, -1, false, "driver", id.empty() ? "no unit to put the chains on" : "the session's plug-in chains could not be read");
            return;
        }
        for (auto it = g_chains.begin(); it != g_chains.end();) {
            if (it->unit != id) { ++it; continue; }
            DestroyOn(*it);
            CloseHelpers(*it);
            it = g_chains.erase(it);
        }
        size_t n = 0;
        for (plughost::SavedChain &sc : list) {
            sc.unit = id;
            n += sc.slots.size();
            g_chains.push_back(FromSaved(sc));
        }
        Save();
        PostResult("session", nullptr, 0, -1, true, "", std::to_string(n) + (n == 1 ? " insert" : " inserts"));
        PostState(true);
    } else if (c.op == "dir") {
        g_mapsDir = c.dir;
        HelperStop();
        g_cat.clear();
        g_lastList.clear();
        LoadIndex();
        Save();
        if (g_listWanted) CatalogStart();
    }
}

static void ThreadMain()
{
    (void)CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);    /* ShellExecuteEx (elevation) */
    for (int k = 0; k < MAX_UNITS; k++) g_unitState[k].slot = k;
    Load();
    LoadIndex();
    while (!g_quit) {
        WaitForSingleObject(g_wake, g_catBuilding ? 0 : g_watch ? 1000 : !g_chains.empty() ? 2000 : 5000);
        if (g_quit) break;
        std::deque<Cmd> cmds;
        {
            std::lock_guard<std::mutex> g(g_cmdLock);
            cmds.swap(g_cmds);
        }
        /* the plug-in host is asked only when something here needs it: the view open, inserts saved, a command */
        bool caps = g_watch || !g_chains.empty() || !cmds.empty() || g_listWanted;
        Refresh(caps);
        /* a burst of knob moves: only the last value of each parameter is planned */
        for (size_t i = 0; i < cmds.size(); i++) {
            const Cmd &c = cmds[i];
            bool later = false;
            for (size_t k = i + 1; k < cmds.size() && c.op == "param"; k++) {
                later |= cmds[k].op == "param" && cmds[k].unit == c.unit && cmds[k].input == c.input && cmds[k].index == c.index;
            }
            if (!later) {
                Run(c);
                Refresh(true);
            }
        }
        Restore();
        CatalogStep();
        if (g_helperProc && GetTickCount64() - g_helperUsed > HELPER_IDLE_MS) HelperStop();
        for (UnitState &u : g_unitState) {
            /* [P7] each present unit's own licence answer, for its own picker */
            g_u = &u;
            if (g_listWanted && g_present && (g_licState == "unknown" || g_licEpoch != g_caps.Epoch) && CallerOk() &&
                GetTickCount64() - g_licAt > (g_licState == "failed" ? 30000u : 5000u)) Licence(false);
        }
        g_u = &g_unitState[0];
        PostStates();
        if (g_listWanted) PostLists(false);
    }
    HelperStop();
    CoUninitialize();
}

static void Queue(Cmd c)
{
    {
        std::lock_guard<std::mutex> g(g_cmdLock);
        g_cmds.push_back(std::move(c));
    }
    if (g_wake) SetEvent(g_wake);
}

/* {"cmd":"plug-...", ...} from the UI */
static void FromUi(const std::string &json)
{
    aggjson::Value v;
    aggjson::Parser p(json);
    if (!p.Parse(v)) return;
    std::string cmd = v.Str("cmd");
    if (cmd.compare(0, 5, "plug-") != 0) return;
    Cmd c;
    c.op = cmd.substr(5);
    plughost::U32(v.Get("unit"), c.unit);   /* [P7] absent = 0, the x8p */
    plughost::U32(v.Get("input"), c.input);
    plughost::U32(v.Get("index"), c.index);
    plughost::U32(v.Get("value"), c.value);
    const aggjson::Value *b = v.Get("on");
    c.on = b && b->kind == aggjson::Value::Bool && b->b;
    b = v.Get("stereo");
    c.stereo = b && b->kind == aggjson::Value::Bool && b->b;
    b = v.Get("units");
    if (b && b->kind == aggjson::Value::Bool && b->b) g_multi = true;
    c.name = v.Str("name");
    c.dir = v.Str("dir");
    /* [Ins] slot, aux, REC/MON, move, Unison */
    if (const aggjson::Value *sl = v.Get("slot")) {
        if (sl->kind == aggjson::Value::String && sl->s == "unison") { c.slot = kUnisonSlot; c.hasSlot = true; }
        else c.hasSlot = plughost::U32(sl, c.slot);
    }
    if (const aggjson::Value *ax = v.Get("aux")) {
        ULONG a = 0;
        if (ax->kind == aggjson::Value::Number && plughost::U32(ax, a) && a <= 1) c.aux = (LONG)a;
    }
    b = v.Get("mon");
    c.mon = b && b->kind == aggjson::Value::Bool && b->b;
    if (v.Str("mode") == "mon") c.mon = true;
    plughost::U32(v.Get("from"), c.from);
    plughost::U32(v.Get("to"), c.to);
    plughost::U32(v.Get("code"), c.code);
    c.db = (LONG)v.Num("db", 0);
    c.unitId = v.Str("unitId");             /* [Sess] */
    if (c.op == "session-set") c.text = json;
    if (const aggjson::Value *w = v.Get("words")) {
        for (const aggjson::Value &x : w->a) {
            ULONG t = 0;
            if (plughost::U32(&x, t)) c.words.push_back(t);
        }
    }
    Queue(c);
}

static void Start()
{
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_thread = std::thread(ThreadMain);
}

static void Stop()
{
    g_quit = true;
    if (g_wake) SetEvent(g_wake);
    if (g_thread.joinable()) g_thread.join();
    if (g_wake) CloseHandle(g_wake);
    g_wake = nullptr;
}

/* ------------------------------------------------------------------ the elevated step (--plugin-admin) */

static DWORD SetDacl(const std::wstring &path, const wchar_t *sddl)
{
    PSECURITY_DESCRIPTOR sd = nullptr;
    PACL dacl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr)) return GetLastError();
    DWORD err = ERROR_INVALID_SECURITY_DESCR;
    if (GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) && present) {
        err = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                    DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr);
    }
    LocalFree(sd);
    return err;
}

/* A record whose SHA-256 is its name and which the driver's Bill check takes. */
static bool RecordOk(const std::string &bytes, const plughost::Sha &want)
{
    plughost::Sha h{};
    PLUG_BILL b;
    return !bytes.empty() && plughost::Sha256(bytes.data(), bytes.size(), h) && h == want &&
           PlugBillCheck((const UCHAR *)bytes.data(), (ULONG)bytes.size(), &b) == PLUG_OK;
}

/*
 * apollo-control.exe --plugin-admin <request>: run elevated by Admin() above (UAC). Hosting on:
 * Parameters\PluginHost = 1 and PluginHostImage = this exe's own NT path (never one from the request);
 * off: PluginHost = 0, PluginHostImage removed. Modules: each named record copied from the maps'
 * store into System32\drivers\ApolloModules (System + Administrators only) after its SHA-256 and the
 * driver's Bill check; one already there and valid is left. Result in <request>.result.
 */
static int AdminMain(const std::wstring &reqPath)
{
    std::string t, detail;
    bool ok = true;
    ULONG installed = 0, present = 0, bad = 0;
    aggjson::Value req;
    if (!ReadAll(reqPath, t, 1u << 22) || !aggjson::Parser(t).Parse(req)) {
        detail = "no readable request";
        ok = false;
    }
    std::string hosting = ok ? req.Str("hosting") : std::string();
    const wchar_t *params = L"SYSTEM\\CurrentControlSet\\Services\\apollo\\Parameters";
    if (ok && (hosting == "on" || hosting == "off")) {
        HKEY k = nullptr;
        LSTATUS e = RegCreateKeyExW(HKEY_LOCAL_MACHINE, params, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr);
        if (e == ERROR_SUCCESS) {
            DWORD v = hosting == "on" ? 1 : 0;
            e = RegSetValueExW(k, APOLLO_PLUGIN_REG_ENABLE, 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
            if (e == ERROR_SUCCESS && hosting == "on") {
                /*
                 * PluginHostImage: this exe's DOS path ("C:\...", never a \Device\HarddiskVolumeN one: the
                 * driver resolves the drive letter at every call, review finding 2), from this process
                 * itself, never from the request. PluginHostUser: the requesting user's SID (the
                 * unelevated app's, in the request: an over-the-shoulder prompt runs this step as another
                 * administrator), else this process's own; the driver also matches the caller's token
                 * user (review finding 1).
                 */
                wchar_t img[1024];
                DWORD n = _countof(img);
                std::wstring user = W(req.Str("user"));
                if (!SidTextOk(user)) user = OwnUserSid();
                if (!QueryFullProcessImageNameW(GetCurrentProcess(), 0, img, &n) || n > APOLLO_PLUGIN_IMAGE_CCH_MAX ||
                    PlugImageDrive(img, n) == 0) {
                    e = ERROR_BAD_PATHNAME;
                    detail = "Apollo Control must run from a local drive (C:\\...) to host plug-ins";
                } else if (!SidTextOk(user) || user.size() > APOLLO_PLUGIN_USER_CCH_MAX) {
                    e = ERROR_INVALID_SID;
                } else {
                    e = RegSetValueExW(k, APOLLO_PLUGIN_REG_IMAGE, 0, REG_SZ, (const BYTE *)img, (n + 1) * sizeof(wchar_t));
                    if (e == ERROR_SUCCESS) {
                        e = RegSetValueExW(k, APOLLO_PLUGIN_REG_USER, 0, REG_SZ, (const BYTE *)user.c_str(),
                                           (DWORD)((user.size() + 1) * sizeof(wchar_t)));
                    }
                }
                if (e != ERROR_SUCCESS) {
                    /* never half a rule: no unelevated caller, hosting stays off */
                    DWORD off = 0;
                    RegSetValueExW(k, APOLLO_PLUGIN_REG_ENABLE, 0, REG_DWORD, (const BYTE *)&off, sizeof(off));
                    RegDeleteValueW(k, APOLLO_PLUGIN_REG_IMAGE);
                    RegDeleteValueW(k, APOLLO_PLUGIN_REG_USER);
                }
            } else if (e == ERROR_SUCCESS) {
                for (const wchar_t *name : { APOLLO_PLUGIN_REG_IMAGE, APOLLO_PLUGIN_REG_USER }) {
                    LSTATUS d = RegDeleteValueW(k, name);
                    if (d != ERROR_SUCCESS && d != ERROR_FILE_NOT_FOUND) e = d;
                }
            }
            RegCloseKey(k);
        }
        if (e != ERROR_SUCCESS) {
            ok = false;
            if (detail.empty()) detail = "Parameters could not be written: " + U8(ErrorText((DWORD)e));
        } else {
            detail = hosting == "on" ? "plug-in hosting on for this Apollo Control" : "plug-in hosting off";
        }
    }
    const aggjson::Value *mods = ok ? req.Get("modules") : nullptr;
    if (ok && mods && mods->kind == aggjson::Value::Array) {
        wchar_t win[MAX_PATH];
        UINT n = GetSystemWindowsDirectoryW(win, MAX_PATH);
        std::wstring dir = std::wstring(win, n) + L"\\" APOLLO_MODULE_DIR_UNDER_ROOT, from = W(req.Str("from"));
        if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            ok = false;
            detail = "the module store could not be created";
        } else if (SetDacl(dir, L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)") != ERROR_SUCCESS) {
            ok = false;
            detail = "the module store could not be made administrator-only";
        }
        for (size_t i = 0; ok && i < mods->a.size(); i++) {
            plughost::Sha want{};
            std::string sha = mods->a[i].s, bytes;
            if (!plughost::ParseShaHex(sha, want)) { bad++; continue; }
            std::wstring dst = dir + L"\\" + W(sha) + L".bill";
            if (ReadAll(dst, bytes, PLUG_BILL_MAX) && RecordOk(bytes, want)) { present++; continue; }
            if (!ReadAll(from + L"\\" + W(sha) + L".bill", bytes, PLUG_BILL_MAX) || !RecordOk(bytes, want)) { bad++; continue; }
            HANDLE f = CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD put = 0;
            bool w = f != INVALID_HANDLE_VALUE && WriteFile(f, bytes.data(), (DWORD)bytes.size(), &put, nullptr) && put == bytes.size();
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
            if (w) { (void)SetDacl(dst, L"D:P(A;;FA;;;SY)(A;;FA;;;BA)"); installed++; } else { bad++; }
        }
        if (ok) {
            detail += (detail.empty() ? "" : "; ") + std::to_string(installed) + " module(s) installed, " + std::to_string(present) + " already there" +
                      (bad ? ", " + std::to_string(bad) + " missing or not valid in the maps' store" : std::string());
        }
    }
    std::string res = std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"installed\":" + std::to_string(installed) +
                      ",\"present\":" + std::to_string(present) + ",\"bad\":" + std::to_string(bad) + ",\"detail\":" + Q(detail) + "}\n";
    (void)WriteAtomic(reqPath + L".result", res);
    return ok ? 0 : 1;
}

}   /* namespace pe */

#undef g_caps
#undef g_capsOk
#undef g_capsErr
#undef g_info
#undef g_infoBytes
#undef g_present
#undef g_unitId
#undef g_gen
#undef g_lic
#undef g_licState
#undef g_licWhy
#undef g_licEpoch
#undef g_licAt
#undef g_lastState
#undef g_lastList
