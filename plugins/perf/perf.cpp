// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  perf — a read-performance front-end. On start() it benchmarks the content
//  generator (the actual work a prfs READ does: bytes = f(ContentConfig, seed,
//  offset)) single- and multi-threaded, then idles. It measures the *ceiling*
//  the NFS path can approach — no sockets, no RPC/XDR, and critically no client
//  page cache — using the store's own content policy so the number reflects what
//  is actually served.
//
//    prfs-host --store /tmp/prfs-big --plugin perf.so \
//      --set perf.threads=16 --set perf.bytes=1G --set perf.blocksize=1M
//
//  Generation is CPU-bound and embarrassingly parallel (a prfs READ takes the
//  store lock only *shared*), so the multi-thread pass shows how throughput
//  scales across cores. Reads over a mount add RPC/XDR + kernel-NFS overhead on
//  top, and must bypass the client cache (dd iflag=direct) to be comparable.
//
#include "prfs/plugin.hpp"
#ifdef PRFS_WITH_CONTENT
#include "prfs/content.hpp"
#endif

#include <spdlog/logger.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace prfs;
using namespace prfs::plugin;

//  Parse a byte count with an optional binary suffix: 1024, 512K, 8M, 4G, 1T.
uint64_t parseSize(std::string const& s, uint64_t def) {
    if (s.empty()) {
        return def;
    }
    uint64_t mult = 1;
    switch (s.back()) {
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
    default:
        break;
    }
    return uint64_t(std::stoull(mult == 1 ? s : s.substr(0, s.size() - 1)) * mult);
}

struct Perf : IService {
    IHost& host;

    explicit Perf(IHost& h)
        : host(h) {}

    std::vector<Option> options() const override {
        return {
            {"perf.bytes", "bytes generated per thread (K/M/G/T suffix ok)", "1G", false},
            {"perf.threads", "worker threads (0 = hardware concurrency)", "0", false},
            {"perf.blocksize", "generation chunk per call", "1M", false},
            {"perf.seed", "content seed (default: a real file's seed)", "", false},
            {"perf.size", "logical file size fed to the generator (default: a real file's)", "",
             false},
        };
    }

    std::string opt(char const* key, char const* def) const {
        std::string v = host.option(key);
        return v.empty() ? std::string(def) : v;
    }

    Error start() override {
#ifndef PRFS_WITH_CONTENT
        host.log().warn("perf: content not compiled (-Dcontent=false) — nothing to benchmark");
        return Error::OK;
#else
        try {
            run();
        } catch (std::exception const& e) {
            host.log().error("perf: {}", e.what());
            return Error::INVAL;
        }
        return Error::OK;
#endif
    }

    void stop() override {}

#ifdef PRFS_WITH_CONTENT
    //  A regular file's (seed, size) so the benchmark mirrors real served
    //  content; nullopt if the tree has no regular file (yet). Bounded DFS.
    std::optional<std::pair<uint64_t, uint64_t>> sampleFile() {
        std::shared_lock<std::shared_mutex> lk(host.storeMutex());
        IPrfs& fs = host.fs();
        std::vector<Node> stack{fs.rwRoot()};
        int budget = 4096; // don't scan a giant tree forever
        while (!stack.empty() && budget-- > 0) {
            Node d = stack.back();
            stack.pop_back();
            for (auto& [name, n] : fs.readdir(d)) {
                if (n->type() == Type::REG) {
                    return std::make_pair(n->contentSeed(), n->size());
                }
                if (n->type() == Type::DIR) {
                    stack.push_back(n);
                }
            }
        }
        return std::nullopt;
    }

    void run() {
        IPrfs& fs = host.fs();

        content::ContentConfig cfg;
        {
            std::shared_lock<std::shared_mutex> lk(host.storeMutex());
            std::string blob = fs.contentConfig();
            if (!blob.empty()) {
                cfg = content::deserialize(blob);
            }
        }

        auto sample = sampleFile();
        uint64_t seed = opt("perf.seed", "").empty() ? (sample ? sample->first : 1)
                                                     : parseSize(opt("perf.seed", "1"), 1);
        uint64_t fsize = opt("perf.size", "").empty()
                             ? (sample ? sample->second : (1ull << 30))
                             : parseSize(opt("perf.size", "1G"), 1ull << 30);
        if (fsize == 0) {
            fsize = 1ull << 30;
        }

        uint64_t const perThread = parseSize(opt("perf.bytes", "1G"), 1ull << 30);
        uint64_t const bs = parseSize(opt("perf.blocksize", "1M"), 1ull << 20);
        unsigned reqThreads = unsigned(std::stoul(opt("perf.threads", "0")));
        unsigned const hw = std::max(1u, std::thread::hardware_concurrency());
        unsigned const threads = reqThreads ? reqThreads : hw;

        host.log().info("perf: generator — blockSize={} entropy={} sparse={}% dedup={}% "
                        "(seed={}, fileSize={} B, {} B/thread)",
                        cfg.blockSize, unsigned(cfg.entropy), unsigned(cfg.sparsePercent),
                        unsigned(cfg.dedupPercent), seed, fsize, perThread);

        //  One pass: `n` threads each generate `perThread` bytes (each a distinct
        //  seed, so it mirrors reading many different files). Returns aggregate
        //  MiB/s over the wall-clock of the whole pass.
        auto pass = [&](unsigned n) -> double {
            std::atomic<uint64_t> total{0};
            auto t0 = std::chrono::steady_clock::now();
            std::vector<std::thread> pool;
            pool.reserve(n);
            for (unsigned k = 0; k < n; ++k) {
                pool.emplace_back([&, k] {
                    std::vector<char> buf(bs);
                    uint64_t done = 0, off = 0;
                    uint64_t sd = seed + k;
                    while (done < perThread) {
                        size_t got = content::read(cfg, sd, fsize, off % fsize, buf.data(), bs);
                        if (got == 0) {
                            break;
                        }
                        done += got;
                        off += got;
                    }
                    total += done;
                });
            }
            for (auto& t : pool) {
                t.join();
            }
            double secs =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            return secs > 0 ? (double(total.load()) / (1u << 20)) / secs : 0.0;
        };

        double single = pass(1);
        host.log().info("perf:  1 thread    {:8.1f} MiB/s", single);
        if (threads > 1) {
            double multi = pass(threads);
            host.log().info("perf: {:2d} threads   {:8.1f} MiB/s  ({:.1f}x, {:.1f} MiB/s/core)",
                            threads, multi, single > 0 ? multi / single : 0.0, multi / threads);
        }
        host.log().info("perf: done (this is the generator ceiling; NFS adds RPC/XDR + kernel "
                        "overhead, and reads must bypass the client cache to compare)");
    }
#endif
};

struct PerfPlugin : IPlugin {
    IHost& host;
    Perf frontend;

    explicit PerfPlugin(IHost& h)
        : host(h)
        , frontend(h) {
        host.registry().provide<IService>(&frontend, "perf");
    }

    ~PerfPlugin() override { host.registry().withdraw<IService>("perf"); }

    char const* name() const override { return "perf"; }

    char const* version() const override { return "0.1"; }
};

} // namespace

extern "C" uint32_t prfs_abi(void) { return prfs::plugin::ABI; }

extern "C" prfs::plugin::IPlugin* prfs_plugin_create(prfs::plugin::IHost& host) {
    return new PerfPlugin(host);
}

extern "C" void prfs_plugin_destroy(prfs::plugin::IPlugin* p) { delete p; }
