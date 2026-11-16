/*
 * apollo-helper: the installer's own tool (installer\README.md). Runs from
 * %ProgramFiles%\Avium\Apollo, which only administrators can write.
 *
 * Exit codes (the installer script reads them):
 *   0     done
 *   1     error (the text says what)
 *   2     bind: none of the named units is connected; the package is staged for later
 *         unbind --unit: that unit is not connected
 *   3     idle-wait: something still uses one of the units after the wait
 *   4     needs an elevated (Run as administrator) prompt
 *   5     stopped before changing anything (the text says why): another Apollo unit would be
 *         left without a driver, or would end up on a different driver image
 *   3010  done, Windows needs a restart to finish
 */
#pragma once

#include "units.h"
#include <string>
#include <vector>

#define HELPER_VERSION      L"2.0.0"

#define EXIT_OK             0
#define EXIT_ERROR          1
#define EXIT_NO_DEVICE      2
#define EXIT_BUSY           3
#define EXIT_NOT_ELEVATED   4
#define EXIT_BLOCKED        5
#define EXIT_REBOOT         3010

/* apollo.inf lists two exact hardware IDs (units.h): the x8p (SUBSYS 0x0014) and the Twin X Gen 1
   (SUBSYS 0x0019). The helper switches either or both (--units); nothing else. */
#define APOLLO_PROVIDER     L"Avium"
#define APOLLO_INF_NAME     L"apollo.inf"
#define UA_INF_NAME         L"uad2pcie.inf"
#define APOLLO_PARAMS_KEY   L"SYSTEM\\CurrentControlSet\\Services\\apollo\\Parameters"
#define APOLLO_SERVICE_KEY  L"SYSTEM\\CurrentControlSet\\Services\\apollo"
/* Windows audio endpoint names (driver\apollo.inf [Strings]): "LINE 1-2 (Apollo x8p)",
   "MON L/R (Apollo Twin X)", the combined set's "Monitor (Apollo)" (the x8p hosts it, and both
   units stream in it). Older x8p builds: "... (Apollo x8p (open driver, test build))", so no
   closing parenthesis on the x8p's tag. On UA's driver a unit's endpoints are UA's
   "(Universal Audio Thunderbolt WDM)" ones, which do not say which unit they belong to. */
#define ENDPOINT_TAG_X8P    L"(Apollo x8p"
#define ENDPOINT_TAG_TWINX  L"(Apollo Twin X"
#define ENDPOINT_TAG_AGG    L"(Apollo)"
#define ENDPOINT_TAG_UA     L"(Universal Audio Thunderbolt"

/* ---- output and small shared helpers (main.cpp) ---- */
void Out(const wchar_t *fmt, ...);          /* stdout, UTF-8 when redirected */
void Err(const wchar_t *fmt, ...);          /* stderr */
std::wstring Fmt(const wchar_t *fmt, ...);
std::wstring ErrText(DWORD err);            /* "message (0x....)" */
bool IsElevated();
bool RequireElevated(const wchar_t *what);  /* prints and returns false when not elevated */
std::wstring Lower(std::wstring s);
bool ContainsI(const std::wstring &hay, const std::wstring &needle);
bool EqualsI(const std::wstring &a, const std::wstring &b);
bool InListI(const std::vector<std::wstring> &list, const std::wstring &s);
bool ReadFileBytes(const std::wstring &path, std::vector<BYTE> &data);
std::vector<std::wstring> TextLines(const std::vector<BYTE> &utf8);    /* split at \n, a trailing \r dropped */
bool WriteFileUtf8(const std::wstring &path, const std::wstring &text); /* false: GetLastError() says why */
std::wstring ProcessPath(DWORD pid);        /* full image path, "" if unreadable */
wchar_t AskKey();                           /* first non-blank key typed, lower case; '\n' = Enter, 'q' = no console */
void *NtdllProc(const char *name);          /* an ntdll export, nullptr if missing */
typedef LONG (WINAPI *NtQuerySystemInformationFn)(ULONG, PVOID, ULONG, PULONG);

/* COM: release and clear. */
template <class T> void Release(T *&p)
{
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}

/* ---- devices and driver packages (pnp.cpp) ---- */
struct Package {
    std::wstring Published;         /* oemNN.inf */
    std::wstring PublishedPath;     /* %windir%\INF\oemNN.inf */
    std::wstring DriverVer;         /* "date, version", for messages */
    std::wstring Version;           /* DriverVer's version part */
    std::wstring Provider;
    std::wstring Signer;            /* UA packages: the catalog's signer, "" if it did not verify */
};

struct ApolloInterface {
    std::wstring Path;              /* GUID_DEVINTERFACE_APOLLO interface */
    std::wstring InstanceId;        /* the device it belongs to, "" if unreadable */
};

std::vector<Device> FindUaDevices(bool includePhantoms);
/* One pass over the driver store: our apollo.inf packages and UA's uad2pcie.inf ones (any signer).
   Either list may be null. */
void ScanDriverStore(std::vector<Package> *apollo, std::vector<Package> *ua);
std::vector<Package> FindUaPackages();      /* ScanDriverStore's UA list alone */
/* UA's package for this exact hardware ID: signed by Microsoft, lists the ID; the newest. */
bool FindUaPackageFor(const std::vector<Package> &uaPackages, const std::wstring &hwid, Package &out);
std::vector<ApolloInterface> FindApolloInterfaces();
std::wstring LoadedApolloImage();           /* the loaded apollo.sys's path, "" if not loaded */
int CmdBind(const std::wstring &inf, const std::vector<UnitKind> &units, bool stageOnly);
int CmdUnbind(const std::wstring &unit);    /* "" = every unit (uninstall), else x8p or twinx */
int CmdQuitUa(const std::vector<UnitKind> &units);

/* ---- audio and driver queries (audio.cpp) ---- */
struct DriverUnitInfo {
    DWORD        Error = ERROR_NOT_FOUND;   /* 0 = GET_INFO answered */
    ULONG        Version = 0;               /* the driver's IOCTL version */
    ULONG        DeviceType = 0;            /* 0x22 x8p, 0x99 Twin X Gen 1 */
    bool         HasUnit = false;           /* version 6+: the fields below are valid */
    ULONG        Mode = 0xFFFFFFFFu;        /* in effect: 0 observe, 1 transport, 2 full; ~0 unknown */
    ULONG        UnitFlags = 0;
    std::wstring ModelName;
    std::wstring UnitKey;
};
DriverUnitInfo QueryDriverUnit(const std::wstring &instanceId);    /* read-only GET_INFO */
int CmdDefaultsSave(const std::wstring &file);
int CmdDefaultsRestore(const std::wstring &file, int passes);
/* Waits until nothing uses any of these units (see audio.cpp for what counts). */
int CmdIdleWait(int seconds, bool interactive, const std::vector<UnitKind> &units);

/* ---- system state, certificates, registry (sys.cpp) ---- */
std::wstring SecureBootState();             /* on, off, unsupported, unknown */
bool TestSigningActive(std::wstring &startOptions, int &ciTestSign);
int TestSigningNextBoot();                  /* 1, 0, -1 = could not read (needs elevation) */
int BitLockerProtection(std::wstring &drive, std::wstring &detail); /* 1 on, 0 off, -1 unknown */
int CmdCert(bool add, const std::wstring &cer, const std::wstring &expect, const std::wstring &record);
int CmdCertRemoveLocal(const std::wstring &file);   /* the per-PC certificates in local-cert.txt */
int CmdRegDefaults(const std::wstring &json);
