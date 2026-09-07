# ADR-0004: Provisional non-commercial licensing

- Status: accepted (provisional — revisit before the first public release)
- Date: 2026-09-07

## Context

The repository is being made public while the application is pre-usable. The
author wants to block commercial use of this early code for now, with the
explicit intention of choosing a different license once the application is
usable. Spec section 10.7 requires deciding project licensing before public
contributions.

## Decision

License the repository under **PolyForm Noncommercial 1.0.0** (`LICENSE`).

Rationale: it is the only established software license whose non-commercial
boundary is purpose-built for code (plain-language definitions of permitted
purposes, patent grant + defense, cure period). Creative Commons NC licenses
(CC BY-NC) are explicitly discouraged by Creative Commons for software.

## Consequences

- Commercial use, distribution for commercial advantage, and paid service
  offerings are excluded until relicensing; hobby, research, educational, and
  noncommercial-organization use is permitted.
- Contributors agree their contributions are offered under the same terms;
  license changes later require consent of all copyright holders — fine while
  the author is the sole contributor, so **relicense before accepting external
  PRs**, or record a CLA/DCO that grants relicensing rights.
- This is a provisional decision: revisit as an ADR update when the product
  direction for licensing is decided (e.g. dual licensing, AGPL, or permissive
  with a commercial tier).
- Dependency licenses are a separate tracking item; the application license
  does not select theirs (spec section 10.7).
