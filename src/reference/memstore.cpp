// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  MemStore — in-memory reference implementation of IPrfs (design §13).
//
//  The executable spec / test oracle. Correctness over speed: it models the
//  observable semantics of the design directly (per-node version lists, link
//  lifetime intervals) and recomputes stats/diffs by scanning. The LMDB backend
//  is validated against this via differential testing.
//
//  Deliberate v1 simplifications (documented, to be revisited):
//   - readdir/parents return vectors, not streaming iterators.
//   - lookup returns nullptr on miss (the NFS front-end maps that to NOENT).
//   - link/unlink do NOT auto-touch mtime/ctime; callers set times explicitly
//     (keeps diffs and determinism tests crisp).
//   - unlink is a generic edge removal (no empty-dir check); rmdir semantics
//     belong to the front-end.
//   - content/targets are stored inline (no interning); dedup stats are phase-2.
//   - `.snapshot` path synthesis is a front-end concern, not modelled here;
//     snapshot views are reached via snapshotRoot(N).
//
#include "prfs/memstore.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace prfs {
namespace {

struct Attr {
    Type type{};
    uint32_t mode = 0, uid = 0, gid = 0, nlink = 0;
    uint64_t size = 0, atime = 0, mtime = 0, ctime = 0, spec = 0;
};

struct NodeVer {
    SnapId snap;
    Attr a;
    std::string blob;
}; // blob = target(LNK)/content(REG)

struct LinkIv {
    SnapId born;
    SnapId dead;
    uint64_t child;
}; // dead == 0  =>  still live

class MemStore;

class MemNode : public INode {
public:
    MemNode(MemStore* s, uint64_t id, SnapId snap)
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
    MemStore* m_store;
    uint64_t m_id;
    SnapId m_snap;
};

class MemStore : public IPrfs {
    friend class MemNode;

public:
    MemStore() {
        m_root = m_next++; // node 1 = root DIR, exists by fiat
        Attr a;
        a.type = Type::DIR;
        a.mode = 0755;
        a.nlink = 1;
        a.atime = a.mtime = a.ctime = now();

        m_nodes[m_root].push_back({m_cur, a, ""});
        m_changes[m_cur].insert(m_root);
    }

    //  Logical clock (design §3): deterministic and script-driven, never
    //  wall-clock. now() reads it; setTime() is the only thing that advances it.
    uint64_t now() const override { return m_clock; }

    void setTime(uint64_t t) override { m_clock = t; }

    Node rwRoot() override { return handle(m_root, LATEST); }

    Node snapshotRoot(SnapId n) override { return handle(m_root, n); }

    std::vector<SnapId> snapshots() const override {
        std::vector<SnapId> v;
        for (SnapId s = 1; s < m_cur; ++s) {
            v.push_back(s);
        }
        return v;
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
        uint64_t child;

        if (linkLiveAt({dir->id(), name}, dir->snap(), child)) {
            return handle(child, dir->snap());
        }
        return nullptr;
    }

    Error link(Node dir, std::string const& name, Node child) override {
        if (dir->type() != Type::DIR) {
            return Error::NOTDIR;
        }
        if (child->type() == Type::DIR && reachable(child->id(), dir->id())) {
            return Error::INVAL; // §2.2 cycle prevention
        }
        uint64_t dummy;

        if (linkLiveAt({dir->id(), name}, LATEST, dummy)) {
            return Error::EXIST;
        }

        m_links[{dir->id(), name}].push_back({m_cur, 0, child->id()});
        m_names[dir->id()].insert(name);
        liveVer(child->id()).a.nlink++;
        touch(dir->id());
        touch(child->id());
        return Error::OK;
    }

    Error unlink(Node dir, std::string const& name) override {
        auto it = m_links.find({dir->id(), name});
        if (it == m_links.end()) {
            return Error::NOENT;
        }
        LinkIv* iv = liveIv(it->second, m_cur);
        if (!iv) {
            return Error::NOENT;
        }

        uint64_t child = iv->child;

        if (iv->born == m_cur) {
            it->second.erase(it->second.begin() + (iv - it->second.data()));
        } else {
            iv->dead = m_cur;
        }
        liveVer(child).a.nlink--;
        touch(dir->id());
        touch(child);
        return Error::OK;
    }

    Error move(Node sdir, std::string const& sn, Node ddir, std::string const& dn) override {
        uint64_t child;

        if (!linkLiveAt({sdir->id(), sn}, LATEST, child)) {
            return Error::NOENT;
        }
        Error e = link(ddir, dn, handle(child, LATEST)); // cycle + EXIST checks
        if (e != Error::OK) {
            return e;
        }
        return unlink(sdir, sn);
    }

    Error setContent(Node reg, std::string const& c) override {
        if (reg->type() != Type::REG) {
            return Error::INVAL;
        }
        liveVer(reg->id()).blob = c;
        touch(reg->id());
        return Error::OK;
    }

    Error setTarget(Node lnk, std::string const& t) override {
        if (lnk->type() != Type::LNK) {
            return Error::INVAL;
        }
        liveVer(lnk->id()).blob = t;
        touch(lnk->id());
        return Error::OK;
    }

    Error setRdev(Node dev, uint32_t maj, uint32_t min) override {
        if (dev->type() != Type::BLK && dev->type() != Type::CHR) {
            return Error::INVAL;
        }
        liveVer(dev->id()).a.spec = (uint64_t(maj) << 32) | min;
        touch(dev->id());
        return Error::OK;
    }

    std::vector<std::pair<std::string, Node>> readdir(Node dir) override {
        std::vector<std::pair<std::string, Node>> out;
        auto ni = m_names.find(dir->id());

        if (ni == m_names.end()) {
            return out;
        }
        for (auto const& name : ni->second) {
            uint64_t child;
            if (linkLiveAt({dir->id(), name}, dir->snap(), child)) {
                out.push_back({name, handle(child, dir->snap())});
            }
        }
        return out;
    }

    std::vector<Node> parents(Node node) override {
        std::set<uint64_t> seen;
        std::vector<Node> out;

        for (auto const& [key, ivs] : m_links) {
            for (auto const& iv : ivs) {
                if (iv.child == node->id() && liveAt(iv, resolve(node->snap())) &&
                    seen.insert(key.first).second) {
                    out.push_back(handle(key.first, node->snap()));
                }
            }
        }
        return out;
    }

    SnapId snapshot() override {
        SnapId v = m_cur;
        ++m_cur;
        return v;
    }

    std::vector<NodeDiff> diffNodes(SnapId a, SnapId b) override {
        SnapId ra = resolve(a), rb = resolve(b);
        std::set<uint64_t> cand;

        for (auto const& [s, ids] : m_changes) {
            if (s > ra && s <= rb) {
                cand.insert(ids.begin(), ids.end());
            }
        }

        std::vector<NodeDiff> out;
        for (uint64_t id : cand) {
            NodeVer const* va = effR(id, ra);
            NodeVer const* vb = effR(id, rb);
            bool eA = va && va->a.nlink > 0;
            bool eB = vb && vb->a.nlink > 0;
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
            bool content =
                va->blob != vb->blob || va->a.spec != vb->a.spec || va->a.size != vb->a.size;
            bool attrs = va->a.mode != vb->a.mode || va->a.uid != vb->a.uid ||
                         va->a.gid != vb->a.gid || va->a.mtime != vb->a.mtime;
            if (content) {
                out.push_back({id, NodeChange::MODIFIED_CONTENT});
            } else if (attrs) {
                out.push_back({id, NodeChange::MODIFIED_ATTRS});
            }
        }
        return out; // already ordered by id (from set)
    }

    std::vector<PathDiff> diffPaths(SnapId a, SnapId b) override {
        SnapId ra = resolve(a), rb = resolve(b);
        std::vector<PathDiff> out;

        for (auto const& [key, ivs] : m_links) {
            uint64_t ca, cb;
            bool la = liveAtR(ivs, ra, ca), lb = liveAtR(ivs, rb, cb);
            if (!la && lb) {
                out.push_back({PathChange::ADDED, key.first, key.second, cb});
            } else if (la && !lb) {
                out.push_back({PathChange::REMOVED, key.first, key.second, ca});
            } else if (la && lb && ca != cb) {
                out.push_back({PathChange::REMOVED, key.first, key.second, ca});
                out.push_back({PathChange::ADDED, key.first, key.second, cb});
            }
        }
        return out;
    }

    Stats stats(SnapId g) const override {
        SnapId rg = resolve(g);
        Stats s;

        for (auto const& [id, vers] : m_nodes) {
            NodeVer const* v = effR(id, rg);
            if (v && v->a.nlink > 0) {
                s.nodes[int(v->a.type)]++;
                if (v->a.type == Type::REG) {
                    s.totalSize += v->a.size;
                }
            }
        }
        for (auto const& [key, ivs] : m_links) {
            for (auto const& iv : ivs) {
                if (liveAt(iv, rg)) {
                    s.links++;
                }
            }
        }
        return s;
    }

private:
    // ---- helpers ----
    SnapId resolve(SnapId g) const { return g == LATEST ? m_cur : g; }

    Node handle(uint64_t id, SnapId snap) { return std::make_shared<MemNode>(this, id, snap); }

    Node alloc(Type t, std::string blob, uint64_t spec) {
        uint64_t id = m_next++;
        Attr a;
        a.type = t;
        a.spec = spec;
        a.atime = a.mtime = a.ctime = now();
        m_nodes[id].push_back({m_cur, a, std::move(blob)});
        m_changes[m_cur].insert(id);
        return handle(id, LATEST);
    }

    // effective version at a resolved snapshot (largest snap <= rg), or null
    NodeVer const* effR(uint64_t id, SnapId rg) const {
        auto it = m_nodes.find(id);
        if (it == m_nodes.end()) {
            return nullptr;
        }
        NodeVer const* best = nullptr;
        for (auto const& v : it->second) {
            if (v.snap <= rg) {
                best = &v;
            } else {
                break;
            }
        }
        return best;
    }

    NodeVer const* eff(uint64_t id, SnapId g) const { return effR(id, resolve(g)); }

    // copy-on-write the node into m_cur and return the live version
    NodeVer& liveVer(uint64_t id) {
        auto& vs = m_nodes[id];
        if (!vs.empty() && vs.back().snap == m_cur) {
            return vs.back();
        }
        NodeVer nv =
            vs.empty() ? NodeVer{m_cur, {}, ""} : NodeVer{m_cur, vs.back().a, vs.back().blob};
        vs.push_back(std::move(nv));
        return vs.back();
    }

    void touch(uint64_t id) { m_changes[m_cur].insert(id); }

    static bool liveAt(LinkIv const& iv, SnapId rg) {
        return iv.born <= rg && (iv.dead == 0 || iv.dead > rg);
    }

    static LinkIv* liveIv(std::vector<LinkIv>& ivs, SnapId rg) {
        for (auto& iv : ivs) {
            if (liveAt(iv, rg)) {
                return &iv;
            }
        }
        return nullptr;
    }

    static bool liveAtR(std::vector<LinkIv> const& ivs, SnapId rg, uint64_t& child) {
        for (auto const& iv : ivs) {
            if (liveAt(iv, rg)) {
                child = iv.child;
                return true;
            }
        }
        return false;
    }

    bool linkLiveAt(std::pair<uint64_t, std::string> const& key, SnapId g, uint64_t& child) const {
        auto it = m_links.find(key);
        if (it == m_links.end()) {
            return false;
        }
        return liveAtR(it->second, resolve(g), child);
    }

    // is `to` reachable from `from` following live directory down-links (at m_cur)?
    bool reachable(uint64_t from, uint64_t to) const {
        std::vector<uint64_t> stack{from};
        std::set<uint64_t> seen;
        while (!stack.empty()) {
            uint64_t n = stack.back();
            stack.pop_back();
            if (n == to) {
                return true;
            }
            if (!seen.insert(n).second) {
                continue;
            }
            auto ni = m_names.find(n);
            if (ni == m_names.end()) {
                continue;
            }
            for (auto const& name : ni->second) {
                uint64_t child;
                if (linkLiveAt({n, name}, LATEST, child)) {
                    NodeVer const* cv = eff(child, LATEST);
                    if (cv && cv->a.type == Type::DIR) {
                        stack.push_back(child);
                    }
                }
            }
        }
        return false;
    }

    std::map<uint64_t, std::vector<NodeVer>> m_nodes;
    std::map<std::pair<uint64_t, std::string>, std::vector<LinkIv>> m_links;
    std::map<uint64_t, std::set<std::string>> m_names;
    std::map<SnapId, std::set<uint64_t>> m_changes;
    uint64_t m_next = 1;
    SnapId m_cur = 1;
    uint64_t m_root = 0;
    uint64_t m_clock = 0;
};

// ---- MemNode accessors ----
Type MemNode::type() const { return m_store->m_nodes.at(m_id).front().a.type; }

uint32_t MemNode::nlink() const {
    auto v = m_store->eff(m_id, m_snap);
    return v ? v->a.nlink : 0;
}

uint32_t MemNode::mode() const {
    auto v = m_store->eff(m_id, m_snap);
    return v ? v->a.mode : 0;
}

uint32_t MemNode::uid() const {
    auto v = m_store->eff(m_id, m_snap);
    return v ? v->a.uid : 0;
}

uint32_t MemNode::gid() const {
    auto v = m_store->eff(m_id, m_snap);
    return v ? v->a.gid : 0;
}

uint64_t MemNode::size() const {
    auto v = m_store->eff(m_id, m_snap);
    return v ? v->a.size : 0;
}

uint64_t MemNode::atime() const {
    auto v = m_store->eff(m_id, m_snap);
    return v ? v->a.atime : 0;
}

uint64_t MemNode::mtime() const {
    auto v = m_store->eff(m_id, m_snap);
    return v ? v->a.mtime : 0;
}

uint64_t MemNode::ctime() const {
    auto v = m_store->eff(m_id, m_snap);
    return v ? v->a.ctime : 0;
}

void MemNode::mode(uint32_t x) {
    m_store->liveVer(m_id).a.mode = x;
    m_store->touch(m_id);
}

void MemNode::uid(uint32_t x) {
    m_store->liveVer(m_id).a.uid = x;
    m_store->touch(m_id);
}

void MemNode::gid(uint32_t x) {
    m_store->liveVer(m_id).a.gid = x;
    m_store->touch(m_id);
}

void MemNode::size(uint64_t x) {
    m_store->liveVer(m_id).a.size = x;
    m_store->touch(m_id);
}

void MemNode::atime(uint64_t x) {
    m_store->liveVer(m_id).a.atime = x;
    m_store->touch(m_id);
}

void MemNode::mtime(uint64_t x) {
    m_store->liveVer(m_id).a.mtime = x;
    m_store->touch(m_id);
}

void MemNode::ctime(uint64_t x) {
    m_store->liveVer(m_id).a.ctime = x;
    m_store->touch(m_id);
}

std::string MemNode::target() const {
    if (type() != Type::LNK) {
        return "";
    }
    auto v = m_store->eff(m_id, m_snap);
    return v ? v->blob : "";
}

std::string MemNode::content() const {
    if (type() != Type::REG) {
        return "";
    }
    auto v = m_store->eff(m_id, m_snap);
    return v ? v->blob : "";
}

std::pair<uint32_t, uint32_t> MemNode::rdev() const {
    auto v = m_store->eff(m_id, m_snap);
    uint64_t s = v ? v->a.spec : 0;
    return {uint32_t(s >> 32), uint32_t(s & 0xffffffff)};
}

} // anonymous namespace

std::unique_ptr<IPrfs> makeMemStore() { return std::make_unique<MemStore>(); }

} // namespace prfs
