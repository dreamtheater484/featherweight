"""Generates the public office corpus in tests/office/ (plan section 10).

  generated/  made with python-docx, openpyxl and python-pptx
  edge/       hand-made files that must round-trip: Markup Compatibility, extLst, unknown
              namespaces, custom XML, unusual XML and zip layouts
  malicious/  hand-made files that must be refused with a plain message: zip bombs, a nesting
              bomb, a DTD, missing content types, and other damage

Requires a venv (not the global Python). From the repository root:
  python -m venv .venv
  .venv\\Scripts\\python -m pip install -r tests/office/requirements.txt
  .venv\\Scripts\\python tests/office/make_test_office.py

The output is deterministic: document dates and zip timestamps are fixed, so regenerating
changes nothing unless this script or a library version changes.
"""
import io
import os
import re
import struct
import zipfile
import zlib
from datetime import datetime

import docx
import openpyxl
import pptx
from docx.enum.section import WD_ORIENT
from docx.enum.style import WD_STYLE_TYPE
from docx.enum.text import WD_BREAK
from docx.opc.constants import RELATIONSHIP_TYPE as RT
from docx.oxml import OxmlElement
from docx.oxml.ns import qn
from docx.shared import Inches, Pt, RGBColor
from openpyxl.chart import BarChart, Reference
from openpyxl.comments import Comment
from openpyxl.drawing.image import Image as XlImage
from openpyxl.formatting.rule import ColorScaleRule
from openpyxl.styles import Alignment, Border, Font, PatternFill, Side
from openpyxl.workbook.defined_name import DefinedName
from openpyxl.worksheet.datavalidation import DataValidation
from PIL import Image
from pptx.chart.data import CategoryChartData
from pptx.enum.chart import XL_CHART_TYPE
from pptx.enum.shapes import MSO_SHAPE
from pptx.util import Emu
from pptx.util import Inches as PInches
from pptx.util import Pt as PPt

ROOT = os.path.dirname(os.path.abspath(__file__))
GENERATED = os.path.join(ROOT, "generated")
EDGE = os.path.join(ROOT, "edge")
MALICIOUS = os.path.join(ROOT, "malicious")

FIXED_DATE = datetime(2026, 1, 1, 12, 0, 0)
ZIP_DATE = (1980, 1, 1, 0, 0, 0)
LOREM = (
    "Featherweight opens documents instantly and never damages them. The quick brown fox jumps "
    "over the lazy dog. Café crème, naïve façade, ångström and Straße test accented text. "
)


# ---------- Helpers ----------


def png(width=96, height=64, seed=0):
    img = Image.new("RGB", (width, height))
    img.putdata([((x * 3 + seed) % 256, (y * 4) % 256, (x + y + seed * 7) % 256)
                 for y in range(height) for x in range(width)])
    out = io.BytesIO()
    img.save(out, format="PNG", optimize=False)
    return out.getvalue()


def read_entries(data):
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        return [(i.filename, z.read(i.filename)) for i in z.infolist()]


def write_zip(entries, compression=zipfile.ZIP_DEFLATED, **kwargs):
    """Writes entries (name, bytes) with fixed timestamps, in order."""
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", compression) as z:
        for name, data in entries:
            info = zipfile.ZipInfo(name, date_time=ZIP_DATE)
            info.compress_type = compression
            info.external_attr = kwargs.get("external_attr", 0)
            z.writestr(info, data)
    return out.getvalue()


def normalize(data):
    """Fixed document dates and zip timestamps, so regenerating is deterministic. Embedded
    workbooks (chart data) are normalized too."""
    entries = []
    for name, body in read_entries(data):
        if name == "docProps/core.xml":
            text = body.decode("utf-8")
            text = re.sub(r"(<dcterms:(created|modified)[^>]*>)[^<]*(</dcterms:\2>)",
                          r"\g<1>2026-01-01T12:00:00Z\g<3>", text)
            body = text.encode("utf-8")
        elif name == "word/comments.xml":
            body = re.sub(rb'w:date="[^"]*"', b'w:date="2026-01-01T12:00:00Z"', body)
        elif name.endswith(".xlsx") and "/embeddings/" in name:
            body = normalize(body)
        entries.append((name, body))
    return write_zip(entries)


def save(folder, name, data):
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, name), "wb") as f:
        f.write(data)
    print(f"  {os.path.relpath(os.path.join(folder, name), ROOT)}  ({len(data):,} bytes)")


def to_bytes(saveable):
    out = io.BytesIO()
    saveable.save(out)
    return out.getvalue()


def edit_part(entries, name, fn):
    return [(n, fn(b) if n == name else b) for n, b in entries]


def set_core(props, title):
    """Core properties for python-docx and python-pptx (author) or openpyxl (creator)."""
    props.title = title
    props.created = FIXED_DATE
    props.modified = FIXED_DATE
    for name in ("author", "creator", "last_modified_by", "lastModifiedBy"):
        if hasattr(props, name):
            setattr(props, name, "Featherweight tests")


# ---------- Documents ----------


def docx_simple():
    d = docx.Document()
    set_core(d.core_properties, "Simple document")
    d.add_heading("A simple document", level=1)
    p = d.add_paragraph("Plain text, ")
    p.add_run("bold").bold = True
    p.add_run(", ")
    p.add_run("italic").italic = True
    p.add_run(" and ")
    r = p.add_run("colored")
    r.font.color.rgb = RGBColor(0x3E, 0x48, 0xD6)
    p.add_run(" runs.")
    for item in ("First item", "Second item", "Third item"):
        d.add_paragraph(item, style="List Bullet")
    for item in ("One", "Two"):
        d.add_paragraph(item, style="List Number")
    d.add_paragraph(LOREM * 3)
    d.add_paragraph().add_run().add_break(WD_BREAK.PAGE)
    d.add_heading("Second page", level=2)
    d.add_paragraph(LOREM)
    return normalize(to_bytes(d))


def add_hyperlink(paragraph, url, text):
    r_id = paragraph.part.relate_to(url, RT.HYPERLINK, is_external=True)
    link = OxmlElement("w:hyperlink")
    link.set(qn("r:id"), r_id)
    run = OxmlElement("w:r")
    rpr = OxmlElement("w:rPr")
    style = OxmlElement("w:u")
    style.set(qn("w:val"), "single")
    rpr.append(style)
    run.append(rpr)
    t = OxmlElement("w:t")
    t.text = text
    run.append(t)
    link.append(run)
    paragraph._p.append(link)


def docx_tables_images():
    d = docx.Document()
    set_core(d.core_properties, "Tables and pictures")
    d.add_heading("Tables, pictures and links", level=1)
    table = d.add_table(rows=4, cols=3)
    table.style = "Table Grid"
    for r, row in enumerate(table.rows):
        for c, cell in enumerate(row.cells):
            cell.text = f"R{r + 1}C{c + 1}"
    table.cell(0, 0).merge(table.cell(0, 1)).text = "Merged header"
    table.cell(2, 2).merge(table.cell(3, 2))
    d.add_paragraph("A picture:")
    d.add_picture(io.BytesIO(png()), width=Inches(2))
    d.add_paragraph("Another, smaller:")
    d.add_picture(io.BytesIO(png(48, 48, seed=5)), width=Inches(0.75))
    p = d.add_paragraph("A link: ")
    add_hyperlink(p, "https://github.com/dreamtheater484/featherweight", "Featherweight on GitHub")
    return normalize(to_bytes(d))


def docx_sections():
    d = docx.Document()
    set_core(d.core_properties, "Sections, headers and footers")
    section = d.sections[0]
    section.header.paragraphs[0].text = "Header of section 1"
    section.footer.paragraphs[0].text = "Footer of section 1"
    d.add_heading("Portrait section", level=1)
    d.add_paragraph(LOREM * 4)
    landscape = d.add_section()
    landscape.orientation = WD_ORIENT.LANDSCAPE
    landscape.page_width, landscape.page_height = section.page_height, section.page_width
    landscape.header.is_linked_to_previous = False
    landscape.header.paragraphs[0].text = "Header of section 2 (landscape)"
    d.add_heading("Landscape section", level=1)
    d.add_paragraph(LOREM * 2)
    return normalize(to_bytes(d))


def docx_styles_unicode():
    d = docx.Document()
    set_core(d.core_properties, "Styles and Unicode")
    style = d.styles.add_style("Featherweight Note", WD_STYLE_TYPE.PARAGRAPH)
    style.font.name = "Georgia"
    style.font.size = Pt(13)
    style.font.italic = True
    d.add_heading("Styles and Unicode", level=1)
    d.add_paragraph("A paragraph in a custom style.", style="Featherweight Note")
    d.add_paragraph("Accents: café, naïve, Ærøskøbing, Łódź, Ελληνικά, Русский.")
    d.add_paragraph("East Asian: 日本語のテキスト、中文文本、한국어 텍스트.")
    d.add_paragraph("Right to left: مرحبا بالعالم and שלום עולם.")
    d.add_paragraph("Emoji outside the BMP: 😀 🎉 𝄞.")
    p = d.add_paragraph("   Leading spaces, a tab\there, and a line break")
    p.add_run().add_break()
    p.add_run("on the next line.  Trailing spaces:   ")
    comment_target = d.add_paragraph("This sentence has a comment.")
    d.add_comment(comment_target.runs, text="A comment on the sentence.", author="Featherweight tests",
                  initials="FT")
    return normalize(to_bytes(d))


# ---------- Spreadsheets ----------


def xlsx_simple():
    wb = openpyxl.Workbook()
    set_core(wb.properties, "Simple workbook")
    ws = wb.active
    ws.title = "Data"
    ws.append(["Item", "Quantity", "Price", "Total", "Date"])
    rows = [("Apples", 4, 0.5), ("Pears", 6, 0.75), ("Café beans", 2, 7.25), ("Bread", 1, 2.1)]
    for i, (item, qty, price) in enumerate(rows, start=2):
        ws.append([item, qty, price, f"=B{i}*C{i}", datetime(2026, 1, i)])
        ws[f"C{i}"].number_format = "#,##0.00"
        ws[f"E{i}"].number_format = "yyyy-mm-dd"
    ws.append(["Sum", "=SUM(B2:B5)", None, "=SUM(D2:D5)"])
    ws["A7"] = "Text with a\nline break"
    ws["A7"].alignment = Alignment(wrap_text=True)
    return normalize(to_bytes(wb))


def xlsx_formatting():
    wb = openpyxl.Workbook()
    set_core(wb.properties, "Formatting")
    ws = wb.active
    ws.title = "Formatted"
    thin = Side(style="thin", color="FF888888")
    for r in range(1, 11):
        for c in range(1, 6):
            cell = ws.cell(row=r, column=c, value=r * c)
            cell.border = Border(left=thin, right=thin, top=thin, bottom=thin)
    ws["A1"].font = Font(bold=True, size=14, color="FF3E48D6")
    ws["B1"].fill = PatternFill("solid", fgColor="FFFFE699")
    ws.merge_cells("A12:E12")
    ws["A12"] = "Merged and centered"
    ws["A12"].alignment = Alignment(horizontal="center")
    ws.column_dimensions["A"].width = 24
    ws.row_dimensions[12].height = 28
    ws.freeze_panes = "B2"
    ws.conditional_formatting.add("C2:C10", ColorScaleRule(start_type="min", start_color="FFF8696B",
                                                           end_type="max", end_color="FF63BE7B"))
    dv = DataValidation(type="list", formula1='"Yes,No,Maybe"', allow_blank=True)
    ws.add_data_validation(dv)
    dv.add("E2:E10")
    ws.auto_filter.ref = "A1:E10"
    return normalize(to_bytes(wb))


def xlsx_sheets_chart():
    wb = openpyxl.Workbook()
    set_core(wb.properties, "Sheets, chart and names")
    data = wb.active
    data.title = "Sales"
    data.append(["Month", "North", "South"])
    for i, month in enumerate(["Jan", "Feb", "Mar", "Apr", "May", "Jun"]):
        data.append([month, 10 + i * 3, 14 - i])
    chart = BarChart()
    chart.title = "Sales by month"
    chart.add_data(Reference(data, min_col=2, min_row=1, max_col=3, max_row=7), titles_from_data=True)
    chart.set_categories(Reference(data, min_col=1, min_row=2, max_row=7))
    data.add_chart(chart, "E2")
    data["A9"] = "See the summary"
    data["A9"].hyperlink = "#Summary!A1"
    data["B9"] = "Website"
    data["B9"].hyperlink = "https://github.com/dreamtheater484/featherweight"
    data["A2"].comment = Comment("The first month.", "Featherweight tests")
    summary = wb.create_sheet("Summary")
    summary["A1"] = "Total North"
    summary["B1"] = "=SUM(Sales!B2:B7)"
    summary["A2"] = "Total South"
    summary["B2"] = "=SUM(SouthRange)"
    wb.defined_names["SouthRange"] = DefinedName("SouthRange", attr_text="Sales!$C$2:$C$7")
    hidden = wb.create_sheet("Hidden")
    hidden["A1"] = "Hidden sheet"
    hidden.sheet_state = "hidden"
    return normalize(to_bytes(wb))


def xlsx_image():
    wb = openpyxl.Workbook()
    set_core(wb.properties, "Picture")
    ws = wb.active
    ws["A1"] = "A picture is anchored at B3."
    ws.add_image(XlImage(io.BytesIO(png(120, 80, seed=9))), "B3")
    return normalize(to_bytes(wb))


# ---------- Presentations ----------


def pptx_simple():
    prs = pptx.Presentation()
    set_core(prs.core_properties, "Simple presentation")
    title = prs.slides.add_slide(prs.slide_layouts[0])
    title.shapes.title.text = "A simple presentation"
    title.placeholders[1].text = "Made by the Featherweight test corpus"
    bullets = prs.slides.add_slide(prs.slide_layouts[1])
    bullets.shapes.title.text = "Bullets"
    body = bullets.placeholders[1].text_frame
    body.text = "First point"
    for text, level in (("A sub-point", 1), ("Second point", 0), ("Café, naïve, 日本語", 0)):
        para = body.add_paragraph()
        para.text = text
        para.level = level
    bullets.notes_slide.notes_text_frame.text = "Speaker notes for the bullet slide."
    return normalize(to_bytes(prs))


def pptx_shapes_table_chart():
    prs = pptx.Presentation()
    set_core(prs.core_properties, "Shapes, table and chart")
    blank = prs.slide_layouts[6]
    s1 = prs.slides.add_slide(blank)
    box = s1.shapes.add_shape(MSO_SHAPE.ROUNDED_RECTANGLE, PInches(1), PInches(1), PInches(3), PInches(1.5))
    box.text_frame.text = "A rounded rectangle"
    star = s1.shapes.add_shape(MSO_SHAPE.STAR_5_POINT, PInches(5), PInches(1), PInches(2), PInches(2))
    star.rotation = 15
    tb = s1.shapes.add_textbox(PInches(1), PInches(3), PInches(6), PInches(1))
    run = tb.text_frame.paragraphs[0].add_run()
    run.text = "A link to Featherweight"
    run.font.size = PPt(20)
    run.hyperlink.address = "https://github.com/dreamtheater484/featherweight"
    s1.shapes.add_picture(io.BytesIO(png(64, 64, seed=3)), PInches(7), PInches(4))

    s2 = prs.slides.add_slide(blank)
    rows, cols = 4, 3
    table = s2.shapes.add_table(rows, cols, PInches(1), PInches(1), PInches(6), PInches(2)).table
    for r in range(rows):
        for c in range(cols):
            table.cell(r, c).text = f"R{r + 1}C{c + 1}"

    s3 = prs.slides.add_slide(blank)
    chart_data = CategoryChartData()
    chart_data.categories = ["Q1", "Q2", "Q3", "Q4"]
    chart_data.add_series("North", (12.0, 15.5, 9.25, 18.0))
    chart_data.add_series("South", (8.0, 11.0, 14.5, 10.0))
    s3.shapes.add_chart(XL_CHART_TYPE.COLUMN_CLUSTERED, PInches(1), PInches(1), PInches(7), PInches(4.5), chart_data)
    s3.notes_slide.notes_text_frame.text = "The chart embeds a workbook."
    return normalize(to_bytes(prs))


def pptx_layouts():
    prs = pptx.Presentation()
    set_core(prs.core_properties, "Every layout")
    prs.slide_width = Emu(12192000)  # 16:9
    prs.slide_height = Emu(6858000)
    for i, layout in enumerate(prs.slide_layouts):
        slide = prs.slides.add_slide(layout)
        if slide.shapes.title is not None:
            slide.shapes.title.text = f"Layout {i + 1}: {layout.name}"
    return normalize(to_bytes(prs))


# ---------- Edge cases that must round-trip ----------

ALTERNATE_CONTENT = (
    '<w:p w14:paraId="1A2B3C4D" w14:textId="77777777"><w:r><mc:AlternateContent>'
    '<mc:Choice Requires="w16du"><w:t>Choice for a namespace nobody understands</w:t></mc:Choice>'
    '<mc:Choice Requires="wps"><w:drawing><wp:anchor distT="0" distB="0" distL="114300" distR="114300" '
    'simplePos="0" relativeHeight="251659264" behindDoc="0" locked="0" layoutInCell="1" allowOverlap="1">'
    '<wp:simplePos x="0" y="0"/><wp:positionH relativeFrom="column"><wp:posOffset>0</wp:posOffset></wp:positionH>'
    '<wp:positionV relativeFrom="paragraph"><wp:posOffset>0</wp:posOffset></wp:positionV>'
    '<wp:extent cx="1828800" cy="457200"/><wp:effectExtent l="0" t="0" r="0" b="0"/><wp:wrapNone/>'
    '<wp:docPr id="1" name="Text Box 1"/><wp:cNvGraphicFramePr/>'
    '<a:graphic xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main">'
    '<a:graphicData uri="http://schemas.microsoft.com/office/word/2010/wordprocessingShape"><wps:wsp>'
    '<wps:cNvSpPr txBox="1"/><wps:spPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="1828800" cy="457200"/></a:xfrm>'
    '<a:prstGeom prst="rect"><a:avLst/></a:prstGeom></wps:spPr><wps:txbx><w:txbxContent><w:p><w:r>'
    '<w:t>Text box (Choice)</w:t></w:r></w:p></w:txbxContent></wps:txbx><wps:bodyPr/></wps:wsp>'
    '</a:graphicData></a:graphic></wp:anchor></w:drawing></mc:Choice>'
    '<mc:Fallback><w:pict><v:shapetype id="_x0000_t202" coordsize="21600,21600" o:spt="202" '
    'path="m,l,21600r21600,l21600,xe"><v:stroke joinstyle="miter"/><v:path gradientshapeok="t" '
    'o:connecttype="rect"/></v:shapetype><v:shape id="Text Box 1" o:spid="_x0000_s1026" type="#_x0000_t202" '
    'style="position:absolute;width:144pt;height:36pt"><v:textbox><w:txbxContent><w:p><w:r>'
    '<w:t>Text box (Fallback)</w:t></w:r></w:p></w:txbxContent></v:textbox></v:shape></w:pict></mc:Fallback>'
    '</mc:AlternateContent></w:r></w:p>'
)


def insert_before_sectpr(xml, fragment):
    i = xml.rfind("<w:sectPr")
    return xml[:i] + fragment + xml[i:]


def edge_alternate_content(simple_docx):
    def fix(body):
        xml = body.decode("utf-8")
        xml = xml.replace('mc:Ignorable="w14 wp14"',
                          'xmlns:w16du="http://schemas.microsoft.com/office/word/2023/wordml/word16du" '
                          'mc:Ignorable="w14 wp14 w16du"', 1)
        return insert_before_sectpr(xml, ALTERNATE_CONTENT).encode("utf-8")

    return write_zip(edit_part(read_entries(simple_docx), "word/document.xml", fix))


def edge_extlst(simple_xlsx):
    ext = (
        '<extLst><ext uri="{78C0D931-6437-407d-A8EE-F0AAD7539E65}" '
        'xmlns:x14="http://schemas.microsoft.com/office/spreadsheetml/2009/9/main">'
        '<x14:conditionalFormattings><x14:conditionalFormatting '
        'xmlns:xm="http://schemas.microsoft.com/office/excel/2006/main">'
        '<x14:cfRule type="dataBar" id="{00000000-0001-0000-0000-000000000000}">'
        '<x14:dataBar minLength="0" maxLength="100" gradient="0"><x14:cfvo type="autoMin"/><x14:cfvo type="autoMax"/>'
        '<x14:negativeFillColor rgb="FFFF0000"/><x14:axisColor rgb="FF000000"/></x14:dataBar></x14:cfRule>'
        '<xm:sqref>B2:B5</xm:sqref></x14:conditionalFormatting></x14:conditionalFormattings></ext>'
        '<ext uri="{05C60535-1F16-4fd2-B633-F4F36F0B64E0}" '
        'xmlns:x14="http://schemas.microsoft.com/office/spreadsheetml/2009/9/main"><x14:sparklineGroups '
        'xmlns:xm="http://schemas.microsoft.com/office/excel/2006/main"><x14:sparklineGroup displayEmptyCellsAs="gap">'
        '<x14:colorSeries rgb="FF376092"/><x14:sparklines><x14:sparkline><xm:f>Data!B2:B5</xm:f><xm:sqref>F2</xm:sqref>'
        '</x14:sparkline></x14:sparklines></x14:sparklineGroup></x14:sparklineGroups></ext></extLst>'
    )

    def fix(body):
        xml = body.decode("utf-8")
        xml = re.sub(r"<worksheet ([^>]*)>",
                     r'<worksheet \1 xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006" '
                     r'xmlns:x14ac="http://schemas.microsoft.com/office/spreadsheetml/2009/9/ac" '
                     r'mc:Ignorable="x14ac">', xml, count=1)
        xml = re.sub(r"<row ", '<row x14ac:dyDescent="0.25" ', xml)
        return xml.replace("</worksheet>", ext + "</worksheet>").encode("utf-8")

    return write_zip(edit_part(read_entries(simple_xlsx), "xl/worksheets/sheet1.xml", fix))


CUSTOM_XML_ITEM = (
    "﻿<?xml version='1.0' encoding='utf-8' standalone='no'?>\r\n"
    "<!-- Custom XML exercises the corners of XML: CRLF line ends, comments, processing\r\n"
    "     instructions, CDATA, character references, single quotes and empty elements. -->\r\n"
    "<?featherweight keep this?>\r\n"
    "<data xmlns='urn:featherweight:custom' xmlns:x='urn:featherweight:extra' version='1'>\r\n"
    "  <entry key='a&amp;b' note='tab\there, line\nbreak, ref&#9;tab &#10;newline &#13;return'>"
    "Text with &lt;markup&gt;, &quot;quotes&quot;, &apos;apostrophes&apos; and a CR&#13;here</entry>\r\n"
    "  <entry key='cdata'><![CDATA[<not markup> & ]] not the end]]></entry>\r\n"
    "  <x:empty></x:empty><x:empty/>\r\n"
    "  <preserved xml:space='preserve'>   spaced   </preserved>\r\n"
    "  <mixed>Some <b>mixed</b> content, <i>kept</i>.</mixed>\r\n"
    "</data>\r\n"
)

ITEM_PROPS = (
    '<?xml version="1.0" encoding="UTF-8" standalone="no"?>\r\n'
    '<ds:datastoreItem ds:itemID="{6F2A1B9E-0C8D-4E5F-9A7B-3C2D1E0F4A5B}" '
    'xmlns:ds="http://schemas.openxmlformats.org/officeDocument/2006/customXml"><ds:schemaRefs/></ds:datastoreItem>'
)


def edge_unknown_namespaces(simple_docx):
    def fix_document(body):
        xml = body.decode("utf-8")
        xml = xml.replace('mc:Ignorable="w14 wp14"',
                          'xmlns:fw="urn:featherweight:test:unknown" mc:Ignorable="w14 wp14 fw"', 1)
        xml = xml.replace("<w:p>", '<w:p fw:tag="first">', 1)
        unknown = ('<fw:annotation fw:level="2" xmlns:fw2="urn:featherweight:test:other">Kept as is, '
                   '<fw:b fw2:weight="heavy">nested</fw:b> too.</fw:annotation>'
                   '<w:p><w:r><w:t xml:space="preserve">After the annotation. </w:t></w:r>'
                   '<fw:inline>inline unknown</fw:inline><w:r><w:t>End.</w:t></w:r></w:p>')
        return insert_before_sectpr(xml, unknown).encode("utf-8")

    def fix_rels(body):
        xml = body.decode("utf-8")
        rel = ('<Relationship Id="rIdCustom1" '
               'Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/customXml" '
               'Target="../customXml/item2.xml"/>')
        return xml.replace("</Relationships>", rel + "</Relationships>").encode("utf-8")

    def fix_types(body):
        xml = body.decode("utf-8")
        override = ('<Override PartName="/customXml/itemProps2.xml" '
                    'ContentType="application/vnd.openxmlformats-officedocument.customXmlProperties+xml"/>')
        return xml.replace("</Types>", override + "</Types>").encode("utf-8")

    entries = read_entries(simple_docx)
    entries = edit_part(entries, "word/document.xml", fix_document)
    entries = edit_part(entries, "word/_rels/document.xml.rels", fix_rels)
    entries = edit_part(entries, "[Content_Types].xml", fix_types)
    item_rels = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\r\n'
                 '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                 '<Relationship Id="rId1" '
                 'Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/customXmlProps" '
                 'Target="itemProps2.xml"/></Relationships>')
    # The template already has customXml/item1.xml.
    entries += [("customXml/item2.xml", CUSTOM_XML_ITEM.encode("utf-8")),
                ("customXml/itemProps2.xml", ITEM_PROPS.encode("utf-8")),
                ("customXml/_rels/item2.xml.rels", item_rels.encode("utf-8"))]
    return write_zip(entries)


def edge_utf16(simple_xlsx):
    def to_utf16(body):
        xml = body.decode("utf-8").replace("encoding='UTF-8'", "encoding='UTF-16'")
        xml = xml.replace('encoding="UTF-8"', 'encoding="UTF-16"')
        return "﻿".encode("utf-16-le") + xml.encode("utf-16-le")

    return write_zip(edit_part(read_entries(simple_xlsx), "docProps/app.xml", to_utf16))


def edge_escaped_names(simple_docx):
    """Part names with a space and a non-ASCII letter; relationship targets %-encode them."""
    def fix_rels(body):
        xml = body.decode("utf-8")
        rels = ('<Relationship Id="rIdSpace" '
                'Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" '
                'Target="media/my%20picture.png"/>'
                '<Relationship Id="rIdAccent" '
                'Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" '
                'Target="media/caf%C3%A9.png"/>')
        return xml.replace("</Relationships>", rels + "</Relationships>").encode("utf-8")

    def fix_types(body):
        xml = body.decode("utf-8")
        if 'Extension="png"' not in xml:
            xml = xml.replace("</Types>", '<Default Extension="png" ContentType="image/png"/></Types>')
        return xml.encode("utf-8")

    entries = read_entries(simple_docx)
    entries = edit_part(entries, "word/_rels/document.xml.rels", fix_rels)
    entries = edit_part(entries, "[Content_Types].xml", fix_types)
    entries += [("word/media/my picture.png", png(16, 16, seed=1)), ("word/media/café.png", png(16, 16, seed=2))]
    return write_zip(entries)


class _Unseekable(io.RawIOBase):
    """A stream zipfile can't seek in, so it writes data descriptors after each entry."""

    def __init__(self):
        self.data = bytearray()

    def writable(self):
        return True

    def write(self, b):
        self.data += b
        return len(b)


def edge_zip_layouts(simple_docx, simple_xlsx, simple_pptx):
    save(EDGE, "zip-stored.xlsx", write_zip(read_entries(simple_xlsx), compression=zipfile.ZIP_STORED))

    entries = read_entries(simple_docx)

    stream = _Unseekable()
    with zipfile.ZipFile(stream, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in entries:
            z.writestr(zipfile.ZipInfo(name, date_time=ZIP_DATE), data, compress_type=zipfile.ZIP_DEFLATED)
    save(EDGE, "zip-data-descriptors.docx", bytes(stream.data))

    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in entries:
            info = zipfile.ZipInfo(name, date_time=ZIP_DATE)
            info.compress_type = zipfile.ZIP_DEFLATED
            with z.open(info, "w", force_zip64=True) as f:
                f.write(data)
    save(EDGE, "zip-zip64-headers.docx", out.getvalue())

    # An archive comment, entry comments, an NTFS timestamp extra field, odd attributes and
    # a directory entry: all must survive.
    out = io.BytesIO()
    ntfs = struct.pack("<HHIHHQQQ", 0x000A, 32, 0, 1, 24, 0x01DC7B4E2A3B4C5D, 0x01DC7B4E2A3B4C5D, 0x01DC7B4E2A3B4C5D)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.comment = b"Archive comment kept by Featherweight"
        z.writestr(zipfile.ZipInfo("ppt/", date_time=ZIP_DATE), b"")
        for i, (name, data) in enumerate(read_entries(simple_pptx)):
            info = zipfile.ZipInfo(name, date_time=(2024, 5, 17, 9, 30, 12))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.extra = ntfs
            info.comment = f"entry {i}".encode()
            info.external_attr = 0x20
            info.create_system = 0
            z.writestr(info, data)
    save(EDGE, "zip-comments-extra.pptx", out.getvalue())


# ---------- Malicious and damaged files: each must be refused ----------


def raw_deflate(data):
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    return c.compress(data) + c.flush()


def build_zip(entries, comment=b""):
    """A zip from scratch, for headers that lie. Each entry: name, payload (as stored),
    method, crc, csize, usize, flags."""
    out = bytearray()
    central = bytearray()
    for e in entries:
        name = e["name"].encode("utf-8")
        offset = len(out)
        header = struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, e.get("flags", 0), e["method"], 0, 0x21,
                             e["crc"], e["csize"], e["usize"], len(name), 0)
        out += header + name + e["payload"]
        central += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 20, 20, e.get("flags", 0), e["method"], 0, 0x21,
                               e["crc"], e["csize"], e["usize"], len(name), 0, 0, 0, 0, 0, offset) + name
    end = struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(entries), len(entries), len(central), len(out), len(comment))
    return bytes(out + central + end + comment)


def deflated_entry(name, data, **overrides):
    payload = raw_deflate(data)
    e = {"name": name, "payload": payload, "method": 8, "crc": zlib.crc32(data), "csize": len(payload),
         "usize": len(data)}
    e.update(overrides)
    return e


def with_document(entries, xml):
    return [(n, xml if n == "word/document.xml" else b) for n, b in entries]


DOC_OPEN = (b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\r\n'
            b'<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body>')
DOC_CLOSE = b"</w:body></w:document>"


def malicious(simple_docx, simple_xlsx, simple_pptx):
    entries = read_entries(simple_docx)

    # Zip bomb: 100 MB of text that compresses about 1000:1.
    bomb = DOC_OPEN + b"<w:p><w:r><w:t>" + b" " * (100 << 20) + b"</w:t></w:r></w:p>" + DOC_CLOSE
    save(MALICIOUS, "zip-bomb-ratio.docx", write_zip(with_document(entries, bomb)))

    # Zip bomb with a lying header: 64 MB inflate from an entry that claims 1,000 bytes.
    zeros = b"\0" * (64 << 20)
    lying = [deflated_entry(n, b) for n, b in entries if n != "word/document.xml"]
    lying.append(deflated_entry("word/document.xml", zeros, usize=1000, crc=zlib.crc32(zeros[:1000])))
    save(MALICIOUS, "zip-bomb-lying-size.docx", build_zip(lying))

    # A part that claims to unpack to 2 GB.
    huge = [deflated_entry(n, b, usize=0x7FFFFFFF) if n == "xl/worksheets/sheet1.xml" else deflated_entry(n, b)
            for n, b in read_entries(simple_xlsx)]
    save(MALICIOUS, "zip-bomb-declared-size.xlsx", build_zip(huge))

    # Overlapping entries (the non-recursive zip bomb): one entry's data contains another entry.
    inner_name = b"word/inner.xml"
    inner_data = b"<inner/>"
    inner = struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, 0, 0, 0, 0x21, zlib.crc32(inner_data), len(inner_data),
                        len(inner_data), len(inner_name), 0) + inner_name + inner_data
    outer_data = b"<!--" + inner + b"-->"
    overlap = [deflated_entry(n, b) for n, b in entries]
    overlap.append({"name": "word/outer.xml", "payload": outer_data, "method": 0, "crc": zlib.crc32(outer_data),
                    "csize": len(outer_data), "usize": len(outer_data)})
    blob = bytearray(build_zip(overlap))
    # Add a central record for the inner entry, pointing inside the outer one.
    end = blob.rfind(b"PK\x05\x06")
    count, cd_size, cd_offset = struct.unpack_from("<HII", blob, end + 10)
    inner_offset = blob.find(inner)
    central = struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 20, 20, 0, 0, 0, 0x21, zlib.crc32(inner_data),
                          len(inner_data), len(inner_data), len(inner_name), 0, 0, 0, 0, 0, inner_offset) + inner_name
    blob[end:end] = central
    struct.pack_into("<HHII", blob, end + len(central) + 8, count + 1, count + 1, cd_size + len(central), cd_offset)
    save(MALICIOUS, "zip-overlapping-entries.docx", bytes(blob))

    # Nesting bomb: 100,000 nested elements.
    depth = 100_000
    nested = DOC_OPEN + b"<w:p>" + b"<w:sdt><w:sdtContent>" * (depth // 2) + b"</w:sdtContent></w:sdt>" * (
        depth // 2) + b"</w:p>" + DOC_CLOSE
    save(MALICIOUS, "deep-nesting.docx", write_zip(with_document(entries, nested)))

    # A DTD with an entity expansion bomb ("billion laughs").
    dtd = [b'<?xml version="1.0" encoding="UTF-8"?>', b'<!DOCTYPE w:document [', b' <!ENTITY lol "lol">']
    for i in range(1, 10):
        dtd.append(b' <!ENTITY lol%d "%s">' % (i, b"".join([b"&lol%s;" % (b"" if i == 1 else b"%d" % (i - 1))] * 10)))
    dtd.append(b"]>")
    laughs = b"\r\n".join(dtd) + DOC_OPEN[DOC_OPEN.index(b"<w:document"):] + b"<w:p><w:r><w:t>&lol9;</w:t></w:r></w:p>" + DOC_CLOSE
    save(MALICIOUS, "dtd-entity-expansion.docx", write_zip(with_document(entries, laughs)))

    # No [Content_Types].xml.
    pptx_entries = [(n, b) for n, b in read_entries(simple_pptx) if n != "[Content_Types].xml"]
    save(MALICIOUS, "missing-content-types.pptx", write_zip(pptx_entries))

    # Zip encryption, which Office files never use.
    encrypted = [deflated_entry(n, b, flags=1 if n == "word/document.xml" else 0) for n, b in entries]
    save(MALICIOUS, "zip-encrypted-entry.docx", build_zip(encrypted))

    # Two entries with the same name.
    dupes = [deflated_entry(n, b) for n, b in entries] + [deflated_entry("word/document.xml", DOC_OPEN + DOC_CLOSE)]
    save(MALICIOUS, "duplicate-part-names.docx", build_zip(dupes))

    # Badly formed XML, an undeclared prefix, an undefined entity.
    save(MALICIOUS, "malformed-xml.docx",
         write_zip(with_document(entries, DOC_OPEN + b"<w:p><w:r><w:t>unclosed</w:r></w:p>" + DOC_CLOSE)))
    save(MALICIOUS, "undeclared-prefix.docx",
         write_zip(with_document(entries, DOC_OPEN + b"<w:p><x:r/></w:p>" + DOC_CLOSE)))
    save(MALICIOUS, "undefined-entity.docx",
         write_zip(with_document(entries, DOC_OPEN + b"<w:p><w:r><w:t>&nbsp;</w:t></w:r></w:p>" + DOC_CLOSE)))

    # Not a zip at all, and a zip cut in half.
    save(MALICIOUS, "not-a-zip.docx", b"This is a text file with a .docx name.\r\n" * 20)
    save(MALICIOUS, "truncated.xlsx", simple_xlsx[: len(simple_xlsx) // 2])


def main():
    print("Generated:")
    simple_docx = docx_simple()
    save(GENERATED, "simple.docx", simple_docx)
    save(GENERATED, "tables-images-links.docx", docx_tables_images())
    save(GENERATED, "sections-headers.docx", docx_sections())
    save(GENERATED, "styles-unicode-comments.docx", docx_styles_unicode())
    simple_xlsx = xlsx_simple()
    save(GENERATED, "simple.xlsx", simple_xlsx)
    save(GENERATED, "formatting.xlsx", xlsx_formatting())
    save(GENERATED, "sheets-chart-names.xlsx", xlsx_sheets_chart())
    save(GENERATED, "picture.xlsx", xlsx_image())
    simple_pptx = pptx_simple()
    save(GENERATED, "simple.pptx", simple_pptx)
    save(GENERATED, "shapes-table-chart.pptx", pptx_shapes_table_chart())
    save(GENERATED, "layouts.pptx", pptx_layouts())

    print("Edge cases:")
    save(EDGE, "alternate-content.docx", edge_alternate_content(simple_docx))
    save(EDGE, "extlst.xlsx", edge_extlst(simple_xlsx))
    save(EDGE, "unknown-namespaces-custom-xml.docx", edge_unknown_namespaces(simple_docx))
    save(EDGE, "utf16-part.xlsx", edge_utf16(simple_xlsx))
    save(EDGE, "escaped-part-names.docx", edge_escaped_names(simple_docx))
    edge_zip_layouts(simple_docx, simple_xlsx, simple_pptx)

    print("Malicious and damaged:")
    malicious(simple_docx, simple_xlsx, simple_pptx)


if __name__ == "__main__":
    main()
