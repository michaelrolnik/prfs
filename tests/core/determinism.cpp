// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  Determinism tests (todo S10). Two independent, freshly-opened instances of
//  the same engine, driven by one shared op stream, must remain bit-for-bit
//  equivalent: identical canonical view, identical global stats, identical
//  snapshot ids, and identical node-id allocation. This catches any hidden
//  nondeterminism (wall-clock timestamps, address- or iteration-order leaking
//  into observable output) that a single run would hide. The store must be a
//  pure function of its operation sequence — the reproducibility the tool sells.
//
#include "prfs/memstore.hpp"
#include "prfs/prfs.hpp"
#include "support.hpp"

#include <gtest/gtest.h>

using namespace prfs;
using namespace prfs::test;

namespace {

std::unique_ptr<IPrfs> oracle() { return makeMemStore(); }

std::unique_ptr<IPrfs> backend() {
    Options o;
    o.clean = true;
    return openPrfs(uniqueTempDir("determinism").string(), o);
}

} // namespace

class DeterminismTest : public ::testing::TestWithParam<Factory> {};

TEST_P(DeterminismTest, SameOpsSameState) {
    Factory f = GetParam();

    for (unsigned seed : {1u, 5u, 99u, 12345u}) {
        SCOPED_TRACE("seed " + std::to_string(seed));
        //  Two fresh instances of the SAME engine, one shared op stream.
        Lockstep m(f, f, seed);

        for (int i = 0; i < 1000; ++i) {
            m.step(i);
            ASSERT_EQ(canon(m.a(), m.a().rwRoot()), canon(m.b(), m.b().rwRoot()));
            ASSERT_EQ(statsStr(m.a()), statsStr(m.b()));

            // Node-id allocation is deterministic across fresh instances.
            for (auto const& p : m.nodes()) {
                ASSERT_EQ(p.a->id(), p.b->id());
            }
        }
        ASSERT_EQ(m.a().snapshots(), m.b().snapshots());
    }
}

INSTANTIATE_TEST_SUITE_P(Engines, DeterminismTest, ::testing::Values(&oracle, &backend));
