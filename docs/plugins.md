# prfs plugin API — design

prfs is a **host** for protocol front-ends. The core (the versioned filesystem `IPrfs`, the
content provider, the rng) is served to the outside world by **plugins** — `nfsv3`, `nfsv4`,
`mount`, and whatever else — loaded at runtime behind a stable API. A front-end is *not* baked
into the core; it is a `.so` the host discovers, loads, and starts.

> **Development principles (binding, design §13):** SOLID, test-first, clang-format. Same rules
> as the rest of the tree.

---

## 1. Architecture

```
prfs host (executable)
  core   : IPrfs store (lmdb|memory) · content provider · rng
  infra  : configuration (CLI11) · logging (spdlog)
  loader : discover → load → ABI-check → create(host) → start();  stop()+unload on shutdown
      │
      ├── plugins/nfsv3.so     serves NFSv3 over RPC
      ├── plugins/mount.so     the MOUNT protocol
      ├── plugins/nfsv4.so     NFSv4 (subset)
      └── …                    any other front-end
```

Layering is strict: `libprfs` knows nothing about plugins; the **host** owns the store and the
loader; **plugins** depend only on the public prfs headers (`IPrfs`, `content`, `rng`) plus the
plugin API header — never on engine internals. A plugin can be developed, built, and shipped
without touching the core.

---

## 2. The boundary (ABI)

**C++ abstract interfaces reached through an `extern "C"` factory.** The interfaces are ordinary
C++ (so a plugin uses `IPrfs` directly, no C shim), while the *entry points* have C linkage so
`dlsym` finds unmangled, stable symbols. An **ABI version** integer is checked at load; a
mismatch is refused with a clear log line.

- On Linux the Itanium C++ ABI (vtable layout) is stable across gcc/clang, so C++ virtual calls
  work across a `.so` boundary. **Constraint:** plugins are built against a compatible C++
  standard library (same `libstdc++`/`libc++`). In-tree plugins satisfy this by construction;
  the constraint is documented for out-of-tree ones. (If toolchain-independent third-party
  binaries ever become a goal, a pure C ABI is the fallback — but that is not today's need.)

---

## 3. Interfaces — `include/prfs/plugin.hpp`

```cpp
namespace prfs::plugin {

inline constexpr uint32_t ABI = 1;   // bump on ANY change to the interfaces below

//  Services the host lends a plugin. Minimal and stable; grows only by ABI bump.
class Host {
public:
    virtual ~Host() = default;

    virtual IPrfs& fs() = 0;                                   // the filesystem namespace
    virtual size_t read(Node file, uint64_t off, char* out, size_t len) = 0;  // file bytes
    virtual content::ContentConfig contentConfig() const = 0; // FS content policy (FSSTAT etc.)
    virtual spdlog::logger& log() = 0;                        // shared structured logger
    virtual std::string option(std::string_view key) const = 0; // plugin config (CLI/file)
};

//  A CLI option a plugin contributes. Kept library-agnostic (no CLI11 type in
//  the ABI): the host registers these with its parser, namespaced by plugin.
struct Option {
    std::string name;   // "port"          → parsed as --<plugin>.<name>
    std::string help;
    std::string def;    // default value ("" if none)
    bool flag = false;  // true ⇒ a boolean switch (no value)
};

//  A protocol front-end. Lifecycle: create → options → start → (serve…) → stop → destroy.
class Plugin {
public:
    virtual ~Plugin() = default;

    virtual char const* name() const = 0;     // "nfsv3"
    virtual char const* version() const = 0;
    virtual std::vector<Option> options() const { return {}; } // CLI args this plugin adds
    virtual Error start() = 0;                 // bind / listen / register; OK or an error
    virtual void stop() = 0;                   // graceful shutdown; join threads
};

} // namespace prfs::plugin

//  Every plugin .so exports these with C linkage (unmangled → dlsym-able):
extern "C" {
uint32_t prfs_plugin_abi(void);                          // must return prfs::plugin::ABI
prfs::plugin::Plugin* prfs_plugin_create(prfs::plugin::Host*);
void prfs_plugin_destroy(prfs::plugin::Plugin*);         // matching deleter (same .so)
}
```

- **`create`/`destroy` pair** so the plugin's `.so` owns its allocation — never `delete` an
  object across a `.so` boundary.
- **`Host` outlives the plugin** and is passed to `create`; the plugin must not retain it past
  `destroy`.
- **`Host::read` hides content vs literal:** the host resolves a `READ` — procedural bytes from
  the content provider (config + `nodeID` seed) or a stored literal blob — so the plugin only
  sees "give me bytes of this file".

---

## 4. The loader

Built-in **and** dynamic, both through the same `Plugin`/`Host` interface:

- **Built-in:** a static registry (self-registration) for plugins compiled into the host —
  e.g. a `null` front-end used in tests. No `dlopen`.
- **Dynamic:** scan a plugin directory (from CLI/config), `dlopen` each candidate, `dlsym`
  `prfs_plugin_abi` and check it equals `ABI` (else skip + log), then `prfs_plugin_create(host)`
  and `start()`.

Several plugins run at once (e.g. `nfsv3` + `mount`). The host owns their lifetimes; on shutdown
it `stop()`s in reverse order, `prfs_plugin_destroy`s, then `dlclose`s. A failed `start()` is
logged and that plugin is dropped without taking the host down.

**Plugins contribute CLI args.** Because a plugin's options are only known once it is loaded, the
flow is: discover → load → `create(host)` → collect each plugin's `options()` → register them
with CLI11 (namespaced `--<plugin>.<name>`, e.g. `--nfsv3.port=2049`) → parse → `start()`. A
plugin reads its parsed values back through `Host::option("port")`; the ABI stays
CLI-library-agnostic (`Option` is a plain descriptor, no CLI11 type crosses the boundary).

## 4a. The light tier — registries (rng, engines)

Not every extension needs the full `Plugin` lifecycle. A **generator** or an **engine** is just a
named factory, so those use a lighter mechanism: a **name-keyed registry** where each
implementation **self-registers** (a `static Register` object; built-ins are kept alive with
meson `link_whole`, and a dynamically-loaded `.so` registers the same way on load). `rng` is the
first example (`prfs::rng::add/get/names`, one file per generator, no central switch — open/
closed). Storage engines (`IKvStore`) could adopt the same registry later. The two tiers:

- **Front-end plugins** — heavy: own a protocol, threads, sockets; `start()`/`stop()`; loaded by
  the plugin host.
- **Registries** — light: a name → factory table for generators/engines; self-registering,
  statically linked or dlopen'd, no per-instance lifecycle.

---

## 5. Threading

The plugin API does not mandate a threading model: a front-end brings **its own** MT server
(thread pool + event loop) — spun up in `start()`, joined in `stop()`. This is deliberate given
the NFS analysis (naive `rpcgen` dispatch is single-threaded, todo L2): a plugin is free to run
its own thread-pool TCP server, or wrap NFS-Ganesha, without the host imposing a loop.

`Host` methods are callable from plugin threads: `IPrfs` is concurrency-safe by design (§8 —
LMDB MVCC, single writer / many readers), the logger is thread-safe (spdlog), and `read` is a
pure content lookup. Writes serialize at the single writer; reads scale — fine for a test target.

---

## 6. Build & layout

```
include/prfs/plugin.hpp     the API (public header)
src/host/                   the loader + main (CLI11 + spdlog)
plugins/null/               built-in test front-end (also buildable as .so)
plugins/nfsv3/  …           real front-ends (each its own meson subdir → <name>.so)
```

- **meson:** `-Dplugins` option; the host links `libprfs` + content + rng + CLI11 + spdlog; each
  plugin is a `shared_module`. Plugins added to the clang-format gate like everything else.
- **spdlog + CLI11** are added as **git submodules** (matching how LMDB/Lua/sol2/Random123 are
  vendored). Logging and CLI live in the host/plugins layer only — never in the leaf libraries.

---

## 7. Testing (test-first)

- A **`null` front-end**: `start()` runs a scripted set of `Host`/`IPrfs` operations and logs,
  then returns — no real network protocol. Proves host↔plugin wiring, the ABI check, the
  create/start/stop/destroy lifecycle, and every `Host` service end to end. Built both **in-tree**
  (registry path) and as a **`.so`** (dlopen path).
- **Loader tests:** ABI-mismatch refused; missing/って bad symbol handled; multiple plugins
  started/stopped in order; a failing `start()` is isolated.

---

## 8. Open questions

- **Filehandle ↔ node.** An NFS front-end must turn a filehandle `(nodeID, snapId)` back into a
  `Node`. Today handles come only from traversal (`rwRoot`/`lookup`/`readdir`); L2 will want a
  small store addition — `IPrfs::nodeById(id, snap)` — plus the T1/T4/T5 carve-outs (`..`
  via-parent, cookie mapping, `.snapshot` listing). Captured here; belongs to L2.
- **Shared executor.** Start with plugin-owned threads; a host-provided thread pool / event loop
  is a later `Host` addition (ABI bump) if front-ends want to share one.
- **Config surface.** CLI11 flags vs a config file vs both, and how per-plugin options are
  namespaced (`--nfsv3.port=2049`). Decide with the host.
- **Other extension points.** Storage engines (`IKvStore`) and content generators already have
  clean interfaces and *could* become plugins under the same pattern — out of scope now.
