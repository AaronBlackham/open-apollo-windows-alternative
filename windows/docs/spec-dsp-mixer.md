# DSP and mixer layer: minimum spec for Apollo x8p and Twin X on Windows

Goal of this layer: (a) a mic on preamp 1 reaches a host capture channel with its gain set, and
(b) host playback reaches the monitor outputs at a sane level. UAD plug-in hosting is out of
scope.

Conventions

- Every citation is `path:line` relative to `apollo-win/reference/open-apollo/` (third-party,
  read only, never built or run).
- **(derived)** marks a value I computed from the source's own tables or formulas rather than
  copied. **(inference)** marks a conclusion the source does not state outright.
- **[core]** = owned by the PCIe bring-up spec (ua_core.c). **[audio]** = owned by the DMA and
  streaming spec (ua_audio.c). Those items are listed only where this layer depends on them.
- "Daemon" = `mixer-engine/ua_mixer_daemon.py` plus `mixer-engine/hardware.py`.

---

## 0. Short answer: does audio flow without mixer init?

The state below assumes the Apollo was power-cycled and nothing but our driver has touched it since.

| Path | What happens | Evidence |
|---|---|---|
| Host playback on MON L/R reaching the monitor outputs | **Flows** once the DSPs are ring-connected (per-DSP connect plus dated finalize) and the transport runs. No input strips and no daemon init are needed. The level and mute are whatever the unit already holds (front-panel knob). The driver never writes the monitor level (setting 2) itself. | Skipping the ring connect "left playback silent" on a Twin X: `CHANGELOG.md:50-52`. Monitor L/R is a plain playback sink, unlike the Virtual devices that need strips: `README.md:233-241`. The driver's connect skips setting 2: `driver/ua_audio.c:3283-3301`. |
| Mic to capture, Apollo x4 and Twin X Gen 2 | **Does not flow.** Record channels, the monitor mix and the readback meters stay at zero until the host builds a Console "Mic Input Strip" on a DSP, using DSP code taken from the user's UAD install. With the strip in place, "an SM7B on input 1 records and reaches MON L/R from a cold start with nothing touched". | `driver/ua_strips.c:7-13`, `HARDWARE-SESSIONS.md:319-333`, `README.md:133` |
| Mic to capture, x8p (0x22) | **Unknown.** Owners verified "all 8 preamps discrete over ALSA" with a driver that never builds strips for the x8p, because 0x22 is missing from the strip model table. The source does not say whether those runs followed a power cycle with only Linux touching the unit. On the x4, earlier "verified" captures predate the strip work, and the strip file says a Linux cold start records zeros. That suggests the x8p runs may have been warm (inference). | `devices/apollo-x8p.json:14`, `driver/ua_strips.c:770-789`, `README.md:125` |
| Mic to capture, Twin X Gen 1 (0x99) | **Assume it does not flow** (same family as Gen 2). The driver has no strip support for 0x99, and the model is unverified. | `driver/ua_strips.c:774-789`, `devices/apollo-twin-x.json:7` |
| Mic into the monitor mix | Needs the strip on **DSP 1**, not DSP 0, plus non-zero bus coefficients, which the firmware defaults to zero. | `HARDWARE-SESSIONS.md:327-328`, `driver/ua_dsp.c:3906-3913` |

So the minimum is: ring connect, rate ack, mailbox init, the input strip (x4/Twin X Gen 2, and
probably x8p and Twin X Gen 1), preamp gain and 48V through the mixer-settings mailbox, and a
monitor level and unmute through the same mailbox. Section 6 gives the ordered list.

---

## 1. Firmware and DSP modules

### 1.1 Nothing is booted onto the DSPs

- The DSPs boot themselves. "The macOS kext never uploads anything at start: the DSPs boot
  themselves, and Bill code reaches a DSP only when the resource manager places a plugin":
  `driver/ua_dsp.c:2848-2853`.
- The old "firmware stage" (`ua-apollo-mixer.bin`) is off by default. The x4 answered every block
  with 0xF0010006 and cold-starts the same without it: `driver/ua_core.c:83-86`,
  `driver/ua_core.c:4393-4401`.
- DSPs 1 and up need no firmware: `driver/ua_core.c:4410-4415`. The old DSP program step
  (command 0x00360000) was removed because it never answered and the cold start works without it:
  `driver/ua_core.c:4436-4444`.
- The host only waits for each DSP's boot word (window +0x1A4) to read something other than 0 or
  0xa8caed0f: `driver/ua_dsp.c:3065-3077`, `driver/ua_dsp.c:3124-3143`.

### 1.2 What is taken from the UAD install: the Console strip modules

The only UA bytes this layer needs are the built-in Console input-strip DSP modules ("Bill"
records): `driver/ua_strips.c:27-33`, `console-rs/crates/oa-assets/src/firmware.rs:9-12`.

| id | Role | Length (bytes) | w4 | Code-pool dwords | Required | SHA-256 (UAD 12.0.0.602) |
|---|---|---|---|---|---|---|
| 0x0EB | Mic strip | 716 | 162 | 0x06c | yes | 2b879e0824970650f4ae0de110bb4bdb22897d0833d85156dd36f51986c15ea7 |
| 0x0DA | Mic strip | 3044 | 744 | 0x1f0 | yes | 37cd9c2df53ec742d61353437ad7f7212633e27801f1cf1f57e3818cbe47dd06 |
| 0x0A5 | Mic strip, Input strip | 1940 | 456 | 0x130 | yes | 1fe90428f88b7f8c0a35b01ea0cfca3937d4b365b7c6ef427d24cfee65ee6c81 |
| 0x0C2 | Mic strip, Input strip | 184 | 33 | 0x016 | yes | ad0946b371df47c9a7d459c7b5b288d58aad9690b6a9a39a6c5895ac36720a2f |
| 0x12B | Mic strip | 452 | 96 | 0x040 | yes | ff60952444ae492aa1a8b85dae686ad8445b9971260174202ad1ce13c73ba02d |
| 0x0DB | Input Strip, reserved (refused by the driver) | 728 | 165 | 0x06e | no | 5937b45b7f08f974f1d8599b3cfaec8130ac7815f3a3c3410612ba041a7cee0a |
| 0x0C9 | Talkback strip, reserved (refused) | 352 | 75 | 0x032 | no | 8222981a81d20c417113b02340e1df8995d4aa11b07942017583f2151a31a24a |

Sources: pins in `console-rs/crates/oa-assets/data/firmware-pins.json:10-46`. Driver table (length
and code-pool dwords) in `driver/ua_strips.c:221-229`. Code-pool dwords = w4 * 2 / 3:
`driver/ua_strips.c:206-208`. The Mic strip loads EB, DA, A5, C2, 12B in that order:
`driver/ua_strips.c:209-212`. The Virtual-input "Input Strip" (pluginId 0x83E) uses only A5 and
C2: `driver/ua_strips.c:22-24`. Only UAD 12.0.0.602 versions are pinned (same file).

Plug-in DSP modules (`oa-assets dsp`, written to `/lib/firmware/universal-audio/plugins`) are for
plug-in hosting only: `console-rs/crates/oa-assets/src/dsp.rs:1-20`,
`console-rs/crates/oa-assets/src/dsp.rs:36`. They are not needed here.

### 1.3 Where they are on a Windows install, and how they are found

- The search covers every `.dll` up to 3 levels under `<Program Files | Program Files (x86)>\Universal
  Audio\Powered Plugins`: `console-rs/crates/oa-assets/src/firmware.rs:236-243`. Plug-in DLLs sit in
  `...\Powered Plugins\UAD-2 Powered Plugins`: `console-rs/crates/oa-assets/src/dsp.rs:76-83`.
- Search order: `console-rs/crates/oa-assets/src/firmware.rs:249-259`,
  `console-rs/crates/oa-assets/src/firmware.rs:383-416`.
  1. The strip-header plug-ins, by file name: `UAD Mic Input Strip Header.dll`,
     `UAD Input Strip Header.dll`, `UAD Talkback Input Strip Header.dll`
     (`console-rs/crates/oa-assets/data/firmware-pins.json:4-8`).
  2. `Windows\System32\UAD2SDK.dll` (`console-rs/crates/oa-assets/data/firmware-pins.json:9`).
  3. Every other Powered plug-in DLL, searched only for ids the first two lack.
- Path parts are matched case-insensitively: `console-rs/crates/oa-assets/src/lib.rs:26-44` (`ci_path`).
- The UAD version comes from `UA.xml` `<swReleaseName>` under `Program Files (x86)\Universal
  Audio\Powered Plugins` (Burn is 32-bit): `console-rs/crates/oa-assets/src/detect.rs:117-128`.
- The doc's copy list for a Windows source: `C:\Program Files\Universal Audio`,
  `C:\Program Files (x86)\Universal Audio`, and `C:\Windows\System32\UAD2SDK.dll` plus
  `UAD2DriverClient.dll`: `docs/uad-install.md:133-146`. `UAD2DriverClient.dll` is read only by the
  plug-in collector: `console-rs/crates/oa-assets/src/dsp.rs:43`. No Apollo and no plug-in licences
  are needed on the Windows side: `docs/uad-install.md:44-47`.
- USB FX3 boot images (`ApolloSolo.bin`, `ApolloTwin.bin`, `ApolloTwinX.bin` under
  `...\Powered Plugins\Firmware\USB`) are for USB Apollos only. Thunderbolt units do not need them:
  `console-rs/crates/oa-assets/src/firmware.rs:13-15`,
  `console-rs/crates/oa-assets/src/firmware.rs:267-272`.

### 1.4 Parsing and extraction (no transformation)

- The scan reads raw file bytes. There is no PE resource parsing and no decompression. At every
  4-byte-aligned offset it accepts a record when all of these hold:
  `console-rs/crates/oa-assets/src/firmware.rs:200-228`.
  - The bytes read `Bill`.
  - The id word's top byte is 0.
  - Word 2 equals 0x02000000.
  - Record length = 0x14 + payload-size word, and the record fits in the file.
- Record layout: `Bill`, id (arch byte 0 on disk), 0x02000000, payload size, w4, payload.
  Header = 0x14 bytes: `console-rs/crates/oa-assets/src/firmware.rs:47-50`.
- A candidate is "verified" only when length, SHA-256 and w4 all match a pin:
  `console-rs/crates/oa-assets/src/firmware.rs:161-164`. An unpinned required module is not
  written unless `--accept-unknown` is given: `console-rs/crates/oa-assets/src/firmware.rs:530-540`.
- The record is written byte-for-byte (`"transform": "none"`) as `dsp/<id>.bill`, with the id as
  3 lowercase hex digits, under `/lib/firmware/universal-audio`:
  `console-rs/crates/oa-assets/src/firmware.rs:41`,
  `console-rs/crates/oa-assets/src/firmware.rs:76-78`,
  `console-rs/crates/oa-assets/src/firmware.rs:541-546`.

### 1.5 How the Linux driver loads and checks them

- The driver reads `universal-audio/dsp/%03x.bill`, falling back to `ua_apollo/dsp/%03x.bill`,
  through `request_firmware`: `driver/ua_plugin_catalog.c:221-255`.
- Generic Bill check: `driver/ua_plugin_catalog.c:42-74`.
  - Magic 0x6c6c6942 (`driver/ua_plugin_host.h:218`).
  - id at most 0xffffff, kind at most 4, arch at most 2.
  - Payload = length - 20.
  - 1 <= w4 <= length/4 - 13.
  - Code pool words = floor(2 * w4 / 3), and must be even.
- Strip check: exact length, the id, w2 = 0x02000000, payload size, and w4 * 2 / 3 equal to the
  table's code-pool dwords: `driver/ua_strips.c:248-274`.
- The modules are read once at probe when `input_strips=1` and the model is in the strip table.
  A missing or unknown module disables the strips for that device:
  `driver/ua_strips.c:936-1028`.
- User space can only name a module by SHA-256. "There is no way to hand the driver code to
  upload": `driver/ua_plugin_catalog.c:7-13`.

### 1.6 When and how the driver uploads them

- **When:** at the first strip pass, `UA_STRIPS_SETTLE_MS` = 500 ms after the audio connect
  (`driver/ua_strips.c:760`, `driver/ua_strips.c:1303-1317`, `driver/ua_audio.c:3379-3380`). The
  pass also waits until the rate is acked to the DSPs and the firmware reports that rate
  (`driver/ua_strips.c:1647-1657`, `driver/ua_audio.c:5168-5180`). It repeats after every power
  cycle and after every rate change: the strips are torn down and rebuilt
  (`driver/ua_audio.c:4574`, `driver/ua_audio.c:4610`, `driver/ua_strips.c:1788-1800`).
- **Which DSP:** `input_strips_dsp`, default **1** (`driver/ua_strips.c:129-131`,
  `driver/ua_strips.c:966`). Its description text says "default 0", but the variable is 1. On DSP 0
  the strips feed the record channels but not the monitor mix: `HARDWARE-SESSIONS.md:327-328`.
- **How:** each module is one transaction on that DSP's rings with one 4-dword reply descriptor.
  - DMA command: `{0x00010000 | (len/4 + 2), dest, <record bytes>}`
    (`driver/ua_plugin_upload.c:23-28`, `driver/ua_plugin_upload.c:39-65`,
    `driver/ua_plugin_host.h:316`).
  - The default form (`input_strips_form=1`, `driver/ua_strips.c:108-111`) keeps the id word as on
    disk and clears record bytes 10-11, the high half of word 2 (`driver/ua_plugin_upload.c:57-63`).
  - Form 0, with the arch byte in the id, was refused by the x4 with
    `80070004 f0010005 020000eb`: `HARDWARE-SESSIONS.md:320-321`.
- **Accepted reply:** `{0x80070004, 0, id (with or without the arch byte), x}`
  (`driver/ua_plugin_upload.c:67-95`). Anything else quarantines the DSP until the next power
  cycle (`driver/ua_plugin_upload.c:181-225`).
- **Destinations:** bottom-up from the code-pool base 0xe0000: 0xe0000, 0xe006c, 0xe025c,
  0xe038c, 0xe03a2 for EB, DA, A5, C2, 12B (`driver/ua_strips.c:38-41`,
  `driver/ua_strips.c:485-496`, `HARDWARE-SESSIONS.md:191-192`).
- **Word 0 per module (derived from len/4 + 2):** EB 0x000100B5, DA 0x000102FB, A5 0x000101E7,
  C2 0x00010030, 12B 0x00010073.
- **Release at teardown:** `{0x00030002, id, 0, 0}` for each module whose last hold goes
  (`driver/ua_plugin_upload.c:162-170`, `driver/ua_plugin_host.h:322`).

### 1.7 Things that are *not* from the UAD install, and are off or unused

| Item | What it is | State | Cite |
|---|---|---|---|
| `ua-apollo-mixer.bin` ('UAFM') | Captured SEL127 "Bill" streams, sent as 0x12 transactions | Off (`mixer_fw_stage=0`) | `driver/ua_apollo.h:536-542`, `driver/ua_dsp.c:2840-2869`, `driver/ua_core.c:83-86` |
| `ua-apollo-plugin-chain.bin` ('UAPC') | DMA payloads from a Windows ring capture | Off (`no_plugins=1`): it faulted the IOMMU and DSP 0 stopped consuming | `driver/ua_audio.c:65-67`, `tools/build-plugin-chain-firmware.py:1-27`, `driver/ua_dsp.c:3442-3505` |
| `ua_dsp_configure_modules` | x4 Windows-captured module setup (`ua_dsp_modules.h`) | Never called | `driver/ua_dsp.c:4263-4264`, `driver/ua_dsp_modules.h:1-10` |
| Captured IO descriptors (`ua_routing.h`) | Copies of 0xC1A4/0xC2C4 | Legacy `UA_IOCTL_SEND_ROUTING` only, which nothing calls. The firmware owns these areas. | `driver/ua_routing.h:7-16`, `driver/ua_dsp.c:4602-4621` |
| Licensing block loaders 0xD0000/0xE0000 | Kext authorization and demo blocks | Never sent | `driver/ua_dsp.c:2901-2905` |

### 1.8 Windows simplification and implications

- With UAD installed natively there is nothing to mount. Read the DLLs from `%ProgramFiles%`,
  `%ProgramFiles(x86)%` and `%SystemRoot%\System32` in place, with the same raw `Bill` scan and the
  same SHA-256 pins.
- A 32-bit process reading `System32` is redirected by WOW64. Use a 64-bit process or `Sysnative`
  (inference, general Windows behaviour).
- A Windows kernel driver has no `request_firmware`. Keep the Linux property of no arbitrary code
  upload from user mode (inference): either
  - the driver reads `<id>.bill` files from an admin-only directory, or
  - the service passes the bytes by IOCTL,
  and in both cases the driver re-checks length, w4 and SHA-256 against pins compiled into it.
- The driver uploads the modules itself at each strip pass (1.6), not once at install.

---

## 2. DSP ring-buffer command protocol

### 2.1 Register map (BAR0)

| Item | Offset | Cite |
|---|---|---|
| DSP i ring bank, i < 4 | 0x2000 + i*0x80 | `driver/ua_apollo.h:486-488`, `driver/ua_apollo.h:1723-1730` |
| DSP i ring bank, i >= 4 | 0x5E00 + i*0x80 (DSP4 0x6000, DSP5 0x6080: derived) | same |
| Command ring | bank + 0x00 | `driver/ua_apollo.h:508` |
| Response ring | bank + 0x40 | `driver/ua_apollo.h:509` |
| Per ring: PAGE0_LO, HI ... PAGE3_HI | +0x00, 0x04, ... 0x1C | `driver/ua_apollo.h:491-498` |
| Per ring: WRITE_PTR | +0x20 | `driver/ua_apollo.h:499` |
| Per ring: DOORBELL | +0x24 | `driver/ua_apollo.h:500-502` |
| Per ring: POSITION (FPGA consumer index) | +0x28 | `driver/ua_apollo.h:503` |
| DMA_CTRL | 0x2200 | `driver/ua_apollo.h:270` |
| v2 reset strobes | DMA_CTRL [16:9], mask 0x1FE00 | `driver/ua_apollo.h:567-568` |
| Per-DSP enable | DMA_CTRL bit (i+1); bit 0 is the global enable | `driver/ua_dsp.c:803-828` |
| DSP i register window | i*0x800, plus 0x2000 for i > 3 | `driver/ua_dsp.c:3083-3087` |
| Window +0x1A4 | boot/ready word (3 = no host since power-up) | `driver/ua_plugin_host.h:274-275` |
| Window +0x0C | measured load | `driver/ua_dsp.c:5105-5114` |
| Window pools | size +0x10+4n, base +0x184+4n, scratch +0x190+4n | `driver/ua_plugin_host.h:282-284` |
| Window FIFO | write index +0x40, 16 slots of 5 dwords from +0x44 (read only) | `driver/ua_dsp.c:4693-4705` |
| DSP count | ext_caps 0x2234 bits [15:8]; the x8p has 6 DSPs | `driver/ua_apollo.h:266`, `driver/ua_routing.h:137-138` |

### 2.2 Ring memory and indices

- Every ring, command and response, is 4 coherent 4 KiB pages: 1024 entries of 16 bytes,
  circular: `driver/ua_apollo.h:506-507`, `driver/ua_apollo.h:510-517`, `driver/ua_dsp.c:838-840`.
- Slot address: page = idx / 256, offset = (idx % 256) * 16: `driver/ua_dsp.c:707-711`.
- POSITION is the FPGA's consumer index, 0..1023. A read of 1024 or more is invalid and the last
  valid value is kept. A read of 0xFFFFFFFF means the device is gone: `driver/ua_dsp.c:119-145`.
- free = (POSITION - write_ptr - 1) & 1023, so one slot is always empty.
  pending = (write_ptr - POSITION) & 1023. write_ptr == POSITION means idle:
  `driver/ua_dsp.c:130-151`.

### 2.3 Bring-up, resync and reprogram

1. **Neutralize, before bus mastering** (probe and resume), for every DSP and both rings.
   This is register-only and needs no ring memory: `driver/ua_dsp.c:510-556`,
   `driver/ua_dsp.c:1116-1143`.
   - Read POSITION, treating a value of 0x400 or more as 0.
   - Write DOORBELL = POSITION, then WRITE_PTR = POSITION.
   - Repeat up to 3 times until POSITION stops moving.
2. **Program each ring:** `driver/ua_dsp.c:586-654`, `driver/ua_dsp.c:843-896`.
   - Resync as in step 1.
   - Write PAGE0..3 LO/HI. Unused pages get 0, but every ring uses all 4.
   - Read the addresses back and compare them.
   - Zero the pages.
3. **DMA_CTRL** (v2, `dma_per_dsp` default on): `driver/ua_dsp.c:792-801`,
   `driver/ua_dsp.c:914-955`.
   - en = OR of BIT(i+1) over the DSPs that have rings.
   - Write `(DMA_CTRL & ~0x1FE00) | en`.
   - cache = `read(DMA_CTRL) & ~0x1FE00`.
   - Write `cache | (en << 8)` to pulse the strobes, then write `cache`, then read to flush.
   - Resync every ring again.
   - "Without this, the FPGA won't process ring entries (POSITION stays at 0)":
     `driver/ua_dsp.c:914-918`. The old OR of 0x1FFFE left every strobe asserted, and after a
     power cycle the DSP command rings were not serviced: `CHANGELOG.md:226`.
4. **After any DMA reset pulse,** rewrite the page addresses without draining. For example
   [audio]'s boot transport start on AudioExtension devices: `driver/ua_dsp.c:1175-1208`,
   `driver/ua_core.c:4383-4391`.
5. **A mid-session reprogram** first lets queued entries drain for up to 20 ms:
   `driver/ua_dsp.c:509-510`, `driver/ua_dsp.c:613-618`.

### 2.4 Entry formats

| Kind | word0 | word1 | word2 | word3 | Cite |
|---|---|---|---|---|---|
| Inline command | `(opcode << 16) \| length_in_dwords`, bit 31 clear | arg | arg | arg | `driver/ua_dsp.c:16-18`, `driver/ua_strips.c:189-190`, `driver/ua_dsp.c:4043` |
| DMA reference | `0x80000000 \| dwords` (dwords <= 0xFFFF) | 0 | bus addr lo | bus addr hi | `driver/ua_apollo.h:519`, `driver/ua_dsp.c:1481-1504` |

- The FPGA fetches the referenced dwords and processes them as command stream.
- A DMA-carried command starts with its own `opcode << 16 | total dwords` word: `0x0014nnnn`
  write, `0x0015nnnn` memspec, `0x0001nnnn` code upload (`driver/ua_plugin_host.h:316-323`,
  `driver/ua_strips.c:555-577`).
- One command may span consecutive DMA_REFs. For example `_sendBlock` sends a header DMA_REF, then
  data DMA_REFs: `driver/ua_dsp.c:2085-2145`.
- Header form for blocks: payload < 0x3FFFC bytes uses one word `(dwords + 1) | cmd`. Larger
  payloads use `{cmd | BIT(30), dwords + 2}`: `driver/ua_dsp.c:2085-2117`.
- **Piece size: the two paths disagree.**
  - The plug-in/strip transaction code never lets a DMA_REF cross a 4 KiB page. It sends a command
    of 4 KiB or less as one DMA_REF, because "1 KiB pieces took the x4 off the bus":
    `driver/ua_dsp_xact.c:110-148`.
  - `_sendBlock` defaults to 1 KiB pieces ("proven"): `driver/ua_dsp.c:1294-1314`.
  - Use the transaction rule for strip work.

### 2.5 Submitting

- Write the entries into ring memory and issue a write barrier. Then write **DOORBELL (+0x24) =
  write_ptr first, WRITE_PTR (+0x20) = write_ptr second.** The reverse order reset the DMA engine
  during large DMA refs and stalled the ring: `driver/ua_dsp.c:736-767`.
- One doorbell per batch is fine: `driver/ua_dsp.c:3538-3615`. Reserve room before queueing a
  batch: `driver/ua_dsp.c:153-186`, `driver/ua_dsp.c:691-716`.
- Inline commands from the driver go through `ua_dsp_ring_send_raw`, which refuses DMA_REFs from
  user space: `driver/ua_dsp.c:4016-4065`.
- **Buffer lifetime:** a buffer referenced by a queued DMA_REF or response descriptor must stay
  alive until POSITION passes it. On a timeout, park it; do not free it:
  `driver/ua_dsp.c:230-259`.

### 2.6 Responses

- **Queue the reply first.** Put a descriptor `{0x80000000 | 4, 0, lo, hi}` on the response ring,
  pointing at a zeroed 4-dword buffer, and ring the response doorbell. Then queue the command:
  `driver/ua_dsp.c:1464-1478`, `driver/ua_dsp.c:2058-2080`, `driver/ua_dsp_xact.c:14-19`,
  `driver/ua_dsp_xact.c:334-341`.
- **Ordering hazard.** The DSP fills response descriptors in ring order, so a stale descriptor
  takes the next reply. Before any command that expects a reply, resync the response ring if it
  has pending entries: `driver/ua_dsp.c:2012-2026`, `driver/ua_dsp_xact.c:282-305`.
- **Completion** is either:
  - resp[0] != 0. `_sendBlock` polls every 100-200 us for the first 200 polls, then every 1-2 ms:
    `driver/ua_dsp.c:2162-2171`.
  - or the response ring's POSITION is past the descriptor: `driver/ua_dsp_xact.c:405-447`.
- **Decoding:**
  - High half of word 0: 0x8004 for firmware (`driver/ua_apollo.h:526`), otherwise param >> 16
    when the param has bit 31 set (`driver/ua_dsp.c:2206-2208`).
  - Word 1 is the DSP status: 0 = OK. 0xF0010001, 0002, 0006, 0007, 0011, 0012 and 0013 map to
    kext errors. 0xF002xxxx is its own class. Anything else is "bad response":
    `driver/ua_dsp.c:1214-1277`.
  - Upload reply: 0x80070004, status 0, id: `driver/ua_plugin_upload.c:81-95`.
  - By default a non-zero status is logged and counted as success; `block_strict=1` turns it into
    -EIO: `driver/ua_dsp.c:1284-1292`.
- **Commands with no reply** (connect, bus coefficients, input params) are fire-and-forget. A
  caller that must know they landed waits for POSITION == write_ptr: `driver/ua_dsp.c:197-221`.

### 2.7 Timeouts

| Wait | Value | Cite |
|---|---|---|
| Ring space before a block | 1000 ms | `driver/ua_dsp.c:2007-2010` |
| Ring space for bus-coefficient replay or move | 50 ms per entry | `driver/ua_dsp.c:3750-3753` |
| Block reply (`_sendBlock`) | 5000 ms | `driver/ua_apollo.h:528` |
| Firmware load reply | 150000 ms | `driver/ua_apollo.h:527` |
| Command-buffer consumption (SRAM data, raw DMA, module config) | 6000 ms | `driver/ua_dsp.c:188-195` |
| Plug-in/strip transaction | 1000 ms; publish retry 50 ms | `driver/ua_plugin_host.h:133-134` |
| Plugin-chain batch idle | 15 ms | `driver/ua_dsp.c:3617-3618` |
| Connect | No reply; 5-10 ms sleep after each doorbell | `driver/ua_dsp.c:3007`, `driver/ua_dsp.c:3046` |
| DSP ready word | 300 ms x 100 polls (DSP 0), x 10 (others); only when ext_caps[7:4] == 1 | `driver/ua_dsp.c:3065-3081`, `driver/ua_dsp.c:3115-3123` |
| Drain before a mid-session reprogram | 20 ms | `driver/ua_dsp.c:510` |
| Generic poll step | 1-1.5 ms | `driver/ua_dsp.c:183`, `driver/ua_dsp.c:219` |

A plug-in/strip transaction that ends unproven (no reply, abandoned entry) quarantines the DSP
until the next power cycle: `driver/ua_dsp_xact.c:519-549`.

### 2.8 Command catalogue (what the minimum path uses)

| word0 | Args (w1, w2, w3) | Target | Meaning | Cite |
|---|---|---|---|---|
| 0x00230002 | 1, 0, 0 | every DSP with ring connect | DSP connect | `driver/ua_apollo.h:533`, `driver/ua_dsp.c:2946-3013` |
| 0x00100002 | date, 0, 0 | DSP 0, queued behind its connect entry | Connect finalize. date = (days since 2007-01-01 UTC << 12) + minute of day; 0 before 2007. The same word alone is the disconnect. | `driver/ua_apollo.h:534`, `driver/ua_dsp.c:769-790`, `driver/ua_dsp.c:3018-3044`, `driver/ua_dsp.c:5035-5056` |
| 0x001A0002 | rate_Hz, 0, 0 | every DSP | Rate ack | `driver/ua_dsp.c:4186-4213` |
| 0x001C0003 | bus 0-6, enable, 0 | every DSP | Bus enable (cache starts all-enabled) | `driver/ua_dsp.c:3654`, `driver/ua_dsp.c:4067-4124` |
| 0x001D0004 | bus 0x00-0x1F, `(side << 24) \| param` (param 0-6), float bits | owner DSP and DSP 0 | Bus coefficient (fader x pan, sends) | `driver/ua_dsp.c:3633-3652`, `driver/ua_dsp.c:3770-3833` |
| 0x001E0004 | input 0-31, sub (5 scale, 7 route, 8 span), value | DSP 0; route also 0x1FFFF to DSPs 1+ | Input param | `driver/ua_apollo.h:630-640`, `driver/ua_dsp.c:4137-4184` |
| 0x001F0004 | `0x20 << 16 \| synch`, header address (0 = stop), 0 | strip DSP | SYNCH: start or stop a strip | `driver/ua_strips.c:193-194`, `driver/ua_strips.c:579-601`, `driver/ua_strips.c:747-752` |
| 0x00080004 | addr, 0, dwords | strip DSP | Zero-fill. Also the SRAM_CFG word before a data DMA_REF. | `driver/ua_plugin_host.h:319`, `driver/ua_strips.c:620-622`, `driver/ua_dsp.c:2289-2296` |
| 0x00030002 | id, 0, 0 | strip DSP | Release a code module | `driver/ua_plugin_host.h:322` |
| DMA 0x0001nnnn | dest, record | strip DSP | Code upload, reply 0x80070004 | `driver/ua_plugin_host.h:316-318` |
| DMA 0x0014nnnn | addr, words... | strip DSP | Write words | `driver/ua_plugin_host.h:321`, `driver/ua_strips.c:565-577` |
| DMA 0x0015nnnn | (target, value) pairs | strip DSP | Memspec link table | `driver/ua_plugin_host.h:320`, `driver/ua_plugin_upload.c:97-160` |

### 2.9 Interrupts (optional)

- Linux polls POSITION and enables no DSP vector by default (`dsp_msg_irq=0`):
  `driver/ua_dsp.c:4679-4711`.
- Each DSP has five vectors, 5n+j: command consumed, response filled, message FIFO, error A,
  error B. With more than 4 DSPs the mapping is 4n+j and error B is missing, which applies to the
  x8p (6 DSPs): `driver/ua_dsp.c:4683-4687`, `driver/ua_dsp.c:4713-4722`.

### 2.10 Sender sketch (per DSP, under one device lock)

```
send_inline(dsp, w0..w3):            # w0 bit 31 clear
    wait_free(cmd, 1)                # refresh POSITION, free = (pos - wr - 1) & 1023
    slot(cmd, wr) = {w0,w1,w2,w3}; wr = (wr + 1) & 1023
    wmb(); write(cmd+0x24, wr); write(cmd+0x20, wr)

send_with_reply(dsp, payload_dwords[]):
    if pending(resp): resync(resp)   # DOORBELL = WRITE_PTR = POSITION, park old buffers
    rbuf = coherent(4 dwords, zeroed); put(resp, {0x80000000|4, 0, lo(rbuf), hi(rbuf)}); doorbell(resp)
    pbuf = coherent(payload, page-aligned); for each <=4 KiB page piece:
        put(cmd, {0x80000000|piece_dwords, 0, lo, hi})
    doorbell(cmd)
    poll rbuf[0] != 0 (or resp POSITION past the slot) until the timeout (2.7)
    on timeout: park rbuf/pbuf until POSITION passes them; treat DSP as unproven
```

---

## 3. The mixer-settings mailbox and the hardware readback (BAR0 0x3800)

This is the second DSP path. Preamp and monitor controls use it, not the rings:
`driver/ua_dsp.c:3647-3650`, `driver/ua_core.c:2994-2999`.

**Registers**

| Item | Value | Cite |
|---|---|---|
| SEQ_WR (host to DSP) | 0x3808 | `driver/ua_apollo.h:329-331` |
| SEQ_RD (DSP ack) | 0x380C | same |
| Settings 0-15 | 0x3800 + 0xB4 + i*8 | `driver/ua_apollo.h:1705-1721` |
| Settings 16-31 | 0x3800 + 0xBC + i*8 | same |
| Settings 32-37 | 0x3800 + 0xC0 + i*8 | same |
| Batch | 38 settings | `driver/ua_apollo.h:333-334` |
| Readback status | 0x3810: 1 = ready; write 0 to re-arm | `driver/ua_apollo.h:895-898` |
| Readback data | 40 words from 0x3814 | same |

**Encoding:** wordA = `(mask[15:0] << 16) | val[15:0]`, wordB = `(mask[31:16] << 16) | val[31:16]`:
`driver/ua_apollo.h:325-327`, `driver/ua_core.c:1514-1519`.

**Protocol (the driver owns it; user space never writes the registers):**

1. A write merges `val`/`mask` into a per-setting cache and marks it dirty. It is refused with
   -EAGAIN until `mixer_ready`: `driver/ua_core.c:1424-1451`.
2. Flush, only when SEQ_RD == the cached SEQ_WR: `driver/ua_core.c:1465-1535`.
   - Write every setting whose mask is non-zero.
   - Write zeros once to every setting written on the previous flush (the one-shot "mailbox").
   - Clear the written masks.
   - Bump SEQ_WR once for the whole batch.
   - Settings with mask 0 are skipped, which keeps the firmware defaults:
     `driver/ua_core.c:1494-1502`.
3. The mailbox behaviour is required. Re-sending persistent masks left PAD, 48V and Mic/Line dead:
   `driver/ua_core.c:103-119`.
4. "The DSP reads ALL 38 settings atomically on each SEQ_WR advance": `driver/ua_core.c:1373-1393`.
5. A 10 ms service loop does the flush. Each pass it: `driver/ua_audio.c:2361-2501`,
   `driver/ua_apollo.h:341-344`.
   - reads 0xC030 and clears it by writing 0. It is not write-1-to-clear: `driver/ua_audio.c:2392-2428`.
   - drains the readback: if 0x3810 == 1, read 40 words, then write 0: `driver/ua_audio.c:2434-2455`.
   - flushes, from the 5th tick on: `driver/ua_audio.c:2457-2462`.
6. Only one consumer may own the one-shot readback status, so the ioctl returns the cached copy:
   `driver/ua_core.c:3460-3480`.

**Readback fields used here**

- Word 0: preamp 1-4 flags, 6 bits each (+0 Mic/Line, +1 PAD, +2 Link, +3 48V, +4 LowCut,
  +5 Phase). Bits 24/25: Hi-Z jack on input 1/2.
- Word 2: [7:0] monitor level, [15:8] HP1, bit 16 mute, bit 17 mono, bit 31 dim.
- Word 3: GainC of preamps 1-4 at shifts 0/6/16/22; dB = GainC + 9.
- Words 5 and 7: the same for preamps 5-8.

Sources: `driver/ua_apollo.h:842-893`, `mixer-engine/hardware.py:5233-5256`. The Mixer Engine drops
a snapshot whose word 38 or 39 is 0xFFFFFFFF: `mixer-engine/hardware.py:5265-5269`.

---

## 4. Boot-time mixer state

### 4.1 What the driver programs by itself (default module parameters, cold start)

1. Rings: neutralize, init, per-DSP DMA_CTRL enable (2.3). Then SEQ_WR = 0:
   `driver/ua_core.c:4316-4335`.
2. No firmware stage and no plugin chain, both off by default (1.7).
3. Ring connect (2.8). Which devices get it: `driver/ua_apollo.h:141-157`,
   `driver/ua_apollo.h:187-197`, `driver/ua_core.c:3675-3693`.
   - Non-AudioExtension types (the x8p) at probe: `driver/ua_core.c:4472-4477`.
   - AudioExtension types (x4, Twin X) right after the ACEFACE handshake:
     `driver/ua_core.c:3695-3726`, `driver/ua_audio.c:3158-3163`.
4. Readback liveness poll after the connect: write 0x3810 = 0, then poll for 1 up to 50 x 10 ms:
   `driver/ua_core.c:4511-4532`.
5. On the [audio] ACEFACE connect:
   - Bus-coefficient replay, only if something was cached. Nothing is cached on a fresh load, so
     nothing is sent: `driver/ua_audio.c:3202-3212`, `driver/ua_dsp.c:3936-3939`.
   - **Rate ack `{0x001A0002, rate_Hz}` to every DSP:** `driver/ua_audio.c:3214-3223`. A DSP that
     has not been told the rate stalls on the strip's SYNCH: `HARDWARE-SESSIONS.md:322-323`.
   - **Mailbox init:** `driver/ua_audio.c:3225-3308`.
     - SEQ_WR = SEQ_RD and the cache is zeroed.
     - Settings 1..37 except 2: val 0x20, mask 0xFF.
     - Setting 35: val 0x05, mask 0x1F.
     - Dirty, so it goes out on the first flush.
     - Skipped only on an inherited DSP, which means a warm x4.
   - Output unmute on types that have an output mute. The x8p and Twin X do not:
     `driver/ua_audio.c:3310-3318`, `driver/ua_core.c:1559-1582`, `driver/ua_apollo.h:190`.
   - Then the 10 ms service loop starts: `driver/ua_audio.c:9262-9271`.
6. Input strips, only with `input_strips=1` (default **off**) and only for the x4 and Twin X Gen 2
   (0x3B, 0x3C, 0x40, 0x41): `driver/ua_strips.c:80-83`, `driver/ua_strips.c:770-789`.
7. Nothing else. No routing tables (`driver/ua_routing.h:7-16`). No module config
   (`driver/ua_dsp.c:4263-4264`). No bus coefficients. No preamp or monitor values beyond item 5.

**Item 5's 0x20 batch is not explained consistently.**

- One comment says setting 24 = 0x20 "enables type 0x01 capture": `driver/ua_audio.c:3251-3259`.
- The same block says the low byte of S3, S13, S16-17 and S23 is preamp 1-2 gain, Hi-Z impedance
  and knob steps: `driver/ua_audio.c:3289-3296`.
- So until the daemon writes gains, the batch puts preamp 1 at GainC 0x20 (41 dB) and GainB/GainA
  0x20 (42 dB) (derived). Its purpose is unproven. Keep it for parity with the verified runs.

### 4.2 What the daemon sends

- About 4 s after start: `mixer-engine/ua_mixer_daemon.py:1333-1342`. 3 s after a late device
  appears: `mixer-engine/ua_mixer_daemon.py:1522-1536`.
- It sends `_push_defaults_to_hardware`: `mixer-engine/ua_mixer_daemon.py:3014-3359`. Section 6.3
  lists every item.
- Every `SET_MIXER_PARAM` is throttled to at least 15 ms apart:
  `mixer-engine/hardware.py:1915-1922`.

### 4.3 Is anything passing at power-on? The evidence

- **Bus coefficients:** a fresh DSP holds the firmware defaults, "bus coefficients zero, every bus
  enabled": `driver/ua_dsp.c:3906-3913`.
- **Output routing and settings:** the firmware has default mixer output routing, and SRAM clears
  "destroy firmware's default mixer output routing": `driver/ua_dsp.c:4424-4426`. It also
  initialises the settings itself, and writing zeros over them "breaks" monitor processing:
  `driver/ua_core.c:1496-1502`. The firmware owns the IO descriptors: `driver/ua_dsp.c:4615-4621`.
- **Analog inputs need strips:** they "reach the DSP mix, the meters and the host only through the
  Console's input strips". After a Linux cold start they stay at zero without them:
  `driver/ua_strips.c:7-13`. Virtual channels "mix nothing and record silence" without strips:
  `driver/ua_strips.c:19-21`, `README.md:239-241`, `CHANGELOG.md:18-26`.
- **Twin X playback needs the ring connect:** without it, playback was silent:
  `CHANGELOG.md:50-52`.
- **Phantom power:** a preamp reported with 48V on stays muted after a cold start until 48V is
  written off and then on: `driver/ua_core.c:2458-2474`.
- **Warm units:** if another OS's driver started the DSPs and the unit stayed powered, that OS's
  strips remain. Our driver will not own the DSP, and inputs stay as that session left them until
  a power cycle: `driver/ua_plugin_place.c:13-21`, `HARDWARE-SESSIONS.md:227-232`.

**Conclusion.**

- Playback to monitor needs only the driver's connect sequence; level and mute are the unit's.
- Mic to capture needs a host-built input strip on the x4 and Twin X Gen 2. Plan for the same on
  the x8p and Twin X Gen 1 until a power-cycled test says otherwise.
- The daemon is not needed for signal to flow. It is needed for gain, 48V, Mic/Line, monitor level
  and mute, and for the mic in the monitor mix.

---

## 5. Preamp control

### 5.1 The "ARM CLI" path is dead; preamps go through the mailbox

- `ua_cli_send_locked` returns -ENOSYS unconditionally, because "CLI commands freeze the ARM MCU":
  `driver/ua_core.c:1300-1327`.
- The CLI ioctl also returns -ENOSYS: `driver/ua_core.c:2966-2969`.
- The kext's `setInputParamARM` routes every preamp param to `CPcieDeviceMixer::WriteSetting`:
  `driver/ua_core.c:2508-2511`, `driver/ua_core.c:2994-2999`, `mixer-engine/hardware.py:320-323`.
- The ARM MCU and the PGA2500 SPI are internal to the unit. Only `ARCHITECTURE.md:98` mentions SPI
  or PGA2500 (inference from a full-tree search).
- Stale text that still says "ARM CLI": `ARCHITECTURE.md:94-98`, `driver/ua_ioctl.h:176-189`, and
  the unused helper `driver/ua_core.c:1715-1745`.
- The CLI registers, for reference only: 0xC3F4..0xC480 (`driver/ua_apollo.h:463-467`).

### 5.2 Input parameters (`SET_MIXER_PARAM`, channel_type 1, channel_idx = preamp index from 0)

| param | Name | Value | Setting (ch 0-3 / ch 4-7) | Bits | Cite |
|---|---|---|---|---|---|
| 0x00 | Mic/Line | 1 = Line, 0 = Mic | S0 / S12 | bit 6*(ch&3) | `driver/ua_apollo.h:594`, `driver/ua_core.c:2556-2577` |
| 0x01 | PAD | 1/0 | S0 / S12 | bit 6*(ch&3)+1 | same |
| 0x02 | Stereo link | even channel only | S0 / S12 | mask 0x104 << 12*((ch&3)>>1) | `driver/ua_core.c:2579-2586` |
| 0x03 | 48V | 1/0 ("triggers ARM safety blink sequence") | S0 / S12 | bit 6*(ch&3)+3 | `driver/ua_apollo.h:588`, `driver/ua_core.c:2556-2577` |
| 0x04 | LowCut | 1/0 | S0 / S12 | +4 | same |
| 0x05 | Phase | 0x3f800000 normal, 0xbf800000 invert (0/1 accepted) | S0 / S12 | +5 | `driver/ua_core.c:2567-2576` |
| 0x06 | GainC | dB - 9, 6 bits | S3 / S14 | 0x3F << (6*(ch&3) + (ch&2 ? 4 : 0)) | `driver/ua_core.c:2587-2598` |
| 0x07 | Mic impedance | code | S1 / S13 | nibble at 9/16/20/24 | `driver/ua_core.c:2599-2604` |
| 0x08 | Custom (Unison) preamp | 1/0 | S11 | bit 8+ch | `driver/ua_core.c:2605-2609` |
| 0x09 | GainB | dB - 10 | S16 / S24 | byte (ch&3) | `driver/ua_core.c:2610-2616` |
| 0x0A | GainA | dB - 10 | S17 / S25 | byte (ch&3) | same |
| 0x11 | Hi-Z impedance | code 0-7 (2.2M, 1M, 453k, 232k, 100k, 70k, 47k, 21k) | S13 | nibble 4*ch, Hi-Z inputs only | `driver/ua_core.c:2623-2631`, `mixer-engine/hardware.py:625-629` |
| 0x13 | Direct-out route | ring `{0x001E0004, ch, 7, v}` to DSP 0; 0x1FFFF to the other DSPs | - | - | `driver/ua_core.c:2675-2676`, `driver/ua_dsp.c:4165-4181` |
| 0x14 | Hi-Z host override | 1/0 | S0 | bit 24+ch (ch < 4) | `driver/ua_core.c:2638-2644` |
| 0x15 | PGA disabled in line mode | 1/0, kind-gated | S15 | bit 24+ch | `driver/ua_core.c:2645-2651` |
| 0x16 | "Level" (span info in the driver) | ring `{0x001E0004, ch, 8, v}` to DSP 0 | - | - | `driver/ua_core.c:2677-2678`, `driver/ua_apollo.h:627` |
| 0x17 | Raw record enable | 1/0 | S5 | bit 16+ch | `driver/ua_core.c:2652-2658` |

**Channel limits**

- The highest channel is set per mixer kind: x8p public 0x16, kind 12, max 7. Twin X public 0x19,
  kind 15, max 1. Twin X Gen 2 public 0x25/0x29, kind 21, max 1. `driver/ua_core.c:1782-1793`,
  `driver/ua_core.c:1807-1819`, `driver/ua_core.c:2547-2550`.
- The ioctl refuses channel_idx >= num_preamps except for 0x13, 0x16, and phase above the preamps:
  `driver/ua_core.c:3008-3020`.

### 5.3 Gain

- Range 10..65 dB: `driver/ua_apollo.h:608-610`.
- Written as a triplet, GainA = dB-10, GainB = dB-10, GainC = dB-9, in the order A, B, C. The
  driver's own control writes all three under one lock, so they share one mailbox commit:
  `driver/ua_audio.c:6948-6964`.
- The daemon sends three ioctls, clamping A and B to 54 and C to 56: `mixer-engine/hardware.py:3342-3376`.
- Example (derived): 40 dB gives 0x0A = 30, 0x09 = 30, 0x06 = 31.
- Gain is refused while the custom (Unison) preamp owns the channel: `driver/ua_audio.c:6956-6959`.
- Readback: GainC in word 3 (word 7 for preamps 5-8); dB = GainC + 9; 0x39 and above reads as
  65 dB: `driver/ua_apollo.h:865-872`, `mixer-engine/hardware.py:5315-5323`.

### 5.4 48V after a cold start

- The phantom supply only comes up on a written 0 then 1 edge. The kext gets that edge because
  "its first settings write zeroes every setting once": `driver/ua_core.c:2458-2474`.
- `ua_preamp_phantom_resync`: `driver/ua_core.c:2475-2499`.
  - It acts only on preamps the unit reports with 48V on, and only for channels 0-3.
  - It writes 0, waits 200 ms (a separate mailbox sequence), then writes 1.
- It runs after the strips go live: `driver/ua_strips.c:1772-1783`, `HARDWARE-SESSIONS.md:329-331`.

### 5.5 Input select (mic, line, Hi-Z)

- **Mic/Line:** param 0x00, 1 = Line. The daemon maps IOType "Line" to 1:
  `mixer-engine/hardware.py:3585-3595`.
- **Hi-Z is jack-detected.** Readback word 0 bit 24 = input 1, bit 25 = input 2:
  `driver/ua_apollo.h:851-856`.
  - The host can override it with param 0x14, but only with jack sensing disabled (monitor param
    0x45 = 1, setting 7 bit 11): `mixer-engine/hardware.py:3673-3703`, `mixer-engine/hardware.py:576`.
  - Hi-Z inputs: x8p inputs 1-2 (`devices/apollo-x8p.json:18`); Twin X input 1
    (`devices/apollo-twin-x.json:12`).

---

## 6. Minimum command sequence

**Labels.** **R** = required for (a) or (b). **L** = required for a sane monitor level. **K** = in
every verified run, purpose unproven, so keep it. **C** = cosmetic, or not needed for (a)/(b).

### 6.1 Apollo x8p

The x8p is device type 0x22, public type 0x16, mixer kind 12, 6 DSPs and 8 preamps. It is a
ring-connect type, not AudioExtension: `devices/apollo-x8p.json:3-25`, `driver/ua_apollo.h:141-157`.

**Phase A: probe, cold (Apollo power-cycled, nothing else has bound since)**

| # | Who | Action | Values | Label | Cite |
|---|---|---|---|---|---|
| A1 | [core] | BAR0 map, program registers, mask IRQs; read num_dsps and public_type | - | R | `driver/ua_core.c:4295-4300` |
| A2 | here | Neutralize all 12 rings, before bus master | 2.3 step 1 | R | `driver/ua_dsp.c:1116-1143` |
| A3 | here | DSP ready wait, if ext_caps[7:4] == 1 | window +0x1A4 not 0 and not 0xa8caed0f | K | `driver/ua_dsp.c:3100-3162` |
| A4 | here | Rings init (4 pages each) plus DMA_CTRL per-DSP enable | 2.3 steps 2-3 | R | `driver/ua_dsp.c:843-1031` |
| A5 | here | SEQ_WR = 0 | write 0x3808 = 0 | K | `driver/ua_core.c:4335` |
| A6 | [audio] | SG table preinit before any transport | - | R | `driver/ua_core.c:4356-4370` |
| A7 | here | Ring connect: each DSP 0..5 gets `{0x00230002, 1, 0, 0}` plus doorbell; then DSP 0 gets `{0x00100002, date, 0, 0}` | 2.8 | R | `driver/ua_core.c:4472-4477`, `driver/ua_dsp.c:2955-3059` |
| A8 | here | Readback liveness | 0x3810 = 0, poll for 1 | C | `driver/ua_core.c:4511-4532` |

**Phase B: audio connect (ACEFACE; [audio] owns the handshake)**

| # | Who | Action | Values | Label | Cite |
|---|---|---|---|---|---|
| B1 | [audio] | ACEFACE handshake and post-handshake reads | 0xC02C = 0x0ACEFACE, 0x2260 = 1, poll 0xC030 bit 21 | R | `driver/ua_audio.c:3036-3157` |
| B2 | here | Rate ack to DSPs 0..5 | `{0x001A0002, 48000, 0, 0}` | R | `driver/ua_audio.c:3214-3223` |
| B3 | here | Mailbox init | SEQ_WR = SEQ_RD; S1..S37 except S2 = 0x20/0xFF; S35 = 0x05/0x1F | K | `driver/ua_audio.c:3237-3308` |
| B4 | here | Start the 10 ms service loop (notif ack, readback drain, flush) | - | R | `driver/ua_audio.c:2361-2501`, `driver/ua_audio.c:9269-9271` |
| B5 | here | **Input strips for preamp 1 (and the rest)** | 6.4. Needs a model entry `{0x22, first_synch 0, first_virtual 0x18}` (derived from `devices/apollo-x8p.json:193-221`) and the pool-layout check to pass on this unit | R if the x8p behaves like the x4 (open) | `driver/ua_strips.c:770-789`, `driver/ua_strips.c:440-483` |
| B6 | here | 48V resync after the strips | 5.4; extend to preamps 5-8, which use word 5 bit 6*(ch&3)+3 (derived from `driver/ua_apollo.h:842-848` and `mixer-engine/hardware.py:5307`) | R for condenser mics | `driver/ua_core.c:2475-2499` |

**Phase C: host init, once `mixer_ready` (minimum subset of 6.3)**

| # | ioctl (type, idx, param, value) | Purpose | Label |
|---|---|---|---|
| C1 | GET_HW_READBACK; accept only if status == 1 and words 38/39 != 0xFFFFFFFF | Keep the unit's own monitor level | L |
| C2 | (1, 0, 0x13, 0x0001FFFF) then (1, 0, 0x16, 0xA0) | Route and Level, as the verified runs sent them | K |
| C3 | (1, 0, 0x00, 0) | Mic, not Line | R |
| C4 | (1, 0, 0x01, 0) | PAD off | R |
| C5 | (1, 0, 0x08, 0) twice | Custom preamp off, so gain is not blocked | K |
| C6 | (1, 0, 0x0A, dB-10), (1, 0, 0x09, dB-10), (1, 0, 0x06, dB-9) | Gain | R |
| C7 | (1, 0, 0x05, 0x3F800000) | Phase normal | C |
| C8 | (1, 0, 0x03, 1 or 0) | 48V as the mic needs | R for a condenser |
| C9 | (2, 1, 0x04, 0) | Monitor source = MIX | L |
| C10 | (2, 1, 0x01, raw) with raw = 192 + 2*dB; take readback word 2 [7:0], or if invalid pick a default (e.g. 152 = -20 dB, our choice) | Monitor level | L |
| C11 | (2, 0, 0x03, 0) | Monitor unmute and not mono (bit 1 mute, bit 0 mono) | L |
| C12 | (2, 0, 0x44, 0) | Dim off | L |
| C13 | (2, 1, 0x20, 0) | OutputsAttenuate off (setting 7 bit 10) | L (defensive) |
| C14 | The rest of the daemon's 57-param monitor block | The daemon says the monitor module "never activates" without it (level changes ignored) | K |

Sources: readback rules `mixer-engine/hardware.py:5257-5269`. Route/Level
`mixer-engine/ua_mixer_daemon.py:3131-3147`. Preamp block
`mixer-engine/ua_mixer_daemon.py:3193-3224`. Gain `mixer-engine/hardware.py:3342-3376`. Monitor
params `driver/ua_core.c:2030-2096`, `mixer-engine/hardware.py:4238-4317`,
`mixer-engine/ua_mixer_daemon.py:3226-3230`, `mixer-engine/ua_mixer_daemon.py:3259-3318`. Level
encoding `mixer-engine/hardware.py:1012-1018`. The ALSA control caps the level at 192 (0 dB):
`driver/ua_core.c:3092`.

**Phase D: streaming.** [audio] runs the transport and maps channels. On the x8p's legacy layout,
capture channel N is physical input N+1: `devices/apollo-x8p.json:14`. Playback slots 0-1 are
MON L/R: `devices/apollo-x8p.json:77-79`.

### 6.2 Twin X: differences from the x8p

- **Types.** Gen 1: device type 0x99, public 0x19, mixer class "Lavern", kind 15
  (`devices/apollo-twin-x.json:3-19`, `driver/ua_apollo.h:127`, `driver/ua_core.c:1784-1785`).
  Gen 2: device types 0x3B, 0x3C, 0x40, 0x41, public 0x25/0x29, "LavernG2", kind 21
  (`devices/apollo-twin-x-gen-2.json:3-9`, `driver/ua_core.c:1792-1793`). Both are AudioExtension
  types: 0x99 is 64 or above, and 0x3B is above 38 (`driver/ua_apollo.h:141-157`).
- **Probe.** No ring connect at probe. Instead [audio] runs its boot transport start with clock
  source 0xC, then the rings are reprogrammed because of the DMA reset pulse:
  `driver/ua_core.c:4372-4391`. The ring connect (A7's commands) runs **right after the ACEFACE
  handshake**, inside the connect: `driver/ua_core.c:3695-3726`, `driver/ua_audio.c:3158-3163`.
  The Twin X needs this connect or playback is silent: `CHANGELOG.md:50-52`.
- **Strips.**
  - Gen 2 is in the strip table: synch 0/1 for ANALOG 1/2, 0x18.. for Virtual. Verified on a Duo
    at 48 kHz: "six strips live after a cold start, Mic 1 records and reaches the monitor mix"
    (`driver/ua_strips.c:776-788`).
  - Gen 1 (0x99) is **not** in the table. Same mixer-input layout (`devices/apollo-twin-x.json:115-137`),
    but unverified.
  - The strips go to DSP 1 by default. A Twin X Solo (one DSP) cannot host them there, because
    `ua_plugin_dsp_admitted` needs dsp < num_dsps (`driver/ua_plugin_place.c:216-224`). Use DSP 0
    and lose the monitor-mix feed (`HARDWARE-SESSIONS.md:327-328`).
- **Preamps.** Channels 0-1 only (max ARM channel 1). Hi-Z on input 1. Link only on channel 0:
  `mixer-engine/model_info.py:196-200`.
- **Monitor init.** 0x21 (DigOutMode) is not sent, because only the Freud classes get it:
  `mixer-engine/model_info.py:36-39`. 0x3E and 0x41 are allowed on kinds 15 and 21:
  `mixer-engine/model_info.py:58-59`.

### 6.3 Everything the daemon sends at startup, labelled

All from `mixer-engine/ua_mixer_daemon.py:3014-3359` unless noted.

| Lines | What | Label |
|---|---|---|
| 3034-3051 | Poll readback (retries 10 x 20 ms on a skipped snapshot); saved state overrides it | L |
| 3052-3061 | No readback and nothing saved: monitor raw 0 (-96 dB, effectively silent) and preamps 10 dB, all switches off | L: **replace the silent default** |
| 3066-3069, 3115-3116 | Preamp gain for every preamp via the triplet (`mixer-engine/hardware.py:3342-3376`) | R |
| 3070-3074 | Monitor MixInSource = "mon" (param 0x04 = 0, ch_idx 1); CRMonitorLevel = mon_raw (param 0x01) | L |
| 3076-3116 | Every input fader at 0 dB and **muted** unless saved: bus coefficient sub 0, L and R. The talkback input stays unmuted. | C for (a)/(b). Mute only affects the monitor mix. |
| 3131-3147 | Route 0x13 = 0x0001FFFF for the route channels (`mixer-engine/model_info.py:171-176`); S/PDIF L 0x0003FFFF; Level 0x16 = 0xA0 for the level channels (`mixer-engine/model_info.py:186-194`) | K |
| 3149-3156 | Bus enable (type 0) for buses 1, 2, 5, 6 = 1. The kext cache starts all-enabled (`driver/ua_dsp.c:4073-4079`) | C |
| 3163-3171 | For ch_idx 0, 1, 7: 0x47 = 1, 0x48 = 0, 0x46 = 0 (talkback off), 0x44 = dim | 0x44 is L; the rest C |
| 3193-3224 | Per preamp: Route, then Level; LowCut, MicLine, 0x08 x2, GainC, Phase, 0x07, PAD, 48V, Link (even channels), Route x2; trailing Level. The daemon thinks 0x16 = 0xA0 may be the "ARM-relay arm signal" (3184-3192). The driver says the relays follow the mailbox (`driver/ua_dsp.c:3489-3494`). | MicLine/PAD/GainC/48V are R; 0x08/Route/Level are K; the rest C |
| 3238-3258 | Gating: skip talkback params on models without talkback, HP2 params without HP2, 0x1E without mirror-to-digital, 0x21 except Freud, and params refused for the mixer kind | (required logic) |
| 3259-3318 | Monitor block in macOS order, 57 params. Notable: 0x6a = 5 (three times), 0x04 = 0, 0x01 = level, 0x03 = mute/mono, 0x44 = dim, 0x20 = 0, 0x32 = 0 (+4 dBu), 0x3e = 1, 0x43 = 7, 0x1f = 1 | 0x01, 0x03, 0x04, 0x44, 0x20 are L; 0x6a and 0x3e are K; the rest K/C, send the whole block once |
| 3322-3340 | CUE 3/4 sends (sub 5/6, both sides) = 0.0 for every send bus | C |
| 3342-3347 | Talkback input fader and sends | C |
| 3349-3359 | Re-apply the saved state (sends, aux, CUE, outputs) | C |

The daemon never sends a rate ack. The driver does it (B2). The daemon also re-runs this whole push
on every device return: `mixer-engine/ua_mixer_daemon.py:1398-1427`.

### 6.4 Building the Mic Input Strip for preamp 1 (exact commands)

These are the x4/Twin X Gen 2 recipe at 48 kHz on the strip DSP (default DSP 1), for the first
strip of a pass (`input_strips_mask=0x1`, `input_strips_virtual=0`). The steps are those listed at
`driver/ua_strips.c:35-55`, implemented by `ua_strips_run` (`driver/ua_strips.c:1618-1786`).

**Preconditions** (`driver/ua_strips.c:1388-1474`):

- The DSPs are ring-connected.
- The strip DSP's boot word & ~0xFF00 == 3 at the cold start (`driver/ua_plugin_place.c:95-136`,
  `driver/ua_plugin_host.h:274-281`).
- Its pools match the recipe: code pool base 0xe0000; pool 1-3 tops 0x9d1b8, 0xc6ffe and
  0x08ffe000, with room (`driver/ua_strips.c:357-361`, `driver/ua_strips.c:426-483`).
- The rate is acked and reported (1.6).

**Steps**

1. Upload five code modules, one transaction each, each waited for (1.6).
2. One transaction for the strip, waited for by ring consumption (`driver/ua_strips.c:1689-1729`).
   It contains the following.
   - **Private addresses, slot 0, top-down (derived** from `driver/ua_strips.c:372-383` and
     `driver/ua_strips.c:498-520`; the first header matches `HARDWARE-SESSIONS.md:209-210`):

     | Row | Address |
     |---|---|
     | header | 0x9d1b0 |
     | m0 | 0x9d178 |
     | m1 | 0xc6fee |
     | m2 | 0x9d168 |
     | m3 | 0x08ffdff0 |
     | m4 | 0x9d164 |
     | c0 | 0x08ffd7f0 |
     | c1 | 0x08ffcff0 |
     | c2 | 0x08ffc7f0 |
     | c3 | 0x08ffbff0 |

   - **10 zero-fills**, inline `{0x00080004, addr, 0, dwords}` with dwords
     8, 0x38, 0x10, 0x10, 0x10, 4, 0x800, 0x800, 0x800, 0x800 (`driver/ua_strips.c:618-622`).
   - **Memspec**, DMA, 19 words (`driver/ua_plugin_upload.c:97-160`):
     `0x00150013`, (0x9d1b6, 0x9d178), (0x9d1ae, 0xc6fee), (0x9d1af, 0x9d168),
     (0x9d193, 0x08ffdff0), (0x9d194, 0x9d164), (0x9d186, 0x08ffd7f0), (0x9d187, 0x08ffcff0),
     (0x9d17e, 0x08ffc7f0), (0x9d17f, 0x08ffbff0) (derived).
   - **Header**, DMA: `{0x00140009, 0x9d1b0, 0x24, 0x40000000, 1, 1, 0x22, 1, 0x846}`
     (`driver/ua_strips.c:554-563`).
   - **Parameters**, DMA, `input_strips_params=1`, the captured layout at 48 kHz
     (`driver/ua_strips.c:639-672`). The words are at `driver/ua_strips.c:538-552`:
     - `{0x00140014, m0, m0a[18]}`
     - `{0x00140012, m0+38, m0b[16]}`
     - `{0x00140011, m3+1, m3w[15]}`

     The older A1 layout "started the strip and hung the x4": `HARDWARE-SESSIONS.md:324-326`.
   - **Start:** `{0x001F0004, 0x00200000 | 0, 0x9d1b0, 0}`, then `{0x001E0004, 0, 8, 0x20}` twice,
     then `{0x001E0004, 0, 7, 0x1ffff}` (`driver/ua_strips.c:583-601`).
3. Make the strip DSP the owner of bus 0 and re-send bus 0's cached coefficients to it. Later
   coefficients go to the owner and DSP 0 (`driver/ua_strips.c:1731-1739`,
   `driver/ua_dsp.c:3860-3903`).
4. Send `{0x001E0004, 0, 7, 0x1ffff}` to every other DSP (`driver/ua_strips.c:1740-1753`).
5. Run the 48V resync (5.4).

**Teardown** before every disconnect, reconnect, rate change, suspend and remove
(`driver/ua_strips.c:1788-1800`):

- `{0x001F0004, 0x00200000 | synch, 0, 0}` for each strip.
- `{0x00030002, id, 0, 0}` for each module.

**What this needs, beyond ua_strips.c**

- The transaction engine: `driver/ua_dsp_xact.c`.
- Upload and memspec: `driver/ua_plugin_upload.c`.
- The module catalog: `driver/ua_plugin_catalog.c`.
- DSP ownership and geometry: `driver/ua_plugin_place.c`.
- The pool ledger: `driver/ua_resource_pool.c`.
- The host work and epoch: `driver/ua_plugin_host.c`.

**Plug-in-only pieces, not needed:**

- `driver/ua_plugin_instance.c` and `driver/ua_plugin_license.c`.
- The board query and authorization.
- The captured chain, `driver/ua_plugin_chain.h`.

The strip planner, `UA_STRIPS_PLANNER_BEGIN..END`, has no kernel calls and is portable as is:
`driver/ua_strips.c:174`, `driver/ua_strips.c:754`.

---

## 7. Meters and events (optional)

- **Event channel.** `read()` on the chardev returns 16-byte `{u32 code, u32 arg, u64 time_ns}`
  records, with a per-open queue that drops the oldest event when full: `driver/ua_event.c:1-69`,
  `driver/ua_ioctl.h:957-978`.
  - Codes: notification status, IO descriptors, rate, connected, offline, events lost, clock lock,
    pause/resume, device error.
  - The decoded notification events: `driver/ua_event.c:93-112`.
  - Not needed for (a)/(b).
- **Hardware meters.** The DSP writes level meters into readback words 8-25: 16-bit input meters
  (even channel in bits 31:16), 12-bit monitor meters in word 18, AUX in words 19/20. Lane mapping
  by synch-in: `mixer-engine/metering.py:520-586`. The meters stay at zero without strips:
  `driver/ua_strips.c:11-13`.
- **Daemon meters.** By default the daemon computes meters from the captured PCM (PipeWire or
  ALSA), not from the readback: `mixer-engine/ua_mixer_daemon.py:1495-1497`.

---

## 8. Porting notes

**Runs on Windows mostly unchanged (pure Python):**

- `state_tree.py`, the protocol and codecs, `model_info.py`.
- `HardwareRouter` and `_push_defaults_to_hardware` (`mixer-engine/hardware.py:2579-5355`,
  `mixer-engine/ua_mixer_daemon.py:3014-3359`).
- `devices/*.json`, the device maps.
- All driver traffic is little-endian `struct.pack('<IIII')` buffers
  (`mixer-engine/hardware.py:1924-1928`, `mixer-engine/hardware.py:2008-2010`). Swapping
  `fcntl.ioctl` for `DeviceIoControl` with our own `CTL_CODE`s keeps every payload as it is.
  Appendix A lists the codes.

**Needs a Windows replacement:**

| Linux mechanism | Where | Windows replacement |
|---|---|---|
| Device discovery `glob('/dev/ua_apollo*')` | `mixer-engine/hardware.py:1529-1532` | Device interface |
| `probe()` via fstat and inode | `mixer-engine/hardware.py:1458-1481` | Handle and device-interface check |
| `os.read` event reader | `mixer-engine/events.py:136` | ReadFile, overlapped |
| `/sys/module/.../toslink_in_bit` | `mixer-engine/hardware.py:694`, `mixer-engine/hardware.py:1443-1454` | Driver param or registry |

**Linux-only, drop or replace:**

- PipeWire/ALSA: `mixer-engine/pipewire_state.py`; `AlsaMeter` and `PipeWireMeter` in
  `mixer-engine/metering.py:81`, `mixer-engine/metering.py:403`; `configs/pipewire`,
  `configs/wireplumber`, `configs/ucm2`.
- systemd and udev: the unit path at `mixer-engine/ua_mixer_daemon.py:1144`, the exit code for
  systemd restart at `mixer-engine/ua_mixer_daemon.py:294`, `configs/udev`, `configs/systemd`.
- Install scripts.

**Kernel side mapping**

| Linux | Windows |
|---|---|
| `dma_alloc_coherent` | WDF common buffers, 4 KiB aligned, 64-bit addresses |
| `delayed_work` 10 ms service loop and strip work | WDF timer plus PASSIVE work item (they take a mutex and sleep) |
| `request_firmware` | Service-read files plus compiled-in pins (1.8) |
| Module parameters | Registry values |

**Smallest viable host side.** A small service that sends the R, L and K items of 6.1 Phase C
through DeviceIoControl, without the Mixer Engine TCP protocol. Port the full daemon only if UA
Console compatibility is wanted.

---

## 9. Open questions and risks

1. **x8p record path after a true cold start (biggest unknown).** The source cannot tell whether
   x8p preamps record without strips (section 0).
   - First hardware test: power-cycle the x8p with only our driver bound, record input 1.
   - If it is silent, add `{0x22, 0, 0x18}` to the strip table and check that the x8p's pool
     geometry passes `ua_strip_layout_check`: `driver/ua_strips.c:440-483`.
   - The recipe was validated only on the x4 and Twin X Gen 2 values.
2. **UA's own Windows driver.** Strips need the strip DSP's boot word to be exactly 3, meaning no
   host since power-up (`driver/ua_plugin_place.c:13-21`). Every attempt needs its own power cycle
   (`HARDWARE-SESSIONS.md:172-175`).
   - With UAD installed natively, UA's kernel driver can bind first. Its files are `uad*.sys` in
     `System32\drivers` and the DriverStore: `console-rs/crates/oa-assets/src/detect.rs:174-184`.
   - So the port must keep UA's driver off the Apollo and require a power cycle after any UA
     session (inference).
   - Pre-OS state is a risk in general: the Linux driver neutralizes stale rings for exactly this
     reason (`driver/ua_dsp.c:1116-1128`).
3. **Port clash.** The daemon impersonates UA's Mixer Engine on TCP 4710
   (`mixer-engine/ua_mixer_daemon.py:1317`). UA's own `UAMixerEngine.exe` ships in the Windows
   install (`console-rs/crates/oa-assets/src/detect.rs:96-97`), so a running UA engine would clash
   with a ported daemon (inference).
4. **Version pins.**
   - Strip modules are pinned only for UAD 12.0.0.602
     (`console-rs/crates/oa-assets/data/firmware-pins.json:10-46`).
   - The driver checks exact lengths and w4 (`driver/ua_strips.c:221-274`).
   - The pool geometry assumes the unit firmware the captures came from (x4 and Twin X share
     release 0x0b0fc9b3: `driver/ua_apollo.h:247-249`).
   - A newer UAD or Apollo firmware on the user's PC may need new pins and a re-check.
5. **Rates other than 48 kHz.** The captured strip parameters are 48 kHz only. Other rates use
   computed ramps and SDK filter bands "not yet verified on hardware": `driver/ua_strips.c:639-646`.
6. **Quarantine.** Any strip or upload transaction that ends unproven takes the DSP out of use
   until the next power cycle: `driver/ua_dsp_xact.c:519-549`, `driver/ua_plugin_upload.c:181-225`.
7. **Ordering against the rate.** The strip pass must wait for the rate ack and the firmware's
   reported rate, or SYNCH stalls: `HARDWARE-SESSIONS.md:322-323`, `driver/ua_audio.c:5168-5180`.
   [audio] owns the rate and clock writes this depends on.
8. **48V resync covers preamps 0-3 only** (`driver/ua_core.c:2482-2495`). The x8p's preamps 5-8
   need it extended (6.1 B6).
9. **Unexplained writes kept for parity.**
   - The connect-time 0x20/0xFF batch, which also sets preamp 1's gain to about 41-42 dB until the
     host writes it (4.1).
   - Route 0x13 = 0x1FFFF. The daemon calls it "enables routing to buses 0-16", but in kext terms
     0x1FFFF means "not routed from this DSP": `mixer-engine/ua_mixer_daemon.py:3131-3133`,
     `driver/ua_apollo.h:636-640`. Windows sends 0x0FFFF where DSP 0 owns the strip
     (`driver/ua_strips.c:124-127`).
   - Level 0x16 = 0xA0, which the driver sends as span info.
   - The full 57-param monitor block.
10. **Order of connect versus ACEFACE on the x8p.** The x8p gets its ring connect before ACEFACE.
    Kext 12 does ACEFACE first (`driver/ua_core.c:3728-3745`, `v12_connect` off). The Linux order is
    the one owners verified.
11. **Silent monitor default.** Without a valid readback and saved state, the daemon writes monitor
    level 0, which is -96 dB (`mixer-engine/ua_mixer_daemon.py:3018-3021`,
    `mixer-engine/ua_mixer_daemon.py:3052`). A Windows host must use the readback or a chosen
    default.
12. **Inherited DSP behaviour.** Only the x4 keeps another session's DSP state (`dsp_inherited`).
    Twin X and the other models always ring-connect and lose any inherited strips:
    `driver/ua_core.c:4247-4255`, `CHANGELOG.md:50-52`.
13. **Driver feature bits.** The daemon drops right-side bus entries unless the driver reports
    `UA_FEAT_BUS_PARAM_SIDE` (bit 2) in GET_VERSION. That corrupts pan and stereo sends:
    `mixer-engine/hardware.py:1972-1998`, `driver/ua_ioctl.h:551-569`.
14. **Stale or contradictory comments in the source.**
    - `ARCHITECTURE.md:98` and `driver/ua_ioctl.h:178-189` still say the preamps use the ARM CLI.
    - `input_strips_dsp` is described as "default 0" but is 1 (`driver/ua_strips.c:129-132`).
    - The strip header comment's "0x410 dwords" per slot in pool 3 predates the 0x800 delay buffers
      (`driver/ua_strips.c:43-45` vs `driver/ua_strips.c:368-383`).
    - `_sendBlock` prefers 1 KiB DMA pieces, while the transactions found 1 KiB pieces dropped the
      x4 (2.4).

---

## Appendix A: driver ABI the daemon uses

From `driver/ua_ioctl.h`. Magic is 'U'. The Linux numbers are `_IOC(dir, 'U', nr, size)`. A Windows
port defines its own CTL_CODEs with the same payloads.

| ioctl | nr | Payload | Use here | Cite |
|---|---|---|---|---|
| GET_DEVICE_INFO | 0x01 | `ua_device_info`: fpga_rev, device_type, subsystem_id, num_dsps, fw_version, fw_v2, serial, ext_caps, public_type, 7 reserved | Model, kind | `driver/ua_ioctl.h:20-31`, `driver/ua_ioctl.h:297` |
| GET_VERSION | 0x04 | `ua_version` (features bits) | Feature gating | `driver/ua_ioctl.h:551-569`, `driver/ua_ioctl.h:605` |
| SET_MIXER_BUS_PARAM | 0x30 | `{bus_id, sub_param 0-6, value float bits, flags}`; flags 0x02, plus 0x100 for the right side, plus 0x200 for solo-mute | Bus coefficients | `driver/ua_ioctl.h:132-148`, `driver/ua_ioctl.h:305`, `mixer-engine/hardware.py:117` |
| SET_MIXER_PARAM | 0x31 | `{channel_type 0 bus-enable / 1 input / 2 monitor / 3 rate-ack, channel_idx, param_id, value}` | Preamp, monitor | `driver/ua_ioctl.h:196-201`, `driver/ua_ioctl.h:306` |
| WRITE_MIXER_SETTING / READ_MIXER_SETTING | 0x36 / 0x37 | `{index 0-37, value, mask, reserved}` | Raw mailbox (debug) | `driver/ua_ioctl.h:289-294`, `driver/ua_ioctl.h:316-317` |
| GET_HW_READBACK | 0x41 | `{status, data[40]}`, the driver's cached copy | Monitor level, preamp state | `driver/ua_ioctl.h:327-332`, `driver/ua_core.c:3460-3480` |
| GET_MIXER_READBACK | 0x33 | 38 setting words plus an XOR check | Debug | `driver/ua_ioctl.h:247-258` |
| read() | - | `ua_event` records | Optional events | `driver/ua_ioctl.h:957-978` |

The monitor-param channel_idx is ignored by the driver; the daemon sends the capture's values:
`mixer-engine/ua_mixer_daemon.py:3161`.

Derived Linux numbers, for matching traces: SET_MIXER_PARAM 0x40105531, SET_MIXER_BUS_PARAM
0x40105530, WRITE_MIXER_SETTING 0x40105536, GET_HW_READBACK 0x80A45541.
