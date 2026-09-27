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

- With the baud-rate generator disabled (WR14 bit 0 = 0) — how Mac drivers get
  57600, clocking from RTxC (3.6864 MHz ÷ 64) — the core falls back to a fixed
  9600 divider (`CPB_9600`). RTxC ÷ 16/32/64 is not modeled.
- With WR11 selecting the TRxC pin, the core substitutes a permanent virtual
  1 MHz clock (added for MIDI). Real StyleWriters can drive TRxC externally for a
  ~1 Mbit/s "fast" mode; if Apple's driver uses it, this substitute is unlikely
  to produce something the daemon can decode.

### Driver disassembly: fast mode is 230.4 kbaud async, clocked from RTxC

Source: the 68k code of the Mac OS 8.1 "Color SW 2500" driver (CODE -8218
`EngineComm`) and the System file's serial patch for the Quadra ROM family
(`PTCH 1660`), both pulled from the Quadra disk image.

- **Normal setup:** the driver opens `.AOut`/`.AIn` (or `.BOut`/`.BIn`) through
  the standard Serial Driver and calls `SerReset` with `$4C00` (57600 baud, 8N1).
  `SerHShake` is all zeros (no XON/XOFF, no CTS, no DTR). This matches lpstyl.
- **Fast mode:** the driver checks the Serial Driver version (Status csCode 9,
  needs version 5 or later), then sends the private Control csCode **`'JF'`
  (`$4A46`)**. `PTCH 1660` handles it by clearing the time constant and
  reprogramming the SCC from a table identical to the ROM's normal one except:
  - **WR11 `$50` → `$00`**: TX and RX clocked from the **RTxC pin** instead of
    the baud-rate generator.
  - **WR14 `$01` → `$00`**: baud-rate generator disabled.

  WR4 keeps the x16 clock mode, so the line runs at **3.6864 MHz / 16 = 230,400
  baud**. It's still **asynchronous** 8N1 with start and stop bits, not a
  synchronous protocol.
- **`'jf'` (`$6A66`)**, with a one-byte parameter, only toggles the
  transmit-interrupt enable (WR1 bit 1) in the driver's shadow copy.
- The driver also reads the break-received bit from `SerStatus`, so the printer
  may signal with a break.

Table format, for the record (`ROM $6AEB8`, the writer both tables use): 16-bit
entries. Fixed entries are (value, register), and the register byte is written
first. `$FF` entries are (register, `$FF`): the value comes from the driver's
per-channel shadow bytes (WR4, WR1, WR3, WR5, WR12, WR13, WR3, WR5, WR1, in
order from `$21`).

**Why the Quadra core fails:** with the BRG disabled, `scc.v` ignores the RTxC
source and falls back to a fixed 9600-baud divider (`CPB_9600`). The Mac then
transmits at 9600 instead of 230,400, so the daemon can't decode anything at any
setting.

**What's needed:**
1. **Quadra core:** when WR11 selects RTxC, clock from 3.6864 MHz (the
   `BRG_RATIO_X128` constant already exists) divided by the WR4 clock mode (x16 →
   230,400, x32 → 115,200, x64 → 57,600), for both TX and RX.
2. **Daemon:** switch the HPS UART from 57600 to 230,400 at the same moment the
   Mac does. The printer must be told first, so there should be a protocol
   command just before `'JF'`, which the daemon can watch for. The HPS 16550
   supports 230,400.

### Open questions / next steps

1. **Rule out AppleTalk.** If AppleTalk is active on the printer port (LocalTalk),
   the Mac runs the SCC in synchronous mode, which would also look like this.
   Check the Chooser / AppleTalk control panel, set AppleTalk inactive (or to
   Ethernet), and retry.
2. **Model RTxC clocking in the Quadra core** (see the disassembly section):
   WR11 = RTxC → 3.6864 MHz ÷ WR4 clock mode, for TX and RX.
3. **Find the speed-switch command.** Trace the callers of the `'JF'` wrapper
   (`EngineComm` `$173A`) to see what the driver tells the printer before
   switching. Then have the daemon change the UART to 230,400 when it sees that
   command.
4. Once real driver traffic reaches the daemon, capture it
   (`touch /tmp/debug_printer_stream` → `/tmp/printer_stream.bin`) and check for
   commands lpstyl never sends; add the capture to `make test`.
5. Later: Color StyleWriter Pro (needs a traffic capture; no public protocol notes),
   and adding the StyleWriter models to the Main OSD printer-model menu (for now,
   `echo stylewriter > /tmp/PRINTER_MODEL` and restart the UART).

## Running it on the MiSTer

```
echo stylewriter > /tmp/PRINTER_MODEL      # or stylewriter1500 / stylewriter2
touch /tmp/debug_printer_stream            # optional: raw capture
/sbin/uartmode 0; /sbin/uartmode 7         # restart the daemon (baud from /tmp/UART_SPEED)
```

Switch back with `echo imagewriter > /tmp/PRINTER_MODEL` and 9600 baud.
