"""Generates the drawing documents of the public corpus (plan section 10, fwdraw in section 4.5):

  generated/drawings-dml.docx  DrawingML: preset shapes in Word's default style, fills (solid,
                               gradients, patterns, pictures), lines (widths, dashes, caps, arrows,
                               connectors), shadows, rotation and flips, groups, pictures (crop,
                               transparency, recoloring, SVG with its PNG copy) and a text box
  generated/drawings-vml.docx  VML: a watermark in the header, shapes, a gradient, lines, a text box,
                               a group, a cropped picture, shape types by number and by path

The XML is written by hand, so this needs only the standard library, and the output is
deterministic (fixed zip timestamps). From the repository root:

  python tests/office/make_drawing_tests.py

Then make their reference renders on the Office reference PC (scripts/reference-renders.ps1).
"""
import os
import struct
import zipfile
import zlib

ROOT = os.path.dirname(os.path.abspath(__file__))
GENERATED = os.path.join(ROOT, "generated")
ZIP_DATE = (1980, 1, 1, 0, 0, 0)
EMU = 12700  # per point

NS = (
    'xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main" '
    'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" '
    'xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing" '
    'xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" '
    'xmlns:pic="http://schemas.openxmlformats.org/drawingml/2006/picture" '
    'xmlns:wps="http://schemas.microsoft.com/office/word/2010/wordprocessingShape" '
    'xmlns:wpg="http://schemas.microsoft.com/office/word/2010/wordprocessingGroup" '
    'xmlns:a14="http://schemas.microsoft.com/office/drawing/2010/main" '
    'xmlns:v="urn:schemas-microsoft-com:vml" '
    'xmlns:o="urn:schemas-microsoft-com:office:office" '
    'xmlns:w10="urn:schemas-microsoft-com:office:word"'
)
WPS = "http://schemas.microsoft.com/office/word/2010/wordprocessingShape"
WPG = "http://schemas.microsoft.com/office/word/2010/wordprocessingGroup"
PIC = "http://schemas.openxmlformats.org/drawingml/2006/picture"
REL = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"

THEME = (
    '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
    '<a:theme xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" name="Office Theme"><a:themeElements>'
    '<a:clrScheme name="Office"><a:dk1><a:sysClr val="windowText" lastClr="000000"/></a:dk1>'
    '<a:lt1><a:sysClr val="window" lastClr="FFFFFF"/></a:lt1><a:dk2><a:srgbClr val="44546A"/></a:dk2>'
    '<a:lt2><a:srgbClr val="E7E6E6"/></a:lt2><a:accent1><a:srgbClr val="4472C4"/></a:accent1>'
    '<a:accent2><a:srgbClr val="ED7D31"/></a:accent2><a:accent3><a:srgbClr val="A5A5A5"/></a:accent3>'
    '<a:accent4><a:srgbClr val="FFC000"/></a:accent4><a:accent5><a:srgbClr val="5B9BD5"/></a:accent5>'
    '<a:accent6><a:srgbClr val="70AD47"/></a:accent6><a:hlink><a:srgbClr val="0563C1"/></a:hlink>'
    '<a:folHlink><a:srgbClr val="954F72"/></a:folHlink></a:clrScheme>'
    '<a:fontScheme name="Office"><a:majorFont><a:latin typeface="Calibri Light"/><a:ea typeface=""/><a:cs typeface=""/>'
    '</a:majorFont><a:minorFont><a:latin typeface="Calibri"/><a:ea typeface=""/><a:cs typeface=""/></a:minorFont></a:fontScheme>'
    '<a:fmtScheme name="Office"><a:fillStyleLst><a:solidFill><a:schemeClr val="phClr"/></a:solidFill>'
    '<a:gradFill rotWithShape="1"><a:gsLst><a:gs pos="0"><a:schemeClr val="phClr"><a:lumMod val="110000"/>'
    '<a:satMod val="105000"/><a:tint val="67000"/></a:schemeClr></a:gs><a:gs pos="50000"><a:schemeClr val="phClr">'
    '<a:lumMod val="105000"/><a:satMod val="103000"/><a:tint val="73000"/></a:schemeClr></a:gs><a:gs pos="100000">'
    '<a:schemeClr val="phClr"><a:lumMod val="105000"/><a:satMod val="109000"/><a:tint val="81000"/></a:schemeClr></a:gs>'
    '</a:gsLst><a:lin ang="5400000" scaled="0"/></a:gradFill><a:gradFill rotWithShape="1"><a:gsLst><a:gs pos="0">'
    '<a:schemeClr val="phClr"><a:satMod val="103000"/><a:lumMod val="102000"/><a:tint val="94000"/></a:schemeClr></a:gs>'
    '<a:gs pos="50000"><a:schemeClr val="phClr"><a:satMod val="110000"/><a:lumMod val="100000"/><a:shade val="100000"/>'
    '</a:schemeClr></a:gs><a:gs pos="100000"><a:schemeClr val="phClr"><a:lumMod val="99000"/><a:satMod val="120000"/>'
    '<a:shade val="78000"/></a:schemeClr></a:gs></a:gsLst><a:lin ang="5400000" scaled="0"/></a:gradFill></a:fillStyleLst>'
    '<a:lnStyleLst><a:ln w="6350" cap="flat" cmpd="sng" algn="ctr"><a:solidFill><a:schemeClr val="phClr"/></a:solidFill>'
    '<a:prstDash val="solid"/><a:miter lim="800000"/></a:ln><a:ln w="12700" cap="flat" cmpd="sng" algn="ctr"><a:solidFill>'
    '<a:schemeClr val="phClr"/></a:solidFill><a:prstDash val="solid"/><a:miter lim="800000"/></a:ln><a:ln w="19050" '
    'cap="flat" cmpd="sng" algn="ctr"><a:solidFill><a:schemeClr val="phClr"/></a:solidFill><a:prstDash val="solid"/>'
    '<a:miter lim="800000"/></a:ln></a:lnStyleLst><a:effectStyleLst><a:effectStyle><a:effectLst/></a:effectStyle>'
    '<a:effectStyle><a:effectLst/></a:effectStyle><a:effectStyle><a:effectLst><a:outerShdw blurRad="57150" dist="19050" '
    'dir="5400000" algn="ctr" rotWithShape="0"><a:srgbClr val="000000"><a:alpha val="63000"/></a:srgbClr></a:outerShdw>'
    '</a:effectLst></a:effectStyle></a:effectStyleLst><a:bgFillStyleLst><a:solidFill><a:schemeClr val="phClr"/>'
    '</a:solidFill><a:solidFill><a:schemeClr val="phClr"><a:tint val="95000"/><a:satMod val="170000"/></a:schemeClr>'
    '</a:solidFill><a:gradFill rotWithShape="1"><a:gsLst><a:gs pos="0"><a:schemeClr val="phClr"><a:tint val="93000"/>'
    '<a:satMod val="150000"/><a:shade val="98000"/><a:lumMod val="102000"/></a:schemeClr></a:gs><a:gs pos="50000">'
    '<a:schemeClr val="phClr"><a:tint val="98000"/><a:satMod val="130000"/><a:shade val="90000"/><a:lumMod val="103000"/>'
    '</a:schemeClr></a:gs><a:gs pos="100000"><a:schemeClr val="phClr"><a:shade val="63000"/><a:satMod val="120000"/>'
    '</a:schemeClr></a:gs></a:gsLst><a:lin ang="5400000" scaled="0"/></a:gradFill></a:bgFillStyleLst></a:fmtScheme>'
    '</a:themeElements><a:objectDefaults/><a:extraClrSchemeLst/></a:theme>'
)

# Word's default shape style (Insert > Shapes in Word 2013 and later).
STYLE = (
    '<wps:style><a:lnRef idx="2"><a:schemeClr val="accent1"><a:shade val="50000"/></a:schemeClr></a:lnRef>'
    '<a:fillRef idx="1"><a:schemeClr val="accent1"/></a:fillRef><a:effectRef idx="0"><a:schemeClr val="accent1"/>'
    '</a:effectRef><a:fontRef idx="minor"><a:schemeClr val="lt1"/></a:fontRef></wps:style>'
)


def png(width, height, pixel):
    """A PNG (RGBA) with pixel(x, y) -> (r, g, b, a)."""
    raw = b"".join(b"\0" + b"".join(bytes(pixel(x, y)) for x in range(width)) for y in range(height))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def photo(x, y, w=120, h=80):
    # A smooth color field with a dark diagonal band: enough structure to see crops and recoloring.
    band = abs((x - y * w / h)) < 10
    return (40 if band else int(255 * x / w), 40 if band else int(255 * y / h), 160 if band else 200, 255)


PHOTO = png(120, 80, photo)
TILE = png(16, 16, lambda x, y: (237, 125, 49, 255) if (x < 8) != (y < 8) else (255, 255, 255, 255))
SVG = (
    '<svg xmlns="http://www.w3.org/2000/svg" width="96" height="64" viewBox="0 0 96 64">'
    '<rect x="2" y="2" width="92" height="60" rx="10" fill="#4472C4"/>'
    '<circle cx="32" cy="32" r="18" fill="#FFC000"/><path d="M56 48 L72 16 L88 48 Z" fill="#70AD47"/></svg>'
).encode()
SVG_PNG = png(96, 64, lambda x, y: (200, 200, 200, 255))  # a plain copy, so it's clear which one is shown


class Doc:
    def __init__(self):
        self.body = []
        self.rels = []  # (id, type, target)
        self.media = {}
        self.next_id = 1

    def rel(self, kind, target):
        rid = f"rId{len(self.rels) + 10}"
        self.rels.append((rid, f"{REL}/{kind}", target))
        return rid

    def image(self, name, data):
        self.media[f"word/media/{name}"] = data
        return self.rel("image", f"media/{name}")

    def para(self, *runs):
        self.body.append("<w:p>" + "".join(runs) + "</w:p>")

    def heading(self, text):
        self.body.append(f'<w:p><w:pPr><w:spacing w:before="240" w:after="60"/></w:pPr><w:r><w:rPr><w:b/></w:rPr>'
                         f'<w:t>{text}</w:t></w:r></w:p>')

    def inline(self, w, h, uri, graphic, name=None):
        n = self.next_id
        self.next_id += 1
        return (f'<w:r><w:drawing><wp:inline distT="0" distB="0" distL="0" distR="0"><wp:extent cx="{int(w * EMU)}" '
                f'cy="{int(h * EMU)}"/><wp:effectExtent l="0" t="0" r="0" b="0"/><wp:docPr id="{n}" '
                f'name="{name or f"Drawing {n}"}"/><wp:cNvGraphicFramePr/><a:graphic><a:graphicData uri="{uri}">'
                f'{graphic}</a:graphicData></a:graphic></wp:inline></w:drawing></w:r>')

    def save(self, path, header=None):
        rels = list(self.rels)
        rels.append(("rId1", f"{REL}/theme", "theme/theme1.xml"))
        if header:
            rels.append(("rId2", f"{REL}/header", "header1.xml"))
        sect = ('<w:sectPr>' + ('<w:headerReference w:type="default" r:id="rId2"/>' if header else '') +
                '<w:pgSz w:w="12240" w:h="15840"/><w:pgMar w:top="1080" w:right="1080" w:bottom="1080" w:left="1080" '
                'w:header="720" w:footer="720" w:gutter="0"/></w:sectPr>')
        document = (f'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<w:document {NS}><w:body>' +
                    "".join(self.body) + sect + "</w:body></w:document>")
        types = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
                 '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
                 '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
                 '<Default Extension="xml" ContentType="application/xml"/><Default Extension="png" ContentType="image/png"/>'
                 '<Default Extension="svg" ContentType="image/svg+xml"/>'
                 '<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.'
                 'wordprocessingml.document.main+xml"/><Override PartName="/word/theme/theme1.xml" '
                 'ContentType="application/vnd.openxmlformats-officedocument.theme+xml"/>' +
                 ('<Override PartName="/word/header1.xml" ContentType="application/vnd.openxmlformats-officedocument.'
                  'wordprocessingml.header+xml"/>' if header else '') + '</Types>')
        root_rels = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
                     '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                     f'<Relationship Id="rId1" Type="{REL}/officeDocument" Target="word/document.xml"/></Relationships>')
        doc_rels = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
                    '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">' +
                    "".join(f'<Relationship Id="{i}" Type="{t}" Target="{target}"/>' for i, t, target in rels) +
                    '</Relationships>')
        parts = [("[Content_Types].xml", types), ("_rels/.rels", root_rels), ("word/document.xml", document),
                 ("word/_rels/document.xml.rels", doc_rels), ("word/theme/theme1.xml", THEME)]
        if header:
            parts.append(("word/header1.xml", f'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
                                              f'<w:hdr {NS}>{header}</w:hdr>'))
        parts += sorted(self.media.items())
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
            for name, data in parts:
                info = zipfile.ZipInfo(name, ZIP_DATE)
                info.compress_type = zipfile.ZIP_DEFLATED
                z.writestr(info, data.encode() if isinstance(data, str) else data)
        print("wrote", os.path.relpath(path, ROOT))


SPACE = '<w:r><w:t xml:space="preserve"> </w:t></w:r>'


def xfrm(w, h, attrs=""):
    return f'<a:xfrm{attrs}><a:off x="0" y="0"/><a:ext cx="{int(w * EMU)}" cy="{int(h * EMU)}"/></a:xfrm>'


def wsp(w, h, geom="rect", props="", style=STYLE, attrs="", av="", text=None):
    body = (f'<wps:txbx><w:txbxContent><w:p><w:r><w:t>{text}</w:t></w:r></w:p></w:txbxContent></wps:txbx>'
            if text else "")
    return (f'<wps:wsp><wps:cNvSpPr/><wps:spPr>{xfrm(w, h, attrs)}<a:prstGeom prst="{geom}"><a:avLst>{av}</a:avLst>'
            f'</a:prstGeom>{props}</wps:spPr>{style}{body}<wps:bodyPr rot="0" vert="horz" wrap="square" lIns="91440" '
            f'tIns="45720" rIns="91440" bIns="45720" anchor="ctr"/></wps:wsp>')


def solid(rgb, extra=""):
    return f'<a:solidFill><a:srgbClr val="{rgb}">{extra}</a:srgbClr></a:solidFill>'


def ln(w, inner="", attrs=""):
    return f'<a:ln w="{int(w * EMU)}"{attrs}>{inner}</a:ln>'


def gradient(stops, how):
    gs = "".join(f'<a:gs pos="{p}"><a:srgbClr val="{c}"/></a:gs>' for p, c in stops)
    return f'<a:gradFill rotWithShape="1"><a:gsLst>{gs}</a:gsLst>{how}</a:gradFill>'


def pic(w, h, rid, blip_extra="", fill_extra="<a:stretch><a:fillRect/></a:stretch>", geom="rect", props="",
        blip_ext=""):
    return (f'<pic:pic><pic:nvPicPr><pic:cNvPr id="0" name="picture"/><pic:cNvPicPr/></pic:nvPicPr><pic:blipFill>'
            f'<a:blip r:embed="{rid}">{blip_extra}{blip_ext}</a:blip>{fill_extra}</pic:blipFill><pic:spPr>{xfrm(w, h)}'
            f'<a:prstGeom prst="{geom}"><a:avLst/></a:prstGeom>{props}</pic:spPr></pic:pic>')


PRESETS = [
    "rect", "roundRect", "ellipse", "triangle", "rtTriangle", "parallelogram", "trapezoid", "diamond",
    "pentagon", "hexagon", "octagon", "star5", "star12", "rightArrow", "leftRightArrow", "upArrow", "chevron",
    "homePlate", "cube", "can", "donut", "noSmoking", "blockArc", "heart", "lightningBolt", "sun", "moon",
    "smileyFace", "cloud", "wave", "doubleWave", "flowChartDecision", "flowChartDocument",
    "flowChartMagneticDisk", "wedgeRectCallout", "wedgeEllipseCallout", "cloudCallout", "leftBrace",
    "bracketPair", "frame", "plaque", "foldedCorner", "bevel", "curvedRightArrow", "circularArrow", "ribbon2",
    "verticalScroll", "gear6", "teardrop", "pie", "chord", "mathPlus", "mathMultiply", "plus", "snip2SameRect",
    "round2DiagRect", "horizontalScroll", "irregularSeal1", "uturnArrow", "quadArrow", "funnel", "halfFrame",
]


def drawings_dml():
    d = Doc()
    photo_id = d.image("photo.png", PHOTO)
    tile_id = d.image("tile.png", TILE)
    svg_png_id = d.image("vector.png", SVG_PNG)
    svg_id = d.image("vector.svg", SVG)

    d.heading("Preset shapes in Word's default style")
    runs = []
    for name in PRESETS:
        runs += [d.inline(48, 36, WPS, wsp(48, 36, name), name), SPACE]
    d.para(*runs)

    d.heading("Fills")
    fills = [
        solid("ED7D31", '<a:alpha val="50000"/>'),
        gradient([(0, "FF0000"), (100000, "0000FF")], '<a:lin ang="0" scaled="0"/>'),
        gradient([(0, "FFFFFF"), (100000, "4472C4")], '<a:lin ang="2700000" scaled="1"/>'),
        gradient([(0, "70AD47"), (50000, "FFC000"), (100000, "ED7D31")], '<a:lin ang="5400000" scaled="0"/>'),
        gradient([(0, "FFFFFF"), (100000, "4472C4")],
                 '<a:path path="circle"><a:fillToRect l="50000" t="50000" r="50000" b="50000"/></a:path>'),
        gradient([(0, "FFFFFF"), (100000, "4472C4")],
                 '<a:path path="rect"><a:fillToRect l="50000" t="50000" r="50000" b="50000"/></a:path>'),
        gradient([(0, "FFC000"), (100000, "7030A0")],
                 '<a:path path="circle"><a:fillToRect l="0" t="0" r="100000" b="100000"/></a:path>'),
        '<a:pattFill prst="ltDnDiag"><a:fgClr><a:srgbClr val="4472C4"/></a:fgClr><a:bgClr><a:srgbClr val="FFFFFF"/>'
        '</a:bgClr></a:pattFill>',
        '<a:pattFill prst="smCheck"><a:fgClr><a:srgbClr val="000000"/></a:fgClr><a:bgClr><a:srgbClr val="FFFFFF"/>'
        '</a:bgClr></a:pattFill>',
        '<a:pattFill prst="pct20"><a:fgClr><a:srgbClr val="C00000"/></a:fgClr><a:bgClr><a:srgbClr val="FFF2CC"/>'
        '</a:bgClr></a:pattFill>',
        '<a:pattFill prst="horzBrick"><a:fgClr><a:srgbClr val="843C0C"/></a:fgClr><a:bgClr><a:srgbClr val="FBE5D6"/>'
        '</a:bgClr></a:pattFill>',
        f'<a:blipFill><a:blip r:embed="{photo_id}"/><a:stretch><a:fillRect/></a:stretch></a:blipFill>',
        f'<a:blipFill><a:blip r:embed="{tile_id}"/><a:tile tx="0" ty="0" sx="100000" sy="100000" flip="none" '
        f'algn="tl"/></a:blipFill>',
    ]
    runs = []
    for i, f in enumerate(fills):
        geom = "ellipse" if i in (4, 6) else "rect"
        runs += [d.inline(72, 48, WPS, wsp(72, 48, geom, f + ln(1, solid("404040")), "")), SPACE]
    d.para(*runs)

    d.heading("Lines, dashes, caps, joins and arrows")
    black = solid("000000")
    lines = [
        ln(0.25, black), ln(1, black), ln(3, black), ln(6, black),
        ln(2, black + '<a:prstDash val="dash"/>'), ln(2, black + '<a:prstDash val="sysDot"/>'),
        ln(2, black + '<a:prstDash val="lgDashDotDot"/>'),
        ln(2, black + '<a:custDash><a:ds d="300000" sp="100000"/><a:ds d="100000" sp="100000"/></a:custDash>'),
        ln(8, solid("4472C4"), ' cap="rnd"'), ln(8, solid("4472C4"), ' cap="sq"'),
    ]
    for l in lines:
        d.para(d.inline(160, 0, WPS, wsp(160, 0, "line", l, "")))
    arrows = ["triangle", "stealth", "diamond", "oval", "arrow"]
    for a in arrows:
        for size in ("sm", "med", "lg"):
            d.para(d.inline(160, 0, WPS, wsp(160, 0, "line", ln(1.5, black + f'<a:headEnd type="{a}" w="{size}" '
                                                              f'len="{size}"/><a:tailEnd type="{a}" w="{size}" len="{size}"/>'),
                                             "")))
    runs = []
    for join in ("<a:miter lim=\"800000\"/>", "<a:round/>", "<a:bevel/>"):
        geom = ('<a:custGeom><a:avLst/><a:gdLst/><a:rect l="0" t="0" r="r" b="b"/><a:pathLst><a:path w="4" h="2">'
                '<a:moveTo><a:pt x="0" y="2"/></a:moveTo><a:lnTo><a:pt x="1" y="0"/></a:lnTo><a:lnTo><a:pt x="2" y="2"/>'
                '</a:lnTo><a:lnTo><a:pt x="3" y="0"/></a:lnTo><a:lnTo><a:pt x="4" y="2"/></a:lnTo></a:path></a:pathLst>'
                '</a:custGeom>')
        shape = (f'<wps:wsp><wps:cNvSpPr/><wps:spPr>{xfrm(96, 36)}{geom}<a:noFill/>'
                 f'{ln(6, solid("ED7D31") + join)}</wps:spPr><wps:bodyPr/></wps:wsp>')
        runs += [d.inline(96, 36, WPS, shape), SPACE]
    for geom in ("bentConnector3", "curvedConnector3"):
        runs += [d.inline(96, 36, WPS, wsp(96, 36, geom, ln(1.5, solid("C00000") + '<a:tailEnd type="triangle"/>'),
                                           "")), SPACE]
    d.para(*runs)

    d.heading("Shadows, rotation, flips and groups")
    shadow = ('<a:effectLst><a:outerShdw blurRad="50800" dist="38100" dir="2700000" algn="tl" rotWithShape="0">'
              '<a:prstClr val="black"><a:alpha val="40000"/></a:prstClr></a:outerShdw></a:effectLst>')
    hard = ('<a:effectLst><a:outerShdw dist="76200" dir="2700000" algn="tl" rotWithShape="0">'
            '<a:srgbClr val="7F7F7F"/></a:outerShdw></a:effectLst>')
    runs = [
        d.inline(72, 48, WPS, wsp(72, 48, "roundRect", shadow)), SPACE,
        d.inline(72, 48, WPS, wsp(72, 48, "ellipse", hard)), SPACE,
        d.inline(72, 36, WPS, wsp(72, 36, "rightArrow", "", attrs=' rot="1800000"')), SPACE,
        d.inline(72, 48, WPS, wsp(72, 48, "rtTriangle", "", attrs=' flipH="1"')), SPACE,
        d.inline(72, 48, WPS, wsp(72, 48, "rtTriangle", "", attrs=' flipV="1"')), SPACE,
    ]
    child = lambda x, y, w, h, geom, color, extra="": (
        f'<wps:wsp><wps:cNvSpPr/><wps:spPr><a:xfrm{extra}><a:off x="{x * EMU}" y="{y * EMU}"/><a:ext cx="{w * EMU}" '
        f'cy="{h * EMU}"/></a:xfrm><a:prstGeom prst="{geom}"><a:avLst/></a:prstGeom>{solid(color)}{ln(1, black)}'
        f'</wps:spPr><wps:bodyPr/></wps:wsp>')
    star = child(12, 8, 24, 16, "star5", "70AD47", ' rot="2700000"')
    for rot in ("", ' rot="900000"'):
        group = (f'<wpg:wgp><wpg:cNvGrpSpPr/><wpg:grpSpPr><a:xfrm{rot}><a:off x="0" y="0"/><a:ext cx="{96 * EMU}" '
                 f'cy="{64 * EMU}"/><a:chOff x="0" y="0"/><a:chExt cx="{48 * EMU}" cy="{32 * EMU}"/></a:xfrm>'
                 f'</wpg:grpSpPr>{child(0, 0, 24, 16, "rect", "4472C4")}{child(24, 16, 24, 16, "ellipse", "FFC000")}'
                 f'{star}</wpg:wgp>')
        runs += [d.inline(96, 64, WPG, group), SPACE]
    d.para(*runs)

    d.heading("Pictures")
    variants = [
        ("", "<a:stretch><a:fillRect/></a:stretch>", "rect", ""),
        ("", '<a:srcRect l="25000" r="25000" t="10000"/><a:stretch><a:fillRect/></a:stretch>', "rect", ""),
        ('<a:alphaModFix amt="50000"/>', "<a:stretch><a:fillRect/></a:stretch>", "rect", ""),
        ("<a:grayscl/>", "<a:stretch><a:fillRect/></a:stretch>", "rect", ""),
        ('<a:duotone><a:prstClr val="black"/><a:srgbClr val="FFC000"/></a:duotone>',
         "<a:stretch><a:fillRect/></a:stretch>", "rect", ""),
        ('<a:biLevel thresh="50000"/>', "<a:stretch><a:fillRect/></a:stretch>", "rect", ""),
        ('<a:lum bright="70000" contrast="-70000"/>', "<a:stretch><a:fillRect/></a:stretch>", "rect", ""),
        ("", "<a:stretch><a:fillRect/></a:stretch>", "ellipse", ln(3, solid("ED7D31"))),
    ]
    runs = []
    for blip, fill, geom, props in variants:
        runs += [d.inline(90, 60, PIC, pic(90, 60, photo_id, blip, fill, geom, props)), SPACE]
    svg_ext = ('<a:extLst><a:ext uri="{96DAC541-7B7A-43D3-8B79-37D633B846F1}"><asvg:svgBlip '
               'xmlns:asvg="http://schemas.microsoft.com/office/drawing/2016/SVG/main" '
               f'r:embed="{svg_id}"/></a:ext></a:extLst>')
    runs += [d.inline(96, 64, PIC, pic(96, 64, svg_png_id, blip_ext=svg_ext)), SPACE]
    d.para(*runs)

    d.heading("A text box")
    d.para(d.inline(200, 60, WPS, wsp(200, 60, "rect", solid("FFF2CC") + ln(1, solid("BF9000")), style="",
                                      text="Text in a text box")))
    d.save(os.path.join(GENERATED, "drawings-dml.docx"))


def drawings_vml():
    d = Doc()
    photo_id = d.image("photo.png", PHOTO)

    def pict(inner):
        return f"<w:r><w:pict>{inner}</w:pict></w:r>"

    d.heading("VML shapes")
    star_type = '<v:shapetype id="_x0000_t12" coordsize="21600,21600" o:spt="12"/>'
    heart = ('<v:shape id="heart" coordsize="21600,21600" path="m10800,5400c10800,0,0,0,0,7200,0,14400,10800,21600,'
             '10800,21600,10800,21600,21600,14400,21600,7200,21600,0,10800,0,10800,5400xe" '
             'style="width:54pt;height:54pt" fillcolor="#c00000" strokecolor="#7f0000" strokeweight="1.5pt"/>')
    d.para(
        pict('<v:rect id="r1" style="width:72pt;height:48pt" fillcolor="#4472c4" strokecolor="#2f528f" '
             'strokeweight="1pt"/>'), SPACE,
        pict('<v:roundrect id="r2" style="width:72pt;height:48pt" arcsize="20%" fillcolor="#ffc000" stroked="f"/>'), SPACE,
        pict('<v:oval id="r3" style="width:72pt;height:48pt" strokecolor="#70ad47" strokeweight="3pt">'
             '<v:fill type="gradient" color="#70ad47" color2="white" angle="90"/></v:oval>'), SPACE,
        pict(star_type + '<v:shape id="r4" type="#_x0000_t12" style="width:54pt;height:54pt" fillcolor="#ed7d31"/>'),
        SPACE, pict(heart), SPACE,
        pict('<v:shape id="r5" style="width:72pt;height:48pt" fillcolor="silver" strokecolor="black">'
             '<v:shadow on="t" color="#7f7f7f" offset="4pt,4pt"/></v:shape>'),
    )
    d.heading("VML lines and a polyline")
    d.para(
        pict('<v:line id="l1" from="0,0" to="144pt,0" strokecolor="black" strokeweight="2pt">'
             '<v:stroke dashstyle="dash" endarrow="block"/></v:line>'))
    d.para(
        pict('<v:polyline id="l2" points="0,30pt,30pt,0,60pt,30pt,90pt,0" filled="f" strokecolor="#c00000" '
             'strokeweight="3pt"><v:stroke joinstyle="round" startarrow="oval" endarrow="classic"/></v:polyline>'))
    d.heading("A VML group, a text box and a cropped picture")
    group = ('<v:group id="g1" style="width:96pt;height:64pt" coordsize="960,640">'
             '<v:rect style="position:absolute;left:0;top:0;width:480;height:320" fillcolor="#4472c4" stroked="f"/>'
             '<v:oval style="position:absolute;left:480;top:320;width:480;height:320" fillcolor="#ffc000"/>'
             '<v:line from="0,640" to="960,0" strokecolor="black" strokeweight="2pt"/></v:group>')
    textbox = ('<v:shapetype id="_x0000_t202" coordsize="21600,21600" o:spt="202" path="m,l,21600r21600,l21600,xe">'
               '<v:stroke joinstyle="miter"/><v:path gradientshapeok="t" o:connecttype="rect"/></v:shapetype>'
               '<v:shape id="t1" type="#_x0000_t202" style="width:144pt;height:48pt" fillcolor="#fff2cc" '
               'strokecolor="#bf9000"><v:textbox><w:txbxContent><w:p><w:r><w:t>Text in a VML text box</w:t></w:r></w:p>'
               '</w:txbxContent></v:textbox></v:shape>')
    picture = ('<v:shapetype id="_x0000_t75" coordsize="21600,21600" o:spt="75" o:preferrelative="t" '
               'path="m@4@5l@4@11@9@11@9@5xe" filled="f" stroked="f"><v:stroke joinstyle="miter"/><v:formulas>'
               '<v:f eqn="if lineDrawn pixelLineWidth 0"/><v:f eqn="sum @0 1 0"/><v:f eqn="sum 0 0 @1"/>'
               '<v:f eqn="prod @2 1 2"/><v:f eqn="prod @3 21600 pixelWidth"/><v:f eqn="prod @3 21600 pixelHeight"/>'
               '<v:f eqn="sum @0 0 1"/><v:f eqn="prod @6 1 2"/><v:f eqn="prod @7 21600 pixelWidth"/>'
               '<v:f eqn="sum @8 21600 0"/><v:f eqn="prod @7 21600 pixelHeight"/><v:f eqn="sum @10 21600 0"/>'
               '</v:formulas><v:path o:extrusionok="f" gradientshapeok="t" o:connecttype="rect"/>'
               '<o:lock v:ext="edit" aspectratio="t"/></v:shapetype>'
               f'<v:shape id="p1" type="#_x0000_t75" style="width:90pt;height:60pt"><v:imagedata r:id="{photo_id}" '
               'cropleft="16384f" croptop="6554f" o:title="photo"/></v:shape>'
               f'<v:shape id="p2" type="#_x0000_t75" style="width:90pt;height:60pt"><v:imagedata r:id="{photo_id}" '
               'gain="19661f" blacklevel="22938f" o:title="washout"/></v:shape>')
    d.para(pict(group), SPACE, pict(textbox), SPACE, pict(picture))
    # Word's watermark: WordArt in the header, behind the text, centered on the page.
    watermark = (
        '<w:p><w:r><w:pict><v:shapetype id="_x0000_t136" coordsize="21600,21600" o:spt="136" adj="10800" '
        'path="m@7,l@8,m@5,21600l@6,21600e"><v:formulas><v:f eqn="sum #0 0 10800"/><v:f eqn="prod #0 2 1"/>'
        '<v:f eqn="sum 21600 0 @1"/><v:f eqn="sum 0 0 @2"/><v:f eqn="sum 21600 0 @3"/><v:f eqn="if @0 @3 0"/>'
        '<v:f eqn="if @0 21600 @1"/><v:f eqn="if @0 0 @2"/><v:f eqn="if @0 @4 21600"/><v:f eqn="mid @5 @6"/>'
        '<v:f eqn="mid @8 @5"/><v:f eqn="mid @7 @8"/><v:f eqn="mid @6 @7"/><v:f eqn="sum @6 0 @5"/></v:formulas>'
        '<v:path textpathok="t" o:connecttype="custom" o:connectlocs="@9,0;@10,10800;@11,21600;@12,10800" '
        'o:connectangles="270,180,90,0"/><v:textpath on="t" fitshape="t"/><v:handles><v:h position="#0,bottomRight" '
        'xrange="6629,14971"/></v:handles><o:lock v:ext="edit" text="t" shapetype="t"/></v:shapetype>'
        '<v:shape id="PowerPlusWaterMarkObject1" o:spid="_x0000_s1025" type="#_x0000_t136" style="position:absolute;'
        'margin-left:0;margin-top:0;width:412.4pt;height:137.45pt;rotation:315;z-index:-251657216;'
        'mso-position-horizontal:center;mso-position-horizontal-relative:margin;mso-position-vertical:center;'
        'mso-position-vertical-relative:margin" o:allowincell="f" fillcolor="silver" stroked="f"><v:fill opacity=".5"/>'
        '<v:textpath style="font-family:&quot;Calibri&quot;;font-size:1pt" string="DRAFT"/></v:shape></w:pict></w:r></w:p>')
    d.save(os.path.join(GENERATED, "drawings-vml.docx"), header=watermark)


def main():
    drawings_dml()
    drawings_vml()


if __name__ == "__main__":
    main()
