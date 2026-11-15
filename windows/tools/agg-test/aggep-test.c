/*
 * aggep-test.c - offline checks of the gate G7 pieces of driver/apollo_agg.h (docs/spec-twinx.md 6.7,
 * 6.9.7): the driver's own code, compiled in user mode.
 *
 *   1. which endpoint table a unit gets: EndpointSet absent/0/unknown = its standalone table on both
 *      models (exactly as before G7); 1 = the combined table on the x8p's hardware ID, none on the
 *      Twin X's;
 *   2. the combined table (APOLLO_AGG_EP_LIST): names, form factors, kinds, IDs, default mask;
 *      [Endpoints] and the standalone tables (endpoint_plan.h): every output jack, the loopback, the
 *      inputs, the combined rows carrying exactly the standalone rows' jacks;
 *   3. driver/apollo.inf: every combined reference string has its three interfaces on the x8p's
 *      install section only, FriendlyName "Apollo", NeverSetAsDefault exactly on the monitor-mix
 *      loopback; [Endpoints] every standalone row of both models likewise on its own section; no
 *      reference string of an earlier build left; the device names without "(open driver, test build)";
 *   4. where each combined endpoint plays / records from (ApolloAggEpResolve) for the presets
 *      (none, Studio, Mirror, Headphones split), master changes, absent and never-seen units;
 *   5. Align and trims: with ALIGN set and every trim 0 the G7 layout compile, pad and follower
 *      capture offset are byte-for-byte G6's (ref-g6.c runs the frozen G6 header) over random
 *      configurations; unaligned and trimmed configurations keep the resampler's look-ahead margin;
 *   6. the per-tick window starts and the timeline takeover / master reclaim decision;
 *   7. the start-ratio table (find, keep, threshold, replacement, reverse pair);
 *   8. [polish Nov 15] the combined paths per sample: the master (timeline) channels an integer copy, bit-exact
 *      through a master-only loop; the follower path unchanged; why the G7 loop CHECK was not a fractional offset.
 * Exit code 0 = every check passed.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>
#include "..\..\include\apollo_ioctl.h"
#include "..\..\driver\apollo_agg.h"
#include "..\..\driver\apollo_unit.h"
#include "..\..\driver\apollo_sat.h"      /* [polish] ApolloSatAdd32, as agg.c AggSat */

/* ref-g6.c */
typedef struct _REF_RT {
    int             Present;
    unsigned long   Guard, Lead, Adc, FrameCfg;
} REF_RT;
unsigned long RefG6CompiledBytes(void);
void RefG6Compile(const APOLLO_AGG_CONFIG *C, const void *Rt, unsigned long Timeline, void *Out);
unsigned long RefG6Pad(const APOLLO_AGG_CONFIG *C, const void *Rt, unsigned long Timeline);
long RefG6DAdc(const void *Rt, unsigned long Timeline, unsigned long U);

static int g_fail, g_pass;

static void Check(int ok, const char *what)
{
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        printf("FAIL: %s\n", what);
    }
}

static unsigned long long g_rng = 0x2545F4914F6CDD1Dull;
static unsigned long Rnd(unsigned long n)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return n ? (unsigned long)((g_rng >> 17) % n) : 0;
}

/* the descriptors as read on the hardware (asrc-test.c, docs/spec-twinx.md 6.9.3) */
static const unsigned short kX8pIn[32] = {
    0x0001, 0x0002, 0x0103, 0x0104, 0x0305, 0x0306, 0x0307, 0x0308, 0x0401, 0x0402, 0x0403, 0x0404, 0x0405, 0x0406,
    0x0407, 0x0408, 0x0501, 0x0502, 0x0701, 0x0702, 0x0703, 0x0704, 0x0705, 0x0706, 0x0707, 0x0708, 0x0901, 0x0902,
    0x0201, 0x0202, 0x0203, 0x0204 };
static const unsigned short kX8pOut[34] = {
    0x0901, 0x0902, 0x0301, 0x0302, 0x0303, 0x0304, 0x0305, 0x0306, 0x0307, 0x0308, 0x0401, 0x0402, 0x0403, 0x0404,
    0x0405, 0x0406, 0x0407, 0x0408, 0x0501, 0x0502, 0x0701, 0x0702, 0x0703, 0x0704, 0x0705, 0x0706, 0x0707, 0x0708,
    0x0A01, 0x0A02, 0x0A03, 0x0A04, 0x0A05, 0x0A06 };
static const unsigned short kTwxIn[34] = {
    0x0001, 0x0002, 0x0103, 0x0104, 0x0305, 0x0306, 0x0307, 0x0308, 0x0401, 0x0402, 0x0403, 0x0404, 0x0405, 0x0406,
    0x0407, 0x0408, 0x0501, 0x0502, 0x0701, 0x0702, 0x0703, 0x0704, 0x0705, 0x0706, 0x0707, 0x0708, 0x0901, 0x0902,
    0x0201, 0x0202, 0x0203, 0x0204, 0x0B01, 0x0B02 };
static const unsigned short kTwxOut[46] = {
    0x0901, 0x0902, 0x0301, 0x0302, 0x0303, 0x0304, 0x0305, 0x0306, 0x0307, 0x0308, 0x0401, 0x0402, 0x0403, 0x0404,
    0x0405, 0x0406, 0x0407, 0x0408, 0x0501, 0x0502, 0x0701, 0x0702, 0x0703, 0x0704, 0x0705, 0x0706, 0x0707, 0x0708,
    0x0A01, 0x0A02, 0x0A03, 0x0A04, 0x0A05, 0x0A06, 0x0A07, 0x0A08, 0x0C01, 0x0C02, 0x0C03, 0x0C04, 0x0C05, 0x0C06,
    0x0C07, 0x0C08, 0x0C09, 0x0C0A };

static APOLLO_AGG_CONFIG g_cfg;

/* x8p (unit 0) + Twin X (unit 1, master): the default configuration's shape, descriptors cached. */
static void BaseConfig(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.Version = APOLLO_AGG_VERSION;
    g_cfg.Bytes = sizeof(g_cfg);
    g_cfg.Flags = APOLLO_AGG_CFG_ENABLED | APOLLO_AGG_CFG_ALIGN;
    g_cfg.UnitCount = 2;
    g_cfg.Master = 1;
    g_cfg.AsrcHalf = 16;
    g_cfg.Unit[0].Uid = 0x3A00C0DE;
    g_cfg.Unit[0].Model = APOLLO_INFO_MODEL_X8P;
    g_cfg.Unit[0].InCount = 32;
    g_cfg.Unit[0].OutCount = 34;
    memcpy(g_cfg.Unit[0].InIds, kX8pIn, sizeof(kX8pIn));
    memcpy(g_cfg.Unit[0].OutIds, kX8pOut, sizeof(kX8pOut));
    g_cfg.Unit[1].Uid = 0x2F00BEEF;
    g_cfg.Unit[1].Model = APOLLO_INFO_MODEL_TWINX;
    g_cfg.Unit[1].InCount = 34;
    g_cfg.Unit[1].OutCount = 46;
    memcpy(g_cfg.Unit[1].InIds, kTwxIn, sizeof(kTwxIn));
    memcpy(g_cfg.Unit[1].OutIds, kTwxOut, sizeof(kTwxOut));
}

static void AddRoute(unsigned char Kind, unsigned char Dst, unsigned short Id0, unsigned short Id1)
{
    APOLLO_AGG_ROUTE_CFG *r = &g_cfg.Route[g_cfg.RouteCount++];

    memset(r, 0, sizeof(*r));
    r->SrcKind = Kind;
    r->DstUnit = Dst;
    r->DstId[0] = Id0;
    r->DstId[1] = Id1;
}

/* ---------------------------------------------------------------- 1 */
static void TestTableChoice(void)
{
    static const unsigned long sets[] = { 0, 2, 3, 0x100, 0xFFFFFFFFul };
    static const int models[] = { APOLLO_MODEL_X8P, APOLLO_MODEL_TWINX, APOLLO_MODEL_NONE };
    unsigned long i, k;

    for (i = 0; i < sizeof(sets) / sizeof(sets[0]); i++) {
        for (k = 0; k < 3; k++) {
            Check(ApolloAggEpTableFor(sets[i], models[k]) == APOLLO_AGG_EPTABLE_STANDALONE,
                  "EndpointSet absent/0/unknown: every unit keeps its standalone table (as before G7)");
        }
    }
    Check(ApolloAggEpTableFor(APOLLO_AGG_EPSET_COMBINED, APOLLO_MODEL_X8P) == APOLLO_AGG_EPTABLE_COMBINED,
          "EndpointSet 1: the x8p hosts the combined table");
    Check(ApolloAggEpTableFor(APOLLO_AGG_EPSET_COMBINED, APOLLO_MODEL_TWINX) == APOLLO_AGG_EPTABLE_NONE,
          "EndpointSet 1: the Twin X creates no circuits (no duplicates)");
    Check(ApolloAggEpTableFor(APOLLO_AGG_EPSET_COMBINED, APOLLO_MODEL_NONE) == APOLLO_AGG_EPTABLE_NONE,
          "EndpointSet 1: any other device none");
    /* the add-time model the driver passes (apollo_unit.h, unchanged) */
    Check(ApolloAddTimeModel(1, APOLLO_SUBSYS_X8P) == APOLLO_MODEL_X8P && ApolloAddTimeModel(1, APOLLO_SUBSYS_TWINX) ==
          APOLLO_MODEL_TWINX && ApolloAddTimeModel(0, 0) == APOLLO_MODEL_X8P, "add-time models by hardware ID");
    /* [Endpoints] the standalone tables (bringup_plan.h numbers = endpoint_plan.h lists; endpoints.c asserts the same) */
    Check(APOLLO_X8P_EP_ROWS == 22 && APOLLO_X8P_EP_DEFAULT == 0x1FFFF && APOLLO_TWX_EP_ROWS == 9 &&
          APOLLO_TWX_EP_DEFAULT == 0x3F && APOLLO_X8P_EP_LIST_COUNT == APOLLO_X8P_EP_ROWS &&
          APOLLO_TWX_EP_LIST_COUNT == APOLLO_TWX_EP_ROWS && APOLLO_X8P_EP_LIST_DEFAULT == APOLLO_X8P_EP_DEFAULT &&
          APOLLO_TWX_EP_LIST_DEFAULT == APOLLO_TWX_EP_DEFAULT,
          "standalone tables: x8p 22 rows 0x1FFFF, Twin X 9 rows 0x3F (lists and numbers agree)");
    Check(ApolloAddTimeEndpoints(1, APOLLO_SUBSYS_X8P, 0, 0) == APOLLO_X8P_EP_ALL &&
          ApolloAddTimeEndpoints(1, APOLLO_SUBSYS_X8P, 1, 3) == APOLLO_X8P_EP_ALL &&
          ApolloAddTimeEndpoints(1, APOLLO_SUBSYS_TWINX, 1, 3) == APOLLO_TWX_EP_ALL,
          "standalone device add: every row's circuit object on both models");
    Check(ApolloEpAllMask(0) == 0 && ApolloEpAllMask(9) == 0x1FF && ApolloEpAllMask(31) == 0x7FFFFFFF &&
          ApolloEpAllMask(32) == 0xFFFFFFFF && ApolloEpAllMask(40) == 0xFFFFFFFF, "row masks up to 32 rows without a 32-bit shift");
    /* the two new per-unit values default to 0 on every model (G6 alignment) */
    Check(ApolloParamDefault(APOLLO_MODEL_X8P, APOLLO_PARAM_CAPTURE_TRIM) == 0 &&
          ApolloParamDefault(APOLLO_MODEL_TWINX, APOLLO_PARAM_CAPTURE_TRIM) == 0 &&
          ApolloParamDefault(APOLLO_MODEL_X8P, APOLLO_PARAM_RENDER_TRIM) == 0 &&
          ApolloParamDefault(APOLLO_MODEL_TWINX, APOLLO_PARAM_RENDER_TRIM) == 0, "trim defaults 0");
    Check(wcscmp(ApolloParamDef(APOLLO_PARAM_CAPTURE_TRIM)->NameW, L"CaptureTrimFrames") == 0 &&
          wcscmp(ApolloParamDef(APOLLO_PARAM_RENDER_TRIM)->NameW, L"RenderTrimFrames") == 0, "trim value names");
}

/* ---------------------------------------------------------------- 2 */
static int Widened(const char *A, const wchar_t *W)
{
    size_t i;

    for (i = 0; A[i] != 0; i++) {
        if ((wchar_t)(unsigned char)A[i] != W[i]) {
            return 0;
        }
    }
    return W[i] == 0;
}

/* The standalone row a combined OUTPUT/INPUT row carries (its unit's model, the same jack), or NULL. */
static const APOLLO_EP_PLAN_ROW *StandaloneOf(const APOLLO_AGG_EP_ROW *R)
{
    int model = (R->Model == APOLLO_INFO_MODEL_X8P) ? 1 : (R->Model == APOLLO_INFO_MODEL_TWINX ? 2 : 0);
    const char *prefix = (model == 1) ? "x8p " : "Twin X ";
    unsigned long i;
    const APOLLO_EP_PLAN_ROW *p;

    if (model == 0 || strncmp(R->Pin, prefix, strlen(prefix)) != 0) {
        return NULL;
    }
    for (i = 0; (p = ApolloEpPlanRow(model, i)) != NULL; i++) {
        if (p->Render == R->Render && !p->NeverDefault && strcmp(p->Pin, R->Pin + strlen(prefix)) == 0) {
            return p;
        }
    }
    return NULL;
}

static void TestStandaloneRows(int Model, unsigned long Rows, unsigned long Default)
{
    static const char *old[] = { "ApolloMon", "ApolloMonIn", "ApolloTwxMon", "ApolloTwxMonIn" };
    const APOLLO_EP_PLAN_ROW *r;
    unsigned long i, k, def = 0, outs = 0, mics = 0, never = 0;
    char what[200];

    for (i = 0; (r = ApolloEpPlanRow(Model, i)) != NULL; i++) {
        sprintf_s(what, sizeof(what), "model %d row %lu %s \"%s\": names fit, new reference string", Model, i, r->Ref, r->Pin);
        Check(strlen(r->Ref) < APOLLO_ENDPOINT_NAME_CHARS && strlen(r->Pin) < APOLLO_ENDPOINT_NAME_CHARS &&
              strncmp(r->Ref, Model == 1 ? "Ap2X8p" : "Ap2Twx", 6) == 0, what);
        for (k = 0; k < sizeof(old) / sizeof(old[0]); k++) {
            Check(_stricmp(old[k], r->Ref) != 0, "no reference string of an earlier build");
        }
        for (k = 0; k < i; k++) {
            const APOLLO_EP_PLAN_ROW *q = ApolloEpPlanRow(Model, k);

            Check(strcmp(q->Ref, r->Ref) != 0, "reference strings unique");
            Check(strcmp(q->Pin, r->Pin) != 0 || q->Render != r->Render, "pin names unique per direction");
            Check(q->Render != r->Render || q->Id[0] != r->Id[0], "one row per channel ID per direction");
        }
        Check(r->Channels == 2 || (r->Id[0] == r->Id[1] && r->Fallback[0] == r->Fallback[1]), "mono repeats channel 0");
        Check(r->Bridge != APOLLO_EP_BRIDGE_MICROPHONE || (!r->Render && r->Channels == 1),
              "Microphone form factor only on the mono mic/line inputs");
        Check(r->Render || r->Bridge != APOLLO_EP_BRIDGE_HEADPHONES, "Headphones only on outputs");
        if (r->Render) {
            outs++;
            Check(r->Channels == 2 && r->Default && !r->NeverDefault, "every output: a stereo pair, in the default set");
            Check(r->Bridge == ((r->Pin[0] == 'H' && r->Pin[1] == 'P') ? APOLLO_EP_BRIDGE_HEADPHONES : APOLLO_EP_BRIDGE_LINE_CONNECTOR),
                  "outputs: HP jacks Headphones, everything else a line connector (never \"Speakers\")");
        }
        if (r->Bridge == APOLLO_EP_BRIDGE_MICROPHONE) {
            mics++;
            Check(r->Default && !r->NeverDefault, "mic/line inputs: in the default set, choosable as default");
        }
        if (r->NeverDefault) {
            never++;
            Check(!r->Render && r->Default && r->Id[0] == APOLLO_CHAN_MON_L && r->Id[1] == APOLLO_CHAN_MON_R &&
                  r->Fallback[0] == 26 && r->Fallback[1] == 27 && strcmp(r->Pin, "Monitor mix") == 0,
                  "the one NeverSetAsDefault row: the monitor-mix loopback, record 26/27");
        }
        if (!r->Render && r->Bridge == APOLLO_EP_BRIDGE_LINE_CONNECTOR && !r->NeverDefault) {
            Check(!r->Default && ((r->Id[0] >> 8) == 0x07 || (r->Id[0] >> 8) == 0x05),
                  "the other inputs are VIRTUAL / S/PDIF pairs, opt-in");
        }
        def |= r->Default ? (1ul << i) : 0;
    }
    sprintf_s(what, sizeof(what), "model %d: %lu rows (want %lu), default 0x%lX (want 0x%lX)", Model, i, Rows, def, Default);
    Check(i == Rows && def == Default, what);
    Check(never == 1, "exactly one loopback row");
    if (Model == 1) {
        static const unsigned short ids[8][2] = { { 0x0901, 0x0902 }, { 0x0301, 0x0302 }, { 0x0303, 0x0304 },
                                                  { 0x0305, 0x0306 }, { 0x0307, 0x0308 }, { 0x0A01, 0x0A02 },
                                                  { 0x0A03, 0x0A04 }, { 0x0501, 0x0502 } };
        static const unsigned short in[8] = { 0x0001, 0x0002, 0x0103, 0x0104, 0x0305, 0x0306, 0x0307, 0x0308 };
        static const char *pins[8] = { "MON L/R", "LINE 1-2", "LINE 3-4", "LINE 5-6", "LINE 7-8", "HP 1", "HP 2", "S/PDIF" };

        Check(outs == 8 && mics == 8, "x8p: 8 output pairs (MON, LINE 1-8, HP 1-2, S/PDIF), 8 mic/line inputs");
        for (k = 0; k < 8; k++) {
            r = ApolloEpPlanRow(1, k);
            sprintf_s(what, sizeof(what), "x8p row %lu: %s on IDs %04X/%04X", k, pins[k], ids[k][0], ids[k][1]);
            Check(r->Render && strcmp(r->Pin, pins[k]) == 0 && r->Id[0] == ids[k][0] && r->Id[1] == ids[k][1] &&
                  r->Fallback[0] == ApolloAggOutSlot(&g_cfg, 0, ids[k][0]) &&
                  r->Fallback[1] == ApolloAggOutSlot(&g_cfg, 0, ids[k][1]), what);
            r = ApolloEpPlanRow(1, ApolloX8pEpIdx_X8pIn1 + k);
            Check(!r->Render && r->Id[0] == in[k] && r->Fallback[0] == k, "x8p MIC/LINE n: the hardware's IDs, slot n-1");
        }
        /* the fallbacks are where the x8p's hardware descriptor has the IDs, within its 34 play slots */
    } else {
        Check(outs == 3 && mics == 2, "Twin X: MON L/R, LINE 3-4, HP out; MIC/LINE 1-2 in");
        r = ApolloEpPlanRow(2, ApolloTwxEpIdx_TwxLine34);
        Check(r->Id[0] == 0x0A03 && r->Id[1] == 0x0A04 && r->Fallback[0] == 30 && r->Fallback[1] == 31,
              "Twin X LINE 3-4 = CUE 2 (UA's route table), slots 30/31 of its descriptor");
        r = ApolloEpPlanRow(2, ApolloTwxEpIdx_TwxHp);
        Check(r->Id[0] == 0x0A01 && r->Id[1] == 0x0A02 && r->Fallback[0] == 28 && r->Fallback[1] == 29 &&
              r->Bridge == APOLLO_EP_BRIDGE_HEADPHONES, "Twin X HP = CUE 1, slots 28/29, Headphones");
        for (i = 0; (r = ApolloEpPlanRow(2, i)) != NULL; i++) {
            if (r->Render) {
                Check(r->Id[0] != 0x0303 && r->Id[0] != 0x0301 && r->Id[0] != 0x0501,
                      "Twin X: no LINE 1-8 / S/PDIF output rows (the superset descriptor lists them; no jack)");
            }
        }
    }
}

static void TestRows(void)
{
    static const unsigned short x8pIds[8] = { 0x0001, 0x0002, 0x0103, 0x0104, 0x0305, 0x0306, 0x0307, 0x0308 };
    static const char *oldRefs[] = { "ApolloMon", "ApolloMonIn", "ApolloIn1", "ApolloIn2", "ApolloIn3", "ApolloIn4",
                                     "ApolloIn5", "ApolloIn6", "ApolloIn7", "ApolloIn8", "ApolloTwxMon",
                                     "ApolloTwxMonIn", "ApolloTwxIn1", "ApolloTwxIn2", "ApolloAggMon", "ApolloAggMonIn",
                                     "ApolloAggX8pIn1", "ApolloAggTwxIn1", "ApolloAggTwxIn2" };
    unsigned long i, k, renders = 0, never = 0, outputs = 0, def = 0, x8pOut = 0, twxOut = 0;

    BaseConfig();                   /* the hardware descriptors, for the fallback checks */
    TestStandaloneRows(1, APOLLO_X8P_EP_ROWS, APOLLO_X8P_EP_DEFAULT);
    TestStandaloneRows(2, APOLLO_TWX_EP_ROWS, APOLLO_TWX_EP_DEFAULT);

    Check(APOLLO_AGG_EP_LIST_COUNT == APOLLO_AGG_EP_ROWS && APOLLO_AGG_EP_ROWS == 31, "31 combined rows");
    Check(ApolloAggEpIdxEnd == APOLLO_AGG_EP_ROWS && ApolloAggEpIdx_AggMon == 0 && ApolloAggEpIdx_AggMonIn == 12 &&
          ApolloAggEpIdx_AggX8pIn1 == 13 && ApolloAggEpIdx_AggTwxIn2 == 22, "row indexes");
    Check(ApolloAggEpRow(APOLLO_AGG_EP_ROWS) == NULL, "no row past the end");
    Check(APOLLO_AGG_EP_ROWS <= APOLLO_ENDPOINTS_ROWS_MAX && APOLLO_ENDPOINTS_ROWS_MAX == 32,
          "fits GET_ENDPOINTS v2 and one DWORD mask (32 rows)");
    Check(APOLLO_AGG_EP_DEFAULT == 0x7FFFFF && APOLLO_AGG_EP_LIST_DEFAULT == APOLLO_AGG_EP_DEFAULT,
          "default mask: Monitor, every output, the loopback, every mic/line input (rows 0-22)");
    for (i = 0; i < APOLLO_AGG_EP_ROWS; i++) {
        const APOLLO_AGG_EP_ROW *r = ApolloAggEpRow(i);
        char what[200];

        if (r == NULL) {
            Check(0, "row missing");
            continue;
        }
        sprintf_s(what, sizeof(what), "row %lu %s: names fit, wide copies equal", i, r->Ref);
        Check(strlen(r->Ref) < APOLLO_ENDPOINT_NAME_CHARS && strlen(r->Pin) < APOLLO_ENDPOINT_NAME_CHARS &&
              strncmp(r->Ref, "Ap2C", 4) == 0 && Widened(r->Ref, r->RefW) && Widened(r->Pin, r->PinW), what);
        for (k = 0; k < i; k++) {
            /* [Endpoints] a pin name may repeat across directions ("x8p S/PDIF" out and in: Windows lists them apart) */
            Check(strcmp(ApolloAggEpRow(k)->Ref, r->Ref) != 0 &&
                  (strcmp(ApolloAggEpRow(k)->Pin, r->Pin) != 0 || ApolloAggEpRow(k)->Render != r->Render),
                  "reference strings unique, pin names unique per direction");
        }
        for (k = 0; k < sizeof(oldRefs) / sizeof(oldRefs[0]); k++) {
            Check(_stricmp(oldRefs[k], r->Ref) != 0, "no reference string of an earlier build");
        }
        for (k = 0; ApolloEpPlanRow(1, k) != NULL || ApolloEpPlanRow(2, k) != NULL; k++) {
            Check((ApolloEpPlanRow(1, k) == NULL || _stricmp(ApolloEpPlanRow(1, k)->Ref, r->Ref) != 0) &&
                  (ApolloEpPlanRow(2, k) == NULL || _stricmp(ApolloEpPlanRow(2, k)->Ref, r->Ref) != 0),
                  "no combined reference string equals a standalone one");
        }
        renders += r->Render;
        never += r->NeverDefault;
        def |= r->Default ? (1ul << i) : 0;
        Check(r->Channels == 2 || (r->Id[0] == r->Id[1] && r->Fallback[0] == r->Fallback[1]), "mono repeats channel 0");
        Check(r->Render == (r->Kind == APOLLO_AGG_EP_MONITOR || r->Kind == APOLLO_AGG_EP_OUTPUT), "kinds and directions agree");
        if (r->Kind == APOLLO_AGG_EP_INPUT || r->Kind == APOLLO_AGG_EP_OUTPUT) {
            const APOLLO_EP_PLAN_ROW *p = StandaloneOf(r);

            sprintf_s(what, sizeof(what), "row %lu \"%s\": the standalone row of that unit, same IDs, slots, form factor, set",
                      i, r->Pin);
            Check(p != NULL && p->Id[0] == r->Id[0] && p->Id[1] == r->Id[1] && p->Fallback[0] == r->Fallback[0] &&
                  p->Fallback[1] == r->Fallback[1] && p->Channels == r->Channels && p->Default == r->Default &&
                  p->Bridge == (r->Bridge == APOLLO_AGG_BRIDGE_MICROPHONE ? APOLLO_EP_BRIDGE_MICROPHONE :
                                (r->Bridge == APOLLO_AGG_BRIDGE_HEADPHONES ? APOLLO_EP_BRIDGE_HEADPHONES :
                                 APOLLO_EP_BRIDGE_LINE_CONNECTOR)) && !r->NeverDefault, what);
            Check(r->Model == APOLLO_INFO_MODEL_X8P || r->Model == APOLLO_INFO_MODEL_TWINX, "unit rows name a model");
        } else {
            Check(r->Bridge == APOLLO_AGG_BRIDGE_LINE_CONNECTOR && r->Model == APOLLO_INFO_MODEL_NONE && r->Default,
                  "Monitor rows: line connector (never renamed \"Speakers\"), no unit, in the default set");
            Check(r->Id[0] == APOLLO_CHAN_MON_L && r->Id[1] == APOLLO_CHAN_MON_R, "Monitor rows: MON L/R IDs");
        }
        if (r->Kind == APOLLO_AGG_EP_OUTPUT) {
            outputs++;
            x8pOut += r->Model == APOLLO_INFO_MODEL_X8P;
            twxOut += r->Model == APOLLO_INFO_MODEL_TWINX;
        }
    }
    Check(def == APOLLO_AGG_EP_DEFAULT, "the list's default column is APOLLO_AGG_EP_DEFAULT");
    Check(renders == 12 && outputs == 11 && x8pOut == 8 && twxOut == 3,
          "render rows: Monitor + 8 x8p jacks + 3 Twin X jacks");
    for (i = 0; i < APOLLO_X8P_EP_ROWS; i++) {
        const APOLLO_EP_PLAN_ROW *p = ApolloEpPlanRow(1, i);
        int found = p->NeverDefault;

        for (k = 0; k < APOLLO_AGG_EP_ROWS && !found; k++) {
            found = StandaloneOf(ApolloAggEpRow(k)) == p;
        }
        Check(found, "every x8p standalone jack (but its loopback) has its combined row");
    }
    for (i = 0; i < APOLLO_TWX_EP_ROWS; i++) {
        const APOLLO_EP_PLAN_ROW *p = ApolloEpPlanRow(2, i);
        int found = p->NeverDefault;

        for (k = 0; k < APOLLO_AGG_EP_ROWS && !found; k++) {
            found = StandaloneOf(ApolloAggEpRow(k)) == p;
        }
        Check(found, "every Twin X standalone jack (but its loopback) has its combined row");
    }
    Check(ApolloAggEpRow(0)->Render && ApolloAggEpRow(0)->Kind == APOLLO_AGG_EP_MONITOR &&
          strcmp(ApolloAggEpRow(0)->Pin, "Monitor") == 0, "row 0 = \"Monitor\" (the windows routes)");
    Check(never == 1 && ApolloAggEpRow(12)->NeverDefault && ApolloAggEpRow(12)->Kind == APOLLO_AGG_EP_LOOPBACK &&
          ApolloAggEpRow(12)->Fallback[0] == 26 && ApolloAggEpRow(12)->Fallback[1] == 27 &&
          strcmp(ApolloAggEpRow(12)->Pin, "Monitor mix") == 0,
          "row 12 = the monitor-mix loopback, NeverSetAsDefault, record 26/27");
    for (i = 0; i < 8; i++) {
        const APOLLO_AGG_EP_ROW *r = ApolloAggEpRow(ApolloAggEpIdx_AggX8pIn1 + i);
        char pin[32];

        sprintf_s(pin, sizeof(pin), "x8p MIC/LINE %lu", i + 1);
        Check(r->Model == APOLLO_INFO_MODEL_X8P && r->Id[0] == x8pIds[i] && r->Fallback[0] == i && strcmp(r->Pin, pin) == 0 &&
              r->Bridge == APOLLO_AGG_BRIDGE_MICROPHONE && r->Default,
              "x8p inputs: the standalone rows' IDs and slots, pin names carry the unit, in the default set");
    }
    for (i = 0; i < 2; i++) {
        const APOLLO_AGG_EP_ROW *r = ApolloAggEpRow(ApolloAggEpIdx_AggTwxIn1 + i);

        Check(r->Model == APOLLO_INFO_MODEL_TWINX && r->Id[0] == (i ? APOLLO_CHAN_IN2 : APOLLO_CHAN_IN1) &&
              r->Fallback[0] == (i ? APOLLO_TWX_SLOT_REC_IN2 : APOLLO_TWX_SLOT_REC_IN1) && r->Default,
              "Twin X inputs: IDs and slots, in the default set now (G5 passed)");
    }
}

/* ---------------------------------------------------------------- 3 */
#define INF_MAX_LINES   2000
static char *g_inf[INF_MAX_LINES];
static int g_infLines;

static char *Trim(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t') {
        s++;
    }
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) {
        *--e = 0;
    }
    return s;
}

static int LoadInf(const char *Path)
{
    FILE *f = NULL;
    char buf[1024];

    if (fopen_s(&f, Path, "r") != 0 || f == NULL) {
        return 0;
    }
    while (g_infLines < INF_MAX_LINES && fgets(buf, sizeof(buf), f) != NULL) {
        char *c = strchr(buf, ';'), *t;

        if (c != NULL && strchr(buf, '"') == NULL) {
            *c = 0;                 /* a comment (no string on the line: [Strings] values have no ';') */
        }
        t = Trim(buf);
        g_inf[g_infLines++] = _strdup(t);
    }
    fclose(f);
    return g_infLines > 0;
}

/* Lines of section Name (without the header), into Out; count returned. */
static int Section(const char *Name, char **Out, int Max)
{
    int i, n = 0, in = 0;
    char hdr[128];

    sprintf_s(hdr, sizeof(hdr), "[%s]", Name);
    for (i = 0; i < g_infLines; i++) {
        if (g_inf[i][0] == '[') {
            in = (_stricmp(g_inf[i], hdr) == 0);
            continue;
        }
        if (in && g_inf[i][0] != 0 && n < Max) {
            Out[n++] = g_inf[i];
        }
    }
    return n;
}

/* [Strings] key whose value is "Value" (one expected), or NULL. */
static const char *StringKeyOf(const char *Value, int *Count)
{
    static char key[128];
    char *lines[400], want[160];
    int n = Section("Strings", lines, 400), i;
    const char *found = NULL;

    *Count = 0;
    sprintf_s(want, sizeof(want), "\"%s\"", Value);
    for (i = 0; i < n; i++) {
        char *eq = strchr(lines[i], '=');

        if (eq != NULL && strcmp(Trim(eq + 1), want) == 0) {
            size_t len = (size_t)(eq - lines[i]);

            if (len >= sizeof(key)) {
                len = sizeof(key) - 1;
            }
            memcpy(key, lines[i], len);
            key[len] = 0;
            found = Trim(key);
            (*Count)++;
        }
    }
    return found;
}

static const char *StringValueOf(const char *Key)
{
    static char val[160];
    char *lines[400];
    int n = Section("Strings", lines, 400), i;

    for (i = 0; i < n; i++) {
        char *eq = strchr(lines[i], '=');
        char k[128];
        size_t len;

        if (eq == NULL) {
            continue;
        }
        len = (size_t)(eq - lines[i]);
        if (len >= sizeof(k)) {
            continue;
        }
        memcpy(k, lines[i], len);
        k[len] = 0;
        if (_stricmp(Trim(k), Key) == 0) {
            strcpy_s(val, sizeof(val), Trim(eq + 1));
            return val;
        }
    }
    return "";
}

/*
 * The AddInterface lines of section Sect for reference-string key Key: categories found (bit 0
 * AUDIO, 1 RENDER, 2 CAPTURE, 3 REALTIME), the interface section (all lines must agree), count.
 */
static int Interfaces(const char *Sect, const char *Key, unsigned *Cats, char *IfSect, size_t IfLen)
{
    char *lines[300], ref[160];
    int n = Section(Sect, lines, 300), i, count = 0;

    *Cats = 0;
    IfSect[0] = 0;
    sprintf_s(ref, sizeof(ref), "%%%s%%", Key);
    for (i = 0; i < n; i++) {
        char tmp[512], *ctx = NULL, *f[4];
        int k = 0;

        if (_strnicmp(lines[i], "AddInterface", 12) != 0) {
            continue;
        }
        strcpy_s(tmp, sizeof(tmp), strchr(lines[i], '=') ? strchr(lines[i], '=') + 1 : "");
        for (f[k] = strtok_s(tmp, ",", &ctx); f[k] != NULL && k < 3; f[++k] = strtok_s(NULL, ",", &ctx)) {
            f[k] = Trim(f[k]);
        }
        if (k < 3 || strcmp(f[1], ref) != 0) {
            continue;
        }
        count++;
        if (strcmp(f[0], "%KSCATEGORY_AUDIO%") == 0) *Cats |= 1;
        else if (strcmp(f[0], "%KSCATEGORY_RENDER%") == 0) *Cats |= 2;
        else if (strcmp(f[0], "%KSCATEGORY_CAPTURE%") == 0) *Cats |= 4;
        else if (strcmp(f[0], "%KSCATEGORY_REALTIME%") == 0) *Cats |= 8;
        if (IfSect[0] == 0) {
            strcpy_s(IfSect, IfLen, f[2]);
        } else if (_stricmp(IfSect, f[2]) != 0) {
            strcpy_s(IfSect, IfLen, "(differs)");
        }
    }
    return count;
}

static int AddRegHas(const char *IfSect, const char *Name)
{
    char *lines[20];
    int n = Section(IfSect, lines, 20), i;

    for (i = 0; i < n; i++) {
        if (_strnicmp(lines[i], "AddReg", 6) == 0 && strstr(lines[i], Name) != NULL) {
            const char *p = strstr(lines[i], Name);
            char after = p[strlen(Name)];

            if (after == 0 || after == ',' || after == ' ') {
                return 1;
            }
        }
    }
    return 0;
}

static int SectionHasLine(const char *Sect, const char *Line)
{
    char *lines[40];
    int n = Section(Sect, lines, 40), i;

    for (i = 0; i < n; i++) {
        if (strcmp(lines[i], Line) == 0) {
            return 1;
        }
    }
    return 0;
}

/* One row's three interfaces on Sect, the AddReg it uses, NeverSetAsDefault as asked, absent from Other. */
static void InfRow(const char *Ref, int Render, int Never, const char *Sect, const char *Other, const char *AddReg)
{
    unsigned cats = 0;
    char ifs[128], what[240];
    int keys = 0, n;
    const char *key = StringKeyOf(Ref, &keys);

    sprintf_s(what, sizeof(what), "INF: %s has exactly one [Strings] key", Ref);
    Check(key != NULL && keys == 1, what);
    if (key == NULL) {
        return;
    }
    n = Interfaces(Sect, key, &cats, ifs, sizeof(ifs));
    sprintf_s(what, sizeof(what), "INF: %s: AUDIO + %s + REALTIME on %s (got %d lines, cats 0x%x)", Ref,
              Render ? "RENDER" : "CAPTURE", Sect, n, cats);
    Check(n == 3 && cats == (Render ? 0xBu : 0xDu), what);
    sprintf_s(what, sizeof(what), "INF: %s: interface section %s uses %s", Ref, ifs, AddReg);
    Check(AddRegHas(ifs, AddReg), what);
    sprintf_s(what, sizeof(what), "INF: %s: NeverSetAsDefault %s", Ref, Never ? "yes" : "no");
    Check(AddRegHas(ifs, "Apollo_NeverDefault_AddReg") == (Never ? 1 : 0), what);
    sprintf_s(what, sizeof(what), "INF: %s: not on %s", Ref, Other);
    Check(Interfaces(Other, key, &cats, ifs, sizeof(ifs)) == 0, what);
}

static void TestInf(const char *Path)
{
    static const char *oldRefs[] = { "ApolloMon", "ApolloMonIn", "ApolloIn1", "ApolloIn8", "ApolloTwxMon",
                                     "ApolloTwxMonIn", "ApolloTwxIn1", "ApolloTwxIn2", "ApolloAggMon", "ApolloAggMonIn",
                                     "ApolloAggX8pIn1", "ApolloAggTwxIn2" };
    const APOLLO_EP_PLAN_ROW *p;
    unsigned long i;
    int keys, total = 0, k;

    if (!LoadInf(Path)) {
        Check(0, "apollo.inf readable");
        return;
    }
    for (i = 0; i < APOLLO_AGG_EP_ROWS; i++) {
        const APOLLO_AGG_EP_ROW *r = ApolloAggEpRow(i);

        InfRow(r->Ref, r->Render, r->NeverDefault, "Apollo_Install.NT.Interfaces", "Apollo_TwinX_Install.NT.Interfaces",
               "Apollo_Agg_Endpoint_AddReg");
        total++;
    }
    for (i = 0; (p = ApolloEpPlanRow(1, i)) != NULL; i++) {
        InfRow(p->Ref, p->Render, p->NeverDefault, "Apollo_Install.NT.Interfaces", "Apollo_TwinX_Install.NT.Interfaces",
               "Apollo_Endpoint_AddReg");
        total++;
    }
    for (i = 0; (p = ApolloEpPlanRow(2, i)) != NULL; i++) {
        InfRow(p->Ref, p->Render, p->NeverDefault, "Apollo_TwinX_Install.NT.Interfaces", "Apollo_Install.NT.Interfaces",
               "Apollo_TwinX_Endpoint_AddReg");
        total++;
    }
    {
        char *lines[400];
        int n = Section("Apollo_Install.NT.Interfaces", lines, 400), m = 0;

        for (k = 0; k < n; k++) {
            m += _strnicmp(lines[k], "AddInterface", 12) == 0;
        }
        n = Section("Apollo_TwinX_Install.NT.Interfaces", lines, 400);
        for (k = 0; k < n; k++) {
            m += _strnicmp(lines[k], "AddInterface", 12) == 0;
        }
        Check(m == 3 * total, "INF: no AddInterface line beyond the rows of the three tables");
    }
    for (i = 0; i < sizeof(oldRefs) / sizeof(oldRefs[0]); i++) {
        Check(StringKeyOf(oldRefs[i], &keys) == NULL, "INF: no reference string of an earlier build left");
    }
    Check(SectionHasLine("Apollo_Agg_Endpoint_AddReg", "HKR,,FriendlyName,,%ApolloAgg.EndpointName%") &&
          strcmp(StringValueOf("ApolloAgg.EndpointName"), "\"Apollo\"") == 0, "INF: combined FriendlyName \"Apollo\"");
    for (k = 0; k < 3; k++) {
        static const char *sects[3] = { "Apollo_Agg_Endpoint_AddReg", "Apollo_Endpoint_AddReg", "Apollo_TwinX_Endpoint_AddReg" };

        Check(SectionHasLine(sects[k], "HKR,,CLSID,,%Proxy.CLSID%") &&
              SectionHasLine(sects[k], "HKR,EP\\0,%PKEY_AudioEndpoint_Association%,,%KSNODETYPE_ANY%") &&
              SectionHasLine(sects[k], "HKR,EP\\0,%PKEY_AudioEndpoint_Supports_EventDriven_Mode%,0x00010001,0x1"),
              "INF: every endpoint AddReg has the proxy CLSID, association and event-driven mode");
    }
    Check(SectionHasLine("Apollo_NeverDefault_AddReg", "HKR,EP\\0,%PKEY_AudioDevice_NeverSetAsDefaultEndpoint%,0x00010001,0x305"),
          "INF: NeverSetAsDefault covers capture, render, console and communications");
    Check(strcmp(StringValueOf("Apollo.EndpointName"), "\"Apollo x8p\"") == 0 &&
          strcmp(StringValueOf("ApolloTwinX.EndpointName"), "\"Apollo Twin X\"") == 0, "INF: standalone FriendlyNames");
    Check(strcmp(StringValueOf("Apollo.DeviceDesc"), "\"Apollo x8p\"") == 0 &&
          strcmp(StringValueOf("ApolloTwinX.DeviceDesc"), "\"Apollo Twin X\"") == 0,
          "INF: [Endpoints] device names without \"(open driver, test build)\"");
}

/* ---------------------------------------------------------------- 4 */
static int Has(const APOLLO_AGG_SREF *S, unsigned long Ch, unsigned long U, unsigned long Slot)
{
    unsigned long d;

    for (d = 0; d < S->Count[Ch]; d++) {
        if (S->Unit[Ch][d] == U && S->Slot[Ch][d] == Slot) {
            return 1;
        }
    }
    return 0;
}

static void TestResolve(void)
{
    APOLLO_AGG_SREF s;
    unsigned long i;

    /* default (no windows route): the master's MON = the Twin X speakers */
    BaseConfig();
    ApolloAggEpResolve(&g_cfg, ApolloAggEpIdx_AggMon, &s);
    Check(s.Count[0] == 1 && s.Count[1] == 1 && Has(&s, 0, 1, 0) && Has(&s, 1, 1, 1), "no route: Monitor -> Twin X MON 0/1");
    ApolloAggEpResolve(&g_cfg, ApolloAggEpIdx_AggMonIn, &s);
    Check(s.Count[0] == 1 && Has(&s, 0, 1, 26) && Has(&s, 1, 1, 27), "no route: Monitor mix <- Twin X record 26/27");
    for (i = 0; i < 8; i++) {
        ApolloAggEpResolve(&g_cfg, ApolloAggEpIdx_AggX8pIn1 + i, &s);
        Check(s.Count[0] == 1 && Has(&s, 0, 0, i) && s.Count[1] == 0, "x8p MIC/LINE n <- x8p record slot n-1 (mono)");
    }
    ApolloAggEpResolve(&g_cfg, ApolloAggEpIdx_AggTwxIn1, &s);
    Check(s.Count[0] == 1 && Has(&s, 0, 1, 0), "TwinX MIC/LINE 1 <- Twin X slot 0");
    ApolloAggEpResolve(&g_cfg, ApolloAggEpIdx_AggTwxIn2, &s);
    Check(s.Count[0] == 1 && Has(&s, 0, 1, 1), "TwinX MIC/LINE 2 <- Twin X slot 1");
    /* [Endpoints] the output jacks: on their own unit's playback slots by ID, whatever the routes say */
    {
        static const struct { int Row; unsigned long U, S0, S1; const char *What; } o[] = {
            { ApolloAggEpIdx_AggX8pMon,    0,  0,  1, "x8p MON L/R -> x8p play 0/1" },
            { ApolloAggEpIdx_AggX8pLine12, 0,  2,  3, "x8p LINE 1-2 -> x8p play 2/3" },
            { ApolloAggEpIdx_AggX8pLine34, 0,  4,  5, "x8p LINE 3-4 -> x8p play 4/5" },
            { ApolloAggEpIdx_AggX8pLine56, 0,  6,  7, "x8p LINE 5-6 -> x8p play 6/7" },
            { ApolloAggEpIdx_AggX8pLine78, 0,  8,  9, "x8p LINE 7-8 -> x8p play 8/9" },
            { ApolloAggEpIdx_AggX8pHp1,    0, 28, 29, "x8p HP 1 -> x8p CUE 1 (28/29)" },
            { ApolloAggEpIdx_AggX8pHp2,    0, 30, 31, "x8p HP 2 -> x8p CUE 2 (30/31)" },
            { ApolloAggEpIdx_AggX8pSpdif,  0, 18, 19, "x8p S/PDIF -> x8p play 18/19" },
            { ApolloAggEpIdx_AggTwxMon,    1,  0,  1, "Twin X MON L/R -> Twin X play 0/1" },
            { ApolloAggEpIdx_AggTwxLine34, 1, 30, 31, "Twin X LINE 3-4 -> Twin X CUE 2 (30/31)" },
            { ApolloAggEpIdx_AggTwxHp,     1, 28, 29, "Twin X HP -> Twin X CUE 1 (28/29)" },
            { ApolloAggEpIdx_AggX8pVirt12, 0, 18, 19, "x8p VIRTUAL 1-2 <- x8p record 18/19" },
            { ApolloAggEpIdx_AggX8pSpdifIn, 0, 16, 17, "x8p S/PDIF in <- x8p record 16/17" },
            { ApolloAggEpIdx_AggTwxVirt34, 1, 20, 21, "Twin X VIRTUAL 3-4 <- Twin X record 20/21" },
        };
        unsigned long n;

        for (n = 0; n < sizeof(o) / sizeof(o[0]); n++) {
            ApolloAggEpResolve(&g_cfg, (unsigned long)o[n].Row, &s);
            Check(s.Count[0] == 1 && s.Count[1] == 1 && Has(&s, 0, o[n].U, o[n].S0) && Has(&s, 1, o[n].U, o[n].S1),
                  o[n].What);
        }
        /* a jack the unit's descriptor does not carry: nowhere (silence), never another channel */
        g_cfg.Unit[0].OutIds[18] = 0x0C01;
        ApolloAggEpResolve(&g_cfg, ApolloAggEpIdx_AggX8pSpdif, &s);
        Check(s.Count[0] == 0 && s.Count[1] == 1 && Has(&s, 1, 0, 19), "an ID missing from the descriptor: that channel silent");
        BaseConfig();
        /* a unit never seen: the row's fallback slots */
        g_cfg.Unit[1].InCount = g_cfg.Unit[1].OutCount = 0;
        ApolloAggEpResolve(&g_cfg, ApolloAggEpIdx_AggTwxHp, &s);
        Check(s.Count[0] == 1 && Has(&s, 0, 1, 28) && Has(&s, 1, 1, 29), "never-seen Twin X: HP fallback 28/29");
        BaseConfig();
        /* the Twin X absent from the configuration: its jacks silent */
        g_cfg.UnitCount = 1;
        g_cfg.Master = 0;
        ApolloAggEpResolve(&g_cfg, ApolloAggEpIdx_AggTwxLine34, &s);
        Check(s.Count[0] == 0 && s.Count[1] == 0, "x8p alone: Twin X LINE 3-4 silent");
        ApolloAggEpResolve(&g_cfg, ApolloAggEpIdx_AggX8pLine12, &s);
        Check(s.Count[0] == 1 && Has(&s, 0, 0, 2), "x8p alone: x8p LINE 1-2 still plays");
        BaseConfig();
    }

    /* Studio: DAW and Windows to the Twin X MON */
    BaseConfig();
    AddRoute(APOLLO_AGG_SRC_DAW, 1, 0x0901, 0x0902);
    AddRoute(APOLLO_AGG_SRC_WINDOWS, 1, 0x0901, 0x0902);
    Check(ApolloAggValidate(&g_cfg) == 0, "Studio config valid");
    ApolloAggEpResolve(&g_cfg, ApolloAggEpIdx_AggMon, &s);
    Check(s.Count[0] == 1 && Has(&s, 0, 1, 0) && Has(&s, 1, 1, 1), "Studio: Monitor -> Twin X MON (sums with the DAW there)");
    /* the DAW route does not move Windows; the layout compile ignores the windows route */
    {
        APOLLO_AGG_UNIT_RT rt[4];
        static APOLLO_AGG_COMPILED a, b;

        memset(rt, 0, sizeof(rt));
        rt[0].Present = rt[1].Present = 1;
        rt[0].Guard = rt[1].Guard = 16;
        rt[0].Lead = rt[1].Lead = 144;
        rt[0].FrameCfg = rt[1].FrameCfg = 8;
        rt[0].Adc = 10;
        rt[1].Adc = 5;
        ApolloAggCompile(&g_cfg, rt, 1, &a);
        g_cfg.RouteCount = 1;
        ApolloAggCompile(&g_cfg, rt, 1, &b);
        Check(memcmp(&a, &b, sizeof(a)) == 0, "the windows route changes nothing ASIO sees");
    }

    /* Mirror: Windows to both units' MON */
    BaseConfig();
    AddRoute(APOLLO_AGG_SRC_WINDOWS, 1, 0x0901, 0x0902);
    AddRoute(APOLLO_AGG_SRC_WINDOWS, 0, 0x0901, 0x0902);
    ApolloAggEpResolve(&g_cfg, ApolloAggEpIdx_AggMon, &s);
    Check(s.Count[0] == 2 && s.Count[1] == 2 && Has(&s, 0, 1, 0) && Has(&s, 0, 0, 0) && Has(&s, 1, 1, 1) && Has(&s, 1, 0, 1),
          "Mirror: Monitor -> Twin X MON and x8p MON");
    ApolloAggEpResolve(&g_cfg, ApolloAggEpIdx_AggMonIn, &s);
    Check(Has(&s, 0, 1, 26) && s.Count[0] == 1, "Mirror: loopback from the first windows route's unit");
    /* the same route twice: once */
    AddRoute(APOLLO_AGG_SRC_WINDOWS, 1, 0x0901, 0x0902);
    ApolloAggEpResolve(&g_cfg, ApolloAggEpIdx_AggMon, &s);
    Check(s.Count[0] == 2, "a destination named twice is ADDed once");

    /* Headphones split: Windows to the Twin X CUE 1 (headphones) */
    BaseConfig();
    AddRoute(APOLLO_AGG_SRC_DAW, 1, 0x0901, 0x0902);
    AddRoute(APOLLO_AGG_SRC_WINDOWS, 1, 0x0A01, 0x0A02);
    ApolloAggEpResolve(&g_cfg, ApolloAggEpIdx_AggMon, &s);
    Check(s.Count[0] == 1 && Has(&s, 0, 1, 28) && Has(&s, 1, 1, 29), "Headphones split: Monitor -> Twin X CUE 1 (28/29)");
    ApolloAggEpResolve(&g_cfg, ApolloAggEpIdx_AggMonIn, &s);
    Check(Has(&s, 0, 1, 26), "Headphones split: loopback still the Twin X monitor mix");

    /* x8p master, no windows route: the x8p MON (it is the monitor unit then) */
    BaseConfig();
    g_cfg.Master = 0;
    ApolloAggEpResolve(&g_cfg, ApolloAggEpIdx_AggMon, &s);
    Check(s.Count[0] == 1 && Has(&s, 0, 0, 0) && Has(&s, 1, 0, 1), "x8p master: Monitor -> x8p MON");
    ApolloAggEpResolve(&g_cfg, ApolloAggEpIdx_AggMonIn, &s);
    Check(Has(&s, 0, 0, 26) && Has(&s, 1, 0, 27), "x8p master: loopback x8p 26/27");

    /* a unit never seen (no descriptor cached): the rows' fallback slots */
    BaseConfig();
    g_cfg.Unit[1].InCount = g_cfg.Unit[1].OutCount = 0;
    ApolloAggEpResolve(&g_cfg, ApolloAggEpIdx_AggMon, &s);
    Check(s.Count[0] == 1 && Has(&s, 0, 1, 0) && Has(&s, 1, 1, 1), "never-seen master: MON fallback 0/1");
    ApolloAggEpResolve(&g_cfg, ApolloAggEpIdx_AggTwxIn2, &s);
    Check(s.Count[0] == 1 && Has(&s, 0, 1, 1), "never-seen Twin X: input fallback slot");
    AddRoute(APOLLO_AGG_SRC_WINDOWS, 1, 0x0A01, 0x0A02);
    ApolloAggEpResolve(&g_cfg, ApolloAggEpIdx_AggMon, &s);
    Check(s.Count[0] == 0 && s.Count[1] == 0, "never-seen unit, non-MON destination: nowhere (silence, never a wrong slot)");

    /* only an x8p configured: Monitor on its MON, the Twin X rows silent */
    BaseConfig();
    g_cfg.UnitCount = 1;
    g_cfg.Master = 0;
    memset(&g_cfg.Unit[1], 0, sizeof(g_cfg.Unit[1]));
    ApolloAggEpResolve(&g_cfg, ApolloAggEpIdx_AggMon, &s);
    Check(s.Count[0] == 1 && Has(&s, 0, 0, 0), "x8p alone: Monitor -> x8p MON");
    ApolloAggEpResolve(&g_cfg, ApolloAggEpIdx_AggTwxIn1, &s);
    Check(s.Count[0] == 0, "x8p alone: TwinX MIC/LINE rows silent");
    /* Twin X first in block order, x8p second: the x8p rows follow the x8p wherever it is */
    BaseConfig();
    {
        APOLLO_AGG_UNIT_CFG t = g_cfg.Unit[0];

        g_cfg.Unit[0] = g_cfg.Unit[1];
        g_cfg.Unit[1] = t;
        g_cfg.Master = 0;
    }
    ApolloAggEpResolve(&g_cfg, ApolloAggEpIdx_AggX8pIn3, &s);
    Check(s.Count[0] == 1 && Has(&s, 0, 1, 2), "block order changed: x8p MIC/LINE 3 still the x8p's slot 2");
    /* broken input: no units, master out of range, unknown row */
    memset(&g_cfg, 0, sizeof(g_cfg));
    ApolloAggEpResolve(&g_cfg, 0, &s);
    Check(s.Count[0] == 0 && s.Count[1] == 0, "no units: nothing");
    BaseConfig();
    g_cfg.Master = 7;
    ApolloAggEpResolve(&g_cfg, 0, &s);
    Check(s.Count[0] == 0, "bad master: nothing");
    BaseConfig();
    ApolloAggEpResolve(&g_cfg, 99, &s);
    Check(s.Count[0] == 0, "unknown row: nothing");
}

/* ---------------------------------------------------------------- 5 */
static void RandomConfig(REF_RT *Rr, APOLLO_AGG_UNIT_RT *Rt, unsigned long *Tl)
{
    unsigned long u, i, n = 1 + Rnd(4);

    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.Version = APOLLO_AGG_VERSION;
    g_cfg.Bytes = sizeof(g_cfg);
    g_cfg.Flags = APOLLO_AGG_CFG_ENABLED | APOLLO_AGG_CFG_ALIGN | (Rnd(4) == 0 ? APOLLO_AGG_CFG_EXPLICIT : 0);
    g_cfg.UnitCount = n;
    g_cfg.Master = Rnd(n);
    g_cfg.AsrcHalf = (Rnd(3) == 0) ? 8 : (Rnd(2) ? 16 : 32);
    for (u = 0; u < n; u++) {
        APOLLO_AGG_UNIT_CFG *uc = &g_cfg.Unit[u];
        int twx = Rnd(2) != 0;

        uc->Uid = 0x1000 + u * 7 + Rnd(3);
        uc->Model = Rnd(8) == 0 ? 0 : (twx ? APOLLO_INFO_MODEL_TWINX : APOLLO_INFO_MODEL_X8P);
        if (Rnd(5) != 0) {
            uc->InCount = (UCHAR)(twx ? 34 : 32);
            uc->OutCount = (UCHAR)(twx ? 46 : 34);
            memcpy(uc->InIds, twx ? kTwxIn : kX8pIn, twx ? sizeof(kTwxIn) : sizeof(kX8pIn));
            memcpy(uc->OutIds, twx ? kTwxOut : kX8pOut, twx ? sizeof(kTwxOut) : sizeof(kX8pOut));
        }
    }
    if (g_cfg.Flags & APOLLO_AGG_CFG_EXPLICIT) {
        g_cfg.InCount = Rnd(40);
        g_cfg.OutCount = Rnd(40);
        for (i = 0; i < g_cfg.InCount; i++) {
            g_cfg.In[i].Unit = (UCHAR)Rnd(n);
            g_cfg.In[i].Slot = (USHORT)Rnd(40);
            g_cfg.In[i].Flags = (UCHAR)(Rnd(6) == 0 ? APOLLO_AGG_CHAN_HIDDEN : 0);
        }
        for (i = 0; i < g_cfg.OutCount; i++) {
            g_cfg.Out[i].Unit = (UCHAR)Rnd(n);
            g_cfg.Out[i].Slot = (USHORT)Rnd(50);
        }
    }
    g_cfg.RouteCount = Rnd(5);
    for (i = 0; i < g_cfg.RouteCount; i++) {
        static const unsigned short ids[] = { 0x0901, 0x0902, 0x0A01, 0x0A02, 0x0A03, 0x0A04, 0x0301, 0x0302, 0x7777 };
        APOLLO_AGG_ROUTE_CFG *r = &g_cfg.Route[i];

        r->SrcKind = (UCHAR)(1 + Rnd(3));
        r->SrcUnit = (UCHAR)Rnd(n);
        r->DstUnit = (UCHAR)Rnd(n);
        r->SrcId[0] = ids[Rnd(9)];
        r->SrcId[1] = ids[Rnd(9)];
        r->DstId[0] = ids[Rnd(9)];
        r->DstId[1] = ids[Rnd(9)];
    }
    memset(Rt, 0, sizeof(APOLLO_AGG_UNIT_RT) * APOLLO_AGG_MAX_UNITS);
    memset(Rr, 0, sizeof(REF_RT) * APOLLO_AGG_MAX_UNITS);
    for (u = 0; u < APOLLO_AGG_MAX_UNITS; u++) {
        Rr[u].Present = Rt[u].Present = (int)Rnd(2);
        Rr[u].Guard = Rt[u].Guard = Rnd(65);
        Rr[u].Lead = Rt[u].Lead = 64 + Rnd(961);
        Rr[u].Adc = Rt[u].Adc = Rnd(21);
        Rr[u].FrameCfg = Rt[u].FrameCfg = 8;
    }
    *Tl = (Rnd(5) == 0) ? 0xFFFFFFFFul : Rnd(n);
}

static void TestAlignTrims(void)
{
    static APOLLO_AGG_COMPILED a;
    static unsigned char b[sizeof(APOLLO_AGG_COMPILED) + 64];
    APOLLO_AGG_UNIT_RT rt[APOLLO_AGG_MAX_UNITS];
    REF_RT rr[APOLLO_AGG_MAX_UNITS];
    unsigned long it, u, same = 0, padSame = 0, offSame = 0, offs = 0, tl;
    int okUnaligned = 1, okTrim = 1, okMargin = 1, okRenMargin = 1;

    Check(ApolloAggTrimOf(0) == 0 && ApolloAggTrimOf(5) == 5 && ApolloAggTrimOf(0xFFFFFFFBul) == -5 &&
          ApolloAggTrimOf(64) == 64 && ApolloAggTrimOf(65) == 64 && ApolloAggTrimOf(0xFFFFFFBFul) == -64 &&
          ApolloAggTrimOf(0x7FFFFFFFul) == 64 && ApolloAggTrimOf(0x80000000ul) == -64, "trim registry values, clamped to +/-64");
    Check(RefG6CompiledBytes() == sizeof(APOLLO_AGG_COMPILED), "compiled layout: same size as G6's");

    /* ALIGN set, every trim 0: G6 exactly */
    for (it = 0; it < 20000; it++) {
        RandomConfig(rr, rt, &tl);
        ApolloAggCompile(&g_cfg, rt, tl, &a);
        RefG6Compile(&g_cfg, rr, tl, b);
        same += (memcmp(&a, b, sizeof(a)) == 0);
        padSame += (ApolloAggPad(&g_cfg, rt, tl) == RefG6Pad(&g_cfg, rr, tl));
        {
            unsigned long t = (tl < g_cfg.UnitCount) ? tl : g_cfg.Master;

            for (u = 0; u < g_cfg.UnitCount; u++) {
                if (u == t) {
                    continue;
                }
                offs++;
                offSame += (ApolloAggCapOffset(&g_cfg, rt, t, u) == RefG6DAdc(rr, t, u) &&
                            ApolloAggRenOffset(rt, t, u, g_cfg.AsrcHalf) == 0);
            }
        }
    }
    printf("G6 identity (ALIGN, trims 0): layout %lu/20000, pad %lu/20000, follower offsets %lu/%lu\n", same, padSame,
           offSame, offs);
    Check(same == 20000, "ALIGN + trims 0: the compiled layout is G6's, byte for byte");
    Check(padSame == 20000, "ALIGN + trims 0: the pad is G6's");
    Check(offSame == offs && offs > 1000, "ALIGN + trims 0: follower capture offset = G6 DAdc, render offset 0");

    for (it = 0; it < 20000; it++) {
        unsigned long t, pad;
        long maxOff = 0;

        RandomConfig(rr, rt, &tl);
        t = (tl < g_cfg.UnitCount) ? tl : g_cfg.Master;
        for (u = 0; u < APOLLO_AGG_MAX_UNITS; u++) {
            rt[u].Guard = 16;               /* the margin argument assumes the units' guards are equal (defaults) */
            rt[u].CapTrim = (long)Rnd(129) - 64;
            rt[u].RenTrim = (long)Rnd(129) - 64;
        }
        /* trimmed and aligned: pad = h + M + the largest follower offset (>= 0) */
        pad = ApolloAggPad(&g_cfg, rt, tl);
        for (u = 0; u < g_cfg.UnitCount; u++) {
            long off;

            if (u == t) {
                continue;
            }
            off = ApolloAggCapOffset(&g_cfg, rt, t, u);
            if (off != ((long)rt[u].Adc - (long)rt[t].Adc) - (rt[u].CapTrim - rt[t].CapTrim)) {
                okTrim = 0;
            }
            {
                /* [G7 review] render: the trim difference, "earlier" clamped to what the lead leaves */
                long raw = -(rt[u].RenTrim - rt[t].RenTrim);
                long max = ApolloAggRenOffsetMax(rt[t].Lead, g_cfg.AsrcHalf, rt[u].FrameCfg);
                long ro = ApolloAggRenOffset(rt, t, u, g_cfg.AsrcHalf);

                if (ro != (raw > max ? max : raw) || ApolloAggRenOffsetRaw(rt, t, u) != raw) {
                    okTrim = 0;
                }
                /*
                 * The follower is written up to staging head (Pm + lead) - h - offset; one tick later it must
                 * still be ahead of Pf + FRAME_CFG + 4 by the margin M (else RenderLate every tick).
                 */
                if (max > 0 && (long)rt[t].Lead - (long)g_cfg.AsrcHalf - ro - APOLLO_AGG_TICK_FRAMES <
                               (long)rt[u].FrameCfg + 4 + APOLLO_AGG_MARGIN) {
                    okRenMargin = 0;
                }
            }
            if (off > maxOff) {
                maxOff = off;
            }
            /*
             * The resampler reads follower frames up to z + h with z = phi(x) + off and x at most the head
             * H = Pm - guard - pad; with phi(Pm) = Pf that is Pf - guard - (pad - h - off): it must stay
             * M frames inside what the follower has recorded (spec 6.3.3 margin).
             */
            if ((long)pad - (long)g_cfg.AsrcHalf - off < APOLLO_AGG_MARGIN && g_cfg.UnitCount >= 2) {
                okMargin = 0;
            }
        }
        if (g_cfg.UnitCount >= 2 && pad != g_cfg.AsrcHalf + APOLLO_AGG_MARGIN + (unsigned long)maxOff) {
            okTrim = 0;
        }
        if (g_cfg.UnitCount < 2 && pad != 0) {
            okTrim = 0;                     /* one unit: no pad, whatever the trims (x8p or Twin X alone) */
        }
        /* unaligned: no pad, followers h + M behind (the same margin), latency = the timeline's own */
        g_cfg.Flags &= ~APOLLO_AGG_CFG_ALIGN;
        if (ApolloAggPad(&g_cfg, rt, tl) != 0) {
            okUnaligned = 0;
        }
        ApolloAggCompile(&g_cfg, rt, tl, &a);
        if (a.Pad != 0 || a.InLatency != rt[t].Guard + rt[t].Adc) {
            okUnaligned = 0;
        }
        for (u = 0; u < g_cfg.UnitCount; u++) {
            if (u != t && (ApolloAggCapOffset(&g_cfg, rt, t, u) != -(long)(g_cfg.AsrcHalf + APOLLO_AGG_MARGIN) ||
                           0 - (long)g_cfg.AsrcHalf - ApolloAggCapOffset(&g_cfg, rt, t, u) < APOLLO_AGG_MARGIN)) {
                okUnaligned = 0;
            }
        }
    }
    Check(okTrim, "trims: offsets are the trim differences (render earlier clamped), pad covers the largest one");
    Check(okRenMargin, "render trims: a follower is always written M frames ahead of what its hardware fetches next tick");
    Check(okMargin, "aligned + trims: every follower read stays M frames inside its recording");
    Check(okUnaligned, "unaligned: pad 0, latency = timeline guard + ADC, followers h + M behind");

    /* the measured G6 figures: aligned 58 / 152 at 128 -> 186 / 280; unaligned 21 */
    BaseConfig();
    memset(rt, 0, sizeof(rt));
    rt[0].Present = rt[1].Present = 1;
    rt[0].Guard = rt[1].Guard = 16;
    rt[0].Lead = rt[1].Lead = 144;
    rt[0].FrameCfg = rt[1].FrameCfg = 8;
    rt[0].Adc = 10;
    rt[1].Adc = 5;
    ApolloAggCompile(&g_cfg, rt, 1, &a);
    Check(a.InLatency == 58 && a.OutLatency == 152 && a.Pad == 37, "x8p + Twin X aligned: in 58, out 152 (G6)");
    rt[0].CapTrim = -5;                     /* take out the +5 frames ADC alignment of a digital loopback */
    ApolloAggCompile(&g_cfg, rt, 1, &a);
    Check(a.Pad == 42 && a.InLatency == 63 && ApolloAggCapOffset(&g_cfg, rt, 1, 0) == 10, "x8p CaptureTrim -5: offset 10, pad 42");
    rt[0].CapTrim = 0;
    g_cfg.Flags &= ~APOLLO_AGG_CFG_ALIGN;
    ApolloAggCompile(&g_cfg, rt, 1, &a);
    Check(a.InLatency == 21 && a.Pad == 0 && ApolloAggCapOffset(&g_cfg, rt, 1, 0) == -32,
          "unaligned: in 21 (the Twin X's own), x8p read 32 frames behind");
}

/* ---------------------------------------------------------------- 6 */
static void TestTick(void)
{
    unsigned long reclaim = 0, i, why = 0;

    Check(ApolloAggStageZeroFrom(0, 100000, 4096) == 100000 - 2048, "staging: first tick zeroes the last half ring");
    Check(ApolloAggStageZeroFrom(99952, 100000, 4096) == 99952, "staging: from the last head");
    Check(ApolloAggStageZeroFrom(100100, 100000, 4096) == 100000 - 2048, "staging: a head from another run");
    Check(ApolloAggStageZeroFrom(90000, 100000, 4096) == 100000 - 2048, "staging: at most half the ring");
    Check(ApolloAggStageZeroFrom(0, 1000, 4096) == 0, "staging: early positions");
    Check(ApolloAggStreamFrom(0, 100000, 8, 100144) == 100144, "streams: first tick renders nothing (start at the lead)");
    Check(ApolloAggStreamFrom(100096, 100000, 8, 100144) == 100096, "streams: from the last head");
    Check(ApolloAggStreamFrom(99000, 100000, 8, 100144) == 100008, "streams: late tick, not before FRAME_CFG");
    /* takeover / reclaim */
    Check(ApolloAggTakeOverWhy(0, 0, 1, 1000, &reclaim) == 1 && reclaim == 0, "timeline stopped: any member takes over");
    Check(ApolloAggTakeOverWhy(0, 1, 0, 1000, &reclaim) == 1, "timeline stopped: taken over even by a slow master");
    Check(ApolloAggTakeOverWhy(1, 0, 1, 1000, &reclaim) == 0 && reclaim == 0, "timeline runs: a follower never takes it");
    for (i = 0; i < 999; i++) {
        why |= ApolloAggTakeOverWhy(1, 1, 1, 1000, &reclaim);
    }
    Check(why == 0 && reclaim == 999, "master running again: waits 1 s of its own ticks");
    Check(ApolloAggTakeOverWhy(1, 1, 1, 1000, &reclaim) == 2 && reclaim == 0, "master reclaims its timeline after 1 s");
    reclaim = 500;
    Check(ApolloAggTakeOverWhy(1, 1, 0, 1000, &reclaim) == 0 && reclaim == 0, "master stops again: the count restarts");
    Check(APOLLO_AGG_MAX_LAG == 1024, "capture catch-up bounded to 1024 frames per tick (G6 review)");
    /* [G7 review] a head from the timeline's previous run (its transport restarted): start over */
    Check(ApolloAggStreamFrom(5000000, 2000, 8, 2144) == 2144, "streams: a head beyond To (another run) starts at the lead");
    Check(ApolloAggStageZeroFrom(5000000, 2144, 4096) == 96, "staging: a head beyond To zeroes the last half ring");
    Check(!ApolloAggTimelineRestarted(0, 1, 1, 5000000, 2000, 3, 4), "restart: nothing recorded yet, nothing to compare");
    Check(!ApolloAggTimelineRestarted(1, 0, 1, 5000000, 2000, 3, 4), "restart: another timeline unit's record only records");
    Check(!ApolloAggTimelineRestarted(1, 1, 1, 5000000, 5000048, 3, 3), "restart: the same run moving on");
    Check(!ApolloAggTimelineRestarted(1, 1, 1, 5000000, 5000000, 3, 3), "restart: a tick without progress");
    Check(ApolloAggTimelineRestarted(1, 1, 1, 5000000, 2000, 3, 3), "restart: the position went back (transport restarted)");
    Check(ApolloAggTimelineRestarted(1, 1, 1, 5000000, 5009000, 3, 4), "restart: the engine was started again (EngStarts)");
    /* [G7 review] how much earlier a follower may play */
    Check(ApolloAggRenOffsetMax(144, 16, 8) == 52 && ApolloAggRenOffsetMax(144, 32, 8) == 36 &&
          ApolloAggRenOffsetMax(144, 8, 8) == 60 && ApolloAggRenOffsetMax(64, 16, 8) == 0 &&
          ApolloAggRenOffsetMax(1024, 16, 8) == 932, "render offset limit: 52 / 36 / 60 at the defaults, 0 for a tiny lead");
    {
        APOLLO_AGG_UNIT_RT rt2[APOLLO_AGG_MAX_UNITS];

        memset(rt2, 0, sizeof(rt2));
        rt2[0].Lead = rt2[1].Lead = 144;
        rt2[0].FrameCfg = rt2[1].FrameCfg = 8;
        rt2[1].RenTrim = 64;                /* the timeline (1) plays later: the follower (0) "earlier" by 128 */
        rt2[0].RenTrim = -64;
        Check(ApolloAggRenOffsetRaw(rt2, 1, 0) == 128 && ApolloAggRenOffset(rt2, 1, 0, 16) == 52 &&
              ApolloAggRenOffset(rt2, 1, 0, 32) == 36, "render trim +128 earlier: clamped to 52 (h 16) / 36 (h 32)");
        rt2[1].RenTrim = -64;
        rt2[0].RenTrim = 64;
        Check(ApolloAggRenOffset(rt2, 1, 0, 32) == -128, "render trim later (-128): kept, it reads older staging");
        rt2[0].RenTrim = rt2[1].RenTrim = 0;
        Check(ApolloAggRenOffset(rt2, 1, 0, 16) == 0, "equal render trims: 0 (G6)");
    }
}

/* ---------------------------------------------------------------- 7 */
static void TestRatios(void)
{
    APOLLO_AGG_RATIO t[APOLLO_AGG_RATIOS];
    double r = 0;
    unsigned long i;

    Check(ApolloAggPpbOf(1.0) == 0 && ApolloAggPpbOf(1.0 + 4.74e-6) == 4740 && ApolloAggPpbOf(1.0 - 4.7404e-6) == -4740 &&
          ApolloAggPpbOf(1.002) == 500000 && ApolloAggPpbOf(0.9) == -500000, "ppb: rounded, clamped to the loop's +/-500 ppm");
    memset(t, 0, sizeof(t));
    Check(ApolloAggRatioFind(t, 4, 1, 2, &r) == 0, "empty table: no start ratio");
    Check(ApolloAggRatioPut(t, 4, 0x3A00C0DE, 0x2F00BEEF, 1.0 + 4.76e-6, 1) == 1, "first lock kept");
    Check(ApolloAggRatioFind(t, 4, 0x3A00C0DE, 0x2F00BEEF, &r) == 1 && fabs(r - (1.0 + 4.76e-6)) < 1e-12,
          "same pair: the kept ratio");
    Check(ApolloAggRatioFind(t, 4, 0x2F00BEEF, 0x3A00C0DE, &r) == 2 && fabs(r * (1.0 + 4.76e-6) - 1.0) < 1e-12,
          "reverse pair (master changed): the inverse");
    Check(ApolloAggRatioPut(t, 4, 0x3A00C0DE, 0x2F00BEEF, 1.0 + 4.779e-6, 2) == 0 && t[0].Ppb == 4760,
          "19 ppb away: not worth a write");
    Check(ApolloAggRatioPut(t, 4, 0x3A00C0DE, 0x2F00BEEF, 1.0 + 4.78e-6, 3) == 1 && t[0].Ppb == 4780 && t[1].Follower == 0,
          "20 ppb away: updated in place");
    Check(ApolloAggRatioPut(t, 4, 0x2F00BEEF, 0x3A00C0DE, 1.0 - 4.78e-6, 4) == 1 &&
          ApolloAggRatioFind(t, 4, 0x2F00BEEF, 0x3A00C0DE, &r) == 1 && fabs(r - (1.0 - 4.78e-6)) < 1e-12,
          "the direct pair wins over the reverse one");
    Check(ApolloAggRatioPut(t, 4, 5, 6, 1.0, 5) == 1 && ApolloAggRatioPut(t, 4, 7, 8, 1.0, 6) == 1, "four pairs fit");
    Check(ApolloAggRatioPut(t, 4, 9, 10, 1.0 + 1e-6, 7) == 1 && ApolloAggRatioFind(t, 4, 9, 10, &r) == 1 &&
          ApolloAggRatioFind(t, 4, 0x3A00C0DE, 0x2F00BEEF, &r) == 2, "full: the oldest entry goes (the reverse fills in)");
    Check(ApolloAggRatioPut(t, 4, 3, 3, 1.0, 8) == 0 && ApolloAggRatioPut(t, 4, 0, 3, 1.0, 8) == 0, "nonsense pairs refused");
    memset(t, 0, sizeof(t));
    t[0].Follower = 1; t[0].Timeline = 2; t[0].Ppb = 600000; t[0].Seq = 9;
    t[1].Follower = 4; t[1].Timeline = 4; t[1].Ppb = 10; t[1].Seq = 9;
    Check(ApolloAggRatioFind(t, 4, 1, 2, &r) == 0 && ApolloAggRatioFind(t, 4, 4, 4, &r) == 0, "unusable entries ignored");
    Check(ApolloAggRatioPut(t, 4, 1, 2, 1.0 + 2e-6, 10) == 1 && t[0].Ppb == 2000, "an unusable entry is overwritten");
    for (i = 0; i < APOLLO_AGG_RATIOS; i++) {
        Check(sizeof(t[i]) == 24, "the stored entry is 24 bytes (REG_BINARY of 96)");
    }
}

/* ---------------------------------------------------------------- 8 */

/*
 * [polish Nov 15] The combined paths per sample. agg.c's ASIO capture, combined Windows capture and follower
 * render take every sample from apollo_agg.h: ApolloAggRingSample on the timeline unit (an integer copy) and
 * ApolloAggAsrcSample on a follower. Checked here:
 *   a. the Monitor / Monitor mix endpoints of the morning configuration (Twin X master and timeline, the DAW
 *      route to its MON; also no route and Studio) name only the master, which is not resampled; the layout
 *      flags agree (TIMELINE, no RESAMPLED, no look-ahead in its latency);
 *   b. the timeline copy is bit-exact for every slot and every value, and equals agg.c's code before;
 *   c. a master-only loop at ratio 1 (Windows render ADDed into the zeroed master ring, the unit's loopback,
 *      combined capture) gives back every sample bit for bit;
 *   d. the follower path is unchanged: ApolloAggAsrcSample equals agg.c's inline dot before (copied below)
 *      bit for bit, capture rings and the staging rings alike;
 *   e. the G7 CHECK of Nov 15 (wasapi-loop "Monitor" -> "Monitor mix": steady errors 1.9e-3 / 2.0e-3, level
 *      -11.98 dBFS, 0 dropouts) is not a fractional offset: wasapi-loop's sine recurrence ignores any constant
 *      delay and gain, so a 0.05-frame offset through the driver's ASRC, or the follower's drift, stays far
 *      under its 1e-4 threshold; other signal in the monitor mix at the level that reads +0.02 dB (about
 *      -35 dBFS, e.g. a live input strip) gives errors of exactly that size.
 */
#define MP_FRAMES   8192u
#define MP_CH       34u
static long g_mpRing[MP_FRAMES * MP_CH];
static long g_mpPlay[MP_FRAMES * MP_CH];
static long g_mpSine[65536];
static APOLLO_ASRC_TABLE g_mpTab;

/* agg.c before [polish]: AggSampleAt's timeline branch (the ASIO capture's tframe[] read is the same) */
static long OldTimelineSample(const long *Ring, unsigned long Frames, unsigned long Channels, unsigned long Slot,
                              unsigned long long X)
{
    const long *fr = (const long *)((const unsigned char *)Ring + (size_t)(X & (Frames - 1)) * Channels * 4);

    return (Slot < Channels) ? fr[Slot] : 0;
}

/* agg.c before [polish]: the follower dot of AggAsioCapture / AggSampleAt (stride Channels * 4 bytes) */
static long OldFollowerSample(const long *Ring, unsigned long Frames, unsigned long Channels, unsigned long Slot,
                              long long Base, const float *W, unsigned long Taps, float Gain)
{
    unsigned long fmask = Frames - 1, fstride = Channels * 4, j;
    float acc = 0;

    for (j = 0; j < Taps; j++) {
        const long *src = (const long *)((const unsigned char *)Ring +
                                         (size_t)((unsigned long long)(Base + (long long)j) & fmask) * fstride);

        acc += W[j] * (float)src[Slot];
    }
    return ApolloAsrcToSample(acc * Gain);
}

/* agg.c before [polish]: AggFollowerRender's dot over one staging ring (4096 frames, one channel) */
static long OldStagingSample(const long *Ring, long long B, const float *W, unsigned long Taps, float Gain)
{
    unsigned long j;
    float acc = 0;

    for (j = 0; j < Taps; j++) {
        acc += W[j] * (float)Ring[(unsigned long long)(B + (long long)j) & 4095ull];
    }
    return ApolloAsrcToSample(acc * Gain);
}

static long Rnd32(void)
{
    unsigned long v = (Rnd(0x10000) << 16) | Rnd(0x10000);

    switch (Rnd(16)) {
    case 0: return (long)0x80000000ul;
    case 1: return 0x7FFFFFFFL;
    case 2: return 0;
    case 3: return (long)0x7FFFFF00L;
    default: return (long)v;
    }
}

/* wasapi-loop's check: the sine recurrence x[n] = 2 cos(w) x[n-1] - x[n-2] on float samples; worst |error|, level */
typedef struct _MP_LOOP {
    double c2, x1, x2, worst, sumSq;
    unsigned long n;
} MP_LOOP;

static void LoopInit(MP_LOOP *L)
{
    memset(L, 0, sizeof(*L));
    L->c2 = 2.0 * cos(2.0 * 3.14159265358979323846 * 997.0 / 48000.0);
}

static void LoopPut(MP_LOOP *L, long S32)
{
    double x = (double)S32 / 2147483648.0;

    if (L->n >= 2) {
        double r = fabs(x - L->c2 * L->x1 + L->x2);

        if (r > L->worst) {
            L->worst = r;
        }
    }
    L->sumSq += x * x;
    L->x2 = L->x1;
    L->x1 = x;
    L->n++;
}

static double LoopLevel(const MP_LOOP *L)
{
    return 20.0 * log10(sqrt(L->sumSq / (double)L->n) * sqrt(2.0));
}

/* The Windows sample of the 997 Hz -12 dBFS test tone at frame n (wasapi-loop's float, as the engine hands it over), plus Extra */
static long ToneS32(unsigned long n, double ExtraAmp, double ExtraHz)
{
    const double pi = 3.14159265358979323846;
    double v = pow(10.0, -12.0 / 20.0) * sin(2.0 * pi * 997.0 * (double)n / 48000.0) +
               ExtraAmp * sin(2.0 * pi * ExtraHz * (double)n / 48000.0 + 0.3);

    return (long)floor(v * 8388607.0 + 0.5) * 256;     /* 24 bits at the top of the S32 slot, as the rings hold */
}

static void TestMasterPath(void)
{
    APOLLO_AGG_SREF s;
    APOLLO_AGG_UNIT_RT rt[APOLLO_AGG_MAX_UNITS];
    static APOLLO_AGG_COMPILED lay;
    unsigned long i, k, c, cfgN, bad;
    char msg[256];

    /* a. the endpoints name the master only; the master is the timeline and is not resampled */
    memset(rt, 0, sizeof(rt));
    rt[0].Present = rt[1].Present = 1;
    rt[0].Guard = rt[1].Guard = 16;
    rt[0].Lead = rt[1].Lead = 144;
    rt[0].FrameCfg = rt[1].FrameCfg = 8;
    rt[0].Adc = 10;
    rt[1].Adc = 5;
    for (cfgN = 0; cfgN < 3; cfgN++) {
        const char *name = cfgN == 0 ? "no route" : (cfgN == 1 ? "the morning config (DAW route to the Twin X MON)" : "Studio");
        int ok = 1;

        BaseConfig();
        if (cfgN >= 1) {
            AddRoute(APOLLO_AGG_SRC_DAW, 1, 0x0901, 0x0902);
        }
        if (cfgN == 2) {
            AddRoute(APOLLO_AGG_SRC_WINDOWS, 1, 0x0901, 0x0902);
        }
        for (k = 0; k < 2; k++) {
            ApolloAggEpResolve(&g_cfg, k == 0 ? ApolloAggEpIdx_AggMon : ApolloAggEpIdx_AggMonIn, &s);
            for (c = 0; c < 2; c++) {
                unsigned long d;

                ok &= s.Count[c] >= 1;
                for (d = 0; d < s.Count[c]; d++) {
                    ok &= s.Unit[c][d] == g_cfg.Master && !ApolloAggResampled(s.Unit[c][d], g_cfg.Master);
                }
            }
        }
        ApolloAggCompile(&g_cfg, rt, g_cfg.Master, &lay);
        for (i = 0; i < lay.InCount; i++) {
            const APOLLO_AGG_LCHAN *L = &lay.In[i];

            if (L->Unit == g_cfg.Master) {
                ok &= (L->Flags & APOLLO_AGG_CH_TIMELINE) != 0 && (L->Flags & APOLLO_AGG_CH_RESAMPLED) == 0 &&
                      L->LatencyFrames == rt[1].Adc + rt[1].Guard;
            } else {
                ok &= (L->Flags & APOLLO_AGG_CH_RESAMPLED) != 0 && ApolloAggResampled(L->Unit, g_cfg.Master);
            }
        }
        sprintf_s(msg, sizeof(msg), "%s: Monitor / Monitor mix on the Twin X master only, which is the timeline: copied, "
                  "not resampled (layout flags agree)", name);
        Check(ok, msg);
    }

    /* b. the timeline copy: every slot, every value, three times round the ring */
    for (i = 0; i < MP_FRAMES * MP_CH; i++) {
        g_mpRing[i] = Rnd32();
    }
    bad = 0;
    for (k = 0; k < 3 * MP_FRAMES; k += 1 + Rnd(3)) {
        unsigned long long x = 0x123456789ull + k;

        for (c = 0; c < MP_CH; c++) {
            long v = ApolloAggRingSample(g_mpRing, MP_FRAMES, MP_CH, c, x);

            bad += v != g_mpRing[(x & (MP_FRAMES - 1)) * MP_CH + c] || v != OldTimelineSample(g_mpRing, MP_FRAMES, MP_CH, c, x);
        }
        bad += ApolloAggRingSample(g_mpRing, MP_FRAMES, MP_CH, MP_CH, x) != 0;
    }
    Check(bad == 0, "timeline sample: the ring's own value, bit for bit, every slot (0x80000000 and 0x7FFFFFFF included), "
          "as agg.c read it before; a slot past the frame reads 0");

    /* c. a master-only loop at ratio 1: Windows render ADDs into the zeroed master ring at the Monitor slots, the
          unit loops MON L/R to record 26/27, the combined capture reads them: the same samples come back */
    BaseConfig();
    AddRoute(APOLLO_AGG_SRC_DAW, 1, 0x0901, 0x0902);
    {
        APOLLO_AGG_SREF ren, cap;
        MP_LOOP lp;
        unsigned long long x0 = 5000000ull;

        ApolloAggEpResolve(&g_cfg, ApolloAggEpIdx_AggMon, &ren);
        ApolloAggEpResolve(&g_cfg, ApolloAggEpIdx_AggMonIn, &cap);
        memset(g_mpPlay, 0, sizeof(g_mpPlay));
        LoopInit(&lp);
        bad = 0;
        for (k = 0; k < 48000; k++) {
            unsigned long long x = x0 + k;
            long src = ToneS32(k, 0.0, 0.0), got[2];
            long *play = g_mpPlay + (x & (MP_FRAMES - 1)) * MP_CH;
            long *rec = g_mpRing + (x & (MP_FRAMES - 1)) * MP_CH;

            for (c = 0; c < 2; c++) {
                play[ren.Slot[c][0]] = ApolloSatAdd32(play[ren.Slot[c][0]], src);    /* agg.c AggStreamRender, timeline */
            }
            rec[26] = play[0];                                                     /* the unit's MON loopback */
            rec[27] = play[1];
            for (c = 0; c < 2; c++) {
                got[c] = ApolloAggResampled(cap.Unit[c][0], g_cfg.Master) ? 0 :
                         ApolloAggRingSample(g_mpRing, MP_FRAMES, MP_CH, cap.Slot[c][0], x);     /* AggSampleAt */
                bad += got[c] != src;
            }
            LoopPut(&lp, got[0]);
            play[0] = play[1] = 0;      /* the engine's zero horizon, before the ring comes round */
        }
        sprintf_s(msg, sizeof(msg), "master-only loop at ratio 1: 48000 frames back bit for bit (%lu differ); wasapi-loop's "
                  "check reads %.1e worst, level %.2f dBFS", bad, lp.worst, LoopLevel(&lp));
        Check(bad == 0 && lp.worst < 1e-6 && fabs(LoopLevel(&lp) + 12.0) < 0.005, msg);
    }

    /* d. the follower path unchanged: capture rings (34 and 32 slots) and the staging rings (one channel) */
    ApolloAsrcBuild(&g_mpTab, 16);
    bad = 0;
    for (k = 0; k < 20000; k++) {
        float w[2 * APOLLO_ASRC_MAX_HALF];
        long long base = (long long)Rnd(3 * MP_FRAMES) - (long long)MP_FRAMES;
        float gain = (k % 3 == 0) ? 1.0f : ((k % 3 == 1) ? 0.37f : 0.0f);
        unsigned long ch = (k & 1) ? 34 : 32, slot = Rnd(ch);

        ApolloAsrcWeights(&g_mpTab, (double)Rnd(100000) / 100000.0, w);
        bad += ApolloAggAsrcSample(g_mpRing, MP_FRAMES, ch, slot, base, w, 32, gain) !=
               OldFollowerSample(g_mpRing, MP_FRAMES, ch, slot, base, w, 32, gain);
        bad += ApolloAggAsrcSample(g_mpRing, 4096, 1, 0, base, w, 32, gain) != OldStagingSample(g_mpRing, base, w, 32, gain);
    }
    Check(bad == 0, "follower samples (capture and staging) equal agg.c's code before, bit for bit (20000 random cases)");

    /* e. the G7 CHECK */
    for (i = 0; i < 65536; i++) {
        g_mpSine[i] = ToneS32(i, 0.0, 0.0);
    }
    {
        MP_LOOP direct, frac, drift, other;
        float w[2 * APOLLO_ASRC_MAX_HALF];
        const double ppm = 4.765e-6;

        LoopInit(&direct);
        LoopInit(&frac);
        LoopInit(&drift);
        LoopInit(&other);
        ApolloAsrcWeights(&g_mpTab, 0.05, w);
        for (k = 0; k < 48000; k++) {
            double z = 1000.05 + (double)k * (1.0 + ppm);
            long long n = ApolloAggFloor(z);
            float wd[2 * APOLLO_ASRC_MAX_HALF];

            LoopPut(&direct, ApolloAggRingSample(g_mpSine, 65536, 1, 0, 1000 + k));
            LoopPut(&frac, ApolloAggAsrcSample(g_mpSine, 65536, 1, 0, 1000 + (long long)k - 15, w, 32, 1.0f));
            ApolloAsrcWeights(&g_mpTab, z - (double)n, wd);
            LoopPut(&drift, ApolloAggAsrcSample(g_mpSine, 65536, 1, 0, n - 15, wd, 32, 1.0f));
            LoopPut(&other, ToneS32(k, pow(10.0, -35.4 / 20.0), 3000.0));
        }
        sprintf_s(msg, sizeof(msg), "wasapi-loop's check through the master copy: worst %.1e (the 24-bit floor), level %.2f dBFS",
                  direct.worst, LoopLevel(&direct));
        Check(direct.worst < 1e-6, msg);
        sprintf_s(msg, sizeof(msg), "a constant 0.05-frame offset through the ASRC: worst %.1e, under the 1e-4 threshold "
                  "(the check ignores constant delay and gain): it cannot have caused the 1.9e-3", frac.worst);
        Check(frac.worst < 1e-5, msg);
        sprintf_s(msg, sizeof(msg), "the follower path drifting at +4.765 ppm (the x8p): worst %.1e, under the threshold too",
                  drift.worst);
        Check(drift.worst < 1e-4, msg);
        sprintf_s(msg, sizeof(msg), "other signal at -35.4 dBFS in the mix (3 kHz here): worst %.1e, level %.2f dBFS = the "
                  "G7 run's 1.9e-3 / 2.0e-3 at -11.98", other.worst, LoopLevel(&other));
        Check(other.worst > 1e-3 && other.worst < 4e-3 && fabs(LoopLevel(&other) + 11.98) < 0.006, msg);
    }
}

int main(int argc, char **argv)
{
    const char *inf = (argc > 1) ? argv[1] : "..\\..\\driver\\apollo.inf";

    TestTableChoice();
    BaseConfig();
    TestRows();
    TestInf(inf);
    TestResolve();
    TestAlignTrims();
    TestTick();
    TestRatios();
    TestMasterPath();
    printf("aggep-test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
