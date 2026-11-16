/*
 * endpoints.c - the tables of Windows audio endpoints (docs/PHASE2.md section 3.1).
 *
 * [Endpoints] What UA's own driver gives Windows users: every physical output pair of a unit is its
 * own playback endpoint named by its jack, every mic/line input a mono recording endpoint, plus the
 * monitor-mix loopback; VIRTUAL and S/PDIF inputs opt-in. The rows are data in endpoint_plan.h (standalone,
 * per model) and apollo_agg.h APOLLO_AGG_EP_LIST (the combined set); here only their component IDs are
 * added. Every row's reference string has its three interfaces in apollo.inf (aggep-test checks it).
 *
 * Row i exists when bit i of the unit's registry value Endpoints (Parameters\Units\<key>, else the x8p's
 * legacy Parameters\Endpoints, else the model's default) is set; device add creates every row's circuit
 * object and PrepareHardware adds the ones the value names (circuit.c). The combined table: bit i of
 * Parameters\Aggregate\Endpoints. All reference strings and component IDs are new since the rows of earlier
 * builds ("ApolloMon", ...), so Windows gives every endpoint a new ID and none inherits a name it cached
 * for an old one (tools\phase1\clean-endpoints.ps1 removes those).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "apollo_stream.h"

/* ------------------------------------------------------------------ */
/* The Apollo x8p, standalone ("<pin> (Apollo x8p)")                   */
/* ------------------------------------------------------------------ */

static const GUID ApolloX8pMonComponent =
    { 0xE06AFE31, 0xEF8E, 0x40D4, { 0xB5, 0xD5, 0x37, 0xD9, 0x01, 0x9D, 0x41, 0x7A } };
static const GUID ApolloX8pLine12Component =
    { 0x0F316240, 0x827A, 0x4983, { 0x8D, 0xD1, 0xB9, 0xD3, 0xF0, 0xAF, 0xBE, 0xE8 } };
static const GUID ApolloX8pLine34Component =
    { 0xB1768DE1, 0x6E8D, 0x416D, { 0x86, 0x29, 0xE3, 0x58, 0xA7, 0x51, 0xBB, 0x2E } };
static const GUID ApolloX8pLine56Component =
    { 0x14162AB3, 0x08DD, 0x4451, { 0x8A, 0x17, 0x10, 0x17, 0x81, 0x2A, 0x06, 0x1A } };
static const GUID ApolloX8pLine78Component =
    { 0x50DE0E07, 0x7CA0, 0x4269, { 0x9B, 0x1F, 0x3C, 0xC3, 0x7F, 0x3D, 0xD7, 0xD9 } };
static const GUID ApolloX8pHp1Component =
    { 0x4D0F85B8, 0xC5F6, 0x4D7E, { 0xAF, 0xC1, 0xB9, 0xA4, 0x59, 0x88, 0xC6, 0x04 } };
static const GUID ApolloX8pHp2Component =
    { 0x9BF8AFE4, 0x35C3, 0x4B25, { 0x99, 0xFA, 0x6B, 0x3F, 0x5E, 0x80, 0xF2, 0x1D } };
static const GUID ApolloX8pSpdifComponent =
    { 0x3273DBDD, 0x2517, 0x4A4E, { 0xBC, 0xF7, 0x00, 0x1F, 0xD6, 0x25, 0x47, 0x5E } };
static const GUID ApolloX8pMixComponent =
    { 0xF0223CB5, 0x79A1, 0x4EE5, { 0x83, 0xC2, 0x75, 0xC8, 0xF0, 0xB3, 0xE3, 0xC8 } };
static const GUID ApolloX8pIn1Component =
    { 0x641AF4FF, 0xAFBA, 0x40DD, { 0xA1, 0x8C, 0x4F, 0x75, 0x29, 0x27, 0x97, 0x2C } };
static const GUID ApolloX8pIn2Component =
    { 0x78934C21, 0x2D4C, 0x4BEC, { 0x8E, 0x7E, 0xBC, 0x0E, 0x47, 0x63, 0x48, 0x64 } };
static const GUID ApolloX8pIn3Component =
    { 0x3ACC1B15, 0x02CE, 0x404E, { 0xA7, 0x9A, 0x40, 0x5C, 0x2A, 0x50, 0x63, 0x6D } };
static const GUID ApolloX8pIn4Component =
    { 0x01A2931B, 0x7B77, 0x437B, { 0xBB, 0xDB, 0x04, 0x9F, 0x0A, 0x78, 0xC9, 0x39 } };
static const GUID ApolloX8pIn5Component =
    { 0x7229BB2E, 0x7C93, 0x46FA, { 0x95, 0x44, 0x88, 0x9E, 0x5B, 0x3A, 0xF6, 0x4C } };
static const GUID ApolloX8pIn6Component =
    { 0xF2BD086F, 0x6F90, 0x44A0, { 0x97, 0xDE, 0x80, 0x7A, 0x7F, 0x5A, 0x52, 0x3B } };
static const GUID ApolloX8pIn7Component =
    { 0xF23F27A3, 0xA017, 0x484F, { 0x92, 0x2C, 0x65, 0x97, 0xD1, 0x1B, 0x5B, 0x6D } };
static const GUID ApolloX8pIn8Component =
    { 0x22F2ED34, 0xDD48, 0x4118, { 0xBC, 0x59, 0xE5, 0xA7, 0x09, 0xC8, 0x18, 0x45 } };
static const GUID ApolloX8pVirt12Component =
    { 0xAB731CFD, 0x4732, 0x4B0E, { 0xB8, 0x81, 0xA9, 0xC9, 0x29, 0x3F, 0xC3, 0x7C } };
static const GUID ApolloX8pVirt34Component =
    { 0x3586E258, 0x2EF5, 0x4491, { 0xAB, 0x36, 0xFC, 0x43, 0x86, 0x0C, 0x39, 0x91 } };
static const GUID ApolloX8pVirt56Component =
    { 0xA07B97F8, 0x8D2B, 0x4CA3, { 0xBC, 0xAC, 0x9C, 0xCA, 0x75, 0x9E, 0xCE, 0x22 } };
static const GUID ApolloX8pVirt78Component =
    { 0x6FB08DB4, 0xE8D5, 0x489F, { 0xBE, 0xEC, 0x07, 0xFA, 0x82, 0x6B, 0x7A, 0xDE } };
static const GUID ApolloX8pSpdifInComponent =
    { 0xFB226DE6, 0xEE14, 0x4323, { 0xB8, 0x5F, 0xC5, 0x23, 0xD1, 0xA4, 0x85, 0x9C } };

#define APOLLO_X8P_ACX_ROW(sym, ref, pin, render, ch, bridge, never, def, i0, i1, f0, f1) \
    { L##ref, L##pin, &Apollo##sym##Component, (render) ? AcxCircuitTypeRender : AcxCircuitTypeCapture, \
      &KSNODETYPE_##bridge, (ch), { (i0), (i1) }, { (f0), (f1) }, 0, APOLLO_INFO_MODEL_X8P },

const APOLLO_ENDPOINT_DESC g_ApolloEndpoints[] = { APOLLO_X8P_EP_LIST(APOLLO_X8P_ACX_ROW) };

/* Every row needs a bit in the DWORD mask and an APOLLO_ACX slot. */
C_ASSERT(RTL_NUMBER_OF(g_ApolloEndpoints) <= APOLLO_ENDPOINTS_ROWS_MAX);
C_ASSERT(APOLLO_ENDPOINTS_ROWS_MAX <= 32);
/* the default mask names rows of this table; the plan (bringup_plan.h, checked offline) says the same */
C_ASSERT(RTL_NUMBER_OF(g_ApolloEndpoints) == APOLLO_X8P_EP_ROWS);
C_ASSERT(APOLLO_X8P_EP_LIST_COUNT == APOLLO_X8P_EP_ROWS);
C_ASSERT(APOLLO_X8P_EP_LIST_DEFAULT == APOLLO_X8P_EP_DEFAULT);
C_ASSERT(APOLLO_X8P_EP_ALL == (1ul << RTL_NUMBER_OF(g_ApolloEndpoints)) - 1);
C_ASSERT(APOLLO_EP_DEFAULT_MASK == APOLLO_X8P_EP_DEFAULT);
C_ASSERT(APOLLO_ADC_FRAMES == APOLLO_X8P_ADC_FRAMES);

const ULONG g_ApolloEndpointCount = RTL_NUMBER_OF(g_ApolloEndpoints);

/* ------------------------------------------------------------------ */
/* The Apollo Twin X Gen 1, standalone ("<pin> (Apollo Twin X)")       */
/* ------------------------------------------------------------------ */

/*
 * Mode 2 only. Own reference strings and component IDs: its INF section (Apollo_TwinX_Install) gives
 * them the "Apollo Twin X" name, and nothing of it can be mistaken for the x8p's circuits.
 * [Endpoints] Its three output jacks (MON L/R, LINE 3-4 = CUE 2, HP = CUE 1), the loopback (never a default),
 * MIC/LINE 1-2 (the input strips' signal; in the default set), VIRTUAL 1-4 and S/PDIF in.
 */
static const GUID ApolloTwxMonComponent =
    { 0x76931447, 0xD261, 0x4CC1, { 0xBD, 0xE7, 0x19, 0x84, 0xA9, 0xE8, 0x4D, 0x1C } };
static const GUID ApolloTwxLine34Component =
    { 0x70B36368, 0xEFD8, 0x4720, { 0x85, 0xE3, 0xC1, 0xD1, 0xB3, 0xF9, 0xD8, 0xDD } };
static const GUID ApolloTwxHpComponent =
    { 0x78D7F1E6, 0xFEB6, 0x44AE, { 0xB6, 0xDA, 0x63, 0xED, 0x99, 0xF2, 0xF4, 0x7D } };
static const GUID ApolloTwxMixComponent =
    { 0x6A61EDE8, 0xAD51, 0x4536, { 0x85, 0xF6, 0x1A, 0x12, 0x3C, 0x35, 0x71, 0x56 } };
static const GUID ApolloTwxIn1Component =
    { 0x3262928C, 0xFEE4, 0x46BD, { 0xAA, 0x6E, 0xEA, 0x5A, 0xF1, 0xE7, 0x04, 0x56 } };
static const GUID ApolloTwxIn2Component =
    { 0x2D1C8358, 0x2B1D, 0x44F0, { 0xA5, 0xA7, 0x00, 0x19, 0x2E, 0xFD, 0x84, 0xF0 } };
static const GUID ApolloTwxVirt12Component =
    { 0x12BF23A2, 0xA7A0, 0x42B2, { 0xB4, 0xB2, 0xF9, 0x0A, 0x5F, 0x5B, 0x93, 0x4D } };
static const GUID ApolloTwxVirt34Component =
    { 0xAD32783C, 0x5F7A, 0x471B, { 0x9A, 0x73, 0xA8, 0x2D, 0x7B, 0xC3, 0xEB, 0x26 } };
static const GUID ApolloTwxSpdifInComponent =
    { 0xC5838E42, 0xC0EE, 0x4163, { 0xA2, 0xC2, 0xF3, 0x27, 0xD9, 0x7B, 0xB4, 0xE7 } };

#define APOLLO_TWX_ACX_ROW(sym, ref, pin, render, ch, bridge, never, def, i0, i1, f0, f1) \
    { L##ref, L##pin, &Apollo##sym##Component, (render) ? AcxCircuitTypeRender : AcxCircuitTypeCapture, \
      &KSNODETYPE_##bridge, (ch), { (i0), (i1) }, { (f0), (f1) }, 0, APOLLO_INFO_MODEL_TWINX },

static const APOLLO_ENDPOINT_DESC g_ApolloTwinXEndpoints[] = { APOLLO_TWX_EP_LIST(APOLLO_TWX_ACX_ROW) };

C_ASSERT(RTL_NUMBER_OF(g_ApolloTwinXEndpoints) == APOLLO_TWX_EP_ROWS);
C_ASSERT(APOLLO_TWX_EP_LIST_COUNT == APOLLO_TWX_EP_ROWS);
C_ASSERT(APOLLO_TWX_EP_ALL == (1ul << RTL_NUMBER_OF(g_ApolloTwinXEndpoints)) - 1);
C_ASSERT(APOLLO_TWX_EP_LIST_DEFAULT == APOLLO_TWX_EP_DEFAULT && (APOLLO_TWX_EP_DEFAULT & ~APOLLO_TWX_EP_ALL) == 0);
C_ASSERT(APOLLO_DEF_ENDPOINTS_TWINX == APOLLO_TWX_EP_DEFAULT);
C_ASSERT(APOLLO_DEF_ENDPOINTS_X8P == APOLLO_X8P_EP_DEFAULT);

/* ------------------------------------------------------------------ */
/* The combined set ("<unit> <pin> (Apollo)")                          */
/* ------------------------------------------------------------------ */

/*
 * The combined table (docs/spec-twinx.md 6.7, 6.9.7): created only on the host unit (the x8p) when
 * Parameters\Aggregate\EndpointSet is 1 (circuit.c). Rows come from APOLLO_AGG_EP_LIST in apollo_agg.h
 * (reference strings, pin names, bridge, kinds, IDs, fallback slots), the one list the offline test checks
 * against apollo.inf; here only their component IDs are added. Their streams run on the aggregate timeline
 * (AggRow1) through its routing and resampler, like the combined ASIO client: ChannelId/FallbackSlot are
 * informative only.
 * [Endpoints] "Monitor" (the windows routes) and its loopback, then both units' output jacks and inputs.
 */
static const GUID ApolloAggMonComponent =
    { 0x3BADB294, 0x33D4, 0x4B9F, { 0xAF, 0xE1, 0xF4, 0x03, 0x88, 0x8E, 0x94, 0xE8 } };
static const GUID ApolloAggX8pMonComponent =
    { 0x1002D161, 0x5D70, 0x47B2, { 0x83, 0x9E, 0x87, 0x7D, 0xD5, 0xB8, 0xC6, 0x18 } };
static const GUID ApolloAggX8pLine12Component =
    { 0xBD86D08C, 0xBE98, 0x48C6, { 0x9D, 0x97, 0x64, 0x0B, 0xA7, 0x0C, 0xD6, 0x55 } };
static const GUID ApolloAggX8pLine34Component =
    { 0xB69353EB, 0xCC99, 0x48DB, { 0x89, 0x9F, 0x75, 0x1D, 0xB4, 0xA7, 0x21, 0x5B } };
static const GUID ApolloAggX8pLine56Component =
    { 0xA2D60A63, 0xBD04, 0x4327, { 0x9B, 0xCE, 0xFC, 0x9C, 0x9E, 0xDF, 0x70, 0xEA } };
static const GUID ApolloAggX8pLine78Component =
    { 0xB8192C35, 0x4427, 0x4C43, { 0x83, 0x7E, 0x83, 0xAE, 0x5E, 0xD2, 0x20, 0x09 } };
static const GUID ApolloAggX8pHp1Component =
    { 0x0649C774, 0x5EE0, 0x4026, { 0x9F, 0xD2, 0x61, 0xC9, 0x9F, 0xFF, 0xBA, 0xFA } };
static const GUID ApolloAggX8pHp2Component =
    { 0x8BD096C6, 0x36C8, 0x42B3, { 0xB9, 0xC9, 0xAE, 0xD2, 0x3D, 0xCD, 0x07, 0xC4 } };
static const GUID ApolloAggX8pSpdifComponent =
    { 0xE76B2934, 0x472A, 0x4784, { 0x90, 0x91, 0xE2, 0x62, 0x64, 0xB6, 0xDC, 0x57 } };
static const GUID ApolloAggTwxMonComponent =
    { 0x8AA7125B, 0xD431, 0x4CCC, { 0x87, 0x6C, 0x94, 0x3E, 0xC4, 0x70, 0xE0, 0xF8 } };
static const GUID ApolloAggTwxLine34Component =
    { 0xFDBD1A7F, 0x86F5, 0x4C3A, { 0xA9, 0x18, 0x8C, 0x50, 0xB7, 0x5E, 0x46, 0x81 } };
static const GUID ApolloAggTwxHpComponent =
    { 0x896A7E2D, 0xAD51, 0x48D1, { 0xAA, 0x35, 0x25, 0x92, 0x03, 0x9E, 0xC0, 0xE5 } };
static const GUID ApolloAggMonInComponent =
    { 0x8398A214, 0xEB24, 0x4CFF, { 0xA4, 0x12, 0x13, 0x56, 0x14, 0x1B, 0x43, 0xBB } };
static const GUID ApolloAggX8pIn1Component =
    { 0x2314062C, 0xA4A4, 0x4751, { 0x87, 0x08, 0x6C, 0xBA, 0x54, 0x87, 0xEB, 0x6C } };
static const GUID ApolloAggX8pIn2Component =
    { 0x2161722B, 0xC94E, 0x4CE1, { 0x95, 0x38, 0x8A, 0xDF, 0xAF, 0x43, 0xF2, 0x37 } };
static const GUID ApolloAggX8pIn3Component =
    { 0x34F4859E, 0xB4EC, 0x4CF4, { 0xA2, 0xE8, 0x18, 0xE8, 0x08, 0x5A, 0x64, 0xFC } };
static const GUID ApolloAggX8pIn4Component =
    { 0x4D8A6AA6, 0x9378, 0x48A1, { 0xAA, 0x7A, 0x8B, 0x1B, 0xD7, 0x2B, 0x4F, 0x28 } };
static const GUID ApolloAggX8pIn5Component =
    { 0x247B42F7, 0x21C3, 0x437E, { 0x91, 0x07, 0x10, 0x7C, 0x27, 0x12, 0x31, 0x86 } };
static const GUID ApolloAggX8pIn6Component =
    { 0x8258827C, 0x782A, 0x4280, { 0x9D, 0x66, 0x23, 0xBC, 0x47, 0xEF, 0xA8, 0xF3 } };
static const GUID ApolloAggX8pIn7Component =
    { 0xBAF5C253, 0xDFAE, 0x4555, { 0x95, 0xBA, 0x58, 0x81, 0x5A, 0x40, 0xA7, 0x63 } };
static const GUID ApolloAggX8pIn8Component =
    { 0x68587497, 0x2C1A, 0x44E1, { 0xB3, 0x3F, 0x5A, 0x0E, 0xF9, 0x7D, 0x67, 0x38 } };
static const GUID ApolloAggTwxIn1Component =
    { 0x39874008, 0xA1BB, 0x45EC, { 0x9F, 0xA0, 0x84, 0x4B, 0x80, 0xF3, 0x2D, 0x4F } };
static const GUID ApolloAggTwxIn2Component =
    { 0x6605AC72, 0x15B3, 0x466F, { 0x92, 0x35, 0x39, 0xDB, 0x67, 0x27, 0xFB, 0x54 } };
static const GUID ApolloAggX8pVirt12Component =
    { 0xA9AF859E, 0x1878, 0x4AA2, { 0x88, 0xFC, 0x1D, 0xD4, 0x8A, 0x5F, 0xDC, 0x4F } };
static const GUID ApolloAggX8pVirt34Component =
    { 0x2E09A0BA, 0x155B, 0x45AF, { 0xA6, 0xBC, 0x56, 0xCC, 0x01, 0xB1, 0x96, 0x76 } };
static const GUID ApolloAggX8pVirt56Component =
    { 0x2683B1FB, 0x533C, 0x4B2E, { 0xAE, 0x55, 0xBF, 0x6B, 0xE4, 0x90, 0x37, 0x50 } };
static const GUID ApolloAggX8pVirt78Component =
    { 0xCB8D8BC8, 0xBC7D, 0x477F, { 0x9D, 0xF1, 0xD9, 0x1A, 0x2B, 0x44, 0xB0, 0x9D } };
static const GUID ApolloAggX8pSpdifInComponent =
    { 0x57C3C982, 0x3FB1, 0x4D25, { 0xB1, 0x18, 0xBD, 0xAD, 0x78, 0x38, 0x29, 0x21 } };
static const GUID ApolloAggTwxVirt12Component =
    { 0xF45834BB, 0x24B7, 0x4540, { 0xAE, 0x7C, 0x65, 0x27, 0xAA, 0xCC, 0xAB, 0x61 } };
static const GUID ApolloAggTwxVirt34Component =
    { 0x6BF70C67, 0x5A5D, 0x437D, { 0x93, 0xF5, 0x8D, 0xF2, 0x7A, 0x87, 0x50, 0xA0 } };
static const GUID ApolloAggTwxSpdifInComponent =
    { 0x27F6F4AD, 0x2924, 0x41D9, { 0x98, 0x84, 0x64, 0x77, 0xE5, 0x5E, 0x94, 0x60 } };

/* AggRow1 = the row's index in the list + 1 (apollo_agg.h ApolloAggEpIdx_*): marks the row as combined. */
#define APOLLO_AGG_ACX_ROW(sym, ref, pin, kind, render, ch, bridge, never, def, model, i0, i1, f0, f1) \
    { L##ref, L##pin, &Apollo##sym##Component, (render) ? AcxCircuitTypeRender : AcxCircuitTypeCapture, \
      &KSNODETYPE_##bridge, (ch), { (i0), (i1) }, { (f0), (f1) }, (UCHAR)(ApolloAggEpIdx_##sym + 1), (UCHAR)(model) },

static const APOLLO_ENDPOINT_DESC g_ApolloAggEndpoints[] = { APOLLO_AGG_EP_LIST(APOLLO_AGG_ACX_ROW) };

C_ASSERT(RTL_NUMBER_OF(g_ApolloAggEndpoints) == APOLLO_AGG_EP_ROWS);
C_ASSERT(APOLLO_AGG_EP_LIST_COUNT == APOLLO_AGG_EP_ROWS);
C_ASSERT(APOLLO_AGG_EP_ROWS <= APOLLO_ENDPOINTS_ROWS_MAX);
C_ASSERT(APOLLO_AGG_EP_LIST_DEFAULT == APOLLO_AGG_EP_DEFAULT);
C_ASSERT(APOLLO_AGG_EP_ROWS == 32 || (APOLLO_AGG_EP_DEFAULT >> APOLLO_AGG_EP_ROWS) == 0);

/*
 * The table object: Model x8p (the host; PrepareHardware adds it only on an identified x8p), ADC
 * unused (combined streams report the aggregate's latency).
 */
const APOLLO_ENDPOINT_TABLE g_ApolloAggEndpointTable = {
    APOLLO_MODEL_X8P, g_ApolloAggEndpoints, RTL_NUMBER_OF(g_ApolloAggEndpoints), APOLLO_X8P_ADC_FRAMES,
    APOLLO_AGG_EP_DEFAULT
};

static const APOLLO_ENDPOINT_TABLE g_ApolloEndpointTables[] = {
    { APOLLO_MODEL_X8P,   g_ApolloEndpoints,      RTL_NUMBER_OF(g_ApolloEndpoints),      APOLLO_X8P_ADC_FRAMES,
      APOLLO_X8P_EP_DEFAULT },
    { APOLLO_MODEL_TWINX, g_ApolloTwinXEndpoints, RTL_NUMBER_OF(g_ApolloTwinXEndpoints), APOLLO_TWX_ADC_FRAMES,
      APOLLO_TWX_EP_DEFAULT },
};

const APOLLO_ENDPOINT_TABLE *ApolloEndpointTableFor(_In_ int Model)
{
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(g_ApolloEndpointTables); i++) {
        if (g_ApolloEndpointTables[i].Model == Model) {
            return &g_ApolloEndpointTables[i];
        }
    }
    return NULL;
}

/*
 * The ring slot of a channel: the first descriptor slot carrying its channel ID (spec-streaming 1.5.1;
 * the descriptor's layout: bringup_plan.h ApolloIoDescCount / ApolloIoDescId).
 * [Endpoints] With a readable descriptor, an ID it does not carry (or carries beyond the unit's transport
 * count, e.g. the x8p's CUE 3/4 past its legacy 34 play slots) has no slot: APOLLO_EP_SLOT_NONE, and the
 * stream plays and records nothing there (stream.c), never another channel. Without one (not read yet):
 * the row's verified fallback, or no slot when even that is past the unit's count.
 */
ULONG ApolloEndpointSlot(_In_ PDEVICE_CONTEXT Ctx, _In_ const APOLLO_ENDPOINT_DESC *Desc,
                         _In_ ULONG Channel)
{
    BOOLEAN render = (Desc->Type == AcxCircuitTypeRender);
    const ULONG *d = render ? Ctx->IoDescOut : Ctx->IoDescIn;
    ULONG count = ApolloIoDescCount(d, render ? 0 : 1);      /* 0: not a usable descriptor */
    ULONG limit = render ? Ctx->PlayChannels : Ctx->RecChannels;
    ULONG fallback = Desc->FallbackSlot[Channel], i;

    if (count != 0) {
        for (i = 0; i < count; i++) {
            if (ApolloIoDescId(d, i) == Desc->ChannelId[Channel]) {
                return (i < limit) ? i : APOLLO_EP_SLOT_NONE;
            }
        }
        return APOLLO_EP_SLOT_NONE;
    }
    return (fallback < limit) ? fallback : APOLLO_EP_SLOT_NONE;
}
