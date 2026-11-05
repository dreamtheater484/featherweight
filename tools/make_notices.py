"""Regenerates THIRD_PARTY_NOTICES.md from the license files shipped with the pinned PDFium build,
plus Ecma's notice for fwdraw's preset shapes.

Run after updating PDFium:  python tools/make_notices.py [third_party/pdfium/<build>-v8]
"""
import glob
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# fwdraw's preset shapes (src/draw/PresetShapes.inc) implement the definitions in ECMA-376, under
# Ecma's default copyright notice (https://ecma-international.org/policies/by-ipr/ecma-text-copyright-policy/),
# which permits this in clause (iv) provided the notice is included.
ECMA = """fwdraw's preset shapes (src/draw/PresetShapes.inc, made by tools/make_preset_shapes.py) implement the
shape definitions in presetShapeDefinitions.xml from ECMA-376 Part 1, 5th edition (December 2016),
Office Open XML File Formats.

COPYRIGHT NOTICE

© 2016 Ecma International

By obtaining and/or copying this work, you (the licensee) agree that you have read, understood, and will
comply with the following terms and conditions.

This document may be copied, published and distributed to others, and certain derivative works of it may
be prepared, copied, published, and distributed, in whole or in part, provided that the above copyright
notice and this Copyright License and Disclaimer are included on all such copies and derivative works. The
only derivative works that are permissible under this Copyright License and Disclaimer are:

(i) works which incorporate all or portion of this document for the purpose of providing commentary or
explanation (such as an annotated version of the document),

(ii) works which incorporate all or portion of this document for the purpose of incorporating features that
provide accessibility,

(iii) translations of this document into languages other than English and into different formats and

(iv) works by making use of this specification in standard conformant products by implementing (e.g. by
copy and paste wholly or partly) the functionality therein.

However, the content of this document itself may not be modified in any way, including by removing the
copyright notice or references to Ecma International, except as required to translate it into languages
other than English or into a different format.

The official version of an Ecma International document is the English language version on the Ecma
International website. In the event of discrepancies between a translated version and the official
version, the official version shall govern.

The limited permissions granted above are perpetual and will not be revoked by Ecma International or its
successors or assigns.

This document and the information contained herein is provided on an "AS IS" basis and ECMA INTERNATIONAL
DISCLAIMS ALL WARRANTIES, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTY THAT THE USE OF THE
INFORMATION HEREIN WILL NOT INFRINGE ANY OWNERSHIP RIGHTS OR ANY IMPLIED WARRANTIES OF MERCHANTABILITY OR
FITNESS FOR A PARTICULAR PURPOSE."""


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
    parts.append(f"\n## ECMA-376 preset shape definitions\n\n```\n{ECMA}\n```\n")
    out =os.path.join(ROOT, "THIRD_PARTY_NOTICES.md")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(parts))
    print("wrote", out, f"({len(files)} licenses)")


if __name__ == "__main__":
    main()
