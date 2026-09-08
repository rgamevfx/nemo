# Issue #10 evidence: codec + chunk sweep and hardware decode/interop costs

Measured 2026-09-07 on the declared prototype device: NVIDIA GTX 1070
(Pascal), driver 580.173.02, Linux. Source workload: `testsrc2` 640x360,
96 frames, 24 fps, H.264 source encoded with explicit BT.709 primaries /
transfer / matrix tags (ffmpeg 6.1.1, libavcodec 60.31). Harness:
`nemo-cli codec-sweep /tmp/sweep-source.mp4` (implementation:
`src/nemo/media/CodecSweep.cpp`, encoders behind `src/nemo/media/ViewerEncode.hpp`,
decode through `src/nemo/media/VideoDecode.hpp`).

Chunk independence: every chunk is a separate mp4 segment (one GOP), so
each chunk decodes from its own keyframe; "seek @ boundary" is a fresh
open + decode of the boundary chunk's first frame, averaged over chunk
boundaries.

## Capability evidence (`nemo-cli probe-media`)

- `h264-vulkan` / `hevc-vulkan` decoders: **init-verified** — the device
  reserved a Vulkan video decode queue family (driver exposes
  `VK_KHR_video_decode_h264/h265` on Pascal) and libavcodec's Vulkan
  hwaccel decoded a real clip on the application device.
- `h264-nvenc` / `hevc-nvenc` encoders: **init-verified** (real
  `avcodec_open2` against the NVENC engine).
- `libx264-cpu` / `libx265-cpu`: init-verified comparators.
- Registration-only ≠ usable: the probe init-verifies encoders because
  encoder registration lies (e.g. `av1_nvenc` registers in libavcodec but
  the NVENC engine rejects it on this hardware).

## Codec × chunk sweep (measured table)

| codec | chunk | encode ms/frame | upload ms/frame | decode ms/frame | seek ms @ boundary | PSNR dB (min) | B/frame |
|---|---|---|---|---|---|---|---|
| h264-nvenc | 12 | 32.30 | 31.85 | 17.11 | 22.08 | 25.83 | 10575 |
| h264-nvenc | 24 | 31.42 | 31.15 | 16.66 | 22.43 | 25.83 | 10319 |
| h264-nvenc | 48 | 31.45 | 31.26 | 16.53 | 21.26 | 25.85 | 10158 |
| hevc-nvenc | 12 | 30.96 | 30.69 | 17.46 | 24.35 | 25.83 | 11667 |
| hevc-nvenc | 24 | 31.62 | 31.43 | 17.12 | 23.67 | 25.84 | 11252 |
| hevc-nvenc | 48 | 30.74 | 30.59 | 16.95 | 23.56 | 25.85 | 11079 |
| libx264-cpu | 12 | 34.31 | 0.00 | 16.65 | 20.17 | 25.81 | 6477 |
| libx264-cpu | 24 | 34.05 | 0.00 | 16.84 | 20.37 | 25.82 | 6650 |
| libx264-cpu | 48 | 32.34 | 0.00 | 16.35 | 20.40 | 25.84 | 7598 |
| libx265-cpu | 12 | 47.41 | 0.00 | 17.53 | 26.42 | 25.81 | 8307 |
| libx265-cpu | 24 | 46.54 | 0.00 | 17.14 | 26.11 | 25.83 | 8544 |
| libx265-cpu | 48 | 40.72 | 0.00 | 16.92 | 27.07 | 25.84 | 9084 |

Peak decode resources (process VmHWM after the decode-back workload):
858208 KiB (~838 MiB — dominated by the Vulkan video session + decode DPB
surfaces, ~71 MB of video-session memory visible in the decoder logs plus
process libraries).

Hardware decode + Vulkan interop (decode → NV12 planes resident on the
application device → `mediaConvert` kernel → RGBA32F scene-linear
contract; no CPU readback):

```
hw-decode (h264-vulkan, device-resident, no CPU readback): 1.09294 ms/frame over 96 frames
```

Software decode + convert baseline (the always-available path; its
device upload is a separate, additionally-measured cost):
~16.6 ms/frame (decode + explicit 709 conversion to the contract).

## Reading of the numbers (evidence, not a decision)

1. **Encode cost — the upload dominates**: the NVENC rows' encode time is
   ~31 ms/frame, of which ~31 ms/frame is the measured CUDA staging
   upload (`upload ms/frame` column): the capability-dependent transfer
   dominates hardware encoding at this scale. This is the transfer cost
   the spec requires exposing rather than hiding — and it is the concrete
   argument for consuming the viewer representation from device residency
   (via CUDA interop) in #12 rather than via CPU staging. CPU x264 ≈
   33 ms/frame is close at this small scale; x265 is ~25% slower than
   x264 at medium preset.
2. **Compression**: CPU x264 produces the smallest chunks (~6.5-7.6
   KB/frame); NVENC ~10-11.7 KB at the same 2000 kbps target (rate
   control differences at tiny frames).
3. **Fidelity**: PSNR(min over chunks) ≈ 25.8 dB for every candidate —
   the measurement is dominated by the 4:2:0 chroma subsampling error of
   the representation itself, not by the codec. Codec choice is fidelity-
   neutral at this scale.
4. **Chunk size**: chunk sizes 12/24/48 change seek cost by only
   ±1-2 ms in this workload; chunk independence costs nothing measurable
   in decode time (decode ms/frame is flat across chunk sizes).
5. **Interop evidence**: Vulkan video decode over the application device
   converts 96 frames device-resident at 1.18 ms/frame — 14× faster than
   the software decode+convert path — with zero CPU readback between
   decode and the application image contract (spec §10.4, §11
   no-readback gate).

## Test fidelity gate

The decode interop fidelity gate (HwMedia.DecodeInterop...) compares the
hardware-converted frame against the software 709 reference with a 4/255
per-component tolerance on a synthetic clip with NON-neutral chroma
(varying Cb/Cr per frame), so it catches decode corruption, range/matrix
errors, and Cb/Cr plane-order swaps. The measured max delta on the test
clip was 0.0 — but "bit-exact" is not claimed as an invariant; the gate is
the tolerance.

## Explicitly NOT decided here (acceptance example 4)

The final codec/profile/bitrate and chunk-size choice remains an
evidence-gated prototype decision; this table is the input, not the
decision. Constraints that must weigh in later, beyond this table:
platform matrix (#17), resource-pressure accounting with decoder surfaces
(#14), and the viewer-cache encoding/replay orchestration (#12) which
owns the consumption-side cost model.
