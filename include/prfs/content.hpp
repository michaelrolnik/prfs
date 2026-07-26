// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

#pragma once
//
//  prfs content provider (docs/content.md, todo L1). Turns a regular file into
//  bytes on demand, at any offset — the READ path of the synthetic target.
//
//  Content is NEVER stored: a file's bytes are a pure function of
//  (ContentConfig, fileSeed = nodeID, offset). A leaf module — standard library
//  plus the rng module (prfs/rng.hpp) for the byte fill; no libprfs / engine.
//
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace prfs::content {

//  Filesystem-level content policy (design §11.2). Held in the store's meta; a
//  per-folder override is a phase-2 refinement. No per-file config. The byte-fill
//  generator is a separate run-wide choice — see prfs/rng.hpp.
struct ContentConfig {
    uint32_t blockSize = 4096;    // generation & dedup granularity (D3)
    uint8_t entropy = 255;        // 0..255 ⇒ 0..8 bits/byte (255 = incompressible)
    uint8_t sparsePercent = 0;    // 0..100: share of a file's blocks that are holes
    uint8_t dedupPercent = 0;     // 0..100: share of blocks that are duplicates
    uint32_t dedupCorpus = 65536; // distinct shared blocks the duplicates draw from

    bool operator==(ContentConfig const&) const = default;
};

std::string serialize(ContentConfig const&);
ContentConfig deserialize(std::string_view); // throws std::runtime_error on bad magic/format

//  Fill up to `len` bytes of the range at `offset`, bounded by `size`; returns
//  the count produced (a short count == EOF). Uses the active rng (rng::active()).
size_t read(ContentConfig const&, uint64_t fileSeed, uint64_t size, uint64_t offset, char* out,
            size_t len);

//  st_blocks support: 512-byte blocks actually allocated (holes excluded).
uint64_t allocatedBlocks(ContentConfig const&, uint64_t fileSeed, uint64_t size);

} // namespace prfs::content
