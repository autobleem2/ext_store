#!/usr/bin/env python3
"""Draws icon.png - the Store's icon in the launcher's Extensions list (extension.ini's Icon=): a shop front with
a striped awning, in gold, on the dark blue card with the PlayStation symbols that PSC-Bios's icon uses - so the
two extensions read as one set.

    python tools/make_store_icon.py            (writes icon.png next to extension.ini)

Our own artwork, generated - rerun after changing it. Needs Pillow.
"""
import math
import os
import random

from PIL import Image, ImageDraw, ImageFilter

W, H = 256, 219       # the size of the other extension/App icons (PSC-Bios, ABFlashKit)
SCALE = 4             # drawn larger, then scaled down: smooth edges
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'icon.png')

GOLD = (222, 206, 118)
GOLD_LIGHT = (246, 236, 170)
GOLD_DARK = (150, 132, 58)
OUTLINE = (40, 34, 12)


def s(v):
    return int(round(v * SCALE))


def card():
    """The card: a rounded rectangle with a black rim and a thin light ring, dark blue inside, lighter in the
    middle, and the four PlayStation symbols scattered faintly over it."""
    w, h = W * SCALE, H * SCALE
    img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    radius = s(24)
    mask = Image.new('L', (w, h), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, w - 1, h - 1), radius=radius, fill=255)

    # the blue, a radial gradient
    bg = Image.new('RGBA', (w, h))
    px = bg.load()
    cx, cy = w / 2, h / 2
    far = math.hypot(cx, cy)
    for y in range(0, h, SCALE):
        for x in range(0, w, SCALE):
            t = min(1.0, math.hypot(x - cx, y - cy) / far)
            c = (int(34 - 22 * t), int(58 - 36 * t), int(88 - 52 * t), 255)
            for yy in range(y, min(h, y + SCALE)):
                for xx in range(x, min(w, x + SCALE)):
                    px[xx, yy] = c

    # the symbols: outlines, faint, a few sizes, never over the middle where the shop stands
    symbols = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(symbols)
    rng = random.Random(7)
    placed = []
    for _ in range(400):
        if len(placed) >= 26:
            break
        size = rng.choice((14, 18, 22, 28))
        x = rng.uniform(10, W - 10 - size)
        y = rng.uniform(10, H - 10 - size)
        if any(abs(x - px_) < (size + ps) * 0.75 and abs(y - py_) < (size + ps) * 0.75 for px_, py_, ps in placed):
            continue
        placed.append((x, y, size))
        alpha = rng.randint(40, 80)
        colour = (200, 210, 225, alpha)
        width = s(2.4)
        box = (s(x), s(y), s(x + size), s(y + size))
        kind = rng.randrange(4)
        if kind == 0:  # circle
            d.ellipse(box, outline=colour, width=width)
        elif kind == 1:  # square
            d.rectangle(box, outline=colour, width=width)
        elif kind == 2:  # triangle
            d.polygon([(s(x + size / 2), s(y)), (s(x + size), s(y + size)), (s(x), s(y + size))], outline=colour,
                      width=width)
        else:  # cross
            d.line((s(x), s(y), s(x + size), s(y + size)), fill=colour, width=width)
            d.line((s(x + size), s(y), s(x), s(y + size)), fill=colour, width=width)
    bg = Image.alpha_composite(bg, symbols)

    img.paste(bg, (0, 0), mask)
    d = ImageDraw.Draw(img)
    # the rims: black outside, a thin light ring inside it
    d.rounded_rectangle((0, 0, w - 1, h - 1), radius=radius, outline=(0, 0, 0, 255), width=s(3))
    d.rounded_rectangle((s(3), s(3), w - s(3), h - s(3)), radius=radius - s(3), outline=(214, 208, 224, 255),
                        width=s(1.5))
    return img


def shop():
    """The shop front, gold: a striped awning with a scalloped edge over a counter window and a door."""
    w, h = W * SCALE, H * SCALE
    layer = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    left, right = 58, 198
    roof_top, awning_bottom = 44, 94
    body_bottom = 176
    stripes = 5
    ow = s(3)

    # the building under the awning
    d.rectangle((s(left + 8), s(awning_bottom - 4), s(right - 8), s(body_bottom)), fill=GOLD, outline=OUTLINE,
                width=ow)
    # the window (left) and the door (right), cut out dark
    dark = (22, 36, 56, 255)
    d.rounded_rectangle((s(left + 22), s(awning_bottom + 18), s(left + 80), s(body_bottom - 22)), radius=s(4),
                        fill=dark, outline=OUTLINE, width=ow)
    d.rounded_rectangle((s(left + 94), s(awning_bottom + 18), s(right - 22), s(body_bottom)), radius=s(4),
                        fill=dark, outline=OUTLINE, width=ow)
    # a knob on the door, and a sill under the window
    d.ellipse((s(right - 34), s(awning_bottom + 50), s(right - 28), s(awning_bottom + 56)), fill=GOLD)
    d.rectangle((s(left + 18), s(body_bottom - 24), s(left + 84), s(body_bottom - 18)), fill=GOLD_LIGHT,
                outline=OUTLINE, width=s(2))
    # the ground line
    d.rounded_rectangle((s(left - 6), s(body_bottom - 2), s(right + 6), s(body_bottom + 8)), radius=s(4),
                        fill=GOLD_DARK, outline=OUTLINE, width=ow)

    # the awning: a trapezoid of stripes, each ending in a half circle - the scallops
    step = (right - left) / stripes
    top_inset = 10
    for i in range(stripes):
        x0, x1 = left + i * step, left + (i + 1) * step
        tx0 = left + top_inset + i * (right - left - 2 * top_inset) / stripes
        tx1 = left + top_inset + (i + 1) * (right - left - 2 * top_inset) / stripes
        colour = GOLD_LIGHT if i % 2 == 0 else GOLD_DARK
        d.polygon([(s(tx0), s(roof_top)), (s(tx1), s(roof_top)), (s(x1), s(awning_bottom - 10)),
                   (s(x0), s(awning_bottom - 10))], fill=colour)
        d.pieslice((s(x0), s(awning_bottom - 10 - step / 2), s(x1), s(awning_bottom - 10 + step / 2)), 0, 180,
                   fill=colour)
    # its outline, drawn over the stripes
    d.line([(s(left + top_inset), s(roof_top)), (s(right - top_inset), s(roof_top)), (s(right), s(awning_bottom - 10))],
           fill=OUTLINE, width=ow, joint='curve')
    d.line([(s(left + top_inset), s(roof_top)), (s(left), s(awning_bottom - 10))], fill=OUTLINE, width=ow)
    for i in range(stripes):
        x0, x1 = left + i * step, left + (i + 1) * step
        d.arc((s(x0), s(awning_bottom - 10 - step / 2), s(x1), s(awning_bottom - 10 + step / 2)), 0, 180,
              fill=OUTLINE, width=ow)
    # the sign board on the roof
    d.rounded_rectangle((s(left + 30), s(roof_top - 22), s(right - 30), s(roof_top - 2)), radius=s(5), fill=GOLD,
                        outline=OUTLINE, width=ow)
    d.rectangle((s(left + 46), s(roof_top - 14), s(right - 46), s(roof_top - 10)), fill=GOLD_DARK)

    # a sheen from the top left: the gold lighter there, darker towards the bottom right
    sheen = Image.new('L', (w, h), 0)
    sp = sheen.load()
    for y in range(0, h, SCALE):
        for x in range(0, w, SCALE):
            t = ((x / w) + (y / h)) / 2
            v = int(max(0, min(255, 150 - 300 * t + 110)))
            for yy in range(y, min(h, y + SCALE)):
                for xx in range(x, min(w, x + SCALE)):
                    sp[xx, yy] = v
    highlight = Image.new('RGBA', (w, h), (255, 255, 230, 0))
    highlight.putalpha(Image.eval(sheen, lambda v: v // 5))
    alpha = layer.getchannel('A')
    lit = Image.alpha_composite(layer, Image.composite(highlight, Image.new('RGBA', (w, h), (0, 0, 0, 0)), alpha))
    return lit


def main():
    img = card()
    glyph = shop()
    # a soft shadow under the shop
    shadow = Image.new('RGBA', glyph.size, (0, 0, 0, 0))
    shadow.putalpha(glyph.getchannel('A').point(lambda v: v * 150 // 255))
    shadow = shadow.filter(ImageFilter.GaussianBlur(s(4)))
    img.alpha_composite(shadow, (s(4), s(5)))
    img.alpha_composite(glyph)
    img = img.resize((W, H), Image.LANCZOS)
    img.save(OUT, optimize=True)
    print('wrote', os.path.normpath(OUT))


if __name__ == '__main__':
    main()
