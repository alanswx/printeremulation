# MiSTer FPGA Computer Cores Printer Interface Catalog

This document catalogs all computer cores in the `MiSTer-devel` repository, detailing their native printer interfaces (Serial vs. Parallel vs. Proprietary Bus), underlying hardware controllers, standard printer models, target emulation protocols, and implementation priority.

---

## 1. Architectural Summary & Taxonomy

Across the 78+ computer platforms supported on MiSTer, printer communication falls into three primary hardware interfaces:

1. **Standard Serial (RS-232 / RS-422)**:
   - Systems: Apple II family (Super Serial Card in Slot 1), Apple IIgs (Port 1), Macintosh Plus / LC, Sinclair QL, Tandy CoCo 2/3.
   - Mechanism: Direct UART data stream (typically 8N1, 9600 or 19200 baud, DTR hardware flow control).
   - MiSTer Fit: Plugs directly into the Cyclone V HPS hardware UART (`/dev/ttyS1`) via `sys_top.v` without any FPGA bridge changes.

2. **Proprietary Serial Peripheral Buses**:
   - Systems: Atari 8-bit (SIO), Commodore 8-bit (IEC), Coleco Adam (AdamNet).
   - Mechanism: High-level bus packets or clocked serial transactions (e.g. AdamNet device #04, Commodore IEC device #4/5, Atari SIO command frame $50/$57).
   - MiSTer Fit: Can be decoded inside the core RTL or translated by the HPS daemon (as implemented in FujiNet).

3. **Standard Centronics Parallel (IEEE 1284 / LPT)**:
   - Systems: IBM PC (XT, 486, Tandy 1000, PCjr), Commodore Amiga, Atari ST, MSX, BBC Micro, Japanese PCs (PC-8801, FM-7, Sharp X68000), Amstrad CPC, SAM Coupé.
   - Mechanism: 8-bit data latch (`D0-D7`) + `/STROBE` output pulse + `BUSY` / `/ACK` input status lines.
   - MiSTer Fit: Requires a shared FPGA BRAM FIFO module (`virtual_centronics.v`) drained via SPI `user_io` commands (`UIO_PRINTER_GET`).

4. **Direct ASIC / Shared DDR3 Framebuffer Capture (Console Integrated Printers)**:
   - Systems: **Casio Loopy** (`Loopy`).
   - Mechanism: The console contains an integrated 3-pass thermal sticker printer driven by custom VDP ASIC registers (`$5D030-$5D044`). The core (`Loopy_MiSTer`) models the stepper motor and thermal pulses in RTL, reconstructs the 128x112 CMY image, and stores it in DDR3 SDRAM at physical address `0x3E400000` (56 KB buffer).
   - MiSTer Fit: Directly mapped on HPS via `shmem_map(0x3E400000, 0x10000)` and converted to 24-bit RGB using the core's 512-entry CMY palette table, exported as PNG or printable PDF sticker sheets.

---

## 2. Complete Cores Catalog

| Core | Home Folder | Native Printer Interface | Hardware Controller / Port | Standard / Target Printers | Emulation Protocol | Priority |
| :--- | :--- | :--- | :--- | :--- | :--- | :---: |
| **Casio Loopy** | `Loopy` | **Built-in Thermal** | Custom VDP ASIC (`$5D030-$5D044`) | Built-in Color Thermal Sticker Printer (XS-11/14/31) | Direct DDR3 Framebuffer (`0x3E400000`) | **Tier 1** |
| **Apple II+/IIe** | `Apple-II` | **Serial** *(or Parallel)* | Slot 1 Super Serial Card (6551 ACIA) *(Grappler+ parallel was optional)* | Apple ImageWriter I/II, Serial Epson | ImageWriter, ESC/P | **Tier 1** |
| **Apple IIgs** | `Apple-IIgs` | **Serial** | Built-in Port 1 (Z8530 SCC ch B) | Apple ImageWriter II (Color), ImageWriter LQ | ImageWriter | **Tier 1** |
| **Apple Mac Plus / LC** | `MACPLUS`, `MacLC` | **Serial** | Built-in Printer Port (Z8530 SCC ch B) | ImageWriter I/II/LQ, LaserWriter | ImageWriter, PostScript | **Tier 1** |
| **ao486 (PC 486)** | `AO486` | **Parallel** & Serial | LPT1 (`$0378`) & COM1/COM2 | Epson FX/LQ, IBM ProPrinter, HP PCL | ESC/P, ProPrinter, PCL | **Tier 1** |
| **IBM PC/XT / EGA** | `PCXT`, `PCXT-EGA` | **Parallel** & Serial | LPT1 (`$0378`) / COM1 (8250 UART) | Epson MX-80, FX-80, IBM Graphics Printer | ESC/P | **Tier 1** |
| **Commodore 64 / 128**| `C64`, `C128` | **Serial (IEC)** *(Parallel via User Port)* | Serial IEC Bus (DIN-6, device 4) *(User Port parallel adapters)* | Commodore MPS 801/803, 1525, Star NL-10 | Commodore MPS / PETSCII, ESC/P | **Tier 1** |
| **Commodore Amiga** | `Amiga` | **Parallel** | DB-25 Centronics Parallel Port (8520 CIA) | Epson FX/LQ, Star LC-10, HP DeskJet | ESC/P, HP PCL, PostScript | **Tier 1** |
| **Atari ST / STe** | `AtariST` | **Parallel** | DB-25 Centronics Parallel Port (PSG YM2149 + MFP) | Epson FX/LQ, Star LC-10, Atari SLM | ESC/P | **Tier 1** |
| **Coleco Adam** | `Adam` | **Serial (AdamNet)** | AdamNet Serial Bus (Device #04) | Coleco SmartWriter Daisy Wheel | Adam SmartWriter | **Tier 1** |
| **Atari 800XL** | `ATARI800` | **Serial (SIO)** | Atari SIO Bus (POKEY serial at 19.2k) | Atari 820, 822, 1025, XMM801, Epson MX-80 | Atari SIO / ESC/P | **Tier 1** |
| **MSX1 / MSX** | `MSX1`, `MSX` | **Parallel** | 14-pin Centronics Parallel Port (standard on all MSX) | MSX matrix printers, Epson ESC/P | ESC/P | **Tier 2** |
| **BBC Micro / Master**| `BBCMicro` | **Parallel** | 26-pin IDC Centronics Parallel (6522 VIA) | Epson FX-80, Star LC-10, Integrex | ESC/P | **Tier 2** |
| **FM-7 / FM-77AV** | `FM-7` | **Parallel** | Centronics Parallel Port | Fujitsu DPL / Epson ESC/P Kanji printers | ESC/P | **Tier 2** |
| **NEC PC-8801** | `PC8801` | **Parallel** | Centronics Parallel (36-pin Amphenol) | NEC PC-PR201 series, Epson ESC/P | ESC/P / PR201 | **Tier 2** |
| **Sharp X68000** | `X68000` | **Parallel** | Half-pitch 36-pin Centronics Parallel | Epson ESC/P, Canon BubbleJet | ESC/P | **Tier 2** |
| **Tandy 1000 / PCjr** | `Tandy1000`, `PCjr` | **Parallel** | Edge/DB-25 Centronics Parallel | Tandy DMP series, IBM Graphics/ProPrinter | ESC/P | **Tier 2** |
| **Sinclair QL** | `QL` | **Serial** | Dual RS-232-C (SER1 / SER2 ports) | Epson FX-80, Qume, Brother daisywheel | ESC/P, ASCII | **Tier 2** |
| **CoCo 2 / 3, Dragon** | `CoCo2`, `COCO3` | **Serial** | 4-pin DIN RS-232 Bit-banger (6821 PIA) | Tandy Line Printer (LP), DMP-105, CGP-115 | ASCII / Tandy DMP | **Tier 2** |
| **Amstrad CPC 6128** | `Amstrad` | **Parallel** | 7-bit Centronics Port (edge connector) | Amstrad DMP 2000 / 3000, Star LC-10 | ESC/P (7-bit) | **Tier 2** |
| **Acorn Archimedes** | `ARCHIE` | **Parallel** | DB-25 Centronics Parallel + RS-423 serial | Epson FX/LQ, PostScript, Canon BJ | ESC/P, PostScript | **Tier 2** |
| **Commodore VIC-20** | `VIC20` | **Serial (IEC)** | Serial IEC Bus (DIN-6) | Commodore VIC-1515, VIC-1525, MPS 801 | Commodore MPS | **Tier 2** |
| **Commodore C16/+4** | `C16` | **Serial (IEC)** | Serial IEC Bus (DIN-6) | Commodore MPS 801, MPS 803 | Commodore MPS | **Tier 2** |
| **Commodore PET** | `PET2001`, `CBM-II` | **IEEE-488 (Parallel)** | IEEE-488 (GPIB) Bus | Commodore 2022, 4022, 8023 | Commodore IEEE | **Tier 3** |
| **TI-99/4A** | `TI-99_4A` | **Parallel** / Serial | Sidecar / PEB RS-232 & Parallel Card | Epson MX-80, TI Thermal Printer | ESC/P | **Tier 3** |
| **SAM Coupé** | `SAMCOUPE` | **Parallel** | Centronics Parallel Port | Epson FX-80, Star LC-10 | ESC/P | **Tier 3** |
| **ZX Spectrum (+3)** | `Spectrum`, `ZXNext` | **Parallel** *(+3 / Next)* | Centronics Parallel Port *(ZX Printer was analog)* | Epson ESC/P, Star LC-10 | ESC/P | **Tier 3** |
| **NeXT** | `NeXT` | **Serial** / Network | RS-423 Serial / Ethernet *(Laser was custom DMA)* | NeXT 400dpi Laser Printer, PostScript | PostScript | **Tier 3** |
| **Apple Lisa** | `Apple-Lisa` | **Serial** | 2x RS-232 ports + Parallel on Lisa 2 | Apple DMP, Apple Daisy Wheel | ImageWriter, ASCII | **Tier 3** |
| **Amstrad PCW** | `Amstrad PCW` | **Dedicated Parallel** | Custom ASIC / Centronics | Bundled Amstrad daisywheel / dot-matrix | Proprietary / ESC/P | **Tier 3** |
| **Oric-1 / Atmos** | `Oric` | **Parallel** | Centronics Parallel Port | MCP-40 4-color pen plotter, Centronics | Centronics, Plotter | **Tier 3** |
| **TRS-80 Model 1** | `TRS-80` | **Parallel** | Expansion Interface Centronics port | Centronics 779, Line Printer II | ASCII / Centronics | **Tier 3** |
| **Sharp MZ** | `SharpMZ` | **Parallel** | Centronics / MZ-1P01 plotter | Sharp MZ-80P series, Centronics | ESC/P, ASCII | **Tier 3** |
| **Acorn Atom / Electron**| `AcornAtom`, `AcornElectron`| **Parallel** | VIA user port / Plus 1 expansion | Epson FX/RX, Seikosha | ESC/P | **Tier 3** |
| **Tatung Einstein** | `TatungEinstein` | **Parallel** & Serial | Centronics Parallel + RS-232 | Epson FX-80, Star | ESC/P | **Tier 3** |
| **SGI Indy** | `SGIIndy` | **Parallel** / Network | IEEE 1284 Parallel (DB-25) | PostScript Laser, HP PCL | PostScript, PCL | **Tier 3** |
| **Altair 8800** | `Altair8800` | **Serial** | S-100 Serial (2SIO) / Current loop | Teletype Model 33 ASR, Diablo daisy wheel | Teletype ASCII | **Tier 3** |
| **BK0011M / Vector06C**| `BK0011M`, `VECTOR06` | **Parallel** | Soviet KR580VV55A (8255) Parallel Port | Robotron, D100, Centronics | ASCII, ESC/P | **Tier 3** |
| **Casio PV-2000** | `Casio_PV-2000` | N/A | No standard printer interface | N/A | N/A | N/A |
| **Bandai RX-78** | `RX78` | N/A | Custom expansion port | N/A | N/A | N/A |
| **Camputers Lynx** | `Lynx48` | Serial / Parallel | RS-232 / parallel expansion | Generic dot matrix | ASCII | Tier 3 |
| **Colour Genie** | `eg2000` | Parallel / Serial | Centronics port + RS-232 | Generic dot matrix | ESC/P, ASCII | Tier 3 |
| **Interact** | `Interact` | N/A | Cassette based | N/A | N/A | N/A |
| **IQ 151** | `IQ151` | Parallel | Centronics expansion module | Robotron | ASCII | Tier 3 |
| **Jupiter Ace** | `Jupiter` | N/A | Edge connector / Forth | N/A | N/A | N/A |
| **Mattel Aquarius** | `AQUARIUS` | Custom Serial | Aquarius 40-col thermal printer | Aquarius Printer | Custom | Tier 3 |
| **MultiComp** | `MultiComp` | Serial | Soft UART console | Generic serial | ASCII | Tier 3 |
| **National JR-100** | `JR100` | N/A | None | N/A | N/A | N/A |
| **PMD 85 / Ondra / Orao**| `PMD85`, `Orao` | Parallel | BT-100 needle printer interface | BT-100, Centronics | Custom / ASCII | Tier 3 |
| **Sord M5** | `Sord M5` | Parallel | Centronics interface | Centronics | ESC/P | Tier 3 |
| **Specialist/MX** | `SPMX` | Parallel | KR580VV55A (8255) | Robotron | ASCII | Tier 3 |
| **Spectravideo SV-328** | `SVI328` | Parallel | Centronics via SVI-801 | Epson ESC/P | ESC/P | Tier 3 |
| **TK2000** | `TK2000` | Serial / Parallel | Built-in RS-232 & Centronics | Epson, ImageWriter | ESC/P | Tier 3 |
| **Tomy Tutor** | `TomyTutor` | Parallel | Tomy printer interface | Tomy Printer | Custom | Tier 3 |
| **TSConf** | `TSConf` | Parallel | LPT port | Epson ESC/P | ESC/P | Tier 3 |
| **VT52** | `VT52` | Serial | RS-232 terminal pass-through | Hardcopy terminal printer | ASCII | Tier 3 |
| **VTech Laser 310** | `Laser` | Serial / Parallel | RS-232 / Centronics | Generic | ASCII | Tier 3 |
| **ZX81** | `ZX81` | Analog / Edge | ZX Printer (metal paper) | ZX Printer | Custom | Tier 3 |
| **Apple I / DEC PDP-1 / EDSAC** | Various | Teletype Serial | Current loop / TTY | Teletype ASR-33 | ASCII | Tier 3 |

---

## 3. Targeted Protocol Breakdown

By implementing four core emulation engines, we cover over 95% of all software:

1. **Apple ImageWriter Engine (I, II, LQ)**:
   - Covers: Apple II+, IIe, IIc, IIgs, Macintosh Plus, Macintosh LC, Apple Lisa.
2. **Epson ESC/P Engine (9-pin & 24-pin)**:
   - Covers: IBM PC/XT, ao486, Commodore Amiga, Atari ST, MSX, BBC Micro, PC-8801, FM-7, Sharp X68000, Sinclair QL, SAM Coupé, Tandy 1000, Amstrad CPC.
3. **Commodore MPS / PETSCII Engine**:
   - Covers: Commodore 64, Commodore 128, VIC-20, C16, Plus/4.
4. **Coleco Adam SmartWriter Engine**:
   - Covers: Coleco Adam.
