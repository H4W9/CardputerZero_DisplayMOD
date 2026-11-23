#!/usr/bin/env python3
"""Draw the 256x256 store icon (same design as the 100px launcher icon).

    python tools/make_store_icon.py   -> store/icon.png
"""
import os
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZE, SS = 256, 4
K = SIZE * SS / 100.0          # design is laid out on a 100x100 grid

TEAL = (56, 200, 190, 255)
SCREEN = (20, 28, 32, 255)
LIME = (190, 240, 61, 255)
GREY = (90, 100, 110, 255)
KNOB = (238, 242, 247, 255)


def box(x0, y0, x1, y1):
    return [x0 * K, y0 * K, x1 * K, y1 * K]


def main():
    im = Image.new("RGBA", (SIZE * SS, SIZE * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # stand
    d.rectangle(box(43, 74, 57, 84), fill=TEAL)
    d.rounded_rectangle(box(30, 81.5, 70, 88), radius=3.2 * K, fill=TEAL)
    # monitor
    d.rounded_rectangle(box(9, 14, 91, 76), radius=6 * K, fill=TEAL)
    d.rounded_rectangle(box(15, 20, 85, 69.5), radius=2.5 * K, fill=SCREEN)
    # toggle on
    d.rounded_rectangle(box(22, 27, 46, 36.5), radius=4.75 * K, fill=LIME)
    d.ellipse(box(36, 27.25, 45.25, 36.25), fill=KNOB)
    d.rounded_rectangle(box(52, 30, 78, 34), radius=2 * K, fill=GREY)
    # toggle off
    d.rounded_rectangle(box(22, 40.5, 46, 50), radius=4.75 * K, fill=GREY)
    d.ellipse(box(22.75, 40.75, 32, 49.75), fill=KNOB)
    d.rounded_rectangle(box(52, 44, 72, 48), radius=2 * K, fill=GREY)
    # slider
    d.rounded_rectangle(box(22, 57, 78, 60.5), radius=1.75 * K, fill=GREY)
    d.rounded_rectangle(box(22, 57, 57, 60.5), radius=1.75 * K, fill=LIME)
    d.ellipse(box(52.5, 54.5, 60.5, 62.5), fill=KNOB)

    out = os.path.join(ROOT, "store", "icon.png")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    im.resize((SIZE, SIZE), Image.LANCZOS).save(out)
    print(out)


if __name__ == "__main__":
    main()
