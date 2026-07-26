# prfs DI — the service registry

A small, typed, C++-native **dependency-injection container**: providers register services keyed
by `(interface-id, name)`; consumers resolve by interface (with an optional name). It is the
substrate the plugin host ([`plugins.md`](plugins.md)) and the built-in extension points (rng
generators, storage engines) all share — one wiring mechanism instead of three.

It is the well-worn **service-locator / DI** pattern (named binding, variant *names*, fail-loud on
unresolved, per-scope isolation, introspection), kept **typed**, **RAII**, and **explicit**: no
`void*` vtable patching, no teardown bookkeeping, no `__attribute__((constructor))` magic, no C
ABI. §8 collects the design decisions.

> **Development principles (binding, design §13):** SOLID, test-first, clang-format.

---

## 1. Concepts

- **Interface** — a C++ abstract type carrying a stable, versioned ID:
  `static constexpr std::string_view ID = "prfs.rng/1"`. Bumping the interface = a new ID.
- **Name** — an optional secondary key that selects a *variant* among providers of the same
  interface: `engine → "lmdb"|"memory"`, `rng → "philox"|"threefry"`, `frontend →
  "nfsv3"|"mount"`. Empty name = the single/default provider. Names are how `-Dstorage=` /
  `-Drng=` / a `--name` flag choose an implementation.
- **Provider** — an implementation registered under `(ID, name)`. The registry stores a
  type-erased pointer tagged with its interface ID; the typed accessors cast it back.
- **Registry** — owns a set of providers. The process has one default registry (`di::global()`);
  a test constructs its own (an ordinary object) for isolation. Registries do not inherit from
  one another.

---

## 2. API — `include/prfs/di.hpp`

```cpp
namespace prfs::di {

struct Unresolved : std::runtime_error {          // thrown by resolve() when absent
    Unresolved(std::string_view id, std::string_view name);
};

class Registry {
public:
    template <class Intf> void provide(Intf* impl, std::string_view name = "");
    template <class Intf> void withdraw(std::string_view name = "");   // for plugin unload

    template <class Intf> Intf& resolve(std::string_view name = "") const;    // throws Unresolved
    template <class Intf> Intf* tryResolve(std::string_view name = "") const; // nullptr if absent
    template <class Intf> std::vector<Intf*> resolveAll() const;               // every name of Intf

    bool has(std::string_view id, std::string_view name = "") const;
    std::vector<std::string> names(std::string_view id) const;  // variants of one interface
    std::vector<std::string> ids() const;                        // all registered interfaces

    //  fail-loud startup validation (see §3)
    template <class Intf> void require(std::string_view name = "");
    void requireAllResolved() const;   // throws once, listing every missing (id, name)
};

Registry& global();                    // default registry — a function-local static (see §2.1)

//  convenience wrappers over global()
template <class Intf> void provide(Intf*, std::string_view name = "");
template <class Intf> Intf& resolve(std::string_view name = "");
template <class Intf> Intf* tryResolve(std::string_view name = "");

} // namespace prfs::di
```

### 2.1 `global()` is a function, not a variable (SIOF-safe)

The default registry is reached only through `global()`, implemented as a **function-local
static** (Meyers singleton):

```cpp
Registry& global() { static Registry r; return r; }
```

This is required, not cosmetic. Built-in providers self-register via `static di::Register<Intf>`
objects that run at **static-init time** and call `global().provide(...)`. A namespace-scope
`Registry` variable could be constructed *after* some other TU's `Register` runs — the
static-initialization-order fiasco. A function-local static is constructed on **first call**, so
the registry always exists before the first `provide()`. (This is exactly why `rng.cpp`'s
`registry()` is a function-local static.)

Storage is a `map<pair<string,string>, Slot>` where `Slot = { void* impl; std::string_view id; }`;
`provide<Intf>` records `{impl, Intf::ID}` at key `(Intf::ID, name)`, `resolve<Intf>` looks up `(Intf::ID,
name)` and `static_cast<Intf*>`s. Type safety rests on the **ID↔type contract** — the usual
convention for a type-erased registry, made explicit by versioning the ID.

---

## 3. Fail-loud (unresolved dependencies)

A missing dependency must fail loudly, not become a silent null that segfaults on first use. Two
mechanisms, both catching it at wiring time:

- **`resolve<Intf>()` throws `di::Unresolved`** (message = interface ID + name) rather than
  returning a surprise null — you cannot accidentally deref an unwired dependency.
- **`require<Intf>()` + `requireAllResolved()`** — a component declares what it needs at startup;
  the host validates once and throws listing *every* missing `(id, name)`, turning a mid-run
  failure into a clear startup error.

Throwing at `resolve()`/startup catches the problem at *wiring* time, before first use — so we
don't need an abort-on-call sentinel proxy.

---

## 4. Scopes = `Registry` instances (RAII)

**A scope is just a `Registry` value.** Construction opens it, destruction closes it — no
explicit teardown, no bookkeeping even for a `dlopen`'d module that outlives the scope it
registered into.

```cpp
di::Registry r;                       // a hermetic scope
r.provide<IStorageEngine>(&fakeEngine, "lmdb");
// ... test against r ...
                                      // destructor cleans up; nothing leaks to global()
```

The default `di::global()` registry wires the running host; each test builds its own and lets it
die at end of scope. No teardown severing, no orphan roots — destructors do it. Isolation is
structural, which fits our test-first style directly.

---

## 5. Registration paths

- **Explicit** — the host/loader wires at startup: `di::global().provide<IStorageEngine>(&lmdb,
  "lmdb")`.
- **Self-register** — a `static di::Register<Intf>` for built-ins compiled in, kept alive by meson
  `link_whole` (the pattern the `rng` module already uses):
  `static di::Register<IRng> r{&philoxImpl, "philox"};`.
- **Dynamic** — a plugin registers into the host's registry after `dlopen` and `withdraw`s on
  unload; see [`plugins.md`](plugins.md).

No `__attribute__((constructor))` — registration is either explicit or a typed static, never
implicit macro side-effects.

---

## 6. Names — resolved on demand

The name is a lookup key: `resolve<Intf>(name)` runs when the consumer asks for the service.
Nothing patches slots as providers register, so there is no binding order to track and no replay.
The build/CLI sets the default name per interface (`-Dstorage=` → engine name, `-Drng=` → rng
name); `resolve<Intf>("")` returns the unnamed provider when there is exactly one, else the
selected default.

---

## 7. Threading

Registration/resolution is a **single-threaded bootstrap**: wire everything at startup on one
thread; during serving the registry is read-only, so concurrent `resolve()` is safe. Mutating
(`provide`/`withdraw`) after bootstrap is the caller's responsibility to serialize (only the
plugin host does it, on load/unload).

---

## 8. Design choices

The registry is deliberately small, typed, and explicit. The decisions:

- **Typed, not `void*`.** Providers are `Intf*`; `provide`/`resolve` are templates keyed on the
  interface type, so callers never handle untyped pointers. The single type-erasure point (the
  internal slot) is bounded by the ID↔type contract (§2).
- **Named binding.** Services are keyed by `(interface-id, name)`, so a consumer names *what* it
  needs, not *who* provides it — providers and consumers never reference each other.
- **Names select variants.** The optional name picks among implementations of one interface
  (engine `lmdb`/`memory`, rng `philox`/`threefry`); the build/CLI sets the default (§6).
- **Fail loud.** `resolve` throws and `requireAllResolved` validates at startup, so a missing
  dependency is a clear error at wiring time, never a silent null (§3).
- **Pull.** A consumer resolves when it needs a service; nothing patches slots as providers
  register, so there is no binding order to manage and no replay (§6).
- **Scopes are objects.** A `Registry` value *is* a scope — construction opens it, destruction
  closes it — so tests get isolation for free with no teardown bookkeeping (§4).
- **Explicit registration.** Providers register explicitly, or via a typed `static Register` for
  compiled-in built-ins (kept by `link_whole`) — no constructor-time macro side effects (§5).

Together these give the decoupling, variant-selection, fail-loud, and test-isolation of a full DI
container while keeping C++ type-safety and RAII and staying within the "C++ interfaces + `extern
"C"` factory" boundary.

---

## 9. `rng` — the first provider

The `rng` module is built on this registry. Each generator implements `IRng` (`prfs.rng/1`) and
self-registers as a di provider named for the algorithm (`di::Register<IRng>{&philox, "philox"}`,
one file per generator, kept alive by `link_whole`); `content` resolves the active one via
`di::resolve<IRng>(active())` (wrapped as `rng::activeRng()`). "Which generator" is a di *name*,
selected by `-Drng=` or `rng::setActive`. It is the container's first real consumer — exercising
self-registration, `link_whole`, and name-selection end to end.

---

## 10. Testing (test-first)

- `provide`/`resolve` round-trip; a wrong name throws `Unresolved`; `tryResolve` returns null.
- `require`/`requireAllResolved` reports *all* missing dependencies at once.
- Two `Registry` instances are isolated — a provider in one is invisible to the other.
- `names()`/`ids()`/`resolveAll()` introspection.
