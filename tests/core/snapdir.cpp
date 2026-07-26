// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  Synthesized .snapshot directory (design §3.2, todo T5 / bug B5). Every live
//  directory D exposes a virtual `.snapshot` dir: a GETATTR-able node with a
//  stable, filehandle-round-trippable id, whose readdir lists one entry "N" per
//  snapshot D existed at, and `D/.snapshot/N` resolves to D viewed at snapshot
//  N. It is resolvable by name but hidden from D's own readdir, only appears in
//  a live view (no nesting), and is DIR-only. Verified on both engines.
//
#include "prfs/memstore.hpp"
#include "prfs/prfs.hpp"
#include "support.hpp"

#include <gtest/gtest.h>

#include <algorithm>

using namespace prfs;
using prfs::test::Factory;
using prfs::test::uniqueTempDir;

namespace {

std::unique_ptr<IPrfs> oracle() { return makeMemStore(); }

std::unique_ptr<IPrfs> backend() {
    Options o;
    o.clean = true;
    return openPrfs(uniqueTempDir("snapdir").string(), o);
}

bool hasName(std::vector<std::pair<std::string, Node>> const& ents, std::string const& n) {
    return std::any_of(ents.begin(), ents.end(), [&](auto const& e) { return e.first == n; });
}

} // namespace

class SnapDirTest : public ::testing::TestWithParam<Factory> {
protected:
    void SetUp() override {
        fs = GetParam()();
        //  snap 1: /f = "v1"   snap 2: /f = "v2"   snap 3: adds /d
        fs->setTime(10);
        auto root = fs->rwRoot();
        f = fs->mkfile("v1");
        fs->link(root, "f", f);
        s1 = fs->snapshot("one");

        fs->setTime(20);
        fs->setContent(f, "v2");
        s2 = fs->snapshot("two");

        fs->setTime(30);
        auto d = fs->mkdir();
        fs->link(root, "d", d);
        s3 = fs->snapshot("three");
    }

    std::unique_ptr<IPrfs> fs;
    Node f;
    SnapId s1 = 0, s2 = 0, s3 = 0;
};

TEST_P(SnapDirTest, LookupYieldsAnAttributableDir) {
    auto root = fs->rwRoot();
    auto snap = fs->lookup(root, SNAPSHOT_NAME);
    ASSERT_TRUE(snap);
    EXPECT_EQ(snap->type(), Type::DIR);
    EXPECT_EQ(snap->mode(), 0555u); // read-only
    EXPECT_EQ(snap->nlink(), 1u);
    EXPECT_NE(snap->id(), root->id());
    EXPECT_EQ(snap->mtime(), root->mtime()); // mirrors the base dir's times
}

TEST_P(SnapDirTest, IdIsStableForFilehandleRoundTrip) {
    auto root = fs->rwRoot();
    EXPECT_EQ(fs->lookup(root, SNAPSHOT_NAME)->id(), fs->lookup(root, SNAPSHOT_NAME)->id());
}

TEST_P(SnapDirTest, HiddenFromReaddirButResolvable) {
    auto root = fs->rwRoot();
    EXPECT_FALSE(hasName(fs->readdir(root), SNAPSHOT_NAME));
    EXPECT_TRUE(fs->lookup(root, SNAPSHOT_NAME));
}

TEST_P(SnapDirTest, ListsEverySnapshotTheDirExistedAt) {
    auto snap = fs->lookup(fs->rwRoot(), SNAPSHOT_NAME);
    auto ents = fs->readdir(snap);
    EXPECT_EQ(ents.size(), 3u); // root existed at snaps 1, 2, 3
    EXPECT_TRUE(hasName(ents, "1"));
    EXPECT_TRUE(hasName(ents, "2"));
    EXPECT_TRUE(hasName(ents, "3"));
}

TEST_P(SnapDirTest, ResolvesBaseAtSnapshotByName) {
    auto snap = fs->lookup(fs->rwRoot(), SNAPSHOT_NAME);

    auto rootAt1 = fs->lookup(snap, "1");
    ASSERT_TRUE(rootAt1);
    EXPECT_EQ(fs->lookup(rootAt1, "f")->content(), "v1");

    auto rootAt2 = fs->lookup(snap, "2");
    ASSERT_TRUE(rootAt2);
    EXPECT_EQ(fs->lookup(rootAt2, "f")->content(), "v2");
}

TEST_P(SnapDirTest, CreationSnapFiltersOlderSnapshots) {
    // /d was created just before snap 3, so its .snapshot lists only "3".
    auto d = fs->lookup(fs->rwRoot(), "d");
    auto snap = fs->lookup(d, SNAPSHOT_NAME);
    ASSERT_TRUE(snap);

    auto ents = fs->readdir(snap);
    EXPECT_EQ(ents.size(), 1u);
    EXPECT_TRUE(hasName(ents, "3"));
    EXPECT_FALSE(fs->lookup(snap, "1")); // d did not exist at snap 1
    EXPECT_TRUE(fs->lookup(snap, "3"));
}

TEST_P(SnapDirTest, RejectsBadNamesAndNoNesting) {
    auto snap = fs->lookup(fs->rwRoot(), SNAPSHOT_NAME);
    EXPECT_FALSE(fs->lookup(snap, "999"));         // not a sealed snapshot
    EXPECT_FALSE(fs->lookup(snap, "abc"));         // not a number
    EXPECT_FALSE(fs->lookup(snap, SNAPSHOT_NAME)); // no /.snapshot/.snapshot
}

TEST_P(SnapDirTest, ReservedNameCannotBeLinkedOrMovedOnto) {
    auto root = fs->rwRoot();
    EXPECT_EQ(fs->link(root, SNAPSHOT_NAME, fs->mkfile("")), Error::INVAL);
    EXPECT_FALSE(hasName(fs->readdir(root), SNAPSHOT_NAME)); // no real entry created
    EXPECT_EQ(fs->move(root, "f", root, SNAPSHOT_NAME), Error::INVAL);
    EXPECT_TRUE(fs->lookup(root, "f")); // source untouched by the rejected move
}

TEST_P(SnapDirTest, NotSynthesizedInFrozenViewsOrOnFiles) {
    // A frozen directory view has no live .snapshot (prevents recursion).
    auto frozenRoot = fs->snapshotRoot(s1);
    EXPECT_FALSE(fs->lookup(frozenRoot, SNAPSHOT_NAME));

    // Files have no .snapshot.
    EXPECT_FALSE(fs->lookup(f, SNAPSHOT_NAME));
}

INSTANTIATE_TEST_SUITE_P(Engines, SnapDirTest, ::testing::Values(&oracle, &backend));
