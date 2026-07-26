// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  Plugin host tests (docs/plugins.md §6). Exercises the loader/lifecycle two
//  ways: adopting an in-tree front-end (provide → resolveAll → start/stop), and
//  dlopen'ing the null plugin .so (ABI check, create → provide, destroy →
//  withdraw). NULL_PLUGIN_SO is the built .so path, from meson.
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
    return std::make_shared<spdlog::logger>("host-test",
                                            std::make_shared<spdlog::sinks::null_sink_mt>());
}

struct CountingFrontend : plugin::IFrontend {
    int started = 0, stopped = 0;

    Error start() override {
        ++started;
        return Error::OK;
    }

    void stop() override { ++stopped; }
};

} // namespace

TEST(Host, AdoptStartStop) {
    di::Registry reg; // isolated scope
    auto fs = makeMemStore();
    auto log = quietLogger();
    host::Host h(*fs, *log, reg);
    host::Loader loader(h);

    CountingFrontend fe;
    h.registry().provide<plugin::IFrontend>(&fe, "test");

    loader.startFrontends();
    EXPECT_EQ(fe.started, 1);
    EXPECT_EQ(h.registry().resolveAll<plugin::IFrontend>().size(), 1u);

    loader.stopAll();
    EXPECT_EQ(fe.stopped, 1);
}

TEST(Host, DlopenNullPluginLifecycle) {
    di::Registry reg;
    auto fs = makeMemStore();
    auto log = quietLogger();
    host::Host h(*fs, *log, reg);

    {
        host::Loader loader(h);
        ASSERT_TRUE(loader.load(NULL_PLUGIN_SO));
        //  create() provided the null front-end into the (isolated) registry.
        EXPECT_TRUE(h.registry().has(plugin::IFrontend::ID, "null"));
        EXPECT_EQ(h.registry().resolveAll<plugin::IFrontend>().size(), 1u);

        loader.startFrontends(); // null start() does an fs op; must not crash
    } // loader dtor: stop → destroy → withdraw → dlclose

    EXPECT_FALSE(h.registry().has(plugin::IFrontend::ID, "null")); // withdrawn on unload
}

TEST(Host, BadPathIsRejectedNotFatal) {
    di::Registry reg;
    auto fs = makeMemStore();
    auto log = quietLogger();
    host::Host h(*fs, *log, reg);
    host::Loader loader(h);

    EXPECT_FALSE(loader.load("/no/such/plugin.so")); // logs + returns false, no throw
    EXPECT_TRUE(h.registry().ids().empty());
}
