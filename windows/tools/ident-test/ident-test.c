/*
 * ident-test.c - checks driver\apollo_model.h (which Apollo, accepted or refused) and
 * driver\apollo_unit.h (per-unit Parameters) on the host, gate G0 of docs/spec-twinx.md 7.
 *
 *   1. open-apollo's own public-type and model-resolution asserts
 *      (oa:driver/tests/test_public_type.py), against our port;
 *   2. whole identifications: FPGA_REV, ext_caps, PCI subsystem, firmware, HW_UID and serial
 *      bytes -> verdict, model, device type, public type, serial text, unit key; Aaron's x8p
 *      as read on Nov 14 2026, synthetic Twin X Gen 1 units, every Gen 2 serial, other models;
 *   3. the unit key and the serial clean-up on their own;
 *   4. the Parameters lookup order and the Mode rules: the x8p keeps today's values, a new
 *      unit starts in observe mode with strips and endpoints off;
 *   5. [G2] device add: the PCI subsystem from the hardware ID, the ACX circuits created
 *      before identification (the x8p's slot as before, the Twin X's none), the log tag.
 *
 * Exit code 0 = all passed.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <string.h>
#include "..\..\driver\apollo_unit.h"   /* includes apollo_model.h */

static int g_checks, g_fail;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        g_checks++;                                             \
        if (!(cond)) {                                          \
            g_fail++;                                           \
            printf("FAIL line %d: ", __LINE__);                 \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

/* ------------------------------------------------------------------ */
/* 1. open-apollo's asserts                                            */
/* ------------------------------------------------------------------ */

static unsigned long Pub(unsigned long Family, unsigned long St, unsigned long Fpga)
{
    return ApolloPublicType(1, Family << 20, Fpga, St);
}

static unsigned long Resolve(unsigned long Family, unsigned long St, unsigned long Fpga,
                             unsigned long Type)
{
    return ApolloResolveDeviceType(Pub(Family, St, Fpga), Type);
}

static void TestUpstream(void)
{
    const unsigned long old = 0x90000000;   /* old-format FPGA_REV: no type byte */
    const unsigned long x4f = 0x9f0ea929;   /* titan */
    const unsigned long NONE = APOLLO_PUBTYPE_NONE;

    CHECK(ApolloTypeFromFpgaRev(x4f) == 0x1F, "fpga type x4");
    CHECK(ApolloTypeFromFpgaRev(old) == 9, "fpga type old");

    CHECK(Pub(1, 0, old) == 0x05, "pub 1");
    CHECK(Pub(5, 0, old) == 0x08, "pub 5");
    CHECK(Pub(10, 0, old) == 0x0F, "pub 10");
    CHECK(Pub(11, 0, old) == 0x0E, "pub 11");
    CHECK(Pub(14, 0, old) == 0x12, "pub 14");
    CHECK(Pub(18, 0, old) == 0x18, "pub 18");
    CHECK(Pub(20, 0, old) == 0, "pub 20");
    CHECK(Pub(21, 0, old) == 0, "pub 21");
    CHECK(Pub(22, 0, old) == 0x20, "pub 22");
    CHECK(Pub(23, 0, old) == 0, "pub 23");
    CHECK(Pub(0, 0, old) == 0, "pub 0");
    CHECK(Pub(0x3f, 0, old) == NONE, "pub 3f");

    CHECK(Pub(16, 0x1F, x4f) == 0x1A, "quad serial");
    CHECK(Pub(16, 0, x4f) == 0x1A, "quad fpga");
    CHECK(Pub(16, 0, 0xb60ea929) == 0x26, "quad gen2");
    CHECK(Pub(16, 0, 0xbf0ea929) == 0x28, "quad gen2 tb4");
    CHECK(Pub(16, 0, old) == NONE, "quad none");

    CHECK(Pub(17, 0x22, old) == 0x16, "hexa x8p");
    CHECK(Pub(17, 0x35, old) == 0x21, "hexa x16d fall-through");
    CHECK(Pub(17, 0x3A, old) == 0x24, "hexa x16 gen2");
    CHECK(Pub(17, 0, 0xb50ea929) == 0x20, "hexa fpga x16d");
    CHECK(Pub(17, 0x36, 0xb50ea929) == 0x20, "hexa x4gen2 serial, fpga x16d");
    CHECK(Pub(17, 0, old) == 0x16, "hexa default");

    CHECK(Pub(15, 0x1E, old) == 0x13, "conrad 2016");
    CHECK(Pub(15, 0x2A, old) == 0x1C, "conrad 2032");
    CHECK(Pub(15, 0x3E, old) == 0x27, "conrad 2110");
    CHECK(Pub(19, 0x27, old) == 0x19, "twin x solo serial");
    CHECK(Pub(19, 0x3B, old) == 0x25, "twin x gen2");
    CHECK(ApolloPublicType(0, 0, 0, 0) == 0, "v1");

    CHECK(Resolve(5, 0, old, 0) == 0x88, "twin");
    CHECK(Resolve(10, 0, old, 0) == 0x8F, "twin b");
    CHECK(Resolve(1, 0, old, 0) == 0x85, "thunderbolt");
    CHECK(Resolve(4, 0, old, 0) == 0x87, "16");
    CHECK(Resolve(6, 0, old, 0) == 0x8B, "rack");
    CHECK(Resolve(8, 0, old, 0) == 0x8C, "8");
    CHECK(Resolve(9, 0, old, 0) == 0x8D, "16 mkii");
    CHECK(Resolve(14, 0, old, 0) == 0x92, "twin mkii");
    CHECK(Resolve(19, 0, old, 0) == 0x99, "twin x");
    CHECK(Resolve(11, 0, old, 0) == 0x8E, "8p public");
    CHECK(Resolve(11, 0, old, 0x0A) == 0x0A, "8p pinned");
    CHECK(Resolve(16, 0x1F, x4f, 0x1F) == 0x1F, "x4 named");
    CHECK(Resolve(15, 0x2A, old, 0x27) == 0x27, "pinned solo stays");
    CHECK(Resolve(19, 0x27, old, 0x27) == 0x99, "2024 on twin x family");
    CHECK(Resolve(19, 0x28, old, 0x28) == 0x99, "2025 on twin x family");
    CHECK(Resolve(15, 0x1E, old, 0x1E) == 0x93, "arrow");
    CHECK(Resolve(15, 0x2A, old, 0x2A) == 0x9C, "solo public");
    CHECK(Resolve(15, 0x3E, old, 0x3E) == 0xA7, "solo public b");
    CHECK(Resolve(15, 0x1E, old, 0x27) == 0x93, "pinned solo, arrow public");
    CHECK(Resolve(15, 0x3E, old, 0x27) == 0x27, "pinned solo, 2110");
    CHECK(Resolve(17, 0x1E, old, 0x1E) == 0x1E, "hexa 2016 not retyped");
    CHECK(Resolve(16, 0, old, 0) == 0, "quad none unchanged");
    CHECK(Resolve(20, 0, old, 0) == 0, "family 20 unchanged");
    CHECK(Resolve(0x3f, 0, old, 0) == 0, "family 3f unchanged");
}

/* ------------------------------------------------------------------ */
/* 2. Whole identifications                                            */
/* ------------------------------------------------------------------ */

#define X8P_FPGA    0xA241C5ACul        /* Aaron's x8p, apollo-diag info Nov 14 2026 */
#define X8P_EXT     0x81110611ul
#define X8P_FW      0x0B0FD613ul
#define X8P_SERIAL  "20000000000001"
#define TWX_FPGA    0x9A0F1234ul        /* synthetic: Gen 1 values are unknown until G2 */
#define TWX_EXT4    0x81300410ul        /* bit 31, family 19, 4 DSPs, ready cap 1 */
#define TWX_EXT2    0x81300210ul        /* Duo */

typedef struct _CASE {
    const char     *What;
    unsigned long   Fpga, Ext, Sub, Fw, UidHi, UidLo;
    const char     *Serial;             /* text; NULL = all 16 bytes 0xFF */
    unsigned char   Tail;               /* bytes after the text */
    int             Verdict, Model;
    unsigned long   Type, Public;
    int             FwUntested;         /* -1 = do not check */
    const char     *WantSerial;         /* NULL = do not check */
    const char     *WantKey;            /* NULL = do not check */
} CASE;

#define ACC     APOLLO_IDENT_ACCEPTED
#define REF     APOLLO_IDENT_REFUSED
#define NAP     APOLLO_IDENT_NOT_APOLLO
#define OFF     APOLLO_IDENT_OFFLINE
#define MX8P    APOLLO_MODEL_X8P
#define MTWX    APOLLO_MODEL_TWINX
#define MNONE   APOLLO_MODEL_NONE

static const CASE g_Cases[] = {
    /* Aaron's x8p, whatever the two bytes after the 14 digits are */
    { "x8p as read, NUL tail", X8P_FPGA, X8P_EXT, 0x14, X8P_FW, 0, 0, X8P_SERIAL, 0x00,
      ACC, MX8P, 0x22, 0x16, 0, X8P_SERIAL, "SN" X8P_SERIAL },
    { "x8p, 0xFF tail", X8P_FPGA, X8P_EXT, 0x14, X8P_FW, 0, 0, X8P_SERIAL, 0xFF,
      ACC, MX8P, 0x22, 0x16, 0, X8P_SERIAL, "SN" X8P_SERIAL },
    { "x8p, literal '?' tail", X8P_FPGA, X8P_EXT, 0x14, X8P_FW, 0, 0, X8P_SERIAL, '?',
      ACC, MX8P, 0x22, 0x16, 0, X8P_SERIAL, "SN" X8P_SERIAL },
    { "x8p, CR tail", X8P_FPGA, X8P_EXT, 0x14, X8P_FW, 0, 0, X8P_SERIAL, 0x0D,
      ACC, MX8P, 0x22, 0x16, 0, X8P_SERIAL, "SN" X8P_SERIAL },
    { "x8p, 0x80 tail", X8P_FPGA, X8P_EXT, 0x14, X8P_FW, 0, 0, X8P_SERIAL, 0x80,
      ACC, MX8P, 0x22, 0x16, 0, X8P_SERIAL, "SN" X8P_SERIAL },
    { "x8p with a HW_UID", X8P_FPGA, X8P_EXT, 0x14, X8P_FW, 0x0123ABCDul, 0x89EF4567ul, X8P_SERIAL, 0,
      ACC, MX8P, 0x22, 0x16, 0, X8P_SERIAL, "0123ABCD89EF4567" },
    { "x8p, other firmware: flagged, accepted", X8P_FPGA, X8P_EXT, 0x14, 0x0B0FD614ul, 0, 0, X8P_SERIAL, 0,
      ACC, MX8P, 0x22, 0x16, 1, NULL, NULL },
    { "x8p, other FPGA: flagged, accepted", 0xA241C5ADul, X8P_EXT, 0x14, X8P_FW, 0, 0, X8P_SERIAL, 0,
      ACC, MX8P, 0x22, 0x16, 1, NULL, NULL },
    { "x8p, serial unreadable: subsystem fallback", X8P_FPGA, X8P_EXT, 0x14, X8P_FW, 0, 0, NULL, 0,
      ACC, MX8P, 0x22, 0x16, 0, "", "" },
    { "x8p digits, 4 DSPs", X8P_FPGA, 0x81110411ul, 0x14, X8P_FW, 0, 0, X8P_SERIAL, 0,
      REF, MNONE, 0x22, 0x16, -1, NULL, NULL },
    { "x8p digits on the Twin X family", TWX_FPGA, TWX_EXT4, 0x19, 0, 0, 0, X8P_SERIAL, 0,
      REF, MNONE, 0x22, 0x19, -1, NULL, NULL },
    { "x8p Gen 2 (2088)", X8P_FPGA, X8P_EXT, 0x14, X8P_FW, 0, 0, "22532088106180", 0,
      REF, MNONE, 0x39, 0x23, -1, NULL, NULL },
    { "x8 (2018)", X8P_FPGA, X8P_EXT, 0x14, X8P_FW, 0, 0, "22532018106180", 0,
      REF, MNONE, 0x21, 0x15, -1, NULL, NULL },
    { "Apollo 8p (subsystem 6)", 0x9A000000ul, 0x80B00410ul, 0x06, 0, 0, 0, "12345678901234", 0,
      REF, MNONE, 0x0A, 0x0E, -1, NULL, NULL },

    /* Twin X Gen 1 (synthetic: family 19, serial with no table entry or 2024/2025) */
    { "Twin X Gen 1, Quad", TWX_FPGA, TWX_EXT4, 0x19, 0x0B0F0000ul, 0, 0, "22532026123456", 0,
      ACC, MTWX, 0x99, 0x19, 1, "22532026123456", "SN22532026123456" },
    { "Twin X Gen 1, Duo", TWX_FPGA, TWX_EXT2, 0x19, 0, 0x11112222ul, 0x33334444ul, "22532026123456", 0,
      ACC, MTWX, 0x99, 0x19, 1, NULL, "1111222233334444" },
    { "Twin X Gen 1, serial 2024 (Solo digits)", TWX_FPGA, TWX_EXT4, 0x19, 0, 0, 0, "22532024123456", 0,
      ACC, MTWX, 0x99, 0x19, 1, NULL, NULL },
    { "Twin X Gen 1, serial 2025 (Arrow digits)", TWX_FPGA, TWX_EXT4, 0x19, 0, 0, 0, "22532025123456", 0,
      ACC, MTWX, 0x99, 0x19, 1, NULL, NULL },
    { "Twin X Gen 1 on another subsystem", TWX_FPGA, TWX_EXT4, 0x1A, 0, 0, 0, "22532026123456", 0,
      ACC, MTWX, 0x99, 0x19, 1, NULL, NULL },
    { "Twin X family, serial unreadable", TWX_FPGA, TWX_EXT4, 0x19, 0, 0, 0, NULL, 0,
      REF, MNONE, 0x99, 0x19, -1, "", "" },
    { "Twin X family, digits 5-8 not digits", TWX_FPGA, TWX_EXT4, 0x19, 0, 0, 0, "2253AB26123456", 0,
      REF, MNONE, 0x99, 0x19, -1, NULL, NULL },
    { "Twin X family, no ext_caps bit 31", TWX_FPGA, 0x01300410ul, 0x19, 0, 0, 0, "22532026123456", 0,
      REF, MNONE, 0x99, 0x19, -1, NULL, NULL },
    { "Twin X family, 6 DSPs", TWX_FPGA, 0x81300610ul, 0x19, 0, 0, 0, "22532026123456", 0,
      REF, MNONE, 0x99, 0x19, -1, NULL, NULL },
    { "Twin X family, 0 DSPs", TWX_FPGA, 0x81300010ul, 0x19, 0, 0, 0, "22532026123456", 0,
      REF, MNONE, 0x99, 0x19, -1, NULL, NULL },

    /* Twin X Gen 2: every serial of it, refused and named */
    { "Twin X Gen 2 (2090)", TWX_FPGA, TWX_EXT2, 0x19, 0, 0, 0, "22532090123456", 0,
      REF, MNONE, 0x3B, 0x25, -1, NULL, NULL },
    { "Twin X Gen 2 (2091)", TWX_FPGA, TWX_EXT4, 0x19, 0, 0, 0, "22532091123456", 0,
      REF, MNONE, 0x3C, 0x25, -1, NULL, NULL },
    { "Twin X Gen 2 TB4 (2107)", TWX_FPGA, TWX_EXT2, 0x19, 0, 0, 0, "22532107123456", 0,
      REF, MNONE, 0x40, 0x29, -1, NULL, NULL },
    { "Twin X Gen 2 TB4 (2108)", TWX_FPGA, TWX_EXT4, 0x19, 0, 0, 0, "22532108123456", 0,
      REF, MNONE, 0x41, 0x29, -1, NULL, NULL },

    /* Other models */
    { "Solo (subsystem 0x0F, 2032)", 0x9A000000ul, 0x80F00110ul, 0x0F, 0, 0, 0, "22532032123456", 0,
      REF, MNONE, 0x27, 0x1C, -1, NULL, NULL },
    { "Arrow (subsystem 0x0F, 2016)", 0x9A000000ul, 0x80F00110ul, 0x0F, 0, 0, 0, "22532016123456", 0,
      REF, MNONE, 0x93, 0x13, -1, NULL, NULL },
    { "x4 (2005)", 0x9f0ea929ul, 0x81000410ul, 0x11, 0, 0, 0, "22532005123456", 0,
      REF, MNONE, 0x1F, 0x1A, -1, NULL, NULL },
    { "Twin MkII (family 14)", 0x9A000000ul, 0x80E00210ul, 0x0C, 0, 0, 0, "22530000123456", 0,
      REF, MNONE, 0x92, 0x12, -1, NULL, NULL },
    { "family 0x3F", 0x9A000000ul, 0x83F00410ul, 0x19, 0, 0, 0, "22530000123456", 0,
      REF, MNONE, 0, APOLLO_PUBTYPE_NONE, -1, NULL, NULL },

    /* Not an Apollo this driver can quiesce, or gone */
    { "v1 firmware", 0x30000000ul, 0, 0x14, 0, 0, 0, X8P_SERIAL, 0,
      NAP, MNONE, 0, 0, -1, NULL, NULL },
    { "9 DSPs", X8P_FPGA, 0x81110911ul, 0x14, 0, 0, 0, X8P_SERIAL, 0,
      NAP, MNONE, 0, 0, -1, NULL, NULL },
    { "FPGA_REV all ones", 0xFFFFFFFFul, 0, 0x14, 0, 0, 0, X8P_SERIAL, 0,
      OFF, MNONE, 0, 0, -1, NULL, NULL },
    { "ext_caps all ones", X8P_FPGA, 0xFFFFFFFFul, 0x14, 0, 0, 0, X8P_SERIAL, 0,
      OFF, MNONE, 0, 0, -1, NULL, NULL },
};

static void MakeIn(const CASE *C, APOLLO_IDENT_IN *In)
{
    size_t n;

    memset(In, 0, sizeof(*In));
    In->FpgaRev = C->Fpga;
    In->ExtCaps = C->Ext;
    In->SubsystemId = C->Sub;
    In->FwRelease = C->Fw;
    In->HwUidHi = C->UidHi;
    In->HwUidLo = C->UidLo;
    if (C->Serial == NULL) {
        memset(In->SerialRaw, 0xFF, sizeof(In->SerialRaw));
        return;
    }
    n = strlen(C->Serial);
    if (n > 16) {
        n = 16;
    }
    memset(In->SerialRaw, C->Tail, sizeof(In->SerialRaw));
    memcpy(In->SerialRaw, C->Serial, n);
}

static void TestCases(void)
{
    size_t i;

    for (i = 0; i < sizeof(g_Cases) / sizeof(g_Cases[0]); i++) {
        const CASE *c = &g_Cases[i];
        APOLLO_IDENT_IN in;
        APOLLO_IDENT id;

        MakeIn(c, &in);
        ApolloIdentResolve(&in, &id);

        CHECK(id.Verdict == c->Verdict, "%s: verdict %d, want %d (%s)", c->What, id.Verdict, c->Verdict, id.Why);
        CHECK(id.Model == c->Model, "%s: model %d, want %d", c->What, id.Model, c->Model);
        if (c->Verdict == ACC || c->Verdict == REF) {
            CHECK(id.DeviceType == c->Type, "%s: type 0x%02lX, want 0x%02lX", c->What, id.DeviceType, c->Type);
            CHECK(id.PublicType == c->Public, "%s: public 0x%02lX, want 0x%02lX", c->What, id.PublicType,
                  c->Public);
        }
        CHECK((id.Verdict == ACC) == (id.Why[0] == 0), "%s: Why '%s' does not match the verdict", c->What,
              id.Why);
        if (c->FwUntested >= 0) {
            CHECK(id.FwUntested == c->FwUntested, "%s: FwUntested %d, want %d", c->What, id.FwUntested,
                  c->FwUntested);
        }
        if (c->WantSerial != NULL) {
            CHECK(strcmp(id.Serial, c->WantSerial) == 0 && id.SerialLen == strlen(c->WantSerial),
                  "%s: serial '%s' (%lu), want '%s'", c->What, id.Serial, id.SerialLen, c->WantSerial);
        }
        if (c->WantKey != NULL) {
            CHECK(strcmp(id.UnitKey, c->WantKey) == 0, "%s: unit key '%s', want '%s'", c->What, id.UnitKey,
                  c->WantKey);
            CHECK((id.UnitKeyKind == APOLLO_UNIT_KEY_NONE) == (c->WantKey[0] == 0), "%s: unit key kind %d",
                  c->What, id.UnitKeyKind);
        }
        /* Never mis-named: an accepted unit's name is its model's, a refused one never claims it. */
        if (id.Model == MX8P) {
            CHECK(strcmp(id.Name, "Apollo x8p") == 0, "%s: accepted x8p named '%s'", c->What, id.Name);
        }
        if (id.Model == MTWX) {
            CHECK(strcmp(id.Name, "Apollo Twin X (Gen 1)") == 0, "%s: accepted Twin X named '%s'", c->What,
                  id.Name);
        }
        printf("  %-44s %-8s type 0x%02lX %-24s %s\n", c->What,
               id.Verdict == ACC ? "ACCEPT" : id.Verdict == REF ? "refuse" : id.Verdict == NAP ? "not-apollo" : "offline",
               id.DeviceType, id.Name, id.Why);
    }
}

/* ------------------------------------------------------------------ */
/* 3. Serial and unit key                                              */
/* ------------------------------------------------------------------ */

static void TestSerialAndKey(void)
{
    static const struct {
        unsigned char Raw[16];
        const char   *Want;
        int           DigitsOk;
    } s[] = {
        { "20000000000001\0", "20000000000001", 1 },
        { { '2','2','5','3','2','0','1','7','1','0','6','1','8','0',0xC2,0xA0 }, "20000000000001", 1 },
        { { '2','2','5','3','2','0','1','7','1','0','6','1','8','0','1','2' }, "2000000000000112", 1 },
        { { '2','2','5','3','2','0','1','7','1','0',0x01,'1','8','0','1','2' }, "2253201710", 1 },
        { { ' ','2','5','3','2','0','1','7','1','0','6','1','8','0','1','2' }, "", 1 },
        { { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF }, "", 0 },
        { "2253-2017-1061?", "2253-2017-1061", 0 },
        { "ABCD2017\0\0\0\0\0\0\0", "ABCD2017", 1 },
    };
    char out[17], key[APOLLO_UNIT_KEY_CHARS];
    size_t i;

    for (i = 0; i < sizeof(s) / sizeof(s[0]); i++) {
        unsigned long n = ApolloSerialClean(s[i].Raw, out);

        CHECK(strcmp(out, s[i].Want) == 0 && n == strlen(s[i].Want), "serial %u: '%s', want '%s'",
              (unsigned)i, out, s[i].Want);
        CHECK(ApolloSerialDigitsOk(s[i].Raw) == s[i].DigitsOk, "serial %u: digits check", (unsigned)i);
    }

    CHECK(ApolloUnitKeyName(0x12345678ul, 0x9ABCDEF0ul, "", 0, key) == APOLLO_UNIT_KEY_HWUID &&
          strcmp(key, "123456789ABCDEF0") == 0, "key hwuid: '%s'", key);
    CHECK(ApolloUnitKeyName(0, 1, "", 0, key) == APOLLO_UNIT_KEY_HWUID && strcmp(key, "0000000000000001") == 0,
          "key hwuid low only: '%s'", key);
    CHECK(ApolloUnitKeyName(0xFFFFFFFFul, 0, X8P_SERIAL, 14, key) == APOLLO_UNIT_KEY_HWUID &&
          strcmp(key, "FFFFFFFF00000000") == 0, "key hwuid high ones: '%s'", key);
    CHECK(ApolloUnitKeyName(0, 0, X8P_SERIAL, 14, key) == APOLLO_UNIT_KEY_SERIAL &&
          strcmp(key, "SN" X8P_SERIAL) == 0, "key serial: '%s'", key);
    CHECK(ApolloUnitKeyName(0xFFFFFFFFul, 0xFFFFFFFFul, X8P_SERIAL, 14, key) == APOLLO_UNIT_KEY_SERIAL &&
          strcmp(key, "SN" X8P_SERIAL) == 0, "key all-ones uid: '%s'", key);
    CHECK(ApolloUnitKeyName(0, 0, "2000000000000112", 16, key) == APOLLO_UNIT_KEY_SERIAL &&
          strcmp(key, "SN2000000000000112") == 0, "key serial 16: '%s'", key);
    CHECK(ApolloUnitKeyName(0, 0, "1234567", 7, key) == APOLLO_UNIT_KEY_NONE && key[0] == 0,
          "key short serial: '%s'", key);
    CHECK(ApolloUnitKeyName(0, 0, "2253-2017-1061", 14, key) == APOLLO_UNIT_KEY_NONE && key[0] == 0,
          "key serial with '-': '%s'", key);
    CHECK(ApolloUnitKeyName(0, 0, "..\\Units", 8, key) == APOLLO_UNIT_KEY_NONE && key[0] == 0,
          "key path characters: '%s'", key);
}

/* ------------------------------------------------------------------ */
/* 4. Parameters                                                       */
/* ------------------------------------------------------------------ */

typedef struct _LEVEL {
    int             Have;
    unsigned long   Value;
} LEVEL;

typedef struct _REG {
    LEVEL Unit[APOLLO_PARAM_COUNT];
    LEVEL Model[APOLLO_PARAM_COUNT];
    LEVEL Global[APOLLO_PARAM_COUNT];
} REG;

/* What unit.c does with registry reads, minus the registry. */
static void Pick(int Model, const REG *R, APOLLO_UNIT *U)
{
    int i;

    memset(U, 0, sizeof(*U));
    for (i = 0; i < APOLLO_PARAM_COUNT; i++) {
        APOLLO_PARAM_LEVELS l;

        l.HaveUnit = R->Unit[i].Have;
        l.Unit = R->Unit[i].Value;
        l.HaveModel = R->Model[i].Have;
        l.Model = R->Model[i].Value;
        l.HaveGlobal = (Model == APOLLO_MODEL_X8P) && R->Global[i].Have;
        l.Global = R->Global[i].Value;
        U->Value[i] = ApolloParamPick(Model, i, &l, &U->Source[i]);
    }
    ApolloUnitFinish(Model, U);
}

static void Set(LEVEL *L, int Id, unsigned long V)
{
    L[Id].Have = 1;
    L[Id].Value = V;
}

/* The values Aaron's PC holds today (step3.ps1 / strips-steps.ps1, Nov 14 2026). */
static void TodaysGlobals(REG *R)
{
    Set(R->Global, APOLLO_PARAM_ENABLED_ENDPOINTS, 0x3FF);
    Set(R->Global, APOLLO_PARAM_STRIPS_ENABLE, 1);
    Set(R->Global, APOLLO_PARAM_STRIP_MASK, 0x03);
    Set(R->Global, APOLLO_PARAM_STRIP_STEPS, 9);
    Set(R->Global, APOLLO_PARAM_MIXER_INIT_BATCH, 1);
}

static void TestParams(void)
{
    REG r;
    APOLLO_UNIT u;

    /* x8p, empty registry: the driver's old built-in defaults */
    memset(&r, 0, sizeof(r));
    Pick(APOLLO_MODEL_X8P, &r, &u);
    CHECK(u.Value[APOLLO_PARAM_MODE] == 2 && u.Active, "x8p default: mode %lu active %d",
          u.Value[APOLLO_PARAM_MODE], u.Active);
    CHECK(u.Value[APOLLO_PARAM_STRIPS_ENABLE] == 0 && u.Value[APOLLO_PARAM_STRIP_STEPS] == 1 &&
          u.Value[APOLLO_PARAM_STRIP_MASK] == 1 && u.Value[APOLLO_PARAM_STRIP_DSP] == 1 &&
          u.Value[APOLLO_PARAM_MIXER_INIT_BATCH] == 1, "x8p default strips values");
    CHECK(u.Value[APOLLO_PARAM_RENDER_LEAD] == 144 && u.Value[APOLLO_PARAM_CAPTURE_GUARD] == 16 &&
          u.Value[APOLLO_PARAM_START_AT_CONNECT] == 1, "x8p default engine values");
    /* [Endpoints] every output, the loopback, MIC/LINE 1-8 (endpoint_plan.h) */
    CHECK(u.Value[APOLLO_PARAM_ENABLED_ENDPOINTS] == 0x1FFFF, "x8p default endpoints 0x%lX",
          u.Value[APOLLO_PARAM_ENABLED_ENDPOINTS]);
    CHECK(u.Value[APOLLO_PARAM_STRIP_PROBE] == 0, "x8p default StripProbe %lu", u.Value[APOLLO_PARAM_STRIP_PROBE]);
    CHECK(u.Value[APOLLO_PARAM_WARM_BOOT_TRANSPORT] == 0, "x8p default WarmBootTransport %lu",
          u.Value[APOLLO_PARAM_WARM_BOOT_TRANSPORT]);
    /* [G4] the Twin X runs Mode 2 now */
    CHECK(ApolloModelHasBringup(APOLLO_MODEL_X8P) && ApolloModelMaxMode(APOLLO_MODEL_X8P) == 2 &&
          ApolloModelHasBringup(APOLLO_MODEL_TWINX) && ApolloModelMaxMode(APOLLO_MODEL_TWINX) == 2 &&
          !ApolloModelHasBringup(APOLLO_MODEL_NONE) && ApolloModelMaxMode(APOLLO_MODEL_NONE) == 0,
          "bring-up paths and max Modes (x8p 2, Twin X 2, none 0)");
    CHECK(ApolloParamDefault(APOLLO_MODEL_TWINX, APOLLO_PARAM_ENABLED_ENDPOINTS) == 0x3F &&
          ApolloParamDefault(APOLLO_MODEL_X8P, APOLLO_PARAM_ENABLED_ENDPOINTS) == 0x1FFFF &&
          ApolloParamDefault(APOLLO_MODEL_NONE, APOLLO_PARAM_ENABLED_ENDPOINTS) == 0,
          "[Endpoints] Endpoints defaults: Twin X 0x3F, x8p 0x1FFFF, others 0");
    CHECK(wcscmp(ApolloParamDef(APOLLO_PARAM_ENABLED_ENDPOINTS)->NameW, L"Endpoints") == 0,
          "[Endpoints] the value is Endpoints (a stored EnabledEndpoints names the old rows: ignored)");
    CHECK(ApolloParamDefault(APOLLO_MODEL_TWINX, APOLLO_PARAM_MODE) == 0, "Twin X default Mode still 0 (observe)");
    /* [Warm] docs/STRIPS.md 12.5: plug-ins on adopted (x8p) or warm-rebuilt (Twin X) strips by default; StripWarmRebuild
       on for the x8p too since its first warm rebuild was accepted on hardware (Nov 15 2026: a rate change left its mics off) */
    CHECK(ApolloParamDefault(APOLLO_MODEL_X8P, APOLLO_PARAM_STRIP_WARM_PLUGINS) == 1 &&
          ApolloParamDefault(APOLLO_MODEL_TWINX, APOLLO_PARAM_STRIP_WARM_PLUGINS) == 1 &&
          ApolloParamDefault(APOLLO_MODEL_NONE, APOLLO_PARAM_STRIP_WARM_PLUGINS) == 0 &&
          wcscmp(ApolloParamDef(APOLLO_PARAM_STRIP_WARM_PLUGINS)->NameW, L"StripWarmPlugins") == 0 &&
          ApolloParamDefault(APOLLO_MODEL_X8P, APOLLO_PARAM_STRIP_WARM_REBUILD) == 1 &&
          ApolloParamDefault(APOLLO_MODEL_NONE, APOLLO_PARAM_STRIP_WARM_REBUILD) == 0,
          "[Warm] StripWarmPlugins: x8p 1, Twin X 1, others 0; StripWarmRebuild x8p 1, others 0");
    /* [Virtual] docs/STRIPS.md 13: no Virtual Input Strip on any model until proven on hardware; hex in the log */
    CHECK(ApolloParamDefault(APOLLO_MODEL_X8P, APOLLO_PARAM_STRIP_VIRTUAL_MASK) == 0 &&
          ApolloParamDefault(APOLLO_MODEL_TWINX, APOLLO_PARAM_STRIP_VIRTUAL_MASK) == 0 &&
          ApolloParamDefault(APOLLO_MODEL_NONE, APOLLO_PARAM_STRIP_VIRTUAL_MASK) == 0 &&
          wcscmp(ApolloParamDef(APOLLO_PARAM_STRIP_VIRTUAL_MASK)->NameW, L"StripVirtualMask") == 0 &&
          strcmp(ApolloParamDef(APOLLO_PARAM_STRIP_VIRTUAL_MASK)->Name, "StripVirtualMask") == 0 &&
          ApolloParamDef(APOLLO_PARAM_STRIP_VIRTUAL_MASK)->Hex && APOLLO_PARAM_STRIP_VIRTUAL_MASK == 19,
          "[Virtual] StripVirtualMask: id 19, 0 on every model, logged in hex");
    CHECK(ApolloParamDefault(APOLLO_MODEL_X8P, APOLLO_PARAM_BUS_ENABLE) == 0 &&
          ApolloParamDefault(APOLLO_MODEL_TWINX, APOLLO_PARAM_BUS_ENABLE) == 0 &&
          ApolloParamDefault(APOLLO_MODEL_NONE, APOLLO_PARAM_BUS_ENABLE) == 0 &&
          wcscmp(ApolloParamDef(APOLLO_PARAM_BUS_ENABLE)->NameW, L"BusEnable") == 0 &&
          ApolloParamDef(APOLLO_PARAM_BUS_ENABLE)->Hex && APOLLO_PARAM_BUS_ENABLE == 20,
          "BusEnable: id 20, 0 (nothing sent) on every model, logged in hex");
    /* [AuxStrips] docs/STRIPS.md 14: StripAuxMask, the x8p's default one constant: 0x5, the L returns of AUX 1 and AUX 2 (proven Nov 16 2026) */
    CHECK(ApolloParamDefault(APOLLO_MODEL_X8P, APOLLO_PARAM_STRIP_AUX_MASK) == APOLLO_DEF_STRIP_AUX_MASK_X8P &&
          APOLLO_DEF_STRIP_AUX_MASK_X8P == 0x5 &&
          ApolloParamDefault(APOLLO_MODEL_TWINX, APOLLO_PARAM_STRIP_AUX_MASK) == 0 &&
          ApolloParamDefault(APOLLO_MODEL_NONE, APOLLO_PARAM_STRIP_AUX_MASK) == 0 &&
          wcscmp(ApolloParamDef(APOLLO_PARAM_STRIP_AUX_MASK)->NameW, L"StripAuxMask") == 0 &&
          strcmp(ApolloParamDef(APOLLO_PARAM_STRIP_AUX_MASK)->Name, "StripAuxMask") == 0 &&
          ApolloParamDef(APOLLO_PARAM_STRIP_AUX_MASK)->Hex && APOLLO_PARAM_STRIP_AUX_MASK == 21 && APOLLO_PARAM_COUNT == 22,
          "[AuxStrips] StripAuxMask: id 21 (after BusEnable), the x8p's APOLLO_DEF_STRIP_AUX_MASK_X8P, 0 on every other model, "
          "logged in hex");
    CHECK(u.Value[APOLLO_PARAM_STRIP_VIRTUAL_MASK] == 0 && u.Source[APOLLO_PARAM_STRIP_VIRTUAL_MASK] == APOLLO_SRC_DEFAULT,
          "[Virtual] x8p default StripVirtualMask 0 (built-in)");
    CHECK(u.Value[APOLLO_PARAM_STRIP_AUX_MASK] == APOLLO_DEF_STRIP_AUX_MASK_X8P &&
          u.Source[APOLLO_PARAM_STRIP_AUX_MASK] == APOLLO_SRC_DEFAULT,
          "[AuxStrips] x8p default StripAuxMask = APOLLO_DEF_STRIP_AUX_MASK_X8P (built-in)");
    {
        int i, j;

        for (i = 0; i < APOLLO_PARAM_COUNT; i++) {
            CHECK(ApolloParamDef(i)->Name != NULL && ApolloParamDef(i)->NameW != NULL, "param %d has no name", i);
            for (j = 0; j < i; j++) {
                CHECK(_stricmp(ApolloParamDef(i)->Name, ApolloParamDef(j)->Name) != 0, "params %d and %d share a name", j, i);
            }
        }
    }

    /* x8p with today's global values: exactly those, from the globals */
    memset(&r, 0, sizeof(r));
    TodaysGlobals(&r);
    Pick(APOLLO_MODEL_X8P, &r, &u);
    CHECK(u.Active && u.Value[APOLLO_PARAM_MODE] == 2, "x8p today: active");
    CHECK(u.Value[APOLLO_PARAM_STRIPS_ENABLE] == 1 && u.Source[APOLLO_PARAM_STRIPS_ENABLE] == APOLLO_SRC_GLOBAL,
          "x8p today: StripsEnable %lu", u.Value[APOLLO_PARAM_STRIPS_ENABLE]);
    CHECK(u.Value[APOLLO_PARAM_STRIP_MASK] == 3 && u.Value[APOLLO_PARAM_STRIP_STEPS] == 9 &&
          u.Value[APOLLO_PARAM_MIXER_INIT_BATCH] == 1 && u.Value[APOLLO_PARAM_ENABLED_ENDPOINTS] == 0x3FF,
          "x8p today: mask/steps/batch/endpoints");
    CHECK(u.Source[APOLLO_PARAM_STRIP_DSP] == APOLLO_SRC_DEFAULT && u.Value[APOLLO_PARAM_STRIP_DSP] == 1,
          "x8p today: StripDsp default");

    /* A Twin X with the x8p's globals set and no key of its own: observe, nothing inherited */
    Pick(APOLLO_MODEL_TWINX, &r, &u);
    CHECK(!u.Active && u.Value[APOLLO_PARAM_MODE] == 0, "new Twin X: mode %lu active %d",
          u.Value[APOLLO_PARAM_MODE], u.Active);
    /* [Twin X G3] its batch default is 1 now: the Twin X-safe skip list (bringup_plan.h, spec-twinx 4.3) */
    /* [G5] its strip defaults: off, and when on the whole chain on inputs 1-2 (StripMask 0x3) */
    CHECK(u.Value[APOLLO_PARAM_STRIPS_ENABLE] == 0 && u.Value[APOLLO_PARAM_ENABLED_ENDPOINTS] == 0 &&
          u.Value[APOLLO_PARAM_MIXER_INIT_BATCH] == 1 && u.Value[APOLLO_PARAM_STRIP_MASK] == 3 &&
          u.Value[APOLLO_PARAM_STRIP_STEPS] == 0 && u.Value[APOLLO_PARAM_STRIP_DSP] == 1 &&
          u.Value[APOLLO_PARAM_WARM_BOOT_TRANSPORT] == 0,
          "new Twin X: strips %lu endpoints 0x%lX batch %lu mask 0x%lX warm boot %lu",
          u.Value[APOLLO_PARAM_STRIPS_ENABLE], u.Value[APOLLO_PARAM_ENABLED_ENDPOINTS],
          u.Value[APOLLO_PARAM_MIXER_INIT_BATCH], u.Value[APOLLO_PARAM_STRIP_MASK],
          u.Value[APOLLO_PARAM_WARM_BOOT_TRANSPORT]);

    /* [Twin X G3] Mode 1 in its unit key: the G3 bring-up runs (active), strips and endpoints off */
    Set(r.Unit, APOLLO_PARAM_MODE, 1);
    Pick(APOLLO_MODEL_TWINX, &r, &u);
    CHECK(u.Active && u.Value[APOLLO_PARAM_MODE] == 1 && u.Source[APOLLO_PARAM_MODE] == APOLLO_SRC_UNIT,
          "Twin X Mode 1: active %d mode %lu", u.Active, u.Value[APOLLO_PARAM_MODE]);
    CHECK(u.Value[APOLLO_PARAM_STRIPS_ENABLE] == 0 && u.Value[APOLLO_PARAM_ENABLED_ENDPOINTS] == 0 &&
          u.Value[APOLLO_PARAM_MIXER_INIT_BATCH] == 1, "Twin X Mode 1: no strips, no endpoints, batch 1");

    /*
     * [G4] Mode 2 in its key (what step-g4.ps1 writes): runs as Mode 2, its endpoints from the
     * built-in default 0x3 (MON L/R out + in); the x8p's global 0x3FF never reaches it.
     */
    Set(r.Unit, APOLLO_PARAM_MODE, 2);
    Pick(APOLLO_MODEL_TWINX, &r, &u);
    CHECK(u.Active && u.Value[APOLLO_PARAM_MODE] == 2 && u.Source[APOLLO_PARAM_MODE] == APOLLO_SRC_UNIT &&
          u.ModeAsked == 2, "Twin X Mode 2: runs as Mode 2 (mode %lu active %d)", u.Value[APOLLO_PARAM_MODE], u.Active);
    CHECK(u.Value[APOLLO_PARAM_ENABLED_ENDPOINTS] == 0x3F && u.Source[APOLLO_PARAM_ENABLED_ENDPOINTS] == APOLLO_SRC_DEFAULT,
          "Twin X Mode 2: endpoints 0x%lX from the default (not the x8p's 0x3FF)", u.Value[APOLLO_PARAM_ENABLED_ENDPOINTS]);
    CHECK(u.Value[APOLLO_PARAM_STRIPS_ENABLE] == 0 && u.Value[APOLLO_PARAM_MIXER_INIT_BATCH] == 1,
          "Twin X Mode 2: strips 0 (default), batch 1");
    /* Its key narrows the set; strips asked for stay a value (strips.c refuses them by the plan until G5) */
    Set(r.Unit, APOLLO_PARAM_STRIPS_ENABLE, 1);
    Set(r.Unit, APOLLO_PARAM_ENABLED_ENDPOINTS, 1);
    Pick(APOLLO_MODEL_TWINX, &r, &u);
    CHECK(u.Active && u.Value[APOLLO_PARAM_MODE] == 2 && u.Value[APOLLO_PARAM_ENABLED_ENDPOINTS] == 1 &&
          u.Source[APOLLO_PARAM_ENABLED_ENDPOINTS] == APOLLO_SRC_UNIT,
          "Twin X Mode 2, EnabledEndpoints 1 in its key: render only (0x%lX)", u.Value[APOLLO_PARAM_ENABLED_ENDPOINTS]);
    /* [G5] the plan has strips now: StripsEnable in its key decides (strips.c) */
    CHECK(ApolloBringupPlanFor(APOLLO_MODEL_TWINX)->Strips && u.Value[APOLLO_PARAM_STRIPS_ENABLE] == 1 &&
          u.Source[APOLLO_PARAM_STRIPS_ENABLE] == APOLLO_SRC_UNIT, "Twin X Mode 2 (G5): StripsEnable 1 from its key runs");
    /* [G5] what step-g5.ps1 writes for the test: dry run, then the full chain; MIC/LINE 1-2 rows opted in */
    Set(r.Unit, APOLLO_PARAM_STRIPS_ENABLE, 2);
    Set(r.Unit, APOLLO_PARAM_ENABLED_ENDPOINTS, 0xF);
    Pick(APOLLO_MODEL_TWINX, &r, &u);
    CHECK(u.Value[APOLLO_PARAM_STRIPS_ENABLE] == 2 && u.Value[APOLLO_PARAM_ENABLED_ENDPOINTS] == 0xF &&
          u.Value[APOLLO_PARAM_STRIP_MASK] == 3 && u.Value[APOLLO_PARAM_STRIP_STEPS] == 0,
          "Twin X G5 dry run: StripsEnable 2, endpoints 0xF, mask 3 and steps 0 from the defaults");
    Set(r.Unit, APOLLO_PARAM_STRIPS_ENABLE, 1);
    Set(r.Unit, APOLLO_PARAM_ENABLED_ENDPOINTS, 1);
    /* Mode 1 (the rollback): endpoints and strips forced off again */
    Set(r.Unit, APOLLO_PARAM_MODE, 1);
    Pick(APOLLO_MODEL_TWINX, &r, &u);
    CHECK(u.Active && u.Value[APOLLO_PARAM_MODE] == 1 && u.Value[APOLLO_PARAM_STRIPS_ENABLE] == 0 &&
          u.Value[APOLLO_PARAM_ENABLED_ENDPOINTS] == 0 && u.Source[APOLLO_PARAM_STRIPS_ENABLE] == APOLLO_SRC_MODE &&
          u.Source[APOLLO_PARAM_ENABLED_ENDPOINTS] == APOLLO_SRC_MODE,
          "Twin X Mode 1 with Mode 2 values in its key: strips and endpoints forced off");
    Set(r.Unit, APOLLO_PARAM_MODE, 9);
    Pick(APOLLO_MODEL_TWINX, &r, &u);
    CHECK(!u.Active && u.Value[APOLLO_PARAM_MODE] == 0, "Twin X Mode 9 -> observe");

    /* x8p Mode 1 (unit key): bring-up, strips and endpoints off whatever the globals say */
    memset(&r, 0, sizeof(r));
    TodaysGlobals(&r);
    Set(r.Unit, APOLLO_PARAM_MODE, 1);
    Pick(APOLLO_MODEL_X8P, &r, &u);
    CHECK(u.Active && u.Value[APOLLO_PARAM_STRIPS_ENABLE] == 0 &&
          u.Source[APOLLO_PARAM_STRIPS_ENABLE] == APOLLO_SRC_MODE &&
          u.Value[APOLLO_PARAM_ENABLED_ENDPOINTS] == 0, "x8p Mode 1");

    /* x8p Mode 0 and an invalid Mode: observe */
    memset(&r.Unit, 0, sizeof(r.Unit));
    Set(r.Unit, APOLLO_PARAM_MODE, 0);
    Pick(APOLLO_MODEL_X8P, &r, &u);
    CHECK(!u.Active, "x8p Mode 0: active %d", u.Active);
    Set(r.Unit, APOLLO_PARAM_MODE, 7);
    Pick(APOLLO_MODEL_X8P, &r, &u);
    CHECK(!u.Active && u.Value[APOLLO_PARAM_MODE] == 0 && u.Source[APOLLO_PARAM_MODE] == APOLLO_SRC_MODE,
          "x8p Mode 7 -> 0");

    /* Order: unit beats model beats global */
    memset(&r, 0, sizeof(r));
    Set(r.Global, APOLLO_PARAM_STRIP_MASK, 0xFF);
    Set(r.Model, APOLLO_PARAM_STRIP_MASK, 0x0F);
    Pick(APOLLO_MODEL_X8P, &r, &u);
    CHECK(u.Value[APOLLO_PARAM_STRIP_MASK] == 0x0F && u.Source[APOLLO_PARAM_STRIP_MASK] == APOLLO_SRC_MODEL,
          "model beats global: 0x%lX", u.Value[APOLLO_PARAM_STRIP_MASK]);
    Set(r.Unit, APOLLO_PARAM_STRIP_MASK, 0x01);
    Pick(APOLLO_MODEL_X8P, &r, &u);
    CHECK(u.Value[APOLLO_PARAM_STRIP_MASK] == 0x01 && u.Source[APOLLO_PARAM_STRIP_MASK] == APOLLO_SRC_UNIT,
          "unit beats model: 0x%lX", u.Value[APOLLO_PARAM_STRIP_MASK]);

    /* A refused model is never active */
    memset(&r, 0, sizeof(r));
    Set(r.Unit, APOLLO_PARAM_MODE, 2);
    Pick(APOLLO_MODEL_NONE, &r, &u);
    CHECK(!u.Active, "refused model active");
}

/* ------------------------------------------------------------------ */
/* 5. [Twin X G2] Device add: hardware ID, circuits, log tag           */
/* ------------------------------------------------------------------ */

static void TestAddTime(void)
{
    /* REG_MULTI_SZ as PnP returns it: the most specific ID first. */
    static const wchar_t twinx[] = L"PCI\\VEN_1A00&DEV_0002&SUBSYS_00191A00&REV_00\0"
                                   L"PCI\\VEN_1A00&DEV_0002&SUBSYS_00191A00\0PCI\\VEN_1A00&DEV_0002&CC_040100\0";
    static const wchar_t x8p[] = L"PCI\\VEN_1A00&DEV_0002&SUBSYS_00141A00&REV_00\0";
    static const wchar_t lower[] = L"pci\\ven_1a00&dev_0002&SUBSYS_00191a00\0";
    static const wchar_t shortHex[] = L"PCI\\VEN_1A00&DEV_0002&SUBSYS_0019\0";
    static const wchar_t noSub[] = L"PCI\\VEN_1A00&DEV_0002&CC_040100\0";
    unsigned long sub = 0xFFFF;
    char tag[8];

    CHECK(ApolloHwidSubsystem(twinx, (unsigned long)(sizeof(twinx) / sizeof(wchar_t)), &sub) && sub == 0x0019,
          "hwid Twin X: 0x%04lX", sub);
    CHECK(ApolloHwidSubsystem(x8p, (unsigned long)(sizeof(x8p) / sizeof(wchar_t)), &sub) && sub == 0x0014,
          "hwid x8p: 0x%04lX", sub);
    CHECK(ApolloHwidSubsystem(lower, (unsigned long)(sizeof(lower) / sizeof(wchar_t)), &sub) && sub == 0x0019,
          "hwid lower-case hex: 0x%04lX", sub);
    CHECK(!ApolloHwidSubsystem(shortHex, (unsigned long)(sizeof(shortHex) / sizeof(wchar_t)), &sub) && sub == 0,
          "hwid with 4 hex digits must not parse");
    CHECK(!ApolloHwidSubsystem(noSub, (unsigned long)(sizeof(noSub) / sizeof(wchar_t)), &sub), "hwid without SUBSYS");
    CHECK(!ApolloHwidSubsystem(NULL, 10, &sub), "hwid NULL");
    /* The count bounds the scan: the SUBSYS field cut off by it is not read. */
    CHECK(!ApolloHwidSubsystem(x8p, 30, &sub), "hwid bounded by the count");

    /*
     * Circuits at device add: the x8p's slot keeps today's set. [G4] The Twin X's slot gets the
     * objects of its own two-row table (added at PrepareHardware in Mode 2 only), never the x8p's
     * global value.
     */
    /* [Endpoints] the x8p like the Twin X: every row's circuit object; its unit value picks the ones added */
    CHECK(ApolloAddTimeEndpoints(1, 0x0014, 0, 0) == 0x3FFFFF, "x8p, no global: all 22 rows");
    CHECK(ApolloAddTimeEndpoints(1, 0x0014, 1, 0x3) == 0x3FFFFF, "x8p, global 3: still all rows (added by the value)");
    CHECK(ApolloAddTimeEndpoints(1, 0x0014, 1, 0x3FF) == 0x3FFFFF, "x8p, an old global 0x3FF: all rows");
    /* [G5] every circuit object of its table (9 rows since [Endpoints]); its unit value adds them */
    CHECK(ApolloAddTimeEndpoints(1, 0x0019, 1, 0x3FF) == 0x1FF, "Twin X with the x8p's global 0x3FF: its own 0x1FF");
    CHECK(ApolloAddTimeEndpoints(1, 0x0019, 0, 0) == 0x1FF, "Twin X, no global: its own 0x1FF");
    CHECK(ApolloAddTimeEndpoints(1, 0x0019, 1, 0) == 0x1FF, "Twin X, x8p global 0: its own 0x1FF");
    CHECK(ApolloAddTimeEndpoints(1, 0x000F, 1, 0x3FF) == 0, "another subsystem: none");
    CHECK(ApolloAddTimeEndpoints(0, 0, 1, 0x3FF) == 0x3FFFFF, "hardware ID unread: the x8p's rows");
    CHECK(ApolloAddTimeModel(1, 0x0014) == APOLLO_MODEL_X8P && ApolloAddTimeModel(0, 0) == APOLLO_MODEL_X8P &&
          ApolloAddTimeModel(1, 0x0019) == APOLLO_MODEL_TWINX && ApolloAddTimeModel(1, 0x000F) == APOLLO_MODEL_NONE,
          "add-time endpoint table: x8p (and unread), Twin X, none");
    CHECK(ApolloAddTimeX8pRules(1, 0x0014) && !ApolloAddTimeX8pRules(1, 0x0019) && ApolloAddTimeX8pRules(0, 0),
          "add-time x8p rules");

    ApolloLogTagFor(1, 0x0014, tag);
    CHECK(strcmp(tag, "x8p") == 0, "tag x8p: %s", tag);
    ApolloLogTagFor(1, 0x0019, tag);
    CHECK(strcmp(tag, "twx") == 0, "tag twx: %s", tag);
    ApolloLogTagFor(1, 0x00A5, tag);
    CHECK(strcmp(tag, "s00a5") == 0, "tag other: %s", tag);
    ApolloLogTagFor(0, 0, tag);
    CHECK(strcmp(tag, "unit") == 0, "tag unknown: %s", tag);
}

int main(void)
{
    printf("identification cases:\n");
    TestUpstream();
    TestCases();
    TestSerialAndKey();
    TestParams();
    TestAddTime();
    printf("ident-test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
