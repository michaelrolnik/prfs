// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

#pragma once
//
//  prfs rng — counter-based random generators (docs/content.md §4). A leaf
//  module: each generator (Philox, Threefry, … from Random123) lives in its own
//  translation unit under src/rng/ behind one API. "Counter-based" = the output
//  is a pure function of a 128-bit counter and a key, so it is random-access
//  (any block/word directly addressable) — the property the content generator
//  needs. New generators are one more file + one `switch` arm.
//
#include <cstdint>
#include <string_view>

namespace prfs::rng {

enum class Kind : uint8_t { Philox, Threefry };

//  128 bits of output from a 128-bit counter (`ctr[4]`) and a 64-bit key
//  (`key[2]`). Stateless: output depends only on (ctr, key).
using Gen4 = void (*)(uint32_t const ctr[4], uint32_t const key[2], uint32_t out[4]);

Gen4 fn(Kind);                       // the generator function for a kind
char const* name(Kind);              // "philox" / "threefry"
bool parse(std::string_view, Kind&); // for a --rng command-line flag

//  Run-wide active generator: the build sets the default (meson -Drng=…) and a
//  tool may override it ONCE at startup (e.g. from a --rng flag). It is a
//  process-wide policy, not a per-call argument; not safe to change mid-run.
Kind active();
void setActive(Kind);

inline Gen4 activeFn() { return fn(active()); }

} // namespace prfs::rng
