#!/usr/bin/env python3
"""Classic Mac resource-fork helper for driver reverse engineering.

FILE is a MacBinary file or a raw resource fork ending in .rsrc.

  macres.py list FILE                  list resources
  macres.py dump FILE TYPE ID OUT      write one resource's data
  macres.py scan FILE                  rough scan for SCCRd/SCCWr ($1D8/$1DC) references
                                       (also matches vtable calls at offset $1D8/$1DC)
  macres.py dis FILE TYPE ID [START [COUNT]]   disassemble a 68k code resource
"""
import struct, sys
import capstone


def read_macbinary(path):
    d = open(path, "rb").read()
    dlen, rlen = struct.unpack(">II", d[83:91])
    name = d[2:2 + d[1]].decode("mac_roman")
    ftype, creator = d[65:69], d[69:73]
    doff = 128
    roff = doff + ((dlen + 127) // 128) * 128
    return name, ftype, creator, d[doff:doff + dlen], d[roff:roff + rlen]


def parse_rsrc(r):
    if not r:
        return {}
    doff, moff, dlen, mlen = struct.unpack(">IIII", r[:16])
    m = r[moff:moff + mlen]
    tl_off, nl_off = struct.unpack(">HH", m[24:28])
    tl = m[tl_off:]
    ntypes = struct.unpack(">H", tl[:2])[0] + 1
    res = {}
    for i in range(ntypes):
        t, cnt, ref = struct.unpack(">4sHH", tl[2 + i * 8:10 + i * 8])
        for j in range(cnt + 1):
            e = tl[ref + j * 12:ref + j * 12 + 12]
            rid, noff, attr_off = struct.unpack(">hhI", e[:8])
            off = attr_off & 0xFFFFFF
            ln = struct.unpack(">I", r[doff + off:doff + off + 4])[0]
            name = ""
            if noff != -1:
                nb = m[nl_off + noff:]
                name = nb[1:1 + nb[0]].decode("mac_roman")
            res[(t, rid)] = (name, r[doff + off + 4:doff + off + 4 + ln])
    return res


def fmt_type(t):
    return t.decode("mac_roman")


def disasm(code, start=0, count=None, base=0):
    md = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_M68K_020)
    off, n = start, 0
    while off < len(code) and (count is None or n < count):
        w = struct.unpack(">H", code[off:off + 2])[0] if off + 2 <= len(code) else 0
        if (w & 0xF000) == 0xA000:                     # Mac A-line trap
            print("%06x: %04x          _Trap $%03X%s" % (base + off, w, w & 0x0FFF,
                  " (OS)" if not (w & 0x0800) else ""))
            off += 2; n += 1
            continue
        insns = list(md.disasm(code[off:off + 16], base + off, 1))
        if not insns:
            print("%06x: %04x          dc.w" % (base + off, w))
            off += 2; n += 1
            continue
        i = insns[0]
        print("%06x: %-14s %s %s" % (i.address, i.bytes.hex(), i.mnemonic, i.op_str))
        off += i.size; n += 1


def scan(res):
    # movea.l $1d8.w / $1dc.w, An  -> 2078 01d8 style (and lea, move.l to Dn)
    pats = {b"\x01\xd8": "SCCRd", b"\x01\xdc": "SCCWr"}
    for (t, rid), (name, data) in sorted(res.items()):
        hits = []
        for p, what in pats.items():
            i = data.find(p)
            while i != -1:
                if i >= 2 and (data[i - 2] & 0xF0) in (0x20, 0x22, 0x24, 0x26, 0x28, 0x2A, 0x2C, 0x2E, 0x41, 0x43, 0x45, 0x47, 0x49, 0x4B, 0x4D, 0x4F):
                    hits.append((i - 2, what))
                i = data.find(p, i + 1)
        if hits:
            print("%s %d %r (%d bytes): %s" % (fmt_type(t), rid, name, len(data),
                  ", ".join("%s@%x" % (w, o) for o, w in sorted(hits)[:12])))


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    if path.endswith(".rsrc"):
        name, ftype, creator, dfork, rfork = path, b"????", b"????", b"", open(path, "rb").read()
    else:
        name, ftype, creator, dfork, rfork = read_macbinary(path)
    res = parse_rsrc(rfork)
    if cmd == "list":
        print("%s  type=%s creator=%s  data=%d rsrc=%d" % (name, ftype, creator, len(dfork), len(rfork)))
        for (t, rid), (n, d) in sorted(res.items()):
            print("  %s %6d %7d  %s" % (fmt_type(t), rid, len(d), n))
    elif cmd == "dump":
        t = sys.argv[3].encode("mac_roman").ljust(4)
        open(sys.argv[5], "wb").write(res[(t, int(sys.argv[4]))][1])
    elif cmd == "scan":
        scan(res)
    elif cmd == "dis":
        t = sys.argv[3].encode("mac_roman").ljust(4)
        code = res[(t, int(sys.argv[4]))][1]
        start = int(sys.argv[5], 16) if len(sys.argv) > 5 else 0
        count = int(sys.argv[6]) if len(sys.argv) > 6 else None
        disasm(code, start, count)


if __name__ == "__main__":
    main()
