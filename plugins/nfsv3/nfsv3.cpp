// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  nfsv3 — an NFSv3 front-end as a prfs service plugin (todo L2). An Asio TCP
//  server that speaks ONC-RPC record marking and dispatches, on the one
//  listener, both the MOUNT program (100005 v3: MNT/UMNT/EXPORT) and the NFS
//  program (100003 v3). Implemented so far: MOUNT MNT hands out the root
//  filehandle for the single export "/"; NFS NULL, GETATTR, LOOKUP, ACCESS let a
//  client walk the tree and stat nodes; READ returns file bytes (sourced by the
//  host — procedural content or literal); READLINK returns a symlink target;
//  READDIR/READDIRPLUS list directories (with synthesized "." and ".."); and
//  FSSTAT/FSINFO/PATHCONF report volume usage, server parameters, and limits
//  (the libprfs_nfs §9 projection). The write surface — SETATTR, WRITE, CREATE,
//  MKDIR, SYMLINK, MKNOD, REMOVE, RMDIR, RENAME, LINK, COMMIT — mutates the live
//  tree (a fh into a snapshot view is read-only → NFS3ERR_ROFS). WRITE splices
//  into a node's literal content (whole-file model).
//
//  Filehandle (nfs_fh3): 16 opaque bytes = big-endian (nodeID, snapId). Decoded
//  back to a node via IPrfs::nodeById(); a null result maps to NFS3ERR_STALE.
//  Keeping snapId in the fh means a handle taken against a snapshot keeps reading
//  that frozen view, while LATEST fh's track the live tree.
//
//  I/O model (standalone Asio, coroutines): one io_context driven by a pool of
//  threads (sized to the hardware). A listener coroutine accepts connections; a
//  per-connection coroutine reads record-marked calls, dispatches, and writes
//  the reply. Dispatch itself is synchronous CPU work (XDR + content RNG), so it
//  runs inline on the io thread — the thread pool is what gives us core
//  parallelism. Port comes from the "port" option (default 2049; 2049 needs
//  privileges, so tests use a high port).
//
#include "prfs/fsstat.hpp"
#include "prfs/plugin.hpp"

#include <asio.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace prfs;
using namespace prfs::plugin;
using asio::ip::tcp;

//  ONC-RPC (RFC 5531) message + accept_stat constants.
constexpr uint32_t RPC_CALL = 0, RPC_REPLY = 1;
constexpr uint32_t MSG_ACCEPTED = 0;
constexpr uint32_t SUCCESS = 0, PROG_UNAVAIL = 1, PROG_MISMATCH = 2, PROC_UNAVAIL = 3,
                   GARBAGE_ARGS = 4;

//  NFSv3 program (RFC 1813).
constexpr uint32_t PROG_NFS = 100003;
constexpr uint32_t NFS_V3 = 3;
constexpr uint32_t NFSPROC3_NULL = 0, NFSPROC3_GETATTR = 1, NFSPROC3_SETATTR = 2,
                   NFSPROC3_LOOKUP = 3, NFSPROC3_ACCESS = 4, NFSPROC3_READLINK = 5,
                   NFSPROC3_READ = 6, NFSPROC3_WRITE = 7, NFSPROC3_CREATE = 8, NFSPROC3_MKDIR = 9,
                   NFSPROC3_SYMLINK = 10, NFSPROC3_MKNOD = 11, NFSPROC3_REMOVE = 12,
                   NFSPROC3_RMDIR = 13, NFSPROC3_RENAME = 14, NFSPROC3_LINK = 15,
                   NFSPROC3_READDIR = 16, NFSPROC3_READDIRPLUS = 17, NFSPROC3_FSSTAT = 18,
                   NFSPROC3_FSINFO = 19, NFSPROC3_PATHCONF = 20, NFSPROC3_COMMIT = 21;

//  createmode3 (CREATE) and stable_how (WRITE).
constexpr uint32_t CREATE_UNCHECKED = 0, CREATE_GUARDED = 1, CREATE_EXCLUSIVE = 2;
constexpr uint32_t WRITE_FILE_SYNC = 2;

//  ftype3 values used by MKNOD.
constexpr uint32_t NF3BLK = 3, NF3CHR = 4, NF3SOCK = 6, NF3FIFO = 7;

//  set_time enum (SETATTR sattr3): 1 = server time, 2 = client-supplied time.
constexpr uint32_t SET_TO_SERVER_TIME = 1, SET_TO_CLIENT_TIME = 2;

//  Largest offset+len a literal WRITE may reach (a synthetic target guard).
constexpr uint64_t MAX_WRITE_FILE = uint64_t(1) << 30;

//  Largest READ we answer in one reply (also the rtmax FSINFO will advertise).
constexpr uint32_t MAX_READ = 1u << 20;

//  cookieverf3 is a fixed 8-byte opaque array (no length prefix).
constexpr size_t NFS3_COOKIEVERFSIZE = 8;

//  PATHCONF policy this synthetic target reports.
constexpr uint32_t PATHCONF_LINKMAX = 0xFFFF; // max hard links
constexpr uint32_t PATHCONF_NAMEMAX = 255;    // max filename length

//  MOUNT program (RFC 1813 appendix I) — v3 only. NFSv4 dropped MOUNT entirely
//  (PUTROOTFH inside COMPOUND replaces it), so this lives with nfsv3, not apart.
//  Same listener as NFS; dispatch keys on the RPC program number.
constexpr uint32_t PROG_MOUNT = 100005;
constexpr uint32_t MOUNT_V3 = 3;
constexpr uint32_t MOUNTPROC3_NULL = 0, MOUNTPROC3_MNT = 1, MOUNTPROC3_UMNT = 3,
                   MOUNTPROC3_EXPORT = 5;
constexpr uint32_t MNT3_OK = 0;

//  A subset of nfsstat3.
constexpr uint32_t NFS3_OK = 0, NFS3ERR_PERM = 1, NFS3ERR_NOENT = 2, NFS3ERR_EXIST = 17,
                   NFS3ERR_NOTDIR = 20, NFS3ERR_ISDIR = 21, NFS3ERR_INVAL = 22, NFS3ERR_FBIG = 27,
                   NFS3ERR_ROFS = 30, NFS3ERR_NOTEMPTY = 66, NFS3ERR_STALE = 70,
                   NFS3ERR_BADTYPE = 10007;

//  access3 bits — what this synthetic (read-mostly) target grants.
constexpr uint32_t ACCESS3_READ = 0x0001, ACCESS3_LOOKUP = 0x0002, ACCESS3_EXECUTE = 0x0020;

//  RPC auth flavors we care about (RFC 5531). AUTH_SYS carries uid/gid.
constexpr uint32_t AUTH_SYS = 1;

//  Caller identity taken from the RPC credential. Absent/other flavor → root.
struct Cred {
    uint32_t uid = 0, gid = 0;
    bool sys = false;
};

uint32_t rd32(uint8_t const* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

uint64_t rd64(uint8_t const* p) { return uint64_t(rd32(p)) << 32 | rd32(p + 4); }

//  XDR reader over one RPC message — bounds-checked; `ok` latches false on
//  underflow so a truncated/garbage call is caught before it is acted on.
struct Reader {
    uint8_t const* p;
    size_t n;
    size_t pos = 0;
    bool ok = true;

    uint32_t u32() {
        if (pos + 4 > n) {
            ok = false;
            return 0;
        }
        uint32_t v = rd32(p + pos);
        pos += 4;
        return v;
    }

    uint64_t u64() {
        uint32_t hi = u32();
        uint32_t lo = u32();
        return uint64_t(hi) << 32 | lo;
    }

    //  opaque<> / string<>: 4-byte length then that many bytes, padded to 4.
    void skipOpaque() {
        uint32_t len = u32();
        size_t pad = (len + 3u) & ~size_t(3);
        if (pos + pad > n) {
            ok = false;
            return;
        }
        pos += pad;
    }

    std::string str() {
        uint32_t len = u32();
        size_t pad = (len + 3u) & ~size_t(3);
        if (!ok || pos + pad > n) {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<char const*>(p + pos), len);
        pos += pad;
        return s;
    }

    //  Skip k raw bytes (a fixed-size opaque array, e.g. cookieverf3).
    void skip(size_t k) {
        if (pos + k > n) {
            ok = false;
            return;
        }
        pos += k;
    }

    //  opaque_auth: flavor + body<400>.
    void skipAuth() {
        u32();
        skipOpaque();
    }

    //  nfs_fh3: our 16-byte (nodeID, snapId) handle.
    bool fh(uint64_t& id, uint64_t& snap) {
        uint32_t len = u32();
        if (!ok || len != 16 || pos + 16 > n) {
            ok = false;
            return false;
        }
        id = rd64(p + pos);
        snap = rd64(p + pos + 8);
        pos += 16;
        return true;
    }
};

//  Parse an opaque_auth credential (flavor + body<400>). For AUTH_SYS the body
//  is {stamp, machinename<>, uid, gid, gids<>} — pull uid/gid out; then advance
//  past the whole body regardless of flavor.
Cred readCred(Reader& r) {
    Cred c;
    uint32_t flavor = r.u32();
    uint32_t len = r.u32();
    size_t end = r.pos + ((len + 3u) & ~size_t(3));
    if (!r.ok || end > r.n) {
        r.ok = false;
        return c;
    }
    if (flavor == AUTH_SYS) {
        r.u32();        // stamp
        r.skipOpaque(); // machinename<>
        c.uid = r.u32();
        c.gid = r.u32();
        c.sys = true;
    }
    r.pos = end; // consume the credential in full (incl. gids / any flavor body)
    return c;
}

//  XDR writer appending big-endian words to a byte vector.
struct Writer {
    std::vector<uint8_t>& v;

    void u32(uint32_t x) {
        v.push_back(uint8_t(x >> 24));
        v.push_back(uint8_t(x >> 16));
        v.push_back(uint8_t(x >> 8));
        v.push_back(uint8_t(x));
    }

    void u64(uint64_t x) {
        u32(uint32_t(x >> 32));
        u32(uint32_t(x));
    }

    void fh(uint64_t id, uint64_t snap) {
        u32(16);
        u64(id);
        u64(snap);
    }

    //  string<> / opaque<>: length then bytes, zero-padded to a 4-byte boundary
    //  (the stream is word-aligned throughout, so v.size() % 4 tracks the pad).
    void str(std::string const& s) {
        u32(uint32_t(s.size()));
        v.insert(v.end(), s.begin(), s.end());
        while (v.size() % 4) {
            v.push_back(0);
        }
    }

    //  nfstime3 — logical clock is a plain counter; map it to whole seconds.
    void time(uint64_t t) {
        u32(uint32_t(t));
        u32(0);
    }

    //  opaque<>: length then the bytes, zero-padded to a 4-byte boundary.
    void opaque(void const* data, size_t len) {
        u32(uint32_t(len));
        auto const* b = static_cast<uint8_t const*>(data);
        v.insert(v.end(), b, b + len);
        while (v.size() % 4) {
            v.push_back(0);
        }
    }
};

//  prfs Type → NFSv3 ftype3, indexed by Type (REG,DIR,LNK,BLK,CHR,FIFO,SOCK).
uint32_t ftype3(Type t) {
    static constexpr uint32_t kFtype3[] = {1, 2, 5, 3, 4, 7, 6};
    return kFtype3[size_t(t)];
}

void encodeFattr(Writer& w, INode& n) {
    uint32_t maj = 0, min = 0;
    if (n.type() == Type::BLK || n.type() == Type::CHR) {
        auto rd = n.rdev();
        maj = rd.first;
        min = rd.second;
    }
    w.u32(ftype3(n.type()));
    w.u32(n.mode());
    w.u32(n.nlink());
    w.u32(n.uid());
    w.u32(n.gid());
    w.u64(n.size());
    w.u64(n.size()); // used ≈ size (content is synthetic)
    w.u32(maj);      // specdata3.specdata1
    w.u32(min);      // specdata3.specdata2
    w.u64(0);        // fsid
    w.u64(n.id());   // fileid
    w.time(n.atime());
    w.time(n.mtime());
    w.time(n.ctime());
}

//  post_op_attr — present-flag then fattr3 (or just the flag when absent).
void encodePostOp(Writer& w, INode* n) {
    if (n) {
        w.u32(1);
        encodeFattr(w, *n);
    } else {
        w.u32(0);
    }
}

//  prfs Error → nfsstat3.
uint32_t toNfsStat(Error e) {
    switch (e) {
    case Error::OK:
        return NFS3_OK;
    case Error::NOENT:
        return NFS3ERR_NOENT;
    case Error::EXIST:
        return NFS3ERR_EXIST;
    case Error::NOTDIR:
        return NFS3ERR_NOTDIR;
    case Error::ISDIR:
        return NFS3ERR_ISDIR;
    case Error::NOTEMPTY:
        return NFS3ERR_NOTEMPTY;
    case Error::PERM:
        return NFS3ERR_PERM;
    case Error::INVAL:
        return NFS3ERR_INVAL;
    }
    return NFS3ERR_INVAL;
}

//  A write fh must name the live tree; snapshot views are read-only.
bool live(uint64_t snap) { return snap == LATEST; }

//  pre_op_attr snapshot — the wcc "before" state, captured before a mutation so
//  the client can validate its cache. Absent when there is no node.
struct PreAttr {
    bool have = false;
    uint64_t size = 0, mtime = 0, ctime = 0;
};

PreAttr preOf(INode* n) {
    if (!n) {
        return {};
    }
    return {true, n->size(), n->mtime(), n->ctime()};
}

void encodePre(Writer& w, PreAttr const& p) {
    if (p.have) {
        w.u32(1);
        w.u64(p.size);
        w.time(p.mtime);
        w.time(p.ctime);
    } else {
        w.u32(0);
    }
}

//  wcc_data — the before/after pair a mutating op reports for a node.
void encodeWcc(Writer& w, PreAttr const& before, INode* after) {
    encodePre(w, before);
    encodePostOp(w, after);
}

//  Stamp a new node's ownership from the caller's AUTH_SYS credential (applied
//  before the client's sattr3, which may still override it).
void applyCred(Cred const& cred, Node const& n) {
    if (cred.sys && n) {
        n->uid(cred.uid);
        n->gid(cred.gid);
    }
}

size_t pad4(size_t n) { return (n + 3) & ~size_t(3); }

//  One READDIR entry with the cookie the client returns to resume after it.
struct DirEnt {
    uint64_t fileid;
    std::string name;
    uint64_t cookie;
    Node node;
};

//  Build a directory's entry list with monotonic cookies. "." and ".." lead
//  (".." resolves via the first parent, or self at the root), then the store's
//  entries in its stable order. cookie = 1-based position, so cookie 0 means
//  "from the start" and a resume drops every entry with cookie <= the client's.
std::vector<DirEnt> listDir(IPrfs& fs, Node dir) {
    std::vector<DirEnt> out;
    out.push_back({dir->id(), ".", 0, dir});
    std::vector<Node> ps = fs.parents(dir);
    Node parent = ps.empty() ? dir : ps.front();
    out.push_back({parent->id(), "..", 0, parent});
    for (auto& [name, node] : fs.readdir(dir)) {
        out.push_back({node->id(), name, 0, node});
    }
    for (size_t i = 0; i < out.size(); ++i) {
        out[i].cookie = i + 1;
    }
    return out;
}

class NfsV3 : public IService {
public:
    explicit NfsV3(IHost& host)
        : m_host(host) {}

    ~NfsV3() override { stop(); }

    std::vector<Option> options() const override {
        return {{"port", "TCP port to serve NFSv3/MOUNT on", "2049", false}};
    }

    Error start() override {
        std::string ps = m_host.option("port");
        int port = ps.empty() ? 2049 : std::atoi(ps.c_str());

        try {
            m_ctx.restart(); // reusable after a prior stop()
            m_acc.emplace(m_ctx);
            m_acc->open(tcp::v4());
            m_acc->set_option(asio::socket_base::reuse_address(true)); // before bind
            m_acc->bind(tcp::endpoint(tcp::v4(), uint16_t(port)));
            m_acc->listen();
        } catch (std::exception const& e) {
            m_host.log().error("nfsv3: bind/listen on port {} failed: {}", port, e.what());
            m_acc.reset();
            return Error::INVAL;
        }

        asio::co_spawn(m_ctx, listener(), asio::detached);

        m_running = true;
        unsigned n = std::max(2u, std::thread::hardware_concurrency());
        for (unsigned i = 0; i < n; ++i) {
            m_threads.emplace_back([this] { m_ctx.run(); });
        }
        m_host.log().info("nfsv3: serving on port {} ({} io threads)", port, n);
        return Error::OK;
    }

    void stop() override {
        if (!m_running.exchange(false)) {
            return;
        }
        m_ctx.stop(); // unblock run() on every io thread
        for (auto& t : m_threads) {
            if (t.joinable()) {
                t.join();
            }
        }
        m_threads.clear();
        m_acc.reset();
    }

private:
    //  Accept connections until the acceptor is closed (by stop()).
    asio::awaitable<void> listener() {
        try {
            for (;;) {
                tcp::socket sock = co_await m_acc->async_accept(asio::use_awaitable);
                asio::co_spawn(m_ctx, session(std::move(sock)), asio::detached);
            }
        } catch (std::exception const&) {
            // acceptor closed on shutdown — stop accepting
        }
    }

    //  Serve one connection: read record-marked RPC messages, reply to each.
    //  Locals (mark, msg, frame) live across each co_await in the coroutine
    //  frame, so their buffers stay valid for the async op.
    asio::awaitable<void> session(tcp::socket sock) {
        try {
            for (;;) {
                uint8_t mark[4];
                co_await asio::async_read(sock, asio::buffer(mark, 4), asio::use_awaitable);
                uint32_t len = rd32(mark) & 0x7fffffff; // one fragment per message for now
                if (len < 24 || len > (1u << 20)) {
                    co_return;
                }
                std::vector<uint8_t> msg(len);
                co_await asio::async_read(sock, asio::buffer(msg), asio::use_awaitable);

                Reader r{msg.data(), msg.size()};
                uint32_t xid = r.u32();
                if (r.u32() != RPC_CALL) {
                    co_return;
                }
                r.u32(); // rpcvers (assume 2)
                uint32_t prog = r.u32();
                uint32_t vers = r.u32();
                uint32_t proc = r.u32();
                Cred cred = readCred(r); // AUTH_SYS uid/gid, if present
                r.skipAuth();            // verf

                std::vector<uint8_t> frame = reply(xid, prog, vers, proc, cred, r);
                co_await asio::async_write(sock, asio::buffer(frame), asio::use_awaitable);
            }
        } catch (std::exception const&) {
            // client closed or read/write error — end the session
        }
    }

    //  Build a complete record-marked accepted reply for one call.
    std::vector<uint8_t> reply(uint32_t xid, uint32_t prog, uint32_t vers, uint32_t proc,
                               Cred const& cred, Reader& r) {
        std::vector<uint8_t> result; // proc result, appended only on SUCCESS
        uint32_t astat;
        if (prog == PROG_NFS) {
            astat = vers == NFS_V3 ? nfsCall(proc, cred, r, result) : PROG_MISMATCH;
        } else if (prog == PROG_MOUNT) {
            astat = vers == MOUNT_V3 ? mountCall(proc, r, result) : PROG_MISMATCH;
        } else {
            astat = PROG_UNAVAIL;
        }

        std::vector<uint8_t> rep;
        Writer w{rep};
        w.u32(xid);
        w.u32(RPC_REPLY);
        w.u32(MSG_ACCEPTED);
        w.u32(0); // verf flavor AUTH_NONE
        w.u32(0); // verf length
        w.u32(astat);
        if (astat == PROG_MISMATCH) {
            w.u32(NFS_V3); // low
            w.u32(NFS_V3); // high
        }
        rep.insert(rep.end(), result.begin(), result.end());

        std::vector<uint8_t> frame;
        Writer fw{frame};
        fw.u32(0x80000000u | uint32_t(rep.size())); // last fragment
        frame.insert(frame.end(), rep.begin(), rep.end());
        return frame;
    }

    //  Dispatch one NFS procedure. Returns the RPC accept_stat; on SUCCESS the
    //  XDR-encoded procedure result (starting with nfsstat3) is written to `out`.
    //  One handler per procedure — each reads its args and writes its result.
    uint32_t nfsCall(uint32_t proc, Cred const& cred, Reader& r, std::vector<uint8_t>& out) {
        Writer w{out};
        switch (proc) {
        case NFSPROC3_NULL:
            return SUCCESS; // empty result
        case NFSPROC3_GETATTR:
            return nfsGetattr(r, w);
        case NFSPROC3_SETATTR:
            return nfsSetattr(r, w);
        case NFSPROC3_READLINK:
            return nfsReadlink(r, w);
        case NFSPROC3_LOOKUP:
            return nfsLookup(r, w);
        case NFSPROC3_ACCESS:
            return nfsAccess(r, w);
        case NFSPROC3_READ:
            return nfsRead(r, w);
        case NFSPROC3_WRITE:
            return nfsWrite(r, w);
        case NFSPROC3_CREATE:
            return nfsCreate(cred, r, w);
        case NFSPROC3_MKDIR:
            return nfsMkdir(cred, r, w);
        case NFSPROC3_SYMLINK:
            return nfsSymlink(cred, r, w);
        case NFSPROC3_MKNOD:
            return nfsMknod(cred, r, w);
        case NFSPROC3_REMOVE:
            return nfsRemove(r, w);
        case NFSPROC3_RMDIR:
            return nfsRmdir(r, w);
        case NFSPROC3_RENAME:
            return nfsRename(r, w);
        case NFSPROC3_LINK:
            return nfsLink(r, w);
        case NFSPROC3_COMMIT:
            return nfsCommit(r, w);
        case NFSPROC3_READDIR:
            return nfsReaddir(r, w);
        case NFSPROC3_READDIRPLUS:
            return nfsReaddirPlus(r, w);
        case NFSPROC3_FSSTAT:
            return nfsFsstat(r, w);
        case NFSPROC3_FSINFO:
            return nfsFsinfo(r, w);
        case NFSPROC3_PATHCONF:
            return nfsPathconf(r, w);
        default:
            m_host.log().info("nfsv3: unimplemented NFS proc {}", proc);
            return PROC_UNAVAIL;
        }
    }

    uint32_t nfsGetattr(Reader& r, Writer& w) {
        uint64_t id, snap;
        if (!r.fh(id, snap)) {
            return GARBAGE_ARGS;
        }
        Node n = m_host.fs().nodeById(id, snap);
        if (!n) {
            w.u32(NFS3ERR_STALE);
            return SUCCESS;
        }
        w.u32(NFS3_OK);
        encodeFattr(w, *n);
        return SUCCESS;
    }

    uint32_t nfsReadlink(Reader& r, Writer& w) {
        uint64_t id, snap;
        if (!r.fh(id, snap)) {
            return GARBAGE_ARGS;
        }
        Node n = m_host.fs().nodeById(id, snap);
        if (!n) {
            w.u32(NFS3ERR_STALE);
            return SUCCESS;
        }
        if (n->type() != Type::LNK) {
            w.u32(NFS3ERR_INVAL);
            encodePostOp(w, n.get());
            return SUCCESS;
        }
        w.u32(NFS3_OK);
        encodePostOp(w, n.get()); // symlink_attributes
        w.str(n->target());       // nfspath3 data
        return SUCCESS;
    }

    uint32_t nfsLookup(Reader& r, Writer& w) {
        IPrfs& fs = m_host.fs();
        uint64_t id, snap;
        if (!r.fh(id, snap)) {
            return GARBAGE_ARGS;
        }
        std::string name = r.str();
        if (!r.ok) {
            return GARBAGE_ARGS;
        }
        Node dir = fs.nodeById(id, snap);
        if (!dir) {
            w.u32(NFS3ERR_STALE);
            return SUCCESS;
        }
        if (dir->type() != Type::DIR) {
            w.u32(NFS3ERR_NOTDIR);
            encodePostOp(w, dir.get()); // dir_attributes
            return SUCCESS;
        }
        Node child = fs.lookup(dir, name);
        if (!child) {
            w.u32(NFS3ERR_NOENT);
            encodePostOp(w, dir.get());
            return SUCCESS;
        }
        w.u32(NFS3_OK);
        w.fh(child->id(), snap);      // object fh keeps the dir's snap view
        encodePostOp(w, child.get()); // obj_attributes
        encodePostOp(w, dir.get());   // dir_attributes
        return SUCCESS;
    }

    uint32_t nfsAccess(Reader& r, Writer& w) {
        uint64_t id, snap;
        if (!r.fh(id, snap)) {
            return GARBAGE_ARGS;
        }
        uint32_t want = r.u32();
        if (!r.ok) {
            return GARBAGE_ARGS;
        }
        Node n = m_host.fs().nodeById(id, snap);
        if (!n) {
            w.u32(NFS3ERR_STALE);
            return SUCCESS;
        }
        w.u32(NFS3_OK);
        encodePostOp(w, n.get());
        //  Read-mostly target: grant read/lookup/execute, never modify.
        w.u32(want & (ACCESS3_READ | ACCESS3_LOOKUP | ACCESS3_EXECUTE));
        return SUCCESS;
    }

    uint32_t nfsRead(Reader& r, Writer& w) {
        uint64_t id, snap;
        if (!r.fh(id, snap)) {
            return GARBAGE_ARGS;
        }
        uint64_t off = r.u64();
        uint32_t cnt = r.u32();
        if (!r.ok) {
            return GARBAGE_ARGS;
        }
        Node n = m_host.fs().nodeById(id, snap);
        if (!n) {
            w.u32(NFS3ERR_STALE);
            return SUCCESS;
        }
        if (n->type() == Type::DIR) {
            w.u32(NFS3ERR_ISDIR);
            encodePostOp(w, n.get());
            return SUCCESS;
        }
        if (n->type() != Type::REG) {
            w.u32(NFS3ERR_INVAL);
            encodePostOp(w, n.get());
            return SUCCESS;
        }
        //  The host sources the bytes — procedural content (the RNG provider) or
        //  literal, already clamped to the file size. cnt is capped so a large
        //  request can't force an outsized allocation/reply.
        if (cnt > MAX_READ) {
            cnt = MAX_READ;
        }
        std::vector<char> buf(cnt);
        size_t got = m_host.read(n, off, buf.data(), cnt);
        bool eof = off + got >= n->size();
        w.u32(NFS3_OK);
        encodePostOp(w, n.get()); // file_attributes
        w.u32(uint32_t(got));     // count
        w.u32(eof ? 1 : 0);       // eof
        w.opaque(buf.data(), got);
        return SUCCESS;
    }

    uint32_t nfsReaddir(Reader& r, Writer& w) {
        IPrfs& fs = m_host.fs();
        uint64_t id, snap;
        if (!r.fh(id, snap)) {
            return GARBAGE_ARGS;
        }
        uint64_t cookie = r.u64();
        r.skip(NFS3_COOKIEVERFSIZE); // cookieverf
        uint32_t count = r.u32();    // max reply size
        if (!r.ok) {
            return GARBAGE_ARGS;
        }
        Node dir = fs.nodeById(id, snap);
        if (!dir) {
            w.u32(NFS3ERR_STALE);
            return SUCCESS;
        }
        if (dir->type() != Type::DIR) {
            w.u32(NFS3ERR_NOTDIR);
            encodePostOp(w, dir.get());
            return SUCCESS;
        }
        std::vector<DirEnt> ents = listDir(fs, dir);
        w.u32(NFS3_OK);
        encodePostOp(w, dir.get()); // dir_attributes
        w.u32(0);                   // cookieverf[8]
        w.u32(0);
        size_t budget = 128; // rough allowance for the header already written
        bool eof = true;
        for (DirEnt const& e : ents) {
            if (e.cookie <= cookie) {
                continue;
            }
            size_t esz = 4 + 8 + 4 + pad4(e.name.size()) + 8;
            if (budget + esz > count) {
                eof = false;
                break;
            }
            w.u32(1); // value-follows
            w.u64(e.fileid);
            w.str(e.name);
            w.u64(e.cookie);
            budget += esz;
        }
        w.u32(0); // end of entries
        w.u32(eof ? 1 : 0);
        return SUCCESS;
    }

    uint32_t nfsReaddirPlus(Reader& r, Writer& w) {
        IPrfs& fs = m_host.fs();
        uint64_t id, snap;
        if (!r.fh(id, snap)) {
            return GARBAGE_ARGS;
        }
        uint64_t cookie = r.u64();
        r.skip(NFS3_COOKIEVERFSIZE); // cookieverf
        r.u32();                     // dircount (advisory)
        uint32_t maxcount = r.u32(); // max reply size
        if (!r.ok) {
            return GARBAGE_ARGS;
        }
        Node dir = fs.nodeById(id, snap);
        if (!dir) {
            w.u32(NFS3ERR_STALE);
            return SUCCESS;
        }
        if (dir->type() != Type::DIR) {
            w.u32(NFS3ERR_NOTDIR);
            encodePostOp(w, dir.get());
            return SUCCESS;
        }
        std::vector<DirEnt> ents = listDir(fs, dir);
        w.u32(NFS3_OK);
        encodePostOp(w, dir.get());
        w.u32(0); // cookieverf[8]
        w.u32(0);
        size_t budget = 128;
        bool eof = true;
        for (DirEnt const& e : ents) {
            if (e.cookie <= cookie) {
                continue;
            }
            //  entry + present post_op_attr (1 + fattr3 = 88) + present
            //  post_op_fh3 (1 + fh = 4 + 20).
            size_t esz = 4 + 8 + 4 + pad4(e.name.size()) + 8 + 88 + 24;
            if (budget + esz > maxcount) {
                eof = false;
                break;
            }
            w.u32(1); // value-follows
            w.u64(e.fileid);
            w.str(e.name);
            w.u64(e.cookie);
            encodePostOp(w, e.node.get()); // name_attributes
            w.u32(1);                      // name_handle present
            w.fh(e.node->id(), snap);
            budget += esz;
        }
        w.u32(0); // end of entries
        w.u32(eof ? 1 : 0);
        return SUCCESS;
    }

    uint32_t nfsFsstat(Reader& r, Writer& w) {
        IPrfs& fs = m_host.fs();
        uint64_t id, snap;
        if (!r.fh(id, snap)) {
            return GARBAGE_ARGS;
        }
        Node n = fs.nodeById(id, snap);
        if (!n) {
            w.u32(NFS3ERR_STALE);
            return SUCCESS;
        }
        FsStat st = fsStat(fs, {}, snap); // dynamic usage at this view
        w.u32(NFS3_OK);
        encodePostOp(w, n.get()); // obj_attributes
        w.u64(st.tbytes);
        w.u64(st.fbytes);
        w.u64(st.abytes);
        w.u64(st.tfiles);
        w.u64(st.ffiles);
        w.u64(st.afiles);
        w.u32(st.invarsec);
        return SUCCESS;
    }

    uint32_t nfsFsinfo(Reader& r, Writer& w) {
        uint64_t id, snap;
        if (!r.fh(id, snap)) {
            return GARBAGE_ARGS;
        }
        Node n = m_host.fs().nodeById(id, snap);
        if (!n) {
            w.u32(NFS3ERR_STALE);
            return SUCCESS;
        }
        FsInfo fi = fsInfo(); // static server parameters
        w.u32(NFS3_OK);
        encodePostOp(w, n.get()); // obj_attributes
        w.u32(fi.rtmax);
        w.u32(fi.rtpref);
        w.u32(fi.rtmult);
        w.u32(fi.wtmax);
        w.u32(fi.wtpref);
        w.u32(fi.wtmult);
        w.u32(fi.dtpref);
        w.u64(fi.maxfilesize);
        w.u32(fi.timeDeltaSec);
        w.u32(fi.timeDeltaNsec);
        w.u32(fi.properties);
        return SUCCESS;
    }

    uint32_t nfsPathconf(Reader& r, Writer& w) {
        uint64_t id, snap;
        if (!r.fh(id, snap)) {
            return GARBAGE_ARGS;
        }
        Node n = m_host.fs().nodeById(id, snap);
        if (!n) {
            w.u32(NFS3ERR_STALE);
            return SUCCESS;
        }
        w.u32(NFS3_OK);
        encodePostOp(w, n.get()); // obj_attributes
        w.u32(PATHCONF_LINKMAX);  // linkmax
        w.u32(PATHCONF_NAMEMAX);  // name_max
        w.u32(1);                 // no_trunc — over-long names error, not truncate
        w.u32(1);                 // chown_restricted
        w.u32(0);                 // case_insensitive
        w.u32(1);                 // case_preserving
        return SUCCESS;
    }

    //  Read an sattr3 and apply the set fields to `n` (nullptr just consumes it).
    void applySattr(Reader& r, Node n) {
        if (r.u32()) { // set_mode3
            uint32_t m = r.u32();
            if (n) {
                n->mode(m);
            }
        }
        if (r.u32()) { // set_uid3
            uint32_t u = r.u32();
            if (n) {
                n->uid(u);
            }
        }
        if (r.u32()) { // set_gid3
            uint32_t g = r.u32();
            if (n) {
                n->gid(g);
            }
        }
        if (r.u32()) { // set_size3
            uint64_t s = r.u64();
            if (n) {
                n->size(s);
            }
        }
        uint32_t sa = r.u32(); // set_atime
        if (sa == SET_TO_SERVER_TIME) {
            if (n) {
                n->atime(m_host.fs().now());
            }
        } else if (sa == SET_TO_CLIENT_TIME) {
            uint32_t sec = r.u32();
            r.u32(); // nsec
            if (n) {
                n->atime(sec);
            }
        }
        uint32_t sm = r.u32(); // set_mtime
        if (sm == SET_TO_SERVER_TIME) {
            if (n) {
                n->mtime(m_host.fs().now());
            }
        } else if (sm == SET_TO_CLIENT_TIME) {
            uint32_t sec = r.u32();
            r.u32(); // nsec
            if (n) {
                n->mtime(sec);
            }
        }
    }

    //  CREATE/MKDIR/SYMLINK/MKNOD share this tail: post_op_fh3 + post_op_attr for
    //  the new object, then the parent directory's wcc_data.
    void encodeNewObject(Writer& w, Node child, PreAttr dirPre, INode* dir) {
        w.u32(1); // handle-follows
        w.fh(child->id(), LATEST);
        encodePostOp(w, child.get()); // obj_attributes
        encodeWcc(w, dirPre, dir);    // dir_wcc
    }

    uint32_t nfsSetattr(Reader& r, Writer& w) {
        IPrfs& fs = m_host.fs();
        uint64_t id, snap;
        if (!r.fh(id, snap)) {
            return GARBAGE_ARGS;
        }
        Node n = fs.nodeById(id, snap);
        if (!n) {
            w.u32(NFS3ERR_STALE);
            encodeWcc(w, {}, nullptr);
            return SUCCESS;
        }
        if (!live(snap)) {
            w.u32(NFS3ERR_ROFS);
            encodeWcc(w, preOf(n.get()), n.get());
            return SUCCESS;
        }
        PreAttr pre = preOf(n.get());
        applySattr(r, n);
        if (r.u32()) { // sattrguard3 — ignore the ctime check
            r.u32();
            r.u32();
        }
        n->ctime(fs.now());
        w.u32(NFS3_OK);
        encodeWcc(w, pre, n.get());
        return SUCCESS;
    }

    uint32_t nfsWrite(Reader& r, Writer& w) {
        IPrfs& fs = m_host.fs();
        uint64_t id, snap;
        if (!r.fh(id, snap)) {
            return GARBAGE_ARGS;
        }
        uint64_t off = r.u64();
        r.u32();                    // count (data length is authoritative)
        r.u32();                    // stable_how
        std::string data = r.str(); // data<>
        if (!r.ok) {
            return GARBAGE_ARGS;
        }
        Node n = fs.nodeById(id, snap);
        if (!n) {
            w.u32(NFS3ERR_STALE);
            encodeWcc(w, {}, nullptr);
            return SUCCESS;
        }
        if (n->type() != Type::REG) {
            w.u32(NFS3ERR_INVAL);
            encodeWcc(w, preOf(n.get()), n.get());
            return SUCCESS;
        }
        if (!live(snap)) {
            w.u32(NFS3ERR_ROFS);
            encodeWcc(w, preOf(n.get()), n.get());
            return SUCCESS;
        }
        if (off + data.size() > MAX_WRITE_FILE) {
            w.u32(NFS3ERR_FBIG);
            encodeWcc(w, preOf(n.get()), n.get());
            return SUCCESS;
        }
        PreAttr pre = preOf(n.get());
        //  Splice into the node's literal content (whole-file model; procedural
        //  content is a read-time concern).
        std::string c = n->content();
        if (off + data.size() > c.size()) {
            c.resize(off + data.size(), '\0');
        }
        std::copy(data.begin(), data.end(), c.begin() + off);
        fs.setContent(n, c);
        if (off + data.size() > n->size()) {
            n->size(off + data.size());
        }
        n->mtime(fs.now());
        w.u32(NFS3_OK);
        encodeWcc(w, pre, n.get());
        w.u32(uint32_t(data.size())); // count written
        w.u32(WRITE_FILE_SYNC);       // committed
        w.u32(0);                     // writeverf[8]
        w.u32(0);
        return SUCCESS;
    }

    //  Shared preamble for the verbs that take a diropargs3 (CREATE/MKDIR/
    //  SYMLINK/MKNOD/REMOVE/RMDIR): read (dir fh, name), resolve and validate the
    //  parent directory. On error it writes the failing dir_wcc and returns the
    //  nfsstat3; on success it returns NFS3_OK with `dir`/`pre`/`name` set.
    uint32_t diropParent(Reader& r, Writer& w, Node& dir, PreAttr& pre, std::string& name) {
        IPrfs& fs = m_host.fs();
        uint64_t id, snap;
        if (!r.fh(id, snap)) {
            return GARBAGE_ARGS;
        }
        name = r.str();
        if (!r.ok) {
            return GARBAGE_ARGS;
        }
        dir = fs.nodeById(id, snap);
        if (!dir) {
            w.u32(NFS3ERR_STALE);
            encodeWcc(w, {}, nullptr);
            return NFS3ERR_STALE;
        }
        if (dir->type() != Type::DIR) {
            w.u32(NFS3ERR_NOTDIR);
            encodeWcc(w, preOf(dir.get()), dir.get());
            return NFS3ERR_NOTDIR;
        }
        if (!live(snap)) {
            w.u32(NFS3ERR_ROFS);
            encodeWcc(w, preOf(dir.get()), dir.get());
            return NFS3ERR_ROFS;
        }
        pre = preOf(dir.get());
        return NFS3_OK;
    }

    //  Finish a create verb: link the new child, then encode the result.
    uint32_t finishCreate(Writer& w, Node dir, PreAttr pre, std::string const& name, Node child) {
        Error e = m_host.fs().link(dir, name, child);
        if (e != Error::OK) {
            w.u32(toNfsStat(e));
            encodeWcc(w, pre, dir.get());
            return SUCCESS;
        }
        w.u32(NFS3_OK);
        encodeNewObject(w, child, pre, dir.get());
        return SUCCESS;
    }

    uint32_t nfsCreate(Cred const& cred, Reader& r, Writer& w) {
        IPrfs& fs = m_host.fs();
        Node dir;
        PreAttr pre;
        std::string name;
        if (diropParent(r, w, dir, pre, name) != NFS3_OK) {
            return SUCCESS;
        }
        uint32_t mode = r.u32(); // createmode3
        if (!r.ok) {
            return GARBAGE_ARGS;
        }
        Node existing = fs.lookup(dir, name);
        if (existing) {
            if (mode == CREATE_GUARDED) {
                w.u32(NFS3ERR_EXIST);
                encodeWcc(w, pre, dir.get());
                return SUCCESS;
            }
            if (mode == CREATE_EXCLUSIVE) {
                r.skip(8); // createverf3
            } else {
                applySattr(r, existing);
            }
            w.u32(NFS3_OK);
            encodeNewObject(w, existing, pre, dir.get());
            return SUCCESS;
        }
        Node child = fs.mkfile("");
        applyCred(cred, child);
        if (mode == CREATE_EXCLUSIVE) {
            r.skip(8); // createverf3
        } else {
            applySattr(r, child);
        }
        return finishCreate(w, dir, pre, name, child);
    }

    uint32_t nfsMkdir(Cred const& cred, Reader& r, Writer& w) {
        IPrfs& fs = m_host.fs();
        Node dir;
        PreAttr pre;
        std::string name;
        if (diropParent(r, w, dir, pre, name) != NFS3_OK) {
            return SUCCESS;
        }
        Node child = fs.mkdir();
        applyCred(cred, child);
        applySattr(r, child); // sattr3
        return finishCreate(w, dir, pre, name, child);
    }

    uint32_t nfsSymlink(Cred const& cred, Reader& r, Writer& w) {
        IPrfs& fs = m_host.fs();
        Node dir;
        PreAttr pre;
        std::string name;
        if (diropParent(r, w, dir, pre, name) != NFS3_OK) {
            return SUCCESS;
        }
        Node child = fs.symlink("");
        applyCred(cred, child);
        applySattr(r, child);         // symlink_attributes (sattr3)
        std::string target = r.str(); // nfspath3 symlink_data
        if (!r.ok) {
            return GARBAGE_ARGS;
        }
        fs.setTarget(child, target);
        return finishCreate(w, dir, pre, name, child);
    }

    uint32_t nfsMknod(Cred const& cred, Reader& r, Writer& w) {
        IPrfs& fs = m_host.fs();
        Node dir;
        PreAttr pre;
        std::string name;
        if (diropParent(r, w, dir, pre, name) != NFS3_OK) {
            return SUCCESS;
        }
        uint32_t type = r.u32(); // ftype3
        Node child;
        if (type == NF3BLK || type == NF3CHR) {
            child = fs.mknod(type == NF3BLK ? Type::BLK : Type::CHR, 0, 0);
            applyCred(cred, child);
            applySattr(r, child); // dev_attributes
            uint32_t maj = r.u32();
            uint32_t min = r.u32(); // specdata3
            fs.setRdev(child, maj, min);
        } else if (type == NF3FIFO) {
            child = fs.mkfifo();
            applyCred(cred, child);
            applySattr(r, child);
        } else if (type == NF3SOCK) {
            child = fs.mksock();
            applyCred(cred, child);
            applySattr(r, child);
        } else {
            w.u32(NFS3ERR_BADTYPE);
            encodeWcc(w, pre, dir.get());
            return SUCCESS;
        }
        return finishCreate(w, dir, pre, name, child);
    }

    uint32_t nfsRemove(Reader& r, Writer& w) {
        IPrfs& fs = m_host.fs();
        Node dir;
        PreAttr pre;
        std::string name;
        if (diropParent(r, w, dir, pre, name) != NFS3_OK) {
            return SUCCESS;
        }
        Error e = fs.unlink(dir, name);
        w.u32(toNfsStat(e));
        encodeWcc(w, pre, dir.get());
        return SUCCESS;
    }

    uint32_t nfsRmdir(Reader& r, Writer& w) {
        IPrfs& fs = m_host.fs();
        Node dir;
        PreAttr pre;
        std::string name;
        if (diropParent(r, w, dir, pre, name) != NFS3_OK) {
            return SUCCESS;
        }
        Node child = fs.lookup(dir, name);
        uint32_t st = NFS3_OK;
        if (!child) {
            st = NFS3ERR_NOENT;
        } else if (child->type() != Type::DIR) {
            st = NFS3ERR_NOTDIR;
        } else if (!fs.readdir(child).empty()) {
            st = NFS3ERR_NOTEMPTY;
        } else {
            st = toNfsStat(fs.unlink(dir, name));
        }
        w.u32(st);
        encodeWcc(w, pre, dir.get());
        return SUCCESS;
    }

    uint32_t nfsRename(Reader& r, Writer& w) {
        IPrfs& fs = m_host.fs();
        uint64_t fid, fsnap;
        if (!r.fh(fid, fsnap)) {
            return GARBAGE_ARGS;
        }
        std::string fromName = r.str();
        uint64_t tid, tsnap;
        if (!r.fh(tid, tsnap)) {
            return GARBAGE_ARGS;
        }
        std::string toName = r.str();
        if (!r.ok) {
            return GARBAGE_ARGS;
        }
        Node fromDir = fs.nodeById(fid, fsnap);
        Node toDir = fs.nodeById(tid, tsnap);
        if (!fromDir || !toDir) {
            w.u32(NFS3ERR_STALE);
            encodeWcc(w, preOf(fromDir ? fromDir.get() : nullptr), fromDir.get());
            encodeWcc(w, preOf(toDir ? toDir.get() : nullptr), toDir.get());
            return SUCCESS;
        }
        PreAttr fpre = preOf(fromDir.get());
        PreAttr tpre = preOf(toDir.get());
        uint32_t st = NFS3ERR_ROFS;
        if (live(fsnap) && live(tsnap)) {
            st = toNfsStat(fs.move(fromDir, fromName, toDir, toName));
        }
        w.u32(st);
        encodeWcc(w, fpre, fromDir.get()); // fromdir_wcc
        encodeWcc(w, tpre, toDir.get());   // todir_wcc
        return SUCCESS;
    }

    uint32_t nfsLink(Reader& r, Writer& w) {
        IPrfs& fs = m_host.fs();
        uint64_t fid, fsnap;
        if (!r.fh(fid, fsnap)) {
            return GARBAGE_ARGS;
        }
        uint64_t did, dsnap;
        if (!r.fh(did, dsnap)) {
            return GARBAGE_ARGS;
        }
        std::string name = r.str();
        if (!r.ok) {
            return GARBAGE_ARGS;
        }
        Node file = fs.nodeById(fid, fsnap);
        Node dir = fs.nodeById(did, dsnap);
        if (!file || !dir) {
            w.u32(NFS3ERR_STALE);
            encodePostOp(w, file.get());
            encodeWcc(w, preOf(dir ? dir.get() : nullptr), dir.get());
            return SUCCESS;
        }
        PreAttr pre = preOf(dir.get());
        uint32_t st = live(dsnap) ? toNfsStat(fs.link(dir, name, file)) : NFS3ERR_ROFS;
        w.u32(st);
        encodePostOp(w, file.get());  // file_attributes
        encodeWcc(w, pre, dir.get()); // linkdir_wcc
        return SUCCESS;
    }

    uint32_t nfsCommit(Reader& r, Writer& w) {
        uint64_t id, snap;
        if (!r.fh(id, snap)) {
            return GARBAGE_ARGS;
        }
        r.u64(); // offset
        r.u32(); // count
        Node n = m_host.fs().nodeById(id, snap);
        if (!n) {
            w.u32(NFS3ERR_STALE);
            encodeWcc(w, {}, nullptr);
            return SUCCESS;
        }
        //  Writes are applied synchronously, so COMMIT is a no-op success.
        w.u32(NFS3_OK);
        encodeWcc(w, preOf(n.get()), n.get());
        w.u32(0); // writeverf[8]
        w.u32(0);
        return SUCCESS;
    }

    //  Dispatch one MOUNT procedure. A single synthetic export, "/", whose MNT
    //  hands back the root filehandle every NFS call then builds on.
    uint32_t mountCall(uint32_t proc, Reader& r, std::vector<uint8_t>& out) {
        Writer w{out};
        switch (proc) {
        case MOUNTPROC3_NULL:
            return SUCCESS;
        case MOUNTPROC3_MNT:
            return mountMnt(r, w);
        case MOUNTPROC3_UMNT:
            return mountUmnt(r, w);
        case MOUNTPROC3_EXPORT:
            return mountExport(r, w);
        default:
            m_host.log().info("nfsv3: unimplemented MOUNT proc {}", proc);
            return PROC_UNAVAIL;
        }
    }

    uint32_t mountMnt(Reader& r, Writer& w) {
        std::string path = r.str(); // dirpath (ignored — one export)
        if (!r.ok) {
            return GARBAGE_ARGS;
        }
        Node root = m_host.fs().rwRoot();
        m_host.log().info("nfsv3: MNT \"{}\" -> root fileid {}", path, root->id());
        w.u32(MNT3_OK);
        w.fh(root->id(), LATEST); // fhandle3 — live root view
        w.u32(2);                 // auth_flavors count
        w.u32(0);                 // AUTH_NONE
        w.u32(1);                 // AUTH_SYS
        return SUCCESS;
    }

    uint32_t mountUmnt(Reader& r, Writer&) {
        std::string path = r.str();
        if (!r.ok) {
            return GARBAGE_ARGS;
        }
        m_host.log().info("nfsv3: UMNT \"{}\"", path);
        return SUCCESS; // void result
    }

    uint32_t mountExport(Reader&, Writer& w) {
        //  exportnode list: one entry "/" with no group restriction.
        w.u32(1);   // value-follows
        w.str("/"); // ex_dir
        w.u32(0);   // ex_groups: none
        w.u32(0);   // end of list
        return SUCCESS;
    }

    IHost& m_host;
    asio::io_context m_ctx;
    std::optional<tcp::acceptor> m_acc;
    std::vector<std::thread> m_threads;
    std::atomic<bool> m_running{false};
};

class NfsV3Plugin : public IPlugin {
public:
    explicit NfsV3Plugin(IHost& host)
        : m_host(host)
        , m_svc(host) {
        m_host.registry().provide<IService>(&m_svc, "nfsv3");
    }

    ~NfsV3Plugin() override { m_host.registry().withdraw<IService>("nfsv3"); }

    char const* name() const override { return "nfsv3"; }

    char const* version() const override { return "0.1"; }

private:
    IHost& m_host;
    NfsV3 m_svc;
};

} // namespace

extern "C" uint32_t prfs_abi(void) { return prfs::plugin::ABI; }

extern "C" prfs::plugin::IPlugin* prfs_plugin_create(prfs::plugin::IHost& host) {
    return new NfsV3Plugin(host);
}

extern "C" void prfs_plugin_destroy(prfs::plugin::IPlugin* p) { delete p; }
