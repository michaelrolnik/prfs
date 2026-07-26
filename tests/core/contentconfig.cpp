// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  FS content-policy blob (design §11.2, todo L1). The store persists an opaque
//  ContentConfig blob in meta and never parses it — the content provider owns
//  the format. Verified on both engines (persistence across reopen is in the
//  crash suite).
//
#include "prfs/memstore.hpp"
#include "prfs/prfs.hpp"
#include "support.hpp"

#include <gtest/gtest.h>

using namespace prfs;
using prfs::test::Factory;
using prfs::test::uniqueTempDir;

static std::unique_ptr<IPrfs> oracle() { return makeMemStore(); }

static std::unique_ptr<IPrfs> backend() {
    Options o;
    o.clean = true;
    return openPrfs(uniqueTempDir("contentcfg").string(), o);
}

class ContentConfigTest : public ::testing::TestWithParam<Factory> {};

TEST_P(ContentConfigTest, EmptyByDefaultThenRoundTrips) {
    auto fs = GetParam()();
    EXPECT_EQ(fs->contentConfig(), "");

    std::string blob(16, '\0'); // binary blob incl. embedded NULs — stored verbatim
    blob[0] = 'P';
    blob[4] = char(0x10);
    blob[8] = char(0xff);
    fs->setContentConfig(blob);
    EXPECT_EQ(fs->contentConfig(), blob);
    EXPECT_EQ(fs->contentConfig().size(), 16u);

    fs->setContentConfig(""); // clearing works too
    EXPECT_EQ(fs->contentConfig(), "");
}

INSTANTIATE_TEST_SUITE_P(Engines, ContentConfigTest, ::testing::Values(&oracle, &backend));
