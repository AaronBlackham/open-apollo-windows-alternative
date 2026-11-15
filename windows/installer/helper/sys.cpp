/*
 * System state (Secure Boot, test signing, BitLocker), the test certificate, and the driver's
 * registry defaults from defaults.json.
 */
#include "common.h"
#include <wbemidl.h>
#include <wincrypt.h>
#include <oleauto.h>
#include <stdlib.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Secure Boot                                                         */
/* ------------------------------------------------------------------ */

std::wstring SecureBootState()
{
    FIRMWARE_TYPE ft = FirmwareTypeUnknown;
    if (GetFirmwareType(&ft) && ft == FirmwareTypeBios)
        return L"unsupported";
    DWORD v = 0, size = sizeof(v);
    LSTATUS st = RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State",
                              L"UEFISecureBootEnabled", RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY, nullptr, &v, &size);
    if (st == ERROR_SUCCESS)
        return v ? L"on" : L"off";
    if (st == ERROR_FILE_NOT_FOUND)
        return L"unsupported";
    return L"unknown";
}

/* ------------------------------------------------------------------ */
/* test signing                                                        */
/* ------------------------------------------------------------------ */

typedef LONG (WINAPI *NtQuerySystemInformationFn)(ULONG, PVOID, ULONG, PULONG);

struct CodeIntegrityInfo {
    ULONG Length;
    ULONG CodeIntegrityOptions;
};
#define SYSTEM_CODE_INTEGRITY_INFORMATION_CLASS 103
#define CI_OPTION_TESTSIGN                      0x02

/* Active for this boot: code integrity's own flag, else the loader's SystemStartOptions. */
bool TestSigningActive(std::wstring &startOptions, int &ciTestSign)
{
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    startOptions.clear();
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control", L"SystemStartOptions",
                     RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buf, &size) == ERROR_SUCCESS)
        startOptions = buf;
    bool opt = false;
    {
        std::wstring s = L" " + Lower(startOptions) + L" ";
        for (wchar_t &c : s)
            if (c == L'\t' || c == L'/')
                c = L' ';
        opt = s.find(L" testsigning ") != std::wstring::npos;
    }

    ciTestSign = -1;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    NtQuerySystemInformationFn q = ntdll ? (NtQuerySystemInformationFn)(void *)GetProcAddress(ntdll,
                                           "NtQuerySystemInformation") : nullptr;
    if (q != nullptr) {
        CodeIntegrityInfo ci = { sizeof(CodeIntegrityInfo), 0 };
        ULONG ret = 0;
        if (q(SYSTEM_CODE_INTEGRITY_INFORMATION_CLASS, &ci, sizeof(ci), &ret) >= 0)
            ciTestSign = (ci.CodeIntegrityOptions & CI_OPTION_TESTSIGN) ? 1 : 0;
    }
    return ciTestSign >= 0 ? ciTestSign == 1 : opt;
}

/* ------------------------------------------------------------------ */
/* WMI helpers                                                         */
/* ------------------------------------------------------------------ */

template <class T> static void Rel(T *&p)
{
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}

static void EnablePrivilege(const wchar_t *name)
{
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return;
    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid))
        AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    CloseHandle(tok);
}

static IWbemServices *WmiConnect(const wchar_t *ns, HRESULT &hr)
{
    IWbemLocator *loc = nullptr;
    IWbemServices *svc = nullptr;
    hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (void **)&loc);
    if (FAILED(hr))
        return nullptr;
    BSTR b = SysAllocString(ns);
    hr = loc->ConnectServer(b, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc);
    SysFreeString(b);
    loc->Release();
    if (FAILED(hr))
        return nullptr;
    hr = CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                           RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    if (FAILED(hr)) {
        svc->Release();
        return nullptr;
    }
    return svc;
}

/* Calls Class.Method on objectPath with one input parameter; returns the out-parameters object. */
static IWbemClassObject *WmiCall(IWbemServices *svc, const wchar_t *cls, const wchar_t *objectPath,
                                 const wchar_t *method, const wchar_t *param, VARIANT *value)
{
    IWbemClassObject *klass = nullptr, *inDef = nullptr, *in = nullptr, *out = nullptr;
    BSTR bc = SysAllocString(cls), bp = SysAllocString(objectPath), bm = SysAllocString(method);
    if (SUCCEEDED(svc->GetObject(bc, 0, nullptr, &klass, nullptr)) && klass != nullptr &&
        SUCCEEDED(klass->GetMethod(method, 0, &inDef, nullptr)) && inDef != nullptr &&
        SUCCEEDED(inDef->SpawnInstance(0, &in)) && in != nullptr && SUCCEEDED(in->Put(param, 0, value, 0))) {
        if (FAILED(svc->ExecMethod(bp, bm, 0, nullptr, in, &out, nullptr)))
            out = nullptr;
    }
    SysFreeString(bc);
    SysFreeString(bp);
    SysFreeString(bm);
    Rel(in);
    Rel(inDef);
    Rel(klass);
    return out;
}

static bool OutTrue(IWbemClassObject *out)
{
    VARIANT v;
    VariantInit(&v);
    bool r = out != nullptr && SUCCEEDED(out->Get(L"ReturnValue", 0, &v, nullptr, nullptr)) && v.vt == VT_BOOL &&
             v.boolVal != VARIANT_FALSE;
    VariantClear(&v);
    return r;
}

/* An embedded object out-parameter, as its relative path (for the next ExecMethod), or the object. */
static IWbemClassObject *OutObject(IWbemClassObject *out, const wchar_t *name)
{
    VARIANT v;
    VariantInit(&v);
    IWbemClassObject *o = nullptr;
    if (out != nullptr && SUCCEEDED(out->Get(name, 0, &v, nullptr, nullptr)) && v.vt == VT_UNKNOWN && v.punkVal)
        v.punkVal->QueryInterface(IID_IWbemClassObject, (void **)&o);
    VariantClear(&v);
    return o;
}

static std::wstring RelPath(IWbemClassObject *o)
{
    VARIANT v;
    VariantInit(&v);
    std::wstring s;
    if (o != nullptr && SUCCEEDED(o->Get(L"__RELPATH", 0, &v, nullptr, nullptr)) && v.vt == VT_BSTR && v.bstrVal)
        s = v.bstrVal;
    VariantClear(&v);
    return s;
}

/* BCD {current}, element BcdLibraryBoolean_AllowPrereleaseSignatures (testsigning), through the
   BCD WMI provider (needs an elevated token with SeBackup/SeRestore enabled). */
int TestSigningNextBoot()
{
    if (!IsElevated())
        return -1;
    EnablePrivilege(SE_BACKUP_NAME);
    EnablePrivilege(SE_RESTORE_NAME);
    HRESULT hr;
    IWbemServices *svc = WmiConnect(L"root\\WMI", hr);
    if (svc == nullptr)
        return -1;
    int result = -1;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(L"");
    IWbemClassObject *out = WmiCall(svc, L"BcdStore", L"BcdStore", L"OpenStore", L"File", &v);
    VariantClear(&v);
    IWbemClassObject *store = OutTrue(out) ? OutObject(out, L"Store") : nullptr;
    Rel(out);
    std::wstring storePath = RelPath(store);
    Rel(store);
    if (!storePath.empty()) {
        VariantInit(&v);
        v.vt = VT_BSTR;
        v.bstrVal = SysAllocString(L"{fa926493-6f1c-4193-a414-58f0b2456d1e}");  /* {current} */
        out = WmiCall(svc, L"BcdStore", storePath.c_str(), L"OpenObject", L"Id", &v);
        VariantClear(&v);
        IWbemClassObject *obj = OutTrue(out) ? OutObject(out, L"Object") : nullptr;
        Rel(out);
        std::wstring objPath = RelPath(obj);
        Rel(obj);
        if (!objPath.empty()) {
            VariantInit(&v);
            v.vt = VT_I4;
            v.lVal = 0x16000049;
            out = WmiCall(svc, L"BcdObject", objPath.c_str(), L"GetElement", L"Type", &v);
            if (out != nullptr) {
                if (!OutTrue(out)) {
                    result = 0;     /* element not set: off */
                } else {
                    IWbemClassObject *el = OutObject(out, L"Element");
                    VARIANT b;
                    VariantInit(&b);
                    if (el != nullptr && SUCCEEDED(el->Get(L"Boolean", 0, &b, nullptr, nullptr)) && b.vt == VT_BOOL)
                        result = b.boolVal != VARIANT_FALSE ? 1 : 0;
                    VariantClear(&b);
                    Rel(el);
                }
                Rel(out);
            }
        }
    }
    svc->Release();
    return result;
}

/* ------------------------------------------------------------------ */
/* BitLocker                                                           */
/* ------------------------------------------------------------------ */

int BitLockerProtection(std::wstring &drive, std::wstring &detail)
{
    wchar_t win[MAX_PATH];
    UINT n = GetSystemWindowsDirectoryW(win, MAX_PATH);
    drive = (n >= 2 && n < MAX_PATH) ? std::wstring(win, 2) : std::wstring(L"C:");
    detail.clear();
    HRESULT hr;
    IWbemServices *svc = WmiConnect(L"ROOT\\CIMV2\\Security\\MicrosoftVolumeEncryption", hr);
    if (svc == nullptr) {
        if (hr == (HRESULT)WBEM_E_INVALID_NAMESPACE) {
            detail = L"BitLocker is not available on this edition";
            return 0;
        }
        detail = IsElevated() ? Fmt(L"BitLocker WMI unavailable (0x%08lX)", (unsigned long)hr)
                              : std::wstring(L"needs an elevated prompt to read");
        return -1;
    }
    int result = 0;
    IEnumWbemClassObject *en = nullptr;
    BSTR lang = SysAllocString(L"WQL");
    BSTR query = SysAllocString(L"SELECT DriveLetter, ProtectionStatus FROM Win32_EncryptableVolume");
    hr = svc->ExecQuery(lang, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
    SysFreeString(lang);
    SysFreeString(query);
    if (FAILED(hr) || en == nullptr) {
        detail = Fmt(L"query failed (0x%08lX)", (unsigned long)hr);
        result = -1;
    } else {
        detail = L"system drive not encrypted";
        for (;;) {
            IWbemClassObject *o = nullptr;
            ULONG got = 0;
            if (en->Next(WBEM_INFINITE, 1, &o, &got) != WBEM_S_NO_ERROR || got == 0)
                break;
            VARIANT dl, ps;
            VariantInit(&dl);
            VariantInit(&ps);
            if (SUCCEEDED(o->Get(L"DriveLetter", 0, &dl, nullptr, nullptr)) && dl.vt == VT_BSTR && dl.bstrVal &&
                EqualsI(dl.bstrVal, drive) && SUCCEEDED(o->Get(L"ProtectionStatus", 0, &ps, nullptr, nullptr))) {
                LONG st = (ps.vt == VT_I4 || ps.vt == VT_UI4) ? ps.lVal : -1;
                if (st == 1) {
                    result = 1;
                    detail = L"protection on";
                } else if (st == 0) {
                    result = 0;
                    detail = L"protection off (not encrypted, or suspended)";
                } else {
                    result = -1;
                    detail = L"protection status unknown (volume locked?)";
                }
            }
            VariantClear(&dl);
            VariantClear(&ps);
            o->Release();
        }
        en->Release();
    }
    svc->Release();
    return result;
}

/* ------------------------------------------------------------------ */
/* certificate                                                         */
/* ------------------------------------------------------------------ */

static const wchar_t *kCertStores[] = { L"Root", L"TrustedPublisher" };

static std::wstring Hex(const BYTE *p, DWORD n)
{
    std::wstring s;
    for (DWORD i = 0; i < n; i++)
        s += Fmt(L"%02X", p[i]);
    return s;
}

static std::vector<std::wstring> ReadRecord(const std::wstring &file, bool &exists)
{
    std::vector<std::wstring> stores;
    std::vector<BYTE> data;
    exists = ReadFileBytes(file, data);
    if (!exists)
        return stores;
    std::wstring t = FromUtf8(std::string(data.begin(), data.end()));
    size_t pos = 0;
    while (pos < t.size()) {
        size_t eol = t.find(L'\n', pos);
        std::wstring line = t.substr(pos, eol == std::wstring::npos ? std::wstring::npos : eol - pos);
        pos = eol == std::wstring::npos ? t.size() : eol + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' '))
            line.pop_back();
        for (const wchar_t *s : kCertStores)
            if (EqualsI(line, s))
                stores.push_back(s);
    }
    return stores;
}

static bool WriteRecord(const std::wstring &file, const std::vector<std::wstring> &stores, const std::wstring &thumb)
{
    std::wstring t = L"# certificate stores Apollo setup added " + thumb + L" to (uninstall removes it only from these)\n";
    for (auto &s : stores)
        t += s + L"\n";
    std::string u = ToUtf8(t);
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD done = 0;
    BOOL ok = WriteFile(h, u.data(), (DWORD)u.size(), &done, nullptr);
    CloseHandle(h);
    return ok && done == u.size();
}

int CmdCert(bool add, const std::wstring &cer, const std::wstring &expectArg, const std::wstring &record)
{
    if (!RequireElevated(add ? L"cert add" : L"cert remove"))
        return EXIT_NOT_ELEVATED;
    std::vector<BYTE> data;
    if (!ReadFileBytes(cer, data) || data.empty()) {
        Err(L"Cannot read %s: %s", cer.c_str(), ErrText(GetLastError()).c_str());
        return EXIT_ERROR;
    }
    PCCERT_CONTEXT ctx = CertCreateCertificateContext(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, data.data(),
                                                      (DWORD)data.size());
    if (ctx == nullptr) {
        Err(L"%s is not a DER certificate: %s", cer.c_str(), ErrText(GetLastError()).c_str());
        return EXIT_ERROR;
    }
    BYTE hash[20];
    DWORD cb = sizeof(hash);
    if (!CertGetCertificateContextProperty(ctx, CERT_SHA1_HASH_PROP_ID, hash, &cb) || cb != 20) {
        Err(L"Cannot hash the certificate.");
        CertFreeCertificateContext(ctx);
        return EXIT_ERROR;
    }
    std::wstring thumb = Hex(hash, cb);
    wchar_t subject[256] = L"";
    CertGetNameStringW(ctx, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, subject, 256);
    Out(L"Certificate: %s, thumbprint %s", subject, thumb.c_str());

    std::wstring expect;
    for (wchar_t c : expectArg)
        if (c != L' ' && c != L':')
            expect += c;
    if (!expect.empty() && !EqualsI(expect, thumb)) {
        Err(L"This is not the expected certificate (expected %s): nothing changed.", expect.c_str());
        CertFreeCertificateContext(ctx);
        return EXIT_ERROR;
    }

    CRYPT_HASH_BLOB blob = { cb, hash };
    int rc = EXIT_OK;
    bool recExists = false;
    std::vector<std::wstring> recorded = record.empty() ? std::vector<std::wstring>() : ReadRecord(record, recExists);

    for (const wchar_t *name : kCertStores) {
        HCERTSTORE st = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                      CERT_SYSTEM_STORE_LOCAL_MACHINE | (add ? 0 : CERT_STORE_OPEN_EXISTING_FLAG), name);
        if (st == nullptr) {
            Err(L"  LocalMachine\\%s: cannot open: %s", name, ErrText(GetLastError()).c_str());
            rc = EXIT_ERROR;
            continue;
        }
        if (add) {
            PCCERT_CONTEXT found = CertFindCertificateInStore(st, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                                                              CERT_FIND_SHA1_HASH, &blob, nullptr);
            if (found != nullptr) {
                CertFreeCertificateContext(found);
                Out(L"  LocalMachine\\%s: already there", name);
            } else if (CertAddCertificateContextToStore(st, ctx, CERT_STORE_ADD_NEW, nullptr)) {
                Out(L"  LocalMachine\\%s: added", name);
                bool dup = false;
                for (auto &r : recorded)
                    dup = dup || EqualsI(r, name);
                if (!dup)
                    recorded.push_back(name);
            } else {
                Err(L"  LocalMachine\\%s: could not add: %s", name, ErrText(GetLastError()).c_str());
                rc = EXIT_ERROR;
            }
        } else {
            bool listed = !recExists;
            for (auto &r : recorded)
                listed = listed || EqualsI(r, name);
            if (!listed) {
                Out(L"  LocalMachine\\%s: left in place (it was there before Apollo setup ran)", name);
            } else {
                int removed = 0;
                for (;;) {
                    PCCERT_CONTEXT found = CertFindCertificateInStore(st, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                                                                      CERT_FIND_SHA1_HASH, &blob, nullptr);
                    if (found == nullptr)
                        break;
                    if (!CertDeleteCertificateFromStore(found)) {   /* frees found either way */
                        Err(L"  LocalMachine\\%s: could not remove: %s", name, ErrText(GetLastError()).c_str());
                        rc = EXIT_ERROR;
                        break;
                    }
                    removed++;
                }
                Out(L"  LocalMachine\\%s: %s", name, removed ? L"removed" : L"was not there");
            }
        }
        CertCloseStore(st, 0);
    }
    if (add && !record.empty() && !WriteRecord(record, recorded, thumb)) {
        Err(L"Could not write %s: %s", record.c_str(), ErrText(GetLastError()).c_str());
        rc = EXIT_ERROR;
    }
    CertFreeCertificateContext(ctx);
    return rc;
}

/* ------------------------------------------------------------------ */
/* registry defaults: a small JSON reader                               */
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
                std::wstring w(1, (wchar_t)cp);
                u += ToUtf8(w);
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

static bool ValidValueName(const std::wstring &n)
{
    if (n.empty() || n.size() > 64)
        return false;
    for (wchar_t c : n)
        if (!((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'_'))
            return false;
    return true;
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
        if (end != nullptr && *end == L'\0' && x <= 0xFFFFFFFFull) {
            out = (DWORD)x;
            return true;
        }
    }
    return false;
}

/*
 * defaults.json:
 *   { "overwrite": false,
 *     "values": { "StripsEnable": 0,                     number      -> REG_DWORD
 *                 "Name": { "type": "dword", "data": "0xFF", "overwrite": true },
 *                 "Text": { "type": "sz", "data": "..." } } }
 * Keys starting with "_" are comments. "overwrite": false keeps a value that is already set (an
 * upgrade keeps the user's tuning); per-value "overwrite" wins over the top-level one.
 */
int CmdRegDefaults(const std::wstring &file)
{
    if (!RequireElevated(L"regdefaults"))
        return EXIT_NOT_ELEVATED;
    std::vector<BYTE> data;
    if (!ReadFileBytes(file, data)) {
        Err(L"Cannot read %s: %s", file.c_str(), ErrText(GetLastError()).c_str());
        return EXIT_ERROR;
    }
    const char *b = (const char *)data.data(), *e = b + data.size();
    if (data.size() >= 3 && (BYTE)b[0] == 0xEF && (BYTE)b[1] == 0xBB && (BYTE)b[2] == 0xBF)
        b += 3;
    JVal root;
    JParser jp(b, e);
    if (!jp.Parse(root) || root.T != JVal::Obj) {
        Err(L"%s is not valid JSON (stopped near byte %zu): nothing written.", file.c_str(), jp.Offset(b));
        return EXIT_ERROR;
    }
    const JVal *ov = root.Get(L"overwrite");
    bool overwriteAll = ov != nullptr && ov->T == JVal::Bool && ov->B;
    const JVal *values = root.Get(L"values");
    if (values == nullptr || values->T != JVal::Obj) {
        Err(L"%s has no \"values\" object: nothing written.", file.c_str());
        return EXIT_ERROR;
    }

    /* Validate everything before writing anything. */
    struct Item { std::wstring Name; DWORD Type; DWORD Dw; std::wstring Sz; bool Overwrite; };
    std::vector<Item> items;
    for (auto &kv : values->O) {
        if (!kv.first.empty() && kv.first[0] == L'_')
            continue;
        if (!ValidValueName(kv.first)) {
            Err(L"Bad value name '%s' (letters, digits, _ only): nothing written.", kv.first.c_str());
            return EXIT_ERROR;
        }
        Item it = { kv.first, REG_DWORD, 0, std::wstring(), overwriteAll };
        const JVal *dataV = &kv.second;
        std::wstring type = L"dword";
        if (kv.second.T == JVal::Obj) {
            const JVal *t = kv.second.Get(L"type");
            const JVal *o = kv.second.Get(L"overwrite");
            dataV = kv.second.Get(L"data");
            if (t != nullptr && t->T == JVal::Str)
                type = Lower(t->S);
            if (o != nullptr && o->T == JVal::Bool)
                it.Overwrite = o->B;
            if (dataV == nullptr) {
                Err(L"'%s' has no \"data\": nothing written.", kv.first.c_str());
                return EXIT_ERROR;
            }
        } else if (kv.second.T == JVal::Str) {
            type = L"sz";
        }
        if (type == L"dword") {
            if (!ToDword(*dataV, it.Dw)) {
                Err(L"'%s' needs a whole number 0..4294967295: nothing written.", kv.first.c_str());
                return EXIT_ERROR;
            }
        } else if (type == L"sz" && dataV->T == JVal::Str) {
            it.Type = REG_SZ;
            it.Sz = dataV->S;
        } else {
            Err(L"'%s': type must be \"dword\" or \"sz\": nothing written.", kv.first.c_str());
            return EXIT_ERROR;
        }
        items.push_back(it);
    }

    HKEY k = nullptr;
    DWORD disp = 0;
    LSTATUS st = RegCreateKeyExW(HKEY_LOCAL_MACHINE, APOLLO_PARAMS_KEY, 0, nullptr, 0,
                                 KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &k, &disp);
    if (st != ERROR_SUCCESS) {
        Err(L"Cannot open HKLM\\%s: %s", APOLLO_PARAMS_KEY, ErrText((DWORD)st).c_str());
        return EXIT_ERROR;
    }
    Out(L"HKLM\\%s:", APOLLO_PARAMS_KEY);
    int rc = EXIT_OK;
    for (auto &it : items) {
        DWORD type = 0, cur = 0, size = sizeof(cur);
        LSTATUS q = RegQueryValueExW(k, it.Name.c_str(), nullptr, &type, (BYTE *)&cur, &size);
        bool exists = q == ERROR_SUCCESS || q == ERROR_MORE_DATA;
        if (exists && !it.Overwrite) {
            if (q == ERROR_SUCCESS && type == REG_DWORD)
                Out(L"  %-24s kept the existing value %lu (0x%lX)", it.Name.c_str(), cur, cur);
            else
                Out(L"  %-24s kept the existing value", it.Name.c_str());
            continue;
        }
        if (it.Type == REG_DWORD) {
            st = RegSetValueExW(k, it.Name.c_str(), 0, REG_DWORD, (const BYTE *)&it.Dw, sizeof(DWORD));
            if (st == ERROR_SUCCESS)
                Out(L"  %-24s = %lu (0x%lX)", it.Name.c_str(), it.Dw, it.Dw);
        } else {
            st = RegSetValueExW(k, it.Name.c_str(), 0, REG_SZ, (const BYTE *)it.Sz.c_str(),
                                (DWORD)((it.Sz.size() + 1) * sizeof(wchar_t)));
            if (st == ERROR_SUCCESS)
                Out(L"  %-24s = \"%s\"", it.Name.c_str(), it.Sz.c_str());
        }
        if (st != ERROR_SUCCESS) {
            Err(L"  %s: %s", it.Name.c_str(), ErrText((DWORD)st).c_str());
            rc = EXIT_ERROR;
        }
    }
    RegCloseKey(k);
    return rc;
}
