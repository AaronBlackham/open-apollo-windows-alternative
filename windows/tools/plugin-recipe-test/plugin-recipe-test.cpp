/*
 * plugin-recipe-test - offline checks of Apollo Control's recipe builder (app/src/plugin_host.h),
 * the C++ port of open-apollo's native carrier that tools/plugin-host/p1_recipe.py runs.
 *
 *   plugin-recipe-test selftest
 *       synthetic bundles (no UA data): the recipe text must equal golden.h, which p1_recipe.py
 *       (upstream's own carrier code) wrote for the same synthetic bundles (make-golden.py); plus
 *       the pieces: write/compact masks, the page cut, readback groups, stereo, saved state
 *   [Ins] the chain composer (BuildChain = upstream's native_chain_composer.compose): synthetic chains of 1-4
 *       effects, mono, stereo, UAD MON, an aux return, the tap plan, plan rebasing, Unison structure, the
 *       driver's checks; against golden_chain.h (make-golden-chain.py: upstream's own compose() on the same bundles)
 *   plugin-recipe-test chain --store DIR [--layout mono|stereo] [--input N] [--mon] [--aux A]
 *       [--delays RECORD MONITOR CAPACITY] [--expect FILE] <bundle json>...
 *       a chain of real bundles in text (chain-compare.py compares it with upstream's compose() on this PC)
 *   plugin-recipe-test compare <python recipe> <bundle json> --store <dir with modules\ and specs>
 *       [--layout mono|stereo] [--input N] [--record-delay 200|carrier] [--specs DIR] [--owned FILE]
 *       a real bundle on this PC: the C++ recipe text against p1_recipe.py's, line by line
 *
 * No device, no network, no UA program. Real bundles and recipes are UA-derived: they stay local.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "..\..\app\src\plugin_host.h"
#include "golden.h"
#include "golden_chain.h"

using namespace plughost;

static int g_pass, g_fail;

static void Ok(bool c, const char *what)
{
    if (c) g_pass++; else g_fail++;
    printf("%s  %s\n", c ? "PASS" : "FAIL", what);
}

static std::vector<std::string> Lines(const std::string &t)
{
    std::vector<std::string> out;
    size_t at = 0;
    while (at < t.size()) {
        size_t e = t.find('\n', at);
        std::string l = t.substr(at, e == std::string::npos ? std::string::npos : e - at);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        out.push_back(l);
        at = e == std::string::npos ? t.size() : e + 1;
    }
    return out;
}

/* First differing line of two recipe texts, or -1. */
static long FirstDiff(const std::string &a, const std::string &b, std::string &la, std::string &lb)
{
    std::vector<std::string> x = Lines(a), y = Lines(b);
    size_t n = std::max(x.size(), y.size());
    for (size_t i = 0; i < n; i++) {
        la = i < x.size() ? x[i] : "<end>";
        lb = i < y.size() ? y[i] : "<end>";
        if (la != lb) return (long)i;
    }
    return -1;
}

/* ------------------------------------------------------------------ synthetic data (no UA bytes) */

/* A Bill record: "Bill", id, kind | flags << 8 | arch << 24, payload bytes, w4, then a counting payload. */
static std::vector<UCHAR> Bill(ULONG id, ULONG kind, ULONG arch, ULONG w4, ULONG words)
{
    std::vector<UCHAR> b(words * 4);
    auto put = [&](size_t o, ULONG v) { b[o] = (UCHAR)v; b[o + 1] = (UCHAR)(v >> 8); b[o + 2] = (UCHAR)(v >> 16); b[o + 3] = (UCHAR)(v >> 24); };
    put(0, 0x6c6c6942);
    put(4, id);
    put(8, kind | (arch << 24));
    put(12, words * 4 - 20);
    put(16, w4);
    for (ULONG i = 5; i < words; i++) put(i * 4, 0x01010101u * i + id);
    return b;
}

struct Synth {
    std::vector<std::vector<UCHAR>> mods;
    std::vector<std::string> shas;
};

static Synth MakeModules()
{
    Synth s;
    s.mods.push_back(Bill(0x501, 0, 2, 30, 43));    /* code: 20 pool-0 words */
    s.mods.push_back(Bill(0x502, 1, 0, 12, 43));    /* data pool 1 */
    s.mods.push_back(Bill(0x503, 0, 1, 30, 43));    /* architecture 1: not the Apollo's, left out */
    s.mods.push_back(Bill(0x504, 3, 0, 8, 43));     /* data pool 3 */
    for (auto &m : s.mods) {
        Sha h{};
        Sha256(m.data(), m.size(), h);
        s.shas.push_back(ShaHex(h));
    }
    return s;
}

/* The synthetic bundle (golden.h was made from the same text by make-golden.py). */
static std::string SynthBundle(const Synth &s, const char *name, const char *layout, ULONG latency, ULONG shadowWords, bool dyn)
{
    std::string j = std::string("{\"schema\":\"openapollo-profiled-runtime-bundle/1\",\"status\":\"pass\",\"name\":\"") + name + "\",";
    j += std::string("\"layout\":\"") + layout + "\",\"carrier_channels\":1,\"latency_source\":\"known_good\",";
    j += "\"dll\":{\"path\":\"x\",\"sha256\":\"00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\"},";
    j += std::string("\"descriptor\":{\"plugin\":\"") + name + "\",\"sample_rate\":48000,\"render_id\":2085,\"flags\":0,";
    j += "\"latency_samples\":" + std::to_string(latency) + ",";
    j += std::string("\"io\":") + (strcmp(layout, "stereo") == 0 ? "[2,2]" : "[1,1]") + ",\"configured_input_channels\":" +
         (strcmp(layout, "stereo") == 0 ? "2" : "1") + ",";
    j += "\"rows\":[[1,2000,4294967295,0],[2,4,3,2],[1,40,16777217,2],[3,2048,33554432,0]],";
    j += "\"resources\":[";
    const ULONG ids[] = { 0x501, 0x502, 0x503, 0x504 }, arch[] = { 2, 0, 1, 0 }, kind[] = { 0, 1, 0, 3 }, w4[] = { 30, 12, 30, 8 };
    for (int i = 0; i < 4; i++) {
        j += (i ? "," : "") + std::string("{\"id\":") + std::to_string(ids[i]) + ",\"architecture\":" + std::to_string(arch[i]) +
             ",\"kind\":" + std::to_string(kind[i]) + ",\"flags\":0,\"words\":" + std::to_string(w4[i]) + ",\"sha256\":\"" + s.shas[i] + "\"}";
    }
    j += "],\"scratch\":[500,0,64]},";
    j += "\"startup\":{\"shadow\":{";
    for (ULONG k = 0; k < shadowWords; k++) {
        ULONG w = 5 + k + (k / 7) * 3;              /* gaps every 7 words */
        char b[64];
        snprintf(b, sizeof(b), "%s\"0x%lx\":%lu", k ? "," : "", w, (k * 2654435761u) % 4000000000u);
        j += b;
    }
    j += ",\"0x1000002\":7,\"0x2000003\":4294967295},\"integer_values\":[3,1],\"formatted\":[{\"raw\":\"3 dB\",\"console_trimmed\":\"3 dB\"},"
         "{\"raw\":\"On\",\"console_trimmed\":\"On\"}]},";
    j += "\"parameter_maxima\":[10,1],\"control_sweep\":[],";
    j += "\"parameter_metadata\":{\"controls\":[{\"path\":\"/devices/0/inputs/0/effects/0/parameters/0/Name\",\"type\":\"string\",\"value\":\"Drive\"},"
         "{\"path\":\"/devices/0/inputs/0/effects/0/parameters/0/NormalizedValue\",\"type\":\"float\",\"value\":0.3},"
         "{\"path\":\"/devices/0/inputs/0/effects/0/parameters/1/Name\",\"type\":\"string\",\"value\":\"Power\"},"
         "{\"path\":\"/devices/0/inputs/0/effects/0/parameters/1/NormalizedValue\",\"type\":\"float\",\"value\":1.0},"
         "{\"path\":\"/devices/0/inputs/0/effects/0/parameters/1/StepValue\",\"type\":\"string\",\"values\":[\"Off\",\"On\"]}]},";
    j += std::string("\"profile\":{\"name\":\"") + name + "\"}";
    if (dyn) j += ",\"dynamic_observation_refs\":[[10,9],[10,30]]";
    j += "}";
    return j;
}

struct SynthCase { const char *what, *name, *file, *layout; ULONG latency, shadow; bool dyn, fixed; };
static const SynthCase kCases[] = {
    { "mono, latency 0, record delay 200", "UAD Synthetic Test", "synthetic-test-mono-runtime-bundle.json", "mono", 0, 60, false, true },
    { "mono, latency 64, the carrier's delay, dynamic refs", "UAD Synthetic Carrier", "synthetic-carrier-mono-runtime-bundle.json", "mono",
      64, 60, true, false },
    { "stereo pair 1/2", "UAD Synthetic Stereo", "synthetic-stereo-stereo-runtime-bundle.json", "stereo", 0, 60, false, true },
    { "staged: 1150 startup words", "UAD Synthetic Staged", "synthetic-staged-mono-runtime-bundle.json", "mono", 0, 1150, false, true },
};

static ModuleReader SynthReader(const Synth &s)
{
    return [s](const std::string &sha, std::vector<UCHAR> &bytes) {
        for (size_t i = 0; i < s.shas.size(); i++) {
            if (s.shas[i] == sha) { bytes = s.mods[i]; return true; }
        }
        return false;
    };
}

/* [Ins] Synthetic chains (golden_chain.h, make-golden-chain.py): members are kCases indexes */
struct ChainCase { const char *what; int members[4]; int n; bool stereo, mon; ULONG record, monitor, capacity; };
static const ChainCase kChainCases[] = {
    { "two mono effects, compose() defaults", { 0, 1 }, 2, false, false, 200, 0, 256 },
    { "three mono effects, UAD MON", { 0, 1, 0 }, 3, false, true, 136, 0, 256 },
    { "four mono effects", { 1, 0, 1, 0 }, 4, false, false, 72, 0, 256 },
    { "two stereo effects on the pair 1/2", { 2, 2 }, 2, true, false, 200, 0, 256 },
    { "one effect alone (N = 2), staged member", { 3 }, 1, false, false, 200, 0, 256 },
};

static bool SynthChain(const Synth &s, const ChainCase &g, std::vector<Bundle> &store, ChainRecipe &c, std::string &err, LONG aux = -1)
{
    std::vector<ChainMember> m;
    store.assign((size_t)g.n, Bundle{});
    for (int k = 0; k < g.n; k++) {
        const SynthCase &sc = kCases[g.members[k]];
        if (!ParseBundle(SynthBundle(s, sc.name, sc.layout, sc.latency, sc.shadow, sc.dyn), sc.file, store[(size_t)k], err)) return false;
    }
    for (int k = 0; k < g.n; k++) m.push_back({ &store[(size_t)k], nullptr, (ULONG)k });
    ChainOptions o;
    o.stereo = g.stereo;
    o.mon = g.mon;
    o.aux = aux;
    o.fixedDelays = true;
    o.recordDelay = g.record;
    o.monitorDelay = g.monitor;
    o.capacity = g.capacity;
    return BuildChain(m, SynthReader(s), o, c, err);
}

static void ChainSelfTest(const Synth &s)
{
    std::string err, la, lb;
    char what[200];

    /* golden: upstream's compose() on the same synthetic bundles */
    for (size_t k = 0; k < _countof(kChainCases); k++) {
        std::vector<Bundle> st;
        ChainRecipe c;
        bool ok = SynthChain(s, kChainCases[k], st, c, err);
        std::string golden;
        for (const char *const *l = kChainGoldenLines[k]; *l != nullptr; l++) golden += std::string(*l) + "\n";
        long d = ok ? FirstDiff(ChainText(c), golden, la, lb) : 0;
        snprintf(what, sizeof(what), "chain golden: %s = upstream compose()'s text%s%s", kChainCases[k].what, ok ? "" : ": ", ok ? "" : err.c_str());
        Ok(ok && d < 0, what);
        if (ok && d >= 0) printf("      line %ld\n      ours     %.160s\n      upstream %.160s\n", d + 1, la.c_str(), lb.c_str());
    }
    /* structure: head, directory, bases, the tap, resources shared once */
    {
        std::vector<Bundle> st;
        ChainRecipe c;
        bool ok = SynthChain(s, kChainCases[2], st, c, err);
        std::map<RW, ULONG> sh = ShadowOf(c.cmd);
        Ok(ok && c.rows[0].dwords == 6 + 2 * 5 && sh[{ 0, 5 }] == 5 && sh[{ 0, 7 }] == kMicCarrier && c.effects.size() == 4 &&
           c.effects[0].base == 10 && c.effects[1].base == 14 && c.effects[3].base == 22 && c.rows[10].link == L(0, 8) &&
           c.rows[14].link == L(0, 10) && c.rows[22].link == L(0, 14),
           "chain of 4: head of 16 words, N = 5, Mic carrier, effect roots under head words 8, 10, 12, 14");
        Ok(ok && (sh[{ 22, 4 }] & kRecordTap) && !(sh[{ 10, 4 }] & kRecordTap) && !(sh[{ 18, 4 }] & kRecordTap) &&
           sh.count({ 1, 4 }) == 0 && c.res.size() == 5 + 3, "UAD REC: the tap on the last effect only; modules shared once");
        Words tc;
        std::vector<Obs> to;
        TapPlan(c, true, tc, to);
        bool none = true;
        for (const Obs &x : to) none &= !(x.row == 1 && x.word == 4) ? (x.v[0] & kRecordTap) == 0 : x.v[0] == kRecordTap;
        Ok(ok && to.size() == 5 && none && !PlugWritesDirectory(tc.data(), (ULONG)tc.size()),
           "tap plan to UAD MON: m0 word 4 = 4, every effect's tap cleared, nothing on the directory (SERVICE takes it)");
        TapPlan(c, false, tc, to);
        Ok(to.back().row == 22 && (to.back().v[0] & kRecordTap) && to.front().row == 1 && to.front().v[0] == 0,
           "tap plan back to UAD REC: the last effect's tap, m0 cleared");
        Words plan{ 0x00140005, L(11, 3), 0, 7, 8 }, out;
        Ok(RebaseCmd(plan, c.effects[2].base, out) && (out[1] >> 24) == c.effects[2].base + 1 && (out[1] & 0xFFFFFF) == 3 &&
           !RebaseCmd(Words{ 0x00140004, L(1, 16), 0, 9 }, 14, out),
           "a planner's plan moves to its effect's rows; one on the carrier rows is refused");
    }
    {
        std::vector<Bundle> st;
        ChainRecipe c;
        bool ok = SynthChain(s, kChainCases[1], st, c, err);
        std::map<RW, ULONG> sh = ShadowOf(c.cmd);
        Ok(ok && sh[{ 1, 4 }] == kRecordTap && !(sh[{ c.effects[2].base, 4 }] & kRecordTap), "UAD MON at CREATE: m0 tapped, no effect tapped");
    }
    {
        /* the delay plan (plan_compensation Medium) */
        ULONG r, m, cap;
        bool over;
        ChainDelays(64, false, r, m, cap, over);
        Ok(r == 136 && m == 0 && cap == 256 && !over, "delays: 64 samples of latency -> record 136, monitor 0, capacity 256");
        ChainDelays(64, true, r, m, cap, over);
        Ok(r == 200, "delays: UAD MON records ahead of the inserts: record 200");
        ChainDelays(300, false, r, m, cap, over);
        Ok(r == 0 && over, "delays: 300 samples is over the Medium budget: record 0, overloaded");
    }
    {
        /* five effects: refused; an aux return: Input Strip carrier, base 6, synch 0x12/0x13 */
        std::vector<Bundle> st;
        ChainRecipe c;
        ChainCase five = { "five", { 0, 0, 0, 0 }, 4, false, false, 200, 0, 256 };
        std::vector<ChainMember> m;
        Bundle b;
        ParseBundle(SynthBundle(s, kCases[0].name, "mono", 0, 60, false), kCases[0].file, b, err);
        for (ULONG k = 0; k < 5; k++) m.push_back({ &b, nullptr, k });
        ChainOptions o;
        Ok(!BuildChain(m, SynthReader(s), o, c, err), "five effects: refused (one to four)");
        ChainCase aux = { "aux", { 2, 2 }, 2, true, false, 200, 0, 256 };
        bool ok = SynthChain(s, aux, st, c, err, 1);
        std::map<RW, ULONG> sh = ShadowOf(c.cmd);
        Ok(ok && c.carrier == kInputCarrier && sh[{ 0, 7 }] == kInputCarrier && c.rows.size() == 6 + 2 * 4 && c.effects[0].base == 6 &&
           c.in1 == 0x12 && c.in2 == 0x13 && c.rows[1].dwords == 0x1e,
           "aux 2 chain: the plain Input Strip carrier (rows 1-5), effects from row 6, synch 0x12/0x13");
        ChainCase monAux = { "aux mon", { 2 }, 1, true, true, 200, 0, 256 };
        Ok(!SynthChain(s, monAux, st, c, err, 0), "an aux chain is always printed (no UAD MON)");
        (void)five;
    }
    {
        /* saved words go after the fresh startup and the readback expects them */
        std::vector<Bundle> st(2);
        std::map<ULONG, ULONG> diff{ { 6, 0x1234 } };
        ParseBundle(SynthBundle(s, kCases[0].name, "mono", 0, 60, false), kCases[0].file, st[0], err);
        ParseBundle(SynthBundle(s, kCases[1].name, "mono", 64, 60, true), kCases[1].file, st[1], err);
        std::vector<ChainMember> m{ { &st[0], nullptr, 0 }, { &st[1], &diff, 2 } };
        ChainRecipe c;
        ChainOptions o;
        bool ok = BuildChain(m, SynthReader(s), o, c, err);
        std::map<RW, ULONG> sh = ShadowOf(c.cmd);
        bool seen = false;
        for (const Obs &x : c.obs) {
            for (size_t k = 0; k < x.v.size(); k++) seen |= x.row == c.effects[1].base && x.word + k == 6 && x.v[k] == 0x1234;
        }
        Ok(ok && sh[{ c.effects[1].base, 6 }] == 0x1234 && seen && c.recordDelay == 136 && c.latency == 64 &&
           c.effects[1].slot == 2, "saved words on slot 2's effect, readback expects them; record 200 - 64");
    }
    {
        /* Unison (structure only): the header is member 0 at head word 7, the driver's UNISON directory check */
        Bundle h, e;
        ParseBundle(SynthBundle(s, kCases[0].name, "mono", 0, 60, false), kCases[0].file, h, err);
        ParseBundle(SynthBundle(s, kCases[1].name, "mono", 0, 60, false), kCases[1].file, e, err);
        ChainRecipe c;
        ChainOptions o;
        Ok(!BuildUnisonChain({ &h, nullptr, 0 }, {}, SynthReader(s), o, c, err), "Unison: an ordinary map is not a Unison header");
        h.unison = true;
        bool ok = BuildUnisonChain({ &h, nullptr, 0 }, { { &e, nullptr, 0 } }, SynthReader(s), o, c, err);
        std::map<RW, ULONG> sh = ShadowOf(c.cmd);
        Ok(ok && c.unison && sh[{ 0, 5 }] == 2 && sh[{ 0, 7 }] == 2085 && sh[{ 0, 9 }] == 2085 && c.rows[0].dwords == 10 &&
           c.rows[1].link == L(0, 6) && (sh[{ c.effects[1].base, 4 }] & kRecordTap) && !(sh[{ 1, 4 }] & kRecordTap),
           "Unison + 1 insert: header at word 7 linked at 6, the insert at 9, REC tap on the insert; passes the driver's checks");
        o.mon = true;
        ok = BuildUnisonChain({ &h, nullptr, 0 }, { { &e, nullptr, 0 } }, SynthReader(s), o, c, err);
        sh = ShadowOf(c.cmd);
        Ok(ok && (sh[{ 1, 4 }] & kRecordTap) && !(sh[{ c.effects[1].base, 4 }] & kRecordTap), "Unison, UAD MON: the header carries the tap");
    }
}

/* ------------------------------------------------------------------ selftest */

/* [Chainfix] which mapped plug-ins Apollo Control may run (plugin_host.h Qualification): synthetic lists, then
   open-apollo's own native_known_good.json when the reference clone is next to this tree (GPL data, no UA bytes). */
static void QualSelfTest()
{
    Qualification f;
    QualFloor(f);
    Ok(!QualWhy(f, "UAD AKG BX 20", "stereo").empty() && !QualWhy(f, "UAD AKG BX 20", "mono").empty(),
       "chainfix: without any list the AKG BX 20 is refused in both layouts (the floor)");
    Ok(QualWhy(f, "UAD AKG BX 20", "stereo").find("Mac trace") != std::string::npos,
       "chainfix: the refusal says why (upstream's reason)");
    Ok(!QualWhy(f, "UAD Studio D Chorus", "mono").empty(), "chainfix: Studio D Chorus (hung DSP 1 upstream) refused by the floor");
    Ok(QualWhy(f, "UAD Precision De-Esser", "mono").empty() && QualWhy(f, "UAD Some Other Plug-in", "stereo").empty(),
       "chainfix: a plug-in on no list may run");

    Qualification q;
    QualFloor(q);
    std::string err;
    const char *kg = "{\"plugins\":[],\"excluded\":{\"UAD UA 1176LN Legacy\":\"hand-built legacy provider\",\"UAD Foo\":\"x\","
                     "\"UAD AKG BX 20\":\"other text\"},\"not_yet_supported\":{\"UAD Bar (mono)\":\"restore check fails\","
                     "\"UAD Baz\":\"load refused\"}}";
    Ok(QualKnownGood(kg, q, err), "chainfix: a known-good list parses");
    Ok(QualWhy(q, "UAD UA 1176LN Legacy", "mono").empty(), "chainfix: excluded upstream but proven here in mono: may run");
    Ok(QualWhy(q, "UAD UA 1176LN Legacy", "stereo").find("hand-built") != std::string::npos,
       "chainfix: the same plug-in in a layout not proven here: refused with upstream's reason");
    Ok(!QualWhy(q, "UAD Foo", "mono").empty() && !QualWhy(q, "UAD Baz", "stereo").empty(),
       "chainfix: excluded and not_yet_supported entries refused in both layouts");
    Ok(!QualWhy(q, "UAD Bar", "mono").empty() && QualWhy(q, "UAD Bar", "stereo").empty(),
       "chainfix: \"<name> (mono)\" refuses that layout only");
    Ok(QualWhy(q, "UAD AKG BX 20", "mono").find("Nov 16 2026") != std::string::npos,
       "chainfix: the floor's text is kept over the list's");
    QualProven("# UAD Foo | mono | a comment\nUAD Foo | stereo | ran on inputs 1-2\r\nUAD Baz | * |\nUAD Bar | quad | bad layout\nno bars\n", q);
    Ok(QualWhy(q, "UAD Foo", "stereo").empty() && !QualWhy(q, "UAD Foo", "mono").empty(),
       "chainfix: proven.txt lifts one layout; a comment line lifts nothing");
    Ok(QualWhy(q, "UAD Baz", "mono").empty() && QualWhy(q, "UAD Baz", "stereo").empty(), "chainfix: proven.txt '*' lifts both");
    Ok(!QualWhy(q, "UAD Bar", "mono").empty(), "chainfix: a malformed proven.txt line lifts nothing");
    Qualification z;
    Ok(!QualKnownGood("not json", z, err) && !QualKnownGood("{\"excluded\":[1]}", z, err),
       "chainfix: an unreadable list is refused (the floor stays)");

    /* open-apollo's own list, when the reference clone is found above this exe */
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0) {
        std::wstring d = exe;
        for (int up = 0; up < 6; up++) {
            size_t sl = d.find_last_of(L'\\');
            if (sl == std::wstring::npos) break;
            d.resize(sl);
            std::wstring f8 = d + L"\\reference\\open-apollo\\mixer-engine\\device_maps\\native_known_good.json";
            if (GetFileAttributesW(f8.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            int cch = WideCharToMultiByte(CP_UTF8, 0, f8.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string p8((size_t)std::max(cch, 1) - 1, '\0'), t;
            WideCharToMultiByte(CP_UTF8, 0, f8.c_str(), -1, p8.data(), cch, nullptr, nullptr);
            Qualification u;
            QualFloor(u);
            bool ok = FileText(p8, t) && QualKnownGood(t, u, err);
            Ok(ok && !QualWhy(u, "UAD AKG BX 20", "stereo").empty() && !QualWhy(u, "UAD Studio D Chorus", "mono").empty() &&
               QualWhy(u, "UAD UA 1176LN Legacy", "mono").empty() && QualWhy(u, "UAD Sphere Mic Collection 180", "stereo").empty() &&
               QualWhy(u, "UAD Lexicon 480L", "mono").empty() && !QualWhy(u, "UAD Sound Machine Wood Works", "mono").empty() &&
               QualWhy(u, "UAD Sound Machine Wood Works", "stereo").empty(),
               "chainfix: open-apollo's native_known_good.json: AKG BX 20 and Studio D Chorus refused, what ran here allowed, "
               "Wood Works refused in mono only");
            break;
        }
    }
}

static int SelfTest()
{
    Synth s = MakeModules();
    std::string err, la, lb;

    /* write(): the mask marks the skipped words between first and last */
    Words w;
    Write(w, 3, { { 5, 1 }, { 7, 2 }, { 9, 3 } });
    Ok(w.size() == 6 && w[0] == 0x00140006 && w[1] == ((3u << 24) | 5) && w[2] == 0x5 && w[3] == 1 && w[5] == 3,
       "write: {5, 7, 9} -> length 6, mask 0b101 (words 6 and 8 skipped)");
    w.clear();
    Ok(!Write(w, 0, { { 0, 1 }, { 33, 2 } }), "write: a span over 33 words is refused");
    /* compact(): groups of first..first+32 per row, rows ascending, base 10 */
    std::map<ULONG, ULONG> cw;
    for (ULONG k = 0; k < 40; k++) cw[k] = k;
    cw[(1u << 24) | 3] = 9;
    w.clear();
    Compact(w, cw);
    Ok(w.size() == (3 + 33) + (3 + 7) + (3 + 1) && (w[1] >> 24) == 10 && (w[37] & 0xFFFFFF) == 33 && (w[47] >> 24) == 11,
       "compact: 40 words -> 33 + 7, then row 11");
    /* the page cut: upstream's test, 25 blocks of 100 words -> 1000, 1000, 500 */
    Words big;
    for (ULONG k = 0; k < 25; k++) {
        big.push_back(0x00140000u | 100);
        big.push_back((10u << 24) | (k * 100));
        big.push_back(0);
        for (int t = 0; t < 97; t++) big.push_back((ULONG)t);
    }
    std::vector<std::pair<ULONG, ULONG>> pg;
    Ok(Pages(big, pg) && pg.size() == 3 && pg[0].second == 1000 && pg[1].second == 1000 && pg[2].second == 500,
       "pages: upstream's 1000, 1000, 500");

    /* golden recipes: p1_recipe.py on the same synthetic bundles (make-golden.py) */
    for (size_t k = 0; k < _countof(kCases); k++) {
        const SynthCase &g = kCases[k];
        Bundle b;
        Recipe r;
        RecipeOptions o;
        std::string text = SynthBundle(s, g.name, g.layout, g.latency, g.shadow, g.dyn);
        o.stereo = strcmp(g.layout, "stereo") == 0;
        o.fixedDelay = g.fixed;
        bool ok = ParseBundle(text, g.file, b, err) && BuildRecipe(b, SynthReader(s), o, r, err);
        std::string mine = ok ? RecipeText(r) : std::string();
        std::string golden;
        for (const char *const *l = kGoldenLines[k]; *l != nullptr; l++) golden += std::string(*l) + "\n";
        long d = ok ? FirstDiff(mine, golden, la, lb) : 0;
        if (!ok) printf("      %s\n", err.c_str());
        else if (d >= 0) printf("      line %ld: ours  %s\n      line %ld: p1_recipe.py  %s\n", d + 1, la.c_str(), d + 1, lb.c_str());
        std::string what = std::string("golden: ") + g.what + " = p1_recipe.py's text";
        Ok(ok && d < 0, what.c_str());
    }

    /* pieces of the built recipe */
    {
        Bundle b;
        Recipe r, r2;
        RecipeOptions o;
        ParseBundle(SynthBundle(s, "UAD Synthetic Test", "mono", 0, 60, true), "x.json", b, err);
        o.input = 5;
        Ok(BuildRecipe(b, SynthReader(s), o, r, err) && r.in1 == 5 && r.in2 == kSynchMono, "x8p input 6: synch 5 / 0x20 (the recipe is the same)");
        Ok(r.modules.size() == 3 && r.codeWords == 20 && r.dataWords == 20, "architecture 1 left out; code 20 words, data 12 + 8");
        bool skipped = true;
        for (const Obs &x : r.obs) {
            for (size_t m = 0; m < x.v.size(); m++) {
                ULONG word = x.word + (ULONG)m;
                if ((x.row == 10 && (word == 9 || word == 30 || word < 3 || word == 4)) || x.v[m] == kNone) skipped = false;
            }
        }
        Ok(skipped, "readback leaves out the dynamic refs, the header words and 0xFFFFFFFF");
        std::map<ULONG, ULONG> diff{ { 6, 0x1234 }, { (2u << 24) | 1, 0x55 } };
        o.diff = &diff;
        Ok(BuildRecipe(b, SynthReader(s), o, r2, err) && r2.cmd.size() == r.cmd.size() + 4 + 4 && r2.initialized[6] == 0x1234,
           "saved state: its WRITEs after the fresh startup, and the readback expects it");
        std::map<ULONG, ULONG> bad{ { 4, 1 } };
        o.diff = &bad;
        Ok(!BuildRecipe(b, SynthReader(s), o, r2, err), "saved state on a host-owned header word is refused");
        std::map<ULONG, ULONG> link{ { 3, 1 } };
        o.diff = &link;
        Ok(!BuildRecipe(b, SynthReader(s), o, r2, err), "saved state on a linked pointer word is refused");
        o.diff = nullptr;
        o.stereo = true;
        Ok(!BuildRecipe(b, SynthReader(s), o, r2, err), "a mono bundle is never built stereo");
        Bundle bs;
        ParseBundle(SynthBundle(s, "UAD Synthetic Stereo", "stereo", 0, 60, false), "y.json", bs, err);
        o.input = 3;
        Ok(!BuildRecipe(bs, SynthReader(s), o, r2, err), "stereo on an odd first input is refused");
        o.input = 6;
        Ok(BuildRecipe(bs, SynthReader(s), o, r2, err) && r2.in1 == 6 && r2.in2 == 7, "stereo pair 7/8: synch 6 / 7");
        Synth broken = s;
        broken.mods[0][40] ^= 1;
        o.stereo = false;
        o.input = 0;
        Ok(!BuildRecipe(b, SynthReader(broken), o, r2, err) && err.find("hash") != std::string::npos, "a module that is not its hash is refused");
    }

    /* [P7] the Twin X: input 1 = DSP input 0 as on the x8p, so its recipe is golden 0's text; input 2; the pair 1/2 only */
    {
        const PLUG_MODEL *twx = PlugModelFor(PLUG_MODEL_TWINX);
        const SynthCase &g = kCases[0];
        Bundle b, bs;
        Recipe r;
        RecipeOptions o;
        std::string golden;
        for (const char *const *l = kGoldenLines[0]; *l != nullptr; l++) golden += std::string(*l) + "\n";
        bool ok = ParseBundle(SynthBundle(s, g.name, g.layout, g.latency, g.shadow, g.dyn), g.file, b, err);
        o.unit = twx;
        o.stereo = strcmp(g.layout, "stereo") == 0;
        o.fixedDelay = g.fixed;
        ok = ok && !o.stereo && BuildRecipe(b, SynthReader(s), o, r, err) && FirstDiff(RecipeText(r), golden, la, lb) < 0;
        Ok(ok, "Twin X input 1: p1_recipe.py's text (golden 0), synch 0 / 0x20");
        o.input = 1;
        Ok(BuildRecipe(b, SynthReader(s), o, r, err) && r.in1 == 1 && r.in2 == kSynchMono, "Twin X input 2: synch 1 / 0x20");
        o.input = 2;
        Ok(!BuildRecipe(b, SynthReader(s), o, r, err), "Twin X input 3: refused (2 strip inputs; the talkback has none)");
        ParseBundle(SynthBundle(s, "UAD Synthetic Stereo", "stereo", 0, 60, false), "y.json", bs, err);
        o.stereo = true;
        o.input = 0;
        Ok(BuildRecipe(bs, SynthReader(s), o, r, err) && r.in1 == 0 && r.in2 == 1, "Twin X stereo pair 1/2: synch 0 / 1");
        o.input = 2;
        Ok(!BuildRecipe(bs, SynthReader(s), o, r, err), "Twin X stereo pair 3/4: refused");
    }

    /* readback groups and the DSP-owned decision */
    {
        std::vector<Obs> o{ { 10, 0, Words(16, 1) }, { 10, 16, Words(16, 1) }, { 10, 32, Words(16, 1) }, { 10, 48, Words(16, 1) },
                            { 10, 64, Words(2, 1) }, { 11, 0, Words(4, 1) }, { 10, 100, Words(3, 1) }, { 10, 90, Words(2, 1) } };
        size_t i = 0, g = 0;
        const size_t want[] = { 4, 5, 6, 7, 8 };
        bool ok = true;
        while (i < o.size() && g < 5) {
            size_t j = ReadGroupEnd(o, i);
            ok = ok && j == want[g] && ReadGroupSpan(o, i, j) <= 64;
            i = j;
            g++;
        }
        Ok(ok && i == o.size(), "readback groups: one row, ascending, at most 64 words (8 runs in 5 reads)");
        std::vector<Obs> one{ { 10, 4, { 1, 2, 3, 4 } } };
        ULONG first[4] = { 1, 9, 3, 7 }, second[4] = { 1, 8, 3, 7 };
        std::set<RW> owned{ { 10, 7 } }, moving;
        std::vector<RW> bad = GroupDiffs(one, 0, 1, first, nullptr, owned, nullptr);
        Ok(bad.size() == 1 && bad[0] == RW(10, 5), "a DSP-owned word is never compared; a wrong word is");
        bad = GroupDiffs(one, 0, 1, first, second, owned, &moving);
        Ok(bad.empty() && moving.count({ 10, 5 }), "a word that moves between two reads is DSP-owned, not a difference");
        second[1] = 9;
        moving.clear();
        bad = GroupDiffs(one, 0, 1, first, second, owned, &moving);
        Ok(bad.size() == 1 && moving.empty(), "the same wrong value twice stays a difference");
    }
    /* saved inserts (Apollo Control's plugins.json): round trip, damaged entries dropped, one per input */
    {
        Saved a;
        a.unit = "SN20000000000001";
        a.input = 2;
        a.name = "UAD Synthetic Test";
        a.file = "synthetic-test-mono-runtime-bundle.json";
        a.sha = std::string(64, 'a');
        a.values = { 7, 1 };
        a.texts = { "7 dB \"hot\"", "On" };
        a.diff = { { 6, 0x1234 }, { (2u << 24) | 1, 0x55 } };
        Saved b = a;
        b.input = 4;
        b.stereo = true;
        b.bypass = true;
        b.diff.clear();
        std::vector<Saved> back;
        std::string dir;
        bool ok = SavedFromJson(SavedToJson({ a, b }, "D:\\maps"), back, dir);
        Ok(ok && back.size() == 2 && dir == "D:\\maps" && back[0].unit == a.unit && back[0].input == 2 && back[0].values == a.values &&
           back[0].texts == a.texts && back[0].diff == a.diff && back[1].stereo && back[1].bypass && back[1].diff.empty(),
           "saved inserts: unit, input, plug-in, values, texts, control words and bypass survive the file");
        std::string bad = "{\"version\":1,\"mapsDir\":\"\",\"inserts\":[{\"unit\":\"u\",\"input\":9,\"name\":\"x\",\"bundle\":\"b.json\"},"
                          "{\"unit\":\"u\",\"input\":1,\"name\":\"x\",\"bundle\":\"..\\\\evil.json\"},"
                          "{\"unit\":\"u\",\"input\":3,\"stereo\":true,\"name\":\"x\",\"bundle\":\"b.json\"},"
                          "{\"unit\":\"u\",\"input\":0,\"name\":\"x\",\"bundle\":\"b.json\",\"diff\":{\"zz\":1}},"
                          "{\"unit\":\"u\",\"input\":0,\"name\":\"y\",\"bundle\":\"c.json\"}]}";
        ok = SavedFromJson(bad, back, dir);
        Ok(ok && back.size() == 1 && back[0].name == "x" && back[0].diff.empty(),
           "a damaged file: inputs past 8, paths, odd stereo pairs dropped; bad words reset; one insert per input");
        Ok(!SavedFromJson("not json", back, dir) && back.empty(), "an unreadable file restores nothing");
        /* the saved words go back on the DSP after the fresh startup (as a cold start rebuilds it) */
        Bundle bd;
        Recipe r;
        RecipeOptions o;
        ParseBundle(SynthBundle(s, "UAD Synthetic Test", "mono", 0, 60, false), a.file, bd, err);
        o.input = a.input;
        o.diff = &a.diff;
        ok = BuildRecipe(bd, SynthReader(s), o, r, err);
        bool seen = false;
        for (const Obs &x : r.obs) {
            for (size_t m = 0; m < x.v.size(); m++) seen |= x.row == 10 && x.word + m == 6 && x.v[m] == 0x1234;
        }
        Ok(ok && r.in1 == 2 && seen, "a saved insert rebuilds on its input with its words, and the readback expects them");
    }
    ChainSelfTest(s);                   /* [Ins] */
    QualSelfTest();                     /* [Chainfix] */
    printf("\nselftest: %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

/* ------------------------------------------------------------------ [Ins] a chain of real bundles in text */

static int ChainCmd(int n, char **a)
{
    std::string store, specs, owned, expect, layout = "mono", err, la, lb;
    std::vector<std::string> files;
    ChainOptions o;
    for (int k = 1; k < n; k++) {
        std::string f = a[k];
        if (f == "--store" && k + 1 < n) store = a[++k];
        else if (f == "--layout" && k + 1 < n) layout = a[++k];
        else if (f == "--input" && k + 1 < n) o.input = strtoul(a[++k], nullptr, 10);
        else if (f == "--aux" && k + 1 < n) o.aux = (LONG)strtoul(a[++k], nullptr, 10);
        else if (f == "--mon") o.mon = true;
        else if (f == "--specs" && k + 1 < n) specs = a[++k];
        else if (f == "--owned" && k + 1 < n) owned = a[++k];
        else if (f == "--expect" && k + 1 < n) expect = a[++k];
        else if (f == "--delays" && k + 3 < n) {
            o.fixedDelays = true;
            o.recordDelay = strtoul(a[++k], nullptr, 10);
            o.monitorDelay = strtoul(a[++k], nullptr, 10);
            o.capacity = strtoul(a[++k], nullptr, 10);
        } else files.push_back(f);
    }
    o.stereo = layout == "stereo";
    o.specsDir = specs;
    o.ownedList = owned;
    std::vector<Bundle> bs(files.size());
    std::vector<ChainMember> m;
    for (size_t k = 0; k < files.size(); k++) {
        std::string t, file = files[k];
        size_t sl = file.find_last_of("\\/");
        if (!FileText(files[k], t) || !ParseBundle(t, sl == std::string::npos ? file : file.substr(sl + 1), bs[k], err)) {
            printf("FAIL  %s: %s\n", files[k].c_str(), err.c_str());
            return 1;
        }
        m.push_back({ &bs[k], nullptr, (ULONG)k });
    }
    ModuleReader rd = [&](const std::string &sha, std::vector<UCHAR> &bytes) {
        std::string t;
        if (!FileText(store + "\\" + sha + ".bill", t)) return false;
        bytes.assign(t.begin(), t.end());
        return true;
    };
    ChainRecipe c;
    if (!BuildChain(m, rd, o, c, err)) {
        printf("FAIL  chain: %s\n", err.c_str());
        return 1;
    }
    std::string text = ChainText(c);
    if (expect.empty()) {
        fputs(text.c_str(), stdout);
        return 0;
    }
    std::string want;
    if (!FileText(expect, want)) { printf("FAIL  cannot read %s\n", expect.c_str()); return 1; }
    long d = FirstDiff(text, want, la, lb);
    if (d >= 0) {
        printf("FAIL  line %ld differs\n      ours     %.200s\n      upstream %.200s\n", d + 1, la.c_str(), lb.c_str());
        return 1;
    }
    printf("PASS  %zu effects (%s%s): %zu lines identical (%zu startup words, %zu page(s), %zu rows, %zu observations)\n", c.effects.size(),
           layout.c_str(), o.mon ? ", UAD MON" : "", Lines(want).size(), c.cmd.size(), c.pages.size(), c.rows.size(), c.obs.size());
    return 0;
}

/* ------------------------------------------------------------------ compare with p1_recipe.py */

static int Compare(int n, char **a)
{
    std::string pyText, bundleText, err, store, specs, owned, layout = "mono", delay = "200", la, lb;
    ULONG input = 0;
    if (n < 4) { fprintf(stderr, "usage: compare <recipe> <bundle> --store DIR [--layout L] [--input N] [--record-delay 200|carrier] [--specs D] [--owned F]\n"); return 2; }
    for (int k = 3; k + 1 < n; k++) {
        std::string f = a[k];
        if (f == "--store") store = a[k + 1];
        else if (f == "--layout") layout = a[k + 1];
        else if (f == "--input") input = strtoul(a[k + 1], nullptr, 10);
        else if (f == "--record-delay") delay = a[k + 1];
        else if (f == "--specs") specs = a[k + 1];
        else if (f == "--owned") owned = a[k + 1];
    }
    if (!FileText(a[1], pyText) || !FileText(a[2], bundleText)) { fprintf(stderr, "cannot read %s or %s\n", a[1], a[2]); return 2; }
    std::string file = a[2];
    size_t sl = file.find_last_of("\\/");
    if (sl != std::string::npos) file = file.substr(sl + 1);
    Bundle b;
    Recipe r;
    RecipeOptions o;
    o.input = input;
    o.stereo = layout == "stereo";
    o.fixedDelay = delay != "carrier";
    o.delay = o.fixedDelay ? strtoul(delay.c_str(), nullptr, 10) : 0;
    o.specsDir = specs;
    o.ownedList = owned;
    ModuleReader rd = [&](const std::string &sha, std::vector<UCHAR> &bytes) {
        std::string t;
        if (!FileText(store + "\\" + sha + ".bill", t)) return false;
        bytes.assign(t.begin(), t.end());
        return true;
    };
    if (!ParseBundle(bundleText, file, b, err) || !BuildRecipe(b, rd, o, r, err)) {
        printf("FAIL  %s: %s\n", file.c_str(), err.c_str());
        return 1;
    }
    long d = FirstDiff(RecipeText(r), pyText, la, lb);
    if (d >= 0) {
        printf("FAIL  %s (%s, input %lu, delay %s): line %ld differs\n      ours          %.200s\n      p1_recipe.py  %.200s\n", file.c_str(),
               layout.c_str(), input, delay.c_str(), d + 1, la.c_str(), lb.c_str());
        return 1;
    }
    printf("PASS  %s (%s, input %lu, delay %s): %zu lines identical (%zu startup words, %zu page(s), %zu readback runs, %zu DSP-owned)\n",
           file.c_str(), layout.c_str(), input, delay.c_str(), Lines(pyText).size(), r.cmd.size(), r.pages.size(), r.obs.size(), r.owned.size());
    return 0;
}

/* catalog <plugins dir>: the picker's catalogue over every mapped bundle, as Apollo Control builds it (timing, counts) */
static int Catalog(const std::string &dir)
{
    std::string t, err;
    std::vector<IndexEntry> idx;
    if (!FileText(dir + "\\bundles\\index.json", t) || !ParseIndex(t, idx, err)) { printf("FAIL  index: %s\n", err.c_str()); return 1; }
    ULONGLONG t0 = GetTickCount64();
    ULONG ok = 0, bad = 0, staged = 0, maxCode = 0;
    for (const IndexEntry &e : idx) {
        Bundle b;
        CatalogItem c;
        if (!FileText(dir + "\\bundles\\" + e.file, t) || !ParseBundle(t, e.file, b, err)) { bad++; printf("      %s: %s\n", e.file.c_str(), err.c_str()); continue; }
        CatalogFrom(b, c);
        if (!c.ok) { bad++; printf("      %s: %s\n", e.file.c_str(), c.why.c_str()); continue; }
        ok++;
        staged += c.pages > 1;
        maxCode = std::max(maxCode, c.codeWords);
    }
    printf("%s  catalogue: %zu bundles, %lu built, %lu refused, %lu staged, largest code %lu words, %llu ms\n", bad ? "FAIL" : "PASS",
           idx.size(), ok, bad, staged, maxCode, GetTickCount64() - t0);
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "selftest") == 0) return SelfTest();
    if (argc >= 3 && strcmp(argv[1], "catalog") == 0) return Catalog(argv[2]);
    if (argc >= 2 && strcmp(argv[1], "compare") == 0) return Compare(argc - 1, argv + 1);
    if (argc >= 2 && strcmp(argv[1], "chain") == 0) return ChainCmd(argc - 1, argv + 1);
    if (argc >= 2 && strcmp(argv[1], "synth") == 0 && argc >= 3) {
        /* make-golden.py: write the synthetic bundles and modules p1_recipe.py reads */
        Synth s = MakeModules();
        std::string dir = argv[2];
        CreateDirectoryA((dir + "\\bundles").c_str(), nullptr);
        CreateDirectoryA((dir + "\\store").c_str(), nullptr);
        CreateDirectoryA((dir + "\\store\\modules").c_str(), nullptr);
        for (size_t i = 0; i < s.mods.size(); i++) {
            FILE *f = nullptr;
            if (fopen_s(&f, (dir + "\\store\\modules\\" + s.shas[i] + ".bill").c_str(), "wb") == 0 && f) {
                fwrite(s.mods[i].data(), 1, s.mods[i].size(), f);
                fclose(f);
            }
        }
        FILE *list = nullptr;
        fopen_s(&list, (dir + "\\cases.txt").c_str(), "wb");
        for (const SynthCase &c : kCases) {
            FILE *f = nullptr;
            std::string t = SynthBundle(s, c.name, c.layout, c.latency, c.shadow, c.dyn);
            if (fopen_s(&f, (dir + "\\bundles\\" + c.file).c_str(), "wb") == 0 && f) {
                fwrite(t.data(), 1, t.size(), f);
                fclose(f);
            }
            if (list) fprintf(list, "%s|%s|%s|%s\n", c.name, c.layout, c.file, c.fixed ? "200" : "carrier");
        }
        if (list) fclose(list);
        /* [Ins] the chain cases (make-golden-chain.py): members, layout, MON, delays */
        FILE *chains = nullptr;
        fopen_s(&chains, (dir + "\\chains.txt").c_str(), "wb");
        for (const ChainCase &g : kChainCases) {
            if (!chains) break;
            for (int k = 0; k < g.n; k++) fprintf(chains, "%s%s", k ? "," : "", kCases[g.members[k]].file);
            fprintf(chains, "|%s|%d|%lu|%lu|%lu\n", g.stereo ? "stereo" : "mono", g.mon ? 1 : 0, g.record, g.monitor, g.capacity);
        }
        if (chains) fclose(chains);
        printf("synthetic bundles and modules in %s\n", dir.c_str());
        return 0;
    }
    fprintf(stderr, "usage: plugin-recipe-test selftest | compare ... | synth DIR\n");
    return 2;
}
