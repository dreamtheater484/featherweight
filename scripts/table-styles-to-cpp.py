# Turns scripts/excel-table-styles.py's JSON into src/sheet/model/TableStylesData.inc: Excel's
# built-in table styles as a compact list of (style, element, part, theme color, tint).
#
#   python scripts/table-styles-to-cpp.py styles.json src/sheet/model/TableStylesData.inc
import json, re, sys

# In the order Excel applies them (TableElement in src/sheet/model/Styles.h).
ELEMENTS = ["wholeTable", "firstColumnStripe", "secondColumnStripe", "firstRowStripe", "secondRowStripe", "lastColumn",
            "firstColumn", "headerRow", "totalRow", "firstHeaderCell", "lastHeaderCell", "firstTotalCell", "lastTotalCell"]
SIDES = ["left", "top", "bottom", "right", "vertical", "horizontal"]
# Parts: 0 fill, 1 font color, 2 bold, 3 italic, 4 stripe size (in `theme`), 8-13 borders by SIDES.


def style_index(name):
    m = re.fullmatch(r"TableStyle(Light|Medium|Dark)(\d+)", name)
    base = {"Light": 0, "Medium": 21, "Dark": 49}[m.group(1)]
    return base + int(m.group(2)) - 1


def border_style(b):
    # BorderStyle in Styles.h: Thin 1, Medium 2, Thick 5, Double 6.
    if b["style"] == -4119:
        return 6
    return {2: 1, -4138: 2, 4: 5, 1: 7}.get(b["weight"], 1)  # 1: xlHairline -> Hair 7


def theme_of(c):
    if "theme" in c:
        return c["theme"] - 1, c.get("tint", 0.0)  # Excel's 1 = lt1 -> SpreadsheetML's 0
    return None


def main(src, out):
    styles = json.load(open(src, encoding="utf-8"))
    rows, skipped = [], []
    for name, elements in sorted(styles.items(), key=lambda kv: style_index(kv[0])):
        s = style_index(name)
        for ename, e in elements.items():
            el = ELEMENTS.index(ename)
            if "size" in e and e["size"] != 1:
                rows.append((s, el, 4, e["size"], 0.0))
            if "fill" in e:
                t = theme_of(e["fill"])
                if t:
                    rows.append((s, el, 0, t[0], t[1]))
                elif e["fill"].get("rgb", 0) != 0:
                    skipped.append((name, ename, "fill", e["fill"]))
            if "font" in e:
                t = theme_of(e["font"])
                if t and not (ename == "wholeTable" and t == (1, 0.0)):  # dk1: the text's own color
                    rows.append((s, el, 1, t[0], t[1]))
            if e.get("bold"):
                rows.append((s, el, 2, 0, 0.0))
            if e.get("italic"):
                rows.append((s, el, 3, 0, 0.0))
            for side, b in e.get("borders", {}).items():
                t = theme_of(b)
                if not t:
                    skipped.append((name, ename, side, b))
                    continue
                rows.append((s, el, (8 + SIDES.index(side)) | (border_style(b) << 4), t[0], t[1]))
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("// Excel's built-in table styles (TableStyleLight1-21, Medium1-28, Dark1-11), read from Excel 2021\n")
        f.write("// by scripts/excel-table-styles.py and written by scripts/table-styles-to-cpp.py. Do not edit.\n")
        f.write("// {style, element (TableElement), part, theme color or stripe size, tint}; part: 0 fill,\n")
        f.write("// 1 font color, 2 bold, 3 italic, 4 stripe size, 8-13 the border on a side (left, top, bottom,\n")
        f.write("// right, vertical, horizontal) with its BorderStyle in the high four bits.\n")
        for r in rows:
            f.write("{%d, %d, %d, %d, %sf},\n" % (r[0], r[1], r[2], r[3], repr(round(r[4], 6))))
    print(len(rows), "entries;", len(skipped), "skipped", skipped[:5])


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
