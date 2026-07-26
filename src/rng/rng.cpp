// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  rng registry — the name → generator map and the run-wide active choice. The
//  generators live one-per-file (philox.cpp, threefry.cpp) and self-register;
//  this file holds no list of them.
//
#include "prfs/rng.hpp"

#include <map>
#include <stdexcept>

//  Build-time default generator name (meson -Drng=…).
#ifndef PRFS_DEFAULT_RNG
#define PRFS_DEFAULT_RNG "philox"
#endif

namespace prfs::rng {
namespace {

//  Function-local static ⇒ constructed on first use, so a generator's static
//  Register can call add() during static init with no order-of-init hazard.
std::map<std::string, Gen4, std::less<>>& registry() {
    static std::map<std::string, Gen4, std::less<>> m;
    return m;
}

std::string s_active;

} // namespace

void add(std::string_view name, Gen4 g) { registry()[std::string(name)] = g; }

bool has(std::string_view name) { return registry().find(name) != registry().end(); }

Gen4 get(std::string_view name) {
    auto it = registry().find(name);
    if (it == registry().end()) {
        throw std::out_of_range("rng: unknown generator '" + std::string(name) + "'");
    }
    return it->second;
}

std::vector<std::string> names() {
    std::vector<std::string> out;
    for (auto const& [name, gen] : registry()) {
        out.push_back(name);
    }
    return out;
}

std::string active() { return s_active.empty() ? PRFS_DEFAULT_RNG : s_active; }

void setActive(std::string_view name) {
    if (!has(name)) {
        throw std::out_of_range("rng: unknown generator '" + std::string(name) + "'");
    }
    s_active = std::string(name);
}

Gen4 activeFn() { return get(active()); }

} // namespace prfs::rng
