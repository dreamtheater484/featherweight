"""Makes the seeds for fuzz_opc, fuzz_check and fuzz_formula from the office corpus.

  seeds/opc/      relationships and content types parts
  seeds/package/  packages as text (the format fuzz_check reads): their XML parts, without the
                  large ones (themes, full style sheets) and without binaries
  seeds/formula/  formulas, sqref lists and field instructions

Uses only the standard library. From the repository root:
  python tests/fuzz/make_seeds.py
"""
import os
import zipfile

ROOT = os.path.dirname(os.path.abspath(__file__))
OFFICE = os.path.join(ROOT, "..", "office")
SEEDS = os.path.join(ROOT, "seeds")

OPC = [
    ("generated/simple.docx", "[Content_Types].xml", "docx-content-types.xml"),
    ("generated/simple.docx", "_rels/.rels", "package.rels"),
    ("generated/tables-images-links.docx", "word/_rels/document.xml.rels", "docx-document.rels"),
    ("edge/escaped-part-names.docx", "word/_rels/document.xml.rels", "escaped-names.rels"),
    ("generated/sheets-chart-names.xlsx", "xl/worksheets/_rels/sheet1.xml.rels", "xlsx-sheet.rels"),
    ("generated/sheets-chart-names.xlsx", "[Content_Types].xml", "xlsx-content-types.xml"),
    ("generated/shapes-table-chart.pptx", "ppt/_rels/presentation.xml.rels", "pptx-presentation.rels"),
    ("generated/shapes-table-chart.pptx", "[Content_Types].xml", "pptx-content-types.xml"),
]

PACKAGES = [
    "generated/simple.docx",
    "generated/styles-unicode-comments.docx",
    "generated/sections-headers.docx",
    "edge/alternate-content.docx",
    "edge/unknown-namespaces-custom-xml.docx",
    "generated/simple.xlsx",
    "generated/sheets-chart-names.xlsx",
    "edge/extlst.xlsx",
    "generated/simple.pptx",
    "broken/bookmark-without-end.docx",
    "broken/duplicate-paragraph-id.docx",
    "broken/field-without-end.docx",
    "broken/stale-calc-chain.xlsx",
    "broken/duplicate-slide-id.pptx",
]

# Large parts that add nothing the fuzzer needs; binaries can't be written as text.
SKIP = ("theme", "stylesWithEffects", "fontTable", "webSettings", "slideLayout", "slideMaster",
        "notesMaster", "word/styles.xml", "printerSettings", "thumbnail", "/media/", "/embeddings/")

FORMULAS = [
    "SUM(Sales!B2:B7)",
    "'My sheet'!A1+'It''s'!B2",
    "SUM(Jan:Mar!$A$1)",
    "'Jan 2024:Mar 2024'!A1",
    "[1]Sheet1!A1+'[2]Other sheet'!B1",
    "\"Sales!A1\"&#REF!+#DIV/0!",
    "OFFSET($A$1,0,0,COUNTA(Data!$A:$A),1)",
    "Table1[[#This Row],[Amount]]*Rate",
    "IF(LOG10(B2)>1,Données!C3,\"no\")",
    "A1:B2 C3 $D$4:$E$5",
    "1:3 A:C",
    " REF _Ref123456 \\h ",
    "PAGEREF Intro \\h",
    "HYPERLINK \"https://example.com/\" \\l \"Intro\"",
    "#_x0000_t75",
]


def as_text(path):
    out = []
    with zipfile.ZipFile(path) as z:
        for info in z.infolist():
            name = info.filename
            if name.endswith("/") or any(s in "/" + name for s in SKIP):
                continue
            data = z.read(name)
            if not (name.endswith(".xml") or name.endswith(".rels") or name.endswith(".vml")):
                continue
            out.append(b"@" + name.encode("utf-8") + b"\n" + data)
    return b"\n".join(out)


def write(folder, name, data):
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, name), "wb") as f:
        f.write(data)
    print(f"  {os.path.relpath(os.path.join(folder, name), ROOT)}  ({len(data):,} bytes)")


def main():
    for source, part, name in OPC:
        with zipfile.ZipFile(os.path.join(OFFICE, source)) as z:
            write(os.path.join(SEEDS, "opc"), name, z.read(part))
    for source in PACKAGES:
        stem = source.replace("/", "-").rsplit(".", 1)
        write(os.path.join(SEEDS, "package"), f"{stem[0]}-{stem[1]}.txt", as_text(os.path.join(OFFICE, source)))
    for i, formula in enumerate(FORMULAS, start=1):
        write(os.path.join(SEEDS, "formula"), f"formula-{i:02}.txt", formula.encode("utf-8"))


if __name__ == "__main__":
    main()
