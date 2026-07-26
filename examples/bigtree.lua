-- SPDX-License-Identifier: MIT
-- Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>
--
-- bigtree — build a large synthetic filesystem and snapshot it. Because prfs
-- GENERATES content (never stores it), a multi-terabyte tree is only a few MB
-- of metadata. This builds a directory tree of the given depth and branching
-- factor, sizes the leaf files so the whole tree sums to a target total, sets a
-- filesystem-wide content policy, and takes several snapshots.
--
-- Run it to build a persistent store, then serve that store over NFS:
--   ./build/prfs-test examples/bigtree.lua /tmp/prfs-big 4 6
--   ./build/prfs-host --store /tmp/prfs-big --port 20490
--   sudo mount -t nfs -o vers=3,proto=tcp,port=20490,mountport=20490,\
--       mountproto=tcp,nolock 127.0.0.1:/ /mnt/prfs
--
-- Usage: prfs-test bigtree.lua <store> [depth] [branching] [totalBytes]

local path      = arg[1] or "/tmp/prfs-big"
local depth     = tonumber(arg[2] or 4)
local branching = tonumber(arg[3] or 6)
local total     = tonumber(arg[4] or (1024 * 1024 * 1024 * 1024)) -- 1 TiB

local fs = prfs.open(path, { clean = true })

-- Anchor the logical clock so `ls -l` shows real dates; advance it per snapshot.
local t = 1700000000 -- 2023-11-14
fs:setTime(t)

-- Filesystem-wide content policy (optional module): incompressible bytes, with
-- some sparse holes and cross-file dedup. All generated, none stored.
if prfs.content then
  fs:setContentConfig(prfs.content.config {
    blockSize = 65536, entropy = 255, sparsePercent = 10, dedupPercent = 20,
  })
end

-- Leaf files = branching^depth; size each so the tree sums to ~total.
local nfiles = math.floor(branching ^ depth)
local fsize  = math.max(1, math.floor(total / nfiles))
local files  = 0

local function build(dir, level)
  if level >= depth then
    for i = 1, branching do
      local f = fs:mkfile("")
      f:setSize(fsize)
      fs:link(dir, string.format("file-%03d.bin", i), f)
      files = files + 1
    end
  else
    for i = 1, branching do
      local d = fs:mkdir()
      fs:link(dir, string.format("dir-%02d", i), d)
      build(d, level + 1)
    end
  end
end

build(fs:root(), 1)

-- Snapshot the freshly built tree, then a few "daily" rounds that add files.
local snaps = { fs:snapshot("base") }
for day = 1, 3 do
  t = t + 86400 -- +1 day
  fs:setTime(t)
  local d = fs:mkdir()
  fs:link(fs:root(), "day-" .. day, d)
  for i = 1, branching do
    local f = fs:mkfile("")
    f:setSize(fsize)
    fs:link(d, string.format("new-%02d.bin", i), f)
    files = files + 1
  end
  snaps[#snaps + 1] = fs:snapshot("day-" .. day)
end

local function human(b)
  local u = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" }
  local i = 1
  while b >= 1024 and i < #u do b = b / 1024; i = i + 1 end
  return string.format("%.1f %s", b, u[i])
end

local st = fs:stats()
print(string.format("built %s: depth=%d branching=%d", path, depth, branching))
print(string.format("  files=%d dirs=%d links=%d", files, st.nodes[1], st.links))
print(string.format("  logical size = %s  (nothing stored — content is generated)",
  human(st.totalSize)))
print("  snapshots (id): " .. table.concat(snaps, ", "))
print("serve it:  prfs-host --store " .. path .. " --port 20490")
