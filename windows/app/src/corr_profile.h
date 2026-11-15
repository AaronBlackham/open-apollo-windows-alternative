/*
 * corr_profile.h - Apollo Control's side of speaker correction (include\apollo_corr.h): the Avium
 * correction profile (Avium Measure docs\correction-profile.md, version 1) read from its JSON text,
 * checked by a port of Avium Measure's validate() (avium_measure\core\profile.py), and compiled for
 * the driver at the unit's sample rate. Header-only C++17 with no Windows UI code, so
 * tools\corr-test\corrprofile-test.cpp checks it offline against Avium Measure's own verdicts.
 *
 *   ParseJson(text, value, err)       JSON as Python's json.loads reads it (NaN / Infinity accepted,
 *                                     integers told from floats by their text, a repeated key keeps the
 *                                     last value at the first key's place)
 *   Validate(value)                   validate(): every problem, the same text in the same order
 *   Design(type, f, gain, q, fs)      dsp\biquad.py design(): the RBJ cookbook and first-order forms
 *   Compile(value, rate, slots, ...)  -> APOLLO_CORR_PROFILE: the coefficients for that rate (the file's,
 *                                     or designed from the parameters when the file has none for it),
 *                                     linear preamp and trims, delays in microseconds, each channel on the
 *                                     playback slot the user chose
 *   StateJson(state)                  the driver's APOLLO_CORR_STATE as JSON for the page
 *
 * Where validate() itself would raise instead of answering (a non-numeric gain or Q on a type that
 * does not use it, a coefficient rate that is not a positive number), this port reports a problem:
 * the profile is refused either way.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#ifndef APOLLO_CORR_VERSION
#include "..\..\include\apollo_corr.h"
#endif

namespace corrprof {

/* ---------------------------------------------------------------- JSON, as Python reads it */

struct J {
    enum Kind { Null, Bool, Int, Float, Str, Arr, Obj } kind = Null;
    bool b = false;
    double n = 0;                                   /* Int and Float */
    std::string s;                                  /* Str (UTF-8); Int: its digits */
    std::vector<J> a;
    std::vector<std::pair<std::string, J>> o;       /* keys once each, in first-seen order */

    const J *Get(const char *key) const
    {
        if (kind != Obj) return nullptr;
        for (const auto &kv : o) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
    bool IsNum() const { return kind == Int || kind == Float; }
};

class Parser {
public:
    explicit Parser(const std::string &t) : t_(t) {}
    bool Parse(J &out, std::string &err)
    {
        Ws();
        if (!Value(out, 0)) {
            err = err_.empty() ? "malformed JSON at offset " + std::to_string(p_) : err_;
            return false;
        }
        Ws();
        if (p_ != t_.size()) {
            err = "extra data at offset " + std::to_string(p_);
            return false;
        }
        return true;
    }
private:
    const std::string &t_;
    size_t p_ = 0;
    std::string err_;
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
    static void Utf8(std::string &s, unsigned cp)
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
    bool Str(std::string &s)
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
                Utf8(s, cp);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }
    bool Value(J &v, int depth)
    {
        if (depth > 64 || p_ >= t_.size()) return false;
        char c = t_[p_];
        if (c == '{') {
            v.kind = J::Obj;
            p_++;
            Ws();
            if (p_ < t_.size() && t_[p_] == '}') { p_++; return true; }
            for (;;) {
                std::string k;
                J x;
                Ws();
                if (!Str(k)) return false;
                Ws();
                if (p_ >= t_.size() || t_[p_++] != ':') return false;
                Ws();
                if (!Value(x, depth + 1)) return false;
                bool dup = false;
                for (auto &kv : v.o) {
                    if (kv.first == k) { kv.second = std::move(x); dup = true; break; }   /* Python: last value, first place */
                }
                if (!dup) v.o.emplace_back(std::move(k), std::move(x));
                Ws();
                if (p_ < t_.size() && t_[p_] == ',') { p_++; continue; }
                if (p_ < t_.size() && t_[p_] == '}') { p_++; return true; }
                return false;
            }
        }
        if (c == '[') {
            v.kind = J::Arr;
            p_++;
            Ws();
            if (p_ < t_.size() && t_[p_] == ']') { p_++; return true; }
            for (;;) {
                J x;
                Ws();
                if (!Value(x, depth + 1)) return false;
                v.a.push_back(std::move(x));
                Ws();
                if (p_ < t_.size() && t_[p_] == ',') { p_++; continue; }
                if (p_ < t_.size() && t_[p_] == ']') { p_++; return true; }
                return false;
            }
        }
        if (c == '"') {
            v.kind = J::Str;
            return Str(v.s);
        }
        if (Lit("true")) { v.kind = J::Bool; v.b = true; return true; }
        if (Lit("false")) { v.kind = J::Bool; return true; }
        if (Lit("null")) { v.kind = J::Null; return true; }
        if (Lit("NaN")) { v.kind = J::Float; v.n = std::nan(""); return true; }
        if (Lit("Infinity")) { v.kind = J::Float; v.n = HUGE_VAL; return true; }
        if (Lit("-Infinity")) { v.kind = J::Float; v.n = -HUGE_VAL; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            size_t s = p_;
            bool flt = false;
            if (t_[p_] == '-') p_++;
            if (p_ >= t_.size() || t_[p_] < '0' || t_[p_] > '9') return false;
            if (t_[p_] == '0') p_++;
            else while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') p_++;
            if (p_ < t_.size() && t_[p_] == '.') {
                flt = true;
                p_++;
                if (p_ >= t_.size() || t_[p_] < '0' || t_[p_] > '9') return false;
                while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') p_++;
            }
            if (p_ < t_.size() && (t_[p_] == 'e' || t_[p_] == 'E')) {
                flt = true;
                p_++;
                if (p_ < t_.size() && (t_[p_] == '+' || t_[p_] == '-')) p_++;
                if (p_ >= t_.size() || t_[p_] < '0' || t_[p_] > '9') return false;
                while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') p_++;
            }
            std::string num = t_.substr(s, p_ - s);
            v.kind = flt ? J::Float : J::Int;
            v.n = strtod(num.c_str(), nullptr);         /* 1e999 -> inf, as float('1e999') */
            if (!flt) {
                /* Python's int: no sign on zero, digits as written otherwise */
                size_t k = (num[0] == '-') ? 1 : 0;
                v.s = (num.compare(k, std::string::npos, "0") == 0) ? std::string("0") : num;
            }
            return true;
        }
        return false;
    }
};

static inline bool ParseJson(const std::string &Text, J &Out, std::string &Err)
{
    Parser p(Text);
    return p.Parse(Out, Err);
}

/* ---------------------------------------------------------------- Python's repr, for the messages */

static inline std::string FloatRepr(double v)
{
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v < 0 ? "-inf" : "inf";
    if (v == 0) return std::signbit(v) ? "-0.0" : "0.0";
    char buf[64];
    int prec;
    for (prec = 1; prec <= 17; prec++) {
        snprintf(buf, sizeof(buf), "%.*e", prec - 1, v);
        if (strtod(buf, nullptr) == v) break;
    }
    /* buf = [-]d[.ddd]e[+-]XX: the shortest digits that read back exactly */
    std::string t(buf), digits;
    bool neg = t[0] == '-';
    size_t e = t.find('e');
    int exp = atoi(t.c_str() + e + 1);
    for (size_t i = neg ? 1 : 0; i < e; i++) {
        if (t[i] != '.') digits += t[i];
    }
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    std::string out = neg ? "-" : "";
    if (exp >= -4 && exp < 16) {
        if (exp >= 0) {
            if ((int)digits.size() <= exp + 1) {
                out += digits + std::string((size_t)(exp + 1) - digits.size(), '0') + ".0";
            } else {
                out += digits.substr(0, (size_t)exp + 1) + "." + digits.substr((size_t)exp + 1);
            }
        } else {
            out += "0." + std::string((size_t)(-exp - 1), '0') + digits;
        }
    } else {
        out += digits.substr(0, 1);
        if (digits.size() > 1) out += "." + digits.substr(1);
        char eb[16];
        snprintf(eb, sizeof(eb), "e%c%02d", exp < 0 ? '-' : '+', exp < 0 ? -exp : exp);
        out += eb;
    }
    return out;
}

static inline std::string StrRepr(const std::string &s)
{
    bool sq = s.find('\'') != std::string::npos, dq = s.find('"') != std::string::npos;
    char q = (sq && !dq) ? '"' : '\'';
    std::string out(1, q);
    for (unsigned char c : s) {
        if (c == (unsigned char)q || c == '\\') { out += '\\'; out += (char)c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20 || c == 0x7F) { char b[8]; snprintf(b, sizeof(b), "\\x%02x", c); out += b; }
        else out += (char)c;
    }
    out += q;
    return out;
}

static inline std::string Repr(const J *v)
{
    if (v == nullptr) return "None";
    switch (v->kind) {
    case J::Null: return "None";
    case J::Bool: return v->b ? "True" : "False";
    case J::Int: return v->s;
    case J::Float: return FloatRepr(v->n);
    case J::Str: return StrRepr(v->s);
    case J::Arr: {
        std::string o = "[";
        for (size_t i = 0; i < v->a.size(); i++) o += (i ? ", " : "") + Repr(&v->a[i]);
        return o + "]";
    }
    case J::Obj: {
        std::string o = "{";
        for (size_t i = 0; i < v->o.size(); i++) o += (i ? ", " : "") + StrRepr(v->o[i].first) + ": " + Repr(&v->o[i].second);
        return o + "}";
    }
    }
    return "None";
}

/* ---------------------------------------------------------------- the format */

static const char *const FORMAT = "avium-correction-profile";
static const int VERSION = 1;
static const int DESIGN_RATE = 48000;
static const unsigned RATES[] = { 44100, 48000, 88200, 96000, 176400, 192000 };
static const size_t MAX_FILTERS = 20;
static const double MAX_ABS_GAIN_DB = 30.0;
static const double HEADROOM_TOLERANCE_DB = 0.05;
static const double BUTTERWORTH_Q = 0.7071067811865476;
static const double PI = 3.141592653589793;
static const double MAX_FREQ_FRACTION = 0.49;

struct TypeInfo {
    const char *name;
    unsigned long code;     /* APOLLO_CORR_TYPE_* */
    bool gain, q, first;
    const char *label;
};
static const TypeInfo TYPES[] = {
    { "peaking", APOLLO_CORR_TYPE_PEAKING, true, true, false, "Peak" },
    { "low_shelf", APOLLO_CORR_TYPE_LOW_SHELF, true, true, false, "Low shelf" },
    { "high_shelf", APOLLO_CORR_TYPE_HIGH_SHELF, true, true, false, "High shelf" },
    { "lowpass", APOLLO_CORR_TYPE_LOWPASS, false, true, false, "Low-pass" },
    { "highpass", APOLLO_CORR_TYPE_HIGHPASS, false, true, false, "High-pass" },
    { "lowpass_1", APOLLO_CORR_TYPE_LOWPASS_1, false, false, true, "Low-pass 6 dB" },
    { "highpass_1", APOLLO_CORR_TYPE_HIGHPASS_1, false, false, true, "High-pass 6 dB" },
    { "low_shelf_1", APOLLO_CORR_TYPE_LOW_SHELF_1, true, false, true, "Low shelf 6 dB" },
    { "high_shelf_1", APOLLO_CORR_TYPE_HIGH_SHELF_1, true, false, true, "High shelf 6 dB" },
    { "notch", APOLLO_CORR_TYPE_NOTCH, false, true, false, "Notch" },
    { "allpass", APOLLO_CORR_TYPE_ALLPASS, false, true, false, "All-pass" },
    { "bandpass", APOLLO_CORR_TYPE_BANDPASS, false, true, false, "Band-pass" },
};

static inline const TypeInfo *TypeOf(const J *v)
{
    if (v == nullptr || v->kind != J::Str) return nullptr;
    for (const auto &t : TYPES) {
        if (v->s == t.name) return &t;
    }
    return nullptr;
}

static inline const TypeInfo *TypeOfCode(unsigned long code)
{
    for (const auto &t : TYPES) {
        if (t.code == code) return &t;
    }
    return nullptr;
}

/* _num(): an int or a float, not a bool, finite */
static inline bool Num(const J *v)
{
    return v != nullptr && v->IsNum() && std::isfinite(v->n);
}

/* float(x) for what float() accepts here: numbers and bools (validate() raises on anything else) */
static inline bool AsFloat(const J *v, double def, double &out)
{
    if (v == nullptr) { out = def; return true; }
    if (v->IsNum()) { out = v->n; return true; }
    if (v->kind == J::Bool) { out = v->b ? 1.0 : 0.0; return true; }
    return false;
}

/* bool(x), Python's truth */
static inline bool Truth(const J *v, bool def)
{
    if (v == nullptr) return def;
    switch (v->kind) {
    case J::Null: return false;
    case J::Bool: return v->b;
    case J::Int: case J::Float: return v->n != 0;
    case J::Str: return !v->s.empty();
    case J::Arr: return !v->a.empty();
    case J::Obj: return !v->o.empty();
    }
    return def;
}

/* dsp\biquad.py design(): (b0, b1, b2, a1, a2); freq clamped to 0.49 fs */
static inline std::array<double, 5> Design(const TypeInfo &t, double freq, double gain_db, double q, double fs)
{
    double f0 = std::min(freq, MAX_FREQ_FRACTION * fs);
    double w0 = 2.0 * PI * f0 / fs;
    double cw = std::cos(w0), sw = std::sin(w0);
    double A = std::pow(10.0, gain_db / 40.0);
    std::string k = t.name;
    if (t.first) {
        double K = 1.0 / std::tan(w0 / 2.0), G = std::pow(10.0, gain_db / 20.0), den0 = K + 1.0, den1 = 1.0 - K, n0, n1;
        if (k == "lowpass_1") { n0 = 1.0; n1 = 1.0; }
        else if (k == "highpass_1") { n0 = K; n1 = -K; }
        else if (k == "low_shelf_1") { n0 = K + G; n1 = G - K; }
        else { n0 = G * K + 1.0; n1 = 1.0 - G * K; }
        return { n0 / den0, n1 / den0, 0.0, den1 / den0, 0.0 };
    }
    double alpha = sw / (2.0 * q), b[3], a[3];
    if (k == "peaking") {
        b[0] = 1 + alpha * A; b[1] = -2 * cw; b[2] = 1 - alpha * A;
        a[0] = 1 + alpha / A; a[1] = -2 * cw; a[2] = 1 - alpha / A;
    } else if (k == "low_shelf") {
        double r = 2.0 * std::sqrt(A) * alpha;
        b[0] = A * ((A + 1) - (A - 1) * cw + r); b[1] = 2 * A * ((A - 1) - (A + 1) * cw); b[2] = A * ((A + 1) - (A - 1) * cw - r);
        a[0] = (A + 1) + (A - 1) * cw + r; a[1] = -2 * ((A - 1) + (A + 1) * cw); a[2] = (A + 1) + (A - 1) * cw - r;
    } else if (k == "high_shelf") {
        double r = 2.0 * std::sqrt(A) * alpha;
        b[0] = A * ((A + 1) + (A - 1) * cw + r); b[1] = -2 * A * ((A - 1) + (A + 1) * cw); b[2] = A * ((A + 1) + (A - 1) * cw - r);
        a[0] = (A + 1) - (A - 1) * cw + r; a[1] = 2 * ((A - 1) - (A + 1) * cw); a[2] = (A + 1) - (A - 1) * cw - r;
    } else if (k == "lowpass") {
        b[0] = (1 - cw) / 2; b[1] = 1 - cw; b[2] = (1 - cw) / 2;
        a[0] = 1 + alpha; a[1] = -2 * cw; a[2] = 1 - alpha;
    } else if (k == "highpass") {
        b[0] = (1 + cw) / 2; b[1] = -(1 + cw); b[2] = (1 + cw) / 2;
        a[0] = 1 + alpha; a[1] = -2 * cw; a[2] = 1 - alpha;
    } else if (k == "notch") {
        b[0] = 1.0; b[1] = -2 * cw; b[2] = 1.0;
        a[0] = 1 + alpha; a[1] = -2 * cw; a[2] = 1 - alpha;
    } else if (k == "allpass") {
        b[0] = 1 - alpha; b[1] = -2 * cw; b[2] = 1 + alpha;
        a[0] = 1 + alpha; a[1] = -2 * cw; a[2] = 1 - alpha;
    } else {    /* bandpass */
        b[0] = alpha; b[1] = 0.0; b[2] = -alpha;
        a[0] = 1 + alpha; a[1] = -2 * cw; a[2] = 1 - alpha;
    }
    double a0 = a[0];
    return { b[0] / a0, b[1] / a0, b[2] / a0, a[1] / a0, a[2] / a0 };
}

/* is_stable(): both roots of z^2 + a1 z + a2 inside 1 - 1e-9 */
static inline bool Stable(double a1, double a2)
{
    if (!std::isfinite(a1) || !std::isfinite(a2)) return false;
    const double lim = 1.0 - 1e-9;
    double disc = a1 * a1 - 4.0 * a2;
    if (disc < 0) return std::sqrt(a2) < lim;                   /* a complex pair: |z|^2 = a2 */
    double sq = std::sqrt(disc), qq = -0.5 * (a1 + (a1 >= 0 ? sq : -sq));
    double r1 = std::fabs(qq), r2 = qq != 0 ? std::fabs(a2 / qq) : 0.0;
    return r1 < lim && r2 < lim;
}

struct Filter {
    const TypeInfo *type = nullptr;
    double freq = 0, gain = 0, q = BUTTERWORTH_Q;
    bool enabled = true;
};

/* peak_gain_db(): the cascade's largest gain, 5 Hz to 0.999 Nyquist at 192 points per octave, plus a flat gain */
static inline double PeakGainDb(const std::vector<Filter> &fl, double fs, double gain_db)
{
    bool any = false;
    for (const auto &f : fl) any = any || f.enabled;
    if (!any) return gain_db;
    double fmin = 5.0, fmax = fs / 2.0 * 0.999;
    int n = (int)std::floor(std::log2(fmax / fmin) * 192) + 1;
    std::vector<std::array<double, 5>> c;
    for (const auto &f : fl) {
        if (f.enabled) c.push_back(Design(*f.type, f.freq, f.gain, f.q, fs));
    }
    double best = -1e300;
    for (int i = 0; i < n; i++) {
        double fr = fmin * std::pow(2.0, (double)i / 192.0), th = 2.0 * PI * fr / fs;
        std::complex<double> z1(std::cos(th), -std::sin(th)), z2 = z1 * z1, h(1.0, 0.0);
        for (const auto &k : c) {
            h *= (k[0] + k[1] * z1 + k[2] * z2) / (1.0 + k[3] * z1 + k[4] * z2);
        }
        double db = 20.0 * std::log10(std::max(std::abs(h), 1e-12));
        if (db > best) best = db;
    }
    return best + gain_db;
}

static inline std::string Fmt(const char *f, double v)
{
    char b[64];
    snprintf(b, sizeof(b), f, v);
    return b;
}

/* validate(): every reason this profile must not be applied (empty: fine) */
static inline std::vector<std::string> Validate(const J &d)
{
    std::vector<std::string> out;
    if (d.kind != J::Obj) return { "not a JSON object" };
    const J *fmt = d.Get("format");
    if (fmt == nullptr || fmt->kind != J::Str || fmt->s != FORMAT) {
        return { "format is " + Repr(fmt) + ", expected '" + FORMAT + "'" };
    }
    const J *v = d.Get("version");
    if (v == nullptr || v->kind != J::Int) return { "version missing" };
    if (v->n > VERSION) return { "version " + v->s + " is newer than this reader (1)" };
    if (v->n < 1) return { "bad version " + v->s };
    double pre = 0.0;
    const J *pv = d.Get("preamp_db");
    if (pv != nullptr) {
        if (!Num(pv) || pv->n > 0.0 || pv->n < -60.0) {
            out.push_back("preamp_db must be a number from -60 to 0");
        } else {
            pre = pv->n;
        }
    }
    const J *chans = d.Get("channels");
    if (chans == nullptr || chans->kind != J::Arr || chans->a.empty()) {
        out.push_back("no channels");
        return out;
    }
    std::vector<std::string> ids;
    for (const J &c : chans->a) {
        if (c.kind != J::Obj) {
            out.push_back("channel is not an object");
            continue;
        }
        const J *cid = c.Get("id");
        if (cid == nullptr || cid->kind != J::Str || cid->s.empty()) {
            out.push_back("channel without an id");
            continue;
        }
        const std::string &id = cid->s;
        bool dup = false;
        for (const auto &x : ids) dup = dup || x == id;
        if (dup) out.push_back("channel " + id + ": duplicate id");
        ids.push_back(id);
        const J *o = c.Get("output");
        bool outOk = o == nullptr || o->kind == J::Bool || (o->kind == J::Int && o->n >= 0);
        if (!outOk) out.push_back("channel " + id + ": output must be a channel index >= 0");
        double g = 0.0;
        const J *gv = c.Get("gain_db");
        if (gv != nullptr) {
            if (!Num(gv) || gv->n > 0.0 || gv->n < -60.0) out.push_back("channel " + id + ": gain_db must be a number from -60 to 0");
            else g = gv->n;
        }
        const J *dl = c.Get("delay_ms");
        if (dl != nullptr && (!Num(dl) || dl->n < 0.0 || dl->n > 100.0)) {
            out.push_back("channel " + id + ": delay_ms must be a number from 0 to 100");
        }
        const J *fl = c.Get("filters");
        static const J empty = [] { J e; e.kind = J::Arr; return e; }();
        if (fl == nullptr) fl = &empty;
        if (fl->kind != J::Arr || fl->a.size() > MAX_FILTERS) {
            out.push_back("channel " + id + ": filters must be a list of at most 20");
            continue;
        }
        std::vector<Filter> made;
        for (size_t i = 0; i < fl->a.size(); i++) {
            const J &b = fl->a[i];
            std::string tag = "channel " + id + " filter " + std::to_string(i + 1);
            const TypeInfo *ty = b.kind == J::Obj ? TypeOf(b.Get("type")) : nullptr;
            if (b.kind != J::Obj || ty == nullptr) {
                out.push_back(tag + ": unknown type " + (b.kind == J::Obj ? Repr(b.Get("type")) : Repr(&b)));
                continue;
            }
            const J *fr = b.Get("freq_hz");
            if (!Num(fr) || !(fr->n > 0 && fr->n < DESIGN_RATE / 2)) {
                out.push_back(tag + ": freq_hz must be between 0 and 24000");
                continue;
            }
            if (ty->gain && (!Num(b.Get("gain_db")) || std::fabs(b.Get("gain_db")->n) > MAX_ABS_GAIN_DB)) {
                out.push_back(tag + ": gain_db must be a number within +-30");
                continue;
            }
            if (ty->q && (!Num(b.Get("q")) || !(b.Get("q")->n >= 0.05 && b.Get("q")->n <= 50))) {
                out.push_back(tag + ": q must be a number from 0.05 to 50");
                continue;
            }
            Filter f;
            f.type = ty;
            f.freq = fr->n;
            if (!AsFloat(b.Get("gain_db"), 0.0, f.gain)) {
                out.push_back(tag + ": gain_db must be a number");     /* validate() would raise here */
                continue;
            }
            if (!AsFloat(b.Get("q"), BUTTERWORTH_Q, f.q)) {
                out.push_back(tag + ": q must be a number");           /* validate() would raise here */
                continue;
            }
            f.enabled = Truth(b.Get("enabled"), true);
            const J *coefs = b.Get("coefficients");
            if (coefs == nullptr) {
                out.push_back(tag + ": coefficients for 48000 Hz missing");     /* {} has no 48000 either */
                continue;
            }
            if (coefs->kind != J::Obj || coefs->Get("48000") == nullptr) {
                out.push_back(tag + ": coefficients for 48000 Hz missing");
                continue;
            }
            for (const auto &kv : coefs->o) {
                const std::string &rate = kv.first;
                const J &cv = kv.second;
                bool five = cv.kind == J::Arr && cv.a.size() == 5;
                for (size_t k = 0; five && k < 5; k++) five = Num(&cv.a[k]);
                if (!five) {
                    out.push_back(tag + ": coefficients at " + rate + " Hz must be 5 finite numbers");
                    break;
                }
                if (!Stable(cv.a[3].n, cv.a[4].n)) {
                    out.push_back(tag + ": unstable at " + rate + " Hz");
                    break;
                }
                char *end = nullptr;
                std::string trimmed = rate;
                while (!trimmed.empty() && isspace((unsigned char)trimmed.front())) trimmed.erase(trimmed.begin());
                while (!trimmed.empty() && isspace((unsigned char)trimmed.back())) trimmed.pop_back();
                double fs = strtod(trimmed.c_str(), &end);
                if (trimmed.empty() || end == nullptr || *end != 0) {
                    out.push_back(tag + ": could not convert string to float: " + StrRepr(rate));
                    break;
                }
                bool match = std::isfinite(fs) && fs > 0;
                if (match) {
                    auto want = Design(*ty, f.freq, f.gain, f.q, fs);
                    double mx = 0, diff = 0;
                    for (int k = 0; k < 5; k++) {
                        mx = std::max(mx, std::fabs(want[k]));
                        diff = std::max(diff, std::fabs(cv.a[k].n - want[k]));
                    }
                    match = !(diff > 1e-6 * std::max(1.0, mx));
                }
                if (!match) {
                    out.push_back(tag + ": coefficients at " + rate + " Hz do not match its parameters");
                    break;
                }
            }
            made.push_back(f);
        }
        double peak = PeakGainDb(made, DESIGN_RATE, g) + pre;
        if (peak > HEADROOM_TOLERANCE_DB) {
            out.push_back("channel " + id + ": peak gain " + Fmt("%+.2f", peak) + " dB after preamp would clip (lower preamp_db)");
        }
    }
    return out;
}

/* ---------------------------------------------------------------- for the driver */

struct SlotPick {
    std::string id;     /* the profile channel's id */
    long slot;          /* playback slot on the unit, -1 = not corrected */
};

static inline void CopyText(char *dst, size_t cap, const std::string &src)
{
    size_t n = std::min(src.size(), cap - 1);
    while (n > 0 && n < src.size() && ((unsigned char)src[n] & 0xC0) == 0x80) n--;     /* whole UTF-8 characters */
    memcpy(dst, src.data(), n);
    dst[n] = 0;
}

static inline std::string HzText(double f)
{
    char b[32];
    if (f >= 1000.0) snprintf(b, sizeof(b), "%g kHz", f / 1000.0);
    else snprintf(b, sizeof(b), "%g Hz", f);
    return b;
}

/*
 * How the profile was measured and what it aims at, one line for the page and the driver's copy:
 * "Standard, 9 spots, loopback timing. Target Studio tilt, -0.8 dB/oct around 1 kHz. Boosts up to 6 dB."
 * Only what the file's "measurement" block says (informational in the format); empty if it says nothing.
 */
static inline std::string Summary(const J &d)
{
    const J *m = d.Get("measurement");
    std::string out;
    if (m == nullptr || m->kind != J::Obj) return out;
    std::vector<std::string> how;
    const J *mode = m->Get("mode");
    if (mode && mode->kind == J::Str && !mode->s.empty()) {
        std::string t = mode->s;
        t[0] = (char)toupper((unsigned char)t[0]);
        how.push_back(t);
    }
    const J *spots = m->Get("spots"), *mmm = m->Get("mmm_seconds");
    if (spots && spots->kind == J::Arr && !spots->a.empty()) {
        how.push_back(std::to_string(spots->a.size()) + (spots->a.size() == 1 ? " spot" : " spots"));
    } else if (Num(mmm)) {
        char b[48];
        snprintf(b, sizeof(b), "moving mic %g s", mmm->n);
        how.push_back(b);
    }
    const J *timing = m->Get("timing");
    if (timing && timing->kind == J::Str && !timing->s.empty() && timing->s != "none") how.push_back(timing->s + " timing");
    for (size_t i = 0; i < how.size(); i++) out += (i ? ", " : "") + how[i];
    if (!out.empty()) out += ".";
    const J *tg = m->Get("target");
    if (tg && tg->kind == J::Obj) {
        const J *nm = tg->Get("name"), *tilt = tg->Get("tilt_db_per_oct"), *pivot = tg->Get("pivot_hz"), *pts = tg->Get("points");
        std::string t = " Target";
        if (nm && nm->kind == J::Str && !nm->s.empty()) t += " " + nm->s;
        if (Num(tilt) && tilt->n != 0.0) {
            char b[48];
            snprintf(b, sizeof(b), ", %+.1f dB/oct", tilt->n);
            t += b;
            if (Num(pivot)) t += " around " + HzText(pivot->n);
        } else {
            t += ", flat";
        }
        if (pts && pts->kind == J::Arr && !pts->a.empty()) t += ", " + std::to_string(pts->a.size()) + " curve points";
        out += t + ".";
    }
    const J *lim = m->Get("limits");
    const J *boost = lim && lim->kind == J::Obj ? lim->Get("max_boost_db") : nullptr;
    if (Num(boost)) {
        char b[48];
        snprintf(b, sizeof(b), " Boosts up to %g dB.", boost->n);
        out += b;
    }
    while (!out.empty() && out[0] == ' ') out.erase(out.begin());
    return out;
}

/*
 * A profile Validate() passed -> what the driver loads: Rate's coefficients (designed from the
 * parameters when the file has none for that rate, as the format says), each picked channel on its
 * slot. false + Err when it cannot be expressed for the driver.
 */
static inline bool Compile(const J &d, unsigned long Rate, const std::vector<SlotPick> &Slots, bool Enable,
                           APOLLO_CORR_PROFILE &P, std::string &Err)
{
    memset(&P, 0, sizeof(P));
    P.Size = sizeof(P);
    P.Version = APOLLO_CORR_VERSION;
    P.Rate = Rate;
    P.Flags = Enable ? APOLLO_CORR_LOAD_ENABLE : 0;
    bool known = false;
    for (unsigned r : RATES) known = known || r == Rate;
    if (!known) {
        Err = "the unit runs at " + std::to_string(Rate) + " Hz, which the format has no coefficients for";
        return false;
    }
    const J *pv = d.Get("preamp_db");
    P.PreampDb = Num(pv) ? pv->n : 0.0;
    P.Preamp = std::pow(10.0, P.PreampDb / 20.0);
    const J *nm = d.Get("name");
    CopyText(P.Name, sizeof(P.Name), nm && nm->kind == J::Str ? nm->s : std::string());
    const J *id = d.Get("id");
    CopyText(P.Id, sizeof(P.Id), id && id->kind == J::Str ? id->s : std::string());
    const J *cr = d.Get("created");
    CopyText(P.Created, sizeof(P.Created), cr && cr->kind == J::Str ? cr->s : std::string());
    CopyText(P.Summary, sizeof(P.Summary), Summary(d));
    const J *chans = d.Get("channels");
    std::string rateKey = std::to_string(Rate);
    for (const J &c : chans->a) {
        const std::string &cid = c.Get("id")->s;
        long slot = -1;
        for (const auto &s : Slots) {
            if (s.id == cid) slot = s.slot;
        }
        if (slot < 0) continue;
        if (P.ChannelCount >= APOLLO_CORR_MAX_CHANNELS) {
            Err = "the driver corrects at most 4 channels";
            return false;
        }
        if (slot > APOLLO_CORR_MAX_SLOT) {
            Err = "channel " + cid + ": no playback slot " + std::to_string(slot);
            return false;
        }
        for (unsigned long k = 0; k < P.ChannelCount; k++) {
            if (P.Channel[k].Slot == (unsigned long)slot) {
                Err = "channels " + std::string(P.Channel[k].Id) + " and " + cid + " are both on playback slot " + std::to_string(slot);
                return false;
            }
        }
        APOLLO_CORR_CHANNEL &h = P.Channel[P.ChannelCount];
        CopyText(h.Id, sizeof(h.Id), cid);
        h.Slot = (unsigned long)slot;
        const J *o = c.Get("output");
        h.Output = (o && o->kind == J::Int) ? (unsigned long)o->n : 0;
        const J *g = c.Get("gain_db");
        h.GainDb = Num(g) ? g->n : 0.0;
        h.Gain = std::pow(10.0, h.GainDb / 20.0);
        const J *dl = c.Get("delay_ms");
        double us = Num(dl) ? dl->n * 1000.0 : 0.0;
        if (us > APOLLO_CORR_MAX_DELAY_US + 0.5) {
            Err = "channel " + cid + ": a delay of " + Fmt("%.2f", us / 1000.0) + " ms is more than the driver's 10 ms";
            return false;
        }
        h.DelayUs = (unsigned long)std::floor(us + 0.5);
        const J *fl = c.Get("filters");
        for (size_t i = 0; fl && fl->kind == J::Arr && i < fl->a.size(); i++) {
            const J &b = fl->a[i];
            const TypeInfo *ty = TypeOf(b.Get("type"));
            APOLLO_CORR_FILTER &f = h.Filter[h.FilterCount++];
            double gain = 0, q = BUTTERWORTH_Q;
            AsFloat(b.Get("gain_db"), 0.0, gain);
            AsFloat(b.Get("q"), BUTTERWORTH_Q, q);
            f.Type = ty->code;
            f.Enabled = Truth(b.Get("enabled"), true) ? 1 : 0;
            f.FreqHz = b.Get("freq_hz")->n;
            f.GainDb = ty->gain ? gain : 0.0;
            f.Q = ty->q ? q : BUTTERWORTH_Q;
            const J *cs = b.Get("coefficients")->Get(rateKey.c_str());
            if (cs != nullptr) {
                for (int k = 0; k < 5; k++) f.Coef[k] = cs->a[(size_t)k].n;
            } else {
                auto w = Design(*ty, f.FreqHz, gain, q, (double)Rate);
                for (int k = 0; k < 5; k++) f.Coef[k] = w[(size_t)k];
            }
        }
        P.ChannelCount++;
    }
    if (P.ChannelCount == 0) {
        Err = "no channel is assigned to an output";
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------- the driver's state for the page */

static inline void Esc(std::string &out, const char *s, size_t cap)
{
    out += '"';
    for (size_t i = 0; i < cap && s[i] != 0; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
        else out += (char)c;
    }
    out += '"';
}

static inline std::string Number(double v)
{
    if (!std::isfinite(v)) return "null";
    char b[40];
    snprintf(b, sizeof(b), "%.17g", v);
    return b;
}

static inline std::string ProfileJson(const APOLLO_CORR_PROFILE &P)
{
    if (P.Size != sizeof(P) || P.ChannelCount == 0 || P.ChannelCount > APOLLO_CORR_MAX_CHANNELS) return "null";
    std::string j = "{\"name\":";
    Esc(j, P.Name, sizeof(P.Name));
    j += ",\"id\":";
    Esc(j, P.Id, sizeof(P.Id));
    j += ",\"created\":";
    Esc(j, P.Created, sizeof(P.Created));
    j += ",\"summary\":";
    Esc(j, P.Summary, sizeof(P.Summary));
    j += ",\"rate\":" + std::to_string(P.Rate) + ",\"preampDb\":" + Number(P.PreampDb) + ",\"channels\":[";
    for (unsigned long c = 0; c < P.ChannelCount; c++) {
        const APOLLO_CORR_CHANNEL &h = P.Channel[c];
        j += c ? ",{\"id\":" : "{\"id\":";
        Esc(j, h.Id, sizeof(h.Id));
        j += ",\"slot\":" + std::to_string(h.Slot) + ",\"output\":" + std::to_string(h.Output) + ",\"gainDb\":" + Number(h.GainDb) +
             ",\"delayMs\":" + Number(h.DelayUs / 1000.0) + ",\"filters\":[";
        for (unsigned long k = 0; k < h.FilterCount && k < APOLLO_CORR_MAX_FILTERS; k++) {
            const APOLLO_CORR_FILTER &f = h.Filter[k];
            const TypeInfo *t = TypeOfCode(f.Type);
            j += k ? ",{\"type\":" : "{\"type\":";
            j += t ? "\"" + std::string(t->name) + "\"" : "null";
            j += std::string(",\"enabled\":") + (f.Enabled ? "true" : "false") + ",\"freq\":" + Number(f.FreqHz) +
                 ",\"gain\":" + Number(f.GainDb) + ",\"q\":" + Number(f.Q) + ",\"coef\":[";
            for (int i = 0; i < 5; i++) j += (i ? "," : "") + Number(f.Coef[i]);
            j += "]}";
        }
        j += "]}";
    }
    return j + "]}";
}

static inline std::string StateJson(const APOLLO_CORR_STATE &S)
{
    std::string j = "{\"flags\":" + std::to_string(S.Flags);
    j += std::string(",\"loaded\":") + ((S.Flags & APOLLO_CORR_F_LOADED) ? "true" : "false");
    j += std::string(",\"enabled\":") + ((S.Flags & APOLLO_CORR_F_ENABLED) ? "true" : "false");
    j += std::string(",\"bypass\":") + ((S.Flags & APOLLO_CORR_F_BYPASS) ? "true" : "false");
    j += std::string(",\"running\":") + ((S.Flags & APOLLO_CORR_F_RUNNING) ? "true" : "false");
    j += std::string(",\"rateMismatch\":") + ((S.Flags & APOLLO_CORR_F_RATE_MISMATCH) ? "true" : "false");
    j += std::string(",\"stored\":") + ((S.Flags & APOLLO_CORR_F_STORED) ? "true" : "false");
    j += std::string(",\"slotRange\":") + ((S.Flags & APOLLO_CORR_F_SLOT_RANGE) ? "true" : "false");
    j += std::string(",\"unitIdle\":") + ((S.Flags & APOLLO_CORR_F_UNIT_IDLE) ? "true" : "false");
    j += ",\"unitRate\":" + std::to_string(S.UnitRate) + ",\"wet\":" + std::to_string(S.WetPermille) +
         ",\"swap\":" + std::to_string(S.SwapPermille) + ",\"runs\":" + std::to_string(S.Runs) +
         ",\"frames\":" + std::to_string(S.Frames) + ",\"late\":" + std::to_string(S.LateFrames) +
         ",\"lastNs\":" + std::to_string(S.LastNs) + ",\"maxNs\":" + std::to_string(S.MaxNs) +
         ",\"storeStatus\":" + std::to_string((unsigned long)S.StoreStatus) + ",\"from\":" + std::to_string(S.LoadedFrom);
    j += ",\"profile\":" + ProfileJson(S.Profile) + "}";
    return j;
}

/* Why the driver refused (APOLLO_CORR_RESULT.Error). */
static inline std::string RefusalText(unsigned long e, unsigned long ch, unsigned long flt)
{
    std::string where = " (channel " + std::to_string(ch + 1) + ", filter " + std::to_string(flt + 1) + ")";
    std::string at = " (channel " + std::to_string(ch + 1) + ")";
    switch (e) {
    case APOLLO_CORR_ERR_SIZE:          return "Apollo Control and the driver are from different builds (profile size)";
    case APOLLO_CORR_ERR_RATE:          return "the driver does not run that sample rate";
    case APOLLO_CORR_ERR_CHANNELS:      return "the driver takes 1 to 4 channels";
    case APOLLO_CORR_ERR_FILTERS:       return "more than 20 filters on a channel" + at;
    case APOLLO_CORR_ERR_NOT_FINITE:    return "a number is not finite" + where;
    case APOLLO_CORR_ERR_UNSTABLE:      return "a filter is unstable" + where;
    case APOLLO_CORR_ERR_GAIN:          return "a preamp or trim above 0 dB or below -60 dB" + at;
    case APOLLO_CORR_ERR_DELAY:         return "a delay longer than 10 ms" + at;
    case APOLLO_CORR_ERR_SLOT:          return "that output does not exist on this unit, or two channels share it" + at;
    case APOLLO_CORR_ERR_CLIP:          return "the corrected signal could clip: the preamp is too high" + at;
    case APOLLO_CORR_ERR_RATE_MISMATCH: return "the unit changed its sample rate: load the profile again";
    case APOLLO_CORR_ERR_OP:            return "the driver does not know that request";
    case APOLLO_CORR_ERR_NO_PROFILE:    return "no profile is loaded on this unit";
    case APOLLO_CORR_ERR_UNIT:          return "this unit does not run audio on the open driver (observe mode)";
    case APOLLO_CORR_ERR_BUSY:          return "the driver is busy, try again";
    case APOLLO_CORR_ERR_TEXT:          return "a name in the profile is too long";
    case APOLLO_CORR_ERR_DC:            return "more than 6 dB of gain below 5 Hz or at Nyquist" + at;
    case APOLLO_CORR_ERR_NARROW:        return "a filter is narrower than the format allows" + where;
    default:                            return "the driver refused the profile (check " + std::to_string(e) + ")";
    }
}

}   /* namespace corrprof */
