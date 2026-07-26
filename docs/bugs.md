# prfs — known bugs & correctness risks

Design-level defects/risks surfaced in review that would become real bugs if built as-is, plus
real bugs found in the code. Each links to a TODO in `todo.md`.

Severity: 🔴 critical · 🟠 high · 🟡 medium. Status: ✅ resolved · ⬜ open.

| ID  | Sev | Status | Title                                                  | TODO |
| --- | --- | ------ | ------------------------------------------------------ | ---- |
| B1  | 🔴  | ⬜     | Directory cycles must be prevented (multi-parent DIRs) | T1   |
| B2  | 🟡  | ✅     | `diff` / `changes` semantics undefined                 | T2   |
| B3  | 🟠  | ⬜     | Snapshot metadata lost                                 | T3   |
| B4  | 🟠  | ⬜     | `readdir` pagination on a live directory               | T4   |
| B5  | 🟡  | ⬜     | Synthesized `.snapshot` has no concrete identity       | T5   |
| B6  | 🟡  | ⬜     | Timestamp source unspecified                           | T6   |
| B7  | 🟡  | ⬜     | `size` vs content block-structure authority            | T7   |
| B8  | 🟡  | ⬜     | `linkMode` in schema ahead of decision                 | T8   |
| B9  | 🟡  | ✅     | `stats().links` undercounted orphan-dir entries        | S9   |

---

## Details

### B1 — directory cycles must be prevented (multi-parent DIRs allowed) 🔴 ⬜

Directories are multi-parent by design (DAG, design §2.2), so a directory link/move could close
a **cycle** → an **infinite loop for a tree-walking archiver**. Fix: reject a cycle-closing
directory link/move via a boolean **DFS reachability check** (files skip it; early-exit). Two
attached sub-decisions: `..` resolution = **via-parent** (filehandle-encoded, depth-bounded
ancestor chain; an NFS-front-end concern, not the store) and directory `nlink`: store keeps the
true `#incoming` count; the front-end reports POSIX-compat (`2 + #subdirs`, default) or faithful
(`#parents`) — a test knob. → T1.

### B3 — snapshot metadata lost 🟠 ⬜

Deleting the stored `.snapshot` node also removed per-snapshot **timestamp/label**; mtime-based
incremental scenarios can't work without it. Fix: reintroduce a `snaps: snapId → {ctime, label}`
table, written by `snapshot()`. → T3.

### B4 — `readdir` pagination on a live (mutable) directory 🟠 ⬜

NFS `READDIR` is paginated with cookies; the live dir can change between calls. Cookie stability
and add/remove-mid-scan behaviour are undefined. (Sealed snapshot dirs are immutable and fine.)
→ T4.

### B5 — synthesized `.snapshot` has no concrete identity 🟡 ⬜

The virtual snapshot-list directory must be GETATTR-able and round-trip an NFS filehandle: it
needs a synthetic node id, synthetic attrs (mode/times/nlink), and a defined `readdir`. Currently
hand-waved. → T5.

### B6 — timestamp source unspecified 🟡 ⬜

For reproducibility and mtime-based tests, times must be deterministic (a logical/script-driven
clock), not wall-clock — otherwise the "reproducible" goal is untrue. → T6.

### B7 — `size` vs content block-structure authority 🟡 ⬜

Two files sharing a `contentID` may have different `size`; the block structure must be
size-parametric and `size` authoritative for `st_size`. Needs stating to avoid a "which wins"
inconsistency. → T7.

### B8 — `linkMode` in schema ahead of decision 🟡 ⬜

The `nodes` value already carries `linkMode`, but the COW-vs-hybrid choice (design §3.4/§11) is
undecided — the schema commits to an unbuilt feature. Mark it reserved or defer. → T8.

## Resolved

### B2 — `diff` / `changes` semantics 🟡 ✅

Specified in design §6.1: `diff(A,B)` is a node-level state-comparison oracle `diffNodes(A,B)`
(CREATED/REMOVED/MODIFIED{content,attrs}) over a `changes` candidate index, plus on-demand
path-level `diffPaths`. Implementation tracked in `todo.md` scaffolding.

### B9 — `stats().links` undercounted orphan-dir entries (backend) 🟡 ✅

The LMDB/mem PrfsStore computed `links` by summing down-link sets only over dirs with `nlink>0`,
dropping entries held inside directories that had been unlinked from the tree (but still exist and
still contain children). Design §9 defines `links` as `±1` per link/unlink with no reachability
condition (matching the reference oracle). Fixed in `src/core/prfs.cpp`: count down-links over
*all* dirs with a live record (`eachEffNode`, no `nlink` filter); node counts keep the `nlink>0`
liveness filter inline. Caught by the differential harness (`tests/test_diff.cpp`).
