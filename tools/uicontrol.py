#!/usr/bin/env python3
"""Управление M5 Stack Radio по COM-порту и снимок экрана.

Примеры:
    python3 tools/uicontrol.py shot player.png
    python3 tools/uicontrol.py send BH      # удержание кнопки B (меню)
    python3 tools/uicontrol.py send SHOT > /dev/null
    python3 tools/uicontrol.py keys A B CH
"""
import sys
import time
import serial
from PIL import Image

PORT = "/dev/ttyUSB0"
BAUD = 115200
W, H = 320, 240


def open_port():
    p = serial.Serial(PORT, BAUD, timeout=0.2)
    p.setDTR(False)
    p.setRTS(False)
    return p


def read_until(p, prefix, timeout=6.0):
    deadline = time.time() + timeout
    line = b""
    while time.time() < deadline:
        line += p.read(1)
        if line.endswith(b"\n"):
            s = line.decode("utf-8", "replace").strip()
            if s.startswith(prefix):
                return s
            line = b""
    raise TimeoutError(f"no line starting with {prefix!r}")


def shot(out_path):
    p = open_port()
    time.sleep(0.3)
    p.reset_input_buffer()
    p.write(b"SHOT\n")
    header = read_until(p, "SHOT ", timeout=10)
    parts = header.split()
    w, h = int(parts[1]), int(parts[2])
    bpp = int(parts[3]) if len(parts) > 3 else 8
    nbytes = w * h if bpp == 8 else (w * h + 7) // 8
    data = b""
    while len(data) < nbytes:
        chunk = p.read(nbytes - len(data))
        if not chunk:
            raise TimeoutError("screenshot timeout")
        data += chunk
    # хвост "\nEND\n"
    tail = p.read(8)
    if b"END" not in tail:
        p.read_until(p, "END", 2)
    p.close()

    img = Image.new("RGB", (w, h))
    px = img.load()
    if bpp == 8:
        for i, v in enumerate(data):
            x, y = i % w, i // w
            r = ((v >> 5) & 0x07) * 255 // 7
            g = ((v >> 2) & 0x07) * 255 // 7
            b = (v & 0x03) * 255 // 3
            px[x, y] = (r, g, b)
    else:
        stride = (w + 7) // 8
        for y in range(h):
            for x in range(w):
                bit = (data[y * stride + (x >> 3)] >> (7 - (x & 7))) & 1
                px[x, y] = (255, 255, 255) if bit else (0, 0, 0)
    img.save(out_path)
    big = img.resize((w * 3, h * 3), Image.NEAREST)
    big.save(out_path.replace(".png", "_3x.png"))
    print(f"saved {out_path} ({w}x{h})")


def send(cmds, wait=0.35):
    p = open_port()
    time.sleep(0.3)
    for c in cmds:
        p.write((c + "\n").encode())
        time.sleep(wait)
        while p.in_waiting:
            print(p.readline().decode("utf-8", "replace").rstrip())
    p.close()


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    if sys.argv[1] == "shot":
        shot(sys.argv[2])
    elif sys.argv[1] == "send":
        send(sys.argv[2:])
    else:
        print(__doc__)
