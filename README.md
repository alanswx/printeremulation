# MiSTer Retro Printer Emulation

Native retro printer emulation for 8-bit and 16-bit computer cores on the **MiSTer FPGA** platform.

This project enables software running on vintage computer cores—such as Broderbund's **The Print Shop**, PrintMaster, SmartWriter, and AppleWorks on the **Apple IIe**, **Apple IIgs**, **Coleco Adam**, **Commodore 64**, and **PC/XT**—to print directly to high-resolution, vector-accurate **PDF documents** saved on the SD card (`/media/fat/printers/`).

---

## Key Features

* **Authentic Dot-Matrix & Color Emulation**:
  * **Apple ImageWriter I / II / LQ**: 72 & 144 DPI graphics slices (`ESC G`, `ESC P`, `ESC S`), 4-color ribbons (`ESC K <0..6>`), and exact vertical line feeds (`ESC T nn`).
  * **Epson ESC/P (FX-80 / MX-80 / LQ)**: 9-pin and 24-pin bit-image modes (60, 120, 240 DPI), GrafTrax graphics, and seamless line pitch (`ESC 3 24`).
  * **Print Shop Specialization (`epsonTPS`)**: Automatic 1-line top offset compensation inherited from FujiNet to eliminate horizontal/vertical seams in multi-slice banners and greeting cards.
  * **Coleco Adam SmartWriter**: Bi-directional daisy wheel line buffering (`adamBidiBuffer`) for Coleco Adam SmartWriter and AdamCalc.
  * **Commodore MPS 803**: PETSCII character set and bit-image graphics for C64/C128.
* **Pure PDF Output (Zero External Dependencies)**:
  * Uses the single-file ANSI C [PDFGen](references/PDFGen) engine (~25 KB compiled, Public Domain).
  * Direct 24-bit RGB and 8-bit grayscale raster embedding at standard physical page sizes (US Letter 8.5x11" / A4).
  * No Ghostscript, CUPS, ImageMagick, or Python dependencies required on MiSTer Linux.
* **Dual Transport Architecture**:
  * **Serial**: Direct routing via Cyclone V HPS hardware UART (`/dev/ttyS1`), controlled by MiSTer's `uartmode` subsystem.
  * **Parallel (Centronics / LPT)**: FPGA BRAM FIFO drained via SPI `user_io` (`UIO_PRINTER_GET`), allowing simultaneous serial modems and parallel printers.

---

## Directory Layout

```
printeremulation/
├── README.md                           # This document
├── AGENTS.md                           # Developer & agent pairing instructions
├── docs/                               # Comprehensive technical documentation
│   ├── ARCHITECTURE.md                 # Full end-to-end architectural specification
│   ├── mister_cores_printer_catalog.md # Survey & catalog of all 78+ MiSTer computer cores
│   ├── fujinet_printer_analysis.md     # Deep dive into FujiNet's printer emulation suite
│   ├── imagewriter_reference.md        # Apple ImageWriter I/II/LQ command reference
│   ├── epson_escp_reference.md         # Epson ESC/P 9-pin/24-pin command reference
│   ├── apple2_printer_interfaces.md    # Super Serial Card, Grappler+, & IIgs SCC interfaces
│   └── mister_integration_plan.md      # MiSTer Main C++ and RTL integration roadmap
├── prototype/                          # Working proof-of-concept implementation
│   ├── printer_to_pdf.c                # Standalone parser & PDF generator
│   └── gen_escp_test.py                # Synthetic ESC/P test generator
└── references/                         # Cloned open-source reference implementations
    ├── fujinet/                        # FujiNet virtual printer library & bus adapters
    ├── ImageWriter/                    # GSport/KEGS ImageWriter C++ engine
    ├── PrinterToPDF/                   # RWAP RetroPrinter ESC/P C engine
    └── PDFGen/                         # Single-file ANSI C PDF generation library
```

---

## Quickstart: Running the Proof of Concept

A standalone prototype is included in `prototype/` that compiles with any standard C compiler on macOS, Linux, or ARM:

```bash
# 1. Compile the prototype
gcc -O2 -Ireferences/PDFGen prototype/printer_to_pdf.c references/PDFGen/pdfgen.c -o prototype/printer_to_pdf

# 2. Test ImageWriter print stream (converts raw Print Shop dump to PDF)
./prototype/printer_to_pdf imagewriter references/ImageWriter/Printer.txt output_imagewriter.pdf

# 3. Test Epson ESC/P print stream (generates synthetic 8-pin graphic passes)
python3 prototype/gen_escp_test.py
./prototype/printer_to_pdf epson test_escp.prn output_escp.pdf
```

---

## Status & Roadmap

- [x] Protocol research and documentation (ESC/P, ImageWriter, Centronics, SSC, SCC).
- [x] Survey of all 78+ MiSTer computer cores and interface requirements.
- [x] Working proof of concept generating valid PDFs from raw streams.
- [x] FujiNet printer emulation architecture analysis and font extraction.
- [ ] Implement `mister_printerd` daemon for the DE10-Nano ARM Linux system.
- [ ] Add `UART Mode: Printer` to MiSTer Main (`menu.cpp` & `user_io.cpp`).
- [ ] Connect SCC Printer port to `UART_*` in the `Apple-IIgs` core.
- [ ] Connect Super Serial Card in Slot 1 to `UART_*` in the `Apple-II` core.
- [ ] Live print testing with Broderbund's The Print Shop.
