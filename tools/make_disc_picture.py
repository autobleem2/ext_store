#!/usr/bin/env python3
"""Draws data/disc.png - the Store's picture for a game whose cover none of its sources knows: a plain compact
disc, seen from the data side (silver, a faint rainbow sheen, the clear ring and the hole), on transparency.

    python tools/make_disc_picture.py            (writes data/disc.png next to this folder)

Our own artwork, generated - rerun after changing it. Needs Pillow.
"""
import math
import os

from PIL import Image, ImageDraw, ImageFilter

SIZE = 256            # the file's size; the Store fits it into its boxes
SCALE = 4             # drawn larger, then scaled down: smooth edges
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'data', 'disc.png')


def disc():
    n = SIZE * SCALE
    c = n / 2
    outer = n * 0.47      # the disc
    data_in = n * 0.20    # where the data (the rainbow) begins
    clear = n * 0.155     # the clear plastic ring
    hole = n * 0.075      # the hole

    img = Image.new('RGBA', (n, n), (0, 0, 0, 0))
    px = img.load()
    for y in range(n):
        dy = y - c
        for x in range(n):
            dx = x - c
            r = math.hypot(dx, dy)
            if r > outer or r < hole:
                continue
            if r < clear:
                # the clear hub: grey, see-through
                px[x, y] = (205, 210, 215, 150)
                continue
            a = math.atan2(dy, dx)
            # silver, lighter along one diagonal (the light), with a faint rainbow round the data area
            light = 0.5 + 0.5 * math.cos(2 * (a - math.pi / 4))
            base = 168 + int(60 * light)
            if r >= data_in:
                t = (a / (2 * math.pi)) * 3.0 + r / outer
                sheen = 0.16 * light
                rr = base + int(255 * sheen * math.sin(2 * math.pi * t))
                gg = base + int(255 * sheen * math.sin(2 * math.pi * t + 2.1))
                bb = base + int(255 * sheen * math.sin(2 * math.pi * t + 4.2))
            else:
                rr = gg = bb = base - 12  # the mirror band inside the data
            px[x, y] = (max(0, min(255, rr)), max(0, min(255, gg)), max(0, min(255, bb)), 255)

    d = ImageDraw.Draw(img)
    # the edges: the disc's rim, the data area's inner edge, the clear ring's, the hole's
    for radius, colour, width in ((outer, (120, 124, 130, 255), 3 * SCALE),
                                  (data_in, (150, 154, 160, 200), 1 * SCALE),
                                  (clear, (140, 145, 150, 220), 2 * SCALE),
                                  (hole, (120, 124, 130, 255), 2 * SCALE)):
        d.ellipse((c - radius, c - radius, c + radius, c + radius), outline=colour, width=width)
    img = img.filter(ImageFilter.GaussianBlur(radius=SCALE * 0.4))
    return img.resize((SIZE, SIZE), Image.LANCZOS)


if __name__ == '__main__':
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    disc().save(OUT, optimize=True)
    print('wrote', os.path.normpath(OUT), os.path.getsize(OUT), 'bytes')
