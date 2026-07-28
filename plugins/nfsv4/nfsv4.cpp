// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  nfsv4 — an NFSv4.0 front-end as a prfs service plugin (todo L2). NFSv4 folds
//  MOUNT away (PUTROOTFH replaces it) and batches operations into a single
//  COMPOUND RPC over the NFS program (100003) version 4. This first increment
//  implements the browse/read path: the COMPOUND framework with a current- and
//  saved-filehandle, plus PUTROOTFH/PUTPUBFH/PUTFH/GETFH/SAVEFH/RESTOREFH,
//  LOOKUP/LOOKUPP, ACCESS, GETATTR (fattr4 attribute bitmaps), READLINK, READ,
//  READDIR, and the minimal client-state ops a Linux client issues at mount
//  (SETCLIENTID/SETCLIENTID_CONFIRM/RENEW/OPEN/CLOSE). The write surface and full
//  lock/share state are follow-ups; unimplemented ops return NFS4ERR_NOTSUPP.
//
//  Filehandle: the same 16 opaque bytes as nfsv3 = big-endian (nodeID, snapId),
//  decoded via IPrfs::nodeById(); a snapshot view (snapId != LATEST) is read-only.
//  Transport: standalone-Asio coroutines, one io_context on a thread pool, ONC-RPC
//  record marking — the nfsv3 model. Port from the "port" option (default 2049).
//
#include "prfs/plugin.hpp"

#include <asio.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace prfs;
using namespace prfs::plugin;
using asio::ip::tcp;

//  ONC-RPC (RFC 5531).
constexpr uint32_t RPC_CALL = 0;
constexpr uint32_t SUCCESS = 0, GARBAGE_ARGS = 4;
constexpr uint32_t AUTH_SYS = 1;

//  NFSv4 program (RFC 7530). No MOUNT program.
constexpr uint32_t PROG_NFS = 100003;
constexpr uint32_t NFS_V4 = 4;
constexpr uint32_t NFSPROC4_NULL = 0, NFSPROC4_COMPOUND = 1;

//  nfs_opnum4 (subset).
enum : uint32_t {
    OP_ACCESS = 3,
    OP_CLOSE = 4,
    OP_GETATTR = 9,
    OP_GETFH = 10,
    OP_LOOKUP = 15,
    OP_LOOKUPP = 16,
    OP_OPEN = 18,
    OP_PUTFH = 22,
    OP_PUTPUBFH = 23,
    OP_PUTROOTFH = 24,
    OP_READ = 25,
    OP_READDIR = 26,
    OP_READLINK = 27,
    OP_RENEW = 30,
    OP_RESTOREFH = 31,
    OP_SAVEFH = 32,
    OP_SETCLIENTID = 35,
    OP_SETCLIENTID_CONFIRM = 36,
};

//  nfsstat4 (subset).
constexpr uint32_t NFS4_OK = 0, NFS4ERR_PERM = 1, NFS4ERR_NOENT = 2, NFS4ERR_IO = 5,
                   NFS4ERR_EXIST = 17, NFS4ERR_NOTDIR = 20, NFS4ERR_ISDIR = 21, NFS4ERR_INVAL = 22,
                   NFS4ERR_ROFS = 30, NFS4ERR_NOTEMPTY = 66, NFS4ERR_STALE = 70,
                   NFS4ERR_BADHANDLE = 10001, NFS4ERR_NOTSUPP = 10004, NFS4ERR_RESOURCE = 10018,
                   NFS4ERR_NOFILEHANDLE = 10020, NFS4ERR_OP_ILLEGAL = 10044;

//  nfs_ftype4.
constexpr uint32_t NF4REG = 1, NF4DIR = 2, NF4BLK = 3, NF4CHR = 4, NF4LNK = 5, NF4SOCK = 6,
                   NF4FIFO = 7;

//  fattr4 attribute bit numbers (RFC 7530 §5) we support.
enum : uint32_t {
    FA_SUPPORTED_ATTRS = 0,
    FA_TYPE = 1,
    FA_FH_EXPIRE_TYPE = 2,
    FA_CHANGE = 3,
    FA_SIZE = 4,
    FA_LINK_SUPPORT = 5,
    FA_SYMLINK_SUPPORT = 6,
    FA_NAMED_ATTR = 7,
    FA_FSID = 8,
    FA_UNIQUE_HANDLES = 9,
    FA_LEASE_TIME = 10,
    FA_RDATTR_ERROR = 11,
    FA_FILEHANDLE = 19,
    FA_FILEID = 20,
    FA_MAXFILESIZE = 27,
    FA_MAXLINK = 28,
    FA_MAXNAME = 29,
    FA_MAXREAD = 30,
    FA_MAXWRITE = 31,
    FA_MODE = 33,
    FA_NO_TRUNC = 34,
    FA_NUMLINKS = 35,
    FA_OWNER = 36,
    FA_OWNER_GROUP = 37,
    FA_SPACE_USED = 45,
    FA_TIME_ACCESS = 47,
    FA_TIME_METADATA = 52,
    FA_TIME_MODIFY = 53,
    FA_MOUNTED_ON_FILEID = 55,
};

constexpr uint32_t LEASE_TIME = 90;
constexpr uint32_t MAX_IO = 1u << 20; // rd/wr transfer ceiling

//  --- XDR reader over one COMPOUND message; `ok` latches false on underflow. ---
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
        uint32_t v = uint32_t(p[pos]) << 24 | uint32_t(p[pos + 1]) << 16 |
                     uint32_t(p[pos + 2]) << 8 | p[pos + 3];
        pos += 4;
        return v;
    }

    uint64_t u64() {
        uint64_t hi = u32();
        return hi << 32 | u32();
    }

    std::string str() { // utf8str/opaque<>
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

    void skip(size_t k) {
        if (pos + k > n) {
            ok = false;
            return;
        }
        pos += k;
    }

    void skipOpaque() { // opaque<> length-prefixed
        uint32_t len = u32();
        skip((len + 3u) & ~size_t(3));
    }

    void skipAuth() {
        u32();
        skipOpaque();
    }

    //  bitmap4 → the words (each covers 32 attrs).
    std::vector<uint32_t> bitmap() {
        uint32_t cnt = u32();
        std::vector<uint32_t> w;
        if (cnt > 16) { // sanity
            ok = false;
            return w;
        }
        for (uint32_t i = 0; i < cnt; ++i) {
            w.push_back(u32());
        }
        return w;
    }

    //  nfs_fh4: 16-byte (nodeID, snapId).
    bool fh(uint64_t& id, uint64_t& snap) {
        uint32_t len = u32();
        if (!ok || len != 16 || pos + 16 > n) {
            ok = false;
            return false;
        }
        id = u64();
        snap = u64();
        return true;
    }
};

//  --- XDR writer, big-endian. ---
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

    void str(std::string const& s) {
        u32(uint32_t(s.size()));
        v.insert(v.end(), s.begin(), s.end());
        while (v.size() % 4) {
            v.push_back(0);
        }
    }

    void bytes(uint8_t const* b, size_t len) {
        u32(uint32_t(len));
        v.insert(v.end(), b, b + len);
        while (v.size() % 4) {
            v.push_back(0);
        }
    }

    void fh(uint64_t id, uint64_t snap) {
        u32(16);
        u64(id);
        u64(snap);
    }

    void time(uint64_t sec, uint32_t nsec) { // nfstime4 { int64 seconds; uint32 nseconds; }
        u64(sec);
        u32(nsec);
    }
};

bool live(uint64_t snap) { return snap == LATEST; }

//  Distinct display fileid per (id, snap): live keeps the raw id; a snapshot view
//  mixes in the snapId so it can't collide with the live node (as in nfsv3).
uint64_t viewFileid(uint64_t id, uint64_t snap) {
    if (snap == LATEST) {
        return id;
    }
    uint64_t x = id * 0x9E3779B97F4A7C15ull + snap;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

uint32_t ftype4(Type t) {
    switch (t) {
    case Type::REG:
        return NF4REG;
    case Type::DIR:
        return NF4DIR;
    case Type::LNK:
        return NF4LNK;
    case Type::BLK:
        return NF4BLK;
    case Type::CHR:
        return NF4CHR;
    case Type::SOCK:
        return NF4SOCK;
    case Type::FIFO:
        return NF4FIFO;
    }
    return NF4REG;
}

uint32_t toNfs4(Error e) {
    switch (e) {
    case Error::OK:
        return NFS4_OK;
    case Error::NOENT:
        return NFS4ERR_NOENT;
    case Error::EXIST:
        return NFS4ERR_EXIST;
    case Error::NOTDIR:
        return NFS4ERR_NOTDIR;
    case Error::ISDIR:
        return NFS4ERR_ISDIR;
    case Error::NOTEMPTY:
        return NFS4ERR_NOTEMPTY;
    case Error::PERM:
        return NFS4ERR_PERM;
    case Error::INVAL:
        return NFS4ERR_INVAL;
    }
    return NFS4ERR_INVAL;
}

bool attrSupported(uint32_t bit) {
    switch (bit) {
    case FA_SUPPORTED_ATTRS:
    case FA_TYPE:
    case FA_FH_EXPIRE_TYPE:
    case FA_CHANGE:
    case FA_SIZE:
    case FA_LINK_SUPPORT:
    case FA_SYMLINK_SUPPORT:
    case FA_NAMED_ATTR:
    case FA_FSID:
    case FA_UNIQUE_HANDLES:
    case FA_LEASE_TIME:
    case FA_RDATTR_ERROR:
    case FA_FILEHANDLE:
    case FA_FILEID:
    case FA_MAXFILESIZE:
    case FA_MAXLINK:
    case FA_MAXNAME:
    case FA_MAXREAD:
    case FA_MAXWRITE:
    case FA_MODE:
    case FA_NO_TRUNC:
    case FA_NUMLINKS:
    case FA_OWNER:
    case FA_OWNER_GROUP:
    case FA_SPACE_USED:
    case FA_TIME_ACCESS:
    case FA_TIME_METADATA:
    case FA_TIME_MODIFY:
    case FA_MOUNTED_ON_FILEID:
        return true;
    default:
        return false;
    }
}

//  The bitmap4 of every attribute we support (words 0..1).
void supportedBitmap(Writer& w) {
    uint32_t w0 = 0, w1 = 0;
    for (uint32_t b = 0; b < 64; ++b) {
        if (attrSupported(b)) {
            (b < 32 ? w0 : w1) |= (1u << (b % 32));
        }
    }
    w.u32(2);
    w.u32(w0);
    w.u32(w1);
}

//  Encode one attribute's value into `w` (increasing bit order, per RFC 7530).
void encodeAttrValue(Writer& w, uint32_t bit, INode& n, uint64_t id, uint64_t snap) {
    switch (bit) {
    case FA_SUPPORTED_ATTRS:
        supportedBitmap(w);
        break;
    case FA_TYPE:
        w.u32(ftype4(n.type()));
        break;
    case FA_FH_EXPIRE_TYPE:
        w.u32(0); // FH4_PERSISTENT
        break;
    case FA_CHANGE:
        w.u64(n.ctime()); // logical clock ⇒ monotonic change id
        break;
    case FA_SIZE:
        w.u64(n.size());
        break;
    case FA_LINK_SUPPORT:
    case FA_SYMLINK_SUPPORT:
    case FA_UNIQUE_HANDLES:
    case FA_NO_TRUNC:
        w.u32(1); // bool true
        break;
    case FA_NAMED_ATTR:
        w.u32(0);
        break;
    case FA_FSID:
        w.u64(snap == LATEST ? 0 : snap); // fsid4.major
        w.u64(0);                         // fsid4.minor
        break;
    case FA_LEASE_TIME:
        w.u32(LEASE_TIME);
        break;
    case FA_RDATTR_ERROR:
        w.u32(NFS4_OK);
        break;
    case FA_FILEHANDLE:
        w.fh(id, snap);
        break;
    case FA_FILEID:
    case FA_MOUNTED_ON_FILEID:
        w.u64(viewFileid(id, snap));
        break;
    case FA_MAXFILESIZE:
        w.u64(uint64_t(1) << 62);
        break;
    case FA_MAXLINK:
        w.u32(0xFFFF);
        break;
    case FA_MAXNAME:
        w.u32(255);
        break;
    case FA_MAXREAD:
    case FA_MAXWRITE:
        w.u64(MAX_IO);
        break;
    case FA_MODE:
        w.u32(n.mode() & 0xFFF);
        break;
    case FA_NUMLINKS:
        w.u32(n.nlink());
        break;
    case FA_OWNER:
        w.str(std::to_string(n.uid())); // numeric-string owner (client maps it)
        break;
    case FA_OWNER_GROUP:
        w.str(std::to_string(n.gid()));
        break;
    case FA_SPACE_USED:
        w.u64(n.size());
        break;
    case FA_TIME_ACCESS:
        w.time(n.atime(), n.atimeNsec());
        break;
    case FA_TIME_METADATA:
        w.time(n.ctime(), 0);
        break;
    case FA_TIME_MODIFY:
        w.time(n.mtime(), n.mtimeNsec());
        break;
    default:
        break;
    }
}

//  Encode fattr4 = { bitmap4 attrmask; opaque attr_vals<> } for the attrs in
//  `req` that we support, in increasing bit order.
void encodeFattr4(Writer& w, std::vector<uint32_t> const& req, INode& n, uint64_t id,
                  uint64_t snap) {
    uint32_t retW0 = 0, retW1 = 0;
    std::vector<uint8_t> vals;
    Writer vw{vals};
    for (uint32_t b = 0; b < 64; ++b) {
        uint32_t word = b / 32;
        if (word >= req.size()) {
            break;
        }
        if (!(req[word] & (1u << (b % 32)))) {
            continue;
        }
        if (!attrSupported(b)) {
            continue;
        }
        (b < 32 ? retW0 : retW1) |= (1u << (b % 32));
        encodeAttrValue(vw, b, n, id, snap);
    }
    w.u32(2); // returned bitmap: two words
    w.u32(retW0);
    w.u32(retW1);
    w.bytes(vals.data(), vals.size());
}

//  Per-COMPOUND current/saved filehandle. A COMPOUND runs to completion inside
//  one session coroutine synchronously (no co_await between ops), so thread-local
//  storage gives each in-flight COMPOUND its own state without cross-connection
//  races. Reset at the start of every COMPOUND.
struct Cfh {
    std::pair<uint64_t, uint64_t> cfh{}, sfh{};
    bool hasCfh = false, hasSfh = false;
};

thread_local Cfh t_fh;

class NfsV4 : public IService {
public:
    explicit NfsV4(IHost& host)
        : m_host(host) {}

    std::vector<Option> options() const override {
        return {{"port", "TCP port to serve NFSv4 on", "2049", false}};
    }

    Error start() override {
        std::string ps = m_host.option("port");
        int port = ps.empty() ? 2049 : std::atoi(ps.c_str());
        try {
            m_ctx.restart();
            m_acc.emplace(m_ctx);
            m_acc->open(tcp::v4());
            m_acc->set_option(asio::socket_base::reuse_address(true));
            m_acc->bind(tcp::endpoint(tcp::v4(), uint16_t(port)));
            m_acc->listen();
        } catch (std::exception& e) {
            m_host.log().error("nfsv4: bind/listen on port {} failed: {}", port, e.what());
            m_acc.reset();
            return Error::INVAL;
        }
        asio::co_spawn(m_ctx, listener(), asio::detached);
        m_running = true;
        unsigned n = std::max(2u, std::thread::hardware_concurrency());
        for (unsigned i = 0; i < n; ++i) {
            m_threads.emplace_back([this] { m_ctx.run(); });
        }
        m_host.log().info("nfsv4: serving on port {} ({} io threads)", port, n);
        return Error::OK;
    }

    void stop() override {
        if (!m_running.exchange(false)) {
            return;
        }
        m_ctx.stop();
        for (auto& t : m_threads) {
            if (t.joinable()) {
                t.join();
            }
        }
        m_threads.clear();
        m_acc.reset();
    }

private:
    asio::awaitable<void> listener() {
        try {
            for (;;) {
                tcp::socket sock = co_await m_acc->async_accept(asio::use_awaitable);
                asio::co_spawn(m_ctx, session(std::move(sock)), asio::detached);
            }
        } catch (std::exception const&) {}
    }

    asio::awaitable<void> session(tcp::socket sock) {
        try {
            for (;;) {
                uint8_t mark[4];
                co_await asio::async_read(sock, asio::buffer(mark, 4), asio::use_awaitable);
                uint32_t len = (uint32_t(mark[0]) << 24 | uint32_t(mark[1]) << 16 |
                                uint32_t(mark[2]) << 8 | mark[3]) &
                               0x7fffffff;
                if (len < 24 || len > (1u << 21)) {
                    co_return;
                }
                std::vector<uint8_t> msg(len);
                co_await asio::async_read(sock, asio::buffer(msg), asio::use_awaitable);
                std::vector<uint8_t> frame = reply(msg);
                co_await asio::async_write(sock, asio::buffer(frame), asio::use_awaitable);
            }
        } catch (std::exception const&) {}
    }

    //  Parse one RPC call and build a complete record-marked accepted reply.
    std::vector<uint8_t> reply(std::vector<uint8_t> const& msg) {
        Reader r{msg.data(), msg.size()};
        uint32_t xid = r.u32();
        r.u32(); // CALL
        r.u32(); // rpcvers
        uint32_t prog = r.u32();
        uint32_t vers = r.u32();
        uint32_t proc = r.u32();
        r.skipAuth(); // cred
        r.skipAuth(); // verf
        m_host.log().debug("nfsv4: rpc prog={} vers={} proc={}", prog, vers, proc);

        std::vector<uint8_t> body;
        Writer w{body};
        uint32_t astat = SUCCESS;
        if (prog != PROG_NFS || vers != NFS_V4) {
            astat = 2; // PROG/PROC mismatch bucket
        } else if (proc == NFSPROC4_NULL) {
            // empty body
        } else if (proc == NFSPROC4_COMPOUND) {
            compound(r, w);
        } else {
            astat = 3; // PROC_UNAVAIL
        }

        std::vector<uint8_t> out;
        Writer ow{out};
        ow.u32(0); // record mark placeholder
        ow.u32(xid);
        ow.u32(1); // REPLY
        ow.u32(0); // MSG_ACCEPTED
        ow.u32(0); // verf flavor AUTH_NONE
        ow.u32(0); // verf len
        ow.u32(astat);
        if (astat == SUCCESS) {
            out.insert(out.end(), body.begin(), body.end());
        }
        uint32_t rlen = uint32_t(out.size() - 4);
        out[0] = uint8_t(0x80 | (rlen >> 24));
        out[1] = uint8_t(rlen >> 16);
        out[2] = uint8_t(rlen >> 8);
        out[3] = uint8_t(rlen);
        return out;
    }

    //  A COMPOUND is a tag, minorversion, and an array of ops; each op's result
    //  begins with its opnum and status. Processing stops at the first non-OK op
    //  (RFC 7530 §14.2). We keep a current (cfh) and saved (sfh) filehandle.
    void compound(Reader& r, Writer& w) {
        t_fh = Cfh{};
        std::string tag = r.str();
        r.u32(); // minorversion (0)
        uint32_t nops = r.u32();
        if (!r.ok || nops > 4096) {
            w.u32(NFS4ERR_RESOURCE);
            w.str(tag);
            w.u32(0);
            return;
        }

        //  COMPOUND4res: status, tag, resarray<>. We emit ops as we go, then
        //  back-patch the overall status and the actual op count.
        std::vector<uint8_t> res;
        Writer rw{res};
        uint32_t last = NFS4_OK;
        uint32_t done = 0;
        std::string trace;
        for (uint32_t i = 0; i < nops && r.ok; ++i) {
            uint32_t op = r.u32();
            trace += std::to_string(op) + " ";
            uint32_t st = doOp(op, r, rw);
            ++done;
            last = st;
            if (st != NFS4_OK) {
                trace += "=> " + std::to_string(st);
                break;
            }
        }
        m_host.log().debug("nfsv4: COMPOUND [ {}] -> {}", trace, last);
        if (!r.ok) {
            last = NFS4ERR_INVAL;
        }
        w.u32(last);
        w.str(tag);
        w.u32(done);
        w.v.insert(w.v.end(), res.begin(), res.end());
    }

    //  Dispatch one operation; writes "opnum, status[, resok]" into `rw` and
    //  returns the status (so COMPOUND can stop the chain on error).
    uint32_t doOp(uint32_t op, Reader& r, Writer& rw) {
        IPrfs& fs = m_host.fs();
        std::shared_lock<std::shared_mutex> lk(m_host.storeMutex());
        rw.u32(op);
        switch (op) {
        case OP_PUTROOTFH: {
            Node root = fs.rwRoot();
            t_fh.cfh = {root->id(), LATEST};
            t_fh.hasCfh = true;
            rw.u32(NFS4_OK);
            return NFS4_OK;
        }
        case OP_PUTPUBFH: {
            Node root = fs.rwRoot();
            t_fh.cfh = {root->id(), LATEST};
            t_fh.hasCfh = true;
            rw.u32(NFS4_OK);
            return NFS4_OK;
        }
        case OP_PUTFH: {
            uint64_t id, snap;
            if (!r.fh(id, snap)) {
                rw.u32(NFS4ERR_BADHANDLE);
                return NFS4ERR_BADHANDLE;
            }
            t_fh.cfh = {id, snap};
            t_fh.hasCfh = true;
            rw.u32(NFS4_OK);
            return NFS4_OK;
        }
        case OP_SAVEFH: {
            if (!t_fh.hasCfh) {
                rw.u32(NFS4ERR_NOFILEHANDLE);
                return NFS4ERR_NOFILEHANDLE;
            }
            t_fh.sfh = t_fh.cfh;
            t_fh.hasSfh = true;
            rw.u32(NFS4_OK);
            return NFS4_OK;
        }
        case OP_RESTOREFH: {
            if (!t_fh.hasSfh) {
                rw.u32(NFS4ERR_NOFILEHANDLE);
                return NFS4ERR_NOFILEHANDLE;
            }
            t_fh.cfh = t_fh.sfh;
            t_fh.hasCfh = true;
            rw.u32(NFS4_OK);
            return NFS4_OK;
        }
        case OP_GETFH: {
            if (!t_fh.hasCfh) {
                rw.u32(NFS4ERR_NOFILEHANDLE);
                return NFS4ERR_NOFILEHANDLE;
            }
            rw.u32(NFS4_OK);
            rw.fh(t_fh.cfh.first, t_fh.cfh.second); // GETFH4resok.object
            return NFS4_OK;
        }
        case OP_LOOKUP:
            return opLookup(r, rw, false);
        case OP_LOOKUPP:
            return opLookup(r, rw, true);
        case OP_ACCESS:
            return opAccess(r, rw);
        case OP_GETATTR:
            return opGetattr(r, rw);
        case OP_READLINK:
            return opReadlink(r, rw);
        case OP_READ:
            return opRead(r, rw);
        case OP_READDIR:
            return opReaddir(r, rw);
        case OP_OPEN:
            return opOpen(r, rw);
        case OP_CLOSE: {
            r.u32();    // seqid
            r.skip(16); // stateid { u32 seqid; opaque other[12] }
            rw.u32(NFS4_OK);
            emitStateid(rw); // an all-zero-ish close stateid
            return NFS4_OK;
        }
        case OP_RENEW: {
            r.u64(); // clientid
            rw.u32(NFS4_OK);
            return NFS4_OK;
        }
        case OP_SETCLIENTID:
            return opSetClientid(r, rw);
        case OP_SETCLIENTID_CONFIRM: {
            r.u64();   // clientid
            r.skip(8); // setclientid_confirm verifier
            rw.u32(NFS4_OK);
            return NFS4_OK;
        }
        default:
            rw.u32(NFS4ERR_NOTSUPP);
            return NFS4ERR_NOTSUPP;
        }
    }

    Node cur(IPrfs& fs) {
        return t_fh.hasCfh ? fs.nodeById(t_fh.cfh.first, t_fh.cfh.second) : nullptr;
    }

    void emitStateid(Writer& rw) {
        rw.u32(1); // stateid.seqid
        for (int i = 0; i < 12; ++i) {
            rw.v.push_back(0);
        } // stateid.other[12]
    }

    uint32_t opLookup(Reader& r, Writer& rw, bool parent) {
        IPrfs& fs = m_host.fs();
        Node dir = cur(fs);
        std::string name;
        if (!parent) {
            name = r.str();
        }
        if (!dir) {
            rw.u32(NFS4ERR_NOFILEHANDLE);
            return NFS4ERR_NOFILEHANDLE;
        }
        if (dir->type() != Type::DIR) {
            rw.u32(NFS4ERR_NOTDIR);
            return NFS4ERR_NOTDIR;
        }
        Node child;
        if (parent) {
            std::vector<Node> ps = fs.parents(dir);
            child = ps.empty() ? dir : ps.front();
        } else {
            child = fs.lookup(dir, name);
        }
        if (!child) {
            rw.u32(NFS4ERR_NOENT);
            return NFS4ERR_NOENT;
        }
        t_fh.cfh = {child->id(), child->snap()};
        t_fh.hasCfh = true;
        rw.u32(NFS4_OK);
        return NFS4_OK;
    }

    uint32_t opAccess(Reader& r, Writer& rw) {
        IPrfs& fs = m_host.fs();
        uint32_t want = r.u32();
        Node n = cur(fs);
        if (!n) {
            rw.u32(NFS4ERR_NOFILEHANDLE);
            return NFS4ERR_NOFILEHANDLE;
        }
        rw.u32(NFS4_OK);
        rw.u32(want); // supported = requested
        rw.u32(want); // access = requested (permissive, like nfsv3)
        return NFS4_OK;
    }

    uint32_t opGetattr(Reader& r, Writer& rw) {
        IPrfs& fs = m_host.fs();
        std::vector<uint32_t> req = r.bitmap();
        Node n = cur(fs);
        if (!n) {
            rw.u32(NFS4ERR_NOFILEHANDLE);
            return NFS4ERR_NOFILEHANDLE;
        }
        rw.u32(NFS4_OK);
        encodeFattr4(rw, req, *n, t_fh.cfh.first, t_fh.cfh.second);
        return NFS4_OK;
    }

    uint32_t opReadlink(Reader&, Writer& rw) {
        IPrfs& fs = m_host.fs();
        Node n = cur(fs);
        if (!n) {
            rw.u32(NFS4ERR_NOFILEHANDLE);
            return NFS4ERR_NOFILEHANDLE;
        }
        if (n->type() != Type::LNK) {
            rw.u32(NFS4ERR_INVAL);
            return NFS4ERR_INVAL;
        }
        rw.u32(NFS4_OK);
        rw.str(n->target());
        return NFS4_OK;
    }

    uint32_t opRead(Reader& r, Writer& rw) {
        IPrfs& fs = m_host.fs();
        r.skip(16); // stateid (any accepted — read-only, no share enforcement)
        uint64_t off = r.u64();
        uint32_t cnt = r.u32();
        Node n = cur(fs);
        if (!n) {
            rw.u32(NFS4ERR_NOFILEHANDLE);
            return NFS4ERR_NOFILEHANDLE;
        }
        if (n->type() == Type::DIR) {
            rw.u32(NFS4ERR_ISDIR);
            return NFS4ERR_ISDIR;
        }
        if (cnt > MAX_IO) {
            cnt = MAX_IO;
        }
        std::vector<char> buf(cnt);
        size_t got = m_host.read(n, off, buf.data(), cnt);
        bool eof = off + got >= n->size();
        rw.u32(NFS4_OK);
        rw.u32(eof ? 1 : 0);
        rw.bytes(reinterpret_cast<uint8_t*>(buf.data()), got);
        return NFS4_OK;
    }

    //  READDIR: cookie/verifier + dircount/maxcount + attr bitmap. Minimal, stable
    //  scheme: cookie = a 1-based ordinal over the store's readdir (re-listed per
    //  call). Good enough for a first cut; the nfsv3 name-cursor is the upgrade.
    uint32_t opReaddir(Reader& r, Writer& rw) {
        IPrfs& fs = m_host.fs();
        uint64_t cookie = r.u64();
        r.skip(8);             // cookieverf
        r.u32();               // dircount
        uint32_t mx = r.u32(); // maxcount
        std::vector<uint32_t> req = r.bitmap();
        Node dir = cur(fs);
        if (!dir) {
            rw.u32(NFS4ERR_NOFILEHANDLE);
            return NFS4ERR_NOFILEHANDLE;
        }
        if (dir->type() != Type::DIR) {
            rw.u32(NFS4ERR_NOTDIR);
            return NFS4ERR_NOTDIR;
        }
        rw.u32(NFS4_OK);
        rw.u64(0); // cookieverf

        auto ents = fs.readdir(dir);
        size_t budget = 64;
        bool eof = true;
        uint64_t idx = 0;
        for (auto& [name, node] : ents) {
            ++idx;
            if (idx <= cookie) {
                continue;
            }
            std::vector<uint8_t> one;
            Writer ew{one};
            ew.u64(idx); // cookie
            ew.str(name);
            encodeFattr4(ew, req, *node, node->id(), node->snap());
            if (budget + one.size() + 16 > mx) {
                eof = false;
                break;
            }
            rw.u32(1); // value-follows
            rw.v.insert(rw.v.end(), one.begin(), one.end());
            budget += one.size() + 4;
        }
        rw.u32(0);           // no more entries
        rw.u32(eof ? 1 : 0); // eof
        return NFS4_OK;
    }

    //  OPEN (CLAIM_NULL, no-create only) — enough for a client to open a file for
    //  reading. Returns a fixed stateid; share/lock state is not enforced.
    uint32_t opOpen(Reader& r, Writer& rw) {
        IPrfs& fs = m_host.fs();
        r.u32();                     // seqid
        r.u32();                     // share_access
        r.u32();                     // share_deny
        r.u64();                     // open_owner.clientid
        r.skipOpaque();              // open_owner.owner<>
        uint32_t opentype = r.u32(); // OPEN4_NOCREATE(0) / OPEN4_CREATE(1)
        if (opentype != 0) {
            rw.u32(NFS4ERR_NOTSUPP); // create surface is a follow-up
            return NFS4ERR_NOTSUPP;
        }
        uint32_t claim = r.u32(); // CLAIM_NULL(0)
        if (claim != 0) {
            rw.u32(NFS4ERR_NOTSUPP);
            return NFS4ERR_NOTSUPP;
        }
        std::string name = r.str();
        Node dir = cur(fs);
        if (!dir || dir->type() != Type::DIR) {
            rw.u32(NFS4ERR_NOTDIR);
            return NFS4ERR_NOTDIR;
        }
        Node child = fs.lookup(dir, name);
        if (!child) {
            rw.u32(NFS4ERR_NOENT);
            return NFS4ERR_NOENT;
        }
        t_fh.cfh = {child->id(), child->snap()};
        t_fh.hasCfh = true;
        rw.u32(NFS4_OK);
        emitStateid(rw); // OPEN4resok.stateid
        rw.u32(0);       // change_info4.atomic = false
        rw.u64(0);       // .before
        rw.u64(0);       // .after
        rw.u32(0);       // rflags
        rw.u32(2);       // attrset bitmap: two zero words
        rw.u32(0);
        rw.u32(0);
        rw.u32(0); // delegation type = OPEN_DELEGATE_NONE
        return NFS4_OK;
    }

    uint32_t opSetClientid(Reader& r, Writer& rw) {
        r.skip(8);      // client.verifier
        r.skipOpaque(); // client.id<>
        r.u32();        // cb_program
        r.skipOpaque(); // cb_location.r_netid
        r.skipOpaque(); // cb_location.r_addr
        r.u32();        // callback_ident
        rw.u32(NFS4_OK);
        rw.u64(++m_clientid); // SETCLIENTID4resok.clientid
        for (int i = 0; i < 8; ++i) {
            rw.v.push_back(0); // setclientid_confirm verifier
        }
        return NFS4_OK;
    }

    IHost& m_host;
    asio::io_context m_ctx;
    std::optional<tcp::acceptor> m_acc;
    std::vector<std::thread> m_threads;
    std::atomic<bool> m_running{false};
    std::atomic<uint64_t> m_clientid{1};
};

struct NfsV4Plugin : IPlugin {
    IHost& host;
    NfsV4 svc;

    explicit NfsV4Plugin(IHost& h)
        : host(h)
        , svc(h) {
        host.registry().provide<IService>(&svc, "nfsv4");
    }

    ~NfsV4Plugin() override { host.registry().withdraw<IService>("nfsv4"); }

    char const* name() const override { return "nfsv4"; }

    char const* version() const override { return "0.1"; }
};

} // namespace

extern "C" uint32_t prfs_abi(void) { return prfs::plugin::ABI; }

extern "C" prfs::plugin::IPlugin* prfs_plugin_create(prfs::plugin::IHost& host) {
    return new NfsV4Plugin(host);
}

extern "C" void prfs_plugin_destroy(prfs::plugin::IPlugin* p) { delete p; }
