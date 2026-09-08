# ADR-0006: OCIO color policy and viewing-transform adapter shape

- Status: accepted
- Date: 2026-09-07
- Specification: revision 2.3, sections 5, 8, and 10.2 (issue #6)

## Context

The project needs a persisted color policy (working space, viewer transform, delivery transform) and a named viewing transform applied to images, with GPU as the native path and CPU retained as a correctness reference within declared tolerances (spec section 8; ADR-0004 already sequenced "GPU OCIO viewing transform" ahead of viewer integration). OpenColorIO joins the dependency set for this milestone.

## Decision

**Policy lives on the Document; the adapter lives in media; execution glue lives in the GPU module's existing compute primitives.**

- `ColorPolicy` is a plain data record on `Document` (working space, viewer transform, delivery transform, optional config path). No OCIO types in core; the module boundary (spec 10.2) holds because core stores policy, it does not evaluate it. Serialization writes `colorPolicy` only when non-default (defaults documented in `Document.hpp`); absent fields fall back field-wise; a non-object `colorPolicy` loads with defaults plus a warning rather than failing the file.
- `nemo::media::OcioContext`/`OcioViewingTransform` wrap OpenColorIO: load a config, resolve a named viewing transform, and expose two results — a CPU reference apply on `CpuImage`, and `buildViewingTransformGpu()`, which extracts the transform's execution data (display/reference matrices, curve parameters, 1D/3D LUT texture values, uniforms) via `GpuShaderDesc` without executing anything. Missing config or transform names throw with the offending name in the message.
- The GPU program (`OcioGpuProgram`: generated OCIO GLSL + LUT textures + uniform bytes) is executed by the gpu module's existing compute path — buffer upload, combined image samplers, one dispatch, readback. The adapter does not own Vulkan objects, and the gpu module does not learn about OCIO: the seam is plain data (`OcioGpuProgram`), produced by media, consumed by whatever executor exists (today a test harness over the bootstrap module; later the native executor from issues #10/#11).
- OCIO's GLSL is compiled to SPIR-V at runtime through glslang (`gpu::compileGlslToSpirv`). Nemo-authored effects remain Slang (issues #3/#8); OCIO-generated code is the one sanctioned GLSL input because OCIO's GPU path emits GLSL and its LUT/curve semantics are exactly what OCIO CPU produces — comparing GPU vs CPU against the same generated program keeps tolerance meaningful.
- 3D LUT textures: OCIO stores RGB triplets; they are expanded to RGBA on upload because 3-component float images are not sampleable on common desktop drivers (NVIDIA exposes only HOST_IMAGE_TRANSFER for `VK_FORMAT_R32G32B32_SFLOAT`).
- The viewing transform is a downstream viewing operation (spec section 8): it is applied at the viewing/delivery boundary by explicit request and is never baked into reusable composition results, which remain scene-linear. Applying it twice is a caller error the policy model does not invite: the adapter applies exactly the named transform per invocation and returns a new image.

## Consequences

- OCIO joins `vcpkg.json` as a pinned manifest entry; glslang is used for OCIO-generated GLSL only (documented at the link site).
- Tolerance is operation-specific, not bitwise: the 2x2 GPU-vs-CPU gate uses per-channel bounds documented on the assertion; OCIO's curve approximation can differ from CPU in the last decimal places.
- Cache/replay of viewed output is out of scope here (ADR-0004's viewer-cache work consumes this adapter).

## Considered options

- Baking the viewing transform into composition evaluation: rejected — spec section 8 forbids contaminating reusable results, and double application would become inevitable.
- Implementing OCIO's LUT/curve math directly in Slang: rejected for this milestone; OCIO's own generated code is the contract of record for its operations, and divergence risk would fall on us.
- Executing the viewing transform on the CPU as the native path: rejected; ADR-0004 makes GPU native with CPU as reference.
- Storing OCIO config objects in the Document: rejected — persistent model stays free of plugin-runtime objects (spec 10.2).
