# 0008 — Explicit built-in node contributions

Date: 2026-09-14
Status: Proposed — implemented on issue/83-node-contributions; owner review pending
References: issue #83; spec §§2, 10.2–10.4, 10.7, 12; ADR-0003, ADR-0004, ADR-0007

## Decision

Use one explicit built-in list (`src/nemo/nodes/BuiltinNodes.inc`) to derive
immutable schema/CPU/editor declarations and native execution registrations.
Node-local modules own descriptors, independent CPU/Slang/GLSL pixel code, typed
effect parameters, payload preparation and local pass descriptions. The schema
projection remains a runtime-object-free `NodeCatalog`; `NodeContributions` and
`EffectLibrary` are separately owned execution snapshots, not persistent model
state. Desktop, CLI and evaluation use the same builders.

This replaces the central per-effect switches, independently maintained runtime
shader inventories and growing shared effect uniform. It does not introduce a
plugin loader, stable binary ABI, service locator or universal extension SDK.
An ordinary effect requires its module, one list entry and normal build wiring;
new shared capabilities still require a change in their existing owning module.

## Boundaries

- Assemble and validate before publication. Duplicate identities, inconsistent
  schema/adapter versions, conflicting editor declarations, invalid payloads or
  pass references, and promised-but-missing implementations reject the whole
  candidate. Explicit backend unavailability or an unavailable kernel remains
  local to that node; unrelated supported nodes still execute.
- CPU execution retains dependency traversal, scoped expansion, effective
  animation/overrides, request validation, result reuse and error context in the
  shared evaluator. Read delegates to the existing source request/media owners;
  media error types remain intact. Output and Viewer retain their distinct roles.
  Formal inputs and network instances remain structural Evaluation behavior.
- Native binding contract v2 separates a 48-byte coordinate/time request at
  set 0/binding 0 from an optional node-local, 16-byte-aligned payload at binding
  1. Pass inputs use set 1 in declared order; output uses set 2/binding 0 and
  optional float weights set 3/binding 0. Blur selects its own horizontal and
  vertical passes, scratch image and weights. The shared executor still owns
  allocations, barriers, recording, submission and retirement. There is no
  per-node wait or routine intermediate CPU readback.
- CPU and GPU preparation callbacks may execute concurrently. Captures must be
  immutable or internally synchronized; context references/spans are valid only
  during invocation. A CPU request retains its registration until return. A GPU
  submission retains registrations, programs, payloads, weights and images until
  actual completion; dropping a result does not release in-flight resources.
  Backend initialization/compilation stays off the UI thread and out of the
  per-frame registration path.
- Descriptor and adapter versions must agree. Changed pixel/payload semantics
  require a version change; native identity also includes binding version and
  shader content. Registration order, labels, paths and callback addresses are
  not semantic identity. CPU keys retain the existing per-node version boundary,
  preserving reuse of unaffected branches; native library identity retains the
  conservative ADR-0007 boundary. Availability/schema compatibility is checked
  before reuse as well as execution.
- Optional editor declarations are projected into `ParameterEditorRegistry` at
  desktop assembly. The existing generic inspector, graph gestures, Commands,
  history and serialization remain their owners. Runtime registrations are not
  serialized; built-in node/parameter identities and image contracts are unchanged.

## Verification seam

`tests/contributions/Affine.{hpp,cpp,slang}` is a real, namespaced, test-only
extension, not another shipped node. `ContributionTests.cpp` assembles it through
the production interfaces and exercises discovery, creation/connections, edits,
undo/redo, persistence, independent CPU/native expected pixels, atomic rejection,
unavailability, stale-reuse prevention and retained ownership. Existing effect
regressions remain the numerical oracle; a production Wayland inspector/Blur
scenario separately checks the accepted surface. Exact results and limitations
are recorded in the issue #83 evidence, not implied by this decision's status.
