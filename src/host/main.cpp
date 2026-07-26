// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  prfs-host — loads front-end plugins and starts them over a prfs store.
//  Minimal for now: no persistent serve loop (front-ends own their threads; a
//  real one will block until signalled). Proves the wiring end to end.
//
//  Usage: prfs-host [--store PATH] [--clean] [--engine NAME] [--plugin FILE.so]...
//
#include "prfs/host.hpp"
#include "prfs/prfs.hpp"

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>

#include <string>
#include <vector>

int main(int argc, char** argv) {
    CLI::App app{"prfs synthetic-NFS host"};
    std::string store = "/tmp/prfs-host";
    std::string engine;
    bool clean = false;
    std::vector<std::string> plugins;

    app.add_option("--store", store, "store path");
    app.add_flag("--clean", clean, "wipe the store on open");
    app.add_option("--engine", engine, "storage engine (di name): lmdb | memory");
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
        prfs::host::Loader loader(host);
        for (std::string const& p : plugins) {
            loader.load(p);
        }
        loader.startFrontends();

        auto n = host.registry().resolveAll<prfs::plugin::IFrontend>().size();
        log->info("prfs-host: store={} engine={} front-ends={}", store, prfs::storageEngine(), n);

        //  A real serve loop blocks here; front-ends run on their own threads.
        loader.stopAll();
    } catch (std::exception const& e) {
        log->error("prfs-host: {}", e.what());
        return 1;
    }
    return 0;
}
