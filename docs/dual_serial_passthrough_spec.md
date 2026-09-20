# Architectural Specification: Dual Serial (Port A & B) Passthrough for MiSTer FPGA

**Author**: MiSTer Retro Printer & Serial Emulation Project  
**Date**: September 2026  
**Status**: Proposal & Evaluation Specification  
**Target Subsystems**: `sys` (MiSTer FPGA framework), `Main_MiSTer` (HPS executable), Linux environment  

---

## 1. Executive Summary & Problem Context

### 1.1 The Dual-Serial Reality of Retro Computers
Many flagship computer architectures of the 1980s and 1990s featured **two independent serial controllers or dual-channel SCCs**:
* **Macintosh (Plus, SE, II, LC, Quadra)**: Zilog Z8530 SCC with **Channel A (Modem)** and **Channel B (Printer)**.
* **Apple IIgs**: Zilog Z8530 SCC with **Channel A (Slot 2 / Modem)** and **Channel B (Slot 1 / Printer)**.
* **Apple IIe**: Expansion slots accommodating simultaneous cards (e.g. **Slot 1 Super Serial Card** for ImageWriter, **Slot 2 Super Serial Card** for Hayes Modem / Wi-Fi).
* **IBM PC (ao486)**: Dual asynchronous communications adapters (**COM1** `$3F8` and **COM2** `$2F8`).
* **Atari ST**: 6850 ACIA for **MIDI** and 68901 MFP for **RS-232 Serial**.

In original hardware, users routinely used both ports simultaneously—for instance, running a **terminal/dial-up BBS or PPP connection on Port A** while sending a document to an **ImageWriter or LaserWriter on Port B**, or running **MIDI instruments alongside an active serial link**.

### 1.2 Current MiSTer Single-UART Limitation
In the current MiSTer framework:
1. **Single FPGA-to-HPS UART Connection**: The `sys` framework instantiates a single hard HPS UART bridge (`cyclonev_hps_interface_peripheral_uart uart` in `sys/sys_top.v`).
2. **Single Hardware Node on Linux**: This hard peripheral is mapped exclusively to `/dev/ttyS1` in the embedded Linux kernel.
3. **Single OSD UART Mode**: `Main_MiSTer` (`menu.cpp` / `user_io.cpp`) provides a single global `uart_mode` selector:
   - `0`: Off
   - `1`: PPP (`pppd /dev/ttyS1`)
   - `2`: Terminal Console (`agetty /dev/ttyS1`)
   - `3..5`: MidiLink (MT-32 / fluidsynth / SoundFont via `/dev/ttyS1`)
   - `7`: Printer (`mister_printerd /dev/ttyS1`)
4. **Mutual Exclusion**: Because only one serial stream reaches the HPS, **running a Modem and a Printer simultaneously is currently impossible**. Cores are forced to either hardcode Channel A to `UART_*` (abandoning Channel B), hardcode Channel B (abandoning Channel A), or implement an OSD mux that toggles between them.

### 1.3 The Underlying Cyclone V Silicon Constraint
The Intel/Altera Cyclone V SoC on the DE10-Nano contains two hard 16550-compatible UART IP cores in silicon:
* **UART0** (`0xFFC02000`, IRQ 48): Pinmuxed in hardware to **dedicated HPS I/O pins 17 & 18**, which connect directly to the on-board FTDI FT232RQ USB-to-UART bridge (the Micro-USB debug console).
* **UART1** (`0xFFC03000`, IRQ 49): Configured to route its pins through the **FPGA fabric interface** (`HPSINTERFACEPERIPHERALUART_X52_Y67_N111`).

> [!IMPORTANT]
> **Silicon Pinmux Rule**: The Cyclone V hardware architecture permits each HPS UART to route EITHER to HPS dedicated I/O pins OR to the FPGA fabric, but **not both**. Diverting UART0 into the FPGA fabric would disable the essential Micro-USB Linux debug console. Furthermore, the Cyclone V HPS macro only exposes one peripheral UART interface block to the FPGA fabric. Therefore, **a second hardware HPS UART cannot simply be instantiated in the FPGA fabric**. Any second serial channel must use an alternate data transport.

---

## 2. Architectural Comparison Matrix

Four primary architectural approaches exist for passing both Serial A and Serial B to the HPS. The following matrix summarizes their characteristics:

| Criterion | Approach 1: Hybrid HW UART + SPI `user_io` PTY | Approach 2: Packet Mux over `/dev/ttyS1` | Approach 3: Soft 16550 on `LWHPS2FPGA` AXI | Approach 4: OSD Dynamic Mux (Time-Shared) |
| :--- | :--- | :--- | :--- | :--- |
| **True Concurrency?** | **Yes** (Simultaneous A & B) | **Yes** (Simultaneous A & B) | **Yes** (Simultaneous A & B) | **No** (A or B selectable) |
| **Serial A Transport** | Hard HPS UART (`/dev/ttyS1`) | Multiplexed `/dev/ttyS1` | Hard HPS UART (`/dev/ttyS1`) | Hard HPS UART (`/dev/ttyS1`) |
| **Serial B Transport** | SPI `user_io` FIFO $\rightarrow$ Linux PTY | Multiplexed `/dev/ttyS1` | Memory-mapped AXI Bus | None (Muxed to `/dev/ttyS1`) |
| **Linux Device Nodes** | `/dev/ttyS1` + `/tmp/ttySerialB` | `/tmp/ttySerialA` + `/tmp/ttySerialB` | `/dev/ttyS1` + `/dev/ttyS2` | `/dev/ttyS1` |
| **Changes to `sys`** | **Low-Medium** (`emu_ports.vh`, `hps_io.sv`) | **Medium-High** (Framer in `sys_top.v`) | **Extreme** (Qsys `sysmem.sv` overhaul) | **Zero** |
| **Changes to `Main_MiSTer`** | **Medium** (PTY pump in `user_io`, OSD) | **High** (Packet framer daemon/thread) | **Low** (Kernel handles it) | **Low** (OSD mux option) |
| **Kernel / DTB Changes** | **Zero** (Standard user-space POSIX) | **Zero** | **High** (Device Tree + DTBO driver) | **Zero** |
| **Latency** | A: < 1 µs, B: ~1–16 ms | Both: ~1–2 ms packet delay | Both: < 5 µs | A or B: < 1 µs |
| **Max Bandwidth** | A: 1.5 Mbps, B: ~230 Kbps | Aggregate: 1.5 Mbps shared | Both: 1.5 Mbps | 1.5 Mbps |
| **Backward Compatibility**| **100%** (unmodified cores unaffected)| **Breaking** (requires mux daemon) | **100%** | **100%** |
| **Overall Difficulty** | **Moderate (Recommended)** | **High / Fragile** | **Very High / High Risk** | **Trivial (Fallback)** |

---

## 3. Approach 1 (Recommended): Hybrid Hardware UART + SPI `user_io` PTY

### 3.1 Concept Overview
* **Serial A (Modem / High-Speed / Real-Time)**: Continues using the native hardware `cyclonev_hps_interface_peripheral_uart` wired to `/dev/ttyS1`. It retains zero-latency, full-speed baud rates up to 1.5 Mbps (ideal for PPP, AppleTalk, SLIP, and MIDI).
* **Serial B (Printer / Aux)**: Connects to a dedicated dual-clock BRAM FIFO in `hps_io.sv`. The HPS periodically drains and fills this FIFO over the existing SPI bus (`user_io`) and exposes it to Linux user-space through a **standard POSIX Pseudoterminal (PTY)** at `/tmp/ttySerialB` (symlinked as `/dev/ttyS2`).
* **Daemon Agnostic**: Any Linux program (`mister_printerd`, `pppd`, `midilink`, `agetty`, `minicom`) opens `/tmp/ttySerialB` using standard `open()`, `read()`, `write()`, and `tcsetattr()`. To the software, it is indistinguishable from a real physical UART.

```
+-------------------------------------------------------------------------------+
|                                  FPGA FABRIC                                  |
|                                                                               |
|  +---------------------+                                                      |
|  | Guest Core (e.g.    |                                                      |
|  | Mac Quadra / IIgs)  |                                                      |
|  |                     |                                                      |
|  |  SCC Channel A ---->|===> UART_* ===================> [HPS Hard UART IP]  |
|  |  (Modem Port)       |                                         ||           |
|  |                     |                                         ||           |
|  |  SCC Channel B ---->|===> UART2_* ===> [hps_io.sv]            ||           |
|  |  (Printer Port)     |                  +-------------+        ||           |
|  +---------------------+                  | TX/RX FIFOs |        ||           |
|                                           +-------------+        ||           |
|                                                  ||              ||           |
|                                                  || (SPI Bus)    ||           |
+--------------------------------------------------||--------------||-----------+
                                                   ||              ||
+--------------------------------------------------||--------------||-----------+
|                                    ARM HPS       \/              \/           |
|  Linux Kernel / Drivers:                   [SPI Controller]  [/dev/ttyS1]     |
|                                                  ||               ||          |
|  Main_MiSTer:                                    ||               ||          |
|    • user_io_poll() ........................ Read/Write           ||          |
|    • PTY Manager (openpty) =====================> [/tmp/ttySerialB] ||          |
|                                                           ||      ||          |
|  User-Space Daemons:                                      \/      \/          |
|    • mister_printerd (ImageWriter / PDF) ................ ||      ||          |
|    • pppd / midilink (Internet / MIDI) ..................         ||          |
+-------------------------------------------------------------------------------+
```

---

### 3.2 Specific Changes Required in `sys` (FPGA Framework)

#### 1. Port Interface Expansion (`sys/emu_ports.vh` & `sys/sys_top.v`)
Add the secondary UART port signals to the top-level core boundary:
```verilog
// --- Primary Serial (Port A / Modem) -> Hard HPS UART ---
input         UART_CTS,
output        UART_RTS,
input         UART_RXD,
output        UART_TXD,
output        UART_DTR,
input         UART_DSR,

// --- Secondary Serial (Port B / Printer) -> SPI user_io ---
input         UART2_CTS,
output        UART2_RTS,
input         UART2_RXD,
output        UART2_TXD,
output        UART2_DTR,
input         UART2_DSR,
```

#### 2. FIFO & Transceiver Implementation in `sys/hps_io.sv`
Inside `hps_io.sv`:
1. **FIFO Buffers**:
   - `uart2_tx_fifo`: 512-byte dual-clock BRAM (stores bytes transmitted by the FPGA guest core destined for HPS).
   - `uart2_rx_fifo`: 256-byte dual-clock BRAM (stores bytes received from HPS destined for the guest core).
2. **UART Transceiver / Bit Deserializer**:
   - Accepts raw `UART2_TXD` / `UART2_RXD` serial bitstreams clocked by an internal baud generator driven by `uart2_speed` (configured by HPS).
   - *Alternative optimization*: If the core exposes a parallel byte interface (strobe + 8-bit data), bypass the serializer to save ~50 ALMs.
3. **SPI Command Handlers**:
   Define new SPI opcodes in the `hps_io` command decoder:
   - `'h3C` (`UIO_SERIAL2_GET`): Reads up to 64 bytes from `uart2_tx_fifo` into HPS.
   - `'h3D` (`UIO_SERIAL2_SEND`): Writes incoming bytes from HPS into `uart2_rx_fifo`.
   - `'h3E` (`UIO_SERIAL2_STATUS`):
     - `io_dout[7:0]` = `{uart2_rx_empty, uart2_rx_full, uart2_tx_empty, uart2_tx_full, UART2_CTS, UART2_DSR, 2'b00}`.
     - Sets `UART2_RTS <= io_din[0]`, `UART2_DTR <= io_din[1]`.
   - `'h45` (`UIO_SET_UART2`): Sets `uart2_mode` (8-bit) and `uart2_speed` (32-bit baud rate).

---

### 3.3 Specific Changes Required in `Main_MiSTer`

#### 1. Header Definitions (`user_io.h`)
```c
#define UIO_SERIAL2_GET     0x3C  // Read pending bytes from FPGA Serial 2 TX FIFO
#define UIO_SERIAL2_SEND    0x3D  // Send bytes to FPGA Serial 2 RX FIFO
#define UIO_SERIAL2_STATUS  0x3E  // Read/write flow control and FIFO occupancy
#define UIO_SET_UART2       0x45  // Set baud rate and mode for Serial 2
```

#### 2. Pseudoterminal (PTY) Manager (`user_io.cpp`)
Implement a lightweight PTY controller that bridges Linux user-space to the SPI FIFO:
```c
static int pty_master_fd = -1;
static int pty_slave_fd  = -1;
static char pty_slave_name[64];

void user_io_serial2_init()
{
    if (openpty(&pty_master_fd, &pty_slave_fd, pty_slave_name, NULL, NULL) < 0) {
        printf("UART2: Failed to allocate PTY: %s\n", strerror(errno));
        return;
    }

    // Set non-blocking on master
    int flags = fcntl(pty_master_fd, F_GETFL, 0);
    fcntl(pty_master_fd, F_SETFL, flags | O_NONBLOCK);

    // Create stable symlinks for daemons
    unlink("/tmp/ttySerialB");
    symlink(pty_slave_name, "/tmp/ttySerialB");
    unlink("/dev/ttyS2");
    symlink(pty_slave_name, "/dev/ttyS2");

    printf("UART2: Virtual Serial B active on %s (symlinked /tmp/ttySerialB)\n", pty_slave_name);
}
```

#### 3. Polling Pump in `user_io_poll()` (`user_io.cpp`)
Inside `user_io_poll()` (which executes at 60 Hz frame intervals or on main loop ticks):
```c
void user_io_serial2_poll()
{
    if (pty_master_fd < 0) return;

    // 1. Drain bytes from FPGA TX FIFO -> Write to PTY Master
    uint8_t rx_buf[128];
    spi_uio_cmd_cont(UIO_SERIAL2_GET);
    int count = spi_b(); // First byte: pending count
    if (count > 0 && count <= sizeof(rx_buf)) {
        for (int i = 0; i < count; i++) rx_buf[i] = spi_b();
        DisableIO();
        write(pty_master_fd, rx_buf, count);
    } else {
        DisableIO();
    }

    // 2. Read bytes from PTY Master -> Send to FPGA RX FIFO
    uint8_t tx_buf[64];
    int n = read(pty_master_fd, tx_buf, sizeof(tx_buf));
    if (n > 0) {
        spi_uio_cmd_cont(UIO_SERIAL2_SEND);
        spi_w(n);
        for (int i = 0; i < n; i++) spi_w(tx_buf[i]);
        DisableIO();
    }
}
```

#### 4. OSD UI Decoupling (`menu.cpp`)
Refactor the single UART menu into two independent sub-menus:
```
+------------------------------------+
|             UART Settings          |
+------------------------------------+
|  Port 1 (Modem)   : [PPP Link    ] |  -> Maps to /dev/ttyS1
|  Port 1 Baud Rate : [115200      ] |
|  Port 2 (Printer) : [ImageWriter ] |  -> Maps to /tmp/ttySerialB
|  Port 2 Baud Rate : [19200       ] |
|  Reset Connections                 |
+------------------------------------+
```

#### 5. Daemon Launch Script Refactor (`/sbin/uartmode`)
Update `/sbin/uartmode` to handle independent port assignments:
```bash
# uartmode <port_num> <mode_id> <baud>
# Port 1 uses /dev/ttyS1; Port 2 uses /tmp/ttySerialB
PORT=$1
MODE=$2
BAUD=$3

TTY_DEV="/dev/ttyS1"
[ "$PORT" == "2" ] && TTY_DEV="/tmp/ttySerialB"

if [ "$MODE" == "7" ]; then
    /media/fat/mister_printerd -d "$TTY_DEV" -b "$BAUD" -m imagewriter -o /media/fat/printers &
elif [ "$MODE" == "1" ]; then
    pppd "$BAUD" "$TTY_DEV" ... &
fi
```

---

### 3.4 Specific Changes Required in Cores (e.g. Mac Quadra / LC / IIgs)

In the core top-level file (e.g. `MacIIvi.sv` or `MacQuadra800.sv`):
```verilog
// Wire Z8530 Channel A to Primary HPS UART (Port 1)
assign UART_TXD = scc_txd_a;
assign scc_rxd_a = UART_RXD;
assign UART_RTS = scc_rts_a;
assign scc_cts_a = UART_CTS;

// Wire Z8530 Channel B to Secondary HPS UART (Port 2 / Printer)
assign UART2_TXD = scc_txd_b;
assign scc_rxd_b = UART2_RXD;
assign UART2_RTS = scc_rts_b;
assign scc_cts_b = UART2_CTS;
```

---

## 4. Approach 2: High-Speed Packet Multiplexer over `/dev/ttyS1`

### 4.1 Concept Overview
Instead of introducing SPI commands, both Serial A and Serial B are packetized into a single high-speed bitstream (e.g. running `/dev/ttyS1` at 1.5 Mbps or 2.0 Mbps continuously).
* **FPGA Side**: A packet framing engine encapsulates incoming bytes into structured frames:
  `[0xAA (Preamble), 0x7E (SOF), CHANNEL_ID (1 byte), LEN (1 byte), PAYLOAD (N bytes), CRC8 (1 byte)]`.
* **HPS Side**: A dedicated background multiplexer daemon (`mister_uarthub`) holds `/dev/ttyS1` open, decodes frames, routes Channel A bytes to `/tmp/ttySerialA`, and Channel B bytes to `/tmp/ttySerialB`.

```
[Core Port A] ---\
                  +--> [FPGA Packet Mux] ===(1.5 Mbps UART)===> [/dev/ttyS1]
[Core Port B] ---/                                                    ||
                                                                      \/
                                                             [mister_uarthub]
                                                               //          \\
                                                               \/          \/
                                                       [/tmp/ttyA]    [/tmp/ttyB]
```

### 4.2 Evaluation of Approach 2
* **Pros**:
  - Completely bypasses the SPI bus (`user_io_poll`).
  - Symmetrical low latency for both ports (~1–2 ms).
* **Cons / Roadblocks**:
  - **High Complexity**: Requires implementing framing, deframing, timeout flushes, CRC checking, and packet recovery state machines in both Verilog RTL and C.
  - **Fragile Failure Modes**: A single corrupted framing byte or framing error on the physical UART stream can desynchronize the stream, leading to dropped printer slices or corrupted modem packets.
  - **Breaking Change**: Older cores or external tools expecting raw byte streaming over `/dev/ttyS1` will completely fail unless a complex negotiation protocol is added.
  - **Daemon Dependency**: If `mister_uarthub` crashes or hangs, all serial communication on the system terminates.

---

## 5. Approach 3: Soft 16550 UART on `LWHPS2FPGA` AXI Bridge

### 5.1 Concept Overview
Instantiate a standard 16550-compatible UART IP block in the FPGA fabric and connect it to the Cyclone V HPS Lightweight AXI-to-FPGA bridge (`h2f_lw_axi`).
* **HPS Side**: The Linux kernel already contains the standard `8250_of` driver. By updating the Device Tree (`.dtb` or `.dtbo`), Linux natively exposes `/dev/ttyS2` as a true hardware UART with interrupt servicing via `f2h_irq`.

### 5.2 Evaluation of Approach 3
* **Pros**:
  - Architecturally "purest" embedded Linux solution.
  - Zero daemon polling overhead; serviced by native kernel interrupts.
* **Cons / Roadblocks**:
  - **Extreme Blast Radius Across Repositories**: The `sys/sysmem.sv` module in MiSTer is generated by Intel Qsys/Platform Designer. Adding an AXI slave interface requires regenerating the Qsys system across every core in the MiSTer ecosystem.
  - **Kernel Device Tree Coupling**: Requires maintaining and deploying custom Device Tree Blobs (`socfpga.dtb`) across all MiSTer Linux distributions. If a user updates their Linux kernel via `update.sh`, a vanilla DTB would break the port.
  - **FPGA Resource Overhead**: A full 16550 IP core with register banks requires 400–600 ALMs.

---

## 6. Comprehensive Difficulty & Effort Evaluation

The following table breaks down the engineering tasks and estimated level of effort for **Approach 1 (Hybrid HW UART + SPI `user_io`)**:

| Subsystem | File / Component | Required Modifications | Risk / Blast Radius | Estimated Effort |
| :--- | :--- | :--- | :--- | :---: |
| **`sys` (Framework)** | `sys/emu_ports.vh` | Add `UART2_*` signal declarations (6 pins). | **Zero**: Cored that omit these signals synthesize without error or default to open. | **1 hour** |
| | `sys/sys_top.v` | Pass `UART2_*` from `emu` into `hps_io`. | **Very Low**: Simple signal pass-through. | **1 hour** |
| | `sys/hps_io.sv` | Instantiate 512B TX / 256B RX BRAM FIFO, baud generator, and handle SPI commands `'h3C`, `'h3D`, `'h3E`, `'h45`. | **Low**: Standard BRAM instantiation; uses existing SPI command decoding pipeline. | **1–2 days** |
| **`Main_MiSTer`** | `user_io.h` | Define `UIO_SERIAL2_*` opcodes and prototypes. | **Zero**: Simple header constants. | **30 mins** |
| | `user_io.cpp` | Implement PTY creation (`openpty`), symlinking (`/tmp/ttySerialB`), and polling routines in `user_io_poll()`. | **Low**: Standard POSIX `termios` code; non-blocking I/O. | **1–2 days** |
| | `menu.cpp` | Split single UART menu into Port 1 (Modem) and Port 2 (Printer) submenus with independent speed/mode variables. | **Low-Medium**: UI state machine adjustment in `menustate`. | **1 day** |
| **Linux Scripts** | `/sbin/uartmode` | Update shell script to accept port selector and target `/tmp/ttySerialB`. | **Low**: Straightforward shell scripting. | **2 hours** |
| **Core Integration** | `MacQuadra800.sv` / `Apple-IIgs.sv` | Wire Z8530 SCC Channel B pins to `UART2_*`. | **Very Low**: Clean 4-line wiring in top-level. | **2 hours per core** |
| **Testing & QA** | System Integration | Simultaneous live test: PPP dial-up on Port A + ImageWriter PDF print on Port B. | **Medium**: Verification of buffer underruns/overruns under load. | **1 day** |

### Total Project Estimate: **4 to 6 engineering days**

---

## 7. Recommendation & Phased Rollout Plan

### Phase 1: Near-Term Fallback (Zero Framework Changes)
* **Action**: In cores with dual serial ports (Macintosh, Apple IIgs), implement an **OSD Mux** in the core's `CONF_STR`:
  `"O[8],Serial Port,Modem (Port A)|Printer (Port B);"`
* **Result**: Allows users to immediately print to `mister_printerd` by switching the OSD to "Printer", without requiring modifications to `sys/` or `Main_MiSTer`.

### Phase 2: Upstream-Ready Dual Serial Infrastructure (Approach 1)
1. **Fork & Branch `sys`**: Implement `UART2_*` and the SPI BRAM FIFO in a test branch of the `sys` repository.
2. **Implement PTY Engine in `Main_MiSTer`**: Add `user_io_serial2_init()` and `user_io_serial2_poll()` to `user_io.cpp`.
3. **Core Validation on Mac Quadra 800**: Connect SCC Channel B to `UART2_*` and verify concurrent operation of `mister_printerd` on `/tmp/ttySerialB` alongside PPP or MidiLink on `/dev/ttyS1`.
4. **Submit Upstream PRs**: Submit clean, isolated PRs to `MiSTer-devel/sys` and `MiSTer-devel/Main_MiSTer`.
