# prfs

**prfs** — *Pseudo-Random File System* — is a **synthetic NFS target** for
exercising archive/backup tools. The filesystem namespace and metadata are real
and versioned, but file **content is generated on the fly** (pseudo-randomly,
from a per-file seed), never stored.

## Why it exists

Teams that build NFS-based archivers and backup tools need a large filesystem to
test against. The usual approach is to provision a big volume (say a terabyte),
spend days filling it with junk data, run the tool, mutate the tree at random,
run again — burning time and real money generating and *storing* content that is
never itself verified. All those bytes only exercise the tool's plumbing; nobody
checks that block #937,142 came back correct, because nobody knows what it was
supposed to be.

prfs removes the storage. The tree, the metadata, the versions, the NFS
semantics are all real — but a file's bytes are a **pure, deterministic function
of a small seed**, produced at READ time by a content generator. Nothing is
written to disk to represent file data. So a "1 TiB filesystem full of files" is
a few megabytes of metadata plus a generator, and every byte is **scripted,
reproducible, and checkable**: read the same file twice (or on another machine,
or next year) and you get the same bytes.

## How content works

- A regular file has a 64-bit **content seed** (its `nodeID` by default).
- READ asks the content provider for `(ContentConfig, seed, offset, length)` and
  gets deterministic bytes back — random-access, no dependence on neighbouring
  ranges. The `ContentConfig` tunes the data (entropy, sparse/hole ratio,
  cross-file dedup, block size); it lives in the store's metadata, not per file.
- **WRITE stores no bytes.** It folds the written data into the file's seed and
  updates the size. READ then regenerates content that *reflects* the write —
  different data in ⇒ different bytes out, same data ⇒ same bytes — while the
  store grows by nothing. (A filehandle into a snapshot is read-only: `ROFS`.)

This is the whole point: writes and rewrites cost a seed update, not a terabyte.

## Architecture

```
        NFS/MOUNT client (mount -t nfs)
                   │  ONC-RPC / TCP
        ┌──────────▼───────────┐
        │  nfsv3 plugin (.so)  │  Asio coroutines; full v3 + MOUNT
        └──────────┬───────────┘
                   │ IPrfs + IHost::read
        ┌──────────▼───────────┐   ┌───────────────────────┐
        │  prfs-host           │──▶│ content provider      │  seed → bytes
        │  (loads plugins, DI) │   │ (Philox/Threefry rng) │
        └──────────┬───────────┘   └───────────────────────┘
                   │ IPrfs
        ┌──────────▼───────────┐
        │  PrfsStore           │  versioned metadata (nodes, links, snapshots)
        │  over IKvStore       │
        └──────────┬───────────┘
             lmdb ─┴─ memory        selectable storage engine
```

- **Metadata store** — a versioned, snapshotting node/link model over a key/value
  engine (`lmdb` on disk, or `memory`). All filesystem logic lives here; an
  independent in-memory oracle validates it by differential testing.
- **Content provider** — the READ-path byte generator (`docs/content.md`), a
  counter-based RNG (Philox/Threefry) behind a config. Pure and storage-free.
- **DI registry** (`docs/di.md`) — storage engines, RNGs, content providers, and
  services are all self-registering providers keyed by interface + name.
- **Plugin host** — `prfs-host` `dlopen`s service plugins (`docs/plugins.md`):
  **nfsv3** (a full read-write NFSv3 + MOUNT server on Asio), **nfsv4** (an
  NFSv4.0 COMPOUND server — read + write + share/byte-range locking, no MOUNT),
  **luactl** (a live
  Lua console over a unix socket), **bigtree** (a native store-builder that
  populates a large synthetic tree on start), and **perf** (benchmarks the
  content generator, single- and multi-threaded).

## Build

```bash
meson setup build           # defaults: -Dstorage=lmdb -Dcontent=true -Dplugins=true
meson compile -C build
meson test    -C build      # differential, invariant, determinism, crash, content,
                            # protocol (nfsv3) + plugin lifecycle (host/luactl/perf)
```

Key options: `-Dstorage=lmdb|memory`, `-Drng=philox|threefry`,
`-Dcontent=`, `-Dplugins=`, `-Dlua=`. `lmdb` is the default (on-disk) engine;
`memory` is the ephemeral one — the whole suite passes on **both**
(`meson setup buildmem -Dstorage=memory && meson test -C buildmem`).

## Run and mount

Start the host (a high port needs no privileges; plugins next to the binary are
auto-discovered):

```bash
./build/prfs-host --clean --store /tmp/prfs --port 20490
```

To get a large tree to browse, build one first — either the native **bigtree**
plugin (fast; build-and-serve in one process, listed before nfsv3 so it
populates before the server accepts clients):

```bash
./build/prfs-host --store /tmp/prfs --clean \
  --plugin ./build/bigtree.so --plugin ./build/nfsv3.so --port 20490 \
  --set bigtree.total=1T --set bigtree.seed=42
```

or the equivalent `examples/bigtree.lua` via the Lua runner (`prfs-test`, needs
`-Dlua`). `--set KEY=VALUE` (repeatable) passes any option through to a plugin.

Mount it (root; explicit ports because prfs has no rpcbind, `nolock` because it
has no NLM):

```bash
sudo mount -t nfs \
  -o vers=3,proto=tcp,port=20490,mountport=20490,mountproto=tcp,nolock \
  127.0.0.1:/ /mnt/prfs

ls -la /mnt/prfs        # browse
cat  /mnt/prfs/somefile # generated bytes, reproducible
```

Everything read back is generated; everything written costs only a seed update.

> **Gotcha — the client page cache, not prfs.** `echo hi > f; cat f` over a mount
> returns `hi`, but prfs stored nothing: the WRITE folded your bytes into the
> file's seed, and your `cat` was served from the Linux NFS client's page cache
> (close-to-open caching) without reaching the server. To see prfs's *generated*
> content (bytes of the written length, not `hi`), bypass the cache — read with
> `dd if=f iflag=direct`, drop caches (`echo 1 | sudo tee /proc/sys/vm/drop_caches`)
> and re-`cat`, remount, or mount `-o noac`. Caching **generated** content is
> harmless (it's a pure function of the seed, so cached == fresh); only bytes a
> client wrote *itself* look stale, and that's true against any NFS server. Run
> the host with `--time-advance` for monotonic mtimes so cache-revalidating
> readers notice changes.

## Measuring read performance

A prfs READ is CPU work (generate the bytes), not I/O, and it parallelizes across
cores (the store lock is taken only *shared* for reads). The **perf** plugin times
the generator directly — no sockets, no RPC, no client cache — using the store's
own content policy, so it's the ceiling the NFS path approaches:

```bash
./build/prfs-host --store /tmp/prfs --plugin ./build/perf.so \
  --set perf.threads=16 --set perf.bytes=1G --set perf.blocksize=1M
# perf:  1 thread     84.0 MiB/s  (generator ceiling)
# perf: 16 threads   948.0 MiB/s  (11.3x, 59.2 MiB/s/core)
# perf: store path    84.2 MiB/s  (100% of ceiling)  [IHost::read, 1 thread]
```

The **store path** line reads through `IHost::read` (seed lookup + `ContentConfig`
fetch + generate) and compares it to the raw generator ceiling — the gap is the
per-READ store overhead the NFS path also pays (here: none worth mentioning).

To measure *end to end* over a mount instead, you **must** bypass the client page
cache or you'll just be timing RAM — read with `dd if=<file> of=/dev/null bs=1M
iflag=direct` (run several in parallel to saturate cores), or use `fio
--direct=1`.

## Plugins

`prfs-host` `dlopen`s service plugins — every `*.so` next to the binary is
auto-discovered (except `null.so`), or name them with `--plugin FILE.so`.
Plugin options are passed generically with `--set KEY=VALUE` (repeatable). Full
model in [`docs/plugins.md`](docs/plugins.md).

| Plugin | Purpose | Key options / flags |
|--------|---------|---------------------|
| `nfsv3` | Full read-write **NFSv3 + MOUNT** server on Asio coroutines — the real front-end | host `--port`, `--time-advance` |
| `nfsv4` | **NFSv4.0 COMPOUND** server (no MOUNT): read + write + share reservations + real-client **byte-range locking** (clientid/lease, OPEN_CONFIRM) | host `--port`, `--time-advance` |
| `luactl` | **Live Lua console** over a unix socket (`socat READLINE UNIX-CONNECT:…`), `fs` bound to the running store | host `--control PATH` |
| `bigtree` | Native **store-builder**: a large, reproducible, randomly-shaped tree (heavy-tailed file sizes, snapshots) | `--set bigtree.total/.depth/.dirs/.files/.seed/.snapshots/.force` |
| `perf` | **Read-performance benchmark**: times the content generator (single/multi-thread) and the store read path vs the ceiling | `--set perf.bytes/.threads/.blocksize/.seed/.size` |
| `null` | Reference/test front-end (one scripted store op); skipped by the default scan | `--set note=…` |

Build-and-serve a big tree in one process — list `bigtree` first so it populates
before `nfsv3` starts serving:

```bash
./build/prfs-host --store /tmp/prfs --clean --port 20490 \
  --plugin ./build/bigtree.so --plugin ./build/nfsv3.so \
  --set bigtree.total=1T --set bigtree.seed=42
```

## Layout

| Path | What |
|------|------|
| `include/prfs/` | public headers (`prfs.hpp`, `content.hpp`, `di.hpp`, `plugin.hpp`, …) |
| `src/core/` | `PrfsStore` — all filesystem logic over an `IKvStore` |
| `src/storage/` | `lmdb` and `memory` engines |
| `src/reference/` | `MemStore` — the independent oracle for differential tests |
| `src/content/` | the procedural content provider |
| `src/rng/` | counter-based generators (Philox, Threefry) |
| `src/host/` | `prfs-host` + the plugin loader |
| `src/lua/` | sol2 bindings + `prfs-test` scenario runner (`-Dlua`) |
| `plugins/nfsv3/` | the NFSv3 + MOUNT service |
| `plugins/nfsv4/` | the NFSv4.0 COMPOUND service (read + write + share/byte-range locking) |
| `plugins/luactl/` | live Lua console over a unix socket |
| `plugins/bigtree/` | native store-builder (large synthetic tree) |
| `plugins/perf/` | read-performance benchmark (content generator) |
| `examples/` | `bigtree.lua` — build a reproducible multi-TiB tree |
| `docs/` | design, content, di, plugins, bugs, todo (see below) |
| `tests/` | mirrors `src/`: contract/differential/invariant/determinism/crash + protocol |

## Documentation

| Doc | What it covers |
|-----|----------------|
| [`docs/design.md`](docs/design.md) | Authoritative spec: node/link/snapshot model, versioning, diffs, invariants |
| [`docs/content.md`](docs/content.md) | The procedural content provider: `ContentConfig`, seed→bytes, sparse/dedup |
| [`docs/di.md`](docs/di.md) | The DI registry: interfaces, names, provide/resolve, self-registration |
| [`docs/plugins.md`](docs/plugins.md) | Plugin ABI + host model; the in-tree services (nfsv3, nfsv4, luactl, bigtree, perf) |
| [`docs/bugs.md`](docs/bugs.md) | The design-bug log (B1–B9) that the `T*` tasks fixed |
| [`docs/todo.md`](docs/todo.md) | Living task list with status, keyed to `docs/bugs.md` |

## License

MIT — © Michael Rolnik &lt;mrolnik@gmail.com&gt;
