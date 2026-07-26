// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  nfsv3 — an NFSv3 front-end as a prfs service plugin (todo L2). This first
//  slice stands up the transport: a TCP server (its own thread) that speaks
//  ONC-RPC record marking and answers RPC NULL (proc 0) with an accepted-success
//  reply, and PROC_UNAVAIL for everything else. The real procedures (GETATTR,
//  LOOKUP, READ, READDIR, FSSTAT/FSINFO, and the MOUNT program) build on this,
//  using IPrfs + nodeById() for filehandles and fsStat/fsInfo (§9).
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
#include <thread>
#include <vector>

namespace {
using namespace prfs;
using namespace prfs::plugin;

constexpr uint32_t RPC_CALL = 0, RPC_REPLY = 1;
constexpr uint32_t MSG_ACCEPTED = 0;
constexpr uint32_t SUCCESS = 0, PROC_UNAVAIL = 3;

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

void wr32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24));
    v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
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

    //  Serve one connection: read record-marked RPC messages, answer NULL.
    void handle(int c) {
        for (;;) {
            uint8_t mark[4];
            if (!recvAll(c, mark, 4)) {
                return;
            }
            uint32_t len = rd32(mark) & 0x7fffffff; // ignore fragmenting for now
            if (len < 24 || len > (1u << 20)) {
                return;
            }
            std::vector<uint8_t> msg(len);
            if (!recvAll(c, msg.data(), len)) {
                return;
            }

            uint32_t xid = rd32(&msg[0]);
            if (rd32(&msg[4]) != RPC_CALL) {
                return;
            }
            uint32_t prog = rd32(&msg[12]);
            uint32_t proc = rd32(&msg[20]);
            uint32_t astat = proc == 0 ? SUCCESS : PROC_UNAVAIL;
            if (proc != 0) {
                m_host.log().info("nfsv3: unimplemented prog={} proc={}", prog, proc);
            }

            std::vector<uint8_t> rep;
            wr32(rep, xid);
            wr32(rep, RPC_REPLY);
            wr32(rep, MSG_ACCEPTED);
            wr32(rep, 0); // verf flavor AUTH_NONE
            wr32(rep, 0); // verf length
            wr32(rep, astat);
            // (NULL result is empty; real procs append their XDR result here)

            std::vector<uint8_t> frame;
            wr32(frame, 0x80000000u | uint32_t(rep.size())); // last fragment
            frame.insert(frame.end(), rep.begin(), rep.end());
            if (!sendAll(c, frame.data(), frame.size())) {
                return;
            }
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
