# Reads Excel's built-in table styles (TableStyleLight1-21, Medium1-28, Dark1-11) from a new,
# separate Excel instance and writes them as JSON: for each style element its fill, font and
# borders, as theme colors with their tint where Excel defines them so. src/sheet/model/TableStylesData.inc
# is generated from it by scripts/table-styles-to-cpp.py.
#
#   python scripts/excel-table-styles.py out.json
#
# Excel runs with alerts, events and macros off, invisibly, and only the Excel this script started
# is ended (Roy may be using Excel at the same time).
import json, sys
import pythoncom, win32api, win32con, win32event, win32process
import win32com.client

ELEMENTS = {0: "wholeTable", 1: "headerRow", 2: "totalRow", 3: "firstColumn", 4: "lastColumn", 5: "firstRowStripe",
            6: "secondRowStripe", 7: "firstColumnStripe", 8: "secondColumnStripe", 9: "firstHeaderCell",
            10: "lastHeaderCell", 11: "firstTotalCell", 12: "lastTotalCell"}
BORDERS = {7: "left", 8: "top", 9: "bottom", 10: "right", 11: "vertical", 12: "horizontal"}


def color(obj):
    """{"theme": n, "tint": t} for a theme color (n as Excel numbers them here: 1 lt1, 2 dk1, 3 lt2,
    4 dk2, 5-10 the accents), else {"rgb": 0xRRGGBB}."""
    try:
        theme = obj.ThemeColor
        if isinstance(theme, int) and 1 <= theme <= 12:
            return {"theme": theme, "tint": round(float(obj.TintAndShade), 6)}
    except Exception:
        pass
    bgr = int(obj.Color)
    return {"rgb": ((bgr & 0xFF) << 16) | (bgr & 0xFF00) | ((bgr >> 16) & 0xFF)}


def main(out):
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
        styles = {}
        for ts in wb.TableStyles:
            name = ts.Name
            if not ts.BuiltIn or not ts.ShowAsAvailableTableStyle or not name.startswith("TableStyle"):
                continue
            elements = {}
            for index, key in ELEMENTS.items():
                e = ts.TableStyleElements(index)
                if not e.HasFormat:
                    continue
                item = {}
                if key in ("firstRowStripe", "secondRowStripe", "firstColumnStripe", "secondColumnStripe"):
                    item["size"] = int(e.StripeSize)
                interior = e.Interior
                if interior.Pattern != -4142 and interior.ColorIndex != -4142:  # xlNone
                    item["fill"] = color(interior)
                font = e.Font
                if font.Bold is True:
                    item["bold"] = True
                if font.Italic is True:
                    item["italic"] = True
                try:
                    fc = font.Color
                    if fc is not None and font.ColorIndex not in (-4105, None):  # xlAutomatic
                        item["font"] = color(font)
                except Exception:
                    pass
                borders = {}
                for bi, bname in BORDERS.items():
                    b = e.Borders(bi)
                    if b.LineStyle in (-4142, None):  # xlNone
                        continue
                    borders[bname] = {"style": int(b.LineStyle), "weight": int(b.Weight), **color(b)}
                if borders:
                    item["borders"] = borders
                elements[key] = item
            styles[name] = elements
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
    with open(out, "w", encoding="utf-8") as f:
        json.dump(styles, f, indent=1, sort_keys=True)
    print(len(styles), "styles")


if __name__ == "__main__":
    main(sys.argv[1])
