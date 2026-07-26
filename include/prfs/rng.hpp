// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

#pragma once
//
//  prfs rng — counter-based random generators as di providers (docs/content.md
//  §4, docs/di.md §9). Each generator implements IRng and self-registers into the
//  DI registry under its name (`di::Register<IRng>`, kept by link_whole). "Which
//  generator" is a di *name*; this header adds only the run-wide active choice on
//  top. Counter-based ⇒ output is a pure function of counter+key (random-access),
//  which is what the content generator needs.
//
#include "prfs/di.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace prfs::rng {

//  A counter-based generator: 128 bits of output from a 128-bit counter
//  (`ctr[4]`) and a 64-bit key (`key[2]`). Registered as a di provider named for
//  the algorithm ("philox", "threefry", …).
struct IRng {
    static constexpr std::string_view ID = "prfs.rng/1";

    virtual ~IRng() = default;
    virtual void generate(uint32_t const ctr[4], uint32_t const key[2], uint32_t out[4]) const = 0;
};

//  Run-wide active generator by name: the build sets the default (meson -Drng=)
//  and a tool may override it ONCE at startup. A process-wide policy, not a
//  per-call argument; not safe to change mid-run.
std::string active();
void setActive(std::string_view name); // throws std::out_of_range if unregistered
IRng const& activeRng();               // resolve the active generator

//  Thin facades over the di registry for the IRng interface.
bool has(std::string_view name);
std::vector<std::string> names();

} // namespace prfs::rng
