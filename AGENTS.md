# AGENTS.md - Developer & AI Assistant Guidelines

Welcome to the **MiSTer Retro Printer Emulation** project. This file provides context, architectural constraints, conventions, and guidelines for AI agents and human developers working across this codebase.

---

## 1. Project Mission & High-Level Scope

The goal of this project is to provide authentic retro printer emulation across the **MiSTer FPGA** computer core ecosystem. 
* **Target Platforms**: Starts with **Apple IIe** and **Apple IIgs**, extending to **Coleco Adam**, **Commodore 64**, **ao486**, **Amiga**, and other systems.
* **Target Printers**: **Apple ImageWriter (I, II, LQ)**, **Epson ESC/P (FX-80, MX-80, LQ-800)**, **Coleco Adam SmartWriter**, and **Commodore MPS 803**.
* **Output Standard**: Multi-page, vector-accurate **PDF documents** (Letter 8.5x11" or A4) stored on the SD card at `/media/fat/printers/Print_YYYY-MM-DD_HH-MM-SS.pdf`.
* **Benchmark Goal**: Successfully boot Broderbund's **The Print Shop** on the Apple IIe / Apple IIgs and print greeting cards, banners, and signs with zero graphical artifacts or alignment seams.

---

## 2. Architectural Boundaries: FPGA vs. HPS

```
+------------------------------------+------------------------------------+
|            FPGA FABRIC             |              ARM HPS               |
+------------------------------------+------------------------------------+
| • Emulates retro computer hardware | • Runs embedded Linux on Cyclone V |
| • Exposes serial pins (UART_*)     | • MiSTer Main binary (menu/OSD)    |
| • Exposes parallel Centronics FIFO | • mister_printerd background daemon|
|   over SPI user_io (UIO_PRINTER_GET| • Interprets ESC sequences         |
| • Handles hardware handshakes      | • Assembles and writes PDF files   |
|   (CTS/RTS, DTR/DSR, BUSY/ACK)     |   to SD card (/media/fat/printers/)|
+------------------------------------+------------------------------------+
```

### Critical Rules for FPGA RTL Work:
1. **Preserve Compatibility**: Do not break existing core peripherals. When multiplexing the HPS UART, gate the signals based on `uart_mode` (e.g. `uart_mode == 8'd7` selects Printer).
2. **Serial Core Mapping**:
   - **Apple IIgs**: Route Z8530 SCC Port 1 (Printer Port, Channel B) to `UART_*`.
   - **Apple IIe**: Route Super Serial Card (Slot 1) to `UART_*`.
3. **Parallel Centronics**: For parallel printers (PC LPT1, Amiga, Grappler+), implement a small dual-clock BRAM FIFO (256–512 bytes). When full, assert `BUSY`. Drain via SPI `user_io`.

---

## 3. HPS / Daemon Constraints & Coding Guidelines

The ARM HPS runs a stripped-down embedded Linux environment on the DE10-Nano:
1. **Zero External Runtime Dependencies**:
   - **DO NOT** depend on Ghostscript (`ps2pdf`), CUPS, ImageMagick, Python, or external shared libraries not already present on MiSTer Linux.
   - **DO** use single-file, self-contained libraries like **[PDFGen](references/PDFGen/pdfgen.h)** or FujiNet's streaming PDF generator.
2. **Memory Footprint**:
   - Although the DE10-Nano has 1 GB of RAM, the printer daemon should remain lightweight (< 10 MB total RAM during active rasterization).
   - Standard 144 DPI 24-bit RGB page canvas is `1224 x 1584 x 3` bytes $\approx 5.8\text{ MB}$.
3. **Cross-Compilation**:
   - Target architecture: ARMv7-A (`arm-linux-gnueabihf`).
   - Code must be clean, portable ANSI C / C++17 that compiles warning-free with `gcc` and `clang`.
4. **Job Demarcation**:
   - Trigger page commit on `0x0C` (`FF` Form Feed).
   - Trigger job flush and file write on inactivity timeout (default: 4 seconds).

---

## 4. Key References in the Repository

Before modifying or implementing printer parsers, inspect the corresponding references and documentation:
* **Core Architecture**: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
* **Computer Cores Catalog**: [docs/mister_cores_printer_catalog.md](docs/mister_cores_printer_catalog.md)
* **FujiNet Analysis**: [docs/fujinet_printer_analysis.md](docs/fujinet_printer_analysis.md)
* **ImageWriter Specifications**: [docs/imagewriter_reference.md](docs/imagewriter_reference.md)
* **Epson ESC/P Specifications**: [docs/epson_escp_reference.md](docs/epson_escp_reference.md)
* **Apple II Interface Hardware**: [docs/apple2_printer_interfaces.md](docs/apple2_printer_interfaces.md)
* **MiSTer Integration Plan**: [docs/mister_integration_plan.md](docs/mister_integration_plan.md)

### Reference Codebases:
* [references/fujinet](references/fujinet): FujiNet virtual printer library (`epson_80`, `epson_tps`, `coleco_printer`, `commodoremps803`, `okimate_10`).
* [references/ImageWriter](references/ImageWriter): GSport/KEGS ImageWriter I/II engine (`imagewriter.cpp`).
* [references/PDFGen](references/PDFGen): ANSI C PDF generator (`pdfgen.c` / `pdfgen.h`).

---

## 5. Working Proof of Concept

A verified prototype exists in `prototype/`:
```bash
# Build prototype:
gcc -O2 -Ireferences/PDFGen prototype/printer_to_pdf.c references/PDFGen/pdfgen.c -o prototype/printer_to_pdf

# Test ImageWriter:
./prototype/printer_to_pdf imagewriter references/ImageWriter/Printer.txt output_imagewriter.pdf

# Test Epson ESC/P:
python3 prototype/gen_escp_test.py
./prototype/printer_to_pdf epson test_escp.prn output_escp.pdf
```
Always run and verify these tests before pushing parser modifications.
