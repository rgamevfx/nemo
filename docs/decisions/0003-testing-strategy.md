# ADR-0003: Testing strategy and feedback loops

- Status: accepted
- Date: 2026-09-06

## Context

Agents need fast, self-runnable feedback. The consultant checklist defines the
test layers; this record fixes where each layer runs and what gates a PR.

## Decision

| Layer | Tooling | Runs |
| --- | --- | --- |
| Core unit (time mapping, graph cycles, overrides, undo/redo, cache invalidation) | GoogleTest + CTest | Every PR, CI, every preset |
| Persistence (round trips, schema versions, missing/unknown data) | GoogleTest | Every PR |
| Image correctness (synthetic inputs, alpha/bounds/channels, CPU vs GPU tolerances) | headless `nemo-cli render` + image diff tooling | Relevant PRs; GPU comparisons on GPU machines |
| GPU validation (lifetimes, sync, shader memory) | Vulkan validation layers + GPU-assisted validation | GPU changes, on real GPU hardware |
| UI behavior (selection, panel linking, workspace restore, timeline) | Qt Test / Qt Quick Test | Relevant PRs |
| Performance (scrub latency, frame time, cancellation, RAM/VRAM, caches) | Representative workloads on dedicated hardware | Scheduled / hardware runs, benchmarking with validation layers off |

Sanitizers: ASan+UBSan preset per PR in CI; TSan preset locally and in CI once
concurrency lands. Vulkan validation layers on during development; off for
benchmarks.

Rules carried from the checklist:

- Agents verify via headless commands; agent-generated image expectations are
  evidence, never the sole correctness oracle — humans inspect diffs.
- CI without real GPU/Wayland is insufficient for platform verification;
  dedicated-hardware runs remain a human/agent task on qualifying machines.

## Consequences

- `nemo-cli` is a permanent product surface (spec section 10.1 "Automation"),
  not test scaffolding; its output formats are public contracts.
- The CI matrix grows only when the corresponding code exists; no placeholder
  jobs.
