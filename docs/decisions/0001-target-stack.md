# ADR-0001: Target stack C++20 + Vulkan + Slang + Qt 6

- Status: accepted (inherited from spec revision 2.2, section 10.1)
- Date: 2026-09-06

## Context

The product is a node-based compositor/NLE where interactive performance,
explicit GPU resource control, and a Nuke-like UI are core. The spec already
fixes the target stack; this record exists so the decision is citable and its
escape hatch is explicit.

## Decision

Adopt the spec section 10.1 stack: C++20 core, Vulkan backend, Slang→SPIR-V
shaders (glslang as the second ingestion path), Qt 6 Quick with custom C++
scene-graph items. CMake + Ninja, pinned dependencies.

The stack is subject to the prototype gates in spec section 11: a vertical
slice (decode → Slang effect → OpenFX effect → OCIO viewing transform →
viewer) must pass before the stack is frozen. Qt Widgets remains the fallback
if prototype results favor it.

## Consequences

- Direct Vulkan (no broad GPU abstraction layer) — resource interoperability
  and explicit scheduling are requirements, not conveniences.
- Every architectural rule in spec section 10.2 applies from day one; retrofit
  ownership boundaries has historically failed in applications of this scale.
- Prototype gates are tracked as GitHub issues, not chat history.
