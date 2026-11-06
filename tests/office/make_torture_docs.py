"""Word documents for the editing torture test's CI subset (tests/office/torture).

The structures the editor must keep intact and that the other public files lack: a table of
contents as Word writes it (hyperlinked entries, each with a PAGEREF field to a _Toc bookmark),
Word's empty _GoBack bookmark, fields inside content controls and a simple field. Written by hand
(the standard library only), deterministically: run it again and the file is byte for byte the
same.

    python tests/office/make_torture_docs.py
"""

import os
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "torture")

W = "http://schemas.openxmlformats.org/wordprocessingml/2006/main"
R = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
HEADINGS = ["Introduction", "Background", "Method", "Results", "Discussion", "Limitations", "Conclusion",
            "References"]
BODY = ("Featherweight opens documents quickly and never damages them. The quick brown fox jumps over the "
        "lazy dog, and the edits around this sentence must leave every field where Word expects it.")


def run(text, rpr=""):
    space = ' xml:space="preserve"' if text != text.strip() else ""
    return f"<w:r>{rpr}<w:t{space}>{text}</w:t></w:r>"


def field(code, result):
    return ('<w:r><w:fldChar w:fldCharType="begin"/></w:r>'
            f'<w:r><w:instrText xml:space="preserve"> {code} </w:instrText></w:r>'
            '<w:r><w:fldChar w:fldCharType="separate"/></w:r>'
            f'{run(result)}<w:r><w:fldChar w:fldCharType="end"/></w:r>')


def toc():
    paras = []
    for i, heading in enumerate(HEADINGS, 1):
        start = ""
        if i == 1:  # the TOC field begins in the first entry's paragraph, as Word writes it
            start = ('<w:r><w:fldChar w:fldCharType="begin"/></w:r>'
                     '<w:r><w:instrText xml:space="preserve"> TOC \\o "1-3" \\h \\z \\u </w:instrText></w:r>'
                     '<w:r><w:fldChar w:fldCharType="separate"/></w:r>')
        pageref = field("PAGEREF _Toc%d \\h" % i, str(i))
        entry = (f'<w:hyperlink w:anchor="_Toc{i}" w:history="1">{run(f"{i}. {heading}")}<w:r><w:tab/></w:r>'
                 f'{pageref}</w:hyperlink>')
        paras.append('<w:p><w:pPr><w:pStyle w:val="TOC1"/><w:tabs><w:tab w:val="right" w:leader="dot" w:pos="9350"/>'
                     f'</w:tabs></w:pPr>{start}{entry}</w:p>')
    paras.append('<w:p><w:r><w:fldChar w:fldCharType="end"/></w:r></w:p>')
    return "".join(paras)


def body():
    paras = ['<w:p><w:pPr><w:pStyle w:val="Title"/></w:pPr>' + run("Contents") + "</w:p>", toc()]
    for i, heading in enumerate(HEADINGS, 1):
        paras.append(f'<w:p><w:pPr><w:pStyle w:val="Heading1"/></w:pPr><w:bookmarkStart w:id="{i}" w:name="_Toc{i}"/>'
                     f'{run(f"{i}. {heading}")}<w:bookmarkEnd w:id="{i}"/></w:p>')
        extra = ""
        if i == 2:  # Word's empty _GoBack bookmark, where the caret was
            extra = '<w:bookmarkStart w:id="100" w:name="_GoBack"/><w:bookmarkEnd w:id="100"/>'
        if i == 3:  # a date field inside an inline content control
            extra = ('<w:sdt><w:sdtPr><w:id w:val="200"/><w:alias w:val="Date"/></w:sdtPr><w:sdtContent>'
                     + field('DATE \\@ "yyyy-MM-dd"', "2026-11-05") + "</w:sdtContent></w:sdt>")
        if i == 4:  # a simple field, and a reference to the first heading
            extra = '<w:fldSimple w:instr=" PAGE ">' + run("4") + "</w:fldSimple>" + run(" See ") + field("REF _Toc1 \\h", "1. Introduction")
        paras.append("<w:p>" + run(BODY + " ") + extra + run(" More text follows here.") + "</w:p>")
    paras.append('<w:sdt><w:sdtPr><w:id w:val="300"/><w:alias w:val="Signature"/></w:sdtPr><w:sdtContent>'
                 "<w:p>" + run("Signed: ") + field("AUTHOR", "Featherweight") + "</w:p></w:sdtContent></w:sdt>")
    return "".join(paras)


def document():
    return (f'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<w:document xmlns:w="{W}" xmlns:r="{R}"><w:body>'
            + body() + '<w:sectPr><w:pgSz w:w="12240" w:h="15840"/><w:pgMar w:top="1440" w:right="1440" w:bottom="1440" '
            'w:left="1440" w:header="720" w:footer="720" w:gutter="0"/></w:sectPr></w:body></w:document>')


def styles():
    def style(kind, sid, name, ppr="", rpr="", default=False):
        d = ' w:default="1"' if default else ""
        return (f'<w:style w:type="{kind}"{d} w:styleId="{sid}"><w:name w:val="{name}"/>'
                + ("" if sid == "Normal" or kind == "character" else '<w:basedOn w:val="Normal"/>')
                + (f"<w:pPr>{ppr}</w:pPr>" if ppr else "") + (f"<w:rPr>{rpr}</w:rPr>" if rpr else "") + "</w:style>")
    return (f'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<w:styles xmlns:w="{W}">'
            '<w:docDefaults><w:rPrDefault><w:rPr><w:sz w:val="22"/></w:rPr></w:rPrDefault></w:docDefaults>'
            + style("paragraph", "Normal", "Normal", default=True)
            + style("paragraph", "Title", "Title", rpr='<w:sz w:val="40"/>')
            + style("paragraph", "Heading1", "heading 1", ppr='<w:keepNext/><w:outlineLvl w:val="0"/>', rpr='<w:b/><w:sz w:val="28"/>')
            + style("paragraph", "TOC1", "toc 1")
            + style("character", "Hyperlink", "Hyperlink", rpr='<w:color w:val="0563C1"/><w:u w:val="single"/>')
            + "</w:styles>")


PARTS = {
    "[Content_Types].xml": (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
        '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
        '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
        '<Default Extension="xml" ContentType="application/xml"/>'
        '<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>'
        '<Override PartName="/word/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"/>'
        "</Types>"),
    "_rels/.rels": (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
        '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
        '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" '
        'Target="word/document.xml"/></Relationships>'),
    "word/_rels/document.xml.rels": (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
        '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
        '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" '
        'Target="styles.xml"/></Relationships>'),
}


def merged_table():
    """A table as Word leaves one after years of edits (as in a World Bank contract in the corpus):
    a header row that starts one grid column in (w:gridBefore), so its cells' edges don't line up
    with the body's, and body rows whose cells span two grid columns, with identical properties
    (the reader shares them)."""
    widths = [10, 1554, 10, 3371, 8280]
    grid = "".join(f'<w:gridCol w:w="{w}"/>' for w in widths)
    def cell(text, width, span=1, extra=""):
        span_xml = f'<w:gridSpan w:val="{span}"/>' if span > 1 else ""
        return (f'<w:tc><w:tcPr><w:tcW w:w="{width}" w:type="dxa"/>{span_xml}{extra}</w:tcPr>'
                f"<w:p>{run(text)}</w:p></w:tc>")
    shd = '<w:shd w:val="clear" w:color="auto" w:fill="auto"/>'
    rows = ['<w:tr><w:trPr><w:gridBefore w:val="1"/><w:wBefore w:w="10" w:type="dxa"/><w:tblHeader/></w:trPr>'
            + cell("Issue", 1564, 2) + cell("Example clause", 3371) + cell("Sources", 8280) + "</w:tr>"]
    for issue in ["Site mobilization", "Community relations", "Third party access", "Security of the site", "Solar"]:
        rows.append("<w:tr>" + cell(issue, 1564, 2) + cell("Consider the clause", 3381, 2, shd)
                    + cell("Added to the sub-clause", 8280, 1, shd) + "</w:tr>")
    return ('<w:tbl><w:tblPr><w:tblStyle w:val="TableGrid"/><w:tblW w:w="13225" w:type="dxa"/>'
            '<w:tblInd w:w="-10" w:type="dxa"/><w:tblLook w:val="04A0" w:firstRow="1" w:lastRow="0" '
            'w:firstColumn="1" w:lastColumn="0" w:noHBand="0" w:noVBand="1"/></w:tblPr>'
            f"<w:tblGrid>{grid}</w:tblGrid>" + "".join(rows) + "</w:tbl>")


def tables_document():
    body = ('<w:p><w:pPr><w:pStyle w:val="Title"/></w:pPr>' + run("Sustainable procurement") + "</w:p>"
            + merged_table() + "<w:p>" + run("The clauses above are examples.") + "</w:p>")
    return (f'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<w:document xmlns:w="{W}" xmlns:r="{R}"><w:body>'
            + body + '<w:sectPr><w:pgSz w:w="16838" w:h="11906" w:orient="landscape"/><w:pgMar w:top="1440" '
            'w:right="1440" w:bottom="1440" w:left="1440" w:header="720" w:footer="720" w:gutter="0"/></w:sectPr>'
            "</w:body></w:document>")


def received_table_document():
    """A table as one arrives by mail, for the friction review's "fix a table" flow
    (docs/friction-review.md, tests/friction-checks.ps1): four columns with a header row and grid
    lines (Table Grid, its borders set on the table, as the style isn't in styles.xml), and a typo in
    a cell."""
    widths = [3000, 1500, 2000, 2000]
    grid = "".join(f'<w:gridCol w:w="{w}"/>' for w in widths)
    def row(texts, header=False):
        trpr = "<w:trPr><w:tblHeader/></w:trPr>" if header else ""
        rpr = "<w:rPr><w:b/></w:rPr>" if header else ""
        return ("<w:tr>" + trpr + "".join(f'<w:tc><w:tcPr><w:tcW w:w="{w}" w:type="dxa"/></w:tcPr><w:p>{run(t, rpr)}</w:p></w:tc>'
                                           for t, w in zip(texts, widths)) + "</w:tr>")
    rows = [row(["Item", "Quantity", "Price", "Total"], True)]
    for texts in [["Desk", "2", "250", "500"], ["Chiar", "4", "80", "320"], ["Lamp", "3", "40", "120"], ["Shelf", "1", "150", "150"]]:
        rows.append(row(texts))
    borders = "".join(f'<w:{side} w:val="single" w:sz="4" w:space="0" w:color="auto"/>'
                      for side in ["top", "left", "bottom", "right", "insideH", "insideV"])
    table = ('<w:tbl><w:tblPr><w:tblStyle w:val="TableGrid"/><w:tblW w:w="0" w:type="auto"/>'
             f'<w:tblBorders>{borders}</w:tblBorders>'
             '<w:tblLook w:val="04A0" w:firstRow="1" w:lastRow="0" w:firstColumn="1" w:lastColumn="0" w:noHBand="0" '
             f'w:noVBand="1"/></w:tblPr><w:tblGrid>{grid}</w:tblGrid>' + "".join(rows) + "</w:tbl>")
    body = ("<w:p>" + run("Quote for the new office") + "</w:p>" + table + "<w:p>" + run("Prices in euros, without VAT.")
            + "</w:p>")
    return (f'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<w:document xmlns:w="{W}" xmlns:r="{R}"><w:body>'
            + body + '<w:sectPr><w:pgSz w:w="11906" w:h="16838"/><w:pgMar w:top="1440" w:right="1440" w:bottom="1440" '
            'w:left="1440" w:header="720" w:footer="720" w:gutter="0"/></w:sectPr></w:body></w:document>')


TRANSITIONAL = {
    "w": W, "r": R,
    "rel": "http://schemas.openxmlformats.org/officeDocument/2006/relationships/",
}
STRICT = {
    "w": "http://purl.oclc.org/ooxml/wordprocessingml/main",
    "r": "http://purl.oclc.org/ooxml/officeDocument/relationships",
    "rel": "http://purl.oclc.org/ooxml/officeDocument/relationships/",
}
MC = "http://schemas.openxmlformats.org/markup-compatibility/2006"
W14 = "http://schemas.microsoft.com/office/word/2010/wordml"
WORDML = "application/vnd.openxmlformats-officedocument.wordprocessingml."


def headers_parts(ns):
    """Two sections; the first has a header and a footer full of what unlinking and editing them must
    keep whole (a PAGE field, a link, a content control, a bookmark, mc:AlternateContent, an element
    Word doesn't know), the second shows them through Link to Previous."""
    strict = ns is STRICT
    root_ns = (f'xmlns:w="{ns["w"]}" xmlns:r="{ns["r"]}" xmlns:mc="{MC}" xmlns:w14="{W14}" '
               'xmlns:x="urn:featherweight:test" mc:Ignorable="w14 x"')
    conformance = ' w:conformance="strict"' if strict else ""
    decl = '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
    header = (decl + f"<w:hdr {root_ns}><w:p><w:pPr><w:pStyle w:val=\"Header\"/></w:pPr>"
              + run("Annual report, page ") + field("PAGE", "1") + run(" ")
              + '<w:hyperlink r:id="rIdLink" w:history="1">' + run("example.com") + "</w:hyperlink>" + run(" ")
              + '<w:sdt><w:sdtPr><w:id w:val="400"/><w:alias w:val="Company"/></w:sdtPr><w:sdtContent>'
              + run("Featherweight Ltd") + "</w:sdtContent></w:sdt>"
              + '<w:bookmarkStart w:id="50" w:name="HeaderMark"/>' + run(" marked") + '<w:bookmarkEnd w:id="50"/>'
              + '<mc:AlternateContent><mc:Choice Requires="w14">' + run(" (new)") + "</mc:Choice>"
              + "<mc:Fallback>" + run(" (old)") + "</mc:Fallback></mc:AlternateContent>"
              + "<x:kept>kept by Featherweight</x:kept>" + "</w:p></w:hdr>")
    footer = (decl + f"<w:ftr {root_ns}><w:p><w:pPr><w:pStyle w:val=\"Footer\"/><w:jc w:val=\"center\"/></w:pPr>"
              + run("Page ") + '<w:fldSimple w:instr=" PAGE ">' + run("1") + "</w:fldSimple>" + run(" of ")
              + '<w:fldSimple w:instr=" NUMPAGES ">' + run("2") + "</w:fldSimple>"
              + run(" ") + '<w:hyperlink w:anchor="Intro">' + run("back to the start") + "</w:hyperlink></w:p></w:ftr>")
    paras = ['<w:p><w:pPr><w:pStyle w:val="Title"/></w:pPr><w:bookmarkStart w:id="1" w:name="Intro"/>'
             + run("Annual report") + '<w:bookmarkEnd w:id="1"/></w:p>']
    for i in range(1, 7):
        paras.append("<w:p>" + run(f"{i}. {BODY}") + "</w:p>")
    sect1 = ('<w:sectPr><w:headerReference w:type="default" r:id="rIdHdr"/>'
             '<w:footerReference w:type="default" r:id="rIdFtr"/><w:pgSz w:w="12240" w:h="15840"/>'
             '<w:pgMar w:top="1440" w:right="1440" w:bottom="1440" w:left="1440" w:header="720" w:footer="720" '
             'w:gutter="0"/></w:sectPr>')
    paras.append("<w:p><w:pPr>" + sect1 + "</w:pPr>" + run("The end of the first section.") + "</w:p>")
    for i in range(7, 11):
        paras.append("<w:p>" + run(f"{i}. {BODY}") + "</w:p>")
    body = "".join(paras) + ('<w:sectPr><w:type w:val="nextPage"/><w:pgSz w:w="12240" w:h="15840"/>'
                             '<w:pgMar w:top="1440" w:right="1440" w:bottom="1440" w:left="1440" w:header="720" '
                             'w:footer="720" w:gutter="0"/></w:sectPr>')
    document_xml = (decl + f'<w:document {root_ns}{conformance}><w:body>' + body + "</w:body></w:document>")
    styles_xml = styles()
    if strict:
        styles_xml = styles_xml.replace(W, ns["w"])
        header = header.replace(W, ns["w"])
    rels = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
            '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
            f'<Relationship Id="rId1" Type="{ns["rel"]}styles" Target="styles.xml"/>'
            f'<Relationship Id="rIdHdr" Type="{ns["rel"]}header" Target="header1.xml"/>'
            f'<Relationship Id="rIdFtr" Type="{ns["rel"]}footer" Target="footer1.xml"/></Relationships>')
    header_rels = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
                   '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                   f'<Relationship Id="rIdLink" Type="{ns["rel"]}hyperlink" Target="https://example.com/" '
                   'TargetMode="External"/></Relationships>')
    types = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
             '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
             '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
             '<Default Extension="xml" ContentType="application/xml"/>'
             f'<Override PartName="/word/document.xml" ContentType="{WORDML}document.main+xml"/>'
             f'<Override PartName="/word/styles.xml" ContentType="{WORDML}styles+xml"/>'
             f'<Override PartName="/word/header1.xml" ContentType="{WORDML}header+xml"/>'
             f'<Override PartName="/word/footer1.xml" ContentType="{WORDML}footer+xml"/></Types>')
    package_rels = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
                    '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                    f'<Relationship Id="rId1" Type="{ns["rel"]}officeDocument" Target="word/document.xml"/>'
                    "</Relationships>")
    return {"[Content_Types].xml": types, "_rels/.rels": package_rels, "word/_rels/document.xml.rels": rels,
            "word/document.xml": document_xml, "word/styles.xml": styles_xml, "word/header1.xml": header,
            "word/_rels/header1.xml.rels": header_rels, "word/footer1.xml": footer}


PLACEHOLDER = '<w:rPr><w:rStyle w:val="PlaceholderText"/></w:rPr>'


def sdt(sdt_id, alias, choice, content, placeholder=False, lock=""):
    """An inline content control as Word writes it: its properties, then the runs it holds."""
    pr = (f'<w:alias w:val="{alias}"/><w:tag w:val="{alias.lower()}"/><w:id w:val="{sdt_id}"/>'
          + (f'<w:lock w:val="{lock}"/>' if lock else "") + ("<w:showingPlcHdr/>" if placeholder else "") + choice)
    return f"<w:sdt><w:sdtPr>{pr}</w:sdtPr><w:sdtContent>{content}</w:sdtContent></w:sdt>"


def list_items(*items):
    return '<w:listItem w:displayText="Choose an item." w:value=""/>' + "".join(
        f'<w:listItem w:displayText="{i}" w:value="{i}"/>' for i in items)


def form_field(name, kind_xml, instr, result=None):
    """A legacy form field: w:ffData in its begin, and a result only when it has one."""
    begin = (f'<w:r><w:fldChar w:fldCharType="begin"><w:ffData><w:name w:val="{name}"/><w:enabled/>'
             f'<w:calcOnExit w:val="0"/>{kind_xml}</w:ffData></w:fldChar></w:r>'
             f'<w:r><w:instrText xml:space="preserve"> {instr} </w:instrText></w:r>')
    if result is None:
        return begin + '<w:r><w:fldChar w:fldCharType="end"/></w:r>'
    return begin + '<w:r><w:fldChar w:fldCharType="separate"/></w:r>' + run(result) + '<w:r><w:fldChar w:fldCharType="end"/></w:r>'


def forms_parts(protected):
    """A form of every kind of content control and legacy form field the editor fills in: rich and
    plain text (one locked), check box, drop-down list, combo box and date controls, a block control
    around two paragraphs, controls in a table, and FORMTEXT, FORMCHECKBOX and FORMDROPDOWN. With
    `protected`, forms protection is enforced (only the fill-ins can change)."""
    decl = '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
    root_ns = f'xmlns:w="{W}" xmlns:r="{R}" xmlns:mc="{MC}" xmlns:w14="{W14}" mc:Ignorable="w14"'
    box = ('<w14:checkbox><w14:checked w14:val="0"/><w14:checkedState w14:val="2612" w14:font="MS Gothic"/>'
           '<w14:uncheckedState w14:val="2610" w14:font="MS Gothic"/></w14:checkbox>')
    gothic = '<w:rPr><w:rFonts w:ascii="MS Gothic" w:eastAsia="MS Gothic" w:hAnsi="MS Gothic" w:hint="eastAsia"/></w:rPr>'
    placeholder_text = f"<w:r>{PLACEHOLDER}<w:t>Click or tap here to enter text.</w:t></w:r>"
    choose = f"<w:r>{PLACEHOLDER}<w:t>Choose an item.</w:t></w:r>"
    paras = ['<w:p><w:pPr><w:pStyle w:val="Title"/></w:pPr>' + run("Application form") + "</w:p>",
             "<w:p>" + run("Name: ") + sdt(501, "Name", "", placeholder_text, placeholder=True) + "</w:p>",
             "<w:p>" + run("Reference: ") + sdt(502, "Reference", "<w:text/>", run("FW-2026-001")) + run(" (plain text)") + "</w:p>",
             "<w:p>" + sdt(503, "Agree", box, f"<w:r>{gothic}<w:t>☐</w:t></w:r>") + run(" I agree to the terms.") + "</w:p>",
             "<w:p>" + run("Colour: ") + sdt(504, "Colour", "<w:dropDownList>" + list_items("Red", "Green", "Blue") + "</w:dropDownList>",
                                            choose, placeholder=True) + "</w:p>",
             "<w:p>" + run("Size: ") + sdt(505, "Size", "<w:comboBox>" + list_items("Small", "Large") + "</w:comboBox>",
                                          run("Medium")) + "</w:p>",
             "<w:p>" + run("Start date: ") + sdt(506, "Start", '<w:date w:fullDate="2026-11-05T00:00:00Z"><w:dateFormat w:val="d MMMM yyyy"/>'
                                                 '<w:lid w:val="en-GB"/><w:storeMappedDataAs w:val="dateTime"/>'
                                                 '<w:calendar w:val="gregorian"/></w:date>', run("5 November 2026")) + "</w:p>",
             "<w:p>" + run("Office use: ") + sdt(507, "Office", "", run("Do not change this"), lock="contentLocked") + "</w:p>",
             ('<w:sdt><w:sdtPr><w:alias w:val="Statement"/><w:tag w:val="statement"/><w:id w:val="508"/></w:sdtPr><w:sdtContent>'
              "<w:p>" + run("Your statement, first paragraph.") + "</w:p><w:p>" + run("And its second paragraph.")
              + "</w:p></w:sdtContent></w:sdt>"),
             "<w:p>" + run("Legacy name: ") + form_field("Text1", "<w:textInput/>", "FORMTEXT", " " * 5) + "</w:p>",
             "<w:p>" + form_field("Check1", '<w:checkBox><w:sizeAuto/><w:default w:val="0"/></w:checkBox>', "FORMCHECKBOX")
             + run(" Send me the newsletter.") + "</w:p>",
             "<w:p>" + run("Legacy size: ") + form_field("Dropdown1", '<w:ddList><w:listEntry w:val="Small"/><w:listEntry w:val="Medium"/>'
                                                        '<w:listEntry w:val="Large"/></w:ddList>', "FORMDROPDOWN") + "</w:p>"]
    cell = '<w:tc><w:tcPr><w:tcW w:w="4675" w:type="dxa"/></w:tcPr><w:p>{}</w:p></w:tc>'
    paras.append('<w:tbl><w:tblPr><w:tblW w:w="9350" w:type="dxa"/><w:tblBorders><w:top w:val="single" w:sz="4" w:space="0" w:color="auto"/>'
                 '<w:bottom w:val="single" w:sz="4" w:space="0" w:color="auto"/><w:insideH w:val="single" w:sz="4" w:space="0" w:color="auto"/>'
                 '</w:tblBorders></w:tblPr><w:tblGrid><w:gridCol w:w="4675"/><w:gridCol w:w="4675"/></w:tblGrid>'
                 "<w:tr>" + cell.format(run("Phone")) + cell.format(sdt(509, "Phone", "<w:text/>", placeholder_text, placeholder=True)) + "</w:tr>"
                 "<w:tr>" + cell.format(run("Contact by post")) + cell.format(sdt(510, "Post", box, f"<w:r>{gothic}<w:t>☐</w:t></w:r>"))
                 + "</w:tr></w:tbl>")
    paras.append("<w:p>" + run("Thank you for applying.") + "</w:p>")
    body = "".join(paras) + ('<w:sectPr><w:pgSz w:w="12240" w:h="15840"/><w:pgMar w:top="1440" w:right="1440" w:bottom="1440" '
                             'w:left="1440" w:header="720" w:footer="720" w:gutter="0"/></w:sectPr>')
    document_xml = decl + f"<w:document {root_ns}><w:body>{body}</w:body></w:document>"
    styles_xml = styles().replace("</w:styles>", '<w:style w:type="character" w:styleId="PlaceholderText"><w:name w:val="Placeholder Text"/>'
                                                 '<w:uiPriority w:val="99"/><w:semiHidden/>'
                                                 '<w:rPr><w:color w:val="808080"/></w:rPr></w:style></w:styles>')
    protection = '<w:documentProtection w:edit="forms" w:enforcement="1"/>' if protected else ""
    settings_xml = decl + f'<w:settings xmlns:w="{W}">{protection}<w:defaultTabStop w:val="720"/></w:settings>'
    rel = TRANSITIONAL["rel"]
    rels = (decl + '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
            f'<Relationship Id="rId1" Type="{rel}styles" Target="styles.xml"/>'
            f'<Relationship Id="rId2" Type="{rel}settings" Target="settings.xml"/></Relationships>')
    types = (decl + '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
             '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
             '<Default Extension="xml" ContentType="application/xml"/>'
             f'<Override PartName="/word/document.xml" ContentType="{WORDML}document.main+xml"/>'
             f'<Override PartName="/word/styles.xml" ContentType="{WORDML}styles+xml"/>'
             f'<Override PartName="/word/settings.xml" ContentType="{WORDML}settings+xml"/></Types>')
    package_rels = (decl + '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                    f'<Relationship Id="rId1" Type="{rel}officeDocument" Target="word/document.xml"/></Relationships>')
    return {"[Content_Types].xml": types, "_rels/.rels": package_rels, "word/_rels/document.xml.rels": rels,
            "word/document.xml": document_xml, "word/styles.xml": styles_xml, "word/settings.xml": settings_xml}


def write_parts(name, parts):
    path = os.path.join(OUT, name)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for part, text in parts.items():
            info = zipfile.ZipInfo(part, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, text.encode("utf-8"))
    print(path)


def write(name, document_xml):
    parts = dict(PARTS)
    parts["word/document.xml"] = document_xml
    parts["word/styles.xml"] = styles()
    path = os.path.join(OUT, name)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for part, text in parts.items():
            info = zipfile.ZipInfo(part, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, text.encode("utf-8"))
    print(path)


def main():
    os.makedirs(OUT, exist_ok=True)
    write("toc-fields.docx", document())
    write("merged-table.docx", tables_document())
    write("received-table.docx", received_table_document())
    write_parts("headers-fields.docx", headers_parts(TRANSITIONAL))
    write_parts("headers-fields-strict.docx", headers_parts(STRICT))
    write_parts("forms.docx", forms_parts(protected=False))
    write_parts("forms-protected.docx", forms_parts(protected=True))


if __name__ == "__main__":
    main()
