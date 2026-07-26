// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  Directory-DAG cycle prevention (design §2.2, todo T1 / bug B1). Directories
//  are multi-parent, so a link/move that closes a directory cycle would send a
//  tree-walking archiver into an infinite loop. The store rejects exactly the
//  cycle-closing directory operations (via a reachability DFS over directory
//  down-links) and nothing else: files are never restricted, and legitimate
//  multi-parent / diamond DAGs are allowed. Verified on both engines.
//
#include "prfs/memstore.hpp"
#include "prfs/prfs.hpp"
#include "support.hpp"

#include <gtest/gtest.h>

using namespace prfs;
using prfs::test::canon;
using prfs::test::Factory;
using prfs::test::uniqueTempDir;

static std::unique_ptr<IPrfs> oracle() { return makeMemStore(); }

static std::unique_ptr<IPrfs> backend() {
    Options o;
    o.clean = true;
    return openPrfs(uniqueTempDir("dag").string(), o);
}

class DagTest : public ::testing::TestWithParam<Factory> {
protected:
    void SetUp() override { fs = GetParam()(); }

    //  link a fresh dir named `name` under `parent`, return it.
    Node dir(Node parent, std::string const& name) {
        auto d = fs->mkdir();
        EXPECT_EQ(fs->link(parent, name, d), Error::OK);
        return d;
    }

    std::unique_ptr<IPrfs> fs;
};

TEST_P(DagTest, SelfLinkRejected) {
    auto root = fs->rwRoot();
    auto a = dir(root, "a");
    EXPECT_EQ(fs->link(a, "self", a), Error::INVAL);
    EXPECT_FALSE(fs->lookup(a, "self"));
}

TEST_P(DagTest, TransitiveCycleRejected) {
    auto root = fs->rwRoot();
    auto a = dir(root, "a");
    auto b = dir(a, "b");
    auto c = dir(b, "c"); // root → a → b → c

    EXPECT_EQ(fs->link(c, "toB", b), Error::INVAL);       // direct parent
    EXPECT_EQ(fs->link(c, "toA", a), Error::INVAL);       // transitive ancestor
    EXPECT_EQ(fs->link(c, "toRoot", root), Error::INVAL); // the root itself
    EXPECT_EQ(c->nlink(), 1u);                            // nothing linked in
}

TEST_P(DagTest, MultiParentAndDiamondAllowed) {
    auto root = fs->rwRoot();
    auto a = dir(root, "a");
    auto b = dir(root, "b");

    // A shared directory under two parents — a DAG, not a cycle.
    auto shared = fs->mkdir();
    EXPECT_EQ(fs->link(a, "s", shared), Error::OK);
    EXPECT_EQ(fs->link(b, "s", shared), Error::OK);
    EXPECT_EQ(shared->nlink(), 2u);
    EXPECT_EQ(fs->parents(shared).size(), 2u);

    // Diamond: root → a → x and root → b → x reaching the same leaf dir x.
    auto x = fs->mkdir();
    EXPECT_EQ(fs->link(shared, "x", x), Error::OK);
    EXPECT_EQ(x->nlink(), 1u);
}

TEST_P(DagTest, FilesAreNeverRestricted) {
    auto root = fs->rwRoot();
    auto a = dir(root, "a");
    auto b = dir(a, "b"); // root → a → b

    // A file may be linked anywhere, any number of times — it can't close a
    // directory cycle, so the check must skip it.
    auto f = fs->mkfile("data");
    EXPECT_EQ(fs->link(root, "f", f), Error::OK);
    EXPECT_EQ(fs->link(a, "f", f), Error::OK);
    EXPECT_EQ(fs->link(b, "f", f), Error::OK);
    EXPECT_EQ(f->nlink(), 3u);
}

TEST_P(DagTest, MoveIntoOwnSubtreeRejectedAndAtomic) {
    auto root = fs->rwRoot();
    auto d = dir(root, "d");
    auto sub = dir(d, "sub"); // root → d → sub

    std::string before = canon(*fs, fs->rwRoot());
    EXPECT_EQ(fs->move(root, "d", sub, "d2"), Error::INVAL); // d under its own descendant
    EXPECT_EQ(canon(*fs, fs->rwRoot()), before) << "rejected move must be a no-op";
    EXPECT_TRUE(fs->lookup(root, "d"));  // still where it was
    EXPECT_FALSE(fs->lookup(sub, "d2")); // and not at the destination
}

TEST_P(DagTest, LegitimateReparentMoveAllowed) {
    auto root = fs->rwRoot();
    auto d = dir(root, "d");
    auto e = dir(root, "e");
    dir(d, "child"); // d has a subtree

    EXPECT_EQ(fs->move(root, "d", e, "d"), Error::OK); // e is not under d → fine
    EXPECT_FALSE(fs->lookup(root, "d"));
    auto moved = fs->lookup(e, "d");
    ASSERT_TRUE(moved);
    EXPECT_TRUE(fs->lookup(moved, "child")); // subtree came along
}

TEST_P(DagTest, CyclePreventionIsDynamic) {
    auto root = fs->rwRoot();
    auto a = dir(root, "a");
    auto b = dir(root, "b");

    EXPECT_EQ(fs->link(a, "b", b), Error::OK);    // a → b
    EXPECT_EQ(fs->link(b, "a", a), Error::INVAL); // would close a ↔ b

    EXPECT_EQ(fs->unlink(a, "b"), Error::OK);  // break a → b
    EXPECT_EQ(fs->link(b, "a", a), Error::OK); // now b → a is acyclic
}

INSTANTIATE_TEST_SUITE_P(Engines, DagTest, ::testing::Values(&oracle, &backend));
