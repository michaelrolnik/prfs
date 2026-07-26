// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  luactl — a live Lua control console as a prfs service plugin. It listens on a
//  unix-domain socket and gives each client a Lua REPL whose global `fs` is the
//  HOST's live store (IHost::fs()) — so you can seed a namespace, snapshot, set
//  a ContentConfig, or inspect stats against the running server. Reuses the
//  existing sol2 bindings (prfs/lua.hpp, registerLua). Connect with socat:
//
//      socat READLINE UNIX-CONNECT:/tmp/prfs.sock          # interactive
//      echo 'print(fs:stats().links)' | socat - UNIX-CONNECT:/tmp/prfs.sock
//
//  Each evaluated line runs under the host's EXCLUSIVE store lock (the console
//  may mutate), so console and NFS access are serialized. Connections are served
//  one at a time — this is an admin channel, not a throughput path. The socket
//  path comes from the "control" option (default /tmp/prfs.sock).
//
#include "prfs/lua.hpp"
#include "prfs/plugin.hpp"

#include <sol/sol.hpp>
#include <spdlog/spdlog.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>

namespace {
using namespace prfs;
using namespace prfs::plugin;

constexpr char const* DEFAULT_SOCK = "/tmp/prfs.sock";

void sendStr(int fd, std::string const& s) {
    size_t n = 0;
    while (n < s.size()) {
        ssize_t k = ::send(fd, s.data() + n, s.size() - n, MSG_NOSIGNAL);
        if (k <= 0) {
            return;
        }
        n += size_t(k);
    }
}

class LuaCtl : public IService {
public:
    explicit LuaCtl(IHost& host)
        : m_host(host) {}

    ~LuaCtl() override { stop(); }

    std::vector<Option> options() const override {
        return {{"control", "unix socket path for the Lua console", DEFAULT_SOCK, false}};
    }

    Error start() override {
        m_path = m_host.option("control");
        if (m_path.empty()) {
            m_path = DEFAULT_SOCK;
        }

        m_listen = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (m_listen < 0) {
            return Error::INVAL;
        }
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (m_path.size() >= sizeof(addr.sun_path)) {
            m_host.log().error("luactl: socket path too long: {}", m_path);
            ::close(m_listen);
            m_listen = -1;
            return Error::INVAL;
        }
        std::strncpy(addr.sun_path, m_path.c_str(), sizeof(addr.sun_path) - 1);
        ::unlink(m_path.c_str()); // clear a stale socket
        if (::bind(m_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
            ::listen(m_listen, 4) < 0) {
            m_host.log().error("luactl: bind/listen on {} failed", m_path);
            ::close(m_listen);
            m_listen = -1;
            return Error::INVAL;
        }

        m_running = true;
        m_thread = std::thread([this] { serve(); });
        m_host.log().info("luactl: console on {} (socat READLINE UNIX-CONNECT:{})", m_path, m_path);
        return Error::OK;
    }

    void stop() override {
        if (!m_running.exchange(false)) {
            return;
        }
        if (m_listen >= 0) {
            ::shutdown(m_listen, SHUT_RDWR);
            ::close(m_listen);
            m_listen = -1;
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
        if (!m_path.empty()) {
            ::unlink(m_path.c_str());
        }
    }

private:
    void serve() {
        while (m_running) {
            int c = ::accept(m_listen, nullptr, nullptr);
            if (c < 0) {
                break; // listen fd closed by stop()
            }
            session(c);
            ::close(c);
        }
    }

    //  One REPL session: a fresh Lua state with `fs` bound to the live store and
    //  `print` redirected to this socket.
    void session(int c) {
        sol::state lua;
        lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table, sol::lib::math,
                           sol::lib::os);
        registerLua(lua);
        lua["fs"] = &m_host.fs(); // the running server's store

        //  print(...) → this connection, tab-separated (Lua tostring per value).
        lua.set_function("print", [c](sol::variadic_args va, sol::this_state ts) {
            sol::state_view v(ts);
            std::string out;
            bool first = true;
            for (auto arg : va) {
                if (!first) {
                    out += '\t';
                }
                first = false;
                out += v["tostring"](arg).template get<std::string>();
            }
            out += '\n';
            sendStr(c, out);
        });

        sendStr(c, "prfs luactl — `fs` is the live store. print(...) to see values. Ctrl-D to "
                   "exit.\n");

        std::string buf;
        char tmp[4096];
        for (;;) {
            ssize_t n = ::recv(c, tmp, sizeof(tmp), 0);
            if (n <= 0) {
                return;
            }
            buf.append(tmp, size_t(n));
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (!line.empty()) {
                    eval(lua, c, line);
                }
            }
        }
    }

    //  Evaluate one line under the EXCLUSIVE store lock. Tries `return <line>`
    //  first (so a bare expression prints its value), then the line as a chunk.
    void eval(sol::state& lua, int c, std::string const& line) {
        std::unique_lock<std::shared_mutex> lk(m_host.storeMutex());

        sol::protected_function_result res =
            lua.safe_script("return " + line, sol::script_pass_on_error);
        if (!res.valid()) {
            res = lua.safe_script(line, sol::script_pass_on_error);
        }
        if (!res.valid()) {
            sol::error e = res;
            sendStr(c, std::string("! ") + e.what() + "\n");
            return;
        }
        //  Echo any returned values (the `return <line>` case).
        int count = res.return_count();
        std::string out;
        for (int i = 0; i < count; ++i) {
            sol::object o = res.get<sol::object>(i);
            out += lua["tostring"](o).get<std::string>();
            out += (i + 1 < count) ? "\t" : "\n";
        }
        sendStr(c, out);
    }

    IHost& m_host;
    std::string m_path;
    int m_listen = -1;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
};

class LuaCtlPlugin : public IPlugin {
public:
    explicit LuaCtlPlugin(IHost& host)
        : m_host(host)
        , m_svc(host) {
        m_host.registry().provide<IService>(&m_svc, "luactl");
    }

    ~LuaCtlPlugin() override { m_host.registry().withdraw<IService>("luactl"); }

    char const* name() const override { return "luactl"; }

    char const* version() const override { return "0.1"; }

private:
    IHost& m_host;
    LuaCtl m_svc;
};

} // namespace

extern "C" uint32_t prfs_abi(void) { return prfs::plugin::ABI; }

extern "C" prfs::plugin::IPlugin* prfs_plugin_create(prfs::plugin::IHost& host) {
    return new LuaCtlPlugin(host);
}

extern "C" void prfs_plugin_destroy(prfs::plugin::IPlugin* p) { delete p; }
