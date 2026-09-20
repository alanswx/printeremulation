# MiSTer Printer Emulation: Technical Architecture & Scope

## 1. Executive Summary

This document specifies the complete end-to-end design for retro printer emulation on the MiSTer FPGA platform, enabling 8-bit and 16-bit computer cores (starting with the **Apple IIe** and **Apple IIgs**) to output authentic print jobs—such as banners, greeting cards, and graphics from **Broderbund's The Print Shop**—directly to high-resolution, vector-accurate **PDF files** saved to the MiSTer SD card (`/media/fat/printers/`).

---

## 2. High-Level System Architecture

```
+-------------------------------------------------------------------------+
|                              FPGA FABRIC                                |
|                                                                         |
|  +------------------------+             +----------------------------+  |
|  |     Apple IIe Core     |             |       Apple IIgs Core      |  |
|  |  Slot 1: Grappler+ (P) |             |  Slot 1 / Port 1: SCC ch B |  |
|  |  Slot 2: Super Serial  |             |  Slot 2 / Port 2: SCC ch A |  |
|  +-----------+------------+             +--------------+-------------+  |
|              | (Parallel or Serial)                    | (Serial)       |
|              v                                         v                |
|  +-------------------------------------------------------------------+  |
|  |                   FPGA Peripheral Transport Layer                 |  |
|  |  Channel 1: Hard HPS UART (sys_top UART_*)                        |  |
|  |  Channel 2: Printer FIFO (SPI user_io or DDR3 Mailbox)            |  |
|  +---------------------------------+---------------------------------+  |
+------------------------------------|------------------------------------+
                                     | Linux Bridge
+------------------------------------v------------------------------------+
|                         ARM HPS (Linux / MiSTer)                        |
|                                                                         |
|  +---------------------+        +------------------------------------+  |
|  |   MiSTer Main Bin   |        |       mister_printerd Daemon       |  |
|  |  - OSD: UART Mode   |------->|  - Reads /dev/ttyS1 (Serial)       |  |
|  |  - "Printer" option | (forks)|  - Reads /dev/mister_prn (FIFO)    |  |
|  |  - Spawns daemon    |        |  - Detects Job Boundaries (FF/time)|  |
|  +---------------------+        +-----------------+------------------+  |
|                                                   |                     |
|                                                   v                     |
|                         +--------------------------------------------+  |
|                         |         Printer Emulation Engines          |  |
|                         |  - Apple ImageWriter I / II / LQ           |  |
|                         |  - Epson ESC/P 9-pin (FX/MX) & 24-pin (LQ) |  |
|                         +-------------------------+------------------+  |
|                                                   |                     |
|                                                   v                     |
|                         +--------------------------------------------+  |
|                         |         PDF Generation (PDFGen)            |  |
|                         |  - Dot-matrix rasterization                |  |
|                         |  - Color ribbon RGB blending               |  |
|                         |  - Multi-page document assembly            |  |
|                         +-------------------------+------------------+  |
|                                                   |                     |
|                                                   v                     |
|                           /media/fat/printers/Print_YYYYMMDD_HHMMSS.pdf |
+-------------------------------------------------------------------------+
```

---

## 3. Communication Transport (FPGA <-> ARM HPS)

### 3.1 Channel 1: Serial Printing via Existing HPS UART (`/dev/ttyS1`)
* **How it works**: The Cyclone V HPS hard peripheral UART1 is routed to `sys_top.v` via `cyclonev_hps_interface_peripheral_uart` (`UART_TXD`, `UART_RXD`, `UART_CTS`, `UART_RTS`, `UART_DTR`, `UART_DSR`).
* **OSD Integration**: In MiSTer Main (`menu.cpp`), add UART Mode `Printer`:
  - Current modes: `0: None`, `1: PPP`, `2: Console`, `3: MIDI`, `4: Modem`, `5: UDP`, `6: SNI`.
  - New mode: `7: Printer` (configurable sub-modes: `ImageWriter 9600`, `ImageWriter 19200`, `Epson FX 9600`).
* **Linux Scripting**: MiSTer calls `uartmode 7` which launches `mister_printerd -d /dev/ttyS1 -b 9600 -p imagewriter`.
* **Hardware Handshake**: The emulated 6551 ACIA (Apple IIe) or Z8530 SCC (Apple IIgs) asserts `CTS`/`RTS` and `DTR`/`DSR` to prevent buffer overflow.

### 3.2 Channel 2: Parallel Port & 2nd Serial via SPI FIFO (`user_io`)
For parallel printers (Centronics / Grappler+ in Apple II Slot 1) or when the user wants simultaneous Modem (BBS on Slot 2) and Printer (Slot 1):
* **FPGA Side**:
  - A simple 512-byte dual-port BRAM FIFO.
  - When the 6502 writes to `$C090` (Grappler Data):
    - Byte is pushed into FIFO.
    - If FIFO level > 480 bytes, Grappler `BUSY` signal is asserted.
    - Grappler `ACK` pulse generated upon latching.
* **SPI Protocol (`hps_io.sv` / `user_io.cpp`)**:
  - Add new SPI command `UIO_PRINTER_GET` (e.g., `0x48`).
  - In `user_io_poll()`:
    ```c
    if (printer_enabled) {
        spi_uio_cmd_cont(UIO_PRINTER_GET);
        uint16_t count = spi_w(0);
        while (count-- > 0) {
            uint8_t byte = spi_w(0);
            printer_feed_byte(byte);
        }
        DisableIO();
    }
    ```
  - At 9600 baud equivalent, data rate is ~1 KB/sec. Draining the FIFO every frame (60 Hz) moves at most ~16 bytes per poll pass, requiring < 2 microseconds of SPI bus time!

---

## 4. Printer Emulation Engines

### 4.1 Apple ImageWriter (I & II)
* **Target Core**: Apple IIe (Super Serial Card in Slot 1) and Apple IIgs (Built-in Port 1).
* **Reference Base**: `GSport` / `KEGS` / `greg-kennedy/ImageWriter`.
* **Key Features**:
  - **Resolution**: 72 DPI (Standard `ESC G`), 144 DPI (High Res `ESC P` / `ESC S`).
  - **Color Ribbon**: ImageWriter II 4-color ribbon (`ESC K <c>`):
    - `0`: Black, `1`: Yellow, `2`: Red, `3`: Blue, `4`: Orange, `5`: Green, `6`: Purple.
  - **Line Advance**: `ESC T nn` (nn/144-inch line spacing).
  - **Form Feed**: `0x0C` (`FF`) advances to next logical page.
* **Print Shop Compatibility**: Print Shop IIe and Print Shop IIGS rely on 8-bit vertical graphics slices. By plotting the pins into an RGB pixel canvas, full-color banners and cards render with 100% historical accuracy.

### 4.2 Epson Dot Matrix (ESC/P and ESC/P2)
* **Target Core**: Apple IIe (Grappler+ Parallel Card in Slot 1), PC/XT, Commodore 64, Atari 8-bit.
* **Reference Base**: `RWAP/PrinterToPDF` & `DOSBox-X`.
* **Key Features**:
  - **9-Pin Graphics Modes**:
    - `ESC K nL nH`: Single-density (60 DPI horizontal, 72 DPI vertical).
    - `ESC L nL nH`: Double-density (120 DPI horizontal, 72 DPI vertical).
    - `ESC Y nL nH`: High-speed double-density (120 DPI horizontal).
    - `ESC Z nL nH`: Quadruple-density (240 DPI horizontal).
    - `ESC * m nL nH`: Generalized bit-image mode.
  - **Line Spacing**:
    - `ESC 3 n`: Set line spacing to n/216-inch (used by graphics software to eliminate gaps between 8-pin passes: `n=24` -> 24/216" = 8/72").
    - `ESC A n`: Set line spacing to n/72-inch.
    - `ESC 2`: Reset to standard 1/6-inch line spacing.
  - **Text Formatting**: Bold (`ESC E`), Italic (`ESC 4`), Underline (`ESC - 1`), Pitch (`ESC P` 10cpi, `ESC M` 12cpi, `SI` condensed).

---

## 5. Job Detection & PDF Assembly

1. **Job Boundary Detection**:
   - **Form Feed (`0x0C`)**: Immediate end-of-page trigger.
   - **Timeout Flush**: If no new bytes are received for 4 seconds (configurable), the current accumulated page is committed and the PDF document is finalized and closed.
2. **PDF Generation via `PDFGen`**:
   - Single-file ANSI-C (`pdfgen.c` / `pdfgen.h`), zero external runtime dependencies.
   - Page dimensions: Standard Letter (612 x 792 points) or A4 (595 x 842 points).
   - Rendered raster buffer is embedded directly using `pdf_add_rgb24()` or `pdf_add_grayscale8()`.
   - Compression: Flate/Zlib stream compression supported for small PDF file sizes.
3. **Storage & User Feedback**:
   - Saved to `/media/fat/printers/Print_YYYYMMDD_HHMMSS.pdf`.
   - OSD pop-up message: `"Printed: Print_20260919_2130.pdf"` displayed on MiSTer screen.
