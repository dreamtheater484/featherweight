"""Sparkline fixtures (S4): Excel makes the workbook and its PDF, which Featherweight's drawing is
compared with (fwtool pdf-compare, SSIM >= 0.95 a page).

  python tests/office/make_sparkline_tests.py [folder]   (default: tests/office/sparklines)

Writes sparklines.xlsx (the "Sparklines" sheet, active: one case a row, the label in A and the
sparkline in B, which alone prints; the data on "Data") and sparklines.xlsx.print.pdf, the sheet
as Excel prints it to "Microsoft Print to PDF". Excel's Export PDF from automation leaves a sheet
with sparklines blank (2021, measured), and its printer path draws each sparkline as a 600 dpi
bitmap the size of its cell, 0.67 % larger than Export PDF lays the page out (row pitch 34.80
against 34.57 pt), which tests/office/compare_sparklines.py allows for.
Always a new, separate Excel instance (DispatchEx), with alerts, events, link updates and macros
off; only that instance is ended.
"""
import os
import sys

import pythoncom
import win32api
import win32con
import win32event
import win32process
import win32com.client

UP = [3, 5, 2, 8, 6, 9, 4, 7, 10, 5, 8, 6]
MIXED = [3, -2, 5, -4, 6, 1, -3, 7, -1, 4, 2, -5]
GAPS = [3, 5, None, 8, 6, None, None, 7, 10, 5, 8, 6]
WINLOSS = [1, -1, 0, 1, 1, -1, 2, -3, 0, 1, -1, 1]

LINE, COLUMN, WINLOSS_TYPE = 1, 2, 3          # xlSparkLine, xlSparkColumn, xlSparkColumnStacked100
GAP, ZERO, SPAN = 1, 2, 3                      # xlNotPlotted, xlZero, xlInterpolated
CUSTOM = 3                                     # xlSparkScaleCustom


def bgr(rgb):
    return ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF)


# (label, data, type, settings)
CASES = [
    ("line", UP, LINE, {}),
    ("line markers", UP, LINE, {"markers": True}),
    ("line high low first last", MIXED, LINE, {"high": True, "low": True, "first": True, "last": True}),
    ("line negative points", MIXED, LINE, {"negative": True, "markers": True}),
    ("line weight 2.25 red", UP, LINE, {"weight": 2.25, "color": 0xC00000}),
    ("line gaps", GAPS, LINE, {"blanks": GAP}),
    ("line gaps as zero", GAPS, LINE, {"blanks": ZERO}),
    ("line gaps connected", GAPS, LINE, {"blanks": SPAN}),
    ("line axis", MIXED, LINE, {"axis": True}),
    ("column", UP, COLUMN, {}),
    ("column negative axis", MIXED, COLUMN, {"axis": True, "negative": True}),
    ("column high low first last", UP, COLUMN, {"high": True, "low": True, "first": True, "last": True}),
    ("win/loss", WINLOSS, WINLOSS_TYPE, {}),
    ("win/loss axis", WINLOSS, WINLOSS_TYPE, {"axis": True, "negative": True}),
    ("line custom -10..15", UP, LINE, {"min": -10, "max": 15}),
    ("line right to left", UP, LINE, {"rtl": True, "markers": True}),
    ("column colors", MIXED, COLUMN, {"color": 0x00B050, "negative": True, "negColor": 0xFFC000}),
]


def make(out):
    os.makedirs(out, exist_ok=True)
    xlsx = os.path.abspath(os.path.join(out, "sparklines.xlsx"))
    pdf = xlsx + ".print.pdf"
    pythoncom.CoInitialize()
    xl = win32com.client.DispatchEx("Excel.Application")
    pid = None
    try:
        _, pid = win32process.GetWindowThreadProcessId(xl.Hwnd)
        xl.Visible = False
        xl.DisplayAlerts = False
        xl.EnableEvents = False
        xl.AskToUpdateLinks = False
        xl.AutomationSecurity = 3  # msoAutomationSecurityForceDisable
        wb = xl.Workbooks.Add()
        while wb.Worksheets.Count < 2:
            wb.Worksheets.Add(After=wb.Worksheets(wb.Worksheets.Count))
        ws, data = wb.Worksheets(1), wb.Worksheets(2)
        ws.Name, data.Name = "Sparklines", "Data"
        ws.Columns("A").ColumnWidth = 24
        ws.Columns("B").ColumnWidth = 40
        for i, (label, values, kind, s) in enumerate(CASES, start=1):
            for j, v in enumerate(values):
                if v is not None:
                    data.Cells(i, j + 1).Value = v
            ws.Cells(i, 1).Value = label
            ws.Rows(i).RowHeight = 36
            src = "Data!A%d:L%d" % (i, i)
            g = ws.Range("B%d" % i).SparklineGroups.Add(kind, src)
            if "color" in s:
                g.SeriesColor.Color = bgr(s["color"])
            if "weight" in s:
                g.LineWeight = s["weight"]
            p = g.Points
            if s.get("markers"):
                p.Markers.Visible = True
            if s.get("high"):
                p.Highpoint.Visible = True
            if s.get("low"):
                p.Lowpoint.Visible = True
            if s.get("first"):
                p.Firstpoint.Visible = True
            if s.get("last"):
                p.Lastpoint.Visible = True
            if s.get("negative"):
                p.Negative.Visible = True
                if "negColor" in s:
                    p.Negative.Color.Color = bgr(s["negColor"])
            if "blanks" in s:
                g.DisplayBlanksAs = s["blanks"]
            if s.get("axis"):
                g.Axes.Horizontal.Axis.Visible = True
            if "min" in s:
                g.Axes.Vertical.MinScaleType = CUSTOM
                g.Axes.Vertical.CustomMinScaleValue = s["min"]
            if "max" in s:
                g.Axes.Vertical.MaxScaleType = CUSTOM
                g.Axes.Vertical.CustomMaxScaleValue = s["max"]
            if s.get("rtl"):
                g.Axes.Horizontal.RightToLeftPlotOrder = True
        # Only the sparklines print, so a page compares them (SSIM inside the ink).
        ws.PageSetup.PrintArea = "$B$1:$B$%d" % len(CASES)
        ws.Activate()
        wb.SaveAs(xlsx, 51)  # xlOpenXMLWorkbook
        if os.path.exists(pdf):
            os.remove(pdf)
        ws.PrintOut(From=1, To=1, Copies=1, Preview=False, ActivePrinter="Microsoft Print to PDF", PrintToFile=True,
                    Collate=True, PrToFileName=pdf, IgnorePrintAreas=False)
        wb.Close(SaveChanges=False)
    finally:
        try:
            xl.Quit()
        except Exception:  # noqa: BLE001
            pass
        del xl
        pythoncom.CoUninitialize()
        if pid:
            try:  # still running? end it: it is ours
                h = win32api.OpenProcess(win32con.PROCESS_TERMINATE | win32con.SYNCHRONIZE, False, pid)
                if win32event.WaitForSingleObject(h, 5000) != win32event.WAIT_OBJECT_0:
                    win32api.TerminateProcess(h, 1)
                win32api.CloseHandle(h)
            except Exception:  # noqa: BLE001
                pass
    print(xlsx)
    print(pdf)


if __name__ == "__main__":
    make(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "sparklines"))
