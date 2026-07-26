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
    ['dag', 'core/dag.cpp', 60],
    ['differential', 'core/differential.cpp', 120],
    ['invariant', 'core/invariant.cpp', 120],
    ['determinism', 'core/determinism.cpp', 120],
    ['crash', 'storage/lmdb/crash.cpp', 120],
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
