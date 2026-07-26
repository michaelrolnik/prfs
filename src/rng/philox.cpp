// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  Philox4x32-10 (Random123) — the default counter-based generator: BigCrush
//  quality, fast, SIMD-friendly, random-access. Key = 2×u32, counter = 4×u32.
//
#include "prfs/rng.hpp"

#include <Random123/philox.h>

namespace prfs::rng {

void philoxGen4(uint32_t const c[4], uint32_t const k[2], uint32_t out[4]) {
    philox4x32_ctr_t ctr = {{c[0], c[1], c[2], c[3]}};
    philox4x32_key_t key = {{k[0], k[1]}};
    philox4x32_ctr_t r = philox4x32(ctr, key);
    out[0] = r.v[0];
    out[1] = r.v[1];
    out[2] = r.v[2];
    out[3] = r.v[3];
}

} // namespace prfs::rng
