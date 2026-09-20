# Casio Loopy Thermal Sticker Printer Emulation Architecture

This document specifies the hardware architecture, software protocols, and integration plan for saving and exporting stickers from the **Casio Loopy** (*My Seal Computer SV-100*) on the **MiSTer FPGA** platform.

---

## 1. System & Hardware Overview

The Casio Loopy (released in Japan in October 1995) is a 32-bit home console powered by a **Hitachi SH-1 (SH7021)** CPU. Its headline feature is an integrated **color thermal transfer sticker ("seal") printer** located on the console's top housing.

### Sticker Cartridges ("Seal Cartridges")
The printer accepts proprietary all-in-one cassettes containing a sticker paper roll and a 3-color thermal transfer ink ribbon:
* **XS-11 (Standard Size)**: 30 mm × 40 mm stickers (100 stickers/cartridge).
* **XS-14 (Mini Size)**: 4 smaller sub-stickers per print (400 stickers total).
* **XS-31 (Video Size)**: 18 mm × 148 mm long horizontal strips intended for VHS tape spine labels (used with the *Magical Shop* video capture cartridge).

### Physical Printing Mechanism
* **4-Phase Stepper Motor**: Steps the paper roll forward and backward through nibbles `0xC, 0x6, 0x3, 0x9`.
* **Thermal Print Head**: Fixed line head with heating elements activated by DMA pulses.
* **3-Pass Subtractive Color**: The mechanism makes three mechanical passes over the paper: **Yellow**, **Magenta**, and **Cyan** (YMC).
* **Mechanical Cycle Time**: Real printing is slow (15–30 seconds per sticker) with audible motor whirring and paper feed calibration.

---

## 2. Software Layer & BIOS Printing Protocol

The Loopy's 32 KB Mask ROM BIOS (mapped at `0x00000000`) provides a high-level printing API so games do not need to bit-bang motor stepping or thermal heating curves directly.

### BIOS Entry Points
* **`0x000006D4` (`_B_PrintOp1`)**: Main entry point called by games to initiate a print job.
  * `GPR[4]` (`R4`): Pointer to uncompressed image buffer in RAM.
  * `GPR[5]` (`R5`): Pointer to 256-entry 16-bit RGB555 palette.
  * `GPR[6]` (`R6`): Packed dimensions `(height << 16) | width` (typically 160×112 or 320×224).
  * `GPR[7]` (`R7`): Option flags.
  * `SP + 4`: Pointer to format byte:
    * High nibble: `pixel_double` (`0` = 1x, `1` = 2x).
    * Low nibble: `pixel_format` (`1` = 16bpp direct RGB555, `3` = 8bpp indexed).
  * `R0` (Return): Status code (`0` = Success, `1` = General Failure, `2` = No Seal Cartridge, `3` = Cancelled, `4` = Paper Jam, `5` = Overheat).
* **`0x00001B76` (`ADDR_MOTOR_MOVE`)**: Paper feed calibration and homing routine.
* **`0x0000115C` (`BiosGetSealType`)**: Polls cartridge notch sense switches to report cassette geometry.

---

## 3. Emulation Implementations: HLE vs. LLE

### High-Level Emulation (Gloopy / LoopyMSE)
Software emulators like `gloopy` intercept the SH-1 CPU execution at the BIOS function boundaries:
1. When `PC == 0x00001B76`, immediately jump to `0x000015FA` (skipping the 2-second mechanical homing sequence).
2. When `PC == 0x000006D4`, read the pixel buffer and palette directly from emulated RAM, render to a PNG/BMP file, set `R0 = 0`, and set `PC = 0x00000FD2` (return).

### Cycle-Accurate RTL Emulation (`Loopy_MiSTer`)
In the official FPGA core ([`MiSTer-devel/Loopy_MiSTer`](https://github.com/MiSTer-devel/Loopy_MiSTer)), developer **Jamie Blanks** implemented full low-level hardware emulation in Verilog:
1. **Hardware Registers ([`rtl/rh7500/vdp_io_print.sv`](file:///Users/alans/dev2/Loopy_MiSTer/rtl/rh7500/vdp_io_print.sv))**:
   * `0x5D030` (`PRINT_SENSORS`): Cassette sense switches, opto sensors, and `ENP` (job enable).
   * `0x5D040` (`PRINT_HEAD_DATA`): 16-bit DMA sink receiving thermal head data.
   * `0x5D042` (`PRINT_MOTOR`): 4-phase stepper drive.
   * `0x5D044` (`PRINT_HEAD_CTRL`): Thermal head row activation.
2. **Audio Synthesis ([`rtl/mainboard/loopy_printer_audio.sv`](file:///Users/alans/dev2/Loopy_MiSTer/rtl/mainboard/loopy_printer_audio.sv))**: Generates motor stepping audio.
3. **Pulse Accumulator ([`rtl/mainboard/loopy_print_capture.sv`](file:///Users/alans/dev2/Loopy_MiSTer/rtl/mainboard/loopy_print_capture.sv))**: Recovers the exact 128×112 sticker image from raw thermal head DMA pulses across the Yellow, Magenta, and Cyan passes, measuring pulse widths to recover 3 bits of intensity per ink (9 bits CMY per pixel).
4. **DDR3 Buffer Store ([`rtl/loopy_print_store.sv`](file:///Users/alans/dev2/Loopy_MiSTer/rtl/loopy_print_store.sv))**: Writes the reconstructed sticker into shared DDR3 SDRAM.
5. **Video Overlay ([`rtl/loopy_print_overlay.sv`](file:///Users/alans/dev2/Loopy_MiSTer/rtl/loopy_print_overlay.sv) & [`rtl/loopy_print_palette.sv`](file:///Users/alans/dev2/Loopy_MiSTer/rtl/loopy_print_palette.sv))**: Displays an on-screen preview (corner or full screen) by mapping `{Cyan[2:0], Magenta[2:0], Yellow[2:0]}` through a 512-entry palette table to RGB555.

---

## 4. The DDR3 Memory Layout

In `rtl/loopy_print_store.sv`:
```verilog
localparam [28:0] DDR_BASE = 29'h07C80000; // Byte 0x3E400000, 64-bit words.
```

* **Physical Address**: `0x3E400000` in DDR3 RAM (outside the savestate area `0x3E000000 - 0x3E100000`).
* **Dimensions**: 128 pixels wide × 112 pixels tall.
* **Row Pitch**: 64 words (64-bit) = 512 bytes per row.
* **Total Buffer Size**: 112 rows × 512 bytes = **57,344 bytes (56 KB)**.
* **64-bit Word Layout**:
  ```
  Bit:  63..56   55..48   47..40   39..32   31..24   23..16   15..8     7..0
        [ Unused ][ Cyan 1 ][ Mag 1 ][ Yell 1 ][ Unused ][ Cyan 0 ][ Mag 0 ][ Yell 0 ]
        Byte 7    Byte 6   Byte 5   Byte 4   Byte 3    Byte 2   Byte 1   Byte 0
        <-------------- Pixel 1 -------------><-------------- Pixel 0 ------------->
  ```
  Each color component is 3 bits (`0..7`).

---

## 5. End-to-End Integration Plan

```
+-------------------------------------------------------------------------------+
|                             FPGA FABRIC (Loopy.sv)                            |
| • Game prints -> VDP printer registers (0x5D030-44)                          |
| • loopy_print_capture reconstructs 128x112 CMY image                          |
| • loopy_print_store writes 56 KB buffer to DDR3 at 0x3E400000                 |
| • On completion, asserts status bit / notification to hps_io                  |
+-------------------------------------------------------------------------------+
                                       │ (Shared DDR3 @ 0x3E400000)
                                       ▼
+-------------------------------------------------------------------------------+
|                            ARM HPS (MiSTer Main)                              |
| • Detects print completion (or user presses OSD "Save sticker to SD")         |
| • Maps physical address 0x3E400000 via shmem_map(0x3E400000, 0x10000)         |
| • Decodes CMY [c:m:y] via 512-entry palette LUT to 24-bit RGB                 |
| • Pipes raster to /media/fat/printers/ or mister_printerd                     |
+-------------------------------------------------------------------------------+
                                       │
                        ┌──────────────┴──────────────┐
                        ▼                             ▼
+-------------------------------+             +---------------------------------+
|         PNG Image             |             |        PDF Sticker Sheet        |
| • 128x112 (or 256x224 scaled) |             | • Multi-sticker grid layout     |
| • 8:7 pixel aspect ratio      |             | • Formatted for adhesive paper  |
| • Saved to SD card            |             | • Vector cutting & peel guides  |
+-------------------------------+             +---------------------------------+
```

### Step 1: Color Palette Decoder (C / C++)
Translate the 512-entry table from `loopy_print_palette.sv` into a static lookup table in C:
```c
static const uint16_t loopy_palette_rgb555[512] = {
    0x7FFF, 0x77B9, 0x77B5, 0x77B1, 0x77AD, 0x77A9, 0x77A5, 0x77C1, // 0x000 - 0x007
    // ... [full 512 entries from loopy_print_palette.sv]
};

static inline void loopy_cmy_to_rgb(uint8_t c, uint8_t m, uint8_t y, uint8_t *r, uint8_t *g, uint8_t *b) {
    uint16_t idx = ((c & 7) << 6) | ((m & 7) << 3) | (y & 7);
    uint16_t c15 = loopy_palette_rgb555[idx];
    *r = ((c15 >> 10) & 0x1F) * 255 / 31;
    *g = ((c15 >>  5) & 0x1F) * 255 / 31;
    *b = ( c15        & 0x1F) * 255 / 31;
}
```

### Step 2: HPS Memory Extraction Routine
```c
#define LOOPY_DDR_STICKER_BASE  0x3E400000
#define LOOPY_STICKER_WIDTH     128
#define LOOPY_STICKER_HEIGHT    112

int loopy_save_sticker(const char *output_png_path) {
    void *base = shmem_map(LOOPY_DDR_STICKER_BASE, 0x10000);
    if (!base) return -1;

    uint8_t rgb24[LOOPY_STICKER_WIDTH * LOOPY_STICKER_HEIGHT * 3];
    uint64_t *words = (uint64_t *)base;

    for (int y = 0; y < LOOPY_STICKER_HEIGHT; y++) {
        for (int x = 0; x < LOOPY_STICKER_WIDTH; x += 2) {
            uint64_t w = words[y * 64 + (x / 2)];
            // Pixel 0
            uint8_t y0 = (w >>  0) & 7;
            uint8_t m0 = (w >>  8) & 7;
            uint8_t c0 = (w >> 16) & 7;
            int idx0 = (y * LOOPY_STICKER_WIDTH + x) * 3;
            loopy_cmy_to_rgb(c0, m0, y0, &rgb24[idx0], &rgb24[idx0 + 1], &rgb24[idx0 + 2]);

            // Pixel 1
            uint8_t y1 = (w >> 32) & 7;
            uint8_t m1 = (w >> 40) & 7;
            uint8_t c1 = (w >> 48) & 7;
            int idx1 = (y * LOOPY_STICKER_WIDTH + x + 1) * 3;
            loopy_cmy_to_rgb(c1, m1, y1, &rgb24[idx1], &rgb24[idx1 + 1], &rgb24[idx1 + 2]);
        }
    }
    // Write PNG or pass to PDF generator
    return save_png(output_png_path, LOOPY_STICKER_WIDTH, LOOPY_STICKER_HEIGHT, rgb24);
}
```

### Step 3: Triggering Mechanism
1. **OSD Option in `Loopy.sv`**:
   Add to the `P4,Printer;` submenu in `CONF_STR`:
   ```verilog
   "P4T[12],Save Sticker to SD;"
   ```
2. **Automatic Detection**:
   The signal `print_show` in `rtl/mainboard/loopy_print_capture.sv` goes high when a print finishes and stays high for 900 frames (15 seconds). Exposing this signal via `hps_io` allows `Main_MiSTer` to automatically save the sticker the instant printing concludes.
