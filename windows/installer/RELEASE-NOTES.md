# Apollo open driver for Windows: test release

An open-source Windows driver stack for two Universal Audio Apollo interfaces on Thunderbolt: the **Apollo x8p**
and the **Apollo Twin X Gen 1**. It replaces UA's Windows driver on the units you choose and gives you Windows sound
devices, an ASIO driver and Apollo Control, a console app for the mixer and monitor section. It is a port of the
open-apollo Linux driver and mixer engine, under the GPL.

This is a **test build**. The driver is not signed by Microsoft, so Windows loads it only in test mode, with Secure
Boot off. Read "Risks" before you install.

## Who it is for

Owners of an Apollo x8p or an Apollo Twin X Gen 1 (or both) connected over Thunderbolt, on Windows 11, who accept
running a test-mode kernel driver on their PC. If your Apollo is any other model, or a Gen 2 unit, this release is
not for you: Setup leaves those on UA's driver.

## What works

* **Windows audio:** every output pair and every mic/line input of each unit as its own Windows sound device, plus
  the monitor mix. Both units can also run as one device: Apollo Control combines them, and Windows then shows one
  set of "(Apollo)" devices for both.
* **ASIO:** "Apollo x8p (Avium)" for the x8p, and "Apollo (Avium)" for both units as one ASIO device.
* **Apollo Control:** the mixer, monitor section, cue mixes, preamps (gain, 48V, pad, low cut, phase, link) and
  outputs of both units, sessions, and undo.
* **Input strips** on the mic/line inputs, using modules Setup copies from your own UAD software install (see
  below).

## What does not work in this release

* **UAD plug-ins.** Hosting UAD plug-ins on the Apollo DSPs needs a developer setup: a Python environment and plug-in
  maps built from your own UAD install (see docs/PLUGINS.md in the source). The installer includes none of that, so
  Apollo Control shows plug-ins as unavailable.
* **Gen 2 units and other Apollo models.** They are never switched and stay on UA's driver.
* **Secure Boot.** The driver cannot run with Secure Boot on (no Microsoft signature).

Nothing made by Universal Audio is in this release. The input strip modules are read from the UAD software already
installed on your PC, at install time. If UAD is not installed, everything else works and the mic strips stay off.

## Requirements

* Windows 11, version 22H2 or later, 64-bit (x64). Arm PCs are not supported.
* Secure Boot turned off in your PC's firmware (UEFI) settings. Setup never changes this setting.
* Test signing turned on. Setup asks for your consent and turns it on; it takes effect after one restart, and Windows
  then shows "Test Mode" in the corner of the desktop.
* An administrator account.
* An Apollo x8p and/or Apollo Twin X Gen 1 on Thunderbolt.
* UA's UAD software installed, for the input strips and for going back to UA's driver later.

## Risks, in plain words

* **Secure Boot off.** Secure Boot protects the start of Windows against tampered boot software. With it off, that
  protection is gone for as long as it stays off.
* **Test mode.** With test signing on, Windows loads any test-signed driver, not only this one. Only install test
  drivers you trust.
* **A driver not signed by Microsoft.** This is a kernel driver: a bug in it can crash Windows (a blue screen) or
  stop audio. Microsoft has not reviewed it.
* **Signed on your own PC.** The download contains no signature from anyone. Setup makes a certificate that exists
  only on your PC ("Apollo driver (signed on YOUR PC NAME)"), signs the driver with it, trusts it on your PC only, and
  deletes its private key right away, so nothing else can ever be signed with it. Uninstall removes it again.
* **BitLocker.** Turning test signing on changes a boot setting that BitLocker checks. If BitLocker protects your
  Windows drive, the next restart may ask for the 48-digit recovery key. Have it ready before you start (your
  Microsoft account has it at https://aka.ms/myrecoverykey). Setup offers to suspend BitLocker for that one restart.
* **Unsigned installer.** Windows SmartScreen may warn that the installer is from an unknown publisher. Check the
  SHA-256 (below) before you run it.

## Install

1. Turn Secure Boot off in your PC's firmware settings (your PC maker's manual says how), and have your BitLocker
   recovery key at hand.
2. Download `ApolloSetup-<version>.exe` and its `.sha256` file. In PowerShell, run
   `Get-FileHash .\ApolloSetup-<version>.exe` and compare the hash with the one in the `.sha256` file.
3. Connect your Apollo units over Thunderbolt, switch them on, and close every program that plays or records through
   them: DAWs, music players, browsers playing sound, UA Console.
4. Run Setup. It explains each step before doing it:
   * BitLocker (only if it applies): suspend it for the next restart (recommended).
   * Test signing: tick the box to allow it.
   * Driver signing: what Setup does to sign the driver on your PC.
   * Apollo units: tick the units to switch to this driver (both are ticked by default).
5. Click Install. Setup copies the files, signs the driver on your PC, writes the driver settings, switches the
   ticked units (it closes UA's apps only if no unit that stays on UA's driver still needs them) and puts your default
   sound devices back. The Results page says what happened to each unit.
6. Restart Windows when Setup asks. After the restart Windows shows "Test Mode". Open Apollo Control from the Start
   menu (Avium Apollo).

If a unit was not connected during Setup, connect it later and run **Connect Apollo units** from the Start menu.
Windows gives a newly connected unit to UA's driver whenever UA's software is installed, so use this shortcut each
time you connect a unit for the first time.

The first time the input strips start, a unit may need one power cycle (off for ten seconds, then on).

## Uninstall and go back to UA's driver

1. Close every program that uses your Apollo units.
2. Open Settings, Apps, Installed apps, find "Apollo open driver" and choose Uninstall (or use **Uninstall Apollo
   driver** in the Start menu).
3. The uninstaller moves your units back to UA's driver, removes the Apollo driver, the ASIO driver, the certificate
   Setup made, the program files and the input strip modules, and puts your default sound devices back.
4. It asks whether to turn test signing off. Choose Yes to leave test mode (it offers to suspend BitLocker for that
   restart first), or No to keep it.
5. Power-cycle each unit (off for ten seconds, then on) so UA's software starts it fresh. Restart Windows if asked.
6. If you turned test signing off, you can turn Secure Boot back on in the firmware settings after that restart.

UA's driver must be installed (UA Connect or the UAD software) for the units to go back to it. Without it, an x8p is
left with no driver until you install UA's software, and with a Twin X on this driver the uninstaller stops before
removing anything and says so: install UA's software first, then uninstall again.

Your Apollo Control settings stay in `%APPDATA%\Avium\ApolloControl`; delete that folder if you do not want them.

To move a single unit back to UA's driver and keep the other on this driver, run this in an administrator command
prompt, then power-cycle that unit:

```
"C:\Program Files\Avium\Apollo\apollo-helper.exe" unbind --unit twinx
```

(use `--unit x8p` for the x8p).

## Files

* `ApolloSetup-<version>.exe`: the installer (unsigned).
* `ApolloSetup-<version>.exe.sha256`: its SHA-256.

The source code, the build scripts and the installer's full documentation (`installer/README.md`) are in the
repository this release belongs to. The driver, Apollo Control and the tools are under GPL-2.0, the ASIO driver under
GPL-3.0 (Steinberg's ASIO SDK, whose licence is installed with it).
