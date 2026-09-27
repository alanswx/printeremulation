#!/usr/bin/env python3
"""Pull a file's resource and data forks out of an HFS disk image (Apple partition map).

  extract_from_hfs.py DISK.hda "System Folder 8.1:Extensions:Color SW 2500" OUTPREFIX

Writes OUTPREFIX.rsrc and OUTPREFIX.data. Needs `pip install machfs`.
"""
import struct, sys
import machfs


def hfs_partition(img):
    """Return (offset, length) in bytes of the first Apple_HFS partition."""
    with open(img, "rb") as f:
        for i in range(1, 64):
            f.seek(i * 512)
            e = f.read(512)
            if e[:2] != b"PM":
                break
            start, count = struct.unpack(">II", e[8:16])
            if e[48:80].split(b"\0")[0] == b"Apple_HFS":
                return start * 512, count * 512
    return 0, None          # bare HFS volume, no partition map


def main():
    img, path, out = sys.argv[1:4]
    off, length = hfs_partition(img)
    with open(img, "rb") as f:
        f.seek(off)
        part = f.read(length) if length else f.read()
    v = machfs.Volume()
    v.read(part)
    item = v
    for p in path.split(":"):
        item = item[p]
    open(out + ".rsrc", "wb").write(item.rsrc)
    open(out + ".data", "wb").write(item.data)
    print("%s: type=%s creator=%s rsrc=%d data=%d" % (path, item.type, item.creator, len(item.rsrc), len(item.data)))


if __name__ == "__main__":
    main()
