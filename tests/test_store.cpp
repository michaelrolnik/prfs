// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  Contract tests for IPrfs (design §13). Parameterized over two engines: the
//  independent reference oracle (MemStore) and the selected production backend
//  (openPrfs → PrfsStore over the -Dstorage= engine). Running both is the
//  differential test — they must agree.
//
#include "prfs/memstore.hpp"
#include "prfs/prfs.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <unistd.h>

using namespace prfs;

namespace {

using Factory = std::function<std::unique_ptr<IPrfs>()>;

std::unique_ptr<IPrfs> oracle() { return makeMemStore(); }

std::unique_ptr<IPrfs> backend() {
    static std::atomic<unsigned> n{0};
    auto dir = std::filesystem::temp_directory_path() /
               ("prfs-test-" + std::to_string(::getpid()) + "-" + std::to_string(n++));
    Options o;
    o.clean = true;
    return openPrfs(dir.string(), o);
}

} // namespace

class StoreTest : public ::testing::TestWithParam<Factory> {
protected:
    void SetUp() override { fs = GetParam()(); }

    std::unique_ptr<IPrfs> fs;
};

TEST_P(StoreTest, CreateLinkLookup) {
    auto root = fs->rwRoot();
    EXPECT_EQ(root->type(), Type::DIR);

    auto f = fs->mkfile("recipe");
    EXPECT_EQ(f->type(), Type::REG);
    EXPECT_EQ(f->nlink(), 0u);

    EXPECT_EQ(fs->link(root, "a", f), Error::OK);
    EXPECT_EQ(f->nlink(), 1u);
    EXPECT_EQ(fs->link(root, "a", f), Error::EXIST); // duplicate name

    auto got = fs->lookup(root, "a");
    ASSERT_TRUE(got);
    EXPECT_EQ(got->id(), f->id());
    EXPECT_FALSE(fs->lookup(root, "nope"));
}

TEST_P(StoreTest, Readdir) {
    auto root = fs->rwRoot();
    fs->link(root, "a", fs->mkfile(""));
    fs->link(root, "b", fs->mkdir());
    EXPECT_EQ(fs->readdir(root).size(), 2u);
}

TEST_P(StoreTest, HardLinkAndParents) {
    auto root = fs->rwRoot();
    auto d = fs->mkdir();
    fs->link(root, "d", d);
    auto f = fs->mkfile("");
    fs->link(root, "x", f);
    fs->link(d, "y", f);
    EXPECT_EQ(f->nlink(), 2u);
    EXPECT_EQ(fs->parents(f).size(), 2u);
}

TEST_P(StoreTest, Unlink) {
    auto root = fs->rwRoot();
    auto f = fs->mkfile("");
    fs->link(root, "a", f);
    EXPECT_EQ(fs->unlink(root, "a"), Error::OK);
    EXPECT_FALSE(fs->lookup(root, "a"));
    EXPECT_EQ(f->nlink(), 0u);
    EXPECT_EQ(fs->unlink(root, "a"), Error::NOENT);
}

TEST_P(StoreTest, Symlink) {
    auto l = fs->symlink("/target/path");
    EXPECT_EQ(l->type(), Type::LNK);
    EXPECT_EQ(l->target(), "/target/path");
}

TEST_P(StoreTest, Device) {
    auto dev = fs->mknod(Type::BLK, 8, 3);
    EXPECT_EQ(dev->type(), Type::BLK);
    EXPECT_EQ(dev->rdev(), std::make_pair(8u, 3u));
}

TEST_P(StoreTest, SnapshotRangeBackAndImmutability) {
    auto root = fs->rwRoot();
    auto f = fs->mkfile("v1");
    f->size(100);
    fs->link(root, "f", f);

    SnapId s1 = fs->snapshot();
    fs->setContent(f, "v2");
    f->size(200);
    fs->snapshot();

    EXPECT_EQ(fs->lookup(fs->rwRoot(), "f")->content(), "v2");

    auto f1 = fs->lookup(fs->snapshotRoot(s1), "f");
    ASSERT_TRUE(f1);
    EXPECT_EQ(f1->content(), "v1");
    EXPECT_EQ(f1->size(), 100u);
}

TEST_P(StoreTest, DiffNodes) {
    auto root = fs->rwRoot();
    auto a = fs->mkfile("a");
    fs->link(root, "a", a);
    SnapId s1 = fs->snapshot();

    auto b = fs->mkfile("b");
    fs->link(root, "b", b);  // CREATED
    fs->setContent(a, "a2"); // MODIFIED_CONTENT
    SnapId s2 = fs->snapshot();

    bool aMod = false, bNew = false;
    for (auto const& nd : fs->diffNodes(s1, s2)) {
        if (nd.id == a->id()) {
            aMod = nd.change == NodeChange::MODIFIED_CONTENT;
        }
        if (nd.id == b->id()) {
            bNew = nd.change == NodeChange::CREATED;
        }
    }
    EXPECT_TRUE(aMod);
    EXPECT_TRUE(bNew);
}

TEST_P(StoreTest, DiffPaths) {
    auto root = fs->rwRoot();
    fs->link(root, "a", fs->mkfile(""));
    SnapId s1 = fs->snapshot();

    fs->link(root, "b", fs->mkfile("")); // ADDED   /b
    fs->unlink(root, "a");               // REMOVED /a
    SnapId s2 = fs->snapshot();

    bool addB = false, remA = false;
    for (auto const& p : fs->diffPaths(s1, s2)) {
        if (p.name == "b" && p.change == PathChange::ADDED) {
            addB = true;
        }
        if (p.name == "a" && p.change == PathChange::REMOVED) {
            remA = true;
        }
    }
    EXPECT_TRUE(addB);
    EXPECT_TRUE(remA);
}

TEST_P(StoreTest, CyclePrevention) {
    auto root = fs->rwRoot();
    auto a = fs->mkdir();
    fs->link(root, "a", a);
    auto b = fs->mkdir();
    fs->link(a, "b", b);

    EXPECT_EQ(fs->link(b, "loop", a), Error::INVAL); // a is an ancestor of b
    EXPECT_EQ(fs->link(root, "b2", b), Error::OK);   // second parent for b — fine (DAG)
    EXPECT_EQ(b->nlink(), 2u);
}

TEST_P(StoreTest, MoveIsRename) {
    auto root = fs->rwRoot();
    auto d = fs->mkdir();
    fs->link(root, "d", d);
    auto f = fs->mkfile("data");
    fs->link(root, "f", f);
    uint64_t fid = f->id();

    EXPECT_EQ(fs->move(root, "f", d, "g"), Error::OK);
    EXPECT_FALSE(fs->lookup(root, "f"));
    auto g = fs->lookup(d, "g");
    ASSERT_TRUE(g);
    EXPECT_EQ(g->id(), fid); // same node → rename, not copy
    EXPECT_EQ(g->content(), "data");
}

TEST_P(StoreTest, Stats) {
    auto root = fs->rwRoot();
    fs->link(root, "a", fs->mkfile(""));
    auto d = fs->mkdir();
    fs->link(root, "d", d);

    auto s = fs->stats();
    EXPECT_EQ(s.nodes[int(Type::DIR)], 2u); // root + d
    EXPECT_EQ(s.nodes[int(Type::REG)], 1u);
    EXPECT_EQ(s.links, 2u);
}

INSTANTIATE_TEST_SUITE_P(Engines, StoreTest, ::testing::Values(&oracle, &backend));
