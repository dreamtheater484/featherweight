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


if __name__ == "__main__":
    main()
