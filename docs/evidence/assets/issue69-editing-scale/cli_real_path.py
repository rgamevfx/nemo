#!/usr/bin/env python3
"""Real-path correctness runner for issue #69 (throwaway evidence harness).

Drives the shipped headless JSON-lines driver (nemo-cli project-session) with
ordinary submitted commands and verifies the observable invariants through its
JSON responses only. The CLI path is used for correctness cross-checking, not
for timing: every gesture response serializes the preview document
("preview": {...}) and its request/response envelope, which would distort those
measurements. Gesture timing therefore comes only from the standalone driver.

Usage:
  python3 cli_real_path.py <nemo-cli> <seed-project.nemo> [output.json]
"""

import json
import subprocess
import sys

CHECKS = []


def check(name, ok, detail):
    CHECKS.append({"check": name, "ok": bool(ok), "detail": detail})


class Session:
    def __init__(self, binary, project):
        self.proc = subprocess.Popen(
            [binary, "project-session", project],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )

    def request(self, payload):
        self.proc.stdin.write(json.dumps(payload) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("driver closed stdout: " + self.proc.stderr.read())
        return json.loads(line)

    def file_state(self):
        return self.request({"op": "file-state"})

    def revision(self):
        return self.file_state()["revision"]

    def query(self):
        return self.request({"op": "query", "network_id": 1, "limit": 256})

    def node_param(self, query, node_id, key):
        for node in query["nodes"]:
            if node["id"] == node_id:
                return node["params"].get(key)
        return None

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=30)
        return self.proc.returncode


def main():
    binary, project = sys.argv[1], sys.argv[2]
    output = sys.argv[3] if len(sys.argv) > 3 else "cli-real-path.json"
    session = Session(binary, project)

    state = session.file_state()
    check("file_state_opens_clean",
          state.get("ok") is True and state.get("dirty") is False and state.get("can_undo") is False,
          {"revision": state.get("revision"), "dirty": state.get("dirty"), "can_undo": state.get("can_undo")})
    start_revision = session.revision()

    source = session.request({"op": "add-node", "network_id": 1, "type": "constcolor", "name": "cli_source",
                              "expected_revision": session.revision()})
    source_id = source["created_node_ids"][0]["id"]
    check("add_node_reports_created_identity",
          source.get("committed") is True and len(source["created_node_ids"]) == 1
          and len(source["changed_node_ids"]) == 1
          and source["created_node_ids"][0]["id"] == source["changed_node_ids"][0]["id"],
          {"created_node_ids": source["created_node_ids"], "revision": source.get("revision")})

    grade = session.request({"op": "add-node", "network_id": 1, "type": "grade", "name": "cli_grade",
                             "expected_revision": session.revision()})
    grade_id = grade["created_node_ids"][0]["id"]
    connect = session.request({"op": "connect", "network_id": 1, "from_node_id": source_id, "from_port": 0,
                               "to_node_id": grade_id, "to_port": 0, "expected_revision": session.revision()})
    check("connect_reports_created_edge",
          connect.get("committed") is True and len(connect["created_edge_ids"]) == 1,
          {"created_edge_ids": connect["created_edge_ids"]})

    gain = {"network_id": 1, "node_id": grade_id, "key": "gain"}


    def gain_edit(value):
        return dict(gain, value={"type": "color", "value": value})


    param = session.request({"op": "set-param", **gain_edit([0.5, 0.5, 0.5, 1.0]),
                             "expected_revision": session.revision()})
    authored = session.query()
    check("set_param_commits_and_is_queryable",
          param.get("committed") is True and len(param["changed_node_ids"]) == 1
          and param["changed_node_ids"][0]["id"] == grade_id
          and session.node_param(authored, grade_id, "gain") == {"type": "color", "value": [0.5, 0.5, 0.5, 1.0]},
          {"changed_node_ids": param["changed_node_ids"],
           "gain": session.node_param(authored, grade_id, "gain")})

    # Preview lifecycle: begin/update publish no revision; cancel publishes
    # nothing; commit is one entry whose undo restores the prior value.
    revision_before_preview = session.revision()
    begin = session.request({"op": "begin-parameter-gesture", "expected_revision": session.revision(),
                             "edits": [gain_edit([0.25, 0.25, 0.25, 1.0])]})
    update = session.request({"op": "update-parameter-gesture", "token": begin["token"],
                              "edits": [gain_edit([0.75, 0.75, 0.75, 1.0])]})
    check("preview_publishes_nothing",
          begin.get("ok") is True and update.get("ok") is True
          and begin.get("revision") == revision_before_preview
          and update.get("revision") == revision_before_preview
          and session.revision() == revision_before_preview,
          {"revision_before": revision_before_preview, "preview_revision": update.get("revision"),
           "published_revision": session.revision(), "preview_serialized_by_driver": "preview" in update})

    cancel = session.request({"op": "cancel-parameter-gesture", "token": begin["token"]})
    check("cancel_publishes_nothing",
          cancel.get("ok") is True and session.revision() == revision_before_preview
          and session.node_param(session.query(), grade_id, "gain") == {"type": "color", "value": [0.5, 0.5, 0.5, 1.0]},
          {"cancel": cancel, "revision": session.revision(),
           "gain": session.node_param(session.query(), grade_id, "gain")})

    commit_begin = session.request({"op": "begin-parameter-gesture", "expected_revision": session.revision(),
                                    "edits": [gain_edit([0.125, 0.125, 0.125, 1.0])]})
    session.request({"op": "update-parameter-gesture", "token": commit_begin["token"],
                     "edits": [gain_edit([0.75, 0.75, 0.75, 1.0])]})
    commit = session.request({"op": "commit-parameter-gesture", "token": commit_begin["token"],
                              "expected_revision": revision_before_preview})
    committed_query = session.query()
    undo = session.request({"op": "undo", "expected_revision": session.revision()})
    undo_query = session.query()
    redo = session.request({"op": "redo", "expected_revision": session.revision()})
    redo_query = session.query()
    check("gesture_commit_one_entry_undo_redo",
          commit.get("committed") is True and undo.get("committed") is True and redo.get("committed") is True
          and session.node_param(committed_query, grade_id, "gain") == {"type": "color", "value": [0.75, 0.75, 0.75, 1.0]}
          and session.node_param(undo_query, grade_id, "gain") == {"type": "color", "value": [0.5, 0.5, 0.5, 1.0]}
          and redo_query["nodes"] == committed_query["nodes"],
          {"committed_gain": session.node_param(committed_query, grade_id, "gain"),
           "undone_gain": session.node_param(undo_query, grade_id, "gain"),
           "redo_restores_document": redo_query["nodes"] == committed_query["nodes"]})

    # Stale and rejected edits publish nothing.
    revision_before_stale = session.revision()
    stale = session.request({"op": "add-node", "network_id": 1, "type": "blur", "name": "stale",
                             "expected_revision": revision_before_stale + 9})
    rejected = session.request({"op": "set-param", "network_id": 1, "node_id": 999999, "key": "gain",
                                "value": {"type": "color", "value": [1.0, 1.0, 1.0, 1.0]},
                                "expected_revision": session.revision()})
    check("stale_and_rejected_publish_nothing",
          stale.get("ok") is False and stale.get("error", {}).get("code") == "revision_conflict"
          and rejected.get("ok") is False and session.revision() == revision_before_stale,
          {"stale_error": stale.get("error"), "rejected_error": rejected.get("error"),
           "revision": session.revision(), "expected_revision": revision_before_stale})

    nodes_committed = len(session.query()["nodes"])
    undone = 0
    while session.file_state().get("can_undo"):
        session.request({"op": "undo", "expected_revision": session.revision()})
        undone += 1
        if undone > 64:
            break
    query_at_open = session.query()
    for _ in range(undone):
        session.request({"op": "redo", "expected_revision": session.revision()})
    query_restored = session.query()
    check("undo_redo_restores_document",
          undone == 5 and len(query_at_open["nodes"]) == nodes_committed - 2
          and query_restored["nodes"] == session.query()["nodes"]
          and session.node_param(query_restored, grade_id, "gain") == {"type": "color", "value": [0.75, 0.75, 0.75, 1.0]},
          {"undone_entries": undone, "nodes_committed": nodes_committed,
           "nodes_at_open": len(query_at_open["nodes"]), "nodes_restored": len(query_restored["nodes"])})

    summary = {"record": "cli_real_path_summary",
               "start_revision": start_revision,
               "passed": sum(1 for item in CHECKS if item["ok"]),
               "failed": sum(1 for item in CHECKS if not item["ok"]),
               "checks": CHECKS}
    exit_code = session.close()
    summary["driver_exit_code"] = exit_code
    with open(output, "w", encoding="utf-8") as stream:
        json.dump(summary, stream, indent=2)
        stream.write("\n")
    print(json.dumps({"passed": summary["passed"], "failed": summary["failed"], "driver_exit_code": exit_code}))
    return 1 if summary["failed"] else 0


if __name__ == "__main__":
    sys.exit(main())
