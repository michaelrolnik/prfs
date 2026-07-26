// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  storage engine: lmdb — a persistent IKvStore over vendored liblmdb
//  (third_party/lmdb). -Dstorage=lmdb (the default).
//
#include "prfs/kvstore.hpp"
#include "prfs/prfs.hpp"

#include <lmdb.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace prfs {
namespace {

char const* kvName(Kv k) {
    switch (k) {
    case Kv::Nodes:
        return "nodes";
    case Kv::DownLinks:
        return "downlinks";
    case Kv::UpLinks:
        return "uplinks";
    case Kv::Changes:
        return "changes";
    case Kv::Meta:
        return "meta";
    default:
        return "";
    }
}

MDB_val mval(std::string_view s) { return MDB_val{s.size(), const_cast<char*>(s.data())}; }

class LmdbCursor : public IKvCursor {
public:
    LmdbCursor(MDB_txn* t, MDB_dbi db) { mdb_cursor_open(t, db, &m_cur); }

    ~LmdbCursor() override { mdb_cursor_close(m_cur); }

    bool seek(std::string_view k) override {
        m_key = mval(k);
        return step(MDB_SET_RANGE);
    }

    bool first() override { return step(MDB_FIRST); }

    bool last() override { return step(MDB_LAST); }

    bool next() override { return step(MDB_NEXT); }

    bool prev() override { return step(MDB_PREV); }

    bool valid() const override { return m_valid; }

    std::string_view key() const override {
        return {static_cast<char const*>(m_key.mv_data), m_key.mv_size};
    }

    std::string_view val() const override {
        return {static_cast<char const*>(m_val.mv_data), m_val.mv_size};
    }

private:
    bool step(MDB_cursor_op op) {
        m_valid = mdb_cursor_get(m_cur, &m_key, &m_val, op) == 0;
        return m_valid;
    }

    MDB_cursor* m_cur = nullptr;
    MDB_val m_key{};
    MDB_val m_val{};
    bool m_valid = false;
};

class LmdbTxn : public IKvTxn {
public:
    LmdbTxn(MDB_env* e, std::array<MDB_dbi, size_t(Kv::COUNT_)> const& dbi, bool write)
        : m_dbi(dbi) {
        mdb_txn_begin(e, nullptr, write ? 0 : MDB_RDONLY, &m_txn);
    }

    ~LmdbTxn() override {
        if (!m_done) {
            mdb_txn_abort(m_txn);
        }
    }

    bool get(Kv k, std::string_view key, std::string& out) override {
        MDB_val kk = mval(key), vv;
        if (mdb_get(m_txn, db(k), &kk, &vv)) {
            return false;
        }
        out.assign(static_cast<char const*>(vv.mv_data), vv.mv_size);
        return true;
    }

    void put(Kv k, std::string_view key, std::string_view v) override {
        MDB_val kk = mval(key), vv = mval(v);
        if (int rc = mdb_put(m_txn, db(k), &kk, &vv, 0)) {
            throw std::runtime_error(std::string("lmdb put: ") + mdb_strerror(rc));
        }
    }

    void del(Kv k, std::string_view key) override {
        MDB_val kk = mval(key);
        mdb_del(m_txn, db(k), &kk, nullptr);
    }

    std::unique_ptr<IKvCursor> cursor(Kv k) override {
        return std::make_unique<LmdbCursor>(m_txn, db(k));
    }

    void commit() override {
        mdb_txn_commit(m_txn);
        m_done = true;
    }

    void abort() override {
        mdb_txn_abort(m_txn);
        m_done = true;
    }

private:
    MDB_dbi db(Kv k) const { return m_dbi[size_t(k)]; }

    MDB_txn* m_txn = nullptr;
    std::array<MDB_dbi, size_t(Kv::COUNT_)> const& m_dbi;
    bool m_done = false;
};

class LmdbKv : public IKvStore {
public:
    LmdbKv(std::string const& path, bool clean) {
        if (clean) {
            std::filesystem::remove_all(path);
        }
        std::filesystem::create_directories(path);

        mdb_env_create(&m_env);
        mdb_env_set_maxdbs(m_env, size_t(Kv::COUNT_));
        mdb_env_set_mapsize(m_env, size_t(1) << 30);
        if (int rc = mdb_env_open(m_env, path.c_str(), 0, 0664)) {
            throw std::runtime_error(std::string("lmdb open: ") + mdb_strerror(rc));
        }

        MDB_txn* t;
        mdb_txn_begin(m_env, nullptr, 0, &t);
        for (size_t i = 0; i < size_t(Kv::COUNT_); ++i) {
            mdb_dbi_open(t, kvName(Kv(i)), MDB_CREATE, &m_dbi[i]);
        }
        mdb_txn_commit(t);
    }

    ~LmdbKv() override { mdb_env_close(m_env); }

    std::unique_ptr<IKvTxn> begin(bool write) override {
        return std::make_unique<LmdbTxn>(m_env, m_dbi, write);
    }

private:
    MDB_env* m_env = nullptr;
    std::array<MDB_dbi, size_t(Kv::COUNT_)> m_dbi{};
};

} // namespace

std::unique_ptr<IKvStore> makeLmdbKv(std::string const& path, bool clean) {
    return std::make_unique<LmdbKv>(path, clean);
}

std::unique_ptr<IPrfs> openPrfs(std::string const& path, Options const& opts) {
    return makePrfsStore(makeLmdbKv(path, opts.clean));
}

} // namespace prfs
