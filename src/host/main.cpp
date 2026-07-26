// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  prfs-host — loads front-end plugins and serves them over a prfs store.
//  Services own their own threads; this process starts them, then blocks until
//  SIGINT/SIGTERM and shuts them down.
//
//  Usage:
//    prfs-host [--store PATH] [--clean] [--engine NAME] [--port N] [--time SECS] \
//              [--control PATH] [--plugin FILE.so]... [--plugin-dir DIR]...
//
//  Plugin discovery: --plugin loads a specific .so; --plugin-dir loads every
//  *.so in a directory. If neither is given, prfs-host scans its own directory
//  (so `./build/prfs-host` finds `./build/*.so`).
//
//  --time seeds the store's logical clock (epoch seconds). New nodes stamp their
//  atime/mtime/ctime with it, so `ls -l` shows real dates instead of 1970. The
//  clock stays deterministic — it does not advance on its own; pass e.g.
//  `--time $(date +%s)` to anchor it to now. --time-advance bumps the clock once
//  per NFS mutation, so successive changes get distinct, increasing mtimes
//  (useful for tools that compare mtimes) — still deterministic per op sequence.
//
#include "prfs/host.hpp"
#include "prfs/prfs.hpp"

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

namespace stdfs = std::filesystem;

namespace {
std::atomic<bool> g_stop{false};

void onSignal(int) { g_stop = true; }

//  Directory of the running executable (for the default plugin scan).
std::string exeDir() {
    char buf[4096];
    ssize_t k = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (k <= 0) {
        return ".";
    }
    buf[k] = '\0';
    return stdfs::path(buf).parent_path().string();
}

//  Load every *.so in `dir` as a plugin (sorted for a deterministic order),
//  skipping any whose filename is in `skip`.
void loadDir(prfs::host::Loader& loader, spdlog::logger& log, std::string const& dir,
             std::vector<std::string> const& skip = {}) {
    std::error_code ec;
    std::vector<std::string> sos;
    for (auto const& e : stdfs::directory_iterator(dir, ec)) {
        if (e.is_regular_file() && e.path().extension() == ".so" &&
            std::find(skip.begin(), skip.end(), e.path().filename().string()) == skip.end()) {
            sos.push_back(e.path().string());
        }
    }
    if (ec) {
        log.warn("prfs-host: cannot scan plugin dir {}: {}", dir, ec.message());
        return;
    }
    std::sort(sos.begin(), sos.end());
    for (std::string const& so : sos) {
        loader.load(so);
    }
}
} // namespace

int main(int argc, char** argv) {
    CLI::App app{"prfs synthetic-NFS host"};
    std::string store = "/tmp/prfs-host";
    std::string engine;
    bool clean = false;
    int port = 0;
    int64_t clockSecs = -1;
    bool timeAdvance = false;
    std::string control;
    std::vector<std::string> plugins;
    std::vector<std::string> pluginDirs;
    std::vector<std::string> sets;

    app.add_option("--store", store, "store path");
    app.add_flag("--clean", clean, "wipe the store on open");
    app.add_option("--engine", engine, "storage engine (di name): lmdb | memory");
    app.add_option("--port", port, "TCP port for NFS/MOUNT services (default 2049)");
    app.add_option("--time", clockSecs, "seed the logical clock (epoch seconds); new nodes use it");
    app.add_flag("--time-advance", timeAdvance,
                 "advance the clock per NFS mutation (distinct mtimes)");
    app.add_option("--control", control, "unix socket path for the luactl Lua console");
    app.add_option("--plugin", plugins, "front-end plugin .so to load")->expected(-1);
    app.add_option("--plugin-dir", pluginDirs, "directory to scan for *.so plugins")->expected(-1);
    app.add_option("--set", sets,
                   "plugin option KEY=VALUE (repeatable), e.g. --set bigtree.total=1T")
        ->expected(-1);
    CLI11_PARSE(app, argc, argv);

    auto log = spdlog::default_logger();

    try {
        if (!engine.empty()) {
            prfs::setStorageEngine(engine);
        }
        prfs::Options opts;
        opts.clean = clean;
        auto fs = prfs::openPrfs(store, opts);

        if (clockSecs >= 0) {
            fs->setTime(uint64_t(clockSecs)); // seed the deterministic logical clock
        }

        prfs::host::Host host(*fs, *log);
        if (port != 0) {
            host.setOption("port", std::to_string(port));
        }
        if (timeAdvance) {
            host.setOption("time-advance", "1");
        }
        if (!control.empty()) {
            host.setOption("control", control);
        }
        //  Generic pass-through so any plugin option is settable from the CLI.
        for (std::string const& kv : sets) {
            auto eq = kv.find('=');
            if (eq == std::string::npos) {
                log->warn("prfs-host: ignoring --set '{}' (expected KEY=VALUE)", kv);
                continue;
            }
            host.setOption(kv.substr(0, eq), kv.substr(eq + 1));
        }
        prfs::host::Loader loader(host);
        for (std::string const& p : plugins) {
            loader.load(p);
        }
        for (std::string const& d : pluginDirs) {
            loadDir(loader, *log, d);
        }
        //  Nothing named explicitly → discover plugins next to the executable,
        //  minus null.so (the reference/test service, not a real front-end).
        if (plugins.empty() && pluginDirs.empty()) {
            loadDir(loader, *log, exeDir(), {"null.so"});
        }
        loader.startServices();

        auto n = host.registry().resolveAll<prfs::plugin::IService>().size();
        log->info("prfs-host: store={} engine={} services={}", store, prfs::storageEngine(), n);

        if (n == 0) {
            log->warn("prfs-host: no services loaded — nothing to serve");
        } else {
            //  Services run on their own threads; block here until signalled.
            std::signal(SIGINT, onSignal);
            std::signal(SIGTERM, onSignal);
            log->info("prfs-host: serving — Ctrl-C to stop");
            while (!g_stop) {
                pause();
            }
            log->info("prfs-host: shutting down");
        }
        loader.stopAll();
    } catch (std::exception const& e) {
        log->error("prfs-host: {}", e.what());
        return 1;
    }
    return 0;
}
