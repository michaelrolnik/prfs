# prfs plugin API — design

prfs is a **host** for extensions. Rather than one hard-wired "plugin" shape, the system
defines a small set of **interfaces, each with a stable ID**; a plugin **exports** whatever
subset it implements (a protocol front-end, an rng generator, a storage engine — one plugin may
export several); and the host **discovers what a loaded plugin exports and wires it up**. The
host is also a **service container**: a component resolves a dependency by interface ID, whether
that dependency is a core service or something another plugin exported. It is COM-lite
capability discovery (`query` by ID) plus dependency injection — without COM's refcounting, since
the host owns all lifetimes.

> **Development principles (binding, design §13):** SOLID, test-first, clang-format.

---

## 1. Model

- **Interfaces have IDs.** Each pluggable interface carries a stable, versioned string ID
  (`"prfs.frontend/1"`). Bumping an interface = a new ID, so old and new never silently mix.
- **Plugins export interfaces.** A plugin's root object answers "which interface IDs do you
  provide?" and hands out a typed pointer per ID (`query`). A plugin may export many.
- **The host discovers and acts.** On load it asks the plugin what it exports and does the right
  thing per interface: a front-end is started, an rng is registered, an engine is made available.
- **The host is a DI container.** `IHost::resolve(id)` returns a service by interface ID — a core
  one (the store, the logger) or one another plugin exported — so plugins compose without knowing
  each other concretely.

```
prfs host (executable)
  core     : IPrfs store · content · rng · logger (spdlog) · config (CLI11)
  container: resolve(interfaceId) → service*        (core + plugin-exported)
  loader   : discover → load → create(host) → for each exported interface: wire it
      │
      ├── plugins/nfsv3.so   exports IFrontend
      ├── plugins/mount.so   exports IFrontend
      ├── plugins/fastrng.so exports IRng
      └── plugins/rocks.so   exports IStorageEngine
```

---

## 2. The ABI boundary

**C++ interfaces reached through an `extern "C"` factory** (chosen: least boilerplate, reuses
`IPrfs`/`IKvStore` directly). The interfaces are ordinary C++ (stable Itanium vtable ABI on
Linux across gcc/clang); the entry points have C linkage so `dlsym` finds unmangled symbols. An
**ABI version** is checked at load; a mismatch is refused. Constraint: plugins built against a
compatible C++ stdlib — in-tree plugins satisfy this by construction.

```cpp
extern "C" {
uint32_t prfs_abi(void);                            // must equal prfs::plugin::ABI
prfs::plugin::IPlugin* prfs_plugin_create(prfs::plugin::IHost*);
void prfs_plugin_destroy(prfs::plugin::IPlugin*);   // matching deleter (same .so)
}
```

---

## 3. Interfaces — `include/prfs/plugin.hpp`

```cpp
namespace prfs::plugin {

inline constexpr uint32_t ABI = 1;

//  Typed capability query: `auto* fe = query<IFrontend>(*plugin);`  (nullptr if absent)
template <class I> I* query(class IPlugin& p);
template <class I> I* resolve(class IHost& h);

//  The DI container the host lends every plugin (and its own core services use).
class IHost {
public:
    virtual ~IHost() = default;

    //  core services
    virtual IPrfs& fs() = 0;                                            // the filesystem
    virtual size_t read(Node file, uint64_t off, char* out, size_t len) = 0; // file bytes
    virtual spdlog::logger& log() = 0;                                 // shared logger
    virtual std::string option(std::string_view key) const = 0;        // parsed CLI/config

    //  dependency injection: fetch any service by interface ID (core or exported)
    virtual void* resolve(std::string_view interfaceId) = 0;
};

//  A loaded plugin's root: identity + capability discovery.
class IPlugin {
public:
    virtual ~IPlugin() = default;

    virtual char const* name() const = 0;                       // "nfsv3"
    virtual char const* version() const = 0;
    virtual std::vector<std::string_view> interfaces() const = 0;   // exported interface IDs
    virtual void* query(std::string_view interfaceId) = 0;          // → interface* or nullptr
};

// ── the exported interfaces (each with a stable, versioned ID) ──────────────

struct Option { std::string name, help, def; bool flag = false; };   // a CLI arg

//  A protocol front-end (nfsv3, mount, …).
struct IFrontend {
    static constexpr std::string_view ID = "prfs.frontend/1";
    virtual ~IFrontend() = default;
    virtual std::vector<Option> options() const { return {}; }   // CLI args it adds
    virtual Error start() = 0;                                    // bind/listen; owns its threads
    virtual void stop() = 0;
};

//  A counter-based random generator (the rng registry's plugin face).
struct IRng {
    static constexpr std::string_view ID = "prfs.rng/1";
    virtual ~IRng() = default;
    virtual char const* rngName() const = 0;
    virtual void gen4(uint32_t const ctr[4], uint32_t const key[2], uint32_t out[4]) const = 0;
};

//  A storage engine behind IKvStore.
struct IStorageEngine {
    static constexpr std::string_view ID = "prfs.engine/1";
    virtual ~IStorageEngine() = default;
    virtual char const* engineName() const = 0;
    virtual std::unique_ptr<IKvStore> open(std::string const& path, bool clean) = 0;
};

} // namespace prfs::plugin
```

`query`/`resolve` are thin typed wrappers over the string-ID lookup:
`return static_cast<I*>(p.query(I::ID));`. Type safety rests on the ID↔type contract — the whole
point of versioning the ID.

---

## 4. Discovery + wiring (the loader as DI container)

```
load(path):
    h = dlopen(path)
    if dlsym(h,"prfs_abi")() != ABI: log+skip
    plug = prfs_plugin_create(host)
    for id in plug->interfaces():
        switch id:
          "prfs.frontend/1": fe = query<IFrontend>(*plug)
                             register fe->options() with CLI11 as --<plug.name>.<opt>
                             frontends.push_back(fe)            # start() after CLI parse
          "prfs.rng/1":      r  = query<IRng>(*plug); rng::add(r->rngName(), bridge(r))
          "prfs.engine/1":   e  = query<IStorageEngine>(*plug); engines.add(e)
          else:              log "unknown interface {id}"       # forward-compatible: ignore
    host.container.publish(plug)                                # its interfaces become resolvable
```

- **Order:** load all → collect front-end `options()` → parse CLI → `start()` front-ends. (A
  plugin's options are only known once loaded, so CLI parsing follows discovery.)
- **DI:** each exported interface is published into the container, so another component gets it
  via `resolve<IRng>(host)` / `host.resolve("prfs.rng/1")` — no direct linkage between plugins.
- **Forward-compatible:** an interface ID the host doesn't know is logged and ignored, not fatal.

Built-in providers use the very same interfaces without `dlopen`: the LMDB/mem engines are
`IStorageEngine`s, the rng generators are `IRng`s, all published into the container at startup.

---

## 5. Lifetime & threading

- **Lifetime:** the host owns each `IPlugin` (`create`→`destroy`); exported interface pointers are
  valid until `destroy`. No refcounting — the host is the single owner (simpler than COM).
- **Threading:** an `IFrontend` brings its own MT server (own thread pool/event loop in `start()`,
  joined in `stop()`) — deliberately, given the NFS analysis (naive `rpcgen` is single-threaded,
  todo L2). `IHost` methods are callable from plugin threads: `IPrfs` is concurrency-safe (§8,
  LMDB MVCC), the logger is thread-safe, `read` is a pure lookup.

---

## 6. Build, layout & testing

```
include/prfs/plugin.hpp     the interfaces + IDs + extern "C" ABI
src/host/                   loader + container + main (CLI11 + spdlog)
plugins/null/               a test plugin exporting IFrontend (+ a test IRng)
plugins/nfsv3/  …           real front-ends → <name>.so
```

- **meson:** `-Dplugins` option; the host links `libprfs` + content + rng + CLI11 + spdlog; each
  plugin is a `shared_module`. **spdlog + CLI11** are git submodules (matching the vendoring
  pattern). Logging/CLI live only in the host+plugin layer; leaf libraries stay free of them.
- **Testing (test-first):** a `null` plugin that exports `IFrontend` whose `start()` runs a
  scripted set of `IHost`/`IPrfs` ops and logs — proves discovery, `query`, the container, the
  ABI check, and lifecycle end to end, built both **in-tree** (registry) and as a **.so** (dlopen).
  Also a trivial `IRng` export to prove non-front-end interfaces wire through. Loader tests:
  ABI-mismatch refused, unknown interface ignored, multiple plugins, isolated `start()` failure.

---

## 7. Open questions

- **Filehandle ↔ node.** An NFS `IFrontend` must turn a filehandle `(nodeID, snapId)` back into a
  `Node`; today handles come only from traversal, so L2 wants `IPrfs::nodeById(id, snap)` (plus
  the T1/T4/T5 carve-outs: `..` via-parent, cookie mapping, `.snapshot` listing).
- **Interface evolution.** Versioned IDs handle breaking changes; whether to also support
  additive minor versions (a plugin exporting `prfs.frontend/1` on a `/2` host) is a policy to
  settle — start strict (exact match).
- **Interface catalogue.** Start with `IFrontend`, `IRng`, `IStorageEngine`; likely follow-ons:
  content generator, auth, metrics. Each is just another ID.
- **Config surface.** CLI11 flags vs config file vs both; per-plugin option namespacing.
