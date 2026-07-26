// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  Tests for the DI registry (docs/di.md §10). Providers register under
//  (interface-id, name); consumers resolve by interface + optional name.
//  Covers round-trip, fail-loud resolve, variant names, resolveAll, withdraw,
//  introspection, requireAllResolved, Registry isolation, and the global()
//  helpers + self-registering di::Register.
//
#include "prfs/di.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace prfs;

namespace {

struct IFoo {
    static constexpr std::string_view ID = "test.foo/1";
    virtual ~IFoo() = default;
    virtual int foo() const = 0;
};

struct IBar {
    static constexpr std::string_view ID = "test.bar/1";
    virtual ~IBar() = default;
    virtual int bar() const = 0;
};

struct Foo : IFoo {
    int v;

    explicit Foo(int x)
        : v(x) {}

    int foo() const override { return v; }
};

struct Bar : IBar {
    int bar() const override { return 42; }
};

} // namespace

TEST(Di, ProvideResolveRoundTrip) {
    di::Registry r;
    Foo f{7};
    r.provide<IFoo>(&f);

    EXPECT_TRUE(r.has(IFoo::ID));
    EXPECT_EQ(&r.resolve<IFoo>(), &f); // same object
    EXPECT_EQ(r.resolve<IFoo>().foo(), 7);
    EXPECT_EQ(r.tryResolve<IFoo>(), &f);
}

TEST(Di, UnresolvedThrowsAndTryResolveIsNull) {
    di::Registry r;
    EXPECT_FALSE(r.has(IFoo::ID));
    EXPECT_EQ(r.tryResolve<IFoo>(), nullptr);
    EXPECT_THROW(r.resolve<IFoo>(), di::Unresolved);
    EXPECT_THROW(r.resolve<IFoo>("missing"), di::Unresolved);
}

TEST(Di, NamesSelectVariants) {
    di::Registry r;
    Foo a{1}, b{2};
    r.provide<IFoo>(&a, "a");
    r.provide<IFoo>(&b, "b");

    EXPECT_EQ(r.resolve<IFoo>("a").foo(), 1);
    EXPECT_EQ(r.resolve<IFoo>("b").foo(), 2);
    EXPECT_THROW(r.resolve<IFoo>("c"), di::Unresolved); // unknown name
    EXPECT_THROW(r.resolve<IFoo>(), di::Unresolved);    // no unnamed provider

    EXPECT_EQ(r.names(IFoo::ID), (std::vector<std::string>{"a", "b"}));
}

TEST(Di, ResolveAll) {
    di::Registry r;
    Foo a{1}, b{2}, c{4};
    r.provide<IFoo>(&a, "a");
    r.provide<IFoo>(&b, "b");
    r.provide<IFoo>(&c); // unnamed also counts

    int sum = 0;
    auto all = r.resolveAll<IFoo>();
    for (IFoo* p : all) {
        sum += p->foo();
    }
    EXPECT_EQ(all.size(), 3u);
    EXPECT_EQ(sum, 7);
    EXPECT_TRUE(r.resolveAll<IBar>().empty());
}

TEST(Di, Withdraw) {
    di::Registry r;
    Foo f{1};
    r.provide<IFoo>(&f, "x");
    EXPECT_TRUE(r.has(IFoo::ID, "x"));
    r.withdraw<IFoo>("x");
    EXPECT_FALSE(r.has(IFoo::ID, "x"));
    EXPECT_EQ(r.tryResolve<IFoo>("x"), nullptr);
}

TEST(Di, IdsIntrospection) {
    di::Registry r;
    Foo f{1};
    Bar b;
    r.provide<IFoo>(&f);
    r.provide<IBar>(&b);
    EXPECT_EQ(r.ids(), (std::vector<std::string>{"test.bar/1", "test.foo/1"})); // sorted, deduped
}

TEST(Di, RequireAllResolvedReportsMissing) {
    di::Registry r;
    r.require<IFoo>();
    r.require<IBar>("x");
    EXPECT_THROW(r.requireAllResolved(), di::Unresolved); // both missing

    Foo f{1};
    Bar b;
    r.provide<IFoo>(&f);
    r.provide<IBar>(&b, "x");
    EXPECT_NO_THROW(r.requireAllResolved());
}

TEST(Di, RegistriesAreIsolated) {
    di::Registry r1, r2;
    Foo f{5};
    r1.provide<IFoo>(&f);
    EXPECT_TRUE(r1.has(IFoo::ID));
    EXPECT_FALSE(r2.has(IFoo::ID)); // no leakage between scopes
}

TEST(Di, GlobalHelpersAndRegister) {
    static Foo gf{99};
    di::provide<IFoo>(&gf, "global-test");
    EXPECT_EQ(di::resolve<IFoo>("global-test").foo(), 99);
    EXPECT_EQ(di::tryResolve<IFoo>("global-test"), &gf);
    di::global().withdraw<IFoo>("global-test"); // keep global() clean

    static Bar gb;
    {
        di::Register<IBar> const reg{&gb, "reg-test"}; // self-registration into global()
        EXPECT_TRUE(di::global().has(IBar::ID, "reg-test"));
    }
    di::global().withdraw<IBar>("reg-test");
}
