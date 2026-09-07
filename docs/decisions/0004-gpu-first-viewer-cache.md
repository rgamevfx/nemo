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
