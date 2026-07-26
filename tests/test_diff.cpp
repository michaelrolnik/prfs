// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  Differential harness (design §13, docs/todo.md). Drives a pseudo-random op
//  sequence into two engines in lockstep — the independent reference oracle
//  (MemStore) and the selected production backend (openPrfs → PrfsStore over
//  the -Dstorage= engine) — and asserts they stay observably equivalent.
//
//  Internal node ids differ between engines, so equivalence is checked by a
//  canonical serialization of the reachable graph: a deterministic DFS over
//  name-sorted children assigns each node a canonical index by first-visit
//  order, then emits per-node attributes and edges. Two engines agree iff their
//  serializations are byte-identical. Error codes, snapshot ids and global
//  stats are cross-checked at every step; every recorded snapshot view is
//  re-checked at the end (exercising range-back reads through the backend).
//
#include "prfs/memstore.hpp"
#include "prfs/prfs.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

using namespace prfs;

namespace {

std::unique_ptr<IPrfs> makeOracle() { return makeMemStore(); }

std::unique_ptr<IPrfs> makeBackend() {
    static std::atomic<unsigned> n{0};
    auto dir = std::filesystem::temp_directory_path() /
               ("prfs-diff-" + std::to_string(::getpid()) + "-" + std::to_string(n++));
    Options o;
    o.clean = true;
    return openPrfs(dir.string(), o);
}

//  Canonical serialization of the graph reachable from `root` (design §2.2 is a
//  DAG, so first-visit dedup terminates). Independent of internal node ids.
std::string canon(IPrfs& fs, Node root) {
    std::map<uint64_t, int> canonOf;
    std::vector<std::string> nodes;
    std::vector<std::string> edges;

    std::function<int(Node)> visit = [&](Node n) -> int {
        auto it = canonOf.find(n->id());
        if (it != canonOf.end()) {
            return it->second;
        }

        int cid = int(canonOf.size());
        canonOf.emplace(n->id(), cid);

        std::string a = "N" + std::to_string(cid) + " t" + std::to_string(int(n->type())) + " nl" +
                        std::to_string(n->nlink()) + " sz" + std::to_string(n->size());
        switch (n->type()) {
        case Type::REG:
            a += " c:" + n->content();
            break;
        case Type::LNK:
            a += " l:" + n->target();
            break;
        case Type::BLK:
        case Type::CHR: {
            auto [maj, min] = n->rdev();
            a += " d:" + std::to_string(maj) + "," + std::to_string(min);
            break;
        }
        default:
            break;
        }
        nodes.push_back(a);

        if (n->type() == Type::DIR) {
            auto ents = fs.readdir(n);
            std::sort(ents.begin(), ents.end(),
                      [](auto const& x, auto const& y) { return x.first < y.first; });
            for (auto const& [name, child] : ents) {
                int c = visit(child);
                edges.push_back("E" + std::to_string(cid) + " " + name + " " + std::to_string(c));
            }
        }
        return cid;
    };
    visit(root);

    std::string out;
    for (auto const& s : nodes) {
        out += s + "\n";
    }
    for (auto const& s : edges) {
        out += s + "\n";
    }
    return out;
}

std::string statsStr(IPrfs const& fs) {
    Stats s = fs.stats();
    std::string out = "links=" + std::to_string(s.links) + " sz=" + std::to_string(s.totalSize);
    for (int i = 0; i < 7; ++i) {
        out += " n" + std::to_string(i) + "=" + std::to_string(s.nodes[i]);
    }
    return out;
}

//  One logical node, held as a handle into each engine (ids may differ).
struct Pair {
    Node o;
    Node b;
};

class Model {
public:
    explicit Model(unsigned seed)
        : m_rng(seed) {
        m_o = makeOracle();
        m_b = makeBackend();
        m_nodes.push_back({m_o->rwRoot(), m_b->rwRoot()});
    }

    void step(int i) {
        SCOPED_TRACE("op #" + std::to_string(i));
        switch (pick(11)) {
        case 0:
            create(Type::REG);
            break;
        case 1:
            create(Type::DIR);
            break;
        case 2:
            create(Type::LNK);
            break;
        case 3:
            create(m_rng() & 1 ? Type::BLK : Type::CHR);
            break;
        case 4:
            create(m_rng() & 1 ? Type::FIFO : Type::SOCK);
            break;
        case 5:
        case 6:
            doLink();
            break;
        case 7:
            doUnlink();
            break;
        case 8:
            doMove();
            break;
        case 9:
            doMutate();
            break;
        case 10:
            doSnapshot();
            break;
        }
        checkEquivalent();
    }

    //  Re-verify every recorded snapshot view — range-back reads through both
    //  engines must still agree.
    void checkHistory() {
        ASSERT_EQ(m_o->snapshots(), m_b->snapshots());
        for (SnapId s : m_o->snapshots()) {
            SCOPED_TRACE("snapshot view " + std::to_string(s));
            ASSERT_EQ(canon(*m_o, m_o->snapshotRoot(s)), canon(*m_b, m_b->snapshotRoot(s)));
        }
    }

private:
    size_t pick(size_t n) { return m_rng() % n; }

    std::string name() { return std::string(1, char('a' + pick(5))); }

    std::string blob() { return "v" + std::to_string(pick(100)); }

    Pair& any() { return m_nodes[pick(m_nodes.size())]; }

    //  A random directory (root, index 0, is always one).
    Pair& dir() {
        std::vector<size_t> dirs;
        for (size_t i = 0; i < m_nodes.size(); ++i) {
            if (m_nodes[i].o->type() == Type::DIR) {
                dirs.push_back(i);
            }
        }
        return m_nodes[dirs[pick(dirs.size())]];
    }

    void create(Type t) {
        Pair p;
        switch (t) {
        case Type::REG: {
            auto c = blob();
            p = {m_o->mkfile(c), m_b->mkfile(c)};
            break;
        }
        case Type::DIR:
            p = {m_o->mkdir(), m_b->mkdir()};
            break;
        case Type::LNK: {
            auto tg = blob();
            p = {m_o->symlink(tg), m_b->symlink(tg)};
            break;
        }
        case Type::FIFO:
            p = {m_o->mkfifo(), m_b->mkfifo()};
            break;
        case Type::SOCK:
            p = {m_o->mksock(), m_b->mksock()};
            break;
        default: { // BLK / CHR
            uint32_t maj = uint32_t(pick(8)), min = uint32_t(pick(8));
            p = {m_o->mknod(t, maj, min), m_b->mknod(t, maj, min)};
            break;
        }
        }
        m_nodes.push_back(p);
    }

    void doLink() {
        Pair& d = dir();
        Pair& c = any();
        auto nm = name();
        ASSERT_EQ(m_o->link(d.o, nm, c.o), m_b->link(d.b, nm, c.b)) << "link name=" << nm;
    }

    //  Bias toward a real name (from the dir listing) so unlinks land.
    std::string existingName(Pair& d) {
        auto ents = m_o->readdir(d.o);
        if (!ents.empty() && (m_rng() & 3)) {
            return ents[pick(ents.size())].first;
        }
        return name();
    }

    void doUnlink() {
        Pair& d = dir();
        auto nm = existingName(d);
        ASSERT_EQ(m_o->unlink(d.o, nm), m_b->unlink(d.b, nm)) << "unlink name=" << nm;
    }

    void doMove() {
        Pair& s = dir();
        auto sn = existingName(s);
        Pair& d = dir();
        auto dn = name();
        ASSERT_EQ(m_o->move(s.o, sn, d.o, dn), m_b->move(s.b, sn, d.b, dn))
            << "move " << sn << " -> " << dn;
    }

    void doMutate() {
        Pair& p = any();
        switch (p.o->type()) {
        case Type::REG: {
            auto c = blob();
            ASSERT_EQ(m_o->setContent(p.o, c), m_b->setContent(p.b, c));
            break;
        }
        case Type::LNK: {
            auto t = blob();
            ASSERT_EQ(m_o->setTarget(p.o, t), m_b->setTarget(p.b, t));
            break;
        }
        case Type::BLK:
        case Type::CHR: {
            uint32_t maj = uint32_t(pick(8)), min = uint32_t(pick(8));
            ASSERT_EQ(m_o->setRdev(p.o, maj, min), m_b->setRdev(p.b, maj, min));
            break;
        }
        default:
            break; // nothing mutable on DIR/FIFO/SOCK here
        }
    }

    void doSnapshot() { ASSERT_EQ(m_o->snapshot(), m_b->snapshot()); }

    void checkEquivalent() {
        ASSERT_EQ(canon(*m_o, m_o->rwRoot()), canon(*m_b, m_b->rwRoot()));
        ASSERT_EQ(statsStr(*m_o), statsStr(*m_b));
    }

    std::mt19937 m_rng;
    std::unique_ptr<IPrfs> m_o;
    std::unique_ptr<IPrfs> m_b;
    std::vector<Pair> m_nodes;
};

class DiffTest : public ::testing::TestWithParam<unsigned> {};

TEST_P(DiffTest, BackendMatchesOracle) {
    Model m(GetParam());

    for (int i = 0; i < 1500; ++i) {
        m.step(i);
    }
    m.checkHistory();
}

INSTANTIATE_TEST_SUITE_P(Seeds, DiffTest,
                         ::testing::Values(1u, 2u, 3u, 7u, 42u, 100u, 2024u, 31337u));

} // namespace
