/*
 * Offline test of units.cpp (classification and the multi-unit decisions). No hardware, no
 * registry: helper\build.cmd builds and runs it, and a failure fails the build.
 * The device data below is what PnP reports for the x8p and the Twin X Gen 1 (Nov 14 2026).
 */
#include "units.h"
#include <stdio.h>

static int g_failed = 0, g_run = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        g_run++;                                                                    \
        if (!(cond)) {                                                              \
            g_failed++;                                                             \
            printf("FAILED line %d: %s\n", __LINE__, #cond);                        \
        }                                                                           \
    } while (0)

static const wchar_t *kX8pInstance = L"PCI\\VEN_1A00&DEV_0002&SUBSYS_00141A00&REV_00\\6&1A2B3C4D&3&00080038";
static const wchar_t *kTwinInstance = L"PCI\\VEN_1A00&DEV_0002&SUBSYS_00191A00&REV_00\\8&5E6F7A8&1&000800200038";

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

static void TestQuitUa()
{
    size_t other = 99;
    /* x8p and Twin X both on UA's driver: the Twin X still needs UA's apps. */
    std::vector<Device> v = { X8p(L"UAD2Pcie"), TwinX(L"UAD2Pcie") };
    CHECK(DecideQuitUa(v, other) == QUIT_OTHER_ON_UA);
    CHECK(other == 1);
    /* Twin X listed first: same answer, the right index. */
    v = { TwinX(L"UAD2Pcie"), X8p(L"UAD2Pcie") };
    other = 99;
    CHECK(DecideQuitUa(v, other) == QUIT_OTHER_ON_UA);
    CHECK(other == 0);
    /* x8p on UA's, the Twin X on ours: nothing else needs UA's apps. */
    v = { X8p(L"UAD2Pcie"), TwinX(L"apollo") };
    CHECK(DecideQuitUa(v, other) == QUIT_CLOSE_APPS);
    /* x8p alone on UA's. */
    v = { X8p(L"UAD2Pcie") };
    CHECK(DecideQuitUa(v, other) == QUIT_CLOSE_APPS);
    /* Twin X with no driver at all does not need UA's apps. */
    v = { X8p(L"UAD2Pcie"), TwinX(L"") };
    CHECK(DecideQuitUa(v, other) == QUIT_CLOSE_APPS);
    /* x8p already on ours, whatever the Twin X does. */
    v = { X8p(L"apollo"), TwinX(L"UAD2Pcie") };
    CHECK(DecideQuitUa(v, other) == QUIT_X8P_ON_APOLLO);
    /* x8p driver unreadable: leave them running. */
    v = { X8p(L""), TwinX(L"apollo") };
    CHECK(DecideQuitUa(v, other) == QUIT_X8P_DRIVER_UNKNOWN);
    /* No x8p: a Twin X alone never makes the helper close UA's apps. */
    v = { TwinX(L"UAD2Pcie") };
    CHECK(DecideQuitUa(v, other) == QUIT_NO_X8P);
    v.clear();
    CHECK(DecideQuitUa(v, other) == QUIT_NO_X8P);
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
    /* Twin X on ours with the package's own version: the x8p may join that image. */
    std::vector<Device> v = { X8p(L"UAD2Pcie", L"18.59.27.698", L"oem194.inf"), TwinX(L"apollo") };
    CHECK(UnitsOnOtherImage(v, kX8pInstance, L"18.7.57.724").empty());
    /* Twin X on an older build of ours: refuse. */
    std::vector<size_t> r = UnitsOnOtherImage(v, kX8pInstance, L"20.26.53.748");
    CHECK(r.size() == 1 && r[0] == 1);
    /* Package version unreadable: refuse while another unit is on ours. */
    r = UnitsOnOtherImage(v, kX8pInstance, L"");
    CHECK(r.size() == 1);
    /* Twin X on UA's driver, or none: no constraint. */
    v = { X8p(L"apollo", L"15.5.41.908"), TwinX(L"UAD2Pcie", L"18.59.27.698", L"oem194.inf") };
    CHECK(UnitsOnOtherImage(v, kX8pInstance, L"20.26.53.748").empty());
    /* The target's own version never matters. */
    v = { X8p(L"apollo", L"15.5.41.908") };
    CHECK(UnitsOnOtherImage(v, kX8pInstance, L"20.26.53.748").empty());
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

int main()
{
    TestClassify();
    TestOwner();
    TestVersionsAndInf();
    TestQuitUa();
    TestHandBack();
    TestBindImage();
    TestPackagesAndIds();
    printf("test_units: %d checks, %d failed\n", g_run, g_failed);
    return g_failed == 0 ? 0 : 1;
}
