// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

#pragma once
//
//  In-memory reference implementation of IPrfs — the executable spec / test
//  oracle (design §13). Correctness over speed: stats and diffs are recomputed
//  by scanning. The LMDB backend is validated against this via differential
//  testing.
//
#include "prfs/prfs.hpp"

namespace prfs {

std::unique_ptr<IPrfs> makeMemStore();

} // namespace prfs
