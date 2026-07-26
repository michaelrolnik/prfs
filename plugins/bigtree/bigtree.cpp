// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  bigtree — a native store-builder front-end. On start() it builds a large,
//  RANDOMLY-SHAPED synthetic filesystem directly into the host's store, then
//  idles (it serves no network). It is the C++ twin of examples/bigtree.lua:
//  same irregular-but-reproducible tree, same randomized heavy-tailed file
//  sizes, same daily snapshot rounds — but it calls IPrfs directly instead of
//  going through the Lua interpreter, so it builds a multi-TiB tree far faster.
//
//  Because prfs GENERATES content (never stores it), the whole tree is a few MB
//  of metadata plus a generator. Pair with nfsv3 to build-and-serve in one shot:
//
//    prfs-host --store /tmp/prfs-big --clean \
//      --plugin bigtree.so --plugin nfsv3.so --port 20490 \
//      --set bigtree.total=1T --set bigtree.seed=42
//
//  bigtree is listed first so it registers (and thus start()s) before nfsv3 and
//  the tree exists before the first client connects. It builds only into an
//  empty store unless bigtree.force is set (so a restart re-serves, not rebuilds).
//
#include "prfs/plugin.hpp"
#ifdef PRFS_WITH_CONTENT
#include "prfs/content.hpp"
#endif

#include <spdlog/logger.h>

#include <cmath>
#include <cstdint>
#include <functional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace prfs;
using namespace prfs::plugin;

//  Parse a byte count with an optional binary suffix: 1024, 512K, 8M, 4G, 1T, 2P.
uint64_t parseSize(std::string const& s) {
    if (s.empty()) {
        return 0;
    }
    char suffix = s.back();
    uint64_t mult = 1;
    switch (suffix) {
    case 'k':
    case 'K':
        mult = 1ull << 10;
        break;
    case 'm':
    case 'M':
        mult = 1ull << 20;
        break;
    case 'g':
    case 'G':
        mult = 1ull << 30;
        break;
    case 't':
    case 'T':
        mult = 1ull << 40;
        break;
    case 'p':
    case 'P':
        mult = 1ull << 50;
        break;
    default:
        break;
    }
    std::string num = (mult == 1) ? s : s.substr(0, s.size() - 1);
    return uint64_t(std::stoull(num) * mult);
}

std::string human(uint64_t b) {
    char const* u[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double v = double(b);
    int i = 0;
    while (v >= 1024.0 && i < 5) {
        v /= 1024.0;
        i++;
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.1f %s", v, u[i]);
    return buf;
}

struct BigTree : IService {
    IHost& host;

    explicit BigTree(IHost& h)
        : host(h) {}

    std::vector<Option> options() const override {
        return {
            {"bigtree.depth", "max tree depth", "4", false},
            {"bigtree.dirs", "up to this many subdirs per dir", "5", false},
            {"bigtree.files", "up to this many files per dir", "8", false},
            {"bigtree.total", "target total logical size (K/M/G/T/P suffix ok)", "1T", false},
            {"bigtree.seed", "RNG seed — same seed builds the same tree", "42", false},
            {"bigtree.snapshots", "daily snapshot rounds after the base snapshot", "3", false},
            {"bigtree.force", "build even if the store is already populated", "", true},
        };
    }

    //  Read a bigtree.* option, falling back to `def` when unset.
    std::string opt(char const* key, char const* def) const {
        std::string v = host.option(key);
        return v.empty() ? std::string(def) : v;
    }

    long long optInt(char const* key, char const* def) const { return std::stoll(opt(key, def)); }

    Error start() override {
        try {
            build();
        } catch (std::exception const& e) {
            host.log().error("bigtree: {}", e.what());
            return Error::INVAL;
        }
        return Error::OK;
    }

    void stop() override {}

    void build() {
        IPrfs& fs = host.fs();
        //  Single-writer: a bulk mutation must hold the store exclusively.
        std::unique_lock<std::shared_mutex> lk(host.storeMutex());

        Node root = fs.rwRoot();
        bool force = !host.option("bigtree.force").empty();
        if (!force) {
            auto existing = fs.readdir(root);
            if (!existing.empty()) {
                host.log().info("bigtree: store already has {} entries — skipping (set "
                                "bigtree.force to rebuild)",
                                existing.size());
                return;
            }
        }

        int const depth = int(optInt("bigtree.depth", "4"));
        int const maxDirs = int(optInt("bigtree.dirs", "5"));
        int const maxFiles = int(optInt("bigtree.files", "8"));
        int const rounds = int(optInt("bigtree.snapshots", "3"));
        uint64_t const total = parseSize(opt("bigtree.total", "1T"));
        uint64_t const seed = uint64_t(optInt("bigtree.seed", "42"));

        //  Hardlinks / symlinks per dir scale with maxFiles (as in bigtree.lua).
        int const maxHard = maxFiles / 4;
        int const maxSym = maxFiles / 3;

        std::mt19937_64 rng(seed); // reproducible: same seed → same tree
        auto randi = [&](int lo, int hi) {
            return lo > hi ? lo : std::uniform_int_distribution<int>(lo, hi)(rng);
        };
        auto uni01 = [&]() { return std::uniform_real_distribution<double>(0.0, 1.0)(rng); };

        //  Anchor the logical clock so `ls -l` shows real dates (unless --time
        //  already set one); advance it per snapshot round below.
        uint64_t t = fs.now();
        if (t == 0) {
            t = 1700000000ull; // 2023-11-14
            fs.setTime(t);
        }

#ifdef PRFS_WITH_CONTENT
        //  Filesystem-wide content policy: incompressible bytes, some sparse
        //  holes, cross-file dedup. All generated, none stored.
        content::ContentConfig cc;
        cc.blockSize = 65536;
        cc.entropy = 255;
        cc.sparsePercent = 10;
        cc.dedupPercent = 20;
        fs.setContentConfig(content::serialize(cc));
#endif

        struct FileRef {
            Node node;
            std::string path;
        };

        std::vector<FileRef> files; // every regular file, for hard/symlink targets
        uint64_t dirs = 0, hard = 0, sym = 0;
        long long id = 0;
        auto uniq = [&]() { return ++id; };

        //  At each directory: a random number of files, hardlinks to random
        //  existing files, symlinks to random existing files, and (until max
        //  depth) subdirs.
        std::function<void(Node, std::string const&, int)> buildDir =
            [&](Node dir, std::string const& prefix, int level) {
                int nf = randi(0, maxFiles);
                for (int i = 0; i < nf; ++i) {
                    std::string name = "file-" + std::to_string(uniq()) + ".bin";
                    Node f = fs.mkfile("");
                    fs.link(dir, name, f);
                    files.push_back({f, prefix + "/" + name});
                }

                if (!files.empty()) {
                    int nh = randi(0, maxHard); // hardlink: a second edge to an existing file
                    for (int i = 0; i < nh; ++i) {
                        FileRef const& tgt = files[randi(0, int(files.size()) - 1)];
                        fs.link(dir, "hardlink-" + std::to_string(uniq()) + ".bin", tgt.node);
                        hard++;
                    }
                    int ns = randi(0, maxSym); // symlink pointing at an existing file
                    for (int i = 0; i < ns; ++i) {
                        FileRef const& tgt = files[randi(0, int(files.size()) - 1)];
                        fs.link(dir, "symlink-" + std::to_string(uniq()), fs.symlink(tgt.path));
                        sym++;
                    }
                }

                if (level < depth) {
                    int nd = randi(1, maxDirs); // >=1 so the tree keeps growing
                    for (int i = 0; i < nd; ++i) {
                        std::string name = "dir-" + std::to_string(uniq());
                        Node d = fs.mkdir();
                        dirs++;
                        fs.link(dir, name, d);
                        buildDir(d, prefix + "/" + name, level + 1);
                    }
                }
            };
        buildDir(root, "", 1);

        //  Give each file a random, heavy-tailed weight (log-uniform over ~3
        //  orders of magnitude: many small files, a few large), then scale the
        //  weights so the whole batch still sums to the target total.
        auto randWeight = [&]() { return std::exp(uni01() * 6.9); }; // ~1x .. 1000x
        double wsum = 0;
        std::vector<double> w(files.size());
        for (size_t i = 0; i < files.size(); ++i) {
            w[i] = randWeight();
            wsum += w[i];
        }
        for (size_t i = 0; i < files.size(); ++i) {
            uint64_t sz =
                wsum > 0 ? uint64_t(std::max(1.0, std::floor(double(total) * w[i] / wsum))) : 1;
            files[i].node->size(sz);
        }
        //  A fresh random size on the same distribution, for the day-round files.
        auto randSize = [&]() {
            return wsum > 0
                       ? uint64_t(std::max(1.0, std::floor(double(total) * randWeight() / wsum)))
                       : uint64_t(1);
        };

        //  Snapshot the freshly built tree, then a few "daily" rounds: modify a
        //  couple of existing files (content + attrs) and add a dir of new files.
        std::vector<SnapId> snaps;
        snaps.push_back(fs.snapshot("base"));
        for (int day = 1; day <= rounds; ++day) {
            t += 86400;
            fs.setTime(t);
            if (!files.empty()) {
                fs.setContentSeed(files[0].node, 1000 + day); // MODIFIED_CONTENT
            }
            if (files.size() > 1) {
                files[1].node->mode(0600 + day); // MODIFIED_ATTRS
            }

            Node d = fs.mkdir();
            fs.link(root, "day-" + std::to_string(day), d);
            int nn = randi(1, maxFiles);
            for (int i = 0; i < nn; ++i) {
                Node f = fs.mkfile("");
                f->size(randSize());
                fs.link(d, "new-" + std::to_string(uniq()) + ".bin", f);
                files.push_back({f, "/day-" + std::to_string(day)});
            }
            snaps.push_back(fs.snapshot("day-" + std::to_string(day)));
        }

        Stats st = fs.stats();
        host.log().info("bigtree: built {} files, {} dirs, {} symlinks, {} hardlinks, {} links "
                        "(seed={}, depth<={}, target {})",
                        files.size(), st.nodes[int(Type::DIR)], st.nodes[int(Type::LNK)], hard,
                        st.links, seed, depth, human(total));
        host.log().info("bigtree: logical size {} across {} snapshots (nothing stored)",
                        human(st.totalSize), snaps.size());
    }
};

struct BigTreePlugin : IPlugin {
    IHost& host;
    BigTree frontend;

    explicit BigTreePlugin(IHost& h)
        : host(h)
        , frontend(h) {
        host.registry().provide<IService>(&frontend, "bigtree");
    }

    ~BigTreePlugin() override { host.registry().withdraw<IService>("bigtree"); }

    char const* name() const override { return "bigtree"; }

    char const* version() const override { return "0.1"; }
};

} // namespace

extern "C" uint32_t prfs_abi(void) { return prfs::plugin::ABI; }

extern "C" prfs::plugin::IPlugin* prfs_plugin_create(prfs::plugin::IHost& host) {
    return new BigTreePlugin(host);
}

extern "C" void prfs_plugin_destroy(prfs::plugin::IPlugin* p) { delete p; }
