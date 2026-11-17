# Makes tests/sheet/objects.xlsx in Excel: shapes with text, a text box, a connector with an arrow,
# a rotated shape, a group, a picture, a combo chart with a secondary axis, a pie, a chart sheet,
# notes (one shown) and, where this Excel has them, threaded comments, as Excel itself writes them. With a second argument,
# also exports Excel's PDF of it there, to hold our drawing up against.
#
#   python tests/sheet/excel_objects.py tests/sheet/objects.xlsx [reference.pdf]
#
# Excel runs in a new, separate instance with alerts, events and macros off; only that instance is
# ended.
import os, sys, zipfile, tempfile
import pythoncom, win32api, win32con, win32event, win32process
import win32com.client

from excel_common import scrub_author

HERE = os.path.dirname(os.path.abspath(__file__))


def main(out, pdf):
    # A picture to insert: the corpus's PNG.
    picture = os.path.join(tempfile.mkdtemp(), "picture.png")
    with zipfile.ZipFile(os.path.join(HERE, "..", "office", "sheet", "pictures-shapes.xlsx")) as z:
        open(picture, "wb").write(z.read("xl/media/image1.png"))
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
        ws = wb.Worksheets(1)
        ws.Name = "Objects"
        rows = [["Month", "North", "South", "West"], ["Jan", 10, 14, 800], ["Feb", 13, 13, 1000], ["Mar", 16, 12, 1200],
                ["Apr", 19, 11, 900], ["May", 22, 10, 1100], ["Jun", 25, 9, 800]]
        for r, row in enumerate(rows):
            for c, v in enumerate(row):
                ws.Cells(r + 1, c + 1).Value = v
        shapes = ws.Shapes
        s = shapes.AddShape(1, 300, 20, 120, 50)  # msoShapeRectangle
        s.Name = "Rectangle text"
        s.TextFrame2.TextRange.Text = "A rectangle with text"
        s = shapes.AddShape(5, 440, 20, 120, 50)  # rounded rectangle
        s.Name = "Rounded bold"
        s.TextFrame2.TextRange.Text = "Bold, centered"
        s.TextFrame2.TextRange.Font.Bold = True
        s.TextFrame2.TextRange.ParagraphFormat.Alignment = 2  # msoAlignCenter
        s.TextFrame2.VerticalAnchor = 3  # msoAnchorMiddle
        s = shapes.AddTextbox(1, 300, 90, 150, 60)
        s.Name = "Text box"
        s.TextFrame2.TextRange.Text = "A text box whose text wraps onto a second line.\rAnd a second paragraph."
        c = shapes.AddConnector(1, 470, 100, 560, 150)  # msoConnectorStraight
        c.Name = "Arrow"
        c.Line.EndArrowheadStyle = 2  # msoArrowheadTriangle
        c.Line.Weight = 1.5
        s = shapes.AddShape(9, 580, 20, 60, 60)  # oval
        s.Name = "Rotated"
        s.Rotation = 30
        a = shapes.AddShape(1, 300, 170, 60, 40)
        a.Name = "Group A"
        b = shapes.AddShape(9, 380, 170, 60, 40)
        b.Name = "Group B"
        b.TextFrame2.TextRange.Text = "B"
        shapes.Range(["Group A", "Group B"]).Group().Name = "Group"
        p = shapes.AddPicture(picture, False, True, 470, 170, 80, 50)
        p.Name = "Picture"

        ch = shapes.AddChart2(-1, 51, 0, 240, 400, 220).Chart  # xlColumnClustered
        ch.SetSourceData(ws.Range("A1:D7"))
        ch.HasTitle = True
        ch.ChartTitle.Text = "Combo with a secondary axis"
        west = ch.SeriesCollection(3)
        west.ChartType = 4  # xlLine
        west.AxisGroup = 2  # xlSecondary
        pie = shapes.AddChart2(-1, 5, 420, 240, 260, 220).Chart  # xlPie
        pie.SetSourceData(ws.Range("A1:B7"))
        pie.ApplyDataLabels()

        n = ws.Range("F10").AddComment("Excel: a note that stays shown.")
        n.Visible = True
        ws.Range("F14").AddComment("Excel: a hidden note.")
        try:
            t = ws.Range("H14").AddCommentThreaded("A threaded comment from Excel.")
            t.AddReply("And a reply.")
        except Exception as e:
            print("no threaded comments:", e)

        cs = wb.Charts.Add(After=ws)
        cs.Name = "Chart sheet"
        cs.SetSourceData(ws.Range("A1:C7"))
        cs.ChartType = 4  # xlLine
        cs.HasTitle = True
        cs.ChartTitle.Text = "Lines on a chart sheet"
        ws.Activate()
        wb.SaveAs(os.path.abspath(out), 51)
        if pdf:
            wb.ExportAsFixedFormat(0, os.path.abspath(pdf))
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
    scrub_author(out)
    print("written", out)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
