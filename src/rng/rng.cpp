// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  rng registry — maps Kind → generator, and holds the run-wide active choice.
//  The generators themselves live one-per-file (philox.cpp, threefry.cpp).
//
#include "prfs/rng.hpp"

//  Build-time default (meson -Drng=…). PRFS_DEFAULT_RNG expands to one of these.
#define PRFS_RNG_PHILOX 0
#define PRFS_RNG_THREEFRY 1
#ifndef PRFS_DEFAULT_RNG
#define PRFS_DEFAULT_RNG PRFS_RNG_PHILOX
#endif

namespace prfs::rng {

void philoxGen4(uint32_t const[4], uint32_t const[2], uint32_t[4]);
void threefryGen4(uint32_t const[4], uint32_t const[2], uint32_t[4]);

namespace {
Kind s_active = Kind(PRFS_DEFAULT_RNG);
}

Gen4 fn(Kind k) { return k == Kind::Threefry ? &threefryGen4 : &philoxGen4; }

char const* name(Kind k) { return k == Kind::Threefry ? "threefry" : "philox"; }

bool parse(std::string_view s, Kind& out) {
    if (s == "philox") {
        out = Kind::Philox;
        return true;
    }
    if (s == "threefry") {
        out = Kind::Threefry;
        return true;
    }
    return false;
}

Kind active() { return s_active; }

void setActive(Kind k) { s_active = k; }

} // namespace prfs::rng
