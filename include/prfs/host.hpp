// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

#pragma once
//
//  prfs host — the concrete IHost and the plugin Loader (docs/plugins.md §4).
//  The Loader dlopen's plugins (or adopts in-tree ones), each of which provides
//  its interfaces into the host's di registry; the Loader then resolves every
//  IFrontend and starts it, and tears everything down in reverse on destruction.
//
#include "prfs/plugin.hpp"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace spdlog {
class logger;
}

namespace prfs::host {

//  IHost over a store + a di registry (default di::global()) + a logger + a bag
//  of parsed CLI options.
class Host : public plugin::IHost {
public:
    Host(IPrfs& fs, spdlog::logger& log, di::Registry& reg = di::global());

    di::Registry& registry() override { return m_reg; }

    IPrfs& fs() override { return m_fs; }

    size_t read(Node file, uint64_t off, char* out, size_t len) override;

    spdlog::logger& log() override { return m_log; }

    std::string option(std::string_view key) const override;

    void setOption(std::string key, std::string value); // loader/CLI fills these

private:
    IPrfs& m_fs;
    spdlog::logger& m_log;
    di::Registry& m_reg;
    std::map<std::string, std::string, std::less<>> m_options;
};

//  Loads plugins and owns their lifetimes. Not thread-safe (bootstrap only).
class Loader {
public:
    explicit Loader(plugin::IHost& host)
        : m_host(host) {}

    ~Loader();

    Loader(Loader const&) = delete;
    Loader& operator=(Loader const&) = delete;

    //  dlopen a plugin .so: checks the ABI, calls create() (which provides into
    //  the host registry). Returns false (and logs) on any failure.
    bool load(std::string const& path);

    //  Adopt an already-created in-tree plugin (its create-equivalent already
    //  provided into the registry). The Loader does not own/free it.
    void adopt(plugin::IPlugin* plug) { m_entries.push_back({plug, nullptr, nullptr}); }

    //  Resolve every provided IFrontend and start it (skips + logs failures).
    void startFrontends();
    void stopAll(); // stop started front-ends, in reverse

private:
    struct Entry {
        plugin::IPlugin* plugin = nullptr;
        void (*destroy)(plugin::IPlugin*) = nullptr; // .so deleter, null for adopted
        void* handle = nullptr;                      // dlopen handle, null for adopted
    };

    plugin::IHost& m_host;
    std::vector<Entry> m_entries;
    std::vector<plugin::IFrontend*> m_started;
};

} // namespace prfs::host
