// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  storage engine: memory — an in-memory ordered-map IKvStore.
//  Exercises the real PrfsStore logic without persistence (and is distinct from
//  the reference oracle, so differential testing is meaningful). -Dstorage=memory.
//
#include "prfs/kvstore.hpp"
#include "prfs/prfs.hpp"

#include <array>
#include <map>

namespace prfs {
namespace {

using Map = std::map<std::string, std::string, std::less<>>;

struct Data {
    std::array<Map, size_t(Kv::COUNT_)> store;
};

class MemCursor : public IKvCursor {
public:
    explicit MemCursor(Map& m)
        : m_map(m)
        , m_it(m.end()) {}

    bool seek(std::string_view k) override {
        m_it = m_map.lower_bound(k);
        return m_it != m_map.end();
    }

    bool first() override {
        m_it = m_map.begin();
        return m_it != m_map.end();
    }

    bool last() override {
        if (m_map.empty()) {
            m_it = m_map.end();
            return false;
        }
        m_it = std::prev(m_map.end());
        return true;
    }

    bool next() override {
        if (m_it != m_map.end()) {
            ++m_it;
        }
        return m_it != m_map.end();
    }

    bool prev() override {
        if (m_it == m_map.begin()) {
            m_it = m_map.end();
            return false;
        }
        --m_it;
        return true;
    }

    bool valid() const override { return m_it != m_map.end(); }

    std::string_view key() const override { return m_it->first; }

    std::string_view val() const override { return m_it->second; }

private:
    Map& m_map;
    Map::iterator m_it;
};

//  Single-writer, no isolation: writes apply directly. PrfsStore validates
//  before writing, so error paths never leave partial state (no rollback needed).
class MemTxn : public IKvTxn {
public:
    explicit MemTxn(Data* d)
        : m_data(d) {}

    bool get(Kv k, std::string_view key, std::string& out) override {
        auto& m = s(k);
        auto it = m.find(key);
        if (it == m.end()) {
            return false;
        }
        out = it->second;
        return true;
    }

    void put(Kv k, std::string_view key, std::string_view v) override {
        s(k)[std::string(key)] = std::string(v);
    }

    void del(Kv k, std::string_view key) override {
        auto& m = s(k);
        auto it = m.find(key);
        if (it != m.end()) {
            m.erase(it);
        }
    }

    std::unique_ptr<IKvCursor> cursor(Kv k) override { return std::make_unique<MemCursor>(s(k)); }

    void commit() override {}

    void abort() override {}

private:
    Map& s(Kv k) { return m_data->store[size_t(k)]; }

    Data* m_data;
};

class MemKv : public IKvStore {
public:
    std::unique_ptr<IKvTxn> begin(bool) override { return std::make_unique<MemTxn>(&m_data); }

private:
    Data m_data;
};

} // namespace

std::unique_ptr<IKvStore> makeMemKv() { return std::make_unique<MemKv>(); }

std::unique_ptr<IPrfs> openPrfs(std::string const& /*path*/, Options const& /*opts*/) {
    return makePrfsStore(makeMemKv());
}

} // namespace prfs
