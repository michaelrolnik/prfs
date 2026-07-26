-- SPDX-License-Identifier: MIT
-- Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>
--
-- Smoke scenario for the prfs Lua bindings (design §12). Exercises the whole
-- surface against the in-memory reference model. Any failed assert() aborts the
-- script and prfs-test exits non-zero.

local s = prfs.mem()

-- create / link / lookup ----------------------------------------------------
local root = s:root()
assert(root:type() == prfs.Type.DIR, "root is a directory")

local d = s:mkdir()
assert(s:link(root, "d", d) == prfs.Error.OK)

local f = s:mkfile("hello")
assert(s:link(d, "f", f) == prfs.Error.OK)
assert(f:nlink() == 1)
assert(s:link(d, "f", f) == prfs.Error.EXIST, "duplicate name rejected")

local got = s:lookup(d, "f")
assert(got and got:id() == f:id())
assert(got:content() == "hello")
assert(not s:lookup(d, "nope"))

-- readdir / parents ---------------------------------------------------------
local ents = s:readdir(d)
assert(#ents == 1 and ents[1].name == "f" and ents[1].node:id() == f:id())

-- hard link: same node under two names, two parents
assert(s:link(root, "f2", f) == prfs.Error.OK)
assert(f:nlink() == 2)
assert(#s:parents(f) == 2)

-- attributes ----------------------------------------------------------------
f:setSize(4096)
assert(f:size() == 4096)

local dev = s:mknod(prfs.Type.BLK, 8, 3)
local maj, min = dev:rdev()
assert(dev:type() == prfs.Type.BLK and maj == 8 and min == 3)

local sym = s:symlink("/some/target")
assert(sym:type() == prfs.Type.LNK and sym:target() == "/some/target")

-- snapshot: range-back + immutability ---------------------------------------
local s1 = s:snapshot()
s:setContent(f, "world")
local s2 = s:snapshot()
assert(s:lookup(d, "f"):content() == "world", "live view updated")

local oldd = s:lookup(s:snapshotRoot(s1), "d")
assert(s:lookup(oldd, "f"):content() == "hello", "old snapshot immutable")

-- diff ----------------------------------------------------------------------
local sawMod = false
for _, nd in ipairs(s:diffNodes(s1, s2)) do
    if nd.id == f:id() and nd.change == prfs.NodeChange.MODIFIED_CONTENT then
        sawMod = true
    end
end
assert(sawMod, "diffNodes reports f content-modified")

-- cycle prevention ----------------------------------------------------------
local a = s:mkdir(); s:link(root, "a", a)
local b = s:mkdir(); s:link(a, "b", b)
assert(s:link(b, "loop", a) == prfs.Error.INVAL, "directory cycle rejected")

-- move is rename ------------------------------------------------------------
assert(s:move(d, "f", root, "moved") == prfs.Error.OK)
assert(not s:lookup(d, "f"))
assert(s:lookup(root, "moved"):id() == f:id())

-- stats ---------------------------------------------------------------------
local st = s:stats()
assert(st.nodes[prfs.Type.DIR] >= 3, "root + d + a + b directories")
assert(st.links >= 4)

-- snapshots list ------------------------------------------------------------
assert(#s:snapshots() == 2)

-- FSSTAT / FSINFO mapping ---------------------------------------------------
local fss = s:fsStat()
assert(fss.tfiles - fss.ffiles == st.nodes[prfs.Type.DIR] + st.nodes[prfs.Type.REG]
    + st.nodes[prfs.Type.LNK] + st.nodes[prfs.Type.BLK] + st.nodes[prfs.Type.CHR]
    + st.nodes[prfs.Type.FIFO] + st.nodes[prfs.Type.SOCK], "used files == live nodes")
assert(fss.abytes == fss.fbytes)

local fsi = prfs.fsInfo()
assert(fsi.rtpref > 0 and fsi.maxfilesize > 0)

-- logical clock -------------------------------------------------------------
assert(s:now() == 0)
s:setTime(1700000000)
assert(s:now() == 1700000000)
local timed = s:mkfile("t")
assert(timed:mtime() == 1700000000, "new node stamps the logical clock")

-- snapshot metadata ---------------------------------------------------------
local labelled = s:snapshot("release-1")
local si = s:snapInfo(labelled)
assert(si.id == labelled and si.ctime == 1700000000 and si.label == "release-1")

-- synthesized .snapshot directory -------------------------------------------
local sd = s:lookup(root, prfs.SNAPSHOT_NAME)
assert(sd and sd:type() == prfs.Type.DIR and sd:mode() == 0x16d) -- 0555
assert(#s:readdir(sd) >= 1, ".snapshot lists snapshots")
assert(s:link(root, prfs.SNAPSHOT_NAME, s:mkfile("")) == prfs.Error.INVAL, "name reserved")

-- paginated readdir ---------------------------------------------------------
local full = s:readdir(root)
local paged, cookie = {}, ""
while true do
    local pg = s:readdirPage(root, cookie, 2)
    for _, e in ipairs(pg.entries) do
        paged[#paged + 1] = e.name
    end
    if pg.eof then break end
    cookie = pg.cookie
end
assert(#paged == #full, "paged readdir reassembles the full listing")

print("prfs lua smoke: OK")
