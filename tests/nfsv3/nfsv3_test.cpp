// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  nfsv3 plugin — end-to-end RPC test. Loads nfsv3.so via the host, then over a
//  TCP connection walks the real client flow: MOUNT MNT to obtain the root
//  filehandle, then the read surface (GETATTR/LOOKUP/ACCESS/READ/READLINK/
//  READDIR/READDIRPLUS/FSSTAT/FSINFO/PATHCONF) and the write surface (MKDIR/
//  CREATE/WRITE/SETATTR/REMOVE/RMDIR, plus a snapshot-fh ROFS check) against the
//  live store the host wraps. Proves the transport (record marking + call/reply)
//  and the XDR / filehandle mapping. NFSV3_PLUGIN_SO is the .so path.
//
#include "prfs/host.hpp"
#include "prfs/memstore.hpp"

#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace prfs;

namespace {

constexpr uint32_t PROG_NFS = 100003, NFS_V3 = 3;
constexpr uint32_t PROG_MOUNT = 100005, MOUNT_V3 = 3;
constexpr uint64_t LAT = ~uint64_t(0); // LATEST

std::shared_ptr<spdlog::logger> quietLogger() {
    return std::make_shared<spdlog::logger>("nfsv3-test",
                                            std::make_shared<spdlog::sinks::null_sink_mt>());
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24));
    v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}

uint32_t get32(uint8_t const* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

uint64_t get64(uint8_t const* p) { return uint64_t(get32(p)) << 32 | get32(p + 4); }

size_t pad4(size_t n) { return (n + 3) & ~size_t(3); }

void putU64(std::vector<uint8_t>& v, uint64_t x) {
    put32(v, uint32_t(x >> 32));
    put32(v, uint32_t(x));
}

std::vector<uint8_t> fhArg(uint64_t id, uint64_t snap) {
    std::vector<uint8_t> a;
    put32(a, 16);
    put32(a, uint32_t(id >> 32));
    put32(a, uint32_t(id));
    put32(a, uint32_t(snap >> 32));
    put32(a, uint32_t(snap));
    return a;
}

std::vector<uint8_t> strArg(std::string const& s) {
    std::vector<uint8_t> a;
    put32(a, uint32_t(s.size()));
    a.insert(a.end(), s.begin(), s.end());
    while (a.size() % 4) {
        a.push_back(0);
    }
    return a;
}

int connectLoopback(int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(uint16_t(port));
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

struct Reply {
    uint32_t astat;
    std::vector<uint8_t> body; // proc result (starts with nfsstat3 for NFS calls)
};

//  Send one RPC call and read back the accepted reply.
Reply rpc(int fd, uint32_t prog, uint32_t vers, uint32_t proc, std::vector<uint8_t> const& args) {
    std::vector<uint8_t> call;
    put32(call, 0x11223344); // xid
    put32(call, 0);          // CALL
    put32(call, 2);          // rpcvers
    put32(call, prog);
    put32(call, vers);
    put32(call, proc);
    put32(call, 0);
    put32(call, 0); // cred AUTH_NONE
    put32(call, 0);
    put32(call, 0); // verf AUTH_NONE
    call.insert(call.end(), args.begin(), args.end());

    std::vector<uint8_t> frame;
    put32(frame, 0x80000000u | uint32_t(call.size()));
    frame.insert(frame.end(), call.begin(), call.end());
    EXPECT_EQ(::send(fd, frame.data(), frame.size(), 0), ssize_t(frame.size()));

    uint8_t mark[4];
    EXPECT_EQ(::recv(fd, mark, 4, MSG_WAITALL), 4);
    uint32_t rlen = get32(mark) & 0x7fffffff;
    std::vector<uint8_t> rep(rlen);
    EXPECT_EQ(::recv(fd, rep.data(), rlen, MSG_WAITALL), ssize_t(rlen));

    //  xid, REPLY(1), MSG_ACCEPTED(0), verf flavor, verf len, [verf body], astat.
    EXPECT_EQ(get32(&rep[0]), 0x11223344u);
    EXPECT_EQ(get32(&rep[4]), 1u); // REPLY
    EXPECT_EQ(get32(&rep[8]), 0u); // MSG_ACCEPTED
    uint32_t verfLen = get32(&rep[16]);
    size_t off = 20 + ((verfLen + 3u) & ~3u);
    uint32_t astat = get32(&rep[off]);
    off += 4;
    return {astat, std::vector<uint8_t>(rep.begin() + off, rep.end())};
}

} // namespace

TEST(NfsV3, MountWalkStat) {
    const int port = 34567;

    auto fs = makeMemStore();
    auto root = fs->rwRoot();
    auto file = fs->mkfile("hi there");
    file->size(8);
    ASSERT_EQ(fs->link(root, "hello", file), Error::OK);
    auto lnk = fs->symlink("/target/path");
    ASSERT_EQ(fs->link(root, "lnk", lnk), Error::OK);
    uint64_t snapId = fs->snapshot(); // a read-only past view
    uint64_t rootId = root->id();
    uint64_t fileId = file->id();
    uint64_t lnkId = lnk->id();

    auto log = quietLogger();
    host::Host h(*fs, *log); // di::global(): engines, rng, and content live here
    h.setOption("port", std::to_string(port));

    host::Loader loader(h);
    ASSERT_TRUE(loader.load(NFSV3_PLUGIN_SO));
    loader.startServices();

    int fd = connectLoopback(port);
    ASSERT_GE(fd, 0) << "could not connect to the nfsv3 service";

    //  NFS NULL — transport sanity.
    EXPECT_EQ(rpc(fd, PROG_NFS, NFS_V3, 0, {}).astat, 0u); // accept_stat SUCCESS

    //  MOUNT MNT "/" → root filehandle.
    Reply mnt = rpc(fd, PROG_MOUNT, MOUNT_V3, 1, strArg("/"));
    ASSERT_EQ(mnt.astat, 0u);
    ASSERT_GE(mnt.body.size(), 24u);
    EXPECT_EQ(get32(&mnt.body[0]), 0u);  // MNT3_OK
    EXPECT_EQ(get32(&mnt.body[4]), 16u); // fhandle3 length
    uint64_t fhId = get64(&mnt.body[8]);
    uint64_t fhSnap = get64(&mnt.body[16]);
    EXPECT_EQ(fhId, rootId);
    EXPECT_EQ(fhSnap, LAT); // live root view

    //  GETATTR the root fh → a directory whose fileid is the root's.
    Reply ga = rpc(fd, PROG_NFS, NFS_V3, 1, fhArg(fhId, fhSnap));
    ASSERT_EQ(ga.astat, 0u);
    EXPECT_EQ(get32(&ga.body[0]), 0u); // NFS3_OK
    EXPECT_EQ(get32(&ga.body[4]), 2u); // ftype3 NF3DIR
    EXPECT_EQ(get64(&ga.body[56]), rootId);

    //  LOOKUP "hello" under the root → the file's fh.
    std::vector<uint8_t> lkArgs = fhArg(fhId, fhSnap);
    std::vector<uint8_t> nm = strArg("hello");
    lkArgs.insert(lkArgs.end(), nm.begin(), nm.end());
    Reply lk = rpc(fd, PROG_NFS, NFS_V3, 3, lkArgs);
    ASSERT_EQ(lk.astat, 0u);
    EXPECT_EQ(get32(&lk.body[0]), 0u);  // NFS3_OK
    EXPECT_EQ(get32(&lk.body[4]), 16u); // object fh length
    uint64_t childId = get64(&lk.body[8]);
    uint64_t childSnap = get64(&lk.body[16]);
    EXPECT_EQ(childId, fileId);

    //  GETATTR the looked-up fh → a regular file of the size we set.
    Reply gf = rpc(fd, PROG_NFS, NFS_V3, 1, fhArg(childId, childSnap));
    ASSERT_EQ(gf.astat, 0u);
    EXPECT_EQ(get32(&gf.body[0]), 0u);  // NFS3_OK
    EXPECT_EQ(get32(&gf.body[4]), 1u);  // ftype3 NF3REG
    EXPECT_EQ(get64(&gf.body[24]), 8u); // size

    //  READ the file → its bytes, count, and eof. Body after status(4) is a
    //  present post_op_attr (1 + fattr3[84] = 88), then count/eof/data<>.
    std::vector<uint8_t> rdArgs = fhArg(childId, childSnap);
    put32(rdArgs, 0);   // offset (u64) high
    put32(rdArgs, 0);   // offset low
    put32(rdArgs, 100); // count (more than the file holds)
    Reply rd = rpc(fd, PROG_NFS, NFS_V3, 6, rdArgs);
    ASSERT_EQ(rd.astat, 0u);
    ASSERT_GE(rd.body.size(), 112u);
    EXPECT_EQ(get32(&rd.body[0]), 0u);   // NFS3_OK
    EXPECT_EQ(get32(&rd.body[92]), 8u);  // count
    EXPECT_EQ(get32(&rd.body[96]), 1u);  // eof
    EXPECT_EQ(get32(&rd.body[100]), 8u); // data length
    //  Content is GENERATED from the seed (nodeID here), never the literal bytes
    //  the store happens to hold — so it is not "hi there".
    EXPECT_NE(std::string(reinterpret_cast<char const*>(&rd.body[104]), 8), "hi there");

    //  READ at/after EOF → zero bytes, eof set.
    std::vector<uint8_t> eofArgs = fhArg(childId, childSnap);
    put32(eofArgs, 0);
    put32(eofArgs, 8); // offset == size
    put32(eofArgs, 100);
    Reply re = rpc(fd, PROG_NFS, NFS_V3, 6, eofArgs);
    ASSERT_EQ(re.astat, 0u);
    EXPECT_EQ(get32(&re.body[92]), 0u); // count 0
    EXPECT_EQ(get32(&re.body[96]), 1u); // eof

    //  LOOKUP a missing name → NFS3ERR_NOENT (2).
    std::vector<uint8_t> missArgs = fhArg(fhId, fhSnap);
    std::vector<uint8_t> miss = strArg("nope");
    missArgs.insert(missArgs.end(), miss.begin(), miss.end());
    EXPECT_EQ(get32(&rpc(fd, PROG_NFS, NFS_V3, 3, missArgs).body[0]), 2u);

    //  ACCESS asking for the full mask (incl. MODIFY/EXTEND/DELETE, 0x3f) → all
    //  granted (permissive target, so write opens succeed).
    std::vector<uint8_t> acArgs = fhArg(fhId, fhSnap);
    put32(acArgs, 0x3f);
    Reply ac = rpc(fd, PROG_NFS, NFS_V3, 4, acArgs);
    ASSERT_EQ(ac.astat, 0u);
    EXPECT_EQ(get32(&ac.body[0]), 0u);     // NFS3_OK
    EXPECT_EQ(get32(&ac.body[92]), 0x3fu); // granted after status + post_op_attr

    //  GETATTR a bogus fh → NFS3ERR_STALE (70).
    EXPECT_EQ(get32(&rpc(fd, PROG_NFS, NFS_V3, 1, fhArg(999999, LAT)).body[0]), 70u);

    //  READDIR the root. Body: status(4) + present dir_attr(88) + cookieverf(8)
    //  = entries begin at 100; each is vf(4)+fileid(8)+name<>+cookie(8), the list
    //  ends with vf=0 then eof. Expect ".", "..", "hello".
    std::vector<uint8_t> ddArgs = fhArg(fhId, fhSnap);
    putU64(ddArgs, 0);   // cookie 0 (from the start)
    putU64(ddArgs, 0);   // cookieverf[8]
    put32(ddArgs, 8192); // count
    Reply dd = rpc(fd, PROG_NFS, NFS_V3, 16, ddArgs);
    ASSERT_EQ(dd.astat, 0u);
    EXPECT_EQ(get32(&dd.body[0]), 0u); // NFS3_OK
    {
        size_t o = 100;
        std::vector<std::string> names;
        while (get32(&dd.body[o]) == 1) {
            o += 4; // value-follows
            o += 8; // fileid
            uint32_t nl = get32(&dd.body[o]);
            o += 4;
            names.emplace_back(reinterpret_cast<char const*>(&dd.body[o]), nl);
            o += pad4(nl);
            o += 8; // cookie
        }
        EXPECT_EQ(get32(&dd.body[o + 4]), 1u); // eof
        EXPECT_EQ(names.size(), 4u);           // ".", "..", "hello", "lnk"
        EXPECT_NE(std::find(names.begin(), names.end(), "."), names.end());
        EXPECT_NE(std::find(names.begin(), names.end(), ".."), names.end());
        EXPECT_NE(std::find(names.begin(), names.end(), "hello"), names.end());
        EXPECT_NE(std::find(names.begin(), names.end(), "lnk"), names.end());
        //  .snapshot is hidden from readdir (NetApp convention) …
        EXPECT_EQ(std::find(names.begin(), names.end(), ".snapshot"), names.end());
    }

    //  … but still resolvable by name (LOOKUP succeeds).
    {
        std::vector<uint8_t> a = fhArg(fhId, fhSnap);
        std::vector<uint8_t> n = strArg(".snapshot");
        a.insert(a.end(), n.begin(), n.end());
        EXPECT_EQ(get32(&rpc(fd, PROG_NFS, NFS_V3, 3, a).body[0]), 0u); // NFS3_OK
    }

    //  READDIR resuming after cookie 2 (past "." and "..") → the rest.
    std::vector<uint8_t> ddArgs2 = fhArg(fhId, fhSnap);
    putU64(ddArgs2, 2);
    putU64(ddArgs2, 0);
    put32(ddArgs2, 8192);
    Reply dd2 = rpc(fd, PROG_NFS, NFS_V3, 16, ddArgs2);
    ASSERT_EQ(dd2.astat, 0u);
    {
        size_t o = 100;
        int n = 0;
        while (get32(&dd2.body[o]) == 1) {
            o += 4 + 8;
            o += 4 + pad4(get32(&dd2.body[o]));
            o += 8;
            ++n;
        }
        EXPECT_EQ(n, 2); // "hello" and "lnk" remain
    }

    //  READDIRPLUS: like READDIR but each entry carries name_attributes
    //  (post_op_attr) and name_handle (post_op_fh3).
    std::vector<uint8_t> dpArgs = fhArg(fhId, fhSnap);
    putU64(dpArgs, 0);   // cookie
    putU64(dpArgs, 0);   // cookieverf
    put32(dpArgs, 4096); // dircount
    put32(dpArgs, 8192); // maxcount
    Reply dp = rpc(fd, PROG_NFS, NFS_V3, 17, dpArgs);
    ASSERT_EQ(dp.astat, 0u);
    EXPECT_EQ(get32(&dp.body[0]), 0u);
    {
        size_t o = 100;
        int n = 0;
        bool sawHello = false;
        while (get32(&dp.body[o]) == 1) {
            o += 4; // value-follows
            uint64_t fid = get64(&dp.body[o]);
            o += 8;
            uint32_t nl = get32(&dp.body[o]);
            o += 4;
            std::string nm(reinterpret_cast<char const*>(&dp.body[o]), nl);
            o += pad4(nl);
            o += 8;                        // cookie
            if (get32(&dp.body[o]) == 1) { // name_attributes present
                o += 4 + 84;
            } else {
                o += 4;
            }
            if (get32(&dp.body[o]) == 1) { // name_handle present
                o += 4;
                o += 4 + pad4(get32(&dp.body[o]));
            } else {
                o += 4;
            }
            if (nm == "hello") {
                sawHello = true;
                EXPECT_EQ(fid, fileId);
            }
            ++n;
        }
        EXPECT_EQ(get32(&dp.body[o + 4]), 1u); // eof
        EXPECT_EQ(n, 4);                       // ".", "..", "hello", "lnk"
        EXPECT_TRUE(sawHello);
    }

    //  READLINK the symlink → its target string (after status + post_op_attr).
    Reply rl = rpc(fd, PROG_NFS, NFS_V3, 5, fhArg(lnkId, fhSnap));
    ASSERT_EQ(rl.astat, 0u);
    EXPECT_EQ(get32(&rl.body[0]), 0u);   // NFS3_OK
    EXPECT_EQ(get32(&rl.body[92]), 12u); // nfspath3 length ("/target/path")
    EXPECT_EQ(std::string(reinterpret_cast<char const*>(&rl.body[96]), 12), "/target/path");

    //  READLINK a non-symlink → NFS3ERR_INVAL (22).
    EXPECT_EQ(get32(&rpc(fd, PROG_NFS, NFS_V3, 5, fhArg(childId, childSnap)).body[0]), 22u);

    //  PATHCONF: obj_attr then linkmax, name_max, and the four booleans.
    Reply pc = rpc(fd, PROG_NFS, NFS_V3, 20, fhArg(fhId, fhSnap));
    ASSERT_EQ(pc.astat, 0u);
    EXPECT_EQ(get32(&pc.body[0]), 0u);    // NFS3_OK
    EXPECT_EQ(get32(&pc.body[96]), 255u); // name_max

    //  FSSTAT: obj_attr then the six size3 counters + invarsec. tbytes is the
    //  advertised capacity (FsConfig default, 1 TiB).
    Reply fst = rpc(fd, PROG_NFS, NFS_V3, 18, fhArg(fhId, fhSnap));
    ASSERT_EQ(fst.astat, 0u);
    EXPECT_EQ(get32(&fst.body[0]), 0u);                 // NFS3_OK
    EXPECT_EQ(get64(&fst.body[92]), uint64_t(1) << 40); // tbytes

    //  FSINFO: obj_attr then the transfer parameters. rtmax matches MAX_READ.
    Reply fsi = rpc(fd, PROG_NFS, NFS_V3, 19, fhArg(fhId, fhSnap));
    ASSERT_EQ(fsi.astat, 0u);
    EXPECT_EQ(get32(&fsi.body[0]), 0u);        // NFS3_OK
    EXPECT_EQ(get32(&fsi.body[92]), 1u << 20); // rtmax

    //  ---- write surface ----
    auto sattrNone = [](std::vector<uint8_t>& a) {
        for (int i = 0; i < 6; ++i) {
            put32(a, 0); // mode/uid/gid/size unset; atime/mtime DONT_CHANGE
        }
    };

    //  MKDIR "sub" under the root, then confirm LOOKUP resolves it.
    std::vector<uint8_t> mkd = fhArg(fhId, fhSnap);
    std::vector<uint8_t> subn = strArg("sub");
    mkd.insert(mkd.end(), subn.begin(), subn.end());
    sattrNone(mkd);
    EXPECT_EQ(rpc(fd, PROG_NFS, NFS_V3, 9, mkd).body[3], 0u); // status low byte == NFS3_OK
    std::vector<uint8_t> lsub = fhArg(fhId, fhSnap);
    std::vector<uint8_t> lsubn = strArg("sub");
    lsub.insert(lsub.end(), lsubn.begin(), lsubn.end());
    EXPECT_EQ(get32(&rpc(fd, PROG_NFS, NFS_V3, 3, lsub).body[0]), 0u); // LOOKUP OK

    //  CREATE "file.txt" (UNCHECKED) → a new file handle.
    std::vector<uint8_t> cr = fhArg(fhId, fhSnap);
    std::vector<uint8_t> crn = strArg("file.txt");
    cr.insert(cr.end(), crn.begin(), crn.end());
    put32(cr, 0); // createmode3 UNCHECKED
    sattrNone(cr);
    Reply crr = rpc(fd, PROG_NFS, NFS_V3, 8, cr);
    ASSERT_EQ(crr.astat, 0u);
    ASSERT_EQ(get32(&crr.body[0]), 0u);  // NFS3_OK
    ASSERT_EQ(get32(&crr.body[4]), 1u);  // handle-follows
    ASSERT_EQ(get32(&crr.body[8]), 16u); // fh length
    uint64_t newId = get64(&crr.body[12]);
    uint64_t newSnap = get64(&crr.body[20]);

    //  WRITE "payload!" at offset 0. The bytes are NOT stored — they are folded
    //  into the file's content seed. count "written" is the full length; size
    //  grows to cover it.
    std::string payload = "payload!";
    std::vector<uint8_t> wr = fhArg(newId, newSnap);
    putU64(wr, 0);                             // offset
    put32(wr, uint32_t(payload.size()));       // count
    put32(wr, 2);                              // stable_how FILE_SYNC
    std::vector<uint8_t> pl = strArg(payload); // data<>
    wr.insert(wr.end(), pl.begin(), pl.end());
    Reply wrr = rpc(fd, PROG_NFS, NFS_V3, 7, wr);
    ASSERT_EQ(wrr.astat, 0u);
    EXPECT_EQ(get32(&wrr.body[0]), 0u); // NFS3_OK
    //  status(4) + wcc pre(28) + wcc post(88) → count at 120.
    EXPECT_EQ(get32(&wrr.body[120]), 8u); // count "written"

    //  GETATTR → size reflects the write (metadata), though no bytes were stored.
    Reply gw = rpc(fd, PROG_NFS, NFS_V3, 1, fhArg(newId, newSnap));
    EXPECT_EQ(get64(&gw.body[24]), 8u); // size == 8

    //  READ returns GENERATED content, never the literal bytes written.
    std::vector<uint8_t> rb = fhArg(newId, newSnap);
    putU64(rb, 0);
    put32(rb, 100);
    Reply rbr = rpc(fd, PROG_NFS, NFS_V3, 6, rb);
    ASSERT_EQ(rbr.astat, 0u);
    EXPECT_EQ(get32(&rbr.body[92]), 8u); // count
    std::string got(reinterpret_cast<char const*>(&rbr.body[104]), 8);
    EXPECT_NE(got, "payload!"); // synthesized from the seed, not stored

#ifdef PRFS_WITH_CONTENT
    //  A different write evolves the seed → different generated content, proving
    //  reads reflect writes without any bytes being stored.
    std::vector<uint8_t> wr2 = fhArg(newId, newSnap);
    putU64(wr2, 0);
    put32(wr2, 8);
    put32(wr2, 2);
    std::vector<uint8_t> pl2 = strArg("XYZW1234");
    wr2.insert(wr2.end(), pl2.begin(), pl2.end());
    ASSERT_EQ(get32(&rpc(fd, PROG_NFS, NFS_V3, 7, wr2).body[0]), 0u);
    Reply rbr3 = rpc(fd, PROG_NFS, NFS_V3, 6, rb);
    std::string got2(reinterpret_cast<char const*>(&rbr3.body[104]), 8);
    EXPECT_NE(got2, got); // the write changed the generated content
#endif

    //  SETATTR: truncate to 4 bytes, then READ sees the shorter file.
    std::vector<uint8_t> sa = fhArg(newId, newSnap);
    put32(sa, 0);                                                    // set_mode3 no
    put32(sa, 0);                                                    // set_uid3 no
    put32(sa, 0);                                                    // set_gid3 no
    put32(sa, 1);                                                    // set_size3 yes
    putU64(sa, 4);                                                   // size = 4
    put32(sa, 0);                                                    // set_atime DONT
    put32(sa, 0);                                                    // set_mtime DONT
    put32(sa, 0);                                                    // sattrguard3: no check
    EXPECT_EQ(get32(&rpc(fd, PROG_NFS, NFS_V3, 2, sa).body[0]), 0u); // NFS3_OK
    Reply rbr2 = rpc(fd, PROG_NFS, NFS_V3, 6, rb);
    EXPECT_EQ(get32(&rbr2.body[92]), 4u); // count now 4

    //  REMOVE the file and RMDIR the (empty) directory.
    std::vector<uint8_t> rm = fhArg(fhId, fhSnap);
    std::vector<uint8_t> rmn = strArg("file.txt");
    rm.insert(rm.end(), rmn.begin(), rmn.end());
    EXPECT_EQ(get32(&rpc(fd, PROG_NFS, NFS_V3, 12, rm).body[0]), 0u);  // REMOVE OK
    EXPECT_EQ(get32(&rpc(fd, PROG_NFS, NFS_V3, 3, lsub).body[0]), 0u); // "sub" still there
    std::vector<uint8_t> rd2 = fhArg(fhId, fhSnap);
    std::vector<uint8_t> rd2n = strArg("sub");
    rd2.insert(rd2.end(), rd2n.begin(), rd2n.end());
    EXPECT_EQ(get32(&rpc(fd, PROG_NFS, NFS_V3, 13, rd2).body[0]), 0u); // RMDIR OK

    //  Writes through a snapshot fh are refused (read-only view) → NFS3ERR_ROFS.
    std::vector<uint8_t> ro = fhArg(rootId, snapId);
    std::vector<uint8_t> ron = strArg("nope");
    ro.insert(ro.end(), ron.begin(), ron.end());
    sattrNone(ro);
    EXPECT_EQ(get32(&rpc(fd, PROG_NFS, NFS_V3, 9, ro).body[0]), 30u); // MKDIR → ROFS

    ::close(fd);
    loader.stopAll();
}

//  --time-advance: each NFS mutation bumps the logical clock, so two files
//  created in sequence get distinct, increasing mtimes.
TEST(NfsV3, TimeAdvance) {
    const int port = 34568;

    auto fs = makeMemStore();
    fs->setTime(1000);
    auto log = quietLogger();
    host::Host h(*fs, *log);
    h.setOption("port", std::to_string(port));
    h.setOption("time-advance", "1");

    host::Loader loader(h);
    ASSERT_TRUE(loader.load(NFSV3_PLUGIN_SO));
    loader.startServices();

    int fd = connectLoopback(port);
    ASSERT_GE(fd, 0);

    Reply mnt = rpc(fd, PROG_MOUNT, MOUNT_V3, 1, strArg("/"));
    ASSERT_EQ(mnt.astat, 0u);
    uint64_t rid = get64(&mnt.body[8]);
    uint64_t rs = get64(&mnt.body[16]);

    auto create = [&](std::string const& name, uint64_t& id, uint64_t& sn) {
        std::vector<uint8_t> cr = fhArg(rid, rs);
        std::vector<uint8_t> nm = strArg(name);
        cr.insert(cr.end(), nm.begin(), nm.end());
        put32(cr, 0); // UNCHECKED
        for (int i = 0; i < 6; ++i) {
            put32(cr, 0); // sattr3 unset
        }
        Reply r = rpc(fd, PROG_NFS, NFS_V3, 8, cr);
        ASSERT_EQ(r.astat, 0u);
        ASSERT_EQ(get32(&r.body[0]), 0u); // NFS3_OK
        id = get64(&r.body[12]);
        sn = get64(&r.body[20]);
    };
    auto mtimeOf = [&](uint64_t id, uint64_t sn) {
        Reply g = rpc(fd, PROG_NFS, NFS_V3, 1, fhArg(id, sn));
        return get32(&g.body[4 + 68]); // fattr3 mtime seconds
    };

    uint64_t i1, s1, i2, s2;
    create("a", i1, s1);
    create("b", i2, s2);
    uint32_t m1 = mtimeOf(i1, s1);
    uint32_t m2 = mtimeOf(i2, s2);
    EXPECT_GE(m1, 1000u); // stamped from the seeded clock
    EXPECT_GT(m2, m1);    // the clock advanced between the two creates

    ::close(fd);
    loader.stopAll();
}

//  A namespace change (create/remove/rename) must bump the PARENT directory's
//  mtime and ctime (POSIX). The store leaves link/unlink timestamps to the caller
//  (memstore.cpp), so nfsv3 stamps them. Regression for the gap pjdfstest
//  surfaced: without it a backup tool that detects changed directories by mtime
//  would miss dirs whose entries were added or removed.
TEST(NfsV3, ParentDirTimestampsOnMutation) {
    const int port = 34576;

    auto fs = makeMemStore();
    auto log = quietLogger();
    host::Host h(*fs, *log);
    h.setOption("port", std::to_string(port));
    h.setOption("time-advance", "1"); // each mutation advances the logical clock

    host::Loader loader(h);
    ASSERT_TRUE(loader.load(NFSV3_PLUGIN_SO));
    loader.startServices();

    int fd = connectLoopback(port);
    ASSERT_GE(fd, 0);

    Reply mnt = rpc(fd, PROG_MOUNT, MOUNT_V3, 1, strArg("/"));
    ASSERT_EQ(mnt.astat, 0u);
    uint64_t rid = get64(&mnt.body[8]);
    uint64_t rs = get64(&mnt.body[16]);

    fs->setTime(1000); // root was created at clock 0; advance so stamps are visible

    auto mtimeOf = [&]() {
        Reply g = rpc(fd, PROG_NFS, NFS_V3, 1, fhArg(rid, rs));
        return get32(&g.body[4 + 68]); // fattr3 mtime seconds
    };
    auto ctimeOf = [&]() {
        Reply g = rpc(fd, PROG_NFS, NFS_V3, 1, fhArg(rid, rs));
        return get32(&g.body[4 + 76]); // fattr3 ctime seconds
    };
    auto mkdirIn = [&](std::string const& name) {
        std::vector<uint8_t> a = fhArg(rid, rs);
        std::vector<uint8_t> nm = strArg(name);
        a.insert(a.end(), nm.begin(), nm.end());
        for (int i = 0; i < 6; ++i) {
            put32(a, 0); // sattr3 unset
        }
        ASSERT_EQ(get32(&rpc(fd, PROG_NFS, NFS_V3, 9, a).body[0]), 0u); // MKDIR OK
    };
    auto rmdirIn = [&](std::string const& name) {
        std::vector<uint8_t> a = fhArg(rid, rs);
        std::vector<uint8_t> nm = strArg(name);
        a.insert(a.end(), nm.begin(), nm.end());
        ASSERT_EQ(get32(&rpc(fd, PROG_NFS, NFS_V3, 13, a).body[0]), 0u); // RMDIR OK
    };

    uint32_t m0 = mtimeOf(), c0 = ctimeOf();

    mkdirIn("sub"); // creating an entry must touch the parent directory
    uint32_t m1 = mtimeOf(), c1 = ctimeOf();
    EXPECT_GT(m1, m0); // parent mtime advanced (FAILS without the fix)
    EXPECT_GT(c1, c0); // parent ctime advanced

    rmdirIn("sub"); // removing an entry must touch the parent directory too
    uint32_t m2 = mtimeOf(), c2 = ctimeOf();
    EXPECT_GT(m2, m1);
    EXPECT_GT(c2, c1);

    ::close(fd);
    loader.stopAll();
}

//  Paged READDIR must be stable under a concurrent mutation: an entry removed
//  *behind* the cursor mid-scan must not cause a surviving entry *ahead* of the
//  cursor to be skipped (or duplicated). The old ordinal-cookie scheme re-lists
//  the whole dir per page and renumbers on every change, so removing a seen entry
//  shifts ordinals and skips the next one; a name-cursor (readdirPage) resumes by
//  name and is immune. Also exercises multi-page paging over a larger directory.
TEST(NfsV3, ReaddirPagedStableUnderMutation) {
    const int port = 34577;

    auto fs = makeMemStore();
    auto log = quietLogger();
    host::Host h(*fs, *log);
    h.setOption("port", std::to_string(port));
    host::Loader loader(h);
    ASSERT_TRUE(loader.load(NFSV3_PLUGIN_SO));
    loader.startServices();

    //  /big with e0..e9 (single-digit names sort lexically e0<e1<...<e9).
    Node big = fs->mkdir();
    ASSERT_EQ(fs->link(fs->rwRoot(), "big", big), Error::OK);
    for (int i = 0; i < 10; ++i) {
        Node f = fs->mkfile("");
        ASSERT_EQ(fs->link(big, "e" + std::to_string(i), f), Error::OK);
    }

    int fd = connectLoopback(port);
    ASSERT_GE(fd, 0);
    Reply mnt = rpc(fd, PROG_MOUNT, MOUNT_V3, 1, strArg("/"));
    ASSERT_EQ(mnt.astat, 0u);
    uint64_t rid = get64(&mnt.body[8]), rs = get64(&mnt.body[16]);

    //  LOOKUP big → its filehandle (LOOKUP3resok: object fh3, then attrs).
    std::vector<uint8_t> lk = fhArg(rid, rs);
    {
        auto n = strArg("big");
        lk.insert(lk.end(), n.begin(), n.end());
    }
    Reply lr = rpc(fd, PROG_NFS, NFS_V3, 3, lk);
    ASSERT_EQ(get32(&lr.body[0]), 0u);
    ASSERT_EQ(get32(&lr.body[4]), 16u); // fh length
    uint64_t bid = get64(&lr.body[8]), bsnap = get64(&lr.body[16]);

    //  One READDIR page; appends (name,cookie) store entries (skips . / ..), and
    //  returns eof. Echoes back whatever cookie the server assigned each entry.
    auto page = [&](uint64_t cookie, std::vector<std::pair<std::string, uint64_t>>& out) -> bool {
        std::vector<uint8_t> a = fhArg(bid, bsnap);
        putU64(a, cookie);
        put32(a, 0);
        put32(a, 0);   // cookieverf[8]
        put32(a, 200); // small count → forces multiple pages
        Reply rr = rpc(fd, PROG_NFS, NFS_V3, 16, a);
        EXPECT_EQ(rr.astat, 0u);
        EXPECT_EQ(get32(&rr.body[0]), 0u);
        size_t o = 100; // status(4) + dir_attributes(88) + cookieverf(8)
        while (get32(&rr.body[o]) == 1) {
            o += 4 + 8; // value-follows + fileid
            uint32_t nl = get32(&rr.body[o]);
            o += 4;
            std::string nm(reinterpret_cast<char const*>(&rr.body[o]), nl);
            o += pad4(nl);
            uint64_t ck = get64(&rr.body[o]);
            o += 8;
            out.push_back({nm, ck});
        }
        o += 4; // value-follows == 0
        return get32(&rr.body[o]) == 1;
    };

    std::vector<std::string> seen; // store entries seen across the whole scan
    auto sawDup = [&](std::string const& n) {
        return std::find(seen.begin(), seen.end(), n) != seen.end();
    };

    uint64_t cookie = 0;
    bool eof = false, mutated = false;
    std::string cursor; // last store entry seen
    for (int guard = 0; !eof && guard < 100; ++guard) {
        std::vector<std::pair<std::string, uint64_t>> ents;
        eof = page(cookie, ents);
        for (auto& [nm, ck] : ents) {
            cookie = ck; // resume after the last entry
            if (nm == "." || nm == "..") {
                continue;
            }
            EXPECT_FALSE(sawDup(nm)) << "duplicate entry: " << nm;
            seen.push_back(nm);
            cursor = nm;
        }
        //  Once we've seen ≥2 store entries, remove one strictly behind the
        //  cursor (already returned), then keep paging.
        if (!mutated && seen.size() >= 2) {
            std::string victim; // smallest seen name != cursor ⇒ behind the cursor
            for (auto const& s : seen) {
                if (s != cursor) {
                    victim = s;
                    break;
                }
            }
            ASSERT_FALSE(victim.empty());
            std::vector<uint8_t> rm = fhArg(bid, bsnap);
            {
                auto n = strArg(victim);
                rm.insert(rm.end(), n.begin(), n.end());
            }
            ASSERT_EQ(get32(&rpc(fd, PROG_NFS, NFS_V3, 12, rm).body[0]), 0u); // REMOVE OK
            mutated = true;
        }
    }

    //  Every original entry must have appeared exactly once: the victim was seen
    //  before removal; every survivor ahead of the cursor must NOT be skipped.
    EXPECT_TRUE(eof);
    EXPECT_TRUE(mutated);
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(sawDup("e" + std::to_string(i))) << "skipped survivor: e" << i;
    }
    EXPECT_EQ(seen.size(), 10u);

    ::close(fd);
    loader.stopAll();
}

//  Browsing `.snapshot/N` must yield the node's snapshot view, with a distinct
//  fsid — otherwise the client sees it as the live node and loops (ELOOP).
TEST(NfsV3, SnapshotBrowse) {
    const int port = 34569;

    auto fs = makeMemStore();
    auto root = fs->rwRoot();
    auto before = fs->mkfile("");
    before->size(10);
    ASSERT_EQ(fs->link(root, "before.txt", before), Error::OK);
    SnapId snap = fs->snapshot("s1");
    auto after = fs->mkfile("");
    after->size(20);
    ASSERT_EQ(fs->link(root, "after.txt", after), Error::OK);

    auto log = quietLogger();
    host::Host h(*fs, *log);
    h.setOption("port", std::to_string(port));
    host::Loader loader(h);
    ASSERT_TRUE(loader.load(NFSV3_PLUGIN_SO));
    loader.startServices();
    int fd = connectLoopback(port);
    ASSERT_GE(fd, 0);

    Reply mnt = rpc(fd, PROG_MOUNT, MOUNT_V3, 1, strArg("/"));
    ASSERT_EQ(mnt.astat, 0u);
    uint64_t rid = get64(&mnt.body[8]);
    uint64_t rs = get64(&mnt.body[16]);

    auto lookup = [&](uint64_t id, uint64_t sn, std::string const& name) {
        std::vector<uint8_t> a = fhArg(id, sn);
        std::vector<uint8_t> n = strArg(name);
        a.insert(a.end(), n.begin(), n.end());
        return rpc(fd, PROG_NFS, NFS_V3, 3, a);
    };

    //  Live root reports fsid 0 and fileid == its nodeID.
    Reply grt = rpc(fd, PROG_NFS, NFS_V3, 1, fhArg(rid, rs));
    EXPECT_EQ(get64(&grt.body[4 + 44]), 0u);  // fsid
    EXPECT_EQ(get64(&grt.body[4 + 52]), rid); // fileid

    //  LOOKUP .snapshot → the synthesized snapshot directory.
    Reply lss = lookup(rid, rs, SNAPSHOT_NAME);
    ASSERT_EQ(get32(&lss.body[0]), 0u) << "LOOKUP .snapshot";
    uint64_t sdId = get64(&lss.body[8]);
    uint64_t sdSnap = get64(&lss.body[16]);

    //  LOOKUP .snapshot/<snap> → the root viewed at that snapshot. The fh must
    //  carry the snapshot, not LATEST (this is the bug).
    Reply lv = lookup(sdId, sdSnap, std::to_string(snap));
    ASSERT_EQ(get32(&lv.body[0]), 0u) << "LOOKUP .snapshot/N";
    uint64_t vId = get64(&lv.body[8]);
    uint64_t vSnap = get64(&lv.body[16]);
    EXPECT_EQ(vId, rid);    // same node (root) …
    EXPECT_EQ(vSnap, snap); // … but reading snapshot `snap`, not LATEST

    //  GETATTR the snapshot root: a DIR whose fsid is the snapshot (≠ live 0), so
    //  its (fsid, fileid) differs from the live root — no loop.
    Reply gv = rpc(fd, PROG_NFS, NFS_V3, 1, fhArg(vId, vSnap));
    EXPECT_EQ(get32(&gv.body[4]), 2u);        // NF3DIR
    EXPECT_EQ(get64(&gv.body[4 + 44]), snap); // snapshot fsid
    //  Same underlying nodeID, but a DISTINCT display fileid — so a client that
    //  collapses fsid to one st_dev still can't confuse /.snapshot/N with / (the
    //  `du` "Circular directory structure" bug).
    EXPECT_NE(get64(&gv.body[4 + 52]), rid);

    //  The snapshot view is the pre-snapshot tree: before.txt present, after.txt
    //  absent; the live tree has both.
    EXPECT_EQ(get32(&lookup(vId, vSnap, "before.txt").body[0]), 0u); // OK
    EXPECT_EQ(get32(&lookup(vId, vSnap, "after.txt").body[0]), 2u);  // NOENT
    EXPECT_EQ(get32(&lookup(rid, rs, "after.txt").body[0]), 0u);     // live has it

    ::close(fd);
    loader.stopAll();
}
