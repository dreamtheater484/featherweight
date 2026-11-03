"""Generates the file icons Featherweight registers for documents, workbooks and presentations
(src/FileDocument.ico, src/FileWorkbook.ico, src/FilePresentation.ico) and a preview PNG.

PDFs keep the app icon (src/Featherweight.ico, made by tools/make_icon.py).
Run:  python tools/make_file_icons.py
"""
import os
import sys

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PAGE = (255, 255, 255)
PAGE_EDGE = (150, 156, 170)
FOLD = (226, 230, 238)
TEXT = (196, 201, 212)

KINDS = {
    "Document": (59, 111, 224),      # blue
    "Workbook": (31, 157, 85),       # green
    "Presentation": (224, 96, 59),   # orange
}


def symbol(d, kind, box, s, color):
    """The kind's mark, in white, inside `box` (x0, y0, x1, y1) of the badge."""
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    line = max(int(s * 0.026), 1)
    if kind == "Document":
        half = h * 0.11
        for i, frac in enumerate((1.0, 1.0, 0.6)):
            y = y0 + h * (0.14 + 0.36 * i)
            d.rounded_rectangle([x0, y - half, x0 + w * frac, y + half], radius=half, fill=color)
    elif kind == "Workbook":
        d.rectangle([x0, y0, x1, y1], outline=color, width=line)
        d.rectangle([x0 + w * 0.36 - line / 2, y0, x0 + w * 0.36 + line / 2, y1], fill=color)
        for frac in (1 / 3, 2 / 3):
            y = y0 + h * frac
            d.rectangle([x0, y - line / 2, x1, y + line / 2], fill=color)
    else:
        d.rectangle([x0, y0, x1, y1 - h * 0.24], outline=color, width=line)
        d.rectangle([x0 + w * 0.2, y0 + h * 0.24, x0 + w * 0.62, y0 + h * 0.46], fill=color)
        d.rectangle([x0 + w / 2 - line / 2, y1 - h * 0.24, x0 + w / 2 + line / 2, y1], fill=color)


def render(kind, size):
    ss = 8
    S = size * ss
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    color = KINDS[kind] + (255,)
    small = size <= 20

    # The page, with its top-right corner folded.
    left, right = S * (0.16 if not small else 0.10), S * (0.84 if not small else 0.90)
    top, bottom = S * 0.06, S * 0.94
    fold = S * (0.22 if not small else 0.28)
    edge = max(int(S * (0.025 if not small else 0.06)), 1)
    outline = [(left, top), (right - fold, top), (right, top + fold), (right, bottom), (left, bottom)]
    d.polygon(outline, fill=PAGE + (255,))
    d.line(outline + [outline[0]], fill=PAGE_EDGE + (255,), width=edge, joint="curve")
    d.polygon([(right - fold, top), (right - fold, top + fold), (right, top + fold)], fill=FOLD + (255,),
              outline=PAGE_EDGE + (255,))
    d.line([(right - fold, top), (right - fold, top + fold), (right, top + fold)], fill=PAGE_EDGE + (255,),
           width=edge, joint="curve")

    # Faint text lines above the badge.
    if not small:
        line = max(int(S * 0.03), 1)
        for i in range(3):
            y = top + fold + S * (0.04 + 0.075 * i)
            end = right - S * 0.10 if i else right - fold - S * 0.04
            if i == 0:
                end = right - fold - S * 0.04
            d.rounded_rectangle([left + S * 0.10, y - line / 2, end, y + line / 2], radius=line / 2,
                                fill=TEXT + (255,))

    # The kind's badge across the lower part of the page.
    if small:
        badge = [left + edge * 1.5, S * 0.42, right - edge * 1.5, bottom - edge * 1.5]
        d.rectangle(badge, fill=color)
        pad = S * 0.10
    else:
        badge = [S * 0.06, S * 0.48, S * 0.70, S * 0.90]
        d.rounded_rectangle(badge, radius=S * 0.08, fill=color)
        pad = S * 0.11
    symbol(d, kind, (badge[0] + pad, badge[1] + pad * 0.85, badge[2] - pad, badge[3] - pad * 0.85), S,
           (255, 255, 255, 255))
    return img.resize((size, size), Image.LANCZOS)


def main():
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    sheet = Image.new("RGBA", (sum(sizes) + 10 * len(sizes), 256 * len(KINDS) + 10 * (len(KINDS) - 1)),
                      (240, 240, 240, 255))
    for row, kind in enumerate(KINDS):
        images = [render(kind, s) for s in sizes]
        path = os.path.join(ROOT, "src", "File%s.ico" % kind)
        images[-1].save(path, format="ICO", sizes=[(s, s) for s in sizes], append_images=images[:-1])
        x = 0
        for s, im in zip(sizes, images):
            sheet.paste(im, (x, row * 266), im)
            x += s + 10
        print("wrote", path)
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "file-icons-preview.png")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    sheet.save(out)
    print("preview:", out)


if __name__ == "__main__":
    sys.exit(main())
