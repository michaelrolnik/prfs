// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  C++ wrapper for the Lua headers. The upstream Lua source mirror ships this as
//  lua.hpp inside its release tarballs, but the git repo does not, so we supply
//  it here and put this directory on the include path ahead of the Lua sources.
//  sol2 does `#include <lua.hpp>`; this resolves it.
//
extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}
