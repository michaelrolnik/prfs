// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  openPrfs — engine-agnostic store factory. It resolves the active storage
//  engine (a di IStorageEngine provider, selected by name) and builds the one
//  PrfsStore over the IKvStore it opens. The engines self-register (memory/kv.cpp,
//  lmdb/kv.cpp); this file names no engine.
//
#include "prfs/di.hpp"
#include "prfs/kvstore.hpp"
#include "prfs/prfs.hpp"

#include <stdexcept>
#include <string>

//  Build-time default engine name (meson -Dstorage=…).
#ifndef PRFS_DEFAULT_STORAGE
#define PRFS_DEFAULT_STORAGE "lmdb"
#endif

namespace prfs {

static std::string s_engine;

std::string storageEngine() { return s_engine.empty() ? PRFS_DEFAULT_STORAGE : s_engine; }

void setStorageEngine(std::string_view name) {
    if (!di::global().has(IStorageEngine::ID, name)) {
        throw std::out_of_range("prfs: unknown storage engine '" + std::string(name) + "'");
    }
    s_engine = std::string(name);
}

std::unique_ptr<IPrfs> openPrfs(std::string const& path, Options const& opts) {
    IStorageEngine const& engine = di::global().resolve<IStorageEngine>(storageEngine());
    return makePrfsStore(engine.open(path, opts.clean));
}

} // namespace prfs
