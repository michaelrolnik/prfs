// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  Property / invariant tests (todo S10). Each engine — oracle and the selected
//  backend — must satisfy these regardless of the other:
//
//    I1  parents(n).size() == n.nlink()             (root excepted: nlink=1, no parent)
//    I2  a directory's readdir names are unique
//    I3  lookup(dir,name) == the readdir child      (and reverse: dir ∈ parents(child))
//    I4  Σ readdir(dir).size() over all dirs == stats().links
//    I5  stats().nodes[type] / totalSize == the live (nlink>0) node population
//    I6  a snapshot's view never changes after later mutations (immutability)
//
//  Driven by the shared Lockstep engine (a() is the system under test; b() is an
//  identical twin the driver keeps in step — its equivalence is checked by the
//  determinism suite, here it just supplies the same op stream).
//
#include "prfs/memstore.hpp"
#include "prfs/prfs.hpp"
#include "support.hpp"

#include <gtest/gtest.h>

#include <map>
#include <set>
#include <string>
#include <vector>

using namespace prfs;
using namespace prfs::test;

namespace {

std::unique_ptr<IPrfs> oracle() { return makeMemStore(); }

std::unique_ptr<IPrfs> backend() {
    Options o;
    o.clean = true;
    return openPrfs(uniqueTempDir("invariant").string(), o);
}

void checkInvariants(IPrfs& fs, std::vector<Node> const& all, uint64_t rootId) {
    // Dedup handles by id (root appears once; each other node created once).
    std::map<uint64_t, Node> uniq;
    for (Node const& n : all) {
        uniq[n->id()] = n;
    }

    // I1: incoming-link count == nlink (root is the sole exception).
    for (auto const& [id, n] : uniq) {
        if (id == rootId) {
            continue;
        }
        ASSERT_EQ(fs.parents(n).size(), n->nlink()) << "I1 node " << id;
    }

    uint64_t linkSum = 0;
    uint64_t totalSize = 0;
    uint64_t nodeCount[7] = {0, 0, 0, 0, 0, 0, 0};

    for (auto const& [id, n] : uniq) {
        if (n->type() == Type::DIR) {
            std::set<std::string> names;
            for (auto const& [nm, ch] : fs.readdir(n)) {
                ASSERT_TRUE(names.insert(nm).second) << "I2 dup name " << nm << " in dir " << id;

                auto look = fs.lookup(n, nm);
                ASSERT_TRUE(look) << "I3 lookup missing " << nm;
                ASSERT_EQ(look->id(), ch->id()) << "I3 lookup≠readdir for " << nm;

                bool backlink = false;
                for (auto const& p : fs.parents(ch)) {
                    backlink = backlink || p->id() == id;
                }
                ASSERT_TRUE(backlink) << "I3 " << id << " ∉ parents(" << ch->id() << ")";

                ++linkSum;
            }
        }
        if (n->nlink() > 0) {
            ++nodeCount[int(n->type())];
            if (n->type() == Type::REG) {
                totalSize += n->size();
            }
        }
    }

    Stats s = fs.stats();
    ASSERT_EQ(s.links, linkSum) << "I4";
    ASSERT_EQ(s.totalSize, totalSize) << "I5 size";
    for (int t = 0; t < 7; ++t) {
        ASSERT_EQ(s.nodes[t], nodeCount[t]) << "I5 nodes[" << t << "]";
    }
}

} // namespace

class InvariantTest : public ::testing::TestWithParam<Factory> {};

TEST_P(InvariantTest, HoldUnderRandomOps) {
    Factory f = GetParam();
    Lockstep m(f, f, 9001);
    uint64_t rootId = m.a().rwRoot()->id();

    struct Recorded {
        SnapId id;
        std::string view;
    };

    std::vector<Recorded> snaps;

    for (int i = 0; i < 800; ++i) {
        m.step(i);
        SCOPED_TRACE("after op #" + std::to_string(i));

        std::vector<Node> all;
        for (auto const& p : m.nodes()) {
            all.push_back(p.a);
        }
        checkInvariants(m.a(), all, rootId);

        // I6: record each new snapshot's view, re-verify every recorded view.
        auto ids = m.a().snapshots();
        while (snaps.size() < ids.size()) {
            SnapId s = ids[snaps.size()];
            snaps.push_back({s, canon(m.a(), m.a().snapshotRoot(s))});
        }
        for (auto const& r : snaps) {
            ASSERT_EQ(r.view, canon(m.a(), m.a().snapshotRoot(r.id)))
                << "I6 snapshot " << r.id << " mutated";
        }
    }
}

//  Atomicity: a rejected operation must leave the store byte-for-byte unchanged
//  (validate-before-write). No partial state on an error path.
TEST_P(InvariantTest, FailedOpsAreNoOps) {
    auto fs = GetParam()();
    auto root = fs->rwRoot();
    auto d = fs->mkdir();
    fs->link(root, "d", d);
    auto e = fs->mkdir();
    fs->link(d, "e", e);
    fs->link(root, "f", fs->mkfile("x"));
    fs->snapshot();

    std::string before = canon(*fs, fs->rwRoot());
    std::string sbefore = statsStr(*fs);

    EXPECT_EQ(fs->link(root, "f", fs->mkfile("y")), Error::EXIST); // duplicate name
    EXPECT_EQ(fs->unlink(root, "nope"), Error::NOENT);             // missing entry
    EXPECT_EQ(fs->link(e, "loop", d), Error::INVAL);               // would close a cycle
    EXPECT_EQ(fs->move(root, "nope", d, "x"), Error::NOENT);       // missing source

    EXPECT_EQ(canon(*fs, fs->rwRoot()), before) << "a failed op mutated reachable state";
    EXPECT_EQ(statsStr(*fs), sbefore) << "a failed op moved the counters";
}

INSTANTIATE_TEST_SUITE_P(Engines, InvariantTest, ::testing::Values(&oracle, &backend));
