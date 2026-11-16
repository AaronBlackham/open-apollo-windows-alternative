# Apollo Twin X (Gen 1, 0x99) on apollo.sys, and the combined x8p + Twin X device

Status: spec (Nov 14 2026). Phase 5 of [PLAN.md](PLAN.md) (`docs/PLAN.md:106-107`). G0-G1 code
(identification, model table, per-unit keys, revert-unit) is built and host-tested in
`builds/work-twinx` (6.9.1), not installed; the INF still matches the x8p only. Nothing else is
implemented or run. **Update:** G1 and G2 passed on the hardware (Nov 14, 6.9.2); the G3 code (the
Twin X Gen 1 bring-up, Mode 1) is built and offline-tested in `builds/work-g3` (6.9.3), not
installed. **Update 2:** G3 passed on the hardware (Nov 14); the G4 code (Twin X Mode 2: its own
Windows endpoints and monitor settings, the rate confirmation fix, per-path step lists) is built and
offline-tested in `builds/work-g4` (6.9.4); G4 passed on the hardware Nov 14. **Update 3:** the G5 code (Twin X
input strips, preamp settings, MIC/LINE 1-2 endpoints) is built and offline-tested in `builds/work-g5` (6.9.5),
not installed. **Update 4 (Nov 15):** G6, the combined device, is built and tested on both units in
`builds/work-g6` (6.9.6): ASRC clock (the Thunderbolt peer clock does not lock without its unpublished P2P
sequence), one ASIO driver "Apollo (Avium)", Apollo Control Apply. G5 and G6 are merged in the live tree.

Decisions this spec builds on (Aaron, Nov 14):

- The x8p and the Twin X appear as **one combined device**: one ASIO driver, and Windows endpoints
  built from the same configuration. Apollo Control sets the channel order, the clock master and
  the output routing (DAW mains and Windows audio to the Twin X monitor outputs, mirroring).
- **Thunderbolt only.** No optical or word-clock cable joins the units. The primary clock design is
  software drift correction with an adaptive resampler (6.3). Clock master defaults to the unit
  that drives the monitors, the Twin X. Optical sync stays documented as the sample-locked
  option if a Toslink cable is added later (6.3.6).
- The Twin X drives Aaron's speakers. Every step keeps a per-unit way back to UA's driver (6.10,
  section 7).
- **Both units are first generation** (Aaron, Nov 14): the Twin X is Gen 1 (device type 0x99) and the
  x8p is Gen 1 (0x22, the unit our driver already runs). This spec designs for those two only.
  Twin X Gen 2 appears only where its verified upstream path is the best evidence for Gen 1 (bring-up
  order, input strips).

## 0. Conventions and evidence

- `oa:` = `apollo-win/reference/open-apollo/` (GPL-2.0, read only), paths as in
  [spec-bringup.md](spec-bringup.md) section 0. Paths without `oa:` are ours, line numbers as of
  Nov 14 2026 (the live tree is being edited by other agents: function names are the anchor).
- **[HW]** measured on Aaron's units. **[UA-LOG]** from UA Console's own logs on this PC
  (`%LOCALAPPDATA%\Universal Audio\Logs\Console_0.log`, `Console_1.log`). **[UA-INF]** from
  UA's installed `C:\Windows\INF\oem194.inf` (uad2pcie, DriverVer 18.59.27.698). **[UA-WEB]** UA's
  help center. **(inference)** = not stated by any source. **(derived)** = computed from cited
  values.
- "Twin X" = Apollo Twin X Gen 1, device type 0x99 = 0x80 | public type 0x19
  (`oa:driver/ua_apollo.h:127`). "Gen 2" (0x3B/0x3C/0x40/0x41) is cited only as upstream evidence;
  it is not Aaron's hardware.

### 0.1 Facts gathered for this spec

| Fact | Value | Source |
|---|---|---|
| Twin X PCI hardware ID | `PCI\VEN_1A00&DEV_0002&SUBSYS_00191A00` | Aaron; UA's INF lists SUBSYS_00191A00 as generic "Apollo Thunderbolt" **[UA-INF]** `oem194.inf:64` |
| Twin subsystems in UA's INF | 0x07, 0x08, 0x0C, 0x0D, 0x0E are "Apollo Twin Thunderbolt" (older Twins); 0x19 is not among them | **[UA-INF]** `oem194.inf:51-57, 64, 153`. `docs/PLAN.md:18` (Twin X = 0x07/0x08/0x0C-0x0E) is wrong for this unit |
| UA ran both units as one multi-unit system | 2023-07-31: device 0 "Apollo Twin X" (DSP spanning 3 slices), device 1 "Apollo x8p" (5 slices), device order changed, clock "Internal", 96 kHz | **[UA-LOG]** `Console_0.log:8398-8420` |
| Twin X DSP count | probably **Quad** (4 DSPs): the x8p (6 DSPs) shows 5 spanning slices, the Twin X 3 (inference: slices = DSPs - 1) | **[UA-LOG]** as above; confirmed at gate G2 from ext_caps[15:8] |
| UA multi-unit clocking | over Thunderbolt from the monitor unit; do not interconnect word clock, ADAT or MADI between Apollos; Twin as monitor unit "not recommended" | **[UA-WEB]** (sources at the end) |
| x8p clock-source mask today | 0xC08C = 0x14A1007E: sources S/PDIF 0x00, ADAT 0x05, Word Clock 0x07, Peer 0x0A, Internal 0x0C; rates: all six | **[HW]** `apollo-diag log`, `rate info 0x00000201 ... clock sources 0x14a1007e` |
| x8p identity | ExtCaps 0x81110611 (audio ext, family 17, 6 DSPs, ready cap 1), FPGA 0xA241C5AC, FW 11.15.54803, DMA_CTRL 0x7F, serial digits 5-8 "2017" | **[HW]** `apollo-diag info` |
| Twin X digital I/O | optical **input only** (S/PDIF or ADAT); no digital output, no word clock | `oa:devices/apollo-twin-x.json:14, 61-72` (no S/PDIF/ADAT playback IDs) |

---

## 1. Identification

### 1.1 What tells the Twin X from the x8p (and from everything else)

Algorithm: `oa:driver/ua_core.c:349-458` (ours `driver/hw.c:148-220`), refined by the public type
(`oa:driver/ua_core.c:560-624`, ours `driver/hw.c:88-135`) and `ua_resolve_public_model`
(`oa:driver/ua_core.c:653-680`, **missing in ours**).

| Field | x8p (0x22) | Twin X (0x99) | Read from |
|---|---|---|---|
| PCI subsystem (config 0x2E) | 0x0014 **[HW]** | 0x0019 (Aaron) | `driver/hw.c:137-146` |
| FPGA_REV 0x2218 | 0xA241C5AC **[HW]**, bit 31 set (v2) | bit 31 set expected; value unknown | `driver/hw.c:152-163` |
| ext_caps 0x2234 bit 31 | 1 | 1 expected | `oa:driver/ua_apollo.h:1749-1761` |
| ext_caps family [25:20] | 17 (HEXA) | **19** | `oa:devices/apollo-twin-x.json:8`, `oa:driver/tests/test_public_type.py:101` |
| ext_caps DSPs [15:8] | 6 | 2 (Duo) or 4 (Quad); probably 4 here (0.1) | `oa:driver/ua_core.c:406` |
| ext_caps ready cap [7:4] | 1 **[HW]** | unknown | `oa:driver/ua_dsp.c:3069-3072` |
| serial digits 5-8 (BAR0 0x20-0x2F, string offset 4) | 2017 | **none in the table**, or 2024/2025 (then retyped, 1.2) | `oa:driver/ua_core.c:473-525`, `oa:CHANGELOG.md:192` |
| public type | 0x16 | **0x19** | `oa:driver/ua_core.c:612-622` |
| device type | 0x22 (serial) | **0x99** = 0x80 \| 0x19 | `oa:driver/ua_core.c:653-680` |
| mixer kind | 12 (0x0C) | **15 (0x0F)** | `oa:driver/ua_core.c:1782-1793` |
| ARM preamp channels | 0-7 | **0-1** | `oa:driver/ua_core.c:1807-1819` |
| Mixer class (UA Mixer Engine) | HEXA | "Lavern", type id 13 | `oa:devices/apollo-twin-x.json:9-10` |
| open-apollo status | verified | **unverified** | `oa:devices/apollo-twin-x.json:7`, `oa:README.md:133` |

**What makes it 0x99:** ext_caps family 19 gives public type 0x19 for every serial except the Gen 2
digits {2090, 2091, 2107, 2108} (`oa:devices/apollo-twin-x.json:8`, `oa:driver/ua_core.c:612-622`); no
serial-table entry names Gen 1, so the device type is 0x80 | 0x19. A family-19 unit with Gen 2 digits
is a Gen 2 (public 0x25/0x29) and is refused (1.3). The subsystem ID does **not** identify the model:
nothing in any source pins 0x0019 (UA's INF lists 0x18-0x1C under one generic name); it only selects
the INF line. The FPGA revision is model-specific and unknown until gate G2.

### 1.2 Fix needed in `ApolloIdentify` before the Twin X is bound

`driver/hw.c:177-196` keeps a non-zero serial-table type. A Twin X whose digits 5-8 read 2024 or
2025 would come out as 0x27 (Solo) or 0x28 (Arrow); open-apollo retypes those to 0x99 on family 19
(`oa:driver/ua_core.c:660-661`, `oa:CHANGELOG.md:192`: "serials 2024/2025 are Twin X units"). Port the
whole of `ua_resolve_public_model`, including the Conrad-family branch (`:662-666`), so a Solo or
Arrow never runs a Twin X path either.

### 1.3 What the driver accepts, refuses, flags

Replace `ApolloSupported()` (`driver/device.c:27-30`, x8p only) with a model table:

| Model row | Accept when | Bring-up path |
|---|---|---|
| x8p | type 0x22, family 17, public 0x16, DSPs 6 | ring-connect (today's) |
| Twin X | type 0x99, family 19, public 0x19, DSPs 2 or 4, ext_caps bit 31, FW v2 | AudioExtension (section 2) |
| anything else (Twin X Gen 2 0x3B/0x3C/0x40/0x41, Solo 0x27, Arrow 0x28, x4 0x1F, Twin MkII 0x92, x8p Gen 2 0x39, ...) | never | stay started and idle (`driver/device.c:101-105`), so `apollo-diag` reports what it is |

Also refuse (idle, `STATUS_NOT_SUPPORTED`): DSPs 0 or > 8 (exists, `driver/hw.c:171-174`); a Twin
type without ext_caps bit 31 (today only logged, `driver/hw.c:210-212`); BAR0 < 0xC500 (exists,
`driver/device.c:72-75`).

**Flag, do not refuse:** firmware/FPGA. `FwUntested` compares against one x8p pair
(`driver/hw.c:213-218`, `include/apollo_ioctl.h:107-110`). Make it a table of tested
`{model, FW_RELEASE, FPGA_REV}` rows; add the Twin X row only after gate G3 passes on Aaron's unit.
Same freeze rule as the x8p (PLAN decision 10): no UA firmware updates on either unit.

**Unit identity for configuration:** read HW_UID at BAR0 0x30/0x34 (`oa:driver/ua_apollo.h:263-264`)
at identify time and use it as the stable per-unit key (6.9). The PnP instance path is not stable
(it encodes the Thunderbolt port), and the serial string contains non-ASCII bytes on the x8p
(`apollo-diag info` prints `??`).

---

## 2. Bring-up: Twin X Gen 1 versus the x8p

Gen 1 takes the AudioExtension branch because 0x99 >= 64 (`oa:driver/ua_apollo.h:141-157`) and needs
the ring connect because public 0x19 is in the kext mask 0x3ff17fcf9b0 (`oa:driver/ua_apollo.h:185-197`).
The full branch table is [spec-bringup.md](spec-bringup.md) 3.0. Step by step, against our
`ApolloBringup()` (`driver/device.c:268-349`):

| Step | x8p today (ours) | Twin X Gen 1 (to add) | Upstream status for Gen 1 | Safe failure detection |
|---|---|---|---|---|
| A Identify, B quiesce | `device.c:86-99` | same | same code | FPGA_REV all ones = gone |
| D warm/cold | `ApolloMixerDspAlive` `hw.c:433-453` | same | same | none needed |
| E1 ProgramRegisters | `device.c:284-286` | same; 8-strobe reset pulse (`hw.c:397-415`) | same | read DMA_CTRL back: no strobe left set |
| E3 DSP ready wait | `hw.c:456-482`, x8p cap = 1 | same, only if ext_caps[7:4] == 1 | cap unknown on Gen 1 | timeout is logged, not fatal (as upstream); report it in GET_INFO |
| Boot words (strips ownership) | `ApolloStripsProbeBoot` before E4 `device.c:295` | same place: **before** the boot transport, which might change them (inference) | n/a | log all DSP words |
| E4 rings init + per-DSP enable pulse | `dsp.c` via `device.c:297` | same; enable bits 0x6 (Duo) or 0x1E (Quad) (derived) | same code | DMA_CTRL readback = 1 + enables |
| E5 SEQ_WR = 0 | `device.c:302` | same | | |
| E6 SG tables | `audio.c:81-109` | same, **mandatory before E7** | `oa:driver/ua_core.c:4348-4370` | SG must be valid before any transport start (IOMMU fault otherwise) |
| E7 **boot transport** | absent | clock_source = 0x0C; full prepare with legacy fallback counts play 3 / rec 2, FRAME_CFG 0x10, ARM, sleep 1-2 ms, AX_CONTROL = **0x20F**, poll FRAME_CTR; early ACEFACE (E8); DMA reset pulse (all 8 strobes for one write, restore, read); sleep 50 ms; re-program every ring's pages (no drain) | `oa:driver/ua_core.c:4372-4391`, `oa:driver/ua_audio.c:2702-2847`; verified only on Gen 2 / x4 | FRAME_CTR still 0 after start = start ignored: log, stop transport, continue to E10 |
| E8 early ACEFACE | absent | 2 tries x 10 polls x 50 ms, `0x10` disconnect doorbell before try 2 | `oa:driver/ua_audio.c:2625-2700` | timeout: `aceface_failed`, fall through |
| E10 ACEFACE retry | absent | once more if E8 failed, transport still running | `oa:driver/ua_core.c:4446-4470` | |
| E11 ring connect at probe | `device.c:320-343` (cold only) | **not here** | `oa:driver/ua_core.c:3702-3706` (deferred) | |
| E12 readback liveness | `device.c:337-342` | same (upstream runs it for both) | `oa:driver/ua_core.c:4511-4538` | log only |
| E13 boot transport stop | absent | AX_CONTROL = 0; `Connected = FALSE`; **keep `AcefaceDone = TRUE`** | `oa:driver/ua_core.c:4549-4554`, `oa:driver/ua_audio.c:2833-2847` | |
| F (warm) | `device.c:305-317` | same, plus F9 = E13 (AX_CONTROL = 0); no boot transport | `oa:driver/ua_core.c:4244-4294` | |
| G5 audio connect | `audio.c:165-218`: full ACEFACE (20 x 10 x 100 ms), reads, rate ack, mixer init | if `AcefaceDone` (cold): skip the handshake; else full ACEFACE with the transport stopped (warm). Then reads, then **ring connect + dated finalize on every connect, cold and warm** (`ApolloDspConnectAll`, the E11 sequence), then rate ack, then mixer init | `oa:driver/ua_audio.c:3000-3401`, ring connect `:3161-3163`, `oa:driver/ua_core.c:3708-3726`; skipping it "left playback silent" `oa:CHANGELOG.md:50-52` | ACEFACE timeout: Stage Failed at AcefaceDone, transport stays stopped, "power-cycle the Twin X". Ring connect: add a consumption check (each DSP's command POSITION reaches WRITE_PTR within 50 ms), else Stage Failed, nothing more is sent |
| Rate ack | `audio.c:205-213` | same, every DSP with rings | same | |
| Mixer init batch | `mixer.c:36-58` | **per-model batch** (4.3) | Gen 2 ran the full x4 batch | |
| Transport start | `audio.c:238-326`, start-at-connect | **descriptor counts** for AX_PLAY_CH / AX_REC_CH; **no AX_P2P_ROUTE write** (`audio.c:275` is a legacy-layout step; io_map=1 skips it, `oa:driver/ua_audio.c:3807-3820`); AX_CONTROL 0xF; clock write 0xC074 = 0x20C + doorbell 4 after start (`audio.c:311-312`) | descriptor layout is the Linux default on the Twin X `oa:driver/ua_audio.c:317-322` | SAMPLE_POS advancing at 48 kHz (engine stats), FRAME_CTR == FRAME_CFG, 0xC07C == 0x201 |
| D0 exit quiesce | `audio.c:585-606` | same; it clears `AcefaceDone`, so the next start is a full ACEFACE (warm) | | |

**Notes and unknowns.**

- **Warm ACEFACE with the transport stopped** works on the x4 (`oa:HARDWARE-SESSIONS.md:99`) but the
  boot-stop comment says a re-ACEFACE "can't succeed because transport is stopped"
  (`oa:driver/ua_audio.c:2837-2843`). Unknown for Gen 1. Detect: a warm start (driver disable/enable
  without a power cycle) that times out in ACEFACE. Fallback if it fails: run E7 (boot transport +
  early ACEFACE) on the warm path too, behind a per-unit value `WarmBootTransport=1`.
- **No inherited strips on the Twin X.** It ring-connects on every connect, so strips UA's driver built
  are lost on a takeover (`oa:CHANGELOG.md:50-52`; spec-bringup R1). Mic capture after any takeover
  needs our strips, which need a cold start (section 5).
- **Notification bank shift 0x28** is documented "for Apollo x4" but used for every model
  (`oa:driver/ua_apollo.h:368-409`); Gen 2 works with it. Detect: no ACEFACE answer at all.
- **What "safe" means here:** every failure ends with the transport stopped, the rings resynced, the
  MMIO gate open only for the 10 ms service pass, and the unit idle on our driver. Nothing in the path
  writes firmware, `DSP_RESET` (0x221C) or `full_shutdown` steps. Recovery is a Twin X power cycle,
  which never drops the x8p (the x8p is upstream).
- The boot transport's 4 MiB rings must exist before E7: ours are allocated in PrepareHardware
  (`driver/device.c:107-110`), so that holds.

---

## 3. Channel layout

### 3.1 What is read at runtime and what is assumed

| Item | Runtime | Assumed |
|---|---|---|
| Slot order and slot counts (AX_REC_CH, AX_PLAY_CH) | IO descriptors 0xC1A4 / 0xC2C4 after connect, torn-read protocol `oa:driver/ua_audio.c:991-1036` | nothing: the FPGA lays the frame out in descriptor order (**[HW]** x8p, `docs/spec-streaming.md:273-277`) |
| Channel names | descriptor channel IDs (type << 8 \| number, `oa:driver/ua_audio.c:906-911`) | Twin X naming from `oa:devices/apollo-twin-x.json:36-71` |
| Preamps, Hi-Z, talkback | readback (Hi-Z detect word 0 bit 24) | 2 preamps, Hi-Z on input 1, talkback mic (`oa:devices/apollo-twin-x.json:11-13`) |
| ADC latency | readback word 33 [28:26] board id | Twin X 5 frames at 48 kHz, board 1: 12 (`oa:driver/ua_models_gen.h:555-559`) |
| Clock sources | 0xC08C [31:16], names at 0xC090 | Internal, S/PDIF (`oa:devices/apollo-twin-x.json:20`) |

Today our driver hard-codes the x8p counts (`driver/audio.c:59-60`, `driver/regs.h:167-170`) and the
endpoint fallback slots (`driver/endpoints.c:27-31`, MON loopback at record 26-27). For the Twin X
the counts come from the descriptor and the fallback slots from a per-model table; `ApolloEndpointSlot`
(`driver/endpoints.c:41-64`) already maps by channel ID.

### 3.2 Expected Twin X order (to compare at gate G3)

No hardware capture exists; the open-apollo fixture is synthesized
(`oa:configs/pipewire/tests/twin-x-io_map-48k.txt:1`). By analogy with the x8p, whose descriptor is its
route list with S/PDIF inserted after ADAT (`docs/spec-streaming.md:258-271`), the Twin X descriptor
is expected (inference) to be its ADAT-mode route list (`oa:driver/ua_models_gen.h:234-239`) plus
S/PDIF:

| Record slot | ID | Name | | Play slot | ID | Name (jack) |
|---|---|---|---|---|---|---|
| 0 | 0x0001 | MIC/LINE/HIZ 1 (Hi-Z jack overrides) | | 0, 1 | 0x0901, 0x0902 | MON L/R (monitor outputs: Aaron's speakers) |
| 1 | 0x0002 | MIC/LINE 2 | | 2, 3 | 0x0A03, 0x0A04 | CUE 2 = LINE 3/4 outputs |
| 2, 3 | 0x0901, 0x0902 | MON L/R (monitor-mix loopback) | | 4-7 | 0x0701-0x0704 | VIRTUAL 1-4 |
| 4-7 | 0x0701-0x0704 | VIRTUAL 1-4 | | 8, 9 | 0x0A01, 0x0A02 | CUE 1 = headphones |
| 8-15 | 0x0401-0x0408 | ADAT 1-8 (optical in, ADAT mode) | | (10+) | 0x0C.. | RESERVED, if present (the x8p has 16) |
| 16, 17 | 0x0501, 0x0502 | S/PDIF L/R (optical in, S/PDIF mode) | | | | |
| 18-21 | 0x0201-0x0204 | AUX1 L/R, AUX2 L/R | | | | |
| 22, 23 | 0x0B01, 0x0B02 | TALKBACK 1, 2 | | | | |

So expect about 24 record and 10 playback slots (inference), against the route-list counts 16/22
record and 10 playback (`oa:devices/apollo-twin-x.json:23-31`). VIRTUAL 1-4 only at 44.1/48 kHz; ADAT
5-8 hidden at 88.2/96 kHz (`oa:devices/apollo-twin-x.json:21, 162`). HP follows CUE 1 by default
(`oa:driver/ua_audio.c:9612`). Talkback slots need the full descriptor count (on the x8p,
`AX_REC_CH = 34`).

What the speakers need: playback slots of 0x0901/0x0902. That path needs ring connect + transport +
clock write, no strips (`docs/spec-dsp-mixer.md:25`), verified on Gen 2 only (`oa:README.md:133`).

---

## 4. Preamp and monitor control

### 4.1 Settings and readback for 2 preamps (kind 15) versus the x8p

Mapping `oa:driver/ua_core.c:2536-2660`, readback `oa:driver/ua_apollo.h:842-890`. "ch" is 0-based.

| Control | x8p (8 preamps, `driver/mixer.c:114-171`) | Twin X Gen 1 (ch 0-1) | Readback |
|---|---|---|---|
| Mic/Line, PAD, 48V, LowCut, Phase | S0 (ch 0-3) / S12 (ch 4-7), bit 6*(ch&3) + {0,1,3,4,5} | S0 bits 0,1,3,4,5 (ch 0) and 6,7,9,10,11 (ch 1); **S12 unused** | word 0, same bits |
| Link | S0 mask 0x104 << 12*((ch&3)>>1), even ch | **ch 0 only** (`oa:mixer-engine/model_info.py:196-200`) | word 0 bit 2 |
| Gain 10-65 dB | GainA S17/S25, GainB S16/S24 byte lane ch&3, GainC S3/S14 6 bits | S17 / S16 lanes 0-1, S3 bits 0-5 / 6-11 | word 3 GainC, dB = GainC + 9 |
| Mic impedance (param 0x07) | S1 / S13 nibbles at 9/16/20/24 | S1 nibbles at 9 (ch 0) and 16 (ch 1); code-to-ohm table not in any source | |
| Custom (Unison) preamp 0x08 | S11 bit 8+ch | S11 bits 8, 9: **keep 0** (gain is refused while set, `oa:driver/ua_audio.c:6956`) | |
| Hi-Z impedance 0x11 | S13 nibble 4*ch, inputs 1-2 | S13 nibble 0, **input 1 only**: codes 0-7 = 2.2M, 1M, 453k, 232k, 100k, 70k, 47k, 21k | Hi-Z detect word 0 bit 24 |
| Monitor level | S2 [7:0], raw 0..192 = -96..0 dB | same | word 2 [7:0] |
| HP level | HP1 S2 [15:8]; HP2 not offered (S14 [7:0] = preamp 5-6 GainC on the x8p) | **one headphone**: S2 [15:8]; refuse HP2 level and HP2 source | word 2 [15:8] |
| Mute, mono, dim | S2 bits 16, 17, 31 | same | word 2 |
| Dim level | S14 [30:28] | same (S14 has no preamp fields on the Twin X) | |
| Monitor source | S2 bit 29 + [19:18] | same | |
| HP1 source | S12 [26:24] | same (`oa:driver/ua_apollo.h:735-736`) | |
| Talkback | S13 bit 8 | same, built-in talkback mic | word 6 bit 8 |
| Clock locked | | | **word 6 bit 31, valid when bit 30 set** (`oa:driver/ua_apollo.h:882-883`) |
| Kind-15-only extras | | 0x3E CR PGA bypass (S15 bit 9), 0x41 remote monitor controller (S15 bit 7), ALT monitor on LINE 3/4 (0x38-0x3C S15, 0x3A S13 [29:28]) (`oa:driver/ua_apollo.h:754-765`, gates `oa:driver/ua_core.c:1854-1858`) | word 6 [29:28] |

Driver changes: `X8P_PREAMPS` (`driver/mixer.c:15, 120`) becomes the model's preamp count; link only on
even channels that have a pair; `ApolloSetMonitorParam` (`driver/mixer.c:182-232`) refuses HP2 source on
one-HP models; `GET_INFO.NumPreamps` per model (`driver/ioctl.c:115`). 48V after a cold start needs the
0-then-1 edge for ch 0-1 (`oa:driver/ua_core.c:2458-2499`), which `strips.c` already does for any mask.

### 4.2 The monitor level after a cold start

The level lives in the unit and follows its knob (`oa:driver/ua_audio.c:8299-8301`); our driver never
writes S2 at connect (`driver/strips.c:298`). After a power cycle without UA's software, nobody pushes a
saved session, so the Twin X comes up at whatever its firmware restores (unknown). The module may also
ignore host level changes until the daemon's 57-param monitor block is sent
(`docs/spec-dsp-mixer.md` 6.1 C14). Rule for every Twin X gate: speakers turned down before the Twin X
starts on our driver; Apollo Control shows readback word 2 before anyone raises them.

### 4.3 Mixer-init batch on the Twin X

The connect batch writes 0x20 with mask 0xFF into S1-S37 except S2, plus S35 = 0x05
(`oa:driver/ua_audio.c:3276-3310`, ours `driver/strips.c:287-320`). On the Twin X the low bytes of these
hold (derived from 4.1): S3 GainC 1-2 (0x20 = 41 dB on input 1, as the x8p showed), S13 Hi-Z impedance,
S16/S17 GainB/GainA, S23 knob steps, S11 [7:0] output reference + mirror config, S14 [7:0] HP level
(0x66), S15 [7:0] ALT-monitor trim + remote-monitor bit, S8 [7:0] device-mode word (0x20 = converter
mode 0x20, TOSLink in and out S/PDIF, multi-unit role 0; `oa:driver/ua_apollo.h:1922-1929`).

`MixerInitBatch` becomes per unit, and value 1 becomes "model-safe" with a skip list per model:

| Model | Skipped settings (value 1) |
|---|---|
| x8p | 2, 3, 12-14, 16-31 (today's, `driver/strips.c:289-290`) |
| Twin X | **2, 3, 11, 13, 14, 15, 16, 17, 23** (preamp, HP, monitor-reference and ALT fields) |

Default for the Twin X: 1. A/B in gate G5 against 2 (legacy, what Gen 2 ran on Linux) only if capture
fails with 1.

### 4.4 Unison: what works without UA's software

- **Hardware side (works):** the two mic preamps and the Hi-Z input are ordinary, clean preamps under
  the mailbox: gain 10-65 dB (`oa:driver/ua_apollo.h:608-610`), 48V, pad, low cut, polarity, link,
  Hi-Z impedance codes. With the custom-preamp bit 0x08 clear the PGA gain is the host's gain.
- **What Unison adds:** a Unison plug-in in the strip's preamp slot takes ownership of
  the channel: open-apollo's host first resets the PGA to zero, waits, then sets CUSTOM, and from then
  on the plug-in's knobs drive the PGA gain and the input impedance through its own image of the
  settings (`oa:driver/ua_core.c:2692-2905`). The emulation itself (tube/transformer coloration, the
  model-specific gain curve and impedance choice) is a **UAD plug-in that runs on the unit's SHARC
  DSPs**. Our driver can host it once ported: open-apollo 2.1.0 runs UAD plug-ins from the user's own install,
  admitted by the Apollo's own licence answer, and its driver implements this handover
  (`oa:driver/ua_plugin_instance.c:1334-1484`), but its mapper does not map Unison preamps yet. Our
  port is [PLUGINS.md](PLUGINS.md): inserts first, Unison last (P8, after G5). Until then Unison is off
  on both units on our driver. We never ship UA's code.
- **Impedance:** without a Unison plug-in the mic input stays at the preamp's default impedance. Param
  0x07 can write an impedance code, but the code-to-ohm table for the mic input is in no source:
  do not expose it. Hi-Z impedance codes are known (table in 4.1) and can be offered for input 1.
- Until then, our own voice chain (`dsp/`, [DSP.md](DSP.md)) covers coloration and dynamics,
  host-side.

---

## 5. Input strips on Twin X Gen 1

### 5.1 What open-apollo's Gen 2 path does

Model table entry `{type, first_synch 0, first_virtual 0x18}` for 0x3B/0x3C/0x40/0x41
(`oa:driver/ua_strips.c:785-789`); strips on `input_strips_dsp` = **DSP 1** by default
(`oa:driver/ua_strips.c:129-132`); "both DSPs' pools read the recipe's geometry on a Duo ... code
0xe0000, tops 0x9d1b8 / 0xc6ffe / 0x08ffe000 ... six strips live after a cold start, Mic 1 records and
reaches the monitor mix" (`oa:driver/ua_strips.c:775-784`). Same Mic Input Strip recipe, same modules
(0xEB, 0xDA, 0xA5, 0xC2, 0x12B), same 48 kHz parameter words as the x4 and our x8p
(`docs/STRIPS.md` 2.5-2.7).

### 5.2 Extending our builder to Gen 1

| Piece | x8p today | Twin X Gen 1 | Known or unknown |
|---|---|---|---|
| Model entry | `{0x22, 0, 0x18}` | `{0x99, 0, 0x18}`: ANALOG 1/2 = synch 0/1, VIRTUAL = synch 24-31, TALKBACK = synch 10 (`oa:devices/apollo-twin-x.json:115-137`, identical to Gen 2) | layout known from UA's Mixer Engine tables; never run |
| Strip DSP | DSP 1 | DSP 1 (exists on Duo and Quad); strips on DSP 1 feed capture **and** the monitor mix (`oa:HARDWARE-SESSIONS.md:327-328`) | |
| Ownership | DSP 1 boot word & ~0xFF00 == 3 before ring connect (`driver/strips.c:455`) | same rule; read before E7. The model byte [15:8] is masked out, so its value does not matter | Gen 1 boot words unknown |
| Pool geometry check | passes on x8p DSP 1 (`docs/STRIPS.md` 2.4) | same check; refuse with nothing sent if the tops differ | unknown; gate G5 dry run |
| Modules | five files, pinned, from Aaron's UAD 11.5.2 (byte-identical to the 12.0.0.602 pins) | same files | known |
| StripMask | 0x01-0xFF | clamp to 0x03 (2 preamps); planner rows for inputs 3-8 never used | |
| Route words to the other DSPs | DSPs 0, 2-5 | DSP 0 (Duo) or 0, 2, 3 (Quad) | |
| 48V resync | preamps 1-8 | preamps 1-2, S0 bits 3 and 9 | |
| Hi-Z on input 1 | same strip | same strip: the strip sits on the DSP input, not on the jack (inference) | |
| Talkback mic | not built | needs the Talkback Input Strip (module 0xC9, `oa:driver/ua_strips.c:27-31`; Aaron's install has `UAD Talkback Input Strip Header.dll`, `apollo-win/ua-files/`); no captured recipe | unknown: talkback stays silent |
| Virtual 1-4 loopback | not built | needs the plain Input Strip 0x83E (`oa:driver/ua_strips.c:19-25`), dropped in v1 | |

**Feedback hazard specific to Aaron's setup.** A live strip puts the input into the Twin X monitor mix
at DSP 1's default level the moment it starts (`docs/STRIPS.md` 2.8), and the Twin X monitor outputs
are the speakers. A mic on the Twin X in the same room can feed back. The x8p never had this problem
(its monitors are not connected). Until the driver has the bus-coefficient path (input fader mute,
`{0x001D0004, bus, (side << 24) | param, float}` to the bus owner and DSP 0,
`oa:driver/ua_dsp.c:3790-3829`), Twin X strip gates run with the speakers off and headphones low.
Adding that mute (default: Twin X inputs muted in the Twin X monitor mix, like UA's daemon,
`docs/spec-dsp-mixer.md` 6.3 lines 3076-3116) is a prerequisite for leaving strips on in daily use.

**Power cycles:** strips need a cold start of the unit they are built on. A Twin X power cycle cold
starts only the Twin X; an x8p power cycle cold starts both (daisy chain), which is the one-step way to
get strips on both.

---

## 6. The combined device

### 6.1 Every x8p-only or single-device place today

| # | Place | What is single-device | Change |
|---|---|---|---|
| 1 | `driver/device.c:27-30` `ApolloSupported` | x8p only | model table (1.3) |
| 2 | `driver/hw.c:177-196` | no public-model retype | 1.2 |
| 3 | `driver/hw.c:213-218`, `include/apollo_ioctl.h:107-110` | one tested FW/FPGA pair | per-model table |
| 4 | `driver/device.c:268-349` | x8p bring-up only | section 2 branch |
| 5 | `driver/audio.c:59-65`, `driver/regs.h:164-170` | x8p counts, rate fields | descriptor counts per unit |
| 6 | `driver/audio.c:203, 275, 311` | no deferred ring connect, legacy P2P write, internal clock only | section 2; clock source per unit (6.3) |
| 7 | `driver/mixer.c:15, 36-58, 120, 182-232` | 8 preamps, one batch, no model gating | 4.1, 4.3 |
| 8 | `driver/strips.c:198-213` | `StripsEnable`, `StripSteps`, `StripMask`, `StripDsp`, `MixerInitBatch` read from the **service-wide** `Services\apollo\Parameters` | per-unit keys (6.9). **Danger today:** a Twin X bound to this build would inherit whatever was set for the x8p (`StripsEnable=1`, `StripMask=0xFF` in T6) |
| 9 | `driver/strips.c:289-290`, `driver/strip_plan.h:16` | x8p skip list, 8 inputs | per-model |
| 10 | `driver/engine.c:57-70` | `RenderLeadFrames`, `CaptureGuardFrames`, `StartTransportAtConnect` service-wide | per unit + aggregate |
| 11 | `driver/engine.c:481-483`, `driver/asio.c:44, 432`, `driver/apollo_stream.h:22` | x8p FRAME_CFG and ADC latency constants | per unit (FRAME_CFG is 8 at 48 kHz on both; ADC 10 vs 5) |
| 12 | `driver/engine.c` (whole), `driver/apollo.h:306-336` | one engine timer and one ASIO client per DEVICE_CONTEXT | aggregate engine (6.2) |
| 13 | `driver/circuit.c:154-169` | `EnabledEndpoints` service-wide | per unit + aggregate endpoint set |
| 14 | `driver/endpoints.c:20-32` | one x8p table, fallback slots 26-27 | per-model tables + aggregate table (6.7) |
| 15 | `driver/apollo.inf:31-32, 110-116` | only `SUBSYS_00141A00`; DeviceDesc, service and endpoint FriendlyName say "Apollo x8p" | 6.10 |
| 16 | `driver/log.c:14-54` | one global 64 KiB ring, lines carry no unit | keep one ring (interleaving helps aggregate debugging), prefix every line with the unit tag (`x8p`, `twx`), grow to 256 KiB |
| 17 | `driver/driver.c:237` | one interface instance per device: already multi-device | add HW_UID, model and role to GET_INFO so clients choose |
| 18 | `tools/apollo-diag/apollo-diag.c:214-285, 2263` | `--index N` exists | add `--unit x8p|twinx|<uid>` |
| 19 | `app/src/main.cpp:405-420`, `app/ui/app.js:13` | opens the first interface; `PREAMPS = 8` | enumerate all, unit tabs, Combined tab |
| 20 | `asio/avium-asio.cpp:27, 39-57, 172-195, 270-289` | name "Apollo x8p (Avium)", x8p name tables, first interface, one clock "Internal" | 6.6 |
| 21 | `include/apollo_asio.h:21-25` | 64 in / 64 out max | 128 / 128 |
| 22 | `tools/phase1/install.ps1:21, 50-54`, `revert.ps1:38-59` | x8p hwid only; revert deletes every package (both units) | 6.10 |
| 23 | `installer/helper/common.h:34-36`, `pnp.cpp:109, 289` | x8p hwid; comment says a Twin X is "SUBSYS 0x000F" (that is the Solo/Conrad pin) | 6.10 |

### 6.2 Kernel structure: one aggregate over two WDFDEVICEs

```
          WDFDRIVER context (lives from DriverEntry to unload)
          APOLLO_AGG
          +-- Mutex (WDFWAITLOCK, PASSIVE): join, leave, config, layout
          +-- Lock (KSPIN_LOCK): everything the tick touches
          +-- Timer (EX_TIMER_HIGH_RESOLUTION, 1 ms grid, as engine.c)
          +-- Member[0..3]: Ctx, Rundown (EX_RUNDOWN_REF), Uid, Role, Present, Joined
          +-- Map (6.3): master->follower frame mapping, ratio, loop state
          +-- Asrc[follower channels] (6.3.3), staging FIFOs
          +-- Route (6.8), Layout (logical channels), Asio client (moved from DEVICE_CONTEXT)
          +-- Stats
DEVICE_CONTEXT (x8p)                     DEVICE_CONTEXT (Twin X)
  BAR0, rings, DSPs, mixer, strips,        same, own bring-up work item,
  own transport (runs while powered)       own transport, own service thread
```

- **One driver, two devices.** Each unit keeps its own bring-up, transport, service thread, strips and
  IOCTL interface. Only the copy engine and its clients (ASIO, ACX streams) move to the aggregate.
- **Join:** at the end of a unit's bring-up work item (`driver/device.c:351-394`), when Connected and
  TransportRunning: take `Agg.Mutex`, stop that unit's own engine timer (`ApolloEngineStop`), record
  the member, recompute roles and layout, (re)start the aggregate timer.
- **Leave:** first thing in `ApolloEvtD0ExitPreInterruptsDisabled` (`driver/device.c:201-234`), before
  `ApolloStripsQuiesce` and `ApolloHwQuiesce`: mark the member not present, `ExWaitForRundownProtectionRelease`
  on its rundown (the tick holds the rundown while it touches that unit's BAR0 or rings), recompute.
  Its rings are freed only later in ReleaseHardware, so a running tick can never touch freed memory.
- **Surprise removal** (`driver/device.c:250-262`): the unit's gate closes at once (lock-free); the tick
  sees `ApolloGateClosed(member)` and treats that unit as absent from the next tick: capture channels
  read zero, render to it is discarded, no MMIO. The formal leave follows in D0 exit.
- **Lock order:** `Agg.Mutex` -> `StripLock` -> `DevLock` (members in index order) -> `Agg.Lock` ->
  member `EngineLock`. `AsioLock` moves into the aggregate and is never held with a DevLock (as today,
  `driver/apollo.h:4-15`). The tick takes only `Agg.Lock` and member `EngineLock`s, never a DevLock.
- **One tick for all units.** Per tick: for each present member, master first, read SAMPLE_POS bracketed
  by QPC (existing dead/slow logic per member, `driver/engine.c:388-452`); extend to 64 bits; zero-ahead
  each playback ring; update the clock map (6.3); render, then capture (6.4); notify clients. Members are
  read back to back, a few microseconds apart; the QPC brackets give the exact instants.
- **Single unit:** the aggregate with one member is today's engine (identity map, no ASRC). The
  x8p-only behaviour does not change.
- **Topology (daisy chain):** the Twin X sits behind the x8p's Thunderbolt switch. Consequences:
  powering off or unplugging the x8p removes **both** units (two surprise removals in any order); the
  Twin X can disappear alone. The **host unit** for the combined Windows endpoints is therefore the
  upstream unit, the x8p (6.7): it outlives the Twin X. Upstream is decided in user mode (Apollo
  Control / installer) from the PnP parent chain: the unit whose PCI bridge is an ancestor of the
  other's PDO is upstream; stored as `HostUnit` (6.9).
- **Roles:** `Master` = the timeline every client sees (ASIO, Windows streams). Default: the unit with
  the monitors, the Twin X. If the configured master is absent, the other unit is **acting master**
  until it returns.

### 6.3 Clock

#### 6.3.1 Options

| Mode | Wiring | Sample-locked | Bit-exact on both units | Status |
|---|---|---|---|---|
| **A. Software drift correction (primary)** | Thunderbolt only (Aaron's setup) | no: one unit resampled | master unit only | this spec |
| B. Optical lock | Toslink x8p optical OUT -> Twin X optical IN | yes | yes | option (6.3.6) |
| C. Thunderbolt peer clock (what UA does) | Thunderbolt only | yes | yes | research only (6.3.7) |

#### 6.3.2 Measuring the rate (mode A)

Each tick gives, per unit u, a 64-bit position `P_u` and the QPC instant `t_u` of the read (middle of the
QPC bracket). Project the follower to the master's instant: `P_f(t_m) = P_f + (t_m - t_f) * Fs_f / QPCfreq`
(the projection is at most a few microseconds, i.e. below 0.5 frame, and uses the follower's current
rate estimate).

- **Clock map:** `phi(m) = phi0 + r * (m - m0)`: master frame m -> follower frame (fractional, Q32.32).
- **Error:** `e = P_f(t_m) - phi(P_m)`, in follower frames. Sources: SAMPLE_POS granularity (whatever the
  FPGA's update step is: measure at G6), read jitter, real drift.
- **Loop:** second-order (PI) filter on `e` drives `r` (the DLL of zita-ajbridge / CoreAudio aggregate
  drift correction): acquisition bandwidth 1 Hz for the first 2 s, tracking bandwidth 0.05 Hz after.
  `r` is clamped to 1 +/- 500 ppm (quartz units are typically within +/- 50 ppm; the x8p reads
  47,999.85 frames/s against QPC, about -3 ppm, **[HW]** engine stats of release1, Nov 14). A required correction
  outside the clamp is a fault (6.3.4), not something to follow.
- **No QPC drift in the result:** `r` is measured position against position; QPC only aligns the two
  reads in time, so the PC's own clock error cancels.
- **Persisted:** the last tracked `r` per unit pair (`Aggregate\LastRatioPpb`) is the start value next
  time; temperature moves it by a few ppm at most, which the acquisition loop absorbs in well under a
  second.

#### 6.3.3 The adaptive resampler (ASRC)

**Where it runs: in the kernel aggregate tick (DISPATCH_LEVEL), not in the ASIO DLL.**

| | Kernel tick (chosen) | ASIO DLL (rejected as primary) |
|---|---|---|
| Serves ASIO, Windows endpoints, routing and mirroring | yes, one timeline for all | ASIO only; Windows endpoints would still need a kernel ASRC |
| Follower render deadline | the tick sees the follower's SAMPLE_POS each ms and writes its ring exactly `lead` ahead | the DLL sees only published halves: needs a larger follower lead |
| Latency model | one, published in the shared block | two (kernel + DLL) |
| CPU context | DISPATCH, must stay bounded | DAW audio thread (MMCSS), AVX allowed |

Float at DISPATCH: on x64 the compiler emits SSE and saving that state is unnecessary; AVX would need
`KeSaveExtendedProcessorState` (allowed at <= DISPATCH, never in the ISR) (**[WIN]** Microsoft Learn,
sources at the end). The ASRC uses **SSE2 only**, no x87, no AVX, and carries the
`_Kernel_float_used_` annotation.

**Filter:** polyphase windowed sinc (Kaiser), half-length `h` taps per side, 256 phases with linear
interpolation between adjacent phase rows (phase-quantization error far below the filter's own error).
Interpolated coefficients are computed **once per output frame** and applied to every follower channel
(all follower channels share the timing), so the per-channel cost is `2h` multiply-adds per frame.

| Setting | `h` | Taps | Added capture latency (look-ahead) | Use |
|---|---|---|---|---|
| Low CPU | 8 | 16 | 8 frames | if the budget check fails |
| **Default** | 16 | 32 | 16 frames (0.33 ms) | daily use |
| High | 32 | 64 | 32 frames (0.67 ms) | critical recording on the follower |

The filter is symmetric and evaluated around the exact mapped position, so it adds delay only where
its `h` future input frames are not yet available: on **capture** (follower frames must have arrived).
On **render** the input is master-timeline staging that clients filled `lead` (144) frames ahead, so
the look-ahead is already there and the follower's output latency equals the master's.

Design target for Default: flat to 20 kHz within 0.01 dB, image/alias and interpolation products at or
below -100 dBFS for a full-scale 1 kHz tone (to be **verified** with Avium Measure at gate G6: sweep and
THD+N through a follower channel, compared with the same channel unresampled in mode B or on the
master). The ratio is within +/- 500 ppm of 1, so the filter acts as a slowly moving fractional delay:
no band-limiting for a rate change is involved.

**Cost and budget:** per tick `F * (2h + N * 2h)` multiply-adds, F = frames per tick (48 at 48 kHz), N =
**active** follower channels (channels with an open ASIO buffer, an ACX stream or a route; idle channels
cost nothing). Default h = 16:

| N active follower channels | MACs per tick (derived) | Expected time per tick (estimate, SSE2) |
|---|---|---|
| 4 (two mics + mains) | 7.7k | under 5 us |
| 20 | 32k | about 10 us |
| 66 (every x8p input and output) | 103k | 20-40 us |

Microsoft's DPC guidance is about 100 us. Rules: measure every tick (`AsrcLastUs`, `AsrcMaxUs`, 99.9th
percentile in GET_AGG_STATE); a tick processes at most 4 ms of backlog (a late tick catches up over the
next ticks within the margin); if the 99.9th percentile exceeds 60 us for 10 s, the driver logs it and
Apollo Control offers Low CPU or fewer follower channels. It never changes quality silently. Fallback if
the laptop cannot meet the budget even at Low CPU: move the ASRC into a real-time kernel thread woken by
the tick, at the cost of +1 ms margin on follower channels.

**Buffers:** per active follower channel a history ring (capture: follower frames; render: master-timeline
staging frames), 4096 float32 each, nonpaged, allocated at PASSIVE when the layout changes (not in the
tick).

**Margin:** the resampler reads follower capture data no closer than `guard + h + M` frames behind the
follower's SAMPLE_POS, and writes follower playback up to `FRAME_CFG + M` ahead of it (reading staging up
to `h` frames past each evaluation point; requires `lead >= FRAME_CFG + M + h + 48`, i.e. 88 <= 144 at
the defaults), with `M = 16` frames (0.33 ms) of jitter margin by default. The loop keeps `e` near 0; M
absorbs read jitter, SAMPLE_POS granularity and tick lateness.

#### 6.3.4 Startup, glitches, loss

- **Formation:** `phi0 = P_f(t_m) - P_m`, `r` = persisted value (else 1). Follower channels stay muted
  (zeros in capture, nothing written to the follower ring by clients) while the loop acquires: unmute
  when `|e| < 0.5` frame for 250 ms continuously, then a 10 ms fade-in. Typical: 0.3-1 s; a first-ever
  run without a persisted ratio: about 2 s. Master channels run from the first tick.
- **Late tick** (DPC delayed): positions are 64-bit and wrap-corrected (`driver/engine.c:454-468`), so
  the ASRC simply processes more frames next tick; inside M nothing is audible.
- **Position jump** (a unit's transport restarted, engine recovery, link stall): `|e| > M - 4` frames ->
  **resync**: follower channels go to zero at once, `phi0` is re-anchored, `r` kept, fade-in after 20 ms.
  Master channels and the client timeline are not touched. Counted in `AggResyncs`.
- **Ratio outside the clamp, or 3 resyncs within 10 s:** follower channels stay muted, state
  `Degraded`, log and Apollo Control banner. Clients keep running on the master.
- **Master lost** (Twin X off): the follower becomes acting master; the client timeline changes clock
  domain, so ASIO gets `kAsioResetRequest` and Windows streams continue with their positions rebased to
  stay monotonic (one short gap). On the Twin X's return, the same in reverse.
- **Follower lost:** follower channels silent; nothing else changes; on return it re-forms as above.

#### 6.3.5 Latency per channel and how ASIO reports it

ASIO 2 reports one input and one output latency for the whole driver (`getLatencies`); there is no
per-channel value. So the driver **aligns** by default: every unit's channels are delayed to the slowest
unit, per direction, and the single reported number is then true for every channel. Multi-unit
recordings are sample-aligned and the DAW's delay compensation is right.

Per-unit latencies in master frames at 48 kHz, default settings (derived; guard 16, lead 144, FRAME_CFG 8,
ASRC h 16 + M 16):

| | Twin X (master) | x8p (follower, resampled) | Aligned (reported) |
|---|---|---|---|
| Input, excluding the ASIO buffer | ADC 5 + guard 16 = **21** | ADC 10 + guard 16 + 16 + 16 = **58** | 58 |
| Output, excluding the ASIO buffer | lead 144 + FRAME_CFG 8 = **152** | 144 + 8 = **152** (look-ahead covered by the lead, 6.3.3) | 152 |
| ASIO at 128 frames | | | in 186 (3.9 ms), out 280 (5.8 ms) |

Today's x8p-only figures are 154 / 280 at 128 (`docs/ASIO.md`): the combined device costs 32 input
frames (0.67 ms) and no output frames. DAC latencies are not in these sums: the
model tables give output latency rows in unstated units (x8p line 39, Twin X line 44.3 at 48 kHz,
`docs/spec-streaming.md` 3.6); a per-unit `RenderTrimFrames` / `CaptureTrimFrames` (default 0) lets a
measured value (Avium Measure, loopback cable) correct the alignment later.

- `Align=0` (Apollo Control: "lowest latency, unaligned"): no padding; `getLatencies` reports the
  master's figures, which are then wrong for follower channels. Only for live use; Apollo Control says so.
- Every channel's own latency, resampled flag and unit are in the shared block's channel table (6.6) and
  in Apollo Control, so the numbers are visible even though ASIO cannot carry them.
- Changing the mode, `h`, the master or `Align` changes the reported latency: the DLL sends
  `kAsioLatenciesChanged`.

#### 6.3.6 Optical lock (option, if a Toslink cable is added later)

- **Only one direction exists:** x8p optical OUT -> Twin X optical IN. The Twin X has no digital output
  (3.2), so the Twin X can only follow. The client timeline can still be the Twin X: in lock both
  counters advance at exactly the same rate, so the master choice changes nothing and no ASRC runs.
- **x8p side:** the optical output must carry S/PDIF (device-mode word S8 [7:0] bit 0x08 clear;
  `oa:driver/ua_apollo.h:1922-1929`). The x8p class defaults to ADAT on that port
  (`oa:mixer-engine/model_info.py:36-38`); our current connect batch writes S8 = 0x20, i.e. S/PDIF out,
  on the x8p (derived from 4.3; unverified on hardware).
- **Twin X side:** clock source S/PDIF = 0x00, offered per UA's tables (`oa:devices/apollo-twin-x.json:20`);
  ADAT 0x05 is not in that table: use it only if the Twin X's 0xC08C mask has bit 5. The optical input
  type is the device-mode bit 0x10 (clear = S/PDIF), which open-apollo never verified
  (`oa:driver/ua_audio.c:1198-1201`).
- **Switching:** the clock-change primitive (`docs/spec-streaming.md` 2.8): 0xC074 =
  `(rate_index << 8) | 0x00`, ack bits cleared, AX_CONNECT = 4, poll 0xC030 for the ack up to 2000 ms.
  "Source 0 is S/PDIF, which never acknowledges without a signal" (`oa:driver/ua_audio.c:5162-5164`):
  no cable = no ack.
- **Automatic detection (`ClockMode=Auto`):** once per Twin X arrival, after its bring-up and before
  any client audio reaches its outputs, the driver tries the switch. Lock = all of: ack within 2 s;
  0xC07C [1:0] == 1 with [15:8] == the expected rate index (`oa:driver/ua_audio.c:2008-2039`); readback
  word 6 bit 31 with bit 30 set; then the two SAMPLE_POS counters keep a constant difference (+/- 1
  frame) for 2 s. Lock -> mode B (ASRC bypassed, `phi` = integer offset, `r` = 1 exactly). No lock ->
  switch the Twin X back to Internal 0x0C, mode A. The probe can click the Twin X outputs once; it is
  skipped while a render stream is active and can be run on demand from Apollo Control.
- **Watching the lock:** the 10 ms service pass already reads 0xC07C (`driver/mixer.c:253`). Lock bits
  lost, or the counter difference moving by more than 2 frames: switch the aggregate to mode A at once
  (the ASRC starts at `r` = 1, which is exact at that instant), then put the Twin X back on Internal after
  500 ms without lock.
- **Bonus once the cable exists:** a bit-exact cross-unit test without analog cables (x8p S/PDIF
  playback slots -> Twin X S/PDIF record slots), and a zero-latency hardware monitoring path (x8p
  Digital Mirror, param 0x1E, sends its monitor mix to S/PDIF; on the Twin X a strip on the S/PDIF
  inputs, synch 8/9, mixes it into the speakers). The latter needs the 0x83E Input Strip (5.2).
- UA's guidance against linking ADAT between Apollos (**[UA-WEB]**) is about UA's own multi-unit mode,
  which already clocks over Thunderbolt. Under our driver that mode is not used. If both units ever go
  back to UA's driver as a multi-unit system, unplug the cable.

#### 6.3.7 Thunderbolt peer clock (promoted Nov 14: first thing to try at G6, ASRC is the fallback)

**Result at G6 (Nov 15, 6.9.5):** source 0x0A is accepted by both units but, with no peer-to-peer path
programmed, the unit loses lock and free-runs (drift unchanged). The P2P programming (peer BAR0 bus address in
0x2280/0x2284 + 8*slot, enable mask, PLL phase machine) is not public and a wrong address faults the IOMMU
(0xE6 under Verifier): not attempted. The ASRC (6.3.3) runs the combined device.

**Update Nov 14 (Aaron's UAD Console feature compilation, `docs/console-parity.md`):** UA states that in
a multi-unit system "all units clock from the monitor unit over Thunderbolt", that Word Clock/ADAT must
not be connected between units, and that expanders show EXT on their panels. So the hardware can be
sample-locked with no cable. Plan: at G3 record the Twin X clock mask on a live (not stale) read and
whether 0x0A is offered; at G6 try peer clock (master = monitor unit at Internal, follower at 0x0A with
AX_CONTROL 0x20F) before building the ASRC; the ASRC (6.3.3) stays as the fallback if the sequence
cannot be found. Unknowns below still apply.

UA combines units with no cable at all (**[UA-WEB]**; **[UA-LOG]** 2023-07-31, clock "Internal"). The
pieces in the source: clock source 0x0A "peer/slave: hidden from the Clock Source menu"
(`oa:driver/ua_apollo.h:928`), offered by the x8p **[HW]** (mask bit 10); AX_CONTROL 0x20F "for a unit
clocked from a peer" (`oa:driver/ua_audio.c:4040-4046`); peer-to-peer route and slot registers
0x224C / 0x2280 / 0x2284 / 0x22C4, which Linux never sets (`oa:driver/ua_apollo.h:290, 304-311`);
multi-unit role and order bits in the device-mode word (`oa:driver/ua_apollo.h:1926-1927`); a PLL phase
pause "multi-unit; never set yet" (`oa:driver/ua_ioctl.h:698`). The sequence and whether it needs PCIe
peer-to-peer DMA between the two units (which DMA remapping may block) are unknown. It would remove the
ASRC entirely; it needs reverse engineering of UA's driver first.

### 6.4 Position and period alignment

- **Client timeline = master frames.** ASIO periods and ACX packet positions count master frames.
- **Master channels:** read and written directly in the master ring, as today (render clients ADD with
  saturation, `driver/apollo_sat.h`).
- **Capture head (all channels):** `H = P_m - guard - pad_m` master frames, with `pad_m` = aligned input
  latency minus the master's own (37 frames at the defaults, 6.3.5). Master channels are read from the
  master ring at `x`; follower channels for master frame `x` are the ASRC's value at follower position
  `phi(x) - d_f` (`d_f` = ADC difference in frames, 5 at the defaults), which needs follower data up to
  `phi(x) - d_f + h <= P_f - guard`; the margin M keeps that true. The ASRC writes these into the capture
  staging FIFO, clients read both kinds up to `H`.
- **Follower render:** clients ADD into a master-timeline staging FIFO (same zero-horizon rule as the ring,
  `driver/engine.c:470-478`); the ASRC writes follower ring frame `k` up to `P_f + FRAME_CFG + M`, from
  staging around `phi^-1(k)`.
- **Period completion:** an ASIO half or an ACX packet is complete when the master head passes it **and**
  the follower ASRC has produced the same frames. Both happen in the same tick by construction (the
  follower read point trails by a fixed margin), so periods are never held back by the follower.
- **Mode B:** `phi` is the integer offset measured at formation; the ASRC is a plain copy with that offset.

### 6.5 Start and stop ordering

- Transports are **never** started or stopped by the aggregate. Each unit starts its transport at
  bring-up and keeps it running while powered (PLAN streaming design, `driver/device.c:367-379`). Joining
  and leaving only change who reads the rings.
- **Formation order:** Mutex; each joining unit's own engine timer stopped under its DevLock (index order);
  positions snapshot; clock map initialised (6.3.4); aggregate timer started; layout version bumped.
- **Rate changes** (not in v1; 48 kHz only on both): dissolve, change each unit with its own sequence,
  re-form.
- **Sleep:** both units leave in D0 exit; on resume each brings itself up and joins.
- **Arrival order does not matter.** Either unit may come up first; the other joins when ready. With the
  Twin X as configured master arriving second, the timeline switches to it (ASIO reset, 6.3.4).

### 6.6 ASIO: one driver for both units

- **Name:** "Apollo (Avium)" (was "Apollo x8p (Avium)", `asio/avium-asio.cpp:27`). DAW preferences that
  named the old driver need one re-selection.
- **Open:** enumerate every `GUID_DEVINTERFACE_APOLLO` instance (CM_Get_Device_Interface_List), open the
  host unit's (any member works: the aggregate IOCTLs route to the driver-global aggregate), then
  `AGG_GET_LAYOUT`. Register `CM_Register_Notification` for arrivals and removals.
- **Channels:** the union, in the configured order (default: master unit first, as UA lists the monitor
  unit first; open question 5). Names carry the unit: `TwinX MIC/LINE/HIZ 1`, `x8p MIC/LINE 3`,
  `TwinX MON L` (out). Limits raised to 128 in / 128 out (`include/apollo_asio.h:22-23`); today's union is
  about 34 + 24 inputs and 34 + 10 outputs on the current layouts (derived).
- **Absent unit:** the channel list stays as configured (DAW routings survive a Twin X power-off);
  its channels are silent and flagged absent in the channel table. The list changes only when the
  configuration changes: then `kAsioResetRequest`.
- **Latency:** `getLatencies` = buffer + aligned figures (6.3.5); per-channel figures in the shared
  block's channel table: `{unit, slot, channelId, latencyFrames, resampled, present}` per channel.
- **Clock sources (`getClockSources`):** one entry per meaningful choice, `isCurrentSource` from
  `AGG_GET_STATE`:
  0 "Twin X (master), x8p drift-corrected", 1 "x8p (master), Twin X drift-corrected",
  2 "Optical lock: x8p clocks the Twin X" (listed only when a lock probe succeeded).
  `setClockSource` -> `AGG_SET_CONFIG` (master / mode). A master change changes the clock domain, so
  the DLL then sends `kAsioResetRequest`; a change that only moves the latency (mode A <-> B, `h`)
  sends `kAsioLatenciesChanged`.
- **Rate:** 48 kHz only (both units).
- **Shared block (APOLLO_ASIO_VERSION 3):** in/out indices become logical channel numbers (USHORT), plus
  `LayoutVersion`, `ClockMode`, `MasterUnit`, `RatioPpb`, `Resyncs` and the channel table page.
- Kernel side: the ASIO client moves from `DEVICE_CONTEXT` (`driver/apollo.h:152-191, 335-336`) to the
  aggregate; one client at a time, as today.

### 6.7 Windows endpoints

- **Same configuration as ASIO.** In Combined mode the endpoints are defined by the aggregate layout and
  created as ACX circuits on the **host unit** (the x8p, upstream, 6.2). Their streams run on the
  aggregate timeline; follower channels pass through the ASRC like ASIO's.
- **The other unit creates no circuits** of its own while Combined is on (no duplicates). With Combined
  off, each unit creates its standalone per-model table (`MON L/R (Apollo Twin X)` etc.).
- **Default set (Combined):** `Monitor` (render, 2 ch) routed by the config (default: Twin X MON L/R,
  i.e. the speakers, no ASRC); `Monitor` capture loopback; one mono capture per preamp input of both units
  plus stereo pairs (x8p 1-8, Twin X 1-2). The endpoint set (which circuits exist) stays an admin
  registry value plus a host-unit restart, as `EnabledEndpoints` is today (`docs/PHASE2.md` 3.5).
- **Names:** pin names carry the unit (`TwinX MIC/LINE 1`); the parenthetical interface FriendlyName is
  "Apollo" for the aggregate circuits (new INF reference strings, 6.10).
- **Absent unit:** circuits stay (they live on the host); its channels stream silence. Only the host's
  removal removes the endpoints, and that removes the Twin X too.
- **Default device:** the combined `Monitor` playing to the Twin X speakers is the endpoint Aaron will
  want as default. Install keeps restoring the previous defaults (`docs/PHASE2.md` 9.4); Apollo Control
  offers "Make Monitor the default output" explicitly.

### 6.8 Routing: what lives where, and what Apollo Control writes

| Layer | Does | Latency | Spans units |
|---|---|---|---|
| **Engine routing matrix** (kernel tick) | logical output -> one or more (unit, slot) destinations, ADD with saturation (mirroring = several destinations); logical input <- (unit, slot), aligned and resampled | host path (6.3.5); follower inputs + `h` + M | yes |
| **DSP mixer, per unit** (mailbox, existing IOCTLs) | monitor level/mute/dim/mono, monitor source, HP source, CUE mirror (0x2E/0x2F), Digital Mirror (0x1E), ALT monitor, talkback, input faders once the bus path exists | zero (on the unit) | no (except through an optical cable, 6.3.6) |

Presets Apollo Control ships (each is just a routing matrix):

| Preset | DAW mains (ASIO out 1-2) | Windows `Monitor` | Notes |
|---|---|---|---|
| **Studio (default)** | Twin X MON L/R | Twin X MON L/R | both ADD into the speakers' slots, like UA's driver lets Windows and the DAW play together |
| Mirror | Twin X MON L/R + x8p MON L/R | same | second pair of monitors or a recorder on the x8p |
| Headphones split | Twin X MON L/R | Twin X CUE 1 (headphones) | Windows audio on the Twin X headphones only |

Cross-unit input monitoring (an x8p mic heard on the Twin X speakers) goes through the engine:
aligned input latency + output latency without any ASIO buffer, 58 + 152 = 210 frames (4.4 ms) at the
defaults (derived from 6.3.5). Zero-latency cross-unit monitoring needs the
optical cable (6.3.6) or mode C.

**What Apollo Control writes:**
- `AGG_SET_CONFIG` (APOLLO_USER, like the monitor controls today): members and order, master, clock mode
  (`Software`, `Auto`, `Optical`), `Align`, ASRC quality, channel order and names, routing matrix. The
  driver validates it, applies it at the next tick boundary, and **persists it itself** under the
  aggregate key (6.9), so the app needs no elevation.
- Per-unit DSP controls: the existing `SET_INPUT_PARAM` / `SET_MONITOR_PARAM` on that unit's handle
  (unit tabs in the UI; the monitor section defaults to the master/monitor unit).
- Admin only (registry + restart, elevated helper): the endpoint set, `Mode` per unit, strips values.

### 6.9 Configuration storage and IOCTLs

**Per-unit registry keys** (replace the service-wide reads in `driver/strips.c:198-213`,
`driver/engine.c:57-70`, `driver/circuit.c:154-169`):

```
HKLM\SYSTEM\CurrentControlSet\Services\apollo\Parameters
    (values here)                  legacy globals: apply to the x8p ONLY (compatibility)
    Units\<UID16hex>\              per unit, keyed by BAR0 HW_UID (0x34:0x30)
        Mode                       0 observe (identify + quiesce only), 1 bring-up + transport,
                                   2 full (default: 2 for the x8p, 0 for any other model)
        WarmBootTransport
        StripsEnable, StripSteps, StripMask, StripDsp, MixerInitBatch
        RenderLeadFrames, CaptureGuardFrames, StartTransportAtConnect
        EnabledEndpoints (standalone), CaptureTrimFrames, RenderTrimFrames, Label
    Models\<type hex>\             optional model defaults (e.g. Models\99)
    Aggregate\
        Enabled, HostUnit, MasterUnit, ClockMode, Align, AsrcQuality,
        Order (REG_BINARY), Routing (REG_BINARY), EndpointSet, LastRatioPpb
```

Lookup: `Units\<uid>` -> `Models\<type>` -> built-in default; the legacy globals only for type 0x22.
This removes the hazard in 6.1 row 8: a newly bound Twin X starts in `Mode=0` with strips off, whatever
the x8p's values are.

**IOCTLs (APOLLO_IOCTL_VERSION 6):**

| Code | Name | Access | Purpose |
|---|---|---|---|
| 0x00 | GET_INFO | user | + `HwUid`, `ModelName[24]`, `Mode`, `AggRole` (standalone/master/follower/acting master), `AggId` |
| 0x30 | AGG_GET_STATE | user | members, presence, roles, clock mode and lock, `RatioPpb`, `e` (frames, filtered), resyncs, `AsrcMaxUs`, per-unit latency, `LayoutVersion` |
| 0x31 | AGG_GET_LAYOUT | user | logical channel table (6.6) |
| 0x32 | AGG_SET_CONFIG | user | 6.8; returns the new `LayoutVersion` |
| 0x33 | AGG_CLOCK_PROBE | user | run the optical lock probe now (6.3.6) |
| 0x20-0x23 | ASIO_* | user | unchanged codes, version 3 structures, aggregate client |

Any member handle reaches the aggregate; per-unit IOCTLs act on the handle's own unit.

#### 6.9.1 As built in G0-G1 (builds\work-twinx, Nov 14)

Code: `driver/apollo_model.h` (identification, shared with apollo-diag and `tools/ident-test`),
`driver/apollo_unit.h` (value names, defaults, lookup order, Mode rules; also host-tested),
`driver/unit.c` (registry reads, log). Callers: `strips.c` (`ApolloStripsPrepareHardware`), `engine.c`
(`ApolloEngineReadConfig`), `circuit.c` (`ApolloAcxPrepareHardware`), all through
`ApolloUnitParam(Ctx, APOLLO_PARAM_*)`. Not yet per unit: GET_INFO fields (layout unchanged), log tags,
`apollo-diag --unit`, the aggregate.

**Unit key name** (`ApolloUnitKeyName`): HW_UID read at identify, BAR0 0x34 then 0x30, as 16 upper-case
hex digits (`Units\0123ABCD89EF4567`). If the HW_UID reads 0 or all ones: `SN` + the serial when it has
8-16 characters, all letters or digits (`Units\SN20000000000001`). Otherwise no unit key (model, legacy
and built-in values only). The driver log names it at every start:
`unit: Units\<key> (HW_UID|serial) present|absent; Models\22 present|absent; legacy globals apply (x8p)`.

**Lookup, per value:** `Units\<key>` -> `Models\<TT>` (TT = device type in hex: `22` x8p, `99` Twin X Gen 1)
-> legacy global `Parameters\<value>` **only when the model is the x8p** -> built-in default of the model.
Then the Mode rules. The log line `unit values: Mode=2/D StripsEnable=1/G ...` gives each value and its
source: D built-in, U unit key, M model key, G legacy global, X forced by Mode.

| Value (REG_DWORD) | x8p default | other models | Read by |
|---|---|---|---|
| `Mode` | 2 | **0** | device.c (bring-up or not) |
| `StripsEnable` | 0 | 0 | strips.c |
| `StripSteps` | 1 | 1 | strips.c |
| `StripMask` | 0x01 | 0x01 | strips.c |
| `StripDsp` | 1 | 1 | strips.c |
| `MixerInitBatch` | 1 (x8p-safe) | **0** (no model-safe batch for the Twin X yet, 4.3); **G3: the Twin X 1** (its own skip list, 6.9.3) | strips.c |
| `RenderLeadFrames` | 144 | 144 | engine.c |
| `CaptureGuardFrames` | 16 | 16 | engine.c |
| `StartTransportAtConnect` | 1 | 1 | engine.c |
| `EnabledEndpoints` | 0x3FF | **0** | circuit.c |
| `StripProbe` | 0 | 0 | strips.c (live tree's StripProbe, STRIPS.md 10.9; wired at the merge) |
| `WarmBootTransport` (G3) | 0 (ignored: no boot transport) | 0 | device.c: 1 = the Twin X runs E7-E13 on a warm start too |
| `AudioRingPages` (G4b) | 0 | 0 | audio.c: 0 = each 4 MiB audio ring one contiguous common buffer if one is left below 4 GiB, else 1024 page common buffers + one linear view (driver/ring_map.h); 1 = always pages. Log: `playback ring: ...` / `record ring: ...` at every start |

**Mode as built:** 0 observe = identify + quiesce (transport stop, IRQs masked, rings resynced), then
idle: no ring/audio memory, no bring-up, no strips, no endpoints; Stage stays 1 (Mapped), LastStatus 0.
1 = bring-up + transport, with `StripsEnable` and `EnabledEndpoints` forced to 0. 2 = full. Any other
value = 0. Only the x8p has a bring-up path in this build: a Twin X stays in observe whatever its Mode
says, and the log says so (`... bring-up (section 2, gate G3) is not in this build: observe only`).
(Superseded at G3, 6.9.3: the Twin X Gen 1 runs Mode 1; a Mode 2 asks for comes down to 1.)

**Refused models** (apollo_model.h; Twin X Gen 2, Solo, Arrow, x4, x8p Gen 2, ...): log line
`model: <name> (type 0x.., public 0x.., family n, n DSPs): REFUSED: <why>`, quiesced, Stage 0x100
(Failed) with FailedStage 1 and LastStatus 0xC00000BB, GET_INFO `DeviceType`/`PublicType` = the resolved
types, never an accepted model's. Firmware/FPGA outside the tested table (x8p: 11.15.54803 / 0xA241C5AC;
no Twin X row until G3) sets `APOLLO_FLAG_FW_UNTESTED` and a log warning only.

**Serial:** GET_INFO `Serial` and the log carry only the valid part: the text up to the first byte that
is not printable ASCII, without trailing punctuation (the x8p: `20000000000001`, was `20000000000001??`).
Digits 5-8 must be four ASCII digits for a Twin X to be accepted (else a Gen 2 cannot be ruled out); on
the x8p a bad serial is logged and the subsystem fallback still applies, as upstream.

**Endpoint objects:** circuits are still created at device add from the legacy global `EnabledEndpoints`
(or 0x3FF), before the unit is known; they stay invisible until PrepareHardware adds those in the unit's
own value. So a unit key can narrow the x8p's set, not widen it past the global value.

**Examples** (admin; take effect at the unit's next start; strips need a cold start):

```
reg add HKLM\SYSTEM\CurrentControlSet\Services\apollo\Parameters\Units\<key> /v Mode /t REG_DWORD /d 0 /f
reg add HKLM\SYSTEM\CurrentControlSet\Services\apollo\Parameters\Models\99 /v Mode /t REG_DWORD /d 0 /f
```

**Scripts (tools/phase1):** `revert-unit.ps1 -Unit twinx|x8p` or `-InstanceId <id>` moves one unit back to
UA (devcon update with UA's `uad2pcie.inf`, signed by Microsoft, and the exact four-part hardware ID;
pnputil remove + rescan if no verified devcon; our package is kept; the other unit is checked unchanged).
`new-revert-shortcut.ps1` makes the desktop shortcut "Twin X back to UA" (runs `revert-unit.ps1 -Unit
twinx -Yes` as administrator in a window that stays open). `install.ps1` refuses any hardware ID but the
x8p's, records every other Apollo's driver before and checks it after, and keeps packages another unit
uses. `revert.ps1` stops before changing anything if another unit is on our driver, unless `-AllUnits`.

#### 6.9.2 As built for gate G2 (builds\work-g2, Nov 14)

- **INF:** second model line `SUBSYS_00191A00` -> `Apollo_TwinX_Install` ("Apollo Twin X (open driver, test
  build)"): same CopyFiles, HW AddReg, service and Wdf sections, **no Interfaces section**. The x8p's
  sections are unchanged.
- **No circuits for the Twin X:** device add reads the PnP hardware ID (`driver.c ApolloReadHardwareId`,
  `apollo_unit.h ApolloHwidSubsystem`). Subsystem 0x0014 or an unreadable ID: circuits from the legacy
  `EnabledEndpoints` as before; any other subsystem: none (`ApolloAddTimeEndpoints`, ident-test section 5).
  PrepareHardware still adds nothing below Mode 2 or for a model that is not the x8p.
- **Observe (Mode 0, and any Twin X in this build):** identify, HW_UID, quiesce, then the notification
  block as UA left it (rate info, clock mask, both IO descriptors, every word in the log as `STALE`),
  nothing else: no rings, audio memory, strips, engine, service thread, timer resolution or endpoints.
  User IOCTLs other than READ_REG, GET_POSITION, GET_READBACK, GET_LEVELS, GET_STRIPS (and the lock-free
  info/log/engine/endpoints set) fail with STATUS_INVALID_DEVICE_STATE; ASIO_OPEN fails too.
- **GET_INFO version 6:** appended `HwUidLo/Hi`, `Model`, `Mode`, `UnitFlags` (RESOLVED, ACTIVE, OBSERVE,
  REFUSED, OWNER_READ, UNIT_KEY, MODEL_KEY), `AggRole`, `AggId`, `HwidSubsystem`, `ModelName[24]`,
  `UnitKey[20]`, `LogTag[8]`, `Owner*`. The driver answers any buffer of at least `APOLLO_INFO_V5_BYTES`
  with as much as fits, so version 5 clients keep working.
- **Log:** one 256 KiB ring; per-unit lines `[  t] [x8p] ...` / `[twx]`; `apollo-diag --unit twinx log`
  shows that unit's and driver-wide lines (`--all`: everything).
- **Clients:** `include/apollo_select.h` lists units through short read-only opens and GET_INFO.
  apollo-diag `--unit x8p|twinx|N` (default the x8p, else the first) and `units`; ASIO, asio-test,
  wasapi-loop, loopback-test: the x8p only; Apollo Control: x8p in slot 0, other units read-only.
- **Scripts:** `install.ps1 -Unit twinx` (writes `Models\99\Mode=0` first, refuses a package other than
  the one the x8p runs), `step-g2.ps1`, `revert-unit.ps1 -Unit twinx` unchanged.

#### 6.9.3 As built for G3 (builds\work-g3, Nov 14; not installed)

**The plan, as data.** `driver/bringup_plan.h` (plain C, shared with `apollo-diag` and
`tools/bringup-plan-test`) holds every per-model decision: path (ring connect / AudioExtension),
highest Mode, boot transport and its counts, early ACEFACE timing, where the ring connect runs and
whether it is checked, where the transport counts come from, the AX_P2P_ROUTE write, the rate
confirmation, the batch-1 skip list, the warm fallback, and what else may run (endpoints, ASIO,
strips, IOCTL classes per Mode). `bringup-plan-test` (783 checks, in `tools/build-all.cmd`) proves
the x8p row equals the pre-G3 code value for value (regs.h constants, the old `kX8pSkip` batch in
every mode, the P2P write, no consumption check) and the Twin X row equals section 2 and 4.3.

| Decision | x8p (unchanged) | Twin X Gen 1 |
|---|---|---|
| Path, highest Mode | ring connect, 2 | AudioExtension, **1** (a Mode 2 key runs as 1, logged) |
| Cold start | E1-E6, E11 ring connect before ACEFACE, E12 | E1-E6, **E7 boot transport** (3 play / 2 rec, FRAME_CFG 0x10, P2P 0x102, AX_CONTROL 0x20F), **E8** early ACEFACE 2 x 10 x 50 ms, DMA pulse + 50 ms + ring pages, **E10**, E12, **E13** |
| Warm start | F, no ring connect (keeps the DSP) | F, F9 (AX_CONTROL 0); `WarmBootTransport=1` runs E7-E13 too |
| Audio connect (G5) | ACEFACE 20 x 10 x 100 ms | skipped if the early one answered, else 20 x 10 x 100 ms with the transport stopped; descriptors read torn-read-safe; **ring connect + dated finalize on every connect, each consumed within 50 ms** |
| Transport counts | legacy 34 / 32 | the IO descriptor counts (46 / 34 expected), refused if unusable |
| AX_P2P_ROUTE at start | written | **not written** |
| After the clock write | rate ack | wait for 0xC07C = 48 kHz locked (500 ms max, logged), SAMPLE_POS must move in 20 ms, position re-seeded, **live clock mask logged**, then the rate ack |
| Batch 1 skips | 2, 3, 12-14, 16-31 | 2, 3, 11, 13-17, 23 (27 settings written; S2 never) |
| Endpoints / ASIO / strips | yes | no / no / no |
| IOCTLs when active | all | Mode 1: reads (READ_REG, GET_POSITION, GET_READBACK, GET_LEVELS, GET_STRIPS) + READ_CAPTURE + WRITE_PLAYBACK; no settings, raw writes, transport start/stop or ASIO (STATUS_INVALID_DEVICE_STATE) |
| A failure | Stage Failed, service thread still starts (TRANSPORT_START retries) | `ApolloAudioExtAbort`: engine + transport stopped, EOB vector off, rings waited on and resynced (date-only disconnect only if the ring connect went through), not connected, no mixer flushes, **no service thread**; the log and GET_INFO name the step |
| Observe | quiesce only | quiesce, then **UA's leftover ring DMA enables switched off** (below) |

**How the x8p stays unchanged.** `ApolloBringup()` dispatches to `ApolloBringupAudioExt()` only when the
plan says AudioExtension; the x8p body below the dispatch is the pre-G3 text. In the shared functions
every Twin X branch is `if (ext)` / a plan field whose x8p value reproduces the old behaviour
(`ApolloAcefaceRun(..., "")` logs the old lines; the P2P write is kept for the x8p; the batch comes
from the x8p's skip mask, proven equal to the old list). The x8p records no G3 steps (GET_INFO v7 fields
stay 0). Visible differences on the x8p: GET_INFO Version 7, the driver banner, one more value in the
`unit values:` line (`WarmBootTransport=0/D`, ignored).

**Cold start, step by step (Mode 1, after a power cycle).** Log lines are `[twx]`; GET_INFO
`BringupSteps` bit n = step n passed (`apollo-diag --unit twinx info` lists them).

| Step | Log line on success | Failure signature (and what it means) |
|---|---|---|
| D | `cold start (Apollo Twin X (Gen 1), AudioExtension path, ...): boot transport yes` | `warm start ...` = the unit was not powered off |
| 1 E1 | `E1: DMA_CTRL 0x00000001 after the 8-strobe pulse (no strobe left set)` | `A STROBE IS STILL SET`: step 1 missing (the DMA engines may stay in reset); FPGA_REV changed: `FAILED at step 1`, nothing written |
| 2 E3 | (silent; up to 30 s for DSP 0) | `DSP n never reported ready (continuing)`: step 2 missing, not fatal (as upstream) |
| boot words | `strips: boot words xxxxxxxx xxxxxxxx 0... (DSP 1 untouched since power-up)` | `already used by a host` = not a cold start |
| 3 E4 | `DMA_CTRL after per-DSP enable: 0x00000007 (enables 0x06)` | other value: step 3 missing |
| 4 E6 | `SG tables programmed (play 0x... rec 0x...), DMA_CTRL 0x00000007` | |
| 5 E7 | `E7: boot transport running: play 3 rec 2, 8192 frames, FRAME_CFG 0x10, AX_CONTROL 0x20f, FRAME_CTR 0x.. after n poll(s)` | `E7: FRAME_CTR still 0 ... ignored; stopped, going on with the early ACEFACE`: the boot start was ignored (not fatal) |
| 6 E8 | `E8 early ACEFACE answered (retry 0, poll n, status 0x...)` | `E8 early ACEFACE never answered` + `E8: no early ACEFACE answer ... (E10 retries once)` |
| 7 pulse | `E7: DMA reset pulse (DMA_CTRL 0x0001fe07, back to 0x00000007), 50 ms, ring pages re-programmed 0x00000000` | a strobe left set or a page read back wrong (`ring 0x.... page n readback ...`) |
| 8 E10 | only if E8 failed: `E10 retry: ACEFACE answered ...` | `E10 retry: ACEFACE never answered`: G5 runs the full 20 s handshake |
| 9 E12 | `E12: mixer readback alive (n polls)` | `silent`: log only (as upstream) |
| 10 E13 | `E13: boot transport stopped (AX_CONTROL 0x000); ACEFACE done (kept for the connect)` | |
| 11 G5 | `G5: ACEFACE already answered by the early handshake: post-handshake work only` | (warm or no early answer) `G5 (transport stopped): ACEFACE never answered`, then `G3 bring-up FAILED at step 11 (G5 ACEFACE), 0xc00000b5 ...`: Stage 0x100, FailedStage 4. Power-cycle the Twin X (warm: or `WarmBootTransport=1`) |
| 12 G5 | `G5: rec  0 0001 MIC/LINE/HIZ 1 \| ...` (9 lines), `G5: play  0 0901 MON L \| ...` (12 lines), `G5: IO descriptors: record 34 slots ... play 46 rec 34`, `G5: MON L/R: playback slots 0/1, record (monitor loopback) slots 26/27 (...)` | `G5: IO descriptors not usable after 5 reads: ...` + `FAILED at step 12`, 0xc0000182, FailedStage 5: no layout, so no transport |
| 13 G5 | `G5: DSP 0 took the ring connect in n ms`, `DSP 1 ...`, `G5: DSP 0 took the dated finalize (date 0x...) in n ms` | `G5: DSP n did NOT take the ring connect within 50 ms (POSITION a, WRITE_PTR b)` + `FAILED at step 13`, FailedStage 3: the ring engine does not fetch; nothing more is sent |
| 14 rate ack | `rate info 0x00000101 (index 1): rate ack 44100 Hz 0x00000000, clock sources 0x1421007e` (the rate the firmware has at connect) | a nonzero status: step 14 missing |
| 15 batch | `mixer init batch: Twin X-safe, preamp/HP/monitor-reference/ALT fields skipped (MixerInitBatch=1): 27 settings ...` | |
| 16 start | `transport started: AX_CONTROL 0x00f SAMPLE_POS n FRAME_CTR 8 ...` | `transport start: no usable IO descriptor counts`: FAILED at step 16 |
| 17 48 kHz | `rate: 0xC07C 0x00000201 after n ms: 48000 Hz locked (clock write 0x20c + doorbell 4)` | `NOT 48000 Hz locked ...: the transport runs on, check the rate` (not fatal, step 17 missing) |
| 18 position | `transport: SAMPLE_POS moved ~960 frames in 20 ms (about 48000 Hz)` | `SAMPLE_POS did not move in 20 ms ...: the start was ignored` + `FAILED at step 18`, transport stopped |
| live clock | `clock sources (LIVE, our transport running): 0x....: sources 0x.... (S/PDIF yes, ADAT yes, Word Clock no, Peer 0x0A OFFERED, Internal 0x0C yes), rates 0x7e` | `Peer 0x0A no`: G6 starts from the ASRC (6.3.3) |
| 19 engine | `engine started at frame n (lead 144, guard 16)` | `engine did not start`: step 19 missing (service pass keeps the ring) |
| done | `G3 bring-up done (cold start): steps 0xffefe, 0 not passed; play 46 rec 34, rate info 0x00000201, clock sources 0x...., DMA_CTRL 0x00000007` | |

Success = Stage 5 Connected, Flags ACEFACE_DONE DSPS_CONNECTED CONNECTED TRANSPORT SERVICE_RUNNING (and
READBACK_VALID once the service drained one), `BringupSteps` 0xFFEFE (step 8 only when E8 failed),
`apollo-diag --unit twinx engine 5` about 1000 ticks/s and 48000 frames/s. Every `FAILED` line ends the
same way: transport stopped, rings resynced, unit idle; the speakers play nothing; a Twin X power cycle
retries (a refused or failed Twin X never touches the x8p).

**Warm vs cold.** Cold = the Twin X was powered off (a cold start needs ITS OWN power cycle: an x8p power
cycle drops the Twin X off the bus but it stays powered, so it comes back warm). Warm = the driver
restarted with the unit powered (driver update, `pnputil /restart-device`, `step-g3.ps1 -WarmTest`): the
log says `warm start ... boot transport no (warm start, WarmBootTransport=0 ...)`, then
`F9: transport stopped (AX_CONTROL 0x000)`, and G5 runs the full ACEFACE with the transport stopped
(`G5 (transport stopped): ACEFACE answered ...`): the open question of section 2's notes. The ring
connect runs on the warm path too. If the warm ACEFACE times out: `FAILED at step 11` with the hint;
set `Units\<key>\WarmBootTransport=1` (DWORD) and restart, or power-cycle.

**Mode 1 on the Twin X runs** the bring-up, the transport (StartTransportAtConnect, default 1), the 1 ms
copy engine and the 10 ms service thread (readback, rate info, the batch flush). It does **not** run
ACX circuits (none are created for SUBSYS 0x0019, and the plan forbids them), strips, ASIO, or any
setting: `apollo-diag --unit twinx info|log|engine|pos|levels|readback|capture|tone` only. `tone` on the
Twin X takes MON L/R from the unit's live playback descriptor (or `--slots`), at -20 dBFS by default and
at most 10 s on a Mode 1 unit.

**Observe change (G2 review).** UA's driver left the Twin X's per-DSP ring DMA engines enabled
(DMA_CTRL enables [8:1]) with ring pages in memory nobody owns any more. The quiesce already pulls every
ring's target back to its position, so nothing is fetched; observe now also writes DMA_CTRL with the
enables and strobes cleared (global bit kept, no strobe pulse, nothing else written) and logs
`observe: DMA_CTRL 0x.... -> 0x....`. Reason: hours in observe with live engines pointing at freed
memory is the window where a stray access becomes an IOMMU / DMA-verifier fault (0xE6). Our own
bring-up re-enables per DSP with its strobe pulse (E1 + E4 cold, F8 warm), as always. Twin X plan only.

**Other G2 review follow-ups.** GET_LOG copies the 256 KiB ring with two `RtlCopyMemory` (and each log
line with at most two) instead of a byte loop under the spin lock; `ApolloReadOwnerState` stops at the
first all-ones read (with FPGA_REV all ones too) instead of 150 reads on a dead link; READ_REG already
reaches observe units (the read class; `ApolloObserveAllows` now derives from the same class table).

**GET_INFO version 7** (same size; the former `Reserved6`): `ClockSrcsLive`, `BringupSteps`,
`BringupFailStep`, `Reserved7`. apollo-diag accepts versions 5, 6 and 7, prints the step list and names
FW_UNTESTED.

**Scripts.** `tools/phase1/step-g3.ps1`: pass 1 (both units onto the package, Twin X kept in observe,
then `Units\<key>\Mode=1`, then power BOTH units off and on), pass 2 (collects and prints PASS/CHECK; no
tone), `-WarmTest`, `-Rollback`. `install.ps1 -Unit twinx` now warns (and asks) when a unit key other than
the x8p's asks for Mode 1 or 2, because since G3 that unit is brought up instead of observed.

**The Twin X descriptors as UA left them (G2, STALE).** Record (34): 0 `0001` MIC/LINE/HIZ 1, 1 `0002`
MIC/LINE/HIZ 2, 2-3 `0103`-`0104` MIC/LINE 3-4, 4-7 `0305`-`0308` LINE 5-8, 8-15 `0401`-`0408` ADAT 1-8,
16-17 `0501`/`0502` S/PDIF L/R, 18-25 `0701`-`0708` VIRTUAL 1-8, **26-27 `0901`/`0902` MON L/R**, 28-31
`0201`-`0204` AUX1 L/R, AUX2 L/R, 32-33 `0B01`/`0B02` TALKBACK 1-2. Playback (46): **0-1 `0901`/`0902` MON
L/R**, 2-9 `0301`-`0308` LINE 1-8, 10-17 ADAT 1-8, 18-19 S/PDIF L/R, 20-27 VIRTUAL 1-8, 28-35 `0A01`-`0A08`
CUE1-CUE4 L/R, 36-45 `0C01`-`0C0A` RESERVED 1-10. The same skeleton as the x8p's (34 record; play
MON, LINE, ADAT, S/PDIF, VIRTUAL, CUE in the x8p's order) with more slots than the Twin X has jacks
(LINE 5-8, ADAT and S/PDIF outputs, MIC/LINE 3-4): a firmware-wide frame, not the 3.2 guess (24 / 10).
The live read at G3 decides; `bringup-plan-test` decodes these words.

#### 6.9.4 As built for G4 (builds\work-g4, Nov 14)

G3 passed on the hardware (Nov 14: cold and warm bring-up, 47,999.8 frames/s, 0 late, a tone read back at
-20.0 dBFS on record 26/27 and heard on the speakers). G4 lets the Twin X run **Mode 2, standalone**: its
own Windows endpoints and the monitor settings that apply to it. Combined stays off (6.7: "with Combined
off, each unit creates its standalone per-model table").

| Decision (`driver/bringup_plan.h`) | x8p (unchanged) | Twin X Gen 1 |
|---|---|---|
| Highest Mode | 2 | **2** (was 1; a Mode 2 key now runs as 2) |
| Windows endpoints | its 10-row table, `EnabledEndpoints` default 0x3FF | **its own 2-row table** (`endpoints.c g_ApolloTwinXEndpoints`), default 0x3, Mode 2 only |
| ASIO / strips | yes / yes | no (x8p-only until Combined) / no (G5) |
| IOCTL classes, Mode 2 | all | reads, capture/playback, **settings** |
| `SET_MONITOR_PARAM` | every code goes on to `mixer.c` (as before) | level (S2 [7:0]), HP1 level (S2 [15:8]), mute, mono, dim; **anything else STATUS_NOT_SUPPORTED** (HP2 source: one headphone; source, HP1 source, dim level, talkback: not verified on the Twin X yet) |
| `SET_INPUT_PARAM`, `SET_MIXER_SETTING` | yes | STATUS_NOT_SUPPORTED (preamps: G5; raw settings never) |
| Rate confirmation after the clock write | none | up to **2 s**, notification status acknowledged while polling, then the **48 kHz rate ack** (below) |
| Steps per path (GET_INFO `BringupApplies`) | none recorded | cold 0xFFEFE (0xFFFFE when E8 fails), warm 0xFF818, warm + `WarmBootTransport` 0xFFEF8 |
| Capture latency reported to Windows | 10 frames | 5 frames (`oa:driver/ua_models_gen.h` Twin X `adc_mframes`) |

**Endpoints.** Device add picks the table by the PCI hardware ID (`apollo_unit.h ApolloAddTimeModel`):
SUBSYS 0x0014 or unread = the x8p's table exactly as before; **0x0019 = the Twin X's two circuit
objects** (the x8p's legacy `EnabledEndpoints` never reaches it); anything else none. The objects stay
unregistered until PrepareHardware adds the ones the unit's own `EnabledEndpoints` allows, which is
none below Mode 2 (Mode 0/1 force it to 0, as G3) and none unless identification says Twin X (a table
chosen for another model adds nothing). Rows:

| Row | Reference string | Pin name | Type | Bridge | Channel IDs | Fallback slots | INF |
|---|---|---|---|---|---|---|---|
| 0 | `ApolloTwxMon` | MON L/R | render | KSNODETYPE_LINE_CONNECTOR | 0x0901/0x0902 | play 0/1 | `Apollo_TwinX_Mon_Interface` |
| 1 | `ApolloTwxMonIn` | MON L/R | capture (monitor-mix loopback) | KSNODETYPE_LINE_CONNECTOR | 0x0901/0x0902 | record 26/27 | `Apollo_TwinX_MonIn_Interface` + NeverSetAsDefault |

The slots come from the live IO descriptor by channel ID (`ApolloEndpointSlot`, as on the x8p); the
fallbacks are the G2/G3 descriptor's (`bringup-plan-test` checks they agree). INF: `Apollo_TwinX_Install`
has an `.Interfaces` section (AUDIO + RENDER/CAPTURE + REALTIME per row) with FriendlyName "Apollo Twin X";
four-part hardware IDs unchanged; infverif clean. Windows composes the endpoint name from the pin name
and the device's own description, so expect "MON L/R (Apollo Twin X (open driver, test build))" as the
x8p shows "MON L/R (Apollo x8p (open driver, test build))"; scripts match `MON L/R (Apollo Twin X`
without the closing parenthesis.

**Stream code, per unit.** The render/capture copies, the zero-ahead and the engine already use the unit's
own `PlayChannels`/`RecChannels`/`RingFrames` (46/34/8192 on the Twin X, 34/32/8192 on the x8p; the same
8192-frame ring, `ApolloPlanBufFrames`). Two Twin X-only gaps closed: the capture latency is per table, and a
stream prepared before the Twin X's descriptor was read (no counts until G5) is marked provisional and
resolves its slots again at Run (the x8p has its counts from PrepareHardware, so it never takes that path).

**Rate (G3 follow-up a).** Evidence: the firmware posts nothing new, the new rate included, while earlier
notification bits stay set; the kext and open-apollo write 0 to 0xC030 after acting on them
(`oa:driver/ua_audio.c:2150-2154`), the new rate is published only after that write
(`oa:driver/ua_audio.c:5357-5361`), and a wait under the device lock must do the acknowledging itself
because "the service loop cannot clear the notification bits meanwhile" (`oa:driver/ua_audio.c:5255-5256`,
`5304-5307`). Our confirmation ran under DevLock before the service thread existed (G3 cold: 0x101 after
529 ms; 0x201 once the service thread ran, NotifStatusSeen 0x23 = descriptor bits 0/1 + clock event bit 5).
The rate-ack order: the connect acks the firmware's rate (`oa:driver/ua_audio.c:3214-3222`, ours unchanged:
44.1 kHz after a power-up), the post-start clock write is followed by the ack at the new rate
(`oa:driver/ua_audio.c:5182-5199`), and the kext's own clock change acks only after the firmware reports the
new rate (`oa:driver/ua_audio.c:5387-5453`). As built: after the clock write, up to 2 s of 10 ms polls, each
first acknowledging 0xC030 (logged as "notifications acknowledged meanwhile") and then reading 0xC07C; then
`rate ack 48000 Hz to the DSPs after the clock change (they had 44100 Hz from the connect)`; step 17 ("48 kHz
locked + acked") passes when 0xC07C is 0x201 and every DSP has the 48 kHz ack. If the acknowledged bits include
the descriptor bits 0/1, both descriptors are read again and compared slot by slot with the ones the
transport runs on (44.1 and 48 kHz are one band; a change is logged and step 17 stays open). A warm start
(0x201 at connect) passes at once, with no extra ack.

**Steps (G3 follow-up b).** GET_INFO `BringupApplies` (the former `Reserved7`, same layout and size,
`APOLLO_IOCTL_VERSION` still 7; a G3 driver returns 0 there) says which steps the start's path runs; apollo-diag
prints the others as `n/a`, and the driver's done line counts only those (`steps 0x... of 0x..., n not
passed`). The G3 hardware runs give 0 not passed warm and only step 17 cold (`bringup-plan-test`).

**What the x8p sees.** Its plan row gains only neutral values (no rate wait, no step record, every setting,
10 rows, 0x3FF, 10 frames), proven value for value by `bringup-plan-test`; its endpoint table, circuits,
INF section and IOCTL behaviour are byte-for-byte the pre-G4 code paths. Visible: the driver banner and the
device-add line wording.

**Script `tools/phase1/step-g4.ps1`.** Pass 1: quit blockers, wait until the x8p is idle, save the defaults to
`step-g4.defaults.json`, put the package on both units (same guards and devcon/install.ps1 branches as G3),
write `Units\<Twin X key>\Mode=2` and `EnabledEndpoints=3`, restore the defaults, then: power the Twin X off,
then the x8p; x8p on, then the Twin X (the x8p's strips need its cold start; the new image loads only when both
let go). `-Restart` does a warm restart of both drivers instead (disable Twin X, x8p; enable x8p, Twin X) and
runs pass 2 at once; the x8p's strips then stay off until its next power cycle. Pass 2: Twin X Mode 2 Connected,
flags, rate 0x201, 46/34, steps (ok / n/a / not passed), 48 kHz and 1000 ticks/s, its two circuits added and
its two Windows endpoints named `MON L/R (Apollo Twin X ...)`; the x8p Connected Mode 2, DMA_CTRL 0x7F,
34/32, no step record, 48 kHz, 10 endpoints all added (10 Windows endpoints), strips Live; restores the saved
defaults; prints PASS/CHECK and then prints, never runs, the loop command
`wasapi-loop.exe 1800 --render "MON L/R (Apollo Twin X" --capture "MON L/R (Apollo Twin X"` with the warning
that it plays a tone into the speakers. `-Rollback`: `Mode=1` (endpoints go away at the Twin X's next start).

#### 6.9.5 As built for G5 (code only; builds\work-g5, Nov 14; not installed, not run)

Written while another agent held the hardware: nothing was installed, restarted or written; the only hardware
facts used are the ones logged at G3 (`tools/phase1/step-g3.log`). Merge base `builds/base-pre-g56`; G6 (the
combined device) was built at the same time in `builds/work-g6`. Rule (Aaron, Nov 14): **nothing connects to
Universal Audio**: no network calls, no UA program, service or SDK DLL is launched or loaded. G5 keeps it: the
strip modules are the five files apollo.sys already reads for the x8p (`System32\drivers\ApolloStrips`, written
by `apollo-diag strips install`, which reads the local UAD files as plain bytes, offline); step-g5.ps1 runs
only apollo-diag and never points at `revert-unit.ps1` (that binds UA's driver).

| Decision | x8p (unchanged) | Twin X Gen 1 |
|---|---|---|
| Strips (`bringup_plan.h` `Strips`) | yes | **yes** (Mode 2; `StripsEnable` decides, default **0**) |
| Strip plan (`strip_plan.h StripModelFor`) | 8 inputs, mask & 0xFF, DSP 1 (0-3), room for 8 | **2 inputs, mask & 0x03, DSP 1 (0 or 1), room for 2** |
| Unit defaults (`apollo_unit.h`) | StripsEnable 0, StripSteps 1, StripMask 0x01, StripDsp 1, batch 1 | StripsEnable **0**, StripSteps **0** (all), StripMask **0x03**, StripDsp 1, batch 1 |
| `SET_INPUT_PARAM` (`preamp_plan.h`) | preamps 0-7, as before | **preamps 0-1**; Hi-Z impedance on 0 |
| `SET_MONITOR_PARAM` | any | level, HP1 level, mute, mono, dim, **talkback** |
| GET_INFO `NumPreamps` | 8 | **2** |
| Endpoint table | 10 rows, 0x3FF | **4 rows**: + `MIC/LINE 1`, `MIC/LINE 2`; default stays **0x3** (rows 2-3 opt-in) |

**The strip plan, value by value** (`StripModelFor(STRIP_MODEL_TWINX)`; strip-plan-test checks each):

| Value | Twin X | Source |
|---|---|---|
| First synch / first virtual | 0 / 0x18 (ANALOG 1/2 = DSP inputs 0/1) | `oa:devices/apollo-twin-x.json:115-137`; Gen 2 entry `oa:driver/ua_strips.c:785-789` |
| Inputs, mask | 2, 0x03 (Hi-Z on input 1 is the same strip) | 5.2; `oa:driver/ua_models_gen.h` Twin X `.preamps = 2, .hiz = 1` |
| Strip DSP | 1 (StripDsp 0 allowed: record only, no monitor mix; anything else -> 1) | `oa:driver/ua_strips.c:129-132`, `oa:HARDWARE-SESSIONS.md:327-328` |
| Ownership | DSP 1 boot word & ~0xFF00 == 3, read before E4/E7 (G3 cold: `00002703 00000003`; after UA: `0008270b 0008000b`) | `oa:driver/ua_plugin_place.c:110-118`; step-g3.log |
| Pool check | code 0xE0000 >= 994 words; tops 0x9D1B8 / 0xC6FFE / 0x08FFE000; room for 2 strips (0x54 / 0x10 / 0x2010 each) above scratch | `oa:driver/ua_strips.c:440-483`; G3 DSP 1: size `4000 a9b9 1fff ff51ae` base `e0000 92800 c5000 8008e52` scratch `80 80 8000` = code 0 for 2 and for 8 |
| Modules | 0xEB, 0xDA, 0xA5, 0xC2, 0x12B at 0xE0000-0xE03A2 (the pins); no 0xC9 talkback, no 0x83E input strip | `oa:driver/ua_strips.c:27-31`; `include/apollo_strip_pins.h` |
| Words | the x8p's for inputs 1-2: headers 0x9D1B0 / 0x9D15C, SYNCH `{001f0004, 0x00200000 \| i, header, 0}`, span x2, route 0x1FFFF | STRIPS.md 2.6-2.7 (byte-identical planner) |
| Routes | route 0x1FFFF to every DSP but the strip DSP: **DSP 0** on the Duo (0, 2, 3 on a Quad) | `oa:driver/ua_strips.c:1740-1753` |
| Record slots | 0x0001 -> slot 0, 0x0002 -> slot 1 (G3 descriptor) | 6.9.3 |
| 48V resync | S0 bits 3 / 9, only where readback says 48V on, 0 then 1 | 4.1; `oa:driver/ua_core.c:2458-2499` |
| Batch | MixerInitBatch 1 (Twin X-safe, 4.3); 2 = the A/B | 4.3 |

**The x8p stays byte-identical.** The planner words are unchanged (strip-plan-test hashes every word the planner
can produce for every input, mask 0-0x1FF and stage, and matches the digests of `base-pre-g56`'s `strip_plan.h`);
the x8p row reproduces the pre-G5 `strips.c` (mask, DSP clamp, route loop; the layout check moved to
`strip_plan.h StripPlanLayoutCheck` and is compared with the old code on 3 geometries x 11 registers x 8 changes);
preamp-plan-test runs every `SET_INPUT_PARAM` input (preamps 0-11, codes 0-0x1FF and extremes, 27 values) through
the new map and a copy of the pre-G5 `mixer.c` and requires the same result and the same writes in the same order.

**Dry run (`StripsEnable=2`).** Every gate as on the x8p (modules verified, DSP owned, pools, 48 kHz), then the
whole pass logged and nothing sent: `strips: dry run plan (Apollo Twin X (Gen 1)): StripMask 0x03 ..., DSP 1 boot
word ..., pools checked for 2 strip(s); 2 PREPARE + 1 START ...`, the module destinations, per input `input n = DSP
input n-1 -> record slot n-1 (000n MIC/LINE/HIZ n); header ..., PREPARE 13 entries, 83 payload dwords` and `input n
START {001f0004 0020000x ...} + span, span, route 0x1ffff; route 0x1ffff to DSP mask 0x01`, then the 48V state.
GET_STRIPS: Idle, "dry run complete: every gate passed, nothing sent".

**Preamps (`driver/preamp_plan.h`, `mixer.c ApolloSetInputParam`).** Mic/Line, PAD, 48V, LowCut, Phase: S0 bit
6*ch + code (bits 0,1,3,4,5 / 6,7,9,10,11), 0/1; Link: preamp 0 only, S0 mask 0x104; gain 10-65 dB: GainA S17 /
GainB S16 lane ch (dB-10, at most 54), GainC S3 bits 6*ch (dB-9); Hi-Z impedance (`APOLLO_PARAM_HIZ_IMP` 0x11):
preamp 0 only, S13 [3:0], codes 0-7 = 2.2M 1M 453k 232k 100k 70k 47k 21k (no readback field: the host keeps it).
Refused: preamps 2-7 and Link on 1 (`STATUS_INVALID_PARAMETER`, the x8p's rule), Hi-Z on 1, mic impedance 0x07,
custom preamp 0x08 and every other code (`STATUS_NOT_SUPPORTED`). Nothing ever writes S11, S12, S14, S24, S25 or
S13 outside [3:0] (preamp-plan-test). Readback: flags word 0, GainC word 3 (dB = GainC + 9), Hi-Z cable word 0 bit
24 (input 1 only), talkback word 6 bit 8, clock word 6 bit 31 when bit 30. `apollo-diag --unit twinx readback`
decodes the Twin X that way; `apollo-diag hiz 1 <0..7|2.2M..21k>`; `info` prints `NumPreamps`.

**Endpoints.** `g_ApolloTwinXEndpoints` rows 2-3: `ApolloTwxIn1` / `ApolloTwxIn2`, pin names `MIC/LINE 1` / `MIC/LINE
2`, mono capture, KSNODETYPE_MICROPHONE (like the x8p's rows 2-9), IDs 0x0001 / 0x0002, fallback slots 0 / 1 (=
the G3 descriptor, bringup-plan-test). INF: `Apollo_TwinX_Mic_Interface` (AUDIO + CAPTURE + REALTIME, no
NeverSetAsDefault). Device add creates all four circuit objects for SUBSYS 0x0019 (`APOLLO_TWX_EP_ALL` 0xF); the
unit's `EnabledEndpoints` adds them (default 0x3: MON only, so a G4 install shows no new endpoint). Windows names:
`MIC/LINE 1 (Apollo Twin X (open driver, test build))`. ASIO stays the x8p's here; the Twin X's inputs reach ASIO
through the combined device (G6).

**Talkback.** `SET_MONITOR_PARAM` talkback (S13 bit 8) is allowed on the Twin X now; there is still no Talkback
Input Strip (0xC9, no captured recipe), so record slots 32-33 stay silent.

**Tests** (`tools/build-all.cmd`): strip-plan-test (902 checks: the digests, the x8p row against the old code, the
Twin X row, its passes through the stream checks), new **preamp-plan-test** (64), bringup-plan-test and ident-test
updated to the G5 Twin X row (strips, preamps, talkback, 4 endpoint rows, defaults 0 / 0 / 0x3).

**Script `tools/phase1/step-g5.ps1`** (admin; needs the G5 apollo.sys on both units, Twin X Mode 2, `NumPreamps 2`;
installs nothing). Run 1: saves the Windows defaults to `step-g5.defaults.json`, writes `Units\<Twin X key>`
StripsEnable 2, StripSteps 0, StripMask 3, StripDsp 1, MixerInitBatch 1, EnabledEndpoints 0xF, then: speakers off,
headphones low, power-cycle the **Twin X only**. Run 2: waits for the bring-up and the strip outcome, restores the
defaults, checks the dry run (cold, 0x201, OWNED, MODULES_OK, layout code 0, "dry run complete", the plan lines for
inputs 1-2 with record slots 0/1 and routes to DSP mask 0x01), the four circuits and the two Windows MIC/LINE
endpoints, the x8p untouched; on PASS writes StripsEnable 1 and asks for another Twin X power cycle. Run 3: strips
Live, stages done 9, 5 replies ok, inputs 1-2 taken, 2 routes, 48V resync state; a silent preamp round trip (low
cut on input 2 on/off seen in readback; Hi-Z 2.2M; preamp 3 refused); then, one question at a time: mic into input 1
(48V? never for a ribbon), 5 s of levels on slot 0; input 2 on slot 1; an instrument in the Hi-Z jack (readback
bit 24, slot 0). Exact zero with something plugged in and sound going in = CHECK with the batch A/B (`-Batch 2`).
`-Rollback`: StripsEnable 0, EnabledEndpoints 3 (Mode 2 stays). It plays nothing.

**Apollo Control. Update (Nov 15, merged):** done in `builds/work-app`: a unit selector in the Control view, per-unit
state, Twin X strips (2, link 1-2, Hi-Z load `hizimp` on input 1) and monitor (level, mute, dim, mono, talkback,
HP1), sends tagged with the unit, inline refusals; 222 app tests; contract in `docs/apollo-control-units.md` 1.2a.
The original note follows. Not changed. Its Status view already decodes the Twin X's two inputs read-only (MODEL_CAPS
0x99: preamps 2, hiz 1; `NumPreamps` 2 now), but every control in it drives unit slot 0, the x8p (`S.inputs`,
`S.links`, `S.mon`, one readback, `hwSend` without a unit). Twin X controls need: a unit selector for the Control
view; per-unit input/link/monitor state and pending maps; `hwSend` with the unit field the host already accepts
(`input\t<ch>\t<param>\t<value>\t<unit>`); the strip count, link pairs (pair 0 only) and Hi-Z from MODEL_CAPS; a
Hi-Z impedance control (host `InputParam` name for 0x11); `onReadback` per selected unit; tests in `app/tests`.

**What only the hardware answers (G5 morning):** the Gen 1 DSP 1 takes the five uploads and starts the strips (the
recipe is verified on Gen 2 and the x8p, never on Gen 1); inputs 1-2 then carry signal with batch 1 (else the batch
2 A/B); the 48V edge brings phantom up on Gen 1; Hi-Z on input 1 lands on slot 0 through the same strip; the Twin X
monitor mix feeds the speakers at DSP 1's default level once a strip runs (hence speakers off); talkback toggles.

#### 6.9.6 As built for G6 (builds\work-g6, Nov 15; installed and tested on both units; written as 6.9.5 in the G6 copy)

**Measured first (6.3.2).** `tools\agg-test\agg-drift 60` (GET_ENGINE_STATS snapshots of both units, 22,627
ticks each): x8p 47,999.949 frames/s, Twin X 47,999.722 against QPC; **the Twin X runs 4.74 ppm slower than
the x8p** (13.7 frames apart after a minute, about 820 an hour). Read jitter of a snapshot against its QPC
stamp: 0.3 frame rms, rare single stamps up to 8 frames late. SAMPLE_POS steps by single frames.

**Thunderbolt peer clock (6.3.7), result.** Read-only survey on both units: P2P slot registers
0x2280-0x229C, P2P enable 0x22C4, peer latency 0x22C8-0x22D8 and 0xC5B8-0xC5C0 all 0; 0x224C x8p 0x121,
Twin X 0x102. Topology: the Twin X sits behind the x8p's Alpine Ridge switch (downstream port 4); the switch
ports report no ACS capability; DMA remapping is on for both units (our INF) and Verifier DMA checks are on.
Experiment (`agg-test probe`, AGG_CLOCK_PROBE, every write logged; no P2P register written): source 0x0A on the
x8p, then on the Twin X, each with AX_CONTROL 0xF and 0x20F. Both firmwares **accept** source 0x0A (clock
event acked in 10-42 ms, 0xC074 reads back 0x20A, 0xC084 48000) but the lock bit then drops (0xC07C 0x201 ->
0x200) and the unit **free-runs on its own crystal**: drift with the x8p on peer -4.777 ppm, with the Twin X on
peer -4.744 ppm, baseline -4.787 ppm. Restored to 0x0C, locked, no glitch the aggregate noticed (0 resyncs).
So the clock only follows once the peer-to-peer path is programmed: slot registers 0x2280/0x2284 + 8*slot =
the peer's BAR0 bus address, the enable mask, and the kext's 3.5 s PLL phase state machine (open-apollo plan
items K2-K5, all "blocked" upstream; their text is not in the public tree). Not attempted: a wrong address
goes upstream to the IOMMU, which with Verifier's DMA checks is a 0xE6 bugcheck. What it would take: the K3/K4
sequence from the kext, then a first run with Verifier DMA checks off. Until then the ASRC below is the clock.

**What runs (driver/agg.c, driver/apollo_agg.h, include/apollo_agg.h).**
- One driver-global aggregate. Each unit keeps its own engine, endpoints and per-unit ASIO client; units in
  Mode 2 with a running transport **join** at the end of their bring-up work item and **leave** in D0 exit
  (after the work-item flush, before anything stops; a rundown reference keeps the tick off a leaving unit).
- **Timeline** = the configured master if it runs, else the first running unit in block order (acting
  master); a unit whose tick finds the timeline stopped takes over at once (gate closed, engine lost).
- The aggregate work runs at the end of the **timeline unit's own tick** (`ApolloAggAfterTick`), after it
  released its EngineLock. It uses the members' position snapshots only: no MMIO, the engines stay the only
  SAMPLE_POS readers. Followers are projected to the timeline read's QPC instant.
- **Lock order:** Agg.Mutex -> (StripLock -> DevLock) -> Agg.Lock -> several units' EngineLocks only in
  ascending HW_UID order. The tick never holds its own EngineLock while it takes Agg.Lock.
- **Clock loop** (`ApolloAggClockUpdate`): map phi(x) = F0 + R(x - M0), re-anchored every tick (continuous),
  PI loop on e = Pf(tm) - phi(Pm), damping 0.707, 1 Hz for the first 2 s then 0.05 Hz; the loop input is
  averaged over 16 ticks while acquiring and 512 while tracking (else read noise puts about 1 ppm of jitter
  on R); raw errors beyond 2 frames clamped; 3 in a row beyond 12 frames = resync; lock = 64-tick average
  under 0.5 frame for 250 ms; R outside +/-500 ppm or 3 resyncs in 10 s = degraded (muted, re-formed 10 s
  later). Followers are muted until locked, then a 10 ms fade-in.
- **ASRC** (`ApolloAsrcBuild/Weights`): windowed sinc, 1024 phases (+1), linear row interpolation, rows
  normalised to unity DC; float MACs (SSE2), coefficients once per output frame for all of a unit's channels.
  `asrc-test --sweep` chose per size (24-bit sine through a delay drifting at 4.74 ppm, error = everything but
  the exact sine):

| h | Kaiser beta / cutoff | flat to 18 kHz | at 20 kHz | error 1 kHz | 10 kHz | 18 kHz | 20 kHz |
|---|---|---|---|---|---|---|---|
| 8 (low CPU) | 8 / 1.00 | 0.13 dB | 1.2 dB | -89 dB | -85 | -38 | -19 |
| **16 (default)** | 10 / 1.00 | 0.0002 dB | 0.026 dB | **-106 dB** | -104 | -111 | -52 |
| 32 (high) | 10 / 0.97 | 0.0001 dB | 0.0001 dB | -108 dB | -111 | -103 | -105 |

  So the 6.3.3 target (flat to 20 kHz within 0.01 dB, -100 dB) holds for h 32 everywhere and for h 16 up to
  18 kHz (0.026 dB at 20 kHz). 256 phases (the spec's figure) measured the same; 1024 are kept as margin.
- **Capture:** head H = Pm - guard - pad, pad = h + M + max ADC excess of another unit (37 at the defaults,
  Twin X master); a follower channel for timeline frame x is the ASRC value at phi(x) + (ADC_f - ADC_tl).
  **Render:** timeline channels ADD into the timeline ring; follower destinations go to a staging ring per
  (unit, slot) on the timeline (4096 frames, 32 rings), from which the ASRC writes the follower ring up to
  Pf + lead - h - M - 2. Output latency is the same for both units; input latency aligned (6.3.5):
  **ASIO at 128 frames: in 186, out 280** (x8p alone: 154 / 280).
- **Layout:** simple = every descriptor slot of each unit in block order, minus the Twin X slots with no jack
  (MIC/LINE 3-4, LINE 5-8, ADAT/S/PDIF outputs, RESERVED): **60 in, 56 out**; explicit = Apollo Control's
  advanced order (slot based, hidden left out). Names carry the unit ("x8p MIC/LINE 1", "TwinX MON L"); the
  x8p's first 8 inputs are named MIC/LINE 1-8 (its firmware frame calls them MIC/LINE/HIZ, MIC/LINE, LINE);
  a routed output says where it plays ("x8p MON L > TwinX"). Routes: each named source pair goes to every
  destination of its routes instead of its own slots; "windows" routes are stored for G7, not used.
  Absent units keep their channels (descriptor cached in the stored config), silent.
- **Config:** AGG_SET_CONFIG (user level) validates (`ApolloAggValidate`), fills the descriptor caches and
  stores the whole APOLLO_AGG_CONFIG as `Parameters\Aggregate\Config` (REG_BINARY), JSON text included.
  Nothing stored = the default: every unit seen since load, **x8p first**, Twin X master, ASRC 16, Studio
  (DAW outputs 1-2 to the Twin X MON). Stored at the end of this session: that default.
- **ASIO:** avium-asio.dll registers two drivers: "Apollo x8p (Avium)" (unchanged, x8p only) and **"Apollo
  (Avium)"** (CLSID {b6a1f3c2-5d4e-4a7b-9c21-3e8f0a6d7c55}): ASIO_OPEN version 3 with logical channel
  numbers on any unit's handle (the DLL prefers the x8p's), shared block version 3 (Reserved[0] layout
  version, [1] timeline epoch, [2] stale flag): a changed epoch or layout sends kAsioResetRequest, a changed
  latency kAsioLatenciesChanged. Clock sources = one per unit ("Twin X clock (master)"); choosing one sets
  the master through AGG_SET_CONFIG. Combined off or no unit running: init fails with that message.
- **Apollo Control:** agg-get / agg-set answer from the driver (`app/src/agg_json.h`: JSON <-> config,
  `tools/agg-test/aggjson-test`); a config without JSON (default, agg-test, the ASIO clock menu) is
  described back as JSON. needsAdmin false.
- **Routing without a reset:** a configuration that keeps the channel lists but changes the routes
  re-resolves the open ASIO client's destinations under Agg.Lock (`AggAsioResolveOutputs`) and remaps the
  staging rings: routing switches at the next tick, as the Units & routing page says. Changed lists or
  latency: LayoutVersion bump, the client goes silent (stale) and its host gets kAsioResetRequest.
  **Built and Code-Analysis clean but not on the hardware yet** (the x8p was left "reboot pending" by a vetoed
  disable at the last install; the installed build, `builds\2026-11-14-g6\package-installed`, re-resolves
  routes only when the client reopens).
- **Not built:** Align=0 (always aligned), RenderTrim/CaptureTrim, persisted LastRatioPpb (the loop locks in
  0.25-1 s without it), AGG_GET_STATE's 99.9th percentile, combined Windows endpoints (G7), an unsolicited
  `agg` message when another tool changes the config. **[G7] All built except the 99.9th percentile: 6.9.7.**

**Hardware results (Nov 15).** Aggregate idle: x8p follower LOCKED at +4.76 ppm, e within +/-0.09 frame.
`asio-test --agg 30 128 --amp -20` (routes none, tone into both units' MON, both MON loopbacks recorded):
11,250 switches, **0 underruns, 0 overruns, 0 resyncs**; both loopbacks -20.00 dBFS; residual 120.7 dB
(Twin X) and 115.4 dB (x8p, through the ASRC twice) below the tone; **phase between the units constant to
0.000 degree over 29 s** (4.74 ppm uncorrected = 49.5 degrees); the fixed +5.000-frame offset is the ADC
alignment, which a digital loopback does not have (a measured RenderTrim/CaptureTrim can take it out).
Twin X disabled during playback: no bugcheck, the x8p became acting master, the stream ran on (0 underruns),
the host got kAsioResetRequest; enabled again: it took the timeline back, the x8p re-locked. 64-frame buffers:
about 3 skipped switches a second in asio-test with **both** the combined and the unchanged x8p-only driver
(this PC/host; 128 is clean on both). Aggregate work 1-10 us a tick; the rare 100-400 us maxima appear in the
units' own ticks too (system stalls, VBS on). x8p-only "Apollo x8p (Avium)": MON loopback PASS (129.9 dB).

**G4 review fixes in the same build.** (1) A Twin X stream that ran before its layout was known keeps
placeholder slots (slot 0 for both channels while the counts are 0): `ApolloStreamsEngineStarted` now
re-resolves every provisional stream (the engine starts only once the counts exist); the Run log text says
"placeholder slots". Not provoked on hardware: on a warm start the descriptors are read about 140 ms after
the circuits appear and Windows only let a stream open about 1 s after the engine started (a stream started
then ran clean: MON loopback -12.0 dBFS on record 26/27, slots 0/1 and 26/27, driver counters 0); a cold
start (power cycle) is the case that hits it. (2) A failed Twin X bring-up now removes its circuits from the
work item (ACX dynamic removal, DevLock released) and a later successful bring-up of the same start adds them
again; not provoked either (no way to fail a warm bring-up on purpose).

**Tools.** `tools\agg-test`: `agg-drift` (drift), `agg-test state|watch|layout|config|set|probe`, offline
`asrc-test` (math, filter, loop incl. `--trace` of a real agg-drift CSV, layout) and `aggjson-test`, both in
build-all. `asio\bin\asio-test --agg <s> [frames]`.

#### 6.9.7 As built for G7 (builds\work-g7, Nov 15; code only: not installed, not run)

Written while the x8p waited for a reboot ("reboot required" since G6 install 3): nothing was installed,
restarted or written; only `agg-test state|config` and `apollo-diag` were run, read-only. Merge base
`builds\base-pre-g7` (G5 + G6 merged). Built clean (`tools\build-all.cmd`: /W4, Code Analysis with no defects,
infverif clean), every offline test passes, Apollo Control's 150 UI tests pass.

**The endpoint set (6.7).** Two admin values under `Parameters\Aggregate`, read at device add (a change needs both
units restarted: a power cycle, or `step-g7.ps1 -Restart`):

| Value | Meaning |
|---|---|
| `EndpointSet` (DWORD) | absent / 0 / anything unknown = **standalone**: each unit its own table, the pre-G7 code path in `circuit.c` unchanged; **1 = combined**: the x8p's hardware ID creates the combined table below, the Twin X's (and any other) creates **no circuits at all** (`apollo_agg.h ApolloAggEpTableFor`) |
| `EndpointMask` (DWORD) | rows of the combined table that exist; default `0x3FF` (rows 0-9); the Twin X MIC/LINE rows (10-11) opt in, like its standalone rows before G5 |

The host is the x8p by hardware ID (the upstream unit, 6.2); a `HostUnit` value is not built. The units' own
`EnabledEndpoints` stay untouched, so `EndpointSet 0` brings back exactly the endpoints they had. PrepareHardware adds
the combined circuits only in Mode 2 on an identified x8p, then tells the aggregate (`ApolloAggEndpointsHosted`).

**The combined table** (`APOLLO_AGG_EP_LIST` in `driver\apollo_agg.h`: one X-macro list generates the ACX rows in
`endpoints.c`, the pure table the test reads, and the row indexes; `aggep-test` checks `apollo.inf` against it):

| Row | Reference string | Pin name | Flow | Bridge (form factor) | Plays to / records from | INF |
|---|---|---|---|---|---|---|
| 0 | `ApolloAggMon` | Monitor | render, 2 ch | LINE_CONNECTOR (not renamed "Speakers") | every destination of the `windows` routes (Studio: Twin X MON = the speakers); no route: the configured master's MON L/R | can be default |
| 1 | `ApolloAggMonIn` | Monitor mix | capture, 2 ch | LINE_CONNECTOR | record 0x0901/0x0902 (monitor-mix loopback) of the first `windows` route's unit, else the master's | NeverSetAsDefault |
| 2-9 | `ApolloAggX8pIn1-8` | x8p MIC/LINE 1-8 | capture, mono | MICROPHONE | the x8p's 0x0001, 0x0002, 0x0103, 0x0104, 0x0305-0x0308 (slots 0-7) | default-capable |
| 10-11 | `ApolloAggTwxIn1-2` | TwinX MIC/LINE 1-2 | capture, mono | MICROPHONE | the Twin X's 0x0001/0x0002 (slots 0/1) | default-capable |

Unit and slot come from the stored configuration (`ApolloAggEpResolve`): input rows follow the first configured unit of
their model wherever it sits in the block order; slots come from the cached descriptor (the row's fallback slot only
while a unit's descriptor was never seen, and only for the ID the row names); a unit not configured or absent =
silence. The INF gives the 12 reference strings their three interfaces each on the x8p's install section only, with
`FriendlyName "Apollo"` (`Apollo_Agg_Endpoint_AddReg`). **Name caveat:** on this PC Windows composed the standalone
names from the device description ("MON L/R (Apollo x8p (open driver, test build))", not the interface FriendlyName
"Apollo x8p"), so expect "Monitor (Apollo x8p (open driver, test build))"; the scripts match `Monitor (` + `(Apollo`
and say which form appeared. Stereo pair rows (6.7) are not built: `GET_ENDPOINTS` carries 16 rows
(`APOLLO_ENDPOINTS_MAX`), and 12 + 5 pairs do not fit without an IOCTL layout change.

**How the streams run (`agg.c`, `stream.c`).** A combined row's stream (`APOLLO_ENDPOINT_DESC.AggRow1 != 0`,
`APOLLO_STREAM.Agg`) is not in the x8p's engine list: Run attaches it to the aggregate's list and the **timeline unit's
tick** (`ApolloAggAfterTick`) moves it, exactly like the combined ASIO client: render ADDs (saturating) into the
timeline ring directly and into staging rings for follower destinations, from which the resampler writes the
follower; capture reads the timeline ring directly and followers through the resampler at the aligned head
(`Pm - guard - pad`). So Windows and the DAW **sum** on the same outputs. Locks: the stream's engine fields are under
`Agg.Lock` (`stream.c StreamLockOf`, the same callbacks as before with the other lock); completions are collected under
`Agg.Lock` and sent after every lock is dropped, under the stream's rundown (engine.c's rule); nothing new is nested.
Positions count timeline frames and are rebased (monotonic) on every timeline change; with no timeline they stand
still. Reported latency: lead of the timeline (render); guard + pad and the timeline's ADC (capture).
Staging is now shared: zeroed once per tick from the last head to `Pm + lead` before any writer
(`ApolloAggStageZeroFrom`), mapped **incrementally** (a client starting or stopping keeps the others' rings and
content; fresh only when the timeline changes), and follower render runs whenever any client renders, reading up to the
common head (G6: the ASIO client's). The routing follows the configuration live (`AggStreamsResolve` on every rebuild).
While the host has its combined circuits added the aggregate runs a timeline even with Combined off (they are then
the only Windows endpoints); the combined ASIO driver still needs `enabled` (ASIO_OPEN/START refuse, and
`AGG_GET_LAYOUT` reports ACTIVE only when enabled).

**Twin X loss.** Its gate closes, the x8p's tick takes the timeline (acting master, epoch bump: ASIO reset request,
Windows streams rebased); destinations on the Twin X go silent (staging only, no follower render), its inputs read
zero; the Twin X's leave waits for the tick's rundown before its rings go. On return it joins, takes the timeline back,
streams rebase. The host's loss removes the endpoints (and, daisy-chained, the Twin X).

**G6 leftovers.**
- **Align=0** (`APOLLO_AGG_CFG_ALIGN` clear; `agg-test set --align 0`, Apollo Control JSON `"align": false`): no pad,
  `getLatencies` = the timeline's own (Twin X master: in 21 + buffer), followers read `h + M` behind phi(x)
  (`ApolloAggCapOffset`); their own latency is longer, which ASIO cannot express (6.3.5).
- **RenderTrim / CaptureTrim**: `Units\<key>\RenderTrimFrames` / `CaptureTrimFrames` (DWORD read as signed frames,
  clamped to +/-64; unit params 13/14, default 0), read when the unit joins and cached in the configuration's unit entry
  (the former `Reserved` USHORT, now two `CHAR`s) so an absent unit keeps them. Only the difference to the timeline's
  counts: a follower's capture reads `CapTrim_f - CapTrim_tl` frames older, its playback plays `RenTrim_f - RenTrim_tl`
  later; the pad grows to cover a negative one. The +5.000-frame G6 loopback offset is taken out by x8p
  `CaptureTrimFrames = 0xFFFFFFFB` (-5) with the Twin X as master (aggep-test: offset 10, pad 42, in 63).
- **Start ratio**: `Parameters\Aggregate\Ratios` (REG_BINARY, 4 x {follower HW_UID, timeline HW_UID, ppb, seq}). A
  follower locked for 30 s whose ratio moved 20 ppb or more from the kept one (checked every 10 min) marks it; the next
  service pass of any unit writes it (try-lock, after DevLock is released), and so does a leave. Formation starts from
  this run's last lock against the same timeline, else the kept pair, else the reverse pair inverted, else 1; logged
  ("formed against the timeline, start ratio +4760 ppb (kept ...)").
- **Config change notification**: `AGG_GET_STATE.ConfigGeneration` (was Reserved[0]) moves on every `AGG_SET_CONFIG`
  and differs after a reload; Apollo Control's worker polls it once a second and sends an unsolicited `agg` when it
  moved for another reason (its own agg-set records the generation it produced). Also `EndpointSet` and
  `EndpointStreams` in the state, flags `ALIGNED` and `RATIOS`.

**G5/G6 review fixes folded in.** (1) `APOLLO_AGG_MAX_LAG` 1024 (was 4096): a capture catch-up beyond 21 ms re-originates
(worst case 8.5 M MACs in one tick instead of 33.8 M; asrc-test bounds it). (2) The configured master takes its timeline
back after it has run 1 s again (`ApolloAggTakeOverWhy` reason 2: an engine recovered with TRANSPORT_START, a transport
started by hand), with the epoch bump (ASIO reset) of a takeover; and a successful `TRANSPORT_START` now joins the unit
(`ioctl.c`, after DevLock is released). (3) `AGG_SET_CONFIG` refusals complete with **success and `Error`** (METHOD_BUFFERED
returned nothing under an error status: "check 0"); Apollo Control words the reason, agg-test and the ASIO clock menu
test `Error`.

**What did not change.** Structure sizes and IOCTL codes (`APOLLO_AGG_VERSION` 1; new fields took Reserved space; a
G6 driver reports generation 0). With `EndpointSet` absent both units create exactly their pre-G7 circuits and their
streams take the pre-G7 code (`AggRow1` 0). With ALIGN set and every trim 0 the aggregate's layout, pad and follower
offsets are G6's byte for byte (`aggep-test` against the frozen `tools\agg-test\ref\apollo_agg_g6.h`, 20,000 random
configurations); one-unit configurations have no pad whatever the flags.

**Tests** (`tools\agg-test\build.cmd`, in build-all): new `aggep-test` (498 checks: table choice, the 12 rows, the INF,
routing resolution for none/Studio/Mirror/Headphones split/x8p master/never-seen/absent units, G6 identity, unaligned
and trimmed margins, the tick windows and takeover/reclaim, the ratio table), `asrc-test` + the lag bound,
`aggjson-test` + `"align"`.

**Tools.** `agg-test state` prints the endpoint set, combined streams, alignment, start ratios and generation;
`agg-test config --save <file>` / `set --load <file>` (exact binary round trip, JSON included); `set --routes
studio|mirror|phones|none` now routes the Windows `Monitor` as well as the DAW pair; `set --align 0|1`.
`asio-test --agg <s> [frames] --also <Hz>` also measures a second tone on both MON loopbacks.

**Script `tools\phase1\step-g7.ps1`** (admin; installs nothing). Pass 1: checks both units on apollo.sys with a G7
package (installed INF has `ApolloAggMon`, generation non-zero) and the Twin X in Mode 2, saves the Windows defaults
(`step-g7.defaults.json`), writes `EndpointSet = 1`, then a power cycle of both units (or `-Restart`: warm, x8p strips
off until its next power cycle). Pass 2: waits for both bring-ups, restores the defaults (several times; a saved
default that was a unit's own endpoint, now gone, moves to its combined counterpart: MON L/R -> Monitor, MIC/LINE n of
the x8p -> x8p MIC/LINE n), checks the driver tables (12 rows on the x8p, none on the Twin X), the aggregate (COMBINED,
active, Twin X master + timeline, x8p LOCKED) and the Windows names (one `Monitor (...)` out, `Monitor mix (...)`,
the MIC/LINE rows, no standalone left); then (unless `-NoAudio`, after a warning) a 20 s `wasapi-loop` Monitor ->
Monitor mix, and with the routing saved and set to none: a 30 s 997 Hz Windows tone on Monitor while `asio-test --agg 20
128 --also 997` plays 1 kHz on both units: PASS when the DAW test passes and the 997 Hz is on the Twin X MON loopback
but not the x8p's (sum proven); the configuration is put back (`set --load`). `-LossTest` (opt-in): 90 s Windows tone
while Aaron powers the Twin X off and on; checks it returns as timeline, the stream kept running, the x8p connected.
`-Rollback`: `EndpointSet` removed, restart, defaults mapped back.

**What only the hardware answers (G7 morning):** Windows takes the combined circuits and their names as expected
(which parenthetical, form factors honoured); the Monitor stream runs glitch-free on the timeline tick and its
completions pace audiodg like the standalone streams; the sum on the Twin X MON; follower destinations (Mirror)
through the shared staging; x8p MIC/LINE through the resampler (needs the x8p's strips: cold start); Twin X loss and
return with a Windows stream open; the master reclaim; a start ratio kept after 30 s locked and used at the next load.

### 6.10 INF, install and revert changes

**INF (`driver/apollo.inf`):**
- Add `%ApolloTwinX.DeviceDesc% = Apollo_Install_TwinX, PCI\VEN_1A00&DEV_0002&SUBSYS_00191A00`; split the
  install section per model so DeviceDesc ("Apollo Twin X (open driver, test build)") and the standalone
  endpoint FriendlyName ("Apollo Twin X") differ; both sections share CopyFiles, Services and Wdf.
- Add the aggregate reference strings (`AggMon`, `AggMonIn`, `AggIn<u><n>`, ...) with FriendlyName
  "Apollo" to both sections (only the host creates them).
- Service DisplayName "Apollo open driver" (no model). Keep the exact four-part IDs: never match
  `DEV_0002` without SUBSYS. UA's WHQL package keeps ranking above ours (spec-bringup R4), so the Twin X
  stays on UA's driver unless forced, and a forced bind defaults to `Mode=0`.

**Scripts and installer (separately from the driver):**

| Script | Change |
|---|---|
| `tools/phase1/install.ps1` | `-Unit x8p|twinx` (hwid from a table, not `install.ps1:21`); for the Twin X: save the six audio defaults, warn that UA Console loses the Twin X, do **not** kill UA apps (the x8p rule at `:72-79` stays), force-bind with devcon for that hwid only, wait for `apollo-diag --unit twinx info`, write the unit key `Mode` as asked (default 0), restore defaults |
| **new** `tools/phase1/revert-unit.ps1 -Unit twinx` | the per-device way back: idle check for that unit only (ASIO, combined endpoints), find UA's published INF (`pnputil /enum-drivers`: Original Name `uad2pcie.inf`, Provider Universal Audio), `devcon update <that oem INF> "PCI\VEN_1A00&DEV_0002&SUBSYS_00191A00"` (forced, only the Twin X), **do not delete our package** (the x8p still uses it), verify DEVPKEY_Device_Service is UA's on the Twin X and still `apollo` on the x8p, restore the audio defaults (Twin X WDM speakers back as communications default), ask for a **Twin X** power cycle (the x8p stays up) |
| `tools/phase1/revert.ps1` | unchanged meaning (everything back to UA, deletes our packages); say clearly it also moves the x8p |
| Desktop shortcut "Twin X back to UA" | runs `revert-unit.ps1 -Unit twinx -Yes` elevated: the panic button for the speakers |
| `installer/helper` | per-unit check/bind/unbind (`common.h:34-36`, `pnp.cpp:109, 289`); unbind of one unit = UA INF force-update as above; fix the "SUBSYS 0x000F" comment |
| `tools/phase1/audio-helpers.ps1` | `Wait-ApolloIdle -Unit`, so reverting the Twin X does not wait for x8p clients |

---

## 7. Phased plan and test gates

Order keeps the speakers working as long as possible: identify, then outputs (speakers back on our
driver at G4), then inputs, then the combined device. Each gate is one session; "power cycle the Twin X"
never drops the x8p.

**Before any Twin X gate (once):** decide where monitoring goes during G2-G3 (open question 1). Either
accept up to about 20 minutes without speakers, or move the speaker cables to the **x8p MON outputs**,
which already play on our driver (verified Nov 14), or use headphones on the x8p.

| Gate | Build / setting | Aaron does | Proves | Rollback |
|---|---|---|---|---|
| **G0** | identify fix, model table, per-unit keys, log tags, `--unit`, offline `ident-test` (identification over recorded and synthetic register sets: x8p real, Gen 1 family 19, digits 2024/2025, Gen 2 digits), `asrc-test` (ASRC + loop on recorded and synthetic position traces: ppm steps, jumps, granularity), strip planner for mask 0x03 | nothing (runs in build-all) | code paths correct without hardware | none |
| **G1** | G0 installed on the x8p only (Twin X untouched) | `step` script as for release4; `apollo-diag info`, `engine` | no x8p regression (per-unit keys read, legacy globals still apply to the x8p, single-member aggregate = today's engine); record the x8p SAMPLE_POS step size and ppm vs QPC for 10 min | previous release |
| **G2 observe** | Twin X bound, `Mode=0` (identify + quiesce, nothing else) | speakers down; `install.ps1 -Unit twinx`; `apollo-diag --unit twinx info` and `log`; then either stay for G3 or `revert-unit` | family 19, DSP count (Duo/Quad), FPGA, FW, serial digits, public 0x19 -> type 0x99, ext_caps ready cap, HW_UID, BAR0 size, previous-owner words 0x01A4/0x01AC, descriptor and clock mask **as UA left them** (stale, labelled so) | `revert-unit.ps1 -Unit twinx`, Twin X power cycle |
| **G3 bring-up + transport** | `Mode=1`, no endpoints, no strips, batch 1 | power-cycle the Twin X (cold); `info`, `log`, `engine`; then `apollo-diag --unit twinx tone 10 --slots <MON L/R>` with speakers low; then disable/enable the Twin X (warm) and repeat `info`. **As built (6.9.3):** `step-g3.ps1` pass 1, power BOTH units off and on (the Twin X needs its own cold start), pass 2 (PASS/CHECK, no tone); the tone by hand (`tone 5`, MON slots from the live descriptor; `levels 5` in a second window); `step-g3.ps1 -WarmTest` | boot transport, early ACEFACE, deferred ring connect consumed, readback alive, 0xC07C 0x201, SAMPLE_POS 48 kHz, descriptor contents and counts (compare 3.2), clock mask (Peer 0x0A? ADAT?), MON loopback at -20.0 dBFS on the MON record slots, audible tone; warm ACEFACE works or needs `WarmBootTransport` | `step-g3.ps1 -Rollback` (observe after a Twin X power cycle), or `revert-unit` + `-Rollback` |
| **G4 speakers on our driver** | `Mode=2`, Combined off, standalone `MON L/R (Apollo Twin X)` | set it as default output; 30-min `wasapi-loop` on the Twin X (MON loopback). **As built (6.9.4):** `step-g4.ps1` pass 1 (package on both units, `Units\<key>\Mode=2`, `EnabledEndpoints=3`), power both units off and on (or `-Restart`: warm, x8p strips off until its power cycle), pass 2 (PASS/CHECK, defaults restored); then the printed `wasapi-loop 1800 --render "MON L/R (Apollo Twin X" --capture "MON L/R (Apollo Twin X"` (plays a tone into the speakers) | Windows audio to the speakers through our driver, glitch-free; the two endpoints named `MON L/R (Apollo Twin X ...)`; HP2/preamp settings refused; on a cold start 0xC07C 0x201 within 2 s of the clock write and the 48 kHz rate ack logged (step 17 ok); warm start steps all ok or n/a; x8p unchanged; **monitoring restored from here** | `step-g4.ps1 -Rollback` (Mode 1 at the next Twin X start), or `revert-unit` (defaults restored) |
| **G5 Twin X inputs** | strips per unit: dry run (`StripsEnable=2`), then full chain (`StripSteps=0`, `StripMask=3`), batch 1; speakers off, headphones low (5.2 feedback) | power-cycle the Twin X each step; `strips`, `levels` while speaking into input 1, then 2, then Hi-Z; `48v` for a condenser. **As built (6.9.5):** `step-g5.ps1` run 1 (values), Twin X power cycle, run 2 (dry run checks, arms the full chain), Twin X power cycle, run 3 (strips, then the inputs one by one; plays nothing) | Gen 1 boot words owned, pool layout passes, modules accepted, mic 1/2/Hi-Z reach capture slots 0/1, 48V resync; if capture stays zero: batch 2 A/B | `StripsEnable=0` + power cycle; or `revert-unit` |
| **G6 combined, software clock** | Combined on, master Twin X, ASRC Default | `asio-test --agg --run 1800 128`: tone out on both units' MON, capture both MON loopbacks; then Ableton 30 min on "Apollo (Avium)" | ratio settles (ppm logged), `e` within +/- 1 frame, 0 resyncs in 30 min, `AsrcMaxUs` within budget, constant phase between the two loopbacks (no drift), alignment error after `Align` within 1 frame; Avium Measure sweep/THD+N on a follower channel meets the 6.3.3 target | Combined off (`AGG_SET_CONFIG`), units stay standalone |
| **G7 Windows endpoints + routing** | Combined endpoint set on the x8p host (admin, host restart) | Apollo Control: Studio preset, then Mirror; play Windows audio and the DAW together; power the Twin X off and on during playback. **As built (6.9.7):** `step-g7.ps1` pass 1 (`Aggregate\EndpointSet = 1`, defaults saved), power both units off and on (or `-Restart`), pass 2 (checks, 20 s WASAPI loop through Monitor, 30 s Windows + DAW sum test, defaults restored; `-LossTest` for the Twin X power cycle) | combined endpoints appear once, Windows + DAW sum on the speakers, Twin X loss = silence + recovery, ASIO reset on master change, no bugcheck | endpoint set back to standalone (`step-g7.ps1 -Rollback`) |
| **G8 optional, optical** | `ClockMode=Auto`, cable added | `AGG_CLOCK_PROBE`; S/PDIF test tone x8p -> Twin X | lock detected automatically, mode B, bit-exact S/PDIF transfer, counter offset constant for 30 min; unplug the cable: falls back to mode A without a dropout on master channels | `ClockMode=Software` |
| **G9 robustness** | all on, Verifier standard (minus 0x2 for timing runs, PLAN notes) | x8p power cycle while streaming (both vanish), sleep/resume, 2-hour soak | surprise-removal order of two units, re-formation, no leaks | per-gate rollbacks |

---

## 8. Open questions (only Aaron or the hardware can answer), ranked

1. **(Aaron) Monitoring during G2-G3.** Accept about 20 minutes without speakers per session, or move the
   speakers to the x8p MON outputs (already working on our driver) or use headphones while the Twin X is
   being brought up?
2. **(Hardware, G2) Twin X identity:** DSP count (the UA log suggests a Quad), FPGA and firmware values,
   serial digits 5-8 (2024/2025 needs the retype of 1.2), ext_caps ready capability, HW_UID.
3. **(Hardware, G3) Does Gen 1 take the Gen 2 bring-up:** boot transport with 0x20F, early ACEFACE,
   deferred ring connect, warm ACEFACE with the transport stopped? The descriptor contents and counts, and
   the clock-source mask (S/PDIF; ADAT; Peer 0x0A).
4. **(Hardware, G5) Does the strip recipe fit Gen 1:** boot-word ownership on DSP 1, pool geometry,
   capture with batch 1.
5. **(Aaron) Default channel order** in the combined ASIO device and endpoints: Twin X first (UA's
   monitor-unit-first order) or x8p first?
6. **(Aaron) ASRC quality default:** Default (h = 16, +16 frames) or High (h = 32, +32 frames)? And is
   `Align` always on for him (recommended for recording)?
7. **(Aaron) Twin X inputs in daily use:** which mics (48V?), does he use the Hi-Z input and the talkback
   mic (talkback has no strip recipe yet)?
8. **(Aaron) Losing UA features on the Twin X while on our driver:** UA Console, Unison and UAD plug-ins on
   the Twin X go away, as they did on the x8p. Confirm before G2.
9. **(Aaron, only if he adds a Toslink cable later) Does anything use the x8p's optical output as ADAT
   today?** Optical sync turns it into S/PDIF.
10. **(Aaron) Rates above 48 kHz** for the combined device: none planned in v1.

---

## Appendix A. Values to compare at gates G2-G3

Measured at G2 (Nov 14, observe, UA's driver had left the unit; `step-g2.log`): **[HW]**. G3 column:
what `step-g3.ps1` pass 2 checks (6.9.3).

| Item | Expected before G2 | Measured at G2 **[HW]** | Expected at G3 (Mode 1, cold) |
|---|---|---|---|
| `info` ExtCaps | 0x813004?? with [15:8] = 0x02 or 0x04 | **0x81310211**: audio ext, family 19, **2 DSPs (Duo)**, ready cap 1 | same |
| PublicType / DeviceType | 0x19 / 0x99 | 0x19 / 0x99 ("Apollo Twin X (Gen 1)") | same |
| Serial, HW_UID, unit key | | 20000000000002 (digits 5-8 "2024": retyped to 0x99, 1.2), HW_UID 0x000000002F00BEEF, `Units\000000002F00BEEF` | same |
| BAR0, FPGA, firmware | | 64 KiB, FPGA 0xA70EA929, FW 0x0B0FC9B3 = 11.15.51635 (untested pair: flagged, FW_UNTESTED) | same; add the tested row after G3 passes |
| Previous owner 0x01A4 / 0x01AC | | 0x0008270B / 0x03220065 | (logged at every start) |
| Stage after G3 | 5 Connected; `WarmStart` 0 after a power cycle | 1 Mapped (observe) | 5 Connected, not WARM_START |
| DMA_CTRL after bring-up | 0x7 (Duo) or 0x1F (Quad) | (not read in observe) | **0x7**; observe now logs UA's value and clears its enables |
| NotifRateInfo | 0x201 after transport start | **0x101** (UA left 44.1 kHz, lock 1) | 0x101 or 0x201 at connect, **0x201** after the start (`rate: ... 48000 Hz locked`) |
| IO descriptors | record about 24, playback about 10 (inference, 3.2) | **record 34, playback 46** (v1; full decode in 6.9.3) | the live counts are the transport counts (expect 34 / 46) |
| MON L/R slots | the slots of 0x0901/0x0902 | playback 0/1, record (loopback) 26/27 | same, from the live descriptor (`G5: MON L/R:` log line) |
| Clock mask 0xC08C | includes 0x0001 (S/PDIF) and 0x1000 (Internal); 0x0400 (Peer) likely | **0x1421007E**: S/PDIF, ADAT, **Peer 0x0A**, Internal; rates 0x7E (44.1-192 kHz) | the LIVE read with our transport running (`ClockSrcsLive`, log `clock sources (LIVE ...)`): Peer offered? (6.3.7) |
| Bring-up steps | | | `BringupSteps` 0xFFEFE (every step but E10, which runs only when E8 fails) |
| Engine | | | about 1000 ticks/s, 48000 frames/s |
| MON loopback of a -20 dBFS tone | -20.0 dBFS on the record slots of 0x0901/0x0902 | | -20.0 dBFS on record 26/27 (`tone` by hand, `levels`) |
| Strip DSP 1 boot word at a cold start | `(w & ~0xFF00) == 3` | | logged (`strips: boot words`), strips stay off (G5) |
| DSP 1 pools | code base 0xE0000, tops 0x9D1B8 / 0xC6FFE / 0x08FFE000 | | logged (`strips: DSP 1 pools`), nothing sent (G5) |

## Sources (web)

- UA help center, Multi-Unit Cascading:
  [help.uaudio.com/hc/en-us/articles/26817742017684](https://help.uaudio.com/hc/en-us/articles/26817742017684-Multi-Unit-Cascading),
  [205927139](https://help.uaudio.com/hc/en-us/articles/205927139),
  [Apollo Solo Multi-Unit Cascading](https://help.uaudio.com/hc/en-us/articles/360051505332-Apollo-Solo-Multi-Unit-Cascading)
  (clocking over Thunderbolt from the monitor unit; do not interconnect word clock/ADAT/MADI between
  Apollos; one desktop unit per system; Twin as monitor unit not recommended). The pages were read
  through search summaries (direct fetch refused, HTTP 403).
- Microsoft Learn: [Floating point support for 64-bit drivers](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/floating-point-support-for-64-bit-drivers),
  [Using Floating Point in a WDM Driver](https://msdn.microsoft.com/en-us/library/windows/hardware/ff565388(d=printer,v=vs.85).aspx),
  [KeSaveExtendedProcessorState](https://learn.microsoft.com/en-gb/windows-hardware/drivers/ddi/wdm/nf-wdm-kesaveextendedprocessorstate),
  [Floating-point annotations for drivers](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/floating-point-annotations-for-drivers).
