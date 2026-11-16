/*
 * monitor_plan.h - the monitor features both units have, as data (docs/console-parity.md, "Monitor"
 * rows and Order item 3): ALT speakers, monitor and headphone sources, cue mixes (send level and pan
 * per input and cue, cue MIX source, mono, mute), talkback routed to the cues; [Mix] the monitor mix itself
 * (per input and the talkback: level -inf..+12 dB, pan, mute, solo, mute-all; "Mixer" row of the parity table).
 *
 * Plain C like preamp_plan.h: no kernel or Windows headers, every function static __inline, every
 * table inside its function. mixer.c encodes SET_MONITOR_PARAM with it, cues.c keeps the cue sends,
 * bringup_plan.h takes the plan bits of the new codes from it, apollo-diag and Apollo Control's host
 * name and decode with it, and tools\mon-plan-test proves that every x8p monitor write that existed
 * before is unchanged and that each model refuses what it lacks.
 *
 * Evidence (oa: = apollo-win\reference\open-apollo, read only):
 *   mailbox fields     kext SetOtherParam disassembly, oa:driver/ua_core.c:1973-2340 (ua_monitor_set_param),
 *                      names oa:driver/ua_apollo.h:651-765, no kind gate for any code used here
 *                      (oa:driver/ua_core.c:1838-1870 ua_other_param_allowed)
 *   ALT speakers       0x38/0x39 trim S15 [6:0] / [22:16] raw = 60 + 2*dB, 0..120 (oa:mixer-engine/hardware.py:619-622,
 *                      device map AltMonTrim -30..+30), 0x3B/0x3C enable S15 bit 8 / 12, 0x3A selection
 *                      S13 [29:28] (0 MAIN, 1 ALT 1, 2 ALT 2; oa:console-rs/WIRING.md row 48), readback
 *                      word 6 [29:28] (oa:driver/ua_apollo.h:884-885). ALT n is output pair n of the line
 *                      outputs (oa:mixer-engine/hardware.py:598-601, 4436-4451): x8p Line 1-2 / 3-4, Twin X
 *                      Line 3/4 (its only line pair: one ALT, spec-twinx 4.1). Medium confidence upstream
 *                      (names resolved from Mixer Engine call sites).
 *   monitor source     0x04 S2 [19:18] + bit 29: kext encoder bit29 = (v >> 2) & 1, [19:18] = v & 3
 *                      (oa:driver/ua_core.c:2055-2065); 0 MIX, 1 CUE 1, 2 CUE 2 DTrace-verified
 *                      (oa:mixer-engine/hardware.py:885-888). CUE 3 = 3 and CUE 4 = 4 (bit 29) follow the
 *                      kext's three-bit encoder and the shared MixInSource numbering, NOT captured: the
 *                      morning test checks them.
 *   headphone source   0x3F/0x40 S12 [26:24] / [29:27], 0 = CUE 1, 1 = CUE 2, DTrace-verified
 *                      (oa:mixer-engine/hardware.py:4391-4400, WIRING.md row 49). x8p: the same kext path
 *                      (no kind gate), and bits 24-29 are clear of its preamp 5-8 flags in S12 [23:0]
 *                      (spec-dsp-mixer 5.2), so CUE 1 / CUE 2 are offered on both units. CUE 3/4, MON and
 *                      NONE have no known encoding: refused (oa:mixer-engine/console_commands.py:123-134).
 *   cue MIX / MONO     0x05/0x07 S2 [25:24] / [27:26] (0 = the cue's own mix, 2 = the monitor mix;
 *                      oa:mixer-engine/hardware.py:877-883, settings "SOURCE MIX/CUE" rack settings.rs:655),
 *                      0x06/0x08 S2 [21:20] / [23:22] swapped (mono = the upper bit, ua_swap2); CUE 3/4
 *                      0x22/0x24 S8 [29:28] / [31:30], 0x23/0x25 S8 [25:24] / [27:26] swapped
 *                      (oa:driver/ua_apollo.h:707-713; medium confidence upstream, x8p only).
 *   cue sends          DSP ring command {0x001D0004, bus, (side << 24) | param, float}: param 1 CUE 1,
 *                      2 CUE 2, 5 CUE 3, 6 CUE 4, side 0 left / 1 right weight (oa:driver/ua_dsp.c:3633-3652,
 *                      oa:driver/ua_apollo.h:825-831), to the DSP that mixes the bus and to DSP 0
 *                      (oa:driver/ua_dsp.c:3790-3829; docs/STRIPS.md 2.8). Level: 10^(dB/20)/sqrt2
 *                      (encode_input_fader, IOKit-verified 0 / -10 / -40 dB, oa:mixer-engine/hardware.py:1023-1038),
 *                      pan: left = lin*sqrt2*cos((p+1)pi/4), right = ...sin (_write_send, hardware.py:2954-2975).
 *                      Buses: ANALOG n = n-1, TALKBACK 0x0A (oa:devices/apollo-x8p.json / apollo-twin-x.json
 *                      mixer_inputs synch_in, talkback_synch_in 10). Send range -inf..+12 dB (device map).
 *                      [Virtual] VIRTUAL n = 0x18 + n - 1 (synch_in 24-31), only where the unit builds that input's
 *                      Input Strip (StripVirtualMask; ApolloMonEncodeEx's Extra).
 *   [Mix] monitor mix  the same command with param 0: UA_BUS_SUB_MAIN "control-room mix coefficient (fader x pan)"
 *                      (oa:driver/ua_apollo.h:825, ua_dsp.c:3644 "param: 0 CR"; hardware.py SUB_PARAM_MIX = 0, "0=main
 *                      mix"). Weights (DTrace-verified encode_mix_coeff / _r, hardware.py:1041-1068): left =
 *                      10^(dB/20) cos((p+1)pi/4), right = ... sin, i.e. the send law above (0 dB centre = 0x3F3504F3
 *                      on both sides, IOKit-captured). A fader or pan move writes sub 0 only, L and R
 *                      (set_bus_fader, hardware.py:2020-2049). Mute = both weights 0.0 (hardware.py:3529-3535); solo
 *                      is the host's: every bus not soloed goes out as 0.0, a muted one stays 0.0
 *                      (_handle_solo_recalc, hardware.py:3240-3321; the kext's negated "solo-mute" weights are off by
 *                      default upstream, ua_dsp.c:3682-3701, so 0.0 is what the stock engine sends). The talkback bus
 *                      has its own law, DTrace-verified: sub 0 only, 10^(dB/20) x 3.976353, L = R, no pan
 *                      (hardware.py:373-377, 3222-3228, 3482-3489). A linked pair: the leader's fader, mute and solo
 *                      drive both buses, its Pan places its own bus and Pan2 the partner's (-1 / +1 on link, 0 / 0 on
 *                      unlink; hardware.py:3814-3980): host work, the driver mixes per bus. Owner: the strip's DSP
 *                      plus DSP 0, re-sent on a bus move (ua_dsp.c:3860-3903): "with the strips on DSP 1, coefficients
 *                      sent only to DSP 0 leave the input in the monitor mix at DSP 1's own level, so fader, pan, mute
 *                      and solo do nothing" (oa:console-rs/WIRING.md row 20), which is how a live Twin X strip reached
 *                      its speakers at about -26 dBFS on Nov 15 with nothing sent. UA's default (inputs on at 0 dB)
 *                      is the zero state here; nothing is sent until the host sets a bus.
 *
 * [Aux] The two stereo aux buses (console-parity "2 stereo aux buses (pre/post), aux returns"), the same bus
 * coefficient command with upstream's layout (oa:mixer-engine/hardware.py:464-520 aux_bus_id and SUB_PARAM_*,
 * 2883-3060 _send_is_post_fader / _input_send_linear / _write_send, 4484-4600 _handle_aux / _handle_aux_send;
 * oa:driver/ua_apollo.h:816-831; oa:console-rs/WIRING.md rows 28-37, "wired" upstream):
 *   input send         {bus = the input's, param 3 (AUX 1) / 4 (AUX 2), L / R}: 10^(dB/20)/sqrt2 with the send's pan
 *                      (the cue send law above); post-fader: the input's monitor-mix level added in dB, 0.0 while the
 *                      input is muted or silenced by a solo (the engine's encode_input_fader(gain + fader)); pre-fader
 *                      ignores fader, mute and solo. Per input, as Console's PostFaderOverride (UA's per-aux SendPostFader
 *                      is the host's "set every input" convenience). To the input's mixing DSP and DSP 0, like a cue send.
 *   aux return         {bus 0x10 (AUX 1) / 0x12 (AUX 2), param 0, side 0 only}: 10^(dB/20), no sqrt2, no pan, no right
 *                      entry ("aux faders send only sub_param=0 (single write, not 3)"); mute = 0.0.
 *   aux to a cue       {bus 0x10 / 0x12, param 1/2/5/6 (CUE 1-4), L / R}: 10^(dB/20) with the pan law (encode_aux_fader +
 *                      _write_send), 0.0 while that cue is muted.
 *   The aux bus mix itself is the DSPs' (bus enable type 3/4 is on by default, the kext's cache starts all-enabled,
 *   spec-dsp-mixer 6.3). Aux returns go to DSP 0, or to the DSP an aux insert chain runs on and DSP 0 (cues.c
 *   ApolloCuesAuxOwner, plugin.c). Aux MONO (0x0A / 0x0F, S6 / S7 [9:8]) is not offered: on the x8p those settings
 *   are not decoded and may hold other fields. Not run on hardware: whether the aux returns (synch 0x10-0x13) carry
 *   audio without an Input Strip there is unproven (DSP inputs with no strip "mix nothing" for the Virtual inputs,
 *   oa:driver/ua_strips.c:19-25); the aux outputs record on the x8p's record slots 28-31 either way.
 *
 * Not here: per-output speaker trims (Speaker Utilities): no mailbox field or bus coefficient is known for
 * a monitor output's trim (the surround trims 0x4B-0x52 / 0x6B-0x72 have no decoded encoding,
 * oa:mixer-engine/hardware.py:4455-4466): host or DSP work. Talkback to the monitor (0x47 TBConfig) and
 * talkback dim are not mailbox writes here; Apollo Control dims the monitor itself while talkback is on.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once
#include "apollo_model.h"

/* ------------------------------------------------------------------ */
/* SET_MONITOR_PARAM codes                                             */
/* ------------------------------------------------------------------ */

/*
 * Copies of apollo_ioctl.h APOLLO_MON_* (which needs Windows types; ioctl.c asserts they are equal).
 * The first ten existed before; their writes on the x8p are unchanged (mon-plan-test).
 */
#define APOLLO_MP_LEVEL         0x01ul  /* raw 0..192, S2 [7:0] */
#define APOLLO_MP_HP1_LEVEL     0x02ul  /* raw 0..192, S2 [15:8] */
#define APOLLO_MP_MUTE          0x03ul  /* 0/1, S2 bit 16 */
#define APOLLO_MP_SOURCE        0x04ul  /* 0 MIX, 1-4 CUE 1-4, S2 [19:18] + bit 29 */
#define APOLLO_MP_MONO          0x10ul  /* 0/1, S2 bit 17 */
#define APOLLO_MP_HP1_SOURCE    0x3Ful  /* 0 CUE 1, 1 CUE 2, S12 [26:24] */
#define APOLLO_MP_HP2_SOURCE    0x40ul  /* 0 CUE 1, 1 CUE 2, S12 [29:27] */
#define APOLLO_MP_DIM_LEVEL     0x43ul  /* 1..7, S14 [30:28] */
#define APOLLO_MP_DIM           0x44ul  /* 0/1, S2 bit 31 */
#define APOLLO_MP_TALKBACK      0x46ul  /* 0/1, S13 bit 8 */
/* [Monitor] new kext ids */
#define APOLLO_MP_CUE1_MIX      0x05ul  /* 0 the cue's own mix, 1 the monitor mix: S2 [25:24] = 0 / 2 */
#define APOLLO_MP_CUE1_MONO     0x06ul  /* 0/1: S2 [21:20], mono = bit 21 */
#define APOLLO_MP_CUE2_MIX      0x07ul  /* S2 [27:26] */
#define APOLLO_MP_CUE2_MONO     0x08ul  /* S2 [23:22], mono = bit 23 */
#define APOLLO_MP_CUE3_MIX      0x22ul  /* S8 [29:28] (x8p) */
#define APOLLO_MP_CUE3_MONO     0x23ul  /* S8 [25:24], mono = bit 25 */
#define APOLLO_MP_CUE4_MIX      0x24ul  /* S8 [31:30] */
#define APOLLO_MP_CUE4_MONO     0x25ul  /* S8 [27:26], mono = bit 27 */
#define APOLLO_MP_ALT1_TRIM     0x38ul  /* raw 0..120 = 60 + 2*dB (-30..+30 dB), S15 [6:0] */
#define APOLLO_MP_ALT2_TRIM     0x39ul  /* S15 [22:16] */
#define APOLLO_MP_ALT_SELECT    0x3Aul  /* 0 MAIN, 1 ALT 1, 2 ALT 2: S13 [29:28] */
#define APOLLO_MP_ALT1_ENABLE   0x3Bul  /* 0/1: S15 bit 8 */
#define APOLLO_MP_ALT2_ENABLE   0x3Cul  /* 0/1: S15 bit 12 */
/*
 * [Out] Output and front-panel settings: kext SetOtherParam ids, encodings as oa:driver/ua_core.c ua_monitor_set_param
 * (no kind gate on the x8p, kind 12, or the Twin X Gen 1, kind 15, except 0x42: kinds 4,6,10-13,16-20, the x8p only),
 * offered per model as oa:devices/apollo-x8p.json / apollo-twin-x.json say the model has it (output_pad line/monitor,
 * mirror_to_digital, func_switch) and the x8p device map lists it (oa:mixer-engine/device_maps/device_map_apollo_x8p.json:
 * outputs/0-7 Pad, outputs/22 Pad + MirrorsToDigital, auxs/0-1 MixToMono, /ClipHold, /PeakHold, /devices/0/FuncSwitchMode).
 * The fields sit clear of the preamp fields (preamp_plan.h: S0 [25:0], S12 [23:0], S13 nibbles, S15 [31:24]).
 */
#define APOLLO_MP_AUX1_MONO     0x0Aul  /* 0/1: S6 [9:8] swapped, mono = bit 9 (CMxAuxBus MixToMono) */
#define APOLLO_MP_AUX2_MONO     0x0Ful  /* 0/1: S7 [9:8] swapped, mono = bit 9 */
#define APOLLO_MP_CLIP_HOLD     0x13ul  /* front-panel meters: 0 NONE, 1 1 s, 2 3 s, 3 5 s, 4 10 s, 5 INFINITE: S1 [30:28] */
#define APOLLO_MP_PEAK_HOLD     0x14ul  /* 0 NONE, 1 1 s, 2 3 s, 3 5 s, 4 10 s: S1 [15:13] */
#define APOLLO_MP_LINE_PAD(n)   (0x19ul + (unsigned long)(n))   /* line pair n (0-3) reference: 0 +4 dBu, 1 -10 dBV: S0 bit 28+n */
#define APOLLO_MP_LINE_PAD_LAST 0x1Cul
#define APOLLO_MP_IDENTIFY      0x1Dul  /* 0 normal, 1-7 the front panel's identify states (UA: 3, 1, 2, 0): S6 [13:11], [15:14] cleared */
#define APOLLO_MP_DIG_MIRROR    0x1Eul  /* 0/1: the monitor mirrored to the S/PDIF output: S6 bit 10 (DTrace-verified upstream) */
#define APOLLO_MP_MON_REF       0x32ul  /* monitor outputs reference: 0 +4 dBu, 1 -10 dBV: S11 bit 4 (DTrace-verified upstream) */
#define APOLLO_MP_FUNC_SWITCH   0x42ul  /* function switch: 0 none, 2 mono, 3 dim, 4 talkback (DescartesFuncSwitchMode): S13 [15:13] */
/*
 * [Monitor] driver-side families (no kext id: the sends are DSP ring commands, not mailbox fields).
 * Cue c is 0-based (0-3), bus = the mixer input bus (ANALOG n = n-1, TALKBACK 0x0A).
 */
#define APOLLO_MP_CUE_SEND(c, b)    (0x1000ul | (((unsigned long)(c) & 0xFul) << 8) | ((unsigned long)(b) & 0xFFul))
#define APOLLO_MP_CUE_PAN(c, b)     (0x2000ul | (((unsigned long)(c) & 0xFul) << 8) | ((unsigned long)(b) & 0xFFul))
#define APOLLO_MP_CUE_MUTE(c)       (0x3000ul | (((unsigned long)(c) & 0xFul) << 8))
#define APOLLO_MP_FAMILY(p)         ((p) & 0xF000ul)
/*
 * [Mix] The monitor mix (what the monitor outputs play): per bus, sub 0 of the same DSP bus coefficients.
 * 0x4000 | kind << 8 | bus; kind 0 level, 1 pan, 2 mute, 3 solo, 4 mute every bus the model mixes (bus 0).
 */
#define APOLLO_MP_MIX_LEVEL(b)      (0x4000ul | ((unsigned long)(b) & 0xFFul))   /* half-dB -120..24, -121 = off */
#define APOLLO_MP_MIX_PAN(b)        (0x4100ul | ((unsigned long)(b) & 0xFFul))   /* -100..100; not the talkback */
#define APOLLO_MP_MIX_MUTE(b)       (0x4200ul | ((unsigned long)(b) & 0xFFul))   /* 0/1 */
#define APOLLO_MP_MIX_SOLO(b)       (0x4300ul | ((unsigned long)(b) & 0xFFul))   /* 0/1 */
#define APOLLO_MP_MIX_MUTE_ALL      0x4400ul                                     /* 0/1 */
#define APOLLO_MP_MIX_FIRST         0x4000ul
#define APOLLO_MP_MIX_LAST          0x44FFul
#define APOLLO_MP_MIX_KIND(p)       (((p) >> 8) & 0xFul)
/*
 * [Aux] The two stereo aux buses: 0x6000 | kind << 8 | aux << 5 | index (aux 0 = AUX 1, 1 = AUX 2; bits 6-7 zero).
 * kind 0 send level, 1 send pan, 2 send post-fader: index = the input's bus (ANALOG n = n-1, TALKBACK 0x0A);
 * kind 3 return level, 4 return mute: index 0; kind 5 return-to-cue level, 6 its pan: index = the cue (0-3).
 */
#define APOLLO_MP_AUX(k, a, i)      (0x6000ul | (((unsigned long)(k) & 0xFul) << 8) | (((unsigned long)(a) & 1ul) << 5) | \
                                     ((unsigned long)(i) & 0x1Ful))
#define APOLLO_MP_AUX_SEND(a, b)    APOLLO_MP_AUX(0, a, b)      /* half-dB -120..24, -121 = off */
#define APOLLO_MP_AUX_PAN(a, b)     APOLLO_MP_AUX(1, a, b)      /* -100..100 */
#define APOLLO_MP_AUX_POST(a, b)    APOLLO_MP_AUX(2, a, b)      /* 0 pre-fader (default), 1 post-fader */
#define APOLLO_MP_AUX_RETURN(a)     APOLLO_MP_AUX(3, a, 0)      /* half-dB -120..24, -121 = off */
#define APOLLO_MP_AUX_MUTE(a)       APOLLO_MP_AUX(4, a, 0)      /* 0/1 */
#define APOLLO_MP_AUX_CUE(a, c)     APOLLO_MP_AUX(5, a, c)      /* half-dB -120..24, -121 = off */
#define APOLLO_MP_AUX_CUE_PAN(a, c) APOLLO_MP_AUX(6, a, c)      /* -100..100 */
#define APOLLO_MP_AUX_FIRST         0x6000ul
#define APOLLO_MP_AUX_LAST          0x66FFul
#define APOLLO_MP_AUX_KIND(p)       (((p) >> 8) & 0xFul)
#define APOLLO_MON_AUX_BUSES        2ul
#define APOLLO_MON_AUX_RETURN_BUS(a) (0x10ul + 2ul * (unsigned long)(a))    /* UA_BUS_AUX1_RETURN / AUX2_RETURN */
#define APOLLO_AUX_SUB(a)           (3ul + (unsigned long)(a))              /* UA_BUS_SUB_AUX1 / AUX2 */

/* Values */
#define APOLLO_MP_LEVEL_MAX         192L
#define APOLLO_MP_ALT_TRIM_MAX      120L        /* +30 dB; 60 = 0 dB */
#define APOLLO_MP_ALT_TRIM_UNITY    60L
#define APOLLO_CUE_SEND_OFF         (-121L)     /* send level: -inf (coefficient 0.0) */
#define APOLLO_CUE_SEND_MIN         (-120L)     /* -60 dB, in half-dB steps */
#define APOLLO_CUE_SEND_MAX         24L         /* +12 dB */
#define APOLLO_CUE_PAN_MAX          100L        /* -100 hard left .. 100 hard right */

#define APOLLO_MIX_LEVEL_OFF        APOLLO_CUE_SEND_OFF     /* [Mix] the monitor mix level: same range as a send */
#define APOLLO_MIX_LEVEL_MAX        APOLLO_CUE_SEND_MAX
#define APOLLO_MIX_PAN_MAX          APOLLO_CUE_PAN_MAX

#define APOLLO_MON_CUES_MAX         4ul
#define APOLLO_MON_BUSES            32ul
#define APOLLO_MON_TALKBACK_BUS     0x0Aul
#define APOLLO_DSP_CMD_BUS_PARAM    0x001D0004ul    /* {cmd, bus, (side << 24) | param, float} */
#define APOLLO_DSP_CMD_BUS_ENABLE   0x001C0003ul    /* [BusEnable] {cmd, bus group 0-6, 1 on / 0 off, 0} to every DSP */
#define APOLLO_MIX_SUB              0ul             /* [Mix] UA_BUS_SUB_MAIN: the monitor (control-room) mix */

/* ------------------------------------------------------------------ */
/* Per model                                                           */
/* ------------------------------------------------------------------ */

typedef struct _APOLLO_MON_MODEL {
    int             Model;              /* APOLLO_MODEL_* */
    const char     *Name;
    unsigned long   Cues;               /* cue buses: CUE 1..Cues */
    unsigned long   Alts;               /* ALT speaker pairs */
    unsigned long   Hps;                /* headphone outputs with a source field (HP1..Hps) */
    unsigned long   SourceMax;          /* monitor source values 0..SourceMax (0 MIX, n CUE n) */
    unsigned long   AnalogBuses;        /* buses 0..AnalogBuses-1 = ANALOG 1..n (the preamps) */
    unsigned long   TalkbackBus;        /* the talkback mic's bus */
    const char     *CueName[APOLLO_MON_CUES_MAX];
    const char     *AltOut[2];          /* the line pair ALT n uses */
    /* [Out] */
    unsigned long   LinePads;           /* line output pairs with a reference level (0x19..) */
    int             MonRef;             /* monitor outputs reference level (0x32) */
    int             DigMirror;          /* monitor to S/PDIF (0x1E) */
    int             FuncSwitch;         /* function switch mode (0x42) */
} APOLLO_MON_MODEL;

/* The monitor plan of a model, or 0 (no monitor control). */
static __inline const APOLLO_MON_MODEL *ApolloMonModelFor(int Model)
{
    static const APOLLO_MON_MODEL t[2] = {
        /* x8p: CUE 1-4 (CueBusCount max 4, oa:devices/apollo-x8p.json cue_buses), ALT 1-2 (MaxAltMons 2,
           oa:mixer-engine/device_maps/device_map_apollo_x8p.json), HP 1-2, ANALOG 1-8, talkback */
        { APOLLO_MODEL_X8P, "Apollo x8p", 4, 2, 2, 4, 8, APOLLO_MON_TALKBACK_BUS,
          { "CUE 1", "CUE 2", "CUE 3", "CUE 4" }, { "Line 1-2", "Line 3-4" },
          /* [Out] Line 1-8 reference per pair, monitor reference, S/PDIF mirror, function switch */
          4, 1, 1, 1 },
        /* Twin X Gen 1 (spec-twinx 4.1): two fixed cues named after their outputs, one ALT on LINE 3/4,
           one headphone, ANALOG 1-2, the built-in talkback mic */
        { APOLLO_MODEL_TWINX, "Apollo Twin X (Gen 1)", 2, 1, 1, 2, 2, APOLLO_MON_TALKBACK_BUS,
          { "HP", "LINE 3/4", 0, 0 }, { "Line 3/4", 0 },
          /* [Out] the monitor reference only (output_pad monitor true, line false; no digital out, no FCN) */
          0, 1, 0, 0 },
    };

    if (Model == APOLLO_MODEL_X8P) {
        return &t[0];
    }
    if (Model == APOLLO_MODEL_TWINX) {
        return &t[1];
    }
    return 0;
}

/* Does model M mix bus B (a send to it means something)? */
static __inline int ApolloMonHasBus(const APOLLO_MON_MODEL *M, unsigned long Bus)
{
    return M != 0 && (Bus < M->AnalogBuses || Bus == M->TalkbackBus);
}

/*
 * [Virtual] The Virtual input buses: VIRTUAL n = bus 0x18 + n - 1, the DSP input a DAW plays into
 * (oa:devices/apollo-x8p.json / apollo-twin-x.json mixer_inputs synch_in 24-31). A Virtual input "mixes nothing and
 * records silence" without an Input Strip on it (oa:driver/ua_strips.c:19-25), so no model mixes them by itself:
 * the unit names the ones whose strip it builds (strips.c ApolloStripsVirtualBuses, from StripVirtualMask) as
 * Extra, a bus mask, and ApolloMonEncodeEx takes them for the monitor mix, the cue sends and the aux sends
 * exactly as an ANALOG bus (level, pan, mute, solo, mute-all; the same coefficient laws). Extra outside
 * APOLLO_MON_VIRTUAL_BUSES is ignored; Extra 0 is ApolloMonEncode, every code and error as before.
 */
#define APOLLO_MON_VIRTUAL_BUS0     0x18ul
#define APOLLO_MON_VIRTUAL_BUS(n)   (APOLLO_MON_VIRTUAL_BUS0 + (unsigned long)(n) - 1ul)     /* n = 1..8 */
#define APOLLO_MON_VIRTUAL_BUSES    0xFF000000ul

/* Does model M mix bus B, with the Virtual buses Extra the unit's strips make live? */
static __inline int ApolloMonHasBusEx(const APOLLO_MON_MODEL *M, unsigned long Extra, unsigned long Bus)
{
    if (ApolloMonHasBus(M, Bus)) {
        return 1;
    }
    return M != 0 && Bus < APOLLO_MON_BUSES && (((Extra & APOLLO_MON_VIRTUAL_BUSES) >> Bus) & 1ul) != 0;
}

/* [Mix] Every bus model M mixes, as a mask (x8p 0x4FF: ANALOG 1-8 + TALKBACK; Twin X 0x403). */
static __inline unsigned long ApolloMonBusMask(const APOLLO_MON_MODEL *M)
{
    unsigned long m = 0, b;

    for (b = 0; M != 0 && b < APOLLO_MON_BUSES; b++) {
        if (ApolloMonHasBus(M, b)) {
            m |= 1ul << b;
        }
    }
    return m;
}

/* The bus coefficient param of cue c (0-based): 1, 2, 5, 6 (oa:driver/ua_apollo.h:825-831), 0 = none. */
static __inline unsigned long ApolloCueSub(unsigned long Cue)
{
    static const unsigned long s[APOLLO_MON_CUES_MAX] = { 1ul, 2ul, 5ul, 6ul };

    return Cue < APOLLO_MON_CUES_MAX ? s[Cue] : 0ul;
}

/*
 * [Monitor] Plan bits of the new codes for bringup_plan.h APOLLO_BRINGUP_PLAN.MonitorParams (G4's ten
 * codes keep 0x001-0x200). 0 = not a monitor code.
 */
#define APOLLO_MON_BIT_CUE1_MIX     0x00000400ul
#define APOLLO_MON_BIT_CUE1_MONO    0x00000800ul
#define APOLLO_MON_BIT_CUE2_MIX     0x00001000ul
#define APOLLO_MON_BIT_CUE2_MONO    0x00002000ul
#define APOLLO_MON_BIT_CUE3_MIX     0x00004000ul
#define APOLLO_MON_BIT_CUE3_MONO    0x00008000ul
#define APOLLO_MON_BIT_CUE4_MIX     0x00010000ul
#define APOLLO_MON_BIT_CUE4_MONO    0x00020000ul
#define APOLLO_MON_BIT_ALT1_TRIM    0x00040000ul
#define APOLLO_MON_BIT_ALT2_TRIM    0x00080000ul
#define APOLLO_MON_BIT_ALT_SELECT   0x00100000ul
#define APOLLO_MON_BIT_ALT1_ENABLE  0x00200000ul
#define APOLLO_MON_BIT_ALT2_ENABLE  0x00400000ul
#define APOLLO_MON_BIT_CUE_SEND     0x00800000ul
#define APOLLO_MON_BIT_CUE_PAN      0x01000000ul
#define APOLLO_MON_BIT_CUE_MUTE     0x02000000ul
#define APOLLO_MON_BIT_MIX_LEVEL    0x04000000ul    /* [Mix] the monitor mix families */
#define APOLLO_MON_BIT_MIX_PAN      0x08000000ul
#define APOLLO_MON_BIT_MIX_MUTE     0x10000000ul
#define APOLLO_MON_BIT_MIX_SOLO     0x20000000ul
#define APOLLO_MON_BIT_MIX_MUTE_ALL 0x40000000ul
#define APOLLO_MON_BIT_AUX          0x80000000ul    /* [Aux] every aux family (one bit: the last free one) */
#define APOLLO_MON_EXT_MIX          (APOLLO_MON_BIT_MIX_LEVEL | APOLLO_MON_BIT_MIX_PAN | APOLLO_MON_BIT_MIX_MUTE | \
                                     APOLLO_MON_BIT_MIX_SOLO | APOLLO_MON_BIT_MIX_MUTE_ALL)
/* The Twin X: CUE 1-2 (HP, LINE 3/4), ALT 1 (LINE 3/4), sends; never CUE 3/4 or ALT 2 */
#define APOLLO_MON_EXT_TWINX        (APOLLO_MON_BIT_CUE1_MIX | APOLLO_MON_BIT_CUE1_MONO | APOLLO_MON_BIT_CUE2_MIX | \
                                     APOLLO_MON_BIT_CUE2_MONO | APOLLO_MON_BIT_ALT1_TRIM | APOLLO_MON_BIT_ALT_SELECT | \
                                     APOLLO_MON_BIT_ALT1_ENABLE | APOLLO_MON_BIT_CUE_SEND | APOLLO_MON_BIT_CUE_PAN | \
                                     APOLLO_MON_BIT_CUE_MUTE)

static __inline unsigned long ApolloMonExtBit(unsigned long Param)
{
    switch (Param) {
    case APOLLO_MP_CUE1_MIX:     return APOLLO_MON_BIT_CUE1_MIX;
    case APOLLO_MP_CUE1_MONO:    return APOLLO_MON_BIT_CUE1_MONO;
    case APOLLO_MP_CUE2_MIX:     return APOLLO_MON_BIT_CUE2_MIX;
    case APOLLO_MP_CUE2_MONO:    return APOLLO_MON_BIT_CUE2_MONO;
    case APOLLO_MP_CUE3_MIX:     return APOLLO_MON_BIT_CUE3_MIX;
    case APOLLO_MP_CUE3_MONO:    return APOLLO_MON_BIT_CUE3_MONO;
    case APOLLO_MP_CUE4_MIX:     return APOLLO_MON_BIT_CUE4_MIX;
    case APOLLO_MP_CUE4_MONO:    return APOLLO_MON_BIT_CUE4_MONO;
    case APOLLO_MP_ALT1_TRIM:    return APOLLO_MON_BIT_ALT1_TRIM;
    case APOLLO_MP_ALT2_TRIM:    return APOLLO_MON_BIT_ALT2_TRIM;
    case APOLLO_MP_ALT_SELECT:   return APOLLO_MON_BIT_ALT_SELECT;
    case APOLLO_MP_ALT1_ENABLE:  return APOLLO_MON_BIT_ALT1_ENABLE;
    case APOLLO_MP_ALT2_ENABLE:  return APOLLO_MON_BIT_ALT2_ENABLE;
    default:
        break;
    }
    if (Param >= APOLLO_MP_MIX_FIRST && Param <= APOLLO_MP_MIX_LAST) {
        switch (APOLLO_MP_MIX_KIND(Param)) {
        case 0:  return APOLLO_MON_BIT_MIX_LEVEL;
        case 1:  return APOLLO_MON_BIT_MIX_PAN;
        case 2:  return APOLLO_MON_BIT_MIX_MUTE;
        case 3:  return APOLLO_MON_BIT_MIX_SOLO;
        default: return APOLLO_MON_BIT_MIX_MUTE_ALL;
        }
    }
    if (Param >= APOLLO_MP_AUX_FIRST && Param <= APOLLO_MP_AUX_LAST) {
        return APOLLO_MON_BIT_AUX;      /* [Aux] */
    }
    if (Param > 0x3FFFul) {
        return 0;
    }
    switch (APOLLO_MP_FAMILY(Param)) {
    case 0x1000ul: return APOLLO_MON_BIT_CUE_SEND;
    case 0x2000ul: return APOLLO_MON_BIT_CUE_PAN;
    case 0x3000ul: return APOLLO_MON_BIT_CUE_MUTE;
    default:       return 0;
    }
}

/*
 * [Out] The output codes have their own plan word (bringup_plan.h APOLLO_BRINGUP_PLAN.OutputParams): MonitorParams has
 * no free bit left. 0 = not an output code.
 */
#define APOLLO_MON_OUT_AUX_MONO     0x001ul
#define APOLLO_MON_OUT_CLIP_HOLD    0x002ul
#define APOLLO_MON_OUT_PEAK_HOLD    0x004ul
#define APOLLO_MON_OUT_LINE_PAD     0x008ul
#define APOLLO_MON_OUT_IDENTIFY     0x010ul
#define APOLLO_MON_OUT_DIG_MIRROR   0x020ul
#define APOLLO_MON_OUT_MON_REF      0x040ul
#define APOLLO_MON_OUT_FUNC_SWITCH  0x080ul
#define APOLLO_MON_OUT_ALL          0x0FFul

static __inline unsigned long ApolloMonOutBit(unsigned long Param)
{
    switch (Param) {
    case APOLLO_MP_AUX1_MONO:
    case APOLLO_MP_AUX2_MONO:   return APOLLO_MON_OUT_AUX_MONO;
    case APOLLO_MP_CLIP_HOLD:   return APOLLO_MON_OUT_CLIP_HOLD;
    case APOLLO_MP_PEAK_HOLD:   return APOLLO_MON_OUT_PEAK_HOLD;
    case APOLLO_MP_IDENTIFY:    return APOLLO_MON_OUT_IDENTIFY;
    case APOLLO_MP_DIG_MIRROR:  return APOLLO_MON_OUT_DIG_MIRROR;
    case APOLLO_MP_MON_REF:     return APOLLO_MON_OUT_MON_REF;
    case APOLLO_MP_FUNC_SWITCH: return APOLLO_MON_OUT_FUNC_SWITCH;
    default:
        return (Param >= APOLLO_MP_LINE_PAD(0) && Param <= APOLLO_MP_LINE_PAD_LAST) ? APOLLO_MON_OUT_LINE_PAD : 0ul;
    }
}

/* ------------------------------------------------------------------ */
/* Encoding                                                            */
/* ------------------------------------------------------------------ */

/* ApolloMonEncode results (mixer.c maps them as preamp_plan.h's) */
#define APOLLO_MON_OK               0   /* *W to the mailbox (SETTING) or to cues.c (SEND, PAN, MUTE) */
#define APOLLO_MON_BAD_VALUE        1   /* STATUS_INVALID_PARAMETER: a value out of range, a bus the model lacks */
#define APOLLO_MON_NOT_HERE         2   /* STATUS_NOT_SUPPORTED: a code, cue, ALT or headphone the model lacks */

#define APOLLO_MONW_SETTING         1   /* Setting, Value, Mask: one mailbox write */
#define APOLLO_MONW_SEND            2   /* Cue, Bus, Arg = level in half-dB (APOLLO_CUE_SEND_OFF = off) */
#define APOLLO_MONW_PAN             3   /* Cue, Bus, Arg = pan -100..100 */
#define APOLLO_MONW_MUTE            4   /* Cue, Arg = 0/1 */
/* [Mix] the monitor mix (cues.c): Bus, Arg; Buses = every bus the model mixes */
#define APOLLO_MONW_MIX_LEVEL       5   /* Arg = level in half-dB (APOLLO_MIX_LEVEL_OFF = off) */
#define APOLLO_MONW_MIX_PAN         6   /* Arg = pan -100..100 */
#define APOLLO_MONW_MIX_MUTE        7   /* Arg = 0/1 */
#define APOLLO_MONW_MIX_SOLO        8   /* Arg = 0/1 */
#define APOLLO_MONW_MIX_MUTE_ALL    9   /* Arg = 0/1, every bus in Buses */
/* [Aux] the aux buses (cues.c): Aux = 0/1, Bus = the input's bus (SEND, PAN, POST) or the cue (CUE, CUE_PAN), Arg */
#define APOLLO_MONW_AUX_SEND        10  /* Arg = half-dB (APOLLO_CUE_SEND_OFF = off) */
#define APOLLO_MONW_AUX_PAN         11  /* Arg = -100..100 */
#define APOLLO_MONW_AUX_POST        12  /* Arg = 0/1 */
#define APOLLO_MONW_AUX_RETURN      13  /* Arg = half-dB */
#define APOLLO_MONW_AUX_MUTE        14  /* Arg = 0/1 */
#define APOLLO_MONW_AUX_CUE         15  /* Arg = half-dB */
#define APOLLO_MONW_AUX_CUE_PAN     16  /* Arg = -100..100 */

typedef struct _APOLLO_MON_WRITE {
    int             Kind;               /* APOLLO_MONW_* */
    unsigned long   Setting, Value, Mask;
    unsigned long   Cue, Bus;
    long            Arg;
    unsigned long   Buses;              /* [Mix] the model's mixed buses (ApolloMonBusMask) */
    unsigned long   Aux;                /* [Aux] 0 = AUX 1, 1 = AUX 2 */
} APOLLO_MON_WRITE;

static __inline int ApolloMonIsMix(const APOLLO_MON_WRITE *W)
{
    return W->Kind >= APOLLO_MONW_MIX_LEVEL && W->Kind <= APOLLO_MONW_MIX_MUTE_ALL;
}

static __inline int ApolloMonIsAux(const APOLLO_MON_WRITE *W)
{
    return W->Kind >= APOLLO_MONW_AUX_SEND && W->Kind <= APOLLO_MONW_AUX_CUE_PAN;
}

static __inline int ApolloMonInRange(long V, long Lo, long Hi)
{
    return V >= Lo && V <= Hi;
}

static __inline int ApolloMonPut(APOLLO_MON_WRITE *W, unsigned long Setting, unsigned long Value, unsigned long Mask)
{
    W->Kind = APOLLO_MONW_SETTING;
    W->Setting = Setting;
    W->Value = Value & Mask;
    W->Mask = Mask;
    return APOLLO_MON_OK;
}

/* A 0/1 switch at Bit of Setting. */
static __inline int ApolloMonSwitch(APOLLO_MON_WRITE *W, long V, unsigned long Setting, unsigned long Bit)
{
    if (!ApolloMonInRange(V, 0, 1)) {
        return APOLLO_MON_BAD_VALUE;
    }
    return ApolloMonPut(W, Setting, V ? (1ul << Bit) : 0ul, 1ul << Bit);
}

/*
 * One SET_MONITOR_PARAM for model M. The ten codes that existed before give exactly the writes and
 * errors mixer.c gave (the x8p's monitor source now also takes CUE 3 and CUE 4).
 * [Virtual] Extra: the Virtual buses the unit's Input Strips mix (ApolloMonHasBusEx); mixer.c passes the unit's,
 * ApolloMonEncode none.
 */
static __inline int ApolloMonEncodeEx(const APOLLO_MON_MODEL *M, unsigned long Extra, unsigned long Param, long Value,
                                      APOLLO_MON_WRITE *W)
{
    unsigned long c, b;

    W->Kind = 0;
    W->Setting = W->Value = W->Mask = 0;
    W->Cue = W->Bus = 0;
    W->Arg = 0;
    W->Buses = 0;
    W->Aux = 0;
    if (M == 0) {
        return APOLLO_MON_NOT_HERE;
    }
    if (Param >= APOLLO_MP_AUX_FIRST && Param <= APOLLO_MP_AUX_LAST) {
        /* [Aux] both models have AUX 1 and 2; sends from the buses the model mixes, returns into its cues */
        c = APOLLO_MP_AUX_KIND(Param);
        b = Param & 0x1Ful;
        if ((Param & 0xC0ul) != 0) {
            return APOLLO_MON_NOT_HERE;
        }
        W->Aux = (Param >> 5) & 1ul;
        W->Bus = b;
        W->Arg = Value;
        switch (c) {
        case 0ul:
        case 1ul:
        case 2ul:
            if (!ApolloMonHasBusEx(M, Extra, b)) {
                return APOLLO_MON_BAD_VALUE;
            }
            if (c == 0ul && !ApolloMonInRange(Value, APOLLO_CUE_SEND_OFF, APOLLO_CUE_SEND_MAX)) {
                return APOLLO_MON_BAD_VALUE;
            }
            if (c == 1ul && !ApolloMonInRange(Value, -APOLLO_CUE_PAN_MAX, APOLLO_CUE_PAN_MAX)) {
                return APOLLO_MON_BAD_VALUE;
            }
            if (c == 2ul && !ApolloMonInRange(Value, 0, 1)) {
                return APOLLO_MON_BAD_VALUE;
            }
            W->Kind = (c == 0ul) ? APOLLO_MONW_AUX_SEND : (c == 1ul) ? APOLLO_MONW_AUX_PAN : APOLLO_MONW_AUX_POST;
            return APOLLO_MON_OK;
        case 3ul:
        case 4ul:
            if (b != 0) {
                return APOLLO_MON_NOT_HERE;
            }
            if (c == 3ul ? !ApolloMonInRange(Value, APOLLO_CUE_SEND_OFF, APOLLO_CUE_SEND_MAX) : !ApolloMonInRange(Value, 0, 1)) {
                return APOLLO_MON_BAD_VALUE;
            }
            W->Kind = (c == 3ul) ? APOLLO_MONW_AUX_RETURN : APOLLO_MONW_AUX_MUTE;
            return APOLLO_MON_OK;
        case 5ul:
        case 6ul:
            if (b >= M->Cues) {
                return APOLLO_MON_NOT_HERE;
            }
            if (c == 5ul ? !ApolloMonInRange(Value, APOLLO_CUE_SEND_OFF, APOLLO_CUE_SEND_MAX) :
                           !ApolloMonInRange(Value, -APOLLO_CUE_PAN_MAX, APOLLO_CUE_PAN_MAX)) {
                return APOLLO_MON_BAD_VALUE;
            }
            W->Kind = (c == 5ul) ? APOLLO_MONW_AUX_CUE : APOLLO_MONW_AUX_CUE_PAN;
            return APOLLO_MON_OK;
        default:
            return APOLLO_MON_NOT_HERE;
        }
    }
    if (Param >= APOLLO_MP_MIX_FIRST && Param <= APOLLO_MP_MIX_LAST) {
        /* [Mix] the monitor mix: per bus the model mixes; the talkback has no pan (one weight on both sides) */
        c = APOLLO_MP_MIX_KIND(Param);
        b = Param & 0xFFul;
        W->Buses = ApolloMonBusMask(M) | (Extra & APOLLO_MON_VIRTUAL_BUSES);    /* [Virtual] mute-all and solo cover them */
        if (c == 4ul) {
            if (b != 0) {
                return APOLLO_MON_NOT_HERE;
            }
            if (!ApolloMonInRange(Value, 0, 1)) {
                return APOLLO_MON_BAD_VALUE;
            }
            W->Kind = APOLLO_MONW_MIX_MUTE_ALL;
            W->Arg = Value;
            return APOLLO_MON_OK;
        }
        if (!ApolloMonHasBusEx(M, Extra, b)) {
            return APOLLO_MON_BAD_VALUE;
        }
        switch (c) {
        case 0ul:
            if (!ApolloMonInRange(Value, APOLLO_MIX_LEVEL_OFF, APOLLO_MIX_LEVEL_MAX)) {
                return APOLLO_MON_BAD_VALUE;
            }
            W->Kind = APOLLO_MONW_MIX_LEVEL;
            break;
        case 1ul:
            if (b == M->TalkbackBus) {
                return APOLLO_MON_NOT_HERE;
            }
            if (!ApolloMonInRange(Value, -APOLLO_MIX_PAN_MAX, APOLLO_MIX_PAN_MAX)) {
                return APOLLO_MON_BAD_VALUE;
            }
            W->Kind = APOLLO_MONW_MIX_PAN;
            break;
        default:        /* 2 mute, 3 solo */
            if (!ApolloMonInRange(Value, 0, 1)) {
                return APOLLO_MON_BAD_VALUE;
            }
            W->Kind = (c == 2ul) ? APOLLO_MONW_MIX_MUTE : APOLLO_MONW_MIX_SOLO;
            break;
        }
        W->Bus = b;
        W->Arg = Value;
        return APOLLO_MON_OK;
    }
    if (Param >= 0x1000ul && Param <= 0x3FFFul) {
        /* [Monitor] cue sends: per cue the model has, per bus it mixes */
        c = (Param >> 8) & 0xFul;
        b = Param & 0xFFul;
        if (c >= M->Cues) {
            return APOLLO_MON_NOT_HERE;
        }
        W->Cue = c;
        switch (APOLLO_MP_FAMILY(Param)) {
        case 0x1000ul:
            if (!ApolloMonHasBusEx(M, Extra, b)) {
                return APOLLO_MON_BAD_VALUE;
            }
            if (!ApolloMonInRange(Value, APOLLO_CUE_SEND_OFF, APOLLO_CUE_SEND_MAX)) {
                return APOLLO_MON_BAD_VALUE;
            }
            W->Kind = APOLLO_MONW_SEND;
            break;
        case 0x2000ul:
            if (!ApolloMonHasBusEx(M, Extra, b)) {
                return APOLLO_MON_BAD_VALUE;
            }
            if (!ApolloMonInRange(Value, -APOLLO_CUE_PAN_MAX, APOLLO_CUE_PAN_MAX)) {
                return APOLLO_MON_BAD_VALUE;
            }
            W->Kind = APOLLO_MONW_PAN;
            break;
        default:        /* 0x3000: a cue's mute, no bus */
            if (b != 0) {
                return APOLLO_MON_NOT_HERE;
            }
            if (!ApolloMonInRange(Value, 0, 1)) {
                return APOLLO_MON_BAD_VALUE;
            }
            W->Kind = APOLLO_MONW_MUTE;
            break;
        }
        W->Bus = b;
        W->Arg = Value;
        return APOLLO_MON_OK;
    }

    switch (Param) {
    /* ---- the codes mixer.c had (spec-dsp-mixer 6.1 C9-C12) ---- */
    case APOLLO_MP_LEVEL:
        if (!ApolloMonInRange(Value, 0, APOLLO_MP_LEVEL_MAX)) {
            return APOLLO_MON_BAD_VALUE;
        }
        return ApolloMonPut(W, 2, (unsigned long)Value, 0x000000FFul);
    case APOLLO_MP_HP1_LEVEL:
        if (!ApolloMonInRange(Value, 0, APOLLO_MP_LEVEL_MAX)) {
            return APOLLO_MON_BAD_VALUE;
        }
        return ApolloMonPut(W, 2, (unsigned long)Value << 8, 0x0000FF00ul);
    case APOLLO_MP_MUTE:
        return ApolloMonSwitch(W, Value, 2, 16);
    case APOLLO_MP_MONO:
        return ApolloMonSwitch(W, Value, 2, 17);
    case APOLLO_MP_SOURCE:
        /* bit29 = (v >> 2) & 1, [19:18] = v & 3 (ua_core.c); CUE 3 and 4 only where the model has them */
        if (!ApolloMonInRange(Value, 0, 4)) {
            return APOLLO_MON_BAD_VALUE;
        }
        if ((unsigned long)Value > M->SourceMax) {
            return APOLLO_MON_NOT_HERE;
        }
        return ApolloMonPut(W, 2, (((unsigned long)Value & 3ul) << 18) | ((((unsigned long)Value >> 2) & 1ul) << 29),
                            0x200C0000ul);
    case APOLLO_MP_DIM:
        return ApolloMonSwitch(W, Value, 2, 31);
    case APOLLO_MP_HP1_SOURCE:
    case APOLLO_MP_HP2_SOURCE:
        c = (Param == APOLLO_MP_HP1_SOURCE) ? 0ul : 1ul;
        if (c >= M->Hps) {
            return APOLLO_MON_NOT_HERE;     /* the Twin X has one headphone */
        }
        if (!ApolloMonInRange(Value, 0, 1)) {
            return APOLLO_MON_BAD_VALUE;    /* CUE 1 / CUE 2 only: the rest has no known encoding */
        }
        return ApolloMonPut(W, 12, (unsigned long)Value << (24ul + 3ul * c), 0x07000000ul << (3ul * c));
    case APOLLO_MP_DIM_LEVEL:
        if (!ApolloMonInRange(Value, 1, 7)) {
            return APOLLO_MON_BAD_VALUE;
        }
        return ApolloMonPut(W, 14, (unsigned long)Value << 28, 0x70000000ul);
    case APOLLO_MP_TALKBACK:
        return ApolloMonSwitch(W, Value, 13, 8);

    /* ---- [Monitor] cue outputs: MIX source and mono ---- */
    case APOLLO_MP_CUE1_MIX:
    case APOLLO_MP_CUE2_MIX:
    case APOLLO_MP_CUE3_MIX:
    case APOLLO_MP_CUE4_MIX:
        c = (Param == APOLLO_MP_CUE1_MIX) ? 0ul : (Param == APOLLO_MP_CUE2_MIX) ? 1ul :
            (Param == APOLLO_MP_CUE3_MIX) ? 2ul : 3ul;
        if (c >= M->Cues) {
            return APOLLO_MON_NOT_HERE;
        }
        if (!ApolloMonInRange(Value, 0, 1)) {
            return APOLLO_MON_BAD_VALUE;
        }
        /* kext value 0 = the cue's own mix, 2 = the monitor mix; fields at 24/26 (S2), 28/30 (S8) */
        b = 24ul + 2ul * (c & 1ul) + ((c >= 2ul) ? 4ul : 0ul);
        return ApolloMonPut(W, (c < 2ul) ? 2ul : 8ul, Value ? (2ul << b) : 0ul, 3ul << b);
    case APOLLO_MP_CUE1_MONO:
    case APOLLO_MP_CUE2_MONO:
    case APOLLO_MP_CUE3_MONO:
    case APOLLO_MP_CUE4_MONO:
        c = (Param == APOLLO_MP_CUE1_MONO) ? 0ul : (Param == APOLLO_MP_CUE2_MONO) ? 1ul :
            (Param == APOLLO_MP_CUE3_MONO) ? 2ul : 3ul;
        if (c >= M->Cues) {
            return APOLLO_MON_NOT_HERE;
        }
        if (!ApolloMonInRange(Value, 0, 1)) {
            return APOLLO_MON_BAD_VALUE;
        }
        /* swapped 2-bit fields at 20/22 (S2), 24/26 (S8): value bit 0 (mono) is the upper bit */
        b = 20ul + 2ul * (c & 1ul) + ((c >= 2ul) ? 4ul : 0ul);
        return ApolloMonPut(W, (c < 2ul) ? 2ul : 8ul, Value ? (2ul << b) : 0ul, 3ul << b);

    /* ---- [Monitor] ALT speakers ---- */
    case APOLLO_MP_ALT1_TRIM:
    case APOLLO_MP_ALT2_TRIM:
        c = (Param == APOLLO_MP_ALT1_TRIM) ? 0ul : 1ul;
        if (c >= M->Alts) {
            return APOLLO_MON_NOT_HERE;
        }
        if (!ApolloMonInRange(Value, 0, APOLLO_MP_ALT_TRIM_MAX)) {
            return APOLLO_MON_BAD_VALUE;
        }
        return ApolloMonPut(W, 15, (unsigned long)Value << (16ul * c), 0x7Ful << (16ul * c));
    case APOLLO_MP_ALT1_ENABLE:
    case APOLLO_MP_ALT2_ENABLE:
        c = (Param == APOLLO_MP_ALT1_ENABLE) ? 0ul : 1ul;
        if (c >= M->Alts) {
            return APOLLO_MON_NOT_HERE;
        }
        return ApolloMonSwitch(W, Value, 15, c ? 12ul : 8ul);
    case APOLLO_MP_ALT_SELECT:
        if (!ApolloMonInRange(Value, 0, 2)) {
            return APOLLO_MON_BAD_VALUE;
        }
        if ((unsigned long)Value > M->Alts) {
            return APOLLO_MON_NOT_HERE;     /* ALT 2 on the Twin X */
        }
        return ApolloMonPut(W, 13, (unsigned long)Value << 28, 0x30000000ul);

    /* ---- [Out] output and front-panel settings ---- */
    case APOLLO_MP_AUX1_MONO:
    case APOLLO_MP_AUX2_MONO:
        /* swapped 2-bit field [9:8] (ua_swap2): mono (value bit 0) is bit 9 */
        if (!ApolloMonInRange(Value, 0, 1)) {
            return APOLLO_MON_BAD_VALUE;
        }
        return ApolloMonPut(W, (Param == APOLLO_MP_AUX1_MONO) ? 6ul : 7ul, Value ? (1ul << 9) : 0ul, 0x300ul);
    case APOLLO_MP_CLIP_HOLD:
        if (!ApolloMonInRange(Value, 0, 5)) {
            return APOLLO_MON_BAD_VALUE;
        }
        return ApolloMonPut(W, 1, (unsigned long)Value << 28, 0x70000000ul);
    case APOLLO_MP_PEAK_HOLD:
        if (!ApolloMonInRange(Value, 0, 4)) {
            return APOLLO_MON_BAD_VALUE;
        }
        return ApolloMonPut(W, 1, (unsigned long)Value << 13, 0x0000E000ul);
    case APOLLO_MP_IDENTIFY:
        if (!ApolloMonInRange(Value, 0, 7)) {
            return APOLLO_MON_BAD_VALUE;
        }
        return ApolloMonPut(W, 6, (unsigned long)Value << 11, 0x0000F800ul);
    case APOLLO_MP_DIG_MIRROR:
        if (!M->DigMirror) {
            return APOLLO_MON_NOT_HERE;
        }
        return ApolloMonSwitch(W, Value, 6, 10);
    case APOLLO_MP_MON_REF:
        if (!M->MonRef) {
            return APOLLO_MON_NOT_HERE;
        }
        return ApolloMonSwitch(W, Value, 11, 4);
    case APOLLO_MP_FUNC_SWITCH:
        if (!M->FuncSwitch) {
            return APOLLO_MON_NOT_HERE;
        }
        if (!ApolloMonInRange(Value, 0, 4) || Value == 1) {
            return APOLLO_MON_BAD_VALUE;    /* 1 = "alt 2": not in the x8p device map's list */
        }
        return ApolloMonPut(W, 13, (unsigned long)Value << 13, 0x0000E000ul);
    default:
        if (Param >= APOLLO_MP_LINE_PAD(0) && Param <= APOLLO_MP_LINE_PAD_LAST) {
            c = Param - APOLLO_MP_LINE_PAD(0);
            if (c >= M->LinePads) {
                return APOLLO_MON_NOT_HERE;
            }
            return ApolloMonSwitch(W, Value, 0, 28ul + c);
        }
        return APOLLO_MON_NOT_HERE;
    }
}

/* One SET_MONITOR_PARAM for model M on a unit without Virtual strips (every code and error as before [Virtual]). */
static __inline int ApolloMonEncode(const APOLLO_MON_MODEL *M, unsigned long Param, long Value, APOLLO_MON_WRITE *W)
{
    return ApolloMonEncodeEx(M, 0ul, Param, Value, W);
}

/* ------------------------------------------------------------------ */
/* Cue send coefficients                                               */
/* ------------------------------------------------------------------ */

typedef char ApolloMonFloatIs32Bits[(sizeof(float) == sizeof(unsigned long)) ? 1 : -1];

/* IEEE single bits of 10^(dB/20)/sqrt2 for half-dB H (-120..+24); 0 = off (generated, checked by mon-plan-test). */
static __inline unsigned long ApolloMonLevelBits(long H)
{
    static const unsigned long t[145] = {
        0x3A395D22ul, 0x3A4458E8ul, 0x3A4FFB4Aul, 0x3A5C4E27ul, 0x3A695BF4ul, 0x3A772FC5ul, 0x3A82EAAAul, 0x3A8AAC89ul,
        0x3A92E413ul, 0x3A9B9841ul, 0x3AA4D077ul, 0x3AAE9486ul, 0x3AB8ECB9ul, 0x3AC3E1D7ul, 0x3ACF7D2Aul, 0x3ADBC88Eul,
        0x3AE8CE71ul, 0x3AF699DFul, 0x3B029B46ul, 0x3B0A5871ul, 0x3B128AFFul, 0x3B1B39E6ul, 0x3B246C84ul, 0x3B2E2AA8ul,
        0x3B387C95ul, 0x3B436B0Dul, 0x3B4EFF57ul, 0x3B5B4346ul, 0x3B684143ul, 0x3B760453ul, 0x3B824C12ul, 0x3B8A048Bul,
        0x3B923221ul, 0x3B9ADBC4ul, 0x3BA408CFul, 0x3BADC109ul, 0x3BB80CB4ul, 0x3BC2F48Bul, 0x3BCE81D0ul, 0x3BDABE4Ful,
        0x3BE7B46Aul, 0x3BF56F22ul, 0x3C01FD0Ful, 0x3C09B0D9ul, 0x3C11D979ul, 0x3C1A7DDBul, 0x3C23A555ul, 0x3C2D57ABul,
        0x3C379D18ul, 0x3C427E52ul, 0x3C4E0495ul, 0x3C5A39A8ul, 0x3C6727E8ul, 0x3C74DA4Cul, 0x3C81AE3Bul, 0x3C895D59ul,
        0x3C918106ul, 0x3C9A202Bul, 0x3CA34218ul, 0x3CACEE8Dul, 0x3CB72DBFul, 0x3CC20860ul, 0x3CCD87A6ul, 0x3CD9B552ul,
        0x3CE69BBAul, 0x3CF445D0ul, 0x3D015F96ul, 0x3D090A0Cul, 0x3D1128CAul, 0x3D19C2B4ul, 0x3D22DF17ul, 0x3D2C85AEul,
        0x3D36BEA9ul, 0x3D4192B5ul, 0x3D4D0B02ul, 0x3D59314Cul, 0x3D660FE1ul, 0x3D73B1AEul, 0x3D811122ul, 0x3D88B6F1ul,
        0x3D90D0C3ul, 0x3D996576ul, 0x3DA27C52ul, 0x3DAC1D0Ful, 0x3DB64FD7ul, 0x3DC11D52ul, 0x3DCC8EAAul, 0x3DD8AD96ul,
        0x3DE5845Dul, 0x3DF31DE6ul, 0x3E00C2DDul, 0x3E086409ul, 0x3E1078F1ul, 0x3E190870ul, 0x3E2219C9ul, 0x3E2BB4AFul,
        0x3E35E148ul, 0x3E40A836ul, 0x3E4C129Eul, 0x3E582A30ul, 0x3E64F92Eul, 0x3E728A77ul, 0x3E8074C7ul, 0x3E881153ul,
        0x3E902154ul, 0x3E98ABA2ul, 0x3EA1B77Cul, 0x3EAB4C8Ful, 0x3EB572FCul, 0x3EC03361ul, 0x3ECB96DDul, 0x3ED7A719ul,
        0x3EE46E53ul, 0x3EF1F762ul, 0x3F0026E1ul, 0x3F07BECFul, 0x3F0FC9EDul, 0x3F184F0Dul, 0x3F21556Aul, 0x3F2AE4AEul,
        0x3F3504F3ul, 0x3F3FBED3ul, 0x3F4B1B67ul, 0x3F572453ul, 0x3F63E3CDul, 0x3F7164A6ul, 0x3F7FB255ul, 0x3F876C7Eul,
        0x3F8F72BAul, 0x3F97F2B0ul, 0x3FA0F394ul, 0x3FAA7D0Bul, 0x3FB4972Dul, 0x3FBF4A8Cul, 0x3FCAA03Bul, 0x3FD6A1DBul,
        0x3FE3599Aul, 0x3FF0D243ul, 0x3FFF1745ul, 0x40071A5Eul, 0x400F1BBDul, 0x4017968Bul, 0x402091F9ul, 0x402A15A8ul,
        0x403429A9ul,
    };

    if (H < APOLLO_CUE_SEND_MIN || H > APOLLO_CUE_SEND_MAX) {
        return 0;
    }
    return t[H - APOLLO_CUE_SEND_MIN];
}

/* IEEE single bits of sqrt2 * cos((P/100 + 1) * pi/4), P -100..100: the left weight; the right one is P's mirror. */
static __inline unsigned long ApolloMonPanBits(long P)
{
    static const unsigned long t[201] = {
        0x3FB504F3ul, 0x3FB50385ul, 0x3FB4FF3Cul, 0x3FB4F816ul, 0x3FB4EE15ul, 0x3FB4E139ul, 0x3FB4D181ul, 0x3FB4BEEFul,
        0x3FB4A982ul, 0x3FB4913Aul, 0x3FB47619ul, 0x3FB4581Eul, 0x3FB4374Aul, 0x3FB4139Dul, 0x3FB3ED18ul, 0x3FB3C3BCul,
        0x3FB3978Aul, 0x3FB36881ul, 0x3FB336A3ul, 0x3FB301F1ul, 0x3FB2CA6Bul, 0x3FB29012ul, 0x3FB252E7ul, 0x3FB212EBul,
        0x3FB1D020ul, 0x3FB18A86ul, 0x3FB1421Eul, 0x3FB0F6E9ul, 0x3FB0A8E9ul, 0x3FB0581Ful, 0x3FB0048Cul, 0x3FAFAE31ul,
        0x3FAF5510ul, 0x3FAEF92Bul, 0x3FAE9A82ul, 0x3FAE3917ul, 0x3FADD4ECul, 0x3FAD6E02ul, 0x3FAD045Bul, 0x3FAC97F9ul,
        0x3FAC28DDul, 0x3FABB709ul, 0x3FAB427Ful, 0x3FAACB40ul, 0x3FAA514Ful, 0x3FA9D4AEul, 0x3FA9555Dul, 0x3FA8D361ul,
        0x3FA84EBAul, 0x3FA7C76Aul, 0x3FA73D75ul, 0x3FA6B0DBul, 0x3FA6219Ful, 0x3FA58FC4ul, 0x3FA4FB4Bul, 0x3FA46438ul,
        0x3FA3CA8Cul, 0x3FA32E49ul, 0x3FA28F73ul, 0x3FA1EE0Cul, 0x3FA14A17ul, 0x3FA0A395ul, 0x3F9FFA8Aul, 0x3F9F4EF8ul,
        0x3F9EA0E2ul, 0x3F9DF04Bul, 0x3F9D3D36ul, 0x3F9C87A4ul, 0x3F9BCF9Aul, 0x3F9B151Bul, 0x3F9A5828ul, 0x3F9998C5ul,
        0x3F98D6F5ul, 0x3F9812BCul, 0x3F974C1Bul, 0x3F968317ul, 0x3F95B7B3ul, 0x3F94E9F1ul, 0x3F9419D6ul, 0x3F934763ul,
        0x3F92729Eul, 0x3F919B88ul, 0x3F90C226ul, 0x3F8FE67Aul, 0x3F8F0889ul, 0x3F8E2855ul, 0x3F8D45E3ul, 0x3F8C6136ul,
        0x3F8B7A51ul, 0x3F8A9138ul, 0x3F89A5EFul, 0x3F88B87Aul, 0x3F87C8DCul, 0x3F86D719ul, 0x3F85E335ul, 0x3F84ED34ul,
        0x3F83F519ul, 0x3F82FAE9ul, 0x3F81FEA8ul, 0x3F810059ul, 0x3F800000ul, 0x3F7DFB44ul, 0x3F7BF285ul, 0x3F79E5CCul,
        0x3F77D520ul, 0x3F75C08Bul, 0x3F73A814ul, 0x3F718BC4ul, 0x3F6F6BA4ul, 0x3F6D47BCul, 0x3F6B2014ul, 0x3F68F4B6ul,
        0x3F66C5ABul, 0x3F6492FAul, 0x3F625CADul, 0x3F6022CEul, 0x3F5DE564ul, 0x3F5BA479ul, 0x3F596016ul, 0x3F571845ul,
        0x3F54CD0Eul, 0x3F527E7Bul, 0x3F502C94ul, 0x3F4DD765ul, 0x3F4B7EF5ul, 0x3F49234Eul, 0x3F46C47Aul, 0x3F446283ul,
        0x3F41FD72ul, 0x3F3F9551ul, 0x3F3D2A29ul, 0x3F3ABC04ul, 0x3F384AEDul, 0x3F35D6EDul, 0x3F33600Dul, 0x3F30E658ul,
        0x3F2E69D9ul, 0x3F2BEA98ul, 0x3F2968A0ul, 0x3F26E3FBul, 0x3F245CB4ul, 0x3F21D2D4ul, 0x3F1F4666ul, 0x3F1CB774ul,
        0x3F1A2609ul, 0x3F17922Eul, 0x3F14FBEFul, 0x3F126355ul, 0x3F0FC86Cul, 0x3F0D2B3Dul, 0x3F0A8BD4ul, 0x3F07EA3Aul,
        0x3F05467Cul, 0x3F02A0A2ul, 0x3EFFF171ul, 0x3EFA9D93ul, 0x3EF545C0ul, 0x3EEFEA0Dul, 0x3EEA8A90ul, 0x3EE52760ul,
        0x3EDFC090ul, 0x3EDA5639ul, 0x3ED4E86Ful, 0x3ECF7748ul, 0x3ECA02DAul, 0x3EC48B3Cul, 0x3EBF1083ul, 0x3EB992C6ul,
        0x3EB4121Aul, 0x3EAE8E97ul, 0x3EA90852ul, 0x3EA37F62ul, 0x3E9DF3DCul, 0x3E9865D8ul, 0x3E92D56Cul, 0x3E8D42AFul,
        0x3E87ADB6ul, 0x3E821699ul, 0x3E78FADDul, 0x3E6DC498ul, 0x3E628A93ul, 0x3E574CFAul, 0x3E4C0BFAul, 0x3E40C7C1ul,
        0x3E35807Dul, 0x3E2A365Bul, 0x3E1EE98Aul, 0x3E139A35ul, 0x3E08488Dul, 0x3DF9E97Aul, 0x3DE33DE8ul, 0x3DCC8EBFul,
        0x3DB5DC5Bul, 0x3D9F2719ul, 0x3D886F53ul, 0x3D636ACAul, 0x3D35F357ul, 0x3D087905ul, 0x3CB5F917ul, 0x3C35FA86ul,
        0x00000000ul,       /* hard right: cos(pi/2), exactly 0 (double gives 8.7e-17) */
    };

    if (P < -APOLLO_CUE_PAN_MAX || P > APOLLO_CUE_PAN_MAX) {
        return 0;
    }
    return t[P + APOLLO_CUE_PAN_MAX];
}

/* A x B of two IEEE singles, multiplied in double and rounded once (the engine computes in double). */
static __inline unsigned long ApolloMonF32Mul(unsigned long A, unsigned long B)
{
    union { float F; unsigned long U; } a, b, r;

    a.U = A;
    b.U = B;
    r.F = (float)((double)a.F * (double)b.F);
    return r.U;
}

/*
 * The left and right weights of one send: level H half-dB (APOLLO_CUE_SEND_OFF = off), pan P; a muted cue
 * sends 0.0. Centre pan gives both sides exactly the level value, which is what the engine writes for a
 * send without pan; 0 dB centre = 0x3F3504F3.
 */
static __inline void ApolloMonSendCoef(long H, long P, int Muted, unsigned long *Left, unsigned long *Right)
{
    unsigned long lin = ApolloMonLevelBits(H);

    *Left = 0;
    *Right = 0;
    if (Muted || lin == 0 || P < -APOLLO_CUE_PAN_MAX || P > APOLLO_CUE_PAN_MAX) {
        return;
    }
    *Left = ApolloMonF32Mul(lin, ApolloMonPanBits(P));
    *Right = ApolloMonF32Mul(lin, ApolloMonPanBits(-P));
}

/*
 * [Mix] IEEE single bits of 10^(dB/20) x 3.976353 for half-dB H (-120..+24): the talkback bus's monitor-mix
 * weight, both sides (oa:mixer-engine/hardware.py TALKBACK_UNITY, DTrace-verified); 0 = off. Generated, checked
 * by mon-plan-test; 0 dB = 0x407E7C91 = 3.976353.
 */
static __inline unsigned long ApolloMixTalkbackBits(long H)
{
    static const unsigned long t[145] = {
        0x3B824C11ul, 0x3B8A048Aul, 0x3B92321Ful, 0x3B9ADBC2ul, 0x3BA408CDul, 0x3BADC108ul, 0x3BB80CB2ul, 0x3BC2F489ul,
        0x3BCE81CEul, 0x3BDABE4Cul, 0x3BE7B468ul, 0x3BF56F20ul, 0x3C01FD0Dul, 0x3C09B0D7ul, 0x3C11D977ul, 0x3C1A7DDAul,
        0x3C23A553ul, 0x3C2D57A9ul, 0x3C379D16ul, 0x3C427E50ul, 0x3C4E0492ul, 0x3C5A39A6ul, 0x3C6727E5ul, 0x3C74DA4Aul,
        0x3C81AE39ul, 0x3C895D57ul, 0x3C918105ul, 0x3C9A202Aul, 0x3CA34216ul, 0x3CACEE8Bul, 0x3CB72DBDul, 0x3CC2085Eul,
        0x3CCD87A3ul, 0x3CD9B54Ful, 0x3CE69BB8ul, 0x3CF445CEul, 0x3D015F95ul, 0x3D090A0Aul, 0x3D1128C8ul, 0x3D19C2B2ul,
        0x3D22DF15ul, 0x3D2C85ACul, 0x3D36BEA7ul, 0x3D4192B3ul, 0x3D4D0B00ul, 0x3D593149ul, 0x3D660FDFul, 0x3D73B1ACul,
        0x3D811120ul, 0x3D88B6F0ul, 0x3D90D0C1ul, 0x3D996574ul, 0x3DA27C50ul, 0x3DAC1D0Dul, 0x3DB64FD5ul, 0x3DC11D50ul,
        0x3DCC8EA8ul, 0x3DD8AD94ul, 0x3DE5845Bul, 0x3DF31DE3ul, 0x3E00C2DCul, 0x3E086408ul, 0x3E1078EFul, 0x3E19086Eul,
        0x3E2219C7ul, 0x3E2BB4ADul, 0x3E35E146ul, 0x3E40A834ul, 0x3E4C129Cul, 0x3E582A2Dul, 0x3E64F92Cul, 0x3E728A75ul,
        0x3E8074C6ul, 0x3E881152ul, 0x3E902153ul, 0x3E98ABA0ul, 0x3EA1B77Aul, 0x3EAB4C8Dul, 0x3EB572FAul, 0x3EC0335Ful,
        0x3ECB96DBul, 0x3ED7A717ul, 0x3EE46E51ul, 0x3EF1F760ul, 0x3F0026E0ul, 0x3F07BECEul, 0x3F0FC9EBul, 0x3F184F0Bul,
        0x3F215569ul, 0x3F2AE4ACul, 0x3F3504F1ul, 0x3F3FBED1ul, 0x3F4B1B65ul, 0x3F572450ul, 0x3F63E3CAul, 0x3F7164A4ul,
        0x3F7FB252ul, 0x3F876C7Cul, 0x3F8F72B9ul, 0x3F97F2AEul, 0x3FA0F392ul, 0x3FAA7D09ul, 0x3FB4972Bul, 0x3FBF4A89ul,
        0x3FCAA039ul, 0x3FD6A1D9ul, 0x3FE35998ul, 0x3FF0D241ul, 0x3FFF1743ul, 0x40071A5Cul, 0x400F1BBBul, 0x40179689ul,
        0x402091F8ul, 0x402A15A6ul, 0x403429A7ul, 0x403ED689ul, 0x404A2559ul, 0x40561FB0ul, 0x4062CFB9ul, 0x40704037ul,
        0x407E7C91ul, 0x4086C86Eul, 0x408EC4F3ul, 0x40973A9Cul, 0x40A03098ul, 0x40A9AE81ul, 0x40B3BC66ul, 0x40BE62CEul,
        0x40C9AAC3ul, 0x40D59DD7ul, 0x40E2462Eul, 0x40EFAE85ul, 0x40FDE23Eul, 0x410676B2ul, 0x410E6E5Eul, 0x4116DEE6ul,
        0x411FCF73ul, 0x4129479Bul, 0x41334F67ul, 0x413DEF5Aul, 0x41493077ul, 0x41551C4Cul, 0x4161BCF6ul, 0x416F1D2Cul,
        0x417D4848ul,
    };

    if (H < APOLLO_CUE_SEND_MIN || H > APOLLO_CUE_SEND_MAX) {
        return 0;
    }
    return t[H - APOLLO_CUE_SEND_MIN];
}

/* ------------------------------------------------------------------ */
/* [Mix] Monitor mix state (inside APOLLO_CUE_STATE, DevLock)          */
/* ------------------------------------------------------------------ */

/*
 * Zero = UA's default and nothing set: every bus at 0 dB, centre, not muted, not soloed, and Set empty, so a
 * zeroed device context sends nothing (no replay, no bus move). Set grows only with what the host set: a bus
 * whose weights went out once is re-sent after every connect and when its strip moves it to another DSP.
 */
typedef struct _APOLLO_MIX_STATE {
    unsigned long   Set;                        /* bit b: bus b's monitor-mix weights went out at least once */
    unsigned long   Muted;                      /* bit b: muted in the monitor mix (the host's own mute) */
    unsigned long   Soloed;                     /* bit b: soloed; while any bus is, every other bus goes out as 0.0 */
    signed char     Level[APOLLO_MON_BUSES];    /* half-dB, 0 = 0 dB, APOLLO_MIX_LEVEL_OFF = off (-inf) */
    signed char     Pan[APOLLO_MON_BUSES];      /* -100..100, 0 = centre */
} APOLLO_MIX_STATE;

/* ------------------------------------------------------------------ */
/* [Aux] Aux bus state (inside APOLLO_CUE_STATE, DevLock)              */
/* ------------------------------------------------------------------ */

/*
 * Zero = nothing set: every send off and pre-fader, centre, every return at 0 dB unmuted, no aux-to-cue send, the
 * returns on DSP 0; a zeroed device context sends nothing. Level = half-dB + 122 (2..146), 0 = off; RetLevel is
 * half-dB itself (0 = 0 dB, APOLLO_CUE_SEND_OFF = off), as the monitor mix's.
 */
typedef struct _APOLLO_AUX_STATE {
    unsigned long   Set[APOLLO_MON_AUX_BUSES];                  /* bit b: bus b's send to aux a went out */
    unsigned long   Post[APOLLO_MON_AUX_BUSES];                 /* bit b: that send is post-fader */
    unsigned char   Level[APOLLO_MON_AUX_BUSES][APOLLO_MON_BUSES];
    signed char     Pan[APOLLO_MON_AUX_BUSES][APOLLO_MON_BUSES];
    unsigned long   RetSet;                                     /* bit a: aux a's return went out */
    unsigned long   RetMuted;                                   /* bit a */
    signed char     RetLevel[APOLLO_MON_AUX_BUSES];
    unsigned long   CueSet[APOLLO_MON_AUX_BUSES];               /* bit c: aux a's send to cue c went out */
    unsigned char   CueLevel[APOLLO_MON_AUX_BUSES][APOLLO_MON_CUES_MAX];
    signed char     CuePan[APOLLO_MON_AUX_BUSES][APOLLO_MON_CUES_MAX];
    unsigned long   Owner[APOLLO_MON_AUX_BUSES];                /* the DSP an aux insert chain runs the return on (0 = none) */
} APOLLO_AUX_STATE;

/* ------------------------------------------------------------------ */
/* Cue send state (cues.c keeps one per device, DevLock)               */
/* ------------------------------------------------------------------ */

/*
 * Zero = nothing set: every level off, every pan centre, no cue muted, so a zeroed device context
 * needs no init. Level[c][b] = half-dB + 122 (2..146), 0 = off.
 */
typedef struct _APOLLO_CUE_STATE {
    unsigned long   Set[APOLLO_MON_CUES_MAX];   /* bit b: cue c's send of bus b went out at least once */
    unsigned long   Muted;                      /* bit c: cue c's sends go out as 0.0 */
    unsigned char   Level[APOLLO_MON_CUES_MAX][APOLLO_MON_BUSES];
    signed char     Pan[APOLLO_MON_CUES_MAX][APOLLO_MON_BUSES];
    unsigned long   Entries;                    /* ring entries queued */
    unsigned long   Busy;                       /* sends a full ring turned away */
    unsigned long   Replays;                    /* cells re-sent after a connect or a bus move */
    APOLLO_MIX_STATE Mix;                       /* [Mix] the monitor mix (sub 0), same rings and rules */
    APOLLO_AUX_STATE Aux;                       /* [Aux] the aux buses (subs 3/4, the returns), same rings and rules */
} APOLLO_CUE_STATE;

static __inline long ApolloCueLevel(const APOLLO_CUE_STATE *S, unsigned long Cue, unsigned long Bus)
{
    unsigned char v;

    if (Cue >= APOLLO_MON_CUES_MAX || Bus >= APOLLO_MON_BUSES) {
        return APOLLO_CUE_SEND_OFF;
    }
    v = S->Level[Cue][Bus];
    return (v == 0) ? APOLLO_CUE_SEND_OFF : (long)v - 122L;
}

/* Records a SEND, PAN or MUTE write (already validated by ApolloMonEncode). */
static __inline void ApolloCueStore(APOLLO_CUE_STATE *S, const APOLLO_MON_WRITE *W)
{
    if (W->Cue >= APOLLO_MON_CUES_MAX) {
        return;
    }
    if (W->Kind == APOLLO_MONW_MUTE) {
        if (W->Arg) {
            S->Muted |= 1ul << W->Cue;
        } else {
            S->Muted &= ~(1ul << W->Cue);
        }
        return;
    }
    if (W->Bus >= APOLLO_MON_BUSES) {
        return;
    }
    if (W->Kind == APOLLO_MONW_SEND) {
        S->Level[W->Cue][W->Bus] = (W->Arg <= APOLLO_CUE_SEND_OFF || W->Arg > APOLLO_CUE_SEND_MAX) ? 0 :
                                   (unsigned char)(W->Arg + 122L);
    } else if (W->Kind == APOLLO_MONW_PAN) {
        S->Pan[W->Cue][W->Bus] = (signed char)((W->Arg < -APOLLO_CUE_PAN_MAX) ? -APOLLO_CUE_PAN_MAX :
                                               (W->Arg > APOLLO_CUE_PAN_MAX) ? APOLLO_CUE_PAN_MAX : W->Arg);
    }
}

/* The two weights cue Cue's send of bus Bus has now (its level, pan and the cue's mute). */
static __inline void ApolloCueCoef(const APOLLO_CUE_STATE *S, unsigned long Cue, unsigned long Bus,
                                   unsigned long *Left, unsigned long *Right)
{
    *Left = 0;
    *Right = 0;
    if (Cue >= APOLLO_MON_CUES_MAX || Bus >= APOLLO_MON_BUSES) {
        return;
    }
    ApolloMonSendCoef(ApolloCueLevel(S, Cue, Bus), (long)S->Pan[Cue][Bus], (int)((S->Muted >> Cue) & 1ul), Left, Right);
}

/* ------------------------------------------------------------------ */
/* [Mix] Monitor mix: weights, store, what a write sends               */
/* ------------------------------------------------------------------ */

static __inline long ApolloMixLevel(const APOLLO_MIX_STATE *S, unsigned long Bus)
{
    return (Bus < APOLLO_MON_BUSES) ? (long)S->Level[Bus] : APOLLO_MIX_LEVEL_OFF;
}

/*
 * Bus goes out as 0.0: muted, or some bus is soloed and this one is not. Solo is host logic (as UA's Mixer
 * Engine, _handle_solo_recalc): the DSP has no solo, the driver keeps the host's own mutes apart from the solo,
 * so ending a solo gives every bus back exactly what was set. Mute wins over solo.
 */
static __inline int ApolloMixSilent(const APOLLO_MIX_STATE *S, unsigned long Bus)
{
    if (Bus >= APOLLO_MON_BUSES || ((S->Muted >> Bus) & 1ul) != 0) {
        return 1;
    }
    return S->Soloed != 0 && ((S->Soloed >> Bus) & 1ul) == 0;
}

/* Bus's two monitor-mix weights now: level x pan (the talkback: its own law, L = R, no pan); 0.0 when silent. */
static __inline void ApolloMixCoef(const APOLLO_MIX_STATE *S, unsigned long Bus, unsigned long *Left, unsigned long *Right)
{
    *Left = 0;
    *Right = 0;
    if (Bus >= APOLLO_MON_BUSES || ApolloMixSilent(S, Bus)) {
        return;
    }
    if (Bus == APOLLO_MON_TALKBACK_BUS) {
        *Left = ApolloMixTalkbackBits(ApolloMixLevel(S, Bus));
        *Right = *Left;
        return;
    }
    ApolloMonSendCoef(ApolloMixLevel(S, Bus), (long)S->Pan[Bus], 0, Left, Right);
}

/* Records a MIX write (already validated by ApolloMonEncode). Set is the caller's: ApolloMixTargets. */
static __inline void ApolloMixStore(APOLLO_MIX_STATE *S, const APOLLO_MON_WRITE *W)
{
    unsigned long bit;

    if (W->Kind == APOLLO_MONW_MIX_MUTE_ALL) {
        if (W->Arg) {
            S->Muted |= W->Buses;
        } else {
            S->Muted &= ~W->Buses;
        }
        return;
    }
    if (W->Bus >= APOLLO_MON_BUSES) {
        return;
    }
    bit = 1ul << W->Bus;
    switch (W->Kind) {
    case APOLLO_MONW_MIX_LEVEL:
        S->Level[W->Bus] = (signed char)((W->Arg < APOLLO_MIX_LEVEL_OFF) ? APOLLO_MIX_LEVEL_OFF :
                                         (W->Arg > APOLLO_MIX_LEVEL_MAX) ? APOLLO_MIX_LEVEL_MAX : W->Arg);
        break;
    case APOLLO_MONW_MIX_PAN:
        S->Pan[W->Bus] = (signed char)((W->Arg < -APOLLO_MIX_PAN_MAX) ? -APOLLO_MIX_PAN_MAX :
                                       (W->Arg > APOLLO_MIX_PAN_MAX) ? APOLLO_MIX_PAN_MAX : W->Arg);
        break;
    case APOLLO_MONW_MIX_MUTE:
        S->Muted = W->Arg ? (S->Muted | bit) : (S->Muted & ~bit);
        break;
    case APOLLO_MONW_MIX_SOLO:
        S->Soloed = W->Arg ? (S->Soloed | bit) : (S->Soloed & ~bit);
        break;
    default:
        break;
    }
}

/*
 * The buses a MIX write sends, from the state Before and After it:
 *   - the bus a level, pan or mute names, always (the host set it: from then on the driver's weights are the
 *     unit's, even where they equal what the unit had);
 *   - every bus of the model on a mute-all (1): the safety button re-sends 0.0 everywhere;
 *   - any other bus of the model whose weights changed: a solo silences or brings back the others, an
 *     unmute-all brings back what is no longer muted. A solo itself sends only what changed, so soloing a bus
 *     never touches the soloed bus, and a bus nobody set is only ever sent when its weights really change.
 */
static __inline unsigned long ApolloMixTargets(const APOLLO_MIX_STATE *Before, const APOLLO_MIX_STATE *After,
                                               const APOLLO_MON_WRITE *W)
{
    unsigned long t = 0, b, l0, r0, l1, r1;

    switch (W->Kind) {
    case APOLLO_MONW_MIX_LEVEL:
    case APOLLO_MONW_MIX_PAN:
    case APOLLO_MONW_MIX_MUTE:
        if (W->Bus < APOLLO_MON_BUSES) {
            t |= 1ul << W->Bus;
        }
        break;
    case APOLLO_MONW_MIX_MUTE_ALL:
        if (W->Arg) {
            t |= W->Buses;
        }
        break;
    default:
        break;
    }
    for (b = 0; b < APOLLO_MON_BUSES; b++) {
        if (((W->Buses >> b) & 1ul) == 0) {
            continue;
        }
        ApolloMixCoef(Before, b, &l0, &r0);
        ApolloMixCoef(After, b, &l1, &r1);
        if (l0 != l1 || r0 != r1) {
            t |= 1ul << b;
        }
    }
    return t;
}

/* ------------------------------------------------------------------ */
/* [Aux] Aux buses: weights and store                                  */
/* ------------------------------------------------------------------ */

/*
 * IEEE single bits of 10^(dB/20) for half-dB H (-120..+24), 0 = off: encode_aux_fader, correctly rounded (a double
 * product of 10^(1/40) steps, checked against libm for every H by mon-plan-test; 0 dB = 1.0 exactly).
 */
static __inline unsigned long ApolloMonDbBits(long H, int OverSqrt2)
{
    union { float F; unsigned long U; } r;
    double v = 1.0;
    long i;

    for (i = 0; i < (H < 0 ? -H : H); i++) {
        v = (H > 0) ? v * 1.0592537251772889 : v / 1.0592537251772889;
    }
    r.F = (float)(OverSqrt2 ? v / 1.4142135623730951 : v);
    return r.U;
}

static __inline unsigned long ApolloMonAuxBits(long H)
{
    if (H < APOLLO_CUE_SEND_MIN || H > APOLLO_CUE_SEND_MAX) {
        return 0;
    }
    return ApolloMonDbBits(H, 0);
}

static __inline long ApolloAuxLevel(const APOLLO_AUX_STATE *S, unsigned long Aux, unsigned long Bus)
{
    unsigned char v;

    if (Aux >= APOLLO_MON_AUX_BUSES || Bus >= APOLLO_MON_BUSES) {
        return APOLLO_CUE_SEND_OFF;
    }
    v = S->Level[Aux][Bus];
    return (v == 0) ? APOLLO_CUE_SEND_OFF : (long)v - 122L;
}

/*
 * Bus's send to aux Aux now (its level, pan, pre/post): pre-fader the cue send law; post-fader the input's monitor-mix
 * level added (10^((send + fader)/20)/sqrt2, the engine's encode_input_fader(gain_db + fader_db)), 0.0 when the input
 * is muted, solo-silenced or its fader is off.
 */
static __inline void ApolloAuxSendCoef(const APOLLO_CUE_STATE *S, unsigned long Aux, unsigned long Bus, unsigned long *Left,
                                       unsigned long *Right)
{
    long h, f, p;
    unsigned long lin;

    *Left = 0;
    *Right = 0;
    if (Aux >= APOLLO_MON_AUX_BUSES || Bus >= APOLLO_MON_BUSES) {
        return;
    }
    h = ApolloAuxLevel(&S->Aux, Aux, Bus);
    p = (long)S->Aux.Pan[Aux][Bus];
    if (((S->Aux.Post[Aux] >> Bus) & 1ul) == 0) {
        ApolloMonSendCoef(h, p, 0, Left, Right);
        return;
    }
    f = ApolloMixLevel(&S->Mix, Bus);
    if (ApolloMixSilent(&S->Mix, Bus) || f == APOLLO_MIX_LEVEL_OFF || h == APOLLO_CUE_SEND_OFF) {
        return;
    }
    /* encode_input_fader(send dB + fader dB) = 10^((h+f)/40)/sqrt2, rounded once */
    lin = ApolloMonDbBits(h + f, 1);
    if (lin == 0 || p < -APOLLO_CUE_PAN_MAX || p > APOLLO_CUE_PAN_MAX) {
        return;
    }
    *Left = ApolloMonF32Mul(lin, ApolloMonPanBits(p));
    *Right = ApolloMonF32Mul(lin, ApolloMonPanBits(-p));
}

/* Aux Aux's return weight (side 0 only): 10^(dB/20), 0.0 when muted or off. */
static __inline unsigned long ApolloAuxReturnBits(const APOLLO_AUX_STATE *S, unsigned long Aux)
{
    long h;

    if (Aux >= APOLLO_MON_AUX_BUSES || ((S->RetMuted >> Aux) & 1ul) != 0) {
        return 0;
    }
    h = (long)S->RetLevel[Aux];
    if (h == APOLLO_CUE_SEND_OFF) {
        return 0;
    }
    return ApolloMonAuxBits(h);
}

/* Aux Aux's return into cue Cue: 10^(dB/20) with the pan law; 0.0 when the cue is muted or the send off. */
static __inline void ApolloAuxCueCoef(const APOLLO_CUE_STATE *S, unsigned long Aux, unsigned long Cue, unsigned long *Left,
                                      unsigned long *Right)
{
    unsigned long lin;
    unsigned char v;
    long p;

    *Left = 0;
    *Right = 0;
    if (Aux >= APOLLO_MON_AUX_BUSES || Cue >= APOLLO_MON_CUES_MAX || ((S->Muted >> Cue) & 1ul) != 0) {
        return;
    }
    v = S->Aux.CueLevel[Aux][Cue];
    p = (long)S->Aux.CuePan[Aux][Cue];
    lin = (v == 0) ? 0 : ApolloMonAuxBits((long)v - 122L);
    if (lin == 0 || p < -APOLLO_CUE_PAN_MAX || p > APOLLO_CUE_PAN_MAX) {
        return;
    }
    /* _write_send: left = linear x sqrt2 x cos((p+1) pi/4), right = ... sin; ApolloMonPanBits = sqrt2 x cos */
    *Left = ApolloMonF32Mul(lin, ApolloMonPanBits(p));
    *Right = ApolloMonF32Mul(lin, ApolloMonPanBits(-p));
}

/* Records an aux write (already validated by ApolloMonEncode). The Set bits are the caller's (cues.c). */
static __inline void ApolloAuxStore(APOLLO_AUX_STATE *S, const APOLLO_MON_WRITE *W)
{
    unsigned long a = W->Aux, b = W->Bus;
    long v = W->Arg;

    if (a >= APOLLO_MON_AUX_BUSES || b >= APOLLO_MON_BUSES) {
        return;
    }
    switch (W->Kind) {
    case APOLLO_MONW_AUX_SEND:
        S->Level[a][b] = (v <= APOLLO_CUE_SEND_OFF || v > APOLLO_CUE_SEND_MAX) ? 0 : (unsigned char)(v + 122L);
        break;
    case APOLLO_MONW_AUX_PAN:
        S->Pan[a][b] = (signed char)((v < -APOLLO_CUE_PAN_MAX) ? -APOLLO_CUE_PAN_MAX : (v > APOLLO_CUE_PAN_MAX) ? APOLLO_CUE_PAN_MAX : v);
        break;
    case APOLLO_MONW_AUX_POST:
        S->Post[a] = v ? (S->Post[a] | (1ul << b)) : (S->Post[a] & ~(1ul << b));
        break;
    case APOLLO_MONW_AUX_RETURN:
        S->RetLevel[a] = (signed char)((v < APOLLO_CUE_SEND_OFF) ? APOLLO_CUE_SEND_OFF : (v > APOLLO_CUE_SEND_MAX) ? APOLLO_CUE_SEND_MAX : v);
        break;
    case APOLLO_MONW_AUX_MUTE:
        S->RetMuted = v ? (S->RetMuted | (1ul << a)) : (S->RetMuted & ~(1ul << a));
        break;
    case APOLLO_MONW_AUX_CUE:
        if (b < APOLLO_MON_CUES_MAX) {
            S->CueLevel[a][b] = (v <= APOLLO_CUE_SEND_OFF || v > APOLLO_CUE_SEND_MAX) ? 0 : (unsigned char)(v + 122L);
        }
        break;
    case APOLLO_MONW_AUX_CUE_PAN:
        if (b < APOLLO_MON_CUES_MAX) {
            S->CuePan[a][b] = (signed char)((v < -APOLLO_CUE_PAN_MAX) ? -APOLLO_CUE_PAN_MAX : (v > APOLLO_CUE_PAN_MAX) ? APOLLO_CUE_PAN_MAX : v);
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* [polish Nov 15] Ring waits per loop (cues.c)                        */
/* ------------------------------------------------------------------ */

/*
 * A loop of sends (the replay after a connect, a cue's mute or unmute, a bus moved to a strip DSP) used to
 * give every cell its own wait for command-ring room, 50 x 1 ms sleeps. A DSP that stopped consuming made
 * every cell for it wait in turn: up to 128 cells (4 cues x 32 buses) x 50 sleeps under DevLock, 6.4 s and
 * far more where a 1 ms sleep lasts a whole 15.6 ms clock tick. Now one APOLLO_CUE_PASS per loop:
 *   - a send waits for room at most APOLLO_CUE_WAIT_MS of real time (KeQueryInterruptTime, not a sleep count);
 *   - a DSP that did not make room in that time is marked busy: every later send of the loop to it goes
 *     without waiting (STATUS_DEVICE_BUSY when its ring is still full; the cell stays cached for the next
 *     replay, as before);
 *   - the whole loop waits APOLLO_CUE_PASS_BUDGET_MS at most, whatever the DSPs do (a DSP that makes room
 *     just before each timeout cannot stretch it either).
 * A healthy DSP whose ring is momentarily full (a burst larger than the ring) still gets its short wait.
 * Times in 100 ns units, as KeQueryInterruptTime. A NULL pass = never wait (single sends).
 */
#define APOLLO_CUE_WAIT_MS          50ull
#define APOLLO_CUE_PASS_BUDGET_MS   200ull
#define APOLLO_CUE_PASS_DSPS        32ul

typedef struct _APOLLO_CUE_PASS {
    unsigned long       Busy;           /* bit d: DSP d timed out in this loop: no more waits for it */
    unsigned long long  Waited;         /* 100 ns spent waiting in this loop */
} APOLLO_CUE_PASS;

#define APOLLO_CUE_PUT_SEND         0   /* the ring has room: write the two entries */
#define APOLLO_CUE_PUT_WAIT         1   /* sleep a little, refresh the ring, look again */
#define APOLLO_CUE_PUT_BUSY         2   /* give up on this send (kept for the next replay) */

/* How long (100 ns) a send to Dsp may wait for room in pass P: 0 = not at all. */
static __inline unsigned long long ApolloCuePassWaitLimit(const APOLLO_CUE_PASS *P, unsigned long Dsp)
{
    unsigned long long per = APOLLO_CUE_WAIT_MS * 10000ull, budget = APOLLO_CUE_PASS_BUDGET_MS * 10000ull;

    if (P == 0 || Dsp >= APOLLO_CUE_PASS_DSPS || ((P->Busy >> Dsp) & 1ul) != 0 || P->Waited >= budget) {
        return 0;
    }
    return (budget - P->Waited < per) ? budget - P->Waited : per;
}

/*
 * One look at DSP Dsp's command ring during a send of pass P. Room: the ring has the two free entries;
 * Elapsed: 100 ns since this send first found it full (0 on the first look). Returns APOLLO_CUE_PUT_*
 * and books the wait in P (Waited; Busy when the send gives up).
 */
static __inline int ApolloCuePutStep(APOLLO_CUE_PASS *P, unsigned long Dsp, int Room, unsigned long long Elapsed)
{
    if (Room) {
        if (P != 0) {
            P->Waited += Elapsed;
        }
        return APOLLO_CUE_PUT_SEND;
    }
    if (Elapsed < ApolloCuePassWaitLimit(P, Dsp)) {
        return APOLLO_CUE_PUT_WAIT;
    }
    if (P != 0) {
        P->Waited += Elapsed;
        if (Dsp < APOLLO_CUE_PASS_DSPS) {
            P->Busy |= 1ul << Dsp;
        }
    }
    return APOLLO_CUE_PUT_BUSY;
}

/* ------------------------------------------------------------------ */
/* Readback (GET_READBACK words)                                       */
/* ------------------------------------------------------------------ */

/* Word 6 [29:28]: the speaker set in use, 0 MAIN, 1 ALT 1, 2 ALT 2 (oa:driver/ua_apollo.h:884-885). */
static __inline unsigned long ApolloRbAltSelect(const unsigned long *Words)
{
    return (Words[6] >> 28) & 3ul;
}

/*
 * Fields the readback MAY echo: word 2 carries setting 2's level, HP1, mute, mono and dim bits and word 5
 * the x8p's S12 preamp 5-8 flags, so the rest of those settings may sit there too. No source says so;
 * apollo-diag prints them as "if echoed" and tools\phase1\step-mon.ps1 checks whether they follow a write.
 */
static __inline unsigned long ApolloRbEchoSource(const unsigned long *Words)
{
    return ((Words[2] >> 18) & 3ul) | (((Words[2] >> 29) & 1ul) << 2);
}

static __inline unsigned long ApolloRbEchoCueMix(const unsigned long *Words, unsigned long Cue)    /* CUE 1-2: 1 = monitor mix */
{
    return ((Words[2] >> (25ul + 2ul * (Cue & 1ul))) & 1ul);
}

static __inline unsigned long ApolloRbEchoCueMono(const unsigned long *Words, unsigned long Cue)   /* CUE 1-2 */
{
    return ((Words[2] >> (21ul + 2ul * (Cue & 1ul))) & 1ul);
}

static __inline unsigned long ApolloRbEchoHpSource(const unsigned long *Words, unsigned long Hp)   /* HP 1-2 */
{
    return (Words[5] >> (24ul + 3ul * (Hp & 1ul))) & 7ul;
}
