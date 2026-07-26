// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

#pragma once
//
//  prfs DI — the service registry (docs/di.md). Providers register under
//  (interface-id, name); consumers resolve by interface (+ optional name). Small,
//  typed, header-only. Each interface carries `static constexpr std::string_view
//  ID`; the registry keys on it, and provide/resolve are the only type-erasure
//  point (bounded by the ID↔type contract).
//
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prfs::di {

//  Thrown by resolve() / requireAllResolved() when a provider is missing.
struct Unresolved : std::runtime_error {
    Unresolved(std::string_view id, std::string_view name)
        : std::runtime_error("di: unresolved " + std::string(id) +
                             (name.empty() ? std::string() : " [" + std::string(name) + "]")) {}
};

class Registry {
public:
    template <class Intf> void provide(Intf* impl, std::string_view name = "") {
        add(Intf::ID, name, static_cast<void*>(impl));
    }

    template <class Intf> void withdraw(std::string_view name = "") { remove(Intf::ID, name); }

    template <class Intf> Intf* tryResolve(std::string_view name = "") const {
        return static_cast<Intf*>(find(Intf::ID, name));
    }

    template <class Intf> Intf& resolve(std::string_view name = "") const {
        if (Intf* p = tryResolve<Intf>(name)) {
            return *p;
        }
        throw Unresolved(Intf::ID, name);
    }

    template <class Intf> std::vector<Intf*> resolveAll() const {
        std::vector<Intf*> out;
        for (void* p : findAll(Intf::ID)) {
            out.push_back(static_cast<Intf*>(p));
        }
        return out;
    }

    template <class Intf> void require(std::string_view name = "") {
        m_required.emplace_back(std::string(Intf::ID), std::string(name));
    }

    bool has(std::string_view id, std::string_view name = "") const {
        return find(id, name) != nullptr;
    }

    //  Variant names registered for one interface (sorted).
    std::vector<std::string> names(std::string_view id) const {
        std::vector<std::string> out;
        for (auto const& [key, impl] : m_map) {
            if (key.first == id) {
                out.push_back(key.second);
            }
        }
        return out;
    }

    //  Every registered interface id, once (sorted).
    std::vector<std::string> ids() const {
        std::vector<std::string> out;
        for (auto const& [key, impl] : m_map) {
            if (out.empty() || out.back() != key.first) {
                out.push_back(key.first);
            }
        }
        return out;
    }

    //  Throws Unresolved listing every declared require() still missing.
    void requireAllResolved() const {
        std::string missing;
        for (auto const& [id, name] : m_required) {
            if (m_map.find({id, name}) != m_map.end()) {
                continue;
            }
            if (!missing.empty()) {
                missing += ", ";
            }
            missing += name.empty() ? id : id + "[" + name + "]";
        }
        if (!missing.empty()) {
            throw Unresolved(missing, "");
        }
    }

private:
    using Key = std::pair<std::string, std::string>; // (interface-id, name)

    void add(std::string_view id, std::string_view name, void* impl) {
        m_map[Key(std::string(id), std::string(name))] = impl;
    }

    void remove(std::string_view id, std::string_view name) {
        m_map.erase(Key(std::string(id), std::string(name)));
    }

    void* find(std::string_view id, std::string_view name) const {
        auto it = m_map.find(Key(std::string(id), std::string(name)));
        return it == m_map.end() ? nullptr : it->second;
    }

    std::vector<void*> findAll(std::string_view id) const {
        std::vector<void*> out;
        for (auto const& [key, impl] : m_map) {
            if (key.first == id) {
                out.push_back(impl);
            }
        }
        return out;
    }

    std::map<Key, void*> m_map;
    std::vector<Key> m_required;
};

//  The process-wide default registry. A function-local static (Meyers singleton)
//  so self-registering providers are safe against the static-init-order fiasco
//  (docs/di.md §2.1); `inline` gives one shared instance across all TUs.
inline Registry& global() {
    static Registry r;
    return r;
}

//  Convenience wrappers over global().
template <class Intf> void provide(Intf* impl, std::string_view name = "") {
    global().provide<Intf>(impl, name);
}

template <class Intf> Intf& resolve(std::string_view name = "") {
    return global().resolve<Intf>(name);
}

template <class Intf> Intf* tryResolve(std::string_view name = "") {
    return global().tryResolve<Intf>(name);
}

//  Self-registration helper for compiled-in built-ins (kept alive by link_whole):
//      static di::Register<IRng> const r{&philoxImpl, "philox"};
template <class Intf> struct Register {
    Register(Intf* impl, std::string_view name = "") { global().provide<Intf>(impl, name); }
};

} // namespace prfs::di
