// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

#pragma once
//
//  prfs plugin API (docs/plugins.md). prfs is a host for extensions: a plugin
//  *provides* implementations of interfaces into the shared di registry, and the
//  host *resolves* them (starts front-ends, uses the selected engine/rng). This
//  header adds the plugin-specific interfaces; the providable interfaces
//  IStorageEngine (kvstore.hpp) and IRng (rng.hpp) are defined with their
//  modules. Boundary: C++ interfaces reached through an extern "C" factory.
//
#include "prfs/di.hpp"
#include "prfs/prfs.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace spdlog {
class logger;
}

namespace prfs::plugin {

//  Bump on ANY change to the interfaces below; the loader refuses a mismatch.
inline constexpr uint32_t ABI = 1;

//  Services the host lends every plugin, plus the shared di registry.
class IHost {
public:
    virtual ~IHost() = default;

    virtual di::Registry& registry() = 0;                                    // provide/resolve
    virtual IPrfs& fs() = 0;                                                 // the filesystem
    virtual size_t read(Node file, uint64_t off, char* out, size_t len) = 0; // file bytes
    virtual spdlog::logger& log() = 0;                                       // shared logger
    virtual std::string option(std::string_view key) const = 0;              // parsed CLI/config
};

//  A loaded plugin's owning root: identity + the objects it provided. create()
//  registered them into host.registry(); destroy() withdraws + frees.
class IPlugin {
public:
    virtual ~IPlugin() = default;

    virtual char const* name() const = 0; // "nfsv3"
    virtual char const* version() const = 0;
};

//  A CLI option a front-end contributes (kept CLI-library-agnostic).
struct Option {
    std::string name, help, def;
    bool flag = false;
};

//  A protocol front-end. The di name is the protocol, e.g. "nfsv3" / "mount".
struct IFrontend {
    static constexpr std::string_view ID = "prfs.frontend/1";

    virtual ~IFrontend() = default;

    virtual std::vector<Option> options() const { return {}; } // CLI args it adds

    virtual Error start() = 0; // bind/listen; owns its threads
    virtual void stop() = 0;
};

} // namespace prfs::plugin

//  Every plugin .so exports these with C linkage (unmangled → dlsym-able).
extern "C" {
uint32_t prfs_abi(void); // must equal prfs::plugin::ABI
prfs::plugin::IPlugin* prfs_plugin_create(prfs::plugin::IHost&);
void prfs_plugin_destroy(prfs::plugin::IPlugin*);
}
