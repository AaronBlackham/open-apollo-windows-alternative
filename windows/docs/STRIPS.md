# Input strips on the x8p: port plan for apollo.sys

Goal: after a cold start, the x8p's mic preamps 1-8 reach record slots 0-7. Today they are exact
digital zero, because the DSP passes analog inputs to the mix, the meters and the host only
through the Console's Mic Input Strips, which the host must build (`oa:driver/ua_strips.c:7-13`).

Conventions

- `oa:` paths are relative to `apollo-win/reference/open-apollo/` (GPL-2.0, read only). Paths
  without `oa:` are ours. Our line numbers are as of Nov 14 2026; for `hw.c`, `ioctl.c` and
  `apollo.h` (edited today) the function name is the reference.
- **(derived)** = computed from the source's tables; **(inference)** = not stated by the source.
- Facts from the real x8p are from `tools/phase1/install.log` and the Nov 14 cold-start test.

**Status (Nov 14 2026).** The T2 dry run and **T3 (uploads) passed** on the real x8p: five replies `80070004 00000000 <id> <word0>`, about 30 ms in all. The strip stages for T4 are implemented (section 10.5) and build clean but have not run on hardware. The T3 build ("uploads only") is
implemented and builds clean but has not run on hardware yet. Section 10 lists what is
implemented, where it differs from this plan, and the exact T3 procedure.

---

## 0. Decisions

| # | Decision |
|---|---|
| D1 | Port the strip path only: transaction engine, upload, memspec, release, boot-word ownership, geometry check, strip planner, teardown/fence, bus-owner table, route defaults, 48V resync. No plug-in hosting, licensing, Virtual Input Strips, other rates. |
| D2 | Model entry `{0x22, first_synch 0, first_virtual 0x18}`. Mic strip for input i (0-based) goes on DSP input (synch) i. |
| D3 | Strips on **DSP 1**, the verified default (`oa:driver/ua_strips.c:129`). |
| D4 | Ownership is decided by DSP 1's boot word (window +0x1A4 = BAR0 0x09A4) read **before our ring connect**, never by the SEQ counters. |
| D5 | Read DSP 1's 11 pool registers and refuse, with nothing sent, unless they give code base 0xE0000 and pool tops 0x9D1B8 / 0xC6FFE / 0x08FFE000 with room. |
| D6 | No general pool ledger: a fixed layout where input i always uses slot i. For masks 0x01 and 0xFF this gives exactly open-apollo's addresses. |
| D7 | Module bytes: `apollo-diag strips install` (elevated) extracts the five Bill records from the user's UAD DLLs into an admin-only directory. The driver reads only those five files, and re-checks length, header, w4 and SHA-256 against pins compiled into the driver. No IOCTL carries module bytes. |
| D8 | Build when all of these hold: cold bring-up, DSPs 48 kHz rate-acked, RATE_INFO locked at index 2, and 500 ms settle. Transport state does not matter. On this unit the Nov 14 connect acked the firmware's retained 44.1 kHz (install.log: `rate info 0x00000101`, `rate ack 44100`), so expect the gate to open at the first `TRANSPORT_START`, whose clock write and rate ack make it 48 kHz. |
| D9 | Tear down on every D0 exit (published, then proven before the ring quiesce). v1 never rebuilds after a warm bring-up; the log asks for a power cycle. **Superseded Nov 15 2026 by section 12:** on the x8p a D0 exit leaves Live strips running and the next warm start adopts them when this unit's record proves them the driver's own; the Twin X rebuilds them warm. |
| D10 | The mixer-init batch on the x8p becomes an "x8p-safe" batch that skips the preamp settings, then gets A/B tested against no batch at all (section 6). |
| D11 | Locks: new `StripLock` (taken before DevLock); never wait for the DSP while holding DevLock. |

---

## 1. What to port

### 1.1 Needed

| Piece | open-apollo | What it does | Our form |
|---|---|---|---|
| Transaction engine | `oa:driver/ua_dsp_xact.c:58-552`, constants `oa:driver/ua_plugin_host.h:126-178` | A batch of ring entries, its DMA payload and its reply descriptors, published atomically (replies first), waited for with the lock released. A transaction whose outcome is unproven quarantines the DSP. | New `strips.c` on top of `dsp.c` (section 3). Buffers come from a preallocated pool, not per-call allocation. |
| Ring primitives | `oa:driver/ua_dsp.c:130-151` (free/pending/update_pos), `:661-666` (resync_now), `:723-767` (put/doorbell), generation `:465-503` | Room checks, a no-wait slot write, abandon via resync | Add to `dsp.c` (3.1). |
| Upload | `oa:driver/ua_plugin_upload.c:24-65`, reply check `:81-95`, run `:185-238` | One DMA command per module plus one 4-dword reply, accepted only as `{0x80070004, 0, id, x}` | Keep. "SDK" form only (`input_strips_form=1`). |
| Memspec | `oa:driver/ua_plugin_upload.c:114-160` | Link table: stores each private block's address into a word of its parent block | Keep the function, or use the precomputed words in 2.6. |
| Release | `oa:driver/ua_plugin_upload.c:163-170` | `{0x00030002, id, 0, 0}` per code module | Keep (code kind only). |
| Module checks | `oa:driver/ua_plugin_catalog.c:42-74` (Bill check), `oa:driver/ua_strips.c:256-274` (strip check) | Magic, id, w2, payload, w4, code-pool words | Fold into the pinned loader (4.3). |
| Ownership | `oa:driver/ua_plugin_place.c:95-136` | DSP usable only when the boot word & ~0xFF00 == 3 at a cold start | Strip DSP only, but log all six boot words (2.3). |
| Geometry | `oa:driver/ua_plugin_place.c:170-213`, `oa:driver/ua_strips.c:426-483` | Read size/base/scratch; check against the recipe | Keep the check verbatim (2.4). |
| Strip planner | `oa:driver/ua_strips.c:174-754` ("no kernel calls") | Rows, addresses, builders, 48 kHz parameter words | Keep the Mic-strip parts (2.5-2.7). |
| Pass / teardown / lost | `oa:driver/ua_strips.c:1388-1474` (begin), `:1581-1786` (upload + run), `:1801-1866` (teardown), `:1874-1888` (lost) | Gates, the uploads, one transaction per strip, a fence-proven teardown | Port to our context (section 5). |
| Host glue | `oa:driver/ua_plugin_host.c:101-118` (publish_now), `:126-168` (fence), `:199-224` (quiesce), `:236-257` (device lost), `:295-331` (epoch, quarantine) | | In `strips.c`. |
| Bus owner + resend | `oa:driver/ua_dsp.c:3852-3903`, send rule `:3790-3829` | The strip's DSP mixes that input. Its coefficients go to the owner, and also to DSP 0 | Owner table plus the hook (2.8). Nothing is cached in v1, so nothing is sent. |
| Route defaults | `oa:driver/ua_strips.c:1740-1753` | `{0x001E0004, synch, 7, 0x1FFFF}` to every DSP that does not host the strip | Keep. |
| Rate gate | `oa:driver/ua_audio.c:5168-5180` | Connected, DSPs acked at the host rate, RATE_INFO locked at its index | Keep (5.2). |
| 48V resync | `oa:driver/ua_core.c:2458-2499`, call `oa:driver/ua_strips.c:1772-1783` | After the first pass of a cold start: 48V 0 then 1 on preamps the unit reports with 48V on | Extend to preamps 5-8 (5.6). |

### 1.2 Dropped for v1

| Dropped | Where | Why |
|---|---|---|
| Plug-in instances, swap, Unison, delay IOCTL | `oa:driver/ua_plugin_instance.c`, `oa:driver/ua_strips.c:1059-1293` | Plug-in hosting is out of scope |
| Licensing / board query | `oa:driver/ua_plugin_license.c`, `oa:driver/ua_plugin_host.h:357-387` | Only needed for plug-ins. Strip modules need no authorization (`oa:driver/ua_strips.c:27-33`) |
| SHA-named plug-in store, catalog refcounts | `oa:driver/ua_plugin_catalog.c:76-219`, `:257-299` | Five fixed files instead |
| General pool ledger | `oa:driver/ua_resource_pool.c` | With strips as the only tenant, it returns the recipe's addresses or the pass refuses (`oa:driver/ua_strips.c:1462-1468`, `:1550-1554`). A fixed table plus the layout check is equivalent. |
| Virtual Input Strips (0x83E, modules A5/C2) | `oa:driver/ua_strips.c:394-412`, `:713-745`, `:1000-1009` | v1 is mic preamps only. Virtual 1-8 would follow at synch 0x18-0x1F. |
| Rates other than 48 kHz, A1 recipe | `oa:driver/ua_strips.c:285-341`, `:675-702` | 48 kHz is the only rate with captured words. The A1 layout "started the strip and hung the x4" (`oa:HARDWARE-SESSIONS.md:324-326`). |
| Arch-byte upload form (`UA_BILL_STRIP`) | `oa:driver/ua_plugin_upload.c:57-60` | Refused: `80070004 f0010005 020000eb`, DSP quarantined (`oa:HARDWARE-SESSIONS.md:320-321`) |
| Reply realignment probe | `oa:driver/ua_dsp_xact.c:642-696` | v1 quarantines on any unexpected reply instead |
| Captured plugin chain, firmware stage | spec 1.7 | Off upstream; the chain faulted the IOMMU |

### 1.3 Upstream defects not to copy

- `oa:driver/ua_strips.c:1603`, `:1609` and `:1838` read and clear `mod->res[0]`, but the upload records
  `res[dsp]` (`oa:driver/ua_plugin_upload.c:226-231`). With the strips on DSP 1, a module whose
  upload was published but not proven ends up `NONE` and is never released. Our port keeps its own
  per-module state: `None`, then `Uploading` once published, then `Resident` once the reply is accepted.
  The teardown releases every module that is not `None`.
- Stale parameter text: `input_strips_dsp` says "default 0" but is 1 (`oa:driver/ua_strips.c:129-132`).
  `input_strips_form` and `input_strips_params` describe 0 as the default, but both are 1
  (`:108-111`, `:141-144`). The verified configuration is DSP 1, SDK form, captured parameters.

---

## 2. x8p specifics

### 2.1 Model entry (verified)

`{ 0x22 /* UA_DEV_APOLLO_X8P */, 0 /* first_synch */, 0x18 /* first_virtual */ }`, added to
`oa:driver/ua_strips.c:770-789`'s table. Source: `oa:devices/apollo-x8p.json:194-201` (ANALOG 1-8 =
synch_in 0-7, preamp true) and `:212-219` (VIRTUAL 1-8 = synch_in 24-31). This is the x4 layout
(`oa:devices/apollo-x4.json:177-198`), extended to 8 preamps. The preamp count is 8
(`oa:devices/apollo-x8p.json:17`). The x8p record frame puts mic/line 1-8 in slots 0-7, so
strip i feeds slot i.

DSP inputs 4-7 have never carried a strip on any unit. The x4 has 4 preamps, and the
`input_strips_inputs=8` aid (`oa:driver/ua_strips.c:119-122`) has no recorded run. Hence the test
order in section 8: input 1 alone first, then all eight.

### 2.2 Strip DSP

DSP 1, as in `oa:driver/ua_strips.c:129`; open-apollo clamps it below 4 (`:966`). On DSP 0 the strips
feed the record channels but not the monitor mix; on DSP 1 they feed both
(`oa:HARDWARE-SESSIONS.md:327-328`). DSP 1 on the x8p:

| Item | BAR0 |
|---|---|
| Register window | 0x0800 (`ApolloDspWindow(1)`) |
| Boot word | 0x09A4 |
| Command ring bank | 0x2080 (`Cmd[1]`), response 0x20C0 (`Resp[1]`) |
| DMA_CTRL enable | bit 2; already set (install.log `DMA_CTRL ... 0x0000007f`) |

Registry `StripDsp` may select 0-3 (the clamp). Use it only to bisect a monitor-mix question.

### 2.3 Ownership: the boot-word rule

Rule (`oa:driver/ua_plugin_place.c:110-118`, `oa:driver/ua_plugin_host.h:274-281`): the strip DSP is
ours only if, at a cold bring-up in this device instance, `(boot & ~0x0000FF00) == 0x00000003`
(3 = no host since power-up; bits 15:8 are a model byte).

- **Read point.** Window +0x1A4 for every DSP (BAR0 0x01A4, 0x09A4, 0x11A4, 0x19A4, 0x41A4,
  0x49A4). Read it after E3 has seen it ready and before E11's `ApolloDspConnectAll`
  (`device.c:271` .. `:304`). The value that ends `ApolloWaitDspsReady`'s poll is the one to
  keep. Our connect turns it into 0x0B + a connect count (`oa:driver/ua_plugin_place.c:14-16`).
- **SEQ counters are not this.** `ApolloMixerDspAlive` (SEQ_WR == SEQ_RD > 0) decides warm or
  cold for the mailbox, but the Nov 14 install shows SEQ 0/0 ("cold start") with
  `previous owner left 0x01A4=0x0010220b`: UA's driver had connected DSP 0, so it is not owned.
  The model byte there is 0x22, so after a real power-up expect about 0x00002203 (inference).
  **Measured Nov 14 after a power cycle:** `00002203 00000003 00000003 00000003 00000003 00000003`.
  DSP 0 carries the model byte; DSPs 1-5 read plain 3, so DSP 1 is owned.
- **Window +0x000 is not the boot word.** The logged "status" words (DSP0 0x03001C00,
  DSP1 0x10000000 ... DSP5 0x50000000) come from `ApolloCheckDsps` reading window +0x000.
- `Owned = cold && (BootWord[StripDsp] & ~0xFF00) == 3 && !Tainted`. A warm bring-up never owns
  in v1 (5.5). Log all six words on every bring-up:
  `strips: boot words 0x%08x ... (DSP %u owned|not owned)`.

### 2.4 Pool geometry: read, check, refuse

Read on the strip DSP, under DevLock, at the start of each pass. Reads only.

| Register | Window offset | BAR0 (DSP 1) | Expected (x4 DSP 0; Twin X Gen 2 both DSPs) |
|---|---|---|---|
| size p0..p3 | +0x010 + 4p | 0x0810, 0x0814, 0x0818, 0x081C | 0x00004000, 0x0000A9B8, 0x00001FFE, 0x00FFE000 |
| base p0..p3 | +0x184 + 4p | 0x0984, 0x0988, 0x098C, 0x0990 | 0x000E0000, 0x00092800, 0x000C5000, 0x08000000 |
| scratch p1..p3 | +0x190 + 4p | 0x0994, 0x0998, 0x099C | 0x00000080, 0x00000080, 0x00008000 |

Sources: `oa:driver/ua_plugin_host.h:282-284`, the x4 values `oa:HARDWARE-SESSIONS.md:124-127`, and the
Twin X Gen 2 note at `oa:driver/ua_strips.c:777-784`. The x8p's firmware (11.15.54803, release
0x0B0FD613) differs from the x4/Twin X captures (0x0B0FC9B3, spec 9.4).

**Measured on the x8p, DSP 1 (Nov 14 2026 dry run, after a power cycle):**

- size `00004000 0000a9b9 00001fff 00ff516a`
- base `000e0000 00092800 000c5000 08008e96`
- scratch `00000080 00000080 00008000`

The sizes differ from the x4 and pool 3 starts at 0x08008E96, not 0x08000000. After rounding
(base up to even, size down to even), the four starts and tops are still the recipe's: code
0xE0000; tops 0x9D1B8, 0xC6FFE, 0x08FFE000. The check returns code 0 for 8 strips, and the
2.6 addresses hold unchanged.

Check (port of `oa:driver/ua_strips.c:440-483`; `hi` = the highest input index in the mask):

```c
static const ULONG kTop[4]     = { 0, 0x0009D1B8, 0x000C6FFE, 0x08FFE000 };
static const ULONG kScratch[4] = { 0, 0x1C0, 0x80, 0 };            /* AllocInfo 0xA9C/0xA98/0xAA0 */
static const ULONG kPerStrip[4] = { 0, 0x54, 0x10, 0x2010 };        /* rows of 2.6, per pool */
#define STRIP_CODE_BASE   0x000E0000
#define STRIP_CODE_WORDS  994                                       /* 0x6C+0x1F0+0x130+0x16+0x40 */

/* 0 = fits the recipe; else a reason code. Size/Base/Scr are the raw registers. */
ULONG StripLayoutCheck(const ULONG Size[4], const ULONG Base[4], const ULONG Scr[4], ULONG Hi)
{
    for (ULONG p = 0; p < 4; p++) {
        if (Size[p] == ~0u || Base[p] == ~0u || (p && Scr[p] == ~0u)) return 1;    /* gone */
        ULONG start = Base[p] + (Base[p] & 1);
        ULONG top   = start + (Size[p] & ~1u);
        ULONG sc    = p ? max(Scr[p] + (Scr[p] & 1), kScratch[p]) : 0;
        ULONG floor = start + sc;
        if (top < start || floor < start) return 4 + p;
        if (p == 0) {
            if (start != STRIP_CODE_BASE) return 2;
            if (top - start < STRIP_CODE_WORDS) return 3;
        } else {
            if (top != kTop[p]) return 4 + p;                              /* not the recipe's top */
            if (floor > top || top - floor < kPerStrip[p] * (Hi + 1)) return 8 + p;  /* no room */
        }
    }
    return 0;
}
```

On failure: state Off, `Why` = "pool N does not end where the recipe expects" (or similar), and
the 11 registers in the log. **No command reaches the DSP.** With the x4 values, the headroom
for 8 strips is large: pool 1 floor 0x929C0 vs lowest block 0x9CF18; pool 2 floor 0xC5080 vs
0xC6F7E; pool 3 floor 0x08008000 vs 0x08FEDF80 (derived).

If the x8p reports other tops: every address in 2.6 is "top minus a constant", so a relocated
layout is a one-line change. But it was never run, so it stays behind an explicit registry
opt-in (`StripAllowRelocated`, default 0) and gets its own power-cycled test.

### 2.5 Code modules: destinations and upload words

Bottom-up from 0xE0000 in Mic-strip order (`oa:driver/ua_strips.c:209-229`, `:485-496`):

| id | len | w4 | pool dwords | dest | DMA word 0 = 0x00010000 \| (len/4 + 2) |
|---|---|---|---|---|---|
| 0x0EB | 716 | 162 | 0x06C | 0xE0000 | 0x000100B5 |
| 0x0DA | 3044 | 744 | 0x1F0 | 0xE006C | 0x000102FB |
| 0x0A5 | 1940 | 456 | 0x130 | 0xE025C | 0x000101E7 |
| 0x0C2 | 184 | 33 | 0x016 | 0xE038C | 0x00010030 |
| 0x12B | 452 | 96 | 0x040 | 0xE03A2 | 0x00010073 |

The code pool ends at 0xE03E2 (994 dwords).

Upload payload (DMA, one command): `{word0, dest, record}`. The record is copied whole, then bytes
10 and 11 (the high half of word 2) are cleared. That turns w2 0x02000000 into 0; the id word stays
as on disk (`oa:driver/ua_plugin_upload.c:55-63`). For 0x0EB the first words are
`000100B5 000E0000 6C6C6942 000000EB 00000000 000002B8 000000A2 ...`. The largest upload (0x0DA) is
3052 bytes, so every upload is one DMA_REF inside one 4 KiB page.

Accepted reply: `r[0] == 0x80070004 && r[1] == 0 && r[2] == id`. r[3] is not checked. In the SDK
form the id goes out without the architecture byte, and open-apollo then accepts only the plain id
(`oa:driver/ua_plugin_upload.c:84-93`).

### 2.6 Eight strips: private addresses, slot = input

Rows per strip, in descriptor order (`oa:driver/ua_strips.c:372-384`): descriptors {0: head, 1: Mic}.

| Row | Pool | Dwords | Link (row, word) |
|---|---|---|---|
| hdr | 1 | 0x008 | none (chain head) |
| m0 | 1 | 0x038 | hdr[6] |
| m1 | 2 | 0x010 | m0[0x36] |
| m2 | 1 | 0x010 | m0[0x37] |
| m3 | 3 | 0x010 | m0[0x1B] |
| m4 | 1 | 0x004 | m0[0x1C] |
| c0 | 3 | 0x800 | m0[0x0E] |
| c1 | 3 | 0x800 | m0[0x0F] |
| c2 | 3 | 0x800 | m0[0x06] |
| c3 | 3 | 0x800 | m0[0x07] |

open-apollo hands blocks out top-down per pool, slot after slot (`oa:driver/ua_strips.c:503-520`).
Our fixed layout always gives input i slot i (D6). With T1..T3 the pool tops:

```
hdr = T1 - 0x08 - 0x54*i    m0 = T1 - 0x40 - 0x54*i    m2 = T1 - 0x50 - 0x54*i    m4 = T1 - 0x54*(i+1)
m1  = T2 - 0x10*(i+1)
m3  = T3 - 0x10 - 0x2010*i  c0 = m3 - 0x800   c1 = m3 - 0x1000   c2 = m3 - 0x1800   c3 = m3 - 0x2000
```

Recipe tops (derived; slot 0 equals spec 6.4; headers 0-3 equal the x4 run's
`0x9d1b0 / 0x9d15c / 0x9d108 / 0x9d0b4`, `oa:HARDWARE-SESSIONS.md:209-210`):

| Input | hdr | m0 | m1 | m2 | m3 | m4 | c0 | c1 | c2 | c3 |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 0x9D1B0 | 0x9D178 | 0xC6FEE | 0x9D168 | 0x08FFDFF0 | 0x9D164 | 0x08FFD7F0 | 0x08FFCFF0 | 0x08FFC7F0 | 0x08FFBFF0 |
| 2 | 0x9D15C | 0x9D124 | 0xC6FDE | 0x9D114 | 0x08FFBFE0 | 0x9D110 | 0x08FFB7E0 | 0x08FFAFE0 | 0x08FFA7E0 | 0x08FF9FE0 |
| 3 | 0x9D108 | 0x9D0D0 | 0xC6FCE | 0x9D0C0 | 0x08FF9FD0 | 0x9D0BC | 0x08FF97D0 | 0x08FF8FD0 | 0x08FF87D0 | 0x08FF7FD0 |
| 4 | 0x9D0B4 | 0x9D07C | 0xC6FBE | 0x9D06C | 0x08FF7FC0 | 0x9D068 | 0x08FF77C0 | 0x08FF6FC0 | 0x08FF67C0 | 0x08FF5FC0 |
| 5 | 0x9D060 | 0x9D028 | 0xC6FAE | 0x9D018 | 0x08FF5FB0 | 0x9D014 | 0x08FF57B0 | 0x08FF4FB0 | 0x08FF47B0 | 0x08FF3FB0 |
| 6 | 0x9D00C | 0x9CFD4 | 0xC6F9E | 0x9CFC4 | 0x08FF3FA0 | 0x9CFC0 | 0x08FF37A0 | 0x08FF2FA0 | 0x08FF27A0 | 0x08FF1FA0 |
| 7 | 0x9CFB8 | 0x9CF80 | 0xC6F8E | 0x9CF70 | 0x08FF1F90 | 0x9CF6C | 0x08FF1790 | 0x08FF0F90 | 0x08FF0790 | 0x08FEFF90 |
| 8 | 0x9CF64 | 0x9CF2C | 0xC6F7E | 0x9CF1C | 0x08FEFF80 | 0x9CF18 | 0x08FEF780 | 0x08FEEF80 | 0x08FEE780 | 0x08FEDF80 |

Memspec (19 dwords; `oa:driver/ua_plugin_upload.c:136-159`):
`0x00150013, (hdr+6, m0), (m0+0x36, m1), (m0+0x37, m2), (m0+0x1B, m3), (m0+0x1C, m4), (m0+0x0E, c0), (m0+0x0F, c1), (m0+0x06, c2), (m0+0x07, c3)`.
Input 1: `00150013 0009d1b6 0009d178 0009d1ae 000c6fee 0009d1af 0009d168 0009d193 08ffdff0 0009d194 0009d164 0009d186 08ffd7f0 0009d187 08ffcff0 0009d17e 08ffc7f0 0009d17f 08ffbff0`.
Input 8: `00150013 0009cf6a 0009cf2c 0009cf62 000c6f7e 0009cf63 0009cf1c 0009cf47 08feff80 0009cf48 0009cf18 0009cf3a 08fef780 0009cf3b 08feef80 0009cf32 08fee780 0009cf33 08fedf80`.
Add a host-side unit test that compares our generator with these two lines.

With mask 0x01 and with 0xFF, the fixed layout is byte-identical to open-apollo's packing. Other
masks leave gaps; the DSP keeps no allocator (`oa:driver/ua_resource_pool.c:7-10`), so gaps are harmless.

### 2.7 One strip's transaction, word by word (input i, synch i, strip DSP)

17 command-ring entries, no reply descriptors. The transaction is complete when the command ring's
POSITION passes the last entry (`oa:driver/ua_strips.c:35-55`, `:610-705`, `:1689-1729`).

| # | Kind | Words |
|---|---|---|
| 1-10 | inline zero-fill | `{0x00080004, addr, 0, dwords}` for hdr 0x8, m0 0x38, m1 0x10, m2 0x10, m3 0x10, m4 0x4, c0-c3 0x800 each (row order) |
| 11 | DMA_REF 19 | the memspec (2.6) |
| 12 | DMA_REF 9 | `{0x00140009, hdr, 0x00000024, 0x40000000, 1, 1, 0x00000022, 1, 0x00000846}` (header; 0x22 is a header field, not the x8p's type; 0x846 = Mic Input Strip) |
| 13 | DMA_REF 55 | `{0x00140014, m0, M0A[18]}`, `{0x00140012, m0+38, M0B[16]}`, `{0x00140011, m3+1, M3W[15]}` |
| 14 | inline SYNCH | `{0x001F0004, 0x00200000 \| i, hdr, 0}` (starts the strip) |
| 15-16 | inline | `{0x001E0004, i, 8, 0x00000020}` twice (span: none) |
| 17 | inline | `{0x001E0004, i, 7, 0x0001FFFF}` (direct-out route: none) |

Payload page layout, packed like `ua_xact_cmd` (`oa:driver/ua_dsp_xact.c:132-147`): memspec at
+0x000, header at +0x04C, parameters at +0x070, end at +0x14C. The three entries point at
those offsets.

48 kHz parameter words (UA's captured layout, verified on the x4; `oa:driver/ua_strips.c:538-552`):

```c
static const ULONG M0A[18] = {
    0x0f1f7fe4, 0x40000000, 0x00000001, 0x00000001, 0x00000004, 0x00000100,
    0x000000c8, 0x3a888889, 0xc0000000, 0xc0000000, 0x00000010, 0x0000004b,
    0x41000000, 0x3ada740e, 0xffffffff, 0x00000001, 0xffffffff, 0x00000001 };
static const ULONG M0B[16] = {
    0x00000040, 0x3cae6c74, 0xbf7ff125, 0x3f7ff892, 0x3909422f, 0x3f800000,
    0xbc2e6fd3, 0xbbc96245, 0x3cae6c74, 0xbf7ff125, 0x3f7ff892, 0x3909422f,
    0x3f800000, 0xbc2e6fd3, 0xbbc96245, 0x00000001 };
static const ULONG M3W[15] = {
    0x00000040, 0x3cae6c74, 0xbf7ff125, 0x3f7ff892, 0x3909422f, 0x3f800000,
    0xbc2e6fd3, 0xbbc96245, 0x3e3b3f3d, 0xbf7baf04, 0x3f7dd5f1, 0x3c20d8d2,
    0x3f7ffcd7, 0xbdbc4dc9, 0xbd565d96 };
```

`StripSteps` bisect, as `input_strips_steps` (`oa:driver/ua_strips.c:91-94`):

| Value | Sends |
|---|---|
| 1 | Uploads only |
| 2 | + entries 1-10 |
| 3 | + 11 |
| 4 | + 12 |
| 5 | + 13 |
| 6 | + 14 (SYNCH) |
| 0 | All |

### 2.8 After each strip is taken (DevLock held)

1. `StripState[i] = Live`. `BusOwner[i] = StripDsp`. Re-send the cached coefficient cells of bus i to the owner
   (`oa:driver/ua_dsp.c:3866-3903`). Our driver has no bus-coefficient path yet, so the cache is
   empty and nothing is sent; the hook must exist before a `SET_BUS_PARAM` IOCTL is added.
   Rule for that IOCTL: `{0x001D0004, bus, (side << 24) | param, float}` goes to `BusOwner[bus]`,
   and also to DSP 0 when the owner is not 0 (`oa:driver/ua_dsp.c:3790-3829`).
2. For every DSP d other than StripDsp with live rings (x8p: 0, 2, 3, 4, 5):
   `{0x001E0004, i, 7, 0x0001FFFF}` plus the doorbell, fire and forget, through `ApolloRingPut`
   (`oa:driver/ua_strips.c:1740-1753`).

**Heads-up:** a strip on DSP 1 puts the input into the monitor mix at DSP 1's own default level,
with no coefficient sent (`oa:HARDWARE-SESSIONS.md:327-333`, `oa:driver/ua_dsp.c:3866-3870`). The mic
becomes audible on the monitors and headphones the moment the strip goes live.

### 2.9 Teardown words

One inline-only transaction on the strip DSP, published without waiting; it becomes the DSP's fence
(`oa:driver/ua_strips.c:747-752`, `:1823-1864`):

- `{0x001F0004, 0x00200000 | i, 0, 0}` for each input whose state is not `None` (`Queued` included).
- Then `{0x00030002, id, 0, 0}` for each module whose state is not `None`, in the order EB, DA, A5, C2, 12B.
- Then `BusOwner[i] = 0` for live strips, and every strip and module returns to `None`.

---

## 3. Transaction engine on our dsp.c

### 3.1 Changes to dsp.c

| Change | Why |
|---|---|
| `APOLLO_RING.Gen` (ULONG), incremented in `ApolloRingResync` whenever `Ring != NULL` (so also in `ApolloRingProgram`, `ApolloRingsInit`, `ApolloRingsQuiesce`) | A transaction whose ring was resynced under it has an unknown outcome (`oa:driver/ua_dsp.c:465-503`) |
| `NTSTATUS ApolloRingRefresh(Ctx, Ring)`: reads POSITION into `Ring->Position` through `ApolloRingReadPos` | Poll without the 100 ms wait inside `ApolloRingPut` |
| `ULONG ApolloRingFreeSlots(Ring)` = `(Position - WritePtr - 1) & 1023`; `ApolloRingPending(Ring)` = `(WritePtr - Position) & 1023` | `oa:driver/ua_dsp.c:130-151` |
| `VOID ApolloRingWrite(Ring, w0..w3)`: slot write and `WritePtr++`, no wait, the caller checked room | All-or-nothing publication |
| Export `ApolloRingDoorbell` (DOORBELL first, WRITE_PTR second, `dsp.c:312-317`) | |
| `NTSTATUS ApolloRingResyncNow(Ctx, Ring)` = `ApolloRingResync(Ctx, Ring->Base, Ring)` | Abandons a stale reply descriptor (`oa:driver/ua_dsp_xact.c:291-303`) |
| `ApolloDspSendInline(Ctx, Dsp, w0..w3)`: `ApolloRingPut` + doorbell; refuses w0 bit 31 | Route defaults (2.8); `oa:driver/ua_dsp.c:4035-4065` |

Nothing else in `dsp.c` changes. The existing senders (connect, finalize, rate ack, date-only
disconnect) keep `ApolloRingPut`. None of them expects a reply, so they cannot steal one of ours.

### 3.2 Transaction object and buffer pool

Allocate the buffers once in `ApolloEvtPrepareHardware` beside the rings, with the same checks as
`ApolloRingAlloc` (`dsp.c:20-46`): page aligned (use `WdfCommonBufferCreateWithConfig`,
alignment 0xFFF) and below 4 GiB (32-bit profile, `driver.c` `WdfDmaEnablerCreate`). Delete them
only in `ApolloEvtReleaseHardware`, after D0Exit's quiesce and 100 ms. A buffer the device may still
read is never freed while the device runs: open-apollo's equivalent rule (`oa:driver/ua_dsp_xact.c:24-28`)
guards against IOMMU faults, and our INF opts into DMA remapping (`apollo.inf` `DmaRemappingCompatible`).
The rule also holds at ReleaseHardware: the D0 exit only resyncs the rings and nothing resets
DSP DMA. So a parked (unproven) slot is deleted only when the device is gone. Otherwise its
buffer is left to the DMA enabler's deletion with the device (`oa:driver/ua_dsp_xact.c:803-806`).

```c
#define XACT_SLOTS       16
#define XACT_PAGE        0x1000
#define XACT_BYTES       (2 * XACT_PAGE)   /* page 0: DMA payload, page 1: reply area */
#define XACT_ENTS_MAX    32                /* largest: a strip, 17 entries */
#define XACT_RX_MAX      2
#define XACT_WAIT_MS     1000              /* oa:driver/ua_plugin_host.h:133 */
#define XACT_PUBLISH_MS  50                /* oa:driver/ua_plugin_host.h:134 */

typedef enum { XactFree, XactBuilding, XactPublished, XactRetired, XactParked } XACT_STATE;
typedef struct { BOOLEAN Pending; ULONG Gen; ULONG End; } XACT_REF;

typedef struct _APOLLO_XACT {
    WDFCOMMONBUFFER Buffer;  PUCHAR Va;  ULONGLONG La;
    XACT_STATE State;
    ULONG      Dsp, Epoch;
    NTSTATUS   Err;                          /* sticky builder error */
    ULONG      N, Ent[XACT_ENTS_MAX][4];     /* DMA refs: Ent[k][2] = payload offset */
    ULONG      Len;                          /* payload bytes used in page 0 */
    ULONG      NRx, RxWords[XACT_RX_MAX], RxOff[XACT_RX_MAX];
    XACT_REF   Ref[2];                       /* [0] command ring, [1] response ring */
    ULONGLONG  PublishedAt;                  /* KeQueryInterruptTime */
} APOLLO_XACT;
```

`XactAlloc(Dsp, Epoch)` (DevLock held) first reclaims parked slots whose refs a poll now proves,
then takes a `Free` slot and zeroes both pages and the struct. With no free slot it returns
`STATUS_DEVICE_BUSY` and publishes nothing. A healthy session needs 5 + 8 + 1 slots in turn, and a
parked slot always comes with a quarantine, so 16 never run out.

### 3.3 Entry formats

| Kind | Ring | w0 | w1 | w2 | w3 | Use |
|---|---|---|---|---|---|---|
| Inline | command | `opcode << 16 \| dwords`, bit 31 clear | arg | arg | arg | Commands of 4 dwords or fewer: 0x0008, 0x001E, 0x001F, 0x0003, 0x001A, 0x001D |
| DMA_REF | command | `0x80000000 \| dwords` | 0 | bus addr lo | bus addr hi | The FPGA fetches the dwords and runs them as a command stream, so one ref may carry several commands (entry 13). Used for 0x0001 upload, 0x0014 write, 0x0015 memspec. Never crosses a 4 KiB page; 1 KiB pieces took the x4 off the bus (`oa:driver/ua_dsp_xact.c:110-121`) |
| Reply descriptor | response | `0x80000000 \| 4` | 0 | reply lo | reply hi | The DSP writes its 4-dword reply there. Each descriptor sits on a 16-byte slot of page 1 (`oa:driver/ua_dsp_xact.c:157-177`) |

Builders: `XactInline(X, w0..w3)` refuses bit 31. `XactCmd(X, dwords)` returns a `PULONG` into page 0 at
`Len`, adds `{0x80000000 | dwords, 0, offset, 0}` and refuses past 4096 bytes (v1 never needs
more). `XactRx(X, dwords)` places a reply slot in page 1. Any failure sets `Err`, which sticks.

### 3.4 Publishing (DevLock held; port of `oa:driver/ua_dsp_xact.c:249-365`)

```c
NTSTATUS XactSubmit(PDEVICE_CONTEXT Ctx, APOLLO_XACT *X)
{
    PAPOLLO_RING cmd = &Ctx->Cmd[X->Dsp], resp = &Ctx->Resp[X->Dsp];
    ULONG k;

    if (X->State != XactBuilding || !NT_SUCCESS(X->Err)) return STATUS_INVALID_DEVICE_STATE;
    if (ApolloGateClosed(Ctx))                         return STATUS_DEVICE_NOT_CONNECTED;
    if (X->Epoch != Ctx->Strips.Epoch)                 return STATUS_CANCELLED;      /* stale */
    if (X->Dsp >= Ctx->NumDsps || !cmd->Live || (X->NRx && !resp->Live))
                                                       return STATUS_INVALID_DEVICE_STATE;
    if (!NT_SUCCESS(ApolloRingRefresh(Ctx, cmd)))      return STATUS_DEVICE_NOT_CONNECTED;
    if (ApolloRingFreeSlots(cmd) < X->N)               return STATUS_RETRY;
    if (X->NRx) {
        if (Ctx->Strips.ReplyWaiting)                  return STATUS_DEVICE_BUSY;
        if (ApolloRingPending(cmd))                    return STATUS_RETRY;  /* nothing ahead may answer into our slot */
        (VOID)ApolloRingRefresh(Ctx, resp);
        if (ApolloRingPending(resp)) {                 /* someone's unanswered descriptor */
            (VOID)ApolloRingResyncNow(Ctx, resp);      /* Gen++ */
            if (ApolloRingPending(resp))               return STATUS_RETRY;
        }
        if (ApolloRingFreeSlots(resp) < X->NRx)        return STATUS_RETRY;
    }
    for (k = 0; k < X->NRx; k++) {                     /* replies first */
        ULONGLONG a = X->La + XACT_PAGE + X->RxOff[k];
        ApolloRingWrite(resp, RING_DMA_REF | X->RxWords[k], 0, (ULONG)a, (ULONG)(a >> 32));
    }
    for (k = 0; k < X->N; k++) {
        ULONG w2 = X->Ent[k][2], w3 = X->Ent[k][3];
        if (X->Ent[k][0] & RING_DMA_REF) { ULONGLONG a = X->La + w2; w2 = (ULONG)a; w3 = (ULONG)(a >> 32); }
        ApolloRingWrite(cmd, X->Ent[k][0], X->Ent[k][1], w2, w3);
    }
    KeMemoryBarrier();                                 /* payload and slots before any doorbell */
    if (X->NRx) { ApolloRingDoorbell(Ctx, resp); X->Ref[1] = (XACT_REF){ TRUE, resp->Gen, resp->WritePtr };
                  Ctx->Strips.ReplyWaiting = TRUE; }
    if (X->N)   { ApolloRingDoorbell(Ctx, cmd);  X->Ref[0] = (XACT_REF){ TRUE, cmd->Gen,  cmd->WritePtr }; }
    X->State = XactPublished;
    X->PublishedAt = KeQueryInterruptTime();
    return STATUS_SUCCESS;
}
```

Room for both rings is checked before the first slot write, so publication is all or nothing and
needs no unwind. `XactPublish` retries `STATUS_RETRY` every 1 ms for 50 ms, releasing DevLock
between tries (`oa:driver/ua_dsp_xact.c:457-470`). After that it gives `STATUS_DEVICE_BUSY`: nothing
was published, nothing changed on the DSP, and nothing is quarantined.

### 3.5 Completion, waiting, timeouts

```c
/* DevLock held. 1 = retired (replies readable), 0 = queued, <0 = outcome unknown. */
int XactPoll(PDEVICE_CONTEXT Ctx, APOLLO_XACT *X)
{
    if (X->State == XactRetired) return 1;
    if (ApolloGateClosed(Ctx))   return -1;                                    /* gone */
    if (X->Epoch != Ctx->Strips.Epoch) return -2;                              /* stale */
    for (ULONG k = 0; k < 2; k++) {
        XACT_REF *r = &X->Ref[k];
        PAPOLLO_RING ring = k ? &Ctx->Resp[X->Dsp] : &Ctx->Cmd[X->Dsp];
        if (!r->Pending) continue;
        if (!ring->Live || ring->Gen != r->Gen) return -2;                     /* resynced under it */
        if (!NT_SUCCESS(ApolloRingRefresh(Ctx, ring))) return -1;
        ULONG dEnd = (r->End - ring->Position) & RING_INDEX_MASK;
        if (dEnd == 0 || dEnd > ApolloRingPending(ring)) r->Pending = FALSE;   /* oa:driver/ua_dsp_xact.c:367-384 */
    }
    if (X->Ref[0].Pending || X->Ref[1].Pending) return 0;
    KeMemoryBarrier();                         /* POSITION past the descriptor: the reply words are in */
    if (X->NRx) Ctx->Strips.ReplyWaiting = FALSE;
    X->State = XactRetired;
    return 1;
}
```

`XactWait` (no lock held): loop { DevLock; `XactPoll`; release; if != 0 return; if
`KeQueryInterruptTime() > PublishedAt + 1000 ms` then `STATUS_IO_TIMEOUT`; `ApolloSleepMs(1)` }.
It polls **before** checking the deadline. The bring-up work item can hold DevLock for seconds;
a transaction the DSP finished meanwhile must still count as done.

`XactRun` = publish + wait. On any error after publication except "device gone", it logs one line
(`cmd end/pos, resp end/pos, reply word 0`, as `oa:driver/ua_dsp_xact.c:504-517`), quarantines
the DSP and parks the slot (`oa:driver/ua_dsp_xact.c:531-552`).

| Wait | Value | Source |
|---|---|---|
| Publish retry (ring room, busy command ring) | 50 ms, 1 ms steps | `oa:driver/ua_plugin_host.h:134` |
| Upload reply, strip consumption, teardown fence | 1000 ms from publication | `oa:driver/ua_plugin_host.h:133` |
| Poll step | 1 ms (D0Entry sets 1 ms timer resolution, `device.c:150-153`) | `oa:driver/ua_dsp_xact.c:495` |
| Settle after the connect, and the rate-gate retry | 500 ms | `oa:driver/ua_strips.c:760`, `:1652-1656` |
| 48V off before on | the off committed (SEQ_RD caught up), then 200 ms | `oa:driver/ua_core.c:2459`, `:2491` |

### 3.6 Reply check, quarantine, parking

- **Upload.** Check the reply as in 2.5. Anything else: log `strips: DSP %u refused module 0x%03x: reply %08x %08x %08x %08x`, quarantine.
- **Quarantine** (`oa:driver/ua_plugin_host.c:318-331`): `Strips.Quarantined = TRUE`, state Failed,
  `Why` set. Nothing more goes to that DSP until the next cold bring-up. Log
  `strips: DSP %u quarantined until the Apollo is power-cycled: %s (0x%08x)`.
- **Release.** A slot in `Building` or `Retired` goes back to `Free`. A slot in `Published` (unproven) goes to `Parked`.
  A parked slot is freed when a later poll finds both refs passed in the same `Gen`, or at
  ReleaseHardware if the device is gone. Otherwise it is left to the device's deletion (3.2).
- **Fence.** The latest teardown transaction of the strip DSP. Every pass and the D0 exit quiesce wait for it
  first (1 s). If it is not taken, the DSP is quarantined (`oa:driver/ua_plugin_host.c:146-168`).

### 3.7 Locks

New in `DEVICE_CONTEXT`: `WDFWAITLOCK StripLock` (open-apollo's `host->mutex`), `WDFWORKITEM StripWork`
(parent: the device), and the `APOLLO_STRIPS` state (section 9). Order: **StripLock, then DevLock**.

| Context | Holds | Rule |
|---|---|---|
| Bring-up work item (`ApolloEvtBringupWork`) | DevLock for the whole bring-up | Records boot words, sets `Owned`, arms. Never waits on a transaction. |
| Service thread (`ApolloServicePass`, 10 ms) | DevLock per pass | Arms `StripWork` (`WdfWorkItemEnqueue` does not block; it is a no-op if already queued). Never waits on a transaction. |
| IOCTL queue (sequential, PASSIVE) | DevLock (3 s acquire timeout, `ioctl.c` `ApolloLockForIoctl`) | `STRIPS_REBUILD` publishes the teardown and re-arms; it does not wait. Any `WRITE_REG` outside 0x3800-0x38FF taints (5.4). |
| `StripWork` (the pass) | StripLock for the pass; DevLock only for publish, one poll, a state change or the route sends | Never sleeps or waits holding DevLock. Holding it would stall the mixer flush, the readback drain, position updates and playback zero-ahead, and IOCTLs would fail with `STATUS_DEVICE_BUSY`. |
| D0 exit quiesce | none on entry, then StripLock, then DevLock | Never call `WdfWorkItemFlush(StripWork)` or `ApolloServiceStop` while holding either lock. |

Existing `ApolloRingPut` callers (`ApolloDspRateAck` from `ApolloTransportStart`, `audio.c:313-315`)
may queue on DSP 1 between a reply transaction's publication and its reply. That is harmless:
those commands get no reply. A reply transaction is never published behind them (3.4).

---

## 4. Module bytes on Windows

### 4.1 Design

1. `apollo-diag strips install [dir]` (elevated, 64-bit, so `System32` is not redirected) scans
   the user's UAD DLLs for the five records, keeps only those whose length, w4 and SHA-256 equal
   the pins, and writes them byte-for-byte to `%SystemRoot%\System32\drivers\ApolloStrips\<id>.bill`
   (`0eb.bill 0da.bill 0a5.bill 0c2.bill 12b.bill`). The directory gets the protected DACL
   `D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)` and each file `D:P(A;;FA;;;SY)(A;;FA;;;BA)`. That is
   admin-only, with **no Users ACE**: a user who could open a `.bill` file with share mode 0 or a
   batch oplock could stall the driver's read, and the D0 exit's flush of that work, so sleep or
   removal would hang. The driver also opens with `FILE_COMPLETE_IF_OPLOCKED` and refuses on
   `STATUS_OPLOCK_BREAK_IN_PROGRESS`.
2. At the first pass (StripWork, PASSIVE, no locks), the driver opens exactly those five names,
   reads exactly the pinned length and re-verifies everything (4.3). The verified copies stay in
   paged pool for the device's lifetime.
3. No IOCTL accepts module bytes or a path. The pins, not the DACL, are the security boundary
   (open-apollo's equivalent: `oa:driver/ua_plugin_catalog.c:7-13`). The DACL only stops a
   non-admin from replacing the files with junk and disabling the strips.

The directory is the admin-only location, rather than an IOCTL fed by a service, because the driver
then needs no user-mode component at boot or after a power cycle.

### 4.2 Pins (compile into `include/apollo_strip_pins.h`, shared by the driver and apollo-diag)

| id | len | w4 | pool dwords (w4*2/3) | SHA-256 of the record |
|---|---|---|---|---|
| 0x0EB | 716 | 162 | 0x06C | `2b879e0824970650f4ae0de110bb4bdb22897d0833d85156dd36f51986c15ea7` |
| 0x0DA | 3044 | 744 | 0x1F0 | `37cd9c2df53ec742d61353437ad7f7212633e27801f1cf1f57e3818cbe47dd06` |
| 0x0A5 | 1940 | 456 | 0x130 | `1fe90428f88b7f8c0a35b01ea0cfca3937d4b365b7c6ef427d24cfee65ee6c81` |
| 0x0C2 | 184 | 33 | 0x016 | `ad0946b371df47c9a7d459c7b5b288d58aad9690b6a9a39a6c5895ac36720a2f` |
| 0x12B | 452 | 96 | 0x040 | `ff60952444ae492aa1a8b85dae686ad8445b9971260174202ad1ce13c73ba02d` |

Sources: `oa:console-rs/crates/oa-assets/data/firmware-pins.json:10-46` and the length/pool table at
`oa:driver/ua_strips.c:221-229`. 0x0DB and 0x0C9 are pinned upstream but reserved; the driver
refuses them. The user's UAD **11.5.2.43082** holds all five byte-identical to these 12.0.0.602 pins:
`node tools/find-strip-modules.js ua-files` re-run today found every one in
`UAD Mic Input Strip Header.dll` (also in `UAD2SDK.dll`). PLAN.md decision 7 ("update UAD to
12.0.0.602") is therefore unnecessary. The pins header holds lengths and hashes only, no UA bytes.

### 4.3 Driver loader (port of `oa:driver/ua_strips.c:256-274` plus the hash)

Per module j:

- `ZwCreateFile(L"\\SystemRoot\\System32\\drivers\\ApolloStrips\\0eb.bill", GENERIC_READ | SYNCHRONIZE,
  OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, FILE_SHARE_READ, FILE_OPEN,
  FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT)`.
- `FileStandardInformation.EndOfFile` must equal the pinned length. Read exactly that many bytes.
- Then all of these must hold:
  - `le32(0) == 0x6C6C6942` ("Bill")
  - `le32(4) == id`
  - `le32(8) == 0x02000000`
  - `le32(12) == len - 20`
  - `le32(16) == w4`
  - `w4 * 2 / 3 == pool dwords`
  - SHA-256 over the whole record equals the pin
- SHA-256 through kernel CNG at PASSIVE: `BCryptOpenAlgorithmProvider(BCRYPT_SHA256_ALGORITHM)`,
  then `BCryptHash`. Link `cng.lib`. A ~150-line built-in SHA-256 is an acceptable alternative.

Any failure: state Off, `Why` = "strip module 0x%03x missing or not the pinned bytes (run apollo-diag
strips install)". Nothing is sent. Never log module contents, only id, length and the first 8 hex
digits of the hash.

### 4.4 The extractor (apollo-diag)

- **Search** (same order as `oa:console-rs/crates/oa-assets/src/firmware.rs:236-259`, `:383-416`):
  `[dir]` if given; else `UAD Mic Input Strip Header.dll` up to 3 levels under
  `%ProgramFiles%` and `%ProgramFiles(x86)%\Universal Audio\Powered Plugins`; then
  `%SystemRoot%\System32\UAD2SDK.dll`. `apollo-win/ua-files/` (the Nov 14 copies) works as `[dir]`
  if UAD gets uninstalled.
- **Scan** raw bytes at every 4-byte-aligned offset (`oa:console-rs/crates/oa-assets/src/firmware.rs:200-228`).
  Accept a record when all of these hold:
  - the bytes read `Bill`;
  - the id's top byte is 0;
  - w2 == 0x02000000;
  - length = 0x14 + w3, and the record fits in the file.

  Keep a record only if (id, len, w4, SHA-256) equals a pin.
- Commands:

  | Command | Elevated | What it does |
  |---|---|---|
  | `strips verify [dir]` | no (yes to check the installed copies: the folder is admin-only) | Scan and report only |
  | `strips install [dir]` | yes | Write the five files and set the DACL |
  | `strips` | no | `GET_STRIPS`, section 9 |
  | `strips rebuild` | yes | Teardown, then rebuild |

### 4.5 Licensing boundary

UA's records never go into the repo, the driver binary, the driver package, the app or the log. They
are read from the user's own install on the user's own disk (`ua-files/README.txt`: "local use only, never commit or share").

---

## 5. Lifecycle

### 5.1 State machine (`Ctx->Strips.State`)

| State | Meaning | Entered |
|---|---|---|
| Off | Disabled, with `Why` | `StripsEnable = 0`; modules missing or unpinned; layout refused; warm bring-up; tainted |
| Idle | Nothing of ours on the DSP | Cold bring-up with `Owned`; after a proven teardown |
| Armed | Waiting for the settle and the rate gate | End of `ApolloAudioConnect` (`audio.c:215`) when Idle and Owned; `ArmedAt = now` |
| Building | Pass running | `StripWork` |
| Live | Pass over, strips running | |
| Failed | Quarantined, or a step failed | |

### 5.2 Build trigger and gates

The service thread (DevLock held) enqueues `StripWork` when all of these hold:

- State is Armed;
- `now - ArmedAt >= 500 ms`;
- `!StopRequested`.

The pass begins (StripLock, fence wait, then DevLock) and re-checks every gate
(`oa:driver/ua_strips.c:1388-1474`):

- `StripsEnable >= 1`, `Owned`, `!Quarantined`, `!Tainted`.
- `Connected && DspsConnected && Cmd[d].Live && Resp[d].Live && d < NumDsps && d < 4`.
- **Rate ready** (`oa:driver/ua_audio.c:5168-5180`):
  `SampleRate == 48000 && DspToldRate == 48000 && (RATE_INFO & 3) == 1 && ((RATE_INFO >> 8) & 0xFF) == 2`.
  Read RATE_INFO fresh from 0xC07C. If this fails, set `ArmedAt = now`, stay Armed, and the service
  thread retries in 500 ms.
- Modules loaded and verified (4.3), loaded outside DevLock before this point.
- Layout check passes on freshly read registers (2.4).

The transport state is not a gate. Linux builds 0.5 s after the connect, before any PCM open
(`oa:driver/ua_audio.c:3370-3381`), and a later transport start with the same rate leaves the strips
alone. On this x8p the first connect acks 44.1 kHz (install.log). The gate therefore opens at the first
`ApolloTransportStart`: its clock write at index 2 and its rate ack 48000 (`audio.c:311-315`) satisfy it.
A DSP that has not been told the rate stalls on SYNCH (`oa:HARDWARE-SESSIONS.md:322-323`).

### 5.3 The pass (StripWork; port of `oa:driver/ua_strips.c:1626-1786`)

```
StripLock
  load + verify modules (once per device instance)            no DevLock
  fence wait (<= 1 s)                                         DevLock per poll
  DevLock: gates (5.2); State = Building; gen = Gen; epoch = Epoch; Dry run? log the plan, State = Idle, out
  for j in EB, DA, A5, C2, 12B:                               (Steps >= 1)
      DevLock: if Gen != gen -> out; ModState[j] = Uploading; build upload X
      XactRun(X); check reply -> ModState[j] = Resident, or quarantine and out
  if Steps != 1: for each i in StripMask (ascending):
      DevLock: if Gen != gen or StopRequested -> out; build strip X for i (2.7, cut at Steps); StripState[i] = Queued
      XactRun(X)                                              wait for command-ring consumption
      DevLock: if Gen != gen -> out; failure -> Failed, out; StripState[i] = Live; AckMs[i]; 2.8 steps 1-2
  DevLock: if Gen == gen: State = Live; phantom = PhantomPending; PhantomPending = FALSE
  if phantom && StripPhantomResync: 48V resync (5.6)
StripLock released
```

Each Live strip logs: `strips: input %u: Mic Input Strip at 0x%05x on DSP %u (taken after %u ms)`.

### 5.4 Teardown and loss

| Event | Action |
|---|---|
| D0 exit, any target state (`device.c:190-214`) | New order: `StopRequested = 1`; flush `BringupWork`; `ApolloServiceStop`; **`ApolloStripsQuiesce`** (flush `StripWork`; StripLock, DevLock, teardown publish (2.9); DevLock released; fence wait 1 s); then DevLock and `ApolloHwQuiesce`. The fence must be proven before `ApolloRingsQuiesce` (`dsp.c:452-479`) resyncs the rings and sends the date-only disconnect, or the resync abandons the teardown. |
| Rate change (future) | Teardown before the clock write, re-arm after the new rate ack (`oa:driver/ua_audio.c:4572-4610`). v1 runs only at 48 kHz. Guard: if strips are Live and `ApolloTransportStart` would ack a rate other than 48000, tear down first. |
| `STRIPS_REBUILD` IOCTL | Teardown, then Armed (`oa:driver/ua_strips.c:1894-1913`) |
| `WRITE_REG` outside 0x3800-0x38FF | Taint, as `oa:driver/ua_plugin_place.c:147-160`: `Owned = FALSE`, Off, `Why` = "raw register write: strips off until a power cycle". Nothing is sent. |
| Surprise removal, all-ones reads | Lost (`oa:driver/ua_strips.c:1874-1888`): forget everything, send nothing, disown, `Epoch++`. Parked buffers are freed at ReleaseHardware. |
| Teardown publish fails (ring full) | Disown: "DSP unknown, power-cycle" (`oa:driver/ua_strips.c:1850-1856`) |

`Epoch` is bumped at every bring-up start, taint and loss; `Gen` at every teardown.

### 5.5 Warm starts, reloads, sleep (v1 policy)

**Superseded Nov 15 2026 by section 12** (adoption of this driver's own strips; `StripAdopt=0` brings
back the policy below). Kept for the history.

- **Strips are built only after a cold bring-up in this device instance (D9).** A warm bring-up
  logs `strips: not built: the DSPs were running when this driver started; power-cycle the Apollo`.
- **Driver reload or reinstall** without an Apollo power cycle: the D0 exit tore the strips down,
  the next load is warm, and the preamps stay silent until a power cycle. open-apollo behaves the
  same (`oa:HARDWARE-SESSIONS.md:227-232`). Every strip test needs its own power cycle.
- **Sleep and resume:** the Apollo stays powered, so resume is warm: no strips until a power cycle.
- **S2 options** (not in v1):
  - Keep ownership across D0 exits in the same device instance when the teardown fence was proven
    (`oa:driver/ua_plugin_place.c:125-128`), then rebuild without a ring reconnect. Rebuild without
    reconnect is proven (50 cycles, `oa:HARDWARE-SESSIONS.md:243-261`), but not after our date-only
    disconnect.
  - Leave the strips running at D3Final so a reload inherits them. Warm loads skip the ring connect,
    and a skipped connect keeps strips another session built (`oa:CHANGELOG.md:221`).

### 5.6 48V resync on 8 preamps (port of `oa:driver/ua_core.c:2475-2499`)

(As implemented, with the guards from the review: two matching valid readbacks, no resync during a
D0 exit, the on flushed for up to 2 s, host 48V writes respected. See 10.7.)

Runs after the first pass that ends Live following a cold bring-up. A rate rebuild keeps the supply up.

1. DevLock: require `ReadbackValid` with `ReadbackSeq` advanced since `ApolloMixerInit`; otherwise
   log and skip. `on` = preamps in StripMask that the unit reports with 48V on:
   - ch 0-3: `Readback[0]` bit `6*ch + 3`;
   - ch 4-7: `Readback[5]` bit `6*(ch-4) + 3` (`oa:mixer-engine/hardware.py:5307`).

   The upstream function covers only ch 0-3; this is the extension (derived). For each preamp in `on`:
   `ApolloSetInputParam(ch, APOLLO_PARAM_48V, 0)`. Note `seq = MixerSeqWr`. Release.
2. Without the lock, poll every 10 ms (up to 500 ms) until `MixerSeqWr > seq` and
   `SEQ_RD == MixerSeqWr` (the off was committed), then sleep 200 ms. The off and the on must not
   merge into one batch.
3. DevLock: `ApolloSetInputParam(ch, APOLLO_PARAM_48V, 1)` for `on`. Log
   `strips: 48V re-applied after the cold start (preamp mask 0x%02x)`.

This never turns phantom on where the unit reports it off (`oa:HARDWARE-SESSIONS.md:329-331`).

Check `StopRequested` before step 1. Once the off is committed, the on must be committed too, even
if a D0 exit started meanwhile: D0 exit stops the service thread first, so write the on and call
`ApolloMixerFlush` under DevLock yourself (wait up to 100 ms for `SEQ_RD == MixerSeqWr`). Otherwise
the unit is left with 48V off, and the next cold start sees "off" and never restores it.

---

## 6. The mixer-init batch on the x8p

`ApolloMixerInit` (`mixer.c:47-55`) sends S1..S37 except S2 as 0x20/0xFF, and S35 as 0x05/0x1F.

- **Origin.** An x4 Windows capture, kept "for parity"; its justification ("setting 24 = 0x20
  enables type 0x01 capture") is about the x4 (`oa:driver/ua_audio.c:3251-3296`).
- **On the x8p**, the low byte of several of these settings is a preamp field:

| Setting | Bits 7:0 on the x8p (spec 5.2) | What 0x20/0xFF did |
|---|---|---|
| S3 | GainC preamp 1 (5:0), preamp 2 (7:6) | Input 1 GainC 0x20 = 41 dB; input 2 GainC low bits cleared |
| S12 | Preamp 5 flags (5:0), preamp 6 Mic/Line and PAD (7:6) | **Input 5 polarity inverted**, 48V/PAD/Line/LowCut/Link off; input 6 Mic, PAD off |
| S13 | Hi-Z impedance inputs 1-2 (nibbles 0-1) | Input 2 Hi-Z code 2 |
| S14 | GainC preamp 5 (5:0), preamp 6 (7:6) | Input 5 at 41 dB |
| S16, S17 | GainB, GainA preamp 1 | Input 1 at 42 dB |
| S24, S25 | GainB, GainA preamp 5 | Input 5 at 42 dB |
| S23 | "knob steps" (`oa:driver/ua_audio.c:3289-3296`) | unknown |
| S18-S22, S26-S30 | Byte lane per preamp of params 0x09-0x0f; S16-S22 for preamps 1-4, S24-S30 for 5-8 (`oa:driver/ua_core.c:2610-2616`) | Byte 0 (preamps 1 and 5) set to 0x20 |
| S31 | Knob steps of preamps 5-8 (`oa:driver/ua_core.c:2617-2622`) | Preamp 5's nibble set |

This matches what the user saw (preamp 1/5 gain, preamp 5 polarity). The "41 dB" on the silent
cold-start preamps came from this batch, not from the user.

**Do the strips need it?** Unproven either way. The strips are pure ring traffic; the only
mailbox use in the strip path is the 48V resync. But every verified strip run had the batch,
because open-apollo sends it unconditionally except on an inherited x4 DSP
(`oa:driver/ua_audio.c:3297-3308`). And the x8p's MON-to-slots-26-27 path was verified with it.

**What the x8p sends instead** (registry `MixerInitBatch`):

| Value | Sends |
|---|---|
| 1 = x8p-safe | 0x20/0xFF on S1, S4-S11, S15, S32-S34, S36, S37; S35 = 0x05/0x1F; never S0, S2, S3, S12-S14, S16-S31 (fixed Nov 14, 10.7) |
| 0 | Nothing |
| 2 | The legacy full batch, for comparison only |

Test T5 (section 8) decides between 0 and 1: if slot 0 (strip) and slots 26-27 (MON tone) both
pass with 0, make 0 the default. Without the batch, cold-start preamp gains are the unit's own,
so the host sets gain explicitly (`apollo-diag gain 1 40`).

---

## 7. Risks

| Risk | Evidence | Mitigation |
|---|---|---|
| **Worse than a hung DSP: the whole unit falls off the bus.** The A1 parameters hung the x4 about 10 ms after SYNCH (BAR0 all ones, DLL replay timeouts on the upstream port). 1 KiB DMA pieces did the same. On Windows that means a surprise removal of the x8p **and the daisy-chained Twin X**. An MMIO access racing a dead Thunderbolt link can stall the CPU; Driver Verifier is on, so any driver mistake on that path is a bugcheck. | `oa:HARDWARE-SESSIONS.md:324-326`, `oa:driver/ua_dsp_xact.c:115-118` | Only the verified words (captured 48 kHz parameters, SDK form, single-page DMA refs). An all-ones POSITION ends every poll as "device gone" (`ApolloRingReadPos`), and the strip work then stops; `ApolloEvtSurpriseRemoval` closes the MMIO gate. Bisect with `StripSteps`, one power cycle per step. Recovery: power-cycle the x8p; if Windows hangs, reboot; turn strips off before re-plugging (section 8). |
| A DMA fault (IOMMU) from a freed or stale buffer | The captured chain faulted the IOMMU and DSP 0 stopped consuming (spec 1.7); our INF enables DMA remapping | Preallocated buffers, freed only at ReleaseHardware; unproven slots parked (3.6) |
| Wrong geometry overwrites live DSP memory and crashes the DSP | Pools are host-managed; the DSP keeps no allocator (`oa:driver/ua_resource_pool.c:7-10`) | Boot-word ownership (2.3) plus the layout check before any command (2.4) |
| A stale reply is taken as ours | Replies fill in ring order (`oa:driver/ua_dsp.c:2012-2026`) | Reply transactions only behind an idle command ring and an empty response ring; any mismatch quarantines |
| A hung DSP or failed transaction | Quarantine until a power cycle (`oa:driver/ua_dsp_xact.c:519-549`) | The DSP stays usable for the mixer; only the strip work stops. Power-cycle. |
| Synch 4-7 never carried a strip on any unit | x4 has 4 preamps | Input 1 first, then 0xFF, then each input in turn (T6) |
| The x8p's geometry or boot word may differ from the x4/Twin X | Different firmware release | Dry run T2 reads both and sends nothing |
| Mic in the monitors as soon as the strip is live (feedback with speakers) | 2.8 heads-up | Headphones, or monitors down, for every strip test |
| The 48V resync drops phantom for about 200 ms on preamps reported on (a pop) | 5.6 | Once per cold start; never switches phantom on |
| A UAD update changes module bytes; an x8p firmware update moves the pools | Pins and layout are exact | Both refuse cleanly with a log line; re-pin after checking |
| Persistent damage | None of these commands writes flash: they are DSP RAM, plus the mailbox settings the unit already accepts from the host. The firmware stage stays off. | |

---

## 8. Bring-up and test order on the x8p

Before every step:

- Monitors down, or headphones.
- **Dynamic** mic on input 1.
- UA Console and UA Connect closed.
- Have `apollo-diag log` ready.

"Power cycle" = x8p off 10 s, then on. This also drops the Twin X behind it; expect that.

Kill switch (admin), then disable/enable the device or power-cycle:
`reg add HKLM\SYSTEM\CurrentControlSet\Services\apollo\Parameters /v StripsEnable /t REG_DWORD /d 0 /f`

| # | Settings | Do | Pass |
|---|---|---|---|
| T0 | (build) | Host-side unit test of the planner: memspec, header and parameter words, addresses, layout check against the x4 registers | Byte-equal to 2.6 and 2.7 |
| T1 | | `apollo-diag strips verify ua-files`, then (admin) `apollo-diag strips install` | Five records, pins match, files written. After the next driver start, `apollo-diag strips` shows modules verified. |
| T2 | `StripsEnable=2` (dry run), mask 0x01 | Power cycle; wait for Connected; `apollo-diag start` (opens the rate gate); `apollo-diag strips`; `apollo-diag log` | DSP 1 boot word & ~0xFF00 == 3 (expect about 0x00002203); 11 geometry registers equal the 2.4 table; "layout ok"; the planned header 0x9D1B0. **Zero DSP commands.** Stop here if anything differs: report the values. |
| T3 | `StripsEnable=1`, `StripSteps=1` | Power cycle (the transport now auto-starts) | 5 uploads, replies `80070004 00000000 000000eb ...` (one per id); modules Resident; not quarantined; `tone 3` still reaches slots 26-27. **Exact procedure: section 10.3.** |
| T4 | `StripSteps` 2, 3, 4, 5, 6, 7, 8, then 9, mask 0x01 | One power cycle per stage; `gain 1 40`; `levels 10` while speaking | **Exact ladder and procedure: section 10.6.** Pass: `input 1: Mic Input Strip started at 0x9d1b0 on DSP 1`, then **slot 0 shows a varying level while speaking and returns near the noise floor when silent; slots 1-7 stay `zero`.** |
| T5 | `MixerInitBatch=1`, then `=0` (at the stage T4 passed) | One power cycle each; `gain 1 40`; `levels` while speaking; `tone 3` + `levels` | **Section 10.6.** If both pass, make 0 the default; if only 1 passes, keep 1. Note input 5's polarity and gain readback in each. |
| T6 | mask 0xFF | Power cycle; `start`; mic into each input in turn; `gain N 40`; `levels` | Input N lands in slot N-1 only; 8 strips at the 2.6 headers |
| T7 | (T6) | `apollo-diag strips rebuild` x20 | Each logs "8 strip(s) stopped, modules released" then 8 Live lines; no quarantine; slot 0 returns each time |
| T8 | (T6) | Condenser on input 3 with `48v 3 1`; power cycle; `start` | `48V re-applied ... mask 0x04`; slot 2 has signal |
| T9 | (T6) | Disable/enable the device in Device Manager (warm); then power cycle | Warm: "not built ... power-cycle", nothing sent. After the power cycle: built again. |
| T10 | (T6) | `pos 30` with the strips live; pull the Thunderbolt cable | No bugcheck. Strips Lost. Replug + power cycle rebuilds them. |

If a step drops the unit off the bus:

1. Write down the step and the last `apollo-diag log` lines.
2. Power-cycle the x8p.
3. Set `StripsEnable=0` if Windows does not recover on its own.
4. Continue the bisect from the previous `StripSteps` value.

T4 with mask 0x01 is the first moment the mic can reach slot 0; T6 is the goal.

---

## 9. New interfaces

(The plan. The T3 build implements only a subset, and its `APOLLO_STRIPS_INFO` differs; see 10.2.)

**Registry** (`HKLM\SYSTEM\CurrentControlSet\Services\apollo\Parameters`): read in PrepareHardware
through `WdfDriverOpenParametersRegistryKey`.

| Value | Default | Meaning |
|---|---|---|
| `StripsEnable` | 0 until T6 passes, then 1 | 0 off, 1 build, 2 dry run (gates, modules, geometry and the plan logged; nothing sent) |
| `StripMask` | 0x01, then 0xFF | bit i = input i+1 |
| `StripDsp` | 1 | 0-3, below `NumDsps` |
| `StripSteps` | 0 | 2.7 bisect |
| `StripPhantomResync` | 1 | 5.6 |
| `MixerInitBatch` | 1 | Section 6 |
| `StripAllowRelocated` | 0 | 2.4; experiment only |

**IOCTLs** (bump `APOLLO_IOCTL_VERSION` to 3):

- `IOCTL_APOLLO_GET_STRIPS = APOLLO_CTL(0x0E, APOLLO_USER)`
- `IOCTL_APOLLO_STRIPS_REBUILD = APOLLO_CTL(0x0F, APOLLO_ADMIN)`

```c
typedef struct _APOLLO_STRIPS_INFO {
    ULONG Version;            /* 1 */
    ULONG State;              /* 0 Off, 1 Idle, 2 Armed, 3 Building, 4 Live, 5 Failed */
    ULONG Flags;              /* 1 Owned, 2 Quarantined, 4 Tainted, 8 ModulesVerified, 0x10 PhantomPending, 0x20 DryRun */
    ULONG Dsp, Mask, Steps, MixerInitBatch;
    ULONG BootWord[8];        /* window +0x1A4 per DSP, read before the ring connect */
    ULONG Geometry[11];       /* strip DSP: size 0-3, base 0-3, scratch 1-3 */
    ULONG ModuleState[5];     /* EB DA A5 C2 12B: 0 none, 1 uploading, 2 resident */
    ULONG StripState[8];      /* 0 none, 1 queued, 2 live */
    ULONG StripHeader[8], StripAckMs[8];
    ULONG LastReply[4];
    ULONG Passes, Created, Teardowns, Failures, Uploads, RepliesOk, RepliesBad, Timeouts;
    CHAR  Why[96];
} APOLLO_STRIPS_INFO;
```

**`APOLLO_STRIPS`** (in `DEVICE_CONTEXT`) holds:

- the registry values above;
- `State`, `Why`;
- `Owned`, `Quarantined`, `Tainted`, `PhantomPending`, `ReplyWaiting`;
- `BootWord[8]`, `Geom[11]`;
- `Epoch`, `Gen`, `ArmedAt`;
- `ModState[5]`, `StripState[8]`, `AckMs[8]`;
- `Fence` (a slot index);
- `Module[5]` and `ModuleLen[5]` (verified copies);
- `BusOwner[32]`;
- `Xact[16]`;
- the counters.

New file `driver/strips.c`; prototypes in `apollo.h`.

---

## 10. Implemented so far: the T3 build (Nov 14 2026)

### 10.1 What is in the build

The scope is T3: the five Mic-strip code modules are uploaded to DSP 1 and their replies are
checked. Nothing else is sent:

- no zero-fill, memspec, header or parameters;
- no SYNCH or `0x001E0004` input words;
- no bus ownership, route commands or 48V resync;
- no change to the mixer batch.

| File | Change |
|---|---|
| `driver/strips.c` (new) | The dry-run probes, folded in with the same log lines. Also the pinned module loader, the transaction engine (section 3), the upload pass, the D0-exit release, `GET_STRIPS` and the taint. |
| `include/apollo_strip_pins.h` (new) | The five pins: id, length, w4, pool words, destination, file name, SHA-256. Shared with apollo-diag; hashes and sizes only. |
| `driver/dsp.c` | `APOLLO_RING.Gen` is bumped by every resync. New `ApolloRingRefresh`, `ApolloRingFreeSlots`, `ApolloRingPending`, `ApolloRingWrite` (no wait) and `ApolloRingResyncNow`. `ApolloRingDoorbell` is now exported. |
| `driver/apollo.h` | `APOLLO_XACT`, `APOLLO_STRIPS` (in `DEVICE_CONTEXT` as `Strips`), `StripLock`, `StripWork`. The dry-run fields (`BootWord`, `StripDspOwned`, `StripLayoutWhy`) moved into `Strips`. |
| `driver/device.c` | Probes call `strips.c`; `ApolloStripsArm` runs after the transport auto-start. PrepareHardware calls `ApolloStripsPrepareHardware`: reads the registry and, only with `StripsEnable=1`, allocates 16 x 8 KiB common buffers below 4 GiB. ReleaseHardware frees them, except unproven (parked) ones while the device is present (10.4). D0 exit calls `ApolloStripsQuiesce` after `ApolloServiceStop` and before `ApolloHwQuiesce`. |
| `driver/driver.c` | Creates `StripLock` and the `StripWork` work item |
| `driver/mixer.c` | The service pass calls `ApolloStripsService` (queues the pass) |
| `driver/ioctl.c` | `IOCTL_APOLLO_GET_STRIPS`. A `WRITE_REG` outside 0x3800-0x38FF taints the strips. |
| `include/apollo_ioctl.h` | Interface version 3; `IOCTL_APOLLO_GET_STRIPS` = function 0x0E, user access; `APOLLO_STRIPS_INFO` |
| `driver/apollo.vcxproj` | `strips.c`, `$(DDK_LIB_PATH)cng.lib` |
| `tools/apollo-diag/apollo-diag.c`, `build.cmd` | `strips`, `strips verify [dir]`, `strips install [dir]`; links `bcrypt.lib advapi32.lib` |

**Build.**

- The EWDK build is clean at /W4 /WX, and all nine `*.nativecodeanalysis.xml` files are 59 bytes.
- `infverif /w` passes.
- apollo-diag builds at /W4 /WX.
- `apollo.sys` imports `BCryptOpenAlgorithmProvider`, `BCryptHash` and `BCryptCloseAlgorithmProvider`
  from `cng.sys`, linked through the EWDK's `km\x64\cng.lib`. (`ksecdd.lib` has no `BCryptHash`.)

`apollo-diag strips verify` (read only, run Nov 14) found all five pinned records in the UAD
install and in `apollo-win\ua-files`. 0x0A5 and 0x0C2 came from `UAD Input Strip Header.dll`;
those are the same bytes as in the Mic header.

**A cold start with `StripsEnable=1`:**

1. **PrepareHardware** logs the registry values.
2. **E3 to E11:** the boot words are read. DSP 1 is owned if `(word & ~0xFF00) == 3`, the start is
   cold and nothing is tainted. Each bring-up is a new epoch.
3. **After bring-up:** the pool registers are read and checked for 8 strips, the transport
   auto-starts, and the strips are Armed.
4. **500 ms later** the service pass queues `StripWork`. Without any lock it reads the five files
   and verifies them. Then it takes StripLock, and under DevLock:
   - checks the gates: Armed; modules verified; 48 kHz (`SampleRate == DspToldRate == 48000`,
     RATE_INFO lock 1 and index 2); owned; not quarantined or tainted; DSP 1 ring-connected and
     live; a fresh layout check returns code 0;
   - marks the pass Building.
5. **Five transactions, in turn.** Each is one reply descriptor `{0x80000004, 0, lo, hi}` on DSP 1's
   response ring, then one DMA_REF `{0x80000000 | (len/4 + 2), 0, lo, hi}` on its command ring.
   The payload is `{0x0001nnnn, dest, record}` with record bytes 10-11 cleared.
   - Each transaction is published only behind an idle command ring and an empty response ring.
   - It is waited for up to 1 s, polling with DevLock taken per poll.
   - The reply must read `{0x80070004, 0, id, x}`.
   - Anything unproven, and any other reply, quarantines DSP 1 until a power cycle.
6. **State Live:** "uploads only: 5 modules resident on DSP 1, no strip started".

**D0 exit, for any reason:**

1. The service thread stops.
2. `StripWork` is flushed.
3. One inline transaction `{0x00030002, id, 0, 0}` releases every module that was sent, whether
   resident, refused or unproven, and is waited for up to 1 s.
4. Only then do `ApolloHwQuiesce` and `ApolloRingsQuiesce` run.

If the device is gone (cable pulled), the modules are forgotten and nothing is sent.

### 10.2 Where the T3 build differs from the plan

| Plan | T3 build | Why |
|---|---|---|
| 9: `StripSteps` 0..6 bisect, default 0 | Read and reported, but the build always stops after the uploads. Default 1. | The strip transaction is the next step. |
| 9: `StripPhantomResync`, `MixerInitBatch`, `StripAllowRelocated`, `STRIPS_REBUILD` IOCTL | Not implemented | Out of T3's scope |
| 9: `APOLLO_STRIPS_INFO` with per-strip arrays | Per-module info instead: id, destination, file status (NTSTATUS), DSP state, reply words. Plus the boot words, the 11 pool registers, the layout code, counters, and the ring indexes of the last unproven transaction. | What T3 needs to read |
| 3.6/5.3: a teardown fence proven by the next pass | The D0-exit quiesce publishes the release and waits for it right away. Passes only follow a cold bring-up, so no fence is carried between passes. | Simpler, same guarantee |
| 3.6: parked slots reclaimed when proven | Parked slots are never reused, and ReleaseHardware frees them only if the device is gone | Every parked slot comes with a quarantine, so 16 slots are enough |
| 2.4: check with `Hi` = the highest input in the mask | Always checks room for 8 strips | Matches the dry run, which passed |
| 4.3: modules read on the first pass | Read once per device start, before StripLock is taken (no lock held during file I/O). A failure needs a new device start: power-cycle the x8p after `strips install`. | Simple and deterministic |
| 5.4 taint | Any `WRITE_REG` outside 0x3800-0x38FF taints. The modules are forgotten (no release is sent later) and the strips are Off until a power cycle. | Matches `oa:driver/ua_plugin_place.c:147-160` |
| `StripsEnable=2` dry run | Also loads and verifies the module files and waits for the 48 kHz gate before declaring success | Proves T1 and the rate gate without sending |
| State Live means strips run | In this build Live means "pass complete: modules resident" | No strips yet |
| 1.3: release only uploaded modules | Releases every module not `None`, including refused or unproven ones. This follows open-apollo's intent without its `res[0]` bug. | 1.3 |

### 10.3 T3 procedure (the user's x8p)

**Preconditions:**

- This build of `apollo.sys` is installed on the x8p (`tools\phase1\install.ps1`, admin).
- The new `apollo-diag.exe` is on hand.
- **Dynamic** mic or nothing on the inputs. No strip is started, so nothing becomes audible, but
  keep the monitors low anyway.

Commands below are relative to `C:\Users\aaron\Desktop\Avium\apollo-win\tools\apollo-diag\`; the
`reg` and `strips install` steps need an **admin** PowerShell.

1. **Install the modules (T1)**, once:

   ```
   .\apollo-diag.exe strips install
   ```

   Pass: five lines `0x0eb  716 bytes  pinned SHA-256 matches (...)` through 0x12b, then
   `0eb.bill ok` .. `12b.bill ok` and `Installed.` If UAD is not installed, pass the copies:
   `.\apollo-diag.exe strips install C:\Users\aaron\Desktop\Avium\apollo-win\ua-files`.
2. **Optional dry run with the new build (T2b):**

   ```
   reg add HKLM\SYSTEM\CurrentControlSet\Services\apollo\Parameters /v StripsEnable /t REG_DWORD /d 2 /f
   ```

   Power-cycle the x8p (off 10 s, on), wait about 10 s, then run `.\apollo-diag.exe strips`. Pass:
   - `State 1 Idle`;
   - `Why dry run complete: every gate passed, nothing sent`;
   - Flags include `OWNED MODULES_OK BOOT_READ LAYOUT_READ`;
   - every module `verified` and `-` on the DSP.

   The log shows `strips: dry run: every gate passed ... nothing sent`.
3. **T3:**

   ```
   reg add HKLM\SYSTEM\CurrentControlSet\Services\apollo\Parameters /v StripsEnable /t REG_DWORD /d 1 /f
   reg add HKLM\SYSTEM\CurrentControlSet\Services\apollo\Parameters /v StripSteps /t REG_DWORD /d 1 /f
   ```

   Power-cycle the x8p and wait about 10 s. Then run:

   ```
   .\apollo-diag.exe strips
   .\apollo-diag.exe log
   .\apollo-diag.exe info
   ```

   **Pass, `strips`:**
   - `State 4 Live (pass complete)`;
   - Flags `OWNED MODULES_OK BOOT_READ LAYOUT_READ POOL_READY`;
   - each module `verified` and `resident (reply 80070004 00000000 000000eb ...)`. The third
     word must be the plain module id; the fourth is not checked;
   - `uploads 5 (replies ok 5, bad 0), timeouts 0`.

   **Pass, `info`:** still Stage 5 Connected.

   **Pass, `log`**, in this order (other lines in between):

   ```
   strips: StripsEnable=1 StripSteps=1 StripMask=0x01 StripDsp=1 (uploads only in this build)
   cold start
   strips: boot words 00002203 00000003 00000003 00000003 00000003 00000003 (DSP 1 untouched since power-up)
   bring-up complete: connected
   strips: DSP 1 pools size 00004000 0000a9b9 00001fff 00ff516a base 000e0000 00092800 000c5000 08008e96 scratch 00000080 00000080 00008000
   strips: layout matches the recipe (code 0) for 8 strips; uploads may follow
   transport auto-start: 0x00000000
   strips: armed (uploads): the pass starts in 500 ms once the DSPs run at 48 kHz
   strips: module 0x0eb verified (716 bytes, w4 162, sha256 2b879e08..)
     ... 0x0da, 0x0a5, 0x0c2, 0x12b verified
   strips: uploading 5 Mic Input Strip modules to DSP 1 (StripSteps=1: uploads only)
   strips: DSP 1 accepted module 0x0eb at 0xe0000 (reply 80070004 00000000 000000eb xxxxxxxx)
   strips: DSP 1 accepted module 0x0da at 0xe006c (...)
   strips: DSP 1 accepted module 0x0a5 at 0xe025c (...)
   strips: DSP 1 accepted module 0x0c2 at 0xe038c (...)
   strips: DSP 1 accepted module 0x12b at 0xe03a2 (...)
   strips: 5 modules resident on DSP 1 (0xe0000..0xe03e1); stopping here, no strip is started in this build
   ```

4. **Nothing else changed:**
   - `.\apollo-diag.exe tone 3` with `.\apollo-diag.exe levels 3` in a second window: slots 26-27
     still carry the tone.
   - `levels`: slot 0 is still `zero`. Expected: no strip has been started yet.
5. **Release (optional).** The release lines are logged at D0 exit, but disabling the device unloads
   the driver and its in-memory log. To see them, either:
   - run Sysinternals DebugView as admin with Capture Kernel, then disable and enable the x8p in
     Device Manager. Expect `apollo: [...] strips: 5 module(s) released on DSP 1 (D0 exit)`. After
     the enable the start is warm: `strips: off: warm start: power-cycle the Apollo for input strips`,
     and nothing is sent; or
   - put the PC to sleep and wake it, then `apollo-diag log`.
6. **Afterwards:** `StripsEnable` may stay 1 (uploads at every power cycle, inaudible) or go back to 0.
   Report the `strips` output and the log lines.

**Failure signs and what to record:**

| Log line | Meaning | Next |
|---|---|---|
| `strips: module 0x... refused: file missing (apollo-diag strips install)` and `strips: off: strip module files missing ...` | Step 1 not done, or not elevated | Do step 1, power-cycle |
| `... refused: SHA-256 is not the pinned one` (or length or header) | The file is not UA's pinned record | Re-run `strips install`; report |
| `strips: waiting for 48 kHz (rate info 0x..., DSPs told N Hz, rate 48000 Hz)` and nothing after it | The rate gate never opened | `apollo-diag start`, then `strips`; report the rate info |
| `strips: DSP 1 refused module 0x0eb: reply 80070004 f0010005 ...` | The DSP answered but refused; DSP 1 quarantined | Report all four reply words, power-cycle. Do not retry without a change. |
| `strips: DSP 1 transaction (1 entries, 1 replies) ended 0xc00000b5 after 1000 ms: cmd end X pos Y, resp end Z pos W, reply word 0x...` | No reply within 1 s; quarantined | Report the line, power-cycle |
| `strips: off: warm start ...` or `strips: boot words ... (DSP 1 already used by a host)` | Not a real power cycle | Power the x8p off for 10 s |
| `strips: raw register write 0x....: strips off until the Apollo is power-cycled` | `apollo-diag write` was used | Power-cycle |
| The x8p drops off the bus or Windows hangs | See section 7 | Power-cycle the x8p (reboot if needed). Set `StripsEnable=0` before reconnecting. Report the last log lines. |
| `strips: module 0x... refused: file is open elsewhere (oplock or share) (0xc0000043)` | Something holds a `.bill` file open | Close it (or reboot), re-run `strips install`, power-cycle |
| `strips: N unproven transaction buffer(s) left to the device's deletion` | A transaction was never proven, and the device was disabled or reinstalled without a power cycle | Expected after a quarantine. The buffers are freed with the device. Power-cycle the Apollo before the next test. |

### 10.4 Review fixes (Nov 14 2026, after the T3 snapshot `builds\2026-11-14-t3-strip-uploads\`)

The live tree has these fixes; the snapshot does not.

| # | Finding | Fix |
|---|---|---|
| 1 | `ApolloStripsReleaseHardware` deleted every slot, parked ones included. A parked descriptor can still be fetched or written by the FPGA: D0 exit only resyncs the rings, nothing resets DSP DMA. Disabling or reinstalling before a power cycle would free it under live DMA (IOMMU fault, 0xE6 under Verifier DMA checks). | A Published or Parked slot is deleted only if the device is gone: `Stage == ApolloStageGone`, or one raw FPGA_REV read returns all-ones. Otherwise its handle and state are kept, the slot is never reused, and the common buffer goes with the DMA enabler when the device is deleted. Log: `strips: N unproven transaction buffer(s) left to the device's deletion: DSP 1 may still reference them`. (`oa:driver/ua_dsp_xact.c:803-806`) |
| 2 | The module folder granted Users read. A user could open a `.bill` file with share mode 0 or a batch oplock. The driver's `ZwCreateFile` would then wait, and the D0 exit's `WdfWorkItemFlush(StripWork)` would wait forever (0x9F, or a hung removal). | Folder `D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)`, each file `D:P(A;;FA;;;SY)(A;;FA;;;BA)`. The installer re-applies the file DACL to existing files, before and after writing them. The driver opens with `FILE_COMPLETE_IF_OPLOCKED` and treats `STATUS_OPLOCK_BREAK_IN_PROGRESS` (a success code) as a refusal: it closes and returns `STATUS_SHARING_VIOLATION` ("file is open elsewhere"). Checking the installed copies with `strips verify` now needs an elevated prompt; without one it says so instead of "missing". |
| 3 | A taint bumped `Gen` but not `Epoch`, and `XactSubmit` checks only `Epoch`. A `WRITE_REG` during a pass could still publish an upload (the 50 ms retry drops DevLock), or mark a module RESIDENT again after the taint forgot it. D0 exit would then release it on a tainted DSP. | `ApolloStripsTaint` also bumps `Epoch` (`oa:driver/ua_plugin_place.c:158`), so nothing built before it can publish, and an in-flight transaction polls as stale (`oa:driver/ua_dsp_xact.c:261-262, 437-438`). After the wait, `StripsUploadOne` changes ModState, counters, Why and quarantine only while `Gen` is still the pass's. Otherwise it logs `strips: module 0x...: outcome 0x... ignored, the strips were turned off meanwhile`. `XactRun` no longer quarantines when the epoch moved under it: the DSP is already disowned. |
| 4 | The reply check also accepted `id \| 0x02000000`, though the SDK form sends the plain id | Only `r[2] == id` (`oa:driver/ua_plugin_upload.c:84-93`). There is no recorded reason to accept the other form. |

**Build after the fixes** (`tools\build-all.cmd`, EWDK at D:):

- The driver, `infverif /w`, apollo-diag, the ASIO driver and test host, and dsp (45/45 checks) build clean.
- All eleven `driver\x64\Release\*.nativecodeanalysis.xml` files are 59 bytes, `strips` included.
- `loopback-test.exe` and `apollo-control.exe` could not link because both were running. Their
  sources were not touched.

### 10.5 The strip stages (T4 build, Nov 14 2026)

T3 passed on the real x8p (build 11.12.23.614): five uploads, replies `80070004 00000000 <id>`
with word 3 echoing the upload's word 0, about 30 ms in all, still Connected. The next build adds
the strip itself, one stage per `StripSteps` value. Every hardware run adds exactly one new kind
of command to what the previous stage proved.

| `StripSteps` | Adds | Words (input 1, DSP 1) |
|---|---|---|
| 1 (absent) | The five module uploads (T3, passed) | 2.5 |
| 2 | 10 zero-fills of the strip's private blocks | `{0x00080004, addr, 0, dwords}`, addr 0x9D1B0 .. 0x08FFBFF0 (2.6 row 1) |
| 3 | + the memspec (1 DMA_REF, 19 dwords) | `00150013 0009d1b6 0009d178 ...` (2.6) |
| 4 | + the instance header (1 DMA_REF, 9 dwords) | `{0x00140009, 0x9D1B0, 0x24, 0x40000000, 1, 1, 0x22, 1, 0x846}` |
| 5 | + the captured 48 kHz parameters (1 DMA_REF, 55 dwords, three writes) | 2.7 |
| 6 | + SYNCH: **the strip starts** | `{0x001F0004, 0x00200000, 0x9D1B0, 0}` |
| 7 | + span (twice) and route on the strip DSP | `{0x001E0004, 0, 8, 0x20}` x2, `{0x001E0004, 0, 7, 0x0001FFFF}` |
| 8 | + bus owner (table only: this driver caches no coefficients, so nothing is re-sent) and the route word to every other DSP | `{0x001E0004, 0, 7, 0x0001FFFF}` to DSPs 0, 2, 3, 4, 5 |
| 9, or 0 | + the 48V resync (mailbox, not the rings). It acts only on preamps in `StripMask` that report 48V on. | S0 bit 3: 0, then after 200 ms 1 |

Stages 2-7 are one transaction per input on the strip DSP, waited for by command-ring
consumption (no reply). Stage 7 is open-apollo's verified full strip on the strip DSP; stage 8
adds its route words; stage 9 adds its 48V resync.

**Teardown at D0 exit.** One transaction on DSP 1:
1. `{0x001F0004, 0x00200000 | i, 0, 0}` for every input whose SYNCH went out. Stages 2-5 never
   send a SYNCH, so they never send a stop either.
2. `{0x00030002, id, 0, 0}` for each module.

Bus owners go back to DSP 0.

**Implementation:**

- `driver\strip_plan.h` (new) is the planner: addresses (slot = input), memspec, header,
  parameters and start words, as a plain list. It has no OS calls. `strips.c` copies the plan into
  a transaction.
- `tools\strip-plan-test\` (new, `build.cmd` builds and runs it) checks the planner's words
  against the 2.6 and 2.7 values, which were computed independently: 182 checks pass. A script
  also compared the parameter arrays and rows with open-apollo's `ua_strips.c`: identical.
  `strip-plan-test.exe --print` prints input 1's whole transaction.
- `MixerInitBatch` (section 6) is a registry value read in `strips.c`. `ApolloMixerInit`
  (`mixer.c`) calls `ApolloStripsMixerInitBatch` instead of its own loop:
  - 0 = none;
  - 1 = x8p-safe: skips S2, S3, S12-S14, S16-S31 (10.7);
  - 2 = legacy.
- `GET_STRIPS` is version 2 and adds: stage asked and done; mixer batch; 48V result and mask;
  route words sent; and per input the state (none, queued, taken), SYNCH sent, header, ms to taken
  and bus owner. `apollo-diag strips` prints them and still reads a version-1 driver.

**Defaults when a value is absent:**

| Value | Absent means |
|---|---|
| `StripsEnable` | 0 |
| `StripSteps` | 1 |
| `StripMask` | 0x01 |
| `StripDsp` | 1 |
| `MixerInitBatch` | **1 (x8p-safe)**, since release5; was 2 (legacy) |

Until release4 the absent `MixerInitBatch` was legacy (2), so these defaults sent exactly what the T3
build sent. Since release5 (Nov 14 2026, section 10.8) the absent default is x8p-safe (1): the
legacy batch's non-preamp settings with the same values, preamp fields skipped, so the unit keeps
its own gains, polarity and 48V. Set 2 explicitly to reproduce the earlier builds.

**Build** (a private copy of `driver` and `include`, because another agent was building in the
live tree):

- EWDK: all 14 `*.nativecodeanalysis.xml` files are 59 bytes.
- `infverif /w` passes.
- apollo-diag and strip-plan-test build at /W4 /WX.
- MSB8029 appeared only because the copy sat under %TEMP%.

### 10.6 T4 procedure: the stage ladder (the user's x8p)

**Preconditions:**

- The T4 build is installed (`tools\phase1\install.ps1`, admin), and its apollo-diag is on hand.
- The modules are installed (T3 step 1).
- A **dynamic** mic is on input 1.
- The x8p's monitors are not connected; Aaron monitors through the Twin X. A started strip puts
  input 1 into the x8p's monitor mix, but nothing on the x8p is audible. Verification is by
  `apollo-diag levels` (record slot 0 = input 1) and the x8p's front-panel input meter.

**Once (admin):**

```
reg add HKLM\SYSTEM\CurrentControlSet\Services\apollo\Parameters /v StripsEnable /t REG_DWORD /d 1 /f
reg add HKLM\SYSTEM\CurrentControlSet\Services\apollo\Parameters /v StripMask /t REG_DWORD /d 1 /f
reg add HKLM\SYSTEM\CurrentControlSet\Services\apollo\Parameters /v MixerInitBatch /t REG_DWORD /d 2 /f
```

**Each stage N = 2, 3, 4, 5, 6, 7, 8, 9, in order, one power cycle each:**

1. `reg add HKLM\SYSTEM\CurrentControlSet\Services\apollo\Parameters /v StripSteps /t REG_DWORD /d N /f`
2. Power-cycle the x8p (off 10 s, on). Wait about 10 s. The transport auto-starts, and the pass
   runs 500 ms after it.
3. Run `.\apollo-diag.exe strips`, then `.\apollo-diag.exe info`, then `.\apollo-diag.exe gain 1 40`,
   then `.\apollo-diag.exe levels 10` while speaking into input 1 and then staying silent.
   Then run `.\apollo-diag.exe log`.

**What each stage must show:**

| N | `strips` | `log` (after the five "accepted module" lines) | `levels` slot 0 |
|---|---|---|---|
| 2-5 | `Stages asked N ..., done N`; input 1 `taken`, SYNCH `-`, header `0x9d1b0`; `info` still Connected | `strips: input 1: strip transaction, E entries (stage N: ...), header 0x9d1b0, DSP 1` (E = 10, 11, 12, 13), then `strips: input 1: Mic Input Strip prepared, not started at 0x9d1b0 on DSP 1 (taken after X ms)`, then `strips: stage N (...) done on StripMask 0x01, DSP 1` | still `zero` (expected: nothing runs yet) |
| 6 | as above, SYNCH `sent` | `... 14 entries ...`, `Mic Input Strip started at 0x9d1b0 ...` | may already show signal |
| 7 | as 6 | `... 17 entries ...` | **pass criterion below** |
| 8 | `bus on DSP` 1 for input 1; `Routes 5 route word(s)` | `strips: input 1: bus 0 now mixed on DSP 1 (...); route 0x1ffff sent to 5 other DSP(s)` | pass criterion |
| 9 | `48V resync: nothing to do: no preamp in StripMask has 48V on` (dynamic mic, 48V off), or `done (preamp mask 0x01)` with 48V on, or `skipped: no hardware readback` (no two matching valid readbacks: nothing was written, 10.7) | `strips: 48V resync: no preamp ...` or `strips: 48V re-applied after the cold start (preamp mask 0x01)` | pass criterion |

**Pass criterion (T4):** record slot 0 shows signal when Aaron speaks into input 1.

- `levels` prints a slot-0 peak around -60..-10 dBFS that rises and falls with the voice, and
  falls back toward the noise floor (a dB value, not `zero`) in silence.
- Slots 1-7 stay `zero`.
- The x8p's front-panel meter for input 1 moves.
- `strips` shows no QUARANTINED flag, and `info` is still Connected.

Expected first at stage 6 or 7. Run 8 and 9 anyway: 8 is open-apollo's verified configuration, 9
matters only with 48V.

**When a stage fails:**

| Symptom | Meaning | Next |
|---|---|---|
| The x8p (and the Twin X behind it) drop off the bus; `apollo-diag` finds no device; Windows hangs | That stage's new command kind is what the DSP did not take. The A1 parameters did this on the x4, about 10 ms after SYNCH. | Power-cycle (reboot if Windows hangs). Set `StripsEnable=0` before reconnecting. Report N and the last `log` lines. Do not retry the same N without a change. |
| `strips: DSP 1 transaction (E entries, 0 replies) ended 0xc00000b5 after 1000 ms: cmd end X pos Y ...`, then `quarantined` | DSP 1 stopped consuming at that stage | Power-cycle. Report the line and N. Run N-1 once more to confirm it still passes. |
| `strips: off: ...` or `waiting for 48 kHz` | A gate, not the strip | See the 10.3 failure table |
| Stage 7 or 8 passes but slot 0 stays `zero` | The strip runs but no signal reaches capture | Check `readback`: input 1 Mic/Line (word 0 bit 0 = 0), PAD off, GainC about 31 at 40 dB. Try `line 1 0`, `pad 1 0`, `gain 1 50`. Then try T5's `MixerInitBatch=1`. Then, as a diagnostic, `StripDsp=0` at stage 8: on the x4, DSP 0 strips fed capture but not the monitor mix. Report `strips`, `readback` and `levels`. |
| Signal in slot 0 but also in another slot | Unexpected | Report `levels` for all slots |

**T5, the mixer batch A/B** (at the stage that passed T4, usually 8):

1. Set `MixerInitBatch=1`, power-cycle, then `gain 1 40`, `levels` while speaking,
   `tone 3 --slots 0,1` and `levels` (slots 26-27).
2. Repeat with `MixerInitBatch=0`.
3. If both pass, make 0 the default; if only 1 passes, keep 1. The log shows
   `mixer init batch: ...`.
4. Without the legacy batch, preamps 1 and 5 keep the unit's own gain and polarity: no more 41 dB,
   no more inverted input 5.

**T6:** `StripMask=0xFF` at the passing stage. Expect 8 strips at headers 0x9d1b0 .. 0x9cf64, and
input N in slot N-1 only (2.6).

**Afterwards:** leave `StripsEnable=1` with `StripSteps` at the stage that passed (or 0 for
everything). Report the `strips` output and the log of each stage.

### 10.7 Review fixes to the stage build (Nov 14 2026, after combined build 12.36.20.8)

The review found stages 2-8 word-for-word correct against 2.7 and `ua_strips.c`. The fixes below
are in the live tree; the frozen `builds\2026-11-14-combined\src` copy does not have them.

**1. Stage 9 could turn 48V on where it was off.** `ReadbackValid` is set by any snapshot with
RB_STATUS = 1. A garbage snapshot could read "48V on", and the resync would then write off and on
to a preamp that had 48V off, which can damage a ribbon mic.

Fix: the resync reads the 48V bits only from a snapshot whose words 38 and 39 and the flag word
in use (0 for preamps 1-4, 5 for 5-8) are not 0xFFFFFFFF. UA's Mixer Engine
(`oa:mixer-engine/hardware.py:5265-5269`) and open-apollo (`oa:driver/ua_core.c:2716-2718`) apply
the same rule. It also waits up to 500 ms for a second, newer snapshot that passes the same check
and reports the same 48V bits. Otherwise it writes nothing and logs `strips: 48V resync skipped: no
two matching valid readbacks ...`; `strips` then shows `skipped: no hardware readback`.

**2. 48V could be left off across a D0 exit.**

Fix:
- The resync is skipped when a D0 exit is under way: `StopRequested` is checked before it starts,
  before the off, and during the readback wait.
- Once the off is written, the on is always written, and the driver keeps flushing the mailbox
  itself for up to 2 s until the on is taken.
- If the on still isn't taken, the log says
  `strips: 48V ON for preamp mask 0x.. was not taken by the mailbox within 2000 ms ...` and the on
  stays queued.
- The D0 exit flushes the strip work item before the hardware quiesce, so a running resync
  finishes first. Worst case is about 3.2 s.

**3. A host 48V change could be overwritten.**

Fix:
- `ApolloMixerWriteSetting` (`mixer.c`) calls `ApolloStripsNoteSettingWrite`. While a resync holds
  48V off, any host write to that preamp's 48V bit removes the preamp from the set the resync
  will switch back on: `strips: 48V of preamp N set by the host during the resync: left as set`.
  Host writes include SET_INPUT_PARAM 48V and a raw S0/S12 setting.
- The resync's own writes go straight into the mailbox cache, so they don't count as host writes.
- A preamp whose 48V bit still has a host write queued in the mailbox when the off would go out
  is also left alone. That write is newer than any readback: `strips: 48V of preamp N has a host
  write queued: left as set`.
- `PhantomMask` in `strips` reports the preamps actually switched back on.

**4. `MixerInitBatch=1` still wrote preamp fields.**

Fix: the x8p-safe batch now also skips the byte lanes S18-S22 and S26-S30
(`oa:driver/ua_core.c:2610-2616`) and S31, the knob steps of preamps 5-8 (`:2617-2622`). It now
writes S1, S4-S11, S15, S32-S34, S36, S37 at 0x20/0xFF and S35 at 0x05/0x1F. The absent default
was then still 2 (legacy); release5 made it 1 (see the defaults table above).

**5. Hardening.**

- The layout check's tops (0x9D1B8, 0xC6FFE, 0x08FFE000) and per-strip pool needs (0x54, 0x10,
  0x2010) are now named constants in `strips.c`.
- `C_ASSERT`s tie them to the planner's macros in `strip_plan.h` (`STRIP_TOP1-3`,
  `STRIP_POOLn_PER_STRIP`, which are built from the row sizes `STRIP_DW_*`).
- `strip-plan-test` also checks at run time that the rows add up per pool. It now runs 185
  checks, all passing.

**Build:**

- The private copy of the live `driver` and `include` was built with the EWDK.
- The `strips` and `mixer` Code Analysis results are 59 bytes (clean), and `infverif /w` passes.
- The one finding in that copy was the ACX agent's in-progress `stream.c(237)` C6011, which
  belongs to that agent.
- apollo-diag builds at /W4 /WX, and strip-plan-test reports 185/185.
- Nothing was built in the live tree.
- Stage 1, and an absent `StripsEnable`, still sent exactly what the T3 build sent (true until
  release5, which changed the absent `MixerInitBatch` to 1).

### 10.8 Eight strips: five inputs at exact zero, and the fix (Nov 14 2026)

**Observed (release4, 15.5.41.908, `StripSteps=9`, `MixerInitBatch=2`).** `StripMask=0x01`: input 1
records. `StripMask=0xFF`: all eight strips taken at the 2.6 headers, but slots 0, 1, 4, 5, 6 read
exact digital zero for minutes (gain and 48V changes do nothing); slots 2, 3, 7 are live.

**Cause.** The pass started each strip in the same transaction as its memory, input after input
(`StripPlanBuild` put SYNCH, span and route after the parameters; port of
`oa:driver/ua_strips.c:583-601`, `:610-705`, run one by one at `:1689-1754`). So strips 1-7 were
running on DSP 1 while the next strips' zero-fills (0x2010 words each), memspec and parameter
writes went to the same DSP, and every strip's DMA payload went through the same transaction
buffer, re-zeroed and rewritten the moment POSITION passed the previous strip (POSITION proves
ring consumption, not execution). Input 8, the only strip after whose SYNCH nothing more was
built, is live; with one strip (Test A) nothing is built after the SYNCH either. Neither is an
address problem: the 80 blocks are disjoint and inside the measured pools, and no write lands
outside its own strip (strip-plan-test). The route word is not per input either: 0x1FFFF is UA's
"no direct output, monitor mix on" default for every input (`oa:mixer-engine/model_features.py:218-235`).
Bus coefficients only move the monitor mix, never a record slot. Why 3 and 4 survived and 1, 2, 5, 6, 7
did not is not determined by the evidence: it looks timing-dependent.

UA builds several strips the other way round: every strip's private blocks and parameters first,
then one "enable" transaction of SYNCH + span/route per strip (the captured Console bring-up,
`oa:tools/uausb/soloinit.py:569-620`). Upstream's own x4 run of the interleaved order had input 2 at
digital zero "whatever is toggled" (`oa:HARDWARE-SESSIONS.md:336-337`, `:392-393`); after every input
had been re-SYNCHed with all strips present (insert DESTROY restores, `oa:driver/ua_strips.c:1101-1132`),
all four inputs carried their carriers (`oa:HARDWARE-SESSIONS.md:406-425`).

**Fix.** A pass is now `strip_plan.h StripPassStep`'s list, which `strips.c` walks:

1. per preamp in `StripMask`, input 1 first: one PREPARE transaction, stages 2-5 only (13 entries:
   10 zero-fills, memspec, header, parameters), waited for. Its buffer is held, never reused,
   until step 2 is taken;
2. one START transaction for all of them: per input `{0x001F0004, 0x00200000|i, hdr_i, 0}` and, from
   stage 7, span, span, route (4 entries per input, 32 for 0xFF), waited for;
3. per input, bus owner and the route word to the other five DSPs (stage 8), then the 48V resync.

No strip's memory is written once any strip runs. For `StripMask=0x01` the DSP gets the same 17
entries in the same order as Test A (13 + 4). No new command kind. `tools\strip-plan-test` checks
the order, the words, the per-strip write ranges and link words, and runs the old order through the
same checks (it fails them for 0xFF and passes for 0x01).

**Next cold start (`StripMask=0xFF`, `StripSteps=9`), log:**

```
strips: input 1: prepare transaction, 13 entries (stage 9: + 48V resync (everything)), header 0x9d1b0, DSP 1
strips: input 1: Mic Input Strip prepared at 0x9d1b0 on DSP 1 (taken after N ms); starts with the others
  ... inputs 2-8 at 0x9d15c 0x9d108 0x9d0b4 0x9d060 0x9d00c 0x9cfb8 0x9cf64
strips: start transaction, 32 entries for 8 strip(s) (stage 9: + 48V resync (everything)), DSP 1
strips: 8 strip(s) started together on DSP 1 (start taken after N ms)
strips: input 1: Mic Input Strip started at 0x9d1b0 on DSP 1
strips: input 1: bus 0 now mixed on DSP 1 (no cached coefficients to re-send); route 0x1ffff sent to 5 other DSP(s)
  ... the same two lines for inputs 2-8
strips: stage 8 (+ bus owner and routes on the other DSPs) done on StripMask 0xff, DSP 1
strips: 48V resync: ...   (as before)
```

`apollo-diag strips`: state Live, stages asked 9 done 9, every input `taken`, SYNCH `sent`, the eight
headers, bus on DSP 1, 40 route words. `apollo-diag levels`: slots 0-7 all show a dB value (open
inputs about -100 dBFS, never `zero`), each follows only its own input; slots 26-27 carry MON.
If a slot is still `zero`, report `strips`, `log` and `levels`, then try `StripMask=0x03` and
`0x0F` (one power cycle each) to see whether the count of strips or particular inputs matter.

### 10.9 Still five zero inputs after 10.8: the live probe and repair ladder (Nov 14 2026, work-placement)

**Hardware matrix** (alive = noise or signal, dead = exact zero):

| Test | Mask | Batch | 48V at boot | Order | Result |
|---|---|---|---|---|---|
| A | 0x01 | 2 | off | old | input 1 zero, then alive after host `48v 1 1`, `48v 2 1`, `gain 1 40` |
| B | 0xFF | 2 | on 1-2 | old | alive 3, 4, 8; dead 1, 2, 5, 6, 7 |
| C | 0xFF | 1 | on 1-4 | 10.8 | identical to B |
| D | 0x03 | 1 | on 1-2 | 10.8 | dead 1, 2; host 48V off/on and gain writes change nothing |

**What it rules out.**
- DSP capacity: D runs two strips and both are dead.
- Strip count or memory slot: input 1 at slot 0 is dead in B, C and D. Alive slots 2, 3, 7 are not a
  prefix in any order the DSP could use (header address, start order, a prepend list).
- Build order and transaction buffers: B and C give the same set.
- The mixer batch: batch 1 and batch 2 give the same set.
- 48V: 3 and 4 live with 48V on, 5-7 dead with it off, and 7 and 8 have identical settings.
- Host writes: in D, mailbox writes of 48V off then on, and of the gain triplet, revive nothing.

**What is left.** All eight strips get the same words (memory, header, parameters) except the
SYNCH input and the slot. So whatever differs is per-input state on the DSPs that this driver never
sets and UA's driver does. The per-unit pattern is stable across power cycles, which fits
firmware defaults or power-up memory. UA does two things here that this driver does not:

1. **Stop before start.** At Console start UA stops every strip input, even on a fresh DSP, before
   building: SYNCH with address 0, then span, span, route (`oa:tools/uausb/soloinit.py:186-190`,
   `:571-576`). On the x4, input 2 read zero "whatever is toggled". It carried its carrier once its
   strip had been stopped and started again (`oa:HARDWARE-SESSIONS.md:336-337`, `:406-425`; insert
   start `oa:driver/ua_plugin_instance.c:706-726`; restore `oa:driver/ua_strips.c:1101-1132`).
2. **Span on every input.** UA's first words after the connect set span none for every input
   (`oa:tools/usb-dsp-init.py:202-227`). This driver sent span only to DSP 1, only for strip inputs,
   and only after their SYNCH. DSPs 0 and 2-5 never got a span word.

Test A's revival is not explained by either. It is the one run on the oldest build.

**The probe (driver).** After the pass and the 48V resync, `strips.c StripsProbe` measures every
strip input's record slot. It reads the capture ring 8 times, 25 ms apart (~8k frames, through
`ApolloLevelsRead`), and an input whose slot never leaves zero is dead. It also logs the DSP's
input meters, readback words 8-11.

Then, while a strip input is dead, it walks `strip_plan.h g_StripProbe`, measuring after every
repair:

| Step | What | Settle before the measurement |
|---|---|---|
| M1 | as built | 5 s (the 48V switch mutes an input for a while) |
| M2 | as built, again | 2 s: does time alone change it? |
| R-a | **stop + start**: a STOP transaction on the strip DSP (per input: SYNCH address 0, span, span, route), then the START transaction again | |
| M3 | | 1.5 s |
| R-b | **span none for inputs 1-8 on every live DSP**: a transaction on the strip DSP, fire and forget on the others | |
| M4 | | 1 s |
| R-c | stop + start again (every span now set before the SYNCH) | |
| M5 | | 1.5 s |

- The ladder stops at the first measurement with every input live.
- Every repair word is a kind the pass already sends: SYNCH, and input words sub 7 and sub 8. The
  one exception is sub 8 (span) to DSPs 0 and 2-5, which only ever got sub 7 before.
- No memory is written, and no DMA is used.
- An unproven transaction quarantines DSP 1, as everywhere else.
- Every sleep is in 50 ms slices and gives up on a D0 exit or a new generation.

**Registry `StripProbe`** (absent = 2):
- 0 = off (release5 behaviour);
- 1 = measure once and log;
- 2 = measure and repair.

Repairs run only after the full chain (`StripSteps` 0 or 9). Otherwise the probe only measures.

Tests: `tools\strip-plan-test` checks the ladder order, the STOP/SPANS words, a re-SYNCH that
writes no memory and stops and starts each input exactly once, and the dead mask. 670 checks pass.
`strip-plan-test --print-probe 0xff` prints the repair words.

**Next cold start (`StripMask=0xFF`, `StripSteps=9`, `MixerInitBatch=1`, `StripProbe` absent),
log after the 10.8 lines:**

```
strips: 48V re-applied after the cold start (preamp mask 0x0f)
strips: probe: measuring StripMask 0xff in 5000 ms (StripProbe=2: repairs while an input reads zero)
strips: probe 1 (as built): StripMask 0xff live 0x8c, exact zero 0x73
strips: probe 1: slot peaks 000000 000000 00xxxx 00xxxx 000000 000000 000000 00xxxx; meters ...
strips: probe 2 (as built, later): ...
strips: probe: stop transaction, 32 entries, StripMask 0xff, DSP 1
strips: probe: stop transaction taken after N ms
strips: probe: start transaction, 32 entries, StripMask 0xff, DSP 1
strips: probe: start transaction taken after N ms
strips: probe 3 (after stop + start): ...
   [then span (8 entries) + "span none for inputs 1-8 sent to 40 other DSP word(s)", probe 4,
    stop + start, probe 5, as long as an input is still dead]
strips: probe: every strip input live (<after what>); at the first measurement 0x73 read exact zero
```

The verdict also goes into the `Why` line of `apollo-diag strips`.

**Reading the result:**

| First all-live line says | Cause | Permanent fix |
|---|---|---|
| `as built, later` | time: an input mute that lifts on its own | none (wait longer) |
| `after stop + start` | the first SYNCH on a cold input does not take | in the pass, send UA's STOP words before the START |
| `after span none on every DSP` | span state on other DSPs, live | send spans to every DSP at bring-up |
| `after spans on every DSP, then stop + start` | span state must be set before the SYNCH | both, spans first (UA's order) |
| `inputs 0x.. still read exact zero` | neither | then run `StripDsp=2` (one power cycle): see whether the dead set follows the DSP or the input |
| `inputs 0x..` = exactly the inputs with nothing plugged in | the unit mutes empty inputs (jack sense, S7), not a strip fault | upstream's x4 "input 2 at digital zero" was "with nothing plugged in" and came back with a source (`oa:HARDWARE-SESSIONS.md:336-337, 392-393, 406-425`); record what is plugged into each input (and 48V) for every run |

Review fixes after the first build of release6 (Nov 14): the stop + start repairs only the dead inputs
(`dead & Mask`), so live inputs keep running and stay controls; a stop whose start is not taken is
logged and shown in the Why line ("stopped and not restarted"); the probe also ends when the unit
is no longer Connected or its stage is Gone.

**Not run on hardware.**

---

## 12. Warm starts: adopting this driver's own strips (Nov 15 2026, work-warm)

After a driver update, a PC restart or a device disable/enable the Apollo stays powered, so the
driver comes up warm. Until now every D0 exit tore the strips down (2.9) and every warm start
refused to build them (D9, 5.5): the mics stayed dead until the Apollo was power-cycled. Code:
`driver/strip_adopt.h` (the rules, plain C), `driver/strip_record.h` (the record), `driver/strips.c`
(`[Warm]` sections), `device.c` (D0 exit), `plugin.c` (restore at exit, record hooks, ownership).

### 12.1 What proves "ours"

Nothing is written to the DSP for it, so nothing can collide with what UA's software or the firmware
keep there. The proof is a record in the registry plus what the hardware shows at the next start:

- **The record**, `Parameters\Units\<unit key>\StripRecord` (REG_BINARY, 200 bytes, FNV-1a check
  word): the unit (HW_UID, device type, FPGA and firmware release, DSP count); every DSP's boot word
  right after this driver's last ring connect; `Anchored`; the strips' state (running / none /
  unknown), `Keep`, and the exact plan: strip DSP, StripMask, rate, stage, a hash of the five module
  pins, the 11 pool registers, every header, and an FNV-1a hash of every word of every transaction
  the pass sends (`StripPlanHash`; the x8p's plan, 0xFF at stage 9 and 48 kHz on DSP 1 of 6, is
  0x209A8BDB, pinned in strip-plan-test); and how many plug-in instances sit on the strip DSP.
- **The boot words.** A host's ring connect changes them (low byte 0x0B, a connect count in bits
  20:19): the x8p reads `00002203 00000003 x5` after a power-up and `0008220b 0008000b x5` after our
  one cold connect; UA's driver left `0010220b`. The x8p's warm start never connects, so equal words
  at a later start mean nobody connected in between. **Only the count and the model byte are compared**
  (`STRIP_BOOT_SESSION_MASK`): the low byte is the session state and our own D0 exit (transport stop,
  ring resync) turns 0x0B into 0x07 (first hardware run, Nov 15 2026: record `0008220b`, next start
  `00082207`, refused as "another host" until this was fixed).
- **Anchored** = every host session since the unit's last power-up was this driver's. Set at a cold
  start whose boot words all read 3 (no host since power-up). Kept while each later start finds the
  words exactly as the record says and, when it connects itself (the Twin X), sees its own connect
  move every word (a counter that stopped counting ends the proof: `NO_ADVANCE`). Lost until the
  next power cycle on any mismatch, raw register write (taint), quarantine (strips or plug-ins), or
  unproven teardown, and written to the registry at that moment.

When the record is written: at the end of every bring-up (only when something changed, so an x8p
warm start that finds its record writes nothing), when a pass starts (state unknown), when strips go
Live (running, with the plan), at an adoption, around every teardown (unknown, then none once
proven), when a plug-in is placed on the strip DSP (before anything is sent) and when one is taken
off after a proven teardown. Never only at shutdown: Windows may not keep registry writes made that
late, so every fact the next start needs is written while the session runs.

### 12.2 What happens at each start

| Case | What the driver does | Log / `apollo-diag strips` |
|---|---|---|
| Cold start (power cycle) | As before: boot word 3, build, 48V resync. Record: anchored, then running + plan. | `Warm start -`, `Anchor 0` |
| x8p warm, the record proves our strips run as planned | **Adopted.** Nothing is sent. Modules resident, every strip of the mask taken and started at its header, buses on the strip DSP (cached mix/cue sends follow), no 48V resync (no power-up happened). Plug-ins allowed (`StripWarmPlugins`, 12.5). | `strips: adopted N strip(s) this driver left running ... nothing sent`; State Live, `ADOPTED PLUGINS_OK` |
| ... and the record saw a plug-in on the strip DSP | **Adopted, restored**: the probe's stop + start (UA's own inline words) on each adopted input puts the plain strip back whatever ran there. Plug-ins after a power cycle (12.5). | `plain strips restored` |
| x8p warm, no record (strips built by an older build) | Off: "unsigned strips (no record): power-cycle once; later restarts keep them". | `Anchor 2` |
| x8p warm after UA's driver (or any other host) used the unit | Off, and the anchor is gone until a power cycle. | `Anchor 6`, then `5` |
| x8p warm, anchored, but the plan changed (StripMask, StripSteps, rate, StripDsp, module pins) | Off ("not this plan") unless `StripWarmRebuild=1`: then the recorded strips are stopped and released, and the pass builds the new plan. | `PLAN_DIFFERS` / `REBUILD` |
| x8p warm, the pools moved | Off, never rebuilt. | `POOLS_DIFFER` |
| x8p warm, the last exit tore them down, or ended mid-pass | Off unless `StripWarmRebuild=1` (rebuild, with a teardown first when something may run). | `TORN_DOWN`, `UNKNOWN_STATE`, `NOT_RUNNING` |
| Twin X warm (it ring-connects at every start, which drops strips) | Nothing can be adopted. Anchored: **rebuilt** after its own connect, the same pass as a cold start minus the 48V resync (`StripWarmRebuild`, default 1 on the Twin X). Not anchored: off, power-cycle the Twin X. | `REBUILT_WARM` |

A warm start decides in the first pass, after the same 500 ms settle and rate gate a build waits for,
and after a valid hardware readback (words 38/39 not all ones, up to 10 s): the anchor needs the boot
words after any connect, the plan needs the settled rate. `StripsEnable=2` logs the decision and sends
nothing.

### 12.3 The D0 exit

A D0 exit keeps the strips running when: StripsEnable 1, `StripAdopt` 1, the model's warm start does
not connect (the x8p), the strips are Live and started, nothing quarantined or tainted, and the record
names them as running, kept and anchored. Then plug-in chains come off with their plain strips SYNCHed
again (`ApolloPluginQuiesce(..., RestoreStrips)`, as a DESTROY does), the decision is checked again,
and nothing is sent for the strips; the ring quiesce and its date-only disconnect follow as before
(upstream inherits strips across exactly that disconnect: "also after a driver reload",
`oa:CHANGELOG.md:221`). Otherwise the teardown of 2.9, as every build did. A surprise removal changes
nothing in the record: a unit that stayed powered still runs its strips and adopts them when it comes
back.

### 12.4 Registry values (per unit, apollo_unit.h)

| Value | x8p | Twin X | Meaning |
|---|---|---|---|
| `StripAdopt` | 1 | 1 | 1: a D0 exit leaves Live strips running and a warm start adopts its own. 0: the old rule (teardown at every exit, power cycle). Ignored for keeping on the Twin X (its connect drops them). |
| `StripWarmRebuild` | 0 | 1 | 1: a warm start that proves only this driver used the DSPs since power-up builds the strips again. On the x8p that means uploads (with replies) to a DSP this driver never connected in this power session: off by default (the x8p adopts instead). It also gates a rebuild after a rate change on an adopted x8p. Since Nov 15 (warmplug) it no longer gates plug-ins (12.5). |
| `StripWarmPlugins` | 1 | 1 | (Nov 15 2026, warmplug) 1: the strip DSP of strips a warm start adopted (x8p) or rebuilt (Twin X), from an anchored record that saw no plug-in left there, takes plug-ins as after a cold start (12.5). 0: plug-ins wait for a power cycle after every warm start (the rule before). Other models 0. |

### 12.5 Plug-ins after a warm start

Plug-in hosting needs DSPs this driver owns. At a warm start that is only the strip DSP, and only when
its strips were adopted (not restored) or rebuilt from a record that saw no plug-in left there.

**What blocked it until Nov 15 (warmplug), with the evidence.** Nothing on the hardware had refused
anything: no build had ever sent an upload to a warm x8p. The refusal was our own gate: `PlugAdoptOk`
needed `StripWarmRebuild=1` (x8p default 0), so `PlugDspOwned` said "not ours" at every x8p warm start, and
Apollo Control refused every warm start on its own (`Warm()`), even the Twin X's rebuilt strips the
driver did own. What the hardware shows about the warm, unconnected session:

- The x8p's warm start programs every ring itself (F8 `ApolloRingsInit`: page addresses, resync, the
  per-DSP enable and strobe pulse, as at E4) and sends the rate ack to every DSP; it never ring-connects.
  The boot words read `00082207 00080007 x5` all session (live read Nov 15, after the warm start and the
  licence queries): session byte 0x07 (a host connected and left), connect count 1, unchanged.
- In that state DSP 0 answers the licence query: an inline command pair plus a 770-word reply descriptor
  on DSP 0's response ring, answered with the full table (log `[38786.320] [x8p] plugins: board table ...
  109 authorized`). So the command ring, the response ring and the DSP firmware's reply path all work in
  session 0x07 without a connect. DSP 1 runs the same firmware family and gets the same ring programming;
  its own first reply is the one thing not yet seen on hardware.
- A ring connect at a warm start cannot keep the adopted strips: a connect makes the DSPs drop the strips
  (`oa:CHANGELOG.md` "kext_connect ... makes the DSPs drop the Console input strips"; the Twin X's warm
  connect does, hence its rebuild). Connect + rebuild on the x8p would trade the adoption for a dropout
  at every start, on a path (warm x8p connect) no build has run: not taken.
- Upstream never places on a DSP it found running (a warm probe owns nothing; ownership survives only a
  resume of the same instance, after its own reconnect), so uploads in session 0x07 are new everywhere.

**What runs now.** `StripWarmPlugins` (default 1, x8p and Twin X) replaces `StripWarmRebuild` as the gate
(`strip_adopt.h StripWarmPluginsOk`: verdict ADOPTED or REBUILD, record `Plugins` 0, the value on). The
anchored record is the proof that only this driver touched the DSPs since power-up (the verdict needs the
anchor), and `Plugins` 0 (written only after a proven teardown) that the room beside the strips is empty, as
after a cold start; the ledger is built from the pool registers exactly as then. The first plug-in upload
to the adopted DSP is the test of the session: an acknowledged reply sets `WarmUploads` PROVEN (log
`strips: DSP 1 acknowledged the first upload of this warm start ...`, `apollo-diag strips` flag
`UPLOADS_PROVEN`, caps `DSP_WARM_PROVEN`); an unproven first step quarantines DSP 1 as any unproven
transaction does (`plugins: DSP 1 did not prove the first plug-in step after this warm start ...`), which
costs what the old rule cost (a power cycle), plus the strips' Failed state until then (their mix and cue
sends fall back to DSP 0, the next warm start does not adopt). The Twin X's rebuild proves its uploads itself.
RESTORE (a chain may have sat on the strips, e.g. the PC crashed with an insert running, so no teardown ran)
keeps plug-ins off until a power cycle: its modules may still be resident. The restore marks the record
(`Plugins` bit 31, `STRIP_REC_PLUGINS_STALE`): later warm starts adopt the plain strips as they are, plug-ins
still wait, and the next cold start clears the mark.

Apollo Control restores the saved inserts once the driver owns DSP 1 and the strips are Live (no cold-start
gate any more); while it does not, caps `Dsp[1].Flags` bits 15:8 name why (`APOLLO_PLUGIN_WHY_*`) and the
insert waits with that reason and the one action: power-cycle the unit. Not changed: a rate change after an
adoption still needs `StripWarmRebuild=1` to build the strips again (a teardown plus uploads at the new rate).

### 12.6 Transition and tests

- The first install of this build cannot adopt: the build it replaces tears the strips down at its own
  D0 exit and never wrote a record. Expect `unsigned strips (no record): power-cycle once`. After one
  power cycle every later driver update, PC restart and disable/enable adopts.
- strip-plan-test (`CheckAdopt`): the record seal against single-byte changes, the plan hash against
  every plan input and pinned for the x8p, the boot-word rules with the logged words, the anchor
  table, the whole decision table, and start-by-start lives of an x8p (cold, three warm adoptions,
  UA in between, power cycle) and a Twin X (cold, warm rebuild, saturated counter, a foreign
  connect). 989 checks in all.
- strip-plan-test also pins `StripWarmPluginsOk` (ADOPTED and REBUILD yes, RESTORE, not anchored, torn
  down, another plan no; StripWarmPlugins 0 no); ident-test the `StripWarmPlugins` defaults.
- Hardware (not run yet): install warm (expect unsigned), power-cycle the x8p once, check
  `apollo-diag --unit x8p strips` (Live, Record RUNNING, ANCHORED, KEEP_AT_EXIT) and input 1 with
  `levels 3`; then `pnputil /disable-device` + `/enable-device` of the x8p and check `Warm start
  ADOPTED` and input 1 again. A Twin X warm restart then exercises the rebuild.
