"""Generates the OCR scan set for the 4.1 gate's "PDF tools / OCR" row (tests\\pdf\\ocr-accuracy.ps1).

English prose from the repository itself (README.md, IMPLEMENTATION_PLAN.md, docs\\*.md, so nothing
copyrighted), drawn at 300 dpi in Windows' fonts in four layouts (one column, two columns, headings
with paragraphs, a ruled table), then made to look like a typical office scan: off-white paper that
isn't quite even, a slight skew, a little blur and noise, JPEG in the PDF.

Writes into DIR:
  scan-NN.pdf       image-only PDFs of 1-4 pages (JPEG pages, 300 dpi)
  scan-NN-pK.png    each page losslessly: the same pixels as the PDF's page before JPEG
  truth.json        the words drawn on each page, in reading order (column by column, a table
                    row by row), split on whitespace exactly as drawn

Run:   python -I tests\\pdf\\make-scan-set.py --out DIR [--seed N] [--pages 24] [--docs 10]
Needs: Pillow and numpy. Deterministic for a seed (same Pillow, numpy and fonts).
"""
import argparse
import datetime
import glob
import hashlib
import html
import json
import os
import random
import re
import sys
import time

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
DPI = 300
PAGE_SIZES = {"A4": (2480, 3508), "Letter": (2550, 3300)}
FONT_DIR = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts")
FAMILIES = [  # name, regular, bold
    ("Arial", "arial.ttf", "arialbd.ttf"),
    ("Times New Roman", "times.ttf", "timesbd.ttf"),
    ("Calibri", "calibri.ttf", "calibrib.ttf"),
    ("Georgia", "georgia.ttf", "georgiab.ttf"),
    ("Verdana", "verdana.ttf", "verdanab.ttf"),
    ("Segoe UI", "segoeui.ttf", "segoeuib.ttf"),
    ("Courier New", "cour.ttf", "courbd.ttf"),
]
LAYOUTS = ["single", "columns", "headings", "table"]


def px(pt):
    return pt * DPI / 72.0


# ---------------------------------------------------------------- text from the repository's Markdown

LETTERS = "A-Za-z\u00c0-\u00d6\u00d8-\u00f6\u00f8-\u00ff"
ALLOWED = set(chr(c) for c in range(0x20, 0x7f)) | set(chr(c) for c in range(0xc0, 0x100) if c not in (0xd7, 0xf7))
ALLOWED |= set("\u2018\u2019\u201c\u201d\u2013\u2014")
WORD = re.compile(
    r"""^[("'\u2018\u201c]*(?:
        [%s]+(?:['\u2019\-][%s]+)*            # words: it's, built-in
      | \d+(?:[.,:\-/]\d+)*%%?                 # 95, 4.0, 1,284, 2026-11-25, 10:30, 95%%
      | \d*(?:1st|2nd|3rd|\dth)               # ordinals
      | [A-Za-z]+\+[A-Za-z]+                  # Ctrl+N
      | \.[A-Za-z]{2,5}                       # .docx
      | %%|&|-|\u2013|\u2014                  # a lone percent sign, ampersand or dash
      | e\.g|i\.e|etc|vs
    )['\u2019"\u201d)]*[.,;:!?]*[)"'\u2019\u201d]*$""" % (LETTERS, LETTERS),
    re.X,
)
HARD_BAD = re.compile(r"[\\_{}=<>|`*#@$^~\[\]]|://")
FILE_NAME = re.compile(r"[A-Za-z0-9]\.[A-Za-z]")
# Identifiers mixing letters and digits (S1, f1-gate, W10, x64, arm64): the plans' jargon, not office prose.
IDENTIFIER = re.compile(r"[A-Za-z]\d|\d[A-Za-z]")
ORDINAL = re.compile(r"\d*(?:1st|2nd|3rd|\dth)")


def strip_inline(s):
    s = re.sub(r"<!--.*?-->", " ", s)
    s = re.sub(r"!\[[^\]]*\]\([^)]*\)", " ", s)              # images
    s = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", s)           # [text](url)
    s = re.sub(r"\[([^\]]+)\]\[[^\]]*\]", r"\1", s)          # [text][ref]
    s = re.sub(r"<https?://[^>]*>", " \x00 ", s)             # autolinks: not prose
    s = re.sub(r"</?[A-Za-z][^>]*>", " ", s)                 # HTML tags
    s = html.unescape(s).replace("\u00a0", " ")

    def code(m):  # a code span that is a plain word stays; anything else marks the sentence as code
        inner = m.group(1).strip()
        return inner if re.fullmatch(r"\.?[A-Za-z][A-Za-z0-9+'-]{0,24}", inner) else " \x00 "

    s = re.sub(r"(`+)(.*?)\1", code, s)
    s = re.sub(r"(\*\*|__)(?=\S)(.+?)(?<=\S)\1", r"\2", s)
    s = re.sub(r"(?<![\w*])\*(?=\S)(.+?)(?<=\S)\*(?![\w*])", r"\1", s)
    s = re.sub(r"(?<![\w])_(?=\S)(.+?)(?<=\S)_(?![\w])", r"\1", s)
    s = re.sub(r"~~(.+?)~~", r"\1", s)
    return re.sub(r"\s+", " ", s).strip()


def plain(sentence, min_words=5):
    """True for a sentence of ordinary prose: no paths, code, symbols or file names."""
    if "\x00" in sentence or any(c not in ALLOWED for c in sentence):
        return False
    toks = sentence.split()
    if len(toks) < min_words or len(toks) > 90:
        return False
    odd = 0
    for t in toks:
        if HARD_BAD.search(t) or len(t) > 28:
            return False
        bare = t.strip("'\"()[]{},.;:!?\u2018\u2019\u201c\u201d")
        if IDENTIFIER.search(bare) and not ORDINAL.fullmatch(bare):
            return False
        if FILE_NAME.search(bare) and not re.fullmatch(r"(e\.g|i\.e|\d+(?:[.,]\d+)+%?|[A-Za-z]\.[A-Za-z]\.?)", bare):
            return False
        if "/" in bare and not re.fullmatch(r"[A-Za-z]+/[A-Za-z]+|\d{1,2}/\d{1,2}/\d{2,4}", bare):
            return False
        if not WORD.match(t):
            odd += 1
    letters = sum(c.isalpha() for c in sentence)
    return odd * 10 <= len(toks) and letters >= 0.6 * len(sentence.replace(" ", ""))


def markdown_blocks(text):
    """Yields ("h", heading) and ("p", paragraph) with Markdown block syntax removed."""
    cur = []
    fence = None

    def flush():
        if cur:
            yield ("p", " ".join(cur))
            cur.clear()

    for line in text.splitlines():
        st = line.strip()
        m = re.match(r"(`{3,}|~{3,})", st)
        if m:
            yield from flush()
            if fence is None:
                fence = m.group(1)[0]
            elif st[0] == fence:
                fence = None
            continue
        if fence is not None:
            continue
        if not st:
            yield from flush()
            continue
        m = re.match(r"#{1,6}\s+(.*?)\s*#*$", st)
        if m:
            yield from flush()
            yield ("h", m.group(1))
            continue
        if re.fullmatch(r"[-*_=\s]{3,}", st):
            yield from flush()
            continue
        if st.startswith("|"):
            yield from flush()
            cells = [c.strip() for c in st.strip("|").split("|")]
            if not all(re.fullmatch(r":?-+:?", c) or not c for c in cells):
                yield ("p", ", ".join(c for c in cells if c))
            continue
        st = re.sub(r"^(>\s*)+", "", st)
        m = re.match(r"([-*+]|\d+[.)])\s+(.*)", st)
        if m:
            yield from flush()
            st = re.sub(r"^\[[ xX]\]\s+", "", m.group(2))
        cur.append(st)
    yield from flush()


def load_prose():
    files = [os.path.join(ROOT, "README.md"), os.path.join(ROOT, "IMPLEMENTATION_PLAN.md")]
    files += sorted(glob.glob(os.path.join(ROOT, "docs", "*.md")))
    paras, heads, seen = [], [], set()
    for f in files:
        if not os.path.isfile(f):
            continue
        with open(f, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        for kind, raw in markdown_blocks(text):
            s = strip_inline(raw)
            if kind == "h":
                s = s.rstrip(":")
                if 2 <= len(s.split()) <= 8 and len(s) <= 56 and plain(s, 2) and s not in seen:
                    seen.add(s)
                    heads.append(s)
                continue
            kept = []
            for sent in re.split(r"(?<=[.!?])\s+(?=[A-Z\"\u201c(])", s):
                sent = sent.strip()
                if plain(sent) and sent not in seen:
                    seen.add(sent)
                    kept.append(sent)
            if kept and sum(len(k.split()) for k in kept) >= 10:
                paras.append(" ".join(kept))
    if len(paras) < 50 or len(heads) < 20:
        sys.exit("make-scan-set: too little prose in README.md, IMPLEMENTATION_PLAN.md and docs\\*.md under " + ROOT)
    return paras, heads


# ---------------------------------------------------------------- drawing pages

class Source:
    """A document's text: paragraphs from the pool in order, and what didn't fit on the last frame."""

    def __init__(self, paras, start):
        self.paras, self.i, self.carry = paras, start, None

    def paragraph(self):
        if self.carry:
            words, self.carry = self.carry, None
            return ("p", words, True)
        p = self.paras[self.i % len(self.paras)]
        self.i += 1
        return ("p", p.split(), False)


class Frame:
    def __init__(self, x0, y0, x1, y1):
        self.x0, self.y0, self.x1, self.y1, self.y = x0, y0, x1, y1, y0

    @property
    def width(self):
        return self.x1 - self.x0


def wrap(words, font, width, indent=0.0):
    lines, cur, avail = [], [], width - indent
    for w in words:
        if cur and font.getlength(" ".join(cur + [w])) > avail:
            lines.append(cur)
            cur, avail = [], width
        cur.append(w)
    if cur:
        lines.append(cur)
    return lines


class Page:
    def __init__(self, size, style):
        self.size, self.st = size, style
        self.img = Image.new("L", size, 255)
        self.draw = ImageDraw.Draw(self.img)
        self.words = []

    def line(self, x, baseline, words, font, width=None, justify=False, align="left"):
        ink = self.st["ink"]
        if justify and len(words) > 1:
            widths = [font.getlength(w) for w in words]
            gap = (width - sum(widths)) / (len(words) - 1)
            if gap <= 2.0 * font.getlength(" "):  # a line that would stretch more stays left-aligned
                xx = x
                for w, wd in zip(words, widths):
                    self.draw.text((xx, baseline), w, font=font, fill=ink, anchor="ls")
                    xx += wd + gap
                self.words += words
                return
        s = " ".join(words)
        if align == "right":
            x = x + width - font.getlength(s)
        elif align == "center":
            x = x + (width - font.getlength(s)) / 2
        self.draw.text((x, baseline), s, font=font, fill=ink, anchor="ls")
        self.words += words

    def heading(self, frame, text, font, lh, space_before, lines_after):
        """Draws a heading if it fits with lines_after body lines below it. False when the frame is full."""
        lines = wrap(text.split(), font, frame.width)
        before = space_before if frame.y > frame.y0 else 0
        need = before + lh * len(lines) + lh * 0.35 + self.st["lh"] * lines_after
        if frame.y + need > frame.y1:
            return False
        frame.y += before
        asc = font.getmetrics()[0]
        for ln in lines:
            self.line(frame.x0, frame.y + asc, ln, font)
            frame.y += lh
        frame.y += lh * 0.35
        return True

    def paragraph(self, frame, words, continued):
        """Draws what fits; returns the words that didn't (None when all did)."""
        st = self.st
        body, lh = st["body"], st["lh"]
        indent = st["indent"] if (st["indent"] and not continued) else 0.0
        lines = wrap(words, body, frame.width, indent)
        fit = int((frame.y1 - frame.y) // lh)
        if fit <= 0:
            return words
        asc = body.getmetrics()[0]
        n = min(fit, len(lines))
        for k in range(n):
            last = k == len(lines) - 1
            x = frame.x0 + (indent if k == 0 else 0.0)
            self.line(x, frame.y + asc, lines[k], body, frame.x1 - x, st["justify"] and not last)
            frame.y += lh
        if n < len(lines):
            return [w for ln in lines[n:] for w in ln]
        frame.y += st["para_gap"]
        return None

    def fill(self, frame, produce, source):
        """Fills a frame from produce() until it is full; a cut paragraph goes back to source.carry."""
        while True:
            item = produce()
            if item[0] == "h":
                if not self.heading(frame, item[1], self.st["sub"], self.st["sub_lh"], self.st["lh"] * 0.8, 2):
                    return
            else:
                rest = self.paragraph(frame, item[1], item[2])
                if rest is not None:
                    source.carry = rest
                    return


def table_rows(rnd, paras, n):
    headers = rnd.choice([("Item", "Count", "Date"), ("Task", "Hours", "Due"), ("Description", "Amount", "Date"),
                          ("Step", "Pages", "Checked"), ("Entry", "Value", "Updated"), ("Activity", "Total", "Recorded")])
    num_kind = rnd.choice(["int", "thousands", "decimal", "percent", "money"])
    date_kind = rnd.choice(["iso", "dmy", "mdy_long", "us", "dots"])
    months = ["January", "February", "March", "April", "May", "June", "July", "August", "September",
              "October", "November", "December"]
    rows = [list(headers)]
    for _ in range(n):
        # A label: two to four plain words from a sentence of the pool.
        for _try in range(50):
            words = [w.strip(",.;:!?()\"'\u2018\u2019\u201c\u201d") for w in rnd.choice(paras).split()]
            k = rnd.randint(2, 4)
            if len(words) <= k:
                continue
            i = rnd.randrange(len(words) - k)
            cand = words[i:i + k]
            if all(re.fullmatch(r"[A-Za-z]+(?:-[A-Za-z]+)?", w) for w in cand):
                break
        label = " ".join(cand)
        label = label[0].upper() + label[1:]
        v = rnd.random()
        if num_kind == "int":
            num = str(rnd.randint(1, 980))
        elif num_kind == "thousands":
            num = "{:,}".format(rnd.randint(1000, 98000))
        elif num_kind == "decimal":
            num = "{:.{}f}".format(v * 400, rnd.choice([1, 2]))
        elif num_kind == "percent":
            num = "{:.1f} %".format(v * 100) if rnd.random() < 0.5 else "{:.1f}%".format(v * 100)
        else:
            num = "${:,.2f}".format(10 + v * 9000)
        d = datetime.date(2026, 1, 1) + datetime.timedelta(days=rnd.randrange(330))
        if date_kind == "iso":
            date = d.isoformat()
        elif date_kind == "dmy":
            date = "%d %s %d" % (d.day, months[d.month - 1][:3], d.year)
        elif date_kind == "mdy_long":
            date = "%s %d, %d" % (months[d.month - 1], d.day, d.year)
        elif date_kind == "us":
            date = "%02d/%02d/%d" % (d.month, d.day, d.year)
        else:
            date = "%02d.%02d.%d" % (d.day, d.month, d.year)
        rows.append([label, num, date])
    return rows


def draw_table(page, frame, rows, rnd):
    st = page.st
    body, bold = st["body"], st["bold_body"]
    s = st["size_px"]
    row_h = round(s * rnd.uniform(1.75, 2.1))
    n = min(len(rows), int((frame.y1 - frame.y) // row_h))
    if n < 4:
        return
    rows = rows[:n]
    w = frame.width
    cols = [0.0, w * 0.52, w * 0.74, w]
    x0, y0 = frame.x0, frame.y
    ink = st["ink"]
    lw = rnd.choice([2, 3])
    if rnd.random() < 0.6:
        page.draw.rectangle([x0, y0, x0 + w, y0 + row_h], fill=228)
    for r in range(n + 1):
        y = y0 + r * row_h
        page.draw.line([(x0, y), (x0 + w, y)], fill=ink, width=lw)
    for c in cols:
        page.draw.line([(x0 + c, y0), (x0 + c, y0 + n * row_h)], fill=ink, width=lw)
    pad = s * 0.45
    asc, desc = body.getmetrics()
    for r, row in enumerate(rows):
        font = bold if r == 0 else body
        base = y0 + r * row_h + (row_h + asc - desc) / 2
        for c, text in enumerate(row):
            align = "right" if (c == 1 and r > 0) else "left"
            page.line(x0 + cols[c] + pad, base, text.split(), font, cols[c + 1] - cols[c] - 2 * pad, align=align)
    frame.y = y0 + n * row_h + st["lh"]


def render_page(size, style, layout, source, heads, paras, rnd, page_no, pages, title):
    page = Page(size, style)
    W, H = size
    mx, mt, mb = style["margins"]
    full = Frame(mx, mt, W - mx, H - mb)
    if page_no == 1:
        page.heading(full, title, style["title"], style["title_lh"], 0, 3)
        full.y += style["lh"] * 0.6

    def paragraphs():
        return source.paragraph()

    if layout == "single":
        page.fill(full, paragraphs, source)
    elif layout == "columns":
        gutter = round(px(rnd.uniform(0.3, 0.42) * 72))
        cw = (full.width - gutter) / 2
        left = Frame(full.x0, full.y, full.x0 + cw, full.y1)
        right = Frame(full.x1 - cw, full.y, full.x1, full.y1)
        page.fill(left, paragraphs, source)
        page.fill(right, paragraphs, source)
    elif layout == "headings":
        state = {"k": 0}

        def headed():
            if source.carry:
                return source.paragraph()
            if state["k"] == 0:
                state["k"] = rnd.randint(1, 3)
                return ("h", rnd.choice(heads))
            state["k"] -= 1
            return source.paragraph()

        page.fill(full, headed, source)
    else:  # table: a heading, a paragraph, the table, then paragraphs
        if source.carry:
            rest = page.paragraph(full, source.carry, True)
            source.carry = rest
        if source.carry is None and page.heading(full, rnd.choice(heads), style["sub"], style["sub_lh"], style["lh"] * 0.8, 3):
            item = source.paragraph()
            rest = page.paragraph(full, item[1], False)
            source.carry = rest
            if rest is None:
                avail = (full.y1 - full.y) * rnd.uniform(0.55, 0.8)
                n = max(4, int(avail // (style["size_px"] * 1.95)) - 1)
                draw_table(page, full, table_rows(rnd, paras, min(n, rnd.randint(10, 22))), rnd)
        page.fill(full, paragraphs, source)

    foot = style["foot"]
    page.line(0, H - mb / 2, ["Page", str(page_no), "of", str(pages)], foot, W, align="center")
    return page


# ---------------------------------------------------------------- the scan look

def scan_look(img, look, nrng):
    """Off-white uneven paper, the skew, dust, blur and sensor noise. Returns an 'L' or 'RGB' image."""
    W, H = img.size
    d = ImageDraw.Draw(img)
    for _ in range(look["dust"]):  # dust specks a fraction of a millimetre wide
        x, y = float(nrng.uniform(0, W)), float(nrng.uniform(0, H))
        r = float(nrng.uniform(0.7, 2.0))
        d.ellipse([x - r, y - r, x + r, y + r], fill=int(nrng.uniform(110, 190)))
    im = img.rotate(look["skew"], resample=Image.BICUBIC, fillcolor=255)
    a = np.asarray(im, dtype=np.float32) / 255.0
    grid = nrng.normal(0.0, 1.0, (11, 8)).astype(np.float32)
    uneven = np.asarray(Image.fromarray(grid).resize((W, H), Image.BICUBIC), dtype=np.float32)
    paper = look["paper"] + look["uneven"] * uneven
    if look["edge"]:  # the shadow of the page's edge on the scanner glass
        k = look["edge_px"]
        ramp = np.linspace(0.55, 1.0, k, dtype=np.float32)
        if look["edge"] == "left":
            paper[:, :k] *= ramp[None, :]
        else:
            paper[:k, :] *= ramp[:, None]
    out = np.clip(paper * a, 0, 255).astype(np.uint8)
    blurred = Image.fromarray(out).filter(ImageFilter.GaussianBlur(look["blur"]))
    g = np.asarray(blurred, dtype=np.float32)
    if look["color"]:
        tint = np.array(look["tint"], dtype=np.float32)
        rgb = g[:, :, None] * tint[None, None, :]
        rgb += nrng.normal(0.0, look["noise"], (H, W, 1)).astype(np.float32)
        rgb += nrng.normal(0.0, look["noise"] * 0.5, (H, W, 3)).astype(np.float32)
        return Image.fromarray(np.clip(rgb + 0.5, 0, 255).astype(np.uint8))
    g = g + nrng.normal(0.0, look["noise"], (H, W)).astype(np.float32)
    return Image.fromarray(np.clip(g + 0.5, 0, 255).astype(np.uint8))


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True)
    ap.add_argument("--seed", type=int, default=4101)
    ap.add_argument("--pages", type=int, default=24)
    ap.add_argument("--docs", type=int, default=10)
    args = ap.parse_args()
    if not (1 <= args.docs <= args.pages <= 4 * args.docs):
        sys.exit("make-scan-set: --pages must be between --docs and 4 x --docs")
    t0 = time.time()
    rnd = random.Random(args.seed)
    nrng = np.random.default_rng(args.seed)
    paras, heads = load_prose()
    families = [f for f in FAMILIES if os.path.isfile(os.path.join(FONT_DIR, f[1]))]
    if not families:
        sys.exit("make-scan-set: none of the fonts are in " + FONT_DIR)

    os.makedirs(args.out, exist_ok=True)
    for old in glob.glob(os.path.join(args.out, "scan-*.pdf")) + glob.glob(os.path.join(args.out, "scan-*.png")):
        os.remove(old)

    counts = [1] * args.docs
    while sum(counts) < args.pages:
        i = rnd.randrange(args.docs)
        if counts[i] < 4:
            counts[i] += 1
    layouts = [LAYOUTS[i % len(LAYOUTS)] for i in range(args.pages)]
    rnd.shuffle(layouts)

    with open(os.path.abspath(__file__), "rb") as fh:  # ocr-accuracy.ps1 makes the set again when this changes
        script_sha = hashlib.sha256(fh.read()).hexdigest()
    truth = {"generator": "tests/pdf/make-scan-set.py", "generatorSha256": script_sha, "seed": args.seed, "dpi": DPI,
             "sources": "README.md, IMPLEMENTATION_PLAN.md, docs/*.md", "documents": []}
    total_words = 0
    li = 0
    for di, count in enumerate(counts, 1):
        # Each family once, then the proportional ones again (a typewriter face is the exception in an office).
        fam = families[di - 1] if di <= len(families) else rnd.choice([f for f in families if f[1] != "cour.ttf"] or families)
        paper = rnd.choice(["A4", "Letter"])
        size = PAGE_SIZES[paper]
        pt = rnd.choice([10, 10.5, 11, 11.5, 12])
        s = px(pt)
        reg = os.path.join(FONT_DIR, fam[1])
        bold = os.path.join(FONT_DIR, fam[2]) if os.path.isfile(os.path.join(FONT_DIR, fam[2])) else reg
        lh = round(s * rnd.uniform(1.2, 1.38))
        title_pt = rnd.choice([16, 18, 20])
        sub_pt = rnd.choice([12.5, 13, 14])
        indent = rnd.random() < 0.35
        style = {
            "font": fam[0], "pt": pt, "size_px": s, "lh": lh,
            "body": ImageFont.truetype(reg, s), "bold_body": ImageFont.truetype(bold, s),
            "title": ImageFont.truetype(bold, px(title_pt)), "title_lh": round(px(title_pt) * 1.25),
            "sub": ImageFont.truetype(bold, px(sub_pt)), "sub_lh": round(px(sub_pt) * 1.25),
            "foot": ImageFont.truetype(reg, px(10)),
            "indent": s * 2.2 if indent else 0.0, "para_gap": 0 if indent else round(lh * rnd.uniform(0.45, 0.8)),
            "justify": rnd.random() < 0.4 and fam[1] != "cour.ttf", "ink": rnd.randint(12, 48),
            "margins": (rnd.randint(230, 300), rnd.randint(250, 330), rnd.randint(300, 360)),
        }
        color = rnd.random() < 0.2
        doc_look = {
            "paper": rnd.uniform(234, 248), "uneven": rnd.uniform(2.0, 5.5), "noise": rnd.uniform(3.0, 6.5),
            "blur": rnd.uniform(0.5, 0.95), "color": color,
            "tint": (1.0, rnd.uniform(0.975, 0.995), rnd.uniform(0.93, 0.975)),
        }
        quality = rnd.randint(75, 88)
        source = Source(paras, rnd.randrange(len(paras)))
        title = rnd.choice(heads)
        name = "scan-%02d" % di
        images, pages = [], []
        for pno in range(1, count + 1):
            layout = layouts[li]
            li += 1
            page = render_page(size, style, layout, source, heads, paras, rnd, pno, count, title)
            look = dict(doc_look, skew=round(rnd.uniform(-0.8, 0.8), 3), dust=rnd.randint(8, 35),
                        edge=rnd.choice([None, None, "left", "top"]), edge_px=rnd.randint(20, 55))
            img = scan_look(page.img, look, nrng)
            png = "%s-p%d.png" % (name, pno)
            img.save(os.path.join(args.out, png), dpi=(DPI, DPI))
            images.append(img)
            pages.append({"png": png, "words": page.words,
                          "look": {"layout": layout, "paper": paper, "font": fam[0], "pt": pt,
                                   "skew": look["skew"], "blur": round(look["blur"], 2),
                                   "noise": round(look["noise"], 2), "color": color, "jpeg": quality}})
            total_words += len(page.words)
        pdf = name + ".pdf"
        fixed = time.strptime("2026-11-25 12:00:00", "%Y-%m-%d %H:%M:%S")  # the same bytes on every run
        images[0].save(os.path.join(args.out, pdf), "PDF", save_all=True, append_images=images[1:], resolution=DPI,
                       quality=quality, title="Scan %d" % di, producer="make-scan-set.py",
                       creationDate=fixed, modDate=fixed)
        truth["documents"].append({"pdf": pdf, "pages": pages})
        print("%s: %d page(s), %s %s pt, %s, JPEG %d, %s" % (pdf, count, fam[0], pt, paper, quality,
                                                             ", ".join(p["look"]["layout"] for p in pages)))

    with open(os.path.join(args.out, "truth.json"), "w", encoding="utf-8", newline="\n") as fh:
        json.dump(truth, fh, ensure_ascii=False, indent=1)
        fh.write("\n")
    print("make-scan-set: %d documents, %d pages, %d words (from %d paragraphs, %d headings) in %.1f s -> %s"
          % (len(counts), sum(counts), total_words, len(paras), len(heads), time.time() - t0, args.out))


if __name__ == "__main__":
    main()
