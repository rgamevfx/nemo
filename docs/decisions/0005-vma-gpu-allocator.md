# ADR-0005: VMA-backed GPU allocator with Nemo-owned budget admission

- Status: accepted
- Date: 2026-09-07
- Specification: revision 2.3, section 10.2 (GPU module row), section 10.4

## Context

Issue #2 requires a budgeted `VmaAllocator`-style allocator in the `nemo::gpu`
module as part of the headless Vulkan bootstrap. The GPU module must expose
resource handles with explicit lifetime and completion rules, and third-party
types should stay behind adapters where practical (spec 10.2). ADR-0002 makes
Vulkan a system package and vcpkg manifest mode the path for pure-C++
dependencies; VulkanMemoryAllocator (VMA) is a header-only pure-C++ library,
available at the pinned baseline (`vulkan-memory-allocator` 3.4.0).

## Decision

Use real VMA, pinned in `vcpkg.json`, as the allocation machinery of
`nemo::gpu`. Hide it behind a narrow RAII interface (`Allocator`, `Buffer`);
no VMA types appear in public headers. Nemo owns configured-budget admission
and accounting (`AllocatorConfig::max_device_bytes`, `charged_bytes()`) and
will own eviction policy later. Issue #2 implements bootstrap allocation and
lifetime behavior only — no custom suballocation, eviction, or pooling.

## Consequences

- The GPU module links `Vulkan::Vulkan` (system package per ADR-0002) and
  privately includes VMA. `VMA_IMPLEMENTATION` lives only in
  `src/nemo/gpu/Allocator.cpp`.
- Budget rejection is descriptive: it names the requested size, the already
  charged bytes, and the budget (repo rule: errors identify the offending
  relationship). The charge is the requested size; VMA's block-level
  accounting is not the Nemo budget.
- Issue #22 keeps `Buffer` and `Image` move-only while `retain()` shares
  allocation/view ownership with submissions. The charge ends at final
  retirement, not destruction of the caller's wrapper. Retained allocations
  keep VMA state alive; Device and Instance still outlive all allocations.
- Creation, retirement, and budget accounting are mutex-protected. Queue
  admission is a separate bounded GPU-execution concern, not a byte-budget
  or eviction policy.

## Retained execution refinement (issue #22)

Device owns persistent per-family submission queues and the immutable compute
pipeline cache. Every application wrapper and FFmpeg uses the same per-family
queue mutex. A submission slot owns a command pool, buffer, fence, completion
identity, and retained resource tokens. Separate pools permit concurrent
recording without violating Vulkan's pool synchronization rule. Capacity or
queue-owner contention returns backpressure rather than waiting for GPU work.

Fence completion, not timeout or cancellation, permits retirement and reuse.
Completion IDs remain queryable after slot reuse. Device-loss submissions
quarantine their slot; teardown drains pending fences without routine
device-wide idle. Device destroys queues before its pipeline cache.

The cache key contains full SPIR-V and normalized descriptor layout; resource
bindings are immutable per pass. Descriptor bundles return to their pool only
after the last pass/submission token retires. Uniform allocations remain
per-dispatch to avoid mutable in-flight data; no image pool or cache budget
policy is introduced. Native graph barriers and dispatches record as one batch.
