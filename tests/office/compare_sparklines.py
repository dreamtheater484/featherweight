"""Sparklines against Excel's (S4): each sparkline of Excel's printed page (one bitmap per cell, see
make_sparkline_tests.py) against the same cell of Featherweight's Export PDF, by SSIM; each must be
>= 0.95.

  build\\Release\\Featherweight.exe tests\\office\\sparklines\\sparklines.xlsx --snapshot x.png --size 1400x900 ^
      --script "exportpdf:ours.pdf"
  python tests/office/compare_sparklines.py tests/office/sparklines/sparklines.xlsx.print.pdf ours.pdf [side.png]

Excel's printer path lays the page out 0.67 % larger than Export PDF, which Featherweight's print
layout matches (S1), so our cell is scaled to Excel's before comparing, at the best offset within
8 pixels (300 dpi) of where the scale puts it. SSIM: 8x8 windows every 4 pixels, gray, standard
constants (as fwtool pdf-compare). Needs PyMuPDF, Pillow and numpy.
"""
import sys

import fitz
import numpy as np
from PIL import Image

DPI = 300
SCALE = DPI / 72.0
K = 34.80 / 34.57  # Excel's printer path against its Export PDF (and ours)
THRESHOLD = 0.95


def render(page):
    pix = page.get_pixmap(dpi=DPI)
    return Image.frombytes('RGB', (pix.width, pix.height), pix.samples).convert('L')


def ssim(a, b):
    a, b = a.astype(float), b.astype(float)
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    h, w = a.shape
    vals = []
    for y in range(0, h - 7, 4):
        for x in range(0, w - 7, 4):
            pa, pb = a[y:y + 8, x:x + 8], b[y:y + 8, x:x + 8]
            ma, mb = pa.mean(), pb.mean()
            cov = ((pa - ma) * (pb - mb)).mean()
            vals.append(((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (pa.var() + pb.var() + c2)))
    return float(np.mean(vals)) if vals else 0.0


def main():
    ref_page, our_page = fitz.open(sys.argv[1])[0], fitz.open(sys.argv[2])[0]
    boxes = sorted([i['bbox'] for i in ref_page.get_image_info()], key=lambda b: (b[1], b[0]))
    if not boxes:
        print('the reference has no sparklines (images)')
        return 2
    ref, ours = render(ref_page), render(our_page)
    worst, scores = 1.0, []
    for n, b in enumerate(boxes, start=1):
        box = tuple(round(v * SCALE) for v in b)
        a = np.asarray(ref.crop(box))
        w, h = box[2] - box[0], box[3] - box[1]

        def cell(dx, dy):
            x, y = box[0] / K + dx, box[1] / K + dy
            return np.asarray(ours.crop((round(x), round(y), round(x + w / K), round(y + h / K))).resize((w, h), Image.BICUBIC))

        best, at = -1.0, (0, 0)
        for dy in range(-8, 9):
            for dx in range(-8, 9):
                v = ssim(a[::2, ::2], cell(dx, dy)[::2, ::2])
                if v > best:
                    best, at = v, (dx, dy)
        v = ssim(a, cell(*at))
        scores.append(v)
        worst = min(worst, v)
        print('%2d  SSIM %.4f  %s' % (n, v, 'ok' if v >= THRESHOLD else 'FAIL'))
    print('%d sparklines: lowest %.4f, mean %.4f; %d at >= %.2f' %
          (len(scores), worst, float(np.mean(scores)), sum(v >= THRESHOLD for v in scores), THRESHOLD))
    if len(sys.argv) > 3:  # Excel's and ours side by side
        x0, x1 = round(min(b[0] for b in boxes) * SCALE) - 10, round(max(b[2] for b in boxes) * SCALE) + 10
        y0, y1 = round(boxes[0][1] * SCALE) - 10, round(boxes[-1][3] * SCALE) + 10
        a = ref.crop((x0, y0, x1, y1))
        c = ours.crop((round(x0 / K), round(y0 / K), round(x1 / K), round(y1 / K))).resize(a.size, Image.BICUBIC)
        side = Image.new('L', (a.width * 2 + 20, a.height), 255)
        side.paste(a, (0, 0))
        side.paste(c, (a.width + 20, 0))
        side.save(sys.argv[3])
    return 0 if worst >= THRESHOLD else 1


if __name__ == '__main__':
    sys.exit(main())
