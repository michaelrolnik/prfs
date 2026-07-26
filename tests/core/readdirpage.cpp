// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  Paginated readdir with a stable cursor (design §6.2, todo T4 / bug B4). NFS
//  READDIR is paginated and the live directory can change between calls. The
//  cursor is the last entry's name, so it is stable across concurrent
//  add/remove: every entry present for the whole scan is returned exactly once,
//  an entry added ahead of the cursor appears later, and one removed before the
//  cursor reaches it is not returned. Verified on both engines.
//
#include "prfs/memstore.hpp"
#include "prfs/prfs.hpp"
#include "support.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace prfs;
using prfs::test::Factory;
using prfs::test::uniqueTempDir;

namespace {

std::unique_ptr<IPrfs> oracle() { return makeMemStore(); }

std::unique_ptr<IPrfs> backend() {
    Options o;
    o.clean = true;
    return openPrfs(uniqueTempDir("readdirpage").string(), o);
}

} // namespace

class ReaddirPageTest : public ::testing::TestWithParam<Factory> {
protected:
    void SetUp() override { fs = GetParam()(); }

    Node linkFile(Node dir, std::string const& name) {
        auto f = fs->mkfile(name);
        EXPECT_EQ(fs->link(dir, name, f), Error::OK);
        return f;
    }

    //  Drain the directory `max` entries at a time, collecting names.
    std::vector<std::string> pageAll(Node dir, size_t max) {
        std::vector<std::string> names;
        std::string cookie;
        for (;;) {
            DirPage p = fs->readdirPage(dir, cookie, max);
            for (auto const& [name, node] : p.entries) {
                names.push_back(name);
            }
            if (p.eof) {
                break;
            }
            cookie = p.cookie;
        }
        return names;
    }

    std::unique_ptr<IPrfs> fs;
};

TEST_P(ReaddirPageTest, EmptyDirectory) {
    DirPage p = fs->readdirPage(fs->rwRoot(), "", 10);
    EXPECT_TRUE(p.entries.empty());
    EXPECT_TRUE(p.eof);
}

TEST_P(ReaddirPageTest, PagesReassembleTheFullListing) {
    auto root = fs->rwRoot();
    for (char c = 'a'; c <= 'j'; ++c) {
        linkFile(root, std::string(1, c));
    }

    std::vector<std::string> expected;
    for (auto const& [name, node] : fs->readdir(root)) {
        expected.push_back(name);
    }
    EXPECT_EQ(pageAll(root, 3), expected);   // page size 3 over 10 entries
    EXPECT_EQ(pageAll(root, 1), expected);   // one at a time
    EXPECT_EQ(pageAll(root, 100), expected); // all in one page
}

TEST_P(ReaddirPageTest, EofIsSetAsSoonAsTheEndIsKnown) {
    auto root = fs->rwRoot();
    linkFile(root, "a");
    linkFile(root, "b");
    linkFile(root, "c");

    // A page that exactly consumes the rest reports eof in the same call — no
    // wasteful extra empty round-trip.
    DirPage p = fs->readdirPage(root, "", 3);
    EXPECT_EQ(p.entries.size(), 3u);
    EXPECT_TRUE(p.eof);

    // A full page with entries still remaining is not eof; the next page is.
    DirPage q1 = fs->readdirPage(root, "", 2);
    EXPECT_EQ(q1.entries.size(), 2u);
    EXPECT_FALSE(q1.eof);
    DirPage q2 = fs->readdirPage(root, q1.cookie, 2);
    EXPECT_EQ(q2.entries.size(), 1u); // just "c"
    EXPECT_TRUE(q2.eof);
}

TEST_P(ReaddirPageTest, EntryAddedAheadOfCursorAppearsLater) {
    auto root = fs->rwRoot();
    linkFile(root, "a");
    linkFile(root, "b");
    linkFile(root, "d");

    DirPage p1 = fs->readdirPage(root, "", 2); // [a, b], cursor "b"
    ASSERT_EQ(p1.entries.size(), 2u);
    EXPECT_EQ(p1.cookie, "b");

    linkFile(root, "c"); // inserted ahead of the cursor (b < c)

    auto rest = std::vector<std::string>{};
    for (auto const& [name, node] : fs->readdirPage(root, p1.cookie, 10).entries) {
        rest.push_back(name);
    }
    EXPECT_EQ(rest, (std::vector<std::string>{"c", "d"})); // c shows up, once
}

TEST_P(ReaddirPageTest, EntryAddedBehindCursorIsNotRevisited) {
    auto root = fs->rwRoot();
    linkFile(root, "b");
    linkFile(root, "c");

    DirPage p1 = fs->readdirPage(root, "", 1); // [b], cursor "b"
    ASSERT_EQ(p1.entries.size(), 1u);

    linkFile(root, "a"); // inserted behind the cursor (a < b) — already passed

    auto rest = std::vector<std::string>{};
    for (auto const& [name, node] : fs->readdirPage(root, p1.cookie, 10).entries) {
        rest.push_back(name);
    }
    EXPECT_EQ(rest, (std::vector<std::string>{"c"})); // "a" is not revisited
}

TEST_P(ReaddirPageTest, EntryRemovedAheadOfCursorIsNotReturned) {
    auto root = fs->rwRoot();
    for (char c = 'a'; c <= 'd'; ++c) {
        linkFile(root, std::string(1, c));
    }

    DirPage p1 = fs->readdirPage(root, "", 2); // [a, b]
    ASSERT_EQ(p1.entries.size(), 2u);

    EXPECT_EQ(fs->unlink(root, "c"), Error::OK); // remove ahead of the cursor

    auto rest = std::vector<std::string>{};
    for (auto const& [name, node] : fs->readdirPage(root, p1.cookie, 10).entries) {
        rest.push_back(name);
    }
    EXPECT_EQ(rest, (std::vector<std::string>{"d"})); // c gone, resume still valid
}

TEST_P(ReaddirPageTest, RemovedCursorEntryStillResumes) {
    auto root = fs->rwRoot();
    for (char c = 'a'; c <= 'd'; ++c) {
        linkFile(root, std::string(1, c));
    }

    DirPage p1 = fs->readdirPage(root, "", 2);   // [a, b], cursor "b"
    EXPECT_EQ(fs->unlink(root, "b"), Error::OK); // the cursor entry itself is gone

    auto rest = std::vector<std::string>{};
    for (auto const& [name, node] : fs->readdirPage(root, p1.cookie, 10).entries) {
        rest.push_back(name);
    }
    EXPECT_EQ(rest, (std::vector<std::string>{"c", "d"})); // resumes after "b" fine
}

TEST_P(ReaddirPageTest, SnapshotDirPaginates) {
    auto root = fs->rwRoot();
    linkFile(root, "f");
    fs->snapshot();
    fs->snapshot();
    fs->snapshot(); // snaps 1, 2, 3

    auto snap = fs->lookup(root, SNAPSHOT_NAME);
    EXPECT_EQ(pageAll(snap, 2), (std::vector<std::string>{"1", "2", "3"}));
}

INSTANTIATE_TEST_SUITE_P(Engines, ReaddirPageTest, ::testing::Values(&oracle, &backend));
