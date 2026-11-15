# Our own DSP

Started Nov 14 2026: our own processing, host-side. UA's licensed UAD plug-ins can also run on the
units' DSPs on our driver (open-apollo 2.1 hosting, port plan in [PLUGINS.md](PLUGINS.md)); this chain
is the complement, not a replacement.

## What exists

`dsp/avdsp.c` + `dsp/avdsp.h`: a voice chain written from scratch in portable C11, 32-bit float,
no allocation, no OS calls. Per channel:

low cut (12/24 dB/oct) -> gate/expander -> 4-band EQ -> compressor -> de-esser -> output gain -> lookahead limiter

| Stage | Design |
|---|---|
| Filters | RBJ cookbook biquads (TDF-II), coefficients computed in double |
| Gate | downward expander, ratio 1.5-100 (100 = hard gate), range, hysteresis, hold |
| Compressor | feed-forward, soft knee, peak or RMS detector, attack/release smoothing in dB |
| De-esser | dynamic high shelf driven by a high-pass detector (4:1, capped at the range). Exactly transparent when idle |
| Limiter | 1.5 ms lookahead; gain = moving average of a sliding minimum, so the output cannot exceed the ceiling |

Every stage crossfades over 10 ms when switched; EQ and low-cut moves are smoothed. Latency is
the limiter's lookahead (72 samples at 48 kHz) and never changes while running. Parameters are
one flat table (`av_voice_params_info`: name, range, default, unit), so the render tool, the
app and a future APO share one definition. Presets: clean, voice, podcast, broadcast.

## Tools

- `dsp\bin\dsp-test.exe`: 45 checks. Filter responses measured against the design (within
  0.0001 dB), the compressor's static curve (within 0.1 dB), gate open/hold/close and
  hysteresis, de-esser reduction and bit-exact transparency, limiter ceiling and latency,
  bit-exact passthrough with everything off, click-free switching, extreme and random
  parameters, no denormals. Speed: the full podcast chain on 8 channels uses about 3.3% of one
  core.
- `dsp\bin\dsp-render.exe in.wav out.wav --preset voice [--set comp.ratio=4 ...]`: process a
  recording (PCM 16/24/32 or float; any rate). `--list` prints every parameter.
- Build: `dsp\build.cmd` (also part of `tools\build-all.cmd`); it runs the checks.

## Where it runs next

1. **Windows, after phase 2**: an Audio Processing Object (APO) on the x8p's capture endpoints,
   so Zoom, Discord and OBS get the processed signal. Apollo Control becomes its panel. The APO
   runs in Windows' audio engine (user mode): a bug stops audio, not the PC. Not in the Apollo's
   zero-latency monitor path.
2. **The Apollo's SHARC DSPs: research, not started.** What is known and what is not:

| Known (open-apollo, spec-dsp-mixer section 1) | Unknown |
|---|---|
| The DSPs accept code as "Bill" records (`Bill`, id, 0x02000000, payload size, w4, payload) uploaded over the DSP command rings to the code pool at 0xE0000, acknowledged with `0x80070004` | How a module's code reaches its audio: buffers, calling convention, parameter memory |
| UA's own Console strip modules load this way on the x4 | How modules are wired into the mixer graph beyond replaying UA's strip sequence |
| The DSPs are SHARC (native 32-bit float), so this C compiles for them | Whether the runtime accepts an unknown module id; a bad module quarantines the DSP until a power cycle (no known risk beyond that) |
| | Toolchain: Analog Devices CrossCore Embedded Studio (paid licence) or our own assembler from ADI's public instruction-set reference |

   Order, if pursued: phase 1 on hardware, then the input strips (phase 3), then disassemble the
   smallest UA strip module (0x0C2, 184 bytes) to learn the module ABI, then load a do-nothing
   module of our own, then a gain stage, then this chain.
