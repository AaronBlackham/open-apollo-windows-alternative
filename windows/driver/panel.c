/*
 * panel.c - [Clock] the front panel of each unit (the big knob, its MONITOR/PREAMP mode, mute, dim, mono, ALT,
 * talkback) and the monitor link: GET_PANEL / SET_PANEL (include\apollo_clock.h). Decoding, arithmetic and the
 * evidence are in clock_plan.h.
 *
 * How the knob and the buttons arrive: no notification bit and no event. The unit acts on them itself (the Twin X's
 * knob in MONITOR mode moves its own monitor level, S2 [7:0]; a press mutes it) and its readback words follow; the
 * 10 ms service pass reads the readback whenever the DSP posts one (mixer.c) and ApolloPanelServicePass compares it
 * with the last one, as open-apollo's mixer daemon does at 50 Hz (oa:mixer-engine/ua_mixer_daemon.py
 * _sync_hw_readback). Each change becomes an event with who made it: a value a host write is expected to bring back
 * within 1.5 s is the host's (ApolloPanelNoteWrite, from SET_MONITOR_PARAM), a value the link wrote is the link's,
 * anything else is the panel's (the knob, a button).
 *
 * The monitor link (driver-wide, off at every load, SET_PANEL turns it on): one unit is the monitor unit (the one
 * with the speakers); the knob and buttons of every other running unit drive it, as UA's desk-remote behaviour
 * ("the Twin X knob mirrors system monitor volume/mute", docs/console-parity.md). Only the knob unit's own changes
 * travel (a host write or a mirrored value never does: no loops). The level travels as steps (the knob's raw delta,
 * 0.5 dB each) added to the monitor unit's level, so the two units' knobs never have to agree on a position; mute,
 * dim and mono as values. Talkback on any unit (its button, or a host write) dims the monitor unit and undims what it
 * dimmed (TALK_DIM); on the monitor unit itself that happens in its own pass. The writes are ordinary
 * SET_MONITOR_PARAM encodings (monitor_plan.h: level, mute, dim, mono, proven on the x8p and on the Twin X's
 * monitor section), into the monitor unit's mailbox under ITS DevLock, which the knob unit's service thread takes
 * without waiting (busy: the change waits for the next pass) while it holds that unit's member rundown (agg.c
 * ApolloAggUnitRef). So no thread ever holds two DevLocks, and nothing waits on another unit.
 *
 * Talkback: the button is readback word 6 bit 8 (and SET_MONITOR_PARAM 0x46). Its mic records on the TALKBACK 1/2
 * record slots (0x0B01/0x0B02): the Twin X runs AX_REC_CH 34 from its descriptor, so they are slots 32/33 of its frame;
 * the x8p's legacy frame is 32 wide and has none. The mic carries silence on both until a talkback input strip
 * (module 0xC9) runs on its DSP input, and no recipe for it is known (spec-twinx 5.2, oa:driver/ua_strips.c:27-31):
 * GET_PANEL says so (TalkStrip 0). Its sends into the cues are the cue sends of bus 0x0A (monitor_plan.h); UA's engine
 * writes only 0x46 on TALK, so the unit itself is taken to gate the talkback bus (unverified until the mic is live).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "apollo.h"
#include <ntstrsafe.h>

C_ASSERT(APOLLO_PANEL_LEVEL == APOLLO_PANEL_F_LEVEL && APOLLO_PANEL_HP == APOLLO_PANEL_F_HP &&
         APOLLO_PANEL_MUTE == APOLLO_PANEL_F_MUTE && APOLLO_PANEL_MONO == APOLLO_PANEL_F_MONO &&
         APOLLO_PANEL_DIM == APOLLO_PANEL_F_DIM && APOLLO_PANEL_ALT == APOLLO_PANEL_F_ALT &&
         APOLLO_PANEL_TALK == APOLLO_PANEL_F_TALK && APOLLO_PANEL_FOCUS == APOLLO_PANEL_F_KNOB &&
         APOLLO_PANEL_PREAMP == APOLLO_PANEL_F_PREAMP && APOLLO_PANEL_LOCK == APOLLO_PANEL_F_LOCK);
C_ASSERT(APOLLO_PANEL_ORIGIN_PANEL == APOLLO_PANEL_FROM_PANEL && APOLLO_PANEL_ORIGIN_HOST == APOLLO_PANEL_FROM_HOST &&
         APOLLO_PANEL_ORIGIN_LINK == APOLLO_PANEL_FROM_LINK && APOLLO_PANEL_ORIGIN_UNIT == APOLLO_PANEL_FROM_UNIT);
C_ASSERT(APOLLO_LINK_LEVEL == APOLLO_PANEL_LINK_LEVEL && APOLLO_LINK_MUTE == APOLLO_PANEL_LINK_MUTE &&
         APOLLO_LINK_DIM == APOLLO_PANEL_LINK_DIM && APOLLO_LINK_MONO == APOLLO_PANEL_LINK_MONO &&
         APOLLO_LINK_TALK_DIM == APOLLO_PANEL_LINK_TALK_DIM);
C_ASSERT(APOLLO_MON_LEVEL == 0x01 && APOLLO_MON_HP1_LEVEL == 0x02 && APOLLO_MON_MUTE == 0x03 && APOLLO_MON_MONO == 0x10 &&
         APOLLO_MON_DIM == 0x44 && APOLLO_MON_ALT_SELECT == 0x3A && APOLLO_MON_TALKBACK == 0x46);  /* ApolloPanelFieldOfMon */
C_ASSERT(APOLLO_CHAN_TALKBACK_1 == 0x0B01 && APOLLO_CHAN_TALKBACK_2 == 0x0B02);

#define PANEL_LINK_LEVEL_HOLD_MS    500ull      /* the link's own last level beats a readback that has not caught up */

/* The link (driver-wide). Not stored: off at every driver load until a host turns it on. */
static volatile LONG    g_LinkFlags;            /* APOLLO_LINK_* */
static volatile LONG64  g_LinkMonitor;          /* the monitor unit's HW_UID */
static volatile LONG    g_LinkApplied, g_LinkBusy, g_LinkDropped;

static ULONGLONG PanelUid(_In_ PDEVICE_CONTEXT Ctx)
{
    return ((ULONGLONG)Ctx->HwUidHi << 32) | Ctx->HwUidLo;
}

static ULONG PanelNowMs(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONGLONG ms = (KeQueryInterruptTime() - Ctx->Panel.T0) / 10000ULL;

    return (ULONG)(ms & 0xFFFFFFFFull);
}

static VOID PanelEvent(_Inout_ PDEVICE_CONTEXT Ctx, _In_ ULONG Field, _In_ ULONG Origin, _In_ LONG Value, _In_ ULONG NowMs)
{
    APOLLO_PANEL_EVENT *e = &Ctx->Panel.Ring[Ctx->Panel.Head % APOLLO_PANEL_EVENTS];

    e->Seq = ++Ctx->Panel.Seq;
    e->Field = (USHORT)Field;
    e->Origin = (USHORT)Origin;
    e->Value = Value;
    e->TimeMs = NowMs;
    Ctx->Panel.Head = (Ctx->Panel.Head + 1) % APOLLO_PANEL_EVENTS;
    if (Ctx->Panel.Count < APOLLO_PANEL_EVENTS) {
        Ctx->Panel.Count++;
    }
    ApolloEventPost(Ctx, APOLLO_EVENT_PANEL, Ctx->Panel.Seq, Field);          /* [Events] */
}

/* One monitor write by the driver itself (DevLock held): the plan filter, the encoding, its echo marked Origin. */
static NTSTATUS PanelWrite(_Inout_ PDEVICE_CONTEXT Ctx, _In_ ULONG Param, _In_ LONG Value, _In_ ULONG Origin)
{
    APOLLO_MONITOR_PARAM p;
    NTSTATUS st;

    if (!Ctx->MixerReady || !ApolloPlanMonitorParamAllowed(ApolloPlanOf(Ctx), Param)) {
        return STATUS_NOT_SUPPORTED;
    }
    p.Param = Param;
    p.Value = Value;
    Ctx->Panel.NextOrigin = Origin;
    st = ApolloSetMonitorParam(Ctx, &p);
    Ctx->Panel.NextOrigin = 0;
    return st;
}

_Use_decl_annotations_
VOID ApolloPanelConnected(PDEVICE_CONTEXT Ctx)
{
    APOLLO_PANEL_RT *p = &Ctx->Panel;

    p->Valid = FALSE;
    p->TalkDimmed = FALSE;
    p->RbSeq = Ctx->ReadbackSeq;
    RtlZeroMemory(p->Expect, sizeof(p->Expect));
    p->NextOrigin = 0;
    p->T0 = KeQueryInterruptTime();
    p->TalkOnMs = 0;
    p->PendLevel = 0;
    p->PendMask = 0;
    p->LinkLevelUntil = 0;
}

_Use_decl_annotations_
VOID ApolloPanelNoteWrite(PDEVICE_CONTEXT Ctx, ULONG Param, LONG Value)
{
    ULONG f = ApolloPanelFieldOfMon(Param);
    LONG v = Value;

    if (f == 0) {
        return;
    }
    if (f == APOLLO_PANEL_MUTE || f == APOLLO_PANEL_MONO || f == APOLLO_PANEL_DIM || f == APOLLO_PANEL_TALK) {
        v = (Value != 0) ? 1 : 0;
    }
    ApolloPanelExpect(Ctx->Panel.Expect, f, v,
                      Ctx->Panel.NextOrigin != 0 ? Ctx->Panel.NextOrigin : APOLLO_PANEL_ORIGIN_HOST, PanelNowMs(Ctx));
}

/*
 * The pass (DevLock), after the readback read: a new readback is decoded and compared with the last; every change is
 * an event; a knob unit's own changes are queued for the monitor unit (sent after the pass); the monitor unit's own
 * talkback dims it here.
 */
_Use_decl_annotations_
VOID ApolloPanelServicePass(PDEVICE_CONTEXT Ctx)
{
    APOLLO_PANEL_RT *p = &Ctx->Panel;
    APOLLO_PANEL_STATE n;
    ULONG mask, f, origin, now, flags, carry;
    ULONGLONG mon, me;
    BOOLEAN knobUnit, monitorUnit;
    LONG v;

    if (!Ctx->ReadbackValid || Ctx->ReadbackSeq == p->RbSeq) {
        return;
    }
    p->RbSeq = Ctx->ReadbackSeq;
    if (Ctx->Readback[38] == 0xFFFFFFFFu || Ctx->Readback[39] == 0xFFFFFFFFu) {
        return;                                 /* the Mixer Engine drops such a snapshot (CMxMixer::TimerCallback) */
    }
    if (!ApolloPanelDecode((const unsigned long *)Ctx->Readback, APOLLO_READBACK_WORDS, &n)) {
        return;
    }
    if (!p->Valid) {
        p->State = n;                           /* the first one only seeds: nothing changed yet */
        p->Valid = TRUE;
        return;
    }
    mask = ApolloPanelDiff(&p->State, &n);
    if (mask == 0) {
        return;
    }
    now = PanelNowMs(Ctx);
    flags = (ULONG)ReadNoFence(&g_LinkFlags);
    mon = (ULONGLONG)ReadNoFence64(&g_LinkMonitor);
    me = PanelUid(Ctx);
    knobUnit = (flags != 0 && mon != 0 && mon != me) ? TRUE : FALSE;
    monitorUnit = (flags != 0 && mon == me) ? TRUE : FALSE;
    if (mask & (1ul << APOLLO_PANEL_TALK)) {
        p->TalkOnMs = (now != 0) ? now : 1;     /* first: a dim in this same readback is already its echo */
    }
    for (f = 1; f < APOLLO_PANEL_FIELDS; f++) {
        if ((mask & (1ul << f)) == 0) {
            continue;
        }
        v = ApolloPanelValue(&n, f);
        origin = ApolloPanelClassify(p->Expect, f, v, now);
        if (f == APOLLO_PANEL_DIM && origin == APOLLO_PANEL_ORIGIN_PANEL && p->TalkOnMs != 0 &&
            now - p->TalkOnMs < APOLLO_PANEL_FW_DIM_MS) {
            /* the unit's own dim with talkback, on or off ("Talkback also sets the dim bit"): never sent on */
            origin = APOLLO_PANEL_ORIGIN_UNIT;
            p->UnitDims = TRUE;
        }
        PanelEvent(Ctx, f, origin, v, now);
        if (knobUnit) {
            carry = ApolloLinkCarries(flags, f, origin, ApolloPanelLinkFocus(Ctx->Ident.Model, n.Focus));
            if (carry != 0) {
                switch (f) {
                case APOLLO_PANEL_LEVEL:
                    p->PendLevel += (LONG)n.Level - (LONG)p->State.Level;
                    break;
                case APOLLO_PANEL_MUTE:
                    p->PendMute = (ULONG)v;
                    break;
                case APOLLO_PANEL_DIM:
                    p->PendDim = (ULONG)v;
                    break;
                case APOLLO_PANEL_MONO:
                    p->PendMono = (ULONG)v;
                    break;
                default:
                    p->PendTalk = (ULONG)v;
                    break;
                }
                p->PendMask |= 1ul << f;
            }
        } else if (monitorUnit && f == APOLLO_PANEL_TALK && (flags & APOLLO_LINK_TALK_DIM) != 0 &&
                   (origin == APOLLO_PANEL_ORIGIN_PANEL || origin == APOLLO_PANEL_ORIGIN_HOST)) {
            int dimmed = p->TalkDimmed ? 1 : 0;
            int d = ApolloLinkTalkDim((unsigned long)v, n.Dim, &dimmed);

            p->TalkDimmed = dimmed ? TRUE : FALSE;
            if (d >= 0) {
                ApolloLogU(Ctx, "panel: talkback %s: dim %s (link)", v ? "on" : "off", d ? "on" : "off");
                (VOID)PanelWrite(Ctx, APOLLO_MON_DIM, d, APOLLO_PANEL_ORIGIN_LINK);
            }
        }
    }
    p->State = n;
}

/*
 * After the pass, no lock held (the service thread): what this knob unit queued goes to the monitor unit, under that
 * unit's DevLock taken without waiting, its member rundown held. Busy: the queue stays for the next pass (the level
 * steps add up). The monitor unit not running: dropped.
 */
_Use_decl_annotations_
VOID ApolloPanelServiceAfter(PDEVICE_CONTEXT Ctx)
{
    APOLLO_PANEL_RT *p = &Ctx->Panel;
    ULONGLONG mon = (ULONGLONG)ReadNoFence64(&g_LinkMonitor), t;
    ULONG flags = (ULONG)ReadNoFence(&g_LinkFlags), cur, lvl;
    LONGLONG none = 0;
    PDEVICE_CONTEXT b;
    PVOID ref = NULL;

    if (p->PendMask == 0) {
        return;
    }
    if (flags == 0 || mon == 0 || mon == PanelUid(Ctx)) {
        p->PendMask = 0;
        p->PendLevel = 0;
        return;
    }
    b = ApolloAggUnitRef(mon, &ref);
    if (b == NULL) {
        InterlockedIncrement(&g_LinkDropped);
        p->PendMask = 0;
        p->PendLevel = 0;
        return;
    }
    if (WdfWaitLockAcquire(b->DevLock, &none) != STATUS_SUCCESS) {
        InterlockedIncrement(&g_LinkBusy);
        ApolloAggUnitUnref(ref);
        return;
    }
    if (!b->Connected || !b->MixerReady || !b->Panel.Valid || ApolloGateClosed(b)) {
        InterlockedIncrement(&g_LinkDropped);
    } else {
        t = KeQueryInterruptTime();
        if ((p->PendMask & (1ul << APOLLO_PANEL_LEVEL)) != 0 && p->PendLevel != 0) {
            cur = (b->Panel.LinkLevelUntil > t) ? b->Panel.LinkLevel : b->Panel.State.Level;
            lvl = ApolloLinkLevel(cur, p->PendLevel);
            if (lvl != cur && NT_SUCCESS(PanelWrite(b, APOLLO_MON_LEVEL, (LONG)lvl, APOLLO_PANEL_ORIGIN_LINK))) {
                b->Panel.LinkLevel = lvl;
                b->Panel.LinkLevelUntil = t + PANEL_LINK_LEVEL_HOLD_MS * 10000ull;
            }
        }
        if (p->PendMask & (1ul << APOLLO_PANEL_MUTE)) {
            (VOID)PanelWrite(b, APOLLO_MON_MUTE, (LONG)p->PendMute, APOLLO_PANEL_ORIGIN_LINK);
        }
        if (p->PendMask & (1ul << APOLLO_PANEL_DIM)) {
            (VOID)PanelWrite(b, APOLLO_MON_DIM, (LONG)p->PendDim, APOLLO_PANEL_ORIGIN_LINK);
        }
        if (p->PendMask & (1ul << APOLLO_PANEL_MONO)) {
            (VOID)PanelWrite(b, APOLLO_MON_MONO, (LONG)p->PendMono, APOLLO_PANEL_ORIGIN_LINK);
        }
        if (p->PendMask & (1ul << APOLLO_PANEL_TALK)) {
            int dimmed = b->Panel.TalkDimmed ? 1 : 0;
            int d = ApolloLinkTalkDim(p->PendTalk, b->Panel.State.Dim, &dimmed);

            b->Panel.TalkDimmed = dimmed ? TRUE : FALSE;
            if (d >= 0) {
                (VOID)PanelWrite(b, APOLLO_MON_DIM, d, APOLLO_PANEL_ORIGIN_LINK);
            }
        }
        InterlockedIncrement(&g_LinkApplied);
    }
    WdfWaitLockRelease(b->DevLock);
    ApolloAggUnitUnref(ref);
    p->PendMask = 0;
    p->PendLevel = 0;
}

/* ------------------------------------------------------------------ */
/* GET_PANEL / SET_PANEL (DevLock held, ioctl.c)                       */
/* ------------------------------------------------------------------ */

static VOID PanelFill(_In_ PDEVICE_CONTEXT Ctx, _Out_ PAPOLLO_PANEL_INFO I)
{
    const APOLLO_PANEL_RT *p = &Ctx->Panel;
    ULONGLONG mon = (ULONGLONG)ReadNoFence64(&g_LinkMonitor);
    ULONG flags = (ULONG)ReadNoFence(&g_LinkFlags), i, k;
    LONG s;

    RtlZeroMemory(I, sizeof(*I));
    I->Version = APOLLO_CLOCK_VERSION;
    I->Bytes = sizeof(*I);
    I->Model = (ULONG)Ctx->Ident.Model;
    I->ReadbackSeq = p->RbSeq;
    I->Seq = p->Seq;
    if (p->Valid) {
        I->Flags |= APOLLO_PANEL_INFO_VALID;
        I->Level = p->State.Level;
        I->HpLevel = p->State.HpLevel;
        I->Mute = p->State.Mute;
        I->Mono = p->State.Mono;
        I->Dim = p->State.Dim;
        I->Alt = p->State.Alt;
        I->Talk = p->State.Talk;
        I->Knob = p->State.Focus;
        I->Preamp = p->State.Preamp;
        I->Lock = p->State.Lock;
    } else {
        I->Lock = -1;
    }
    if (flags != 0 && mon != 0) {
        I->Flags |= (mon == PanelUid(Ctx)) ? APOLLO_PANEL_INFO_MONITOR_UNIT : APOLLO_PANEL_INFO_KNOB_UNIT;
    }
    if (p->TalkDimmed) {
        I->Flags |= APOLLO_PANEL_INFO_TALK_DIMMED;
    }
    if (p->UnitDims) {
        I->Flags |= APOLLO_PANEL_INFO_UNIT_DIMS;
    }
    if (p->PendMask != 0) {
        I->Flags |= APOLLO_PANEL_INFO_LINK_PENDING;
    }
    /* talkback: the descriptor's TALKBACK slots, if the frame (AX_REC_CH) reaches them */
    I->TalkRecChannels = Ctx->RecChannels;
    for (k = 0; k < 2; k++) {
        s = ApolloIoDescFind((const unsigned long *)Ctx->IoDescIn, 1, (unsigned short)(APOLLO_CHAN_TALKBACK_1 + k));
        I->TalkSlot[k] = (s >= 0 && (ULONG)s < Ctx->RecChannels) ? (ULONG)s : APOLLO_PANEL_NO_SLOT;
    }
    I->TalkStrip = 0;
    I->LinkFlags = flags;
    I->LinkMonitorUid = mon;
    I->LinkApplied = (ULONG)ReadNoFence(&g_LinkApplied);
    I->LinkBusy = (ULONG)ReadNoFence(&g_LinkBusy);
    I->LinkDropped = (ULONG)ReadNoFence(&g_LinkDropped);
    I->EventCount = p->Count;
    for (i = 0; i < p->Count && i < APOLLO_PANEL_EVENTS; i++) {
        I->Event[i] = p->Ring[(p->Head + APOLLO_PANEL_EVENTS - p->Count + i) % APOLLO_PANEL_EVENTS];
    }
}

_Use_decl_annotations_
NTSTATUS ApolloPanelIoctl(PDEVICE_CONTEXT Ctx, ULONG Code, PVOID In, size_t InLen, PVOID Out, size_t OutLen,
                          ULONG_PTR *Info)
{
    *Info = 0;
    if (Out == NULL || OutLen < sizeof(APOLLO_PANEL_INFO)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (Code == IOCTL_APOLLO_SET_PANEL) {
        APOLLO_PANEL_LINK l;

        if (In == NULL || InLen < sizeof(l)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        l = *(const APOLLO_PANEL_LINK *)In;     /* METHOD_BUFFERED: before the output overwrites it */
        if (l.Version != APOLLO_CLOCK_VERSION || (l.Flags & ~APOLLO_LINK_ALL) != 0 ||
            (l.Flags != 0 && l.MonitorUid == 0)) {
            return STATUS_INVALID_PARAMETER;
        }
        if (!Ctx->Unit.Active || Ctx->Unit.Value[APOLLO_PARAM_MODE] < APOLLO_MODE_FULL) {
            return STATUS_INVALID_DEVICE_STATE; /* a read-only unit changes nothing */
        }
        InterlockedExchange64(&g_LinkMonitor, (LONG64)(l.Flags != 0 ? l.MonitorUid : 0));
        InterlockedExchange(&g_LinkFlags, (LONG)l.Flags);
        ApolloLogU(Ctx, "panel: monitor link %s (flags 0x%02lx, monitor unit HW_UID 0x%016llx)",
                  l.Flags != 0 ? "on" : "off", l.Flags, l.MonitorUid);
    }
    PanelFill(Ctx, (PAPOLLO_PANEL_INFO)Out);
    *Info = sizeof(APOLLO_PANEL_INFO);
    return STATUS_SUCCESS;
}
