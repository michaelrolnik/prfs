// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

#pragma once
//
//  prfs — public interface (see docs/design.md).
//
//  A node handle is (nodeID, snapId). `Node` == shared_ptr<INode>. The store is
//  the factory. Content is opaque to the store. Everything is content-free.
//
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace prfs {

enum class Type { REG, DIR, LNK, BLK, CHR, FIFO, SOCK };

enum class Error { OK, NOENT, EXIST, NOTDIR, ISDIR, INVAL, NOTEMPTY, PERM };

using SnapId = uint64_t;
inline constexpr SnapId LATEST = ~SnapId(0); // resolve at cur_snap (dynamic)

class INode { // handle carries (id, snap)
public:
    virtual ~INode() = default;

    virtual uint64_t id() const = 0;
    virtual SnapId snap() const = 0; // the view this handle reads at
    virtual Type type() const = 0;   // immutable

    virtual uint32_t mode() const = 0;
    virtual void mode(uint32_t) = 0;
    virtual uint32_t nlink() const = 0; // #incoming links (design §2.2)
    virtual uint32_t uid() const = 0;
    virtual void uid(uint32_t) = 0;
    virtual uint32_t gid() const = 0;
    virtual void gid(uint32_t) = 0;
    virtual uint64_t size() const = 0;
    virtual void size(uint64_t) = 0;
    virtual uint64_t atime() const = 0;
    virtual void atime(uint64_t) = 0;
    virtual uint64_t mtime() const = 0;
    virtual void mtime(uint64_t) = 0;
    virtual uint64_t ctime() const = 0;
    virtual void ctime(uint64_t) = 0;

    //  type-specific views over `spec`:
    virtual std::string target() const = 0;                 // LNK
    virtual std::pair<uint32_t, uint32_t> rdev() const = 0; // BLK/CHR (major,minor)
    virtual std::string content() const = 0;                // REG (opaque block struct)
};

using Node = std::shared_ptr<INode>;

struct Stats {                                 // O(1) global stats (design §9)
    uint64_t nodes[7] = {0, 0, 0, 0, 0, 0, 0}; // indexed by Type
    uint64_t links = 0;
    uint64_t totalSize = 0;
};

//  diff results (design §6.1)
enum class NodeChange { CREATED, REMOVED, MODIFIED_CONTENT, MODIFIED_ATTRS };

struct NodeDiff {
    uint64_t id;
    NodeChange change;
};

enum class PathChange { ADDED, REMOVED };

struct PathDiff {
    PathChange change;
    uint64_t parent;
    std::string name;
    uint64_t child;
};

class IPrfs {
public:
    virtual ~IPrfs() = default;

    //  roots
    virtual Node rwRoot() = 0;             // (root, LATEST)
    virtual Node snapshotRoot(SnapId) = 0; // (root, N)
    virtual std::vector<SnapId> snapshots() const = 0;

    //  creation — typed verbs; node born valid, immutable type fixed here
    virtual Node mkdir() = 0;
    virtual Node mkfile(std::string const& content) = 0;
    virtual Node symlink(std::string const& target) = 0;
    virtual Node mknod(Type t, uint32_t major, uint32_t minor) = 0;
    virtual Node mkfifo() = 0;
    virtual Node mksock() = 0;

    //  namespace
    virtual Node lookup(Node dir, std::string const& name) = 0;
    virtual Error link(Node dir, std::string const& name, Node child) = 0;
    virtual Error unlink(Node dir, std::string const& name) = 0;
    virtual Error move(Node sdir, std::string const& sn, Node ddir, std::string const& dn) = 0;

    //  mutators (type-guarded; never change type)
    virtual Error setContent(Node reg, std::string const& content) = 0;
    virtual Error setTarget(Node lnk, std::string const& target) = 0;
    virtual Error setRdev(Node dev, uint32_t major, uint32_t minor) = 0;

    //  iteration (vectors for now; streaming iterators are a later refinement)
    virtual std::vector<std::pair<std::string, Node>> readdir(Node dir) = 0;
    virtual std::vector<Node> parents(Node node) = 0;

    //  logical clock (design §3) — deterministic, script-driven, never wall-clock.
    //  New nodes stamp atime/mtime/ctime = now(); other time changes are explicit.
    virtual uint64_t now() const = 0;     // read the logical time (does not advance)
    virtual void setTime(uint64_t t) = 0; // set the logical time (the only advance)

    //  versioning
    virtual SnapId snapshot() = 0;
    virtual std::vector<NodeDiff> diffNodes(SnapId a, SnapId b) = 0;
    virtual std::vector<PathDiff> diffPaths(SnapId a, SnapId b) = 0;

    //  statistics — O(1) in a real backend; the reference model recomputes
    virtual Stats stats(SnapId g = LATEST) const = 0;
};

struct Options {
    bool clean = false; // wipe an existing store on open
};

//  Production store. The concrete backend is selected at BUILD time
//  (meson -Dstorage=lmdb|memory|…); this is declared here and defined by the
//  chosen backend. Adding an engine = a new IPrfs implementation + a build choice.
std::unique_ptr<IPrfs> openPrfs(std::string const& path, Options const& opts = {});

} // namespace prfs
