# Asks Excel how table styles format cells, for tests/unit/test_sheet_model.cpp: makes a workbook of
# tables in custom styles (every element filled, or every element bordered, in colors that tell
# them apart) and in built-in styles, with their options on and off, and writes what Excel shows in
# each cell (Range.DisplayFormat) next to it.
#
#   python tests/sheet/excel_table_styles.py tests/sheet/table-styles.xlsx tests/unit/sheet_table_styles_table.inc
#   python tests/sheet/excel_table_styles.py out.xlsx out.inc --all   (one custom style per element too)
#
# Excel runs in a new, separate instance with alerts, events and macros off; only that instance is
# ended.
import os, sys
import pythoncom, win32api, win32con, win32event, win32process
import win32com.client

from excel_common import scrub_author

ELEMENTS = ["wholeTable", "headerRow", "totalRow", "firstColumn", "lastColumn", "firstRowStripe", "secondRowStripe",
            "firstColumnStripe", "secondColumnStripe", "firstHeaderCell", "lastHeaderCell", "firstTotalCell", "lastTotalCell"]
BORDERS = [7, 8, 9, 10, 11, 12]  # xlEdgeLeft, Top, Bottom, Right, xlInsideVertical, Horizontal
CONFIGS = {  # headers, totals, first column, last column, row stripes, column stripes
    "all": (True, True, True, True, True, True),
    "bare": (True, True, False, False, False, False),
    "noheader": (False, False, True, True, True, True),
    "default": (True, False, False, False, True, False),
}
BUILT_IN = ["TableStyleMedium2", "TableStyleLight1", "TableStyleLight9", "TableStyleLight15", "TableStyleMedium9",
            "TableStyleMedium15", "TableStyleDark1", "TableStyleDark8", "TableStyleLight21", "TableStyleMedium28"]


def code(e, s):  # a color telling element e and side s (6: the fill) apart, as Excel's BGR
    return ((e + 1) * 16) | (((s + 1) * 16) << 8) | (0x80 << 16)


def rgb(bgr):
    bgr = int(bgr)
    return ((bgr & 0xFF) << 16) | (bgr & 0xFF00) | ((bgr >> 16) & 0xFF)


def border_style(line, weight):  # BorderStyle in src/sheet/model/Styles.h
    if line == -4119:
        return 6
    return {2: 1, -4138: 2, 4: 5, 1: 7}.get(weight, 1)


def main(out_xlsx, out_inc, everything):
    pythoncom.CoInitialize()
    xl = win32com.client.DispatchEx("Excel.Application")
    pid = None
    rows = []
    try:
        try:
            _, pid = win32process.GetWindowThreadProcessId(xl.Hwnd)
        except Exception:
            pid = None
        xl.Visible = False
        xl.DisplayAlerts = False
        xl.EnableEvents = False
        xl.AutomationSecurity = 3
        xl.ScreenUpdating = False
        wb = xl.Workbooks.Add()
        while wb.Worksheets.Count < 2:
            wb.Worksheets.Add(After=wb.Worksheets(wb.Worksheets.Count))

        def custom(name, elements, fills=True, borders=True, sizes=None):
            ts = wb.TableStyles.Add(name)
            for e in elements:
                el = ts.TableStyleElements(ELEMENTS.index(e))
                if fills:
                    el.Interior.Color = code(ELEMENTS.index(e), 6)
                if borders:
                    for i, bi in enumerate(BORDERS):
                        b = el.Borders(bi)
                        b.LineStyle = 1
                        b.Weight = 2
                        b.Color = code(ELEMENTS.index(e), i)
                if sizes and e in sizes:
                    el.StripeSize = sizes[e]
            return name

        probes = []  # (sheet, style, config)
        for c in ("all", "bare", "noheader"):
            probes.append((1, custom("P_fills", ELEMENTS, borders=False) if c == "all" else "P_fills", c))
            probes.append((1, custom("P_borders", ELEMENTS, fills=False) if c == "all" else "P_borders", c))
        probes.append((1, custom("P_stripes2", ELEMENTS[5:9], sizes={"firstRowStripe": 2, "firstColumnStripe": 2}), "all"))
        if everything:
            for e in ELEMENTS:
                n = custom("P_" + e, [e])
                for c in ("all", "bare", "noheader"):
                    probes.append((1, n, c))
        for s in BUILT_IN:
            probes.append((2, s, "all"))
        probes.append((2, "TableStyleMedium2", "default"))
        probes.append((2, "TableStyleLight15", "bare"))

        top = {1: 2, 2: 2}
        for i, (sh, style, cfg) in enumerate(probes):
            ws = wb.Worksheets(sh)
            r0 = top[sh]
            top[sh] += 10
            ws.Range(ws.Cells(r0, 2), ws.Cells(r0, 7)).Value = ["C%d" % (k + 1) for k in range(6)]
            for r in range(1, 6):
                for k in range(6):
                    ws.Cells(r0 + r, 2 + k).Value = r * 10 + k
            lo = ws.ListObjects.Add(1, ws.Range(ws.Cells(r0, 2), ws.Cells(r0 + 5, 7)), None, 1)
            lo.Name = "T%d" % i
            lo.TableStyle = style
            o = CONFIGS[cfg]
            lo.ShowTotals = o[1]
            lo.ShowHeaders = o[0]
            lo.ShowTableStyleFirstColumn = o[2]
            lo.ShowTableStyleLastColumn = o[3]
            lo.ShowTableStyleRowStripes = o[4]
            lo.ShowTableStyleColumnStripes = o[5]
            rng = lo.Range
            for r in range(rng.Rows.Count):
                for c in range(rng.Columns.Count):
                    row, col = rng.Row + r, rng.Column + c
                    d = ws.Cells(row, col).DisplayFormat
                    it = d.Interior
                    fill = rgb(it.Color) if it.ColorIndex != -4142 and it.Pattern != -4142 else -1
                    font = rgb(d.Font.Color)
                    bold = 1 if d.Font.Bold is True else 0
                    sides = []
                    for bi in BORDERS[:4]:
                        b = d.Borders(bi)
                        if b.LineStyle in (-4142, None):
                            sides += [0, -1]
                        else:
                            sides += [border_style(int(b.LineStyle), int(b.Weight)), rgb(b.Color)]
                    rows.append((sh - 1, row - 1, col - 1, fill, font, bold, sides, "%s %s" % (style, cfg)))
        wb.SaveAs(os.path.abspath(out_xlsx), 51)
        wb.Close(False)
    finally:
        xl.Quit()
        del xl
        pythoncom.CoUninitialize()
        if pid:
            try:
                h = win32api.OpenProcess(win32con.PROCESS_TERMINATE | win32con.SYNCHRONIZE, False, pid)
                if win32event.WaitForSingleObject(h, 5000) != 0:
                    win32api.TerminateProcess(h, 1)
                win32api.CloseHandle(h)
            except Exception:
                pass

    scrub_author(out_xlsx)

    def hx(v):
        return "-1" if v < 0 else "0x%06X" % v

    with open(out_inc, "w", encoding="utf-8", newline="\n") as f:
        f.write("// Generated by tests/sheet/excel_table_styles.py with tests/sheet/table-styles.xlsx. Do not edit by\n")
        f.write("// hand. What Excel 2021 shows in each cell of the tables (Range.DisplayFormat): {sheet, row, col,\n")
        f.write("// fill (-1: none), font color, bold, then style (BorderStyle) and color of the left, top, bottom and\n")
        f.write("// right borders}.\n")
        last = None
        for sh, r, c, fill, font, bold, sides, label in rows:
            if label != last:
                f.write("// %s\n" % label)
                last = label
            s = ", ".join("%d, %s" % (sides[i], hx(sides[i + 1])) for i in range(0, 8, 2))
            f.write("{%d, %d, %d, %s, %s, %d, %s},\n" % (sh, r, c, hx(fill), hx(font), bold, s))
    print(len(rows), "cells")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], "--all" in sys.argv[3:])
