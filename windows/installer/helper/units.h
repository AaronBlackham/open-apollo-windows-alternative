/*
 * Apollo units: which Universal Audio PCI device is which, which driver it is on, and the
 * decisions the helper makes when more than one unit is connected. Pure logic (strings in,
 * answers out), so test_units.cpp checks it without hardware; pnp.cpp fills the Device records.
 *
 * Units are told apart by their exact four-part hardware ID (docs/spec-twinx.md 0.1), never by
 * DEV_0002 alone. apollo.inf lists exactly these two, and the installer can switch either:
 *   PCI\VEN_1A00&DEV_0002&SUBSYS_00141A00   Apollo x8p (Gen 1)
 *   PCI\VEN_1A00&DEV_0002&SUBSYS_00191A00   Apollo Twin X Gen 1 (Mode 2 from Parameters\Models\99)
 * Every other Universal Audio device (Gen 2 units, other models) is "other": never touched.
 * The rules mirror tools\phase1\install.ps1, step-ep.ps1, revert.ps1 and revert-unit.ps1.
 */
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>

#define X8P_HWID            L"PCI\\VEN_1A00&DEV_0002&SUBSYS_00141A00"
#define TWINX_HWID          L"PCI\\VEN_1A00&DEV_0002&SUBSYS_00191A00"
#define UA_PCI_PREFIX       L"PCI\\VEN_1A00&DEV_0002"
#define APOLLO_SERVICE      L"apollo"
#define UA_SERVICE          L"UAD2Pcie"
#define UA_PROVIDER         L"Universal Audio"

enum UnitKind { UNIT_OTHER = 0, UNIT_X8P, UNIT_TWINX };

/* Which driver a unit is on: ours (service apollo), UA's (UAD2Pcie / provider Universal Audio),
   another one, or none. */
enum DriverOwner { OWNER_NONE = 0, OWNER_APOLLO, OWNER_UA, OWNER_OTHER };

struct Device {
    std::wstring InstanceId;
    std::wstring Hwid;              /* exact four-part ID, upper case; "" if it could not be read */
    UnitKind     Kind = UNIT_OTHER;
    std::wstring Description;
    std::wstring Service;
    std::wstring Provider;
    std::wstring InfPath;           /* published name, e.g. oem12.inf */
    std::wstring DriverVersion;
    bool         Present = false;
    ULONG        Problem = 0;
    bool         HasProblem = false;
    bool IsX8p() const { return Kind == UNIT_X8P; }
    bool OnApollo() const;
    DriverOwner Owner() const;
};

/* ASCII case-insensitive equality (hardware IDs, services, INF and registry names are ASCII). */
bool UnitIEq(const std::wstring &a, const std::wstring &b);

/* UTF-16 <-> UTF-8, here so the offline test links them too (defaults_plan.cpp uses them). */
std::string ToUtf8(const std::wstring &s);
std::wstring FromUtf8(const std::string &s);

/* PCI\VEN_1A00&DEV_0002&SUBSYS_xxxxxxxx exactly (8 hex digits, nothing after). */
bool IsFourPartUaHwid(const std::wstring &s);

/* The exact four-part ID from a device's hardware ID list, else from its instance ID
   (PCI\VEN_1A00&DEV_0002&SUBSYS_xxxxxxxx&REV_xx\...), upper case; "" if neither has one. */
std::wstring FourPartHwid(const std::vector<std::wstring> &hardwareIds, const std::wstring &instanceId);

UnitKind ClassifyHwid(const std::wstring &fourPart);
const wchar_t *UnitKindKey(UnitKind k);     /* "x8p", "twinx", "other" (JSON, --units) */
const wchar_t *UnitKindName(UnitKind k);    /* "Apollo x8p", "Apollo Twin X", "Universal Audio device" */

/* The units this installer can switch (x8p, Twin X Gen 1) and their exact hardware IDs. */
bool IsSupportedKind(UnitKind k);
const wchar_t *UnitHwid(UnitKind k);        /* "" for UNIT_OTHER */

/* --units: "x8p", "twinx", "x8p,twinx" (any order, no repeats), "all" (both), "none" (empty list).
   Case-insensitive, spaces around commas ignored. Out: in bind order (x8p first, it is upstream
   on the Thunderbolt chain, as step-ep.ps1 does it). False on anything else. */
bool ParseUnitList(const std::wstring &s, std::vector<UnitKind> &out);
std::wstring UnitListText(const std::vector<UnitKind> &kinds);     /* "x8p,twinx" */
bool KindIn(const std::vector<UnitKind> &kinds, UnitKind k);

DriverOwner OwnerOf(const std::wstring &service, const std::wstring &provider);
const wchar_t *OwnerKey(DriverOwner o);     /* "apollo", "ua", "other", "none" */

/* Dotted numeric versions ("18.59.27.698"): <0, 0, >0. Missing or non-numeric parts count as 0. */
int CompareDriverVersions(const std::wstring &a, const std::wstring &b);

/* An INF's text lists this exact hardware ID (case-insensitive, not as part of a longer ID;
   "...SUBSYS_00141A00&REV_00" counts). The check revert-unit.ps1 makes with Select-String. */
bool TextListsHwid(const std::wstring &text, const std::wstring &hwid);

/* Indices of the devices carrying this exact hardware ID. */
std::vector<size_t> UnitsWithHwid(const std::vector<Device> &devs, const std::wstring &hwid);

/* A connected supported unit that "connect" / Setup should switch: not already running on our
   driver (on it with a problem code counts as not running). */
bool NeedsSwitch(const Device &d);

/*
 * quit-ua (install.ps1): UA's apps talk to every unit on UA's driver. Close them only when a unit
 * that is about to be switched (a target) is leaving UA's driver AND no connected unit that stays
 * (any other Universal Audio device: an unselected unit, a Gen 2, another model) is still on UA's
 * driver; otherwise leave them running. present: connected devices only. other: the unit that
 * still needs UA's apps (QUIT_OTHER_ON_UA).
 */
enum QuitUaDecision {
    QUIT_NO_UNIT,               /* no target unit connected */
    QUIT_NOTHING_TO_RELEASE,    /* no target is on UA's driver (already ours, or none/another) */
    QUIT_DRIVER_UNKNOWN,        /* a target's driver service could not be read: leave them running */
    QUIT_OTHER_ON_UA,           /* a unit that stays still uses UA's driver: leave them running */
    QUIT_CLOSE_APPS             /* close them */
};
QuitUaDecision DecideQuitUa(const std::vector<Device> &present, const std::vector<UnitKind> &targets, size_t &other);

/*
 * unbind (every unit): removing our package moves every unit on it. Each unit other than the
 * x8p that is on our driver, connected or only remembered, is handed back to UA's driver first
 * (revert.ps1 -AllUnits, revert-unit.ps1); the x8p goes back with the package (its next-best
 * driver). all: connected and remembered devices.
 */
std::vector<size_t> UnitsToHandBack(const std::vector<Device> &all);

/*
 * bind (install.ps1): one service, one loaded apollo.sys image. While a unit that is NOT being
 * switched keeps the driver loaded, a unit bound now joins that image whatever package it was
 * installed from, so every such unit on our driver must already run this package's version.
 * Returns the connected units (not among targetInstances) on our driver with another version.
 */
std::vector<size_t> UnitsOnOtherImage(const std::vector<Device> &present, const std::vector<std::wstring> &targetInstances,
                                      const std::wstring &packageVersion);

/* Some device (connected or remembered) is installed from this published INF (oemNN.inf). */
bool PackageInUse(const std::vector<Device> &all, const std::wstring &published);

/*
 * The loaded apollo.sys versus a package: with DestinationDirs 13 the driver runs from its
 * driver-store folder, "...\FileRepository\apollo.inf_amd64_<hash>\apollo.sys". The folder name
 * (case-insensitive) after FileRepository, or "" if the path has none. Works on the kernel's
 * "\SystemRoot\System32\DriverStore\..." form and on "C:\Windows\System32\DriverStore\...".
 */
std::wstring DriverStoreFolder(const std::wstring &path);
/* 1 = the loaded image is from that package's folder, 0 = from another one, -1 = cannot tell. */
int LoadedImageMatches(const std::wstring &loadedImagePath, const std::wstring &packageStorePath);
