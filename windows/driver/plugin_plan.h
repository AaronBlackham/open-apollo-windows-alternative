/*
 * plugin_plan.h - the pure parts of UAD plug-in hosting, phase P0 (docs/PLUGINS.md 2.1): the
 * transaction layout with 4 KiB pages and long replies, the DSP pool ledger, module record
 * checks, the request validation (rows, WRITEs, chain directory, CREATE flags, mono or stereo
 * inputs), the licence decode
 * and date, staged startup paging (a startup over one 1016-word page), and the words of every
 * plug-in transaction (upload, privates, start, stage, teardown, read), and what hosting is per
 * model (PLUG_MODEL: the x8p and, since P7, the Twin X Gen 1 Duo). No OS
 * calls and no allocation: apollo.sys (plugin.c), tools\plugin-plan-test and tools\plugin-host
 * (apollo-plug check) build the same code. The includer defines ULONG, UCHAR and ULONGLONG.
 *
 * Ported from open-apollo (GPL-2.0): ua_dsp_xact.c:122-177 (paging), ua_resource_pool.c (ledger),
 * ua_plugin_catalog.c:42-74 (Bill check), ua_plugin_upload.c:24-170 (upload, memspec, release),
 * ua_plugin_instance.c:393-750 (checks, chain directory, start words), ua_strips.c:1101-1132
 * (restore), ua_plugin_license.c:40-96 (date, decode).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

/* ------------------------------------------------------------------ */
/* Errors (the driver maps them to NTSTATUS)                           */
/* ------------------------------------------------------------------ */

#define PLUG_OK                 0u
#define PLUG_EINVAL             1u
#define PLUG_E2BIG              2u
#define PLUG_ENOSPC             3u
#define PLUG_EEXIST             4u
#define PLUG_EACCES             5u
#define PLUG_ENOTSUP            6u
#define PLUG_ERANGE             7u

#define PLUG_NONE               0xFFFFFFFFu

/* ------------------------------------------------------------------ */
/* Device words                                                        */
/* ------------------------------------------------------------------ */

#define PLUG_RING_DMA_REF       0x80000000u
#define PLUG_CMD_UPLOAD_CODE    0x00010000u     /* | (record dwords + 2), by DMA */
#define PLUG_CMD_UPLOAD_DATA    0x00040000u
#define PLUG_REPLY_UPLOAD       0x80070004u     /* {.., 0, id, x} */
#define PLUG_CMD_ZERO           0x00080004u     /* {cmd, addr, 0, dwords} */
#define PLUG_CMD_MEMSPEC        0x00150000u     /* | (2n - 1), by DMA */
#define PLUG_CMD_WRITE          0x0014u         /* opcode half of a parameter WRITE */
#define PLUG_CMD_RELEASE_CODE   0x00030002u     /* {cmd, id, 0, 0} */
#define PLUG_CMD_RELEASE_DATA   0x00060003u     /* {cmd, id, kind (4 -> 1), 0} */
#define PLUG_CMD_SYNCH          0x001F0004u     /* {cmd, in2 << 16 | in1, head (0 = stop), 0} */
#define PLUG_CMD_INPUT          0x001E0004u     /* {cmd, input, param, value} */
#define PLUG_CMD_READ           0x000C0004u     /* {cmd, addr, n, word}: reply {0x8001nnnn, word, values} */
#define PLUG_REPLY_READ         0x80010000u
#define PLUG_INPUT_ROUTE        7u
#define PLUG_INPUT_SPAN         8u
#define PLUG_SPAN_NONE          0x20u
#define PLUG_ROUTE_NONE         0x0001FFFFu
#define PLUG_SYNCH_MONO         0x20u
#define PLUG_MIC_CARRIER        0x846u
#define PLUG_INPUT_CARRIER      0x83Eu

/* The board query (ua_plugin_host.h:368-376) */
#define PLUG_BQ_CMD_DATE        0x00100002u
#define PLUG_BQ_CMD_QUERY       0x00110002u
#define PLUG_BQ_REPLY           0x80030302u
#define PLUG_BQ_WORDS           768u
#define PLUG_BQ_RX_WORDS        (PLUG_BQ_WORDS + 2u)
#define PLUG_BQ_SENTINEL        0x83000000u
#define PLUG_BQ_BASE            0x800u
#define PLUG_FILETIME_2007      128120832000000000ULL   /* 2007-01-01 00:00 UTC in FILETIME units */

/* ------------------------------------------------------------------ */
/* Limits                                                              */
/* ------------------------------------------------------------------ */

#define PLUG_PAGE               4096u
#define PLUG_PAY_PAGES_MAX      64u             /* 256 KiB of DMA payload per transaction */
#define PLUG_XACT_ENTS_MAX      128u            /* command-ring entries (a quarter ring) */
#define PLUG_XACT_RX_MAX        8u              /* reply descriptors */
#define PLUG_XACT_RX_WORDS_MAX  1024u           /* dwords of one reply */
#define PLUG_RX_PAGES_MAX       8u
#define PLUG_RES_MAX            64u
/* 127, not upstream's 128: the privates transaction is one zero-fill per row plus the memspec, and
   PLUG_XACT_ENTS_MAX is 128 entries (a 128-row CREATE could never be sent: review P0 finding 5). */
#define PLUG_PRIV_MAX           127u
#define PLUG_CMD_WORDS_MAX      1016u
#define PLUG_READ_WORDS_MAX     64u
#define PLUG_BILL_HDR           20u
#define PLUG_BILL_MAGIC         0x6C6C6942u     /* "Bill" */
#define PLUG_BILL_MAX           ((256u << 10) - 8u)
#define PLUG_POOLS              4u
#define PLUG_EXTENTS            160u            /* per pool: free and used extents together */

/* ------------------------------------------------------------------ */
/* Little helpers                                                      */
/* ------------------------------------------------------------------ */

static __inline ULONG PlugLe32(const UCHAR *P)
{
    return (ULONG)P[0] | ((ULONG)P[1] << 8) | ((ULONG)P[2] << 16) | ((ULONG)P[3] << 24);
}

static __inline ULONG PlugPopcount(ULONG V)
{
    ULONG n = 0;

    while (V != 0) {
        V &= V - 1;
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Module records (ua_plugin_catalog.c:42-74)                          */
/* ------------------------------------------------------------------ */

typedef struct _PLUG_BILL {
    ULONG   Id;             /* 24 bits on disk */
    ULONG   W2;             /* kind 7:0, flags 23:8, architecture 31:24 */
    ULONG   Kind, Flags, Arch;
    ULONG   W4;
    ULONG   Words;          /* pool words: floor(2 * w4 / 3) for code, else w4 */
    ULONG   Pool;           /* 0 code, 1 data (kinds 1 and 4), 2, 3 */
} PLUG_BILL;

/* Is Data a record UAD2System would register? PLUG_OK or PLUG_EINVAL. */
static __inline ULONG PlugBillCheck(const UCHAR *Data, ULONG Len, PLUG_BILL *Out)
{
    PLUG_BILL b;
    ULONG payload;

    if (Data == 0 || Len < 13 * 4 + 4 || Len > PLUG_BILL_MAX || (Len & 3) != 0) {
        return PLUG_EINVAL;
    }
    if (PlugLe32(Data) != PLUG_BILL_MAGIC) {
        return PLUG_EINVAL;
    }
    b.Id = PlugLe32(Data + 4);
    b.W2 = PlugLe32(Data + 8);
    payload = PlugLe32(Data + 12);
    b.W4 = PlugLe32(Data + 16);
    b.Kind = b.W2 & 0xFFu;
    b.Flags = (b.W2 >> 8) & 0xFFFFu;
    b.Arch = b.W2 >> 24;
    if (b.Id > 0xFFFFFFu || b.Kind > 4 || b.Arch > 2 || (b.Kind == 0 && b.Flags != 0) ||
        payload != Len - PLUG_BILL_HDR || b.W4 == 0 || b.W4 > Len / 4 - 13) {
        return PLUG_EINVAL;
    }
    if (b.Kind == 0) {
        b.Words = (ULONG)(((ULONGLONG)b.W4 * 2) / 3);
        if (b.Words == 0 || (b.Words & 1) != 0) {
            return PLUG_EINVAL;
        }
        b.Pool = 0;
    } else {
        b.Words = b.W4;
        b.Pool = (b.Kind == 4) ? 1 : b.Kind;
    }
    if (Out != 0) {
        *Out = b;
    }
    return PLUG_OK;
}

/* The set one CREATE uploads (ua_plugin_instance.c:578-604): kinds, architectures, duplicates. */
static __inline ULONG PlugCheckSet(const PLUG_BILL *B, ULONG N)
{
    ULONG i, j;

    for (i = 0; i < N; i++) {
        if (B[i].Kind > 4 || (B[i].Kind == 3 && B[i].Flags != 0)) {
            return PLUG_ENOTSUP;
        }
        if (B[i].Arch == 1) {
            return PLUG_EINVAL;             /* the Apollo's DSPs take architectures 0 and 2 */
        }
        for (j = 0; j < i; j++) {
            if (B[j].Id == B[i].Id && B[j].Arch == B[i].Arch) {
                return PLUG_EEXIST;
            }
        }
    }
    return PLUG_OK;
}

/* Word 0 of an upload: pool and length (record + two words). */
static __inline ULONG PlugUploadHeader(const PLUG_BILL *B, ULONG Len)
{
    return (B->Pool ? PLUG_CMD_UPLOAD_DATA : PLUG_CMD_UPLOAD_CODE) | (Len / 4 + 2);
}

/* The DSP's acceptance (ua_plugin_upload.c:81-95), SDK form: the plain id (or with the arch byte). */
static __inline ULONG PlugUploadReplyOk(const ULONG R[4], const PLUG_BILL *B)
{
    return R[0] == PLUG_REPLY_UPLOAD && R[1] == 0 &&
           (R[2] == B->Id || R[2] == (B->Id | (B->Arch << 24)));
}

/* ------------------------------------------------------------------ */
/* Licence (ua_plugin_license.c:40-96)                                 */
/* ------------------------------------------------------------------ */

/* (days since 2007-01-01 UTC << 12) + minute of the day; PLUG_ERANGE before 2007. */
static __inline ULONG PlugDate(ULONGLONG FileTime, ULONG *Date)
{
    ULONGLONG secs;

    *Date = 0;
    if (FileTime < PLUG_FILETIME_2007) {
        return PLUG_ERANGE;
    }
    secs = (FileTime - PLUG_FILETIME_2007) / 10000000ULL;
    *Date = (ULONG)(((secs / 86400ULL) << 12) + ((secs % 86400ULL) / 60ULL));
    return PLUG_OK;
}

#define PLUG_LIC_AUTHORIZED     0u
#define PLUG_LIC_DEMO           1u
#define PLUG_LIC_NOT_STARTED    2u
#define PLUG_LIC_EXPIRED        3u
#define PLUG_LIC_UNAVAILABLE    4u
#define PLUG_LIC_UNKNOWN        0xFFFFFFFFu

static __inline ULONG PlugLicenseState(ULONG Word)
{
    switch (Word) {
    case 0x81000000u:
    case 0x82000000u: return PLUG_LIC_AUTHORIZED;
    case 0x80000000u: return PLUG_LIC_NOT_STARTED;
    case 0x00000000u: return PLUG_LIC_EXPIRED;
    case 0x83000000u: return PLUG_LIC_UNAVAILABLE;
    default:          return PLUG_LIC_DEMO;
    }
}

static __inline ULONG PlugLicenseAdmits(ULONG State)
{
    return State == PLUG_LIC_AUTHORIZED || State == PLUG_LIC_DEMO;
}

/* ------------------------------------------------------------------ */
/* Private rows, memspec, WRITE checks (ua_plugin_upload.c:97-160,     */
/* ua_plugin_instance.c:393-558)                                       */
/* ------------------------------------------------------------------ */

typedef struct _PLUG_ROW {
    ULONG   Pool, Dwords, Link, Flags;
} PLUG_ROW;

/*
 * The link table of a chain: {0x0015nnnn, (address, value)...}, 2n - 1 words, or 0 on error.
 * Desc[c] = the first row of descriptor c (Desc[0] = 0). A descriptor's first row links into
 * the head's rows, its other rows into its own.
 */
static __inline ULONG PlugMemspec(const PLUG_ROW *Rows, ULONG N, const ULONG *Desc, ULONG NDesc,
                                  const ULONG *Addr, ULONG *Words)
{
    ULONG c, j, m = 0, first, last, tgtFirst, tgtCount, t, off, link;

    if (Rows == 0 || Desc == 0 || Addr == 0 || Words == 0 || N == 0 || N > PLUG_PRIV_MAX ||
        NDesc == 0 || NDesc > N || Desc[0] != 0) {
        return 0;
    }
    for (j = 0; j < N; j++) {
        if (Rows[j].Pool < 1 || Rows[j].Pool > 3 || Rows[j].Dwords == 0 ||
            (Rows[j].Flags != 0 && Rows[j].Flags != 2) || Addr[j] == 0xFFFFFFFFu ||
            (Addr[j] & 1) != 0 || (ULONGLONG)Addr[j] + Rows[j].Dwords > 0xFFFFFFFFULL) {
            return 0;
        }
    }
    for (c = 1; c < NDesc; c++) {
        if (Desc[c] <= Desc[c - 1] || Desc[c] >= N) {
            return 0;
        }
    }
    if (Rows[0].Link != 0xFFFFFFFFu) {
        return 0;
    }
    Words[m++] = PLUG_CMD_MEMSPEC | (2 * N - 1);
    for (c = 0; c < NDesc; c++) {
        first = Desc[c];
        last = (c + 1 < NDesc) ? Desc[c + 1] : N;
        for (j = first; j < last; j++) {
            link = Rows[j].Link;
            if (link == 0xFFFFFFFFu) {
                if (j != 0) {
                    return 0;
                }
                continue;
            }
            tgtFirst = (j == first) ? 0 : first;
            tgtCount = (j == first) ? ((NDesc > 1) ? Desc[1] : N) : last - first;
            t = link >> 24;
            off = link & 0xFFFFFFu;
            if (t >= tgtCount || off >= Rows[tgtFirst + t].Dwords) {
                return 0;
            }
            Words[m++] = Addr[tgtFirst + t] + off;
            Words[m++] = Addr[j];
        }
    }
    return (m == 2 * N - 1) ? m : 0;
}

/* Are the rows one valid descriptor? Checked with placeholder addresses (0). */
static __inline ULONG PlugCheckRows(const PLUG_ROW *Rows, ULONG N, ULONG *Scratch2N, ULONG *ScratchN)
{
    static const ULONG desc[1] = { 0 };
    ULONG i;

    if (N == 0 || N > PLUG_PRIV_MAX) {
        return PLUG_EINVAL;
    }
    for (i = 0; i < N; i++) {
        if (Rows[i].Dwords > 0x1000000u) {
            return PLUG_EINVAL;
        }
    }
    if (N == 1) {
        return (Rows[0].Link == 0xFFFFFFFFu && Rows[0].Pool >= 1 && Rows[0].Pool <= 3 &&
                Rows[0].Dwords != 0 && (Rows[0].Flags == 0 || Rows[0].Flags == 2)) ? PLUG_OK : PLUG_EINVAL;
    }
    for (i = 0; i < N; i++) {
        ScratchN[i] = 0;
    }
    return PlugMemspec(Rows, N, desc, 1, ScratchN, Scratch2N) ? PLUG_OK : PLUG_EINVAL;
}

/* Does a row link into word W of row R (a pointer the memspec owns)? */
static __inline ULONG PlugLinkWord(const PLUG_ROW *Rows, ULONG N, ULONG R, ULONG W)
{
    ULONG j;

    for (j = 1; j < N; j++) {
        if (Rows[j].Link != 0xFFFFFFFFu && (Rows[j].Link >> 24) == R && (Rows[j].Link & 0xFFFFFFu) == W) {
            return 1;
        }
    }
    return 0;
}

/*
 * The next value's word of a WRITE at Word with skip Mask (bit i skips word Word + i + 1): Pos is
 * the candidate (Word for the first value, the previous value's word + 1 after that).
 */
static __inline ULONG PlugWriteSkip(ULONG Word, ULONG Mask, ULONG Pos)
{
    while (Pos > Word && Pos - Word - 1 < 32 && ((Mask >> (Pos - Word - 1)) & 1)) {
        Pos++;
    }
    return Pos;
}

/*
 * UA's WRITE blocks, each inside its row and clear of every linked word. Returns PLUG_OK and
 * *NCmds, or PLUG_EINVAL.
 */
static __inline ULONG PlugCheckCmds(const PLUG_ROW *Rows, ULONG NRows, const ULONG *C, ULONG NW, ULONG *NCmds)
{
    ULONG i = 0, k, cmds = 0, len, row, word, mask, pos;

    *NCmds = 0;
    while (i < NW) {
        len = C[i] & 0xFFFFu;
        if ((C[i] >> 16) != PLUG_CMD_WRITE || len < 4 || len > NW - i) {
            return PLUG_EINVAL;
        }
        row = C[i + 1] >> 24;
        word = C[i + 1] & 0xFFFFFFu;
        mask = C[i + 2];
        if (row >= NRows || word >= Rows[row].Dwords ||
            len - 3 + PlugPopcount(mask) > Rows[row].Dwords - word) {
            return PLUG_EINVAL;
        }
        for (k = 0, pos = word; k < len - 3; k++, pos++) {
            pos = PlugWriteSkip(word, mask, pos);
            if (PlugLinkWord(Rows, NRows, row, pos)) {
                return PLUG_EINVAL;
            }
        }
        i += len;
        cmds++;
    }
    *NCmds = cmds;
    return PLUG_OK;
}

/*
 * The chain directory, recovered from the checked WRITEs (ua_plugin_instance.c:478-537): head
 * word 0 = 0x40000000, word 5 = N (2..5), word 7 = the carrier (0x846 Mic, 0x83E Input), words
 * 7 + 2i the effects' render ids, each pointer word 6 + 2i with exactly one linked row. Without
 * Chain: refused when the head names more than 2 members. Ids[] = the effects (N - 1 of them),
 * Ids[0] must be Primary. PLUG_EACCES: an effect id outside the licence table.
 *
 * [Ins] Unison (CREATE F_UNISON, upstream UA_PLUGIN_CREATE_F_UNISON, ua_ioctl.h:1189-1194): N = 1..5,
 * member 0 (word 7) is a licensed integrated preamp header, not the implicit Mic carrier. Then Ids[]
 * holds every member (N of them, the header first), Ids[0] must be Primary and *Carrier is the
 * header's id. Every member's id must be inside the licence table.
 */
static __inline ULONG PlugChainIdsEx(const PLUG_ROW *Rows, ULONG NR, const ULONG *Cmd, ULONG NW, ULONG Chain,
                                     ULONG Unison, ULONG Primary, ULONG Ids[5], ULONG *NIds, ULONG *Carrier);

static __inline ULONG PlugChainIds(const PLUG_ROW *Rows, ULONG NR, const ULONG *Cmd, ULONG NW, ULONG Chain,
                                   ULONG Primary, ULONG Ids[5], ULONG *NIds, ULONG *Carrier)
{
    return PlugChainIdsEx(Rows, NR, Cmd, NW, Chain, 0, Primary, Ids, NIds, Carrier);
}

static __inline ULONG PlugChainIdsEx(const PLUG_ROW *Rows, ULONG NR, const ULONG *Cmd, ULONG NW, ULONG Chain,
                                     ULONG Unison, ULONG Primary, ULONG Ids[5], ULONG *NIds, ULONG *Carrier)
{
    ULONG head[16], seen[16];
    ULONG i, k, pos, len, n, j, links, w0;

    *NIds = 0;
    *Carrier = PLUG_MIC_CARRIER;
    for (i = 0; i < 16; i++) {
        head[i] = 0;
        seen[i] = 0;
    }
    for (i = 0; i < 5; i++) {
        Ids[i] = 0;
    }
    for (i = 0; i < NW; i += len) {
        len = Cmd[i] & 0xFFFFu;
        if (len < 4) {
            return PLUG_EINVAL;
        }
        if ((Cmd[i + 1] >> 24) != 0) {
            continue;
        }
        w0 = Cmd[i + 1] & 0xFFFFFFu;
        for (k = 0, pos = w0; k < len - 3; k++, pos++) {
            pos = PlugWriteSkip(w0, Cmd[i + 2], pos);
            if (pos < 16) {
                if (seen[pos] && (pos == 0 || pos == 5 || (pos >= 7 && (pos & 1)))) {
                    return PLUG_EINVAL;
                }
                seen[pos] = 1;
                head[pos] = Cmd[i + 3 + k];
            }
        }
    }
    if (!Chain) {
        return (Unison || (seen[5] && head[5] > 2)) ? PLUG_ENOTSUP : PLUG_OK;
    }
    n = head[5];
    if (Unison) {
        /* [Ins] the header is member 0: N 1..5, every member a licensed id, the header the Primary */
        if (!seen[0] || head[0] != 0x40000000u || !seen[5] || n < 1 || n > 5 || Rows[0].Pool != 1 ||
            Rows[0].Dwords != 6 + 2 * n || !seen[7] || head[7] == PLUG_MIC_CARRIER || head[7] == PLUG_INPUT_CARRIER) {
            return PLUG_EINVAL;             /* the header is a licensed plug-in, never the implicit strip carriers */
        }
        for (i = 0; i < n; i++) {
            if (!seen[7 + 2 * i]) {
                return PLUG_EINVAL;
            }
            links = 0;
            for (j = 1; j < NR; j++) {
                if (Rows[j].Link == 6 + 2 * i) {
                    links++;
                }
            }
            if (links != 1) {
                return PLUG_EINVAL;
            }
            if (head[7 + 2 * i] < PLUG_BQ_BASE || head[7 + 2 * i] >= PLUG_BQ_BASE + PLUG_BQ_WORDS) {
                return PLUG_EACCES;
            }
            Ids[i] = head[7 + 2 * i];
        }
        if (Ids[0] != Primary) {
            return PLUG_EINVAL;
        }
        *Carrier = head[7];
        *NIds = n;
        return PLUG_OK;
    }
    if (!seen[0] || head[0] != 0x40000000u || !seen[5] || n < 2 || n > 5 || Rows[0].Pool != 1 ||
        Rows[0].Dwords != 6 + 2 * n || !seen[7] || (head[7] != PLUG_MIC_CARRIER && head[7] != PLUG_INPUT_CARRIER)) {
        return PLUG_EINVAL;
    }
    *Carrier = head[7];
    for (i = 0; i < n; i++) {
        if (!seen[7 + 2 * i]) {
            return PLUG_EINVAL;
        }
        links = 0;
        for (j = 1; j < NR; j++) {
            if (Rows[j].Link == 6 + 2 * i) {
                links++;
            }
        }
        if (links != 1) {
            return PLUG_EINVAL;
        }
        if (i >= 1) {
            if (head[7 + 2 * i] < PLUG_BQ_BASE || head[7 + 2 * i] >= PLUG_BQ_BASE + PLUG_BQ_WORDS) {
                return PLUG_EACCES;
            }
            Ids[i - 1] = head[7 + 2 * i];
        }
    }
    if (Ids[0] != Primary) {
        return PLUG_EINVAL;
    }
    *NIds = n - 1;
    return PLUG_OK;
}

/* Does a WRITE touch the head directory (words 0, 5, odd >= 7 of row 0)? SERVICE refuses it. */
static __inline ULONG PlugWritesDirectory(const ULONG *Cmd, ULONG NW)
{
    ULONG i, k, pos, len;

    for (i = 0; i < NW; i += len) {
        len = Cmd[i] & 0xFFFFu;
        if (len < 4) {
            return 1;
        }
        if ((Cmd[i + 1] >> 24) != 0) {
            continue;
        }
        for (k = 0, pos = Cmd[i + 1] & 0xFFFFFFu; k < len - 3; k++, pos++) {
            pos = PlugWriteSkip(Cmd[i + 1] & 0xFFFFFFu, Cmd[i + 2], pos);
            if (pos == 0 || pos == 5 || (pos >= 7 && (pos & 1))) {
                return 1;
            }
        }
    }
    return 0;
}

/* Two SYNCH input pairs share an actual input (0x20 is the mono sentinel). */
static __inline ULONG PlugInputsOverlap(ULONG A, ULONG B)
{
    ULONG a1 = A & 0xFFFFu, a2 = A >> 16, b1 = B & 0xFFFFu, b2 = B >> 16;

    return a1 == b1 || a1 == b2 || a2 == b1 || (a2 != PLUG_SYNCH_MONO && a2 == b2);
}

/*
 * The inputs of a routed chain (CREATE F_START / F_STAGE), Synch = in1 | in2 << 16, as this host
 * takes them. PLUG_EINVAL: outside upstream's CREATE contract, in1 >= 0x20, in2 > 0x20 or
 * in1 == in2 (oa:driver/ua_plugin_instance.c:1565-1567). Mono (in2 0x20): any in1 below 0x20, as
 * in P0 (whether that input has a live plain strip is the strip target's question). A pair: a
 * linked pair of the unit's NumInputs analog inputs, in1 even and in2 = in1 + 1 (x8p: 0/1, 2/3,
 * 4/5, 6/7 = inputs 1/2 .. 7/8; upstream's STEREO_FIRST pairs, oa:mixer-engine/
 * native_chain_composer.py:200-204). PLUG_ENOTSUP: any other pairing upstream's contract allows.
 */
static __inline ULONG PlugSynchCheck(ULONG Synch, ULONG NumInputs)
{
    ULONG in1 = Synch & 0xFFFFu, in2 = Synch >> 16;

    if (in1 >= PLUG_SYNCH_MONO || in2 > PLUG_SYNCH_MONO || in1 == in2) {
        return PLUG_EINVAL;
    }
    if (in2 == PLUG_SYNCH_MONO) {
        return PLUG_OK;
    }
    if ((in1 & 1) != 0 || in2 != in1 + 1 || in2 >= NumInputs) {
        return PLUG_ENOTSUP;
    }
    return PLUG_OK;
}

/*
 * [Ins] The two stereo aux buses' returns (oa:driver/ua_apollo.h:816-817 UA_BUS_AUX1_RETURN 0x10,
 * AUX2_RETURN 0x12, the R slots 0x11 / 0x13; synch_in = bus id, oa:mixer-engine/hardware.py:360-370):
 * DSP inputs 0x10/0x11 (aux 1) and 0x12/0x13 (aux 2), on the x8p and the Twin X alike. An aux insert
 * chain is SYNCH'd on that pair with the plain Input Strip carrier (0x83E), as upstream composes a
 * Virtual input (native_chain_composer.py:236-245). No plain strip runs there to retain: its teardown
 * stops the chain and restores nothing (experimental, docs/apollo-control-units.md 1.7).
 */
#define PLUG_AUX_BUSES          2u
#define PLUG_AUX_FIRST_SYNCH    0x10u

static __inline ULONG PlugAuxSynch(ULONG Aux)
{
    return (Aux < PLUG_AUX_BUSES) ? ((PLUG_AUX_FIRST_SYNCH + 2u * Aux) | ((PLUG_AUX_FIRST_SYNCH + 2u * Aux + 1u) << 16)) : PLUG_NONE;
}

/* The aux bus (0, 1) whose return pair Synch is, or PLUG_NONE. */
static __inline ULONG PlugAuxOf(ULONG Synch)
{
    ULONG a;

    for (a = 0; a < PLUG_AUX_BUSES; a++) {
        if (Synch == PlugAuxSynch(a)) {
            return a;
        }
    }
    return PLUG_NONE;
}

/* ------------------------------------------------------------------ */
/* The caller rule's pure parts (plugin.c PlugCallerAllowed, Nov 15)   */
/* ------------------------------------------------------------------ */

#define PLUG_SID_CCH_MAX        184u            /* the longest SID string: 15 sub-authorities */
#define PLUG_SID_SUBAUTH_MAX    15u

/* ASCII letters fold to upper case; every other character compares as it is (fail closed). */
static __inline WCHAR PlugFoldW(WCHAR C)
{
    return (C >= L'a' && C <= L'z') ? (WCHAR)(C - L'a' + L'A') : C;
}

/*
 * PluginHostImage as Apollo Control stores it: a DOS path "X:\..." (drive letter, colon,
 * backslash, then the rest, no NUL, no second colon, no forward slash). Returns the drive letter
 * in upper case, or 0 for anything else (a device path, a UNC or \\?\ path, a stream): such a
 * value names no program, and the driver takes no unelevated caller.
 */
static __inline WCHAR PlugImageDrive(const WCHAR *P, ULONG N)
{
    WCHAR d;
    ULONG i;

    if (P == 0 || N < 4) {
        return 0;
    }
    d = PlugFoldW(P[0]);
    if (d < L'A' || d > L'Z' || P[1] != L':' || P[2] != L'\\') {
        return 0;
    }
    for (i = 3; i < N; i++) {
        if (P[i] == 0 || P[i] == L':' || P[i] == L'/') {
            return 0;
        }
    }
    return d;
}

/*
 * Is Located (SeLocateProcessImageName's NT path, "\Device\HarddiskVolume3\...\x.exe") the
 * stored DOS path Stored ("C:\...\x.exe") once its drive letter is resolved now to Target (what
 * \GLOBAL??\C: links to, "\Device\HarddiskVolume3")? 1 = the same file name: Target (a trailing
 * backslash ignored) then Stored from its first backslash, ASCII letters case-insensitive.
 */
static __inline ULONG PlugImageMatch(const WCHAR *Located, ULONG NL, const WCHAR *Target, ULONG NT, const WCHAR *Stored, ULONG NS)
{
    ULONG i;

    if (Located == 0 || Target == 0 || PlugImageDrive(Stored, NS) == 0) {
        return 0;
    }
    while (NT > 0 && Target[NT - 1] == L'\\') {
        NT--;
    }
    if (NT == 0 || Target[0] != L'\\' || NL != NT + (NS - 2)) {
        return 0;
    }
    for (i = 0; i < NT; i++) {
        if (Located[i] == 0 || PlugFoldW(Located[i]) != PlugFoldW(Target[i])) {
            return 0;
        }
    }
    for (i = 2; i < NS; i++) {
        if (PlugFoldW(Located[NT + i - 2]) != PlugFoldW(Stored[i])) {
            return 0;
        }
    }
    return 1;
}

/*
 * A SID string as ConvertSidToStringSidW and RtlConvertSidToUnicodeString write one: "S-1-",
 * the identifier authority (decimal, or 0x and 12 hex digits for a large one), then 1 to 15
 * decimal sub-authorities of at most 10 digits, single dashes between. 1 = well formed.
 */
static __inline ULONG PlugSidTextOk(const WCHAR *S, ULONG N)
{
    ULONG i = 4, field = 0, digits;

    if (S == 0 || N < 7 || N > PLUG_SID_CCH_MAX || PlugFoldW(S[0]) != L'S' || S[1] != L'-' || S[2] != L'1' || S[3] != L'-') {
        return 0;
    }
    for (;;) {
        digits = 0;
        if (field == 0 && i + 1 < N && S[i] == L'0' && PlugFoldW(S[i + 1]) == L'X') {
            for (i += 2; i < N && ((S[i] >= L'0' && S[i] <= L'9') || (PlugFoldW(S[i]) >= L'A' && PlugFoldW(S[i]) <= L'F')); i++) {
                digits++;
            }
            if (digits != 12) {
                return 0;
            }
        } else {
            for (; i < N && S[i] >= L'0' && S[i] <= L'9'; i++) {
                digits++;
            }
            if (digits == 0 || digits > 10) {
                return 0;
            }
        }
        field++;
        if (i == N) {
            break;
        }
        if (S[i] != L'-' || i + 1 == N) {
            return 0;
        }
        i++;
    }
    return (field >= 2 && field - 1 <= PLUG_SID_SUBAUTH_MAX) ? 1 : 0;
}

/* The same SID, as two well-formed strings (the letters S and hex digits in either case). */
static __inline ULONG PlugSidTextEqual(const WCHAR *A, ULONG NA, const WCHAR *B, ULONG NB)
{
    ULONG i;

    if (!PlugSidTextOk(A, NA) || !PlugSidTextOk(B, NB) || NA != NB) {
        return 0;
    }
    for (i = 0; i < NA; i++) {
        if (PlugFoldW(A[i]) != PlugFoldW(B[i])) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Staged startup (ua_ioctl.h:1095-1100, :1154, :1215-1217;            */
/* ua_plugin_instance.c:1277-1332, :1557-1606; native_carrier.py:255)  */
/* ------------------------------------------------------------------ */

/* CREATE flags, as apollo_plugin.h's APOLLO_PLUGIN_F_* (upstream's bit numbers) */
#define PLUG_F_CHECK            0x01u
#define PLUG_F_START            0x02u
#define PLUG_F_STAGE            0x04u
#define PLUG_F_CHAIN            0x08u
#define PLUG_F_UNISON           0x10u   /* [Ins] upstream's UA_PLUGIN_CREATE_F_UNISON: STAGE | CHAIN | UNISON only */

/*
 * The CREATE flag combinations this host takes: CHECK alone; none (unrouted); START | CHAIN; and
 * STAGE | CHAIN, a chain whose startup is over one command page: its first page goes with the
 * CREATE (no strip stopped, nothing routed), the rest as SERVICE writes, then op START routes it.
 * START and STAGE exclude each other and CHECK; CHAIN needs one of them, and either needs CHAIN
 * (chains only, review P0 finding 2). [Ins] UNISON only as STAGE | CHAIN | UNISON: a Unison chain
 * is always staged, begun (analog lease) and activated (upstream: "UNISON with START returns
 * EOPNOTSUPP", ua_ioctl.h:1196-1197). PLUG_OK or PLUG_EINVAL.
 */
static __inline ULONG PlugCreateFlagsOk(ULONG F)
{
    ULONG rs = F & (PLUG_F_START | PLUG_F_STAGE);

    if (F & PLUG_F_UNISON) {
        return (F == (PLUG_F_STAGE | PLUG_F_CHAIN | PLUG_F_UNISON)) ? PLUG_OK : PLUG_EINVAL;
    }
    if ((F & ~(PLUG_F_CHECK | PLUG_F_START | PLUG_F_STAGE | PLUG_F_CHAIN)) != 0) {
        return PLUG_EINVAL;
    }
    if (F & PLUG_F_CHECK) {
        return (F == PLUG_F_CHECK) ? PLUG_OK : PLUG_EINVAL;
    }
    if (rs == (PLUG_F_START | PLUG_F_STAGE)) {
        return PLUG_EINVAL;
    }
    if ((rs != 0) != ((F & PLUG_F_CHAIN) != 0)) {
        return PLUG_EINVAL;
    }
    return PLUG_OK;
}

/*
 * The next startup page at word At of a whole WRITE sequence, as upstream's chunks(blocks())
 * cuts it (native_carrier.py:255-271): whole blocks, in order, as many as fit Max words. Returns
 * its length in words, 0 at the end, PLUG_NONE for a malformed block or one longer than Max
 * (upstream refuses it: "one WRITE block exceeds SERVICE ABI").
 */
static __inline ULONG PlugChunkNext(const ULONG *Cmd, ULONG NW, ULONG At, ULONG Max)
{
    ULONG n = 0, len;

    if (At >= NW) {
        return 0;
    }
    while (At + n < NW) {
        len = Cmd[At + n] & 0xFFFFu;
        if ((Cmd[At + n] >> 16) != PLUG_CMD_WRITE || len < 4 || len > NW - At - n || len > Max) {
            return PLUG_NONE;
        }
        if (n + len > Max) {
            break;
        }
        n += len;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* The pool ledger of one DSP (ua_resource_pool.c)                     */
/* ------------------------------------------------------------------ */

/*
 * Rules as UAD2System applies them: geometry rounded (base and scratch up to even, capacity
 * down); pool 0 (code) filled from its low end, pools 1-3 from their high end, first fit, every
 * request rounded up to even; pools 1-3 keep a scratch area at their bottom; an exact fit takes
 * an extent whole except the terminal one (the last, at the far end), which stays non-empty; a
 * shared resource takes its extent once and a hold per user; freed extents merge. Extents are
 * kept in list order: pool 0 ascending, pools 1-3 descending, the terminal one last. Nothing
 * here talks to the device. Fixed arrays (PLUG_EXTENTS per pool), no allocation.
 */
typedef struct _PLUG_EXTENT {
    ULONG   Base, Words, Requested, Refs;
    ULONG   Cookie;             /* 0 = free */
    ULONG   Identity;           /* shared extents match on it */
    UCHAR   Shared, Terminal, Pad[2];
} PLUG_EXTENT;

typedef struct _PLUG_POOL {
    ULONG       N;
    PLUG_EXTENT E[PLUG_EXTENTS];
} PLUG_POOL;

/*
 * One user's hold on an extent (ua_resource_pool.c:48-55): a lease names its hold ticket, never
 * reused, so a stale or repeated release can never drop another user's hold.
 */
#define PLUG_HOLDS              512u

typedef struct _PLUG_HOLD {
    ULONG   Ticket;
    ULONG   Kind;
    ULONG   Extent;             /* the extent's cookie */
} PLUG_HOLD;

typedef struct _PLUG_LEDGER {
    ULONG       Valid;
    ULONG       NextCookie;
    ULONG       NextTicket;
    ULONG       NHolds;
    ULONG       Base[PLUG_POOLS];           /* rounded */
    ULONG       Words[PLUG_POOLS];          /* rounded, scratch included */
    ULONG       ScratchMin[PLUG_POOLS];     /* the register's, rounded */
    ULONG       Scratch[PLUG_POOLS];        /* current */
    PLUG_POOL   Pool[PLUG_POOLS];
    PLUG_HOLD   Hold[PLUG_HOLDS];
} PLUG_LEDGER;

typedef struct _PLUG_LEASE {
    ULONG   Kind, Base, Words, Cookie;      /* Cookie = the hold ticket; 0 = no lease */
} PLUG_LEASE;

typedef struct _PLUG_REQ {
    ULONG   Kind, Words, Identity, Shared;
} PLUG_REQ;

typedef struct _PLUG_USAGE {
    ULONG   Base, Total, Scratch, Used, Free, Largest, Allocations, Holds;
} PLUG_USAGE;

static __inline ULONG PlugEvenUp(ULONG V)
{
    return V + (V & 1);
}

/* Geometry: raw Base/Words/Scratch per pool (Scratch[0] must be 0). */
static __inline ULONG PlugLedgerInit(PLUG_LEDGER *L, const ULONG Base[4], const ULONG Words[4], const ULONG Scratch[4])
{
    ULONG i, b, size, scr;

    L->Valid = 0;
    L->NextCookie = 0;
    L->NextTicket = 0;
    L->NHolds = 0;
    for (i = 0; i < PLUG_POOLS; i++) {
        L->Pool[i].N = 0;
    }
    for (i = 0; i < PLUG_POOLS; i++) {
        PLUG_EXTENT *e = &L->Pool[i].E[0];

        if (Base[i] == 0xFFFFFFFFu || Words[i] == 0xFFFFFFFFu || Scratch[i] == 0xFFFFFFFFu ||
            (i == 0 && Scratch[i] != 0)) {
            return PLUG_EINVAL;
        }
        b = PlugEvenUp(Base[i]);
        size = Words[i] & ~1u;
        scr = PlugEvenUp(Scratch[i]);
        if (b < Base[i] || size == 0 || scr + 2 > size || (ULONGLONG)b + size > 0x100000000ULL) {
            return PLUG_EINVAL;
        }
        e->Base = b + scr;
        e->Words = size - scr;
        e->Requested = 0;
        e->Refs = 0;
        e->Cookie = 0;
        e->Identity = 0;
        e->Shared = 0;
        e->Terminal = 1;
        L->Pool[i].N = 1;
        L->Base[i] = b;
        L->Words[i] = size;
        L->Scratch[i] = scr;
        L->ScratchMin[i] = scr;
    }
    L->Valid = 1;
    return PLUG_OK;
}

static __inline ULONG PlugLedgerNewCookie(PLUG_LEDGER *L)
{
    if (++L->NextCookie == 0) {
        L->NextCookie = 1;
    }
    return L->NextCookie;
}

/* A new hold on extent E of pool Kind, into Out. The caller checked NHolds < PLUG_HOLDS. */
static __inline void PlugLedgerHold(PLUG_LEDGER *L, ULONG Kind, const PLUG_EXTENT *E, PLUG_LEASE *Out)
{
    PLUG_HOLD *h = &L->Hold[L->NHolds++];

    if (++L->NextTicket == 0) {
        L->NextTicket = 1;
    }
    h->Ticket = L->NextTicket;
    h->Kind = Kind;
    h->Extent = E->Cookie;
    Out->Kind = Kind;
    Out->Base = E->Base;
    Out->Words = E->Words;
    Out->Cookie = h->Ticket;
}

static __inline ULONG PlugLedgerReserve(PLUG_LEDGER *L, ULONG Kind, ULONG Words, ULONG Identity, ULONG Shared,
                                        PLUG_LEASE *Out)
{
    PLUG_POOL *p;
    ULONG k, m, req = Words, we;

    Out->Kind = Out->Base = Out->Words = Out->Cookie = 0;
    if (!L->Valid || Kind >= PLUG_POOLS || Words == 0 || Words >= 0xFFFFFFFEu || (Shared && Identity == 0)) {
        return PLUG_EINVAL;
    }
    if (L->NHolds >= PLUG_HOLDS) {
        return PLUG_ENOSPC;
    }
    p = &L->Pool[Kind];
    if (Shared) {
        for (k = 0; k < p->N; k++) {
            PLUG_EXTENT *e = &p->E[k];

            if (e->Cookie == 0 || e->Identity != Identity) {
                continue;
            }
            if (!e->Shared || e->Requested != req || e->Refs == 0xFFFFFFFFu) {
                return PLUG_EEXIST;
            }
            e->Refs++;
            PlugLedgerHold(L, Kind, e, Out);
            return PLUG_OK;
        }
    }
    we = PlugEvenUp(Words);
    for (k = 0; k < p->N; k++) {
        PLUG_EXTENT *e = &p->E[k], *n;

        if (e->Cookie != 0 || e->Words < we || (e->Terminal && e->Words <= we)) {
            continue;
        }
        if (e->Words == we) {
            n = e;
        } else {
            if (p->N >= PLUG_EXTENTS) {
                return PLUG_ENOSPC;
            }
            for (m = p->N; m > k; m--) {
                p->E[m] = p->E[m - 1];
            }
            p->N++;
            n = &p->E[k];
            e = &p->E[k + 1];
            n->Words = we;
            if (Kind == 0) {
                n->Base = e->Base;
                e->Base += we;
            } else {
                n->Base = e->Base + e->Words - we;
            }
            e->Words -= we;
            n->Terminal = 0;
        }
        n->Cookie = PlugLedgerNewCookie(L);
        n->Identity = Identity;
        n->Requested = req;
        n->Refs = 1;
        n->Shared = Shared ? 1 : 0;
        PlugLedgerHold(L, Kind, n, Out);
        return PLUG_OK;
    }
    return PLUG_ENOSPC;
}

/* *Last = the extent itself went back (its last hold). A ticket that is not held: PLUG_EINVAL. */
static __inline ULONG PlugLedgerRelease(PLUG_LEDGER *L, const PLUG_LEASE *Lease, ULONG *Last)
{
    PLUG_POOL *p;
    ULONG k, m, h, ext;

    if (Last != 0) {
        *Last = 0;
    }
    if (!L->Valid || Lease->Kind >= PLUG_POOLS || Lease->Cookie == 0) {
        return PLUG_EINVAL;
    }
    for (h = 0; h < L->NHolds; h++) {
        if (L->Hold[h].Ticket == Lease->Cookie && L->Hold[h].Kind == Lease->Kind) {
            break;
        }
    }
    if (h == L->NHolds) {
        return PLUG_EINVAL;
    }
    ext = L->Hold[h].Extent;
    p = &L->Pool[Lease->Kind];
    for (k = 0; k < p->N; k++) {
        PLUG_EXTENT *e = &p->E[k];

        if (e->Cookie != ext) {
            continue;
        }
        if (e->Base != Lease->Base || e->Words != Lease->Words || e->Refs == 0) {
            return PLUG_EINVAL;
        }
        L->Hold[h] = L->Hold[--L->NHolds];
        if (--e->Refs != 0) {
            return PLUG_OK;
        }
        if (Last != 0) {
            *Last = 1;
        }
        e->Cookie = 0;
        e->Identity = 0;
        e->Requested = 0;
        e->Shared = 0;
        /* list neighbours are memory neighbours: merge free ones */
        if (k + 1 < p->N && p->E[k + 1].Cookie == 0) {
            PLUG_EXTENT *nx = &p->E[k + 1];

            nx->Base = (e->Base < nx->Base) ? e->Base : nx->Base;
            nx->Words += e->Words;
            for (m = k; m + 1 < p->N; m++) {
                p->E[m] = p->E[m + 1];
            }
            p->N--;
        }
        if (k > 0 && p->E[k - 1].Cookie == 0) {
            PLUG_EXTENT *pv = &p->E[k - 1], *cur = &p->E[k];

            cur->Base = (pv->Base < cur->Base) ? pv->Base : cur->Base;
            cur->Words += pv->Words;
            for (m = k - 1; m + 1 < p->N; m++) {
                p->E[m] = p->E[m + 1];
            }
            p->N--;
        }
        return PLUG_OK;
    }
    return PLUG_EINVAL;
}

/* Every request or none: a failure gives back what this call took, last first. */
static __inline ULONG PlugLedgerReserveBatch(PLUG_LEDGER *L, const PLUG_REQ *Rq, ULONG N, PLUG_LEASE *Out)
{
    ULONG i, err = PLUG_OK;

    for (i = 0; i < N; i++) {
        Out[i].Kind = Out[i].Base = Out[i].Words = Out[i].Cookie = 0;
    }
    if (N == 0 || N > PLUG_RES_MAX + PLUG_PRIV_MAX) {
        return PLUG_EINVAL;
    }
    for (i = 0; i < N; i++) {
        err = PlugLedgerReserve(L, Rq[i].Kind, Rq[i].Words, Rq[i].Identity, Rq[i].Shared, &Out[i]);
        if (err != PLUG_OK) {
            break;
        }
    }
    if (err != PLUG_OK) {
        while (i > 0) {
            i--;
            (void)PlugLedgerRelease(L, &Out[i], 0);
            Out[i].Kind = Out[i].Base = Out[i].Words = Out[i].Cookie = 0;
        }
    }
    return err;
}

/* The scratch at the bottom of data pool Kind: Words (even), never below the register's. */
static __inline ULONG PlugLedgerSetScratch(PLUG_LEDGER *L, ULONG Kind, ULONG Words)
{
    PLUG_POOL *p;
    PLUG_EXTENT *t;
    ULONG want;

    if (!L->Valid || Kind == 0 || Kind >= PLUG_POOLS || Words >= 0xFFFFFFFEu) {
        return PLUG_EINVAL;
    }
    p = &L->Pool[Kind];
    if (p->N == 0) {
        return PLUG_EINVAL;
    }
    want = PlugEvenUp(Words);
    if (want < L->ScratchMin[Kind]) {
        want = L->ScratchMin[Kind];
    }
    t = &p->E[p->N - 1];
    if (!t->Terminal || t->Cookie != 0 || t->Base != L->Base[Kind] + L->Scratch[Kind]) {
        return PLUG_EINVAL;
    }
    if (want > L->Scratch[Kind]) {
        ULONG grow = want - L->Scratch[Kind];

        if (t->Words <= grow) {
            return PLUG_ENOSPC;
        }
        t->Base += grow;
        t->Words -= grow;
    } else {
        ULONG shrink = L->Scratch[Kind] - want;

        t->Base -= shrink;
        t->Words += shrink;
    }
    L->Scratch[Kind] = want;
    return PLUG_OK;
}

static __inline void PlugLedgerUsage(const PLUG_LEDGER *L, PLUG_USAGE U[4])
{
    ULONG i, k;

    for (i = 0; i < PLUG_POOLS; i++) {
        const PLUG_POOL *p = &L->Pool[i];

        U[i].Base = L->Base[i];
        U[i].Total = L->Words[i];
        U[i].Scratch = L->Scratch[i];
        U[i].Used = U[i].Free = U[i].Largest = U[i].Allocations = U[i].Holds = 0;
        if (!L->Valid) {
            continue;
        }
        for (k = 0; k < p->N; k++) {
            const PLUG_EXTENT *e = &p->E[k];

            if (e->Cookie != 0) {
                U[i].Used += e->Words;
                U[i].Allocations++;
                U[i].Holds += e->Refs;
            } else {
                ULONG fits = e->Words & ~1u;

                U[i].Free += e->Words;
                if (e->Terminal) {
                    fits = (fits >= 2) ? fits - 2 : 0;
                }
                if (fits > U[i].Largest) {
                    U[i].Largest = fits;
                }
            }
        }
    }
}

/* No allocation left but those with Identity Keep (0: none at all). */
static __inline ULONG PlugLedgerIdle(const PLUG_LEDGER *L, ULONG Keep)
{
    ULONG i, k;

    for (i = 0; i < PLUG_POOLS; i++) {
        for (k = 0; k < L->Pool[i].N; k++) {
            if (L->Pool[i].E[k].Cookie != 0 && (Keep == 0 || L->Pool[i].E[k].Identity != Keep)) {
                return 0;
            }
        }
    }
    return 1;
}

/*
 * The strip DSP: the Mic Input Strips keep their fixed addresses (strip_plan.h, docs/STRIPS.md
 * 2.6). Their room is booked first, so the ledger never hands it out: the code modules at the
 * bottom of pool 0 (CodeWords from the code base), and Strips x PerStrip[p] words at the top of
 * pools 1-3, which is exactly where StripPlanAddrs puts strips 0..Strips-1 (plugin-plan-test
 * proves it). Identity = PLUG_ID_STRIPS, never released.
 */
#define PLUG_ID_STRIPS          0xFFFFFFF0u

/*
 * The Mic strip's scratch (its AllocInfo: strip_plan.h StripPlanLayoutCheck kScratch): pools 1-3
 * keep at least 0x1C0 / 0x80 / 0 words at their bottom while strips run there, as upstream raises
 * them before the strips are placed (oa:driver/ua_strips.c:1447-1455). The x8p's DSP 1 register
 * says 0x80 for pool 1, so without this the words 0x80..0x1C0 the live strips use would count as
 * free (review P0 finding 1). The floor becomes the ledger's minimum: no later scratch restore
 * (a chain going away) ever drops below it.
 */
#define PLUG_STRIP_SCRATCH1     0x1C0u
#define PLUG_STRIP_SCRATCH2     0x80u
#define PLUG_STRIP_SCRATCH3     0u

static __inline ULONG PlugLedgerStripScratch(PLUG_LEDGER *L)
{
    static const ULONG k[4] = { 0, PLUG_STRIP_SCRATCH1, PLUG_STRIP_SCRATCH2, PLUG_STRIP_SCRATCH3 };
    ULONG p, err;

    for (p = 1; p < PLUG_POOLS; p++) {
        err = PlugLedgerSetScratch(L, p, (L->Scratch[p] > k[p]) ? L->Scratch[p] : k[p]);
        if (err != PLUG_OK) {
            return err;
        }
        L->ScratchMin[p] = L->Scratch[p];
    }
    return PLUG_OK;
}

/*
 * On a fresh ledger: the strips' scratch floor first, then their room (both never handed out). [Virtual] Room[p] = the
 * words pool p's strips take below its top (strip_plan.h StripPlanRoom: the model's Mic strips and the Input Strips of
 * StripVirtualMask), one block per pool from the top down.
 */
static __inline ULONG PlugLedgerBookStripRoom(PLUG_LEDGER *L, ULONG CodeWords, const ULONG Room[4], PLUG_LEASE Out[4])
{
    ULONG p, err;

    Out[0].Kind = Out[0].Base = Out[0].Words = Out[0].Cookie = 0;
    err = PlugLedgerStripScratch(L);
    if (err != PLUG_OK) {
        return err;
    }
    err = PlugLedgerReserve(L, 0, CodeWords, PLUG_ID_STRIPS, 0, &Out[0]);
    for (p = 1; p < PLUG_POOLS && err == PLUG_OK; p++) {
        err = PlugLedgerReserve(L, p, Room[p], PLUG_ID_STRIPS, 0, &Out[p]);
    }
    return err;
}

/* The same for Strips strips of PerStrip[p] words each (the Mic strips only: every build before [Virtual]). */
static __inline ULONG PlugLedgerBookStrips(PLUG_LEDGER *L, ULONG CodeWords, const ULONG PerStrip[4], ULONG Strips,
                                           PLUG_LEASE Out[4])
{
    ULONG room[4], p;

    for (p = 0; p < 4; p++) {
        room[p] = PerStrip[p] * Strips;
    }
    return PlugLedgerBookStripRoom(L, CodeWords, room, Out);
}

/* ------------------------------------------------------------------ */
/* Transactions: entries, paged payload, replies (ua_dsp_xact.c)       */
/* ------------------------------------------------------------------ */

/*
 * One request to one DSP: command-ring entries (inline, or DMA_REFs into the payload) and
 * reply descriptors. The payload is Cap bytes of separate 4 KiB pages: a DMA_REF never crosses
 * a page (no large contiguous buffer: the G4 0xc000009a), a command of at most 4 KiB starts on
 * the next page rather than straddle one, a longer one is split at page boundaries only. Each
 * reply sits in its own 16-byte-aligned slot of the reply area, never across a page.
 * Ent[k][2] of a DMA_REF is the payload byte offset; the driver turns it into the page address.
 */
typedef struct _PLUG_XL {
    ULONG   Err;
    ULONG   N;
    ULONG   Ent[PLUG_XACT_ENTS_MAX][4];
    ULONG   Len;                /* payload bytes used */
    ULONG   Cap;                /* payload bytes available (pages * 4096) */
    ULONG   NRx;
    ULONG   RxWords[PLUG_XACT_RX_MAX];
    ULONG   RxOff[PLUG_XACT_RX_MAX];        /* bytes into the reply area */
    ULONG   RxLen;
    ULONG   RxFill;             /* the reply area's initial word */
} PLUG_XL;

/* The payload pages (the includer's view of them: driver page VAs, or test buffers). */
typedef struct _PLUG_PAY {
    ULONG   NPages;
    UCHAR  *Page[PLUG_PAY_PAGES_MAX];
} PLUG_PAY;

static __inline void PlugXlInit(PLUG_XL *X, ULONG CapBytes)
{
    ULONG i;

    X->Err = (CapBytes > PLUG_PAY_PAGES_MAX * PLUG_PAGE) ? PLUG_EINVAL : PLUG_OK;
    X->N = 0;
    X->Len = 0;
    X->Cap = CapBytes;
    X->NRx = 0;
    X->RxLen = 0;
    X->RxFill = 0;
    for (i = 0; i < PLUG_XACT_RX_MAX; i++) {
        X->RxWords[i] = 0;
        X->RxOff[i] = 0;
    }
}

static __inline void PlugXlAdd(PLUG_XL *X, ULONG W0, ULONG W1, ULONG W2, ULONG W3)
{
    if (X->Err != PLUG_OK) {
        return;
    }
    if (X->N >= PLUG_XACT_ENTS_MAX) {
        X->Err = PLUG_E2BIG;
        return;
    }
    X->Ent[X->N][0] = W0;
    X->Ent[X->N][1] = W1;
    X->Ent[X->N][2] = W2;
    X->Ent[X->N][3] = W3;
    X->N++;
}

/* An inline command: bit 31 of W0 clear, never a DMA reference. */
static __inline void PlugXlInline(PLUG_XL *X, ULONG W0, ULONG W1, ULONG W2, ULONG W3)
{
    if (W0 & PLUG_RING_DMA_REF) {
        if (X->Err == PLUG_OK) {
            X->Err = PLUG_EINVAL;
        }
        return;
    }
    PlugXlAdd(X, W0, W1, W2, W3);
}

/* Room for one DMA-carried command of Dwords: its payload byte offset, or PLUG_NONE (Err set). */
static __inline ULONG PlugXlCmd(PLUG_XL *X, ULONG Dwords)
{
    ULONG bytes = Dwords * 4, off, at, left, piece;

    if (X->Err != PLUG_OK) {
        return PLUG_NONE;
    }
    if (Dwords == 0 || Dwords > 0xFFFFu) {
        X->Err = PLUG_EINVAL;
        return PLUG_NONE;
    }
    off = X->Len;
    if (bytes <= PLUG_PAGE && (off % PLUG_PAGE) + bytes > PLUG_PAGE) {
        off += PLUG_PAGE - off % PLUG_PAGE;
    }
    if (off > X->Cap || bytes > X->Cap - off) {
        X->Err = PLUG_ENOSPC;
        return PLUG_NONE;
    }
    for (at = off, left = bytes; left != 0; at += piece, left -= piece) {
        piece = PLUG_PAGE - at % PLUG_PAGE;
        if (piece > left) {
            piece = left;
        }
        PlugXlAdd(X, PLUG_RING_DMA_REF | (piece / 4), 0, at, 0);
    }
    if (X->Err != PLUG_OK) {
        return PLUG_NONE;
    }
    X->Len = off + bytes;
    return off;
}

/* A reply descriptor of Dwords (replies fill in the order they are added): its index or PLUG_NONE. */
static __inline ULONG PlugXlRx(PLUG_XL *X, ULONG Dwords)
{
    ULONG k = X->NRx;

    if (X->Err != PLUG_OK) {
        return PLUG_NONE;
    }
    if (Dwords == 0 || Dwords > PLUG_XACT_RX_WORDS_MAX) {
        X->Err = PLUG_EINVAL;
        return PLUG_NONE;
    }
    if (k >= PLUG_XACT_RX_MAX) {
        X->Err = PLUG_E2BIG;
        return PLUG_NONE;
    }
    if (X->RxLen % PLUG_PAGE + Dwords * 4 > PLUG_PAGE) {
        X->RxLen += PLUG_PAGE - X->RxLen % PLUG_PAGE;
    }
    if (X->RxLen + Dwords * 4 > PLUG_RX_PAGES_MAX * PLUG_PAGE) {
        X->Err = PLUG_E2BIG;
        return PLUG_NONE;
    }
    X->RxOff[k] = X->RxLen;
    X->RxWords[k] = Dwords;
    X->RxLen += (Dwords * 4 + 15) & ~15u;
    X->NRx = k + 1;
    return k;
}

static __inline ULONG PlugXlRxPages(const PLUG_XL *X)
{
    return (X->RxLen + PLUG_PAGE - 1) / PLUG_PAGE;
}

static __inline ULONG PlugXlPayPages(const PLUG_XL *X)
{
    return (X->Len + PLUG_PAGE - 1) / PLUG_PAGE;
}

/* Copy Bytes into the paged payload at Off. */
static __inline ULONG PlugPayCopy(const PLUG_PAY *P, ULONG Off, const void *Src, ULONG Bytes)
{
    const UCHAR *s = (const UCHAR *)Src;
    ULONG done = 0;

    if (Off > P->NPages * PLUG_PAGE || Bytes > P->NPages * PLUG_PAGE - Off) {
        return PLUG_E2BIG;
    }
    while (done < Bytes) {
        ULONG at = Off + done, pg = at / PLUG_PAGE, in = at % PLUG_PAGE, n = PLUG_PAGE - in, i;

        if (n > Bytes - done) {
            n = Bytes - done;
        }
        if (P->Page[pg] == 0) {
            return PLUG_EINVAL;
        }
        for (i = 0; i < n; i++) {
            P->Page[pg][in + i] = s[done + i];
        }
        done += n;
    }
    return PLUG_OK;
}

static __inline ULONG PlugPayPutByte(const PLUG_PAY *P, ULONG Off, UCHAR V)
{
    return PlugPayCopy(P, Off, &V, 1);
}

/* A DMA command of NW words written from W. */
static __inline ULONG PlugXlCmdWords(PLUG_XL *X, const PLUG_PAY *P, const ULONG *W, ULONG NW)
{
    ULONG off = PlugXlCmd(X, NW), e;

    if (off == PLUG_NONE) {
        return X->Err;
    }
    e = PlugPayCopy(P, off, W, NW * 4);
    if (e != PLUG_OK && X->Err == PLUG_OK) {
        X->Err = e;
    }
    return X->Err;
}

/* ------------------------------------------------------------------ */
/* The words of each plug-in transaction                               */
/* ------------------------------------------------------------------ */

/*
 * One module upload (ua_plugin_upload.c:39-65, SDK form): a reply descriptor of 4 words, then
 * {header, dest, record} by DMA, the record's bytes 10-11 cleared (word 2's flag/arch half).
 */
static __inline ULONG PlugBuildUpload(PLUG_XL *X, const PLUG_PAY *P, const PLUG_BILL *B, const UCHAR *Rec,
                                      ULONG Len, ULONG Dest)
{
    ULONG hdr[2], off;

    if ((Len & 3) != 0 || Len / 4 + 2 > 0xFFFFu || Dest == 0xFFFFFFFFu) {
        X->Err = PLUG_EINVAL;
        return X->Err;
    }
    (void)PlugXlRx(X, 4);
    off = PlugXlCmd(X, Len / 4 + 2);
    if (off == PLUG_NONE) {
        return X->Err;
    }
    hdr[0] = PlugUploadHeader(B, Len);
    hdr[1] = Dest;
    if (PlugPayCopy(P, off, hdr, 8) != PLUG_OK || PlugPayCopy(P, off + 8, Rec, Len) != PLUG_OK ||
        PlugPayPutByte(P, off + 8 + 10, 0) != PLUG_OK || PlugPayPutByte(P, off + 8 + 11, 0) != PLUG_OK) {
        X->Err = PLUG_E2BIG;
    }
    return X->Err;
}

/* The release of one resource (CResourcePool::UnmapResource): no reply. */
static __inline void PlugBuildRelease(PLUG_XL *X, const PLUG_BILL *B)
{
    if (B->Kind == 0) {
        PlugXlInline(X, PLUG_CMD_RELEASE_CODE, B->Id, 0, 0);
    } else {
        PlugXlInline(X, PLUG_CMD_RELEASE_DATA, B->Id, (B->Kind == 4) ? 1 : B->Kind, 0);
    }
}

/*
 * The private rows (ua_plugin_instance.c:610-658): a zero-fill per row (dwords rounded up to
 * even), then the memspec (one descriptor) when there is more than one row. Words = scratch of
 * 2N words.
 */
static __inline ULONG PlugBuildPrivates(PLUG_XL *X, const PLUG_PAY *P, const PLUG_ROW *Rows, ULONG N,
                                        const ULONG *Addr, ULONG *Words)
{
    static const ULONG desc[1] = { 0 };
    ULONG i, n;

    for (i = 0; i < N; i++) {
        PlugXlInline(X, PLUG_CMD_ZERO, Addr[i], 0, Rows[i].Dwords + (Rows[i].Dwords & 1));
    }
    if (N > 1) {
        n = PlugMemspec(Rows, N, desc, 1, Addr, Words);
        if (n == 0) {
            if (X->Err == PLUG_OK) {
                X->Err = PLUG_EINVAL;
            }
            return X->Err;
        }
        (void)PlugXlCmdWords(X, P, Words, n);
    }
    return X->Err;
}

/*
 * Checked WRITEs with word 1 relocated from row << 24 | word to the row's DSP address
 * (UAD2System _fixParameterAddresses, ua_plugin_instance.c:665-679). Out may be Cmd.
 */
static __inline ULONG PlugRelocate(ULONG *Out, const ULONG *Cmd, ULONG NW, const ULONG *RowAddr, ULONG NRows)
{
    ULONG i, j, len, row;

    for (i = 0; i < NW; i += len) {
        len = Cmd[i] & 0xFFFFu;
        if (len < 4 || len > NW - i) {
            return PLUG_EINVAL;
        }
        row = Cmd[i + 1] >> 24;
        if (row >= NRows) {
            return PLUG_EINVAL;
        }
        for (j = 0; j < len; j++) {
            Out[i + j] = Cmd[i + j];
        }
        Out[i + 1] = RowAddr[row] + (Cmd[i + 1] & 0xFFFFFFu);
    }
    return PLUG_OK;
}

/*
 * ua_inst_start (ua_plugin_instance.c:689-739). Route (F_START, op START): stop what runs on each
 * input, the relocated WRITEs (one DMA command, none when NW is 0: op START of a staged chain),
 * SYNCH the head on Synch, then span none twice and route 0x1FFFF per input. Without Route
 * (F_STAGE): the WRITEs only; nothing is stopped, started or routed. Reloc = scratch of NW words.
 */
static __inline ULONG PlugBuildStartEx(PLUG_XL *X, const PLUG_PAY *P, const ULONG *Cmd, ULONG NW,
                                       const ULONG *RowAddr, ULONG NRows, ULONG Synch, ULONG *Reloc, ULONG Route)
{
    ULONG in[2], k;

    in[0] = Synch & 0xFFFFu;
    in[1] = Synch >> 16;
    if (!Route && NW == 0) {
        X->Err = PLUG_EINVAL;               /* a stage with nothing to write */
        return X->Err;
    }
    for (k = 0; Route && k < 2; k++) {
        if (in[k] != PLUG_SYNCH_MONO) {
            PlugXlInline(X, PLUG_CMD_SYNCH, (PLUG_SYNCH_MONO << 16) | in[k], 0, 0);
        }
    }
    if (NW != 0) {
        if (PlugRelocate(Reloc, Cmd, NW, RowAddr, NRows) != PLUG_OK) {
            X->Err = PLUG_EINVAL;
            return X->Err;
        }
        (void)PlugXlCmdWords(X, P, Reloc, NW);
    }
    if (!Route) {
        return X->Err;
    }
    PlugXlInline(X, PLUG_CMD_SYNCH, Synch, RowAddr[0], 0);
    for (k = 0; k < 2; k++) {
        if (in[k] == PLUG_SYNCH_MONO) {
            continue;
        }
        PlugXlInline(X, PLUG_CMD_INPUT, in[k], PLUG_INPUT_SPAN, PLUG_SPAN_NONE);
        PlugXlInline(X, PLUG_CMD_INPUT, in[k], PLUG_INPUT_SPAN, PLUG_SPAN_NONE);
        PlugXlInline(X, PLUG_CMD_INPUT, in[k], PLUG_INPUT_ROUTE, PLUG_ROUTE_NONE);
    }
    return X->Err;
}

/* F_START: stop the input's plain strip, the WRITEs, SYNCH the head, span/route (P0's start). */
static __inline ULONG PlugBuildStart(PLUG_XL *X, const PLUG_PAY *P, const ULONG *Cmd, ULONG NW,
                                     const ULONG *RowAddr, ULONG NRows, ULONG Synch, ULONG *Reloc)
{
    return PlugBuildStartEx(X, P, Cmd, NW, RowAddr, NRows, Synch, Reloc, 1);
}

/*
 * The teardown's first words (ua_plugin_instance.c:267-271, ua_strips.c:1101-1132): with Stop,
 * the chain's SYNCH with head 0; with Restore, each input's retained plain strip SYNCH'd again at
 * its header (Header[k], 0 = none), span none twice and route 0x1FFFF. The releases follow
 * (PlugBuildRelease).
 */
static __inline ULONG PlugBuildStop(PLUG_XL *X, ULONG Synch, ULONG Stop, ULONG Restore, const ULONG Header[2])
{
    ULONG in[2], k;

    in[0] = Synch & 0xFFFFu;
    in[1] = Synch >> 16;
    if (Stop) {
        PlugXlInline(X, PLUG_CMD_SYNCH, Synch, 0, 0);
    }
    for (k = 0; Restore && k < 2; k++) {
        if (in[k] == PLUG_SYNCH_MONO || Header[k] == 0) {
            continue;
        }
        PlugXlInline(X, PLUG_CMD_SYNCH, (PLUG_SYNCH_MONO << 16) | in[k], Header[k], 0);
        PlugXlInline(X, PLUG_CMD_INPUT, in[k], PLUG_INPUT_SPAN, PLUG_SPAN_NONE);
        PlugXlInline(X, PLUG_CMD_INPUT, in[k], PLUG_INPUT_SPAN, PLUG_SPAN_NONE);
        PlugXlInline(X, PLUG_CMD_INPUT, in[k], PLUG_INPUT_ROUTE, PLUG_ROUTE_NONE);
    }
    return X->Err;
}

/* SERVICE read (ua_plugin_instance.c:1072-1077): one reply of NR + 2 words, prefill 0xFFCCCCCC. */
#define PLUG_READ_FILL          0xFFCCCCCCu

static __inline ULONG PlugBuildRead(PLUG_XL *X, ULONG Addr, ULONG NR, ULONG Word)
{
    X->RxFill = PLUG_READ_FILL;
    (void)PlugXlRx(X, NR + 2);
    PlugXlInline(X, PLUG_CMD_READ, Addr, NR, Word);
    return X->Err;
}

static __inline ULONG PlugReadReplyOk(const ULONG *R, ULONG NR, ULONG Word)
{
    return R[0] == (PLUG_REPLY_READ | (NR + 2)) && R[1] == Word;
}

/* The board query (ua_plugin_license.c:140-149): the reply descriptor first, then date and query. */
static __inline ULONG PlugBuildBoardQuery(PLUG_XL *X, ULONG Date, ULONG Cookie)
{
    X->RxFill = PLUG_BQ_SENTINEL;
    (void)PlugXlRx(X, PLUG_BQ_RX_WORDS);
    PlugXlInline(X, PLUG_BQ_CMD_DATE, Date, 0, 0);
    PlugXlInline(X, PLUG_BQ_CMD_QUERY, Cookie, 0, 0);
    return X->Err;
}

/* ------------------------------------------------------------------ */
/* Hosting per model (P7, Nov 15 2026: the Twin X Gen 1 Duo)            */
/* ------------------------------------------------------------------ */

/*
 * Every insert is a chain that takes the place of one input's plain Mic Input Strip on the strip DSP
 * (oa:driver/ua_strips.c:1059-1098), so a model hosts plug-ins exactly where it builds strips
 * (strip_plan.h StripModelFor; plugin-plan-test checks that the two tables agree). What differs per
 * model is data, read by apollo.sys (the gate, its logs), Apollo Control (inputs, pairs, DSP room) and
 * apollo-plug (input checks, the offline ledger):
 *   - the strip DSP: DSP 1 on both (the strip pass's StripDsp decides at run time; DSP 0 is the
 *     mixer's and never takes a placement);
 *   - the ledger books the room of the unit's own strips only (x8p 8, Twin X 2): the rest of DSP 1's
 *     pools is plug-in room;
 *   - the licence: the board query on that unit's own DSP 0, its own table (each unit answers for
 *     itself: a licence bought for one unit is not the other's);
 *   - the strip pass sent route 0x1FFFF to every other DSP (x8p 0, 2-5; Twin X Duo 0 only); a chain
 *     sends span and route on the strip DSP only (ua_plugin_instance.c:689-740), so nothing new;
 *   - the inputs: analog 1..Inputs = DSP inputs FirstSynch.. (0 on both), pairs 2k / 2k+1 below Inputs
 *     (Twin X: the pair 1/2 only); a Hi-Z jack feeds the same DSP input, so it shares that input's
 *     strip and insert; the talkback mic has no strip (no captured Talkback Input Strip recipe), so
 *     a chain on it is refused, as is any other DSP input outside InputMask.
 * StripDspRegs: the strip DSP's pool registers as measured (CAPS Regs order: sizes 0-3, bases 0-3,
 * scratch 1-3), for the offline tools only; the driver reads the live ones.
 */
#define PLUG_MODEL_X8P          1u      /* = apollo_model.h APOLLO_MODEL_X8P = strip_plan.h STRIP_MODEL_X8P */
#define PLUG_MODEL_TWINX        2u      /* = APOLLO_MODEL_TWINX = STRIP_MODEL_TWINX: Twin X Gen 1 (0x99), Duo */

typedef struct _PLUG_MODEL {
    ULONG       Model;              /* PLUG_MODEL_* */
    const char *Name;               /* "x8p", "Twin X": texts in the host and the tools */
    ULONG       Dsps;               /* DSPs on the unit */
    ULONG       StripDsp;           /* where the strips and every insert run */
    ULONG       BoardDsp;           /* the board query (licence table): the unit's own DSP 0 */
    ULONG       Strips;             /* strips whose room the ledger books on the strip DSP */
    ULONG       Inputs;             /* analog inputs 1..Inputs take an insert */
    ULONG       FirstSynch;         /* DSP input of analog input 1 */
    ULONG       InputMask;          /* DSP inputs with a Mic Input Strip (bit = DSP input) */
    ULONG       RouteDsps;          /* DSPs the strip pass sends route 0x1FFFF to: all but the strip DSP */
    ULONG       HizMask;            /* DSP inputs whose Hi-Z jack shares the input's strip */
    ULONG       Talkback;           /* DSP input of the talkback mic (no strip: refused), PLUG_NONE = not looked up */
    ULONG       StripDspRegs[11];   /* measured, CAPS Regs order */
} PLUG_MODEL;

static __inline const PLUG_MODEL *PlugModelFor(ULONG Model)
{
    static const PLUG_MODEL t[2] = {
        {
            /* docs/PLUGINS.md 3.4, docs/STRIPS.md 2.4 (pools measured Nov 14) */
            PLUG_MODEL_X8P, "x8p", 6, 1, 0, 8, 8, 0, 0xFF, 0x3D, 0x03, PLUG_NONE,
            { 0x4000, 0xa9b9, 0x1fff, 0x00ff516a, 0xe0000, 0x92800, 0xc5000, 0x08008e96, 0x80, 0x80, 0x8000 },
        },
        {
            /* docs/spec-twinx.md 5.2 and 6.9.5: strips on DSP 1 (G5, Nov 15), TALKBACK = synch 10
               (oa:devices/apollo-twin-x.json:115-137); pools as step-g5.log read them (same tops as the x8p,
               pool 3 base 0x08008e52) */
            PLUG_MODEL_TWINX, "Twin X", 2, 1, 0, 2, 2, 0, 0x03, 0x01, 0x01, 10,
            { 0x4000, 0xa9b9, 0x1fff, 0x00ff51ae, 0xe0000, 0x92800, 0xc5000, 0x08008e52, 0x80, 0x80, 0x8000 },
        },
    };
    ULONG i;

    for (i = 0; i < 2; i++) {
        if (t[i].Model == Model) {
            return &t[i];
        }
    }
    return 0;
}

/*
 * A routed chain's Synch on this model: PlugSynchCheck with the model's inputs (malformed EINVAL, a
 * pairing it does not take ENOTSUP), then every actual input must have a strip there (ENOTSUP: the
 * talkback, a virtual input, an input the unit does not have). The host and the tools ask this before
 * the driver does; apollo.sys refuses the same inputs in its strip target (STATUS_NOT_FOUND).
 */
static __inline ULONG PlugModelSynch(const PLUG_MODEL *M, ULONG Synch)
{
    ULONG in1 = Synch & 0xFFFFu, in2 = Synch >> 16, e;

    if (M == 0) {
        return PLUG_ENOTSUP;
    }
    e = PlugSynchCheck(Synch, M->FirstSynch + M->Inputs);
    if (e != PLUG_OK) {
        return e;
    }
    if (((M->InputMask >> in1) & 1u) == 0 || (in2 != PLUG_SYNCH_MONO && ((M->InputMask >> in2) & 1u) == 0)) {
        return PLUG_ENOTSUP;
    }
    return PLUG_OK;
}
