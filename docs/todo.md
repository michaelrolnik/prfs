# prfs — TODO

Living task list. `T*` items fix the correspondingly-numbered risks in `bugs.md`.

> **Every task follows design §13 — SOLID and test-first (a failing test first, then code to
> green against the reference oracle). Non-negotiable.**

Status: ✅ done · 🚧 in progress · ⬜ open.

| ID  | Status | Task                                                                        |
| --- | ------ | --------------------------------------------------------------------------- |
| T1  | ✅     | Directory DAG: DFS cycle-prevention on dir link/move (fixes B1)             |
| T2  | ✅     | `diffNodes` / `diffPaths` design (fixes B2)                                 |
| T3  | ✅     | `snaps: snapId → {ctime, label}` table (fixes B3)                           |
| T4  | ✅     | `readdir` cookies + live-dir mutate-mid-scan (fixes B4)                     |
| T5  | ✅     | Synthesized `.snapshot` node: identity, attrs, `readdir`, filehandle (B5)   |
| T6  | ✅     | Deterministic logical / script-driven clock (fixes B6)                     |
| T7  | ✅     | `size` vs content-block-structure authority (fixes B7)                      |
| T8  | ✅     | Mark `linkMode` reserved / defer (fixes B8)                                 |
| T9  | ⬜     | Define `Error`, `Stats`, `Options` when fleshing design §7                  |
| D1  | ✅     | Link-set scaling: range-partitioned bucketed COW (design §11.1)             |
| D2  | ✅     | Content: config-driven, generated per file (nodeID seed); nothing stored (§11.2) |
| D3  | ✅     | Block size: filesystem-level (+ folder override); not per-file (§11.3)      |
| D4  | ✅     | `changes`: candidate index + state-comparison (design §6.1)                 |
| D5  | ✅     | GC: invariant fixed now, compaction later (design §11.4)                    |
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
| S12 | ✅     | Statistics (§9): `FSSTAT`/`FSINFO` mapping                                  |
| L1  | ✅     | Content provider (`libprfs-content`) + `rng` (di); FS config in store meta; `prfs.content` Lua |
| L2  | ✅     | NFS front-end as a plugin: `nfsv3` + MOUNT shipped (`nfsv4` still future)    |
| L3  | ⬜     | GC / compaction: reclaim superseded link-set versions + unref content       |
| L4  | ⬜     | Dedup-ratio stats (phase-2): content refcounting                            |
| L5  | ✅     | DI registry ✅ · plugin host ✅ (loader, prfs-host, serve loop, `--set`)    |
| L6  | ✅     | Extra plugins: `luactl` (console) · `bigtree` (tree builder) · `perf` (read bench) |

---

## Design gaps to close (before / with scaffolding)

- **T1** ✅ — directory DAG (design §2.2): DFS cycle-prevention on dir link/move — `link`/`move` reject a cycle-closing directory edge via `reachable(child, dir)` over directory down-links; files skip it, self-links caught, `move` links-before-unlinks so it's covered and atomic. Store keeps `nlink = #incoming`. Both engines; hardened by `tests/core/dag.cpp` (7 cases × 2 engines). Fixes B1. *Carved out to the NFS front-end (todo L2, not `libprfs`):* `..` = **via-parent** (filehandle-encoded, depth-bounded chain); dir-`nlink` reporting mode (POSIX-compat `2 + #subdirs` default / faithful `#parents`).
- **T2** ✅ — design done (fixes B2): `diffNodes(A,B)` = node-level state comparison + on-demand `diffPaths` over a `changes` candidate index (design §6.1). Implementation tracked under Scaffolding.
- **T3** ✅ — `snaps: snapId → {ctime, label}` sub-store (`Kv::Snaps`), written by `snapshot(label)` (ctime = logical `now()`, §3.5), read via `IPrfs::snapInfo(id)` (design §3.2). Persisted (survives reopen); both engines; Lua `store:snapshot([label])` / `store:snapInfo(id)`; tests `tests/core/snapshot.cpp` + crash-suite persistence. Fixes B3.
- **T4** ✅ — paginated `readdirPage(dir, after, max)` with a **name cursor** (design §6.2): stable across concurrent add/remove (entry returned once; adds-ahead appear, adds-behind don't, removals-ahead skipped, removed-cursor still resumes), `eof` set as soon as the end is known; `.snapshot` dir paginates numerically. Both engines; Lua `store:readdirPage`; `tests/core/readdirpage.cpp`. Fixes B4. *Deferred to L2:* string-cookie → 64-bit NFS cookie mapping for over-long names.
- **T5** ✅ — synthesized `.snapshot` node (design §3.2): concrete id `D.id | (1<<63)` (reserved top-bit id-space, filehandle round-trip), GETATTR-able read-only `0555` dir (nlink 1, times mirror `D`), `readdir` lists `"N"` per snapshot `D` existed at, `lookup(snapDir,"N")` → `(D.id, N)`. Live-view-only (no nesting), DIR-only, hidden from `readdir` but resolvable; `link`/`move` reject the reserved name. Both engines; Lua `prfs.SNAPSHOT_NAME`; `tests/core/snapdir.cpp`. Fixes B5. *Deferred to L2:* config flag to list `.snapshot` in `readdir`.
- **T6** ✅ — timestamp policy: a deterministic, script-driven **logical clock** (design §3.5). `IPrfs::now()`/`setTime()` on the interface; new nodes stamp `atime=mtime=ctime=now()`, other time changes explicit; never wall-clock; persisted in `meta` (survives reopen). Both engines; Lua `store:now()`/`setTime()`; tests `tests/core/clock.cpp` + crash-suite persistence. Fixes B6.
- **T7** ✅ — `size` (the per-node attr) is authoritative for `st_size`/`READ`; the shared block structure is size-parametric and the store never derives `size` from `content` (design §2.1). Fixes B7.
- **T8** ✅ — `linkMode` marked **reserved** in the schema; implementation is pure COW and `NodeRec` carries no `linkMode` field, so the schema no longer commits to the undecided §3.4 hybrid (design §5). Fixes B8.
- **T9** ⬜ — define `Error`, `Stats`, `Options` when fleshing design §7.

## Decisions (design §11)

- **D1** ✅ — Link-set scaling: **range-partitioned bucketed COW** (design §11.1). One read/write path, copy bounded to the touched bucket, buckets partitioned by name range so the §6.2 readdir cursor stays stable. v1 ships single-bucket (== pure COW §3.3); multi-bucket + `linkMode`/bucket-count reserved.
- **D2** ✅ — Content: **config-driven, generated on the fly; nothing stored** (design §11.2). A file's bytes = f(effective `ContentConfig`, `nodeID` seed, offset); no per-file recipe. Config is FS-wide (in `meta`) with an optional per-folder override (nice-to-have). Knobs: `blockSize`, `entropy`, `dedupPercent` (size-independent dedup ratio), `sparsePercent`. Spec'd in `docs/content.md` (L1).
- **D3** ✅ — Block size: **filesystem-level** (+ optional per-folder override), part of `ContentConfig`; **not per-file**, not denormalized on nodes (design §11.3).
- **D4** ✅ — `changes`: candidate index + state-comparison (design §6.1).
- **D5** ✅ — GC: **invariant fixed now, compaction later** (design §11.4). Reclaimable ⟺ no retained snapshot resolves to it; inert until snapshot pruning + content refcounting exist.

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
- **S12** ✅ — Statistics (§9): **`FSSTAT`/`FSINFO` mapping** (`include/prfs/fsstat.hpp`, `src/nfs/fsstat.cpp`, lib `prfs-nfs`). Pure engine-independent projection of the O(1) `Stats` → `FsStat`/`FsInfo` given policy `FsConfig` (synthetic capacity, transfer sizes, `properties` = LINK|SYMLINK|HOMOGENEOUS|CANSETTIME); capacity clamps up so free never underflows, used bytes round to `blockSize`. Unit + store-integration tests (`tests/nfs/fsstat.cpp`); Lua `store:fsStat()` / `prfs.fsInfo()`. The store's own incremental `Stats` counters remain a later refinement (currently recomputed by scan).

## Later / separate modules

- **L1** ✅ — **Content provider** (`libprfs-content`) + **`rng`** (di providers), built & tested ([`docs/content.md`](content.md)). Config-driven generation (`ContentConfig`: blockSize/entropy/dedupPercent/sparsePercent), deterministic random-access, whole-FS dedup; generators are `IRng` di providers (Philox default + Threefry, Random123 submodule), `-Drng=` default / `setActive`. `IPrfs::contentConfig()/setContentConfig()` persist the FS config (opaque blob) in `meta`. Lua: `store:contentConfig`/`setContentConfig`, `prfs.content.config/read/allocatedBlocks`, `prfs.rng` (gated by `-Dcontent`). Tests: `tests/content/`, `tests/core/contentconfig.cpp`, crash-suite persistence, `smoke.lua`.
- **L5 dep** — the tool/host layer uses **spdlog** (logging) and **CLI11** (CLI) as git submodules; leaf libraries stay logging/CLI-free.
- **L2** ✅ — **NFS front-end** shipped as the `nfsv3` plugin (`plugins/nfsv3`): the full v3 procedure set + MOUNT program on standalone-Asio coroutines with an own io-thread pool (the self-contained XDR + thread-pool TCP-server option from the analysis; not `rpcgen`, not Ganesha). 16-byte `(nodeID, snapId)` filehandles via `nodeById`; snapshot views read-only (`ROFS`); `..` via-parent; AUTH_SYS creds stamped; permissive ACCESS; synthesized `.snapshot` LOOKUP-able but hidden from readdir. `tests/nfsv3/`. *Remaining carve-outs:* READDIR cookies use an O(n) ordinal (not the `readdirPage` name-cursor); single `"/"` MOUNT export; no rpcbind/NLM (mount with explicit ports + `nolock`); directory-`nlink` mode still POSIX-compat only; `nfsv4` is a future plugin. **Conformance:** `scripts/pjdfstest.sh` runs the pjdfstest POSIX suite over a mount (`sudo scripts/pjdfstest.sh [category]`) — structural categories largely pass; permission/ownership/timestamp-vs-wallclock failures are by design (no mode enforcement, deterministic clock). It surfaced **B10** (parent-dir mtime/ctime not stamped on namespace mutations), fixed via `touchDir` in the nfsv3 handlers (regression `tests/nfsv3/…ParentDirTimestampsOnMutation`).
- **L3** ⬜ — **GC / compaction**: reclaim superseded link-set versions + unreferenced `content`/`strings`.
- **L4** ⬜ — **Dedup-ratio stats** (phase-2): content refcounting (`logicalSize` vs `physicalSize`).
- **L5a** ✅ — **DI registry** ([`docs/di.md`](di.md)): header-only `include/prfs/di.hpp` — typed container keyed by `(interface-id, name)`: `provide`/`resolve`/`tryResolve`/`resolveAll`/`withdraw`/`has`/`names`/`ids`/`require`/`requireAllResolved`, a `Register` self-registration helper, and a `global()` Meyers singleton. Fail-loud (`resolve` throws `Unresolved`); a `Registry` *is* a scope (RAII). `tests/di/di_test.cpp` (9 cases). `rng` is migrated onto it as the first real provider: generators implement `IRng` and self-register (`di::Register<IRng>`, `link_whole`), `content` resolves via `di::resolve<IRng>`.
- **L5b** ✅ — **Plugin host** ([`docs/plugins.md`](plugins.md)) over the DI registry. `include/prfs/plugin.hpp` (`IHost`, `IPlugin`, `IService`, `ABI`, `extern "C"` factory); `include/prfs/host.hpp` + `src/host/host.cpp` (`Host` = IHost; `Loader` = `dlopen`+ABI-check+create → provide, `resolveAll<IService>`→start, teardown → stop/destroy/`withdraw`/`dlclose`); `prfs-host` executable (CLI11 + spdlog) auto-discovers `*.so` next to the binary (skipping `null.so`), serves until SIGINT, then stops/joins. spdlog + CLI11 vendored as submodules. `IContentProvider` added — content generation is a di provider (default `"config"`; `Host::read` resolves it). `IHost::storeMutex()` (shared_mutex) serializes store access across services. Plugin options are set via the generic `--set KEY=VALUE` pass-through into `IHost::option()`. `tests/host/host_test.cpp` (adopt, dlopen lifecycle, bad-path).
- **L6** ✅ — **Extra service plugins.** `plugins/luactl` — a live Lua console over a unix socket (`--control`; `socat READLINE UNIX-CONNECT:/tmp/prfs.sock`), `fs` bound to the running store, thread per connection, mutations serialized by the host store lock; `tests/luactl/`. `plugins/bigtree` — a native store-builder (C++ twin of `examples/bigtree.lua`): builds the same irregular-but-reproducible tree with heavy-tailed random file sizes normalized to a target total, symlinks/hardlinks, a base snapshot plus daily rounds — directly via `IPrfs`, so it is far faster than the Lua path. Options via `--set bigtree.*` (`total` K/M/G/T/P, `depth`, `dirs`, `files`, `seed`, `snapshots`, `force`); builds only into an empty store unless `bigtree.force`; sets a content policy when `-Dcontent`. Pair `bigtree` (listed first) with `nfsv3` to build-and-serve in one process. `plugins/perf` — read-performance benchmark: times the content generator directly (no sockets/RPC/cache) using the store's `ContentConfig`, single- and multi-threaded, reporting MiB/s + scaling (a READ is CPU-bound and parallelizes under the shared store lock). Options via `--set perf.*` (`bytes`, `threads`, `blocksize`, `seed`, `size`). It's the ceiling the NFS path approaches; over a mount, measure cache-bypassed (`dd iflag=direct`). Both plugins link `prfs_content_dep` under `-Dcontent`.
