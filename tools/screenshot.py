"""Grabs the badge's current frame over USB serial and saves it as a PNG.

Usage: python3 tools/screenshot.py out.png [port]   (finds the badge by itself)
"""
import sys
import serial
from PIL import Image

VW = VH = 90
SCALE = 4
def find_port():
    from serial.tools import list_ports
    for p in list_ports.comports():
        if p.vid == 0x303A:  # Espressif's USB serial
            return p.device
    raise SystemExit("badge not found on USB")


port = sys.argv[2] if len(sys.argv) > 2 else find_port()
s = serial.Serial(port, 115200, timeout=3)
s.reset_input_buffer()
s.write(b"S")
while s.readline().strip() != b"SHOT":
    pass
data = s.readline().strip().decode()
img = Image.new("RGB", (VW * SCALE, VH * SCALE))
for i in range(VW * VH):
    v = int(data[i * 4:i * 4 + 4], 16)
    v = ((v & 0xFF) << 8) | (v >> 8)  # pixels are stored byte-swapped
    r, g, b = (v >> 11) << 3, ((v >> 5) & 0x3F) << 2, (v & 0x1F) << 3
    x, y = i % VW, i // VW
    img.paste((r, g, b), (x * SCALE, y * SCALE, x * SCALE + SCALE, y * SCALE + SCALE))
img.save(sys.argv[1])
