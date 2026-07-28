---
title: prfs
---

# prfs — Pseudo-Random File System

A **synthetic NFS target** for exercising archive/backup tools. The namespace,
metadata, versioning, and NFS (v3 + v4.0) semantics are all real, but file
**content is generated on the fly from a per-file seed and never stored** — so a "1 TiB
filesystem full of files" is a few hundred KB of metadata plus a generator, and
every byte is reproducible.

## Documentation

- [Design](design.md) — authoritative spec: node/link/snapshot model, versioning, diffs, invariants
- [Content](content.md) — the procedural content provider: `ContentConfig`, seed→bytes, sparse/dedup
- [DI registry](di.md) — interfaces, names, provide/resolve, self-registration
- [Plugins](plugins.md) — plugin ABI + host model; in-tree services (nfsv3, nfsv4, luactl, bigtree, perf)
- [Known bugs](bugs.md) — the design-bug log (B1–B13) and their fixes
- [TODO](todo.md) — living task list

## At a glance

- **Store** — versioned node/link/snapshot model over a KV engine (`lmdb` on disk / `memory`), validated by an independent oracle via differential testing.
- **Content** — a WRITE folds its bytes into the file's seed (nothing stored); READ regenerates deterministically.
- **Serving** — a full read-write **NFSv3 + MOUNT** server you can `mount -t nfs`, and an **NFSv4.0** server (`mount -o vers=4.0`) with read + write + share reservations + real-client byte-range locking; plus plugins for building trees (`bigtree`), a live Lua console (`luactl`), and read benchmarking (`perf`).

Source: [github.com/michaelrolnik/prfs](https://github.com/michaelrolnik/prfs)
