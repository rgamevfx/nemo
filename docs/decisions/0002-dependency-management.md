# ADR-0002: Hybrid dependency management (vcpkg manifest + system media/UI stack)

- Status: accepted
- Date: 2026-09-06

## Context

The consultant checklist recommends one dependency manager in vcpkg manifest
mode with pinned versions. Full vcpkg coverage including Qt6 means multi-hour
source builds per fresh machine/CI runner and a very large cache. The
alternatives drift: unpinned system packages reproduce nothing.

## Decision

Hybrid:

- **vcpkg manifest mode** (`vcpkg.json`, `builtin-baseline` pinned) for pure
  C++ libraries where vcpkg is cheap and reliable: GoogleTest, nlohmann-json
  today; OpenImageIO, OpenColorIO, OpenEXR, fmt, and the shader toolchain as
  modules land.
- **System packages** for the heavy platform stack — Qt 6, Vulkan headers/
  validation layers, FFmpeg dev libraries — with required versions documented
  in `AGENTS.md` and CI (Ubuntu 24.04 baseline).
- Windows: same split, with vcpkg manifest entries + documented installers;
  the Windows preset path is validated before Windows becomes a CI target.
- Bumping the vcpkg baseline or adding a dependency is a reviewed change
  (explicit human review per the contribution checklist).

## Consequences

- First configure in minutes; CI stays fast with vcpkg binary caching.
- Version drift risk exists for system packages; mitigated by pinning the
  Ubuntu CI image version and recording package versions in the CI workflow.
- If/when Windows becomes first-class in CI, revisit: manifest-only may win
  there and per-OS overrides via `vcpkg.json` "overrides" are available.
