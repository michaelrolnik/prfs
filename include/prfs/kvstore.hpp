// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

#pragma once
//
//  IKvStore — the storage abstraction under PrfsStore (design §4).
//
//  An ordered key/value store with a fixed set of named sub-stores, MVCC-style
//  transactions, and cursors. All the versioned-filesystem logic lives in
//  PrfsStore and is written once against this interface; each engine (LMDB,
//  rocksdb, in-memory, …) only implements IKvStore.
//
//  Keys/values are opaque byte strings; PrfsStore owns the encoding (big-endian
//  composite keys). Values returned by get()/cursor are valid only for the
//  lifetime of the transaction (they may point into an mmap) — copy to keep.
//
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace prfs {

//  fixed sub-stores (LMDB dbis / rocksdb column families / separate maps)
enum class Kv { Nodes, DownLinks, UpLinks, Changes, Meta, Snaps, COUNT_ };

class IKvCursor {
public:
    virtual ~IKvCursor() = default;

    virtual bool seek(std::string_view key) = 0; // position at first key >= key
    virtual bool first() = 0;
    virtual bool last() = 0;
    virtual bool next() = 0;
    virtual bool prev() = 0;
    virtual bool valid() const = 0;

    virtual std::string_view key() const = 0;
    virtual std::string_view val() const = 0;
};

class IKvTxn {
public:
    virtual ~IKvTxn() = default;

    virtual bool get(Kv store, std::string_view key, std::string& out) = 0;
    virtual void put(Kv store, std::string_view key, std::string_view val) = 0; // write txn
    virtual void del(Kv store, std::string_view key) = 0;                       // write txn
    virtual std::unique_ptr<IKvCursor> cursor(Kv store) = 0;

    virtual void commit() = 0; // no-op for read txns
    virtual void abort() = 0;
};

class IKvStore {
public:
    virtual ~IKvStore() = default;

    virtual std::unique_ptr<IKvTxn> begin(bool write) = 0;
};

//  the one FS implementation, over any engine
std::unique_ptr<class IPrfs> makePrfsStore(std::unique_ptr<IKvStore> kv);

//  engine factories (defined by the selected backend)
std::unique_ptr<IKvStore> makeLmdbKv(std::string const& path, bool clean);
std::unique_ptr<IKvStore> makeMemKv();

} // namespace prfs
