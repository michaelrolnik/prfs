// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  Content-provider tests (docs/content.md §7). The generator is a pure,
//  deterministic, random-access function of (config, fileSeed, offset); these
//  pin determinism, range equivalence, EOF, the entropy/dedup/sparse dials,
//  config round-trip, and RNG configurability.
//
#include "prfs/content.hpp"
#include "prfs/rng.hpp"

#include <gtest/gtest.h>

#include <set>
#include <string>

using namespace prfs;
using content::ContentConfig;

namespace {

std::string gen(ContentConfig const& c, uint64_t seed, uint64_t size) {
    std::string s(size, '\0');
    size_t n = content::read(c, seed, size, 0, s.data(), size);
    s.resize(n);
    return s;
}

size_t distinct(std::string const& s) { return std::set<char>(s.begin(), s.end()).size(); }

} // namespace

//  Every test starts from the default generator so ordering can't leak state.
class ContentTest : public ::testing::Test {
protected:
    void SetUp() override { rng::setActive(rng::Kind::Philox); }
};

TEST_F(ContentTest, Deterministic) {
    ContentConfig c;
    EXPECT_EQ(gen(c, 42, 8192), gen(c, 42, 8192)); // same inputs → same bytes
    EXPECT_NE(gen(c, 42, 8192), gen(c, 43, 8192)); // different file → different bytes
}

TEST_F(ContentTest, RandomAccessEquivalence) {
    ContentConfig c;
    const uint64_t N = 4096 * 3 + 137;
    std::string full = gen(c, 7, N);

    // byte-by-byte
    std::string bb(N, '\0');
    for (uint64_t o = 0; o < N; ++o) {
        content::read(c, 7, N, o, &bb[o], 1);
    }
    EXPECT_EQ(bb, full);

    // in mis-aligned chunks (not a multiple of blockSize)
    std::string ch(N, '\0');
    for (uint64_t o = 0; o < N; o += 100) {
        content::read(c, 7, N, o, &ch[o], std::min<uint64_t>(100, N - o));
    }
    EXPECT_EQ(ch, full);
}

TEST_F(ContentTest, SizeAndEof) {
    ContentConfig c;
    char buf[64];
    EXPECT_EQ(content::read(c, 1, 100, 100, buf, 64), 0u); // at EOF
    EXPECT_EQ(content::read(c, 1, 100, 200, buf, 64), 0u); // past EOF
    EXPECT_EQ(content::read(c, 1, 100, 80, buf, 64), 20u); // clamps to size
}

TEST_F(ContentTest, EntropyDial) {
    ContentConfig c;

    c.entropy = 0; // constant → fully compressible
    std::string lo = gen(c, 5, 4096);
    EXPECT_EQ(distinct(lo), 1u);

    c.entropy = 255; // incompressible → ~all 256 values
    std::string hi = gen(c, 5, 4096);
    EXPECT_GT(distinct(hi), 200u);

    // monotone: more entropy, no fewer distinct symbols
    c.entropy = 64;
    size_t mid = distinct(gen(c, 5, 4096));
    EXPECT_LE(distinct(lo), mid);
    EXPECT_LE(mid, distinct(hi));
}

TEST_F(ContentTest, DedupIsWholeFsAndRatioBounded) {
    ContentConfig c;
    c.blockSize = 64;
    c.dedupPercent = 100; // every block a duplicate
    c.dedupCorpus = 4;    // from a 4-block corpus

    std::set<std::string> blocks; // block 0 of many distinct files
    for (uint64_t seed = 1; seed <= 300; ++seed) {
        blocks.insert(gen(c, seed, 64));
    }
    EXPECT_LE(blocks.size(), 4u); // cross-file dedup: ≤ corpus distinct contents
    EXPECT_GE(blocks.size(), 1u);

    c.dedupPercent = 0; // no dedup → essentially all distinct
    std::set<std::string> uniq;
    for (uint64_t seed = 1; seed <= 300; ++seed) {
        uniq.insert(gen(c, seed, 64));
    }
    EXPECT_GT(uniq.size(), 290u);
}

TEST_F(ContentTest, Sparse) {
    ContentConfig c;
    c.blockSize = 512;
    const uint64_t N = 512 * 10;

    c.sparsePercent = 100; // all holes
    EXPECT_EQ(content::allocatedBlocks(c, 7, N), 0u);
    std::string z = gen(c, 7, N);
    EXPECT_EQ(distinct(z), 1u);
    EXPECT_EQ(z[0], '\0');

    c.sparsePercent = 0; // fully allocated
    EXPECT_EQ(content::allocatedBlocks(c, 7, N), 10u);

    c.sparsePercent = 50; // somewhere in between, deterministically
    uint64_t half = content::allocatedBlocks(c, 7, N);
    EXPECT_GT(half, 0u);
    EXPECT_LT(half, 10u);
}

TEST_F(ContentTest, ConfigRoundTrip) {
    ContentConfig c{8192, 200, 10, 25, 1024};
    EXPECT_EQ(content::deserialize(content::serialize(c)), c);

    EXPECT_THROW(content::deserialize("nope"), std::runtime_error);               // bad magic
    EXPECT_THROW(content::deserialize(std::string(8, '\0')), std::runtime_error); // truncated
}

TEST_F(ContentTest, RngIsConfigurable) {
    ContentConfig c; // full entropy

    rng::setActive(rng::Kind::Philox);
    std::string p = gen(c, 42, 4096);
    EXPECT_EQ(p, gen(c, 42, 4096)); // deterministic per generator

    rng::setActive(rng::Kind::Threefry);
    std::string t = gen(c, 42, 4096);
    EXPECT_EQ(t, gen(c, 42, 4096));
    EXPECT_NE(p, t); // switching the generator changes the bytes

    rng::Kind k;
    EXPECT_TRUE(rng::parse("threefry", k));
    EXPECT_EQ(k, rng::Kind::Threefry);
    EXPECT_FALSE(rng::parse("bogus", k));
    EXPECT_STREQ(rng::name(rng::Kind::Philox), "philox");
}
