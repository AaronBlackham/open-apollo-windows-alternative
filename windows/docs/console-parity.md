# Console feature parity: what the units can do, and where we stand

Source: Aaron's "UAD Console Feature Compilation, Apollo Twin X & x8p" (Nov 14 2026, from the 39
articles of UA's UAD Console manual plus both Gen 1 hardware manuals). Both units are **Gen 1**:
Twin X Gen 1 **Duo** (2 DSPs, G2 reading) and x8p Gen 1 (6 DSPs). Gen 2-only features (Auto-Gain) do not
apply. UAD plug-ins Aaron is licensed for **can** run on the units' DSPs under our driver, as open-apollo
2.1.0 hosts them: UA's own DSP code from the local install, admitted by the Apollo's own licence answer.
Not built yet; plan and gates in [PLUGINS.md](PLUGINS.md). Our own DSP (dsp\) stays for host-side work.

Status: **have** = works on our driver today; **driver** = mixer/hardware setting we can drive
(docs\spec-dsp-mixer.md); **host** = our kernel engine / Windows side; **DSP** = needs our own SHARC
code (research, shared with "our own plugins"); **UAD** = UA's plug-in on the unit's DSP, licence-gated,
planned in PLUGINS.md; **n/a** = hardware the unit lacks.

## Key facts that change our design

- **Multi-unit clock over Thunderbolt** ("All units clock from the monitor unit over Thunderbolt; don't
  connect Word Clock/ADAT between units; expanders show EXT"). The x8p's clock-source mask already lists a
  Peer source (0x0A = mask bit 10, 0x14A1); the Twin X's stale mask 0x1421 has bit 10 too, so both
  units offer it (confirm on a live read at G3). => The combined device should be **sample-locked in
  hardware** via the Thunderbolt peer clock; spec-twinx 6.3.7 moves from research to the primary path,
  software ASRC (6.3.3) becomes the fallback.
- UA's recommended topology: rack (x8p) = monitor unit, Twin X = expander; speakers only on the monitor
  unit; the expander's monitor outs become line outs; the Twin X becomes the system's **talkback unit**;
  the Twin X knob mirrors system monitor volume/mute as a desk remote. Aaron's speakers are on the Twin X
  today, so our default master stays configurable (Units & routing page).
- Monitor Correction by Sonarworks runs on the Apollo DSP: Twin X Duo with bass management + speaker +
  headphone correction ~13% of one DSP core => DSP-native room correction is realistic (our own).

## Feature table

| Area | Feature | Twin X Gen 1 | x8p Gen 1 | Our status |
|---|---|---|---|---|
| Preamps | Gain 10-65 dB, Mic/Line, 48V (mic), Pad -20 (mic), Polarity, Low Cut 75 Hz | 2 ch | 8 ch | x8p **have** (Apollo Control); Twin X **driver** (G5) |
| Preamps | Hi-Z (auto on TS plug), front jacks | 1 | 2 | readback shows Hi-Z jack; **have** read, switching is hardware |
| Preamps | Stereo link (odd+even, not Hi-Z) | yes | yes | x8p **have** (LINK); Twin X **driver** |
| Preamps | Line Input Gain Bypass | no | per channel | **driver** (setting, x8p) |
| Preamps | Unison emulations, impedance switching | yes | yes | **UAD**, last phase (PLUGINS.md P8; Unison is unmapped upstream); clean preamps at 5.4 kOhm = **have** |
| Preamps | Auto-Gain | Gen 2 only | Gen 2 only | could be **host** feature (our own) |
| Inputs | Optical: 8 ADAT or 2 S/PDIF (format menu) | in only | in + out format | **code** (Nov 15, not run; gated `try`): format per port, S8 bit 0x10 / 0x08 masked; ADAT 4 ch at 88.2/96 on the Twin X (S/MUX), 8 on the x8p, none at 176.4/192; the combined layout and endpoint rows follow once a format is set (`driver/clock.c`, `clock_plan.h`; apollo-control-units.md 1.10) |
| Inputs | Virtual inputs (DAW -> Console) | 8 | 8 | slots exist (rec 18-25 / play 20-27); **host** routing |
| Inputs | SR Convert on S/PDIF in | yes | yes | **code** (Nov 15, not run; gated `try`): S1 bit 31; excludes the S/PDIF clock |
| Mixer | Per-input pan, solo, mute, fader (+12 dB), monitor mix | yes | yes | **code** (Nov 15, not run; `tools\phase1\step-mix.ps1` next): level -inf..+12 dB, pan, mute, solo per input and the talkback (no pan), mute-all; bus coefficient param 0 (`driver/cues.c`, `monitor_plan.h`); solo in the driver; linked pair = one strip with two pans (host); Apollo Control "Monitor mix" card. Nothing sent until set: the unit keeps its default |
| Mixer | 2 stereo aux buses (pre/post), aux returns | yes | yes | **code** (Nov 15, not run): per input send level, pan, pre/post; return level and mute; returns into the cues (`driver/monitor_plan.h` [Aux], `cues.c`); inserts on the returns experimental (apollo-control-units.md 1.9) |
| Mixer | Cue mixes | 2 fixed (HP, Line 3/4) | 2-4 | **code** (Nov 15, not run): send level + pan per input and talkback, per cue (DSP bus coefficients, `driver/cues.c`), cue mute; Apollo Control Cues page |
| Mixer | Cue outputs matrix (MIX on/off, MONO), HP source | HP + Line 3/4 selector | HP1/HP2 switches, Cue Mirror | **code**: MIX source and MONO per cue; HP sources CUE 1 / CUE 2 on both units (x8p enabled: same kext field, morning check); Cue Mirror **driver** |
| Mixer | Output Flex Routing (input -> output, 16 routes) | no | yes | **driver** (x8p) |
| Mixer | Talkback (TALK button / FCN), strip, routing | yes (talkback unit) | yes (FCN) | TALK **have**; the button tracked from the readback with its origin, dim on talkback across units (monitor link `talkdim`) **code** (Nov 15, not run); routing to cues **code**; record slots 32/33 on the Twin X (AX_REC_CH 34, there already); talkback strip: UA's own (module 0xC9) has no recipe, the plain Input Strip on DSP input 0x0A carries the mic (Twin X, Nov 16 2026: the room at -45.9 dBFS with TALK on, zero with TALK off) **code** (StripTalkback, default 0 until the hardware run; docs/STRIPS.md 15) |
| Mixer | Channel DSP Pairing | up to 2 | up to 4 | **DSP** (only matters with plug-ins) |
| Monitor | Level, mute, dim (-9..-60), mono, ALT 1/2 + level/trim | 1 ALT | 2 ALT | x8p level/mute/dim/mono **have**; ALT on, trim, MAIN/ALT select **code** (select reads back) |
| Monitor | Front panel: big knob (MONITOR/PREAMP), press = mute, DIM, ALT, TALK; desk-remote of the monitor unit | yes (the knob unit) | monitor knob | **code** (Nov 15, not run): changes read from the readback each 10 ms pass with their origin (`driver/panel.c`, GET_PANEL); knob mode read-only; monitor link (SET_PANEL, off by default): a knob unit's turns, mute, dim, mono drive the monitor unit (apollo-control-units.md 1.10) |
| Monitor | Monitor source select (MIX / cue) | HP, Line 3/4 | Cue 1-4 | **code** (x8p CUE 3/4 by the kext encoder, not captured) |
| Monitor | Speaker Utilities (per-output solo/mute/trim +-10 dB) | yes (Console 1.2.2) | yes | **host** or **DSP** (no mixer field or coefficient for a per-output trim; the surround trims 0x4B-0x52 are undecoded) |
| Monitor | Bass Management (LR 12/24 dB, xover 80/100/120, sub out) | out 4 | out 8 | **host** now (engine), **DSP** later |
| Monitor | Room/headphone correction (Sonarworks on DSP) | HP1 | HP1/HP2 | ours: Avium Measure + REW import; **host** first, **DSP** later |
| Monitor | Surround 2.1-7.1, fold-downs | no | yes | **driver**/**host** (x8p) |
| Monitor | Headroom +20/+24 dBu, output reference levels | no | yes | **driver** (x8p) |
| Monitor | FCN switch assignment | no | ALT2/Mono/Dim/Talkback/None | **driver** (x8p) |
| System | Sample rate 44.1-192 kHz (S/PDIF <= 96) | yes | yes | all six rates, per unit and on the combined device (driver `rate.c`, `SET_RATE`; ASIO Set/CanSampleRate; `apollo-diag rate`; host `rate-get`/`rate-set`, UI selector to come: apollo-control-units.md 1.6). Plug-in inserts 48 kHz only; input strips at rates other than 48 kHz use open-apollo's per-rate words, unverified on hardware |
| System | Clock source: Internal, S/PDIF, ADAT (+Word Clock x8p) | yes | yes | Internal **have**; external **code** (Nov 15, not run; gated `try`): switched with the transport running, the lock must hold 1 s or Internal comes back; lost for 1 s: Internal (unless hold); lock, 0xC074/0xC07C/0xC084 and the measured rate in GET_CLOCK (`driver/clock.c`; apollo-control-units.md 1.10) |
| System | Buffer size (Windows) | yes | yes | ASIO 64-1024 **have** |
| System | Input Delay Compensation | yes | yes | **host** (ASIO latency reporting) |
| System | I/O Matrix (remap/rename driver channels) | yes | yes | **host**: Units & routing channel order (have, preview) |
| System | Multi-unit cascading (one system, shared buses) | expander | monitor unit | combined device, gates G6-G7 |
| Recall | Sessions (.uadmix), Console Recall plug-in, presets, scenes, MIDI | yes | yes | Apollo Control presets (have, basic); sessions/recall **host** later |
| Plug-ins | UAD inserts (4 per strip + Unison), REC/MON print | yes | yes | **code** (Nov 15, not run): chains of 4 per input / pair / aux, REC/MON per input, DSP room and cycle budget; Unison: the driver's analog lease and the host's slot, no Unison map yet (apollo-control-units.md 1.9) |

## Order (Aaron's priorities so far)

1. Twin X on our driver: G3 bring-up and transport, G4 speakers back through our driver, G5 preamps.
2. Combined device: Thunderbolt peer clock first (no resampling), ASRC fallback; Units & routing Apply.
3. Monitor features both units have: ALT, monitor source, cues, talkback, bass management, speaker trims.
4. Room correction (host first), then our own DSP plug-ins and DSP-native correction.
5. Sample rates beyond 48 kHz (driver and host done, Nov 15; the UI selector next), external clock, digital I/O formats, surround (x8p).
