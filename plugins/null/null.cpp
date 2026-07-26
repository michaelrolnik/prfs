// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  null — the reference/test front-end plugin. It provides an IFrontend named
//  "null" that, on start(), performs one scripted store operation (no network),
//  and withdraws itself on destroy. Proves the whole host↔plugin path: dlopen,
//  ABI check, create → provide, resolveAll → start/stop, destroy → withdraw.
//
#include "prfs/plugin.hpp"

namespace {
using namespace prfs;
using namespace prfs::plugin;

struct NullFrontend : IFrontend {
    IHost& host;
    bool running = false;

    explicit NullFrontend(IHost& h)
        : host(h) {}

    std::vector<Option> options() const override {
        return {{"note", "a note this front-end records", "hi", false}};
    }

    Error start() override {
        running = true;
        host.fs().mkdir(); // a harmless scripted op to prove fs() works
        return Error::OK;
    }

    void stop() override { running = false; }
};

struct NullPlugin : IPlugin {
    IHost& host;
    NullFrontend frontend;

    explicit NullPlugin(IHost& h)
        : host(h)
        , frontend(h) {
        host.registry().provide<IFrontend>(&frontend, "null");
    }

    ~NullPlugin() override { host.registry().withdraw<IFrontend>("null"); }

    char const* name() const override { return "null"; }

    char const* version() const override { return "0.1"; }
};

} // namespace

extern "C" uint32_t prfs_abi(void) { return prfs::plugin::ABI; }

extern "C" prfs::plugin::IPlugin* prfs_plugin_create(prfs::plugin::IHost& host) {
    return new NullPlugin(host);
}

extern "C" void prfs_plugin_destroy(prfs::plugin::IPlugin* p) { delete p; }
