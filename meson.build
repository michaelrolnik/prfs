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
    'tests/test_store.cpp',
    'tests/test_diff.cpp',
)
if clang_format.found()
    run_target('format', command : [clang_format, '-i'] + fmt_files)
    test('clang-format', clang_format, args : ['--dry-run', '--Werror'] + fmt_files)
endif

subdir('tests')

