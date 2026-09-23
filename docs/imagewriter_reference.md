# Apple ImageWriter I & II Technical Reference Summary

## 1. Serial Protocol & Hardware Interface

* **Baud Rates**: 
  - Standard: 9600 baud (Switch SW1-1, SW1-2).
  - High-Speed / Apple IIgs: 19200 baud.
* **Format**: 8 Data bits, 1 Stop bit, No parity (8N1).
* **Handshake / Flow Control**:
  - Hardware Flow Control: DTR (Pin 4 on DB-25, Pin 1 on Mini-DIN 8) drops low when the internal print buffer is nearly full (typically when < 256 bytes remain in a 2KB buffer).
  - Hardware Handshake lines `CTS`/`RTS` must be asserted for proper serial flow control.
  - Software Flow Control: XON (`0x11`) / XOFF (`0x13`) supported on some firmware versions.

---

## 2. Core Control Codes

| Code (Hex) | ASCII | Description |
| :--- | :--- | :--- |
| `0x08` | `BS` | Backspace 1 character width. |
| `0x09` | `HT` | Horizontal tab (default 8 character columns). Used as prefix on Apple Super Serial Card (`<Ctrl-I>`). |
| `0x0A` | `LF` | Line feed (advances paper by current line spacing and returns to left margin). |
| `0x0C` | `FF` | **Form Feed** (ejects paper to top of next form / page). Triggers page commit in daemon. |
| `0x0D` | `CR` | Carriage return (moves print head to left margin). |
| `0x0E` | `SO` | Double-width / Expanded text mode on. |
| `0x0F` | `SI` | Condensed text mode on (15 cpi). |
| `0x1B` | `ESC` | Escape prefix for multi-byte command sequences. |

---

## 3. ImageWriter Escape Sequences

### 3.1 Line Spacing & Paper Movement

| Sequence | Description | Notes |
| :--- | :--- | :--- |
| `ESC A` | Set line spacing to 1/6 inch (6 LPI) | Default text line spacing (24/144"). |
| `ESC B` | Set line spacing to 1/8 inch (8 LPI) | Compact text spacing (18/144"). |
| `ESC T nn` | **Set line spacing to nn/144 inch** | `nn` is a 2-digit ASCII decimal string (`01` to `99`). Crucial for dot-matrix graphics passes! |
| `ESC f` | Forward half-line feed (1/12 inch) | Advances paper 12/144". Used for subscripts/half-lines. |
| `ESC r` | Reverse line feed (1/6 inch) | Paper moves backward 24/144". Used for multi-pass color printing. |
| `ESC F d1 d2 d3 d4` | **Absolute horizontal position** | Sets head position to dot column `d1..d4` (4 decimal ASCII digits). |

### 3.2 Dot-Matrix Graphics Modes

ImageWriter uses vertical 8-bit columns where **Bit 0 is the top dot (Pin 1)** and **Bit 7 is the bottom dot (Pin 8)**:

```
Bit 0 (LSB)  o  Pin 1 (Top dot)
Bit 1        o  Pin 2
Bit 2        o  Pin 3
Bit 3        o  Pin 4
Bit 4        o  Pin 5
Bit 5        o  Pin 6
Bit 6        o  Pin 7
Bit 7 (MSB)  o  Pin 8 (Bottom dot)
```

| Sequence | Mode | Horizontal DPI | Format of Length | Notes |
| :--- | :--- | :--- | :--- | :--- |
| `ESC G d1 d2 d3 d4 <data>` | **Standard Graphics** | 72 DPI | 4-digit ASCII decimal length | Standard density (e.g. `ESC G 0682`). |
| `ESC P d1 d2 d3 d4 <data>` | **High-Density Graphics** | 144 DPI | 4-digit ASCII decimal length | Double density graphics. |
| `ESC S d1 d2 d3 d4 <data>` | **120 DPI Graphics** | 120 DPI | 4-digit ASCII decimal length | Often used in *The Print Shop* (`ESC S 0960` for 8-inch width). |
| `ESC g d1 d2 d3 d4 <data>` | 72 DPI alternate | 72 DPI | 4-digit ASCII decimal length | Matches `ESC G`. |

#### Integer Coordinate Mapping:
To avoid floating-point registration shift across multiple passes (e.g. when mapping 120 DPI to a 144 DPI canvas), each dot's horizontal bounds are calculated using exact integer ratios:
$$\text{col\_x} = \text{margin\_left} + \frac{(\text{start\_col} + \text{col}) \times \text{dpi}}{\text{unit}}$$
$$\text{col\_next} = \text{margin\_left} + \frac{(\text{start\_col} + \text{col} + 1) \times \text{dpi}}{\text{unit}}$$

---

### 3.3 ImageWriter II Color Ribbon Control

The ImageWriter II supports a 4-color ribbon (Black, Yellow, Magenta/Red, Cyan/Blue) which mechanically shifts up and down.

Command syntax: `ESC K <color_code>`

| Code | Color | Escape Sequence | RGB Value | Notes |
| :--- | :--- | :--- | :--- | :--- |
| `0` | Black | `ESC K 0` | `#000000` | Default text and outlines. |
| `1` | Yellow | `ESC K 1` | `#FFDF00` | Bright pure yellow. |
| `2` | Red / Magenta | `ESC K 2` | `#E31B23` | Process magenta/red. |
| `3` | Blue / Cyan | `ESC K 3` | `#0080FF` | Process cyan/blue. |
| `4` | Orange | `ESC K 4` | `#FF8000` | Blended yellow + red pass. |
| `5` | Green | `ESC K 5` | `#00B050` | Blended yellow + blue pass. |
| `6` | Purple | `ESC K 6` | `#7030A0` | Blended red + blue pass. |

*Note: In multicolor graphics (such as *The Print Shop* banners), the software issues multiple passes over the same physical line (using CR without LF or `ESC r`), changing the ribbon color before each pass to blend colors or draw distinct elements.*

---

### 3.4 Apple Super Serial Card (SSC) Command Sequences

When connected through an Apple Super Serial Card (or IIgs firmware mimicking SSC), the printer stream may contain embedded card configuration escapes prefixed by `0x09` (`HT` / `Ctrl-I`):

| Sequence | Meaning | Parser Behavior |
| :--- | :--- | :--- |
| `<Ctrl-I> Z` | Card reset / reinitialize | Swallowed (no output). |
| `<Ctrl-I> N` | Disable automatic line feed | Swallowed (no output). |
| `<Ctrl-I> 80N` | Set 80-column line width without LF | Swallowed (no output). |
| `<Ctrl-I> 0N` | Disable line length limit | Swallowed (no output). |
| `<Ctrl-I> <other>` | Standard tab stop | Advance to next 8-character tab stop. |

---

## 4. The Print Shop Printing Workflow

When Broderbund's *The Print Shop* prints to an ImageWriter II:
1. Emits reset and line feed setting: `ESC T 24` (sets line pitch to 24/144" = 1/6" or 12/144" = 1/12").
2. Emits SSC commands: `<Ctrl-I> Z`, `<Ctrl-I> 80N`.
3. For each graphic raster stripe across the page:
   - Sets ribbon color: `ESC K 1` (Yellow), `ESC K 2` (Red), `ESC K 3` (Blue), or `ESC K 0` (Black).
   - Sets absolute horizontal position: `ESC F nnnn`.
   - Sets graphic mode and streams column bytes: `ESC S 0960 <data>`.
   - Sends carriage return (`\r`) to return head without paper feed, or line feed (`\n`) when all ribbon passes are complete.
4. On page end, sends `0x0C` (`FF`) or pauses for the next banner segment.
