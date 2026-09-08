#!/usr/bin/env python3
"""Filter jscpd's production C++ clones to added/modified PR lines; never gate on clones."""

import argparse
import html
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
from urllib.parse import quote

HUNK = re.compile(rb"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@", re.MULTILINE)
MAX_PAIRS = 30
NOTES = """
Advisory only: these are textual similarities, not proven architectural faults.
Review whether the responsibility already has an owner. CPU/GPU implementations,
source/replay semantics, and independent test oracles can legitimately differ.

Scope: production `.cpp`/`.hpp` files under `src/` and `apps/`; generated/vendor
code is excluded. Only pairs touching added/modified lines are shown. Untouched
legacy duplication and deletion-only changes are excluded. See the full detector
JSON artifact for all detected pairs. No automatic refactoring or merge gate.
"""


def git(*args):
    result = subprocess.run(["git", *args], check=True, capture_output=True, timeout=60)
    return result.stdout


def changed_intervals(base, head):
    # NUL-delimited paths and literal pathspecs preserve spaces, brackets, and
    # newlines. Include BOTH rename paths so a pure move has no added lines.
    options = ["--no-ext-diff", "--no-textconv", "--no-color", "--find-renames"]
    entries = iter(git("diff", *options, "--name-status", "-z", "--diff-filter=AMR", base, head, "--").split(b"\0"))
    changed = {}
    for status in entries:
        if not status:
            continue
        old = next(entries).decode("utf-8")
        new = next(entries).decode("utf-8") if status.startswith(b"R") else old
        if not new.startswith(("src/", "apps/")) or not new.endswith((".cpp", ".hpp")):
            continue
        paths = [f":(literal){old}"]
        if old != new:
            paths.append(f":(literal){new}")
        diff = git("diff", *options, "--unified=0", "--inter-hunk-context=0", base, head, "--", *paths)
        intervals = []
        for match in HUNK.finditer(diff):
            start = int(match[1])
            count = int(match[2]) if match[2] is not None else 1
            if count:
                intervals.append((start, start + count - 1))
        if intervals:
            changed[new] = intervals
    return changed


def clone_side(value):
    if not isinstance(value, dict):
        raise ValueError("invalid detector clone side")
    name, start, end = value.get("name"), value.get("start"), value.get("end")
    if not isinstance(name, str) or not name or type(start) is not int or type(end) is not int:
        raise ValueError("invalid detector filename or line range")
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts or start < 1 or end < start:
        raise ValueError("detector path or line range is outside the source tree")
    return path.as_posix(), start, end


def candidates(report, changed):
    data = json.loads(Path(report).read_text(encoding="utf-8"))
    if not isinstance(data, dict) or not isinstance(data.get("duplicates"), list):
        raise ValueError("detector report has no duplicates array")
    found = []
    for pair in data["duplicates"]:
        if not isinstance(pair, dict) or type(pair.get("tokens")) is not int or pair["tokens"] < 1:
            raise ValueError("invalid detector clone entry")
        first, second = clone_side(pair.get("firstFile")), clone_side(pair.get("secondFile"))
        if any(start <= last and end >= first_line
               for name, start, end in (first, second)
               for first_line, last in changed.get(name, [])):
            found.append((first, second, pair["tokens"]))
    return sorted(set(found))


def code(text):
    # HTML code spans avoid Markdown delimiters in untrusted filenames.
    escaped = html.escape(text).replace("|", "&#124;").replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t")
    return f"<code>{escaped}</code>"


def location(side, head):
    path, start, end = side
    label = code(f"{path}:{start}-{end}")
    server = os.environ.get("GITHUB_SERVER_URL", "")
    repository = os.environ.get("GITHUB_REPOSITORY", "")
    if server.startswith("https://") and repository:
        url = f"{server.rstrip('/')}/{quote(repository, safe='/')}/blob/{head}/{quote(path)}#L{start}-L{end}"
        return f'<a href="{html.escape(url, quote=True)}">{label}</a>'
    return label


def summary(pairs, changed, base, head):
    lines = ["## C++ duplication candidates (advisory)", "",
             f"Comparing merge base `{base[:12]}` to `{head[:12]}`; "
             f"{len(changed)} production C++ file(s) have added/modified lines.", ""]
    if pairs:
        lines += [f"**{len(pairs)} duplication candidate(s) touching added/modified C++ lines.**", "",
                  "| First location | Second location | Tokens |", "| --- | --- | --- |"]
        for first, second, tokens in pairs[:MAX_PAIRS]:
            lines.append(f"| {location(first, head)} | {location(second, head)} | {tokens} |")
        if len(pairs) > MAX_PAIRS:
            lines += ["", f"Showing the first {MAX_PAIRS} pairs; the full JSON is in the artifact."]
    else:
        lines.append("No duplication candidates touch added/modified C++ lines.")
    return "\n".join(lines) + "\n" + NOTES


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("base", "head", "report", "output"):
        parser.add_argument(f"--{flag}", required=True)
    args = parser.parse_args()
    status = 0
    # Resolve file arguments before switching to the repository root.
    report = Path(args.report).resolve()
    output = Path(args.output).resolve()
    try:
        root = git("rev-parse", "--show-toplevel").decode().rstrip("\n")
        os.chdir(root)
        base = git("rev-parse", "--verify", "--end-of-options", f"{args.base}^{{commit}}").decode().strip()
        head = git("rev-parse", "--verify", "--end-of-options", f"{args.head}^{{commit}}").decode().strip()
        merge_base = git("merge-base", base, head).decode().strip()
        changed = changed_intervals(merge_base, head)
        content = summary(candidates(report, changed), changed, merge_base, head)
    except (OSError, ValueError, subprocess.SubprocessError, StopIteration) as error:
        status = 1
        content = ("## C++ duplication report unavailable\n\n"
                   "**Tooling failure, not a clean scan.** Check the workflow logs.\n\n"
                   f"Reason: {code(str(error))}\n\nThis check is advisory.\n")
        print(f"duplication report unavailable: {error}", file=sys.stderr)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(content, encoding="utf-8")
    return status


if __name__ == "__main__":
    sys.exit(main())
