"""SSIM between two folders of PNG pictures, as fwtool pdf-compare measures it (tools/fwtool/PdfCompare.cpp:
grey levels, 11 x 11 Gaussian windows with sigma 1.5, edges clamped, K1 0.01, K2 0.03, the mean over
the picture). P3's picture export check (scripts/picture-export-check.ps1) uses it to compare each
exported slide picture with P1's page renderer's drawing of the same slide at the same size.

  python scripts/png-ssim.py --json OUT.json --pairs A1.png B1.png [A2.png B2.png ...]

Writes {"pairs": [{"a", "b", "ssim", "error"}]}. Needs Pillow and numpy.
"""
import argparse
import json
import sys

import numpy as np
from PIL import Image


def grey(path):
    im = Image.open(path)
    if im.mode in ("RGBA", "LA", "PA") or (im.mode == "P" and "transparency" in im.info):
        im = im.convert("RGBA")
        white = Image.new("RGBA", im.size, (255, 255, 255, 255))
        im = Image.alpha_composite(white, im)
    return np.asarray(im.convert("L"), dtype=np.float64)


_k = np.exp(-((np.arange(11) - 5) ** 2) / (2 * 1.5 * 1.5))
_k /= _k.sum()


def gauss(a):
    # Across, then down, edges clamped, as PdfCompare.cpp's Gauss.
    p = np.pad(a, ((0, 0), (5, 5)), mode="edge")
    t = sum(_k[i] * p[:, i:i + a.shape[1]] for i in range(11))
    p = np.pad(t, ((5, 5), (0, 0)), mode="edge")
    return sum(_k[i] * p[i:i + a.shape[0], :] for i in range(11))


def ssim(a, b):
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    ma, mb = gauss(a), gauss(b)
    va, vb, cov = gauss(a * a) - ma * ma, gauss(b * b) - mb * mb, gauss(a * b) - ma * mb
    s = ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2))
    return float(s.mean()) if s.size else 1.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", required=True)
    ap.add_argument("--pairs", nargs="+", required=True)
    args = ap.parse_args()
    if len(args.pairs) % 2:
        sys.exit("--pairs takes pictures two by two")
    out = []
    for a, b in zip(args.pairs[0::2], args.pairs[1::2]):
        row = {"a": a, "b": b, "ssim": None, "error": None}
        try:
            ga, gb = grey(a), grey(b)
            if ga.shape != gb.shape:
                row["error"] = f"sizes differ: {ga.shape[1]}x{ga.shape[0]} against {gb.shape[1]}x{gb.shape[0]}"
            else:
                row["ssim"] = round(ssim(ga, gb), 6)
        except Exception as e:  # a missing or unreadable picture is reported, not the end of the run
            row["error"] = str(e)
        out.append(row)
    with open(args.json, "w", encoding="utf-8") as f:
        json.dump({"pairs": out}, f, indent=1)


if __name__ == "__main__":
    main()
