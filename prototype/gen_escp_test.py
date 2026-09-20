#!/usr/bin/env python3
"""Generate sample Epson ESC/P graphics stream simulating The Print Shop"""

import sys

def main():
    out = bytearray()
    
    # ESC @: Initialize
    out += b'\x1b@'
    
    # ESC 3 24: Set line spacing to 24/216" (8/72") for seamless 8-dot graphics
    out += b'\x1b3\x18'
    
    # Print 5 bands of 8-pin graphics (ESC K nL nH <bytes>)
    width = 300
    nL = width & 0xFF
    nH = (width >> 8) & 0xFF
    
    for band in range(10):
        out += b'\x1bK' + bytes([nL, nH])
        for x in range(width):
            # Create a nice checkerboard / sine pattern
            val = 0
            if (x // 10) % 2 == (band % 2):
                val = 0xAA # alternate dots
            else:
                val = 0x55
            # Top/bottom border
            if band == 0: val |= 0x80
            if band == 9: val |= 0x01
            out.append(val)
        out += b'\r\n'
        
    # Form Feed to finish page
    out += b'\x0c'
    
    with open("test_escp.prn", "wb") as f:
        f.write(out)
    print("Wrote test_escp.prn ({} bytes)".format(len(out)))

if __name__ == "__main__":
    main()
