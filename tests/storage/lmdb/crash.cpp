// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  Crash-safety / durability (todo S10). PrfsStore commits every operation in
//  its own KV transaction, so dropping the store object (no clean close beyond
//  the destructor) models a process death after committed writes: reopening the
//  same path must recover exactly the committed state — live view, global stats,
//  and every historical snapshot's range-back view. Subsequent writes after a
//  reopen must themselves persist across a second reopen.
//
//  Only meaningful for a persistent backend; a runtime probe skips the
//  in-memory engine (openPrfs there ignores the path).
//
#include "prfs/memstore.hpp"
#include "prfs/prfs.hpp"
#include "support.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace prfs;
using namespace prfs::test;

namespace {

std::unique_ptr<IPrfs> open(std::filesystem::path const& p, bool clean) {
    Options o;
    o.clean = clean;
    return openPrfs(p.string(), o);
}

//  True if committed writes survive dropping and reopening the store.
bool persistent(std::filesystem::path const& p) {
    {
        auto fs = open(p, true);
        fs->link(fs->rwRoot(), "probe", fs->mkfile("x"));
        fs->snapshot();
    }
    auto fs = open(p, false);
    return fs->lookup(fs->rwRoot(), "probe") != nullptr;
}

} // namespace

TEST(CrashSafety, CommittedStateSurvivesReopen) {
    if (!persistent(uniqueTempDir("probe"))) {
        GTEST_SKIP() << "non-persistent backend";
    }

    auto path = uniqueTempDir("crash");
    std::string view;
    std::string stats;
    std::vector<SnapId> snaps;
    std::vector<std::string> snapViews;
    uint64_t clock = 0;

    //  Build a non-trivial history, then drop the store (== crash) without any
    //  explicit shutdown beyond object destruction.
    {
        Lockstep m([&] { return open(path, true); }, [] { return makeMemStore(); }, 4242);
        for (int i = 0; i < 700; ++i) {
            m.step(i);
        }
        m.a().setTime(1234567); // the logical clock must survive too
        clock = m.a().now();
        view = canon(m.a(), m.a().rwRoot());
        stats = statsStr(m.a());
        snaps = m.a().snapshots();
        for (SnapId s : snaps) {
            snapViews.push_back(canon(m.a(), m.a().snapshotRoot(s)));
        }
    }

    //  Reopen (clean=false): everything committed must be back.
    {
        auto fs = open(path, false);
        EXPECT_EQ(view, canon(*fs, fs->rwRoot())) << "live view lost across reopen";
        EXPECT_EQ(stats, statsStr(*fs)) << "counters lost across reopen";
        EXPECT_EQ(clock, fs->now()) << "logical clock lost across reopen";
        ASSERT_EQ(snaps, fs->snapshots());
        for (size_t i = 0; i < snaps.size(); ++i) {
            SCOPED_TRACE("snapshot view " + std::to_string(snaps[i]));
            EXPECT_EQ(snapViews[i], canon(*fs, fs->snapshotRoot(snaps[i])));
        }

        //  Writes after a reopen must also persist.
        EXPECT_EQ(fs->link(fs->rwRoot(), "postcrash", fs->mkfile("later")), Error::OK);
    }

    //  Second reopen sees the post-reopen write.
    {
        auto fs = open(path, false);
        auto n = fs->lookup(fs->rwRoot(), "postcrash");
        ASSERT_TRUE(n) << "write after reopen did not persist";
        EXPECT_EQ(n->content(), "later");
    }
}
