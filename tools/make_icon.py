"""Generates src/Featherweight.ico (and a preview PNG) for Featherweight.

The feather geometry mirrors src/Logo.cpp so the icon and the in-app logo match.
Run:  python tools/make_icon.py
"""
import math
import os
import sys

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TILE_TOP = (99, 125, 255)     # #637DFF
TILE_BOTTOM = (62, 72, 214)   # #3E48D6
QUILL = (62, 72, 214)


def feather_outline(steps=400):
    """Feather outline in unit space: base at (0,0), tip at (1,0); returns list of (x, y)."""
    upper, lower = [], []
    for i in range(steps + 1):
        t = i / steps
        # Asymmetric vanes: a little fuller on the upper side, narrowing to a soft tip.
        wu = 0.185 * math.sin(math.pi * (t ** 0.72)) * (1.0 - 0.18 * t)
        wl = 0.135 * math.sin(math.pi * (t ** 0.80)) * (1.0 - 0.10 * t)
        # Two splits in the upper vane: a slanted cut that opens towards the tip.
        for c, d, span in ((0.44, 0.075, 0.10), (0.66, 0.055, 0.08)):
            if c - span <= t <= c:
                wu -= d * (t - (c - span)) / span
            elif c < t < c + 0.012:
                wu -= d * (1.0 - (t - c) / 0.012)
        upper.append((t, -wu))
        lower.append((t, wl))
    return upper + lower[::-1]


def quill(steps=40):
    return [(-0.16 + 0.86 * i / steps, 0.0) for i in range(steps + 1)]


def transform(points, cx, cy, length, angle_deg):
    a = math.radians(angle_deg)
    ca, sa = math.cos(a), math.sin(a)
    out = []
    for x, y in points:
        x -= 0.42  # center the feather (incl. quill) on the tile
        px = x * length
        py = y * length
        out.append((cx + px * ca - py * sa, cy + px * sa + py * ca))
    return out


def render(size):
    ss = 8
    S = size * ss
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))

    # Rounded tile with a vertical gradient.
    grad = Image.new("RGBA", (S, S))
    gd = ImageDraw.Draw(grad)
    for y in range(S):
        t = y / max(1, S - 1)
        col = tuple(int(TILE_TOP[i] + (TILE_BOTTOM[i] - TILE_TOP[i]) * t) for i in range(3)) + (255,)
        gd.line([(0, y), (S, y)], fill=col)
    mask = Image.new("L", (S, S), 0)
    inset = S * (0.06 if size >= 32 else 0.02)
    radius = S * 0.22
    ImageDraw.Draw(mask).rounded_rectangle([inset, inset, S - inset, S - inset], radius=radius, fill=255)
    img.paste(grad, (0, 0), mask)

    d = ImageDraw.Draw(img)
    length = S * (0.80 if size >= 32 else 0.86)
    angle = -45
    cx, cy = S * 0.5, S * 0.5
    d.polygon(transform(feather_outline(), cx, cy, length, angle), fill=(255, 255, 255, 255))
    q = transform(quill(), cx, cy, length, angle)
    width = max(int(S * (0.028 if size >= 32 else 0.05)), 1)
    d.line(q, fill=QUILL + (255,), width=width, joint="curve")
    # The quill's stem continues outside the feather in white.
    stem = transform([(-0.16, 0.0), (0.0, 0.0)], cx, cy, length, angle)
    d.line(stem, fill=(255, 255, 255, 255), width=width, joint="curve")

    return img.resize((size, size), Image.LANCZOS)


def main():
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    images = [render(s) for s in sizes]
    ico_path = os.path.join(ROOT, "src", "Featherweight.ico")
    images[-1].save(ico_path, format="ICO", sizes=[(s, s) for s in sizes], append_images=images[:-1])
    # The logo used in the README (transparent background, works on light and dark pages).
    render(512).resize((256, 256), Image.LANCZOS).save(os.path.join(ROOT, "docs", "logo.png"))
    preview = os.path.join(ROOT, "docs", "icon-preview.png")
    sheet = Image.new("RGBA", (sum(sizes) + 10 * len(sizes), 256), (240, 240, 240, 255))
    x = 0
    for s, im in zip(sizes, images):
        sheet.paste(im, (x, 0), im)
        x += s + 10
    sheet.save(preview)
    print("wrote", ico_path, "and", preview)


if __name__ == "__main__":
    sys.exit(main())
