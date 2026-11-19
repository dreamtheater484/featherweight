# Pivot table fixtures made by Excel (S4, s4-pivot): the expected values for Featherweight's refresh.
#
#   python tests/sheet/excel_pivots.py <out folder>
#
# Writes three workbooks with the same pivots (one per sheet, every layout and function the refresh
# handles), each scrubbed of Windows' user name:
#   pivots-fresh.xlsx      the pivots as Excel created them
#   pivots-stale.xlsx      the source data changed afterwards (values, new and gone items, a table
#                          grown by rows), the pivots not refreshed
#   pivots-refreshed.xlsx  the same after Excel's own Refresh All: what refreshing the stale file must give
# Excel runs in a new instance of its own (DispatchEx), alerts, events and macros off, and only that
# instance is ended.
import os, re, sys, zipfile
import pythoncom, win32api, win32con, win32event, win32process
import win32com.client

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from excel_common import scrub_author, AUTHOR

xlDatabase, xlRowField, xlColumnField, xlPageField, xlDataField = 1, 1, 2, 3, 4
xlSum, xlCount, xlAverage, xlMax, xlMin = -4157, -4112, -4106, -4136, -4139
xlCompactRow, xlTabularRow, xlOutlineRow = 0, 1, 2
xlAscending, xlDescending = 1, 2

HEAD = ["Region", "Product", "Quarter", "Year", "Sales", "Units"]
ROWS = [
    ["North", "A", "Q1", 2024, 120.5, 3], ["North", "B", "Q2", 2024, 80.25, 2], ["North", "C", "Q3", 2025, 310, 7],
    ["South", "A", "Q1", 2025, 45, 1], ["South", "D", "Q4", 2024, 99.99, 4], ["South", "B", "Q2", 2025, 150, 5],
    ["East", "C", "Q3", 2024, 75.5, 2], ["East", "A", "Q4", 2025, 60, 2], ["East", "D", "Q1", 2024, 210, 6],
    ["West", "B", "Q2", 2024, 33.3, 1], ["West", "C", "Q3", 2025, 400, 9], ["West", None, "Q4", 2024, 12, 1],
    ["North", "D", "Q1", 2025, 18.75, 1], ["South", "C", "Q2", 2024, 64, 3], ["East", "B", "Q3", 2025, 128, 4],
    ["West", "A", "Q4", 2025, 256, 8], ["North", "A", "Q2", 2025, 90, 3], ["South", "A", "Q3", 2024, 11.5, 1],
    ["East", "C", "Q1", 2025, 49, 2], ["West", "D", "Q2", 2024, 300, 6],
]


def fill(ws, top, rows):
    data = [HEAD] + rows
    for r, row in enumerate(data):
        for c, v in enumerate(row):
            if v is not None:
                ws.Cells(top + r, c + 1).Value = v


def make(xl, path_fresh, path_stale, path_refreshed):
    wb = xl.Workbooks.Add()
    while wb.Worksheets.Count < 2:
        wb.Worksheets.Add(After=wb.Worksheets(wb.Worksheets.Count))
    data = wb.Worksheets(1)
    data.Name = "Data"
    fill(data, 1, ROWS)
    src = "Data!R1C1:R%dC%d" % (len(ROWS) + 1, len(HEAD))
    # A table as a second source, on its own sheet.
    tsheet = wb.Worksheets(2)
    tsheet.Name = "TableData"
    fill(tsheet, 1, ROWS[:12])
    lo = tsheet.ListObjects.Add(1, tsheet.Range("A1:F13"), None, 1)
    lo.Name = "Sales"

    def new_pivot(name, source=src):
        ws = wb.Worksheets.Add(After=wb.Worksheets(wb.Worksheets.Count))
        ws.Name = name
        pc = wb.PivotCaches().Create(xlDatabase, source, 6)
        return pc.CreatePivotTable(ws.Range("A3") if name not in ("Pages", "Filters") else ws.Range("A5"), name, True, 6)

    def data_field(pt, field, func, caption=None, fmt=None):
        df = pt.AddDataField(pt.PivotFields(field), caption or ("%s of %s" % ({xlSum: "Sum", xlCount: "Count",
                             xlAverage: "Average", xlMax: "Max", xlMin: "Min"}[func], field)), func)
        if fmt:
            df.NumberFormat = fmt
        return df

    # 1. Compact: one row field, one sum.
    pt = new_pivot("Compact")
    pt.PivotFields("Region").Orientation = xlRowField
    data_field(pt, "Sales", xlSum)
    # 2. Compact with rows, columns and two levels, a page field showing all.
    pt = new_pivot("Pages")
    pt.PivotFields("Year").Orientation = xlPageField
    pt.PivotFields("Region").Orientation = xlRowField
    pt.PivotFields("Product").Orientation = xlRowField
    pt.PivotFields("Quarter").Orientation = xlColumnField
    data_field(pt, "Sales", xlSum, fmt="#.##0,00")  # the region's form (en-NL)
    # 3. Outline form, two data fields (count and average).
    pt = new_pivot("Outline")
    pt.RowAxisLayout(xlOutlineRow)
    pt.PivotFields("Region").Orientation = xlRowField
    pt.PivotFields("Product").Orientation = xlRowField
    data_field(pt, "Sales", xlCount)
    data_field(pt, "Units", xlAverage, fmt="0,0")
    # 4. Tabular form: min and max, a column field, the style changed.
    pt = new_pivot("Tabular")
    pt.RowAxisLayout(xlTabularRow)
    pt.PivotFields("Region").Orientation = xlRowField
    pt.PivotFields("Quarter").Orientation = xlRowField
    pt.PivotFields("Year").Orientation = xlColumnField
    data_field(pt, "Sales", xlMax)
    data_field(pt, "Sales", xlMin)
    pt.TableStyle2 = "PivotStyleMedium9"
    # 5. A page field with one item chosen, a hidden row item, no row grand totals.
    pt = new_pivot("Filters")
    pt.PivotFields("Region").Orientation = xlPageField
    pt.PivotFields("Product").Orientation = xlRowField
    pt.PivotFields("Year").Orientation = xlColumnField
    data_field(pt, "Units", xlSum)
    pt.PivotFields("Region").CurrentPage = "North"
    pt.PivotFields("Product").PivotItems("C").Visible = False
    pt.ColumnGrand = False
    # 6. Values on rows, sorted descending, no subtotals on the outer field.
    pt = new_pivot("DataOnRows")
    pt.PivotFields("Region").Orientation = xlRowField
    pt.PivotFields("Quarter").Orientation = xlRowField
    data_field(pt, "Sales", xlSum)
    data_field(pt, "Units", xlSum)
    pt.DataPivotField.Orientation = xlRowField
    pt.PivotFields("Region").AutoSort(xlDescending, "Region")
    pt.PivotFields("Region").Subtotals = (False,) * 12
    # 7. Numbers as row items, a table as the source.
    pt = new_pivot("FromTable", "Sales")
    pt.PivotFields("Year").Orientation = xlRowField
    pt.PivotFields("Region").Orientation = xlColumnField
    data_field(pt, "Sales", xlAverage, fmt="0,00")
    pt.RowGrand = False
    # 8. Two pivots on one cache: the second shares the first's.
    pt = new_pivot("Shared")
    pt.PivotFields("Product").Orientation = xlRowField
    data_field(pt, "Units", xlSum)
    pt2 = pt.PivotCache().CreatePivotTable(wb.Worksheets("Shared").Range("E3"), "Shared2", True, 6)
    pt2.PivotFields("Quarter").Orientation = xlRowField
    data_field(pt2, "Sales", xlCount)

    wb.Worksheets("Compact").Activate()
    wb.SaveAs(os.path.abspath(path_fresh), 51)
    # The source changes: values, a region renamed (a new item, one gone), a blank filled, the table grows.
    data.Range("E2").Value = 1000
    data.Range("A5").Value = "Central"
    data.Range("B13").Value = "E"
    data.Range("F20").Value = 30
    data.Range("D8").Value = 2026
    lo.ListRows.Add()
    for c, v in enumerate(["Central", "E", "Q1", 2026, 77, 7]):
        tsheet.Cells(14, c + 1).Value = v
    lo.ListRows.Add()
    for c, v in enumerate(["North", "A", "Q2", 2024, 5, 1]):
        tsheet.Cells(15, c + 1).Value = v
    wb.SaveAs(os.path.abspath(path_stale), 51)
    wb.RefreshAll()
    wb.SaveAs(os.path.abspath(path_refreshed), 51)
    wb.Close(False)


def scrub_pivot_user(path):
    """Pivot caches record who refreshed them (refreshedBy): Windows' user name. Replace it."""
    tmp = path + ".tmp"
    with zipfile.ZipFile(path) as src, zipfile.ZipFile(tmp, "w") as dst:
        for info in src.infolist():
            data = src.read(info.filename)
            if re.fullmatch(r"xl/pivotCache/pivotCacheDefinition\d+\.xml", info.filename):
                text = data.decode("utf-8")
                text = re.sub(r'refreshedBy="[^"]*"', 'refreshedBy="%s"' % AUTHOR, text)
                data = text.encode("utf-8")
            elif info.filename == "xl/workbook.xml":  # the folder it was saved in (the user's)
                text = data.decode("utf-8")
                text = re.sub(r'<mc:AlternateContent[^>]*><mc:Choice Requires="x15"><x15ac:absPath [^>]*/>'
                              r'</mc:Choice></mc:AlternateContent>', '', text)
                data = text.encode("utf-8")
            dst.writestr(info, data)
    os.replace(tmp, path)


def main(out):
    os.makedirs(out, exist_ok=True)
    paths = [os.path.join(out, n) for n in ("pivots-fresh.xlsx", "pivots-stale.xlsx", "pivots-refreshed.xlsx")]
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
        xl.AskToUpdateLinks = False
        make(xl, *paths)
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
    for p in paths:
        scrub_author(p)
        scrub_pivot_user(p)
        print("written", p)


if __name__ == "__main__":
    main(sys.argv[1])
