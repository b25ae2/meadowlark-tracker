#!/usr/bin/env python3
"""Draws the Meadowlark app icon into platforms/desktop/packaging: meadowlark.png (1024x1024,
the macOS .icns is made from it at build time) and meadowlark-<size>.png for Linux's icon
theme. A dark tile with four tracker pattern columns and a lit cursor row.

    python3 tools/make_icon.py
"""
import os
from PIL import Image, ImageDraw

S = 1024
SS = 4  # supersampling for smooth edges
DIR = os.path.join(os.path.dirname(__file__), "..", "platforms", "desktop", "packaging")
LINUX_SIZES = (64, 128, 256, 512)


def main():
    n = S * SS
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    pad = 80 * SS  # macOS-style margin around the tile
    d.rounded_rectangle((pad, pad, n - pad, n - pad), radius=180 * SS, fill=(18, 20, 22, 255))

    # Title bar line, as on the tracker's screen.
    left, right = pad + 90 * SS, n - pad - 90 * SS
    d.rectangle((left, pad + 190 * SS, right, pad + 196 * SS), fill=(90, 96, 100, 255))

    colors = [(92, 200, 220), (230, 180, 60), (170, 110, 230), (92, 200, 220)]
    cols = 4
    gap = 26 * SS
    colw = (right - left - gap * (cols - 1)) // cols
    rows = 7
    top = pad + 250 * SS
    rowh = 78 * SS
    cursor = 3
    for c in range(cols):
        x0 = left + c * (colw + gap)
        for r in range(rows):
            y = top + r * rowh
            if r == cursor:
                continue
            # A note step ("C-4") as a long dash, an empty one as short dashes.
            if (c + r) % 3 == 0:
                d.rounded_rectangle((x0, y, x0 + colw * 0.62, y + 22 * SS), radius=8 * SS, fill=colors[c] + (255,))
            else:
                for k in range(3):
                    dx = x0 + k * colw // 3
                    d.rounded_rectangle((dx, y + 6 * SS, dx + colw // 5, y + 16 * SS), radius=4 * SS,
                                        fill=(70, 74, 78, 255))
    # The cursor row: green across the first column, outline across the rest.
    y = top + cursor * rowh - 18 * SS
    d.rounded_rectangle((left - 10 * SS, y, right + 10 * SS, y + 58 * SS), radius=14 * SS,
                        outline=(60, 220, 90, 255), width=8 * SS)
    d.rounded_rectangle((left, y + 12 * SS, left + colw, y + 46 * SS), radius=8 * SS, fill=(60, 220, 90, 255))

    icon = img.resize((S, S), Image.LANCZOS)
    icon.save(os.path.join(DIR, "meadowlark.png"))
    for size in LINUX_SIZES:
        icon.resize((size, size), Image.LANCZOS).save(os.path.join(DIR, "meadowlark-%d.png" % size))


if __name__ == "__main__":
    main()
