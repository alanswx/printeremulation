# MiSTer Printer Emulation: Integration & Implementation Plan

This document outlines the step-by-step roadmap to integrate printer emulation into the MiSTer binary and FPGA cores.

---

## 1. MiSTer Main Binary Changes (`menu.cpp`, `user_io.cpp`, `user_io.h`)

### 1.1 OSD Menu Additions (`menu.cpp`)
1. **Extend `config_uart_msg`**:
   ```cpp
   // Current:
   const char *config_uart_msg[] = { "      None", "       PPP", "   Console", "      MIDI", "     Modem", "UDP", "SNI"};
   // Proposed:
   const char *config_uart_msg[] = { "      None", "       PPP", "   Console", "      MIDI", "     Modem", "UDP", "SNI", "   Printer"};
   ```
2. **Printer Submenu (`MENU_PRINTER`)**:
   - **Printer Model**: `Apple ImageWriter II` | `Epson FX-80 (9-pin)` | `Epson LQ-800 (24-pin)`
   - **Output Format**: `PDF (Vector/Dots)` | `PDF + PNG Thumbnail` | `Text Dump`
   - **Paper Size**: `US Letter (8.5x11")` | `A4 (210x297mm)` | `Continuous Fanfold`
   - **Job Timeout**: `2s` | `4s` | `8s` | `Manual / Form Feed Only`
   - **Baud Rate**: `9600` (Default for ImageWriter/SSC) | `19200` (IIgs) | `4800` | `2400`
3. **Configuration Persistence**:
   - Saved to `/media/fat/config/printer.<core_name>` or within the standard INI/CFG structure.

### 1.2 Linux Scripts & Daemon Execution (`user_io.cpp`)
* When UART mode `7` (Printer) is activated:
  ```cpp
  // In SetUARTMode(int mode):
  char cmd[128];
  sprintf(cmd, "uartmode %d", mode);
  system(cmd);
  ```
* In the MiSTer Linux root filesystem (`/sbin/uartmode`):
  - When called with `uartmode 7`:
    ```bash
    killall mister_printerd 2>/dev/null
    /media/fat/Scripts/mister_printerd -d /dev/ttyS1 -b "$BAUD" -m "$MODEL" -o /media/fat/printers &
    ```

---

## 2. The `mister_printerd` Daemon

A standalone, lightweight C daemon compiled with the MiSTer ARM toolchain:
* **Binary Footprint**: < 60 KB.
* **Dependencies**: Standard POSIX libc and `pdfgen.c` (zero external shared libraries).
* **Execution Loop**:
  1. Open `/dev/ttyS1` at specified baud (e.g. 9600, 8N1) with hardware flow control (`CRTSCTS`).
  2. Maintain a `select()` / `poll()` loop with a 4-second timeout timer.
  3. When bytes arrive:
     - Reset the inactivity timer.
     - Feed bytes to active printer parser (`imagewriter` or `escp`).
     - If byte is `0x0C` (`FF` Form Feed), commit current page to PDF.
  4. When timer expires (or SIGTERM / core unload):
     - If page has uncommitted dots, commit final page.
     - Finalize and write `/media/fat/printers/Print_YYYY-MM-DD_HH-MM-SS.pdf`.
     - Notify user via MiSTer socket or OSD announcement.

---

## 3. FPGA Core Integration: Apple IIe & Apple IIgs

### 3.1 Apple IIgs Core (`Apple-IIgs`)
* The IIgs has two Z8530 SCC serial channels:
  - Channel A: Modem Port (Port 2).
  - Channel B: Printer Port (Port 1).
* **Routing Strategy**:
  - In `iigs.sv`, add a simple multiplexer for `sys_top` UART:
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
  - Result: Switching MiSTer UART mode seamlessly toggles between Modem (BBS/tcpser) and Printer (ImageWriter/Print Shop) without requiring any hardware or pin changes!

### 3.2 Apple IIe Core (`Apple-II`)
* Current state:
  - Slot 2: Super Serial Card (SSC) routed to `UART_*`.
  - Slot 1: Currently empty.
* **Phase 1 (Serial)**:
  - Route Slot 1 Super Serial Card to `UART_*` when UART mode is `Printer` (or allow user to configure Slot 1 as Printer SSC).
  - Select ImageWriter or Serial Epson in The Print Shop.
* **Phase 2 (Parallel Grappler+ via SPI FIFO)**:
  - Implement `grappler_plus.v` in Slot 1.
  - Latch 6502 writes to `$C090` into a small BRAM FIFO.
  - Read bytes over SPI in `user_io_poll()` via `UIO_PRINTER_GET`.
### 3.3 Casio Loopy Core (`Loopy_MiSTer`)
* Current state:
  - The core already emulates the thermal head DMA and 4-phase stepper motor in RTL.
  - The 128x112 CMY sticker image is reconstructed in RTL and written to DDR3 at `0x3E400000` (`loopy_print_store.sv`, 56 KB buffer).
  - The core displays an on-screen preview overlay, but cannot save to SD card.
* **HPS Extraction Plan**:
  1. Add OSD trigger button to `CONF_STR` in `Loopy.sv`:
     `"P4T[12],Save sticker to SD;"`
     and/or expose the `print_show` signal to `hps_io` for automatic saving upon print completion.
  2. In `Main_MiSTer` (or `mister_printerd`):
     - Map `0x3E400000` via `shmem_map(0x3E400000, 0x10000)`.
     - Decode 3-bit CMY dot data into 24-bit RGB using the 512-entry palette LUT.
     - Save as `/media/fat/printers/Loopy_YYYY-MM-DD_HH-MM-SS.png` or lay out into a multi-sticker PDF sheet.

---

## 4. Testing & Verification Checklist

- [x] **PDF Generation Engine**: Verify `PDFGen` compiles cleanly without dependencies and produces valid multi-page PDF documents.
- [x] **ImageWriter Protocol Verification**: Successfully parse 72/144 DPI graphics slices (`ESC G / ESC P`) and line spacing (`ESC T nn`) from real sample data.
- [x] **Epson ESC/P Protocol Verification**: Successfully parse 8-pin graphics (`ESC K / ESC L / ESC Y / ESC Z`) and line spacing (`ESC 3 24`) into seamless raster pages.
- [x] **Coleco Adam & Commodore MPS 803**: Verified daisy wheel and PETSCII 7-dot matrix parsers.
- [x] **Cross-compilation**: Built `mister_printerd` (66 KB) with the `arm-none-linux-gnueabihf` toolchain for the DE10-Nano.
- [x] **Integration with MiSTer Main**: Added UART Mode 7 to `Main_MiSTer` (`menu.cpp`, `user_io.cpp`, `Makefile`), built combined binary with Dani's Quadra 800 PR #1321, and pushed to `alanswx/Main_MiSTer`.
- [x] **Casio Loopy Reverse Engineering**: Mapped the RTL DDR3 print buffer at `0x3E400000`, verified CMY 3-pass packing and palette decoding.
- [ ] **Live Hardware Print Test**: Boot The Print Shop on Apple IIe and Apple IIgs, print a greeting card and banner, and verify output PDF formatting and color fidelity.
- [ ] **Casio Loopy HPS Saver Implementation**: Wire `shmem_map(0x3E400000, 0x10000)` into `Main_MiSTer` and add OSD save trigger to `Loopy_MiSTer`.
