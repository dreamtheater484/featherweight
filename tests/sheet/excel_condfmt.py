"""Ask Excel how conditional formats show (s1-numfmt's reference for src/sheet/condfmt).

    python tests/sheet/excel_condfmt.py tests/sheet/condfmt-cases.xlsx tests/sheet/condfmt-expected.json

Writes a workbook with one case per column (its name in row 1, its values below, its rules on the
values; a few cases take two columns, the cells listed column by column), opens it in Excel, and records what Excel shows for each value cell through
Range.DisplayFormat: the fill (none or its RGB), the font color, bold and italic. Data bars and icon
sets aren't in DisplayFormat; their cases are in the workbook for the reference renders only.

Time periods depend on the day: the values are dates around today, and the JSON records today's
serial number, which the unit test passes to the engine.

Excel runs as a new, separate instance (DispatchEx), invisible, with alerts, events and macros off,
and only that instance is ended (excel_numfmt.py's Excel class).
"""
import datetime
import json
import os
import sys
import zipfile
from xml.sax.saxutils import escape, quoteattr

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from excel_numfmt import Excel  # noqa: E402

MAIN = "http://schemas.openxmlformats.org/spreadsheetml/2006/main"
REL = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
X14 = "http://schemas.microsoft.com/office/spreadsheetml/2009/9/main"
XM = "http://schemas.microsoft.com/office/excel/2006/main"

TODAY = (datetime.date.today() - datetime.date(1899, 12, 30)).days

# Differential formats the rules use (dxfId): what each sets.
DXFS = [
    '<font><color rgb="FF9C0006"/></font><fill><patternFill><bgColor rgb="FFFFC7CE"/></patternFill></fill>',  # 0 red
    '<font><color rgb="FF006100"/></font><fill><patternFill><bgColor rgb="FFC6EFCE"/></patternFill></fill>',  # 1 green
    '<font><b/></font>',                                                                                          # 2 bold
    '<font><color rgb="FF0000FF"/></font>',                                                                       # 3 blue text
    '<font><i/></font>',                                                                                          # 4 italic
    '<fill><patternFill><bgColor rgb="FFFFEB9C"/></patternFill></fill>',                                          # 5 yellow fill
    '<fill><patternFill patternType="solid"><fgColor rgb="FF00B0F0"/><bgColor rgb="FF7030A0"/></patternFill></fill>',  # 6 solid, both colors
    '<font><b val="0"/><color theme="5"/></font>',                                                                # 7 not bold, theme color
]

E = "#N/A"
NUMS = [1, 5, 7.5, -2, 3, 10, 5, 0, 12]
MIXED = [5, None, "abc", True, E, -1, 3, 7, "5"]
TEXTS = ["banana", "Apple", "an", None, 123, 1.25, "BAND", "  ", E, "pear*", "a*e", "TRUE", True]
DUPS = [1, "a", "A", 1, "1", None, 2, E, E, 2.0, "b", True, True]


def cell_is(op, *formulas, dxf=0, values=NUMS, stop=False):
    f = "".join("<formula>%s</formula>" % escape(x) for x in formulas)
    return {"name": "cellIs %s %s" % (op, " ".join(formulas)), "values": values,
            "rules": ['<cfRule type="cellIs" dxfId="%d" priority="P"%s operator="%s">%s</cfRule>' %
                      (dxf, ' stopIfTrue="1"' if stop else "", op, f)]}


def rule(name, xml, values=NUMS):
    return {"name": name, "values": values, "rules": [xml] if isinstance(xml, str) else xml}


def time_case(period):
    days = [TODAY + d for d in range(-40, 41, 1) if d in (-35, -31, -14, -9, -8, -7, -6, -3, -1, 0, 1, 2, 5, 7, 8, 12, 33)]
    days += [TODAY + 0.75, TODAY - 0.5, "x"]
    return rule("timePeriod " + period, '<cfRule type="timePeriod" dxfId="0" priority="P" timePeriod="%s"><formula>X</formula></cfRule>' % period,
                days)


CASES = [
    cell_is("greaterThan", "5"),
    cell_is("greaterThan", "5", values=MIXED),
    cell_is("lessThan", "5", values=MIXED),
    cell_is("equal", "0", values=MIXED),
    cell_is("equal", "5", values=MIXED),
    cell_is("notEqual", "5", values=MIXED),
    cell_is("greaterThanOrEqual", "5"),
    cell_is("lessThanOrEqual", "3"),
    cell_is("between", "3", "7"),
    cell_is("between", "7", "3"),
    cell_is("notBetween", "3", "7"),
    cell_is("notBetween", "7", "3"),
    cell_is("between", "3", "7", values=MIXED),
    cell_is("equal", '"abc"', values=["abc", "ABC", "abd", None, 0, E]),
    cell_is("greaterThan", '"b"', values=["abc", "Bcd", "c", None, 5, True, E]),
    cell_is("lessThan", '"b"', values=["abc", "Bcd", "c", None, 5, True, E]),
    cell_is("equal", "TRUE", values=[True, False, 1, "TRUE", None]),
    cell_is("greaterThan", "{c}1+2", values=[1, 2, 6, 7, 12, 13]),          # relative: the cell above + 2
    cell_is("greaterThan", "$A$3", values=[1, 2, 3, 6, 8, 9]),              # absolute
    cell_is("greaterThan", "AVERAGE(${c}$2:${c}$7)", values=[1, 2, 3, 6, 8, 9]),
    rule("expression MOD", '<cfRule type="expression" dxfId="0" priority="P"><formula>MOD({c}2,2)=0</formula></cfRule>'),
    rule("expression value", '<cfRule type="expression" dxfId="0" priority="P"><formula>{c}2</formula></cfRule>', MIXED),
    rule("expression text", '<cfRule type="expression" dxfId="0" priority="P"><formula>"x"</formula></cfRule>', [1, 2]),
    rule("expression row", '<cfRule type="expression" dxfId="2" priority="P"><formula>MOD(ROW(),2)=1</formula></cfRule>'),
    rule("expression error", '<cfRule type="expression" dxfId="0" priority="P"><formula>1/({c}2-5)&gt;0</formula></cfRule>', NUMS),
    cell_is("equal", '""', values=[None, "", 0, "a", False]),
    rule("expression ISBLANK", '<cfRule type="expression" dxfId="0" priority="P"><formula>ISBLANK({c}2)</formula></cfRule>', MIXED),
    rule("top 3", '<cfRule type="top10" dxfId="0" priority="P" rank="3"/>'),
    rule("top 3 mixed", '<cfRule type="top10" dxfId="0" priority="P" rank="3"/>', MIXED),
    rule("top 3 no errors", '<cfRule type="top10" dxfId="0" priority="P" rank="3"/>', [5, None, "abc", True, -1, 3, 7, "5", 9]),
    rule("bottom 2", '<cfRule type="top10" dxfId="0" priority="P" rank="2" bottom="1"/>'),
    rule("top 20%", '<cfRule type="top10" dxfId="0" priority="P" rank="20" percent="1"/>'),
    rule("top 10% of 5", '<cfRule type="top10" dxfId="0" priority="P" rank="10" percent="1"/>', [1, 2, 3, 4, 5]),
    rule("top 50% of 5", '<cfRule type="top10" dxfId="0" priority="P" rank="50" percent="1"/>', [1, 2, 3, 4, 5]),
    rule("bottom 30%", '<cfRule type="top10" dxfId="0" priority="P" rank="30" percent="1" bottom="1"/>'),
    rule("top 99 of 9", '<cfRule type="top10" dxfId="0" priority="P" rank="99"/>'),
    rule("above average", '<cfRule type="aboveAverage" dxfId="0" priority="P"/>'),
    rule("below average", '<cfRule type="aboveAverage" dxfId="0" priority="P" aboveAverage="0"/>'),
    rule("equal or above", '<cfRule type="aboveAverage" dxfId="0" priority="P" equalAverage="1"/>', [1, 2, 3, 4, 5]),
    rule("equal or below", '<cfRule type="aboveAverage" dxfId="0" priority="P" aboveAverage="0" equalAverage="1"/>', [1, 2, 3, 4, 5]),
    rule("1 std dev above", '<cfRule type="aboveAverage" dxfId="0" priority="P" stdDev="1"/>', [1, 2, 3, 4, 5, 6, 7, 8, 30]),
    rule("1 std dev below", '<cfRule type="aboveAverage" dxfId="0" priority="P" aboveAverage="0" stdDev="1"/>', [1, 2, 3, 4, 5, 6, 7, 8, 30, -20]),
    rule("2 std dev above", '<cfRule type="aboveAverage" dxfId="0" priority="P" stdDev="2"/>', [1, 2, 3, 4, 5, 6, 7, 8, 30, 3.2]),
    rule("std dev: population", '<cfRule type="aboveAverage" dxfId="0" priority="P" stdDev="1"/>', [0, 0, 0, 16.6, 20]),
    rule("top 1 ties", '<cfRule type="top10" dxfId="0" priority="P" rank="1"/>', [5, 5, 3, 5, 1]),
    rule("bottom 2 ties", '<cfRule type="top10" dxfId="0" priority="P" rank="2" bottom="1"/>', [1, 4, 1, 1, 9]),
    rule("above average mixed", '<cfRule type="aboveAverage" dxfId="0" priority="P"/>', MIXED),
    rule("above average no errors", '<cfRule type="aboveAverage" dxfId="0" priority="P"/>', [5, None, "abc", True, -1, 3, 7, "5"]),
    rule("duplicates", '<cfRule type="duplicateValues" dxfId="0" priority="P"/>', DUPS),
    rule("duplicates by type", '<cfRule type="duplicateValues" dxfId="0" priority="P"/>', [True, "TRUE", 7, "7.0", "07", 7.0, "x", 0.5, "50%", "", None]),
    rule("unique", '<cfRule type="uniqueValues" dxfId="0" priority="P"/>', DUPS),
    rule("contains an", '<cfRule type="containsText" dxfId="0" priority="P" operator="containsText" text="an"><formula>X</formula></cfRule>', TEXTS),
    rule("contains a*e", '<cfRule type="containsText" dxfId="0" priority="P" operator="containsText" text="a*e"><formula>X</formula></cfRule>', TEXTS),
    rule("contains 2", '<cfRule type="containsText" dxfId="0" priority="P" operator="containsText" text="2"><formula>X</formula></cfRule>', TEXTS),
    rule("not contains an", '<cfRule type="notContainsText" dxfId="0" priority="P" operator="notContains" text="an"><formula>X</formula></cfRule>', TEXTS),
    rule("begins with b", '<cfRule type="beginsWith" dxfId="0" priority="P" operator="beginsWith" text="b"><formula>X</formula></cfRule>', TEXTS),
    rule("begins with 1", '<cfRule type="beginsWith" dxfId="0" priority="P" operator="beginsWith" text="1"><formula>X</formula></cfRule>', TEXTS),
    rule("ends with *", '<cfRule type="endsWith" dxfId="0" priority="P" operator="endsWith" text="*"><formula>X</formula></cfRule>', TEXTS),
    rule("ends with E", '<cfRule type="endsWith" dxfId="0" priority="P" operator="endsWith" text="E"><formula>X</formula></cfRule>', TEXTS),
    rule("blanks", '<cfRule type="containsBlanks" dxfId="0" priority="P"><formula>X</formula></cfRule>', TEXTS),
    rule("no blanks", '<cfRule type="notContainsBlanks" dxfId="0" priority="P"><formula>X</formula></cfRule>', TEXTS),
    rule("errors", '<cfRule type="containsErrors" dxfId="0" priority="P"><formula>X</formula></cfRule>', TEXTS),
    rule("no errors", '<cfRule type="notContainsErrors" dxfId="0" priority="P"><formula>X</formula></cfRule>', TEXTS),
] + [time_case(p) for p in ("today", "yesterday", "tomorrow", "last7Days", "thisMonth", "lastMonth", "nextMonth",
                             "thisWeek", "lastWeek", "nextWeek")] + [
    rule("scale 2 min max", '<cfRule type="colorScale" priority="P"><colorScale><cfvo type="min"/><cfvo type="max"/><color rgb="FFF8696B"/><color rgb="FF63BE7B"/></colorScale></cfRule>'),
    rule("scale 3 percentile", '<cfRule type="colorScale" priority="P"><colorScale><cfvo type="min"/><cfvo type="percentile" val="50"/><cfvo type="max"/><color rgb="FF5A8AC6"/><color rgb="FFFCFCFF"/><color rgb="FFF8696B"/></colorScale></cfRule>'),
    rule("scale num percent", '<cfRule type="colorScale" priority="P"><colorScale><cfvo type="num" val="0"/><cfvo type="percent" val="80"/><color rgb="FF000000"/><color rgb="FFFFFFFF"/></colorScale></cfRule>'),
    rule("scale percentile 25 75", '<cfRule type="colorScale" priority="P"><colorScale><cfvo type="percentile" val="25"/><cfvo type="percentile" val="75"/><color rgb="FFFF0000"/><color rgb="FF0000FF"/></colorScale></cfRule>'),
    rule("scale formula", '<cfRule type="colorScale" priority="P"><colorScale><cfvo type="formula" val="MIN(${c}$2:${c}$10)+1"/><cfvo type="num" val="$A$4"/><color rgb="FFFFFF00"/><color rgb="FF00FF00"/></colorScale></cfRule>'),
    rule("scale mixed", '<cfRule type="colorScale" priority="P"><colorScale><cfvo type="min"/><cfvo type="max"/><color rgb="FFF8696B"/><color rgb="FF63BE7B"/></colorScale></cfRule>', MIXED),
    rule("scale theme", '<cfRule type="colorScale" priority="P"><colorScale><cfvo type="min"/><cfvo type="max"/><color theme="4" tint="0.5"/><color indexed="10"/></colorScale></cfRule>'),
    rule("scale one value", '<cfRule type="colorScale" priority="P"><colorScale><cfvo type="min"/><cfvo type="max"/><color rgb="FFF8696B"/><color rgb="FF63BE7B"/></colorScale></cfRule>', [4, None, 4, "x"]),
    rule("bold then fill", ['<cfRule type="cellIs" dxfId="2" priority="P" operator="greaterThan"><formula>2</formula></cfRule>',
                            '<cfRule type="cellIs" dxfId="5" priority="P" operator="greaterThan"><formula>4</formula></cfRule>']),
    rule("red stops green", ['<cfRule type="cellIs" dxfId="0" priority="P" stopIfTrue="1" operator="greaterThan"><formula>6</formula></cfRule>',
                             '<cfRule type="cellIs" dxfId="1" priority="P" operator="greaterThan"><formula>2</formula></cfRule>',
                             '<cfRule type="cellIs" dxfId="2" priority="P" operator="greaterThan"><formula>4</formula></cfRule>']),
    rule("red over green", ['<cfRule type="cellIs" dxfId="0" priority="P" operator="greaterThan"><formula>6</formula></cfRule>',
                            '<cfRule type="cellIs" dxfId="1" priority="P" operator="greaterThan"><formula>2</formula></cfRule>']),
    rule("fill over scale", ['<cfRule type="cellIs" dxfId="5" priority="P" operator="greaterThan"><formula>6</formula></cfRule>',
                             '<cfRule type="colorScale" priority="P"><colorScale><cfvo type="min"/><cfvo type="max"/><color rgb="FFF8696B"/><color rgb="FF63BE7B"/></colorScale></cfRule>']),
    rule("scale over fill", ['<cfRule type="colorScale" priority="P"><colorScale><cfvo type="min"/><cfvo type="max"/><color rgb="FFF8696B"/><color rgb="FF63BE7B"/></colorScale></cfRule>',
                             '<cfRule type="cellIs" dxfId="6" priority="P" operator="greaterThan"><formula>6</formula></cfRule>']),
    rule("stop hides scale", ['<cfRule type="cellIs" dxfId="4" priority="P" stopIfTrue="1" operator="lessThan"><formula>3</formula></cfRule>',
                              '<cfRule type="colorScale" priority="P"><colorScale><cfvo type="min"/><cfvo type="max"/><color rgb="FFF8696B"/><color rgb="FF63BE7B"/></colorScale></cfRule>']),
    rule("solid both colors", '<cfRule type="cellIs" dxfId="6" priority="P" operator="greaterThan"><formula>4</formula></cfRule>'),
    rule("not bold theme", '<cfRule type="cellIs" dxfId="7" priority="P" operator="greaterThan"><formula>4</formula></cfRule>'),
    # Priority is what counts, not the order written: the second rule has the higher priority.
    rule("priority order", ['<cfRule type="cellIs" dxfId="0" priority="900" operator="greaterThan"><formula>2</formula></cfRule>',
                            '<cfRule type="cellIs" dxfId="1" priority="899" operator="greaterThan"><formula>4</formula></cfRule>']),
    # Two ranges: relative references are relative to the first range's top-left cell.
    {"name": "two ranges", "values": [1, 2, 3, 4, 5, 6, 7, 8], "sqref": "{c}6:{c}9 {c}2:{c}5",
     "rules": ['<cfRule type="expression" dxfId="0" priority="P"><formula>{c}6&gt;4</formula></cfRule>']},
    # Which cell relative references start from: the ranges' bounding box's top-left, the first
    # range written, or the top range. Two columns each.
    {"name": "base column", "values": [1, 2, 3, 4, 5, 6], "values2": [1, 2, 3, 4, 5, 6], "sqref": "{d}2:{d}3 {c}5:{c}6",
     "rules": ['<cfRule type="expression" dxfId="0" priority="P"><formula>COLUMN({c}2)=COLUMN()</formula></cfRule>']},
    {"name": "base row", "values": [1, 2, 3, 4, 5, 6], "values2": [1, 2, 3, 4, 5, 6], "sqref": "{c}5:{c}6 {d}2:{d}3",
     "rules": ['<cfRule type="expression" dxfId="0" priority="P"><formula>ROW({c}2)=ROW()</formula></cfRule>']},
    {"name": "two ranges top", "values": [1, 2, 3, 4, 5, 6, 7, 8], "sqref": "{c}2:{c}3 {c}7:{c}9",
     "rules": ['<cfRule type="top10" dxfId="0" priority="P" rank="2"/>']},
    # Excel 2010's extension list: a rule reading another sheet, and a data bar's extra settings.
    {"name": "x14 other sheet", "values": [1, 2, 3, 4, 5, 6], "x14": [
        '<x14:cfRule type="cellIs" priority="P" operator="greaterThan" id="{00000000-0000-0000-0000-0000000000AA}">'
        '<xm:f>Other!$A$1</xm:f><x14:dxf><font><color rgb="FF9C0006"/></font><fill><patternFill><bgColor rgb="FFFFC7CE"/></patternFill></fill></x14:dxf></x14:cfRule>',
        '<x14:cfRule type="expression" priority="P" id="{00000000-0000-0000-0000-0000000000AB}">'
        '<xm:f>Other!$A$2={c}2</xm:f><x14:dxf><font><b/></font></x14:dxf></x14:cfRule>']},
    {"name": "data bar", "values": [1, 5, 7.5, -2, 3, 10, None, "x", 0],
     "rules": ['<cfRule type="dataBar" priority="P"><dataBar><cfvo type="min"/><cfvo type="max"/><color rgb="FF638EC6"/></dataBar>'
               '<extLst><ext uri="{B025F937-C7B1-47D3-B67F-A62EFF666E3E}" xmlns:x14="%s"><x14:id>{00000000-0000-0000-0000-0000000000B1}</x14:id></ext></extLst></cfRule>' % X14],
     "x14": ['<x14:cfRule type="dataBar" id="{00000000-0000-0000-0000-0000000000B1}"><x14:dataBar minLength="0" maxLength="100" border="1" '
             'negativeBarBorderColorSameAsPositive="0"><x14:cfvo type="autoMin"/><x14:cfvo type="autoMax"/><x14:borderColor rgb="FF638EC6"/>'
             '<x14:negativeFillColor rgb="FFFF0000"/><x14:negativeBorderColor rgb="FFFF0000"/><x14:axisColor rgb="FF000000"/></x14:dataBar></x14:cfRule>']},
    rule("icons 3 arrows", '<cfRule type="iconSet" priority="P"><iconSet iconSet="3Arrows"><cfvo type="percent" val="0"/><cfvo type="percent" val="33"/><cfvo type="percent" val="67"/></iconSet></cfRule>'),
    {"name": "icons custom", "values": NUMS, "x14": [
        '<x14:cfRule type="iconSet" priority="P" id="{00000000-0000-0000-0000-0000000000C1}"><x14:iconSet iconSet="3Triangles" custom="1">'
        '<x14:cfvo type="percent"><xm:f>0</xm:f></x14:cfvo><x14:cfvo type="num"><xm:f>3</xm:f></x14:cfvo><x14:cfvo type="num" gte="0"><xm:f>7.5</xm:f></x14:cfvo>'
        '<x14:cfIcon iconSet="3Stars" iconId="0"/><x14:cfIcon iconSet="NoIcons" iconId="0"/><x14:cfIcon iconSet="3Flags" iconId="2"/></x14:iconSet></x14:cfRule>']},
]

ERRORS = {"#N/A", "#DIV/0!", "#VALUE!", "#REF!", "#NAME?", "#NUM!", "#NULL!"}


def col_name(i):
    s = ""
    i += 1
    while i:
        i, r = divmod(i - 1, 26)
        s = chr(65 + r) + s
    return s


def columns():
    """Each case's first column (0-based): B onwards, two-column cases taking two."""
    col = 1
    for case in CASES:
        yield col
        col += 2 if "values2" in case else 1


def cell_xml(ref, v):
    if v is None:
        return ""
    if isinstance(v, bool):
        return '<c r="%s" t="b"><v>%d</v></c>' % (ref, v)
    if isinstance(v, str) and v in ERRORS:
        return '<c r="%s" t="e"><v>%s</v></c>' % (ref, v)
    if isinstance(v, str):
        return '<c r="%s" t="inlineStr"><is><t xml:space="preserve">%s</t></is></c>' % (ref, escape(v))
    return '<c r="%s"><v>%r</v></c>' % (ref, float(v))


def write(path):
    rows = {}
    blocks, ext = [], []
    priority = 0
    for case, col in zip(CASES, columns()):
        c, d = col_name(col), col_name(col + 1)  # column A holds constants the rules read
        rows.setdefault(1, []).append(cell_xml("%s1" % c, case["name"]))
        for k, (letter, values) in enumerate(((c, case["values"]), (d, case.get("values2", [])))):
            for j, v in enumerate(values):
                if v is not None:
                    rows.setdefault(j + 2, []).append(cell_xml("%s%d" % (letter, j + 2), v))
        sqref = case.get("sqref", "{c}2:{c}%d" % (len(case["values"]) + 1)).replace("{c}", c).replace("{d}", d)
        first = sqref.split()[0].split(":")[0]
        parts = []
        for xml in case.get("rules", []):
            if 'priority="P"' in xml:
                priority += 1
                xml = xml.replace('priority="P"', 'priority="%d"' % priority)
            # Text rules' formulas as Excel writes them, at the range's first cell.
            xml = xml.replace("<formula>X</formula>", "<formula>%s</formula>" % escape(text_formula(xml, first)))
            parts.append(xml.replace("{c}", c).replace("{d}", d))
        if parts:
            blocks.append('<conditionalFormatting sqref="%s">%s</conditionalFormatting>' % (sqref, "".join(parts)))
        for xml in case.get("x14", []):
            if 'priority="P"' in xml:
                priority += 1
                xml = xml.replace('priority="P"', 'priority="%d"' % priority)
            ext.append('<x14:conditionalFormatting xmlns:xm="%s">%s<xm:sqref>%s</xm:sqref></x14:conditionalFormatting>' %
                       (XM, xml.replace("{c}", c).replace("{d}", d), sqref))
    # Column A: constants for absolute references (A3 = 4, A4 = 9).
    for r, v in ((3, 4), (4, 9)):
        rows.setdefault(r, []).insert(0, cell_xml("A%d" % r, v))
    sheet_rows = "".join('<row r="%d">%s</row>' % (r, "".join(cells)) for r, cells in sorted(rows.items()))
    ext_xml = ('<extLst><ext uri="{78C0D931-6437-407d-A8EE-F0AAD7539E65}" xmlns:x14="%s"><x14:conditionalFormattings>%s'
               '</x14:conditionalFormattings></ext></extLst>' % (X14, "".join(ext))) if ext else ""
    sheet = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<worksheet xmlns="%s" xmlns:r="%s">' % (MAIN, REL) +
             '<sheetData>%s</sheetData>%s%s</worksheet>' % (sheet_rows, "".join(blocks), ext_xml))
    other = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<worksheet xmlns="%s"><sheetData>'
             '<row r="1"><c r="A1"><v>3</v></c></row><row r="2"><c r="A2"><v>5</v></c></row></sheetData></worksheet>' % MAIN)
    styles = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<styleSheet xmlns="%s">' % MAIN +
              '<fonts count="1"><font><sz val="11"/><name val="Calibri"/><family val="2"/><scheme val="minor"/></font></fonts>'
              '<fills count="2"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill></fills>'
              '<borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders>'
              '<cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>'
              '<cellXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/></cellXfs>'
              '<cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles>'
              '<dxfs count="%d">%s</dxfs></styleSheet>' % (len(DXFS), "".join("<dxf>%s</dxf>" % d for d in DXFS)))
    workbook = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<workbook xmlns="%s" xmlns:r="%s">' % (MAIN, REL) +
                '<sheets><sheet name="Cases" sheetId="1" r:id="rId1"/><sheet name="Other" sheetId="2" r:id="rId3"/></sheets></workbook>')
    ws_type = "application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"
    types = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
             '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
             '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
             '<Default Extension="xml" ContentType="application/xml"/>'
             '<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>'
             '<Override PartName="/xl/worksheets/sheet1.xml" ContentType="%s"/>'
             '<Override PartName="/xl/worksheets/sheet2.xml" ContentType="%s"/>'
             '<Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>'
             '</Types>' % (ws_type, ws_type))
    pkg_rels = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
                '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                '<Relationship Id="rId1" Type="%s/officeDocument" Target="xl/workbook.xml"/></Relationships>' % REL)
    wb_rels = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
               '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
               '<Relationship Id="rId1" Type="%s/worksheet" Target="worksheets/sheet1.xml"/>'
               '<Relationship Id="rId2" Type="%s/styles" Target="styles.xml"/>'
               '<Relationship Id="rId3" Type="%s/worksheet" Target="worksheets/sheet2.xml"/></Relationships>' % (REL, REL, REL))
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", types)
        z.writestr("_rels/.rels", pkg_rels)
        z.writestr("xl/workbook.xml", workbook)
        z.writestr("xl/_rels/workbook.xml.rels", wb_rels)
        z.writestr("xl/styles.xml", styles)
        z.writestr("xl/worksheets/sheet1.xml", sheet)
        z.writestr("xl/worksheets/sheet2.xml", other)


def text_formula(xml, cell):
    """The formula Excel writes for a text, blank, error or time-period rule (the engine doesn't
    read it: these rules are evaluated from their attributes)."""
    import re
    t = re.search(r'type="(\w+)"', xml).group(1)
    m = re.search(r'text="([^"]*)"', xml)
    text = m.group(1) if m else ""
    q = '"%s"' % text
    p = re.search(r'timePeriod="(\w+)"', xml)
    return {
        "containsText": "NOT(ISERROR(SEARCH(%s,%s)))" % (q, cell),
        "notContainsText": "ISERROR(SEARCH(%s,%s))" % (q, cell),
        "beginsWith": "LEFT(%s,LEN(%s))=%s" % (cell, q, q),
        "endsWith": "RIGHT(%s,LEN(%s))=%s" % (cell, q, q),
        "containsBlanks": "LEN(TRIM(%s))=0" % cell,
        "notContainsBlanks": "LEN(TRIM(%s))>0" % cell,
        "containsErrors": "ISERROR(%s)" % cell,
        "notContainsErrors": "NOT(ISERROR(%s))" % cell,
        "timePeriod": {
            "today": "FLOOR(%(c)s,1)=TODAY()",
            "yesterday": "FLOOR(%(c)s,1)=TODAY()-1",
            "tomorrow": "FLOOR(%(c)s,1)=TODAY()+1",
            "last7Days": "AND(TODAY()-FLOOR(%(c)s,1)<=6,FLOOR(%(c)s,1)<=TODAY())",
            "thisMonth": "AND(MONTH(%(c)s)=MONTH(TODAY()),YEAR(%(c)s)=YEAR(TODAY()))",
            "lastMonth": "AND(MONTH(%(c)s)=MONTH(EDATE(TODAY(),0-1)),YEAR(%(c)s)=YEAR(EDATE(TODAY(),0-1)))",
            "nextMonth": "AND(MONTH(%(c)s)=MONTH(EDATE(TODAY(),0+1)),YEAR(%(c)s)=YEAR(EDATE(TODAY(),0+1)))",
            "thisWeek": "AND(TODAY()-ROUNDDOWN(%(c)s,0)<=WEEKDAY(TODAY())-1,ROUNDDOWN(%(c)s,0)-TODAY()<=7-WEEKDAY(TODAY()))",
            "lastWeek": "AND(TODAY()-ROUNDDOWN(%(c)s,0)>=(WEEKDAY(TODAY())),TODAY()-ROUNDDOWN(%(c)s,0)<(WEEKDAY(TODAY())+7))",
            "nextWeek": "AND(ROUNDDOWN(%(c)s,0)-TODAY()>(7-WEEKDAY(TODAY())),ROUNDDOWN(%(c)s,0)-TODAY()<(15-WEEKDAY(TODAY())))",
        }.get(p.group(1) if p else "", "") % {"c": cell},
    }.get(t, "")


def rgb(bgr):
    bgr = int(bgr)
    return "%02X%02X%02X" % (bgr & 0xFF, (bgr >> 8) & 0xFF, (bgr >> 16) & 0xFF)


def main(xlsx, out):
    xlsx = os.path.abspath(xlsx)
    write(xlsx)
    cases = []
    with Excel() as xl:
        wb = xl.xl.Workbooks.Open(xlsx, 0, True)
        try:
            ws = wb.Worksheets("Cases")
            for case, col in zip(CASES, columns()):
                cells = []
                for k, values in enumerate((case["values"], case.get("values2", []))):
                    for j in range(len(values)):
                        d = ws.Cells(j + 2, col + 1 + k).DisplayFormat
                        pattern = int(d.Interior.Pattern)
                        cells.append({
                            "fill": None if pattern == -4142 else rgb(d.Interior.Color),
                            "font": rgb(d.Font.Color),
                            "bold": bool(d.Font.Bold),
                            "italic": bool(d.Font.Italic),
                        })
                cases.append({"name": case["name"], "column": col_name(col), "rows": len(case["values"]), "cells": cells})
        finally:
            wb.Close(False)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        # One case per line.
        f.write('{"today": %d, "cases": [\n' % TODAY)
        f.write(",\n".join(json.dumps(c, separators=(",", ":")) for c in cases))
        f.write("\n]}\n")
    print("%d cases, %d cells" % (len(cases), sum(len(c["cells"]) for c in cases)))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
