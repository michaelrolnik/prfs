// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  nfsv3 — an NFSv3 front-end as a prfs service plugin (todo L2). An Asio TCP
//  server that speaks ONC-RPC record marking and dispatches, on the one
//  listener, both the MOUNT program (100005 v3: MNT/UMNT/EXPORT) and the NFS
//  program (100003 v3). Implemented so far: MOUNT MNT hands out the root
//  filehandle for the single export "/"; NFS NULL, GETATTR, LOOKUP, ACCESS let a
//  client walk the tree and stat nodes, and READ returns file bytes (sourced by
//  the host — procedural content or literal). Remaining procedures (READDIR,
//  FSSTAT/FSINFO, READLINK, …) build on the same dispatch.
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
#include "prfs/plugin.hpp"

#include <asio.hpp>
#include <spdlog/spdlog.h>

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
constexpr uint32_t NFSPROC3_NULL = 0, NFSPROC3_GETATTR = 1, NFSPROC3_LOOKUP = 3,
                   NFSPROC3_ACCESS = 4, NFSPROC3_READ = 6;

//  Largest READ we answer in one reply (also the rtmax FSINFO will advertise).
constexpr uint32_t MAX_READ = 1u << 20;

//  MOUNT program (RFC 1813 appendix I) — v3 only. NFSv4 dropped MOUNT entirely
//  (PUTROOTFH inside COMPOUND replaces it), so this lives with nfsv3, not apart.
//  Same listener as NFS; dispatch keys on the RPC program number.
constexpr uint32_t PROG_MOUNT = 100005;
constexpr uint32_t MOUNT_V3 = 3;
constexpr uint32_t MOUNTPROC3_NULL = 0, MOUNTPROC3_MNT = 1, MOUNTPROC3_UMNT = 3,
                   MOUNTPROC3_EXPORT = 5;
constexpr uint32_t MNT3_OK = 0;

//  A subset of nfsstat3.
constexpr uint32_t NFS3_OK = 0, NFS3ERR_NOENT = 2, NFS3ERR_ISDIR = 21, NFS3ERR_INVAL = 22,
                   NFS3ERR_NOTDIR = 20, NFS3ERR_STALE = 70;

//  access3 bits — what this synthetic (read-mostly) target grants.
constexpr uint32_t ACCESS3_READ = 0x0001, ACCESS3_LOOKUP = 0x0002, ACCESS3_EXECUTE = 0x0020;

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
                r.skipAuth(); // cred
                r.skipAuth(); // verf

                std::vector<uint8_t> frame = reply(xid, prog, vers, proc, r);
                co_await asio::async_write(sock, asio::buffer(frame), asio::use_awaitable);
            }
        } catch (std::exception const&) {
            // client closed or read/write error — end the session
        }
    }

    //  Build a complete record-marked accepted reply for one call.
    std::vector<uint8_t> reply(uint32_t xid, uint32_t prog, uint32_t vers, uint32_t proc,
                               Reader& r) {
        std::vector<uint8_t> result; // proc result, appended only on SUCCESS
        uint32_t astat;
        if (prog == PROG_NFS) {
            astat = vers == NFS_V3 ? nfsCall(proc, r, result) : PROG_MISMATCH;
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
    uint32_t nfsCall(uint32_t proc, Reader& r, std::vector<uint8_t>& out) {
        IPrfs& fs = m_host.fs();
        Writer w{out};

        switch (proc) {
        case NFSPROC3_NULL:
            return SUCCESS; // empty result

        case NFSPROC3_GETATTR: {
            uint64_t id, snap;
            if (!r.fh(id, snap)) {
                return GARBAGE_ARGS;
            }
            Node n = fs.nodeById(id, snap);
            if (!n) {
                w.u32(NFS3ERR_STALE);
                return SUCCESS;
            }
            w.u32(NFS3_OK);
            encodeFattr(w, *n);
            return SUCCESS;
        }

        case NFSPROC3_LOOKUP: {
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

        case NFSPROC3_ACCESS: {
            uint64_t id, snap;
            if (!r.fh(id, snap)) {
                return GARBAGE_ARGS;
            }
            uint32_t want = r.u32();
            if (!r.ok) {
                return GARBAGE_ARGS;
            }
            Node n = fs.nodeById(id, snap);
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

        case NFSPROC3_READ: {
            uint64_t id, snap;
            if (!r.fh(id, snap)) {
                return GARBAGE_ARGS;
            }
            uint64_t off = r.u64();
            uint32_t cnt = r.u32();
            if (!r.ok) {
                return GARBAGE_ARGS;
            }
            Node n = fs.nodeById(id, snap);
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
            //  The host sources the bytes — procedural content (the RNG provider)
            //  or literal, already clamped to the file size. cnt is capped so a
            //  large request can't force an outsized allocation/reply.
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

        default:
            m_host.log().info("nfsv3: unimplemented NFS proc {}", proc);
            return PROC_UNAVAIL;
        }
    }

    //  Dispatch one MOUNT procedure. A single synthetic export, "/", whose MNT
    //  hands back the root filehandle every NFS call then builds on.
    uint32_t mountCall(uint32_t proc, Reader& r, std::vector<uint8_t>& out) {
        IPrfs& fs = m_host.fs();
        Writer w{out};

        switch (proc) {
        case MOUNTPROC3_NULL:
            return SUCCESS;

        case MOUNTPROC3_MNT: {
            std::string path = r.str(); // dirpath (ignored — one export)
            if (!r.ok) {
                return GARBAGE_ARGS;
            }
            Node root = fs.rwRoot();
            m_host.log().info("nfsv3: MNT \"{}\" -> root fileid {}", path, root->id());
            w.u32(MNT3_OK);
            w.fh(root->id(), LATEST); // fhandle3 — live root view
            w.u32(2);                 // auth_flavors count
            w.u32(0);                 // AUTH_NONE
            w.u32(1);                 // AUTH_SYS
            return SUCCESS;
        }

        case MOUNTPROC3_UMNT: {
            std::string path = r.str();
            if (!r.ok) {
                return GARBAGE_ARGS;
            }
            m_host.log().info("nfsv3: UMNT \"{}\"", path);
            return SUCCESS; // void result
        }

        case MOUNTPROC3_EXPORT: {
            //  exportnode list: one entry "/" with no group restriction.
            w.u32(1);   // value-follows
            w.str("/"); // ex_dir
            w.u32(0);   // ex_groups: none
            w.u32(0);   // end of list
            return SUCCESS;
        }

        default:
            m_host.log().info("nfsv3: unimplemented MOUNT proc {}", proc);
            return PROC_UNAVAIL;
        }
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
