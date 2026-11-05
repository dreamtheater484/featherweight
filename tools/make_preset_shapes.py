"""Generates src/draw/PresetShapes.inc: DrawingML's preset shapes as compact text for draw::Geometry.

The shapes come from presetShapeDefinitions.xml, in OfficeOpenXML-DrawingMLGeometries.zip inside
ECMA-376 Part 1, 5th edition (December 2016), used under Ecma's copyright license (see
THIRD_PARTY_NOTICES.md). Download ECMA-376-1_5th_edition_december_2016.zip from
https://ecma-international.org/publications-and-standards/standards/ecma-376/ and run, from the
repository root (standard library only):

  python tools/make_preset_shapes.py <ECMA-376-1_5th_edition_december_2016.zip | presetShapeDefinitions.xml>

Each shape becomes one string of statements separated by ';' (Geometry.cpp reads them):
  a NAME FMLA        an adjust value and its default (avLst)
  g NAME FMLA        a guide (gdLst)
  p W H F S          a path: its own width and height (0: the shape's), fill (n normal, 0 none,
                     l lighten, L lightenLess, d darken, D darkenLess) and stroke (1 or 0)
  m X Y / l X Y      moveTo / lnTo
  A WR HR ST SW      arcTo
  q X1 Y1 X2 Y2      quadBezTo
  c X1 Y1 X2 Y2 X3 Y3  cubicBezTo
  z                  close
  r L T R B          the text rectangle
Adjust handles and connection sites aren't needed to draw, so they're left out.

The 5th edition lists upDownArrow twice and leaves out upArrow, so upArrow is made here from
downArrow, turned upside down.
"""
import hashlib
import io
import os
import sys
import xml.etree.ElementTree as ET
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "src", "draw", "PresetShapes.inc")
ZIP_SHA256 = "9d0bcad9cf06054785b03762fcfadbf6bab7e54a5f9d69434e34b7fd464d4129"
NS = "{http://schemas.openxmlformats.org/drawingml/2006/main}"
FILL = {"norm": "n", "none": "0", "lighten": "l", "lightenLess": "L", "darken": "d", "darkenLess": "D"}


def load(path):
    if path.lower().endswith(".zip"):
        data = open(path, "rb").read()
        digest = hashlib.sha256(data).hexdigest()
        if digest != ZIP_SHA256:
            sys.exit(f"{path}: SHA-256 {digest} is not the 5th edition's {ZIP_SHA256}")
        outer = zipfile.ZipFile(io.BytesIO(data))
        inner = zipfile.ZipFile(io.BytesIO(outer.read("OfficeOpenXML-DrawingMLGeometries.zip")))
        return inner.read("presetShapeDefinitions.xml")
    return open(path, "rb").read()


def local(e):
    return e.tag.split("}")[-1]


def shape_def(shape):
    out = []
    for section, letter in (("avLst", "a"), ("gdLst", "g")):
        for list_el in shape.findall(NS + section):
            for gd in list_el.findall(NS + "gd"):
                out.append(f"{letter} {gd.get('name')} {' '.join(gd.get('fmla').split())}")
    for path_list in shape.findall(NS + "pathLst"):
        for path in path_list.findall(NS + "path"):
            stroke = "0" if path.get("stroke") in ("0", "false") else "1"
            out.append(f"p {path.get('w', '0')} {path.get('h', '0')} {FILL[path.get('fill', 'norm')]} {stroke}")
            for cmd in path:
                kind = local(cmd)
                pts = [f"{pt.get('x')} {pt.get('y')}" for pt in cmd.findall(NS + "pt")]
                if kind == "moveTo":
                    out.append("m " + pts[0])
                elif kind == "lnTo":
                    out.append("l " + pts[0])
                elif kind == "arcTo":
                    out.append(f"A {cmd.get('wR')} {cmd.get('hR')} {cmd.get('stAng')} {cmd.get('swAng')}")
                elif kind == "quadBezTo":
                    out.append("q " + " ".join(pts))
                elif kind == "cubicBezTo":
                    out.append("c " + " ".join(pts))
                elif kind == "close":
                    out.append("z")
                else:
                    sys.exit(f"unknown path command {kind}")
    rect = shape.find(NS + "rect")
    if rect is not None:
        out.append(f"r {rect.get('l')} {rect.get('t')} {rect.get('r')} {rect.get('b')}")
    return ";".join(out)


def upside_down(text):
    # Only what downArrow uses: paths in the shape's own coordinates, with lines.
    head, guides, body, flipped = [], [], [], {}

    def flip(y):
        if y not in flipped:
            flipped[y] = f"flip{len(flipped)}"
            guides.append(f"g {flipped[y]} +- b 0 {y}")
        return flipped[y]

    for statement in text.split(";"):
        f = statement.split(" ")
        if f[0] in ("a", "g"):
            head.append(statement)
        elif f[0] == "p" and f[1:3] == ["0", "0"] or f[0] == "z":
            body.append(statement)
        elif f[0] in ("m", "l"):
            body.append(f"{f[0]} {f[1]} {flip(f[2])}")
        elif f[0] == "r":
            body.append(f"r {f[1]} {flip(f[4])} {f[3]} {flip(f[2])}")
        else:
            sys.exit(f"can't turn '{statement}' upside down")
    return ";".join(head + guides + body)


def literal(text, width=100):
    # Adjacent literals, each well under MSVC's per-literal limit.
    chunks = [text[i:i + width] for i in range(0, len(text), width)] or [""]
    return "\n     ".join(f'"{c}"' for c in chunks)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = ET.fromstring(load(sys.argv[1]))
    shapes = {}
    for shape in root:
        # The 5th edition lists upDownArrow twice, identically; keep the first.
        shapes.setdefault(shape.tag, shape_def(shape))
    shapes.setdefault("upArrow", upside_down(shapes["downArrow"]))
    lines = [
        "// Generated by tools/make_preset_shapes.py from presetShapeDefinitions.xml in ECMA-376 Part 1,",
        "// 5th edition, used under Ecma International's copyright license (THIRD_PARTY_NOTICES.md).",
        "// upArrow, missing from that edition, is downArrow upside down.",
        "// Do not edit. Sorted by name; the statements are described in the script.",
        "// clang-format off",
    ]
    for name in sorted(shapes):
        lines.append(f'{{"{name}",\n     {literal(shapes[name])}}},')
    lines.append("// clang-format on")
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote", OUT, f"({len(shapes)} shapes, {sum(len(s) for s in shapes.values())} characters)")


if __name__ == "__main__":
    main()
