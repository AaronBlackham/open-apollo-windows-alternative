# Apollo Windows driver: build plan

Goal: the Apollo x8p (then the Twin X) works in Zoom, Discord and every other Windows app with no crackle, and Windows sees every individual channel.

Source of truth for hardware behaviour: the three specs in this folder, ported from open-apollo (GPL-2.0, `reference/open-apollo`).

- [spec-bringup.md](spec-bringup.md): PCI identity, resources, probe-to-ready sequence, teardown, ioctl ABI
- [spec-streaming.md](spec-streaming.md): DMA ring, transport, position, latency, endpoints
- [spec-dsp-mixer.md](spec-dsp-mixer.md): DSP rings, mixer mailbox, preamps, input strips

## Architecture

One KMDF PCI function driver (`apollo.sys`) using ACX for the audio endpoints. It binds only to the unit it is installed on, by exact hardware ID:

| Unit | Hardware ID | Bound to |
|---|---|---|
| Apollo x8p | `PCI\VEN_1A00&DEV_0002&SUBSYS_00141A00` | ours (first target) |
| Twin X | one of UA's Twin subsystems (0x07, 0x08, 0x0C-0x0E), read when connected | UA's driver until phase 5 |

UA's package (`uad2pcie.inf`, WHQL) ranks higher than a test-signed driver, so ours is assigned to the x8p by hand at install.

Streaming design (spec-streaming §3, §6):

- The Apollo moves every channel through one interleaved ring per direction: 8192 frames, 32-bit containers with 24 valid bits, 32 record / 34 playback channels on the x8p (ADAT layout).
- The only clock is `AX_SAMPLE_POS` (BAR0 0x2244), a frame counter that wraps with the ring. The driver extends it to 64 bits and timestamps every read.
- A 1 ms high-resolution timer reads `SAMPLE_POS` and copies frames between the hardware ring and one small buffer per Windows endpoint. Windows is told the position the hardware actually reached, so its audio engine never drifts against the Apollo's clock.
- Transport starts once, at 48 kHz, and keeps running for as long as the unit is powered. Endpoints open and close without touching the transport (rapid start/stop wedged the PCIe link on Linux).

Every channel is exposed. Each input appears twice: as its own mono endpoint and inside a stereo pair (MIC 1-2, ADAT 3-4 ...). Outputs are stereo pairs. The copy engine fans one ring slot out to every endpoint that uses it.

## Phases and test gates

### Phase 0: code without hardware (now)
- KMDF driver skeleton: PnP/power callbacks, BAR0 mapping, MMIO access layer with a surprise-removal gate, single MSI interrupt + DPC, 32-bit DMA enabler, common buffers for both rings and the SG tables.
- x8p bring-up path from spec-bringup §3 (quiesce first, before anything else touches the device; identity; cold/warm detection; DSP ring connect; SG tables before any transport start; reset-strobe pulse per issue #46).
- Transport prepare/start/stop from spec-streaming §2 including the post-start clock write (0xC074 = 0x20C, doorbell 4).
- Diagnostic IOCTL interface and a small `apollo-diag.exe`: read identity, dump registers, start transport, watch `SAMPLE_POS`, capture N seconds of the record ring to a WAV with every channel.
- INF for the x8p hardware ID only.

Gate: builds clean with the EWDK at warning level 4, Code Analysis clean.

Status (Nov 10): **gate passed.** Built with EWDK 28000.2526 (VS 2026 Build Tools, MSVC 14.50):
driver clean at /W4 /WX with Code Analysis, `infverif /w` reports the INF valid, catalog
generated and test-signed; `apollo-diag.exe` clean at /W4 /WX. Nothing has run on hardware yet.
Fixes the first build needed: `WdfWaitLockAcquire` takes a `PLONGLONG` timeout; the vcxproj
needs `FilesToPackage` for inf2cat; the IOCTL device type must be unsigned; one C6386 clamp in log.c.

| File | What it does |
|---|---|
| `driver/driver.c` | DriverEntry, device add: interrupt, 32-bit DMA enabler, admin-only interface, sequential passive queue |
| `driver/device.c` | PnP/power lifecycle; ordered x8p bring-up (cold E1-E12 / warm F path, then ACEFACE connect) in a work item |
| `driver/hw.c` | MMIO gate, timing, model identification, vector mask helpers, ISR/DPC, DMA_CTRL strobe rules, liveness checks |
| `driver/dsp.c` | DSP command rings: allocate, neutralize, program, per-DSP enable pulse, connect + dated finalize, rate ack |
| `driver/audio.c` | 4 MiB rings + SG tables, ACEFACE + post-handshake reads, transport prepare/start/stop + post-start clock write, 64-bit position, capture/playback copies, playback zero-ahead, quiesce |
| `driver/mixer.c` | Mixer mailbox (one-shot masks, single SEQ bump), readback drain, preamp gain/48V/pad/line, 10 ms service thread |
| `driver/ioctl.c` | Diagnostic IOCTLs (buffered I/O; inputs copied before outputs are written) |
| `driver/apollo.inf` | Matches only `PCI\VEN_1A00&DEV_0002&SUBSYS_00141A00`; one MSI message; DMA-remapping compatible |
| `tools/apollo-diag/` | `info`, `dump`, `read`/`write`, `start`/`stop`, `pos`, `capture` (WAV + per-channel peak/RMS), `tone`, `gain`/`pad`/`48v`/`line`, `setting`, `readback` |

### Phase 1: first contact (x8p connected, test signing on)
1. Load the driver on the x8p with UA's software closed. Read identity: model must resolve to 0x22.
2. Bring-up completes; `SAMPLE_POS` advances at 48 kHz (no freeze: the issue #46 symptom).
3. Power-cycle the x8p, then record input 1 with `apollo-diag`. **This answers the open question: does the x8p need DSP input strips for the mic to reach capture?** (spec-dsp-mixer §3)
4. Channel identity: signal into each input in turn, confirm which ring slot it lands in. Required for slots above record 16 / playback 18, where the firmware descriptor (34/52) disagrees with the verified layout (32/34).
5. Unplug and replug the Thunderbolt cable while streaming: no bugcheck.

### Phase 2: Windows endpoints
- ACX capture and render endpoints for every channel, named from the route table (`MIC/LINE 3`, `ADAT 5`, `VIRTUAL 1`, `MON L/R` ...).
- Copy engine at 1 ms; playback safety offset 8 frames at 44.1/48k; capture latency reported as 10 frames (x8p).
- Glitch test: play a 1 kHz sine out of Monitor and loop it into input 1 for 30 minutes; analyse for discontinuities. Then 30-minute Zoom and Discord calls.

Gate: zero discontinuities over 30 minutes, under Driver Verifier.

### Phase 3: controls
- Preamp gain, 48V (needs the 0-then-1 edge, 200 ms apart, after a power cycle, for all 8 preamps), pad, mic/line/Hi-Z through the mixer mailbox at BAR0 0x3800.
- Monitor level: read back from the unit; never default to the daemon's -96 dB.
- Small tray app for these controls.
- Input strips if phase 1 step 3 shows the x8p needs them (modules from the local UAD install; pins exist only for UAD 12.0.0.602, installed here is 11.4.0).

Status (Nov 14): **control app built ahead of hardware** (Aaron asked for a configurable UI that
controls the unit). Unverified on hardware until phase 1 runs.

| Piece | What it does |
|---|---|
| `include/apollo_ioctl.h` v2 | Access split: interactive users get read access, which covers status, levels, readback, log, preamp and monitor IOCTLs; raw registers, raw mixer settings, transport and ring I/O need admin. New: `SET_MONITOR_PARAM`, `GET_LEVELS`; input params gain LowCut, Phase, Link |
| `driver/mixer.c` | `ApolloSetMonitorParam`: level, HP1 level, mute, mono, dim, dim level, source, HP1/HP2 source, talkback (open-apollo `ua_monitor_set_param` bits) |
| `driver/audio.c` | `ApolloLevelsRead`: per-slot peak over the newest 1024 capture frames |
| `driver/apollo.inf`, `driver.c` | Device SDDL `D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;IU)` for the whole stack + `FILE_DEVICE_SECURE_OPEN` |
| `app/` | `apollo-control.exe`: Win32 + WebView2 host (`src/main.cpp`), UI in `ui/`. Strips for inputs 1-8 (gain, Mic/Line, 48V, PAD, low cut, polarity, link, meters), monitor (level, mute, dim, mono, dim level, source), headphones, talkback, 32-slot meter bridge, device info + driver log, settings (names, colors, hide, 48V confirm, monitor limit, gain lock, meters, theme, tray, start with Windows), presets. Follows the front panel through readback and flags changes the unit did not take. Demo mode (`--demo`, or open `ui/index.html` in a browser). Closes its device handle on PnP query-remove so installs never wait on it |
| `app/tests/` | Playwright checks of the demo model and the hardware message path (`run.cmd`) |

Build: `tools\build-all.cmd` (driver with Code Analysis, infverif, apollo-diag, app).
Not offered: HP2 level (kext 0x66 lands in S14 bits 7:0, which hold preamp 5-6 GainC on the
x8p). Meters need the transport running, which the driver starts only in phase 2. On a cold
start the monitor module may ignore level changes until the full 57-param monitor block is sent
(spec-dsp-mixer 6.1 C14): watch for it in phase 1 test 5.

### Our own DSP (Nov 14; alongside UA plug-ins, which our driver can host too: docs/PLUGINS.md)
Voice chain in `dsp/` (low cut, gate, EQ, compressor, de-esser, limiter), 45 checks passing,
`dsp-render` for WAV files. Runs as a Windows APO on the capture endpoints after phase 2; the
on-chip SHARC route is research. Details: [DSP.md](DSP.md).

### Phase 4: robustness
- Sleep/resume, hot-unplug during a call, long soak, Driver Verifier with standard + DMA verification.

### Phase 5: Twin X
- Twin X path differs (boot transport, early ACEFACE, ring connect after ACEFACE on every connect; capture needs input strips). Gen 1 is unverified upstream.

### Later: signing for normal boot
- Test signing needs Secure Boot off. Running with Secure Boot on needs attestation signing (EV certificate plus a Microsoft Partner Center hardware account).

## Decisions

| # | Decision | Status |
|---|---|---|
| 1 | Toolchain | EWDK 26H1 ISO, downloading to `C:\Users\aaron\Downloads\EWDK` |
| 2 | First target | x8p |
| 3 | Every channel visible in Windows | required (Aaron, Nov 10) |
| 4 | Endpoint layout | every input as a mono endpoint AND in a stereo pair; outputs as stereo pairs (Aaron, Nov 10) |
| 5 | Twin X generation | open |
| 6 | Test machine | this laptop (Aaron, Nov 10). Secure Boot already off, C: not encrypted, memory integrity off; test signing = `bcdedit /set testsigning on` + reboot. Runbook: [PHASE1.md](PHASE1.md) |
| 7 | Update UAD to 12.0.0.602 if the x8p needs input strips | open, depends on phase 1 step 3 |
| 8 | Control IOCTLs open to interactive (non-admin) users so the app runs without elevation; raw register/transport access stays admin | Nov 14 (made with the app build) |
| 9 | ASIO driver too, for Ableton and Avium Measure (Aaron, Nov 14). User-mode ASIO DLL under the ASIO SDK's GPLv3 option (dual-licensed since Oct 2025), separate from the GPL-2.0 kernel driver; kernel adds ring mapping + a period event. Order: input strips (mics) and ASIO, then ACX Windows devices | Nov 14 |
| 10 | Firmware frozen at 11.15.54803 / FPGA 0xA241C5AC; no own firmware (brick risk); driver flags any other version | Nov 14 |

## Top risks

- **Silent mic after a power cycle** if the x8p needs input strips (proven only on x4 and Twin X Gen 2).
- **UA's driver touching the unit first.** Strips can only be built on a DSP nothing else touched since power-up; power-cycle after any UA session.
- **Bus mastering before our quiesce.** If UA's driver left the transport running, the quiesce must be the very first register access.
- **Long waits at start-up** (ACEFACE up to 20 s, DSP ready 30 s or more) must run in a work item, not inline in device start.
- **Kernel bugs crash the whole PC.** Every test build runs under Driver Verifier first; kernel dumps are kept for analysis.
