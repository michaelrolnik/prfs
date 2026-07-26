# prfs — TODO

Living task list. `T*` items fix the correspondingly-numbered risks in `bugs.md`.

> **Every task follows design §13 — SOLID and test-first (a failing test first, then code to
> green against the reference oracle). Non-negotiable.**

Status: ✅ done · ⬜ open.

| ID  | Status | Task                                                                        |
| --- | ------ | --------------------------------------------------------------------------- |
| T1  | ⬜     | Directory DAG: DFS cycle-prevention on dir link/move (fixes B1)             |
| T2  | ✅     | `diffNodes` / `diffPaths` design (fixes B2)                                 |
| T3  | ⬜     | `snaps: snapId → {ctime, label}` table (fixes B3)                           |
| T4  | ⬜     | `readdir` cookies + live-dir mutate-mid-scan (fixes B4)                     |
| T5  | ⬜     | Synthesized `.snapshot` node: identity, attrs, `readdir`, filehandle (B5)   |
| T6  | ⬜     | Deterministic logical / script-driven clock (fixes B6)                     |
| T7  | ⬜     | `size` vs content-block-structure authority (fixes B7)                      |
| T8  | ⬜     | Mark `linkMode` reserved / defer (fixes B8)                                 |
| T9  | ⬜     | Define `Error`, `Stats`, `Options` when fleshing design §7                  |
| D1  | ⬜     | Hybrid link-set: pure COW vs per-dir COW/LOG vs bucketed COW                |
| D2  | ⬜     | Content layout: procedural recipe vs explicit extent list vs both           |
| D3  | ⬜     | Block size: per fs/folder/file; denormalized `blockSize` at create?         |
| D4  | ✅     | `changes`: candidate index + state-comparison (design §6.1)                 |
| D5  | ⬜     | GC: write the invariant into the design now; compaction later               |
| S1  | ✅     | Repo skeleton + Meson build                                                 |
| S2  | ✅     | `INode`/`IPrfs` interface headers                                           |
| S3  | ✅     | In-memory reference model / oracle                                          |
| S4  | ✅     | gtest contract tests — 12 cases                                             |
| S5  | ✅     | clang-format + `format` target + enforced test gate                         |
| S6  | ✅     | Storage backend build-config (`-Dstorage=lmdb\|memory`)                     |
| S7  | ✅     | Storage-backend interface (`IKvStore`/`IKvTxn`/`IKvCursor`)                 |
| S8  | ✅     | Vendor LMDB submodule + `LmdbKv` / `MemKv` engines                          |
| S9  | ✅     | Differential harness (caught B9)                                            |
| S10 | ✅     | Extend tests: invariant/property, determinism, crash-safety                 |
| S11 | ✅     | `prfs-lua` bindings (sol2) + `prfs-test` Lua harness (§12)                  |
| S12 | ⬜     | Statistics (§9), incl. `FSSTAT`/`FSINFO` mapping                            |
| L1  | ⬜     | Content provider (own doc): block/pattern model, dedup                      |
| L2  | ⬜     | NFS front-end: v3 first; v4 subset or NFS-Ganesha FSAL                      |
| L3  | ⬜     | GC / compaction: reclaim superseded link-set versions + unref content       |
| L4  | ⬜     | Dedup-ratio stats (phase-2): content refcounting                            |

---

## Design gaps to close (before / with scaffolding)

- **T1** ⬜ — directory DAG (design §2.2): DFS cycle-prevention on dir link/move (files skip, early-exit). Store `nlink = #incoming` (+ optional per-dir subdir counter for POSIX-compat). NFS-front-end tasks (not `libprfs`): `..` = **via-parent** (filehandle-encoded, depth-bounded chain); dir-`nlink` reporting mode (POSIX-compat default / faithful). (fixes B1)
- **T2** ✅ — design done (fixes B2): `diffNodes(A,B)` = node-level state comparison + on-demand `diffPaths` over a `changes` candidate index (design §6.1). Implementation tracked under Scaffolding.
- **T3** ⬜ — add `snaps: snapId → {ctime, label}` table; `snapshot()` writes it (fixes B3).
- **T4** ⬜ — define `readdir` cookies + live-dir mutate-mid-scan behaviour (fixes B4).
- **T5** ⬜ — define the synthesized `.snapshot` node: identity, attrs, `readdir`, filehandle (fixes B5).
- **T6** ⬜ — define timestamp policy: deterministic logical / script-driven clock (fixes B6).
- **T7** ⬜ — state `size` vs content-block-structure relationship (fixes B7).
- **T8** ⬜ — mark `linkMode` reserved, or defer until the hybrid decision (fixes B8).
- **T9** ⬜ — define `Error`, `Stats`, `Options` when fleshing design §7.

## Open decisions (design §11)

- **D1** ⬜ — Hybrid link-set: pure COW vs per-dir COW/LOG vs bucketed COW. (Deciding input: are large directories mutated across snapshots?)
- **D2** ⬜ — Content layout: procedural recipe vs explicit extent list vs both.
- **D3** ⬜ — Block size: per fs/folder/file; denormalized `blockSize` attr resolved at create time?
- **D4** ✅ — `changes`: decided — candidate index + state-comparison (design §6.1).
- **D5** ⬜ — GC: write the invariant into the design now; implement compaction later.

## Scaffolding (once the critical/high gaps are closed)

- **S1** ✅ — Repo skeleton + Meson build (`include/`, `src/reference/`, `tests/`).
- **S2** ✅ — `INode`/`IPrfs` interface headers (interface-first, design §7).
- **S3** ✅ — In-memory **reference model** / oracle (`src/reference/memstore.cpp`).
- **S4** ✅ — gtest **contract tests** — 12 cases green (create/link/lookup/readdir, hardlinks, unlink, symlink, device, snapshot range-back + immutability, diffNodes, diffPaths, cycle-prevention, move-is-rename, stats).
- **S5** ✅ — **clang-format** (`.clang-format`) + meson `format` target + enforced `clang-format` test gate.
- **S6** ✅ — Storage backend **build-config** (`-Dstorage=lmdb|memory`) via the `openPrfs` seam.
- **S7** ✅ — **Storage-backend interface** (`IKvStore`/`IKvTxn`/`IKvCursor`): one `PrfsStore` (all FS logic, design §5/§6) over swappable engines.
- **S8** ✅ — Vendor LMDB as a git submodule (`third_party/lmdb`) + **`LmdbKv`** and **`MemKv`** engines — contract tests green against both via `INSTANTIATE_TEST_SUITE_P` (oracle + backend).
- **S9** ✅ — **Differential harness** (`tests/test_diff.cpp`): pseudo-random op sequences driven into oracle + backend in lockstep; canonical reachable-graph serialization, error codes, snapshot ids and global stats cross-checked every step; every snapshot view re-verified at the end. 8 seeds × 1500 ops. Caught the `stats().links` orphan-dir undercount (bugs.md B9).
- **S10** ✅ — Extend tests: **invariant/property** (`tests/core/invariant.cpp`: nlink=#parents, unique names, lookup≡readdir, Σ down-links≡`stats().links`, live-node counts, snapshot immutability, failed-op-is-no-op), **determinism** (`tests/core/determinism.cpp`: same op stream → identical view/stats/ids across fresh instances), **crash-safety** (`tests/storage/lmdb/crash.cpp`: committed state + range-back views survive drop/reopen; post-reopen writes persist; auto-skips non-persistent engines). Test tree mirrors `src/` (`tests/core/*` = `PrfsStore`, `tests/storage/lmdb/*` = engine-specific); shared `tests/support.hpp` (canon, temp dir, `Lockstep` driver).
- **S11** ✅ — **`prfs-lua`** bindings (sol2) + **`prfs-test`** scenario runner (§12). `libprfs` stays lua-free; `registerLua()` (`src/lua/bindings.cpp`, `include/prfs/lua.hpp`) installs a `prfs` global (factories `open`/`mem`, `Type`/`Error`/`NodeChange`/`PathChange` enums) plus `IPrfs`/`INode` usertypes. `prfs-test` (`src/lua/harness.cpp`) embeds Lua and runs a `.lua` scenario; `tests/lua/smoke.lua` exercises the surface and is a meson test (`lua-scenario`). Lua 5.4.7 + sol2 v3.5.0 vendored as git submodules; whole layer gated by `-Dlua` (default on).
- **S12** ⬜ — Statistics (§9), incl. the `FSSTAT`/`FSINFO` mapping.

## Later / separate modules

- **L1** ⬜ — **Content provider** (own doc): block/pattern model (ZERO/ONE/RANDOM/SEEDED/DEDUP), procedural knobs, dedup.
- **L2** ⬜ — **NFS front-end**: v3 first; v4 subset or NFS-Ganesha FSAL per the earlier analysis.
- **L3** ⬜ — **GC / compaction**: reclaim superseded link-set versions + unreferenced `content`/`strings`.
- **L4** ⬜ — **Dedup-ratio stats** (phase-2): content refcounting (`logicalSize` vs `physicalSize`).
