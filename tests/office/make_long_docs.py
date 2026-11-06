"""Makes long Word documents for timing and memory measurements (plan section 11 budgets: a
typical 20-page document and a 300-page one).

The XML is written the way Word 365 writes it: Word's namespace declarations and mc:Ignorable
list, rsid and w14 paragraph ids on every paragraph, runs split by revision session, proofing
marks, a full styles part with latent styles, a theme, a table of contents field, headings with
bookmarks, lists, tables, footnotes, comments, links, pictures and a footer with a page field.

Uses only the standard library, and the output is deterministic. From the repository root:
  python tests/office/make_long_docs.py <output folder> [pages ...]
Without pages, it makes long-20.docx and long-300.docx.
"""
import os
import random
import sys
import zipfile
import zlib
import struct

W_NS = (
    'xmlns:wpc="http://schemas.microsoft.com/office/word/2010/wordprocessingCanvas" '
    'xmlns:cx="http://schemas.microsoft.com/office/drawing/2014/chartex" '
    'xmlns:cx1="http://schemas.microsoft.com/office/drawing/2015/9/8/chartex" '
    'xmlns:aink="http://schemas.microsoft.com/office/drawing/2016/ink" '
    'xmlns:am3d="http://schemas.microsoft.com/office/drawing/2017/model3d" '
    'xmlns:o="urn:schemas-microsoft-com:office:office" '
    'xmlns:oel="http://schemas.microsoft.com/office/2019/extlst" '
    'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" '
    'xmlns:m="http://schemas.openxmlformats.org/officeDocument/2006/math" '
    'xmlns:v="urn:schemas-microsoft-com:vml" '
    'xmlns:wp14="http://schemas.microsoft.com/office/word/2010/wordprocessingDrawing" '
    'xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing" '
    'xmlns:w10="urn:schemas-microsoft-com:office:word" '
    'xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main" '
    'xmlns:w14="http://schemas.microsoft.com/office/word/2010/wordml" '
    'xmlns:w15="http://schemas.microsoft.com/office/word/2012/wordml" '
    'xmlns:w16cex="http://schemas.microsoft.com/office/word/2018/wordml/cex" '
    'xmlns:w16cid="http://schemas.microsoft.com/office/word/2016/wordml/cid" '
    'xmlns:w16="http://schemas.microsoft.com/office/word/2018/wordml" '
    'xmlns:w16du="http://schemas.microsoft.com/office/word/2023/wordml/word16du" '
    'xmlns:w16sdtdh="http://schemas.microsoft.com/office/word/2020/wordml/sdtdatahash" '
    'xmlns:w16se="http://schemas.microsoft.com/office/word/2015/wordml/symex" '
    'xmlns:wpg="http://schemas.microsoft.com/office/word/2010/wordprocessingGroup" '
    'xmlns:wpi="http://schemas.microsoft.com/office/word/2010/wordprocessingInk" '
    'xmlns:wne="http://schemas.microsoft.com/office/word/2006/wordml" '
    'xmlns:wps="http://schemas.microsoft.com/office/word/2010/wordprocessingShape" '
    'xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006" '
    'mc:Ignorable="w14 w15 w16se w16cid w16 w16cex w16sdtdh w16du wp14"'
)
DECL = '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\r\n'
WORDS = (
    "the of and to in a is that for it as was with be by on not he this are or his from at which but have an "
    "they you were her she there been one all we their has would when if so no what up out can more about "
    "other into some could them time only new these two may then do first any my now such like our over "
    "man me even most made after also did many before must through back years where much your way well down "
    "should because each just those people how too little state good very make world still own see men work "
    "long get here between both life being under never day same another know while last might us great old "
    "year off come since against go came right used take three report budget council project school results "
    "survey market customer service quarter analysis approach method evidence policy review section figure"
).split()


class Doc:
    def __init__(self, pages, seed):
        self.rng = random.Random(seed)
        self.pages = pages
        self.rsids = ["00%06X" % self.rng.randrange(0x1000000) for _ in range(40)]
        self.footnotes = []
        self.comments = []
        self.headings = []
        self.bookmark = 0
        self.ids = set()

    def para_id(self):
        while True:
            v = "%08X" % self.rng.randrange(0x7FFFFFFF)
            if v not in self.ids:
                self.ids.add(v)
                return v

    def p_open(self, extra=""):
        r = self.rng.choice(self.rsids)
        return '<w:p w14:paraId="%s" w14:textId="%s" w:rsidR="%s" w:rsidRDefault="%s" w:rsidP="%s"%s>' % (
            self.para_id(), self.para_id(), r, self.rng.choice(self.rsids), r, extra)

    def words(self, n):
        out = [self.rng.choice(WORDS) for _ in range(n)]
        out[0] = out[0].capitalize()
        return " ".join(out)

    def run(self, text, rpr="", rsid=True):
        attr = ' w:rsidR="%s"' % self.rng.choice(self.rsids) if rsid and self.rng.random() < 0.4 else ""
        space = ' xml:space="preserve"' if text != text.strip() else ""
        return '<w:r%s>%s<w:t%s>%s</w:t></w:r>' % (attr, "<w:rPr>%s</w:rPr>" % rpr if rpr else "", space, text)

    def body_para(self, style=None, numbered=None):
        rng = self.rng
        ppr = ""
        if style:
            ppr += '<w:pStyle w:val="%s"/>' % style
        if numbered:
            ppr += '<w:numPr><w:ilvl w:val="0"/><w:numId w:val="%d"/></w:numPr>' % numbered
        if rng.random() < 0.3:
            ppr += '<w:spacing w:after="120" w:line="276" w:lineRule="auto"/>'
        if rng.random() < 0.2:
            ppr += '<w:jc w:val="both"/>'
        parts = [self.p_open(), "<w:pPr>%s</w:pPr>" % ppr if ppr else ""]
        n = rng.randint(12, 30) if numbered else rng.randint(70, 130)
        done = 0
        while done < n:
            k = min(n - done, rng.randint(3, 14))
            text = self.words(k) if done == 0 else " " + " ".join(rng.choice(WORDS) for _ in range(k))
            roll = rng.random()
            if roll < 0.25:  # pasted or reformatted text: direct fonts and size on the run
                parts.append(self.run(text, '<w:rFonts w:ascii="Calibri" w:hAnsi="Calibri" w:cs="Calibri"/>'
                                            '<w:sz w:val="22"/><w:szCs w:val="22"/>'))
            elif roll < 0.33:
                parts.append(self.run(text, '<w:b/><w:bCs/>'))
            elif roll < 0.39:
                parts.append(self.run(text, '<w:i/><w:iCs/>'))
            elif roll < 0.45:
                parts.append('<w:proofErr w:type="spellStart"/>' + self.run(text) + '<w:proofErr w:type="spellEnd"/>')
            elif roll < 0.48:
                parts.append('<w:hyperlink r:id="rIdLink" w:history="1">%s</w:hyperlink>' %
                             self.run(text, '<w:rStyle w:val="Hyperlink"/>', False))
            elif roll < 0.52:
                parts.append(self.run(text, '<w:lang w:val="en-GB"/>'))
            else:
                parts.append(self.run(text))
            done += k
        if not numbered and rng.random() < 0.12:
            fid = len(self.footnotes) + 1
            self.footnotes.append(self.words(rng.randint(8, 25)) + ".")
            parts.append('<w:r><w:rPr><w:rStyle w:val="FootnoteReference"/></w:rPr><w:footnoteReference w:id="%d"/></w:r>' % fid)
        if not numbered and rng.random() < 0.04:
            cid = len(self.comments)
            self.comments.append(self.words(rng.randint(5, 15)) + ".")
            parts.insert(2, '<w:commentRangeStart w:id="%d"/>' % cid)
            parts.append('<w:commentRangeEnd w:id="%d"/><w:r><w:rPr><w:rStyle w:val="CommentReference"/></w:rPr>'
                         '<w:commentReference w:id="%d"/></w:r>' % (cid, cid))
        parts.append(self.run("."))
        if rng.random() < 0.15:
            parts.insert(2, "<w:r><w:lastRenderedPageBreak/></w:r>")
        parts.append("</w:p>")
        return "".join(parts)

    def heading(self, level):
        text = self.words(self.rng.randint(2, 6))
        name = "_Toc%09d" % (100000000 + self.bookmark)
        self.headings.append((level, text, name))
        b = self.bookmark
        self.bookmark += 1
        return (self.p_open() + '<w:pPr><w:pStyle w:val="Heading%d"/></w:pPr><w:bookmarkStart w:id="%d" w:name="%s"/>'
                % (level, b, name) + self.run(text) + '<w:bookmarkEnd w:id="%d"/></w:p>' % b)

    def table(self):
        rng = self.rng
        cols, rows = 4, rng.randint(4, 8)
        out = ['<w:tbl><w:tblPr><w:tblStyle w:val="TableGrid"/><w:tblW w:w="0" w:type="auto"/>'
               '<w:tblLook w:val="04A0" w:firstRow="1" w:lastRow="0" w:firstColumn="1" w:lastColumn="0" '
               'w:noHBand="0" w:noVBand="1"/></w:tblPr><w:tblGrid>']
        out += ['<w:gridCol w:w="2337"/>'] * cols
        out.append("</w:tblGrid>")
        for r in range(rows):
            out.append('<w:tr w:rsidR="%s" w14:paraId="%s" w14:textId="%s">' % (rng.choice(self.rsids), self.para_id(), self.para_id()))
            for c in range(cols):
                shade = '<w:shd w:val="clear" w:color="auto" w:fill="D9E2F3" w:themeFill="accent1" w:themeFillTint="33"/>' if r == 0 else ""
                text = self.words(rng.randint(1, 4)) if r == 0 or c == 0 else "%d.%d" % (rng.randint(0, 999), rng.randint(0, 99))
                out.append('<w:tc><w:tcPr><w:tcW w:w="2337" w:type="dxa"/>%s</w:tcPr>%s<w:pPr><w:spacing w:after="0"/></w:pPr>%s</w:p></w:tc>'
                           % (shade, self.p_open(), self.run(text, "<w:b/><w:bCs/>" if r == 0 else "")))
            out.append("</w:tr>")
        out.append("</w:tbl>")
        return "".join(out)

    def picture(self, n):
        return (self.p_open() + '<w:pPr><w:jc w:val="center"/></w:pPr><w:r><w:rPr><w:noProof/></w:rPr><w:drawing>'
                '<wp:inline distT="0" distB="0" distL="0" distR="0" wp14:anchorId="%s" wp14:editId="%s">'
                '<wp:extent cx="3657600" cy="2057400"/><wp:effectExtent l="0" t="0" r="0" b="0"/>'
                '<wp:docPr id="%d" name="Picture %d" descr="A chart"/><wp:cNvGraphicFramePr>'
                '<a:graphicFrameLocks xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" noChangeAspect="1"/>'
                '</wp:cNvGraphicFramePr><a:graphic xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main">'
                '<a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/picture">'
                '<pic:pic xmlns:pic="http://schemas.openxmlformats.org/drawingml/2006/picture"><pic:nvPicPr>'
                '<pic:cNvPr id="%d" name="Picture %d"/><pic:cNvPicPr/></pic:nvPicPr><pic:blipFill>'
                '<a:blip r:embed="rIdImage1"><a:extLst><a:ext uri="{28A0092B-C50C-407E-A947-70E740481C1C}">'
                '<a14:useLocalDpi xmlns:a14="http://schemas.microsoft.com/office/drawing/2010/main" val="0"/></a:ext>'
                '</a:extLst></a:blip><a:stretch><a:fillRect/></a:stretch></pic:blipFill><pic:spPr><a:xfrm>'
                '<a:off x="0" y="0"/><a:ext cx="3657600" cy="2057400"/></a:xfrm><a:prstGeom prst="rect"><a:avLst/>'
                '</a:prstGeom></pic:spPr></pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>'
                % (self.para_id(), self.para_id(), n, n, n, n))

    def body(self):
        rng = self.rng
        content = []
        pictures = 0
        for page in range(self.pages):
            if page % 3 == 0:
                content.append(self.heading(1 if page % 9 == 0 else 2))
            for _ in range(rng.randint(3, 5)):
                content.append(self.body_para())
            if page % 4 == 1:
                for _ in range(rng.randint(3, 5)):
                    content.append(self.body_para("ListParagraph", 1 + (page // 4) % 2))
            if page % 10 == 5:
                content.append(self.table())
                content.append(self.body_para())
            if page % 25 == 12:
                pictures += 1
                content.append(self.picture(pictures))
        # The table of contents comes first, built from the headings.
        toc = [self.p_open() + '<w:pPr><w:pStyle w:val="TOCHeading"/></w:pPr>' + self.run("Contents") + "</w:p>"]
        for i, (level, text, name) in enumerate(self.headings):
            begin = ('<w:r><w:fldChar w:fldCharType="begin"/></w:r><w:r><w:instrText xml:space="preserve"> TOC \\o "1-3" \\h \\z \\u </w:instrText></w:r>'
                     '<w:r><w:fldChar w:fldCharType="separate"/></w:r>') if i == 0 else ""
            end = '<w:r><w:fldChar w:fldCharType="end"/></w:r>' if i == len(self.headings) - 1 else ""
            toc.append(self.p_open() + '<w:pPr><w:pStyle w:val="TOC%d"/><w:tabs><w:tab w:val="right" w:leader="dot" '
                       'w:pos="9350"/></w:tabs><w:rPr><w:noProof/></w:rPr></w:pPr>%s<w:hyperlink w:anchor="%s" w:history="1">'
                       '<w:r w:rsidRPr="%s"><w:rPr><w:rStyle w:val="Hyperlink"/><w:noProof/></w:rPr><w:t>%s</w:t></w:r>'
                       '<w:r><w:rPr><w:noProof/><w:webHidden/></w:rPr><w:tab/></w:r><w:r><w:rPr><w:noProof/><w:webHidden/></w:rPr>'
                       '<w:fldChar w:fldCharType="begin"/></w:r><w:r><w:rPr><w:noProof/><w:webHidden/></w:rPr><w:instrText '
                       'xml:space="preserve"> PAGEREF %s \\h </w:instrText></w:r><w:r><w:rPr><w:noProof/><w:webHidden/></w:rPr>'
                       '</w:r><w:r><w:rPr><w:noProof/><w:webHidden/></w:rPr><w:fldChar w:fldCharType="separate"/></w:r><w:r>'
                       '<w:rPr><w:noProof/><w:webHidden/></w:rPr><w:t>%d</w:t></w:r><w:r><w:rPr><w:noProof/><w:webHidden/>'
                       '</w:rPr><w:fldChar w:fldCharType="end"/></w:r></w:hyperlink>%s</w:p>'
                       % (min(level, 3), begin, name, rng.choice(self.rsids), text, name, 3 * i + 2, end))
        sect = ('<w:sectPr w:rsidR="%s"><w:footerReference w:type="default" r:id="rIdFooter1"/><w:footnotePr>'
                '<w:numRestart w:val="eachSect"/></w:footnotePr><w:pgSz w:w="12240" w:h="15840"/><w:pgMar w:top="1440" '
                'w:right="1440" w:bottom="1440" w:left="1440" w:header="720" w:footer="720" w:gutter="0"/>'
                '<w:cols w:space="720"/><w:docGrid w:linePitch="360"/></w:sectPr>' % self.rsids[0])
        return (DECL + "<w:document %s><w:body>" % W_NS + "".join(toc) + "".join(content) + sect +
                "</w:body></w:document>")

    def notes(self):
        sep = ('<w:footnote w:type="separator" w:id="-1"><w:p w14:paraId="%s" w14:textId="%s"><w:pPr><w:spacing w:after="0" '
               'w:line="240" w:lineRule="auto"/></w:pPr><w:r><w:separator/></w:r></w:p></w:footnote>'
               '<w:footnote w:type="continuationSeparator" w:id="0"><w:p w14:paraId="%s" w14:textId="%s"><w:pPr>'
               '<w:spacing w:after="0" w:line="240" w:lineRule="auto"/></w:pPr><w:r><w:continuationSeparator/></w:r>'
               '</w:p></w:footnote>' % (self.para_id(), self.para_id(), self.para_id(), self.para_id()))
        items = "".join(
            '<w:footnote w:id="%d">%s<w:pPr><w:pStyle w:val="FootnoteText"/></w:pPr><w:r><w:rPr><w:rStyle '
            'w:val="FootnoteReference"/></w:rPr><w:footnoteRef/></w:r>%s</w:p></w:footnote>'
            % (i + 1, self.p_open(), self.run(" " + text)) for i, text in enumerate(self.footnotes))
        return DECL + "<w:footnotes %s>" % W_NS + sep + items + "</w:footnotes>"

    def comments_xml(self):
        items, ex = [], []
        for i, text in enumerate(self.comments):
            pid = self.para_id()
            items.append('<w:comment w:id="%d" w:author="Sam Reviewer" w:date="2026-03-0%dT10:00:00Z" w:initials="SR">'
                         '<w:p w14:paraId="%s" w14:textId="77777777" w:rsidR="%s" w:rsidRDefault="%s"><w:pPr>'
                         '<w:pStyle w:val="CommentText"/></w:pPr><w:r><w:rPr><w:rStyle w:val="CommentReference"/></w:rPr>'
                         '<w:annotationRef/></w:r>%s</w:p></w:comment>'
                         % (i, 1 + i % 9, pid, self.rsids[1], self.rsids[2], self.run(text)))
            ex.append('<w15:commentEx w15:paraId="%s" w15:done="%d"/>' % (pid, i % 3 == 0))
        return (DECL + "<w:comments %s>" % W_NS + "".join(items) + "</w:comments>",
                DECL + "<w15:commentsEx %s>" % W_NS + "".join(ex) + "</w15:commentsEx>")

    def footer(self):
        return (DECL + "<w:ftr %s>" % W_NS + self.p_open() + '<w:pPr><w:pStyle w:val="Footer"/><w:jc w:val="center"/></w:pPr>'
                '<w:r><w:fldChar w:fldCharType="begin"/></w:r><w:r><w:instrText xml:space="preserve"> PAGE   \\* MERGEFORMAT '
                '</w:instrText></w:r><w:r><w:fldChar w:fldCharType="separate"/></w:r><w:r><w:rPr><w:noProof/></w:rPr><w:t>2'
                '</w:t></w:r><w:r><w:rPr><w:noProof/></w:rPr><w:fldChar w:fldCharType="end"/></w:r></w:p></w:ftr>')

    def settings(self):
        rsids = "".join('<w:rsid w:val="%s"/>' % r for r in self.rsids)
        return (DECL + "<w:settings %s>" % W_NS.replace('xmlns:w="', 'xmlns:sl="http://schemas.openxmlformats.org/schemaLibrary/2006/main" xmlns:w="') +
                '<w:zoom w:percent="100"/><w:proofState w:spelling="clean" w:grammar="clean"/><w:defaultTabStop w:val="720"/>'
                '<w:characterSpacingControl w:val="doNotCompress"/><w:footnotePr><w:footnote w:id="-1"/><w:footnote w:id="0"/>'
                '</w:footnotePr><w:compat><w:compatSetting w:name="compatibilityMode" w:uri="http://schemas.microsoft.com/office/word" '
                'w:val="15"/><w:compatSetting w:name="overrideTableStyleFontSizeAndJustification" '
                'w:uri="http://schemas.microsoft.com/office/word" w:val="1"/><w:compatSetting w:name="enableOpenTypeFeatures" '
                'w:uri="http://schemas.microsoft.com/office/word" w:val="1"/><w:compatSetting w:name="doNotFlipMirrorIndents" '
                'w:uri="http://schemas.microsoft.com/office/word" w:val="1"/><w:compatSetting w:name="differentiateMultirowTableHeaders" '
                'w:uri="http://schemas.microsoft.com/office/word" w:val="1"/></w:compat><w:rsids><w:rsidRoot w:val="%s"/>%s'
                '</w:rsids><m:mathPr><m:mathFont m:val="Cambria Math"/><m:brkBin m:val="before"/><m:brkBinSub m:val="--"/>'
                '<m:smallFrac m:val="0"/><m:dispDef/><m:lMargin m:val="0"/><m:rMargin m:val="0"/><m:defJc m:val="centerGroup"/>'
                '<m:wrapIndent m:val="1440"/><m:intLim m:val="subSup"/><m:naryLim m:val="undOvr"/></m:mathPr>'
                '<w:themeFontLang w:val="en-US"/><w:clrSchemeMapping w:bg1="light1" w:t1="dark1" w:bg2="light2" w:t2="dark2" '
                'w:accent1="accent1" w:accent2="accent2" w:accent3="accent3" w:accent4="accent4" w:accent5="accent5" '
                'w:accent6="accent6" w:hyperlink="hyperlink" w:followedHyperlink="followedHyperlink"/>'
                '<w:shapeDefaults><o:shapedefaults v:ext="edit" spidmax="1026"/><o:shapelayout v:ext="edit"><o:idmap v:ext="edit" '
                'data="1"/></o:shapelayout></w:shapeDefaults><w:decimalSymbol w:val="."/><w:listSeparator w:val=","/>'
                '<w14:docId w14:val="1A2B3C4D"/><w15:chartTrackingRefBased/><w15:docId w15:val="{5C2D9E1A-1111-4A2B-9C3D-0123456789AB}"/>'
                '</w:settings>' % (self.rsids[0], rsids))


def styles_xml():
    latent = "".join('<w:lsdException w:name="%s" w:semiHidden="1" w:unhideWhenUsed="1"/>' % n for n in
                     ["heading %d" % i for i in range(2, 10)] + ["toc %d" % i for i in range(1, 10)] +
                     ["Normal Indent", "footnote text", "annotation text", "header", "footer", "index heading",
                      "caption", "table of figures", "envelope address", "envelope return", "footnote reference",
                      "annotation reference", "line number", "page number", "endnote reference", "endnote text",
                      "table of authorities", "macro", "toa heading", "List", "List Bullet", "List Number"] +
                     ["Grid Table %d%s" % (i, s) for i in range(1, 8) for s in ["", " Light", " Accent 1", " Accent 2",
                                                                               " Accent 3", " Accent 4", " Accent 5", " Accent 6"]] +
                     ["List Table %d%s" % (i, s) for i in range(1, 8) for s in ["", " Light", " Accent 1", " Accent 2",
                                                                               " Accent 3", " Accent 4", " Accent 5", " Accent 6"]])

    def pstyle(sid, name, based, ppr, rpr, extra=""):
        return ('<w:style w:type="paragraph" w:styleId="%s"><w:name w:val="%s"/>%s<w:next w:val="Normal"/>%s<w:uiPriority w:val="9"/>'
                '<w:qFormat/><w:pPr>%s</w:pPr><w:rPr>%s</w:rPr></w:style>'
                % (sid, name, '<w:basedOn w:val="%s"/>' % based if based else "", extra, ppr, rpr))
    styles = [
        '<w:style w:type="paragraph" w:default="1" w:styleId="Normal"><w:name w:val="Normal"/><w:qFormat/></w:style>',
        pstyle("Heading1", "heading 1", "Normal", '<w:keepNext/><w:keepLines/><w:spacing w:before="360" w:after="80"/><w:outlineLvl w:val="0"/>',
               '<w:rFonts w:asciiTheme="majorHAnsi" w:eastAsiaTheme="majorEastAsia" w:hAnsiTheme="majorHAnsi" w:cstheme="majorBidi"/>'
               '<w:color w:val="0F4761" w:themeColor="accent1" w:themeShade="BF"/><w:sz w:val="40"/><w:szCs w:val="40"/>', '<w:link w:val="Heading1Char"/>'),
        pstyle("Heading2", "heading 2", "Normal", '<w:keepNext/><w:keepLines/><w:spacing w:before="160" w:after="80"/><w:outlineLvl w:val="1"/>',
               '<w:rFonts w:asciiTheme="majorHAnsi" w:eastAsiaTheme="majorEastAsia" w:hAnsiTheme="majorHAnsi" w:cstheme="majorBidi"/>'
               '<w:color w:val="0F4761" w:themeColor="accent1" w:themeShade="BF"/><w:sz w:val="32"/><w:szCs w:val="32"/>', '<w:link w:val="Heading2Char"/>'),
        pstyle("ListParagraph", "List Paragraph", "Normal", '<w:ind w:left="720"/><w:contextualSpacing/>', ""),
        pstyle("TOCHeading", "TOC Heading", "Heading1", '<w:outlineLvl w:val="9"/>', ""),
        pstyle("TOC1", "toc 1", "Normal", '<w:spacing w:after="100"/>', ""),
        pstyle("TOC2", "toc 2", "Normal", '<w:spacing w:after="100"/><w:ind w:left="220"/>', ""),
        pstyle("TOC3", "toc 3", "Normal", '<w:spacing w:after="100"/><w:ind w:left="440"/>', ""),
        pstyle("FootnoteText", "footnote text", "Normal", '<w:spacing w:after="0" w:line="240" w:lineRule="auto"/>', '<w:sz w:val="20"/><w:szCs w:val="20"/>'),
        pstyle("CommentText", "annotation text", "Normal", '<w:spacing w:line="240" w:lineRule="auto"/>', '<w:sz w:val="20"/><w:szCs w:val="20"/>'),
        pstyle("Footer", "footer", "Normal", '<w:tabs><w:tab w:val="center" w:pos="4680"/><w:tab w:val="right" w:pos="9360"/></w:tabs><w:spacing w:after="0" w:line="240" w:lineRule="auto"/>', ""),
        '<w:style w:type="character" w:default="1" w:styleId="DefaultParagraphFont"><w:name w:val="Default Paragraph Font"/><w:uiPriority w:val="1"/><w:semiHidden/><w:unhideWhenUsed/></w:style>',
        '<w:style w:type="character" w:customStyle="1" w:styleId="Heading1Char"><w:name w:val="Heading 1 Char"/><w:basedOn w:val="DefaultParagraphFont"/><w:link w:val="Heading1"/><w:uiPriority w:val="9"/><w:rPr><w:rFonts w:asciiTheme="majorHAnsi" w:eastAsiaTheme="majorEastAsia" w:hAnsiTheme="majorHAnsi" w:cstheme="majorBidi"/><w:color w:val="0F4761" w:themeColor="accent1" w:themeShade="BF"/><w:sz w:val="40"/><w:szCs w:val="40"/></w:rPr></w:style>',
        '<w:style w:type="character" w:customStyle="1" w:styleId="Heading2Char"><w:name w:val="Heading 2 Char"/><w:basedOn w:val="DefaultParagraphFont"/><w:link w:val="Heading2"/><w:uiPriority w:val="9"/><w:rPr><w:sz w:val="32"/><w:szCs w:val="32"/></w:rPr></w:style>',
        '<w:style w:type="character" w:styleId="Hyperlink"><w:name w:val="Hyperlink"/><w:basedOn w:val="DefaultParagraphFont"/><w:uiPriority w:val="99"/><w:unhideWhenUsed/><w:rPr><w:color w:val="467886" w:themeColor="hyperlink"/><w:u w:val="single"/></w:rPr></w:style>',
        '<w:style w:type="character" w:styleId="FootnoteReference"><w:name w:val="footnote reference"/><w:basedOn w:val="DefaultParagraphFont"/><w:rPr><w:vertAlign w:val="superscript"/></w:rPr></w:style>',
        '<w:style w:type="character" w:styleId="CommentReference"><w:name w:val="annotation reference"/><w:basedOn w:val="DefaultParagraphFont"/><w:rPr><w:sz w:val="16"/><w:szCs w:val="16"/></w:rPr></w:style>',
        '<w:style w:type="table" w:default="1" w:styleId="TableNormal"><w:name w:val="Normal Table"/><w:uiPriority w:val="99"/><w:semiHidden/><w:unhideWhenUsed/><w:tblPr><w:tblInd w:w="0" w:type="dxa"/><w:tblCellMar><w:top w:w="0" w:type="dxa"/><w:left w:w="108" w:type="dxa"/><w:bottom w:w="0" w:type="dxa"/><w:right w:w="108" w:type="dxa"/></w:tblCellMar></w:tblPr></w:style>',
        '<w:style w:type="table" w:styleId="TableGrid"><w:name w:val="Table Grid"/><w:basedOn w:val="TableNormal"/><w:uiPriority w:val="39"/><w:pPr><w:spacing w:after="0" w:line="240" w:lineRule="auto"/></w:pPr><w:tblPr><w:tblBorders><w:top w:val="single" w:sz="4" w:space="0" w:color="auto"/><w:left w:val="single" w:sz="4" w:space="0" w:color="auto"/><w:bottom w:val="single" w:sz="4" w:space="0" w:color="auto"/><w:right w:val="single" w:sz="4" w:space="0" w:color="auto"/><w:insideH w:val="single" w:sz="4" w:space="0" w:color="auto"/><w:insideV w:val="single" w:sz="4" w:space="0" w:color="auto"/></w:tblBorders></w:tblPr></w:style>',
        '<w:style w:type="numbering" w:default="1" w:styleId="NoList"><w:name w:val="No List"/><w:uiPriority w:val="99"/><w:semiHidden/><w:unhideWhenUsed/></w:style>',
    ]
    return (DECL + "<w:styles %s>" % W_NS +
            '<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:asciiTheme="minorHAnsi" w:eastAsiaTheme="minorHAnsi" '
            'w:hAnsiTheme="minorHAnsi" w:cstheme="minorBidi"/><w:kern w:val="2"/><w:sz w:val="24"/><w:szCs w:val="24"/>'
            '<w:lang w:val="en-US" w:eastAsia="en-US" w:bidi="ar-SA"/><w14:ligatures w14:val="standardContextual"/></w:rPr>'
            '</w:rPrDefault><w:pPrDefault><w:pPr><w:spacing w:after="160" w:line="278" w:lineRule="auto"/></w:pPr>'
            '</w:pPrDefault></w:docDefaults><w:latentStyles w:defLockedState="0" w:defUIPriority="99" w:defSemiHidden="0" '
            'w:defUnhideWhenUsed="0" w:defQFormat="0" w:count="376">' + latent + "</w:latentStyles>" + "".join(styles) +
            "</w:styles>")


def numbering_xml():
    def lvl(i, fmt, text, left):
        return ('<w:lvl w:ilvl="%d" w:tplc="04090001"><w:start w:val="1"/><w:numFmt w:val="%s"/><w:lvlText w:val="%s"/>'
                '<w:lvlJc w:val="left"/><w:pPr><w:ind w:left="%d" w:hanging="360"/></w:pPr>%s</w:lvl>'
                % (i, fmt, text, left, '<w:rPr><w:rFonts w:ascii="Symbol" w:hAnsi="Symbol" w:hint="default"/></w:rPr>'
                   if fmt == "bullet" else ""))
    bullet = "".join(lvl(i, "bullet", "", 720 * (i + 1)) for i in range(9))
    number = "".join(lvl(i, ["decimal", "lowerLetter", "lowerRoman"][i % 3], "%%%d." % (i + 1), 720 * (i + 1)) for i in range(9))
    return (DECL + "<w:numbering %s>" % W_NS +
            '<w:abstractNum w:abstractNumId="0" w15:restartNumberingAfterBreak="0"><w:nsid w:val="1A2B3C4D"/>'
            '<w:multiLevelType w:val="hybridMultilevel"/><w:tmpl w:val="04090001"/>' + bullet + "</w:abstractNum>"
            '<w:abstractNum w:abstractNumId="1" w15:restartNumberingAfterBreak="0"><w:nsid w:val="2B3C4D5E"/>'
            '<w:multiLevelType w:val="hybridMultilevel"/><w:tmpl w:val="0409000F"/>' + number + "</w:abstractNum>"
            '<w:num w:numId="1" w16cid:durableId="111111111"><w:abstractNumId w:val="0"/></w:num>'
            '<w:num w:numId="2" w16cid:durableId="222222222"><w:abstractNumId w:val="1"/></w:num></w:numbering>')


def theme_xml():
    scripts = ["Jpan", "Hang", "Hans", "Hant", "Arab", "Hebr", "Thai", "Ethi", "Beng", "Gujr", "Khmr", "Knda", "Guru",
               "Cans", "Cher", "Yiii", "Tibt", "Thaa", "Deva", "Telu", "Taml", "Syrc", "Orya", "Mlym", "Laoo", "Sinh",
               "Mong", "Viet", "Uigh", "Geor", "Armn", "Bugi", "Bopo", "Java", "Lisu", "Mymr", "Nkoo", "Olck", "Osma",
               "Phag", "Sora", "Tale", "Talu", "Tfng"]
    fonts = "".join('<a:font script="%s" typeface="Font %s"/>' % (s, s) for s in scripts)
    colors = [("dk1", '<a:sysClr val="windowText" lastClr="000000"/>'), ("lt1", '<a:sysClr val="window" lastClr="FFFFFF"/>'),
              ("dk2", '<a:srgbClr val="0E2841"/>'), ("lt2", '<a:srgbClr val="E8E8E8"/>'), ("accent1", '<a:srgbClr val="156082"/>'),
              ("accent2", '<a:srgbClr val="E97132"/>'), ("accent3", '<a:srgbClr val="196B24"/>'), ("accent4", '<a:srgbClr val="0F9ED5"/>'),
              ("accent5", '<a:srgbClr val="A02B93"/>'), ("accent6", '<a:srgbClr val="4EA72E"/>'), ("hlink", '<a:srgbClr val="467886"/>'),
              ("folHlink", '<a:srgbClr val="96607D"/>')]
    fill = ('<a:gradFill rotWithShape="1"><a:gsLst><a:gs pos="0"><a:schemeClr val="phClr"><a:lumMod val="110000"/>'
            '<a:satMod val="105000"/><a:tint val="67000"/></a:schemeClr></a:gs><a:gs pos="100000"><a:schemeClr val="phClr">'
            '<a:lumMod val="105000"/><a:satMod val="109000"/><a:tint val="81000"/></a:schemeClr></a:gs></a:gsLst>'
            '<a:lin ang="5400000" scaled="0"/></a:gradFill>')
    return (DECL + '<a:theme xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" name="Office Theme">'
            '<a:themeElements><a:clrScheme name="Office">' + "".join("<a:%s>%s</a:%s>" % (n, v, n) for n, v in colors) +
            '</a:clrScheme><a:fontScheme name="Office"><a:majorFont><a:latin typeface="Aptos Display" panose="02110004020202020204"/>'
            '<a:ea typeface=""/><a:cs typeface=""/>' + fonts + '</a:majorFont><a:minorFont><a:latin typeface="Aptos" '
            'panose="02110004020202020204"/><a:ea typeface=""/><a:cs typeface=""/>' + fonts + '</a:minorFont></a:fontScheme>'
            '<a:fmtScheme name="Office"><a:fillStyleLst><a:solidFill><a:schemeClr val="phClr"/></a:solidFill>' + fill * 2 +
            '</a:fillStyleLst><a:lnStyleLst>' + '<a:ln w="12700" cap="flat" cmpd="sng" algn="ctr"><a:solidFill><a:schemeClr '
            'val="phClr"/></a:solidFill><a:prstDash val="solid"/><a:miter lim="800000"/></a:ln>' * 3 + '</a:lnStyleLst>'
            '<a:effectStyleLst><a:effectStyle><a:effectLst/></a:effectStyle><a:effectStyle><a:effectLst/></a:effectStyle>'
            '<a:effectStyle><a:effectLst><a:outerShdw blurRad="57150" dist="19050" dir="5400000" algn="ctr" rotWithShape="0">'
            '<a:srgbClr val="000000"><a:alpha val="63000"/></a:srgbClr></a:outerShdw></a:effectLst></a:effectStyle>'
            '</a:effectStyleLst><a:bgFillStyleLst><a:solidFill><a:schemeClr val="phClr"/></a:solidFill>' + fill * 2 +
            '</a:bgFillStyleLst></a:fmtScheme></a:themeElements><a:objectDefaults/><a:extraClrSchemeLst/></a:theme>')


def png():
    width, height = 64, 36
    raw = b"".join(b"\x00" + b"".join(bytes([x * 4, y * 7, 160]) for x in range(width)) for y in range(height))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def make(path, pages):
    d = Doc(pages, seed=pages)
    document = d.body()
    comments, comments_ex = d.comments_xml()
    rel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
    wml = "application/vnd.openxmlformats-officedocument.wordprocessingml."
    parts = [
        ("word/styles.xml", wml + "styles+xml", "rIdStyles", rel + "styles", styles_xml()),
        ("word/settings.xml", wml + "settings+xml", "rIdSettings", rel + "settings", d.settings()),
        ("word/numbering.xml", wml + "numbering+xml", "rIdNumbering", rel + "numbering", numbering_xml()),
        ("word/footnotes.xml", wml + "footnotes+xml", "rIdFootnotes", rel + "footnotes", d.notes()),
        ("word/comments.xml", wml + "comments+xml", "rIdComments", rel + "comments", comments),
        ("word/commentsExtended.xml", wml + "commentsExtended+xml", "rIdCommentsEx",
         "http://schemas.microsoft.com/office/2011/relationships/commentsExtended", comments_ex),
        ("word/footer1.xml", wml + "footer+xml", "rIdFooter1", rel + "footer", d.footer()),
        ("word/theme/theme1.xml", "application/vnd.openxmlformats-officedocument.theme+xml", "rIdTheme", rel + "theme", theme_xml()),
    ]
    types = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\r\n<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
             '<Default Extension="png" ContentType="image/png"/><Default Extension="rels" '
             'ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/>'
             '<Override PartName="/word/document.xml" ContentType="%sdocument.main+xml"/>' % wml +
             "".join('<Override PartName="/%s" ContentType="%s"/>' % (p[0], p[1]) for p in parts) + "</Types>")
    rels = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\r\n<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">' +
            "".join('<Relationship Id="%s" Type="%s" Target="%s"/>' % (p[2], p[3], p[0][5:]) for p in parts) +
            '<Relationship Id="rIdImage1" Type="%simage" Target="media/image1.png"/>' % rel +
            '<Relationship Id="rIdLink" Type="%shyperlink" Target="https://example.com/report" TargetMode="External"/>' % rel +
            "</Relationships>")
    root_rels = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\r\n<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                 '<Relationship Id="rId1" Type="%sofficeDocument" Target="word/document.xml"/></Relationships>' % rel)
    entries = [("[Content_Types].xml", types), ("_rels/.rels", root_rels), ("word/document.xml", document),
               ("word/_rels/document.xml.rels", rels)] + [(p[0], p[4]) for p in parts] + [("word/media/image1.png", png())]
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in entries:
            info = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data.encode("utf-8") if isinstance(data, str) else data)
    print("  %s: %d pages, %s bytes, document.xml %s bytes" % (path, pages, format(os.path.getsize(path), ","),
                                                                 format(len(document.encode("utf-8")), ",")))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for pages in [int(p) for p in sys.argv[2:]] or [20, 300]:
        make(os.path.join(out, "long-%d.docx" % pages), pages)


if __name__ == "__main__":
    main()
