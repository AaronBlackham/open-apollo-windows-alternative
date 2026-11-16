/*
 * agg_json.h - Apollo Control's side of the combined device (gate G6, docs/apollo-control-units.md
 * section 2): the Units & routing configuration as JSON <-> the driver's APOLLO_AGG_CONFIG
 * (include/apollo_agg.h). Header-only C++17, no Windows UI code, so tools/agg-test/aggjson-test.cpp
 * checks it offline.
 *
 *   AggCompile(json, cfg, err)  the "config" object -> APOLLO_AGG_CONFIG (the JSON text itself is kept
 *                               in cfg.Json, verbatim, for the round trip)
 *   AggDescribe(cfg, units)     a config object for a driver configuration that carries no JSON (the
 *                               driver's default, or one set by agg-test / the ASIO driver)
 *
 * Unit ids are the driver's unit keys: HW_UID as 16 upper-case hex digits. Ports and channel IDs
 * per model follow the table in docs/apollo-control-units.md section 2.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstring>
#include <cstdio>
#include <cwchar>

namespace aggjson {

/* ---------------------------------------------------------------- JSON */

struct Value {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    bool b = false;
    double n = 0;
    std::string s;                                  /* UTF-8 */
    std::vector<Value> a;
    std::vector<std::pair<std::string, Value>> o;   /* in order */
    size_t begin = 0, end = 0;                      /* the value's text in the source */

    const Value *Get(const char *key) const
    {
        if (kind != Object) return nullptr;
        for (const auto &kv : o) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
    std::string Str(const char *key, const char *def = "") const
    {
        const Value *v = Get(key);
        return (v && v->kind == String) ? v->s : std::string(def);
    }
    double Num(const char *key, double def = 0) const
    {
        const Value *v = Get(key);
        return (v && v->kind == Number) ? v->n : def;
    }
};

class Parser {
public:
    explicit Parser(const std::string &t) : t_(t) {}
    bool Parse(Value &out)
    {
        Ws();
        if (!ParseValue(out, 0)) return false;
        Ws();
        return p_ == t_.size();
    }
private:
    const std::string &t_;
    size_t p_ = 0;
    void Ws()
    {
        while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\t' || t_[p_] == '\n' || t_[p_] == '\r')) p_++;
    }
    bool Lit(const char *w)
    {
        size_t n = strlen(w);
        if (t_.compare(p_, n, w) != 0) return false;
        p_ += n;
        return true;
    }
    static void PutUtf8(std::string &s, unsigned cp)
    {
        if (cp < 0x80) s += (char)cp;
        else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
        else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    }
    bool Hex4(unsigned &v)
    {
        v = 0;
        for (int i = 0; i < 4; i++) {
            if (p_ >= t_.size()) return false;
            char c = t_[p_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool ParseString(std::string &s)
    {
        if (p_ >= t_.size() || t_[p_] != '"') return false;
        p_++;
        while (p_ < t_.size()) {
            char c = t_[p_++];
            if (c == '"') return true;
            if ((unsigned char)c < 0x20) return false;
            if (c != '\\') { s += c; continue; }
            if (p_ >= t_.size()) return false;
            char e = t_[p_++];
            switch (e) {
            case '"': s += '"'; break;
            case '\\': s += '\\'; break;
            case '/': s += '/'; break;
            case 'b': s += '\b'; break;
            case 'f': s += '\f'; break;
            case 'n': s += '\n'; break;
            case 'r': s += '\r'; break;
            case 't': s += '\t'; break;
            case 'u': {
                unsigned cp;
                if (!Hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && p_ + 1 < t_.size() && t_[p_] == '\\' && t_[p_ + 1] == 'u') {
                    unsigned lo;
                    p_ += 2;
                    if (!Hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                PutUtf8(s, cp);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }
    bool ParseValue(Value &v, int depth)
    {
        if (depth > 32 || p_ >= t_.size()) return false;
        v.begin = p_;
        char c = t_[p_];
        bool ok = true;
        if (c == '{') {
            v.kind = Value::Object;
            p_++;
            Ws();
            if (p_ < t_.size() && t_[p_] == '}') { p_++; }
            else {
                for (;;) {
                    std::string k;
                    Value x;
                    Ws();
                    if (!ParseString(k)) return false;
                    Ws();
                    if (p_ >= t_.size() || t_[p_++] != ':') return false;
                    Ws();
                    if (!ParseValue(x, depth + 1)) return false;
                    v.o.emplace_back(std::move(k), std::move(x));
                    Ws();
                    if (p_ < t_.size() && t_[p_] == ',') { p_++; continue; }
                    if (p_ < t_.size() && t_[p_] == '}') { p_++; break; }
                    return false;
                }
            }
        } else if (c == '[') {
            v.kind = Value::Array;
            p_++;
            Ws();
            if (p_ < t_.size() && t_[p_] == ']') { p_++; }
            else {
                for (;;) {
                    Value x;
                    Ws();
                    if (!ParseValue(x, depth + 1)) return false;
                    v.a.push_back(std::move(x));
                    Ws();
                    if (p_ < t_.size() && t_[p_] == ',') { p_++; continue; }
                    if (p_ < t_.size() && t_[p_] == ']') { p_++; break; }
                    return false;
                }
            }
        } else if (c == '"') {
            v.kind = Value::String;
            ok = ParseString(v.s);
        } else if (Lit("true")) {
            v.kind = Value::Bool;
            v.b = true;
        } else if (Lit("false")) {
            v.kind = Value::Bool;
        } else if (Lit("null")) {
            v.kind = Value::Null;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            size_t s = p_;
            if (t_[p_] == '-') p_++;
            while (p_ < t_.size() && ((t_[p_] >= '0' && t_[p_] <= '9') || t_[p_] == '.' || t_[p_] == 'e' ||
                                      t_[p_] == 'E' || t_[p_] == '+' || t_[p_] == '-')) p_++;
            v.kind = Value::Number;
            v.n = strtod(t_.substr(s, p_ - s).c_str(), nullptr);
        } else {
            return false;
        }
        v.end = p_;
        return ok;
    }
};

/* ---------------------------------------------------------------- units and ports */

static inline bool ParseUid(const std::string &id, unsigned long long &uid)
{
    if (id.size() != 16) return false;
    uid = 0;
    for (char c : id) {
        uid <<= 4;
        if (c >= '0' && c <= '9') uid |= (unsigned)(c - '0');
        else if (c >= 'A' && c <= 'F') uid |= (unsigned)(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f') uid |= (unsigned)(c - 'a' + 10);
        else return false;
    }
    return uid != 0;
}

static inline std::string UidText(unsigned long long uid)
{
    char b[24];
    snprintf(b, sizeof(b), "%016llX", uid);
    return b;
}

static inline unsigned long ModelOfType(double deviceType)
{
    return deviceType == 0x22 ? APOLLO_INFO_MODEL_X8P : (deviceType == 0x99 ? APOLLO_INFO_MODEL_TWINX : 0);
}

/* A port's two playback channel IDs (docs/apollo-control-units.md section 2), 0 = unknown. */
static inline bool PortIds(unsigned long model, const std::string &port, unsigned short &l, unsigned short &r)
{
    l = r = 0;
    if (port == "mon") { l = 0x0901; r = 0x0902; }
    else if (model == APOLLO_INFO_MODEL_TWINX && port == "line3-4") { l = 0x0A03; r = 0x0A04; }
    else if (model == APOLLO_INFO_MODEL_TWINX && port == "hp") { l = 0x0A01; r = 0x0A02; }
    else if (model != APOLLO_INFO_MODEL_TWINX && port == "hp1") { l = 0x0A01; r = 0x0A02; }
    else if (model != APOLLO_INFO_MODEL_TWINX && port == "hp2") { l = 0x0A03; r = 0x0A04; }
    else if (model != APOLLO_INFO_MODEL_TWINX && port.size() == 7 && port.compare(0, 4, "line") == 0 &&
             port[5] == '-' && port[4] >= '1' && port[4] <= '7' && port[6] == port[4] + 1 && ((port[4] - '0') & 1)) {
        l = (unsigned short)(0x0300 + (port[4] - '0'));
        r = (unsigned short)(l + 1);
    }
    return l != 0;
}

static inline const char *PortOf(unsigned long model, unsigned short l)
{
    static char b[16];
    if (l == 0x0901) return "mon";
    if (model == APOLLO_INFO_MODEL_TWINX) return l == 0x0A03 ? "line3-4" : (l == 0x0A01 ? "hp" : nullptr);
    if (l == 0x0A01) return "hp1";
    if (l == 0x0A03) return "hp2";
    if (l >= 0x0301 && l <= 0x0307 && (l & 1)) {
        snprintf(b, sizeof(b), "line%u-%u", (unsigned)(l & 0xFF), (unsigned)(l & 0xFF) + 1);
        return b;
    }
    return nullptr;
}

/* ---------------------------------------------------------------- JSON -> driver */

/*
 * Json: the whole UI message or just the config object; Config: the "config" value inside it (or
 * the object itself). Returns false with Err set (text for the UI's error.detail).
 */
static inline bool AggCompile(const std::string &ConfigText, APOLLO_AGG_CONFIG &C, std::string &Err)
{
    Value v;
    Parser p(ConfigText);

    memset(&C, 0, sizeof(C));
    if (!p.Parse(v) || v.kind != Value::Object) {
        Err = "the configuration is not a JSON object";
        return false;
    }
    if (ConfigText.size() > APOLLO_AGG_JSON_MAX) {
        Err = "the configuration is larger than the driver keeps (16 KiB)";
        return false;
    }
    C.Version = APOLLO_AGG_VERSION;
    C.Bytes = sizeof(C);
    C.Flags = APOLLO_AGG_CFG_ENABLED | APOLLO_AGG_CFG_ALIGN;
    const Value *en = v.Get("enabled");
    if (en && en->kind == Value::Bool && !en->b) C.Flags &= ~APOLLO_AGG_CFG_ENABLED;
    /* [G7] "align": false = lowest latency, unaligned (spec 6.3.5); absent or true = aligned, as before */
    const Value *al = v.Get("align");
    if (al && al->kind == Value::Bool && !al->b) C.Flags &= ~APOLLO_AGG_CFG_ALIGN;
    std::string q = v.Str("asrcQuality", "default");
    C.AsrcHalf = q == "low" ? 8 : (q == "high" ? 32 : 16);

    const Value *units = v.Get("units");
    if (!units || units->kind != Value::Array || units->a.empty() || units->a.size() > APOLLO_AGG_MAX_UNITS) {
        Err = "units: 1 to 4 units expected";
        return false;
    }
    std::map<std::string, unsigned> index;
    for (const Value &u : units->a) {
        unsigned long long uid;
        std::string id = u.Str("id");
        if (!ParseUid(id, uid)) {
            Err = "unit id \"" + id + "\" is not a driver unit key (16 hex digits)";
            return false;
        }
        if (index.count(id)) {
            Err = "unit " + id + " is listed twice";
            return false;
        }
        index[id] = C.UnitCount;
        C.Unit[C.UnitCount].Uid = uid;
        C.Unit[C.UnitCount].Model = ModelOfType(u.Num("deviceType"));
        C.UnitCount++;
    }
    std::string master = v.Str("master");
    if (!index.count(master)) {
        Err = "the master \"" + master + "\" is not one of the units";
        return false;
    }
    C.Master = index[master];

    const Value *ch = v.Get("channels");
    if (ch && ch->kind == Value::Object) {
        C.Flags |= APOLLO_AGG_CFG_EXPLICIT;
        for (int dir = 0; dir < 2; dir++) {
            const Value *list = ch->Get(dir == 0 ? "in" : "out");
            APOLLO_AGG_CHAN_CFG *dst = dir == 0 ? C.In : C.Out;
            ULONG &n = dir == 0 ? C.InCount : C.OutCount;
            if (!list || list->kind != Value::Array) continue;
            for (const Value &e : list->a) {
                std::string id = e.Str("unit");
                double slot = e.Num("slot", -1);
                if (!index.count(id) || slot < 0 || slot >= APOLLO_AGG_UNIT_SLOTS || n >= APOLLO_AGG_MAX_CH) {
                    Err = "channels: an entry names an unknown unit or slot";
                    return false;
                }
                const Value *h = e.Get("hidden");
                dst[n].Unit = (UCHAR)index[id];
                dst[n].Slot = (USHORT)slot;
                dst[n].Flags = (h && h->kind == Value::Bool && h->b) ? APOLLO_AGG_CHAN_HIDDEN : 0;
                n++;
            }
        }
    }

    const Value *routes = v.Get("routes");
    if (routes && routes->kind == Value::Array) {
        for (const Value &r : routes->a) {
            APOLLO_AGG_ROUTE_CFG &o = C.Route[C.RouteCount];
            std::string src = r.Str("src"), dst = r.Str("unit");
            const Value *ids = r.Get("ids");
            if (C.RouteCount >= APOLLO_AGG_MAX_ROUTES) {
                Err = "more than 16 routes";
                return false;
            }
            if (!index.count(dst)) {
                Err = "a route goes to an unknown unit";
                return false;
            }
            o.DstUnit = (UCHAR)index[dst];
            if (ids && ids->kind == Value::Array && ids->a.size() == 2 && ids->a[0].kind == Value::Number &&
                ids->a[1].kind == Value::Number) {
                o.DstId[0] = (USHORT)ids->a[0].n;
                o.DstId[1] = (USHORT)ids->a[1].n;
            } else if (!PortIds(C.Unit[o.DstUnit].Model, r.Str("port"), o.DstId[0], o.DstId[1])) {
                Err = "a route has neither ids nor a known port";
                return false;
            }
            if (src == "daw") {
                o.SrcKind = APOLLO_AGG_SRC_DAW;
            } else if (src == "windows") {
                o.SrcKind = APOLLO_AGG_SRC_WINDOWS;
            } else {
                size_t colon = src.find(':');
                std::string su = colon == std::string::npos ? std::string() : src.substr(0, colon);
                if (!index.count(su)) {
                    Err = "route source \"" + src + "\" is not daw, windows or <unit>:<port>";
                    return false;
                }
                o.SrcKind = APOLLO_AGG_SRC_PORT;
                o.SrcUnit = (UCHAR)index[su];
                if (!PortIds(C.Unit[o.SrcUnit].Model, src.substr(colon + 1), o.SrcId[0], o.SrcId[1])) {
                    Err = "route source port \"" + src.substr(colon + 1) + "\" is not known for that model";
                    return false;
                }
            }
            C.RouteCount++;
        }
    }
    memcpy(C.Json, ConfigText.data(), ConfigText.size());
    C.JsonBytes = (ULONG)ConfigText.size();
    return true;
}

/* ---------------------------------------------------------------- driver -> JSON */

static inline void Esc(std::string &out, const std::string &s)
{
    out += '"';
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", (unsigned)c); out += b; }
        else out += c;
    }
    out += '"';
}

/* S as a JSON string (Esc); Apollo Control's host headers quote every UTF-8 string with this one. */
static inline std::string Quote(const std::string &s)
{
    std::string o;
    Esc(o, s);
    return o;
}

/* What the UI knows about a unit (serial, model name), by unit key; may be empty. */
struct UnitText {
    std::string model, serial;
};

static inline std::string AggDescribe(const APOLLO_AGG_CONFIG &C, const std::map<std::string, UnitText> &Known)
{
    std::string j = "{\"version\":1,\"units\":[";
    for (ULONG u = 0; u < C.UnitCount && u < APOLLO_AGG_MAX_UNITS; u++) {
        std::string id = UidText(C.Unit[u].Uid);
        auto k = Known.find(id);
        std::string model = (k != Known.end() && !k->second.model.empty()) ? k->second.model :
                            (C.Unit[u].Model == APOLLO_INFO_MODEL_X8P ? "Apollo x8p" :
                             (C.Unit[u].Model == APOLLO_INFO_MODEL_TWINX ? "Apollo Twin X" : "Apollo"));
        char t[64];
        if (u) j += ',';
        j += "{\"id\":";
        Esc(j, id);
        j += ",\"model\":";
        Esc(j, model);
        snprintf(t, sizeof(t), ",\"deviceType\":%u,\"serial\":", C.Unit[u].Model == APOLLO_INFO_MODEL_X8P ? 0x22u :
                 (C.Unit[u].Model == APOLLO_INFO_MODEL_TWINX ? 0x99u : 0u));
        j += t;
        Esc(j, k != Known.end() ? k->second.serial : std::string());
        j += '}';
    }
    j += "],\"master\":";
    Esc(j, C.Master < C.UnitCount ? UidText(C.Unit[C.Master].Uid) : std::string());
    if (C.Flags & APOLLO_AGG_CFG_EXPLICIT) {
        j += ",\"channels\":{";
        for (int dir = 0; dir < 2; dir++) {
            const APOLLO_AGG_CHAN_CFG *e = dir == 0 ? C.In : C.Out;
            ULONG n = dir == 0 ? C.InCount : C.OutCount;
            j += dir == 0 ? "\"in\":[" : ",\"out\":[";
            for (ULONG i = 0; i < n && i < APOLLO_AGG_MAX_CH; i++) {
                char t[64];
                if (i) j += ',';
                j += "{\"unit\":";
                Esc(j, e[i].Unit < C.UnitCount ? UidText(C.Unit[e[i].Unit].Uid) : std::string());
                snprintf(t, sizeof(t), ",\"slot\":%u,\"hidden\":%s}", (unsigned)e[i].Slot,
                         (e[i].Flags & APOLLO_AGG_CHAN_HIDDEN) ? "true" : "false");
                j += t;
            }
            j += ']';
        }
        j += '}';
    } else {
        j += ",\"channels\":null";
    }
    /* the preset label, from the routes */
    int daw = 0, other = 0;
    bool dawToTwx = false, winToHp = false;
    for (ULONG r = 0; r < C.RouteCount && r < APOLLO_AGG_MAX_ROUTES; r++) {
        if (C.Route[r].SrcKind == APOLLO_AGG_SRC_DAW) {
            daw++;
            dawToTwx = dawToTwx || C.Unit[C.Route[r].DstUnit].Model == APOLLO_INFO_MODEL_TWINX;
        } else if (C.Route[r].SrcKind == APOLLO_AGG_SRC_WINDOWS && C.Route[r].DstId[0] == 0x0A01) {
            winToHp = true;
        } else {
            other++;
        }
    }
    const char *preset = (other == 0 && daw == 1 && dawToTwx) ? (winToHp ? "phones" : "studio") :
                         ((other == 0 && daw >= 2) ? "mirror" : "custom");
    j += ",\"preset\":\"";
    j += preset;
    j += "\",\"routes\":[";
    for (ULONG r = 0; r < C.RouteCount && r < APOLLO_AGG_MAX_ROUTES; r++) {
        const APOLLO_AGG_ROUTE_CFG &o = C.Route[r];
        unsigned long dm = o.DstUnit < C.UnitCount ? C.Unit[o.DstUnit].Model : 0;
        const char *dport = PortOf(dm, o.DstId[0]);
        char t[64];
        if (r) j += ',';
        j += "{\"src\":";
        if (o.SrcKind == APOLLO_AGG_SRC_DAW) Esc(j, "daw");
        else if (o.SrcKind == APOLLO_AGG_SRC_WINDOWS) Esc(j, "windows");
        else {
            const char *sp = PortOf(o.SrcUnit < C.UnitCount ? C.Unit[o.SrcUnit].Model : 0, o.SrcId[0]);
            Esc(j, (o.SrcUnit < C.UnitCount ? UidText(C.Unit[o.SrcUnit].Uid) : std::string()) + ":" + (sp ? sp : "?"));
        }
        j += ",\"unit\":";
        Esc(j, o.DstUnit < C.UnitCount ? UidText(C.Unit[o.DstUnit].Uid) : std::string());
        j += ",\"port\":";
        Esc(j, dport ? dport : "");
        snprintf(t, sizeof(t), ",\"ids\":[%u,%u]}", (unsigned)o.DstId[0], (unsigned)o.DstId[1]);
        j += t;
    }
    j += "],\"asrcQuality\":";
    Esc(j, C.AsrcHalf == 8 ? "low" : (C.AsrcHalf == 32 ? "high" : "default"));
    j += (C.Flags & APOLLO_AGG_CFG_ALIGN) ? ",\"align\":true" : ",\"align\":false";     /* [G7] */
    j += (C.Flags & APOLLO_AGG_CFG_ENABLED) ? ",\"enabled\":true}" : ",\"enabled\":false}";
    return j;
}

/* The "config" value's text inside a UI message {"cmd":"agg-set","config":{...}} ("" if none). */
static inline std::string ConfigTextOf(const std::string &Message)
{
    Value v;
    Parser p(Message);
    if (!p.Parse(v) || v.kind != Value::Object) return {};
    const Value *c = v.Get("config");
    if (!c || c->kind != Value::Object) return {};
    return Message.substr(c->begin, c->end - c->begin);
}

}   /* namespace aggjson */
