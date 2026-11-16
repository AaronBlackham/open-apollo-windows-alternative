/*
 * plugin_host.h - Apollo Control's pure side of UAD plug-in hosting (docs/PLUGINS.md 5): a mapped
 * runtime bundle -> the CREATE recipe apollo.sys takes (IOCTL 0x42, include/apollo_plugin.h), the
 * readback the startup is proven by, the catalogue of mapped plug-ins, the saved inserts.
 * Header-only C++20, no UI and no device code, so tools/plugin-recipe-test checks it offline
 * against tools/plugin-host/p1_recipe.py (open-apollo's own carrier code) on the same bundles.
 *
 * Ported from open-apollo 2.1.0 (GPL-2.0), mixer-engine/native_carrier.py:
 *   GeneratedOrdinaryCarrier.__init__/_build  rows, WRITEs, resources, scratch (BuildRecipe)
 *   write(), compact(), check(), blocks()/chunks(), resource()
 *   CacheWrites.observations()                the startup's readback runs (Observations)
 * and mixer-engine/native_profiled_carrier_provider.py: ProfiledCarrierProvider.observations()
 * (_filtered_observations: host-owned header words, dynamic refs, calibrations, 16-word runs) and
 * StereoProfiledCarrierProvider (channel counts 2 in rows 0, 1, 10; in2 = in1 + 1). The carrier's
 * record delay is upstream's (200 - latency, or 1000 - latency past 200) unless a fixed one is
 * asked for (p1_recipe.py's default 200, kept for the comparison test).
 *
 * Saved control state (a "diff": the words the accepted parameter changes left, from
 * tools/plugin-host/plughelper.py) is replayed after the fresh startup, as upstream's chain
 * composer keeps neighbours' controls across a rebuild ("fresh initializers precede the retained
 * accepted control state", native_chain_composer.py:270-274): no emulator at start-up.
 *
 * [Ins] Chains: mixer-engine/native_chain_composer.py compose() (up to four effects, UAD REC / MON, the Input Strip
 * carrier of an aux return), ComposedChain.record_tap_plan and plan_compensation at Medium (BuildChain, TapPlan,
 * ChainDelays); proven line for line against upstream's own compose() on synthetic bundles (golden_chain.h) and on this
 * PC's real bundles (tools/plugin-recipe-test/chain-compare.py). A Unison header chain (BuildUnisonChain) has no
 * upstream composer to compare with: structure and the driver's checks only. Saved chains: plugins.json version 2.
 *
 * Bundles, modules and recipes are derived from the user's own UA install: they stay on this PC
 * (apollo-win\plugins or %LOCALAPPDATA%\Avium\ApolloPlugins), never in the repo or the installer.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "..\..\include\apollo_ioctl.h"             /* (agg_json.h needs the AGG types) */
#include "..\..\include\apollo_agg.h"
#include "agg_json.h"                               /* aggjson::Value / Parser */
#include "..\..\include\apollo_strip_pins.h"        /* the 5 Mic strip modules by SHA-256 */
#pragma warning(push)
#pragma warning(disable : 4505)                     /* plugin_plan.h: helpers this file does not call */
#include "..\..\driver\plugin_plan.h"               /* the driver's own checks: rows, WRITEs, directory, pages */
#pragma warning(pop)

namespace plughost {

using Words = std::vector<ULONG>;
using Sha = std::array<UCHAR, 32>;

static const ULONG kNone = 0xFFFFFFFFu;
static const ULONG kPageWords = 1016;               /* MAX_COMMAND_WORDS, native_carrier.py:15 */
static const ULONG kPlainRecordDelay = 200;         /* strip_plan.h m0 word 16 = 0xc8 */
static const ULONG kScratchMin[3] = { 448, 192, 0 };/* native_carrier.py:17 */
static const ULONG kMicCarrier = 0x846;
static const ULONG kInputs = 8;                     /* the most strip inputs a unit has (x8p 8, Twin X 2: PLUG_MODEL Inputs) */
static const ULONG kSynchMono = 0x20;

struct Row { ULONG pool = 0, dwords = 0, link = 0, flags = 0; };
struct ResDesc { ULONG id = 0, arch = 0, kind = 0, words = 0; std::string sha; };
struct Module { ULONG id = 0, arch = 0, kind = 0, flags = 0, pool = 0, words = 0, bytes = 0; std::string sha; };
struct Param { std::string name; ULONG max = 0; std::vector<std::string> steps; };
struct Obs { ULONG row = 0, word = 0; Words v; };
using RW = std::pair<ULONG, ULONG>;

/* ------------------------------------------------------------------ small helpers */

static inline std::string Hex8(ULONG v)
{
    char b[16];
    snprintf(b, sizeof(b), "0x%08lx", v);
    return b;
}

static inline std::string ShaHex(const Sha &s)
{
    static const char hx[] = "0123456789abcdef";
    std::string o;
    for (UCHAR c : s) {
        o += hx[c >> 4];
        o += hx[c & 15];
    }
    return o;
}

static inline bool ParseShaHex(const std::string &t, Sha &out)
{
    auto hv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if (t.size() != 64) return false;
    for (size_t i = 0; i < 32; i++) {
        int h = hv(t[2 * i]), l = hv(t[2 * i + 1]);
        if (h < 0 || l < 0) return false;
        out[i] = (UCHAR)(h * 16 + l);
    }
    return true;
}

static inline bool Sha256(const void *data, size_t len, Sha &out)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (len > 0xFFFFFFFFu || BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return false;
    }
    bool ok = BCryptHash(alg, nullptr, 0, (PUCHAR)data, (ULONG)len, out.data(), 32) == 0;
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

/* A JSON number that is a whole uint32 (bundles hold them as plain integers). */
static inline bool U32(const aggjson::Value *v, ULONG &out)
{
    if (v == nullptr || v->kind != aggjson::Value::Number || v->n < 0 || v->n > 4294967295.0 || v->n != (double)(ULONGLONG)v->n) {
        return false;
    }
    out = (ULONG)(ULONGLONG)v->n;
    return true;
}

static inline bool U32Text(const std::string &s, ULONG &out)
{
    char *end = nullptr;
    if (s.empty()) return false;
    unsigned long long v = strtoull(s.c_str(), &end, 0);
    if (end == nullptr || *end != 0 || v > 0xFFFFFFFFull) return false;
    out = (ULONG)v;
    return true;
}

static inline std::string Lower(std::string s)
{
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

/* "UAD Precision De-Esser" -> "Precision De-Esser" (what the UI shows) */
static inline std::string Label(const std::string &name)
{
    return (name.size() > 4 && name.compare(0, 4, "UAD ") == 0) ? name.substr(4) : name;
}

/* oa_offline.slug: lower-case, "uad " dropped, runs of anything else -> "-" */
static inline std::string Slug(const std::string &name)
{
    std::string n = Lower(name), o;
    if (n.compare(0, 4, "uad ") == 0) n = n.substr(4);
    bool dash = false;
    for (char c : n) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (ok) {
            if (dash && !o.empty()) o += '-';
            dash = false;
            o += c;
        } else {
            dash = true;
        }
    }
    return o;
}

/* ------------------------------------------------------------------ bundle */

struct Bundle {
    std::string file, name, layout, status, dllSha, metaStem;
    ULONG renderId = 0, latency = 0, flags = 0, sampleRate = 48000, channels = 1, io[2] = { 1, 1 };
    std::vector<Row> rows;                          /* the descriptor's own rows (row 0 = chain row 10) */
    std::vector<ResDesc> resources;                 /* every architecture, descriptor order */
    ULONG scratch[3] = { 0, 0, 0 };
    std::map<ULONG, ULONG> shadow;                  /* startup words: row << 24 | word -> value */
    Words values;                                   /* integer parameter values at load */
    std::vector<std::string> texts;                 /* the plug-in's own texts at load */
    std::vector<Param> params;
    std::set<RW> dynRefs;                           /* chain (row, word) the running DSP owns */
    std::map<RW, std::pair<ULONG, ULONG>> calib;    /* hardware_observation_overrides */
    bool unison = false;                            /* [Ins] a Unison (integrated preamp) form: descriptor flag 0x40 or "unison": true */
};

static inline bool ParseBundle(const std::string &text, const std::string &file, Bundle &b, std::string &err)
{
    using aggjson::Value;
    Value root;
    aggjson::Parser p(text);
    b = Bundle{};
    b.file = file;
    if (!p.Parse(root) || root.kind != Value::Object) { err = "not a JSON bundle"; return false; }
    if (root.Str("schema") != "openapollo-profiled-runtime-bundle/1") { err = "not an open-apollo runtime bundle"; return false; }
    b.status = root.Str("status");
    if (b.status != "pass") { err = "the bundle did not pass the mapper"; return false; }
    b.name = root.Str("name");
    b.layout = root.Str("layout");
    if (b.name.empty() || (b.layout != "mono" && b.layout != "stereo")) { err = "bundle without a name or layout"; return false; }
    if (const Value *dll = root.Get("dll")) b.dllSha = dll->Str("sha256");
    const Value *d = root.Get("descriptor");
    if (d == nullptr || d->kind != Value::Object) { err = "no descriptor"; return false; }
    if (!U32(d->Get("render_id"), b.renderId) || !U32(d->Get("latency_samples"), b.latency)) { err = "descriptor without render id or latency"; return false; }
    if (const Value *f = d->Get("flags")) { if (!U32(f, b.flags)) { err = "descriptor flags"; return false; } }
    if (const Value *f = d->Get("sample_rate")) { if (!U32(f, b.sampleRate)) { err = "descriptor sample rate"; return false; } }
    if (const Value *f = d->Get("configured_input_channels")) { if (!U32(f, b.channels)) { err = "descriptor channels"; return false; } }
    if (const Value *io = d->Get("io")) {
        if (io->kind != Value::Array || io->a.size() != 2 || !U32(&io->a[0], b.io[0]) || !U32(&io->a[1], b.io[1])) { err = "descriptor io"; return false; }
    }
    const Value *rows = d->Get("rows");
    if (rows == nullptr || rows->kind != Value::Array) { err = "descriptor without rows"; return false; }
    for (const Value &r : rows->a) {
        Row x;
        if (r.kind != Value::Array || r.a.size() != 4 || !U32(&r.a[0], x.pool) || !U32(&r.a[1], x.dwords) ||
            !U32(&r.a[2], x.link) || !U32(&r.a[3], x.flags)) { err = "four-field private row required"; return false; }
        b.rows.push_back(x);
    }
    const Value *res = d->Get("resources");
    if (res == nullptr || res->kind != Value::Array) { err = "descriptor without resources"; return false; }
    for (const Value &r : res->a) {
        ResDesc x;
        if (r.kind != Value::Object || !U32(r.Get("id"), x.id) || !U32(r.Get("architecture"), x.arch)) { err = "descriptor resource"; return false; }
        U32(r.Get("kind"), x.kind);
        U32(r.Get("words"), x.words);
        x.sha = r.Str("sha256");
        b.resources.push_back(x);
    }
    const Value *sc = d->Get("scratch");
    if (sc == nullptr || sc->kind != Value::Array || sc->a.size() != 3 || !U32(&sc->a[0], b.scratch[0]) ||
        !U32(&sc->a[1], b.scratch[1]) || !U32(&sc->a[2], b.scratch[2])) { err = "descriptor scratch"; return false; }
    const Value *st = root.Get("startup");
    const Value *sh = st ? st->Get("shadow") : nullptr;
    if (sh == nullptr || sh->kind != Value::Object) { err = "no startup words"; return false; }
    for (const auto &kv : sh->o) {
        ULONG k = 0, v = 0;
        if (!U32Text(kv.first, k) || !U32(&kv.second, v)) { err = "startup word not a uint32"; return false; }
        b.shadow[k] = v;
    }
    if (const Value *iv = st->Get("integer_values")) {
        for (const Value &x : iv->a) { ULONG v = 0; U32(&x, v); b.values.push_back(v); }
    }
    if (const Value *fm = st->Get("formatted")) {
        for (const Value &x : fm->a) b.texts.push_back(x.Str("console_trimmed"));
    }
    const Value *mx = root.Get("parameter_maxima");
    if (mx == nullptr || mx->kind != Value::Array) { err = "no parameter maxima"; return false; }
    for (const Value &x : mx->a) {
        Param pr;
        if (!U32(&x, pr.max)) { err = "parameter maximum"; return false; }
        b.params.push_back(pr);
    }
    if (const Value *meta = root.Get("parameter_metadata")) {
        if (const Value *ctl = meta->Get("controls")) {
            for (const Value &c : ctl->a) {
                std::string path = c.Str("path");
                size_t at = path.find("/parameters/");
                if (at == std::string::npos) continue;
                size_t s = at + 12, e = path.find('/', s);
                if (e == std::string::npos) continue;
                ULONG idx = 0;
                if (!U32Text(path.substr(s, e - s), idx) || idx >= b.params.size()) continue;
                std::string field = path.substr(e + 1);
                if (field == "Name") {
                    b.params[idx].name = c.Str("value");
                } else if (field == "StepValue") {
                    if (const Value *vs = c.Get("values")) {
                        for (const Value &x : vs->a) b.params[idx].steps.push_back(x.s);
                    }
                }
            }
        }
    }
    for (size_t i = 0; i < b.params.size(); i++) {
        if (b.params[i].name.empty()) b.params[i].name = "Parameter " + std::to_string(i + 1);
    }
    if (b.values.size() != b.params.size()) b.values.assign(b.params.size(), 0);
    if (b.texts.size() != b.params.size()) b.texts.assign(b.params.size(), std::string());
    if (const Value *dr = root.Get("dynamic_observation_refs")) {
        for (const Value &x : dr->a) {
            ULONG r = 0, w = 0;
            if (x.kind == Value::Array && x.a.size() == 2 && U32(&x.a[0], r) && U32(&x.a[1], w) && r <= 0xFF && w <= 0xFFFFFF) {
                b.dynRefs.insert({ r, w });
            } else { err = "dynamic observation ref"; return false; }
        }
    }
    if (const Value *ev = root.Get("hardware_dynamic_observation_evidence")) {
        for (const Value &x : ev->a) {
            ULONG r = 0, w = 0;
            if (!U32(x.Get("row"), r) || !U32(x.Get("offset"), w) || r > 0xFF || w > 0xFFFFFF) { err = "dynamic observation evidence"; return false; }
            b.dynRefs.insert({ r, w });
        }
    }
    if (const Value *ov = root.Get("hardware_observation_overrides")) {
        for (const Value &x : ov->a) {
            ULONG r = 0, w = 0;
            const Value *sv = x.Get("source_values"), *obv = x.Get("observed_values");
            if (!U32(x.Get("row"), r) || !U32(x.Get("offset"), w) || sv == nullptr || obv == nullptr || sv->a.size() != obv->a.size()) {
                err = "hardware observation calibration"; return false;
            }
            for (size_t i = 0; i < sv->a.size(); i++) {
                ULONG a = 0, o = 0;
                if (!U32(&sv->a[i], a) || !U32(&obv->a[i], o)) { err = "hardware observation calibration"; return false; }
                b.calib[{ r, w + (ULONG)i }] = { a, o };
            }
        }
    }
    if (const Value *un = root.Get("unison")) b.unison = un->kind == Value::Bool && un->b;
    if (b.flags & 0x40) b.unison = true;
    if (const Value *prof = root.Get("profile")) {
        if (const Value *cm = prof->Get("captured_metadata")) {
            std::string m = cm->Str("path");
            size_t sl = m.find_last_of("\\/");
            std::string stem = (sl == std::string::npos) ? m : m.substr(sl + 1);
            if (stem.size() > 5 && Lower(stem.substr(stem.size() - 5)) == ".json") stem.resize(stem.size() - 5);
            b.metaStem = stem;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ carrier (native_carrier.py) */

static inline ULONG L(ULONG r, ULONG w) { return (r << 24) | w; }

/* write(): one masked WRITE of values {word: value}, span <= 33 words */
static inline bool Write(Words &out, ULONG row, const std::map<ULONG, ULONG> &values)
{
    if (values.empty()) return false;
    ULONG first = values.begin()->first, last = values.rbegin()->first;
    if (last - first > 32) return false;
    ULONG mask = 0;
    for (ULONG p = first + 1; p <= last; p++) {
        if (!values.count(p)) mask |= 1u << (p - first - 1);
    }
    out.push_back(0x00140000u | (3 + (ULONG)values.size()));
    out.push_back(L(row, first));
    out.push_back(mask);
    for (const auto &kv : values) out.push_back(kv.second);
    return true;
}

/* compact(words, base=10): per row ascending, each group from the lowest word up to first + 32 */
static inline void Compact(Words &out, const std::map<ULONG, ULONG> &words, ULONG base = 10)
{
    std::map<ULONG, std::vector<std::pair<ULONG, ULONG>>> rows;
    for (const auto &kv : words) rows[kv.first >> 24].push_back({ kv.first & 0xFFFFFFu, kv.second });
    for (auto &r : rows) {
        auto &items = r.second;
        std::sort(items.begin(), items.end());
        size_t i = 0;
        while (i < items.size()) {
            ULONG first = items[i].first;
            size_t j = i;
            std::map<ULONG, ULONG> grp;
            while (j < items.size() && items[j].first <= first + 32) {
                grp[items[j].first] = items[j].second;
                j++;
            }
            Write(out, base + r.first, grp);
            i = j;
        }
    }
}

/* check(): the driver's ua_inst_check_cmds, offline (native_carrier.py check) */
static inline bool Check(const std::vector<Row> &rows, const Words &cmd, std::string &err)
{
    std::set<RW> links;
    for (size_t k = 1; k < rows.size(); k++) links.insert({ rows[k].link >> 24, rows[k].link & 0xFFFFFFu });
    size_t i = 0;
    while (i < cmd.size()) {
        if (i + 3 > cmd.size()) { err = "bad WRITE header"; return false; }
        ULONG ln = cmd[i] & 0xFFFF, row = cmd[i + 1] >> 24, word = cmd[i + 1] & 0xFFFFFF, mask = cmd[i + 2];
        if ((cmd[i] >> 16) != 0x14 || ln < 4 || ln > cmd.size() - i) { err = "bad WRITE header at " + std::to_string(i); return false; }
        if (row >= rows.size() || (ULONGLONG)word + ln - 3 + PlugPopcount(mask) > rows[row].dwords) {
            err = "WRITE past row " + std::to_string(row); return false;
        }
        ULONG pos = word;
        for (ULONG k = 0; k < ln - 3; k++) {
            while (pos > word && pos - word - 1 < 32 && ((mask >> (pos - word - 1)) & 1)) pos++;
            if (links.count({ row, pos })) { err = "value on linked word " + std::to_string(pos) + " of row " + std::to_string(row); return false; }
            pos++;
        }
        i += ln;
    }
    return true;
}

/* blocks()/chunks() over single WRITE blocks: whole blocks, as many as fit one page */
static inline bool Pages(const Words &cmd, std::vector<std::pair<ULONG, ULONG>> &pages)
{
    pages.clear();
    ULONG at = 0, len;
    while ((len = PlugChunkNext(cmd.data(), (ULONG)cmd.size(), at, kPageWords)) != 0) {
        if (len == PLUG_NONE) return false;
        pages.push_back({ at, len });
        at += len;
    }
    return !pages.empty() && at == cmd.size();
}

/* The Mic Input Strip startup WRITEs of a chain (native_carrier.py MIC_STRIP) */
static inline void MicStrip(Words &out)
{
    static const ULONG b0[] = { 0x40000000, 1, 1, 0x800, 0x3a888889, 0xc0000000, 0xc0000000, 0x10, 0x4b,
                                0x41000000, 0x3ada740e, kNone, 1, kNone, 1 };
    static const ULONG b1[] = { 0x3cae6c74, 0xbf7ff125, 0x3f7ff892, 0x3909422f, 0x3f800000, 0xbc2e6fd3, 0xbbc96245,
                                0x3cae6c74, 0xbf7ff125, 0x3f7ff892, 0x3909422f, 0x3f800000, 0xbc2e6fd3, 0xbbc96245, 1 };
    static const ULONG b2[] = { 0x3cae6c74, 0xbf7ff125, 0x3f7ff892, 0x3909422f, 0x3f800000, 0xbc2e6fd3, 0xbbc96245,
                                0x3e3b3f3d, 0xbf7baf04, 0x3f7dd5f1, 0x3c20d8d2, 0x3f7ffcd7, 0xbdbc4dc9, 0xbd565d96 };
    static const ULONG b3[] = { 0x3e8 };
    struct B { ULONG row, word, mask; const ULONG *v; size_t n; };
    static const B blocks[] = {
        { 0, 0x00, 0x0f1fffec, b0, _countof(b0) }, { 0, 0x26, 0x00000040, b1, _countof(b1) },
        { 3, 0x01, 0x00000040, b2, _countof(b2) }, { 0, 0x10, 0x00000000, b3, _countof(b3) },
    };
    for (const B &b : blocks) {
        out.push_back(0x00140000u | (ULONG)(b.n + 3));
        out.push_back(L(1 + b.row, b.word));
        out.push_back(b.mask);
        out.insert(out.end(), b.v, b.v + b.n);
    }
}

/* head_rows(): the chain head (row 0) and the Mic Input Strip (rows 1-9) */
static inline std::vector<Row> HeadRows()
{
    return { { 1, 10, kNone, 0 }, { 1, 56, L(0, 6), 0 }, { 2, 16, L(1, 54), 2 }, { 1, 16, L(1, 55), 2 },
             { 3, 16, L(1, 27), 0 }, { 1, 4, L(1, 28), 0 }, { 3, 2048, L(1, 14), 2 }, { 3, 2048, L(1, 15), 2 },
             { 3, 2048, L(1, 6), 2 }, { 3, 2048, L(1, 7), 2 } };
}

/* resource(): a module record from the store, checked as upstream's carrier checks it */
using ModuleReader = std::function<bool(const std::string &sha, std::vector<UCHAR> &bytes)>;

static inline bool CheckModule(const ResDesc &r, const std::vector<UCHAR> &payload, Module &m, std::string &err)
{
    Sha h{}, want{};
    if (!ParseShaHex(r.sha, want)) { err = "resource content hash required"; return false; }
    if (!Sha256(payload.data(), payload.size(), h) || h != want) { err = "resource store hash mismatch (" + r.sha.substr(0, 16) + ")"; return false; }
    if (payload.size() < 56 || payload.size() % 4) { err = "bounded complete Bill record required"; return false; }
    auto le = [&](size_t o) { return (ULONG)payload[o] | ((ULONG)payload[o + 1] << 8) | ((ULONG)payload[o + 2] << 16) | ((ULONG)payload[o + 3] << 24); };
    ULONG magic = le(0), ident = le(4), w2 = le(8), size = le(12), words = le(16);
    ULONG kind = w2 & 255, flags = (w2 >> 8) & 0xFFFF, arch = w2 >> 24;
    if (magic != 0x6c6c6942 || ident > 0xFFFFFF || kind > 4 || (arch != 0 && arch != 2) || size != payload.size() - 20 ||
        words == 0 || words > payload.size() / 4 - 13 || (kind == 0 && flags) || (kind == 3 && flags)) {
        err = "resource is outside qualified original loader contract"; return false;
    }
    ULONG alloc = kind ? words : (ULONG)((ULONGLONG)words * 2 / 3);
    if (alloc == 0 || (kind == 0 && (alloc & 1))) { err = "invalid original code allocation"; return false; }
    if (r.id != ident || r.arch != arch) { err = "descriptor resource identity differs from Bill"; return false; }
    PLUG_BILL pb;
    if (PlugBillCheck(payload.data(), (ULONG)payload.size(), &pb) != PLUG_OK) { err = "the driver's Bill check refuses it"; return false; }
    m = Module{ ident, arch, kind, flags, kind == 4 ? 1u : kind, alloc, (ULONG)payload.size(), r.sha };
    return true;
}

/* ------------------------------------------------------------------ recipe */

struct RecipeOptions {
    const PLUG_MODEL *unit = nullptr;               /* [P7] the unit's plan (plugin_plan.h PlugModelFor); null = the x8p */
    ULONG input = 0;                                /* analog input index (0 = input 1; the pair's first for stereo) */
    bool stereo = false;
    bool fixedDelay = false;                        /* true: delay below (p1_recipe.py's 200); false: the carrier's */
    ULONG delay = 200;
    bool verifyModules = true;                      /* false: the catalogue (descriptor fields only) */
    const std::map<ULONG, ULONG> *diff = nullptr;   /* saved control words, replayed after the startup */
    std::string specsDir;                           /* plugins\specs (meter words), optional */
    std::string ownedList;                          /* plugins\p1\dsp-owned.txt, optional */
    bool unisonHeader = false;                      /* [Ins] build a Unison form's member recipe (BuildUnisonChain only) */
};

struct Owned { ULONG row, word; std::string src; };

struct Recipe {
    std::string name, layout, bundle, dllSha, effect;
    ULONG renderId = 0, latency = 0, recordDelay = 0, carrierDelay = 0, in1 = 0, in2 = kSynchMono;
    LONG expectShift = 0;
    ULONG scratch[3] = { 0, 0, 0 };
    std::vector<Sha> res;                           /* chain resources: 5 strip pins + the plug-in's */
    std::vector<Row> rows;                          /* chain rows */
    Words cmd;                                      /* the whole startup */
    std::vector<std::pair<ULONG, ULONG>> pages;     /* (at, words) */
    std::vector<Module> modules;                    /* the plug-in's own (architectures 0 and 2) */
    std::vector<Row> localRows;
    std::vector<Obs> obs;                           /* readback runs */
    std::vector<Owned> owned;                       /* DSP-owned words: never compared */
    std::map<ULONG, ULONG> initialized;             /* the plug-in's words as the startup leaves them */
    ULONG codeWords = 0, dataWords = 0;             /* the plug-in's own pool words */
    bool staged() const { return pages.size() > 1; }
};

/* A whole file; the path is UTF-8 (opened wide, so any profile path works). */
static inline bool FileText(const std::string &path, std::string &out)
{
    FILE *f = nullptr;
    out.clear();
    int cch = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), nullptr, 0);
    std::wstring w((size_t)std::max(cch, 0), L'\0');
    if (cch <= 0 || MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), w.data(), cch) != cch) return false;
    if (_wfopen_s(&f, w.c_str(), L"rb") != 0 || f == nullptr) return false;
    std::vector<char> buf(65536);
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) out.append(buf.data(), n);
    fclose(f);
    return true;
}

/* tail for reverbs and echoes by name, else delay when there is latency, else level (p1_recipe.py) */
static inline std::string EffectMode(const std::string &name, ULONG latency)
{
    static const char *tail[] = { "480l", "rmx16", "plate", "verb", "reverb", "room", "hall", "chamber", "spring",
                                  "echo", "delay", "dmx", "ocean way", "capitol", "cooper time" };
    std::string n = Lower(name);
    for (const char *t : tail) {
        if (n.find(t) != std::string::npos) return "tail";
    }
    return latency > 0 ? "delay" : "level";
}

/* The running DSP's own words: the bundle's dynamic refs, the panel spec's meters, the hardware list. */
static inline void OwnedWords(const Bundle &b, const RecipeOptions &o, std::vector<Owned> &out)
{
    std::map<RW, std::string> found;
    for (const RW &rw : b.dynRefs) found.emplace(rw, "bundle");
    if (!o.specsDir.empty() && !b.metaStem.empty()) {
        std::string t;
        if (FileText(o.specsDir + "\\PluginSpec_" + b.metaStem + ".json", t)) {
            aggjson::Value root;
            aggjson::Parser p(t);
            if (p.Parse(root)) {
                if (const aggjson::Value *ms = root.Get("meterSources")) {
                    for (const auto &kv : ms->o) {
                        ULONG ref = 0;
                        const aggjson::Value *v = kv.second.Get("vendorSpecificFloat");
                        if (v != nullptr && U32(v, ref)) {
                            ULONG r = ref >> 24, w = ref & 0xFFFFFF;
                            if (r < b.rows.size() && w < b.rows[r].dwords) found.emplace(RW{ 10 + r, w }, "meter");
                        }
                    }
                }
            }
        }
    }
    if (!o.ownedList.empty()) {
        std::string t;
        if (FileText(o.ownedList, t)) {
            size_t at = 0;
            while (at < t.size()) {
                size_t e = t.find('\n', at);
                std::string line = t.substr(at, e == std::string::npos ? std::string::npos : e - at);
                at = (e == std::string::npos) ? t.size() : e + 1;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                size_t a = line.find('|');
                if (line.empty() || line.find_first_not_of(' ') == std::string::npos || line[line.find_first_not_of(' ')] == '#' ||
                    a == std::string::npos) continue;
                auto trim = [](std::string s) {
                    size_t f = s.find_first_not_of(" \t"), l = s.find_last_not_of(" \t");
                    return f == std::string::npos ? std::string() : s.substr(f, l - f + 1);
                };
                size_t c = line.find('|', a + 1);
                std::string nm = trim(line.substr(0, a)), rw = trim(line.substr(a + 1, c == std::string::npos ? std::string::npos : c - a - 1));
                ULONG r = 0, w = 0;
                if (nm != b.name || sscanf_s(rw.c_str(), "%lu %lu", &r, &w) != 2) continue;
                found.emplace(RW{ r, w }, "hardware");
            }
        }
    }
    out.clear();
    for (const auto &kv : found) out.push_back({ kv.first.first, kv.first.second, kv.second });
}

/*
 * CacheWrites.observations(words, words) then ProfiledCarrierProvider._filtered_observations():
 * runs of at most 64 (codec) then of 16 words, the host-owned header words (row 10 words 0,1,2,4;
 * rows >= 11 words 0,1,2,6), 0xFFFFFFFF and the dynamic refs left out; a calibration replaces a
 * recorded value by what the hardware shows. Over 32000 runs: 64-word runs.
 */
static inline void Observations(const Bundle &b, const std::map<ULONG, ULONG> &words, std::vector<Obs> &out)
{
    std::vector<Obs> src;
    for (const auto &kv : words) {
        ULONG row = 10 + (kv.first >> 24), off = kv.first & 0xFFFFFF;
        if (!src.empty() && src.back().row == row && src.back().word + src.back().v.size() == off && src.back().v.size() < 64) {
            src.back().v.push_back(kv.second);
        } else {
            src.push_back({ row, off, { kv.second } });
        }
    }
    auto filtered = [&](size_t width) {
        std::vector<Obs> o;
        for (const Obs &s : src) {
            Obs pend{ s.row, 0, {} };
            ULONG lastWord = 0;
            auto flush = [&]() { if (!pend.v.empty()) { o.push_back(pend); pend.v.clear(); } };
            for (size_t i = 0; i < s.v.size(); i++) {
                ULONG word = s.word + (ULONG)i, value = s.v[i];
                auto c = b.calib.find({ s.row, word });
                if (c != b.calib.end() && value == c->second.first) value = c->second.second;
                if ((s.row == 10 && (word == 0 || word == 1 || word == 2 || word == 4)) || value == kNone ||
                    (s.row >= 11 && (word == 0 || word == 1 || word == 2 || word == 6)) || b.dynRefs.count({ s.row, word })) {
                    flush();
                    continue;
                }
                if (!pend.v.empty() && word != lastWord + 1) flush();
                if (pend.v.empty()) pend.word = word;
                pend.v.push_back(value);
                lastWord = word;
                if (pend.v.size() == width) flush();
            }
            flush();
        }
        return o;
    };
    out = filtered(16);
    if (out.size() > 32000) out = filtered(64);
}

static inline bool BuildRecipe(const Bundle &b, const ModuleReader &read, const RecipeOptions &o, Recipe &r, std::string &err)
{
    r = Recipe{};
    r.name = b.name;
    r.layout = b.layout;
    r.bundle = b.file;
    r.dllSha = b.dllSha;
    /* GeneratedOrdinaryCarrier.__init__ */
    if (o.stereo != (b.layout == "stereo")) { err = "the bundle's layout is " + b.layout; return false; }
    /* [P7] the unit's input -> DSP input (FirstSynch + input: the same on the x8p and the Twin X), its strip inputs and pairs */
    const PLUG_MODEL *unit = o.unit ? o.unit : PlugModelFor(PLUG_MODEL_X8P);
    if (unit == nullptr || o.input >= kInputs) { err = "no such input or pair"; return false; }
    const ULONG in1 = unit->FirstSynch + o.input, in2 = o.stereo ? in1 + 1 : kSynchMono;
    if (PlugModelSynch(unit, in1 | (in2 << 16)) != PLUG_OK) { err = "no such input or pair"; return false; }
    if (b.sampleRate != 48000 || !(b.channels == 1 || (b.channels == 2 && b.io[0] == 2 && b.io[1] == 2))) { err = "this proven carrier supports mono48k only"; return false; }
    if (!((b.io[0] == 1 && b.io[1] == 1) || (b.io[0] == 2 && b.io[1] == 2))) { err = "descriptor IO capabilities differ from proven mono normalization"; return false; }
    if (o.stereo && !(b.io[0] == 2 && b.io[1] == 2)) { err = "original 2/2 descriptor required for stereo wrapper"; return false; }
    if (b.renderId == 0 || b.renderId > 0xFFFFFF || ((b.flags & 0x40) && !o.unisonHeader)) { err = "ordinary licensed descriptor required"; return false; }
    ULONG budget = b.latency <= 200 ? 200 : 1000;   /* ProfiledCarrierProvider: Medium / Long */
    bool overload = b.latency > 1000;
    if (!(b.latency <= budget || overload)) { err = "known latency must fit explicit compensation budget"; return false; }
    r.renderId = b.renderId;
    r.latency = b.latency;
    r.carrierDelay = budget > b.latency ? budget - b.latency : 0;
    if (b.rows.empty() || b.rows.size() > 118) { err = "effect rows must fit CREATE128 with carrier10"; return false; }
    std::set<ULONG> pointers;
    for (size_t i = 0; i < b.rows.size(); i++) {
        const Row &x = b.rows[i];
        if (x.pool < 1 || x.pool > 3 || x.dwords == 0 || x.dwords >= (1u << 24) || (x.flags != 0 && x.flags != 2)) { err = "unsupported private allocation"; return false; }
        if (i == 0) {
            if (x.link != kNone || x.dwords < 5) { err = "ordinary original root row required"; return false; }
        } else if (x.link == kNone || (x.link >> 24) >= b.rows.size() || (x.link & 0xFFFFFF) >= b.rows[x.link >> 24].dwords) {
            err = "relocation target leaves descriptor"; return false;
        }
        if (i) pointers.insert(x.link);
    }
    r.localRows = b.rows;
    auto address = [&](ULONG ref, bool control) -> bool {
        ULONG row = ref >> 24, off = ref & 0xFFFFFF;
        if (row >= b.rows.size() || off >= b.rows[row].dwords) { err = "original reference leaves owned descriptor"; return false; }
        if (pointers.count(ref)) { err = "control write targets DSP-owned readback"; return false; }
        if (control && row == 0 && (off == 0 || off == 1 || off == 2 || off == 4)) { err = "control write collides with host-owned header"; return false; }
        return true;
    };
    for (const auto &kv : b.shadow) {
        if (!address(kv.first, false)) return false;
    }
    if (o.diff) {
        for (const auto &kv : *o.diff) {
            if (!address(kv.first, true)) return false;
        }
    }
    /* resources: architectures 0 and 2, checked from the store (or, for the catalogue, as the descriptor says) */
    std::set<std::pair<ULONG, ULONG>> ids;
    for (const ResDesc &d : b.resources) {
        if (d.arch != 0 && d.arch != 2) continue;
        Module m;
        if (o.verifyModules) {
            std::vector<UCHAR> bytes;
            if (!read || !read(d.sha, bytes)) { err = "module " + d.sha.substr(0, 16) + " is not in the module store"; return false; }
            if (!CheckModule(d, bytes, m, err)) return false;
        } else {
            m = Module{ d.id, d.arch, d.kind, 0, d.kind == 4 ? 1u : d.kind, d.kind ? d.words : (ULONG)((ULONGLONG)d.words * 2 / 3), 0, d.sha };
        }
        if (!ids.insert({ m.id, m.arch }).second) { err = "duplicate original resource identity"; return false; }
        r.modules.push_back(m);
        (m.pool == 0 ? r.codeWords : r.dataWords) += m.words;
    }
    if (b.scratch[0] > 65535 || b.scratch[1] > 65535) { err = "descriptor scratch exceeds CREATE ABI"; return false; }
    for (int i = 0; i < 3; i++) r.scratch[i] = std::max(kScratchMin[i], b.scratch[i]);
    r.recordDelay = o.fixedDelay ? o.delay : r.carrierDelay;
    if (r.recordDelay > 2000) { err = "record delay over the chain strip's 2048 samples"; return false; }
    /* _build */
    std::map<ULONG, ULONG> head{ { 0, 0x40000000 }, { 1, 1 }, { 2, 1 }, { 4, 0x22 }, { 5, 2 }, { 7, kMicCarrier }, { 9, b.renderId } };
    Write(r.cmd, 0, head);
    MicStrip(r.cmd);
    Write(r.cmd, 1, { { 16, r.recordDelay } });
    r.rows = HeadRows();
    for (size_t i = 0; i < b.rows.size(); i++) {
        const Row &x = b.rows[i];
        r.rows.push_back({ x.pool, x.dwords, i == 0 ? L(0, 8) : L(10 + (x.link >> 24), x.link & 0xFFFFFF), x.flags });
    }
    std::map<ULONG, ULONG> words = b.shadow;
    ULONG w4 = words.count(4) ? words[4] : 0;
    words[0] = 0x40000000;
    words[1] = 1;
    words[2] = 1;
    words[4] = w4 | 4;
    Compact(r.cmd, words);
    if (o.diff && !o.diff->empty()) {
        Compact(r.cmd, *o.diff);                    /* the accepted control state after the fresh startup */
        for (const auto &kv : *o.diff) words[kv.first] = kv.second;
    }
    r.initialized = words;
    if (!Check(r.rows, r.cmd, err)) return false;
    /* resources: the 5 pinned strip modules, then the plug-in's not already listed */
    for (int k = 0; k < APOLLO_STRIP_MODULES; k++) {
        Sha s;
        memcpy(s.data(), g_ApolloStripPins[k].Sha256, 32);
        r.res.push_back(s);
    }
    for (const Module &m : r.modules) {
        Sha s{};
        if (!ParseShaHex(m.sha, s)) { err = "resource content hash required"; return false; }
        if (std::find(r.res.begin(), r.res.end(), s) == r.res.end()) r.res.push_back(s);
    }
    if (r.res.size() > 64) { err = "combined carrier resources exceed CREATE64"; return false; }
    /* stereo wrapper */
    if (o.stereo) {
        for (ULONG row : { 0u, 1u, 10u }) Write(r.cmd, row, { { 1, 2 }, { 2, 2 } });
        if (!Check(r.rows, r.cmd, err)) return false;
        r.in1 = in1;
        r.in2 = in2;
    } else {
        r.in1 = in1;
        r.in2 = kSynchMono;
    }
    if (!Pages(r.cmd, r.pages)) { err = "the startup cannot be cut into pages of whole WRITE blocks"; return false; }
    /* the driver's own checks, as it will run them */
    {
        std::vector<PLUG_ROW> pr;
        for (const Row &x : r.rows) pr.push_back({ x.pool, x.dwords, x.link, x.flags });
        std::vector<ULONG> s2(2 * pr.size() + 2), s1(pr.size() + 1);
        ULONG n = 0, ids5[5], nids = 0, carrier = 0;
        if (pr.size() > PLUG_PRIV_MAX || PlugCheckRows(pr.data(), (ULONG)pr.size(), s2.data(), s1.data()) != PLUG_OK) { err = "the driver refuses the chain rows"; return false; }
        for (const auto &pg : r.pages) {
            if (PlugCheckCmds(pr.data(), (ULONG)pr.size(), r.cmd.data() + pg.first, pg.second, &n) != PLUG_OK) { err = "the driver refuses a startup page"; return false; }
            if (&pg != &r.pages[0] && PlugWritesDirectory(r.cmd.data() + pg.first, pg.second)) { err = "a later startup page writes the chain directory"; return false; }
        }
        if (PlugChainIds(pr.data(), (ULONG)pr.size(), r.cmd.data(), r.pages[0].second, 1, b.renderId, ids5, &nids, &carrier) != PLUG_OK ||
            nids != 1 || ids5[0] != b.renderId || carrier != kMicCarrier) { err = "the chain directory is not Mic carrier + this plug-in"; return false; }
    }
    /* readback */
    Observations(b, words, r.obs);
    if (o.stereo) {
        for (ULONG row : { 0u, 1u, 10u }) r.obs.push_back({ row, 1, { 2, 2 } });
    }
    if (r.obs.empty() || r.obs.size() > 32768) { err = "bounded nonempty observation set required"; return false; }
    OwnedWords(b, o, r.owned);
    r.effect = EffectMode(b.name, b.latency);
    r.expectShift = (LONG)r.recordDelay + (LONG)b.latency - (LONG)kPlainRecordDelay;
    return true;
}

/* The recipe in p1_recipe.py's text form (tools/plugin-recipe-test compares the two line by line). */
static inline std::string RecipeText(const Recipe &r)
{
    std::string t;
    char b[256];
    auto line = [&](const std::string &s) { t += s; t += '\n'; };
    line("apollo-plug-recipe 1");
    line("# Derived from UA files on this PC: keep local (never the repo, the installer or a log).");
    line("name " + r.name);
    line("layout " + r.layout);
    line("bundle " + r.bundle);
    line("dll_sha256 " + r.dllSha);
    snprintf(b, sizeof(b), "render_id 0x%lx", r.renderId); line(b);
    line("latency " + std::to_string(r.latency));
    line("record_delay " + std::to_string(r.recordDelay));
    snprintf(b, sizeof(b), "# record path: chain %lu + %lu against the plain strip's %lu", r.recordDelay, r.latency, kPlainRecordDelay); line(b);
    line("expect_shift " + std::to_string(r.expectShift));
    line("effect " + r.effect);
    snprintf(b, sizeof(b), "synch %lu %lu", r.in1, r.in2); line(b);
    snprintf(b, sizeof(b), "scratch %lu %lu %lu", r.scratch[0], r.scratch[1], r.scratch[2]); line(b);
    if (r.staged()) {
        snprintf(b, sizeof(b), "# startup %zu words in %zu pages: page 0 with CREATE STAGE+CHAIN, the rest by SERVICE, then START "
                               "(upstream chunks(blocks()))", r.cmd.size(), r.pages.size());
        line(b);
    }
    for (const auto &p : r.pages) { snprintf(b, sizeof(b), "chunk %lu %lu", p.first, p.second); line(b); }
    line("# chain: Mic Input Strip carrier (rows 0-9, the 5 pinned strip modules) + the plug-in");
    for (const Sha &s : r.res) line("resource " + ShaHex(s));
    for (const Row &x : r.rows) { snprintf(b, sizeof(b), "row %lu %lu 0x%08lx %lu", x.pool, x.dwords, x.link, x.flags); line(b); }
    for (size_t i = 0; i < r.cmd.size(); i += 12) {
        std::string s = "cmd";
        for (size_t k = i; k < r.cmd.size() && k < i + 12; k++) s += " " + Hex8(r.cmd[k]);
        line(s);
    }
    line("# unrouted: the plug-in alone (its own modules, architectures 0 and 2, and its descriptor rows)");
    for (const Module &m : r.modules) line("plain_resource " + m.sha);
    for (const Row &x : r.localRows) { snprintf(b, sizeof(b), "plain_row %lu %lu 0x%08lx %lu", x.pool, x.dwords, x.link, x.flags); line(b); }
    for (const Module &m : r.modules) {
        snprintf(b, sizeof(b), " 0x%03lx kind %lu arch %lu pool %lu words %lu bytes %lu", m.id, m.kind, m.arch, m.pool, m.words, m.bytes);
        line("module " + m.sha + b);
    }
    for (const Obs &x : r.obs) {
        std::string s = "observe " + std::to_string(x.row) + " " + std::to_string(x.word) + " " + std::to_string(x.v.size());
        for (ULONG v : x.v) s += " " + Hex8(v);
        line(s);
    }
    for (const Owned &w : r.owned) line("dsp_owned " + std::to_string(w.row) + " " + std::to_string(w.word) + " " + w.src);
    return t;
}

/* ------------------------------------------------------------------ [Ins] chains (native_chain_composer.py compose)
 *
 * Up to four effects on one input (or linked pair, or an aux return), one physical chain: a port of open-apollo 2.1.0's
 * mixer-engine/native_chain_composer.py compose() (and ComposedEffect, ComposedChain.record_tap_plan, _live_observations
 * of plugin_native_chain.py) over the C++ carrier above. Each member is first built as its own single-effect carrier
 * recipe (BuildRecipe = GeneratedOrdinaryCarrier.composition_arguments, StereoProfiledCarrierProvider's stereo words
 * included); compose() then keeps the first member's carrier rows 1-9 (the Mic Input Strip), or for an aux return the
 * plain Input Strip (the Virtual-input form, carrier 0x83E), sizes the head for N = 1 + effects, writes the delay words,
 * puts each effect's rows under head word 8 + 2k, its startup blocks rebased to its base row, its startup observations
 * again ("fresh initializers"), then (ours) its accepted control words (the saved "diff"), and the record tap: bit 0x4 of
 * word 4 on the last effect (UAD REC) or on the Mic strip's m0 (UAD MON, every effect cleared). The text form (ChainText)
 * is what tools/plugin-recipe-test compares with upstream's own compose() on real bundles (chain-compare.py).
 */

static const ULONG kRecordTap = 0x4;
static const ULONG kInputCarrier = 0x83E;
static const ULONG kChainEffectsMax = 4;
static const ULONG kDelayBudget = 200;              /* MAX_DELAY_COMP_SAMPLES "Medium": the plain strips' record delay (m0 word 16) */

struct ChainMember {
    const Bundle *bundle = nullptr;
    const std::map<ULONG, ULONG> *diff = nullptr;   /* accepted control words in the plug-in's own rows (row 0 = chain row 10) */
    ULONG slot = 0;                                 /* the logical slot 0..3; members come in slot order */
};

struct ChainOptions {
    const PLUG_MODEL *unit = nullptr;               /* null = the x8p */
    ULONG input = 0;                                /* analog input (a pair's first); unused for an aux */
    bool stereo = false;
    LONG aux = -1;                                  /* 0 / 1: the aux return pair (stereo only); -1: an analog input */
    bool mon = false;                               /* UAD MON: record before the inserts */
    bool fixedDelays = false;                       /* true: the three numbers below; false: plan_compensation at Medium */
    ULONG recordDelay = 200, monitorDelay = 0, capacity = 256;
    bool verifyModules = true;
    std::string specsDir, ownedList;
};

struct ChainEffect {
    std::string name, layout, bundle, dllSha;
    ULONG slot = 0, renderId = 0, latency = 0, base = 0, rows = 0, tapFlags = 0, codeWords = 0, dataWords = 0, pages = 0;
    std::vector<Module> modules;
};

struct ChainRecipe {
    std::vector<ChainEffect> effects;
    ULONG carrier = kMicCarrier, in1 = 0, in2 = kSynchMono, count = 1;
    ULONG recordDelay = 0, monitorDelay = 0, capacity = 0, latency = 0, recordLatency = 0;
    bool mon = false, overloaded = false, unison = false;
    ULONG scratch[3] = { 0, 0, 0 };
    std::vector<Sha> res;
    std::vector<Row> rows;
    Words cmd;
    std::vector<std::pair<ULONG, ULONG>> pages;
    std::vector<Obs> obs;                           /* compose()'s observations: the whole startup, read back while staged */
    std::vector<Obs> live;                          /* _live_observations: the header words once it runs */
    std::vector<Owned> owned;                       /* DSP-owned words, chain rows */
    ULONG codeWords = 0, dataWords = 0;             /* the members' own pool words (distinct modules) */
    bool staged() const { return pages.size() > 1; }
};

/* _blocks(): the WRITE blocks of a command list as (offset, length); false on a malformed list */
static inline bool Blocks(const Words &cmd, std::vector<std::pair<size_t, ULONG>> &out)
{
    out.clear();
    size_t i = 0;
    while (i < cmd.size()) {
        ULONG n = cmd[i] & 0xFFFF;
        if ((cmd[i] >> 16) != 0x14 || n < 4 || i + n > cmd.size()) return false;
        out.push_back({ i, n });
        i += n;
    }
    return true;
}

/* _shadow(): (row, word) -> the value the WRITEs leave there */
static inline std::map<RW, ULONG> ShadowOf(const Words &cmd)
{
    std::map<RW, ULONG> out;
    std::vector<std::pair<size_t, ULONG>> bl;
    if (!Blocks(cmd, bl)) return out;
    for (const auto &b : bl) {
        ULONG row = cmd[b.first + 1] >> 24, start = cmd[b.first + 1] & 0xFFFFFF, mask = cmd[b.first + 2];
        ULONG pos = start;
        for (ULONG k = 3; k < b.second; k++) {
            pos = PlugWriteSkip(start, mask, pos);
            out[{ row, pos }] = cmd[b.first + k];
            pos++;
        }
    }
    return out;
}

/* _tap() */
static inline ULONG Tap(ULONG flags, bool last, bool mon)
{
    return (last && !mon) ? (flags | kRecordTap) : (flags & ~kRecordTap);
}

/* plan_compensation(Medium) for one chain: record = budget - latency through the record tap, monitor 0 (no group alignment:
   the retained strips' delay IOCTL is not built), capacity the smallest power of two >= budget + 16 + 32 (256). */
static inline void ChainDelays(ULONG latency, bool mon, ULONG &record, ULONG &monitor, ULONG &capacity, bool &overloaded)
{
    ULONG rec = mon ? 0 : latency;
    record = rec < kDelayBudget ? kDelayBudget - rec : 0;
    monitor = 0;
    capacity = 128;
    while (capacity < kDelayBudget + 16 + 32) capacity *= 2;
    overloaded = latency > kDelayBudget;
}

/* input_strip_rows() and INPUT_STRIP (native_carrier.py:65-81): the plain Input Strip in a chain, rows 1-5 */
static inline std::vector<Row> InputStripRows()
{
    return { { 1, 0x1e, L(0, 6), 0 }, { 3, 2048, L(1, 14), 2 }, { 3, 2048, L(1, 15), 2 }, { 3, 2048, L(1, 6), 2 }, { 3, 2048, L(1, 7), 2 } };
}

static inline void InputStrip(Words &out)
{
    static const ULONG v[] = { 0x40000000, 1, 1, 0x100, 0x3a888889, 0x3b83126f };
    out.push_back(0x00140000u | (ULONG)(_countof(v) + 3));
    out.push_back(L(1, 0));
    out.push_back(0x00dfffec);
    out.insert(out.end(), v, v + _countof(v));
}

/* The driver's own checks on a composed chain, as apollo.sys will run them (rows, every page, the directory). */
static inline bool ChainDriverChecks(const ChainRecipe &c, bool unison, std::string &err)
{
    std::vector<PLUG_ROW> pr;
    for (const Row &x : c.rows) pr.push_back({ x.pool, x.dwords, x.link, x.flags });
    std::vector<ULONG> s2(2 * pr.size() + 2), s1(pr.size() + 1);
    ULONG n = 0, ids5[5], nids = 0, carrier = 0;
    if (pr.size() > PLUG_PRIV_MAX) { err = "the chain has " + std::to_string(pr.size()) + " private rows: the driver takes 127"; return false; }
    if (c.res.size() > PLUG_RES_MAX) { err = "the chain needs " + std::to_string(c.res.size()) + " modules: the driver takes 64"; return false; }
    if (PlugCheckRows(pr.data(), (ULONG)pr.size(), s2.data(), s1.data()) != PLUG_OK) { err = "the driver refuses the chain rows"; return false; }
    for (const auto &pg : c.pages) {
        if (PlugCheckCmds(pr.data(), (ULONG)pr.size(), c.cmd.data() + pg.first, pg.second, &n) != PLUG_OK) { err = "the driver refuses a startup page"; return false; }
        if (&pg != &c.pages[0] && PlugWritesDirectory(c.cmd.data() + pg.first, pg.second)) { err = "a later startup page writes the chain directory"; return false; }
    }
    ULONG first = c.effects.empty() ? 0 : c.effects[0].renderId;
    if (PlugChainIdsEx(pr.data(), (ULONG)pr.size(), c.cmd.data(), c.pages[0].second, 1, unison ? 1 : 0, first, ids5, &nids, &carrier) != PLUG_OK ||
        nids != c.effects.size() || (!unison && carrier != c.carrier)) {
        err = "the chain directory is not what was composed";
        return false;
    }
    for (size_t k = 0; k < c.effects.size(); k++) {
        if (ids5[k] != c.effects[k].renderId) { err = "the chain directory names another plug-in"; return false; }
    }
    return true;
}

/* The members' carrier recipes (each with no saved words: those go after the fresh initializers). */
static inline bool ChainMembers(const std::vector<ChainMember> &m, const ModuleReader &read, const ChainOptions &o, bool unisonFirst,
                                std::vector<Recipe> &rs, std::string &err)
{
    rs.assign(m.size(), Recipe{});
    for (size_t k = 0; k < m.size(); k++) {
        if (m[k].bundle == nullptr) { err = "no bundle"; return false; }
        RecipeOptions ro;
        ro.unit = o.unit;
        ro.input = o.aux >= 0 ? 0 : o.input;       /* an aux chain uses the members' rows and words only */
        ro.stereo = o.stereo;
        ro.verifyModules = o.verifyModules;
        ro.specsDir = o.specsDir;
        ro.ownedList = o.ownedList;
        ro.unisonHeader = unisonFirst && k == 0;
        if (!BuildRecipe(*m[k].bundle, read, ro, rs[k], err)) { err = m[k].bundle->name + ": " + err; return false; }
    }
    return true;
}

/*
 * One member as compose() places it at Base: its rows (root under head word Link), its startup blocks (rows >= 10)
 * rebased, its startup observations again, its saved words, its tap write; its observations and DSP-owned words.
 */
static inline bool ChainPlace(ChainRecipe &c, const ChainMember &m, const Recipe &r, ULONG k, ULONG headLink, bool last,
                              std::vector<Obs> &obs, std::string &err)
{
    ULONG base = (ULONG)c.rows.size();
    for (size_t i = 10; i < r.rows.size(); i++) {
        const Row &x = r.rows[i];
        ULONG target = (i == 10) ? L(0, headLink) : L(base + (x.link >> 24) - 10, x.link & 0xFFFFFF);
        c.rows.push_back({ x.pool, x.dwords, target, x.flags });
    }
    std::vector<std::pair<size_t, ULONG>> bl;
    if (!Blocks(r.cmd, bl)) { err = "bounded original WRITE blocks required"; return false; }
    for (const auto &b : bl) {
        ULONG row = r.cmd[b.first + 1] >> 24;
        if (row < 10) continue;
        c.cmd.push_back(r.cmd[b.first]);
        c.cmd.push_back(L(base + row - 10, r.cmd[b.first + 1] & 0xFFFFFF));
        c.cmd.insert(c.cmd.end(), r.cmd.begin() + (ptrdiff_t)b.first + 2, r.cmd.begin() + (ptrdiff_t)(b.first + b.second));
    }
    /* "fresh initializers precede the retained accepted control state": the startup observations (rows >= 10) again */
    std::map<ULONG, ULONG> current;
    for (const Obs &o : r.obs) {
        if (o.row < 10) continue;
        for (size_t i = 0; i < o.v.size(); i++) current[((o.row - 10) << 24) | (o.word + (ULONG)i)] = o.v[i];
    }
    Compact(c.cmd, current, base);
    if (m.diff && !m.diff->empty()) Compact(c.cmd, *m.diff, base);     /* ours: the accepted controls (empty upstream) */
    std::map<RW, ULONG> sh = ShadowOf(r.cmd);
    auto f = sh.find({ 10, 4 });
    ULONG flags = (f == sh.end()) ? 0 : f->second;
    ChainEffect e;
    e.name = r.name;
    e.layout = r.layout;
    e.bundle = r.bundle;
    e.dllSha = r.dllSha;
    e.slot = m.slot;
    e.renderId = r.renderId;
    e.latency = r.latency;
    e.base = base;
    e.rows = (ULONG)(r.rows.size() - 10);
    e.tapFlags = flags & ~kRecordTap;
    e.modules = r.modules;
    e.codeWords = r.codeWords;
    e.dataWords = r.dataWords;
    e.pages = (ULONG)r.pages.size();
    Write(c.cmd, base, { { 1, c.count }, { 2, c.count }, { 4, Tap(flags, last, c.mon) } });
    c.effects.push_back(e);
    /* ComposedEffect._observations over the provider's own (with the saved words: what the DSP must hold) */
    std::map<ULONG, ULONG> words = r.initialized;
    if (m.diff) for (const auto &kv : *m.diff) words[kv.first] = kv.second;
    std::vector<Obs> eo;
    Observations(*m.bundle, words, eo);
    if (c.count == 2) for (ULONG row : { 0u, 1u, 10u }) eo.push_back({ row, 1, { 2, 2 } });
    for (const Obs &o : eo) {
        if (o.row == 1 && o.word == 16 && o.v.size() == 1) obs.push_back({ 1, 16, { c.recordDelay } });
        else if (o.row >= 10) obs.push_back({ base + o.row - 10, o.word, o.v });
        else if ((o.row == 0 || o.row == 1) && o.word == 1 && o.v == Words{ 2, 2 }) obs.push_back(o);
        else { err = "unexpected carrier observation"; return false; }
    }
    for (const Owned &w : r.owned) c.owned.push_back({ w.row >= 10 ? base + w.row - 10 : w.row, w.word, w.src });
    (void)k;
    return true;
}

static inline void ChainFinish(ChainRecipe &c, std::vector<Obs> &obs)
{
    /* dict.fromkeys(observations): the first of equal (row, word, values) kept, in order */
    std::set<std::tuple<ULONG, ULONG, Words>> seen;
    for (const Obs &o : obs) {
        if (seen.insert({ o.row, o.word, o.v }).second) c.obs.push_back(o);
    }
    std::set<ULONG> heads;
    for (const ChainEffect &e : c.effects) heads.insert(e.base);
    for (const Obs &o : c.obs) {
        if (o.row == 0 || (o.row == 1 && o.word == 4) || (heads.count(o.row) && (o.word == 1 || o.word == 4))) c.live.push_back(o);
    }
    if (c.live.empty()) c.live = c.obs;
    std::set<std::string> mods;
    for (const ChainEffect &e : c.effects) {
        for (const Module &m : e.modules) {
            if (!mods.insert(m.sha).second) continue;
            (m.pool == 0 ? c.codeWords : c.dataWords) += m.words;
        }
    }
}

static inline bool BuildChain(const std::vector<ChainMember> &m, const ModuleReader &read, const ChainOptions &o, ChainRecipe &c,
                              std::string &err)
{
    c = ChainRecipe{};
    if (m.empty() || m.size() > kChainEffectsMax) { err = "one to four effects required"; return false; }
    for (size_t k = 0; k < m.size(); k++) {
        if (m[k].slot > 3 || (k && m[k].slot <= m[k - 1].slot)) { err = "strictly increasing logical slots 0..3 required"; return false; }
    }
    const bool aux = o.aux >= 0;
    if (aux && (o.aux > 1 || !o.stereo || o.mon)) { err = "an aux bus takes stereo inserts, always printed"; return false; }
    std::vector<Recipe> rs;
    if (!ChainMembers(m, read, o, false, rs, err)) return false;
    for (const Recipe &r : rs) {
        for (int i = 0; i < 10; i++) {
            if (r.rows[i].pool != rs[0].rows[i].pool || r.rows[i].dwords != rs[0].rows[i].dwords || r.rows[i].link != rs[0].rows[i].link ||
                r.rows[i].flags != rs[0].rows[i].flags) { err = "all effects must share the same proven carrier"; return false; }
        }
        c.latency += r.latency;
    }
    const ULONG n = (ULONG)m.size();
    c.count = o.stereo ? 2 : 1;
    c.mon = o.mon;
    c.carrier = aux ? kInputCarrier : kMicCarrier;
    c.recordLatency = o.mon ? 0 : c.latency;
    if (o.fixedDelays) {
        if (o.recordDelay >= 2048 || o.monitorDelay >= 2048 || o.capacity > 2048 || (o.capacity & (o.capacity - 1))) {
            err = "delays must fit the 2048-word carrier buffers"; return false;
        }
        c.recordDelay = o.recordDelay;
        c.monitorDelay = o.monitorDelay;
        c.capacity = o.capacity;
        c.overloaded = c.latency > kDelayBudget;
    } else {
        ChainDelays(c.latency, o.mon, c.recordDelay, c.monitorDelay, c.capacity, c.overloaded);
    }
    if (aux) {
        c.in1 = PLUG_AUX_FIRST_SYNCH + 2 * (ULONG)o.aux;
        c.in2 = c.in1 + 1;
    } else {
        c.in1 = rs[0].in1;
        c.in2 = o.stereo ? rs[0].in2 : kSynchMono;
    }
    c.rows.assign(rs[0].rows.begin(), rs[0].rows.begin() + 10);
    c.rows[0] = { 1, 6 + 2 * (1 + n), kNone, 0 };
    std::map<ULONG, ULONG> head{ { 0, 0x40000000 }, { 1, c.count }, { 2, c.count }, { 4, 0x22 }, { 5, 1 + n }, { 7, c.carrier } };
    if (aux) {
        c.rows.resize(1);
        for (const Row &x : InputStripRows()) c.rows.push_back(x);
        InputStrip(c.cmd);
    } else {
        std::vector<std::pair<size_t, ULONG>> bl;
        if (!Blocks(rs[0].cmd, bl)) { err = "bounded original WRITE blocks required"; return false; }
        for (const auto &b : bl) {
            ULONG row = rs[0].cmd[b.first + 1] >> 24;
            if (row >= 1 && row < 10) c.cmd.insert(c.cmd.end(), rs[0].cmd.begin() + (ptrdiff_t)b.first, rs[0].cmd.begin() + (ptrdiff_t)(b.first + b.second));
        }
    }
    Write(c.cmd, 1, { { 1, c.count }, { 2, c.count }, { 5, c.capacity }, { 8, c.monitorDelay }, { 16, c.recordDelay } });
    if (o.mon) Write(c.cmd, 1, { { 4, kRecordTap } });
    std::vector<Obs> obs{ { 0, 0, { 0x40000000, c.count, c.count } }, { 0, 4, { 0x22, 1 + n } }, { 1, 1, { c.count, c.count } },
                          { 1, 5, { c.capacity } }, { 1, 8, { c.monitorDelay } }, { 1, 16, { c.recordDelay } } };
    if (o.mon) obs.push_back({ 1, 4, { kRecordTap } });
    std::vector<std::vector<Obs>> per(n);
    for (ULONG k = 0; k < n; k++) {
        if (!ChainPlace(c, m[k], rs[k], k, 8 + 2 * k, k + 1 == n, per[k], err)) return false;
        head[9 + 2 * k] = rs[k].renderId;
        for (const Sha &s : rs[k].res) {
            if (std::find(c.res.begin(), c.res.end(), s) == c.res.end()) c.res.push_back(s);
        }
    }
    Words body;
    Write(body, 0, head);
    c.cmd.insert(c.cmd.begin(), body.begin(), body.end());
    if (!Check(c.rows, c.cmd, err)) return false;
    std::map<RW, ULONG> sh = ShadowOf(c.cmd);
    for (ULONG k = 0; k < n; k++) {
        obs.insert(obs.end(), per[k].begin(), per[k].end());
        obs.push_back({ c.effects[k].base, 1, { c.count, c.count } });
        obs.push_back({ c.effects[k].base, 4, { sh[{ c.effects[k].base, 4 }] } });
        obs.push_back({ 0, 9 + 2 * k, { c.effects[k].renderId } });
    }
    for (int p = 0; p < 3; p++) {
        for (const Recipe &r : rs) c.scratch[p] = std::max(c.scratch[p], r.scratch[p]);
    }
    if (!Pages(c.cmd, c.pages)) { err = "the startup cannot be cut into pages of whole WRITE blocks"; return false; }
    ChainFinish(c, obs);
    if (c.obs.empty() || c.obs.size() > 32768) { err = "bounded nonempty observation set required"; return false; }
    return ChainDriverChecks(c, false, err);
}

/*
 * ComposedChain.record_tap_plan: the live UAD REC / MON flip, word 4 of m0 and of each effect, all outside the chain
 * directory, so one SERVICE write takes it on the running chain; its readback.
 */
static inline void TapPlan(const ChainRecipe &c, bool mon, Words &cmd, std::vector<Obs> &obs)
{
    std::map<ULONG, ULONG> words;                   /* row << 24 | word (rows < 256 here) */
    const size_t n = c.effects.size();
    if (!c.unison) words[L(1, 4)] = mon ? kRecordTap : 0;
    for (size_t k = 0; k < n; k++) {
        bool tap = c.unison ? ((k == 0) ? (mon || n == 1) : (k + 1 == n && !mon)) : (k + 1 == n && !mon);
        words[L(c.effects[k].base, 4)] = tap ? (c.effects[k].tapFlags | kRecordTap) : (c.effects[k].tapFlags & ~kRecordTap);
    }
    cmd.clear();
    obs.clear();
    Compact(cmd, words, 0);
    for (const auto &kv : words) obs.push_back({ kv.first >> 24, kv.first & 0xFFFFFF, { kv.second } });
}

/*
 * [Ins] Unison (experimental: upstream composes none, its mapper maps no Unison form; docs/PLUGINS.md P8): the header (a
 * bundle with b.unison) is member 0 in place of the Mic carrier, linked at head word 6, its own rows and startup words at
 * base 1; then 0-4 effects as compose() places them. The tap: UAD REC on the last effect (the header when it is alone),
 * UAD MON on the header ("Unison insert processing is always recorded"). No Mic strip, so no carrier delay words: the
 * header owns its own. Driver: CREATE F_STAGE | F_CHAIN | F_UNISON, then BEGIN_UNISON, ACTIVATE_UNISON.
 */
static inline bool BuildUnisonChain(const ChainMember &header, const std::vector<ChainMember> &m, const ModuleReader &read,
                                    const ChainOptions &o, ChainRecipe &c, std::string &err)
{
    c = ChainRecipe{};
    if (header.bundle == nullptr || !header.bundle->unison) { err = "not a Unison preamp map"; return false; }
    if (m.size() > kChainEffectsMax || o.aux >= 0) { err = "a Unison chain is the header and up to four inserts on an analog input"; return false; }
    std::vector<ChainMember> all{ header };
    all.insert(all.end(), m.begin(), m.end());
    std::vector<Recipe> rs;
    if (!ChainMembers(all, read, o, true, rs, err)) return false;
    const ULONG n = (ULONG)all.size();
    c.unison = true;
    c.count = o.stereo ? 2 : 1;
    c.mon = o.mon;
    for (const Recipe &r : rs) c.latency += r.latency;
    c.recordLatency = o.mon ? rs[0].latency : c.latency;
    c.carrier = rs[0].renderId;
    c.in1 = rs[0].in1;
    c.in2 = o.stereo ? rs[0].in2 : kSynchMono;
    c.rows.push_back({ 1, 6 + 2 * n, kNone, 0 });
    std::map<ULONG, ULONG> head{ { 0, 0x40000000 }, { 1, c.count }, { 2, c.count }, { 4, 0x22 }, { 5, n } };
    std::vector<Obs> obs{ { 0, 0, { 0x40000000, c.count, c.count } }, { 0, 4, { 0x22, n } } };
    std::vector<std::vector<Obs>> per(n);
    for (ULONG k = 0; k < n; k++) {
        bool last = (k + 1 == n);
        /* the header carries the tap in MON, and in REC when it is alone */
        bool tapHere = (k == 0) ? (o.mon || n == 1) : (last && !o.mon);
        c.mon = false;
        if (!ChainPlace(c, all[k], rs[k], k, 6 + 2 * k, tapHere, per[k], err)) return false;
        c.mon = o.mon;
        head[7 + 2 * k] = rs[k].renderId;
        for (const Sha &s : rs[k].res) {
            if (std::find(c.res.begin(), c.res.end(), s) == c.res.end()) c.res.push_back(s);
        }
    }
    /* no Mic strip: drop the observations ChainPlace kept on carrier rows 0/1 other than the stereo counts */
    Words body;
    Write(body, 0, head);
    c.cmd.insert(c.cmd.begin(), body.begin(), body.end());
    if (!Check(c.rows, c.cmd, err)) return false;
    std::map<RW, ULONG> sh = ShadowOf(c.cmd);
    for (ULONG k = 0; k < n; k++) {
        for (const Obs &x : per[k]) {
            if (x.row == 1 && x.word == 16) continue;
            obs.push_back(x);
        }
        obs.push_back({ c.effects[k].base, 1, { c.count, c.count } });
        obs.push_back({ c.effects[k].base, 4, { sh[{ c.effects[k].base, 4 }] } });
        obs.push_back({ 0, 7 + 2 * k, { c.effects[k].renderId } });
    }
    for (int p = 0; p < 3; p++) {
        for (const Recipe &r : rs) c.scratch[p] = std::max(c.scratch[p], r.scratch[p]);
    }
    if (!Pages(c.cmd, c.pages)) { err = "the startup cannot be cut into pages of whole WRITE blocks"; return false; }
    ChainFinish(c, obs);
    return ChainDriverChecks(c, true, err);
}

/* The chain in text (plugin-recipe-test chain; chain-compare.py prints upstream's compose() the same way). */
static inline std::string ChainText(const ChainRecipe &c)
{
    std::string t;
    char b[256];
    auto line = [&](const std::string &s) { t += s; t += '\n'; };
    line("apollo-plug-chain 1");
    snprintf(b, sizeof(b), "carrier 0x%lx", c.carrier); line(b);
    snprintf(b, sizeof(b), "synch %lu %lu", c.in1, c.in2); line(b);
    snprintf(b, sizeof(b), "delays %lu %lu %lu", c.recordDelay, c.monitorDelay, c.capacity); line(b);
    std::string ids = "render_ids";
    for (const ChainEffect &e : c.effects) { snprintf(b, sizeof(b), " 0x%lx", e.renderId); ids += b; }
    line(ids);
    snprintf(b, sizeof(b), "scratch %lu %lu %lu", c.scratch[0], c.scratch[1], c.scratch[2]); line(b);
    for (const Sha &s : c.res) line("resource " + ShaHex(s));
    for (const Row &x : c.rows) { snprintf(b, sizeof(b), "row %lu %lu 0x%08lx %lu", x.pool, x.dwords, x.link, x.flags); line(b); }
    for (size_t i = 0; i < c.cmd.size(); i += 12) {
        std::string s = "cmd";
        for (size_t k = i; k < c.cmd.size() && k < i + 12; k++) s += " " + Hex8(c.cmd[k]);
        line(s);
    }
    for (const Obs &x : c.obs) {
        std::string s = "observe " + std::to_string(x.row) + " " + std::to_string(x.word) + " " + std::to_string(x.v.size());
        for (ULONG v : x.v) s += " " + Hex8(v);
        line(s);
    }
    return t;
}

/* A planner's plan (rows in the plug-in's single-carrier space: its root = row 10) moved to its base row in the chain. */
static inline bool RebaseCmd(const Words &in, ULONG base, Words &out)
{
    std::vector<std::pair<size_t, ULONG>> bl;
    out.clear();
    if (!Blocks(in, bl)) return false;
    for (const auto &b : bl) {
        ULONG row = in[b.first + 1] >> 24;
        if (row < 10) return false;                 /* a plan writes only the effect's own rows (ComposedEffect._row) */
        out.push_back(in[b.first]);
        out.push_back(L(base + row - 10, in[b.first + 1] & 0xFFFFFF));
        out.insert(out.end(), in.begin() + (ptrdiff_t)b.first + 2, in.begin() + (ptrdiff_t)(b.first + b.second));
    }
    return true;
}

/* ------------------------------------------------------------------ readback (as apollo-plug ReadBack) */

/* Observation runs [I, J) that one SERVICE read covers: one row, ascending, a span of at most 64 words. */
static inline size_t ReadGroupEnd(const std::vector<Obs> &o, size_t I)
{
    size_t j = I + 1;
    ULONG end = o[I].word + (ULONG)o[I].v.size();
    while (j < o.size()) {
        ULONG e2 = o[j].word + (ULONG)o[j].v.size();
        if (o[j].row != o[I].row || o[j].word < o[j - 1].word || std::max(e2, end) - o[I].word > PLUG_READ_WORDS_MAX) break;
        end = std::max(end, e2);
        j++;
    }
    return j;
}

static inline ULONG ReadGroupSpan(const std::vector<Obs> &o, size_t I, size_t J)
{
    ULONG end = 0;
    for (size_t k = I; k < J; k++) end = std::max(end, o[k].word + (ULONG)o[k].v.size());
    return end - o[I].word;
}

/*
 * The words of read group [I, J) that differ, given what one read returned (V from o[I].word on).
 * Owned words are skipped. Second (optional): a second read of the same span; a word that differs
 * and changed between the two reads is DSP-owned (moving): added to *Moving, not a difference.
 */
static inline std::vector<RW> GroupDiffs(const std::vector<Obs> &o, size_t I, size_t J, const ULONG *V, const ULONG *Second,
                                         const std::set<RW> &owned, std::set<RW> *Moving)
{
    std::vector<RW> bad;
    for (size_t k = I; k < J; k++) {
        for (size_t m = 0; m < o[k].v.size(); m++) {
            ULONG word = o[k].word + (ULONG)m, at = word - o[I].word;
            if (owned.count({ o[k].row, word })) continue;
            if (V[at] == o[k].v[m]) continue;
            if (Second != nullptr && Second[at] != V[at]) {
                if (Moving) Moving->insert({ o[k].row, word });
                continue;
            }
            bad.push_back({ o[k].row, word });
        }
    }
    return bad;
}

/* ------------------------------------------------------------------ catalogue and licence */

struct IndexEntry { std::string name, layout, file; };

static inline bool ParseIndex(const std::string &text, std::vector<IndexEntry> &out, std::string &err)
{
    aggjson::Value root;
    aggjson::Parser p(text);
    out.clear();
    if (!p.Parse(root) || root.kind != aggjson::Value::Object) { err = "index.json is not JSON"; return false; }
    const aggjson::Value *b = root.Get("bundles");
    if (b == nullptr || b->kind != aggjson::Value::Array) { err = "index.json lists no bundles"; return false; }
    for (const aggjson::Value &e : b->a) {
        IndexEntry x{ e.Str("name"), e.Str("layout"), e.Str("file") };
        if (!x.name.empty() && (x.layout == "mono" || x.layout == "stereo") && !x.file.empty() &&
            x.file.find_first_of("\\/:") == std::string::npos) out.push_back(x);
    }
    return true;
}

/* One mapped plug-in as the picker shows it (both layouts folded together). */
struct CatalogItem {
    std::string name, monoFile, stereoFile;
    ULONG renderId = 0, latency = 0, codeWords = 0, dataWords = 0, startupWords = 0, params = 0, pages = 0;
    bool ok = false;
    std::string why;
};

/* Fill the descriptor-derived fields of one catalogue entry from a parsed bundle (no module reads). */
static inline void CatalogFrom(const Bundle &b, CatalogItem &c)
{
    RecipeOptions o;
    o.verifyModules = false;
    o.stereo = b.layout == "stereo";
    Recipe r;
    std::string err;
    c.name = b.name;
    c.renderId = b.renderId;
    c.latency = b.latency;
    c.params = (ULONG)b.params.size();
    if (BuildRecipe(b, ModuleReader(), o, r, err)) {
        c.ok = true;
        c.codeWords = r.codeWords;
        c.dataWords = r.dataWords;
        c.startupWords = (ULONG)r.cmd.size();
        c.pages = (ULONG)r.pages.size();
    } else {
        c.ok = false;
        c.why = err;
    }
}

/* ------------------------------------------------------------------ [Chainfix] which mapped plug-ins may run
 *
 * Nov 15 2026: UAD Sphere Mic Collection 180 + UAD AKG BX 20 (stereo, x8p inputs 1-2) stopped DSP 1 from the moment it was
 * routed. The chain itself is upstream's compose() byte for byte (chain-compare.py), and every pool, row and cycle figure
 * of it is inside what single plug-ins already ran at on this x8p. What stands out is the AKG BX 20: open-apollo does not
 * offer it from a user's install at all (native_known_good.json "excluded": "startup words recorded from a Mac trace"),
 * and the map our mapper built of it starts the plug-in with 47 words for 70 rows and a million words of state. Upstream's
 * list names every plug-in its live gate could not qualify from a user's install ("excluded") or saw fail on the Apollo
 * ("not_yet_supported": Studio D Chorus "mono load hung DSP 1 (-110, quarantined until a cold start)", loads refused).
 * Upstream offers none of them; Apollo Control now refuses them too, unless this unit ran the plug-in in that layout
 * (kProvenHere, plugins\p1\proven.txt) or HKCU\Software\Avium\ApolloControl\ExperimentalPlugins = 1.
 * Entries "<name> (mono)" / "<name> (stereo)" name one layout only. Without the list file the floor below still holds.
 */
struct Qualification {
    std::map<std::string, std::string> refused;     /* "<name>|<layout or *>" -> upstream's reason */
    std::set<std::string> proven;                   /* "<name>|<layout or *>" */
};

static inline std::string QualKey(std::string name, std::string &layout)
{
    layout = "*";
    for (const char *l : { "mono", "stereo" }) {
        std::string tail = std::string(" (") + l + ")";
        if (name.size() > tail.size() && name.compare(name.size() - tail.size(), tail.size(), tail) == 0) {
            name.resize(name.size() - tail.size());
            layout = l;
        }
    }
    return name;
}

/* The floor: what this x8p and upstream proved, kept even without the files. */
static inline void QualFloor(Qualification &q)
{
    q.refused["UAD AKG BX 20|*"] = "startup words recorded from a Mac trace (open-apollo); the chain it was in stopped this x8p's "
                                   "DSP 1 on Nov 15 2026";
    q.refused["UAD Studio D Chorus|*"] = "skipped: mono load hung DSP 1 (-110, quarantined until a cold start) (open-apollo)";
    /* docs/PLUGINS.md 4.1 / step-p1, Nov 15 2026: gate P1 passed on x8p input 1 (mono) */
    for (const char *n : { "UAD Ampeg SVTVR Classic", "UAD Lexicon 480L", "UAD Precision De-Esser", "UAD UA 1176LN Legacy" }) {
        q.proven.insert(std::string(n) + "|mono");
    }
    /* Apollo Control on x8p inputs 1-2, Nov 15 2026 (alone, mono and stereo) */
    q.proven.insert("UAD Sphere Mic Collection 180|mono");
    q.proven.insert("UAD Sphere Mic Collection 180|stereo");
}

/* upstream's native_known_good.json: its "excluded" and "not_yet_supported" maps (name -> reason) */
static inline bool QualKnownGood(const std::string &text, Qualification &q, std::string &err)
{
    aggjson::Value root;
    aggjson::Parser p(text);
    if (!p.Parse(root) || root.kind != aggjson::Value::Object) { err = "the known-good list is not JSON"; return false; }
    for (const char *k : { "excluded", "not_yet_supported" }) {
        const aggjson::Value *m = root.Get(k);
        if (m == nullptr) continue;
        if (m->kind != aggjson::Value::Object) { err = std::string("\"") + k + "\" is not an object"; return false; }
        for (const auto &kv : m->o) {
            std::string layout, name = QualKey(kv.first, layout);
            if (name.empty() || kv.second.kind != aggjson::Value::String) continue;
            q.refused.emplace(name + "|" + layout, kv.second.s + " (open-apollo)");     /* the floor's text stays */
        }
    }
    return true;
}

/* plugins\p1\proven.txt: "<name> | <mono|stereo|*> | <note>" per line, '#' comments */
static inline void QualProven(const std::string &text, Qualification &q)
{
    size_t at = 0;
    auto trim = [](const std::string &s) {
        size_t f = s.find_first_not_of(" \t\r"), l = s.find_last_not_of(" \t\r");
        return f == std::string::npos ? std::string() : s.substr(f, l - f + 1);
    };
    while (at < text.size()) {
        size_t e = text.find('\n', at);
        std::string line = trim(text.substr(at, e == std::string::npos ? std::string::npos : e - at));
        at = (e == std::string::npos) ? text.size() : e + 1;
        if (line.empty() || line[0] == '#') continue;
        size_t a = line.find('|');
        if (a == std::string::npos) continue;
        size_t c = line.find('|', a + 1);
        std::string name = trim(line.substr(0, a)), layout = trim(line.substr(a + 1, c == std::string::npos ? std::string::npos : c - a - 1));
        if (name.empty() || (layout != "mono" && layout != "stereo" && layout != "*")) continue;
        q.proven.insert(name + "|" + layout);
    }
}

/* "" = it may run in this layout; else why not (upstream's reason). */
static inline std::string QualWhy(const Qualification &q, const std::string &name, const std::string &layout)
{
    if (q.proven.count(name + "|" + layout) || q.proven.count(name + "|*")) return std::string();
    for (const char *l : { "*", "" }) {
        auto f = q.refused.find(name + "|" + (*l ? std::string(l) : layout));
        if (f != q.refused.end()) {
            return "not qualified on an Apollo from a user's install: " + f->second + ". It has not run on this unit in " + layout +
                   ", and a plug-in that fails here can stop the DSP until a power cycle";
        }
    }
    return std::string();
}

/* ------------------------------------------------------------------ saved inserts */

/* {"0x<ref>": value} <-> map (plughelper.py's "diff") */
static inline bool DiffFromJson(const aggjson::Value *v, std::map<ULONG, ULONG> &out)
{
    out.clear();
    if (v == nullptr || v->kind == aggjson::Value::Null) return true;
    if (v->kind != aggjson::Value::Object) return false;
    for (const auto &kv : v->o) {
        ULONG k = 0, x = 0;
        if (!U32Text(kv.first, k) || !U32(&kv.second, x)) return false;
        out[k] = x;
    }
    return true;
}

static inline std::string JsonEsc(const std::string &s)
{
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

static inline std::string DiffToJson(const std::map<ULONG, ULONG> &d)
{
    std::string o = "{";
    bool first = true;
    char b[48];
    for (const auto &kv : d) {
        snprintf(b, sizeof(b), "%s\"0x%lx\":%lu", first ? "" : ",", kv.first, kv.second);
        o += b;
        first = false;
    }
    return o + "}";
}

/*
 * One saved insert (%APPDATA%\Avium\ApolloControl\plugins.json): which unit and input, the plug-in
 * and the bundle it was built from (by name and SHA-256: a plug-in mapped again starts from its
 * defaults), the parameter values and the plug-in's texts for them, and the accepted control words.
 */
struct Saved {
    std::string unit;
    ULONG input = 0;
    bool stereo = false, bypass = false;
    std::string name, file, sha;
    Words values;
    std::vector<std::string> texts;
    std::map<ULONG, ULONG> diff;
};

static inline std::string SavedToJson(const std::vector<Saved> &list, const std::string &mapsDir)
{
    std::string j = "{\"version\":1,\"mapsDir\":" + JsonEsc(mapsDir) + ",\"inserts\":[";
    bool first = true;
    for (const Saved &in : list) {
        j += first ? "{" : ",{";
        first = false;
        j += "\"unit\":" + JsonEsc(in.unit) + ",\"input\":" + std::to_string(in.input) + ",\"stereo\":" + (in.stereo ? "true" : "false") +
             ",\"bypass\":" + (in.bypass ? "true" : "false") + ",\"name\":" + JsonEsc(in.name) + ",\"bundle\":" + JsonEsc(in.file) +
             ",\"bundleSha\":" + JsonEsc(in.sha) + ",\"values\":[";
        for (size_t k = 0; k < in.values.size(); k++) j += (k ? "," : "") + std::to_string(in.values[k]);
        j += "],\"texts\":[";
        for (size_t k = 0; k < in.texts.size(); k++) j += (k ? "," : "") + JsonEsc(in.texts[k]);
        j += "],\"diff\":" + DiffToJson(in.diff) + "}";
    }
    return j + "]}\n";
}

/* What a damaged or hand-edited file holds that cannot be an insert is dropped, entry by entry. */
static inline bool SavedFromJson(const std::string &text, std::vector<Saved> &list, std::string &mapsDir)
{
    aggjson::Value root;
    aggjson::Parser p(text);
    list.clear();
    mapsDir.clear();
    if (!p.Parse(root) || root.kind != aggjson::Value::Object) return false;
    mapsDir = root.Str("mapsDir");
    const aggjson::Value *a = root.Get("inserts");
    if (a == nullptr || a->kind != aggjson::Value::Array) return true;
    std::set<std::pair<std::string, ULONG>> taken;
    for (const aggjson::Value &e : a->a) {
        Saved in;
        ULONG v = 0;
        if (e.kind != aggjson::Value::Object || !U32(e.Get("input"), in.input) || in.input >= kInputs) continue;
        const aggjson::Value *b = e.Get("stereo");
        in.stereo = b && b->kind == aggjson::Value::Bool && b->b;
        b = e.Get("bypass");
        in.bypass = b && b->kind == aggjson::Value::Bool && b->b;
        in.unit = e.Str("unit");
        in.name = e.Str("name");
        in.file = e.Str("bundle");
        in.sha = e.Str("bundleSha");
        if (in.name.empty() || in.file.empty() || in.file.find_first_of("\\/:") != std::string::npos ||
            (in.stereo && ((in.input & 1) || in.input + 1 >= kInputs))) continue;
        bool ok = true;
        if (const aggjson::Value *vs = e.Get("values")) {
            for (const aggjson::Value &x : vs->a) { ok = ok && U32(&x, v); in.values.push_back(v); }
        }
        if (const aggjson::Value *ts = e.Get("texts")) {
            for (const aggjson::Value &x : ts->a) in.texts.push_back(x.s);
        }
        if (!ok || !DiffFromJson(e.Get("diff"), in.diff)) { in.values.clear(); in.texts.clear(); in.diff.clear(); }
        /* one insert per input and unit: the first one listed */
        if (taken.count({ in.unit, in.input }) || (in.stereo && taken.count({ in.unit, in.input + 1 }))) continue;
        taken.insert({ in.unit, in.input });
        if (in.stereo) taken.insert({ in.unit, in.input + 1 });
        list.push_back(std::move(in));
    }
    return true;
}


/* ------------------------------------------------------------------ [Ins] saved chains (plugins.json version 2)
 *
 * Per unit and target (an analog input or linked pair, or an aux bus): UAD REC / MON, up to four slots (0-3) each with
 * its plug-in, bundle (name and SHA-256), bypass, parameter values, texts and accepted control words, and the Unison
 * preamp slot. A version 1 file ("inserts", one per input) reads as chains with slot 0.
 */
struct SavedSlot {
    ULONG slot = 0;
    bool bypass = false;
    std::string name, file, sha;
    Words values;
    std::vector<std::string> texts;
    std::map<ULONG, ULONG> diff;
};

struct SavedChain {
    std::string unit;
    ULONG input = 0;
    LONG aux = -1;                                  /* 0/1: an aux bus (always stereo); -1: an analog input */
    bool stereo = false, mon = false;
    std::vector<SavedSlot> slots;                   /* by slot, ascending, at most one per slot */
    bool hasUnison = false;
    SavedSlot unison;
};

static inline std::string SavedSlotJson(const SavedSlot &s)
{
    std::string j = "{\"slot\":" + std::to_string(s.slot) + ",\"bypass\":" + (s.bypass ? "true" : "false") + ",\"name\":" + JsonEsc(s.name) +
                    ",\"bundle\":" + JsonEsc(s.file) + ",\"bundleSha\":" + JsonEsc(s.sha) + ",\"values\":[";
    for (size_t k = 0; k < s.values.size(); k++) j += (k ? "," : "") + std::to_string(s.values[k]);
    j += "],\"texts\":[";
    for (size_t k = 0; k < s.texts.size(); k++) j += (k ? "," : "") + JsonEsc(s.texts[k]);
    return j + "],\"diff\":" + DiffToJson(s.diff) + "}";
}

static inline std::string ChainsToJson(const std::vector<SavedChain> &list, const std::string &mapsDir)
{
    std::string j = "{\"version\":2,\"mapsDir\":" + JsonEsc(mapsDir) + ",\"chains\":[";
    bool first = true;
    for (const SavedChain &c : list) {
        j += first ? "{" : ",{";
        first = false;
        j += "\"unit\":" + JsonEsc(c.unit) + ",\"input\":" + std::to_string(c.input) + ",\"aux\":" + (c.aux < 0 ? std::string("null") : std::to_string(c.aux)) +
             ",\"stereo\":" + (c.stereo ? "true" : "false") + ",\"mon\":" + (c.mon ? "true" : "false") + ",\"slots\":[";
        for (size_t k = 0; k < c.slots.size(); k++) j += (k ? "," : "") + SavedSlotJson(c.slots[k]);
        j += "],\"unison\":" + (c.hasUnison ? SavedSlotJson(c.unison) : std::string("null")) + "}";
    }
    return j + "]}\n";
}

static inline bool SavedSlotFrom(const aggjson::Value &e, SavedSlot &s)
{
    ULONG v = 0;
    const aggjson::Value *b = e.Get("bypass");
    s.bypass = b && b->kind == aggjson::Value::Bool && b->b;
    s.name = e.Str("name");
    s.file = e.Str("bundle");
    s.sha = e.Str("bundleSha");
    if (s.name.empty() || s.file.empty() || s.file.find_first_of("\\/:") != std::string::npos) return false;
    bool ok = true;
    if (const aggjson::Value *vs = e.Get("values")) {
        for (const aggjson::Value &x : vs->a) { ok = ok && U32(&x, v); s.values.push_back(v); }
    }
    if (const aggjson::Value *ts = e.Get("texts")) {
        for (const aggjson::Value &x : ts->a) s.texts.push_back(x.s);
    }
    if (!ok || !DiffFromJson(e.Get("diff"), s.diff)) { s.values.clear(); s.texts.clear(); s.diff.clear(); }
    return true;
}

/* What cannot be a chain is dropped, entry by entry; one chain per unit and input (or aux): the first listed. */
static inline bool ChainsFromJson(const std::string &text, std::vector<SavedChain> &list, std::string &mapsDir)
{
    aggjson::Value root;
    aggjson::Parser p(text);
    list.clear();
    mapsDir.clear();
    if (!p.Parse(root) || root.kind != aggjson::Value::Object) return false;
    if (root.Get("chains") == nullptr) {
        /* version 1: one insert per input = a chain with slot 0 */
        std::vector<Saved> old;
        if (!SavedFromJson(text, old, mapsDir)) return false;
        for (Saved &o : old) {
            SavedChain c;
            c.unit = o.unit;
            c.input = o.input;
            c.stereo = o.stereo;
            SavedSlot s;
            s.bypass = o.bypass;
            s.name = o.name;
            s.file = o.file;
            s.sha = o.sha;
            s.values = o.values;
            s.texts = o.texts;
            s.diff = o.diff;
            c.slots.push_back(s);
            list.push_back(c);
        }
        return true;
    }
    mapsDir = root.Str("mapsDir");
    const aggjson::Value *a = root.Get("chains");
    if (a->kind != aggjson::Value::Array) return true;
    std::set<std::pair<std::string, ULONG>> taken;                  /* (unit, input) or (unit, 100 + aux) */
    for (const aggjson::Value &e : a->a) {
        SavedChain c;
        if (e.kind != aggjson::Value::Object) continue;
        c.unit = e.Str("unit");
        const aggjson::Value *ax = e.Get("aux");
        ULONG v = 0;
        if (ax != nullptr && ax->kind == aggjson::Value::Number) {
            if (!U32(ax, v) || v > 1) continue;
            c.aux = (LONG)v;
            c.stereo = true;
        } else if (!U32(e.Get("input"), c.input) || c.input >= kInputs) {
            continue;
        }
        const aggjson::Value *b = e.Get("stereo");
        if (c.aux < 0) c.stereo = b && b->kind == aggjson::Value::Bool && b->b;
        b = e.Get("mon");
        c.mon = c.aux < 0 && b && b->kind == aggjson::Value::Bool && b->b;
        if (c.aux < 0 && c.stereo && ((c.input & 1) || c.input + 1 >= kInputs)) continue;
        if (const aggjson::Value *ss = e.Get("slots")) {
            for (const aggjson::Value &x : ss->a) {
                SavedSlot s;
                if (x.kind != aggjson::Value::Object || !U32(x.Get("slot"), s.slot) || s.slot > 3 || !SavedSlotFrom(x, s)) continue;
                bool dup = false;
                for (const SavedSlot &t : c.slots) dup |= t.slot == s.slot;
                if (!dup) c.slots.push_back(s);
            }
        }
        std::sort(c.slots.begin(), c.slots.end(), [](const SavedSlot &x, const SavedSlot &y) { return x.slot < y.slot; });
        if (const aggjson::Value *u = e.Get("unison")) {
            if (u->kind == aggjson::Value::Object && c.aux < 0) c.hasUnison = SavedSlotFrom(*u, c.unison);
        }
        ULONG k1 = c.aux >= 0 ? 100 + (ULONG)c.aux : c.input, k2 = (c.aux < 0 && c.stereo) ? c.input + 1 : k1;
        if (taken.count({ c.unit, k1 }) || taken.count({ c.unit, k2 })) continue;
        taken.insert({ c.unit, k1 });
        taken.insert({ c.unit, k2 });
        list.push_back(std::move(c));
    }
    return true;
}

}   /* namespace plughost */
