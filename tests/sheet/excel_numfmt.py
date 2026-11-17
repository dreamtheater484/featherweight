"""Ask Excel how it shows values in number formats (s1-numfmt's reference).

    python tests/sheet/excel_numfmt.py probe <cases.json> <out.json>
    python tests/sheet/excel_numfmt.py table <cases.txt> <out.inc>
    python tests/sheet/excel_numfmt.py typed <typed_cases.json> <out.inc>

`probe` reads a JSON list of [code, value] or [code, value, width]: value is a number, or a string
(a text value). It writes, for each, Excel's displayed text (Range.Text) in a column `width`
characters wide (default 255, the widest), which follows Windows' regional settings, and the TEXT
function's result called in English (LCID 1033), which is what en-US shows; or "!invalid" when
Excel refuses the code.

`table` reads the case list of tests/sheet/numfmt_cases.txt (one case per line: the code or
builtin:<id>, a tab, the value, in quotes for text, and optionally a tab and 1904; lines starting with
# are comments), shows them in a generated workbook, and writes tests/unit/sheet_numfmt_table.inc,
which tests/unit/test_sheet_numfmt.cpp checks the engine against.

Excel runs as a new, separate instance (DispatchEx), invisible, with alerts, events and macros off,
and only that instance is ended, so an Excel Roy has open is never touched. Windows' regional
settings decide Range.Text's separators, short date and AM/PM (this PC: en-NL, so "1.234,50",
dd/mm/yyyy and "am"); TEXT called in English shows en-US's ("1,234.50", "PM").
"""
import json
import sys
import time

import pythoncom
import win32api
import win32com.client
import win32com.client.dynamic
import win32con
import win32process

# Every call in English (LCID 1033). Excel reads format codes in the caller's language: with the
# default (0) it takes the user's, which on a Dutch-region PC means "Standaard", "jjjj" and ","
# as the decimal separator. pywin32's property writes always pass 0, hence put() below.
win32com.client.dynamic.LCID = 0x409


def put(obj, name, value):
    o = obj._oleobj_
    o.Invoke(o.GetIDsOfNames(name), 0x409, pythoncom.DISPATCH_PROPERTYPUT, 0, value)


def get(obj, name):
    o = obj._oleobj_
    return o.Invoke(o.GetIDsOfNames(name), 0x409, pythoncom.DISPATCH_PROPERTYGET, 1)


class Excel:
    def __enter__(self):
        pythoncom.CoInitialize()
        self.xl = win32com.client.DispatchEx("Excel.Application")
        _, self.pid = win32process.GetWindowThreadProcessId(self.xl.Hwnd)
        self.xl.Visible = False
        self.xl.DisplayAlerts = False
        self.xl.EnableEvents = False
        self.xl.ScreenUpdating = False
        self.xl.AutomationSecurity = 3  # msoAutomationSecurityForceDisable
        self.wb = self.xl.Workbooks.Add()
        self.ws = self.wb.Worksheets(1)
        return self

    def __exit__(self, *exc):
        try:
            self.wb.Close(False)
        except Exception:
            pass
        try:
            self.xl.Quit()
        except Exception:
            pass
        self.ws = self.wb = self.xl = None
        pythoncom.CoUninitialize()
        # End only the Excel this script started, if Quit left it running.
        for _ in range(50):
            try:
                h = win32api.OpenProcess(win32con.SYNCHRONIZE | win32con.PROCESS_TERMINATE, False, self.pid)
            except Exception:
                return
            try:
                import win32event
                if win32event.WaitForSingleObject(h, 100) == win32event.WAIT_OBJECT_0:
                    return
            finally:
                win32api.CloseHandle(h)
        try:
            h = win32api.OpenProcess(win32con.PROCESS_TERMINATE, False, self.pid)
            win32api.TerminateProcess(h, 1)
            win32api.CloseHandle(h)
        except Exception:
            pass

    def show(self, cases):
        """[(code, value, width)] -> [(text, textfn)]"""
        ws = self.ws
        widths = {}
        out = []
        for i, case in enumerate(cases):
            code, value = case[0], case[1]
            width = case[2] if len(case) > 2 else 255
            col = widths.setdefault(width, len(widths) + 1)
            if col == len(widths):
                ws.Columns(col).ColumnWidth = width
            cell = ws.Cells(i + 1, col)
            try:
                if isinstance(value, str):
                    put(cell, "NumberFormat", "@")
                    put(cell, "Value2", value)
                    put(cell, "NumberFormat", code)
                else:
                    put(cell, "NumberFormat", code)
                    put(cell, "Value2", float(value))
                text = get(cell, "Text")
            except Exception:
                out.append(("!invalid", "!invalid"))
                continue
            try:
                fn = self.xl.WorksheetFunction.Text(value, code)
            except Exception:
                fn = "!error"
            out.append((text, fn))
        return out

    def show_file(self, cases, date1904=False, width=255):
        """[(format, value)] -> [(text, Excel's code in English)], from a workbook file, as Excel
        shows a file: format is a built-in id (int) or a code (str, stored as a custom format)."""
        import os
        import tempfile
        folder = tempfile.mkdtemp(prefix="fw-numfmt-")
        path = os.path.join(folder, "cases.xlsx")
        write_xlsx(path, cases, date1904, width)
        wb = self.xl.Workbooks.Open(path, 0, True)
        try:
            ws = wb.Worksheets(1)
            out = []
            for i in range(len(cases)):
                cell = ws.Cells(i + 1, 1)
                out.append((get(cell, "Text"), get(cell, "NumberFormat")))
            return out
        finally:
            wb.Close(False)
            os.remove(path)
            os.rmdir(folder)


def write_xlsx(path, cases, date1904=False, width=255):
    """A minimal workbook: one cell per case in column A, `width` characters wide."""
    import zipfile
    from xml.sax.saxutils import escape, quoteattr
    main = "http://schemas.openxmlformats.org/spreadsheetml/2006/main"
    rel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
    customs, ids = {}, []
    for fmt, _ in cases:
        if isinstance(fmt, int):
            ids.append(fmt)
        else:
            ids.append(customs.setdefault(fmt, 164 + len(customs)))
    numfmts = "".join('<numFmt numFmtId="%d" formatCode=%s/>' % (i, quoteattr(c)) for c, i in customs.items())
    styles = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<styleSheet xmlns="%s">' % main +
              ('<numFmts count="%d">%s</numFmts>' % (len(customs), numfmts) if customs else "") +
              '<fonts count="1"><font><sz val="11"/><name val="Calibri"/><family val="2"/><scheme val="minor"/></font></fonts>'
              '<fills count="2"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill></fills>'
              '<borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders>'
              '<cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>'
              '<cellXfs count="%d"><xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/>' % (len(ids) + 1) +
              "".join('<xf numFmtId="%d" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>' % n for n in ids) +
              '</cellXfs><cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles></styleSheet>')
    rows = []
    for i, (_, value) in enumerate(cases):
        r = i + 1
        if isinstance(value, str):
            cell = '<c r="A%d" s="%d" t="inlineStr"><is><t xml:space="preserve">%s</t></is></c>' % (r, r, escape(value))
        else:
            cell = '<c r="A%d" s="%d"><v>%s</v></c>' % (r, r, repr(float(value)))
        rows.append('<row r="%d">%s</row>' % (r, cell))
    sheet = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<worksheet xmlns="%s">' % main +
             '<cols><col min="1" max="1" width="%s" customWidth="1"/></cols>' % width +
             "<sheetData>%s</sheetData></worksheet>" % "".join(rows))
    workbook = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<workbook xmlns="%s" xmlns:r="%s">' % (main, rel) +
                ('<workbookPr date1904="1"/>' if date1904 else "") +
                '<sheets><sheet name="Sheet1" sheetId="1" r:id="rId1"/></sheets></workbook>')
    types = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
             '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
             '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
             '<Default Extension="xml" ContentType="application/xml"/>'
             '<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>'
             '<Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>'
             '<Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>'
             '</Types>')
    pkg_rels = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
                '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                '<Relationship Id="rId1" Type="%s/officeDocument" Target="xl/workbook.xml"/></Relationships>' % rel)
    wb_rels = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
               '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
               '<Relationship Id="rId1" Type="%s/worksheet" Target="worksheets/sheet1.xml"/>'
               '<Relationship Id="rId2" Type="%s/styles" Target="styles.xml"/></Relationships>' % (rel, rel))
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", types)
        z.writestr("_rels/.rels", pkg_rels)
        z.writestr("xl/workbook.xml", workbook)
        z.writestr("xl/_rels/workbook.xml.rels", wb_rels)
        z.writestr("xl/styles.xml", styles)
        z.writestr("xl/worksheets/sheet1.xml", sheet)


def probe_file(src, dst, date1904=False):
    with open(src, encoding="utf-8") as f:
        cases = json.load(f)
    with Excel() as xl:
        res = xl.show_file([(c[0], c[1]) for c in cases], date1904)
    with open(dst, "w", encoding="utf-8") as f:
        json.dump([{"format": c[0], "value": c[1], "text": t, "code": code} for c, (t, code) in zip(cases, res)], f,
                  ensure_ascii=False, indent=0)


def probe(src, dst):
    with open(src, encoding="utf-8") as f:
        cases = json.load(f)
    with Excel() as xl:
        res = xl.show(cases)
    with open(dst, "w", encoding="utf-8") as f:
        json.dump([{"code": c[0], "value": c[1], "text": t, "fn": fn} for c, (t, fn) in zip(cases, res)], f,
                  ensure_ascii=False, indent=0)


def read_cases(path):
    """[(format, value, date1904)]: format is a code, or an int for builtin:<id>."""
    cases = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\r\n")
            # A comment has no tab: "#,##0<tab>1234.5" is a case.
            if not line or (line.startswith("#") and "\t" not in line):
                continue
            fields = line.split("\t")
            code, value = fields[0], fields[1]
            fmt = int(code[8:]) if code.startswith("builtin:") else code
            if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
                value = value[1:-1]
            else:
                value = float(value)
            cases.append((fmt, value, len(fields) > 2 and fields[2] == "1904"))
    return cases


def cpp_string(s):
    if s is None:
        return "nullptr"
    out = 'L"'
    for ch in s:
        o = ord(ch)
        if ch == '"' or ch == "\\":
            out += "\\" + ch
        elif 32 <= o < 127:
            out += ch
        elif o < 0x10000:
            out += '\\x%04X" L"' % o  # a new literal, so a following hex digit isn't eaten
        else:
            out += "\\U%08X" % o
    return out + '"'


def table(src, dst):
    cases = read_cases(src)
    # A workbook holds about 250 custom formats, so the cases go in chunks of at most 120 codes, each
    # checked in a new workbook (Excel refuses a whole file for one code it can't read).
    chunks, codes = [[]], set()
    for case in cases:
        if isinstance(case[0], str) and case[0] not in codes and len(codes) >= 120:
            chunks.append([])
            codes = set()
        if isinstance(case[0], str):
            codes.add(case[0])
        chunks[-1].append(case)
    rows = []
    with Excel() as xl:
        for chunk in chunks:
            book = xl.xl.Workbooks.Add()
            cell = book.Worksheets(1).Cells(1, 1)
            valid = []
            for fmt, value, d1904 in chunk:
                if isinstance(fmt, str):
                    try:
                        put(cell, "NumberFormat", fmt)
                    except Exception:
                        print("Excel refuses %r: left out" % fmt)
                        continue
                valid.append((fmt, value, d1904))
            book.Close(False)
            for d1904 in (False, True):
                part = [c for c in valid if c[2] == d1904]
                if not part:
                    continue
                shown = xl.show_file([(f, v) for f, v, _ in part], d1904)
                for (fmt, value, _), (text, english) in zip(part, shown):
                    us = None
                    # TEXT() shows [$-F800] and [$-F400] in Windows' own patterns, not en-US's.
                    system = isinstance(fmt, str) and any(t in fmt.upper() for t in ("F800", "F400", "X-SYS"))
                    if not d1904 and not system:
                        try:
                            us = xl.xl.WorksheetFunction.Text(value, english if isinstance(fmt, int) else fmt)
                        except Exception:
                            us = "!error"
                    rows.append((fmt, value, d1904, text, us))
    lines = ["// Generated by tests/sheet/excel_numfmt.py from tests/sheet/numfmt_cases.txt. Do not edit by hand.",
             "// Excel 2021 (16.0.20326) on the reference PC: `nl` is the cell's Range.Text in a 255-character",
             "// column with Windows set to en-NL (fills repeat to the width; \"####\" when it can't be shown); `us`",
             "// is TEXT() called in English, which shows en-US (\"!error\": it can't be shown; nullptr: not asked).",
             "// {code, built-in id (-1: the code), 1904, number, text value (nullptr: the number), nl, us}"]
    for fmt, value, d1904, text, us in rows:
        code = "" if isinstance(fmt, int) else fmt
        bid = fmt if isinstance(fmt, int) else -1
        num, txt = (0.0, value) if isinstance(value, str) else (value, None)
        lines.append("{%s, %d, %s, %s, %s, %s, %s}," % (cpp_string(code), bid, "true" if d1904 else "false",
                                                    repr(float(num)), cpp_string(txt), cpp_string(text), cpp_string(us)))
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print(len(rows), "cases")


# The built-in formats Excel gives typed values, by the code it reports in English.
TYPED_FORMATS = {"General": 0, "#,##0": 3, "#,##0.00": 4, "$#,##0_);[Red]($#,##0)": 6,
                 "$#,##0.00_);[Red]($#,##0.00)": 8, "0%": 9, "0.00%": 10, "0.00E+00": 11, "# ?/?": 12, "# ??/??": 13,
                 "m/d/yyyy": 14, "d-mmm-yy": 15, "d-mmm": 16, "mmm-yy": 17, "h:mm AM/PM": 18, "h:mm:ss AM/PM": 19,
                 "h:mm": 20, "h:mm:ss": 21, "m/d/yyyy h:mm": 22, "[h]:mm:ss": 46, "mm:ss.0": 47}


def typed(src, dst):
    """Types each string of tests/sheet/typed_cases.json into a General cell as a user does
    (FormulaLocal, Windows' region) and writes what Excel made of it for tests/unit/test_sheet_typed.cpp."""
    import datetime
    with open(src, encoding="utf-8") as f:
        cases = json.load(f)
    rows = []
    with Excel() as xl:
        cell = xl.ws.Cells(1, 1)
        xl.ws.Columns(1).ColumnWidth = 60  # error names show whole
        for text in cases:
            cell.Clear()
            cell.FormulaLocal = text
            v = cell.Value2
            code = get(cell, "NumberFormat")
            if code not in TYPED_FORMATS:
                print("left out %r: format %r" % (text, code))
                continue
            if isinstance(v, bool):
                kind, number, shown = "Bool", 1.0 if v else 0.0, None
            elif isinstance(v, float):
                kind, number, shown = "Number", v, None
            elif isinstance(v, int):
                kind, number, shown = "Error", 0.0, get(cell, "Text")
            else:
                kind, number, shown = "Text", 0.0, v
            rows.append((text, kind, number, shown, cell.PrefixCharacter == "'", TYPED_FORMATS[code]))
    year = datetime.date.today().year
    lines = ["// Generated by tests/sheet/excel_numfmt.py typed from tests/sheet/typed_cases.json. Do not edit by hand.",
             "// Excel 2021 on the reference PC (Windows' region en-NL): each string typed into a General cell.",
             "// {typed, kind, number, text (Text: the cell's; Error: its name), apostrophe, built-in format}",
             "constexpr int kTypedYear = %d;  // the year Excel gave dates typed without one" % year,
             "const TypedCase kTypedCases[] = {"]
    for text, kind, number, shown, quoted, fmt in rows:
        lines.append("    {%s, TypedValue::%s, %r, %s, %s, %d}," % (cpp_string(text), kind, number, cpp_string(shown),
                                                                  "true" if quoted else "false", fmt))
    lines.append("};")
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print(len(rows), "cases")


if __name__ == "__main__":
    if sys.argv[1] == "typed":
        typed(sys.argv[2], sys.argv[3])
    elif sys.argv[1] == "probe":
        probe(sys.argv[2], sys.argv[3])
    elif sys.argv[1] in ("probefile", "probefile1904"):
        probe_file(sys.argv[2], sys.argv[3], sys.argv[1] == "probefile1904")
    elif sys.argv[1] == "table":
        table(sys.argv[2], sys.argv[3])
