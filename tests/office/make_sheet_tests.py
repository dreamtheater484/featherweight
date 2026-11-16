"""Generates S1's public spreadsheet test files (docs/s1-plan.md, "Corpus and measuring").

  sheet/  workbooks made with openpyxl, at least one per S1 feature: the number-format sampler,
          the 1900 and 1904 date systems, merged cells and frozen panes, rows and columns, text
          and alignment, borders and fills, sheets and views, one sheet per function, other
          formulas (operators, references, names, errors, arrays, tables), conditional formats,
          tables, pictures, notes, charts and a chart sheet, and print settings.
          openpyxl writes formulas without cached values; scripts/make-sheet-files.ps1 has Excel
          open each one, add what openpyxl can't write (shared formulas, dynamic arrays, shapes,
          threaded comments, sparklines), recalculate and save a copy in excel/.
  csv/    CSV and TSV files byte for byte as written here, and csv/expected.json: each file's
          encoding, byte-order mark, delimiter and line ends, and the exact text of every field.
          Opening a CSV must keep each of these values unchanged (plan section 7.1).

Requires the venv (tests/office/README.md, "Regenerating"):
  .venv\\Scripts\\python tests/office/make_sheet_tests.py

The output is deterministic: dates and zip timestamps are fixed.
"""
import codecs
import io
import json
import os
import re
import sys
import zipfile

import openpyxl
from openpyxl.cell.rich_text import CellRichText, TextBlock
from openpyxl.cell.text import InlineFont
from openpyxl.chart import (AreaChart, BarChart, DoughnutChart, LineChart, PieChart, RadarChart,
                            Reference, ScatterChart, Series)
from openpyxl.chart.label import DataLabelList
from openpyxl.comments import Comment
from openpyxl.drawing.image import Image as XlImage
from openpyxl.drawing.spreadsheet_drawing import AnchorMarker, TwoCellAnchor
from openpyxl.formatting.rule import (CellIsRule, ColorScaleRule, DataBarRule, FormulaRule,
                                      IconSetRule, Rule)
from openpyxl.styles import (Alignment, Border, Font, GradientFill, PatternFill, Protection,
                             Side)
from openpyxl.styles.colors import Color
from openpyxl.styles.differential import DifferentialStyle
from openpyxl.utils import get_column_letter, range_boundaries
from openpyxl.utils.datetime import CALENDAR_MAC_1904
from openpyxl.workbook.defined_name import DefinedName
from openpyxl.worksheet.filters import AutoFilter
from openpyxl.worksheet.formula import ArrayFormula
from openpyxl.worksheet.pagebreak import Break
from openpyxl.worksheet.properties import PageSetupProperties
from openpyxl.worksheet.table import Table, TableColumn, TableFormula, TableStyleInfo
from openpyxl.worksheet.views import Pane
from PIL import Image

from make_test_office import edit_part, normalize, png, read_entries, save, set_core, to_bytes, write_zip

ROOT = os.path.dirname(os.path.abspath(__file__))
SHEET = os.path.join(ROOT, "sheet")
CSV = os.path.join(ROOT, "csv")


def finish(wb, title, fix_styles=False):
    set_core(wb.properties, title)
    data = to_bytes(wb)
    if fix_styles:
        data = write_zip(edit_part(read_entries(data), "xl/styles.xml", builtin_ids))
    return normalize(data)


# ---------- Number formats ----------

# A built-in format is written as Excel writes it: numFmtId only, no numFmt element, so the
# reader must know the code (and take the locale-dependent ones from the locale). openpyxl can't
# do that for every id, so each gets a placeholder custom code that builtin_ids() turns into the id.
def builtin(n):
    return f'"fwbuiltin{n}"'


def builtin_ids(body):
    text = body.decode("utf-8")
    mapping = {}

    def drop(m):
        mapping[m.group(1)] = m.group(2)
        return ""
    text = re.sub(r'<numFmt numFmtId="(\d+)" formatCode="&quot;fwbuiltin(\d+)&quot;"\s*/>', drop, text)
    left = len(re.findall(r"<numFmt ", text))
    if left:
        text = re.sub(r'<numFmts count="\d+"', f'<numFmts count="{left}"', text)
    else:
        text = re.sub(r'<numFmts count="\d+"\s*(/>|>\s*</numFmts>)', "", text)
    text = re.sub(r'numFmtId="(\d+)"', lambda m: f'numFmtId="{mapping.get(m.group(1), m.group(1))}"', text)
    return text.encode("utf-8")


SAMPLE_VALUES = [1234.5678, -1234.5678, 0, 0.125, 45678.6789, "Text"]
BUILTIN_IDS = list(range(0, 59))  # 0-49 are Excel's own; 50-58 East Asian (none in this locale)

CUSTOM_CODES = [
    # Digits, separators, scaling, percent, scientific
    "0", "0.00", "#,##0", "#,##0.00", "#.##", "0.0#", "#", "00000", "000-00-0000", "(###) ###-####",
    "#,###", "#,##0,", '#,##0,,"M"', "0.0,,", "0%", "0.00%", "0.0E+00", "0.00E+00", "##0.0E+0", "0E-0",
    "0.000E+000", "0.000000000000000000", "#,##0.000000", "???.???", "#??.??", "0.0?", "0.00_);(0.00)",
    # Fractions
    "# ?/?", "# ??/??", "# ???/???", "?/2", "?/4", "# ?/8", "# ??/16", "?/10", "# ?/100", "0 ?/?",
    # Sections and conditions
    '0;-0;"zero"', '0;(0);"-"', '0.00;[Red]-0.00;0.00;"text: "@', '[>=1000]#,##0,"K";0',
    '[>=1000000]0.0,,"M";[>=1000]0.0,"K";0', '[<0]"neg";[>0]"pos";"zero"', "[Red][<=-1]0;[Blue][>=1]0;0.00",
    '[=0]"none";0', ";;;", "0;;", ";;;@", '"pos";"neg"', "General;-General;\"zero\";@",
    # Colors
    "[Red]0", "[Blue]0;[Red]-0", "[Green]0.00", "[Magenta]0", "[Cyan]0", "[Yellow]0", "[White]0",
    "[Black]0", "[Color10]0", "[Color46]0.00", "[COLOR3]0", "[Red]General", "[Blue]@",
    # Literals, escapes, text
    '"Total: "0.00', '0.00" kg"', "\\$0.00", "0\\%", '0" units"', "@", '"Name: "@', '@" (text)"', '"("@")"',
    'General" units"', '"x"General',
    # Currencies and locales
    "[$€-407] #,##0.00", "#,##0.00 [$€-1]", "[$$-409]#,##0.00", "[$£-809]#,##0.00", "[$¥-411]#,##0",
    "[$CHF] #,##0.00", "[$USD] #,##0.00", "[$-409]mmmm d, yyyy", "[$-407]mmmm d, yyyy",
    "[$-40C]dddd d mmmm yyyy", "[$-411]yyyy\"年\"m\"月\"d\"日\"", "[$-F800]dddd, mmmm dd, yyyy",
    "[$-F400]h:mm:ss AM/PM", "[$-x-sysdate]dddd, mmmm dd, yyyy", "[$-x-systime]h:mm:ss AM/PM",
    "[$-409]h:mm AM/PM;@", "[$-en-US]d-mmm-yyyy",
    # Accounting: _ and * (padding and fill)
    "_(* #,##0.00_);_(* (#,##0.00);_(* \"-\"??_);_(@_)", '_("$"* #,##0.00_);_("$"* (#,##0.00);_("$"* "-"??_);_(@_)',
    '_-* #,##0.00_-;-* #,##0.00_-;_-* "-"??_-;_-@_-', '_-[$€-x-euro1] * #,##0.00_-;-[$€-x-euro1] * #,##0.00_-',
    "* #,##0", "#,##0.00*-", "@*.", "*-0", "0_)", '"$"* #,##0.00',
    # Dates and times
    "d/m/yyyy", "m/d/yyyy", "yyyy-mm-dd", "dd-mmm-yy", "mmm-yy", "mmmm yyyy", "mmmmm", "ddd", "dddd",
    "d mmmm yyyy", "yy", "yyyy", "e", "h:mm", "h:mm:ss", "hh:mm:ss.000", "h:mm AM/PM", "h:mm:ss A/P",
    "h AM/PM", "[h]:mm:ss", "[h]:mm", "[mm]:ss", "[ss]", "[ss].00", "mm:ss.0", "m/d/yyyy h:mm",
    "yyyy-mm-dd hh:mm:ss", "dd/mm/yyyy hh:mm", "d-mmm", "mm", "m:ss", "h:m:s", "yyyy-mm-dd\"T\"hh:mm",
]

NARROW_CODES = ["General", "0", "0.00", "#,##0.00", "0.00E+00", "0%", "yyyy-mm-dd", "dddd d mmmm yyyy",
                "h:mm:ss", "[h]:mm:ss", "@", '"Total: "0.00', "_(* #,##0.00_);_(* (#,##0.00);_(* \"-\"??_);_(@_)"]

GENERAL_VALUES = [0, 1, -1, 0.1, 0.5, 1 / 3, 2 / 3, 10, 100, 1000, 12345, 123456789, 1234567890,
                  12345678901, 123456789012, 1.23456789012345e15, 1e15, 1e16, 12345678901234567890,
                  0.1 + 0.2, 0.000012345, 1e-10, 1.5e-300, 9.99999999999999e307, 100000, 1000000,
                  99999999999, 3.14159265358979, -3.14159265358979, 2.5e-5, 123.456, 1234567.891,
                  0.0001, 0.00001, -0.000123456789, 1234.5678, 45678.6789, 1e100, -1e-100, 7.0e22]
GENERAL_WIDTHS = [1.5, 3, 5, 7, 8.43, 10, 12, 15, 20, 30]

DATE_SERIALS = [0, 0.25, 1, 59, 60, 61, 366, 45678, 45678.5, 45678.999988426, 45678.9999999, 2958465,
                2958465.9999, 2958466, -1, -0.5, 1.5, 10.75, 100.123456]
DATE_FORMATS = [("General", "General"), ("14 (short date)", builtin(14)), ("22 (date and time)", builtin(22)),
                ("yyyy-mm-dd", "yyyy-mm-dd"), ("d/m/yyyy", "d/m/yyyy"), ("m/d/yy", "m/d/yy"),
                ("dddd d mmmm yyyy", "dddd d mmmm yyyy"), ("mmm-yy", "mmm-yy"), ("h:mm:ss", "h:mm:ss"),
                ("[h]:mm:ss", "[h]:mm:ss"), ("[mm]:ss", "[mm]:ss"), ("[ss].000", "[ss].000"),
                ("hh:mm:ss.00", "hh:mm:ss.00"), ("h:mm AM/PM", "h:mm AM/PM"),
                ("yyyy-mm-dd hh:mm:ss", "yyyy-mm-dd hh:mm:ss"), ("18 (h:mm AM/PM)", builtin(18)),
                ("21 (h:mm:ss)", builtin(21)), ("46 ([h]:mm:ss)", builtin(46)), ("47 (mmss.0)", builtin(47))]

BOLD = Font(bold=True)


def header(ws, values, row=1):
    for c, v in enumerate(values, start=1):
        cell = ws.cell(row=row, column=c, value=v)
        cell.font = BOLD


def xlsx_numfmt():
    wb = openpyxl.Workbook()
    labels = ["1234.5678", "-1234.5678", "0", "0.125", "45678.6789", "Text"]

    ws = wb.active
    ws.title = "Built-in"
    header(ws, ["Id", "openpyxl's code"] + labels)
    from openpyxl.styles.numbers import BUILTIN_FORMATS
    for r, n in enumerate(BUILTIN_IDS, start=2):
        ws.cell(row=r, column=1, value=n)
        ws.cell(row=r, column=2, value=BUILTIN_FORMATS.get(n, "(not in openpyxl)"))
        for c, v in enumerate(SAMPLE_VALUES, start=3):
            ws.cell(row=r, column=c, value=v).number_format = builtin(n)
    ws.column_dimensions["B"].width = 44
    for c in range(3, 9):
        ws.column_dimensions[get_column_letter(c)].width = 24
    ws.freeze_panes = "C2"

    ws = wb.create_sheet("Custom")
    header(ws, ["Format code"] + labels)
    for r, code in enumerate(CUSTOM_CODES, start=2):
        ws.cell(row=r, column=1, value=code)
        for c, v in enumerate(SAMPLE_VALUES, start=2):
            ws.cell(row=r, column=c, value=v).number_format = code
    ws.column_dimensions["A"].width = 60
    for c in range(2, 8):
        ws.column_dimensions[get_column_letter(c)].width = 26
    ws.freeze_panes = "B2"

    # The same codes in columns too narrow for most values: "####", General rounding, text clipped.
    ws = wb.create_sheet("Narrow")
    header(ws, ["Format code"] + labels)
    for r, code in enumerate(NARROW_CODES, start=2):
        ws.cell(row=r, column=1, value=code)
        for c, v in enumerate(SAMPLE_VALUES, start=2):
            ws.cell(row=r, column=c, value=v).number_format = code
    ws.column_dimensions["A"].width = 50
    for c, w in zip(range(2, 8), [4, 6, 8, 3, 10, 5]):
        ws.column_dimensions[get_column_letter(c)].width = w

    # General as Excel shows it depends on the column width (up to 11 characters, then rounding
    # or scientific): each value in columns of different widths.
    ws = wb.create_sheet("General")
    header(ws, ["Value (repr)"] + [f"width {w}" for w in GENERAL_WIDTHS])
    for r, v in enumerate(GENERAL_VALUES, start=2):
        ws.cell(row=r, column=1, value=repr(v))
        for c in range(2, 2 + len(GENERAL_WIDTHS)):
            ws.cell(row=r, column=c, value=v)
    ws.column_dimensions["A"].width = 26
    for c, w in enumerate(GENERAL_WIDTHS, start=2):
        ws.column_dimensions[get_column_letter(c)].width = w
    return finish(wb, "Number formats", fix_styles=True)


def xlsx_dates(epoch1904):
    wb = openpyxl.Workbook()
    if epoch1904:
        wb.epoch = CALENDAR_MAC_1904
    ws = wb.active
    ws.title = "Dates"
    header(ws, ["Serial"] + [label for label, _ in DATE_FORMATS])
    for r, v in enumerate(DATE_SERIALS, start=2):
        ws.cell(row=r, column=1, value=v)
        for c, (_, code) in enumerate(DATE_FORMATS, start=2):
            ws.cell(row=r, column=c, value=v).number_format = code
    ws.column_dimensions["A"].width = 16
    for c in range(2, 2 + len(DATE_FORMATS)):
        ws.column_dimensions[get_column_letter(c)].width = 30
    # Date functions follow the workbook's date system (cached values come from Excel's copy).
    ws = wb.create_sheet("Functions")
    header(ws, ["Formula", "Result", "As date"])
    for r, f in enumerate(["=DATE(2025,1,31)", "=DATE(1904,1,1)", "=DATE(1900,1,1)", "=DATE(1900,3,1)",
                           "=YEAR(0)", "=YEAR(1)", "=WEEKDAY(0)", "=DATEVALUE(\"2025-01-31\")",
                           "=TIME(25,0,0)", "=DAY(60)"], start=2):
        ws.cell(row=r, column=1, value=f[1:])
        ws.cell(row=r, column=2, value=f)
        ws.cell(row=r, column=3, value=f).number_format = "yyyy-mm-dd hh:mm"
    ws.column_dimensions["A"].width = 30
    ws.column_dimensions["B"].width = 14
    ws.column_dimensions["C"].width = 20
    return finish(wb, "Dates, 1904 date system" if epoch1904 else "Dates, 1900 date system", fix_styles=True)


# ---------- Layout ----------


def fill_grid(ws, rows, cols, start_row=1):
    for r in range(start_row, start_row + rows):
        for c in range(1, cols + 1):
            ws.cell(row=r, column=c, value=f"{get_column_letter(c)}{r}")


def xlsx_merged_frozen():
    wb = openpyxl.Workbook()
    ws = wb.active
    ws.title = "Merged"
    fill_grid(ws, 30, 10)
    thin = Side(style="thin", color="FF000000")
    for rng, text, align in [("B2:D2", "Across three columns", Alignment(horizontal="center")),
                             ("B4:B8", "Down five rows", Alignment(vertical="center", wrap_text=True)),
                             ("D4:F8", "A block of 3 by 5, centered both ways", Alignment(horizontal="center", vertical="center", wrap_text=True)),
                             ("H2:J3", "Right and bottom", Alignment(horizontal="right", vertical="bottom")),
                             ("B10:F10", "A long merged title whose text is longer than the merged area itself, clipped", None),
                             ("B12:C13", 1234567.891, None)]:
        ws.merge_cells(rng)
        first = ws[rng.split(":")[0]]
        first.value = text
        if align:
            first.alignment = align
    for row in ws["H5:J7"]:
        for cell in row:
            cell.border = Border(left=thin, right=thin, top=thin, bottom=thin)
    ws.merge_cells("H5:J7")
    ws["H5"] = "Merged with a border"
    ws["B12"].number_format = "#,##0.00"
    ws.merge_cells("A20:J20")
    ws["A20"] = "Merged across the frozen boundary in the next sheet's style"
    ws["A20"].fill = PatternFill("solid", fgColor="FFFFE699")

    ws = wb.create_sheet("Frozen rows and columns")
    fill_grid(ws, 60, 20)
    ws.freeze_panes = "C3"
    ws = wb.create_sheet("Frozen top row")
    header(ws, [f"Header {c}" for c in range(1, 16)])
    fill_grid(ws, 60, 12, start_row=2)
    ws.freeze_panes = "A2"
    ws = wb.create_sheet("Frozen first column")
    fill_grid(ws, 25, 30)
    ws.freeze_panes = "B1"
    ws = wb.create_sheet("Split")
    fill_grid(ws, 40, 15)
    ws.sheet_view.pane = Pane(xSplit=3000, ySplit=2000, topLeftCell="E10", activePane="bottomRight", state="split")
    ws = wb.create_sheet("Frozen and scrolled")
    fill_grid(ws, 60, 16)
    ws.sheet_view.pane = Pane(xSplit=2, ySplit=4, topLeftCell="F30", activePane="bottomRight", state="frozen")
    return finish(wb, "Merged cells and frozen panes")


def xlsx_rows_columns():
    wb = openpyxl.Workbook()
    ws = wb.active
    ws.title = "Widths and heights"
    fill_grid(ws, 20, 12)
    for c, w in zip("ABCDEFGHIJKL", [2, 4, 8.43, 12, 20, 40, 0.5, 9.14, 15.71, 1, 30, 100]):
        ws.column_dimensions[c].width = w
    for r, h in [(2, 5), (3, 30), (4, 60), (5, 120), (6, 0.75), (7, 409), (8, 15.75)]:
        ws.row_dimensions[r].height = h
    ws["F4"].alignment = Alignment(wrap_text=True)
    ws["F4"] = "A tall row with wrapped text that needs several lines in this column"

    ws = wb.create_sheet("Hidden")
    fill_grid(ws, 30, 12)
    for c in "CEF":
        ws.column_dimensions[c].hidden = True
    for r in (3, 5, 6, 7, 20):
        ws.row_dimensions[r].hidden = True

    ws = wb.create_sheet("Outline")
    header(ws, ["Region", "Item", "Q1", "Q2", "Q3", "Q4", "Year"])
    r = 2
    for region in ["North", "South", "East"]:
        start = r
        for item in ["Apples", "Pears", "Plums"]:
            ws.append([region, item, 10, 20, 30, 40, f"=SUM(C{r}:F{r})"])
            r += 1
        ws.append([f"{region} total", None] + [f"=SUBTOTAL(9,{c}{start}:{c}{r - 1})" for c in "CDEFG"])
        ws.row_dimensions.group(start, r - 1, outline_level=2, hidden=(region == "South"))
        r += 1
    ws.append(["Grand total", None] + [f"=SUBTOTAL(9,{c}2:{c}{r - 1})" for c in "CDEFG"])
    ws.row_dimensions.group(2, r - 1, outline_level=1)
    ws.column_dimensions.group("C", "F", outline_level=1, hidden=False)
    ws.sheet_format.outlineLevelRow = 2
    ws.sheet_format.outlineLevelCol = 1

    ws = wb.create_sheet("Outline above and left")
    fill_grid(ws, 20, 10)
    ws.row_dimensions.group(3, 8, outline_level=1, hidden=True)
    ws.column_dimensions.group("C", "E", outline_level=1, hidden=True)
    ws.sheet_properties.outlinePr.summaryBelow = False
    ws.sheet_properties.outlinePr.summaryRight = False
    ws.sheet_format.outlineLevelRow = 1
    ws.sheet_format.outlineLevelCol = 1

    ws = wb.create_sheet("Default sizes")
    fill_grid(ws, 15, 8)
    ws.sheet_format.defaultRowHeight = 24
    ws.sheet_format.customHeight = True
    ws.sheet_format.baseColWidth = 14
    ws.sheet_format.defaultColWidth = 14.7109375
    return finish(wb, "Rows and columns")


def xlsx_text():
    wb = openpyxl.Workbook()
    ws = wb.active
    ws.title = "Alignment"
    ws.column_dimensions["A"].width = 26
    for c in "BCDEFG":
        ws.column_dimensions[c].width = 18
    header(ws, ["Setting", "Text", "Number"])
    rows = [("General", None), ("left", Alignment(horizontal="left")), ("center", Alignment(horizontal="center")),
            ("right", Alignment(horizontal="right")), ("fill", Alignment(horizontal="fill")),
            ("justify", Alignment(horizontal="justify", wrap_text=True)),
            ("centerContinuous", Alignment(horizontal="centerContinuous")),
            ("distributed", Alignment(horizontal="distributed")), ("indent 2", Alignment(horizontal="left", indent=2)),
            ("right indent 3", Alignment(horizontal="right", indent=3)), ("top", Alignment(vertical="top")),
            ("center (vertical)", Alignment(vertical="center")), ("bottom", Alignment(vertical="bottom")),
            ("shrink to fit", Alignment(shrink_to_fit=True)), ("wrap", Alignment(wrap_text=True)),
            ("rotate 45", Alignment(text_rotation=45)), ("rotate 90", Alignment(text_rotation=90)),
            ("rotate -45 (135)", Alignment(text_rotation=135)), ("rotate -90 (180)", Alignment(text_rotation=180)),
            ("vertical stacked (255)", Alignment(text_rotation=255)), ("wrap and rotate 30", Alignment(text_rotation=30, wrap_text=True))]
    for r, (label, al) in enumerate(rows, start=2):
        ws.cell(row=r, column=1, value=label)
        t = ws.cell(row=r, column=2, value="Some text to align")
        n = ws.cell(row=r, column=3, value=1234.5)
        if al:
            t.alignment = al
            n.alignment = al
        ws.row_dimensions[r].height = 60 if "rotate" in label or "stacked" in label or label in ("top", "center (vertical)", "bottom", "wrap") else None

    ws = wb.create_sheet("Overflow")
    ws.column_dimensions["A"].width = 12
    long = "This text is much longer than its cell and spills into the empty cells next to it"
    ws["A1"] = long
    ws["A2"] = long
    ws["B2"] = "stop"
    ws["C3"] = long
    ws["C3"].alignment = Alignment(horizontal="right")
    ws["E4"] = long
    ws["E4"].alignment = Alignment(horizontal="center")
    ws["A5"] = 12345678901234
    ws["A6"] = "A line break here\nand a second line, not wrapped"
    ws["A7"] = "A line break here\nand a second line, wrapped"
    ws["A7"].alignment = Alignment(wrap_text=True)
    ws["A8"] = "   leading and trailing spaces   "
    ws["A9"] = "Tab\tinside"
    ws["H10"] = long
    ws["G10"] = None

    ws = wb.create_sheet("Fonts")
    ws.column_dimensions["A"].width = 30
    ws.column_dimensions["B"].width = 50
    fonts = [("Calibri 11", Font(name="Calibri", size=11)), ("Arial 10", Font(name="Arial", size=10)),
             ("Times New Roman 12", Font(name="Times New Roman", size=12)), ("Courier New 10", Font(name="Courier New", size=10)),
             ("Segoe UI 9", Font(name="Segoe UI", size=9)), ("Cambria 14", Font(name="Cambria", size=14)),
             ("Calibri 6", Font(size=6)), ("Calibri 24", Font(size=24)), ("Calibri 36", Font(size=36)),
             ("bold", Font(bold=True)), ("italic", Font(italic=True)), ("bold italic", Font(bold=True, italic=True)),
             ("underline single", Font(underline="single")), ("underline double", Font(underline="double")),
             ("underline single accounting", Font(underline="singleAccounting")),
             ("underline double accounting", Font(underline="doubleAccounting")), ("strikethrough", Font(strike=True)),
             ("superscript", Font(vertAlign="superscript")), ("subscript", Font(vertAlign="subscript")),
             ("red (RGB)", Font(color="FFFF0000")), ("theme 4 (accent 1)", Font(color=Color(theme=4))),
             ("theme 5, tint -0.25", Font(color=Color(theme=5, tint=-0.25))), ("indexed 10", Font(color=Color(indexed=10))),
             ("a font that isn't installed", Font(name="Featherweight Missing Font", size=11))]
    for r, (label, f) in enumerate(fonts, start=1):
        ws.cell(row=r, column=1, value=label)
        ws.cell(row=r, column=2, value="The quick brown fox 0123456789").font = f
    r = len(fonts) + 2
    ws.cell(row=r, column=1, value="rich text runs")
    ws.cell(row=r, column=2, value=CellRichText(
        "Plain, ", TextBlock(InlineFont(b=True), "bold, "), TextBlock(InlineFont(i=True, color="FF0070C0"), "blue italic, "),
        TextBlock(InlineFont(sz=16, rFont="Cambria"), "Cambria 16, "), TextBlock(InlineFont(u="single", strike=True), "struck"),
        TextBlock(InlineFont(vertAlign="superscript"), "2")))
    ws.cell(row=r + 1, column=1, value="scripts")
    ws.cell(row=r + 1, column=2, value="Café Straße ÅÆØ Ελληνικά Русский 東京 서울 עברית العربية 𝄞")
    ws.cell(row=r + 2, column=1, value="right to left")
    ws.cell(row=r + 2, column=2, value="مرحبا بكم في Featherweight").alignment = Alignment(readingOrder=2)
    return finish(wb, "Text, alignment and fonts")


def xlsx_borders_fills():
    wb = openpyxl.Workbook()
    ws = wb.active
    ws.title = "Borders"
    ws.column_dimensions["A"].width = 22
    styles = ["thin", "medium", "thick", "dashed", "dotted", "double", "hair", "mediumDashed", "dashDot",
              "mediumDashDot", "dashDotDot", "mediumDashDotDot", "slantDashDot"]
    for r, s in enumerate(styles, start=2):
        ws.cell(row=r * 2, column=1, value=s)
        side = Side(style=s, color="FF000000")
        ws.cell(row=r * 2, column=2).border = Border(left=side, right=side, top=side, bottom=side)
        ws.cell(row=r * 2, column=4).border = Border(bottom=Side(style=s, color="FFC00000"))
    d = Side(style="thin", color="FF0070C0")
    ws["F4"].border = Border(diagonal=d, diagonalUp=True)
    ws["F6"].border = Border(diagonal=d, diagonalDown=True)
    ws["F8"].border = Border(diagonal=Side(style="double", color="FF00B050"), diagonalUp=True, diagonalDown=True)
    ws["F10"].border = Border(left=Side(style="thick", color=Color(theme=4)), right=Side(style="thick", color=Color(theme=9, tint=0.4)))
    # Adjacent cells with different borders on the shared edge: which one wins.
    ws["H4"].border = Border(right=Side(style="thick", color="FFFF0000"))
    ws["I4"].border = Border(left=Side(style="thin", color="FF0000FF"))
    ws["H6"].border = Border(bottom=Side(style="double"))
    ws["H7"].border = Border(top=Side(style="dotted"))

    ws = wb.create_sheet("Fills")
    ws.column_dimensions["A"].width = 22
    patterns = ["solid", "darkGray", "mediumGray", "lightGray", "gray125", "gray0625", "darkHorizontal",
                "darkVertical", "darkDown", "darkUp", "darkGrid", "darkTrellis", "lightHorizontal", "lightVertical",
                "lightDown", "lightUp", "lightGrid", "lightTrellis"]
    for r, p in enumerate(patterns, start=1):
        ws.cell(row=r, column=1, value=p)
        ws.cell(row=r, column=2).fill = PatternFill(p, fgColor="FF4472C4", bgColor="FFFFFF00")
        ws.cell(row=r, column=3, value="text on it").fill = PatternFill(p, fgColor=Color(theme=6), bgColor=Color(theme=0))
    r = len(patterns) + 2
    ws.cell(row=r, column=1, value="linear gradient 0")
    ws.cell(row=r, column=2).fill = GradientFill(stop=("FFFF0000", "FF0000FF"), degree=0)
    ws.cell(row=r + 1, column=1, value="linear gradient 90")
    ws.cell(row=r + 1, column=2).fill = GradientFill(stop=("FFFFFFFF", "FF00B050"), degree=90)
    ws.cell(row=r + 2, column=1, value="path gradient")
    ws.cell(row=r + 2, column=2).fill = GradientFill(type="path", left=0.5, right=0.5, top=0.5, bottom=0.5, stop=("FFFFFFFF", "FF7030A0"))

    ws = wb.create_sheet("Theme colors")
    header(ws, ["Theme"] + [f"tint {t}" for t in (-0.5, -0.25, 0, 0.4, 0.6, 0.8)])
    for theme in range(10):
        ws.cell(row=theme + 2, column=1, value=theme)
        for c, t in enumerate((-0.5, -0.25, 0, 0.4, 0.6, 0.8), start=2):
            cell = ws.cell(row=theme + 2, column=c, value="Aa")
            cell.fill = PatternFill("solid", fgColor=Color(theme=theme, tint=t))
            cell.font = Font(color=Color(theme=(theme + 1) % 10))

    ws = wb.create_sheet("Indexed colors")
    for i in range(66):
        r, c = i % 22 + 1, i // 22 * 2 + 1
        ws.cell(row=r, column=c, value=i)
        ws.cell(row=r, column=c + 1).fill = PatternFill("solid", fgColor=Color(indexed=i))
    ws["H1"] = "Protected, locked and hidden formula cells"
    ws["H2"] = "=1+1"
    ws["H2"].protection = Protection(locked=True, hidden=True)
    return finish(wb, "Borders, fills and colors")


def xlsx_sheets():
    wb = openpyxl.Workbook()
    ws = wb.active
    ws.title = "First"
    ws["A1"] = "The first sheet. The workbook opens on the third sheet."
    ws["A2"] = "A link to another sheet"
    ws["A2"].hyperlink = "#'Right to left'!B2"
    ws["A2"].style = "Hyperlink"
    ws["A3"] = "A web link"
    ws["A3"].hyperlink = "https://example.com/featherweight"
    ws["A3"].style = "Hyperlink"
    ws["A4"] = "A mail link"
    ws["A4"].hyperlink = "mailto:test@example.com"
    ws["A4"].style = "Hyperlink"
    ws.sheet_properties.tabColor = "FFC00000"
    ws = wb.create_sheet("Hidden")
    ws["A1"] = "A hidden sheet"
    ws.sheet_state = "hidden"
    ws = wb.create_sheet("Active, zoomed")
    fill_grid(ws, 10, 5)
    ws.sheet_view.zoomScale = 150
    ws.sheet_view.selection[0].activeCell = "C4"
    ws.sheet_view.selection[0].sqref = "C4:D6"
    ws.sheet_properties.tabColor = Color(theme=5)
    ws = wb.create_sheet("Very hidden")
    ws["A1"] = "A very hidden sheet"
    ws.sheet_state = "veryHidden"
    ws = wb.create_sheet("Right to left")
    fill_grid(ws, 10, 5)
    ws.sheet_view.rightToLeft = True
    ws["B2"] = "مرحبا"
    ws = wb.create_sheet("No gridlines or headings")
    fill_grid(ws, 10, 5)
    ws.sheet_view.showGridLines = False
    ws.sheet_view.showRowColHeaders = False
    ws = wb.create_sheet("Zeros hidden")
    for r in range(1, 6):
        ws.cell(row=r, column=1, value=r - 3)
        ws.cell(row=r, column=2, value=0)
    ws.sheet_view.showZeros = False
    ws = wb.create_sheet("Zoom 50")
    fill_grid(ws, 40, 12)
    ws.sheet_view.zoomScale = 50
    ws = wb.create_sheet("Page layout view")
    fill_grid(ws, 20, 5)
    ws.sheet_view.view = "pageLayout"
    ws = wb.create_sheet("Empty")
    wb.active = 2
    for s in wb.worksheets:
        s.sheet_view.tabSelected = s.title == "Active, zoomed"
    return finish(wb, "Sheets and views")


# ---------- Formulas ----------

# Every function sheet has the same data: numbers in E, text in F, mixed values in G, dates in H,
# an error in J2, and lookup tables in L1:N6 (vertical) and L8:P9 (horizontal).
DATA_NUM = [10, 20, 30, 40.5, -5, 0, 1000000, 2.5]
DATA_TEXT = ["apple", "Banana", "cherry", "date", "Apple", "  spaced   text ", "x1", "Straße"]
DATA_MIXED = [1, "2", True, None, "text", 3.5, False, "4"]
DATA_DATE = [45658, 45688, 45716, 45747, 45777, 45808, 46022, 44927]

FUNCTIONS = {
    "SUM": ["=SUM(E2:E9)", "=SUM(E2,E3,100)", "=SUM(G2:G9)", '=SUM(1,"2",TRUE)', "=SUM(E2:E9,J2)", "=SUM(E10:E12)",
            "=SUM(E:E)", "=SUM(E2:E9 E5:E6)", "=SUM((E2:E3,E5))", "=SUM(0.1,0.2,-0.3)"],
    "IF": ['=IF(E2>15,"big","small")', '=IF(E3>15,"big","small")', '=IF(E7,"nonzero","zero")', '=IF(F2="APPLE",1,2)',
           "=IF(TRUE,E2)", "=IF(FALSE,1)", "=IF(J2>0,1,2)", '=IF(E10,"x","y")', '=IF(E2<10,"low",IF(E2<20,"mid","high"))',
           '=IF("TRUE",1,2)', '=IF(F2,1,2)'],
    "AVERAGE": ["=AVERAGE(E2:E9)", "=AVERAGE(G2:G9)", "=AVERAGE(E10:E12)", '=AVERAGE(1,"2",TRUE)', "=AVERAGE(E2:E9,J2)",
                "=AVERAGE(E2,E5)"],
    "COUNT": ["=COUNT(E2:E9)", "=COUNT(G2:G9)", '=COUNT(1,"2",TRUE,"x")', "=COUNT(E:E)", "=COUNT(J2)", "=COUNT(H2:H9)"],
    "COUNTA": ["=COUNTA(F2:F9)", "=COUNTA(G2:G9)", "=COUNTA(E10:E12)", '=COUNTA(1,"",FALSE)', "=COUNTA(J2,K2)", "=COUNTA(K2:K3)"],
    "COUNTIF": ['=COUNTIF(E2:E9,">15")', '=COUNTIF(F2:F9,"apple")', '=COUNTIF(F2:F9,"a*")', '=COUNTIF(F2:F9,"?????")',
                "=COUNTIF(E2:E9,30)", '=COUNTIF(G2:G9,"2")', '=COUNTIF(E2:E9,"<>0")', '=COUNTIF(F2:F9,"<>apple")',
                '=COUNTIF(G2:G9,"")', '=COUNTIF(H2:H9,">="&DATE(2025,3,1))', '=COUNTIF(F2:F9,"~*")', '=COUNTIF(G2:G9,TRUE)',
                '=COUNTIF(E2:E9,"<=2.5")', '=COUNTIF(F2:F9,"*a*")'],
    "COUNTIFS": ['=COUNTIFS(E2:E9,">0",H2:H9,"<46000")', '=COUNTIFS(F2:F9,"*a*",E2:E9,"<=30")', '=COUNTIFS(E2:E9,">100")',
                 '=COUNTIFS(E2:E9,">0",E2:E9,"<50")'],
    "SUMIF": ['=SUMIF(E2:E9,">15")', '=SUMIF(F2:F9,"apple",E2:E9)', '=SUMIF(F2:F9,"*e*",E2:E9)', '=SUMIF(L2:L6,"c",M2:M6)',
              '=SUMIF(F2:F9,"a*",E2:E3)', '=SUMIF(G2:G9,"2",E2:E9)', '=SUMIF(E2:E9,"<>"&E3)'],
    "SUMIFS": ['=SUMIFS(E2:E9,F2:F9,"<>apple",E2:E9,">0")', '=SUMIFS(M2:M6,L2:L6,">b")', '=SUMIFS(E2:E9,H2:H9,">="&H4)'],
    "AVERAGEIF": ['=AVERAGEIF(E2:E9,">0")', '=AVERAGEIF(F2:F9,"apple",E2:E9)', '=AVERAGEIF(E2:E9,">1E9")'],
    "AVERAGEIFS": ['=AVERAGEIFS(E2:E9,E2:E9,">0",E2:E9,"<100")', '=AVERAGEIFS(M2:M6,L2:L6,"<>c")'],
    "MIN": ["=MIN(E2:E9)", "=MIN(G2:G9)", "=MIN(E10:E12)", '=MIN(1,"0.5",TRUE)', "=MIN(H2:H9)", "=MIN(E2:E9,J2)"],
    "MAX": ["=MAX(E2:E9)", "=MAX(G2:G9)", "=MAX(E10:E12)", '=MAX(-1,"-0.5",FALSE)', "=MAX(H2:H9)", "=MAX(E2,E5,-100)"],
    "ROUND": ["=ROUND(2.5,0)", "=ROUND(-2.5,0)", "=ROUND(1.005,2)", "=ROUND(1234.5678,-2)", "=ROUND(0.285,2)", "=ROUND(E5,0)",
              "=ROUND(2.675,2)", "=ROUND(-0.5,0)", "=ROUND(1/3,10)", '=ROUND("3.14159",2)', "=ROUND(E9,0)", "=ROUND(123456789.123456789,5)"],
    "ROUNDUP": ["=ROUNDUP(3.2,0)", "=ROUNDUP(-3.2,0)", "=ROUNDUP(3.14159,3)", "=ROUNDUP(31415.92654,-2)", "=ROUNDUP(0.1+0.2,1)"],
    "ROUNDDOWN": ["=ROUNDDOWN(3.7,0)", "=ROUNDDOWN(-3.7,0)", "=ROUNDDOWN(3.14159,3)", "=ROUNDDOWN(31415.92654,-2)", "=ROUNDDOWN(1-1E-16,0)"],
    "INT": ["=INT(8.9)", "=INT(-8.9)", '=INT("3.5")', "=INT(E5)", "=INT(H2+0.75)", "=INT(1E15+0.5)"],
    "ABS": ["=ABS(-5)", "=ABS(E6)", '=ABS("-2")', "=ABS(F2)", "=ABS(E10)", "=ABS(-0)"],
    "MOD": ["=MOD(10,3)", "=MOD(-10,3)", "=MOD(10,-3)", "=MOD(5.5,2)", "=MOD(1,0)", "=MOD(E5,7)", "=MOD(H2,7)", "=MOD(3.3,1.1)"],
    "VLOOKUP": ['=VLOOKUP("c",L2:N6,3,FALSE)', '=VLOOKUP("C",L2:N6,2,0)', '=VLOOKUP("z",L2:N6,2,FALSE)', "=VLOOKUP(3.5,M2:N6,2,TRUE)",
                "=VLOOKUP(0,M2:N6,2)", '=VLOOKUP("b*",L2:N6,3,FALSE)', '=VLOOKUP("c",L2:N6,4,FALSE)', "=VLOOKUP(99,M2:N6,2)",
                '=VLOOKUP("c",L2:N6,0,FALSE)'],
    "HLOOKUP": ['=HLOOKUP("c",L8:P9,2,FALSE)', '=HLOOKUP("z",L8:P9,2,FALSE)', '=HLOOKUP("c",L8:P9,3,FALSE)', '=HLOOKUP("d",L8:P9,1)'],
    "INDEX": ["=INDEX(L2:N6,2,3)", "=INDEX(M2:M6,4)", "=SUM(INDEX(L2:N6,0,2))", "=INDEX(L2:N6,6,1)", "=INDEX((L2:L6,N2:N6),2,1,2)",
              "=INDEX(L8:P9,2,5)", "=INDEX(E2:E9,0)"],
    "MATCH": ['=MATCH("c",L2:L6,0)', "=MATCH(3.5,M2:M6,1)", "=MATCH(3.5,M2:M6)", '=MATCH("z",L2:L6,0)', '=MATCH("C*",L2:L6,0)',
              "=MATCH(4,{5,4,3,2,1},-1)", '=MATCH("apple",F2:F9,0)', "=MATCH(0,M2:M6,1)"],
    "XLOOKUP": ['=_xlfn.XLOOKUP("c",L2:L6,N2:N6)', '=_xlfn.XLOOKUP("z",L2:L6,N2:N6,"none")', "=_xlfn.XLOOKUP(3.5,M2:M6,N2:N6,,-1)",
                "=_xlfn.XLOOKUP(3.5,M2:M6,N2:N6,,1)", '=_xlfn.XLOOKUP("apple",F2:F9,E2:E9,,0,-1)', '=_xlfn.XLOOKUP("b*",L2:L6,M2:M6,,2)',
                '=_xlfn.XLOOKUP("z",L2:L6,N2:N6)'],
    "IFERROR": ['=IFERROR(1/0,"div")', '=IFERROR(E2,"x")', "=IFERROR(J2,0)", '=IFERROR(NA(),"na")', '=IFERROR(E10,"x")'],
    "IFNA": ['=_xlfn.IFNA(NA(),"na")', '=_xlfn.IFNA(1/0,"x")', '=_xlfn.IFNA(VLOOKUP("z",L2:N6,2,FALSE),"missing")', '=_xlfn.IFNA(E2,"x")'],
    "AND": ["=AND(TRUE,E2>5)", "=AND(TRUE,FALSE)", "=AND(G2:G9)", "=AND(E10:E12)", '=AND(1,"TRUE")', "=AND(E2,E3,E4)"],
    "OR": ["=OR(FALSE,0,E7)", "=OR(G2:G9)", "=OR(FALSE,E5)", "=OR(E10:E12)", "=OR(J2,TRUE)"],
    "NOT": ["=NOT(E7)", "=NOT(1)", '=NOT("TRUE")', "=NOT(E10)", "=NOT(F2)"],
    "ISBLANK": ["=ISBLANK(E10)", "=ISBLANK(G5)", "=ISBLANK(F2)", '=ISBLANK("")', "=ISBLANK(K2)", "=ISBLANK(J2)"],
    "ISNUMBER": ["=ISNUMBER(E2)", "=ISNUMBER(G3)", "=ISNUMBER(H2)", "=ISNUMBER(J2)", "=ISNUMBER(G4)", '=ISNUMBER("1")'],
    "ISERROR": ["=ISERROR(J2)", "=ISERROR(1/0)", "=ISERROR(NA())", "=ISERROR(E2)", '=ISERROR(VALUE("x"))'],
    "CONCATENATE": ['=CONCATENATE(F2," ",F3)', "=CONCATENATE(E2,E9)", "=CONCATENATE(TRUE,1.5)", "=CONCATENATE(H2)",
                    "=CONCATENATE(0.1+0.2)", "=CONCATENATE(1/3)", "=CONCATENATE(1E20)", "=CONCATENATE(123456789012345678)",
                    "=CONCATENATE(E10,\"|\")", "=CONCATENATE(-0.000001234)"],
    "CONCAT": ["=_xlfn.CONCAT(F2:F4)", '=_xlfn.CONCAT(E2:E4,"-")', "=_xlfn.CONCAT(G2:G9)", "=_xlfn.CONCAT(L2:M3)"],
    "TEXTJOIN": ['=_xlfn.TEXTJOIN(", ",TRUE,F2:F9)', '=_xlfn.TEXTJOIN("-",FALSE,G2:G9)', '=_xlfn.TEXTJOIN("",TRUE,E2:E4,"x")'],
    "LEFT": ["=LEFT(F3,3)", "=LEFT(F3)", "=LEFT(1234.5,3)", "=LEFT(F9,5)", "=LEFT(F3,0)", "=LEFT(F3,-1)", "=LEFT(F3,100)", '=LEFT("𝄞x",1)'],
    "RIGHT": ["=RIGHT(F3,3)", "=RIGHT(F3)", "=RIGHT(1234.5,3)", "=RIGHT(F3,0)", "=RIGHT(F3,-1)", "=RIGHT(TRUE,2)"],
    "MID": ["=MID(F3,2,3)", "=MID(F3,10,2)", "=MID(F3,0,2)", "=MID(F3,2,-1)", "=MID(1234.5,2,3)", "=MID(F9,5,2)"],
    "LEN": ["=LEN(F3)", "=LEN(F7)", "=LEN(1/3)", "=LEN(E10)", "=LEN(TRUE)", '=LEN("𝄞")', "=LEN(F9)", "=LEN(1E20)", "=LEN(J2)"],
    "TRIM": ["=TRIM(F7)", '=TRIM("  a  b  ")', "=TRIM(E2)", '=TRIM("no change")'],
    "UPPER": ["=UPPER(F2)", "=UPPER(F9)", '=UPPER("éàü ñ")', "=UPPER(1.5)"],
    "LOWER": ["=LOWER(F3)", '=LOWER("ÀÉÎ STRASSE")', "=LOWER(TRUE)"],
    "PROPER": ['=PROPER("hello WORLD o\'neil 2nd-place")', "=PROPER(F7)", '=PROPER("ÉCOLE élève")', '=PROPER("a1b2 c3")'],
    "SUBSTITUTE": ['=SUBSTITUTE(F7," ","")', '=SUBSTITUTE("a-b-c","-","+",2)', '=SUBSTITUTE("aaa","a","b",4)', '=SUBSTITUTE("abc","","x")',
                   '=SUBSTITUTE("aaa","aa","b")', '=SUBSTITUTE(1234.5,".",",")', '=SUBSTITUTE("abc","b","x",0)'],
    "FIND": ['=FIND("an",F3)', '=FIND("A",F3)', '=FIND("",F2)', '=FIND("a",F3,3)', '=FIND("a",F3,20)', '=FIND("ß",F9)'],
    "SEARCH": ['=SEARCH("AN",F3)', '=SEARCH("b?n",F3)', '=SEARCH("*a",F3)', '=SEARCH("~?","what?")', '=SEARCH("x",F3)', '=SEARCH("a",F3,3)'],
    "VALUE": ['=VALUE("123")', '=VALUE("123.45")', '=VALUE("1,234.5")', '=VALUE("$1,000")', '=VALUE("50%")', '=VALUE("1E3")',
              '=VALUE("abc")', '=VALUE("2025-01-02")', '=VALUE("12:00")', '=VALUE(" 42 ")', '=VALUE("-7")', '=VALUE("(5)")', "=VALUE(E2)"],
    "TEXT": ['=TEXT(1234.567,"#,##0.00")', '=TEXT(0.25,"0%")', '=TEXT(H2,"yyyy-mm-dd")', '=TEXT(H2,"dddd")', '=TEXT(-5,"0;(0)")',
             '=TEXT(1/3,"# ?/?")', '=TEXT(F2,"@")', '=TEXT(45678.5,"h:mm AM/PM")', '=TEXT(1234.567,"0.0E+00")', '=TEXT(E2,"000")',
             '=TEXT(1234567,"[>=1000000]0.0,,""M"";0")', '=TEXT(H2,"mmm d, yyyy")'],
    "DATE": ["=DATE(2025,1,31)", "=DATE(2025,13,1)", "=DATE(2025,2,30)", "=DATE(2025,0,0)", "=DATE(99,1,1)", "=DATE(1900,2,29)",
             "=DATE(10000,1,1)", "=DATE(-1,1,1)", "=DATE(2025,1,1.9)", "=DATE(1899,12,31)"],
    "YEAR": ["=YEAR(H2)", "=YEAR(60)", '=YEAR("2025-03-04")', "=YEAR(0)", "=YEAR(-1)", "=YEAR(2958465)"],
    "MONTH": ["=MONTH(H3)", "=MONTH(60)", "=MONTH(0)", "=MONTH(-1)", '=MONTH("15 March 2025")'],
    "DAY": ["=DAY(H4)", "=DAY(60)", "=DAY(0)", "=DAY(61)", "=DAY(H2+0.99)"],
    "TODAY": ["=TODAY()", "=TODAY()-TODAY()", "=ISNUMBER(TODAY())", "=TODAY()>DATE(2026,1,1)"],
    "NOW": ["=NOW()", "=INT(NOW())=TODAY()", "=NOW()>=TODAY()"],
    "EDATE": ["=EDATE(H2,1)", "=EDATE(DATE(2025,1,31),1)", "=EDATE(H2,-13)", "=EDATE(H2,1.9)", "=EDATE(DATE(2024,2,29),12)", '=EDATE("x",1)'],
    "EOMONTH": ["=EOMONTH(H2,0)", "=EOMONTH(DATE(2024,1,15),1)", "=EOMONTH(H2,-1)", "=EOMONTH(H2,1.5)", "=EOMONTH(-1,0)"],
    "WEEKDAY": ["=WEEKDAY(H2)", "=WEEKDAY(H2,2)", "=WEEKDAY(H2,3)", "=WEEKDAY(H2,11)", "=WEEKDAY(0)", "=WEEKDAY(60)", "=WEEKDAY(H2,4)",
                "=WEEKDAY(H2,17)"],
    "NETWORKDAYS": ["=NETWORKDAYS(H2,H3)", "=NETWORKDAYS(H3,H2)", "=NETWORKDAYS(H2,H7,H4:H5)", "=NETWORKDAYS(DATE(2025,1,4),DATE(2025,1,5))",
                    "=NETWORKDAYS(H2,H2)", "=NETWORKDAYS(H2,H9)"],
    "SUMPRODUCT": ["=SUMPRODUCT(E2:E5,M2:M5)", '=SUMPRODUCT((F2:F9="apple")*E2:E9)', "=SUMPRODUCT(E2:E3,E2:E4)", "=SUMPRODUCT(--(E2:E9>10))",
                   "=SUMPRODUCT(G2:G9)", "=SUMPRODUCT(E2:E9*1)", "=SUMPRODUCT({1,2,3},{4,5,6})"],
    "ROW": ["=ROW()", "=ROW(E5)", "=ROW(E2:E9)", "=ROW(5:7)", "=SUM(ROW(E2:E4))"],
    "COLUMN": ["=COLUMN()", "=COLUMN(F1)", "=COLUMN(E2:G2)", "=COLUMN(C:D)"],
    "OFFSET": ["=OFFSET(E2,2,0)", "=SUM(OFFSET(E2,0,0,3,1))", "=OFFSET(E2,-5,0)", "=SUM(OFFSET(L2:N6,1,1,2,1))", "=OFFSET(E2,0,0,0,1)"],
    "INDIRECT": ['=INDIRECT("E3")', '=INDIRECT("R3C5",FALSE)', '=SUM(INDIRECT("E2:E4"))', "=INDIRECT(\"'Other sheet'!A1\")",
                 '=INDIRECT("nosuch")', '=INDIRECT("E"&2+1)'],
}


def function_data(ws):
    ws["E1"], ws["F1"], ws["G1"], ws["H1"], ws["J1"], ws["K1"] = "Num", "Text", "Mixed", "Date", "Error", "Empty text"
    for i in range(8):
        ws.cell(row=i + 2, column=5, value=DATA_NUM[i])
        ws.cell(row=i + 2, column=6, value=DATA_TEXT[i])
        ws.cell(row=i + 2, column=7, value=DATA_MIXED[i])
        ws.cell(row=i + 2, column=8, value=DATA_DATE[i]).number_format = "yyyy-mm-dd"
    ws["J2"] = "#DIV/0!"  # an error constant
    ws["K2"] = '=""'       # a formula that returns empty text
    for i, (k, n, t) in enumerate(zip("abcde", range(1, 6), ["one", "two", "three", "four", "five"]), start=2):
        ws.cell(row=i, column=12, value=k)
        ws.cell(row=i, column=13, value=n)
        ws.cell(row=i, column=14, value=t)
    ws["L1"], ws["M1"], ws["N1"] = "Key", "Number", "Name"
    for c, (k, v) in enumerate(zip("abcde", [10, 20, 30, 40, 50]), start=12):
        ws.cell(row=8, column=c, value=k)
        ws.cell(row=9, column=c, value=v)
    for col, w in (("A", 34), ("B", 18), ("F", 16), ("H", 12)):
        ws.column_dimensions[col].width = w


def table_columns(ws, tab):
    """Columns named after the header cells (as Excel requires), and an AutoFilter that leaves
    out the totals row."""
    tab._initialise_columns()
    min_col, min_row, max_col, max_row = range_boundaries(tab.ref)
    for i, col in enumerate(tab.tableColumns):
        col.id = i + 1
        col.name = str(ws.cell(row=min_row, column=min_col + i).value)
    if tab.totalsRowCount:
        tab.autoFilter = AutoFilter(ref=f"{get_column_letter(min_col)}{min_row}:{get_column_letter(max_col)}{max_row - 1}")


def other_sheet(wb):
    ws = wb.create_sheet("Other sheet")
    ws["A1"] = 42
    ws["B2"] = 7
    ws["A3"] = "text on another sheet"


def xlsx_functions():
    wb = openpyxl.Workbook()
    first = True
    for name, formulas in FUNCTIONS.items():
        ws = wb.active if first else wb.create_sheet(name)
        ws.title = name
        first = False
        ws["A1"], ws["B1"] = "Formula", "Result"
        ws["A1"].font = ws["B1"].font = BOLD
        for r, f in enumerate(formulas, start=2):
            ws.cell(row=r, column=1, value=f[1:].replace("_xlfn.", ""))
            ws.cell(row=r, column=2, value=f)
        function_data(ws)
        ws.print_area = f"A1:B{len(formulas) + 1}"
    other_sheet(wb)
    return finish(wb, "One sheet per function")


def xlsx_formulas():
    wb = openpyxl.Workbook()
    ws = wb.active
    ws.title = "Operators"
    function_data(ws)
    ops = ["=1+2*3", "=(1+2)*3", "=-2^2", "=2^-1", "=2^3^2", "=10%", "=50%*E2", '="a"&"b"', "=1&2", '=E2&""', '=1="1"',
           '="a"<"b"', '="A"="a"', '="10"<"9"', '=1<"a"', '="a"<TRUE', "=TRUE>1", "=E10=0", '=E10=""', "=0.1+0.2=0.3",
           "=0.1+0.2-0.3", "=1/3*3=1", "=9.99999999999999E+307*10", '="5"+"5"', '="5"*2', "=TRUE+TRUE", '=""+1', "=E2:E3", "=E2:E9 E5:G5",
           "=-E5", "=+E2", "=--\"3\"", "=E2-E3*E4/E5", "=2^0.5", "=(-8)^(1/3)", "=0^0", "=1/0", "=SUM(E2:E9)/COUNT(E2:E9)",
           "=1E+307*10/10", "=12345678901234567890", "=0.1*3", "=1-0.9", "=100*1.1", "=9.99999999999999E+307*1"]
    ws["A1"], ws["B1"] = "Formula", "Result"
    for r, f in enumerate(ops, start=2):
        ws.cell(row=r, column=1, value=f[1:])
        ws.cell(row=r, column=2, value=f)

    ws = wb.create_sheet("References")
    function_data(ws)
    refs = ["='Other sheet'!A1", "='Other sheet'!B2*2", "=SUM(Jan:Mar!B2)", "=AVERAGE(Jan:Mar!B2:B3)", "=$E$2+E$3+$E4",
            "=TaxRate*100", "=SUM(Numbers)", "=LocalName", "=Twice", "=Empty!A1", "=Empty!A1&\"\"", "=SUM(E:E)", "=SUM(9:9)",
            "=Jan!B2+Feb!B2", "=COUNTA('Other sheet'!A1:A3)", "='Other sheet'!A3", "=SUM(Numbers)*TaxRate", "=INDEX(Numbers,3)"]
    ws["A1"], ws["B1"] = "Formula", "Result"
    for r, f in enumerate(refs, start=2):
        ws.cell(row=r, column=1, value=f[1:])
        ws.cell(row=r, column=2, value=f)
    wb.defined_names["TaxRate"] = DefinedName("TaxRate", attr_text="0.21")
    wb.defined_names["Numbers"] = DefinedName("Numbers", attr_text="References!$E$2:$E$9")
    wb.defined_names["Twice"] = DefinedName("Twice", attr_text="References!$E$2*2")
    ws.defined_names["LocalName"] = DefinedName("LocalName", attr_text="References!$E$3")

    ws = wb.create_sheet("Errors")
    function_data(ws)
    errs = ["=1/0", "=NA()", "=xyzzy", "=SQRT(-1)", '="a"+1', "=E2:E3 F5:F6", "=INDEX(E2:E3,5)", "=#REF!", "=1+NA()", "=J2",
            "=ERROR.TYPE(J2)", "=ERROR.TYPE(NA())", "=ISERROR(J2)", "=IFERROR(1/0,\"caught\")", "=SUM(E2:E9,J2)", "=NA()=NA()",
            "=ISNA(NA())", "=ERROR.TYPE(1)", "=1/0+NA()", "=NA()+1/0"]
    ws["A1"], ws["B1"] = "Formula", "Result"
    for r, f in enumerate(errs, start=2):
        ws.cell(row=r, column=1, value=f[1:])
        ws.cell(row=r, column=2, value=f)
    for r, e in enumerate(["#NULL!", "#DIV/0!", "#VALUE!", "#REF!", "#NAME?", "#NUM!", "#N/A"], start=2):
        ws.cell(row=r, column=4, value=e)  # error constants

    ws = wb.create_sheet("Arrays")
    function_data(ws)
    ws["A1"], ws["B1"] = "Array formula (entered with Ctrl+Shift+Enter)", "Result"
    ws["A2"] = "{=SUM(E2:E5*M2:M5)}"
    ws["B2"] = ArrayFormula("B2", "=SUM(E2:E5*M2:M5)")
    ws["A3"] = "{=E2:E5*2} over B3:B6"
    ws["B3"] = ArrayFormula("B3:B6", "=E2:E5*2")
    ws["A16"] = "{=TRANSPOSE(L2:L6)} over B16:F16 (C16:F16 in the same array)"
    ws["B16"] = ArrayFormula("B16:F16", "=TRANSPOSE(L2:L6)")
    ws["A8"] = "{=MAX(IF(F2:F9=\"apple\",E2:E9))}"
    ws["B8"] = ArrayFormula("B8", '=MAX(IF(F2:F9="apple",E2:E9))')
    ws["A9"] = "{=ROW(E2:E4)} over B9:B12 (one more cell than values: #N/A)"
    ws["B9"] = ArrayFormula("B9:B12", "=ROW(E2:E4)")
    ws["A13"] = "{1,2;3,4} constant over B13:C14"
    ws["B13"] = ArrayFormula("B13:C14", "={1,2;3,4}")

    ws = wb.create_sheet("Structured references")
    header(ws, ["Region", "Qty", "Price", "Amount"])
    for r, (reg, q, p) in enumerate([("North", 4, 2.5), ("South", 7, 1.25), ("East", 2, 10), ("West", 5, 3)], start=2):
        ws.append([reg, q, p, f"=Orders[[#This Row],[Qty]]*Orders[[#This Row],[Price]]"])
    ws.append(["Total", "=SUBTOTAL(109,Orders[Qty])", None, "=SUBTOTAL(109,Orders[Amount])"])
    tab = Table(displayName="Orders", ref="A1:D6", totalsRowCount=1)
    tab.tableStyleInfo = TableStyleInfo(name="TableStyleMedium2", showRowStripes=True)
    table_columns(ws, tab)
    tab.tableColumns[0].totalsRowLabel = "Total"
    tab.tableColumns[1].totalsRowFunction = "sum"
    tab.tableColumns[3].totalsRowFunction = "sum"
    tab.tableColumns[3].calculatedColumnFormula = TableFormula(attr_text="Orders[[#This Row],[Qty]]*Orders[[#This Row],[Price]]")
    ws.add_table(tab)
    ws["F1"], ws["G1"] = "Formula", "Result"
    for r, f in enumerate(["=SUM(Orders[Amount])", "=COUNTA(Orders[Region])", "=SUM(Orders[[#Totals],[Qty]])",
                           "=ROWS(Orders[#Data])", "=INDEX(Orders[Price],2)", "=SUM(Orders[[Qty]:[Price]])",
                           '=SUMIF(Orders[Region],"North",Orders[Amount])', "=Orders[[#Headers],[Price]]"], start=2):
        ws.cell(row=r, column=6, value=f[1:])
        ws.cell(row=r, column=7, value=f)
    ws.column_dimensions["F"].width = 40

    # Filled in by Excel (scripts/make-sheet-files.ps1): shared formulas and dynamic arrays.
    ws = wb.create_sheet("Shared")
    function_data(ws)
    ws["A1"] = "Column B gets =E2*2 down to row 20 and column C =B2+$M$2, both shared, from Excel"
    ws = wb.create_sheet("Dynamic arrays")
    function_data(ws)
    ws["A1"] = "Excel writes spilling formulas here: SEQUENCE, SORT, UNIQUE, FILTER, a range times 2, and one blocked (#SPILL!)"

    other_sheet(wb)
    for name, v in (("Jan", 1), ("Feb", 2), ("Mar", 3)):
        s = wb.create_sheet(name)
        s["B2"], s["B3"] = v, v * 10
    wb.create_sheet("Empty")
    return finish(wb, "Formulas: operators, references, names, errors, arrays")


# ---------- Conditional formats, tables, pictures, notes ----------


def xlsx_conditional():
    wb = openpyxl.Workbook()
    ws = wb.active
    ws.title = "Rules"
    header(ws, ["Cell is", "Formula", "2-color scale", "3-color scale", "Data bar", "Icons", "Top 3", "Bottom 20%",
                "Above average", "Duplicates", "Text contains", "Blanks"])
    vals = [12, 45, 7, 88, 23, 56, 91, 3, 34, 67, 45, 12, 78, 29, 50]
    words = ["apple", "banana", "cherry", "apple", "kiwi", "date", "banana", "fig", "grape", "apple", "lemon",
             "mango", "nut", "olive", "pear"]
    for i, v in enumerate(vals, start=2):
        for c in range(1, 11):
            ws.cell(row=i, column=c, value=v if c != 10 else words[i - 2])
        ws.cell(row=i, column=11, value=words[i - 2])
        ws.cell(row=i, column=12, value=None if i % 4 == 0 else v)
    red = DifferentialStyle(font=Font(color="FF9C0006"), fill=PatternFill(bgColor="FFFFC7CE"))
    green = DifferentialStyle(font=Font(color="FF006100"), fill=PatternFill(bgColor="FFC6EFCE"))
    bold = DifferentialStyle(font=Font(bold=True, italic=True), border=Border(bottom=Side(style="thin", color="FF0000FF")))
    cf = ws.conditional_formatting
    cf.add("A2:A16", CellIsRule(operator="greaterThan", formula=["50"], font=Font(color="FF9C0006"), fill=PatternFill(bgColor="FFFFC7CE")))
    cf.add("A2:A16", CellIsRule(operator="between", formula=["20", "40"], fill=PatternFill(bgColor="FFFFEB9C"), stopIfTrue=True))
    cf.add("B2:B16", FormulaRule(formula=["MOD(B2,2)=0"], fill=PatternFill(bgColor="FFDDEBF7")))
    cf.add("C2:C16", ColorScaleRule(start_type="min", start_color="FFF8696B", end_type="max", end_color="FF63BE7B"))
    cf.add("D2:D16", ColorScaleRule(start_type="min", start_color="FF5A8AC6", mid_type="percentile", mid_value=50,
                                    mid_color="FFFCFCFF", end_type="max", end_color="FFF8696B"))
    cf.add("E2:E16", DataBarRule(start_type="min", end_type="max", color="FF638EC6", showValue=True))
    cf.add("F2:F16", IconSetRule("3TrafficLights1", "percent", [0, 33, 67], showValue=True))
    r = Rule(type="top10", rank=3, dxf=green)
    cf.add("G2:G16", r)
    cf.add("H2:H16", Rule(type="top10", rank=20, percent=True, bottom=True, dxf=red))
    cf.add("I2:I16", Rule(type="aboveAverage", aboveAverage=True, dxf=bold))
    cf.add("J2:J16", Rule(type="duplicateValues", dxf=red))
    cf.add("K2:K16", Rule(type="containsText", operator="containsText", text="an", dxf=green,
                          formula=['NOT(ISERROR(SEARCH("an",K2)))']))
    cf.add("L2:L16", Rule(type="containsBlanks", dxf=DifferentialStyle(fill=PatternFill(bgColor="FFD9D9D9")),
                          formula=["LEN(TRIM(L2))=0"]))
    ws = wb.create_sheet("Icons and bars")
    for c, (iset, pct) in enumerate([("3Arrows", [0, 33, 67]), ("4Rating", [0, 25, 50, 75]), ("5Quarters", [0, 20, 40, 60, 80]),
                                     ("3Symbols", [0, 33, 67]), ("3Flags", [0, 33, 67])], start=1):
        ws.cell(row=1, column=c, value=iset).font = BOLD
        for r in range(2, 12):
            ws.cell(row=r, column=c, value=(r - 2) * 10 - 20)
        col = get_column_letter(c)
        ws.conditional_formatting.add(f"{col}2:{col}11", IconSetRule(iset, "percent", pct, showValue=(c != 3), reverse=(c == 5)))
    ws["G1"] = "Negative bars"
    for r in range(2, 12):
        ws.cell(row=r, column=7, value=(r - 6) * 7)
    ws.conditional_formatting.add("G2:G11", DataBarRule(start_type="num", start_value=-30, end_type="num", end_value=40, color="FF5B9BD5"))
    return finish(wb, "Conditional formats")


def xlsx_tables():
    wb = openpyxl.Workbook()
    ws = wb.active
    ws.title = "Tables"
    styles = [("Sales", "TableStyleMedium9", True, False, True), ("Staff", "TableStyleLight1", False, True, False),
              ("Stock", "TableStyleDark3", True, True, False)]
    row = 1
    for name, style, stripes, cols, totals in styles:
        top = row
        ws.cell(row=row, column=1, value=f"{name} table ({style})").font = BOLD
        row += 1
        hdr = row
        for c, h in enumerate(["Item", "Category", "Qty", "Price", "Amount"], start=1):
            ws.cell(row=row, column=c, value=h)
        for i in range(6):
            row += 1
            ws.cell(row=row, column=1, value=f"{name} item {i + 1}")
            ws.cell(row=row, column=2, value=["A", "B", "C"][i % 3])
            ws.cell(row=row, column=3, value=i * 3 + 1)
            ws.cell(row=row, column=4, value=round(1.25 * (i + 1), 2)).number_format = "#,##0.00"
            ws.cell(row=row, column=5, value=f"={name}[[#This Row],[Qty]]*{name}[[#This Row],[Price]]").number_format = "#,##0.00"
        if totals:
            row += 1
            ws.cell(row=row, column=1, value="Total")
            ws.cell(row=row, column=3, value=f"=SUBTOTAL(109,{name}[Qty])")
            ws.cell(row=row, column=5, value=f"=SUBTOTAL(109,{name}[Amount])").number_format = "#,##0.00"
        tab = Table(displayName=name, ref=f"A{hdr}:E{row}", totalsRowCount=1 if totals else None)
        tab.tableStyleInfo = TableStyleInfo(name=style, showRowStripes=stripes, showColumnStripes=cols,
                                            showFirstColumn=(name == "Stock"), showLastColumn=(name == "Stock"))
        table_columns(ws, tab)
        tab.tableColumns[4].calculatedColumnFormula = TableFormula(attr_text=f"{name}[[#This Row],[Qty]]*{name}[[#This Row],[Price]]")
        if totals:
            tab.tableColumns[0].totalsRowLabel = "Total"
            tab.tableColumns[2].totalsRowFunction = "sum"
            tab.tableColumns[4].totalsRowFunction = "sum"
        ws.add_table(tab)
        row += 3
    ws = wb.create_sheet("No header row")
    for r in range(1, 6):
        ws.append([r, r * r, f"row {r}"])
    tab = Table(displayName="Squares", ref="A1:C5", headerRowCount=0)
    tab.tableColumns = [TableColumn(id=1, name="Column1"), TableColumn(id=2, name="Column2"), TableColumn(id=3, name="Column3")]
    tab.tableStyleInfo = TableStyleInfo(name="TableStyleMedium4", showRowStripes=True)
    ws.add_table(tab)
    for c, w in (("A", 18), ("B", 12), ("E", 12)):
        wb["Tables"].column_dimensions[c].width = w
    return finish(wb, "Tables")


def jpeg(width, height, seed):
    img = Image.new("RGB", (width, height))
    img.putdata([((x * 5 + seed) % 256, (y * 3 + seed) % 256, (x * y) % 256) for y in range(height) for x in range(width)])
    out = io.BytesIO()
    img.save(out, format="JPEG", quality=85)
    return out.getvalue()


def xlsx_pictures():
    wb = openpyxl.Workbook()
    ws = wb.active
    ws.title = "Pictures"
    fill_grid(ws, 40, 12)
    ws["A1"] = "A PNG at B3 (one-cell anchor), a JPEG from E3 to H12 (two-cell anchor), a PNG over hidden column J"
    ws.add_image(XlImage(io.BytesIO(png(160, 100, seed=3))), "B3")
    img = XlImage(io.BytesIO(jpeg(200, 150, seed=5)))
    img.anchor = TwoCellAnchor(editAs="oneCell", _from=AnchorMarker(col=4, row=2, colOff=0, rowOff=0),
                               to=AnchorMarker(col=8, row=12, colOff=0, rowOff=0))
    ws.add_image(img)
    ws.add_image(XlImage(io.BytesIO(png(64, 64, seed=11))), "I15")
    ws.column_dimensions["J"].hidden = True
    ws = wb.create_sheet("Shapes")
    ws["A1"] = "Excel adds shapes here (scripts/make-sheet-files.ps1): rectangle, ellipse, arrow, text box, line, a group."
    return finish(wb, "Pictures and shapes")


def xlsx_notes():
    wb = openpyxl.Workbook()
    ws = wb.active
    ws.title = "Notes"
    fill_grid(ws, 20, 8)
    ws["B2"].comment = Comment("A short note.", "Featherweight tests")
    c = Comment("A longer note, with a second line.\nIt is shown in a bigger box.", "Featherweight tests", width=300, height=120)
    ws["D4"].comment = c
    ws["F10"].comment = Comment("Note on a cell in a hidden row.", "Featherweight tests")
    ws.row_dimensions[10].hidden = True
    ws["A15"].comment = Comment("Ünïcödé nötë: 東京 😀", "Featherweight tests")
    ws = wb.create_sheet("Threaded comments")
    fill_grid(ws, 10, 5)
    ws["A12"] = "Threaded comments on B2 (with a reply) and D5 (resolved), as Excel 365 writes them, each with its note for older Excel."
    for ref, (cid, thread) in THREADS.items():
        lines = ["[Threaded comment]", "", THREAD_NOTICE, ""]
        for i, (_, text, _) in enumerate(thread):
            lines += ["Comment:" if i == 0 else "Reply:", "    " + text]
        ws[ref].comment = Comment("\n".join(lines), f"tc={cid}", width=240, height=110)
    set_core(wb.properties, "Notes and comments")
    return normalize(write_zip(add_threads(read_entries(to_bytes(wb)))))


# Threaded comments (Excel 365): a ThreadedComments part per sheet and a person list per workbook.
# Excel 2021's object model can't write them, so they're written here as Excel 365 does. The ids
# are fixed GUIDs, so the file stays the same each time.
PERSON = "{6B0C3E1A-2F4D-4C5B-9A7E-1D2C3B4A5F60}"
THREADS = {
    "B2": ("{6B0C3E1A-2F4D-4C5B-9A7E-1D2C3B4A5F61}", [("2026-01-01T12:00:00.00", "A threaded comment.", None),
                                                     ("2026-01-01T12:05:00.00", "A reply to it.", "{6B0C3E1A-2F4D-4C5B-9A7E-1D2C3B4A5F62}")]),
    "D5": ("{6B0C3E1A-2F4D-4C5B-9A7E-1D2C3B4A5F63}", [("2026-01-01T12:10:00.00", "A second thread, resolved.", None)]),
}
THREAD_NOTICE = ("Your version of Excel allows you to read this threaded comment; however, any edits to it will get removed "
                 "if the file is opened in a newer version of Excel. Learn more: https://go.microsoft.com/fwlink/?linkid=870924")


def add_threads(entries):
    ns = 'xmlns="http://schemas.microsoft.com/office/spreadsheetml/2018/threadedcomments" xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main"'
    items = []
    for ref, (cid, thread) in THREADS.items():
        for i, (when, text, reply_id) in enumerate(thread):
            ident = cid if i == 0 else reply_id
            parent = f' parentId="{cid}"' if i else ""
            done = ' done="1"' if ref == "D5" and i == 0 else ""
            items.append(f'<threadedComment ref="{ref}" dT="{when}" personId="{PERSON}" id="{ident}"{parent}{done}><text>{text}</text></threadedComment>')
    threads = f'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\r\n<ThreadedComments {ns}>{"".join(items)}</ThreadedComments>'
    person = (f'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\r\n<personList {ns}><person displayName="Featherweight tests" '
              f'id="{PERSON}" userId="Featherweight tests" providerId="None"/></personList>')
    rel_thread = "http://schemas.microsoft.com/office/2017/10/relationships/threadedComment"
    rel_person = "http://schemas.microsoft.com/office/2017/10/relationships/person"
    uids = {cid: ref for ref, (cid, _) in THREADS.items()}

    def fix(name, body):
        text = body.decode("utf-8")
        if name == "[Content_Types].xml":
            text = text.replace("</Types>",
                                '<Override PartName="/xl/threadedComments/threadedComment1.xml" ContentType="application/vnd.ms-excel.threadedcomments+xml"/>'
                                '<Override PartName="/xl/persons/person.xml" ContentType="application/vnd.ms-excel.person+xml"/></Types>')
        elif name == "xl/_rels/workbook.xml.rels":
            text = text.replace("</Relationships>", f'<Relationship Id="rIdPerson" Type="{rel_person}" Target="persons/person.xml"/></Relationships>')
        elif name == "xl/worksheets/_rels/sheet2.xml.rels":
            text = text.replace("</Relationships>", f'<Relationship Id="rIdThreads" Type="{rel_thread}" Target="../threadedComments/threadedComment1.xml"/></Relationships>')
        elif name == "xl/comments/comment2.xml":
            # Each note names its thread, as Excel writes it.
            text = text.replace("<comments ", '<comments xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006" mc:Ignorable="xr" '
                                'xmlns:xr="http://schemas.microsoft.com/office/spreadsheetml/2014/revision" ', 1)
            for cid, ref in uids.items():
                text = re.sub(rf'(<comment ref="{ref}"[^>]*?)(/?>)', rf'\g<1> xr:uid="{cid}"\g<2>', text, count=1)
        return text.encode("utf-8")
    out = [(n, fix(n, b)) for n, b in entries]
    names = [n for n, _ in out]
    for needed in ("xl/worksheets/_rels/sheet2.xml.rels", "xl/comments/comment2.xml"):
        assert needed in names, needed
    return out + [("xl/threadedComments/threadedComment1.xml", threads.encode("utf-8")), ("xl/persons/person.xml", person.encode("utf-8"))]


def chart_data(ws):
    ws.append(["Month", "North", "South", "West"])
    for i, m in enumerate(["Jan", "Feb", "Mar", "Apr", "May", "Jun"]):
        ws.append([m, 10 + i * 3, 14 - i, 8 + (i * 7) % 5])


def add_series(chart, ws, cols=(2, 4), cats=True):
    chart.add_data(Reference(ws, min_col=cols[0], min_row=1, max_col=cols[1], max_row=7), titles_from_data=True)
    if cats:
        chart.set_categories(Reference(ws, min_col=1, min_row=2, max_row=7))
    for ax in (getattr(chart, "x_axis", None), getattr(chart, "y_axis", None)):
        if ax is not None:
            ax.delete = False
    return chart


def xlsx_charts():
    wb = openpyxl.Workbook()
    ws = wb.active
    ws.title = "Charts"
    chart_data(ws)
    place = iter([f"{c}{r}" for r in (9, 25, 41, 57, 73) for c in ("A", "I", "Q")])

    def put(chart, title):
        chart.title = title
        chart.width, chart.height = 14, 7.5
        ws.add_chart(chart, next(place))

    col = add_series(BarChart(), ws)
    col.type = "col"
    put(col, "Clustered column")
    st = add_series(BarChart(), ws)
    st.type, st.grouping, st.overlap = "col", "stacked", 100
    put(st, "Stacked column")
    pct = add_series(BarChart(), ws)
    pct.type, pct.grouping, pct.overlap = "col", "percentStacked", 100
    put(pct, "100% stacked column")
    bar = add_series(BarChart(), ws)
    bar.type = "bar"
    bar.dataLabels = DataLabelList()
    bar.dataLabels.showVal = True
    put(bar, "Clustered bar with data labels")
    line = add_series(LineChart(), ws)
    for s in line.series:
        s.marker.symbol = "circle"
    put(line, "Line with markers")
    pie = add_series(PieChart(), ws, cols=(2, 2))
    pie.dataLabels = DataLabelList()
    pie.dataLabels.showPercent = True
    put(pie, "Pie")
    dn = add_series(DoughnutChart(), ws, cols=(2, 3))
    put(dn, "Doughnut")
    area = add_series(AreaChart(), ws)
    area.grouping = "stacked"
    put(area, "Stacked area")
    sc = ScatterChart()
    sc.style = 13
    xs = Reference(ws, min_col=2, min_row=2, max_row=7)
    for c in (3, 4):
        s = Series(Reference(ws, min_col=c, min_row=1, max_row=7), xs, title_from_data=True)
        s.marker.symbol = "diamond"
        sc.series.append(s)
    sc.x_axis.delete = False
    sc.y_axis.delete = False
    sc.x_axis.title, sc.y_axis.title = "North", "Others"
    put(sc, "Scatter")
    combo = add_series(BarChart(), ws, cols=(2, 3))
    combo.type = "col"
    ln = add_series(LineChart(), ws, cols=(4, 4))
    ln.y_axis.axId = 200
    ln.y_axis.title = "West"
    ln.y_axis.crosses = "max"
    combo += ln
    put(combo, "Combo: columns and a line on a secondary axis")
    radar = add_series(RadarChart(), ws)
    put(radar, "Radar (beyond S1's list: drawn from Excel's fallback, if any)")
    leg = add_series(LineChart(), ws)
    leg.legend.position = "b"
    leg.y_axis.scaling.min, leg.y_axis.scaling.max = 0, 40
    leg.y_axis.majorGridlines = None
    put(leg, "Line, legend at the bottom, fixed axis")

    cs = wb.create_chartsheet("Chart sheet")
    big = add_series(BarChart(), ws)
    big.type = "col"
    big.title = "A chart on its own sheet"
    cs.add_chart(big)
    wb.create_sheet("Sparklines")["A1"] = "Excel adds sparklines here (scripts/make-sheet-files.ps1)."
    return finish(wb, "Charts")


def xlsx_print():
    wb = openpyxl.Workbook()

    def table(ws, rows=120, cols=14):
        header(ws, ["Id"] + [f"Column {c}" for c in range(2, cols + 1)])
        for r in range(2, rows + 2):
            ws.cell(row=r, column=1, value=r - 1)
            for c in range(2, cols + 1):
                ws.cell(row=r, column=c, value=(r * 37 + c * 11) % 1000 / 10)

    ws = wb.active
    ws.title = "Print area and titles"
    table(ws)
    ws.print_area = "A1:J80"
    ws.print_title_rows = "1:1"
    ws.print_title_cols = "A:A"
    ws.oddHeader.center.text = "&A"
    ws.oddFooter.center.text = "Page &P of &N"
    ws = wb.create_sheet("Fit to width, landscape")
    table(ws, 120, 16)
    ws.page_setup.orientation = "landscape"
    ws.page_setup.paperSize = ws.PAPERSIZE_A4
    ws.page_setup.fitToWidth = 1
    ws.page_setup.fitToHeight = 0
    ws.sheet_properties.pageSetUpPr = PageSetupProperties(fitToPage=True)
    ws = wb.create_sheet("Fit to one page")
    table(ws, 90, 16)
    ws.page_setup.paperSize = ws.PAPERSIZE_LETTER
    ws.page_setup.fitToWidth = 1
    ws.page_setup.fitToHeight = 1
    ws.sheet_properties.pageSetUpPr = PageSetupProperties(fitToPage=True)
    ws = wb.create_sheet("Scale, margins, centered")
    table(ws, 40, 6)
    ws.page_setup.scale = 75
    ws.page_margins.left = ws.page_margins.right = 1.5
    ws.page_margins.top = ws.page_margins.bottom = 1.25
    ws.page_margins.header = ws.page_margins.footer = 0.5
    ws.print_options.horizontalCentered = True
    ws.print_options.verticalCentered = True
    ws = wb.create_sheet("Headers and footers")
    table(ws, 100, 8)
    ws.oddHeader.left.text = "&F"
    ws.oddHeader.center.text = '&"Arial,Bold"&14Quarterly report'
    ws.oddHeader.right.text = "&D &T"
    ws.oddFooter.left.text = "&KFF0000Confidential"
    ws.oddFooter.right.text = "Page &P of &N"
    ws.HeaderFooter.differentFirst = True
    ws.firstHeader.center.text = "First page header"
    ws.HeaderFooter.differentOddEven = True
    ws.evenFooter.left.text = "Even page &P"
    ws.page_setup.firstPageNumber = 5
    ws.page_setup.useFirstPageNumber = True
    ws = wb.create_sheet("Gridlines and headings")
    table(ws, 60, 8)
    ws.print_options.gridLines = True
    ws.print_options.headings = True
    ws = wb.create_sheet("Page breaks, order")
    table(ws, 70, 18)
    ws.row_breaks.append(Break(id=25))
    ws.row_breaks.append(Break(id=60))
    ws.col_breaks.append(Break(id=6))
    ws.page_setup.pageOrder = "overThenDown"
    ws = wb.create_sheet("Not printed")
    ws["A1"] = "An empty print: this sheet has one cell."
    return finish(wb, "Print settings")


# ---------- CSV ----------

CSV_FILES = {}


def csv_file(name, rows, encoding="utf-8", bom=False, delimiter=",", newline="\r\n", final_newline=True,
             quote_all=False, raw=None, note=None, rule=None):
    """Writes rows as Excel would (fields quoted when they hold the delimiter, a quote or a line
    break), or `raw` text as it is, and records what a reader must find in csv/expected.json."""
    if raw is None:
        def q(f):
            if quote_all or any(ch in f for ch in (delimiter, '"', "\r", "\n")):
                return '"' + f.replace('"', '""') + '"'
            return f
        text = newline.join(delimiter.join(q(f) for f in row) for row in rows)
        if final_newline and rows:
            text += newline
    else:
        text = raw
    data = text.encode(encoding)
    if bom:
        data = codecs.BOM_UTF8 + data if encoding == "utf-8" else data
    save(CSV, name, data)
    entry = {"encoding": encoding, "bom": bom, "delimiter": delimiter,
             "lineEnding": {"\r\n": "CRLF", "\n": "LF", "\r": "CR"}.get(newline, newline)}
    if note:
        entry["note"] = note
    if rule:
        entry["rule"] = rule
    else:
        entry["rows"] = rows
    CSV_FILES[name] = entry


def utf16(text, big_endian=False):
    return (codecs.BOM_UTF16_BE + text.encode("utf-16-be")) if big_endian else (codecs.BOM_UTF16_LE + text.encode("utf-16-le"))


def make_csv():
    people = [["Name", "City", "Amount", "Date", "Note"],
              ["Zoë Ångström", "Malmö", "1234.5", "2025-01-31", "Café crème"],
              ["José Núñez", "São Paulo", "-17", "2025-02-28", "naïve façade"],
              ["Jürgen Groß", "Köln", "0.25", "2025-03-01", "Straße"],
              ["山田 太郎", "東京", "1000000", "2025-12-31", "日本語のテキスト"],
              ["Ελένη", "Αθήνα", "3.14159", "2026-01-01", "emoji 😀 and 🇳🇱"],
              ["سارة", "القاهرة", "42", "2026-06-15", "right to left"]]
    csv_file("utf8.csv", people, note="UTF-8 without a byte-order mark")
    csv_file("utf8-bom.csv", people, bom=True, note="UTF-8 with a byte-order mark, as Excel writes CSV UTF-8")
    for name, be, delim in (("utf16le.csv", False, ","), ("utf16be.csv", True, ","), ("utf16le.tsv", False, "\t")):
        text = "\r\n".join(delim.join(r) for r in people) + "\r\n"
        data = utf16(text, be)
        save(CSV, name, data)
        CSV_FILES[name] = {"encoding": "utf-16-be" if be else "utf-16-le", "bom": True, "delimiter": delim,
                           "lineEnding": "CRLF", "rows": people,
                           "note": "UTF-16 with a byte-order mark" + (", tab-delimited as Excel's Unicode Text" if delim == "\t" else "")}
    ansi = [["Product", "Price", "Note"], ["Crème brûlée", "€4,50", "“quoted” – dash … ellipsis"],
            ["Müsli", "€3,20", "‘single’ quotes and ™"], ["Smørrebrød", "€12,00", "Œuvre, ½ portion, ±5 °C"]]
    csv_file("ansi-1252.csv", ansi, encoding="cp1252", note="Windows-1252 (ANSI) with characters from 0x80-0x9F")
    euro = [["Artikel", "Aantal", "Prijs", "Datum", "Totaal"], ["Appels", "4", "0,50", "31-12-2025", "2,00"],
            ["Kaas", "1", "1.234,56", "01-02-2026", "1.234,56"], ["Brood; vers", "2", "2,10", "15-03-2026", "4,20"],
            ["Melk", "-3", "-1,05", "29-02-2024", "3,15"]]
    csv_file("semicolon.csv", euro, delimiter=";", note="semicolon-delimited, decimal commas, day-first dates (as Dutch Excel writes)")
    hint = "sep=;\r\n" + "\r\n".join(";".join(r) for r in euro[:3]) + "\r\n"
    csv_file("sep-hint.csv", euro[:3], raw=hint, delimiter=";",
             note="Excel's sep= line names the delimiter; it is not a row (Excel doesn't show it)")
    grid = [["id", "name", "score", "passed"], ["1", "alpha", "9.5", "TRUE"], ["2", "beta", "7.25", "FALSE"],
            ["3", "gamma, delta", "10", "true"], ["4", "", "0", ""]]
    csv_file("tab.tsv", grid, delimiter="\t")
    csv_file("pipe.csv", grid, delimiter="|", note="pipe-delimited")
    quoted = [["Field", "Value"], ["comma", "a, b, c"], ["quotes", 'She said "hello"'], ["only quotes", '""'],
              ["LF inside", "line one\nline two"], ["CRLF inside", "line one\r\nline two\r\nline three"],
              ["spaces kept", "  padded  "], ["empty quoted", ""], ["delimiter only", ","], ["quote at start", '"start']]
    csv_file("quoted.csv", quoted, note="fields quoted when needed; line breaks inside quotes stay in the value")
    raw = ('a,"b",c\r\n'
           '"x""y",z""w,"multi\r\nline"\r\n'
           '"",,""\r\n'
           'a"b,c\r\n')
    csv_file("quote-corners.csv", [["a", "b", "c"], ['x"y', 'z""w', "multi\r\nline"], ["", "", ""], ['a"b', "c"]], raw=raw,
             note='a quote inside an unquoted field is a literal character (Excel keeps z""w as it is)')
    simple = [["a", "b", "c"], ["1", "2", "3"], ["x", "y", "z"]]
    csv_file("lf.csv", simple, newline="\n", note="LF line ends")
    csv_file("cr.csv", simple, newline="\r", note="CR line ends (classic Mac OS)")
    csv_file("mixed-endings.csv", simple, raw="a,b,c\r\n1,2,3\nx,y,z\r", note="CRLF, LF and CR in one file")
    csv_file("no-final-newline.csv", simple, final_newline=False)
    zeros = [["Kind", "Value"], ["zip code", "01234"], ["phone", "0612345678"], ["agent", "007"], ["padded id", "000123"],
             ["zero", "0"], ["double zero", "00"], ["half", "0.50"], ["negative zero", "-0"], ["international", "+31 6 1234 5678"],
             ["hex", "0x1F"], ["one point zero", "1.0"], ["trailing zero", "1.50"], ["ten ten", "10.10"],
             ["plus sign", "+5"], ["leading dot", ".5"], ["trailing dot", "5."]]
    csv_file("leading-zeros.csv", zeros, note="each value must show exactly as written")
    longs = [["Kind", "Value"], ["15 digits", "123456789012345"], ["16 digits", "1234567890123456"],
             ["test card number", "4111111111111111"], ["19 digits", "1234567890123456789"], ["20 digits", "12345678901234567890"],
             ["30 digits", "123456789012345678901234567890"], ["IBAN (example)", "NL91ABNA0417164300"], ["EAN-13", "8712345678906"],
             ["ISBN-13", "9780306406157"], ["many decimals", "0.1234567890123456789"], ["big float", "1.23456789012345678E+20"],
             ["scientific", "1E5"], ["small scientific", "1e-3"], ["negative 17 digits", "-12345678901234567"]]
    csv_file("long-numbers.csv", longs, note="16 or more significant digits stay text; 15 or fewer may become numbers only if they read back the same")
    datelike = [["Kind", "Value"]] + [[k, v] for k, v in [
        ("day-month", "1-2"), ("slash", "1/2"), ("gene MAR1", "MAR1"), ("gene SEPT2", "SEPT2"), ("gene MARCH1", "MARCH1"),
        ("gene DEC1", "DEC1"), ("gene OCT4", "OCT4"), ("three parts", "1-2-3"), ("day-mon", "2-Mar"), ("mon-day", "Mar-2"),
        ("month/day", "12/31"), ("day/month", "31/12"), ("ISO date", "2025-01-02"), ("US date", "01/02/2025"),
        ("impossible date", "13/13/2025"), ("time", "1:30"), ("long time", "25:00"), ("AM time", "10:00 AM"),
        ("scientific-like", "1e5"), ("capital E", "3E4"), ("accounting negative", "(123)"), ("dollars", "$1,000"),
        ("euros", "€1.000"), ("thousands comma", "1,000"), ("thousands dot", "1.000"), ("percent", "50%"),
        ("TRUE", "TRUE"), ("false", "false"), ("yes", "yes"), ("error text", "#N/A"), ("div error text", "#DIV/0!"),
        ("spaces around", " 12 "), ("trailing space", "12 "), ("inner space", "1 2"), ("fractions", "1/2/3"), ("4/5/6", "4/5/6"),
        ("fraction", "1 1/2"), ("version", "1.2.3"), ("ip address", "192.168.1.1"), ("year only", "2025"), ("quarter", "Q1-2025")]]
    csv_file("date-like.csv", datelike, note="codes that look like dates, times or numbers")
    formulas = [["Kind", "Value"], ["equals", "=1+2"], ["plus", "+1+2"], ["minus", "-1+2"], ["at", "@SUM(1,2)"],
                ["function", "=SUM(1,2)"], ["link", '=HYPERLINK("https://example.com","link")'], ["reference", "=A1"],
                ["tab first", "\t=1+2"]]
    csv_file("formulas-text.csv", formulas, note="formula-like text stays text and is never calculated (nothing active)")
    ragged = [["a", "b", "c"], ["1", "2"], ["1", "2", "3", "4", "5"], [""], ["x", "", "", ""], ["", "", "", ""], ["last"]]
    csv_file("ragged.csv", ragged, raw="a,b,c\r\n1,2\r\n1,2,3,4,5\r\n\r\nx,,,\r\n,,,\r\nlast\r\n",
             note="rows of different lengths, an empty line, trailing delimiters")
    csv_file("empty.csv", [], raw="", note="an empty file")
    csv_file("header-only.csv", [["only", "a", "header"]])
    csv_file("one-column.csv", [["value"], ["1"], ["two"], ["3.5"]], note="no delimiter anywhere: one column")
    csv_file("malformed.csv", [["a", "b"], ["1", "unclosed quote,2\r\n3,4\r\n"]], raw='a,b\r\n1,"unclosed quote,2\r\n3,4\r\n',
             note="an opening quote that never closes: the rest of the file is that field (as Excel reads it)")
    nums = [["Kind", "Value"], ["integer", "42"], ["negative", "-5"], ["decimal", "3.75"], ["thousands, quoted", "1,234.56"],
            ["scientific", "1.5E+3"], ["percent", "12.5%"], ["currency", "$19.99"], ["US date", "12/31/2025"],
            ["time", "1:30 PM"], ["date and time", "12/31/2025 23:59"], ["big", "999999999999999"]]
    csv_file("numbers-us.csv", nums, note="values in en-US form")
    def wide(n):
        return ",".join(f"c{c}" if c in (1, 16384, 16385, n) else str(c % 10) for c in range(1, n + 1)) + "\r\n"
    rule = "2 rows, the same: field c (from 1) is 'c' followed by c when c is {}, else str(c % 10)"
    csv_file("wide-16384.csv", None, raw=wide(16384) * 2, rule=rule.format("1 or 16384"),
             note="exactly as many columns as a sheet has (XFD)")
    csv_file("wide-16500.csv", None, raw=wide(16500) * 2, rule=rule.format("1, 16384, 16385 or 16500"),
             note="more columns than a sheet has: fields after column XFD don't fit")
    long_field = "x" * 40000
    csv_file("long-field.csv", None, raw="short,long\r\n" + "a," + long_field + "\r\n",
             rule="row 1: short, long; row 2: a, then 40000 times x",
             note="a field longer than a cell holds (32767 characters)")
    tall = [["n", "square"]] + [[str(i), str(i * i)] for i in range(1, 2001)]
    csv_file("tall.csv", tall, newline="\n", note="2000 rows")

    with open(os.path.join(CSV, "expected.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump({"comment": "What a reader must find in each file: encoding, byte-order mark, delimiter, line ends and every "
                              "field's exact text (rows), or a rule for the big ones. Made by make_sheet_tests.py.",
                   "files": CSV_FILES}, f, ensure_ascii=False, indent=1)
        f.write("\n")
    print("  csv/expected.json")


def scrub(path):
    """Takes the saving PC's traces out of a workbook Excel saved (scripts/make-sheet-files.ps1):
    the folder it was saved in, the author, and the threaded comments' person (name and account).
    Every other part keeps its bytes, and the zip keeps its order and dates."""
    name = "Featherweight tests"
    fixes = {
        "docProps/core.xml": [(rb"(<dc:creator>)[^<]*(</dc:creator>)", rb"\g<1>" + name.encode() + rb"\g<2>"),
                              (rb"(<cp:lastModifiedBy>)[^<]*(</cp:lastModifiedBy>)", rb"\g<1>" + name.encode() + rb"\g<2>")],
        "xl/workbook.xml": [(rb'(<x15ac:absPath\b[^>]*\burl=")[^"]*(")', rb"\g<1>C:\\Featherweight\\\g<2>")],
        "docProps/app.xml": [(rb"(<Company>)[^<]*(</Company>)", rb"\g<1>\g<2>"), (rb"(<Manager>)[^<]*(</Manager>)", rb"\g<1>\g<2>")],
    }
    person = [(rb'\bdisplayName="[^"]*"', b'displayName="' + name.encode() + b'"'),
              (rb'\buserId="[^"]*"', b'userId="' + name.encode() + b'"'),
              (rb'\bproviderId="[^"]*"', b'providerId="None"')]
    with open(path, "rb") as f:
        data = f.read()
    out = io.BytesIO()
    with zipfile.ZipFile(io.BytesIO(data)) as zin, zipfile.ZipFile(out, "w") as zout:
        for info in zin.infolist():
            body = zin.read(info.filename)
            for pattern, repl in fixes.get(info.filename, []) + (person if info.filename.startswith("xl/persons/") else []):
                body = re.sub(pattern, repl, body)
            copy = zipfile.ZipInfo(info.filename, date_time=info.date_time)
            copy.compress_type = info.compress_type
            copy.external_attr = info.external_attr
            zout.writestr(copy, body)
    with open(path, "wb") as f:
        f.write(out.getvalue())
    print(f"  scrubbed {os.path.relpath(path, ROOT)}")


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--scrub":
        for p in sys.argv[2:]:
            scrub(p)
        return
    print("Workbooks:")
    for name, fn in [("numfmt.xlsx", xlsx_numfmt), ("dates-1900.xlsx", lambda: xlsx_dates(False)),
                     ("dates-1904.xlsx", lambda: xlsx_dates(True)), ("merged-frozen.xlsx", xlsx_merged_frozen),
                     ("rows-columns.xlsx", xlsx_rows_columns), ("text.xlsx", xlsx_text), ("borders-fills.xlsx", xlsx_borders_fills),
                     ("sheets.xlsx", xlsx_sheets), ("functions.xlsx", xlsx_functions), ("formulas.xlsx", xlsx_formulas),
                     ("conditional-formats.xlsx", xlsx_conditional), ("tables.xlsx", xlsx_tables),
                     ("pictures-shapes.xlsx", xlsx_pictures), ("notes.xlsx", xlsx_notes), ("charts.xlsx", xlsx_charts),
                     ("print-settings.xlsx", xlsx_print)]:
        save(SHEET, name, fn())
    print("CSV:")
    make_csv()


if __name__ == "__main__":
    main()
