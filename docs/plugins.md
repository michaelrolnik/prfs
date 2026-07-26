# prfs plugin API — design

prfs is a **host** for extensions. Rather than one hard-wired "plugin" shape, the system defines
a small set of **interfaces, each with a stable ID**; a plugin **provides** whatever subset it
implements (a protocol front-end, an rng generator, a storage engine — one plugin may provide
several) into the shared **DI registry** ([`di.md`](di.md)); and the host **resolves** from that
registry — starting every provided front-end, using the selected engine, letting `content` pick
the active rng. One wiring mechanism (the `di::Registry`) for built-ins and plugins alike.

> **Development principles (binding, design §13):** SOLID, test-first, clang-format.

---

## 1. Model

- **Interfaces have IDs** — each pluggable interface carries a stable versioned ID
  (`"prfs.frontend/1"`); it is the `di` key. Bumping the interface = a new ID.
- **Plugins provide into the registry** — on load a plugin registers its implementations:
  `reg.provide<IFrontend>(myServer)`, `reg.provide<IRng>(myGen, "fastrng")`. Built-ins do the
  same at startup; a plugin is just late registration after `dlopen`.
- **The host resolves and acts** — `reg.resolveAll<IFrontend>()` → start each; the store opens via
  `resolve<IStorageEngine>(storageName)`; `content` uses `resolve<IRng>(rngName)`. An interface
  nobody provided simply isn't there (`requireAllResolved` reports it loudly at startup).
- **DI, not bespoke discovery** — there is no plugin-specific `query`/`interfaces` protocol; the
  registry *is* the discovery surface (`ids`/`names`/`resolveAll`, [`di.md`](di.md)).

```
prfs host (executable)
  core     : IPrfs store · content · rng · logger (spdlog) · config (CLI11)
  registry : di::Registry  —  (interface-id, name) → provider   (built-ins + plugins)
  loader   : dlopen → abi-check → register(host) → resolveAll<IFrontend> → start
      │
      ├── plugins/nfsv3.so   provides IFrontend "nfsv3"
      ├── plugins/mount.so   provides IFrontend "mount"
      ├── plugins/fastrng.so provides IRng "fastrng"
      └── plugins/rocks.so   provides IStorageEngine "rocksdb"
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
uint32_t prfs_abi(void);                                // must equal prfs::plugin::ABI
prfs::plugin::IPlugin* prfs_plugin_create(prfs::plugin::IHost&); // provides into host.registry()
void prfs_plugin_destroy(prfs::plugin::IPlugin*);       // withdraws its providers, then frees
}
```

---

## 3. Interfaces — `include/prfs/plugin.hpp`

The container itself is the generic `di::Registry` ([`di.md`](di.md)); this header adds the
prfs-specific interfaces and the plugin entry contract.

```cpp
namespace prfs::plugin {

inline constexpr uint32_t ABI = 1;

//  Services the host lends every plugin, plus the shared DI registry. Core
//  services are also published in the registry, but the hot ones are direct.
class IHost {
public:
    virtual ~IHost() = default;

    virtual di::Registry& registry() = 0;                              // provide/resolve services
    virtual IPrfs& fs() = 0;                                            // the filesystem
    virtual size_t read(Node file, uint64_t off, char* out, size_t len) = 0; // file bytes
    virtual spdlog::logger& log() = 0;                                 // shared logger
    virtual std::string option(std::string_view key) const = 0;        // parsed CLI/config
};

//  A loaded plugin's owning root: identity + the objects it provided. Its
//  create() registered them into host.registry(); destroy() withdraws + frees.
class IPlugin {
public:
    virtual ~IPlugin() = default;
    virtual char const* name() const = 0;      // "nfsv3"
    virtual char const* version() const = 0;
};

// ── the providable interfaces (each with a stable, versioned ID = its di key) ──

struct Option { std::string name, help, def; bool flag = false; };   // a CLI arg

//  A protocol front-end. The di name is the protocol, e.g. "nfsv3" / "mount".
struct IFrontend {
    static constexpr std::string_view ID = "prfs.frontend/1";
    virtual ~IFrontend() = default;
    virtual std::vector<Option> options() const { return {}; }   // CLI args it adds
    virtual Error start() = 0;                                    // bind/listen; owns its threads
    virtual void stop() = 0;
};

//  A counter-based random generator. The di name is the generator, e.g. "philox" (di.md §9).
struct IRng {
    static constexpr std::string_view ID = "prfs.rng/1";
    virtual ~IRng() = default;
    virtual void gen4(uint32_t const ctr[4], uint32_t const key[2], uint32_t out[4]) const = 0;
};

//  A storage engine behind IKvStore. The di name is the engine, "lmdb" / "memory".
struct IStorageEngine {
    static constexpr std::string_view ID = "prfs.engine/1";
    virtual ~IStorageEngine() = default;
    virtual std::unique_ptr<IKvStore> open(std::string const& path, bool clean) = 0;
};

} // namespace prfs::plugin
```

The variant of each interface (which protocol / generator / engine) is the **`di` name**, so a
plugin providing two rng generators just `provide<IRng>(a,"x")` / `provide<IRng>(b,"y")`.

---

## 4. Loading + wiring

```
load(path):
    h = dlopen(path)
    if dlsym(h,"prfs_abi")() != ABI: log+skip; return
    plug = prfs_plugin_create(host)          # plugin provides its interfaces into host.registry()
    plugins.push_back(plug)                   # own it; destroy() (→ withdraw) on shutdown

after all loaded:
    for fe in reg.resolveAll<IFrontend>():    # built-in + plugin front-ends, uniformly
        register fe->options() with CLI11 as --<name>.<opt>
    parse CLI
    reg.requireAllResolved()                  # fail loud: any declared dep still missing
    for fe in reg.resolveAll<IFrontend>(): fe->start()
```

- **No bespoke discovery** — a plugin simply `provide`s; the registry is the discovery surface.
  `resolveAll<IFrontend>()` yields built-in and plugin front-ends the same way; the store opens
  via `resolve<IStorageEngine>(storageName)`; `content` uses `resolve<IRng>(rngName)`.
- **Order** — load all → collect `options()` → parse CLI → `requireAllResolved()` → `start()`.
  (A plugin's options are only known once it has registered, so CLI parse follows loading.)
- **Fail loud, not silent** — a missing dependency surfaces at `requireAllResolved()` with its
  `(id, name)`, and `resolve()` throws rather than returning null (di.md §3).
- **Lifetime** — the host owns each `IPlugin`; on shutdown it `stop()`s front-ends, then
  `prfs_plugin_destroy` (which `withdraw`s the plugin's providers so no dangling entry survives
  `dlclose`).

Built-in providers use the very same registry without `dlopen`: the LMDB/mem engines register as
`IStorageEngine` names, the rng generators as `IRng` names, at startup.

---

## 5. Lifetime & threading

- **Lifetime:** the host owns each `IPlugin` (`create`→`destroy`); the interfaces it provided into
  the registry are valid until `destroy`, which `withdraw`s them first. No refcounting — the host
  is the single owner, so a `dlclose` can't leave a dangling registry entry.
- **Threading:** an `IFrontend` brings its own MT server (own thread pool/event loop in `start()`,
  joined in `stop()`) — deliberately, given the NFS analysis (naive `rpcgen` is single-threaded,
  todo L2). `IHost` methods are callable from plugin threads: `IPrfs` is concurrency-safe (§8,
  LMDB MVCC), the logger is thread-safe, `read` is a pure lookup.

---

## 6. Build, layout & testing

```
include/prfs/di.hpp         the di::Registry (di.md)
include/prfs/plugin.hpp     the prfs interfaces + IDs + extern "C" ABI
src/host/                   loader + di wiring + main (CLI11 + spdlog)
plugins/null/               a test plugin providing IFrontend (+ a test IRng)
plugins/nfsv3/  …           real front-ends → <name>.so
```

- **meson:** `-Dplugins` option; the host links `libprfs` + content + rng + CLI11 + spdlog; each
  plugin is a `shared_module`. **spdlog + CLI11** are git submodules (matching the vendoring
  pattern). Logging/CLI live only in the host+plugin layer; leaf libraries stay free of them.
- **Testing (test-first):** a `null` plugin that provides `IFrontend` whose `start()` runs a
  scripted set of `IHost`/`IPrfs` ops and logs — proves `provide`/`resolveAll`, the ABI check, and
  the lifecycle end to end, built both **in-tree** (direct `provide`) and as a **.so** (dlopen).
  Also a trivial `IRng` provider to prove non-front-end interfaces wire through. Registry-level
  behaviour (round-trip, names, `requireAllResolved`, isolation) is tested in `di` (di.md §10).
  Loader tests: ABI-mismatch refused, multiple plugins, isolated `start()` failure, clean
  `withdraw` on unload.

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
