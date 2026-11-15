/*
 * Apollo units: classification and the multi-unit decisions (units.h). No device or registry
 * access here: test_units.cpp links this file alone.
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

/* ASCII case-insensitive; hardware IDs, services and INF names are ASCII. */
bool UnitIEq(const std::wstring &a, const std::wstring &b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++)
        if (Up(a[i]) != Up(b[i]))
            return false;
    return true;
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
    /* PCI\VEN_1A00&DEV_0002&SUBSYS_00191A00&REV_00\8&5E6F7A8&1&000800200038 */
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

QuitUaDecision DecideQuitUa(const std::vector<Device> &present, size_t &other)
{
    const Device *x8p = nullptr;
    for (auto &d : present)
        if (d.IsX8p() && x8p == nullptr)
            x8p = &d;
    if (x8p == nullptr)
        return QUIT_NO_X8P;
    if (x8p->OnApollo())
        return QUIT_X8P_ON_APOLLO;
    if (x8p->Service.empty())
        return QUIT_X8P_DRIVER_UNKNOWN;
    for (size_t i = 0; i < present.size(); i++) {
        if (&present[i] == x8p || UnitIEq(present[i].InstanceId, x8p->InstanceId))
            continue;
        if (present[i].Owner() == OWNER_UA) {
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

std::vector<size_t> UnitsOnOtherImage(const std::vector<Device> &present, const std::wstring &targetInstance,
                                      const std::wstring &packageVersion)
{
    std::vector<size_t> r;
    for (size_t i = 0; i < present.size(); i++) {
        const Device &d = present[i];
        if (UnitIEq(d.InstanceId, targetInstance) || !d.OnApollo())
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
