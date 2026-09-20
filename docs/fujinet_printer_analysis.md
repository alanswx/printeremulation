# FujiNet Virtual Printer Emulation Analysis

## 1. Executive Summary

FujiNet (created by Thom Cherryhomes, Jeff Piepmeier, Oscar Fowler, and the FujiNet team) contains one of the most mature, comprehensive, and well-tested retro printer emulation suites in existence. Designed originally for the ESP32 to emulate Atari SIO, Commodore IEC, Coleco AdamNet, and Apple II SmartPort peripherals, its printer subsystem (`lib/printer-emulator/`) supports a vast collection of vintage printers, parsing their control codes and producing PDF, SVG, PNG, and HTML outputs.

The entire source code has been checked out into [`references/fujinet/`](file:///Users/alans/dev2/printeremulation/references/fujinet).

---

## 2. Emulated Printers in FujiNet

| Printer Model | Source Files | Description & Features | Applicable MiSTer Cores |
| :--- | :--- | :--- | :--- |
| **Epson MX-80 / FX-80** | [`epson_80.cpp`](file:///Users/alans/dev2/printeremulation/references/fujinet/lib/printer-emulator/epson_80.cpp), [`epson_80.h`](file:///Users/alans/dev2/printeremulation/references/fujinet/lib/printer-emulator/epson_80.h) | Complete ESC/P command set: Pica, Elite, Compressed, Expanded, Underline, Italic, Emphasized, Double-strike. Line spacing: `ESC 2`, `ESC 0`, `ESC 1`, `ESC 3 n`, `ESC A n`. Graphics: 60/120/240 DPI (`ESC K`, `ESC L`, `ESC Y`, `ESC Z`). | Apple II, PC/XT, Atari, C64 |
| **Epson (The Print Shop)** | [`epson_tps.h`](file:///Users/alans/dev2/printeremulation/references/fujinet/lib/printer-emulator/epson_tps.h) | Derived from `epson80`. Overrides `post_new_file()` to adjust vertical origin (`pdf_dY = lineHeight;`) to perfectly eliminate the vertical seam in Print Shop banners/cards. | Apple II, Apple IIgs, C64, Atari |
| **Coleco Adam SmartWriter** | [`coleco_printer.cpp`](file:///Users/alans/dev2/printeremulation/references/fujinet/lib/printer-emulator/coleco_printer.cpp), [`coleco_printer.h`](file:///Users/alans/dev2/printeremulation/references/fujinet/lib/printer-emulator/coleco_printer.h) | Emulates the AdamNet daisy wheel printer ($04), including bi-directional printing buffer (`adamBidiBuffer`) required for SmartWriter word processor and AdamCalc. | Coleco Adam |
| **Commodore MPS 803** | [`commodoremps803.cpp`](file:///Users/alans/dev2/printeremulation/references/fujinet/lib/printer-emulator/commodoremps803.cpp), [`commodoremps803.h`](file:///Users/alans/dev2/printeremulation/references/fujinet/lib/printer-emulator/commodoremps803.h) | Commodore matrix printer supporting PETSCII character set, reverse mode, and bit-image graphics. | C64 / C128 / VIC-20 |
| **Okidata OKIMATE 10** | [`okimate_10.cpp`](file:///Users/alans/dev2/printeremulation/references/fujinet/lib/printer-emulator/okimate_10.cpp), [`okimate_10.h`](file:///Users/alans/dev2/printeremulation/references/fujinet/lib/printer-emulator/okimate_10.h) | Novel color thermal wax transfer printer with multi-pass color ribbons. | Apple II, C64, Atari, PC |
| **Atari 1020 Plotter** | [`svg_plotter.cpp`](file:///Users/alans/dev2/printeremulation/references/fujinet/lib/printer-emulator/svg_plotter.cpp), [`atari_1020.cpp`](file:///Users/alans/dev2/printeremulation/references/fujinet/lib/printer-emulator/atari_1020.cpp) | 4-color pen plotter (Alps mechanism). Renders calligraphic text and vector drawing commands directly into vector SVG. | Atari 8-bit |
| **Atari 820 / 822 / 825 / 1025 / 1027 / 1029 / XMM801 / XDM121** | `atari_*.cpp` | Full lineup of Atari thermal, matrix, and daisy-wheel printers. | Atari 8-bit |

---

## 3. Key Technical Discoveries & Solutions from FujiNet

### 3.1 The Print Shop Seam Fix (`epsonTPS`)
In `epson_tps.h`:
```cpp
class epsonTPS : public epson80
{
protected:
    virtual void post_new_file() override
    {
        epson80::post_new_file();
        pdf_dY = lineHeight; // go up one line for The Print Shop
    };
```
Broderbund's *The Print Shop* initializes printing with a preliminary carriage return and line feed before starting graphic slices. In standard printers, this pushes the printhead down by 1 text line. FujiNet's `epsonTPS` detects this and adjusts the top-of-page offset by exactly `lineHeight`, ensuring that multi-slice graphics (cards, banners) start at the true margin without an extra gap!

### 3.2 FujiNet's Streaming PDF Generation Strategy (`pdf_printer.cpp`)
* On the ESP32 (which has very little RAM), FujiNet cannot afford an 8 MB bitmap canvas in memory.
* **Jeff Piepmeier's Solution**:
  - Emits PDF stream operators directly to a file (`%PDF-1.4`, objects, xref table).
  - Uses embedded TrueType / Type 3 fonts located in [`data/webui/common/f/`](file:///Users/alans/dev2/printeremulation/references/fujinet/data/webui/common/f/).
  - For graphics, uses an 8-pin font (`F15`) where characters `1..8` correspond to the 8 vertical pins of a dot-matrix head.
* **MiSTer Advantage**:
  - MiSTer has **1 GB of DDR3 RAM**.
  - We have the option to:
    1. Use raster buffering (via `PDFGen`, zero font dependencies, pixel-perfect bitmap embedding).
    2. Or port FujiNet's vector PDF streaming method with embedded fonts for crisp vector text.
    3. Or combine both: use FujiNet's state machines for parsing and `PDFGen` for single-file, zero-dependency PDF rendering!

### 3.3 Platform Bus Adapters
FujiNet organizes communication into clean adapter classes:
* `rs232Printer` (`lib/device/rs232/`): Buffers incoming serial bytes and feeds `_pptr->process()`. This is **identical** to reading `/dev/ttyS1` on MiSTer!
* `adamPrinter` (`lib/device/adamnet/`): Hooks into AdamNet packets (device ID 4 = printer), feeding `coleco_printer`.
* `iwmPrinter` (`lib/device/iwm/`): Hooks into Apple II SmartPort character calls.

---

## 4. Immediate Value for Our MiSTer Implementation

1. **Epson ESC/P + Print Shop Parser**: We can adapt `epson_80.cpp` and `epson_tps.h` directly for our MiSTer printer daemon.
2. **Coleco Adam SmartWriter**: Since you have `ColecoAdam_MiSTer` in your dev tree, we can easily add Adam printer support using `coleco_printer.cpp` and `adamBidiBuffer`!
3. **Commodore MPS 803**: Provides instant C64 printer support.
4. **Color Printing (Okimate 10 & ImageWriter II)**: Validates color ribbon multi-pass logic.
