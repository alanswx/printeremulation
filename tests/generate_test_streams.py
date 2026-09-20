#!/usr/bin/env python3
"""Generate sample print streams for ImageWriter, Coleco Adam, Commodore MPS 803, and Epson ESC/P."""

import os

def gen_imagewriter_color():
    # ImageWriter II 4-color test:
    # ESC K c: color select (0=Black, 1=Yellow, 2=Red/Magenta, 3=Blue/Cyan, 4=Orange, 5=Green, 6=Purple)
    # ESC G n1 n2 n3 n4: 72 DPI graphics
    # ESC T 1 6: 16/144" (8/72") line spacing
    out = bytearray()
    out += b"\x1bN" # Normal 10 cpi
    out += b"*** Apple ImageWriter II Color Test ***\r\n"
    
    colors = [
        (0, "Black Pass: "),
        (1, "Yellow Pass: "),
        (2, "Red/Magenta Pass: "),
        (3, "Blue/Cyan Pass: "),
        (4, "Orange Pass: "),
        (5, "Green Pass: "),
        (6, "Purple Pass: ")
    ]
    
    for color_idx, label in colors:
        out += f"\x1bK{color_idx}".encode('ascii')
        out += label.encode('ascii')
        # Add a block of graphics dots in this color
        # 100 columns: ESC G 0 1 0 0
        width = 100
        out += b"\x1bG0100"
        for x in range(width):
            out.append(0xFF if (x % 4 != 0) else 0x81)
        out += b"\r\n"
        
    out += b"\x0c" # Form Feed
    return out

def gen_adam_smartwriter():
    # Coleco Adam SmartWriter daisy wheel:
    # ASCII text with CR, LF, FF
    out = bytearray()
    out += b"========================================\r\n"
    out += b"   COLECO ADAM SMARTWRITER TEST PAGE    \r\n"
    out += b"========================================\r\n\r\n"
    out += b"The quick brown fox jumps over the lazy dog.\r\n"
    out += b"0123456789 !\"#$%&'()*+,-./:;<=>?@\r\n"
    out += b"Testing bidirectional line printing simulation...\r\n"
    out += b"\x0c" # FF
    return out

def gen_mps803():
    # Commodore MPS 803:
    # Text + ESC / 7-dot graphics
    out = bytearray()
    out += b"*** COMMODORE MPS 803 DOT MATRIX TEST ***\r\n"
    out += b"READY.\r\n"
    out += b"10 PRINT \"HELLO COMMODORE 64!\"\r\n"
    out += b"20 GOTO 10\r\n"
    # 7-dot graphics mode: CHR$(8)
    out += b"\x08"
    for x in range(150):
        out.append(0x7F if (x % 4 != 0) else 0x41)
    out += b"\x0f\r\n"
    out += b"COMMODORE GRAPHICS PASS COMPLETE.\r\n"
    out += b"\x0c"
    return out

def gen_escp_tps():
    # Epson ESC/P 8-pin graphics (The Print Shop TPS mode):
    out = bytearray()
    out += b'\x1b@'       # ESC @: Initialize
    out += b'\x1b3\x18'   # ESC 3 24: 24/216" line spacing
    width = 300
    nL = width & 0xFF
    nH = (width >> 8) & 0xFF
    for band in range(10):
        out += b'\x1bK' + bytes([nL, nH])
        for x in range(width):
            val = 0xAA if ((x // 10) % 2 == (band % 2)) else 0x55
            if band == 0: val |= 0x80
            if band == 9: val |= 0x01
            out.append(val)
        out += b'\r\n'
    out += b'\x0c'
    return out

def main():
    os.makedirs("tests/data", exist_ok=True)
    
    iw_data = gen_imagewriter_color()
    with open("tests/data/test_imagewriter_color.prn", "wb") as f:
        f.write(iw_data)
        
    adam_data = gen_adam_smartwriter()
    with open("tests/data/test_adam.prn", "wb") as f:
        f.write(adam_data)
        
    mps_data = gen_mps803()
    with open("tests/data/test_mps803.prn", "wb") as f:
        f.write(mps_data)

    escp_data = gen_escp_tps()
    with open("tests/data/test_escp.prn", "wb") as f:
        f.write(escp_data)
        
    print("Generated test streams in tests/data/")

if __name__ == "__main__":
    main()
