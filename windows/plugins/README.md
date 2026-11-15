# apollo-win/plugins: open-apollo's plug-in mapper and module store, on Windows

User-mode only (docs/PLUGINS.md, gate P0 "mapper on Windows"). No driver code, no device access,
nothing elevated. Built Nov 14 2026; the generic (UA-made) family ported to UA's 11.4 framework
Nov 15 2026 (section below).

## Never distribute what this folder makes

`store\`, `bundles\`, `work\` and `downloads\` hold or are derived from Universal Audio's files
(DSP modules cut from the user's own Powered plug-in DLLs, maps made by running those DLLs in an
emulator). They stay on this PC. They must never go into the repo, a build folder, the installer
(`installer\stage.ps1` copies an explicit list and does not touch this folder), a log or anything
shared. `specs\` is open-apollo-panels (CC0), still kept local. Only `tools\` is source code.

## Nothing here talks to UA

- No network access to any Universal Audio domain or service; no UA program or service is
  started; UAD2SDK.dll and UAD2DriverClient.dll are never loaded or called natively (the mapper
  reads UAD2SDK.dll's bytes and runs the parts a plug-in's constructor needs inside the emulator,
  as it does the plug-in itself).
- DLLs are only **read as bytes**: the store builder scans them for Bill records; the mapper
  parses them with pefile and runs plug-in code inside **Unicorn** (an x86-64 CPU emulator, in
  Python). Every Win32 import is a Python trap (`executor/offline_host.py`); the emulated
  `LoadLibraryExW` hands out virtual handles for kernel32 only; no `ctypes` load of any UA DLL,
  no sockets. Emulated code cannot reach the network.
- The only download is open-apollo's pinned panel specs (GitHub), checked against the pinned
  SHA-256 before use. (The Python wheels came from PyPI before this rule was set; see below.)

## Layout

| Path | What |
|---|---|
| `.venv\` | Python 3.13.1 venv: numpy 2.5.4, unicorn 2.1.4, capstone 5.0.9, pefile 2024.8.26 (upstream's `requirements.txt`) |
| `tools\open-apollo\` | Vendored open-apollo 2.1.0 code (GPL-2.0, `LICENSE`, `NOTICE.md`), same relative layout as upstream so its paths still resolve: `tools\plugin-mapper\` (patched, below), `mixer-engine\` (unmodified subset: the native carrier, providers, composer and what they import, plus `device_maps\native_known_good.json` and two test files), `tools\plugin-qualify.py` (unmodified, its offline panel check), `configs\plugin-panels.conf` (the pin), `console-rs\crates\oa-assets\data\firmware-pins.json` (strip module pins) |
| `tools\oa_assets_dsp.py` | Python port of `oa-assets dsp` (Rust): builds the module store |
| `tools\install_panels.py` | Python port of `scripts/install-panels.sh` |
| `tools\offline_check.py` | Upstream's offline recipe checks per bundle, compared with upstream's reference data (plus the upstream-evidence checks below) |
| `tools\bench.py` | What gate P1 needs from one bundle |
| `tools\run_upstream_tests.py` | Runs upstream's synthetic mapper/carrier tests without pytest |
| `tools\winshim_selftest.py` | Self-test of the Windows shims |
| `downloads\panels-2026.10.20\` | The panel-specs tarball as downloaded |
| `specs\` | The 224 installed panel specs (`PluginSpec_*.json`, `.source` = the SHA-256) |
| `store\` | Module store: `manifest.json`, `modules\<sha256>.bill`, `dsp\<id>.bill` (pinned strip modules) |
| `bundles\` | Mapper output: `index.json`, `progress.json`, `<slug>-<layout>-runtime-bundle.json`, `metadata\`; `offline-check.json` (every bundle), `offline-check-generic.json` (the generic family with `--defaults`), `offline-check-sphere.json` (the four Sphere bundles with `--defaults`), `p1-candidates.json` (the ranking below) |
| `p1\` | `authorized.txt` (the x8p's licence answer, Nov 15), `renderids.txt`, gate P1 recipes (`tools\plugin-host\p1_recipe.py`), `dsp-owned.txt` (words a hardware run saw the DSP write itself, e.g. the UA 1176LN Legacy's meter word: readbacks skip them; read by p1_recipe.py and Apollo Control) |
| `work\` | Mapper scratch (failed plug-ins keep their logs here); delete freely |

## Commands (from `apollo-win\plugins`)

```bat
rem 1. Panel specs (once): download, check the pinned SHA-256, install
.venv\Scripts\python.exe -I tools\install_panels.py --conf tools\open-apollo\configs\plugin-panels.conf --downloads downloads --dest specs

rem 2. Module store from the UAD install (reads bytes only), then the pinned strip modules
.venv\Scripts\python.exe -I -X utf8 tools\oa_assets_dsp.py dsp --from C:\ --out store --pins tools\open-apollo\console-rs\crates\oa-assets\data\firmware-pins.json
.venv\Scripts\python.exe -I -X utf8 tools\oa_assets_dsp.py strips --store store --pins tools\open-apollo\console-rs\crates\oa-assets\data\firmware-pins.json --out store\dsp

rem 3. Map (run from the mapper folder: upstream uses python -m plugin_mapper). Pass ABSOLUTE paths:
rem    a bundle records its metadata path as given, and offline_check --defaults / the emulator
rem    reopen it from another folder (a relative --out made bundles whose defaults check fails).
cd tools\open-apollo\tools\plugin-mapper
set P=C:\Users\aaron\Desktop\Avium\apollo-win\plugins
..\..\..\..\.venv\Scripts\python.exe -X utf8 -m plugin_mapper --from C:\ --out %P%\bundles --work %P%\work --specs %P%\specs --module-store %P%\store --authorized %P%\p1\authorized.txt --workers 16
rem    add --only "UAD API Preamp" (repeatable) for one plug-in (--only ignores --authorized);
rem    --retry-failures to try failures again; --force to rebuild a current map; --all for plug-ins
rem    not on the known-good list (the 1176LN/1176SE/LA-2A/Pultec-Pro Legacy, CS-1, ... need it or --only)
cd ..\..\..\..

rem 4. Offline checks, bench
cd tools
..\.venv\Scripts\python.exe -X utf8 offline_check.py --reference ..\..\reference\open-apollo --json ..\bundles\offline-check.json
rem    --defaults also starts each plug-in in the emulator (slow: the two Chambers take ~15 min each)
..\.venv\Scripts\python.exe -X utf8 bench.py "UAD AMS RMX16" --layout mono
..\.venv\Scripts\python.exe -X utf8 run_upstream_tests.py
```

`-X utf8` is required (the mapper checks it): Linux reads text as UTF-8, Windows defaults to the
ANSI code page, which would silently misread the panel specs. `--authorized all` maps without a
licence answer; `--authorized FILE` (one name per line) maps only what the x8p authorizes:
`p1\authorized.txt` is the x8p's own answer on our driver (Nov 15 2026, 112 names). `auto` asks
upstream's daemon, which is not here.

## What was changed for Windows

All patched files carry a "Modified by Avium, 2026-11-14" note (2026-11-15 for the 11.4 framework
port and the Sphere builds, sections below). Linux behaviour is unchanged: every shim calls the
original Linux function on Linux.

| Upstream | Linux-only | Windows | Where |
|---|---|---|---|
| `plugin_mapper/__main__.py:455,533` | `fcntl.flock(LOCK_EX\|LOCK_NB)` on `.build.lock` | `winshim.lock_nb`: `msvcrt.locking` byte 0, non-blocking | `__main__.py` |
| `discovery/run_catalog.py:33` | `fcntl.flock` on `.runner.lock` | `winshim.lock_nb` | `discovery/run_catalog.py` |
| `execution/run_catalog.py:128` | `fcntl.flock(LOCK_EX)` (blocking) on the cache lock | `with ..., winshim.held(handle)`: retried until granted, released before the handle closes | `execution/run_catalog.py` |
| `discovery/run_catalog.py:65,68` | `Popen(start_new_session=True)`, `os.killpg(pid, SIGKILL)` on timeout | `winshim.new_group()` (`CREATE_NEW_PROCESS_GROUP`) and `winshim.Group`: a Job Object holding the worker, `TerminateJobObject` on timeout (plus `proc.kill()`). A venv `python.exe` is a launcher running the real interpreter as a child: the job holds both | `discovery/run_catalog.py` |
| `__main__.py:480-482` | `multiprocessing.get_context('fork')` (tables) | `winshim.process_context()`: `spawn` | `__main__.py` |
| `__main__.py:36-44` | `XDG_*`, `~/.local/share`, `~/.cache`, `/usr/share`, `/lib/firmware` defaults | `%LOCALAPPDATA%\Avium\ApolloPlugins\{maps,work,specs,store,generated}` (docs/PLUGINS.md 2.6); we pass every path explicitly anyway | `__main__.py` |
| `carrier/descriptor.py:24`, `execution/run_catalog.py:10` | default store `/lib/firmware/universal-audio/plugins/modules` | `%LOCALAPPDATA%\Avium\ApolloPlugins\store\modules` (only when `OA_MODULE_STORE` is unset; the mapper always sets it) | both |
| (implicit) | text I/O is UTF-8 | `winshim.require_utf8()`: refuses to run without UTF-8 mode, sets `PYTHONUTF8=1` for every child | `__main__.py` |
| `__main__.py:93` | `os.replace` of `index.json`/`progress.json` | `winshim.replace`: retried for 2.5 s while a reader holds the file (Windows refuses to replace an open file) | `__main__.py` |
| `execution/run_catalog.py:51` | runtime manifest hashes `*.so*` of unicorn/capstone | also `*.dll`, `*.pyd` (what the Windows wheels ship) | `execution/run_catalog.py` |

New file: `plugin_mapper/winshim.py`. Known remaining difference: JSON files the mapper writes have
CRLF line ends on Windows (Python text mode). The content is identical; every hash of them is
computed and compared on this PC, so nothing depends on it.

The module extractor: upstream builds the store with `oa-assets dsp` (Rust). Building it needs
about 60 crates from crates.io, which this PC may not fetch, so `tools\oa_assets_dsp.py` is a
line-by-line Python port of `dsp.rs` and the helpers it uses (`firmware::all_bill_records`,
`pe.rs` version readers, `plugins/pe.rs` resource walker, `detect.rs`), writing the same
`open-apollo-plugin-dsp/1` manifest (keys sorted, 2-space indent, as serde_json writes it) and the
same `modules\<sha256>.bill` store. Port differences: no Unix file modes (NTFS ACLs inherit), and
with `--from C:\` (the live system rather than a mounted partition) "--out inside the install"
means inside the folders it reads (`Program Files*\Universal Audio`, `Windows`,
`Common Files\VST3`) instead of the whole drive. `strips` is an addition: the Console strip
modules the native carrier needs as `dsp\<id>.bill`, taken from the store only when their SHA-256
is pinned (what `oa-assets firmware` writes without `--accept-unknown`).

## Results, Nov 14 2026 (UAD Powered Plug-Ins 11.5.2.43082)

- **Panel specs**: open-apollo-panels 2026.10.20, 1,007,696 bytes, SHA-256
  `10f24b1db43151ce2ad9066c8874666be5c8e94dd64285e31e3c9839107ac3d0` = the pin; 224 specs.
- **Module store**: 220 plug-in DLLs + UAD2SDK.dll (5 records) + UAD2DriverClient.dll (2): 3,310
  plug-in records, **950 distinct modules**, 18.7 MB; kinds 764/132/44/10 (code, 1, 2, 3),
  architectures 186/381/383 (0, 1, 2), largest 246,068 bytes, no (id, arch) with two versions.
  All 7 Console strip modules match upstream's firmware pins (pinned for UAD 12.0.0.602; the
  11.5.2 bytes are identical). `uad_version` is null: 11.5.2's UA.xml has no `swReleaseName`.
- **Upstream tests** (`run_upstream_tests.py`): 14/14 pass on Windows (mapper cache/index/progress/
  stage, carrier recipe, registry). The fork-only tables test is not runnable under spawn.
- **Shim self-test** (`winshim_selftest.py`): locks refuse a second handle and a second process;
  the Job Object kill ends a worker and its grandchild.
- **Mapping**: see `bundles\index.json` and `bundles\offline-check.json`. On Nov 14 no
  generic-family plug-in mapped (all 96 stopped at `parent constructor anchor not unique 0` or
  `ambiguous primary constructor/PB ... anchor_linked=0`): UA's 11.4 framework, fixed Nov 15
  (next section).
- What maps (20 plug-ins, 39 bundles: 19 mono, 20 stereo; 579 s with 16 workers; log
  `work\map-all.log`), all partner families: vst2 (AMS RMX16, RMX16 Expanded), vst2_bx (ADA
  Flanger, Ampeg B15N/SVT3Pro/SVTVR/SVTVR Classic, ENGL E765 RT, bx_tuner), dspaudioprocessor
  (AMS DMX 15-80 S, Auto-Tune Realtime Access/Advanced, C-Suite C-Vox, Sound Machine Wood Works
  stereo only, as upstream lists it), paramobj (Auto-Tune Realtime X), sonnox (Oxford Inflator,
  Limiter), vstmain (Oxford SuprEsser DS, Oxford Dynamic EQ), vst2_relab (Lexicon 480L). Every
  bundle builds upstream's recipe from the store, and its parameter count and panel result equal
  upstream's measured status (the two panel "fails", Auto-Tune Realtime X ordinal 20 and Oxford
  Dynamic EQ ordinal 101, are upstream's own). The vst2_bx family and Lexicon 480L need staged
  startup (more than one 1016-word page), which gate P1 does not have.
- One page, mono and stereo, known-good, no DSP-owned words: AMS DMX 15-80 S, AMS RMX16,
  RMX16 Expanded, C-Suite C-Vox (latency 0) and **Oxford SuprEsser DS (latency 178, 674 command
  words)**. Their emulator starts and load-time texts work here (`offline_check.py --defaults`).
- Not mapped for other reasons: Sphere Mic Collection and 180 (vstmain pinned to UAD 11.8.3 DLLs;
  mapped Nov 15, last section), RealVerb-Pro (its spec needs step texts from `oa-assets plugins`,
  not ported).

`tools\install_panels.py` does what `install-panels.sh` does: pin from `plugin-panels.conf`,
download into a new folder, SHA-256 before anything reads it, only plain files under one
`open-apollo-panels-<version>/` top, `VERSION` equal to the pin, only `PluginSpec_[a-z0-9_]+.json`
installed, swapped in with one rename.

## The generic (UA-made) family on UA's 11.4 framework, Nov 15 2026

The UA-made plug-ins of UAD 11.5.2 are UA's `11.4.0 09-06-2024` builds. Their framework `PlugIn`
base class has **one 8-byte member less** than the builds upstream mapped, somewhere between
+0x1e8 and +0x338: the base constructor stores the caller's max-table argument (caller stack
+0x88) at **+0x340, not +0x348**, and every member after it, the derived class's included, is 8
bytes lower. Read offline (pefile + capstone, `generate_profiles.Image`) in all **123** PlugIn DLLs
of the install:

| What upstream's code binds | Upstream | 11.4 (all 123 DLLs) |
|---|---|---|
| max-table member (base constructor store; the normalizer reads it) | +0x348 | **+0x340** |
| member pointer the secondary PlugIn interface's descriptor getter dispatches through (`mov rcx,[rcx+X]; jmp [rax+N]`, 16 DLLs) | +0x368 | **+0x360** |
| host interface (base constructor stores the factory return) | +0x100 | same |
| channel counts from constant stack arguments | +0x1d8/+0x1dc | same |
| fs and 1/fs (framework init stores slot 0xf8's `movss xmm0,[rcx+0x18]` there) | +0x1e0/+0x1e4 | same |
| framework init calls parent slots 0x578 then 0x538 | yes | same |
| parent slots 0x4e0 (`[this+0x100]->vt[0x118]`), 0x518/0x520 (`[this+0x100]->vt[0x30]/[0x28]`), 0x180/0x188 (host callback request 4) | | same |
| parent vtable | | 179-182 slots; 0x5a0 is a derived-class slot where present (the 0x518/0x520 composition in 10 DLLs, handled as upstream does) |

The layout is a detected parameter, not a fork: `discovery/stages/generate_profiles.py`
`FRAMEWORK_LAYOUTS` keys the two layouts (`open-apollo`, `uad-11.4`) by the base constructor's
max-table store, which must hit exactly one of them exactly once (`max_table_layout`). The anchor
search, the normalizer and the store check use the detected member; a `uad-11.4` profile carries
`framework_layout` (name, members, the constructor that proved it), and its member-pointer
descriptor must be +0x360. `executor/generic_capture.py` reads `framework_layout` (default:
upstream's +0x348/+0x368, so an upstream profile runs exactly as before). Bundles embed their
profile, so the emulator the daemon and `offline_check --defaults` start uses the same layout.
One more fix, not layout: `discovery/stages/probe_formatter_abi.py` skips a call target outside
the image (a pdata-less slot decoded into data, Avalon VT-737sp) instead of failing on it.
`MAPPER_VERSION` is unchanged, so the partner maps stay current: all 39 partner bundles and the
metadata files are **byte-identical** to Nov 14 (SHA-256 of every file; only `index.json` and
`progress.json` changed). `run_upstream_tests.py`: 14/14 pass.

**What maps now** (`index.json`, 251 bundles: 125 mono, 126 stereo): **106 generic plug-ins, mono
and stereo (212 bundles)**: all 96 that failed on Nov 14 (every known-good generic entry but
RealVerb-Pro, still blocked by its step texts), and 10 authorized ones upstream's known-good list
does not offer (UA 1176LN Legacy, UA 1176SE Legacy, Teletronix LA-2A Legacy, Pultec-Pro Legacy,
CS-1, EMT 250, Lexicon 224, AKG BX 20, Moog Multimode Filter XL, API Vision Channel Strip; the
last two are "blocked" upstream). 99 of the 106 are authorized on the x8p (`p1\authorized.txt`);
the other 7 (Harrison 32C SE, LA-6176, Neve 1084, Precision Channel Strip, Delay Mod, Delay Mod L,
Reflection Engine) were mapped only to prove the port. Mapping 98 took 1748 s with 16 workers (the
two Chambers inflate their .irz: 864 s and 1086 s). One transient failure (`PermissionError`
replacing `execution\...\ledger.json`: Windows held the file; `execution/run_catalog.py`'s `save`
is not shimmed) mapped on retry.

### Checked against upstream, without hardware

Upstream publishes no bundles and no per-plug-in DSP module hashes, so "same DSP as upstream's
verified bundle" cannot be checked directly. `offline_check.py` checks what upstream does publish
(results in `bundles\offline-check-generic.json`: 212 generic bundles, with `--defaults`):

| Check | Result (212 generic bundles) |
|---|---|
| recipe: upstream's `ProfiledCarrierProvider` builds the CREATE (every module from the store by SHA-256, Bill contract, WRITE check, startup replay = shadow) | all 212; one page for 190; 22 need staging (Capitol Chambers, Galaxy Tape Echo, Helios Type 69, Hitsville EQ / EQ Mastering / Reverb Chambers, Manley Massive Passive / MST, SSL E Channel Strip, Culture Vulture, V76 Preamp) |
| defaults: the emulator starts the plug-in and learns every value and text | 212/212 |
| modules all from this DLL; strip modules = firmware pins | 212/212 |
| pinned: the plug-in's own modules whose id has a firmware pin (0eb, 0c2, 0da, 0db, 12b, ...) equal UAD 12.0.0.602's SHA-256 | 190 bundles have one or more: all equal |
| render id = UA's manifest and UA.xml | 198; 14 (7 plug-ins) where UA's own data disagree and the plug-in reports its collection id (API 560 reports 0x853, UA.xml's "API 500 EQ Collection", which the x8p authorizes; the CS-1 family, Neve 1084, Harrison 32C SE) |
| parameters and panel equal upstream's measured status | every known-good one; the panel "fails" are upstream's own (Hitsville EQ, UA 610-A, 610-B) |
| steps: stepped parameters' maxima = the spec's step counts, and the plug-in's own text at the value it holds = the spec's label (the spec is the Console's list, from a newer UAD) | 199 pass; in 64 a control holds another value in that layout (a pad in Line mode, a linked channel's control in a mono insert) and its text names it correctly; fails: the CS-1 family (its stepped controls keep the 32768 domain: the parent constructor fills the domain table outside the span the executor runs), LA-6176 (another step order), Precision K-Stereo (held back upstream) |
| art: the DLL resources upstream's art recipe lists, by SHA-256 | identical for 102 plug-ins (the plug-in release upstream drew, rebuilt on the 11.4 framework); differ for EMT 140, EMT 250, Little Labs VOG, Trident A-Range (updated since) |
| dyn_refs: the known-good DSP-owned (row, word) positions fall inside this recipe's rows | 88 bundles with refs: all inside |
| genuine: UA 1176LN Legacy, the only plug-in whose genuine UA CREATE upstream recorded (driver/tests/test_plugin_upload.py, HARDWARE-SESSIONS.md) | modules 113/0eb/114/016 with the same word 2, length and word 4 (4/4) and 6 private rows (6/6): the DSP side equals the genuine upload |

What our bundles cannot have: a latency for the 10 plug-ins upstream does not list (0 is used,
`latency_source: unknown`), and upstream's live gate (its x4 ran the newer builds).

### First hardware load: ranking (`bundles\p1-candidates.json`)

30 authorized generic plug-ins pass every check mono and stereo, on one page, with no DSP-owned
words and a known-good entry. Ranked by upstream's hardware evidence for the same plug-in, then
risk (DSP-owned words, staging, held values, size):

1. **Precision De-Esser** (render 0x825): UA-made, 7 parameters, 159 command words, 14 rows, 9
   modules, latency 0, no DSP-owned words, no held values; upstream's x4 live gate passed (load,
   every parameter, three controls read back); art 12/12 and both pinned modules identical. The
   smallest clean candidate, already on the gate P1 list. Recipe `p1\precision-de-esser-mono-in1.recipe`.
2. **Cambridge** (0x81e): UA's EQ, upstream **controls verified** on the x4 (load, 40/40 controls,
   40/40 texts, mono and stereo), latency 0, no DSP-owned words, 195 words, 15 rows; art 20/20,
   pinned 3/3. The strongest hardware evidence of any latency-0 candidate; more controls.
3. **API 550A** (0x853): controls verified upstream (12/12), no DSP-owned words, 203 words, 20
   rows; art 10/10, all 5 pinned modules identical; latency 55, so step-p1's record-lag shift
   (55 samples) shows the chain runs.

The examples asked about: **UA 1176LN Legacy** has the only true DSP-side match (genuine CREATE
modules and rows above) and maps cleanly (185 words, no held values), but upstream never offered a
generic bundle for it (its working provider was hand-built), its control mapping is unchecked
against UA's tables and its latency is unknown: a good fourth. **Pultec EQP-1A Legacy** and **Neve
1073 Legacy** pass everything but carry 2 DSP-owned words each (step-p1's readback does not skip
them). **Teletronix LA-2A Legacy** passes everything offline but has no upstream hardware record
and no latency. Recipes for the first three and the 1176LN Legacy are in `p1\`.

## Sphere Mic Collection and Sphere Mic Collection 180 on UAD 11.5.2's builds, Nov 15 2026

Upstream's vstmain profile (`discovery/vstmain_profiles.py`) took only the two DLLs it pins by
SHA-256 (UAD 11.8.3: `bf25d617...`, `c7f45127...`), so both stopped at `original DLL identity
differs`. UAD 11.5.2 ships UA's **2024-09-06 builds** (`5e8e50f6...`, `772d1058...`; PE time stamps
2024-09-06 15:25 and 15:35 UTC; file version 1.0.0 and JUCE v3.2.0 in both). Read offline (pefile,
capstone; the latency in the emulator):

| | Upstream's pinned build | 11.5.2 (both DLLs) |
|---|---|---|
| art: every resource upstream's art recipe lists (5 and 3) | at the recipe's file offsets | byte-identical, 512 bytes (one file block) earlier: the code before it is one block smaller |
| code | startup FUN_180267610 (upstream's note) | every function moved: handler slot 17 -> 0x180267a30 (180: 0x180264470) |
| what the profile binds | `SphereUADProcessor` handler sub-object +0x1da8, >= 19 slots, slot 3 -> the 0xae8-byte AllocInfo builder, the render-id descriptor sub-object +0x1dd8 | same (20 slots); render ids 0x87a / 0x87b = UA's manifest |
| latency the plug-in reports (AEffect.initialDelay after open, resume and startup) | 24 = known-good (Console) | 24 |

The build is a checked parameter, not a fork. `vstmain_profiles.py`: a pinned build takes upstream's
path unchanged; another build of a kind listed in `STRUCTURE` (only `sphere`) continues when
`structural_identity` finds again: JUCE v3.2.0; one RTTI type each for `SphereUADProcessor` and
`MicModUADAudioProcessor`; the `VSTPluginMain` export and one each of the `UAD_CreateIUADPlug` and
`UAD_GetPluginBarHeight` imports the executor binds; exactly one render-id descriptor (the
inventory's) with its `SphereUADProcessor` sub-object 0x30 behind the handler; handler slots 3, 15
and 17 as thunks into real functions (slot 3's = the AllocInfo builder). Everything else was already
derived from the image or is checked at run time by the executor (handler vtable and slots against
the object the original `VSTPluginMain` builds, interface 0x67, parameter count, every constructor
default). The profile records it in `dspap.evidence.build` and carries
`vstmain.latency_check = {effect_field 0x50, expect 24}`: `executor/vstmain_capture.py` (patched)
compares the latency the build itself reports with the known-good one, so the mapper, `offline_check
--defaults` and Apollo Control's planner (`plughelper.py`) refuse a build that reports another. The
Soup and Ocean Way entries stay pinned (their layout facts are not derived). Proof: the profiles of
the two pinned builds installed here (Oxford Dynamic EQ, SuprEsser DS) are byte-identical before and
after; SuprEsser's own AEffect.initialDelay is 178 = its known-good latency (the new check's
calibration); a wrong render id, handler offset, kind or inventory hash is refused. `MAPPER_VERSION`
is unchanged (no other map is rebuilt). Mapped with `--only` both names `--retry-failures` (79 s,
`work\map-sphere.log`); `index.json` now lists **255 bundles (127 mono, 128 stereo)**.

| | Sphere Mic Collection (0x87a) | Sphere Mic Collection 180 (0x87b) |
|---|---|---|
| layouts | mono, stereo | mono, stereo |
| own modules | 1: id 377, arch 2, **2,500 code words** (+ the 5 pinned strip modules) | 1: id 379, arch 2, **3,062 code words** |
| DSP 1 beside the 8 strips (15,390 code words free) | fits, 12,890 left | fits, 12,328 left |
| own rows | 9: 1,014 dwords pool 1, 256 pool 2 | 9: 1,554 pool 1, 512 pool 2 |
| startup | 783 (mono) / 798 (stereo) command words: **one page** | 1,063 / 1,078: **two pages (staged)** |
| latency, load units | 24 samples, 900 | 24, 1,190 |
| parameters | 32: 20 stepped, 12 continuous | 32 (Mic Pan and Stereo Width in place of Mic Mix and Mic2 Align) |
| DSP-owned words | meters at chain row 18 words 0, 1, 4 (panel spec), skipped by readbacks | same |
| `offline_check --defaults` (`offline-check-sphere.json`) | every check passes but panel, which is upstream's own result (no panel control for ordinal 25, Meter Zoom) | every check but panel (upstream's own: ordinals 24, 25, 29) and recipe: needs staging (gate P1 tests one page) |

Every value text of all 20 stepped parameters (the plug-in's own formatter, every value, in the
emulator) equals the Console's step list in the spec. Mic models (Mic1 Type and Mic2 Type, both
plug-ins, 40): LD-47K, LD-49K, LD-67, LD-67 NOS, LD-87 Vintage, LD-87 Modern, LD-87 TK, LD-103,
LD-12, LD-251, LD-800, LD-37A, LD-37P, LD-BV1, LD-414 Brass, LD-414 Nylon, LD-414 US, LD-414 T2,
LD-563, LD-017T, SD-451, SD-416, RB-4038, RB-77DX Satin, RB-77DX Umber, RB-121, RB-160, DN-57, DN-7,
DN-20, DN-409N, DN-409U, DN-421N, DN-421S, DN-421B, DN-12A, DN-12E, Sphere Linear, Sphere Diffuse,
Sphere Direct. Patterns: Omni, Sub-Omni, Wide-Card, Sub-Card, Cardioid, Super-Card, Hyper-Card,
Sub-8, Figure-8. Source Mic: NONE, DLX, LX, L22 (the 180: NONE, DLX, L22); default NONE.

**Layouts.** The plug-in's DSP descriptor is 2-in/2-out in both layouts and its startup words are the
same word for word (639; the 180: 895): the layout is the carrier's channel count only. Stereo is
what the Console uses for a Sphere on a linked pair: upstream's `StereoProfiledCarrierProvider`
writes 2 in / 2 out to chain rows 0, 1 and the plug-in's root row 10 and feeds inputs 1/2 (synch
in1, in1 + 1), so the modeler gets both L22 capsules (front on input 1, rear on input 2 as the
L22's breakout is normally wired; Swap Channels if it is the other way round). Upstream's live gate
passed both layouts on its x4 (`native_known_good.json`: stereo = a linked pair). Mono gets input 1
only (one capsule).
Recipes: `p1\sphere-mic-collection-stereo-in1.recipe` (one page, effect `delay`: the record lag of
input 1 shifts by 24 samples) and `p1\sphere-mic-collection-180-stereo-in1.recipe` (2 pages,
staged).

Not proven here: no hardware record for these builds (upstream's live gate ran the 11.8.3 ones on
an x4); the stereo chain needs apollo.sys with FEAT_STEREO; the 180's staged startup is outside gate
P1's one-page tests.
