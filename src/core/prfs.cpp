// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  PrfsStore — the one versioned-filesystem implementation of IPrfs, written
//  against IKvStore (design §5/§6). Engines (LMDB, memory, …) only implement
//  IKvStore; all the node/link/snapshot/diff logic lives here.
//
//  Model: copy-on-write per directory-version link sets; per-node range-back
//  reads. Mirrors the reference oracle's observable semantics.
//
#include "prfs/kvstore.hpp"
#include "prfs/prfs.hpp"

#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <vector>

namespace prfs {

// ---- key encoding --------------------------------------------------------
static std::string be(uint64_t x) {
    std::string s(8, '\0');

    for (int i = 7; i >= 0; --i) {
        s[i] = char(x & 0xff);
        x >>= 8;
    }
    return s;
}

static uint64_t rdbe(char const* p) {
    auto b = reinterpret_cast<unsigned char const*>(p);
    uint64_t x = 0;

    for (int i = 0; i < 8; ++i) {
        x = (x << 8) | b[i];
    }
    return x;
}

// ---- synthesized .snapshot dir (design §3.2) -----------------------------
// A node id with the top bit set is the virtual snapshot-list directory of the
// node in the low 63 bits — a real, filehandle-round-trippable id in a reserved
// id-space (real ids come from a small counter, so the bit is always free).
static constexpr uint64_t SNAP_BIT = uint64_t(1) << 63;

static bool isSnapDir(uint64_t id) { return (id & SNAP_BIT) != 0; }

static uint64_t snapBase(uint64_t id) { return id & ~SNAP_BIT; }

//  The record/store types are file-local; the anonymous namespace gives them
//  internal linkage (a type can't be `static`).
namespace {

struct NodeRec {
    uint32_t type = 0, mode = 0, uid = 0, gid = 0, nlink = 0;
    uint64_t size = 0, atime = 0, mtime = 0, ctime = 0, spec = 0, dnLinkVer = 0, upLinkVer = 0;
    std::string blob;
};

std::string serialize(NodeRec const& r) {
    std::string s;
    auto u32 = [&](uint32_t v) { s.append(reinterpret_cast<char const*>(&v), 4); };
    auto u64 = [&](uint64_t v) { s.append(reinterpret_cast<char const*>(&v), 8); };

    u32(r.type);
    u32(r.mode);
    u32(r.uid);
    u32(r.gid);
    u32(r.nlink);
    u64(r.size);
    u64(r.atime);
    u64(r.mtime);
    u64(r.ctime);
    u64(r.spec);
    u64(r.dnLinkVer);
    u64(r.upLinkVer);
    s.append(r.blob);
    return s;
}

NodeRec deserialize(std::string_view v) {
    NodeRec r;
    char const* p = v.data();
    size_t off = 0;
    auto u32 = [&](uint32_t& x) {
        memcpy(&x, p + off, 4);
        off += 4;
    };
    auto u64 = [&](uint64_t& x) {
        memcpy(&x, p + off, 8);
        off += 8;
    };

    u32(r.type);
    u32(r.mode);
    u32(r.uid);
    u32(r.gid);
    u32(r.nlink);
    u64(r.size);
    u64(r.atime);
    u64(r.mtime);
    u64(r.ctime);
    u64(r.spec);
    u64(r.dnLinkVer);
    u64(r.upLinkVer);
    r.blob.assign(p + off, v.size() - off);
    return r;
}

class PrfsStore;

class PrfsNode : public INode {
public:
    PrfsNode(PrfsStore* s, uint64_t id, SnapId snap)
        : m_store(s)
        , m_id(id)
        , m_snap(snap) {}

    uint64_t id() const override { return m_id; }

    SnapId snap() const override { return m_snap; }

    Type type() const override;

    uint32_t mode() const override;
    void mode(uint32_t) override;
    uint32_t nlink() const override;
    uint32_t uid() const override;
    void uid(uint32_t) override;
    uint32_t gid() const override;
    void gid(uint32_t) override;
    uint64_t size() const override;
    void size(uint64_t) override;
    uint64_t atime() const override;
    void atime(uint64_t) override;
    uint64_t mtime() const override;
    void mtime(uint64_t) override;
    uint64_t ctime() const override;
    void ctime(uint64_t) override;

    std::string target() const override;
    std::pair<uint32_t, uint32_t> rdev() const override;
    std::string content() const override;

private:
    PrfsStore* m_store;
    uint64_t m_id;
    SnapId m_snap;
};

class PrfsStore : public IPrfs {
    friend class PrfsNode;

public:
    explicit PrfsStore(std::unique_ptr<IKvStore> kv)
        : m_kv(std::move(kv)) {
        auto w = m_kv->begin(true);

        if (!getMeta(w.get(), "root", m_root)) {
            m_root = 1;
            m_next = 2;
            m_cur = 1;

            NodeRec r;
            r.type = uint32_t(Type::DIR);
            r.mode = 0755;
            r.nlink = 1;
            r.dnLinkVer = 1;
            r.upLinkVer = 1;
            r.atime = r.mtime = r.ctime = m_clock;

            storeNode(w.get(), m_root, m_cur, r);
            putMeta(w.get(), "root", m_root);
            putMeta(w.get(), "next_node", m_next);
            putMeta(w.get(), "cur_snap", m_cur);
            putMeta(w.get(), "clock", m_clock);
        } else {
            getMeta(w.get(), "next_node", m_next);
            getMeta(w.get(), "cur_snap", m_cur);
            getMeta(w.get(), "clock", m_clock);
        }
        w->commit();
    }

    //  Logical clock (design §3): deterministic and script-driven, never
    //  wall-clock. now() reads it; setTime() is the only thing that advances it.
    uint64_t now() const override { return m_clock; }

    void setTime(uint64_t t) override {
        m_clock = t;

        auto w = m_kv->begin(true);
        putMeta(w.get(), "clock", m_clock);
        w->commit();
    }

    Node rwRoot() override { return handle(m_root, LATEST); }

    Node snapshotRoot(SnapId n) override { return handle(m_root, n); }

    std::vector<SnapId> snapshots() const override {
        std::vector<SnapId> out;

        for (SnapId s = 1; s < m_cur; ++s) {
            out.push_back(s);
        }
        return out;
    }

    Node mkdir() override { return alloc(Type::DIR, "", 0); }

    Node mkfile(std::string const& c) override { return alloc(Type::REG, c, 0); }

    Node symlink(std::string const& t) override { return alloc(Type::LNK, t, 0); }

    Node mkfifo() override { return alloc(Type::FIFO, "", 0); }

    Node mksock() override { return alloc(Type::SOCK, "", 0); }

    Node mknod(Type t, uint32_t maj, uint32_t min) override {
        return alloc(t, "", (uint64_t(maj) << 32) | min);
    }

    Node lookup(Node dir, std::string const& name) override {
        auto r = m_kv->begin(false);

        //  Inside a synthesized .snapshot dir: the name is a snapId N →
        //  the base node viewed at snapshot N.
        if (isSnapDir(dir->id())) {
            uint64_t base = snapBase(dir->id());
            SnapId n;
            if (!parseSnap(name, n) || n < 1 || n >= m_cur || !existedAt(r.get(), base, n)) {
                return nullptr;
            }
            return handle(base, n);
        }

        NodeRec dr;
        if (!loadNode(r.get(), dir->id(), rr(dir->snap()), dr)) {
            return nullptr;
        }

        //  The virtual .snapshot dir is synthesized only in a *live* directory
        //  view — never inside an already-frozen one (no /.snapshot/5/.snapshot).
        if (name == SNAPSHOT_NAME && dir->snap() == LATEST && dr.type == uint32_t(Type::DIR)) {
            return handle(dir->id() | SNAP_BIT, LATEST);
        }

        std::string child;
        if (!r->get(Kv::DownLinks, be(dir->id()) + be(dr.dnLinkVer) + name, child)) {
            return nullptr;
        }
        return handle(rdbe(child.data()), dir->snap());
    }

    Error link(Node dir, std::string const& name, Node child) override {
        auto w = m_kv->begin(true);
        Error e = linkT(w.get(), dir->id(), name, child->id());

        if (e == Error::OK) {
            w->commit();
        }
        return e;
    }

    Error unlink(Node dir, std::string const& name) override {
        auto w = m_kv->begin(true);
        Error e = unlinkT(w.get(), dir->id(), name);

        if (e == Error::OK) {
            w->commit();
        }
        return e;
    }

    Error move(Node sdir, std::string const& sn, Node ddir, std::string const& dn) override {
        auto w = m_kv->begin(true);
        NodeRec sr;

        if (!loadNode(w.get(), sdir->id(), m_cur, sr)) {
            return Error::NOENT;
        }

        std::string child;
        if (!w->get(Kv::DownLinks, be(sdir->id()) + be(sr.dnLinkVer) + sn, child)) {
            return Error::NOENT;
        }

        Error e = linkT(w.get(), ddir->id(), dn, rdbe(child.data()));
        if (e != Error::OK) {
            return e;
        }
        e = unlinkT(w.get(), sdir->id(), sn);
        if (e != Error::OK) {
            return e;
        }
        w->commit();
        return Error::OK;
    }

    Error setContent(Node reg, std::string const& c) override { return setBlob(reg, Type::REG, c); }

    Error setTarget(Node lnk, std::string const& t) override { return setBlob(lnk, Type::LNK, t); }

    Error setRdev(Node dev, uint32_t maj, uint32_t min) override {
        auto w = m_kv->begin(true);
        NodeRec r;

        if (!loadNode(w.get(), dev->id(), m_cur, r)) {
            return Error::NOENT;
        }
        if (r.type != uint32_t(Type::BLK) && r.type != uint32_t(Type::CHR)) {
            return Error::INVAL;
        }
        r.spec = (uint64_t(maj) << 32) | min;
        storeNode(w.get(), dev->id(), m_cur, r);
        w->commit();
        return Error::OK;
    }

    std::vector<std::pair<std::string, Node>> readdir(Node dir) override {
        std::vector<std::pair<std::string, Node>> out;
        auto r = m_kv->begin(false);

        //  Listing a .snapshot dir: one entry "N" per snapshot the base existed
        //  at, each resolving to the base viewed at N.
        if (isSnapDir(dir->id())) {
            uint64_t base = snapBase(dir->id());
            for (SnapId n = 1; n < m_cur; ++n) {
                if (existedAt(r.get(), base, n)) {
                    out.push_back({std::to_string(n), handle(base, n)});
                }
            }
            return out;
        }

        NodeRec dr;
        if (!loadNode(r.get(), dir->id(), rr(dir->snap()), dr)) {
            return out;
        }
        scanDownlinks(r.get(), dir->id(), dr.dnLinkVer, [&](std::string const& n, uint64_t c) {
            out.push_back({n, handle(c, dir->snap())});
        });
        //  .snapshot is resolvable by name but hidden from readdir by default,
        //  so a tree-walking archiver does not recurse into every snapshot.
        return out;
    }

    DirPage readdirPage(Node dir, std::string const& after, size_t max) override {
        DirPage page;
        auto r = m_kv->begin(false);

        //  .snapshot dir: entries "N" paginate by snapId (numeric cursor).
        if (isSnapDir(dir->id())) {
            uint64_t base = snapBase(dir->id());
            SnapId start = 1;
            SnapId c;
            if (!after.empty() && parseSnap(after, c)) {
                start = c + 1;
            }
            for (SnapId n = start; n < m_cur; ++n) {
                if (!existedAt(r.get(), base, n)) {
                    continue;
                }
                if (page.entries.size() == max) {
                    return page; // eof stays false: more remain
                }
                page.entries.push_back({std::to_string(n), handle(base, n)});
                page.cookie = std::to_string(n);
            }
            page.eof = true;
            return page;
        }

        NodeRec dr;
        if (!loadNode(r.get(), dir->id(), rr(dir->snap()), dr)) {
            page.eof = true;
            return page;
        }

        //  Normal dir: name cursor over the ordered down-link set. A name-keyed
        //  cursor is stable across add/remove — no cookie verifier needed.
        std::string pfx = be(dir->id()) + be(dr.dnLinkVer);
        auto cur = r->cursor(Kv::DownLinks);
        for (bool ok = cur->seek(pfx + after); ok; ok = cur->next()) {
            auto k = cur->key();
            if (k.size() < 16 || memcmp(k.data(), pfx.data(), 16) != 0) {
                break; // left this directory's key range
            }
            std::string name(k.data() + 16, k.size() - 16);
            if (name <= after) {
                continue; // skip the cursor's own (or an earlier) entry
            }
            if (page.entries.size() == max) {
                return page; // eof stays false: at least one more entry exists
            }
            page.entries.push_back({name, handle(rdbe(cur->val().data()), dir->snap())});
            page.cookie = name;
        }
        page.eof = true;
        return page;
    }

    std::vector<Node> parents(Node node) override {
        std::vector<Node> out;
        std::set<uint64_t> seen;
        auto r = m_kv->begin(false);
        NodeRec nr;

        if (!loadNode(r.get(), node->id(), rr(node->snap()), nr)) {
            return out;
        }

        std::string pfx = be(node->id()) + be(nr.upLinkVer);
        auto c = r->cursor(Kv::UpLinks);
        for (bool ok = c->seek(pfx); ok; ok = c->next()) {
            auto k = c->key();
            if (k.size() < 16 || memcmp(k.data(), pfx.data(), 16) != 0) {
                break;
            }
            uint64_t container = rdbe(k.data() + 16);
            if (seen.insert(container).second) {
                out.push_back(handle(container, node->snap()));
            }
        }
        return out;
    }

    SnapId snapshot(std::string const& label) override {
        SnapId v = m_cur;
        m_cur = v + 1;
        auto w = m_kv->begin(true);

        putMeta(w.get(), "cur_snap", m_cur);
        //  snaps: snapId → { ctime(8) ‖ label } (design §3.2)
        w->put(Kv::Snaps, be(v), be(m_clock) + label);
        w->commit();
        return v;
    }

    SnapInfo snapInfo(SnapId g) const override {
        SnapInfo out;
        out.id = g;

        auto r = m_kv->begin(false);
        std::string rec;
        if (r->get(Kv::Snaps, be(g), rec) && rec.size() >= 8) {
            out.ctime = rdbe(rec.data());
            out.label.assign(rec.data() + 8, rec.size() - 8);
        }
        return out;
    }

    std::vector<NodeDiff> diffNodes(SnapId a, SnapId b) override {
        SnapId ra = rr(a), rb = rr(b);
        std::set<uint64_t> cand;
        std::vector<NodeDiff> out;
        auto r = m_kv->begin(false);

        candidates(r.get(), ra, rb, cand);
        for (uint64_t id : cand) {
            NodeRec va, vb;
            bool eA = loadNode(r.get(), id, ra, va) && va.nlink > 0;
            bool eB = loadNode(r.get(), id, rb, vb) && vb.nlink > 0;

            if (!eA && !eB) {
                continue;
            }
            if (!eA && eB) {
                out.push_back({id, NodeChange::CREATED});
                continue;
            }
            if (eA && !eB) {
                out.push_back({id, NodeChange::REMOVED});
                continue;
            }
            bool content = va.blob != vb.blob || va.spec != vb.spec || va.size != vb.size;
            bool attrs =
                va.mode != vb.mode || va.uid != vb.uid || va.gid != vb.gid || va.mtime != vb.mtime;
            if (content) {
                out.push_back({id, NodeChange::MODIFIED_CONTENT});
            } else if (attrs) {
                out.push_back({id, NodeChange::MODIFIED_ATTRS});
            }
        }
        return out;
    }

    std::vector<PathDiff> diffPaths(SnapId a, SnapId b) override {
        SnapId ra = rr(a), rb = rr(b);
        std::set<uint64_t> cand;
        std::vector<PathDiff> out;
        auto r = m_kv->begin(false);

        candidates(r.get(), ra, rb, cand);
        for (uint64_t dir : cand) {
            std::map<std::string, uint64_t> sa, sb;
            linkSet(r.get(), dir, ra, sa);
            linkSet(r.get(), dir, rb, sb);

            for (auto const& [name, child] : sb) {
                if (!sa.count(name)) {
                    out.push_back({PathChange::ADDED, dir, name, child});
                }
            }
            for (auto const& [name, child] : sa) {
                if (!sb.count(name)) {
                    out.push_back({PathChange::REMOVED, dir, name, child});
                }
            }
        }
        return out;
    }

    Stats stats(SnapId g) const override {
        SnapId rg = g == LATEST ? m_cur : g;
        Stats s;
        auto r = m_kv->begin(false);

        // node counts: only nodes with an incoming link are "alive" (design §9)
        eachEffNode(r.get(), rg, [&](uint64_t, NodeRec const& n) {
            if (n.nlink == 0) {
                return;
            }
            s.nodes[n.type]++;
            if (n.type == uint32_t(Type::REG)) {
                s.totalSize += n.size;
            }
        });
        // live links: every live down-link counts (links±1 per link/unlink,
        // design §9) — including entries inside now-orphaned dirs (nlink==0).
        eachEffNode(r.get(), rg, [&](uint64_t id, NodeRec const& n) {
            if (n.type == uint32_t(Type::DIR)) {
                scanDownlinks(r.get(), id, n.dnLinkVer,
                              [&](std::string const&, uint64_t) { s.links++; });
            }
        });
        return s;
    }

private:
    // ---- helpers ----
    SnapId rr(SnapId g) const { return g == LATEST ? m_cur : g; }

    Node handle(uint64_t id, SnapId snap) { return std::make_shared<PrfsNode>(this, id, snap); }

    bool getMeta(IKvTxn* t, char const* key, uint64_t& out) const {
        std::string v;
        if (!t->get(Kv::Meta, key, v)) {
            return false;
        }
        memcpy(&out, v.data(), 8);
        return true;
    }

    void putMeta(IKvTxn* t, char const* key, uint64_t val) {
        t->put(Kv::Meta, key, std::string_view(reinterpret_cast<char const*>(&val), 8));
    }

    bool loadNode(IKvTxn* t, uint64_t id, SnapId rg, NodeRec& out) const {
        auto c = t->cursor(Kv::Nodes);
        std::string key = be(id) + be(rg);
        bool pos = c->seek(key);

        if (pos) {
            auto k = c->key();
            if (!(k.size() == 16 && rdbe(k.data()) == id && rdbe(k.data() + 8) == rg)) {
                pos = c->prev();
            }
        } else {
            pos = c->last();
        }
        if (pos) {
            auto k = c->key();
            if (k.size() == 16 && rdbe(k.data()) == id && rdbe(k.data() + 8) <= rg) {
                out = deserialize(c->val());
                return true;
            }
        }
        return false;
    }

    void storeNode(IKvTxn* t, uint64_t id, SnapId snap, NodeRec const& r) {
        t->put(Kv::Nodes, be(id) + be(snap), serialize(r));
        t->put(Kv::Changes, be(snap) + be(id), std::string_view());
    }

    Node alloc(Type type, std::string blob, uint64_t spec) {
        auto w = m_kv->begin(true);
        uint64_t id = m_next++;

        putMeta(w.get(), "next_node", m_next);

        NodeRec r;
        r.type = uint32_t(type);
        r.spec = spec;
        r.blob = std::move(blob);
        r.dnLinkVer = m_cur;
        r.upLinkVer = m_cur;
        r.atime = r.mtime = r.ctime = m_clock;

        storeNode(w.get(), id, m_cur, r);
        w->commit();
        return handle(id, LATEST);
    }

    template <class F> void scanDownlinks(IKvTxn* t, uint64_t dir, uint64_t dv, F fn) const {
        std::string pfx = be(dir) + be(dv);
        auto c = t->cursor(Kv::DownLinks);

        for (bool ok = c->seek(pfx); ok; ok = c->next()) {
            auto k = c->key();
            if (k.size() < 16 || memcmp(k.data(), pfx.data(), 16) != 0) {
                break;
            }
            std::string name(k.data() + 16, k.size() - 16);
            fn(name, rdbe(c->val().data()));
        }
    }

    void linkSet(IKvTxn* t, uint64_t dir, SnapId rg, std::map<std::string, uint64_t>& out) const {
        NodeRec dr;
        if (!loadNode(t, dir, rg, dr) || dr.type != uint32_t(Type::DIR)) {
            return;
        }
        scanDownlinks(t, dir, dr.dnLinkVer, [&](std::string const& n, uint64_t c) { out[n] = c; });
    }

    // Yields each node's effective record at rg (the newest version ≤ rg),
    // regardless of nlink. Callers filter for liveness as needed.
    template <class F> void eachEffNode(IKvTxn* t, SnapId rg, F fn) const {
        auto c = t->cursor(Kv::Nodes);
        uint64_t curId = 0;
        bool have = false;
        NodeRec chosen;
        auto finish = [&]() {
            if (have) {
                fn(curId, chosen);
            }
        };
        for (bool ok = c->first(); ok; ok = c->next()) {
            auto k = c->key();
            uint64_t id = rdbe(k.data());
            SnapId snap = rdbe(k.data() + 8);
            if (id != curId) {
                finish();
                curId = id;
                have = false;
            }
            if (snap <= rg) {
                chosen = deserialize(c->val());
                have = true;
            }
        }
        finish();
    }

    void candidates(IKvTxn* t, SnapId ra, SnapId rb, std::set<uint64_t>& out) const {
        auto c = t->cursor(Kv::Changes);
        std::string lo = be(ra + 1);

        for (bool ok = c->seek(lo); ok; ok = c->next()) {
            auto k = c->key();
            uint64_t snap = rdbe(k.data());
            if (snap > rb) {
                break;
            }
            out.insert(rdbe(k.data() + 8));
        }
    }

    uint64_t ensureWritableDnLinks(IKvTxn* t, uint64_t dir) {
        NodeRec r;
        loadNode(t, dir, m_cur, r);
        if (r.dnLinkVer == m_cur) {
            return m_cur;
        }

        std::vector<std::pair<std::string, uint64_t>> copies;
        scanDownlinks(t, dir, r.dnLinkVer,
                      [&](std::string const& n, uint64_t c) { copies.push_back({n, c}); });
        for (auto const& [name, child] : copies) {
            t->put(Kv::DownLinks, be(dir) + be(m_cur) + name, be(child));
        }

        r.dnLinkVer = m_cur;
        storeNode(t, dir, m_cur, r);
        return m_cur;
    }

    uint64_t ensureWritableUpLinks(IKvTxn* t, uint64_t node) {
        NodeRec r;
        loadNode(t, node, m_cur, r);
        if (r.upLinkVer == m_cur) {
            return m_cur;
        }

        std::vector<std::string> copies; // suffix = container(8) ‖ name
        std::string pfx = be(node) + be(r.upLinkVer);
        auto c = t->cursor(Kv::UpLinks);
        for (bool ok = c->seek(pfx); ok; ok = c->next()) {
            auto k = c->key();
            if (k.size() < 16 || memcmp(k.data(), pfx.data(), 16) != 0) {
                break;
            }
            copies.emplace_back(k.data() + 16, k.size() - 16);
        }
        for (auto const& suffix : copies) {
            t->put(Kv::UpLinks, be(node) + be(m_cur) + suffix, std::string_view());
        }

        r.upLinkVer = m_cur;
        storeNode(t, node, m_cur, r);
        return m_cur;
    }

    bool reachable(IKvTxn* t, uint64_t from, uint64_t to) const {
        std::vector<uint64_t> st{from};
        std::set<uint64_t> seen;

        while (!st.empty()) {
            uint64_t n = st.back();
            st.pop_back();
            if (n == to) {
                return true;
            }
            if (!seen.insert(n).second) {
                continue;
            }
            NodeRec r;
            if (!loadNode(t, n, m_cur, r) || r.type != uint32_t(Type::DIR)) {
                continue;
            }
            scanDownlinks(t, n, r.dnLinkVer, [&](std::string const&, uint64_t child) {
                NodeRec cr;
                if (loadNode(t, child, m_cur, cr) && cr.type == uint32_t(Type::DIR)) {
                    st.push_back(child);
                }
            });
        }
        return false;
    }

    Error linkT(IKvTxn* t, uint64_t dir, std::string const& name, uint64_t child) {
        if (name == SNAPSHOT_NAME) {
            return Error::INVAL; // reserved for the synthesized snapshot dir (§3.2)
        }

        NodeRec dr, cr;
        if (!loadNode(t, dir, m_cur, dr)) {
            return Error::NOENT;
        }
        if (dr.type != uint32_t(Type::DIR)) {
            return Error::NOTDIR;
        }
        if (!loadNode(t, child, m_cur, cr)) {
            return Error::NOENT;
        }
        if (cr.type == uint32_t(Type::DIR) && reachable(t, child, dir)) {
            return Error::INVAL;
        }

        std::string tmp;
        if (t->get(Kv::DownLinks, be(dir) + be(dr.dnLinkVer) + name, tmp)) {
            return Error::EXIST;
        }

        uint64_t dv = ensureWritableDnLinks(t, dir);
        uint64_t uv = ensureWritableUpLinks(t, child);
        t->put(Kv::DownLinks, be(dir) + be(dv) + name, be(child));
        t->put(Kv::UpLinks, be(child) + be(uv) + be(dir) + name, std::string_view());

        loadNode(t, child, m_cur, cr);
        cr.nlink++;
        storeNode(t, child, m_cur, cr);
        return Error::OK;
    }

    Error unlinkT(IKvTxn* t, uint64_t dir, std::string const& name) {
        NodeRec dr;
        if (!loadNode(t, dir, m_cur, dr)) {
            return Error::NOENT;
        }

        std::string child;
        if (!t->get(Kv::DownLinks, be(dir) + be(dr.dnLinkVer) + name, child)) {
            return Error::NOENT;
        }
        uint64_t cid = rdbe(child.data());

        uint64_t dv = ensureWritableDnLinks(t, dir);
        uint64_t uv = ensureWritableUpLinks(t, cid);
        t->del(Kv::DownLinks, be(dir) + be(dv) + name);
        t->del(Kv::UpLinks, be(cid) + be(uv) + be(dir) + name);

        NodeRec cr;
        loadNode(t, cid, m_cur, cr);
        if (cr.nlink) {
            cr.nlink--;
        }
        storeNode(t, cid, m_cur, cr);
        return Error::OK;
    }

    Error setBlob(Node n, Type want, std::string const& blob) {
        auto w = m_kv->begin(true);
        NodeRec r;
        if (!loadNode(w.get(), n->id(), m_cur, r)) {
            return Error::NOENT;
        }
        if (r.type != uint32_t(want)) {
            return Error::INVAL;
        }
        r.blob = blob;
        storeNode(w.get(), n->id(), m_cur, r);
        w->commit();
        return Error::OK;
    }

    // for PrfsNode
    NodeRec readNode(uint64_t id, SnapId snap) {
        if (isSnapDir(id)) {
            //  Synthetic GETATTR: a read-only directory mirroring the base
            //  node's timestamps, so the handle is fully attributable.
            NodeRec base = readNode(snapBase(id), snap);
            NodeRec s;
            s.type = uint32_t(Type::DIR);
            s.mode = 0555;
            s.nlink = 1;
            s.atime = base.atime;
            s.mtime = base.mtime;
            s.ctime = base.ctime;
            return s;
        }

        auto r = m_kv->begin(false);
        NodeRec n;
        loadNode(r.get(), id, rr(snap), n);
        return n;
    }

    //  A snapshot N is listed under D/.snapshot iff it is sealed and D existed
    //  at it (loadNode floors to the newest record ≤ N).
    bool existedAt(IKvTxn* t, uint64_t id, SnapId n) const {
        NodeRec tmp;
        return loadNode(t, id, n, tmp);
    }

    static bool parseSnap(std::string const& s, SnapId& out) {
        if (s.empty()) {
            return false;
        }
        SnapId v = 0;
        for (char c : s) {
            if (c < '0' || c > '9') {
                return false;
            }
            v = v * 10 + uint64_t(c - '0');
        }
        out = v;
        return true;
    }

    void mutateNode(uint64_t id, std::function<void(NodeRec&)> fn) {
        auto w = m_kv->begin(true);
        NodeRec r;
        if (!loadNode(w.get(), id, m_cur, r)) {
            return;
        }
        fn(r);
        storeNode(w.get(), id, m_cur, r);
        w->commit();
    }

    std::unique_ptr<IKvStore> m_kv;
    uint64_t m_next = 1;
    SnapId m_cur = 1;
    uint64_t m_root = 0;
    uint64_t m_clock = 0;
};

// ---- PrfsNode accessors --------------------------------------------------
Type PrfsNode::type() const { return Type(m_store->readNode(m_id, m_snap).type); }

uint32_t PrfsNode::mode() const { return m_store->readNode(m_id, m_snap).mode; }

uint32_t PrfsNode::nlink() const { return m_store->readNode(m_id, m_snap).nlink; }

uint32_t PrfsNode::uid() const { return m_store->readNode(m_id, m_snap).uid; }

uint32_t PrfsNode::gid() const { return m_store->readNode(m_id, m_snap).gid; }

uint64_t PrfsNode::size() const { return m_store->readNode(m_id, m_snap).size; }

uint64_t PrfsNode::atime() const { return m_store->readNode(m_id, m_snap).atime; }

uint64_t PrfsNode::mtime() const { return m_store->readNode(m_id, m_snap).mtime; }

uint64_t PrfsNode::ctime() const { return m_store->readNode(m_id, m_snap).ctime; }

void PrfsNode::mode(uint32_t x) {
    m_store->mutateNode(m_id, [&](NodeRec& r) { r.mode = x; });
}

void PrfsNode::uid(uint32_t x) {
    m_store->mutateNode(m_id, [&](NodeRec& r) { r.uid = x; });
}

void PrfsNode::gid(uint32_t x) {
    m_store->mutateNode(m_id, [&](NodeRec& r) { r.gid = x; });
}

void PrfsNode::size(uint64_t x) {
    m_store->mutateNode(m_id, [&](NodeRec& r) { r.size = x; });
}

void PrfsNode::atime(uint64_t x) {
    m_store->mutateNode(m_id, [&](NodeRec& r) { r.atime = x; });
}

void PrfsNode::mtime(uint64_t x) {
    m_store->mutateNode(m_id, [&](NodeRec& r) { r.mtime = x; });
}

void PrfsNode::ctime(uint64_t x) {
    m_store->mutateNode(m_id, [&](NodeRec& r) { r.ctime = x; });
}

std::string PrfsNode::target() const {
    NodeRec r = m_store->readNode(m_id, m_snap);
    return r.type == uint32_t(Type::LNK) ? r.blob : "";
}

std::string PrfsNode::content() const {
    NodeRec r = m_store->readNode(m_id, m_snap);
    return r.type == uint32_t(Type::REG) ? r.blob : "";
}

std::pair<uint32_t, uint32_t> PrfsNode::rdev() const {
    uint64_t s = m_store->readNode(m_id, m_snap).spec;
    return {uint32_t(s >> 32), uint32_t(s & 0xffffffff)};
}

} // namespace

std::unique_ptr<IPrfs> makePrfsStore(std::unique_ptr<IKvStore> kv) {
    return std::make_unique<PrfsStore>(std::move(kv));
}

} // namespace prfs
