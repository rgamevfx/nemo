# Codec experiment evidence: corrected #23 measurements and historical #10 data

**Historical #10 workload**, measured 2026-09-07 on the declared prototype device: NVIDIA GTX 1070
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

The tables and figures in the historical sections below are retained with
their original scope. They are not corrected by relabeling them as new
measurements. The replacement #23 experiment and its limitations are
recorded in [Corrected #23 experiment](#corrected-23-experiment).

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
   benchmark (#16 owns the integrated graph/viewer gate for that workload).
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

## Historical #10 codec × chunk sweep (original measurements)

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

## Corrected #23 experiment

The replacement harness measures **media preparation and independently
reusable codec chunks**, not a graph, asynchronous cache, viewer, or visible
latency. It does not choose a codec, playback budget, or release default.
The old hardware-source-decode comparison is no longer run by
`codec-sweep`: source decode/conversion and an isolated host→device transfer
are different measurements. No replacement interop speedup is claimed.

### Reproduction environment and source

- Linux x86-64; Intel Core i7-8750H; NVIDIA GeForce GTX 1070, 8192 MiB;
  NVIDIA driver 580.173.02.
- Release build: `cmake --build --preset release -j 3` (C++20, release IPO).
  Debug and ASan+UBSan builds were also built. The linker emits the existing
  third-party minizip-ng `mktemp` warning; changed project sources build
  without compiler warnings.
- FFmpeg `6.1.1-3ubuntu5`; libavcodec `60.31.102`, libavformat `60.16.100`,
  libavutil `58.29.100`, libswscale `7.5.100`.
- No benchmark runs overlap the build/test jobs or one another. This
  command creates no Vulkan instance: there are no Vulkan validation
  layers to disable. NVENC uses CUDA staging. Debug/sanitizer correctness
  checks retain their normal validation settings.
- Synthetic `testsrc2`, 3840×2160, **200 frames at 24 fps**, losslessly
  encoded by libx264 for reproducible source samples. `ffprobe` reports
  H.264 High 4:4:4 Predictive profile but **actual yuv420p samples** (the
  lossless x264 profile name does not imply a 4:4:4 pixel format).
  Matrix/transfer/primaries are BT.709, range limited, chroma left-sited.

```bash
work=$(mktemp -d)
ffmpeg -hide_banner -loglevel error \
  -f lavfi -i testsrc2=size=3840x2160:rate=24 -frames:v 200 \
  -c:v libx264 -preset ultrafast -crf 0 -threads 2 -pix_fmt yuv420p \
  -color_primaries bt709 -color_trc bt709 -colorspace bt709 \
  -color_range tv -chroma_sample_location left "$work/reference-4k-200.mp4"

build/release/apps/nemo-cli/nemo-cli codec-sweep "$work/reference-4k-200.mp4" \
  --width 1920 --height 1080 --max-frames 200 \
  --codecs h264-nvenc,hevc-nvenc \
  --chunks 4,8,12 --bit-depth 8 --bitrate-kbps 2000
```

Source-file SHA-256 on this FFmpeg build:
`03d5e46edc8e1f16ee32ae49ffd1e55643f132cd809b8f614ed28ddb41f09074`.
Regeneration on other FFmpeg versions may produce different container
bytes. Frame content, tags, source generation and library versions—not a
file hash alone—define the interpretation.

### Timing, fidelity and memory boundaries

Every candidate records codec/profile/bit depth/target bitrate/chunk
length, requested and verified frame counts, and actual unavailable
reasons. H.264 uses explicit **high**, HEVC explicit **main**, both 8-bit
4:2:0. `--profile` and `--bit-depth` expose the supported subset:
unsupported combinations fail rather than being silently downgraded.
Rate control is target ABR; preset/rate-control details not overridden by
the harness retain the recorded FFmpeg version's defaults. CPU encoder
threads are two; x265 pools and frame threads are also two.

- All stage fields use **milliseconds**. Per-frame tables divide totals
  by verified frame count, including short final chunks.
- Initialization includes codec/device/pool setup. Host frame allocation
  and packing, CPU RGB→YUV conversion, CUDA transfer-API submission, codec
  submission/drain, and mux/header/trailer/close have disjoint timers.
  H2D API times only `av_hwframe_transfer_data`. FFmpeg 6.1.1
  [submits asynchronous CUDA copies without an explicit H2D completion wait](https://github.com/FFmpeg/FFmpeg/blob/n6.1.1/libavutil/hwcontext_cuda.c);
  **isolated DMA completion time is unavailable**. Encoder send/drain can
  include upload dependency waits, so neither timer is pure device-engine
  time. This does not establish physical transfer dominance or an
  interop speedup. Copy extents include copied row padding
  (`min(host pitch, device pitch) × plane rows`), with **host→device**
  direction explicit. Driver-internal traffic is not measured.
  CPU rows have no transfer.
- Complete chunk time encloses setup through closed, finalized readable
  output, including timer/bookkeeping gaps. It excludes source preparation,
  decoder verification, and encoder-resource destruction after the
  function's final timestamp. Decode-back success is required before any
  chunk is credited. Thus its reciprocal is **encode-side construction
  throughput**, not full sequential harness or integrated cache throughput.
- First-chunk and subsequent-chunk means are reported separately.
  **Every chunk creates fresh encoder/device/pool resources**; subsequent
  chunks are not a warm-encoder benchmark. Source decoder state and the
  per-chunk host conversion scratch buffer are reused. OS/library caches
  are uncontrolled, so first use is not asserted to be process-cold.
  Cross-chunk warm encoder/device reuse is explicitly unavailable.
- Incremental decode samples nearest source luma pixel centers at
  1920×1080, with bilinear reconstruction of left-sited source chroma,
  then expands limited-range BT.709 to display-referred float RGB.
  **No 4K float source frame or 200-frame float history is created.**
  This is a declared diagnostic nearest-neighbor sampling policy, not an
  antialiased scaling-quality recommendation or #11 viewing transform.
- Fidelity is full-image RGB PSNR **against the sampled reference**, peak
  1.0, no clipping, alpha excluded, plus maximum absolute component error.
  Resolution-approximation error against the 4K source is not measured.
  Replayed chroma is reconstructed bilinearly at its declared left siting.
  Each encoded chunk's exact count, dimensions, corrupt-frame flags,
  codec/demux errors and finite samples are checked: empty, short,
  extra-frame or mismatched chunk decodes never produce fidelity success.
  Independent FFV1 integer color/gray and spatial-boundary fixtures check
  interpretation without Nemo's encoder generating their oracle.
- Active reference float payload is bounded to a chunk, with a **512 MiB
  limit**; replay comparison retains one float frame. Reference/replay
  payload accounting is separate from cumulative process `VmHWM`.
  Decoder surfaces, encoder/decoder internals and VRAM are **unavailable,
  not zero and not inferred from RSS**. No decoder-memory conclusion
  follows from the process peak.
- `--max-frames` is an upper bound, not a demand for nonexistent source
  frames. Known container counts bound the selected range; premature EOF
  before that expected count is an error. With no container count, clean
  EOF defines the selected range. Empty sources still fail. Every row
  records its actual selected/verified count, so a shorter source cannot
  pass for the 200-frame workload.

### Correctness evidence

- `ctest --preset debug`: **195/195 passed** after final boundary fixes.
- `ctest --preset asan -R 'Media|Viewer'`: **67/67 passed**, including the
  incremental decoder's source/viewer consumers and ownership paths.
- Final focused debug Media checks: **35 passed**; final release and
  ASan+UBSan checks (`MediaEncode.*:MediaDecode.*:HwMedia.CodecSweep*`):
  **34 passed each**. Run preset checks sequentially or with separate
  `TMPDIR`s: an attempted concurrent debug/release/ASan run collided in
  the existing test fixtures' fixed filenames; sequential reruns passed.
- Actual CLI: 13 invalid cases rejected (zero/negative/malformed/trailing
  comma/empty/missing/overflowing values); two concurrent eight-frame,
  eight-frame-chunk commands succeeded with boundary seek **n/a**, never
  NaN, including with the default frame cap. A cap of nine accepts the
  eight-frame source as eight frames. Empty input fails diagnostically.
- Unsupported 10-bit, `main10` profile, and unknown codec candidates
  retained their diagnostic reasons and unavailable measurement columns.
- Regressions cover incomplete/mismatched decode, mux-finalization failure
  with no reusable-frame credit, timing scale/exclusive stages, independent
  color/sampling interpretation, and concurrent temporary-file isolation.

Public Media API changes (`EncodeStats`, span-based encoding, bounded
`ViewerReferenceDecoder`, sweep options/report/comparison) require human
review. Independent Standards and Spec review agents were attempted but
could not start because their configured provider rejected requests with
HTTP 429. The implementation received a local standards/spec self-review;
this is not represented as an independent review.

### Measured reference results and limits

Captured tables preserve every measured row and scoped memory observation:

- [2000 kbps: H.264/HEVC × 4/8/12-frame chunks](issue23-reference-2000kbps.txt)
- [8000 kbps: H.264/HEVC × 4/8/12-frame chunks](issue23-reference-8000kbps.txt)

The second command uses the reproduction command above with
`--bitrate-kbps 8000`. Each of the **12 candidate rows verified all 200
frames** at 1920×1080. These were separate, sequential release processes.
The 64×48 eight-frame CPU runs are diagnostic boundary evidence only, not
a CPU comparator at the declared reference scale.

| codec | target kbps | chunk | complete ms/frame | PSNR dB | container B/frame |
|---|---:|---:|---:|---:|---:|
| h264-nvenc | 2000 | 4 | 67.386 | 25.812 | 19054.305 |
| h264-nvenc | 2000 | 8 | 43.715 | 25.892 | 18853.395 |
| h264-nvenc | 2000 | 12 | 37.186 | 25.877 | 17451.055 |
| hevc-nvenc | 2000 | 4 | 52.934 | 26.061 | 22445.660 |
| hevc-nvenc | 2000 | 8 | 36.931 | 26.068 | 19600.390 |
| hevc-nvenc | 2000 | 12 | 31.606 | 26.051 | 17590.805 |
| h264-nvenc | 8000 | 4 | 67.626 | 26.340 | 60670.920 |
| h264-nvenc | 8000 | 8 | 44.836 | 26.363 | 68914.420 |
| h264-nvenc | 8000 | 12 | 36.267 | 26.366 | 67647.375 |
| hevc-nvenc | 8000 | 4 | 53.337 | 26.357 | 59050.070 |
| hevc-nvenc | 8000 | 8 | 37.311 | 26.371 | 60557.810 |
| hevc-nvenc | 8000 | 12 | 31.476 | 26.375 | 59549.520 |

What these observations support:

1. **Setup matters.** Initialization spans 10.540–43.727 ms/frame across
   these fresh-resource configurations. Longer chunks amortize that cost,
   but these rows do not measure warm encoder/device reuse.
2. **The old upload-dominance inference remains invalid.** CPU RGB→YUV
   conversion spans 18.862–19.485 ms/frame; transfer-API submission spans
   0.665–0.698 ms/frame. Those scopes are now separated, but the latter is
   not pure DMA completion time. Each hardware row submits 622080000
   bytes host→device over 200 frames.
3. **Higher bitrate buys a small aggregate fidelity improvement here at
   substantially greater size.** PSNR changes from 25.812–26.068 dB at the
   2000 kbps target to 26.340–26.375 dB at 8000 kbps. The full tables also
   report large local maximum errors, including values over 1.0 on the
   unclipped floating reference. This pattern/sampling/4:2:0 experiment
   does not establish acceptable artist-facing quality, codec neutrality,
   or isolated compression error. Short chunks do not necessarily attain
   their nominal ABR target; container bytes are measured, not predicted.
4. **Memory remains separately scoped.** The largest retained reference
   payload is 398131200 bytes (12 frames), plus one 33177600-byte replay
   frame. Process peaks reach 737268 KiB and 735820 KiB in the two sweeps.
   These include other allocations and remain cumulative across rows.
   There is no isolated decoder-surface or VRAM measurement.
5. **No target attainment follows.** Source preparation alone measures
   98.286–105.721 ms/frame and software decode-back 82.482–92.987 ms/frame.
   The complete encode-side metric excludes those stages, comparison and
   presentation. The command-wide wall times, 348.46 and 352.51 seconds,
   include six repeated candidate workloads each, not one integrated
   range-cache request. #16 still owns connected graph/viewer latency,
   cache throughput and default recommendations.

After the final frame-cap/EOF and report-wording refinements, a separate
release confirmation (`--codecs hevc-nvenc --profile main --chunks 12
--bit-depth 8 --bitrate-kbps 8000`, otherwise the same 200-frame command)
again verified **200/200** frames: **26.375 dB**, maximum error **0.894**,
**59549.520 container bytes/frame**. Its complete encode-side time was
**32.265 ms/frame** and process peak **692572 KiB**. The exact row is
appended to the 8000 kbps artifact; it confirms the final path without
replacing the earlier measurements or treating run-to-run timing changes
as a performance improvement.
