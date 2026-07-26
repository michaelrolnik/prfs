# prfs content provider — design

The **content provider** (`libprfs_content`) turns a regular file's *block structure* (its
recipe) into bytes, on demand, at any offset. It is the `READ` path of the synthetic NFS
target. It is a **separate module**: the metadata store (`libprfs`) never parses content — it
only interns the opaque recipe bytes under a `contentID` (design §2.1). This doc specifies the
recipe format, the deterministic generator, and the module's API.

> **Development principles (binding, design §13):** SOLID, test-first (a failing test first,
> then code to green), clang-format enforced. Same rules as the core library.

---

## 1. Scope

- **In scope:** a recipe format; a pure, deterministic, random-access byte generator
  (`read(recipe, size, offset, len)`); patterns (hole / zero / fill / random / dedup); a
  recipe builder + (de)serializer; Lua bindings for scenarios.
- **Out of scope:** the metadata namespace (that is `libprfs`), the NFS wire protocol (that is
  L2), and any persistence — the provider is stateless and holds no store.

The provider consumes what the store already exposes per regular file: `node.content()` (the
serialized recipe, the interned block structure) and `node.size()` (authoritative length, T7).

```
READ(node, offset, len):                        # the L2 front-end will do this
    recipe = content::deserialize(node.content())
    return content::read(recipe, node.size(), offset, len)
```

---

## 2. Requirements

1. **Deterministic & reproducible.** `read` is a pure function of `(recipe, size, offset, len)`.
   The same inputs yield the same bytes on every host and every run — no wall-clock, no
   `Math.random`, defined endianness and wrapping arithmetic. This is what makes a whole test
   scenario reproducible.
2. **Random access.** Byte at `offset` is computable in `O(blockSize)` without generating the
   preceding bytes, so a tester can `READ` a range deep inside a multi-terabyte file cheaply.
   Reading `[0,N)` in one call must equal the concatenation of any partition of that range.
3. **Size-authoritative (T7).** `size` bounds output: a `read` past `size` returns a short
   count (EOF); the recipe's extent lengths are advisory geometry, `size` is the truth.
4. **Controllable for backup testing.** The knobs a backup/archive tool cares about are
   first-class: **compressibility** (entropy ratio), **dedup** (unique-block ratio), and
   **sparseness** (holes). These let a scenario target a specific dedup ratio or compression
   ratio and observe how the tool behaves.

---

## 3. The block structure (recipe)

Per design §11.2, a recipe is an **ordered list of extents**; each extent is a **procedural
recipe** covering a contiguous byte range. The whole-file-from-one-seed case is a single
extent. Sparse files interleave `HOLE` extents. Block-level dedup uses the `DEDUP` pattern.

### 3.1 Patterns

| Pattern  | Bytes produced                                             | Params            |
| -------- | ---------------------------------------------------------- | ----------------- |
| `HOLE`   | zeros; **sparse** — not counted in `st_blocks`             | —                 |
| `ZERO`   | zeros; allocated (counted in `st_blocks`)                  | —                 |
| `FILL`   | a constant byte repeated (`ONE` = `FILL 0xFF`)             | `byte`            |
| `RANDOM` | seeded pseudo-random, `compressibility`-controlled entropy | `seed`, `compress`|
| `DEDUP`  | blocks drawn from a pool of `U` distinct random blocks     | `seed`, `unique`  |

`SEEDED` in earlier notes is just `RANDOM` with an explicit `seed`. `compress ∈ 0..255` is the
approximate compressible fraction of each block (`0` = incompressible, `255` ≈ all filler).
`unique = U` is the number of distinct blocks a `DEDUP` extent cycles through, so its dedup
ratio ≈ `blocks / U`.

### 3.2 Extent record

```
Extent = { pattern:u8, length:u64, seed:u64, param0:u32, param1:u32 }
    length          bytes this extent covers (advisory; size wins, req. 3)
    seed            base seed for RANDOM/DEDUP
    param0          FILL: byte; RANDOM: compress(0..255); DEDUP: compress
    param1          DEDUP: unique-block count U
```

### 3.3 Serialization

The interned `content` blob the store holds is:

```
recipe := magic("PRC1") ‖ blockSize:u32 ‖ extentCount:u32 ‖ Extent*   # all fields little-endian
```

`blockSize` is the per-file generation/dedup granularity (D3): resolved from the fs default at
create, overridable per file. `deserialize` validates the magic and returns `{blockSize,
extents}`; malformed input is a hard error (the store never hands out non-recipe bytes).

Because the blob is hash-deduplicated by the store, two files with an identical recipe share
one `contentID` — file-level dedup — while `DEDUP` extents give block-level dedup *within* a
file. The two compose (design §9 `logicalSize` vs `physicalSize`).

---

## 4. Deterministic random-access generation

Generation is **block-indexed**. For a byte at file offset `o`, `block = o / blockSize`; the
provider generates the covering block(s) and copies out the requested slice. Blocks are
independent, so any range is reachable directly (req. 2).

### 4.1 Per-block bytes

A block is filled with 64-bit words from a **counter-based mixer** (random-access by
construction — no streaming state). With the SplitMix64 finalizer as the mixer:

```
mix64(x):  x ^= x>>30; x *= 0xbf58476d1ce4e5b9;
           x ^= x>>27; x *= 0x94d049bb133111eb;
           x ^= x>>31; return x            # all multiplies wrap mod 2^64

GOLDEN = 0x9E3779B97F4A7C15
blockKey(seed, blockIndex) = mix64(seed ^ (blockIndex * GOLDEN))
word(seed, blockIndex, j)  = mix64(blockKey(seed, blockIndex) + j * GOLDEN)
```

Word `j` of the block is `word(...)` serialized **little-endian**; the block is the first
`blockSize` bytes of `word 0, word 1, …`. This is not cryptographic — it is a fast, portable,
well-distributed synthetic stream. (A counter RNG like Philox is a drop-in upgrade if a
scenario ever needs statistical-test-grade output; the interface does not change.)

### 4.2 Pattern application, per block

- `HOLE` / `ZERO`: the block is zeros (they differ only in `st_blocks`, §5).
- `FILL`: the block is `param0` repeated.
- `RANDOM`: the first `floor(blockSize * compress/255)` bytes are `0x00` (compressible filler),
  the rest are the `word()` stream. `compress = 0` ⇒ fully incompressible.
- `DEDUP`: the block's content is the `RANDOM` content of **canonical block** `blockIndex % U`
  — so the extent cycles through `U` distinct blocks. A deduplicating tool collapses it to `U`.

Every case is a pure function of `(seed, blockIndex)` and the params — satisfying reqs. 1–2.

---

## 5. Size authority & sparseness (T7)

`read(recipe, size, offset, len)` clamps to `size`: it produces `min(len, size - offset)`
bytes (0 if `offset >= size`). Extent `length`s lay out geometry; if they sum below `size` the
tail reads as `ZERO`, if above they are truncated — `size` always wins.

Sparseness is reported, not just read: a `HOLE` extent's blocks are **not** counted in
`st_blocks` (they read as zeros but occupy no "space"), while `ZERO` blocks are. The provider
exposes `allocatedBlocks(recipe, size)` so the front-end can answer `st_blocks` — letting a
scenario test how a backup tool handles sparse files vs zero-filled ones.

---

## 6. API

```cpp
namespace prfs::content {

enum class Pattern : uint8_t { HOLE, ZERO, FILL, RANDOM, DEDUP };

struct Extent {
    Pattern  pattern = Pattern::ZERO;
    uint64_t length  = 0;
    uint64_t seed    = 0;
    uint32_t param0  = 0;   // FILL: byte; RANDOM/DEDUP: compress
    uint32_t param1  = 0;   // DEDUP: unique-block count
};

struct Recipe {
    uint32_t            blockSize = 4096;
    std::vector<Extent> extents;
};

std::string serialize(Recipe const&);              // → the interned content blob
Recipe      deserialize(std::string_view);         // throws on bad magic/format

//  Fill up to `len` bytes of the range at `offset`, bounded by `size`.
//  Returns the number of bytes actually produced (a short count == EOF).
size_t read(Recipe const&, uint64_t size, uint64_t offset, char* out, size_t len);

//  st_blocks support: 512-byte blocks actually "allocated" (HOLE excluded).
uint64_t allocatedBlocks(Recipe const&, uint64_t size);

} // namespace prfs::content
```

The module depends only on the standard library (no `libprfs`, no engine) — it is a leaf. The
front-end and Lua harness pull it in alongside the store.

---

## 7. Testing (test-first)

Each behaviour gets a failing test before code. Planned `tests/content/*`:

- **Determinism** — same `(recipe, offset, len)` yields identical bytes across two calls and
  two freshly-built recipes; no hidden state.
- **Random-access equivalence** — reading `[0,N)` in one call equals the concatenation of an
  arbitrary partition of `[0,N)` (page-by-page, byte-by-byte, mis-aligned to `blockSize`).
- **Size/EOF** — reads clamp to `size`; `offset >= size` yields 0; extents under/over `size`
  behave per §5.
- **Patterns** — `ZERO`/`HOLE` all-zero; `FILL` constant; `RANDOM` `compress` shifts the
  zero-fraction monotonically; two identical `RANDOM` extents match, different seeds differ.
- **Dedup** — a `DEDUP U` extent contains exactly `U` distinct block contents; block `b`
  equals block `b + U`; `allocatedBlocks`/dedup ratio track `U`.
- **Sparse** — `HOLE` reads zeros but is excluded from `allocatedBlocks`; `ZERO` is included.
- **Serialize round-trip** — `deserialize(serialize(r)) == r`; bad magic / truncated blob is a
  hard error.

A cross-check against the store: a scenario that `mkfile(serialize(recipe))` + `setSize(size)`,
then reads via `content::read(deserialize(node.content()), node.size(), …)`, proving the
store↔provider handoff.

---

## 8. Build & bindings

- `libprfs_content` static library from `src/content/*.cpp` + `include/prfs/content.hpp`; added
  to the clang-format `fmt_files` gate; tests under `tests/content/` wired like the others.
- **Lua** (`prfs.content`): a recipe builder (`content.recipe{blockSize=, ...extents}`),
  `content.serialize/deserialize`, and `content.read(recipe, size, off, len)` so scenarios can
  attach content to a node and assert the bytes a `READ` would return.
- Gated by a build option (`-Dcontent`, default on) mirroring `-Dlua`, so the core can build
  standalone.

---

## 9. Open questions

- **Compressibility model.** The per-block "zero-prefix" gives an approximate, monotonic ratio;
  a scenario wanting a precise post-compression size may need an entropy-shaping model instead.
  Start approximate; revisit if a test needs exactness.
- **Cross-file dedup pools.** `DEDUP` dedups *within* a file. Sharing a block pool *across*
  files (a global dedup corpus) would need a pool id in the recipe — deferred until a
  multi-file dedup scenario asks for it.
- **`allocatedBlocks` vs the store's `totalSize`.** The store's O(1) `totalSize` (§9) is
  logical (`Σ size`); physical/allocated accounting (holes, dedup) is the provider's, feeding
  the phase-2 `logicalSize`/`physicalSize` dedup ratio.
