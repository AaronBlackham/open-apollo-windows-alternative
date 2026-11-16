/*
 * cues.c - [Monitor] the cue mixes' sends: per input (and the talkback mic) and cue, a level and a pan,
 * sent to the DSPs as bus coefficients {0x001D0004, bus, (side << 24) | param, float}
 * (docs/spec-dsp-mixer.md 2.8; driver\monitor_plan.h has the encoding and the evidence).
 *
 * Rules (oa:driver/ua_dsp.c:3790-3903, docs/STRIPS.md 2.8):
 *   - a send goes to the DSP that mixes its bus (the strip's DSP once the input strip runs: strips.c
 *     BusOwner) and also to DSP 0 when that is another DSP;
 *   - every send is cached, whether or not the ring took it: when a strip moves a bus to its DSP the
 *     bus's sends are re-sent there (ApolloCuesBusMoved), and after a connect every cached send goes out
 *     again (ApolloCuesReplay; nothing cached, nothing sent);
 *   - a muted cue sends 0.0 for each of its sends and keeps the levels; unmuting sends them again.
 * Fire and forget like the strips' route words: two entries and one doorbell per DSP, never waiting on
 * the DSP; a full ring answers STATUS_DEVICE_BUSY. [polish Nov 15] Loops of sends (replay, mute, bus move)
 * wait for room through one APOLLO_CUE_PASS (monitor_plan.h): at most 50 ms per send, none after a DSP
 * timed out once in the loop, 200 ms per loop in all, so one hung DSP no longer stalls every cell.
 *
 * [Mix] The monitor mix rides the same machinery with param 0 (UA_BUS_SUB_MAIN): per bus a level, a pan, a
 * mute and a solo (Ctx->Cues.Mix, monitor_plan.h ApolloMixCoef). Same owner rule, same cache: a bus the host
 * set is re-sent after a connect and on a bus move, before the cue sends (mutes reach the speakers first);
 * nothing set, nothing sent, so the unit keeps its own default (UA: inputs on at 0 dB) until the host
 * writes. Solo lives here rather than in Apollo Control: the replay after a connect must rebuild the soloed
 * mix without the app, every client (app, apollo-diag, step scripts) sees one solo, the host's own mutes stay
 * apart from the solo's silencing so ending a solo restores exactly what was set, and a solo change is one
 * loop under DevLock with one wait budget.
 *
 * [Aux] The two stereo aux buses ride it too (monitor_plan.h "[Aux]"): each input's send to AUX 1 / AUX 2 (param 3 / 4, L
 * and R, to the input's mixing DSP and DSP 0, pre- or post-fader), each aux's return into the monitor mix (bus 0x10 /
 * 0x12, param 0, the LEFT entry only, as UA's engine writes it) and its sends into the cues (same buses, param 1/2/5/6).
 * Same cache and replay: what the host set goes out again after a connect, a moved input bus re-sends its aux sends, a
 * monitor-mix change re-sends the post-fader sends of the buses it changed, a cue mute silences the aux returns into
 * that cue. The returns go to DSP 0, and also to the DSP an aux insert chain runs on while it runs
 * (ApolloCuesAuxOwner, plugin.c).
 *
 * DevLock held for every function here (ioctl.c, strips.c, mixer.c ApolloMixerInit).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "apollo.h"

/* monitor_plan.h keeps its own copies of the SET_MONITOR_PARAM codes (plain C) */
C_ASSERT(APOLLO_MP_LEVEL == APOLLO_MON_LEVEL && APOLLO_MP_HP1_LEVEL == APOLLO_MON_HP1_LEVEL &&
         APOLLO_MP_MUTE == APOLLO_MON_MUTE && APOLLO_MP_SOURCE == APOLLO_MON_SOURCE &&
         APOLLO_MP_MONO == APOLLO_MON_MONO && APOLLO_MP_HP1_SOURCE == APOLLO_MON_HP1_SOURCE &&
         APOLLO_MP_HP2_SOURCE == APOLLO_MON_HP2_SOURCE && APOLLO_MP_DIM_LEVEL == APOLLO_MON_DIM_LEVEL &&
         APOLLO_MP_DIM == APOLLO_MON_DIM && APOLLO_MP_TALKBACK == APOLLO_MON_TALKBACK);
C_ASSERT(APOLLO_MP_CUE1_MIX == APOLLO_MON_CUE1_MIX && APOLLO_MP_CUE1_MONO == APOLLO_MON_CUE1_MONO &&
         APOLLO_MP_CUE2_MIX == APOLLO_MON_CUE2_MIX && APOLLO_MP_CUE2_MONO == APOLLO_MON_CUE2_MONO &&
         APOLLO_MP_CUE3_MIX == APOLLO_MON_CUE3_MIX && APOLLO_MP_CUE3_MONO == APOLLO_MON_CUE3_MONO &&
         APOLLO_MP_CUE4_MIX == APOLLO_MON_CUE4_MIX && APOLLO_MP_CUE4_MONO == APOLLO_MON_CUE4_MONO);
C_ASSERT(APOLLO_MP_ALT1_TRIM == APOLLO_MON_ALT1_TRIM && APOLLO_MP_ALT2_TRIM == APOLLO_MON_ALT2_TRIM &&
         APOLLO_MP_ALT_SELECT == APOLLO_MON_ALT_SELECT && APOLLO_MP_ALT1_ENABLE == APOLLO_MON_ALT1_ENABLE &&
         APOLLO_MP_ALT2_ENABLE == APOLLO_MON_ALT2_ENABLE);
C_ASSERT(APOLLO_MP_CUE_SEND(3, 0x0A) == APOLLO_MON_CUE_SEND(3, 0x0A) && APOLLO_MP_CUE_PAN(1, 7) == APOLLO_MON_CUE_PAN(1, 7) &&
         APOLLO_MP_CUE_MUTE(2) == APOLLO_MON_CUE_MUTE(2) && APOLLO_CUE_SEND_OFF == APOLLO_MON_SEND_OFF &&
         APOLLO_MON_TALKBACK_BUS == APOLLO_MON_BUS_TALKBACK);
C_ASSERT(APOLLO_MON_BUSES == 32 && APOLLO_STRIP_INPUTS <= APOLLO_MON_BUSES);
/* [Virtual] VIRTUAL 1-8 = buses 0x18-0x1F, inside the 32-bit bus masks; the encoder's and the GET_STRIPS numbering agree */
C_ASSERT(APOLLO_MON_VIRTUAL_BUS0 == APOLLO_STRIP_VIRTUAL_BUS0 && APOLLO_STRIP_VIRTUAL_BUS0 + APOLLO_STRIP_VIRTUALS <= APOLLO_MON_BUSES &&
         APOLLO_MON_BUS_VIRTUAL(1) == APOLLO_MON_VIRTUAL_BUS(1) && APOLLO_MON_VIRTUAL_BUSES == (0xFFul << APOLLO_STRIP_VIRTUAL_BUS0));
C_ASSERT(APOLLO_MP_MIX_LEVEL(0x0A) == APOLLO_MON_MIX_LEVEL(0x0A) && APOLLO_MP_MIX_PAN(7) == APOLLO_MON_MIX_PAN(7) &&
         APOLLO_MP_MIX_MUTE(1) == APOLLO_MON_MIX_MUTE(1) && APOLLO_MP_MIX_SOLO(0) == APOLLO_MON_MIX_SOLO(0) &&
         APOLLO_MP_MIX_MUTE_ALL == APOLLO_MON_MIX_MUTE_ALL && APOLLO_MIX_SUB == 0);
C_ASSERT(APOLLO_MP_AUX_SEND(1, 0x0A) == APOLLO_MON_AUX_SEND(1, 0x0A) && APOLLO_MP_AUX_PAN(0, 7) == APOLLO_MON_AUX_PAN(0, 7) &&
         APOLLO_MP_AUX_POST(1, 3) == APOLLO_MON_AUX_POST(1, 3) && APOLLO_MP_AUX_RETURN(1) == APOLLO_MON_AUX_RETURN(1) &&
         APOLLO_MP_AUX_MUTE(0) == APOLLO_MON_AUX_MUTE(0) && APOLLO_MP_AUX_CUE(1, 3) == APOLLO_MON_AUX_CUE(1, 3) &&
         APOLLO_MP_AUX_CUE_PAN(0, 1) == APOLLO_MON_AUX_CUE_PAN(0, 1));
C_ASSERT(APOLLO_MON_AUX_RETURN_BUS(1) < APOLLO_MON_BUSES && APOLLO_MON_AUX_BUSES == APOLLO_PLUGIN_AUX_BUSES);

#define CUES_LOG_BUSY_MAX   5       /* "ring full" lines per device, then only the counter */

/*
 * The DSP that mixes Bus: its strip's DSP once that strip runs (strips.c BusOwner), else DSP 0; DSP 0 for
 * anything unusable (oa:driver/ua_dsp.c ua_dsp_bus_owner). A quarantined strip DSP gets nothing more.
 * [Virtual] An ANALOG bus is its Mic strip's slot, a VIRTUAL bus (0x18-0x1F) its Input Strip's (strips.c).
 */
static ULONG CuesOwner(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Bus)
{
    ULONG slot = ApolloStripsBusSlot(Bus);
    ULONG d = (slot < APOLLO_STRIP_SLOTS) ? Ctx->Strips.BusOwner[slot] : 0;

    if (d == 0 || d >= Ctx->NumDsps || d >= APOLLO_MAX_DSPS || !Ctx->Cmd[d].Allocated || !Ctx->Cmd[d].Live) {
        return 0;
    }
    if (Ctx->Strips.Quarantined && d == Ctx->Strips.Dsp) {
        return 0;
    }
    return d;
}

/*
 * One send's two entries (left, right) on DSP Dsp, one doorbell. Pass: the loop's wait budget (NULL = no wait):
 * a full ring is looked at again every 1 ms while ApolloCuePutStep allows (real time, KeQueryInterruptTime).
 */
static NTSTATUS CuesPutEx(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Dsp, _In_ ULONG Bus, _In_ ULONG Sub,
                          _In_ ULONG Left, _In_ ULONG Right, _In_ BOOLEAN LeftOnly, _Inout_opt_ APOLLO_CUE_PASS *Pass)
{
    PAPOLLO_RING cmd;
    ULONGLONG t0 = 0, elapsed = 0;
    BOOLEAN waiting = FALSE;
    int step;

    if (ApolloGateClosed(Ctx)) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    if (Dsp >= Ctx->NumDsps || Dsp >= APOLLO_MAX_DSPS) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    cmd = &Ctx->Cmd[Dsp];
    if (!cmd->Allocated || !cmd->Live) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    for (;;) {
        if (!NT_SUCCESS(ApolloRingRefresh(Ctx, cmd))) {
            return STATUS_DEVICE_NOT_CONNECTED;     /* all ones: the unit is gone */
        }
        if (waiting) {
            elapsed = KeQueryInterruptTime() - t0;
        }
        step = ApolloCuePutStep(Pass, Dsp, ApolloRingFreeSlots(cmd) >= 2, elapsed);
        if (step == APOLLO_CUE_PUT_SEND) {
            break;
        }
        if (step == APOLLO_CUE_PUT_BUSY) {
            if (Ctx->Cues.Busy++ < CUES_LOG_BUSY_MAX) {
                if (waiting) {
                    ApolloLogU(Ctx, "cues: DSP %lu command ring full for %lu ms (POSITION %lu, WRITE_PTR %lu): bus %lu "
                              "param %lu not sent, kept for the next replay; no more waits for this DSP in this pass",
                              Dsp, (ULONG)(elapsed / 10000ULL), cmd->Position, cmd->WritePtr, Bus, Sub);
                } else {
                    ApolloLogU(Ctx, "cues: DSP %lu command ring full (POSITION %lu, WRITE_PTR %lu): bus %lu param %lu "
                              "not sent, kept for the next replay", Dsp, cmd->Position, cmd->WritePtr, Bus, Sub);
                }
            }
            return STATUS_DEVICE_BUSY;
        }
        if (!waiting) {
            t0 = KeQueryInterruptTime();
            waiting = TRUE;
        }
        ApolloSleepMs(1);
    }
    ApolloRingWrite(cmd, APOLLO_DSP_CMD_BUS_PARAM, Bus, Sub, Left);
    if (!LeftOnly) {
        ApolloRingWrite(cmd, APOLLO_DSP_CMD_BUS_PARAM, Bus, (1ul << 24) | Sub, Right);
    }
    ApolloRingDoorbell(Ctx, cmd);
    Ctx->Cues.Entries += LeftOnly ? 1 : 2;
    return STATUS_SUCCESS;
}

static NTSTATUS CuesPut(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Dsp, _In_ ULONG Bus, _In_ ULONG Sub,
                        _In_ ULONG Left, _In_ ULONG Right, _Inout_opt_ APOLLO_CUE_PASS *Pass)
{
    return CuesPutEx(Ctx, Dsp, Bus, Sub, Left, Right, FALSE, Pass);
}

/*
 * Cue Cue's send of Bus as it stands now: to the owner and DSP 0, or (OwnerOnly) to the owner alone. Pass: the
 * loop's wait budget (CuesPut), NULL for a single send that never waits.
 */
static NTSTATUS CuesSendCell(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Cue, _In_ ULONG Bus, _In_ BOOLEAN OwnerOnly,
                             _Inout_opt_ APOLLO_CUE_PASS *Pass)
{
    ULONG l, r, owner = CuesOwner(Ctx, Bus), sub = ApolloCueSub(Cue);
    NTSTATUS status, s0;

    if (sub == 0 || Bus >= APOLLO_MON_BUSES) {
        return STATUS_INVALID_PARAMETER;
    }
    ApolloCueCoef(&Ctx->Cues, Cue, Bus, &l, &r);
    status = CuesPut(Ctx, owner, Bus, sub, l, r, Pass);
    if (owner != 0 && !OwnerOnly) {
        s0 = CuesPut(Ctx, 0, Bus, sub, l, r, Pass);
        if (NT_SUCCESS(status)) {
            status = s0;
        }
    }
    return status;
}

static BOOLEAN CuesReady(_In_ PDEVICE_CONTEXT Ctx)
{
    return Ctx->MixerReady && Ctx->Connected && Ctx->RingsLive && !ApolloGateClosed(Ctx);
}

/*
 * [Mix] Bus's monitor-mix weights as they stand (level, pan, its mute, the solo): param 0, to the owner and
 * DSP 0, or (OwnerOnly) to the owner alone. Pass as CuesSendCell.
 */
static NTSTATUS MixSendCell(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Bus, _In_ BOOLEAN OwnerOnly, _Inout_opt_ APOLLO_CUE_PASS *Pass)
{
    ULONG l, r, owner;
    NTSTATUS status, s0;

    if (Bus >= APOLLO_MON_BUSES) {
        return STATUS_INVALID_PARAMETER;
    }
    owner = CuesOwner(Ctx, Bus);
    ApolloMixCoef(&Ctx->Cues.Mix, Bus, &l, &r);
    status = CuesPut(Ctx, owner, Bus, APOLLO_MIX_SUB, l, r, Pass);
    if (owner != 0 && !OwnerOnly) {
        s0 = CuesPut(Ctx, 0, Bus, APOLLO_MIX_SUB, l, r, Pass);
        if (NT_SUCCESS(status)) {
            status = s0;
        }
    }
    return status;
}

/* ------------------------------------------------------------------ */
/* [Aux] The aux buses                                                  */
/* ------------------------------------------------------------------ */

/* The DSP that mixes aux Aux's return: an aux insert chain's while it runs (plugin.c), else DSP 0; CuesOwner's rules. */
static ULONG AuxOwner(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Aux)
{
    ULONG d = (Aux < APOLLO_MON_AUX_BUSES) ? Ctx->Cues.Aux.Owner[Aux] : 0;

    if (d == 0 || d >= Ctx->NumDsps || d >= APOLLO_MAX_DSPS || !Ctx->Cmd[d].Allocated || !Ctx->Cmd[d].Live) {
        return 0;
    }
    if (Ctx->Strips.Quarantined && d == Ctx->Strips.Dsp) {
        return 0;
    }
    return d;
}

/* Bus's send to aux Aux as it stands (param 3 / 4): to the input's mixing DSP and DSP 0, or (OwnerOnly) the owner. */
static NTSTATUS AuxSendCell(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Aux, _In_ ULONG Bus, _In_ BOOLEAN OwnerOnly,
                            _Inout_opt_ APOLLO_CUE_PASS *Pass)
{
    ULONG l, r, owner;
    NTSTATUS status, s0;

    if (Aux >= APOLLO_MON_AUX_BUSES || Bus >= APOLLO_MON_BUSES) {
        return STATUS_INVALID_PARAMETER;
    }
    owner = CuesOwner(Ctx, Bus);
    ApolloAuxSendCoef(&Ctx->Cues, Aux, Bus, &l, &r);
    status = CuesPut(Ctx, owner, Bus, APOLLO_AUX_SUB(Aux), l, r, Pass);
    if (owner != 0 && !OwnerOnly) {
        s0 = CuesPut(Ctx, 0, Bus, APOLLO_AUX_SUB(Aux), l, r, Pass);
        if (NT_SUCCESS(status)) {
            status = s0;
        }
    }
    return status;
}

/* Aux Aux's return into the monitor mix: bus 0x10 / 0x12, param 0, the left entry only. */
static NTSTATUS AuxReturnCell(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Aux, _In_ BOOLEAN OwnerOnly, _Inout_opt_ APOLLO_CUE_PASS *Pass)
{
    ULONG v, owner, bus;
    NTSTATUS status, s0;

    if (Aux >= APOLLO_MON_AUX_BUSES) {
        return STATUS_INVALID_PARAMETER;
    }
    bus = APOLLO_MON_AUX_RETURN_BUS(Aux);
    owner = AuxOwner(Ctx, Aux);
    v = ApolloAuxReturnBits(&Ctx->Cues.Aux, Aux);
    status = CuesPutEx(Ctx, owner, bus, APOLLO_MIX_SUB, v, 0, TRUE, Pass);
    if (owner != 0 && !OwnerOnly) {
        s0 = CuesPutEx(Ctx, 0, bus, APOLLO_MIX_SUB, v, 0, TRUE, Pass);
        if (NT_SUCCESS(status)) {
            status = s0;
        }
    }
    return status;
}

/* Aux Aux's return into cue Cue: bus 0x10 / 0x12, the cue's param, L and R. */
static NTSTATUS AuxCueCell(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Aux, _In_ ULONG Cue, _In_ BOOLEAN OwnerOnly,
                           _Inout_opt_ APOLLO_CUE_PASS *Pass)
{
    ULONG l, r, owner, bus, sub = ApolloCueSub(Cue);
    NTSTATUS status, s0;

    if (Aux >= APOLLO_MON_AUX_BUSES || sub == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    bus = APOLLO_MON_AUX_RETURN_BUS(Aux);
    owner = AuxOwner(Ctx, Aux);
    ApolloAuxCueCoef(&Ctx->Cues, Aux, Cue, &l, &r);
    status = CuesPut(Ctx, owner, bus, sub, l, r, Pass);
    if (owner != 0 && !OwnerOnly) {
        s0 = CuesPut(Ctx, 0, bus, sub, l, r, Pass);
        if (NT_SUCCESS(status)) {
            status = s0;
        }
    }
    return status;
}

/* The post-fader aux sends of every bus in Buses, again (a monitor-mix change moved their fader, mute or solo). */
static ULONG AuxPostResend(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Buses, _Inout_opt_ APOLLO_CUE_PASS *Pass)
{
    ULONG a, b, n = 0;

    for (a = 0; a < APOLLO_MON_AUX_BUSES; a++) {
        ULONG m = Buses & Ctx->Cues.Aux.Post[a] & Ctx->Cues.Aux.Set[a];

        for (b = 0; m != 0 && b < APOLLO_MON_BUSES; b++) {
            if ((m & (1ul << b)) != 0 && NT_SUCCESS(AuxSendCell(Ctx, a, b, FALSE, Pass))) {
                n++;
            }
        }
    }
    return n;
}

/* An aux write: stored, its cell(s) sent and kept for the replay. A level or pan move never waits for ring room. */
static NTSTATUS AuxApply(_In_ PDEVICE_CONTEXT Ctx, _In_ const APOLLO_MON_WRITE *W)
{
    APOLLO_AUX_STATE *x = &Ctx->Cues.Aux;
    ULONG a = W->Aux, b = W->Bus;

    if (a >= APOLLO_MON_AUX_BUSES || b >= APOLLO_MON_BUSES) {
        return STATUS_INVALID_PARAMETER;
    }
    ApolloAuxStore(x, W);
    switch (W->Kind) {
    case APOLLO_MONW_AUX_SEND:
    case APOLLO_MONW_AUX_PAN:
    case APOLLO_MONW_AUX_POST:
        x->Set[a] |= 1ul << b;
        if (W->Kind == APOLLO_MONW_AUX_POST) {
            ApolloLogU(Ctx, "aux: bus %lu's send to AUX %lu is %s-fader", b, a + 1, W->Arg ? "post" : "pre");
        }
        return AuxSendCell(Ctx, a, b, FALSE, NULL);
    case APOLLO_MONW_AUX_RETURN:
    case APOLLO_MONW_AUX_MUTE:
        x->RetSet |= 1ul << a;
        if (W->Kind == APOLLO_MONW_AUX_MUTE) {
            ApolloLogU(Ctx, "aux: AUX %lu return %s", a + 1, W->Arg ? "muted" : "unmuted");
        }
        return AuxReturnCell(Ctx, a, FALSE, NULL);
    case APOLLO_MONW_AUX_CUE:
    case APOLLO_MONW_AUX_CUE_PAN:
        if (b >= APOLLO_MON_CUES_MAX) {
            return STATUS_INVALID_PARAMETER;
        }
        x->CueSet[a] |= 1ul << b;
        return AuxCueCell(Ctx, a, b, FALSE, NULL);
    default:
        return STATUS_INVALID_PARAMETER;
    }
}

/*
 * [Mix] A monitor-mix write: stored, then every bus it changes (ApolloMixTargets) goes out and joins the replay
 * set. A fader or pan move of one bus: no wait on a full ring, as a single cue send (a drag sends dozens). A
 * mute, a solo, mute-all, or anything that sends several buses: one APOLLO_CUE_PASS, so a mute gets its short
 * wait for room. A mute, a solo and mute-all are logged (step-mix reads the line); levels and pans are not.
 */
static NTSTATUS MixApply(_In_ PDEVICE_CONTEXT Ctx, _In_ const APOLLO_MON_WRITE *W)
{
    APOLLO_MIX_STATE before = Ctx->Cues.Mix;
    APOLLO_CUE_PASS pass = { 0, 0 };
    NTSTATUS status = STATUS_SUCCESS, st;
    ULONG targets, b, n = 0;
    BOOLEAN nowait;

    ApolloMixStore(&Ctx->Cues.Mix, W);
    targets = ApolloMixTargets(&before, &Ctx->Cues.Mix, W);
    Ctx->Cues.Mix.Set |= targets;
    nowait = ((W->Kind == APOLLO_MONW_MIX_LEVEL || W->Kind == APOLLO_MONW_MIX_PAN) &&
              targets != 0 && (targets & (targets - 1)) == 0) ? TRUE : FALSE;
    for (b = 0; b < APOLLO_MON_BUSES; b++) {
        if ((targets & (1ul << b)) == 0) {
            continue;
        }
        st = MixSendCell(Ctx, b, FALSE, nowait ? NULL : &pass);
        if (NT_SUCCESS(st)) {
            n++;
        } else if (NT_SUCCESS(status)) {
            status = st;
        }
    }
    /* [Aux] a post-fader send carries the fader, the mute and the solo: those of the buses this write changed */
    (VOID)AuxPostResend(Ctx, targets, nowait ? NULL : &pass);
    if (W->Kind == APOLLO_MONW_MIX_MUTE || W->Kind == APOLLO_MONW_MIX_SOLO) {
        ApolloLogU(Ctx, "mix: bus %lu %s: %lu bus(es) sent (0x%08x); muted 0x%lx, soloed 0x%lx", W->Bus,
                  W->Kind == APOLLO_MONW_MIX_MUTE ? (W->Arg ? "muted" : "unmuted") : (W->Arg ? "soloed" : "unsoloed"),
                  n, status, Ctx->Cues.Mix.Muted, Ctx->Cues.Mix.Soloed);
    } else if (W->Kind == APOLLO_MONW_MIX_MUTE_ALL) {
        ApolloLogU(Ctx, "mix: all %s: %lu bus(es) sent (0x%08x); muted 0x%lx, soloed 0x%lx",
                  W->Arg ? "muted" : "unmuted", n, status, Ctx->Cues.Mix.Muted, Ctx->Cues.Mix.Soloed);
    }
    return status;
}

/*
 * A SEND, PAN or MUTE write (mixer.c ApolloSetMonitorParam, after ApolloMonEncode validated it for the
 * model). STATUS_DEVICE_NOT_READY before the mixer is up, with nothing kept (as a mailbox write: the host
 * sends it again). Once taken it is kept even if a full ring turned it away (STATUS_DEVICE_BUSY): the next
 * replay or bus move sends it.
 */
NTSTATUS ApolloCuesApply(_In_ PDEVICE_CONTEXT Ctx, _In_ const APOLLO_MON_WRITE *W)
{
    NTSTATUS status = STATUS_SUCCESS, st;
    ULONG b, n = 0;
    APOLLO_CUE_PASS pass = { 0, 0 };

    if (W->Cue >= APOLLO_MON_CUES_MAX || (W->Kind != APOLLO_MONW_MUTE && W->Bus >= APOLLO_MON_BUSES)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!CuesReady(Ctx)) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (ApolloMonIsMix(W)) {
        return MixApply(Ctx, W);        /* [Mix] the monitor mix */
    }
    if (ApolloMonIsAux(W)) {
        return AuxApply(Ctx, W);        /* [Aux] the aux buses */
    }
    ApolloCueStore(&Ctx->Cues, W);
    if (W->Kind != APOLLO_MONW_MUTE) {
        Ctx->Cues.Set[W->Cue] |= 1ul << W->Bus;
    }
    if (W->Kind == APOLLO_MONW_MUTE) {
        /* every send the host set on this cue: as 0.0 now, or back to its level */
        for (b = 0; b < APOLLO_MON_BUSES; b++) {
            if ((Ctx->Cues.Set[W->Cue] & (1ul << b)) == 0) {
                continue;
            }
            st = CuesSendCell(Ctx, W->Cue, b, FALSE, &pass);
            if (NT_SUCCESS(st)) {
                n++;
            } else if (NT_SUCCESS(status)) {
                status = st;
            }
        }
        for (b = 0; b < APOLLO_MON_AUX_BUSES; b++) {
            /* [Aux] the aux returns into this cue follow its mute */
            if ((Ctx->Cues.Aux.CueSet[b] & (1ul << W->Cue)) != 0 && NT_SUCCESS(AuxCueCell(Ctx, b, W->Cue, FALSE, &pass))) {
                n++;
            }
        }
        ApolloLogU(Ctx, "cues: cue %lu %s: %lu send(s) re-sent (0x%08x)", W->Cue + 1, W->Arg ? "muted" : "unmuted",
                  n, status);
        return status;
    }
    return CuesSendCell(Ctx, W->Cue, W->Bus, FALSE, NULL);
}

/*
 * strips.c, right after BusOwner[Bus] became the strip's DSP: that DSP mixes the input from now on, so it
 * gets the bus's cached sends (DSP 0 has them already). [Mix] Its monitor-mix weights first, if the host set
 * them. Returns how many went out.
 */
ULONG ApolloCuesBusMoved(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Bus)
{
    ULONG c, n = 0;
    APOLLO_CUE_PASS pass = { 0, 0 };

    if (Bus >= APOLLO_MON_BUSES || !CuesReady(Ctx)) {
        return 0;
    }
    if ((Ctx->Cues.Mix.Set & (1ul << Bus)) != 0 && NT_SUCCESS(MixSendCell(Ctx, Bus, TRUE, &pass))) {
        n++;
    }
    for (c = 0; c < APOLLO_MON_CUES_MAX; c++) {
        if ((Ctx->Cues.Set[c] & (1ul << Bus)) != 0 && NT_SUCCESS(CuesSendCell(Ctx, c, Bus, TRUE, &pass))) {
            n++;
        }
    }
    for (c = 0; c < APOLLO_MON_AUX_BUSES; c++) {
        /* [Aux] its aux sends */
        if ((Ctx->Cues.Aux.Set[c] & (1ul << Bus)) != 0 && NT_SUCCESS(AuxSendCell(Ctx, c, Bus, TRUE, &pass))) {
            n++;
        }
    }
    Ctx->Cues.Replays += n;
    return n;
}

/*
 * [Ins] plugin.c, DevLock held: an aux insert chain started on DSP Dsp (or, Dsp 0, its chain went): from now on the aux
 * return's coefficients go to that DSP and DSP 0, and the new owner gets the ones the host set. Returns how many went out.
 */
ULONG ApolloCuesAuxOwner(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Aux, _In_ ULONG Dsp)
{
    APOLLO_CUE_PASS pass = { 0, 0 };
    ULONG c, n = 0;

    if (Aux >= APOLLO_MON_AUX_BUSES || Ctx->Cues.Aux.Owner[Aux] == Dsp) {
        return 0;
    }
    Ctx->Cues.Aux.Owner[Aux] = Dsp;
    if (Dsp == 0 || !CuesReady(Ctx)) {
        return 0;
    }
    if ((Ctx->Cues.Aux.RetSet & (1ul << Aux)) != 0 && NT_SUCCESS(AuxReturnCell(Ctx, Aux, TRUE, &pass))) {
        n++;
    }
    for (c = 0; c < APOLLO_MON_CUES_MAX; c++) {
        if ((Ctx->Cues.Aux.CueSet[Aux] & (1ul << c)) != 0 && NT_SUCCESS(AuxCueCell(Ctx, Aux, c, TRUE, &pass))) {
            n++;
        }
    }
    Ctx->Cues.Replays += n;
    ApolloLogU(Ctx, "aux: AUX %lu return mixed on DSP %lu: %lu cell(s) re-sent there", Aux + 1, Dsp, n);
    return n;
}

/*
 * ApolloMixerInit (every connect): a DSP that restarted holds zero coefficients, so every send the host
 * set goes out again, each to its owner (DSP 0 here: the strips come later and move their buses with
 * ApolloCuesBusMoved) (oa:driver/ua_dsp.c ua_dsp_replay_bus_params). Nothing set, nothing sent.
 * [Mix] The monitor-mix buses the host set go first (a muted or solo-silenced input is 0.0 before any cue
 * send), in the same pass.
 */
/*
 * [BusEnable] The DSP output buses the model mixes, switched ON on every DSP at each connect: {0x001C0003, bus, 1, 0},
 * bus = 0 the monitor mix, 1 CUE 1, 2 CUE 2, 3 AUX 1, 4 AUX 2, 5 CUE 3, 6 CUE 4 (the same index as the bus coefficient's
 * sub-param). UA's kext keeps a 7-entry cache that starts "all enabled" and sends only changes (CUAD2DeviceMixer::
 * SetBusEnable; oa:driver/ua_dsp.c ua_dsp_set_bus_enable), so a unit that UA's own mixer engine ran last may come to us
 * with buses it switched off: upstream sends the first request for every bus for that reason. Nov 16 2026 on the x8p,
 * right after UA's engine had run it: a 0 dB send of VIRTUAL 1 into AUX 1 left AUX 1's record slots exact zero.
 * CUE 3/4 only on a model with four cues (Console turns them off at 176.4 kHz and up: left as the DSP has them then).
 */
static VOID CuesBusEnableAll(_In_ PDEVICE_CONTEXT Ctx)
{
    const APOLLO_MON_MODEL *m = ApolloMonModelFor(Ctx->Ident.Model);
    ULONG d, b, sent = 0, full = 0, buses = 0;

    if (m == 0) {
        return;
    }
    buses = 0x1Ful;                                     /* monitor, CUE 1, CUE 2, AUX 1, AUX 2 */
    if (m->Cues >= 4 && Ctx->SampleRate < 176400) {
        buses |= (1ul << 5) | (1ul << 6);               /* CUE 3, CUE 4 */
    }
    for (d = 0; d < Ctx->NumDsps && d < APOLLO_MAX_DSPS; d++) {
        PAPOLLO_RING cmd = &Ctx->Cmd[d];

        if (!cmd->Allocated || !cmd->Live || (Ctx->Strips.Quarantined && d == Ctx->Strips.Dsp)) {
            continue;
        }
        if (!NT_SUCCESS(ApolloRingRefresh(Ctx, cmd))) {
            return;
        }
        for (b = 0; b < 7; b++) {
            if ((buses & (1ul << b)) == 0) {
                continue;
            }
            if (ApolloRingFreeSlots(cmd) < 2) {
                full++;
                continue;
            }
            ApolloRingWrite(cmd, APOLLO_DSP_CMD_BUS_ENABLE, b, 1, 0);
            sent++;
        }
        ApolloRingDoorbell(Ctx, cmd);
    }
    ApolloLogU(Ctx, "mix: DSP buses 0x%02lx enabled on %lu DSP(s) (%lu command(s)%s)", buses, Ctx->NumDsps, sent,
               full ? ", some skipped: ring full" : "");
}

VOID ApolloCuesReplay(_In_ PDEVICE_CONTEXT Ctx)
{
    ULONG c, b, n = 0, failed = 0, mixN = 0, mixFailed = 0;
    APOLLO_CUE_PASS pass = { 0, 0 };

    if (!CuesReady(Ctx)) {
        return;
    }
    /*
     * [BusEnable] NOT sent: Nov 16 2026 on both units, {0x001C0003, b, 1, 0} for b 0-6 (x8p) / 0-4 (Twin X) on every DSP
     * at the connect left EVERY record slot of both units exact zero (VIRTUAL 1, MON, the mic inputs) until a power
     * cycle; the build before, without it, recorded VIRTUAL 1 at -30 dBFS and the monitor mix at -33 dBFS. The bus
     * groups or the value's meaning on these kinds are not what oa:driver/ua_dsp.c assumes. Kept for the record only.
     */
    (VOID)CuesBusEnableAll;
    for (b = 0; b < APOLLO_MON_BUSES; b++) {
        if ((Ctx->Cues.Mix.Set & (1ul << b)) == 0) {
            continue;
        }
        if (NT_SUCCESS(MixSendCell(Ctx, b, FALSE, &pass))) {
            mixN++;
        } else {
            mixFailed++;
        }
    }
    if (mixN != 0 || mixFailed != 0) {
        Ctx->Cues.Replays += mixN;
        ApolloLogU(Ctx, "mix: %lu monitor-mix bus(es) re-sent after the connect, %lu failed; muted 0x%lx, soloed 0x%lx",
                  mixN, mixFailed, Ctx->Cues.Mix.Muted, Ctx->Cues.Mix.Soloed);
    }
    for (c = 0; c < APOLLO_MON_CUES_MAX; c++) {
        for (b = 0; b < APOLLO_MON_BUSES; b++) {
            if ((Ctx->Cues.Set[c] & (1ul << b)) == 0) {
                continue;
            }
            if (NT_SUCCESS(CuesSendCell(Ctx, c, b, FALSE, &pass))) {
                n++;
            } else {
                failed++;
            }
        }
    }
    if (n != 0 || failed != 0) {
        Ctx->Cues.Replays += n;
        ApolloLogU(Ctx, "cues: %lu cached send(s) re-sent after the connect, %lu failed; muted cues 0x%lx; "
                  "waited %lu ms, busy DSPs 0x%lx", n, failed, Ctx->Cues.Muted, (ULONG)(pass.Waited / 10000ULL), pass.Busy);
    }
    /* [Aux] the aux sends, returns and returns into the cues the host set; an aux chain did not survive the connect */
    n = 0;
    failed = 0;
    for (c = 0; c < APOLLO_MON_AUX_BUSES; c++) {
        Ctx->Cues.Aux.Owner[c] = 0;
        for (b = 0; b < APOLLO_MON_BUSES; b++) {
            if ((Ctx->Cues.Aux.Set[c] & (1ul << b)) != 0) {
                if (NT_SUCCESS(AuxSendCell(Ctx, c, b, FALSE, &pass))) {
                    n++;
                } else {
                    failed++;
                }
            }
        }
        if ((Ctx->Cues.Aux.RetSet & (1ul << c)) != 0) {
            if (NT_SUCCESS(AuxReturnCell(Ctx, c, FALSE, &pass))) {
                n++;
            } else {
                failed++;
            }
        }
        for (b = 0; b < APOLLO_MON_CUES_MAX; b++) {
            if ((Ctx->Cues.Aux.CueSet[c] & (1ul << b)) != 0) {
                if (NT_SUCCESS(AuxCueCell(Ctx, c, b, FALSE, &pass))) {
                    n++;
                } else {
                    failed++;
                }
            }
        }
    }
    if (n != 0 || failed != 0) {
        Ctx->Cues.Replays += n;
        ApolloLogU(Ctx, "aux: %lu aux cell(s) re-sent after the connect, %lu failed", n, failed);
    }
}
