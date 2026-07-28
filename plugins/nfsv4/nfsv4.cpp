// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  nfsv4 — an NFSv4.0 front-end as a prfs service plugin (todo L2). NFSv4 folds
//  MOUNT away (PUTROOTFH replaces it) and batches operations into a single
//  COMPOUND RPC over the NFS program (100003) version 4. Read surface: the
//  COMPOUND framework with a current- and saved-filehandle, plus PUTROOTFH/
//  PUTPUBFH/PUTFH/GETFH/SAVEFH/RESTOREFH, LOOKUP/LOOKUPP, ACCESS, GETATTR (fattr4
//  attribute bitmaps), READLINK, READ, READDIR, and the client-state ops
//  (SETCLIENTID/SETCLIENTID_CONFIRM/RENEW/CLOSE). Write surface: OPEN with
//  OPEN4_CREATE, WRITE (fold-into-seed, like nfsv3), SETATTR (decode a client
//  fattr4), CREATE (dir/symlink/device/fifo/socket), REMOVE, RENAME, LINK,
//  COMMIT. State (RFC 7530 §9): OPEN establishes share reservations with real
//  stateids and enforces conflicts (SHARE_DENIED); LOCK/LOCKT/LOCKU enforce
//  byte-range conflicts (DENIED); READ/WRITE/CLOSE validate the stateid (an
//  unknown one → BAD_STATEID, a WRITE without WRITE access → OPENMODE). Mutating
//  store ops take the store lock exclusively and stamp the parent dir's
//  mtime/ctime; open/lock state is guarded by a separate mutex. Simplified for a
//  single-client test target: no lease timers (RENEW always OK), no seqid replay
//  cache, no reboot/grace recovery. Unimplemented ops → NFS4ERR_NOTSUPP.
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
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <set>
#include <unordered_map>
#include <utility>
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
    OP_COMMIT = 5,
    OP_CREATE = 6,
    OP_GETATTR = 9,
    OP_GETFH = 10,
    OP_LINK = 11,
    OP_LOCK = 12,
    OP_LOCKT = 13,
    OP_LOCKU = 14,
    OP_LOOKUP = 15,
    OP_LOOKUPP = 16,
    OP_OPEN = 18,
    OP_OPEN_CONFIRM = 20,
    OP_OPEN_DOWNGRADE = 21,
    OP_PUTFH = 22,
    OP_PUTPUBFH = 23,
    OP_PUTROOTFH = 24,
    OP_READ = 25,
    OP_READDIR = 26,
    OP_READLINK = 27,
    OP_REMOVE = 28,
    OP_RENAME = 29,
    OP_RENEW = 30,
    OP_RESTOREFH = 31,
    OP_SAVEFH = 32,
    OP_SETATTR = 34,
    OP_SETCLIENTID = 35,
    OP_SETCLIENTID_CONFIRM = 36,
    OP_WRITE = 38,
    OP_RELEASE_LOCKOWNER = 39,
};

//  openflag4 / createhow4 / claim / share and stable_how4.
constexpr uint32_t OPEN4_NOCREATE = 0, OPEN4_CREATE = 1;
constexpr uint32_t UNCHECKED4 = 0, GUARDED4 = 1, EXCLUSIVE4 = 2;
constexpr uint32_t CLAIM_NULL = 0;
constexpr uint32_t SET_TO_SERVER_TIME4 = 0, SET_TO_CLIENT_TIME4 = 1;

//  share_access4 / share_deny4 bits (OPEN); nfs_lock_type4 (LOCK).
constexpr uint32_t ACCESS_READ = 1, ACCESS_WRITE = 2, ACCESS_BOTH = 3;
constexpr uint32_t DENY_NONE = 0, DENY_READ = 1, DENY_WRITE = 2, DENY_BOTH = 3;
constexpr uint32_t READ_LT = 1, WRITE_LT = 2, READW_LT = 3, WRITEW_LT = 4;
constexpr uint32_t OPEN4_RESULT_CONFIRM = 2, OPEN4_RESULT_LOCKTYPE_POSIX = 4;

//  nfsstat4 (subset).
constexpr uint32_t NFS4_OK = 0, NFS4ERR_PERM = 1, NFS4ERR_NOENT = 2, NFS4ERR_IO = 5,
                   NFS4ERR_EXIST = 17, NFS4ERR_NOTDIR = 20, NFS4ERR_ISDIR = 21, NFS4ERR_INVAL = 22,
                   NFS4ERR_ROFS = 30, NFS4ERR_NOTEMPTY = 66, NFS4ERR_STALE = 70,
                   NFS4ERR_BADHANDLE = 10001, NFS4ERR_BAD_COOKIE = 10003, NFS4ERR_NOTSUPP = 10004,
                   NFS4ERR_EXPIRED = 10011, NFS4ERR_CLID_INUSE = 10017,
                   NFS4ERR_STALE_CLIENTID = 10022, NFS4ERR_DENIED = 10010,
                   NFS4ERR_SHARE_DENIED = 10015, NFS4ERR_RESOURCE = 10018,
                   NFS4ERR_NOFILEHANDLE = 10020, NFS4ERR_BAD_STATEID = 10025,
                   NFS4ERR_LOCK_RANGE = 10028, NFS4ERR_ATTRNOTSUPP = 10032,
                   NFS4ERR_OPENMODE = 10038, NFS4ERR_OP_ILLEGAL = 10044;

//  READDIR cookie ↔ name bridge (as in nfsv3): NFSv4 cookies are opaque u64 the
//  client echoes back; the store resumes readdirPage by NAME. An entry's cookie
//  is a 64-bit hash of its name with the top bit set — always > 2 (RFC 7530
//  reserves cookies 0/1/2) and resolvable back to the name-cursor (stable under
//  concurrent add/remove, unlike a positional ordinal). Cookie 0 = start.
constexpr uint64_t CK_ENTRY = uint64_t(1) << 63;

uint64_t nameCookie(std::string const& name) {
    uint64_t h = 1469598103934665603ull; // FNV-1a/64
    for (unsigned char c : name) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h | CK_ENTRY;
}

//  attribute-fold for WRITE: a write stores no bytes — it evolves the file's
//  content seed (FNV-1a over seed, offset, data), so READ regenerates content
//  reflecting the write while the store grows by nothing. Never returns 0.
uint64_t mixSeed(uint64_t seed, uint64_t off, std::string const& data) {
    uint64_t h = seed ^ (off + 0x9E3779B97F4A7C15ull);
    for (unsigned char c : data) {
        h = (h ^ c) * 0x100000001B3ull;
    }
    return h ? h : 1;
}

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

//  --- NFSv4 open/lock state (RFC 7530 §9). Simplified for a single-client test
//  target: no lease timers (leases never expire; RENEW always OK), no seqid
//  replay cache, no reboot/grace recovery. What IS enforced: share reservations
//  (OPEN access/deny conflicts → SHARE_DENIED), byte-range lock conflicts (→
//  DENIED), and stateid validity on READ/WRITE/CLOSE/LOCK. ---

//  A stateid is { uint32 seqid; opaque other[12] }. We pack other as an 8-byte
//  id (the table key) + a 4-byte tag (0 = open, 1 = lock). All-zero / all-ones
//  are the anonymous / read-bypass special stateids (valid, no state lookup).
struct Stateid {
    uint32_t seqid = 0;
    uint64_t id = 0;
    uint32_t tag = 0;
    bool special = false; // all-zeros or all-ones
};

struct OpenState {
    uint64_t clientid = 0;
    std::string owner;
    uint64_t fileId = 0;
    uint32_t access = 0, deny = 0;
    uint32_t seqid = 1;
};

//  A client's lease record (SETCLIENTID/SETCLIENTID_CONFIRM). NFSv4.0 opens and
//  locks are anchored to a *confirmed* clientid; a stale one (e.g. cached across
//  a server restart) must be rejected so the client re-runs the handshake — that
//  rejection is what makes real-client byte-range locking work. The callback
//  fields are recorded but unused: we grant no delegations, so no CB_ channel is
//  needed. Leases never expire here (deterministic target, no wall clock).
struct ClientRec {
    uint64_t clientid = 0;    // server-assigned handle
    std::string id;           // nfs_client_id4.id (stable per client instance)
    uint64_t verifier = 0;    // client boot verifier (changes ⇒ client rebooted)
    uint64_t confirmVerf = 0; // server-minted; echoed back by SETCLIENTID_CONFIRM
    bool confirmed = false;
    uint32_t cbProgram = 0; // callback program/addr — recorded, not dialed
    std::string cbNetid, cbAddr;
    uint32_t cbIdent = 0;
};

struct LockRange {
    uint64_t off = 0, len = 0; // len == ~0 ⇒ to EOF
    uint32_t type = 0;         // READ_LT / WRITE_LT
};

struct LockState {
    uint64_t clientid = 0;
    std::string owner;
    uint64_t fileId = 0;
    uint32_t seqid = 1;
    std::vector<LockRange> ranges;
};

//  [off,len) overlap, with len==~0 meaning "to EOF".
bool overlaps(uint64_t aoff, uint64_t alen, uint64_t boff, uint64_t blen) {
    uint64_t aend = alen == ~0ull ? ~0ull : aoff + alen;
    uint64_t bend = blen == ~0ull ? ~0ull : boff + blen;
    return aoff < bend && boff < aend;
}

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
    static bool isMutating(uint32_t op) {
        switch (op) {
        case OP_OPEN: // may create
        case OP_WRITE:
        case OP_SETATTR:
        case OP_CREATE:
        case OP_REMOVE:
        case OP_RENAME:
        case OP_LINK:
            return true;
        default:
            return false;
        }
    }

    uint32_t doOp(uint32_t op, Reader& r, Writer& rw) {
        IPrfs& fs = m_host.fs();
        //  Reads run under a shared lock (content generation parallelizes);
        //  mutations take it exclusively.
        std::shared_lock<std::shared_mutex> rl;
        std::unique_lock<std::shared_mutex> wl;
        if (isMutating(op)) {
            wl = std::unique_lock<std::shared_mutex>(m_host.storeMutex());
        } else {
            rl = std::shared_lock<std::shared_mutex>(m_host.storeMutex());
        }
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
        case OP_OPEN_CONFIRM:
            return opOpenConfirm(r, rw);
        case OP_OPEN_DOWNGRADE:
            return opOpenDowngrade(r, rw);
        case OP_CLOSE:
            return opClose(r, rw);
        case OP_LOCK:
            return opLock(r, rw);
        case OP_LOCKT:
            return opLockt(r, rw);
        case OP_LOCKU:
            return opLocku(r, rw);
        case OP_RENEW:
            return opRenew(r, rw);
        case OP_SETCLIENTID:
            return opSetClientid(r, rw);
        case OP_SETCLIENTID_CONFIRM:
            return opSetClientidConfirm(r, rw);
        case OP_WRITE:
            return opWrite(r, rw);
        case OP_SETATTR:
            return opSetattr(r, rw);
        case OP_CREATE:
            return opCreate(r, rw);
        case OP_REMOVE:
            return opRemove(r, rw);
        case OP_RENAME:
            return opRename(r, rw);
        case OP_LINK:
            return opLink(r, rw);
        case OP_COMMIT: {
            r.u64(); // offset
            r.u32(); // count
            rw.u32(NFS4_OK);
            for (int i = 0; i < 8; ++i) {
                rw.v.push_back(0); // writeverf4 (nothing is buffered)
            }
            return NFS4_OK;
        }
        case OP_RELEASE_LOCKOWNER:
            return opReleaseLockowner(r, rw);
        default:
            rw.u32(NFS4ERR_NOTSUPP);
            return NFS4ERR_NOTSUPP;
        }
    }

    //  change_info4 { bool atomic; changeid4 before; changeid4 after; } — the
    //  dir's change id (its ctime) straddling the mutation.
    void changeInfo(Writer& rw, uint64_t before, uint64_t after) {
        rw.u32(1);
        rw.u64(before);
        rw.u64(after);
    }

    //  A namespace mutation stamps the parent dir's mtime/ctime = now() (POSIX;
    //  the store leaves this to the caller — see nfsv3). Returns the change id
    //  (ctime) before the stamp, for change_info4.
    uint64_t touchDir(Node const& dir) {
        uint64_t before = dir->ctime();
        uint64_t t = m_host.fs().now();
        dir->mtime(t);
        dir->ctime(t);
        return before;
    }

    //  Decode a client fattr4 { bitmap4; opaque vals<> } and apply the settable
    //  attributes to `n`, consuming the bytes in bit order. NFS4ERR_ATTRNOTSUPP
    //  if a set attribute isn't one we can apply (we can't skip an unknown one).
    uint32_t applyFattr4(Reader& r, Node const& n) {
        std::vector<uint32_t> bm = r.bitmap();
        r.u32(); // attrlist length (we decode attr-by-attr instead)
        for (uint32_t b = 0; b < bm.size() * 32; ++b) {
            if (!(bm[b / 32] & (1u << (b % 32)))) {
                continue;
            }
            switch (b) {
            case FA_SIZE: {
                uint64_t s = r.u64();
                if (n) {
                    n->size(s);
                }
                break;
            }
            case FA_MODE: {
                uint32_t m = r.u32();
                if (n) {
                    n->mode(m & 0xFFF);
                }
                break;
            }
            case FA_OWNER: {
                std::string o = r.str();
                if (n) {
                    n->uid(uint32_t(std::strtoul(o.c_str(), nullptr, 10)));
                }
                break;
            }
            case FA_OWNER_GROUP: {
                std::string g = r.str();
                if (n) {
                    n->gid(uint32_t(std::strtoul(g.c_str(), nullptr, 10)));
                }
                break;
            }
            case 48:   // FATTR4_TIME_ACCESS_SET (settime4)
            case 54: { // FATTR4_TIME_MODIFY_SET
                uint32_t how = r.u32();
                uint64_t sec = m_host.fs().now();
                uint32_t nsec = 0;
                if (how == SET_TO_CLIENT_TIME4) {
                    sec = r.u64();
                    nsec = r.u32();
                }
                if (n) {
                    if (b == 48) {
                        n->atime(sec);
                        n->atimeNsec(nsec);
                    } else {
                        n->mtime(sec);
                        n->mtimeNsec(nsec);
                    }
                }
                break;
            }
            default:
                return NFS4ERR_ATTRNOTSUPP; // unknown settable attr: can't skip it
            }
        }
        return r.ok ? NFS4_OK : NFS4ERR_INVAL;
    }

    Node cur(IPrfs& fs) {
        return t_fh.hasCfh ? fs.nodeById(t_fh.cfh.first, t_fh.cfh.second) : nullptr;
    }

    //  stateid4 { seqid; other[12] } — other = 8-byte id ‖ 4-byte tag.
    void writeStateid(Writer& rw, uint32_t seqid, uint64_t id, uint32_t tag) {
        rw.u32(seqid);
        rw.u64(id);
        rw.u32(tag);
    }

    Stateid readStateid(Reader& r) {
        Stateid s;
        s.seqid = r.u32();
        s.id = r.u64();
        s.tag = r.u32();
        s.special = (s.seqid == 0 && s.id == 0 && s.tag == 0) ||
                    (s.seqid == 0xffffffffu && s.id == ~0ull && s.tag == 0xffffffffu);
        return s;
    }

    //  Validate a stateid presented on READ/WRITE/CLOSE for `fileId`. Special
    //  stateids are always accepted. Returns the open's access bits (via `access`)
    //  when it names a live open, or a special all-access when special. Sets an
    //  nfsstat4 error otherwise. Caller holds m_stateMu.
    uint32_t checkStateid(Stateid const& s, uint64_t fileId, uint32_t& access) {
        if (s.special) {
            access = ACCESS_BOTH;
            return NFS4_OK;
        }
        if (s.tag == 0) {
            auto it = m_opens.find(s.id);
            if (it == m_opens.end() || it->second.fileId != fileId) {
                return NFS4ERR_BAD_STATEID;
            }
            access = it->second.access;
            return NFS4_OK;
        }
        auto it = m_locks.find(s.id); // a lock stateid also authorizes I/O
        if (it == m_locks.end() || it->second.fileId != fileId) {
            return NFS4ERR_BAD_STATEID;
        }
        access = ACCESS_BOTH;
        return NFS4_OK;
    }

    //  Encode LOCK4denied { offset; length; locktype; lock_owner{clientid,owner} }.
    void writeDenied(Writer& rw, LockRange const& c, uint64_t clientid, std::string const& owner) {
        rw.u64(c.off);
        rw.u64(c.len);
        rw.u32(c.type);
        rw.u64(clientid);
        rw.str(owner);
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
        Stateid sid = readStateid(r);
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
        {
            std::lock_guard<std::mutex> sl(m_stateMu);
            uint32_t acc = 0, st = checkStateid(sid, n->id(), acc);
            if (st != NFS4_OK) {
                rw.u32(st);
                return st;
            }
            if (!(acc & ACCESS_READ)) {
                rw.u32(NFS4ERR_OPENMODE);
                return NFS4ERR_OPENMODE;
            }
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

    //  Remember an entry cookie→name so a resume is O(1); bounded (clears on
    //  overflow, a miss then falls back to a scan). Guarded independently.
    void rememberCookie(uint64_t ck, std::string const& name) {
        std::lock_guard<std::mutex> lk(m_ckMu);
        if (m_ckName.size() >= CK_CACHE_MAX) {
            m_ckName.clear();
        }
        m_ckName[ck] = name;
    }

    //  The name to resume readdirPage *after*, for an entry cookie. Cache hit is
    //  O(1); a miss falls back to a scan for a still-present entry, else nullopt
    //  ⇒ NFS4ERR_BAD_COOKIE (the client restarts the scan).
    std::optional<std::string> resumeName(IPrfs& fs, Node const& dir, uint64_t ck) {
        {
            std::lock_guard<std::mutex> lk(m_ckMu);
            auto it = m_ckName.find(ck);
            if (it != m_ckName.end()) {
                return it->second;
            }
        }
        std::string after;
        for (;;) {
            DirPage pg = fs.readdirPage(dir, after, 256);
            for (auto const& [name, node] : pg.entries) {
                if (nameCookie(name) == ck) {
                    return name;
                }
            }
            if (pg.entries.empty() || pg.eof) {
                return std::nullopt;
            }
            after = pg.cookie;
        }
    }

    //  READDIR — pages the store's stable readdirPage name-cursor (O(page), immune
    //  to concurrent add/remove). Cookie 0 starts; an entry cookie resolves to its
    //  name (cache or scan) to resume. NFSv4 READDIR carries no "." / "..".
    uint32_t opReaddir(Reader& r, Writer& rw) {
        IPrfs& fs = m_host.fs();
        uint64_t cookie = r.u64();
        r.skip(8);             // cookieverf
        r.u32();               // dircount (advisory)
        uint32_t mx = r.u32(); // maxcount (reply size cap)
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
        std::string after;
        if (cookie != 0) {
            auto rn = resumeName(fs, dir, cookie);
            if (!rn) {
                rw.u32(NFS4ERR_BAD_COOKIE);
                return NFS4ERR_BAD_COOKIE;
            }
            after = *rn;
        }
        rw.u32(NFS4_OK);
        for (int i = 0; i < 8; ++i) {
            rw.v.push_back(0); // cookieverf (name-cursor is stable ⇒ constant)
        }

        size_t budget = 96; // status + cookieverf + trailing eof already accounted
        bool eof = false;
        bool stop = false;
        while (!stop) {
            DirPage pg = fs.readdirPage(dir, after, 512);
            for (auto& [name, node] : pg.entries) {
                std::vector<uint8_t> one;
                Writer ew{one};
                uint64_t ck = nameCookie(name);
                ew.u64(ck);
                ew.str(name);
                encodeFattr4(ew, req, *node, node->id(), node->snap());
                if (budget + one.size() + 8 > mx) {
                    stop = true;
                    break;
                }
                rememberCookie(ck, name);
                rw.u32(1); // value-follows
                rw.v.insert(rw.v.end(), one.begin(), one.end());
                budget += one.size() + 4;
                after = name;
            }
            if (stop) {
                break;
            }
            if (pg.entries.empty() || pg.eof) {
                eof = true;
                break;
            }
            after = pg.cookie;
        }
        rw.u32(0);           // no more entries
        rw.u32(eof ? 1 : 0); // eof
        return NFS4_OK;
    }

    //  OPEN (CLAIM_NULL): opens a file for reading, or creates a regular file
    //  (OPEN4_CREATE). Establishes share-reservation state and returns a real
    //  stateid; a conflicting share_access/share_deny → NFS4ERR_SHARE_DENIED.
    uint32_t opOpen(Reader& r, Writer& rw) {
        IPrfs& fs = m_host.fs();
        r.u32();                     // open_seqid
        uint32_t access = r.u32();   // share_access
        uint32_t deny = r.u32();     // share_deny
        uint64_t clientid = r.u64(); // open_owner.clientid
        std::string owner = r.str(); // open_owner.owner<>
        if (!clientConfirmed(clientid)) {
            rw.u32(NFS4ERR_STALE_CLIENTID); // force SETCLIENTID/CONFIRM before state
            return NFS4ERR_STALE_CLIENTID;
        }
        uint32_t opentype = r.u32(); // openflag4.opentype
        uint32_t createmode = 0;
        bool haveAttrs = false;
        Reader attrsAt{}; // position of createattrs, decoded after the file exists
        if (opentype == OPEN4_CREATE) {
            createmode = r.u32();
            if (createmode == EXCLUSIVE4) {
                r.skip(8); // createverf4
            } else {
                attrsAt = r; // createattrs fattr4 follows here
                haveAttrs = true;
                r.bitmap();                          // consume the fattr4 to reach the claim
                r.skip((r.u32() + 3u) & ~size_t(3)); // attrlist<>
            }
        }
        uint32_t claim = r.u32();
        if (claim != CLAIM_NULL) {
            rw.u32(NFS4ERR_NOTSUPP);
            return NFS4ERR_NOTSUPP;
        }
        std::string name = r.str();
        if (!r.ok) {
            rw.u32(NFS4ERR_INVAL);
            return NFS4ERR_INVAL;
        }
        Node dir = cur(fs);
        if (!dir || dir->type() != Type::DIR) {
            rw.u32(NFS4ERR_NOTDIR);
            return NFS4ERR_NOTDIR;
        }
        if (opentype == OPEN4_CREATE && !live(t_fh.cfh.second)) {
            rw.u32(NFS4ERR_ROFS);
            return NFS4ERR_ROFS;
        }
        uint64_t before = dir->ctime();
        Node child = fs.lookup(dir, name);
        bool created = false;
        if (!child) {
            if (opentype != OPEN4_CREATE) {
                rw.u32(NFS4ERR_NOENT);
                return NFS4ERR_NOENT;
            }
            child = fs.mkfile("");
            if (fs.link(dir, name, child) != Error::OK) {
                rw.u32(NFS4ERR_IO);
                return NFS4ERR_IO;
            }
            if (haveAttrs) {
                applyFattr4(attrsAt, child); // mode/size/owner from createattrs
            }
            touchDir(dir);
            created = true;
        } else if (opentype == OPEN4_CREATE && createmode == GUARDED4) {
            rw.u32(NFS4ERR_EXIST);
            return NFS4ERR_EXIST;
        }
        t_fh.cfh = {child->id(), child->snap()};
        t_fh.hasCfh = true;

        //  Establish (or upgrade) the open state, enforcing share reservations.
        uint64_t fileId = child->id();
        uint32_t stSeqid;
        uint64_t stId;
        bool needConfirm;
        {
            std::lock_guard<std::mutex> sl(m_stateMu);
            uint64_t upId = 0;
            for (auto& [k, o] : m_opens) {
                if (o.fileId == fileId && o.clientid == clientid && o.owner == owner) {
                    upId = k; // same open-owner ⇒ upgrade, not a conflict
                    break;
                }
            }
            if (upId) {
                OpenState& o = m_opens[upId];
                o.access |= access;
                o.deny |= deny;
                stSeqid = ++o.seqid;
                stId = upId;
            } else {
                for (auto const& [k, o] : m_opens) {
                    if (o.fileId == fileId && ((access & o.deny) || (deny & o.access))) {
                        rw.u32(NFS4ERR_SHARE_DENIED);
                        return NFS4ERR_SHARE_DENIED;
                    }
                }
                stId = m_nextState++;
                m_opens[stId] = OpenState{clientid, owner, fileId, access, deny, 1};
                stSeqid = 1;
            }
            //  A brand-new open-owner must be confirmed (OPEN_CONFIRM) before it can
            //  hold locks. Without OPEN4_RESULT_CONFIRM the Linux client never marks
            //  the owner usable ⇒ fcntl short-circuits to ENOLCK. (knfsd does this.)
            needConfirm = m_confirmedOwners.find({clientid, owner}) == m_confirmedOwners.end();
        }
        //  LOCKTYPE_POSIX advertises POSIX byte-range semantics; CONFIRM requests the
        //  open-owner handshake for a not-yet-confirmed owner.
        uint32_t rflags = OPEN4_RESULT_LOCKTYPE_POSIX | (needConfirm ? OPEN4_RESULT_CONFIRM : 0);
        rw.u32(NFS4_OK);
        writeStateid(rw, stSeqid, stId, 0);                      // OPEN4resok.stateid
        changeInfo(rw, before, created ? dir->ctime() : before); // cinfo
        rw.u32(rflags);                                          // rflags
        rw.u32(2);                                               // attrset bitmap (empty)
        rw.u32(0);
        rw.u32(0);
        rw.u32(0); // delegation type = OPEN_DELEGATE_NONE
        return NFS4_OK;
    }

    uint32_t opClose(Reader& r, Writer& rw) {
        r.u32(); // seqid
        Stateid sid = readStateid(r);
        std::lock_guard<std::mutex> sl(m_stateMu);
        if (!sid.special) {
            auto it = m_opens.find(sid.id);
            if (it == m_opens.end()) {
                rw.u32(NFS4ERR_BAD_STATEID);
                return NFS4ERR_BAD_STATEID;
            }
            m_opens.erase(it);
        }
        rw.u32(NFS4_OK);
        writeStateid(rw, sid.seqid + 1, sid.id, sid.tag); // CLOSE4resok.open_stateid
        return NFS4_OK;
    }

    uint32_t opOpenConfirm(Reader& r, Writer& rw) {
        Stateid sid = readStateid(r);
        r.u32(); // seqid
        std::lock_guard<std::mutex> sl(m_stateMu);
        uint32_t sq = sid.seqid;
        auto it = m_opens.find(sid.id);
        if (it != m_opens.end()) {
            sq = ++it->second.seqid;
            //  The open-owner is now confirmed — future opens by it skip CONFIRM,
            //  and it can hold locks.
            m_confirmedOwners.insert({it->second.clientid, it->second.owner});
        }
        rw.u32(NFS4_OK);
        writeStateid(rw, sq, sid.id, sid.tag);
        return NFS4_OK;
    }

    uint32_t opOpenDowngrade(Reader& r, Writer& rw) {
        Stateid sid = readStateid(r);
        r.u32();                   // seqid
        uint32_t access = r.u32(); // reduced share_access
        uint32_t deny = r.u32();   // reduced share_deny
        std::lock_guard<std::mutex> sl(m_stateMu);
        auto it = m_opens.find(sid.id);
        if (it == m_opens.end()) {
            rw.u32(NFS4ERR_BAD_STATEID);
            return NFS4ERR_BAD_STATEID;
        }
        it->second.access = access;
        it->second.deny = deny;
        rw.u32(NFS4_OK);
        writeStateid(rw, ++it->second.seqid, sid.id, sid.tag);
        return NFS4_OK;
    }

    //  LOCK — establish a byte-range lock; a conflicting range held by a different
    //  lock-owner → NFS4ERR_DENIED with the conflicting LOCK4denied.
    uint32_t opLock(Reader& r, Writer& rw) {
        IPrfs& fs = m_host.fs();
        uint32_t locktype = r.u32();
        r.u32(); // reclaim
        uint64_t off = r.u64();
        uint64_t len = r.u64();
        uint32_t isNew = r.u32(); // new_lock_owner
        uint64_t clientid = 0;
        std::string owner;
        Stateid existing{};
        if (isNew) {
            r.u32();        // open_seqid
            readStateid(r); // open_stateid
            r.u32();        // lock_seqid
            clientid = r.u64();
            owner = r.str();
            if (!clientConfirmed(clientid)) {
                rw.u32(NFS4ERR_STALE_CLIENTID);
                return NFS4ERR_STALE_CLIENTID;
            }
        } else {
            existing = readStateid(r); // lock_stateid
            r.u32();                   // lock_seqid
        }
        Node n = cur(fs);
        if (!n) {
            rw.u32(NFS4ERR_NOFILEHANDLE);
            return NFS4ERR_NOFILEHANDLE;
        }
        uint64_t fileId = n->id();
        bool write = (locktype == WRITE_LT || locktype == WRITEW_LT);
        std::lock_guard<std::mutex> sl(m_stateMu);
        if (!isNew) {
            auto it = m_locks.find(existing.id);
            if (it == m_locks.end()) {
                rw.u32(NFS4ERR_BAD_STATEID);
                return NFS4ERR_BAD_STATEID;
            }
            clientid = it->second.clientid;
            owner = it->second.owner;
        }
        for (auto const& [k, L] : m_locks) {
            if (L.fileId != fileId || (L.clientid == clientid && L.owner == owner)) {
                continue;
            }
            for (auto const& rg : L.ranges) {
                if (overlaps(off, len, rg.off, rg.len) && (write || rg.type == WRITE_LT)) {
                    rw.u32(NFS4ERR_DENIED);
                    writeDenied(rw, rg, L.clientid, L.owner);
                    return NFS4ERR_DENIED;
                }
            }
        }
        uint64_t lid = 0;
        for (auto& [k, L] : m_locks) {
            if (L.fileId == fileId && L.clientid == clientid && L.owner == owner) {
                lid = k;
                break;
            }
        }
        if (!lid) {
            lid = m_nextState++;
            m_locks[lid] = LockState{clientid, owner, fileId, 1, {}};
        }
        LockState& L = m_locks[lid];
        L.ranges.push_back({off, len, write ? WRITE_LT : READ_LT});
        rw.u32(NFS4_OK);
        writeStateid(rw, ++L.seqid, lid, 1); // LOCK4resok.lock_stateid
        return NFS4_OK;
    }

    //  LOCKT — test for a conflicting lock without acquiring one.
    uint32_t opLockt(Reader& r, Writer& rw) {
        IPrfs& fs = m_host.fs();
        uint32_t locktype = r.u32();
        uint64_t off = r.u64();
        uint64_t len = r.u64();
        uint64_t clientid = r.u64();
        std::string owner = r.str();
        Node n = cur(fs);
        if (!n) {
            rw.u32(NFS4ERR_NOFILEHANDLE);
            return NFS4ERR_NOFILEHANDLE;
        }
        uint64_t fileId = n->id();
        bool write = (locktype == WRITE_LT || locktype == WRITEW_LT);
        std::lock_guard<std::mutex> sl(m_stateMu);
        for (auto const& [k, L] : m_locks) {
            if (L.fileId != fileId || (L.clientid == clientid && L.owner == owner)) {
                continue;
            }
            for (auto const& rg : L.ranges) {
                if (overlaps(off, len, rg.off, rg.len) && (write || rg.type == WRITE_LT)) {
                    rw.u32(NFS4ERR_DENIED);
                    writeDenied(rw, rg, L.clientid, L.owner);
                    return NFS4ERR_DENIED;
                }
            }
        }
        rw.u32(NFS4_OK); // LOCKT4res on OK carries no body
        return NFS4_OK;
    }

    //  LOCKU — release the given byte range from the lock owner's held ranges.
    uint32_t opLocku(Reader& r, Writer& rw) {
        r.u32();                      // locktype
        r.u32();                      // seqid
        Stateid sid = readStateid(r); // lock_stateid
        uint64_t off = r.u64();
        uint64_t len = r.u64();
        std::lock_guard<std::mutex> sl(m_stateMu);
        auto it = m_locks.find(sid.id);
        if (it == m_locks.end()) {
            rw.u32(NFS4ERR_BAD_STATEID);
            return NFS4ERR_BAD_STATEID;
        }
        auto& ranges = it->second.ranges;
        ranges.erase(
            std::remove_if(ranges.begin(), ranges.end(),
                           [&](LockRange const& rg) { return overlaps(off, len, rg.off, rg.len); }),
            ranges.end());
        rw.u32(NFS4_OK);
        writeStateid(rw, ++it->second.seqid, sid.id, 1);
        return NFS4_OK;
    }

    //  RELEASE_LOCKOWNER — the client is done with a lock-owner (sent after LOCKU).
    //  Forget its lock state. The Linux client issues this to tidy up; replying OK
    //  (rather than NOTSUPP) completes the lock lifecycle cleanly.
    uint32_t opReleaseLockowner(Reader& r, Writer& rw) {
        uint64_t clientid = r.u64(); // lock_owner.clientid
        std::string owner = r.str(); // lock_owner.owner<>
        std::lock_guard<std::mutex> sl(m_stateMu);
        for (auto it = m_locks.begin(); it != m_locks.end();) {
            it = (it->second.clientid == clientid && it->second.owner == owner) ? m_locks.erase(it)
                                                                                : std::next(it);
        }
        rw.u32(NFS4_OK);
        return NFS4_OK;
    }

    //  WRITE — stores no bytes: folds the data into the file's content seed and
    //  grows the size, so READ regenerates content reflecting the write. The
    //  stateid must name an open with WRITE access (or be a special stateid).
    uint32_t opWrite(Reader& r, Writer& rw) {
        IPrfs& fs = m_host.fs();
        Stateid sid = readStateid(r);
        uint64_t off = r.u64();
        r.u32(); // stable_how4
        std::string data = r.str();
        Node n = cur(fs);
        if (!n) {
            rw.u32(NFS4ERR_NOFILEHANDLE);
            return NFS4ERR_NOFILEHANDLE;
        }
        if (!live(t_fh.cfh.second)) {
            rw.u32(NFS4ERR_ROFS);
            return NFS4ERR_ROFS;
        }
        if (n->type() == Type::DIR) {
            rw.u32(NFS4ERR_ISDIR);
            return NFS4ERR_ISDIR;
        }
        {
            std::lock_guard<std::mutex> sl(m_stateMu);
            uint32_t acc = 0, st = checkStateid(sid, n->id(), acc);
            if (st != NFS4_OK) {
                rw.u32(st);
                return st;
            }
            if (!(acc & ACCESS_WRITE)) {
                rw.u32(NFS4ERR_OPENMODE);
                return NFS4ERR_OPENMODE;
            }
        }
        fs.setContentSeed(n, mixSeed(n->contentSeed(), off, data));
        if (off + data.size() > n->size()) {
            n->size(off + data.size());
        }
        n->mtime(fs.now());
        rw.u32(NFS4_OK);
        rw.u32(uint32_t(data.size())); // count
        rw.u32(2);                     // committed = FILE_SYNC4
        for (int i = 0; i < 8; ++i) {
            rw.v.push_back(0); // writeverf4
        }
        return NFS4_OK;
    }

    uint32_t opSetattr(Reader& r, Writer& rw) {
        IPrfs& fs = m_host.fs();
        r.skip(16); // stateid
        Node n = cur(fs);
        if (!n) {
            rw.u32(NFS4ERR_NOFILEHANDLE);
            return NFS4ERR_NOFILEHANDLE;
        }
        if (!live(t_fh.cfh.second)) {
            r.bitmap();
            r.skip((r.u32() + 3u) & ~size_t(3));
            rw.u32(NFS4ERR_ROFS);
            rw.u32(0); // attrsset bitmap (empty)
            return NFS4ERR_ROFS;
        }
        uint32_t st = applyFattr4(r, n);
        if (st == NFS4_OK) {
            n->ctime(fs.now()); // any attr change bumps ctime
        }
        rw.u32(st);
        rw.u32(0); // attrsset bitmap we set (empty is accepted)
        return st;
    }

    //  CREATE — non-regular objects (dir, symlink, device, fifo, socket); a
    //  regular file is created via OPEN. createtype4 switches on the ftype.
    uint32_t opCreate(Reader& r, Writer& rw) {
        IPrfs& fs = m_host.fs();
        uint32_t type = r.u32(); // nfs_ftype4
        Node child;
        std::string linkdata;
        uint32_t maj = 0, min = 0;
        if (type == NF4LNK) {
            linkdata = r.str();
        } else if (type == NF4BLK || type == NF4CHR) {
            maj = r.u32();
            min = r.u32();
        }
        std::string name = r.str();
        Node dir = cur(fs);
        if (!dir || dir->type() != Type::DIR) {
            rw.u32(NFS4ERR_NOTDIR);
            return NFS4ERR_NOTDIR;
        }
        if (!live(t_fh.cfh.second)) {
            r.bitmap();
            r.skip((r.u32() + 3u) & ~size_t(3));
            rw.u32(NFS4ERR_ROFS);
            return NFS4ERR_ROFS;
        }
        switch (type) {
        case NF4DIR:
            child = fs.mkdir();
            break;
        case NF4LNK:
            child = fs.symlink(linkdata);
            break;
        case NF4BLK:
            child = fs.mknod(Type::BLK, maj, min);
            break;
        case NF4CHR:
            child = fs.mknod(Type::CHR, maj, min);
            break;
        case NF4SOCK:
            child = fs.mksock();
            break;
        case NF4FIFO:
            child = fs.mkfifo();
            break;
        default:
            rw.u32(NFS4ERR_INVAL);
            return NFS4ERR_INVAL;
        }
        uint64_t before = dir->ctime();
        if (fs.link(dir, name, child) != Error::OK) {
            rw.u32(NFS4ERR_EXIST);
            return NFS4ERR_EXIST;
        }
        uint32_t ast = applyFattr4(r, child); // createattrs
        touchDir(dir);
        t_fh.cfh = {child->id(), child->snap()};
        t_fh.hasCfh = true;
        rw.u32(NFS4_OK);
        changeInfo(rw, before, dir->ctime());
        rw.u32(2); // attrset bitmap
        rw.u32(0);
        rw.u32(0);
        return ast == NFS4_OK ? NFS4_OK : NFS4_OK; // attrs best-effort
    }

    uint32_t opRemove(Reader& r, Writer& rw) {
        IPrfs& fs = m_host.fs();
        std::string name = r.str();
        Node dir = cur(fs);
        if (!dir || dir->type() != Type::DIR) {
            rw.u32(NFS4ERR_NOTDIR);
            return NFS4ERR_NOTDIR;
        }
        if (!live(t_fh.cfh.second)) {
            rw.u32(NFS4ERR_ROFS);
            return NFS4ERR_ROFS;
        }
        Node victim = fs.lookup(dir, name);
        if (victim && victim->type() == Type::DIR && !fs.readdir(victim).empty()) {
            rw.u32(NFS4ERR_NOTEMPTY);
            return NFS4ERR_NOTEMPTY;
        }
        uint64_t before = dir->ctime();
        Error e = fs.unlink(dir, name);
        if (e != Error::OK) {
            rw.u32(toNfs4(e));
            return toNfs4(e);
        }
        touchDir(dir);
        rw.u32(NFS4_OK);
        changeInfo(rw, before, dir->ctime());
        return NFS4_OK;
    }

    //  RENAME — source dir is the SAVED fh, target dir the CURRENT fh.
    uint32_t opRename(Reader& r, Writer& rw) {
        IPrfs& fs = m_host.fs();
        std::string oldname = r.str();
        std::string newname = r.str();
        if (!t_fh.hasSfh || !t_fh.hasCfh) {
            rw.u32(NFS4ERR_NOFILEHANDLE);
            return NFS4ERR_NOFILEHANDLE;
        }
        Node sdir = fs.nodeById(t_fh.sfh.first, t_fh.sfh.second);
        Node ddir = fs.nodeById(t_fh.cfh.first, t_fh.cfh.second);
        if (!sdir || !ddir) {
            rw.u32(NFS4ERR_STALE);
            return NFS4ERR_STALE;
        }
        if (!live(t_fh.sfh.second) || !live(t_fh.cfh.second)) {
            rw.u32(NFS4ERR_ROFS);
            return NFS4ERR_ROFS;
        }
        uint64_t sbefore = sdir->ctime(), dbefore = ddir->ctime();
        Error e = fs.move(sdir, oldname, ddir, newname);
        if (e != Error::OK) {
            rw.u32(toNfs4(e));
            return toNfs4(e);
        }
        touchDir(sdir);
        touchDir(ddir);
        rw.u32(NFS4_OK);
        changeInfo(rw, sbefore, sdir->ctime()); // source_cinfo
        changeInfo(rw, dbefore, ddir->ctime()); // target_cinfo
        return NFS4_OK;
    }

    //  LINK — hard-link the SAVED-fh file into the CURRENT-fh dir under newname.
    uint32_t opLink(Reader& r, Writer& rw) {
        IPrfs& fs = m_host.fs();
        std::string newname = r.str();
        if (!t_fh.hasSfh || !t_fh.hasCfh) {
            rw.u32(NFS4ERR_NOFILEHANDLE);
            return NFS4ERR_NOFILEHANDLE;
        }
        Node file = fs.nodeById(t_fh.sfh.first, t_fh.sfh.second);
        Node dir = fs.nodeById(t_fh.cfh.first, t_fh.cfh.second);
        if (!file || !dir || dir->type() != Type::DIR) {
            rw.u32(NFS4ERR_NOTDIR);
            return NFS4ERR_NOTDIR;
        }
        if (!live(t_fh.cfh.second)) {
            rw.u32(NFS4ERR_ROFS);
            return NFS4ERR_ROFS;
        }
        uint64_t before = dir->ctime();
        Error e = fs.link(dir, newname, file);
        if (e != Error::OK) {
            rw.u32(toNfs4(e));
            return toNfs4(e);
        }
        touchDir(dir);
        rw.u32(NFS4_OK);
        changeInfo(rw, before, dir->ctime());
        return NFS4_OK;
    }

    //  Is `clientid` a confirmed lease this server boot? OPEN/LOCK require it; a
    //  stale (unconfirmed) clientid ⇒ NFS4ERR_STALE_CLIENTID, forcing the client
    //  to (re)run SETCLIENTID/CONFIRM before it can hold state.
    bool clientConfirmed(uint64_t clientid) {
        std::lock_guard<std::mutex> lk(m_clientMu);
        auto it = m_clients.find(clientid);
        return it != m_clients.end() && it->second.confirmed;
    }

    //  Drop all open/lock state owned by `clientid` (client reboot / lease loss).
    void purgeClientState(uint64_t clientid) {
        std::lock_guard<std::mutex> sl(m_stateMu);
        for (auto it = m_opens.begin(); it != m_opens.end();) {
            it = it->second.clientid == clientid ? m_opens.erase(it) : std::next(it);
        }
        for (auto it = m_locks.begin(); it != m_locks.end();) {
            it = it->second.clientid == clientid ? m_locks.erase(it) : std::next(it);
        }
    }

    //  SETCLIENTID (RFC 7530 §16.33): register a client instance and hand back a
    //  clientid plus a confirm verifier the client must echo in _CONFIRM. Idempotent
    //  on (id, verifier): a repeat returns the *same* clientid (RFC 7530 §9.1.1 case
    //  "update") rather than churning a fresh one — otherwise the client re-confirms
    //  endlessly and its just-confirmed lease gets purged as "stale".
    uint32_t opSetClientid(Reader& r, Writer& rw) {
        uint64_t verifier = r.u64(); // client.verifier
        std::string id = r.str();    // client.id<>
        uint32_t cbProgram = r.u32();
        std::string cbNetid = r.str(); // cb_location.r_netid
        std::string cbAddr = r.str();  // cb_location.r_addr
        uint32_t cbIdent = r.u32();    // callback_ident
        if (!r.ok) {
            rw.u32(NFS4ERR_INVAL);
            return NFS4ERR_INVAL;
        }
        std::lock_guard<std::mutex> lk(m_clientMu);
        uint64_t clientid = 0;
        for (auto const& [k, c] : m_clients) {
            if (c.id == id && c.verifier == verifier) { // same instance ⇒ reuse
                clientid = k;
                break;
            }
        }
        uint64_t cv;
        if (clientid) {
            ClientRec& rec = m_clients[clientid]; // refresh callback, keep confirm state
            rec.cbProgram = cbProgram;
            rec.cbNetid = cbNetid;
            rec.cbAddr = cbAddr;
            rec.cbIdent = cbIdent;
            cv = rec.confirmVerf;
        } else {
            clientid = m_nextClientid++;
            //  Confirm verifier: deterministic mix of the request (no wall clock/RNG).
            cv = viewFileid(clientid ^ verifier, std::hash<std::string>{}(id));
            m_clients[clientid] =
                ClientRec{clientid, id, verifier, cv, false, cbProgram, cbNetid, cbAddr, cbIdent};
        }
        m_host.log().info("nfsv4: SETCLIENTID id='{}' cb={}/{} -> clientid={} (callback recorded)",
                          id, cbNetid, cbAddr, clientid);
        rw.u32(NFS4_OK);
        rw.u64(clientid); // SETCLIENTID4resok.clientid
        rw.u64(cv);       // setclientid_confirm verifier (8 bytes)
        return NFS4_OK;
    }

    //  SETCLIENTID_CONFIRM: promote the pending clientid to confirmed. If another
    //  confirmed lease exists for the same id string (client rebooted / re-mounted),
    //  purge its state first — this is the reboot-recovery path.
    uint32_t opSetClientidConfirm(Reader& r, Writer& rw) {
        uint64_t clientid = r.u64();
        uint64_t confirm = r.u64(); // setclientid_confirm verifier
        std::lock_guard<std::mutex> lk(m_clientMu);
        auto it = m_clients.find(clientid);
        if (it == m_clients.end()) {
            rw.u32(NFS4ERR_STALE_CLIENTID);
            return NFS4ERR_STALE_CLIENTID;
        }
        ClientRec& rec = it->second;
        if (rec.confirmVerf != confirm) {
            rw.u32(NFS4ERR_STALE_CLIENTID);
            return NFS4ERR_STALE_CLIENTID;
        }
        if (!rec.confirmed) {
            std::vector<uint64_t> stale; // other confirmed leases for the same client
            for (auto const& [k, c] : m_clients) {
                if (k != clientid && c.confirmed && c.id == rec.id) {
                    stale.push_back(k);
                }
            }
            for (uint64_t k : stale) {
                purgeClientState(k);
                m_clients.erase(k);
            }
            rec.confirmed = true;
            m_host.log().info("nfsv4: SETCLIENTID_CONFIRM clientid={} confirmed ({} stale purged)",
                              clientid, stale.size());
        }
        rw.u32(NFS4_OK);
        return NFS4_OK;
    }

    //  RENEW: refresh the lease. Unknown/unconfirmed clientid ⇒ STALE_CLIENTID.
    //  Leases never expire here, so a confirmed clientid always renews OK.
    uint32_t opRenew(Reader& r, Writer& rw) {
        uint64_t clientid = r.u64();
        if (!clientConfirmed(clientid)) {
            rw.u32(NFS4ERR_STALE_CLIENTID);
            return NFS4ERR_STALE_CLIENTID;
        }
        rw.u32(NFS4_OK);
        return NFS4_OK;
    }

    IHost& m_host;
    asio::io_context m_ctx;
    std::optional<tcp::acceptor> m_acc;
    std::vector<std::thread> m_threads;
    std::atomic<bool> m_running{false};

    //  Client lease registry (SETCLIENTID). Guarded by m_clientMu, which is always
    //  taken *before* m_stateMu (reboot recovery purges open/lock state). Keyed by
    //  the server-assigned clientid; m_nextClientid mints them.
    std::mutex m_clientMu;
    uint64_t m_nextClientid = 1;
    std::unordered_map<uint64_t, ClientRec> m_clients;

    //  READDIR cookie→resume-name cache (see nameCookie/resumeName). Bounded;
    //  guarded independently of the store lock since READDIR holds it shared.
    static constexpr size_t CK_CACHE_MAX = 1u << 16;
    std::mutex m_ckMu;
    std::unordered_map<uint64_t, std::string> m_ckName;

    //  Open/lock state (guarded by m_stateMu, taken after the store lock). Keyed
    //  by the stateid's 8-byte id; `m_nextState` mints ids.
    std::mutex m_stateMu;
    uint64_t m_nextState = 1;
    std::unordered_map<uint64_t, OpenState> m_opens;
    std::unordered_map<uint64_t, LockState> m_locks;
    //  Open-owners (clientid, owner) that completed OPEN_CONFIRM. Once confirmed, an
    //  owner can hold locks and its later opens skip the CONFIRM handshake.
    std::set<std::pair<uint64_t, std::string>> m_confirmedOwners;
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
