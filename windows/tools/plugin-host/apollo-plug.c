/*
 * apollo-plug.c - the host side of UAD plug-in hosting, phase P0 / gate P1 (docs/PLUGINS.md 4.1).
 *
 * Reads a recipe file that tools\plugin-host\p1_recipe.py wrote from a mapped bundle (plugins\bundles,
 * built by open-apollo's own carrier code), installs its modules into the admin-only module store,
 * and runs the gate P1 steps through apollo.sys's IOCTL block 0x40-0x43 (include\apollo_plugin.h).
 *
 *   apollo-plug [--unit x8p|twinx] caps          (P7: --unit twinx = the Twin X Gen 1: its inputs 1-2, its ledger)
 *   apollo-plug check <recipe> [--store DIR]           offline: the driver's own checks and encodings
 *   apollo-plug modules install <recipe> --from DIR    elevated: DIR\<sha256>.bill -> ApolloModules
 *   apollo-plug modules verify <recipe>                elevated: the installed copies
 *   apollo-plug licence [--recipe R] [--names F] [--authorized-out F]
 *   apollo-plug create-check <recipe>
 *   apollo-plug create-plain <recipe> --dsp N
 *   apollo-plug create-start <recipe> --dsp 1 [--hold S] [--end destroy|close] [--ref SLOT] [--mon] [--staged]
 *
 * A startup over one 1016-word page (Lexicon 480L, the vst2_bx family) is staged as upstream's
 * daemon stages it (plugin_native_chain.py:323-371): CREATE STAGE+CHAIN with page 0 (the plain
 * strip keeps running), the other pages as SERVICE writes, the whole startup read back while
 * nothing routes it, then START. Any failure before START destroys the never-routed chain; Ctrl+C
 * or a crash closes the handle, and the driver does the same. Pages are cut as upstream's
 * chunks(blocks()) cuts them and compared with the recipe's own cut (apollo-plug check).
 *
 * Readback: the recipe's "dsp_owned <row> <word> <source>" words (meters, envelopes:
 * the running DSP writes them) are never compared. A word that differs is read once more; one whose
 * value moved between the two reads is the DSP's own too (upstream learns them per instance,
 * oa:mixer-engine/plugin_native.py:214-223), the same wrong value twice is a difference. After a
 * restore the driver's state is checked (strips, instances, the DSP's ledger against its baseline,
 * GET_STRIPS), not the sound: two outputs of one mic differ acoustically. A stereo recipe (layout
 * stereo, synch in1 in1+1) needs FEAT_STEREO; its effect is still measured on in1.
 *
 * It never contacts Universal Audio and never loads a UA program or DLL: module records are read
 * as bytes and checked here and again by the driver. The licence table is never printed or
 * logged whole: counts per state and the states of the plug-ins asked about only. Output lines
 * starting "PASS " / "CHECK " / "INFO " are what tools\phase1\step-p1.ps1 reads.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <bcrypt.h>
#include <sddl.h>
#include <aclapi.h>

#include "..\..\include\apollo_ioctl.h"
#include "..\..\include\apollo_select.h"
#include "..\..\include\apollo_strip_pins.h"
#include "..\..\driver\strip_plan.h"
#include "..\..\driver\plugin_plan.h"

#define FULL_SCALE          2147483648.0
#define MEAS_FRAMES         24000u      /* 0.5 s at 48 kHz */
#define MEAS_TAIL_FRAMES    144000u     /* 3 s: room for several claps and their decay */
#define MAX_LAG             1024
#define OBS_MAX             8192u
#define RECIPE_CMD_MAX      (1u << 18)  /* startup words of one recipe (a staged startup: many pages) */
#define RECIPE_PAGES_MAX    512u
#define OWNED_MAX           1024u       /* DSP-owned words of one recipe (its dsp_owned lines + those learned in a run) */
/* System + Administrators only, as apollo-diag strips install (docs/STRIPS.md 4.1) */
#define MOD_DIR_SDDL        L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)"
#define MOD_FILE_SDDL       L"D:P(A;;FA;;;SY)(A;;FA;;;BA)"

static HANDLE g_dev = INVALID_HANDLE_VALUE;
static BOOL g_admin = FALSE;
static int g_want = APOLLO_PICK_X8P;
/* [P7] the unit's hosting plan (plugin_plan.h): --unit, else the opened unit's model; the x8p by default */
static const PLUG_MODEL *g_model;
static APOLLO_UNIT_ENTRY g_units[APOLLO_SELECT_MAX];
static int g_pass, g_check;

static void Pass(const char *fmt, ...)
{
    va_list ap;
    g_pass++;
    printf("PASS   ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static void Check(const char *fmt, ...)
{
    va_list ap;
    g_check++;
    printf("CHECK  ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static void Info(const char *fmt, ...)
{
    va_list ap;
    printf("INFO   ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static const char *StatusText(LONG s)
{
    switch ((ULONG)s) {
    case 0x00000000u: return "success";
    case 0xC0000022u: return "ACCESS_DENIED (the device does not authorize it)";
    case 0xC00000BBu: return "NOT_SUPPORTED";
    case 0xC000000Du: return "INVALID_PARAMETER";
    case 0xC0000184u: return "INVALID_DEVICE_STATE";
    case 0xC00000B5u: return "IO_TIMEOUT (the DSP did not answer in time)";
    case 0xC0000120u: return "CANCELLED (epoch changed or the device stopped)";
    case 0xC000009Du: return "DEVICE_NOT_CONNECTED";
    case 0xC00000A3u: return "DEVICE_NOT_READY";
    case 0x80000011u: return "DEVICE_BUSY";
    case 0xC000009Au: return "INSUFFICIENT_RESOURCES (no room or no memory)";
    case 0xC0000035u: return "OBJECT_NAME_COLLISION (a module with that id is already in use)";
    case 0xC0000225u: return "NOT_FOUND";
    case 0xC0000034u: return "OBJECT_NAME_NOT_FOUND (a module is not in the store)";
    case 0xC0000428u: return "INVALID_IMAGE_HASH";
    case 0xC000007Bu: return "INVALID_IMAGE_FORMAT";
    case 0xC00000D0u: return "REQUEST_NOT_ACCEPTED (the DSP refused an upload)";
    case 0xC000038Eu: return "DEVICE_PROTOCOL_ERROR (a reply that is not ours)";
    case 0xC0000182u: return "DEVICE_CONFIGURATION_ERROR (pool map)";
    default:          return "see ntstatus.h";
    }
}

static const char *StepText(ULONG s)
{
    static const char *t[] = { "none", "validate", "licence", "modules", "place", "upload", "privates", "start",
                               "done", "teardown", "service write", "service read", "route (START)" };
    return s < sizeof(t) / sizeof(t[0]) ? t[s] : "?";
}

static const char *LicText(ULONG s)
{
    switch (s) {
    case APOLLO_PLUGIN_LICENSE_AUTHORIZED:  return "authorized";
    case APOLLO_PLUGIN_LICENSE_DEMO:        return "demo running";
    case APOLLO_PLUGIN_LICENSE_NOT_STARTED: return "demo not started";
    case APOLLO_PLUGIN_LICENSE_EXPIRED:     return "expired";
    case APOLLO_PLUGIN_LICENSE_UNAVAILABLE: return "unavailable";
    default:                                return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* Device                                                              */
/* ------------------------------------------------------------------ */

static BOOL OpenDevice(void)
{
    int n = ApolloListUnits(g_units, APOLLO_SELECT_MAX, NULL, 0), i;

    if (n <= 0) {
        fprintf(stderr, "No Apollo on apollo.sys.\n");
        return FALSE;
    }
    i = ApolloPickUnit(g_units, n, g_want);
    if (i < 0) {
        fprintf(stderr, "No Apollo matches the --unit asked for.\n");
        return FALSE;
    }
    if (ApolloInfoIsTwinX(&g_units[i].Info, g_units[i].InfoBytes)) {
        g_model = PlugModelFor(PLUG_MODEL_TWINX);
    } else if (ApolloInfoIsX8p(&g_units[i].Info, g_units[i].InfoBytes)) {
        g_model = PlugModelFor(PLUG_MODEL_X8P);
    }
    g_dev = CreateFileW(g_units[i].Path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    g_admin = (g_dev != INVALID_HANDLE_VALUE);
    if (g_dev == INVALID_HANDLE_VALUE) {
        g_dev = CreateFileW(g_units[i].Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (g_dev == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Opening the Apollo failed: error %lu\n", GetLastError());
        return FALSE;
    }
    return TRUE;
}

static void CloseDevice(void)
{
    if (g_dev != INVALID_HANDLE_VALUE) {
        CloseHandle(g_dev);
    }
    g_dev = INVALID_HANDLE_VALUE;
}

static BOOL GetCaps(APOLLO_PLUGIN_CAPS *c)
{
    DWORD ret = 0;

    memset(c, 0, sizeof(*c));
    if (!DeviceIoControl(g_dev, IOCTL_APOLLO_GET_PLUGIN_CAPS, NULL, 0, c, sizeof(*c), &ret, NULL) || ret < sizeof(*c)) {
        fprintf(stderr, "GET_PLUGIN_CAPS failed (error %lu): the running apollo.sys is not the P0 build?\n", GetLastError());
        return FALSE;
    }
    return TRUE;
}

/* One PLUGIN_INSTANCE call. Returns FALSE when the IOCTL itself failed (malformed, disabled, busy). */
static BOOL Instance(APOLLO_PLUGIN_INSTANCE *q, const UCHAR (*sha)[32], const PLUG_ROW *rows, const ULONG *cmd,
                     ULONG *values)
{
    size_t inLen = sizeof(*q) + (size_t)q->NResources * 32 + (size_t)q->NPrivate * sizeof(PLUG_ROW) +
                   (size_t)q->NCommands * 4;
    size_t outLen = sizeof(*q) + (size_t)q->ReadWords * 4;
    size_t len = inLen > outLen ? inLen : outLen;
    UCHAR *buf = (UCHAR *)calloc(1, len), *p;
    DWORD ret = 0;
    BOOL ok;

    if (buf == NULL) {
        return FALSE;
    }
    q->Size = sizeof(*q);
    p = buf;
    memcpy(p, q, sizeof(*q));
    p += sizeof(*q);
    if (q->NResources) {
        memcpy(p, sha, (size_t)q->NResources * 32);
    }
    p += (size_t)q->NResources * 32;
    if (q->NPrivate) {
        memcpy(p, rows, (size_t)q->NPrivate * sizeof(PLUG_ROW));
    }
    p += (size_t)q->NPrivate * sizeof(PLUG_ROW);
    if (q->NCommands) {
        memcpy(p, cmd, (size_t)q->NCommands * 4);
    }
    ok = DeviceIoControl(g_dev, IOCTL_APOLLO_PLUGIN_INSTANCE, buf, (DWORD)inLen, buf, (DWORD)outLen, &ret, NULL);
    if (!ok) {
        DWORD e = GetLastError();
        fprintf(stderr, "PLUGIN_INSTANCE op %lu: the IOCTL failed (error %lu%s)\n", q->Op, e,
                e == ERROR_NOT_SUPPORTED ? ": plug-in hosting is off (PluginHost) or not this unit" :
                e == ERROR_BUSY ? ": the host is busy (a strip pass?)" : "");
        free(buf);
        return FALSE;
    }
    memcpy(q, buf, sizeof(*q));
    if (values != NULL && q->ReadWords) {
        memcpy(values, buf + sizeof(*q), (size_t)q->ReadWords * 4);
    }
    free(buf);
    return TRUE;
}

static void PrintOutcome(const char *what, const APOLLO_PLUGIN_INSTANCE *q)
{
    printf("%s: status 0x%08lx %s, step %s, %lu ms; licence %s (word state only), handle %lu, DSP %lu, epoch %lu, "
           "%lu uploaded, %lu resident", what, (ULONG)q->Status, StatusText(q->Status), StepText(q->Step), q->ElapsedMs,
           LicText(q->LicenseState), q->Handle, q->Dsp, q->Epoch, q->Uploads, q->Resident);
    if (q->Status != 0 && (q->Detail[0] || q->Detail[1] || q->Detail[2] || q->Detail[3])) {
        printf("; detail %08lx %08lx %08lx %08lx", q->Detail[0], q->Detail[1], q->Detail[2], q->Detail[3]);
    }
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Recipe                                                              */
/* ------------------------------------------------------------------ */

typedef struct _OBS {
    ULONG Row, Word, Count;
    ULONG V[PLUG_READ_WORDS_MAX];
} OBS;

typedef struct _RECIPE {
    char    Name[160];
    char    Layout[16];
    char    Effect[16];         /* tail | delay | level ("" = an older recipe: delay if it has a shift, else level) */
    ULONG   RenderId, Latency, In1, In2, Scratch[3];
    LONG    ExpectShift;        /* record lag shift while the chain runs (p1_recipe.py), -99999 = not given */
    ULONG   NRes, NRows, NCmd, NPRes, NPRows, NObs;
    UCHAR   Res[PLUG_RES_MAX][32];
    PLUG_ROW Rows[PLUG_PRIV_MAX];
    ULONG  *Cmd;                /* RECIPE_CMD_MAX words: the whole startup */
    ULONG   NChunk;             /* upstream's page cut as p1_recipe.py wrote it ("chunk at words") */
    ULONG   ChunkAt[RECIPE_PAGES_MAX], ChunkLen[RECIPE_PAGES_MAX];
    UCHAR   PRes[PLUG_RES_MAX][32];
    PLUG_ROW PRows[PLUG_PRIV_MAX];
    OBS    *Obs;
    /* words the running DSP writes itself, never compared by a readback: the recipe's "dsp_owned <row>
       <word> <source>" lines (source bundle | meter | hardware, p1_recipe.py) first, then the words a run
       saw move between two reads (source "moving", this run only) */
    ULONG   NOwned, NOwnedRecipe;
    ULONG   OwnedRow[OWNED_MAX], OwnedWord[OWNED_MAX];
    char    OwnedSrc[OWNED_MAX][12];
} RECIPE;

static void FreeRecipe(RECIPE *r)
{
    free(r->Obs);
    free(r->Cmd);
    r->Obs = NULL;
    r->Cmd = NULL;
}

static int HexVal(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static BOOL ParseSha(const char *s, UCHAR out[32])
{
    int i;

    if (s == NULL || strlen(s) != 64) {
        return FALSE;
    }
    for (i = 0; i < 32; i++) {
        int h = HexVal(s[2 * i]), l = HexVal(s[2 * i + 1]);
        if (h < 0 || l < 0) {
            return FALSE;
        }
        out[i] = (UCHAR)(h * 16 + l);
    }
    return TRUE;
}

static void ShaHex(const UCHAR sha[32], char out[65])
{
    static const char hx[] = "0123456789abcdef";
    int i;

    for (i = 0; i < 32; i++) {
        out[2 * i] = hx[sha[i] >> 4];
        out[2 * i + 1] = hx[sha[i] & 15];
    }
    out[64] = 0;
}

static int StripPinOf(const UCHAR sha[32])
{
    int j;

    for (j = 0; j < APOLLO_STRIP_MODULES; j++) {
        if (memcmp(g_ApolloStripPins[j].Sha256, sha, 32) == 0) {
            return j;
        }
    }
    return -1;
}

static BOOL ParseU(const char *s, ULONG *v)
{
    char *end = NULL;

    if (s == NULL || *s == 0) {
        return FALSE;
    }
    *v = strtoul(s, &end, 0);
    return end != s && *end == 0;
}

static BOOL LoadRecipe(const char *path, RECIPE *r)
{
    FILE *f = NULL;
    char line[16384];
    int ln = 0;
    BOOL ok = TRUE, magic = FALSE;

    memset(r, 0, sizeof(*r));
    r->ExpectShift = -99999;
    r->Obs = (OBS *)calloc(OBS_MAX, sizeof(OBS));
    r->Cmd = (ULONG *)calloc(RECIPE_CMD_MAX, sizeof(ULONG));
    if (r->Obs == NULL || r->Cmd == NULL || fopen_s(&f, path, "r") != 0 || f == NULL) {
        fprintf(stderr, "Cannot read the recipe %s\n", path);
        FreeRecipe(r);
        return FALSE;
    }
    while (ok && fgets(line, sizeof(line), f) != NULL) {
        char *ctx = NULL, *kw, *t;
        ULONG v[4];

        ln++;
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '#' || line[0] == 0) {
            continue;
        }
        if (strncmp(line, "name ", 5) == 0) {
            strncpy_s(r->Name, sizeof(r->Name), line + 5, _TRUNCATE);
            continue;
        }
        kw = strtok_s(line, " \t", &ctx);
        if (kw == NULL) {
            continue;
        }
        if (strcmp(kw, "apollo-plug-recipe") == 0) {
            t = strtok_s(NULL, " \t", &ctx);
            magic = (t != NULL && strcmp(t, "1") == 0);
        } else if (strcmp(kw, "layout") == 0) {
            t = strtok_s(NULL, " \t", &ctx);
            strncpy_s(r->Layout, sizeof(r->Layout), t ? t : "", _TRUNCATE);
        } else if (strcmp(kw, "render_id") == 0) {
            ok = ParseU(strtok_s(NULL, " \t", &ctx), &r->RenderId);
        } else if (strcmp(kw, "latency") == 0) {
            ok = ParseU(strtok_s(NULL, " \t", &ctx), &r->Latency);
        } else if (strcmp(kw, "expect_shift") == 0) {
            t = strtok_s(NULL, " \t", &ctx);
            ok = t != NULL;
            if (ok) {
                r->ExpectShift = strtol(t, NULL, 0);
            }
        } else if (strcmp(kw, "synch") == 0) {
            ok = ParseU(strtok_s(NULL, " \t", &ctx), &r->In1) && ParseU(strtok_s(NULL, " \t", &ctx), &r->In2);
        } else if (strcmp(kw, "scratch") == 0) {
            ok = ParseU(strtok_s(NULL, " \t", &ctx), &r->Scratch[0]) && ParseU(strtok_s(NULL, " \t", &ctx), &r->Scratch[1]) &&
                 ParseU(strtok_s(NULL, " \t", &ctx), &r->Scratch[2]);
        } else if (strcmp(kw, "resource") == 0 || strcmp(kw, "plain_resource") == 0) {
            BOOL plain = kw[0] == 'p';
            ULONG *n = plain ? &r->NPRes : &r->NRes;
            ok = *n < PLUG_RES_MAX && ParseSha(strtok_s(NULL, " \t", &ctx), plain ? r->PRes[*n] : r->Res[*n]);
            (*n)++;
        } else if (strcmp(kw, "row") == 0 || strcmp(kw, "plain_row") == 0) {
            BOOL plain = kw[0] == 'p';
            ULONG *n = plain ? &r->NPRows : &r->NRows;
            PLUG_ROW *row = plain ? &r->PRows[*n] : &r->Rows[*n];
            ok = *n < PLUG_PRIV_MAX && ParseU(strtok_s(NULL, " \t", &ctx), &v[0]) && ParseU(strtok_s(NULL, " \t", &ctx), &v[1]) &&
                 ParseU(strtok_s(NULL, " \t", &ctx), &v[2]) && ParseU(strtok_s(NULL, " \t", &ctx), &v[3]);
            if (ok) {
                row->Pool = v[0]; row->Dwords = v[1]; row->Link = v[2]; row->Flags = v[3];
                (*n)++;
            }
        } else if (strcmp(kw, "cmd") == 0) {
            while (ok && (t = strtok_s(NULL, " \t", &ctx)) != NULL) {
                ok = r->NCmd < RECIPE_CMD_MAX && ParseU(t, &r->Cmd[r->NCmd]);
                r->NCmd++;
            }
        } else if (strcmp(kw, "chunk") == 0) {
            ok = r->NChunk < RECIPE_PAGES_MAX && ParseU(strtok_s(NULL, " \t", &ctx), &r->ChunkAt[r->NChunk]) &&
                 ParseU(strtok_s(NULL, " \t", &ctx), &r->ChunkLen[r->NChunk]);
            r->NChunk++;
        } else if (strcmp(kw, "effect") == 0) {
            t = strtok_s(NULL, " \t", &ctx);
            ok = t != NULL && (strcmp(t, "tail") == 0 || strcmp(t, "delay") == 0 || strcmp(t, "level") == 0);
            if (ok) {
                strncpy_s(r->Effect, sizeof(r->Effect), t, _TRUNCATE);
            }
        } else if (strcmp(kw, "observe") == 0) {
            OBS *o = &r->Obs[r->NObs];
            ULONG k = 0;
            ok = r->NObs < OBS_MAX && ParseU(strtok_s(NULL, " \t", &ctx), &o->Row) &&
                 ParseU(strtok_s(NULL, " \t", &ctx), &o->Word) && ParseU(strtok_s(NULL, " \t", &ctx), &o->Count) &&
                 o->Count >= 1 && o->Count <= PLUG_READ_WORDS_MAX;
            while (ok && k < o->Count) {
                ok = ParseU(strtok_s(NULL, " \t", &ctx), &o->V[k]);
                k++;
            }
            r->NObs++;
        } else if (strcmp(kw, "dsp_owned") == 0) {
            ok = r->NOwned < OWNED_MAX && ParseU(strtok_s(NULL, " \t", &ctx), &r->OwnedRow[r->NOwned]) &&
                 ParseU(strtok_s(NULL, " \t", &ctx), &r->OwnedWord[r->NOwned]);
            if (ok) {
                t = strtok_s(NULL, " \t", &ctx);
                strncpy_s(r->OwnedSrc[r->NOwned], sizeof(r->OwnedSrc[0]), t ? t : "?", _TRUNCATE);
                r->NOwned++;
            }
        } else {
            /* other keywords (module, dll, bundle, ...) are information only */
        }
    }
    fclose(f);
    if (!ok || !magic) {
        fprintf(stderr, "%s line %d: %s\n", path, ln, magic ? "not understood" : "not an apollo-plug recipe (version 1)");
        FreeRecipe(r);
        return FALSE;
    }
    r->NOwnedRecipe = r->NOwned;
    if (r->Effect[0] == 0) {
        strcpy_s(r->Effect, sizeof(r->Effect), (r->ExpectShift != -99999 && r->ExpectShift != 0) ? "delay" : "level");
    }
    return TRUE;
}

/*
 * The startup's pages as the staged path sends them: PlugChunkNext (upstream's chunks(blocks()),
 * native_carrier.py:255) over the whole startup. One page when it fits 1016 words. FALSE when a
 * block is malformed or longer than one page.
 */
static BOOL Pages(const RECIPE *r, ULONG *At, ULONG *Len, ULONG *N)
{
    ULONG at = 0, n = 0, len;

    *N = 0;
    while ((len = PlugChunkNext(r->Cmd, r->NCmd, at, PLUG_CMD_WORDS_MAX)) != 0) {
        if (len == PLUG_NONE || n >= RECIPE_PAGES_MAX) {
            return FALSE;
        }
        At[n] = at;
        Len[n++] = len;
        at += len;
    }
    *N = n;
    return n != 0 && at == r->NCmd;
}

/* Does the recipe's own page cut (upstream's, from p1_recipe.py) equal ours? */
static BOOL PagesMatchUpstream(const RECIPE *r, const ULONG *At, const ULONG *Len, ULONG N)
{
    ULONG k;

    if (r->NChunk != N) {
        return FALSE;
    }
    for (k = 0; k < N; k++) {
        if (r->ChunkAt[k] != At[k] || r->ChunkLen[k] != Len[k]) {
            return FALSE;
        }
    }
    return TRUE;
}

/*
 * Observation runs I.. that one SERVICE read covers: one row, ascending, the whole span at most
 * 64 words (inside the row, as each run is). Returns the index after the group (ReadGroupSpan: the
 * words to read).
 */
static ULONG ReadGroupEnd(const RECIPE *r, ULONG I)
{
    const OBS *o = &r->Obs[I];
    ULONG j = I + 1, end = o->Word + o->Count;

    while (j < r->NObs) {
        const OBS *nx = &r->Obs[j];
        ULONG e2 = nx->Word + nx->Count;

        if (nx->Row != o->Row || nx->Word < r->Obs[j - 1].Word || (e2 > end ? e2 : end) - o->Word > PLUG_READ_WORDS_MAX) {
            break;
        }
        if (e2 > end) {
            end = e2;
        }
        j++;
    }
    return j;
}

static ULONG ReadGroupSpan(const RECIPE *r, ULONG I, ULONG J)
{
    ULONG k, end = 0;

    for (k = I; k < J; k++) {
        if (r->Obs[k].Word + r->Obs[k].Count > end) {
            end = r->Obs[k].Word + r->Obs[k].Count;
        }
    }
    return end - r->Obs[I].Word;
}

/* ---- the recipe's inputs: mono, or a stereo pair ---- */

/* Mono on one of the unit's strip inputs (in2 0x20), or a linked pair in1 / in1 + 1 there (x8p 1-8, Twin X 1-2). */
static BOOL InputsOk(const RECIPE *r)
{
    if (PlugModelSynch(g_model, r->In1 | (r->In2 << 16)) != PLUG_OK) {
        return FALSE;
    }
    if (strcmp(r->Layout, "mono") == 0) {
        return r->In2 == PLUG_SYNCH_MONO;
    }
    if (strcmp(r->Layout, "stereo") == 0) {
        return r->In2 == r->In1 + 1;
    }
    return FALSE;
}

static BOOL Stereo(const RECIPE *r)
{
    return r->In2 != PLUG_SYNCH_MONO;
}

/* The chain's inputs as CAPS StripsStarted bits (InputsOk first). */
static ULONG InMask(const RECIPE *r)
{
    return (1u << r->In1) | (Stereo(r) ? (1u << r->In2) : 0u);
}

/* "1" or "1/2": the chain's inputs. */
static const char *InNums(const RECIPE *r)
{
    static char t[24];

    if (Stereo(r)) sprintf_s(t, sizeof(t), "%lu/%lu", r->In1 + 1, r->In2 + 1);
    else sprintf_s(t, sizeof(t), "%lu", r->In1 + 1);
    return t;
}

/* "input 1" or "inputs 1/2". */
static const char *InText(const RECIPE *r)
{
    static char t[32];

    sprintf_s(t, sizeof(t), "input%s %s", Stereo(r) ? "s" : "", InNums(r));
    return t;
}

/* ---- DSP-owned words: never compared by a readback ---- */

static BOOL IsOwned(const RECIPE *r, ULONG Row, ULONG Word)
{
    ULONG k;

    for (k = 0; k < r->NOwned; k++) {
        if (r->OwnedRow[k] == Row && r->OwnedWord[k] == Word) {
            return TRUE;
        }
    }
    return FALSE;
}

/* Row R word W is DSP-owned for the rest of this run (seen moving: upstream learns it per instance). */
static void LearnOwned(RECIPE *r, ULONG Row, ULONG Word)
{
    if (r->NOwned < OWNED_MAX && !IsOwned(r, Row, Word)) {
        r->OwnedRow[r->NOwned] = Row;
        r->OwnedWord[r->NOwned] = Word;
        strcpy_s(r->OwnedSrc[r->NOwned], sizeof(r->OwnedSrc[0]), "moving");
        r->NOwned++;
    }
}

/* "row 10 word 5 (hardware), row 10 word 45 (meter)": the first N DSP-owned words. */
static void OwnedText(const RECIPE *r, ULONG N, char *Buf, size_t Cap)
{
    size_t at = 0;
    ULONG k;

    Buf[0] = 0;
    for (k = 0; k < N && k < r->NOwned; k++) {
        int w = _snprintf_s(Buf + at, Cap - at, _TRUNCATE, "%srow %lu word %lu (%s)", k ? ", " : "", r->OwnedRow[k],
                            r->OwnedWord[k], r->OwnedSrc[k]);
        if (w < 0) {
            break;                  /* full: the count says how many there are */
        }
        at += (size_t)w;
    }
}

/*
 * One readback word (create-start and selftest use the same rule): a DSP-owned word is never
 * compared; a word as written is the same; a word that differs and changed between two reads is
 * the running DSP code's (a meter, a ramp, a pointer: oa:mixer-engine/plugin_native.py:214-223);
 * the same wrong value twice, or no second read, is a difference (a WRITE that failed reads back
 * the same wrong value both times).
 */
#define RB_SAME             0
#define RB_OWNED            1
#define RB_MOVES            2
#define RB_DIFFERS          3

static int ReadbackWord(BOOL Owned, ULONG Want, ULONG First, BOOL HaveSecond, ULONG Second)
{
    if (Owned) {
        return RB_OWNED;
    }
    if (First == Want) {
        return RB_SAME;
    }
    return (HaveSecond && Second != First) ? RB_MOVES : RB_DIFFERS;
}

typedef struct _RB {
    ULONG   Runs, Words, Bad, Fail, Reads;  /* runs read, words compared, words that differ, runs not read, SERVICE reads */
    ULONG   Owned, Moving;                  /* not compared: the DSP-owned words read, the words seen moving (learned) */
} RB;

/* Words of runs I..J-1 (one read group, V read from the group's first word) that differ, DSP-owned ones aside. */
static ULONG GroupDiffs(const RECIPE *r, ULONG I, ULONG J, const ULONG *V)
{
    ULONG k, m, nd = 0, w0 = r->Obs[I].Word;

    for (k = I; k < J; k++) {
        const OBS *ob = &r->Obs[k];

        for (m = 0; m < ob->Count; m++) {
            if (!IsOwned(r, ob->Row, ob->Word + m) && V[ob->Word - w0 + m] != ob->V[m]) {
                nd++;
            }
        }
    }
    return nd;
}

/*
 * Runs I..J-1 of one read group into C: V the first read, V2 a second read of the same span (NULL:
 * none). A word seen moving is learned (not compared again in this run) and reported.
 */
static void GroupClassify(RECIPE *r, ULONG I, ULONG J, const ULONG *V, const ULONG *V2, RB *C, BOOL Quiet)
{
    ULONG k, m, w0 = r->Obs[I].Word;

    for (k = I; k < J; k++) {
        const OBS *ob = &r->Obs[k];

        C->Runs++;
        for (m = 0; m < ob->Count; m++) {
            ULONG at = ob->Word - w0 + m, w = ob->Word + m;

            switch (ReadbackWord(IsOwned(r, ob->Row, w), ob->V[m], V[at], V2 != NULL, V2 != NULL ? V2[at] : 0)) {
            case RB_OWNED:
                C->Owned++;
                break;
            case RB_MOVES:
                C->Moving++;
                LearnOwned(r, ob->Row, w);
                if (!Quiet) {
                    printf("  row %lu word %lu: written 0x%08lx, read 0x%08lx then 0x%08lx\n", ob->Row, w, ob->V[m], V[at], V2[at]);
                    Info("row %lu word %lu moves while the DSP runs: DSP-owned, not compared (add it to plugins\\p1\\dsp-owned.txt)",
                         ob->Row, w);
                }
                break;
            case RB_DIFFERS:
                C->Words++;
                C->Bad++;
                if (!Quiet && C->Bad <= 8) {
                    if (V2 != NULL) printf("  differs: row %lu word %lu: written 0x%08lx, read 0x%08lx twice\n", ob->Row, w, ob->V[m], V[at]);
                    else printf("  differs: row %lu word %lu: written 0x%08lx, read 0x%08lx\n", ob->Row, w, ob->V[m], V[at]);
                }
                break;
            default:
                C->Words++;
                break;
            }
        }
    }
}

/* "" or "; not compared: 2 DSP-owned, 0 seen moving" */
static const char *RbSkipped(const RB *C)
{
    static char t[96];

    t[0] = 0;
    if (C->Owned != 0 || C->Moving != 0) {
        sprintf_s(t, sizeof(t), "; not compared: %lu DSP-owned, %lu seen moving", C->Owned, C->Moving);
    }
    return t;
}

/* ---- the strip DSP's ledger as apollo.sys builds it ---- */

/* The x8p's DSP 1 pool registers, measured Nov 14 (docs/STRIPS.md), in CAPS Regs order: size 0-3, base 0-3, scratch 1-3;
   [P7] each model's are in plugin_plan.h (the Twin X's from step-g5.log) */
#define g_x8pDsp1Regs   (PlugModelFor(PLUG_MODEL_X8P)->StripDspRegs)

/*
 * DSP D's ledger as driver\plugin.c PlugLedgerFor builds it the first time in an epoch: PlugLedgerInit
 * from the pool registers (CAPS Regs: sizes 0-3, bases 0-3, scratch 1-3); on the strip DSP the strips'
 * scratch floor and room (PlugLedgerBookStrips, the unit's strips: x8p 8, Twin X 2), which must land where the strip plan puts
 * them. PLUG_OK, or the ledger's error (not a usable pool map). [Virtual] The room includes the Virtual Input Strips the unit
 * built (g_virtualMask, from its GET_STRIPS version 4; 0 offline and for an older driver), as plugin.c books them.
 * [AuxStrips] And its aux-return Input Strips (g_auxMask, GET_STRIPS version 5; 0 offline and for an older driver).
 * [TalkStrip] And its talkback Input Strip (g_talkback, GET_STRIPS version 6; 0 offline and for an older driver).
 */
static ULONG g_virtualMask;
static ULONG g_auxMask;
static ULONG g_talkback;

static ULONG BuildLedger(PLUG_LEDGER *L, const ULONG Regs[11], BOOL StripDsp, PLUG_LEASE Book[4])
{
    ULONG size[4], base[4], scr[4], p, err, room[4];

    for (p = 0; p < 4; p++) {
        size[p] = Regs[p];
        base[p] = Regs[4 + p];
        scr[p] = p ? Regs[7 + p] : 0;
    }
    err = PlugLedgerInit(L, base, size, scr);
    if (err == PLUG_OK && StripDsp) {
        StripPlanRoomTalkback(g_model->Strips, g_virtualMask, g_auxMask, g_talkback, room);
        err = PlugLedgerBookStripRoom(L, STRIP_CODE_WORDS, room, Book);
        if (err == PLUG_OK && (Book[0].Base != APOLLO_STRIP_CODE_BASE || Book[1].Base + Book[1].Words != STRIP_TOP1 ||
                               Book[2].Base + Book[2].Words != STRIP_TOP2 || Book[3].Base + Book[3].Words != STRIP_TOP3)) {
            err = PLUG_EINVAL;
        }
    }
    return err;
}

static void UsageToCaps(const PLUG_USAGE U[4], APOLLO_PLUGIN_POOL_USAGE Out[4])
{
    ULONG p;

    for (p = 0; p < 4; p++) {
        Out[p].Base = U[p].Base;
        Out[p].Total = U[p].Total;
        Out[p].Scratch = U[p].Scratch;
        Out[p].Used = U[p].Used;
        Out[p].Free = U[p].Free;
        Out[p].Largest = U[p].Largest;
        Out[p].Allocations = U[p].Allocations;
        Out[p].Holds = U[p].Holds;
    }
}

/*
 * The idle ledger (on the strip DSP: the strips' room only) as CAPS would report it, from the pool
 * registers: the baseline a restore returns to when CAPS had no ledger before the CREATE (apollo.sys
 * builds it inside the first CREATE after a power cycle). FALSE: not a usable pool map.
 */
static BOOL LedgerBaseline(const ULONG Regs[11], BOOL StripDsp, APOLLO_PLUGIN_POOL_USAGE Out[4])
{
    static PLUG_LEDGER L;
    PLUG_LEASE book[4];
    PLUG_USAGE u[4];

    memset(Out, 0, 4 * sizeof(*Out));
    if (BuildLedger(&L, Regs, StripDsp, book) != PLUG_OK) {
        return FALSE;
    }
    PlugLedgerUsage(&L, u);
    UsageToCaps(u, Out);
    return TRUE;
}

/* Used, Allocations, Scratch and Holds of the four pools (what a restore must give back). */
static BOOL SameUsage(const APOLLO_PLUGIN_POOL_USAGE *x, const APOLLO_PLUGIN_POOL_USAGE *y)
{
    ULONG p;

    for (p = 0; p < 4; p++) {
        if (x[p].Used != y[p].Used || x[p].Allocations != y[p].Allocations || x[p].Scratch != y[p].Scratch ||
            x[p].Holds != y[p].Holds) {
            return FALSE;
        }
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Module files                                                        */
/* ------------------------------------------------------------------ */

static UCHAR *ReadFileBytes(const WCHAR *path, DWORD *size)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER sz;
    UCHAR *buf;
    DWORD got = 0;

    *size = 0;
    if (h == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (LONGLONG)PLUG_BILL_MAX) {
        CloseHandle(h);
        SetLastError(ERROR_FILE_TOO_LARGE);
        return NULL;
    }
    buf = (UCHAR *)malloc((size_t)sz.QuadPart);
    if (buf == NULL || !ReadFile(h, buf, (DWORD)sz.QuadPart, &got, NULL) || got != (DWORD)sz.QuadPart) {
        free(buf);
        CloseHandle(h);
        return NULL;
    }
    CloseHandle(h);
    *size = got;
    return buf;
}

static BOOL Sha256(const UCHAR *data, DWORD len, UCHAR out[32])
{
    static BCRYPT_ALG_HANDLE alg;       /* opened once: a provider open costs more than hashing a record */

    if (alg == NULL && BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) {
        alg = NULL;
        return FALSE;
    }
    return BCryptHash(alg, NULL, 0, (PUCHAR)data, len, out, 32) == 0;
}

/* A record: read, its SHA-256 = Sha, a Bill record UAD2System would register. */
static BOOL CheckRecord(const WCHAR *path, const UCHAR sha[32], PLUG_BILL *b, UCHAR **data, DWORD *len)
{
    UCHAR h[32];

    *data = ReadFileBytes(path, len);
    if (*data == NULL) {
        return FALSE;
    }
    if (!Sha256(*data, *len, h) || memcmp(h, sha, 32) != 0 || PlugBillCheck(*data, *len, b) != PLUG_OK) {
        free(*data);
        *data = NULL;
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    return TRUE;
}

static BOOL ModuleDir(WCHAR *dir, size_t cch)
{
    WCHAR win[MAX_PATH];
    UINT n = GetSystemWindowsDirectoryW(win, MAX_PATH);

    return n > 0 && n < MAX_PATH && swprintf_s(dir, cch, L"%s\\%s", win, APOLLO_MODULE_DIR_UNDER_ROOT) >= 0;
}

static DWORD SetDacl(WCHAR *path, const WCHAR *sddl)
{
    PSECURITY_DESCRIPTOR sd = NULL;
    PACL dacl = NULL;
    BOOL present = FALSE, defaulted = FALSE;
    DWORD err;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, NULL)) {
        return GetLastError();
    }
    if (!GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) || !present) {
        LocalFree(sd);
        return ERROR_INVALID_SECURITY_DESCR;
    }
    err = SetNamedSecurityInfoW(path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                NULL, NULL, dacl, NULL);
    LocalFree(sd);
    return err;
}

/* The recipe's modules (chain + plain), the pinned strip modules excepted (ApolloStrips has them). */
static ULONG RecipeModules(const RECIPE *r, UCHAR (*out)[32])
{
    ULONG n = 0, i, j;

    for (i = 0; i < r->NRes + r->NPRes; i++) {
        const UCHAR *s = i < r->NRes ? r->Res[i] : r->PRes[i - r->NRes];
        BOOL dup = FALSE;

        if (StripPinOf(s) >= 0) {
            continue;
        }
        for (j = 0; j < n; j++) {
            dup |= memcmp(out[j], s, 32) == 0;
        }
        if (!dup) {
            memcpy(out[n++], s, 32);
        }
    }
    return n;
}

static int CmdModules(int n, char **a)
{
    static RECIPE r;
    static UCHAR list[2 * PLUG_RES_MAX][32];
    WCHAR dir[MAX_PATH], src[MAX_PATH], dst[MAX_PATH], from[MAX_PATH];
    BOOL install;
    ULONG count, i, bad = 0;
    int k;

    if (n < 3 || (_stricmp(a[1], "install") != 0 && _stricmp(a[1], "verify") != 0)) {
        fprintf(stderr, "usage: modules install <recipe> --from DIR | modules verify <recipe>\n");
        return 2;
    }
    install = _stricmp(a[1], "install") == 0;
    if (!LoadRecipe(a[2], &r)) {
        return 2;
    }
    from[0] = 0;
    for (k = 3; k + 1 < n; k++) {
        if (strcmp(a[k], "--from") == 0 && MultiByteToWideChar(CP_ACP, 0, a[k + 1], -1, from, MAX_PATH) == 0) {
            from[0] = 0;
        }
    }
    if (install && from[0] == 0) {
        fprintf(stderr, "modules install needs --from DIR (plugins\\store\\modules)\n");
        return 2;
    }
    if (!ModuleDir(dir, MAX_PATH)) {
        return 2;
    }
    count = RecipeModules(&r, list);
    if (install) {
        if (!CreateDirectoryW(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
            fprintf(stderr, "Creating %ls failed (error %lu): run elevated.\n", dir, GetLastError());
            return 2;
        }
        if (SetDacl(dir, MOD_DIR_SDDL) != ERROR_SUCCESS) {
            fprintf(stderr, "Making %ls admin-only failed: run elevated.\n", dir);
            return 2;
        }
    }
    printf("%s %lu module(s) of %s (the 5 Mic strip modules come from ApolloStrips) in %ls\n",
           install ? "Installing" : "Checking", count, r.Name, dir);
    for (i = 0; i < count; i++) {
        char hex[65];
        PLUG_BILL b;
        UCHAR *data = NULL;
        DWORD len = 0;

        ShaHex(list[i], hex);
        swprintf_s(dst, MAX_PATH, L"%s\\%hs.bill", dir, hex);
        if (install) {
            HANDLE h;
            DWORD wrote = 0;

            swprintf_s(src, MAX_PATH, L"%s\\%hs.bill", from, hex);
            if (!CheckRecord(src, list[i], &b, &data, &len)) {
                printf("  %.16s..  source missing or not its hash (error %lu)\n", hex, GetLastError());
                bad++;
                continue;
            }
            if (GetFileAttributesW(dst) != INVALID_FILE_ATTRIBUTES) {
                (void)SetDacl(dst, MOD_FILE_SDDL);
            }
            h = CreateFileW(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h == INVALID_HANDLE_VALUE || !WriteFile(h, data, len, &wrote, NULL) || wrote != len) {
                printf("  %.16s..  could not be written (error %lu)\n", hex, GetLastError());
                if (h != INVALID_HANDLE_VALUE) {
                    CloseHandle(h);
                }
                free(data);
                bad++;
                continue;
            }
            CloseHandle(h);
            (void)SetDacl(dst, MOD_FILE_SDDL);
            free(data);
            data = NULL;
        }
        if (!CheckRecord(dst, list[i], &b, &data, &len)) {
            DWORD e = GetLastError();
            printf("  %.16s..  %s\n", hex, e == ERROR_ACCESS_DENIED ? "cannot be read without elevation (admin-only)" :
                   e == ERROR_FILE_NOT_FOUND ? "MISSING" : "NOT its hash or not a valid record");
            bad++;
            continue;
        }
        printf("  %.16s..  id 0x%03lx kind %lu arch %lu, %lu bytes, %lu pool-%lu words: ok\n", hex, b.Id, b.Kind, b.Arch,
               len, b.Words, b.Pool);
        free(data);
    }
    if (bad == 0) {
        Pass("module store: %lu module(s) of %s installed and verified (SHA-256, Bill header)", count, r.Name);
    } else {
        Check("module store: %lu of %lu module(s) missing or wrong", bad, count);
    }
    return bad ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* check: the driver's validation and encodings, offline               */
/* ------------------------------------------------------------------ */

static UCHAR g_pg[PLUG_PAY_PAGES_MAX][PLUG_PAGE];

static int CmdCheck(int n, char **a)
{
    static RECIPE r;
    static PLUG_LEDGER L;
    static PLUG_XL x;
    static ULONG s2[2 * PLUG_PRIV_MAX], s1[PLUG_PRIV_MAX], reloc[PLUG_CMD_WORDS_MAX], addr[PLUG_PRIV_MAX];
    static PLUG_REQ rq[PLUG_RES_MAX + PLUG_PRIV_MAX];
    static PLUG_LEASE got[PLUG_RES_MAX + PLUG_PRIV_MAX];
    static PLUG_BILL bills[PLUG_RES_MAX];
    static ULONG pgAt[RECIPE_PAGES_MAX], pgLen[RECIPE_PAGES_MAX];
    static char owned[4096];
    const char *store = NULL;
    PLUG_LEASE book[4];
    PLUG_PAY pay;
    ULONG ncmds = 0, ids[5], nids = 0, carrier = 0, i, m = 0, nb = 0, e, npg = 0, reads = 0, nin;
    BOOL paged, staged, stereo;
    int k;

    if (n < 2 || !LoadRecipe(a[1], &r)) {
        fprintf(stderr, "usage: check <recipe> [--store DIR]\n");
        return 2;
    }
    for (k = 2; k + 1 < n; k++) {
        if (strcmp(a[k], "--store") == 0) {
            store = a[k + 1];
        }
    }
    printf("Recipe %s (%s): render id 0x%03lx, latency %lu, synch %lu/%lu, scratch %lu %lu %lu; chain %lu modules, "
           "%lu rows, %lu command words; plain %lu modules, %lu rows; %lu readback runs\n", r.Name, r.Layout, r.RenderId,
           r.Latency, r.In1, r.In2, r.Scratch[0], r.Scratch[1], r.Scratch[2], r.NRes, r.NRows, r.NCmd, r.NPRes, r.NPRows, r.NObs);
    stereo = Stereo(&r);
    nin = stereo ? 2 : 1;
    if (InputsOk(&r) && !stereo) {
        Pass("mono, %s input %lu (DSP input %lu, in2 0x20)", g_model->Name, r.In1 + 1, r.In1);
    } else if (InputsOk(&r)) {
        Pass("stereo, %s inputs %lu/%lu (DSP inputs %lu/%lu, synch in1 in1+1): create-start needs FEAT_STEREO and both "
             "plain strips live; the effect is measured on input %lu", g_model->Name, r.In1 + 1, r.In2 + 1, r.In1, r.In2, r.In1 + 1);
    } else {
        Check("layout %s synch %lu/%lu: neither mono (in2 0x20) on a %s strip input (1-%lu) nor a linked pair in1/in1+1 there",
              r.Layout, r.In1, r.In2, g_model->Name, g_model->Inputs);
        stereo = FALSE;
        nin = 1;
    }
    if (r.RenderId >= PLUG_BQ_BASE && r.RenderId < PLUG_BQ_BASE + PLUG_BQ_WORDS) {
        Pass("render id 0x%03lx is in the licence table (index %lu)", r.RenderId, r.RenderId - PLUG_BQ_BASE);
    } else {
        Check("render id 0x%03lx outside the licence table", r.RenderId);
    }
    paged = r.NCmd >= 4 && Pages(&r, pgAt, pgLen, &npg);
    staged = paged && npg > 1;
    if (paged && !staged) {
        Pass("startup: %lu words, one command page (limit %u)", r.NCmd, PLUG_CMD_WORDS_MAX);
    } else if (staged) {
        Pass("startup: %lu words in %lu pages of whole WRITE blocks (staged: CREATE STAGE+CHAIN with page 0, %lu SERVICE "
             "writes, then START)", r.NCmd, npg, npg - 1);
    } else {
        Check("startup: %lu words cannot be cut into pages of whole WRITE blocks of at most %u words", r.NCmd,
              PLUG_CMD_WORDS_MAX);
    }
    if (paged && r.NChunk != 0) {
        if (PagesMatchUpstream(&r, pgAt, pgLen, npg)) Pass("page cut identical to upstream's chunks(blocks()) (%lu page(s))", npg);
        else Check("page cut differs from upstream's (recipe %lu page(s), ours %lu)", r.NChunk, npg);
    } else if (staged) {
        Check("the recipe carries no upstream page cut (chunk lines): make it again with this p1_recipe.py");
    }
    Info("effect measured as: %s; latency %lu, record lag shift expected %ld", r.Effect, r.Latency, r.ExpectShift);
    if (r.NRes >= 1 && r.NRes <= PLUG_RES_MAX && r.NRows >= 1 && r.NRows <= PLUG_PRIV_MAX && r.NPRes >= 1 && r.NPRows >= 1) {
        Pass("sizes inside the CREATE limits (64 modules, 128 rows)");
    } else {
        Check("sizes outside the CREATE limits");
    }
    e = PlugCheckRows(r.Rows, r.NRows, s2, s1);
    if (e == PLUG_OK) Pass("chain rows: one valid descriptor (driver check_rows)"); else Check("chain rows refused (%lu)", e);
    e = PlugCheckRows(r.PRows, r.NPRows, s2, s1);
    if (e == PLUG_OK) Pass("plain rows: one valid descriptor"); else Check("plain rows refused (%lu)", e);
    e = PlugCheckCmds(r.Rows, r.NRows, r.Cmd, r.NCmd, &ncmds);
    if (e == PLUG_OK) Pass("WRITEs: %lu blocks inside their rows, clear of every linked word (driver check_cmds)", ncmds);
    else Check("WRITEs refused by the driver's check_cmds (%lu)", e);
    /* the directory as the driver sees it at CREATE: the whole startup, or page 0 of a staged one */
    e = PlugChainIds(r.Rows, r.NRows, r.Cmd, staged ? pgLen[0] : r.NCmd, 1, r.RenderId, ids, &nids, &carrier);
    if (e == PLUG_OK && nids == 1 && ids[0] == r.RenderId && carrier == PLUG_MIC_CARRIER) {
        Pass("chain directory%s: Mic carrier 0x846 + effect 0x%03lx (one licence answer admits it)", staged ? " in page 0" : "",
             ids[0]);
    } else {
        Check("chain directory refused (%lu, %lu ids, carrier 0x%lx)", e, nids, carrier);
    }
    if (PlugWritesDirectory(r.Cmd, staged ? pgLen[0] : r.NCmd)) {
        Info("the startup writes the head directory (expected at CREATE; SERVICE may not)");
    }
    if (staged) {
        ULONG bad = 0, c = 0, dir = 0;

        for (i = 0; i < npg; i++) {
            if (PlugCheckCmds(r.Rows, r.NRows, r.Cmd + pgAt[i], pgLen[i], &c) != PLUG_OK || pgLen[i] < 4) {
                bad++;
            }
            if (i > 0 && PlugWritesDirectory(r.Cmd + pgAt[i], pgLen[i])) {
                dir++;
            }
        }
        if (bad == 0) Pass("every page passes the driver's check_cmds on its own (as each SERVICE write is checked)");
        else Check("%lu page(s) refused by check_cmds", bad);
        if (dir == 0) Pass("pages 1..%lu leave the chain directory alone (SERVICE refuses a directory write)", npg - 1);
        else Check("%lu later page(s) write the chain directory: the driver would refuse them", dir);
    }

    /* modules: the store copies, as the driver will check them */
    for (i = 0; i < r.NRes; i++) {
        int pin = StripPinOf(r.Res[i]);
        if (pin >= 0) {
            m++;
            continue;
        }
        if (store != NULL) {
            WCHAR path[MAX_PATH];
            char hex[65];
            UCHAR *data = NULL;
            DWORD len = 0;

            ShaHex(r.Res[i], hex);
            swprintf_s(path, MAX_PATH, L"%hs\\%hs.bill", store, hex);
            if (CheckRecord(path, r.Res[i], &bills[nb], &data, &len)) {
                printf("  module %.16s.. id 0x%03lx kind %lu arch %lu pool %lu, %lu words, %lu bytes\n", hex, bills[nb].Id,
                       bills[nb].Kind, bills[nb].Arch, bills[nb].Pool, bills[nb].Words, len);
                rq[nb].Kind = bills[nb].Pool;
                rq[nb].Words = bills[nb].Words;
                rq[nb].Identity = 100 + nb;
                rq[nb].Shared = 1;
                nb++;
                free(data);
            } else {
                Check("module %.16s.. missing from %s or not its hash", hex, store);
            }
        }
    }
    if (m == STRIP_MODULE_IDS) {
        Pass("the 5 Mic Input Strip modules are the pinned ones (shared with the strips on DSP 1, not uploaded)");
    } else {
        Check("%lu of the 5 pinned strip modules in the chain resources", m);
    }
    if (store != NULL) {
        e = PlugCheckSet(bills, nb);
        if (e == PLUG_OK && nb + m == r.NRes) Pass("module set uploadable: %lu plug-in module(s), kinds/archs/no duplicates", nb);
        else Check("module set refused (%lu) or incomplete (%lu of %lu)", e, nb + m, r.NRes);
    }

    /* placement on the unit's strip DSP beside its strips (x8p 8, Twin X 2; its measured pools, the ledger as the
       driver builds it), and the transactions the driver builds */
    e = BuildLedger(&L, g_model->StripDspRegs, TRUE, book);
    if (e == PLUG_OK) e = PlugLedgerSetScratch(&L, 1, r.Scratch[0]);
    if (e == PLUG_OK) e = PlugLedgerSetScratch(&L, 2, r.Scratch[1]);
    if (e == PLUG_OK) e = PlugLedgerSetScratch(&L, 3, r.Scratch[2]);
    for (i = 0; i < r.NRows; i++) {
        rq[nb + i].Kind = r.Rows[i].Pool;
        rq[nb + i].Words = r.Rows[i].Dwords;
        rq[nb + i].Identity = 0;
        rq[nb + i].Shared = 0;
    }
    if (e == PLUG_OK) e = PlugLedgerReserveBatch(&L, rq, nb + r.NRows, got);
    if (e == PLUG_OK) {
        PLUG_USAGE u[4];
        PlugLedgerUsage(&L, u);
        Pass("fits the %s's DSP %lu beside its %lu strips (measured pools): code %lu of %lu words used, pool 1 %lu, pool 2 %lu, "
             "pool 3 %lu", g_model->Name, g_model->StripDsp, g_model->Strips, u[0].Used, u[0].Total, u[1].Used, u[2].Used, u[3].Used);
        Info("DSP %lu room left after it: code %lu words (largest %lu), pool 1 %lu, pool 2 %lu, pool 3 %lu; scratch now %lu / %lu / "
             "%lu", g_model->StripDsp, u[0].Free, u[0].Largest, u[1].Free, u[2].Free, u[3].Free, u[1].Scratch, u[2].Scratch,
             u[3].Scratch);
        for (i = 0; i < r.NRows; i++) {
            addr[i] = got[nb + i].Base;
        }
        Info("chain head would be at 0x%05lx (pool 1)", addr[0]);
    } else {
        Check("does not fit the %s's DSP %lu beside its strips (ledger %lu)%s", g_model->Name, g_model->StripDsp, e,
              g_model->Dsps > 2 ? ": the unrouted test on DSP 2 (create-plain --dsp 2) is the first step for it" : "");
        for (i = 0; i < r.NRows; i++) {
            addr[i] = 0x1000 + 2 * i;
        }
    }
    memset(&pay, 0, sizeof(pay));
    pay.NPages = PLUG_PAY_PAGES_MAX;
    for (i = 0; i < PLUG_PAY_PAGES_MAX; i++) {
        pay.Page[i] = g_pg[i];
    }
    PlugXlInit(&x, PLUG_PAGE);
    e = PlugBuildPrivates(&x, &pay, r.Rows, r.NRows, addr, s2);
    if (e == PLUG_OK) Pass("privates transaction: %lu entries (%lu zero-fills + memspec %lu words)", x.N, r.NRows, 2 * r.NRows - 1);
    else Check("privates transaction not buildable (%lu)", e);
    /* entries: a stop per input, the WRITEs (1 DMA command), SYNCH head, span, span, route per input */
    if (!staged) {
        PlugXlInit(&x, PLUG_PAGE);
        e = PlugBuildStart(&x, &pay, r.Cmd, r.NCmd, addr, r.NRows, r.In1 | (r.In2 << 16), reloc);
        if (e == PLUG_OK && x.N == 4 * nin + 2) Pass("start transaction: stop %s, WRITEs (1 DMA page), SYNCH head, span, span, route%s",
                                                     stereo ? "2 strips" : "strip", stereo ? " on each input" : "");
        else Check("start transaction: %lu entries (%lu expected), error %lu", x.N, 4 * nin + 2, e);
    } else {
        ULONG okPages = 0;

        PlugXlInit(&x, PLUG_PAGE);
        e = PlugBuildStartEx(&x, &pay, r.Cmd, pgLen[0], addr, r.NRows, r.In1 | (r.In2 << 16), reloc, 0);
        if (e == PLUG_OK && x.N == 1 && x.NRx == 0) Pass("stage transaction: page 0 as 1 DMA command (%lu words), no SYNCH, no route",
                                                          pgLen[0]);
        else Check("stage transaction: %lu entries, error %lu", x.N, e);
        for (i = 1; i < npg; i++) {
            PlugXlInit(&x, PLUG_PAGE);
            if (PlugRelocate(reloc, r.Cmd + pgAt[i], pgLen[i], addr, r.NRows) == PLUG_OK &&
                PlugXlCmdWords(&x, &pay, reloc, pgLen[i]) == PLUG_OK && x.N == 1) {
                okPages++;
            }
        }
        if (okPages == npg - 1) Pass("SERVICE pages: %lu transactions of 1 DMA command each", okPages);
        else Check("SERVICE pages: %lu of %lu buildable", okPages, npg - 1);
        PlugXlInit(&x, 0);
        e = PlugBuildStartEx(&x, &pay, NULL, 0, addr, r.NRows, r.In1 | (r.In2 << 16), reloc, 1);
        if (e == PLUG_OK && x.N == 4 * nin + 1) Pass("START transaction: stop %s, SYNCH the staged head, span, span, route%s",
                                                     stereo ? "2 strips" : "strip", stereo ? " on each input" : "");
        else Check("START transaction: %lu entries (%lu expected), error %lu", x.N, 4 * nin + 1, e);
    }
    {
        ULONG hdr[2], sa[STRIP_PLAN_ROWS];
        StripPlanAddrs(r.In1 < 8 ? r.In1 : 0, sa);
        hdr[0] = sa[0];
        hdr[1] = 0;
        if (stereo) {
            StripPlanAddrs(r.In2, sa);
            hdr[1] = sa[0];
        }
        PlugXlInit(&x, 0);
        e = PlugBuildStop(&x, r.In1 | (r.In2 << 16), 1, 1, hdr);
        for (i = 0; i < nb; i++) {
            PlugBuildRelease(&x, &bills[i]);
        }
        if (e == PLUG_OK && x.Err == PLUG_OK && stereo) {
            Pass("teardown: stop, plain strips %lu/%lu restored at 0x%05lx/0x%05lx, %lu release(s)", r.In1 + 1, r.In2 + 1, hdr[0],
                 hdr[1], nb);
        } else if (e == PLUG_OK && x.Err == PLUG_OK) {
            Pass("teardown: stop, plain strip %lu restored at 0x%05lx, %lu release(s)", r.In1 + 1, hdr[0], nb);
        } else {
            Check("teardown not buildable");
        }
    }
    for (i = 0, e = 0; i < r.NObs; i++) {
        const OBS *o = &r.Obs[i];
        if (o->Row >= r.NRows || o->Word >= r.Rows[o->Row].Dwords || o->Count > r.Rows[o->Row].Dwords - o->Word) {
            e++;
        }
    }
    if (r.NObs && e == 0) Pass("%lu readback runs inside their rows", r.NObs);
    else Check("%lu readback runs, %lu outside their rows", r.NObs, e);
    for (i = 0; i < r.NObs;) {
        i = ReadGroupEnd(&r, i);
        reads++;
    }
    Info("readback: %lu runs in %lu SERVICE reads (runs of one row merged up to %u words)%s", r.NObs, reads,
         PLUG_READ_WORDS_MAX, staged ? ", read while staged, before START" : "");
    /* DSP-owned words: inside the chain's rows, and how many of them the readback reads (and skips) */
    if (r.NOwned != 0) {
        ULONG out = 0, inRuns = 0, total = 0, j;

        for (j = 0; j < r.NOwned; j++) {
            if (r.OwnedRow[j] >= r.NRows || r.OwnedWord[j] >= r.Rows[r.OwnedRow[j]].Dwords) {
                out++;
                Check("dsp_owned row %lu word %lu (%s) is outside the chain's rows", r.OwnedRow[j], r.OwnedWord[j], r.OwnedSrc[j]);
            }
        }
        for (i = 0; i < r.NObs; i++) {
            for (j = 0; j < r.Obs[i].Count; j++) {
                total++;
                inRuns += IsOwned(&r, r.Obs[i].Row, r.Obs[i].Word + j) ? 1 : 0;
            }
        }
        OwnedText(&r, r.NOwned, owned, sizeof(owned));
        if (out == 0) Pass("%lu DSP-owned word(s) inside their rows, never compared by the readback: %s", r.NOwned, owned);
        Info("readback: %lu of its %lu words are DSP-owned and skipped (%lu compared); a word that moves between two reads "
             "is skipped too", inRuns, total, total - inRuns);
    } else {
        Info("no dsp_owned lines: none known for this plug-in, or a recipe made before Nov 15 2026 (make it again with this "
             "p1_recipe.py to carry the bundle's, the meters' and plugins\\p1\\dsp-owned.txt's)");
    }
    printf("\nSummary: %d PASS, %d CHECK\n", g_pass, g_check);
    FreeRecipe(&r);
    return g_check ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* caps / licence                                                      */
/* ------------------------------------------------------------------ */

static void PrintCaps(const APOLLO_PLUGIN_CAPS *c)
{
    ULONG d, p, i;

    printf("Plug-in host     ABI %lu, %s (%s)\n", c->Abi, c->Enabled ? "ENABLED" : "disabled", c->Why);
    printf("Operations       0x%02lx:%s%s%s%s\n", c->Operations, c->Operations & APOLLO_PLUGIN_OP_BOARD_QUERY ? " board-query" : "",
           c->Operations & APOLLO_PLUGIN_OP_CREATE ? " create" : "", c->Operations & APOLLO_PLUGIN_OP_DESTROY ? " destroy" : "",
           c->Operations & APOLLO_PLUGIN_OP_SERVICE ? " service" : "");
    printf("Features         0x%02lx:%s%s%s%s%s%s%s\n", c->Features, c->Features & APOLLO_PLUGIN_FEAT_CHECK ? " check" : "",
           c->Features & APOLLO_PLUGIN_FEAT_PLAIN ? " plain" : "", c->Features & APOLLO_PLUGIN_FEAT_START_CHAIN ? " start-chain" : "",
           c->Features & APOLLO_PLUGIN_FEAT_SERVICE ? " service" : "",
           c->Features & APOLLO_PLUGIN_FEAT_STAGE ? " stage (startups over one page)" : "",
           c->Features & APOLLO_PLUGIN_FEAT_STEREO ? " stereo" : "",
           c->Features & APOLLO_PLUGIN_FEAT_UNITS ? " units (x8p and Twin X)" : "");
    printf("Epoch            %lu; strip DSP %lu, strips state %lu, started 0x%02lx\n", c->Epoch, c->StripDsp, c->StripsState,
           c->StripsStarted);
    printf("DSPs             owned 0x%02lx, quarantined 0x%02lx\n", c->OwnedMask, c->QuarantinedMask);
    for (d = 0; d < c->NumDsps && d < APOLLO_PLUGIN_DSPS; d++) {
        const APOLLO_PLUGIN_DSP_INFO *o = &c->Dsp[d];

        printf("  DSP %lu  boot %08lx load %08lx %s%s%s%s%s%s  xacts %lu uploads %lu (ok %lu bad %lu) zero %lu memspec %lu "
               "writes %lu stops %lu releases %lu timeouts %lu\n", d, o->BootWord, o->Load,
               o->Flags & APOLLO_PLUGIN_DSP_OWNED ? "owned " : "", o->Flags & APOLLO_PLUGIN_DSP_ADMITTED ? "admitted " : "",
               o->Flags & APOLLO_PLUGIN_DSP_QUARANTINED ? "QUARANTINED " : "", o->Flags & APOLLO_PLUGIN_DSP_LEDGER ? "ledger " : "",
               o->Flags & APOLLO_PLUGIN_DSP_STRIPS ? "strips " : "", o->Flags & APOLLO_PLUGIN_DSP_RINGS ? "rings" : "",
               o->Xacts, o->Uploads, o->RepliesOk, o->RepliesBad, o->ZeroFills, o->Memspecs, o->Writes, o->Stops, o->Releases,
               o->Timeouts);
        if (o->Flags & (APOLLO_PLUGIN_DSP_WARM | APOLLO_PLUGIN_DSP_WARM_PROVEN | APOLLO_PLUGIN_DSP_WHY_MASK)) {
            /* [Warm] owned through a warm adoption (proven by an acknowledged upload), or why it is not owned */
            printf("         warm start: %s%s why-not-owned %lu\n", o->Flags & APOLLO_PLUGIN_DSP_WARM ? "owned through the adoption " : "",
                   o->Flags & APOLLO_PLUGIN_DSP_WARM_PROVEN ? "uploads proven " : "", (ULONG)APOLLO_PLUGIN_DSP_WHY(o->Flags));
        }
        if (o->Flags & APOLLO_PLUGIN_DSP_LEDGER) {
            for (p = 0; p < 4; p++) {
                printf("         pool %lu base %08lx total %lu scratch %lu used %lu free %lu largest %lu allocations %lu holds %lu\n",
                       p, o->Pool[p].Base, o->Pool[p].Total, o->Pool[p].Scratch, o->Pool[p].Used, o->Pool[p].Free,
                       o->Pool[p].Largest, o->Pool[p].Allocations, o->Pool[p].Holds);
            }
        }
    }
    printf("Instances        %lu\n", c->Instances);
    for (i = 0; i < c->Instances && i < APOLLO_PLUGIN_INSTANCES_MAX; i++) {
        const APOLLO_PLUGIN_INSTANCE_INFO *o = &c->Inst[i];
        printf("  handle %lu render 0x%03lx DSP %lu synch 0x%05lx head 0x%05lx epoch %lu modules %lu rows %lu flags 0x%02lx%s%s%s%s%s\n",
               o->Handle, o->RenderId, o->Dsp, o->Synch, o->HeadAddress, o->Epoch, o->Modules, o->Rows, o->Flags,
               o->Flags & APOLLO_PLUGIN_INST_STARTED ? " started" : "", o->Flags & APOLLO_PLUGIN_INST_LOADED ? " loaded" : "",
               o->Flags & APOLLO_PLUGIN_INST_STAGED ? " staged" : "",
               o->Flags & APOLLO_PLUGIN_INST_LOST ? " LOST" : "", o->Flags & APOLLO_PLUGIN_INST_MINE ? " mine" : "");
    }
    printf("Pages            %lu allocated, %lu parked; catalog %lu; board queries %lu (%lu failed), admitted %lu, refused %lu\n",
           c->PagesAllocated, c->PagesParked, c->CatalogEntries, c->BoardQueries, c->BoardQueryFailures, c->Admitted, c->Refused);
    printf("Last             status 0x%08lx (%s), step %s\n", (ULONG)c->LastStatus, StatusText(c->LastStatus), StepText(c->LastStep));
}

static int CmdCaps(void)
{
    static APOLLO_PLUGIN_CAPS c;

    if (!GetCaps(&c)) {
        return 2;
    }
    PrintCaps(&c);
    return 0;
}

/* "0x8db UAD Oxford SuprEsser DS" lines: render id -> name. */
typedef struct _NAMEMAP {
    ULONG Id;
    char Name[120];
} NAMEMAP;

static ULONG LoadNames(const char *path, NAMEMAP *out, ULONG max)
{
    FILE *f = NULL;
    char line[256];
    ULONG n = 0;

    if (path == NULL || fopen_s(&f, path, "r") != 0 || f == NULL) {
        return 0;
    }
    while (n < max && fgets(line, sizeof(line), f) != NULL) {
        char *sp;
        ULONG id;

        line[strcspn(line, "\r\n")] = 0;
        sp = strchr(line, ' ');
        if (sp == NULL || line[0] == '#') {
            continue;
        }
        *sp = 0;
        if (!ParseU(line, &id)) {
            continue;
        }
        out[n].Id = id;
        strncpy_s(out[n].Name, sizeof(out[n].Name), sp + 1, _TRUNCATE);
        n++;
    }
    fclose(f);
    return n;
}

static int CmdLicence(int n, char **a)
{
    static APOLLO_BOARD_QUERY q;
    static RECIPE r;
    static NAMEMAP names[1024];
    static const char *cand[] = { "UAD Oxford SuprEsser DS", "UAD AMS DMX 15-80 S", "UAD AMS RMX16", "UAD AMS RMX16 Expanded",
                                  "UAD C-Suite C-Vox", "UAD Lexicon 480L", "UAD Ampeg SVTVR Classic" };
    const char *recipe = NULL, *namesPath = NULL, *authOut = NULL;
    ULONG cnt[5] = { 0, 0, 0, 0, 0 }, i, nn, st;
    DWORD ret = 0;
    int k;

    for (k = 1; k + 1 < n; k++) {
        if (strcmp(a[k], "--recipe") == 0) recipe = a[k + 1];
        if (strcmp(a[k], "--names") == 0) namesPath = a[k + 1];
        if (strcmp(a[k], "--authorized-out") == 0) authOut = a[k + 1];
    }
    if (recipe != NULL && !LoadRecipe(recipe, &r)) {
        return 2;
    }
    nn = LoadNames(namesPath, names, 1024);
    memset(&q, 0, sizeof(q));
    q.Size = sizeof(q);
    if (!DeviceIoControl(g_dev, IOCTL_APOLLO_BOARD_QUERY, &q, sizeof(q), &q, sizeof(q), &ret, NULL) || ret < sizeof(q)) {
        DWORD e = GetLastError();
        Check("licence: BOARD_QUERY refused by the driver (error %lu%s)", e,
              e == ERROR_NOT_SUPPORTED ? ": PluginHost is 0, or the unit is not in Mode 2, or this apollo.sys hosts on the x8p only" : "");
        return 1;
    }
    printf("Board query: status 0x%08lx (%s), date 0x%08lx, epoch %lu, cookie %08lx, reply %08lx %08lx, %lu reply words "
           "written, %lu ms\n", (ULONG)q.Status, StatusText(q.Status), q.Date, q.Epoch, q.Cookie, q.Reply0, q.Reply1,
           q.WordsWritten, q.ElapsedMs);
    if (q.Status != 0) {
        Check("licence: no answer from the %s (status 0x%08lx, reply %08lx %08lx, %lu words written)", g_model->Name,
              (ULONG)q.Status, q.Reply0, q.Reply1, q.WordsWritten);
        if (q.WordsWritten == 0 && q.Reply0 == PLUG_BQ_SENTINEL) {
            Info("the reply area stayed at its prefill: DSP 0 did not answer (warm start? try again after a %s power cycle)",
                 g_model->Name);
        }
        FreeRecipe(&r);
        return 1;
    }
    for (i = 0; i < APOLLO_BOARD_QUERY_WORDS; i++) {
        st = PlugLicenseState(q.Words[i]);
        if (st < 5) cnt[st]++;
    }
    Pass("licence: the %s answered the board query (0x80030302, cookie echoed): %lu authorized, %lu demo running, "
         "%lu demo not started, %lu expired, %lu unavailable", g_model->Name, cnt[0], cnt[1], cnt[2], cnt[3], cnt[4]);
    if (cnt[0] + cnt[1] == 0) {
        Check("licence: the table admits nothing (UA's software writes it: put the unit on UA's driver, sync, come back)");
    }
    /* the P1 candidates, by name through the names file */
    for (k = 0; k < (int)(sizeof(cand) / sizeof(cand[0])); k++) {
        for (i = 0; i < nn; i++) {
            if (_stricmp(names[i].Name, cand[k]) == 0 && names[i].Id >= PLUG_BQ_BASE && names[i].Id < PLUG_BQ_BASE + PLUG_BQ_WORDS) {
                st = PlugLicenseState(q.Words[names[i].Id - PLUG_BQ_BASE]);
                Info("candidate %-26s render 0x%03lx: %s", cand[k], names[i].Id, LicText(st));
                break;
            }
        }
    }
    if (recipe != NULL) {
        if (r.RenderId < PLUG_BQ_BASE || r.RenderId >= PLUG_BQ_BASE + PLUG_BQ_WORDS) {
            Check("licence: the recipe's render id 0x%03lx is outside the table", r.RenderId);
        } else {
            st = PlugLicenseState(q.Words[r.RenderId - PLUG_BQ_BASE]);
            if (PlugLicenseAdmits(st)) Pass("licence: %s (0x%03lx) is %s on this %s", r.Name, r.RenderId, LicText(st), g_model->Name);
            else Check("licence: %s (0x%03lx) is %s on this %s: pick an authorized candidate", r.Name, r.RenderId, LicText(st),
                       g_model->Name);
        }
    }
    if (authOut != NULL && nn != 0) {
        FILE *f = NULL;
        ULONG w = 0;
        if (fopen_s(&f, authOut, "w") == 0 && f != NULL) {
            for (i = 0; i < nn; i++) {
                if (names[i].Id >= PLUG_BQ_BASE && names[i].Id < PLUG_BQ_BASE + PLUG_BQ_WORDS &&
                    PlugLicenseAdmits(PlugLicenseState(q.Words[names[i].Id - PLUG_BQ_BASE]))) {
                    fprintf(f, "%s\n", names[i].Name);
                    w++;
                }
            }
            fclose(f);
            Info("wrote %lu authorized plug-in name(s) to %s (the mapper's and p1_recipe.py's --authorized FILE)", w, authOut);
        }
    }
    SecureZeroMemory(&q, sizeof(q));
    FreeRecipe(&r);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Measurement: record slots through READ_CAPTURE (elevated handle)    */
/* ------------------------------------------------------------------ */

typedef struct _MEAS {
    BOOL    Valid;
    ULONG   Frames, NSel;
    ULONG   Slot[8];
    double  Rms[8], Peak[8];
    BOOL    Zero[8];
    LONG   *Buf;            /* Frames x NSel */
} MEAS;

static double Db(double v)
{
    return v <= 0.0 ? -200.0 : 20.0 * log10(v / FULL_SCALE);
}

/* RMS, peak and exact zero per slot of a filled capture (Buf, Frames, NSel set); marks it valid. */
static void MeasStats(MEAS *m)
{
    ULONG k, i;

    for (k = 0; k < m->NSel; k++) {
        double ss = 0.0, pk = 0.0;
        BOOL zero = TRUE;

        for (i = 0; i < m->Frames; i++) {
            double v = (double)m->Buf[(size_t)i * m->NSel + k];
            ss += v * v;
            if (fabs(v) > pk) pk = fabs(v);
            if (v != 0.0) zero = FALSE;
        }
        m->Rms[k] = sqrt(ss / (double)m->Frames);
        m->Peak[k] = pk;
        m->Zero[k] = zero;
    }
    m->Valid = TRUE;
}

static BOOL Capture(const ULONG *slots, ULONG nsel, ULONG Frames, MEAS *m)
{
    APOLLO_INFO info;
    APOLLO_POSITION pos;
    APOLLO_CAPTURE_REQUEST req;
    DWORD bytes = 0, ret = 0;
    UCHAR *buf;
    ULONGLONG next;
    ULONG got = 0, C, k, i;
    DWORD t0 = GetTickCount(), limit = Frames / 48 + 2500;

    memset(m, 0, sizeof(*m));
    if (!g_admin || nsel == 0 || nsel > 8 || Frames < MEAS_FRAMES || Frames > MEAS_TAIL_FRAMES) {
        return FALSE;
    }
    if (ApolloQueryInfo(g_dev, &info, &bytes) != 0 || info.RecChannels == 0 || !(info.Flags & APOLLO_FLAG_TRANSPORT)) {
        return FALSE;
    }
    C = info.RecChannels;
    if (!DeviceIoControl(g_dev, IOCTL_APOLLO_GET_POSITION, NULL, 0, &pos, sizeof(pos), &ret, NULL)) {
        return FALSE;
    }
    m->Buf = (LONG *)calloc((size_t)Frames * nsel, sizeof(LONG));
    buf = (UCHAR *)malloc(sizeof(APOLLO_CAPTURE_HEADER) + (size_t)4096 * C * sizeof(LONG));
    if (m->Buf == NULL || buf == NULL) {
        free(buf);
        free(m->Buf);
        m->Buf = NULL;
        return FALSE;
    }
    next = pos.Position;
    while (got < Frames && GetTickCount() - t0 < limit) {
        PAPOLLO_CAPTURE_HEADER h = (PAPOLLO_CAPTURE_HEADER)buf;
        const LONG *data = (const LONG *)(buf + sizeof(*h));
        ULONG want = Frames - got;

        memset(&req, 0, sizeof(req));
        req.StartPosition = next;
        req.MaxFrames = want < 4096 ? want : 4096;
        if (!DeviceIoControl(g_dev, IOCTL_APOLLO_READ_CAPTURE, &req, sizeof(req), buf,
                             (DWORD)(sizeof(*h) + (size_t)req.MaxFrames * C * 4), &ret, NULL) ||
            ret < sizeof(*h) || h->Channels != C || h->Frames > req.MaxFrames) {
            break;
        }
        if (h->Frames == 0) {
            if (h->Flags & APOLLO_CAPTURE_OVERRUN) {
                next = h->HwPosition;
            }
            Sleep(20);
            continue;
        }
        for (i = 0; i < h->Frames && got < Frames; i++, got++) {
            for (k = 0; k < nsel; k++) {
                m->Buf[(size_t)got * nsel + k] = (slots[k] < C) ? data[(size_t)i * C + slots[k]] : 0;
            }
        }
        next = h->StartPosition + h->Frames;
    }
    free(buf);
    if (got < Frames / 2) {
        free(m->Buf);
        m->Buf = NULL;
        return FALSE;
    }
    m->Frames = got;
    m->NSel = nsel;
    for (k = 0; k < nsel; k++) {
        m->Slot[k] = slots[k];
    }
    MeasStats(m);
    return TRUE;
}

static void FreeMeas(MEAS *m)
{
    free(m->Buf);
    m->Buf = NULL;
}

/* Lag of slot index A against B (positive: A later), normalized correlation in *Corr; -9999 = none. */
static int Lag(const MEAS *m, ULONG A, ULONG B, double *Corr)
{
    double best = 0.0, ea = 0.0, eb = 0.0, norm, *xa, *xb;
    int L, k, nl = 4, bestL = -9999;
    ULONG t, n = m->Frames;

    *Corr = 0.0;
    if (!m->Valid || A >= m->NSel || B >= m->NSel || n < 4 * MAX_LAG) {
        return -9999;
    }
    xa = (double *)malloc(n * sizeof(double));
    xb = (double *)malloc(n * sizeof(double));
    if (xa == NULL || xb == NULL) {
        free(xa);
        free(xb);
        return -9999;
    }
    for (t = 0; t < n; t++) {
        xa[t] = (double)m->Buf[(size_t)t * m->NSel + A] / FULL_SCALE;
        xb[t] = (double)m->Buf[(size_t)t * m->NSel + B] / FULL_SCALE;
    }
    for (t = MAX_LAG; t < n - MAX_LAG; t++) {
        ea += xa[t] * xa[t];
        eb += xb[t] * xb[t];
    }
    if (ea > 0.0 && eb > 0.0) {
        norm = sqrt(ea * eb);
        /* four lags per pass: independent sums, each added in the same order as one lag alone (the same
           result to the bit), so the adds do not wait on one another */
        for (L = -MAX_LAG; L <= MAX_LAG; L += nl) {
            double s[4] = { 0.0, 0.0, 0.0, 0.0 };

            nl = (MAX_LAG - L + 1 < 4) ? MAX_LAG - L + 1 : 4;
            if (nl == 4) {
                double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;

                for (t = MAX_LAG; t < n - MAX_LAG; t++) {
                    double x = xa[t];
                    ULONG u = t - L;

                    s0 += x * xb[u];
                    s1 += x * xb[u - 1];
                    s2 += x * xb[u - 2];
                    s3 += x * xb[u - 3];
                }
                s[0] = s0;
                s[1] = s1;
                s[2] = s2;
                s[3] = s3;
            } else {
                for (k = 0; k < nl; k++) {
                    for (t = MAX_LAG; t < n - MAX_LAG; t++) {
                        s[k] += xa[t] * xb[t - L - k];
                    }
                }
            }
            for (k = 0; k < nl; k++) {
                double c = s[k] / norm;

                if (fabs(c) > fabs(best)) {
                    best = c;
                    bestL = L + k;
                }
            }
        }
    }
    free(xa);
    free(xb);
    if (fabs(best) < 0.3) {
        bestL = -9999;              /* no common source: no lag */
    }
    *Corr = best;
    return bestL;
}

static void PrintMeas(const char *when, const MEAS *m)
{
    ULONG k;

    if (!m->Valid) {
        printf("  %-10s no capture (not elevated, or the transport is stopped)\n", when);
        return;
    }
    printf("  %-10s", when);
    for (k = 0; k < m->NSel; k++) {
        if (m->Zero[k]) printf("  slot %lu: zero", m->Slot[k]);
        else printf("  slot %lu: rms %.1f peak %.1f", m->Slot[k], Db(m->Rms[k]), Db(m->Peak[k]));
    }
    printf("\n");
}

/* ---- what a plug-in does to input 1: tails (reverbs, echoes) and spectrum (amps, EQs) ---- */

#define BLOCK_FRAMES        480u        /* 10 ms */
#define TAIL_BLOCKS_MAX     (MEAS_TAIL_FRAMES / BLOCK_FRAMES)

static double Db10(double e)
{
    return e <= 1e-20 ? -200.0 : 10.0 * log10(e);
}

static int CmpDouble(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static double Median(double *v, ULONG n)
{
    if (n == 0) {
        return 0.0;
    }
    qsort(v, n, sizeof(double), CmpDouble);
    return (n & 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/* Mean square (full scale 1) of the 10 ms blocks of slot index K: the block count. */
static ULONG BlockEnergy(const MEAS *m, ULONG K, double *E, ULONG Max)
{
    ULONG nb = m->Frames / BLOCK_FRAMES, b, t;

    if (nb > Max) {
        nb = Max;
    }
    for (b = 0; b < nb; b++) {
        double s = 0.0;
        for (t = 0; t < BLOCK_FRAMES; t++) {
            double v = (double)m->Buf[((size_t)b * BLOCK_FRAMES + t) * m->NSel + K] / FULL_SCALE;
            s += v * v;
        }
        E[b] = s / BLOCK_FRAMES;
    }
    return nb;
}

static double MeanOf(const double *E, ULONG From, ULONG To)
{
    double s = 0.0;
    ULONG b;

    for (b = From; b <= To; b++) {
        s += E[b];
    }
    return s / (double)(To - From + 1);
}

typedef struct _TAIL {
    ULONG   Events;         /* short sounds found (claps, clicks, plucks) */
    double  Tail;           /* median dB: input 1's energy 100-400 ms after each against its peak (input 1 alone) */
    double  RefTail;        /* median dB: the same for input 1 against input 2 (the dry source): about 0 = no tail added */
} TAIL;

/*
 * Short sounds and what follows them. The sounds are found in input 2's envelope when it carries
 * the same source (the dry reference), else in input 1's: a 10 ms block at least 20 dB over the
 * capture's 10th percentile and the loudest within +-50 ms, events 300 ms apart. A reverb on
 * input 1 keeps energy 100-400 ms after each, where the dry source has decayed.
 */
static void TailStats(const MEAS *m, ULONG A, ULONG B, BOOL HasRef, TAIL *t)
{
    static double e1[TAIL_BLOCKS_MAX], e2[TAIL_BLOCKS_MAX], d[TAIL_BLOCKS_MAX], s[TAIL_BLOCKS_MAX], s2[TAIL_BLOCKS_MAX];
    const double *pk;
    ULONG nb, b, k, last = 0, n = 0;
    double floor10;

    memset(t, 0, sizeof(*t));
    if (!m->Valid || A >= m->NSel || (HasRef && B >= m->NSel)) {
        return;
    }
    nb = BlockEnergy(m, A, e1, TAIL_BLOCKS_MAX);
    if (HasRef) {
        (void)BlockEnergy(m, B, e2, TAIL_BLOCKS_MAX);
    }
    pk = HasRef ? e2 : e1;
    if (nb < 50) {
        return;
    }
    for (b = 0; b < nb; b++) {
        d[b] = Db10(pk[b]);
        s[b] = d[b];
    }
    qsort(s, nb, sizeof(double), CmpDouble);
    floor10 = s[nb / 10];
    for (b = 5; b + 41 < nb; b++) {
        BOOL peak = d[b] >= floor10 + 20.0;
        double t1, p1;

        for (k = 1; peak && k <= 5; k++) {
            peak = d[b] >= d[b - k] && d[b] >= d[b + k];
        }
        if (!peak || (n > 0 && b < last + 30)) {
            continue;
        }
        t1 = MeanOf(e1, b + 10, b + 40);
        p1 = MeanOf(e1, b - 1, b + 1);
        s[n] = Db10(t1) - Db10(p1);
        if (HasRef) {
            s2[n] = (Db10(t1) - Db10(MeanOf(e2, b + 10, b + 40))) - (Db10(p1) - Db10(MeanOf(e2, b - 1, b + 1)));
        }
        n++;
        last = b;
    }
    t->Events = n;
    t->Tail = Median(s, n);
    t->RefTail = HasRef ? Median(s2, n) : 0.0;
}

/* High-frequency tilt of slot index K: 10 log10(E[(x[t] - x[t-1])^2] / E[x^2]) (a first difference
   weighs the top octaves); a cabinet or a low-pass moves it by several dB. */
static double HfTilt(const MEAS *m, ULONG K)
{
    double e = 0.0, de = 0.0, prev = 0.0;
    ULONG t;

    if (!m->Valid || K >= m->NSel) {
        return 0.0;
    }
    for (t = 0; t < m->Frames; t++) {
        double v = (double)m->Buf[(size_t)t * m->NSel + K] / FULL_SCALE;
        e += v * v;
        if (t > 0) {
            de += (v - prev) * (v - prev);
        }
        prev = v;
    }
    return (e <= 1e-20) ? 0.0 : Db10(de / e);
}

/* ---- the verdicts (create-start and selftest use the same thresholds) ---- */

#define TAIL_MIN_DB         6.0     /* a reverb adds at least this much energy 100-400 ms after a short sound */
#define LEVEL_MIN_DB        0.5     /* P0's level change */
#define TILT_MIN_DB         3.0     /* HF tilt of input 1 against input 2 */
#define TILT_ALONE_DB       6.0     /* HF tilt of input 1 alone (the source itself varies) */
#define CORR_DROP           0.2
#define LIVE_DB             (-80.0) /* a capture quieter than this proves nothing */

/* 1: a tail was added; 0: none; -1: too few short sounds to tell. */
static int TailVerdict(const TAIL *B, const TAIL *D, BOOL HasRef)
{
    if (B->Events < 2 || D->Events < 2) {
        return -1;
    }
    return ((HasRef ? D->RefTail - B->RefTail : D->Tail - B->Tail) >= TAIL_MIN_DB) ? 1 : 0;
}

typedef struct _SPECT {
    BOOL    Live;           /* input 1 (and the reference) above LIVE_DB */
    int     Lag;            /* -9999: none */
    double  Rel, Corr;      /* input 1 against the reference */
    double  Tilt;           /* HF tilt: input 1 minus the reference's, or input 1 alone */
} SPECT;

static void SpectOf(const MEAS *m, BOOL HasRef, ULONG RefIdx, SPECT *s)
{
    memset(s, 0, sizeof(*s));
    s->Lag = -9999;
    if (!m->Valid) {
        return;
    }
    s->Live = Db(m->Rms[0]) > LIVE_DB && (!HasRef || Db(m->Rms[RefIdx]) > LIVE_DB);
    s->Tilt = HfTilt(m, 0);
    if (HasRef) {
        s->Lag = Lag(m, 0, RefIdx, &s->Corr);
        s->Rel = Db(m->Rms[0]) - Db(m->Rms[RefIdx]);
        s->Tilt -= HfTilt(m, RefIdx);
    }
}

/* 1: input 1 measurably changed (level, spectrum, correlation or lag); 0: not; -1: a capture is silent. */
static int LevelVerdict(const SPECT *B, const SPECT *D, BOOL HasRef)
{
    if (!B->Live || !D->Live) {
        return -1;
    }
    if (!HasRef) {
        return fabs(D->Tilt - B->Tilt) >= TILT_ALONE_DB ? 1 : 0;
    }
    return ((B->Lag != -9999 && D->Lag != -9999 && D->Lag != B->Lag) || fabs(D->Rel - B->Rel) >= LEVEL_MIN_DB ||
            fabs(D->Tilt - B->Tilt) >= TILT_MIN_DB || B->Corr - D->Corr >= CORR_DROP) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* The CREATE steps                                                    */
/* ------------------------------------------------------------------ */

static int CmdCreateCheck(int n, char **a)
{
    static RECIPE r;
    APOLLO_PLUGIN_INSTANCE q;

    if (n < 2 || !LoadRecipe(a[1], &r)) {
        return 2;
    }
    memset(&q, 0, sizeof(q));
    q.Op = APOLLO_PLUGIN_INST_CREATE;
    q.Flags = APOLLO_PLUGIN_F_CHECK;
    q.RenderId = r.RenderId;
    q.Dsp = APOLLO_PLUGIN_DSP_ANY;
    if (!Instance(&q, NULL, NULL, NULL, NULL)) {
        Check("CREATE CHECK: the IOCTL was refused");
        return 1;
    }
    PrintOutcome("CREATE CHECK", &q);
    if (q.Status == 0 && q.Handle == 0) Pass("CREATE CHECK: %s admitted by the %s (%s), nothing placed", r.Name, g_model->Name,
                                             LicText(q.LicenseState));
    else Check("CREATE CHECK: %s not admitted (0x%08lx %s, licence %s)", r.Name, (ULONG)q.Status, StatusText(q.Status),
               LicText(q.LicenseState));
    FreeRecipe(&r);
    return q.Status == 0 ? 0 : 1;
}

static BOOL Destroy(ULONG handle, APOLLO_PLUGIN_INSTANCE *q)
{
    memset(q, 0, sizeof(*q));
    q->Op = APOLLO_PLUGIN_INST_DESTROY;
    q->Handle = handle;
    return Instance(q, NULL, NULL, NULL, NULL);
}

static ULONG ArgDsp(int n, char **a, ULONG def)
{
    int k;
    ULONG v = def;

    for (k = 1; k + 1 < n; k++) {
        if (strcmp(a[k], "--dsp") == 0) {
            (void)ParseU(a[k + 1], &v);
        }
    }
    return v;
}

static int CmdCreatePlain(int n, char **a)
{
    static RECIPE r;
    static APOLLO_PLUGIN_CAPS c0, c1, c2;
    APOLLO_PLUGIN_INSTANCE q, d;
    ULONG dsp;
    BOOL ledger0;

    if (n < 2 || !LoadRecipe(a[1], &r)) {
        return 2;
    }
    dsp = ArgDsp(n, a, 2);
    if (dsp == 0 || dsp >= APOLLO_PLUGIN_DSPS || !GetCaps(&c0)) {
        return 2;
    }
    if (!(c0.Dsp[dsp].Flags & APOLLO_PLUGIN_DSP_ADMITTED)) {
        Check("DSP %lu is not admitted for placement (flags 0x%02lx; owned 0x%02lx): power-cycle the %s first", dsp,
              c0.Dsp[dsp].Flags, c0.OwnedMask, g_model->Name);
        return 1;
    }
    ledger0 = (c0.Dsp[dsp].Flags & APOLLO_PLUGIN_DSP_LEDGER) != 0;
    memset(&q, 0, sizeof(q));
    q.Op = APOLLO_PLUGIN_INST_CREATE;
    q.RenderId = r.RenderId;
    q.Dsp = dsp;
    q.NResources = r.NPRes;
    q.NPrivate = r.NPRows;
    if (!Instance(&q, (const UCHAR(*)[32])r.PRes, r.PRows, NULL, NULL)) {
        Check("CREATE (unrouted) on DSP %lu: the IOCTL was refused", dsp);
        return 1;
    }
    PrintOutcome("CREATE unrouted", &q);
    if (q.Status != 0) {
        Check("CREATE unrouted on DSP %lu: 0x%08lx %s at step %s (detail %08lx %08lx %08lx %08lx)", dsp, (ULONG)q.Status,
              StatusText(q.Status), StepText(q.Step), q.Detail[0], q.Detail[1], q.Detail[2], q.Detail[3]);
        (void)GetCaps(&c1);
        PrintCaps(&c1);
        return 1;
    }
    Pass("CREATE unrouted on DSP %lu: %lu module(s) uploaded, each acknowledged {80070004 0 id}; %lu row(s) zero-filled and "
         "linked (handle %lu)", dsp, q.Uploads, r.NPRows, q.Handle);
    (void)GetCaps(&c1);
    PrintCaps(&c1);
    if (c1.Dsp[dsp].RepliesBad == 0 && c1.Dsp[dsp].Timeouts == 0 && !(c1.Dsp[dsp].Flags & APOLLO_PLUGIN_DSP_QUARANTINED)) {
        Pass("DSP %lu: 0 refused uploads, 0 timeouts, not quarantined", dsp);
    } else {
        Check("DSP %lu: %lu refused, %lu timeouts, flags 0x%02lx", dsp, c1.Dsp[dsp].RepliesBad, c1.Dsp[dsp].Timeouts,
              c1.Dsp[dsp].Flags);
    }
    if (!Destroy(q.Handle, &d) || d.Status != 0) {
        Check("DESTROY: 0x%08lx %s", (ULONG)d.Status, StatusText(d.Status));
        return 1;
    }
    PrintOutcome("DESTROY", &d);
    (void)GetCaps(&c2);
    if (c2.Instances == 0 && (!ledger0 || SameUsage(c0.Dsp[dsp].Pool, c2.Dsp[dsp].Pool)) &&
        c2.Dsp[dsp].Pool[0].Used == 0 && c2.Dsp[dsp].Pool[1].Used == 0 && c2.Dsp[dsp].Pool[2].Used == 0 &&
        c2.Dsp[dsp].Pool[3].Used == 0) {
        Pass("DESTROY: DSP %lu's ledger back to baseline (nothing allocated), %lu release(s) sent, no instance left", dsp,
             c2.Dsp[dsp].Releases);
    } else {
        Check("after DESTROY: %lu instance(s); DSP %lu pool used %lu %lu %lu %lu", c2.Instances, dsp, c2.Dsp[dsp].Pool[0].Used,
              c2.Dsp[dsp].Pool[1].Used, c2.Dsp[dsp].Pool[2].Used, c2.Dsp[dsp].Pool[3].Used);
    }
    if (c2.QuarantinedMask == 0) Pass("no DSP quarantined"); else Check("quarantined DSPs 0x%02lx", c2.QuarantinedMask);
    FreeRecipe(&r);
    return g_check ? 1 : 0;
}

/* One SERVICE read of Count words at Row/Word into V (FALSE: the IOCTL or the read failed; *Status). */
static BOOL ServiceRead(const APOLLO_PLUGIN_INSTANCE *inst, ULONG Row, ULONG Word, ULONG Count, ULONG *V, LONG *Status,
                        ULONG Reply[2])
{
    APOLLO_PLUGIN_INSTANCE q;
    BOOL ok;

    memset(&q, 0, sizeof(q));
    q.Op = APOLLO_PLUGIN_INST_SERVICE;
    q.Handle = inst->Handle;
    q.Epoch = inst->Epoch;
    q.ReadRef = (Row << 24) | Word;
    q.ReadWords = Count;
    ok = Instance(&q, NULL, NULL, NULL, V);
    *Status = ok ? q.Status : (LONG)0xC0000001;
    Reply[0] = q.Detail[0];
    Reply[1] = q.Detail[1];
    return ok && q.Status == 0;
}

/*
 * SERVICE reads of every observation run: the words the startup wrote, as the DSP holds them.
 * Runs of one row that fit one 64-word read go as one read (the words between them are read, not
 * compared). DSP-owned words are never compared; a group with a difference is read once more (the
 * same span) and a word that moved between the two reads is learned as DSP-owned (GroupClassify).
 */
static void ReadBack(RECIPE *r, const APOLLO_PLUGIN_INSTANCE *inst, RB *C)
{
    static char list[4096];
    ULONG i = 0, j, span, rep[2], v[PLUG_READ_WORDS_MAX], v2[PLUG_READ_WORDS_MAX], failedReads = 0;
    LONG st;

    memset(C, 0, sizeof(*C));
    if (r->NOwnedRecipe != 0) {
        OwnedText(r, r->NOwnedRecipe, list, sizeof(list));
        Info("readback: %lu DSP-owned word(s) of the recipe never compared: %s", r->NOwnedRecipe, list);
    }
    while (i < r->NObs) {
        const OBS *o = &r->Obs[i];
        BOOL two = FALSE;

        j = ReadGroupEnd(r, i);
        span = ReadGroupSpan(r, i, j);
        C->Reads++;
        if (!ServiceRead(inst, o->Row, o->Word, span, v, &st, rep)) {
            C->Fail += j - i;
            if (++failedReads <= 3) {
                printf("  read row %lu word %lu x%lu: 0x%08lx %s (reply %08lx %08lx)\n", o->Row, o->Word, span, (ULONG)st,
                       StatusText(st), rep[0], rep[1]);
            }
            if (st == (LONG)0xC00000B5 || st == (LONG)0xC0000120 || st == (LONG)0xC0000001) {
                C->Fail += r->NObs - j;     /* a timeout quarantined the DSP (or the IOCTL failed): nothing more to read */
                break;
            }
            i = j;
            continue;
        }
        if (GroupDiffs(r, i, j, v) != 0) {
            /* the same read once more: a word the running DSP writes moves, a failed WRITE stays wrong */
            C->Reads++;
            two = ServiceRead(inst, o->Row, o->Word, span, v2, &st, rep);
            if (!two) {
                printf("  second read row %lu word %lu x%lu: 0x%08lx %s (reply %08lx %08lx)\n", o->Row, o->Word, span, (ULONG)st,
                       StatusText(st), rep[0], rep[1]);
            }
        }
        GroupClassify(r, i, j, v, two ? v2 : NULL, C, FALSE);
        i = j;
    }
}

/*
 * The chain head (row 0) as the startup's page 0 writes it: every value its WRITE blocks put in
 * row 0, read back in one SERVICE read and compared (upstream reads row 0 once a chain runs:
 * plugin_native_chain.py:108-118), DSP-owned words aside and with the readback's second-read rule.
 * *Known = the words compared.
 */
static int HeadCheck(RECIPE *r, ULONG Page0, const APOLLO_PLUGIN_INSTANCE *inst, ULONG *Known)
{
    ULONG want[PLUG_READ_WORDS_MAX], have[PLUG_READ_WORDS_MAX], v[PLUG_READ_WORDS_MAX], v2[PLUG_READ_WORDS_MAX], n, i, k, pos,
          len, w0, rep[2], bad = 0, nd = 0;
    BOOL two = FALSE;
    LONG st;

    *Known = 0;
    n = r->Rows[0].Dwords < PLUG_READ_WORDS_MAX ? r->Rows[0].Dwords : PLUG_READ_WORDS_MAX;
    memset(have, 0, sizeof(have));
    memset(want, 0, sizeof(want));
    for (i = 0; i + 3 < Page0; i += len) {
        len = r->Cmd[i] & 0xFFFFu;
        if (len < 4) {
            return -1;
        }
        if ((r->Cmd[i + 1] >> 24) != 0) {
            continue;
        }
        w0 = r->Cmd[i + 1] & 0xFFFFFFu;
        for (k = 0, pos = w0; k < len - 3; k++, pos++) {
            pos = PlugWriteSkip(w0, r->Cmd[i + 2], pos);
            if (pos < n) {
                want[pos] = r->Cmd[i + 3 + k];
                have[pos] = 1;
            }
        }
    }
    if (!ServiceRead(inst, 0, 0, n, v, &st, rep)) {
        printf("  head read: 0x%08lx %s (reply %08lx %08lx)\n", (ULONG)st, StatusText(st), rep[0], rep[1]);
        return -1;
    }
    for (pos = 0; pos < n; pos++) {
        nd += (have[pos] && !IsOwned(r, 0, pos) && v[pos] != want[pos]) ? 1 : 0;
    }
    if (nd != 0) {
        two = ServiceRead(inst, 0, 0, n, v2, &st, rep);
        if (!two) {
            printf("  second head read: 0x%08lx %s (reply %08lx %08lx)\n", (ULONG)st, StatusText(st), rep[0], rep[1]);
        }
    }
    for (pos = 0; pos < n; pos++) {
        if (!have[pos]) {
            continue;
        }
        switch (ReadbackWord(IsOwned(r, 0, pos), want[pos], v[pos], two, two ? v2[pos] : 0)) {
        case RB_OWNED:
            break;
        case RB_MOVES:
            LearnOwned(r, 0, pos);
            printf("  head word %lu: written 0x%08lx, read 0x%08lx then 0x%08lx\n", pos, want[pos], v[pos], v2[pos]);
            Info("row 0 word %lu moves while the DSP runs: DSP-owned, not compared (add it to plugins\\p1\\dsp-owned.txt)", pos);
            break;
        case RB_DIFFERS:
            (*Known)++;
            bad++;
            printf("  head word %lu differs: written 0x%08lx, read 0x%08lx%s\n", pos, want[pos], v[pos], two ? " twice" : "");
            break;
        default:
            (*Known)++;
            break;
        }
    }
    return (int)bad;
}

/*
 * GET_STRIPS (user IOCTL, status only): FALSE when it failed or the driver's struct is older than version 3 (every field
 * this tool reads before [Virtual]). [Virtual] A version 4 driver also gives the Virtual strips it built: their room is
 * part of the strip DSP's ledger (BuildLedger); an older driver's VirtualMask stays 0 (none). [AuxStrips] A version 5
 * driver also gives the aux-return strips it built (their room is booked too); a version 4 driver's AuxMask stays 0.
 * [TalkStrip] A version 6 driver also gives its talkback strip (its room is booked too); a version 5 driver's stays 0.
 */
static BOOL GetStrips(APOLLO_STRIPS_INFO *s)
{
    DWORD ret = 0;
    BOOL ok;

    memset(s, 0, sizeof(*s));
    ok = DeviceIoControl(g_dev, IOCTL_APOLLO_GET_STRIPS, NULL, 0, s, (DWORD)sizeof(*s), &ret, NULL) &&
         ret >= APOLLO_STRIPS_INFO_V3_SIZE && s->Version >= 2;
    if (ok) {
        g_virtualMask = (ret >= APOLLO_STRIPS_INFO_V4_SIZE && s->Version >= 4) ? (s->VirtualMask & 0xFFu) : 0;
        g_auxMask = (ret >= APOLLO_STRIPS_INFO_V5_SIZE && s->Version >= 5) ? (s->AuxMask & 0xFu) : 0;
        g_talkback = (ret >= sizeof(*s) && s->Version >= 6) ? (s->Talkback & 1u) : 0;
    }
    return ok;
}

/* Instances left of a create-start: its handle, or any routed or staged chain on one of its inputs. */
static ULONG InstancesLeft(const APOLLO_PLUGIN_CAPS *c, ULONG Handle, ULONG Synch)
{
    ULONG k, n = 0;

    for (k = 0; k < c->Instances && k < APOLLO_PLUGIN_INSTANCES_MAX; k++) {
        const APOLLO_PLUGIN_INSTANCE_INFO *o = &c->Inst[k];

        if (o->Handle == Handle ||
            ((o->Flags & (APOLLO_PLUGIN_INST_STARTED | APOLLO_PLUGIN_INST_STAGED)) && PlugInputsOverlap(o->Synch, Synch))) {
            n++;
        }
    }
    return n;
}

/*
 * The staged startup's pages 1.. as SERVICE writes (plugin_native_chain.py:331-333): each one
 * bounded transaction, checked by the driver against the CREATE layout. FALSE at the first that
 * fails (an unproven one quarantined the DSP; the caller destroys the never-routed instance).
 */
static BOOL StagePages(const RECIPE *r, const APOLLO_PLUGIN_INSTANCE *inst, const ULONG *At, const ULONG *Len, ULONG N,
                       ULONG *Ms)
{
    DWORD t0 = GetTickCount();
    ULONG k;

    *Ms = 0;
    for (k = 1; k < N; k++) {
        APOLLO_PLUGIN_INSTANCE q;

        memset(&q, 0, sizeof(q));
        q.Op = APOLLO_PLUGIN_INST_SERVICE;
        q.Handle = inst->Handle;
        q.Epoch = inst->Epoch;
        q.NCommands = Len[k];
        if (!Instance(&q, NULL, NULL, r->Cmd + At[k], NULL)) {
            Check("startup page %lu of %lu: the SERVICE IOCTL was refused", k, N - 1);
            return FALSE;
        }
        if (q.Status != 0) {
            Check("startup page %lu of %lu (%lu words at word %lu): 0x%08lx %s at step %s (%lu ms)", k, N - 1, Len[k], At[k],
                  (ULONG)q.Status, StatusText(q.Status), StepText(q.Step), q.ElapsedMs);
            return FALSE;
        }
    }
    *Ms = GetTickCount() - t0;
    return TRUE;
}

/* Take a chain down after a failed step (DESTROY; a staged one was never routed: releases only). */
static void TakeDown(ULONG Handle, const char *Why)
{
    APOLLO_PLUGIN_INSTANCE d;

    if (Destroy(Handle, &d)) {
        PrintOutcome("DESTROY", &d);
        if (d.Status == 0) Info("instance %lu destroyed after %s", Handle, Why);
        else Check("DESTROY after %s: 0x%08lx %s", Why, (ULONG)d.Status, StatusText(d.Status));
    } else {
        Check("DESTROY after %s: the IOCTL was refused (closing the handle destroys it)", Why);
    }
}

/* Lag, level and correlation of input 1 against the reference in one capture. */
static void Against(const MEAS *m, BOOL HasRef, ULONG RefIdx, int *OutLag, double *Rel, double *Corr)
{
    *OutLag = -9999;
    *Rel = 0.0;
    *Corr = 0.0;
    if (m->Valid && HasRef) {
        *OutLag = Lag(m, 0, RefIdx, Corr);
        *Rel = Db(m->Rms[0]) - Db(m->Rms[RefIdx]);
    }
}

/* The pools that differ from the baseline, one line each (before a CHECK). */
static void PrintLedgerDiff(const APOLLO_PLUGIN_POOL_USAGE *Base, const APOLLO_PLUGIN_POOL_USAGE *Now)
{
    ULONG p;

    for (p = 0; p < 4; p++) {
        if (Base[p].Used != Now[p].Used || Base[p].Allocations != Now[p].Allocations || Base[p].Scratch != Now[p].Scratch ||
            Base[p].Holds != Now[p].Holds) {
            printf("  pool %lu: used %lu (baseline %lu), allocations %lu (%lu), scratch 0x%lx (0x%lx), holds %lu (%lu)\n", p,
                   Now[p].Used, Base[p].Used, Now[p].Allocations, Base[p].Allocations, Now[p].Scratch, Base[p].Scratch,
                   Now[p].Holds, Base[p].Holds);
        }
    }
}

static int CmdCreateStart(int n, char **a)
{
    static RECIPE r;
    static APOLLO_PLUGIN_CAPS c0, c1, c2;
    static APOLLO_STRIPS_INFO s0, s2;
    ULONG monSlots[2] = { 26, 27 };     /* the x8p's MON loopback; [P7] the Twin X's below */
    static ULONG pgAt[RECIPE_PAGES_MAX], pgLen[RECIPE_PAGES_MAX];
    MEAS before = { 0 }, during = { 0 }, after = { 0 };
    ULONG slots[8], nsel = 0, dsp, hold = 60, ref = 1, refIdx = 1, k, loadMax = 0, mon = 0, inMask, synch, left;
    ULONG npg = 0, frames, known = 0, pagesMs = 0, baseRegs[11];
    APOLLO_PLUGIN_POOL_USAGE base[4];
    APOLLO_PLUGIN_INSTANCE q, d;
    RB rb;
    const char *end = "destroy", *baseHow = NULL;
    int i, lag0 = -9999, lag1 = -9999, hb;
    double rel0 = 0.0, rel1 = 0.0, cor0 = 0.0, cor1 = 0.0;
    BOOL hasRef, staged, forceStaged = FALSE, tail, level, stereo, strips0, closed;
    TAIL tb = { 0 }, td = { 0 }, ta = { 0 };
    DWORD tStage = 0;

    if (n < 2 || !LoadRecipe(a[1], &r)) {
        return 2;
    }
    dsp = ArgDsp(n, a, 1);
    for (i = 2; i < n; i++) {
        if (i + 1 < n && strcmp(a[i], "--hold") == 0) (void)ParseU(a[i + 1], &hold);
        if (i + 1 < n && strcmp(a[i], "--end") == 0) end = a[i + 1];
        if (i + 1 < n && strcmp(a[i], "--ref") == 0) (void)ParseU(a[i + 1], &ref);
        if (strcmp(a[i], "--mon") == 0) mon = 1;
        if (strcmp(a[i], "--staged") == 0) forceStaged = TRUE;
    }
    if (hold > 600) hold = 600;
    closed = _stricmp(end, "close") == 0;
    tail = strcmp(r.Effect, "tail") == 0;
    level = strcmp(r.Effect, "level") == 0;
    frames = tail ? MEAS_TAIL_FRAMES : MEAS_FRAMES;
    if (dsp == 0 || dsp >= APOLLO_PLUGIN_DSPS) {
        fprintf(stderr, "--dsp %lu: START places on the strip DSP (1)\n", dsp);
        FreeRecipe(&r);
        return 2;
    }
    if (!InputsOk(&r)) {
        Check("layout %s synch %lu/%lu: neither mono on a %s strip input nor a linked pair in1/in1+1 there (run apollo-plug "
              "--unit %s check)", r.Layout, r.In1, r.In2, g_model->Name, g_model->Model == PLUG_MODEL_TWINX ? "twinx" : "x8p");
        FreeRecipe(&r);
        return 1;
    }
    stereo = Stereo(&r);
    inMask = InMask(&r);
    synch = r.In1 | (r.In2 << 16);
    memset(base, 0, sizeof(base));
    memset(baseRegs, 0, sizeof(baseRegs));
    if (!Pages(&r, pgAt, pgLen, &npg)) {
        Check("the startup (%lu words) cannot be cut into pages of whole WRITE blocks: run apollo-plug check", r.NCmd);
        FreeRecipe(&r);
        return 1;
    }
    staged = npg > 1 || forceStaged;
    if (!GetCaps(&c0)) {
        FreeRecipe(&r);
        return 2;
    }
    if (staged && !(c0.Features & APOLLO_PLUGIN_FEAT_STAGE)) {
        Check("the running apollo.sys cannot stage a startup over one page (no FEAT_STAGE): install the build with staged "
              "startup first");
        FreeRecipe(&r);
        return 1;
    }
    if (stereo && !(c0.Features & APOLLO_PLUGIN_FEAT_STEREO)) {
        Check("the running apollo.sys cannot start a stereo chain (Features 0x%02lx, no FEAT_STEREO 0x%02lx): install the build "
              "with stereo inserts first", c0.Features, (ULONG)APOLLO_PLUGIN_FEAT_STEREO);
        FreeRecipe(&r);
        return 1;
    }
    if (!(c0.Dsp[dsp].Flags & APOLLO_PLUGIN_DSP_ADMITTED) || !(c0.Dsp[dsp].Flags & APOLLO_PLUGIN_DSP_STRIPS)) {
        Check("DSP %lu is not the admitted strip DSP (flags 0x%02lx, owned 0x%02lx): power-cycle the %s (strips live)", dsp,
              c0.Dsp[dsp].Flags, c0.OwnedMask, g_model->Name);
        FreeRecipe(&r);
        return 1;
    }
    if ((c0.StripsStarted & inMask) != inMask) {
        if (stereo) Check("the plain strips of inputs %s are not both running (started 0x%02lx): a stereo START needs both live",
                          InNums(&r), c0.StripsStarted);
        else Check("the plain strip of input %lu is not running (started 0x%02lx): START needs it live", r.In1 + 1, c0.StripsStarted);
        FreeRecipe(&r);
        return 1;
    }
    strips0 = GetStrips(&s0);
    Info("%s (%s, %s): startup %lu words in %lu page(s): %s; effect measured as %s on input %lu", r.Name, r.Layout, InText(&r),
         r.NCmd, npg, staged ? "staged (CREATE STAGE, SERVICE pages, readback, START)" : "one CREATE START", r.Effect, r.In1 + 1);
    /* the ledger the restore must come back to: CAPS before the CREATE once DSP's ledger exists; after a
       power cycle apollo.sys builds it inside the first CREATE, so then it is derived after the CREATE */
    if (c0.Dsp[dsp].Flags & APOLLO_PLUGIN_DSP_LEDGER) {
        APOLLO_PLUGIN_POOL_USAGE dv[4];
        BOOL same;

        memcpy(base, c0.Dsp[dsp].Pool, sizeof(base));
        memcpy(baseRegs, c0.Dsp[dsp].Regs, sizeof(baseRegs));
        baseHow = "CAPS before the CREATE";
        same = LedgerBaseline(c0.Dsp[dsp].Regs, TRUE, dv) && SameUsage(dv, base);
        Info("DSP %lu ledger baseline: CAPS before the CREATE (built earlier this epoch): used %lu / %lu / %lu / %lu, scratch "
             "0x%lx / 0x%lx / 0x%lx%s", dsp, base[0].Used, base[1].Used, base[2].Used, base[3].Used, base[1].Scratch,
             base[2].Scratch, base[3].Scratch, c0.Instances != 0 ? "" : same ? "; the one derived from its pool registers is the same" :
             "; the one derived from its pool registers DIFFERS (report it)");
    }
    /* slots: input 1 (its record slot = its input), the reference, the unit's other inputs, the MON loopback
       ([P7] the Twin X: record slots 0/1 = inputs 1/2 and MON loopback 26/27, both MEASURED on the hardware at G3
       from its live IO descriptor (34 record slots, an x8p-like superset), so the same 26/27 as the x8p) */
    slots[nsel++] = r.In1;
    hasRef = ref != r.In1 && ref < g_model->Inputs;
    if (hasRef) {
        refIdx = nsel;
        slots[nsel++] = ref;
    }
    for (k = 0; k < g_model->Inputs && nsel < 6; k++) {
        if (k != r.In1 && (!hasRef || k != ref)) slots[nsel++] = k;
    }
    if (mon && nsel <= 6) {
        slots[nsel++] = monSlots[0];
        slots[nsel++] = monSlots[1];
    }
    if (stereo && hasRef && ref == r.In2) {
        Info("stereo: the reference (input %lu) is the chain's second channel and is processed too: input %lu is measured "
             "against it; a dry reference outside the pair (--ref) is sharper", ref + 1, r.In1 + 1);
    }
    Info("DSP %lu load word before: 0x%08lx", dsp, c0.Dsp[dsp].Load);
    if (Capture(slots, nsel, frames, &before)) {
        PrintMeas("before", &before);
        Against(&before, hasRef, refIdx, &lag0, &rel0, &cor0);
        if (hasRef) {
            Info("before: input %lu vs input %lu: %+.2f dB, lag %d samples (correlation %.2f), HF tilt %+.1f / %+.1f dB",
                 r.In1 + 1, ref + 1, rel0, lag0, cor0, HfTilt(&before, 0), HfTilt(&before, refIdx));
        }
        if (tail) {
            TailStats(&before, 0, refIdx, hasRef, &tb);
            Info("before: %lu short sound(s); tail %+.1f dB%s", tb.Events, tb.Tail, hasRef ? "" : " (no reference)");
            if (hasRef) Info("before: tail against input %lu %+.1f dB", ref + 1, tb.RefTail);
        }
    } else {
        Info("no capture before (needs an elevated handle and the transport running)");
    }

    memset(&q, 0, sizeof(q));
    q.Op = APOLLO_PLUGIN_INST_CREATE;
    q.Flags = (staged ? APOLLO_PLUGIN_F_STAGE : APOLLO_PLUGIN_F_START) | APOLLO_PLUGIN_F_CHAIN;
    q.RenderId = r.RenderId;
    q.Dsp = dsp;
    q.NResources = r.NRes;
    q.NPrivate = r.NRows;
    q.NCommands = staged ? pgLen[0] : r.NCmd;
    q.Synch = synch;
    q.Scratch[0] = r.Scratch[0];
    q.Scratch[1] = r.Scratch[1];
    q.Scratch[2] = r.Scratch[2];
    tStage = GetTickCount();
    if (!Instance(&q, (const UCHAR(*)[32])r.Res, r.Rows, r.Cmd, NULL)) {
        Check("CREATE %s+CHAIN: the IOCTL was refused", staged ? "STAGE" : "START");
        FreeMeas(&before);
        FreeRecipe(&r);
        return 1;
    }
    PrintOutcome(staged ? "CREATE STAGE+CHAIN" : "CREATE START+CHAIN", &q);
    if (q.Status != 0) {
        Check("CREATE %s+CHAIN on DSP %lu %s: 0x%08lx %s at step %s (detail %08lx %08lx %08lx %08lx)",
              staged ? "STAGE" : "START", dsp, InText(&r), (ULONG)q.Status, StatusText(q.Status), StepText(q.Step), q.Detail[0],
              q.Detail[1], q.Detail[2], q.Detail[3]);
        (void)GetCaps(&c1);
        PrintCaps(&c1);
        FreeMeas(&before);
        FreeRecipe(&r);
        return 1;
    }
    if (baseHow == NULL) {
        /* the first CREATE after a power cycle built DSP's ledger: its idle form, as the driver built it */
        (void)GetCaps(&c1);
        if ((c1.Dsp[dsp].Flags & APOLLO_PLUGIN_DSP_LEDGER) && LedgerBaseline(c1.Dsp[dsp].Regs, TRUE, base)) {
            memcpy(baseRegs, c1.Dsp[dsp].Regs, sizeof(baseRegs));
            baseHow = "derived from the pool registers";
            Info("DSP %lu had no ledger before the CREATE (apollo.sys builds it inside the first CREATE after a power cycle): "
                 "baseline derived from its pool registers as the driver builds it (the strips booked, Virtual 0x%02lx, Aux 0x%lx, "
                 "Talkback %lu): used %lu / %lu / %lu / %lu, scratch 0x%lx / 0x%lx / 0x%lx", dsp, g_virtualMask, g_auxMask, g_talkback,
                 base[0].Used, base[1].Used, base[2].Used,
                 base[3].Used, base[1].Scratch, base[2].Scratch, base[3].Scratch);
        } else {
            Info("DSP %lu had no ledger before the CREATE, and %s after it: no ledger baseline", dsp,
                 (c1.Dsp[dsp].Flags & APOLLO_PLUGIN_DSP_LEDGER) ? "its pool registers are not a usable map" : "none");
        }
    }
    if (!staged) {
        Pass("CREATE START+CHAIN: %s running on DSP %lu %s (handle %lu): %lu uploaded, %lu resident (the strip's)",
             r.Name, q.Dsp, InText(&r), q.Handle, q.Uploads, q.Resident);
        ReadBack(&r, &q, &rb);
        if (rb.Fail == 0 && rb.Bad == 0 && rb.Runs == r.NObs) {
            Pass("readback: %lu run(s), %lu word(s) in %lu read(s), every one as written%s", rb.Runs, rb.Words, rb.Reads,
                 RbSkipped(&rb));
        } else {
            Check("readback: %lu of %lu run(s) read (%lu failed), %lu of %lu word(s) differ%s", rb.Runs, r.NObs, rb.Fail, rb.Bad,
                  rb.Words, RbSkipped(&rb));
        }
    } else {
        Pass("CREATE STAGE+CHAIN: %s staged on DSP %lu for %s (handle %lu): %lu uploaded, %lu resident (the strip's); "
             "page 0 (%lu words) written; %s's plain strip%s still run%s", r.Name, q.Dsp, InText(&r), q.Handle, q.Uploads,
             q.Resident, pgLen[0], InText(&r), stereo ? "s" : "", stereo ? "" : "s");
        if (!StagePages(&r, &q, pgAt, pgLen, npg, &pagesMs)) {
            TakeDown(q.Handle, "a startup page failed (nothing was routed: the inputs kept their plain strips)");
            (void)GetCaps(&c1);
            PrintCaps(&c1);
            FreeMeas(&before);
            FreeRecipe(&r);
            return 1;
        }
        Pass("startup pages: %lu SERVICE write(s), %lu words in all, each one transaction the DSP took (%lu ms)", npg - 1,
             r.NCmd - pgLen[0], pagesMs);
        /* the whole startup read back while nothing routes it (plugin_native_chain.py:108-112) */
        ReadBack(&r, &q, &rb);
        if (rb.Fail == 0 && rb.Bad == 0 && rb.Runs == r.NObs) {
            Pass("staged readback: %lu run(s), %lu word(s) in %lu read(s), every one as written%s", rb.Runs, rb.Words, rb.Reads,
                 RbSkipped(&rb));
        } else {
            Check("staged readback: %lu of %lu run(s) read (%lu failed), %lu of %lu word(s) differ%s: not routed", rb.Runs, r.NObs,
                  rb.Fail, rb.Bad, rb.Words, RbSkipped(&rb));
            TakeDown(q.Handle, "the staged readback (nothing was routed)");
            FreeMeas(&before);
            FreeRecipe(&r);
            return 1;
        }
        hb = HeadCheck(&r, pgLen[0], &q, &known);
        if (hb == 0 && known != 0) {
            Pass("chain head: %lu word(s) of row 0 as page 0 wrote them", known);
        } else {
            Check("chain head: %d of %lu word(s) differ (or the read failed): not routed", hb, known);
            TakeDown(q.Handle, "the head check (nothing was routed)");
            FreeMeas(&before);
            FreeRecipe(&r);
            return 1;
        }
        (void)GetCaps(&c1);
        for (k = 0; k < c1.Instances && k < APOLLO_PLUGIN_INSTANCES_MAX; k++) {
            if (c1.Inst[k].Handle == q.Handle) {
                break;
            }
        }
        if (k < c1.Instances && (c1.Inst[k].Flags & APOLLO_PLUGIN_INST_STAGED) && !(c1.Inst[k].Flags & APOLLO_PLUGIN_INST_STARTED) &&
            (c1.StripsStarted & inMask) == inMask) {
            Pass("staged, not routed: %s's plain strip%s still run%s", InText(&r), stereo ? "s" : "", stereo ? "" : "s");
        } else {
            /* Any failure before START takes the chain down, nothing routed (staged-startup review 1). */
            Check("instance %lu after staging: flags 0x%02lx, strips started 0x%02lx: not routed", q.Handle,
                  k < c1.Instances ? c1.Inst[k].Flags : 0, c1.StripsStarted);
            TakeDown(q.Handle, "the pre-START state check (nothing was routed)");
            FreeMeas(&before);
            FreeRecipe(&r);
            return 1;
        }
        memset(&d, 0, sizeof(d));
        d.Op = APOLLO_PLUGIN_INST_START;
        d.Handle = q.Handle;
        d.Epoch = q.Epoch;
        if (!Instance(&d, NULL, NULL, NULL, NULL) || d.Status != 0) {
            PrintOutcome("START", &d);
            Check("START of the staged chain: 0x%08lx %s at step %s", (ULONG)d.Status, StatusText(d.Status), StepText(d.Step));
            (void)GetCaps(&c1);
            PrintCaps(&c1);
            TakeDown(q.Handle, "START");
            FreeMeas(&before);
            FreeRecipe(&r);
            return 1;
        }
        PrintOutcome("START", &d);
        Pass("START: %s routed on DSP %lu %s (plain strip%s stopped, head SYNCH'd, span, route); staged startup %lu ms in all",
             r.Name, dsp, InText(&r), stereo ? "s" : "", GetTickCount() - tStage);
        hb = HeadCheck(&r, pgLen[0], &q, &known);
        if (hb == 0 && known != 0) Pass("chain head while it runs: %lu word(s) of row 0 as written", known);
        else Check("chain head while it runs: %d of %lu word(s) differ (or the read failed)", hb, known);
    }

    /* hold: the load word and the levels every ~5 s */
    for (k = 0; k < hold; k += 5) {
        (void)GetCaps(&c1);
        if (c1.Dsp[dsp].Load > loadMax) loadMax = c1.Dsp[dsp].Load;
        printf("  t=%2lus load 0x%08lx instances %lu%s\n", k, c1.Dsp[dsp].Load, c1.Instances,
               (c1.QuarantinedMask & (1u << dsp)) ? " QUARANTINED" : "");
        if (k == 0 && Capture(slots, nsel, frames, &during)) {
            PrintMeas("during", &during);
        } else if (k != 0) {
            MEAS t;
            if (Capture(slots, nsel, MEAS_FRAMES, &t)) {
                PrintMeas("during", &t);
                FreeMeas(&t);
            }
        }
        Sleep(k + 5 <= hold ? 4500 : 0);
    }
    Info("DSP %lu load word: before 0x%08lx, highest while running 0x%08lx", dsp, c0.Dsp[dsp].Load, loadMax);
    if (loadMax > c0.Dsp[dsp].Load) Pass("DSP %lu load word rose while the chain ran (0x%08lx -> 0x%08lx)", dsp, c0.Dsp[dsp].Load, loadMax);
    else Check("DSP %lu load word did not rise (0x%08lx -> 0x%08lx): its meaning is not proven; report it", dsp, c0.Dsp[dsp].Load, loadMax);

    /* the plug-in's effect on input 1 (in1 of a stereo pair), as the recipe says to measure it */
    Against(&during, hasRef, refIdx, &lag1, &rel1, &cor1);
    if (during.Valid && hasRef) {
        Info("during: input %lu vs input %lu: %+.2f dB, lag %d samples (correlation %.2f), HF tilt %+.1f / %+.1f dB; plug-in "
             "latency %lu", r.In1 + 1, ref + 1, rel1, lag1, cor1, HfTilt(&during, 0), HfTilt(&during, refIdx), r.Latency);
    }
    if (!before.Valid || !during.Valid) {
        Check("no captures before and during: the plug-in's effect on input %lu is not measured", r.In1 + 1);
    } else if (tail) {
        TailStats(&during, 0, refIdx, hasRef, &td);
        Info("during: %lu short sound(s); tail %+.1f dB%s", td.Events, td.Tail, hasRef ? "" : " (no reference)");
        if (hasRef) Info("during: tail against input %lu %+.1f dB", ref + 1, td.RefTail);
        hb = TailVerdict(&tb, &td, hasRef);
        if (hb < 0) {
            Check("too few short sounds on input %lu (%lu before, %lu during): clap or click about once a second, with "
                  "silence between, through the whole run", r.In1 + 1, tb.Events, td.Events);
        } else if (hb > 0) {
            Pass("the plug-in processes input %lu: a tail after its short sounds, %+.1f dB %s (was %+.1f dB)", r.In1 + 1,
                 hasRef ? td.RefTail : td.Tail, hasRef ? "against the dry input 2" : "against their peaks",
                 hasRef ? tb.RefTail : tb.Tail);
        } else {
            Check("no tail added on input %lu: %+.1f dB during vs %+.1f dB before (%.0f dB needed)%s", r.In1 + 1,
                  hasRef ? td.RefTail : td.Tail, hasRef ? tb.RefTail : tb.Tail, TAIL_MIN_DB,
                  hasRef ? "" : "; the same source on input 2 makes this measure sharper");
        }
    } else if (!level && hasRef && r.ExpectShift != -99999 && r.ExpectShift != 0) {
        if (lag0 == -9999 || lag1 == -9999) {
            Check("input %lu's record lag could not be measured (no common source on inputs %lu and %lu)", r.In1 + 1, r.In1 + 1,
                  ref + 1);
        } else if (abs((lag1 - lag0) - (int)r.ExpectShift) <= 2) {
            Pass("the plug-in processes input %lu: its record lag shifted by %d samples (expected %ld: record delay + "
                 "latency %lu against the plain strip's 200)", r.In1 + 1, lag1 - lag0, r.ExpectShift, r.Latency);
        } else {
            Check("input %lu's record lag shifted by %d samples, expected %ld (latency %lu): report it", r.In1 + 1,
                  lag1 - lag0, r.ExpectShift, r.Latency);
        }
    } else {
        SPECT sb, sd;

        SpectOf(&before, hasRef, refIdx, &sb);
        SpectOf(&during, hasRef, refIdx, &sd);
        hb = LevelVerdict(&sb, &sd, hasRef);
        if (hb < 0) {
            Check("input %lu%s silent before or during: make sound into %s for the whole run", r.In1 + 1,
                  hasRef ? " or the reference" : "", hasRef ? "both inputs" : "input 1");
        } else if (hb > 0 && hasRef) {
            Pass("the plug-in's effect is measurable on input %lu against input %lu: level %+.2f -> %+.2f dB, HF tilt %+.1f -> "
                 "%+.1f dB, correlation %.2f -> %.2f, lag %d -> %d", r.In1 + 1, ref + 1, sb.Rel, sd.Rel, sb.Tilt, sd.Tilt,
                 sb.Corr, sd.Corr, sb.Lag, sd.Lag);
        } else if (hb > 0) {
            Pass("the plug-in changes input %lu's spectrum: HF tilt %+.1f -> %+.1f dB (no reference on input 2)", r.In1 + 1,
                 sb.Tilt, sd.Tilt);
        } else if (hasRef) {
            Check("no measurable change on input %lu (level %+.2f -> %+.2f dB, HF tilt %+.1f -> %+.1f dB, correlation %.2f -> "
                  "%.2f, lag %d -> %d): make sound into both inputs", r.In1 + 1, sb.Rel, sd.Rel, sb.Tilt, sd.Tilt, sb.Corr,
                  sd.Corr, sb.Lag, sd.Lag);
        } else {
            Check("no reference on input 2 and input %lu's HF tilt moved %+.1f -> %+.1f dB only (%.0f dB needed): the same "
                  "source on input 2 is needed to prove the effect", r.In1 + 1, sb.Tilt, sd.Tilt, TILT_ALONE_DB);
        }
    }
    if (during.Valid && before.Valid) {
        ULONG same = 0, cmp = 0;
        for (k = 1; k < nsel; k++) {
            if (slots[k] >= g_model->Inputs || (hasRef && k == refIdx) || (stereo && slots[k] == r.In2)) continue;
            cmp++;
            same += (before.Zero[k] == during.Zero[k]);
        }
        if (same == cmp) Pass("inputs other than %s: live/silent as before (%lu checked)", InNums(&r), cmp);
        else Check("inputs other than %s changed between live and exact zero", InNums(&r));
    }

    /* restore: DESTROY, or close the handle */
    if (closed) {
        CloseDevice();
        Sleep(200);
        if (!OpenDevice()) {
            Check("could not reopen the Apollo after closing the handle");
            FreeMeas(&before);
            FreeMeas(&during);
            FreeRecipe(&r);
            return 1;
        }
    } else if (!Destroy(q.Handle, &d) || d.Status != 0) {
        Check("DESTROY: 0x%08lx %s", (ULONG)d.Status, StatusText(d.Status));
    } else {
        PrintOutcome("DESTROY", &d);
        Pass("DESTROY: chain stopped, plain strip%s %s restored, modules released", stereo ? "s" : "", InNums(&r));
    }
    (void)GetCaps(&c2);

    /* after the restore: the driver's own state, not the sound (Nov 15: input 1 against input 2 after the
       restore compared the two outputs of a dual-diaphragm mic, which differ acoustically, not the teardown) */
    left = InstancesLeft(&c2, q.Handle, synch);
    if (left == 0) {
        Pass("%s: no instance of ours left (handle %lu gone, nothing on %s; %lu instance(s) on the unit)",
             closed ? "handle closed" : "after DESTROY", q.Handle, InText(&r), c2.Instances);
    } else {
        Check("%s: %lu instance(s) left with handle %lu or on %s", closed ? "handle closed" : "after DESTROY", left, q.Handle,
              InText(&r));
    }
    if ((c2.StripsStarted & inMask) == inMask && c2.StripsState == APOLLO_STRIPS_STATE_LIVE && c2.QuarantinedMask == 0) {
        Pass("strips live, %s's plain strip%s retained, no DSP quarantined", InText(&r), stereo ? "s" : "");
    } else {
        Check("after restore: strips state %lu, started 0x%02lx, quarantined 0x%02lx", c2.StripsState, c2.StripsStarted,
              c2.QuarantinedMask);
    }
    if (baseHow == NULL) {
        Check("DSP %lu's ledger: no baseline to compare against (CAPS reported no usable ledger)", dsp);
    } else if (!(c2.Dsp[dsp].Flags & APOLLO_PLUGIN_DSP_LEDGER)) {
        Check("DSP %lu reports no ledger after the restore (flags 0x%02lx)", dsp, c2.Dsp[dsp].Flags);
    } else if (SameUsage(base, c2.Dsp[dsp].Pool)) {
        Pass("DSP %lu's ledger back to its baseline (the strips' room only; baseline: %s)", dsp, baseHow);
    } else {
        PrintLedgerDiff(base, c2.Dsp[dsp].Pool);
        Check("DSP %lu's ledger differs from its baseline (%s)%s", dsp, baseHow,
              memcmp(baseRegs, c2.Dsp[dsp].Regs, sizeof(baseRegs)) != 0 ? "; its pool registers changed since" : "");
    }
    if (!GetStrips(&s2)) {
        Check("GET_STRIPS after the restore failed (error %lu): the input strips' own state is not proven", GetLastError());
    } else {
        ULONG in[2], bad = 0;

        in[0] = r.In1;
        in[1] = stereo ? r.In2 : r.In1;
        for (k = 0; k < (stereo ? 2u : 1u); k++) {
            ULONG x = in[k];

            if (s2.StripState[x] != APOLLO_STRIP_TAKEN || !s2.StripStarted[x] || s2.StripHeader[x] == 0 ||
                (strips0 && s2.StripHeader[x] != s0.StripHeader[x])) {
                bad++;
                printf("  input %lu: strip state %lu (%lu = taken), started %lu, header 0x%05lx (before the CREATE 0x%05lx)\n", x + 1,
                       s2.StripState[x], (ULONG)APOLLO_STRIP_TAKEN, s2.StripStarted[x], s2.StripHeader[x],
                       strips0 ? s0.StripHeader[x] : 0ul);
            }
        }
        if (bad == 0 && s2.State == APOLLO_STRIPS_STATE_LIVE && !(s2.Flags & APOLLO_STRIPS_FLAG_QUARANTINED)) {
            if (stereo) {
                Pass("GET_STRIPS: the plain strips of inputs %s taken and started, headers 0x%05lx/0x%05lx%s", InNums(&r),
                     s2.StripHeader[in[0]], s2.StripHeader[in[1]], strips0 ? " as before the CREATE" : "");
            } else {
                Pass("GET_STRIPS: input %lu's plain strip taken and started, header 0x%05lx%s", r.In1 + 1, s2.StripHeader[in[0]],
                     strips0 ? " as before the CREATE" : "");
            }
        } else {
            Check("GET_STRIPS after the restore: strips state %lu, flags 0x%02lx; %lu of %s's plain strip(s) not taken and started "
                  "at their header", s2.State, s2.Flags, bad, InText(&r));
        }
    }
    Sleep(500);
    (void)GetCaps(&c2);
    Info("DSP %lu load word after: 0x%08lx", dsp, c2.Dsp[dsp].Load);
    if (Capture(slots, nsel, frames, &after)) {
        PrintMeas("after", &after);
        if (hasRef) {
            double c = 0.0;
            int lag2 = Lag(&after, 0, refIdx, &c);

            Info("after: input %lu vs input %lu: %+.2f dB, lag %d samples (correlation %.2f): information only (two sources, "
                 "not a teardown check)", r.In1 + 1, ref + 1, Db(after.Rms[0]) - Db(after.Rms[refIdx]), lag2, c);
        }
        if (before.Valid) {
            /* exact zero only: whether input 1 carries anything, not how loud (the sources themselves vary) */
            if (before.Zero[0] == after.Zero[0]) Pass("input %lu: %s before and after", r.In1 + 1, after.Zero[0] ? "silent" : "live");
            else Check("input %lu: %s before, %s after", r.In1 + 1, before.Zero[0] ? "zero" : "live", after.Zero[0] ? "zero" : "live");
        }
        if (tail && before.Valid && during.Valid) {
            /* the reverb's own proof, as gate P1 ran it (Lexicon 480L, Nov 15): a tail added to the short sounds is gone
               again. A reference only normalizes the sounds' decay; a tail is not something two diaphragms differ by. */
            TailStats(&after, 0, refIdx, hasRef, &ta);
            if (ta.Events >= 2 && tb.Events >= 2) {
                double was = hasRef ? tb.RefTail : tb.Tail, now = hasRef ? ta.RefTail : ta.Tail;
                if (now - was <= TAIL_MIN_DB / 2) Pass("the tail is gone after the restore: %+.1f dB (before %+.1f dB)", now, was);
                else Check("input %lu still has a tail after the restore: %+.1f dB (before %+.1f dB)", r.In1 + 1, now, was);
            } else {
                Info("after: %lu short sound(s): too few to compare the tail", ta.Events);
            }
        }
    }
    FreeMeas(&before);
    FreeMeas(&during);
    FreeMeas(&after);
    FreeRecipe(&r);
    printf("\nSummary: %d PASS, %d CHECK\n", g_pass, g_check);
    return g_check ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* selftest: the effect measurements, the page cut, the readback grouping and DSP-owned words,   */
/* the ledger baseline and the inputs, on synthetic data (no device, no recipe, no UA data)      */
/* ------------------------------------------------------------------ */

static ULONG g_rng = 12345u;

static double Noise(void)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return (double)(g_rng >> 8) / 8388608.0 - 1.0;          /* [-1, 1) */
}

/* A two-slot capture (slot 0 = input 1, slot 1 = input 2) of Frames from two signal arrays. */
static BOOL SynthMeas(MEAS *m, const double *In1, const double *In2, ULONG Frames)
{
    ULONG t;

    memset(m, 0, sizeof(*m));
    m->Buf = (LONG *)calloc((size_t)Frames * 2, sizeof(LONG));
    if (m->Buf == NULL) {
        return FALSE;
    }
    for (t = 0; t < Frames; t++) {
        double a = In1[t] > 0.999 ? 0.999 : (In1[t] < -0.999 ? -0.999 : In1[t]);
        double b = In2[t] > 0.999 ? 0.999 : (In2[t] < -0.999 ? -0.999 : In2[t]);
        m->Buf[2 * (size_t)t] = (LONG)(a * FULL_SCALE);
        m->Buf[2 * (size_t)t + 1] = (LONG)(b * FULL_SCALE);
    }
    m->Frames = Frames;
    m->NSel = 2;
    m->Slot[0] = 0;
    m->Slot[1] = 1;
    MeasStats(m);
    return TRUE;
}

/*
 * The ledger baseline, the readback's DSP-owned words and second-read rule, the recipe's inputs
 * (gate P1 findings): pure functions on synthetic values (no recipe, no UA data).
 */
static void SelfTestState(void)
{
    static PLUG_LEDGER L;
    static RECIPE rr;
    /* a chain on the strip DSP: one shared module (code) and three rows, as a CREATE reserves them */
    static const PLUG_REQ rq[4] = { { 0, 628, 0x123, 1 }, { 1, 64, 0, 0 }, { 2, 34, 0, 0 }, { 3, 16, 0, 0 } };
    static const struct { const char *Layout; ULONG In1, In2, Ok; } cs[] = {
        { "mono", 0, 0x20, 1 }, { "mono", 7, 0x20, 1 }, { "stereo", 0, 1, 1 }, { "stereo", 2, 3, 1 }, { "stereo", 6, 7, 1 },
        { "mono", 8, 0x20, 0 }, { "mono", 0, 1, 0 }, { "stereo", 0, 0x20, 0 }, { "stereo", 0, 2, 0 }, { "stereo", 7, 8, 0 },
        { "", 0, 0x20, 0 } };
    APOLLO_PLUGIN_POOL_USAGE b[4], now[4], none[4];
    PLUG_LEASE book[4], got[4];
    PLUG_USAGE u[4];
    ULONG sb[4] = { 0, 0, 0, 0 }, v1[8], v2[8], ok, p, k;
    BOOL placedDiffers;
    RB c;

    /* 1. the ledger baseline a first CREATE after a power cycle compares against */
    ok = LedgerBaseline(g_x8pDsp1Regs, TRUE, b) && b[0].Used == STRIP_CODE_WORDS && b[1].Used == 8 * STRIP_POOL1_PER_STRIP &&
         b[2].Used == 8 * STRIP_POOL2_PER_STRIP && b[3].Used == 8 * STRIP_POOL3_PER_STRIP && b[1].Scratch == PLUG_STRIP_SCRATCH1 &&
         b[2].Scratch == PLUG_STRIP_SCRATCH2 && b[3].Scratch == 0x8000;
    for (p = 0; p < 4; p++) {
        ok &= b[p].Allocations == 1 && b[p].Holds == 1;
    }
    if (ok) Pass("ledger baseline from the x8p's DSP 1 pool registers: the 8 strips' room (used %lu / %lu / %lu / %lu, one "
                 "allocation each), scratch 0x%lx / 0x%lx / 0x%lx", b[0].Used, b[1].Used, b[2].Used, b[3].Used, b[1].Scratch,
                 b[2].Scratch, b[3].Scratch);
    else Check("ledger baseline from the measured pool registers: used %lu / %lu / %lu / %lu, scratch 0x%lx / 0x%lx / 0x%lx",
               b[0].Used, b[1].Used, b[2].Used, b[3].Used, b[1].Scratch, b[2].Scratch, b[3].Scratch);
    /* the driver's ledger: built at the CREATE, the chain's scratch raised and its leases taken (PlugScratchRaise,
       the placement), then everything given back and the scratch restored (PlugScratchRestore) */
    ok = BuildLedger(&L, g_x8pDsp1Regs, TRUE, book) == PLUG_OK;
    for (p = 1; p < 4; p++) {
        sb[p] = L.Scratch[p];
    }
    ok &= PlugLedgerSetScratch(&L, 2, 192) == PLUG_OK && PlugLedgerReserveBatch(&L, rq, 4, got) == PLUG_OK;
    PlugLedgerUsage(&L, u);
    UsageToCaps(u, now);
    placedDiffers = !SameUsage(b, now);
    for (p = 0; p < 4; p++) {
        ok &= PlugLedgerRelease(&L, &got[p], NULL) == PLUG_OK;
    }
    for (p = 1; p < 4; p++) {
        ok &= PlugLedgerSetScratch(&L, p, sb[p]) == PLUG_OK;
    }
    PlugLedgerUsage(&L, u);
    UsageToCaps(u, now);
    memset(none, 0, sizeof(none));
    if (ok && placedDiffers && SameUsage(b, now) && !SameUsage(b, none)) {
        Pass("ledger baseline: a placed chain differs from it, the same ledger after DESTROY equals it, and CAPS without a "
             "ledger (before a first CREATE) is never taken for it");
    } else {
        Check("ledger baseline: placed differs %d, released equals %d, an empty CAPS differs %d", placedDiffers, SameUsage(b, now),
              !SameUsage(b, none));
    }

    /* 2. readback: row 7 runs at words 5 (DSP-owned), 7..10 and 12: one read of 8 words (6 and 11 read, not compared) */
    memset(&rr, 0, sizeof(rr));
    rr.Obs = (OBS *)calloc(3, sizeof(OBS));
    if (rr.Obs == NULL) {
        Check("readback rule: no memory");
        return;
    }
    rr.Obs[0].Row = 7; rr.Obs[0].Word = 5; rr.Obs[0].Count = 1; rr.Obs[0].V[0] = 0;
    rr.Obs[1].Row = 7; rr.Obs[1].Word = 7; rr.Obs[1].Count = 4;
    rr.Obs[1].V[0] = 0x11111111; rr.Obs[1].V[1] = 0x22222222; rr.Obs[1].V[2] = 0x33333333; rr.Obs[1].V[3] = 0x44444444;
    rr.Obs[2].Row = 7; rr.Obs[2].Word = 12; rr.Obs[2].Count = 1; rr.Obs[2].V[0] = 0x55555555;
    rr.NObs = 3;
    rr.OwnedRow[0] = 7;
    rr.OwnedWord[0] = 5;
    strcpy_s(rr.OwnedSrc[0], sizeof(rr.OwnedSrc[0]), "meter");
    rr.NOwned = rr.NOwnedRecipe = 1;
    v1[0] = 0x3e000001;                                     /* word 5: the meter moved */
    v1[1] = 0xdeadbeef;                                     /* word 6: between runs */
    v1[2] = 0x11111111; v1[3] = 0x22222222; v1[4] = 0x33333333; v1[5] = 0x44444444;
    v1[6] = 0xdeadbeef;                                     /* word 11: between runs */
    v1[7] = 0x55555555;
    memset(&c, 0, sizeof(c));
    ok = ReadGroupEnd(&rr, 0) == 3 && ReadGroupSpan(&rr, 0, 3) == 8 && GroupDiffs(&rr, 0, 3, v1) == 0;
    GroupClassify(&rr, 0, 3, v1, NULL, &c, TRUE);
    ok &= c.Runs == 3 && c.Words == 5 && c.Bad == 0 && c.Owned == 1 && c.Moving == 0;
    if (ok) Pass("readback: a dsp_owned word is never compared (5 words compared, 1 DSP-owned skipped, none differ)");
    else Check("readback: dsp_owned skip: %lu runs, %lu compared, %lu differ, %lu owned", c.Runs, c.Words, c.Bad, c.Owned);
    /* word 8 differs and moves between the two reads: the DSP's own, learned for the rest of the run */
    memcpy(v2, v1, sizeof(v2));
    v1[3] = 0x22220001;
    v2[3] = 0x22220002;
    memset(&c, 0, sizeof(c));
    ok = GroupDiffs(&rr, 0, 3, v1) == 1;
    GroupClassify(&rr, 0, 3, v1, v2, &c, TRUE);
    ok &= c.Bad == 0 && c.Moving == 1 && c.Words == 4 && rr.NOwned == 2 && IsOwned(&rr, 7, 8) &&
          strcmp(rr.OwnedSrc[1], "moving") == 0;
    memset(&c, 0, sizeof(c));
    ok &= GroupDiffs(&rr, 0, 3, v1) == 0;
    GroupClassify(&rr, 0, 3, v1, NULL, &c, TRUE);
    ok &= c.Bad == 0 && c.Owned == 2 && c.Moving == 0;
    /* moved back to the written value between the reads: still moving */
    ok &= ReadbackWord(FALSE, 0x22222222, 0x22220001, TRUE, 0x22222222) == RB_MOVES;
    if (ok) Pass("readback: a word that moves between two reads is the DSP's (not a difference, learned, skipped after)");
    else Check("readback: moving word: %lu differ, %lu moving, %lu owned", c.Bad, c.Moving, c.Owned);
    /* the same wrong value twice, or no second read: a difference */
    rr.NOwned = 1;
    v2[3] = v1[3];
    memset(&c, 0, sizeof(c));
    GroupClassify(&rr, 0, 3, v1, v2, &c, TRUE);
    ok = c.Bad == 1 && c.Moving == 0 && c.Words == 5;
    memset(&c, 0, sizeof(c));
    GroupClassify(&rr, 0, 3, v1, NULL, &c, TRUE);
    ok &= c.Bad == 1 && c.Moving == 0 && rr.NOwned == 1;
    ok &= ReadbackWord(TRUE, 1, 2, TRUE, 3) == RB_OWNED && ReadbackWord(FALSE, 1, 1, FALSE, 0) == RB_SAME &&
          ReadbackWord(FALSE, 1, 2, TRUE, 2) == RB_DIFFERS && ReadbackWord(FALSE, 1, 2, FALSE, 0) == RB_DIFFERS;
    if (ok) Pass("readback: the same wrong value twice, or no second read, stays a difference");
    else Check("readback: a wrong value read twice: %lu differ, %lu moving", c.Bad, c.Moving);
    free(rr.Obs);
    rr.Obs = NULL;

    /* 3. the recipe's inputs: mono on inputs 1-8, stereo pairs in1/in1+1 */
    ok = 1;
    for (k = 0; k < sizeof(cs) / sizeof(cs[0]); k++) {
        memset(&rr, 0, sizeof(rr));
        strcpy_s(rr.Layout, sizeof(rr.Layout), cs[k].Layout);
        rr.In1 = cs[k].In1;
        rr.In2 = cs[k].In2;
        ok &= (InputsOk(&rr) ? 1u : 0u) == cs[k].Ok;
        if (cs[k].Ok) {
            ok &= InMask(&rr) == ((1u << cs[k].In1) | (cs[k].In2 < 8 ? 1u << cs[k].In2 : 0u));
        }
    }
    if (ok) Pass("inputs: mono on 1-8 and stereo pairs in1/in1+1 accepted (both strips in the mask); input 9, a mono pair, "
                 "a stereo 0x20 or a gap refused");
    else Check("inputs: the mono / stereo rule differs");
}

static int CmdSelfTest(void)
{
    const ULONG F = MEAS_TAIL_FRAMES;
    double *dry = (double *)calloc(F, sizeof(double)), *in1 = (double *)calloc(F, sizeof(double)),
           *rev = (double *)calloc(F, sizeof(double)), *amp = (double *)calloc(F, sizeof(double)),
           *sil = (double *)calloc(F, sizeof(double)), *cont = (double *)calloc(F, sizeof(double)),
           *del = (double *)calloc(F, sizeof(double));
    MEAS mb = { 0 }, md = { 0 }, mn = { 0 }, ms = { 0 }, ca = { 0 }, cb = { 0 }, cd = { 0 };
    TAIL tb, td, tn, tsil;
    SPECT sb, sd, sn;
    ULONG t, k, at[RECIPE_PAGES_MAX], len[RECIPE_PAGES_MAX], npg = 0;
    double lp = 0.0, c = 0.0;
    static RECIPE r;
    int v;

    if (!dry || !in1 || !rev || !amp || !sil || !cont || !del) {
        fprintf(stderr, "no memory\n");
        return 2;
    }
    /* clicks every 0.6 s (3 ms bursts, 4 ms decay) on a -70 dB room; the two inputs carry the same source */
    for (t = 0; t < F; t++) {
        double ts = (double)t / 48000.0, x = 3e-4 * Noise();
        for (k = 0; k < 5; k++) {
            double t0 = 0.25 + 0.6 * k, u = ts - t0;
            if (u >= 0.0 && u < 0.003) x += 0.5 * Noise() * exp(-u / 0.004);
        }
        dry[t] = x;
        in1[t] = x + 3e-4 * Noise();
    }
    /* the reverb: the dry clicks at 0.7 plus a noise tail per click (10 ms pre-delay, RT60 about 1.7 s) */
    for (t = 0; t < F; t++) {
        double ts = (double)t / 48000.0, y = 0.7 * in1[t];
        for (k = 0; k < 5; k++) {
            double u = ts - (0.25 + 0.6 * k) - 0.010;
            if (u >= 0.0) y += 0.05 * Noise() * exp(-u / 0.25);
        }
        rev[t] = y;
    }
    (void)SynthMeas(&mb, in1, dry, F);
    (void)SynthMeas(&md, rev, dry, F);
    (void)SynthMeas(&mn, in1, dry, F);
    (void)SynthMeas(&ms, sil, sil, F);
    TailStats(&mb, 0, 1, TRUE, &tb);
    TailStats(&md, 0, 1, TRUE, &td);
    TailStats(&mn, 0, 1, TRUE, &tn);
    TailStats(&ms, 0, 1, TRUE, &tsil);
    printf("tail: dry %lu event(s) tail %+.1f ref %+.1f; reverb %lu event(s) tail %+.1f ref %+.1f\n", tb.Events, tb.Tail,
           tb.RefTail, td.Events, td.Tail, td.RefTail);
    if (tb.Events >= 4 && td.Events >= 4) Pass("tail: the clicks are found (%lu dry, %lu with the reverb)", tb.Events, td.Events);
    else Check("tail: clicks found %lu / %lu", tb.Events, td.Events);
    if (TailVerdict(&tb, &td, TRUE) == 1) Pass("tail against input 2: the reverb is seen (%+.1f -> %+.1f dB)", tb.RefTail, td.RefTail);
    else Check("tail against input 2: the reverb is not seen (%+.1f -> %+.1f dB)", tb.RefTail, td.RefTail);
    if (TailVerdict(&tb, &tn, TRUE) == 0) Pass("tail against input 2: nothing seen when nothing changed (%+.1f -> %+.1f dB)",
                                              tb.RefTail, tn.RefTail);
    else Check("tail against input 2: a false tail (%+.1f -> %+.1f dB)", tb.RefTail, tn.RefTail);
    TailStats(&mb, 0, 1, FALSE, &tb);
    TailStats(&md, 0, 1, FALSE, &td);
    TailStats(&mn, 0, 1, FALSE, &tn);
    if (TailVerdict(&tb, &td, FALSE) == 1) Pass("tail on input 1 alone: the reverb is seen (%+.1f -> %+.1f dB)", tb.Tail, td.Tail);
    else Check("tail on input 1 alone: the reverb is not seen (%+.1f -> %+.1f dB)", tb.Tail, td.Tail);
    if (TailVerdict(&tb, &tn, FALSE) == 0) Pass("tail on input 1 alone: nothing seen when nothing changed");
    else Check("tail on input 1 alone: a false tail (%+.1f -> %+.1f dB)", tb.Tail, tn.Tail);
    if (TailVerdict(&tb, &tsil, TRUE) == -1) Pass("tail: a silent capture is 'too few short sounds', never a pass");
    else Check("tail: a silent capture gave a verdict");

    /* an amp: low-passed (one pole, 2 kHz) and clipped; the source is modulated noise on both inputs */
    for (t = 0; t < F; t++) {
        double x = 0.2 * Noise() * (0.6 + 0.4 * sin(2.0 * 3.14159265358979 * 4.0 * (double)t / 48000.0));
        cont[t] = x;
        lp += 0.23 * (x - lp);
        amp[t] = 0.5 * tanh(4.0 * lp);
        del[t] = t >= 64 ? cont[t - 64] : 0.0;
    }
    (void)SynthMeas(&ca, cont, cont, F);
    (void)SynthMeas(&cb, amp, cont, F);
    (void)SynthMeas(&cd, del, cont, F);
    SpectOf(&ca, TRUE, 1, &sb);
    SpectOf(&cb, TRUE, 1, &sd);
    SpectOf(&ca, TRUE, 1, &sn);
    printf("level: plain rel %+.2f tilt %+.1f corr %.2f lag %d; amp rel %+.2f tilt %+.1f corr %.2f lag %d\n", sb.Rel, sb.Tilt,
           sb.Corr, sb.Lag, sd.Rel, sd.Tilt, sd.Corr, sd.Lag);
    if (LevelVerdict(&sb, &sd, TRUE) == 1) Pass("level against input 2: the amp is seen");
    else Check("level against input 2: the amp is not seen");
    if (LevelVerdict(&sb, &sn, TRUE) == 0) Pass("level against input 2: nothing seen when nothing changed");
    else Check("level against input 2: a false change");
    SpectOf(&ca, FALSE, 1, &sb);
    SpectOf(&cb, FALSE, 1, &sd);
    if (LevelVerdict(&sb, &sd, FALSE) == 1) Pass("input 1 alone: the amp's spectrum is seen (HF tilt %+.1f -> %+.1f dB)", sb.Tilt, sd.Tilt);
    else Check("input 1 alone: the amp's spectrum is not seen (HF tilt %+.1f -> %+.1f dB)", sb.Tilt, sd.Tilt);
    SpectOf(&ms, TRUE, 1, &sd);
    if (LevelVerdict(&sb, &sd, TRUE) == -1) Pass("level: a silent capture is refused, never a pass");
    else Check("level: a silent capture gave a verdict");
    v = Lag(&cd, 0, 1, &c);
    if (v == 64) Pass("lag: a 64-sample delay on input 1 measures 64 (correlation %.2f)", c);
    else Check("lag: a 64-sample delay measured %d", v);

    /* the page cut (upstream test_long_plan_splits_...: 25 x 100 words -> 1000, 1000, 500) and the readback groups */
    memset(&r, 0, sizeof(r));
    r.Cmd = (ULONG *)calloc(RECIPE_CMD_MAX, sizeof(ULONG));
    r.Obs = (OBS *)calloc(16, sizeof(OBS));
    if (r.Cmd != NULL && r.Obs != NULL) {
        for (k = 0; k < 25; k++) {
            r.Cmd[r.NCmd++] = 0x00140000u | 100;
            r.Cmd[r.NCmd++] = (10u << 24) | (k * 100);
            r.Cmd[r.NCmd++] = 0;
            for (t = 0; t < 97; t++) r.Cmd[r.NCmd++] = t;
        }
        r.NChunk = 3;
        r.ChunkAt[0] = 0; r.ChunkLen[0] = 1000;
        r.ChunkAt[1] = 1000; r.ChunkLen[1] = 1000;
        r.ChunkAt[2] = 2000; r.ChunkLen[2] = 500;
        if (Pages(&r, at, len, &npg) && npg == 3 && PagesMatchUpstream(&r, at, len, npg)) Pass("pages: upstream's 1000, 1000, 500");
        else Check("pages: %lu page(s), not upstream's cut", npg);
        r.ChunkLen[2] = 499;
        if (!PagesMatchUpstream(&r, at, len, npg)) Pass("pages: a different cut is noticed");
        else Check("pages: a different cut passed");
        {
            static const ULONG obs[8][3] = { { 10, 0, 16 }, { 10, 16, 16 }, { 10, 32, 16 }, { 10, 48, 16 }, { 10, 64, 2 },
                                             { 11, 0, 4 }, { 10, 100, 3 }, { 10, 90, 2 } };
            static const ULONG want[] = { 4, 5, 6, 7, 8 };
            ULONG i = 0, g = 0, ok = 1;

            for (k = 0; k < 8; k++) {
                r.Obs[k].Row = obs[k][0];
                r.Obs[k].Word = obs[k][1];
                r.Obs[k].Count = obs[k][2];
            }
            r.NObs = 8;
            while (i < r.NObs && g < 5) {
                ULONG j = ReadGroupEnd(&r, i);
                ok &= j == want[g] && ReadGroupSpan(&r, i, j) <= PLUG_READ_WORDS_MAX;
                i = j;
                g++;
            }
            ok &= i == r.NObs && ReadGroupSpan(&r, 0, 4) == 64;
            if (ok) Pass("readback groups: one row, ascending, at most 64 words per read (8 runs in 5 reads)");
            else Check("readback groups differ");
        }
    }
    FreeRecipe(&r);
    SelfTestState();
    FreeMeas(&mb);
    FreeMeas(&md);
    FreeMeas(&mn);
    FreeMeas(&ms);
    FreeMeas(&ca);
    FreeMeas(&cb);
    FreeMeas(&cd);
    free(dry); free(in1); free(rev); free(amp); free(sil); free(cont); free(del);
    printf("\nselftest: %d PASS, %d CHECK\n", g_pass, g_check);
    return g_check ? 1 : 0;
}

/* ------------------------------------------------------------------ */

static void Usage(void)
{
    fprintf(stderr,
            "usage: apollo-plug [--unit x8p|twinx|N] <command>\n"
            "  caps                                   the plug-in host, DSPs, ledgers, instances\n"
            "  check <recipe> [--store DIR]           offline: the driver's checks and encodings for the recipe\n"
            "  selftest                               offline: the effect measurements, page cut, readback groups and\n"
            "                                         DSP-owned words, the ledger baseline, mono / stereo inputs\n"
            "  modules install <recipe> --from DIR    elevated: the recipe's modules into System32\\drivers\\ApolloModules\n"
            "  modules verify <recipe>                elevated: the installed copies\n"
            "  licence [--recipe R] [--names F] [--authorized-out F]   the board query (counts only)\n"
            "  create-check <recipe>                  CREATE with CHECK: the licence answer, nothing placed\n"
            "  create-plain <recipe> --dsp N          CREATE unrouted (the plug-in's own modules and rows), then DESTROY\n"
            "  create-start <recipe> --dsp 1 [--hold S] [--end destroy|close] [--ref SLOT] [--mon] [--staged]\n"
            "                                         CREATE START+CHAIN on the recipe's input (or stereo pair), readback,\n"
            "                                         hold, restore, the driver's state after it;\n"
            "                                         a startup over one page (or --staged): CREATE STAGE+CHAIN, the\n"
            "                                         other pages by SERVICE, readback while staged, then START\n");
}

int main(int argc, char **argv)
{
    int i = 1, rc;
    const char *cmd;

    g_model = PlugModelFor(PLUG_MODEL_X8P);
    if (i + 1 < argc && strcmp(argv[i], "--unit") == 0) {
        /* [P7] twinx: the Twin X Gen 1 (offline commands use its plan too: check, its inputs and ledger) */
        if (_stricmp(argv[i + 1], "twinx") == 0) { g_want = APOLLO_PICK_TWINX; g_model = PlugModelFor(PLUG_MODEL_TWINX); }
        else if (_stricmp(argv[i + 1], "x8p") == 0) g_want = APOLLO_PICK_X8P;
        else g_want = atoi(argv[i + 1]);
        i += 2;
    }
    if (i >= argc) {
        Usage();
        return 2;
    }
    cmd = argv[i];
    if (_stricmp(cmd, "check") == 0) {
        return CmdCheck(argc - i, argv + i);
    }
    if (_stricmp(cmd, "modules") == 0) {
        return CmdModules(argc - i, argv + i);
    }
    if (_stricmp(cmd, "selftest") == 0) {
        g_model = PlugModelFor(PLUG_MODEL_X8P);     /* its cases are the x8p's */
        return CmdSelfTest();
    }
    if (!OpenDevice()) {
        return 2;
    }
    if (_stricmp(cmd, "caps") == 0) rc = CmdCaps();
    else if (_stricmp(cmd, "licence") == 0 || _stricmp(cmd, "license") == 0) rc = CmdLicence(argc - i, argv + i);
    else if (_stricmp(cmd, "create-check") == 0) rc = CmdCreateCheck(argc - i, argv + i);
    else if (_stricmp(cmd, "create-plain") == 0) rc = CmdCreatePlain(argc - i, argv + i);
    else if (_stricmp(cmd, "create-start") == 0) rc = CmdCreateStart(argc - i, argv + i);
    else {
        Usage();
        rc = 2;
    }
    CloseDevice();
    return rc;
}
