-- SPDX-License-Identifier: MIT
-- Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>
--
-- bigtree — build a large, RANDOMLY-SHAPED synthetic filesystem and snapshot it.
-- At each level a seeded RNG picks how many subfolders, files, hardlinks, and
-- symlinks to create, so the tree is irregular like a real one — but fully
-- reproducible: the same seed always builds the same tree. Because prfs
-- GENERATES content (never stores it), a multi-terabyte tree is only a few MB of
-- metadata.
--
-- Files are sized so the whole tree sums to a target total, a filesystem-wide
-- content policy is set, and several snapshots are taken.
--
-- Build a persistent store, then serve it over NFS:
--   ./build/prfs-test examples/bigtree.lua /tmp/prfs-big 4 5 8
--   ./build/prfs-host --store /tmp/prfs-big --port 20490
--   sudo mount -t nfs -o vers=3,proto=tcp,port=20490,mountport=20490,\
--       mountproto=tcp,nolock 127.0.0.1:/ /mnt/prfs
--
-- Usage: prfs-test bigtree.lua <store> [depth] [maxDirs] [maxFiles] [totalBytes] [seed]

local path     = arg[1] or "/tmp/prfs-big"
local depth    = tonumber(arg[2] or 4)                          -- max tree depth
local maxDirs  = tonumber(arg[3] or 5)                          -- up to this many subdirs per dir
local maxFiles = tonumber(arg[4] or 8)                          -- up to this many files per dir
local total    = tonumber(arg[5] or (1024 * 1024 * 1024 * 1024)) -- 1 TiB
local seed     = tonumber(arg[6] or 42)

-- Hardlinks / symlinks per dir scale with maxFiles.
local maxHard = math.floor(maxFiles / 4)
local maxSym  = math.floor(maxFiles / 3)

math.randomseed(seed) -- reproducible: same seed → same tree

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

local files = {}                          -- { {node=, path=}, ... } — every regular file
local dirs, hard, sym, id = 0, 0, 0, 0
local function uniq() id = id + 1; return id end

-- At each directory: a random number of files, hardlinks to random existing
-- files, symlinks to random existing files, and (until max depth) subdirs.
local function build(dir, prefix, level)
  for _ = 1, math.random(0, maxFiles) do
    local name = string.format("file-%d.bin", uniq())
    local f = fs:mkfile("")
    fs:link(dir, name, f)
    files[#files + 1] = { node = f, path = prefix .. "/" .. name }
  end

  if #files > 0 then
    for _ = 1, math.random(0, maxHard) do -- hardlink: a second edge to an existing file
      local tgt = files[math.random(#files)]
      fs:link(dir, string.format("hardlink-%d.bin", uniq()), tgt.node)
      hard = hard + 1
    end
    for _ = 1, math.random(0, maxSym) do -- symlink pointing at an existing file
      local tgt = files[math.random(#files)]
      fs:link(dir, string.format("symlink-%d", uniq()), fs:symlink(tgt.path))
      sym = sym + 1
    end
  end

  if level < depth then
    for _ = 1, math.random(1, maxDirs) do -- >=1 so the tree keeps growing
      local name = string.format("dir-%d", uniq())
      local d = fs:mkdir()
      dirs = dirs + 1
      fs:link(dir, name, d)
      build(d, prefix .. "/" .. name, level + 1)
    end
  end
end

build(fs:root(), "", 1)

-- Distribute the target total across all files (content is generated to size).
local fsize = math.max(1, math.floor(total / math.max(1, #files)))
for _, f in ipairs(files) do f.node:setSize(fsize) end

-- Snapshot the freshly built tree, then a few "daily" rounds: modify a couple
-- of existing files (content + attrs) and add a directory of new files.
local snaps = { fs:snapshot("base") }
for day = 1, 3 do
  t = t + 86400
  fs:setTime(t)
  if files[1] then fs:setContentSeed(files[1].node, 1000 + day) end -- MODIFIED_CONTENT
  if files[2] then files[2].node:setMode(0600 + day) end            -- MODIFIED_ATTRS

  local d = fs:mkdir()
  fs:link(fs:root(), "day-" .. day, d)
  for i = 1, math.random(1, maxFiles) do
    local f = fs:mkfile("")
    f:setSize(fsize)
    fs:link(d, string.format("new-%d.bin", uniq()), f)
    files[#files + 1] = { node = f, path = "/day-" .. day }
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
print(string.format("built %s: depth<=%d maxDirs=%d maxFiles=%d seed=%d", path, depth, maxDirs,
  maxFiles, seed))
print(string.format("  files=%d dirs=%d symlinks=%d hardlinks=%d links=%d",
  #files, st.nodes[1], st.nodes[2], hard, st.links))
print(string.format("  logical size = %s  (nothing stored — content is generated)",
  human(st.totalSize)))
print("  snapshots (id): " .. table.concat(snaps, ", "))

-- What changed between consecutive snapshots (diffPaths names + diffNodes tally).
print("changes between snapshots:")
for i = 2, #snaps do
  local a, b = snaps[i - 1], snaps[i]
  local added, removed, sample = 0, 0, {}
  for _, p in ipairs(fs:diffPaths(a, b)) do
    if p.change == prfs.PathChange.ADDED then added = added + 1 else removed = removed + 1 end
    if #sample < 4 then
      sample[#sample + 1] = (p.change == prfs.PathChange.ADDED and "+" or "-") .. p.name
    end
  end
  local nc = { 0, 0, 0, 0 } -- CREATED, REMOVED, MODIFIED_CONTENT, MODIFIED_ATTRS
  for _, d in ipairs(fs:diffNodes(a, b)) do nc[d.change + 1] = nc[d.change + 1] + 1 end
  print(string.format("  %d -> %d: paths +%d -%d  |  nodes created=%d removed=%d content=%d attrs=%d",
    a, b, added, removed, nc[1], nc[2], nc[3], nc[4]))
  print("           e.g. " .. table.concat(sample, " "))
end

print("serve it:  prfs-host --store " .. path .. " --port 20490")
