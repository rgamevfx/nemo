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

## Corrections (2026-09-08) — replacement experiment in #23

The tables and figures below are retained as historical measurements, but
several attributions drawn from them are wrong or overstated. No corrected
measurements exist yet; the corrected experiment is owned by #23 and is
not landed. Until it reports, do not cite the numbers below as gate
evidence.

1. **The `upload ms/frame` column is not an isolated transfer
   measurement.** Its timer encloses CPU RGB→YUV conversion,
   allocation/packing, and copies together with the transfer. The claim
   that hardware transfer dominates encoding therefore misattributes the
   cost: the stages cannot be separated from this
   table. Each transfer must report bytes and direction with conversion
   and allocation presented as separate stages (#23).
2. **The `encode ms/frame` column excludes encoder initialization and
   mux/finalization.** It covers the frame-processing loop and codec drain;
   complete reusable-chunk cost (setup through finalized readable output,
   cold/warm distinguished) was not measured here.
3. **The peak-resource number is process `VmHWM`, not isolated decoder or
   VRAM accounting.** The ~838 MiB figure attributes decoder surfaces only
   by log inference; decoder surfaces, VRAM, and retained
   source/reference buffers must be measured separately (#23).
4. **The fidelity-neutrality claim is unsupported.** Similar aggregate
   PSNR across candidates on a single 4:2:0 workload does not establish
   codec neutrality. Independently derived fixtures and separated error
   sources are needed before attributing the differences.
5. **This workload is small and diagnostic.** 640x360 x 96 frames of
   `testsrc2` is evidence about itself only; it is not the declared
   reference workload and not a gate for the integrated visible-latency
   benchmark (#16 owns that 4K-source→1080p 200-frame media workload).
6. **The interop comparison is not an integrated speedup measurement.**
   The hardware path returns resident images; the software reference builds
   host images. These are different endpoints, and the historical 1.09294
   versus 1.18 ms/frame discrepancy is unresolved. #23 must rerun with
   explicit stage boundaries before deriving comparative speedups.

## Capability evidence (`nemo-cli probe-media`)

- `h264-vulkan` / `hevc-vulkan` probe evidence is **queue-verified**:
  the application reserved a video decode queue. That is not profile- or
  clip-level decode verification. The separate H.264 runtime measurement
  below exercised an actual clip; it does not establish every HEVC profile.
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

Historical process peak RSS: 858208 KiB (~838 MiB). This includes retained
host images, libraries and other process allocations; it does not isolate
decoder memory or measure decoder VRAM.

Hardware decode + Vulkan interop (decode → application-device NV12 planes
→ `mediaConvert` → resident RGBA32F; no CPU readback). The implementation
called this scene-linear, but matrix-only conversion leaves nonlinear RGB;
#21 owns the corrected source/replay interpretation.

```
hw-decode (h264-vulkan, device-resident, no CPU readback): 1.09294 ms/frame over 96 frames
```

Software decode + conversion historical baseline: ~16.6 ms/frame. This
produces host images using the same matrix-only conversion; this report
does not isolate its subsequent device-upload cost.

## What the historical numbers support

- The table compares four codec configurations and three chunk lengths on
  the declared small workload. Its frame-processing and combined
  conversion/staging values are retained, not reinterpreted as isolated
  transfer or complete reusable-chunk timings.
- The recorded payload sizes vary by candidate at the 2000 kbps target;
  codec defaults and rate-control behavior must be recorded explicitly in
  the replacement experiment. Payload bytes are not complete container size.
- PSNR values are similar on this workload. Their relative contributions
  from conversion, chroma reconstruction, subsampling and compression are
  not isolated, so no codec-neutral fidelity conclusion follows.
- Boundary-seek values describe fresh software open/decode for these small
  segments, not warm hardware reverse/random viewer replay.
- Resident hardware decode was exercised. Neither this timing nor the
  separate module tests prove the connected source→effect→viewing
  transform→viewer path or its latency target.

## Test fidelity gate

The decode interop comparison (`HwMedia.DecodeInterop...`) checks hardware
against the software 709 conversion with a 4/255 per-component tolerance
on a synthetic clip with non-neutral chroma. The reported max delta was
0.0. This is useful parity evidence, including Cb/Cr ordering; because both
paths share the matrix/range assumptions, it does not independently prove
source interpretation or scene-linear correctness. #21 and #18 require
independently derived color fixtures.

## Explicitly NOT decided here (acceptance example 4)

No final codec/profile/bitrate or chunk size is selected here. #23 owns
corrected experimental evidence; #12 records a provisional configurable
replay choice; #16 recommends defaults from integrated measurements.
Platform support (#17) and resource-pressure accounting (#14) remain
required inputs, not conclusions from this historical table.
