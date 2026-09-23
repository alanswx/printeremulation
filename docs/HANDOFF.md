# MiSTer Retro Printer Emulation: Project Handoff & Status

**Updated**: September 22, 2026  
**Repositories**:
* Standalone Daemon & Specs: [`/Users/alans/dev2/printeremulation`](https://github.com/alanswx/printeremulation)
* MiSTer Main Fork: [`/Users/alans/dev2/MainMess`](https://github.com/alanswx/Main_MiSTer)
* Apple IIgs Core: [`/Users/alans/dev2/Apple-IIgs_MiSTer`](https://github.com/MiSTer-devel/Apple-IIgs_MiSTer)
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

## 2. Major Milestone: Live Hardware Verification (Apple IIgs)

The Apple IIgs core has been **fully verified end-to-end on live MiSTer hardware** using Broderbund's *The Print Shop* configured for an Apple ImageWriter II with 4-color ribbon.

### Verified Print Runs (Archived in `debug_archive/`):
1. **"GO TEAM!" 4-Page Continuous Banner**:
   - **Stream Size**: 1,088,153 bytes captured over UART at 9600 baud with **0 framing errors**.
   - **Raw Stream Capture**: `debug_archive/PrintShop_Color_Raw_1.08MB.bin`
   - **PDF Output**: [`debug_archive/Print_2026-09-22_23-09-44.pdf`](../debug_archive/Print_2026-09-22_23-09-44.pdf) (4.28 MB, 4 pages).
   - **Rendered PNGs**: [`debug_archive/pages/`](../debug_archive/pages/) (`banner-1.png` through `banner-4.png`).
   - Features: Full-color soccer ball, yellow lightning bolts, red burst, blue clouds, decorative side borders, seamless inter-page lettering.

---

## 3. Parser & Daemon Breakthroughs

During testing on real hardware, several key fixes were developed and deployed into [`src/parser_imagewriter.c`](../src/parser_imagewriter.c) and [`src/printer.h`](../src/printer.h):

1. **Exact 120 DPI Integer Column Alignment**:
   ImageWriter graphics lines often use 120 DPI units (`ESC S 0960`), which caused cumulative rounding errors when placed onto a 144 DPI canvas. The parser now computes absolute column positions with pure integer math:
   ```c
   int col_x    = margin_left + (int)(((long)(iw.start_col + iw.g_cols_read) * c->dpi) / unit);
   int col_next = margin_left + (int)(((long)(iw.start_col + iw.g_cols_read + 1) * c->dpi) / unit);
   int dot_w    = col_next - col_x;
   ```
   This guarantees that color ribbon passes (Yellow, Cyan, Magenta, Black) overlap with single-dot precision.
2. **Super Serial Card Initialization Filtering**:
   The SSC firmware sends `<Ctrl-I> Z` (reset) and `<Ctrl-I> 80N` (disable line wrap / auto LF). The parser implements a dedicated state machine (`IW_STATE_TAB`, `IW_STATE_TAB_NUM`) to silently swallow these commands without misinterpreting them as text.
3. **Paper Motion Enhancements**:
   Implemented `ESC F nnnn` (absolute horizontal position), `ESC f` (forward half-line feed, 1/12"), and `ESC r` (reverse line feed, 1/6").
4. **Adaptive Timeout**:
   Increased `DEFAULT_TIMEOUT_SEC` in [`src/printer.h`](../src/printer.h) to 12 seconds so 65816 CPU rasterization pauses do not trigger premature page commits.

---

## 4. Current Implementation Status

### A. Standalone Printer Daemon (`mister_printerd`)
* **Location**: `src/` in [alanswx/printeremulation](https://github.com/alanswx/printeremulation)
* **Status**: Complete, production-ready, verified on hardware, zero external dependencies.
* **Size**: ~66 KB stripped ARM ELF binary.
* **Live Deployment**: Active on `mister.local` (`/media/fat/mister_printerd`).

### B. MiSTer Main Binary Integration
* **Location**: [alanswx/Main_MiSTer](https://github.com/alanswx/Main_MiSTer)
* **Features**: Added UART Mode 7 (`Printer`) to OSD menu, `/sbin/uartmode` script automation, and embedded daemon support.
* **Hardware Deployment**: Staged at `/media/fat/MiSTer_quadra_printer` on `mister.local`.

### C. Apple IIgs Core RTL Routing
* **Location**: [Apple-IIgs_MiSTer](https://github.com/MiSTer-devel/Apple-IIgs_MiSTer)
* **Status**: Hardware UART connects to Z8530 SCC Channel B (`scc_txd_b`, Slot 1 / Printer) and Channel A (`scc_txd_a`, Slot 2 / Modem). Verified streaming at 9600 baud with 0 framing errors.

### D. Casio Loopy Thermal Sticker Printer
* **Location**: [`Loopy_MiSTer`](https://github.com/MiSTer-devel/Loopy_MiSTer) & [`docs/casio_loopy_printer.md`](casio_loopy_printer.md)
* **Status**: Reverse-engineered RTL buffer at `0x3E400000` (128×112 CMY, 56 KB) and 512-entry palette table. Ready for HPS extractor tool.

---

## 5. Next Action Items for Pair Programmer / Agent

### Action 1: Apple IIe Core RTL Routing (Slot 1 Super Serial Card)
1. **Target**: `Apple-II_MiSTer` (`rtl/apple2.v` / `Apple-II.sv`).
2. **Task**: Apply [`patches/apple2_slot1_printer.patch`](../patches/apple2_slot1_printer.patch) to route Slot 1 SSC to `UART_*` when UART mode is `Printer`.
3. **Test**: Run Apple IIe version of *The Print Shop* or *AppleWorks*.

### Action 2: Casio Loopy DDR3 Sticker Saver
1. In `Main_MiSTer`: Implement `loopy_save_sticker()` using `shmem_map(0x3E400000, 0x10000)`.
2. Save raw sticker buffer to `/media/fat/printers/Loopy_YYYY-MM-DD_HH-MM-SS.png` or PDF.

### Action 3: BRAM Centronics FIFO for Parallel Cores
1. Implement `virtual_centronics.v` dual-clock FIFO for `ao486` (LPT1), `Amiga`, and `PC-88`.
2. Drain via SPI `user_io` (`UIO_PRINTER_GET`).
