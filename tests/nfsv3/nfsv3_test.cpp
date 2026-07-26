// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  nfsv3 plugin — RPC transport smoke test. Loads nfsv3.so via the host, starts
//  it on a test port, and sends an ONC-RPC NULL call over TCP, asserting an
//  accepted-success reply with the xid echoed. Proves the service plugin serves
//  RPC end to end (record marking + call/reply). NFSV3_PLUGIN_SO is the .so path.
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

} // namespace

TEST(NfsV3, RpcNullRoundTrip) {
    const int port = 34567;
    di::Registry reg;
    auto fs = makeMemStore();
    auto log = quietLogger();
    host::Host h(*fs, *log, reg);
    h.setOption("port", std::to_string(port));

    host::Loader loader(h);
    ASSERT_TRUE(loader.load(NFSV3_PLUGIN_SO));
    loader.startServices();

    int fd = connectLoopback(port);
    ASSERT_GE(fd, 0) << "could not connect to the nfsv3 service";

    //  RPC NULL: xid, CALL, rpcvers=2, prog=NFS(100003), vers=3, proc=0,
    //  cred(AUTH_NONE,0), verf(AUTH_NONE,0).
    std::vector<uint8_t> call;
    put32(call, 0x12345678);
    put32(call, 0);      // CALL
    put32(call, 2);      // rpcvers
    put32(call, 100003); // NFS
    put32(call, 3);      // vers 3
    put32(call, 0);      // proc NULL
    put32(call, 0);
    put32(call, 0); // cred
    put32(call, 0);
    put32(call, 0); // verf

    std::vector<uint8_t> frame;
    put32(frame, 0x80000000u | uint32_t(call.size()));
    frame.insert(frame.end(), call.begin(), call.end());
    ASSERT_EQ(::send(fd, frame.data(), frame.size(), 0), ssize_t(frame.size()));

    uint8_t mark[4];
    ASSERT_EQ(::recv(fd, mark, 4, MSG_WAITALL), 4);
    uint32_t rlen = get32(mark) & 0x7fffffff;
    ASSERT_GE(rlen, 24u);
    std::vector<uint8_t> rep(rlen);
    ASSERT_EQ(::recv(fd, rep.data(), rlen, MSG_WAITALL), ssize_t(rlen));

    EXPECT_EQ(get32(&rep[0]), 0x12345678u); // xid echoed
    EXPECT_EQ(get32(&rep[4]), 1u);          // REPLY
    EXPECT_EQ(get32(&rep[8]), 0u);          // MSG_ACCEPTED
    EXPECT_EQ(get32(&rep[20]), 0u);         // accept_stat SUCCESS

    ::close(fd);
    loader.stopAll();
}
