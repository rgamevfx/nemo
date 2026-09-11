# Issue tracker: GitHub

Issues and specs for this repo live as GitHub issues. Use the `gh` CLI for all operations.

## Pre-edit contract

Before application edits, record this compact contract in the owning issue.
Reuse existing issue sections where sufficient; a start comment may supply
the missing source-to-production mapping. A claim alone does not pass this gate.

| Field | Required content |
| --- | --- |
| Work class | **Prototype port**, **system extension**, or **new design**; classify mixed work by slice |
| Authority | Approved prototype/current coverage entries for observable UI; spec/ADRs for production ownership; linked owner decision for new design |
| Existing owner | Accepted production component and concrete extension entry point/callers; distinguish accepted code from unreviewed worktree changes |
| Change boundary | What is missing and will change; accepted behavior and adjacent systems that remain unchanged |
| Proof | Per-behavior production check and, for UI, reference scenario/capture and matched native interaction evidence |

For every affected prototype behavior, map the current coverage entry and
archived source function/handler to the production owner and observable check.
Account for all entries assigned to the ticket; shared entries name the other
owning ticket rather than silently narrowing acceptance. Read supersession
records before using older screenshots or gesture descriptions. Reference
paths and entry IDs belong in the ticket even when a generated spec omits paths.

For a system extension, identify supported schema/execution/host contracts
and demonstrate that discovery and presentation consume them. A missing
capability is a prerequisite gap: record it on its owning ticket and add a
native/body dependency only when delivery actually depends on it. Independent
backend work does not wait for unrelated UI work.

For new design, list the unresolved observable decisions. Obtain narrow owner
approval for the affected slice; implementation may continue on independent
specified slices. Do not treat a generated spec, assignment, or passing tests
as design approval.

## Implementation and review gates

Implement one complete observable behavior at a time. For a prototype port,
exercise its reference scenario and compare the native result before extending
the next behavior. Capture relevant hover, selection, focus, cancellation,
disabled and narrow-layout states, not just idle appearance. Use matched
viewport, DPR, theme, fonts and panel arrangement; identify content/platform
differences separately from design deviations. Preserve actual application
input-path verification described in `ownership.md`.

Record two review findings separately:

- **Production correctness:** commands/history, persistence, ownership,
  evaluation and failure invariants relevant to the task.
- **Prototype conformance:** each mapped UI behavior and appearance, with
  retained reference/production evidence and explicit owner-approved exceptions.
  Backend-only tasks state why this gate is not applicable.

An unexplained mismatch is unfinished work. Fix it before widening the change;
owner approval is needed to change the contract, not for every faithful slice.
On completion, update the existing contribution guide if the extension path
changed, so the next feature uses the same owner rather than a parallel path.
Documentation-only workflow work records link/contract verification instead
of claiming runtime evidence.

## Conventions

- **Create an issue**: `gh issue create --title "..." --body "..."`. Use a heredoc for multi-line bodies.
- **Read an issue**: use explicit JSON fields to avoid legacy Projects
  (classic) queries in the installed CLI's default text view:
  `gh issue view <number> --json title,body,labels,comments --jq '{title, body, labels: [.labels[].name], comments: [.comments[].body]}'`.
- **List issues**: `gh issue list --state open --json number,title,body,labels,comments --jq '[.[] | {number, title, body, labels: [.labels[].name], comments: [.comments[].body]}]'` with appropriate `--label` and `--state` filters.
- **Comment on an issue**: `gh issue comment <number> --body "..."`
- **Apply / remove labels**: `gh issue edit <number> --add-label "..."` / `--remove-label "..."`
- **Close**: `gh issue close <number> --comment "..."`

Before closing implementation work, account for every acceptance example
with executable evidence or an explicitly approved scope change. Record
unmet requirements and environment limitations; a passing suite or a
zero-match test filter does not close an unexercised gate. A closed mapping
ticket records planning completion, not completion of its implementation
children.

When changing prerequisites, update both native blocked-by links and the
body's Dependencies section, preserve an acyclic graph, and link each child
to its parent. Triage readiness describes specification completeness;
execution readiness additionally requires all blockers to be closed.

Infer the repo from `git remote -v`; `gh` does this automatically when run inside a clone.

## Pull requests as a triage surface

**PRs as a request surface: no.** _(Set to `yes` if this repo treats external PRs as feature requests; `/triage` reads this flag.)_

When set to `yes`, PRs run through the same labels and states as issues, using the `gh pr` equivalents:

- **Read a PR**: `gh pr view <number> --comments` and `gh pr diff <number>` for the diff.
- **List external PRs for triage**: `gh pr list --state open --json number,title,body,labels,author,authorAssociation,comments` then keep only `authorAssociation` of `CONTRIBUTOR`, `FIRST_TIME_CONTRIBUTOR`, or `NONE` (drop `OWNER`/`MEMBER`/`COLLABORATOR`).
- **Comment / label / close**: `gh pr comment`, `gh pr edit --add-label`/`--remove-label`, `gh pr close`.

GitHub shares one number space across issues and PRs, so a bare `#42` may be either: resolve with `gh pr view 42` and fall back to `gh issue view 42`.

## When a skill says "publish to the issue tracker"

Create a GitHub issue.

## When a skill says "fetch the relevant ticket"

Run `gh issue view <number> --json title,body,labels,comments` (shape it
with `jq` as in Conventions).

## Wayfinding operations

Used by `/wayfinder`. The **map** is a single issue with **child** issues as tickets.

- **Map**: a single issue labelled `wayfinder:map`, holding the Notes / Decisions-so-far / Fog body. `gh issue create --label wayfinder:map`.
- **Child ticket**: an issue linked to the map as a GitHub sub-issue (`gh api` on the sub-issues endpoint). Where sub-issues aren't enabled, add the child to a task list in the map body and put `Part of #<map>` at the top of the child body. Labels: `wayfinder:<type>` (`research`/`prototype`/`grilling`/`task`). Once claimed, the ticket is assigned to the driving dev.
- **Blocking**: GitHub's **native issue dependencies**, the canonical, UI-visible representation. Add an edge with `gh api --method POST repos/<owner>/<repo>/issues/<child>/dependencies/blocked_by -F issue_id=<blocker-db-id>`, where `<blocker-db-id>` is the blocker's numeric **database id** (`gh api repos/<owner>/<repo>/issues/<n> --jq .id`, _not_ the `#number` or `node_id`). GitHub reports `issue_dependencies_summary.blocked_by` (open blockers only, the live gate). Where dependencies aren't available, fall back to a `Blocked by: #<n>, #<n>` line at the top of the child body. A ticket is unblocked when every blocker is closed.
- **Frontier query**: list the map's open children (`gh issue list --state open`, scoped to the map's sub-issues / task list), drop any with an open blocker (`issue_dependencies_summary.blocked_by > 0`, or an open issue in the `Blocked by` line) or an assignee; first in map order wins.
- **Claim**: `gh issue edit <n> --add-assignee @me`, the session's first write.
- **Resolve**: `gh issue comment <n> --body "<answer>"`, then `gh issue close <n>`, then append a context pointer (gist + link) to the map's Decisions-so-far.
