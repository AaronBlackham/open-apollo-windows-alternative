/*
 * bringup_plan.h - what differs between the models' bring-up, as data (docs/spec-twinx.md
 * sections 2, 3, 4.3 and 6.9.3, gate G3; docs/spec-bringup.md 3.0).
 *
 * Plain C like apollo_model.h: no kernel or Windows headers, every function static __inline and
 * every table inside its function. driver\device.c, audio.c, dsp.c, strips.c, ioctl.c, asio.c and
 * unit.c (through apollo_unit.h) read it; tools\bringup-plan-test proves that the x8p row is what
 * the driver did before G3 and that the Twin X row matches the spec's step table; apollo-diag
 * decodes IO descriptors and the G3 step names with it.
 *
 * Two bring-up paths (spec-bringup 3.0):
 *   ring connect   Apollo x8p (0x22): the ring connect runs in the cold probe, before ACEFACE;
 *                  a warm start keeps the DSP as the previous owner left it. Legacy DMA layout
 *                  34 play / 32 record. Everything as before G3: the x8p code is not changed.
 *   AudioExtension Apollo Twin X Gen 1 (0x99, device type >= 64): a cold start runs the boot
 *                  transport with an early ACEFACE (E7-E13); the ring connect and the dated
 *                  finalize go out after ACEFACE on EVERY connect, cold and warm, and must be
 *                  consumed; the transport takes its counts from the firmware IO descriptors and
 *                  writes no AX_P2P_ROUTE. Mode 1 (G3) = bring-up + transport only. Mode 2 (G4)
 *                  adds its standalone Windows endpoints (MON L/R render, MON L/R monitor-loopback
 *                  capture) and the monitor settings that apply to it (level, HP1 level, mute,
 *                  mono, dim); no ASIO, no strips, no preamp settings (G5).
 *                  [G5] Mode 2 also runs its input strips (strips.c, strip_plan.h; StripsEnable,
 *                  default 0), its preamp settings (preamp_plan.h), talkback, and two more
 *                  endpoint rows, MIC/LINE 1-2 (opt-in at G5; [Endpoints] Nov 15: the full table of
 *                  endpoint_plan.h, MIC/LINE 1-2 in the default set).
 *                  [Monitor] Mode 2 also takes the monitor and HP1 sources, ALT 1 and the cue mixes
 *                  (monitor_plan.h).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once
#include "apollo_model.h"
#include "monitor_plan.h"       /* [Monitor] ALT, sources, cues: the codes and plan bits beyond G4's ten */
#include "endpoint_plan.h"      /* [Endpoints] the standalone endpoint tables as data */

/* ------------------------------------------------------------------ */
/* IOCTL classes (ioctl.c maps every code to one; asio.c is ASIO)      */
/* ------------------------------------------------------------------ */

#define APOLLO_IOCTL_CLASS_READ         0x01u   /* READ_REG, GET_POSITION, GET_READBACK, GET_LEVELS, GET_STRIPS */
#define APOLLO_IOCTL_CLASS_AUDIO_IO     0x02u   /* READ_CAPTURE, WRITE_PLAYBACK (apollo-diag capture, tone) */
#define APOLLO_IOCTL_CLASS_TRANSPORT    0x04u   /* TRANSPORT_START, TRANSPORT_STOP */
#define APOLLO_IOCTL_CLASS_SETTINGS     0x08u   /* SET_INPUT_PARAM, SET_MONITOR_PARAM, SET_MIXER_SETTING */
#define APOLLO_IOCTL_CLASS_RAW_WRITE    0x10u   /* WRITE_REG */
#define APOLLO_IOCTL_CLASS_ASIO         0x20u   /* ASIO_OPEN and the rest (asio.c) */
#define APOLLO_IOCTL_CLASS_ALL          0x3Fu
/* GET_INFO, GET_LOG, GET_ENGINE_STATS and GET_ENDPOINTS are lock-free and answer on every unit. */

/* ------------------------------------------------------------------ */
/* Bring-up steps of the AudioExtension path (GET_INFO v7)             */
/* ------------------------------------------------------------------ */

/* GET_INFO BringupSteps: bit n set = step n passed at the last bring-up. BringupFailStep: n. */
#define APOLLO_BSTEP_NONE               0
#define APOLLO_BSTEP_E1_PROGRAM         1   /* ProgramRegisters: TRANSPORT 0, 8-strobe pulse, no strobe left */
#define APOLLO_BSTEP_E3_READY           2   /* DSP ready wait: every DSP answered (or not asked for) */
#define APOLLO_BSTEP_E4_RINGS           3   /* rings programmed, per-DSP enable + strobe pulse */
#define APOLLO_BSTEP_E6_SG              4   /* SG tables before any transport start */
#define APOLLO_BSTEP_E7_BOOT_START      5   /* boot transport: FRAME_CTR moved after AX_CONTROL 0x20F */
#define APOLLO_BSTEP_E8_ACEFACE         6   /* early ACEFACE answered */
#define APOLLO_BSTEP_E7_PULSE           7   /* DMA reset pulse, 50 ms, ring pages re-programmed */
#define APOLLO_BSTEP_E10_ACEFACE        8   /* ACEFACE retry answered (only run when E8 failed) */
#define APOLLO_BSTEP_E12_READBACK       9   /* mixer readback alive */
#define APOLLO_BSTEP_E13_BOOT_STOP      10  /* boot transport stopped, ACEFACE kept */
#define APOLLO_BSTEP_G5_ACEFACE         11  /* audio connect: handshake done (early one kept, or full) */
#define APOLLO_BSTEP_G5_DESCRIPTORS     12  /* both IO descriptors read, usable and stable */
#define APOLLO_BSTEP_G5_RING_CONNECT    13  /* ring connect + dated finalize CONSUMED by every DSP */
#define APOLLO_BSTEP_G5_RATE_ACK        14  /* rate ack queued to every DSP */
#define APOLLO_BSTEP_G5_MIXER_INIT      15  /* mixer init batch queued (model-safe) */
#define APOLLO_BSTEP_XPORT_START        16  /* transport started on the descriptor counts */
#define APOLLO_BSTEP_RATE_48K           17  /* 0xC07C reads 48 kHz, locked, after the clock write (with the
                                               notifications acknowledged meanwhile), and every DSP has the
                                               48 kHz rate ack (G4) */
#define APOLLO_BSTEP_SAMPLE_POS         18  /* SAMPLE_POS advances */
#define APOLLO_BSTEP_ENGINE             19  /* the 1 ms copy engine runs */
#define APOLLO_BSTEP_COUNT              20

static __inline const char *ApolloBstepName(unsigned long Step)
{
    static const char *const n[APOLLO_BSTEP_COUNT] = {
        "-", "E1 ProgramRegisters", "E3 DSP ready", "E4 rings + enables", "E6 SG tables",
        "E7 boot transport start", "E8 early ACEFACE", "E7 DMA pulse + ring re-program",
        "E10 ACEFACE retry", "E12 readback alive", "E13 boot transport stop", "G5 ACEFACE",
        "G5 IO descriptors", "G5 ring connect consumed", "G5 rate ack", "G5 mixer init",
        "transport start", "48 kHz locked + acked", "SAMPLE_POS advancing", "copy engine",
    };

    return Step < APOLLO_BSTEP_COUNT ? n[Step] : "?";
}

/*
 * [G4] Which steps a bring-up path runs at all (GET_INFO BringupApplies, the former Reserved7):
 * a step outside the mask is "n/a" for that start, not "not passed". Same bit layout as
 * BringupSteps. Ranges of steps: E1-E6 (cold), E4-E6 (warm: F8, F7), the boot transport E7-E13,
 * the audio connect and the transport (G5 on, every start).
 */
#define APOLLO_BSTEPS(a, b)             ((((1ul << ((b) + 1)) - 1ul)) & ~((1ul << (a)) - 1ul))
#define APOLLO_BSTEPS_COLD              APOLLO_BSTEPS(APOLLO_BSTEP_E1_PROGRAM, APOLLO_BSTEP_E6_SG)
#define APOLLO_BSTEPS_WARM              APOLLO_BSTEPS(APOLLO_BSTEP_E4_RINGS, APOLLO_BSTEP_E6_SG)
#define APOLLO_BSTEPS_BOOT              APOLLO_BSTEPS(APOLLO_BSTEP_E7_BOOT_START, APOLLO_BSTEP_E13_BOOT_STOP)
#define APOLLO_BSTEPS_CONNECT           APOLLO_BSTEPS(APOLLO_BSTEP_G5_ACEFACE, APOLLO_BSTEP_ENGINE)

/* ------------------------------------------------------------------ */
/* The plan                                                            */
/* ------------------------------------------------------------------ */

typedef struct _APOLLO_BRINGUP_PLAN {
    int             Model;              /* APOLLO_MODEL_* */
    const char     *Name;
    unsigned long   MaxMode;            /* highest unit Mode this build runs (apollo_unit.h caps it) */
    int             AudioExtension;     /* 1: the Twin X Gen 1 path (spec-twinx 2); 0: ring connect (x8p) */

    /* Cold start: the boot transport (spec-twinx 2 E7-E13, spec-bringup 3.5) */
    int             BootTransport;
    unsigned long   BootPlay, BootRec;  /* legacy fallback counts: UA_DEFAULT_PLAY_CH + 1 / UA_DEFAULT_REC_CH */
    unsigned long   BootFrameCfg;       /* AX_FRAME_CFG */
    unsigned long   BootAxStart;        /* AX_CONTROL: start + extended mode */
    int             BootP2pRoute;       /* AX_P2P_ROUTE = 0x100 | (BootPlay - 1), as ua_boot_transport_start */
    unsigned long   BootStartPolls;     /* FRAME_CTR polls, 1 ms apart */
    unsigned long   EarlyAcefaceTries, EarlyAcefacePolls, EarlyAcefacePollMs;
    unsigned long   PulseSettleMs;      /* after the DMA reset pulse */
    int             RingConnectAtProbe; /* E11 in the cold probe, before ACEFACE */
    int             WarmBootTransport;  /* the per-unit WarmBootTransport=1 runs E7-E13 on a warm start too */

    /* Audio connect (G5) */
    unsigned long   AcefaceTries, AcefacePolls, AcefacePollMs;
    int             RingConnectAtConnect;   /* after ACEFACE, on every connect */
    unsigned long   ConnectConsumeMs;   /* each command must be consumed (POSITION == WRITE_PTR) this fast; 0 = not checked */

    /* Transport */
    int             CountsFromDescriptor;   /* AX_PLAY_CH / AX_REC_CH = IO descriptor counts */
    unsigned long   LegacyPlay, LegacyRec;  /* else these */
    int             P2pRouteAtStart;    /* AX_P2P_ROUTE written at the transport start */
    unsigned long   AxStart;            /* AX_CONTROL start value */
    unsigned long   FrameCfg48k;        /* AX_FRAME_CFG at 44.1/48 kHz */
    int             ConfirmRate;        /* after the clock write: wait for 48 kHz, check SAMPLE_POS, log the clock mask */

    /* Mixer init batch, MixerInitBatch=1 (model-safe): bit s = setting s is left alone */
    unsigned long long BatchSkip;
    const char     *BatchName;

    /* What else runs on the unit */
    int             Endpoints, Asio, Strips;
    unsigned long   IoctlMode1, IoctlMode2; /* APOLLO_IOCTL_CLASS_* a Mode 1 / Mode 2 unit answers */
    int             ObserveDmaOff;      /* observe: switch off the previous owner's ring DMA engine enables */
    int             ServiceAfterFailure;    /* a failed bring-up still starts the 10 ms service thread */
    int             NamesFailStep;      /* failures record APOLLO_BSTEP_* (GET_INFO BringupFailStep) */

    /*
     * [G4] After the clock write (ConfirmRate): poll 0xC07C up to this long, 10 ms apart, writing 0
     * to the notification status (0xC030) whenever it is set, as the kext's handler does: the
     * firmware posts nothing new, the new rate included, while earlier bits stay set
     * (oa:driver/ua_audio.c:2150-2154, 5357-5361). Then the 48 kHz rate ack to every DSP.
     */
    unsigned long   RateConfirmMs;
    /* [G4] Steps each path runs (APOLLO_BSTEPS_*; all 0 = the path records no steps, the x8p) */
    unsigned long   StepsCold, StepsWarm, StepsBoot, StepsConnect;
    /*
     * [G4] The SETTINGS class in detail: monitor parameters (APOLLO_PLAN_MON_BIT; ANY = every code
     * goes on to mixer.c, as before G4), preamp parameters (SET_INPUT_PARAM), raw mixer settings
     * (SET_MIXER_SETTING).
     */
    unsigned long   MonitorParams;
    int             InputParams, RawMixerSettings;
    /* [Out] The output and front-panel codes (monitor_plan.h APOLLO_MON_OUT_*; MonitorParams ANY takes them all) */
    unsigned long   OutputParams;
    /* [G4] Standalone Windows endpoints: rows of the model's table (endpoints.c), default EnabledEndpoints */
    unsigned long   EndpointRows, EndpointDefault;
    unsigned long   AdcFrames;          /* capture latency reported to Windows, frames at 48 kHz */
    /*
     * [Lat] The DAC delay per rate band (44.1/48, 88.2/96, 176.4/192 kHz), 1/100 frame: UA's Mixer Engine
     * "output_delay_comp" row, the figure UA's own engine adds to the output latency it reports
     * (oa:devices/apollo-x8p.json, apollo-twin-x.json; oa:mixer-engine/model_features.py latency_figures).
     * Not measured on this driver (docs/ASIO.md, Latency).
     */
    unsigned long   DacCenti[3];
} APOLLO_BRINGUP_PLAN;

#define APOLLO_PLAN_BIT(s)      (1ull << (s))
#define APOLLO_PLAN_BITS(a, b)  (((1ull << ((b) + 1)) - 1ull) & ~((1ull << (a)) - 1ull))   /* settings a..b */

/*
 * [G4] Monitor parameter codes (copies of apollo_ioctl.h APOLLO_MON_*, which needs Windows types;
 * ioctl.c asserts they are equal) and their bits in APOLLO_BRINGUP_PLAN.MonitorParams.
 */
#define APOLLO_PLAN_MON_LEVEL       0x01ul
#define APOLLO_PLAN_MON_HP1_LEVEL   0x02ul
#define APOLLO_PLAN_MON_MUTE        0x03ul
#define APOLLO_PLAN_MON_SOURCE      0x04ul
#define APOLLO_PLAN_MON_MONO        0x10ul
#define APOLLO_PLAN_MON_HP1_SOURCE  0x3Ful
#define APOLLO_PLAN_MON_HP2_SOURCE  0x40ul
#define APOLLO_PLAN_MON_DIM_LEVEL   0x43ul
#define APOLLO_PLAN_MON_DIM         0x44ul
#define APOLLO_PLAN_MON_TALKBACK    0x46ul
#define APOLLO_PLAN_MON_ANY         0xFFFFFFFFul

static __inline unsigned long ApolloPlanMonBit(unsigned long Param)
{
    switch (Param) {
    case APOLLO_PLAN_MON_LEVEL:      return 0x001ul;
    case APOLLO_PLAN_MON_HP1_LEVEL:  return 0x002ul;
    case APOLLO_PLAN_MON_MUTE:       return 0x004ul;
    case APOLLO_PLAN_MON_SOURCE:     return 0x008ul;
    case APOLLO_PLAN_MON_MONO:       return 0x010ul;
    case APOLLO_PLAN_MON_HP1_SOURCE: return 0x020ul;
    case APOLLO_PLAN_MON_HP2_SOURCE: return 0x040ul;
    case APOLLO_PLAN_MON_DIM_LEVEL:  return 0x080ul;
    case APOLLO_PLAN_MON_DIM:        return 0x100ul;
    case APOLLO_PLAN_MON_TALKBACK:   return 0x200ul;
    default:                         return ApolloMonExtBit(Param);     /* [Monitor] 0x400 and up */
    }
}

/*
 * [G4] The Twin X's standalone endpoint table (endpoints.c g_ApolloTwinXEndpoints): row 0 renders
 * MON L/R (its monitor outputs, Aaron's speakers), row 1 captures the monitor-mix loopback. The
 * slots come from the live IO descriptor by channel ID (0x0901/0x0902); these are the fallbacks,
 * the slots that descriptor had at G2 and G3 (play 0/1, record 26/27).
 * [G5] Rows 2-3 capture MIC/LINE 1 and 2, mono (0x0001/0x0002, record 0/1 at G3): they carry the
 * input strips' signal. Opt-in: the default stays 0x3 (MON only) until G5 passes on the hardware;
 * device add creates every row's circuit object (APOLLO_TWX_EP_ALL), the unit value adds them.
 */
/*
 * [Endpoints] Nov 15: the tables of endpoint_plan.h (every output jack, the loopback, every mic/line input,
 * VIRTUAL and S/PDIF inputs opt-in). Twin X: MON L/R, LINE 3-4, HP out; Monitor mix, MIC/LINE 1-2 in by
 * default (G5 passed); VIRTUAL 1-2, 3-4, S/PDIF in opt-in. The numbers below are the lists' (endpoint_plan.h
 * computes them; endpoints.c and the tests assert both agree).
 */
#define APOLLO_TWX_EP_ROWS              9ul
#define APOLLO_TWX_EP_DEFAULT           0x3Ful
#define APOLLO_TWX_EP_ALL               ((1ul << APOLLO_TWX_EP_ROWS) - 1ul)
#define APOLLO_TWX_SLOT_PLAY_MON_L      0
#define APOLLO_TWX_SLOT_PLAY_MON_R      1
#define APOLLO_TWX_SLOT_REC_MON_L       26
#define APOLLO_TWX_SLOT_REC_MON_R       27
#define APOLLO_TWX_SLOT_REC_IN1         0       /* [G5] 0x0001 MIC/LINE/HIZ 1 */
#define APOLLO_TWX_SLOT_REC_IN2         1       /* [G5] 0x0002 MIC/LINE/HIZ 2 */
#define APOLLO_CHAN_IN1                 0x0001u
#define APOLLO_CHAN_IN2                 0x0002u
#define APOLLO_TWX_ADC_FRAMES           5ul     /* oa:driver/ua_models_gen.h adc_mframes 5000 at 48 kHz */
/* [Endpoints] x8p: MON L/R, LINE 1-2..7-8, HP 1, HP 2, S/PDIF out; Monitor mix, MIC/LINE 1-8 in; VIRTUAL, S/PDIF in opt-in */
#define APOLLO_X8P_EP_ROWS              22ul
#define APOLLO_X8P_EP_DEFAULT           0x1FFFFul
#define APOLLO_X8P_EP_ALL               ((1ul << APOLLO_X8P_EP_ROWS) - 1ul)
#define APOLLO_X8P_ADC_FRAMES           10ul    /* spec-streaming 3.6 (apollo_stream.h APOLLO_ADC_FRAMES) */

/* The plan of an accepted model, or 0 (no bring-up path: the unit only observes). */
static __inline const APOLLO_BRINGUP_PLAN *ApolloBringupPlanFor(int Model)
{
    static const APOLLO_BRINGUP_PLAN t[2] = {
        {
            /* Apollo x8p: the path every release before G3 ran, value for value (bringup-plan-test 1) */
            .Model = APOLLO_MODEL_X8P, .Name = "Apollo x8p", .MaxMode = 2, .AudioExtension = 0,
            .BootTransport = 0,
            .RingConnectAtProbe = 1, .WarmBootTransport = 0,
            .AcefaceTries = 20, .AcefacePolls = 10, .AcefacePollMs = 100,     /* regs.h ACEFACE_* */
            .RingConnectAtConnect = 0, .ConnectConsumeMs = 0,
            .CountsFromDescriptor = 0, .LegacyPlay = 34, .LegacyRec = 32,     /* X8P_PLAY/REC_CHANNELS */
            .P2pRouteAtStart = 1, .AxStart = 0xF, .FrameCfg48k = 0x08, .ConfirmRate = 0,
            /* strips.c kX8pSkip before G3: 2, 3, 12, 13, 14, 16-31 */
            .BatchSkip = APOLLO_PLAN_BIT(2) | APOLLO_PLAN_BIT(3) | APOLLO_PLAN_BITS(12, 14) |
                         APOLLO_PLAN_BITS(16, 31),
            .BatchName = "x8p-safe, preamp fields skipped",
            .Endpoints = 1, .Asio = 1, .Strips = 1,
            .IoctlMode1 = APOLLO_IOCTL_CLASS_ALL, .IoctlMode2 = APOLLO_IOCTL_CLASS_ALL,
            .ObserveDmaOff = 0, .ServiceAfterFailure = 1, .NamesFailStep = 0,
            /* [G4] none of the new knobs changes anything on the x8p */
            .RateConfirmMs = 0,
            .StepsCold = 0, .StepsWarm = 0, .StepsBoot = 0, .StepsConnect = 0,
            .MonitorParams = APOLLO_PLAN_MON_ANY, .InputParams = 1, .RawMixerSettings = 1,
            .OutputParams = APOLLO_MON_OUT_ALL,
            .EndpointRows = APOLLO_X8P_EP_ROWS, .EndpointDefault = APOLLO_X8P_EP_DEFAULT,
            .AdcFrames = APOLLO_X8P_ADC_FRAMES,
            .DacCenti = { 4300, 1525, 1550 },                                 /* [Lat] 43, 15.25, 15.5 */
        },
        {
            /* Apollo Twin X Gen 1: docs/spec-twinx.md section 2 (step table) and 4.3; G4: Mode 2; G5: inputs */
            .Model = APOLLO_MODEL_TWINX, .Name = "Apollo Twin X (Gen 1)", .MaxMode = 2, .AudioExtension = 1,
            .BootTransport = 1, .BootPlay = 3, .BootRec = 2, .BootFrameCfg = 0x10, .BootAxStart = 0x20F,
            .BootP2pRoute = 1, .BootStartPolls = 10,
            .EarlyAcefaceTries = 2, .EarlyAcefacePolls = 10, .EarlyAcefacePollMs = 50, .PulseSettleMs = 50,
            .RingConnectAtProbe = 0, .WarmBootTransport = 1,
            .AcefaceTries = 20, .AcefacePolls = 10, .AcefacePollMs = 100,
            .RingConnectAtConnect = 1, .ConnectConsumeMs = 50,
            .CountsFromDescriptor = 1, .LegacyPlay = 0, .LegacyRec = 0,
            .P2pRouteAtStart = 0, .AxStart = 0xF, .FrameCfg48k = 0x08, .ConfirmRate = 1,
            /* spec-twinx 4.3: preamp, headphone, monitor-reference and ALT fields */
            .BatchSkip = APOLLO_PLAN_BITS(2, 3) | APOLLO_PLAN_BIT(11) | APOLLO_PLAN_BITS(13, 17) |
                         APOLLO_PLAN_BIT(23),
            .BatchName = "Twin X-safe, preamp/HP/monitor-reference/ALT fields skipped",
            /*
             * [G4] Windows endpoints in Mode 2 (standalone MON L/R); ASIO stays the x8p's.
             * [G5] Input strips in Mode 2 (strip_plan.h StripModelFor; StripsEnable decides, default 0).
             */
            .Endpoints = 1, .Asio = 0, .Strips = 1,
            .IoctlMode1 = APOLLO_IOCTL_CLASS_READ | APOLLO_IOCTL_CLASS_AUDIO_IO,
            .IoctlMode2 = APOLLO_IOCTL_CLASS_READ | APOLLO_IOCTL_CLASS_AUDIO_IO | APOLLO_IOCTL_CLASS_SETTINGS,
            .ObserveDmaOff = 1, .ServiceAfterFailure = 0, .NamesFailStep = 1,
            .RateConfirmMs = 2000,
            .StepsCold = APOLLO_BSTEPS_COLD, .StepsWarm = APOLLO_BSTEPS_WARM, .StepsBoot = APOLLO_BSTEPS_BOOT,
            .StepsConnect = APOLLO_BSTEPS_CONNECT,
            /*
             * spec-twinx 4.1: S2 level [7:0], HP1 [15:8] (one headphone), mute 16, mono 17, dim 31;
             * [G5] talkback S13 bit 8 (its built-in mic; readback word 6 bit 8)
             * [Monitor] monitor source (MIX, HP, LINE 3/4), HP1 source, CUE 1-2 MIX/mono, ALT 1 on
             * LINE 3/4, the cue sends (monitor_plan.h; the dim level stays refused, unverified here)
             * [Mix] the monitor mix of inputs 1-2 and the talkback (level, pan, mute, solo, mute-all)
             * [Aux] the two aux buses: sends, returns, returns into its two cues (monitor_plan.h)
             */
            .MonitorParams = 0x001ul | 0x002ul | 0x004ul | 0x010ul | 0x100ul | 0x200ul |
                             0x008ul | 0x020ul | APOLLO_MON_EXT_TWINX | APOLLO_MON_EXT_MIX | APOLLO_MON_BIT_AUX,
            /* [G5] its 2 preamps and the Hi-Z impedance of input 1 (preamp_plan.h); raw settings never */
            .InputParams = 1, .RawMixerSettings = 0,
            /* [Out] masked bit writes only: aux mono, meter holds, identify, the monitor reference (monitor_plan.h) */
            .OutputParams = APOLLO_MON_OUT_AUX_MONO | APOLLO_MON_OUT_CLIP_HOLD | APOLLO_MON_OUT_PEAK_HOLD |
                            APOLLO_MON_OUT_IDENTIFY | APOLLO_MON_OUT_MON_REF,
            .EndpointRows = APOLLO_TWX_EP_ROWS, .EndpointDefault = APOLLO_TWX_EP_DEFAULT,
            .AdcFrames = APOLLO_TWX_ADC_FRAMES,
            .DacCenti = { 4430, 1635, 1735 },                                 /* [Lat] 44.3, 16.35, 17.35 */
        },
    };

    if (Model == APOLLO_MODEL_X8P) {
        return &t[0];
    }
    if (Model == APOLLO_MODEL_TWINX) {
        return &t[1];
    }
    return 0;
}

/* May a unit of this plan, in this Mode, answer an IOCTL of this class? Class 0 = no class. */
static __inline int ApolloPlanIoctlAllowed(const APOLLO_BRINGUP_PLAN *P, unsigned long Mode, unsigned long Class)
{
    unsigned long allowed;

    if (Class == 0) {
        return 1;                       /* unknown codes go on to "invalid request" */
    }
    if (P == 0) {
        return 0;
    }
    allowed = (Mode >= 2) ? P->IoctlMode2 : P->IoctlMode1;
    return (allowed & Class) == Class;
}

/* [G4] Within the SETTINGS class: may this unit take SET_MONITOR_PARAM Param? (x8p: any, as before) */
static __inline int ApolloPlanMonitorParamAllowed(const APOLLO_BRINGUP_PLAN *P, unsigned long Param)
{
    unsigned long b = ApolloPlanMonBit(Param), o = ApolloMonOutBit(Param);

    if (P == 0) {
        return 0;
    }
    if (P->MonitorParams == APOLLO_PLAN_MON_ANY) {
        return 1;
    }
    if (o != 0) {
        return (P->OutputParams & o) != 0;      /* [Out] */
    }
    return b != 0 && (P->MonitorParams & b) != 0;
}

/*
 * [G4] The steps a start of this plan runs (GET_INFO BringupApplies): cold or warm, the boot
 * transport or not, and E10 only when the early ACEFACE (E8) did not answer. 0 = no step record.
 */
static __inline unsigned long ApolloPlanBstepApplies(const APOLLO_BRINGUP_PLAN *P, int Warm, int Boot,
                                                     int EarlyAnswered)
{
    unsigned long m;

    if (P == 0) {
        return 0;
    }
    m = (Warm ? P->StepsWarm : P->StepsCold) | P->StepsConnect;
    if (Boot) {
        m |= P->StepsBoot;
        if (EarlyAnswered) {
            m &= ~(1ul << APOLLO_BSTEP_E10_ACEFACE);
        }
    }
    return m;
}

/*
 * The mixer-init batch (strips.c ApolloStripsMixerInitBatch, spec-twinx 4.3): does batch Mode
 * (MixerInitBatch: 0 none, 1 model-safe, 2 legacy) write 0x20/0xFF to setting S? S2 (monitor)
 * never; S35 gets its own 0x05/0x1F in modes 1 and 2. Model-safe with no plan writes nothing.
 */
static __inline int ApolloPlanBatchWrites(const APOLLO_BRINGUP_PLAN *P, unsigned long Mode, unsigned long S)
{
    if (Mode == 0 || S == 0 || S == 2 || S == 35 || S > 37) {
        return 0;
    }
    if (Mode == 1) {
        return P != 0 && (P->BatchSkip & APOLLO_PLAN_BIT(S)) == 0;
    }
    return 1;
}

/* DMA_CTRL per-DSP enables [8:1]: one per DSP (spec-bringup E4). x8p 6 DSPs: 0x7E; Duo 0x6; Quad 0x1E. */
static __inline unsigned long ApolloPlanDmaEnables(unsigned long NumDsps)
{
    return (NumDsps == 0 || NumDsps > 8) ? 0 : ((((1ul << NumDsps) - 1ul) << 1) & 0x1FEul);
}

/* ua_calc_buf_frame_size: min(8192, rounddown_pow2(4 MiB / (max(play, rec) * 4) / 2)). */
static __inline unsigned long ApolloPlanBufFrames(unsigned long Play, unsigned long Rec)
{
    unsigned long m = Play > Rec ? Play : Rec, f, p = 1;

    if (m == 0) {
        m = 1;
    }
    f = 0x400000ul / (m * 4ul) / 2ul;
    while (p <= f / 2ul) {
        p <<= 1;
    }
    return p > 8192ul ? 8192ul : p;
}

/* NOTIF_CLOCK_CFG word: (rate index << 8) | source; 48 kHz from Internal 0x0C = 0x20C. */
static __inline unsigned long ApolloPlanClockWord(unsigned long RateIndex, unsigned long Source)
{
    return ((RateIndex & 0xFFul) << 8) | (Source & 0xFFul);
}

/* NOTIF_RATE_INFO (0xC07C): [15:8] rate index, [1:0] lock. 0x201 = 48 kHz locked. */
static __inline int ApolloPlanRateLocked(unsigned long RateInfo, unsigned long RateIndex)
{
    return RateInfo != 0xFFFFFFFFul && ((RateInfo >> 8) & 0xFFul) == RateIndex && (RateInfo & 3ul) != 0;
}

/* ------------------------------------------------------------------ */
/* Firmware IO descriptors (0xC1A4 record, 0xC2C4 playback)            */
/* ------------------------------------------------------------------ */

/*
 * 72 words each (oa:driver/ua_apollo.h:424-452): [0] version 1, [1] 0x46, [2] all ones,
 * [3] record count, [4] playback count, [5] flags, [6..] 16-bit channel IDs in slot order, low
 * half first. Channel ID = type << 8 | number (1-based). The FPGA lays the DMA frame out in this
 * order (verified on the x8p, docs/spec-streaming.md).
 */
#define APOLLO_IODESC_WORDS     72
#define APOLLO_IODESC_HDR       6
#define APOLLO_IODESC_VERSION   1ul
#define APOLLO_IODESC_LENGTH    0x46ul
#define APOLLO_IODESC_MAX       64ul    /* UA_MAX_CHANNELS = AUDIO_MAX_CHANNELS */

#define APOLLO_CHAN_MON_L       0x0901u
#define APOLLO_CHAN_MON_R       0x0902u

/* The slot count of a descriptor, or 0 when its header is not usable (ua_io_desc_hdr_count). */
static __inline unsigned long ApolloIoDescCount(const unsigned long *W, int Record)
{
    unsigned long c;

    if (W[0] != APOLLO_IODESC_VERSION || W[1] != APOLLO_IODESC_LENGTH) {
        return 0;
    }
    c = W[Record ? 3 : 4];
    return (c == 0 || c > APOLLO_IODESC_MAX) ? 0 : c;
}

/* The channel ID of a slot (no range check beyond the 72 words). */
static __inline unsigned short ApolloIoDescId(const unsigned long *W, unsigned long Slot)
{
    unsigned long w = (Slot / 2ul + APOLLO_IODESC_HDR < APOLLO_IODESC_WORDS) ? W[APOLLO_IODESC_HDR + Slot / 2ul] : 0;

    return (unsigned short)((Slot & 1ul) ? (w >> 16) : (w & 0xFFFFul));
}

/* The first slot carrying Id (FindIndexForChannelID), or -1. */
static __inline long ApolloIoDescFind(const unsigned long *W, int Record, unsigned short Id)
{
    unsigned long c = ApolloIoDescCount(W, Record), i;

    for (i = 0; i < c; i++) {
        if (ApolloIoDescId(W, i) == Id) {
            return (long)i;
        }
    }
    return -1;
}

static __inline void ApolloStrPut(char *Out, unsigned long Len, unsigned long *N, const char *S)
{
    while (*S != 0 && *N + 1 < Len) {
        Out[(*N)++] = *S++;
    }
    if (Len != 0) {
        Out[*N < Len ? *N : Len - 1] = 0;
    }
}

static __inline void ApolloStrPutU(char *Out, unsigned long Len, unsigned long *N, unsigned long V)
{
    char d[12];
    int k = 0;

    do {
        d[k++] = (char)('0' + (V % 10ul));
        V /= 10ul;
    } while (V != 0 && k < 11);
    while (k > 0 && *N + 1 < Len) {
        Out[(*N)++] = d[--k];
    }
    if (Len != 0) {
        Out[*N < Len ? *N : Len - 1] = 0;
    }
}

/*
 * A channel's name the way the kext's _parseIONames spells it (ua_chan_name): stereo types
 * (AUX, S/PDIF, AES/EBU, MON, CUE) are named by pair + L/R, numbered types carry the number,
 * AUX/CUE run the pair number into the type ("AUX1 L"). Returns the length written.
 */
static __inline unsigned long ApolloChanName(unsigned short Id, char *Out, unsigned long Len)
{
    static const char *const names[15] = {
        "MIC/LINE/HIZ", "MIC/LINE", "AUX", "LINE", "ADAT", "S/PDIF", "AES/EBU", "VIRTUAL", "MADI",
        "MON", "CUE", "TALKBACK", "RESERVED", "DANTE", "UNKNOWN",
    };
    unsigned long type = (unsigned long)Id >> 8, num = (unsigned long)Id & 0xFFul, n = 0;
    const char *lr = (num & 1ul) ? "L" : "R";
    int stereo, numbered;

    if (Len == 0) {
        return 0;
    }
    Out[0] = 0;
    if (Id > 0x0DFFu) {
        type = 0x0E;
    }
    stereo = (int)((0x0664ul >> type) & 1ul);
    numbered = (int)((0x7D9Ful >> type) & 1ul);
    if (stereo) {
        num = (num + 1ul) / 2ul;
    }
    ApolloStrPut(Out, Len, &n, names[type]);
    if (numbered && stereo) {
        ApolloStrPutU(Out, Len, &n, num);
        ApolloStrPut(Out, Len, &n, " ");
        ApolloStrPut(Out, Len, &n, lr);
    } else if (numbered) {
        ApolloStrPut(Out, Len, &n, " ");
        ApolloStrPutU(Out, Len, &n, num);
    } else if (stereo) {
        ApolloStrPut(Out, Len, &n, " ");
        ApolloStrPut(Out, Len, &n, lr);
    }
    return n;
}

/* Clock sources in NOTIF_CLOCK_SRCS [31:16]: bit n = source id n. */
#define APOLLO_CLKSRC_SPDIF     0x00
#define APOLLO_CLKSRC_ADAT      0x05
#define APOLLO_CLKSRC_WORDCLOCK 0x07
#define APOLLO_CLKSRC_PEER      0x0A    /* peer/slave over Thunderbolt: hidden from UA's menu */
#define APOLLO_CLKSRC_INTERNAL  0x0C

static __inline int ApolloClockOffers(unsigned long ClockSrcs, unsigned long Source)
{
    return Source < 16 && ((ClockSrcs >> (16 + Source)) & 1ul) != 0;
}
