/*
 * Devices and driver packages: find the Apollo units, force apollo.inf onto the units being
 * switched (UA's WHQL package ranks higher, so plain PnP never picks ours while UA's is installed),
 * hand units back to UA's driver and remove our packages again, and close UA's apps before PnP
 * swaps a unit's driver under them.
 *
 * apollo.inf lists two units, the x8p and the Twin X Gen 1; Setup switches the ones the user
 * ticks (--units). Every other Universal Audio device stays on UA's driver and is checked
 * unchanged. The rules follow tools\phase1\install.ps1 and step-ep.ps1 (bind, quit-ua),
 * revert.ps1 -AllUnits and revert-unit.ps1 (unbind); the decisions themselves are in units.cpp.
 */
#include "common.h"
#include <initguid.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <setupapi.h>
#include <newdev.h>
#include <tlhelp32.h>
#include <stddef.h>
#include <string.h>
#include "apollo_ioctl_min.h"

/* ------------------------------------------------------------------ */
/* properties                                                          */
/* ------------------------------------------------------------------ */

static bool PropBytes(DEVINST dn, const DEVPROPKEY &key, DEVPROPTYPE want, std::vector<BYTE> &buf)
{
    DEVPROPTYPE type = 0;
    ULONG size = 0;
    CONFIGRET cr = CM_Get_DevNode_PropertyW(dn, &key, &type, nullptr, &size, 0);
    if (cr != CR_BUFFER_SMALL || size == 0)
        return false;
    buf.assign((size_t)size + sizeof(WCHAR) * 2, 0);
    cr = CM_Get_DevNode_PropertyW(dn, &key, &type, buf.data(), &size, 0);
    return cr == CR_SUCCESS && type == want;
}

static std::wstring PropString(DEVINST dn, const DEVPROPKEY &key)
{
    std::vector<BYTE> buf;
    if (!PropBytes(dn, key, DEVPROP_TYPE_STRING, buf))
        return std::wstring();
    return std::wstring((const wchar_t *)buf.data());
}

static std::vector<std::wstring> PropStringList(DEVINST dn, const DEVPROPKEY &key)
{
    std::vector<std::wstring> r;
    std::vector<BYTE> buf;
    if (!PropBytes(dn, key, DEVPROP_TYPE_STRING_LIST, buf))
        return r;
    for (const wchar_t *p = (const wchar_t *)buf.data(); *p != L'\0'; p += wcslen(p) + 1)
        r.push_back(p);
    return r;
}

bool ProblemNeedsRestart(ULONG problem)
{
    /* NEED_RESTART, WILL_BE_REMOVED, DRIVER_FAILED_PRIOR_UNLOAD, HELD_FOR_EJECT */
    return problem == 14 || problem == 21 || problem == 38 || problem == 47;
}

static const wchar_t *ProblemText(ULONG problem)
{
    switch (problem) {
    case 10: return L"the device could not start (see apollo-diag log)";
    case 14: return L"a restart is needed";
    case 18: return L"the driver needs reinstalling";
    case 21: return L"Windows is removing the device";
    case 22: return L"the device is disabled";
    case 24: return L"the device is not present or not all of its drivers are installed";
    case 28: return L"no driver is installed";
    case 31: return L"the driver could not be loaded";
    case 37: return L"the driver's DriverEntry failed";
    case 38: return L"the previous driver is still loaded: restart";
    case 39: return L"the driver is missing or damaged";
    case 43: return L"the driver reported a failure";
    case 45: return L"the device is not connected";
    case 47: return L"the device is prepared for safe removal: replug or restart";
    case 52: return L"Windows refused the driver's signature (test signing not active yet, or the certificate is missing)";
    default: return L"see Device Manager";
    }
}

/* ------------------------------------------------------------------ */
/* devices                                                             */
/* ------------------------------------------------------------------ */

std::vector<Device> FindUaDevices(bool includePhantoms)
{
    std::vector<Device> out;
    ULONG flags = CM_GETIDLIST_FILTER_ENUMERATOR | (includePhantoms ? 0 : CM_GETIDLIST_FILTER_PRESENT);
    std::vector<wchar_t> list;
    CONFIGRET cr;
    do {
        ULONG len = 0;
        cr = CM_Get_Device_ID_List_SizeW(&len, L"PCI", flags);
        if (cr != CR_SUCCESS || len == 0)
            return out;
        list.assign(len, L'\0');
        cr = CM_Get_Device_ID_ListW(L"PCI", list.data(), len, flags);
    } while (cr == CR_BUFFER_SMALL);
    if (cr != CR_SUCCESS)
        return out;

    const std::wstring prefix = UA_PCI_PREFIX;
    for (const wchar_t *id = list.data(); *id != L'\0'; id += wcslen(id) + 1) {
        if (_wcsnicmp(id, prefix.c_str(), prefix.size()) != 0)
            continue;
        DEVINST dn = 0;
        if (CM_Locate_DevNodeW(&dn, (DEVINSTID_W)id,
                               includePhantoms ? CM_LOCATE_DEVNODE_PHANTOM : CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
            continue;
        Device d;
        d.InstanceId = id;
        /* The exact four-part ID decides which unit this is; never DEV_0002 alone (units.h). */
        d.Hwid = FourPartHwid(PropStringList(dn, DEVPKEY_Device_HardwareIds), d.InstanceId);
        d.Kind = ClassifyHwid(d.Hwid);
        d.Description = PropString(dn, DEVPKEY_Device_FriendlyName);
        if (d.Description.empty())
            d.Description = PropString(dn, DEVPKEY_Device_DeviceDesc);
        d.Service = PropString(dn, DEVPKEY_Device_Service);
        d.Provider = PropString(dn, DEVPKEY_Device_DriverProvider);
        d.InfPath = PropString(dn, DEVPKEY_Device_DriverInfPath);
        d.DriverVersion = PropString(dn, DEVPKEY_Device_DriverVersion);
        ULONG status = 0, problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, dn, 0) == CR_SUCCESS) {
            d.Present = true;
            d.Status = status;
            d.HasProblem = (status & DN_HAS_PROBLEM) != 0;
            d.Problem = d.HasProblem ? problem : 0;
        }
        out.push_back(d);
    }
    return out;
}

std::wstring DeviceLabel(const Device &d)
{
    return Fmt(L"%s (%s)", UnitKindName(d.Kind), d.InstanceId.c_str());
}

static const Device *FindByInstance(const std::vector<Device> &devs, const std::wstring &instance)
{
    for (auto &d : devs)
        if (EqualsI(d.InstanceId, instance))
            return &d;
    return nullptr;
}

static void PrintDevice(const Device &d)
{
    Out(L"  %s%s: %s", UnitKindName(d.Kind), d.Present ? L"" : L" (not connected now)", d.InstanceId.c_str());
    Out(L"    driver: %s%s%s%s", d.Service.empty() ? L"none" : d.Service.c_str(),
        d.Provider.empty() ? L"" : (L" from " + d.Provider).c_str(),
        d.DriverVersion.empty() ? L"" : (L" " + d.DriverVersion).c_str(),
        d.InfPath.empty() ? L"" : (L" (" + d.InfPath + L")").c_str());
    if (d.HasProblem)
        Out(L"    problem %lu: %s", d.Problem, ProblemText(d.Problem));
    else if (d.Present)
        Out(L"    started");
}

/* Every connected unit other than skip, as it is now (bind/unbind --unit compare them afterwards). */
static std::vector<Device> OtherUnits(const std::vector<Device> &present, const std::wstring &skip)
{
    std::vector<Device> r;
    for (auto &d : present)
        if (!EqualsI(d.InstanceId, skip))
            r.push_back(d);
    return r;
}

static void PrintOthers(const std::vector<Device> &others)
{
    if (others.empty())
        return;
    Out(L"Other Universal Audio devices (left as they are):");
    for (auto &o : others)
        PrintDevice(o);
}

/* install.ps1 / revert-unit.ps1: every other unit must keep the driver and state it had.
   Returns false (and prints WARNING lines) if one changed. */
static bool CheckOthersUnchanged(const std::vector<Device> &before, const std::vector<Device> &after)
{
    bool ok = true;
    for (auto &o : before) {
        const Device *p = FindByInstance(after, o.InstanceId);
        if (p == nullptr) {
            Out(L"WARNING: the %s is no longer connected (switched off or unplugged?).", DeviceLabel(o).c_str());
        } else if (!EqualsI(p->Service, o.Service) || !EqualsI(p->InfPath, o.InfPath) ||
                   p->DriverVersion != o.DriverVersion || p->HasProblem != o.HasProblem || p->Problem != o.Problem) {
            ok = false;
            Out(L"WARNING: the %s CHANGED: %s %s %s problem %lu -> %s %s %s problem %lu.", DeviceLabel(o).c_str(),
                o.Service.c_str(), o.InfPath.c_str(), o.DriverVersion.c_str(), o.Problem, p->Service.c_str(),
                p->InfPath.c_str(), p->DriverVersion.c_str(), p->Problem);
        } else {
            Out(L"Unchanged: the %s on %s (%s).", UnitKindName(p->Kind), p->Service.empty() ? L"no driver" : p->Service.c_str(),
                p->InfPath.c_str());
        }
    }
    return ok;
}

/* GUID_DEVINTERFACE_APOLLO interfaces, each with the device instance it belongs to. */
std::vector<ApolloInterface> FindApolloInterfaces()
{
    std::vector<ApolloInterface> r;
    GUID g = HELPER_GUID_DEVINTERFACE_APOLLO;
    std::vector<wchar_t> list;
    CONFIGRET cr = CR_FAILURE;
    for (int tries = 0; tries < 4; tries++) {
        ULONG len = 0;
        if (CM_Get_Device_Interface_List_SizeW(&len, &g, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS ||
            len <= 1)
            return r;
        list.assign(len, L'\0');
        cr = CM_Get_Device_Interface_ListW(&g, nullptr, list.data(), len, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (cr != CR_BUFFER_SMALL)
            break;
    }
    if (cr != CR_SUCCESS)
        return r;
    for (const wchar_t *p = list.data(); *p != L'\0'; p += wcslen(p) + 1) {
        ApolloInterface a;
        a.Path = p;
        DEVPROPTYPE type = 0;
        ULONG size = 0;
        if (CM_Get_Device_Interface_PropertyW(p, &DEVPKEY_Device_InstanceId, &type, nullptr, &size, 0) == CR_BUFFER_SMALL &&
            size > 0) {
            std::vector<BYTE> buf((size_t)size + sizeof(WCHAR) * 2, 0);
            if (CM_Get_Device_Interface_PropertyW(p, &DEVPKEY_Device_InstanceId, &type, buf.data(), &size, 0) == CR_SUCCESS &&
                type == DEVPROP_TYPE_STRING)
                a.InstanceId = (const wchar_t *)buf.data();
        }
        r.push_back(a);
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* packages                                                            */
/* ------------------------------------------------------------------ */

static std::wstring InfField(HINF h, const wchar_t *section, const wchar_t *key, DWORD field)
{
    INFCONTEXT ctx;
    wchar_t buf[512];
    DWORD need = 0;
    if (!SetupFindFirstLineW(h, section, key, &ctx))
        return std::wstring();
    if (!SetupGetStringFieldW(&ctx, field, buf, 512, &need))
        return std::wstring();
    return buf;
}

/* The version part of an INF's DriverVer ("" if unreadable). */
static std::wstring InfVersion(const std::wstring &path)
{
    HINF h = SetupOpenInfFileW(path.c_str(), nullptr, INF_STYLE_WIN4, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return std::wstring();
    std::wstring v = InfField(h, L"Version", L"DriverVer", 2);
    SetupCloseInfFile(h);
    return v;
}

/* The whole INF as text, for a hardware-ID search: UTF-16 with a BOM, else bytes widened one to
   one (hardware IDs are ASCII). */
static bool ReadInfText(const std::wstring &path, std::wstring &text)
{
    std::vector<BYTE> b;
    text.clear();
    if (!ReadFileBytes(path, b))
        return false;
    if (b.size() >= 2 && b[0] == 0xFF && b[1] == 0xFE) {
        text.resize((b.size() - 2) / sizeof(wchar_t));
        if (!text.empty())
            memcpy(&text[0], b.data() + 2, text.size() * sizeof(wchar_t));
        return true;
    }
    size_t start = (b.size() >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) ? 3 : 0;
    text.reserve(b.size());
    for (size_t i = start; i < b.size(); i++)
        text += (wchar_t)b[i];
    return true;
}

/* Who signed the catalog an INF verifies against (the "Signer Name" pnputil shows), "" if it
   does not verify. Read-only. */
static std::wstring InfSigner(const std::wstring &path)
{
    SP_INF_SIGNER_INFO_W si = {};
    si.cbSize = sizeof(si);
    if (!SetupVerifyInfFileW(path.c_str(), nullptr, &si))
        return std::wstring();
    return si.DigitalSigner;
}

/* One pass over %windir%\INF\oem*.inf: our packages (original name apollo.inf, provider Avium)
   and UA's (original name uad2pcie.inf, provider Universal Audio). Either list may be null. */
static void ScanDriverStore(std::vector<Package> *apollo, std::vector<Package> *ua)
{
    wchar_t win[MAX_PATH];
    UINT n = GetWindowsDirectoryW(win, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return;
    std::wstring dir = std::wstring(win) + L"\\INF\\";
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"oem*.inf").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE)
        return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        std::wstring path = dir + fd.cFileName;

        DWORD size = 0;
        if (!SetupGetInfInformationW(path.c_str(), INFINFO_INF_NAME_IS_ABSOLUTE, nullptr, 0, &size) || size == 0)
            continue;
        std::vector<BYTE> info(size);
        if (!SetupGetInfInformationW(path.c_str(), INFINFO_INF_NAME_IS_ABSOLUTE, (PSP_INF_INFORMATION)info.data(),
                                     size, nullptr))
            continue;
        SP_ORIGINAL_FILE_INFO_W ofi = {};
        ofi.cbSize = sizeof(ofi);
        if (!SetupQueryInfOriginalFileInformationW((PSP_INF_INFORMATION)info.data(), 0, nullptr, &ofi))
            continue;
        std::wstring orig = ofi.OriginalInfName;
        size_t slash = orig.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            orig = orig.substr(slash + 1);
        bool isApollo = apollo != nullptr && EqualsI(orig, APOLLO_INF_NAME);
        bool isUa = ua != nullptr && EqualsI(orig, UA_INF_NAME);
        if (!isApollo && !isUa)
            continue;

        HINF h = SetupOpenInfFileW(path.c_str(), nullptr, INF_STYLE_WIN4, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        Package p;
        p.Provider = InfField(h, L"Version", L"Provider", 1);
        std::wstring date = InfField(h, L"Version", L"DriverVer", 1);
        p.Version = InfField(h, L"Version", L"DriverVer", 2);
        SetupCloseInfFile(h);
        p.Published = fd.cFileName;
        p.PublishedPath = path;
        p.DriverVer = date + (p.Version.empty() ? L"" : L", " + p.Version);
        if (isApollo && EqualsI(p.Provider, APOLLO_PROVIDER)) {
            apollo->push_back(p);
        } else if (isUa && p.Provider.size() >= wcslen(UA_PROVIDER) &&
                   EqualsI(p.Provider.substr(0, wcslen(UA_PROVIDER)), UA_PROVIDER)) {
            p.Signer = InfSigner(path);
            ua->push_back(p);
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

std::vector<Package> FindApolloPackages()
{
    std::vector<Package> out;
    ScanDriverStore(&out, nullptr);
    return out;
}

std::vector<Package> FindUaPackages()
{
    std::vector<Package> out;
    ScanDriverStore(nullptr, &out);
    return out;
}

/* revert-unit.ps1 Find-UaInf: uad2pcie.inf, provider Universal Audio, signed by Microsoft, and its
   INF lists this exact hardware ID; the newest if several. */
bool FindUaPackageFor(const std::vector<Package> &uaPackages, const std::wstring &hwid, Package &out)
{
    bool found = false;
    if (!IsFourPartUaHwid(hwid))
        return false;
    for (auto &p : uaPackages) {
        if (p.Signer.size() < 10 || !EqualsI(p.Signer.substr(0, 10), L"Microsoft "))
            continue;
        std::wstring text;
        if (!ReadInfText(p.PublishedPath, text) || !TextListsHwid(text, hwid))
            continue;
        if (!found || CompareDriverVersions(p.Version, out.Version) > 0) {
            out = p;
            found = true;
        }
    }
    return found;
}

/* ------------------------------------------------------------------ */
/* moving a unit between drivers                                       */
/* ------------------------------------------------------------------ */

/* devcon update <inf> <hwid>: Windows installs the package on every connected device with this
   exact four-part hardware ID, and on nothing else. Never a shorter ID. */
static bool ForceDriver(const std::wstring &hwid, const std::wstring &inf, bool &reboot)
{
    if (!IsFourPartUaHwid(hwid)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    BOOL rb = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, hwid.c_str(), inf.c_str(),
                                            INSTALLFLAG_FORCE | INSTALLFLAG_NONINTERACTIVE, &rb))
        return false;
    if (rb)
        reboot = true;
    return true;
}

/* revert-unit.ps1 step 5: this unit back onto UA's package. */
static bool MoveToUa(const Device &d, const Package &ua, bool &reboot)
{
    Out(L"Moving the %s back to UA's driver (%s, %s)...", DeviceLabel(d).c_str(), ua.Published.c_str(),
        ua.DriverVer.c_str());
    if (!ForceDriver(d.Hwid, ua.PublishedPath, reboot)) {
        Err(L"  failed: %s", ErrText(GetLastError()).c_str());
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* the loaded driver image                                             */
/* ------------------------------------------------------------------ */

typedef LONG (WINAPI *NtQuerySystemInformationFn)(ULONG, PVOID, ULONG, PULONG);

/* SYSTEM_MODULE_INFORMATION (class 11) entries; only the names are read (the kernel may zero the
   addresses for callers without SeDebugPrivilege). */
struct HelperModuleInfo {
    HANDLE Section;
    PVOID  MappedBase;
    PVOID  ImageBase;
    ULONG  ImageSize;
    ULONG  Flags;
    USHORT LoadOrderIndex;
    USHORT InitOrderIndex;
    USHORT LoadCount;
    USHORT OffsetToFileName;
    UCHAR  FullPathName[256];
};
struct HelperModules {
    ULONG            NumberOfModules;
    HelperModuleInfo Modules[1];
};

/* "\SystemRoot\System32\DriverStore\FileRepository\apollo.inf_amd64_...\apollo.sys", or "". */
std::wstring LoadedApolloImage()
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    NtQuerySystemInformationFn q = ntdll ? (NtQuerySystemInformationFn)(void *)GetProcAddress(ntdll,
                                           "NtQuerySystemInformation") : nullptr;
    if (q == nullptr)
        return std::wstring();
    std::vector<BYTE> buf(256 * 1024);
    LONG st = 0;
    for (int tries = 0; tries < 5; tries++) {
        ULONG need = 0;
        st = q(11, buf.data(), (ULONG)buf.size(), &need);
        if (st != (LONG)0xC0000004)        /* STATUS_INFO_LENGTH_MISMATCH */
            break;
        buf.assign((size_t)need + 64 * 1024, 0);
    }
    if (st < 0 || buf.size() < sizeof(ULONG))
        return std::wstring();
    const HelperModules *m = (const HelperModules *)buf.data();
    size_t max = (buf.size() - offsetof(HelperModules, Modules)) / sizeof(HelperModuleInfo);
    for (ULONG i = 0; i < m->NumberOfModules && i < max; i++) {
        const HelperModuleInfo &e = m->Modules[i];
        char path[257];
        memcpy(path, e.FullPathName, 256);
        path[256] = '\0';
        size_t off = e.OffsetToFileName < 256 ? e.OffsetToFileName : 0;
        if (_stricmp(path + off, "apollo.sys") != 0)
            continue;
        std::wstring w;
        for (const char *c = path; *c; c++)
            w += (wchar_t)(unsigned char)*c;
        return w;
    }
    return std::wstring();
}

/* ------------------------------------------------------------------ */
/* bind                                                                */
/* ------------------------------------------------------------------ */

static void ExplainInstallError(DWORD e)
{
    switch (e) {
    case ERROR_NO_SUCH_DEVINST:
        Err(L"The unit disappeared during the install: check the Thunderbolt cable and power, then retry.");
        break;
    case ERROR_NO_CATALOG_FOR_OEM_INF:
    case ERROR_AUTHENTICODE_PUBLISHER_NOT_TRUSTED:
    case ERROR_AUTHENTICODE_TRUSTED_PUBLISHER:
    case ERROR_AUTHENTICODE_TRUST_NOT_ESTABLISHED:
    case ERROR_DRIVER_STORE_ADD_FAILED:
    case (DWORD)TRUST_E_NOSIGNATURE:
    case (DWORD)CERT_E_UNTRUSTEDROOT:
    case (DWORD)TRUST_E_SUBJECT_NOT_TRUSTED:
    case (DWORD)CERT_E_CHAINING:
        Err(L"Windows does not trust the package's signature. Apollo setup signs the driver on this PC and trusts");
        Err(L"that certificate (Trusted Root, Trusted Publishers): run Apollo setup again, then retry.");
        break;
    case ERROR_IN_WOW64:
        Err(L"This must run as a 64-bit process.");
        break;
    default:
        break;
    }
}

/* How to move one unit back to UA's driver, for messages. */
static std::wstring HandBackHint(const Device &d)
{
    if (IsSupportedKind(d.Kind))
        return Fmt(L"apollo-helper unbind --unit %s (elevated)", UnitKindKey(d.Kind));
    return L"Device Manager: that device, Update driver, UA's driver";
}

/*
 * apollo.inf lists both supported units. One that is not being switched, is not on UA's driver and
 * has no UA driver installed has nothing ranked above ours, so Windows may give it the Apollo
 * driver by itself when it next connects. Said, never done.
 */
static void NoteUnitsOurInfCouldTake(const std::wstring &inf, const std::vector<Device> &all,
                                     const std::vector<Package> &ua, const std::vector<UnitKind> &targets)
{
    std::wstring text;
    if (!ReadInfText(inf, text))
        return;
    std::vector<std::wstring> said;
    for (auto &d : all) {
        if (KindIn(targets, d.Kind) || d.OnApollo() || d.Owner() == OWNER_UA || d.Hwid.empty() ||
            !TextListsHwid(text, d.Hwid))
            continue;
        bool dup = false;
        for (auto &h : said)
            dup = dup || EqualsI(h, d.Hwid);
        Package p;
        if (dup || FindUaPackageFor(ua, d.Hwid, p))
            continue;
        said.push_back(d.Hwid);
        /* One line: Setup copies "Note:" lines into its Results page. */
        Out(L"Note: the %s has no UA driver on this PC and was not switched; this Apollo package lists it, so Windows "
            L"may give it the Apollo driver by itself when it next connects. Install UA Connect to keep it on UA's "
            L"driver.", UnitKindName(d.Kind));
    }
}

struct BindTarget {
    Device Before;
    bool   Installed = false;       /* UpdateDriverForPlugAndPlayDevices succeeded */
    bool   Restart = false;         /* this unit finishes after a restart */
    bool   Running = false;         /* on our driver, started, after the install */
    bool   Failed = false;
    std::wstring Why;
};

/*
 * bind <inf> --units ...: stage the package, then force it onto each named unit that is connected,
 * by its exact four-part hardware ID (UA's WHQL package ranks higher, so plain PnP never picks
 * ours while UA's is installed). The x8p first, then the Twin X (step-ep.ps1). Every other
 * Universal Audio device is checked unchanged afterwards.
 */
int CmdBind(const std::wstring &infArg, const std::vector<UnitKind> &units, bool stageOnly)
{
    if (!RequireElevated(L"bind"))
        return EXIT_NOT_ELEVATED;
    wchar_t full[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(infArg.c_str(), MAX_PATH * 2, full, nullptr);
    if (n == 0 || n >= MAX_PATH * 2 || GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) {
        Err(L"No driver package at %s", infArg.c_str());
        return EXIT_ERROR;
    }

    std::vector<Device> present = FindUaDevices(false);
    if (!stageOnly) {
        for (UnitKind k : units) {
            std::vector<size_t> same = UnitsWithHwid(present, UnitHwid(k));
            if (same.size() > 1) {
                Err(L"%zu connected devices carry the %s's hardware ID %s; the install would move all of them.",
                    same.size(), UnitKindName(k), UnitHwid(k));
                for (size_t i : same)
                    Err(L"  %s", present[i].InstanceId.c_str());
                Err(L"Connect one %s at a time. Nothing changed.", UnitKindName(k));
                return EXIT_BLOCKED;
            }
        }
    }

    /* Stage first: the published name and driver-store folder are needed afterwards. */
    wchar_t published[MAX_PATH] = L"";
    if (!SetupCopyOEMInfW(full, nullptr, SPOST_PATH, 0, published, MAX_PATH, nullptr, nullptr)) {
        DWORD e = GetLastError();
        Err(L"Staging the driver package failed: %s", ErrText(e).c_str());
        ExplainInstallError(e);
        return EXIT_ERROR;
    }
    const wchar_t *base = wcsrchr(published, L'\\');
    std::wstring publishedName = base ? base + 1 : published;
    wchar_t store[MAX_PATH * 2] = L"";
    DWORD need = 0;
    if (!SetupGetInfDriverStoreLocationW(published, nullptr, nullptr, store, MAX_PATH * 2, &need))
        store[0] = L'\0';
    Out(L"The driver package is staged as %s.", publishedName.c_str());

    std::vector<BindTarget> targets;
    for (UnitKind k : units) {
        std::vector<size_t> same = UnitsWithHwid(present, UnitHwid(k));
        if (same.empty()) {
            if (!stageOnly)
                Out(L"Unit: the %s is not connected; connect it, then run \"Connect Apollo units\" from the Start menu.",
                    UnitKindName(k));
            continue;
        }
        BindTarget t;
        t.Before = present[same[0]];
        targets.push_back(t);
    }
    if (stageOnly || targets.empty()) {
        if (stageOnly)
            Out(L"--stage-only: no device was touched.");
        else
            Out(L"No unit to switch is connected: the driver package is staged for when one is.");
        PrintOthers(present);
        NoteUnitsOurInfCouldTake(full, FindUaDevices(true), FindUaPackages(), units);
        return EXIT_NO_DEVICE;
    }

    std::vector<std::wstring> targetIds;
    for (auto &t : targets)
        targetIds.push_back(t.Before.InstanceId);
    std::vector<Device> others;
    for (auto &d : present) {
        bool isTarget = false;
        for (auto &id : targetIds)
            isTarget = isTarget || EqualsI(id, d.InstanceId);
        if (!isTarget)
            others.push_back(d);
    }

    /* install.ps1: one service, one loaded image. A unit on our driver that is not being switched
       keeps apollo.sys loaded, and a unit switched now would join that image, not this package's. */
    std::wstring pkgVer = InfVersion(full);
    std::vector<size_t> blockers = UnitsOnOtherImage(present, targetIds, pkgVer);
    if (!blockers.empty()) {
        for (size_t i : blockers) {
            const Device &o = present[i];
            Err(L"The %s runs the Apollo driver %s; this package is %s.", DeviceLabel(o).c_str(),
                o.DriverVersion.empty() ? L"(version unknown)" : o.DriverVersion.c_str(),
                pkgVer.empty() ? L"(version unreadable)" : pkgVer.c_str());
        }
        Err(L"Every unit on the Apollo driver shares one loaded driver image, so the units switched now would keep");
        Err(L"running that one instead of this package. Switch that unit too, or move it back to UA's driver first:");
        for (size_t i : blockers)
            Err(L"  %s", HandBackHint(present[i]).c_str());
        Err(L"Nothing changed.");
        return EXIT_BLOCKED;
    }

    for (auto &t : targets) {
        Out(L"%s before:", UnitKindName(t.Before.Kind));
        PrintDevice(t.Before);
    }
    PrintOthers(others);

    for (auto &t : targets) {
        Out(L"Installing %s on the %s only (%s), forced over a higher-ranked driver...", full,
            UnitKindName(t.Before.Kind), t.Before.Hwid.c_str());
        bool rb = false;
        if (!ForceDriver(t.Before.Hwid, full, rb)) {
            DWORD e = GetLastError();
            Err(L"Installing the driver on the %s failed: %s", UnitKindName(t.Before.Kind), ErrText(e).c_str());
            ExplainInstallError(e);
            t.Failed = true;
            t.Why = ErrText(e);
            continue;
        }
        t.Installed = true;
        t.Restart = rb;
        Sleep(2000);
    }
    Sleep(1000);

    std::vector<Device> nowPresent = FindUaDevices(false);
    bool restart = false, anyRunning = false;
    for (auto &t : targets) {
        if (!t.Installed)
            continue;
        const Device *found = FindByInstance(nowPresent, t.Before.InstanceId);
        for (size_t i = 0; found == nullptr && i < nowPresent.size(); i++)
            if (nowPresent[i].Kind == t.Before.Kind)
                found = &nowPresent[i];
        if (found == nullptr) {
            Out(L"The %s is not visible right now (it may be restarting).", UnitKindName(t.Before.Kind));
            continue;
        }
        Device after = *found;
        Out(L"%s after:", UnitKindName(after.Kind));
        PrintDevice(after);
        if (after.HasProblem && ProblemNeedsRestart(after.Problem)) {
            t.Restart = true;
        } else if (after.HasProblem) {
            std::wstring opts;
            int ci = -1;
            if (after.Problem == 52 && !TestSigningActive(opts, ci)) {
                /* Unsigned-driver refusal: test signing is on for the next start only. */
                Out(L"Test signing is not active yet: the driver loads on the %s after the restart.", UnitKindName(after.Kind));
                t.Restart = true;
            } else {
                Err(L"The %s reports problem %lu: %s", UnitKindName(after.Kind), after.Problem, ProblemText(after.Problem));
                t.Failed = true;
                t.Why = Fmt(L"problem %lu, %s", after.Problem, ProblemText(after.Problem));
            }
        } else if (!after.OnApollo()) {
            if (!t.Restart) {
                Err(L"The %s is not on the Apollo driver after the install (service '%s').", UnitKindName(after.Kind),
                    after.Service.c_str());
                t.Failed = true;
                t.Why = L"Windows kept it on its previous driver";
            }
        } else {
            t.Running = !t.Restart;
        }
        if (t.Restart)
            restart = true;
        if (t.Running)
            anyRunning = true;
    }
    /* Only the named units' hardware IDs: no other device may have moved (install.ps1). */
    CheckOthersUnchanged(others, nowPresent);

    /* One loaded image for every unit on the service: a unit switched while another unit (the
       other one switched just now included) kept the old build loaded runs that old build. */
    if (anyRunning && !restart) {
        std::wstring loaded = LoadedApolloImage();
        int m = LoadedImageMatches(loaded, store);
        if (m == 1) {
            Out(L"The loaded apollo.sys is this package's (%s).", DriverStoreFolder(loaded).c_str());
        } else if (m == 0) {
            restart = true;
            for (auto &t : targets)
                if (t.Running) {
                    t.Running = false;
                    t.Restart = true;
                }
            Out(L"An earlier build of apollo.sys is still loaded (%s): every unit on the Apollo driver shares one loaded",
                DriverStoreFolder(loaded).c_str());
            Out(L"driver, so this build (%s) loads at the next restart of Windows.", DriverStoreFolder(store).c_str());
        } else {
            Out(L"Could not tell which build of apollo.sys is loaded ('%s').", loaded.c_str());
        }
    }

    /* Older copies of our package nobody uses any more: not this one, and not one another unit
       (connected or remembered) is installed from. Not while a restart is pending: the old driver
       may still be loaded from its driver-store folder. Without SUOI_FORCEDELETE Windows refuses
       to delete a package a device is installed with. */
    if (!restart && anyRunning) {
        std::vector<Device> all = FindUaDevices(true);
        for (auto &p : FindApolloPackages()) {
            if (EqualsI(p.Published, publishedName))
                continue;
            if (PackageInUse(all, p.Published)) {
                Out(L"Kept %s (%s): an Apollo unit is installed from it.", p.Published.c_str(), p.DriverVer.c_str());
                continue;
            }
            if (SetupUninstallOEMInfW(p.Published.c_str(), 0, nullptr))
                Out(L"Removed an older, unused Apollo package: %s (%s)", p.Published.c_str(), p.DriverVer.c_str());
        }
    }
    NoteUnitsOurInfCouldTake(full, FindUaDevices(true), FindUaPackages(), units);

    /* One line per unit: Setup copies "Unit:" lines into its Results page. */
    int failed = 0;
    for (auto &t : targets) {
        const wchar_t *name = UnitKindName(t.Before.Kind);
        if (t.Failed) {
            failed++;
            Out(L"Unit: the %s was NOT switched (%s). It stays on its previous driver; fix the cause, then run "
                L"\"Connect Apollo units\" from the Start menu.", name, t.Why.c_str());
        } else if (t.Restart) {
            Out(L"Unit: the %s switches to the Apollo driver at the next restart of Windows.", name);
        } else if (t.Running) {
            Out(L"Unit: the %s now runs on the Apollo driver.", name);
        } else {
            Out(L"Unit: the %s has the Apollo driver installed; it was not visible straight afterwards (run "
                L"'apollo-helper check' in a minute to confirm).", name);
        }
    }
    if (restart)
        Out(L"Restart Windows to finish switching to the Apollo driver.");
    if (failed > 0)
        return EXIT_ERROR;
    return restart ? EXIT_REBOOT : EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* unbind                                                              */
/* ------------------------------------------------------------------ */

/* Fallback when DiUninstallDriver refuses: uninstall the devices using the package, then force-delete it. */
static bool RemovePackageHard(const Package &p, bool &reboot)
{
    for (auto &d : FindUaDevices(true)) {
        if (!EqualsI(d.InfPath, p.Published))
            continue;
        HDEVINFO set = SetupDiCreateDeviceInfoList(nullptr, nullptr);
        if (set == INVALID_HANDLE_VALUE)
            continue;
        SP_DEVINFO_DATA dd = {};
        dd.cbSize = sizeof(dd);
        if (SetupDiOpenDeviceInfoW(set, d.InstanceId.c_str(), nullptr, 0, &dd)) {
            BOOL rb = FALSE;
            if (DiUninstallDevice(nullptr, set, &dd, 0, &rb))
                Out(L"  uninstalled device %s", d.InstanceId.c_str());
            else
                Err(L"  could not uninstall device %s: %s", d.InstanceId.c_str(), ErrText(GetLastError()).c_str());
            if (rb)
                reboot = true;
        }
        SetupDiDestroyDeviceInfoList(set);
    }
    if (SetupUninstallOEMInfW(p.Published.c_str(), SUOI_FORCEDELETE, nullptr))
        return true;
    Err(L"  could not delete %s: %s", p.Published.c_str(), ErrText(GetLastError()).c_str());
    return false;
}

/* The 'apollo' kernel service stays behind after its package is gone; delete it once no device
   (connected or remembered) is set up with it and it is not running. */
static void RemoveOrphanService(bool &reboot)
{
    for (auto &d : FindUaDevices(true)) {
        if (d.OnApollo()) {
            Out(L"The apollo service is still assigned to %s; it is removed after a restart and another uninstall.",
                d.InstanceId.c_str());
            return;
        }
    }
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm == nullptr)
        return;
    SC_HANDLE svc = OpenServiceW(scm, APOLLO_SERVICE, DELETE | SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    if (svc == nullptr) {
        DWORD e = GetLastError();
        CloseServiceHandle(scm);
        /* No service, but 'regdefaults' may have created Services\apollo\Parameters before any x8p was
           ever connected: a key with no service Type is that leftover, not a service. */
        DWORD type = 0, size = sizeof(type);
        if (e == ERROR_SERVICE_DOES_NOT_EXIST &&
            RegGetValueW(HKEY_LOCAL_MACHINE, APOLLO_SERVICE_KEY, L"Type", RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY,
                         nullptr, &type, &size) == ERROR_FILE_NOT_FOUND) {
            HKEY k = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, APOLLO_SERVICE_KEY, 0, DELETE | KEY_READ | KEY_WOW64_64KEY, &k) ==
                ERROR_SUCCESS) {
                LSTATUS st = RegDeleteTreeW(k, nullptr);
                RegCloseKey(k);
                if (st == ERROR_SUCCESS && RegDeleteKeyExW(HKEY_LOCAL_MACHINE, APOLLO_SERVICE_KEY, KEY_WOW64_64KEY, 0) ==
                    ERROR_SUCCESS)
                    Out(L"Removed the driver settings Setup had written (Services\\apollo, no service behind it).");
            }
        }
        return;
    }
    SERVICE_STATUS st = {};
    std::vector<BYTE> cfgBuf(8192);
    DWORD need = 0;
    QUERY_SERVICE_CONFIGW *cfg = (QUERY_SERVICE_CONFIGW *)cfgBuf.data();
    bool ours = QueryServiceConfigW(svc, cfg, (DWORD)cfgBuf.size(), &need) &&
                cfg->dwServiceType == SERVICE_KERNEL_DRIVER && cfg->lpBinaryPathName != nullptr &&
                ContainsI(cfg->lpBinaryPathName, L"apollo.sys");
    if (!ours) {
        Out(L"A service named 'apollo' exists but does not look like this driver's; left alone.");
    } else if (QueryServiceStatus(svc, &st) && st.dwCurrentState != SERVICE_STOPPED) {
        Out(L"The apollo driver is still loaded; Windows unloads it at the next restart.");
        reboot = true;
    } else if (DeleteService(svc)) {
        Out(L"Removed the leftover 'apollo' service entry (and its Parameters values).");
    } else if (GetLastError() == ERROR_SERVICE_MARKED_FOR_DELETE) {
        Out(L"The 'apollo' service entry is already marked for removal at the next restart.");
    } else {
        Err(L"Could not remove the 'apollo' service entry: %s", ErrText(GetLastError()).c_str());
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
}

/* "the x8p and the Apollo Twin X": the units a message is about. */
static std::wstring UnitNames(const std::vector<Device> &devs)
{
    std::vector<std::wstring> names;
    for (auto &d : devs) {
        std::wstring n = d.IsX8p() ? L"the x8p" : (L"the " + std::wstring(UnitKindName(d.Kind)));
        bool dup = false;
        for (auto &x : names)
            dup = dup || x == n;
        if (!dup)
            names.push_back(n);
    }
    std::wstring s;
    for (size_t i = 0; i < names.size(); i++)
        s += (i == 0 ? L"" : (i + 1 == names.size() ? L" and " : L", ")) + names[i];
    return s;
}

/*
 * Every unit (the uninstaller): revert.ps1 -AllUnits, made safe for a second unit. Removing our
 * package moves every unit installed from it, so each unit other than the x8p that is on our
 * driver goes back to UA's driver first, the way revert-unit.ps1 does it. If UA's driver for such
 * a unit is not installed it would be left with no driver at all: stop before changing anything.
 */
static int UnbindAll()
{
    bool reboot = false;
    std::vector<Device> all = FindUaDevices(true);
    std::vector<Package> ours, ua;
    ScanDriverStore(&ours, &ua);

    std::vector<Device> wasOnApollo;
    for (auto &d : all)
        if (d.OnApollo())
            wasOnApollo.push_back(d);
    if (!all.empty()) {
        Out(L"Universal Audio devices (connected, or remembered by Windows):");
        for (auto &d : all)
            PrintDevice(d);
    }

    /* 1. Units other than the x8p on our driver, and UA's driver for each. */
    std::vector<size_t> back = UnitsToHandBack(all);
    std::vector<Package> uaFor(back.size());
    std::vector<bool> missing(back.size(), false);
    bool blocked = false;
    for (size_t k = 0; k < back.size(); k++) {
        const Device &d = all[back[k]];
        missing[k] = !FindUaPackageFor(ua, d.Hwid, uaFor[k]);
        blocked = blocked || missing[k];
    }
    if (!back.empty()) {
        Out(L"Units other than the x8p on the Apollo driver, which go back to UA's driver first:");
        for (size_t k = 0; k < back.size(); k++) {
            const Device &d = all[back[k]];
            if (missing[k])
                Out(L"  - %s (%s): UA's driver for it is NOT installed (%s from Universal Audio, signed by Microsoft,"
                    L" listing %s)", DeviceLabel(d).c_str(), d.Present ? L"connected" : L"not connected now", UA_INF_NAME,
                    d.Hwid.empty() ? L"its hardware ID" : d.Hwid.c_str());
            else
                Out(L"  %s (%s): UA's driver %s (%s)", DeviceLabel(d).c_str(), d.Present ? L"connected" : L"not connected now",
                    uaFor[k].Published.c_str(), uaFor[k].DriverVer.c_str());
        }
    }
    if (blocked) {
        Err(L"Removing the Apollo driver now would leave that unit with no driver at all. Install UA Connect (UA's");
        Err(L"driver) first, or move the unit to another driver yourself, then run this again. Nothing changed.");
        return EXIT_BLOCKED;
    }

    /* 2. Hand back the connected ones. One call per hardware ID: Windows moves every connected
       device with that ID. A remembered one gets UA's driver when it next connects, because
       ours is gone by then. */
    std::vector<std::wstring> movedIds;
    std::vector<size_t> moved;
    for (size_t k = 0; k < back.size(); k++) {
        const Device &d = all[back[k]];
        if (!d.Present) {
            Out(L"The %s is not connected: when it next connects, Windows gives it UA's driver (%s).",
                DeviceLabel(d).c_str(), uaFor[k].Published.c_str());
            continue;
        }
        bool dup = false;
        for (auto &h : movedIds)
            dup = dup || EqualsI(h, d.Hwid);
        if (dup || MoveToUa(d, uaFor[k], reboot)) {
            if (!dup)
                movedIds.push_back(d.Hwid);
            moved.push_back(back[k]);
        } else {
            Out(L"  Removing the Apollo package below still hands it to UA's driver (the next-best one).");
        }
    }
    if (!moved.empty()) {
        Sleep(3000);
        std::vector<Device> now = FindUaDevices(false);
        for (size_t i : moved) {
            const Device *p = FindByInstance(now, all[i].InstanceId);
            if (p == nullptr) {
                Out(L"  The %s is not visible right now (restarting, or switched off).", UnitKindName(all[i].Kind));
                continue;
            }
            Out(L"  The %s is now on %s (%s %s)%s", UnitKindName(p->Kind), p->Service.empty() ? L"no driver" : p->Service.c_str(),
                p->InfPath.c_str(), p->DriverVersion.c_str(), p->OnApollo() ? L": still ours until a restart" : L".");
            if (p->OnApollo() || (p->HasProblem && ProblemNeedsRestart(p->Problem)))
                reboot = true;
        }
    }

    /* The x8p goes back with the package, to the next-best driver: UA's, if there is one. */
    for (auto &d : all) {
        Package p;
        if (d.IsX8p() && d.OnApollo() && !FindUaPackageFor(ua, d.Hwid, p)) {
            Out(L"UA's driver for the x8p is not installed, so the x8p has no driver after this until UA Connect is.");
            break;
        }
    }

    /* 3. Our packages. */
    if (ours.empty())
        Out(L"No Apollo driver package (Avium, apollo.inf) is installed.");
    for (auto &p : ours) {
        Out(L"Removing %s (%s)...", p.Published.c_str(), p.DriverVer.c_str());
        BOOL rb = FALSE;
        BOOL ok = DiUninstallDriverW(nullptr, p.PublishedPath.c_str(), 0, &rb);
        DWORD e = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok) {
            /* Some Windows builds want the driver-store copy of the INF instead of the published one. */
            wchar_t store[MAX_PATH * 2];
            DWORD need = 0;
            if (SetupGetInfDriverStoreLocationW(p.PublishedPath.c_str(), nullptr, nullptr, store, MAX_PATH * 2, &need)) {
                ok = DiUninstallDriverW(nullptr, store, 0, &rb);
                if (!ok)
                    e = GetLastError();
            }
        }
        if (ok) {
            Out(L"  removed; devices that used it were reinstalled with their next-best driver");
        } else {
            Out(L"  Windows' package uninstall said: %s; uninstalling its devices first", ErrText(e).c_str());
            if (!RemovePackageHard(p, reboot))
                continue;
            Out(L"  removed");
        }
        if (rb)
            reboot = true;
    }

    /* 4. Rescan, report, and the leftover service. */
    DEVINST root = 0;
    if (CM_Locate_DevNodeW(&root, nullptr, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) {
        Out(L"Scanning for hardware changes...");
        CM_Reenumerate_DevNode(root, CM_REENUMERATE_SYNCHRONOUS | CM_REENUMERATE_RETRY_INSTALLATION);
        Sleep(2000);
    }
    std::vector<Device> devs = FindUaDevices(false);
    if (!devs.empty())
        Out(L"Universal Audio devices now:");
    for (auto &d : devs) {
        PrintDevice(d);
        if (d.HasProblem && ProblemNeedsRestart(d.Problem))
            reboot = true;
    }
    RemoveOrphanService(reboot);

    std::wstring names = UnitNames(wasOnApollo);
    if (reboot) {
        Out(L"Restart Windows to finish (something still had a unit open).%s%s%s", names.empty() ? L"" : L" Then power-cycle ",
            names.c_str(), names.empty() ? L"" : L".");
        return EXIT_REBOOT;
    }
    if (!names.empty())
        Out(L"Power-cycle %s so UA's software starts each fresh.", names.c_str());
    return EXIT_OK;
}

/*
 * One unit back to UA's driver (revert-unit.ps1): only that unit's exact hardware ID, our package
 * stays installed (another unit may still use it; the uninstaller removes it), every other unit
 * is checked unchanged. The caller makes sure nothing uses the unit (idle-wait for the x8p).
 */
static int UnbindOne(const std::wstring &hwid)
{
    UnitKind kind = ClassifyHwid(hwid);
    std::vector<Device> present = FindUaDevices(false);
    std::vector<size_t> same = UnitsWithHwid(present, hwid);
    if (same.empty()) {
        Out(L"No %s (%s) is connected: is it switched on? Nothing changed.", UnitKindName(kind), hwid.c_str());
        return EXIT_NO_DEVICE;
    }
    if (same.size() > 1) {
        Err(L"%zu connected devices carry %s; Windows would move all of them. Nothing changed:", same.size(), hwid.c_str());
        for (size_t i : same)
            Err(L"  %s", present[i].InstanceId.c_str());
        return EXIT_BLOCKED;
    }
    const Device d = present[same[0]];
    std::vector<Device> others = OtherUnits(present, d.InstanceId);
    PrintDevice(d);
    PrintOthers(others);
    if (!d.OnApollo()) {
        Out(L"The %s is not on the Apollo driver: nothing to do.", UnitKindName(kind));
        return EXIT_OK;
    }
    Package ua;
    if (!FindUaPackageFor(FindUaPackages(), hwid, ua)) {
        Err(L"UA's driver for the %s (%s from Universal Audio, signed by Microsoft, listing %s) is not installed.",
            UnitKindName(kind), UA_INF_NAME, hwid.c_str());
        Err(L"Install UA Connect, then run this again. Nothing changed.");
        return EXIT_BLOCKED;
    }
    bool reboot = false;
    if (!MoveToUa(d, ua, reboot))
        return EXIT_ERROR;
    Sleep(3000);
    std::vector<Device> after = FindUaDevices(false);
    bool failed = false;
    const Device *p = FindByInstance(after, d.InstanceId);
    if (p == nullptr) {
        Out(L"The %s is not visible right now (restarting, or switched off). Power-cycle it, then run 'apollo-helper check'.",
            UnitKindName(kind));
    } else {
        PrintDevice(*p);
        if (p->HasProblem && ProblemNeedsRestart(p->Problem))
            reboot = true;
        if (p->OnApollo() && !reboot) {
            failed = true;
            Err(L"The %s is STILL on the Apollo driver: power-cycle it (or restart Windows) and run this again.",
                UnitKindName(kind));
        }
    }
    if (!CheckOthersUnchanged(others, after))
        failed = true;
    Out(L"The Apollo driver package stays installed (the uninstaller removes it).");
    if (kind == UNIT_X8P)
        Out(L"Power-cycle the x8p (off 10 s, on) so UA's software starts it fresh. A Twin X behind it drops too and comes "
            L"back on its own driver.");
    else
        Out(L"Power-cycle the %s (off 10 s, on) so UA's software starts it fresh. The x8p stays up.", UnitKindName(kind));
    if (failed)
        return EXIT_ERROR;
    return reboot ? EXIT_REBOOT : EXIT_OK;
}

int CmdUnbind(const std::wstring &unit)
{
    if (!RequireElevated(L"unbind"))
        return EXIT_NOT_ELEVATED;
    if (unit.empty())
        return UnbindAll();
    if (EqualsI(unit, L"x8p"))
        return UnbindOne(X8P_HWID);
    if (EqualsI(unit, L"twinx"))
        return UnbindOne(TWINX_HWID);
    Err(L"--unit takes x8p or twinx.");
    return EXIT_ERROR;
}

/* ------------------------------------------------------------------ */
/* quit-ua                                                             */
/* ------------------------------------------------------------------ */

struct CloseCtx { DWORD Pid; int Posted; };

static BOOL CALLBACK PostCloseToWindow(HWND hwnd, LPARAM lp)
{
    CloseCtx *c = (CloseCtx *)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == c->Pid && GetWindow(hwnd, GW_OWNER) == nullptr && IsWindowVisible(hwnd)) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        c->Posted++;
    }
    return TRUE;
}

static std::wstring ProcessPath(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr)
        return std::wstring();
    wchar_t buf[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    std::wstring r;
    if (QueryFullProcessImageNameW(h, 0, buf, &n))
        r.assign(buf, n);
    CloseHandle(h);
    return r;
}

/* install.ps1: UA's apps talk to every unit on UA's driver. They are closed only when a unit being
   switched is leaving UA's driver and no connected unit that stays is still on it (units.cpp
   DecideQuitUa). */
int CmdQuitUa(const std::vector<UnitKind> &units)
{
    std::vector<Device> present = FindUaDevices(false);
    size_t other = 0;
    switch (DecideQuitUa(present, units, other)) {
    case QUIT_NO_UNIT:
        Out(L"No unit to switch is connected: UA's apps can stay open.");
        return EXIT_OK;
    case QUIT_NOTHING_TO_RELEASE:
        Out(L"No unit to switch is on UA's driver: UA's apps can stay open.");
        return EXIT_OK;
    case QUIT_DRIVER_UNKNOWN:
        Out(L"Could not read which driver a unit is on: UA's apps are left running.");
        return EXIT_OK;
    case QUIT_OTHER_ON_UA:
        /* One line: Setup copies "Note:" lines into its Results page. */
        Out(L"Note: UA's apps were left running because the %s stays on UA's driver and uses them. If Windows needs a "
            L"restart to switch a unit, power-cycle that unit instead.", UnitKindName(present[other].Kind));
        return EXIT_OK;
    case QUIT_CLOSE_APPS:
        break;
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        Err(L"Cannot list processes: %s", ErrText(GetLastError()).c_str());
        return EXIT_ERROR;
    }
    struct Target { DWORD Pid; std::wstring Name; std::wstring Path; };
    std::vector<Target> targets;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == GetCurrentProcessId() || pe.th32ProcessID == 0)
            continue;
        std::wstring path = ProcessPath(pe.th32ProcessID);
        if (path.empty() || !(ContainsI(path, L"\\Universal Audio\\") || ContainsI(path, L"\\UA Connect\\")))
            continue;
        DWORD session = 0;
        if (ProcessIdToSessionId(pe.th32ProcessID, &session) && session == 0) {
            Out(L"Leaving UA's service %s (PID %lu) running.", pe.szExeFile, pe.th32ProcessID);
            continue;
        }
        targets.push_back({ pe.th32ProcessID, pe.szExeFile, path });
    }
    CloseHandle(snap);
    if (targets.empty()) {
        Out(L"No UA apps are running.");
        return EXIT_OK;
    }
    Out(L"The units being switched are on UA's driver and no unit that stays needs UA's apps: closing them.");

    std::vector<HANDLE> handles;
    for (auto &t : targets) {
        Out(L"Closing %s (PID %lu, %s)", t.Name.c_str(), t.Pid, t.Path.c_str());
        CloseCtx c = { t.Pid, 0 };
        EnumWindows(PostCloseToWindow, (LPARAM)&c);
        handles.push_back(OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, t.Pid));
    }
    ULONGLONG deadline = GetTickCount64() + 5000;
    int rc = EXIT_OK;
    for (size_t i = 0; i < targets.size(); i++) {
        HANDLE h = handles[i];
        if (h == nullptr) {
            Err(L"  could not open %s to close it: %s", targets[i].Name.c_str(), ErrText(GetLastError()).c_str());
            rc = EXIT_ERROR;
            continue;
        }
        ULONGLONG now = GetTickCount64();
        DWORD wait = now < deadline ? (DWORD)(deadline - now) : 0;
        if (WaitForSingleObject(h, wait) != WAIT_OBJECT_0) {
            if (TerminateProcess(h, 1) && WaitForSingleObject(h, 3000) == WAIT_OBJECT_0)
                Out(L"  %s did not close on request: ended it", targets[i].Name.c_str());
            else {
                Err(L"  could not end %s: %s", targets[i].Name.c_str(), ErrText(GetLastError()).c_str());
                rc = EXIT_ERROR;
            }
        } else {
            Out(L"  %s closed", targets[i].Name.c_str());
        }
        CloseHandle(h);
    }
    return rc;
}
