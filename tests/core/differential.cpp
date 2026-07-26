// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  Differential harness (design §13, todo S9). Drives a pseudo-random op
//  sequence into two engines in lockstep — the independent reference oracle
//  (MemStore) and the selected production backend (openPrfs → PrfsStore over
//  the -Dstorage= engine) — and asserts they stay observably equivalent:
//  identical canonical reachable-graph serialization and global stats after
//  every step (the Lockstep driver already cross-checks error codes and
//  snapshot ids), and identical range-back views for every recorded snapshot.
//
#include "prfs/memstore.hpp"
#include "prfs/prfs.hpp"
#include "support.hpp"

#include <gtest/gtest.h>

using namespace prfs;
using namespace prfs::test;

namespace {

std::unique_ptr<IPrfs> makeOracle() { return makeMemStore(); }

std::unique_ptr<IPrfs> makeBackend() {
    Options o;
    o.clean = true;
    return openPrfs(uniqueTempDir("diff").string(), o);
}

} // namespace

class DiffTest : public ::testing::TestWithParam<unsigned> {};

TEST_P(DiffTest, BackendMatchesOracle) {
    Lockstep m(&makeOracle, &makeBackend, GetParam());

    for (int i = 0; i < 1500; ++i) {
        m.step(i);
        ASSERT_EQ(canon(m.a(), m.a().rwRoot()), canon(m.b(), m.b().rwRoot()));
        ASSERT_EQ(statsStr(m.a()), statsStr(m.b()));
    }

    // Every recorded snapshot view must still agree (range-back reads).
    ASSERT_EQ(m.a().snapshots(), m.b().snapshots());
    for (SnapId s : m.a().snapshots()) {
        SCOPED_TRACE("snapshot view " + std::to_string(s));
        ASSERT_EQ(canon(m.a(), m.a().snapshotRoot(s)), canon(m.b(), m.b().snapshotRoot(s)));
    }
}

INSTANTIATE_TEST_SUITE_P(Seeds, DiffTest,
                         ::testing::Values(1u, 2u, 3u, 7u, 42u, 100u, 2024u, 31337u));
