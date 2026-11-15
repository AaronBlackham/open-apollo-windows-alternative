/*
 * Apollo units: which Universal Audio PCI device is which, which driver it is on, and the
 * decisions the helper makes when more than one unit is connected. Pure logic (strings in,
 * answers out), so test_units.cpp checks it without hardware; pnp.cpp fills the Device records.
 *
 * Units are told apart by their exact four-part hardware ID (docs/spec-twinx.md 0.1), never by
 * DEV_0002 alone:
 *   PCI\VEN_1A00&DEV_0002&SUBSYS_00141A00   Apollo x8p      the only unit this installer binds
 *   PCI\VEN_1A00&DEV_0002&SUBSYS_00191A00   Apollo Twin X   apollo.inf lists it too (test build,
 *                                                           Mode 0 = observe by default); the
 *                                                           installer never binds it, but has to
 *                                                           cope with it on either driver
 * The rules mirror tools\phase1\install.ps1, revert.ps1 and revert-unit.ps1.
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
    ULONG        Status = 0;
    ULONG        Problem = 0;
    bool         HasProblem = false;
    bool IsX8p() const { return Kind == UNIT_X8P; }
    bool OnApollo() const;
    DriverOwner Owner() const;
};

bool UnitIEq(const std::wstring &a, const std::wstring &b);

/* PCI\VEN_1A00&DEV_0002&SUBSYS_xxxxxxxx exactly (8 hex digits, nothing after). */
bool IsFourPartUaHwid(const std::wstring &s);

/* The exact four-part ID from a device's hardware ID list, else from its instance ID
   (PCI\VEN_1A00&DEV_0002&SUBSYS_xxxxxxxx&REV_xx\...), upper case; "" if neither has one. */
std::wstring FourPartHwid(const std::vector<std::wstring> &hardwareIds, const std::wstring &instanceId);

UnitKind ClassifyHwid(const std::wstring &fourPart);
const wchar_t *UnitKindKey(UnitKind k);     /* "x8p", "twinx", "other" (JSON) */
const wchar_t *UnitKindName(UnitKind k);    /* "Apollo x8p", "Apollo Twin X", "Universal Audio device" */

DriverOwner OwnerOf(const std::wstring &service, const std::wstring &provider);
const wchar_t *OwnerKey(DriverOwner o);     /* "apollo", "ua", "other", "none" */

/* Dotted numeric versions ("18.59.27.698"): <0, 0, >0. Missing or non-numeric parts count as 0. */
int CompareDriverVersions(const std::wstring &a, const std::wstring &b);

/* An INF's text lists this exact hardware ID (case-insensitive, not as part of a longer ID;
   "...SUBSYS_00141A00&REV_00" counts). The check revert-unit.ps1 makes with Select-String. */
bool TextListsHwid(const std::wstring &text, const std::wstring &hwid);

/* Indices of the devices carrying this exact hardware ID. */
std::vector<size_t> UnitsWithHwid(const std::vector<Device> &devs, const std::wstring &hwid);

/*
 * quit-ua (install.ps1): UA's apps talk to every unit on UA's driver. Close them only when the
 * x8p is about to leave UA's driver AND no other connected unit is still on UA's driver;
 * otherwise leave them running. present: connected devices only. other: the unit that still
 * needs UA's apps (QUIT_OTHER_ON_UA).
 */
enum QuitUaDecision {
    QUIT_NO_X8P,                /* no x8p connected */
    QUIT_X8P_ON_APOLLO,         /* already on our driver: nothing to release */
    QUIT_X8P_DRIVER_UNKNOWN,    /* its driver service could not be read: leave them running */
    QUIT_OTHER_ON_UA,           /* another unit still uses UA's driver: leave them running */
    QUIT_CLOSE_APPS             /* close them */
};
QuitUaDecision DecideQuitUa(const std::vector<Device> &present, size_t &other);

/*
 * unbind (every unit): removing our package moves every unit on it. Each unit other than the
 * x8p that is on our driver, connected or only remembered, is handed back to UA's driver first
 * (revert.ps1 -AllUnits, revert-unit.ps1). all: connected and remembered devices.
 */
std::vector<size_t> UnitsToHandBack(const std::vector<Device> &all);

/*
 * bind (install.ps1): one service, one loaded apollo.sys image. While another unit keeps the
 * driver loaded, a unit bound now joins that image whatever package it was installed from, so
 * every other connected unit on our driver must already run this package's version.
 * Returns the connected units (other than target) on our driver with another version.
 */
std::vector<size_t> UnitsOnOtherImage(const std::vector<Device> &present, const std::wstring &targetInstance,
                                      const std::wstring &packageVersion);

/* Some device (connected or remembered) is installed from this published INF (oemNN.inf). */
bool PackageInUse(const std::vector<Device> &all, const std::wstring &published);
