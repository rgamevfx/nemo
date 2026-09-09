# ADR-0004: GPU-first compositing and display-referred viewer caching

- Status: accepted
- Date: 2026-09-06
- Specification: revision 2.3, sections 8, 10.3–10.4, and 11

## Context

Interactive compositing and fast composition-cache construction are the primary performance goals. A CPU reference evaluator and shader-compilation/bootstrap milestones do not prove a GPU image path. Conversely, keeping 100–200 half-float frames in VRAM solely for playback consumes working memory needed for effects. Video compression is useful for playback storage but is not a general representation for scene-linear float intermediates, alpha, or named VFX channels.

## Decision

Keep the Vulkan/Slang stack from ADR-0001 and prove real GPU image execution early: decode → native effect → GPU OCIO viewing transform → viewer, followed by compressed-cache encoding and replay. CPU implementations remain correctness references and supported media/plugin paths; CPU pixel buffers must not define the universal evaluation interface. OpenFX CPU/OpenGL compatibility and graph/timeline integration remain mandatory gates, not prerequisites that defer native GPU proof.

Separate scene-linear half-/full-float processing and selective GPU intermediate reuse from the viewer playback cache. The latter stores the selected-resolution, display-transformed Rec.709 image as compact 4:2:0 video in a budgeted disk-backed cache, with a bounded RAM hot set and small decoded GPU queue. Preserve explicit color interpretation and do not apply the viewer transform twice. Accepted chroma/compression differences are separate from resolution approximation; cached playback does not imply draft effect processing. It cannot supply scene-linear node inputs, precise source-value inspection, or full-quality export. Keep UI decorations outside the encoded image.

Viewer resolution is user-controlled through Auto / Full / Half / Quarter. Auto uses stable levels based on physical image area, aspect ratio, and zoom. Region-of-interest and sampling resolution are distinct; effects preserve full-resolution coordinate semantics and declare supported reductions and dependencies. Unsupported reductions must not silently alter operation meaning.

Display requested results immediately and cache current, reusable representations asynchronously. Render only requested frames and their necessary dependencies: no render-ahead or idle range filling. An explicit range-cache command requests its range; ordinary automatic caching does not. Pausing or revisiting a matching cached frame does not rerender or force refinement. Bounded queues discard superseded cache work and never block interactive rendering.

Effective dependencies determine validity; request revision/identity prevents stale publication. Relevant source, graph, time, and viewing changes invalidate affected representations. Viewing-transform changes preserve upstream scene-linear reuse. Resolution/region/channel changes can select another representation without deleting valid siblings. Eviction removes valid residency under budget pressure; it is not invalidation. Settings expose disk location, disk/RAM/VRAM budgets, and cache clearing. Account for in-flight resources, effects, decoder surfaces, and presentation as well as retained cache data.

Validate NVIDIA first without making it a product requirement. NVENC and NVDEC are encoder/decoder engines, not codecs; hardware capabilities and Vulkan interoperability must be measured behind the Media/GPU interfaces. HEVC remains a candidate. Choose codec/profile, bit depth, bitrate, independently decodable chunk sizes, and default budgets from evidence rather than committing them here.

## Considered options

- CPU-first production rendering: rejected as the architectural default; retain CPU reference value without imposing CPU residency on native execution.
- A float VRAM playback history: rejected for range playback; retain bounded float working images and valuable intermediate reuse.
- Video-encoded graph intermediates: rejected because the viewer representation does not preserve the composition image contract and adds encode/decode work to reuse.
- Rerender on pause and speculative range filling: rejected; only changed or missing requested representations require evaluation.
- Retain every valid entry indefinitely: rejected in favor of bounded storage and least-recently-used eligible eviction.

## Consequences and evidence

The first uncached traversal can run at live-render speed; later replay benefits from retained representations. A display-transform change requires new viewer-cache data when those frames are requested, not automatic rerendering of the whole range. Compression fidelity must be characterized rather than called pixel-identical.

Initial targets on the GTX 1070 are 4K source → 1080p preview, 200 explicitly requested frames, and 24 fps playback. A simple grade/transform/merge graph targets ≤100 ms p95 edit-to-visible latency and ≥24 reusable cached frames/s including rendering and encoding. Report a heavier blur/keying graph separately. These are unmeasured targets, not performance claims. Record end-to-end latency, cache construction/backlog, warm forward/reverse/random replay, dropped frames, transfer costs, and peak budgets with reproducible hardware, driver, media, graph, and color settings.

See [specification sections 8 and 11](../composition_network_vfx_nle_spec_v2.md) for the normative policy and gates, and [tracking issue #7](https://github.com/rgamevfx/nemo/issues/7) for implementation dependencies. ADR-0001's Vulkan/Slang stack and ADR-0003's correctness/validation strategy remain in force; headless GPU execution is not CPU-only verification.

### Native viewer handoff (issue #11)

The accepted handoff uses **separate Vulkan logical devices on the same
physical GPU**. Nemo owns execution; Qt adopts a presentation-only device.
Only finished display-referred images cross this boundary through Vulkan
external memory and semaphores. Decode surfaces, effects, scene-linear
intermediates, and caches remain private to execution. Stock Qt is retained;
neither framework patches nor whole-job UI/renderer serialization are the
integration strategy.

This replaces the shared-device/two-queue design: native resize stress
reproduced worker submission overlapping Qt's `vkDeviceWaitIdle` during
swapchain recreation. Separate queue indices cannot isolate a device-wide
wait, and Qt 6.4 performs it before `beforeFrameBegin`.

The GPU presentation module owns compatible export/import allocations,
platform handle transport, semaphore ordering, and external queue-family
ownership transfer. It validates actual image and semaphore capabilities;
unsupported configurations fail explicitly, without CPU presentation
fallback. Qt retains both sides until every sampling frame has completed,
including after panel closure. Shutdown stops execution, destroys Qt's
scene graph, then drains both devices before releasing remaining owners.

The implementation writes presentation quantization directly into dedicated,
exportable memory and imports that allocation on Qt's device. Physical
bytes are charged once; either image retains both allocations. Execution
device teardown precedes presentation-device destruction because a timed-out
producer submission may still retain an imported image.

Native Wayland verification on the GTX 1070 covers real 10-bit 4:4:4 media,
resolution/ROI changes, concurrent viewer panels, resize, closure/reopening,
source replacement, and shutdown without synchronization warnings or routine
CPU readback. Linux opaque-FD transport is exercised; Windows opaque-Win32
transport is implemented but still requires Windows build/runtime evidence.

Qt 6.4's diagnostic `QQuickWindow::grabWindow()` path independently emits
swapchain synchronization warnings. A Qt-only window, with no Nemo execution
or shared images, reproduces both warnings. This is not a production viewer
operation or a reason to suppress validation; screenshot-free interaction
and diagnostic capture are reported separately in issue #11.

`SourceSession` owns bounded decoder/frame state outside the Document.
Source references persist path, signed frame offset/step, interpretation
overrides, and explicit media revision; reload advances that revision.
Source interpretation remains the Media module's responsibility. Container
pixel aspect feeds viewer fit and Auto selection. Source uploads on the
software decode path are disclosed; effects, OCIO, and presentation stay
device-resident. Presentation quantization targets RGBA8 UNORM and
premultiplies alpha for Qt; it does not apply another viewing transform.

Evaluation requests separate the full image domain (`fullWidth`,
`fullHeight`), full-resolution ROI, and sampling scale (1/2/4). An omitted
domain is valid only for an origin-aligned whole-image request. Generators
and source sampling use the full domain even for cropped requests.
Unsupported node reductions and out-of-domain requests are errors.
The current reference domain limit is 8192 pixels on each axis.

Run `nemo-ui --source /path/to/tagged-media.mkv` with `OCIO` set to the
project configuration. The viewer exposes frame, Auto/Full/Half/Quarter,
wheel zoom, drag pan, and Fit controls. `nemo-cli validate-gpu` accepts
persistent sources and emits scene-linear diagnostic output; it is not a
viewing-transformed export. Native source, color, coexistence, and
completion regressions live in `ViewerTests.cpp`; model and ROI contracts
live in `ViewerModelTests.cpp`. Issue #11 records machine-specific UI and
transfer evidence. Compressed replay, budget policy, and scheduling remain
the separate follow-on responsibilities above.
