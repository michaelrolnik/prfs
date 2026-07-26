# prfs content provider — design

The **content provider** (`libprfs_content`) turns a regular file into bytes, on demand, at any
offset. It is the `READ` path of the synthetic NFS target. It is a **separate module**: the
metadata store (`libprfs`) never generates or stores content bytes.

The defining choice: **content bytes are never stored — they are generated on the fly** from a
small **config** plus the file's identity. The prfs DB holds only metadata and that config; a
file's data is a pure function of `(config, fileSeed, offset)`.

> **Development principles (binding, design §13):** SOLID, test-first (a failing test first,
> then code to green), clang-format enforced. Same rules as the core library.

---

## 1. Where the config lives (what the DB stores)

Configuration granularity, coarse to fine:

- **Filesystem (mandatory).** One `ContentConfig` for the whole store, held in `meta`
  (`"content_config"`). This is the base every file uses.
- **Folder (nice-to-have).** A directory may carry an override that applies to the subtree
  below it, inherited by descendants until another override. Stored as `folderContent: nodeID →
  ContentConfig` (a small table; phase-2, off by default).
- **File (none).** A file stores **no** content recipe. Its characteristics are *derived* from
  its node id (see §4). Per-file configuration is deliberately not a thing — it buys nothing for
  a synthetic target and would bloat every node.

So the DB stores: metadata (always) + one FS config + optional folder overrides. **Never block
data.** A `READ` regenerates bytes; a snapshot of a 10 TB tree costs kilobytes.

```
READ(node, offset, len):                         # the L2 front-end will do this
    cfg  = effectiveConfig(node)                  # folder override ↑ tree, else FS config
    seed = node.id()                              # the file's stable identity
    return content::read(cfg, seed, node.size(), offset, len)
```

(Literal small files still work: if a node has explicit `content()` bytes, the front-end serves
those verbatim and skips the generator. Procedural files — the common case — have none.)

---

## 2. Requirements

1. **Deterministic & reproducible.** `read` is a pure function of `(config, fileSeed, size,
   offset, len)` — same inputs, same bytes on every host and run. No wall-clock, no ambient
   randomness; defined endianness and wrapping arithmetic. `fileSeed = nodeID`, and node ids are
   allocated deterministically, so a rebuilt tree yields identical content.
2. **Random access.** The byte at `offset` is computable in `O(blockSize)` without generating
   the preceding bytes, so a tester can `READ` deep inside a multi-terabyte file cheaply.
   Reading `[0,N)` in one call equals the concatenation of any partition of that range.
3. **Size-authoritative (T7).** `size` (a node attribute) bounds output: a `read` past `size`
   returns a short count (EOF). The generator produces exactly `size` bytes of geometry.
4. **The knobs a backup/archive tool cares about, FS-wide:**
   - **entropy** — bits/byte; *compressibility is the derived `1 − entropy/8`*.
   - **dedup** — a global pool of `U` distinct blocks shared across the whole FS.
   - **sparseness** — a fraction of blocks that are holes (unallocated, read as zeros).

---

## 3. ContentConfig

```cpp
struct ContentConfig {
    uint32_t blockSize     = 4096;  // generation & dedup granularity (D3)
    uint8_t  entropy       = 255;   // 0..255 ⇒ 0..8 bits/byte (255 = incompressible)
    uint8_t  sparsePercent = 0;     // 0..100: share of a file's blocks that are holes
    uint64_t dedupPool     = 0;     // 0 = every block unique; N = N distinct blocks FS-wide
};

serialize:   magic("PCC1") ‖ blockSize:u32 ‖ entropy:u8 ‖ sparsePercent:u8 ‖ pad ‖ dedupPool:u64
```

`blockSize` is FS-level (a per-folder override is the only finer grain, §1) — never per-file.
It feeds `statfs f_bsize` and the FSINFO transfer granularity (design §9). `entropy` and
`dedupPool` are the compression- and dedup-ratio dials; `sparsePercent` the sparse dial.

---

## 4. Generation — derived per file, per block

Everything is **block-indexed**. For byte offset `o`, `block = o / blockSize`; the provider
generates the covering block(s) and copies out the requested slice. Blocks are independent, so
any range is reachable directly (req. 2). Per-file variety comes from mixing the `fileSeed` into
every derivation — no two files (different node ids) generate the same stream, yet each is
reproducible.

Mixer (SplitMix64 finalizer; all multiplies wrap mod 2⁶⁴):

```
mix64(x):  x ^= x>>30; x *= 0xbf58476d1ce4e5b9;
           x ^= x>>27; x *= 0x94d049bb133111eb;
           x ^= x>>31; return x
GOLDEN = 0x9E3779B97F4A7C15
h(a,b,tag) = mix64(mix64(a*GOLDEN + b) + tag)         # a portable coordinate hash
```

For file `S`, block `b`, config `C`:

1. **Sparse?** `isHole = h(S, b, HOLE_TAG) % 100 < C.sparsePercent`. If so the block is zeros and
   is **not** counted in `st_blocks` (§5); done.
2. **Pick the block's source seed:**
   - `C.dedupPool == 0`: `src = h(S, b, DATA_TAG)` — unique per (file, block).
   - else: `canon = h(S, b, DEDUP_TAG) % C.dedupPool; src = h(0, canon, POOL_TAG)` — the pool is
     keyed on `0` (not `S`), so identical `canon` across *different files* yields identical
     blocks → **cross-file dedup**, ratio ≈ totalBlocks / `dedupPool`.
3. **Fill at target entropy.** With `K = clamp(round(2^(8·entropy/255)), 1, 256)` equiprobable
   symbols: word `j` of the block is `mix64(src + j·GOLDEN)`; each output byte becomes
   `symbol = value % K` mapped to `symbol·255/(K−1)` (or `0` when `K==1`). The stream then
   carries `log2(K) ≈ 8·entropy/255` bits/byte, and a compressor reduces it to ≈ that ratio.
   `entropy=255 ⇒ K=256` incompressible; `entropy=0 ⇒ K=1` constant (fully compressible).

Each step is a pure function of `(S, b, C)` — satisfying reqs. 1–2.

---

## 5. Size authority & sparseness (T7)

`read(cfg, seed, size, offset, len)` produces `min(len, size − offset)` bytes (0 if
`offset ≥ size`) — `size` always wins over block geometry. `allocatedBlocks(cfg, seed, size)`
returns the 512-byte block count actually "allocated" (holes excluded), so the front-end can
answer `st_blocks` — letting a scenario test how a backup tool treats sparse vs zero-filled
files. Because sparseness is derived from `(seed, block)`, it is stable and reproducible per
file.

---

## 6. API

```cpp
namespace prfs::content {

struct ContentConfig { uint32_t blockSize = 4096; uint8_t entropy = 255;
                       uint8_t sparsePercent = 0; uint64_t dedupPool = 0; };

std::string   serialize(ContentConfig const&);
ContentConfig deserialize(std::string_view);       // throws on bad magic/format

//  Fill up to `len` bytes of the range at `offset`, bounded by `size`; returns
//  the count produced (a short count == EOF). Pure; no store, no engine.
size_t read(ContentConfig const&, uint64_t fileSeed, uint64_t size,
            uint64_t offset, char* out, size_t len);

//  st_blocks support: 512-byte blocks allocated (holes excluded).
uint64_t allocatedBlocks(ContentConfig const&, uint64_t fileSeed, uint64_t size);

} // namespace prfs::content
```

The module depends only on the standard library (a leaf — no `libprfs`, no engine). The
front-end and Lua harness pull it in alongside the store; the store supplies the effective
config and `fileSeed = node.id()`.

---

## 7. Testing (test-first)

Each behaviour gets a failing test before code (`tests/content/*`):

- **Determinism** — same `(config, seed, offset, len)` → identical bytes across calls; a
  different `seed` gives a different stream (files differ), same seed reproduces (rebuild-safe).
- **Random-access equivalence** — `[0,N)` in one call equals any partition of it (page-by-page,
  byte-by-byte, mis-aligned to `blockSize`).
- **Size/EOF** — reads clamp to `size`; `offset ≥ size` → 0.
- **Entropy** — measured byte-alphabet size / order-0 entropy tracks `entropy` monotonically;
  `entropy=255` uses ~all 256 values, `entropy=0` is constant.
- **Dedup** — with `dedupPool=U`, the number of distinct block contents across many files is
  ≤ `U`; two different files share blocks when `canon` collides; `dedupPool=0` → all unique.
- **Sparse** — ≈ `sparsePercent` of blocks are holes; holes read zero and are excluded from
  `allocatedBlocks`.
- **Config round-trip** — `deserialize(serialize(c)) == c`; bad magic / truncated is a hard error.

Integration cross-check: a scenario stores an FS config, creates files (metadata only), and
verifies `content::read(cfg, node.id(), node.size(), …)` — proving the store↔provider handoff
holds nothing but config + node id.

---

## 8. Build & bindings

- `libprfs_content` static library from `src/content/*.cpp` + `include/prfs/content.hpp`; added
  to the clang-format `fmt_files` gate; tests under `tests/content/` wired like the others.
- **Store integration:** an FS `ContentConfig` in `meta` (a config setter/getter on `IPrfs`);
  the per-folder override table is phase-2.
- **Lua** (`prfs.content`): build/serialize a config, and `content.read(cfg, seed, size, off,
  len)` so scenarios can assert the bytes a `READ` would return.
- Gated by a build option (`-Dcontent`, default on) mirroring `-Dlua`.

---

## 9. Open questions

- **Entropy realism.** An i.i.d. reduced alphabet gives a clean order-0 entropy target; an LZ
  compressor may do slightly better/worse depending on incidental repeats. Good enough for a
  ratio dial; revisit only if a scenario needs a precise post-compression size.
- **Per-folder overrides.** Specified but phase-2 — start FS-wide only. The inheritance walk (up
  to the nearest override) is the only added read cost when enabled.
- **File-size distribution.** How *big* generated files are (and the tree shape) is a *generator*
  concern (who calls `mkfile`/`setSize`), not the content provider's — likely a separate
  scenario-builder helper, out of scope here.
