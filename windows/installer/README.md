# ApolloSetup: test-mode installer for the Apollo open driver

`ApolloSetup-<DriverVer>.exe` is one file that installs the open `apollo.sys` driver for the Universal Audio
**Apollo x8p** and **Apollo Twin X Gen 1** (Thunderbolt), its tools, the ASIO driver and Apollo Control, and
switches the units you choose from UA's driver to this one. It is a **test-mode** installer: the driver is not
signed by Microsoft. Nothing in the download is signed by anyone: Setup signs the driver on your PC, with a
certificate made there and kept there (see "How the driver is signed").

The text for the GitHub release is [RELEASE-NOTES.md](RELEASE-NOTES.md).

## Requirements and limits

- **Windows 11 22H2 (build 22621) or later, x64 only.** Setup refuses anything else, including Arm64.
- **Secure Boot off.** With Secure Boot on, Windows will not enter test mode, so Setup stops at once and changes
  nothing. Turning Secure Boot off is a firmware (UEFI) setting; Setup never touches it.
- **Test signing on.** Setup asks for consent and turns it on (`bcdedit /set {current} testsigning on`); it takes
  effect after one restart. Windows then shows "Test Mode" on the desktop.
- **Administrator rights.** Setup elevates once at start.
- **Windows PowerShell 5.1** (part of Windows) able to run a script from the install folder: Setup runs
  `sign-local.ps1` with `-ExecutionPolicy Bypass`. A Group Policy that forbids scripts makes signing fail, and Setup
  then stops before switching any unit.
- **Units:** Apollo x8p (`PCI\VEN_1A00&DEV_0002&SUBSYS_00141A00`) and Apollo Twin X Gen 1
  (`PCI\VEN_1A00&DEV_0002&SUBSYS_00191A00`), on Thunderbolt, alone or both. `apollo.inf` lists exactly these two
  IDs (`stage.ps1` refuses any other UA ID). Gen 2 units and every other Universal Audio device stay on UA's driver.
- **UAD software for the mic strips.** The five input-strip modules are copied out of UA's UAD install on your PC
  (`apollo-diag strips install`). Without UAD the driver works, but the preamp strips stay off.
- **WebView2 runtime** for Apollo Control (part of Windows 11).
- **No Driver Verifier.** Setup never enables it; that stays a developer-machine step (`tools\phase1\setup.ps1`).

## What is not in this release

- **A Python runtime or plug-in maps.** UAD plug-in hosting is optional and set up after install, per user, from
  Apollo Control or the Start menu (section "UAD plug-ins", below). Setup bundles none of it: no Python, no wheels,
  no maps.
- **Anything made by Universal Audio.** No UA file is in the installer. The input-strip modules come from your own
  UAD install (`apollo-diag strips install`, as above), at install time, on your PC; the plug-ins' modules and maps
  come from it when you set up plug-ins.
- **Other units.** Gen 2 units and other Apollo models are never switched.
- **A Microsoft signature.** The driver runs in test mode only.

Setup says the same on its last page.

## What gets installed

`C:\Program Files\Avium\Apollo\` (admin-only, so nothing elevated ever runs from a user-writable folder):

| Path | What |
|---|---|
| `apollo-control.exe`, `ui\` | Apollo Control (WebView2 app; runs as the normal user) |
| `plughelper.py` | Apollo Control's plug-in parameter planner (runs in the per-user Python that "Set up UAD plug-ins" installs) |
| `plugin-setup\tools\` | "Set up UAD plug-ins": `setup-plugins.ps1`, `setup_plugins.py`, open-apollo's vendored mapper (GPL-2.0) and our ports (`install_panels.py`, `oa_assets_dsp.py`, `oa_offline.py`); source only |
| `apollo-helper.exe` | the installer's own helper (below); also behind the "Connect Apollo units" shortcut |
| `sign-local.ps1` | signs the driver on this PC (below) |
| `driver\apollo.inf`, `apollo.sys`, `apollo.cat` | the driver package: unsigned in the download, signed on this PC |
| `asio\avium-asio.dll` | ASIO drivers "Apollo x8p (Avium)" and "Apollo (Avium)" (both units as one device), registered with regsvr32 |
| `tools\apollo-diag.exe`, `loopback-test.exe`, `wasapi-loop.exe`, `asio-test.exe` | diagnostics |
| `docs\RELEASE-NOTES.md`, `INSTALLER.md` (this file), `ASIO.md`, `ASIO-SDK-LICENSE.txt` | documents |
| `defaults.json` | the driver registry values for this release |
| `payload-manifest.sha256` | SHA-256 of every file above as downloaded (`apollo.sys` and `apollo.cat` differ once signed here) |
| `state\` | runtime files: `local-cert.txt` (thumbprints of the certificates made here), `local-cert.cer`, saved audio defaults |

Elsewhere: the per-PC certificate (public part only) in LocalMachine **Trusted Root** and **Trusted Publishers**;
the driver package in the Windows driver store; values under
`HKLM\SYSTEM\CurrentControlSet\Services\apollo\Parameters` (and `Parameters\Models\99`); the strip modules in
`%SystemRoot%\System32\drivers\ApolloStrips` (admin-only); the Start-menu folder **Avium Apollo**; optionally the
`HKCU\...\Run` value "Avium Apollo Control" (the same value the app's own "start with Windows" switch writes).

## How the driver is signed (per PC)

The release carries **no signature and no certificate of anyone**: `apollo.sys` and `apollo.cat` are unsigned
(the build strips any build-time signature and makes the catalog again, unsigned, with Inf2Cat, then checks), and so
are Setup and the helper. Windows still needs a signed package, so after copying the files Setup runs, elevated:

```
powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "{app}\sign-local.ps1" -Package "{app}\driver" -State "{app}\state"
```

`sign-local.ps1`:

1. makes a new code-signing certificate "Apollo driver (signed on <PC name>)", key non-exportable, in
   LocalMachine\My, and writes its thumbprint to `state\local-cert.txt` at once;
2. signs `apollo.sys` and `apollo.cat` with it (SHA-256, no time-stamp server is contacted);
3. adds the public certificate to LocalMachine Trusted Root and Trusted Publishers (Windows installs a test-signed
   package from a trusted publisher without asking, once test mode is on);
4. deletes the private key, whatever happened before (nothing can ever be signed with that certificate again);
5. checks both signatures are Valid.

It loads PowerShell's certificate modules from System32 only (`$PSHOME\Modules`), never from the user's module
folder. It runs only once test signing is on (this boot, or turned on by Setup for the next one), and before any
unit is touched. If it fails, Setup removes whatever certificate it recorded (`apollo-helper cert remove-local`),
says why, and switches no unit. The wizard explains all of this on its "Driver signing" page.

A repair or an update signs again with a new certificate; each one's thumbprint is added to `local-cert.txt`. The
older ones stay trusted until uninstall (their private keys are gone, and a unit that was not switched again may
still be installed from the package they signed). **Uninstall removes exactly the certificates listed in
`local-cert.txt`** from Trusted Root, Trusted Publishers and LocalMachine\My, by thumbprint, and nothing else. An
install from before Nov 16 2026 (which shipped `driver\apollo.cer`) is still cleaned up: its uninstall removes that
certificate from the stores `state\cert-record.txt` says Setup added it to, and only with that record.

## The wizard, page by page

1. **Secure Boot check** (before any page). If Secure Boot is on, a message says why the driver cannot load and
   Setup closes without changing anything.
2. **Welcome.** What this is, which units, how it is signed, close audio programs first.
3. **BitLocker** (only when test signing is off and BitLocker protects, or may protect, the Windows drive). Turning
   on test signing changes a boot setting BitLocker checks, so the next start may ask for the 48-digit recovery
   key: have it ready (https://aka.ms/myrecoverykey). "Suspend BitLocker for the next restart" (on by default) runs
   `manage-bde -protectors -disable C: -RebootCount 1`; protection resumes by itself after that restart.
4. **Test signing** (only when it is off). Explains test mode and asks for consent; Setup cannot go on without it.
5. **Driver signing.** The per-PC signing above, in four steps, with this PC's name.
6. **Apollo units.** One checkbox per supported unit found (Win32_PnPEntity, read before anything is installed),
   ticked by default, with its current driver. A unit already on the Apollo driver is always updated (box greyed):
   every unit on the driver shares one loaded image. Two units of the same model: greyed, not switched (Windows would
   move both). Other Universal Audio devices are listed as "stays on its own driver". With no supported unit
   connected, the page says so: the driver is prepared and "Connect Apollo units" switches a unit later.
7. **Select additional tasks.** "Start Apollo Control when I sign in" (off by default).
8. **Ready to install.** The exact list of what will happen, in order, naming the units.
9. **Installing.** In this order:
   1. (only if consented) suspend BitLocker, then turn on test signing. If Windows refuses, Setup stops here with
      nothing installed.
   2. Copy the files to Program Files.
   3. Log `apollo-helper check` (the full system state) into the Setup log.
   4. **Sign the driver on this PC** (`sign-local.ps1`). On failure: remove the certificate again, a clear message,
      and no unit is switched.
   5. Register the ASIO DLL (`regsvr32`).
   6. Write the driver registry values from `defaults.json` (before any unit switches, so the driver's first start
      reads them).
   7. Check that nothing uses the units being switched (`idle-wait --units ...`: ACX streams and ASIO on their own
      device interfaces, audio sessions on their endpoints, on the combined "(Apollo)" endpoints and, while a unit is
      still on UA's driver, on UA's "(Universal Audio Thunderbolt ...)" endpoints, and the Apollo tools). If something
      does, a dialog lists it: **Retry**, **Ignore** (go ahead; Windows may then want a restart), or **Abort** (skip
      the switch; use "Connect Apollo units" later).
   8. Save your 6 default playback/recording devices (console, multimedia, communications).
   9. Close UA's apps, but only if a unit being switched is on UA's driver and no connected unit that stays (an
      unticked unit, a Gen 2, another model) is still on UA's driver: UA's apps serve every unit on UA's driver.
   10. Switch the chosen units, the x8p first: `UpdateDriverForPlugAndPlayDevices` with `INSTALLFLAG_FORCE` on each
       unit's exact hardware ID (UA's WHQL driver outranks ours). Every other UA device is checked unchanged
       afterwards. With no unit chosen or connected, the package is only staged. Older copies of our package that no
       unit (connected or remembered) is installed from are removed.
   11. Put your default devices back where Windows moved them (three passes over about 8 seconds).
   12. If UAD is installed: `apollo-diag strips install`. Otherwise a note that the mics need UA's modules.
   13. Start-menu shortcuts: **Apollo Control**, **Connect Apollo units**, **Set up UAD plug-ins**, **Uninstall
       Apollo driver**; the sign-in entry if chosen.
   14. With the task **Set up UAD plug-in hosting after install** (unticked by default): the **Set up UAD plug-ins**
       shortcut opens as the user who started Setup (never elevated), in its own window. It explains and asks before
       it downloads anything. Not in a silent install.
10. **Results.** What each step did, one line per unit, in plain words (the full log is `%TEMP%\Setup Log *.txt`).
11. **Finished.** Asks to restart if test signing was just turned on or a switch needs one, says how UAD plug-ins are
    set up, and says plainly what is not in this release (UA files, other units).

### "Connect Apollo units" (Start menu)

UA's driver ranks higher than ours, so whenever UA's software is installed Windows gives a newly connected unit to
UA. This shortcut runs `apollo-helper connect`: it asks for administrator rights, checks test signing is active,
lists the connected units and asks per unit "Switch ... to the Apollo driver? [Y/n]", waits until nothing uses
them, saves the default devices, closes UA's apps when no unit that stays needs them, switches them and restores the
defaults, then waits for Enter so you can read the result. (Without UA's software installed, Windows picks the
staged Apollo driver by itself.)

## Two units

The rules are those of `tools\phase1\install.ps1`, `step-ep.ps1`, `revert.ps1` and `revert-unit.ps1`; the
decisions are in `helper\units.cpp` and tested offline at every build (`test_units`).

| Situation | What happens |
|---|---|
| Both units on UA's driver, both ticked | UA's apps are closed, the x8p is switched, then the Twin X. Both start on this package's driver. |
| One unit ticked, the other stays on UA's driver | UA's apps stay open (the other unit needs them). If Windows wants a restart for the switched unit, power-cycle that unit. |
| Both units on an older build of the Apollo driver (update or repair) | Both are switched to the new package. They share one loaded `apollo.sys`; if the old image is still loaded afterwards (the helper compares the loaded module with the package's driver-store folder), Setup asks for a restart and keeps the old package until then. |
| A unit on the Apollo driver that is not being switched, with another build | `bind` stops with nothing changed (exit 5): the switched unit would join that unit's loaded image. Setup always includes units already on the Apollo driver, so this happens only with `apollo-helper bind --units ...` by hand. |
| A supported unit with no UA driver installed, not switched | A note: `apollo.inf` lists it, so Windows may give it the Apollo driver by itself when it next connects. |
| Two devices with the same hardware ID | `bind` and `unbind --unit` stop: Windows would move both. |
| Uninstall | The Twin X (any unit other than the x8p) on the Apollo driver goes back to UA's driver first, explicitly; then the package goes and the x8p takes its next-best driver (UA's). Without UA's driver for the Twin X the uninstall stops with nothing removed: install UA Connect, then uninstall again. |

## Driver settings (defaults.json)

Written to `HKLM\SYSTEM\CurrentControlSet\Services\apollo\Parameters` before any unit switches. Only values that
differ from the driver's built-in defaults (`driver\apollo_unit.h`) are written; the driver looks each value up in
`Units\<unit key>`, then `Models\<TT>`, then the service-wide values (x8p only), then its built-in default.

| Where | Values | Why |
|---|---|---|
| `Parameters` (service-wide, the x8p only) | `StripsEnable` 1, `StripSteps` 9, `StripMask` 0xFF (forced) | input strips on all eight inputs (built-in: off, 1, 0x01) |
| `Parameters\Models\99` (Twin X Gen 1) | `Mode` 2, `StripsEnable` 1 (forced) | full mode (endpoints, monitor, preamps) with its strips; built-in: Mode 0 = observe only |

Everything else stays at the driver's built-in value: the x8p's `MixerInitBatch` 1, `Endpoints` 0x1FFFF; the Twin
X's `StripMask` 0x3, `StripDsp` 1, `MixerInitBatch` 1, `StripSteps` 0, `Endpoints` 0x3F. The old `EnabledEndpoints`
value is no longer written (the driver ignores it since the endpoint tables were renumbered; the value is now
`Endpoints`). Setup never writes `Units\<key>` (Apollo Control stores per-unit choices such as the sample rate there).

Format (`helper\defaults_plan.h`): `values` for `Parameters` itself, `subkeys` for keys below it (1 to 3 parts of
letters, digits, `_`), a number is a REG_DWORD, `{ "type": "dword", "data": "0xFF" }` or `"sz"` for other forms,
`"overwrite"` per value, per subkey or for the whole file (a value already on the machine is kept unless forced).
Unknown keys are errors. `test_units` parses the release's own `defaults.json` at every build.

## Uninstall (Settings, Apps, or the Start-menu entry)

1. Log `apollo-helper check` (every unit and its driver).
2. Check nothing uses any Apollo unit (Retry/Ignore/Abort; Abort stops with nothing changed).
3. Save the default audio devices.
4. `apollo-helper unbind`: units other than the x8p on the Apollo driver go back to UA's driver first (stops with
   nothing removed if UA's driver for one is missing); then every driver-store package with provider "Avium" and
   original name `apollo.inf` is removed (the x8p gets its next-best driver, UA's), a rescan, and the leftover
   `apollo` service entry is deleted once nothing uses it.
5. Put the default audio devices back; a message says which units to power-cycle.
6. `regsvr32 /u` the ASIO DLL.
7. Remove the per-PC certificates listed in `state\local-cert.txt` (`apollo-helper cert remove-local`); for an
   install from before Nov 16 2026, `driver\apollo.cer` from the stores `state\cert-record.txt` lists.
8. Remove the files, `state\`, `%SystemRoot%\System32\drivers\ApolloStrips` and `...\ApolloModules` (the plug-ins'
   modules, if plug-in hosting was used).
9. Offer to turn test signing off (default **No**: leave it on). Yes offers to suspend BitLocker for the restart first.

Afterwards power-cycle each unit so UA's software starts it fresh. Your Apollo Control settings
(`%APPDATA%\Avium\ApolloControl`) are kept, and so are the plug-in maps and the private Python of anyone who set up
plug-ins (`%LOCALAPPDATA%\Avium\ApolloPlugins`, about 230 MB plus the maps): delete that folder to remove them. It
holds data made from your UAD install: never share it.

## UAD plug-ins (optional, set up after install)

Hosting UAD plug-ins on the Apollo DSPs needs, on each PC and for each Windows user, a private Python with four
packages, open-apollo's panel specs, a module store read from your own UAD install, and a map of each plug-in (made by
running the plug-in's own code in an x86-64 emulator). **Set up UAD plug-ins** does all of it, from three places that
run the same script (`plugin-setup\tools\setup-plugins.ps1`): Apollo Control's plug-in browser (a box while plug-ins
are not set up), Apollo Control's Settings (Plug-in hosting, UAD plug-in setup), and the Start-menu shortcut (a
console window; Setup's optional task opens it after install). Apollo Control explains it in a dialog first, runs the
script hidden at below-normal priority, shows its progress, offers Cancel, and lists each plug-in as soon as it is
mapped.

| Step | What |
|---|---|
| Check | 64-bit Windows; UA's UAD install (`Program Files\Universal Audio\Powered Plugins\UAD-2 Powered Plugins` and `System32\UAD2SDK.dll`, by file name only); 700 MB free. Without UAD it stops here: "No UAD install found", nothing downloaded |
| Python | Python 3.13.17, the 64-bit embeddable package from www.python.org, unpacked into `python\` (its `._pth` removed so the mapper's own scripts run; `PYTHONHOME` pins it, so no other Python on the PC is used) |
| Packages | pip 26.2.2 runs from its wheel, offline, and installs numpy 2.5.4, unicorn 2.1.4, capstone 5.0.9 and pefile 2024.8.26 from the downloaded wheels (`--no-index --require-hashes --only-binary :all: --no-deps`) |
| Mapper code | `plugin-setup\tools\` copied to `tools\` (files that changed only) |
| Panel specs | open-apollo-panels 2026.10.20 from its GitHub release, downloaded by the script (Windows checks GitHub's certificate: a fresh Windows fetches that root on demand, which Python's own download could not) and checked against the pinned SHA-256 `10f24b1d...c3d0`; then `tools\install_panels.py --from` checks it again and installs the specs |
| Module store | `tools\oa_assets_dsp.py dsp` and `strips` read the plug-in DLLs as bytes (nothing is loaded or run) into `store\` |
| Plug-ins to map | the plug-ins your units license (Apollo Control hands over the render ids their licence answers admit), or every plug-in open-apollo can map when no unit has answered (plug-in hosting off, no unit connected) |
| Mapping | `python -m plugin_mapper` with absolute paths into `bundles\`, CPU threads less one at a time |
| Modules | while plug-in hosting is on: one administrator prompt, `apollo-control.exe --plugin-admin` copies the maps' verified modules into `System32\drivers\ApolloModules` (what Settings, Install modules does). With hosting off, turning it on copies them |

Downloads (each checked against the SHA-256 in `setup-plugins.ps1` before it is used; nothing else is downloaded, and
never from a Universal Audio domain):

| File | From | Bytes | SHA-256 | Hash source |
|---|---|---|---|---|
| `python-3.13.17-embed-amd64.zip` | www.python.org/ftp/python/3.13.17/ | 11,441,267 | `9e35319bffedab8b36ad44d8f377cb4f1733bcec257a0b6d04d94f01486ddb38` | python.org release page for 3.13.17 |
| `pip-26.2.2-py3-none-any.whl` | files.pythonhosted.org | 1,819,850 | `fd7f4dfa8190e014013b9f08f3e973f4e4c26ffd179a204895533a7254813a70` | PyPI (also on python.org's 3.13.17 page) |
| `numpy-2.5.4-cp313-cp313-win_amd64.whl` | files.pythonhosted.org | 12,566,113 | `8b4d2fd2d34e5f8c9235ee787de5631a37a28402b15cb80814df973d2be54129` | PyPI |
| `unicorn-2.1.4-cp37-abi3-win_amd64.whl` | files.pythonhosted.org | 15,943,079 | `d7107500c64ce5c168fbff6bef9485b5db1350050036f4cea568650cf8bdbdf5` | PyPI |
| `capstone-5.0.9-py3-none-win_amd64.whl` | files.pythonhosted.org | 1,273,459 | `732cedbbb56d42e723f14d7af6387f1454194a820b4b96b56d1e53f865ef85d0` | PyPI |
| `pefile-2024.8.26-py3-none-any.whl` | files.pythonhosted.org | 74,766 | `76f8b485dcd3b1bb8166f1128d395fa3d87af26360c2358fb75b80019b957c6f` | PyPI |
| `open-apollo-panels-2026.10.20.tar.gz` | github.com (open-apollo-panels release) | 1,007,696 | `10f24b1db43151ce2ad9066c8874666be5c8e94dd64285e31e3c9839107ac3d0` | open-apollo's `configs\plugin-panels.conf` |

About 42 MB in all. None of it is bundled: Setup is about 3 MB, and Python and the wheels (already compressed
archives) would add about 42 MB for a feature many users never turn on. What Setup does carry is the code
(`plugin-setup\tools\`, 91 files, 3.1 MB, about 0.3 MB in Setup).

Everything goes to `%LOCALAPPDATA%\Avium\ApolloPlugins` (Apollo Control finds it there): `python\`, `tools\`,
`downloads\`, `specs\`, `store\`, `bundles\`, `work\`, `setup\` (`status.json` for Apollo Control, `setup.log`).
Never Program Files, the install folder or the registry. A second run continues: finished steps are checked and
skipped, the mapper keeps every map and every plug-in that failed with the same files. Run it again after a UAD
update or after you buy or demo a plug-in.

Times measured on the developer PC (Nov 16 2026, i9-12900H, fast link): downloads 3 s; Python and packages 20 s; panel
specs 6 s; module store 13 s (229 DLLs of UAD 12.0.0.602, 971 modules); one plug-in on one idle worker 9 s (AMS
RMX16) to 28 s (Precision De-Esser); a rerun with everything in place 4 s. With 16 workers busy a plug-in takes about
5 times longer, a few take several minutes, and the Capitol and Hitsville Chambers 15 to 18 minutes each. Disk:
about 230 MB before maps; maps are mostly under 1 MB a plug-in, the two Chambers about 200 MB each (every plug-in:
about 560 MB).

## Command line

Standard Inno Setup switches work (`/SILENT`, `/VERYSILENT`, `/LOG="file"`, `/NORESTART`). Extra:

| Switch | Meaning |
|---|---|
| `/TESTSIGNING=on` | required for a silent install while test signing is off (silent installs cannot ask) |
| `/SUSPENDBITLOCKER=no` | silent install: do not suspend BitLocker (default: suspend if BitLocker is on) |
| `/UNITS=all` | silent install: which connected units to switch, `all` (default), `x8p`, `twinx`, `x8p,twinx` or `none`; a unit already on the Apollo driver is always updated |
| `/SANDBOXTEST=1` | testing only, see below: continue past the Secure Boot stop, leave test signing alone, sign the package, stage it, switch no unit |
| `/FAKEUNITS=x8p=UAD2Pcie,twinx=apollo,other=000F1A00` | with `/SANDBOXTEST=1` only: pretend these units are connected (`x8p2=` for two x8ps; the value is the driver service), to see the units page and the ready list where no unit exists; ignored otherwise |

A silent uninstall (`unins000.exe /VERYSILENT`) leaves test signing on.

## apollo-helper

`C:\Program Files\Avium\Apollo\apollo-helper.exe <command>`. Source in `installer\helper\` (C++, EWDK, /W4 /WX).
Exit codes: 0 done, 1 error, 2 none of the named units connected (staged only) or the named unit is not connected,
3 still in use, 4 needs elevation, 5 stopped before changing anything (a unit would lose its driver, or run another
driver build), 3010 restart needed. `U` below: `x8p`, `twinx`, `x8p,twinx`, `all` (the default) or `none`.

| Command | Admin | Does |
|---|---|---|
| `check` | no (more detail when elevated) | JSON: OS build, Secure Boot, test signing (this boot and next boot), BitLocker, `units`: every connected UA device with its kind (`x8p`, `twinx`, `other`), `supported`, exact hardware ID, driver (`apollo`, `ua`, `other`, `none`), INF and version, and for units on our driver what the driver says (`GET_INFO`: model, Mode, observe/active, unit key); `remembered_on_apollo`; `unit_settings`; `apollo_loaded_image` (the loaded `apollo.sys`); our packages; UA's packages and signer; UAD; WebView2 |
| `bind <inf> [--units U] [--stage-only]` | yes | stage the package, then force it onto each named connected unit (x8p first); one `Unit:` line per unit; restart (3010) when a unit or the loaded image needs one |
| `unbind` | yes | hand every unit other than the x8p on our driver back to UA's driver (stops with 5 if UA's driver for one is missing), then remove all Avium `apollo.inf` packages, rescan, report |
| `unbind --unit x8p\|twinx` | yes | that unit only back to UA's driver; our package stays (`revert-unit.ps1`). Run `idle-wait --units <it>` first |
| `defaults save <file>` / `defaults restore <file> [--passes N]` | no | the default endpoints, through IPolicyConfig |
| `idle-wait <seconds> [--ask] [--units U]` | no | wait until nothing uses the named units |
| `quit-ua [--units U]` | no | close UA's apps, only while a named unit is on UA's driver and no unit that stays is |
| `regdefaults <defaults.json>` | yes | write the Parameters values and subkeys |
| `cert remove-local <local-cert.txt>` | yes | remove the per-PC certificates listed there (Root, TrustedPublisher, My with its key), by thumbprint |
| `cert add <cer> [--expect <sha1>] [--record <file>]` / `cert remove <cer> [--record <file>]` | yes | a certificate file (installs before Nov 16 2026); `remove --record` touches only the stores the record lists |
| `connect` | asks | the Start-menu shortcut |

`installer\helper\ioctl_check.cpp` compiles against the packaged driver's own `apollo_ioctl.h` and fails the build if
the IOCTL codes, struct sizes or offsets the helper reads ever change. `installer\helper\test_units.cpp` tests the
unit logic in `units.cpp` (which unit is which, `--units` lists, quit-ua with targets, the uninstall hand-back, the
one-image rule, the loaded-image comparison) and `defaults_plan.cpp` (defaults.json, plus the release's own file)
without hardware; `helper\build.cmd` runs it and fails on any failed check.

## Building

### A public release: `installer\release.cmd`

From a plain command prompt, with the EWDK ISO mounted (default `D:`):

```
installer\release.cmd [release folder] [EWDK drive]
installer\release.cmd C:\apollo-release D:
```

1. Copies `driver include app asio tools dsp installer third_party docs` into `<release folder>\src` (default
   `C:\apollo-release`, refused under `C:\Users` or inside the source tree). Never copied: build outputs, logs,
   per-PC captures (`*.defaults.json`, `*.used`, `*.aggconfig.bin`), `tools\bin`, and `builds`, `reference`,
   `ua-files`, `plugins`.
2. Copies `installer\tools\inno-6.7.4` there (nothing is downloaded).
3. Puts `release-nosign.targets` there as `Directory.Build.targets`: the WDK test-signs nothing in that tree.
4. Runs `tools\build-all.cmd` there (every offline suite), then `installer\build.cmd <that tree> <EWDK> release`.
5. Copies `ApolloSetup-<DriverVer>.exe` and `.sha256` to `installer\out\release\` here.

Binaries embed their PDB paths; built in the release folder they name it, never a user folder. The leak scan
(`scan-release.ps1`) then reads every byte of the payload and of Setup, as ASCII and UTF-16, for any path into the
Users folder of drive C, the profile path, the user name as a path part, the PC name, the WDK's developer test certificate name, and the unit
keys and Universal Audio device instance IDs this PC has seen (read from the registry at run time), and fails the
release build on any hit. Any build (release or not) fails if a payload file or Setup carries a signature.

### A test build: `installer\build.cmd`

```
installer\build.cmd [build folder] [EWDK drive] [release]
```

The build folder is a built source tree (`driver\x64\Release\apollo\`, `tools\*\`, `asio\bin\`, `include\`; the
default is this tree, as `tools\build-all.cmd` left it) or a frozen build (`package\`, `bin\`, `src\include\`). The
app comes from `app\bin\` (exe + `ui\`). Nothing outside `installer\` is built or written.

1. Inno Setup **6.7.4** in `installer\tools\inno-6.7.4\` (fetched by `get-inno.ps1` if missing: the file
   jrsoftware.org links to, checked against the pinned SHA-256 from jrsoftware's signed `.issig` and an Authenticode
   signature by "Pyrsys B.V.", installed per-user and portable).
2. The EWDK environment (`SetupBuildEnv.cmd amd64`).
3. `apollo-helper.exe` at /W4 /WX with CFG, /sdl, System32-only DLL loading and only the PDB's file name embedded,
   after the offline test (`test_units`, including the release's `defaults.json`).
4. The payload in `out\stage\` (`stage.ps1`): the INF's provider is "Avium", it names `apollo.cat`, it lists exactly
   the x8p's and the Twin X Gen 1's IDs and no other UA ID; `apollo.sys` loses any signature (`signtool remove /s`),
   `apollo.cat` is made again by Inf2Cat (`/os:10_x64,10_NI_X64 /uselocaltime`), both are checked `NotSigned`, and no
   staged file may carry any signature or be a certificate file; `payload-manifest.sha256`. The EWDK's `signtool`
   (x64) and `Inf2Cat` (x86) come from the EWDK environment, else the newest kit version under the EWDK drive.
5. `ApolloSetup.iss` compiled (unsigned). Log: `out\iscc-<DriverVer>.log`; Inno's list of compiled files:
   `out\ApolloSetup-<DriverVer>-files.txt`.
6. `verify.ps1`: the files compiled into Setup are exactly the payload manifest, same SHA-256 each; Setup carries no
   signature. Prints the size and SHA-256 and writes `<exe>.sha256`.
7. `scan-release.ps1` on the payload and Setup: fatal in release mode, a warning otherwise (a test build from a tree
   under `C:\Users` names it; never publish one).

Output: `installer\out\ApolloSetup-<DriverVer>.exe`, where DriverVer is the INF's.

**New Inno Setup version:** change the version, URL and SHA-256 at the top of `get-inno.ps1` (take the SHA-256 from
`https://files.jrsoftware.org/is/6/innosetup-<ver>.exe.issig`) and `INNO_VER` in `build.cmd` and `release.cmd`.

## Testing it safely

Kernel drivers cannot load in Windows Sandbox, and no test of this installer should run on a development PC with
live units (the uninstaller unbinds them and removes every Avium `apollo.inf` package).

1. **Static (every build):** the helper (/W4 /WX), its offline tests, the IOCTL layout, the INF's IDs, the unsigned
   payload, the compile, the file list against the manifest, the leak scan.
2. **Windows Sandbox: wizard, layout, signing, uninstall.** Copy the exe into the folder your Sandbox `.wsb` maps
   read-only, open the sandbox, and in it run:

   ```
   Desktop\installers\ApolloSetup-<DriverVer>.exe /SANDBOXTEST=1 /LOG="%USERPROFILE%\Desktop\setup.log"
   ```

   `/SANDBOXTEST=1` continues past the Secure Boot stop, leaves test signing alone, runs the per-PC signing (the
   sandbox has its own certificate stores) and only stages the driver (there is no unit there anyway). Check: the
   pages and texts; `C:\Program Files\Avium\Apollo` against `payload-manifest.sha256` (all but `apollo.sys` and
   `apollo.cat`, which are signed now); `state\local-cert.txt`; `certlm.msc` (the "Apollo driver (signed on ...)"
   certificate in Trusted Root and Trusted Publishers, none in Personal); `Get-AuthenticodeSignature` on the two
   driver files (Valid); `pnputil /enum-drivers` (an "Avium" apollo.inf); `reg query
   HKLM\SYSTEM\CurrentControlSet\Services\apollo\Parameters /s`; the ASIO keys under `HKLM\SOFTWARE\ASIO`; the
   Start-menu shortcuts. Then uninstall from Settings and check that all of it is gone, the certificate included.
3. **A spare test PC with the units** (Secure Boot off, BitLocker recovery key at hand):
   - fresh install with both units on UA's driver, both ticked: restart, "Test Mode", `apollo-helper check` (both
     `on_apollo_driver: true`, Twin X `mode: 2`), Apollo Control, the default devices unchanged, the strips;
   - one unit unticked (UA's apps must stay open, that unit unchanged);
   - install with no unit connected, then "Connect Apollo units";
   - Setup run again over the install (repair): a second thumbprint in `local-cert.txt`, both units on the new
     package, and the restart the shared image asks for;
   - uninstall: both units back on UA's driver after a power cycle, no "Apollo driver (signed on ...)" certificate
     left in `certlm.msc`; and uninstall with UA's driver removed (it must stop with nothing removed).

What has **not** been exercised on hardware yet: every path that changes a real unit (bind of either or both units,
unbind, unbind --unit, quit-ua, the loaded-image check after a two-unit update, IPolicyConfig restore, idle-wait
against running streams), the BCD and BitLocker paths, and the wizard on a PC with units. `sign-local.ps1` itself
signed an unsigned package that the kernel loaded on both units in test mode (Nov 16 2026).

## Files in this folder

| File | Role |
|---|---|
| `build.cmd` | the installer build (above) |
| `release.cmd` | a public release, built in a neutral folder |
| `release-nosign.targets` | becomes `Directory.Build.targets` in the release tree: no WDK test signing there |
| `ApolloSetup.iss` | the Inno Setup script: wizard, install and uninstall logic |
| `sign-local.ps1` | per-PC signing, run by Setup (shipped) |
| `defaults.json` | driver registry values for the release |
| `stage.ps1` | payload, unsigned driver package, checks, manifest |
| `verify.ps1` | compiled Setup against the manifest, unsigned |
| `scan-release.ps1` | the leak scan |
| `get-inno.ps1` | fetch and verify the pinned Inno Setup |
| `RELEASE-NOTES.md` | the text for the GitHub release (shipped) |
| `helper\` | apollo-helper sources and `build.cmd` |
| `tools\` | downloaded Inno Setup (not source) |
| `out\` | build output (not source); `out\release\` holds what release.cmd made |
