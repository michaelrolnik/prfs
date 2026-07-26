// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  prfs-lua — sol2 bindings for IPrfs / INode (design §12).
//
//  Lua API (installed under the global `prfs`):
//    prfs.open(path, {clean=bool})  -> store   (the -Dstorage= backend)
//    prfs.mem()                     -> store   (the in-memory reference model)
//    prfs.Type / prfs.Error / prfs.NodeChange / prfs.PathChange  -- enums
//
//    store:root() / :snapshotRoot(id) / :snapshots()
//    store:mkdir() / :mkfile(c) / :symlink(t) / :mknod(type,maj,min) / :mkfifo() / :mksock()
//    store:lookup(dir,name) / :link(dir,name,child) -> Error / :unlink(dir,name) / :move(...)
//    store:setContent / :setTarget / :setRdev
//    store:readdir(dir) -> { {name=,node=}, ... }   store:parents(node) -> { node, ... }
//    store:readdirPage(dir, after, max) -> { entries={{name=,node=},...}, cookie=, eof= }
//    store:now() / :setTime(t)   -- logical clock (deterministic, script-driven)
//    store:snapshot([label]) -> id   :snapInfo(id) -> {id,ctime,label}
//    store:diffNodes(a,b)   :diffPaths(a,b)   :stats()   :fsStat()
//    store:contentConfig() / :setContentConfig(blob)   -- FS content policy (opaque)
//    prfs.content.config{...} -> blob   .read(blob,seed,size,off,len)   .allocatedBlocks(...)
//    prfs.rng.setActive(name) / .active() / .names()    (both only with -Dcontent)
//
//    node:id() :type() :nlink() :size()/:setSize(v) :mode()/:setMode(v) (uid/gid/atime/mtime/ctime)
//    node:target() :content() :rdev() -> (major, minor)
//
#include "prfs/fsstat.hpp"
#include "prfs/lua.hpp"
#include "prfs/memstore.hpp"
#include "prfs/prfs.hpp"

#ifdef PRFS_WITH_CONTENT
#include "prfs/content.hpp"
#include "prfs/rng.hpp"
#endif

#include <sol/sol.hpp>

#include <memory>
#include <string>

namespace prfs {

static std::shared_ptr<IPrfs> luaOpen(std::string const& path, sol::optional<sol::table> opts) {
    Options o;
    if (opts) {
        o.clean = opts->get_or("clean", false);
    }
    return openPrfs(path, o);
}

static std::shared_ptr<IPrfs> luaMem() { return makeMemStore(); }

static sol::table readdirLua(IPrfs& s, Node dir, sol::this_state ts) {
    sol::state_view lua(ts);
    sol::table out = lua.create_table();
    int i = 1;

    for (auto const& [name, node] : s.readdir(dir)) {
        out[i++] = lua.create_table_with("name", name, "node", node);
    }
    return out;
}

static sol::table readdirPageLua(IPrfs& s, Node dir, std::string const& after, size_t max,
                                 sol::this_state ts) {
    sol::state_view lua(ts);
    DirPage p = s.readdirPage(dir, after, max);
    sol::table ents = lua.create_table();
    int i = 1;

    for (auto const& [name, node] : p.entries) {
        ents[i++] = lua.create_table_with("name", name, "node", node);
    }
    return lua.create_table_with("entries", ents, "cookie", p.cookie, "eof", p.eof);
}

static sol::table parentsLua(IPrfs& s, Node node, sol::this_state ts) {
    sol::state_view lua(ts);
    sol::table out = lua.create_table();
    int i = 1;

    for (auto const& p : s.parents(node)) {
        out[i++] = p;
    }
    return out;
}

static sol::table snapshotsLua(IPrfs& s, sol::this_state ts) {
    sol::state_view lua(ts);
    sol::table out = lua.create_table();
    int i = 1;

    for (SnapId id : s.snapshots()) {
        out[i++] = id;
    }
    return out;
}

static sol::table diffNodesLua(IPrfs& s, SnapId a, SnapId b, sol::this_state ts) {
    sol::state_view lua(ts);
    sol::table out = lua.create_table();
    int i = 1;

    for (auto const& d : s.diffNodes(a, b)) {
        out[i++] = lua.create_table_with("id", d.id, "change", int(d.change));
    }
    return out;
}

static sol::table diffPathsLua(IPrfs& s, SnapId a, SnapId b, sol::this_state ts) {
    sol::state_view lua(ts);
    sol::table out = lua.create_table();
    int i = 1;

    for (auto const& p : s.diffPaths(a, b)) {
        out[i++] = lua.create_table_with("change", int(p.change), "parent", p.parent, "name",
                                         p.name, "child", p.child);
    }
    return out;
}

static SnapId snapshotLua(IPrfs& s, sol::optional<std::string> label) {
    return s.snapshot(label.value_or(""));
}

static sol::table snapInfoLua(IPrfs& s, SnapId g, sol::this_state ts) {
    sol::state_view lua(ts);
    SnapInfo i = s.snapInfo(g);
    return lua.create_table_with("id", i.id, "ctime", i.ctime, "label", i.label);
}

static sol::table statsLua(IPrfs& s, sol::this_state ts) {
    sol::state_view lua(ts);
    Stats st = s.stats();
    sol::table nodes = lua.create_table();

    for (int t = 0; t < 7; ++t) {
        nodes[t] = st.nodes[t];
    }
    return lua.create_table_with("nodes", nodes, "links", st.links, "totalSize", st.totalSize);
}

static sol::table fsStatLua(IPrfs& s, sol::this_state ts) {
    sol::state_view lua(ts);
    FsStat r = fsStat(s);
    return lua.create_table_with( //
        "tbytes", r.tbytes, "fbytes", r.fbytes, "abytes", r.abytes, "tfiles", r.tfiles, "ffiles",
        r.ffiles, "afiles", r.afiles, "invarsec", r.invarsec);
}

static sol::table fsInfoLua(sol::this_state ts) {
    sol::state_view lua(ts);
    FsInfo i = fsInfo();
    return lua.create_table_with( //
        "rtmax", i.rtmax, "rtpref", i.rtpref, "rtmult", i.rtmult, "wtmax", i.wtmax, "wtpref",
        i.wtpref, "wtmult", i.wtmult, "dtpref", i.dtpref, "maxfilesize", i.maxfilesize,
        "timeDeltaSec", i.timeDeltaSec, "timeDeltaNsec", i.timeDeltaNsec, "properties",
        i.properties);
}

#ifdef PRFS_WITH_CONTENT
//  Build + serialize an FS content policy from a Lua table of named fields.
static std::string contentConfigLua(sol::table t) {
    content::ContentConfig c;
    c.blockSize = t.get_or("blockSize", c.blockSize);
    c.entropy = uint8_t(t.get_or("entropy", int(c.entropy)));
    c.sparsePercent = uint8_t(t.get_or("sparsePercent", int(c.sparsePercent)));
    c.dedupPercent = uint8_t(t.get_or("dedupPercent", int(c.dedupPercent)));
    c.dedupCorpus = t.get_or("dedupCorpus", c.dedupCorpus);
    return content::serialize(c);
}

//  Bytes a READ would return for a file, from a serialized config + node seed.
static std::string contentReadLua(std::string const& cfg, uint64_t seed, uint64_t size,
                                  uint64_t offset, size_t len) {
    content::ContentConfig c = content::deserialize(cfg);
    std::string out(len, '\0');
    out.resize(content::read(c, seed, size, offset, out.data(), len));
    return out;
}

static uint64_t contentAllocLua(std::string const& cfg, uint64_t seed, uint64_t size) {
    return content::allocatedBlocks(content::deserialize(cfg), seed, size);
}

static sol::table rngNamesLua(sol::this_state ts) {
    sol::state_view lua(ts);
    sol::table out = lua.create_table();
    int i = 1;

    for (auto const& n : rng::names()) {
        out[i++] = n;
    }
    return out;
}
#endif

void registerLua(sol::state_view lua) {
    sol::table t = lua.create_named_table("prfs");

    // Enums are exposed as plain integer tables; sol2 converts Lua numbers to the
    // matching C++ enum on the way into a bound function, and back on return.
    t["Type"] = lua.create_table_with( //
        "REG", int(Type::REG), "DIR", int(Type::DIR), "LNK", int(Type::LNK), "BLK", int(Type::BLK),
        "CHR", int(Type::CHR), "FIFO", int(Type::FIFO), "SOCK", int(Type::SOCK));
    t["Error"] = lua.create_table_with( //
        "OK", int(Error::OK), "NOENT", int(Error::NOENT), "EXIST", int(Error::EXIST), "NOTDIR",
        int(Error::NOTDIR), "ISDIR", int(Error::ISDIR), "INVAL", int(Error::INVAL), "NOTEMPTY",
        int(Error::NOTEMPTY), "PERM", int(Error::PERM));
    t["NodeChange"] = lua.create_table_with( //
        "CREATED", int(NodeChange::CREATED), "REMOVED", int(NodeChange::REMOVED),
        "MODIFIED_CONTENT", int(NodeChange::MODIFIED_CONTENT), "MODIFIED_ATTRS",
        int(NodeChange::MODIFIED_ATTRS));
    t["PathChange"] = lua.create_table_with( //
        "ADDED", int(PathChange::ADDED), "REMOVED", int(PathChange::REMOVED));

    lua.new_usertype<INode>(          //
        "INode", sol::no_constructor, //
        "id", &INode::id,             //
        "type", &INode::type,         //
        "nlink", &INode::nlink,       //
        "mode", [](INode& n) { return n.mode(); }, "setMode",
        [](INode& n, uint32_t v) { n.mode(v); }, //
        "uid", [](INode& n) { return n.uid(); }, "setUid",
        [](INode& n, uint32_t v) { n.uid(v); }, //
        "gid", [](INode& n) { return n.gid(); }, "setGid",
        [](INode& n, uint32_t v) { n.gid(v); }, //
        "size", [](INode& n) { return n.size(); }, "setSize",
        [](INode& n, uint64_t v) { n.size(v); }, //
        "atime", [](INode& n) { return n.atime(); }, "setAtime",
        [](INode& n, uint64_t v) { n.atime(v); }, //
        "mtime", [](INode& n) { return n.mtime(); }, "setMtime",
        [](INode& n, uint64_t v) { n.mtime(v); }, //
        "ctime", [](INode& n) { return n.ctime(); }, "setCtime",
        [](INode& n, uint64_t v) { n.ctime(v); }, //
        "target", &INode::target,                 //
        "content", &INode::content,               //
        "rdev",
        [](INode& n) {
            auto p = n.rdev();
            return std::make_tuple(p.first, p.second);
        });

    lua.new_usertype<IPrfs>(                    //
        "IPrfs", sol::no_constructor,           //
        "root", &IPrfs::rwRoot,                 //
        "snapshotRoot", &IPrfs::snapshotRoot,   //
        "snapshots", &snapshotsLua,             //
        "snapInfo", &snapInfoLua,               //
        "mkdir", &IPrfs::mkdir,                 //
        "mkfile", &IPrfs::mkfile,               //
        "symlink", &IPrfs::symlink,             //
        "mknod", &IPrfs::mknod,                 //
        "mkfifo", &IPrfs::mkfifo,               //
        "mksock", &IPrfs::mksock,               //
        "lookup", &IPrfs::lookup,               //
        "link", &IPrfs::link,                   //
        "unlink", &IPrfs::unlink,               //
        "move", &IPrfs::move,                   //
        "setContent", &IPrfs::setContent,       //
        "setTarget", &IPrfs::setTarget,         //
        "setRdev", &IPrfs::setRdev,             //
        "readdir", &readdirLua,                 //
        "readdirPage", &readdirPageLua,         //
        "parents", &parentsLua,                 //
        "now", &IPrfs::now,                     //
        "setTime", &IPrfs::setTime,             //
        "snapshot", &snapshotLua,               //
        "diffNodes", &diffNodesLua,             //
        "diffPaths", &diffPathsLua,             //
        "stats", &statsLua,                     //
        "fsStat", &fsStatLua,                   //
        "contentConfig", &IPrfs::contentConfig, //
        "setContentConfig", &IPrfs::setContentConfig);

    t["open"] = &luaOpen;
    t["mem"] = &luaMem;
    t["fsInfo"] = &fsInfoLua;
    t["SNAPSHOT_NAME"] = SNAPSHOT_NAME;

#ifdef PRFS_WITH_CONTENT
    //  prfs.content — build a config, and read the bytes a file's config yields.
    sol::table content = lua.create_table();
    content["config"] = &contentConfigLua;
    content["read"] = &contentReadLua;
    content["allocatedBlocks"] = &contentAllocLua;
    t["content"] = content;

    //  prfs.rng — the run-wide generator (di name).
    sol::table rng = lua.create_table();
    rng["setActive"] = [](std::string const& name) { prfs::rng::setActive(name); };
    rng["active"] = []() { return prfs::rng::active(); };
    rng["names"] = &rngNamesLua;
    t["rng"] = rng;
#endif
}

} // namespace prfs
