/*
 * apollo_model.h - which Apollo is this, and does this driver run it (docs/spec-twinx.md 1).
 *
 * Plain C, no kernel or Windows headers: driver\hw.c, tools\apollo-diag and tools\ident-test
 * compile the same code. Every function is static __inline and every table lives inside its
 * function, so a file that uses only part of this gets no unused-code warnings.
 *
 * Identification is open-apollo's, step for step (oa:driver/ua_core.c:349-680):
 * ua_detect_capabilities (subsystem pins, then the serial table on digits 5-8),
 * ua_compute_public_type (the kext's UAD2DeviceType: ext_caps family, refined by the serial
 * type and the FPGA_REV type) and ua_resolve_public_model (serials 2024/2025 on the Twin X
 * family are a Twin X, the Conrad family is named by its public type, and a unit no serial
 * names gets 0x80 | public type). Then a model table decides what this driver runs:
 *
 *   Apollo x8p          type 0x22, family 17, public 0x16, 6 DSPs
 *   Apollo Twin X Gen 1 type 0x99, family 19, public 0x19, 2 or 4 DSPs, ext_caps bit 31,
 *                       v2 firmware, serial digits 5-8 readable (else a Gen 2 cannot be ruled out)
 *   anything else       refused: the driver stays started and idle so apollo-diag reports it
 *
 * Firmware/FPGA pairs are flagged, never refused: ApolloFwTested() holds the tested rows.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

/* Device types (internal numbering, oa:driver/ua_apollo.h:84-128) */
#define APOLLO_DEVTYPE_X8P          0x22u
#define APOLLO_DEVTYPE_TWINX        0x99u   /* 0x80 | public 0x19: Twin X Gen 1 */
#define APOLLO_PUBLIC_BASE          0x80u   /* device type of a unit the serial table cannot name */
#define APOLLO_PUBTYPE_NONE         0xFFFFFFFFu     /* GetCapabilities: -1 */

/* The x8p firmware this driver was tested on (Aaron's unit, Nov 14 2026). hw.c asserts these
   equal APOLLO_TESTED_FW_RELEASE / APOLLO_TESTED_FPGA_REV in apollo_ioctl.h. */
#define APOLLO_X8P_TESTED_FW        0x0B0FD613u     /* 11.15.54803 */
#define APOLLO_X8P_TESTED_FPGA      0xA241C5ACu

/* Models this driver accepts (APOLLO_IDENT.Model) */
#define APOLLO_MODEL_NONE           0
#define APOLLO_MODEL_X8P            1
#define APOLLO_MODEL_TWINX          2

/* APOLLO_IDENT.Verdict */
#define APOLLO_IDENT_ACCEPTED       0   /* a model row matched */
#define APOLLO_IDENT_REFUSED        1   /* identified, not a model this driver runs: stay idle */
#define APOLLO_IDENT_NOT_APOLLO     2   /* v1 firmware or more than 8 DSPs: stop at identify */
#define APOLLO_IDENT_OFFLINE        3   /* FPGA_REV or ext_caps read all ones */

/* APOLLO_IDENT.UnitKeyKind: the name of the unit's registry key (unit.c, spec-twinx 6.9) */
#define APOLLO_UNIT_KEY_NONE        0   /* no stable identity: no per-unit key */
#define APOLLO_UNIT_KEY_HWUID       1   /* 16 hex digits: HW_UID high word, then low word */
#define APOLLO_UNIT_KEY_SERIAL      2   /* "SN" + serial, when the HW_UID reads 0 or all ones */
#define APOLLO_UNIT_KEY_CHARS       20  /* "SN" + 16 characters + NUL */

typedef struct _APOLLO_IDENT_IN {
    unsigned long   FpgaRev;        /* BAR0 0x2218 */
    unsigned long   ExtCaps;        /* BAR0 0x2234, read only when FpgaRev bit 31 is set */
    unsigned long   SubsystemId;    /* PCI config 0x2E */
    unsigned long   FwRelease;      /* BAR0 0x0008 */
    unsigned long   HwUidLo;        /* BAR0 0x0030 */
    unsigned long   HwUidHi;        /* BAR0 0x0034 */
    unsigned char   SerialRaw[16];  /* BAR0 0x0020-0x002F as read */
} APOLLO_IDENT_IN;

typedef struct _APOLLO_IDENT {
    int             Verdict;        /* APOLLO_IDENT_* */
    int             Model;          /* APOLLO_MODEL_*, NONE unless accepted */
    unsigned long   DeviceType;     /* resolved internal type, 0 = unidentified */
    unsigned long   PublicType;     /* kext UAD2DeviceType; APOLLO_PUBTYPE_NONE = -1 */
    unsigned long   SerialType;     /* serial-table type of digits 5-8, 0 = no entry */
    unsigned long   Family;         /* ext_caps [25:20] */
    unsigned long   NumDsps;        /* ext_caps [15:8] */
    int             FwV2;           /* FPGA_REV bit 31 */
    int             Irq64;          /* ext_caps bit 31, not the Rack family (6) */
    int             FwUntested;     /* FW_RELEASE / FPGA_REV are not a tested pair for Model */
    int             SerialDigitsOk; /* serial digits 5-8 are four ASCII digits */
    unsigned long   SerialLen;      /* characters in Serial */
    char            Serial[17];     /* the printable part of the serial, NUL terminated */
    int             UnitKeyKind;    /* APOLLO_UNIT_KEY_* */
    char            UnitKey[APOLLO_UNIT_KEY_CHARS];
    const char     *Name;           /* model name of DeviceType */
    const char     *Why;            /* "" when accepted, else why not */
} APOLLO_IDENT;

/* ------------------------------------------------------------------ */
/* Serial (BAR0 0x20-0x2F)                                             */
/* ------------------------------------------------------------------ */

static __inline int ApolloSerialAlnum(unsigned char C)
{
    return (C >= '0' && C <= '9') || (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z');
}

/*
 * The valid part of the serial. The x8p's 16 bytes hold 14 digits and then two bytes that are
 * not characters (the driver used to show them as "22532017106180??"). The serial is the text
 * up to the first byte that is not printable ASCII (NUL, control, 0x7F and up, space), without
 * trailing punctuation (so a literal '?' fill goes too). Out gets 17 bytes, NUL padded.
 */
static __inline unsigned long ApolloSerialClean(const unsigned char *Raw, char *Out)
{
    unsigned long n = 0, i;

    while (n < 16 && Raw[n] > 0x20 && Raw[n] < 0x7F) {
        n++;
    }
    while (n > 0 && !ApolloSerialAlnum(Raw[n - 1])) {
        n--;
    }
    for (i = 0; i < n; i++) {
        Out[i] = (char)Raw[i];
    }
    for (; i < 17; i++) {
        Out[i] = 0;
    }
    return n;
}

/* Digits 5-8 (string offset 4) are what the serial table matches: four ASCII digits or nothing. */
static __inline int ApolloSerialDigitsOk(const unsigned char *Raw)
{
    int i;

    for (i = 4; i < 8; i++) {
        if (Raw[i] < '0' || Raw[i] > '9') {
            return 0;
        }
    }
    return 1;
}

/* _deviceTypeFromSerialNumber (kext 12.0.0, 22 entries, oa:driver/ua_core.c:473-496) */
static __inline unsigned long ApolloSerialType(const unsigned char *Raw)
{
    static const struct {
        char            Digits[5];
        unsigned long   Type;
    } t[] = {
        { "2005", 0x1F }, { "2019", 0x20 }, { "2018", 0x21 }, { "2017", 0x22 },
        { "2020", 0x23 }, { "2024", 0x27 }, { "2025", 0x28 }, { "2073", 0x35 },
        { "2082", 0x35 }, { "2016", 0x1E }, { "2032", 0x2A }, { "2092", 0x36 },
        { "2086", 0x37 }, { "2087", 0x38 }, { "2088", 0x39 }, { "2089", 0x3A },
        { "2090", 0x3B }, { "2091", 0x3C }, { "2107", 0x40 }, { "2108", 0x41 },
        { "2109", 0x3F }, { "2110", 0x3E },
    };
    unsigned int i;

    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        if (Raw[4] == (unsigned char)t[i].Digits[0] && Raw[5] == (unsigned char)t[i].Digits[1] &&
            Raw[6] == (unsigned char)t[i].Digits[2] && Raw[7] == (unsigned char)t[i].Digits[3]) {
            return t[i].Type;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Types (oa:driver/ua_core.c:435-680)                                 */
/* ------------------------------------------------------------------ */

/* CUAD2Device::_deviceTypeFromFPGAVersion: a new-format FPGA_REV carries a serial-table type. */
static __inline unsigned long ApolloTypeFromFpgaRev(unsigned long Rev)
{
    return ((Rev & 0x60u) == 0x20u) ? ((Rev >> 24) & 0x7Fu) : (Rev >> 28);
}

/* ua_detect_capabilities, v2 firmware: subsystem pins first, then the serial, 0x0014 as fallback. */
static __inline unsigned long ApolloDeviceTypeOf(unsigned long SubsystemId, unsigned long SerialType)
{
    if (SubsystemId == 0x0006) {
        return 0x0A;                    /* Apollo 8p */
    }
    if (SubsystemId == 0x000F) {
        return 0x27;                    /* Apollo Solo */
    }
    if (SerialType == 0 && SubsystemId == 0x0014) {
        return APOLLO_DEVTYPE_X8P;      /* serial unresolved: the x8p's subsystem */
    }
    return SerialType;
}

/* ua_compute_public_type: the kext's UAD2DeviceType (0 for v1 firmware). */
static __inline unsigned long ApolloPublicType(int FwV2, unsigned long ExtCaps, unsigned long FpgaRev,
                                               unsigned long SerialType)
{
    static const unsigned char familyMap[22] = {
        0x05, 0x04, 0x00, 0x07, 0x08, 0x0B, 0x04, 0x0C, 0x0D, 0x0F, 0x0E,
        0x00, 0x00, 0x12, 0x13, 0x1A, 0x16, 0x18, 0x19, 0x00, 0x00, 0x20,
    };
    unsigned long fam, base, st = SerialType, ft;

    if (!FwV2) {
        return 0;
    }
    fam = (ExtCaps >> 20) & 0x3Fu;
    if (fam == 0x3F) {
        return APOLLO_PUBTYPE_NONE;
    }
    if (fam == 0 || fam > 22) {
        return 0;
    }
    base = familyMap[fam - 1];
    if (base != 0x13 && base != 0x16 && base != 0x19 && base != 0x1A) {
        return base;
    }
    ft = ApolloTypeFromFpgaRev(FpgaRev);

    switch (base) {
    case 0x13:                          /* Conrad: Arrow / Solo */
        if (st == 0x1E) {
            return 0x13;
        }
        if (st == 0x3E) {
            return 0x27;
        }
        return 0x1C;                    /* serial 2032 or any other */
    case 0x1A:                          /* QUAD: x4 */
        if (st != 0x1F && st != 0x36 && st != 0x3F) {
            st = ft;
        }
        if (st == 0x1F) {
            return 0x1A;
        }
        if (st == 0x36) {
            return 0x26;
        }
        if (st == 0x3F) {
            return 0x28;
        }
        return APOLLO_PUBTYPE_NONE;
    case 0x16:                          /* HEXA: x6/x8/x8p/x16 */
        if (st >= 0x20 && st <= 0x23) {
            return 0x14 + (st - 0x20);
        }
        if (st == 0x35) {
            return 0x21;                /* the kext's fall-through */
        }
        if (st >= 0x37 && st <= 0x3A) {
            return 0x21 + (st - 0x37);
        }
        return (ft == 0x35) ? 0x20 : 0x16;
    default:                            /* 0x19: Twin X family */
        if (st == 0x27 || st == 0x28 || st == 0x26 || st == 0x2E || st == 0x2F) {
            return 0x19;
        }
        if (st == 0x3B || st == 0x3C || st == 0x3D) {
            return 0x25;
        }
        if (st == 0x40 || st == 0x41) {
            return 0x29;
        }
        return 0x19;
    }
}

/* ua_resolve_public_model: the device type once the public type is known. */
static __inline unsigned long ApolloResolveDeviceType(unsigned long PublicType, unsigned long DeviceType)
{
    unsigned long pt = PublicType, dt = DeviceType;

    if (pt == 0 || pt == APOLLO_PUBTYPE_NONE || pt >= APOLLO_PUBLIC_BASE) {
        return dt;
    }
    if (pt == 0x19 && (dt == 0x27 || dt == 0x28)) {
        return APOLLO_PUBLIC_BASE | pt;     /* serial 2024/2025 on the Twin X family: Twin X */
    }
    if ((pt == 0x13 || pt == 0x1C || pt == 0x27) &&
        (dt == 0x1E || dt == 0x2A || dt == 0x3E || (pt == 0x13 && dt == 0x27))) {
        return APOLLO_PUBLIC_BASE | pt;     /* Conrad family: Arrow (0x13) or Solo (0x1C/0x27) */
    }
    if (dt == 0) {
        return APOLLO_PUBLIC_BASE | pt;     /* no serial-table type */
    }
    return dt;
}

/* CPcieDevice::Name() for the types open-apollo names (oa:driver/ua_models_gen.h:1128-1161). */
static __inline const char *ApolloModelName(unsigned long DeviceType)
{
    switch (DeviceType) {
    case 0x00: return "unidentified";
    case 0x0A: return "Apollo 8p";
    case 0x1E: case 0x2A: case 0x3E: return "Apollo (unidentified HEXA)";
    case 0x1F: return "Apollo x4";
    case 0x20: return "Apollo x6";
    case 0x21: return "Apollo x8";
    case 0x22: return "Apollo x8p";
    case 0x23: return "Apollo x16";
    case 0x27: return "Apollo Solo";
    case 0x28: return "Arrow";
    case 0x35: return "Apollo x16D";
    case 0x36: return "Apollo x4 Gen 2";
    case 0x37: return "Apollo x6 Gen 2";
    case 0x38: return "Apollo x8 Gen 2";
    case 0x39: return "Apollo x8p Gen 2";
    case 0x3A: return "Apollo x16 Gen 2";
    case 0x3B: case 0x3C: return "Apollo Twin X Gen 2";
    case 0x3F: return "Apollo x4 Gen 2 TB4";
    case 0x40: case 0x41: return "Apollo Twin X Gen 2 TB4";
    case 0x85: return "Apollo Thunderbolt";
    case 0x87: return "Apollo 16";
    case 0x88: case 0x8F: return "Apollo Twin";
    case 0x8B: return "Realtime Rack";
    case 0x8C: return "Apollo 8";
    case 0x8D: return "Apollo 16 MkII";
    case 0x8E: return "Apollo 8p";
    case 0x92: return "Apollo Twin MkII";
    case 0x93: return "Arrow";
    case 0x99: return "Apollo Twin X (Gen 1)";
    case 0x9C: case 0xA7: return "Apollo Solo";
    default:   return "unknown UA device";
    }
}

/* ------------------------------------------------------------------ */
/* Tested firmware and the unit key                                    */
/* ------------------------------------------------------------------ */

/* Flag, never refuse (spec-twinx 1.3). Freeze rule: no UA firmware updates on either unit. */
static __inline int ApolloFwTested(int Model, unsigned long FwRelease, unsigned long FpgaRev)
{
    static const struct {
        int             Model;
        unsigned long   FwRelease;
        unsigned long   FpgaRev;
    } t[] = {
        { APOLLO_MODEL_X8P, APOLLO_X8P_TESTED_FW, APOLLO_X8P_TESTED_FPGA },     /* Aaron's x8p */
        /* Apollo Twin X Gen 1: add its row only after gate G3 passes on Aaron's unit. */
    };
    unsigned int i;

    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        if (t[i].Model == Model && t[i].FwRelease == FwRelease && t[i].FpgaRev == FpgaRev) {
            return 1;
        }
    }
    return 0;
}

/*
 * The unit's registry key name under Parameters\Units (spec-twinx 1.3, 6.9): the HW_UID, which
 * stays with the unit, as 16 upper-case hex digits (high word first). A HW_UID of 0 or all ones
 * says nothing: then "SN" + the serial, if it has 8 or more characters, all letters or digits.
 * The PnP instance path is never used (it encodes the Thunderbolt port).
 */
static __inline int ApolloUnitKeyName(unsigned long UidHi, unsigned long UidLo, const char *Serial,
                                      unsigned long SerialLen, char *Out)
{
    static const char hex[] = "0123456789ABCDEF";
    unsigned long i;

    for (i = 0; i < APOLLO_UNIT_KEY_CHARS; i++) {
        Out[i] = 0;
    }
    if (!((UidHi == 0 && UidLo == 0) || (UidHi == 0xFFFFFFFFu && UidLo == 0xFFFFFFFFu))) {
        for (i = 0; i < 8; i++) {
            Out[i] = hex[(UidHi >> (28 - 4 * i)) & 0xF];
            Out[8 + i] = hex[(UidLo >> (28 - 4 * i)) & 0xF];
        }
        return APOLLO_UNIT_KEY_HWUID;
    }
    if (SerialLen < 8 || SerialLen > 16) {
        return APOLLO_UNIT_KEY_NONE;
    }
    for (i = 0; i < SerialLen; i++) {
        if (!ApolloSerialAlnum((unsigned char)Serial[i])) {
            return APOLLO_UNIT_KEY_NONE;
        }
    }
    Out[0] = 'S';
    Out[1] = 'N';
    for (i = 0; i < SerialLen; i++) {
        Out[2 + i] = Serial[i];
    }
    return APOLLO_UNIT_KEY_SERIAL;
}

/* ------------------------------------------------------------------ */
/* The whole identification                                            */
/* ------------------------------------------------------------------ */

static __inline void ApolloIdentResolve(const APOLLO_IDENT_IN *In, APOLLO_IDENT *Out)
{
    unsigned long i;
    unsigned char *p = (unsigned char *)Out;

    for (i = 0; i < sizeof(*Out); i++) {
        p[i] = 0;
    }
    Out->Name = ApolloModelName(0);
    Out->Why = "";
    Out->FwUntested = 1;
    Out->SerialLen = ApolloSerialClean(In->SerialRaw, Out->Serial);
    Out->SerialDigitsOk = ApolloSerialDigitsOk(In->SerialRaw);
    Out->UnitKeyKind = ApolloUnitKeyName(In->HwUidHi, In->HwUidLo, Out->Serial, Out->SerialLen,
                                         Out->UnitKey);

    if (In->FpgaRev == 0xFFFFFFFFu) {
        Out->Verdict = APOLLO_IDENT_OFFLINE;
        Out->Why = "FPGA_REV reads all ones: device offline";
        return;
    }
    if ((In->FpgaRev & 0x80000000u) == 0) {
        Out->Verdict = APOLLO_IDENT_NOT_APOLLO;
        Out->Why = "v1 firmware (FPGA_REV bit 31 clear) is not supported by this driver";
        return;
    }
    Out->FwV2 = 1;
    if (In->ExtCaps == 0xFFFFFFFFu) {
        Out->Verdict = APOLLO_IDENT_OFFLINE;
        Out->Why = "ext_caps reads all ones: device offline";
        return;
    }
    Out->Family = (In->ExtCaps >> 20) & 0x3Fu;
    Out->NumDsps = (In->ExtCaps >> 8) & 0xFFu;
    Out->Irq64 = (In->ExtCaps & 0x80000000u) != 0 && Out->Family != 6;
    if (Out->NumDsps > 8) {
        Out->Verdict = APOLLO_IDENT_NOT_APOLLO;
        Out->Why = "ext_caps reports more than 8 DSPs: not an Apollo";
        return;
    }

    Out->SerialType = ApolloSerialType(In->SerialRaw);
    Out->DeviceType = ApolloDeviceTypeOf(In->SubsystemId, Out->SerialType);
    Out->PublicType = ApolloPublicType(1, In->ExtCaps, In->FpgaRev, Out->SerialType);
    Out->DeviceType = ApolloResolveDeviceType(Out->PublicType, Out->DeviceType);
    Out->Name = ApolloModelName(Out->DeviceType);
    Out->Verdict = APOLLO_IDENT_REFUSED;

    if (Out->DeviceType == APOLLO_DEVTYPE_X8P) {
        if (Out->Family != 17 || Out->PublicType != 0x16 || Out->NumDsps != 6) {
            Out->Why = "x8p serial or subsystem, but ext_caps family, public type or DSP count "
                       "is not an x8p's (17, 0x16, 6)";
        } else {
            Out->Verdict = APOLLO_IDENT_ACCEPTED;
            Out->Model = APOLLO_MODEL_X8P;
        }
    } else if (Out->DeviceType == APOLLO_DEVTYPE_TWINX) {
        if (Out->Family != 19 || Out->PublicType != 0x19) {
            Out->Why = "Twin X type, but not the Twin X family (19, public 0x19)";
        } else if ((In->ExtCaps & 0x80000000u) == 0) {
            Out->Why = "Twin X without ext_caps bit 31 (no audio extension)";
        } else if (Out->NumDsps != 2 && Out->NumDsps != 4) {
            Out->Why = "Twin X with a DSP count other than 2 (Duo) or 4 (Quad)";
        } else if (!Out->SerialDigitsOk) {
            Out->Why = "Twin X family, but serial digits 5-8 are not digits: a Gen 2 cannot be ruled out";
        } else {
            Out->Verdict = APOLLO_IDENT_ACCEPTED;
            Out->Model = APOLLO_MODEL_TWINX;
        }
    } else if (Out->DeviceType == 0x3B || Out->DeviceType == 0x3C ||
               Out->DeviceType == 0x40 || Out->DeviceType == 0x41) {
        Out->Why = "Apollo Twin X Gen 2 is not supported: this driver runs the Twin X Gen 1 (0x99)";
    } else if (Out->DeviceType == 0) {
        Out->Why = "model unidentified (no serial-table type, no public type)";
    } else {
        Out->Why = "this driver runs only the Apollo x8p (0x22) and the Apollo Twin X Gen 1 (0x99)";
    }

    Out->FwUntested = !ApolloFwTested(Out->Model, In->FwRelease, In->FpgaRev);
}
