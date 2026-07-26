// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  prfs-test — the Lua scenario runner (design §12). Embeds Lua, installs the
//  prfs bindings, and executes a .lua scenario file. Extra argv are passed to
//  the script as the `arg` table (arg[1], arg[2], …). Exit status: 0 on success,
//  1 on a Lua/runtime error (e.g. a failed assert), 2 on bad usage.
//
//  Usage: prfs-test <scenario.lua> [args...]
//
#include "prfs/lua.hpp"

#include <sol/sol.hpp>

#include <cstdio>
#include <exception>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <scenario.lua> [args...]\n", argv[0]);
        return 2;
    }

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table, sol::lib::math,
                       sol::lib::os, sol::lib::io, sol::lib::package);
    prfs::registerLua(lua);

    sol::table args = lua.create_table();
    for (int i = 2; i < argc; ++i) {
        args[i - 1] = std::string(argv[i]);
    }
    lua["arg"] = args;

    try {
        sol::protected_function_result r = lua.safe_script_file(argv[1]);
        if (!r.valid()) {
            sol::error err = r;
            std::fprintf(stderr, "prfs-test: %s\n", err.what());
            return 1;
        }
    } catch (std::exception const& e) {
        std::fprintf(stderr, "prfs-test: %s\n", e.what());
        return 1;
    }
    return 0;
}
