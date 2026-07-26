// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

#pragma once
//
//  prfs rng — counter-based random generators behind a name-keyed registry
//  (docs/content.md §4). "Counter-based" = output is a pure function of a 128-bit
//  counter and a key, so it is random-access (any block/word directly
//  addressable) — the property the content generator needs.
//
//  Generators are a registry, not a switch: each one lives in its own file and
//  self-registers (static built-ins via `Register`, kept alive by link_whole; a
//  generator plugin registers the same way when dlopen'd). Adding a generator is
//  one new file — no central list to edit (open/closed).
//
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace prfs::rng {

//  128 bits of output from a 128-bit counter (`ctr[4]`) and a 64-bit key
//  (`key[2]`). Stateless: output depends only on (ctr, key).
using Gen4 = void (*)(uint32_t const ctr[4], uint32_t const key[2], uint32_t out[4]);

void add(std::string_view name, Gen4); // register a generator
bool has(std::string_view name);
Gen4 get(std::string_view name);  // throws std::out_of_range if unknown
std::vector<std::string> names(); // registered generators (for --help / listing)

//  Run-wide active generator by name: the build sets the default (meson -Drng=…)
//  and a tool may override it ONCE at startup (e.g. from a --rng flag). A
//  process-wide policy, not a per-call argument; not safe to change mid-run.
std::string active();
void setActive(std::string_view name); // throws std::out_of_range if unknown
Gen4 activeFn();

//  Self-registration helper: `static rng::Register const r{"philox", &gen};`
struct Register {
    Register(std::string_view name, Gen4 g) { add(name, g); }
};

} // namespace prfs::rng
