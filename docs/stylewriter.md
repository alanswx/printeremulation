# Apple StyleWriter Emulation — Status (parked 2026-09-27)

The daemon can emulate an Apple **StyleWriter II**, **Color StyleWriter 1500** and
**Color StyleWriter 2500**. The protocol and image decoding are verified offline
against lpstyl, a reverse-engineered host driver. Printing from Apple's own driver
on the MiSTer **Quadra 800** core does **not** work yet: the core's serial port is
not producing normal async data, so the daemon never sees a StyleWriter command.
The Color StyleWriter Pro is not supported (no protocol documentation exists).

## What's implemented

`src/parser_stylewriter.c`, selected with `-m stylewriter` (= 2500),
`-m stylewriter1500` or `-m stylewriter2`:

- **Bidirectional.** Unlike every other printer here, the StyleWriter host polls the
  printer and waits for answers. The daemon now writes replies back on the serial
  fd (`JobState.reply_fd`, only when the device is a tty), and opens the port
  *without* RTS/CTS for StyleWriter models: the printer has no flow control.
- **Status queries** (`FF FF FF x`, one-byte reply): `1` busy = 0, `2` error = 0,
  `B` buffer = drained (2500: `0x80`, 1500: `0x87`, II: `0xF8`), `p` sub-model
  (2500: `0x05`, 1500: `0x04`), `H` cartridge = color (`0x81`). `I` (reset) and
  `S` (resume after paper-out) get no reply.
- **Identify** `?` → `CS\r` (Color StyleWriters) or `SW\r` (StyleWriter II).
- **Image bands:** `R` (mono) or `c` (CMYK) + rect (left, top, right, bottom as
  LE16, inclusive, printer coordinates) + `G` + LE16 size + data + `00`.
  Rows are XOR-differenced per plane (history resets each band), planes C, M, Y, K
  per row, encoded as `01-3F` literals, `81-BE` unchanged runs, `C1-FE` inverted
  runs, `80` = rest of row unchanged. The encoding never emits `FF`, which makes
  `FF FF FF` queries unambiguous even mid-band.
- **Rendering:** 360 dpi dots are downsampled onto the 144 dpi canvas with area
  coverage (anti-aliased, not bolded); CMYK coverage maps to RGB. Page origin
  uses lpstyl's margins: 72 dots left / 90 top for the color models, 96 / 90 for
  the StyleWriter II (its 90-dot margin is trimmed to whole bytes).
- `0C` commits the page.

Protocol source: `references/lpstyl/README.protocol` and `lpstyl.c`
(<https://github.com/Godzil/lpstyl>).

## Offline verification

`tests/stylewriter_loopback.py` renders at 360 dpi (one printer dot per canvas
pixel) and compares the PDF's page, extracted with `pdfimages`, bit for bit with
the source image (~11 million dots):

| Host | Models | Result |
|---|---|---|
| Python port of lpstyl's encoder, replayed from a file (`make test`, step 7) | 2500 mono + CMYK, 1500 CMYK, II mono | pass |
| Real lpstyl over a pseudo-terminal (`make test-stylewriter-lpstyl`, needs `references/lpstyl`) | 2500, 1500, II mono | pass (full handshake, ~6 s/page) |

lpstyl's own **color** mode does not match: its `bitcmyk` chunky-to-planar reader
flushes on `(i & 3) == 0` instead of `== 3`, misaligning the planes. Color decoding
is therefore verified only through the Python encoder.

## Hardware findings (Quadra 800 core, Mac OS 8, Apple "Color SW 2500" driver)

The ImageWriter path works on the same core and port at 9600 baud. With the
StyleWriter driver selected:

- `ttyS1` received data with many framing errors and breaks, and the daemon sent
  nothing (`tx:0` in `/proc/tty/driver/serial`): no StyleWriter command was
  recognized.
- The capture was **byte-identical** (`00`/`E0` only) whether the HPS UART ran at
  57600 or 9600, so this is not a simple baud mismatch.
- Oversampled at 460800 the line reads as nothing but `00`/breaks: the Mac's TX
  line is held low for long stretches instead of idling high.

Relevant Quadra core behavior (`MacQuadra800_MiSTer/rtl/scc.v`, ~line 1620):

- With the baud-rate generator disabled (WR14 bit 0 = 0) and the clock taken
  straight from RTxC, the core falls back to a fixed 9600 divider (`CPB_9600`).
  RTxC ÷ 16/32/64 isn't modeled. Only the (unused on 68k) fast mode needs this.
  Normal 57600 uses the baud-rate generator with time constant 0 and x16, which
  the core handles.
- With WR11 selecting the TRxC pin, the core substitutes a permanent virtual
  1 MHz clock (added for MIDI). The StyleWriter driver doesn't select TRxC.

### Driver disassembly (full map: [stylewriter_driver_map.md](stylewriter_driver_map.md))

The Mac OS 8.1 "Color SW 2500" driver was pulled from the Quadra disk image and
disassembled.

- **The 68k driver runs at 57,600 baud, 8N1, with no handshaking, for the whole
  job.** Its command set is a superset of lpstyl's. The handshake replies, the
  band format and the row coding all match what the daemon implements.
- **A 230.4 kbaud fast mode exists but is dead code on 68k.** It works like
  this: send `'h'` to the printer, then the private Serial Driver call `'JF'`,
  which Mac OS 8.1 patches in (`PTCH 1660`) to clock the SCC from RTxC ÷ 16. But
  nothing in the 68k code calls it, so it presumably belongs to the PowerPC path.
- **Correction:** an earlier version of this document blamed fast mode, and
  the Quadra core's missing RTxC clocking, for the failure. That can't be it,
  because the 68k driver never enters fast mode. The Quadra failure is still
  unexplained.
- **Parser fix from the map:** `'m'` takes one parameter byte, not two.

### Open questions / next steps

1. **Rule out AppleTalk.** The driver's `Open` refuses the port (error −23) if
   another driver already has it, and LocalTalk on the printer port would
   also put the SCC in a synchronous mode that looks like our capture. Check
   the Chooser and the AppleTalk control panel, set AppleTalk inactive (or to
   Ethernet), and retry.
2. **Check the port mapping and the core's SCC writes.** Confirm which SCC
   channel the Chooser's printer/modem port maps to on the Quadra core, and log
   WR4/WR5/WR11–WR14 while printing. The 68k driver should program ordinary
   57,600 x16 async.
3. Once real driver traffic reaches the daemon, capture it
   (`touch /tmp/debug_printer_stream` → `/tmp/printer_stream.bin`), check query
   `'e'` (not in lpstyl) and anything else unexpected, and add the capture to
   `make test`.
4. Only for a PowerPC host: support fast mode. The daemon would switch the UART
   to 230,400 on `'h'`, and the core would clock the SCC from RTxC.
5. Later: Color StyleWriter Pro (needs a traffic capture; no public protocol
   notes), and adding the StyleWriter models to the Main OSD printer-model menu
   (for now, `echo stylewriter > /tmp/PRINTER_MODEL` and restart the UART).

## Running it on the MiSTer

```
echo stylewriter > /tmp/PRINTER_MODEL      # or stylewriter1500 / stylewriter2
touch /tmp/debug_printer_stream            # optional: raw capture
/sbin/uartmode 0; /sbin/uartmode 7         # restart the daemon (baud from /tmp/UART_SPEED)
```

Switch back with `echo imagewriter > /tmp/PRINTER_MODEL` and 9600 baud.
