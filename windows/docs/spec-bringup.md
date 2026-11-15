# Apollo Thunderbolt on Windows: hardware bring-up spec

Scope: PCI identity, resources, the ordered bring-up from probe to "ready to start the audio
transport", teardown/removal/power, the ioctl ABI, and the Linux to KMDF mapping, for the
Apollo x8p (device_type 0x22) and the Apollo Twin X (Gen 1 0x99, Gen 2 0x3B/0x3C/0x40/0x41).
The streaming path (transport prepare/start, PCM copy, period timing) and the DSP/mixer layer
(mixer settings mailbox, bus params, input strips, plug-ins) are covered by the other two specs;
where bring-up depends on them this document says so and stops at the interface.

Source: `apollo-win/reference/open-apollo` (shallow clone, no git history; read only).

## 0. Conventions

- Citations are `file:line` relative to `reference/open-apollo/driver/` for driver sources and
  the driver's tests (`ua_core.c:4747`, `tests/test_aer_containment.py:13`), and relative to
  `reference/open-apollo/` for everything else (`devices/apollo-x8p.json:14`,
  `CHANGELOG.md:226`, `driver/BUILDING.md:93`, `mixer-engine/hardware.py:767`).
- Line numbers refer to the shallow clone downloaded on 2026-11-10; there is no git history, so
  commit hashes quoted from the source (6016d4e, 2df18d5) cannot be checked out.
- "kext" = UA's macOS driver UAD2System.kext, which the Linux authors reverse engineered; the
  source's kext notes are quoted as the source states them, not verified here.
- "R x" = 32-bit MMIO read of BAR0 offset x, "W x = v" = 32-bit MMIO write. All registers are
  32-bit, little endian, 4-byte aligned (`ua_core.c:1234-1236`, `ua_apollo.h:1688-1700`).
- "default" = the Linux module-parameter default. The Windows port should implement the default
  branch only unless this spec says otherwise; the parameters are listed in Appendix A.
- "(derived)" marks a value computed here from cited source, not written in the source.
- "(Windows)" marks Windows-side knowledge that is not in the source; verify against Microsoft
  documentation and on the target machine.

## Summary of the five most important findings

1. **Model branch.** The x8p (0x22) is a "ring-connect" device: no boot transport, ring connect
   at cold probe only, ACEFACE later in the audio connect. Both Twin X generations are
   "AudioExtension" devices by device type (0x99 >= 64, 0x3B >= 39) that also need the ring
   connect (public types 0x19/0x25/0x29 are in the kext set), so on a cold start they run a boot
   transport plus early ACEFACE, and their ring connect is deferred to right after the ACEFACE in
   the audio connect, cold or warm (`ua_apollo.h:149-157`, `ua_core.c:3686-3706`,
   `ua_audio.c:3161-3163`). Section 3.0 has the full table.
2. **Quiesce before DMA, valid SG before transport.** The Linux driver stops the transport,
   masks/clears every vector and pulls every DSP ring back to its position before bus mastering is
   enabled, and programs the 1024-entry play/rec SG tables before any transport start, warm or
   cold. An SG entry of 0 made the FPGA DMA to address 0, an IOMMU fault, a fatal PCIe error and a
   host lockup (`ua_core.c:4348-4370`, `ua_core.c:4858-4868`). On Windows pci.sys enables bus
   mastering before the driver runs, so the quiesce must be the very first thing the driver does.
3. **The reset-strobe re-clear (issue #46, x8p).** DMA_CTRL (0x2200) bits [16:9] are per-engine
   reset strobes. The old ring init OR'ed 0x1FFFE and left DMA_CTRL at 0x1ffff with every strobe
   asserted, and the DSP command rings were never serviced. Every DMA_CTRL sequence must now be
   assert, deassert, read back, with exactly one enable bit per existing DSP and no strobe left set
   (`ua_dsp.c:792-828`, `ua_dsp.c:914-955`, `CHANGELOG.md:226`).
4. **No firmware is loaded at bring-up by default.** The DSPs boot themselves; the optional
   "mixer firmware stage" (`ua-apollo-mixer.bin`) and the captured plug-in chain
   (`ua-apollo-plugin-chain.bin`) are both off by default and the chain is known harmful (IOMMU
   faults, wedged ring) (`ua_core.c:83-86`, `ua_audio.c:65-67`, `CHANGELOG.md:198`). The
   bring-up is pure register work plus ring commands.
5. **Analog inputs need DSP-side "input strips" after a cold start.** After a driver cold start
   every analog record channel reads zero until the Console input strips are built on the DSP
   (`ua_strips.c:7-12`). The driver can build them only for the x4 and Twin X Gen 2
   (`ua_strips.c:761-800`); the x8p and Twin X Gen 1 cannot. For a Zoom/Discord microphone this
   is the decisive risk, and it is owned by the DSP spec. On a warm takeover the x8p keeps the
   strips the previous OS built (it is never ring-connected on the warm path), the Twin X loses
   them (ring connect on every connect, `CHANGELOG.md:50-52`).

---

## 1. PCI identity

### 1.1 IDs

| Item | Value | Source |
|---|---|---|
| Vendor ID | 0x1A00 | `ua_apollo.h:47` |
| Device ID, UAD-2 PCIe cards | 0x0001 | `ua_apollo.h:48` |
| Device ID, Apollo Thunderbolt (all Apollos) | 0x0002 | `ua_apollo.h:49` |
| Linux match table | both 0x0001 and 0x0002, any subsystem | `ua_core.c:146-150` |
| Subsystem 0x0011 | x4 (QUAD) | `ua_apollo.h:55` |
| Subsystem 0x000F | Apollo Solo (pin) | `ua_apollo.h:56` |
| Subsystem 0x0006 | Apollo 8p, original (pin) | `ua_apollo.h:57` |
| Subsystem 0x0014 | Apollo x8p (fallback only) | `ua_apollo.h:58`, `devices/apollo-x8p.json:7` |
| Subsystem 0x05, 0x0A, 0x0B | INTx/MSI selection only (UAD-2 Satellite TB = 0x0A/0x0B) | `ua_core.c:4569-4591` |

The subsystem ID is read from PCI config offset 0x2E (`ua_core.c:4569-4572`, Linux
`pdev->subsystem_device` at `ua_core.c:4783`). The subsystem **vendor** ID is not in the source.
The Windows INF should match `PCI\VEN_1A00&DEV_0002` (optionally with `&SUBSYS_0014xxxx`, where
xxxx is the subsystem vendor read from the real unit). Do not match DEV_0001 (UAD-2 PCIe cards).

### 1.2 Model identification algorithm (exact)

`ua_detect_capabilities()` (`ua_core.c:349-458`), run right after BAR0 is mapped:

```
fpga = R 0x2218                                   ; cached, ua_core.c:4782
subsys = PCI config 0x2E                          ; ua_core.c:4783
if fpga == 0xFFFFFFFF: fail -ENODEV (device offline)           ; ua_core.c:361-365
if (s32)fpga >= 0:                                 ; bit 31 clear = v1 firmware
    fw_v2 = false
    device_type = (fpga >> 28) - 1                 ; ua_core.c:368-370
    num_dsps = {1:1, 2:2, 3:4, 4:1}[fpga >> 28], else 0 + error log   ; ua_core.c:378-395
else:                                              ; v2 firmware (every TB3 Apollo)
    fw_v2 = true
    ext_caps = R 0x2234
    if ext_caps == 0xFFFFFFFF: fail -ENODEV        ; ua_core.c:400-404
    num_dsps = (ext_caps >> 8) & 0xFF              ; ua_core.c:406
    if num_dsps > 8: fail -ENODEV ("not an Apollo") ; ua_core.c:413-418
    switch subsys:
      0x0006 -> device_type = 0x0A (Apollo 8p)     ; ua_core.c:436-438
      0x000F -> device_type = 0x27 (Solo)          ; ua_core.c:439-441
      default:
        device_type = serial_lookup()              ; ua_core.c:443
        if device_type == 0 and subsys == 0x0014: device_type = 0x22 (x8p fallback)  ; ua_core.c:444-450
public_type = compute_public_type()                ; ua_core.c:455
resolve_public_model()                             ; ua_core.c:456
```

**Serial lookup** (`ua_core.c:504-525`, `ua_apollo.h:205-224`): read 4 dwords at BAR0 0x20,
0x24, 0x28, 0x2C, copy as 16 bytes (little-endian dword order) into an ASCII string, and compare
**characters 5-8 (string offset +4, length 4)** against this table, first match wins
(`ua_core.c:473-496`):

| Serial digits 5-8 | device_type | Serial digits 5-8 | device_type |
|---|---|---|---|
| 2005 | 0x1F x4 | 2092 | 0x36 x4 Gen 2 |
| 2019 | 0x20 x6 | 2086 | 0x37 x6 Gen 2 |
| 2018 | 0x21 x8 | 2087 | 0x38 x8 Gen 2 |
| **2017** | **0x22 x8p** | 2088 | 0x39 x8p Gen 2 |
| 2020 | 0x23 x16 | 2089 | 0x3A x16 Gen 2 |
| 2024 | 0x27 Solo | **2090** | **0x3B Twin X Gen 2** |
| 2025 | 0x28 Arrow | **2091** | **0x3C Twin X Gen 2 (B)** |
| 2073 | 0x35 x16D | **2107** | **0x40 Twin X Gen 2 TB4** |
| 2082 | 0x35 x16D | **2108** | **0x41 Twin X Gen 2 TB4 (B)** |
| 2016 | 0x1E (unidentified HEXA) | 2109 | 0x3F x4 Gen 2 TB4 |
| 2032 | 0x2A (unidentified HEXA) | 2110 | 0x3E (unidentified HEXA) |

The leading four digits are **not** the model (x4 reads "2019 2005 01xxxx"; matching digits 1-4
would call it an x6; the x8p reads "2008 2017 00xxxx") (`ua_apollo.h:211-221`).

**Public type** (the kext's UAD2DeviceType; every kext mask test uses it, not device_type)
(`ua_core.c:545-624`, `ua_apollo.h:178-186`):

```
if !fw_v2: return 0
fam = (ext_caps >> 20) & 0x3f
if fam == 0x3f: return 0xFFFFFFFF (UA_PUBTYPE_NONE)
if fam == 0 or fam > 22: return 0
base = family_map[fam-1], family_map = {0x05,0x04,0x00,0x07,0x08,0x0B,0x04,0x0C,0x0D,0x0F,
        0x0E,0x00,0x00,0x12,0x13,0x1A,0x16,0x18,0x19,0x00,0x00,0x20}   ; ua_core.c:562-566
if base not in {0x13,0x16,0x19,0x1A}: return base
st = serial_lookup() ; ft = type_from_fpga_rev(fpga)                  ; ua_core.c:582-583
  type_from_fpga_rev: (rev & 0x60) == 0x20 ? (rev >> 24) & 0x7f : rev >> 28   ; ua_core.c:540-543
base 0x16 (HEXA, family 17): st in 0x20..0x23 -> 0x14 + (st - 0x20); st == 0x35 -> 0x21;
        st in 0x37..0x3A -> 0x21 + (st - 0x37); else ft == 0x35 ? 0x20 : 0x16   ; ua_core.c:603-611
base 0x19 (Twin X, family 19): st in {0x27,0x28,0x26,0x2E,0x2F} -> 0x19;
        st in {0x3B,0x3C,0x3D} -> 0x25; st in {0x40,0x41} -> 0x29; else 0x19    ; ua_core.c:612-622
(0x13 and 0x1A branches: Conrad family and x4, ua_core.c:586-602)
```

**resolve_public_model** (`ua_core.c:653-680`): if public type is 0, NONE, or >= 0x80, keep. If
public type is 0x19 and device_type is 0x27 or 0x28 (serials 2024/2025 on a Twin X family unit),
or on the Conrad family, set `device_type = 0x80 | public_type`. Otherwise, if device_type is still
0, set `device_type = 0x80 | public_type`. This is how a Twin X Gen 1 becomes 0x99
(`ua_apollo.h:109-128`, `devices/apollo-twin-x.json:3-8`).

Model names come from the generated table (`ua_models_gen.h:1128-1162`): 0x22 "Apollo x8p",
0x3B/0x3C "Apollo Twin X Gen 2", 0x40/0x41 "Apollo Twin X Gen 2 TB4", 0x99 "Apollo Twin X".

### 1.3 What the three targets present

| | Apollo x8p | Twin X Gen 1 | Twin X Gen 2 |
|---|---|---|---|
| PCI device | 0x0002 | 0x0002 | 0x0002 |
| Subsystem | 0x0014 (`devices/apollo-x8p.json:7`) | not in source | not in source |
| Serial digits 5-8 | 2017 (leading 2008) (`devices/apollo-x8p.json:6-9`) | none known (`devices/apollo-twin-x.json:6`) | 2090, 2091, 2107, 2108 (`devices/apollo-twin-x-gen-2.json:6`) |
| ext_caps family | 17 (HEXA, base 0x16) (derived from `ua_core.c:562-566`; test `tests/test_public_type.py:89`) | 19 (`devices/apollo-twin-x.json:8`) | 19 (`tests/test_public_type.py:101`) |
| device_type | 0x22 by serial | 0x99 = 0x80 + 0x19 | 0x3B / 0x3C / 0x40 / 0x41 |
| public_type | 0x16 (`devices/apollo-x8p.json:5`) | 0x19 | 0x25 (2090/2091), 0x29 (2107/2108) |
| DSPs | 6 (example probe banner, `tools/contribute/device-probe.sh:147`; "HEXA", `ua_apollo.h:68`) | from ext_caps; Duo/Quad not stated | Duo verified with 2 DSPs (`ua_strips.c:777-781`) |
| Mixer kind | 0x0C (`ua_core.c:1782-1783`) | 0x0F (`ua_core.c:1784-1785`) | 0x15 (`ua_core.c:1792-1793`) |
| Status | verified (`devices/apollo-x8p.json:12`) | unverified (`devices/apollo-twin-x.json:7`) | verified, Duo at 48 kHz with input_strips=1 (`devices/apollo-twin-x-gen-2.json:7`, `README.md:133`) |

Observed identity values from other units, for sanity checks: x4 FPGA_REV 0x9f0ea929
(`ua_core.c:538`); FW_RELEASE 0x0b0fc9b3 = 11.15.51635, shared by the x4 and the Twin X built from
the same drop (`ua_apollo.h:247-249`); DSP 0 status word 0x1c00 on the x4 (`ua_apollo.h:236-237`).
The x8p's firmware IO descriptor lists 34 inputs and 52 outputs (`ua_apollo.h:443-444`).

### 1.4 Identity registers

| Offset | Name | Use | Source |
|---|---|---|---|
| 0x0000 | FW_VERSION (really DSP 0 status word) | GET_DEVICE_INFO.fw_version; DSP fault check | `ua_apollo.h:233-257` |
| 0x0004 | ID_WORD / FW_BOOT | bootloader release | `ua_apollo.h:258-259` |
| 0x0008 | FW_RELEASE | major [31:24], minor [23:16], build [15:0]; Mixer Engine expects major 9 or 11 | `ua_apollo.h:238-246, 260`, `ua_ioctl.h:688-690` |
| 0x0020-0x002F | serial, 16 ASCII | model lookup; SYSTEM_INFO.serial | `ua_apollo.h:223-224`, `ua_core.c:1053-1061` |
| 0x0030 / 0x0034 | HW_UID lo/hi | unit id, batch ioctl hwid check | `ua_apollo.h:263-264` |
| 0x01AC | DEV_INFO | read only for public types in mask 0x3ff16fcf9a0 | `ua_apollo.h:261-262`, `ua_core.c:1018-1025` |
| 0x2218 | FPGA_REV | cached at probe; re-checked at every (re)init | `ua_apollo.h:265` |
| 0x2234 | EXT_CAPS | [31] audio extension / 64 vectors, [25:20] family+1, [15:8] DSP count, [7:4] DSP-ready capability | `ua_apollo.h:64-68, 266`, `ua_dsp.c:3069-3072`, `ua_apollo.h:1749-1761` |
| 0x2238 | SERIAL | GET_DEVICE_INFO.serial; reads 0 on the x4 | `ua_apollo.h:267`, `ua_ioctl.h:639-643` |

---

## 2. Resources

### 2.1 BAR0

- Only BAR0 is used; it is mapped whole (`ua_core.c:4772`). No other BAR is touched.
- Minimum size: the kext asserts 0x2800 (10 KiB); smaller fails probe (`ua_core.c:4763-4770`,
  `ua_apollo.h:561`).
- The driver accesses offsets up to the CLI response buffer end 0xC4FF (`ua_apollo.h:463-467`,
  `ua_ioctl.h:265-266`), so the real BAR must be at least 0xC500 bytes (derived); expect 64 KiB.
  The exact size is not in the source: read it from the Windows resource list and refuse a BAR
  smaller than 0xC500.
- Map uncached (Windows: `MmMapIoSpaceEx(..., PAGE_READWRITE | PAGE_NOCACHE)`).
- Every BAR0 access goes through a gate: once `shutdown` is set, reads return 0xFFFFFFFF and
  writes are dropped (`ua_apollo.h:1687-1700`, `tests/test_mmio_shutdown_gate.py`). The gate is a
  lock-free flag test, deliberately not a spinlock, because a read on a dead link can stall
  (`tests/test_mmio_shutdown_gate.py:1-9`).

### 2.2 Register map used by bring-up

DSP windows and ring banks (two different per-DSP areas):

| DSP n | Window (status, boot word) `n*0x800 (+0x2000 if n>3)` | Ring bank `0x2000+n*0x80` (n<4), `0x5E00+n*0x80` (n>=4) | cmd ring | resp ring |
|---|---|---|---|---|
| 0 | 0x0000 | 0x2000 | 0x2000 | 0x2040 |
| 1 | 0x0800 | 0x2080 | 0x2080 | 0x20C0 |
| 2 | 0x1000 | 0x2100 | 0x2100 | 0x2140 |
| 3 | 0x1800 | 0x2180 | 0x2180 | 0x21C0 |
| 4 | 0x4000 | 0x6000 | 0x6000 | 0x6040 |
| 5 | 0x4800 | 0x6080 | 0x6080 | 0x60C0 |
| 6 | 0x5000 | 0x6100 | 0x6100 | 0x6140 |
| 7 | 0x5800 | 0x6180 | 0x6180 | 0x61C0 |

Formulas: window `ua_core.c:727-731` / `ua_dsp.c:3083-3087`; bank `ua_apollo.h:485-488, 1723-1730`;
cmd at bank+0x00, resp at bank+0x40 (`ua_apollo.h:508-509`). Table values derived.

DSP window words: +0x00 status (fault mask 0x0080007F, slave bits 0x1C00) (`ua_core.c:724-725`);
+0x0C measured load (`ua_ioctl.h:45-46`); +0x40 message FIFO write index, +0x44 16 slots x 5
dwords (`ua_dsp.c:4702-4705`); +0x1A4 boot/ready word, 0 or 0xA8CAED0F = not started
(`ua_dsp.c:3065-3075`).

Per-ring registers (offset from the ring base) (`ua_apollo.h:490-503`):

| +off | Name | Meaning |
|---|---|---|
| 0x00/0x04 | PAGE0_LO/HI | bus address of ring page 0 |
| 0x08/0x0C | PAGE1_LO/HI | page 1 |
| 0x10/0x14 | PAGE2_LO/HI | page 2 |
| 0x18/0x1C | PAGE3_LO/HI | page 3 |
| 0x20 | WRITE_PTR | host producer index (persisted copy) |
| 0x24 | DOORBELL | FPGA's target index; writing it starts processing |
| 0x28 | POSITION | FPGA consumer index, 0..1023; a read >= 0x400 is invalid |

Rings: 4 pages x 4 KiB = 1024 entries of 16 bytes, circular, index wraps with & 0x3ff; one slot
always stays empty; WRITE_PTR == POSITION means idle (`ua_apollo.h:505-519`, `ua_dsp.c:119-151`).
Entry: inline `{cmd, param, 0, 0}` or DMA reference `{dwords | 0x80000000, 0, addr_lo, addr_hi}`
(`ua_dsp.c:12-18, 50-56`), little endian (`driver/BUILDING.md:93-94`).

Global and transport registers:

| Offset | Name | Semantics | Source |
|---|---|---|---|
| 0x2200 | DMA_CTRL | bit 0 global enable; v2: enables [8:1], strobes [16:9]; v1: enables [4:1], strobes [12:9] | `ua_apollo.h:270, 563-568`, `ua_core.c:257-266` |
| 0x2204 | IMR | enable mask, HW vectors 0-31 | `ua_apollo.h:271` |
| 0x2208 | ISR | status, write-1-to-clear, vectors 0-31 | `ua_apollo.h:272` |
| 0x2210-0x223C | (unknown) | read for a debug dump only | `ua_dsp.c:999-1024` |
| 0x221C | DSP_RESET | never written by the driver | `ua_apollo.h:276` |
| 0x2220 | TRANSPORT | only ever written 0 ("0 = stop") | `ua_apollo.h:277`, writes at `ua_core.c:799, 864, 4277, 4639, 5042`, `ua_audio.c:9695` |
| 0x2240 | AX_BUF_SIZE | frames - 1 | `ua_apollo.h:287` |
| 0x2244 | AX_SAMPLE_POS | | `ua_apollo.h:288` |
| 0x2248 | AX_CONTROL | 0 stop, 1 arm, 0xF start, 0x20F start + ext mode | `ua_apollo.h:289, 346-353` |
| 0x224C | AX_P2P_ROUTE | | `ua_apollo.h:290` |
| 0x2250 | AX_PLAY_CH | | `ua_apollo.h:291` |
| 0x2254 | AX_FRAME_CTR | read | `ua_apollo.h:292` |
| 0x2258 | AX_FRAME_CFG | | `ua_apollo.h:293` |
| 0x225C | AX_REC_CH | | `ua_apollo.h:294` |
| 0x2260 | AX_CONNECT | doorbell: 1 connect, 4 clock, 0x10 disconnect | `ua_apollo.h:295, 355-358` |
| 0x2264 | ISR_HI | W1C, vectors 32-63 | `ua_apollo.h:280` |
| 0x2268 | IMR_HI | vectors 32-63 | `ua_apollo.h:281` |
| 0x226C | AX_DMA_BLKSZ | | `ua_apollo.h:296` |
| 0x2270 | AX_TIMER_CFG | | `ua_apollo.h:297` |
| 0x22C0 | AX_STATUS | read fence only | `ua_apollo.h:299-302` |
| 0x22C4 | AX_P2P_ENABLE | written 0 at shutdown | `ua_apollo.h:304-307` |
| 0x3800 | MIXER_BASE | mixer window (DSP/mixer spec) | `ua_apollo.h:329` |
| 0x3808 | MIXER_SEQ_WR | host sequence | `ua_apollo.h:330` |
| 0x380C | MIXER_SEQ_RD | DSP sequence | `ua_apollo.h:331` |
| 0x3810 | MIXER_RB_STATUS | 1 = readback ready; write 0 to re-arm | `ua_apollo.h:896` |
| 0x3814 | MIXER_RB_DATA | 40 words | `ua_apollo.h:897-898` |
| 0x8000 | PLAY SG | 1024 x {lo, hi}, write-only (reads 0) | `ua_apollo.h:362`, `ua_audio.c:719-732` |
| 0xA000 | REC SG | 1024 x {lo, hi}, write-only | `ua_apollo.h:363` |
| 0xC000 | NOTIF_CONFIG | 10 dwords written after connect, no bank shift | `ua_apollo.h:378-382, 411-415` |
| 0xC028 | NOTIF_READBACK | 95 dwords read after connect | `ua_apollo.h:384, 409, 414` |
| 0xC02C | NOTIF_ACEFACE | magic target | `ua_apollo.h:401` |
| 0xC030 | NOTIF_STATUS | write 0 to clear (NOT write-1-to-clear) | `ua_apollo.h:402`, `ua_audio.c:2400-2402` |
| 0xC07C | NOTIF_RATE_INFO | rate index << 8, lock in [1:0] | `ua_apollo.h:404, 1200` |
| 0xC080 | NOTIF_XPORT_INFO | | `ua_apollo.h:405` |
| 0xC084 | NOTIF_CLOCK_INFO | | `ua_apollo.h:406` |
| 0xC08C | NOTIF_CLOCK_SRCS | [31:16] source mask, [7:0] rate mask | `ua_apollo.h:407-408`, `ua_audio.c:3133-3138` |
| 0xC090 | clock names, 256 bytes | | `ua_apollo.h:934` |
| 0xC198 / 0xC19C | firmware latency out/in | | `ua_apollo.h:936-937` |
| 0xC1A4 / 0xC2C4 | input / output IO descriptor, 72 dwords | streaming spec | `ua_apollo.h:421-452` |
| 0xC3F4-0xC4FF | ARM CLI | never used: "freezes the ARM MCU" | `ua_apollo.h:463-467`, `ua_core.c:1321-1327` |

The 0xC0xx bank offset is `0x0A * 4 = 0x28`, documented as "Bank shift for Apollo x4 (slot ID)"
and used unconditionally for every model (`ua_apollo.h:368-409`).

### 2.3 DMA

- **Mask: 32 bit**, on purpose: "the FPGA may only use the low 32 bits internally"; a buffer at
  0x19f800000 failed (`ua_core.c:4747-4761`). The `dma64_audio` experiment (off) raises it only
  around the two audio ring allocations (`ua_audio.c:85-94, 663-717`).
- Allocations made at bring-up (all live from probe to remove; reused on resume):

| Buffer | Size and shape | Allocation | Source |
|---|---|---|---|
| DSP n command ring | 4 pages x 4 KiB, zeroed | `dma_alloc_coherent` per page | `ua_dsp.c:62-100, 857-878` |
| DSP n response ring | same | same | same |
| Audio play ring | 4 MiB = 1024 SG entries x 4 KiB; 1024 separate kernel pages, not contiguous | `dma_alloc_pages` + `vmap` | `ua_audio.c:468-556, 663-717`, `ua_apollo.h:364-366` |
| Audio record ring | same | same | same |
| Command/response buffers of DSP block sends | per command | `dma_alloc_coherent`; on timeout parked as "orphans", max 16 buffers / 8 MiB | `ua_dsp.c:230-263` (DSP spec) |
| Plug-in chain payloads | off by default, do not port | | `ua_apollo.h:1455-1462` |

- The audio rings are allocated and the SG tables programmed at **device start**, not at stream
  open, because a cold Twin X start runs a boot transport before any stream exists
  (`ua_core.c:4364-4383`). This constrains the streaming spec's buffer ownership (section 7, R6).

### 2.4 Interrupts

**Type** (`ua_core.c:4569-4591, 4873-4904`, matching the kext's `_initializeInterrupts`): device
0x0001 always INTx; device 0x0002 with subsystem 0x05 INTx; 0x0A/0x0B MSI (satellite_msi default
on, as the UA Windows INF does); every Apollo uses MSI. `pci_alloc_irq_vectors(1, 2, MSI | MSI-X)`
(`ua_apollo.h:900-902`); if that fails, one INTx vector. **Only vector 0 is ever requested**
(`ua_core.c:4908-4910`). INTx is requested shared and the ISR returns "not mine" when nothing
enabled is pending.

**64-vector mode** (IMR_HI/ISR_HI in use) when `fw_v2 && (ext_caps & BIT(31)) &&
((ext_caps >> 20) & 0x3f) != 6`, which is every known Apollo (`ua_apollo.h:1749-1761`,
`CHANGELOG.md:202`).

**Vector map** (logical -> hardware bit) (`ua_apollo.h:469-483, 1732-1775`):

| Logical | HW bit | Register bit | Meaning | Enabled when |
|---|---|---|---|---|
| 0x28 | 32 | ISR_HI/IMR_HI bit 0 | audio-extension notification | SG programming, with clear-pending (`ua_audio.c:802-803`) |
| 0x46 | 62 | bit 30 | periodic timer | only with period_irq=1 (default off) (`ua_audio.c:3505-3517`) |
| 0x47 | 63 | bit 31 | end of buffer | transport prepare (`ua_audio.c:3880`), streaming spec |
| DSP n slot j | n*5+j (<=4 DSPs), n*4+j with j<4 (>4 DSPs) | ISR/IMR low word | DSP FIFO/error/ring vectors | only with dsp_msg_irq (default 0) (`ua_dsp.c:4708-4722`) |

So after bring-up only vector 0x28 is enabled. With the defaults the device raises only
end-of-buffer and notification interrupts, "about 6 a second at 48 kHz" (`CHANGELOG.md:204`).

**ISR** (`ua_core.c:159-234`), exactly:

```
if shutdown: return NONE                                   ; 166-168
lock irq_lock (spinlock, irqsave)                          ; 170
lo = R 0x2208
if lo == 0xFFFFFFFF: unlock; return NONE  (device gone)    ; 189-192
lo &= cached IMR
hi = irq64 ? (R 0x2264 & cached IMR_HI) : 0                ; 195-197
if !lo && !hi: unlock; return NONE                         ; 199-202
if lo: W 0x2208 = lo  ; ack only the bits handled          ; 205-206
if hi: W 0x2264 = hi                                       ; 207-208
unlock
first-interrupt timestamp (self-test)                      ; 221-222
wake = audio_irq(lo, hi)     ; hi bits 30/31, only while transport running  ; ua_audio.c:6816-6875
dsp_irq(lo, hi)              ; only with dsp_msg_irq                        ; ua_dsp.c:4810-4877
notif_irq(hi)                ; hi bit 0, only with notif_irq (default off)   ; ua_audio.c:2521-2550
return wake ? WAKE_THREAD : HANDLED
```

Never ack bits that are not enabled: the DSP "noise" bits 0x18c63 re-set at once and the MSI storm
"overwhelms the Thunderbolt link" (`ua_core.c:172-181`). The threaded half only serves period_irq
streams (`ua_core.c:236-245`, `ua_audio.c:6405`), i.e. nothing in the default configuration.

Vector helpers, all under the same irq_lock the ISR holds (`ua_apollo.h:1777-1864`):
- enable(v, clear_pending): set cache bit; if clear_pending W1C its ISR bit; W IMR(_HI) = cache;
  if clear_pending R 0x2218 as a posted-write flush (no flush from the ISR re-arm path).
- mask_all(clear_pending): caches = 0; W 0x2204 = 0; W 0x2268 = 0 (irq64); if clear_pending
  W 0x2208 = 0xFFFFFFFF and W 0x2264 = 0xFFFFFFFF (irq64).
- disable(v): clear cache bit; W IMR(_HI) = cache; no flush.

The probe's interrupt "self-test" is passive: it writes nothing and only logs whether any
interrupt arrived within 50 ms (`ua_core.c:4653-4695`). There is no software-triggerable vector.

### 2.5 PCIe link, ASPM, config space

- The driver never needs config writes; the kext only reads config space (`ua_audio.c:75-79`).
- After the first successful ACEFACE handshake, `ua_pcie_setup()` reads DEVCTL/DEVSTA/LNKCTL/LNKSTA
  and, if LNKCTL has L0s enabled, **disables L0s on the Apollo's link** through the ASPM core
  (`pci_disable_link_state(PCIE_LINK_STATE_L0S)`). Reason: the FPGA advertises an unlimited L0s
  exit latency and the Thunderbolt bridge logged a steady stream of correctable Replay Timer
  Timeouts while streaming (`ua_audio.c:3902-3950`, `CHANGELOG.md:243`). Once per device
  (`pcie_setup_done`).
- `pcie_tuning=1` (off by default) also disables L1, clears DevSta error bits, and sets the
  completion timeout to range D (DEVCTL2[3:0] = 0x6, 65-210 ms) on the Apollo and every upstream
  bridge (`ua_audio.c:3946-4025`). It is off because "the April Thunderbolt link drops followed
  these writes by ~3 ms" (`ua_audio.c:75-83`).
- Link speed/width are read from LNKSTA only for reporting (`ua_core.c:1080-1084`). An x4 behind
  Thunderbolt trains at x1 Gen1, enough for every channel at 192 kHz (`mixer-engine/hardware.py:765-769`).
- `pci_restore_state()` only in AER slot reset (`ua_core.c:5173`). No FLR or secondary bus reset is
  ever issued; the kext's HardResetDSPs is not ported (`ua_core.c:1634-1636`).

### 2.6 IOMMU notes

- Every device-visible address (SG entries, ring pages, DMA references) is a DMA API bus address,
  i.e. an IOMMU address when one is on. Windows must likewise use DMA-adapter logical addresses,
  never `MmGetPhysicalAddress` (Windows; section 6).
- SG entries left at 0 or at a previous OS's addresses made the FPGA DMA to invalid addresses:
  IOMMU fault, PCIe Uncorrectable Fatal error, host lockup with VT-d on (`ua_core.c:4282-4288,
  4348-4370`, `ua_audio.c:811-825`).
- Free nothing the device may still write: the audio teardown waits 100 ms after stopping the
  transport "otherwise the IOMMU revokes the mapping while the device is still writing, causing a
  DMA fault that kills the Thunderbolt bridge" (`ua_audio.c:9688-9696`); timed-out ring
  buffers are parked, not freed (`ua_dsp.c:230-259`).
- A DMA reference to address 0 sent through the raw ring ioctl made the IOMMU fault the Apollo and
  wedged its transport until a power cycle; the captured plug-in chain faulted the IOMMU on one of
  its DMA references (`CHANGELOG.md:198, 200`).
- The x8p was verified with `intel_iommu=on` and no DMAR faults (`devices/apollo-x8p.json:14`).
  The installer still adds `iommu=pt` on "most Intel systems" (`docs/install.md:27-28`).

---

## 3. Bring-up sequence: probe to "ready to start the audio transport"

### 3.0 Model branches

| Decision | x8p 0x22 | Twin X Gen 1 0x99 | Twin X Gen 2 0x3B/0x3C/0x40/0x41 | Source |
|---|---|---|---|---|
| `ua_uses_audio_extension(device_type)` | false (bit 34 in ring mask 0x7f17fcf9b0) | true (type >= 64) | true (types >= 39 not in mask) | `ua_apollo.h:141-157` |
| Ring connect needed (kext_connect, public type in 0x3ff17fcf9b0) | yes (0x16) | yes (0x19) | yes (0x25/0x29) | `ua_apollo.h:189-197`, `ua_core.c:3686-3693` |
| Ring connect deferred to after ACEFACE | no | yes | yes | `ua_core.c:3702-3706` |
| Where the ring connect runs | cold probe only, before ACEFACE | inside the audio connect, cold and warm | same | `ua_core.c:4472-4509`, `ua_audio.c:3161-3163`, `CHANGELOG.md:184` |
| Cold start: boot transport + early ACEFACE + DMA pulse | no | yes | yes | `ua_core.c:4372-4391` |
| `dsp_inherited` on a warm probe (skip ring connect, keep strips) | false | false | false (x4 only) | `ua_core.c:4247-4255` |
| Legacy DMA counts (play/rec) used by the boot transport | 34/32 | 3/2 fallback | 3/2 fallback | `ua_audio.c:246-264, 329-351` |
| Default channel layout | legacy | firmware descriptor | firmware descriptor | `ua_audio.c:305-322` |
| 64-vector mode | yes | yes | yes | `ua_apollo.h:1757-1761` |
| 8-engine DMA masks in `ua_reset_dma_engines` (num_dsps > 4) | yes (6 DSPs) | no | no | `ua_core.c:810-824` |
| Driver can build analog input strips | no | no | yes (input_strips=1) | `ua_strips.c:761-800` |
| Output mute param (public mask 0x8010080000) | no | no | no | `ua_apollo.h:190-203`, `ua_core.c:1575-1582` |

Note: the comment at `ua_core.c:4356-4362` lists "Twin X" among ring-connect devices. That comment
uses the old numbering in which 0x23 was called a Twin X (`ua_audio.c:250`); by the current code
both Twin X generations take the AudioExtension path.

### 3.1 Phase A: enable, map, identify (`ua_core.c:4697-4856`)

1. Allocate the device struct, locks, event channel, unit-group node, resume work item
   (`ua_core.c:4708-4726`). Plug-in host state init (out of scope) (`ua_core.c:4728-4731`).
2. `pcim_enable_device` (memory decode). **Bus mastering stays off** (`ua_core.c:4733-4745`).
3. DMA mask 32 bit (`ua_core.c:4747-4761`).
4. BAR0 length >= 0x2800, map BAR0 (`ua_core.c:4763-4779`).
5. `fpga_rev = R 0x2218` ("first thing the kext does after mapping"), `subsystem_id` from config
   (`ua_core.c:4781-4783`).
6. Identify (section 1.2). Failure aborts probe (`ua_core.c:4786-4788`).
7. Log banner (parsed by `tools/contribute/device-probe.sh`, keep its shape for diagnostics):
   name, FPGA rev, subsystem, DSP count, FW v1/v2; device and public type; `R 0x0008` firmware
   release (`ua_core.c:4790-4807`).
8. v2 only: warn if `!(ext_caps & BIT(31))` or family == 6 (no audio extension in the kext); log
   `R 0x01A4` and `R 0x01AC` as the previous owner left them (`ua_core.c:4809-4830`).
9. Debug reads of the previous session's ring state for DSP 0-3 (WRITE_PTR, POSITION,
   PAGE0_HI/LO), SEQ_WR, SEQ_RD and `R 0x3894` (`ua_core.c:4832-4856`). Registers only: the old
   code that memremap()ed the previous owner's ring page was removed as unsafe. Optional; note it
   reads banks of DSPs 2-3 even on a 2-DSP unit.

### 3.2 Phase B: quiesce before bus mastering (`ua_core.c:4858-4868`)

B1. Stop transport and interrupts (`ua_hw_stop_xport_irqs`, `ua_core.c:841-868`):
```
ax = R 0x2248 ; log "previous session left the transport running" if ax != 0 and != ~0
W 0x2248 = 0
R 0x2248                                   ; flush
[warm_transport0=1 only] W 0x2220 = 0      ; default off: a warm probe keeps the previous owner's value
mask_all(clear_pending=true): W 0x2204 = 0; W 0x2268 = 0 (irq64); W 0x2208 = 0xFFFFFFFF;
                              W 0x2264 = 0xFFFFFFFF (irq64)        ; ua_apollo.h:1828-1844
```
AX_CONTROL = 0 is safe with a live mixer DSP (pcm_close does it on every stop) (`ua_core.c:837-839`).

B2. Neutralize every DSP ring by register only, no ring memory needed (`ua_dsp.c:1116-1143`,
resync helper `ua_dsp.c:512-556`). For each DSP i < num_dsps and each ring base in
{bank(i)+0x00, bank(i)+0x40}:
```
pos = R base+0x28 ; if pos == 0xFFFFFFFF -> -ENODEV (stop) ; if pos >= 0x400 -> pos = 0
repeat up to 3 times:
    W base+0x24 = pos        ; DOORBELL (target) first
    W base+0x20 = pos        ; WRITE_PTR
    next = R base+0x28 (same normalisation, ~0 -> -ENODEV)
    if next == pos: done
    pos = next
after 3 passes: W base+0x24 = pos; W base+0x20 = pos
```
Purpose: whatever the previous owner queued, the FPGA's target is pulled back to its position so
nothing in stale or freed ring pages is fetched once bus mastering is on (`ua_dsp.c:1116-1128`).

B3. `pci_set_master` (`ua_core.c:4868`).

B4. Firmware cache request for resume, only for enabled stages; nothing with defaults
(`ua_core.c:4870-4871`, `ua_dsp.c:1145-1173`).

### 3.3 Phase C: interrupts (`ua_core.c:4873-4916`)

Allocate MSI (1-2 vectors) or INTx per section 2.4; request vector 0 with the hard handler and a
thread; INTx shared. At this point IMR = IMR_HI = 0, so nothing can fire yet. Then
`ua_strips_init` (no-op with input_strips=0, DSP spec) (`ua_core.c:4918-4919`).

### 3.4 Phase D: warm or cold (`ua_mixer_dsp_alive`, `ua_core.c:4192-4229`)

```
seq_wr = R 0x3808 ; seq_rd = R 0x380C ; R 0x3800, R 0x01A4, R 0x01AC (log)
if !(seq_rd > 0 && seq_rd == seq_wr): COLD
W 0x3810 = 0                                ; re-arm readback
repeat 30 times: sleep 10-12 ms; if R 0x3810 == 1: WARM
COLD  ("SEQ counters look alive but readback dead": a zombie DSP left by a broken session)
```
Worst case about 360 ms (derived). WARM means a mixer DSP left running by the previous owner
(another OS, UA's own driver, or this driver after an unload, since the default teardown keeps the
DSP running).

### 3.5 Phase E: cold path (`ua_dsp_init_and_load(dsp_was_dead=true)`, `ua_core.c:4295-4547`)

E1. ProgramRegisters (`ua_core.c:786-808`):
```
if R 0x2218 != cached fpga_rev: fail -EIO ("hardware init failed")          ; 790-796
W 0x2220 = 0                                                               ; 799
ua_reset_dma (8-engine masks on every v2 device, global forced on)         ; 327-336
    v = (R 0x2200) & ~0x1FFFE ; v |= 0x1
    W 0x2200 = v | 0x1FE00     ; assert all 8 strobes
    W 0x2200 = v               ; deassert
    R 0x2200                   ; flush ; cache = v                          ; 267-280
    W 0x2208 = 0xFFFFFFFF ; W 0x2264 = 0xFFFFFFFF (irq64)
check_dsps(early): for each DSP, R window(i)+0x00, log fault bits (& 0x0080007F),
    warn on a 1-DSP unit with slave bits 0x1C00 ; never fails here         ; 740-778
```
After E1 DMA_CTRL = bit 0 only (plus whatever bits >= 17 read back) (derived).

E2. Software state: fw_loaded = dsps_connected = plugins_activated = dsp_inherited = false
(`ua_core.c:4301-4308`).

E3. DSP ready wait (`ua_dsp_wait_ready`, `ua_dsp.c:3061-3162`; called at `ua_core.c:4321-4326`).
Only if fw_v2 and `((R 0x2234) >> 4) & 0xF == 1` (dsp_ready_wait=1 default; =2 forces it):
```
for each DSP i: tries = (i == 0) ? 100 : 10
    loop: if shutdown or R 0x2218 == 0xFFFFFFFF -> -ENODEV
          v = R window(i)+0x1A4
          if v != 0 && v != 0xA8CAED0F: ready
          sleep 300 ms
```
A timeout is logged and the cold start continues (never fails probe). Worst case 30 s for DSP 0
plus 3 s per other DSP (derived). Whether the x8p or Twin X report capability 1 is not in the
source.

E4. DSP rings init (`ua_dsp_rings_init`, `ua_dsp.c:834-1031`), for each DSP i < num_dsps:
- allocate cmd and resp rings (4 zeroed 4 KiB coherent pages each) if not allocated (resume reuses
  them) (`ua_dsp.c:857-878`); a failed DSP is skipped with a warning; ring init failure is
  non-fatal (`ua_core.c:4328-4330`);
- program each ring with no drain (`__ua_dsp_ring_program`, `ua_dsp.c:586-654`): resync as in B2
  (also updates the software indices), then W PAGEk_LO/HI = page k bus address for k = 0..3
  (0 for unused pages), read all four back and warn on a mismatch, zero the pages, `live = true`;
- `rings_allocated = true`; read both POSITIONs (debug).

Then **the per-DSP DMA engine enable (the reset strobe re-clear, issue #46)** (`ua_dsp.c:914-955`,
dma_per_dsp default on `ua_dsp.c:792-801`):
```
en = OR over DSPs with rings of BIT(i+1)          ; x8p 6 DSPs: 0x7E ; 2-DSP Twin X: 0x6 (derived)
d = R 0x2200
W 0x2200 = (d & ~0x1FE00) | en                    ; enables on, no strobes
cache = (R 0x2200) & ~0x1FE00
W 0x2200 = cache | (en << 8)                      ; pulse only these DSPs' strobes (bit i+9), engines enabled
W 0x2200 = cache                                  ; deassert
R 0x2200                                          ; flush
resync every allocated ring again (cmd and resp)  ; the pulse may move POSITION
```
"Reset while enabled" is what kicks the ring engine; without it POSITION stays 0 and doorbells are
ignored (`ua_dsp.c:969-975`). The old code (`dma_per_dsp=0`) OR'ed 0x1FFFE and left DMA_CTRL
reading 0x1ffff with every strobe asserted; the DSP never consumed its command ring after a power
cycle (`ua_dsp.c:956-997`, `CHANGELOG.md:226`). Rule for every DMA_CTRL writer: one enable per
existing DSP, strobes are pulsed (assert, deassert, read) and never left set. The other writers
follow the same rule: `ua_dma_enable_engine` drops strobes (`ua_core.c:282-298`),
`ua_dma_reset_engine` pulses one (`ua_core.c:300-320`), the SG programming uses
`ua_dsp_dma_ctrl_clean` (below).

Then debug reads of 0x2210, 0x2214, 0x221C, 0x2220, 0x2224, 0x2228, 0x222C, 0x2230, 0x2234,
0x2238, 0x223C (`ua_dsp.c:999-1024`) and `ua_dsp_irq_arm` (no-op with dsp_msg_irq=0).

E5. `W 0x3808 = 0` (MIXER_SEQ_WR) (`ua_core.c:4335`).

E6. Audio DMA pre-init and SG (`ua_audio_preinit_dma`, `ua_audio.c:811-845`; called at
`ua_core.c:4364-4370`; failure fails the probe):
- derive legacy channel counts if not yet set (`ua_audio.c:831-833`, streaming spec);
- allocate the two 4 MiB rings once (`ua_audio.c:835-841`);
- program the SG tables (`ua_audio_program_sg`, `ua_audio.c:733-809`):
```
ax = R 0x2248 ; log if armed ; W 0x2248 = 0 ; R 0x2248          ; never rewrite SG under a running transport
for i in 0..1023:
    W 0x8000 + i*8     = lo32(play_page_addr(i))  ; W 0x8000 + i*8 + 4 = hi32(...)
    W 0xA000 + i*8     = lo32(rec_page_addr(i))   ; W 0xA000 + i*8 + 4 = hi32(...)
    (page_addr(i) = kernel page i/(PAGE_SIZE/4K) + (i % (PAGE_SIZE/4K)) * 4 KiB)
no doorbell
d = R 0x2200 ; W 0x2200 = ua_dsp_dma_ctrl_clean(d) ; R 0x2200
    clean(d) = (d & ~(0x1FE00 | 0x1FE)) | 0x1 | en(num_dsps)    ; v2, dma_per_dsp   ; ua_dsp.c:815-828
enable_vector(0x28, clear_pending=true): IMR_HI cache |= 1; W 0x2264 = 0x1; W 0x2268 = cache; R 0x2218
```
After E6 DMA_CTRL = 0x7F on a 6-DSP x8p, 0x7 on a 2-DSP Twin X (derived, bits >= 17 assumed 0).
This is the "SG table on ring-connect devices" fix (2df18d5): ring-connect devices like the x8p
used to have no SG programming at all, SAMPLE_POS frozen at 0 and a host lockup when a stream
opened (`ua_core.c:4356-4363`, `devices/apollo-x8p.json:14`).

E7. **Twin X only (AudioExtension)** (`ua_core.c:4372-4391`):
- `clock_source = 0xC` (internal; source 0 gets no FPGA ack) (`ua_core.c:4374-4381`);
- boot transport start (`ua_boot_transport_start`, `ua_audio.c:2702-2831`), with the legacy counts
  play = 3, rec = 2 for every Twin X type (no row in the legacy table, fallback
  `UA_DEFAULT_PLAY_CH + 1` / `UA_DEFAULT_REC_CH`, `ua_audio.c:348-350`, `ua_apollo.h:911-912`):
```
frames = min(8192, rounddown_pow2(0x400000 / (max(play,rec)*4) / 2))   ; ua_audio.c:389-407 ; = 8192 (derived)
W 0x2240 = frames - 1                       ; 0x1FFF (derived)
W 0x226C = ((frames - 1) * play) >> 10      ; (8191*3)>>10 = 23 (derived)
W 0x2244 = 0
W 0x2248 = 0
W 0x2258 = 0x10
W 0x2250 = play                             ; 3
W 0x225C = rec                              ; 2
W 0x224C = 0x100 | (play - 1)               ; 0x102 (derived)
W 0x2248 = 0x1                              ; arm (DMA enable)
R 0x22C0                                    ; read fence
sleep 1-2 ms                                ; REQUIRED: back-to-back ARM/START is silently ignored (FRAME_CTR stays 0)
W 0x2248 = 0x20F                            ; start + extended mode
poll up to 10 x: if R 0x2254 != 0 break; sleep 1-2 ms
R 0x2248 (log)
early ACEFACE handshake (E8)
d = R 0x2200 ; W 0x2200 = d | 0x1FE00 ; W 0x2200 = d ; R 0x2200     ; DMA reset pulse "to activate ring buffers"
sleep 50 ms                                 ; "without this delay, FPGA ignores ring entries"
```
- then re-write every ring's page addresses (`ua_dsp_rings_reprogram`, `ua_dsp.c:1175-1208`):
  for each allocated DSP, `__ua_dsp_ring_program(no drain)` on cmd and resp (resync, pages,
  verify, zero). "The DMA pulse deactivates the FPGA ring engine; re-writing page addresses
  re-initializes it" (`ua_core.c:4385-4390`).

Note the boot transport's DMA pulse asserts all eight strobes (0x1FE00) for one write even on a
2-DSP unit (`ua_audio.c:2813-2817`); it restores the previous value immediately.

E8. **Early ACEFACE handshake** (`ua_aceface_handshake`, `ua_audio.c:2625-2700`), called from the
boot transport (Twin X cold) and retried after it (E10):
```
if aceface_done: return 0
for retry in 0..1:                                     ; EARLY_ACEFACE_RETRIES = 2
    if retry == 1: clear_stale: W 0x2260 = 0x10 ; R 0x2218 ; sleep 50 ms     ; ua_audio.c:2616-2623
    W 0xC02C = 0x0ACEFACE
    W 0x2260 = 0x1
    for poll in 0..9:                                  ; UA_CONNECT_POLLS = 10
        s = R 0xC030
        if s == 0xFFFFFFFF: return -ENODEV
        if s & BIT(21):
            write connect config: W 0xC000 = 0x0A ; W 0xC004 = 0x17C ; W 0xC008..0xC024 = 0 (8 dwords)
            read 95 dwords from 0xC028 (diagnostic)
            W 0xC030 = 0                               ; clear: write 0, not W1C
            aceface_done = true
            pcie_setup()  (L0s off, section 2.5)
            return 0
        sleep 50 ms                                    ; EARLY_ACEFACE_POLL_MS
aceface_failed = true ; return -ETIMEDOUT ("power-cycle Apollo to connect")
```
Constants: `ua_apollo.h:355-359, 396-415, 954, 957-960`, `ua_audio.c:2650-2651, 2867-2906`. About
1 s worst case (derived). The disconnect doorbell on the second try clears a host connection that
a previous owner left behind; without it "no connect ack ever came" on an x4 (`ua_audio.c:2608-2615`).

E9. Firmware stages, all skipped by default:
- `mixer_fw_stage=1` would send `ua-apollo-mixer.bin` to DSP 0 (section 3.8). Default off:
  "the x4 answers every block with DSP status 0xF0010006 and cold-starts the same without it"
  (`ua_core.c:83-86, 4393-4408`).
- `no_plugins=0` would load the plug-in chain payloads; default `no_plugins=true`
  (`ua_core.c:4417-4434`, `ua_audio.c:65-73`).
- The old "DSP program" upload (command 0x00360000) is gone: no kext counterpart, timed out with
  -110 on every x4 cold start (`ua_core.c:4436-4444`).
- DSPs 1+ never get firmware; "they boot on their own" (`ua_core.c:4410-4415`).

E10. Twin X only: if the early ACEFACE did not complete, retry it once, transport still running
(`ua_core.c:4446-4470`). The full connect is deferred to the audio connect (phase G).

E11. **x8p only: ring connect at probe** (`ua_core.c:4472-4509`), because the x8p needs the ring
connect and it is not deferred:
- v12_connect=1 would ACEFACE first; default off (`ua_core.c:3728-3745`, `ua_core.c:98-101`);
- `ua_dsp_connect_retry` (`ua_dsp.c:3164-3195`) -> `ua_dsp_connect_all` (`ua_dsp.c:2942-3059`):
```
if num_dsps == 0: return 0
for each DSP i with rings (and property 0xC = public type in the ring-connect mask, ua_dsp.c:4981-4993):
    ring program with drain (wait up to 20 ms for queued entries, then resync, pages, zero)  ; ua_dsp.c:510, 668-673
    put {0x00230002, 1, 0, 0} on the cmd ring
    doorbell: memory barrier; W cmd+0x24 = write_ptr ; W cmd+0x20 = write_ptr   ; DOORBELL first
    sleep 5-10 ms
sleep 1-2 ms
DSP 0: put {0x00100002, date, 0, 0} on the cmd ring (appended, no ring reset); doorbell; sleep 5-10 ms
dsps_connected = true ; ua_dsp_irq_arm (no-op by default)
```
  date = `((days since 2007-01-01 00:00 UTC) << 12) + minute_of_day`, 0 for a clock before 2007;
  "the Windows driver agrees" (`ua_dsp.c:769-790`). Nothing waits for a DSP response; the call
  fails only if an entry cannot be queued (ring full, device gone) (`ua_dsp.c:2997-3002, 3035-3040`).
  Doorbell order matters: writing WRITE_PTR first started DMA and the later DOORBELL write reset the
  engine mid-transfer (`ua_dsp.c:736-767`).
- On failure, retry once: `ua_dma_reset_engine(0)` (clear enable bit 1, pulse strobe bit 9, read),
  `ua_dsp_rings_reprogram`, `ua_dma_enable_engine(0)` (set bit 1, drop strobes), connect_all again
  (`ua_dsp.c:3183-3194`, `ua_core.c:300-320, 282-298`).
- plug-in chain activation and bus-coefficient replay only when pending (nothing by default)
  (`ua_core.c:4486-4508`, DSP spec).

E12. Readback liveness poll, both models (`ua_core.c:4511-4538`): `W 0x3810 = 0`; up to 50 x
(sleep 10-12 ms; `R 0x3810 == 1` -> alive). Not fatal either way.

E13. Twin X only, **both cold and warm paths**: boot transport stop (`ua_core.c:4549-4554`,
`ua_audio.c:2833-2847`): `W 0x2248 = 0`; `audio.connected = false`; `aceface_done` stays true so
the audio connect only does the post-handshake work.

### 3.6 Phase F: warm path (`ua_dsp_init_and_load(dsp_was_dead=false)`, `ua_core.c:4244-4294`)

F1. `dsp_inherited = strips_model_known(type) && type == 0x1F` (x4 only); false for x8p and Twin
X (`ua_core.c:4247-4255`).
F2. Plug-in host bookkeeping (out of scope) (`ua_core.c:4256-4257`).
F3. `R 0x2218` must equal the cache, else -EIO (`ua_core.c:4258-4261`).
F4. check_dsps(early), log only (`ua_core.c:4262`).
F5. ACEFACE is not assumed to persist across OS reboots: `aceface_done` stays false, the audio
connect does the full handshake (`ua_core.c:4263-4265`).
F6. `warm_dma_reset=1` only (default off): `W 0x2220 = 0`, `ua_reset_dma_engines` (4- or 8-engine
masks by DSP count, no global bit, no ISR write), `W 0x2208 = 0xFFFFFFFF` (`ua_core.c:4267-4280,
810-824`). Off because "an earlier 8-engine reset killed a running mixer DSP" (`ua_core.c:81`).
F7. SG programming is mandatory on warm boot too, "the previous OS's SG entries point to invalid
physical addresses" (preinit_dma as E6; a failure only warns here) (`ua_core.c:4282-4294`).
F8. DSP rings init as E4 (allocate/program, per-DSP DMA enable and strobe kick, resync) (`ua_core.c:4328`).
F9. Twin X: boot transport stop as E13.

There is **no ring connect for the x8p on the warm path**: the only call site is inside the cold
block (E11) and the deferred call in the audio connect is for AudioExtension devices only
(`ua_core.c:3702-3706`). The Twin X gets its ring connect in phase G.

### 3.7 Phase G: audio init and connect (`ua_audio_init`, `ua_audio.c:9453-9674`)

G1. Software state: locks, service work, period watchdog, hrtimer, completion; sample_rate =
48000; rate_index = 2 (48 kHz); clock_source = 0xC (`ua_audio.c:9460-9484`). Channel model
(`ua_audio.c:9486-9509`, streaming spec).
G2. `ua_audio_preinit_dma` again: a no-op once allocated; the retry if a warm preinit failed
(`ua_audio.c:9510-9520`).
G3. Create and register the ALSA card, proc files, mixer controls (`ua_audio.c:9522-9648`). On
Windows these become the audio endpoints (streaming spec) and the control surface (DSP spec).
G4. Under the device lock, `ua_audio_initial_connect` (`ua_audio.c:9650-9661, 9219-9275`):
`warm_boot=1` (default off) would skip ACEFACE and mark connected; `no_connect=1` (off) skips;
default: `ua_audio_connect`; on success start the 10 ms DSP service loop. A failed connect is not
fatal: "will retry on stream open" (`ua_audio.c:9263-9271`).

G5. `ua_audio_connect` (`ua_audio.c:3000-3401`):
```
if connected: return 0
if aceface_done (Twin X cold): goto post_handshake
for retry in 0..19:                                      ; UA_CONNECT_RETRIES = 20
    if retry == 1: clear_stale (W 0x2260 = 0x10 ; R 0x2218 ; sleep 50 ms)
    W 0xC02C = 0x0ACEFACE
    W 0x2260 = 0x1
    for poll in 0..9:                                    ; UA_CONNECT_POLLS = 10
        s = R 0xC030 ; if s == 0xFFFFFFFF: return -ENODEV
        if s & BIT(21):
            connect config (as E8) ; 95-dword readback ; aceface_done = true ; pcie_setup
            goto post_handshake
        sleep 100 ms                                     ; UA_CONNECT_POLL_MS
aceface_failed = true ; return -ETIMEDOUT                ; ~20 s worst case (derived)
post_handshake:
    s = R 0xC030 ; if s: W 0xC030 = 0
    adopt_fw_rate (default off)
    read both IO descriptors 0xC1A4 / 0xC2C4 (72 dwords) and build the channel map  ; streaming spec
    R 0xC07C (rate info), R 0xC084 (clock info) ; clock-lock bookkeeping
    s = R 0xC08C ; clock_src_mask = s >> 16 ; fw_rate_mask = s & 0xFF
    build the clock-source list (reads 0xC090 names) ; read latency 0xC198/0xC19C
    R 0xC080 (transport info)
    connected = true ; post event UA_EVT_CONNECTED (0x1001)
    ua_kext_ring_connect: Twin X -> ua_dsp_connect_retry (E11 sequence) unless dsp_inherited ; x8p: no-op
    plug-in chain (off) ; bus coefficient replay if pending (DSP spec)
    rate ack (rate_ack_real=1): {0x001A0002, rate_Hz, 0, 0} + doorbell on every DSP with rings,
        rate = the firmware's rate from 0xC07C if known, else sample_rate           ; ua_dsp.c:4196-4214
    mixer init (DSP spec): seq = R 0x380C ; W 0x3808 = seq ; zero caches ; mixer_ready = true ;
        unless dsp_inherited: settings 1..37 except 2 = {0x20, mask 0xFF}, setting 35 = {0x05, 0x1F}, dirty
        SetOutputMute(0) (no-op on these models)
    return 0      ; the transport is NOT started here
```
The connect reads after bit 21 mirror the kext, which "synthetically injects bits 0+1+22" so the
host reads the IO descriptors, rate, clock and transport info; "without these reads, the DSP may not
advance to active audio routing" (`ua_audio.c:3089-3106`). Constants: `ua_apollo.h:957-960`. The
rate ack matters: "a DSP that has not been told the rate does not start an input strip"
(`ua_audio.c:135-138`).

Transport start is deferred to stream open on purpose: starting it during probe/connect "triggers
an immediate PCIe AER error on some Thunderbolt controllers (kernel 6.18+, Maple Ridge), killing
the link within ~50ms" (`ua_audio.c:3366-3377`).

G6. The DSP service loop (`ua_audio.c:2361-2501, 2558-2602`), owned by the DSP spec but a
bring-up dependency: every 10 ms it reads 0xC030 (0xFFFFFFFF stops it: device gone), acts on the
bits and writes 0 last; reads 0xC07C and the transport fault flags; drains the readback
(`R 0x3810 == 1` -> 40 words from 0x3814, `W 0x3810 = 0`); flushes mixer settings after 5 passes.
The readback drain is hypothesised to be the host liveness signal the DSP needs
(`ua_audio.c:2355-2358`, `ua_apollo.h:341-344`).

### 3.8 Phase H: final checks and the control node (`ua_core.c:4933-4969`)

H1. `ua_check_dsps(final)`: same reads as E1; fails probe only with dsp_status_check=2 (default 1
logs). "The healthy values of the two bit groups (low 7 and bit 23) are not known for the x4 yet"
(`ua_core.c:709-725, 4933-4936`).
H2. Register the control node `/dev/ua_apolloN` (N < 8), join the unit group (first unit is master)
(`ua_core.c:4938-4967`, `ua_core.c:880-897`).
H3. Passive interrupt self-test, log only (`ua_core.c:4968`).

### 3.9 State at "ready to start the audio transport"

| Item | Value | Source |
|---|---|---|
| AX_CONTROL 0x2248 | 0 | E13 / connect does not start it |
| TRANSPORT 0x2220 | 0 on a cold start; previous owner's value on a warm start | `ua_core.c:799, 88-91` |
| DMA_CTRL 0x2200 | bit 0 + one enable per DSP, no strobe | E4, E6 |
| IMR 0x2204 | 0 (DSP vectors off) | B1 |
| IMR_HI 0x2268 | 0x1 (notification 0x28) | E6 |
| SG 0x8000 / 0xA000 | 1024 valid entries each | E6 / F7 |
| DSP rings | programmed, live, idle; connect + finalize sent (x8p cold, Twin X always) | E4, E11, G5 |
| NOTIF config 0xC000-0xC024 | {0x0A, 0x17C, 0 x 8} | E8 / G5 |
| 0xC030 | cleared (0) | G5 |
| MIXER_SEQ_WR 0x3808 | = SEQ_RD at connect | G5 |
| Flags | aceface_done = true, connected = true, mixer_ready = true, service loop running | G5 |

What the streaming side expects next (streaming spec owns it): prepare/start writes the
PrepareTransport sequence (BUF_SIZE, DMA_BLKSZ, SAMPLE_POS, FRAME_CFG, channel counts, timer),
enables vector 0x47, arms and starts AX_CONTROL with 0xF by default (`ua_audio.c:113-116`), and if
`connected` is false it retries the connect first (`ua_audio.c:9215-9217`). With kext_connect the
transport start should also send the StartTransport ring notification
`{0x00230002, 0}` per DSP + `{0x00100002, date}` to DSP 0, which the Linux driver documents but has
"not wired up yet" (`ua_dsp.c:4995-5033`).

### 3.10 Firmware and DSP images at this stage

| File | Loaded when | Format | Where from | Default |
|---|---|---|---|---|
| `ua-apollo-mixer.bin` | cold start, mixer_fw_stage=1 | header 16 bytes: +0 magic 0x5541464D 'UAFM', +4 version 1, +8 block count (1..64); then count x 12-byte entries {+0 sram_addr (a captured host address, never used), +4 size (dword multiple), +8 offset (after the table)}; each block sent to DSP 0 with `ua_dsp_send_block(cmd 0x120000, expected response 0x80040000)` | `request_firmware` (`/lib/firmware`); "captured SEL127 'Bill' resource streams", not a DSP boot image | off | 
| `ua-apollo-plugin-chain.bin` | cold start, no_plugins=0 | magic 0x43504155 'UAPC', version 1; DMA_REF payloads captured from the Windows driver | `request_firmware` | off; known to fault the IOMMU and wedge DSP 0 |
| `universal-audio/dsp/<id>.bill` | input_strips=1 after a cold start | UA's Console strip modules from the user's own UA install | `/lib/firmware`; written by `oa-assets firmware` | off; DSP spec |

Sources: `ua_apollo.h:521-555`, `ua_dsp.c:2729-2940`, `ua_core.c:4393-4408`, `ua_audio.c:61-73`,
`ua_strips.c:27-32`, `CHANGELOG.md:198`, `ARCHITECTURE.md:68`. `UA_IOCTL_LOAD_FIRMWARE` lets a
privileged tool push a blob up to 4 MiB through `ua_dsp_load_firmware` (`ua_core.c:3645-3673`); the
kext's own LoadFirmware timeout is 150 s (`ua_apollo.h:527`). Commands 0xD0000/0xE0000 are
licensing loaders and are never sent (`ua_dsp.c:2901-2905`).

For the Windows port: implement no firmware loading for bring-up. If the DSP spec later needs the
strip modules, read them from a driver-relative directory at PASSIVE_LEVEL and keep them in memory
so a cold re-init after resume needs no file access (the Linux driver uses the firmware cache for
the same reason, `ua_dsp.c:1145-1158`).

### 3.11 Timing budget (worst case, derived from the cited loops)

| Step | Worst case | Source |
|---|---|---|
| Liveness check | 30 x 12 ms = 360 ms | `ua_core.c:4218-4223` |
| DSP ready wait (if capability 1) | 30 s (DSP 0) + 3 s per other DSP | `ua_dsp.c:3124-3143` |
| Boot transport + early ACEFACE | ~1.1 s + 50 ms | `ua_audio.c:2650-2651, 2784-2796, 2824` |
| Ring connect (per attempt) | per DSP 20 + 10 ms, + 12 ms; retried once | `ua_dsp.c:510, 3007, 3016, 3046` |
| Readback poll | 50 x 12 ms = 600 ms | `ua_core.c:4516-4522` |
| Full ACEFACE | 20 x 10 x 100 ms = 20 s (+50 ms) | `ua_apollo.h:957-960`, `ua_audio.c:3036-3395` |
| Interrupt self-test | 50 x 1.5 ms | `ua_core.c:4681-4686` |

Linux resume deliberately keeps only the fast part inline and defers the cold re-init and the full
ACEFACE loop to a work item (`ua_core.c:5327-5335, 5372-5381`). The Windows port should do the
same at first start (section 6.2).

---

## 4. Teardown, surprise removal, power

### 4.1 The hardware quiesce (`ua_hw_quiesce_locked`, `ua_core.c:1623-1706`)

Mirrors the kext's CPcieDevice::Shutdown + DisableInterrupts. Caller holds the device lock, has
stopped the service loop and the period timer, and the device is present.

```
1. (state) connected = 0 is applied last
2. SetOutputMute(1) + flush + 50 ms     ; no-op on x8p/Twin X (not output-mute types)   ; 1594-1621
3. disable vector 0x47 ; W 0x2248 = 0 ; transport_running = false
3a. plug-in host teardown (out of scope)
3b. DSP rings quiesce (ua_dsp.c:1067-1114):
      if dsps_connected: date-only disconnect {0x00100002, date} to DSP 0  ; ua_dsp.c:5035-5056
      disarm DSP vectors
      each live ring: wait up to 5 ms for idle (POSITION == write_ptr)
      free orphans the rings are provably past
      resync every ring (DOORBELL = WRITE_PTR = POSITION) ; live = false
4. [full_shutdown=1] ua_reset_dma_engines
5. W 0x2248 = 0 ; [full_shutdown=1] W 0xC02C = 0 ; R 0x2248 ; W 0x22C4 = 0 ; disable vector 0x28
6. W 0x2260 = 0x10 (disconnect doorbell, ALWAYS) ; R 0x2218
7. mask_all(clear_pending): W 0x2204 = 0 ; W 0x2268 = 0 ; W 0x2208 = ~0 ; W 0x2264 = ~0
1. connected = false ; mixer_ready = false
```
Without full_shutdown the steps that would kill the running mixer DSP or force a cold boot are
skipped, so the next start takes the gentle warm path (`ua_core.c:1644-1650`). Step 6 is always
done: without it the audio extension stays connected to the departed host and ignores the next
ACEFACE, and the doorbell alone does not disturb the mixer SEQ counters (`ua_core.c:1690-1698`).

### 4.2 Orderly remove (`ua_remove`, `ua_core.c:4987-5107`)

```
dead = !pci_device_is_present()
pause_set(UA_PAUSE_DEVICE) (event 0x1005) ; leave unit group (next unit becomes master)
0. take ioctl_sem for write, removed = true, release ; post UA_EVT_DEVICE_OFFLINE (arg = dead)
1. cancel resume work ; stop DSP service loop ; cancel period watchdog ; stop period timer   (no lock held)
1b. plug-in host quiesce
2. lock ; if !dead: hw_quiesce (4.1) ; W 0x2220 = 0 ; shutdown = 1 (gate closes) ; unlock
3. if !dead: sleep 100 ms ; pci_clear_master
4. mask caches only (writes are gated) ; free_irq (waits for a running handler) ; free vectors
5. cancel watchdog, timer, service loop again (a racing prepare may have restarted them)
6. audio fini: stop service, stop transport, disconnect, W 0x2248 = 0, W 0x2220 = 0 (gated), 100 ms,
   free the ALSA card (waits for open files), free audio DMA rings under dma_sem   ; ua_audio.c:9676-9735
7. free DSP ring pages ; free parked buffers only if dead or full_shutdown (else leak them, the next
   probe turns bus mastering on again)                                           ; ua_dsp.c:1033-1065
8. free plug-in payloads
9. remove the node; the struct lives until the last open file closes             ; ua_core.c:4593-4604
```

The soft disconnect used by a reconnect (`ua_audio_disconnect_hw`, `ua_audio.c:3403-3457`) differs:
it masks all vectors without clearing pending bits, sleeps 50 ms, and sends the 0x10 disconnect
only if the mixer DSP is not alive (`SEQ_RD > 0 && SEQ_RD == SEQ_WR` keeps it connected), because
"sending UA_AX_DISCONNECT kills the mixer DSP readback".

### 4.3 Surprise removal (Thunderbolt unplug)

- Detection: `pci_device_is_present()` in remove, the quiesce, and the probe unwind
  (`ua_core.c:4990, 5255, 4629`); every polling loop treats a 0xFFFFFFFF read as "device gone":
  identification (`ua_core.c:361, 400`), ISR (`ua_core.c:189`), ring position (`ua_dsp.c:140`),
  ring resync (`ua_dsp.c:533, 543`), ACEFACE (`ua_audio.c:2671, 3056`), DSP ready wait
  (`ua_dsp.c:3130-3136`), service loop (`ua_audio.c:2405-2412`), output mute poll (`ua_core.c:1612`).
- MMIO gate: `atomic shutdown`, set in remove (before the lock is dropped), error_detected,
  `.shutdown`, and a failed slot reset; every `ua_read` then returns 0xFFFFFFFF without touching
  the bus and every `ua_write` is dropped (`ua_apollo.h:1687-1700`). On a dead device remove skips
  the quiesce and the bus-master clear entirely (`ua_core.c:5039-5061`).
- Software stops first, without the lock (the service handler takes it), then the lock, then the
  gate (`ua_core.c:5008-5045`); ioctls in flight are waited out through ioctl_sem and later ones
  get -ENODEV (`ua_core.c:4148-4165`). Hot-unplug while the daemon or PipeWire had the node open
  is a fixed issue (`README.md:93`, `CHANGELOG.md:245`).

### 4.4 AER containment (`ua_core.c:5109-5230`)

- error_detected: gate closed first, then cancel resume work, stop the service loop, plug-in host
  "device lost", connected = transport_running = false, clear the mask caches; permanent failure ->
  DISCONNECT, else NEED_RESET (`ua_core.c:5113-5147`, order pinned by
  `tests/test_aer_containment.py:13-24`).
- slot_reset: re-enable, restore config, read FPGA_REV **past the gate** (raw ioread32); 0xFFFFFFFF
  or a different revision -> DISCONNECT; else under the lock: gate open, ACEFACE flags false,
  stop_xport_irqs (B1), neutralize (B2), set master, then warm init (`ua_dsp_init_and_load(resume,
  warm)`) if the mixer DSP is alive, otherwise defer a cold re-init; resume_pending = true
  (`ua_core.c:5149-5214`, `tests/test_aer_containment.py:27-39`).
- resume: queue the resume work, which does the deferred cold init and the full connect
  (`ua_core.c:5216-5224, 5336-5370`).

### 4.5 Reboot/power-off (`.shutdown`, `ua_core.c:5295-5312`) and suspend/resume

- Common quiesce (`ua_quiesce_device`, `ua_core.c:5245-5293`): cancel resume work; for sleep, the
  audio suspend (controls to D3hot, stop service loop, watchdog, timer) (`ua_audio.c:9277-9298`),
  otherwise stop them directly; plug-in host quiesce; under the lock clear resume flags, hw_quiesce
  if present, aceface_done = aceface_failed = connected = transport_running = false ("ACEFACE does
  not survive the link going down"); then clear bus master if present.
- `.shutdown`: quiesce, then gate closed (`ua_core.c:5308-5311`). The mixer DSP keeps running unless
  full_shutdown=1, so the next boot can take the warm path.
- suspend: pause_set(DEVICE), quiesce(sleep) (`ua_core.c:5314-5325`).
- resume (`ua_core.c:5372-5467`): if gated, only reopen controls. `R 0x2218`: 0xFFFFFFFF -> not
  reachable (hotplug will remove it, return success); a different revision -> -EIO and
  aceface_failed. Else under the lock B1 + B2 + set master; warm init if alive, otherwise defer;
  try a fast connect (early ACEFACE, about 1 s); if it does not answer, the work item runs the full
  20 s connect. Streams come back through a fresh prepare.
- Status: "suspend/resume (untested)" (`README.md:117`); "warm reboot required an Apollo power
  cycle on v1.0.0 ... not yet re-tested" (`README.md:97`).

### 4.6 Probe failure unwind (`ua_probe_unwind_dsp`, `ua_core.c:4606-4651`)

Same order as remove: stop the audio-side software, lock, hw_quiesce, W 0x2220 = 0, gate, unlock,
synchronize the IRQ, sleep 100 ms, clear bus master, then free the audio card, DSP rings, payloads,
audio rings; nothing is freed while bus mastering is on (`tests/test_probe_unwind.py:58-90`).

---

## 5. ioctl ABI (`ua_ioctl.h`)

Node: `/dev/ua_apolloN`, magic 'U' (`ua_ioctl.h:14`), ABI 1.8 (`ua_ioctl.h:548-549`). The node
also serves `read()`/`poll()`/SIGIO events of `struct ua_event {u32 code; u32 arg; u64 time_ns}`
(16 bytes, 64-entry queue per open file) (`ua_ioctl.h:866-985`, `ua_apollo.h:1665-1673`). "Raw"
ioctls need CAP_SYS_RAWIO (`ua_core.c:1868-1895`); every ioctl returns -ENODEV once removed or gated
(`ua_core.c:4158-4163`).

Category: **D** = used by the mixer daemon for basic operation (`mixer-engine/hardware.py`,
`mixer-engine/events.py:136`); **d** = useful for diagnostics, the daemon does not call it;
**T** = raw/debug/tooling; **P** = plug-in hosting, OUT OF SCOPE; **X** = dead.

| nr | Name | Dir | Struct (bytes) | Raw | Handler | What it does | Cat |
|---|---|---|---|---|---|---|---|
| 0x01 | GET_DEVICE_INFO | R | ua_device_info (64) | | `ua_core.c:1177-1195` | identity; fw_version = R 0x0000, serial = R 0x2238, ext_caps = R 0x2234 | D |
| 0x02 | GET_DSP_INFO | RW | ua_dsp_info (32) | | `ua_core.c:1197-1221` | per DSP: bank base, status = (R bank != 0), load words | D |
| 0x03 | GET_TRANSPORT_INFO | R | ua_transport_info (64) | | `ua_core.c:1246-1266` | cached channel counts, frame size, rate, connected/running | d |
| 0x04 | GET_VERSION | R | ua_version (48) | | `ua_core.c:968-1011` | ABI major/minor, features (UA_FEAT_*), features2 | D |
| 0x05 | GET_SYSTEM_INFO | R | ua_system_info (148) | | `ua_core.c:1027-1092` | identity, 16-char serial (0x20-0x2F), fw_release, dev_info, HW UID, clock/lock, link speed/width, unit group | D |
| 0x06 | GET_PAUSE_INFO | R | ua_pause_info (32) | | `ua_core.c:1094-1107` | pause reasons, can_stream | d |
| 0x07 | GET_PLUGIN_CAPS | R | ua_plugin_caps (64) | | `ua_core.c:1113-1142` | plug-in state and operations (0 unless plugin_host=1) | P |
| 0x08 | GET_CLOCK_SOURCES | R | ua_clock_sources (656) | | `ua_core.c:1148-1175` | device clock menu read at connect | D |
| 0x10 | READ_REG | RW | ua_reg_io (8) | no | `ua_core.c:1223-1244` | BAR0 read, offset < BAR size, 4-aligned | D (diagnostics) |
| 0x11 | WRITE_REG | W | ua_reg_io (8) | yes | `ua_core.c:1268-1290` | BAR0 write under the lock | T (daemon fallback only) |
| 0x20 | RESET_DMA | none | - | yes | `ua_core.c:3975-3979` | `ua_reset_dma` | T |
| 0x30 | SET_MIXER_BUS_PARAM | W | ua_mixer_bus_param (16) | | `ua_core.c:3334-3374` | one bus coefficient to the DSP ring | D |
| 0x31 | SET_MIXER_PARAM | W | ua_mixer_param (16) | | `ua_core.c:2974-3270` | preamp/monitor params, bus enable, type 3 = rate ack | D |
| 0x32 | GET_MIXER_PARAM | RW | ua_mixer_param (16) | | `ua_core.c:3272-3332` | bus mask (type 0); preamp query refused | d |
| 0x33 | GET_MIXER_READBACK | RW | ua_mixer_readback (180) | | `ua_core.c:3422-3458` | 38 settings + XOR check words | D |
| 0x34 | SET_DRIVER_PARAM | W | ua_driver_param (16) | | `ua_core.c:3571-3608` | 0 = rate (Hz), 1 = clock source, 0x101-0x105 stored only | D |
| 0x35 | GET_DRIVER_PARAM | RW | ua_driver_param (16) | | `ua_core.c:3610-3639` | 0 rate, 1 source, 2 transport running | D |
| 0x36 | WRITE_MIXER_SETTING | W | ua_mixer_setting (16) | | `ua_core.c:3482-3498` | setting 0-37 into the mailbox | D |
| 0x37 | READ_MIXER_SETTING | RW | ua_mixer_setting (16) | | `ua_core.c:3500-3522` | setting 0-51 | D |
| 0x3c | SET_MIXER_BUS_PARAMS | RW | ua_mixer_bus_params (32) + user pointer to <= 64 x 16-byte entries | | `ua_core.c:3376-3420` | batched bus params, hwid check | d (ABI 1.x feature) |
| 0x40 | CLI_COMMAND | RW | ua_cli_command (264) | yes | `ua_core.c:2966-2969` | always -ENOSYS ("CLI freezes the ARM MCU") | D (stub; the daemon calls it and falls back) |
| 0x41 | GET_HW_READBACK | R | ua_hw_readback (164) | | `ua_core.c:3460-3480` | cached 40-word readback from the service loop | D |
| 0x50 | LOAD_FIRMWARE | W | ua_fw_load (16) + user pointer, <= 4 MiB | yes | `ua_core.c:3645-3673` | blob to DSP 0 | T |
| 0x51 | DSP_CONNECT | none | - | yes | `ua_core.c:3747-3772` | reconnect (AudioExtension: ACEFACE) + ring connect | T |
| 0x52 | DSP_TEST_DMA | none | - | yes | `ua_core.c:4019-4026` | DMA_REF self-test | T |
| 0x53 | DSP_TEST_SEND | none | - | yes | `ua_core.c:4028-4035` | block-send self-test | T |
| 0x54 | DSP_SEND_BLOCK | W | ua_dsp_block (24) + user pointer, <= 4 MiB | yes | `ua_core.c:3774-3813` | `_sendBlock` replay | T |
| 0x60 | SET_ROUTING_TABLE | W | ua_routing_table (24) | - | not dispatched | -ENOTTY | X |
| 0x61 | SEND_ROUTING | none | - | yes | `ua_core.c:4040-4047` | overwrite the IO descriptor (experiment) | T |
| 0x62 | GET_IO_MAP | R | ua_io_map_info (3984) | | `ua_core.c:3841-3862` | descriptors + channel map | d |
| 0x70 | RING_SEND | W | ua_ring_cmd (20) | yes | `ua_core.c:4052-4069` | one inline ring entry (DMA_REF refused) | T |
| 0x71 | LOAD_FW_DSP | W | u32 (4) | yes | `ua_core.c:4071-4077` | retired, -ENOTTY | X |
| 0x72 | DSP_SEND_RAW | W | ua_dsp_block (24) + pointer, <= 4096 | yes | `ua_core.c:4110-4138` | raw DMA ref | T |
| 0x73 | DSP_SRAM_DATA | W | ua_dsp_block (24) + pointer, <= 4096 | yes | `ua_core.c:4079-4108` | SRAM config + data pair | T |
| 0x80 | DMA_TEST | RW | ua_dma_test (1048) | yes | `ua_core.c:3815-3839` | capture canary test | T |
| 0x90 | BOARD_QUERY | RW | ua_board_query (3096) | | `ua_core.c:3933-3934` | authorization table | P |
| 0x94 | PLUGIN_INSTANCE | RW | ua_plugin_instance (88) | | `ua_core.c:3936-3937` | plug-in instances | P |
| 0x95 | PLUGIN_DELAY | RW | ua_plugin_delay (72) | | `ua_core.c:3939-3940` | strip delays | P |
| read() | events | R | ua_event (16) x n | | `ua_event.c:192-249` | events 0x64-0x6f (kext) and 0x1000-0x1007 | D |

Struct definitions and field meaning: `ua_ioctl.h:20-62, 82-103, 132-137, 196-201, 235-258,
268-294, 327-339, 362-368, 385-391, 428-479, 507-515, 596-603, 626-686, 714-719, 786-797,
815-827, 847-862, 1039-1046, 1232-1266`. Sizes: from `mixer-engine/hardware.py:96-203, 246`,
the BUILD_BUG_ONs (`ua_core.c:1118, 1154`), and computed for the rest (derived). Reserved
numbers: 0x09-0x0e never assigned, 0x90-0x9f reserved for plug-in hosting (`ua_ioctl.h:536-547`).

Daemon calls found in `mixer-engine/hardware.py`: 0x01, 0x02, 0x04, 0x05, 0x08, 0x10, 0x11, 0x30,
0x31, 0x33, 0x34, 0x35, 0x36, 0x37, 0x40, 0x41 (and 0x90 for plug-ins). WRITE_REG and the
register-level mixer path are only a fallback when WRITE_MIXER_SETTING returns ENOTTY
(`mixer-engine/hardware.py:1590-1612`). The plug-in modules call 0x94/0x95
(`mixer-engine/plugin_driver.py:17-20`).

Windows notes for this ABI (section 6 has the mechanics): keep the struct layouts byte for byte
(all fixed size, 64-bit pointers stored in `__u64`, so 32- and 64-bit callers share them); define
IOCTL codes with `CTL_CODE(<device type>, 0x800 + nr, METHOD_BUFFERED, <access>)`; the four ioctls
with embedded user pointers (0x3c, 0x50, 0x54, 0x72/0x73) need either caller-context probing or a
flattened variable-length input buffer.

---

## 6. Linux to Windows mapping

### 6.1 Facility table

The "Used at" column is from the source. The KMDF/WDM and gotcha columns are Windows-side
knowledge, not from the source: check API availability (KMDF version, minimum Windows build)
against the WDK you build with.

| Linux facility | Used at | KMDF / WDM equivalent | Gotchas |
|---|---|---|---|
| `pci_register_driver`, id table, `MODULE_DEVICE_TABLE` | `ua_core.c:146-151, 5483-5512` | `DriverEntry` + `WdfDriverCreate`; INF `PCI\VEN_1A00&DEV_0002...`; `EvtDriverDeviceAdd` | Match DEV_0002 only. Ranking vs UA's own signed INF (section 7, R4). |
| `probe` / `remove` | `ua_core.c:4697, 4987` | `EvtDriverDeviceAdd`, `EvtDevicePrepareHardware`, `EvtDeviceD0Entry(PostInterruptsEnabled)`, `EvtDeviceD0Exit`, `EvtDeviceReleaseHardware`, object cleanup | Split per section 6.2. |
| `pcim_enable_device` | `ua_core.c:4734` | done by pci.sys at start | (Windows) pci.sys sets memory decode and bus mastering when it starts the device, before `EvtDevicePrepareHardware`. |
| `pci_set_master` / `pci_clear_master` | `ua_core.c:4868, 5060, 4644`, `5292` | no function-driver API; pci.sys owns the Command register | The Linux "quiesce before BME" order cannot be reproduced: do phase B as the first MMIO in PrepareHardware. A Command-register write through `BUS_INTERFACE_STANDARD.SetBusData` is possible but fights pci.sys (Windows; verify before using). |
| `dma_set_mask_and_coherent(32)` | `ua_core.c:4757` | `WdfDmaEnablerCreate` with `WdfDmaProfileScatterGather` (32-bit) | Use the 32-bit profile so common-buffer logical addresses are below 4 GiB. The 64-bit profile only if dma64_audio is ever validated. |
| `dma_alloc_coherent` (DSP ring pages, 4 KiB) | `ua_dsp.c:80-86` | `WdfCommonBufferCreate` (or `...WithConfig` with alignment); `WdfCommonBufferGetAlignedLogicalAddress` for the device, `...GetAlignedVirtualAddress` for the CPU | A 16 KiB common buffer per ring is fine (pages consecutive). Allocate in PrepareHardware, keep across D-states. |
| `dma_alloc_pages` + `vmap` (audio rings) | `ua_audio.c:518-556` | streaming spec: common buffer(s) or MDL pages mapped through the DMA adapter | Device addresses must come from the DMA adapter (logical), never `MmGetPhysicalAddress`/raw PFNs, or DMA remapping breaks (Windows). |
| `pci_resource_len`, `pcim_iomap`, `ioread32`/`iowrite32` | `ua_core.c:4764-4779`, `ua_apollo.h:1688-1700` | translated `CmResourceTypeMemory` descriptor in PrepareHardware; `MmMapIoSpaceEx(PAGE_READWRITE|PAGE_NOCACHE)`; `READ_REGISTER_ULONG` / `WRITE_REGISTER_ULONG`; `MmUnmapIoSpace` in ReleaseHardware | Keep the lock-free shutdown gate in front of every access. |
| `pdev->device`, `subsystem_device` | `ua_core.c:4578-4581, 4783` | `BUS_INTERFACE_STANDARD.GetBusData` (config 0x02, 0x2E) via `WdfFdoQueryForInterface`, or parse the hardware ID | |
| `pci_alloc_irq_vectors(1, 2, MSI|MSIX)` / INTx fallback | `ua_core.c:4879-4904` | INF `Interrupt Management\MessageSignaledInterruptProperties: MSISupported=1, MessageNumberLimit=1`; `WdfInterruptCreate`; line-based fallback is automatic | Only vector 0 is used; limiting to 1 message avoids having to create a WDFINTERRUPT per granted message (Windows; verify). |
| `request_threaded_irq` (hard + thread) | `ua_core.c:4908-4914, 159-245` | `EvtInterruptIsr` (DIRQL) + `EvtInterruptDpc` via `WdfInterruptQueueDpcForIsr`; passive work via `WdfInterruptQueueWorkItemForIsr` | The ISR may read/write BAR0 (Linux does). For INTx return FALSE when nothing enabled is pending. |
| `spin_lock_irqsave(&irq_lock)` around IMR/ISR | `ua_apollo.h:1792-1864`, `ua_core.c:170` | `WdfInterruptAcquireLock` / `WdfInterruptSynchronize` | Inside the ISR the interrupt lock is already held: call lock-free variants of enable/disable_vector from ISR context (Linux re-takes irq_lock from `ua_audio_irq`, `ua_audio.c:6848-6851`). |
| `synchronize_irq`, `free_irq` | `ua_core.c:4642, 5069-5070` | `WdfInterruptSynchronize` with an empty callback; KMDF disconnects at D0Exit/remove | |
| `mutex` (`ua->lock`) | throughout | `WDFWAITLOCK` (PASSIVE) | All MMIO polling with sleeps runs under it: PASSIVE only. |
| `rw_semaphore ioctl_sem` + `removed` | `ua_core.c:5000-5005, 4158-4163` | rundown protection (`ExAcquireRundownProtection`) or stop the queue (`WdfIoQueueStopSynchronously`) before teardown | Return STATUS_NO_SUCH_DEVICE (proposal) where Linux returns -ENODEV. |
| `atomic_t shutdown` | `ua_apollo.h:1629` | `volatile LONG` with `InterlockedExchange` / `ReadAcquire` | Set it in `EvtDeviceSurpriseRemoval` and at the start of D0Exit to D3Final. |
| `work_struct` on `system_freezable_wq` (resume work) | `ua_core.c:4723, 5223, 5453` | `WDFWORKITEM`; `WdfWorkItemFlush` = `cancel_work_sync` | Queue the slow connect from D0EntryPostInterruptsEnabled. |
| `delayed_work` 10 ms service loop | `ua_audio.c:2361-2501` | periodic `WDFTIMER` that queues a passive work item, or a system thread with `KeDelayExecutionThread` | (Windows) default timer resolution is about 15.6 ms; a 10 ms cadence needs a high-resolution timer (`WDF_TIMER_CONFIG.UseHighResolutionTimer`, KMDF 1.13+, or `ExAllocateTimer(EX_TIMER_HIGH_RESOLUTION)`). |
| `hrtimer` (period timer) | `ua_audio.c:9468-9476` | streaming spec | |
| `msleep`, `usleep_range` | throughout | `KeDelayExecutionThread` (PASSIVE); `KeStallExecutionProcessor` for short spins | Sleeps round up to the timer tick: the "1-2 ms" ARM-to-START gap (`ua_audio.c:2775-2784`) becomes ~16 ms (longer is safe there); poll loops bounded by a count become much longer, so bound them by elapsed time instead. |
| `request_firmware`, `firmware_request_cache` | `ua_dsp.c:2877, 1159-1173` | `ZwCreateFile`/`ZwReadFile` at PASSIVE from a driver-relative directory (`IoGetDriverDirectory`, Win10 2004+) | Not needed with the defaults. |
| `wait_event`, `wake_up`, `kfifo`, `poll`, `fasync` (event channel) | `ua_event.c:30-256` | inverted call: pend a "wait event" IOCTL (or READ) in a manual queue, complete it when an event is posted | Per-open-file queue of 64, EVENTS_LOST semantics (`ua_ioctl.h:873-879`). |
| `completion` | `ua_apollo.h:1253` | `KEVENT` | |
| `cdev`, `class`, `ida`, `/dev/ua_apolloN` | `ua_core.c:4943-4965, 5497-5505` | `WdfDeviceCreateDeviceInterface` with a private GUID; user mode enumerates instances | |
| `unlocked_ioctl`, `_IOR/_IOW/_IOWR`, `compat_ptr_ioctl` | `ua_core.c:4148-4178` | `EvtIoDeviceControl`, `CTL_CODE(..., METHOD_BUFFERED, ...)`; `WdfRequestIsFrom32BitProcess` for WOW64 | |
| `copy_from_user` / `copy_to_user` | handlers | `WdfRequestRetrieveInputBuffer` / `...OutputBuffer` | Embedded pointers (0x3c, 0x50, 0x54, 0x72, 0x73) need `EvtIoInCallerContext` + `WdfRequestProbeAndLockUserBufferForRead`, or redesign. |
| `capable(CAP_SYS_RAWIO)` | `ua_core.c:4154-4155` | admin-only device interface (SDDL) for the raw set, or compile them out of release builds | The daemon's set must stay reachable by the desktop user (`CHANGELOG.md:217`). |
| `module_param` | `ua_core.c:73-131` etc. | registry values (`WdfDriverOpenParametersRegistryKey`, `WdfDeviceOpenRegistryKey`) | Implement the defaults only (Appendix A). |
| `dev_info` / `dev_dbg` | throughout | WPP or TraceLogging | Keep the probe banner shape. |
| `kzalloc`, `kvmalloc` | throughout | `ExAllocatePool2(POOL_FLAG_NON_PAGED, tag)` | |
| `ktime_get_ns`, `jiffies` | throughout | `KeQueryInterruptTimePrecise`, `KeQueryPerformanceCounter` | |
| `ktime_get_real_seconds` (connect date) | `ua_dsp.c:779-790` | `KeQuerySystemTimePrecise`; seconds since 2007-01-01 UTC = (t - 128120832000000000) / 10^7 | The constant is the FILETIME of 2007-01-01 00:00 UTC (derived). |
| `wmb()` before the doorbell | `ua_dsp.c:753-754` | `KeMemoryBarrier()` | Ring entries live in common-buffer memory written by the CPU, the doorbell is MMIO. |
| `cpu_to_le32` | ring entries | no-op on x64/ARM64 | |
| `pci_device_is_present` | `ua_core.c:4629, 4990, 5255` | config read of vendor ID through `BUS_INTERFACE_STANDARD` (0xFFFF = gone), plus `R 0x2218 == 0xFFFFFFFF` | |
| `pci_error_handlers` (error_detected / slot_reset / resume) | `ua_core.c:5109-5230` | no KMDF equivalent; PCIe errors go to WHEA; unplug arrives as surprise removal; `GUID_DEVICE_RESET_INTERFACE_STANDARD` exists for driver-requested resets (Windows; verify) | Treat any all-ones read as device loss; no in-driver recovery path. |
| `pci_restore_state` | `ua_core.c:5173` | pci.sys restores config on D0 | |
| `pci_disable_link_state(L0S)` | `ua_audio.c:3937-3944` | no function-driver API (Windows) | Options to evaluate: leave it to the OS ASPM policy; a LNKCTL write via `SetBusData` (pci.sys may rewrite it on D-state changes). Section 7, R9. |
| `pcie_capability_read_word(LNKSTA)` | `ua_core.c:1080` | walk the capability list via `GetBusData` | |
| `.shutdown` | `ua_core.c:5301-5312` | D0Exit on system shutdown (Windows; verify the TargetState seen, and Fast Startup = hibernate) | |
| `dev_pm_ops` suspend/resume | `ua_core.c:5314-5467` | `EvtDeviceD0Exit(WdfPowerDeviceD3)` / `EvtDeviceD0Entry(PreviousState)` | Do not enable S0 idle (`WdfDeviceAssignS0IdleSettings`): Linux has no runtime PM for the device. |
| ALSA (`snd_card_*`) | `ua_audio.c:9522-9648` | ACX or PortCls/WaveRT | Streaming spec. |

### 6.2 Proposed KMDF callback placement (design proposal)

1. **EvtDriverDeviceAdd**: PnP/power callbacks; buffered I/O; no S0 idle; `WdfInterruptCreate`
   (ISR + DPC); `WdfDmaEnablerCreate` (32-bit scatter/gather profile); device interface; IOCTL
   queue; wait lock; work items; service timer.
2. **EvtDevicePrepareHardware**: map BAR0 (check >= 0xC500); **phase B first** (B1, B2), since bus
   mastering is already on; then phase A identification (1.2); allocate the DSP ring common buffers
   (count from ext_caps) and the audio rings (streaming spec). Fail with
   STATUS_DEVICE_NOT_CONNECTED on an all-ones FPGA_REV.
3. **EvtDeviceD0Entry(PreviousState)**: re-check FPGA_REV (resume: 0xFFFFFFFF = gone, mismatch =
   fail); phase B again (cheap, matches Linux resume, `ua_core.c:5414-5422`); phase D (<= 360 ms);
   warm path F (fast). For a cold start, only E1 here; the rest (E3 onwards, which can take 30 s
   or more) goes to a work item. Keep IMR/IMR_HI at 0 here; enable vector 0x28 only after
   interrupts are connected (step 4). Rationale: KMDF enables interrupts after D0Entry returns.
4. **EvtDeviceD0EntryPostInterruptsEnabled**: queue the bring-up work item: the rest of E (or
   nothing for warm), then phase G (ACEFACE, ring connect, rate ack, mixer init), then start the
   service timer; publish "connected" so the audio endpoints can start streams; until then stream
   start fails busy, as Linux pcm_prepare does with `resume_pending` (`ua_core.c:5334`).
5. **EvtDeviceD0Exit(TargetState)**: stop the service timer and flush the work item (outside the
   wait lock), then under the lock section 4.1 if present; for D3Final also `W 0x2220 = 0`, close
   the gate, sleep 100 ms (`ua_core.c:5039-5061`). For D3 (sleep) follow `ua_quiesce_device`
   (`ua_core.c:5251-5293`) and reset aceface/connected flags.
6. **EvtDeviceSurpriseRemoval**: close the gate, stop the service timer, post DEVICE_OFFLINE
   (arg 1), fail pending event waits.
7. **EvtDeviceReleaseHardware**: free common buffers (only after the quiesce, or when the device is
   gone; parked buffers follow `ua_dsp.c:1033-1065`), unmap BAR0.

---

## 7. Open questions and risks

**R1 (high): silent microphone.** Analog record channels read zero after a driver cold start until
input strips exist (`ua_strips.c:7-12`); the driver can only build them for the x4 and Twin X
Gen 2, and only with input_strips=1 (`ua_strips.c:761-800`, `README.md:133`). The x8p is not in the
strip table, yet it is "verified" with "all 8 preamps discrete over ALSA" (`devices/apollo-x8p.json:14`):
the source does not say whether that was after a cold start or with strips inherited from another
OS. A power cycle "leaves the analog inputs silent until input strips exist ... otherwise Console
on macOS/Windows does, and main keeps them on a warm switch" (`HARDWARE-SESSIONS.md:81-84`). The x8p
warm path sends no ring connect (section 3.6), so strips built by UA's Windows driver should
survive a takeover (inference, untested); but the connect's mixer init still rewrites settings
1-37 (except 2) to 0x20/0xFF on the x8p, because `dsp_inherited` is x4-only, and those settings
carry preamp gain, HiZ impedance and knob-step fields (`ua_audio.c:3289-3308`). The Twin X
ring-connects on every connect (`CHANGELOG.md:50-52`), so a Twin X Gen 1 would lose inherited
strips and cannot rebuild them. The DSP spec owns the fix; bring-up must at least keep the x8p
warm path free of a ring connect and never set full_shutdown.

**R2 (high): DMA before quiesce on Windows.** Linux enables bus mastering only after stopping the
transport, masking vectors and neutralizing rings (`ua_core.c:4740-4745, 4858-4868`). On Windows
bus mastering is already on when the driver first runs (Windows). If the previous owner left the
transport running or a ring target ahead of its position, the device can DMA into memory it no
longer owns in the window before phase B. Mitigation: phase B as the first MMIO. With DMA
remapping on, such stale DMA faults in the IOMMU instead, which on Linux became a fatal PCIe error
(`ua_core.c:4348-4355`).

**R3 (high): Kernel DMA Protection / DMA remapping.** Thunderbolt PCIe devices on Windows 11 are
subject to Kernel DMA Protection; drivers declare DMA-remapping compatibility in the INF
(`DmaRemappingCompatible`) and must give the device only DMA-adapter logical addresses (Windows;
verify the exact INF directive). Every address the Linux driver hands the FPGA is a DMA API
address (SG, ring pages, DMA references), so the port maps cleanly, but any physical address
(e.g. from `MmGetPhysicalAddress` or WaveRT page helpers that return physical addresses) will fault.
An IOMMU fault on this device was catastrophic on Linux (section 2.6).

**R4 (high): replacing UA's driver on the same function.** (a) Driver selection: UA's package is
signed and installed; Windows ranks signature class before hardware-ID specificity, so a
test-signed driver may lose to UA's until UA's package is removed (`pnputil /delete-driver`)
(Windows; verify). (b) Device state UA's driver may leave: transport running (`ua_core.c:841-849`),
TRANSPORT non-zero (`ua_core.c:88-91`), SG entries and ring pages pointing at UA's freed buffers
(`ua_core.c:4282-4288`, `ua_dsp.c:1116-1128`), a host connection that ignores ACEFACE until the
0x10 disconnect doorbell (`ua_audio.c:2608-2615`), DSP state including Console strips
(`ua_apollo.h:1541-1548`), plug-in resources in the DSP pools (`ua_plugin_place.c:122-125`). The
Linux bring-up handles each of these (phase B, SG reprogram, clear_stale, warm path); keep all of
them. (c) Evidence of UA's Windows behaviour in the source: its INF ships MSISupported=1
(`ua_core.c:121-127`); it sends the same connect date (`ua_dsp.c:769-776`); it uses FRAME_CFG
0x0B/0x16/0x2C where the kext uses 8/0x10/0x20 (`ua_audio.c:147-150`); it clocks off the
end-of-buffer vector (`ua_audio.c:3514-3516`); the captured plug-in chain contains Windows host
physical addresses 0x9_492Cxxxx..0x9_493Fxxxx (`ua_apollo.h:544-552`), so captures from it must
never be replayed with raw addresses.

**R5 (medium): long blocking steps.** DSP ready wait up to 30 s + 3 s per DSP, full ACEFACE up to
20 s (section 3.11). Running these in PnP/power callbacks stalls boot/resume and risks the power
watchdog (Windows). Defer them (section 6.2).

**R6 (medium): audio ring ownership crosses specs.** The SG tables must point at valid, mapped
memory whenever the transport could run, including the Twin X boot transport before any stream
exists (`ua_core.c:4364-4383`), and buffers may only be freed after the transport stopped plus
100 ms (`ua_audio.c:9688-9696`). The Linux rings live from probe to remove. If the Windows audio
layer allocates per stream, it must keep the SG table valid at all times or keep a permanent
pair of rings.

**R7 (medium): Twin X Gen 1 is unverified end to end.** Unknown: subsystem ID (a value equal to
0x0006 or 0x000F would pin the wrong model, `ua_core.c:435-441`), serial digits 5-8 (a value that
matches another table entry other than 2024/2025 would misname it, `ua_core.c:653-680`), DSP count,
ext_caps[7:4], and whether the notification bank shift 0x0A (documented "for Apollo x4") applies
(`ua_apollo.h:397`). Gen 2 is verified only on a Duo (serial 2090) at 48 kHz.

**R8 (medium): ordering and comment ambiguities.**
- x8p: ring connect before ACEFACE (verified on Linux) vs the kext's ACEFACE first; v12_connect=1
  reorders but is "EXPERIMENTAL, needs ... x8p testers" (`ua_core.c:98-101, 3728-3733`). Follow the
  verified Linux order.
- "Clearing aceface_done forces a re-ACEFACE that can't succeed because transport is stopped"
  (`ua_audio.c:2837-2843`) vs the warm path, which runs the full ACEFACE with the transport stopped
  and works on the x4 (`HARDWARE-SESSIONS.md:99`). Unclear whether AudioExtension ACEFACE needs a
  running transport only on a cold DSP.
- `ua_core.c:4356-4362` calls the Twin X a ring-connect device (old numbering, section 3.0).
- DSP vector map: `ua_apollo.h:1736-1739` says extended mode maps DSP n to HW bits n*4..n*4+3,
  `ua_dsp_vec_hwbit` uses n*5+j for <= 4 DSPs (`ua_dsp.c:4714-4722`); matters only with dsp_msg_irq.
- "6016d4e (reset strobe re-clear)" (`devices/apollo-x8p.json:14`) is not in this shallow clone;
  its identification with dma_per_dsp is an inference from `ua_dsp.c:792-801, 914-927` and
  `CHANGELOG.md:226`, both of which cite issue #46 and the 0x1ffff readback.

**R9 (medium): ASPM L0s.** Linux turns L0s off on the Apollo link because of correctable error
storms while streaming (`ua_audio.c:3928-3944`). A Windows function driver has no supported
equivalent, and Windows' PCIe link power management policy may enable L0s/L1 (Windows). Expect
correctable AER noise; evaluate a LNKCTL write and the power-plan setting on the target.

**R10 (medium): no programmatic recovery from a wedged DSP.** No DSP hard reset, FLR or bus reset
exists in the driver (`ua_core.c:1634-1636`; 0x221C never written). Every documented wedge (plug-in
chain, DMA_REF to 0, too-fast rate changes, prepare churn) needed an Apollo power cycle
(`CHANGELOG.md:198, 200, 222`, `ua_apollo.h:1384-1393`, `HARDWARE-SESSIONS.md:78-88`). The Windows
driver should report "power-cycle the Apollo" rather than retry blindly.

**R11 (medium): surprise removal costs.** An MMIO read on a dead link can stall up to the PCIe
completion timeout (`ua_apollo.h:1789-1791`, `tests/test_mmio_shutdown_gate.py:1-9`); poll loops
must check the gate and all-ones each iteration. Whether a link-down on this TB path becomes a
WHEA bugcheck or a clean surprise removal on Windows depends on platform AER/DPC support (Windows;
test). Linux needed udev rules to keep desktop TB add-in card ports out of D3cold for hot-plug to
work (`README.md:98`); Windows TB controller power management may need the same attention.

**R12 (low): BAR0 size and DSP count unconfirmed.** Only the 0x2800 minimum is in the source;
offsets reach 0xC4FF. The x8p's 6 DSPs come from an example banner (`tools/contribute/device-probe.sh:147`).

**R13 (low): DSP status health unknown.** Fault mask 0x0080007F healthy values are not known even
for the x4 (`ua_core.c:713-718`); keep it log-only.

**R14 (low): MSI message count.** Linux allows 1-2 vectors and uses one (`ua_apollo.h:901-902`,
`ua_core.c:4908`). Limit to one message in the INF.

**R15 (low): suspend/resume and warm reboot are untested on Linux** (`README.md:97, 117`), and
Windows Fast Startup turns shutdown into hibernation (Windows), so the "resume" path (FPGA_REV check,
warm/cold decision) runs on what users think is a cold boot. The Linux logic for this is complete;
test it explicitly.

**R16 (info): never port these.** The ARM CLI (freezes the MCU, `ua_core.c:1321-1327`), the mixer
firmware stage and the plug-in chain (section 3.10), full_shutdown as a default (forces a cold DSP
boot next time and loses Console state, `HARDWARE-SESSIONS.md:80`), and SET_ROUTING_TABLE /
LOAD_FW_DSP (dead).

---

## Appendix A: module parameters that change the bring-up (implement the defaults)

| Parameter | Default | Effect when changed | Source |
|---|---|---|---|
| full_shutdown | off | remove/shutdown/suspend also reset DMA engines, clear ACEFACE magic; next start is cold | `ua_core.c:73-76` |
| warm_dma_reset | off | warm start also TRANSPORT=0 + kext engine reset | `ua_core.c:78-81` |
| warm_transport0 | off | TRANSPORT=0 on every start | `ua_core.c:88-91` |
| mixer_fw_stage | off | cold start sends ua-apollo-mixer.bin | `ua_core.c:83-86` |
| kext_connect | **on** | ring connect chosen by public type; 0 = by serial type | `ua_core.c:93-96` |
| v12_connect | off | v12 ring-connect set, ACEFACE before ring connect | `ua_core.c:98-101` |
| mixer_mailbox | on | DSP spec | `ua_core.c:116-119` |
| satellite_msi | on | Satellite only | `ua_core.c:128-131` |
| dsp_status_check | 1 (log) | 2 fails probe on DSP fault bits | `ua_core.c:719-722` |
| irq_selftest | on | passive log | `ua_core.c:4669-4672` |
| dma_per_dsp | **on** | 0 = old all-bits OR (the issue #46 bug) | `ua_dsp.c:798-801` |
| dsp_ready_wait | 1 | 2 = every v2 device, 0 = off | `ua_dsp.c:3078-3081` |
| dsp_msg_irq | 0 | DSP vectors | `ua_dsp.c:4708-4711` |
| no_plugins | **on** | 0 sends the captured plug-in chain | `ua_audio.c:65-67` |
| no_connect / warm_boot | off | skip ACEFACE (debug / "warm boot from Windows/macOS") | `ua_audio.c:53-59` |
| pcie_tuning | off | ASPM off + completion timeouts on the chain | `ua_audio.c:80-83` |
| dma64_audio | off | audio rings above 4 GiB | `ua_audio.c:91-94` |
| rate_ack_real | on | rate ack at connect | `ua_audio.c:135-138` |
| notif_irq | off | service pass from the notification vector | `ua_audio.c:139-142` |
| io_map | -1 | layout per model (streaming spec) | `ua_audio.c:48-51` |
| input_strips | off | build input strips (DSP spec, R1) | `ua_strips.c:80-82` |
| plugin_host | off | plug-in hosting (out of scope) | `ua_plugin_host.c:26-29` |
