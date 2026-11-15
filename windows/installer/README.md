# ApolloSetup: test-mode installer for the Apollo x8p open driver

`ApolloSetup-<DriverVer>.exe` is one file that installs the test-signed `apollo.sys` driver for the
Universal Audio Apollo x8p (Thunderbolt), its tools, the ASIO driver and Apollo Control, and switches
the x8p from UA's driver to ours. It is a **test-mode** installer: the driver is signed with the
developer test certificate "WDKTestCert aaron" (thumbprint `8ACDF79F5C64144C08650382DB32B1866ABC2EC6`),
not by Microsoft. A Microsoft-signed (attestation) release is planned and will replace all of this.

## Requirements and limits

- **Windows 11 22H2 (build 22621) or later, x64 only.** Setup refuses anything else, including Arm64.
- **Secure Boot off.** With Secure Boot on, Windows will not enter test mode, so Setup stops at once
  and changes nothing. Turning Secure Boot off is a firmware (UEFI) setting; Setup never touches it.
- **Test signing on.** Setup asks for consent and turns it on (`bcdedit /set {current} testsigning on`);
  it takes effect after one restart. Windows then shows "Test Mode" on the desktop.
- **Administrator rights.** Setup elevates once at start.
- **x8p only.** Setup binds exactly `PCI\VEN_1A00&DEV_0002&SUBSYS_00141A00`. From gate G2 on,
  `apollo.inf` also lists the Apollo Twin X Gen 1 (`SUBSYS_00191A00`, test only, Mode 0 = observe by
  default), but Setup never binds it: a Twin X stays on whatever driver it has. One that a developer put on
  the Apollo driver (`tools\phase1\install.ps1 -Unit twinx`) is handled: see "Two units" below. Any other
  UA device stays on UA's driver.
- **UAD software for the mic strips.** The five input-strip modules are copied out of UA's UAD install
  (`apollo-diag strips install`). Without UAD, the driver works but the preamp strips stay off.
- **WebView2 runtime** for Apollo Control (part of Windows 11).
- **No Driver Verifier.** Setup never enables it; that stays a developer-machine step
  (`tools\phase1\setup.ps1`).

## What gets installed

`C:\Program Files\Avium\Apollo\` (admin-only, so nothing elevated ever runs from a user-writable folder):

| Path | What |
|---|---|
| `apollo-control.exe`, `ui\` | Apollo Control (WebView2 app; runs as the normal user) |
| `apollo-helper.exe` | the installer's own helper (below); also behind the "Connect" shortcut |
| `driver\apollo.inf/.sys/.cat` | the driver package, exactly as frozen |
| `driver\apollo.cer` | the test certificate |
| `asio\avium-asio.dll` | ASIO driver "Apollo x8p (Avium)", registered with regsvr32 |
| `tools\apollo-diag.exe`, `loopback-test.exe`, `wasapi-loop.exe`, `asio-test.exe` | diagnostics |
| `docs\PHASE1.md`, `PHASE2.md`, `STRIPS.md`, `ASIO.md` | the project documents |
| `defaults.json` | the driver registry values for this release |
| `payload-manifest.sha256` | SHA-256 of every file above (check with `Get-FileHash`) |
| `state\` | runtime files: saved audio defaults, which certificate stores Setup added to |

Elsewhere: the certificate in LocalMachine **Trusted Root** and **Trusted Publishers**; the driver package
in the Windows driver store; values under `HKLM\SYSTEM\CurrentControlSet\Services\apollo\Parameters`;
the strip modules in `%SystemRoot%\System32\drivers\ApolloStrips` (admin-only); Start-menu folder
**Avium Apollo**; optionally the `HKCU\...\Run` value "Avium Apollo Control" (the same value the app's
own "start with Windows" switch writes).

## The wizard, page by page

1. **Secure Boot check** (before any page). If Secure Boot is on: a message explains that test-signed
   drivers cannot load with Secure Boot on and that a signed release is planned; Setup closes.
2. **Welcome.** What this is, x8p only, UAD needed for the strips, close audio programs first.
3. **BitLocker** (only when test signing is off and BitLocker protects, or may protect, the Windows
   drive). Turning on test signing changes a boot setting BitLocker checks, so the next start may ask
   for the 48-digit recovery key: have it ready (https://aka.ms/myrecoverykey). The checkbox
   "Suspend BitLocker for the next restart" (on by default) runs
   `manage-bde -protectors -disable C: -RebootCount 1`; protection resumes by itself after that restart.
4. **Test signing** (only when it is off). Explains test mode and asks for consent; Setup cannot go on
   without it. Nothing is changed until you click Install.
5. **Select additional tasks.** "Start Apollo Control when I sign in" (off by default).
6. **Ready to install.** The exact list of what will happen, in order.
7. **Installing.** In this order:
   1. (only if consented) suspend BitLocker, then turn on test signing. If Windows refuses, Setup
      stops here with nothing installed.
   2. Copy the files to Program Files.
   3. Log `apollo-helper check` (the full system state) into the Setup log.
   4. Add the certificate to Trusted Root and Trusted Publishers. The helper refuses any certificate
      whose thumbprint is not the one pinned at build time, and records which stores it actually added
      to (a store that already had it is left alone at uninstall).
   5. Register the ASIO DLL (`regsvr32`).
   6. Write the driver registry values from `defaults.json`. This happens before the driver binds, so its
      first start reads them. Values already on the machine are kept unless the release forces them.
   7. Check that nothing uses the x8p (ACX streams and ASIO on the x8p's own device interface, audio
      sessions on its "... (Apollo x8p (open driver, test build))" endpoints, the Apollo tools; a Twin X
      never counts). If something does, a dialog lists it: **Retry**, **Ignore** (go ahead; Windows may
      then want a restart), or **Abort** (skip the driver switch; use "Connect" later).
   8. Save your 6 default playback/recording devices (console, multimedia, communications).
   9. Close UA's apps, but only if the x8p is still on UA's driver and no other connected unit (a Twin X)
      is: UA's apps serve every unit on UA's driver, so they keep running for it.
   10. Bind the driver to the x8p's exact hardware ID: `UpdateDriverForPlugAndPlayDevices` with
       `INSTALLFLAG_FORCE`, because UA's WHQL driver outranks ours. With no x8p connected, the package is
       only staged. Older copies of our package that no unit (connected or remembered) is installed from
       are removed.
   11. Put your default devices back where Windows moved them (three passes over about 8 seconds).
   12. If UAD is installed: `apollo-diag strips install`. Otherwise a note that the mics need UA's
       modules.
   13. Start-menu shortcuts: **Apollo Control**, **Connect x8p to Apollo driver**, **Uninstall Apollo
       driver**; the sign-in entry if chosen.
8. **Results.** What each step did, in plain words (the full log is `%TEMP%\Setup Log *.txt`).
9. **Finished.** Asks to restart if test signing was just turned on or the driver switch needs one.
   Until that restart the x8p cannot run on the Apollo driver.

### "Connect x8p to Apollo driver" (Start menu)

UA's driver ranks higher than ours, so whenever UA's software is installed Windows gives a newly
connected x8p to UA. This shortcut runs `apollo-helper connect`: it asks for administrator rights,
checks test signing is active, waits until nothing uses the x8p, saves the default devices, closes UA's
apps, binds the driver and restores the defaults, then waits for Enter so you can read the result.
(Without UA's software installed, Windows picks the staged Apollo driver by itself.)

## Uninstall (Settings, Apps, or the Start-menu entry)

1. Log `apollo-helper check` (every unit and its driver).
2. Check nothing uses the x8p (Retry/Ignore/Abort; Abort stops with nothing changed).
3. Save the default audio devices.
4. `apollo-helper unbind`:
   1. Every unit other than the x8p that is on the Apollo driver (a Twin X), connected or only
      remembered by Windows, must have UA's driver installed (`uad2pcie.inf` from Universal Audio,
      signed by Microsoft, listing its exact hardware ID). If one has not, the uninstall **stops with
      nothing removed** and says so: removing our package would leave that unit with no driver at all.
      Install UA Connect, then uninstall again.
   2. Each such connected unit is moved back to UA's driver first, by its exact hardware ID (the way
      `tools\phase1\revert-unit.ps1` does it). A remembered one gets UA's driver when it next connects.
   3. Remove every driver-store package with provider "Avium" and original name `apollo.inf` (the x8p
      gets its next-best driver, i.e. UA's), rescan, and delete the leftover `apollo` service entry once
      nothing uses it.
5. Put the default audio devices back; a message says which units to power-cycle.
6. `regsvr32 /u` the ASIO DLL.
7. Remove the certificate from the stores Setup added it to.
8. Remove the files, `state\` and `%SystemRoot%\System32\drivers\ApolloStrips`.
9. Offer to turn test signing off (default **No**: leave it on). Yes offers to suspend BitLocker for
   the restart first.

Afterwards power-cycle the x8p (and a Twin X that was on the Apollo driver) so UA's software starts each
fresh. Your Apollo Control settings (`%APPDATA%\Avium\ApolloControl`) are kept.

## Two units (x8p and Twin X)

Setup and the helper bind the x8p only, but behave correctly with a Twin X on either driver. The rules
are those of `tools\phase1\install.ps1`, `revert.ps1` and `revert-unit.ps1`; the decisions are in
`helper\units.cpp` and tested offline at every build (`test_units`).

| Situation | What happens |
|---|---|
| Twin X on UA's driver | Untouched. UA's apps are not closed when the x8p switches (the Twin X still needs them); if Windows then wants a restart for the x8p, power-cycle the x8p. |
| Twin X on the Apollo driver, same build as Setup's | The x8p binds normally; the Twin X is checked unchanged afterwards and its package is never deleted while it uses it. |
| Twin X on the Apollo driver, another build | `bind` stops with nothing changed (exit 5): both units share one loaded `apollo.sys`, so the x8p would not run the new package. Move the Twin X back first: `apollo-helper unbind --unit twinx` (elevated) or `revert-unit.ps1 -Unit twinx`, then "Connect x8p to Apollo driver". |
| Twin X with no UA driver installed | A note: `apollo.inf` lists it, so Windows may give it the Apollo driver by itself when it next connects (observe mode unless its unit key says otherwise). Install UA Connect to prevent that. |
| Uninstall with a Twin X on the Apollo driver | It goes back to UA's driver first; without UA's driver the uninstall stops (above). |
| Two devices with the same hardware ID | `bind` / `unbind --unit` stop: Windows would move both. |

## Command line

Standard Inno Setup switches work (`/SILENT`, `/VERYSILENT`, `/LOG="file"`, `/NORESTART`). Extra:

| Switch | Meaning |
|---|---|
| `/TESTSIGNING=on` | required for a silent install while test signing is off (silent installs cannot ask) |
| `/SUSPENDBITLOCKER=no` | silent install: do not suspend BitLocker (default: suspend if BitLocker is on) |
| `/SANDBOXTEST=1` | testing only, see below: continue past the Secure Boot stop, leave test signing alone, stage the driver without binding it |

A silent uninstall (`unins000.exe /VERYSILENT`) leaves test signing on.

## apollo-helper

`C:\Program Files\Avium\Apollo\apollo-helper.exe <command>`. Source in `installer\helper\`
(C++, EWDK, /W4 /WX). Exit codes: 0 done, 1 error, 2 no x8p (staged only) or the named unit is not
connected, 3 still in use, 4 needs elevation, 5 stopped before changing anything (another unit would lose
its driver, or runs another driver build), 3010 restart needed.

| Command | Admin | Does |
|---|---|---|
| `check` | no (more detail when elevated) | JSON: OS build, Secure Boot, test signing (this boot and next boot), BitLocker on the Windows drive, `x8p` (as before), `units`: every connected UA device with its kind (`x8p`, `twinx`, `other`), exact hardware ID, driver (`apollo`, `ua`, `other`, `none`), INF and version, and for units on our driver what the driver says (`GET_INFO`: model, Mode in effect, observe/active, unit key); `remembered_on_apollo`; `unit_settings` (`Parameters\Models\*` and `Units\*` Mode values); our packages (and whether a unit uses each); UA's packages and signer; UAD, WebView2 |
| `bind <inf> [--stage-only]` | yes | force the driver onto the x8p's exact hardware ID, or stage it if no x8p is connected; stops (5) if another unit on our driver runs a different build |
| `unbind` | yes | hand every other unit on our driver back to UA's driver (stops with 5 if UA's driver for one is missing), then remove all Avium `apollo.inf` packages, rescan, report |
| `unbind --unit x8p\|twinx` | yes | that unit only back to UA's driver; our package stays (`revert-unit.ps1`). Run `idle-wait` first for the x8p |
| `defaults save <file>` / `defaults restore <file> [--passes N]` | no | the default endpoints, through IPolicyConfig |
| `idle-wait <seconds> [--ask]` | no | wait until nothing uses the x8p (driver IOCTLs `GET_ENDPOINTS`/`GET_ENGINE_STATS` on the x8p's interface only, audio sessions on its endpoints, the tools) |
| `quit-ua` | no | close UA's apps, only while the x8p is on UA's driver and no other connected unit is |
| `regdefaults <defaults.json>` | yes | write the Parameters values |
| `cert add <cer> [--expect <sha1>] [--record <file>]` / `cert remove <cer> [--record <file>]` | yes | LocalMachine Root + TrustedPublisher |
| `connect` | asks | the Start-menu shortcut |

`installer\helper\ioctl_check.cpp` compiles against the packaged driver's own `apollo_ioctl.h` and fails
the build if the IOCTL codes, struct sizes or offsets the helper reads ever change (`GET_INFO`'s unit fields
only from IOCTL version 6 on; a version 5 driver reports no Mode). `installer\helper\test_units.cpp` tests
the unit logic in `units.cpp` (which unit is which, quit-ua, the uninstall hand-back, the one-image rule)
without hardware; `helper\build.cmd` runs it and fails on any failed check.

## Building a release

From a plain command prompt, with the EWDK ISO mounted (default `D:`):

```
installer\build.cmd [frozen build folder] [EWDK drive]
installer\build.cmd ..\builds\2026-11-14-combined D:
```

The frozen build folder must have the usual layout: `package\` (apollo.inf/.sys/.cat), `bin\`
(apollo-diag.exe, loopback-test.exe, wasapi-loop.exe, asio-test.exe, avium-asio.dll) and
`src\include\apollo_ioctl.h`. The app comes from `app\bin\` (exe + `ui\`), the documents from `docs\`,
the certificate from `driver\x64\Release\apollo.cer`. Nothing outside `installer\` is built or written.

What `build.cmd` does:

1. Fetches Inno Setup **6.7.4** into `installer\tools\inno-6.7.4\` if missing (`get-inno.ps1`): the file
   jrsoftware.org links to (jrsoftware's GitHub release), checked against the pinned SHA-256 from
   jrsoftware's signed `.issig` and an Authenticode signature by "Pyrsys B.V.", installed per-user and
   portable (`/CURRENTUSER /PORTABLE=1`: no admin, no registry, no uninstaller).
2. Sets up the EWDK (`SetupBuildEnv.cmd amd64`).
3. Builds `apollo-helper.exe` at /W4 /WX with CFG, /sdl and System32-only DLL loading, after running
   the offline unit test (`test_units`).
4. Signs the helper with the test certificate if its private key is in `CurrentUser\My` or
   `PrivateCertStore` (otherwise Setup is built unsigned and the build says so).
5. Stages the payload in `out\stage\` (`stage.ps1`): checks the certificate's thumbprint, that it signed
   `apollo.cat` and `apollo.sys`, that the INF's provider is "Avium", that it lists the x8p's hardware ID
   and no UA ID other than the x8p's and the Twin X Gen 1's exact ones; writes `payload-manifest.sha256`.
6. Compiles `ApolloSetup.iss` (signing Setup and its uninstaller when possible). Log:
   `out\iscc-<DriverVer>.log`; Inno's own list of compiled files: `out\ApolloSetup-<DriverVer>-files.txt`.
7. Verifies (`verify.ps1`) that the files compiled into Setup are exactly the payload manifest, with the
   same SHA-256 each, then prints the output's size and SHA-256 and writes `<exe>.sha256`.

Output: `installer\out\ApolloSetup-<DriverVer>.exe`, where DriverVer is the INF's (e.g. `12.36.20.8`).

**Per-release settings:** edit `installer\defaults.json`, or put a `defaults.json` in the frozen build's
root (it wins). A number is a REG_DWORD; `{ "type": "dword", "data": "0xFF" }` or `"sz"` for other forms;
`"overwrite": true` on one value forces it over what an earlier install left.

**New Inno Setup version:** change the version, URL and SHA-256 at the top of `get-inno.ps1` (take the
SHA-256 from `https://files.jrsoftware.org/is/6/innosetup-<ver>.exe.issig`) and `INNO_VER` in `build.cmd`.

## Testing it safely

Kernel drivers cannot load in Windows Sandbox, and no test of this installer should run on the
development PC with the x8p (the uninstaller would remove the developer's own test certificate only if
Setup added it, but it does unbind and remove every Avium apollo.inf package).

1. **Static (every build):** `build.cmd` already checks the helper (/W4 /WX), the IOCTL layout, the
   certificate, the compile, and the file list against the manifest.
2. **Windows Sandbox: layout, UI and uninstall.** Copy the exe into `C:\Users\aaron\Sandbox\installers`
   (mapped read-only), open `Test Sandbox.wsb`, and in the sandbox run:

   ```
   Desktop\installers\ApolloSetup-12.36.20.8.exe /SANDBOXTEST=1 /LOG="%USERPROFILE%\Desktop\setup.log"
   ```

   `/SANDBOXTEST=1` continues past the Secure Boot stop (the sandbox VM may report Secure Boot on),
   leaves test signing alone and only stages the driver (no x8p there anyway). Check: the pages and
   texts; `C:\Program Files\Avium\Apollo` against `payload-manifest.sha256`; the Start-menu shortcuts;
   `certlm.msc` (Trusted Root and Trusted Publishers); `pnputil /enum-drivers` (an "Avium" apollo.inf);
   `reg query HKLM\SYSTEM\CurrentControlSet\Services\apollo\Parameters`; the ASIO key under
   `HKLM\SOFTWARE\ASIO`; `apollo-helper check` (x8p absent, UAD absent). Then uninstall from Settings and
   check that all of it is gone (the Results page and the log say what each step did).
3. **A spare test PC with an x8p** (Secure Boot off, BitLocker recovery key at hand): install, restart,
   confirm "Test Mode", `apollo-helper check` (x8p `on_apollo_driver: true`), Apollo Control, the default
   devices unchanged, then uninstall and confirm the x8p is back on UA's driver after a power cycle.
   Also try: install with the x8p unplugged, plug it in, run "Connect x8p to Apollo driver".

What has **not** been exercised yet: every path that needs real hardware or a real install (bind,
unbind, unbind --unit, quit-ua, IPolicyConfig restore, idle-wait against a running driver, the
BCD/BitLocker WMI reads, the wizard itself). The build checks are static, plus the offline unit test.
`apollo-helper check` (read-only) has run on the development PC with the x8p and a Twin X both on the
Apollo driver (Nov 14 2026, g3 build): both units, their Modes (x8p 2, Twin X 1), the unit keys and UA's
package (`uad2pcie.inf`, Microsoft-signed, listing both IDs) were reported correctly.
With two units, also test on the spare PC: Twin X on UA's driver (Setup must leave UA's apps open and the
Twin X unchanged), Twin X on the Apollo driver (uninstall moves it back first), and uninstall with UA's
driver removed (it must stop with nothing removed).

## Files in this folder

| File | Role |
|---|---|
| `build.cmd` | the build (above) |
| `ApolloSetup.iss` | the Inno Setup script: wizard, install and uninstall logic |
| `defaults.json` | driver registry defaults for the release |
| `get-inno.ps1` | fetch and verify the pinned Inno Setup |
| `stage.ps1`, `verify.ps1`, `find-signing-cert.ps1` | staging + manifest, output check, signing-key lookup |
| `helper\` | apollo-helper sources and `build.cmd` |
| `tools\` | downloaded Inno Setup (not source) |
| `out\` | build output (not source) |
