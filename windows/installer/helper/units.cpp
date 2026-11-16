/*
 * Apollo units: classification and the multi-unit decisions (units.h). No device or registry
 * access here: test_units.cpp links it without the rest of the helper.
 */
#include "units.h"
#include <wchar.h>

static wchar_t Up(wchar_t c)
{
    return (c >= L'a' && c <= L'z') ? (wchar_t)(c - L'a' + L'A') : c;
}

static bool IsHex(wchar_t c)
{
    c = Up(c);
    return (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F');
}

static bool IsIdChar(wchar_t c)
{
    c = Up(c);
    return (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') || c == L'_';
}

static std::wstring Upper(std::wstring s)
{
    for (wchar_t &c : s)
        c = Up(c);
    return s;
}

bool UnitIEq(const std::wstring &a, const std::wstring &b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++)
        if (Up(a[i]) != Up(b[i]))
            return false;
    return true;
}

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

static bool StartsI(const std::wstring &s, const std::wstring &prefix)
{
    return s.size() >= prefix.size() && UnitIEq(s.substr(0, prefix.size()), prefix);
}

bool Device::OnApollo() const
{
    return UnitIEq(Service, APOLLO_SERVICE);
}

DriverOwner Device::Owner() const
{
    return OwnerOf(Service, Provider);
}

bool IsFourPartUaHwid(const std::wstring &s)
{
    const std::wstring prefix = std::wstring(UA_PCI_PREFIX) + L"&SUBSYS_";
    if (s.size() != prefix.size() + 8 || !StartsI(s, prefix))
        return false;
    for (size_t i = prefix.size(); i < s.size(); i++)
        if (!IsHex(s[i]))
            return false;
    return true;
}

std::wstring FourPartHwid(const std::vector<std::wstring> &hardwareIds, const std::wstring &instanceId)
{
    for (auto &h : hardwareIds)
        if (IsFourPartUaHwid(h))
            return Upper(h);
    /* PCI\VEN_1A00&DEV_0002&SUBSYS_00191A00&REV_00\8&5E6F7A8&0&000000100010 */
    size_t first = instanceId.find(L'\\');
    if (first == std::wstring::npos)
        return std::wstring();
    size_t second = instanceId.find(L'\\', first + 1);
    std::wstring id = instanceId.substr(0, second);
    size_t rev = Upper(id).find(L"&REV_");
    if (rev != std::wstring::npos)
        id = id.substr(0, rev);
    return IsFourPartUaHwid(id) ? Upper(id) : std::wstring();
}

UnitKind ClassifyHwid(const std::wstring &fourPart)
{
    if (UnitIEq(fourPart, X8P_HWID))
        return UNIT_X8P;
    if (UnitIEq(fourPart, TWINX_HWID))
        return UNIT_TWINX;
    return UNIT_OTHER;
}

const wchar_t *UnitKindKey(UnitKind k)
{
    switch (k) {
    case UNIT_X8P:   return L"x8p";
    case UNIT_TWINX: return L"twinx";
    default:         return L"other";
    }
}

const wchar_t *UnitKindName(UnitKind k)
{
    switch (k) {
    case UNIT_X8P:   return L"Apollo x8p";
    case UNIT_TWINX: return L"Apollo Twin X";
    default:         return L"Universal Audio device";
    }
}

bool IsSupportedKind(UnitKind k)
{
    return k == UNIT_X8P || k == UNIT_TWINX;
}

const wchar_t *UnitHwid(UnitKind k)
{
    switch (k) {
    case UNIT_X8P:   return X8P_HWID;
    case UNIT_TWINX: return TWINX_HWID;
    default:         return L"";
    }
}

static std::wstring Trim(const std::wstring &s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t'))
        a++;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t'))
        b--;
    return s.substr(a, b - a);
}

bool KindIn(const std::vector<UnitKind> &kinds, UnitKind k)
{
    for (UnitKind x : kinds)
        if (x == k)
            return true;
    return false;
}

bool ParseUnitList(const std::wstring &s, std::vector<UnitKind> &out)
{
    out.clear();
    std::wstring t = Trim(s);
    if (UnitIEq(t, L"all")) {
        out = { UNIT_X8P, UNIT_TWINX };
        return true;
    }
    if (UnitIEq(t, L"none"))
        return true;
    if (t.empty())
        return false;
    bool x8p = false, twinx = false;
    size_t pos = 0;
    for (;;) {
        size_t comma = t.find(L',', pos);
        std::wstring part = Trim(t.substr(pos, comma == std::wstring::npos ? std::wstring::npos : comma - pos));
        if (UnitIEq(part, L"x8p")) {
            if (x8p)
                return false;
            x8p = true;
        } else if (UnitIEq(part, L"twinx")) {
            if (twinx)
                return false;
            twinx = true;
        } else {
            return false;
        }
        if (comma == std::wstring::npos)
            break;
        pos = comma + 1;
    }
    if (x8p)
        out.push_back(UNIT_X8P);
    if (twinx)
        out.push_back(UNIT_TWINX);
    return true;
}

std::wstring UnitListText(const std::vector<UnitKind> &kinds)
{
    std::wstring s;
    for (UnitKind k : kinds) {
        if (!s.empty())
            s += L",";
        s += UnitKindKey(k);
    }
    return s.empty() ? std::wstring(L"none") : s;
}

bool NeedsSwitch(const Device &d)
{
    return IsSupportedKind(d.Kind) && d.Present && !(d.OnApollo() && !d.HasProblem);
}

DriverOwner OwnerOf(const std::wstring &service, const std::wstring &provider)
{
    if (service.empty())
        return OWNER_NONE;
    if (UnitIEq(service, APOLLO_SERVICE))
        return OWNER_APOLLO;
    if (UnitIEq(service, UA_SERVICE) || StartsI(provider, UA_PROVIDER))
        return OWNER_UA;
    return OWNER_OTHER;
}

const wchar_t *OwnerKey(DriverOwner o)
{
    switch (o) {
    case OWNER_APOLLO: return L"apollo";
    case OWNER_UA:     return L"ua";
    case OWNER_OTHER:  return L"other";
    default:           return L"none";
    }
}

static std::vector<unsigned long long> VersionParts(const std::wstring &v)
{
    std::vector<unsigned long long> parts;
    unsigned long long cur = 0;
    for (size_t i = 0; i <= v.size(); i++) {
        if (i == v.size() || v[i] == L'.') {
            parts.push_back(cur);
            cur = 0;
        } else if (v[i] >= L'0' && v[i] <= L'9' && cur < 100000000000ull) {
            cur = cur * 10 + (unsigned long long)(v[i] - L'0');
        }
    }
    while (!parts.empty() && parts.back() == 0)
        parts.pop_back();
    return parts;
}

int CompareDriverVersions(const std::wstring &a, const std::wstring &b)
{
    std::vector<unsigned long long> x = VersionParts(a), y = VersionParts(b);
    size_t n = x.size() > y.size() ? x.size() : y.size();
    for (size_t i = 0; i < n; i++) {
        unsigned long long p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q)
            return p < q ? -1 : 1;
    }
    return 0;
}

bool TextListsHwid(const std::wstring &text, const std::wstring &hwid)
{
    if (hwid.empty() || text.size() < hwid.size())
        return false;
    const std::wstring hay = Upper(text), needle = Upper(hwid);
    for (size_t pos = hay.find(needle); pos != std::wstring::npos; pos = hay.find(needle, pos + 1)) {
        bool before = pos == 0 || !IsIdChar(hay[pos - 1]);
        size_t end = pos + needle.size();
        bool after = end == hay.size() || !IsIdChar(hay[end]);
        if (before && after)
            return true;
    }
    return false;
}

std::vector<size_t> UnitsWithHwid(const std::vector<Device> &devs, const std::wstring &hwid)
{
    std::vector<size_t> r;
    for (size_t i = 0; i < devs.size(); i++)
        if (!hwid.empty() && UnitIEq(devs[i].Hwid, hwid))
            r.push_back(i);
    return r;
}

QuitUaDecision DecideQuitUa(const std::vector<Device> &present, const std::vector<UnitKind> &targets, size_t &other)
{
    bool anyTarget = false, leaving = false, unknown = false;
    for (auto &d : present) {
        if (!IsSupportedKind(d.Kind) || !KindIn(targets, d.Kind))
            continue;
        anyTarget = true;
        if (d.Service.empty())
            unknown = true;
        else if (d.Owner() == OWNER_UA)
            leaving = true;
    }
    if (!anyTarget)
        return QUIT_NO_UNIT;
    if (!leaving)
        return unknown ? QUIT_DRIVER_UNKNOWN : QUIT_NOTHING_TO_RELEASE;
    /* A unit that stays where it is (not a target: unselected, a Gen 2, another model) on UA's
       driver still needs UA's apps. */
    for (size_t i = 0; i < present.size(); i++) {
        const Device &d = present[i];
        if (IsSupportedKind(d.Kind) && KindIn(targets, d.Kind))
            continue;
        if (d.Owner() == OWNER_UA) {
            other = i;
            return QUIT_OTHER_ON_UA;
        }
    }
    return QUIT_CLOSE_APPS;
}

std::vector<size_t> UnitsToHandBack(const std::vector<Device> &all)
{
    std::vector<size_t> r;
    for (size_t i = 0; i < all.size(); i++)
        if (!all[i].IsX8p() && all[i].OnApollo())
            r.push_back(i);
    return r;
}

std::vector<size_t> UnitsOnOtherImage(const std::vector<Device> &present, const std::vector<std::wstring> &targetInstances,
                                      const std::wstring &packageVersion)
{
    std::vector<size_t> r;
    for (size_t i = 0; i < present.size(); i++) {
        const Device &d = present[i];
        bool target = false;
        for (auto &t : targetInstances)
            target = target || UnitIEq(d.InstanceId, t);
        if (target || !d.OnApollo())
            continue;
        if (packageVersion.empty() || d.DriverVersion.empty() ||
            CompareDriverVersions(d.DriverVersion, packageVersion) != 0)
            r.push_back(i);
    }
    return r;
}

bool PackageInUse(const std::vector<Device> &all, const std::wstring &published)
{
    for (auto &d : all)
        if (!published.empty() && UnitIEq(d.InfPath, published))
            return true;
    return false;
}

std::wstring DriverStoreFolder(const std::wstring &path)
{
    const std::wstring key = L"\\FILEREPOSITORY\\";
    std::wstring up = Upper(path);
    size_t at = up.find(key);
    if (at == std::wstring::npos)
        return std::wstring();
    size_t start = at + key.size();
    size_t end = up.find(L'\\', start);
    if (end == std::wstring::npos || end == start)
        return std::wstring();
    return up.substr(start, end - start);
}

int LoadedImageMatches(const std::wstring &loadedImagePath, const std::wstring &packageStorePath)
{
    std::wstring a = DriverStoreFolder(loadedImagePath), b = DriverStoreFolder(packageStorePath);
    if (a.empty() || b.empty())
        return -1;
    return a == b ? 1 : 0;
}
