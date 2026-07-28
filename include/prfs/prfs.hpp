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
#include <string_view>
#include <utility>
#include <vector>

namespace prfs {

enum class Type { REG, DIR, LNK, BLK, CHR, FIFO, SOCK };

enum class Error { OK, NOENT, EXIST, NOTDIR, ISDIR, INVAL, NOTEMPTY, PERM };

using SnapId = uint64_t;
inline constexpr SnapId LATEST = ~SnapId(0); // resolve at cur_snap (dynamic)

//  Reserved name of the per-directory snapshot-list directory, synthesized by
//  the store (design §3.2). `D/.snapshot/N` resolves to D viewed at snapshot N.
inline constexpr char const* SNAPSHOT_NAME = ".snapshot";

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

    //  Sub-second (nanosecond) component of atime/mtime, for NFS nfstime3 and
    //  sub-second-mtime tools (rsync, utimensat). ctime is always stamped from
    //  the integer logical clock, so it carries none. The atime(uint64_t) /
    //  mtime(uint64_t) setters reset the ns to 0 (server-time semantics); a
    //  client SETATTR sets the ns explicitly afterwards.
    virtual uint32_t atimeNsec() const = 0;
    virtual void atimeNsec(uint32_t) = 0;
    virtual uint32_t mtimeNsec() const = 0;
    virtual void mtimeNsec(uint32_t) = 0;

    //  type-specific views over `spec`:
    virtual std::string target() const = 0;                 // LNK
    virtual std::pair<uint32_t, uint32_t> rdev() const = 0; // BLK/CHR (major,minor)
    virtual std::string content() const = 0;                // REG (opaque block struct)

    //  REG content seed (design §11.2): the value the content provider hashes to
    //  generate bytes. Defaults to the nodeID; a WRITE evolves it (see
    //  setContentSeed) so generated content reflects writes without storing bytes.
    virtual uint64_t contentSeed() const = 0;
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

//  Per-snapshot metadata (design §3.2). `ctime` is the logical time snapshot()
//  ran; `label` is an optional caller-supplied name (e.g. "monday").
struct SnapInfo {
    SnapId id = 0;
    uint64_t ctime = 0;
    std::string label;
};

enum class PathChange { ADDED, REMOVED };

struct PathDiff {
    PathChange change;
    uint64_t parent;
    std::string name;
    uint64_t child;
};

//  A page of directory entries with a resume cursor (design §6.2). The cursor is
//  the last entry's name — opaque to the caller, and stable across concurrent
//  add/remove: resuming returns the entries ordered after it, each exactly once.
struct DirPage {
    std::vector<std::pair<std::string, Node>> entries;
    std::string cookie; // pass as `after` to continue; "" = start / nothing more
    bool eof = false;   // true when this page reached the end of the directory
};

class IPrfs {
public:
    virtual ~IPrfs() = default;

    //  roots
    virtual Node rwRoot() = 0;             // (root, LATEST)
    virtual Node snapshotRoot(SnapId) = 0; // (root, N)
    virtual std::vector<SnapId> snapshots() const = 0;
    virtual SnapInfo snapInfo(SnapId) const = 0; // {ctime,label}; unknown id → ctime 0

    //  Reconstruct a handle from a decoded filehandle (nodeID, snapId) — the NFS
    //  front-end's fh↔node round-trip. nullptr if nothing resolves there (STALE);
    //  accepts synthesized `.snapshot` ids too.
    virtual Node nodeById(uint64_t id, SnapId snap) = 0;

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

    //  Set a REG's content seed (design §11.2). The synthetic-target WRITE path:
    //  a write stores no bytes — it folds the written data into this seed, and
    //  READ regenerates content from (ContentConfig, seed, offset).
    virtual Error setContentSeed(Node reg, uint64_t seed) = 0;

    //  iteration (vectors for now; streaming iterators are a later refinement)
    virtual std::vector<std::pair<std::string, Node>> readdir(Node dir) = 0;
    //  Paginated readdir: up to `max` entries after cursor `after` ("" starts).
    //  Stable under concurrent mutation (design §6.2); frozen views never change.
    virtual DirPage readdirPage(Node dir, std::string const& after, size_t max) = 0;
    virtual std::vector<Node> parents(Node node) = 0;

    //  logical clock (design §3) — deterministic, script-driven, never wall-clock.
    //  New nodes stamp atime/mtime/ctime = now(); other time changes are explicit.
    virtual uint64_t now() const = 0;     // read the logical time (does not advance)
    virtual void setTime(uint64_t t) = 0; // set the logical time (the only advance)

    //  Filesystem content policy (design §11.2): an opaque serialized
    //  ContentConfig the store persists but never interprets. The content
    //  provider (docs/content.md) owns the format; empty until set.
    virtual std::string contentConfig() const = 0;
    virtual void setContentConfig(std::string const& blob) = 0;

    //  versioning
    virtual SnapId snapshot(std::string const& label = "") = 0;
    virtual std::vector<NodeDiff> diffNodes(SnapId a, SnapId b) = 0;
    virtual std::vector<PathDiff> diffPaths(SnapId a, SnapId b) = 0;

    //  statistics — O(1) in a real backend; the reference model recomputes
    virtual Stats stats(SnapId g = LATEST) const = 0;
};

struct Options {
    bool clean = false; // wipe an existing store on open
};

//  Production store. openPrfs opens over the *active* storage engine — a di
//  provider selected by name. The build sets the default (meson -Dstorage=), and
//  setStorageEngine() may override it once at startup. Adding an engine = a new
//  IStorageEngine provider (see kvstore.hpp), no change here.
std::unique_ptr<IPrfs> openPrfs(std::string const& path, Options const& opts = {});

std::string storageEngine();                  // the active engine name
void setStorageEngine(std::string_view name); // throws std::out_of_range if unregistered

} // namespace prfs
