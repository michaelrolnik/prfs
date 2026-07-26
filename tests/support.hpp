// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

#pragma once
//
//  Shared test support: a temp-dir helper, a canonical serialization of the
//  reachable graph (engine-id independent), and a lockstep random-op driver
//  that applies the same pseudo-random operation to two engines and asserts
//  their error codes / snapshot ids agree. Used by the differential,
//  determinism and invariant suites.
//
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

namespace prfs::test {

using Factory = std::function<std::unique_ptr<IPrfs>()>;

inline std::filesystem::path uniqueTempDir(std::string const& tag) {
    static std::atomic<unsigned> n{0};
    return std::filesystem::temp_directory_path() /
           ("prfs-" + tag + "-" + std::to_string(::getpid()) + "-" + std::to_string(n++));
}

//  Canonical serialization of the graph reachable from `root` (design §2.2 is a
//  DAG, so first-visit dedup terminates). Independent of internal node ids: a
//  deterministic DFS over name-sorted children numbers nodes by first-visit
//  order. Two views are structurally identical iff their strings are equal.
inline std::string canon(IPrfs& fs, Node root) {
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

inline std::string statsStr(IPrfs const& fs) {
    Stats s = fs.stats();
    std::string out = "links=" + std::to_string(s.links) + " sz=" + std::to_string(s.totalSize);
    for (int i = 0; i < 7; ++i) {
        out += " n" + std::to_string(i) + "=" + std::to_string(s.nodes[i]);
    }
    return out;
}

//  One logical node held as a handle into each engine (ids may differ per engine).
struct Pair {
    Node a;
    Node b;
};

//  Applies one pseudo-random operation to two engines in lockstep, asserting
//  that mutating verbs return the same Error and snapshot() the same SnapId.
//  Read-side equivalence (canon/stats/ids) is left to the caller, which owns
//  the interpretation (differ vs determinism vs single-engine invariants).
class Lockstep {
public:
    Lockstep(Factory fa, Factory fb, unsigned seed)
        : m_rng(seed) {
        m_a = fa();
        m_b = fb();
        m_nodes.push_back({m_a->rwRoot(), m_b->rwRoot()});
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
    }

    IPrfs& a() { return *m_a; }

    IPrfs& b() { return *m_b; }

    std::vector<Pair>& nodes() { return m_nodes; }

private:
    size_t pick(size_t n) { return m_rng() % n; }

    std::string name() { return std::string(1, char('a' + pick(5))); }

    std::string blob() { return "v" + std::to_string(pick(100)); }

    Pair& any() { return m_nodes[pick(m_nodes.size())]; }

    //  A random directory (root, index 0, is always one).
    Pair& dir() {
        std::vector<size_t> dirs;
        for (size_t i = 0; i < m_nodes.size(); ++i) {
            if (m_nodes[i].a->type() == Type::DIR) {
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
            p = {m_a->mkfile(c), m_b->mkfile(c)};
            break;
        }
        case Type::DIR:
            p = {m_a->mkdir(), m_b->mkdir()};
            break;
        case Type::LNK: {
            auto tg = blob();
            p = {m_a->symlink(tg), m_b->symlink(tg)};
            break;
        }
        case Type::FIFO:
            p = {m_a->mkfifo(), m_b->mkfifo()};
            break;
        case Type::SOCK:
            p = {m_a->mksock(), m_b->mksock()};
            break;
        default: { // BLK / CHR
            uint32_t maj = uint32_t(pick(8)), min = uint32_t(pick(8));
            p = {m_a->mknod(t, maj, min), m_b->mknod(t, maj, min)};
            break;
        }
        }
        m_nodes.push_back(p);
    }

    void doLink() {
        Pair& d = dir();
        Pair& c = any();
        auto nm = name();
        ASSERT_EQ(m_a->link(d.a, nm, c.a), m_b->link(d.b, nm, c.b)) << "link name=" << nm;
    }

    //  Bias toward a real name (from the dir listing) so unlinks/moves land.
    std::string existingName(Pair& d) {
        auto ents = m_a->readdir(d.a);
        if (!ents.empty() && (m_rng() & 3)) {
            return ents[pick(ents.size())].first;
        }
        return name();
    }

    void doUnlink() {
        Pair& d = dir();
        auto nm = existingName(d);
        ASSERT_EQ(m_a->unlink(d.a, nm), m_b->unlink(d.b, nm)) << "unlink name=" << nm;
    }

    void doMove() {
        Pair& s = dir();
        auto sn = existingName(s);
        Pair& d = dir();
        auto dn = name();
        ASSERT_EQ(m_a->move(s.a, sn, d.a, dn), m_b->move(s.b, sn, d.b, dn))
            << "move " << sn << " -> " << dn;
    }

    void doMutate() {
        Pair& p = any();
        switch (p.a->type()) {
        case Type::REG: {
            auto c = blob();
            ASSERT_EQ(m_a->setContent(p.a, c), m_b->setContent(p.b, c));
            break;
        }
        case Type::LNK: {
            auto t = blob();
            ASSERT_EQ(m_a->setTarget(p.a, t), m_b->setTarget(p.b, t));
            break;
        }
        case Type::BLK:
        case Type::CHR: {
            uint32_t maj = uint32_t(pick(8)), min = uint32_t(pick(8));
            ASSERT_EQ(m_a->setRdev(p.a, maj, min), m_b->setRdev(p.b, maj, min));
            break;
        }
        default:
            break; // nothing mutable on DIR/FIFO/SOCK here
        }
    }

    void doSnapshot() { ASSERT_EQ(m_a->snapshot(), m_b->snapshot()); }

    std::mt19937 m_rng;
    std::unique_ptr<IPrfs> m_a;
    std::unique_ptr<IPrfs> m_b;
    std::vector<Pair> m_nodes;
};

} // namespace prfs::test
