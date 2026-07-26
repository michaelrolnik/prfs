// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  prfs host — Host (IHost impl) + Loader (dlopen + lifecycle). See host.hpp.
//
#include "prfs/host.hpp"

#ifdef PRFS_WITH_CONTENT
#include "prfs/content.hpp"
#endif

#include <spdlog/spdlog.h>

#include <algorithm>
#include <dlfcn.h>
#include <utility>

namespace prfs::host {

Host::Host(IPrfs& fs, spdlog::logger& log, di::Registry& reg)
    : m_fs(fs)
    , m_log(log)
    , m_reg(reg) {}

size_t Host::read(Node file, uint64_t off, char* out, size_t len) {
    uint64_t size = file->size();
    if (off >= size || len == 0) {
        return 0;
    }
    size_t want = size_t(std::min<uint64_t>(len, size - off));

#ifdef PRFS_WITH_CONTENT
    //  Procedural content: the active provider generates bytes from the store's
    //  opaque config + the file's node id as seed.
    std::string cfg = m_fs.contentConfig();
    if (!cfg.empty()) {
        if (content::IContentProvider* cp =
                m_reg.tryResolve<content::IContentProvider>(content::provider())) {
            return cp->read(cfg, file->id(), size, off, out, len);
        }
    }
#endif

    //  Fallback: literal stored bytes (small files), zero-filled to size.
    std::string lit = file->content();
    for (size_t i = 0; i < want; ++i) {
        out[i] = off + i < lit.size() ? lit[off + i] : '\0';
    }
    return want;
}

std::string Host::option(std::string_view key) const {
    auto it = m_options.find(key);
    return it == m_options.end() ? std::string() : it->second;
}

void Host::setOption(std::string key, std::string value) {
    m_options[std::move(key)] = std::move(value);
}

Loader::~Loader() {
    stopAll();
    //  Reverse order: destroy() withdraws the plugin's providers, then dlclose.
    for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it) {
        if (it->destroy) {
            it->destroy(it->plugin);
        }
        if (it->handle) {
            dlclose(it->handle);
        }
    }
}

bool Loader::load(std::string const& path) {
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (h == nullptr) {
        m_host.log().error("plugin: dlopen {} failed: {}", path, dlerror());
        return false;
    }

    auto abi = reinterpret_cast<uint32_t (*)()>(dlsym(h, "prfs_abi"));
    auto create =
        reinterpret_cast<plugin::IPlugin* (*)(plugin::IHost&)>(dlsym(h, "prfs_plugin_create"));
    auto destroy = reinterpret_cast<void (*)(plugin::IPlugin*)>(dlsym(h, "prfs_plugin_destroy"));
    if (abi == nullptr || create == nullptr || destroy == nullptr) {
        m_host.log().error("plugin: {} is missing prfs_abi/create/destroy", path);
        dlclose(h);
        return false;
    }
    if (abi() != plugin::ABI) {
        m_host.log().error("plugin: {} ABI {} != host {}", path, abi(), plugin::ABI);
        dlclose(h);
        return false;
    }

    plugin::IPlugin* p = create(m_host); // provides into m_host.registry()
    m_host.log().info("plugin: loaded {} {}", p->name(), p->version());
    m_entries.push_back({p, destroy, h});
    return true;
}

void Loader::startFrontends() {
    for (plugin::IFrontend* fe : m_host.registry().resolveAll<plugin::IFrontend>()) {
        if (fe->start() == Error::OK) {
            m_started.push_back(fe);
        } else {
            m_host.log().error("plugin: a front-end failed to start");
        }
    }
}

void Loader::stopAll() {
    for (auto it = m_started.rbegin(); it != m_started.rend(); ++it) {
        (*it)->stop();
    }
    m_started.clear();
}

} // namespace prfs::host
