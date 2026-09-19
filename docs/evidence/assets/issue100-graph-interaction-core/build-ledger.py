#!/usr/bin/env python3
"""Build the issue #100 conformance ledger from the #44 machine reference index.

The ledger is derived by rule, never by hand, so a reviewer can re-derive every
one of its rows:

  1. every ``source`` string of the index is split into ``<file>`` and
     ``<region>``; each region is matched against the ordered REGION RULES
     below, which name the owning unit of the interaction rewrite and the units
     that support it;
  2. a row's PRIMARY REGION is the first of its own regions (in the row's
     source order) that appears in the PRECEDENCE list; the primary region
     decides the row's owning unit (through the region rules) and its primary
     re-verification evidence (through the EVIDENCE RULES);
  3. re-verification is composed of the primary evidence, an owning-issue
     override for rows shared with #49/#46, and the APPENDED EVIDENCE rules
     (gesture cost budget, scoped port geometry, pipe-pull locking, search/text
     protection).

Usage:
    python3 build-ledger.py             # writes conformance-ledger.json
    python3 build-ledger.py --markdown  # prints the ledger section for
                                        # docs/evidence/issue100-graph-interaction-core.md

Only the Python standard library is used, and the JSON is written
deterministically: two runs over the committed input produce identical bytes.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ASSETS = HERE.parent
INPUT = ASSETS / "issue44-graph-editing" / "reference-mapping.json"
OUTPUT = HERE / "conformance-ledger.json"
LEDGER_NAME = "issue100-graph-interaction-core"
INPUT_REPO_PATH = "docs/evidence/assets/issue44-graph-editing/reference-mapping.json"
OUTPUT_REPO_PATH = "docs/evidence/assets/issue100-graph-interaction-core/conformance-ledger.json"

# Expected row count of the committed machine index. A mismatch means the input
# changed and the ledger must be re-derived deliberately, not silently rescaled.
EXPECTED_ROWS = 227

# The interaction-layer units of issue #100, in contract order. Contract order
# also orders `supporting_units` and breaks nothing else: every row rule below
# names its unit explicitly.
UNITS = ["scene", "geometry", "hit-test", "gestures", "command-facade", "painter", "panel"]

# --- region rules -----------------------------------------------------------
# Ordered; the first rule whose `file` or `region` substring matches a source
# string (case-insensitive) owns that source. File rules come first because
# they are the more specific statement for the files the ticket names.
REGION_RULES = [
    {
        "id": "R01",
        "file": "theme.qml",
        "unit": "painter",
        "supporting": [],
        "reason": "category colours are authored in Theme.qml and applied by the painter",
    },
    {
        "id": "R02",
        "file": "studiomodel.qml",
        "unit": "panel",
        "supporting": ["command-facade"],
        "reason": "scope navigation and hierarchy commands are panel state plus the shared command path",
    },
    {
        "id": "R03",
        "region": "port geometry/core guard",
        "unit": "geometry",
        "supporting": ["hit-test"],
        "reason": "port position, hit region and card core guard are geometry facts",
    },
    {
        "id": "R04",
        "region": "ports",
        "unit": "geometry",
        "supporting": ["hit-test"],
        "reason": "port position and hit region are geometry facts",
    },
    {
        "id": "R05",
        "region": "node visuals",
        "unit": "geometry",
        "supporting": ["hit-test"],
        "reason": "the card rectangle the visuals fill is geometry",
    },
    {
        "id": "R06",
        "region": "pipe pulls and release",
        "unit": "geometry",
        "supporting": ["gestures", "painter"],
        "reason": "the pulled route is a geometry polyline consumed by the connect session and the painter",
    },
    {
        "id": "R07",
        "region": "route geometry/gesture",
        "unit": "geometry",
        "supporting": ["gestures", "painter"],
        "reason": "route polyline and point-to-segment projection are geometry; the reroute session consumes them",
    },
    {
        "id": "R08",
        "region": "wire paint",
        "unit": "geometry",
        "supporting": ["gestures", "painter"],
        "reason": "the wire path the painter walks is the geometry polyline",
    },
    {
        "id": "R09",
        "region": "wires",
        "unit": "geometry",
        "supporting": ["gestures", "painter"],
        "reason": "wire geometry is owned once and consumed by both picking and drawing",
    },
    {
        "id": "R10",
        "region": "node drag",
        "unit": "gestures",
        "supporting": ["hit-test", "command-facade"],
        "reason": "node move is a gesture session; it commits one move command through the facade",
    },
    {
        "id": "R11",
        "region": "drag/selection",
        "unit": "gestures",
        "supporting": ["hit-test", "command-facade"],
        "reason": "drag and marquee selection are gesture sessions over the hit-test result",
    },
    {
        "id": "R12",
        "region": "snapping",
        "unit": "gestures",
        "supporting": ["geometry"],
        "reason": "snapping adjusts the session's candidate positions against geometry",
    },
    {
        "id": "R13",
        "region": "creation",
        "unit": "command-facade",
        "supporting": ["gestures", "scene"],
        "reason": "creation commits one command through the facade and reads the scene for its anchor",
    },
    {
        "id": "R14",
        "region": "delete",
        "unit": "command-facade",
        "supporting": ["gestures", "scene"],
        "reason": "deletion commits one command through the facade; the scene records what was removed",
    },
    {
        "id": "R15",
        "region": "click/double-click",
        "unit": "panel",
        "supporting": ["gestures"],
        "reason": "click arbitration and double-click dispatch are panel input plumbing",
    },
    {
        "id": "R16",
        "region": "input owner",
        "unit": "panel",
        "supporting": ["gestures"],
        "reason": "the panel owns the input path and hands the gesture sessions their events",
    },
    {
        "id": "R17",
        "region": "search",
        "unit": "panel",
        "supporting": [],
        "reason": "the search field is panel chrome; it drives placement through the same command path",
    },
    {
        "id": "R18",
        "region": "category toolbar",
        "unit": "panel",
        "supporting": [],
        "reason": "the tools/category hotbar is panel chrome",
    },
    {
        "id": "R19",
        "region": "category tokens",
        "unit": "panel",
        "supporting": [],
        "reason": "category tokens are panel-side settings consumed by placement",
    },
    {
        "id": "R20",
        "region": "inspector arrangement",
        "unit": "panel",
        "supporting": [],
        "reason": "inspector popup arrangement is panel state",
    },
]

# --- row rule: primary region precedence ------------------------------------
# Ordered region substrings. A row's primary region is its first region (in the
# row's source order) that contains one of these, in this order; the precedence
# enumerates the whole region vocabulary so every region has a declared rank.
# The rank encodes the scenario's subject: a scope/hierarchy row names the #49
# evidence, a creation row the command path, a pipe or route row the geometry it
# draws, a drag row the gesture session, and chrome/appearance rows their own
# owner.
PRECEDENCE = [
    "category colors",
    "hierarchy commands (#49)",
    "creation",
    "delete",
    "pipe pulls and release",
    "route geometry/gesture",
    "node drag",
    "drag/selection",
    "snapping",
    "wires",
    "wire paint",
    "ports",
    "port geometry/core guard",
    "node visuals",
    "click/double-click",
    "search",
    "category toolbar",
    "category tokens",
    "inspector arrangement (#46)",
    "input owner",
    "createNode/deleteSelectedNodes",
]

# --- evidence rules ---------------------------------------------------------
# Primary evidence by primary region, matched as a substring against the primary
# region (ordered, so the longer keys precede the shorter ones they contain).
# Every region of the vocabulary appears once, so every row resolves.
EVIDENCE_RULES = [
    ("createNode/deleteSelectedNodes", "CatalogMenuCreatesRealNodesAndTimelineSeeks"),
    ("port geometry/core guard", "EqualNodeIdsInDifferentScopesKeepTheirOwnPortGeometry"),
    ("category colors", "NEMO100_EVIDENCE_DIR graph-node-style.json"),
    ("category toolbar", "CatalogMenuCreatesRealNodesAndTimelineSeeks"),
    ("category tokens", "NEMO100_EVIDENCE_DIR graph-node-style.json"),
    ("hierarchy commands", "SubnetNavigationShowsTypedTerminalsAndRestoresScopedView"),
    ("inspector arrangement", "SubnetParameterPopoutExposesEditsAndReordersRows"),
    ("click/double-click", "LowZoomConnectedNodeBodiesRemainSelectable"),
    ("pipe pulls and release", "GraphPipePullRetainsRoutesOnCancelAndDisconnectsOnRelease"),
    ("route geometry/gesture", "GraphScreenSpaceHitTestingSurvivesEveryZoomLevel"),
    ("wire paint", "NEMO100_EVIDENCE_DIR graph-wire-preview.json"),
    ("wires", "GraphPipePullRetainsRoutesOnCancelAndDisconnectsOnRelease"),
    ("node drag", "GraphDragPreviewCancellationAndGroupOffsets"),
    ("drag/selection", "GraphDragPreviewCancellationAndGroupOffsets"),
    ("snapping", "GraphDragPreviewCancellationAndGroupOffsets"),
    ("node visuals", "NEMO100_EVIDENCE_DIR graph-node-style.json"),
    ("ports", "EqualNodeIdsInDifferentScopesKeepTheirOwnPortGeometry"),
    ("creation", "CatalogMenuCreatesRealNodesAndTimelineSeeks"),
    ("delete", "CatalogMenuCreatesRealNodesAndTimelineSeeks"),
    ("search", "GraphSearchCreatesImmediatelyAndProtectsSelectionWhileEditingText"),
    ("input owner", "GraphViewGestureWritesOnceAndOtherPanelsDoNotRewriteIt"),
]

# Appended evidence: matched against the row's whole region set.
COST_BUDGET = "WorkspaceDragTest.GraphGestureCostBudget"
APPEND_RULES = [
    {
        "id": "A01",
        "all_of": ["input owner"],
        "any_of": ["node drag", "drag/selection", "pipe pulls and release"],
        "evidence": COST_BUDGET,
        "reason": "the row exercises the pointer-move input path, so the gesture cost budget is asserted as counts",
    },
    {
        "id": "A02",
        "all_of": ["port geometry/core guard"],
        "any_of": [],
        "evidence": "EqualNodeIdsInDifferentScopesKeepTheirOwnPortGeometry",
        "reason": "the row exercises scoped port geometry, drawn and picked from one geometry owner",
    },
    {
        "id": "A03",
        "all_of": ["pipe pulls and release"],
        "any_of": [],
        "evidence": "GraphPipePullLocksSourceOrDestinationOnPress",
        "reason": "a pulled pipe locks its source or destination on press",
    },
    {
        "id": "A04",
        "all_of": ["search"],
        "any_of": [],
        "evidence": "GraphSearchCreatesImmediatelyAndProtectsSelectionWhileEditingText",
        "reason": "the row reaches placement or selection through the search field",
    },
]

# Rows shared with another ticket name that ticket's evidence instead of being
# narrowed to the rewrite's generic scenario.
SHARED_ISSUE_EVIDENCE = {
    46: ("SubnetParameterPopoutExposesEditsAndReordersRows", []),
    49: (
        "RemovingActiveSubnetUnwindsToParentAndKeepsSharedDefinition",
        [("SubnetNavigationShowsTypedTerminalsAndRestoresScopedView", "hierarchy commands")],
    ),
}

SOURCE_RE = re.compile(r"^(?P<file>[^\s:]+)(?::(?P<range>\d[\d,\s-]*))?\s*:?\s*(?P<region>.*?)\s*$")


def split_source(source: str) -> tuple[str, str]:
    """Return (file, region) for one `"<file> <region>"` index source string."""
    match = SOURCE_RE.match(source)
    if match is None or not match.group("region"):
        raise SystemExit(f"build-ledger: cannot split source string {source!r} into file and region")
    return match.group("file"), match.group("region")


def rule_for(file: str, region: str) -> dict | None:
    lowered_file = file.lower()
    lowered_region = region.lower()
    for rule in REGION_RULES:
        if "file" in rule and rule["file"] in lowered_file:
            return rule
        if "region" in rule and rule["region"] in lowered_region:
            return rule
    return None


def primary_source(sources: list[tuple[str, dict]]) -> tuple[str, dict] | None:
    """Return (primary region, its rule) for a row's matched sources.

    `sources` is the row's (region, rule) pairs in the row's own source order.
    Precedence keys are whole regions, matched case-insensitively.
    """
    for key in PRECEDENCE:
        for region, rule in sources:
            if key.lower() == region.lower():
                return region, rule
    return None


def evidence_for(primary_region: str) -> str:
    lowered = primary_region.lower()
    for key, evidence in EVIDENCE_RULES:
        if key in lowered:
            return evidence
    raise SystemExit(f"build-ledger: no evidence rule for primary region {primary_region!r}")


def resolve_evidence(issue: int, regions: list[str], primary_region: str) -> list[str]:
    if issue in SHARED_ISSUE_EVIDENCE:
        default, overrides = SHARED_ISSUE_EVIDENCE[issue]
        primary = default
        for evidence, condition in overrides:
            if any(condition in region.lower() for region in regions):
                primary = evidence
                break
    else:
        primary = evidence_for(primary_region)

    verification = [primary]
    lowered = [region.lower() for region in regions]
    for rule in APPEND_RULES:
        if not all(any(needle in region for region in lowered) for needle in rule["all_of"]):
            continue
        if rule["any_of"] and not any(any(needle in region for region in lowered) for needle in rule["any_of"]):
            continue
        if rule["evidence"] not in verification:
            verification.append(rule["evidence"])
    return verification


def ordered_supporting(units: set[str], owning: str) -> list[str]:
    return [unit for unit in UNITS if unit in units and unit != owning]


def build() -> dict:
    reference = json.loads(INPUT.read_text(encoding="utf-8"))
    rows = reference["entries"]
    if len(rows) != EXPECTED_ROWS:
        raise SystemExit(f"build-ledger: {INPUT} holds {len(rows)} rows, expected {EXPECTED_ROWS}")

    region_vocabulary: set[str] = set()
    unmapped: set[str] = set()
    rule_stats = {
        rule["id"]: {"sources": 0, "rows": 0, "primary_rows": 0, "regions": set()} for rule in REGION_RULES
    }
    entries = []
    by_unit: dict[str, int] = {unit: 0 for unit in UNITS}
    by_unit_involvement: dict[str, int] = {unit: 0 for unit in UNITS}
    by_issue: dict[str, int] = {}
    by_evidence: dict[str, int] = {}
    by_appended: dict[str, int] = {}

    for row in rows:
        matched: list[tuple[str, dict]] = []
        touched: set[str] = set()
        for source in row["source"]:
            file, region = split_source(source)
            region_vocabulary.add(region)
            rule = rule_for(file, region)
            if rule is None:
                unmapped.add(region)
                continue
            matched.append((region, rule))
            touched.add(rule["unit"])
            touched.update(rule["supporting"])
            stats = rule_stats[rule["id"]]
            stats["sources"] += 1
            stats["regions"].add(region)

        if not matched:
            raise SystemExit(f"build-ledger: row {row['entry']!r} has no region matched by the rule table")

        for rule in {id(rule): rule for _, rule in matched}.values():
            rule_stats[rule["id"]]["rows"] += 1

        primary = primary_source(matched)
        if primary is None:
            raise SystemExit(
                f"build-ledger: no precedence entry matches row {row['entry']!r} regions "
                + ", ".join(region for region, _ in matched)
            )
        primary_region, primary_rule = primary
        owning = primary_rule["unit"]
        rule_stats[primary_rule["id"]]["primary_rows"] += 1

        issue = row["implementation_issue"]
        verification = resolve_evidence(issue, [region for region, _ in matched], primary_region)

        entries.append(
            {
                "entry": row["entry"],
                "source": list(row["source"]),
                "implementation_issue": issue,
                "owning_unit": owning,
                "supporting_units": ordered_supporting(touched, owning),
                "reference_scenario": row["entry"],
                "re_verification": verification,
                "note": row["note"],
            }
        )

        by_unit[owning] += 1
        for unit in touched:
            by_unit_involvement[unit] += 1
        by_issue[str(issue)] = by_issue.get(str(issue), 0) + 1
        by_evidence[verification[0]] = by_evidence.get(verification[0], 0) + 1
        for evidence in verification[1:]:
            by_appended[evidence] = by_appended.get(evidence, 0) + 1

    if unmapped:
        raise SystemExit(
            "build-ledger: regions not matched by the rule table: " + ", ".join(sorted(unmapped))
        )

    dead = [rule_id for rule_id, stats in rule_stats.items() if stats["sources"] == 0]
    if dead:
        raise SystemExit("build-ledger: rule table entries matching no source: " + ", ".join(dead))

    ranked = {key.lower() for key in PRECEDENCE}
    vocabulary = {region.lower() for region in region_vocabulary}
    if ranked != vocabulary:
        missing = sorted(vocabulary - ranked)
        extra = sorted(ranked - vocabulary)
        raise SystemExit(
            "build-ledger: precedence list must enumerate the region vocabulary exactly; "
            f"unranked regions: {missing or 'none'}; ranked but absent from the index: {extra or 'none'}"
        )

    by_rule = {}
    for rule in REGION_RULES:
        stats = rule_stats[rule["id"]]
        by_rule[rule["id"]] = {
            "match": f"file contains {rule['file']!r}" if "file" in rule else f"region contains {rule['region']!r}",
            "owning_unit": rule["unit"],
            "supporting_units": list(rule["supporting"]),
            "regions_matched": sorted(stats["regions"]),
            "sources": stats["sources"],
            "rows": stats["rows"],
            "primary_rows": stats["primary_rows"],
            "reason": rule["reason"],
        }

    summary = {
        "row_count": len(entries),
        "by_unit": by_unit,
        "by_implementation_issue": {key: by_issue[key] for key in sorted(by_issue, key=int)},
        "by_rule": by_rule,
        "unmapped_regions": [],
        "by_unit_involvement": by_unit_involvement,
        "by_primary_evidence": {key: by_evidence[key] for key in sorted(by_evidence)},
        "by_appended_evidence": {key: by_appended[key] for key in sorted(by_appended)},
        "region_count": len(region_vocabulary),
        "regions": sorted(region_vocabulary),
        "source_string_count": sum(len(row["source"]) for row in rows),
    }

    return {
        "ledger": LEDGER_NAME,
        "generated_by": "docs/evidence/assets/issue100-graph-interaction-core/build-ledger.py",
        "input": INPUT_REPO_PATH,
        "output": OUTPUT_REPO_PATH,
        "unit_contract": UNITS,
        "shared_issues": {
            issue: count
            for issue, count in summary["by_implementation_issue"].items()
            if issue != "44"
        }
        | {"note": "rows shared with another ticket keep its issue and name its evidence"},
        "entries": entries,
        "summary": summary,
    }


def markdown(ledger: dict) -> str:
    summary = ledger["summary"]
    lines: list[str] = []
    lines.append("## Conformance ledger")
    lines.append("")
    lines.append(
        "Every graph-assigned row of the #44 machine reference index "
        f"(`{INPUT_REPO_PATH}`, {summary['row_count']} rows, "
        f"{summary['source_string_count']} source strings, {summary['region_count']} distinct regions) is mapped to the "
        "unit of this rewrite that owns it and to the evidence it will be re-verified against. The ledger is generated "
        "by rule: `python3 docs/evidence/assets/issue100-graph-interaction-core/build-ledger.py` regenerates "
        f"`conformance-ledger.json` byte-identically from the committed input and prints `{summary['row_count']} rows, "
        f"{len(summary['unmapped_regions'])} unmapped`."
    )
    lines.append("")
    lines.append(
        "`owning_unit` and `supporting_units` are drawn from the unit contract of the ticket "
        "(`scene`, `geometry`, `hit-test`, `gestures`, `command-facade`, `painter`, `panel`); `reference_scenario` is "
        "the index entry verbatim (session file plus pointer); `re_verification` is an ordered evidence list whose "
        "first entry is the primary re-verification and whose later entries are the appended rules below."
    )
    lines.append("")
    lines.append("### Region rule table")
    lines.append("")
    lines.append(
        "First matching rule wins; file rules are evaluated before region rules, because a file the ticket names is "
        "the more specific statement. `sources` counts the index source strings the rule matched, `rows` the distinct "
        "rows those strings belong to, and `primary_rows` the rows this rule owns outright."
    )
    lines.append("")
    lines.append(
        "| rule | source pattern | owning unit | supporting units | regions matched | sources | rows | primary rows | why |"
    )
    lines.append("|---|---|---|---|---|---|---|---|---|")
    for rule_id, rule in summary["by_rule"].items():
        regions = "<br>".join(f"`{region}`" for region in rule["regions_matched"]) or "-"
        supporting = ", ".join(f"`{unit}`" for unit in rule["supporting_units"]) or "-"
        lines.append(
            f"| {rule_id} | {rule['match']} | `{rule['owning_unit']}` | {supporting} | {regions} | "
            f"{rule['sources']} | {rule['rows']} | {rule['primary_rows']} | {rule['reason']} |"
        )
    lines.append("")
    lines.append("### Row rule: primary region")
    lines.append("")
    lines.append(
        "A row's primary region is its first region, in the row's own source order, that appears in the precedence "
        "list below; the primary region decides the row's owning unit (through the region rule table) and its primary "
        "re-verification evidence. Every region of the vocabulary has a declared rank, so no row can fall through. A "
        "row that carries scope/hierarchy sources is owned by `panel` and names the #49 subnet evidence rather than "
        "being narrowed to the generic drag path; `geometry` regions rank below the gesture sessions they serve, "
        "except where the region is itself the scenario's subject (a pipe pull or a route edit)."
    )
    lines.append("")
    lines.append("| rank | region substring | owning unit |")
    lines.append("|---|---|---|")
    rule_by_region = {}
    for rule_id, rule in summary["by_rule"].items():
        for region in rule["regions_matched"]:
            rule_by_region[region] = rule["owning_unit"]
    for rank, key in enumerate(PRECEDENCE, start=1):
        region = next(region for region in summary["regions"] if region.lower() == key.lower())
        lines.append(f"| {rank} | `{region}` | `{rule_by_region[region]}` |")
    lines.append("")
    lines.append("### Evidence rule")
    lines.append("")
    lines.append("Primary evidence by primary region, then the appended rules over the row's whole region set:")
    lines.append("")
    lines.append("| primary region contains | evidence |")
    lines.append("|---|---|")
    for key, evidence in EVIDENCE_RULES:
        lines.append(f"| `{key}` | `{evidence}` |")
    lines.append("")
    for rule in APPEND_RULES:
        conditions = " and ".join(f"`{needle}`" for needle in rule["all_of"])
        if rule["any_of"]:
            conditions += " together with any of " + ", ".join(f"`{needle}`" for needle in rule["any_of"])
        lines.append(f"- {rule['id']}: when the row's regions include {conditions} → append `{rule['evidence']}` — {rule['reason']}.")
    lines.append(
        "- Rows shared with another ticket override the primary evidence: #46 rows name "
        "`SubnetParameterPopoutExposesEditsAndReordersRows`, and #49 rows name "
        "`SubnetNavigationShowsTypedTerminalsAndRestoresScopedView` when they carry the hierarchy region and "
        "`RemovingActiveSubnetUnwindsToParentAndKeepsSharedDefinition` otherwise."
    )
    lines.append("")
    lines.append(
        "Three listed evidence names have no row in this index: `GraphWheelBurstAppliesPerTurnAndPersistsOnceWhenItSettles`, "
        "`GraphViewGestureWritesOnceAndOtherPanelsDoNotRewriteIt` (declared only as the `input owner` fallback, which no "
        "row reaches because every input-owner row also carries a behaviour region) and `GraphViewInMotionIsPersistedWhenTheWindowCloses`. "
        "The index carries no view/navigation region, so the #84 write-frequency and settle behaviour is re-verified "
        "through the cost budget the gesture rows append rather than through a row of its own."
    )
    lines.append("")
    lines.append("### Mapping counts")
    lines.append("")
    lines.append("Rows per owning unit (" + f"{summary['row_count']} rows):")
    lines.append("")
    lines.append("| owning unit | rows | rows owning or supporting |")
    lines.append("|---|---|---|")
    for unit in UNITS:
        lines.append(f"| `{unit}` | {summary['by_unit'][unit]} | {summary['by_unit_involvement'][unit]} |")
    lines.append("")
    lines.append(
        "`scene` and `hit-test` own no row outright: the index's rows are scenario checks, and scene projection and "
        "the pick appear as supporting units of the rows that consume them."
    )
    lines.append("")
    lines.append("Rows per implementation issue (the owning ticket, preserved from the index):")
    lines.append("")
    lines.append("| implementation issue | rows |")
    lines.append("|---|---|")
    for issue, count in summary["by_implementation_issue"].items():
        lines.append(f"| #{issue} | {count} |")
    lines.append("")
    lines.append("Rows per rule (rules are listed in table order):")
    lines.append("")
    lines.append("| rule | source pattern | owning unit | sources | rows | primary rows |")
    lines.append("|---|---|---|---|---|---|")
    for rule_id, rule in summary["by_rule"].items():
        lines.append(
            f"| {rule_id} | {rule['match']} | `{rule['owning_unit']}` | {rule['sources']} | {rule['rows']} | "
            f"{rule['primary_rows']} |"
        )
    lines.append("")
    lines.append("Rows per primary evidence:")
    lines.append("")
    lines.append("| primary evidence | rows |")
    lines.append("|---|---|")
    for evidence, count in summary["by_primary_evidence"].items():
        lines.append(f"| `{evidence}` | {count} |")
    lines.append("")
    lines.append("Rows that append each additional evidence item:")
    lines.append("")
    lines.append("| appended evidence | rows |")
    lines.append("|---|---|")
    for evidence, count in summary["by_appended_evidence"].items():
        lines.append(f"| `{evidence}` | {count} |")
    lines.append("")
    lines.append(f"`summary.unmapped_regions` is empty: all {summary['region_count']} distinct regions of the "
                 f"{summary['source_string_count']} source strings are matched by the rule table, and the generator "
                 "fails loudly if one ever is not.")
    lines.append("")
    lines.append("### Shared entries")
    lines.append("")
    shared = ledger["shared_issues"]
    lines.append(
        f"The index assigns {shared.get('49', 0)} rows to #49 and {shared.get('46', 0)} rows to #46. Those rows keep "
        "their `implementation_issue` verbatim and "
        "name the owning ticket's evidence rather than being narrowed silently to this rewrite's scenario: the #49 rows "
        "keep `SubnetNavigationShowsTypedTerminalsAndRestoresScopedView` (and "
        "`RemovingActiveSubnetUnwindsToParentAndKeepsSharedDefinition` where the row is not a navigation row), and the "
        "#46 rows keep `SubnetParameterPopoutExposesEditsAndReordersRows`. Their `source` regions are still mapped to "
        "the unit of this rewrite that must preserve the behaviour — `panel` for the shared `StudioModel.qml` scope and "
        "hierarchy commands, with `command-facade` alongside — so a green result here cannot be read as re-owning the "
        "other ticket's acceptance."
    )
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--markdown", action="store_true", help="print the ledger section for the evidence document")
    arguments = parser.parse_args()

    ledger = build()
    if arguments.markdown:
        sys.stdout.write(markdown(ledger))
        return 0

    OUTPUT.write_text(json.dumps(ledger, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"{ledger['summary']['row_count']} rows, {len(ledger['summary']['unmapped_regions'])} unmapped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
