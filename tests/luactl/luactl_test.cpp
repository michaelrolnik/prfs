// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  luactl plugin — Lua console smoke test. Loads luactl.so via the host, connects
//  to its unix socket, and drives the REPL: an expression echoes its value, and a
//  mutation through `fs` (the live store) is visible both to a follow-up console
//  query and directly on the store the test holds. LUACTL_PLUGIN_SO is the .so.
//
#include "prfs/host.hpp"
#include "prfs/memstore.hpp"

#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <memory>
#include <string>

using namespace prfs;

namespace {

std::shared_ptr<spdlog::logger> quietLogger() {
    return std::make_shared<spdlog::logger>("luactl-test",
                                            std::make_shared<spdlog::sinks::null_sink_mt>());
}

int connectUnix(std::string const& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, path.c_str(), sizeof(a.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

//  Read one '\n'-terminated line (without the newline).
std::string recvLine(int fd) {
    std::string s;
    char c;
    while (::recv(fd, &c, 1, 0) == 1) {
        if (c == '\n') {
            break;
        }
        s.push_back(c);
    }
    return s;
}

void sendLine(int fd, std::string s) {
    s.push_back('\n');
    ASSERT_EQ(::send(fd, s.data(), s.size(), 0), ssize_t(s.size()));
}

} // namespace

TEST(LuaCtl, ConsoleEvalAndMutate) {
    std::string sock = "/tmp/prfs-luactl-test.sock";
    ::unlink(sock.c_str());

    auto fs = makeMemStore();
    auto log = quietLogger();
    host::Host h(*fs, *log); // di::global()
    h.setOption("control", sock);

    host::Loader loader(h);
    ASSERT_TRUE(loader.load(LUACTL_PLUGIN_SO));
    loader.startServices();

    int fd = connectUnix(sock);
    ASSERT_GE(fd, 0) << "could not connect to the luactl socket";

    recvLine(fd); // banner

    //  A bare expression echoes its value (the `return <line>` path).
    sendLine(fd, "6 * 7");
    EXPECT_EQ(recvLine(fd), "42");

    //  A mutation through the live store: link() echoes its Error return (0 = OK).
    sendLine(fd, "fs:link(fs:root(), 'hi', fs:mkdir())");
    EXPECT_EQ(recvLine(fd), "0"); // Error.OK

    //  The link is visible to a follow-up console query (sequential eval) …
    sendLine(fd, "fs:lookup(fs:root(), 'hi') ~= nil");
    EXPECT_EQ(recvLine(fd), "true");

    //  … and on the same store the test holds.
    EXPECT_TRUE(fs->lookup(fs->rwRoot(), "hi"));

    //  A SECOND console connects while the first is still open — thread-per-
    //  connection, so it gets its banner without the first disconnecting (this
    //  would block if sessions were serialized). It sees the first's mutation …
    int fd2 = connectUnix(sock);
    ASSERT_GE(fd2, 0);
    recvLine(fd2); // banner
    sendLine(fd2, "fs:lookup(fs:root(), 'hi') ~= nil");
    EXPECT_EQ(recvLine(fd2), "true");

    //  … and a mutation on the second is visible on the first (one store).
    sendLine(fd2, "fs:link(fs:root(), 'hi2', fs:mkdir())");
    EXPECT_EQ(recvLine(fd2), "0"); // Error.OK
    sendLine(fd, "fs:lookup(fs:root(), 'hi2') ~= nil");
    EXPECT_EQ(recvLine(fd), "true");

    ::close(fd2);
    ::close(fd);
    loader.stopAll();
    ::unlink(sock.c_str());
}
