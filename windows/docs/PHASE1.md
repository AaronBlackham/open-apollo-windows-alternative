# Phase 1 runbook: first contact with the x8p

Test machine: this laptop. The admin steps are scripts in `tools\phase1\` that you run in an
admin PowerShell (Claude Code's auto mode will not switch on test signing or add a root
certificate by itself). Each script writes a `.log` next to it that Claude reads. You handle the
hardware: cables, the x8p's power switch, the mic.

## Before starting

- **Save your work.** A driver bug blue-screens the whole PC.
- Quit UAD Console and UA Connect.
- The Twin X stays connected (Aaron, Nov 14) and on UA's driver: our INF matches only the x8p.
  It is daisy-chained behind the x8p's Thunderbolt card, so the x8p power cycle (test 5) and
  cable pull (test 7) drop it off the bus too; it re-enumerates when the x8p comes back.
- `install.ps1` closes UA Connect and UA's mixer for the driver handover. Reopen UA Connect
  afterwards if you want Console for the Twin X; it can no longer reach the x8p.
- Plug a **dynamic** mic (no phantom needed) into input 1.
- Turn the monitors or headphones **down** before any tone test.

## Paths

- `<package>` = `apollo-win\driver\x64\Release\apollo\` (apollo.sys, apollo.inf, apollo.cat)
- Test certificate = `apollo-win\driver\x64\Release\apollo.cer` ("WDKTestCert aaron", valid to 2036)
- Tool = `apollo-win\tools\apollo-diag\apollo-diag.exe`
- `devcon.exe` is copied to `apollo-win\tools\bin\`. The crash-dump debugger (`kd.exe`/`cdb.exe`)
  is on the EWDK ISO under `Program Files\Windows Kits\10\Debuggers\x64\` (the ISO unmounts at
  every reboot; remount it from `Downloads\EWDK`).
- Rebuild: mount the EWDK ISO, run `LaunchBuildEnv.cmd`, then `driver\build.cmd` and
  `tools\apollo-diag\build.cmd`.

## 1. One-time setup (one reboot)

In an admin PowerShell:

```
powershell -ExecutionPolicy Bypass -File C:\Users\aaron\Desktop\Avium\apollo-win\tools\phase1\setup.ps1
```

It runs `bcdedit /set testsigning on`, `verifier /standard /driver apollo.sys`, and adds
`apollo.cer` to the Root and TrustedPublisher stores. Then reboot. A "Test Mode" watermark appears in the corner of the screen.

Driver Verifier makes driver bugs crash immediately, at the line responsible, instead of
quietly corrupting memory. If the PC crashes on every boot with the x8p connected: unplug
the x8p, boot, then run `verifier /reset`.

## 2. Put our driver on the x8p

1. Connect and power on the x8p. UA's driver binds to it as usual.
2. In an admin PowerShell:
   `powershell -ExecutionPolicy Bypass -File C:\Users\aaron\Desktop\Avium\apollo-win\tools\phase1\install.ps1`
   It checks test signing is active, closes UA's apps, stages the package (`pnputil /add-driver`),
   forces it onto the x8p (`devcon update ... "PCI\VEN_1A00&DEV_0002&SUBSYS_00141A00"`, because
   UA's Microsoft-signed package ranks higher), waits up to 2 minutes for bring-up, then saves
   `apollo-diag info` and `apollo-diag log` to `install.log`.
3. Expect device type 0x22, 6 DSPs, stage Connected, and a **warm** start (UA's DSP was still
   running).

Since the Nov 14 build (interface v2), `info`, the preamp commands, `mon`, `levels`, `readback`
and `log` work from a normal prompt; `start`, `stop`, `pos`, `capture`, `tone`, `read`, `write`,
`dump` and `setting` still need the admin PowerShell. The control app is
`apollo-win\app\bin\apollo-control.exe` (no admin needed).

## 3. Tests, in order (each one gates the next)

| # | Test | What to do | Pass |
|---|---|---|---|
| 1 | Identity | `info` | type 0x22, serial digits 5-8 read 2017, 6 DSPs |
| 2 | Clock runs | `start`, then `pos 5` | about 48000 frames/s, no stalls, no backwards jumps |
| 3 | Playback | `tone 5` with the monitors low | 1 kHz tone from the monitor outputs |
| 4 | Capture, warm | `capture 5 warm.wav` while talking into input 1 | slot 0's peak is far above the other slots |
| 5 | **Capture after a power cycle** | power the x8p off for 10 s and back on; `info` (warm flag now off); `start`; `gain 1 40`; `capture 5 cold.wav` while talking | slot 0 has signal: no input strips needed. Silent: the x8p needs input strips, and that phase 3 work moves up |
| 6 | Channel identity | signal into each input in turn, `capture` each time | input N lands in slot N-1; ADAT/S/PDIF/virtual slots if you can feed them |
| 7 | Hot unplug | `start`, `pos 30`, pull the Thunderbolt cable mid-run | no blue screen; replugging brings it back |
| 8 | Controls | open Apollo Control; change input 1 gain, PAD and 48V (nothing plugged in that minds phantom), then monitor level, mute, dim; then turn the x8p's own monitor and gain knobs | the unit follows the app; the app follows the front panel; no grey "unconfirmed" dots stay lit; `apollo-diag readback` agrees |

After every test, `apollo-diag log` shows what the driver did.

## Going back to UA's driver

- `tools\phase1\revert.ps1` (admin): removes every staged `apollo.inf` package and rescans, so
  the x8p falls back to UA's `oem194.inf`.
- By hand: Device Manager, the x8p, Update driver, Browse my computer, Let me pick, then UA's
  "Universal Audio Apollo Thunderbolt" driver (`oem194.inf`).
- Power-cycle the x8p afterwards so UA's software starts it fresh.

Undo test mode when development is over: `bcdedit /set testsigning off` and `verifier /reset`.

## If it blue-screens

Write down the stop code on the blue screen. After the reboot, Claude reads the crash dump in
`C:\Windows\Minidump\` with the EWDK's debugger and fixes the cause before the next attempt.
