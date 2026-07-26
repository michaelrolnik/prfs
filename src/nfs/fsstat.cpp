// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>

//
//  FSSTAT / FSINFO projection (design §9). Pure functions over Stats + FsConfig;
//  no storage, no engine dependency beyond the IPrfs::stats() virtual.
//
#include "prfs/fsstat.hpp"

namespace prfs {

//  Advertised capacity must cover what is already used, so free never underflows.
static uint64_t freeOf(uint64_t used, uint64_t capacity) {
    return capacity > used ? capacity - used : 0;
}

FsStat fsStat(Stats const& s, FsConfig const& cfg) {
    uint64_t usedFiles = 0;
    for (uint64_t n : s.nodes) {
        usedFiles += n;
    }
    uint64_t usedBytes = s.totalSize;

    // Round the used-byte figure up to a whole block, then to a whole capacity.
    uint64_t block = cfg.blockSize ? cfg.blockSize : 1;
    usedBytes = ((usedBytes + block - 1) / block) * block;

    uint64_t tbytes = cfg.capacityBytes > usedBytes ? cfg.capacityBytes : usedBytes;
    uint64_t tfiles = cfg.capacityFiles > usedFiles ? cfg.capacityFiles : usedFiles;

    FsStat r;
    r.tbytes = tbytes;
    r.fbytes = freeOf(usedBytes, tbytes);
    r.abytes = r.fbytes; // no per-user reservation
    r.tfiles = tfiles;
    r.ffiles = freeOf(usedFiles, tfiles);
    r.afiles = r.ffiles;
    r.invarsec = 0; // usage changes with every mutation
    return r;
}

FsInfo fsInfo(FsConfig const& cfg) {
    FsInfo r;
    r.rtmax = cfg.rtmax;
    r.rtpref = cfg.rtpref;
    r.rtmult = cfg.rtmult;
    r.wtmax = cfg.wtmax;
    r.wtpref = cfg.wtpref;
    r.wtmult = cfg.wtmult;
    r.dtpref = cfg.dtpref;
    r.maxfilesize = cfg.maxfilesize;
    r.timeDeltaSec = cfg.timeDeltaSec;
    r.timeDeltaNsec = cfg.timeDeltaNsec;
    r.properties = cfg.properties;
    return r;
}

FsStat fsStat(IPrfs const& fs, FsConfig const& cfg, SnapId g) { return fsStat(fs.stats(g), cfg); }

} // namespace prfs
