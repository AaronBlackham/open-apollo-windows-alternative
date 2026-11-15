# Apollo Thunderbolt audio streaming: specification for the Windows port

Scope: the audio streaming path only (DMA rings, transport, position, timing, channel layout, the
ALSA surface and how it maps to ACX/WaveRT). PCIe bring-up (`ua_core.c`) and the DSP/mixer layer
(`ua_dsp.c`, `mixer-engine/`) belong to other specs; they are described here only where streaming
depends on them, and each such place is marked **[dep]**.

Source: `apollo-win/reference/open-apollo` (shallow clone, read only, never built or run).

## 0. Conventions

* **Citations** are `file:line` relative to `reference/open-apollo/`. Files in `driver/` are cited
  without the `driver/` prefix (`ua_audio.c:3726` means `driver/ua_audio.c` line 3726).
* **Tags** on statements that are not plain "the code does X":
  * **[HW]** the source says this was verified on hardware (which unit is named).
  * **[INF]** my inference from the source; not stated by it. Treat as a hypothesis to test.
  * **[WIN]** Windows-side design note. Not from the Linux source; from general ACX/WaveRT
    knowledge. Verify names against the WDK headers before coding.
* All registers are 32-bit little-endian MMIO at a BAR0 byte offset, accessed with
  `ioread32`/`iowrite32` (`ua_apollo.h:1687-1700`). A read of `0xFFFFFFFF` means the device has left
  the bus; the driver checks for it on every path that matters (`ua_audio.c:2405-2412`,
  `ua_audio.c:6032-6058`, `ua_audio.c:4097-4103`).
* Device types used here: x8p `0x22` (`ua_apollo.h:91`); Twin X Gen 1 `0x99` = `0x80 | public 0x19`
  (`ua_apollo.h:127`); Twin X Gen 2 `0x3B`/`0x3C` (`ua_apollo.h:102-103`), TB4 variants `0x40`/`0x41`
  (`ua_apollo.h:106-107`); all four Gen 2 types use one model table (`ua_models_gen.h:1143-1147`).
* Rate indexes: 1 = 44.1k, 2 = 48k, 3 = 88.2k, 4 = 96k, 5 = 176.4k, 6 = 192k (`ua_apollo.h:939-945`).

### 0.1 Register quick reference (streaming subset)

| Offset | Name | Role in streaming | Cite |
|---|---|---|---|
| 0x2200 | DMA_CTRL | bit 0 global DMA enable, bits [8:1] per-engine enables, bits [16:9] reset strobes (v2) | `ua_apollo.h:270`, `ua_apollo.h:563-568` |
| 0x2204 / 0x2208 | IMR / ISR | interrupt mask / status for HW bits 0-31; ISR is write-1-to-clear | `ua_apollo.h:271-272`, `ua_apollo.h:1805` |
| 0x2264 / 0x2268 | ISR_HI / IMR_HI | HW bits 32-63 (audio vectors live here) | `ua_apollo.h:280-281`, `ua_apollo.h:1741-1747` |
| 0x2218 | FPGA_REV | liveness check and posted-write flush read | `ua_apollo.h:265`, `ua_apollo.h:1813-1814` |
| 0x2220 | TRANSPORT | written 0 at cold init and teardown only; not the stream start | `ua_apollo.h:277`, `ua_core.c:798-799`, `ua_audio.c:9694-9696` |
| 0x2240 | AX_BUF_SIZE | ring length in frames minus 1 | `ua_apollo.h:287` |
| 0x2244 | AX_SAMPLE_POS | hardware frame position in the ring (both directions) | `ua_apollo.h:288` |
| 0x2248 | AX_CONTROL | 0 stop, 1 arm, 0xF start (0x20F peer-clocked); bits 7/8 fault flags | `ua_apollo.h:289`, `ua_apollo.h:346-353`, `ua_audio.c:2041-2079` |
| 0x224C | AX_P2P_ROUTE | `0x100 \| (play_ch - 1)`, legacy layout only | `ua_apollo.h:290`, `ua_audio.c:3807-3820` |
| 0x2250 | AX_PLAY_CH | playback slots per frame (stride / 4) | `ua_apollo.h:291` |
| 0x2254 | AX_FRAME_CTR | counts up to FRAME_CFG after arm (start gate) | `ua_apollo.h:292`, `ua_audio.c:3826-3855` |
| 0x2258 | AX_FRAME_CFG | safety offset in frames per rate band | `ua_apollo.h:293`, `ua_audio.c:1937-1950` |
| 0x225C | AX_REC_CH | record slots per frame | `ua_apollo.h:294` |
| 0x2260 | AX_CONNECT | doorbell: 1 connect, 4 clock change, 0x10 disconnect | `ua_apollo.h:295`, `ua_apollo.h:355-359` |
| 0x226C | AX_DMA_BLKSZ | `((frames - 1) * play_ch) >> 10` | `ua_apollo.h:296`, `ua_audio.c:3729-3731` |
| 0x2270 | AX_TIMER_CFG | period-timer interval in frames minus 1 (only with `period_irq=1`) | `ua_apollo.h:297`, `ua_audio.c:3505-3547` |
| 0x22C0 | AX_STATUS | read fence after arm; read once when fault bit 7 appears | `ua_apollo.h:299-302`, `ua_audio.c:3804-3805` |
| 0x22C4 | AX_P2P_ENABLE | peer routes; always 0 here | `ua_apollo.h:304-307` |
| 0x8000 + 8i | play SG entry i (lo, hi) | i = 0..1023, write-only | `ua_apollo.h:362`, `ua_audio.c:719-732` |
| 0xA000 + 8i | rec SG entry i (lo, hi) | i = 0..1023, write-only | `ua_apollo.h:363` |
| 0xC030 | NOTIF_STATUS | notification bits; cleared by writing 0 (not W1C) | `ua_apollo.h:402`, `ua_audio.c:2392-2403` |
| 0xC074 | NOTIF_CLOCK_CFG | `(rate_index << 8) \| clock_source` | `ua_apollo.h:403`, `ua_audio.c:4290-4301` |
| 0xC07C | NOTIF_RATE_INFO | rate index in [15:8], lock state in [1:0] (1 = locked) | `ua_apollo.h:404`, `ua_audio.c:2000-2039` |
| 0xC08C | NOTIF_CLOCK_SRCS | [31:16] offered clock sources, [7:0] firmware rate mask | `ua_apollo.h:407-408`, `ua_audio.c:3133-3138` |
| 0xC198 / 0xC19C | latency words | firmware latency out / in (interpretation uncertain) | `ua_apollo.h:935-937` |
| 0xC1A4 / 0xC2C4 | IO descriptors | 72 words each: record (input) / playback (output) slot map | `ua_apollo.h:420-452` |

## 1. DMA buffer architecture

### 1.1 Allocation

* Two rings, one per direction, each exactly `UA_DMA_BUF_SIZE = 0x400000` (4 MiB)
  (`ua_apollo.h:365`).
* Each ring is allocated as separate kernel pages (`dma_alloc_pages`, 1024 pages at 4 KiB), not one
  coherent block, and `vmap()`ed into one flat CPU view (`ua_audio.c:468-495`, `ua_audio.c:518-556`).
  Reason given: a single 4 MiB physically contiguous allocation below 4 GiB failed with -ENOMEM after
  hours of uptime on a host without an IOMMU (`ua_audio.c:471-479`). The FPGA walks a 1024-entry SG
  table of 4 KiB pages, "so the device never needed contiguity" (`ua_audio.c:478-480`).
* Pages are zero-filled at allocation (`ua_audio.c:545`). Both rings are mapped
  `DMA_BIDIRECTIONAL` (`ua_audio.c:493-497`). The memory is non-coherent by contract; the code syncs
  ranges for device/CPU, which "on cache-coherent x86 both are no-ops" (`ua_audio.c:489-492`,
  `ua_audio.c:558-616`).
* Rings are device-lifetime: allocated once at probe (before any transport start) and never
  reallocated per stream (`ua_audio.c:811-845`, `ua_audio.c:4802-4807`).
* DMA addressing: the device is kept at a 32-bit DMA mask because "a buffer at 0x19f800000 failed"
  and the FPGA "may truncate" (`ua_core.c:4747-4761`, `ua_audio.c:85-90`). The source notes Windows
  and macOS never constrain the range and the SG table carries a high dword (`ua_audio.c:86-90`); a
  64-bit experiment (`dma64_audio`) exists, default off (`ua_audio.c:91-94`, `ua_audio.c:670-705`).
* Alignment: SG pages are 4 KiB (`UA_PCIE_PAGE_SIZE = 0x1000`, `ua_apollo.h:558`); entry i must
  point at a 4 KiB aligned page. Frames straddle page boundaries freely (the FPGA sees one flat
  4 MiB ring through the table).

**[WIN]** Allocate each ring as WDF common buffer(s) from a DMA enabler limited to 32-bit
addresses. Either one 4 MiB contiguous common buffer per direction (simplest; entry i = base +
i*4096) or 1024 page-sized common buffers. Use logical (IOMMU) addresses only: Windows 11 Kernel DMA
Protection applies to Thunderbolt, and the Linux notes show what a bad address does (an IOMMU fault
escalating to a PCIe fatal error, `ua_audio.c:812-819`).

### 1.2 Scatter-gather table

Programmed by `ua_audio_program_sg()` (`ua_audio.c:733-809`):

1. Read AX_CONTROL (0x2248); write AX_CONTROL = 0; read AX_CONTROL back as a flush. "Never rewrite
   the table under a running transport" (kext ProgramRegisters does the same) (`ua_audio.c:746-760`).
2. For i = 0..1023 (`UA_DMA_SG_ENTRIES`, `ua_apollo.h:364`), with `addr_p(i)` / `addr_r(i)` = bus
   address of byte `i * 4096` of the playback / record ring (`ua_audio.c:762-764`):
   * write `0x8000 + i*8` = low 32 bits of `addr_p(i)`; write `0x8000 + i*8 + 4` = high 32 bits;
   * write `0xA000 + i*8` = low 32 bits of `addr_r(i)`; write `0xA000 + i*8 + 4` = high 32 bits
     (`ua_audio.c:765-773`).
   Entry layout: 8 bytes, `{u32 addr_lo, u32 addr_hi}`. 1024 x 4 KiB = 4 MiB (`ua_audio.c:528`,
   `ua_audio.c:722-724`). The SG SRAM "is write-only (reads return 0x00000000)" (`ua_audio.c:728`).
3. No doorbell after the table (`ua_audio.c:776`).
4. DMA_CTRL: read, compute the clean value, write, read back (`ua_audio.c:786-800`). Clean value
   for v2 firmware with the default `dma_per_dsp=1`:
   `(v & ~(0x1FE00 | 0x1FE)) | BIT(0) | (one bit (i+1) for each DSP i < num_dsps)`
   (`ua_dsp.c:815-828`). v1 firmware: `(v | 0x1F) & ~0x1E00` (`ua_dsp.c:820-821`). x4 reads 0x1F
   at this point (`ua_audio.c:3705-3707`). **[INF]** x8p (6 DSPs, `ua_routing.h:137`) gives low
   bits 0x7F.
5. EnableVector(0x28 notification, clearPending=1) (`ua_audio.c:802-803`), see 3.3.

When it runs: at probe before the first transport start (cold and warm paths, every device class)
(`ua_core.c:4282-4294`, `ua_core.c:4364-4370`), at audio init if that failed (`ua_audio.c:9510-9520`),
and on resume (`ua_audio.c:847-862`). **[HW]** "Ring-connect devices (x8/x8p/x16/Twin X, issue #46)
need this too ... SAMPLE_POS frozen at 0, and a host lockup as soon as a stream opened"
(`ua_core.c:4356-4363`); the x8p needed commit 2df18d5 "SG table on ring-connect devices"
(`devices/apollo-x8p.json:14`). With all-zero entries the FPGA DMAs to physical 0, which under an
IOMMU caused a PCIe uncorrectable fatal error (`ua_audio.c:811-825`).

### 1.3 Ring geometry

* One ring per direction, interleaved frames, a single ring per direction (not per channel).
  Frame f of a direction starts at byte `f * stride`, channel slot c at `+ c * 4`
  (`ua_audio.c:4859-4861`, `ua_audio.c:4957-4961`).
* `stride = channels * 4`, with `channels = AX_PLAY_CH` for playback and `AX_REC_CH` for record;
  the two directions have independent strides (`ua_audio.c:4921-4934`).
* Ring length in frames (both directions share it): `frames = rounddown_pow2(0x400000 /
  (max(play_ch, rec_ch) * 4) / 2)`, capped at 8192 (`ua_audio.c:380-407`, `UA_MAX_BUF_FRAMES`,
  `ua_apollo.h:914-915`). For any layout of up to 64 channels (`UA_MAX_CHANNELS`, `ua_apollo.h:908`)
  this is always 8192 (`ua_audio.c:1647-1653`). So: **ring = 8192 frames = 170.67 ms at 48 kHz,
  185.76 ms at 44.1 kHz** (arithmetic).
* Only `8192 * stride` bytes of the 4 MiB are used, e.g. x8p legacy playback 8192 x 34 x 4 =
  0x110000 bytes, record 8192 x 32 x 4 = 0x100000 (arithmetic). The ring wraps at frame 8192, not at
  4 MiB (`ua_audio.c:4958-4960`, `ua_audio.c:598-615`). All 1024 SG entries are still programmed.
* A stride change zeroes the whole playback ring, "Data laid out at the old stride would play back
  as noise" (`ua_audio.c:1416-1421`).

### 1.4 Sample format

* 32-bit signed little-endian container (`ua_apollo.h:904-905`, `SNDRV_PCM_FMTBIT_S32_LE`
  `ua_audio.c:4638`), 24 significant bits in the top 24 bits ("The converters are 24 bit ... data in
  the top 24 bits", `ua_audio.c:4716-4724`).
* Interleaved, no per-channel planes (`SNDRV_PCM_INFO_INTERLEAVED`, `ua_audio.c:4636`).
* **[WIN]** Matches `WAVEFORMATEXTENSIBLE` with `wBitsPerSample = 32`, `wValidBitsPerSample = 24`,
  `KSDATAFORMAT_SUBTYPE_PCM` (valid bits are MSB-aligned by definition).

### 1.5 Channel layouts: legacy vs descriptor ("io_map")

The driver has two ways to decide the per-frame slot count and which ALSA channel lives in which
slot (`ua_audio.c:208-219`, `ua_audio.c:864-901`):

* **Legacy (io_map=0):** fixed counts per device type, ALSA channel N = DMA slot N
  (`ua_audio.c:221-264`, `ua_audio.c:353-378`). Labels are taken from the model's route table only
  when the counts match, "labels only; whether the firmware puts them there is what io_map=1
  settles" (`ua_audio.c:353-358`).
* **Descriptor (io_map=1), the kext's model:** AX_PLAY_CH/AX_REC_CH = firmware descriptor slot
  counts; ALSA channels follow the model's route table for the current rate band and TOSLink mode;
  each ALSA channel is copied to/from the first slot whose ID matches (`ua_audio.c:867-884`).
* Default `io_map=-1`: descriptor layout on every model **except the x8p**, "which owners verified on
  the legacy layout (issues #46/#52)" (`ua_audio.c:48-51`, `ua_audio.c:305-322`). So on Linux the
  x8p runs legacy, the Twin X runs descriptor. Without a valid descriptor in both directions the
  layout stays legacy (`ua_audio.c:1310-1344`).

#### 1.5.1 IO descriptor format (firmware-written, host never writes it)

72 words at 0xC1A4 (record/input) and 0xC2C4 (playback/output) (`ua_apollo.h:420-452`):

| Word | Value |
|---|---|
| 0 | 1 (version) |
| 1 | 0x46 (70 words follow) |
| 2 | 0xFFFFFFFF |
| 3 | input slot count (meaningful in the input area) |
| 4 | output slot count (meaningful in the output area) |
| 5 | 2 in the input area, 0 in the output area (unused) |
| 6.. | u16 channel IDs in slot order, low half-word first; padding 0x00FF00FF |

(`ua_apollo.h:424-445`, `ua_routing.h:33-47`). Channel ID = `type << 8 | number` (number 1-based);
types: 0x00 MIC/LINE/HIZ, 0x01 MIC/LINE, 0x02 AUX, 0x03 LINE, 0x04 ADAT, 0x05 S/PDIF, 0x06 AES/EBU,
0x07 VIRTUAL, 0x08 MADI, 0x09 MON, 0x0A CUE, 0x0B TALKBACK, 0x0C RESERVED, 0x0D DANTE, 0x0E UNKNOWN
(`ua_audio.c:906-911`).

Read protocol `ua_io_desc_fetch()` (`ua_audio.c:991-1036`):
1. Read words 0-5. Usable only if word0 == 1, word1 == 0x46 and the count word (3 for record, 4 for
   playback) is 1..64 (`ua_audio.c:978-989`).
2. Read `ceil(count/2)` ID words from word 6 (`ua_audio.c:1021-1027`).
3. Re-read word 0, word 1 and the count word; any difference means a torn read (republish or link
   drop) and the read is discarded (`ua_audio.c:1029-1032`).
4. An unusable read never replaces the last valid descriptor (`ua_audio.c:1049-1071`,
   `ua_audio.c:885-888`).

When it is read: after connect (the kext synthesises notification bits 0+1) (`ua_audio.c:1521-1536`),
on notification bit 0 (output) / bit 1 (input) in the service pass (`ua_audio.c:2422-2424`,
`ua_apollo.h:948-949`), at prepare (`ua_audio.c:1570-1586`), at open (`ua_audio.c:1669-1676`), after
a TOSLink mode change (`ua_audio.c:1552-1568`), and after a clock change, polled every 10 ms for
100 ms and accepted only when two consecutive valid reads agree (`ua_audio.c:903-904`,
`ua_audio.c:1689-1761`).

**The descriptor is not a constant.** An x4 io_map fixture (not marked synthesized) shows 22 record
/ 24 playback slots at 48 kHz (`configs/pipewire/tests/x4-io_map-48k.txt:28`,
`configs/pipewire/tests/x4-io_map-48k.txt:55`) and 20/20 at 192 kHz
(`configs/pipewire/tests/x4-io_map-192k.txt:28`, `:55`), while a macOS warm-boot dump of the same model
holds 34 input / 48 output slots (`ua_routing.h:30-51`, `ua_routing.h:89-93`). Read it at runtime.

#### 1.5.2 Mapping algorithm (io_map=1)

`ua_chan_map_build()` (`ua_audio.c:1244-1297`):
* Route list = `model->routes[band][mode][dir]`. Band: Hi when `(rate >> 7) >= 0x55f`, i.e. 176.4
  and 192 kHz (`ua_audio.c:1160-1164`). Mode: for x8p (`UA_TOSLINK_PER_DIR`) record uses the host-side
  "Digital Input Mode" (default ADAT for the x8p) and playback the "Digital Output Mode" (param 0x21
  value 8 = ADAT); for the Twin family (`UA_TOSLINK_INPUT`) both use the input mode, default S/PDIF
  (`ua_audio.c:1166-1196`, `ua_models_gen.h:1015-1017`, `ua_models_gen.h:552-553`). The output mode
  cache starts at 0 (S/PDIF) (`ua_audio.c:9616`).
* For each route ID in order: if the rate hides it (Twin family: ADAT number > 4 while
  50001 <= rate <= 150000, `adat_mid_rate_max = 4`) or no descriptor slot carries it, the ALSA channel
  gets no slot (`UA_CHAN_NO_SLOT = 0xFF`); otherwise it gets the first matching slot
  (`ua_audio.c:1231-1242`, `ua_audio.c:1149-1158`, `ua_audio.c:1265-1283`, `ua_apollo.h:1120-1128`).
  Channel count therefore stays fixed across rates (`ua_apollo.h:1120-1126`).
* If no route ID matched at all, every descriptor slot is exposed in slot order (`ua_audio.c:1284-1296`).
* AX_REC_CH = input descriptor count, AX_PLAY_CH = output descriptor count, plus a flag channel only
  when diagnostic flags are set; `UA_AX_DIAG_FLAGS = 0`, so no extra channel (`ua_audio.c:1332-1336`,
  `ua_apollo.h:454-462`). **[INF]** Older driver builds evidently added one slot (the unmarked x4
  fixture shows "DMA frame 23 slots" for a 22-slot descriptor, `configs/pipewire/tests/x4-io_map-48k.txt:5`,
  `:28`) and streamed correctly, which suggests the FPGA honours AX_*_CH as the stride whatever the
  descriptor says.
* Copy semantics: playback zeroes the whole hardware frame, then writes each mapped channel to its
  slot; capture returns 0 for unmapped channels (`ua_audio.c:4828-4883`).
* A layout change while the transport runs marks it stale and forces open streams into XRUN so they
  re-prepare and the transport restarts on the new layout (`ua_audio.c:1433-1504`).

### 1.6 Apollo x8p (0x22) channel layout

Counts: legacy row `{0x22, play 34, rec 32, preamps 8, hiz 2}` (`ua_audio.c:256`); route counts
LoMid and Hi: S/PDIF mode record 26 / playback 28, ADAT mode record 32 / playback 34
(`devices/apollo-x8p.json:29-38`); legacy labels use the ADAT lists (`legacy_mode = UA_DIG_ADAT`,
`ua_models_gen.h:1015`), whose counts equal the legacy counts (`ua_audio.c:367-370`).

**[HW]** On the legacy layout (`ua_audio.c:311-316`): "all 8 preamps discrete over ALSA ... channel identity 1:1 (physical input N ->
capture AUX(N-1)), and 26/26 @ 48k full duplex stable over a 60s soak. d2d10bb raised the legacy DMA
counts to 34 play / 32 rec ... and a 32-channel capture ran clean on hardware"
(`devices/apollo-x8p.json:14`). "Still unverified: rates other than 48k, digital I/O and clock"
(same line). README: "playback, all 8 preamps, channel identity at 48 kHz" (`README.md:129`).

Legacy layout (Linux default on the x8p), ALSA channel = DMA slot. IDs are the ADAT route lists
(`ua_models_gen.h:174-189`):

| Rec ch | ID | Name | | Play ch | ID | Name |
|---|---|---|---|---|---|---|
| 0, 1 | 0x0001, 0x0002 | MIC/LINE/HIZ 1, 2 | | 0, 1 | 0x0901, 0x0902 | MON L, R |
| 2, 3 | 0x0103, 0x0104 | MIC/LINE 3, 4 | | 2-9 | 0x0301-0x0308 | LINE 1-8 |
| 4-7 | 0x0305-0x0308 | MIC/LINE 5-8 (type LINE) | | 10-17 | 0x0401-0x0408 | ADAT 1-8 |
| 8-15 | 0x0401-0x0408 | ADAT 1-8 | | 18-25 | 0x0701-0x0708 | VIRTUAL 1-8 |
| 16-23 | 0x0701-0x0708 | VIRTUAL 1-8 | | 26-33 | 0x0A01-0x0A08 | CUE1-CUE4 L/R |
| 24, 25 | 0x0901, 0x0902 | MON L, R | | | | |
| 26-29 | 0x0201-0x0204 | AUX1 L/R, AUX2 L/R | | | | |
| 30, 31 | 0x0B01, 0x0B02 | TALKBACK 1, 2 | | | | |

Firmware descriptor captured on a running x8p over READ_REG (FPGA 0xa241c5ac, 6 DSPs, FW v2), 34
input and 52 output slots (`ua_routing.h:135-289`):

| In slot | ID | | Out slot | ID |
|---|---|---|---|---|
| 0-1 | 0x0001, 0x0002 | | 0-1 | 0x0901, 0x0902 (MON) |
| 2-3 | 0x0103, 0x0104 | | 2-9 | 0x0301-0x0308 (LINE 1-8) |
| 4-7 | 0x0305-0x0308 | | 10-17 | 0x0401-0x0408 (ADAT) |
| 8-15 | 0x0401-0x0408 | | 18-19 | 0x0501, 0x0502 (S/PDIF) |
| 16-17 | 0x0501, 0x0502 (S/PDIF) | | 20-27 | 0x0701-0x0708 (VIRTUAL) |
| 18-25 | 0x0701-0x0708 (VIRTUAL) | | 28-35 | 0x0A01-0x0A08 (CUE1-4) |
| 26-27 | 0x0901, 0x0902 (MON) | | 36-51 | 0x0C01-0x0C10 (RESERVED) |
| 28-31 | 0x0201-0x0204 (AUX) | | | |
| 32-33 | 0x0B01, 0x0B02 (TALKBACK) | | | |

**[HW, Windows port, Nov 14 2026] Resolved: the FPGA uses descriptor order.** On Aaron's x8p (same
FPGA 0xa241c5ac) with AX_REC_CH = 32, `apollo-diag tone` on playback slots 0-1 came back at exactly
-20.0 dBFS on record slots 26-27 (0-based), the descriptor's MON L/R, and nowhere else. So record
16-17 = S/PDIF, 18-25 = VIRTUAL, 26-27 = MON, 28-31 = AUX, and TALKBACK (32-33) needs AX_REC_CH = 34.
Mic inputs read exact digital zero after a cold start: input strips are needed (spec-dsp-mixer 6.4).

**[INF] Discrepancy to keep in mind.** Legacy programs AX_REC_CH = 32 and AX_PLAY_CH = 34 against a
34/52-slot descriptor. If the FPGA lays the narrower frame out in descriptor order (the kext's model),
legacy record channels 16+ and playback channels 18+ carry different signals than their legacy
labels (descriptor has S/PDIF at in 16-17 / out 18-19, shifting everything after). The source does
not say which; the hardware checks cover record 0-7 and playback (`devices/apollo-x8p.json:14`). The
channels a conferencing endpoint needs, record slots 0-7 (preamps) and playback slots 0-1 (MON), are
the same in both orders.

**[INF]** x8p under io_map=1 with that descriptor (derived with 1.5.2, not run on hardware): record
32 ALSA channels (ADAT list) on slots 0-15, 18-33, AX_REC_CH = 34; playback 28 ALSA channels
(S/PDIF list, output mode cache 0) on slots 0-9, 18-35, AX_PLAY_CH = 52.

### 1.7 Apollo Twin X (0x99 Gen 1, 0x3B/0x3C/0x40/0x41 Gen 2)

* No legacy row: a legacy fallback would be 3 play / 2 rec (`ua_audio.c:340-350`,
  `ua_apollo.h:911-912`), a placeholder. The Twin X runs the descriptor layout by default
  (`ua_audio.c:317-322`). **No hardware descriptor capture exists in the source** (only x4 and x8p,
  `ua_routing.h:301-318`); the Twin X fixture is "synthesized ... not a hardware capture"
  (`configs/pipewire/tests/twin-x-io_map-48k.txt:1`). Read the descriptor at runtime.
* Model: 2 preamps, 1 HiZ, talkback, desktop, TOSLink rule "input-mode", default input S/PDIF,
  ADAT 5-8 hidden at 88.2/96k (`ua_models_gen.h:545-582`, `devices/apollo-twin-x.json:11-21`).
* Record route list, S/PDIF mode (16): 0x0001, 0x0002 (MIC/LINE/HIZ 1, 2), 0x0901, 0x0902 (MON L/R),
  0x0701-0x0704 (VIRTUAL 1-4), 0x0501, 0x0502 (S/PDIF L/R), 0x0201-0x0204 (AUX1/AUX2 L/R), 0x0B01,
  0x0B02 (TALKBACK) (`ua_models_gen.h:228-232`). ADAT mode (22) replaces S/PDIF with 0x0401-0x0408
  (`ua_models_gen.h:234-239`).
* Playback route list (10, both modes): 0x0901, 0x0902 (MON L/R), 0x0A03, 0x0A04 ("LINE 3/4"),
  0x0701-0x0704 (VIRTUAL 1-4), 0x0A01, 0x0A02 ("HP L/R") (`ua_models_gen.h:197-201`,
  `devices/apollo-twin-x.json:61-72`). The JSON names the CUE IDs LINE 3/4 and HP L/R; the driver's
  generic namer calls them CUE2 and CUE1 (`ua_audio.c:932-956`).
* **[HW]** "Gen 2 Duo partially verified: playback, Mic 1 capture and direct monitoring at 48 kHz with
  `input_strips=1` (after a power cycle with the driver loaded). Gen 1, other rates and digital I/O
  untested" (`README.md:133`). **[dep]** Capture on the Twin X therefore depended on the DSP layer
  building input strips. A warm move from another OS used to leave "playback silent" until the DSP
  ring connect was no longer skipped (`CHANGELOG.md:50-52`).

## 2. Transport

### 2.1 What "transport" means here

The "audio extension transport" is the FPGA engine that moves interleaved frames between the two
host rings and the converters/DSP. Its registers "control DMA streaming, NOT the device mixer"
(`ua_apollo.h:283-286`). There is **one** transport for both directions: AX_CONTROL starts
playback and record together (0xF = DMA_EN | PLAY_EN | REC_EN | IRQ_EN, `ua_apollo.h:347-352`), and
"Playback and capture share one free-running transport" with one position register
(`ua_audio.c:186-192`). There is no per-direction start. The separate TRANSPORT register 0x2220 is
only written 0 at cold init and teardown (`ua_core.c:798-799`, `ua_audio.c:9694-9696`).

### 2.2 Preconditions (all must hold before prepare)

1. Rings allocated and the SG table programmed while AX_CONTROL = 0 (1.2).
2. **[dep]** The audio extension connected (ACEFACE handshake, notification config words, IO
   descriptor read, mixer batch init) (`ua_audio.c:3000-3401`). Two parts of that connect are
   needed for signal flow:
   * capture routing: unless the DSP was inherited from another OS, settings 1..37 except 2 get
     value 0x20 mask 0xFF and setting 35 value 0x05 mask 0x1F, "capture routing requires settings
     1-37 = 0x20/0xFF (Windows BAR0 capture) plus setting[35] = 0x05" (`ua_audio.c:3278-3308`);
   * rate ack `{0x001A0002, rate_Hz}` to every DSP (`ua_audio.c:3214-3223`, `ua_dsp.c:4186-4213`);
     "a DSP that has not been told the rate does not start an input strip" (`ua_audio.c:135-138`).
3. **[dep]** The 10 ms DSP service loop running: it clears 0xC030 (write 0, last step of a pass),
   drains the mixer readback and flushes mixer settings (`ua_audio.c:2347-2501`). Without host
   servicing "the DSP halts" (`ua_audio.c:2350-2355`), and "the firmware posts nothing new while
   earlier bits stay set" (`ua_audio.c:2150-2155`).
4. Notification vector 0x28 enabled (from SG programming, re-armed after a soft disconnect)
   (`ua_audio.c:802-803`, `ua_audio.c:5694-5703`).
5. Transport start is deferred to the first prepare, never at probe: starting it at connect "triggers
   an immediate PCIe AER error on some Thunderbolt controllers (kernel 6.18+, Maple Ridge), killing
   the link within ~50ms" (`ua_audio.c:3366-3377`).

### 2.3 Prepare sequence (exact)

`ua_audio_prepare_transport(ua, timer_frames)`, caller holds the device lock, transport stopped
(`ua_audio.c:3661-3888`). Values for the x8p legacy layout at 48 kHz in brackets (arithmetic from
the formulas).

| Step | Action | Cite |
|---|---|---|
| 0 | io_map=1 only: re-read descriptors and commit the layout (counts, stride, map) | `ua_audio.c:3681-3687` |
| 0a | Clear the AX fault latch (kext does this in PrepareTransport) | `ua_audio.c:3689-3690` |
| 0b | Read DMA_CTRL; if it differs from the clean value (1.2 step 4), write the clean value. x8p issue #46: DMA_CTRL read 0x1ffff at stream time, reset strobes re-asserted after SG programming, "transport arms and acks (CTRL readback 0x28f) but SAMPLE_POS/FRAME_CTR never advance" | `ua_audio.c:3697-3719` |
| 1 | AX_BUF_SIZE (0x2240) = frames - 1 [0x1FFF] | `ua_audio.c:3726-3727` |
| 2 | AX_DMA_BLKSZ (0x226C) = ((frames - 1) * play_ch) >> 10 [(8191*34)>>10 = 271 = 0x10F] | `ua_audio.c:3729-3731` |
| 3 | AX_SAMPLE_POS (0x2244) = 0 | `ua_audio.c:3733-3734` |
| 3b/4 | Only with period_irq=1: AX_TIMER_CFG (0x2270) = interval - 1 and EnableVector(0x46, clear). Without a timer, mask 0x46 if a previous session left it armed | `ua_audio.c:3736-3757` |
| 5 | AX_SAMPLE_POS = 0 again ("kext does this twice") | `ua_audio.c:3759-3760` |
| 6 | Read AX_SAMPLE_POS (fence) | `ua_audio.c:3762-3763` |
| 7 | AX_CONTROL (0x2248) = 0 | `ua_audio.c:3765-3766` |
| 8 | AX_FRAME_CFG (0x2258) = 0x08 (44.1/48k), 0x10 (88.2/96k), 0x20 (176.4/192k); `frame_cfg_win=1` uses the UA Windows driver's 0x0B/0x16/0x2C. "Previously hardcoded to 0x10 (wrong for 48kHz!)" [0x08] | `ua_audio.c:3768-3785`, `ua_audio.c:1937-1950`, `ua_audio.c:147-150` |
| 9 | AX_PLAY_CH (0x2250) = play_channels [34] | `ua_audio.c:3787-3792` |
| 10 | AX_REC_CH (0x225C) = rec_channels [32] | `ua_audio.c:3794-3799` |
| 11 | AX_CONTROL = 1 (ARM, DMA_EN) | `ua_audio.c:3801-3802` |
| 12 | Read AX_STATUS (0x22C0) as a fence | `ua_audio.c:3804-3805` |
| 13 | Legacy layout only: AX_P2P_ROUTE (0x224C) = 0x100 \| (play_ch - 1) [0x121]. Written after ARM ("Previously written before DMA enable (wrong order)"). The kext writes it only with diag flag bit 1, i.e. never; io_map=1 skips it | `ua_audio.c:3807-3820` |
| 13b | Read AX_CONTROL; check fault bits 7/8 | `ua_audio.c:3822`, `ua_audio.c:3833` |
| 14 | FRAME_CTR gate (default `frame_ctr_gate=2`): read AX_FRAME_CTR (0x2254); while it != FRAME_CFG and retries <= 2: sleep 1.0-1.5 ms and re-read (3 reads, as the kext). A mismatch is logged, not an error; FRAME_CTR 0 is warned | `ua_audio.c:3826-3877`, `ua_audio.c:123-126` |
| 15 | EnableVector(0x47 end-of-buffer, clearPending=1) | `ua_audio.c:3879-3880` |

Why the gate matters: the old "first non-zero" rule "starts a Hi-band transport at FRAME_CTR 4 of
0x20" (`ua_audio.c:123-126`); with the gate "the macOS driver's own rate-change sequence
(`rate_change=1`) records 11 of 14 runs instead of 5" (`CHANGELOG.md:203`). Also: "The FPGA state
machine needs time between the ARM write and the START write. Without this delay, the transport start
is silently ignored (FRAME_CTR stays 0)" (`ua_audio.c:2775-2784`, boot path, which sleeps 1-2 ms).
**[WIN]** Keep at least 1 ms between ARM (step 11) and START (2.4), and gate on FRAME_CTR == FRAME_CFG.

### 2.4 Start sequence

`ua_audio_start_transport()` (`ua_audio.c:4031-4141`):

1. If already running, return (`ua_audio.c:4036-4037`).
2. ctrl = 0xF (default; "the kext's value for a unit on its own clock") or 0x20F with
   `ax_control_ext=1` (peer-clocked) (`ua_audio.c:4039-4055`, `ua_audio.c:113-116`).
   **[HW]** titan x4: 0xF passed 28 of 28 ratecheck runs, 0x20F 14 of 14 (`ua_audio.c:4044-4046`).
3. Clear stale pending interrupts: read ISR (0x2208), write it back if non-zero; same for ISR_HI
   (0x2264) when the 64-vector block is in use (`ua_audio.c:4057-4076`).
4. AX_CONTROL = ctrl (`ua_audio.c:4082-4084`).
5. Read back AX_CONTROL, AX_SAMPLE_POS, AX_FRAME_CTR, AX_STATUS (`ua_audio.c:4086-4094`).
   AX_CONTROL == 0xFFFFFFFF means the device is gone (-ENODEV) (`ua_audio.c:4096-4103`).
6. Check AX_CONTROL fault bits (`ua_audio.c:4105`).
7. Bookkeeping: transport generation +1, transport_running = true, optional CPU latency QoS
   (`ua_audio.c:4107-4126`).
8. udelay(100), read SAMPLE_POS/FRAME_CTR/DMA_CTRL for the log only (`ua_audio.c:4128-4138`).

### 2.5 Post-start clock write (required for capture)

Immediately after every transport start in prepare (`ua_audio.c:5755-5756`):
write 0xC074 = `(rate_index << 8) | clock_source`, then AX_CONNECT = 4; no wait for an ack
(`ua_audio.c:5182-5200`). At 48 kHz on internal clock that is 0x20C (index 2, source 0x0C).

"Clock write AFTER transport start enables capture ... If done BEFORE transport, the DSP activates
with no audio flowing and routes silence ... (Discovered 2026-03-18: ordering is critical)"
(`ua_audio.c:5152-5166`). Use source 0x0C for internal: "source 0 is S/PDIF, which never acknowledges
without a signal" (`ua_audio.c:5162-5164`); 0x0C is also what macOS writes (0x020C for 48 kHz)
(`ua_core.c:4373-4381`, `ua_audio.c:9482-9484`). If the DSPs were last told a different rate, a rate
ack follows (`ua_audio.c:5196-5199`). Gated by `post_start_clock` (default on) (`ua_audio.c:105-108`).

### 2.6 Stop sequence

`ua_audio_stop_transport()` (`ua_audio.c:4143-4167`): release QoS; if running: DisableVector(0x47);
AX_CONTROL = 0; transport_running = false; read AX_CONTROL and SAMPLE_POS for the log.

### 2.7 Transport lifecycle in the Linux driver

* Started by the first `pcm_prepare` of either direction when not running (`ua_audio.c:5735-5774`).
  A second stream's prepare finds it running and does not touch it, in particular no TIMER_CFG
  rewrite "under a running transport (#42)" (`ua_audio.c:5775-5779`).
* trigger START/STOP never touch the transport; they only choose the period-wakeup source
  (`ua_audio.c:6574-6668`).
* Stopped only when both substreams are closed (`ua_audio.c:4770-4786`), and by teardown/suspend.
* PipeWire is configured never to close the device: `session.suspend-timeout-seconds = 0`, "Apollo
  firmware can be sensitive to transport resets" (`configs/wireplumber/51-ua-apollo.conf:18-20`).
  So in practice the transport runs continuously while PipeWire runs.
* Restarted (stop, prepare, start, clock write) when the io_map layout went stale
  (`ua_audio.c:5728-5733`, `ua_audio.c:5202-5228`).

### 2.8 Clock-change primitive

`ua_audio_clock_change_locked(clock_config)`, mirrors the kext's `_setSampleClock`
(`ua_audio.c:4332-4340`, `ua_audio.c:4398-4455`):

1. Hi-band dwell: if the clock is at 176.4/192k and leaving it within `hi_dwell_ms` (3000) of when it
   got there, wait (running DSP service steps every 25 ms) (`ua_audio.c:4341-4396`, `ua_audio.c:109-112`).
2. Write 0xC074 = clock_config (`ua_audio.c:4409`).
3. Read 0xC030; 0xFFFFFFFF = gone. If any ack bit is set, write `status & ~ack_mask` (clears only the
   ack bits; IO-descriptor bits stay for the service loop) (`ua_audio.c:4411-4415`).
4. AX_CONNECT = 4 (clock doorbell) (`ua_audio.c:4417-4418`).
5. Poll 0xC030 every 10 ms for up to 2000 ms (`UA_CLOCK_TIMEOUT_MS`, `ua_apollo.h:963-964`) for an
   ack bit: bits 5, 22, 4 by default, bit 5 only with `clock_ack_bit5` ("as the kext and Windows do")
   (`ua_audio.c:4316-4330`, `ua_audio.c:4420-4445`, `ua_audio.c:143-146`).
6. Timeout: FPGA_REV == 0xFFFFFFFF means offline, else -ETIMEDOUT (`ua_audio.c:4447-4454`). Callers
   retry once (`ua_audio.c:4577-4581`, `ua_audio.c:5419-5423`).

At connect a clock write was tried and dropped: "bit 22 ack never arrives (bit 21 reappears)"
(`ua_audio.c:3360-3364`). The ALSA "Sample Rate" control path (`ua_audio_set_clock_cfg`,
`ua_audio.c:4523-4615`) uses this primitive without restarting the transport; it is refused with
-EBUSY when an open stream holds another rate (`ua_audio.c:4547-4553`).

### 2.9 Sample-rate change (stream rate differs from the device rate)

Default `rate_change=0` path, inside `pcm_prepare` (`ua_audio.c:5596-5774`):

1. With the transport stopped, `ua_audio_hi_from_hi()` runs; it acts only when the target is
   176.4/192k and 0xC07C says the clock is already at 176.4/192k (see 7, item R4)
   (`ua_audio.c:5605-5606`, `ua_audio.c:5261-5277`).
2. Hi-band dwell for the new index (`ua_audio.c:5634`).
3. Cancel the period hrtimer (`ua_audio.c:5639-5642`).
4. Soft disconnect `ua_audio_disconnect_hw()` (`ua_audio.c:3409-3457`): stop the service loop
   (non-waiting); AX_CONTROL = 0; mask every interrupt (IMR = IMR_HI = 0); sleep 50 ms; read
   SEQ_WR (0x3808) and SEQ_RD (0x380C); if `seq_rd > 0 && seq_rd == seq_wr` skip the disconnect
   doorbell (it "kills the mixer DSP readback"), else AX_CONNECT = 0x10 and sleep 50 ms;
   connected = false.
5. sample_rate / rate_index = new values (`ua_audio.c:5655-5663`).
6. Connect again (handshake already done, so only the post-handshake part) (`ua_audio.c:5689-5691`).
7. Re-enable vector 0x28 and the DSP vectors (`ua_audio.c:5694-5703`), restart the service loop,
   whose 5-pass (50 ms) mixer-flush delay restarts (`ua_audio.c:5705-5711`, `ua_audio.c:2457-2462`).
8. Prepare, start, post-start clock write (2.3-2.5) (`ua_audio.c:5747-5756`).
9. io_map: settle poll of the descriptors (100 ms); if the layout changed, stop/prepare/start/clock
   again (`ua_audio.c:5758-5767`, `ua_audio.c:5202-5228`).
10. period_irq=1 only: wait up to 500 ms for 0xC07C to read locked at the new index, then restart once
    more (`ua_audio.c:5469-5535`, `ua_audio.c:5769-5774`).

Alternative `rate_change=1` (the kext's sequence, `ua_audio.c:5342-5466`): transport stopped,
clock-change primitive, poll 0xC07C [15:8] every 25 ms up to 4000 ms for the new index, rate ack to
every DSP, hold 1500 ms with service steps, re-read descriptors; no disconnect. It is the less
reliable one in the source's own tests (11 of 14 recording runs, `CHANGELOG.md:203`, versus 28/28
for the default, `ua_audio.c:4044-4046`).

Both directions share one clock: the rate rule offers the second direction only the first one's rate
and hw_params refuses a mismatch with -EBUSY (`ua_audio.c:4653-4669`, `ua_audio.c:4791-4815`). The
ping-pong this prevents "left the DSP recording silence until a power cycle"
(`ua_audio.c:4726-4734`, `CHANGELOG.md:267-269`).

### 2.10 Teardown, suspend, resume

* Remove/fini: stop the service loop, stop transport, disconnect, then AX_CONTROL = 0,
  TRANSPORT = 0, **sleep 100 ms before freeing the rings**: "the firmware may continue DMA for a brief
  period after disconnect ... the IOMMU revokes the mapping while the device is still writing,
  causing a DMA fault that kills the Thunderbolt bridge" (`ua_audio.c:9676-9696`). Rings are freed
  under the write lock of the copy path's semaphore (`ua_audio.c:9726-9734`).
* **[dep]** Full quiesce order (kext `CPcieDevice::Shutdown`): output mute (some types), DisableVector
  (0x47), AX_CONTROL = 0, DSP rings drained, AX_CONTROL = 0, flush read, AX_P2P_ENABLE = 0,
  DisableVector(0x28), AX_CONNECT = 0x10, read FPGA_REV, mask and W1C all interrupts
  (`ua_core.c:1623-1705`).
* Suspend: stop the service loop, the IRQ-path watchdog and the period timer (`ua_audio.c:9277-9298`).
  Resume: SG reprogrammed (`ua_audio.c:847-862`), connect redone; "Streams come back through a fresh
  prepare (no SNDRV_PCM_INFO_RESUME), which restarts the transport" (`ua_audio.c:9300-9344`).

### 2.11 Cold-boot transport **[dep]**

For AudioExtension devices on a cold start, ua_core runs a pre-firmware transport (full prepare with
legacy counts and FRAME_CFG 0x10, ARM, 1-2 ms, AX_CONTROL = 0x20F, FRAME_CTR poll, early ACEFACE, DMA
reset pulse, 50 ms) so the DSP command rings work, and stops it after firmware load
(`ua_audio.c:2702-2847`, `ua_core.c:4372-4391`, `ua_core.c:4549-4554`). The SG table must be
programmed before it (`ua_core.c:4348-4355`). Classification: x8p 0x22 is ring-connect, not
AudioExtension (`ua_apollo.h:141-157`); Twin X 0x99 and 0x3B count as AudioExtension
(`ua_apollo.h:151-157`).

## 3. Position and timing

### 3.1 What the host can read

| Register | Meaning | Use |
|---|---|---|
| AX_SAMPLE_POS 0x2244 | current frame index in the ring, shared by both directions | the only position source; ALSA pointer = `sp % buffer_size` (`ua_audio.c:6681-6702`) |
| AX_FRAME_CTR 0x2254 | rises to FRAME_CFG after ARM | start gate only (`ua_audio.c:3826-3855`) |
| AX_CONTROL 0x2248 | bits 7, 8 = fault flags latched by the kext and Windows | polled every 10 ms in the service pass (`ua_audio.c:2041-2079`, `ua_audio.c:2430-2432`) |
| 0xC07C | rate index [15:8], lock (bits [1:0] == 1) | clock lock control, rate verification (`ua_audio.c:2000-2039`) |
| AX_STATUS 0x22C0 | fence read; logged on fault bit 7 | (`ua_apollo.h:299-302`, `ua_audio.c:2062`) |

No 64-bit sample counter and no hardware timestamp register is used anywhere. WALLCLOCK (0x220C,
"Wallclock / interrupt pending", `ua_apollo.h:273`) is defined but never read. **[INF]** The host has
to extend SAMPLE_POS to a 64-bit position itself.

### 3.2 SAMPLE_POS in detail

* **Units: frames** of the direction's frame (not bytes, not samples). ALSA uses it directly as the
  hardware pointer in frames (`ua_audio.c:6684-6702`); the boundary test works in frames
  (`ua_audio.c:5985-5986`).
* **Range and wrap: [INF]** 0..8191, wrapping at the ring length (BUF_SIZE + 1). The source always
  reduces it with `% buffer_size` defensively (`ua_audio.c:6702`, `ua_audio.c:6334`); after an
  end-of-buffer interrupt "SAMPLE_POS should be near 0" (`ua_audio.c:6357-6362`); the IRQ trace
  reports "frames since the previous event (mod the ring)" (`ua_audio.c:6462-6467`). Whether the raw
  register ever exceeds 8191 is not stated.
* **Shared:** one register for playback and capture (`ua_audio.c:186-192`, `ua_audio.c:6721-6722`).
  Byte offset in a direction's ring = `SAMPLE_POS * stride(dir)`.
* **Meaning:** the frame the transport "reads (playback) or writes (capture) next"
  (`ua_audio.c:6717-6720`). Linux lets ALSA treat everything before it as consumed (playback) or
  complete (capture), with no capture-side safety margin (`ua_audio.c:6670-6703`), and capture is
  hardware-verified clean on that basis (`devices/apollo-x8p.json:14`, `README.md:52`).
* **Reset:** prepare writes 0 twice; it starts advancing at START (`ua_audio.c:3733-3763`).
* **Read cost:** a non-posted MMIO read over Thunderbolt. No typical latency figure is given. The
  driver keeps a histogram of AX_CONTROL read times (<10 us, <50 us, <100 us, <1 ms, >=1 ms) and logs
  reads of 1 ms or more as bus stalls (`ua_audio.c:2081-2120`); the kext asks for a 10 us max bus
  stall while streaming (`ua_audio.c:2083-2086`). On a dead link a read "would stall a CPU for a full
  completion timeout" and returns 0xFFFFFFFF (`ua_audio.c:6036-6042`). The default path reads it 1000
  times a second (`ua_audio.c:164-172`).
* **Liveness:** a SAMPLE_POS frozen across 8 prepares within 2 s each is treated as a stalled
  transport (`ua_audio.c:5548-5586`).

### 3.3 Interrupts

* Vectors (logical -> hardware bit): 0x28 notification -> HW 32 (ISR_HI bit 0); 0x46 period timer
  -> HW 62 (ISR_HI bit 30); 0x47 end-of-buffer -> HW 63 (ISR_HI bit 31) (`ua_apollo.h:1732-1775`,
  `ua_apollo.h:481-483`). These only exist in 64-vector mode: v2 firmware, ext_caps bit 31 set,
  family `(ext_caps >> 20) & 0x3f` != 6, "every known Apollo" (`ua_apollo.h:1749-1761`,
  `CHANGELOG.md:202`).
* EnableVector(v, clearPending): under the IRQ lock set the cached mask bit, W1C the pending bit if
  asked, write the mask register, and only when clearing, read FPGA_REV to flush
  (`ua_apollo.h:1777-1816`). DisableVector: clear the bit and write the mask, no flush
  (`ua_apollo.h:1846-1864`).
* ISR (`ua_core.c:159-234`): read ISR; 0xFFFFFFFF = not ours; mask with the cached IMR; read ISR_HI
  and mask; ack only the masked bits by writing them back. "Acking unmasked DSP noise bits (0x18c63)
  causes them to immediately re-set, generating an MSI storm that overwhelms the Thunderbolt link"
  (`ua_core.c:172-181`). MSI with 1-2 vectors; legacy INTx only for device 0x0001 (UAD-2 PCIe) and
  subsystem 0x05, with subsystems 0x0A/0x0B on MSI unless `satellite_msi=0` (`ua_core.c:4873-4899`).
* End-of-buffer (0x47): one per 8192-frame wrap, enabled at prepare step 15, disabled at stop; re-enabled
  (no pending clear) after each fire (`ua_audio.c:3879-3880`, `ua_audio.c:4156-4157`,
  `ua_audio.c:6848-6851`). That is 5.86 per second at 48 kHz (arithmetic); with the defaults the device
  raises only end-of-buffer and notification interrupts, "about 6 a second at 48 kHz"
  (`CHANGELOG.md:204`). "The reference drivers take their audio clock from interrupts ... from the
  end-of-buffer vector (kext: UAD2AudioEngine::_bufferCallback -> takeTimeStamp, once per 8192-frame
  wrap; Windows: the same fast callback)" (`ua_audio.c:5890-5895`).
* Period timer (0x46): only with `period_irq=1` (default off). TIMER_CFG = interval - 1; "with
  interval - 1 the interrupt keeps a fixed phase against SAMPLE_POS over thousands of periods, with
  interval or interval + 1 it drifts by a frame per period" (`ua_audio.c:3519-3527`). Interval forced
  to >= 1 ms (`ua_audio.c:3532-3546`). Treated as one-shot: masked in the hard IRQ, re-armed with the
  pending bit cleared after service (`ua_audio.c:6366-6394`, `ua_audio.c:6837-6850`). Known problems in
  7 (items T1-T5). **[HW]** only on the x4 (titan).
* Notification (0x28): optional `notif_irq` runs the service pass at once; storm guard masks it above
  2000/s (`ua_audio.c:2508-2550`).

### 3.4 How Linux generates period wakeups (default)

* A 1 ms soft hrtimer (softirq context, not hard IRQ) reads SAMPLE_POS and, per running substream,
  calls `snd_pcm_period_elapsed()` when `pos / period != last / period` (`ua_audio.c:5996-6080`,
  `ua_audio.c:5946-5994`). Wakeups are therefore paced by the device clock (SAMPLE_POS), with up to
  ~1 ms of quantisation plus softirq latency. A soft hrtimer rather than a workqueue because jiffy
  granularity (4 ms at HZ=250) is "longer than a 256-frame period at 96 kHz" (`ua_audio.c:6005-6011`).
* All-ones from SAMPLE_POS stops the timer and XRUNs both streams (`ua_audio.c:6032-6058`).
* The timer is shared; it stops only when no running substream needs it (`ua_audio.c:6637-6663`).
* `period_irq=1` alternative: threaded handler reads SAMPLE_POS once per timer interrupt, with a
  10 ms watchdog that falls back to the hrtimer if the interrupt goes silent while SAMPLE_POS moves,
  or if the last 64 interrupts landed on average more than a quarter interval past a boundary
  (`ua_audio.c:5878-5934`, `ua_audio.c:6193-6250`, `ua_audio.c:6309-6364`). Usable only if the
  interval divides 8192 and the period, and period * 4 <= 8192 (`ua_audio.c:6281-6307`).

### 3.5 Where a stream starts in the shared ring

Because the transport is already running when a second stream starts, SAMPLE_POS is "anywhere in the
ring when a stream starts". With ALSA's default hw_ptr = 0, "a playback stream whose prefill is
shorter than that xruns ... again after each recovery" (`ua_audio.c:186-198`). Fix
(`pcm_live_start=1`, default): at the RESET after prepare, set hw_ptr = `SAMPLE_POS % buffer_size`
rounded down to `min_align` (`ua_audio.c:6705-6770`); the trigger starts the boundary tracking at that
position (`ua_audio.c:6591-6616`). **[HW]** x4: a duplex start went from 7 playback xruns to 1
(`CHANGELOG.md:230`). "An offset between SAMPLE_POS and the reported pointer would not do: ALSA puts
frame n at ring offset n % buffer_size" (`ua_audio.c:6724-6727`).

### 3.6 Latency and safety offsets

* Playback safety offset = FRAME_CFG: 8 / 16 / 32 frames (44.1-48 / 88.2-96 / 176.4-192k), "the
  safety offset the kext hands CoreAudio" (`ua_audio.c:1937-1950`); 166.7 us in every band
  (`configs/wireplumber/51-ua-apollo.conf:53-55`). UA's Windows driver uses 0x0B / 0x16 / 0x2C
  (`ua_audio.c:147-150`).
* Reported delay (`runtime->delay`): playback = safety offset ("+ DAC latency, unknown"), capture =
  ADC latency rounded to whole frames (`ua_audio.c:1974-1998`, `ua_audio.c:5783-5793`).
* ADC latency (milli-frames per rate index 1..6): x8p `{10000, 10000, 10000, 10000, 11000, 11000}`,
  board 2 all 10000 (`ua_models_gen.h:1019-1023`); Twin X `{5000, 5000, 5000, 5000, 6000, 6000}`,
  board 1 `{12000, 12000, 9000, 9000, 5000, 5000}` (`ua_models_gen.h:555-559`). Board id = mixer
  readback word 33 [28:26] (`ua_audio.c:1952-1972`, `ua_apollo.h:890`).
* Mixer Engine output latency rows (units not stated; the input row uses frames): x8p line
  `[39, 39, 11.25, 11.25, 11.5, 11.5]`, monitor `[43, 43, 15.25, 15.25, 15.5, 15.5]`, dsp_to_fpga 6,
  fpga_to_dsp 2 (`devices/apollo-x8p.json:224-236`); Twin X line `[44.3, 44.3, 16.35, ...]`, monitor
  `[42.3, 42.3, 14.35, ...]` (`devices/apollo-twin-x.json:140-152`); Twin X Gen 2 line
  `[16.3, 16.3, 16.35, ...]` (`devices/apollo-twin-x-gen-2.json:144-147`).
* Firmware latency words 0xC198 (out) / 0xC19C (in) are logged per rate; their meaning is inferred
  by the source (`ua_audio.c:2176-2203`).

### 3.7 XRUN detection and recovery

* The hardware does not report under/overruns. The ring is free-running; ALSA's core detects xruns
  from hw_ptr vs appl_ptr.
* The driver forces XRUN when: BAR0 reads all-ones (`ua_audio.c:6032-6058`); the transport stopped
  or restarted under an IRQ-served stream (`ua_audio.c:5936-5944`, `ua_audio.c:6691-6700`); the
  io_map layout changed while running (`ua_audio.c:1433-1469`).
* Recovery = userspace re-prepares; prepare restarts the transport if needed and puts hw_ptr at the
  live position (`ua_audio.c:5735-5774`, 3.5).
* Prepare-churn guard (issue #42): 8 prepares with SAMPLE_POS frozen, each within 2 s of the last,
  refuse prepares for 30 s, because "the register traffic eventually wedges the PCIe link (hard
  freeze, no oops)" (`ua_audio.c:5548-5586`, `ua_apollo.h:1384-1393`).
* After ACEFACE failed, prepare returns -ENODEV instead of retrying (PipeWire's retries "created an
  infinite 20s-blocking loop that freezes the system") (`ua_audio.c:5666-5687`).

### 3.8 Recommended position model for Windows **[WIN]**

What the hardware gives you: an exact, frame-granular, wrapping (8192) position register shared by
both directions, plus a once-per-wrap interrupt. What it does not give: a byte position per stream,
a 64-bit counter, a timestamp.

1. **One owner reads SAMPLE_POS**: a 1 ms high-resolution timer DPC (the Linux default cadence,
   3.4). Bracket each read with QPC; store `(pos64, qpc)` where `pos64` accumulates
   `(raw - last_raw) & 8191`. Reads must come at least twice per wrap (every < 85 ms at 48 kHz);
   at 1 ms that is never close. Use EOB interrupts as a wrap witness and a second timestamp source
   (what UA's own drivers clock from, `ua_audio.c:5890-5895`).
2. **Do not map BAR0 to the audio engine** as a position register (`KSPROPERTY_RTAUDIO_POSITIONREGISTER`):
   SAMPLE_POS is frames in an 8192-frame hardware ring with a 32/34/52-slot stride, not a byte offset
   in the engine's buffer, and a user-mode read on a dropped Thunderbolt link stalls (3.2).
3. **GetPosition / GetPresentationPosition** return the cached value extrapolated by
   `(qpc_now - qpc_last) * rate`, clamped to never pass the next real read; return the QPC of the
   underlying read for presentation position.
4. **Pace everything from SAMPLE_POS, never from QPC alone.** Linux reports periods only when
   SAMPLE_POS crosses a boundary (`ua_audio.c:5985-5991`); a QPC-paced notification would drift
   against the Apollo crystal and glitch over minutes.
5. **Render lead:** write the hardware ring at least FRAME_CFG frames ahead of SAMPLE_POS (the
   safety offset, 3.6) plus timer jitter. **Capture lag:** frames before SAMPLE_POS are complete
   (Linux reads up to it, 3.2).
6. **Latency reporting:** render FifoSize = FRAME_CFG frames, capture delay = ADC latency (3.6). The
   [INF] interpretation that FRAME_CFG is a pre-fetch FIFO comes from FRAME_CTR filling to FRAME_CFG
   after ARM (`ua_audio.c:123-126`, `ua_audio.c:3826-3832`).
7. **Link loss:** treat 0xFFFFFFFF as fatal for the stream, stop the timer (as `ua_audio.c:6032-6058`),
   and let PnP surprise-removal take over.

## 4. Constraints

| Constraint | Value | Cite |
|---|---|---|
| Rates | 44100, 48000, 88200, 96000, 176400, 192000 | `ua_audio.c:4639-4643` |
| Format | S32_LE, 24 significant bits | `ua_audio.c:4638`, `ua_audio.c:4722` |
| Access | interleaved, block transfer; no MMAP flag (copy callbacks only) | `ua_audio.c:4636-4637`, `configs/wireplumber/51-ua-apollo.conf:15-16` |
| Channels | fixed to the layout's count per direction (min = max) | `ua_audio.c:4690-4699` |
| Buffer size | fixed to the ring: 8192 frames | `ua_audio.c:4701-4714` |
| buffer_bytes_max | 0x400000 | `ua_audio.c:4646` |
| Period bytes | 256 .. 0x200000 | `ua_audio.c:4647-4648` |
| Periods | 2 .. 64 (no integer-periods constraint added) | `ua_audio.c:4649-4650`, `ua_audio.c:4671-4744` |
| Rate coupling | other direction offered only the first direction's rate; hw_params -EBUSY on mismatch | `ua_audio.c:4653-4669`, `ua_audio.c:4808-4813` |
| Clock-change refusal | -EBUSY while a stream holds another rate | `ua_audio.c:4488-4502`, `ua_audio.c:4547-4553` |
| Hi-band dwell | >= 3000 ms at 176.4/192k before leaving | `ua_audio.c:109-112`, `ua_audio.c:4341-4352` |
| Max channels per direction | 64 | `ua_apollo.h:908` |
| Clock sources | Internal 0x0C, S/PDIF 0x00, ADAT 0x05, Word Clock 0x07, Dante 0x09; must be in the device mask 0xC08C[31:16] once read; peer 0x0A hidden | `ua_apollo.h:917-928`, `ua_audio.c:4457-4486` |
| x8p clock sources | Internal, Word Clock, ADAT (no S/PDIF) | `devices/apollo-x8p.json:26` |
| Twin X clock sources | Internal, S/PDIF | `devices/apollo-twin-x.json:20` |
| Clock lock | `(0xC07C & 3) == 1` | `ua_audio.c:2008-2039` |
| Browser quanta | PipeWire-pulse floors Firefox/Chromium/Discord at 512/48000: "quantum 900/1200 ... causes choppy audio when capturing from Apollo virtual sources" | `configs/pipewire/50-apollo-pulse-rules.conf:1-24` |

## 5. ALSA to Windows mapping

### 5.1 Why not map the hardware ring straight into the Windows engine **[WIN]**

A WaveRT/ACX buffer the engine reads directly must have the endpoint's own channel count. The
hardware frame is 32-52 slots wide (1.6), its length is fixed at 8192 frames (1.3) and both
directions share one free-running position (2.1). A stereo or mono endpoint therefore cannot be
the DMA ring. Linux never mapped it either: it advertises no MMAP and moves every frame through copy
callbacks (`ua_audio.c:4885-5059`, `configs/wireplumber/51-ua-apollo.conf:15-16`). Recommended:
small per-endpoint cyclic buffers (WaveRT `AllocateAudioBuffer`, or ACX RT packets) and a driver
**copy engine** on the 1 ms timer that scatters/gathers slots between those buffers and the rings
(the Windows form of `ua_pcm_copy` and `ua_pcm_copy_mapped`).

### 5.2 PCM operations

| Linux (ALSA) | What it does here | Windows equivalent **[WIN]** |
|---|---|---|
| `snd_card_new`, `snd_pcm_new(.., 1, 1)` | one card, one playback and one capture substream (`ua_audio.c:9522-9549`) | ACX: one render and one capture circuit per endpoint (`AcxCircuitCreate`); PortCls: Wave + Topology subdevices per endpoint |
| `ua_pcm_hw` (`ua_audio.c:4635-4651`) | format, rates, channel and size limits | data ranges / format lists (`KSDATARANGE_AUDIO`, ACX `AcxDataFormatCreate`) with 32/24-bit PCM at the device rate |
| `open` (`ua_audio.c:4671-4744`) | fixes channels and buffer size, msbits 24, rate rule | stream creation (`EvtAcxCircuitCreateStream` / `IMiniportWaveRT::NewStream`); validate format and rate |
| `hw_params` / `hw_free` (`ua_audio.c:4791-4826`) | record/clear the stream rate, refuse a different rate | stream format check at creation; one device rate for all endpoints |
| `prepare` (`ua_audio.c:5822-5876`) | connect if needed, rate change, prepare + start transport, clock write, delay | `EvtAcxStreamPrepareHardware` / `SetState(KSSTATE_ACQUIRE)`: passive level, may sleep (rate changes take seconds, 2.9). Start the transport here, or keep it running all the time (6.4) |
| `trigger START/STOP` (`ua_audio.c:6574-6668`) | only picks the period-wakeup source; transport untouched | `EvtAcxStreamRun` / `EvtAcxStreamPause`, `SetState(RUN/PAUSE)`: record the hardware start offset (3.5), enable/disable this endpoint in the copy engine; never stop the transport here |
| `close` (`ua_audio.c:4746-4789`) | stop transport when both directions closed | `EvtAcxStreamReleaseHardware` / stream cleanup with a device-level reference count |
| `pointer` (`ua_audio.c:6670-6703`) | `SAMPLE_POS % buffer_size` | `EvtAcxStreamGetPresentationPosition`, `EvtAcxStreamGetCurrentPacket` / `GetCapturePacket`; WaveRT `GetPosition(KSAUDIO_POSITION)` (3.8) |
| `ioctl RESET` (`ua_audio.c:6733-6770`) | start the stream at the live position | offset captured at Run, applied in the copy engine |
| `copy`, `fill_silence` (`ua_audio.c:4885-5150`) | scatter/gather per frame, zero unused slots | the copy engine (5.1) |
| `runtime->delay` (`ua_audio.c:5790-5793`) | FRAME_CFG / ADC latency | `EvtAcxStreamGetHwLatency` (FifoSize, Delay) / `GetHardwareLatency(KSRTAUDIO_HWLATENCY)` |
| `snd_pcm_period_elapsed` (`ua_audio.c:5990`) | period notification paced by SAMPLE_POS | `AcxRtStreamNotifyPacketComplete` / `KeSetEvent` on the WaveRT notification event, fired by the copy engine when the endpoint buffer crosses a packet |
| `snd_pcm_stop(XRUN)` | forced re-prepare | no equivalent; avoid needing one: freeze conferencing slot maps, restart the transport internally on stride changes, surprise-remove on link loss |
| `snd_pcm_hw_constraint_minmax(BUFFER_SIZE, 8192)` | buffer = hardware ring | not needed with the copy engine (any engine buffer size) |
| `snd_pcm_hw_constraint_msbits(24)` | 24 valid bits | `wValidBitsPerSample = 24` |

### 5.3 Infrastructure

| Linux | Windows **[WIN]** |
|---|---|
| hrtimer 1 ms soft (`ua_audio.c:9468-9476`) | high-resolution WDF timer (`UseHighResolutionTimer`) or `ExAllocateTimer(EX_TIMER_HIGH_RESOLUTION)`, DPC level |
| hard IRQ + threaded handler (`ua_core.c:159-245`) | `EvtInterruptIsr` (ack only, as `ua_core.c:182-210`) + `EvtInterruptDpc` |
| `delayed_work` 10 ms service loop (`ua_audio.c:2558-2573`) | passive-level WDF timer or work item **[dep]** |
| `ua->lock` mutex, `audio->lock`/`timer_lock` spinlocks | WDFWAITLOCK; WDFSPINLOCK / KSPIN_LOCK |
| `dma_alloc_pages` + `vmap` (`ua_audio.c:518-556`) | common buffer(s) from a 32-bit DMA enabler (1.1) |
| `dma_sync_single_*` | coherent common buffer on x86: nothing |
| module parameters (`ua_audio.c:40-202`) | registry values under the device's Parameters key |
| `/proc/asound/cardN/*` | WPP/ETW plus debug IOCTLs |
| suspend/resume (`ua_audio.c:9277-9344`) | `EvtDeviceD0Exit` / `EvtDeviceD0Entry`: reprogram SG, reconnect, restart transport |

### 5.4 ALSA mixer controls and what they write

Mechanism **[dep]**: every write lands in a cached 38-entry mixer-setting table and goes to the
hardware on the next 10 ms service pass: wait for SEQ_RD (0x380C) == cached SEQ_WR, write each dirty
setting as two words `A = mask[15:0] << 16 | val[15:0]`, `B = mask[31:16] << 16 | val[31:16]` at
`0x3800 + 0xB4 + 8i` (i <= 15), `+0xBC + 8i` (16-31), `+0xC0 + 8i` (32-37), then SEQ_WR + 1
(0x3808); in the default mailbox mode a written setting is sent as zeros on the following pass
(`ua_core.c:1453-1535`, `ua_apollo.h:1702-1721`). Front-panel changes come back through the 40-word
readback at 0x3814 (status 0x3810) and update the controls (`ua_audio.c:8295-8434`,
`ua_apollo.h:895-898`). Puts take the device lock (`ua_audio.c:7053-7067`).

Preamp controls, one set per preamp channel `ch` (x8p 8, Twin X 2; `ua_audio.c:8482-8633`). "Setting
s0/s12" means setting 0 for ch 0-3 and setting 12 for ch 4-7; bit position `param + 6*(ch&3)`
(`ua_core.c:2556-2577`):

| Control | Range | Hardware write | Cite |
|---|---|---|---|
| Line N Capture Volume | 10..65 dB, 1 dB | GainA param 0x0A = dB-10 (setting 17/25, byte lane (ch&3)*8), GainB 0x09 = dB-10 (setting 16/24), GainC 0x06 = dB-9 (setting 3/14, 6 bits at shift 6*(ch&3) + 4 if ch&2); -EBUSY on custom/Unison preamps | `ua_audio.c:6904-6971`, `ua_core.c:2587-2616`, `ua_apollo.h:605-610` |
| Mic N 48V Phantom Power Switch | bool | param 0x03, s0/s12 | `ua_audio.c:8506-8522`, `ua_apollo.h:596` |
| Line N Pad Switch | bool | param 0x01, s0/s12 | `ua_audio.c:8524-8539` |
| Line N Phase Invert Switch | bool | param 0x05, s0/s12 | `ua_audio.c:8541-8557` |
| Line N LowCut Switch | bool | param 0x04, s0/s12 | `ua_audio.c:8559-8575` |
| Line N Input Select | Mic / Line | param 0x00 (1 = Line), s0/s12 | `ua_audio.c:7232-7275`, `ua_apollo.h:594` |
| Line N HiZ Detect Switch | read-only | readback word 0 bit 24+ch (inputs 1-2) | `ua_audio.c:8595-8612`, `ua_apollo.h:856` |
| Line N HiZ Impedance | 2.2M,1M,453k,232k,100k,70k,47k,21k | param 0x11, setting 13 nibble 4*ch; host-cached, nothing written until changed | `ua_audio.c:7277-7326`, `ua_core.c:2623-2631` |

Monitor and global controls (`ua_audio.c:8635-9194`; mapping `ua_core.c:2014-2409`):

| Control | Values | Param (ch_idx) -> setting bits | Cite |
|---|---|---|---|
| Monitor Playback Volume | 0..192, raw = 192 + 2*dB (0 = -96 dB) | 0x01 (1) -> s2 [7:0] | `ua_audio.c:7069-7113`, `ua_core.c:2032` |
| Monitor Playback Switch | 1 = unmuted | 0x03 (0) value 2 = mute, keeps mono bit -> s2 [17:16] | `ua_audio.c:7168-7201`, `ua_core.c:2042` |
| Monitor Mono Switch | bool | 0x03 (0) value bit 0, keeps mute -> s2 bit 17 | `ua_audio.c:7328-7362` |
| Monitor Dim Switch | bool | 0x44 (0) -> s2 bit 31 | `ua_audio.c:7203-7230`, `ua_core.c:2092` |
| Monitor Dim Level | -9..-60 dB, 7 steps (hw 1-7) | 0x43 (0) -> s14 [30:28] | `ua_audio.c:7464-7509`, `ua_core.c:2284` |
| Monitor Source | MIX / CUE 1 / CUE 2 | 0x04 (1) -> s2 bit 29 + [19:18] | `ua_audio.c:7364-7410`, `ua_core.c:2055` |
| Headphone 1/2 Source | CUE 1 / CUE 2 | 0x3f / 0x40 (1) -> s12 [26:24] / [29:27] | `ua_audio.c:7412-7462`, `ua_core.c:2253-2258` |
| Headphone 1 Playback Volume | 0..192 | 0x02 (1) -> s2 [15:8] | `ua_audio.c:7115-7154`, `ua_core.c:2037` |
| Headphone 2 Playback Volume | read-only mirror | readback word 7 [7:0], only on models with <= 4 preamps (so never updated on the x8p) | `ua_audio.c:7116-7121`, `ua_audio.c:8427-8433` |
| Monitor Talkback Switch | bool | 0x46, ch_idx 1 on / 9 off -> s13 bit 8 | `ua_audio.c:7511-7541`, `ua_core.c:2276` |
| Digital Output Mode | S/PDIF / ADAT (hw 0 / 8) | 0x21 (0) -> s8 [7:0]; re-reads descriptors; only on per-direction TOSLink models (x8p yes, Twin X no) | `ua_audio.c:7543-7597`, `ua_audio.c:8842-8859` |
| Digital Input Mode | S/PDIF / ADAT | host-side only (picks record routes); registered only with io_map enabled (Twin X yes, x8p not by default) | `ua_audio.c:7599-7657`, `ua_audio.c:8861-8879` |
| Output Reference Level | +4 dBu / -10 dBV | 0x32 (1) -> s11 bit 4 | `ua_audio.c:7659-7699`, `ua_core.c:2230` |
| CUE 1/2 Mirror Source | None, S/PDIF, Line 1-2, Line 3-4, ADAT 1-2 .. 7-8 (hw 0xFFFFFFFF, 6, 8, 10, 16, 18, 20, 22) | 0x2e / 0x2f (9) -> s6 [7:0] / s7 [7:0] | `ua_audio.c:7728-7806`, `ua_core.c:2126-2144` |
| CUE 1/2 Mirror Switch | bool | 0x3b / 0x3c (9) -> s15 bit 8 / bit 12 | `ua_audio.c:7808-7846`, `ua_core.c:2312-2317` |
| Digital Mirror Switch | bool | 0x1e (9) -> s6 bit 10 | `ua_audio.c:7848-7877`, `ua_core.c:2121` |
| Identify Switch | bool | 0x1d (0) -> s6 [13:11] | `ua_audio.c:7879-7908`, `ua_core.c:2115` |
| S/PDIF SRC Switch | bool | 0x1f (1) -> s1 bit 31 | `ua_audio.c:7910-7939`, `ua_core.c:2024` |
| DSP Spanning Switch | bool | 0x16: no hardware write (cache only) | `ua_audio.c:7941-7970`, `ua_core.c:2409` |
| CUE 1/2 Mono Switch | bool | 0x06 / 0x08 (0) -> s2 [21:20] / [23:22] | `ua_audio.c:7972-8010`, `ua_core.c:2071-2082` |
| CUE 1/2 Mix Switch | inverted: hw 0 = on, 2 = off | 0x05 / 0x07 (0) -> s2 [25:24] / [27:26] | `ua_audio.c:8012-8054`, `ua_core.c:2066-2077` |
| Sample Rate (card) | 44100..192000 | clock change (2.8) | `ua_audio.c:8056-8106` |
| Clock Source (card) | Internal 0x0C, S/PDIF 0x00, ADAT 0x05, Word Clock 0x07 | clock change with source; committed on ack | `ua_audio.c:8108-8205` |
| Clock Mode (card) | read-only Internal/External | derived from Clock Source | `ua_audio.c:7701-7726` |
| Clock Locked (card) | read-only | `(0xC07C & 3) == 1` | `ua_audio.c:8207-8216` |

Controls that matter for a conferencing signal path:
* **Capture:** preamp gain, 48V (condenser mics), Mic/Line, Pad. The x8p preamps captured with no
  extra DSP setup on Linux (`devices/apollo-x8p.json:14`); the cached gain starts at 0 and is synced
  from the readback, so do not write defaults at start (`ua_audio.c:9593-9606`, `ua_audio.c:8389-8398`).
* **Playback to speakers:** host channel MON L/R reaches the monitor outputs; Monitor Volume/Mute
  mirror a hardware knob that "applies itself" (`ua_audio.c:8299-8301`). Leave them alone at start:
  writing setting 2 at connect "corrupts monitor standalone operation" (`ua_audio.c:3283-3287`).
* **[WIN]** Expose none of these as standard Windows endpoint volume nodes at first (the Linux node
  uses software volume, `configs/wireplumber/51-ua-apollo.conf:36-37`); put them behind a private
  IOCTL for a control app, the way the mixer daemon uses the driver.

## 6. Endpoint recommendation for conferencing

### 6.1 Rationale from the routing data

* Every model's route tables put the analog inputs first in record and MON L/R first in playback
  (`ua_models_gen.h:158-189`, `ua_models_gen.h:197-201`, `ua_models_gen.h:228-239`), and the captured
  x8p descriptor agrees (`ua_routing.h:148-151`, `ua_routing.h:223`). On the x8p these slots are the
  same in the legacy and descriptor layouts (1.6).
* The verified Linux desktop path is exactly this: PipeWire loopbacks expose "Apollo Mic n" (one
  mono source per analog input), "Apollo Mic 1+2" (stereo), and "Apollo Monitor L/R" (sink on MON
  L/R) (`configs/pipewire/setup-apollo-io.sh:456-497`). Desktop audio "work[s] through Apollo
  Monitor" and WebRTC capture is clean (`README.md:50-52`).
* The raw multichannel node is a known hazard: "apps selecting 'Apollo x4 Pro' (22ch) as input can
  hang the system" (`README.md:91`).

### 6.2 Apollo x8p

| Endpoint | Type | Channels | Hardware slots (both layouts) |
|---|---|---|---|
| Apollo Mic 1 (default communications capture) | capture, mono | 1 | record slot of ID 0x0001 = slot 0 |
| Apollo Mic 1+2 | capture, stereo | 2 | 0x0001, 0x0002 = slots 0, 1 |
| Apollo Monitor (default render) | render, stereo | 2 | playback 0x0901, 0x0902 = slots 0, 1 |
| optional: Apollo Headphones 1 | render, stereo | 2 | 0x0A01, 0x0A02 (CUE1; HP1 follows CUE 1 by default, `ua_audio.c:9612`) = legacy slots 26-27, descriptor slots 28-29 |
| optional: Apollo Mic 3..8 | capture, mono | 1 each | slots 2-7 |

Program the hardware the way it is verified: legacy counts AX_PLAY_CH = 34, AX_REC_CH = 32,
P2P_ROUTE = 0x121 (1.6, 2.3), at 48 kHz on internal clock. Read and log the descriptor anyway, map
endpoint slots by channel ID, and offer descriptor counts (34/52) as a registry switch until an x8p
run confirms them.

### 6.3 Apollo Twin X

| Endpoint | Type | IDs |
|---|---|---|
| Apollo Mic 1 (default capture) | mono | 0x0001 |
| Apollo Mic 1+2 | stereo | 0x0001, 0x0002 |
| Apollo Monitor (default render) | stereo | 0x0901, 0x0902 |
| optional: Apollo Headphones | stereo | 0x0A01, 0x0A02 ("HP L/R", `devices/apollo-twin-x.json:70-71`) |

Descriptor layout is mandatory (no legacy row, 1.7). Expect a DSP-layer dependency for capture
(`input_strips=1` on Linux, `README.md:133`) **[dep]**.

### 6.4 Full layout and rate policy **[WIN]**

* Do not expose the 32/34-channel (or descriptor-sized) layout to the shared-mode engine for the
  conferencing product. It needs no more than two channels each way, its order beyond the first slots
  is not hardware-verified on the x8p (1.6), and a big default-capable endpoint invites apps to pick
  it (6.1). If DAWs need it later, add it as a separate, non-default endpoint (or ASIO) sharing the
  same transport and copy engine.
* 48 kHz only to begin with (44.1 kHz optional). That keeps FRAME_CFG at 8 frames and avoids every
  Hi-band hazard (7, R3-R5), and only 48 kHz is verified on the x8p (`devices/apollo-x8p.json:14`).
* Keep the transport running from D0 entry to D0 exit (zeroing the playback ring when no render
  endpoint runs) instead of starting and stopping it with streams: Linux effectively does this via
  PipeWire (2.7), and transport churn is the root of issue #42 (7, X1).

## 7. Known bugs, fixes and fragile spots

Transport and DMA:
* **D1** x8p: DMA_CTRL reset strobes re-asserted after SG programming (DMA_CTRL 0x1ffff, CTRL
  readback 0x28f), SAMPLE_POS/FRAME_CTR frozen; fix = re-clean DMA_CTRL at every prepare
  (`ua_audio.c:3697-3719`, fix 6016d4e per `devices/apollo-x8p.json:14`); ring init writing all eight
  strobes broke DSP rings after a power cycle until `dma_per_dsp` (`CHANGELOG.md:226`, `ua_dsp.c:795-801`).
* **D2** SG table never programmed on ring-connect devices: SAMPLE_POS frozen at 0, host lockup
  (`ua_core.c:4356-4363`); stale SG from the previous OS on warm boot (`ua_core.c:4282-4288`); zero
  entries -> IOMMU fault -> PCIe fatal (`ua_audio.c:811-825`).
* **D3** Never rewrite SG with the transport armed (`ua_audio.c:746-760`).
* **D4** 100 ms DMA drain before freeing rings or the TB bridge dies (`ua_audio.c:9688-9696`).
* **D5** 64-bit buffer at 0x19f800000 failed; kept below 4 GiB (`ua_core.c:4747-4756`).
* **D6** 4 MiB contiguous allocation fails after fragmentation; per-page allocation (`ua_audio.c:471-479`).
* **D7** Stride change: zero the playback ring or it plays noise (`ua_audio.c:1416-1421`).
* **D8 [INF]** trigger STOP does not clear the playback ring (`ua_audio.c:6637-6663`) while the
  transport may keep running for capture (`ua_audio.c:4770-4786`); the free-running FPGA keeps
  reading the ring, so stale audio could loop. Zero it on render stop in the Windows driver.
* **X1** Issue #42: open/prepare churn against a stalled transport wedges the PCIe link (hard
  freeze); churn guard (`ua_audio.c:5548-5586`); TIMER_CFG rewrite under a running transport was a
  suspect (`ua_audio.c:3667-3669`, `ua_audio.c:5776-5778`).
* **X2** Transport start at probe killed the link on Maple Ridge (AER) (`ua_audio.c:3366-3375`).
* **X3** ARM -> START too fast: start silently ignored (`ua_audio.c:2775-2784`); FRAME_CTR gate
  (`ua_audio.c:3826-3855`, `CHANGELOG.md:203`).
* **X4** P2P_ROUTE must be written after ARM (`ua_audio.c:3807-3812`); FRAME_CFG was wrong (0x10) at
  48 kHz before (`ua_audio.c:3768-3775`).
* **X5** AX_CONTROL bits 7/8 are fault flags; the stuck x8p read 0x28f (bit 7 set)
  (`ua_audio.c:2041-2079`, `ua_audio.c:3700-3704`). Log and count them.
* **X6** ASPM L0s on the Apollo link: steady correctable Replay Timer Timeouts while streaming; L0s
  disabled (`ua_audio.c:3928-3944`, `CHANGELOG.md:243`).
* **X7** Acking unmasked ISR bits -> MSI storm (`ua_core.c:172-181`).

Position, periods, timers:
* **P1** Duplex start on a running transport: 7 playback xruns -> 1 with live start
  (`ua_audio.c:186-202`, `CHANGELOG.md:230`).
* **P2** One stream stopping took the other's period wakeups ("fail with an I/O error after 10 s")
  (`CHANGELOG.md:246`, `ua_audio.c:6637-6663`).
* **P3** `hrtimer_cancel` from trigger STOP inside the timer callback deadlocks; use try-cancel
  (`ua_audio.c:6645-6649`).
* **P4** Timer armed after removal raced; START re-checks shutdown under the timer lock
  (`ua_audio.c:6600-6611`, `ua_audio.c:6258-6279`, `driver/tests/test_period_timer_lifetime.py:1-10`).
* **P5** Substream used after close; the period check holds the lock that close clears the pointer
  under (`ua_audio.c:6060-6066`, `driver/tests/test_pcm_notification_lifetime.py:1-12`).
* **P6** Hard IRQ used to call period_elapsed on top of the hrtimer: double wakeups and a SAMPLE_POS
  read in hard-IRQ context (`ua_audio.c:5908-5912`, `ua_audio.c:6861-6867`).
* **P7** BAR0 reads in hard-IRQ context with interrupts off stall a CPU on a wedged link; the timer
  moved to softirq (`ua_audio.c:6005-6011`). "Firefox Snap crashes: kernel hrtimer lockup when using
  WebRTC capture" (`README.md:90`).
* **T1** Period-timer interrupt originally fired once and stopped (`ua_audio.c:5999-6003`).
* **T2** Second interrupt for the same boundary on 8-15% of boundaries at 48 kHz when re-armed from
  the hard IRQ (`ua_audio.c:6366-6377`, `ua_apollo.h:1175-1181`).
* **T3** Timer 2-14% fast (13% in the changelog) when started before the clock locked after
  192 -> 48 kHz (`ua_audio.c:5475-5491`, `CHANGELOG.md:232`).
* **T4** Timer 23 and 296 frames off the boundaries after a restart without rewriting TIMER_CFG
  (`ua_audio.c:3740-3745`).
* **T5** TIMER_CFG = interval drifts a frame per period; interval - 1 is exact (`ua_audio.c:3519-3527`).

Clock and rate:
* **R1** Playback/capture rate ping-pong (~5 changes/s) left the DSP recording silence until a power
  cycle (`ua_audio.c:4726-4734`, `CHANGELOG.md:269`).
* **R2** Clock write must follow transport start or capture routes silence (`ua_audio.c:5152-5161`);
  source 0 (S/PDIF) never acks without a signal (`ua_audio.c:5162-5164`).
* **R3** Leaving 176.4/192k within ~3 s wedged the x4's firmware rate (silence until reload or power
  cycle); 3000 ms dwell (`ua_audio.c:4341-4352`, `CHANGELOG.md:222`).
* **R4** A Hi-band start with the clock already Hi-band records nothing (192k) or fails 1 in 6
  (176.4k); a 48 kHz session of 400 ms (from 192k) or 1200 ms (from 176.4k) first fixes it
  (`ua_audio.c:5230-5259`, `CHANGELOG.md:244`).
* **R5** First non-zero FRAME_CTR at Hi band was 4 of 0x20 (`ua_audio.c:123-126`).
* **R6** Notification status is cleared by writing 0, not W1C, and only after every bit was handled
  (`ua_audio.c:2144-2156`, `ua_audio.c:2400-2401`, `CHANGELOG.md:231`).
* **R7** A rate change used to leave the device disconnected with the service loop stopped
  (`ua_audio.c:5619-5625`).
* **R8** "WebRTC capture pitch drift ... ~1-3s when browser capture starts" from PipeWire's DLL
  lock-in (`README.md:92`); browser quanta 900/1200 caused choppy capture (`configs/pipewire/50-apollo-pulse-rules.conf:1-3`).

Connect and lifecycle **[dep]**:
* **C1** A host that left without disconnecting blocks the next ACEFACE until AX_CONNECT = 0x10
  (`ua_audio.c:2608-2623`).
* **C2** Repeated 20 s ACEFACE loops from prepare froze the system (`ua_audio.c:5668-5687`).
* **C3** The soft disconnect must not send 0x10 while the mixer DSP is healthy, or the readback dies
  until a firmware reload (`ua_audio.c:3439-3454`).
* **C4** Inherited DSP state from Windows/macOS: ring connect drops input strips (mic records digital
  zero) (`CHANGELOG.md:221`); now kept only on the x4 (`ua_core.c:4252-4255`, `CHANGELOG.md:50-52`).
* **C5** `alsactl restore` can freeze the DSP (`CHANGELOG.md:299`). **[WIN]** Do not blindly restore
  all mixer state at start.

## 8. Not verified, open questions

* x8p: rates other than 48 kHz, digital I/O, clock sources, io_map=1 (`devices/apollo-x8p.json:14`,
  `ua_audio.c:305-316`); which descriptor slots a 32-slot legacy frame carries past slot 15 (1.6).
* Twin X: no hardware descriptor; Gen 1 untested; capture needed input strips on Gen 2
  (`README.md:133`).
* Whether SAMPLE_POS marks the DMA fetch point or the converter point, and therefore whether FRAME_CFG
  should be added to or subtracted from it for render presentation position (3.6, 3.8).
* SAMPLE_POS read latency over Thunderbolt (no figures in the source, 3.2).
* Whether the period timer is phase-locked to the ring wrap for intervals that do not divide 8192
  (`ua_audio.c:6281-6307` only allows dividing intervals).
* Meaning of DMA_BLKSZ, P2P_ROUTE, 0xC080 and the latency words beyond what the formulas say.
