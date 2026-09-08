# Triage Labels

The skills speak in terms of five canonical triage roles; this repo's
tracker uses the same label strings.

| Label              | Meaning                                  |
| ------------------ | ---------------------------------------- |
| `needs-triage`     | Maintainer needs to evaluate this issue  |
| `needs-info`       | Waiting on reporter for more information |
| `ready-for-agent`  | Fully specified, ready for an AFK agent  |
| `ready-for-human`  | Requires human implementation            |
| `wontfix`          | Will not be actioned                     |

When a skill mentions a role (e.g. "apply the AFK-ready triage label"), use
the label string from this table.

`ready-for-agent` means implementation is sufficiently specified. Before
claiming it, also check native dependencies and assignment: a fully
specified ticket can still be blocked. `ready-for-human` marks work whose
remaining execution requires human access or judgement.
