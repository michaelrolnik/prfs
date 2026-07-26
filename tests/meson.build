# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

gtest_dep = dependency('gtest', main : true, required : true)

#   The tests dir is on the include path so subdir suites can #include "support.hpp".
test_inc = include_directories('.')

#   Mirrors src/: core/* exercises PrfsStore (all engines); storage/lmdb/* is
#   engine-specific. name → source, followed by an optional per-test timeout.
tests = [
    ['clock', 'core/clock.cpp', 60],
    ['contract', 'core/contract.cpp', 60],
    ['contentconfig', 'core/contentconfig.cpp', 60],
    ['dag', 'core/dag.cpp', 60],
    ['differential', 'core/differential.cpp', 120],
    ['invariant', 'core/invariant.cpp', 120],
    ['determinism', 'core/determinism.cpp', 120],
    ['snapshot', 'core/snapshot.cpp', 60],
    ['snapdir', 'core/snapdir.cpp', 60],
    ['readdirpage', 'core/readdirpage.cpp', 60],
    ['crash', 'storage/lmdb/crash.cpp', 120],
    ['engine', 'storage/engine_test.cpp', 60],
    ['fsstat', 'nfs/fsstat.cpp', 60],
]

foreach t : tests
    exe = executable(
        'test_' + t[0],
        t[1],
        include_directories : test_inc,
        dependencies        : [prfs_ref_dep, prfs_dep, prfs_nfs_dep, gtest_dep],
    )
    test(t[0], exe, timeout : t[2])
endforeach

#   Plugin host — dlopens the null.so built above; NULL_PLUGIN_SO is its path.
if get_option('plugins')
    test_host = executable(
        'test_host',
        'host/host_test.cpp',
        include_directories : [test_inc, prfs_inc, spdlog_inc],
        override_options    : ['warning_level=0'],
        cpp_args            : ['-DNULL_PLUGIN_SO="' + null_so.full_path() + '"'],
        dependencies        : [prfs_host_dep, prfs_ref_dep, gtest_dep],
    )
    test('host', test_host, depends : null_so, timeout : 60)

    nfsv3_test_args = ['-DNFSV3_PLUGIN_SO="' + nfsv3_so.full_path() + '"']
    if get_option('content')
        nfsv3_test_args += '-DPRFS_WITH_CONTENT' # enables the seed→content assertions
    endif
    test_nfsv3 = executable(
        'test_nfsv3',
        'nfsv3/nfsv3_test.cpp',
        include_directories : [test_inc, prfs_inc, spdlog_inc],
        override_options    : ['warning_level=0'],
        cpp_args            : nfsv3_test_args,
        dependencies        : [prfs_host_dep, prfs_ref_dep, gtest_dep],
    )
    test('nfsv3', test_nfsv3, depends : nfsv3_so, timeout : 60)

    if get_option('lua')
        test_luactl = executable(
            'test_luactl',
            'luactl/luactl_test.cpp',
            include_directories : [test_inc, prfs_inc, spdlog_inc],
            override_options    : ['warning_level=0'],
            cpp_args            : ['-DLUACTL_PLUGIN_SO="' + luactl_so.full_path() + '"'],
            dependencies        : [prfs_host_dep, prfs_ref_dep, gtest_dep],
        )
        test('luactl', test_luactl, depends : luactl_so, timeout : 60)
    endif

    #   perf — dlopen perf.so and run the generator benchmark on a tiny workload.
    test_perf = executable(
        'test_perf',
        'perf/perf_test.cpp',
        include_directories : [test_inc, prfs_inc, spdlog_inc],
        override_options    : ['warning_level=0'],
        cpp_args            : ['-DPERF_PLUGIN_SO="' + perf_so.full_path() + '"'],
        dependencies        : [prfs_host_dep, prfs_ref_dep, gtest_dep],
    )
    test('perf', test_perf, depends : perf_so, timeout : 60)
endif

#   DI registry is header-only (include/prfs/di.hpp) — links only gtest.
test_di = executable(
    'test_di',
    'di/di_test.cpp',
    include_directories : [test_inc, prfs_inc],
    dependencies        : [gtest_dep],
)
test('di', test_di, timeout : 30)

#   Content provider is a standalone leaf — links only prfs-content + gtest.
if get_option('content')
    test_content = executable(
        'test_content',
        'content/content_test.cpp',
        dependencies : [prfs_content_dep, gtest_dep],
    )
    test('content', test_content, timeout : 60)
endif
