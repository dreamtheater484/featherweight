"""Regenerates THIRD_PARTY_NOTICES.md from the license files shipped with the pinned PDFium build.

Run after updating PDFium:  python tools/make_notices.py [third_party/pdfium/<build>-v8]
"""
import glob
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    if len(sys.argv) > 1:
        pkg = sys.argv[1]
    else:
        candidates = sorted(glob.glob(os.path.join(ROOT, "third_party", "pdfium", "*-v8")))
        if not candidates:
            sys.exit("PDFium package not found; configure the CMake build first.")
        pkg = candidates[-1]
    version = open(os.path.join(pkg, "VERSION"), encoding="utf-8").read().split()
    build = next((v.split("=")[1] for v in version if v.startswith("BUILD=")), "?")

    parts = [
        "# Third-party notices\n",
        "Featherweight itself is released under the MIT License (see LICENSE).\n",
        "Featherweight uses **PDFium** (https://pdfium.googlesource.com/pdfium/), built by "
        f"[bblanchon/pdfium-binaries](https://github.com/bblanchon/pdfium-binaries), chromium/{build}. "
        "PDFium and the libraries it includes are distributed under the licenses below.\n",
    ]
    files = [os.path.join(pkg, "LICENSE")] + sorted(glob.glob(os.path.join(pkg, "licenses", "*")))
    for path in files:
        name = os.path.splitext(os.path.basename(path))[0]
        title = "pdfium-binaries" if path.endswith("LICENSE") else name
        text = open(path, encoding="utf-8", errors="replace").read().strip()
        parts.append(f"\n## {title}\n\n```\n{text}\n```\n")
    out = os.path.join(ROOT, "THIRD_PARTY_NOTICES.md")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(parts))
    print("wrote", out, f"({len(files)} licenses)")


if __name__ == "__main__":
    main()
