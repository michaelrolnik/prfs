// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  nfsv4 plugin — end-to-end RPC test. Loads nfsv4.so via the host, then drives a
//  single COMPOUND over TCP: PUTROOTFH → LOOKUP → GETATTR → READ, and checks the
//  filehandle flow, fattr4 attribute encoding, and the read path. NFSv4 has no
//  MOUNT program — PUTROOTFH obtains the root. NFSV4_PLUGIN_SO is the .so path.
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

constexpr uint32_t PROG_NFS = 100003, NFS_V4 = 4;

std::shared_ptr<spdlog::logger> quietLogger() {
    return std::make_shared<spdlog::logger>("nfsv4-test",
                                            std::make_shared<spdlog::sinks::null_sink_mt>());
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24));
    v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}

void putU64(std::vector<uint8_t>& v, uint64_t x) {
    put32(v, uint32_t(x >> 32));
    put32(v, uint32_t(x));
}

uint32_t get32(uint8_t const* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

uint64_t get64(uint8_t const* p) { return uint64_t(get32(p)) << 32 | get32(p + 4); }

size_t pad4(size_t n) { return (n + 3) & ~size_t(3); }

void putStr(std::vector<uint8_t>& v, std::string const& s) {
    put32(v, uint32_t(s.size()));
    v.insert(v.end(), s.begin(), s.end());
    while (v.size() % 4) {
        v.push_back(0);
    }
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
    std::vector<uint8_t> body;
};

Reply rpc(int fd, uint32_t proc, std::vector<uint8_t> const& args) {
    std::vector<uint8_t> call;
    put32(call, 0x0a0b0c0d); // xid
    put32(call, 0);          // CALL
    put32(call, 2);          // rpcvers
    put32(call, PROG_NFS);
    put32(call, NFS_V4);
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

    uint32_t verfLen = get32(&rep[16]);
    size_t off = 20 + pad4(verfLen);
    uint32_t astat = get32(&rep[off]);
    off += 4;
    return {astat, std::vector<uint8_t>(rep.begin() + off, rep.end())};
}

} // namespace

TEST(NfsV4, CompoundBrowseRead) {
    const int port = 34580;

    auto fs = makeMemStore();
    auto root = fs->rwRoot();
    auto file = fs->mkfile("hi there");
    file->size(8);
    ASSERT_EQ(fs->link(root, "hello", file), Error::OK);

    auto log = quietLogger();
    host::Host h(*fs, *log);
    h.setOption("port", std::to_string(port));
    host::Loader loader(h);
    ASSERT_TRUE(loader.load(NFSV4_PLUGIN_SO));
    loader.startServices();

    int fd = connectLoopback(port);
    ASSERT_GE(fd, 0);

    //  NULL — transport sanity.
    EXPECT_EQ(rpc(fd, 0, {}).astat, 0u);

    //  COMPOUND { PUTROOTFH, LOOKUP "hello", GETATTR {TYPE,SIZE}, READ }.
    std::vector<uint8_t> c;
    put32(c, 0);  // tag ""
    put32(c, 0);  // minorversion
    put32(c, 4);  // numops
    put32(c, 24); // OP_PUTROOTFH
    put32(c, 15); // OP_LOOKUP
    putStr(c, "hello");
    put32(c, 9);                     // OP_GETATTR
    put32(c, 1);                     // bitmap: one word
    put32(c, (1u << 1) | (1u << 4)); // FATTR4_TYPE | FATTR4_SIZE
    put32(c, 25);                    // OP_READ
    for (int i = 0; i < 4; ++i) {
        put32(c, 0); // stateid (16 bytes)
    }
    putU64(c, 0);  // offset
    put32(c, 100); // count

    Reply rp = rpc(fd, 1, c);
    ASSERT_EQ(rp.astat, 0u);

    size_t o = 0;
    EXPECT_EQ(get32(&rp.body[o]), 0u); // COMPOUND status
    o += 4;
    uint32_t taglen = get32(&rp.body[o]);
    o += 4 + pad4(taglen);
    EXPECT_EQ(get32(&rp.body[o]), 4u); // numres
    o += 4;

    //  PUTROOTFH result: opnum, status.
    EXPECT_EQ(get32(&rp.body[o]), 24u);
    o += 4;
    EXPECT_EQ(get32(&rp.body[o]), 0u);
    o += 4;
    //  LOOKUP result.
    EXPECT_EQ(get32(&rp.body[o]), 15u);
    o += 4;
    EXPECT_EQ(get32(&rp.body[o]), 0u);
    o += 4;
    //  GETATTR result: opnum, status, fattr4 { bitmap4, attrlist<> }.
    EXPECT_EQ(get32(&rp.body[o]), 9u);
    o += 4;
    EXPECT_EQ(get32(&rp.body[o]), 0u);
    o += 4;
    uint32_t bcnt = get32(&rp.body[o]);
    o += 4 + bcnt * 4; // skip returned bitmap words
    uint32_t alen = get32(&rp.body[o]);
    o += 4;
    //  attrs in increasing bit order: TYPE (u32), then SIZE (u64).
    EXPECT_EQ(get32(&rp.body[o]), 1u);     // NF4REG
    EXPECT_EQ(get64(&rp.body[o + 4]), 8u); // size
    o += pad4(alen);
    //  READ result: opnum, status, eof, data<>.
    EXPECT_EQ(get32(&rp.body[o]), 25u);
    o += 4;
    EXPECT_EQ(get32(&rp.body[o]), 0u);
    o += 4;
    EXPECT_EQ(get32(&rp.body[o]), 1u); // eof (whole 8-byte file read)
    o += 4;
    EXPECT_EQ(get32(&rp.body[o]), 8u); // data length

    ::close(fd);
    loader.stopAll();
}

TEST(NfsV4, WriteSurface) {
    const int port = 34581;

    auto fs = makeMemStore();
    auto root = fs->rwRoot();

    auto log = quietLogger();
    host::Host h(*fs, *log);
    h.setOption("port", std::to_string(port));
    host::Loader loader(h);
    ASSERT_TRUE(loader.load(NFSV4_PLUGIN_SO));
    loader.startServices();

    int fd = connectLoopback(port);
    ASSERT_GE(fd, 0);

    auto compound = [&](std::vector<uint8_t> ops, uint32_t nops) {
        std::vector<uint8_t> c;
        put32(c, 0); // tag ""
        put32(c, 0); // minorversion
        put32(c, nops);
        c.insert(c.end(), ops.begin(), ops.end());
        return rpc(fd, 1, c);
    };
    auto stateid = [](std::vector<uint8_t>& v) {
        for (int i = 0; i < 4; ++i) {
            put32(v, 0);
        }
    };

    //  COMPOUND { PUTROOTFH, OPEN(create "new.txt" mode 0644), WRITE "hello", CLOSE }.
    {
        std::vector<uint8_t> ops;
        put32(ops, 24); // PUTROOTFH
        put32(ops, 18); // OPEN
        put32(ops, 0);  // seqid
        put32(ops, 2);  // share_access WRITE
        put32(ops, 0);  // share_deny NONE
        putU64(ops, 0); // owner.clientid
        putStr(ops, "ownr");
        put32(ops, 1); // opentype OPEN4_CREATE
        put32(ops, 0); // createmode UNCHECKED4
        put32(ops, 2); // createattrs bitmap: two words
        put32(ops, 0);
        put32(ops, (1u << (33 - 32))); // FATTR4_MODE
        put32(ops, 4);                 // attrlist length
        put32(ops, 0644);              // mode
        put32(ops, 0);                 // claim CLAIM_NULL
        putStr(ops, "new.txt");
        put32(ops, 38); // WRITE
        stateid(ops);
        putU64(ops, 0); // offset
        put32(ops, 2);  // stable FILE_SYNC4
        putStr(ops, "hello");
        put32(ops, 4); // CLOSE
        put32(ops, 0); // seqid
        stateid(ops);
        Reply r = compound(ops, 4);
        ASSERT_EQ(r.astat, 0u);
        EXPECT_EQ(get32(&r.body[0]), 0u); // COMPOUND status OK
    }
    {
        Node f = fs->lookup(root, "new.txt");
        ASSERT_TRUE(f);
        EXPECT_EQ(f->type(), Type::REG);
        EXPECT_EQ(f->size(), 5u); // "hello" grew the size to 5
    }

    //  COMPOUND { PUTROOTFH, CREATE(NF4DIR "mydir") }.
    {
        std::vector<uint8_t> ops;
        put32(ops, 24); // PUTROOTFH
        put32(ops, 6);  // CREATE
        put32(ops, 2);  // createtype NF4DIR (no typedata)
        putStr(ops, "mydir");
        put32(ops, 0); // createattrs: empty bitmap
        put32(ops, 0); // attrlist length 0
        Reply r = compound(ops, 2);
        ASSERT_EQ(r.astat, 0u);
        EXPECT_EQ(get32(&r.body[0]), 0u);
    }
    {
        Node d = fs->lookup(root, "mydir");
        ASSERT_TRUE(d);
        EXPECT_EQ(d->type(), Type::DIR);
    }

    //  COMPOUND { PUTROOTFH, LOOKUP "new.txt", SETATTR mode 0600 }.
    {
        std::vector<uint8_t> ops;
        put32(ops, 24); // PUTROOTFH
        put32(ops, 15); // LOOKUP
        putStr(ops, "new.txt");
        put32(ops, 34); // SETATTR
        stateid(ops);
        put32(ops, 2); // bitmap two words
        put32(ops, 0);
        put32(ops, (1u << (33 - 32))); // MODE
        put32(ops, 4);                 // attrlist length
        put32(ops, 0600);
        Reply r = compound(ops, 3);
        ASSERT_EQ(r.astat, 0u);
        EXPECT_EQ(get32(&r.body[0]), 0u);
    }
    EXPECT_EQ(fs->lookup(root, "new.txt")->mode() & 0777, 0600u);

    //  COMPOUND { PUTROOTFH, REMOVE "new.txt" }.
    {
        std::vector<uint8_t> ops;
        put32(ops, 24); // PUTROOTFH
        put32(ops, 28); // REMOVE
        putStr(ops, "new.txt");
        Reply r = compound(ops, 2);
        ASSERT_EQ(r.astat, 0u);
        EXPECT_EQ(get32(&r.body[0]), 0u);
    }
    EXPECT_FALSE(fs->lookup(root, "new.txt"));

    ::close(fd);
    loader.stopAll();
}
