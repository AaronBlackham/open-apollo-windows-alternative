# open-apollo for Windows

A Windows driver stack for Universal Audio Apollo Thunderbolt interfaces, ported from open-apollo's Linux
driver and mixer engine in this repository. It replaces UA's Windows driver on the unit and gives Windows
every channel as its own sound device, an ASIO driver, and a console app.

Targets: **Apollo x8p (Gen 1)** and **Apollo Twin X (Gen 1)**, alone or together. Windows 11 x64.

## What is here

| Folder | What it is |
|---|---|
| `driver/` | `apollo.sys`: KMDF + ACX 1.1 kernel driver. Bring-up, DSP rings, input strips, mixer, monitor and cues, Windows audio endpoints, the combined two-unit device, UAD plug-in hosting, sample rates, speaker correction, clock source |
| `include/` | The driver's IOCTL interface (`apollo_ioctl.h` and friends) |
| `asio/` | `avium-asio.dll`: ASIO driver for one unit or both units as one device |
| `app/` | Apollo Control: Win32 + WebView2 console app (six themes, both units side by side, plug-in browser, inserts, aux buses, sessions) |
| `dsp/` | A portable C voice-processing chain (EQ, compressor, gate, de-esser, limiter) |
| `tools/` | `apollo-diag` (diagnostics and raw control), `apollo-plug`, offline test suites, the hardware step scripts in `tools/phase1/` |
| `plugins/tools/` | open-apollo's plug-in mapper and module store, ported to run on Windows |
| `installer/` | A test-mode installer (Inno Setup + a helper) |
| `docs/` | Plans, specs and the host/UI contract (`docs/apollo-control-units.md`) |

## Status (November 2026)

Proven on an x8p and a Twin X Gen 1:
- Cold and warm bring-up of both units; transport at 48 kHz; surprise removal and power-cycle recovery
- Input strips on all inputs, kept running across driver reloads (warm-start adoption)
- Preamp controls, monitor section, monitor mix, cues
- 23 Windows sound devices (every output pair, every mic/line input, the monitor mix); 30-minute WASAPI runs clean
- ASIO for one unit, and both units as one ASIO device (drift correction over Thunderbolt)
- UAD plug-ins on the x8p's DSP: Ampeg SVTVR Classic, Lexicon 480L, Precision De-Esser, UA 1176LN Legacy

Built and passing the offline suites, not yet run on hardware: sample rates 44.1 to 192 kHz, speaker
correction, plug-ins on the Twin X, four inserts per input with REC/MON, aux buses, clock source and digital
I/O formats (gated), front-panel link, sessions. Against open-apollo's parity map of 1,015 UA driver
behaviours this port covered 61.7% (open-apollo: 72.8% on Oct 11 2026).

## Build

1. Mount the Enterprise WDK (EWDK) ISO, then from a command prompt:
   `<EWDK>\BuildEnv\SetupBuildEnv.cmd amd64`
2. `tools\build-all.cmd`

This builds every test suite, the driver (test-signed), the tools, the ASIO DLL and Apollo Control, and runs the
offline checks. The driver build signs with a local test certificate; see `docs/PHASE1.md`.

## Install (development)

The driver is test-signed: Windows must be in test-signing mode (`bcdedit /set testsigning on`, Secure Boot off).
From an administrator PowerShell, with nothing playing through the Apollo:
`powershell -ExecutionPolicy Bypass -File tools\phase1\step-ep.ps1`
It stages the package, binds both units, restores your Windows default devices and lists the endpoints.
`tools\phase1\revert.ps1` puts UA's driver back.

## Universal Audio files

Nothing made by Universal Audio is in this repository. The input-strip modules and UAD plug-ins are read at run
time from the UAD software already installed on the same PC, and the plug-in licences from the unit itself.
Nothing here contacts UA's servers or loads UA's DLLs. Do not commit anything from `plugins/bundles`,
`plugins/p1`, `plugins/store` or a UA install (`.gitignore` blocks them).

## Licences

- Driver, app, tools: GPL-2.0, as open-apollo
- ASIO driver (`asio/`): GPL-3.0, as Steinberg's ASIO SDK in `third_party/asiosdk`
- `app/third_party/webview2`: Microsoft's WebView2 SDK, BSD licence (its `LICENSE.txt`)
