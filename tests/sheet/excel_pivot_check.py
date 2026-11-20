# Excel's check of pivots Featherweight wrote (S4, s4-pivot): each workbook is opened in Excel
# (a new instance of its own, DispatchEx; alerts, events and macros off), every pivot table's cells
# read, Excel's Refresh All run, and the cells read again. They must be the same: Featherweight's
# refresh (or new pivot) shows what Excel's own refresh shows. Run scripts/excel-open-check.ps1
# first for repairs (a repaired file fails here too, since it doesn't open normally).
#
#   python tests/sheet/excel_pivot_check.py <workbook>...
import sys
import pythoncom, win32api, win32con, win32event, win32process
import win32com.client


def cells(wb):
    out = {}
    for ws in wb.Worksheets:
        for pt in ws.PivotTables():
            rng = pt.TableRange2
            vals = rng.Value
            if not isinstance(vals, tuple):
                vals = ((vals,),)
            for r, row in enumerate(vals):
                for c, v in enumerate(row):
                    out[(ws.Name, rng.Row + r, rng.Column + c)] = v
    return out


def same(a, b):
    if isinstance(a, float) and isinstance(b, float):
        return abs(a - b) <= 1e-9 * max(1.0, abs(b))
    return a == b


def main(paths):
    pythoncom.CoInitialize()
    xl = win32com.client.DispatchEx("Excel.Application")
    pid = None
    bad = 0
    try:
        try:
            _, pid = win32process.GetWindowThreadProcessId(xl.Hwnd)
        except Exception:
            pid = None
        xl.Visible = False
        xl.DisplayAlerts = False
        xl.EnableEvents = False
        xl.AutomationSecurity = 3
        xl.AskToUpdateLinks = False
        for p in paths:
            wb = xl.Workbooks.Open(p, 0, True)
            before = cells(wb)
            wb.RefreshAll()
            after = cells(wb)
            diffs = [(k, before.get(k), after.get(k)) for k in sorted(set(before) | set(after), key=str)
                     if not same(before.get(k), after.get(k))]
            print("%s: %d pivot cells, %d differ after Excel's refresh" % (p, len(after), len(diffs)))
            for k, x, y in diffs[:15]:
                print("   %s!R%dC%d: ours %r, Excel's %r" % (k[0], k[1], k[2], x, y))
            bad += len(diffs) > 0
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
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
