# ASIO for the x8p (Ableton, Avium Measure)

Decided Nov 14 2026 (PLAN.md decision 9). Builds on the phase 2 copy engine (PHASE2.md §5): ASIO
is one more client of the 1 ms engine tick, next to the ACX streams.

## Pieces

| Piece | Where | Licence |
|---|---|---|
| Kernel: shared double buffers, a period event, engine hook | `apollo.sys` (`asio.c`, new IOCTLs) | GPL-2.0 |
| ASIO driver: COM in-proc server implementing `IASIO` | `asio\avium-asio.dll` (C++) | GPLv3, using the ASIO SDK under its GPLv3 option (dual-licensed since Oct 2025); a separate program that talks to the kernel driver only through IOCTLs |
| Registration | `HKLM\SOFTWARE\ASIO\Apollo x8p (Avium)` (CLSID, Description) + `HKCR\CLSID\{...}\InprocServer32` (path, `ThreadingModel=Apartment`); `regsvr32` | |

The ASIO SDK is downloaded from Steinberg into `apollo-win\third_party\asiosdk\` (headers
`asio.h`, `asiodrvr.h`, `iasiodrv.h`, `asiosys.h`); its LICENSE.txt states the dual licence.

## Data path

```
Ableton  <-- bufferSwitch(i) --  avium-asio.dll worker thread (MMCSS "Pro Audio")
                                   | waits on the period event
                                   v
shared memory (mapped into the client): control block + 2 halves x channels x N frames (Int32)
                                   ^ engine tick copies, 1 ms, DISPATCH
x8p hardware ring (8192 frames, S32 MSB-aligned 24-bit, interleaved, descriptor slot order)
```

- Sample type `ASIOSTInt32LSB`: the hardware words are already 32-bit with 24 valid bits at the
  top, so no conversion, only de-interleave/interleave.
- Rate: 48000 only (`canSampleRate` true for 48000 alone). Clock source: Internal.
- Channels: every record slot as an input (32 now, 34 once REC_CH is 34) and every playback slot as
  an output (34, 36 later), named from the firmware descriptor: `MIC/LINE 1` ... `ADAT 1` ...
  `S/PDIF L` ... `VIRTUAL 1` ... `MON L/R`, `AUX 1 L` ..., outputs `MON L/R`, `LINE 1-8`, `ADAT`,
  `S/PDIF`, `VIRTUAL`, `CUE 1-4 L/R`.
- Buffer sizes: min 64, preferred 128, max 1024, power-of-two granularity. 32 needs the hardware
  period-timer interrupt (later), not the 1 ms tick.

## Kernel side

**Status (Nov 14):** implemented in `driver\asio.c` with the engine in `driver\engine.c`, built clean,
not yet run on the hardware. Timing, deviations and the test steps (`asio-test --run 10 128`,
`regsvr32`, Ableton) are in PHASE2.md §9-§10. Output frame `a` plays at `Origin + a + 2N + guard +
lead`, so the host has exactly one period after `bufferSwitch` before its half is due; a late half
plays silence and counts an underrun. After the independent review (PHASE2.md §9.3 F1, F3, F4):
the shared block is a pagefile-backed section (locked for the engine, one view in the client),
only the process that opened the handle may use ASIO on it, and the handshake is per half
(`ClientDone[2]`, `APOLLO_ASIO_VERSION` 2): the DLL stores the SwitchCount it processed in
`ClientDone[half]`, the engine plays a half only on an exact match, and the DLL counts the switches
it skipped in `ClientSkipped`.

New IOCTLs (APOLLO_USER: audio streaming is something any Windows app may do; one ASIO client at a
time, `STATUS_DEVICE_BUSY` for a second):

| IOCTL | In | Out | Notes |
|---|---|---|---|
| `ASIO_OPEN` | buffer frames, input slot mask, output slot mask, event HANDLE | user address of the shared block, its size | Must run in the caller's context to map memory into its process: handled in `EvtIoInCallerContext` (WdfDeviceInitSetIoInCallerContextCallback), the rest forwarded to the queue. Memory: `ZwCreateSection` (pagefile-backed), `MmMapViewInSystemSpace` + `MmProbeAndLockPages` + `MmGetSystemAddressForMdlSafe` for the engine, `ZwMapViewOfSection` (read/write, `ViewUnmap`) into the caller (was: `MmAllocatePagesForMdlEx` mapped with `MmMapLockedPagesSpecifyCache(UserMode)`). Event: `ObReferenceObjectByHandle(EVENT_MODIFY_STATE, *ExEventObjectType, UserMode)`. |
| `ASIO_START` / `ASIO_STOP` | none | none | join/leave the engine's active list under EngineLock |
| `ASIO_CLOSE` | none | none | also done in `EvtFileCleanup` / `EvtFileClose` if the client dies; the view is removed with `ZwUnmapViewOfSection` from any context |

Shared control block (first page), written by the engine, read by the DLL:
`Version, BufferFrames, InCount, OutCount, InSlots[64], OutSlots[64], HalfReady (index of the half
the client must process now), SwitchCount, SamplePosition (64-bit, frames), Qpc, QpcFrequency,
Overruns, Underruns`. Buffers follow, page-aligned: `In[2][InCount][N]`, `Out[2][OutCount][N]`.

Engine tick, per ASIO client (same timing model as an ACX stream, PHASE2.md §5.4):
- Capture: copy hardware frames up to `HwPos - CaptureGuardFrames` into the current input half.
- Render: copy the client's output half into the ring up to `HwPos + RenderLeadFrames`.
- When a half fills: publish `HalfReady`, `SamplePosition`, `Qpc` (interlocked), `KeSetEvent`.
- If the client has not returned (`outputReady` or the next wait) before its half is due, count an
  underrun and play zeros for it.

**Engine change shared with ACX:** render clients must ADD into the ring (saturating), not
overwrite, so Windows audio and Ableton can both play to MON at once (as UA's driver allows). The
zero horizon (4096 frames ahead) already clears every frame before any client writes it.

## DLL side (`IASIO`)

| Method | Behaviour |
|---|---|
| `init` | open the device (GENERIC_READ), check `APOLLO_INFO` (Connected, transport running, 48 kHz) |
| `getChannels`, `getChannelInfo` | from the descriptor-order tables above |
| `getBufferSize` | 64 / 128 / 1024 / -1 |
| `createBuffers` | `ASIO_OPEN`; hand out pointers into the shared halves; start the worker thread |
| `start` / `stop` | `ASIO_START` / `ASIO_STOP` |
| worker | `WaitForSingleObject(event)`, read `HalfReady`, call `bufferSwitchTimeInfo` (with `SamplePosition` and system time from `Qpc`) or `bufferSwitch` |
| `getSamplePosition` | `SamplePosition` + nanoseconds from `Qpc` |
| `getLatencies` | input: N + capture guard + ADC + strip path; output: N + RenderLeadFrames + FRAME_CFG + DAC (section Latency, Nov 15) |
| `outputReady` | supported: tells the engine the half is ready, so it can be queued at once |
| `controlPanel` | launch `apollo-control.exe` |
| `future` | `kAsioCanTimeInfo`, `kAsioCanTimeCode` false, `kAsioSupportsTimeInfo` |

## Latency estimate at 128 frames

Output ≈ 128 + 144 + 8 = 280 frames (5.8 ms); input ≈ 128 + 10 + 16 = 154 frames (3.2 ms). The
render lead (144) is the knob: the phase 2 glitch test decides how low it can go. The hardware
period-timer interrupt (vector 0x46, `AX_TIMER_CFG`) can later replace the 1 ms tick for ASIO to
reach 64 or 32 frames.

## Latency (Nov 15 2026, warmplug: built, offline tests only)

What `getLatencies` (both ASIO drivers), `GET_RATE` (`InPathFrames`, `DacFrames`), `apollo-diag rate` ("Reported") and
the Windows endpoints (`StreamGetHwLatency` Delay) report, in frames at the unit's rate, beyond the buffer N:

| Part | Input | Output | Source |
|---|---|---|---|
| Copy engine | capture guard (16 at 48 kHz, scaled per rate) | render lead (144, scaled) + FRAME_CFG (8 / 16 / 32) | ours (engine.c, rate_plan.h) |
| Converter | ADC: x8p 10 (11 at 176.4/192), Twin X 5 (6) | DAC: x8p 43 / 15 / 16, Twin X 44 / 16 / 17 (per band, rounded) | UA's Mixer Engine tables via `oa:devices/*.json` (`bringup_plan.h` AdcFrames, DacCenti) |
| Strip path, only while the unit's strips run | the strip DSP's block: 64, 128 above 100 kHz (twice the strips' 32 / 64 block size) + the plain strips' record delay 200 | | UA's `GetAsioLatencyForDelayComp` (`oa:mixer-engine/model_features.py`); 200 = our strip recipe's m0 word (`strip_plan.h`), measured upstream (a record delay of 1000 recorded exactly 800 later) |
| Over-budget plug-in chain | its record path (record delay + plug-in latency through the tap) over 200; the worst routed chain counts | | Apollo Control tells the driver after each build (op LATENCY, `apollo_plugin.h`); chain `latency` / `recordDelay` as `docs/apollo-control-units.md` 1.9 |
| Combined device (agg) | the aligned layout (timeline guard + Pad: the ASRC look-ahead, the margin and the alignment of the units' ADC/trims, as before) + the largest strip path of the joined units | timeline lead + FRAME_CFG + the largest DAC | agg.c (InExtra, OutExtra) |

At 48 kHz on the x8p with its strips running: input N + 16 + 10 + 264 = N + 290, output N + 144 + 8 + 43 = N + 195 (was
N + 26 and N + 152). A chain within the 200-sample budget changes nothing (its record delay absorbs its latency, as UA's
delay compensation does); one over it (UA would say "overloaded") adds the excess for every input, since ASIO has one
input latency: its own input lines up, the others land that much early. The strip path is added only while strips run
(no strip, no DSP on the record path; the mic inputs record silence then anyway).

Changes reach the host: the 10 ms service pass republishes the figures when the strip path, a chain or the rate moves
(`asio.c ApolloLatencyRefresh`; logged as `latency: reported input ...`), into the unit's shared block and the combined
device's; the DLL compares them on every wake-up and sends `kAsioLatenciesChanged` (the combined client also on its own
latency moves, and `kAsioResetRequest` for a layout or timeline change, as before). Before `createBuffers` the single-unit
DLL answers from `GET_RATE`. Windows asks a stream once at creation: a chain added later counts for streams opened after it.

**Not known, not added (documented guesses avoided):** UA's own host-side pads (+8 in, +4 out: replaced by our engine's
guard, lead and FRAME_CFG); per-channel differences (ADAT, S/PDIF and the virtual inputs have no strip here, the outputs
39 / 41 / 43 frames by kind: the DAC figure is UA's delay-compensated maximum, the monitor path's); the DAC rows' unit
(frames, inferred from the input row and UA's use of both in one sum). **Verify with a loopback** (an x8p line out into
input 1 with a cable, Avium Measure's round trip, or a DAW's latency test): the measured round trip should be the
reported input + output (2N + 485 at 48 kHz) within a few frames.

## Order

After PHASE2.md steps 0-1 (ACX init, copy engine) pass on the x8p. Then: kernel IOCTLs + shared
block, the DLL, registration, then Ableton at 128 frames for 30 minutes and Avium Measure's
loopback measurement as the acceptance tests.
