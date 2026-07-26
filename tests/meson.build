# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

gtest_dep = dependency('gtest', main : true, required : true)

test_store = executable(
    'test_store',
    'test_store.cpp',
    dependencies : [prfs_ref_dep, prfs_dep, gtest_dep],
)

test('store', test_store)
