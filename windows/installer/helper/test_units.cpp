/*
 * Offline test of units.cpp (classification and the multi-unit decisions) and defaults_plan.cpp
 * (defaults.json). No hardware, no registry: helper\build.cmd builds and runs it, and a failure
 * fails the build. The device data below has the shape PnP reports for the x8p and the Twin X
 * Gen 1 (made-up instance IDs). Optional argument: a defaults.json to check (the release's own).
 */
#include "units.h"
#include "defaults_plan.h"
#include <stdio.h>
#include <string.h>

static int g_failed = 0, g_run = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        g_run++;                                                                    \
        if (!(cond)) {                                                              \
            g_failed++;                                                             \
            printf("FAILED line %d: %s\n", __LINE__, #cond);                        \
        }                                                                           \
    } while (0)

static const wchar_t *kX8pInstance = L"PCI\\VEN_1A00&DEV_0002&SUBSYS_00141A00&REV_00\\6&1A2B3C4D&0&00000010";
static const wchar_t *kTwinInstance = L"PCI\\VEN_1A00&DEV_0002&SUBSYS_00191A00&REV_00\\8&5E6F7A8&0&000000100010";

static Device Unit(UnitKind kind, const wchar_t *instance, const wchar_t *service, const wchar_t *version,
                   const wchar_t *inf, bool present = true)
{
    Device d;
    d.InstanceId = instance;
    d.Hwid = FourPartHwid({}, instance);
    d.Kind = kind;
    d.Service = service;
    d.Provider = UnitIEq(service, L"apollo") ? L"Avium" : (UnitIEq(service, L"UAD2Pcie") ? L"Universal Audio" : L"");
    d.DriverVersion = version;
    d.InfPath = inf;
    d.Present = present;
    return d;
}

static Device X8p(const wchar_t *service, const wchar_t *version = L"18.7.57.724", const wchar_t *inf = L"oem141.inf")
{
    return Unit(UNIT_X8P, kX8pInstance, service, version, inf);
}

static Device TwinX(const wchar_t *service, const wchar_t *version = L"18.7.57.724", const wchar_t *inf = L"oem141.inf",
                    bool present = true)
{
    return Unit(UNIT_TWINX, kTwinInstance, service, version, inf, present);
}

static void TestClassify()
{
    std::vector<std::wstring> x8pIds = { L"PCI\\VEN_1A00&DEV_0002&SUBSYS_00141A00&REV_00",
                                         L"PCI\\VEN_1A00&DEV_0002&SUBSYS_00141A00",
                                         L"PCI\\VEN_1A00&DEV_0002&CC_048000", L"PCI\\VEN_1A00&DEV_0002&CC_0480" };
    std::vector<std::wstring> twinIds = { L"PCI\\VEN_1A00&DEV_0002&SUBSYS_00191A00&REV_00",
                                          L"PCI\\VEN_1A00&DEV_0002&SUBSYS_00191A00",
                                          L"PCI\\VEN_1A00&DEV_0002&CC_048000", L"PCI\\VEN_1A00&DEV_0002&CC_0480" };
    CHECK(FourPartHwid(x8pIds, kX8pInstance) == X8P_HWID);
    CHECK(ClassifyHwid(FourPartHwid(x8pIds, kX8pInstance)) == UNIT_X8P);
    CHECK(FourPartHwid(twinIds, kTwinInstance) == TWINX_HWID);
    CHECK(ClassifyHwid(FourPartHwid(twinIds, kTwinInstance)) == UNIT_TWINX);

    /* No hardware ID list (a remembered device whose properties are gone): from the instance ID. */
    CHECK(FourPartHwid({}, kTwinInstance) == TWINX_HWID);
    CHECK(FourPartHwid({}, kX8pInstance) == X8P_HWID);
    /* Lower case in, upper case out. */
    CHECK(FourPartHwid({ L"pci\\ven_1a00&dev_0002&subsys_00191a00" }, L"") == TWINX_HWID);

    /* Another UA model: its own exact ID, classified as other. */
    std::wstring other = FourPartHwid({ L"PCI\\VEN_1A00&DEV_0002&SUBSYS_000F1A00&REV_01",
                                        L"PCI\\VEN_1A00&DEV_0002&SUBSYS_000F1A00" }, L"");
    CHECK(other == L"PCI\\VEN_1A00&DEV_0002&SUBSYS_000F1A00");
    CHECK(ClassifyHwid(other) == UNIT_OTHER);

    /* Never DEV_0002 alone, never a REV or CC form, never another vendor or device. */
    CHECK(FourPartHwid({ L"PCI\\VEN_1A00&DEV_0002", L"PCI\\VEN_1A00&DEV_0002&CC_0480" }, L"") == L"");
    CHECK(!IsFourPartUaHwid(L"PCI\\VEN_1A00&DEV_0002"));
    CHECK(!IsFourPartUaHwid(L"PCI\\VEN_1A00&DEV_0002&SUBSYS_00141A00&REV_00"));
    CHECK(!IsFourPartUaHwid(L"PCI\\VEN_1A00&DEV_0003&SUBSYS_00141A00"));
    CHECK(!IsFourPartUaHwid(L"PCI\\VEN_1A00&DEV_0002&SUBSYS_00141A0G"));
    CHECK(!IsFourPartUaHwid(L"PCI\\VEN_1A00&DEV_0002&SUBSYS_00141A000"));
    CHECK(FourPartHwid({}, L"USB\\VID_1234&PID_0001\\5&1") == L"");
    CHECK(FourPartHwid({}, L"") == L"");
    CHECK(ClassifyHwid(L"") == UNIT_OTHER);
    CHECK(ClassifyHwid(L"PCI\\VEN_1A00&DEV_0002") == UNIT_OTHER);

    CHECK(UnitIEq(UnitKindKey(UNIT_X8P), L"x8p"));
    CHECK(UnitIEq(UnitKindKey(UNIT_TWINX), L"twinx"));
    CHECK(UnitIEq(UnitKindKey(UNIT_OTHER), L"other"));
}

static void TestOwner()
{
    CHECK(OwnerOf(L"apollo", L"Avium") == OWNER_APOLLO);
    CHECK(OwnerOf(L"Apollo", L"") == OWNER_APOLLO);
    CHECK(OwnerOf(L"UAD2Pcie", L"Universal Audio") == OWNER_UA);
    CHECK(OwnerOf(L"uad2pcie", L"") == OWNER_UA);
    CHECK(OwnerOf(L"UadNext", L"Universal Audio, Inc.") == OWNER_UA);
    CHECK(OwnerOf(L"", L"Universal Audio") == OWNER_NONE);
    CHECK(OwnerOf(L"something", L"Someone") == OWNER_OTHER);
    CHECK(X8p(L"apollo").OnApollo());
    CHECK(!X8p(L"UAD2Pcie").OnApollo());
}

static void TestVersionsAndInf()
{
    CHECK(CompareDriverVersions(L"18.7.57.724", L"18.7.57.724") == 0);
    CHECK(CompareDriverVersions(L"18.59.27.698", L"18.7.57.724") > 0);
    CHECK(CompareDriverVersions(L"15.5.41.908", L"18.7.57.724") < 0);
    CHECK(CompareDriverVersions(L"1.2", L"1.2.0.0") == 0);
    CHECK(CompareDriverVersions(L"", L"0.0") == 0);
    CHECK(CompareDriverVersions(L"10.0", L"9.99") > 0);

    /* UA's uad2pcie.inf as published on the development PC (oem194.inf), [Manufacturer] lines. */
    std::wstring ua = L"[Manufacturer]\r\n"
                      L"%UAD2ApolloThunderbolt.DeviceDesc%=UAD2Pcie_Inst,       PCI\\VEN_1A00&DEV_0002&SUBSYS_00141A00\r\n"
                      L"%UAD2ApolloThunderbolt.DeviceDesc%=UAD2Pcie_Inst,       PCI\\VEN_1A00&DEV_0002&SUBSYS_00191A00\r\n";
    CHECK(TextListsHwid(ua, X8P_HWID));
    CHECK(TextListsHwid(ua, TWINX_HWID));
    CHECK(!TextListsHwid(ua, L"PCI\\VEN_1A00&DEV_0002&SUBSYS_000F1A00"));
    CHECK(TextListsHwid(L"x = s, pci\\ven_1a00&dev_0002&subsys_00191a00&rev_00", TWINX_HWID));
    CHECK(!TextListsHwid(L"x = s, PCI\\VEN_1A00&DEV_0002&SUBSYS_00191A001", TWINX_HWID));
    CHECK(!TextListsHwid(L"x = s, XPCI\\VEN_1A00&DEV_0002&SUBSYS_00191A00", TWINX_HWID));
    CHECK(!TextListsHwid(L"x = s, PCI\\VEN_1A00&DEV_0002", TWINX_HWID));
    CHECK(!TextListsHwid(ua, L""));
}

static void TestUnitLists()
{
    std::vector<UnitKind> u;
    CHECK(ParseUnitList(L"all", u) && u.size() == 2 && u[0] == UNIT_X8P && u[1] == UNIT_TWINX);
    CHECK(ParseUnitList(L"x8p", u) && u.size() == 1 && u[0] == UNIT_X8P);
    CHECK(ParseUnitList(L"twinx", u) && u.size() == 1 && u[0] == UNIT_TWINX);
    /* Any order in, bind order out: the x8p first. */
    CHECK(ParseUnitList(L"twinx,x8p", u) && u.size() == 2 && u[0] == UNIT_X8P && u[1] == UNIT_TWINX);
    CHECK(ParseUnitList(L" X8P , TwinX ", u) && u.size() == 2);
    CHECK(ParseUnitList(L"none", u) && u.empty());
    CHECK(!ParseUnitList(L"", u));
    CHECK(!ParseUnitList(L"x8p,x8p", u));
    CHECK(!ParseUnitList(L"x8p,", u));
    CHECK(!ParseUnitList(L"x4", u));
    CHECK(!ParseUnitList(L"x8p twinx", u));
    CHECK(!ParseUnitList(L"other", u));
    CHECK(UnitListText({ UNIT_X8P, UNIT_TWINX }) == L"x8p,twinx");
    CHECK(UnitListText({}) == L"none");
    CHECK(IsSupportedKind(UNIT_X8P) && IsSupportedKind(UNIT_TWINX) && !IsSupportedKind(UNIT_OTHER));
    CHECK(UnitIEq(UnitHwid(UNIT_TWINX), TWINX_HWID) && UnitIEq(UnitHwid(UNIT_X8P), X8P_HWID));
    CHECK(UnitHwid(UNIT_OTHER)[0] == L'\0');

    /* Which connected units still need switching. */
    CHECK(NeedsSwitch(X8p(L"UAD2Pcie")));
    CHECK(NeedsSwitch(TwinX(L"")));
    CHECK(!NeedsSwitch(TwinX(L"apollo")));
    Device broken = X8p(L"apollo");
    broken.HasProblem = true;
    broken.Problem = 52;
    CHECK(NeedsSwitch(broken));
    CHECK(!NeedsSwitch(TwinX(L"UAD2Pcie", L"18.59.27.698", L"oem194.inf", false)));
    Device gen2 = Unit(UNIT_OTHER, L"PCI\\VEN_1A00&DEV_0002&SUBSYS_000F1A00&REV_01\\4&1", L"UAD2Pcie", L"1.0", L"oem1.inf");
    CHECK(!NeedsSwitch(gen2));
}

static void TestQuitUa()
{
    const std::vector<UnitKind> x8pOnly = { UNIT_X8P }, both = { UNIT_X8P, UNIT_TWINX }, twinOnly = { UNIT_TWINX };
    size_t other = 99;
    /* Only the x8p switches, the Twin X stays on UA's driver: it still needs UA's apps. */
    std::vector<Device> v = { X8p(L"UAD2Pcie"), TwinX(L"UAD2Pcie") };
    CHECK(DecideQuitUa(v, x8pOnly, other) == QUIT_OTHER_ON_UA);
    CHECK(other == 1);
    /* Twin X listed first: same answer, the right index. */
    v = { TwinX(L"UAD2Pcie"), X8p(L"UAD2Pcie") };
    other = 99;
    CHECK(DecideQuitUa(v, x8pOnly, other) == QUIT_OTHER_ON_UA);
    CHECK(other == 0);
    /* Both switch, both on UA's: nothing that stays needs UA's apps. */
    v = { X8p(L"UAD2Pcie"), TwinX(L"UAD2Pcie") };
    CHECK(DecideQuitUa(v, both, other) == QUIT_CLOSE_APPS);
    /* Only the Twin X switches: the x8p stays on UA's and keeps them. */
    other = 99;
    CHECK(DecideQuitUa(v, twinOnly, other) == QUIT_OTHER_ON_UA);
    CHECK(other == 0);
    /* Both switch, but a Gen 2 (or any other model) stays on UA's driver: leave them running. */
    Device gen2 = Unit(UNIT_OTHER, L"PCI\\VEN_1A00&DEV_0002&SUBSYS_000F1A00&REV_01\\4&1", L"UAD2Pcie", L"1.0", L"oem1.inf");
    v = { X8p(L"UAD2Pcie"), TwinX(L"UAD2Pcie"), gen2 };
    other = 99;
    CHECK(DecideQuitUa(v, both, other) == QUIT_OTHER_ON_UA);
    CHECK(other == 2);
    /* x8p on UA's, the Twin X on ours: nothing else needs UA's apps. */
    v = { X8p(L"UAD2Pcie"), TwinX(L"apollo") };
    CHECK(DecideQuitUa(v, x8pOnly, other) == QUIT_CLOSE_APPS);
    CHECK(DecideQuitUa(v, both, other) == QUIT_CLOSE_APPS);
    /* x8p alone on UA's. */
    v = { X8p(L"UAD2Pcie") };
    CHECK(DecideQuitUa(v, x8pOnly, other) == QUIT_CLOSE_APPS);
    CHECK(DecideQuitUa(v, both, other) == QUIT_CLOSE_APPS);
    /* A Twin X with no driver at all does not need UA's apps. */
    v = { X8p(L"UAD2Pcie"), TwinX(L"") };
    CHECK(DecideQuitUa(v, x8pOnly, other) == QUIT_CLOSE_APPS);
    /* ... and as a target next to one leaving UA's driver, it does not stop the close either. */
    CHECK(DecideQuitUa(v, both, other) == QUIT_CLOSE_APPS);
    /* Every target already on ours: nothing to release, whatever stays on UA's. */
    v = { X8p(L"apollo"), TwinX(L"UAD2Pcie") };
    CHECK(DecideQuitUa(v, x8pOnly, other) == QUIT_NOTHING_TO_RELEASE);
    v = { X8p(L"apollo"), TwinX(L"apollo") };
    CHECK(DecideQuitUa(v, both, other) == QUIT_NOTHING_TO_RELEASE);
    /* A target's driver unreadable and none leaving UA's: leave them running. */
    v = { X8p(L""), TwinX(L"apollo") };
    CHECK(DecideQuitUa(v, x8pOnly, other) == QUIT_DRIVER_UNKNOWN);
    CHECK(DecideQuitUa(v, both, other) == QUIT_DRIVER_UNKNOWN);
    /* No target connected: never close UA's apps. */
    v = { TwinX(L"UAD2Pcie") };
    CHECK(DecideQuitUa(v, x8pOnly, other) == QUIT_NO_UNIT);
    CHECK(DecideQuitUa(v, {}, other) == QUIT_NO_UNIT);
    v = { gen2 };
    CHECK(DecideQuitUa(v, both, other) == QUIT_NO_UNIT);
    v.clear();
    CHECK(DecideQuitUa(v, both, other) == QUIT_NO_UNIT);
}

static void TestHandBack()
{
    /* Both on ours: only the Twin X is handed back before the package goes. */
    std::vector<Device> v = { X8p(L"apollo"), TwinX(L"apollo") };
    std::vector<size_t> r = UnitsToHandBack(v);
    CHECK(r.size() == 1 && r[0] == 1);
    /* A remembered (not connected) Twin X on ours counts too. */
    v = { X8p(L"apollo"), TwinX(L"apollo", L"18.7.57.724", L"oem141.inf", false) };
    r = UnitsToHandBack(v);
    CHECK(r.size() == 1 && r[0] == 1);
    /* Twin X on UA's driver: nothing to hand back. */
    v = { X8p(L"apollo"), TwinX(L"UAD2Pcie", L"18.59.27.698", L"oem194.inf") };
    CHECK(UnitsToHandBack(v).empty());
    /* x8p alone. */
    v = { X8p(L"apollo") };
    CHECK(UnitsToHandBack(v).empty());
    /* An unknown UA model on ours is handed back like the Twin X. */
    Device o = Unit(UNIT_OTHER, L"PCI\\VEN_1A00&DEV_0002&SUBSYS_000F1A00&REV_01\\4&1", L"apollo", L"1.0", L"oem141.inf");
    v = { X8p(L"UAD2Pcie"), o };
    r = UnitsToHandBack(v);
    CHECK(r.size() == 1 && r[0] == 1);
    CHECK(v[1].Hwid == L"PCI\\VEN_1A00&DEV_0002&SUBSYS_000F1A00");
}

static void TestBindImage()
{
    const std::vector<std::wstring> x8pOnly = { kX8pInstance }, both = { kX8pInstance, kTwinInstance };
    /* Twin X on ours with the package's own version: the x8p may join that image. */
    std::vector<Device> v = { X8p(L"UAD2Pcie", L"18.59.27.698", L"oem194.inf"), TwinX(L"apollo") };
    CHECK(UnitsOnOtherImage(v, x8pOnly, L"18.7.57.724").empty());
    /* Twin X on an older build of ours, not being switched: refuse. */
    std::vector<size_t> r = UnitsOnOtherImage(v, x8pOnly, L"20.26.53.748");
    CHECK(r.size() == 1 && r[0] == 1);
    /* ... but switched together with the x8p: no constraint (a restart may be needed, bind says so). */
    CHECK(UnitsOnOtherImage(v, both, L"20.26.53.748").empty());
    /* Package version unreadable: refuse while another unit is on ours. */
    r = UnitsOnOtherImage(v, x8pOnly, L"");
    CHECK(r.size() == 1);
    CHECK(UnitsOnOtherImage(v, both, L"").empty());
    /* Twin X on UA's driver, or none: no constraint. */
    v = { X8p(L"apollo", L"15.5.41.908"), TwinX(L"UAD2Pcie", L"18.59.27.698", L"oem194.inf") };
    CHECK(UnitsOnOtherImage(v, x8pOnly, L"20.26.53.748").empty());
    /* The targets' own versions never matter. */
    v = { X8p(L"apollo", L"15.5.41.908") };
    CHECK(UnitsOnOtherImage(v, x8pOnly, L"20.26.53.748").empty());
    v = { X8p(L"apollo", L"15.5.41.908"), TwinX(L"apollo", L"15.5.41.908") };
    CHECK(UnitsOnOtherImage(v, both, L"20.26.53.748").empty());
    /* Only the Twin X switched while the x8p runs an older build of ours: refuse. */
    r = UnitsOnOtherImage(v, { kTwinInstance }, L"20.26.53.748");
    CHECK(r.size() == 1 && r[0] == 0);
}

static void TestLoadedImage()
{
    const wchar_t *kernel = L"\\SystemRoot\\System32\\DriverStore\\FileRepository\\apollo.inf_amd64_0123abcd4567ef89\\apollo.sys";
    const wchar_t *same = L"C:\\Windows\\System32\\DriverStore\\FileRepository\\APOLLO.INF_AMD64_0123ABCD4567EF89\\apollo.inf";
    const wchar_t *other = L"C:\\Windows\\System32\\DriverStore\\FileRepository\\apollo.inf_amd64_fedcba9876543210\\apollo.inf";
    CHECK(DriverStoreFolder(kernel) == L"APOLLO.INF_AMD64_0123ABCD4567EF89");
    CHECK(LoadedImageMatches(kernel, same) == 1);
    CHECK(LoadedImageMatches(kernel, other) == 0);
    CHECK(LoadedImageMatches(L"", same) == -1);
    CHECK(LoadedImageMatches(L"\\SystemRoot\\System32\\drivers\\apollo.sys", same) == -1);
    CHECK(LoadedImageMatches(kernel, L"") == -1);
    CHECK(DriverStoreFolder(L"C:\\x\\FileRepository\\") == L"");
    CHECK(DriverStoreFolder(L"C:\\x\\FileRepository\\only") == L"");
}

static std::vector<BYTE> Bytes(const char *s)
{
    return std::vector<BYTE>((const BYTE *)s, (const BYTE *)s + strlen(s));
}

static const DefaultItem *Find(const std::vector<DefaultItem> &v, const wchar_t *sub, const wchar_t *name)
{
    for (auto &i : v)
        if (UnitIEq(i.SubKey, sub) && UnitIEq(i.Name, name))
            return &i;
    return nullptr;
}

static void TestDefaults()
{
    std::vector<DefaultItem> v;
    std::wstring why;
    /* The release's shape: legacy values for the x8p, Models\99 for the Twin X. */
    const char *good =
        "{ \"_comment\": [\"x\"], \"overwrite\": false,\n"
        "  \"values\": { \"StripsEnable\": { \"type\": \"dword\", \"data\": 1, \"overwrite\": true },\n"
        "              \"StripMask\": { \"type\": \"dword\", \"data\": \"0xFF\" }, \"_note\": 5, \"Plain\": 7,\n"
        "              \"Text\": \"abc\" },\n"
        "  \"subkeys\": { \"Models\\\\99\": { \"overwrite\": true, \"_c\": 1,\n"
        "                 \"values\": { \"Mode\": 2, \"StripsEnable\": { \"data\": 1, \"overwrite\": false } } } } }";
    CHECK(ParseDefaults(Bytes(good), v, why));
    CHECK(v.size() == 6);
    const DefaultItem *a = Find(v, L"", L"StripsEnable");
    CHECK(a && a->Type == REG_DWORD && a->Dw == 1 && a->Overwrite);
    a = Find(v, L"", L"StripMask");
    CHECK(a && a->Dw == 0xFF && !a->Overwrite);
    a = Find(v, L"", L"Plain");
    CHECK(a && a->Dw == 7 && !a->Overwrite);
    a = Find(v, L"", L"Text");
    CHECK(a && a->Type == REG_SZ && a->Sz == L"abc");
    a = Find(v, L"Models\\99", L"Mode");
    CHECK(a && a->Dw == 2 && a->Overwrite);             /* the subkey's overwrite */
    a = Find(v, L"Models\\99", L"StripsEnable");
    CHECK(a && a->Dw == 1 && !a->Overwrite);            /* the value's own wins */
    CHECK(Find(v, L"", L"_note") == nullptr);

    /* A UTF-8 BOM is fine; no subkeys is fine (the old format). */
    CHECK(ParseDefaults(Bytes("\xEF\xBB\xBF{ \"values\": { \"A\": 1 } }"), v, why) && v.size() == 1);

    /* Refused, with nothing returned. */
    const char *bad[] = {
        "",                                                     /* not JSON */
        "[1]",                                                  /* not an object */
        "{ \"value\": { \"A\": 1 } }",                          /* typo at the top */
        "{ \"values\": { \"A\": 1 }, \"subkey\": {} }",
        "{ \"values\": { \"A B\": 1 } }",                       /* bad value name */
        "{ \"values\": { \"A\": -1 } }",                        /* not a DWORD */
        "{ \"values\": { \"A\": 4294967296 } }",
        "{ \"values\": { \"A\": 1.5 } }",
        "{ \"values\": { \"A\": \"-1\" } }",                    /* (an sz, fine) ... */
        "{ \"values\": { \"A\": { \"type\": \"dword\", \"data\": \"-1\" } } }",
        "{ \"values\": { \"A\": { \"type\": \"qword\", \"data\": 1 } } }",
        "{ \"values\": { \"A\": { \"data\": 1, \"overwrit\": true } } }",
        "{ \"values\": { \"A\": { \"type\": \"dword\" } } }",
        "{ \"values\": { \"A\": 1, \"a\": 2 } }",               /* twice (case-insensitive) */
        "{ \"values\": {}, \"subkeys\": { \"Models\\\\\\\\99\": { \"values\": { \"Mode\": 2 } } } }",
        "{ \"values\": {}, \"subkeys\": { \"\\\\Models\": { \"values\": { \"Mode\": 2 } } } }",
        "{ \"values\": {}, \"subkeys\": { \"Models\\\\\": { \"values\": { \"Mode\": 2 } } } }",
        "{ \"values\": {}, \"subkeys\": { \"Models\\\\..\": { \"values\": { \"Mode\": 2 } } } }",
        "{ \"values\": {}, \"subkeys\": { \"A\\\\B\\\\C\\\\D\": { \"values\": { \"Mode\": 2 } } } }",
        "{ \"values\": {}, \"subkeys\": { \"Models\\\\99\": { \"Mode\": 2 } } }",          /* no values */
        "{ \"values\": {}, \"subkeys\": { \"Models\\\\99\": { \"values\": {} } } }",       /* empty */
        "{ \"values\": {}, \"subkeys\": { \"Models\\\\99\": { \"values\": { \"Mode\": 2 } },"
        " \"models\\\\99\": { \"values\": { \"Mode\": 1 } } } }",                           /* twice */
        "{ \"overwrite\": 1, \"values\": { \"A\": 1 } }",
    };
    int n = 0;
    for (const char *b : bad) {
        n++;
        bool ok = ParseDefaults(Bytes(b), v, why);
        if (n == 9) {
            CHECK(ok && v.size() == 1 && v[0].Type == REG_SZ);     /* a plain string is REG_SZ */
            continue;
        }
        if (ok)
            printf("FAILED: bad defaults case %d was accepted\n", n);
        CHECK(!ok && v.empty() && !why.empty());
    }
    CHECK(ValidSubKeyPath(L"Models\\99") && ValidSubKeyPath(L"Aggregate") && ValidSubKeyPath(L"A\\B\\C"));
    CHECK(!ValidSubKeyPath(L"") && !ValidSubKeyPath(L"A\\\\B") && !ValidSubKeyPath(L"A/B"));
}

/* The release's own defaults.json, when given: it parses, and says what the release wants. */
static int CheckDefaultsFile(const char *path)
{
    FILE *f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || f == nullptr) {
        printf("FAILED: cannot open %s\n", path);
        return 1;
    }
    std::vector<BYTE> data;
    BYTE buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
        data.insert(data.end(), buf, buf + got);
    fclose(f);
    std::vector<DefaultItem> v;
    std::wstring why;
    if (!ParseDefaults(data, v, why)) {
        printf("FAILED: %s: %ls\n", path, why.c_str());
        return 1;
    }
    printf("%s: %zu value(s)\n", path, v.size());
    for (auto &i : v) {
        if (i.Type == REG_DWORD)
            printf("  Parameters%ls%ls\\%ls = %lu (0x%lX)%s\n", i.SubKey.empty() ? L"" : L"\\", i.SubKey.c_str(),
                   i.Name.c_str(), i.Dw, i.Dw, i.Overwrite ? ", overwrite" : "");
        else
            printf("  Parameters%ls%ls\\%ls = \"%ls\"%s\n", i.SubKey.empty() ? L"" : L"\\", i.SubKey.c_str(),
                   i.Name.c_str(), i.Sz.c_str(), i.Overwrite ? ", overwrite" : "");
    }
    return 0;
}

static void TestPackagesAndIds()
{
    std::vector<Device> v = { X8p(L"apollo", L"20.26.53.748", L"oem200.inf"),
                              TwinX(L"apollo", L"18.7.57.724", L"oem141.inf", false) };
    CHECK(PackageInUse(v, L"oem200.inf"));
    CHECK(PackageInUse(v, L"OEM141.INF"));      /* a remembered unit keeps its package too */
    CHECK(!PackageInUse(v, L"oem150.inf"));
    CHECK(!PackageInUse(v, L""));

    v = { X8p(L"apollo"), TwinX(L"UAD2Pcie"), X8p(L"UAD2Pcie") };
    v[2].InstanceId = L"PCI\\VEN_1A00&DEV_0002&SUBSYS_00141A00&REV_00\\7&1";
    CHECK(UnitsWithHwid(v, X8P_HWID).size() == 2);
    CHECK(UnitsWithHwid(v, TWINX_HWID).size() == 1);
    CHECK(UnitsWithHwid(v, L"").empty());
}

int main(int argc, char **argv)
{
    TestClassify();
    TestOwner();
    TestVersionsAndInf();
    TestUnitLists();
    TestQuitUa();
    TestHandBack();
    TestBindImage();
    TestLoadedImage();
    TestPackagesAndIds();
    TestDefaults();
    printf("test_units: %d checks, %d failed\n", g_run, g_failed);
    int fileRc = argc > 1 ? CheckDefaultsFile(argv[1]) : 0;
    return (g_failed == 0 && fileRc == 0) ? 0 : 1;
}
