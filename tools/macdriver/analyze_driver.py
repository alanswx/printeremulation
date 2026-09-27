#!/usr/bin/env python3
"""Annotated whole-driver disassembly for a classic Mac 'PRER' printer driver.

  analyze_driver.py DRIVER.rsrc OUTDIR

Written for Apple's Color StyleWriter drivers (jump table in CODE -8190, OS glue
in segment Main); see docs/stylewriter_driver_map.md. Needs capstone.

Writes OUTDIR/seg_<id>_<name>.s for every CODE segment, with:
  - jsr N(a5) resolved through the A5 jump table (CODE -8190 here) to Segment:offset,
    or to a known OS-glue name
  - A-line traps named
  - functions split at link.w / after rts|jmp(a0)|jmp(a1), labelled F_<seg>_<off>
and OUTDIR/index.tsv: seg, name, func offset, jt entries pointing at it, callers.
"""
import collections, importlib.util, os, re, struct, sys
import capstone

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("m", os.path.join(HERE, "macres.py"))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

TRAPS = {
    0x000: "_Open", 0x001: "_Close", 0x002: "_Read", 0x003: "_Write", 0x004: "_Control",
    0x005: "_Status", 0x006: "_KillIO", 0x00F: "_MountVol", 0x01E: "_NewPtr", 0x01F: "_DisposePtr",
    0x022: "_NewHandle", 0x023: "_DisposeHandle", 0x024: "_SetHandleSize", 0x025: "_GetHandleSize",
    0x029: "_HLock", 0x02A: "_HUnlock", 0x02E: "_BlockMove", 0x03B: "_Delay", 0x03C: "_CmpString",
    0x051: "_ReadXPRam", 0x05C: "_MemoryDispatch", 0x085: "_PMgrOp", 0x09E: "_Shutdown?",
    0x11E: "_NewPtrSys", 0x122: "_NewHandleSys", 0x1AD: "_Gestalt", 0x31E: "_NewPtrClear",
    0x346: "_GetTrapAddress", 0x51E: "_NewPtrSysClear", 0x746: "_GetToolTrapAddress",
    0x975: "_TickCount", 0x9F0: "_LoadSeg", 0x9F1: "_UnloadSeg", 0x9A0: "_GetResource",
    0x9A2: "_LoadResource", 0x9A3: "_ReleaseResource", 0x9A4: "_HomeResFile", 0x9A6: "_GetResAttrs",
    0x9AF: "_ResError", 0x9B4: "_UseResFile", 0x994: "_CurResFile", 0x9A8: "_GetResInfo",
    0x9FF: "_Debugger", 0x9E9: "_NumToString", 0x9EE: "_Pack7", 0x9E7: "_Pack5", 0x9ED: "_Pack6",
    0x9F2: "_Pack2", 0x9F3: "_Pack3", 0x9EB: "_Pack4/FP68K", 0x9EC: "_Pack5", 0xA05: "_Pack8",
    0xA06: "_Pack9", 0xA07: "_Pack10", 0xA2C: "_Pack11", 0xA82: "_Pack12", 0x9C8: "_SysBeep",
    0x9EA: "_Pack1", 0x8FD: "_PrGlue", 0x8FE: "_InitFonts", 0x86E: "_InitGraf", 0x850: "_InitCursor",
    0x851: "_SetCursor", 0x853: "_HideCursor", 0x9B0: "_GetIndResource", 0x8A1: "_FrameRect",
    0x8A2: "_PaintRect", 0x8A3: "_EraseRect", 0x874: "_GetPort", 0x873: "_SetPort", 0x9BD: "_GetNamedResource",
    0x991: "_ModalDialog", 0x97C: "_GetNewDialog", 0x983: "_DisposeDialog", 0x98D: "_GetDItem",
    0x98E: "_SetDItem", 0x990: "_GetIText", 0x98F: "_SetIText", 0x986: "_Alert", 0x985: "_StopAlert",
    0x89F: "_Unimplemented", 0x0AD: "_Gestalt", 0x88F: "_OSDispatch", 0xABF: "_ComponentDispatch",
    0x82B: "_Pack14/Help", 0x9DB: "_OpenRFPerm", 0x99A: "_CloseResFile", 0x9A5: "_CountResources",
    0xAA5: "_GetIndType?", 0x9AB: "_AddResource", 0x9AA: "_ChangedResource", 0x999: "_UpdateResFile",
    0x9AD: "_RmveResource", 0x81F: "_Get1Resource", 0x80E: "_Get1IndResource", 0x80D: "_Count1Resources",
    0x827: "_GetNextEvent?", 0x970: "_GetNextEvent", 0x860: "_WaitNextEvent", 0x9C7: "_EventAvail",
    0xA60: "_DeferUserFn/IdleUpdate?", 0x0AF: "_SerialPower?", 0x812: "_PPC/ADSP?",
}

GLUE = {}  # A5 offset -> name, filled from the known Main glue block below


def main():
    path, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    res = m.parse_rsrc(open(path, "rb").read())
    names = {rid: n for (t, rid), (n, _) in res.items() if t == b"CODE"}
    jt = res[(b"CODE", -8190)][1]
    _, _, jtsize, _ = struct.unpack(">IIII", jt[:16])
    entries = {}                       # a5 offset -> (seg, code offset incl. 4-byte header)
    by_target = collections.defaultdict(list)
    for k in range(jtsize // 8):
        off, _, seg, _ = struct.unpack(">HHhH", jt[16 + k * 8:24 + k * 8])
        a5 = 0x20 + k * 8 + 2
        entries[a5] = (seg, off + 4)
        by_target[(seg, off + 4)].append(a5)

    glue_by_main_off = {0x20BA: "OpenDriver", 0x20E0: "CloseDriver", 0x20FE: "SerReset",
                        0x2122: "SerHShake", 0x214E: "SerSetBrk", 0x2170: "SerClrBrk",
                        0x2178: "SerGetBuf", 0x21A0: "SerStatus", 0x21CC: "EqualString",
                        0x220C: "GetDCtlEntry", 0x2226: "CloseDriver2", 0x2240: "FSRead",
                        0x2244: "FSWrite", 0x2288: "Control", 0x22BE: "Status",
                        0x1CB4: "Gestalt", 0x247A: "PMgrOp?"}
    for a5, (seg, off) in entries.items():
        if seg == -8191 and off in glue_by_main_off:
            GLUE[a5] = glue_by_main_off[off]

    md = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_M68K_020)
    callers = collections.defaultdict(set)
    index = []
    for (t, rid), (name, code) in sorted(res.items(), key=lambda x: x[0][1]):
        if t != b"CODE" or rid == -8190:
            continue
        lines, off, func = [], 4, None
        starts = {o for (s, o) in by_target if s == rid}
        prev_end = True
        while off < len(code):
            w = struct.unpack(">H", code[off:off + 2])[0] if off + 2 <= len(code) else 0
            new_func = off in starts or w == 0x4E56 and prev_end
            if new_func:
                func = off
                eps = ",".join("$%X" % a for a in by_target.get((rid, off), []))
                lines.append("\nF_%d_%04X:%s" % (-rid, off, ("   ; A5 entries " + eps) if eps else ""))
                index.append((rid, name, off, eps))
            prev_end = False
            if (w & 0xF000) == 0xA000:
                lines.append("%06x: %04x          %s" % (off, w, TRAPS.get(w & 0x0FFF, "_Trap $%03X" % (w & 0xFFF))))
                off += 2
                continue
            ins = list(md.disasm(code[off:off + 16], off, 1))
            if not ins:
                lines.append("%06x: %04x          dc.w" % (off, w))
                off += 2
                prev_end = True
                continue
            i = ins[0]
            text = "%s %s" % (i.mnemonic, i.op_str)
            mm = re.match(r"jsr \$([0-9a-f]+)\(a5\)", text)
            if mm:
                a5 = int(mm.group(1), 16)
                if a5 in GLUE:
                    text += "        ; " + GLUE[a5]
                elif a5 in entries:
                    s, o = entries[a5]
                    text += "        ; -> %s:F_%d_%04X" % (names.get(s, s), -s, o)
                    callers[(s, o)].add((rid, func))
            lines.append("%06x: %-14s %s" % (i.address, i.bytes.hex(), text))
            off += i.size
            if i.mnemonic in ("rts", "rtd") or text.startswith("jmp (a0)") or text.startswith("jmp (a1)"):
                prev_end = True
        fn = os.path.join(outdir, "seg_%d_%s.s" % (-rid, re.sub(r"[^A-Za-z0-9_]", "_", name or "noname")))
        open(fn, "w").write("; CODE %d  %s  (%d bytes)\n" % (rid, name, len(code)) + "\n".join(lines) + "\n")
    with open(os.path.join(outdir, "index.tsv"), "w") as f:
        f.write("seg\tname\tfunc\tA5entries\tcallers\n")
        for rid, name, off, eps in index:
            cs = ",".join("%s:%04X" % (names.get(s, s), o or 0) for s, o in sorted(callers.get((rid, off), []), key=str))
            f.write("%d\t%s\tF_%d_%04X\t%s\t%s\n" % (rid, name, -rid, off, eps, cs))
    print("segments:", len([1 for (t, r) in res if t == b'CODE']), "functions:", len(index))


if __name__ == "__main__":
    main()
