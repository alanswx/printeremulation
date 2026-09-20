# Epson ESC/P (9-Pin & 24-Pin) Reference Summary

## 1. Overview & Hardware Interfaces

Epson ESC/P (Epson Standard Code for Printers) is the industry-standard dot-matrix printer language created by Epson for the MX-80, FX-80, and LQ series. In retro computers (Apple II, Commodore 64, IBM PC, Atari 8-bit), Epson printers were predominantly connected via:
* **Centronics Parallel Interface** (36-pin or 25-pin D-sub):
  - Data lines `D0-D7`.
  - `/STROBE` (Input, active low pulse when data is valid).
  - `BUSY` (Output, active high when receiving/processing byte or paper out).
  - `/ACK` (Output, active low pulse when byte accepted).
* **Serial RS-232 / Current Loop Interface**:
  - 8N1 or 7E1, 300 to 9600 baud, DTR hardware flow control.

---

## 2. Core Control Codes

| Code (Hex) | ASCII | Function |
| :--- | :--- | :--- |
| `0x08` | `BS` | Backspace 1 column. |
| `0x09` | `HT` | Horizontal tab. |
| `0x0A` | `LF` | Line Feed (advances paper by current line spacing). |
| `0x0C` | `FF` | **Form Feed** (ejects page to top of next page). |
| `0x0D` | `CR` | Carriage Return (moves head to left margin). |
| `0x0E` | `SO` | Select Double-Width printing for current line. |
| `0x0F` | `SI` | Select Condensed printing mode. |
| `0x12` | `DC2` | Cancel Condensed printing. |
| `0x14` | `DC4` | Cancel Double-Width printing. |
| `0x18` | `CAN` | Cancel print line buffer. |
| `0x1B` | `ESC` | Escape prefix. |

---

## 3. Essential Escape Sequences

### 3.1 Printer Initialization
* `ESC @` (`1B 40`): **Initialize Printer**. Resets all settings, tabs, line spacing, and margins to default power-on values.

### 3.2 Line Spacing Commands
Graphics printing in software like The Print Shop requires exact vertical alignment without white horizontal gaps between passes:
* `ESC 2`: Set line spacing to **1/6 inch** (default text, 6 LPI).
* `ESC 0`: Set line spacing to **1/8 inch** (8 LPI).
* `ESC 1`: Set line spacing to **7/72 inch**.
* `ESC A n`: Set line spacing to **n/72 inch** ($0 \le n \le 85$).
* `ESC 3 n`: **Set line spacing to n/216 inch** (or n/180" on 24-pin models).
  - For standard 8-dot graphics: $n = 24$ sets spacing to $24/216" = 8/72" = 1/9"$, ensuring 8-dot passes touch edge-to-edge with zero seam!

### 3.3 Bit-Image Graphics Modes

All standard Epson 8-pin graphics follow the binary length format:
`ESC <mode_char> nL nH <data_bytes...>`
Where column count = $nL + (nH \times 256)$.

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

| Sequence | Density Mode | Horiz. DPI | Vert. DPI | Description |
| :--- | :--- | :--- | :--- | :--- |
| `ESC K nL nH` | **Single Density** | 60 DPI | 72 DPI | Standard 8-pin bit image. |
| `ESC L nL nH` | **Double Density** | 120 DPI | 72 DPI | Half-speed for dense graphics. |
| `ESC Y nL nH` | **High-Speed Double** | 120 DPI | 72 DPI | Skips adjacent horizontal dots. |
| `ESC Z nL nH` | **Quadruple Density** | 240 DPI | 72 DPI | Very high resolution plotting. |
| `ESC * m nL nH` | **Generalized Bit Image** | Variable | 72 / 180 | $m=0$: 60 DPI 8-pin; $m=1$: 120 DPI 8-pin; $m=32$: 60 DPI 24-pin; $m=33$: 120 DPI 24-pin; $m=39$: 180 DPI 24-pin. |

---

## 4. The Print Shop with Grappler+ / Parallel Interface

1. On the Apple IIe, The Print Shop sends data to Slot 1 (where the Grappler+ parallel card is installed).
2. The card asserts STROBE to the printer and polls BUSY.
3. Print Shop initializes: `ESC @`, then `ESC 3 24` ($24/216"$ line spacing).
4. Each row prints:
   - `ESC K <nL> <nH> <bytes...>`
   - `\r \n`
5. At the end of the sign or banner: `\x0C` (Form Feed).
