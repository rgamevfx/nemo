# Rendering & Media Context

Read this before rendering, media decode/encode, GPU execution, viewer
cache, or performance work. Ticket bodies own current status and scope —
this file carries only the approved contracts that stay true across them.

## Two color contracts — never conflate

- **Source-linear working images**: source decode interprets the source and
  converts to the scene-linear working space. Y′CbCr matrix/range conversion
  alone yields nonlinear R′G′B′ — matrix conversion is not linearization.
  Honor transfer, primaries, matrix, range, chroma location, bit depth, and
  project color policy; make ambiguous/unsupported interpretation explicit
  rather than guessing silently. The project's authored color-config path is
  passed per viewer session to `ViewerSession` (through
  `ViewerRuntime`/`ViewerScheduler`, including the cache-viewer path); an empty
  path keeps the existing `$OCIO` environment fallback resolved on the first
  viewing request, and no process-global environment state is mutated.
- **Display-referred replay**: viewer-cache chunks are the baked
  display-referred representation. Decode them without re-applying the
  source linearization or view transform; interpretation metadata
  (transfer and primaries included) travels inside the chunk and round-trips
  through pixel verification.

## Validate capabilities and formats — never assume

- Registration is not usability: init-verify codecs/decoders against the
  actual device and report unsupported profiles and combinations with
  clip/format/reason.
- Validate the actual decoded frame (pixel format, dimensions, plane count,
  bit depth) before touching planes. Convert supported formats deliberately;
  reject the rest with a diagnostic. No assumption of planar 8-bit 4:2:0.
- Per-frame hardware/software selection is verified against the produced
  path, and fallback reasons are reported accurately.

## Per-submission retained ownership

- Every resource referenced by a submission — images, buffers, views,
  descriptors, foreign decoder frames — stays alive until GPU completion,
  on success, timeout, cancellation, and failure alike. RAII and
  exception-safe FFmpeg ownership end to end, including AVBufferRef and
  extension-name storage tied to decoder/device lifetime.
- A retry never resets or reuses an in-flight command buffer or fence.
- Teardown drains safely; a wait timeout is never a license to free.

## Ownership boundaries (do not blur)

| Concern | Owner |
| --- | --- |
| Queue serialization (including FFmpeg sharing the application device), submission, completion identity, resource retirement | GPU execution mechanism (#22) |
| Request prioritization, publication policy, UI responsiveness | Scheduler (#13) |
| Byte budgets, admission, eviction of cached results | Cache accounting (#14) |
| Viewer-cache storage and replay orchestration | #12 |
| Media contract correctness: interpretation, format validation, FFmpeg ownership | #21 |

GPU owns the mechanism; #13 owns policy on top of it. #14 adds accounting
and admission to the retained-resource mechanism. A bug at one boundary is
filed against its owner, not patched in a neighbor.

## Execution shape

- Device-resident encode: encoders consume the device-resident viewer
  representation (interop), not CPU staging by default; a CPU staging path
  is a measured, disclosed cost, never the assumed interface.
- No per-node idle: batch compatible barriers and dispatches into
  submissions; no routine `vkDeviceWaitIdle` per node or dispatch, and no
  host wait per barrier.
- Pool and reuse (command buffers, fences, pipelines, images) only where a
  measurement shows the benefit; a reduced API-call count alone is not a
  performance win.

## Fidelity

- Independent oracles: fixtures and reference interpretations are derived
  independently of the code under test — independently tagged gray/color
  sources, corrupt-input cases. Agreement between paths that share math
  verifies parity, not independent correctness. Nonlinear midgray is not linear.
- Prototype and test-pass are not gate evidence: #10's green prototype did
  not establish the corrected contracts (#21), and a passing test suite is
  not a substitute for the demonstrated acceptance examples in the ticket.

## Profiling discipline

Every metric states its scope and units; transfers also state bytes and direction:

- **Scope**: exactly which stages are inside the timer — CPU
  conversion/allocation/packing vs the host→device transfer vs encoder
  submission/drain. Report initialization and mux/finalization separately
  and include them in total independently reusable-chunk cost. Per-frame
  averages must name their denominator and included stages.
- **Units**: consistent ns/ms fields and rendered units that agree; bytes
  moved and host/device direction for every transfer.
- **Cold/warm**: distinguish first-use from reused resources.
- **Memory**: attribute process peak RSS separately from decoder surfaces,
  VRAM, and retained source/reference buffers — a process-wide VmHWM is not
  isolated decoder/VRAM accounting.
- Small diagnostic workloads (e.g. the 640×360 sweep) are evidence about
  themselves, not the reference gate; the integrated visible-latency
  benchmark belongs to #16. Corrected codec measurements are #23's.
