// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  rng — the run-wide active-generator facade over the DI registry. The
//  generators themselves live one-per-file (philox.cpp, threefry.cpp) and
//  self-register as IRng providers; this file holds no list of them.
//
#include "prfs/rng.hpp"

#include "prfs/di.hpp"

#include <stdexcept>

//  Build-time default generator name (meson -Drng=…).
#ifndef PRFS_DEFAULT_RNG
#define PRFS_DEFAULT_RNG "philox"
#endif

namespace prfs::rng {

static std::string s_active;

std::string active() { return s_active.empty() ? PRFS_DEFAULT_RNG : s_active; }

void setActive(std::string_view name) {
    if (!di::global().has(IRng::ID, name)) {
        throw std::out_of_range("rng: unknown generator '" + std::string(name) + "'");
    }
    s_active = std::string(name);
}

IRng const& activeRng() { return di::global().resolve<IRng>(active()); }

bool has(std::string_view name) { return di::global().has(IRng::ID, name); }

std::vector<std::string> names() { return di::global().names(IRng::ID); }

} // namespace prfs::rng
