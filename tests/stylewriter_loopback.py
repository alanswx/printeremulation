#!/usr/bin/env python3
"""StyleWriter loopback test: drive the daemon's StyleWriter emulation with lpstyl.

lpstyl (references/lpstyl) is a reverse-engineered host driver for the Apple
StyleWriter family. It talks to the printer over a bidirectional serial line, so
this test connects it to mister_printerd through a pseudo-terminal:

    lpstyl --(pty master)--> [pty] <--(pty slave)-- mister_printerd -m stylewriter*

The daemon renders at 360 dpi (the StyleWriter's native resolution), so every
printer dot maps to exactly one canvas pixel. The page is pulled back out of the
PDF with pdfimages and compared bit for bit against the image lpstyl was given.

Usage: tests/stylewriter_loopback.py [--model stylewriter2500|stylewriter1500|stylewriter2]
                                     [--color] [--keep DIR]
Needs: build/mister_printerd, build/lpstyl (see Makefile), pdfimages (poppler).
"""
import argparse, os, pty, random, shutil, signal, subprocess, sys, tempfile, time, glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DAEMON = os.path.join(ROOT, "build", "mister_printerd")
LPSTYL = os.path.join(ROOT, "build", "lpstyl")

W, H = 3060, 3960            # US letter at 360 dpi (lpstyl's default page)

# Printable area lpstyl uses for each model: (left margin, top margin, width)
GEOMETRY = {
    "stylewriter2500": (72, 90, 2920),
    "stylewriter1500": (72, 90, 2920),
    "stylewriter2":    (96, 90, 2880),   # 90-dot margin, trimmed to whole bytes
}
TOP, BOTTOM = 90, 90


def make_planes(color):
    """Build test planes (lists of bytearrays, 1 bit per dot, MSB = leftmost)."""
    rowb = (W + 7) // 8
    rnd = random.Random(1234)
    names = ["C", "M", "Y", "K"] if color else ["K"]
    planes = {n: [bytearray(rowb) for _ in range(H)] for n in names}

    def fill(plane, x0, y0, x1, y1, pattern=None):
        for y in range(y0, y1):
            row = plane[y]
            for x in range(x0, x1):
                on = True if pattern is None else pattern(x, y)
                if on:
                    row[x >> 3] |= 0x80 >> (x & 7)

    k = planes["K"]
    fill(k, 72, 120, W - 60, 180)                                    # full-width bar (long black runs)
    fill(k, 300, 300, 900, 900, lambda x, y: (x // 4 + y // 4) % 2)  # checkerboard (literals)
    fill(k, 1000, 300, 1600, 900, lambda x, y: rnd.random() < 0.3)  # noise (literals, XOR churn)
    for i in range(40):                                              # text-like strokes
        fill(k, 200 + i * 60, 1000, 200 + i * 60 + 12, 1100 + (i % 5) * 20)
    fill(k, 72, 1300, 80, 3800)                                      # left edge column
    fill(k, 72 + 2900, 1300, 72 + 2920, 3800)                        # right edge of printable area
    fill(k, 500, 3700, 2500, 3860)                                   # near the bottom
    if color:
        fill(planes["C"], 400, 2000, 1400, 2600)
        fill(planes["M"], 1000, 2200, 2000, 2800)
        fill(planes["Y"], 1600, 2000, 2600, 2600, lambda x, y: (x + y) % 3 != 0)
    return planes


def write_input(path, planes, color):
    """pbmraw for mono, lpstyl 'bitcmyk' (4 bits/dot, C M Y K from MSB) for color."""
    with open(path, "wb") as f:
        if not color:
            f.write(b"P4\n%d %d\n" % (W, H))
            for row in planes["K"]:
                f.write(row)
            return
        for y in range(H):
            out = bytearray((W * 4 + 7) // 8)
            for x in range(W):
                byte, bit = x >> 3, 0x80 >> (x & 7)
                nib = 0
                for i, n in enumerate("CMYK"):
                    if planes[n][y][byte] & bit:
                        nib |= 8 >> i
                if x & 1:
                    out[x >> 1] |= nib
                else:
                    out[x >> 1] |= nib << 4
            f.write(out)


# --- Independent host: a Python port of lpstyl's encoder, writing a stream file ---

def encode_scanline(src, print_rowbytes):
    """Port of lpstyl's encodescanline(): runs of 00/FF plus literal blocks."""
    MAX_RUN, MAX_BLOCK = 0x3E, 0x3E
    n = len(src)
    if not any(src):
        return bytes([0x80])
    out = bytearray()
    s = 0
    while s < n:
        run_start = run_len = 0
        run_char = None
        i = s
        while i < n:
            if run_char is not None:
                if src[i] != run_char:
                    if i - run_start >= 1:
                        break
                    run_char = None
                elif i - run_start >= MAX_RUN:
                    break
            else:
                if src[i] in (0x00, 0xFF):
                    run_char, run_start = src[i], i
                elif i - s >= MAX_BLOCK:
                    break
            i += 1
        if run_char is not None:
            run_len = i - run_start
        else:
            run_start, run_len = i, 0
        if run_start != s:
            out.append(run_start - s)
            out += src[s:run_start]
            s = run_start
        if run_len > 0:
            if run_char == 0xFF:
                out.append(0xC0 + run_len)
            elif s + run_len < n:
                out.append(0x80 + run_len)
            else:
                break                          # trailing white: taken up by the padding
            s += run_len
    while s < print_rowbytes:
        run = min(print_rowbytes - s, MAX_RUN)
        out.append(0x80 + run)
        s += run
    return bytes(out)


def python_host_stream(planes, model, color, band_rows=150):
    """The byte stream lpstyl would send, cut into bands of band_rows rows."""
    left, top, pw = GEOMETRY[model]
    rowb = pw // 8
    lb = left // 8
    order = ["C", "M", "Y", "K"] if color else ["K"]
    ff = lambda q: b"\xff\xff\xff" + q
    out = bytearray(ff(b"I") + b"?")
    if model == "stylewriter2":
        out += b"nuA"
    else:
        out += ff(b"p") + b"D" + ff(b"H") + b"m0nZAH" + b"L"
    y_end = H - BOTTOM - 1
    for y0 in range(top, y_end, band_rows):
        y1 = min(y0 + band_rows, y_end)
        data = bytearray()
        last = {n: bytes(rowb) for n in order}
        for y in range(y0, y1):
            for n in order:
                row = bytes(planes[n][y][lb:lb + rowb])
                delta = bytes(a ^ b for a, b in zip(row, last[n]))
                data += encode_scanline(delta, rowb)
                last[n] = row
        out += ff(b"1") + ff(b"2") + ff(b"B")          # status polls, as lpstyl does
        out += (b"c" if color else b"R")
        for v in (0, y0 - top, pw - 1, y1 - 1 - top):
            out += bytes([v & 0xFF, v >> 8])
        out += b"G" + bytes([len(data) & 0xFF, len(data) >> 8]) + data + b"\x00"
    out += b"\x0c" + ff(b"1")
    return bytes(out)


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts, i = [], 0
    while len(parts) < 4:                     # magic, width, height, maxval
        while data[i:i+1].isspace():
            i += 1
        j = i
        while not data[j:j+1].isspace():
            j += 1
        parts.append(data[i:j])
        i = j
    i += 1
    return int(parts[1]), int(parts[2]), data[i:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default="stylewriter2500", choices=sorted(GEOMETRY))
    ap.add_argument("--color", action="store_true", help="send a CMYK page")
    ap.add_argument("--host", default="lpstyl", choices=["lpstyl", "python"],
                    help="lpstyl over a pty, or a Python encoder replayed from a file")
    ap.add_argument("--keep", help="keep the work directory here")
    args = ap.parse_args()
    if args.color and args.model == "stylewriter2":
        sys.exit("the StyleWriter II is monochrome")

    work = args.keep or tempfile.mkdtemp(prefix="swtest_")
    os.makedirs(work, exist_ok=True)
    outdir = os.path.join(work, "out")
    shutil.rmtree(outdir, ignore_errors=True)
    os.makedirs(outdir)

    planes = make_planes(args.color)
    inpath = os.path.join(work, "page.bitcmyk" if args.color else "page.pbm")
    write_input(inpath, planes, args.color)

    dlog = open(os.path.join(work, "daemon.log"), "w")
    if args.host == "python":
        stream = os.path.join(work, "page.prn")
        t0 = time.time()
        with open(stream, "wb") as f:
            f.write(python_host_stream(planes, args.model, args.color))
        rc = subprocess.run([DAEMON, "-d", stream, "-m", args.model, "-r", "360", "-t", "2",
                             "-o", outdir, "-v"], stdout=dlog, stderr=subprocess.STDOUT).returncode
        elapsed = time.time() - t0
        return check(args, planes, work, outdir, rc, elapsed, "from a file")

    master, slave = pty.openpty()
    slave_name = os.ttyname(slave)
    daemon = subprocess.Popen([DAEMON, "-d", slave_name, "-b", "57600", "-m", args.model,
                               "-r", "360", "-t", "2", "-o", outdir, "-v"],
                              stdout=dlog, stderr=subprocess.STDOUT)
    time.sleep(0.5)

    lp_args = [LPSTYL, "-v", "-t", "bitcmyk" if args.color else "pbmraw"]
    if args.color:
        lp_args += ["-w", str(W), "-h", str(H)]
    t0 = time.time()
    with open(inpath, "rb") as fin, open(os.path.join(work, "lpstyl.log"), "w") as llog:
        rc = subprocess.run(lp_args, stdin=fin, stdout=master, stderr=llog, timeout=600).returncode
    elapsed = time.time() - t0

    time.sleep(3)                             # let the inactivity timeout close the job
    daemon.send_signal(signal.SIGTERM)
    daemon.wait(timeout=30)
    os.close(master)
    os.close(slave)
    return check(args, planes, work, outdir, rc, elapsed, "over the pty")


def check(args, planes, work, outdir, rc, elapsed, how):
    pdfs = glob.glob(os.path.join(outdir, "*.pdf"))
    if rc != 0 or len(pdfs) != 1:
        print("FAIL: host rc=%d, %d PDFs (logs in %s)" % (rc, len(pdfs), work))
        return 1
    subprocess.run(["pdfimages", pdfs[0], os.path.join(work, "img")], check=True)
    w, h, pix = read_ppm(sorted(glob.glob(os.path.join(work, "img-*.ppm")))[0])

    left, top, pw = GEOMETRY[args.model]
    y_end = H - BOTTOM - 1                    # lpstyl's last printed row (exclusive)
    bad = 0
    first_bad = None
    for y in range(top, y_end):
        rows = {n: planes[n][y] for n in planes}
        for x in range(left, left + pw):
            byte, bit = x >> 3, 0x80 >> (x & 7)
            ink = {n: bool(rows[n][byte] & bit) for n in rows}
            i = (y * w + x) * 3
            r, g, b = pix[i], pix[i + 1], pix[i + 2]
            k = ink.get("K", False)
            er = 0 if (k or ink.get("C")) else 255
            eg = 0 if (k or ink.get("M")) else 255
            eb = 0 if (k or ink.get("Y")) else 255
            if (r, g, b) != (er, eg, eb):
                bad += 1
                if first_bad is None:
                    first_bad = (x, y, (r, g, b), (er, eg, eb))
    total = (y_end - top) * pw
    if bad:
        print("FAIL: %d of %d dots differ; first at %s (logs in %s)" % (bad, total, first_bad, work))
        return 1
    print("PASS: %s %s page via %s, %d dots identical (%.1fs %s)" %
          (args.model, "CMYK" if args.color else "mono", args.host, total, elapsed, how))
    if not args.keep:
        shutil.rmtree(work)
    return 0


if __name__ == "__main__":
    sys.exit(main())
