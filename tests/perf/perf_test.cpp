// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  perf plugin tests (docs/plugins.md §8). dlopen perf.so, run the content-
//  generator benchmark on a deliberately tiny workload (a few KiB across two
//  threads, so it's fast and deterministic to *run* — the throughput number is
//  machine-dependent, so we assert the load → start → withdraw lifecycle and
//  that the multi-threaded benchmark completes without crashing, not a MiB/s.
//  PERF_PLUGIN_SO is the built .so path, from meson.
//
#include "prfs/host.hpp"
#include "prfs/memstore.hpp"

#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <gtest/gtest.h>

#include <memory>

using namespace prfs;

namespace {

std::shared_ptr<spdlog::logger> quietLogger() {
    return std::make_shared<spdlog::logger>("perf-test",
                                            std::make_shared<spdlog::sinks::null_sink_mt>());
}

} // namespace

TEST(Perf, BenchmarkRunsAndLifecycle) {
    di::Registry reg; // isolated scope
    auto fs = makeMemStore();
    auto log = quietLogger();
    host::Host h(*fs, *log, reg);

    //  Keep the workload tiny so the test is fast: 64 KiB per thread, 2 threads,
    //  4 KiB chunks. Enough to exercise the threaded generation path end to end.
    h.setOption("perf.bytes", "65536");
    h.setOption("perf.threads", "2");
    h.setOption("perf.blocksize", "4096");

    {
        host::Loader loader(h);
        ASSERT_TRUE(loader.load(PERF_PLUGIN_SO));
        //  create() provided the perf front-end into the isolated registry.
        EXPECT_TRUE(h.registry().has(plugin::IService::ID, "perf"));
        EXPECT_EQ(h.registry().resolveAll<plugin::IService>().size(), 1u);

        loader.startServices(); // runs the benchmark across threads; must not crash
    } // loader dtor: stop → destroy → withdraw → dlclose

    EXPECT_FALSE(h.registry().has(plugin::IService::ID, "perf")); // withdrawn on unload
}
