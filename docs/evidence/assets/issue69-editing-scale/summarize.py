#!/usr/bin/env python3
"""Aggregate issue #69 driver JSONL into report tables (throwaway evidence tool).

Usage: python3 summarize.py timings-release.jsonl retention-release.jsonl
Writes <name>-summary.md next to each input and prints the tables.
"""

import json
import sys
from collections import OrderedDict


def load(path):
    rows = []
    with open(path, encoding="utf-8") as stream:
        for line in stream:
            line = line.strip()
            if line.startswith("{"):
                rows.append(json.loads(line))
    return rows


def us(value):
    return f"{value:.3f}"


def timings_tables(rows):
    workloads = OrderedDict()
    for row in rows:
        if row["record"] == "workload":
            workloads[row["variant"]] = row
    lines = []
    lines.append("| workload | networks | nodes | edges | params (bool/int/float/string/choice/vec2/vec3/color) | "
                 "anim ch | anim keys | media entries/bins/marks | serialized B |")
    lines.append("|---|---|---|---|---|---|---|---|---|")
    for name, row in workloads.items():
        c = row["composition"]
        p = c["parameters_by_type"]
        params = "/".join(str(p.get(k, 0)) for k in
                          ("boolean", "integer", "float", "string", "choice", "vector2", "vector3", "color"))
        lines.append(f"| {name} | {c['networks']} | {c['nodes']} | {c['edges']} | {params} | "
                     f"{c['animation_channels']} | {c['animation_keys']} | "
                     f"{c['media_entries']}/{c['media_bins']}/{c['media_marks']} | {c['serialized_bytes']} |")
    lines.append("")
    lines.append("| workload | op | cold n | cold p50 | cold p95 | warm n | warm p50 | warm p95 | warm min | warm max | notes |")
    lines.append("|---|---|---|---|---|---|---|---|---|---|---|")
    for row in rows:
        if row["record"] != "op":
            continue
        cold, warm = row["cold"], row["warm"]
        warm_cells = ["-"] * 5
        if warm and warm.get("count"):
            warm_cells = [str(warm["count"]), us(warm["p50_us"]), us(warm["p95_us"]), us(warm["min_us"]),
                          us(warm["max_us"])]
        cold_cells = [str(cold.get("count", 0)), us(cold.get("p50_us", 0.0)), us(cold.get("p95_us", 0.0))]
        notes = json.dumps(row.get("notes", {}), sort_keys=True).replace("|", "\\|")
        lines.append(f"| {row['variant']} | {row['op']} | " + " | ".join(cold_cells + warm_cells) + f" | `{notes}` |")
    return "\n".join(lines)


def retention_tables(rows):
    lines = ["| phase | notes | delta bytes |", "|---|---|---|"]
    for row in rows:
        if row["record"] != "retention":
            continue
        notes = json.dumps(row["notes"], sort_keys=True)
        lines.append(f"| {row['phase']} | `{notes}` | {row['delta_bytes']} |")
    for row in rows:
        if row["record"] == "history_bound_observation":
            lines.append(f"| observed retained entries ({row['commits']} commits, capacity "
                         f"{row['history_capacity']}) | | {row['observed_retained_entries']} |")
        if row["record"] == "process_memory":
            lines.append(f"| process peak RSS KiB | not retained history | {row['max_rss_kib']} |")
    return "\n".join(lines)


def main():
    for path in sys.argv[1:]:
        rows = load(path)
        table = timings_tables(rows) if "timings" in path else retention_tables(rows)
        out = path.rsplit(".", 1)[0] + "-summary.md"
        with open(out, "w", encoding="utf-8") as stream:
            stream.write(table + "\n")
        print(f"== {path} -> {out}")
        print(table)
        print()


if __name__ == "__main__":
    main()
