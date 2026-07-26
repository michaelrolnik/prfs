// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

#pragma once
//
//  FSSTAT / FSINFO mapping (design §9). The store's O(1) Stats map onto what an
//  NFS front-end reports for FSSTAT (dynamic volume usage) and FSINFO (static
//  server parameters). This layer is a pure, engine-independent projection: no
//  storage, no XDR — the front-end serializes these structs. A synthetic target
//  has no real device, so capacity is a policy (FsConfig), not a measured value.
//
#include "prfs/prfs.hpp"

#include <cstdint>

namespace prfs {

//  NFSv3 FSINFO `properties` bits (RFC 1813 §3.3.19).
namespace fsf {
inline constexpr uint32_t LINK = 0x0001;        // hard links supported
inline constexpr uint32_t SYMLINK = 0x0002;     // symlinks supported
inline constexpr uint32_t HOMOGENEOUS = 0x0008; // PATHCONF uniform across the fs
inline constexpr uint32_t CANSETTIME = 0x0010;  // SETATTR can set arbitrary times
} // namespace fsf

//  Policy knobs a synthetic target advertises (there is no backing device).
struct FsConfig {
    uint64_t blockSize = 4096;                  // reporting block granularity
    uint64_t capacityBytes = uint64_t(1) << 40; // advertised total space (1 TiB)
    uint64_t capacityFiles = uint64_t(1) << 32; // advertised total inodes (~4.3 G)

    //  FSINFO transfer sizes / limits.
    uint32_t rtmax = 1u << 20;  // max read
    uint32_t rtpref = 1u << 20; // preferred read
    uint32_t rtmult = 4096;     // read size granularity
    uint32_t wtmax = 1u << 20;  // max write
    uint32_t wtpref = 1u << 20; // preferred write
    uint32_t wtmult = 4096;     // write size granularity
    uint32_t dtpref = 1u << 16; // preferred READDIR size
    uint64_t maxfilesize = (uint64_t(1) << 63) - 1;
    uint32_t timeDeltaSec = 0; // server time granularity (logical clock: 1 tick)
    uint32_t timeDeltaNsec = 1;
    uint32_t properties = fsf::LINK | fsf::SYMLINK | fsf::HOMOGENEOUS | fsf::CANSETTIME;
};

//  NFSv3 FSSTAT3resok (RFC 1813 §3.3.18) — dynamic, per snapshot.
struct FsStat {
    uint64_t tbytes = 0;   // total bytes
    uint64_t fbytes = 0;   // free bytes
    uint64_t abytes = 0;   // available to this user
    uint64_t tfiles = 0;   // total file slots
    uint64_t ffiles = 0;   // free file slots
    uint64_t afiles = 0;   // available to this user
    uint32_t invarsec = 0; // seconds the result stays invariant (0 = volatile)
};

//  NFSv3 FSINFO3resok (RFC 1813 §3.3.19) — static server parameters.
struct FsInfo {
    uint32_t rtmax = 0, rtpref = 0, rtmult = 0;
    uint32_t wtmax = 0, wtpref = 0, wtmult = 0;
    uint32_t dtpref = 0;
    uint64_t maxfilesize = 0;
    uint32_t timeDeltaSec = 0, timeDeltaNsec = 0;
    uint32_t properties = 0;
};

//  Pure projections.
FsStat fsStat(Stats const& s, FsConfig const& cfg = {});
FsInfo fsInfo(FsConfig const& cfg = {});

//  Convenience: read the store's stats at snap `g` and project them.
FsStat fsStat(IPrfs const& fs, FsConfig const& cfg = {}, SnapId g = LATEST);

} // namespace prfs
