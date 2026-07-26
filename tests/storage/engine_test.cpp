// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  Storage-engine selection via di. Both engines compile and self-register as
//  IStorageEngine providers; openPrfs resolves the active one by name
//  (setStorageEngine), independent of the -Dstorage build default. This proves
//  the engine tier of the di registry end to end.
//
#include "prfs/prfs.hpp"
#include "support.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

using namespace prfs;
using prfs::test::uniqueTempDir;

static std::unique_ptr<IPrfs> open(std::string const& path, bool clean) {
    Options o;
    o.clean = clean;
    return openPrfs(path, o);
}

TEST(StorageEngine, BothRegisteredAndSelectable) {
    EXPECT_NO_THROW(setStorageEngine("memory"));
    EXPECT_NO_THROW(setStorageEngine("lmdb"));
    EXPECT_THROW(setStorageEngine("bogus"), std::out_of_range);
    setStorageEngine("memory"); // reflected in storageEngine()
    EXPECT_EQ(storageEngine(), "memory");
}

TEST(StorageEngine, MemoryIsNonPersistent) {
    setStorageEngine("memory");
    auto path = uniqueTempDir("engine");
    {
        auto fs = open(path.string(), true);
        fs->link(fs->rwRoot(), "x", fs->mkfile("y"));
    }
    auto fs = open(path.string(), false); // reopen — memory keeps nothing
    EXPECT_FALSE(fs->lookup(fs->rwRoot(), "x"));
}

TEST(StorageEngine, LmdbIsPersistent) {
    setStorageEngine("lmdb");
    auto path = uniqueTempDir("engine");
    {
        auto fs = open(path.string(), true);
        fs->link(fs->rwRoot(), "x", fs->mkfile("y"));
    }
    auto fs = open(path.string(), false); // reopen — lmdb persisted it
    ASSERT_TRUE(fs->lookup(fs->rwRoot(), "x"));
    EXPECT_EQ(fs->lookup(fs->rwRoot(), "x")->content(), "y");
}
