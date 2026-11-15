/*
 * ref-g6.c - the combined device's layout code exactly as gate G6 shipped it (ref/apollo_agg_g6.h, a
 * frozen copy), compiled on its own and exported through plain functions, so aggep-test.c can run
 * the G7 header and the G6 one on the same inputs and require the same results (ALIGN set, every
 * trim 0: "Combined as before G7").
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <windows.h>
#include <string.h>
#include "ref\apollo_agg_g6.h"

/* The runtime values per unit, without the G7 trims (the G6 struct has none). */
typedef struct _REF_RT {
    int             Present;
    unsigned long   Guard, Lead, Adc, FrameCfg;
} REF_RT;

static void RefRt(const REF_RT *In, APOLLO_AGG_UNIT_RT *Out)
{
    int u;

    memset(Out, 0, sizeof(APOLLO_AGG_UNIT_RT) * APOLLO_AGG_MAX_UNITS);
    for (u = 0; u < APOLLO_AGG_MAX_UNITS; u++) {
        Out[u].Present = In[u].Present;
        Out[u].Guard = In[u].Guard;
        Out[u].Lead = In[u].Lead;
        Out[u].Adc = In[u].Adc;
        Out[u].FrameCfg = In[u].FrameCfg;
    }
}

unsigned long RefG6CompiledBytes(void)
{
    return (unsigned long)sizeof(APOLLO_AGG_COMPILED);
}

/* ApolloAggCompile of G6 into Out (RefG6CompiledBytes() bytes). */
void RefG6Compile(const APOLLO_AGG_CONFIG *C, const void *Rt, unsigned long Timeline, void *Out)
{
    APOLLO_AGG_UNIT_RT rt[APOLLO_AGG_MAX_UNITS];

    RefRt((const REF_RT *)Rt, rt);
    ApolloAggCompile(C, rt, Timeline, (APOLLO_AGG_COMPILED *)Out);
}

unsigned long RefG6Pad(const APOLLO_AGG_CONFIG *C, const void *Rt, unsigned long Timeline)
{
    APOLLO_AGG_UNIT_RT rt[APOLLO_AGG_MAX_UNITS];

    RefRt((const REF_RT *)Rt, rt);
    return ApolloAggPad(C, rt, Timeline);
}

/* G6's follower capture offset: agg.c set DAdc = Adc(u) - Adc(timeline) every tick. */
long RefG6DAdc(const void *Rt, unsigned long Timeline, unsigned long U)
{
    const REF_RT *r = (const REF_RT *)Rt;

    return (long)r[U].Adc - (long)r[Timeline].Adc;
}
