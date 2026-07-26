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

#   Production store — PrfsStore (all FS logic) over an IKvStore engine. Both
#   engines are compiled and self-register as di IStorageEngine providers;
#   -Dstorage picks the runtime default (openPrfs resolves it by name).
storage = get_option('storage')
message('prfs default storage engine: @0@'.format(storage))

#   liblmdb (git submodule) — always built (the lmdb engine needs it).
lmdb_dir = 'third_party/lmdb/libraries/liblmdb'
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

#   The engines — each self-registers an IStorageEngine (link_whole keeps them).
libprfs_engines = static_library(
    'prfs-engines',
    files('src/storage/memory/kv.cpp', 'src/storage/lmdb/kv.cpp'),
    include_directories : prfs_inc,
    dependencies        : [lmdb_dep],
)

#   Core store logic + engine-agnostic openPrfs (resolves the default engine).
libprfs = static_library(
    'prfs',
    files('src/core/prfs.cpp', 'src/core/open.cpp'),
    include_directories : prfs_inc,
    cpp_args            : ['-DPRFS_DEFAULT_STORAGE="' + storage + '"'],
)

prfs_dep = declare_dependency(
    link_with           : libprfs,
    link_whole          : libprfs_engines,
    include_directories : prfs_inc,
    dependencies        : [lmdb_dep],
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

#   Content provider — the READ-path byte generator (docs/content.md, L1). A leaf
#   (stdlib + Random123 submodule only, no engine). The RNG is a run-wide choice:
#   -Drng picks the build-time default; a tool may override it once at startup.
prfs_content_dep = disabler()
if get_option('content')
    #   rng — counter-based generators (Philox/Threefry) behind one API, one file
    #   per generator. Wraps the Random123 submodule; -Drng picks the default.
    r123_inc = include_directories('third_party/random123/include', is_system : true)
    libprfs_rng = static_library(
        'prfs-rng',
        files('src/rng/rng.cpp', 'src/rng/philox.cpp', 'src/rng/threefry.cpp'),
        include_directories : [prfs_inc, r123_inc],
        cpp_args            : ['-DPRFS_DEFAULT_RNG="' + get_option('rng') + '"'],
    )
    #   link_whole keeps the generators' self-registration objects — nothing
    #   references them directly, so a plain link would drop them (open/closed).
    prfs_rng_dep = declare_dependency(
        link_whole          : libprfs_rng,
        include_directories : prfs_inc,
    )
    libprfs_content = static_library(
        'prfs-content',
        'src/content/content.cpp',
        include_directories : prfs_inc,
        dependencies        : [prfs_rng_dep],
    )
    prfs_content_dep = declare_dependency(
        link_with           : libprfs_content,
        include_directories : prfs_inc,
        dependencies        : [prfs_rng_dep],
    )
    message('prfs content RNG default: @0@'.format(get_option('rng')))
endif

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
    #   The content provider is optional; expose prfs.content/prfs.rng only when built.
    lua_deps = [prfs_dep, prfs_ref_dep, prfs_nfs_dep, sol2_dep]
    lua_args = []
    if get_option('content')
        lua_deps += prfs_content_dep
        lua_args += '-DPRFS_WITH_CONTENT'
    endif
    libprfs_lua = static_library(
        'prfs-lua',
        'src/lua/bindings.cpp',
        include_directories : prfs_inc,
        override_options    : ['warning_level=0'],  # sol2 is warning-heavy
        cpp_args            : lua_args,
        dependencies        : lua_deps,
    )
    prfs_lua_dep = declare_dependency(
        link_with           : libprfs_lua,
        include_directories : prfs_inc,
        dependencies        : lua_deps,
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
    'src/core/open.cpp',
    'src/storage/memory/kv.cpp',
    'src/storage/lmdb/kv.cpp',
    'include/prfs/fsstat.hpp',
    'src/nfs/fsstat.cpp',
    'include/prfs/di.hpp',
    'tests/di/di_test.cpp',
    'include/prfs/rng.hpp',
    'src/rng/rng.cpp',
    'src/rng/philox.cpp',
    'src/rng/threefry.cpp',
    'include/prfs/content.hpp',
    'src/content/content.cpp',
    'tests/content/content_test.cpp',
    'include/prfs/lua.hpp',
    'src/lua/bindings.cpp',
    'src/lua/harness.cpp',
    'tests/support.hpp',
    'tests/core/clock.cpp',
    'tests/core/contract.cpp',
    'tests/core/contentconfig.cpp',
    'tests/core/dag.cpp',
    'tests/core/differential.cpp',
    'tests/core/invariant.cpp',
    'tests/core/determinism.cpp',
    'tests/core/snapshot.cpp',
    'tests/core/snapdir.cpp',
    'tests/core/readdirpage.cpp',
    'tests/storage/lmdb/crash.cpp',
    'tests/storage/engine_test.cpp',
    'tests/nfs/fsstat.cpp',
)
if clang_format.found()
    run_target('format', command : [clang_format, '-i'] + fmt_files)
    test('clang-format', clang_format, args : ['--dry-run', '--Werror'] + fmt_files)
endif

subdir('tests')

