# prfs — TODO

Living task list. `T*` items fix the correspondingly-numbered risks in `bugs.md`.

> **Every task follows design §13 — SOLID and test-first (a failing test first, then code to
> green against the reference oracle). Non-negotiable.**

## Design gaps to close (before / with scaffolding)

- [ ] **T1** — directory DAG (design §2.2): DFS cycle-prevention on dir link/move (files skip, early-exit). Store `nlink = #incoming` (+ optional per-dir subdir counter for POSIX-compat). NFS-front-end tasks (not `libprfs`): `..` = **via-parent** (filehandle-encoded, depth-bounded chain); dir-`nlink` reporting mode (POSIX-compat default / faithful). (fixes B1)
- ✅ **T2 — design done** (fixes B2): `diffNodes(A,B)` = node-level state comparison + on-demand `diffPaths` over a `changes` candidate index (design §6.1). Implementation tracked under Scaffolding.
- [ ] **T3** — add `snaps: snapId → {ctime, label}` table; `snapshot()` writes it (fixes B3).
- [ ] **T4** — define `readdir` cookies + live-dir mutate-mid-scan behaviour (fixes B4).
- [ ] **T5** — define the synthesized `.snapshot` node: identity, attrs, `readdir`, filehandle (fixes B5).
- [ ] **T6** — define timestamp policy: deterministic logical / script-driven clock (fixes B6).
- [ ] **T7** — state `size` vs content-block-structure relationship (fixes B7).
- [ ] **T8** — mark `linkMode` reserved, or defer until the hybrid decision (fixes B8).
- [ ] Define `Error`, `Stats`, `Options` when fleshing design §7.

## Open decisions (design §11)

- [ ] Hybrid link-set: pure COW vs per-dir COW/LOG vs bucketed COW. (Deciding input: are large directories mutated across snapshots?)
- [ ] Content layout: procedural recipe vs explicit extent list vs both.
- [ ] Block size: per fs/folder/file; denormalized `blockSize` attr resolved at create time?
- ✅ `changes`: **decided** — candidate index + state-comparison (design §6.1).
- [ ] GC: write the invariant into the design now; implement compaction later.

## Scaffolding (once the critical/high gaps are closed)

- ✅ Repo skeleton + Meson build (`include/`, `src/reference/`, `tests/`).
- ✅ `INode`/`IPrfs` interface headers (interface-first, design §7).
- ✅ In-memory **reference model** / oracle (`src/reference/memstore.cpp`).
- ✅ gtest **contract tests** — 12 cases green (create/link/lookup/readdir, hardlinks, unlink, symlink, device, snapshot range-back + immutability, diffNodes, diffPaths, cycle-prevention, move-is-rename, stats).
- ✅ **clang-format** (`.clang-format`) + meson `format` target + enforced `clang-format` test gate.
- ✅ Storage backend **build-config** (`-Dstorage=lmdb|memory`) via the `openPrfs` seam.
- ✅ **Storage-backend interface** (`IKvStore`/`IKvTxn`/`IKvCursor`): one `PrfsStore` (all FS logic, design §5/§6) over swappable engines.
- ✅ Vendor LMDB as a git submodule (`third_party/lmdb`) + **`LmdbKv`** and **`MemKv`** engines — contract tests green against both via `INSTANTIATE_TEST_SUITE_P` (oracle + backend).
- [ ] **Differential harness**: randomized op sequences, assert PrfsStore backend ≡ oracle at every snapshot.
- [ ] Extend tests: invariant/property, determinism, crash-safety.
- [ ] `prfs-lua` bindings (sol2) + `prfs-test` Lua harness (§12).
- [ ] Statistics (§9), incl. the `FSSTAT`/`FSINFO` mapping.

## Later / separate modules

- [ ] **Content provider** (own doc): block/pattern model (ZERO/ONE/RANDOM/SEEDED/DEDUP), procedural knobs, dedup.
- [ ] **NFS front-end**: v3 first; v4 subset or NFS-Ganesha FSAL per the earlier analysis.
- [ ] **GC / compaction**: reclaim superseded link-set versions + unreferenced `content`/`strings`.
- [ ] **Dedup-ratio stats** (phase-2): content refcounting (`logicalSize` vs `physicalSize`).
