"""Generates tests/office/layout/: Word documents for the D1 layout measurement (plan section 10,
milestone D1). Each one stresses what decides where lines and pages break:

  hyphenation.docx         automatic hyphenation in justified text, soft and non-breaking hyphens
  footnotes.docx           footnotes that wrap and continue, an endnote, headers and a PAGE field
  tabs-columns.docx        right, center and decimal tabs with leaders, ligatures, two columns
  textbox-table.docx       a floating text box with square wrapping, a table that splits across
                           pages with a repeated header row, superscript, small caps and caps
  report.docx              ten pages of headings and paragraphs with different line spacing,
                           keep-with-next, widow control and a page break

Written by hand with the standard library only (no venv needed), in the WordprocessingML Word
writes, with Windows' own fonts (Calibri, Cambria). The output is deterministic. From the
repository root:
  python tests/office/make_layout_docs.py
Then make their reference renders on the Office reference PC:
  powershell -ExecutionPolicy Bypass -File scripts\\reference-renders.ps1 -Folders tests\\office\\layout
"""
import io
import os
import zipfile
from xml.sax.saxutils import escape

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LAYOUT = os.path.join(ROOT, "tests", "office", "layout")
ZIP_DATE = (2026, 1, 1, 12, 0, 0)

NS = {
    "w": "http://schemas.openxmlformats.org/wordprocessingml/2006/main",
    "r": "http://schemas.openxmlformats.org/officeDocument/2006/relationships",
    "mc": "http://schemas.openxmlformats.org/markup-compatibility/2006",
    "w14": "http://schemas.microsoft.com/office/word/2010/wordml",
    "wp": "http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing",
    "a": "http://schemas.openxmlformats.org/drawingml/2006/main",
    "wps": "http://schemas.microsoft.com/office/word/2010/wordprocessingShape",
    "v": "urn:schemas-microsoft-com:vml",
    "o": "urn:schemas-microsoft-com:office:office",
    "w10": "urn:schemas-microsoft-com:office:word",
}
XMLNS = " ".join(f'xmlns:{k}="{v}"' for k, v in NS.items()) + ' mc:Ignorable="w14"'
HEAD = '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\r\n'
CT = "application/vnd.openxmlformats-officedocument.wordprocessingml."
RT = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/"

# ---------- Text ----------

WORDS = (
    "the of and to in is that for it as with was on be by at this from or which an are have not but "
    "page line word text layout break measure document reader office paragraph column table footnote "
    "header footer section margin spacing justified hyphen ligature compatibility characterization "
    "internationalization incomprehensibility responsibilities administration representative "
    "extraordinary communication infrastructure interoperability approximately notwithstanding "
    "considerable environment establishment simultaneously typographical proportional "
    "office official affidavit fluently final efficient flow fifty "
    "small light fast quiet clear plain simple careful exact steady honest local private"
).split()


class Text:
    """Deterministic pseudo-English, so regenerating changes nothing."""

    def __init__(self, seed):
        self.state = seed

    def next(self, n):
        self.state = (self.state * 1103515245 + 12345) & 0x7FFFFFFF
        return self.state % n

    def sentence(self, low=8, high=20):
        words = [WORDS[self.next(len(WORDS))] for _ in range(low + self.next(high - low + 1))]
        s = " ".join(words)
        return s[0].upper() + s[1:] + "."

    def paragraph(self, sentences=4):
        return " ".join(self.sentence() for _ in range(sentences))


# ---------- WordprocessingML ----------


def run(text, rpr=""):
    rpr = f"<w:rPr>{rpr}</w:rPr>" if rpr else ""
    return f'<w:r>{rpr}<w:t xml:space="preserve">{escape(text)}</w:t></w:r>'


def para(content, ppr="", style=None):
    if style:
        ppr = f'<w:pStyle w:val="{style}"/>' + ppr
    ppr = f"<w:pPr>{ppr}</w:pPr>" if ppr else ""
    return f"<w:p>{ppr}{content}</w:p>"


def text_para(text, ppr="", style=None, rpr=""):
    return para(run(text, rpr), ppr, style)


def tab():
    return "<w:r><w:tab/></w:r>"


def field(instr, result):
    return (f'<w:r><w:fldChar w:fldCharType="begin"/></w:r><w:r><w:instrText xml:space="preserve"> {instr} '
            f'</w:instrText></w:r><w:r><w:fldChar w:fldCharType="separate"/></w:r>{run(result)}'
            f'<w:r><w:fldChar w:fldCharType="end"/></w:r>')


def note_ref(kind, nid):
    style = "FootnoteReference" if kind == "footnote" else "EndnoteReference"
    return f'<w:r><w:rPr><w:rStyle w:val="{style}"/></w:rPr><w:{kind}Reference w:id="{nid}"/></w:r>'


def sect_pr(header=None, footer=None, w=12240, h=15840, margins=(1440, 1440, 1440, 1440), cols=1,
            kind=None, title_pg=False):
    top, right, bottom, left = margins
    refs = ""
    if header:
        refs += f'<w:headerReference w:type="default" r:id="{header}"/>'
    if footer:
        refs += f'<w:footerReference w:type="default" r:id="{footer}"/>'
    t = f'<w:type w:val="{kind}"/>' if kind else ""
    orient = ' w:orient="landscape"' if w > h else ""
    return (f'<w:sectPr>{refs}{t}<w:pgSz w:w="{w}" w:h="{h}"{orient}/>'
            f'<w:pgMar w:top="{top}" w:right="{right}" w:bottom="{bottom}" w:left="{left}" w:header="720" '
            f'w:footer="720" w:gutter="0"/><w:cols w:num="{cols}" w:space="720"/>'
            f'{"<w:titlePg/>" if title_pg else ""}<w:docGrid w:linePitch="360"/></w:sectPr>')


def styles(font="Calibri", heading_font=None, size=22):
    heading_font = heading_font or font

    def pstyle(sid, name, ppr="", rpr="", based="Normal", next_="Normal"):
        return (f'<w:style w:type="paragraph" w:styleId="{sid}"><w:name w:val="{name}"/>'
                f'<w:basedOn w:val="{based}"/><w:next w:val="{next_}"/><w:qFormat/>'
                f'<w:pPr>{ppr}</w:pPr><w:rPr>{rpr}</w:rPr></w:style>')

    hf = f'<w:rFonts w:ascii="{heading_font}" w:hAnsi="{heading_font}" w:cs="{heading_font}"/>'
    return (HEAD + f'<w:styles {XMLNS}><w:docDefaults><w:rPrDefault><w:rPr>'
            f'<w:rFonts w:ascii="{font}" w:eastAsia="{font}" w:hAnsi="{font}" w:cs="{font}"/>'
            f'<w:sz w:val="{size}"/><w:szCs w:val="{size}"/><w:lang w:val="en-US" w:eastAsia="en-US" w:bidi="ar-SA"/>'
            '</w:rPr></w:rPrDefault><w:pPrDefault><w:pPr><w:spacing w:after="160" w:line="259" w:lineRule="auto"/>'
            '</w:pPr></w:pPrDefault></w:docDefaults>'
            '<w:style w:type="paragraph" w:default="1" w:styleId="Normal"><w:name w:val="Normal"/><w:qFormat/>'
            '<w:pPr><w:widowControl/></w:pPr></w:style>'
            '<w:style w:type="character" w:default="1" w:styleId="DefaultParagraphFont">'
            '<w:name w:val="Default Paragraph Font"/><w:uiPriority w:val="1"/><w:semiHidden/></w:style>'
            + pstyle("Heading1", "heading 1", '<w:keepNext/><w:keepLines/><w:spacing w:before="360" w:after="80"/>'
                     '<w:outlineLvl w:val="0"/>', hf + '<w:b/><w:sz w:val="32"/><w:szCs w:val="32"/>')
            + pstyle("Heading2", "heading 2", '<w:keepNext/><w:keepLines/><w:spacing w:before="160" w:after="80"/>'
                     '<w:outlineLvl w:val="1"/>', hf + '<w:b/><w:sz w:val="26"/><w:szCs w:val="26"/>')
            + pstyle("Header", "header", '<w:tabs><w:tab w:val="center" w:pos="4680"/><w:tab w:val="right" '
                     'w:pos="9360"/></w:tabs><w:spacing w:after="0" w:line="240" w:lineRule="auto"/>')
            + pstyle("Footer", "footer", '<w:tabs><w:tab w:val="center" w:pos="4680"/><w:tab w:val="right" '
                     'w:pos="9360"/></w:tabs><w:spacing w:after="0" w:line="240" w:lineRule="auto"/>')
            + pstyle("FootnoteText", "footnote text", '<w:spacing w:after="0" w:line="240" w:lineRule="auto"/>',
                     '<w:sz w:val="20"/><w:szCs w:val="20"/>')
            + pstyle("EndnoteText", "endnote text", '<w:spacing w:after="0" w:line="240" w:lineRule="auto"/>',
                     '<w:sz w:val="20"/><w:szCs w:val="20"/>')
            + '<w:style w:type="character" w:styleId="FootnoteReference"><w:name w:val="footnote reference"/>'
            '<w:rPr><w:vertAlign w:val="superscript"/></w:rPr></w:style>'
            '<w:style w:type="character" w:styleId="EndnoteReference"><w:name w:val="endnote reference"/>'
            '<w:rPr><w:vertAlign w:val="superscript"/></w:rPr></w:style>'
            '<w:style w:type="table" w:default="1" w:styleId="TableNormal"><w:name w:val="Normal Table"/>'
            '<w:tblPr><w:tblInd w:w="0" w:type="dxa"/><w:tblCellMar><w:top w:w="0" w:type="dxa"/>'
            '<w:left w:w="108" w:type="dxa"/><w:bottom w:w="0" w:type="dxa"/><w:right w:w="108" w:type="dxa"/>'
            '</w:tblCellMar></w:tblPr></w:style>'
            '<w:style w:type="table" w:styleId="TableGrid"><w:name w:val="Table Grid"/>'
            '<w:basedOn w:val="TableNormal"/><w:pPr><w:spacing w:after="0" w:line="240" w:lineRule="auto"/></w:pPr>'
            '<w:tblPr><w:tblBorders><w:top w:val="single" w:sz="4" w:space="0" w:color="auto"/>'
            '<w:left w:val="single" w:sz="4" w:space="0" w:color="auto"/>'
            '<w:bottom w:val="single" w:sz="4" w:space="0" w:color="auto"/>'
            '<w:right w:val="single" w:sz="4" w:space="0" w:color="auto"/>'
            '<w:insideH w:val="single" w:sz="4" w:space="0" w:color="auto"/>'
            '<w:insideV w:val="single" w:sz="4" w:space="0" w:color="auto"/></w:tblBorders></w:tblPr></w:style>'
            '</w:styles>')


def settings(hyphenate=False, notes=False):
    return (HEAD + f'<w:settings {XMLNS}><w:zoom w:percent="100"/><w:defaultTabStop w:val="720"/>'
            + ('<w:autoHyphenation/><w:hyphenationZone w:val="357"/>' if hyphenate else "")
            + '<w:characterSpacingControl w:val="doNotCompress"/>'
            + ('<w:footnotePr><w:footnote w:id="-1"/><w:footnote w:id="0"/></w:footnotePr>'
               '<w:endnotePr><w:endnote w:id="-1"/><w:endnote w:id="0"/></w:endnotePr>' if notes else "")
            + '<w:compat><w:compatSetting w:name="compatibilityMode" w:uri="http://schemas.microsoft.com/office/word" '
            'w:val="15"/></w:compat></w:settings>')


def notes_part(kind, notes):
    """kind is "footnote" or "endnote"; notes are (id, text)."""
    style = "FootnoteText" if kind == "footnote" else "EndnoteText"
    body = (f'<w:{kind} w:type="separator" w:id="-1"><w:p><w:pPr><w:spacing w:after="0" w:line="240" '
            f'w:lineRule="auto"/></w:pPr><w:r><w:separator/></w:r></w:p></w:{kind}>'
            f'<w:{kind} w:type="continuationSeparator" w:id="0"><w:p><w:pPr><w:spacing w:after="0" w:line="240" '
            f'w:lineRule="auto"/></w:pPr><w:r><w:continuationSeparator/></w:r></w:p></w:{kind}>')
    ref_style = "FootnoteReference" if kind == "footnote" else "EndnoteReference"
    for nid, text in notes:
        body += (f'<w:{kind} w:id="{nid}"><w:p><w:pPr><w:pStyle w:val="{style}"/></w:pPr>'
                 f'<w:r><w:rPr><w:rStyle w:val="{ref_style}"/></w:rPr><w:{kind}Ref/></w:r>'
                 f'{run(" " + text)}</w:p></w:{kind}>')
    return HEAD + f'<w:{kind}s {XMLNS}>{body}</w:{kind}s>'


def header_footer(tag, paragraphs):
    return HEAD + f'<w:{tag} {XMLNS}>{"".join(paragraphs)}</w:{tag}>'


def package(body, style_xml, settings_xml, parts=(), xmlns=XMLNS):
    """parts: (relationship id, type, target, content type, xml) for headers, footers and notes."""
    rels = [("rIdStyles", "styles", "styles.xml"), ("rIdSettings", "settings", "settings.xml")]
    overrides = [("/word/document.xml", CT + "document.main+xml"), ("/word/styles.xml", CT + "styles+xml"),
                 ("/word/settings.xml", CT + "settings+xml")]
    entries = []
    for rid, kind, target, ctype, xml in parts:
        rels.append((rid, kind, target))
        overrides.append(("/word/" + target, CT + ctype))
        entries.append(("word/" + target, xml))
    types = (HEAD + '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
             '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
             '<Default Extension="xml" ContentType="application/xml"/>'
             + "".join(f'<Override PartName="{n}" ContentType="{t}"/>' for n, t in overrides) + "</Types>")
    root_rels = (HEAD + '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                 f'<Relationship Id="rId1" Type="{RT}officeDocument" Target="word/document.xml"/></Relationships>')
    doc_rels = (HEAD + '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                + "".join(f'<Relationship Id="{i}" Type="{RT}{k}" Target="{t}"/>' for i, k, t in rels)
                + "</Relationships>")
    document = HEAD + f'<w:document {xmlns}><w:body>{body}</w:body></w:document>'
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in [("[Content_Types].xml", types), ("_rels/.rels", root_rels),
                           ("word/document.xml", document), ("word/_rels/document.xml.rels", doc_rels),
                           ("word/styles.xml", style_xml), ("word/settings.xml", settings_xml)] + entries:
            info = zipfile.ZipInfo(name, date_time=ZIP_DATE)
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data.encode("utf-8"))
    return out.getvalue()


def header_parts(header_text, footer_paragraphs):
    return [
        ("rIdHeader1", "header", "header1.xml", "header+xml",
         header_footer("hdr", [para(run(t), style="Header") for t in header_text])),
        ("rIdFooter1", "footer", "footer1.xml", "footer+xml", header_footer("ftr", footer_paragraphs)),
    ]


# ---------- Documents ----------

JUSTIFY = '<w:jc w:val="both"/>'


def hyphenation():
    t = Text(1)
    body = text_para("Automatic hyphenation", style="Heading1")
    for i in range(5):
        body += text_para(t.paragraph(5), JUSTIFY)
    # A soft hyphen Word may use, a non-breaking hyphen it may not, and hard hyphens it may break after.
    body += para(run("Soft hyphens mark where a word may break: super") + "<w:r><w:softHyphen/></w:r>"
                 + run("cali") + "<w:r><w:softHyphen/></w:r>" + run("fragilistic") + "<w:r><w:softHyphen/></w:r>"
                 + run("expialidocious, while non") + "<w:r><w:noBreakHyphen/></w:r>"
                 + run("breaking hyphens never break, and well-known, up-to-date, state-of-the-art hard "
                       "hyphens may. ") + run(t.paragraph(3)), JUSTIFY)
    # Narrow columns make hyphenation work hard.
    for i in range(3):
        body += text_para(t.paragraph(4), '<w:ind w:left="2880" w:right="1440"/>' + JUSTIFY)
    body += text_para(t.paragraph(4), '<w:suppressAutoHyphens/>' + JUSTIFY)
    body += sect_pr(margins=(1440, 1800, 1440, 1800))
    return package(body, styles("Cambria", size=22), settings(hyphenate=True))


def footnotes():
    t = Text(2)
    notes, endnotes = [], [(1, "The endnote, at the end of the document. " + t.sentence())]
    body = text_para("Footnotes and endnotes", style="Heading1")
    nid = 1
    for i in range(14):
        content = run(t.sentence() + " " + t.sentence(4, 6))
        if i % 2 == 0:
            content += note_ref("footnote", nid)
            # Some notes are long, so they wrap and sometimes continue on the next page.
            notes.append((nid, t.paragraph(1 + (nid % 3) * 2)))
            nid += 1
        content += run(" " + t.paragraph(2))
        if i == 9:
            content += note_ref("endnote", 1)
        body += para(content)
    body += sect_pr("rIdHeader1", "rIdFooter1")
    parts = header_parts(["Footnotes and endnotes", "A running header on two lines"],
                         [para(run("Page ") + field("PAGE", "1") + tab() + run("Featherweight tests"),
                               style="Footer")])
    parts += [("rIdFootnotes", "footnotes", "footnotes.xml", "footnotes+xml", notes_part("footnote", notes)),
              ("rIdEndnotes", "endnotes", "endnotes.xml", "endnotes+xml", notes_part("endnote", endnotes))]
    return package(body, styles("Calibri"), settings(notes=True), parts)


LIGATURES = '<w14:ligatures w14:val="standard"/>'


def tabs_columns():
    t = Text(3)
    body = text_para("Tabs, ligatures and columns", style="Heading1")
    leaders = '<w:tabs><w:tab w:val="right" w:leader="dot" w:pos="9360"/></w:tabs>'
    for name, page in [("Introduction", "1"), ("Office affidavits and official files", "4"),
                       ("Efficient flow of fifty final figures", "12"), ("Appendix", "107")]:
        body += para(run(name) + tab() + run(page), leaders + '<w:spacing w:after="0"/>')
    body += text_para("")
    stops = ('<w:tabs><w:tab w:val="left" w:pos="1440"/><w:tab w:val="center" w:pos="4320"/>'
             '<w:tab w:val="decimal" w:pos="7200"/></w:tabs><w:spacing w:after="0"/>')
    for row in [("Item", "Description", "Amount"), ("1", "Office supplies", "12.50"),
                ("2", "Fluent affidavits", "1,250.00"), ("3", "Fifty files", "7.5")]:
        body += para(run(row[0]) + tab() + run(row[1]) + tab() + run(row[2]), stops)
    body += text_para("")
    body += para(run("Ligatures: " + "office, official, affidavit, fluently, efficient, fifty, final, flow. ",
                     LIGATURES) + run(t.paragraph(3), LIGATURES))
    body += para("", sect_pr())  # ends the one-column section
    for i in range(9):
        body += text_para(t.paragraph(3), JUSTIFY if i % 2 else "")
    body += sect_pr(cols=2, kind="continuous")
    return package(body, styles("Calibri"), settings())


def text_box(text_paragraphs):
    content = "".join(text_paragraphs)
    cx, cy = 2286000, 1600200
    return (
        '<w:r><mc:AlternateContent><mc:Choice Requires="wps"><w:drawing>'
        '<wp:anchor distT="0" distB="0" distL="114300" distR="114300" simplePos="0" relativeHeight="251659264" '
        'behindDoc="0" locked="0" layoutInCell="1" allowOverlap="1"><wp:simplePos x="0" y="0"/>'
        '<wp:positionH relativeFrom="margin"><wp:align>right</wp:align></wp:positionH>'
        '<wp:positionV relativeFrom="paragraph"><wp:posOffset>0</wp:posOffset></wp:positionV>'
        f'<wp:extent cx="{cx}" cy="{cy}"/><wp:effectExtent l="0" t="0" r="0" b="0"/>'
        '<wp:wrapSquare wrapText="bothSides"/><wp:docPr id="1" name="Text Box 1"/><wp:cNvGraphicFramePr/>'
        '<a:graphic><a:graphicData uri="http://schemas.microsoft.com/office/word/2010/wordprocessingShape">'
        '<wps:wsp><wps:cNvSpPr txBox="1"/><wps:spPr><a:xfrm><a:off x="0" y="0"/>'
        f'<a:ext cx="{cx}" cy="{cy}"/></a:xfrm><a:prstGeom prst="rect"><a:avLst/></a:prstGeom>'
        '<a:solidFill><a:srgbClr val="FFFFFF"/></a:solidFill><a:ln w="6350"><a:solidFill><a:srgbClr val="000000"/>'
        f'</a:solidFill></a:ln></wps:spPr><wps:txbx><w:txbxContent>{content}</w:txbxContent></wps:txbx>'
        '<wps:bodyPr rot="0" vert="horz" wrap="square" lIns="91440" tIns="45720" rIns="91440" bIns="45720" '
        'anchor="t" anchorCtr="0"><a:noAutofit/></wps:bodyPr></wps:wsp></a:graphicData></a:graphic></wp:anchor>'
        '</w:drawing></mc:Choice><mc:Fallback><w:pict><v:shapetype id="_x0000_t202" coordsize="21600,21600" '
        'o:spt="202" path="m,l,21600r21600,l21600,xe"><v:stroke joinstyle="miter"/><v:path gradientshapeok="t" '
        'o:connecttype="rect"/></v:shapetype><v:shape id="Text Box 1" o:spid="_x0000_s1026" '
        'type="#_x0000_t202" style="position:absolute;margin-left:0;margin-top:0;width:180pt;height:126pt;'
        'z-index:251659264;mso-position-horizontal:right;mso-position-horizontal-relative:margin" '
        f'fillcolor="white" strokeweight=".5pt"><v:textbox><w:txbxContent>{content}</w:txbxContent></v:textbox>'
        '<w10:wrap type="square"/></v:shape></w:pict></mc:Fallback></mc:AlternateContent></w:r>')


def cell(text, width, header=False):
    rpr = "<w:b/>" if header else ""
    return (f'<w:tc><w:tcPr><w:tcW w:w="{width}" w:type="dxa"/></w:tcPr>'
            f'{para(run(text, rpr))}</w:tc>')


def textbox_table():
    t = Text(4)
    body = text_para("A text box and a long table", style="Heading1")
    box = [text_para("Text box", rpr="<w:b/>"), text_para(t.paragraph(2))]
    body += para(text_box(box) + run(t.paragraph(4)))
    for i in range(3):
        body += text_para(t.paragraph(3))
    body += para(run("Formatting that changes widths: E = mc") + run("2", '<w:vertAlign w:val="superscript"/>')
                 + run(", H") + run("2", '<w:vertAlign w:val="subscript"/>') + run("O, ")
                 + run("small capitals that take less room, ", "<w:smallCaps/>")
                 + run("all capitals that take more, ", "<w:caps/>") + run(t.paragraph(2)))
    widths = (1800, 3780, 3780)
    rows = ('<w:tr><w:trPr><w:tblHeader/></w:trPr>'
            + "".join(cell(h, w, True) for h, w in zip(("Row", "First column", "Second column"), widths)) + "</w:tr>")
    for i in range(1, 31):
        rows += ("<w:tr>" + cell(str(i), widths[0]) + cell(t.sentence(4, 14), widths[1])
                 + cell(t.sentence(3, 10), widths[2]) + "</w:tr>")
    body += ('<w:tbl><w:tblPr><w:tblStyle w:val="TableGrid"/><w:tblW w:w="0" w:type="auto"/>'
             '<w:tblLook w:val="04A0" w:firstRow="1" w:lastRow="0" w:firstColumn="1" w:lastColumn="0" '
             'w:noHBand="0" w:noVBand="1"/></w:tblPr><w:tblGrid>'
             + "".join(f'<w:gridCol w:w="{w}"/>' for w in widths) + "</w:tblGrid>" + rows + "</w:tbl>")
    body += text_para(t.paragraph(2))
    body += sect_pr()
    return package(body, styles("Calibri"), settings())


def report():
    t = Text(5)
    body = ""
    spacing = ['<w:spacing w:after="160" w:line="259" w:lineRule="auto"/>',
               '<w:spacing w:after="120" w:line="360" w:lineRule="auto"/>',
               '<w:spacing w:before="60" w:after="60" w:line="300" w:lineRule="exact"/>',
               '<w:spacing w:after="200" w:line="280" w:lineRule="atLeast"/>']
    for chapter in range(1, 7):
        body += text_para(f"Chapter {chapter}. " + t.sentence(3, 6)[:-1], style="Heading1")
        for section in range(1, 3):
            body += text_para(f"{chapter}.{section} " + t.sentence(2, 5)[:-1], style="Heading2")
            for i in range(3 + (chapter + section) % 3):
                ppr = spacing[(chapter + section + i) % 4] + (JUSTIFY if (chapter + i) % 2 else "")
                body += text_para(t.paragraph(3 + i % 3), ppr)
        if chapter == 3:
            body += para('<w:r><w:br w:type="page"/></w:r>')
    body += sect_pr("rIdHeader1", "rIdFooter1")
    parts = header_parts(["Featherweight layout report"],
                         [para(tab() + run("Page ") + field("PAGE", "1") + run(" of ") + field("NUMPAGES", "1"),
                               style="Footer")])
    return package(body, styles("Cambria", "Calibri"), settings(), parts)


# ---------- Equations (OMML, D5) ----------


def mr(text, sty=None, scr=None):
    props = (f'<m:sty m:val="{sty}"/>' if sty else "") + (f'<m:scr m:val="{scr}"/>' if scr else "")
    props = f"<m:rPr>{props}</m:rPr>" if props else ""
    return f'<m:r>{props}<m:t xml:space="preserve">{escape(text)}</m:t></m:r>'


def mx(tag, *content, pr=""):
    return f"<m:{tag}>" + (f"<m:{tag}Pr>{pr}</m:{tag}Pr>" if pr else "") + "".join(content) + f"</m:{tag}>"


def arg(tag, *content):
    return f"<m:{tag}>" + "".join(content) + f"</m:{tag}>"


def val(tag, v):
    return f'<m:{tag} m:val="{escape(v)}"/>'


def frac(num, den, kind=None):
    return mx("f", arg("num", num), arg("den", den), pr=val("type", kind) if kind else "")


def ssup(base, s):
    return mx("sSup", arg("e", base), arg("sup", s))


def ssub(base, s):
    return mx("sSub", arg("e", base), arg("sub", s))


def subsup(base, b, t):
    return mx("sSubSup", arg("e", base), arg("sub", b), arg("sup", t))


def rad(e, deg=None):
    return mx("rad", arg("deg", deg or ""), arg("e", e), pr="" if deg else val("degHide", "1"))


def nary(chr_, low, high, e, loc=None):
    pr = val("chr", chr_) + (val("limLoc", loc) if loc else "")
    return mx("nary", arg("sub", low), arg("sup", high), arg("e", e), pr=pr)


def delim(*args, beg=None, end=None):
    pr = (val("begChr", beg) if beg is not None else "") + (val("endChr", end) if end is not None else "")
    return mx("d", *[arg("e", a) for a in args], pr=pr)


def func(name, e):
    return mx("func", arg("fName", mr(name, "p")), arg("e", e))


def omath(*content):
    return "<m:oMath>" + "".join(content) + "</m:oMath>"


def display(*content):
    return para("<m:oMathPara>" + omath(*content) + "</m:oMathPara>")


def equations():
    """Every OMML construct the reader shows, inline in text and as display equations."""
    x2 = ssup(mr("x"), mr("2"))
    body = text_para("Equations", style="Heading1")
    body += para(run("The area of a circle is ") + omath(mr("A=π"), ssup(mr("r"), mr("2"))) +
                 run(", and a small fraction ") + omath(frac(mr("a"), mr("b"))) + run(" sits in the line, as do ") +
                 omath(frac(mr("1"), mr("2"), "lin")) + run(" and ") + omath(frac(mr("3"), mr("4"), "skw")) + run("."))
    body += display(mr("x="), frac(mr("-b±") + rad(ssup(mr("b"), mr("2")) + mr("-4ac")),
                                    mr("2a")))
    body += display(nary("∑", mr("i=1"), mr("n"), mr("i")), mr("="), frac(mr("n") + delim(mr("n+1")), mr("2")))
    body += display(nary("∫", mr("0"), mr("∞"), ssup(mr("e"), mr("-") + x2) + mr("d") + mr("x")), mr("="),
                    frac(rad(mr("π")), mr("2")))
    body += display(mr("A="), delim(mx("m", arg("mr", arg("e", mr("a")), arg("e", mr("b"))),
                                          arg("mr", arg("e", mr("c")), arg("e", mr("d"))))),
                    mr(","), mr(" "), mr("det"), delim(mr("A"), beg="|", end="|"), mr("=ad-bc"))
    body += display(mr("f"), delim(mr("x")), mr("="),
                    delim(mx("eqArr", arg("e", mr("x,") + mr("&") + mr("x≥0")),
                             arg("e", mr("-x,") + mr("&") + mr("x<0"))), beg="{", end=""))
    body += para(run("A limit inline: ") +
                 omath(mx("func", arg("fName", mx("limLow", arg("e", mr("lim", "p")), arg("lim", mr("x→0")))),
                          arg("e", frac(func("sin", mr("x")), mr("x")))), mr("=1")) +
                 run(", and a sum inline: ") + omath(nary("∑", mr("k=0"), mr("∞"), frac(mr("1"), ssup(mr("2"), mr("k"))))) +
                 run(" which equals two."))
    body += display(mx("acc", arg("e", mr("x"))), mr("+"), mx("bar", arg("e", mr("y")), pr=val("pos", "top")), mr("+"),
                    mx("acc", arg("e", mr("v")), pr=val("chr", "⃗")), mr("+"),
                    mx("acc", arg("e", mr("abc")), pr=val("chr", "̃")), mr("+"), rad(mr("x+1"), mr("3")))
    body += display(mx("groupChr", arg("e", mr("a+b+c"))), mr("+"), mx("borderBox", arg("e", mr("E=m") + ssup(mr("c"), mr("2")))),
                    mr("+"), mx("bar", arg("e", mr("z"))))
    body += display(mr("1+"), frac(mr("1"), mr("1+") + frac(mr("1"), mr("1+") + frac(mr("1"), mr("x")))))
    body += display(nary("∏", mr("j=1"), mr("m"), ssub(mr("a"), mr("j"))), mr("≤"),
                    nary("∮", mr("C"), mr(""), mr("F·d") + mr("r"), "subSup"), mr("+"),
                    subsup(mr("T"), mr("ij"), mr("k")), mr("∈"), mr("R", scr="double-struck"))
    body += para("<m:oMathPara>" + omath(mr("a=b+c")) + omath(mr("d=e-f")) + "</m:oMathPara>")
    body += text_para("The last paragraph is plain text after the equations.")
    body += sect_pr()
    math = ' xmlns:m="http://schemas.openxmlformats.org/officeDocument/2006/math"'
    return package(body, styles("Cambria", "Calibri"), settings(), xmlns=XMLNS + math)


def main():
    os.makedirs(LAYOUT, exist_ok=True)
    for name, make in [("hyphenation.docx", hyphenation), ("footnotes.docx", footnotes),
                       ("tabs-columns.docx", tabs_columns), ("textbox-table.docx", textbox_table),
                       ("report.docx", report), ("equations.docx", equations)]:
        data = make()
        with open(os.path.join(LAYOUT, name), "wb") as f:
            f.write(data)
        print(f"  {os.path.relpath(os.path.join(LAYOUT, name), ROOT)}  ({len(data):,} bytes)")


if __name__ == "__main__":
    main()
