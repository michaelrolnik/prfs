// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  Logical clock (design §3, todo T6 / bug B6). Timestamps are a deterministic,
//  script-driven logical clock — never wall-clock — so scenarios are
//  reproducible and mtime-window incrementals are testable. now() reads the
//  clock without advancing it; setTime() is the only thing that moves it; new
//  nodes stamp atime/mtime/ctime = now(); all other time changes are explicit.
//  Verified on both engines.
//
#include "prfs/memstore.hpp"
#include "prfs/prfs.hpp"
#include "support.hpp"

#include <gtest/gtest.h>

using namespace prfs;
using prfs::test::Factory;
using prfs::test::uniqueTempDir;

static std::unique_ptr<IPrfs> oracle() { return makeMemStore(); }

static std::unique_ptr<IPrfs> backend() {
    Options o;
    o.clean = true;
    return openPrfs(uniqueTempDir("clock").string(), o);
}

class ClockTest : public ::testing::TestWithParam<Factory> {
protected:
    void SetUp() override { fs = GetParam()(); }

    std::unique_ptr<IPrfs> fs;
};

TEST_P(ClockTest, StartsAtZeroAndReadDoesNotAdvance) {
    EXPECT_EQ(fs->now(), 0u);
    EXPECT_EQ(fs->now(), 0u); // reading is not a tick
}

TEST_P(ClockTest, NewNodesStampCurrentTime) {
    fs->setTime(1000);
    EXPECT_EQ(fs->now(), 1000u);

    auto a = fs->mkfile("a");
    EXPECT_EQ(a->atime(), 1000u);
    EXPECT_EQ(a->mtime(), 1000u);
    EXPECT_EQ(a->ctime(), 1000u);

    // Same logical instant → same stamp (creation order does not advance time).
    auto b = fs->mkdir();
    EXPECT_EQ(b->mtime(), 1000u);

    fs->setTime(2000);
    auto c = fs->mkfile("c");
    EXPECT_EQ(c->mtime(), 2000u);
    EXPECT_EQ(a->mtime(), 1000u); // earlier node keeps its stamp
}

TEST_P(ClockTest, ExplicitSettersOverrideIndividualFields) {
    fs->setTime(500);
    auto f = fs->mkfile("x");

    f->mtime(9999);
    EXPECT_EQ(f->mtime(), 9999u);
    EXPECT_EQ(f->ctime(), 500u); // only the field that was set changes
    EXPECT_EQ(f->atime(), 500u);
}

TEST_P(ClockTest, SetTimeIsFreeMovingForScriptControl) {
    fs->setTime(100);
    fs->setTime(50); // scripts own the timeline; moving back is allowed
    EXPECT_EQ(fs->now(), 50u);

    auto f = fs->mkfile("");
    EXPECT_EQ(f->mtime(), 50u);
}

TEST_P(ClockTest, SnapshotPreservesPerVersionTimes) {
    fs->setTime(10);
    auto root = fs->rwRoot();
    auto f = fs->mkfile("v1");
    fs->link(root, "f", f);
    SnapId s1 = fs->snapshot();

    fs->setTime(20);
    f->mtime(fs->now()); // an explicit "touch" at the new time
    fs->snapshot();

    // The sealed snapshot keeps the old mtime; the live view has the new one.
    EXPECT_EQ(fs->lookup(fs->snapshotRoot(s1), "f")->mtime(), 10u);
    EXPECT_EQ(fs->lookup(fs->rwRoot(), "f")->mtime(), 20u);
}

INSTANTIATE_TEST_SUITE_P(Engines, ClockTest, ::testing::Values(&oracle, &backend));
