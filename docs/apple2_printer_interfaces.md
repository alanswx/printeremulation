# Apple II & Apple IIgs Printer Interfaces

## 1. Apple Super Serial Card (SSC) - Serial Printing

### 1.1 Overview
The Apple Super Serial Card (SSC) was the standard serial interface for the Apple II/II+ and IIe, typically placed in:
* **Slot 1** when used for printers (ImageWriter I/II, serial dot-matrix).
* **Slot 2** when used for modems (Hayes Micromodem, standard RS-232 modems).

### 1.2 Hardware Architecture
* **UART Controller**: MOS Technology 6551 ACIA (Asynchronous Communications Interface Adapter).
* **Clock**: 1.8432 MHz crystal oscillator providing standard baud rates from 50 to 19,200 baud.
* **Firmware ROM**: 2 KB 2716 EPROM mapped at `$C100-$C1FF` (when Slot 1 selected) and `$C800-$CFFF` (shared slot-ROM space).

### 1.3 6551 ACIA Register Map (Slot 1 Base: `$C098` / Slot 2 Base: `$C0A8`)
Given slot $S$ ($S=1 \rightarrow \text{base } \$C090 + 8 = \$C098$):

| Address | R/W | Register | Function |
| :--- | :--- | :--- | :--- |
| `$C088 + (S \times 16)` | R/W | **Transmit / Receive Data** | Reading gets incoming RX byte; writing queues byte to TX shift register. |
| `$C089 + (S \times 16)` | R | **Status Register** | Bit 7: IRQ; Bit 4: Transmit Data Register Empty; Bit 3: Receive Data Register Full; Bit 1: Overrun; Bit 0: Parity error. |
| `$C08A + (S \times 16)` | R/W | **Command Register** | Controls DTR, parity enable, receiver interrupt control. |
| `$C08B + (S \times 16)` | R/W | **Control Register** | Stop bits (bit 7), word length (bits 6-5), baud rate generator divisor (bits 3-0). |

---

## 2. Grappler+ & Apple Parallel Interface - Parallel Printing

### 2.1 Overview
Parallel printing on the Apple II was overwhelmingly handled by Centronics-style interface cards installed in **Slot 1**:
* **Orange Micro Grappler+**: The most popular graphic printer interface for Apple II, supported directly by The Print Shop, PrintMaster, AppleWorks, and Broderbund titles.
* **Apple Parallel Interface Card**: Apple's official Centronics interface.

### 2.2 Grappler+ Register Architecture (Slot 1 Base: `$C090`)

| Address | R/W | Description |
| :--- | :--- | :--- |
| `$C090` | Write | **Printer Data Output**: Latches 8-bit byte onto Centronics `D0-D7` lines and fires `/STROBE` low pulse. |
| `$C091` / `$C092` | Read | **Printer Status**: Bit 7 indicates `BUSY` state ($1 = \text{Busy/not ready}, 0 = \text{Ready for next byte}$). |

### 2.3 Slot 1 ROM Firmware Entry Points
Apple II peripheral ROMs adhere to the Apple Pascal / ProDOS firmware protocol:
* `$C100`: Card initialization (`PR#1`).
* `$C105` or `$C107`: Output character routine. Software calls `JSR $C107` with the character byte in the accumulator `A`. The ROM waits for the printer `BUSY` bit to clear, writes `A` to `$C090`, and returns.

---

## 3. Apple IIgs Serial Ports (Zilog Z8530 SCC)

### 3.1 Overview
The Apple IIgs includes two high-speed serial ports driven by a Zilog 8530 SCC:
* **Port 1 (Printer Port)**: Mini-DIN 8, RS-422, default 9600 or 19200 baud.
* **Port 2 (Modem Port)**: Mini-DIN 8, RS-422, default 2400 to 19200 baud.

### 3.2 Slot Mapping & Control Panel
In the Apple IIgs Control Panel (Desk Accessory / ROM):
* Slot 1 can be configured as **"Printer Port"** (internal SCC Channel B) or **"Your Card"** (Slot 1 physical card).
* Slot 2 can be configured as **"Modem Port"** (internal SCC Channel A) or **"Your Card"**.
* GS/OS Print Manager and Apple IIgs native software (Print Shop IIGS, Paintworks Gold, GraphicWriter) communicate with the ImageWriter II directly over Port 1.

---

## 4. Hardware/RTL Strategies for MiSTer

### Strategy 1: Shared UART with Mode Switch (Zero FPGA Bridge Changes)
* The FPGA core routes the active serial device to `UART_*` pins on `sys_top`.
* When using Apple IIgs:
  - If MiSTer UART Mode = `Modem` $\rightarrow$ SCC Port 2 connects to HPS UART.
  - If MiSTer UART Mode = `Printer` $\rightarrow$ SCC Port 1 connects to HPS UART.
* When using Apple IIe:
  - Super Serial Card connects to HPS UART.

### Strategy 2: Dedicated Parallel / 2nd Serial FIFO (Enables Simultaneous Modem + Printer)
* Create an RTL module `virtual_centronics.v`:
  ```verilog
  module virtual_centronics (
      input         clk,
      input         reset,
      input         cs,
      input         we,
      input   [7:0] data_in,
      output        busy,
      // SPI user_io drain interface
      input         drain_req,
      output  [7:0] drain_data,
      output        fifo_empty
  );
  ```
* 6502 writes byte $\rightarrow$ FIFO latches byte $\rightarrow$ `busy` goes high momentarily $\rightarrow$ byte read out by ARM via SPI `user_io_poll()` $\rightarrow$ fed to printer daemon!
