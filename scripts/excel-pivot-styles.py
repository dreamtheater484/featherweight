# Reads Excel's built-in PivotTable styles (PivotStyleLight1-28, Medium1-28, Dark1-28) from a new,
# separate Excel instance and writes src/sheet/pivot/PivotStylesData.inc: for each style element its
# fill, font and borders, as theme colors with their tint (S4 s4-pivot; as scripts/excel-table-styles.py
# and table-styles-to-cpp.py do for table styles).
#
#   python scripts/excel-pivot-styles.py src/sheet/pivot/PivotStylesData.inc
#
# Excel runs with alerts, events and macros off, invisibly, and only the Excel this script started
# is ended (Roy may be using Excel at the same time).
import os, re, sys
import pythoncom, win32api, win32con, win32event, win32process
import win32com.client

# XlTableStyleElementType 0-27: wholeTable, headerRow, grandTotalRow, firstColumn,
# grandTotalColumn, rowStripe1-2, columnStripe1-2, firstHeaderCell, lastHeaderCell,
# firstTotalCell, lastTotalCell, subtotalColumn1-3, subtotalRow1-3, blankRow,
# columnSubheading1-3, rowSubheading1-3, pageFieldLabels, pageFieldValues.
ELEMENTS = range(28)
STRIPES = (5, 6, 7, 8)
BORDERS = {7: 0, 8: 1, 9: 2, 10: 3, 11: 4, 12: 5}  # xlEdgeLeft.. -> left, top, bottom, right, vertical, horizontal


def theme(obj):
    """(n, tint) for a theme color, SpreadsheetML's numbering (0 lt1, 1 dk1, 2 lt2, 3 dk2, 4-9 accents)."""
    try:
        t = obj.ThemeColor
        if isinstance(t, int) and 1 <= t <= 12:
            return t - 1, round(float(obj.TintAndShade), 6)
    except Exception:
        pass
    return None


def border_style(b):
    if int(b.LineStyle) == -4119:
        return 6  # double
    return {2: 1, -4138: 2, 4: 5, 1: 7}.get(int(b.Weight), 1)


def style_index(name):
    m = re.fullmatch(r"PivotStyle(Light|Medium|Dark)(\d+)", name)
    if not m:
        return None
    return {"Light": 0, "Medium": 28, "Dark": 56}[m.group(1)] + int(m.group(2)) - 1


def main(out):
    rows, skipped = [], 0
    pythoncom.CoInitialize()
    xl = win32com.client.DispatchEx("Excel.Application")
    pid = None
    try:
        try:
            _, pid = win32process.GetWindowThreadProcessId(xl.Hwnd)
        except Exception:
            pid = None
        xl.Visible = False
        xl.DisplayAlerts = False
        xl.EnableEvents = False
        xl.AutomationSecurity = 3
        wb = xl.Workbooks.Add()
        for ts in wb.TableStyles:
            s = style_index(ts.Name)
            if s is None or not ts.BuiltIn:
                continue
            for el in ELEMENTS:
                try:
                    e = ts.TableStyleElements(el)
                    if not e.HasFormat:
                        continue
                except Exception:
                    continue
                if el in STRIPES and int(e.StripeSize) != 1:
                    rows.append((s, el, 4, int(e.StripeSize), 0.0))
                interior = e.Interior
                if interior.Pattern != -4142 and interior.ColorIndex != -4142:
                    t = theme(interior)
                    if t:
                        rows.append((s, el, 0, t[0], t[1]))
                    else:
                        skipped += 1
                font = e.Font
                try:
                    if font.ColorIndex not in (-4105, None):
                        t = theme(font)
                        if t and not (el == 0 and t == (1, 0.0)):
                            rows.append((s, el, 1, t[0], t[1]))
                except Exception:
                    pass
                if font.Bold is True:
                    rows.append((s, el, 2, 0, 0.0))
                if font.Italic is True:
                    rows.append((s, el, 3, 0, 0.0))
                for bi, side in BORDERS.items():
                    b = e.Borders(bi)
                    if b.LineStyle in (-4142, None):
                        continue
                    t = theme(b)
                    if not t:
                        skipped += 1
                        continue
                    rows.append((s, el, (8 + side) | (border_style(b) << 4), t[0], t[1]))
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
    rows.sort(key=lambda r: (r[0], r[1]))
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("// Excel's built-in PivotTable styles (PivotStyleLight1-28, Medium1-28, Dark1-28), read from Excel\n")
        f.write("// 2021 by scripts/excel-pivot-styles.py. Do not edit. {style, element (XlTableStyleElementType),\n")
        f.write("// part, theme color or stripe size, tint}; parts as TableStylesData.inc: 0 fill, 1 font color,\n")
        f.write("// 2 bold, 3 italic, 4 stripe size, 8-13 a side's border with its BorderStyle in the high bits.\n")
        for r in rows:
            f.write("{%d, %d, %d, %d, %sf},\n" % (r[0], r[1], r[2], r[3], repr(round(r[4], 6))))
    print(len(rows), "entries,", skipped, "skipped (not theme colors)")


if __name__ == "__main__":
    main(sys.argv[1])
