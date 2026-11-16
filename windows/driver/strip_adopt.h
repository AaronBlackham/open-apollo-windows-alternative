/*
 * strip_adopt.h - warm starts: adopting this driver's own input strips (docs/STRIPS.md 12).
 *
 * After a driver update, a PC restart or a device restart the Apollo stays powered, so the
 * driver comes up WARM. On the x8p the warm path never ring-connects, so the DSPs still run
 * whatever the last host left there. When that host was this driver and its D0 exit left its
 * strips running, they are stopped and built again (StripWarmRebuild, default 1), or, with
 * StripWarmRebuild=0, adopted: marked Live without sending anything. Adoption is not proven to
 * pass audio (StripAdoptDecide).
 *
 * What proves "ours" (nothing is written to the DSP for it, so nothing can collide with what
 * UA's software or the firmware keep there): a record in the registry, written by the driver
 * at the moments it knows the strip DSP's state, sealed with an FNV-1a check word:
 *   - the unit (HW_UID, device type, FPGA and firmware release, DSP count);
 *   - every DSP's boot word right after this driver's last ring connect. A host's connect
 *     changes it (low byte 0x0B and a connect count in bits 20:19: x8p 0008220b/0008000b after
 *     our one cold connect, Nov 15 log; Twin X 0008270b), so equal words at the next start mean
 *     nobody connected since;
 *   - Anchored: every host session since the unit's last power-up was this driver's. Set at a
 *     cold start whose boot words all read 3 (no host since power-up), kept while each later
 *     start finds the words unchanged and (when it connects) sees its own connect move them,
 *     lost for good (until a power cycle) on any mismatch, taint, quarantine or unproven
 *     teardown;
 *   - the strips: State (RUNNING / NONE / UNKNOWN), whether the D0 exit keeps them (Keep), and
 *     the exact plan: strip DSP, StripMask, rate, stage, the five module pins (hash), the pool
 *     registers, every header, and an FNV-1a hash of every word of every transaction the pass
 *     sends (strip_plan.h);
 *   - Plugins: plug-in instances placed on the strip DSP when it was last written (written when
 *     one is placed, and only after a proven teardown when one goes).
 * The decision (StripAdoptDecide) is plain C: tools\strip-plan-test runs its whole table.
 *
 * Twin X: its warm start ring-connects (spec-twinx 2: skipping it leaves playback silent), and a
 * connect makes the DSPs drop the strips (oa:CHANGELOG.md:50-52), so nothing can be adopted
 * there. With the same proof it gets a warm REBUILD instead (StripWarmRebuild, default on for
 * the Twin X), the same pass as after a cold start, sent after its own connect.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

/* strip_plan.h first: StripPlanHash and the boot-word checks use it (it has #pragma once). */
#include "strip_plan.h"
#include "strip_record.h"

#define STRIP_REC_MODULES       5u              /* = STRIP_MODULE_IDS = APOLLO_STRIP_MODULES */

/* ---- FNV-1a, 32 bit, a word at a time (little-endian bytes) ---- */

#define STRIP_FNV_INIT          0x811C9DC5u

static __inline ULONG StripFnv(ULONG H, ULONG V)
{
    ULONG i;

    for (i = 0; i < 4; i++) {
        H ^= (V >> (8 * i)) & 0xFFu;
        H *= 0x01000193u;
    }
    return H;
}

static __inline ULONG StripFnvWords(ULONG H, const ULONG *W, ULONG N)
{
    ULONG i;

    for (i = 0; i < N; i++) {
        H = StripFnv(H, W[i]);
    }
    return H;
}

/* The check word: every word before Check. */
static __inline ULONG StripRecordSum(const STRIP_RECORD *R)
{
    return StripFnvWords(STRIP_FNV_INIT, (const ULONG *)R,
                         (ULONG)((sizeof(*R) - sizeof(R->Check)) / sizeof(ULONG)));
}

/*
 * [Virtual] The version a record is sealed as: 2 when its plan mask names Virtual strips, else 1 (strip_record.h).
 * [AuxStrips] 3 when it names aux-return strips (bits 16-19), whatever else it names. [TalkStrip] 4 when it names the
 * talkback strip (bit 20), whatever else it names.
 */
static __inline ULONG StripRecordVersionOf(const STRIP_RECORD *R)
{
    if (((R->Mask >> 20) & 1u) != 0) {
        return STRIP_REC_VERSION_TALKBACK;
    }
    if (((R->Mask >> 16) & 0xFu) != 0) {
        return STRIP_REC_VERSION_AUX;
    }
    return ((R->Mask >> 8) & 0xFFu) != 0 ? STRIP_REC_VERSION_VIRTUAL : STRIP_REC_VERSION;
}

static __inline void StripRecordSeal(STRIP_RECORD *R)
{
    R->Magic = STRIP_REC_MAGIC;
    R->Version = StripRecordVersionOf(R);
    R->Size = (ULONG)sizeof(*R);
    R->Check = StripRecordSum(R);
}

/*
 * Bytes = what the registry value held. 1 = a record this build wrote and nobody changed. [Virtual] Version 1 (no
 * Virtual strips, every earlier build's) or 2 (Virtual strips in the plan mask), and the one its mask asks for.
 * [AuxStrips] Or 3 (aux-return strips, plan mask bits 16-19). [TalkStrip] Or 4 (the talkback strip, bit 20); nothing past
 * bit 20.
 */
static __inline int StripRecordValid(const STRIP_RECORD *R, ULONG Bytes)
{
    return Bytes == sizeof(*R) && R->Magic == STRIP_REC_MAGIC && R->Version == StripRecordVersionOf(R) &&
           (R->Mask >> 21) == 0 && R->Size == sizeof(*R) && R->Check == StripRecordSum(R) &&
           R->NumDsps <= STRIP_REC_DSPS && R->State <= STRIP_REC_UNKNOWN;
}

/* ---- The plan's identity (strip_plan.h must be included first) ---- */

/* The words a plan hash starts with. */
static __inline ULONG StripPlanHashBegin(ULONG Mask, ULONG Limit, ULONG Rate, ULONG Dsp)
{
    return StripFnv(StripFnv(StripFnv(StripFnv(STRIP_FNV_INIT, Mask), Limit), Rate), Dsp);
}

/* One transaction of the pass into the hash: its kind, input (slot), up to MaxEnts entries and the payload. */
static __inline ULONG StripPlanHashXact(ULONG H, ULONG Kind, ULONG In, const STRIP_PLAN *Plan, ULONG MaxEnts)
{
    ULONG j;

    H = StripFnv(H, Kind);
    H = StripFnv(H, In);
    H = StripFnv(H, Plan->N);
    for (j = 0; j < Plan->N && j < MaxEnts; j++) {
        H = StripFnv(H, Plan->Ent[j].Dma);
        H = StripFnvWords(H, Plan->Ent[j].W, 4);
        H = StripFnv(H, Plan->Ent[j].Off);
        H = StripFnv(H, Plan->Ent[j].Dwords);
    }
    return StripFnvWords(H, Plan->Payload, Plan->Len <= STRIP_PLAN_PAYLOAD_MAX ? Plan->Len : 0);
}

/* The end of a plan hash: the DSPs that get the route word (stage 8 on); a real hash of 0 is mapped to 1. */
static __inline ULONG StripPlanHashEnd(ULONG H, ULONG Limit, ULONG Dsp, ULONG NumDsps)
{
    if (StripPlanSteps(Limit) >= STRIP_STEP_OTHERS) {
        H = StripFnv(H, StripRouteDsps(NumDsps, Dsp));
        H = StripFnv(H, STRIP_CMD_INPUT);
        H = StripFnv(H, STRIP_INPUT_ROUTE);
        H = StripFnv(H, STRIP_ROUTE_NONE);
    }
    return H ? H : 1u;
}

/*
 * Every word the pass for Mask at stage Limit and Rate sends on the strip DSP, in order (each
 * PREPARE, then the START), plus the DSPs that get the route word: one FNV-1a. Plan = scratch.
 * 0 on a planner error (a real hash of 0 is mapped to 1).
 */
static __inline ULONG StripPlanHash(ULONG Mask, ULONG Limit, ULONG Rate, ULONG Dsp, ULONG NumDsps, STRIP_PLAN *Plan)
{
    ULONG h = StripPlanHashBegin(Mask, Limit, Rate, Dsp), k, kind, in, rc;

    for (k = 0; k <= STRIP_PLAN_INPUTS; k++) {
        kind = StripPassStep(Mask, Limit, k, &in);
        if (kind == STRIP_XACT_END) {
            break;
        }
        rc = (kind == STRIP_XACT_PREPARE) ? StripPlanPrepareRate(in, Limit, Rate, Plan) : StripPlanStart(Mask, Limit, Plan);
        if (rc != 0 || Plan->Err != 0) {
            return 0;
        }
        h = StripPlanHashXact(h, kind, in, Plan, STRIP_PLAN_ENTS_MAX);
    }
    return StripPlanHashEnd(h, Limit, Dsp, NumDsps);
}

/*
 * [Virtual] The same identity for a plan mask Pm (StripMask | StripVirtualMask << 8, strip_plan.h StripPlanPm) with the
 * model's MicSlots (where the Virtual strips sit): every Mic and Virtual PREPARE, then the START, word for word. For a
 * Pm of 0..0xFF it is StripPlanHash's value, so a record any earlier build wrote matches a start without Virtual
 * strips exactly as before (strip-plan-test). [AuxStrips] [TalkStrip] The aux-return and talkback slots the same way; a
 * Pm without bit 20 hashes as the aux build did. 0 on a planner error.
 */
static __inline ULONG StripPlanHashPm(ULONG Pm, ULONG MicSlots, ULONG Limit, ULONG Rate, ULONG Dsp, ULONG NumDsps,
                                      STRIP_PLAN *Plan)
{
    ULONG h = StripPlanHashBegin(Pm, Limit, Rate, Dsp), k, kind, in, rc;

    for (k = 0; k <= STRIP_PLAN_SLOTS; k++) {
        kind = StripPassStepPm(Pm, Limit, k, &in);
        if (kind == STRIP_XACT_END) {
            break;
        }
        rc = (kind == STRIP_XACT_PREPARE) ? StripPlanPrepareSlot(in, MicSlots, Limit, Rate, Plan) :
                                            StripPlanStartPm(Pm, MicSlots, Limit, Plan);
        if (rc != 0 || Plan->Err != 0) {
            return 0;
        }
        h = StripPlanHashXact(h, kind, in, Plan, STRIP_PLAN_ENT_SLOTS);
    }
    return StripPlanHashEnd(h, Limit, Dsp, NumDsps);
}

/* Header[i] of every input in Mask (0 elsewhere): the address its SYNCH points at. */
static __inline void StripPlanHeaders(ULONG Mask, ULONG Header[STRIP_REC_INPUTS])
{
    ULONG a[STRIP_PLAN_ROWS], i;

    for (i = 0; i < STRIP_REC_INPUTS; i++) {
        Header[i] = 0;
        if (i < STRIP_PLAN_INPUTS && (Mask & (1u << i))) {
            StripPlanAddrs(i, a);
            Header[i] = a[0];
        }
    }
}

/* ---- Boot words ---- */

/* Cold start: every DSP's boot word says no host since power-up (strip_plan.h StripOwned). */
static __inline int StripBootAllUntouched(const ULONG *Pre, ULONG N)
{
    ULONG d;

    if (N == 0 || N > STRIP_REC_DSPS) {
        return 0;
    }
    for (d = 0; d < N; d++) {
        if (!StripOwned(Pre[d])) {
            return 0;
        }
    }
    return 1;
}

/*
 * The part of a boot word that says who connected: the connect count (bits 20:19) and the model byte.
 * The low byte is the session state: 0x0B while a host is connected, 0x07 after this driver's own D0 exit
 * (transport stop + ring resync; seen on both units Nov 15 2026: x8p 0008220b at bring-up, 00082207 at
 * the next start). It moves at our own exit, so it proves nothing about other hosts; a foreign connect
 * moves the count.
 */
#define STRIP_BOOT_SESSION_MASK     0xFFFFFF00u

static __inline int StripBootSame(const ULONG *A, const ULONG *B, ULONG N)
{
    ULONG d;

    if (N == 0 || N > STRIP_REC_DSPS) {
        return 0;
    }
    for (d = 0; d < N; d++) {
        if ((A[d] & STRIP_BOOT_SESSION_MASK) != (B[d] & STRIP_BOOT_SESSION_MASK)) {
            return 0;
        }
    }
    return 1;
}

/*
 * Our own connect moved every DSP's word (the counter still counts): otherwise a later foreign
 * connect could leave the same words, and the words prove nothing from here on. The session byte
 * alone moving (0x07 -> 0x0B) is not a count.
 */
static __inline int StripBootAdvanced(const ULONG *Pre, const ULONG *Post, ULONG N)
{
    ULONG d;

    if (N == 0 || N > STRIP_REC_DSPS) {
        return 0;
    }
    for (d = 0; d < N; d++) {
        if ((Pre[d] & STRIP_BOOT_SESSION_MASK) == (Post[d] & STRIP_BOOT_SESSION_MASK) ||
            Post[d] == 0xFFFFFFFFu || Post[d] == 0) {
            return 0;
        }
    }
    return 1;
}

/* ---- The unit ---- */

typedef struct _STRIP_UNIT_ID {
    ULONG   HwUidLo, HwUidHi, DeviceType, FpgaRev, FwRelease, NumDsps;
} STRIP_UNIT_ID;

static __inline int StripRecordSameUnit(const STRIP_RECORD *R, const STRIP_UNIT_ID *U)
{
    return R->HwUidLo == U->HwUidLo && R->HwUidHi == U->HwUidHi && R->DeviceType == U->DeviceType &&
           R->FpgaRev == U->FpgaRev && R->FwRelease == U->FwRelease && R->NumDsps == U->NumDsps;
}

/* ---- The anchor, after this start's bring-up ---- */

#define STRIP_ANCHOR_OK             0u  /* anchored */
#define STRIP_ANCHOR_COLD_USED      1u  /* cold start, but a DSP's boot word says a host used it since power-up */
#define STRIP_ANCHOR_NO_RECORD      2u  /* warm start, no record: built by a driver without records, or never */
#define STRIP_ANCHOR_BAD_RECORD     3u  /* warm start, a record this build cannot trust */
#define STRIP_ANCHOR_OTHER_UNIT     4u  /* the record is another unit's, or another firmware's */
#define STRIP_ANCHOR_LOST_BEFORE    5u  /* the record was no longer anchored */
#define STRIP_ANCHOR_OTHER_HOST     6u  /* the boot words changed since this driver's last connect */
#define STRIP_ANCHOR_NO_ADVANCE     7u  /* this start's own connect did not move the boot words */
#define STRIP_ANCHOR_NOT_READ       8u  /* boot words not read (no unit key, no DSPs) */

/*
 * Cold: this start reset the DSPs' host state (no running mixer). Connected: this start sent the
 * ring connect (x8p cold, every Twin X start). Pre: boot words before that connect; Post: after
 * the bring-up. Rec: the record found at this start (RecOk: valid and the same unit).
 */
static __inline ULONG StripAnchor(int Cold, int Connected, const ULONG *Pre, const ULONG *Post, ULONG N,
                                  const STRIP_RECORD *Rec, ULONG RecWhy)
{
    if (N == 0 || N > STRIP_REC_DSPS) {
        return STRIP_ANCHOR_NOT_READ;
    }
    if (Cold) {
        if (!StripBootAllUntouched(Pre, N)) {
            return STRIP_ANCHOR_COLD_USED;
        }
    } else {
        if (RecWhy != STRIP_ANCHOR_OK || Rec == 0) {
            return RecWhy != STRIP_ANCHOR_OK ? RecWhy : STRIP_ANCHOR_NO_RECORD;
        }
        if (!Rec->Anchored) {
            return STRIP_ANCHOR_LOST_BEFORE;
        }
        if (!StripBootSame(Pre, Rec->BootWord, N)) {
            return STRIP_ANCHOR_OTHER_HOST;
        }
    }
    if (Connected && !StripBootAdvanced(Pre, Post, N)) {
        return STRIP_ANCHOR_NO_ADVANCE;
    }
    return STRIP_ANCHOR_OK;
}

/* ---- The record across a start ---- */

/* The strips part back to nothing of ours (State NONE) or not known (UNKNOWN). Plugins is kept. */
static __inline void StripRecordClearStrips(STRIP_RECORD *R, ULONG State)
{
    ULONG i;

    R->State = State;
    R->Keep = 0;
    R->Dsp = R->Mask = R->Rate = R->Limit = 0;
    R->ModuleHash = R->PlanHash = 0;
    for (i = 0; i < STRIP_REC_INPUTS; i++) {
        R->Header[i] = 0;
    }
    for (i = 0; i < 11; i++) {
        R->Geom[i] = 0;
    }
    R->BuiltDate = 0;
    R->Adoptions = 0;
}

/*
 * The record after this start's bring-up (strips.c StripsRecRefresh): the boot words read now (after any
 * connect of this start), the anchor, and what the strips part says now: a cold start holds nothing of
 * ours; a warm start without a record holds something unknown; a warm start that connected (the Twin X)
 * dropped what ran; an x8p warm start with its record keeps the record's strips part as it was.
 * Rec = the record found (valid, this unit), or 0. Writes and Check are left for the writer.
 */
static __inline void StripRecordAfterBringup(int Cold, int Connected, const STRIP_RECORD *Rec, ULONG Anchor,
                                             const ULONG *Post, ULONG N, STRIP_RECORD *Next)
{
    ULONG d;
    const unsigned char *src = (const unsigned char *)Rec;
    unsigned char *dst = (unsigned char *)Next;

    for (d = 0; d < sizeof(*Next); d++) {
        dst[d] = Rec ? src[d] : 0;
    }
    Next->Anchored = (Anchor == STRIP_ANCHOR_OK) ? 1u : 0u;
    for (d = 0; d < STRIP_REC_DSPS; d++) {
        Next->BootWord[d] = (d < N) ? Post[d] : 0;
    }
    if (Cold) {
        StripRecordClearStrips(Next, STRIP_REC_NONE);
        Next->Plugins = 0;
    } else if (Rec == 0) {
        StripRecordClearStrips(Next, STRIP_REC_UNKNOWN);
    } else if (Connected) {
        StripRecordClearStrips(Next, STRIP_REC_NONE);
    }
}

/* The same record apart from the write counter and the check word: nothing to write. */
static __inline int StripRecordSameContent(const STRIP_RECORD *A, const STRIP_RECORD *B)
{
    const ULONG *a = (const ULONG *)A, *b = (const ULONG *)B;
    ULONG i, n = (ULONG)(sizeof(*A) / sizeof(ULONG));
    ULONG wr = (ULONG)(((const unsigned char *)&A->Writes - (const unsigned char *)A) / sizeof(ULONG));
    ULONG ck = (ULONG)(((const unsigned char *)&A->Check - (const unsigned char *)A) / sizeof(ULONG));

    for (i = 0; i < n; i++) {
        if (i != wr && i != ck && a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/* ---- The decision at a warm start ---- */

#define STRIP_ADOPT_NONE            0u  /* nothing decided (a cold start, strips off) */
#define STRIP_ADOPT_ADOPTED         1u  /* this driver's strips run as planned: adopted, nothing sent */
#define STRIP_ADOPT_RESTORE         2u  /* ... but a plug-in chain may sit on them: stop + start each (inline) */
#define STRIP_ADOPT_REBUILD         3u  /* ours since the last power-up: built again, no power cycle */
#define STRIP_ADOPT_NOT_ANCHORED    4u  /* see the anchor reason */
#define STRIP_ADOPT_NOT_RUNNING     5u  /* nothing of ours runs there, and warm rebuilds are off */
#define STRIP_ADOPT_UNKNOWN_STATE   6u  /* the last session ended in a pass or an unproven step; rebuilds off */
#define STRIP_ADOPT_TORN_DOWN       7u  /* the last exit was told to tear them down; rebuilds off */
#define STRIP_ADOPT_PLAN_DIFFERS    8u  /* they run, but not the plan this start asks for; rebuilds off */
#define STRIP_ADOPT_POOLS_DIFFER    9u  /* the strip DSP's pool registers are not the recorded ones */
#define STRIP_ADOPT_DISABLED        10u /* StripAdopt is 0 */
#define STRIP_ADOPT_DROPPED         11u /* the model's warm connect dropped them, and warm rebuilds are off */
#define STRIP_ADOPT_LAYOUT          12u /* the pools do not fit the recipe: nothing is ever sent */
#define STRIP_ADOPT_COUNT           13u

typedef struct _STRIP_ADOPT_NOW {
    ULONG   Anchor;             /* StripAnchor for this start */
    ULONG   ConnectsOnWarm;     /* the model ring-connects on a warm start (the Twin X) */
    ULONG   Adopt;              /* StripAdopt */
    ULONG   WarmRebuild;        /* StripWarmRebuild */
    ULONG   Dsp, Mask, Rate, Limit;
    ULONG   ModuleHash, PlanHash;
    ULONG   LayoutOk;           /* the pool registers fit the recipe (StripPlanLayoutCheck 0) */
    ULONG   Geom[11];
} STRIP_ADOPT_NOW;

static __inline int StripRecordPlanSame(const STRIP_RECORD *R, const STRIP_ADOPT_NOW *N)
{
    return R->Dsp == N->Dsp && R->Mask == N->Mask && R->Rate == N->Rate && R->Limit == N->Limit &&
           R->ModuleHash == N->ModuleHash && R->PlanHash == N->PlanHash && N->PlanHash != 0;
}

static __inline int StripRecordGeomSame(const STRIP_RECORD *R, const STRIP_ADOPT_NOW *N)
{
    ULONG i;

    for (i = 0; i < 11; i++) {
        if (R->Geom[i] != N->Geom[i]) {
            return 0;
        }
    }
    return 1;
}

/*
 * A warm start, the record found at it (R: valid and the same unit whenever Now->Anchor is OK).
 * *Teardown = the strips of the record still run and must be stopped (and their modules released)
 * before a rebuild writes their memory again.
 */
static __inline ULONG StripAdoptDecide(const STRIP_RECORD *R, const STRIP_ADOPT_NOW *Now, int *Teardown)
{
    int rebuild = Now->WarmRebuild != 0;

    *Teardown = 0;
    if (!Now->LayoutOk) {
        return STRIP_ADOPT_LAYOUT;
    }
    if (Now->Anchor != STRIP_ANCHOR_OK || R == 0) {
        return STRIP_ADOPT_NOT_ANCHORED;
    }
    if (Now->ConnectsOnWarm) {
        /* the connect at this start dropped whatever ran: build again, or nothing */
        return rebuild ? STRIP_ADOPT_REBUILD : STRIP_ADOPT_DROPPED;
    }
    if (R->State == STRIP_REC_NONE) {
        return rebuild ? STRIP_ADOPT_REBUILD : STRIP_ADOPT_NOT_RUNNING;
    }
    if (R->State == STRIP_REC_UNKNOWN) {
        *Teardown = rebuild;
        return rebuild ? STRIP_ADOPT_REBUILD : STRIP_ADOPT_UNKNOWN_STATE;
    }
    /* RUNNING */
    if (!StripRecordGeomSame(R, Now)) {
        return STRIP_ADOPT_POOLS_DIFFER;
    }
    if (!R->Keep) {
        *Teardown = rebuild;
        return rebuild ? STRIP_ADOPT_REBUILD : STRIP_ADOPT_TORN_DOWN;
    }
    if (!StripRecordPlanSame(R, Now)) {
        *Teardown = rebuild;
        return rebuild ? STRIP_ADOPT_REBUILD : STRIP_ADOPT_PLAN_DIFFERS;
    }
    /*
     * The same plan, kept at exit: rebuild whenever rebuilds are allowed. Adopted strips were never heard
     * passing audio: on Nov 16 2026 an x8p that adopted its strips after a warm restart recorded exact zero
     * on every input (the monitor mix still played) until a power cycle. The warm rebuild (stop, then the
     * pass again) is the path proven on hardware (VIRTUAL 1 recorded at -30 dBFS right after one), so it
     * wins; adoption stays for StripWarmRebuild=0.
     */
    if (rebuild) {
        *Teardown = 1;
        return STRIP_ADOPT_REBUILD;
    }
    if (!Now->Adopt) {
        return STRIP_ADOPT_DISABLED;
    }
    /* a chain counted there: put the plain strips back; only the stale mark (already restored): adopted as they are */
    return (R->Plugins & ~STRIP_REC_PLUGINS_STALE) != 0 ? STRIP_ADOPT_RESTORE : STRIP_ADOPT_ADOPTED;
}

/*
 * [Warm] docs/STRIPS.md 12.5: may the plug-in host place on the strip DSP after this warm start's verdict? Only strips
 * this start adopted (no ring connect: the x8p) or rebuilt (the Twin X; also needs no plug-in counted since), from a
 * record that is anchored (the verdict needs it) and saw no plug-in left there, with StripWarmPlugins on. RESTORE (a
 * chain may have sat on the strips) keeps them off until a power cycle: its modules may still be resident.
 */
static __inline int StripWarmPluginsOk(ULONG Verdict, ULONG RecPlugins, ULONG WarmPlugins)
{
    return (Verdict == STRIP_ADOPT_ADOPTED || Verdict == STRIP_ADOPT_REBUILD) && RecPlugins == 0 && WarmPlugins != 0;
}

static __inline const char *StripAnchorText(ULONG A)
{
    switch (A) {
    case STRIP_ANCHOR_OK:           return "only this driver used the DSPs since the last power-up";
    case STRIP_ANCHOR_COLD_USED:    return "cold start, but a host used a DSP since power-up";
    case STRIP_ANCHOR_NO_RECORD:    return "no record: these strips were built by a driver without adoption records, or never";
    case STRIP_ANCHOR_BAD_RECORD:   return "the record is damaged or from another version";
    case STRIP_ANCHOR_OTHER_UNIT:   return "the record is another unit's or another firmware's";
    case STRIP_ANCHOR_LOST_BEFORE:  return "an earlier start already lost track of the DSPs";
    case STRIP_ANCHOR_OTHER_HOST:   return "the boot words changed: another host connected since this driver's last start";
    case STRIP_ANCHOR_NO_ADVANCE:   return "this start's own connect did not move the boot words (counter saturated)";
    case STRIP_ANCHOR_NOT_READ:     return "boot words not read, or no unit key to keep a record under";
    default:                        return "?";
    }
}

static __inline const char *StripAdoptText(ULONG V)
{
    switch (V) {
    case STRIP_ADOPT_NONE:          return "not a warm start";
    case STRIP_ADOPT_ADOPTED:       return "adopted";
    case STRIP_ADOPT_RESTORE:       return "adopted, plain strips restored";
    case STRIP_ADOPT_REBUILD:       return "rebuilt at a warm start";
    case STRIP_ADOPT_NOT_ANCHORED:  return "not proven to be this driver's";
    case STRIP_ADOPT_NOT_RUNNING:   return "none of this driver's strips run there (StripWarmRebuild is 0)";
    case STRIP_ADOPT_UNKNOWN_STATE: return "the last session ended mid-pass (StripWarmRebuild is 0)";
    case STRIP_ADOPT_TORN_DOWN:     return "the last exit tore them down (StripWarmRebuild is 0)";
    case STRIP_ADOPT_PLAN_DIFFERS:  return "the running strips are not this start's plan (StripWarmRebuild is 0)";
    case STRIP_ADOPT_POOLS_DIFFER:  return "the strip DSP's pools are not the recorded ones";
    case STRIP_ADOPT_DISABLED:      return "StripAdopt is 0";
    case STRIP_ADOPT_DROPPED:       return "the warm ring connect dropped them (StripWarmRebuild is 0)";
    case STRIP_ADOPT_LAYOUT:        return "the strip DSP's pools do not fit the recipe";
    default:                        return "?";
    }
}
