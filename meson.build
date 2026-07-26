# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

project(
    'prfs',
    ['c', 'cpp'],
    version         : '0.1.0',
    default_options : ['cpp_std=c++20', 'warning_level=2'],
)

prfs_inc = include_directories('include')

#   Reference implementation — the in-memory oracle (design §13). The LMDB
#   backend will be a second library validated against this.
libprfs_ref = static_library(
    'prfs-ref',
    'src/reference/memstore.cpp',
    include_directories : prfs_inc,
)

prfs_ref_dep = declare_dependency(
    link_with           : libprfs_ref,
    include_directories : prfs_inc,
)

#   Production store — PrfsStore (all FS logic) over an IKvStore engine chosen
#   at build time (-Dstorage=…).
storage = get_option('storage')
message('prfs storage backend: @0@'.format(storage))

prfs_core = files('src/core/prfs.cpp')

if storage == 'memory'
    libprfs = static_library(
        'prfs',
        prfs_core + files('src/storage/memory/kv.cpp'),
        include_directories : prfs_inc,
    )
elif storage == 'lmdb'
    lmdb_dir = 'third_party/lmdb/libraries/liblmdb'  # git submodule
    lmdb_inc = include_directories(lmdb_dir)
    lmdb_lib = static_library(
        'lmdb',
        lmdb_dir / 'mdb.c', lmdb_dir / 'midl.c',
        include_directories : lmdb_inc,
        override_options    : ['warning_level=0', 'c_std=c11'],
        dependencies        : [dependency('threads')],
    )
    lmdb_dep = declare_dependency(
        link_with           : lmdb_lib,
        include_directories : lmdb_inc,
        dependencies        : [dependency('threads')],
    )
    libprfs = static_library(
        'prfs',
        prfs_core + files('src/storage/lmdb/kv.cpp'),
        include_directories : prfs_inc,
        dependencies        : [lmdb_dep],
    )
endif

prfs_dep = declare_dependency(
    link_with           : libprfs,
    include_directories : prfs_inc,
)

#   NFS mapping — the FSSTAT/FSINFO projection over the store's O(1) Stats
#   (design §9). Pure and engine-independent; the future NFS front-end serializes
#   these structs. Depends only on the public interface, so it links no engine.
libprfs_nfs = static_library(
    'prfs-nfs',
    'src/nfs/fsstat.cpp',
    include_directories : prfs_inc,
)

prfs_nfs_dep = declare_dependency(
    link_with           : libprfs_nfs,
    include_directories : prfs_inc,
)

#   Scripting — prfs-lua (sol2 bindings) + prfs-test (scenario runner), design
#   §12. libprfs stays lua-free; this sits on top. Lua and sol2 are git
#   submodules; the whole layer is optional (-Dlua=false).
prfs_lua_dep = disabler()
if get_option('lua')
    cc = meson.get_compiler('c')
    lua_dir = 'third_party/lua'  # git submodule (pinned v5.4.7)
    #   Single-TU Lua library build: onelua.c with MAKE_LIB pulls in core + libs.
    lua_lib = static_library(
        'lua',
        lua_dir / 'onelua.c',
        c_args              : '-DMAKE_LIB',
        include_directories : include_directories(lua_dir),
        override_options    : ['warning_level=0', 'c_std=c11'],
    )
    lua_dep = declare_dependency(
        link_with           : lua_lib,
        # lua-cxx supplies the lua.hpp wrapper sol2 expects; it precedes the sources.
        include_directories : include_directories('third_party/lua-cxx', lua_dir),
        dependencies        : [cc.find_library('m', required : false)],
    )
    sol2_dep = declare_dependency(  # header-only; git submodule
        include_directories : include_directories('third_party/sol2/include'),
        compile_args        : ['-DSOL_ALL_SAFETIES_ON=1'],
        dependencies        : [lua_dep],
    )
    libprfs_lua = static_library(
        'prfs-lua',
        'src/lua/bindings.cpp',
        include_directories : prfs_inc,
        override_options    : ['warning_level=0'],  # sol2 is warning-heavy
        dependencies        : [prfs_dep, prfs_ref_dep, prfs_nfs_dep, sol2_dep],
    )
    prfs_lua_dep = declare_dependency(
        link_with           : libprfs_lua,
        include_directories : prfs_inc,
        dependencies        : [prfs_dep, prfs_ref_dep, prfs_nfs_dep, sol2_dep],
    )
    prfs_test = executable(
        'prfs-test',
        'src/lua/harness.cpp',
        override_options    : ['warning_level=0'],
        dependencies        : [prfs_lua_dep],
    )
    test('lua-scenario', prfs_test, args : files('tests/lua/smoke.lua'), timeout : 60)
endif

#   Formatting — clang-format (.clang-format) is the style authority.
#   `meson compile format` rewrites in place; the `clang-format` test gates it.
clang_format = find_program('clang-format', required : false)
fmt_files = files(
    'include/prfs/prfs.hpp',
    'include/prfs/kvstore.hpp',
    'include/prfs/memstore.hpp',
    'src/reference/memstore.cpp',
    'src/core/prfs.cpp',
    'src/storage/memory/kv.cpp',
    'src/storage/lmdb/kv.cpp',
    'include/prfs/fsstat.hpp',
    'src/nfs/fsstat.cpp',
    'include/prfs/lua.hpp',
    'src/lua/bindings.cpp',
    'src/lua/harness.cpp',
    'tests/support.hpp',
    'tests/core/clock.cpp',
    'tests/core/contract.cpp',
    'tests/core/dag.cpp',
    'tests/core/differential.cpp',
    'tests/core/invariant.cpp',
    'tests/core/determinism.cpp',
    'tests/core/snapshot.cpp',
    'tests/storage/lmdb/crash.cpp',
    'tests/nfs/fsstat.cpp',
)
if clang_format.found()
    run_target('format', command : [clang_format, '-i'] + fmt_files)
    test('clang-format', clang_format, args : ['--dry-run', '--Werror'] + fmt_files)
endif

subdir('tests')

