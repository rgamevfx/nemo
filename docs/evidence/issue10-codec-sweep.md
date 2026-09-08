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

| codec | chunk | encode ms/frame | decode ms/frame | seek ms @ boundary | PSNR dB (min) | B/frame |
|---|---|---|---|---|---|---|
| h264-nvenc | 12 | 30.41 | 16.53 | 21.58 | 25.83 | 10575 |
| h264-nvenc | 24 | 30.12 | 16.27 | 21.44 | 25.83 | 10319 |
| h264-nvenc | 48 | 30.14 | 16.10 | 21.18 | 25.85 | 10158 |
| hevc-nvenc | 12 | 30.09 | 17.19 | 22.79 | 25.83 | 11667 |
| hevc-nvenc | 24 | 30.00 | 16.84 | 22.78 | 25.84 | 11252 |
| hevc-nvenc | 48 | 30.09 | 16.58 | 22.50 | 25.85 | 11079 |
| libx264-cpu | 12 | 33.16 | 16.34 | 19.99 | 25.81 | 6477 |
| libx264-cpu | 24 | 33.00 | 16.07 | 19.81 | 25.82 | 6650 |
| libx264-cpu | 48 | 32.08 | 16.17 | 20.32 | 25.84 | 7598 |
| libx265-cpu | 12 | 45.49 | 17.37 | 26.59 | 25.81 | 8307 |
| libx265-cpu | 24 | 44.14 | 16.47 | 24.43 | 25.83 | 8544 |
| libx265-cpu | 48 | 38.76 | 16.35 | 21.58 | 25.84 | 9084 |

Hardware decode + Vulkan interop (decode → NV12 planes resident on the
application device → `mediaConvert` kernel → RGBA32F scene-linear
contract; no CPU readback):

```
hw-decode (h264-vulkan, device-resident, no CPU readback): 1.18028 ms/frame over 96 frames
```

Software decode + convert baseline (the always-available path whose
upload cost is the measured capability-dependent transfer):
~16.5 ms/frame (decode + sw conversion; device upload excluded).

## Reading of the numbers (evidence, not a decision)

1. **Encode cost**: NVENC ≈ 30 ms/frame at this resolution — dominated by
   CUDA staging (`uploadNsPerFrame` is measured and surfaced per encode;
   the 64x360-scale test frames make the fixed session setup amortize
   poorly). CPU x264 ≈ 33 ms/frame is close at this small scale; x265 is
   ~25% slower than x264 at medium preset.
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

## Explicitly NOT decided here (acceptance example 4)

The final codec/profile/bitrate and chunk-size choice remains an
evidence-gated prototype decision; this table is the input, not the
decision. Constraints that must weigh in later, beyond this table:
platform matrix (#17), resource-pressure accounting with decoder surfaces
(#14), and the viewer-cache encoding/replay orchestration (#12) which
owns the consumption-side cost model.
