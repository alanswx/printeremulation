# MiSTer Retro Printer Emulation: Project Handoff & Status

**Generated**: September 20, 2026  
**Repositories**:
* Standalone Daemon & Specs: [`/Users/alans/dev2/printeremulation`](https://github.com/alanswx/printeremulation)
* MiSTer Main Fork: [`/Users/alans/dev2/MainMess`](https://github.com/alanswx/Main_MiSTer)
* Casio Loopy Core: [`/Users/alans/dev2/Loopy_MiSTer`](https://github.com/MiSTer-devel/Loopy_MiSTer)
* Target Hardware: `root@mister.local` (Cyclone V ARMv7 Linux 5.15.1)

---

## 1. Executive Summary

This project delivers authentic retro printer emulation across the MiSTer FPGA ecosystem, saving vector-accurate, multi-page **PDF documents** and sticker images directly to `/media/fat/printers/`.

The system supports three distinct transport channels:
1. **Serial Printing (UART Mode 7 `Printer`)**: Routed directly over the Cyclone V hardware UART (`/dev/ttyS1`).
2. **Parallel Centronics (SPI FIFO `UIO_PRINTER_GET`)**: BRAM FIFO drained every frame via SPI `user_io`.
3. **Shared DDR3 Framebuffer Capture**: High-speed memory-mapped capture for consoles with built-in printers (e.g. **Casio Loopy** sticker buffer at `0x3E400000`).

---

## 2. Current Implementation Status

### A. Standalone Printer Daemon (`mister_printerd`)
* **Location**: `src/` in [alanswx/printeremulation](https://github.com/alanswx/printeremulation)
* **Status**: Complete, fully tested, zero external runtime dependencies.
* **Size**: ~66 KB compiled & stripped ELF ARM binary.
* **Engines Implemented**:
  * **Apple ImageWriter I/II**: 72 & 144 DPI graphics slices (`ESC G`, `ESC P`, `ESC S`), 4-color ribbons (`ESC K <0..6>`), and variable line spacing (`ESC T nn`).
  * **Epson ESC/P**: 9-pin / 24-pin bit-image modes (`ESC K/L/Y/Z`), GrafTrax graphics, and seamless 1-line top offset compensation (`epsonTPS`) for Broderbund's *The Print Shop*.
  * **Coleco Adam SmartWriter**: Bidirectional daisy wheel line printing simulation (`adamBidiBuffer`).
  * **Commodore MPS 803**: PETSCII character glyphs and 7-dot matrix graphics (`CHR$(8)`).
  * **PDF Engine**: Vendored single-file ANSI C [PDFGen](src/pdfgen.h), multi-page document generation, auto-timestamping (`Print_YYYY-MM-DD_HH-MM-SS.pdf`), and 4-second inactivity auto-flush.
* **Test Suite**: 6-test automated matrix (`make test`) passes cleanly on both host and ARM.

### B. MiSTer Main Binary Integration
* **Location**: [alanswx/Main_MiSTer](https://github.com/alanswx/Main_MiSTer)
* **Branches**:
  * `master`: Upstream (`5b3ae64`) + Dani's PR #1321 (Quadra 800 SCSI/Ethernet) + Printer UART Mode.
  * `q800-printer`: Same integrated build as `master`.
  * `printer-support`: Isolated printer patch on top of upstream (clean for PR to `MiSTer-devel`).
* **Main Changes**:
  * `menu.cpp`: Added `"   Printer"` to `config_uart_msg` (Mode 7).
  * `user_io.cpp`: Added `/tmp/uartmode7` detection in `GetUARTMode()`.
  * `Makefile`: Added `mister_printerd` build rule and multi-target compilation.
  * `support/printer/`: Embedded full daemon and parsers.
* **Hardware Deployment on `mister.local`**:
  * Staged Main binary: `/media/fat/MiSTer_quadra_printer` (1.2 MB).
  * Installed daemon: `/media/fat/mister_printerd` (66 KB).
  * Patched script: `/sbin/uartmode` supports Mode 7 (`Printer`) with core autodetection (backup at `/sbin/uartmode.bak`).

### C. Casio Loopy Thermal Sticker Printer (Discovery & Specification)
* **Location**: [`Loopy_MiSTer`](https://github.com/MiSTer-devel/Loopy_MiSTer) & [`docs/casio_loopy_printer.md`](casio_loopy_printer.md)
* **Discovery**: The `Loopy_MiSTer` core (by Jamie Blanks) **already emulates the thermal head DMA and stepper motor in RTL**, reconstructs the 128×112 CMY image, and stores it in DDR3 SDRAM at physical address **`0x3E400000`** (56 KB buffer).
* **Missing Link**: The core displays an on-screen preview, but has no mechanism to dump this buffer to SD card.
* **Plan Complete**: Documented memory layout, 512-entry CMY-to-RGB palette table, and HPS `shmem_map` extraction routine in [`docs/casio_loopy_printer.md`](casio_loopy_printer.md).

---

## 3. Next Action Items for Pair Programmer / Agent

### Action 1: Apple IIgs Core RTL Routing (Serial Printer Port)
1. **Target**: `Apple-IIgs_MiSTer` (`rtl/iigs.sv`).
2. **Task**: Route Z8530 SCC Channel B (Port 1, Printer) to `UART_*` when `uart_mode == 8'd7`:
   ```verilog
   wire uart_is_printer = (uart_mode == 8'd7);
   assign UART_TXD = uart_is_printer ? scc_printer_txd : scc_modem_txd;
   assign UART_RTS = uart_is_printer ? scc_printer_rts : scc_modem_rts;
   assign UART_DTR = uart_is_printer ? scc_printer_dtr : scc_modem_dtr;
   assign scc_printer_rxd = uart_is_printer ? UART_RXD : 1'b1;
   assign scc_printer_cts = uart_is_printer ? UART_CTS : 1'b0;
   assign scc_modem_rxd   = uart_is_printer ? 1'b1 : UART_RXD;
   assign scc_modem_cts   = uart_is_printer ? 1'b0 : UART_CTS;
   ```
3. **Reference Patch**: See [`patches/apple2_slot1_printer.patch`](../patches/apple2_slot1_printer.patch).

### Action 2: Apple IIe Core RTL Routing (Slot 1 Super Serial Card)
1. **Target**: `Apple-II_MiSTer` (`rtl/apple2.v` / `Apple-II.sv`).
2. **Task**: Connect Super Serial Card in Slot 1 to `UART_*` pins when UART mode is `Printer`.

### Action 3: Live Print Shop Benchmark on MiSTer
1. Launch Apple IIe or Apple IIgs on `mister.local`.
2. In the OSD menu: Set `UART Mode: Printer`.
3. Boot Broderbund's *The Print Shop* (or *The Print Shop IIGS*).
4. Configure printer: Apple ImageWriter (or Epson FX-80).
5. Print a greeting card or banner.
6. Verify output: Check `/media/fat/printers/Print_YYYY-MM-DD_HH-MM-SS.pdf` for:
   - Proper dot alignment with zero horizontal or vertical slice seams.
   - Accurate 4-color ribbon reproduction (ImageWriter II).
   - Clean page margins.

### Action 4: Casio Loopy DDR3 Sticker Saver
1. In `Main_MiSTer`: Add `loopy_save_sticker()` using `shmem_map(0x3E400000, 0x10000)`.
2. In `Loopy_MiSTer`: Add OSD trigger button (`P4T[12],Save sticker to SD;`) in `CONF_STR` of `Loopy.sv`.
3. Output sticker images to `/media/fat/printers/Loopy_YYYY-MM-DD_HH-MM-SS.png` or lay out into printable multi-sticker PDF sheets.

---

## 4. Key Reference Files & Addresses

| Asset | Location / Address | Notes |
|---|---|---|
| **Daemon Source** | `printeremulation/src/` | Portable C99, builds standalone |
| **MiSTer Main Fork** | `MainMess/` (`alanswx/Main_MiSTer`) | Branches: `master`, `q800-printer`, `printer-support` |
| **MiSTer Staged Executable** | `/media/fat/MiSTer_quadra_printer` | 1.2 MB, includes Q800 + Printer |
| **MiSTer Daemon Executable** | `/media/fat/mister_printerd` | 66 KB, stripped ARM ELF |
| **MiSTer UART Script** | `/sbin/uartmode` | Handles mode `7` -> spawns `mister_printerd` |
| **Loopy DDR3 Buffer** | Physical address `0x3E400000` | 128×112 CMY pixels, 56 KB total |
| **Loopy Palette Table** | `Loopy_MiSTer/rtl/loopy_print_palette.sv` | 512-entry CMY `{c[2:0], m[2:0], y[2:0]}` -> RGB555 |
| **Output Directory** | `/media/fat/printers/` | Auto-created on first print |
