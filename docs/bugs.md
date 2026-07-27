# prfs — known bugs & correctness risks

Design-level defects/risks surfaced in review that would become real bugs if built as-is, plus
real bugs found in the code. Each links to a TODO in `todo.md`.

Severity: 🔴 critical · 🟠 high · 🟡 medium. Status: ✅ resolved · ⬜ open.

**All currently-tracked bugs are resolved.** New findings get a row above and a write-up under
Resolved (or a reopened Open section).

| ID  | Sev | Status | Title                                                  | TODO |
| --- | --- | ------ | ------------------------------------------------------ | ---- |
| B1  | 🔴  | ✅     | Directory cycles must be prevented (multi-parent DIRs) | T1   |
| B2  | 🟡  | ✅     | `diff` / `changes` semantics undefined                 | T2   |
| B3  | 🟠  | ✅     | Snapshot metadata lost                                 | T3   |
| B4  | 🟠  | ✅     | `readdir` pagination on a live directory               | T4   |
| B5  | 🟡  | ✅     | Synthesized `.snapshot` has no concrete identity       | T5   |
| B6  | 🟡  | ✅     | Timestamp source unspecified                           | T6   |
| B7  | 🟡  | ✅     | `size` vs content block-structure authority            | T7   |
| B8  | 🟡  | ✅     | `linkMode` in schema ahead of decision                 | T8   |
| B9  | 🟡  | ✅     | `stats().links` undercounted orphan-dir entries        | S9   |
| B10 | 🟠  | ✅     | Parent dir mtime/ctime not updated on namespace change | L2   |

---

## Resolved

### B1 — directory cycles must be prevented (multi-parent DIRs allowed) 🔴 ✅

Directories are multi-parent by design (DAG, design §2.2), so a directory link/move could close a
**cycle** → an **infinite loop for a tree-walking archiver**. Fixed: `link`/`move` reject a
cycle-closing directory edge via a boolean **DFS reachability check** over directory down-links
(`reachable(child, dir)` — reject when `dir` is reachable from `child`). Files skip the check
(they can't close a directory cycle) and self-links are caught (`from==to`); `move` is covered
because it links before it unlinks, and the reject path writes nothing (atomic). Both engines
implement it independently; validated by `tests/core/dag.cpp` (self-link, transitive cycle,
multi-parent/diamond allowed, files unrestricted, move-into-own-subtree, dynamic re-evaluation)
and by the differential/invariant harnesses. Store keeps the true `nlink = #incoming`.

Carved out to the **NFS front-end** (not `libprfs`, tracked under todo L2): `..` resolution =
**via-parent** (filehandle-encoded, depth-bounded ancestor chain), and directory-`nlink`
reporting mode — POSIX-compat (`2 + #subdirs`, default) vs faithful (`#parents`).

### B6 — timestamp source unspecified 🟡 ✅

For reproducibility and mtime-based tests, times must be deterministic — a logical/script-driven
clock, not wall-clock. Fixed (design §3.5): `IPrfs::now()` reads a logical clock (no advance),
`setTime(t)` is the only thing that moves it (script owns the timeline; persisted in `meta` so a
reopen resumes it), new nodes stamp `atime=mtime=ctime=now()`, and all other time changes stay
explicit. Both engines; verified by `tests/core/clock.cpp` and clock-persistence in the crash
suite. Nothing in the store ever reads wall-clock time.

### B3 — snapshot metadata lost 🟠 ✅

Per-snapshot **timestamp/label** had no home, so mtime-based / labelled incremental scenarios
had no anchor. Fixed (design §3.2): a `snaps: snapId → {ctime, label}` sub-store, written by
`snapshot(label)` (ctime = the logical clock at seal time, §3.5) and read via `snapInfo(id)`.
Persisted (survives reopen); both engines; `tests/core/snapshot.cpp` + crash-suite persistence.
Exposed to Lua as `store:snapshot([label])` / `store:snapInfo(id)`.

### B5 — synthesized `.snapshot` has no concrete identity 🟡 ✅

The virtual snapshot-list directory needed a synthetic node id, synthetic attrs, and a defined
`readdir` to be GETATTR-able and round-trip an NFS filehandle. Fixed (design §3.2): `.snapshot`
of node `D` has the concrete id `D.id | (1<<63)` (reserved top-bit id-space), reports as a
read-only `0555` dir (`nlink 1`, times mirroring `D`), lists `"N"` per snapshot `D` existed at,
and `lookup(snapDir,"N")` → `(D.id, N)`. Live-view-only (no nesting), DIR-only, hidden from
`readdir` but resolvable; `link`/`move` reject the reserved name (`INVAL`). Both engines;
`tests/core/snapdir.cpp` (9 cases × 2 engines). *Deferred to the NFS front-end (todo L2):* a
config flag to also *list* `.snapshot` in `readdir`.

### B4 — `readdir` pagination on a live (mutable) directory 🟠 ✅

NFS `READDIR` is paginated and the live dir can change between calls; cookie stability and
add/remove-mid-scan behaviour were undefined. Fixed (design §6.2): `readdirPage(dir, after, max)`
uses the entry **name** as the cursor over the name-ordered link set — inherently stable, no
`cookieverf` needed. Every entry present for the whole scan is returned once; adds ahead of the
cursor appear later, adds behind are not revisited, removals ahead are skipped, and removing the
cursor entry still resumes. Both engines; `tests/core/readdirpage.cpp` + Lua `store:readdirPage`.
*Deferred to L2:* mapping the string cookie to a 64-bit NFS cookie for over-long names.

### B7 — `size` vs content block-structure authority 🟡 ✅

Two files sharing a `contentID` may have different `size`. Resolved (design §2.1): the per-node
`size` attribute — not the shared block structure — is authoritative for `st_size`/`READ`; the
block structure is size-parametric (same recipe, any length), and the content provider generates
`size` bytes from it. `size` is set explicitly on the node; the store never derives it from
`content` (which it never parses). Doc clarification; no code change (the store already treats
`size` as a plain node attribute).

### B8 — `linkMode` in schema ahead of decision 🟡 ✅

The schema listed a `linkMode` attr for a per-directory COW-vs-LOG strategy that is undecided
(§3.4/§11). Resolved (design §5): marked **reserved** and documented that the implementation is
pure COW — `NodeRec` carries no `linkMode` field, so the schema no longer commits to an unbuilt
feature. Adding it later is a record-format bump behind the existing versioning.

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

### B10 — parent directory mtime/ctime not updated on namespace change 🟠 ✅

Found running **pjdfstest** over an NFS mount (`scripts/pjdfstest.sh`). A create/remove/rename in a
directory stamped the *child* node's times but left the **parent directory's `mtime`/`ctime`
frozen** (e.g. `mkdir/00.t` 33-34, `rmdir/00.t` 8-9, and the parent-time checks in
`link`/`unlink`/`rename`). This matters for prfs's own purpose: a backup tool that detects changed
directories by `mtime` would *miss* dirs whose entries were added or removed. The store
deliberately leaves link/unlink timestamps to the caller (design §3.5; `memstore.cpp`: "link/unlink
do NOT auto-touch mtime/ctime; callers set times explicitly"), and the nfsv3 front-end simply
wasn't stamping the parent. Fixed in `plugins/nfsv3/nfsv3.cpp`: a `touchDir()` helper sets the
parent's `mtime = ctime = now()` after a successful CREATE/MKDIR/SYMLINK/MKNOD (via `finishCreate`),
REMOVE, RMDIR, RENAME (both dirs), and LINK — before the `dir_wcc` is encoded, so the reply's
weak-cache-consistency attrs reflect the change. Only reached for live dirs (snapshot dirs `ROFS`
out first). Reproduction test `tests/nfsv3/nfsv3_test.cpp::ParentDirTimestampsOnMutation` (RED
before, GREEN after); the store/oracle are unchanged, so the differential suite is unaffected.
(Permission/ownership/timestamp-vs-wallclock failures pjdfstest also reports are **by design** —
prfs enforces no Unix modes and uses a deterministic logical clock.)
