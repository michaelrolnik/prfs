// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  prfs content provider — deterministic, random-access byte generation
//  (docs/content.md). Block-indexed: each block is classified (hole / dup /
//  unique) from a coordinate hash of (fileSeed, blockIndex), then filled by the
//  active counter-based rng (prfs/rng.hpp) at a target entropy. Pure functions
//  of (config, fileSeed, blockIndex) — any range recomputes the same bytes
//  without touching its predecessors.
//
#include "prfs/content.hpp"

#include "prfs/di.hpp"
#include "prfs/rng.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace prfs::content {

static constexpr uint64_t GOLDEN = 0x9E3779B97F4A7C15ull;
static constexpr uint64_t HOLE_TAG = 1, DUP_TAG = 2, DUP2_TAG = 3, DATA_TAG = 4, CORPUS_TAG = 5;

static uint64_t mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

//  Portable coordinate hash — used for the block-type DECISIONS only (bucketing),
//  never for the content bytes.
static uint64_t h(uint64_t a, uint64_t b, uint64_t tag) {
    return mix64(mix64(a * GOLDEN + b) + tag);
}

//  Equiprobable alphabet size for a target entropy (0..255 ⇒ 0..8 bits/byte).
static int alphabet(uint8_t entropy) {
    long k = std::lround(std::pow(2.0, 8.0 * double(entropy) / 255.0));
    return int(std::clamp<long>(k, 1, 256));
}

static uint8_t mapByte(uint8_t v, int K) {
    if (K <= 1) {
        return 0;
    }
    return uint8_t((v % K) * 255 / (K - 1));
}

static bool isHole(ContentConfig const& c, uint64_t seed, uint64_t b) {
    return c.sparsePercent > 0 && h(seed, b, HOLE_TAG) % 100 < c.sparsePercent;
}

//  Source seed for a non-hole block: a shared FS-wide corpus entry (dedup) or a
//  per-(file,block) unique value.
static uint64_t blockSrc(ContentConfig const& c, uint64_t seed, uint64_t b) {
    if (c.dedupPercent > 0 && c.dedupCorpus > 0 && h(seed, b, DUP_TAG) % 100 < c.dedupPercent) {
        uint64_t canon = h(seed, b, DUP2_TAG) % c.dedupCorpus;
        return h(0, canon, CORPUS_TAG); // keyed on the global 0 ⇒ cross-file dedup
    }
    return h(seed, b, DATA_TAG);
}

static void genBlock(ContentConfig const& c, uint64_t seed, uint64_t b, int K, rng::IRng const& gen,
                     char* buf) {
    uint32_t bs = c.blockSize;
    if (isHole(c, seed, b)) {
        std::memset(buf, 0, bs);
        return;
    }

    uint64_t src = blockSrc(c, seed, b);
    uint32_t key[2] = {uint32_t(src), uint32_t(src >> 32)};
    uint32_t bl = uint32_t(b), bh = uint32_t(b >> 32);

    for (uint32_t off = 0, g = 0; off < bs; off += 16, ++g) {
        uint32_t ctr[4] = {bl, bh, g, 0};
        uint32_t out[4];
        gen.generate(ctr, key, out);
        for (uint32_t i = 0; i < 16 && off + i < bs; ++i) {
            uint8_t raw = uint8_t(out[i / 4] >> (8 * (i % 4)));
            buf[off + i] = char(mapByte(raw, K));
        }
    }
}

std::string serialize(ContentConfig const& c) {
    std::string s;
    s.reserve(16);
    s.append("PCC1", 4);
    auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            s.push_back(char(v >> (8 * i)));
        }
    };
    u32(c.blockSize);
    s.push_back(char(c.entropy));
    s.push_back(char(c.sparsePercent));
    s.push_back(char(c.dedupPercent));
    s.push_back(char(0)); // reserved
    u32(c.dedupCorpus);
    return s;
}

ContentConfig deserialize(std::string_view v) {
    if (v.size() < 16 || v.substr(0, 4) != "PCC1") {
        throw std::runtime_error("content: bad config blob");
    }
    auto u32 = [&](size_t off) {
        uint32_t x = 0;
        for (int i = 0; i < 4; ++i) {
            x |= uint32_t(uint8_t(v[off + i])) << (8 * i);
        }
        return x;
    };
    ContentConfig c;
    c.blockSize = u32(4);
    c.entropy = uint8_t(v[8]);
    c.sparsePercent = uint8_t(v[9]);
    c.dedupPercent = uint8_t(v[10]);
    c.dedupCorpus = u32(12);
    return c;
}

size_t read(ContentConfig const& c, uint64_t seed, uint64_t size, uint64_t offset, char* out,
            size_t len) {
    if (offset >= size || len == 0) {
        return 0;
    }
    uint64_t end = std::min<uint64_t>(offset + len, size);
    uint32_t bs = c.blockSize ? c.blockSize : 1;
    int K = alphabet(c.entropy);
    rng::IRng const& gen = rng::activeRng();
    std::vector<char> block(bs);

    for (uint64_t b = offset / bs, last = (end - 1) / bs; b <= last; ++b) {
        genBlock(c, seed, b, K, gen, block.data());
        uint64_t bstart = b * bs;
        uint64_t s = std::max(offset, bstart);
        uint64_t e = std::min(end, bstart + bs);
        std::memcpy(out + (s - offset), block.data() + (s - bstart), size_t(e - s));
    }
    return size_t(end - offset);
}

uint64_t allocatedBlocks(ContentConfig const& c, uint64_t seed, uint64_t size) {
    if (size == 0) {
        return 0;
    }
    uint32_t bs = c.blockSize ? c.blockSize : 1;
    uint64_t nblocks = (size + bs - 1) / bs;
    uint64_t allocBytes = 0;

    for (uint64_t b = 0; b < nblocks; ++b) {
        if (isHole(c, seed, b)) {
            continue;
        }
        allocBytes += std::min<uint64_t>(bs, size - b * bs);
    }
    return (allocBytes + 511) / 512;
}

//  The default content provider — the config-driven generator above, behind the
//  IContentProvider di interface. Its opaque config is a serialized ContentConfig.
namespace {
struct DefaultProvider : IContentProvider {
    size_t read(std::string_view cfg, uint64_t seed, uint64_t size, uint64_t off, char* out,
                size_t len) const override {
        return prfs::content::read(deserialize(cfg), seed, size, off, out, len);
    }

    uint64_t allocatedBlocks(std::string_view cfg, uint64_t seed, uint64_t size) const override {
        return prfs::content::allocatedBlocks(deserialize(cfg), seed, size);
    }
};
} // namespace

static DefaultProvider g_defaultProvider;
static di::Register<IContentProvider> const reg{&g_defaultProvider, "config"};

static std::string s_provider;

std::string provider() { return s_provider.empty() ? "config" : s_provider; }

void setProvider(std::string_view name) {
    if (!di::global().has(IContentProvider::ID, name)) {
        throw std::out_of_range("content: unknown provider '" + std::string(name) + "'");
    }
    s_provider = std::string(name);
}

IContentProvider const& activeProvider() {
    return di::global().resolve<IContentProvider>(provider());
}

} // namespace prfs::content
