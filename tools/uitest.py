#!/usr/bin/env python3
"""Сброс, прогон экранов по COM-порту и снимки (проверка вёрстки)."""
import time
import serial
from PIL import Image

PORT = "/dev/ttyUSB0"
BAUD = 115200
OUT = "/tmp/opencode"


def open_port():
    p = serial.Serial(PORT, BAUD, timeout=0.25)
    p.setDTR(False)
    p.setRTS(False)
    time.sleep(0.2)
    p.reset_input_buffer()
    return p


def reset(p):
    p.setDTR(False)
    p.setRTS(True)
    time.sleep(0.15)
    p.setRTS(False)
    time.sleep(0.2)


def send(p, cmd, wait=0.5, quiet=False):
    p.write((cmd + "\n").encode())
    time.sleep(wait)
    while p.in_waiting:
        line = p.readline().decode("utf-8", "replace").rstrip()
        if line and not quiet:
            print("  <-", line)


def shot(p, name):
    p.reset_input_buffer()
    p.write(b"SHOT\n")
    deadline = time.time() + 12
    line = b""
    header = None
    while time.time() < deadline:
        line += p.read(1)
        if line.endswith(b"\n"):
            s = line.decode("utf-8", "replace").strip()
            line = b""
            if s.startswith("SHOT "):
                header = s
                break
    if not header:
        print("  !! no SHOT header")
        return
    parts = header.split()
    w, h = int(parts[1]), int(parts[2])
    bpp = int(parts[3]) if len(parts) > 3 else 8
    nbytes = w * h if bpp == 8 else (w * h + 7) // 8
    data = b""
    while len(data) < nbytes:
        chunk = p.read(nbytes - len(data))
        if not chunk:
            print("  !! timeout")
            return
        data += chunk
    p.read(8)

    img = Image.new("RGB", (w, h))
    px = img.load()
    if bpp == 8:
        for i, v in enumerate(data):
            px[i % w, i // w] = (((v >> 5) & 7) * 255 // 7,
                                 ((v >> 2) & 7) * 255 // 7,
                                 (v & 3) * 255 // 3)
    else:
        stride = (w + 7) // 8
        for y in range(h):
            for x in range(w):
                bit = (data[y * stride + (x >> 3)] >> (7 - (x & 7))) & 1
                px[x, y] = (255, 255, 255) if bit else (0, 0, 0)
    img.save(f"{OUT}/{name}.png")
    img.resize((w * 3, h * 3), Image.NEAREST).save(f"{OUT}/{name}_3x.png")
    print(f"  shot {name}")


def main():
    p = open_port()
    print("reset...")
    reset(p)
    time.sleep(9)

    print("player"); shot(p, "u1_player")
    print("menu"); send(p, "BH"); shot(p, "u2_menu")
    print("stations"); send(p, "B"); shot(p, "u3_stations")
    send(p, "C"); shot(p, "u3b_stations_sel")
    send(p, "BH", wait=1.0)

    print("wifi scan")
    send(p, "BH"); send(p, "C"); send(p, "B", wait=0.4)
    time.sleep(0.4)
    shot(p, "u4a_scanning")
    time.sleep(1.0)
    shot(p, "u4_wifi")

    print("password")
    send(p, "B", wait=0.8)
    shot(p, "u5_password")
    send(p, "B", wait=0.2); send(p, "B", wait=0.2); send(p, "B", wait=0.4)
    shot(p, "u5b_password_typed")

    # вниз в ряд спецклавиш и на "<" (назад к списку)
    send(p, "CH", wait=0.2); send(p, "CH", wait=0.2); send(p, "CH", wait=0.2); send(p, "CH", wait=0.25)
    for _ in range(3):
        send(p, "C", wait=0.2)
    send(p, "B", wait=0.6)
    shot(p, "u6_wifi_again")

    print("player again")
    send(p, "BH", wait=2.0)
    shot(p, "u7_player_again")

    print("info")
    send(p, "BH"); send(p, "C"); send(p, "C"); send(p, "B")
    shot(p, "u8_info")
    send(p, "B"); send(p, "BH", wait=1.0)
    shot(p, "u9_final")

    p.close()


if __name__ == "__main__":
    main()
