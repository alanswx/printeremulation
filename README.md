# MiSTer Retro Printer Emulation

Native retro printer emulation for 8-bit and 16-bit computer cores on the **MiSTer FPGA** platform.

This project enables software running on vintage computer cores—such as Broderbund's **The Print Shop**, PrintMaster, SmartWriter, and AppleWorks on the **Apple IIe**, **Apple IIgs**, **Coleco Adam**, **Commodore 64**, and **PC/XT**—to print directly to high-resolution, vector-accurate **PDF documents** saved to `/media/fat/printers/Print_YYYY-MM-DD_HH-MM-SS.pdf`.

---

## Key Features

* **Authentic Dot-Matrix & Color Emulation**:
  * **Apple ImageWriter I / II / LQ**: 72 & 144 DPI graphics slices (`ESC G`, `ESC P`, `ESC S`), 4-color ribbons (`ESC K <0..6>`), and exact vertical line feeds (`ESC T nn`).
  * **Epson ESC/P (FX-80 / MX-80 / LQ)**: 9-pin and 24-pin bit-image modes (60, 120, 240 DPI), GrafTrax graphics, and seamless line pitch (`ESC 3 24`).
  * **Print Shop Specialization (`epsonTPS`)**: Automatic 1-line top offset compensation inherited from FujiNet to eliminate horizontal/vertical seams in multi-slice banners and greeting cards.
  * **Coleco Adam SmartWriter**: Bi-directional daisy wheel line buffering (`adamBidiBuffer`) for Coleco Adam SmartWriter and AdamCalc.
  * **Commodore MPS 803**: PETSCII character set and bit-image graphics for C64/C128.
* **Pure PDF Output (Zero External Dependencies)**:
  * Self-contained ANSI C [PDFGen](src/pdfgen.h) engine (~25 KB compiled, Public Domain).
  * Direct 24-bit RGB and 8-bit grayscale raster embedding at standard physical page sizes (US Letter 8.5x11" / A4).
  * No Ghostscript, CUPS, ImageMagick, or Python runtime dependencies on MiSTer Linux.
* **Dual Transport Architecture**:
  * **Serial**: Direct routing via Cyclone V HPS hardware UART (`/dev/ttyS1`), managed as UART Mode 7 (`Printer`) via `/sbin/uartmode`.
  * **Parallel (Centronics / LPT)**: FPGA BRAM FIFO drained via SPI `user_io` (`UIO_PRINTER_GET`), allowing simultaneous serial modems and parallel printers.

---

## Directory Layout

```
printeremulation/
├── README.md                           # Project overview and usage
├── AGENTS.md                           # Developer & AI assistant guidelines
├── Makefile                            # Build, test, cross-compile, and deploy targets
├── src/                                # mister_printerd source code
│   ├── mister_printerd.c               # Background daemon, UART/stdin ingestion, auto-flush
│   ├── printer.h                       # Shared types, printer modes, JobState, and API
│   ├── canvas.c                        # 144 DPI 24-bit RGB raster canvas with clipping & blit
│   ├── pdf_writer.c                    # PDF document packaging, multi-page, auto-timestamping
│   ├── font5x7.h                       # Embedded 5x7 bitmap font for ASCII/PETSCII rendering
│   ├── parser_imagewriter.c            # Apple ImageWriter I/II parser & color ribbon mapper
│   ├── parser_escp.c                   # Epson ESC/P & Print Shop TPS seam-free parser
│   ├── parser_adam.c                   # Coleco Adam SmartWriter daisy wheel parser
│   ├── parser_mps803.c                 # Commodore MPS 803 dot-matrix parser
│   ├── pdfgen.c                        # Single-file ANSI C PDF generation library
│   └── pdfgen.h                        # PDFGen header
├── docs/                               # Comprehensive technical documentation
│   ├── ARCHITECTURE.md                 # Full end-to-end architectural specification
│   ├── mister_cores_printer_catalog.md # Survey & catalog of all 78+ MiSTer computer cores
│   ├── fujinet_printer_analysis.md     # Deep dive into FujiNet's printer emulation suite
│   ├── imagewriter_reference.md        # Apple ImageWriter I/II/LQ command reference
│   ├── epson_escp_reference.md         # Epson ESC/P 9-pin/24-pin command reference
│   ├── apple2_printer_interfaces.md    # Super Serial Card, Grappler+, & IIgs SCC interfaces
│   └── mister_integration_plan.md      # MiSTer Main C++ and RTL integration roadmap
├── patches/                            # Integration patches for MiSTer repositories
│   ├── mister_main_printer.patch       # Main_MiSTer patch (OSD menu, user_io, support/printer)
│   └── apple2_slot1_printer.patch      # Apple II SSC Slot 1 UART bridge RTL patch
├── scripts/                            # Helper and system integration scripts
│   ├── uartmode                        # Host test script for uartmode transitions
│   └── uartmode_mister_patched         # Drop-in /sbin/uartmode for MiSTer Linux
├── tests/                              # Automated test suite
│   ├── generate_test_streams.py        # Stream generator for ImageWriter, ESC/P, Adam, MPS 803
│   └── samples/                        # Authentic printer stream dumps (The Print Shop, etc.)
└── prototype/                          # Original standalone proof of concept
    ├── printer_to_pdf.c                # Early prototype parser
    └── gen_escp_test.py                # ESC/P test pattern script
```

---

## Building & Testing

### 1. Build and Run Local Test Suite (Host)
```bash
# Build host binary and execute 5-printer test matrix
make test
```
The test suite generates multi-page PDF output for:
1. Apple ImageWriter I (authentic *The Print Shop* raw dump)
2. Apple ImageWriter II 4-color ribbon graphics
3. Epson ESC/P with `epsonTPS` seamless line pitch
4. Coleco Adam SmartWriter daisy wheel line buffering
5. Commodore MPS 803 PETSCII and 7-dot graphics

Output PDFs are placed into `printers/`.

### 2. Cross-Compile for MiSTer ARM
If an ARM cross compiler is on your `$PATH`, `make arm` will use it directly. Otherwise, it automatically falls back to Docker:
```bash
make arm
```
Produces stripped binary: `build/mister_printerd.arm` (~66 KB).

### 3. Deploy to MiSTer
```bash
make deploy
```
Copies `mister_printerd` to `root@mister.local:/media/fat/mister_printerd` and sets execute permissions.

---

## Status & Roadmap

- [x] Protocol research and documentation (ESC/P, ImageWriter, Centronics, SSC, SCC).
- [x] Survey of all 78+ MiSTer computer cores and interface requirements.
- [x] Working proof of concept generating valid PDFs from raw streams.
- [x] Implement lightweight `mister_printerd` daemon for ARM Linux.
- [x] Implement complete parser matrix (ImageWriter I/II, ESC/P, Coleco Adam, Commodore MPS 803).
- [x] Multi-page PDF output with auto-timestamping and 4-second flush timeout.
- [x] MiSTer Main integration (UART Mode 7 `Printer` in OSD and `/sbin/uartmode`).
- [x] Upstream fork synchronization with `Main_MiSTer` and Dani's Quadra 800 PR.
- [ ] Connect SCC Printer port to `UART_*` in the `Apple-IIgs` core.
- [ ] Connect Super Serial Card in Slot 1 to `UART_*` in the `Apple-II` core.
- [ ] End-to-end live testing with The Print Shop on hardware.
