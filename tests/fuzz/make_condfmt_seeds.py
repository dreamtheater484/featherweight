"""Seeds for fuzz_condfmt: worksheet parts with conditional formats.

    python tests/fuzz/make_condfmt_seeds.py

From tests/sheet/condfmt-cases.xlsx (Excel's cases), one seed per rule block with the sheet's
values, and the conditional-format workbooks of tests/office (each sheet whole).
"""
import os
import re
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "tests", "fuzz", "seeds", "condfmt")


def sheets(path):
    with zipfile.ZipFile(path) as z:
        for n in sorted(z.namelist()):
            if re.match(r"xl/worksheets/sheet\d+\.xml$", n):
                yield n, z.read(n).decode("utf-8")


def trimmed(cases, xml):
    """The sheet's start and cells, only in column A and the columns `xml`'s ranges name."""
    refs = " ".join(a or b for a, b in re.findall(r'sqref="([^"]*)"|<xm:sqref>([^<]*)<', xml))
    cols = {"A"} | set(re.findall(r"\b([A-Z]{1,3})\d+", refs))
    start = cases[:cases.index("<sheetData>") + len("<sheetData>")]
    rows = []
    for r, cells in re.findall(r'<row r="(\d+)">(.*?)</row>', cases):
        kept = "".join(c for c in re.findall(r'<c r="[A-Z]+\d+".*?(?:/>|</c>)', cells)
                       if re.match(r'<c r="([A-Z]+)', c).group(1) in cols)
        if kept:
            rows.append('<row r="%s">%s</row>' % (r, kept))
    return start + "".join(rows) + "</sheetData>"


def main():
    os.makedirs(OUT, exist_ok=True)
    count = 0
    _, cases = next(sheets(os.path.join(ROOT, "tests", "sheet", "condfmt-cases.xlsx")))
    blocks = re.findall(r"<conditionalFormatting .*?</conditionalFormatting>", cases)
    ext = re.findall(r"<x14:conditionalFormatting .*?</x14:conditionalFormatting>", cases)
    for i, b in enumerate(blocks):
        with open(os.path.join(OUT, "case-%03d.xml" % i), "w", encoding="utf-8", newline="\n") as f:
            f.write(trimmed(cases, b) + b + "</worksheet>")
        count += 1
    x14 = ('<extLst><ext uri="{78C0D931-6437-407d-A8EE-F0AAD7539E65}" '
           'xmlns:x14="http://schemas.microsoft.com/office/spreadsheetml/2009/9/main"><x14:conditionalFormattings>%s'
           '</x14:conditionalFormattings></ext></extLst>')
    for i, e in enumerate(ext):
        # A data bar's Excel 2007 rule points to its Excel 2010 settings: keep the pair.
        m = re.search(r'id="(\{[^}]*\})"', e)
        linked = [b for b in blocks if m and m.group(1) in b]
        with open(os.path.join(OUT, "x14-%02d.xml" % i), "w", encoding="utf-8", newline="\n") as f:
            f.write(trimmed(cases, e) + "".join(linked) + x14 % e + "</worksheet>")
        count += 1
    for name in ("excel", "sheet"):
        path = os.path.join(ROOT, "tests", "office", name, "conditional-formats.xlsx")
        if not os.path.exists(path):
            continue
        for part, xml in sheets(path):
            with open(os.path.join(OUT, "%s-%s" % (name, os.path.basename(part))), "w", encoding="utf-8", newline="\n") as f:
                f.write(xml)
            count += 1
    print("%d seeds in %s" % (count, OUT))


if __name__ == "__main__":
    main()
