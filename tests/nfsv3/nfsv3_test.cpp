// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  nfsv3 plugin — end-to-end RPC test. Loads nfsv3.so via the host, then over a
//  TCP connection walks the real client flow: MOUNT MNT to obtain the root
//  filehandle, then NFS NULL/GETATTR/LOOKUP/ACCESS/READ against the live store
//  the host wraps. Proves the transport (record marking + call/reply) and the
//  XDR / filehandle mapping. NFSV3_PLUGIN_SO is the .so path.
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

    di::Registry reg;
    auto fs = makeMemStore();
    auto root = fs->rwRoot();
    auto file = fs->mkfile("hi there");
    file->size(8);
    ASSERT_EQ(fs->link(root, "hello", file), Error::OK);
    uint64_t rootId = root->id();
    uint64_t fileId = file->id();

    auto log = quietLogger();
    host::Host h(*fs, *log, reg);
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
    EXPECT_EQ(std::string(reinterpret_cast<char const*>(&rd.body[104]), 8), "hi there");

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

    //  ACCESS the root asking for READ|LOOKUP (0x03) → both granted.
    std::vector<uint8_t> acArgs = fhArg(fhId, fhSnap);
    put32(acArgs, 0x03);
    Reply ac = rpc(fd, PROG_NFS, NFS_V3, 4, acArgs);
    ASSERT_EQ(ac.astat, 0u);
    EXPECT_EQ(get32(&ac.body[0]), 0u);     // NFS3_OK
    EXPECT_EQ(get32(&ac.body[92]), 0x03u); // granted after status + post_op_attr

    //  GETATTR a bogus fh → NFS3ERR_STALE (70).
    EXPECT_EQ(get32(&rpc(fd, PROG_NFS, NFS_V3, 1, fhArg(999999, LAT)).body[0]), 70u);

    ::close(fd);
    loader.stopAll();
}
