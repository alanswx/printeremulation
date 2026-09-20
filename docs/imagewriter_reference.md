# Apple ImageWriter I & II Technical Reference Summary

## 1. Serial Protocol & Hardware Interface

* **Baud Rates**: 
  - Standard: 9600 baud (Switch SW1-1, SW1-2).
  - High-Speed / Apple IIgs: 19200 baud.
* **Format**: 8 Data bits, 1 Stop bit, No parity (8N1).
* **Handshake / Flow Control**:
  - Hardware Flow Control: DTR (Pin 4 on DB-25, Pin 1 on Mini-DIN 8) drops low when the internal print buffer is nearly full (typically when < 256 bytes remain in a 2KB buffer).
  - Software Flow Control: XON (`0x11`) / XOFF (`0x13`) supported on some firmware versions.

---

## 2. Core Control Codes

| Code (Hex) | ASCII | Description |
| :--- | :--- | :--- |
| `0x08` | `BS` | Backspace 1 character width. |
| `0x09` | `HT` | Horizontal tab (default 8 character columns). |
| `0x0A` | `LF` | Line feed (advances paper by current line spacing). |
| `0x0C` | `FF` | **Form Feed** (ejects paper to top of next form / page). |
| `0x0D` | `CR` | Carriage return (moves print head to left margin). |
| `0x0E` | `SO` | Double-width / Expanded text mode on. |
| `0x0F` | `SI` | Condensed text mode on (15 cpi). |
| `0x1B` | `ESC` | Escape prefix for multi-byte command sequences. |

---

## 3. ImageWriter Escape Sequences

### 3.1 Line Spacing & Paper Movement

| Sequence | Description | Notes |
| :--- | :--- | :--- |
| `ESC A` | Set line spacing to 1/6 inch (6 LPI) | Default text line spacing. |
| `ESC B` | Set line spacing to 1/8 inch (8 LPI) | Compact text spacing. |
| `ESC T nn` | **Set line spacing to nn/144 inch** | `nn` is a 2-digit ASCII decimal string (`01` to `99`). Crucial for dot-matrix graphics passes! |
| `ESC f` | Forward half-line feed (1/12 inch) | Used for subscripts. |
| `ESC r` | Reverse line feed (1/6 inch) | Paper moves backward. |

### 3.2 Dot-Matrix Graphics Modes

ImageWriter uses vertical 8-bit columns where **Bit 7 is the top dot** and **Bit 0 is the bottom dot**:

```
Bit 7 (MSB)  o  Pin 1 (Top)
Bit 6        o  Pin 2
Bit 5        o  Pin 3
Bit 4        o  Pin 4
Bit 3        o  Pin 5
Bit 2        o  Pin 6
Bit 1        o  Pin 7
Bit 0 (LSB)  o  Pin 8 (Bottom)
```

| Sequence | Mode | Horizontal DPI | Format of Length |
| :--- | :--- | :--- | :--- |
| `ESC G d1 d2 d3 d4 <data>` | **Standard Graphics** | 72 DPI | `d1..d4` is a 4-digit ASCII decimal number specifying the number of graphic columns. E.g., `ESC G 0682` = 682 columns. |
| `ESC P d1 d2 d3 d4 <data>` | **High-Density Graphics** | 144 DPI | `d1..d4` is 4-digit ASCII decimal length. |
| `ESC S d1 d2 d3 d4 <data>` | Alternate 144 DPI mode | 144 DPI | 4-digit ASCII decimal length. |

### 3.3 ImageWriter II Color Ribbon Control

The ImageWriter II supports a 4-color ribbon (Black, Yellow, Magenta/Red, Cyan/Blue) which mechanically shifts up and down.

Command syntax: `ESC K <color_code>`

| Code | Color | Hex Code | RGB Equivalent |
| :--- | :--- | :--- | :--- |
| `0` | Black | `ESC K 0` | `#000000` |
| `1` | Yellow | `ESC K 1` | `#FFDF00` |
| `2` | Red / Magenta | `ESC K 2` | `#E31B23` |
| `3` | Blue / Cyan | `ESC K 3` | `#0080FF` |
| `4` | Orange | `ESC K 4` | `#FF8000` (Yellow + Red pass) |
| `5` | Green | `ESC K 5` | `#00B050` (Yellow + Blue pass) |
| `6` | Purple | `ESC K 6` | `#7030A0` (Red + Blue pass) |

*Note: In multicolor printing (such as in Print Shop IIGS), the printer performs multiple passes on the same line, changing the ribbon color between passes without advancing the paper, or using line feed followed by reverse feed.*

---

## 4. The Print Shop Printing Workflow

When Broderbund's *The Print Shop* prints to an ImageWriter:
1. Emits reset and line feed setting: `ESC T 24` (sets line pitch to 24/144" = 1/6" or 12/144" = 1/12").
2. For each graphic raster stripe across the page:
   - Sets graphic mode: `ESC G 0682` (or `ESC P 1364`).
   - Streams 682 raw 8-pin column bytes.
   - Sends carriage return (`\r`) and line feed (`\n`).
3. Upon completing all slices of the page:
   - Emits Form Feed (`0x0C`).
