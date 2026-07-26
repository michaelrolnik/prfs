// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  nfsv3 — an NFSv3 front-end as a prfs service plugin (todo L2). A TCP server
//  (its own thread) that speaks ONC-RPC record marking and dispatches, on the
//  one listener, both the MOUNT program (100005 v3: MNT/UMNT/EXPORT) and the NFS
//  program (100003 v3). Implemented so far: MOUNT MNT hands out the root
//  filehandle for the single export "/"; NFS NULL, GETATTR, LOOKUP, ACCESS let a
//  client walk the tree and stat nodes. Remaining NFS procedures (READ, READDIR,
//  FSSTAT/FSINFO, …) build on the same dispatch.
//
//  Filehandle (nfs_fh3): 16 opaque bytes = big-endian (nodeID, snapId). Decoded
//  back to a node via IPrfs::nodeById(); a null result maps to NFS3ERR_STALE.
//  Keeping snapId in the fh means a handle taken against a snapshot keeps reading
//  that frozen view, while LATEST fh's track the live tree.
//
//  Threading note (docs L2): one accept+serve thread for now — a thread pool is
//  the next step. Port comes from the "port" option (default 2049; 2049 needs
//  privileges, so tests use a high port).
//
#include "prfs/plugin.hpp"

#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace prfs;
using namespace prfs::plugin;

//  ONC-RPC (RFC 5531) message + accept_stat constants.
constexpr uint32_t RPC_CALL = 0, RPC_REPLY = 1;
constexpr uint32_t MSG_ACCEPTED = 0;
constexpr uint32_t SUCCESS = 0, PROG_UNAVAIL = 1, PROG_MISMATCH = 2, PROC_UNAVAIL = 3,
                   GARBAGE_ARGS = 4;

//  NFSv3 program (RFC 1813).
constexpr uint32_t PROG_NFS = 100003;
constexpr uint32_t NFS_V3 = 3;
constexpr uint32_t NFSPROC3_NULL = 0, NFSPROC3_GETATTR = 1, NFSPROC3_LOOKUP = 3,
                   NFSPROC3_ACCESS = 4;

//  MOUNT program (RFC 1813 appendix I) — v3 only. NFSv4 dropped MOUNT entirely
//  (PUTROOTFH inside COMPOUND replaces it), so this lives with nfsv3, not apart.
//  Same listener as NFS; dispatch keys on the RPC program number.
constexpr uint32_t PROG_MOUNT = 100005;
constexpr uint32_t MOUNT_V3 = 3;
constexpr uint32_t MOUNTPROC3_NULL = 0, MOUNTPROC3_MNT = 1, MOUNTPROC3_UMNT = 3,
                   MOUNTPROC3_EXPORT = 5;
constexpr uint32_t MNT3_OK = 0;

//  A subset of nfsstat3.
constexpr uint32_t NFS3_OK = 0, NFS3ERR_NOENT = 2, NFS3ERR_NOTDIR = 20, NFS3ERR_STALE = 70;

//  access3 bits — what this synthetic (read-mostly) target grants.
constexpr uint32_t ACCESS3_READ = 0x0001, ACCESS3_LOOKUP = 0x0002, ACCESS3_EXECUTE = 0x0020;

bool recvAll(int fd, void* buf, size_t n) {
    auto* p = static_cast<char*>(buf);
    while (n) {
        ssize_t r = ::recv(fd, p, n, 0);
        if (r <= 0) {
            return false;
        }
        p += r;
        n -= size_t(r);
    }
    return true;
}

bool sendAll(int fd, void const* buf, size_t n) {
    auto* p = static_cast<char const*>(buf);
    while (n) {
        ssize_t r = ::send(fd, p, n, MSG_NOSIGNAL);
        if (r <= 0) {
            return false;
        }
        p += r;
        n -= size_t(r);
    }
    return true;
}

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
};

//  prfs Type → NFSv3 ftype3.
uint32_t ftype3(Type t) {
    switch (t) {
    case Type::REG:
        return 1;
    case Type::DIR:
        return 2;
    case Type::BLK:
        return 3;
    case Type::CHR:
        return 4;
    case Type::LNK:
        return 5;
    case Type::SOCK:
        return 6;
    case Type::FIFO:
        return 7;
    }
    return 1;
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

        m_listen = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_listen < 0) {
            return Error::INVAL;
        }
        int on = 1;
        ::setsockopt(m_listen, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(uint16_t(port));
        if (::bind(m_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
            ::listen(m_listen, 16) < 0) {
            m_host.log().error("nfsv3: bind/listen on port {} failed", port);
            ::close(m_listen);
            m_listen = -1;
            return Error::INVAL;
        }

        m_running = true;
        m_thread = std::thread([this] { serve(); });
        m_host.log().info("nfsv3: serving on port {}", port);
        return Error::OK;
    }

    void stop() override {
        if (!m_running.exchange(false)) {
            return;
        }
        if (m_listen >= 0) {
            ::shutdown(m_listen, SHUT_RDWR); // break accept()
            ::close(m_listen);
            m_listen = -1;
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

private:
    void serve() {
        while (m_running) {
            int c = ::accept(m_listen, nullptr, nullptr);
            if (c < 0) {
                break; // listen fd closed by stop()
            }
            handle(c);
            ::close(c);
        }
    }

    //  Serve one connection: read record-marked RPC messages, reply to each.
    void handle(int c) {
        for (;;) {
            uint8_t mark[4];
            if (!recvAll(c, mark, 4)) {
                return;
            }
            uint32_t len = rd32(mark) & 0x7fffffff; // one fragment per message for now
            if (len < 24 || len > (1u << 20)) {
                return;
            }
            std::vector<uint8_t> msg(len);
            if (!recvAll(c, msg.data(), len)) {
                return;
            }

            Reader r{msg.data(), msg.size()};
            uint32_t xid = r.u32();
            if (r.u32() != RPC_CALL) {
                return;
            }
            r.u32(); // rpcvers (assume 2)
            uint32_t prog = r.u32();
            uint32_t vers = r.u32();
            uint32_t proc = r.u32();
            r.skipAuth(); // cred
            r.skipAuth(); // verf

            std::vector<uint8_t> frame = reply(xid, prog, vers, proc, r);
            if (!sendAll(c, frame.data(), frame.size())) {
                return;
            }
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
    int m_listen = -1;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
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
