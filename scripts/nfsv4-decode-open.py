#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Michael Rolnik <mrolnik@gmail.com>
#
# Minimal pcap decoder for the NFSv4 ENOLCK investigation: find NFSv4 COMPOUND
# *replies* and print each OPEN (opnum 18) result's key fields — rflags (with the
# CONFIRM / LOCKTYPE_POSIX bits broken out), delegation type, stateid, attrset.
# Loopback RPC replies are small (one TCP segment each), so no TCP reassembly is
# needed. Usage: nfsv4-decode-open.py <file.pcap>
import struct
import sys

LINKHDR = {0: 4, 1: 14, 113: 16, 276: 20, 101: 0}  # NULL, EN10MB, SLL, SLL2, RAW

OPS = {
    3: "ACCESS", 4: "CLOSE", 6: "CREATE", 9: "GETATTR", 10: "GETFH", 11: "LINK",
    12: "LOCK", 13: "LOCKT", 14: "LOCKU", 15: "LOOKUP", 16: "LOOKUPP", 18: "OPEN",
    20: "OPEN_CONFIRM", 22: "PUTFH", 24: "PUTROOTFH", 25: "READ", 26: "READDIR",
    27: "READLINK", 28: "REMOVE", 29: "RENAME", 30: "RENEW", 34: "SETATTR",
    38: "WRITE", 35: "SETCLIENTID", 36: "SETCLIENTID_CONFIRM",
}
DELEG = {0: "NONE", 1: "READ", 2: "WRITE", 4: "NONE_EXT"}


def pcap_packets(path):
    with open(path, "rb") as f:
        data = f.read()
    magic = struct.unpack("<I", data[:4])[0]
    le = magic in (0xA1B2C3D4, 0xA1B23C4D)
    end = "<" if le else ">"
    net = struct.unpack(end + "I", data[20:24])[0]
    off = 24
    while off + 16 <= len(data):
        _, _, incl, _ = struct.unpack(end + "IIII", data[off:off + 16])
        off += 16
        yield net, data[off:off + incl]
        off += incl


def tcp_payload(net, pkt):
    lh = LINKHDR.get(net, 14)
    p = pkt[lh:]
    if len(p) < 20 or (p[0] >> 4) != 4:  # IPv4 only
        return None
    ihl = (p[0] & 0xF) * 4
    if p[9] != 6:  # TCP
        return None
    tcp = p[ihl:]
    if len(tcp) < 20:
        return None
    doff = (tcp[12] >> 4) * 4
    return tcp[doff:]


class R:
    def __init__(self, b):
        self.b, self.o = b, 0

    def u32(self):
        v = struct.unpack(">I", self.b[self.o:self.o + 4])[0]
        self.o += 4
        return v

    def u64(self):
        v = struct.unpack(">Q", self.b[self.o:self.o + 8])[0]
        self.o += 8
        return v

    def opaque(self):
        n = self.u32()
        s = self.b[self.o:self.o + n]
        self.o += (n + 3) & ~3
        return s

    def stateid(self):
        seqid = self.u32()
        other = self.b[self.o:self.o + 12]
        self.o += 12
        return seqid, other.hex()


def decode_reply(payload):
    # RPC record mark, then reply: xid, mtype(1), reply_stat(0), verf, accept_stat(0).
    if len(payload) < 4:
        return None
    mark = struct.unpack(">I", payload[:4])[0]
    if not (mark & 0x80000000):
        return None
    r = R(payload[4:])
    try:
        r.u32()                       # xid
        if r.u32() != 1:              # msg_type REPLY
            return None
        if r.u32() != 0:              # reply_stat MSG_ACCEPTED
            return None
        r.u32(); r.opaque()           # verf flavor + body
        if r.u32() != 0:              # accept_stat SUCCESS
            return None
        # COMPOUND4res
        r.u32()                       # status
        r.opaque()                    # tag
        nres = r.u32()
        out = []
        for _ in range(nres):
            op = r.u32()
            st = r.u32()
            name = OPS.get(op, str(op))
            if op == 18 and st == 0:   # OPEN4resok
                seqid, other = r.stateid()
                r.u32(); r.u64(); r.u64()          # cinfo: atomic, before, after
                rflags = r.u32()
                bm = r.u32()
                words = [r.u32() for _ in range(bm)]
                dtype = r.u32()
                bits = []
                if rflags & 0x2:
                    bits.append("CONFIRM")
                if rflags & 0x4:
                    bits.append("LOCKTYPE_POSIX")
                out.append(
                    "OPEN -> rflags=0x{:x} [{}]  deleg={}  stateid(seqid={},other={})  attrset={}"
                    .format(rflags, "|".join(bits) or "-", DELEG.get(dtype, dtype),
                            seqid, other, words))
            else:
                out.append("{}({})={}".format(name, op, st))
                if st != 0:
                    break
                # We only fully parse ops that appear before OPEN in these COMPOUNDs
                # (PUTFH/PUTROOTFH have no resok body); bail if we hit an unparsed one.
                if op not in (22, 23, 24, 32, 31):
                    out.append("...(stopped: unparsed op body)")
                    break
        return out
    except (struct.error, IndexError):
        return None


def main():
    if len(sys.argv) != 2:
        print("usage: nfsv4-decode-open.py <file.pcap>", file=sys.stderr)
        sys.exit(2)
    found = False
    for net, pkt in pcap_packets(sys.argv[1]):
        payload = tcp_payload(net, pkt)
        if not payload:
            continue
        dec = decode_reply(payload)
        if dec and any(s.startswith("OPEN ->") for s in dec):
            print("  " + "\n  ".join(dec))
            found = True
    if not found:
        print("  (no OPEN reply found — try a larger capture window)")


if __name__ == "__main__":
    main()
