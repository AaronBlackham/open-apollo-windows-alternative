# Apollo Control: units and the combined device

Status (Nov 14 2026): UI and host are in the live tree. UI: `app\ui\units.js`, `units.css`,
`app\tests\units.js`, merged with the G2 multi-unit work in `app.js`. Host (`app\src\main.cpp`, G2):
every unit enumerated and sensed live, `units` sent, per-unit messages tagged, settings refused below
Mode 2, `agg-get`/`agg-set` answered "not available" (1.4). The driver IOCTLs (`AGG_*`, spec 6.9) are
not built yet. Design: [spec-twinx.md](spec-twinx.md) 6.3 (clock), 6.6 (ASIO), 6.7 (endpoints),
6.8 (routing), 6.9 (storage). Section 3.2 gives the Twin X channel order the UI shows as "expected".

**Where per-unit state lives (UI).** `app.js` handles every host message and owns the units:
`S.unitList` (the last `units` list, normalised) and `S.units[slot]` (`info`, readback `words` /
`rbValid`, `live`; empty slots are `undefined`). `units.js` reads those and keeps only its page
state (driver config, draft, drift, demo). `app.js` calls `Units.onUnits(prevList)` after each list
(notices, draft) and `Units.onAgg(m)`; the demo lists its units through `setUnitList`, the same path
as the host's.

## 1. Messages

### 1.1 Host to UI: `units`

Sent once the UI has said `hello`, again after every `resend`, and whenever the set of units or a
field below changes. The host builds it after each rescan, before that pass's per-unit messages, and
again at the end of the pass when a Mode, stage or `active` changed.

```json
{"type":"units","units":[
  {"id":"SN20000000000001","index":0,"model":"Apollo x8p","deviceType":34,"serial":"20000000000001",
   "fw":"11.15.54803","mode":2,"present":true,"active":true,"observe":false,"stage":5},
  {"id":"7C1E40A2D3F09B61","index":1,"model":"Apollo Twin X (Gen 1)","deviceType":153,"serial":"2253202500117",
   "fw":"11.15.54803","mode":0,"present":true,"active":false,"observe":true,"stage":1}]}
```

| Field | Meaning |
|---|---|
| `id` | the unit key of spec 6.9.1: HW_UID as 16 upper-case hex digits, else `SN` + serial. Stable across replugs; the configuration is keyed by it |
| `index` | the host's slot, the routing tag the per-unit messages carry (`"unit"`). Slot 0 is kept for the x8p, the unit the Control view drives; any other unit takes the lowest free of 1-3 and keeps it while present. Indexes can have gaps (slot 0 empty without an x8p, slot 1 empty after a unit left) |
| `model`, `deviceType` | display name (the driver's `ModelName` when it has one); 0x22 x8p, 0x99 Twin X Gen 1 |
| `serial`, `fw` | GET_INFO serial (valid part only) and firmware `major.minor.build` |
| `mode` | `Units\<key>\Mode`: 0 observe, 1 bring-up, 2 full; `null` if unknown (a unit the driver did not accept) |
| `observe` | the driver does not run the unit (identified and quiesced). Read-only, like any `mode` below 2: the host sends it no setting |
| `stage` | the unit's bring-up stage (`status.info.stage`) |
| `present` | `false` means it is leaving; the UI treats it as gone |
| `active` | the unit is running audio: stage connected and Mode 2 |
| `inputs`, `outputs` (optional) | the descriptor once the driver reports it: arrays of `{"id":0x0901,"name":"MON L"}` in slot order. Without them the UI uses the model table and marks Twin X names "expected" |

Rules: a slot not in the list (or listed `present:false`) is gone: the UI drops its Status section and
live state. A `status` with `present:true` opens its slot even before the next list names it;
`status` with `present:false` closes it at once. `readback` and `live` for a closed slot are dropped
(late messages from a unit that left). A host that never sends `units` keeps today's behaviour: one
unit, index 0.

### 1.2 Per-unit messages

`status`, `readback`, `levels` and `live` carry `"unit": <index>`. Untagged means index 0, as today.

| Message | Index 0 | Other indices |
|---|---|---|
| `status` | `S.host`: Device, the x8p's Status section; the top bar, banner and Control view while the x8p is selected | `S.units[i].info`: the unit's Status section (model, Mode, observe note) and Units row (rate); the top bar, banner and Control view while it is selected |
| `readback` | `S.ctl[0]` (the x8p's controls, also its Status rows), and its words for the clock column | `S.ctl[i]` (its controls, kept while another unit is shown), its Status preamp rows (bits as spec 4.1, preamp count from `numPreamps` or `MODEL_CAPS`) and clock lock (word 6 bit 31, valid with bit 30). None for an observe unit |
| `levels` | `S.ctl[0]`: the Control meters while the x8p is selected, and the Channels view | `S.ctl[i]`: the Control meters while it is selected |
| `live` | per unit (already tagged) | same; frames/s feeds the drift column |

### 1.2a The Control view shows every unit (segments, no tabs)

Every present unit of a known model (`MODEL_CAPS`: x8p, Twin X) gets a segment on the Control view, side by side: its
head (name, model, clock, sample rate, DSP room), its strips in signal-flow order (preamp, plug-in insert, monitor-mix
fader with the input meter), a talkback strip and a master section (monitor level, MUTE, DIM, main out L/R meters,
MUTE ALL). Every control sends to its own unit (`app.js` `segs`, `syncSegments`); Twin X controls are named "Twin X
input 1 gain" and so on. A unit that leaves greys out in place until it returns or is hidden. `S.sel` is the focused
unit: the monitor column (source, speakers, headphones, ALT, talkback), the top bar, the banner, presets, the tray menu
and the M/D/O keys follow it; the monitor column's unit buttons (`#monUnits`) or a segment's name focus another. Each
unit keeps its own state in `S.ctl[slot]` (`S.inputs`, `S.links`, `S.mon`, `S.pending` read the focused one). The
focus is saved as `cfg.controlUnit` (the unit id) and wins whenever that unit is present; otherwise the x8p's slot 0,
else the first present unit. `--view control:twinx` (or `control:x8p`, `control:<slot>`; `?view=` in a browser) asks
for a unit until the user picks one. Focusing sends nothing.

Settings carry the slot as a last field, slot 0 untagged as before: `input\t<ch>\t<param>\t<value>\t<unit>`,
`monitor\t<param>\t<value>\t<unit>`. The Twin X (spec-twinx 4.1, `driver/preamp_plan.h`): inputs 1-2,
link on 1-2 only, Hi-Z impedance on input 1 as param `hizimp` (host `APOLLO_PARAM_HIZ_IMP` 0x11, codes
0-7 = 2.2M 1M 453k 232k 100k 70k 47k 21k; no readback field, so the app keeps the last value per unit
in `cfg.unitPrefs[id].hizImp`), monitor level, mute, dim, mono, talkback and the headphone level (digital
on the Twin X); no dim level (its driver plan refuses it). Its input names live in
`cfg.unitPrefs[id].channels` (double-click a strip name); `cfg.channels` stays the x8p's.

**[Monitor] ALT speakers, sources, cues, talkback** (`driver/monitor_plan.h`, `MODEL_CAPS` `cues`, `alts`,
`hps`). The monitor column has a Speakers choice (MAIN plus each ALT that is on; `altsel`, confirmed by
readback word 6 [29:28]), the Source per model (x8p MIX, CUE 1-4; Twin X MIX, HP, LINE 3/4), HP sources
CUE 1 / CUE 2 on both units (the Twin X: one), an ALT speakers card (`alt<n>` on, `alt<n>trim` raw
60 + 2*dB; x8p Line 1-2 / 3-4, Twin X Line 3/4) and "Dim the monitor while talking" (`cfg.talkbackDim`:
DIM goes on with talkback, from here or the unit's button, and off again after). The **Cues** tab
(Ctrl+3) shows one cue at a time: a send level and pan per input and the talkback (`send:<cue>:<bus>`,
`pan:<cue>:<bus>`, half-dB, bus = input - 1 or 10), and the cue's source (CUE/MIX, `cue<n>mix`), MONO,
MUTE and where it is heard. None of these read back, so they live in `cfg.mixes[x8p | unit id]` and go
out again once per connection, mutes first. Refusals show under the Cues page's selector too. The host
side of these names is `app/src/monitor_cmds.h`.

**[Mix] Monitor mix** (`driver/monitor_plan.h` APOLLO_MON_MIX_*, `driver/cues.c`). On every strip of every segment: a
console fader (-inf..+12 dB, half-dB; double-click 0 dB, Ctrl/Alt/Shift-drag fine), a pan (none for the talkback: its
weight is one value on both sides), M and S, and a talkback strip; MUTE ALL in the master section (one driver call,
`mixmuteall`). Names: `mix:<bus>` (half-dB, -121 off), `mixpan:<bus>`, `mixmute:<bus>`, `mixsolo:<bus>`, `mixmuteall`, bus
= input - 1 or 10. Solo is the driver's (it keeps the mutes apart and rebuilds the soloed mix after a reconnect); the
strips a solo or mute silences are greyed. A linked pair (readback link bits) is one stereo channel: its first input's
fader, M and S drive both buses, each input keeps its own pan; LINK pressed here sets the pans L100 / R100 and copies input
1's level, mute and solo, as UA's engine does; unlink puts both pans back to the centre. No readback for the
coefficients: an untouched channel shows UA's default (0 dB, centre) and nothing is sent for it; what was touched lives in
`cfg.mixes[x8p | unit id].main[bus]` and goes out once per connection, mutes and solos before anything else. The master
section's main out L/R meters are the unit's monitor-mix loopback (record slots 26-27, or the live list's ApolloMonIn
row), the level the speakers get before the monitor knob. M, S and MUTE ALL are `role="switch"`, so they stay apart from
the monitor's MUTE and the TALKBACK button. Tests: `app\tests\mix.js`.

**Refusals.** A host `error` with code 1, 50, 87 or 4317 (the host's read-only refusal) is shown inline
over the segments for that unit ("Twin X input 1 gain was not changed: not supported by the installed
driver on the Apollo Twin X (Gen 1)."), grouped by reason with Dismiss, the control marked red and put
back to what the readback says (or the value before the write where there is no readback). Other codes
(21 not ready, 170 busy) stay toasts. A refusal for a unit not on screen (it left, or another view is shown) also toasts. A unit whose driver
reports `numPreamps` 0 (the G6 build on the Twin X) gets a note up front that input settings will be
refused. A unit below Mode 2 shows a read-only banner, is dimmed and is sent nothing.

### 1.5 [Plug-ins] UAD plug-in inserts on the x8p and the Twin X (docs/PLUGINS.md 5, 4.1c)

On the Control view (`app/ui/plugins.js`; no Plug-ins tab since Nov 15 2026): the browser docked beside the strips, the
insert slot on every strip of every unit, drag and drop (and the same by keyboard), the parameter drawer under the
strips. The host's plug-in thread (`app/src/plugin_engine.h`) owns every device call.

**[P7] Per unit (Nov 15 2026).** Plug-ins are per unit, keyed by the host slot (the `unit` index of section 1.1):
slot 0 is the x8p, a Twin X Gen 1 is the slot the worker gave it. Every `plug-*` command takes an optional `"unit":
<slot>` (absent = 0, the x8p, as before); every `plugins`, `plugin-list` and `plugin-result` carries `"unit": <slot>`
(it was always 0). Per unit: its own insert list, saved inserts (by unit id, as before), its own licence answer (the
picker of `plugin-list` for that slot lists what that unit is licensed for), its own DSP room (`dsp`), strips and
hosting state. **Opt-in:** the host sends messages for slots other than 0 only after the page says it reads `unit`:
`"units":true` on any `plug-*` command (the UI sends `{"cmd":"plug-state","units":true}` at hello and `units:true` on
`plug-state`, `plug-watch` and `plug-list` whenever the Control view shows), so a page from before P7 never takes a
Twin X snapshot for the x8p's. `plug-hosting`, `plug-modules` and `plug-dir` are global (one driver setting, one module
store, one maps folder): send them without a unit (their `plugin-result` comes back on the slot sent, 0 by default).
`plug-list` asks the named unit's licence fresh and lists every hosting unit (one `plugin-list` per slot). The host sends
`plugins` for slot 0 always and for any other slot once a unit that hosts was seen there (`present:false` when it leaves).

New fields of `plugins` (all slots, the x8p too), so the UI needs no model table of its own:

| Field | x8p | Twin X Gen 1 |
|---|---|---|
| `model` | `"x8p"` | `"twinx"` (`null` for a slot without a unit that hosts) |
| `inputs` | 8 (input indexes 0-7) | 2 (0-1) |
| `pairs` | `[0,2,4,6]`: the first input of each stereo pair | `[0]`: the pair 1/2 only |
| `hiz` | `[0,1]`: inputs whose Hi-Z jack shares the input's strip and insert | `[0]`: Hi-Z is input 1 (no separate slot) |
| `supported` | `true` | `false` only when the installed apollo.sys answers without FEAT_UNITS (it hosts on the x8p only); then the slots say why (insert `code` `unit`). The talkback has no strip: no slot |

What the UI does with them: a unit's slots take drops once its slot's `plugins` says `supported` (absent counts as true:
a host before P7 sends slot 0 only); otherwise, and for a unit with no `plugins` at all, they read "not supported on
this driver build". One slot per input below `inputs`; a linked pair takes a stereo insert when its first input is in
`pairs` (the Twin X link is 1-2). Each unit's `plugins` / `plugin-list` decide its licences and its DSP room (shown in
its segment's head and, for the unit being placed on, the browser).

UI to host, JSON strings: `plug-state`, `plug-watch` / `plug-unwatch` (refresh each second while the Control view is
shown), `plug-list` (catalogue and a fresh licence answer), `plug-insert` `{unit, input, name, stereo}` (input = the
pair's first for stereo; replaces what runs there), `plug-remove` `{unit, input}`, `plug-bypass` `{unit, input, on}`,
`plug-retry` `{unit, input}`, `plug-param` `{unit, input, index, value}` (the integer value 0..max; the host plans only
the last of a burst per parameter), `plug-hosting` `{on}` and `plug-modules` (an administrator prompt), `plug-dir`
`{dir}` (the maps folder, '' = found by itself). A move is the UI's own sequence: `plug-insert` on the new input, then,
once that insert runs, one `plug-param` per parameter that differs, then `plug-remove` of the old one (on its own unit).

Host to UI:

```json
{"type":"plugins","unit":0,"present":true,"unitId":"SN20000000000001",
 "model":"x8p","inputs":8,"pairs":[0,2,4,6],"hiz":[0,1],"supported":true,
 "hosting":{"driver":true,"on":true,"caller":true,"stereo":true,"staged":true,"callerRule":true,"why":"enabled","admin":""},
 "dsp":{"index":1,"room":13386,"total":16384,"largest":13386,"estimate":false,"quarantined":false,"owned":true,"admitted":true,"load":1989,
   "pools":[{"total":16384,"used":2998,"free":13386,"largest":13386,"scratch":0},{"total":43008,"used":3186,"free":39822,"largest":30000,"scratch":448},
            {"total":8192,"used":512,"free":7680,"largest":7680,"scratch":192},{"total":16777216,"used":4096,"free":16773120,"largest":16773120,"scratch":0}]},
 "strips":{"live":true,"started":255,"warm":false,"state":4},
 "maps":{"ok":true,"dir":"...\\plugins","configured":"","why":"","python":true,"catalog":"ready","progress":[251,251]},
 "inserts":[{"input":0,"stereo":false,"bypass":false,"name":"UAD Precision De-Esser","label":"Precision De-Esser",
   "state":"running","code":"","why":"","busy":"","latency":0,"codeWords":2004,"pages":1,
   "params":[{"name":"Thresh","max":32768,"value":0,"text":"0.00 dB","steps":null},
             {"name":"Speed","max":1,"value":1,"text":"Fast","steps":["Slow","Fast"]}]}]}
{"type":"plugin-list","unit":0,"state":"ok","why":"","licence":"ok","licenceWhy":"","mapped":126,"authorized":101,
 "items":[{"name":"UAD Precision De-Esser","label":"Precision De-Esser","mono":true,"stereo":true,"latency":0,
   "codeWords":2004,"dataWords":310,"renderId":2105,"pages":1,"params":7,"licensed":true,
   "category":"Special Processing","tags":["Special Processing","Dynamics"],"art":"https://uad-art.apollo/UAD%20Precision%20De-Esser.png"}]}
{"type":"plugin-result","unit":0,"op":"insert","input":0,"ok":false,"code":"room","detail":"no room on DSP 1 ..."}
```

Added Nov 15 2026 (additive; a UI treats a missing field as empty): `dsp.pools` (the ledger's four pools, `[]` without
the ledger: the UI then uses room/total for pool 0); per item `dataWords`, `renderId`, `category` (UA's own
`primary_category` from the local UAD install's `Manifest\<name>\manifest.json`, else the first `UA.xml` category that
lists the plug-in, else ''), `tags` (the manifest's `categories`) and `art` (the manifest's `thumbnail`, or `<name>.png`,
when that file is in the install's `Plug-In Icons`, as `https://uad-art.apollo/<percent-encoded file>`; else ''). Apollo
Control maps the virtual host `uad-art.apollo` to that folder read-only (WebView2, deny-CORS: images only); nothing is
copied, nothing UA runs, nothing goes on the network. The manifests and `UA.xml` are read as plain files on the plug-in
thread, again whenever the catalogue is rebuilt.

**Types in the browser** (`plugins.js` `GROUPS`, `categorize`): UA's words map to EQ, Dynamics (compressors, limiters,
de-essers), Preamp & channel strip, Reverb, Delay & echo, Modulation, Guitar & bass amps, Tape & saturation, Mastering,
Special & other. "Delay and Modulation" splits by the tags; a generic "Special Processing" takes a more specific tag
(the De-Essers file under Dynamics). Without UA data the name rules `NAME_RULES` decide (amp, reverb, delay, modulation,
EQ, dynamics, preamp, tape, mastering words, in that order), else Special & other.

**[Warm] (Nov 15 2026, warmplug)** `dsp` gains `warm` (the driver owns the strip DSP through a warm start's adoption or
rebuild), `proven` (that DSP acknowledged an upload in this warm session) and `ownWhy` (while `owned` is false: the
driver's reason, `apollo_plugin.h` APOLLO_PLUGIN_WHY_*, in words ending with the one action, "power-cycle the x8p (off,
10 s, on): its saved inserts come back after it"; '' while owned). The UI takes `owned` as the gate (no longer
`strips.warm`): a warm start whose strips were adopted places and restores inserts as a cold start does. A host without
`ownWhy` keeps the old rule (`strips.warm` blocks). While the driver's warm check has not run yet the insert waits with
code `strips` ("the x8p is starting: the driver checks its input strips first"); every other reason is code `warm`.
After a chain runs the host sends the driver its record path (op LATENCY), so an over-budget chain moves the DAW's input
latency (`kAsioLatenciesChanged`).

Insert `state`: running, loading (`busy` names the step), bypassed, waiting (it cannot go in now: `code` warm, absent,
strips, hosting, caller, [Rate] `rate`: the unit is not at 48 kHz, [P7] `unit`: the driver hosts on the x8p only) or failed (`code` licence, room, quarantine, modules, readback, maps, staged, stereo, busy,
driver). `licensed` is null while the licence is unknown (hosting off, no answer). Refusals (`plugin-result` with ok
false) show in the browser's notes and mark the slot (a toast when the Control view is not shown); the drawer shows a
waiting or failed insert's reason with Try again. Tests: `app\tests\plugins.js`.

**[PlugSetup] Set up plug-ins (Nov 16 2026, a release install).** Global, no unit. UI to host: `plug-setup-state` (sent
with hello and when the Control view shows; a unit that hosts for this program answers its licence query first),
`plug-setup-start` `{all}` (false: the plug-ins the present units' licence answers admit, true: every plug-in
open-apollo can map; the host passes `-Authorized all` anyway when no unit has answered) and `plug-setup-cancel`.
Host to UI, whenever it changes (each second while a setup runs):

```json
{"type":"plugin-setup","offered":true,"available":true,"uad":true,"dir":"C:\\Users\\...\\AppData\\Local\\Avium\\ApolloPlugins",
 "inUse":"","configured":"","dev":false,"ready":false,"running":true,"licensed":37,"licUnits":1,"mappable":119,"cpus":8,
 "hosting":true,"why":"",
 "status":{"state":"running","phase":"map","text":"...","percent":42,"done":3,"total":12,"eta":300,"code":"","error":"",
   "summary":"","modules":"","mode":"licensed","log":"...\\setup\\setup.log",
   "phases":[{"id":"check","label":"Checking this PC","state":"done","detail":"..."}]}}
```

`offered`: no other maps folder is in use or set (a developer layout, `dev`, or a folder set in Settings is left alone);
`available`: the setup script is installed next to the exe; `uad`: UAD's plug-in folder and `UAD2SDK.dll` exist (by
name); `ready`: maps and their Python are in use; `licensed`: how many render ids the units' answers admit (-1: no
unit answered); `mappable`: how many of open-apollo's known-good plug-ins the UAD install has; `status`: the script's
`setup\status.json` (state running, done, failed, cancelled, or `stopped` when nobody runs it any more; `code` nouad,
network, hash, space, python, packages, specs, store, licence, map, modules, cancelled). The browser shows a box while
plug-ins are not set up (or a setup runs or stopped) and Settings a row; both open the explanation dialog before
`plug-setup-start`. Without maps, `plug-hosting` `{on}` now turns hosting on without modules (so the licence answer can
choose what the setup maps). Tests: `app\tests\plugin-setup.js`.

### 1.6 [Rate] Sample rate (44.1-192 kHz), per unit and on the combined device

The host side is `app/src/rate_host.h` (routed from `main.cpp`, two marked hunks); the driver side is
`driver/rate.c`, `include/apollo_rate.h`. The UI side is `app/ui/app.js` [Rate] (each segment's head, a
shared bar over the segments for the combined device, the Device view). The current rate is already in every `status` (`info.sampleRate`); the
messages below add what can be chosen and the change itself.

UI to host, JSON strings like `agg-*` and `corr-*` (`postMessage(JSON.stringify(...))`):

```json
{"cmd":"rate-get"}
{"cmd":"rate-get","unit":"000000002F00BEEF"}
{"cmd":"rate-set","unit":"000000003A00C0DE","rate":96000,"combined":false}
{"cmd":"rate-set","unit":"000000003A00C0DE","rate":44100,"combined":true}
```

`unit` is the `id` of the `units` list (HW_UID as 16 hex digits, or `SN<serial>`). `rate-get` without
`unit` answers for every unit. `rate` is in Hz: 44100, 48000, 88200, 96000, 176400 or 192000.
`combined:true` changes every unit of the combined device (the timeline unit, normally the master,
first; then the followers); a unit that runs in the combined device with another unit refuses a plain
`rate-set` (code `combined`), so the selector sends `combined:true` whenever `combined.member` is true.

Host to UI:

```json
{"type":"rate","unit":"000000003A00C0DE","present":true,"rate":48000,"locked":true,"changing":false,
 "offered":[44100,48000,88200,96000,176400,192000],"allowed":[44100,48000,88200,96000,176400,192000],
 "stored":48000,"asio":false,"streams":true,"plugins48k":false,"correctionMismatch":false,"stripsRebuilt":false,
 "combined":{"member":true,"rate":48000,"allowed":[44100,48000,88200,96000,176400,192000],"units":2,
             "stored":48000,"asio":false,"mismatch":false},
 "last":{"code":"","why":"96 kHz, locked in 0 ms","ms":2410,"changes":1},
 "frames":{"frameCfg":8,"lead":144,"guard":16,"adc":10,"inPath":264,"dac":43}}
{"type":"rate","unit":"...","present":false,"error":2,"detail":"that unit is not on apollo.sys right now"}
{"type":"rate-result","unit":"000000003A00C0DE","rate":96000,"combined":false,"pending":true,"code":"","why":"Changing to 96.0 kHz","ms":0}
{"type":"rate-result","unit":"000000003A00C0DE","rate":96000,"combined":false,"pending":false,"ok":true,"code":"","why":"96 kHz, locked in 0 ms","ms":2410}
```

| Field | Meaning |
|---|---|
| `rate`, `locked` | what the unit runs at now, and whether its clock reports a lock (0xC07C) |
| `changing` | a change is under way on it (or on the combined device): show progress, send nothing |
| `offered` | the rates the unit reports (its clock-source word); all six on the x8p and the Twin X |
| `allowed` | what `rate-set` may ask for this unit alone now ([Clock] on a S/PDIF or ADAT clock, 1.10, it stops at 96 kHz; on Internal and Word Clock it equals `offered`) |
| `stored` | the rate the unit comes up at next time (the driver stores every successful change) |
| `asio` | an ASIO host (a DAW) has this unit: a change from here is refused (`asio`); the DAW changes it itself |
| `streams` | Windows apps have it open: they get a short gap while Windows re-opens them at the new rate |
| `plugins48k` | the x8p is not at 48 kHz: plug-in inserts wait (their `plugins` state says `waiting`, code `rate`) and come back by themselves at 48 kHz |
| `correctionMismatch` | speaker correction holds a profile made for another rate and is stopped: offer to load the profile again (`corr-load` compiles it at the unit's rate) |
| `stripsRebuilt` | the last change rebuilt the x8p's input strips: the analog inputs are back a moment after the change |
| `combined` | `member`: this unit runs in the combined device with another unit (set the rate there, `combined:true`); `rate`: the timeline's; `allowed`: what every unit allows; `asio`: "Apollo (Avium)" is open in a host; `mismatch`: a unit runs at another rate and is muted until the rates agree |
| `last` | the last change on this unit: `code` (below, "" = done), `why` (one line, show it as is), `ms` it took |
| `frames` | at this rate: AX_FRAME_CFG, the engine's lead and guard, the ADC delay (Status/Device rows); [Lat] (Nov 15, warmplug; 0 from an older driver) `inPath` (strips running: the strip DSP's block 64/128, the strips' record delay 200, an over-budget chain's excess; else 0) and `dac`: reported input = guard + adc + inPath, output = lead + frameCfg + dac, plus the buffer (docs/ASIO.md, Latency) |

`rate-result` codes: `rate` not one of the six, `unsupported` not offered, `not-ready` the unit does not
run in Mode 2, `asio` a DAW has the unit, `combined` set the combined rate instead, `combined-asio` a DAW
has "Apollo (Avium)", `busy` another change is under way, `lock` the clock did not take the new rate (the
unit stays at the old one; `why` says which), `hardware` the unit stopped answering, `no-combined` no
combined device runs, `partial` the timeline unit changed and a follower did not (its channels stay muted:
show `why`, offer to try again), `absent`, `driver` (an apollo.sys without rate change: 48 kHz only),
`error`.

Behaviour the UI implements:

- **Where.** A selector in each unit's segment head on the Control view and on the Device view, showing
  `rate`; the rates in `offered`, those not in `allowed` (or `combined.allowed` for a member) disabled. With
  `combined.member` true, one selector for the combined device (`combined.rate`) over the segments and on the
  Device view, each member's head saying SHARED. No answer within 1.5 s, or a `rate` with `error` (an
  apollo.sys without rate change): the selector stays disabled at the unit's own rate, "48 kHz (fixed in
  this driver build)".
- **Changing.** On `rate-set` the host answers `pending:true` at once and the final `rate-result` 2-3 s
  later per unit (a combined change of two units about 5-6 s). Disable the selector while `pending` or any
  unit reports `changing`; the meters may stall for those seconds (the driver holds the unit's lock while
  its clock moves). After the final result the host sends a fresh `rate` for every unit; also send
  `rate-get` on the first `units` list and whenever a `status` brings another `sampleRate` (a DAW or
  apollo-diag changed it).
- **Before the change, say what it does** (one line, no dialog): Windows apps playing through the unit
  pause briefly while Windows re-opens them; a DAW using the unit must change the rate itself (refused
  here while it is open); plug-in inserts are off at rates other than 48 kHz; at 44.1 kHz the combined
  device's resampled channels are clean to about 16 kHz (18 kHz at the "high" resampler quality), at every
  other rate to 20 kHz.
- **After.** `correctionMismatch` true: a notice on the Speakers page with "Load the profile again".
  `plugins48k` true: the plug-in browser and every slot of that unit say "Plug-ins run at 48 kHz only" (the
  inserts' `code` is `rate`); a drop there is refused with that line.
- **Errors.** `rate-result` with `ok:false`: inline under the selector with `why` (it is already worded);
  `busy` and `changing` are toasts. The demo (`--demo`) may simulate a change: `pending`, then `ok` after
  2 s, `rate` updated.

What happens underneath (for the wording): the driver stops the unit's transport, moves the clock in the
kext's order (clock word, acknowledgement, the firmware's rate, every DSP told, 1.5 s hold), starts the
transport again and checks the lock; Windows endpoints switch to the new format (ACX format change) and
streams opened at the old rate play silence until Windows re-opens them; ASIO hosts get
`kAsioLatenciesChanged` (their own SetSampleRate) or a reset request (the combined driver); the input
strips are rebuilt at the new rate; the rate is stored per unit (`Units\<key>\SampleRate`) and for the
combined device (`Parameters\Aggregate\SampleRate`, plus absent units' keys). Leaving 176.4/192 kHz waits
until the clock has been there 3 s, and a 176.4/192 kHz rate from a 176.4/192 kHz clock runs a short
48 kHz session first (open-apollo's x4 findings), so those changes take up to 3 s longer.

Tests: `app\tests\rate.js` (the UI against a stub host); the host side is exercised by `apollo-diag rate`
and the driver suites (`tools\rate-test`).

### 1.7 [Endpoints] Windows endpoints: which ones exist (per unit, and the combined set)

The host side is `app/src/endpoints_host.h` (routed from `main.cpp`, two marked hunks: the `endpoints-*`
route and the elevated `--endpoints-admin` step); the driver side is `driver/endpoint_plan.h` (the
standalone tables), `driver/apollo_agg.h` `APOLLO_AGG_EP_LIST` (the combined set), `driver/circuit.c`, and
`IOCTL_APOLLO_GET_ENDPOINTS` version 2 (`include/apollo_ioctl.h`). The UI adds the checkboxes (one per row,
a "Defaults" button); nothing here needs the UI to know the tables: every row comes with its name.

What Windows gets (what UA's own driver gives Windows users): every physical output pair of a unit is its
own playback endpoint, named by its jack; every mic/line input a mono recording endpoint; the monitor-mix
loopback (never a default device). VIRTUAL and S/PDIF inputs exist but are off by default.

| Set | Rows (default set in bold) | Windows names |
|---|---|---|
| x8p standalone | **MON L/R, LINE 1-2, LINE 3-4, LINE 5-6, LINE 7-8, HP 1, HP 2, S/PDIF** out; **Monitor mix, MIC/LINE 1-8** in; VIRTUAL 1-2..7-8, S/PDIF in | `LINE 1-2 (Apollo x8p)` |
| Twin X standalone | **MON L/R, LINE 3-4, HP** out; **Monitor mix, MIC/LINE 1-2** in; VIRTUAL 1-2, 3-4, S/PDIF in | `HP (Apollo Twin X)` |
| combined (x8p hosts, `Parameters\Aggregate\EndpointSet` 1) | **Monitor** (follows the `windows` routes), **every jack above** prefixed with its unit, **Monitor mix**, **every mic/line input**; the VIRTUAL and S/PDIF inputs of both | `x8p LINE 1-2 (Apollo)`, `Twin X MON L/R (Apollo)` |

UI to host, JSON strings like `rate-*` (`postMessage(JSON.stringify(...))`):

```json
{"cmd":"endpoints-get"}
{"cmd":"endpoints-get","unit":"000000003A00C0DE"}
{"cmd":"endpoints-set","unit":"000000003A00C0DE","rows":[0,1,2,3,4,5,6,7,8,9,10]}
{"cmd":"endpoints-set","unit":"000000003A00C0DE","mask":8388607}
{"cmd":"endpoints-set","unit":"000000003A00C0DE","default":true}
```

`unit` is the `id` of the `units` list. `endpoints-get` without `unit` answers for every unit. `endpoints-set`
takes the rows to have (`rows`, row numbers of the `endpoints` message), or the same as a bit mask
(`mask`, bit i = row i), or `default:true` (the registry value is deleted: the driver's default set). It
always changes the set the unit shows now: the combined set when `set` is `combined` (the x8p), else the
unit's own. A Twin X whose `set` is `none` (the combined set runs on the x8p) has nothing to change: use
the x8p's rows.

Host to UI:

```json
{"type":"endpoints","unit":"000000003A00C0DE","present":true,"model":"x8p","set":"combined","combinedOn":true,
 "count":31,"mask":8388607,"defaultMask":8388607,"stored":null,"storedIn":"default","next":8388607,
 "restartNeeded":false,"version":2,
 "rows":[{"row":0,"name":"Monitor","circuit":"Ap2CMon","render":true,"channels":2,"model":null,
          "enabled":true,"added":true,"default":true,"next":true,"open":true,"running":true,"noSlot":false},
         {"row":2,"name":"x8p LINE 1-2","circuit":"Ap2CX8pLine12","render":true,"channels":2,"model":"x8p",
          "enabled":true,"added":true,"default":true,"next":true,"open":false,"running":false,"noSlot":false}]}
{"type":"endpoints","unit":"000000002F00BEEF","present":true,"model":"twinx","set":"none",...,"rows":[]}
{"type":"endpoints","unit":"...","present":false,"error":2,"detail":"that unit is not on apollo.sys right now"}
{"type":"endpoints-result","unit":"000000003A00C0DE","pending":true,"code":"","detail":"asking for administrator rights to store the endpoint set","restartNeeded":true}
{"type":"endpoints-result","unit":"000000003A00C0DE","pending":false,"ok":true,"code":"","detail":"stored; Windows shows the new set after the unit's next start (power cycle, driver update, or disable/enable)","restartNeeded":true}
```

| Field | Meaning |
|---|---|
| `set` | which table the unit shows now: `standalone` (its own), `combined` (the x8p hosting both units), `none` (no endpoints: the Twin X while the combined set runs, or a unit below Mode 2) |
| `combinedOn` | `Parameters\Aggregate\EndpointSet` is 1 now (the combined set after the next start of both units) |
| `mask` | the rows Windows has now (bit i = row i), from the unit's last start |
| `defaultMask` | the rows of the driver's default set (the `default` flags) |
| `stored`, `storedIn` | the registry value the next start reads (`null` = none: the default set) and where it is: `unit` (`Units\<key>\Endpoints`), `model` (`Models\<TT>`), `global` (the x8p's legacy `Parameters\Endpoints`), `Aggregate` (the combined set), `default` |
| `next` | the rows the next start will have (`stored`, else `defaultMask`) |
| `restartNeeded` | `next` differs from `mask`: say "applies after the unit's next start" |
| `rows[]` | `name` (the pin name: Windows shows `<name> (<device>)`), `circuit` (the reference string), `render` (playback), `channels`, `model` (whose jack: `x8p`, `twinx`, `null` = the combined Monitor rows, which follow the routing), `enabled` (in `mask`), `added` (Windows has it now), `default`, `next` (in `next`: the checkbox state), `open` / `running` (an app has it open / plays or records on it now), `noSlot` (the unit's channel layout has no such channel at the current rate: it is silent) |

`endpoints-result` codes: `""` done, `cancelled` (the administrator prompt was declined: nothing changed),
`elevation` (the elevated step did not start), `registry` (the value could not be written), `busy` (another
change waits for its prompt), `absent`, `none` (the unit shows no set of its own), `unit`, `mask`.

Notes for the UI:
- The change needs administrator rights (one UAC prompt per `endpoints-set`): the endpoint set is a
  registry value under the driver's service key, which only administrators can write.
- It applies when the unit starts again. The host does not restart the unit: an app playing through one of
  its endpoints would make Windows ask for a reboot. Offer the hint instead ("power-cycle the Apollo, or
  restart it from Device Manager, after closing apps that play through it").
- Unchecking a row an app has `open` is allowed (it goes away at the next start); say which app-facing
  endpoint the user is about to lose if `open` is true.
- The monitor-mix loopback rows are never offered as Windows defaults (INF NeverSetAsDefault); the others
  can become the default when Windows first sees them: the install scripts put the previous defaults back.

Older drivers: a version-1 driver (`version` 1) has at most 16 rows and the old row numbers (stored as
`EnabledEndpoints` / `Aggregate\EndpointMask`, which this driver ignores); the host shows its rows but an
`endpoints-set` against it writes the new value the old driver does not read.

UI: Settings, **Windows audio** (`app/ui/endpoints.js`): one block per unit in units-list order, a checkbox
per row (checked from `next`) under Playback (outputs) and Recording (inputs). The boxes are a draft until
**Apply** (one `endpoints-set` with `rows`, so one UAC prompt); **Undo changes** drops the draft; **Use defaults**
sends `default:true`. Words beside a row only where it differs from the plain case: "Added / Removed at next
start" (box vs `added`), "Off", "Playing" / "Recording" / "Open in an app" (`running` / `open`), "Silent at this
rate" (`noSlot`), "Follows the routing" (`model` null, playback), "Not in the defaults". Unchecking a row an app
has open asks first and names it. A Twin X with `set` `none` says its endpoints are part of the combined set
(change them on the x8p); `present:false` shows the host's `detail`; a `version` 1 driver shows its rows with
Apply disabled. `endpoints-get` goes out when Settings opens (at most every 3 s) and after every final
`endpoints-result`; with no answer in 4 s the panel says the driver does not report endpoints. The demo answers
with this section's tables (the combined set while the demo runs combined) and stores nothing.

Tests: `app\tests\endpoints.js` (stub host: get on open, boxes from `next`, the draft, `endpoints-set` rows,
pending / cancelled / stored, the open-endpoint warning, `default:true`, the Twin X under the combined set, a
unit not there). The driver's tables are checked offline by `tools\agg-test\aggep-test` (rows, default sets,
INF interfaces, where each combined row plays and records) and on hardware by `apollo-diag endpoints` (all
rows, set, slots).

### 1.8 [Themes] Lab's Avium Measure hand-off: `measure-open`

The Lab theme's measurement panel (`app/ui/signatures.js`) posts the current routing:

```json
{"cmd":"measure-open","routing":{"format":"apollo-routing","version":1,"created":"2026-11-15T14:00:00.000Z",
 "app":"Apollo Control 1.x","combined":true,"focus":{"slot":0,"input":3},
 "units":[{"slot":0,"model":"Apollo x8p","serial":"...","id":"000000003A00C0DE","rate":48000,
   "inputs":[{"ch":1,"name":"Vox","gain":38,"line":false,"p48":true,"pad":false,"lowcut":false,"phase":false,"insert":""}],
   "mainOut":{"loopbackSlots":[24,25],"mute":false,"dim":false,"levelDb":-18}}]}}
```

The host (`main.cpp` `MeasureOpen`) writes the message as it came to
`%APPDATA%\Avium\ApolloControl\measure-handoff.json`, then starts Avium Measure through its App Paths
registration (`AviumMeasure.exe --apollo-routing "<file>"`, no UI on failure, nothing installed, no network)
and answers `{"type":"measure-result","ok":<written>,"launched":<started>,"file":"<path>","why":"<text>"}`.
In the demo (no host) the UI saves the same JSON as `apollo-routing.json`. Avium Measure does not read the
file yet: its side (an `--apollo-routing` import) is still to be written.

### 1.9 [Ins] Tracking: four inserts per input, UAD REC / MON, the Unison slot, the aux buses

Back end built Nov 15 2026 (`builds\work-ins`, merged into the live tree the same day), offline tests only, **nothing of it run on hardware**. Driver:
`driver/plugin.c`, `plugin_plan.h`, `unison_plan.h`, `preamp_plan.h` (field map), `monitor_plan.h` + `cues.c` (aux),
`mixer.c` (the lease's write guard); `include/apollo_plugin.h` (Unison ops, aux synch, feature bits),
`include/apollo_ioctl.h` (aux codes). Host: `app/src/plugin_host.h` (BuildChain = upstream's `compose()`,
TapPlan, BuildUnisonChain, plugins.json version 2), `app/src/plugin_engine.h` (chains), `app/src/monitor_cmds.h`
(aux names). Everything below is additive: a page that sends only the section 1.5 messages keeps working (one slot per
input, `inserts` as before).

#### 1.9.1 What the user gets (UA Console's tracking workflow)

| Console feature | Here |
|---|---|
| 4 insert slots per input (and per linked pair) | slots 0-3 per input; the active (not bypassed) slots run as one chain on the unit's strip DSP, in slot order |
| Insert Effects REC / MON per channel | `mon` per input: REC prints the inserts (the record channel carries them), MON records the dry input and the inserts are heard in the monitor mix only |
| Unison slot before the inserts | the `unison` slot of an input: built through the driver's analog lease; **no Unison map exists** (the mapper maps only the insert form of a plug-in, upstream's too), so a Unison pick is refused with code `unison` until one does |
| AUX 1 / AUX 2 (stereo) | per input: send level, pan, PRE/POST; per aux: return level, mute, sends into the cues; up to four inserts on each aux return (experimental, off by default) |

#### 1.9.2 UI to host (JSON, `{"cmd": "plug-..."}`, as section 1.5; `unit` as there)

A target is an analog input, `"input": n` (a linked pair: its first input), or an aux bus, `"aux": 0 | 1` (AUX 1, AUX 2;
`input` ignored). `slot` is 0-3; `"slot": "unison"` names the Unison slot where noted.

| Command | Fields | What happens |
|---|---|---|
| `plug-insert` | `unit, input \| aux, slot, name, stereo` | puts `name` in that slot (replacing what was there), rebuilds the chain. No `slot` = slot 0 (the section 1.5 meaning). An aux is always stereo. Linking or unlinking: an insert with the other layout converts the input's chain when every other slot has a map in that layout, else it is refused with code `layout` |
| `plug-remove` | `unit, input \| aux, slot` | removes that slot, rebuilds. No `slot` = the whole chain (its REC/MON stays) |
| `plug-move` | `unit, input \| aux, from, to` | within one chain: to an empty slot it moves, onto a used slot the two swap; rebuilds. Between inputs or units it stays the UI's sequence of section 1.5 (insert, params, remove) |
| `plug-bypass` | `unit, input \| aux, slot, on` | bypass = that slot leaves the DSP chain, its settings kept; rebuilds. No `slot` = every slot. `"slot":"unison"` bypasses the Unison slot |
| `plug-param` | `unit, input \| aux, slot, index, value` | as 1.5, for that slot (no `slot`: the chain's first slot) |
| `plug-retry` | `unit, input \| aux` | builds the chain again (a failed or waiting one) |
| `plug-recmon` | `unit, input, mon` (`true` = MON, `false` = REC; or `"mode":"mon"`) | stored per input (also with no insert: a chain built later is composed with it); on a running chain a live flip when the record delay stays (no plug-in latency), else a rebuild |
| `plug-unison` | `unit, input, name` | the Unison slot (refused with code `unison` while no Unison map exists) |
| `plug-unison-remove` | `unit, input` | removes it, the input's preamp is ordinary again |
| `plug-unison-gain` | `unit, input, db` (10-65) | while a Unison chain runs: the real preamp's gain through the lease (GainC dB-9, GainA/B dB-10) |
| `plug-unison-impedance` | `unit, input, code` (0-15) | the mic impedance code (param 0x07; no code-to-ohm table is known) |
| `plug-unison-params` | `unit, input, words` | raw `{input, parameter, value}` triples for the lease (the plug-in's own analog journal; the driver refuses fields outside the lease) |

A rebuild takes the input back to its plain strip for a moment: the new chain is staged and read back next to the
running one when DSP 1 has room for both, then the old one is destroyed and the new one routed (two transactions);
without room for both, the old one goes first (the gap is the new chain's uploads and pages, up to about 2 s for a
reverb). A refused change leaves the previous chain running (rebuilt if the attempt had taken it down) and
`plugin-result` says why.

#### 1.9.3 Host to UI

`plugin-result` gains `aux` (null or 0/1) and `slot` (null or 0-3) next to `op, input, ok, code, detail`; ops `move`,
`recmon`, `unison`, `unison-remove`, `unison-gain`, `unison-impedance`, `unison-params` are new.

`plugins` (per unit) keeps every 1.5 field (`inserts` now lists each input's first active slot, so a pre-chains page
still shows one insert) and adds:

```json
{"type":"plugins","unit":0, ...,
 "slots":4,"recmon":true,"auxBuses":2,"unison":true,"auxInserts":false,
 "chains":[
  {"input":0,"aux":null,"stereo":false,"mon":false,"state":"running","code":"","why":"","busy":"",
   "latency":64,"recordDelay":136,"overloaded":false,"running":true,"load":{"before":1989,"after":4781},
   "slots":[{"slot":0,"name":"UAD Precision De-Esser","label":"Precision De-Esser","bypass":false,"state":"running",
             "code":"","why":"","latency":0,"codeWords":2004,"pages":1,"params":[...]},
            {"slot":2,"name":"UAD Lexicon 480L","label":"Lexicon 480L","bypass":true,"state":"bypassed", ...}],
   "unison":null,"analog":false},
  {"input":0,"aux":1,"stereo":true,"mon":false,"state":"waiting","code":"aux",
   "why":"aux inserts are off: they are experimental (...AuxInserts = 1 turns them on)", ...}]}
```

| Field | Meaning |
|---|---|
| `slots`, `recmon`, `auxBuses` | 4, true, 2 on both units: draw four slot rows per strip, a REC/MON toggle, an Aux section |
| `unison` | the driver has the Unison lease (FEAT_UNISON). Draw the Unison slot above the inserts either way; picks are refused (`unison`) until a Unison map exists |
| `auxInserts` | aux return chains are taken now (FEAT_AUX and `Parameters\AuxInserts` = 1). False: show the aux slots disabled with the chain's `why` |
| chain `state` | `idle` (no active slot: the plain strip), `loading` (`busy` names the step), `running`, `waiting` (`code` as 1.5: warm, absent, strips, hosting, caller, rate, unit; `aux` aux inserts off), `failed` (`code` as 1.5 plus `cycles`, `layout`, `unison`) |
| chain `latency`, `recordDelay`, `overloaded` | the active slots' latency (samples at 48 kHz), the chain's record delay (200 less the latency through the record tap; MON: 200), and whether the latency is over the 200-sample budget (the record then lags) |
| chain `load` | DSP 1's load word (1/100 %) just before and the peak just after the last build |
| slot `state` | `running`, `bypassed`, `waiting`, `failed` (its own `code`/`why`, else the chain's) |
| `unison`, `analog` | the Unison slot (same fields, `"slot":"unison"`) and whether its analog lease is held |

Codes new in this section: `room` (no room on DSP 1 for the chain: the message gives words needed and free), `cycles`
(DSP 1 at 85 % load, or the learned cost of the added plug-ins would pass 95 %, or the chain ran above 95 % and was taken
off again), `layout`, `unison`, `aux`.

Budgets per chain: at most 4 inserts (and the Unison header); at most 127 private rows and 64 modules (the driver's
CREATE limits); code words of the modules no other running chain holds against DSP 1's largest free extent; the load
word as above (`load`, `cycles`). The load word is DSP 1's own measured load (window +0x0C), as UA's Mixer Engine shows
TotalDSPLoad; its meaning on our driver is not proven (it stayed raised after a destroy at gate P1).

#### 1.9.4 REC / MON underneath

Bit 0x4 of word 4 of a chain block is the record tap (open-apollo `RECORD-PRE-EFFECTS.md`, bench-proven on an x4 for
one mono insert): REC puts it on the last insert, MON on the Mic strip's m0 (every insert's cleared). A running chain
flips with one SERVICE write of those words plus a readback (they are outside the chain directory); when the record
delay changes with the switch (any plug-in latency), the chain is rebuilt with the new delay. With no insert the switch
changes nothing audible. An aux chain is always printed. A Unison header carries the tap in MON ("Unison insert
processing is always recorded"). Unverified on our hardware: more than one insert, stereo, a Unison header (upstream
lists the same open points).

#### 1.9.5 The Unison slot underneath

Driver (`apollo_plugin.h` ops 5-7, `driver/unison_plan.h`): CREATE `F_STAGE | F_CHAIN | F_UNISON` (the header is chain
member 0, licensed like the inserts), BEGIN_UNISON captures the input's ordinary preamp settings (readback flags and
GainC over the write cache), takes the lane (any other write to its fields, from Apollo Control's preamp controls, the
raw tools or the strips' 48V resync, gets STATUS_DEVICE_BUSY), writes PGA 0, waits for the mailbox and a fresh
readback plus 80 ms; ACTIVATE_UNISON routes the chain; the host then writes CUSTOM 1 and waits 80 ms (UA's own
journal); UNISON_PARAMS carries gain, impedance and the plug-in's own triples; DESTROY or closing Apollo Control puts
the baseline back (CUSTOM off, 80 ms, the gains, GainC read back). What is missing for a working Unison slot:
1. **A Unison map**: the bundle of a plug-in's Unison form (the integrated preamp header's descriptor, startup words and
   control sweep). open-apollo's mapper maps only the insert form (`oa:tools/plugin-mapper/README.md:72`) and its daemon
   enables Unison only for a provider that declares it, which none does. A bundle with `"unison": true` (or descriptor
   flag 0x40) is all the host needs to build it.
2. **The plug-in's analog journal**: which `{parameter, value}` triples UA's worker sends when the Unison plug-in's gain
   and impedance knobs move (the custom byte lanes 0x09-0x0F, the impedance codes). Without it `plug-unison-gain` drives
   the ordinary gain fields and `plug-unison-impedance` a raw code.
3. **A hardware gate**: never run upstream on anything but its own bench; the lease's settling (80 ms) and the restore
   order come from upstream's driver.

#### 1.9.6 The aux buses (monitor names, `monitor\t<name>\t<value>[\t<unit>]`, both units)

| Name | Value | Meaning |
|---|---|---|
| `auxsend:<a>:<bus>` | half-dB -120..24, -121 off | input `bus`'s send to AUX a (a = 1, 2; bus as the cue sends: ANALOG n = n-1, TALKBACK = 10) |
| `auxpan:<a>:<bus>` | -100..100 | its pan |
| `auxpost:<a>:<bus>` | 0 pre (default), 1 post | post-fader: follows that input's monitor-mix fader, mute and solo (Console's per-send PostFaderOverride; the aux strip's PRE/POST button is the UI sending this for every input) |
| `auxret:<a>` | half-dB -120..24, -121 off | AUX a's return into the monitor mix (UA's aux fader) |
| `auxmute:<a>` | 0/1 | its MUTE |
| `auxcue:<a>:<c>` | half-dB, -121 off | AUX a's return into CUE c (c = 1-4; the x8p 4 cues, the Twin X 2) |
| `auxcuepan:<a>:<c>` | -100..100 | its pan |

Nothing reads back (as the cue sends): the UI keeps what it set (in `cfg.mixes[x8p | unit id].aux`, say) and the driver
replays it after every connect. The zero state is "nothing set", so the Aux section should send each aux's return level
and mute state once per connection, as the monitor mix does. Coefficients (`driver/monitor_plan.h`): sends as the cue
sends (10^(dB/20)/sqrt2 with the pan law), post-fader with the input's fader added in dB and 0.0 while it is muted or
solo-silenced; the return 10^(dB/20); returns into the cues 10^(dB/20) with the pan law, 0.0 while that cue is muted. Aux
MONO (kext 0x0A / 0x0F) is not offered: its settings are not decoded on the x8p.

**The aux returns need Input Strips (measured on the x8p, Nov 16 2026; docs/STRIPS.md 14).** The returns are DSP inputs
0x10/0x11 (AUX 1 L/R) and 0x12/0x13 (AUX 2 L/R), and like the Virtual inputs they mix nothing and record silence without
a strip: the input sends were always right, but AUX 1 and 2 stayed at exact zero. A throwaway build with the plain Input
Strip on 0x10 carried VIRTUAL 1's -40 dBFS tone, sent at 0 dB, into AUX 1 L's record slot (28) at -43.0 dBFS (-3 dB is
the send's pan law) and Mic 1's noise sent at +12 dB, while 0x11 without a strip stayed exact zero. The unit value
**`StripAuxMask`** (bit 0 AUX 1, bit 2 AUX 2: one strip on each return's L input, 0x10 / 0x12; a strip on an R input
covered input 1's record slot, so the models refuse bits 1 and 3) builds them in the strip pass. **The x8p's default is
0x5: AUX 1 and AUX 2 work out of the box** (proven on the x8p Nov 16 2026: sends reach the buses, `auxret` reaches both
MON sides at its level, `auxmute` silences it); the Twin X's is 0 (not run there). The aux buses are mono: `auxret` goes
to the L input's left and right entries, `auxcue` / `auxcuepan` to its one cell with the pan law, and the input sends
into a strip-backed aux take the left entry at the centre value (an aux send's pan does nothing there). The moment the
strip starts every return and cue cell is written, the UI's value or 0.0, so a new strip never joins MON at the DSP's own
level. GET_STRIPS version 5 (`AuxMask`, `AuxReturnStereo` = the strip runs) tells the UI whether an aux carries audio;
without the strips (StripAuxMask 0) every write is the single left entry on bus 0x10 / 0x12 of before.

Aux inserts: the chain runs on the aux return (DSP inputs 0x10/0x11, 0x12/0x13) with the plain Input Strip carrier,
before the monitor and cue mixes, so "a reverb on the headphones while tracking dry" is AUX 1 with an insert, inputs'
AUX 1 sends, `auxcue:1:<c>` into the headphone cue and `auxret:1` -121 (or a level) for the speakers. Off by default
(`Parameters\AuxInserts` = 1 turns it on, an administrator's registry value), never run on hardware. An aux insert on a
return whose Input Strips StripAuxMask builds is refused (`STATUS_RESOURCE_IN_USE` 0xC0000708, logged "plugins: an aux
insert on AUX n refused"): the chain would SYNCH the same inputs. The other return stays open to inserts.

#### 1.9.7 What the UI does with it

- **Strips**: four slot rows under each strip's preamp (the Unison slot above them, drawn disabled with its reason
  while no Unison map exists); drop or pick into a row = `plug-insert` with `slot`; drag between rows of one strip =
  `plug-move`; the x / Delete = `plug-remove` with `slot`; B = `plug-bypass` with `slot`. Show the chain's `state`/`busy`
  once per strip and each slot's own `state`.
- **REC / MON**: a two-state toggle per strip (and per linked pair: send the pair's first input), always enabled;
  `mon` from the chain (no chain: REC). Show `recordDelay` / `overloaded` in the strip's tooltip.
- **Aux section** (per unit): AUX 1 and AUX 2 strips with a return fader (`auxret`), MUTE (`auxmute`), PRE/POST (sends
  `auxpost` for every input), cue send knobs (`auxcue`), four insert slots (target `aux`); per input strip two send knobs
  (`auxsend`, with pan when the strip is wide enough, `auxpan`).
- **DSP room**: `dsp.room` and the chains' `load`; a refused build's `room` / `cycles` text inline under the strip.

UI as built (Nov 15 2026, `app/ui/plugins.js`, `app/ui/auxbus.js`, every theme): slot rows follow the host's `slots`
(a host without it keeps one row and the 1.5 messages, with no `slot` field); the narrow layouts (console, HUD,
cards) show the used rows and the next empty one, and every row while a plug-in is placed; the Unison slot is
disabled with its reason (no lease, a `unison` refusal's detail, the chain's `why`, or no list item with
`"unison": true`); REC / MON shows only where the host sends `recmon`; the chain's state line shows only when it
is not plainly running (loading, waiting, failed, over the latency budget). The Aux section stores
`cfg.mixes[...].aux = [{ret, mute, post, sends: {bus: {level, pan}}, cues: {c: {level, pan}}}, ...]`; on connect
`applySavedMix` sends, after the monitor mix, each aux's `auxmute`, its `auxpost` (when set), sends, cue sends, then
`auxret` (0 dB until set). In the console and HUD layouts the AUX strips sit behind the master's AUX button (they
need the width); the other layouts always show them. Typed: `in 3 rec|mon`, `in 3 aux1 -12`,
`insert 1176 on 2 slot 3`, `remove 2 slot 3`, `insert eq on aux1`, `aux 1 return -6|mute|post|cue 2 -10`. Checks:
`app/tests/plugins.js` part D (stub host).

Tests: `tools/plugin-recipe-test` (selftest: the chain composer = upstream `compose()` on five synthetic chains,
`golden_chain.h` from `make-golden-chain.py`; tap plans, rebasing, delays, aux and Unison structure, the driver's
checks), `tools/plugin-recipe-test/chain-compare.py` (this PC's real bundles: 24 chains of 2-4 inserts, mono, stereo,
REC and MON, identical line for line on Nov 15), `tools/plugin-plan-test` TestIns (flags, the Unison directory, aux
pairs, the preamp field map against SET_INPUT_PARAM's writes, the lease's capture / patch / restore plan),
`tools/mon-plan-test` CheckAux and `mon-cmds-test` (codes, plan filter, weights against upstream's laws, names).

### 1.10 [Clock] Clock source, digital I/O formats, the front panel, talkback

Built Nov 15 2026 in `builds\work-clock`, offline checks only, **nothing of it run on hardware**. Driver: `driver/clock.c`
(clock source, digital formats), `driver/panel.c` (front panel, monitor link), `driver/clock_plan.h` (the models, the
rules, the evidence), `include/apollo_clock.h` (IOCTL 0x53-0x56). Host: `app/src/clock_host.h`, routed from `main.cpp`
(three marked hunks: the include, the `clock-` / `digital-` / `panel-` route, `PollUnit` in the worker's pass).
apollo-diag: `clock`, `digital`, `panel`, `talkback`. UI built Nov 16 2026: `app/ui/clockui.js` (the Device view's "Clock and
digital I/O" panel, each segment head's clock badge, the talkback strip's note, the Units & routing "Monitor link" panel;
hooks marked `[Clock]` in `app.js`, `units.js`, `index.html`, `app.css`), checks in `app\tests\clock.js` (stub host and demo).
Not built from 1.10.4: the front panel's fields replacing the readback decoding, the combined monitor level in the master
section, and "Dim on talkback" handing over from `cfg.talkbackDim` to the link's `talkDim`.

#### 1.10.1 What each unit has

| | Twin X Gen 1 | x8p Gen 1 |
|---|---|---|
| Clock sources (jacks) | Internal, S/PDIF and ADAT (both on its one optical input) | Internal, Word Clock, ADAT (optical), S/PDIF (coax, per Aaron's hardware list) |
| The unit's own list (0xC08C [31:16], live reads) | S/PDIF, ADAT, Peer, Internal (0x1421) | S/PDIF, ADAT, Word Clock, Peer, Internal (0x14A1) |
| Optical format | input only: ADAT or S/PDIF | input and output, each ADAT or S/PDIF |
| ADAT channels at 44.1/48, 88.2/96 (S/MUX), 176.4/192 kHz | 8, 4, 0 | 8, 8 (two ports), 0 |
| S/PDIF | 44.1-96 kHz, on the optical input in S/PDIF format | 44.1-96 kHz, coax, any optical format |
| SR Convert (S/PDIF input) | yes | yes |
| Front panel | big knob (MONITOR/PREAMP button), knob press = mute, TALK, ALT | monitor knob (if it reports: word 2), preamp knob |

Peer (Thunderbolt) is never offered: without the peer-to-peer path the unit free-runs on it (spec-twinx 6.3.7).
**Proven on these units: Internal only.** Every external source and every format write has never run here: the driver
refuses them unless the request says `try` (code `unproven`); the UI shows them as "Experimental" and sends `try:true`
only after the user confirms. Nothing unproven runs by itself: a connect (bring-up, wake) puts the unit on Internal with
the formats the connect batch writes (S/PDIF in and out), whatever was chosen before.

#### 1.10.2 UI to host (JSON strings, `postMessage(JSON.stringify(...))`)

```json
{"cmd":"clock-get"}
{"cmd":"clock-get","unit":"000000003A00C0DE"}
{"cmd":"clock-set","unit":"000000003A00C0DE","source":"wordclock","try":true}
{"cmd":"clock-set","unit":"000000002F00BEEF","source":"adat","try":true,"hold":false}
{"cmd":"clock-set","unit":"000000002F00BEEF","source":"internal"}
{"cmd":"digital-set","unit":"000000002F00BEEF","what":"optical-in","value":"adat","try":true}
{"cmd":"digital-set","unit":"000000003A00C0DE","what":"optical-out","value":"spdif","try":true}
{"cmd":"digital-set","unit":"000000003A00C0DE","what":"src","value":true,"try":true}
{"cmd":"panel-get"}
{"cmd":"panel-link","monitor":"000000002F00BEEF","level":true,"mute":true,"dim":true,"mono":false,"talkDim":true}
{"cmd":"panel-link","monitor":null}
```

`unit` is the `id` of the `units` list. `source`: `internal`, `spdif`, `adat`, `wordclock`. `hold`: stay on the source
when it loses lock (default: back to Internal after 1 s). `panel-link` `monitor` is the unit with the speakers (an id
with a HW_UID); every other running unit of the combined device is then a knob unit; `monitor:null` (or every flag
false) turns the link off. The link is driver-wide and not stored: off at every driver load, so the host sends its saved
`panel-link` (cfg, below) once after each `units` list that has both units.

#### 1.10.3 Host to UI

```json
{"type":"clock","unit":"000000002F00BEEF","present":true,"source":"internal","wanted":"internal","locked":true,
 "changing":false,"fellBack":false,"hold":false,"reapply":false,
 "sources":["internal","spdif","adat"],"offered":["internal","spdif","adat","peer"],"usable":["internal","spdif"],
 "proven":["internal"],"rate":48000,"ratesOnSource":[44100,48000,88200,96000,176400,192000],
 "registers":{"clockCfg":524,"rateInfo":513,"clockInfo":48000},"readbackLock":true,
 "combined":{"member":true,"timeline":true},
 "digital":{"has":["optical-in","src"],"proven":[],"known":false,"opticalIn":"spdif","opticalOut":null,"coax":false,
            "src":false,"channels":{"adatIn":8,"adatOut":0,"spdifIn":2,"spdifOut":0}},
 "last":{"code":"","why":"","ms":0,"changes":0,"fallbacks":0,"lockLosses":0},"measuredHz":47999.722}
{"type":"clock-result","unit":"000000002F00BEEF","what":"source","value":"adat","pending":true,"code":"","why":"Switching the clock (up to 4 s; it goes back to Internal if the source does not lock)","ms":0}
{"type":"clock-result","unit":"000000002F00BEEF","what":"source","value":"adat","pending":false,"ok":false,"code":"lock","why":"ADAT was not acknowledged in 2 s (no signal on the input?): back on Internal","ms":2260}
{"type":"panel","unit":"000000002F00BEEF","present":true,"valid":true,"seq":12,"knob":"monitor","preamp":1,
 "level":148,"levelDb":-22.0,"hp":112,"hpDb":-40.0,"mute":false,"dim":false,"mono":false,"alt":0,"talkback":false,
 "lock":true,"role":"monitor","talkDimmed":false,"unitDims":false,"pending":false,
 "talkbackChannel":{"recChannels":34,"slots":[32,33],"strip":false},
 "link":{"on":true,"monitor":"000000002F00BEEF","level":true,"mute":true,"dim":true,"mono":false,"talkDim":true,
         "applied":3,"busy":0,"dropped":0},
 "events":[{"seq":12,"field":"level","value":148,"origin":"panel","ms":34512}]}
{"type":"panel-link","ok":true,"code":"","why":"the monitor link is on","monitor":"000000002F00BEEF"}
{"type":"clock","unit":"...","present":false,"error":2,"detail":"that unit is not on apollo.sys right now"}
```

`units` entries gain two summaries once the worker has polled the unit (absent with an older apollo.sys), and the list
is sent again when they change: `"clock":{"source":"internal","wanted":"internal","locked":true,"fellBack":false}` and
`"panel":{"knob":"monitor","role":null}` (`role` `monitor` / `knob` while the monitor link is on). Enough for a unit's
head and the Units view; the full state is in `clock` and `panel`.

`clock`: `source` what the unit runs on, `wanted` what was asked for (they differ while it `fellBack` or waits to go
back on after a transport restart, `reapply`); `sources` the jacks, `offered` the unit's own list, `usable` what a
`clock-set` (with `try`) may ask for now (jack, offered, rate, optical format, SR Convert), `proven` what needs no
`try`; `ratesOnSource` the rates `rate-set` allows on `wanted` (S/PDIF and ADAT: 44.1-96 kHz; the `rate` message's
`allowed` says the same); `registers` raw (`clockInfo` = 0xC084: 48000 on the G6 probe, its meaning for an external
source is not established: show it in Status only); `readbackLock` word 6 bit 31 (null: not reported); `measuredHz` the
unit's position against the PC clock over the last second (on an external clock: what it really runs at);
`combined.timeline` this unit's clock is the combined device's timeline. `digital.known` false until a format was set
since the connect: the formats shown are then the connect batch's (S/PDIF) and the layouts show every slot as before;
`channels` how many ADAT / S/PDIF channels carry audio now (unknown format: as ADAT for the count).
Sent: on `clock-get`, after every `clock-set` / `digital-set` (with a fresh `rate`), and unsolicited from the worker
whenever it changes (`measuredHz` alone: every 5 s).

`clock-result` codes: `""` done, `invalid`, `not-here` (the model has no such jack or control), `not-offered`,
`unproven` (needs `try`), `rate` (the source does not carry the unit's rate), `port` (the optical port is in the other
format: set it first; or the clock runs from the port being changed), `src` (SR Convert and the S/PDIF clock exclude
each other), `not-ready`, `busy` (a rate or clock change is under way), `lock` (the source did not lock: back on
Internal; `why` says whether it never answered or answered and lost it), `hardware`, `driver` (an older apollo.sys),
`error`. `why` is worded for the user: show it as is.

`panel`: the unit's front panel decoded from its readback, sent whenever anything in it changes (the knob, a button, a
setting from here coming back) and on `panel-get` / `panel-link`. `knob` is the big knob's mode (the MONITOR/PREAMP
button; `preamp` = which preamp the knob edits in PREAMP mode, 1-based), read-only: no write for it is known. `level`
and `hp` raw (dB = (raw - 192) / 2). `events` the last 16 changes, oldest first, with `origin`: `panel` (the unit's own
knob or button), `host` (a setting from Windows coming back), `link` (mirrored from the other unit, or the talkback
dim), `unit` (the unit dimmed itself on talkback). `role`: `monitor` (the link's monitor unit), `knob` (a knob unit),
null (link off). `talkbackChannel`: AX_REC_CH and the record slots of TALKBACK 1/2 (null: not in the unit's frame; the
x8p's legacy frame is 32 wide), `strip` false = the mic carries silence (below).

#### 1.10.4 What the UI does with it

- **Clock (per unit; Device view and each segment's head):** a "Clock" selector with `sources`; entries not in `usable`
  disabled with the reason (rate, format, SR Convert), entries not in `proven` marked "Experimental" (one confirmation:
  "never run on these units: the unit may go silent for up to 2 s and comes back on Internal if the source does not
  lock"; then `try:true`). Show LOCKED / NOT LOCKED from `locked`, and in Status `measuredHz`, `registers`,
  `readbackLock`. `changing` or a `pending` result: disable the selector (1-4 s). `fellBack`: a banner "ADAT lost lock:
  back on Internal" with "Try again" (the same `clock-set`). `hold` is an advanced checkbox ("stay on the source when
  its signal drops").
- **Digital I/O (Device view):** per unit the optical input format (Twin X, x8p) and output format (x8p), SR Convert;
  all marked Experimental (`try:true` after one confirmation). After a change the combined layout may change (ADAT
  channels appear or go; at 88.2/96 kHz the Twin X keeps ADAT 1-4): the host's `agg` and the ASIO client follow by
  themselves (the driver bumps the layout; a DAW gets a reset request). Show `channels` as "ADAT in 8 ch, S/PDIF in 2
  ch" and, at 176.4/192 kHz, "digital I/O off at this rate".
- **Store** `cfg.unitPrefs[id].clock = {source, hold}` and `.digital = {opticalIn, opticalOut, src}` only after an `ok`
  result, and send them again (with `try:true`, the user confirmed once) after the unit's `units` arrival: the driver
  forgets them at every connect on purpose.
- **Front panel:** each unit's master section shows the knob mode (MONITOR / PREAMP n / HP) next to its monitor level,
  MUTE, DIM, MONO, ALT and TALK, all from `panel` (they replace the readback-word decoding for these fields when
  `panel` arrives; a host without `panel` keeps decoding `readback` words as today). A short highlight on the control
  an `origin:"panel"` event changed ("Twin X knob").
- **Combined monitor level:** with `panel-link` on, the master section above the segments shows the monitor unit's level
  as THE monitor level, its fader sends `monitor\t1\t<raw>\t<monitor unit slot>`; the knob unit's own monitor level
  is shown small ("knob unit, drives the monitor"). Units & routing gets "Speakers on" (monitor unit) and the link's
  checkboxes (level, mute, dim, mono, dim on talkback), saved as `cfg.panelLink = {monitor, level, mute, dim, mono,
  talkDim}` and sent after each `units` list with both units present. Default: off (no link), as today.
- **Talkback:** TALK on each unit (`monitor\t70\t0|1\t<slot>`, readback/`panel` `talkback`); the Cues page's talkback
  row is the cue sends of bus 10 (`send:<cue>:10`); "Dim on talkback" is `panel-link` `talkDim` when the link is on,
  else Apollo Control's own `cfg.talkbackDim` (host side, as today). Where `talkbackChannel.strip` is false, the
  talkback strip and its meter say "talkback mic: no signal yet (its input strip is not built in this driver)".

#### 1.10.5 What happens underneath (for the wording)

- **Clock change** (driver, the caller's thread, the transport running, as G6's probe did on both units): checks (jack,
  the unit's list, rate, format, SR Convert, proven or `try`); the clock word 0xC074 = (rate index << 8) | source,
  doorbell 4; up to 2 s for the unit's clock event, then the lock (0xC07C) must hold 1 s; otherwise Internal is written
  back the same way and `lock` is answered. A source that never locked is not kept. The unit's lock is held about 1 s
  on success, up to 4 s on failure; the meters stall meanwhile; a missing signal can silence the unit for those 2 s.
- **Watch:** on an external source the service pass watches 0xC07C every 10 ms; unlocked for 1 s, the unit goes back to
  Internal (`fellBack`, `wanted` keeps the choice) unless `hold`.
- **Rate changes** write Internal (the proven path, unchanged): on an external source the unit is on Internal during the
  change and goes back on afterwards (same checks, same 1 s lock rule; a source that does not carry the new rate stays
  off: `fellBack`). `rate-set` refuses rates the wanted source does not carry (S/PDIF, ADAT: 96 kHz at most).
- **Digital formats:** one masked mixer-mailbox write each (device-mode word S8 bit 0x10 optical in ADAT, bit 0x08
  optical out ADAT; S1 bit 31 SR Convert), the kext's encodings via open-apollo, never run here. The connect batch
  writes S8 = 0x20 (both S/PDIF) at every connect.
- **The combined device and an external clock:** the timeline is still the configured master's frames, whatever it is
  clocked from; every other unit is resampled to it by the ASRC, which follows any clock within +/-500 ppm. Two units on
  one external source (x8p word clock, Twin X ADAT from the same device; or the Twin X's optical input fed from the
  x8p's optical output with the Twin X on S/PDIF/ADAT) run sample-locked: the loop settles at a ratio of exactly 1 (the
  ASRC still runs; the bit-exact copy of spec 6.3.6 mode B is not built). A source change on the timeline unit is a
  step the followers see: a resync, and if the timeline stalls for the 2 s of a failed change, the followers are muted
  and re-formed (about 10 s). So: change the clock of the follower freely; for the timeline unit, prefer a moment with no
  recording, or make the other unit the master first. A follower whose external clock differs by more than 500 ppm (a
  source at another rate) is Degraded (muted), as any unit would be. Thunderbolt peer clock stays unavailable.
- **Front panel:** no notification announces the knob. The unit acts on its own knob and buttons (the Twin X's knob in
  MONITOR mode moves its own monitor level, its press mutes) and its 40-word readback follows; the driver's 10 ms service
  pass compares each new readback with the last one (open-apollo's daemon does the same at 50 Hz) and records every
  change with who made it.
- **Monitor link:** a knob unit's own changes go to the monitor unit as ordinary monitor settings (level, mute, dim,
  mono; proven writes): the level as steps (the knob's 0.5 dB steps added to the monitor unit's level, so the two
  knobs never have to agree; a knob unit at the end of its own range stops sending steps: keep its level mid-range),
  mute/dim/mono as values; talkback on any unit dims the monitor unit and undims only what it dimmed. Changes made from
  Windows never travel, so nothing loops. Only units in the combined device take part.
- **Talkback:** the button is readback word 6 bit 8 (and `monitor` 70 = 0x46). The Twin X records TALKBACK 1/2 on record
  slots 32/33 (its frame is 34 wide, AX_REC_CH 34 from its descriptor), and the combined layout lists them as Twin X
  inputs. The mic carries silence until a talkback input strip (module 0xC9) runs on its DSP input; no recipe for it is
  known, so this driver builds none. Its sends into the cues are the cue sends of bus 10; UA's engine writes only 0x46
  when TALK changes, so the unit is taken to gate the talkback bus itself (to check once the mic carries signal).

Tests: `tools/rate-test` (7: sources per model against the live 0xC08C words, every refusal, digital channels per
format and rate, the masked writes, the combined layout following a format), `tools/mon-plan-test` (8: the live readback
words of both units decoded, changes and their origin, the link's rules). On hardware: apollo-diag `clock`, `panel`,
`talkback` (read-only), then `panel 30` while turning the knob, `panel link`, `digital ... --try`, `clock ... --try`.

### 1.11 [Sess] Sessions, the last state, Start with Windows and the tray

Built Nov 15 2026 (`builds\work-sess`). UI: `app/ui/sessions.js` (model, apply, the views), `sessions.css`, a
`sessions: { layout }` line and a `[Sess]` block in each theme pack, hooks in `app.js` and `cmd.js`. Host:
`app/src/session_host.h` (the files), `main.cpp` (autostart, tray, one instance, routing), `plugin_engine.h`
(`plug-session-get` / `-set`). Checks: `app\tests\sessions.js` (stub host and demo). Nothing of it run on hardware.

#### 1.11.1 What a session holds

`%APPDATA%\Avium\ApolloControl\sessions\<name>.json`, UTF-8, one object. The name becomes the file name with `<>:"/\|?*`
and control characters as `_`, at most 80 characters, device names prefixed `_`; the name inside the file is the one
shown. `last-state.json` in the same folder is the autosave (1.11.4), never listed as a named session.

```json
{"format":"apollo-control-session","version":1,"name":"Tracking drums","saved":"2026-11-15T18:02:11.000Z","app":"1.0.0",
 "view":{"look":"studio","screen":"control","unit":"SN20000000000001"},
 "units":[{"id":"SN20000000000001","model":"Apollo x8p","deviceType":34,"serial":"20000000000001","rate":48000,
   "inputs":[{"gain":38,"line":false,"pad":false,"p48":true,"lowcut":false,"phase":false}],
   "links":[false,true,false,false],"hizImp":-1,
   "monitor":{"level":152,"mute":false,"dim":false,"mono":false,"dimLevel":1,"source":0,"hp1":null,"hp1src":0,"hp2src":1,"alt":0},
   "mix":{"alt":[{},{}],"cues":[{},{},{},{}],"main":{"0":{"level":-12}},"aux":[{"ret":-6},{}]},
   "channels":[{"name":"Kick","color":"red","hidden":false}],
   "plugins":{"chains":[{"unit":"SN20000000000001","input":0,"aux":null,"stereo":false,"mon":false,"slots":[...],"unison":null}]}}]}
```

| Field | Meaning |
|---|---|
| `version` | 1. A file with a higher version is refused ("saved by a newer Apollo Control"); one without `format` is not a session |
| `view` | Apollo Control's own state: the theme, the screen, the focused unit; a load restores them |
| `units[]` | one part per unit, by unit id (section 1.1). A part for a unit that was not connected when saving is the one it had when last here (or the one waiting for it), flagged `"absent":true` |
| `rate` | the unit's sample rate (its `rate` entry, else `status.info.sampleRate`) |
| `inputs`, `links`, `hizImp` | the preamps as the app shows them (readback wins); the pairs the model links (`null` = not linkable); the Twin X Hi-Z load (-1 = never set) |
| `monitor` | level (raw, 192 = 0 dB), mute, dim, mono, dim level 1-7, source, headphone level (models with a digital one, else `null`), headphone sources, speakers (`alt`) |
| `mix` | the unit's `cfg.mixes` entry as stored (1.2a [Monitor] / [Mix], 1.9.7): monitor mix, cues, ALT outputs, aux buses: only what was touched |
| `channels` | input names, colours, hidden |
| `plugins` | the engine's chains for this unit, plugins.json version 2 as is (slots with plug-in, bundle, SHA-256, bypass, values, texts, accepted words; REC/MON; Unison). `null`: the host did not answer, a load leaves the inserts alone |

#### 1.11.2 UI to host (JSON strings, as `rate-*`)

| Command | Answer |
|---|---|
| `{"cmd":"session-list"}` | `{"type":"sessions","dir":"...","list":[{"name","file","saved","modified","version","units":[{"id","model","deviceType","rate"}],"inserts","bad"}],"last":{...}\|null}`, newest first; `bad` is set for a file that is not a session (listed, not loadable) |
| `{"cmd":"session-save","name","session":{...}}` | writes `<name>.json` (temp file, then moved over), `{"type":"session-result","op":"save","ok","name","file","why"}`, then a fresh `sessions` |
| `{"cmd":"session-load","name","file"?}` | `{"type":"session","op":"load","name","file","session":{...}}` (the file as it is), or a `session-result` with `ok:false` (`why` `none`: no such file) |
| `{"cmd":"session-load","last":true}` | the same with `op` `last` (sent at hello: the parts of units not connected now) |
| `{"cmd":"session-delete","name","file"?}` | the file goes to the Recycle Bin; a result and a fresh `sessions` |
| `{"cmd":"session-autosave","session":{...}}` | writes `last-state.json`; a result with `op` `autosave` |
| `{"cmd":"session-export","name","session"}` or `{..., "file"}` | a Save As dialog (from the window procedure, not inside the WebView2 handler), then the file written or copied; a result with `op` `export` (`cancelled` true when the dialog was closed) |
| `{"cmd":"session-import"}` | an Open dialog, then `{"type":"session","op":"import","file","session"}` for a session file, else a failed result; the page saves it into the folder under its name (asking before replacing one) and offers to load it |
| `{"cmd":"session-recent","names":[...]}` | nothing: the tray menu's recent sessions (at most 8, the page's order) |
| `{"cmd":"session-plugins"}` | `{"type":"plugin-session","op":"get","doc":{"version":2,"mapsDir","chains":[...]}}`: every unit's chains, read only (main.cpp hands it to the plug-in thread as `plug-session-get`; under its own name so the page's `plug-*` commands stay the user's actions) |
| `{"cmd":"plug-session-set","unit":<slot>,"unitId","chains":[...]}` | the unit's chains are replaced: running ones destroyed (the plain strips back), the new ones saved in plugins.json and built by the engine's restore once the unit's strips are Live (waiting or failing with their reason like any saved chain); `{"type":"plugin-result","op":"session","ok",...}` and a fresh `plugins`. The id is the unit's own while it is here, else `unitId` |
| `{"cmd":"autostart-get"}`, `{"cmd":"autostart-set","on"}` | `{"type":"autostart","on","current","command","error","detail"}`: whether HKCU `...\CurrentVersion\Run` holds `Avium Apollo Control`, whether it starts this exe, its text. Only `autostart-set` writes (or deletes) it; the page sends it only when the user flips "Start Apollo Control when I sign in". The value is `"<this exe>" --minimized` (per user, no administrator) |
| `prefs\t<close to tray>[\t...]` | the window's close button: hide to the tray (1) or quit (0). Further fields are ignored (the Run value is `autostart-set`'s alone) |
| `show` | the window shows and comes forward (a session load from the tray needs its 48V question answered) |

Host to UI, tray menu picks: `{"type":"tray","action":"mute"|"dim"|"volup"|"voldown"}` (the focused unit, as M and D;
volume 2 dB a step, never past Settings' monitor limit) and `{"type":"tray","action":"session","name":"..."}`.

#### 1.11.3 Loading (sessions.js `apply`)

1. **Which unit takes which part**: the present unit with the part's id; else one of the same model that no other part
   names (another x8p); else the part waits (below). A read-only unit (below Mode 2) is skipped and said so.
2. **48V**: if the load would switch phantom power on anywhere (after links: a linked pair's second input takes the
   first's), one question first, naming the inputs: Turn on 48V / Load with 48V off / Cancel (nothing sent). Asked
   always, whatever "Ask before turning on 48V" says. Nothing is ever turned on silently.
3. **Per unit, in order**: the **rate** (`rate-set`, `combined:true` for a member of the combined device, once for all
   its members; the final `rate-result` is waited for, up to 20 s; a rate not allowed now is reported, the rest still
   loads); the **preamps** (links first, then line, pad, low cut, polarity, gain, 48V per input, as `setInput`; gains stay
   when Settings locks them; the Hi-Z load); the names; the **mix**: the stored mix is replaced and sent as a connect sends
   it (mutes and solos first, pans and levels, aux, ALT, cues), with UA's default sent for whatever the session leaves
   untouched that the unit was sent before (0 dB, centre, unmuted, sends off); the **monitor** (mute, dim, mono, dim
   level, source, headphone sources and level, speakers, then the level, never past the monitor limit; talkback never);
   last the **plug-ins** (`plug-session-set`).
4. **A unit that is not connected** keeps its part in `cfg.session.pending[unit id]` (in config.json, so a restart keeps
   it) and the session list says WAITING. When it connects (and its strips are Live, 1.11.4) its part goes out in the same
   order, with the same 48V question (the window comes forward for it), instead of its last state.
5. The session becomes the current one (`cfg.session.name`, the Session button), the theme and screen follow `view`, and
   "edited" (a word on the button and in the list) compares the state with the one taken just after the load or save
   (`cfg.session.base`).

#### 1.11.4 The last state

What the app keeps across restarts and replugs: config.json's `cfg.mixes` (monitor mix, cues, aux, ALT and now the
monitor routing, `mixes[...].mon`: source, headphone sources, dim level, none of which reads back), the Hi-Z load
(`unitPrefs`), and the engine's plugins.json. On launch and on each unit's (re)connect, `app.js syncMixApply` sends it
once per connection **once the unit's strips are Live**: `live.strips.state` 4, or 0 / 5 (off, failed: nothing to wait
for), or no strips reported; idle, armed or building for 30 s after the connect is no longer waited for; a host that
never sends `live` gets it at once. The preamps, 48V, monitor level, mute, dim and mono read back and stay as the unit
reports them, so the autosave never turns 48V on. The plug-in engine restores its chains on its own (1.5). sessions.js
mirrors all of it to `sessions\last-state.json` within about 5 s of a change (a full session, units not connected now
included); the list shows it as Last state, loadable like a session (then with the 48V question).

#### 1.11.5 Start with Windows, the tray, one instance

Settings, Window and tray: **Start Apollo Control when I sign in** (shows what the Run value says; flipping it is the
only thing that writes it) and **Close to the tray** (the user's choice once set; until then on exactly while it starts
with Windows). The tray menu: Open, Monitor volume up, Monitor volume down, Mute monitor, Dim monitor (checked from the
page's `trayState`), Recent sessions (a submenu), Quit; the tooltip names the session and the monitor level. A second
start of the exe focuses the first (it passes it the foreground); a second `--minimized` start leaves it in the tray.

#### 1.11.6 The views (docs/THEMES.md: one model, many views)

The Session button in the top bar (name, "edited") opens the list; Ctrl+O opens it in every theme, Ctrl+S saves (Save
as when it has no name), Esc closes it and gives focus back. Each pack names its layout: Studio `console` (SESSION, the
session on a scribble strip, backlit SAVE / SAVE AS / IMPORT / EXPORT keys, a RECALL list), Cyberpunk `hud` (save-state
cards), Lab `table` (sortable columns, soft-keys per row, a status line), Mono `terminal` (text rows with `[load]`
`[exp]` `[del]` and a `session>` prompt; `session save <name>`, `session load <name>`, `session list`, `session delete
<name>`, `session last`, `session export [name]`, `session import` on its command line and in Ctrl+K, which also lists
Load session: <name> per session), Day `plain` (plain words, 44 px buttons), Arcade `slots` (SAVE SLOTS: numbered
slots with LOAD / SAVE / ERASE, an AUTO slot for the last state, an empty slot to save into). Every layout has the same
actions and accessible names; the current session, edited, waiting and the last state are words, never colour alone.
In the demo (no host) sessions live in the browser's storage, never in the host's folder.

### 1.12 Nov 16 2026: outputs, driver events, parameters, undo, copy, diagnostics

Built and installed Nov 16 (driver 6.55.42.311 and later); parity rows in `docs/status/parity.json`.

#### 1.12.1 [Out] Output and front-panel settings (`app/ui/outputs.js`, `driver/monitor_plan.h`)

Monitor params, one `monitor\t<name>\t<value>[\t<slot>]` each, kept in `cfg.mixes[<unit>].out` and sent again once per
connection after the ALT outputs (`savedMixOut`, through `Outs.applyOut`); nothing reads back.

| name | value | x8p | Twin X Gen 1 | mailbox |
|---|---|---|---|---|
| `linepad1`..`linepad4` | 0 +4 dBu, 1 -10 dBV (Line 1-2 .. 7-8) | yes | no (refused, 50) | S0 bit 28+n |
| `monref` | the same, monitor outputs | yes | yes | S11 bit 4 |
| `digmirror` | 0/1, monitor to S/PDIF | yes | no | S6 bit 10 |
| `fcn` | 0 none, 2 mono, 3 dim, 4 talkback | yes | no | S13 [15:13] |
| `cliphold` / `peakhold` | 0-5 / 0-4 (none, 1, 3, 5, 10 s, infinite) | yes | yes | S1 [30:28] / [15:13] |
| `aux1mono` / `aux2mono` | 0/1 | yes | yes | S6 / S7 [9:8] |
| `identify` | 0-7 (the UI blinks 1 and 2 for 4 s, then 0) | yes | yes | S6 [13:11] |

The Outputs card in the monitor column shows what the focused model has. A value out of range answers 87, a control the
model lacks 50 (shown inline like the other refusals).

#### 1.12.2 [Events] The driver's event channel (`include/apollo_events.h`, `driver/events.c`)

`IOCTL_APOLLO_WAIT_EVENTS` (0x59, user level): a per-unit ring of 32 events; a wait with nothing newer than its
`AfterSeq` is parked until the next event (NOWAIT answers at once). Kinds: STAGE, LEAVING (D0 exit, removal, update:
close the handle), RATE, CLOCK, PANEL, DSP_FAULT (a quarantine), ENGINE_LOST, PARAMS. The host's event pump (`main.cpp
EventPumpMain`) keeps one overlapped handle per unit, closed with the slot's handle on every removal notice, primes it
with a NOWAIT read (old ring entries are never posted) and posts `{"type":"event","unit":<slot>,"seq","kind","a","b",
"reset"}` for every kind but PANEL. The UI toasts `dsp-fault` and `engine-lost` and asks for the rate after `rate` and
`clock`. apollo-diag: `events [seconds]`.

#### 1.12.3 [Params] Driver Parameters at run time (`include/apollo_params.h`, `driver/params.c`)

`GET_PARAMS` (0x57, user level): every per-unit Parameter as this start runs it, its source (D default, U unit key,
M model key, G legacy global, X forced by Mode), the model default and what `Units\<key>` holds now (pending = applies at
the next start). `SET_PARAM` (0x58, administrators): writes or removes `Units\<key>\<name>` for the next start. The
host's `params-get {unit?}` answers `{"type":"params",...}`; the Device view shows a read-only table. apollo-diag:
`params`, `param <name> <value>|--remove` (elevated).

#### 1.12.4 [Undo] Undo and redo (`app/ui/undo.js`)

Per unit: a baseline at the last quiet moment (`Sess.undoPart`: the session part without plug-ins, rate, names or
identity); a change this app sends (`hwSend`) opens a step, committed 0.7 s after the last send of a run if the state
differs; while nothing is changed here the baseline follows the unit once a second, so the front panel, readback and a
connect's replay never become steps. Undo applies the step through the session paths (`Sess.undoApply`) and never
switches 48V on. Ctrl+Z, Ctrl+Y / Ctrl+Shift+Z, the header's two buttons; 50 steps per unit. Checks: `app/tests/undo.js`.

#### 1.12.5 [Copy] Channel copy and paste (`app/ui/chcopy.js`)

Right-click a strip's number or name: Copy, Paste. The preamp (never 48V on), the monitor-mix level, pan and mute, the
cue sends and the aux sends; across units too; one undo step.

#### 1.12.6 [Diag] Save diagnostics, the app log, crash dumps (`app/src/diag_host.h`, `main.cpp`)

`{"cmd":"diag-save"}` writes `%LOCALAPPDATA%\Avium\ApolloControl\diagnostics\<date-time>\` (driver log; per unit the
identity, readback, events, rate, clock, panel and parameters; Apollo Control's config, plugins.json, plughelper.log,
apollo-control.log and the newest crash dump; the scripts' logs in a source tree) and opens it; reply `diag-result`.
`apollo-control.log` (same folder's parent, 1 MiB then `.1`) records start and quit, units arriving and leaving, refused
settings and driver events. An unhandled exception writes a minidump to `crashes\` (the last 10 kept). Nothing is sent
anywhere.

### 1.3 UI to host: `agg-get`, `agg-set`

Posted as **strings holding JSON** (`postMessage(JSON.stringify(...))`), because the host reads
messages with `TryGetWebMessageAsString`. `HandleUiMessage` must test for a leading `{` before it
splits on tabs.

```json
{"cmd":"agg-get"}
{"cmd":"agg-set","config":{ ...section 2... }}
```

`agg-get` goes out on the first `units` list and when the view opens without an answer yet (at most
every 3 s).

### 1.4 Host to UI: `agg`

```json
{"type":"agg","config":{...}|null,"pending":false,"error":null,"detail":"","needsAdmin":false}
```

| Field | Meaning |
|---|---|
| `config` | what the driver holds now; `null` = nothing saved; absent = unchanged (fine on `pending`) |
| `pending` | `true` while an `agg-set` is still under way (waiting for the driver's next tick). Send one or more, then a final reply with `pending:false` |
| `error` | `null`, a string, or `{"code":<Win32>,"detail":"..."}`. Codes the UI words: 5 access denied, 87 config rejected (+ detail), 1 or 50 no combined device in this build (preview), 170 busy, 1223 prompt cancelled (only for a host that elevates) |
| `detail` | text shown while `pending` |
| `needsAdmin`, `elevated` (optional) | `false`: Apply needs no elevation, because `AGG_SET_CONFIG` is a user-level IOCTL and the driver stores the configuration itself (spec 6.8). The host sends `false`. The UI starts at `false` and keeps the last value when a reply leaves it out; `true` (with `elevated`) is only for a host that would apply through an elevated helper |

Reply to `agg-get` with the current config. Reply to `agg-set` with zero or more `pending:true`, then
the final state: the config as the driver stored it (normalised is fine) or an `error`. Send an
unsolicited `agg` whenever the stored config changes for another reason.

`config:null` with error 1 or 50 puts the page in preview mode (Apply off). No `agg` within 5 s of
`agg-get` does the same ("this host build has no combined device yet"). No final answer within
60 s of `agg-set` shows an error and re-enables Apply.

## 2. Configuration (`config`)

```json
{
  "version": 1,
  "units": [
    {"id":"7C1E40A2D3F09B61","model":"Apollo Twin X","deviceType":153,"serial":"2253202500117"},
    {"id":"SN20000000000001","model":"Apollo x8p","deviceType":34,"serial":"20000000000001"}],
  "master": "7C1E40A2D3F09B61",
  "channels": null,
  "preset": "studio",
  "routes": [
    {"src":"daw","unit":"7C1E40A2D3F09B61","port":"mon","ids":[2305,2306]},
    {"src":"windows","unit":"7C1E40A2D3F09B61","port":"mon","ids":[2305,2306]},
    {"src":"7C1E40A2D3F09B61:line3-4","unit":"7C1E40A2D3F09B61","port":"line3-4","ids":[2563,2564]},
    {"src":"SN20000000000001:mon","unit":"SN20000000000001","port":"mon","ids":[2305,2306]}]
}
```

| Field | Meaning |
|---|---|
| `units` | members in **block order**: the combined ASIO/Windows channel list is each unit's channels in this order. Absent units stay listed (their channels are silent, spec 6.6) until the user forgets them |
| `master` | configured clock master (unit id). If it is absent or not running, the first running unit in `units` order is acting master (spec 6.3.4) |
| `channels` | `null` = simple (whole blocks, every channel shown). Advanced: `{"in":[{"unit":id,"slot":n,"hidden":bool},...],"out":[...]}` = the full logical order; hidden entries are left out of the ASIO list |
| `preset` | `studio`, `mirror`, `phones` (headphones split) or `custom`; a label, the routes are authoritative |
| `routes` | the engine routing matrix (spec 6.8): one stereo source to one stereo destination per entry; several entries per source = mirroring; several sources on one destination add with saturation |
| other keys | kept on the round trip (room for `clockMode`, `align`, `asrcQuality` of spec 6.8) |

**Sources:** `daw` = logical ASIO outputs 1-2, whatever they are; `windows` = the combined Windows
`Monitor` render endpoint; `<unit id>:<port>` = the logical output pair whose channels are that
unit's port (found by channel ID, so it survives reordering). **Destinations:** `unit` + `port`, with
`ids` = the two playback channel IDs, which the driver maps to slots through the unit's descriptor.

| Model | `port` | IDs | Jack |
|---|---|---|---|
| x8p | `mon` | 0x0901, 0x0902 | monitor outputs |
| x8p | `line1-2` .. `line7-8` | 0x0301-0x0308 | line outputs 1-8 |
| x8p | `hp1`, `hp2` | 0x0A01/02, 0x0A03/04 | CUE 1, CUE 2 (headphones while their source is that cue) |
| Twin X | `mon` | 0x0901, 0x0902 | monitor outputs |
| Twin X | `line3-4` | 0x0A03, 0x0A04 | CUE 2 = LINE 3/4 outputs |
| Twin X | `hp` | 0x0A01, 0x0A02 | CUE 1 = headphones |

**Presets** (every other pair keeps its own output): Studio = DAW mains and Windows to the Twin X
MON; Mirror = both to every unit's MON; Headphones split = DAW mains to the Twin X MON, Windows to
its headphones. Without a Twin X, the master's MON stands in.

## 3. Behaviour the UI implements

- **Unit arrives:** a short notice ("Apollo Twin X connected"), a Status section, a Units row. A
  full-mode unit (`mode` 2) the configuration does not know joins the draft at the end of the block
  order (other units' channel numbers stay put); a first Twin X becomes the draft master. Nothing is
  written until Apply. Units below Mode 2 (or `observe`) are listed "not combined" and shown
  read-only: "observe mode, read-only" in the Status header, no input rows for an observe unit.
- **Unit leaves:** notice ("... disconnected"), its Status section and live state are dropped, its
  configuration stays, greyed "not connected" with **Forget**. Forget asks first, then removes it from
  the draft; Apply writes it.
- **Defaults** (nothing saved): units that run audio, Twin X first and master, simple order, Studio.
- **Validation:** Apply is blocked when nothing is routed at all, every input or output is hidden, or
  no master is set. Warnings: DAW mains or Windows going nowhere or only to absent units, routes to a
  unit below Mode 2.
- **Apply bar:** the change list, what Apply interrupts (ASIO client reset for master, order,
  add/forget; one short gap in Windows streams for a master change; routing alone switches at the next
  tick), the note that no administrator rights are needed, and the `pending`/`error` states. An open
  ASIO client gets a confirm dialog first.
- **Drift:** `(fps_unit / fps_master - 1) * 1e6` from each unit's `live.engine.framesPerSec`, smoothed.
  Two independent 1 s QPC estimates, so expect a few ppm of noise; `AGG_GET_STATE.RatioPpb` (spec 6.9)
  can replace it later.

**Elevation, decided.** Spec 6.8 makes `AGG_SET_CONFIG` an APOLLO_USER IOCTL with the driver
persisting the config itself, so Apply needs no elevation; only the endpoint set, `Mode` and strips
values need admin (separate tools). The host sends `needsAdmin:false` and the page says "No
administrator rights needed".

## 4. Host checklist

1. Done (G2): enumerate every `GUID_DEVINTERFACE_APOLLO` instance; slot 0 for the x8p, others the
   lowest free of 1-3, kept while the unit stays; send `units` after `hello`, after `resend`, and on
   every arrival, removal, Mode, stage or `active` change.
2. Done (G2): tag `status`, `readback`, `levels`, `live` with `"unit"` (0 included). `units` goes out
   right after a rescan, before a new slot's first message; `status` `present:false` on removal.
3. Done: `HandleUiMessage` routes strings that start with `{` to `HandleUiJson`.
4. Done (G6, spec-twinx 6.9.5): `agg-get` reads `AGG_GET_CONFIG` and answers the JSON the driver
   stored, or a description of its binary config when it holds none (its default: x8p first, Twin X
   master, Studio; or one set by `agg-test` or the ASIO clock menu). `agg-set` compiles the config
   (`app/src/agg_json.h`; unit ids = HW_UID unit keys; ports per the table in section 2), answers
   `pending`, calls `AGG_SET_CONFIG` (user level) and answers the stored config, or an error: 87 with
   the reason for a config the host or the driver refuses, 1 for a driver before G6 (preview), 50 when no
   Apollo runs. Config keys the driver uses besides section 2: `asrcQuality` (`low`/`default`/`high` =
   h 8/16/32) and `enabled` (false = Combined off); every other key is kept verbatim in the stored JSON.
5. `--view units` opens this page (the UI accepts it; the host passes the string through); so does
   `--view control:twinx` for the Control view on the Twin X (1.2a).
6. Done: `input` takes `hizimp` (`APOLLO_PARAM_HIZ_IMP`) besides line, pad, link, 48v, lowcut, phase, gain.
7. Done (G7, spec-twinx 6.9.7): the unsolicited `agg` of 1.4. The worker polls `AGG_GET_STATE.ConfigGeneration` once a
   second on a unit it holds; when it moved and is not the generation the host's own agg-set produced, it answers as
   for `agg-get` (the stored JSON, or a description). Refusals: the driver now completes `AGG_SET_CONFIG` with success
   and `APOLLO_AGG_SET_RESULT.Error`; the host replies 87 with the reason in words (no more "check 0").
8. G7 config key `"align"`: absent or `true` = aligned (as before), `false` = lowest latency, unaligned
   (`APOLLO_AGG_CFG_ALIGN` clear); described back. The page has no control for it yet (keys it does not know are
   kept on the round trip). Note for presets: the `windows` route now drives the combined Windows "Monitor"
   endpoint (when `Parameters\Aggregate\EndpointSet` is 1); without a `windows` route Windows plays on the master's MON.

## 5. Demo and tests

In demo mode the page simulates a Twin X beside the simulated x8p, plus the driver's answers.
Scenarios (the Demo switch on the Units panel, or `?units=` in a browser): `x8p` (x8p only),
`both` (default), `unplugged` (Twin X saved, not connected), `observe` (Twin X at Mode 0).

`app\tests\units.js` (in `run.cmd`) plays the host through a stub `chrome.webview` and checks: the
`agg-get`/`agg-set` strings, arrival notice and Status section, master default, drift, presets,
Apply with a host that asks for elevation, pending, error and success, validation, master-change
warning, block order, removal (section gone, late messages dropped, config greyed with Forget),
Forget and Revert, observe read-only, advanced order, a unit in slot 2 with slot 1 empty, the host's
"not available" `agg` reply (preview, no administrator claim), the demo scenarios, and no horizontal
overflow at 980 px. `host-path.js` and `status.js` cover the G2 side: unit-tagged messages leave the
x8p controls alone, `MODEL_CAPS` hides the x8p's headphone knobs, observe sections, slots opening
and closing.
`control-units.js` covers section 1.2a: the selector (present units only, remembered across a restart and
a replug, the `--view` pin), the Twin X strips and monitor, per-unit state, readback, pending writes and
meters, sends tagged with the unit, refusals inline and reverted, the no-preamp note, a read-only unit, the
Twin X alone, tray mute, the demo (an x8p preset recalled on the Twin X) and 980 px.
