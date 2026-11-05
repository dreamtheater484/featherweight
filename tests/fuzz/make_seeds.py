"""Makes the seeds for fuzz_opc, fuzz_check and fuzz_formula from the office corpus.

  seeds/opc/      relationships and content types parts
  seeds/package/  packages as text (the format fuzz_check reads): their XML parts, without the
                  large ones (themes, full style sheets) and without binaries
  seeds/formula/  formulas, sqref lists and field instructions
  seeds/drawing/  drawings cut from the corpus, in the format fuzz_drawing reads (the element with
                  its namespaces, then "@<relationship id>" sections with the pictures it uses),
                  and tiny pictures in each format fwdraw decodes
  seeds/picture/  a tiny picture in each format a picture can be inserted from (fuzz_picture),
                  and one it refuses

Uses only the standard library. From the repository root:
  python tests/fuzz/make_seeds.py
"""
import os
import posixpath
import re
import struct
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


DRAWINGS = [
    "generated/drawings-dml.docx",
    "generated/drawings-vml.docx",
    "generated/tables-images-links.docx",
    "edge/alternate-content.docx",
    "generated/shapes-table-chart.pptx",
    "generated/picture.xlsx",
]
# The elements fuzz_drawing starts from, by part folder.
ELEMENTS = {
    "word/": rb"<w:(drawing|pict|object)\b.*?</w:\1>",
    "ppt/slides/": rb"<p:(spTree)\b.*?</p:\1>",
    "xl/drawings/": rb"<(?:xdr:)?(sp|pic|grpSp|cxnSp|graphicFrame)\b.*?</(?:xdr:)?\1>",
}
# Larger pictures are replaced by the package's smallest, and drawings that differ only in their
# preset shape are kept a few times, so the seeds stay small.
MAX_PICTURE = 4096
SAME_SHAPE = 4


def rels_of(z, part):
    folder, name = posixpath.split(part)
    try:
        data = z.read(f"{folder}/_rels/{name}.rels")
    except KeyError:
        return {}
    out = {}
    for m in re.finditer(rb"<Relationship\b[^>]*>", data):
        attrs = dict(re.findall(rb'(\w+)="([^"]*)"', m.group(0)))
        if attrs.get(b"TargetMode") != b"External":
            out[attrs[b"Id"]] = posixpath.normpath(posixpath.join(folder, attrs[b"Target"].decode()))
    return out


def drawing_seeds(path):
    """Each drawing of a document, with its part's namespaces and the pictures it refers to."""
    seeds, shapes = [], {}
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        theme = next((z.read(n) for n in names if re.search(r"theme/theme1\.xml$", n)), None)
        media = sorted((z.getinfo(n).file_size, n) for n in names if "/media/" in n)
        for part in sorted(names):
            pattern = next((p for prefix, p in ELEMENTS.items() if part.startswith(prefix)), None)
            if not pattern or not part.endswith(".xml") or "/_rels/" in part:
                continue
            data = z.read(part)
            root = re.search(rb"<[\w:]+\s[^>]*xmlns[^>]*>", data)
            namespaces = b" ".join(re.findall(rb'xmlns(?::\w+)?="[^"]*"', root.group(0))) if root else b""
            rels = rels_of(z, part)
            for m in re.finditer(pattern, data, re.S):
                element = m.group(0)
                shape = re.sub(rb'\b(prst|id|name)="[^"]*"', b"", element)
                shapes[shape] = shapes.get(shape, 0) + 1
                if shapes[shape] > SAME_SHAPE:
                    continue
                name_end = re.match(rb"<[\w:]+", element).end()
                element = element[:name_end] + b" " + namespaces + element[name_end:]
                sections = []
                for rid in dict.fromkeys(re.findall(rb'r:(?:embed|id|pict|dm)="([^"]+)"', element)):
                    target = rels.get(rid)
                    if target not in names:
                        continue
                    if z.getinfo(target).file_size > MAX_PICTURE and target.split(".")[-1] != "svg":
                        target = media[0][1]
                    sections.append(b"@" + rid + b"\n" + z.read(target))
                if theme and not seeds:
                    sections.append(b"@theme\n" + theme)
                seeds.append(b"\n".join([element] + sections))
    return seeds


def tiny_pictures():
    """A picture in each format fwdraw reads: a pixel or two, or a rectangle."""
    rows = b"".join(bytes([0, 0, 255, 0, 255, 0, 0, 0]) for _ in range(2))  # 24-bit rows padded to 4 bytes
    bmp = (b"BM" + struct.pack("<IHHI", 54 + len(rows), 0, 0, 54) +
           struct.pack("<IiiHHIIiiII", 40, 2, 2, 1, 24, 0, len(rows), 2835, 2835, 0, 0) + rows)
    gif = (b"GIF89a\x01\x00\x01\x00\x80\x00\x00\xff\x00\x00\x00\x00\x00"
           b",\x00\x00\x00\x00\x01\x00\x01\x00\x00\x02\x02D\x01\x00;")
    # TIFF: little endian, 2 x 2, uncompressed RGB.
    entries = [(256, 3, 1, 2), (257, 3, 1, 2), (258, 3, 3, None), (259, 3, 1, 1), (262, 3, 1, 2),
               (273, 4, 1, None), (277, 3, 1, 3), (278, 3, 1, 2), (279, 4, 1, 12), (284, 3, 1, 1)]
    bits_at = 8 + 2 + 12 * len(entries) + 4
    pixels_at = bits_at + 6
    tiff = b"II*\0" + struct.pack("<IH", 8, len(entries))
    for tag, kind, count, value in entries:
        value = bits_at if tag == 258 else pixels_at if tag == 273 else value
        tiff += struct.pack("<HHII" if kind == 4 or count > 1 else "<HHIHH", tag, kind, count, value,
                            *([] if kind == 4 or count > 1 else [0]))
    tiff += struct.pack("<I3H", 0, 8, 8, 8) + bytes(range(0, 240, 20))
    with zipfile.ZipFile(os.path.join(OFFICE, "generated", "drawings-dml.docx")) as z:
        png = z.read("word/media/tile.png")  # 16 x 16
        svg = z.read("word/media/vector.svg")
    ico = struct.pack("<3H", 0, 1, 1) + struct.pack("<4B2H2I", 16, 16, 0, 0, 1, 32, len(png), 22) + png
    # EMF: the header, a rectangle, the end.
    records = struct.pack("<2I4i", 43, 24, 10, 10, 90, 60) + struct.pack("<5I", 14, 20, 0, 16, 20)
    emf = struct.pack("<2I4i4i4I2H3I4i", 1, 88, 0, 0, 100, 70, 0, 0, 2646, 1852, 0x464D4520, 0x10000,
                      88 + len(records), 3, 1, 0, 0, 0, 0, 1024, 768, 320, 240) + records
    # WMF: the placeable header, the header, a rectangle, the end.
    placeable = struct.pack("<IH4hHI", 0x9AC6CDD7, 0, 0, 0, 100, 70, 1440, 0)
    checksum = 0
    for (word,) in struct.iter_unpack("<H", placeable):
        checksum ^= word
    body = struct.pack("<IH4h", 7, 0x041B, 60, 90, 10, 10) + struct.pack("<IH", 3, 0)
    wmf = (placeable + struct.pack("<H", checksum) +
           struct.pack("<3HIHIH", 1, 9, 0x300, 9 + len(body) // 2, 0, 7, 0) + body)
    return {"tile.png": png, "two.bmp": bmp, "one.gif": gif, "two.tif": tiff, "tile.ico": ico,
            "rect.emf": emf, "rect.wmf": wmf, "bom.svg": b"\xef\xbb\xbf" + svg}


def tiny_jpeg():
    """A baseline JPEG of one gray pixel: one component, a DC-only scan, one-symbol tables."""
    def segment(marker, body):
        return bytes([0xFF, marker]) + struct.pack(">H", len(body) + 2) + body
    quant = segment(0xDB, bytes([0] + [1] * 64))
    frame = segment(0xC0, struct.pack(">BHHB", 8, 1, 1, 1) + bytes([1, 0x11, 0]))
    table = bytes([1] + [0] * 15 + [0])  # one code of one bit, for symbol 0
    huffman = segment(0xC4, bytes([0x00]) + table) + segment(0xC4, bytes([0x10]) + table)
    scan = segment(0xDA, bytes([1, 1, 0x00, 0, 63, 0])) + bytes([0x3F])  # DC 0, then end of block
    return bytes([0xFF, 0xD8]) + quant + frame + huffman + scan + bytes([0xFF, 0xD9])


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
    for source in DRAWINGS:
        stem = source.split("/")[-1].replace(".", "-")
        for i, seed in enumerate(drawing_seeds(os.path.join(OFFICE, source)), start=1):
            write(os.path.join(SEEDS, "drawing"), f"{stem}-{i:03}.xml", seed)
    pictures = tiny_pictures()
    for name, data in pictures.items():
        write(os.path.join(SEEDS, "drawing"), name, data)
    pictures["gray.jpg"] = tiny_jpeg()
    for name in ("tile.png", "gray.jpg", "one.gif", "two.bmp", "two.tif", "bom.svg", "tile.ico"):
        write(os.path.join(SEEDS, "picture"), name, pictures[name])


if __name__ == "__main__":
    main()
