// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  Philox4x32-10 (Random123) — the default counter-based generator: BigCrush
//  quality, fast, SIMD-friendly, random-access. Self-registers as "philox".
//
#include "prfs/di.hpp"
#include "prfs/rng.hpp"

#include <Random123/philox.h>

namespace prfs::rng {

//  A file-local type can't be `static`; the anonymous namespace gives it
//  internal linkage. Everything that can be `static` (the objects below) is.
namespace {
struct Philox : IRng {
    void gen4(uint32_t const c[4], uint32_t const k[2], uint32_t out[4]) const override {
        philox4x32_ctr_t ctr = {{c[0], c[1], c[2], c[3]}};
        philox4x32_key_t key = {{k[0], k[1]}};
        philox4x32_ctr_t r = philox4x32(ctr, key);
        out[0] = r.v[0];
        out[1] = r.v[1];
        out[2] = r.v[2];
        out[3] = r.v[3];
    }
};
} // namespace

static Philox g_philox;
static di::Register<IRng> const reg{&g_philox, "philox"};

} // namespace prfs::rng
