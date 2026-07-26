// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  Threefry4x32-20 (Random123) — an alternative counter-based generator (a
//  reduced-round Threefish). Same random-access contract as Philox; its 4×u32
//  key takes the 64-bit content seed in its low two words. Registers as
//  "threefry".
//
#include "prfs/di.hpp"
#include "prfs/rng.hpp"

#include <Random123/threefry.h>

namespace prfs::rng {

//  A file-local type can't be `static`; the anonymous namespace gives it
//  internal linkage. Everything that can be `static` (the objects below) is.
namespace {
struct Threefry : IRng {
    void generate(uint32_t const c[4], uint32_t const k[2], uint32_t out[4]) const override {
        threefry4x32_ctr_t ctr = {{c[0], c[1], c[2], c[3]}};
        threefry4x32_key_t key = {{k[0], k[1], 0, 0}};
        threefry4x32_ctr_t r = threefry4x32(ctr, key);
        out[0] = r.v[0];
        out[1] = r.v[1];
        out[2] = r.v[2];
        out[3] = r.v[3];
    }
};
} // namespace

static Threefry g_threefry;
static di::Register<IRng> const reg{&g_threefry, "threefry"};

} // namespace prfs::rng
