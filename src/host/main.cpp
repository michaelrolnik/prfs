// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  prfs-host — loads front-end plugins and serves them over a prfs store.
//  Services own their own threads; this process starts them, then blocks until
//  SIGINT/SIGTERM and shuts them down.
//
//  Usage:
//    prfs-host [--store PATH] [--clean] [--engine NAME] [--port N] \
//              [--plugin FILE.so]...
//
#include "prfs/host.hpp"
#include "prfs/prfs.hpp"

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <csignal>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
std::atomic<bool> g_stop{false};

void onSignal(int) { g_stop = true; }
} // namespace

int main(int argc, char** argv) {
    CLI::App app{"prfs synthetic-NFS host"};
    std::string store = "/tmp/prfs-host";
    std::string engine;
    bool clean = false;
    int port = 0;
    std::vector<std::string> plugins;

    app.add_option("--store", store, "store path");
    app.add_flag("--clean", clean, "wipe the store on open");
    app.add_option("--engine", engine, "storage engine (di name): lmdb | memory");
    app.add_option("--port", port, "TCP port for NFS/MOUNT services (default 2049)");
    app.add_option("--plugin", plugins, "front-end plugin .so to load")->expected(-1);
    CLI11_PARSE(app, argc, argv);

    auto log = spdlog::default_logger();

    try {
        if (!engine.empty()) {
            prfs::setStorageEngine(engine);
        }
        prfs::Options opts;
        opts.clean = clean;
        auto fs = prfs::openPrfs(store, opts);

        prfs::host::Host host(*fs, *log);
        if (port != 0) {
            host.setOption("port", std::to_string(port));
        }
        prfs::host::Loader loader(host);
        for (std::string const& p : plugins) {
            loader.load(p);
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
