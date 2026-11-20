"""Generates the public set of presentations in tests/office/presentations/ (P1, docs/p1-plan.md):
one deck per feature of plan section 8.2 that P1 shows or plays, made with python-pptx and, where
python-pptx has no API (transitions, animations, backgrounds, embedded fonts, sections), with the
XML PowerPoint writes. Apache POI's decks (SmartArt and others python-pptx can't make) are listed
in tests/office/presentations/README.md and fetched separately.

Requires the venv of tests/office/requirements.txt. From the repository root:
  .venv\\Scripts\\python tests/office/make_presentations.py

The output is deterministic (fixed dates and zip timestamps), as make_test_office.py's.
"""
import io
import os
import re
import struct
import sys

import pptx
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from PIL import Image, ImageDraw
from pptx.chart.data import BubbleChartData, CategoryChartData, XyChartData
from pptx.dml.color import RGBColor
from pptx.enum.chart import XL_CHART_TYPE, XL_LEGEND_POSITION, XL_LABEL_POSITION
from pptx.enum.shapes import MSO_CONNECTOR, MSO_SHAPE
from pptx.enum.text import MSO_ANCHOR, PP_ALIGN
from pptx.opc.constants import RELATIONSHIP_TYPE as RT
from pptx.opc.package import Part
from pptx.opc.packuri import PackURI
from pptx.oxml import parse_xml
from pptx.oxml.ns import nsdecls, qn
from pptx.shapes.placeholder import ChartPlaceholder, PicturePlaceholder, TablePlaceholder
from pptx.util import Emu, Inches, Pt

sys.dont_write_bytecode = True  # no __pycache__ in tests/office
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_test_office import normalize, png, read_entries, set_core, to_bytes, write_zip  # noqa: E402

ROOT = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(ROOT, "presentations")
NS = nsdecls("a", "p", "r")
P14 = "http://schemas.microsoft.com/office/powerpoint/2010/main"
MAIN_TYPES = {
    ".pptx": "application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml",
    ".ppsx": "application/vnd.openxmlformats-officedocument.presentationml.slideshow.main+xml",
    ".potx": "application/vnd.openxmlformats-officedocument.presentationml.template.main+xml",
}


# ---------- Helpers ----------


def X(xml):
    """An element from XML that may use the a:, p: and r: prefixes."""
    xml = xml.strip()
    tag_end = min(i for i in (xml.find(" "), xml.find(">"), xml.find("/>")) if i > 0)
    return parse_xml(xml[:tag_end] + " " + NS + xml[tag_end:])


def append_all(parent, xml):
    """Appends each element of an XML fragment (siblings, a: prefix) to parent."""
    for el in list(parse_xml(f"<x {NS}>{xml}</x>")):
        parent.append(el)


def body_pr(tf, children="", **attrs):
    """Sets a text frame's a:bodyPr: its attributes, and its children replaced (in schema order:
    prstTxWarp, then the autofit choice, then scene3d)."""
    bp = tf._txBody.find(qn("a:bodyPr"))
    for c in list(bp):
        bp.remove(c)
    for k, v in attrs.items():
        bp.set(k, v)
    if children:
        append_all(bp, children)


def master_shape(prs, xml):
    """A shape on the slide master (python-pptx can't add one)."""
    prs.slide_master._element.find(qn("p:cSld")).find(qn("p:spTree")).append(X(xml))


def new_deck(title, wide=False):
    prs = pptx.Presentation()
    set_core(prs.core_properties, title)
    if wide:
        prs.slide_width, prs.slide_height = Emu(12192000), Emu(6858000)
    return prs


def add_slide(prs, layout=6, title=None):
    slide = prs.slides.add_slide(prs.slide_layouts[layout])
    if title is not None and slide.shapes.title is not None:
        slide.shapes.title.text = title
    return slide


def label(slide, text, left=0.4, top=0.15, width=9.2, height=0.6, size=20):
    box = slide.shapes.add_textbox(Inches(left), Inches(top), Inches(width), Inches(height))
    box.text_frame.text = text
    box.text_frame.paragraphs[0].runs[0].font.size = Pt(size)
    return box


def slide_append(slide, el):
    """Appends a p:transition, p:timing or mc:AlternateContent in schema order (before p:extLst)."""
    root = slide._element
    ext = root.find(qn("p:extLst"))
    if ext is not None:
        ext.addprevious(el)
    else:
        root.append(el)


def set_background(slide_or_layout, bg_xml):
    """Gives a slide (or layout, or master) its own background: p:bg first in p:cSld."""
    csld = slide_or_layout._element.find(qn("p:cSld"))
    old = csld.find(qn("p:bg"))
    if old is not None:
        csld.remove(old)
    csld.insert(0, X(bg_xml))


def image_rid(slide, data):
    _, rid = slide.part.get_or_add_image_part(io.BytesIO(data))
    return rid


def jpeg(width, height, seed=0):
    """A photo-like JPEG: soft gradients, a sun and hills."""
    img = Image.new("RGB", (width, height))
    px = img.load()
    for y in range(height):
        for x in range(width):
            px[x, y] = (min(255, 90 + y * 120 // height + seed * 9), min(255, 140 + x * 60 // width), 230 - y * 90 // height)
    d = ImageDraw.Draw(img)
    d.ellipse((width * 0.65, height * 0.1, width * 0.85, height * 0.1 + width * 0.2), fill=(255, 220, 90))
    d.polygon([(0, height), (0, height * 0.7), (width * 0.3, height * 0.5), (width * 0.6, height * 0.75),
               (width * 0.85, height * 0.55), (width, height * 0.65), (width, height)], fill=(60, 120 + seed * 5, 70))
    out = io.BytesIO()
    img.save(out, format="JPEG", quality=85)
    return out.getvalue()


def rgba_png(size=128):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.ellipse((8, 8, size - 8, size - 8), fill=(200, 40, 60, 200))
    d.rectangle((size // 3, size // 3, size * 2 // 3, size * 2 // 3), fill=(255, 255, 255, 255))
    out = io.BytesIO()
    img.save(out, format="PNG")
    return out.getvalue()


def gif(size=96):
    img = Image.new("P", (size, size), 0)
    img.putpalette([255, 255, 255, 0, 120, 200, 240, 160, 0] + [0] * (256 * 3 - 9))
    d = ImageDraw.Draw(img)
    d.rectangle((10, 10, size - 10, size - 10), fill=1)
    d.ellipse((30, 30, size - 30, size - 30), fill=2)
    out = io.BytesIO()
    img.save(out, format="GIF")
    return out.getvalue()


def fix_chart(body):
    """Two python-pptx departures from the schema, which the Open XML SDK validator reports:
    negative axis ids (unsigned and below 2^31 as PowerPoint writes them; each loses its sign, so the
    axes still pair up), and c:smooth in a radar chart's series (not in CT_RadarSer)."""
    body = re.sub(rb'(<c:(?:axId|crossAx) val=")-(\d+)"', rb'\1\2"', body)
    if b"<c:radarChart>" in body:
        body = re.sub(rb'<c:smooth val="[01]"/>', b"", body)
    return body


def finish(prs, ext=".pptx"):
    data = normalize(to_bytes(prs))
    main = MAIN_TYPES[".pptx"].encode()
    entries = []
    for n, b in read_entries(data):
        if n == "[Content_Types].xml" and ext != ".pptx":
            b = b.replace(main, MAIN_TYPES[ext].encode())
        elif n.startswith("ppt/charts/chart") and n.endswith(".xml"):
            b = fix_chart(b)
        entries.append((n, b))
    return write_zip(entries)


def save(name, data):
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, name), "wb") as f:
        f.write(data)
    print(f"  presentations/{name}  ({len(data):,} bytes)")


# ---------- Animations: the timing tree PowerPoint writes ----------

# kind: (presetID, presetClass, presetSubtype, duration in ms)
EFFECTS = {
    "appear": (1, "entr", 0, 0),
    "fade": (10, "entr", 0, 500),
    "fly": (2, "entr", 4, 500),
    "wipe": (22, "entr", 4, 500),
    "zoom": (53, "entr", 16, 500),
    "float": (42, "entr", 0, 1000),
    "split": (16, "entr", 21, 500),
    "wheel": (21, "entr", 1, 2000),       # not in P1's list: plays as appear
    "checkerboard": (5, "entr", 10, 500),  # not in P1's list either
    "grow": (6, "emph", 0, 2000),
    "spin": (8, "emph", 0, 2000),
    "fadeout": (10, "exit", 0, 500),
    "path": (0, "path", 0, 2000),
}


class Timing:
    """Builds p:timing for a slide from steps (trigger, kind, spid[, paragraph]), trigger being
    "click", "with" or "after", as PowerPoint's animation pane lists them."""

    def __init__(self):
        self.n = 0
        self.builds = {}  # spid -> (grpId, build)

    def id(self):
        self.n += 1
        return self.n

    @staticmethod
    def target(spid, para):
        if para is None:
            return f'<p:spTgt spid="{spid}"/>'
        return f'<p:spTgt spid="{spid}"><p:txEl><p:pRg st="{para}" end="{para}"/></p:txEl></p:spTgt>'

    def behaviours(self, kind, tgt):
        dur = EFFECTS[kind][3]

        def setv(val, delay=0):
            return (f'<p:set><p:cBhvr><p:cTn id="{self.id()}" dur="1" fill="hold"><p:stCondLst><p:cond delay="{delay}"/>'
                    f'</p:stCondLst></p:cTn><p:tgtEl>{tgt}</p:tgtEl><p:attrNameLst><p:attrName>style.visibility</p:attrName>'
                    f'</p:attrNameLst></p:cBhvr><p:to><p:strVal val="{val}"/></p:to></p:set>')

        def effect(transition, flt, d=dur):
            return (f'<p:animEffect transition="{transition}" filter="{flt}"><p:cBhvr><p:cTn id="{self.id()}" dur="{d}"/>'
                    f'<p:tgtEl>{tgt}</p:tgtEl></p:cBhvr></p:animEffect>')

        def anim(attr, frm, to, d=dur):
            return (f'<p:anim calcmode="lin" valueType="num"><p:cBhvr additive="base"><p:cTn id="{self.id()}" dur="{d}" fill="hold"/>'
                    f'<p:tgtEl>{tgt}</p:tgtEl><p:attrNameLst><p:attrName>{attr}</p:attrName></p:attrNameLst></p:cBhvr>'
                    f'<p:tavLst><p:tav tm="0"><p:val><p:strVal val="{frm}"/></p:val></p:tav><p:tav tm="100000"><p:val>'
                    f'<p:strVal val="{to}"/></p:val></p:tav></p:tavLst></p:anim>')

        if kind == "appear":
            return setv("visible")
        if kind == "fade":
            return setv("visible") + effect("in", "fade")
        if kind == "fly":
            return setv("visible") + anim("ppt_x", "#ppt_x", "#ppt_x") + anim("ppt_y", "1+#ppt_h/2", "#ppt_y")
        if kind == "wipe":
            return setv("visible") + effect("in", "wipe(down)")
        if kind == "zoom":
            return setv("visible") + anim("ppt_w", "0", "#ppt_w") + anim("ppt_h", "0", "#ppt_h") + effect("in", "fade")
        if kind == "float":
            return (setv("visible") + effect("in", "fade") + anim("ppt_x", "#ppt_x", "#ppt_x")
                    + anim("ppt_y", "#ppt_y+.1", "#ppt_y"))
        if kind == "split":
            return setv("visible") + effect("in", "barn(inVertical)")
        if kind == "wheel":
            return setv("visible") + effect("in", "wheel(1)")
        if kind == "checkerboard":
            return setv("visible") + effect("in", "checkerboard(across)")
        if kind == "grow":
            return (f'<p:animScale><p:cBhvr><p:cTn id="{self.id()}" dur="{dur}" fill="hold"/><p:tgtEl>{tgt}</p:tgtEl>'
                    f'</p:cBhvr><p:by x="150000" y="150000"/></p:animScale>')
        if kind == "spin":
            return (f'<p:animRot by="21600000"><p:cBhvr><p:cTn id="{self.id()}" dur="{dur}" fill="hold"/><p:tgtEl>{tgt}</p:tgtEl>'
                    f'<p:attrNameLst><p:attrName>r</p:attrName></p:attrNameLst></p:cBhvr></p:animRot>')
        if kind == "fadeout":
            return effect("out", "fade") + setv("hidden", dur - 1)
        if kind == "path":
            return (f'<p:animMotion origin="layout" path="M 0 0 L 0.25 0.25 E" pathEditMode="relative" ptsTypes="AA">'
                    f'<p:cBhvr><p:cTn id="{self.id()}" dur="{dur}" fill="hold"/><p:tgtEl>{tgt}</p:tgtEl><p:attrNameLst>'
                    f'<p:attrName>ppt_x</p:attrName><p:attrName>ppt_y</p:attrName></p:attrNameLst></p:cBhvr>'
                    f'<p:rCtr x="12500" y="12500"/></p:animMotion>')
        raise ValueError(kind)

    def effect(self, kind, spid, node, delay=0, para=None, grp=0):
        pid, cls, sub, _ = EFFECTS[kind]
        tid = self.id()
        body = self.behaviours(kind, self.target(spid, para))
        sub_attr = f' presetSubtype="{sub}"' if cls != "emph" or sub else ""
        return (f'<p:par><p:cTn id="{tid}" presetID="{pid}" presetClass="{cls}"{sub_attr} fill="hold" grpId="{grp}" '
                f'nodeType="{node}"><p:stCondLst><p:cond delay="{delay}"/></p:stCondLst><p:childTnLst>{body}</p:childTnLst>'
                f'</p:cTn></p:par>')

    def groups(self, steps, interactive=False):
        """The click groups of a sequence: each a par of sub-pars that start one after another."""
        groups = []
        for step in steps:
            trigger = step[0]
            if trigger == "click" or not groups:
                groups.append({"auto": trigger != "click", "subs": [{"delay": 0, "items": [step]}]})
            elif trigger == "with":
                groups[-1]["subs"][-1]["items"].append(step)
            else:
                last = groups[-1]["subs"][-1]
                end = last["delay"] + max(EFFECTS[s[1]][3] for s in last["items"])
                groups[-1]["subs"].append({"delay": end, "items": [step]})
        out = []
        for g in groups:
            gid = self.id()
            if interactive:
                conds = '<p:cond delay="0"/>'
            elif g["auto"]:
                conds = '<p:cond delay="indefinite"/><p:cond evt="onBegin" delay="0"><p:tn val="2"/></p:cond>'
            else:
                conds = '<p:cond delay="indefinite"/>'
            subs = []
            for s in g["subs"]:
                sid = self.id()
                items = []
                for step in s["items"]:
                    trigger, kind, spid = step[:3]
                    para = step[3] if len(step) > 3 else None
                    node = {"click": "clickEffect", "with": "withEffect", "after": "afterEffect"}[trigger]
                    grp, build = self.builds.get(spid, (0, None))
                    if spid not in self.builds:
                        self.builds[spid] = (0, "p" if para is not None else None)
                    items.append(self.effect(kind, spid, node, para=para, grp=grp))
                subs.append(f'<p:par><p:cTn id="{sid}" fill="hold"><p:stCondLst><p:cond delay="{s["delay"]}"/></p:stCondLst>'
                            f'<p:childTnLst>{"".join(items)}</p:childTnLst></p:cTn></p:par>')
            out.append(f'<p:par><p:cTn id="{gid}" fill="hold"><p:stCondLst>{conds}</p:stCondLst>'
                       f'<p:childTnLst>{"".join(subs)}</p:childTnLst></p:cTn></p:par>')
        return "".join(out)

    def xml(self, steps, triggers=(), kinds=None):
        """steps: the main sequence; triggers: (trigger spid, steps) played on a click on that shape;
        kinds: spid -> "text", "shape", "chart" or "other" (what the build list says of it)."""
        kinds = kinds or {}
        self.id(), self.id()  # 1: the root, 2: the main sequence
        main = self.groups(steps)
        seqs = []
        for trigger, tsteps in triggers:
            sid = self.id()
            inner = self.groups(tsteps, interactive=True)
            on = f'<p:cond evt="onClick" delay="0"><p:tgtEl><p:spTgt spid="{trigger}"/></p:tgtEl></p:cond>'
            seqs.append(f'<p:seq concurrent="1" nextAc="seek"><p:cTn id="{sid}" restart="whenNotActive" fill="hold" '
                        f'evtFilter="cancelBubble" nodeType="interactiveSeq"><p:stCondLst>{on}</p:stCondLst>'
                        f'<p:endSync evt="end" delay="0"><p:rtn val="all"/></p:endSync><p:childTnLst>{inner}</p:childTnLst>'
                        f'</p:cTn><p:nextCondLst>{on}</p:nextCondLst></p:seq>')
        slide_cond = '<p:tgtEl><p:sldTgt/></p:tgtEl>'
        blds = []
        # PowerPoint lists shapes (bldP) and charts (bldGraphic); pictures and tables aren't listed.
        for spid, (grp, build) in sorted(self.builds.items()):
            kind = kinds.get(spid, "shape")
            if build:
                blds.append(f'<p:bldP spid="{spid}" grpId="{grp}" build="{build}"/>')
            elif kind == "text":
                blds.append(f'<p:bldP spid="{spid}" grpId="{grp}"/>')
            elif kind == "shape":
                blds.append(f'<p:bldP spid="{spid}" grpId="{grp}" animBg="1"/>')
            elif kind == "chart":
                blds.append(f'<p:bldGraphic spid="{spid}" grpId="{grp}"><p:bldAsOne/></p:bldGraphic>')
        return (f'<p:timing><p:tnLst><p:par><p:cTn id="1" dur="indefinite" restart="never" nodeType="tmRoot"><p:childTnLst>'
                f'<p:seq concurrent="1" nextAc="seek"><p:cTn id="2" dur="indefinite" nodeType="mainSeq"><p:childTnLst>{main}'
                f'</p:childTnLst></p:cTn><p:prevCondLst><p:cond evt="onPrev" delay="0">{slide_cond}</p:cond></p:prevCondLst>'
                f'<p:nextCondLst><p:cond evt="onNext" delay="0">{slide_cond}</p:cond></p:nextCondLst></p:seq>{"".join(seqs)}'
                f'</p:childTnLst></p:cTn></p:par></p:tnLst><p:bldLst>{"".join(blds)}</p:bldLst></p:timing>')


def animate(slide, steps, triggers=()):
    kinds = {}
    for s in slide.shapes:
        tag = s._element.tag
        if tag == qn("p:graphicFrame"):
            kinds[s.shape_id] = "chart" if s.has_chart else "other"
        elif tag == qn("p:pic"):
            kinds[s.shape_id] = "other"
        else:
            kinds[s.shape_id] = "text" if s.has_text_frame and s.text_frame.text else "shape"
    slide_append(slide, X(Timing().xml(steps, triggers, kinds)))


# ---------- Embedded fonts: a pixel font, wrapped as Embedded OpenType ----------

PIXELS = {
    "A": " ### |#   #|#   #|#####|#   #|#   #|#   #", "B": "#### |#   #|#   #|#### |#   #|#   #|#### ",
    "C": " ### |#   #|#    |#    |#    |#   #| ### ", "D": "#### |#   #|#   #|#   #|#   #|#   #|#### ",
    "E": "#####|#    |#    |#### |#    |#    |#####", "F": "#####|#    |#    |#### |#    |#    |#    ",
    "G": " ### |#   #|#    |# ###|#   #|#   #| ####", "H": "#   #|#   #|#   #|#####|#   #|#   #|#   #",
    "I": " ### |  #  |  #  |  #  |  #  |  #  | ### ", "J": "  ###|   # |   # |   # |   # |#  # | ##  ",
    "K": "#   #|#  # |# #  |##   |# #  |#  # |#   #", "L": "#    |#    |#    |#    |#    |#    |#####",
    "M": "#   #|## ##|# # #|# # #|#   #|#   #|#   #", "N": "#   #|##  #|# # #|#  ##|#   #|#   #|#   #",
    "O": " ### |#   #|#   #|#   #|#   #|#   #| ### ", "P": "#### |#   #|#   #|#### |#    |#    |#    ",
    "Q": " ### |#   #|#   #|#   #|# # #|#  # | ## #", "R": "#### |#   #|#   #|#### |# #  |#  # |#   #",
    "S": " ####|#    |#    | ### |    #|    #|#### ", "T": "#####|  #  |  #  |  #  |  #  |  #  |  #  ",
    "U": "#   #|#   #|#   #|#   #|#   #|#   #| ### ", "V": "#   #|#   #|#   #|#   #|#   #| # # |  #  ",
    "W": "#   #|#   #|#   #|# # #|# # #|# # #| # # ", "X": "#   #|#   #| # # |  #  | # # |#   #|#   #",
    "Y": "#   #|#   #| # # |  #  |  #  |  #  |  #  ", "Z": "#####|    #|   # |  #  | #   |#    |#####",
    "0": " ### |#   #|#  ##|# # #|##  #|#   #| ### ", "1": "  #  | ##  |  #  |  #  |  #  |  #  | ### ",
    "2": " ### |#   #|    #|   # |  #  | #   |#####", "3": "#####|   # |  #  |   # |    #|#   #| ### ",
    "4": "   # |  ## | # # |#  # |#####|   # |   # ", "5": "#####|#    |#### |    #|    #|#   #| ### ",
    "6": "  ## | #   |#    |#### |#   #|#   #| ### ", "7": "#####|    #|   # |  #  | #   | #   | #   ",
    "8": " ### |#   #|#   #| ### |#   #|#   #| ### ", "9": " ### |#   #|#   #| ####|    #|   # | ##  ",
    ".": "     |     |     |     |     | ##  | ##  ", ",": "     |     |     |     | ##  |  #  | #   ",
    ":": "     | ##  | ##  |     | ##  | ##  |     ", "!": "  #  |  #  |  #  |  #  |  #  |     |  #  ",
    "?": " ### |#   #|    #|   # |  #  |     |  #  ", "-": "     |     |     |#####|     |     |     ",
    "'": "  #  |  #  |     |     |     |     |     ",
}
PIXEL_FAMILY = "FW Pixel"


def pixel_font():
    """A TrueType font of 5x7 pixel letters (lower case drawn as upper case), so a render that
    falls back to another font is plain to see. Made here; public domain."""
    names = {c: (c if c.isalpha() else "g%04X" % ord(c)) for c in PIXELS}
    order = [".notdef", "space"] + [names[c] for c in PIXELS]
    cmap = {32: "space"}
    for c, n in names.items():
        cmap[ord(c)] = n
        if c.isalpha():
            cmap[ord(c.lower())] = n
    glyphs, metrics = {}, {}
    pen = TTGlyphPen(None)
    for pts in (((50, 0), (50, 700), (550, 700), (550, 0)), ((100, 50), (500, 50), (500, 650), (100, 650))):  # a hollow box
        pen.moveTo(pts[0])
        for pt in pts[1:]:
            pen.lineTo(pt)
        pen.closePath()
    glyphs[".notdef"], metrics[".notdef"] = pen.glyph(), (600, 50)
    glyphs["space"], metrics["space"] = TTGlyphPen(None).glyph(), (600, 0)
    for c, rows in PIXELS.items():
        pen = TTGlyphPen(None)
        xmin = 600
        for r, row in enumerate(rows.split("|")):
            for col, ch in enumerate(row):
                if ch != "#":
                    continue
                x, y = 50 + col * 100, (6 - r) * 100
                xmin = min(xmin, x)
                pen.moveTo((x, y))
                pen.lineTo((x, y + 100))
                pen.lineTo((x + 100, y + 100))
                pen.lineTo((x + 100, y))
                pen.closePath()
        glyphs[names[c]], metrics[names[c]] = pen.glyph(), (600, xmin)
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap(cmap)
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=900, descent=-250)
    fb.setupNameTable({"familyName": PIXEL_FAMILY, "styleName": "Regular", "uniqueFontIdentifier": "FW Pixel Regular 1.000",
                       "fullName": PIXEL_FAMILY, "psName": "FWPixel-Regular", "version": "Version 1.000",
                       "copyright": "Public domain (CC0 1.0), made for Featherweight's tests"})
    fb.setupOS2(sTypoAscender=800, sTypoDescender=-200, sTypoLineGap=200, usWinAscent=900, usWinDescent=250,
                fsType=0, achVendID="FWT ", ulUnicodeRange1=1, ulCodePageRange1=1, usWeightClass=400)
    fb.setupPost()
    fb.font.recalcTimestamp = False
    fb.font["head"].created = fb.font["head"].modified = 0xE0B5_0000  # fixed, for a deterministic file
    out = io.BytesIO()
    fb.font.save(out)
    return out.getvalue(), fb.font


def eot(ttf, font):
    """Embedded OpenType (version 0x00020001, uncompressed, not XOR-ed), as PowerPoint's .fntdata."""
    os2, head = font["OS/2"], font["head"]
    name = font["name"]

    def s(text):
        b = text.encode("utf-16-le")
        return struct.pack("<H", len(b)) + b

    panose = bytes([os2.panose.bFamilyType, os2.panose.bSerifStyle, os2.panose.bWeight, os2.panose.bProportion,
                    os2.panose.bContrast, os2.panose.bStrokeVariation, os2.panose.bArmStyle, os2.panose.bLetterForm,
                    os2.panose.bMidline, os2.panose.bXHeight])
    fixed = struct.pack("<LL", 0x00020001, 0) + panose + struct.pack(
        "<BBLHH4L2LL4L", 1, 0, os2.usWeightClass, os2.fsType, 0x504C,
        os2.ulUnicodeRange1, os2.ulUnicodeRange2, os2.ulUnicodeRange3, os2.ulUnicodeRange4,
        os2.ulCodePageRange1, os2.ulCodePageRange2, head.checkSumAdjustment, 0, 0, 0, 0)
    names = (struct.pack("<H", 0) + s(name.getDebugName(1)) + struct.pack("<H", 0) + s(name.getDebugName(2))
             + struct.pack("<H", 0) + s(name.getDebugName(5)) + struct.pack("<H", 0) + s(name.getDebugName(4))
             + struct.pack("<H", 0) + s(""))
    body = fixed + names + ttf
    return struct.pack("<LL", 8 + len(body), len(ttf)) + body


# ---------- The decks ----------


def deck_layouts_placeholders():
    prs = new_deck("Layouts and placeholders", wide=True)
    for i, layout in enumerate(prs.slide_layouts):
        slide = prs.slides.add_slide(layout)
        for ph in list(slide.placeholders):
            kind = ph.placeholder_format.type
            idx = ph.placeholder_format.idx
            if ph == slide.shapes.title:
                ph.text = f"Layout {i + 1}: {layout.name}"
            elif "PICTURE" in str(kind):
                PicturePlaceholder(ph._element, ph._parent).insert_picture(io.BytesIO(jpeg(400, 300, seed=i)))
            elif "OBJECT" in str(kind) and idx == 2 and layout.name == "Two Content":
                data = CategoryChartData()
                data.categories = ["A", "B", "C"]
                data.add_series("Series 1", (3, 5, 2))
                ChartPlaceholder(ph._element, ph._parent).insert_chart(XL_CHART_TYPE.PIE, data)
            elif "OBJECT" in str(kind) and layout.name == "Comparison" and idx == 4:
                table = TablePlaceholder(ph._element, ph._parent).insert_table(3, 2).table
                for r in range(3):
                    for c in range(2):
                        table.cell(r, c).text = f"{'AB'[c]}{r + 1}"
            elif "OBJECT" in str(kind) and layout.name == "Content with Caption":
                pass  # an empty placeholder: its prompt never shows in the show or in a PDF
            elif hasattr(ph, "text_frame"):
                tf = ph.text_frame
                tf.text = f"Placeholder idx {idx} ({str(kind).split(' ')[0]})"
                p = tf.add_paragraph()
                p.text = "A second level"
                p.level = 1
    # A placeholder moved and recoloured on the slide, overriding its layout.
    slide = add_slide(prs, 1, "An overridden placeholder")
    body = slide.placeholders[1]
    body.left, body.top, body.width, body.height = Inches(6), Inches(2), Inches(6), Inches(4)
    body.text_frame.text = "Moved, resized, and red text"
    body.text_frame.paragraphs[0].runs[0].font.color.rgb = RGBColor(0xC0, 0x00, 0x00)
    # Footer, date and slide number placeholders, copied from the layout's (fields).
    for n, text in enumerate(("Date, footer and slide number", "On the next slide too")):
        slide = add_slide(prs, 5, text)
        add_footer_placeholders(slide)
    return finish(prs)


def add_footer_placeholders(slide):
    """Footer, date and slide number placeholders, as PowerPoint adds them (Insert > Header & Footer)."""
    layout = slide.slide_layout
    next_id = max(s.shape_id for s in slide.shapes) + 1
    for ph in layout.placeholders:
        kind = str(ph.placeholder_format.type)
        if not any(k in kind for k in ("DATE", "FOOTER", "SLIDE_NUMBER")):
            continue
        t = "dt" if "DATE" in kind else "ftr" if "FOOTER" in kind else "sldNum"
        if t == "dt":
            para = '<a:fld id="{0D5A7B3C-1A2B-4C5D-8E9F-0A1B2C3D4E5F}" type="datetime1"><a:rPr lang="en-US"/><a:t>1/1/2026</a:t></a:fld>'
        elif t == "sldNum":
            para = '<a:fld id="{1E6B8C4D-2B3C-4D6E-9F0A-1B2C3D4E5F60}" type="slidenum"><a:rPr lang="en-US"/><a:t>&#8249;#&#8250;</a:t></a:fld>'
        else:
            para = '<a:r><a:rPr lang="en-US"/><a:t>Featherweight public set</a:t></a:r>'
        slide.shapes._spTree.append(X(
            f'<p:sp><p:nvSpPr><p:cNvPr id="{next_id}" name="{t} {next_id}"/><p:cNvSpPr><a:spLocks noGrp="1"/></p:cNvSpPr>'
            f'<p:nvPr><p:ph type="{t}" sz="quarter" idx="{ph.placeholder_format.idx}"/></p:nvPr></p:nvSpPr><p:spPr/>'
            f'<p:txBody><a:bodyPr/><a:lstStyle/><a:p>{para}<a:endParaRPr lang="en-US"/></a:p></p:txBody></p:sp>'))
        next_id += 1


THEME_COLORS = (
    '<a:clrScheme name="Featherweight Test"><a:dk1><a:srgbClr val="1B1B1B"/></a:dk1><a:lt1><a:srgbClr val="FFFFFF"/></a:lt1>'
    '<a:dk2><a:srgbClr val="1F3A5F"/></a:dk2><a:lt2><a:srgbClr val="F2EBD9"/></a:lt2><a:accent1><a:srgbClr val="C0392B"/></a:accent1>'
    '<a:accent2><a:srgbClr val="E67E22"/></a:accent2><a:accent3><a:srgbClr val="27AE60"/></a:accent3><a:accent4><a:srgbClr val="2980B9"/>'
    '</a:accent4><a:accent5><a:srgbClr val="8E44AD"/></a:accent5><a:accent6><a:srgbClr val="16A085"/></a:accent6><a:hlink>'
    '<a:srgbClr val="0645AD"/></a:hlink><a:folHlink><a:srgbClr val="6B4BA1"/></a:folHlink></a:clrScheme>')
THEME_FONTS = (
    '<a:fontScheme name="Featherweight Test"><a:majorFont><a:latin typeface="Georgia"/><a:ea typeface=""/><a:cs typeface=""/>'
    '<a:font script="Jpan" typeface="Yu Mincho"/></a:majorFont><a:minorFont><a:latin typeface="Verdana"/><a:ea typeface=""/>'
    '<a:cs typeface=""/><a:font script="Jpan" typeface="Yu Gothic"/></a:minorFont></a:fontScheme>')


def retheme(data):
    def fix(body):
        text = body.decode("utf-8")
        text = re.sub(r"<a:clrScheme .*?</a:clrScheme>", THEME_COLORS, text, flags=re.S)
        text = re.sub(r"<a:fontScheme .*?</a:fontScheme>", THEME_FONTS, text, flags=re.S)
        text = re.sub(r'(<a:theme [^>]*name=")[^"]*"', r'\1Featherweight Test"', text)
        return text.encode("utf-8")

    return write_zip([(n, fix(b) if n == "ppt/theme/theme1.xml" else b) for n, b in read_entries(data)])


def deck_themes():
    prs = new_deck("Theme colors, fonts and the style matrix", wide=True)
    s = add_slide(prs, 5, "Theme colors: accents, tints and shades")
    for i in range(6):
        for j, mod in enumerate(("", '<a:lumMod val="20000"/><a:lumOff val="80000"/>', '<a:lumMod val="60000"/><a:lumOff val="40000"/>',
                                 '<a:lumMod val="75000"/>', '<a:lumMod val="50000"/>', '<a:alpha val="40000"/>')):
            box = s.shapes.add_shape(MSO_SHAPE.RECTANGLE, Inches(0.7 + i * 2), Inches(1.6 + j * 0.85), Inches(1.8), Inches(0.75))
            sppr = box._element.spPr
            sppr.append(X(f'<a:solidFill><a:schemeClr val="accent{i + 1}">{mod}</a:schemeClr></a:solidFill>'))
            box.text_frame.text = f"accent{i + 1}" if j == 0 else ""
    s = add_slide(prs, 5, "Theme fonts: +mj-lt and +mn-lt")
    tb = s.shapes.add_textbox(Inches(0.8), Inches(1.8), Inches(11), Inches(4))
    tf = tb.text_frame
    tf.text = "Body text in the minor font (Verdana)"
    p = tf.add_paragraph()
    r = p.add_run()
    r.text = "A run in the major font (Georgia)"
    r.font.name = "+mj-lt"
    p = tf.add_paragraph()
    r = p.add_run()
    r.text = "Text in dk2, lt2 on dk2, hlink and folHlink colors"
    r._r.get_or_add_rPr().append(X('<a:solidFill><a:schemeClr val="tx2"/></a:solidFill>'))
    s = add_slide(prs, 5, "The style matrix: shapes styled by the theme")
    for i, shape in enumerate((MSO_SHAPE.ROUNDED_RECTANGLE, MSO_SHAPE.OVAL, MSO_SHAPE.CHEVRON, MSO_SHAPE.HEXAGON)):
        sh = s.shapes.add_shape(shape, Inches(0.8 + i * 3), Inches(2), Inches(2.6), Inches(1.8))
        sh.text_frame.text = f"fillRef {i + 1}"
        style = sh._element.find(qn("p:style"))
        style.find(qn("a:fillRef")).set("idx", str(min(i + 1, 3)))
        style.find(qn("a:fillRef"))[0].set("val", f"accent{i + 2}")
        style.find(qn("a:effectRef")).set("idx", str(min(i, 2)))
    s = add_slide(prs, 5, "A dark slide: the color map overridden")
    clr = s._element.find(qn("p:clrMapOvr"))
    clr.clear()
    clr.append(X('<a:overrideClrMapping bg1="dk1" tx1="lt1" bg2="dk2" tx2="lt2" accent1="accent1" accent2="accent2" '
                 'accent3="accent3" accent4="accent4" accent5="accent5" accent6="accent6" hlink="hlink" folHlink="folHlink"/>'))
    set_background(s, '<p:bg><p:bgRef idx="1001"><a:schemeClr val="bg1"/></p:bgRef></p:bg>')
    tb = s.shapes.add_textbox(Inches(0.8), Inches(2), Inches(10), Inches(1))
    tb.text_frame.text = "Light text on the dark background, from the same theme"
    return retheme(finish(prs))


def deck_text_autofit():
    prs = new_deck("Text and autofit", wide=True)
    long_text = " ".join(["Text that overflows its box, so PowerPoint shrank it to fit."] * 6)
    s = add_slide(prs, 1, "Shrink text on overflow (normAutofit)")
    body = s.placeholders[1].text_frame
    body.text = long_text
    for _ in range(4):
        body.add_paragraph().text = long_text
    body_pr(body, '<a:normAutofit fontScale="55000" lnSpcReduction="20000"/>')
    s = add_slide(prs, 6)
    label(s, "Resize shape to fit text, no autofit, and overflow")
    for i, (fit, text) in enumerate((('<a:spAutoFit/>', "spAutoFit: the box grew to fit this text."),
                                     ('<a:noAutofit/>', long_text), ('<a:noAutofit/>', "Wrap off: this line runs past its box's edge."))):
        tb = s.shapes.add_textbox(Inches(0.5 + i * 4.2), Inches(1.2), Inches(3.8), Inches(1.2))
        tb.text_frame.text = text
        body_pr(tb.text_frame, fit, wrap="square" if i != 2 else "none", rtlCol="0")
        tb.line.color.rgb = RGBColor(0x80, 0x80, 0x80)
    s = add_slide(prs, 6)
    label(s, "Vertical text, columns, anchors and insets")
    for i, (attrs, text) in enumerate((('vert="vert"', "Vertical text, turned 90 degrees"),
                                       ('vert="vert270"', "Vertical text, turned 270 degrees"),
                                       ('vert="eaVert"', "縦書きの日本語テキスト"),
                                       ('numCol="2" spcCol="182880"', long_text),
                                       ('anchor="b" lIns="365760" tIns="365760"', "Anchored at the bottom with wide insets"),
                                       ('anchor="ctr" anchorCtr="1"', "Centred both ways"))):
        sh = s.shapes.add_shape(MSO_SHAPE.RECTANGLE, Inches(0.4 + (i % 3) * 4.2), Inches(1.0 + (i // 3) * 3.1), Inches(3.9), Inches(2.9))
        sh.text_frame.text = text
        bp = sh.text_frame._txBody.find(qn("a:bodyPr"))
        for kv in attrs.split(" "):
            k, v = kv.split("=")
            bp.set(k, v.strip('"'))
    s = add_slide(prs, 6)
    label(s, "Character formatting")
    tb = s.shapes.add_textbox(Inches(0.5), Inches(1), Inches(12), Inches(6))
    tf = tb.text_frame
    tf.word_wrap = True
    runs = (('b="1"', "bold"), ('i="1"', "italic"), ('u="sng"', "underline"), ('u="dbl"', "double"), ('u="wavy"', "wavy"),
            ('strike="sngStrike"', "strike"), ('strike="dblStrike"', "double strike"), ('baseline="30000"', "super"),
            ('baseline="-25000"', "sub"), ('cap="all"', "all caps"), ('cap="small"', "Small Caps"), ('spc="600"', "spaced"),
            ('spc="-150"', "condensed"), ('sz="3600"', "36 pt"), ('sz="1000"', "10 pt"))
    p = tf.paragraphs[0]
    for attrs, text in runs:
        p.add_run().text = " "
        r = p.add_run()
        r.text = text
        rpr = r._r.get_or_add_rPr()
        for kv in attrs.split(" "):
            k, v = kv.split("=")
            rpr.set(k, v.strip('"'))
    p = tf.add_paragraph()
    for xml, text in (('<a:solidFill><a:srgbClr val="2E75B6"/></a:solidFill>', "colored "),
                      ('<a:gradFill><a:gsLst><a:gs pos="0"><a:srgbClr val="C00000"/></a:gs><a:gs pos="100000"><a:srgbClr val="0070C0"/></a:gs></a:gsLst><a:lin ang="0" scaled="0"/></a:gradFill>', "gradient text "),
                      ('<a:ln w="12700"><a:solidFill><a:srgbClr val="000000"/></a:solidFill></a:ln><a:noFill/>', "outlined "),
                      ('<a:effectLst><a:outerShdw blurRad="38100" dist="38100" dir="2700000" algn="tl"><a:srgbClr val="000000"><a:alpha val="43000"/></a:srgbClr></a:outerShdw></a:effectLst>', "shadowed "),
                      ('<a:highlight><a:srgbClr val="FFFF00"/></a:highlight>', "highlighted")):
        r = p.add_run()
        r.text = text
        r.font.size = Pt(28)
        append_all(r._r.get_or_add_rPr(), xml)
    for text, attrs in (("Left aligned, 1.5 line spacing, 12 pt before", ('algn="l"', '<a:lnSpc><a:spcPct val="150000"/></a:lnSpc><a:spcBef><a:spcPts val="1200"/></a:spcBef>')),
                        ("Centred", ('algn="ctr"', "")), ("Right aligned", ('algn="r"', "")),
                        ("Justified text runs to both edges of the box when it wraps over more than one line, as this one does here.", ('algn="just"', "")),
                        ("مرحبا بالعالم: right-to-left Arabic text", ('algn="r" rtl="1"', "")),
                        ("日本語のテキストと漢字、ひらがな、カタカナ", ('algn="l"', ""))):
        p = tf.add_paragraph()
        r = p.add_run()
        r.text = text
        r.font.size = Pt(18)
        ppr = p._p.get_or_add_pPr()
        for kv in attrs[0].split(" "):
            k, v = kv.split("=")
            ppr.set(k, v.strip('"'))
        if attrs[1]:
            append_all(ppr, attrs[1])
    p = tf.add_paragraph()
    p.add_run().text = "A line break here"
    p._p.append(X('<a:br><a:rPr lang="en-US"/></a:br>'))
    r = p.add_run()
    r.text = "and a tab\tafter it; slide "
    p._p.append(X('<a:fld id="{2F7C9D5E-3C4D-4E7F-8A1B-2C3D4E5F6071}" type="slidenum"><a:rPr lang="en-US"/><a:t>4</a:t></a:fld>'))
    s = add_slide(prs, 6)
    label(s, "WordArt: text warped and text effects")
    for i, warp in enumerate(("textArchUp", "textWave1", "textInflate", "textSlantUp")):
        tb = s.shapes.add_textbox(Inches(0.5 + i * 3.1), Inches(2), Inches(2.9), Inches(2))
        tb.text_frame.text = "WordArt"
        tb.text_frame.paragraphs[0].runs[0].font.size = Pt(44)
        body_pr(tb.text_frame, f'<a:prstTxWarp prst="{warp}"><a:avLst/></a:prstTxWarp><a:normAutofit/>', wrap="square")
    return finish(prs)


def deck_bullets():
    prs = new_deck("Bullets and numbering")
    s = add_slide(prs, 1, "Bullets from the master, nine levels")
    tf = s.placeholders[1].text_frame
    tf.text = "Level 1"
    for lvl in range(1, 9):
        p = tf.add_paragraph()
        p.text = f"Level {lvl + 1}"
        p.level = lvl
    s = add_slide(prs, 1, "Bullet characters, fonts, colors and sizes")
    tf = s.placeholders[1].text_frame
    items = (('<a:buFont typeface="Arial"/><a:buChar char="&#8226;"/>', "Arial bullet"),
             ('<a:buFont typeface="Wingdings" pitchFamily="2" charset="2"/><a:buChar char="&#167;"/>', "Wingdings square"),
             ('<a:buFont typeface="Wingdings" pitchFamily="2" charset="2"/><a:buChar char="&#216;"/>', "Wingdings arrow"),
             ('<a:buClr><a:srgbClr val="C00000"/></a:buClr><a:buSzPct val="150000"/><a:buFont typeface="Arial"/><a:buChar char="&#8211;"/>', "Red dash at 150 %"),
             ('<a:buFontTx/><a:buChar char="&#10003;"/>', "Check mark in the text's font"),
             ('<a:buNone/>', "No bullet"))
    for i, (xml, text) in enumerate(items):
        p = tf.paragraphs[0] if i == 0 else tf.add_paragraph()
        p.text = text
        ppr = p._p.get_or_add_pPr()
        ppr.set("marL", "342900")
        ppr.set("indent", "-342900")
        append_all(ppr, xml)
    s = add_slide(prs, 1, "Numbering schemes")
    tf = s.placeholders[1].text_frame
    for i, (scheme, start) in enumerate((("arabicPeriod", 1), ("arabicPeriod", 2), ("arabicParenR", 1), ("romanUcPeriod", 1),
                                         ("romanLcPeriod", 4), ("alphaUcPeriod", 1), ("alphaLcParenR", 3), ("arabicParenBoth", 9),
                                         ("circleNumDbPlain", 1))):
        p = tf.paragraphs[0] if i == 0 else tf.add_paragraph()
        p.text = f"{scheme}, starting at {start}"
        p.level = 1 if i in (1, 4) else 0
        ppr = p._p.get_or_add_pPr()
        ppr.set("marL", "457200" if p.level == 0 else "914400")
        ppr.set("indent", "-457200")
        ppr.append(X(f'<a:buFont typeface="+mj-lt"/>'))
        ppr.append(X(f'<a:buAutoNum type="{scheme}"' + (f' startAt="{start}"' if start != 1 else "") + "/>"))
    s = add_slide(prs, 1, "Picture bullets")
    rid = image_rid(s, png(32, 32, seed=5))
    tf = s.placeholders[1].text_frame
    for i in range(3):
        p = tf.paragraphs[0] if i == 0 else tf.add_paragraph()
        p.text = f"Picture bullet {i + 1}"
        ppr = p._p.get_or_add_pPr()
        ppr.append(X(f'<a:buBlip><a:blip r:embed="{rid}"/></a:buBlip>'))
    return finish(prs)


TABLE_STYLES = (
    ("{5C22544A-7EE6-4342-B048-85BDC9FD1C3A}", "Medium Style 2 - Accent 1"),
    ("{21E4AEA4-8DFA-4A89-87EB-49C32662AFE8}", "Medium Style 2 - Accent 2"),
    ("{F5AB1C69-6EDB-4FF4-983F-18BD219EF322}", "Medium Style 2 - Accent 3"),
    ("{073A0DAA-6AF3-43AB-8588-CEC1D06C72B9}", "Medium Style 2"),
    ("{2D5ABB26-0587-4C30-8999-92F81FD0307C}", "No Style, No Grid"),
    ("{5940675A-B579-460E-94D1-54222C63F5DA}", "No Style, Table Grid"),
)


def deck_tables():
    prs = new_deck("Tables", wide=True)
    s = add_slide(prs, 5, "Built-in table styles")
    for i, (guid, name) in enumerate(TABLE_STYLES):
        gf = s.shapes.add_table(4, 3, Inches(0.4 + (i % 3) * 4.2), Inches(1.6 + (i // 3) * 2.8), Inches(4), Inches(2))
        t = gf.table
        for r in range(4):
            for c in range(3):
                t.cell(r, c).text = name if (r, c) == (0, 0) else f"{r}.{c}"
        t._tbl.tblPr.find(qn("a:tableStyleId")).text = guid
    s = add_slide(prs, 5, "Style options: first and last row and column, bands")
    for i, opts in enumerate(({"firstRow": "1", "bandRow": "1"}, {"firstCol": "1", "lastCol": "1", "bandCol": "1"},
                              {"lastRow": "1", "firstRow": "0", "bandRow": "0"})):
        gf = s.shapes.add_table(5, 4, Inches(0.4 + i * 4.2), Inches(2), Inches(4), Inches(3))
        t = gf.table
        for r in range(5):
            for c in range(4):
                t.cell(r, c).text = f"{r}{c}"
        for k, v in opts.items():
            t._tbl.tblPr.set(k, v)
    s = add_slide(prs, 5, "Merged cells, fills, borders, margins and vertical text")
    gf = s.shapes.add_table(5, 5, Inches(0.5), Inches(1.6), Inches(12), Inches(5))
    t = gf.table
    for r in range(5):
        for c in range(5):
            t.cell(r, c).text = f"R{r + 1}C{c + 1}"
    t.cell(0, 0).merge(t.cell(0, 2))
    t.cell(0, 0).text = "Merged across three columns"
    t.cell(1, 0).merge(t.cell(3, 0))
    t.cell(1, 0).text = "Merged down three rows"
    t.cell(1, 1).fill.solid()
    t.cell(1, 1).fill.fore_color.rgb = RGBColor(0xFF, 0xE6, 0x99)
    tc = t.cell(2, 2)._tc
    tcpr = tc.get_or_add_tcPr()
    for side in ("lnL", "lnR", "lnT", "lnB"):
        tcpr.append(X(f'<a:{side} w="38100" cmpd="sng"><a:solidFill><a:srgbClr val="C00000"/></a:solidFill><a:prstDash val="dash"/></a:{side}>'))
    tcpr2 = t.cell(2, 3)._tc.get_or_add_tcPr()
    tcpr2.set("vert", "vert270")
    t.cell(2, 3).text = "Vertical"
    c = t.cell(3, 3)
    c.margin_left = c.margin_top = Inches(0.3)
    c.vertical_anchor = MSO_ANCHOR.BOTTOM
    c.text = "Wide margins, bottom"
    t.cell(4, 4).text = "Right aligned, bold"
    t.cell(4, 4).text_frame.paragraphs[0].alignment = PP_ALIGN.RIGHT
    t.cell(4, 4).text_frame.paragraphs[0].runs[0].font.bold = True
    t.columns[0].width = Inches(1.6)
    t.rows[4].height = Inches(1.4)
    return finish(prs)


def deck_charts():
    prs = new_deck("Charts", wide=True)
    cat = CategoryChartData()
    cat.categories = ["Q1", "Q2", "Q3", "Q4"]
    cat.add_series("North", (12.0, 15.5, 9.25, 18.0))
    cat.add_series("South", (8.0, 11.0, 14.5, 10.0))
    cat.add_series("West", (5.5, 7.0, 6.0, 12.5))
    pie = CategoryChartData()
    pie.categories = ["Rent", "Food", "Travel", "Other"]
    pie.add_series("Spending", (45, 25, 18, 12))
    xy = XyChartData()
    for name, k in (("Measured", 1.0), ("Model", 1.2)):
        s = xy.add_series(name)
        for x in range(1, 9):
            s.add_data_point(x, round(k * x * x / 4 + (x % 3), 2))
    bubble = BubbleChartData()
    bs = bubble.add_series("Cities")
    for x, y, z in ((1, 3, 10), (2, 5, 25), (3, 2, 5), (4, 6, 40)):
        bs.add_data_point(x, y, z)
    charts = ((XL_CHART_TYPE.COLUMN_CLUSTERED, cat), (XL_CHART_TYPE.COLUMN_STACKED, cat), (XL_CHART_TYPE.BAR_CLUSTERED, cat),
              (XL_CHART_TYPE.COLUMN_STACKED_100, cat), (XL_CHART_TYPE.LINE_MARKERS, cat), (XL_CHART_TYPE.AREA_STACKED, cat),
              (XL_CHART_TYPE.PIE, pie), (XL_CHART_TYPE.PIE_EXPLODED, pie), (XL_CHART_TYPE.DOUGHNUT, pie),
              (XL_CHART_TYPE.XY_SCATTER_LINES, xy), (XL_CHART_TYPE.BUBBLE, bubble), (XL_CHART_TYPE.RADAR_MARKERS, cat))
    for i in range(0, len(charts), 2):
        s = add_slide(prs, 6)
        label(s, " and ".join(str(c[0]).split(" ")[0].replace("_", " ").title() for c in charts[i:i + 2]))
        for j, (kind, data) in enumerate(charts[i:i + 2]):
            ch = s.shapes.add_chart(kind, Inches(0.4 + j * 6.4), Inches(1.0), Inches(6.2), Inches(6.0), data).chart
            ch.has_title = True
            ch.chart_title.text_frame.text = str(kind).split(" ")[0].replace("_", " ").title()
            ch.has_legend = True
            ch.legend.position = (XL_LEGEND_POSITION.BOTTOM, XL_LEGEND_POSITION.RIGHT)[j]
            ch.legend.include_in_layout = False
            if kind in (XL_CHART_TYPE.PIE, XL_CHART_TYPE.DOUGHNUT, XL_CHART_TYPE.COLUMN_CLUSTERED):
                plot = ch.plots[0]
                plot.has_data_labels = True
                plot.data_labels.number_format = '0"%"' if kind != XL_CHART_TYPE.COLUMN_CLUSTERED else "0.0"
                plot.data_labels.number_format_is_linked = False
                if kind == XL_CHART_TYPE.PIE:
                    plot.data_labels.position = XL_LABEL_POSITION.OUTSIDE_END
    return finish(prs)


def deck_pictures():
    prs = new_deck("Pictures", wide=True)
    s = add_slide(prs, 6)
    label(s, "PNG, JPEG, GIF, transparency, crop, rotation and flips")
    s.shapes.add_picture(io.BytesIO(png(160, 120, seed=1)), Inches(0.4), Inches(1.0), Inches(3))
    s.shapes.add_picture(io.BytesIO(jpeg(640, 480)), Inches(3.6), Inches(1.0), Inches(3))
    s.shapes.add_picture(io.BytesIO(gif()), Inches(6.8), Inches(1.0), Inches(2))
    s.shapes.add_picture(io.BytesIO(rgba_png()), Inches(9.2), Inches(1.0), Inches(2))
    crop = s.shapes.add_picture(io.BytesIO(jpeg(640, 480, seed=2)), Inches(0.4), Inches(4.0), Inches(3), Inches(2.25))
    crop.crop_left, crop.crop_right, crop.crop_top, crop.crop_bottom = 0.25, 0.1, 0.2, 0.05
    rot = s.shapes.add_picture(io.BytesIO(jpeg(640, 480, seed=3)), Inches(3.8), Inches(4.0), Inches(3))
    rot.rotation = 30
    flip = s.shapes.add_picture(io.BytesIO(jpeg(640, 480, seed=4)), Inches(7.2), Inches(4.0), Inches(2.6))
    flip._element.spPr.find(qn("a:xfrm")).set("flipH", "1")
    flipv = s.shapes.add_picture(io.BytesIO(png(160, 120, seed=6)), Inches(10.2), Inches(4.0), Inches(2.6))
    flipv._element.spPr.find(qn("a:xfrm")).set("flipV", "1")
    s = add_slide(prs, 6)
    label(s, "Picture effects, fills, outlines and an SVG with its PNG fallback")
    faded = s.shapes.add_picture(io.BytesIO(jpeg(640, 480, seed=5)), Inches(0.4), Inches(1.0), Inches(3))
    faded._element.blipFill.find(qn("a:blip")).append(X('<a:alphaModFix amt="45000"/>'))
    grey = s.shapes.add_picture(io.BytesIO(jpeg(640, 480, seed=6)), Inches(3.6), Inches(1.0), Inches(3))
    grey._element.blipFill.find(qn("a:blip")).append(X('<a:grayscl/>'))
    framed = s.shapes.add_picture(io.BytesIO(jpeg(640, 480, seed=7)), Inches(6.8), Inches(1.0), Inches(3))
    framed.line.color.rgb = RGBColor(0x1F, 0x3A, 0x5F)
    framed.line.width = Pt(6)
    framed._element.spPr.append(X('<a:effectLst><a:outerShdw blurRad="76200" dist="50800" dir="2700000" algn="tl">'
                                  '<a:srgbClr val="000000"><a:alpha val="50000"/></a:srgbClr></a:outerShdw></a:effectLst>'))
    rid = image_rid(s, jpeg(400, 400, seed=8))
    oval = s.shapes.add_shape(MSO_SHAPE.OVAL, Inches(0.4), Inches(4.2), Inches(3), Inches(2.6))
    sppr = oval._element.spPr
    sppr.insert(2, X(f'<a:blipFill dpi="0" rotWithShape="1"><a:blip r:embed="{rid}"/><a:srcRect/><a:stretch><a:fillRect/></a:stretch></a:blipFill>'))
    rid2 = image_rid(s, png(48, 48, seed=9))
    tile = s.shapes.add_shape(MSO_SHAPE.ROUNDED_RECTANGLE, Inches(3.6), Inches(4.2), Inches(3), Inches(2.6))
    tile._element.spPr.insert(2, X(f'<a:blipFill dpi="0" rotWithShape="1"><a:blip r:embed="{rid2}"/><a:srcRect/>'
                                   '<a:tile tx="0" ty="0" sx="100000" sy="100000" flip="none" algn="tl"/></a:blipFill>'))
    svg = (b'<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200" viewBox="0 0 200 200">'
           b'<circle cx="100" cy="100" r="90" fill="#2980B9"/><path d="M55 105 L90 140 L150 65" stroke="#FFFFFF" '
           b'stroke-width="18" fill="none" stroke-linecap="round" stroke-linejoin="round"/></svg>')
    fallback_png = Image.new("RGB", (200, 200), (41, 128, 185))
    buf = io.BytesIO()
    fallback_png.save(buf, format="PNG")
    pic = s.shapes.add_picture(io.BytesIO(buf.getvalue()), Inches(7.2), Inches(4.2), Inches(2.6), Inches(2.6))
    svg_part = Part(PackURI("/ppt/media/image-svg1.svg"), "image/svg+xml", s.part.package, svg)
    svg_rid = s.part.relate_to(svg_part, RT.IMAGE)
    pic._element.blipFill.find(qn("a:blip")).append(X(
        f'<a:extLst><a:ext uri="{{96DAC541-7B7A-43D3-8B79-37D633B846F1}}"><asvg:svgBlip '
        f'xmlns:asvg="http://schemas.microsoft.com/office/drawing/2016/SVG/main" r:embed="{svg_rid}"/></a:ext></a:extLst>'))
    s = add_slide(prs, 6)
    big = s.shapes.add_picture(io.BytesIO(jpeg(1920, 1080, seed=10)), 0, 0, prs.slide_width, prs.slide_height)
    big.name = "Full-slide photo"
    cap = s.shapes.add_textbox(Inches(0.5), Inches(6.2), Inches(8), Inches(0.8))
    cap.text_frame.text = "A full-bleed photo with a caption over it"
    cap.text_frame.paragraphs[0].runs[0].font.color.rgb = RGBColor(0xFF, 0xFF, 0xFF)
    cap.text_frame.paragraphs[0].runs[0].font.size = Pt(28)
    return finish(prs)


def deck_shapes_groups():
    prs = new_deck("Shapes, lines and groups", wide=True)
    s = add_slide(prs, 6)
    label(s, "Preset shapes with solid, gradient and pattern fills")
    kinds = (MSO_SHAPE.RECTANGLE, MSO_SHAPE.ROUNDED_RECTANGLE, MSO_SHAPE.OVAL, MSO_SHAPE.ISOSCELES_TRIANGLE, MSO_SHAPE.RIGHT_ARROW,
             MSO_SHAPE.CHEVRON, MSO_SHAPE.PENTAGON, MSO_SHAPE.HEXAGON, MSO_SHAPE.STAR_5_POINT, MSO_SHAPE.STAR_16_POINT,
             MSO_SHAPE.HEART, MSO_SHAPE.LIGHTNING_BOLT, MSO_SHAPE.CLOUD, MSO_SHAPE.DONUT, MSO_SHAPE.BLOCK_ARC, MSO_SHAPE.CAN,
             MSO_SHAPE.CUBE, MSO_SHAPE.FLOWCHART_DECISION, MSO_SHAPE.FLOWCHART_DOCUMENT, MSO_SHAPE.RECTANGULAR_CALLOUT,
             MSO_SHAPE.CLOUD_CALLOUT, MSO_SHAPE.LEFT_RIGHT_ARROW, MSO_SHAPE.CIRCULAR_ARROW, MSO_SHAPE.SMILEY_FACE)
    fills = ('<a:solidFill><a:srgbClr val="4472C4"/></a:solidFill>',
             '<a:gradFill rotWithShape="1"><a:gsLst><a:gs pos="0"><a:srgbClr val="FFC000"/></a:gs><a:gs pos="100000"><a:srgbClr val="C00000"/></a:gs></a:gsLst><a:lin ang="5400000" scaled="0"/></a:gradFill>',
             '<a:gradFill><a:gsLst><a:gs pos="0"><a:srgbClr val="FFFFFF"/></a:gs><a:gs pos="100000"><a:srgbClr val="70AD47"/></a:gs></a:gsLst><a:path path="circle"><a:fillToRect l="50000" t="50000" r="50000" b="50000"/></a:path></a:gradFill>',
             '<a:pattFill prst="dkDnDiag"><a:fgClr><a:srgbClr val="7030A0"/></a:fgClr><a:bgClr><a:srgbClr val="FFFFFF"/></a:bgClr></a:pattFill>')
    for i, kind in enumerate(kinds):
        sh = s.shapes.add_shape(kind, Inches(0.4 + (i % 8) * 1.6), Inches(1.0 + (i // 8) * 2.0), Inches(1.4), Inches(1.6))
        sh._element.spPr.insert(2, X(fills[i % 4]))
    s = add_slide(prs, 6)
    label(s, "Lines: widths, dashes, arrowheads, compound lines and connectors")
    for i, (w, dash, head, tail) in enumerate(((1, "solid", "none", "none"), (3, "dash", "triangle", "none"), (6, "sysDot", "none", "stealth"),
                                               (2, "lgDashDot", "oval", "diamond"), (9, "solid", "arrow", "arrow"))):
        ln = s.shapes.add_connector(MSO_CONNECTOR.STRAIGHT, Inches(0.6), Inches(1.2 + i * 0.6), Inches(6), Inches(1.2 + i * 0.6))
        sppr = ln._element.spPr
        sppr.append(X(f'<a:ln w="{w * 12700}"><a:solidFill><a:srgbClr val="1F3A5F"/></a:solidFill><a:prstDash val="{dash}"/>'
                      f'<a:headEnd type="{head}"/><a:tailEnd type="{tail}"/></a:ln>'))
    dbl = s.shapes.add_shape(MSO_SHAPE.RECTANGLE, Inches(7), Inches(1.2), Inches(2.5), Inches(1.6))
    dbl._element.spPr.append(X('<a:ln w="76200" cmpd="dbl"><a:solidFill><a:srgbClr val="C00000"/></a:solidFill></a:ln>'))
    a = s.shapes.add_shape(MSO_SHAPE.OVAL, Inches(7), Inches(4.2), Inches(1.5), Inches(1.2))
    b = s.shapes.add_shape(MSO_SHAPE.RECTANGLE, Inches(10.5), Inches(5.6), Inches(1.8), Inches(1.2))
    for kind in (MSO_CONNECTOR.ELBOW, MSO_CONNECTOR.CURVE):
        c = s.shapes.add_connector(kind, 0, 0, 0, 0)
        c.begin_connect(a, 3 if kind == MSO_CONNECTOR.ELBOW else 2)
        c.end_connect(b, 1 if kind == MSO_CONNECTOR.ELBOW else 0)
    s = add_slide(prs, 6)
    label(s, "Groups: nested, rotated, flipped and scaled")
    outer = s.shapes.add_group_shape()
    outer.shapes.add_shape(MSO_SHAPE.RECTANGLE, Inches(0.5), Inches(1.5), Inches(2), Inches(1.2)).text_frame.text = "Outer"
    inner = outer.shapes.add_group_shape()
    inner.shapes.add_shape(MSO_SHAPE.OVAL, Inches(1.0), Inches(3.0), Inches(1.5), Inches(1.5)).text_frame.text = "Inner"
    inner.shapes.add_shape(MSO_SHAPE.STAR_5_POINT, Inches(2.6), Inches(3.0), Inches(1.5), Inches(1.5))
    rotated = s.shapes.add_group_shape()
    rotated.shapes.add_shape(MSO_SHAPE.RECTANGLE, Inches(5), Inches(1.5), Inches(2.4), Inches(1)).text_frame.text = "Rotated group"
    rotated.shapes.add_shape(MSO_SHAPE.RIGHT_ARROW, Inches(5), Inches(2.7), Inches(2.4), Inches(1))
    rotated._element.grpSpPr.find(qn("a:xfrm")).set("rot", str(20 * 60000))
    flipped = s.shapes.add_group_shape()
    flipped.shapes.add_shape(MSO_SHAPE.RIGHT_ARROW, Inches(8.5), Inches(1.5), Inches(2.4), Inches(1)).text_frame.text = "Flipped"
    flipped.shapes.add_shape(MSO_SHAPE.ISOSCELES_TRIANGLE, Inches(8.5), Inches(2.7), Inches(1.4), Inches(1.2))
    flipped._element.grpSpPr.find(qn("a:xfrm")).set("flipH", "1")
    scaled = s.shapes.add_group_shape()
    scaled.shapes.add_shape(MSO_SHAPE.RECTANGLE, Inches(5), Inches(4.5), Inches(3), Inches(1.5)).text_frame.text = "Scaled to half"
    scaled.shapes.add_shape(MSO_SHAPE.OVAL, Inches(8.2), Inches(4.5), Inches(1.5), Inches(1.5))
    ext = scaled._element.grpSpPr.find(qn("a:xfrm")).find(qn("a:ext"))
    ext.set("cx", str(int(ext.get("cx")) // 2))
    ext.set("cy", str(int(ext.get("cy")) // 2))
    s = add_slide(prs, 6)
    label(s, "Effects: shadows, glow, soft edges, reflection and bevel")
    for i, eff in enumerate((
            '<a:effectLst><a:outerShdw blurRad="63500" dist="76200" dir="2700000" algn="tl"><a:srgbClr val="000000"><a:alpha val="60000"/></a:srgbClr></a:outerShdw></a:effectLst>',
            '<a:effectLst><a:innerShdw blurRad="63500" dist="50800" dir="13500000"><a:srgbClr val="000000"><a:alpha val="50000"/></a:srgbClr></a:innerShdw></a:effectLst>',
            '<a:effectLst><a:glow rad="139700"><a:srgbClr val="FFC000"><a:alpha val="60000"/></a:srgbClr></a:glow></a:effectLst>',
            '<a:effectLst><a:softEdge rad="127000"/></a:effectLst>',
            '<a:effectLst><a:reflection blurRad="6350" stA="50000" endA="300" endPos="55000" dir="5400000" sy="-100000" algn="bl" rotWithShape="0"/></a:effectLst>',
            '<a:scene3d><a:camera prst="orthographicFront"/><a:lightRig rig="threePt" dir="t"/></a:scene3d><a:sp3d><a:bevelT w="152400" h="50800"/></a:sp3d>')):
        sh = s.shapes.add_shape(MSO_SHAPE.ROUNDED_RECTANGLE, Inches(0.5 + (i % 3) * 4.2), Inches(1.3 + (i // 3) * 3), Inches(3.4), Inches(2.2))
        sh.text_frame.text = ("Outer shadow", "Inner shadow", "Glow", "Soft edges", "Reflection", "Bevel")[i]
        append_all(sh._element.spPr, eff)
    s = add_slide(prs, 6)
    label(s, "Custom geometry (freeform) and adjusted preset shapes")
    s.shapes._spTree.append(X(
        '<p:sp><p:nvSpPr><p:cNvPr id="50" name="Freeform 1"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr><a:xfrm><a:off x="914400" y="1371600"/>'
        '<a:ext cx="3657600" cy="3200400"/></a:xfrm><a:custGeom><a:avLst/><a:gdLst/><a:ahLst/><a:cxnLst/><a:rect l="0" t="0" r="r" b="b"/>'
        '<a:pathLst><a:path w="400" h="350"><a:moveTo><a:pt x="0" y="350"/></a:moveTo><a:lnTo><a:pt x="80" y="60"/></a:lnTo>'
        '<a:cubicBezTo><a:pt x="140" y="-40"/><a:pt x="260" y="-40"/><a:pt x="320" y="60"/></a:cubicBezTo><a:lnTo><a:pt x="400" y="350"/></a:lnTo>'
        '<a:arcTo wR="200" hR="80" stAng="0" swAng="10800000"/><a:close/></a:path></a:pathLst></a:custGeom><a:solidFill><a:srgbClr val="27AE60"/>'
        '</a:solidFill><a:ln w="28575"><a:solidFill><a:srgbClr val="145A32"/></a:solidFill></a:ln></p:spPr><p:txBody><a:bodyPr/><a:lstStyle/>'
        '<a:p><a:r><a:rPr lang="en-US"/><a:t>Freeform</a:t></a:r></a:p></p:txBody></p:sp>'))
    for i, adj in enumerate(("adj 16667", "adj 50000", "adj 5000")):
        sh = s.shapes.add_shape(MSO_SHAPE.ROUNDED_RECTANGLE, Inches(6 + i * 2.3), Inches(2), Inches(2), Inches(2))
        sh.adjustments[0] = int(adj.split()[1]) / 100000
        sh.text_frame.text = adj
    return finish(prs)


def deck_backgrounds():
    prs = new_deck("Backgrounds", wide=True)
    # The master gets a soft gradient; one layout overrides it with its own color.
    set_background(prs.slide_master, '<p:bg><p:bgPr><a:gradFill rotWithShape="1"><a:gsLst><a:gs pos="0"><a:srgbClr val="FFFFFF"/></a:gs>'
                                     '<a:gs pos="100000"><a:srgbClr val="DDE7F3"/></a:gs></a:gsLst><a:lin ang="5400000" scaled="0"/>'
                                     '</a:gradFill><a:effectLst/></p:bgPr></p:bg>')
    set_background(prs.slide_layouts[5], '<p:bg><p:bgPr><a:solidFill><a:srgbClr val="FDF2E9"/></a:solidFill><a:effectLst/></p:bgPr></p:bg>')
    master_shape(prs, f'<p:sp><p:nvSpPr><p:cNvPr id="90" name="Master band"/><p:cNvSpPr/><p:nvPr userDrawn="1"/></p:nvSpPr>'
                      f'<p:spPr><a:xfrm><a:off x="0" y="{Inches(7.1)}"/><a:ext cx="{prs.slide_width}" cy="{Inches(0.4)}"/></a:xfrm>'
                      '<a:prstGeom prst="rect"><a:avLst/></a:prstGeom><a:solidFill><a:srgbClr val="1F3A5F"/></a:solidFill>'
                      '<a:ln><a:noFill/></a:ln></p:spPr></p:sp>')
    add_slide(prs, 1, "From the master: a gradient and a band").placeholders[1].text = "Nothing set on this slide"
    add_slide(prs, 5, "From the layout: its own color")
    s = add_slide(prs, 5, "Solid color on the slide")
    set_background(s, '<p:bg><p:bgPr><a:solidFill><a:srgbClr val="E8F6EF"/></a:solidFill><a:effectLst/></p:bgPr></p:bg>')
    s = add_slide(prs, 5, "Radial gradient on the slide")
    set_background(s, '<p:bg><p:bgPr><a:gradFill><a:gsLst><a:gs pos="0"><a:srgbClr val="FFF2CC"/></a:gs><a:gs pos="60000">'
                      '<a:srgbClr val="F4B183"/></a:gs><a:gs pos="100000"><a:srgbClr val="C55A11"/></a:gs></a:gsLst><a:path path="circle">'
                      '<a:fillToRect l="50000" t="50000" r="50000" b="50000"/></a:path></a:gradFill><a:effectLst/></p:bgPr></p:bg>')
    s = add_slide(prs, 5, "A picture, stretched")
    rid = image_rid(s, jpeg(1280, 720, seed=11))
    set_background(s, f'<p:bg><p:bgPr><a:blipFill dpi="0" rotWithShape="1"><a:blip r:embed="{rid}"/><a:srcRect/><a:stretch><a:fillRect/>'
                      '</a:stretch></a:blipFill><a:effectLst/></p:bgPr></p:bg>')
    s = add_slide(prs, 5, "A picture, tiled, and the master's band hidden")
    rid = image_rid(s, png(64, 64, seed=12))
    set_background(s, f'<p:bg><p:bgPr><a:blipFill dpi="0" rotWithShape="1"><a:blip r:embed="{rid}"/><a:srcRect/><a:tile tx="0" ty="0" '
                      'sx="100000" sy="100000" flip="none" algn="tl"/></a:blipFill><a:effectLst/></p:bgPr></p:bg>')
    s._element.set("showMasterSp", "0")
    s = add_slide(prs, 5, "A pattern")
    set_background(s, '<p:bg><p:bgPr><a:pattFill prst="smGrid"><a:fgClr><a:srgbClr val="BDD7EE"/></a:fgClr><a:bgClr><a:srgbClr val="FFFFFF"/>'
                      '</a:bgClr></a:pattFill><a:effectLst/></p:bgPr></p:bg>')
    s = add_slide(prs, 5, "A theme background style (bgRef 1003)")
    set_background(s, '<p:bg><p:bgRef idx="1003"><a:schemeClr val="bg2"/></p:bgRef></p:bg>')
    return finish(prs)


def deck_notes():
    prs = new_deck("Speaker notes")
    for i, notes in enumerate((
            ("Welcome everyone.", "Introduce yourself and the agenda."),
            ("This slide has a long note that runs over several lines, as speakers' notes often do. " * 4,),
            ("Bold and bullets in the notes:", "* first point", "* second point"),
            ())):
        s = add_slide(prs, 1, f"Slide {i + 1}: notes")
        s.placeholders[1].text = "See the notes below" if notes else "This slide has no notes"
        if not notes:
            continue
        tf = s.notes_slide.notes_text_frame
        tf.text = notes[0]
        tf.paragraphs[0].runs[0].font.bold = i == 2
        for line in notes[1:]:
            p = tf.add_paragraph()
            p.text = line.lstrip("* ")
            if line.startswith("*"):
                ppr = p._p.get_or_add_pPr()
                ppr.set("marL", "285750")
                ppr.set("indent", "-285750")
                ppr.append(X('<a:buFont typeface="Arial"/>'))
                ppr.append(X('<a:buChar char="&#8226;"/>'))
    return finish(prs)


TRANSITIONS = (
    ("Fade", '<p:transition spd="med"><p:fade/></p:transition>'),
    ("Fade through black", '<p:transition spd="slow"><p:fade thruBlk="1"/></p:transition>'),
    ("Push up", '<p:transition spd="med"><p:push dir="u"/></p:transition>'),
    ("Wipe, direction r", '<p:transition spd="med"><p:wipe dir="r"/></p:transition>'),
    ("Split vertical out", '<p:transition spd="med"><p:split orient="vert" dir="out"/></p:transition>'),
    ("Cover, direction l", '<p:transition spd="med"><p:cover dir="l"/></p:transition>'),
    ("Uncover (pull), direction l", '<p:transition spd="med"><p:pull dir="l"/></p:transition>'),
    ("Zoom", '<p:transition spd="med"><p:zoom/></p:transition>'),
    ("Dissolve (plays as a fade)", '<p:transition spd="med"><p:dissolve/></p:transition>'),
    ("Random bars (plays as a fade)", '<p:transition spd="fast"><p:randomBar dir="vert"/></p:transition>'),
    ("Vortex, PowerPoint 2010 (plays as a fade)",
     '<mc:AlternateContent xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006"><mc:Choice '
     f'xmlns:p14="{P14}" Requires="p14"><p:transition spd="slow" p14:dur="2000"><p14:vortex dir="r"/></p:transition>'
     '</mc:Choice><mc:Fallback><p:transition spd="slow"><p:fade/></p:transition></mc:Fallback></mc:AlternateContent>'),
    ("Morph (plays as a fade)",
     '<mc:AlternateContent xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006"><mc:Choice '
     'xmlns:p159="http://schemas.microsoft.com/office/powerpoint/2015/09/main" Requires="p159"><p:transition spd="slow" '
     f'xmlns:p14="{P14}" p14:dur="2000"><p159:morph option="byObject"/></p:transition></mc:Choice><mc:Fallback>'
     '<p:transition spd="slow"><p:fade/></p:transition></mc:Fallback></mc:AlternateContent>'),
    ("Push, 1.5 s, and on to the next after 2 s",
     '<mc:AlternateContent xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006"><mc:Choice '
     f'xmlns:p14="{P14}" Requires="p14"><p:transition spd="slow" p14:dur="1500" advTm="2000"><p:push dir="l"/></p:transition>'
     '</mc:Choice><mc:Fallback><p:transition spd="slow" advTm="2000"><p:push dir="l"/></p:transition></mc:Fallback></mc:AlternateContent>'),
    ("No transition", ""),
)


def deck_transitions():
    prs = new_deck("Transitions", wide=True)
    colors = ("FDEDEC", "EBF5FB", "E9F7EF", "FEF9E7", "F4ECF7", "E8F8F5", "FBEEE6")
    for i, (name, xml) in enumerate(TRANSITIONS):
        s = add_slide(prs, 5, f"{i + 1}. {name}")
        set_background(s, f'<p:bg><p:bgPr><a:solidFill><a:srgbClr val="{colors[i % len(colors)]}"/></a:solidFill><a:effectLst/></p:bgPr></p:bg>')
        sh = s.shapes.add_shape(MSO_SHAPE.OVAL, Inches(4.5 + (i % 4) * 0.8), Inches(2.5), Inches(3), Inches(3))
        sh.text_frame.text = str(i + 1)
        if xml:
            slide_append(s, X(xml) if not xml.startswith("<mc:") else parse_xml(xml.replace("<mc:AlternateContent ", f"<mc:AlternateContent {NS} ", 1)))
    return finish(prs)


def deck_animations():
    prs = new_deck("Animations", wide=True)
    s = add_slide(prs, 5, "Entrances on click: appear, fade, fly in, wipe, zoom, float, split")
    ids = []
    for i, kind in enumerate(("appear", "fade", "fly", "wipe", "zoom", "float", "split")):
        sh = s.shapes.add_shape(MSO_SHAPE.ROUNDED_RECTANGLE, Inches(0.4 + i * 1.8), Inches(3), Inches(1.6), Inches(1.6))
        sh.text_frame.text = kind
        ids.append((kind, sh.shape_id))
    animate(s, [("click", k, i) for k, i in ids])
    s = add_slide(prs, 5, "Emphasis and exit: grow, spin, fade out; a motion path")
    a = s.shapes.add_shape(MSO_SHAPE.STAR_5_POINT, Inches(1), Inches(2.5), Inches(2), Inches(2))
    b = s.shapes.add_shape(MSO_SHAPE.RIGHT_ARROW, Inches(4.5), Inches(2.5), Inches(2.5), Inches(1.5))
    c = s.shapes.add_shape(MSO_SHAPE.OVAL, Inches(8), Inches(2.5), Inches(2), Inches(2))
    d = s.shapes.add_shape(MSO_SHAPE.RECTANGLE, Inches(1), Inches(5.2), Inches(1.5), Inches(1))
    for sh, t in ((a, "grow"), (b, "spin"), (c, "fade out"), (d, "path")):
        sh.text_frame.text = t
    animate(s, [("click", "grow", a.shape_id), ("click", "spin", b.shape_id), ("click", "fadeout", c.shape_id),
                ("click", "path", d.shape_id)])
    s = add_slide(prs, 5, "With and after the previous; the first starts by itself")
    shapes = []
    for i in range(5):
        sh = s.shapes.add_shape(MSO_SHAPE.OVAL, Inches(0.8 + i * 2.4), Inches(3), Inches(1.8), Inches(1.8))
        sh.text_frame.text = ("after (auto)", "with", "after", "click", "with")[i]
        shapes.append(sh.shape_id)
    animate(s, [("after", "fade", shapes[0]), ("with", "zoom", shapes[1]), ("after", "fly", shapes[2]),
                ("click", "wipe", shapes[3]), ("with", "split", shapes[4])])
    s = add_slide(prs, 1, "Bullets one by one")
    body = s.placeholders[1]
    tf = body.text_frame
    tf.text = "First point"
    for t in ("Second point", "Third point", "Fourth point"):
        tf.add_paragraph().text = t
    animate(s, [("click", "fly", body.shape_id, i) for i in range(4)])
    s = add_slide(prs, 5, "Other effects play as appear; a trigger plays on a click on its shape")
    w = s.shapes.add_shape(MSO_SHAPE.OVAL, Inches(1), Inches(2.5), Inches(2.5), Inches(2.5))
    w.text_frame.text = "wheel"
    cb = s.shapes.add_shape(MSO_SHAPE.RECTANGLE, Inches(4.5), Inches(2.5), Inches(2.5), Inches(2.5))
    cb.text_frame.text = "checkerboard"
    button = s.shapes.add_shape(MSO_SHAPE.ROUNDED_RECTANGLE, Inches(8.5), Inches(5.5), Inches(3), Inches(1))
    button.text_frame.text = "Click me"
    shown = s.shapes.add_shape(MSO_SHAPE.CLOUD, Inches(8.5), Inches(2), Inches(3), Inches(2.5))
    shown.text_frame.text = "Shown by the trigger"
    animate(s, [("click", "wheel", w.shape_id), ("click", "checkerboard", cb.shape_id)],
            triggers=[(button.shape_id, [("click", "fade", shown.shape_id)])])
    s = add_slide(prs, 5, "Animated pictures, charts and tables")
    pic = s.shapes.add_picture(io.BytesIO(jpeg(640, 480, seed=13)), Inches(0.5), Inches(1.8), Inches(4))
    data = CategoryChartData()
    data.categories = ["A", "B", "C"]
    data.add_series("S", (1, 3, 2))
    chart = s.shapes.add_chart(XL_CHART_TYPE.COLUMN_CLUSTERED, Inches(4.8), Inches(1.8), Inches(4), Inches(3.5), data)
    table = s.shapes.add_table(3, 2, Inches(9.2), Inches(1.8), Inches(3.5), Inches(1.5))
    animate(s, [("click", "fade", pic.shape_id), ("click", "wipe", chart.shape_id), ("click", "zoom", table.shape_id)])
    return finish(prs)


def deck_hyperlinks():
    prs = new_deck("Hyperlinks and actions", wide=True)
    index = add_slide(prs, 5, "Links: web, mail, slides and show actions")
    targets = [add_slide(prs, 5, f"Target slide {i + 2}") for i in range(3)]
    tb = index.shapes.add_textbox(Inches(0.6), Inches(1.6), Inches(7), Inches(4.5))
    tf = tb.text_frame
    p = tf.paragraphs[0]
    r = p.add_run()
    r.text = "A web link (https)"
    r.hyperlink.address = "https://example.com/featherweight"
    p = tf.add_paragraph()
    r = p.add_run()
    r.text = "A mail link"
    r.hyperlink.address = "mailto:someone@example.com?subject=Featherweight"
    for i, t in enumerate(targets):
        p = tf.add_paragraph()
        r = p.add_run()
        r.text = f"Jump to slide {i + 2}"
        rid = index.part.relate_to(t.part, RT.SLIDE)
        r._r.get_or_add_rPr().append(X(f'<a:hlinkClick r:id="{rid}" action="ppaction://hlinksldjump" tooltip="Slide {i + 2}"/>'))
    actions = (("Next slide", "nextslide"), ("Previous slide", "previousslide"), ("First slide", "firstslide"),
               ("Last slide", "lastslide"), ("End show", "endshow"))
    for i, (text, jump) in enumerate(actions):
        btn = index.shapes.add_shape(MSO_SHAPE.ROUNDED_RECTANGLE, Inches(8.4), Inches(1.4 + i * 1.05), Inches(3.6), Inches(0.85))
        btn.text_frame.text = text
        btn._element.nvSpPr.cNvPr.append(X(f'<a:hlinkClick r:id="" action="ppaction://hlinkshowjump?jump={jump}"/>'))
    shape_link = index.shapes.add_shape(MSO_SHAPE.OVAL, Inches(0.6), Inches(6.0), Inches(3), Inches(0.9))
    shape_link.text_frame.text = "A shape linked to the web"
    shape_link.click_action.hyperlink.address = "https://example.org/"
    hover = index.shapes.add_shape(MSO_SHAPE.OVAL, Inches(4), Inches(6.0), Inches(3.6), Inches(0.9))
    hover.text_frame.text = "Mouse over: slide 3"
    rid = index.part.relate_to(targets[1].part, RT.SLIDE)
    hover._element.nvSpPr.cNvPr.append(X(f'<a:hlinkHover r:id="{rid}" action="ppaction://hlinksldjump"/>'))
    for t in targets:
        back = t.shapes.add_shape(MSO_SHAPE.ACTION_BUTTON_HOME, Inches(11.5), Inches(6.2), Inches(1), Inches(0.8))
        back.click_action.target_slide = index
        pic = t.shapes.add_picture(io.BytesIO(png(120, 80, seed=7)), Inches(1), Inches(2), Inches(3))
        pic.click_action.hyperlink.address = "https://example.net/picture"
    return finish(prs)


def deck_embedded_fonts():
    prs = new_deck("Embedded fonts", wide=True)
    ttf, font = pixel_font()
    part = Part(PackURI("/ppt/fonts/font1.fntdata"), "application/x-fontdata", prs.part.package, eot(ttf, font))
    rid = prs.part.relate_to(part, RT.FONT)
    pres = prs.part._element
    pres.set("embedTrueTypeFonts", "1")
    pres.find(qn("p:notesSz")).addnext(X(
        f'<p:embeddedFontLst><p:embeddedFont><p:font typeface="{PIXEL_FAMILY}" pitchFamily="2" charset="0"/>'
        f'<p:regular r:id="{rid}"/></p:embeddedFont></p:embeddedFontLst>'))
    for title, size in (("THE FW PIXEL FONT IS EMBEDDED IN THIS DECK.", 40), ("Lower case is drawn as upper case: 0123456789!", 28)):
        s = add_slide(prs, 6)
        tb = s.shapes.add_textbox(Inches(0.6), Inches(1.5), Inches(12), Inches(4))
        tb.text_frame.word_wrap = True
        tb.text_frame.text = title
        run = tb.text_frame.paragraphs[0].runs[0]
        run.font.name = PIXEL_FAMILY
        run.font.size = Pt(size)
        p = tb.text_frame.add_paragraph()
        p.text = "This line is in the theme font, for comparison."
    return finish(prs)


def deck_hidden_slides():
    prs = new_deck("Hidden slides")
    prs.slide_width, prs.slide_height = Emu(6858000), Emu(9144000)  # portrait, 7.5 x 10 in
    for i in range(5):
        s = add_slide(prs, 6)
        label(s, f"Slide {i + 1}" + (" (hidden)" if i in (1, 3) else ""), width=6.5, height=1, size=36)
        label(s, "Hidden: the show skips it" if i in (1, 3) else "Shown", top=2, width=6.5)
        if i in (1, 3):
            s._element.set("show", "0")
    return finish(prs)


def deck_sections_custom_shows():
    prs = new_deck("Sections and custom shows", wide=True)
    slides = [add_slide(prs, 5, f"Slide {i + 1}") for i in range(6)]
    sld_ids = [e for e in prs.slides._sldIdLst]
    sections = (("Introduction", "{4F3A2B1C-0D9E-4F8A-9B7C-6D5E4F3A2B10}", sld_ids[0:2]),
                ("Main part", "{5A4B3C2D-1E0F-4A9B-8C7D-6E5F4A3B2C21}", sld_ids[2:5]),
                ("Empty section", "{6B5C4D3E-2F1A-4B0C-9D8E-7F6A5B4C3D32}", []),
                ("Closing", "{7C6D5E4F-3A2B-4C1D-8E9F-0A7B6C5D4E43}", sld_ids[5:]))
    xml = "".join(f'<p14:section name="{n}" id="{g}"><p14:sldIdLst>' + "".join(f'<p14:sldId id="{e.get("id")}"/>' for e in ids)
                  + "</p14:sldIdLst></p14:section>" for n, g, ids in sections)
    pres = prs.part._element
    ext = pres.find(qn("p:extLst"))
    if ext is None:
        ext = X("<p:extLst/>")
        pres.append(ext)
    ext.append(parse_xml(f'<p:ext {NS} uri="{{521415D9-36F7-43E2-AB2F-B90AF26B5E84}}"><p14:sectionLst xmlns:p14="{P14}">{xml}'
                         '</p14:sectionLst></p:ext>'))
    rid = lambda i: sld_ids[i].get(qn("r:id"))  # noqa: E731
    pres.find(qn("p:notesSz")).addnext(X(
        f'<p:custShowLst><p:custShow name="Short version" id="0"><p:sldLst><p:sld r:id="{rid(0)}"/><p:sld r:id="{rid(3)}"/>'
        f'<p:sld r:id="{rid(5)}"/></p:sldLst></p:custShow></p:custShowLst>'))
    btn = slides[0].shapes.add_shape(MSO_SHAPE.ROUNDED_RECTANGLE, Inches(8), Inches(5.5), Inches(4), Inches(1))
    btn.text_frame.text = "Play the custom show"
    btn._element.nvSpPr.cNvPr.append(X('<a:hlinkClick r:id="" action="ppaction://customshow?id=0&amp;return=true"/>'))
    return finish(prs)


def deck_show(ext):
    prs = new_deck("A slide show (.ppsx)" if ext == ".ppsx" else "A template (.potx)", wide=True)
    s = add_slide(prs, 0, "Opens straight into the show" if ext == ".ppsx" else "A template opens as a new presentation")
    s.placeholders[1].text = "Esc leaves the show" if ext == ".ppsx" else "With its layouts and theme"
    slide_append(s, X('<p:transition spd="med"><p:fade/></p:transition>'))
    s = add_slide(prs, 1, "Second slide")
    s.placeholders[1].text = "Bullets fly in"
    s.placeholders[1].text_frame.add_paragraph().text = "One by one"
    slide_append(s, X('<p:transition spd="med"><p:push dir="u"/></p:transition>'))
    animate(s, [("click", "fly", s.placeholders[1].shape_id, 0), ("click", "fly", s.placeholders[1].shape_id, 1)])
    return finish(prs, ext)


DECKS = (
    ("layouts-placeholders.pptx", deck_layouts_placeholders),
    ("themes.pptx", deck_themes),
    ("text-autofit.pptx", deck_text_autofit),
    ("bullets.pptx", deck_bullets),
    ("tables.pptx", deck_tables),
    ("charts.pptx", deck_charts),
    ("pictures.pptx", deck_pictures),
    ("shapes-groups.pptx", deck_shapes_groups),
    ("backgrounds.pptx", deck_backgrounds),
    ("notes.pptx", deck_notes),
    ("transitions.pptx", deck_transitions),
    ("animations.pptx", deck_animations),
    ("hyperlinks.pptx", deck_hyperlinks),
    ("embedded-fonts.pptx", deck_embedded_fonts),
    ("hidden-slides.pptx", deck_hidden_slides),
    ("sections-custom-shows.pptx", deck_sections_custom_shows),
    ("show.ppsx", lambda: deck_show(".ppsx")),
    ("template.potx", lambda: deck_show(".potx")),
)


def main():
    only = set(sys.argv[1:])
    print("Presentations:")
    for name, make in DECKS:
        if not only or name in only:
            save(name, make())


if __name__ == "__main__":
    main()
