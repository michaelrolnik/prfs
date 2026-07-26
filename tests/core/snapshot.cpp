// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  Snapshot metadata (design §3.2, todo T3 / bug B3). Each snapshot() records
//  {ctime, label}: ctime is the logical time (T6 clock) the snapshot was taken,
//  label is an optional caller name. snapInfo(id) reads it back. Without this,
//  mtime-based / labelled incremental scenarios have no per-snapshot anchor.
//  Verified on both engines.
//
#include "prfs/memstore.hpp"
#include "prfs/prfs.hpp"
#include "support.hpp"

#include <gtest/gtest.h>

using namespace prfs;
using prfs::test::Factory;
using prfs::test::uniqueTempDir;

namespace {

std::unique_ptr<IPrfs> oracle() { return makeMemStore(); }

std::unique_ptr<IPrfs> backend() {
    Options o;
    o.clean = true;
    return openPrfs(uniqueTempDir("snapshot").string(), o);
}

} // namespace

class SnapshotMetaTest : public ::testing::TestWithParam<Factory> {
protected:
    void SetUp() override { fs = GetParam()(); }

    std::unique_ptr<IPrfs> fs;
};

TEST_P(SnapshotMetaTest, RecordsClockAndLabel) {
    fs->setTime(1000);
    SnapId s1 = fs->snapshot("monday");

    SnapInfo i = fs->snapInfo(s1);
    EXPECT_EQ(i.id, s1);
    EXPECT_EQ(i.ctime, 1000u);
    EXPECT_EQ(i.label, "monday");
}

TEST_P(SnapshotMetaTest, DefaultLabelIsEmpty) {
    fs->setTime(7);
    SnapId s = fs->snapshot();
    EXPECT_EQ(fs->snapInfo(s).label, "");
    EXPECT_EQ(fs->snapInfo(s).ctime, 7u);
}

TEST_P(SnapshotMetaTest, EachSnapshotStampsIndependently) {
    fs->setTime(100);
    SnapId a = fs->snapshot("a");
    fs->setTime(250);
    SnapId b = fs->snapshot("b");

    EXPECT_EQ(fs->snapInfo(a).ctime, 100u);
    EXPECT_EQ(fs->snapInfo(a).label, "a");
    EXPECT_EQ(fs->snapInfo(b).ctime, 250u);
    EXPECT_EQ(fs->snapInfo(b).label, "b");
}

TEST_P(SnapshotMetaTest, UnknownSnapIdYieldsEmptyMeta) {
    fs->snapshot("only");
    SnapInfo none = fs->snapInfo(999);
    EXPECT_EQ(none.id, 999u);
    EXPECT_EQ(none.ctime, 0u);
    EXPECT_TRUE(none.label.empty());
}

TEST_P(SnapshotMetaTest, MetadataAlignsWithSnapshotList) {
    fs->setTime(10);
    fs->snapshot("x");
    fs->setTime(20);
    fs->snapshot("y");

    auto ids = fs->snapshots();
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(fs->snapInfo(ids[0]).label, "x");
    EXPECT_EQ(fs->snapInfo(ids[1]).label, "y");
}

INSTANTIATE_TEST_SUITE_P(Engines, SnapshotMetaTest, ::testing::Values(&oracle, &backend));
