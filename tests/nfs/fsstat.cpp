// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  Tests for the FSSTAT / FSINFO projection (design §9). The mapping is a pure
//  function of (Stats, FsConfig), so most checks need no store; one integration
//  case drives a real store and confirms used-file / used-byte accounting.
//
#include "prfs/fsstat.hpp"
#include "prfs/memstore.hpp"
#include "prfs/prfs.hpp"

#include <gtest/gtest.h>

using namespace prfs;

namespace {

Stats makeStats(uint64_t reg, uint64_t dir, uint64_t totalSize) {
    Stats s;
    s.nodes[int(Type::REG)] = reg;
    s.nodes[int(Type::DIR)] = dir;
    s.totalSize = totalSize;
    return s;
}

} // namespace

TEST(FsStat, UsedFilesAreSummedAcrossTypes) {
    Stats s = makeStats(/*reg*/ 3, /*dir*/ 2, /*bytes*/ 0);
    s.nodes[int(Type::LNK)] = 1;

    FsConfig cfg;
    FsStat r = fsStat(s, cfg);

    EXPECT_EQ(r.tfiles, cfg.capacityFiles);
    EXPECT_EQ(r.tfiles - r.ffiles, 6u); // 3 + 2 + 1
    EXPECT_EQ(r.afiles, r.ffiles);
}

TEST(FsStat, UsedBytesRoundUpToABlock) {
    FsConfig cfg;
    cfg.blockSize = 4096;

    // 1 byte used → one whole block reported as used.
    FsStat r = fsStat(makeStats(1, 0, 1), cfg);
    EXPECT_EQ(r.tbytes - r.fbytes, 4096u);
    EXPECT_EQ(r.abytes, r.fbytes);

    // Exactly one block stays one block (no extra rounding).
    EXPECT_EQ(fsStat(makeStats(1, 0, 4096), cfg).fbytes, cfg.capacityBytes - 4096u);
}

TEST(FsStat, CapacityGrowsToCoverOverflowSoFreeNeverUnderflows) {
    FsConfig cfg;
    cfg.capacityBytes = 1000;
    cfg.capacityFiles = 2;
    cfg.blockSize = 1;

    // Usage exceeds the advertised capacity: total clamps up, free is zero.
    FsStat r = fsStat(makeStats(/*reg*/ 5, 0, /*bytes*/ 5000), cfg);
    EXPECT_EQ(r.tbytes, 5000u);
    EXPECT_EQ(r.fbytes, 0u);
    EXPECT_EQ(r.tfiles, 5u);
    EXPECT_EQ(r.ffiles, 0u);
}

TEST(FsStat, Invarsec) {
    EXPECT_EQ(fsStat(makeStats(0, 0, 0)).invarsec, 0u); // volatile
}

TEST(FsInfo, ReflectsConfigAndCapabilities) {
    FsConfig cfg;
    FsInfo i = fsInfo(cfg);

    EXPECT_EQ(i.rtpref, cfg.rtpref);
    EXPECT_EQ(i.wtmax, cfg.wtmax);
    EXPECT_EQ(i.maxfilesize, cfg.maxfilesize);
    EXPECT_TRUE(i.properties & fsf::LINK);
    EXPECT_TRUE(i.properties & fsf::SYMLINK);
    EXPECT_TRUE(i.properties & fsf::HOMOGENEOUS);
    EXPECT_TRUE(i.properties & fsf::CANSETTIME);
}

TEST(FsStat, ProjectsFromAStore) {
    auto fs = makeMemStore();
    auto root = fs->rwRoot();
    fs->link(root, "a", fs->mkfile("x"));
    auto big = fs->mkfile("");
    big->size(10000);
    fs->link(root, "big", big);
    auto d = fs->mkdir();
    fs->link(root, "d", d);

    FsConfig cfg;
    FsStat r = fsStat(*fs, cfg);

    // root + d (2 dirs) + a + big (2 regs) = 4 used files.
    EXPECT_EQ(r.tfiles - r.ffiles, 4u);
    // used bytes cover the 10000-byte file, rounded up to a block.
    uint64_t usedBytes = r.tbytes - r.fbytes;
    EXPECT_GE(usedBytes, 10000u);
    EXPECT_EQ(usedBytes % cfg.blockSize, 0u);
}
