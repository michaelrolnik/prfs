// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

#pragma once
//
//  prfs-lua — sol2 bindings (design §12). `libprfs` itself stays lua-free; this
//  layer exposes IPrfs / INode to Lua so test scenarios are scripted, not
//  recompiled. registerLua() installs a global `prfs` table (factories + enums)
//  and the IPrfs / INode usertypes into the given Lua state.
//
#include <sol/forward.hpp>

namespace prfs {

void registerLua(sol::state_view lua);

} // namespace prfs
