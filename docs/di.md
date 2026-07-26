# prfs DI — the service registry

A small, typed, C++-native **dependency-injection container**: providers register services keyed
by `(interface-id, flavor)`; consumers resolve by interface (with an optional flavor). It is the
substrate the plugin host ([`plugins.md`](plugins.md)) and the built-in extension points (rng
generators, storage engines) all share — one wiring mechanism instead of three.

It follows the well-worn **service-locator / DI** pattern (named binding, variant *flavors*,
fail-loud on unresolved, per-scope isolation, introspection) but re-expressed idiomatically in
C++: **typed** (no `void*` vtable patching), **RAII** (scopes are objects, no teardown
machinery), and **explicit** (no `__attribute__((constructor))` magic, no C ABI). §8 lays out the
choices against a C-style runtime-linker container and why we made them.

> **Development principles (binding, design §13):** SOLID, test-first, clang-format.

---

## 1. Concepts

- **Interface** — a C++ abstract type carrying a stable, versioned ID:
  `static constexpr std::string_view ID = "prfs.rng/1"`. Bumping the interface = a new ID.
- **Flavor** — an optional secondary key that selects a *variant* among providers of the same
  interface: `engine → "lmdb"|"memory"`, `rng → "philox"|"threefry"`, `frontend →
  "nfsv3"|"mount"`. Empty flavor = the single/default provider. Flavors are how `-Dstorage=` /
  `-Drng=` / a `--flavor` flag choose an implementation.
- **Provider** — an implementation registered under `(ID, flavor)`. The registry stores a
  type-erased pointer tagged with its interface ID; the typed accessors cast it back.
- **Registry** — owns a set of providers. The process has one default registry (`di::global()`);
  a test constructs its own (an ordinary object) for isolation. Registries do not inherit from
  one another.

---

## 2. API — `include/prfs/di.hpp`

```cpp
namespace prfs::di {

struct Unresolved : std::runtime_error {          // thrown by resolve() when absent
    Unresolved(std::string_view id, std::string_view flavor);
};

class Registry {
public:
    template <class I> void provide(I* impl, std::string_view flavor = "");
    template <class I> void withdraw(std::string_view flavor = "");   // for plugin unload

    template <class I> I& resolve(std::string_view flavor = "") const;    // throws Unresolved
    template <class I> I* tryResolve(std::string_view flavor = "") const; // nullptr if absent
    template <class I> std::vector<I*> resolveAll() const;               // every flavor of I

    bool has(std::string_view id, std::string_view flavor = "") const;
    std::vector<std::string> flavors(std::string_view id) const;  // variants of one interface
    std::vector<std::string> ids() const;                        // all registered interfaces

    //  fail-loud startup validation (see §3)
    template <class I> void require(std::string_view flavor = "");
    void requireAllResolved() const;   // throws once, listing every missing (id, flavor)
};

Registry& global();                    // default registry — a function-local static (see §2.1)

//  convenience wrappers over global()
template <class I> void provide(I*, std::string_view flavor = "");
template <class I> I& resolve(std::string_view flavor = "");
template <class I> I* tryResolve(std::string_view flavor = "");

} // namespace prfs::di
```

### 2.1 `global()` is a function, not a variable (SIOF-safe)

The default registry is reached only through `global()`, implemented as a **function-local
static** (Meyers singleton):

```cpp
Registry& global() { static Registry r; return r; }
```

This is required, not cosmetic. Built-in providers self-register via `static di::Register<I>`
objects that run at **static-init time** and call `global().provide(...)`. A namespace-scope
`Registry` variable could be constructed *after* some other TU's `Register` runs — the
static-initialization-order fiasco. A function-local static is constructed on **first call**, so
the registry always exists before the first `provide()`. (This is exactly why `rng.cpp`'s
`registry()` is a function-local static.)

Storage is a `map<pair<string,string>, Slot>` where `Slot = { void* impl; std::string_view id; }`;
`provide<I>` records `{impl, I::ID}` at key `(I::ID, flavor)`, `resolve<I>` looks up `(I::ID,
flavor)` and `static_cast<I*>`s. Type safety rests on the **ID↔type contract** — the usual
convention for a type-erased registry, made explicit by versioning the ID.

---

## 3. Fail-loud (unresolved dependencies)

One classic way to make a missing dependency safe is to point an unresolved interface at a
sentinel vtable that `abort()`s with a backtrace when called, instead of leaving a null that
segfaults. We get the same guarantee more simply, because we *pull*:

- **`resolve<I>()` throws `di::Unresolved`** (message = interface ID + flavor) rather than
  returning a surprise null — you cannot accidentally deref an unwired dependency.
- **`require<I>()` + `requireAllResolved()`** — a component declares what it needs at startup;
  the host validates once and throws listing *every* missing `(id, flavor)`, turning a mid-run
  failure into a clear startup error.

Throwing at `resolve()`/startup catches the problem at *wiring* time, before first use — so we
don't need an abort-on-call sentinel proxy.

---

## 4. Scopes = `Registry` instances (RAII)

A C-style container needs explicit `scope_create`/`destroy`/`teardown` (and orphan-root
gymnastics) because its registration is global and constructor-time, and a `dlopen`'d module can
outlive the scope that adopted it. We sidestep all of that: **a scope is just a `Registry`
value.**

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
- **Self-register** — a `static di::Register<I>` for built-ins compiled in, kept alive by meson
  `link_whole` (the pattern the `rng` module already uses):
  `static di::Register<IRng> r{&philoxImpl, "philox"};`.
- **Dynamic** — a plugin registers into the host's registry after `dlopen` and `withdraw`s on
  unload; see [`plugins.md`](plugins.md).

No `__attribute__((constructor))` — registration is either explicit or a typed static, never
implicit macro side-effects.

---

## 6. Flavors — pull, not push

A **push**-binding container patches import-slots as providers arrive, so selecting a flavor means
*refining already-bound imports* and *replaying* a remembered flavor list — real machinery. Ours
is **pull**: `resolve<I>(flavor)` is just a lookup key, chosen when the consumer asks. Simpler, and
it needs no replay. The build/CLI sets the default flavor per interface (`-Dstorage=` → engine
flavor,
`-Drng=` → rng flavor); `resolve<I>("")` returns the unflavored provider when there is exactly
one, else the selected default.

---

## 7. Threading

Registration/resolution is a **single-threaded bootstrap**: wire everything at startup on one
thread; during serving the registry is read-only, so concurrent `resolve()` is safe. Mutating
(`provide`/`withdraw`) after bootstrap is the caller's responsibility to serialize (only the
plugin host does it, on load/unload).

---

## 8. Design choices (vs a C-style runtime linker)

| A C-style runtime-linker container     | prfs `di`                                  |
| -------------------------------------- | ------------------------------------------ |
| named, decoupled import/export binding | ✅ `(interface-id, flavor)` keyed registry |
| flavors (variant selection)            | ✅ kept — pull-side lookup key             |
| unresolved sentinel (fail loud)        | ✅ `resolve` throws · `requireAllResolved` |
| scopes (test isolation)                | ✅ a `Registry` object (RAII)              |
| introspection / checkup                | ✅ `ids`/`flavors`/`has`/`resolveAll`      |
| C ABI, `void*` vtable patching         | ❌ keep C++ interfaces (type-safe)         |
| `__attribute__((constructor))` macros  | ❌ explicit or typed `Register`            |
| dlopen/orphan-root/teardown machinery  | ❌ RAII + the plugin host handle lifetime  |
| push-binding + flavor replay           | ❌ pull (`resolve` on demand)              |

Net: the decoupling + flavors + fail-loud + test-scopes of a runtime-linker DI, at a fraction of
the code, with C++ type-safety and RAII intact — without undoing the "C++ interfaces + `extern
"C"` factory" boundary decision.

---

## 9. Relationship to the `rng` registry

`rng`'s `name → Gen4` map is a special case of this: interface `IRng`, flavor = the generator
name. Once `di` lands, rng generators become `IRng` providers and `rng::activeFn()` becomes
`di::resolve<IRng>(activeFlavor)`. The migration is optional — the current `rng` registry already
embodies the pattern (self-registering, `link_whole`, no central switch), so it is the working
prototype of the `di` container.

---

## 10. Testing (test-first)

- `provide`/`resolve` round-trip; a wrong flavor throws `Unresolved`; `tryResolve` returns null.
- `require`/`requireAllResolved` reports *all* missing dependencies at once.
- Two `Registry` instances are isolated — a provider in one is invisible to the other.
- `flavors()`/`ids()`/`resolveAll()` introspection.
