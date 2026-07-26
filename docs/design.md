# prfs — design

**Status:** draft / living document
**Scope:** the metadata-management library only. Content generation and the NFS
front-end are separate modules layered on top.
**Development principles (binding):** **SOLID** and **test-first** are required practices for
all work on this codebase — kept and followed for every change, not optional. See §13.

---

## 1. Purpose

`prfs` is a library that manages a **versioned filesystem namespace** — nodes,
the links between them, their names, attributes, and point-in-time snapshots — backed
by an embedded key/value store. It is the metadata brain of a synthetic NFS target used
to exercise archive/backup tools, but it knows nothing about file *content*.

**Goals**

- Present an arbitrarily large, realistic directory tree cheaply.
- Full snapshot/versioning with cheap, controlled diffs (for incremental-backup testing).
- Reproducible and deterministic.
- Small, fast, lean dependency footprint.
- Optimize the hot path: reads, and `readdir` in particular.

**Non-goals (explicitly out of scope for this library)**

- Generating or storing file **content**. A regular file references an opaque *content*
  entity (a "block structure"); a separate *content provider* turns it into bytes. The
  store interns and hands back those bytes but never interprets them.
- Speaking any network protocol. The NFS (v3/v4) server is a separate front-end.

**Conventions**

- **All IDs are 64-bit** (`nodeID`, `stringID`, `contentID`, `snapId`, and the link-set
  versions). Every key field is a `uint64`.
- All multi-field keys are **big-endian packed** so lexicographic byte order equals
  logical order — this is what makes range order and the `snapId ≤ G` range-back correct.

---

## 2. Core model

Everything is a **node**. There is no structural distinction between a file, a
directory, a symlink, or a device — they are all nodes; a node's `type` attribute tells a
consumer how to interpret it, and in particular how to interpret its `spec`. **`type` is
immutable** — fixed at creation, never changed (there is no "change type" operation);
turning one kind of node into another means creating a new node.

- **Node** — identity (`nodeID`) + attributes + a polymorphic reference (`spec`). A node
  does **not** carry its own name.
- **Name** — a property of the *edge*, not the node. The same node reached under two
  names is a hard link.
- **Down link** — a directory entry: an edge from a container node to a child node,
  bearing the name (as a `stringID`). "Directory contents" = the set of a container's
  down links.
- **Up link** — the reverse edge (child → container), so a node can find its
  parents/names. Multiple up links == multiple hard links.
- **Interned entities** — names, symlink targets, and file content are **interned**: each
  distinct value is stored once, hash-deduplicated, and referenced everywhere by a 64-bit
  id. Two such tables exist (`strings`, `content`) with identical machinery.

A **node handle** is the pair `(nodeID, snapId)`: *which* node, and *at which snapshot
view* to read it and its subtree.

### 2.1 `spec` — a reference interpreted by node type

A single 64-bit field on the node, reused per type (the way a real inode overloads its
fields rather than carrying a fat struct):

| Node type      | `spec` holds                                  | Storage                                    |
|----------------|------------------------------------------------|--------------------------------------------|
| `LNK`          | `stringID` of the target                       | interned `strings` table                   |
| `BLK` / `CHR`  | device number `(major:32 ‖ minor:32)`          | **inline** — fits in the 64 bits, no table |
| `REG`          | `contentID` of the block structure             | interned `content` table                   |
| `DIR`          | `0` (contents are its down links, `dnLinkVer`) | —                                          |
| `FIFO` / `SOCK`| `0`                                            | —                                          |

- **Symlink**: `readlink(node,G)` → `strings[getattr(node,G).spec].bytes`.
- **Device**: the `(major, minor)` pair is the whole "content"; it packs into the 64-bit
  `spec` directly (this is the old `spec`; NFS `specdata1`/`specdata2`).
- **Regular file**: `spec` is a `contentID` into the interned `content` table, whose
  entry is the opaque *block structure* (the extent/pattern recipe). Because `content` is
  hash-deduplicated, two identical files share one `contentID` → metadata-level dedup, and
  the block structure is stored once. The store never parses it; the content provider does.

There is deliberately **no per-node `seed`** — a regular file's content recipe (seed,
compressibility, dedup, block patterns) lives inside its shared `content` entity.

**`size` is authoritative for `st_size`.** The per-node `size` attribute — not the shared
block structure — is what `GETATTR`/`READ` report and what the content provider is asked to
fill up to. The block structure is deliberately **size-parametric**: two files can share one
`contentID` yet have different `size`s (same recipe, different lengths), so the provider
generates `size` bytes from the recipe. `size` is set explicitly on the node; the store never
derives it from `content` (which it never parses). A `READ` past `size` is short/EOF
regardless of what the recipe could produce.

### 2.2 Directory graph — multiple parents (a DAG)

A directory may have **several parents** (several up links), so the namespace is a directed
**acyclic** graph, not a tree. This is deliberate — it lets scenarios build hard-linked,
shared-subtree structures that ordinary filesystems refuse to create, stressing how an
archiver copes.

- **Acyclicity is an invariant.** A directory link/move is rejected if it would create a
  cycle. Because the graph is always acyclic beforehand, adding edge `dst → node` can only
  close a cycle *through that edge*, so the check is a single boolean reachability query —
  early-exit, no enumeration:

  ```
  canLink(dst, node):
      if node.type != DIR:             return OK        # files are leaves — cannot cycle
      if reachable(from=node, to=dst): return EINVAL    # DFS/BFS, stop at first hit
      return OK
  ```

  Only directory links are checked; file hard links never cycle. During normal top-down
  construction the reachable subtree is empty, so the check is O(1); only moving an
  already-populated directory costs O(reachable subgraph).

- **`..` is multi-valued → via-parent (path-faithful).** The traversal parent is carried in
  the **filehandle**, so `..` returns the parent actually descended through, and the same
  directory reached via different parents has distinct filehandles. Recursive `../..` needs
  the ancestor chain, and NFS filehandles are size-bounded (v3 ≤ 64 B, v4 ≤ 128 B), so encode
  a **bounded ancestor-nodeID chain** (pop one per `..`); since this is a synthetic tool,
  **bound tree depth** so the chain always fits (fully faithful), with a primary-parent
  fallback only past that depth. This lives entirely in the **NFS front-end** (filehandle
  encoding + `.`/`..` synthesis) — the store's only role is `parents()` enumeration.

- **`nlink`.** The **store** maintains the honest structural count: `nlink = #incoming
  links` — a file's hard-link count, a directory's parent count (±1 per link/unlink, O(1)).
  That is *not* POSIX's directory `nlink = 2 + #subdirs`, and the difference matters:
  `find`/`fts` and some archivers use the "dir `nlink` − 2 = #subdirs" **leaf optimization**.
  So the **NFS front-end** reports directory `nlink` in one of two modes (a test knob):
  **POSIX-compat** (default) = `2 + #child-subdirs` (leaf optimization works; needs a per-dir
  subdir counter, O(1) to maintain), or **faithful** = the true `#parents` (deliberately
  stresses tools that assume the POSIX convention). Files always report the true hard-link
  count.

- **Walk multiplicity (intended).** A tree-walking archiver reaches a shared subtree once per
  parent — backing it up repeatedly unless it dedups by `(dev, ino)`. That is a feature here;
  note that path-based `du` legitimately double-counts while the O(1) `stats` (§9), which
  count nodes/links not paths, stay correct.

---

## 3. Versioning & snapshots

Every entity record is stamped with the `snapId` it was written in. "Version" **is** the
snapshot id.

- There is one mutable **current** snapshot, `cur_snap`. Mutations write records stamped
  with `cur_snap`.
- `snapshot()` seals the current view and advances `cur_snap`.
- A read "as of snapshot `G`" resolves a node record by a **range-back**: seek to
  `(nodeID ‖ G)` and step to the largest record with `snapId ≤ G`.

Taking a snapshot itself touches only a couple of keys. Link sets are copied lazily, on
modification (§3.3) — never at snapshot time.

### 3.5 Timestamps — a logical, script-driven clock

`atime`/`mtime`/`ctime` come from a **logical clock**, never wall-clock. This is what makes
scenarios reproducible and lets tests drive **mtime-window incrementals** deterministically.

- `now()` reads the clock; it **does not advance** — reads are not ticks.
- `setTime(t)` is the *only* thing that moves the clock. The script owns the timeline (it may
  even move backward — the store does not enforce monotonicity). Persisted with the store
  (in `meta`), so a reopened store resumes its timeline.
- A newly created node stamps `atime = mtime = ctime = now()`. Nodes born at the same logical
  instant share a stamp — creation order does not implicitly advance time.
- Every other time change is **explicit**: attribute setters (`node.mtime(v)`, …) touch only
  the field named, and `link`/`unlink` never auto-touch times (a relink/rename is invisible
  to mtime-based backups by design — that is what `diffPaths` is for, §6.1).

Typical scenario: `setTime(monday); …create files…; snapshot(); setTime(tuesday); …touch some
files at now()…; snapshot()` — then `diff` over that mtime window is exactly reproducible.

### 3.1 Link-set versions (per node)

A node's link sets are versioned as a whole, and the version lives in the node's
attributes:

- `dnLinkVer` — the `snapId` at which this node's **down-link set** was last (re)written.
- `upLinkVer` — the `snapId` at which this node's **up-link set** was last (re)written.

Down/up link records are keyed by that link-set version, not by a per-link snapId:

```
downlinks:  containerID(8) ‖ dnLinkVer(8) ‖ stringID(8)  →  { childNodeID(8) }
uplinks:    nodeID(8)      ‖ upLinkVer(8) ‖ containerID(8) ‖ stringID(8)  →  {}
```

So the set stored under one `(container, dnLinkVer)` prefix *is exactly* the live
directory contents at that version — no tombstones, no per-name version search.

### 3.2 Roots and snapshot access

- **RW root** = `(rootNodeID, LATEST)` — resolves at `cur_snap`, mutable.
- **RO root for snapshot N** = `(rootNodeID, N)` — the same root node, frozen, read-only.

There are **no stored `.snapshot` links**. Instead `.snapshot` is a **virtual directory
synthesized in every directory** (WAFL-style): in any directory `D` viewed live,
`D/.snapshot/N` resolves to `(D.nodeID, N)` — the same node, viewed at snapshot `N`. The
same subtree is thus reachable at any snapshot, in place, at zero storage cost.

Rules that make it behave (and not explode a tree walk):

1. `D/.snapshot/N` → `(D.nodeID, N)`, listing only snapshots **≥ `D`'s creation snapId**
   (`D` did not exist before).
2. **Live view only** — `.snapshot` is synthesized only when the handle is at `LATEST`; it
   never appears inside an already-frozen view. This is what prevents
   `/.snapshot/5/.snapshot/3/…` recursion.
3. **Hidden from `readdir` by default** — resolvable by name but not listed, so a
   tree-walking archiver doesn't recurse into every snapshot. A config flag exposes it when
   you specifically want to test snapshot-aware backup.

The reserved `.snapshot` name is owned by the **store**, so Lua tests and the NFS front-end
behave identically. `link`/`move` reject the name (`INVAL`) so a real entry can never shadow
the synthesized one. Because it is synthesized, `snapshot()` writes **no** link — it is just
*seal + `cur_snap++`* plus one small metadata record (§6).

**Identity (filehandle round-trip).** The `.snapshot` dir of node `D` has the concrete id
`D.nodeID | (1<<63)` — a real id in a reserved top-bit id-space (node ids come from a small
counter, so the bit is always free). It is fully `GETATTR`-able: a read-only (`0555`)
directory, `nlink 1`, timestamps mirroring `D`. `lookup(D,".snapshot")` returns that handle;
`readdir` of it lists `"N"` per snapshot `D` existed at; `lookup(snapDir,"N")` returns
`(D.nodeID, N)`. So both the list dir and its entries round-trip an NFS filehandle. Both
engines implement this identically.

**Snapshot metadata.** A `snaps: snapId → { ctime, label }` table records, for each sealed
snapshot, the logical time it was taken (`ctime = now()`, §3.5) and an optional caller
`label`. `snapshot("monday")` writes it; `snapInfo(id)` reads it back (an unknown id yields
`ctime = 0`, empty label). This is the per-snapshot anchor that mtime-window incrementals and
the synthesized `.snapshot` listing (name + mtime per entry) build on. Persisted like the
other store meta, so it survives reopen.

**View inheritance (the traversal rule).** A child normally inherits its parent's snapshot
view; the *only* place the view changes is crossing a synthesized `.snapshot/N`. This is
why stored links need no per-link `childSnapId`:

```
resolve(parentHandle, name):
    if parentHandle.snapId == LATEST and name == ".snapshot":
        return virtual snapshot-list bound to parentHandle.nodeID   # entries "N" ≥ node's birth
    if parentHandle is a snapshot-list for node K and name == "N":
        return (K, N)                                               # the only view switch
    child = lookup(parentHandle.nodeID, name, parentHandle.snapId)
    return (child, parentHandle.snapId)                             # normal link: inherit view
```

### 3.3 Copy-on-write of link sets (default)

We version link **sets**, not individual links. The first time a node's link set is
modified in a new snapshot, the whole set is copied to a fresh version; subsequent changes
within the same snapshot are applied in place. Deletion is just absence from the new set —
there are **no tombstone records**.

```
ensureWritableDnLinks(dir):
    if dir.dnLinkVer == cur_snap:            # already writable this snapshot
        return dir.dnLinkVer
    dv = cur_snap
    copy  (dir ‖ dir.dnLinkVer ‖ *)  →  (dir ‖ dv ‖ *)     # copy the whole set once
    dir.dnLinkVer = dv                       # (persisted in the dir's cur_snap node record)
    return dv
```

`ensureWritableUpLinks(node)` is identical on `upLinkVer`. `link`/`unlink` call these,
then add / omit the single entry. Reads never see partial or deleted state because a
sealed snapshot's set is immutable — only the current snapshot's copy is mutated.

**Why this is the default.** It makes the hot path — `readdir`/`lookup` — a single
contiguous scan of exactly the live entries, with trivial read logic and no tombstone
accumulation. Crucially, the copy happens **only on a post-snapshot modification**: a
directory built once and then only read is *never* copied, regardless of size. The cost
appears only for directories that are both **large and repeatedly mutated across
snapshots** — handled by §3.4.

### 3.4 Hybrid link-set mode (optional, per directory)

"Sometimes copy-on-write, sometimes don't." A directory that is large *and* mutated a
little across many snapshots would, under pure COW, re-copy its whole set each snapshot
(e.g. a huge maildir gaining a few files per backup cycle). Such a directory may instead
use **LOG mode** — per-link versioning with tombstones:

```
downlinks(LOG):  containerID(8) ‖ stringID(8) ‖ snapId(8)
                 → { flags(live|deleted), childNodeID(8) }

    change:  append one record stamped cur_snap                     # O(1), no set copy
    delete:  hard-delete if born this snapshot, else append {deleted}
    readdir: scan container prefix, group by stringID, take the effective (≤ G) record,
             skip deleted
```

A directory's mode is a flag in its node attrs, chosen by policy: a **fan-out threshold**
(auto → LOG above N entries) and/or an explicit hint from the generator (which usually
knows a directory's intended size). Mode is fixed at creation in v1; dynamic switching is
future work.

Cost: two read paths and two write paths for links (COW vs LOG) — the price for keeping the
common case (small dirs) simple and the rare large-churny case cheap to write.

**Single-model alternative — bucketed COW.** Instead of two modes, *always* COW but
partition a directory's entries into buckets by `hash(stringID)` and copy only the touched
bucket (`containerID ‖ bucketNo ‖ bucketVer ‖ stringID`). Copy cost is bounded to one
bucket regardless of fan-out, with one read/write model and no tombstones; the cost is a
bucket dimension in the keys and per-bucket versioning. Bucket count is 1 for ordinary dirs
(identical to §3.3) and larger for dirs you build big. See §11 for the open decision.

---

## 4. Storage engine: LMDB

An ordered, embedded, memory-mapped B+tree store. Chosen because the workload is
**read-mostly and prefix-scan-dominant**, which is exactly what a B-tree is best at:

- Prefix scans (readdir, version history, diffs) are a tree descent + a contiguous leaf
  walk — no bloom filters or prefix-extractor tuning.
- Zero-copy reads (values are pointers into the mmap).
- Atomic read-modify-write is inherent: a whole mutation batch runs in one single-writer
  transaction; no CAS, no merge operators, no conflict handling.
- Crash-proof by design (copy-on-write + double meta pages) — no WAL, no recovery step.
- Native duplicate keys (`MDB_DUPSORT`) for collision-safe interning.
- Tiny dependency (~2 C files), instant build, almost no tuning surface.

**Design constraints to respect:** a single concurrent writer (fine — mutations are
batched); a preset max map size (set generously, sparse); values are mmap pointers (copy
out before the txn ends); keep read transactions short.

---

## 5. On-disk schema (LMDB sub-databases)

| Sub-DB     | Key                                                       | Value                                                                       | Flags     |
|------------|-----------------------------------------------------------|-----------------------------------------------------------------------------|-----------|
| `meta`     | `"next_node"`/`"next_str"`/`"next_content"`/`"cur_snap"`/`"root"`/`"clock"` | scalar                                                 | —         |
| `nodes`    | `nodeID(8) ‖ snapId(8)`                                    | `{type, mode, nlink, uid, gid, size, atime, mtime, ctime, spec(8), dnLinkVer(8), upLinkVer(8)}` `[linkMode — reserved]` | —         |
| `downlinks`| `containerID(8) ‖ dnLinkVer(8) ‖ stringID(8)`            | `{childNodeID(8)}`                                                          | —         |
| `uplinks`  | `nodeID(8) ‖ upLinkVer(8) ‖ containerID(8) ‖ stringID(8)`| `{}`                                                                        | —         |
| `strings`  | `stringID(8)`                                             | `{snapId(8), bytes}`                                                         | —         |
| `strhash`  | `hash(8)` → dup `stringID(8)`                             | —                                                                           | `DUPSORT` |
| `content`  | `contentID(8)`                                           | `{snapId(8), block-structure bytes}` (opaque)                               | —         |
| `conhash`  | `hash(8)` → dup `contentID(8)`                            | —                                                                           | `DUPSORT` |
| `changes`  | `snapId(8) ‖ nodeID(8)`                                  | `hint(1)` — candidate index for `diff` (§6.1)                               | —         |
| `snaps`    | `snapId(8)`                                              | `{ctime(8), label bytes}` — per-snapshot metadata (§3.2)                    | —         |
| `stats`    | `snapId(8)`                                              | `{nodes[7], links, totalSize, …}` (§9)                                      | —         |

Notes:

- **No tombstones in the default (COW) mode.** A directory's live contents == the set under
  its effective `(container, dnLinkVer)` prefix. LOG-mode dirs (§3.4) instead key entries
  by `container ‖ stringID ‖ snapId` with a live/deleted flag — the `linkMode` attr says
  which representation a directory uses.
- **`linkMode` is reserved, not yet stored.** The COW-vs-LOG-vs-bucketed hybrid (§3.4/§11)
  is undecided, so the current implementation is **pure COW** and `NodeRec` carries no
  `linkMode` field. The name is held in the schema for the eventual per-directory strategy;
  until that decision lands, treat directories as COW-only. Adding it later is a record-format
  bump behind the existing versioning, not a redesign.
- `nodes.spec` is polymorphic by `type` (§2.1): `stringID` (`LNK`), packed device number
  (`BLK`/`CHR`), `contentID` (`REG`), or `0`.
- `strings` and `content` are twin interned tables — immutable, hash-deduplicated,
  referenced by a 64-bit id. Both use the same `intern` machinery (below).
- **`.snapshot` is synthesized, not stored** (§3.2): a virtual `.snapshot/N` in any live
  directory `D` resolves to `(D, N)`. That is the only view switch, so stored down links
  need no `childSnapId`.
- `changes` is only a **candidate index** for `diff` (§6.1): `snapId ‖ nodeID → hint`,
  upserted O(1) per node mutation; `diff` derives the precise change by state comparison
  (no full scan, no change-log replay).

### Interning (used by both `strings` and `content`)

```
intern(table, hashtab, bytes):
    h = hash(bytes)
    for id in hashtab[h]:                 # DUPSORT: usually one, more only on collision
        if table[id].bytes == bytes: return id
    id = meta.next_<table>++
    put table[id]   = { cur_snap, bytes }
    put hashtab[h]  = id                    # add as a dup
    return id
```

Hashing keys the reverse index (compact, fixed-size); reading the value back guards
against hash collisions so two distinct values never alias.

---

## 6. Operations

Reads take a snapshot `G` (from the handle) and range-back to the effective node record,
then read that node's link-set version / `spec` directly. Mutations run inside one write
transaction and are stamped with `cur_snap`.

```
getattr(node, G):
    seek nodes to (node ‖ G); if not exact step PREV
    if same nodeID → attrs (incl. spec, dnLinkVer, upLinkVer, linkMode)

lookup(container, name, G):                 # COW mode
    dv  = getattr(container, G).dnLinkVer
    sid = intern(strings, strhash, name)
    v   = get downlinks (container ‖ dv ‖ sid)          # single get
    if v → child = (v.childNodeID, G)                   # child inherits the view

readdir(container, G):                       # COW mode
    dv = getattr(container, G).dnLinkVer
    prefix-scan downlinks on (container ‖ dv)           # the set IS the live children
    # each hit: stringID from key, childNodeID from value; resolve name via strings[sid]
    # (LOG-mode dirs scan container prefix, group by stringID, take effective ≤ G, skip deleted)

parents(node, G):
    uv = getattr(node, G).upLinkVer
    prefix-scan uplinks on (node ‖ uv)

readlink(node, G):  return strings[getattr(node, G).spec].bytes          # LNK
content(node, G):   return content[getattr(node, G).spec].bytes          # REG (opaque)
rdev(node, G):      r = getattr(node, G).spec; return (r>>32, r & 0xffffffff)  # BLK/CHR

create(type) -> Node:
    id = meta.next_node++
    put nodes(id ‖ cur_snap) = attrs{type, dnLinkVer=cur_snap, upLinkVer=cur_snap, spec=0, …}
    return (id, LATEST)

link(container, name, child):                # dispatches on container.linkMode
    if child.type==DIR and reachable(from=child, to=container): return EINVAL   # §2.2 cycle check
    dv  = ensureWritableDnLinks(container)              # §3.3 (COW) or append (LOG)
    uv  = ensureWritableUpLinks(child)
    sid = intern(strings, strhash, name)
    put downlinks(...) = { child.nodeID }
    put uplinks  (...) = {}
    child.nlink++                                       # new child node record at cur_snap
    touch container mtime
    record change

unlink(container, name):                     # COW: omit from new set; LOG: tombstone
    ...
    child.nlink--   (if 0 → node considered deleted)
    record change

move(srcDir, sn, dstDir, dn):  link(dst) + unlink(src) in one txn   # link enforces the §2.2 cycle check

snapshot() -> SnapId:
    Vn = cur_snap                    # seal the current view
    cur_snap = Vn + 1                # ...that is all — .snapshot/N is synthesized (§3.2)
    return Vn

diffNodes(V):  ≡ diffNodes(V-1, V)        # snapshot-comparison oracle — see §6.1
```

**Traversal** (`resolve`) composes `lookup` with the view-inheritance rule and the
synthesized `.snapshot/N` view switch — defined in §3.2. Regular links always inherit the
parent's view; RO roots and per-directory snapshot access need no stored special-casing.

### 6.1 Diff — the snapshot-comparison oracle

`diff` reports the *true* set of changes between two snapshots — the oracle an incremental
archiver's own detection is validated against (à la ZFS `diff` / NetApp SnapDiff). It is a
**state comparison**, not a change-log replay, so multi-version churn coalesces correctly.
Both a node-level and a path-level form are supported; they answer different questions.

**Node-level: `diffNodes(A, B)`** — "which files/nodes changed" (content backup).

```
candidates = distinct nodeIDs written in snapIds (A, B]        # from the `changes` index
for each nodeID:
    sA = state @A (range-back ≤ A, may be ∅);  sB = state @B (range-back ≤ B, may be ∅)
    ∅,∃ → CREATED ;  ∃,∅ → REMOVED ;  ∃,∃ →
        MODIFIED(content)  if spec differs                  # re-copy data
        MODIFIED(attrs)    if only mode/uid/mtime differ    # metadata only
```

`diffNodes(V)` ≡ `diffNodes(V-1, V)`. Cost is **O(churn)**, not tree size.

**Path-level (on demand): `diffPaths(A, B)`** — "which paths appeared/disappeared"
(namespace-aware tools: `rsync --delete`, ZFS `diff`). For each changed **directory** among
the candidates, set-diff its down-link set @A vs @B → `ADDED(parent,name,child)` /
`REMOVED(parent,name)`. Derived on demand (O(changed-dir size)); no path records are stored.

**`changes` is only a candidate index** — `snapId ‖ nodeID → hint`, upserted O(1) per node
mutation. It bounds the candidate set; the *precise* change comes from the comparison, which
also coalesces (created-then-deleted in the window → nothing). This settles the index-vs-scan
question as a hybrid: index for candidates, comparison for truth.

**Notable properties.** A `move` keeps the `nodeID`, so `diff` reports it as one node whose
*path changed but content didn't* — a real **rename**, which mtime-based backups cannot see.
And since range-back yields per-snapshot `mtime`, `diff` can also be expressed as an **mtime
window** to test mtime-based incrementals specifically.

### 6.2 Paginated readdir (stable cursor)

NFS `READDIR` is paginated and the live directory can change between calls, so the cursor must
survive concurrent mutation. `readdirPage(dir, after, max)` returns up to `max` entries whose
name is `> after` (empty `after` starts), plus the last name as the resume `cookie` and an
`eof` flag.

The cursor **is the entry name**, and entries are stored name-ordered — so it is inherently
stable, needing no NFS `cookieverf`:

- Every entry present for the whole scan is returned **exactly once**.
- An entry added **ahead** of the cursor (name `> cookie`) shows up in a later page; one added
  **behind** it (already passed) does not.
- An entry removed before the cursor reaches it is simply not returned; removing the cursor's
  own entry still resumes correctly (`seek` floors to the next name).
- `eof` is set as soon as the end is known, so a page that exactly consumes the rest reports
  `eof` in the same call — no wasted empty round-trip.

Frozen snapshot views are immutable, so pagination over them is trivially stable. The
synthesized `.snapshot` dir paginates by snapId (numeric cursor). Mapping the string cookie to
a 64-bit NFS cookie (for names that don't fit) is an NFS-front-end concern (L2).

---

## 7. Library interface (sketch)

Content-free: no `read`/`write` of bytes anywhere. `content()` returns the *opaque* block
structure for the provider to expand.

```cpp
enum class Type { REG, DIR, LNK, BLK, CHR, FIFO, SOCK };
using SnapId = uint64_t;

class INode {                                 // handle == shared_ptr<INode>, carries (id, snapId)
public:
    virtual uint64_t id()     const = 0;
    virtual SnapId   snapId() const = 0;
    virtual Type     type()   const = 0;   // immutable

    virtual uint32_t mode()  const = 0;  virtual void mode(uint32_t)  = 0;
    virtual uint32_t nlink() const = 0;
    virtual uint32_t uid()   const = 0;  virtual void uid(uint32_t)   = 0;
    virtual uint32_t gid()   const = 0;  virtual void gid(uint32_t)   = 0;
    virtual uint64_t size()  const = 0;  virtual void size(uint64_t)  = 0;
    virtual uint64_t atime() const = 0;  /* mtime, ctime … */

    // type-specific views over spec:
    virtual std::string           target()  const = 0;   // LNK  → interned string
    virtual std::pair<uint32_t,uint32_t> rdev() const = 0;// BLK/CHR → (major, minor)
    virtual std::string           content() const = 0;   // REG  → opaque block structure
};
using Node = std::shared_ptr<INode>;

class IPrfs {
public:
    // roots
    virtual Node                rwRoot()                 = 0;   // (root, LATEST)
    virtual Node                snapshotRoot(SnapId)     = 0;   // (root, N)
    virtual std::vector<SnapId> snapshots() const        = 0;

    // creation — typed verbs; a node is born valid, its (immutable) type fixed here
    virtual Node   mkdir  ()                                               = 0;   // DIR
    virtual Node   mkfile (std::string const& blockStruct)                = 0;   // REG  → interns content
    virtual Node   symlink(std::string const& target)                     = 0;   // LNK  → interns target
    virtual Node   mknod  (Type t, uint32_t major, uint32_t minor)        = 0;   // BLK / CHR
    virtual Node   mkfifo ()                                               = 0;   // FIFO
    virtual Node   mksock ()                                               = 0;   // SOCK

    // namespace
    virtual Node   lookup(Node dir, std::string const& name)              = 0;   // uses dir->snapId()
    virtual Error  link  (Node dir, std::string const& name, Node child)  = 0;
    virtual Error  unlink(Node dir, std::string const& name)              = 0;
    virtual Error  move  (Node sdir, std::string const& sn,
                          Node ddir, std::string const& dn)               = 0;

    // mutators — change an existing node (writes a new version); type-guarded, never change type
    virtual Error  setTarget (Node lnk, std::string const& target)        = 0;   // LNK only
    virtual Error  setRdev   (Node dev, uint32_t major, uint32_t minor)   = 0;   // BLK/CHR only
    virtual Error  setContent(Node reg, std::string const& blockStruct)   = 0;   // REG only

    // iteration
    virtual std::unique_ptr<IDirIterator>  readdir(Node dir)  = 0;   // (name, child) pairs
    virtual std::unique_ptr<ILinkIterator> parents(Node node) = 0;   // reverse links

    // versioning
    virtual SnapId                             snapshot()  = 0;
    virtual std::unique_ptr<INodeDiffIterator> diffNodes(SnapId a, SnapId b) = 0;  // CREATED/REMOVED/MODIFIED (§6.1)
    virtual std::unique_ptr<IPathDiffIterator> diffPaths(SnapId a, SnapId b) = 0;  // ADDED/REMOVED

    // statistics — O(1); defaults to the live view (§9). Stats is a fixed struct.
    virtual Stats                          stats(SnapId = LATEST) const = 0;
};

std::unique_ptr<IPrfs> openPrfs(std::string const& path, Options const&);
```

---

## 8. Concurrency

LMDB provides everything the store needs, so there is **no application-level lock** — no
recursive mutex, no read/write lock:

- **Writer serialization** — one write transaction per environment at a time; a second
  write `mdb_txn_begin` blocks until the first commits. All mutations are serialized by LMDB.
- **Reader/writer isolation** — MVCC: read txns see a consistent snapshot, never block the
  writer, and the writer never blocks them.

A whole mutation batch runs in one write transaction → atomic and isolated. The old
per-file striped/recursive `FileLocker` (which compensated for rocksdb's lack of
cross-operation isolation) is **obsolete** and removed.

Recommended threading model: funnel all mutations through a **single writer thread** (a
serialized write queue) — clean txn boundaries, zero locks in the rest of the code — while
readers run concurrently on their own read txns (per-thread, or `MDB_NOTLS` + a
reader-slot pool). Keep counters (`cur_snap`, `next_node`, …) in `meta` and mutate them
inside the write txn so they need no separate lock.

---

## 9. Statistics

Aggregate statistics are maintained **incrementally**, so they read in **O(1)** — no tree
walk. Every mutation already runs in one write transaction; it updates a small fixed counter
record in the same txn (O(1) extra, no contention under the single writer).

```
stats:  snapId(8) → { nodes[REG,DIR,LNK,BLK,CHR,FIFO,SOCK],
                      links, totalSize,
                      logicalSize, physicalSize,      # phase-2 (dedup ratio)
                      uniqueStrings, uniqueContent }

update (inside the mutation's txn, all O(1)):
    create(type):          nodes[type]++
    unlink → nlink==0:     nodes[type]-- ; if REG: totalSize -= size
    link / unlink:         links ±1
    setContent / resize:   totalSize += (new - old)

read:
    stats(G):  range-back stats to the largest snapId ≤ G     # live view = stats(cur_snap)
```

Both the **current** and any **historical** snapshot's stats are O(1). They map directly
onto NFS **`FSSTAT`/`FSINFO`**. `snapshot()` carries the counters forward under the new
snapId and otherwise writes nothing.

**FSSTAT / FSINFO mapping.** A synthetic target has no backing device, so *capacity* is a
policy (`FsConfig`), not a measurement; *usage* comes from `Stats`. The projection is a pure
function (`prfs::fsStat` / `prfs::fsInfo`, `include/prfs/fsstat.hpp`) — no storage, no XDR —
that the NFS front-end serializes. It is engine-independent (depends only on `Stats`).

```
FSSTAT3resok (dynamic, per snapshot)        source
  tbytes   total bytes                       max(FsConfig.capacityBytes, usedBytes)
  fbytes   free bytes                         tbytes − usedBytes
  abytes   available (no reservation)         == fbytes
  tfiles   total file slots                   max(FsConfig.capacityFiles, usedFiles)
  ffiles   free file slots                    tfiles − usedFiles
  afiles   available                          == ffiles
  invarsec invariant window                   0 (usage is volatile)
      usedFiles = Σ nodes[REG..SOCK]      usedBytes = totalSize, rounded up to blockSize
      capacity clamps up to usedX so free never underflows

FSINFO3resok (static server params)         source
  rt*/wt*/dtpref  transfer sizes             FsConfig (defaults 1 MiB rd/wr, 64 KiB readdir)
  maxfilesize                                FsConfig (default 2^63−1)
  time_delta                                 FsConfig (logical clock → {0s, 1ns})
  properties                                 LINK | SYMLINK | HOMOGENEOUS | CANSETTIME
```

`properties` advertises what the store actually supports: hard links (§2.2), symlinks,
homogeneous PATHCONF, and settable times (`SETATTR`). Reported through the Lua bindings as
`store:fsStat()` and `prfs.fsInfo()`.

**Dedup ratio (phase-2).** Because `content` is interned/hash-deduped, tracking
`logicalSize` (Σ file sizes) vs `physicalSize` (Σ unique content-entity sizes) yields a live
dedup ratio — exactly what an archive tester wants to observe. It requires **refcounting**
the interned content entities (adjust `physicalSize` on the 0↔1 transition); that same
refcount is what enables GC (§11). Refcounting under versioning has real subtlety
(live-refcount vs referenced-by-any-snapshot), so it is opt-in, not day-one.

**Not O(1): per-directory / subtree stats.** "`du` of `/a/b`" needs size aggregates
propagated up the tree per change (an augmented tree) — O(depth) per mutation, and messy
with hard links and versioning. Global stats are O(1); subtree stats are an O(subtree) scan
or a separate structure if ever needed.

---

## 10. Separation of concerns / composition

```
NFS front-end (v3/v4)      ──►  IPrfs  (this library: metadata, versioned)
        │                              │
        └──► content provider ◄────────┘   reads Node.content()/target()/rdev(), makes bytes
```

The store hands over `size()`, `type()`, and the type-specific `content()`/`target()`/
`rdev()`. A separate content provider produces the actual bytes on `READ`. Swapping that
provider for one that reads real blocks would turn this into a real file server without
touching the store.

---

## 11. Open questions

- **Hybrid link-set strategy (§3.4)** — do test scenarios repeatedly mutate *large*
  directories across snapshots? If not, pure COW (§3.3) is optimal and simplest. If yes,
  pick: **per-directory COW/LOG mode** (matches "sometimes copy, sometimes don't"; two code
  paths) or **bucketed COW** (one model, bounded copy). Decision pending.
- **Content layout** — is the `content` block structure a **procedural recipe**
  (seed + compress/dedup/sparse knobs) or an **explicit extent list**, or both? Format is
  the content provider's concern; the store only interns the bytes.
- **Block size** — per fs / folder / file? Leaning: a denormalized `blockSize` node
  attribute (feeds `st_blksize`/`st_blocks`), resolved at create time. Not yet in the schema.
- **GC** — with copy-on-write link sets, a whole `(container, dnLinkVer)` set becomes
  droppable at once when no surviving snapshot references it; likewise superseded
  `content`/`strings` entities. A compaction pass is the main mitigation for §3.3/§3.4
  amplification — future work.
- **Content provider contract** — the block-structure format and the block/pattern model
  (ZERO/ONE/RANDOM/SEEDED/DEDUP) live outside this library and need their own doc.

---

## 12. Scripting / test harness (planned)

Tests are driven from **Lua** scripts, not recompiled C++, so scenarios (build a tree, set
content, snapshot, mutate, assert a diff) iterate fast and can generate large trees in a
loop. The core stays lua-free:

```
libprfs      pure C++ store — NO lua dependency
   │
prfs-lua     bindings: exposes IPrfs / INode to Lua
   │
prfs-test    harness: embeds lua, opens a store, runs .lua scenarios + asserts
```

Binding library: **sol2** (leaning — clean `shared_ptr`/overload/property handling for
C++20) or LuaBridge3 (familiar). Lua 5.4 (or LuaJIT to generate very large trees fast).

---

## 13. Design principles (SOLID) & testing

> **Binding.** SOLID, test-first, and **clang-format** (`.clang-format`) are **required** and
> must be kept and followed for every change — new features, bug fixes, and refactors alike.
> Code that violates them is not "done." Formatting is enforced by the `clang-format` meson
> test (`meson compile format` rewrites in place). This is the standing quality bar for the
> project, not an aspiration.

### SOLID

- **Single responsibility.** Each module does exactly one job: `prfs` = versioned metadata
  only; the **content provider** = bytes; the **NFS front-end** = protocol; `prfs-lua` +
  `prfs-test` = scripting/tests. The store never generates content nor speaks NFS.
- **Open/closed.** Consumers use the `IPrfs`/`INode` abstractions; `openPrfs()` hides the
  LMDB backend, so an alternate backend (in-memory oracle, a different KV) drops in without
  touching callers. Content is an opaque blob → a new content model needs no store change.
  The link-set strategy (COW / LOG / bucketed) sits behind `linkMode`, extensibly.
- **Liskov.** Every `IPrfs` implementation must honour the same contract: range-back read
  semantics, snapshot immutability, `nlink == #uplinks`, sealed snapshots never change. The
  in-memory oracle and the LMDB backend are substitutable — differential testing enforces it.
- **Interface segregation.** Small, focused interfaces: `INode` (a handle) vs `IPrfs` (the
  store) vs the per-iterator interfaces. Type-specific behaviour is reached through narrow
  capability interfaces (`ISymlink`/`IRegular`/`IDevice`) rather than one fat node.
- **Dependency inversion.** The NFS front-end, the Lua harness, and the content provider all
  depend on `IPrfs`/`INode`, not on LMDB; the store depends on "opaque content bytes," not on
  any content model. Everything is wired through the `openPrfs` factory.

### Testing strategy

- **Unit (gtest).** create/link/lookup/readdir/unlink, symlink resolve, hard links, snapshot
  + range-back, `diff`, the COW delete rule, interning/dedup, O(1) `stats`, and the guard
  rails (single-parent-dir, move-cycle rejection).
- **Invariant / property tests.** After any op sequence: `readdir(D,G)` equals the set of live
  down links; `nlink == #uplinks`; a sealed snapshot's reads never change after later
  mutations; `stats(G)` equals a full-scan recomputation (cross-checks the O(1) counters).
- **Differential testing (highest value).** A tiny **in-memory reference model** of the FS is
  the oracle; drive both it and the LMDB store with randomized op sequences and assert
  identical observable state at every snapshot. Best test for a versioned store, and it
  doubles as the Liskov check.
- **Determinism.** A seeded scenario must yield a byte-identical DB — golden-image tests guard
  reproducibility.
- **Crash safety.** Kill mid-write, reopen, assert consistency (LMDB gives this by design; the
  test proves we don't undermine it).
- **Concurrency.** Many concurrent readers + the single writer thread; assert readers see a
  stable snapshot and never observe a torn mutation.
- **Lua scenarios (§12).** End-to-end: build a tree, snapshot, mutate, assert the `diff` and
  `stats` — the readable, fast-iterating layer.
- **Integration (front-end, separate).** Run real archivers (`tar`, `restic`, `borg`) and an
  NFS conformance suite (pynfs / Connectathon) against the mounted server.

### Test-first (TDD)

The interface-first design makes test-first natural, and we adopt it:

1. **Write the contract as tests before the backend.** The `IPrfs`/`INode` semantics
   (range-back reads, snapshot immutability, `nlink == #uplinks`, cycle rejection, `diff`
   results) become gtest cases first — red.
2. **Build the in-memory reference model first**, test-driven, as the executable spec and
   oracle. It is tiny and forces the contract to be precise.
3. **Then implement the LMDB backend to green**, running the same tests plus the differential
   harness that checks it against the oracle at every step.
4. Every fix in `bugs.md` and every new operation starts with a **failing test that pins the
   intended behaviour**, then the code that makes it pass.

This keeps the LMDB backend honest against a simpler oracle and turns the design's invariants
into executable checks.
