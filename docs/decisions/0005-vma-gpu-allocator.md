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
- `Buffer` is a move-only RAII handle that uncharges on release; buffers must
  be released before their allocator (asserted in debug).
- When evaluation (#8) submits work concurrently, the allocator's threading
  contract must be revisited and bounded queues added; the bootstrap is not
  thread-safe.
