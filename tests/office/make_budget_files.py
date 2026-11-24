"""Generates S1's opening-budget files (plan section 11, docs/s1-plan.md "Budgets"), which are too
big for the repository:

  million-cells.xlsx   50,000 rows by 20 columns: numbers, text, dates, booleans, a few formats,
                       and a formula column (cached values included, as Excel writes them)
  100mb.csv            about 100 MB of UTF-8 CSV: 1.1 million rows of ids, names, amounts,
                       dates and codes with leading zeros, some fields quoted
  fifty-mb.xlsx        S4's very large workbook (docs/s4-plan.md, "Scale"): the million-cell
                       workbook's rows, 384,000 of them (7.68 million cells), about 50 MB
  deck-50.pptx         P1's opening budget (docs/p1-plan.md): 50 slides as a lecture has them,
                       titles and bullets, 20 photos, 5 charts, 5 tables, groups and shapes, notes
  deck-typical.pptx    P1's memory budget: a typical 20-slide deck (8 photos, 2 charts, a table)
  deck-video.pptx      P3's idle budget: a 1080p H.264 video on the first slide (needs ffmpeg on the
                       PATH to make the clip; skipped without it)

Output goes to the folder given (default: build/budgets, which git ignores):
  .venv\\Scripts\\python tests/office/make_budget_files.py [folder]
  .venv\\Scripts\\python tests/office/make_budget_files.py --decks [folder]   (only the decks)

The output is deterministic (fixed seed, fixed dates), so timings compare across runs.
"""
import os
import random
import sys
from datetime import datetime

import xlsxwriter
from xlsxwriter.utility import xl_col_to_name

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WORDS = ["apple", "pear", "plum", "cherry", "grape", "melon", "lemon", "kiwi", "mango", "peach", "Café", "Straße", "naïve"]


def million_cells(path, rows=50_000, title="One million cells"):
    cols = 20
    rnd = random.Random(1)
    wb = xlsxwriter.Workbook(path, {"constant_memory": True, "strings_to_numbers": False})
    wb.set_properties({"title": title, "author": "Featherweight tests", "created": datetime(2026, 1, 1)})
    ws = wb.add_worksheet("Data")
    bold = wb.add_format({"bold": True})
    money = wb.add_format({"num_format": "#,##0.00"})
    date = wb.add_format({"num_format": "yyyy-mm-dd"})
    pct = wb.add_format({"num_format": "0.0%"})
    ws.freeze_panes(1, 1)
    ws.write_row(0, 0, ["Id", "Name", "Amount", "Date", "Share", "Flag"] + [f"Value {c}" for c in range(7, cols)] + ["Total"], bold)
    for r in range(1, rows):
        a = rnd.random() * 10000
        ws.write_number(r, 0, r)
        ws.write_string(r, 1, f"{rnd.choice(WORDS)} {r % 997}")
        ws.write_number(r, 2, round(a, 2), money)
        ws.write_number(r, 3, 45000 + (r % 3650), date)
        ws.write_number(r, 4, rnd.random(), pct)
        ws.write_boolean(r, 5, r % 3 == 0)
        vals = [rnd.randint(-1000, 1000) for _ in range(6, cols - 1)]
        for c, v in enumerate(vals, start=6):
            ws.write_number(r, c, v)
        # The formula's cached value is written too, as Excel would have it.
        ws.write_formula(r, cols - 1, f"=SUM(G{r + 1}:{xl_col_to_name(cols - 2)}{r + 1})+C{r + 1}", money,
                         sum(vals) + round(a, 2))
    ws.set_column(1, 1, 18)
    ws.set_column(2, 3, 12)
    wb.close()


def big_csv(path, target=100 * 1024 * 1024):
    rnd = random.Random(2)
    size = 0
    n = 0
    with open(path, "w", encoding="utf-8", newline="") as f:
        header = "id,name,amount,date,postcode,code,note\r\n"
        f.write(header)
        size += len(header.encode("utf-8"))
        while size < target:
            n += 1
            name = f"{rnd.choice(WORDS)} {n % 1009}"
            note = '"said ""hello"", then left"' if n % 50 == 0 else ('"multi, part"' if n % 7 == 0 else "plain")
            line = (f"{n},{name},{rnd.random() * 100000:.2f},2025-{1 + n % 12:02d}-{1 + n % 28:02d},"
                    f"{n % 100000:05d},{rnd.randint(0, 10**17):018d},{note}\r\n")
            f.write(line)
            size += len(line.encode("utf-8"))
    return n


def deck(path, slides, photos, charts, tables):
    """A deck as consumers receive them: python-pptx's default theme at 16:9, photos as 1600 x 900
    JPEGs, charts with their workbooks, notes on every slide."""
    import io
    import pptx
    from pptx.chart.data import CategoryChartData
    from pptx.enum.chart import XL_CHART_TYPE
    from pptx.enum.shapes import MSO_SHAPE
    from pptx.util import Emu, Inches
    sys.dont_write_bytecode = True
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from make_presentations import jpeg
    from make_test_office import normalize, set_core, to_bytes
    prs = pptx.Presentation()
    set_core(prs.core_properties, f"{slides}-slide deck")
    prs.slide_width, prs.slide_height = Emu(12192000), Emu(6858000)
    rnd = random.Random(slides)
    pics = [jpeg(1600, 900, seed=i) for i in range(photos)]
    kinds = ["photo"] * photos + ["chart"] * charts + ["table"] * tables
    kinds += ["bullets"] * (slides - 1 - len(kinds))
    rnd.shuffle(kinds)
    title = prs.slides.add_slide(prs.slide_layouts[0])
    title.shapes.title.text = f"A {slides}-slide deck"
    title.placeholders[1].text = "Featherweight's opening budget"
    for i, kind in enumerate(kinds):
        s = prs.slides.add_slide(prs.slide_layouts[5 if kind != "bullets" else 1])
        s.shapes.title.text = f"{i + 2}. " + " ".join(rnd.choice(WORDS) for _ in range(4)).capitalize()
        if kind == "bullets":
            tf = s.placeholders[1].text_frame
            tf.text = " ".join(rnd.choice(WORDS) for _ in range(9))
            for lvl in (0, 1, 1, 0, 1):
                para = tf.add_paragraph()
                para.text = " ".join(rnd.choice(WORDS) for _ in range(rnd.randint(4, 12)))
                para.level = lvl
            grp = s.shapes.add_group_shape()
            for k in range(3):
                grp.shapes.add_shape(MSO_SHAPE.CHEVRON, Inches(9 + k * 1.1), Inches(6.2), Inches(1), Inches(0.6))
        elif kind == "photo":
            s.shapes.add_picture(io.BytesIO(pics.pop()), Inches(1.2), Inches(1.6), Inches(9.5))
        elif kind == "chart":
            data = CategoryChartData()
            data.categories = ["2021", "2022", "2023", "2024", "2025"]
            for name in ("North", "South", "West"):
                data.add_series(name, [round(rnd.uniform(5, 50), 1) for _ in range(5)])
            s.shapes.add_chart(XL_CHART_TYPE.COLUMN_CLUSTERED, Inches(1), Inches(1.6), Inches(11), Inches(5.4), data)
        else:
            t = s.shapes.add_table(8, 5, Inches(1), Inches(1.6), Inches(11), Inches(5)).table
            for r in range(8):
                for c in range(5):
                    t.cell(r, c).text = rnd.choice(WORDS) if c == 0 or r == 0 else f"{rnd.uniform(0, 1000):.1f}"
        s.notes_slide.notes_text_frame.text = " ".join(rnd.choice(WORDS) for _ in range(30))
    with open(path, "wb") as f:
        f.write(normalize(to_bytes(prs)))


def video_deck(path):
    """P3's idle budget (docs/p3-plan.md, "Lightweight"): a deck whose first slide holds a 1080p H.264
    video, embedded as PowerPoint embeds it, with its poster frame. The clip is ffmpeg's test pattern,
    10 s at 30 fps without sound. Without ffmpeg on the PATH there's no such deck (returns False)."""
    import shutil
    import subprocess
    import tempfile
    import pptx
    from pptx.util import Emu, Inches
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        return False
    sys.dont_write_bytecode = True
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from make_test_office import normalize, set_core, to_bytes
    with tempfile.TemporaryDirectory() as tmp:
        clip, poster = os.path.join(tmp, "clip-1080p.mp4"), os.path.join(tmp, "poster.png")
        quiet = ["-hide_banner", "-loglevel", "error", "-y"]
        subprocess.run([ffmpeg, *quiet, "-f", "lavfi", "-i", "testsrc2=size=1920x1080:rate=30:duration=10",
                        "-c:v", "libx264", "-preset", "veryfast", "-pix_fmt", "yuv420p", "-threads", "1",
                        "-movflags", "+faststart", "-an", clip], check=True)
        subprocess.run([ffmpeg, *quiet, "-i", clip, "-frames:v", "1", poster], check=True)
        prs = pptx.Presentation()
        set_core(prs.core_properties, "A deck with a video")
        prs.slide_width, prs.slide_height = Emu(12192000), Emu(6858000)
        s = prs.slides.add_slide(prs.slide_layouts[5])
        s.shapes.title.text = "A 1080p video on the first slide"
        s.shapes.add_movie(clip, Inches(1.33), Inches(1.5), Inches(10.67), Inches(6), poster_frame_image=poster,
                           mime_type="video/mp4")
        for i in range(2, 5):
            s = prs.slides.add_slide(prs.slide_layouts[1])
            s.shapes.title.text = f"{i}. " + " ".join(WORDS[(i + k) % len(WORDS)] for k in range(4)).capitalize()
            s.placeholders[1].text = " ".join(WORDS[(i * k) % len(WORDS)] for k in range(9))
        with open(path, "wb") as f:
            f.write(normalize(to_bytes(prs)))
    return True


def decks(out):
    for name, slides, photos, charts, tables in (("deck-50.pptx", 50, 20, 5, 5), ("deck-typical.pptx", 20, 8, 2, 1)):
        d = os.path.join(out, name)
        deck(d, slides, photos, charts, tables)
        print(f"  {d}  ({os.path.getsize(d):,} bytes, {slides} slides)")
    v = os.path.join(out, "deck-video.pptx")
    if video_deck(v):
        print(f"  {v}  ({os.path.getsize(v):,} bytes, a 1080p H.264 video on slide 1)")
    else:
        print("  deck-video.pptx not made: ffmpeg isn't on the PATH (P3's idle budget with a video)")


def main():
    args = [a for a in sys.argv[1:] if a != "--decks"]
    out = args[0] if args else os.path.join(ROOT, "build", "budgets")
    os.makedirs(out, exist_ok=True)
    if "--decks" in sys.argv:  # only the decks (scripts/budgets.ps1 makes them for its timings)
        decks(out)
        return
    x = os.path.join(out, "million-cells.xlsx")
    million_cells(x)
    print(f"  {x}  ({os.path.getsize(x):,} bytes, 1,000,000 cells)")
    c = os.path.join(out, "100mb.csv")
    rows = big_csv(c)
    print(f"  {c}  ({os.path.getsize(c):,} bytes, {rows:,} rows)")
    f = os.path.join(out, "fifty-mb.xlsx")
    million_cells(f, rows=384_000, title="About 50 MB")
    print(f"  {f}  ({os.path.getsize(f):,} bytes, 7,680,000 cells)")
    decks(out)


if __name__ == "__main__":
    main()
