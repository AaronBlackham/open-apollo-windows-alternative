/*
 * endpoint_plan.h - [Endpoints] the standalone Windows endpoint tables of each model, as data: what UA's own
 * driver gives Windows users (every physical output pair its own playback endpoint, named by its jack; every
 * mic/line input a mono recording endpoint; the monitor-mix loopback), plus the VIRTUAL and S/PDIF inputs
 * (off by default). Plain C like bringup_plan.h: no kernel or Windows headers. driver\endpoints.c builds the
 * ACX rows from these lists (adding only the component IDs), tools\agg-test\aggep-test checks them against
 * driver\apollo.inf, and the combined table (apollo_agg.h APOLLO_AGG_EP_LIST) carries the same jacks.
 *
 * X(sym, reference string, pin name, render, channels, bridge, NeverSetAsDefault, in the default set,
 *   channel id 0, channel id 1, fallback slot 0, fallback slot 1)
 *
 *   reference string  the ACX circuit name = the INF reference string. "Ap2..." since this table (Nov 15):
 *                     new strings give new Windows endpoint IDs, so names Windows cached for the old rows
 *                     ("MON L/R", "Speakers") never stick to these (tools\phase1\clean-endpoints.ps1 removes
 *                     the old ones).
 *   pin name          what Sound settings shows, "<pin> (<device>)": "LINE 1-2 (Apollo x8p)".
 *   bridge            LINE_CONNECTOR (line and monitor outputs, the loopback, VIRTUAL/S/PDIF inputs: Windows
 *                     keeps the pin name; a Speaker form factor would be renamed "Speakers"), HEADPHONES (the
 *                     HP outputs), MICROPHONE (the mic/line inputs: what conferencing apps look for).
 *   channel ids       IO descriptor IDs (type << 8 | number). The slot is looked up by ID in the unit's live
 *                     descriptor (endpoints.c ApolloEndpointSlot): an ID the descriptor does not carry plays
 *                     and records nothing, it never lands on another channel. Mono rows repeat channel 0.
 *   fallback slots    the slots of the descriptors read on the hardware (G3 log; x8p legacy 34 play / 32
 *                     record): used only while no descriptor has been read.
 *
 * Apollo x8p descriptor (play): MON 0901/0902 at 0-1, LINE 1-8 0301-0308 at 2-9, ADAT 10-17, S/PDIF
 * 0501/0502 at 18-19, VIRTUAL 20-27, CUE 1-4 0A01-0A08 at 28-35 (legacy count 34). HP 1 and HP 2 are fed by
 * CUE 1 and CUE 2 (their default headphone sources). Record: MIC/LINE 1-8 at 0-7, ADAT 8-15, S/PDIF 16-17,
 * VIRTUAL 18-25, MON (the monitor mix) 26-27.
 * Apollo Twin X Gen 1: the descriptor is an x8p-like superset (34 record / 46 playback), but the unit has
 * only MON L/R, LINE 3/4 and one headphone jack. UA's route table names its LINE 3/4 0x0A03/0x0A04 (CUE 2)
 * and its HP 0x0A01/0x0A02 (CUE 1) (oa:devices/apollo-twin-x.json "routes"); the 0x0303/0x0304 the superset
 * also lists drive no jack. Inputs: MIC/LINE/HIZ 1-2 at 0-1, S/PDIF (optical, S/PDIF mode) 16-17, VIRTUAL
 * 1-4 at 18-21 (virtual_max 4), MON 26-27.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#define APOLLO_EP_BRIDGE_LINE_CONNECTOR 0
#define APOLLO_EP_BRIDGE_HEADPHONES     1
#define APOLLO_EP_BRIDGE_MICROPHONE     2

#define APOLLO_X8P_EP_LIST(X) \
    X(X8pMon,     "Ap2X8pMon",     "MON L/R",     1, 2, LINE_CONNECTOR, 0, 1, 0x0901, 0x0902,  0,  1) \
    X(X8pLine12,  "Ap2X8pLine12",  "LINE 1-2",    1, 2, LINE_CONNECTOR, 0, 1, 0x0301, 0x0302,  2,  3) \
    X(X8pLine34,  "Ap2X8pLine34",  "LINE 3-4",    1, 2, LINE_CONNECTOR, 0, 1, 0x0303, 0x0304,  4,  5) \
    X(X8pLine56,  "Ap2X8pLine56",  "LINE 5-6",    1, 2, LINE_CONNECTOR, 0, 1, 0x0305, 0x0306,  6,  7) \
    X(X8pLine78,  "Ap2X8pLine78",  "LINE 7-8",    1, 2, LINE_CONNECTOR, 0, 1, 0x0307, 0x0308,  8,  9) \
    X(X8pHp1,     "Ap2X8pHp1",     "HP 1",        1, 2, HEADPHONES,     0, 1, 0x0A01, 0x0A02, 28, 29) \
    X(X8pHp2,     "Ap2X8pHp2",     "HP 2",        1, 2, HEADPHONES,     0, 1, 0x0A03, 0x0A04, 30, 31) \
    X(X8pSpdif,   "Ap2X8pSpdif",   "S/PDIF",      1, 2, LINE_CONNECTOR, 0, 1, 0x0501, 0x0502, 18, 19) \
    X(X8pMix,     "Ap2X8pMix",     "Monitor mix", 0, 2, LINE_CONNECTOR, 1, 1, 0x0901, 0x0902, 26, 27) \
    X(X8pIn1,     "Ap2X8pIn1",     "MIC/LINE 1",  0, 1, MICROPHONE,     0, 1, 0x0001, 0x0001,  0,  0) \
    X(X8pIn2,     "Ap2X8pIn2",     "MIC/LINE 2",  0, 1, MICROPHONE,     0, 1, 0x0002, 0x0002,  1,  1) \
    X(X8pIn3,     "Ap2X8pIn3",     "MIC/LINE 3",  0, 1, MICROPHONE,     0, 1, 0x0103, 0x0103,  2,  2) \
    X(X8pIn4,     "Ap2X8pIn4",     "MIC/LINE 4",  0, 1, MICROPHONE,     0, 1, 0x0104, 0x0104,  3,  3) \
    X(X8pIn5,     "Ap2X8pIn5",     "MIC/LINE 5",  0, 1, MICROPHONE,     0, 1, 0x0305, 0x0305,  4,  4) \
    X(X8pIn6,     "Ap2X8pIn6",     "MIC/LINE 6",  0, 1, MICROPHONE,     0, 1, 0x0306, 0x0306,  5,  5) \
    X(X8pIn7,     "Ap2X8pIn7",     "MIC/LINE 7",  0, 1, MICROPHONE,     0, 1, 0x0307, 0x0307,  6,  6) \
    X(X8pIn8,     "Ap2X8pIn8",     "MIC/LINE 8",  0, 1, MICROPHONE,     0, 1, 0x0308, 0x0308,  7,  7) \
    X(X8pVirt12,  "Ap2X8pVirt12",  "VIRTUAL 1-2", 0, 2, LINE_CONNECTOR, 0, 0, 0x0701, 0x0702, 18, 19) \
    X(X8pVirt34,  "Ap2X8pVirt34",  "VIRTUAL 3-4", 0, 2, LINE_CONNECTOR, 0, 0, 0x0703, 0x0704, 20, 21) \
    X(X8pVirt56,  "Ap2X8pVirt56",  "VIRTUAL 5-6", 0, 2, LINE_CONNECTOR, 0, 0, 0x0705, 0x0706, 22, 23) \
    X(X8pVirt78,  "Ap2X8pVirt78",  "VIRTUAL 7-8", 0, 2, LINE_CONNECTOR, 0, 0, 0x0707, 0x0708, 24, 25) \
    X(X8pSpdifIn, "Ap2X8pSpdifIn", "S/PDIF",      0, 2, LINE_CONNECTOR, 0, 0, 0x0501, 0x0502, 16, 17)

#define APOLLO_TWX_EP_LIST(X) \
    X(TwxMon,     "Ap2TwxMon",     "MON L/R",     1, 2, LINE_CONNECTOR, 0, 1, 0x0901, 0x0902,  0,  1) \
    X(TwxLine34,  "Ap2TwxLine34",  "LINE 3-4",    1, 2, LINE_CONNECTOR, 0, 1, 0x0A03, 0x0A04, 30, 31) \
    X(TwxHp,      "Ap2TwxHp",      "HP",          1, 2, HEADPHONES,     0, 1, 0x0A01, 0x0A02, 28, 29) \
    X(TwxMix,     "Ap2TwxMix",     "Monitor mix", 0, 2, LINE_CONNECTOR, 1, 1, 0x0901, 0x0902, 26, 27) \
    X(TwxIn1,     "Ap2TwxIn1",     "MIC/LINE 1",  0, 1, MICROPHONE,     0, 1, 0x0001, 0x0001,  0,  0) \
    X(TwxIn2,     "Ap2TwxIn2",     "MIC/LINE 2",  0, 1, MICROPHONE,     0, 1, 0x0002, 0x0002,  1,  1) \
    X(TwxVirt12,  "Ap2TwxVirt12",  "VIRTUAL 1-2", 0, 2, LINE_CONNECTOR, 0, 0, 0x0701, 0x0702, 18, 19) \
    X(TwxVirt34,  "Ap2TwxVirt34",  "VIRTUAL 3-4", 0, 2, LINE_CONNECTOR, 0, 0, 0x0703, 0x0704, 20, 21) \
    X(TwxSpdifIn, "Ap2TwxSpdifIn", "S/PDIF",      0, 2, LINE_CONNECTOR, 0, 0, 0x0501, 0x0502, 16, 17)

/* Row indexes as constants: ApolloX8pEpIdx_X8pMon = 0, ..., ApolloTwxEpIdx_TwxMon = 0, ... */
#define APOLLO_EP_ENUM_X8P(sym, ref, pin, render, ch, bridge, never, def, i0, i1, f0, f1) ApolloX8pEpIdx_##sym,
#define APOLLO_EP_ENUM_TWX(sym, ref, pin, render, ch, bridge, never, def, i0, i1, f0, f1) ApolloTwxEpIdx_##sym,
enum { APOLLO_X8P_EP_LIST(APOLLO_EP_ENUM_X8P) ApolloX8pEpIdxEnd };
enum { APOLLO_TWX_EP_LIST(APOLLO_EP_ENUM_TWX) ApolloTwxEpIdxEnd };

/* Row counts and default sets, computed from the lists (bringup_plan.h states them as numbers; both assert). */
#define APOLLO_EP_COUNT_ONE(sym, ref, pin, render, ch, bridge, never, def, i0, i1, f0, f1) + 1
#define APOLLO_EP_DEFBIT_X8P(sym, ref, pin, render, ch, bridge, never, def, i0, i1, f0, f1) \
    | ((def) ? (1ul << ApolloX8pEpIdx_##sym) : 0ul)
#define APOLLO_EP_DEFBIT_TWX(sym, ref, pin, render, ch, bridge, never, def, i0, i1, f0, f1) \
    | ((def) ? (1ul << ApolloTwxEpIdx_##sym) : 0ul)
#define APOLLO_X8P_EP_LIST_COUNT        (0 APOLLO_X8P_EP_LIST(APOLLO_EP_COUNT_ONE))
#define APOLLO_TWX_EP_LIST_COUNT        (0 APOLLO_TWX_EP_LIST(APOLLO_EP_COUNT_ONE))
#define APOLLO_X8P_EP_LIST_DEFAULT      (0ul APOLLO_X8P_EP_LIST(APOLLO_EP_DEFBIT_X8P))
#define APOLLO_TWX_EP_LIST_DEFAULT      (0ul APOLLO_TWX_EP_LIST(APOLLO_EP_DEFBIT_TWX))

/* Every row of a table of N rows (N <= 32: masks are one DWORD; a 33rd row needs a QWORD value). */
static __inline unsigned long ApolloEpAllMask(unsigned long N)
{
    return (N >= 32ul) ? 0xFFFFFFFFul : ((1ul << N) - 1ul);
}

/* One row, for the offline checks (the driver builds its ACX rows from the lists directly). */
typedef struct _APOLLO_EP_PLAN_ROW {
    const char     *Ref;
    const char     *Pin;
    unsigned char   Render;
    unsigned char   Channels;
    unsigned char   Bridge;         /* APOLLO_EP_BRIDGE_* */
    unsigned char   NeverDefault;   /* INF NeverSetAsDefault (the monitor-mix loopback) */
    unsigned char   Default;        /* in the default set */
    unsigned short  Id[2];
    unsigned char   Fallback[2];
} APOLLO_EP_PLAN_ROW;

#define APOLLO_EP_PLAN_ROW_INIT(sym, ref, pin, render, ch, bridge, never, def, i0, i1, f0, f1) \
    { ref, pin, render, ch, APOLLO_EP_BRIDGE_##bridge, never, def, { i0, i1 }, { f0, f1 } },

/* Model: 1 = the x8p's table, 2 = the Twin X's (apollo_model.h APOLLO_MODEL_*); NULL past the end. */
static __inline const APOLLO_EP_PLAN_ROW *ApolloEpPlanRow(int Model, unsigned long I)
{
    static const APOLLO_EP_PLAN_ROW x8p[] = { APOLLO_X8P_EP_LIST(APOLLO_EP_PLAN_ROW_INIT) };
    static const APOLLO_EP_PLAN_ROW twx[] = { APOLLO_TWX_EP_LIST(APOLLO_EP_PLAN_ROW_INIT) };

    if (Model == 1) {
        return (I < sizeof(x8p) / sizeof(x8p[0])) ? &x8p[I] : 0;
    }
    if (Model == 2) {
        return (I < sizeof(twx) / sizeof(twx[0])) ? &twx[I] : 0;
    }
    return 0;
}
