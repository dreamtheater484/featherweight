"""Generates tests/pdfs/scanned.pdf: two pages that are only pictures of text, as a scanner makes
them (F1, f1-pdf: Recognize text). The second page is stored turned a quarter (its /Rotate shows it
upright), as scans often are.

Requires: pip install reportlab pillow
Run:      python tests/make_scan_pdf.py
"""
import io
import os

from PIL import Image, ImageDraw, ImageFont
from pypdf import PdfReader, PdfWriter
from reportlab.lib.pagesizes import letter
from reportlab.lib.utils import ImageReader
from reportlab.pdfgen import canvas

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pdfs", "scanned.pdf")
DPI = 150

PAGES = [
    [
        "Featherweight reads scanned pages",
        "",
        "The quick brown fox jumps over the lazy dog.",
        "Recognized text can be found, selected and copied.",
        "Invoice number 20481, due on 12 December 2026.",
    ],
    [
        "A second page, scanned sideways",
        "",
        "Windows recognizes the text offline.",
        "Nothing is uploaded and nothing is downloaded.",
    ],
]


def page_image(lines):
    w, h = int(8.5 * DPI), int(11 * DPI)
    img = Image.new("L", (w, h), 255)
    draw = ImageDraw.Draw(img)
    font = ImageFont.truetype(os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts", "arial.ttf"), int(DPI * 0.2))
    y = int(DPI * 1.0)
    for line in lines:
        draw.text((int(DPI * 1.0), y), line, fill=0, font=font)
        y += int(DPI * 0.4)
    return img


def main():
    buf = io.BytesIO()
    c = canvas.Canvas(buf, pagesize=letter, invariant=1)
    c.setTitle("Scanned pages")
    for i, lines in enumerate(PAGES):
        img = page_image(lines)
        if i == 1:
            # Stored sideways: the picture turned counterclockwise on a landscape page.
            img = img.rotate(90, expand=True)
            c.setPageSize((letter[1], letter[0]))
            c.drawImage(ImageReader(img), 0, 0, letter[1], letter[0])
        else:
            c.setPageSize(letter)
            c.drawImage(ImageReader(img), 0, 0, letter[0], letter[1])
        c.showPage()
    c.save()
    # The sideways page shows upright: /Rotate 90.
    reader = PdfReader(io.BytesIO(buf.getvalue()))
    writer = PdfWriter()
    for i, page in enumerate(reader.pages):
        if i == 1:
            page.rotate(90)
        writer.add_page(page)
    with open(OUT, "wb") as f:
        writer.write(f)
    print(OUT, os.path.getsize(OUT), "bytes")


if __name__ == "__main__":
    main()
