# 0007 — Evaluator result reuse and invalidation

Date: 2026-09-07
Status: Accepted
References: issue #9, spec revision 2.3 §§8, 10.3, ADR-0004

## Context

The evaluator (issue #1 CPU reference, issue #8 native GPU execution) is a
pure function: every request re-executes the full dependency chain and
allocates fresh results. Spec sections 8/10.3 require reuse of matching
branch results, dependency-scoped invalidation, representation coexistence,
and publication freshness (stale work never overwrites newer results). No
document revision, result cache, or publication guard existed.

## Decision

1. **Content-derived keys, not history-derived.** A node result's identity
   (`ResultKey`, `src/nemo/core/evaluation/Reuse.hpp/.cpp`) covers the
   implementation version, node type, the node's authored parameter state,
   the keys of its effective inputs in declared port order, the mapped
   local time, region, full image domain, sampling scale, channels, quality,
   working space, source reference content (including media revision), and
   (for the GPU path) a fingerprint of the supplied effect library. Keys are computed
   before execution, so authored state is what is hashed:
   executor-injected defaults are a deterministic function of the
   implementation version. A node with an explicit default value and one
   relying on the default get different keys — conservative: this can
   only miss reuse, never serve a wrong result. The canonical form is
   length-prefixed per field (injective over arbitrary strings), so hash
   collisions and content aliasing cannot cause wrong reuse; equality
   compares the canonical form.

   Consequences: dependency-scoped invalidation falls out of key
   computation — an edit changes the keys of exactly the affected results;
   shared VFX with different grades reuse the shared upstream results;
   structurally identical occurrences share results regardless of node
   identity; unrelated document edits (including revision bumps) never
   invalidate branch reuse.

2. **Conservative key fields.** Mapped local time enters every node's key,
   even for operations that ignore it (e.g. `constcolor`). This is a
   superset of effective dependencies: correctness over hit rate. Per-type
   temporal-dependence refinement (and region/channel support-region
   narrowing) can tighten keys without changing the contract.

3. **Viewer state is excluded from scene-linear keys** (ADR-0004). A
   viewer/delivery-transform edit invalidates `viewerResultKey()`
   representations — the identity seam for the viewer-cache tickets #11/#12
   — while upstream scene-linear reuse stays valid.

4. **Publication freshness is a ticket, not invalidation.**
   `ResultCache::beginTicket(document)` captures the document revision
   (`Document::stateRevision()`: graph edit counter + color policy + source content)
   and a request generation. A computed result publishes only while the
   ticket still matches; superseded publications are discarded and counted
   (`staleRejected`), never stored. Reuse identity deliberately ignores the
   revision, so the guard never becomes an invalidation broadcast.

5. **Bounded residency, one policy for both residencies.** `ResultCache<T>`
   is a core header template instantiated for `CpuImage` (core) and
   `eval::GpuNodeImage` (eval module); GPU-resident results are never
   converted to CPU buffers for caching. The entry cap is a drop-oldest
   bound; the LRU/disk eviction policy of issue #14 builds elsewhere on
   this bound. Eviction is distinct from invalidation: an evicted valid
   identity may be computed again on demand.

6. **Plan evidence.** `PlanStep::cacheReused` and the reuse/miss/stale
   counters make avoided work observable without exposing storage layout.

## Alternatives considered

- **Document-revision-keyed cache entries** (compare revision to reuse):
  rejected — every unrelated edit would invalidate all branch reuse, which
  spec section 10.3 forbids.
- **Per-node-id cache maps**: rejected — content identity (shared VFX,
  equivalent occurrences, moved compositions) would not reuse across node
  identities.
- **Observer/event invalidation wired into `Graph`**: deferred — key
  computation already scopes invalidation; events matter only for #13's
  asynchronous scheduler and explicit UI-facing invalidation commands.

## Non-goals / deferred

Asynchronous publication and cancellation (#13), automatic LRU/budget
policy and disk eviction (#14), compressed viewer-cache representations
(#12), viewer presentation (#11).

## Verification

`tests/ReuseTests.cpp` (CPU, 12 scenarios: acceptance examples 1–6 of issue
#9) and `Effect.GpuReuseAvoidsRecomputationAndPreservesResults` (native GPU
path with Slang kernels) plus the full `ctest` matrix.
