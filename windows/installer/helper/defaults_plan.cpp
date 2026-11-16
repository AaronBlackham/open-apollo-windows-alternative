/*
 * defaults.json parsing and validation (defaults_plan.h). No registry access: test_units.cpp
 * links it with units.cpp only.
 */
#include "defaults_plan.h"
#include "units.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <wchar.h>
#include <wctype.h>

static std::wstring Fmt1(const wchar_t *fmt, const std::wstring &a)
{
    wchar_t buf[512];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, a.c_str());
    return buf;
}

static std::wstring Fmt2(const wchar_t *fmt, const std::wstring &a, const std::wstring &b)
{
    wchar_t buf[512];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, a.c_str(), b.c_str());
    return buf;
}

/* ------------------------------------------------------------------ */
/* a small JSON reader                                                 */
/* ------------------------------------------------------------------ */

struct JVal {
    enum Type { Null, Bool, Num, Str, Arr, Obj } T = Null;
    bool B = false;
    double N = 0;
    bool Integral = false;
    std::wstring S;
    std::vector<JVal> A;
    std::vector<std::pair<std::wstring, JVal>> O;
    const JVal *Get(const wchar_t *k) const
    {
        for (auto &p : O)
            if (p.first == k)
                return &p.second;
        return nullptr;
    }
};

class JParser {
public:
    JParser(const char *p, const char *e) : P(p), E(e) {}
    bool Parse(JVal &v)
    {
        if (!Value(v, 0))
            return false;
        Ws();
        return P == E;
    }
    size_t Offset(const char *start) const { return (size_t)(P - start); }

private:
    const char *P, *E;
    void Ws()
    {
        while (P < E && (*P == ' ' || *P == '\t' || *P == '\r' || *P == '\n'))
            P++;
    }
    bool Lit(const char *s)
    {
        size_t n = strlen(s);
        if ((size_t)(E - P) < n || memcmp(P, s, n) != 0)
            return false;
        P += n;
        return true;
    }
    bool String(std::wstring &out)
    {
        if (P >= E || *P != '"')
            return false;
        P++;
        std::string u;
        while (P < E && *P != '"') {
            char c = *P++;
            if ((unsigned char)c < 0x20)
                return false;
            if (c != '\\') {
                u += c;
                continue;
            }
            if (P >= E)
                return false;
            c = *P++;
            switch (c) {
            case '"': u += '"'; break;
            case '\\': u += '\\'; break;
            case '/': u += '/'; break;
            case 'b': u += '\b'; break;
            case 'f': u += '\f'; break;
            case 'n': u += '\n'; break;
            case 'r': u += '\r'; break;
            case 't': u += '\t'; break;
            case 'u': {
                if (E - P < 4)
                    return false;
                unsigned cp = 0;
                for (int i = 0; i < 4; i++) {
                    char h = *P++;
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                    else return false;
                }
                u += ToUtf8(std::wstring(1, (wchar_t)cp));
                break;
            }
            default:
                return false;
            }
        }
        if (P >= E)
            return false;
        P++;
        out = FromUtf8(u);
        return true;
    }
    bool Number(JVal &v)
    {
        const char *s = P;
        if (P < E && *P == '-')
            P++;
        bool frac = false;
        while (P < E && ((*P >= '0' && *P <= '9') || *P == '.' || *P == 'e' || *P == 'E' || *P == '+' || *P == '-')) {
            if (*P == '.' || *P == 'e' || *P == 'E')
                frac = true;
            P++;
        }
        if (P == s)
            return false;
        std::string t(s, P);
        char *end = nullptr;
        v.T = JVal::Num;
        v.N = strtod(t.c_str(), &end);
        v.Integral = !frac;
        return end != nullptr && *end == '\0';
    }
    bool Value(JVal &v, int depth)
    {
        if (depth > 32)
            return false;
        Ws();
        if (P >= E)
            return false;
        if (*P == '{') {
            P++;
            v.T = JVal::Obj;
            Ws();
            if (P < E && *P == '}') {
                P++;
                return true;
            }
            for (;;) {
                Ws();
                std::wstring k;
                if (!String(k))
                    return false;
                Ws();
                if (P >= E || *P++ != ':')
                    return false;
                JVal child;
                if (!Value(child, depth + 1))
                    return false;
                v.O.push_back({ k, child });
                Ws();
                if (P < E && *P == ',') {
                    P++;
                    continue;
                }
                if (P < E && *P == '}') {
                    P++;
                    return true;
                }
                return false;
            }
        }
        if (*P == '[') {
            P++;
            v.T = JVal::Arr;
            Ws();
            if (P < E && *P == ']') {
                P++;
                return true;
            }
            for (;;) {
                JVal child;
                if (!Value(child, depth + 1))
                    return false;
                v.A.push_back(child);
                Ws();
                if (P < E && *P == ',') {
                    P++;
                    continue;
                }
                if (P < E && *P == ']') {
                    P++;
                    return true;
                }
                return false;
            }
        }
        if (*P == '"') {
            v.T = JVal::Str;
            return String(v.S);
        }
        if (Lit("true")) {
            v.T = JVal::Bool;
            v.B = true;
            return true;
        }
        if (Lit("false")) {
            v.T = JVal::Bool;
            v.B = false;
            return true;
        }
        if (Lit("null")) {
            v.T = JVal::Null;
            return true;
        }
        return Number(v);
    }
};

/* ------------------------------------------------------------------ */
/* validation                                                          */
/* ------------------------------------------------------------------ */

static bool NameChar(wchar_t c)
{
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'_';
}

static bool ValidValueName(const std::wstring &n)
{
    if (n.empty() || n.size() > 64)
        return false;
    for (wchar_t c : n)
        if (!NameChar(c))
            return false;
    return true;
}

bool ValidSubKeyPath(const std::wstring &path)
{
    if (path.empty() || path.size() > 100)
        return false;
    int parts = 0;
    size_t len = 0;
    for (size_t i = 0; i <= path.size(); i++) {
        if (i == path.size() || path[i] == L'\\') {
            if (len == 0 || len > 32)
                return false;       /* empty part: leading, trailing or doubled backslash */
            parts++;
            len = 0;
        } else if (NameChar(path[i])) {
            len++;
        } else {
            return false;
        }
    }
    return parts >= 1 && parts <= 3;
}

static bool ToDword(const JVal &v, DWORD &out)
{
    if (v.T == JVal::Num && v.Integral && v.N >= 0 && v.N <= 4294967295.0 && floor(v.N) == v.N) {
        out = (DWORD)v.N;
        return true;
    }
    if (v.T == JVal::Str && !v.S.empty()) {
        wchar_t *end = nullptr;
        unsigned long long x = wcstoull(v.S.c_str(), &end, 0);
        if (end != nullptr && *end == L'\0' && x <= 0xFFFFFFFFull && v.S[0] != L'-') {
            out = (DWORD)x;
            return true;
        }
    }
    return false;
}

static bool IsComment(const std::wstring &k)
{
    return !k.empty() && k[0] == L'_';
}

static std::wstring Where(const std::wstring &sub, const std::wstring &name)
{
    return sub.empty() ? name : sub + L"\\" + name;
}

/* One "values" object into items. */
static bool ParseValues(const JVal &values, const std::wstring &sub, bool overwriteDefault,
                        std::vector<DefaultItem> &items, std::wstring &error)
{
    if (values.T != JVal::Obj) {
        error = sub.empty() ? std::wstring(L"\"values\" is not an object") : Fmt1(L"\"%s\": \"values\" is not an object", sub);
        return false;
    }
    for (auto &kv : values.O) {
        if (IsComment(kv.first))
            continue;
        if (!ValidValueName(kv.first)) {
            error = Fmt1(L"bad value name '%s' (letters, digits, _ only)", Where(sub, kv.first));
            return false;
        }
        for (auto &it : items) {
            if (UnitIEq(it.SubKey, sub) && UnitIEq(it.Name, kv.first)) {
                error = Fmt1(L"'%s' is given twice", Where(sub, kv.first));
                return false;
            }
        }
        DefaultItem it;
        it.SubKey = sub;
        it.Name = kv.first;
        it.Overwrite = overwriteDefault;
        const JVal *dataV = &kv.second;
        std::wstring type = L"dword";
        if (kv.second.T == JVal::Obj) {
            for (auto &f : kv.second.O) {
                if (!IsComment(f.first) && f.first != L"type" && f.first != L"data" && f.first != L"overwrite") {
                    error = Fmt2(L"'%s': unknown key \"%s\"", Where(sub, kv.first), f.first);
                    return false;
                }
            }
            const JVal *t = kv.second.Get(L"type");
            const JVal *o = kv.second.Get(L"overwrite");
            dataV = kv.second.Get(L"data");
            if (t != nullptr) {
                if (t->T != JVal::Str) {
                    error = Fmt1(L"'%s': \"type\" must be a string", Where(sub, kv.first));
                    return false;
                }
                type = t->S;
                for (wchar_t &c : type)
                    c = (wchar_t)towlower(c);
            }
            if (o != nullptr) {
                if (o->T != JVal::Bool) {
                    error = Fmt1(L"'%s': \"overwrite\" must be true or false", Where(sub, kv.first));
                    return false;
                }
                it.Overwrite = o->B;
            }
            if (dataV == nullptr) {
                error = Fmt1(L"'%s' has no \"data\"", Where(sub, kv.first));
                return false;
            }
        } else if (kv.second.T == JVal::Str) {
            type = L"sz";
        }
        if (type == L"dword") {
            if (!ToDword(*dataV, it.Dw)) {
                error = Fmt1(L"'%s' needs a whole number 0..4294967295", Where(sub, kv.first));
                return false;
            }
        } else if (type == L"sz" && dataV->T == JVal::Str) {
            it.Type = REG_SZ;
            it.Sz = dataV->S;
        } else {
            error = Fmt1(L"'%s': type must be \"dword\" or \"sz\" (with string data)", Where(sub, kv.first));
            return false;
        }
        items.push_back(it);
    }
    return true;
}

bool ParseDefaults(const std::vector<BYTE> &data, std::vector<DefaultItem> &out, std::wstring &error)
{
    out.clear();
    error.clear();
    const char *b = data.empty() ? "" : (const char *)data.data(), *e = b + data.size();
    if (data.size() >= 3 && (BYTE)b[0] == 0xEF && (BYTE)b[1] == 0xBB && (BYTE)b[2] == 0xBF)
        b += 3;
    JVal root;
    JParser jp(b, e);
    if (!jp.Parse(root) || root.T != JVal::Obj) {
        wchar_t buf[96];
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"not valid JSON (stopped near byte %zu)", jp.Offset(b));
        error = buf;
        return false;
    }
    for (auto &kv : root.O) {
        if (!IsComment(kv.first) && kv.first != L"overwrite" && kv.first != L"values" && kv.first != L"subkeys") {
            error = Fmt1(L"unknown top-level key \"%s\"", kv.first);
            return false;
        }
    }
    const JVal *ov = root.Get(L"overwrite");
    if (ov != nullptr && ov->T != JVal::Bool) {
        error = L"\"overwrite\" must be true or false";
        return false;
    }
    bool overwriteAll = ov != nullptr && ov->B;
    const JVal *values = root.Get(L"values");
    if (values == nullptr) {
        error = L"no \"values\" object";
        return false;
    }
    std::vector<DefaultItem> items;
    if (!ParseValues(*values, L"", overwriteAll, items, error))
        return false;

    const JVal *subs = root.Get(L"subkeys");
    if (subs != nullptr) {
        if (subs->T != JVal::Obj) {
            error = L"\"subkeys\" is not an object";
            return false;
        }
        for (auto &kv : subs->O) {
            if (IsComment(kv.first))
                continue;
            if (!ValidSubKeyPath(kv.first)) {
                error = Fmt1(L"bad subkey '%s' (1 to 3 parts of letters, digits, _, joined by single backslashes)", kv.first);
                return false;
            }
            for (auto &it : items) {
                if (UnitIEq(it.SubKey, kv.first)) {
                    error = Fmt1(L"subkey '%s' is given twice", kv.first);
                    return false;
                }
            }
            if (kv.second.T != JVal::Obj) {
                error = Fmt1(L"subkey '%s' is not an object", kv.first);
                return false;
            }
            for (auto &f : kv.second.O) {
                if (!IsComment(f.first) && f.first != L"overwrite" && f.first != L"values") {
                    error = Fmt2(L"subkey '%s': unknown key \"%s\"", kv.first, f.first);
                    return false;
                }
            }
            bool subOverwrite = overwriteAll;
            const JVal *so = kv.second.Get(L"overwrite");
            if (so != nullptr) {
                if (so->T != JVal::Bool) {
                    error = Fmt1(L"subkey '%s': \"overwrite\" must be true or false", kv.first);
                    return false;
                }
                subOverwrite = so->B;
            }
            const JVal *sv = kv.second.Get(L"values");
            if (sv == nullptr) {
                error = Fmt1(L"subkey '%s' has no \"values\" object", kv.first);
                return false;
            }
            size_t before = items.size();
            if (!ParseValues(*sv, kv.first, subOverwrite, items, error))
                return false;
            if (items.size() == before) {
                error = Fmt1(L"subkey '%s' has no values", kv.first);
                return false;
            }
        }
    }
    out = items;
    return true;
}
