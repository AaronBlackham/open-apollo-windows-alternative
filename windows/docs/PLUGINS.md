# UAD plug-ins on the Apollo DSPs: how open-apollo hosts them, and the port to apollo.sys

Status (Nov 14 2026, late): **P0 code built, not yet run on hardware.** Kernel: `driver/plugin.c` (engine v2,
board query, ledger, module store `System32\drivers\ApolloModules`, INSTANCE CREATE/DESTROY/SERVICE, quarantine)
and `driver/plugin_plan.h` (pure parts, `tools/plugin-plan-test`); IOCTL block 0x40-0x43 in
`include/apollo_plugin.h` (interface version 8), gated by `Parameters\PluginHost`. Host: `tools/plugin-host`
(`p1_recipe.py` from a bundle, `apollo-plug.exe`); gate script `tools/phase1/step-p1.ps1`. Deviations from the
plan below: the strips keep their fixed addresses and the ledger books their whole room instead (D6 unchanged,
strip-plan-test untouched); the P1 recipe keeps the plain strip's record delay (200) so the record lag shifts by
the plug-in's latency. P0 review fixes: the strip DSP's ledger keeps the Mic strip's scratch floor (pools 1-3 at
least 0x1C0 / 0x80 / 0); F_START needs F_CHAIN; nothing is placed on the strip DSP before its strip pass is over
(Live, Off or Failed) and the 5 strip module ids are reserved there; SERVICE refuses an unrouted instance; at most
127 private rows; `step-p1.ps1` puts `PluginHost` back on every exit (`-KeepOn` keeps it). Known limit, left as is:
a plug-in call waits for StripLock at most 10 s, but its DevLock waits are unbounded (as every IOCTL's but the
3 s ones in ioctl.c), so a call that arrives during a long bring-up (ACEFACE, DSP-ready waits) waits for it to end.

Nov 15 2026, night (warmplug): **plug-ins after a warm start, and the chains' latency in the DAW, built with offline
tests, not run on hardware.** After an x8p warm start that adopted its strips (`docs/STRIPS.md` 12), DSP 1 takes plug-ins as
after a cold start: `StripWarmPlugins` (default 1) replaces `StripWarmRebuild` as the gate, the anchored record is the
proof that only this driver touched the DSPs since power-up, the first acknowledged upload proves the warm session
(session byte 0x07, no ring connect; DSP 0 already answers the licence query in that state on hardware), and an unproven
first step quarantines DSP 1 (a power cycle, as before). Apollo Control restores the saved inserts once the driver owns
DSP 1 (no cold-start gate); otherwise the insert waits with the driver's reason (caps `Dsp[].Flags` 15:8,
`APOLLO_PLUGIN_WHY_*`) and "power-cycle the x8p". Op LATENCY (9, FEAT_LATENCY): the host tells the driver each routed
chain's record path; whatever goes over the plain strip's 200 samples is added to the unit's reported input latency
(`docs/ASIO.md`, Latency). Features `FEAT_WARM` 0x800, `FEAT_LATENCY` 0x1000; additive to interface version 8.

Nov 15 2026, evening: **[Ins] tracking back end, built with offline tests, not run on hardware**
(docs/apollo-control-units.md 1.9): chains of up to four inserts per input, linked pair or aux bus (the host's C++
port of upstream's `compose()`, identical line for line to upstream's own on 24 real chains of 2-4 inserts and five
synthetic goldens; add / remove / move / bypass rebuild the chain, staged beside the running one when DSP 1 has room);
UAD REC / MON per input (the record tap, a live SERVICE flip or a rebuild, P4's REC/MON half); DSP room and a cycle
budget (DSP 1's load word) per chain; the Unison lease in the driver (P8's kernel half: CREATE `F_UNISON`, ops
BEGIN_UNISON / ACTIVATE_UNISON / UNISON_PARAMS, the analog image capture, write guard and restore, `driver/unison_plan.h`)
and the host's Unison slot, waiting for a Unison map that no mapper builds yet; inserts on the two aux returns
(experimental, `Parameters\AuxInserts`). The retained strips' delay IOCTL (0x43, group monitor alignment) is still
not built: a chain aligns its own record path to the plain strips' 200 samples.

Nov 15 2026, afternoon: **P7, plug-ins on the Twin X Gen 1 Duo, built with offline tests, not run on hardware**
(section 4.1c): apollo.sys hosts on the Twin X too (FEAT_UNITS), Apollo Control keeps plug-in state, licence and DSP
room per unit, `apollo-plug --unit twinx` and `step-p1.ps1 -Unit twinx` run gate P1 on Twin X input 1.

Nov 15 2026, later: **plug-ins in Apollo Control (section 5), built with offline tests, not run on hardware**: a
Plug-ins tab and an insert slot per x8p strip, a picker of the mapped plug-ins the x8p licenses, a generic parameter
panel planned by the plug-in's own setter, inserts saved per unit and input and put back once the strips are Live,
stereo inserts on a linked pair (driver FEAT_STEREO) and the plug-in calls limited to elevated tools and Apollo
Control's own executable (driver FEAT_CALLER, `PluginHostImage`). Gate P1 had passed on the x8p that morning: Ampeg
SVTVR Classic, Lexicon 480L, Precision De-Esser and UA 1176LN Legacy loaded on DSP 1 input 1, processed audio and
unloaded cleanly.

Nov 15 2026: gate P1 licence step passed on the x8p (109 authorized). Of the plug-ins our mapper maps, only Lexicon
480L and Ampeg SVTVR Classic are authorized, and both need a startup over one 1016-word page. **Staged startup is
built (code and offline tests only, not run on hardware), section 4.1b**: CREATE `F_STAGE | F_CHAIN`, SERVICE
pages, op START (8), `FEAT_STAGE`, all additive to interface version 8. Plan text follows. It supersedes the earlier claim in our docs
that UAD plug-ins cannot run on our driver: open-apollo 2.1.0 runs UA's own DSP code, taken from the
user's own UAD install, on the Apollo's SHARCs, after the Apollo's own licence answer admits each
plug-in (`oa:CHANGELOG.md:58-103`). Our input strips (`docs/STRIPS.md`) are the first half of that
path, already working on the x8p.

Conventions

- `oa:` paths are relative to `apollo-win/reference/open-apollo/` (GPL-2.0, read only). Paths without
  `oa:` are ours. **(derived)** = computed from the source; **(inference)** = not stated by it.
- Facts about this PC come from read-only checks on Nov 14 2026 (section 3): directory listings,
  version resources, byte scans that print counts only, and non-admin `apollo-diag --unit x8p info`
  and `strips`. Nothing was written to either unit.

Rules that hold in every phase

1. **Only what the Apollo authorizes.** The driver asks the device for its licence table before every
   placement and places nothing it does not admit. No override, no cache, no demo start, no table in
   any log. Commands `0x000D0000` and `0x000E0000` (UA's licensing block loaders,
   `oa:driver/ua_dsp.c:2901-2905`) are never sent.
2. **No UA bytes leave this PC.** Modules, bundles, tables, panel art and captures stay in their local
   folders (section 2.6). None of it enters the repo, a build folder, the installer or a log. A repo
   guard refuses them, like `oa:tools/check-clean-repo.py` rule `plugin-bundle`
   (`oa:docs/plugin-mapping.md:15-19`).
3. **New licences and demos go through UA's software.** The table lives on the device and only UA's
   software writes it (inference, section 1.6). After a purchase, Aaron puts that unit back on UA's driver
   (`tools\phase1\revert-unit.ps1`), lets UA Connect sync, then rebinds ours and power-cycles.

---

## 0. In short

- **It is feasible on the x8p.** open-apollo's hosting was proven on an Apollo x4 **Gen 1** (type 0x1F,
  `oa:driver/ua_apollo.h:88`; derived: the strip model table that unit runs lists no other x4,
  `oa:driver/ua_strips.c:775`), the same X-series Gen 1 family as the x8p. Our x8p
  already runs the same Mic Input Strip recipe on the same DSP 1 with the same pool geometry tops
  (`docs/STRIPS.md` 2.4). A plug-in insert is that strip transaction extended (section 1.3).
- **Kernel work:** a bigger transaction engine (multi-page payloads, long replies), a module store
  read by hash, a pool ledger, the licence query, and an INSTANCE IOCTL with CREATE, DESTROY, SERVICE,
  then SWAP and delay (section 2.1).
- **User-mode work:** the mapper runs on Windows almost as is (pure Python plus Unicorn; four
  Linux-only calls to patch). The daemon's native backend is reused as a Python helper process that
  Apollo Control supervises (section 2.3).
- **UI:** a generic parameter panel (names, steps and the plug-in's own value texts) in Apollo Control
  is realistic now. Drawn panels from open-apollo's spec runtime are a later, optional port. UA's own
  plug-in editors cannot be hosted (section 2.5).
- **Gate P1** (section 4.1): read the licence table, then one authorized known-good plug-in, mono,
  defaults, on x8p input 1, proven by the DSP's acknowledgements, recipe readback, DSP 1's load word and
  a measured change on input 1, then a clean restore of the plain strip.
- **Biggest risks:** whether the x8p answers the licence query on our driver; DSP 1 room with 8 strips
  plus inserts on one SHARC; bundles built from UAD 11.5.2 where the live gate used another version;
  a bad recipe hanging DSP 1, after which only an x8p power cycle helps (and it drops the Twin X).
  Section 4.3.

---

## 1. How it works upstream

### 1.1 The pieces and where they run

| Piece | Where | What it does | Source |
|---|---|---|---|
| `oa-assets dsp` (Rust) | setup, once | Scans every Powered plug-in DLL (and `UAD2SDK.dll`, `UAD2DriverClient.dll`) for "Bill" records, stores each distinct record once as `modules/<sha256>.bill`, and writes `manifest.json` (DLL, UA.xml entry, render id, VST ids, version, SHA-256, its modules) | `oa:console-rs/crates/oa-assets/src/dsp.rs:1-20`, `:35-43`, `:72-83` |
| `tools/plugin-mapper` (Python + Unicorn) | setup, background | Runs each Powered DLL in an x86-64 emulator against the user's `UAD2SDK.dll`, reads the plug-in's DSP descriptor and startup words, sweeps every control, writes a mono and a stereo runtime bundle | `oa:mixer-engine/NATIVE-HOSTING.md:16-26`, `oa:tools/plugin-mapper/README.md:1-73` |
| Mixer daemon, native backend | always on | Offers mapped, known-good, authorized plug-ins per input; builds the CREATE recipe; plans control changes in emulator workers or from tables; verifies every write by readback; chains, swaps, delay compensation, the licence mirror | `oa:mixer-engine/NATIVE-HOSTING.md:28-158`, `oa:mixer-engine/plugin_native.py`, `plugin_native_chain.py`, `native_carrier.py`, `native_chain_composer.py`, `plugin_delay.py`, `plugin_license.py` |
| Driver `plugin_host=1 input_strips=1` | kernel | DSP ownership, pool ledger, module catalog read from the store by hash, licence query, instance CREATE/DESTROY/SERVICE/SWAP/START, Unison analog lease, strip delays | `oa:driver/ua_plugin_host.h:1-453`, `ua_plugin_instance.c`, `ua_plugin_license.c`, `ua_plugin_catalog.c`, `ua_resource_pool.c`, `ua_strips.c:1053-1290` |
| Console `plugin-ui` (Rust) | desktop | Plug-in windows: UA's HTML GUI if present (macOS installs only), else a panel spec drawn with art regenerated from the user's install, else a generic parameter list | `oa:console-rs/crates/plugin-ui/README.md:1-20`, `RUNTIME-V2.md:1-60` |

### 1.2 From the UAD install to the SHARC

1. **Modules.** Each Powered plug-in DLL carries its DSP code and data as Bill records
   (`oa:console-rs/crates/oa-assets/src/dsp.rs:4-6`). A record is a 20-byte header
   (`"Bill"`, id, w2 = kind 7:0 / flags 23:8 / architecture 31:24, payload bytes, w4) plus payload.
   Code (kind 0) takes `floor(2 * w4 / 3)` words of pool 0; data kinds 1-4 take w4 words of pool 1, 2
   or 3 (kind 4 goes to pool 1) (`oa:driver/ua_plugin_catalog.c:42-74`). The Apollo's DSPs take
   architectures 0 and 2; architecture 1 is refused (`oa:driver/ua_plugin_instance.c:578-608`).
2. **Descriptor.** The mapper constructs the plug-in in the emulator and decodes its DSP descriptor:
   the resources (pointers to Bill records, hashed and checked against the store), the private memory
   rows (pool, dwords, link, flags), scratch, IO, render id, latency fields
   (`oa:tools/plugin-mapper/plugin_mapper/carrier/descriptor.py:8-30`).
3. **Recipe.** The daemon's carrier turns the bundle into a CREATE request: the five strip modules
   plus the plug-in's architecture 0/2 resources by SHA-256, the private rows (chain head, a fresh
   Mic Input Strip, the plug-in's rows linked under the head), WRITE commands (head, Mic strip startup,
   record delay, the plug-in's startup words) and scratch, always `dsp=1`
   (`oa:mixer-engine/native_carrier.py:317-425`, rows at `:53-63`).
4. **CREATE, in the driver, one operation under the host mutex**
   (`oa:driver/ua_plugin_instance.c:7-23`, `:762-962`):
   1. a fresh licence answer for every render id in the chain (section 1.6); nothing else unless
      it admits;
   2. modules read from `/lib/firmware/universal-audio/plugins/modules/<sha256>.bill` by the driver
      itself, checked as UAD2System's IsValid does; a file whose hash is not its name is refused
      (`oa:driver/ua_plugin_host.h:204-221`, `oa:driver/ua_plugin_catalog.c:195-229`);
   3. the last teardown on every DSP proven (fences);
   4. one placement on an owned, unquarantined DSP through the pool ledger, in the answer's epoch
      (`oa:driver/ua_plugin_place.c:239-266`, `oa:driver/ua_resource_pool.c:1-26`: code from the
      bottom of pool 0, data from the top of pools 1-3, first fit, scratch kept free, shared modules
      held once per DSP);
   5. each module not yet resident uploaded and acknowledged: one DMA command
      `{0x0001nnnn (pool 0) or 0x0004nnnn, dest, record with bytes 10-11 cleared}` split only at 4 KiB
      page boundaries, reply `{0x80070004, 0, id, x}` (`oa:driver/ua_plugin_upload.c:24-95`,
      `oa:driver/ua_dsp_xact.c:111-150`);
   6. private rows zero-filled `{0x00080004, addr, 0, words}` and linked by one memspec
      `{0x0015nnnn, (addr, value)...}` (`oa:driver/ua_plugin_instance.c:610-658`,
      `oa:driver/ua_plugin_upload.c:97-160`);
   7. with `F_START`: stop the input's plain strip, send the WRITEs relocated from `row << 24 | word`
      to DSP addresses, SYNCH the chain head, send span and route (section 1.4).
5. **Ownership.** A DSP is used only if its boot word (window +0x1A4) read exactly 3 (model byte
   masked) at a cold start in this device instance, and nothing raw touched it since
   (`oa:driver/ua_plugin_place.c:95-136`, `oa:driver/ua_plugin_host.h:264-285`). Our strips use the
   same rule (`docs/STRIPS.md` 2.3).

### 1.3 Insert transactions versus the strip transaction we already run

Our strip pass sends, per input, 10 zero-fills, one memspec, the header WRITE, the parameter WRITEs,
the SYNCH and span/route words (`docs/STRIPS.md` 2.7). An insert is the same machinery with these
differences:

| Step | Our plain strip (input i, DSP 1) | Insert chain on input i (`oa:`) |
|---|---|---|
| Licence | none (strip modules need none) | fresh board query; every effect in the head admitted (`ua_plugin_license.c:246-284`) |
| Modules | 5 pinned strip modules | the same 5 (already resident, shared) plus the plug-in's own, by hash, any number up to 64 per CREATE (`ua_ioctl.h:1109`) |
| Head row | pool 1, 8 dwords: `0x40000000, 1, 1, -, 0x22, N=1, ptr, 0x846` | pool 1, `6 + 2N` dwords, N = 2..5: Mic carrier `0x846` at word 7, then one `(pointer, render id)` pair per effect at words `6 + 2k` / `7 + 2k`, k = 1..N-1 (`ua_plugin_instance.c:478-538`, `native_carrier.py:393-401`) |
| Mic strip rows | 9 rows, ours, fixed addresses | 9 new rows inside the chain (rows 1-9), same startup words; the record delay (m0 word 16) and record-tap flags (word 4) are owned by the composer (`native_carrier.py:22-31`, `:53-63`) |
| Plug-in rows | none | the descriptor's rows, its root linked to head word `6 + 2k`, the rest linked inside the effect (`native_chain_composer.py:259-265`) |
| Parameters | 3 fixed WRITE blocks | WRITEs checked by the driver: inside their row, never on a linked word; at most 1016 words in the START page, larger startups go through STAGE and later chunks (`ua_plugin_instance.c:438-476`, `ua_ioctl.h:1124-1159`) |
| Start | `{0x001F0004, 0x200000 \| i, hdr, 0}` | `{0x001F0004, 0x20 << 16 \| i, 0, 0}` (stop the plain strip), WRITEs, `{0x001F0004, in2 << 16 \| in1, head, 0}`, span none twice, route `0x1FFFF` (`ua_plugin_instance.c:689-740`) |
| Teardown | release modules, strips off | DESTROY or handle close: stop the chain, re-SYNCH the **retained** plain strip header with its span/route, then release what only this instance held (`ua_strips.c:1101-1129`, `ua_plugin_instance.c:964-1011`) |

The plain strips stay allocated while an insert overrides them ("retained"): the chain may replace an
input only if that input's strip is live on the same DSP in the same epoch and generation
(`oa:driver/ua_strips.c:1059-1098`). So every insert runs on the strip DSP, DSP 1
(`native_carrier.py:416`, `native_chain_composer.py:298`).

### 1.4 Chains and routing

- **Chain = one head, one carrier, up to four effects.** The composer builds one CREATE for a Mic
  carrier and 1-4 effects, in slot order, sharing the head and the resources
  (`oa:mixer-engine/native_chain_composer.py:191-301`). The driver re-derives the head directory from
  the WRITEs, so a client cannot hide a member from the licence check, and refuses SERVICE writes
  that touch the directory: a topology change is always a new admitted CREATE
  (`oa:driver/ua_plugin_instance.c:478-557`).
- **Replacement without dropping audio first.** A new chain is created with `F_STAGE` (initialized,
  not routed), then `SWAP` moves the input to it and the old allocation is retired after the
  candidate's state is published; failures reverse, uncertain I/O quarantines
  (`oa:mixer-engine/NATIVE-HOSTING.md:79-90`, `oa:driver/ua_ioctl.h:1215-1229`,
  `ua_plugin_instance.c:1160-1332`). Neighbouring inserts keep their control state across a rebuild
  (`native_chain_composer.py:270-274`).
- **Routing** is the input's SYNCH on the strip DSP: the chain's output takes the plain strip's place
  for the record channel and for the monitor mix. Stereo is one chain started on two DSP inputs
  (`in2` = the second input; mono uses `0x20`) on a linked pair (`oa:driver/ua_ioctl.h:1145-1147`).
  One routed owner per input (`ua_plugin_instance.c:742-750`, `:847-853`).
- **UAD REC / MON.** Bit `0x4` of word 4 of a block is the record tap: on the last effect the record
  is wet (REC); on the Mic strip's m0 the record is dry while the monitor stays wet (MON). Proven live
  on the x4 (`oa:HARDWARE-SESSIONS.md:450-519`, `oa:mixer-engine/native_chain_composer.py:16-22`).
- **Virtual inputs** take the plain Input Strip `0x83E` as carrier (`oa:mixer-engine/NATIVE-HOSTING.md:106-122`).
  We do not build Virtual strips yet (`docs/STRIPS.md` 1.2).

### 1.5 Delay compensation

- Settings Off / Short / Medium / Medium-Long / Long = 0 / 100 / 200 / 300 / 1000 samples, a global
  input setting, not a plug-in property (`oa:mixer-engine/model_features.py:22-27`,
  `oa:mixer-engine/NATIVE-HOSTING.md:124-142`).
- Effect latency comes from `native_known_good.json` (the executor cannot see it,
  `oa:tools/plugin-mapper/README.md:60-61`), summed once per chain; record delay uses the latency up to
  the record tap, monitor delay the maximum of the synchronous group; a chain over budget publishes
  `InputDelayCompOverloaded` (`oa:mixer-engine/plugin_delay.py:1-67`).
- Two places get the delays: each chain writes its own m0 words 5 (capacity), 8 (monitor) and 16
  (record) (`native_chain_composer.py:250-251`), and the **retained plain strips** get the same words
  through `UA_IOCTL_PLUGIN_DELAY` (0x95), written and read back with the DSP read command
  `{0x000C0004, addr, 12, 5}` / reply `0x8001000E` (`oa:driver/ua_strips.c:1135-1290`,
  `oa:driver/ua_ioctl.h:1249-1266`).
- ASIO latency reporting adds the DSP block (64 frames at 48 kHz) and 8 input frames
  (`oa:mixer-engine/model_features.py:29-36`). Ours (Nov 15, warmplug; `docs/ASIO.md`, Latency): the DSP block, the
  plain strips' 200 and, per op LATENCY, the worst routed chain's record path over 200, while the strips run; the
  host-side pads are our own engine's figures instead.
- Evidence and caveats: retained-strip record delay measured at all five budgets
  (`NATIVE-HOSTING.md:140-142`); a captured chain recipe added exactly 800 samples (Mic strip 1000
  versus the plain strip's 200, `oa:HARDWARE-SESSIONS.md:427-429`); the 1176 Rev A chain measured
  55 samples where the list says 0 (`oa:HARDWARE-SESSIONS.md:515-516`). The list's latencies are
  not all measured (the latency column of `oa:docs/status/PLUGINS.md` is mostly unchecked).

### 1.6 Licences: the board query, the mirror, and what the Apollo enforces

- **The query** (`oa:driver/ua_plugin_host.h:357-376`, `oa:driver/ua_plugin_license.c:98-187`): one
  transaction on **DSP 0**. Response ring: one reply descriptor `{0x80000302, 0, lo, hi}` (770 dwords,
  0xC08 bytes) whose area is prefilled with `0x83000000`. Command ring: `{0x00100002, date, 0, 0}` and
  `{0x00110002, cookie, 0, 0}`. Accept only after the response ring retired the descriptor, with word 0
  `0x80030302` and word 1 echoing the cookie, within 2 s; words 2..769 are the 768-word table, index
  `render id - 0x800`. The date is `(days since 2007-01-01 UTC << 12) + minute of day` from the system
  clock; a clock before 2007 refuses (`ua_plugin_license.c:40-60`). **The query sets the device's
  date**, as UA's own driver does on every query (`oa:driver/ua_ioctl.h:1004-1005`).
- **Decode** (`ua_plugin_license.c:62-96`): `0x81000000`, `0x82000000` authorized; `0x80000000` demo not
  started; `0` expired; `0x83000000` unavailable; anything else a running demo (low 31 bits =
  countdown). Only authorized and running demo admit.
- **Gate:** every CREATE asks again in the same operation and epoch; one fresh answer admits a whole
  chain; nothing is cached across queries, epochs or devices (`ua_plugin_license.c:189-284`).
  `UA_IOCTL_BOARD_QUERY` (0x90) returns the raw table to user space (`ua_plugin_license.c:288-323`).
  Works warm: it needs only a connected unit with live DSP 0 rings, not DSP ownership
  (`oa:driver/ua_plugin_host.c:342-364`).
- **Mirror:** the daemon publishes `/plugins/N/{Authorized,Status,Demoing,DemoDaysRemaining}` from the
  table, render ids via the `oa-assets dsp` manifest, withdraws it on reconnect or rate change, logs
  counts only (`oa:mixer-engine/plugin_license.py:1-36`, `LicenseMirror` at `:244`). The mapper maps
  only authorized plug-ins (`--authorized auto`, `oa:mixer-engine/plugin_authorization.py:1-14`).
- **What the Apollo itself enforces is unknown**: "Whether the DSP firmware itself refuses
  unauthorized code is unknown: this gate is the only enforcement this driver can claim"
  (`oa:driver/ua_plugin_license.c:24-25`). The table survives power cycles without UA's software:
  the x4 answered after a cold start under Linux, 134 authorized, 48 demo not started, 82 expired, 504
  unavailable (`oa:HARDWARE-SESSIONS.md:309-313`). So it is device-resident, written by UA's software
  (inference; the licensing block loaders open-apollo never sends, `oa:driver/ua_dsp.c:2901-2905`).

### 1.7 What the mapper extracts, and what a bundle holds

- **Per DLL** (`oa:tools/plugin-mapper/README.md:48-65`): static inventory (RTTI, descriptor producer),
  discovery of the parameter block, program loader, CRT/host context and formatter ABI (per family:
  generic CHyp/JUCE found from the DLL itself; DSPAudioProcessor, VST-main, ParamObj and Sonnox pinned
  to UAD 11.8.3 addresses; VST2 IUADPlug, bx, relab), then execution: construct, load Console defaults,
  read the descriptor, sweep every control at 0 / middle / maximum and check each restore puts the
  words back. Every plug-in runs twice, answering the channel-count query as a mono and as a stereo
  host (`oa:mixer-engine/NATIVE-HOSTING.md:18-26`).
- **Inputs:** the DLLs and `UAD2SDK.dll`, the module store (to verify descriptor resources by hash),
  the panel specs from the pinned open-apollo-panels release (parameter names, steps, defaults; a
  plug-in without a spec is skipped), and for a few plug-ins the step texts `oa-assets plugins`
  imports (`oa:tools/plugin-mapper/README.md:14-17`, `plugin_mapper/__main__.py:23-24`, `:540-580`).
  No DLL is loaded into a process, no device or network is touched: every import of the plug-in is a
  rejecting trap until an adapter binds it (`plugin_mapper/executor/offline_host.py:1-45`).
- **Bundle** `openapollo-profiled-runtime-bundle/1` (`plugin_mapper/carrier/build_bundle.py:58-72`):
  name, layout, carrier channels, latency source, DLL path and SHA-256, the descriptor (render id,
  IO, rows, resources with id/architecture/kind/words/SHA-256, scratch, flags), the startup words,
  parameter maxima, the control sweep (per control: integer, readback, the plug-in's own formatted
  text, the DSP events), parameter metadata, and the executed profile. `index.json` lists what was
  built, failures with reasons, mono-only plug-ins and those waiting for a licence
  (`oa:docs/plugin-mapping.md:59-63`).
- **Tables (optional):** for plug-ins whose words depend on few controls at a time, a learned,
  differential-gated table (zlib words, tied to the bundle's SHA-256) replaces the emulator at run time
  (`oa:docs/plugin-mapping.md:183-224`).
- **known-good list** `oa:mixer-engine/device_maps/native_known_good.json`: 119 entries (names, live
  layouts, family, latency, DSP-owned word positions), 112 offered in at least one layout; only these
  are offered unless `--advertise-all-plugins` (`NATIVE-HOSTING.md:60-65`). Measured on one x4 at 48 kHz.

### 1.8 Live parameters, readback and meters

- A knob change becomes a plan: the writes, the exact words to read back, the display values and the
  proposed state (`oa:mixer-engine/plugin_native.py:30-35`). The plan comes from running the plug-in's
  own setter in an emulator worker (about 2.5 ms per change once built; building takes seconds of pure
  Python), or from tables (`oa:mixer-engine/NATIVE-HOSTING.md:34-49`, `oa:docs/plugin-mapping.md:165-181`).
- The backend reads the prior words, sends the writes through `SERVICE` in chunks of at most 1016
  words, reads back every observation and accepts the state only when they match; words that move
  while the DSP runs (meters, envelopes, delay pointers) are learned per instance and skipped
  (`oa:mixer-engine/plugin_native.py:196-233`, `:445-495`).
- `SERVICE` in the driver: WRITEs checked against the retained CREATE layout and relocated, then an
  optional read `{0x000C0004, addr, n, word}`, reply `0x8001nnnn` with the word echoed; a reply that is
  not its own triggers a resync probe (`oa:driver/ua_plugin_instance.c:1013-1158`).
- Meters are explicit allowlists inside the effect's own rows (`oa:mixer-engine/NATIVE-HOSTING.md:91-97`).

### 1.9 Unison

- A Unison chain starts with a licensed integrated preamp header instead of the Mic carrier
  (`F_UNISON`, `oa:driver/ua_ioctl.h:1178-1214`). `BEGIN_UNISON` captures the preamp settings image,
  takes analog ownership of the lane, writes PGA gain 0, waits for consumption and readback plus 80 ms;
  `ACTIVATE_UNISON` routes the chain; `UNISON_PARAMS` writes `{input, parameter, value}` mailbox triples
  (at most 192 words); DESTROY or close restores the baseline (CUSTOM off, 80 ms, ordinary gain)
  (`oa:driver/ua_plugin_instance.c:1334-1484`, `oa:driver/ua_core.c:2692-2940`). This is the ownership
  handover `docs/spec-twinx.md` 4.4 describes. Ordinary writes to an owned lane return busy.
- **Not reachable from user-built maps in 2.1.0:** the mapper does not map Unison preamps
  (`oa:tools/plugin-mapper/README.md:72`), and the daemon turns Unison on only for a provider that
  declares `unison_supported` (`oa:mixer-engine/plugin_native.py:564-566`), which the bundle providers
  never do. The Console filters Unison slots with a list of 47 plug-in ids
  (`oa:mixer-engine/device_maps/unison_plugin_ids.json`, `oa:CHANGELOG.md:136`).
- The analog image already handles preamps beyond 4 (`mixer_val[ch < 4 ? 3 : 14]`,
  `oa:driver/ua_core.c:2706`), which is the x8p's setting 14 for preamps 5-8.

### 1.10 How the UI is drawn

- Console's plug-in window chrome is rebuilt from Rack.dll's layout; the editor inside is, in order:
  UA's HTML/JS GUI (a "lunacomponent", found only in macOS installs), a panel spec, or a generic
  parameter list (`oa:console-rs/crates/plugin-ui/README.md:1-20`, `:60-65`).
- Panel specs (schema 2) come from the pinned open-apollo-panels release; sprite fields name recipe
  ids and `oa-assets plugins` regenerates the art from the user's own DLLs; toolbars, labels, lamps,
  graphs and meters are drawn in code (`oa:console-rs/crates/plugin-ui/RUNTIME-V2.md:1-60`,
  `oa:docs/install.md:136`: 206 of 224 panels from a UAD 12 install).
- Windows open on the drop and stay locked until the plug-in is live; refused loads say why
  (`oa:CHANGELOG.md:78-82`).

### 1.11 What upstream has proven on hardware

| Result | Unit | Source |
|---|---|---|
| Ownership, pool map, board query (cold) | x4 Gen 1 | `oa:HARDWARE-SESSIONS.md:309-313` |
| First native CREATE (1176, 4 modules, 6 rows) on an idle DSP 3, not routed, then DESTROY | x4 Gen 1 | `:314-318` |
| A 1176 insert started on input 1, audio held 10-20 s, plain strip rebuilt | x4 Gen 1 | `:353-398` |
| Live controls with exact readback, four inputs, 336 updates, no quarantine | x4 Gen 1 | `:400-448` |
| REC/MON tap | x4 Gen 1 (Mac Studio, Asahi) | `:450-519` |
| 111 plug-ins load mono and stereo at 2.1.0; the known-good list now has 119 entries, 112 offered (live gate: load, three controls read back, unload), 48 kHz | x4 Gen 1 | `oa:CHANGELOG.md:60-63`, `oa:docs/plugin-mapping.md:7` |
| Strips (not plug-ins) | Twin X Gen 2 Duo | `oa:driver/ua_strips.c:776-784` |

Nothing upstream ran on an x8p, on a Twin X Gen 1, at another rate, or with more than four strips.

---

## 2. What maps onto our stack

### 2.1 Kernel

| Piece | Port from | Our form |
|---|---|---|
| Transaction engine v2 | `oa:driver/ua_dsp_xact.c`, limits `ua_plugin_host.h:126-135` | Today: 16 slots of 2 pages, 32 entries, 2 replies of at most 64 words, 4 words copied out (`driver/apollo.h:62-66`, `driver/strips.c:843-901`). Needed: payloads up to 256 KiB as **separate 4 KiB pages** (a DMA_REF never crosses a page, so no large contiguous buffer, which is what failed at G4 with `0xc000009a`); 128 entries; reply descriptors up to 1024 dwords, several per transaction, copied out whole; a per-transaction reply prefill. The largest module on this PC is 246,068 bytes (section 3.1). |
| Module store and catalog | `oa:driver/ua_plugin_catalog.c:42-299` | `System32\drivers\ApolloModules\<sha256>.bill`, admin-only DACL and `FILE_COMPLETE_IF_OPLOCKED` like `ApolloStrips` (`docs/STRIPS.md` 4.1). The driver opens by the hash the client names, checks name = SHA-256 (CNG, as now) and the Bill header, keeps per-DSP residency with the epoch. Here the DACL is the boundary (no pins for arbitrary plug-ins), as `/lib/firmware` is upstream. |
| Pool ledger | `oa:driver/ua_resource_pool.c` (493 lines, no device access) | New `pool.c`. The strips reserve through it instead of the fixed table (D6); with masks 0x01 and 0xFF this reproduces the 2.6 addresses, which `strip-plan-test` can prove. Scratch raise for chains (`ua_plugin_instance.c:198-240`; chain minimum 448 / 192 / 0, `native_carrier.py:17`). |
| Geometry for DSPs 0-5 | `oa:driver/ua_plugin_place.c:170-213` | Today only DSP 1 is read (`StripsReadLayout`). Read all owned DSPs (windows `n * 0x800`, +0x2000 for n > 3, `driver/regs.h:31`). |
| Board query + admit | `oa:driver/ua_plugin_license.c` (all) | On DSP 0 (section 3.3). Never cached; counts-only log. |
| Instance IOCTL | `oa:driver/ua_plugin_instance.c` (1671 lines), contract `oa:driver/ua_ioctl.h:1058-1247` | CREATE (`CHECK`, plain, `START`, `CHAIN`, later `STAGE`), DESTROY, SERVICE, then SWAP and START_STAGED. Instances belong to the **file object**: `EvtFileCleanup` destroys them and restores the retained strips. Validation (`check_rows`, `check_cmds`, `chain_ids`, `writes_directory`, `check_set`) ported verbatim. |
| Strip override / restore | `oa:driver/ua_strips.c:1059-1129` | Hooks in `strips.c`: per-input "overridden" state, generation counter, restore words; `ApolloStripsQuiesce` and taint also end instances. |
| Strip delays | `oa:driver/ua_strips.c:1135-1290` | Same words, **8 inputs** on the x8p (upstream refuses unless exactly 4 Mic strips, `:1224`, and the struct has 4-entry arrays, `ua_ioctl.h:1260-1265`). |
| Unison lease | `oa:driver/ua_plugin_instance.c:1334-1484`, `ua_core.c:2692-2940` | Last (phase P8). |
| Caps | `oa:driver/ua_ioctl.h:572-594`, `:724-799` | `GET_PLUGIN_CAPS` with our feature bits, owned/quarantined DSP masks, instance count, ledger usage and each DSP's measured load word (window +0x0C, `driver/regs.h` `DSP_WIN_LOAD`) for non-admin readers. |

New IOCTLs, block 0x40 (0x30-0x33 are reserved for the aggregate, `docs/spec-twinx.md` 6.9), interface
version 8 (since Nov 15: BOARD_QUERY and PLUGIN_INSTANCE only from an elevated administrator's handle or from the
program `Parameters\PluginHostImage` names, section 5.3), all `APOLLO_USER` like upstream's (nothing raw reaches the device: no address, command
word or code byte comes from user space, `oa:driver/ua_ioctl.h:989-992`):

| Code | Name | Notes |
|---|---|---|
| 0x40 | `GET_PLUGIN_CAPS` | read only |
| 0x41 | `BOARD_QUERY` | read only for the table; sends the date (section 1.6) |
| 0x42 | `PLUGIN_INSTANCE` | ops 1-8 as upstream; variable-length input (`METHOD_BUFFERED` caps the size: resources, rows and commands go in one input buffer, at most 64 hashes, 128 rows, 1016 command words) |
| 0x43 | `PLUGIN_DELAY` | 8-input arrays |

Locks as for the strips (`docs/STRIPS.md` D11): a host lock before DevLock, never wait on the DSP
holding DevLock.

### 2.2 What is x4 or Gen 2 specific, and what holds for the x8p and the Twin X Gen 1 Duo

| Item | Upstream (x4 Gen 1 0x1F, Twin X Gen 2) | x8p Gen 1 (0x22, 6 DSPs) | Twin X Gen 1 Duo (0x99, 2 DSPs) |
|---|---|---|---|
| SHARC generation, Bill architectures 0/2, upload and reply words | x4 Gen 1 | same X-series Gen 1 family (inference from the serial table and identical strip behaviour) | same family (inference) |
| Pool geometry tops 0x9D1B8 / 0xC6FFE / 0x08FFE000, code base 0xE0000 | x4, Twin X Gen 2 | **verified** on DSP 1 (`docs/STRIPS.md` 2.4); DSPs 2-5 unread | unknown until G5 |
| Firmware | x4/Twin X captures 0x0B0FC9B3 | 0x0B0FD613 (11.15.54803), frozen | 0x0B0FC9B3 |
| Strip DSP and therefore every insert | DSP 1 (`ua_strips.c:129`) | DSP 1 runs **8** strips plus every insert; DSPs 2-5 idle | DSP 1 runs 2 strips plus every insert; DSP 0 is the mixer (strips there miss the monitor mix, `oa:HARDWARE-SESSIONS.md:327-328`) |
| Input map for inserts | `DSP_INPUTS` analog 0-3 + Virtual 14-17, pairs (0, 2, 14, 16) (`native_carrier.py:33-41`) | analog synch 0-7, pairs 1/2, 3/4, 5/6, 7/8 | synch 0-1, pair 1/2 |
| Instances | `max_instances=4` (`plugin_native.py:570`) | up to 8 inputs | 2 |
| Delay IOCTL | exactly 4 Mic strips (`ua_strips.c:1224`) | 8 | 2 |
| Boot word model byte | Twin X Gen 2 0x27 (`ua_plugin_host.h:276-281`) | 0x22 (measured: `00002203` cold) | masked out; Gen 1 value read at G5 |
| Unison analog image | preamps 1-4 setting 3, 5+ setting 14 (`ua_core.c:2706`) | 8 Unison preamps, settings 3 and 14 (our readback notes, spec-dsp-mixer) | 2 preamps + Hi-Z, Twin X setting layout (`docs/spec-twinx.md` 4.1) |
| Known-good list, latencies, DSP-owned words | measured on the x4 only | assume valid, confirm per plug-in | same |

Gen 2 only: the Twin X Gen 2 strip entries and boot-word note. Nothing in plug-in hosting depends on
Gen 2 features.

### 2.3 User-mode host: replacing the daemon

Upstream splits the work: providers plan, the backend owns every device operation
(`oa:mixer-engine/plugin_native.py:1-5`). The mixer itself (faders, monitor) is already Apollo
Control plus our driver, so only the native-hosting part of the daemon is needed.

| Option | Reuse | For | Against |
|---|---|---|---|
| **A. Python helper process** (recommended for P1-P5): vendored copies of the native backend modules (`plugin_native*.py`, `plugin_transactions.py`, `plugin_publication.py`, `native_carrier.py`, `native_chain_composer.py`, `native_profiled_carrier_provider.py`, `profiled_bundle_registry.py`, `emulator_worker.py`, `plugin_delay.py`, `plugin_license.py`) plus the mapper's executor, with `plugin_driver.py` rewritten over `DeviceIoControl` (ctypes) and a small state adapter in place of the daemon's tree | most of it | the emulator is Python anyway; tested logic, same receipts and gates; runs at user integrity and holds no admin rights | ships a Python runtime and four wheels |
| A'. Same, but Apollo Control owns the device handle and the helper only plans | as A | one device client | every transaction crosses a pipe; upstream's fd-ownership semantics need care |
| B. C++ backend inside Apollo Control, tables-only plug-ins | little | no Python at run time | only plug-ins with passing tables (all-stepped ones), the rest need the emulator; large rewrite |

Recommended shape: `apollo-plughost` (Python 3.13, venv under the user's profile) holds its own handle,
so its exit destroys every instance and restores the plain strips (upstream semantics, fail safe).
Apollo Control (already tray-resident, `app/src/main.cpp` `--minimized`) starts and supervises it and
talks JSON over a named pipe: list (authorized, mapped, known-good), load, unload, set, meters,
licence states, mapping progress. Saved inserts are reloaded only on an explicit user action, never
automatically (`oa:mixer-engine/NATIVE-HOSTING.md:100-101`).

### 2.4 The mapper on Windows

- **Runtime:** pure Python plus `unicorn==2.1.4`, `capstone==5.0.9`, `pefile`, `numpy`
  (`oa:tools/plugin-mapper/requirements.txt`). All four publish Windows x64 wheels. This PC has Python
  3.13 and 3.11; use 3.13 (the pinned numpy is likely too new for 3.11, to check at install).
- **Linux-only calls to patch** in a vendored copy: `fcntl.flock` (`plugin_mapper/__main__.py:455`,
  `:533`, `discovery/run_catalog.py:33`, `execution/run_catalog.py:128`) to `msvcrt.locking`;
  `os.killpg` (`discovery/run_catalog.py:68`) to a Job Object or `taskkill /T`; the `fork` pool
  (`__main__.py:480-482`) to `spawn`; default paths `/lib/firmware/...`, `XDG_*`, `~/.local/share`
  (`__main__.py:36-44`) to `%LOCALAPPDATA%\Avium\...`. `OA_MODULE_STORE` already redirects the store
  (`carrier/descriptor.py:24`, `execution/run_catalog.py:10`).
- **Module store for the mapper:** a user-readable copy of the same records (the driver's copy stays
  admin-only). Built by a new `apollo-diag modules install` (C, extends today's strip extractor;
  `oa-assets dsp` is the reference) or by `oa-assets` itself (Rust 1.94.1 is installed here; two
  `std::os::unix` imports need `cfg(unix)` guards, `oa:console-rs/crates/oa-assets/src/dsp.rs:548`,
  `firmware.rs:30`).
- **Panel specs:** download the pinned open-apollo-panels release (`oa:configs/plugin-panels.conf:5-7`,
  about 1 MB, SHA-256 checked). Required: the mapper skips plug-ins without a spec.
- **Authorization filter:** `--authorized FILE` from our board query until the helper answers like the
  daemon does.
- **Version limit:** families pinned to UAD 11.8.3 refuse other versions
  (`oa:tools/plugin-mapper/README.md:67-73`). Here that is 12 of the 112 offered known-good plug-ins
  (section 3.2).

### 2.5 UI: what is realistic

| Option | Realistic? | Notes |
|---|---|---|
| Generic parameter panel in Apollo Control (WebView2) | **yes, P2** | names and steps from the bundle's metadata, live texts from the plug-in's own formatter, readback-confirmed values, refusals shown with the reason |
| open-apollo's panel spec runtime, ported to JS in our UI | later (P6, optional) | `plugin-ui` is Rust drawing into Console's draw lists (`panel.rs`, `widgets.rs`, `graph.rs`, `meters.rs`); art from `oa-assets plugins` built on Windows. Knobs, switches, lamps and sprite meters first; graphs last |
| UA's own HTML GUIs | no | lunacomponents ship only with macOS installs; this PC's UAD Console 1.2.1 has none (section 3.1) |
| UA's own editor from the Powered DLL (VST) | no | the editor needs the plug-in object running natively against `UAD2SDK.dll` and UA's driver stack, which no longer owns the units; replacing that SDK is a separate reverse-engineering project with its own legal questions |
| UADx native VST3 GUIs (45 `uaudio_*.vst3` here) | no | separate CPU products with their own licences; their parameters drive their own native DSP |

### 2.6 Files and where they live

| What | Where | Who writes |
|---|---|---|
| Driver module store | `%SystemRoot%\System32\drivers\ApolloModules\<sha256>.bill` (SY+BA only) | `apollo-diag modules install` (elevated) |
| Mapper store copy, bundles, tables, index, progress | `%LOCALAPPDATA%\Avium\ApolloPlugins\{modules,maps,work}` | mapper, as the user |
| Panel specs | `%LOCALAPPDATA%\Avium\ApolloPlugins\specs` | downloader (pinned hash) |
| Python venv | `%LOCALAPPDATA%\Avium\ApolloPlugins\venv` | installer step |
| Repo | code, tests with synthetic I/O, the known-good list as upstream ships it | never UA-derived data; add a guard to `tools\build-all.cmd` |

---

## 3. Feasibility on this PC (Nov 14 2026, read-only)

### 3.1 The UAD install

| Location | Found |
|---|---|
| Uninstall registry | UAD Powered Plug-Ins **11.5.2.43082**, UA Connect 1.5.0 |
| `C:\Program Files\Universal Audio\Powered Plugins\UAD-2 Powered Plugins` | **220** Powered plug-in DLLs; 123 carry UA's `11.4.0` version string, 97 partner version strings. Every one carries Bill records |
| `C:\Windows\System32` | `UAD2SDK.dll` (3,791,360 bytes) and `UAD2DriverClient.dll` (260,096 bytes), both 2024-11-15, no version resource |
| `C:\Program Files (x86)\Universal Audio\Powered Plugins` | `UA.xml` (181 plug-in registry entries, every one with a render id inside the table's 0x800-0xAFF), `Manifest` (220 JSON), `Plug-In Icons` (217), `LookAndFeel` (333 PNG), `Presets` (9,286), `Factory Presets` (9,127), `Firmware` (59), `Data` (3), `Drivers` (87) |
| `C:\Program Files (x86)\Universal Audio\UAD Console` | UAD Console 1.2.1.371; `Resources` 2,033 files (321 plug-in manifests JSON, 1,655 images); **no lunacomponent GUIs** |
| `C:\Program Files\Steinberg\VSTPlugins\Universal Audio` | 419 VST2 shell DLLs (not mapper inputs) |
| `C:\Program Files\Common Files\VST3` | 45 UADx `uaudio_*.vst3` bundles (native CPU plug-ins, not DSP) |
| `%ProgramData%\Universal Audio` | absent |

Bill records (byte scan, counts only): **950 distinct records** across the 220 DLLs plus the two
System32 DLLs; kinds 764 code, 132 kind 1, 44 kind 2, 10 kind 3; architectures 186 arch 0, 381 arch 1,
383 arch 2; largest 246,068 bytes, so every module fits one 256 KiB upload; no (id, architecture)
pair with two different byte versions. Code is architecture 2 and data architecture 0 in the
plug-ins checked.

### 3.2 Mapper inputs

| Input | Here? |
|---|---|
| Powered DLLs for the known-good list | **all 119** installed |
| `UAD2SDK.dll` | yes (a different SDK than the gate's is used, and recorded in `index.json`) |
| Families mappable from UAD 11.5.2 | 100 of the 112 offered: generic 90, vst2_bx 7, vst2 2, vst2_relab 1 |
| Families pinned to 11.8.3 (refused here) | 12: DSPAudioProcessor 5 (AMS DMX 15-80 S, Auto-Tune Realtime Access and Advanced, C-Suite C-Vox, Sound Machine Wood Works), VST-main 4 (Oxford Dynamic EQ, Oxford SuprEsser DS, Sphere Mic Collection, Sphere Mic Collection 180), Sonnox 2 (Oxford Inflator, Oxford Limiter), ParamObj 1 (Auto-Tune Realtime X) |
| Module store | not yet (section 2.4) |
| Panel specs | not yet (download) |
| Step texts (`oa-assets plugins`) | not yet; needed by a few plug-ins only |
| Python + wheels | Python 3.13 and 3.11 present; unicorn, capstone, pefile, numpy not installed |
| Rust (for `oa-assets`) | 1.94.1 present |

Note for Aaron's Townsend Sphere: the two Sphere Mic Collection plug-ins are in the 11.8.3-pinned
family. They map only from a UAD 11.8.3 install or after the VST-main profile learns to find its
addresses (`oa:docs/plugin-mapping.md:246-248`).

### 3.3 Reading the licence mirror from the x8p

What the driver must send and accept (from section 1.6), on our registers:

| Item | Value |
|---|---|
| DSP | 0. Ring bank BAR0 0x2000: command ring at +0x00, response ring at +0x40; per ring page registers +0x00..+0x1C, `WRITE_PTR` +0x20, `DOORBELL` +0x24, `POSITION` +0x28 (`driver/regs.h:39-50`) |
| Response ring | one descriptor `{0x80000302, 0, reply lo, reply hi}`; reply area 3,080 bytes prefilled with `0x83000000` (fits our reply page) |
| Command ring | inline `{0x00100002, date, 0, 0}`, `{0x00110002, cookie, 0, 0}`, then the doorbell; replies published first (as `XactSubmit` already does) |
| Accept | response ring `POSITION` past the descriptor within 2 s; reply word 0 `0x80030302`, word 1 = cookie; epoch unchanged. Words 2..769 = table |
| Never | `0x000D0000`, `0x000E0000`; any write of the table; demo start |
| Gating | Stage Connected with live DSP 0 rings. Ownership not needed, so it works **warm** |

Engine changes it needs: `XactRx` above 64 words (`STRIP_RX_WORDS_MAX`), `XactReply` copying more than
4 words, a per-transaction prefill, and a DSP 0 transaction beside the existing DSP 0 senders
(connect, finalize, rate ack: none expects a reply, `docs/STRIPS.md` 3.1). Our engine has never
published a reply descriptor on DSP 0; upstream does this on the x4.

Current state (non-admin `apollo-diag`): x8p Connected, Mode 2, 48 kHz locked, **warm start**: boot
words `00082207 00080007 x5`, strips Off ("power-cycle the Apollo for input strips"), DSP 1 pools as
measured in `docs/STRIPS.md` 2.4. A board query could run in this state once built; placement could
not (no DSP is owned until a power cycle).

### 3.4 Room on DSP 1 of the x8p (derived)

DSP 1 code pool: 0x4000 = 16,384 words, of which the strips use 994 (0x3E2). Architecture-2 code per
DLL (upper bounds: a DLL can carry variants a descriptor does not use): Pultec HLF-3C 1,178,
dbx 160 1,946, Precision De-Esser 2,004, LA-2A Silver 2,590, UA 610-B 3,102, 1176 Rev A 3,586,
Pure Plate 3,746, Neve 1073 5,876. Instances of one plug-in share its code. So DSP 1 holds roughly
three to six distinct plug-in types next to the 8 strips. Data pools are not the limit (pool 1 about
43K words, pool 3 about 16.7M). Each chain adds its own Mic strip copy: 86 pool-1 words and four
2,048-word pool-3 commons. The SHARC's cycle budget is the unknown: watch DSP 1's load word.

---

## 4. Phased port plan and hardware gates

Each gate is one session. "Power cycle the x8p" also drops the Twin X (daisy chain): never during a
Twin X test, and Aaron loses his speakers for about a minute. Inserts on DSP 1 are in the x8p
monitor mix, so HP1 carries them: headphones low.

| Gate | Build / setting | Aaron does | Proves | Rollback |
|---|---|---|---|---|
| **P0** (no hardware) | Engine v2, ledger (strips through it), module store, board query, INSTANCE CREATE/DESTROY with START+CHAIN mono, caps; offline tests ported (`memspec`, `check_cmds`, `chain_ids`, ledger, Bill check, date encoding, licence decode) beside `strip-plan-test` (which must still prove the strip addresses); mapper on Windows; helper or bench script with ctypes `DeviceIoControl`; `apollo-diag modules install` and `plugins licence/check/caps`; Verifier run | nothing | code paths, and that the strips are unchanged | none |
| **P1 one insert** | section 4.1 | one x8p power cycle; speak or play into input 1 | the x8p answers the licence query; a plug-in's modules upload and its chain runs on DSP 1; clean restore | close the bench (restores the strip); worst case power cycle; previous release |
| **P2 live controls** | `SERVICE` writes + read; emulator workers (or tables); Apollo Control "Inserts" page with the generic panel | load 3-5 authorized plug-ins one at a time; turn each control | every parameter at 0 / middle / max accepted and read back, texts match the bundle's sweep, unload restores; upstream's live gate per plug-in on the x8p | unload; helper exit |
| **P3 chains, stereo, all inputs** | `STAGE`, `SWAP`, `START_STAGED`; composer with x8p input map and pairs 1/2..7/8 | 4-insert chain on one input; a stereo pair; one insert on each of 8 inputs | swap without a gap, control retention, stereo L/R distinct, DSP 1 capacity: code pool use and load word with N inserts | as P2 |
| **P4 delay compensation, REC/MON** | `PLUGIN_DELAY` for 8 strips; settings Off..Long; REC/MON switch; ASIO latency includes the delay | loopback cable (an x8p line out to input 1, line) | record alignment at all five settings measured, overload warning, REC/MON both ways | setting Off |
| **P5 daily use** | helper as the owner, licence mirror in the UI, catalogue names and icons from the local install, explicit restore of saved inserts, installer step (venv, store, specs, mapping in the background) | normal sessions, a driver update + power cycle, sleep/resume | inserts survive what they should, nothing reloads silently, warm starts say "power-cycle for inserts" | uninstall the feature |
| **P6 panels (optional)** | panel spec renderer in our UI, art built locally | use the drawn panels | specs render with the user's art; generic panel stays the fallback | generic panel |
| **P7 Twin X inserts** | built Nov 15 (section 4.1c); DSP 1 of the Duo | as P1-P2 on Twin X input 1 | Gen 1 Duo geometry, licence query, one insert, restore | as P1 |
| **P8 Unison (research)** | Unison analog lease port; a Unison preamp family in the mapper (none upstream) | Unison preamp on one input | gain/impedance handover and restore, no clicks or 48V surprises | destroy restores the baseline |
| **P9 more DSPs on the x8p (research)** | strips and inserts for some inputs on DSPs 2-5 (bus owner and route rules) | as P3 | inserts beyond DSP 1's budget | strips back on DSP 1 |

### 4.1 Gate P1 in detail: one known-good plug-in on x8p input 1

**Choosing the plug-in.** The board query decides. Take the first of these the x8p authorizes; all are
known-good mono and stereo, in families that map from UAD 11.5.2, with no DSP-owned words listed:

1. API Preamp (generic, latency 55; "controls verified" upstream), then API 560, API 550A, Avalon
   VT-737sp (55), Bermuda Triangle (88). A non-zero latency makes the proof cable-free (below).
2. Otherwise any authorized generic entry with no `dynamic_refs`, smallest first: Pultec HLF-3C,
   dbx 160, Precision De-Esser, UA 610-B (probably authorized, it is in the bundle UA ships with
   Apollo hardware; inference), Neve 1073.

Also required: its startup fits one command page (the carrier builds `create_arguments()` without
staging, `oa:mixer-engine/native_carrier.py:422-425`), or a driver with staged startup (section 4.1b).

**Preconditions.** P0 build installed; the x8p cold-started with our build (8 strips live, DSP 1
owned, boot words `00002203 00000003 x5`); no Twin X test running; Apollo Control is the fixed build
or closed; ask Aaron what is plugged into inputs 1 and 2 (his Sphere's two outputs on 1-2 give a
reference pair).

**Steps** (a bench script; each step stops the session on any failure):

1. `apollo-diag plugins licence`: counts per state, and the state of each candidate only. Warm is
   fine, so this can run before the power cycle.
2. Map the chosen plug-in on this PC (`--only`), then offline checks of the recipe.
3. `CREATE` with `CHECK`: admitted, nothing placed.
4. `CREATE` without `START` on **DSP 2** (idle, never routed), as upstream's first native CREATE ran on
   an idle DSP (`oa:HARDWARE-SESSIONS.md:314-318`): every upload `80070004 0 id`, rows zero-filled and
   linked; `DESTROY`; DSP 2's ledger back to baseline.
5. `CREATE` `START`+`CHAIN` on DSP 1, input 1, mono, defaults: plain strip 1 overridden; read back the
   recipe's observations; DSP 1 load word before and during; record slot 0 and MON loopback slots
   26/27, plus slot 1 if the Sphere's second output is there; hold 60 s.
6. `DESTROY`, then repeat 5 and end by closing the handle instead: plain strip 1 restored both ways.

**Pass.** All uploads acknowledged, 0 timeouts, 0 quarantines, readback exact; DSP 1 load word rises
while the chain runs and returns after; on input 1 the plug-in's effect is measurable: with the Sphere
on 1-2, the lag of slot 0 against slot 1 shifts by the plug-in's latency (55 for the API entries, or a
measured value to report), or else a level change at defaults; inputs 2-8 unchanged; after restore, the
driver's state is back (strips Live, input 1's plain strip taken and started with its header, no instance,
DSP 1's ledger at its baseline, nothing quarantined) and input 1 is live as before; x8p stays Connected; no
bugcheck. (Nov 15: the level and lag of input 1 against input 2 after the restore are no longer compared:
the Sphere's two diaphragms differ acoustically. The first CREATE after a power cycle builds DSP 1's ledger,
so apollo-plug derives the "before" ledger from the pool registers as the driver builds it. Readback skips
the recipe's `dsp_owned` words and any word that moves between two reads: the 1176LN Legacy's row 10 word 5.)

**Rollback.** Closing the bench handle restores the strip. A quarantine or a hang needs an x8p power
cycle (drops the Twin X). Package rollback with `install.ps1 -Package <previous release>`.

### 4.1b Staged startup: a startup over one 1016-word page (built Nov 15 2026, not run yet)

**Upstream's model, ported as is.** A chain whose startup words exceed one command page is created
with `F_STAGE` (`oa:driver/ua_ioctl.h:1154`, `:1215-1217`): the same placement, uploads and private
rows as `F_START`, then one transaction with the first page of WRITEs only; nothing is stopped,
SYNCH'd or routed, so the input's plain strip keeps running. The other pages go as SERVICE writes,
which the driver already allows on a strip-replacing instance and checks against the CREATE layout
(never the chain directory). Op `START` (8, `UA_FEAT2_PLUGIN_START_STAGED`, `ua_ioctl.h:1094-1101`,
`ua_plugin_instance.c:1277-1332`) then routes it exactly as `F_START` would, without WRITEs. The
pages are cut at whole WRITE blocks, at most 1016 words each (`native_carrier.py:255-271`); the
daemon's order is CREATE stage, the pages, the readback of the whole startup while nothing routes
it, then START (`plugin_native_chain.py:323-371`, `:108-118`).

| Piece | Where |
|---|---|
| Flag rules, page cut, stage/START transaction words | `driver/plugin_plan.h`: `PlugCreateFlagsOk`, `PlugChunkNext`, `PlugBuildStartEx` (Route 0 = stage) |
| CREATE `F_STAGE`, op START, `FEAT_STAGE`, `INST_STAGED`, step `ROUTE` | `driver/plugin.c` (`PlugCreate`, `PlugStart`, `PlugStartStaged`), `include/apollo_plugin.h` |
| Recipe with upstream's own page cut (`chunk` lines) and an `effect` hint | `tools/plugin-host/p1_recipe.py` |
| Stage, pages, readback while staged, row-0 head check before and after START; tail and level measures | `tools/plugin-host/apollo-plug.c` (`create-start`, auto-staged; `--staged` forces it), `selftest` |
| `-Recipe lexicon-480l` (short names), effect-specific prompts, staged-driver check | `tools/phase1/step-p1.ps1` (default recipe: `ampeg-svtvr-classic`) |

Safety as in P0: every transaction (the stage page, each SERVICE page, START) is one bounded
transaction (1 s) and an unproven outcome quarantines the DSP. Any failure before START, a
readback difference, or Ctrl+C / a crash (the handle closes) takes the chain down without ever
touching the input: a staged chain that was never routed only releases its modules and rows.
START refuses, before sending anything, a chain that is not staged, an input another instance
routes, or a plain strip rebuilt since the CREATE (generation).

Offline proof: `plugin-plan-test` (flag rules equal P0's without STAGE; the page cut equals
upstream's on upstream's own three chunk tests and on reference cuts computed with the vendored
2.1.0 code; a synthetic 5.6K-word startup cut, staged, paged and routed puts exactly the words of
the one-shot form on the DSP; own golden digest, P0's golden unchanged); `apollo-plug check` on
each recipe (every page passes `check_cmds`, pages after the first never touch the directory, the
cut equals the recipe's upstream cut, DSP 1 fit with the measured pools); `apollo-plug selftest`.

**The two recipes** (mono, input 1, plug-in defaults; record delay 200 as in 4.1):

| | Ampeg SVTVR Classic (vst2_bx) | Lexicon 480L (vst2_relab) |
|---|---|---|
| Startup | 2,597 words, 3 pages | 64,407 words, 64 pages (63 SERVICE writes) |
| Modules, rows | 1 own module (4,214 code words), 20 rows | 2 (15,066 code words + a 1,624-word data module), 90 rows |
| DSP 1 beside the 8 strips (ledger, measured pools) | fits: code 5,208 of 16,384 used | fits: code 16,060 of 16,384 used, **324 left**: no other distinct plug-in fits beside it |
| Latency (known-good list) | 0 | 64 samples |
| Readback while staged | 155 runs in 40 reads | 3,745 runs (58,732 words) in 934 reads |
| Effect measure | level / spectrum against input 2 (or input 1's HF tilt alone) | tail after claps (against input 2 when it has the same source) |

DSP 1's cycle budget for these is not known offline: watch its load word. If a recipe does not fit
DSP 1, `create-plain --dsp 2` (the unrouted CREATE on the idle DSP 2) is its first test.

**Hardware gate (Aaron).** Needs this build installed and the x8p's strips Live: an x8p power cycle
after the install (drops the Twin X; headphones low). Then `step-p1.ps1 -Recipe ampeg-svtvr-classic`
(the smaller startup) and `step-p1.ps1 -Recipe lexicon-480l`. Pass as in 4.1, plus: every page
taken, staged readback exact, the head as written before and after START, and the effect measured
(Ampeg: input 1 changes against input 2; Lexicon: a tail at least 6 dB above the plain strip's
after short sounds, gone again after the restore).

### 4.1c P7: the Twin X Gen 1 Duo (built Nov 15 2026, code and offline tests; not run on hardware)

Facts it rests on (G5 on hardware, Nov 15): 2 DSPs; strips on DSP 1 (StripMask 0x3), modules 0xEB/0xDA/0xA5/0xC2/0x12B
at 0xE0000, headers 0x9D1B0 / 0x9D15C, route words to DSP 0 only; DSP 1 pools code base 0xE0000, tops 0x9D1B8 /
0xC6FFE / 0x08FFE000 (`step-g5.log`: size `4000 a9b9 1fff ff51ae`, base `e0000 92800 c5000 8008e52`, scratch `80 80
8000`). Upstream never hosted plug-ins on a Duo (strips only, Twin X Gen 2, `oa:driver/ua_strips.c:775-784`); its
licence query is per device on DSP 0 (`oa:driver/ua_plugin_license.c:98-187`), as ours.

| What | Twin X | Where |
|---|---|---|
| The plan per model | `PLUG_MODEL` (`PlugModelFor`, `PlugModelSynch`): strip DSP 1, board query DSP 0, 2 strips' room, inputs 1-2 = DSP inputs 0-1, the pair 1/2, Hi-Z = input 1's strip, talkback (DSP input 10) refused, routes DSP 0 only, measured pools | `driver/plugin_plan.h` |
| Driver gate | the x8p and the Twin X in Mode 2 (`PlugEnabled`); everything else was per device already: its own strip DSP, ledger (`LayoutStrips` 2), board query and licence table, strip target (mask 0x03), pairs (2 inputs). `FEAT_UNITS` (0x80) says so; the x8p's words, ledger and checks are unchanged | `driver/plugin.c`, `include/apollo_plugin.h` |
| Apollo Control | per-unit state (`UnitState`: caps, info, licence, snapshots; each unit through its own slot handle), inserts saved per unit id (as before), the recipe built for the unit's inputs (`RecipeOptions.unit`), the picker per unit's licence, DSP room per unit, `unit` on every plug-in message (`docs/apollo-control-units.md` 1.5) | `app/src/plugin_engine.h`, `app/src/plugin_host.h` |
| Tools | `p1_recipe.py --unit twinx` (inputs 0-1; `<name>-twinx-<layout>-in<n>.recipe`), `apollo-plug --unit twinx` (its inputs, its measured ledger offline, texts; the MON loopback as record slots 2/3, expected), `step-p1.ps1 -Unit twinx` (the Twin X's hwid, log tag `twx`, the unrouted CREATE on DSP 1 since a Duo has no idle DSP) | `tools/plugin-host`, `tools/phase1/step-p1.ps1` |
| Offline proof | plugin-plan-test `TestModels`: the x8p row = the pre-P7 constants, both rows = strip_plan.h's, the x8p input rule unchanged, the Twin X's inputs and refusals, its ledger (2 strips at the tops, headers inside, a chain placed and back to baseline); plugin-recipe-test: Twin X input 1 = golden 0's text, input 2, the pair 1/2, inputs 3 and pair 3/4 refused | `tools/plugin-plan-test`, `tools/plugin-recipe-test` |

**Room on the Twin X's DSP 1 after its 2 strips** (ledger from the measured pools): code 15,390 of 16,384 words
(largest 15,388), the same as the x8p (the strip code is 994 words whatever their number); data pool 1 42,832 words, pool
2 8,030, pool 3 16.7M (6 strips' more data room than the x8p). So the code pool is again the limit: roughly three to six
distinct plug-in types, or one Lexicon 480L (15,066 code words) alone. Its cycle budget is unknown: watch DSP 1's load word.

**Hardware gate (with Aaron).** Needs this build installed on the Twin X, Mode 2, `StripsEnable 1` (step-g5.ps1 run 2 left
it so) and the recipe's modules in the store. A Twin X power cycle drops only the Twin X. Its monitor mix is the
speakers: a started chain is heard there at once.

1. Speakers down or off, headphones low. Power the Twin X off, wait 10 s, on; wait about 60 s (strips Live: `apollo-diag
   --unit twinx strips` says Live, inputs 1-2 taken).
2. Elevated: `step-p1.ps1 -Unit twinx` (default recipe Ampeg SVTVR Classic, staged 3 pages; the x8p's input-1 recipe
   file is used when no `-twinx-` one exists: the same words). It reads the Twin X's own licence (its authorized names
   go to `plugins\p1\authorized.txt`), CREATE CHECK, the unrouted CREATE on DSP 1, then START on input 1, restored by
   DESTROY, then by closing the handle. If the Twin X does not license the Ampeg (its table is its own), make an
   authorized one's recipe (normal prompt): `p1_recipe.py "<name>" --unit twinx --authorized plugins\p1\authorized.txt`.
3. Listen: play or speak into Twin X input 1 when asked (a mic, or an instrument in the Hi-Z jack: the same strip). While
   the chain runs the amp character is audible on input 1 (in headphones / speakers); input 2 stays dry. After each
   restore input 1 is plain again. `-Recipe lexicon-480l` next: a reverb tail on claps, gone after the restore.
4. Pass as gate P1: every upload acknowledged, readback exact, 0 quarantines, DSP 1's ledger back to its baseline, strips
   Live, the x8p untouched. Then Apollo Control: insert on Twin X input 1 from the Plug-ins tab once the UI enables it.

### 4.2 Effort (rough, one developer; this project's agents have been faster)

| Phase | Kernel | User mode | Hardware session |
|---|---|---|---|
| P0 | 6-9 days | 4-6 days | none |
| P1 | fixes only | analysis script | 1 (about 45 min, one power cycle) |
| P2 | 1-2 days (`SERVICE`, read) | 4-6 days (workers, UI panel, gate tooling) | 1-2 |
| P3 | 3-4 days (`STAGE`, `SWAP`, `START_STAGED`, multi-input) | 2-3 days | 1-2 |
| P4 | 1-2 days | 2 days | 1 |
| P5 | 1 day | 5-8 days | soak |
| P6 | none | 10-20 days | none |
| P7 | 1-2 days after G5 | 1 day | 1 |
| P8 | 5-10 days | 10+ days (mapper family) | several; uncertain |
| P9 | 5-10 days | 1-2 days | several; research |

### 4.3 Riskiest unknowns, ranked

1. **The x8p's licence answer on our driver.** First reply descriptor our engine posts on DSP 0, with a
   770-word reply; the table must be there (UA wrote it while the unit was on UA's software). An empty
   or failed answer stops everything, by design: no bypass.
2. **DSP 1 capacity.** All 8 strips and every insert share one SHARC and a 16,384-word code pool
   (section 3.4). Upstream never ran more than 4 strips plus inserts. Spreading over DSPs 2-5 (P9) is
   new ground: on the x4, strips on DSP 0 missed the monitor mix, and no strip ever ran on DSPs 2-5.
3. **UAD version drift.** The live gate used another UAD version; our bundles come from 11.5.2 DLLs
   (most at 11.4.0). Recipes may differ, and readback catches only some errors (audio checks catch the
   rest). Twelve known-good plug-ins cannot map here at all. Updating UAD on this PC is possible but
   UA's installer must never update firmware (`docs/PLAN.md` decision 10).
4. **A bad recipe hangs a DSP or drops the unit off the bus.** Seen upstream: the A1 strip parameters
   took the x4 off PCIe; Studio D Chorus hung DSP 1 (`oa:HARDWARE-SESSIONS.md:324-326`,
   `oa:docs/status/PLUGINS.md:39`). Recovery is a power cycle, which on this desk also drops the Twin X.
   New kernel DMA lifetimes (multi-page payloads, long replies) are the bugcheck risk: Verifier first.
5. **Mapper on Windows:** the patches are small but the emulator is slow (seconds per plug-in build,
   about 2.5 ms per control change); some families may hit Windows-specific behaviour in the
   emulated CRT.
6. **Warm starts.** Any driver update or warm restart leaves no DSP owned: no strips and no inserts until
   an x8p power cycle (as for the strips today). Superseded Nov 15: strips are adopted (`docs/STRIPS.md` 12) and
   inserts go back on the adopted strip DSP (12.5); a power cycle only when the record cannot prove it.
7. **Twin X Gen 1:** strips not yet (G5), a 2-DSP Duo, and its main draw (Unison) is unmapped upstream.

---

## 5. Plug-ins in Apollo Control (Nov 15 2026: built, offline tests only)

### 5.1 What the user can do

- **On the Control view** (since Nov 15 2026 there is no Plug-ins tab): a **browser** docked beside the strips, the
  plug-ins grouped by type from UA's own categories in the local install (docs/apollo-control-units.md 1.5), with search,
  a "fits the DSP" filter, counts, previews from the install's own thumbnails and a detail card (DSP code against the
  room, latency, parameters, mono/stereo, staged startup, licence); an **insert slot** on every strip between the
  preamp and the monitor fader. **Drag** a plug-in onto a slot to load it, a loaded slot onto another to move it, back
  onto the browser (or the slot's x) to unload it; every slot says why it does not take a plug-in. The keyboard does
  the same (Enter picks up, Enter places, M moves, B bypasses, Delete removes, Esc cancels). The driver's chain takes a
  carrier and up to four effects per input (`driver/plugin_plan.h` PlugChainIds, N = 2..5); since [Ins] (Nov 15) the host
  composes up to four per input (`plugin_host.h` BuildChain; docs/apollo-control-units.md 1.9).
- **Picker**: the plug-ins that are both mapped (`bundles\index.json`) and authorized by this x8p's own
  licence answer (BOARD_QUERY, read when the tab opens, kept in memory only); search; each with the code
  words it needs on DSP 1, its latency, parameter count and a multi-page startup; Insert is off with the
  reason when DSP 1 has no room. Mono, or **stereo on a linked pair** (1/2, 3/4, 5/6, 7/8) when the bundle
  has a stereo layout and the driver has FEAT_STEREO.
- **Insert / Replace / Remove / Bypass** (bypass takes the chain off the DSP, the plain strip runs, its
  settings stay; turning it off inserts it again). One-page startups go as one CREATE START; longer ones
  staged (CREATE STAGE, SERVICE pages, readback and head check while nothing routes them, then START).
- **Parameter drawer** (opened in place under the strips, pointing at its strip): a knob per continuous parameter
  with numeric entry, buttons or a list per stepped one, the plug-in's own texts, and per control RB OK / SENT / NOT
  CONFIRMED; each change is planned by the plug-in's own setter, sent through SERVICE and read back before it counts; a
  refused or unconfirmed change is put back and said inline.
- **DSP 1 room**: the code words left (the ledger's, or 16,384 less the strips' 994 before the first CREATE).
- **Refusals inline**, by code: not licensed, no room, needs a power cycle (warm start), DSP quarantined,
  modules missing, hosting off, not the host program, strips not live, not confirmed (readback).
- **Saved inserts** per unit and input: put back whenever the unit's strips are Live and the driver owns its strip DSP
  (a cold start, Apollo Control starting, and since Nov 15 a warm start that adopted or rebuilt the strips, FEAT_WARM);
  otherwise they wait with the driver's reason (`dsp.ownWhy`) and "power-cycle the x8p".
- **Settings, Plug-in hosting**: Turn on / Turn off (an administrator prompt each time), Install modules,
  the maps folder.

Inserts belong to Apollo Control's device handle (instances belong to the handle that created them): they
run while Apollo Control runs (it lives in the tray) and come back on the next start; quitting it, a driver
update or an x8p removal restores the plain strips.

### 5.2 How it is built

| Piece | Where |
|---|---|
| Recipe from a bundle: the C++ port of upstream's `GeneratedOrdinaryCarrier` (rows, startup WRITEs, resources, scratch, carrier record delay), `StereoProfiledCarrierProvider`, the provider's filtered readback, the page cut, the driver's own checks (`plugin_plan.h`), the DSP-owned words (bundle dynamic refs, panel-spec meters, `plugins\p1\dsp-owned.txt`), the catalogue, the saved-insert file | `app/src/plugin_host.h` (header only, no device code) |
| Plug-in thread: caps, licence, CREATE / stage / START / SERVICE / DESTROY through the x8p slot's handle, readback with moving-word learning, the planner process, the catalogue cache, restore, the elevated step | `app/src/plugin_engine.h` (included by `main.cpp`) |
| Parameter planner: upstream's provider `plan_control` / `plan_restore` / `accept` in open-apollo's offline emulator (or the mapper's tables when they exist), JSON lines over pipes; never touches a device | `tools/plugin-host/plughelper.py` (copied next to the exe) |
| UI | `app/ui/plugins.js`, `plugins.css`; hooks in `app.js`, `index.html` |
| Offline tests | `tools/plugin-recipe-test` (selftest: golden recipes from `p1_recipe.py` on synthetic bundles, `make-golden.py`; `compare.py` on this PC's bundles; `catalog`), `app/tests/plugins.js` |

**Recipe: C++ port, not Python.** Saved inserts must come back at start-up with no Python, so the recipe is
built in C++ from the bundle and the module store; the 500-odd lines it needs from the carrier port
directly. Proof: `compare.py` builds every mapped bundle on this PC both ways (251 bundles, mono and stereo,
record delay 200 and the carrier's: 502 recipes) and the text is identical line for line; the golden test
keeps that true without UA data. Saved control state is replayed as WRITEs after the fresh startup, as
upstream's chain composer keeps a neighbour's controls across a rebuild.

**Parameters: the plug-in's own setter, on demand.** A bundle holds a control sweep at 0, the middle and the
maximum of each control (the mapper's check), not a full map; the words one control writes can depend on
others (De-Esser word 17 follows its Filter switch). The mapper's emulator-free tables cover only plug-ins
whose words depend on few controls: on this PC the UA 1176LN Legacy passed the tables gate (788 s), the
Precision De-Esser (history-dependent) and Cambridge (coupled parameters) were refused. So Apollo Control
starts `plughelper.py` (the plugins venv, `-I -X utf8`) at the first parameter change and quits it after 5
idle minutes; the provider uses a `.tables` sidecar by itself where one exists. Startup and restore never
need it.

### 5.3 PluginHost: who may make the plug-in calls

The review found that with `PluginHost=1` the calls are user-level, so any local process could place
plug-ins. Decision, both parts built:

1. **Hosting is on only while Apollo Control's setting says so.** Turn on runs Apollo Control itself
   elevated once (`--plugin-admin`, UAC): `PluginHost=1`; `PluginHostImage` = its own DOS path
   (`C:\...`, from `QueryFullProcessImageNameW` in the elevated process itself, never from the request;
   a program not on a drive letter is refused); `PluginHostUser` = the requesting user's SID string (the
   unelevated app puts its own in the request, so an over-the-shoulder prompt still names the right
   user; checked as a SID, else the elevated process's own); and the module store filled with every
   mapped plug-in's verified modules. A failure writes none of it and leaves hosting off. Turn off: the
   inserts are taken down first, then `PluginHost=0` and both names removed. Trade-off: one
   administrator prompt each way, and the Settings toggle cannot be flipped silently by anything else.
2. **The driver names the caller** (FEAT_CALLER): BOARD_QUERY and PLUGIN_INSTANCE are taken only from an
   elevated administrator's handle (apollo-plug, step-p1.ps1), or from a handle used by the process that
   opened it when (b1) that process's primary token user is `PluginHostUser` (`PsReferencePrimaryToken`,
   `SeQueryInformationToken(TokenUser)`, compared as SID strings) and (b2) its image
   (`SeLocateProcessImageName`, an NT path) is `PluginHostImage` with the drive letter resolved at each
   call through `\GLOBAL??\X:` (`ZwOpenSymbolicLinkObject` / `ZwQuerySymbolicLinkObject`, the global
   namespace only). Anyone else: STATUS_ACCESS_DENIED before anything is locked or sent; any error fails
   closed. CAPS stays open; `Enabled` bit 1 says whether this caller may. Pure parts (`PlugImageDrive`,
   `PlugImageMatch`, `PlugSidTextOk`, `PlugSidTextEqual` in `plugin_plan.h`) are in plugin-plan-test.

What this proves (review findings 1 and 2, Nov 15): this program, started by this user, on the volume
its drive letter names now. Another account starting the same executable (even suspended, to inject into
it) fails b1; a stored `\Device\HarddiskVolumeN` that names another volume after a reboot can no longer
happen. What it cannot prove: that the program is unmodified in memory. A process of the same user can
still start it suspended or write into it; on a single-user studio PC that is the user's own session, and
no other account, program, script or stray tool can drive DSP 1. A moved exe or a new drive letter for its
volume needs Turn on again (Apollo Control says "not the host program").

### 5.4 Messages (`docs/apollo-control-units.md` 1.5)

UI to host, JSON strings: `plug-state`, `plug-watch`, `plug-unwatch`, `plug-list`, `plug-insert`
{input, name, stereo}, `plug-remove`, `plug-bypass` {on}, `plug-retry`, `plug-param` {input, index, value},
`plug-hosting` {on}, `plug-modules`, `plug-dir` {dir}. Host to UI: `plugins` (hosting, dsp, strips, maps,
inserts with parameters), `plugin-list`, `plugin-result` {op, input, ok, code, detail}.

### 5.5 Hardware test (after this build is installed and the x8p power-cycled)

1. Settings, Plug-in hosting, **Turn on**: one UAC prompt; then `apollo-plug caps` (unelevated) shows
   Enabled 1 with Operations 0 ("not the plug-in host"), Apollo Control's plug-in browser (Control view) shows HOSTING ON.
2. Picker: about 101 of 126 mapped plug-ins licensed (by name, the morning's answer: 99 UA-made
   plus Ampeg SVTVR Classic and Lexicon 480L). Insert **Precision De-Esser** on input 1: Running, the
   effect audible on input 1; the Control view's slot names it.
3. Turn **Thresh** and **Speed**: the first change takes a few seconds (planner start), later ones fast;
   each reads back; the sound follows. Watch `%LOCALAPPDATA%\Avium\ApolloControl\plughelper.log` for errors.
4. **UA 1176LN Legacy** on input 2 (its meter word row 10 word 5 skipped), **Lexicon 480L** staged (64
   pages; DSP 1 room near 300 words after it; then the picker offers nothing large).
5. **Stereo**: link inputs 1-2, Remove input 1's insert, insert Precision De-Esser stereo on 1-2: both
   inputs processed, left and right distinct, inputs 3-8 unchanged; Remove restores both plain strips.
6. **Bypass** on and off; **Remove**; **Replace**.
7. Quit Apollo Control from the tray: the plain strips are back (inputs sound dry). Start it again: the
   saved inserts return with their parameter settings (audibly the same).
8. Restart the PC with the x8p on (a warm start): the strips are adopted and the inserts come back by themselves
   (`apollo-diag --unit x8p strips`: `UPLOADS_PROVEN`); if the first upload went unanswered the browser says so and asks
   for an x8p power cycle, after which they come back.
9. Refusals: an unlicensed plug-in never appears; fill DSP 1 (Lexicon) and try another: "No room".
10. step-p1.ps1 still works elevated, and an unelevated `apollo-plug licence` is refused (ERROR_ACCESS_DENIED).
11. Settings, **Turn off**: inserts taken down, one UAC prompt, the tab says hosting is off.

## 6. Questions for Aaron (ask one at a time)

1. Which plug-ins he wants first on the x8p inputs (to order P2 after the gate plug-in).
2. Whether a Python runtime under his profile is acceptable for the helper (option A), or tables-only
   C++ (option B, fewer plug-ins).
3. Whether to update UAD on this PC to 11.8.3 for the 12 pinned-family plug-ins (Sphere Mic Collection
   among them), knowing the firmware rule.
