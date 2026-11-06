#include "TestHarness.h"

#include <string>
#include <utility>
#include <vector>

#include "PkgTestUtil.h"
#include "doc/docx/Docx.h"
#include "doc/model/Schema.h"
#include "doc/model/Utf.h"
#include "pkg/Canonical.h"
#include "pkg/Check.h"

using namespace fw::doc;
using fw::pkg::Status;

namespace {

constexpr const char* kNs =
    "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
    "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
    "xmlns:w14=\"http://schemas.microsoft.com/office/word/2010/wordml\" "
    "xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\" "
    "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
    "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
    "xmlns:wps=\"http://schemas.microsoft.com/office/word/2010/wordprocessingShape\" "
    "xmlns:v=\"urn:schemas-microsoft-com:vml\" xmlns:o=\"urn:schemas-microsoft-com:office:office\" "
    "xmlns:w10=\"urn:schemas-microsoft-com:office:word\" "
    "xmlns:m=\"http://schemas.openxmlformats.org/officeDocument/2006/math\" "
    "xmlns:x=\"urn:unknown\" mc:Ignorable=\"w14 x\"";

// A Word package built from parts given as XML.
struct Fixture {
  std::string body;         // inside w:body
  std::string documentXml;  // the whole main part, instead of body
  std::string styles, numbering, settings, theme, comments, commentsEx, footnotes, header, footer;

  std::vector<uint8_t> Bytes() const {
    struct Part {
      std::string name, type, rel, xml;
    };
    std::vector<Part> parts;
    const std::string wml = "application/vnd.openxmlformats-officedocument.wordprocessingml.";
    const std::string rel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
    auto add = [&](const std::string& name, const std::string& type, const std::string& relType, const std::string& root,
                   const std::string& inner) {
      if (inner.empty()) return;
      std::string xml = inner.starts_with("<?xml") || inner.starts_with("<" + root)
                            ? inner
                            : "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<" + root + " " + kNs + ">" +
                                  inner + "</" + root + ">";
      parts.push_back({name, type, relType, xml});
    };
    add("word/styles.xml", wml + "styles+xml", rel + "styles", "w:styles", styles);
    add("word/numbering.xml", wml + "numbering+xml", rel + "numbering", "w:numbering", numbering);
    add("word/settings.xml", wml + "settings+xml", rel + "settings", "w:settings", settings);
    add("word/comments.xml", wml + "comments+xml", rel + "comments", "w:comments", comments);
    add("word/footnotes.xml", wml + "footnotes+xml", rel + "footnotes", "w:footnotes", footnotes);
    add("word/header1.xml", wml + "header+xml", rel + "header", "w:hdr", header);
    add("word/footer1.xml", wml + "footer+xml", rel + "footer", "w:ftr", footer);
    if (!commentsEx.empty())
      parts.push_back({"word/commentsExtended.xml", wml + "commentsExtended+xml",
                       "http://schemas.microsoft.com/office/2011/relationships/commentsExtended",
                       "<w15:commentsEx xmlns:w15=\"http://schemas.microsoft.com/office/word/2012/wordml\">" + commentsEx +
                           "</w15:commentsEx>"});
    if (!theme.empty())
      parts.push_back({"word/theme/theme1.xml", "application/vnd.openxmlformats-officedocument.theme+xml", rel + "theme",
                       "<a:theme xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" name=\"T\">"
                       "<a:themeElements>" + theme + "</a:themeElements></a:theme>"});
    std::string types =
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/word/document.xml\" ContentType=\"" + wml + "document.main+xml\"/>";
    std::string rels = "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
    int id = 1;
    for (const auto& p : parts) {
      types += "<Override PartName=\"/" + p.name + "\" ContentType=\"" + p.type + "\"/>";
      std::string relId = p.name == "word/header1.xml" ? "rIdHeader" : p.name == "word/footer1.xml" ? "rIdFooter"
                                                                                               : "rId" + std::to_string(id++);
      rels += "<Relationship Id=\"" + relId + "\" Type=\"" + p.rel + "\" Target=\"" +
              p.name.substr(5) + "\"/>";
    }
    rels += "<Relationship Id=\"rIdLink\" Type=\"" + rel +
            "hyperlink\" Target=\"https://example.com/\" TargetMode=\"External\"/></Relationships>";
    types += "</Types>";
    std::string document = documentXml.empty()
                               ? "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<w:document " +
                                     std::string(kNs) + "><w:body>" + body + "</w:body></w:document>"
                               : documentXml;
    std::vector<std::pair<std::string, std::string>> entries = {
        {"[Content_Types].xml", types},
        {"_rels/.rels", "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                        "<Relationship Id=\"rId1\" Type=\"" + rel + "officeDocument\" Target=\"word/document.xml\"/>"
                        "</Relationships>"},
        {"word/document.xml", document},
        {"word/_rels/document.xml.rels", rels},
    };
    for (const auto& p : parts) entries.emplace_back(p.name, p.xml);
    return test::MakeZip(entries);
  }

  std::unique_ptr<Document> Load(Status& st) const {
    auto pkg = test::OpenBytes(Bytes(), st);
    if (!pkg) return nullptr;
    return docx::Read(std::move(pkg), st);
  }
};

std::unique_ptr<Document> LoadOk(const Fixture& f) {
  Status st;
  auto doc = f.Load(st);
  if (!doc) std::printf("  load failed: %s\n", st.message.c_str());
  return doc;
}

std::string Canon(std::string_view xml) {
  std::string out;
  Status st = fw::pkg::CanonicalizeXml(xml, out);
  return st.Ok() ? out : "ERROR: " + st.message;
}

// Every part the model owns, written from the model, equals the original after canonicalization.
bool RoundTrips(const Document& doc) {
  bool ok = true;
  for (uint32_t i = 0; i < doc.Parts().size(); ++i) {
    std::vector<uint8_t> bytes;
    if (!doc.Package().ReadPart(doc.Parts()[i].name, bytes).Ok()) return false;
    std::string out;
    if (!docx::WritePart(doc, i, out).Ok()) return false;
    std::string a = Canon(test::Text(bytes)), b = Canon(out);
    if (a != b) {
      size_t at = 0;
      while (at < a.size() && at < b.size() && a[at] == b[at]) ++at;
      std::printf("  %s differs at %zu:\n    original: %s\n    written:  %s\n", doc.Parts()[i].name.c_str(), at,
                  a.substr(at > 60 ? at - 60 : 0, 200).c_str(), b.substr(at > 60 ? at - 60 : 0, 200).c_str());
      ok = false;
    }
  }
  return ok;
}

std::u16string U(std::string_view s) { return ToUtf16(s); }

uint32_t FirstPara(const Document& doc) { return doc.StoryParagraphs(doc.MainStory()).at(0); }

// Paragraphs of the main story in order.
std::vector<uint32_t> Paras(const Document& doc) { return doc.StoryParagraphs(doc.MainStory()); }

}  // namespace

TEST(Doc_SchemaIsSound) {
  const Schema& s = Schema::Get();
  for (const auto& e : s.Errors()) std::printf("  schema: %s\n", e.c_str());
  CHECK(s.Errors().empty());
  CHECK(s.TypeCount() > 100);
  // The same local name has its own type where it appears.
  TypeId run = s.Elem(s.Child(s.FindType("InlineContent"), Name("r"))).type;
  TypeId inRun = s.Elem(s.Child(run, Name("rPr"))).type;
  TypeId inPara = s.Elem(s.Child(s.FindType("PPr"), Name("rPr"))).type;
  CHECK(inRun != inPara);
  CHECK(s.Child(inPara, Name("ins")) != kNoDef && s.Child(inRun, Name("ins")) == kNoDef);
  CHECK(Name("r:id") == NameOf(Ns::R, "id") && Name("id") == NameOf(Ns::W, "id"));
}

TEST(Doc_ValuesParse) {
  CHECK(ParseOnOff("on") == true && ParseOnOff("0") == false && !ParseOnOff("maybe"));
  CHECK(ParseInt("240") == 240 && ParseInt("-12") == -12 && ParseInt("+5") == 5 && ParseInt("10.6") == 11);
  CHECK(!ParseInt("12px") && !ParseInt(""));
  CHECK(ParseMeasure("12pt", 20) == 240 && ParseMeasure("1in", 20) == 1440 && ParseMeasure("2.54cm", 20) == 1440);
  CHECK(ParseMeasure("720", 20) == 720 && ParseHex("3E48D6") == 0x3E48D6u);
  CHECK(ToUtf8(ToUtf16("café 😀")) == "café 😀");
}

TEST(Doc_ParagraphTextAndItems) {
  Fixture f;
  f.body =
      "<w:p><w:r><w:t xml:space=\"preserve\">A </w:t><w:tab/><w:t>B</w:t><w:br/><w:t>C</w:t><w:br w:type=\"page\"/>"
      "<w:br w:type=\"column\"/><w:cr/><w:noBreakHyphen/><w:softHyphen/><w:sym w:font=\"Symbol\" w:char=\"F0B7\"/>"
      "<w:ptab w:relativeTo=\"margin\" w:alignment=\"right\" w:leader=\"none\"/><w:lastRenderedPageBreak/>"
      "<w:footnoteReference w:id=\"1\"/><w:t>D</w:t></w:r></w:p>";
  f.footnotes = "<w:footnote w:type=\"separator\" w:id=\"-1\"><w:p><w:r><w:separator/></w:r></w:p></w:footnote>"
                "<w:footnote w:id=\"1\"><w:p><w:r><w:footnoteRef/><w:t>Note</w:t></w:r></w:p></w:footnote>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  uint32_t p = FirstPara(*doc);
  CHECK(doc->Para(p).text == U("A \tB\vC\f\x0E\v‑­\t\x02") + u"D");
  auto items = doc->Items(p);
  std::vector<ItemKind> kinds;
  for (const auto& i : items) kinds.push_back(i.kind);
  std::vector<ItemKind> want = {ItemKind::Tab, ItemKind::LineBreak, ItemKind::PageBreak, ItemKind::ColumnBreak,
                                ItemKind::LineBreak, ItemKind::NoBreakHyphen, ItemKind::SoftHyphen, ItemKind::Symbol,
                                ItemKind::PositionalTab, ItemKind::RenderedPageBreak, ItemKind::FootnoteRef};
  CHECK(kinds == want);
  CHECK(items[9].length == 0 && items[9].offset == 13);
  CHECK(doc->Runs(p).size() == 1);
  uint32_t note = doc->NoteStory(StoryKind::Footnote, "1");
  CHECK(note != kNone && doc->Para(doc->StoryParagraphs(note)[0]).text == u"\x02Note");
  CHECK(doc->NoteStory(StoryKind::Footnote, "-1") != kNone);
  CHECK(RoundTrips(*doc));
}

TEST(Doc_FieldsAndTheirResults) {
  Fixture f;
  // A complex field across runs and paragraphs, a nested field, and a simple field.
  f.body =
      "<w:p><w:r><w:t xml:space=\"preserve\">Page </w:t></w:r>"
      "<w:r><w:fldChar w:fldCharType=\"begin\"/></w:r><w:r><w:instrText xml:space=\"preserve\"> PAGE </w:instrText></w:r>"
      "<w:r><w:fldChar w:fldCharType=\"separate\"/></w:r><w:r><w:t>3</w:t></w:r><w:r><w:fldChar w:fldCharType=\"end\"/></w:r>"
      "<w:r><w:t xml:space=\"preserve\"> of </w:t></w:r><w:fldSimple w:instr=\" NUMPAGES \"><w:r><w:t>9</w:t></w:r></w:fldSimple></w:p>"
      "<w:p><w:r><w:fldChar w:fldCharType=\"begin\"/><w:instrText>IF </w:instrText></w:r>"
      "<w:r><w:fldChar w:fldCharType=\"begin\"/><w:instrText>DATE</w:instrText><w:fldChar w:fldCharType=\"separate\"/>"
      "<w:t>today</w:t><w:fldChar w:fldCharType=\"end\"/></w:r>"
      "<w:r><w:instrText xml:space=\"preserve\"> = 1 </w:instrText><w:fldChar w:fldCharType=\"separate\"/><w:t>yes</w:t></w:r></w:p>"
      "<w:p><w:r><w:t>still</w:t><w:fldChar w:fldCharType=\"end\" w:dirty=\"true\"/></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  auto fields = doc->Fields(doc->MainStory());
  CHECK(fields.size() == 4);
  if (fields.size() != 4) return;
  CHECK(fields[0].Type() == u"PAGE" && fields[0].result == u"3" && fields[0].complete && !fields[0].simple);
  CHECK(fields[1].simple && fields[1].Type() == u"NUMPAGES" && fields[1].result == u"9");
  CHECK(fields[2].code == u"IF  = 1 " && fields[2].result == u"yes\rstill" && fields[2].depth == 0);
  CHECK(fields[3].depth == 1 && fields[3].parent == 2 && fields[3].result == u"today");
  CHECK(fields[2].end.para == Paras(*doc)[2] && fields[2].begin.para == Paras(*doc)[1]);
  // Runs say which text is a field code (hidden in the view).
  auto runs = doc->Runs(Paras(*doc)[0]);
  std::u16string shown;
  for (const auto& r : runs)
    if (!r.fieldCode) shown += std::u16string_view(doc->Para(Paras(*doc)[0]).text).substr(r.start, r.end - r.start);
  CHECK(shown == u"Page 3 of 9");
  auto second = doc->Runs(Paras(*doc)[2]);  // starts inside the IF field's result
  CHECK(!second.empty() && !second[0].fieldCode && second[0].fieldDepth == 1);
  // Asked for first, a paragraph inside a field still knows it: the pass goes as far as it needs.
  auto fresh = LoadOk(f);
  CHECK(fresh && fresh->Runs(Paras(*fresh)[2]).at(0).fieldDepth == 1);
  CHECK(RoundTrips(*doc));
}

TEST(Doc_RoundTripKeepsEverything) {
  // Every structure the model reads, with unknown markup mixed in at every level.
  Fixture f;
  f.documentXml = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<!-- before -->\r\n") +
      "<w:document " + kNs + " x:docAttr=\"1\"><x:beforeBody/><w:body>"
      "<w:bookmarkStart w:id=\"0\" w:name=\"top\"/>"
      "<w:p w14:paraId=\"1A2B3C4D\" w14:textId=\"77777777\" w:rsidR=\"00AB12CD\" x:mark=\"p\">"
      "<w:pPr><w:pStyle w:val=\"Heading1\"/><x:pprExt/><w:spacing w:before=\"120\" w:after=\"0\" x:spc=\"1\"/>"
      "<w:rPr><w:ins w:id=\"9\" w:author=\"A\" w:date=\"2026-01-01T00:00:00Z\"/><w:b/></w:rPr></w:pPr>"
      "<w:r w:rsidR=\"00AB12CD\"><w:rPr><w:b w:val=\"true\"/><w:i w:val=\"on\"/><w14:ligatures w14:val=\"standard\"/>"
      "<w:rPrChange w:id=\"5\" w:author=\"B\"><w:rPr><w:i/></w:rPr></w:rPrChange></w:rPr>"
      "<w:t xml:space=\"preserve\"> lead</w:t><x:inRun/><w:t xml:space=\"default\">x</w:t><w:t>a<![CDATA[<b>]]>c</w:t></w:r>"
      "<w:hyperlink r:id=\"rIdLink\" w:history=\"1\"><w:r><w:t>link</w:t></w:r></w:hyperlink>"
      "<w:hyperlink w:anchor=\"top\"><w:r><w:t>anchor</w:t></w:r></w:hyperlink>"
      "<w:commentRangeStart w:id=\"3\"/><w:ins w:id=\"1\" w:author=\"A\" w:date=\"2026-01-01T00:00:00Z\">"
      "<w:r><w:t>new</w:t></w:r></w:ins><w:del w:id=\"2\" w:author=\"A\"><w:r><w:delText>old</w:delText></w:r></w:del>"
      "<w:moveFromRangeStart w:id=\"7\" w:name=\"m1\" w:author=\"A\"/><w:moveFrom w:id=\"8\" w:author=\"A\"><w:r>"
      "<w:delText>moved</w:delText></w:r></w:moveFrom><w:moveFromRangeEnd w:id=\"7\"/>"
      "<w:commentRangeEnd w:id=\"3\"/><w:r><w:commentReference w:id=\"3\"/></w:r>"
      "<w:sdt><w:sdtPr><w:alias w:val=\"Name\"/><w:id w:val=\"42\"/><w14:checkbox/><w:showingPlcHdr/></w:sdtPr>"
      "<w:sdtEndPr><w:rPr><w:b/></w:rPr></w:sdtEndPr><w:sdtContent><w:r><w:t>control</w:t></w:r></w:sdtContent></w:sdt>"
      "<w:smartTag w:uri=\"urn:s\" w:element=\"place\"><w:smartTagPr><w:attr w:name=\"n\" w:val=\"v\"/></w:smartTagPr>"
      "<w:r><w:t>Paris</w:t></w:r></w:smartTag>"
      "<w:customXml w:element=\"c\"><w:r><w:t>cx</w:t></w:r></w:customXml>"
      "<w:dir w:val=\"rtl\"><w:r><w:t>rtl</w:t></w:r></w:dir>"
      "<w:permStart w:id=\"11\" w:edGrp=\"everyone\"/><w:r><w:t>free</w:t></w:r><w:permEnd w:id=\"11\"/>"
      "<w:proofErr w:type=\"spellStart\"/><w:r><w:t>teh</w:t></w:r><w:proofErr w:type=\"spellEnd\"/>"
      "<m:oMath><m:r><m:t>x=1</m:t></m:r></m:oMath>"
      "<w:r><w:object w:dxaOrig=\"1440\" w:dyaOrig=\"720\"><v:shape id=\"o1\" style=\"width:72pt;height:36pt\"/>"
      "<o:OLEObject ProgID=\"Equation.3\" ShapeID=\"o1\" r:id=\"rIdLink\"/></w:object></w:r>"
      "<w:r><mc:AlternateContent><mc:Choice Requires=\"x\"><w:t>never</w:t></mc:Choice>"
      "<mc:Fallback><w:t>fallback</w:t></mc:Fallback></mc:AlternateContent></w:r>"
      "<!-- in a paragraph --><?pi data?></w:p>"
      "<x:between>kept <x:deep/></x:between>"
      "<w:tbl><w:tblPr><w:tblStyle w:val=\"TableGrid\"/><w:tblW w:w=\"0\" w:type=\"auto\"/>"
      "<w:tblLook w:val=\"04A0\" w:firstRow=\"1\" w:lastRow=\"0\" w:firstColumn=\"1\" w:lastColumn=\"0\" w:noHBand=\"0\" "
      "w:noVBand=\"1\"/></w:tblPr><w:tblGrid><w:gridCol w:w=\"1000\"/><w:gridCol w:w=\"2000\"/><w:gridCol w:w=\"3000\"/>"
      "</w:tblGrid>"
      "<w:tr w14:paraId=\"0A0B0C0D\"><w:tblPrEx><w:tblBorders><w:top w:val=\"single\" w:sz=\"4\"/></w:tblBorders>"
      "</w:tblPrEx><w:trPr><w:gridBefore w:val=\"1\"/><w:trHeight w:val=\"300\" w:hRule=\"exact\"/></w:trPr>"
      "<w:tc><w:tcPr><w:tcW w:w=\"5000\" w:type=\"dxa\"/><w:gridSpan w:val=\"2\"/><w:vMerge w:val=\"restart\"/></w:tcPr>"
      "<w:p><w:r><w:t>merged</w:t></w:r></w:p></w:tc></w:tr>"
      "<w:sdt><w:sdtPr><w:id w:val=\"43\"/></w:sdtPr><w:sdtContent><w:tr><w:trPr><w:gridBefore w:val=\"1\"/></w:trPr>"
      "<w:tc><w:tcPr><w:gridSpan w:val=\"2\"/><w:vMerge/></w:tcPr><w:p/></w:tc></w:tr></w:sdtContent></w:sdt>"
      "<w:tr><w:tc><w:p><w:r><w:t>a</w:t></w:r></w:p></w:tc>"
      "<w:sdt><w:sdtContent><w:tc><w:tbl><w:tr><w:tc><w:p><w:r><w:t>nested</w:t></w:r></w:p></w:tc></w:tr></w:tbl><w:p/>"
      "</w:tc></w:sdtContent></w:sdt><w:tc><w:p/></w:tc></w:tr></w:tbl>"
      "<w:sdt><w:sdtPr><w:docPartObj><w:docPartGallery w:val=\"Cover Pages\"/><w:docPartUnique/></w:docPartObj>"
      "</w:sdtPr><w:sdtContent><w:p><w:r><w:t>in a block control</w:t></w:r></w:p></w:sdtContent></w:sdt>"
      "<w:customXml w:uri=\"urn:c\" w:element=\"block\"><w:p><w:r><w:t>custom</w:t></w:r></w:p></w:customXml>"
      "<mc:AlternateContent><mc:Choice Requires=\"x\"><w:p><w:r><w:t>never</w:t></w:r></w:p></mc:Choice>"
      "<mc:Fallback><w:p><w:r><w:t>block fallback</w:t></w:r></w:p></mc:Fallback></mc:AlternateContent>"
      "<w:p><w:pPr><w:tabs>\n  </w:tabs><w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/><w:titlePg/></w:sectPr></w:pPr></w:p>"
      "<w:bookmarkEnd w:id=\"0\"/>"
      "<w:sectPr w:rsidR=\"00FC693F\"><w:pgSz w:w=\"15840\" w:h=\"12240\" w:orient=\"landscape\"/>"
      "<w:pgMar w:top=\"1440\" w:right=\"1800\" w:bottom=\"1440\" w:left=\"1800\" w:header=\"720\" w:footer=\"720\" "
      "w:gutter=\"0\"/><w:cols w:space=\"720\"/><x:sectExt/></w:sectPr>"
      "</w:body><x:afterBody/></w:document>\r\n<!-- after -->";
  f.styles = "<w:docDefaults><w:rPrDefault><w:rPr><w:sz w:val=\"22\"/></w:rPr></w:rPrDefault></w:docDefaults>"
             "<w:latentStyles w:defLockedState=\"0\" w:count=\"1\"><w:lsdException w:name=\"Normal\"/></w:latentStyles>"
             "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/>"
             "<w:qFormat/><x:styleExt/></w:style>"
             "<w:style w:type=\"paragraph\" w:styleId=\"Heading1\"><w:name w:val=\"heading 1\"/><w:basedOn w:val=\"Normal\"/>"
             "<w:pPr><w:keepNext/><w:outlineLvl w:val=\"0\"/></w:pPr><w:rPr><w:b/><w:sz w:val=\"32\"/></w:rPr></w:style>";
  f.settings = "<w:zoom w:percent=\"100\"/><w:defaultTabStop w:val=\"709\"/><w:autoHyphenation/>"
               "<w:compat><w:compatSetting w:name=\"compatibilityMode\" w:uri=\"http://schemas.microsoft.com/office/word\" "
               "w:val=\"15\"/></w:compat><w14:docId w14:val=\"1\"/>";
  f.comments = "<w:comment w:id=\"3\" w:author=\"Ann\" w:initials=\"A\"><w:p w14:paraId=\"00000011\"><w:r>"
               "<w:annotationRef/><w:t>Look</w:t></w:r></w:p></w:comment>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  CHECK(RoundTrips(*doc));
  auto paras = Paras(*doc);
  const Paragraph& first = doc->Para(paras[0]);
  CHECK(doc->W14ParaId(paras[0]) == "1A2B3C4D");
  CHECK(doc->ParaNode(paras[0]).AttrOr(Name("rsidR")) == "00AB12CD");  // rsids are the model's, not the store's
  // The display-only fallback's text is shown; the kept block is what's written.
  CHECK(first.text.find(u"fallback") != std::u16string::npos && first.text.find(u"never") == std::u16string::npos);
  CHECK(first.text.find(u"a<b>c") != std::u16string::npos);
  bool sawDisplayOnly = false;
  for (const auto& r : doc->Runs(paras[0])) sawDisplayOnly |= r.displayOnly;
  CHECK(sawDisplayOnly);
  // Blocks: paragraph, table, block-level content control, custom XML, the display-only paragraph, ...
  auto blocks = doc->Blocks(doc->MainStory());
  CHECK(blocks.size() == 6);
  CHECK(blocks[1].kind == BlockKind::Table && blocks[2].kind == BlockKind::ContentControl &&
        blocks[3].kind == BlockKind::CustomXml && blocks[4].node.DisplayOnly());
  // Tables: rows inside row-level controls, cells inside cell-level ones, spans and merges.
  Node table = blocks[1].node;
  auto rows = doc->TableRows(table);
  CHECK(rows.size() == 3 && doc->TableGrid(table).size() == 3);
  Node merged = doc->RowCells(rows[0])[0];
  CHECK(doc->CellGridColumn(merged) == 1 && doc->CellRowSpan(merged) == 2);
  CHECK(doc->RowCells(rows[2]).size() == 3 && doc->CellGridColumn(doc->RowCells(rows[2])[2]) == 2);
  CHECK(doc->ResolvedCell(merged).gridSpan == 2 && doc->ResolvedRow(rows[0]).heightRule == "exact");
  // Pairs: the bookmark spans blocks; the comment, move and permission ranges close.
  size_t complete = 0;
  for (const auto& pr : doc->Pairs()) complete += pr.open && pr.close;
  CHECK(complete == doc->Pairs().size() && doc->Pairs().size() >= 4);
  auto comments = doc->Comments();
  CHECK(comments.size() == 1 && comments[0].author == "Ann" && comments[0].start.para == paras[0] &&
        comments[0].reference.para == paras[0] && comments[0].start.offset < comments[0].end.offset);
  // Revisions, of each kind present.
  int ins = 0, del = 0, moveFrom = 0, mark = 0, fmt = 0;
  for (const auto& r : doc->Revisions(doc->MainStory())) {
    ins += r.kind == RevisionKind::Insertion;
    del += r.kind == RevisionKind::Deletion;
    moveFrom += r.kind == RevisionKind::MoveFrom;
    mark += r.kind == RevisionKind::MarkInsertion;
    fmt += r.kind == RevisionKind::RunFormat;
  }
  CHECK(ins == 1 && del == 1 && moveFrom == 1 && mark == 1 && fmt == 1);
  // Drawings: the embedded object and the equation, each one character.
  CHECK(doc->Drawings().size() == 2);
  CHECK(doc->Drawings()[0].kind == DrawingKind::Equation && doc->Drawings()[1].kind == DrawingKind::Object);
  CHECK(doc->Drawings()[1].cx.Value() == 914400 && doc->DrawingXml(doc->Drawings()[1]).find("OLEObject") != std::string::npos);
  // Links.
  CHECK(doc->HyperlinkTarget(doc->Runs(paras[0])[1].hyperlink) == "https://example.com/");
  CHECK(doc->HyperlinkTarget(doc->Runs(paras[0])[2].hyperlink) == "#top");
  // Sections: one in a paragraph, one at the end of the body.
  CHECK(doc->Sections().size() == 2);
  CHECK(doc->Sections()[0].props.titlePage && !doc->Sections()[0].props.landscape);
  CHECK(doc->Sections()[1].props.landscape && doc->Sections()[1].props.marginStart.Value() == 1800);
  CHECK(doc->Settings().compatibilityMode == 15 && doc->Settings().defaultTabStop.Value() == 709 &&
        doc->Settings().autoHyphenation);
  // The store holds what the model doesn't know, and the saved file passes the checker.
  CHECK(doc->Store().FragmentCount() > 10);
  CHECK(docx::WriteAllParts(*doc).Ok());
  fw::pkg::MemorySink sink;
  CHECK(doc->Package().Save(sink).Ok());
  Status st;
  auto again = test::OpenBytes(std::move(sink.bytes), st);
  CHECK(again && fw::pkg::CheckPackage(*again).Ok());
}

TEST(Doc_OtherNamespaceForms) {
  // Strict OOXML namespaces, written back as strict.
  Fixture strict;
  strict.documentXml =
      "<w:document xmlns:w=\"http://purl.oclc.org/ooxml/wordprocessingml/main\" "
      "xmlns:r=\"http://purl.oclc.org/ooxml/officeDocument/relationships\" w:conformance=\"strict\"><w:body>"
      "<w:p><w:r><w:rPr><w:b w:val=\"true\"/><w:sz w:val=\"28\"/></w:rPr><w:t>strict</w:t></w:r>"
      "<w:hyperlink r:id=\"rIdLink\"><w:r><w:t>x</w:t></w:r></w:hyperlink></w:p></w:body></w:document>";
  auto doc = LoadOk(strict);
  CHECK(doc);
  if (doc) {
    uint32_t p = FirstPara(*doc);
    CHECK(doc->Para(p).text == u"strictx");
    CHECK(doc->ResolvedRun(p, doc->Runs(p)[0].run).bold && doc->ResolvedRun(p, doc->Runs(p)[0].run).size.Value() == 28);
    CHECK(RoundTrips(*doc));
    std::string out;
    CHECK(docx::WritePart(*doc, 0, out).Ok() && out.find("purl.oclc.org") != std::string::npos);
  }
  // A default namespace for WordprocessingML: the writer declares a prefix of its own.
  Fixture plain;
  plain.documentXml =
      "<document xmlns=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
      "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><body><p><r><t>plain</t></r></p>"
      "<p><pPr><jc w:val=\"center\"/></pPr></p></body></document>";
  doc = LoadOk(plain);
  CHECK(doc);
  if (doc) {
    CHECK(doc->Para(FirstPara(*doc)).text == u"plain");
    CHECK(doc->ResolvedPara(Paras(*doc)[1]).jc == Justification::Center);
    CHECK(RoundTrips(*doc));
  }
  // An inner element that rebinds the writer's prefix: the part is kept, not rewritten.
  Fixture rebound;
  rebound.documentXml = std::string("<w:document ") + kNs + "><w:body><w:p xmlns:r=\"urn:other\"><w:r><w:t>t</w:t></w:r>"
                        "</w:p></w:body></w:document>";
  doc = LoadOk(rebound);
  CHECK(doc && !doc->Parts()[0].writable);
  std::string out;
  CHECK(doc && !docx::WritePart(*doc, 0, out).Ok());
  // A part with an mc:MustUnderstand namespace nobody understands is shown, not edited.
  Fixture must;
  must.documentXml = std::string("<w:document ") + kNs + " mc:MustUnderstand=\"x\"><w:body><w:p/></w:body></w:document>";
  doc = LoadOk(must);
  CHECK(doc && !doc->Editable());
}

TEST(Doc_RefusesWhatIsntAWordDocument) {
  Status st;
  auto pkg = test::OpenBytes(test::MakeZip(test::MiniPackage()), st);
  CHECK(pkg);
  CHECK(!docx::Read(std::move(pkg), st) && st.code == Status::Code::Unsupported && !st.message.empty());
  Fixture noBody;
  noBody.documentXml = std::string("<w:document ") + kNs + "/>";
  CHECK(!noBody.Load(st) && st.code == Status::Code::Damaged);
  Fixture broken;
  broken.body = "<w:p><w:r><w:t>unclosed</w:r></w:p>";
  CHECK(!broken.Load(st) && st.code == Status::Code::Damaged);
  Fixture notWord;
  notWord.documentXml = "<html><body/></html>";
  CHECK(!notWord.Load(st) && st.code == Status::Code::Unsupported);
}

TEST(Doc_TextBoxesAndDrawings) {
  Fixture f;
  f.body =
      "<w:p><w:r><mc:AlternateContent><mc:Choice Requires=\"wps\"><w:drawing><wp:anchor distT=\"0\" distB=\"0\" "
      "distL=\"114300\" distR=\"114300\" simplePos=\"0\" relativeHeight=\"5\" behindDoc=\"1\" locked=\"0\" "
      "layoutInCell=\"1\" allowOverlap=\"1\"><wp:simplePos x=\"0\" y=\"0\"/><wp:positionH relativeFrom=\"column\">"
      "<wp:posOffset>12700</wp:posOffset></wp:positionH><wp:positionV relativeFrom=\"paragraph\"><wp:align>top</wp:align>"
      "</wp:positionV><wp:extent cx=\"1828800\" cy=\"457200\"/><wp:effectExtent l=\"1\" t=\"2\" r=\"3\" b=\"4\"/>"
      "<wp:wrapSquare wrapText=\"bothSides\"/><wp:docPr id=\"7\" name=\"Text Box 7\" descr=\"alt\"/>"
      "<a:graphic><a:graphicData uri=\"http://schemas.microsoft.com/office/word/2010/wordprocessingShape\"><wps:wsp>"
      "<wps:txbx><w:txbxContent><w:p><w:r><w:t>inside</w:t></w:r></w:p></w:txbxContent></wps:txbx></wps:wsp>"
      "</a:graphicData></a:graphic></wp:anchor></w:drawing></mc:Choice><mc:Fallback><w:pict><v:shape id=\"s\" "
      "style=\"position:absolute;width:144pt;height:36pt\"><v:textbox><w:txbxContent><w:p><w:r><w:t>vml copy</w:t>"
      "</w:r></w:p></w:txbxContent></v:textbox></v:shape></w:pict></mc:Fallback></mc:AlternateContent></w:r></w:p>"
      "<w:p><w:r><w:pict><v:shape id=\"t\" style=\"width:72pt;height:1in\"><w10:wrap type=\"topAndBottom\"/></v:shape>"
      "</w:pict></w:r><w:r><w:drawing><wp:inline><wp:extent cx=\"100\" cy=\"200\"/><wp:docPr id=\"8\" name=\"P\"/>"
      "</wp:inline></w:drawing></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  CHECK(doc->Drawings().size() == 3);
  if (doc->Drawings().size() != 3) return;
  const Drawing& box = doc->Drawings()[0];
  CHECK(box.displayOnly && box.anchored && box.behindDoc && box.wrap == WrapType::Square && box.wrapText == "bothSides");
  CHECK(box.cx.Value() == 1828800 && box.hOffset && box.hOffset->Value() == 12700 && box.vAlign == "top");
  CHECK(box.hRelativeFrom == "column" && box.id == 7 && box.descr == "alt" && box.effectB.Value() == 4);
  CHECK(box.relativeHeight == 5 && box.textBoxes.size() == 1);
  CHECK(doc->DrawingXml(box).starts_with("<wp:anchor"));
  const Story& tb = doc->Stories()[box.textBoxes[0]];
  CHECK(tb.kind == StoryKind::TextBox && tb.drawing == 0);
  CHECK(doc->Para(doc->StoryParagraphs(box.textBoxes[0])[0]).text == u"inside");
  const Drawing& vml = doc->Drawings()[1];
  CHECK(vml.kind == DrawingKind::Vml && !vml.anchored && vml.cx.Value() == 914400 && vml.cy.Value() == 914400 &&
        vml.wrap == WrapType::TopAndBottom);
  const Drawing& pic = doc->Drawings()[2];
  CHECK(!pic.anchored && pic.wrap == WrapType::Inline && pic.cy.Value() == 200 && pic.pos.offset == 1);
  auto items = doc->Items(Paras(*doc)[1]);
  CHECK(items.size() == 2 && items[1].kind == ItemKind::Drawing && items[1].drawing == 2);
  CHECK(RoundTrips(*doc));
}

TEST(Doc_SharesRepeatedPropertySets) {
  Fixture f;
  auto run = [](const char* rPr) { return std::string("<w:r><w:rPr>") + rPr + "</w:rPr><w:t>x</w:t></w:r>"; };
  const char* big = "<w:b/><w:sz w:val=\"28\"/>";
  f.body = "<w:p>" + run(big) + run(big) + run("<w:b/><w:sz w:val=\"30\"/>") +
           run("<w:b/><w:sz w:val=\"28\" x:extra=\"1\"/>") + run("<w:b/><x:unknown/><w:sz w:val=\"28\"/>") +
           run("<w:i/><w:rPrChange w:id=\"1\" w:author=\"A\"><w:rPr><w:b/><w:sz w:val=\"28\"/></w:rPr></w:rPrChange>") +
           run(big) + "</w:p><w:p><w:pPr><w:rPr><w:ins w:id=\"5\" w:author=\"A\"/></w:rPr></w:pPr>" + run(big) + "</w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  uint32_t p = Paras(*doc)[0];
  auto runs = doc->Runs(p);
  CHECK(runs.size() == 7);
  if (runs.size() != 7) return;
  auto set = [&](size_t i) { return runs[i].run.Child(Name("rPr")).FirstChild().Id(); };
  CHECK(set(0) == set(1) && set(0) == set(6));                                // one set, used three times
  CHECK(set(2) != set(0) && set(3) != set(0) && set(4) != set(0) && set(5) != set(0));  // a value differs, or the store holds part of it
  for (size_t i : {0, 1, 6}) CHECK(doc->ResolvedRun(p, runs[i].run).bold && doc->ResolvedRun(p, runs[i].run).size.Value() == 28);
  // Walks into properties still find each revision once.
  int format = 0, mark = 0;
  for (const auto& r : doc->Revisions(doc->MainStory())) {
    format += r.kind == RevisionKind::RunFormat;
    mark += r.kind == RevisionKind::MarkInsertion;
  }
  CHECK(format == 1 && mark == 1);
  CHECK(RoundTrips(*doc));  // every user writes the set
}

// ---------- Property resolution: each departure from ECMA-376 (Resolve.cpp, D1-D8) ----------

namespace {

std::string Styles(std::string_view more) {
  return "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/></w:style>"
         "<w:style w:type=\"character\" w:default=\"1\" w:styleId=\"DefaultParagraphFont\"><w:name w:val=\"Default Paragraph Font\"/>"
         "<w:rPr><w:b/><w:color w:val=\"FF0000\"/></w:rPr></w:style>" +
         std::string(more);
}

const RunProps& RunAt(const Document& doc, size_t para, size_t run = 0) {
  uint32_t p = Paras(doc).at(para);
  return doc.ResolvedRun(p, doc.Runs(p).at(run).run);
}

}  // namespace

TEST(Resolve_D4_OriginalFormattingBeforeTrackedChanges) {
  // Word's Original view: a w:pPrChange's w:pPr (and the paragraph style it names) and a
  // w:rPrChange's w:rPr are the formatting from before the change. The resolved properties stay
  // the new ones.
  Fixture f;
  f.styles = Styles("<w:style w:type=\"paragraph\" w:styleId=\"Heading1\"><w:rPr><w:sz w:val=\"32\"/></w:rPr></w:style>");
  f.numbering = "<w:abstractNum w:abstractNumId=\"0\"><w:lvl w:ilvl=\"0\"><w:start w:val=\"1\"/><w:numFmt w:val=\"decimal\"/>"
                "<w:lvlText w:val=\"%1.\"/><w:pPr><w:ind w:left=\"720\" w:hanging=\"360\"/></w:pPr></w:lvl></w:abstractNum>"
                "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>";
  f.body = "<w:p><w:pPr><w:pStyle w:val=\"Heading1\"/><w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"1\"/></w:numPr>"
           "<w:rPr><w:sz w:val=\"40\"/><w:rPrChange w:id=\"1\" w:author=\"A\"><w:rPr/></w:rPrChange></w:rPr>"
           "<w:pPrChange w:id=\"2\" w:author=\"A\"><w:pPr><w:jc w:val=\"center\"/></w:pPr></w:pPrChange></w:pPr>"
           "<w:r><w:rPr><w:i/><w:rPrChange w:id=\"3\" w:author=\"A\"><w:rPr><w:u w:val=\"single\"/></w:rPr></w:rPrChange>"
           "</w:rPr><w:t>a</w:t></w:r><w:r><w:t>b</w:t></w:r></w:p>"
           "<w:p><w:r><w:rPr><w:i/></w:rPr><w:t>c</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  const std::vector<uint32_t> p = Paras(*doc);
  CHECK(p.size() == 2);
  if (p.size() != 2) return;
  const std::vector<TextRun> runs = doc->Runs(p[0]);
  CHECK(runs.size() == 2);
  if (runs.size() != 2) return;
  // Now: Heading 1, numbered, not centered.
  const ParaProps& now = doc->ResolvedPara(p[0]);
  CHECK(now.style == "Heading1" && now.numId == "1" && now.jc != Justification::Center && now.indStart.Value() == 720);
  // Before: Normal, centered, no list.
  const ParaProps& was = doc->OriginalPara(p[0]);
  CHECK(was.style == "Normal" && was.numId.empty() && was.jc == Justification::Center && was.indStart.Value() == 0);
  // "a": italic and 16 pt now; underlined and 10 pt before (Normal's size).
  const RunProps& a = doc->ResolvedRun(p[0], runs[0].run);
  CHECK(a.italic && a.size.Value() == 32 && a.underline == Underline::None);
  const RunProps& oldA = doc->OriginalRun(p[0], runs[0].run);
  CHECK(!oldA.italic && oldA.size.Value() == 20 && oldA.underline == Underline::Single);
  // "b" has no change of its own, but its paragraph's style was Normal.
  CHECK(doc->ResolvedRun(p[0], runs[1].run).size.Value() == 32 && doc->OriginalRun(p[0], runs[1].run).size.Value() == 20);
  CHECK(doc->ResolvedMark(p[0]).size.Value() == 40 && doc->OriginalMark(p[0]).size.Value() == 20);
  // Asked again, the same answers (cached).
  CHECK(&doc->OriginalRun(p[0], runs[0].run) == &oldA && &doc->OriginalPara(p[0]) == &was);
  // Nothing changed: the resolved properties themselves.
  CHECK(&doc->OriginalPara(p[1]) == &doc->ResolvedPara(p[1]));
  const Node c = doc->Runs(p[1]).at(0).run;
  CHECK(&doc->OriginalRun(p[1], c) == &doc->ResolvedRun(p[1], c) && doc->OriginalRun(p[1], c).italic);
  CHECK(&doc->OriginalMark(p[1]) == &doc->ResolvedMark(p[1]));
}

TEST(Resolve_D1_TogglesResetInParagraphStylesAndToggleInCharacterStyles) {
  Fixture f;
  f.styles = Styles(
      "<w:style w:type=\"paragraph\" w:styleId=\"BoldPara\"><w:rPr><w:b/><w:i/></w:rPr></w:style>"
      "<w:style w:type=\"paragraph\" w:styleId=\"BoldChild\"><w:basedOn w:val=\"BoldPara\"/><w:rPr><w:b/></w:rPr></w:style>"
      "<w:style w:type=\"paragraph\" w:styleId=\"NotBold\"><w:basedOn w:val=\"BoldPara\"/><w:rPr><w:b w:val=\"0\"/></w:rPr></w:style>"
      "<w:style w:type=\"character\" w:styleId=\"BoldChar\"><w:rPr><w:b/></w:rPr></w:style>"
      "<w:style w:type=\"character\" w:styleId=\"NoBoldChar\"><w:rPr><w:b w:val=\"0\"/></w:rPr></w:style>"
      "<w:style w:type=\"table\" w:styleId=\"BoldTable\"><w:rPr><w:b/></w:rPr></w:style>");
  auto run = [](const char* pStyle, const char* rPr) {
    return std::string("<w:p><w:pPr><w:pStyle w:val=\"") + pStyle + "\"/></w:pPr><w:r><w:rPr>" + rPr +
           "</w:rPr><w:t>x</w:t></w:r></w:p>";
  };
  f.body = run("BoldPara", "<w:rStyle w:val=\"BoldChar\"/>") +   // 0: bold paragraph style, bold character style
           run("BoldPara", "<w:rStyle w:val=\"NoBoldChar\"/>") +   // 1: a false toggle changes nothing
           run("Normal", "<w:rStyle w:val=\"BoldChar\"/>") +       // 2: toggles off -> on
           run("BoldChild", "") +                                  // 3: no toggling within one style chain
           run("NotBold", "") +                                    // 4: the closest style in the chain wins
           run("BoldPara", "<w:rStyle w:val=\"BoldChar\"/><w:b/>") +  // 5: direct formatting is absolute
           run("Normal", "") +                                     // 6: DefaultParagraphFont is ignored (D6)
           "<w:tbl><w:tblPr><w:tblStyle w:val=\"BoldTable\"/></w:tblPr><w:tblGrid><w:gridCol w:w=\"100\"/></w:tblGrid>"
           "<w:tr><w:tc>" + run("NotBold", "") + run("Normal", "") + "</w:tc></w:tr></w:tbl>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  CHECK(!RunAt(*doc, 0).bold);       // ECMA agrees: two levels toggle off
  CHECK(RunAt(*doc, 0).italic);
  CHECK(RunAt(*doc, 1).bold);
  CHECK(RunAt(*doc, 2).bold);
  CHECK(RunAt(*doc, 3).bold);        // ECMA would XOR the chain to not bold... it doesn't: neither does Word
  CHECK(!RunAt(*doc, 4).bold);
  CHECK(RunAt(*doc, 5).bold);
  CHECK(!RunAt(*doc, 6).bold && RunAt(*doc, 6).color.automatic);
  // In a bold table style: the paragraph style resets bold off (ECMA would toggle it on: true XOR false).
  CHECK(!RunAt(*doc, 7).bold);
  CHECK(RunAt(*doc, 8).bold);
}

TEST(Resolve_D2_DirectNumberingOverridesTheParagraphStyle) {
  Fixture f;
  f.styles = Styles(
      "<w:style w:type=\"paragraph\" w:styleId=\"Indented\"><w:pPr><w:ind w:left=\"2880\"/></w:pPr></w:style>"
      "<w:style w:type=\"paragraph\" w:styleId=\"ListStyle\"><w:pPr><w:numPr><w:numId w:val=\"1\"/></w:numPr>"
      "<w:ind w:left=\"2880\"/></w:pPr></w:style>");
  f.numbering = "<w:abstractNum w:abstractNumId=\"0\"><w:lvl w:ilvl=\"0\"><w:start w:val=\"1\"/><w:numFmt w:val=\"decimal\"/>"
                "<w:lvlText w:val=\"%1.\"/><w:pPr><w:tabs><w:tab w:val=\"num\" w:pos=\"720\"/></w:tabs>"
                "<w:ind w:left=\"720\" w:hanging=\"360\"/></w:pPr></w:lvl></w:abstractNum>"
                "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>";
  f.body = "<w:p><w:pPr><w:pStyle w:val=\"Indented\"/><w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"1\"/></w:numPr></w:pPr></w:p>"
           "<w:p><w:pPr><w:pStyle w:val=\"ListStyle\"/></w:pPr></w:p>"
           "<w:p><w:pPr><w:pStyle w:val=\"ListStyle\"/><w:numPr><w:numId w:val=\"0\"/></w:numPr></w:pPr></w:p>"
           "<w:p><w:pPr><w:pStyle w:val=\"Indented\"/><w:numPr><w:numId w:val=\"1\"/></w:numPr><w:ind w:left=\"100\"/></w:pPr></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  auto p = Paras(*doc);
  const ParaProps& direct = doc->ResolvedPara(p[0]);
  CHECK(direct.indStart.Value() == 720 && direct.firstLine.Value() == -360);  // ECMA: the style's 2880
  CHECK(direct.numId == "1" && direct.tabs.size() == 1 && direct.tabs[0].align == TabAlign::Num);
  const ParaProps& fromStyle = doc->ResolvedPara(p[1]);
  CHECK(fromStyle.indStart.Value() == 2880 && fromStyle.firstLine.Value() == -360 && fromStyle.numId == "1");
  CHECK(doc->ResolvedPara(p[2]).numId.empty() && !doc->Label(p[2]));
  // Out of the style's list, it loses the style's indents too (as Word does).
  CHECK(doc->ResolvedPara(p[2]).indStart.Value() == 0 && doc->ResolvedPara(p[2]).firstLine.Value() == 0);
  CHECK(doc->ResolvedPara(p[3]).indStart.Value() == 100);  // direct indents beat both
}

TEST(Numbering_ASectionBreakParagraphIsNotNumbered) {
  // An empty numbered paragraph that only holds a section break gets no number and isn't counted.
  Fixture f;
  f.numbering = "<w:abstractNum w:abstractNumId=\"0\"><w:lvl w:ilvl=\"0\"><w:start w:val=\"1\"/><w:numFmt w:val=\"decimal\"/>"
                "<w:lvlText w:val=\"%1\"/></w:lvl></w:abstractNum><w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>";
  const std::string num = "<w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"1\"/></w:numPr>";
  f.body = "<w:p><w:pPr>" + num + "</w:pPr><w:r><w:t>One</w:t></w:r></w:p>"
           "<w:p><w:pPr>" + num + "<w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/></w:sectPr></w:pPr><w:bookmarkStart w:id=\"1\" w:name=\"b\"/></w:p>"
           "<w:p><w:pPr>" + num + "</w:pPr><w:r><w:t>Two</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  auto p = Paras(*doc);
  CHECK(p.size() == 3);
  if (p.size() != 3) return;
  CHECK(!doc->Label(p[1]));
  auto two = doc->Label(p[2]);
  CHECK(two && two->text == u"2");
}

TEST(Resolve_D5_StyleListLevelSitsBetweenTheStyleAndItsBase) {
  Fixture f;
  f.styles = Styles(
      "<w:style w:type=\"paragraph\" w:styleId=\"ListParagraph\"><w:pPr><w:ind w:left=\"720\"/></w:pPr></w:style>"
      "<w:style w:type=\"paragraph\" w:styleId=\"Numbered\"><w:basedOn w:val=\"ListParagraph\"/><w:pPr><w:numPr>"
      "<w:numId w:val=\"1\"/></w:numPr></w:pPr></w:style>"
      "<w:style w:type=\"paragraph\" w:styleId=\"Numbered2\"><w:basedOn w:val=\"Numbered\"/><w:pPr><w:numPr>"
      "<w:ilvl w:val=\"1\"/></w:numPr></w:pPr></w:style>");
  auto lvl = [](int i, const char* left) {
    return "<w:lvl w:ilvl=\"" + std::to_string(i) + "\"><w:start w:val=\"1\"/><w:numFmt w:val=\"decimal\"/>" +
           "<w:lvlText w:val=\"%1.\"/><w:pPr><w:ind w:left=\"" + left + "\" w:hanging=\"360\"/></w:pPr></w:lvl>";
  };
  f.numbering = "<w:abstractNum w:abstractNumId=\"0\">" + lvl(0, "3240") + lvl(1, "4320") +
                "</w:abstractNum><w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>";
  f.body = "<w:p><w:pPr><w:pStyle w:val=\"Numbered\"/></w:pPr></w:p><w:p><w:pPr><w:pStyle w:val=\"Numbered2\"/></w:pPr></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  auto p = Paras(*doc);
  // The level's indent beats List Paragraph's, which the list's style is only based on.
  CHECK(doc->ResolvedPara(p[0]).indStart.Value() == 3240 && doc->ResolvedPara(p[0]).ilvl == 0);
  // A style that sets only the level keeps the list of the style it's based on.
  CHECK(doc->ResolvedPara(p[1]).numId == "1" && doc->ResolvedPara(p[1]).ilvl == 1);
  CHECK(doc->ResolvedPara(p[1]).indStart.Value() == 4320);
}

TEST(Resolve_D3_DefaultStyleSizeDoesNotOverrideTableStyle) {
  for (bool override : {false, true}) {
    Fixture f;
    f.styles = "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:pPr><w:jc w:val=\"left\"/></w:pPr>"
               "<w:rPr><w:sz w:val=\"24\"/></w:rPr></w:style>"
               "<w:style w:type=\"paragraph\" w:styleId=\"Big\"><w:basedOn w:val=\"Normal\"/><w:rPr><w:sz w:val=\"24\"/></w:rPr></w:style>"
               "<w:style w:type=\"table\" w:styleId=\"T\"><w:pPr><w:jc w:val=\"center\"/></w:pPr><w:rPr><w:sz w:val=\"16\"/></w:rPr></w:style>";
    if (override)
      f.settings = "<w:compat><w:compatSetting w:name=\"overrideTableStyleFontSizeAndJustification\" "
                   "w:uri=\"http://schemas.microsoft.com/office/word\" w:val=\"1\"/></w:compat>";
    f.body = "<w:tbl><w:tblPr><w:tblStyle w:val=\"T\"/></w:tblPr><w:tblGrid><w:gridCol w:w=\"100\"/></w:tblGrid><w:tr><w:tc>"
             "<w:p><w:r><w:t>a</w:t></w:r></w:p><w:p><w:pPr><w:pStyle w:val=\"Big\"/></w:pPr><w:r><w:t>b</w:t></w:r></w:p>"
             "</w:tc></w:tr></w:tbl><w:p><w:r><w:t>c</w:t></w:r></w:p>";
    auto doc = LoadOk(f);
    CHECK(doc);
    if (!doc) return;
    CHECK(RunAt(*doc, 0).size.Value() == (override ? 24 : 16));
    CHECK(doc->ResolvedPara(Paras(*doc)[0]).jc == (override ? Justification::Start : Justification::Center));
    CHECK(RunAt(*doc, 1).size.Value() == 24);  // only the default style itself
    CHECK(RunAt(*doc, 2).size.Value() == 24);  // outside the table
  }
}

TEST(Resolve_D4_D5_TableConditionalFormatting) {
  Fixture f;
  auto shd = [](const char* type, const char* fill) {
    return std::string("<w:tblStylePr w:type=\"") + type + "\"><w:tcPr><w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"" +
           fill + "\"/></w:tcPr></w:tblStylePr>";
  };
  f.styles = Styles(
      "<w:style w:type=\"table\" w:styleId=\"Grid\"><w:tblPr><w:tblStyleRowBandSize w:val=\"1\"/>"
      "<w:tblBorders><w:insideH w:val=\"single\" w:sz=\"8\"/></w:tblBorders></w:tblPr>"
      "<w:tcPr><w:shd w:val=\"clear\" w:fill=\"EEEEEE\"/></w:tcPr>" +
      shd("wholeTable", "000001") + shd("firstRow", "111111") + shd("firstCol", "222222") + shd("band1Horz", "333333") +
      shd("band2Horz", "444444") + shd("lastCol", "555555") + shd("nwCell", "666666") + shd("seCell", "777777") +
      "</w:style>"
      "<w:style w:type=\"table\" w:styleId=\"NoBands\"><w:basedOn w:val=\"Grid\"/><w:tblPr><w:tblStyleRowBandSize "
      "w:val=\"0\"/></w:tblPr></w:style>");
  auto table = [](const char* style, const char* look) {
    std::string t = std::string("<w:tbl><w:tblPr><w:tblStyle w:val=\"") + style + "\"/>" + look +
                    "</w:tblPr><w:tblGrid><w:gridCol w:w=\"100\"/><w:gridCol w:w=\"100\"/><w:gridCol w:w=\"100\"/></w:tblGrid>";
    for (int r = 0; r < 4; ++r) {
      t += "<w:tr>";
      for (int c = 0; c < 3; ++c) t += "<w:tc><w:p/></w:tc>";
      t += "</w:tr>";
    }
    return t + "</w:tbl><w:p/>";
  };
  f.body = table("Grid", "") +  // D5: no tblLook means firstRow, firstColumn, noVBand
           table("Grid", "<w:tblLook w:val=\"0120\"/>") +  // firstRow and lastColumn, from the bits
           table("Grid", "<w:tblLook w:val=\"0120\" w:firstRow=\"0\" w:lastColumn=\"1\"/>") +  // named attributes win
           table("NoBands", "");
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  auto blocks = doc->Blocks(doc->MainStory());
  auto fill = [&](size_t t, size_t r, size_t c) {
    Node cell = doc->RowCells(doc->TableRows(blocks[t * 2].node)[r])[c];
    return doc->ResolvedCell(cell).shading.fill.rgb;
  };
  // Table 1: row 0 is the header; column 0 is first; rows 1-3 band 1, 2, 1.
  CHECK(fill(0, 0, 0) == 0x666666);  // nwCell: both the first row and the first column
  CHECK(fill(0, 0, 1) == 0x111111);  // first row over the whole table
  CHECK(fill(0, 1, 0) == 0x222222);  // first column over the row band (D4: bands come first)
  CHECK(fill(0, 1, 1) == 0x333333 && fill(0, 2, 1) == 0x444444 && fill(0, 3, 2) == 0x333333);
  CHECK(fill(0, 3, 2) != 0x777777);  // seCell needs lastRow and lastColumn, both off
  // Table 2: firstRow and lastColumn from w:val; the first row beats the last column (D4).
  CHECK(fill(1, 0, 2) == 0x111111 && fill(1, 2, 2) == 0x555555 && fill(1, 1, 0) == 0x333333);
  // Table 3: the named attributes are read and w:val is not (D5).
  CHECK(fill(2, 0, 1) == 0x333333 && fill(2, 1, 2) == 0x555555);
  // Table 4: band size 0 means no banding.
  CHECK(fill(3, 1, 1) == 0xEEEEEE);
  // Borders: the cell's inner edges take the style's insideH.
  Node mid = doc->RowCells(doc->TableRows(blocks[0].node)[1])[1];
  CellProps cp = doc->ResolvedCell(mid);
  CHECK(cp.top.style == BorderStyle::Single && cp.top.width.Value() == 8 && !cp.start.Present());
  CHECK(cp.margins.start.value == 108 && doc->ResolvedTable(blocks[0].node).look.noVBand);
}

TEST(Resolve_D7_D8_FontsAndDefaults) {
  Fixture f;
  f.theme = "<a:clrScheme name=\"c\"><a:dk1><a:sysClr val=\"windowText\" lastClr=\"000000\"/></a:dk1><a:lt1><a:srgbClr "
            "val=\"FFFFFF\"/></a:lt1><a:dk2><a:srgbClr val=\"1F497D\"/></a:dk2><a:lt2><a:srgbClr val=\"EEECE1\"/></a:lt2>"
            "<a:accent1><a:srgbClr val=\"4F81BD\"/></a:accent1><a:accent2><a:srgbClr val=\"C0504D\"/></a:accent2>"
            "</a:clrScheme><a:fontScheme name=\"f\"><a:majorFont><a:latin typeface=\"Cambria\"/><a:ea typeface=\"\"/>"
            "<a:cs typeface=\"\"/><a:font script=\"Jpan\" typeface=\"MS Mincho\"/></a:majorFont><a:minorFont>"
            "<a:latin typeface=\"Calibri\"/><a:ea typeface=\"\"/><a:cs typeface=\"Arial\"/></a:minorFont></a:fontScheme>";
  f.settings = "<w:themeFontLang w:val=\"en-US\" w:eastAsia=\"ja-JP\"/>";
  f.styles = Styles("<w:style w:type=\"paragraph\" w:styleId=\"Themed\"><w:rPr><w:rFonts w:asciiTheme=\"minorHAnsi\" "
                    "w:hAnsiTheme=\"minorHAnsi\" w:eastAsiaTheme=\"majorEastAsia\" w:cstheme=\"minorBidi\"/></w:rPr></w:style>");
  f.body = "<w:p><w:pPr><w:pStyle w:val=\"Themed\"/></w:pPr><w:r><w:t>a</w:t></w:r>"
           "<w:r><w:rPr><w:rFonts w:ascii=\"Arial\"/></w:rPr><w:t>b</w:t></w:r>"
           "<w:r><w:rPr><w:rFonts w:ascii=\"Arial\" w:asciiTheme=\"majorHAnsi\"/></w:rPr><w:t>c</w:t></w:r>"
           "<w:r><w:rPr><w:color w:val=\"000000\" w:themeColor=\"accent2\" w:themeShade=\"BF\"/></w:rPr><w:t>d</w:t></w:r>"
           "<w:r><w:rPr><w:color w:val=\"000000\" w:themeColor=\"accent1\" w:themeTint=\"99\"/></w:rPr><w:t>e</w:t></w:r>"
           "<w:r><w:rPr><w:color w:val=\"000000\" w:themeColor=\"text2\"/></w:rPr><w:t>f</w:t></w:r></w:p>"
           "<w:p><w:r><w:t>plain</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  const RunProps& a = RunAt(*doc, 0, 0);
  CHECK(a.fonts.ascii == "Calibri" && a.fonts.hAnsi == "Calibri" && a.fonts.eastAsia == "MS Mincho" && a.fonts.cs == "Arial");
  CHECK(RunAt(*doc, 0, 1).fonts.ascii == "Arial" && RunAt(*doc, 0, 1).fonts.hAnsi == "Calibri");  // D7: later level wins
  CHECK(RunAt(*doc, 0, 2).fonts.ascii == "Cambria");  // D7: in one element the theme font wins
  // [MS-OI29500] 2.1.72's own examples: C0504D shaded BF is 943634, 4F81BD tinted 99 is 95B3D7.
  CHECK(RunAt(*doc, 0, 3).color.rgb == 0x943634 && RunAt(*doc, 0, 4).color.rgb == 0x95B3D7);
  CHECK(RunAt(*doc, 0, 5).color.rgb == 0x1F497D);  // text2 through the default mapping to dark2
  const RunProps& plain = RunAt(*doc, 1);
  CHECK(plain.fonts.ascii == "Times New Roman" && plain.size.Value() == 20);  // D8
}

TEST(Resolve_ParagraphPropertiesLayer) {
  Fixture f;
  f.styles = "<w:docDefaults><w:pPrDefault><w:pPr><w:spacing w:after=\"160\" w:line=\"259\" w:lineRule=\"auto\"/></w:pPr>"
             "</w:pPrDefault></w:docDefaults>" +
             Styles("<w:style w:type=\"paragraph\" w:styleId=\"Base\"><w:pPr><w:tabs><w:tab w:val=\"left\" w:pos=\"1000\"/>"
                    "<w:tab w:val=\"right\" w:pos=\"9000\" w:leader=\"dot\"/></w:tabs><w:pBdr><w:bottom w:val=\"double\" "
                    "w:sz=\"6\" w:space=\"1\" w:color=\"4F81BD\"/></w:pBdr><w:ind w:firstLine=\"360\"/></w:pPr></w:style>"
                    "<w:style w:type=\"paragraph\" w:styleId=\"Child\"><w:basedOn w:val=\"Base\"/><w:pPr><w:tabs>"
                    "<w:tab w:val=\"clear\" w:pos=\"1000\"/><w:tab w:val=\"center\" w:pos=\"4500\"/></w:tabs>"
                    "<w:spacing w:before=\"240\"/><w:jc w:val=\"both\"/></w:pPr></w:style>");
  f.body = "<w:p><w:pPr><w:pStyle w:val=\"Child\"/><w:spacing w:line=\"360\" w:lineRule=\"exact\"/><w:ind w:hanging=\"200\"/>"
           "<w:keepNext/><w:widowControl w:val=\"0\"/></w:pPr></w:p><w:p><w:pPr><w:pStyle w:val=\"Missing\"/></w:pPr></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  const ParaProps& p = doc->ResolvedPara(Paras(*doc)[0]);
  CHECK(p.style == "Child" && p.jc == Justification::Both && p.keepNext && !p.widowControl);
  CHECK(p.spaceBefore.Value() == 240 && p.spaceAfter.Value() == 160 && p.line == 360 && p.lineRule == LineRule::Exact);
  CHECK(p.firstLine.Value() == -200);
  CHECK(p.tabs.size() == 2 && p.tabs[0].pos.Value() == 4500 && p.tabs[0].align == TabAlign::Center &&
        p.tabs[1].leader == TabLeader::Dot);
  CHECK(p.borders.bottom.style == BorderStyle::Double && p.borders.bottom.color.rgb == 0x4F81BD);
  const ParaProps& missing = doc->ResolvedPara(Paras(*doc)[1]);
  CHECK(missing.style == "Normal" && missing.line == 259 && missing.widowControl);
}

// ---------- Numbering ----------

TEST(Numbering_FormatsNumbers) {
  CHECK(FormatNumber(4, "upperRoman") == u"IV" && FormatNumber(1994, "lowerRoman") == u"mcmxciv");
  CHECK(FormatNumber(3, "lowerLetter") == u"c" && FormatNumber(28, "upperLetter") == u"BB");
  CHECK(FormatNumber(2, "ordinal") == u"2nd" && FormatNumber(13, "ordinal") == u"13th" && FormatNumber(21, "ordinal") == u"21st");
  CHECK(FormatNumber(21, "cardinalText") == u"Twenty-one" && FormatNumber(12, "ordinalText") == u"Twelfth");
  CHECK(FormatNumber(40, "ordinalText") == u"Fortieth" && FormatNumber(5, "decimalZero") == u"05");
  CHECK(FormatNumber(6, "chicago") == u"††" && FormatNumber(3, "decimalEnclosedCircle") == u"③");
  CHECK(FormatNumber(7, "none").empty() && FormatNumber(7, "someFutureFormat") == u"7");
  // Right-to-left and Asian scripts (D5).
  CHECK(FormatNumber(15, "hebrew1") == u"טו" && FormatNumber(123, "hebrew1") == u"קכג");
  CHECK(FormatNumber(3, "arabicAbjad") == u"ج" && FormatNumber(3, "arabicAlpha") == u"ت");
  CHECK(FormatNumber(12, "hindiNumbers") == u"١٢" && FormatNumber(23, "hebrew2") == u"אא");
  CHECK(FormatNumber(21, "japaneseCounting") == u"二十一" && FormatNumber(10, "chineseCounting") == u"十");
  CHECK(FormatNumber(105, "japaneseCounting") == u"百五" && FormatNumber(2, "aiueoFullWidth") == u"イ");
}

TEST(Numbering_LabelsCountRestartAndContinue) {
  Fixture f;
  auto lvl = [](int ilvl, const char* fmt, const char* text, const char* extra = "") {
    return "<w:lvl w:ilvl=\"" + std::to_string(ilvl) + "\"><w:start w:val=\"1\"/><w:numFmt w:val=\"" + fmt +
           "\"/>" + extra + "<w:lvlText w:val=\"" + text + "\"/><w:lvlJc w:val=\"left\"/></w:lvl>";
  };
  f.numbering = "<w:abstractNum w:abstractNumId=\"0\">" + lvl(0, "decimal", "%1.") + lvl(1, "lowerLetter", "%1.%2)") +
                lvl(2, "upperRoman", "%3", "<w:lvlRestart w:val=\"0\"/>") + lvl(3, "decimal", "%1.%5") + "</w:abstractNum>"
                "<w:abstractNum w:abstractNumId=\"1\">" + lvl(0, "bullet", "\xEF\x82\xB7") +
                lvl(1, "upperRoman", "%1.%2", "<w:isLgl/>") + "</w:abstractNum>"
                "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>"
                "<w:num w:numId=\"2\"><w:abstractNumId w:val=\"0\"/></w:num>"
                "<w:num w:numId=\"3\"><w:abstractNumId w:val=\"0\"/><w:lvlOverride w:ilvl=\"0\"><w:startOverride "
                "w:val=\"7\"/></w:lvlOverride></w:num>"
                "<w:num w:numId=\"4\"><w:abstractNumId w:val=\"1\"/></w:num>";
  auto p = [](int num, int ilvl) {
    return "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"" + std::to_string(ilvl) + "\"/><w:numId w:val=\"" + std::to_string(num) +
           "\"/></w:numPr></w:pPr><w:r><w:t>x</w:t></w:r></w:p>";
  };
  f.body = p(1, 0) + p(1, 1) + p(1, 1) + p(1, 2) + p(1, 0) + p(1, 1) + p(1, 2) +  // 1. a) b) I 2. a) II
           p(2, 0) +                                                             // 3.: num 2 continues the list
           p(3, 0) + p(3, 0) +                                                   // 7. 8.: the override restarts it
           p(1, 3) +                                                             // %5 is below level 4: no label
           p(4, 0) + p(4, 1) + "<w:p><w:r><w:t>unnumbered</w:t></w:r></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  std::vector<std::u16string> labels;
  for (uint32_t para : Paras(*doc)) {
    auto l = doc->Label(para);
    labels.push_back(l ? l->text : u"-");
  }
  std::vector<std::u16string> want = {u"1.", u"1.a)", u"1.b)", u"I", u"2.", u"2.a)", u"II", u"3.", u"7.", u"8.",
                                      u"", u"", u"1.1", u"-"};
  CHECK(labels == want);
  for (size_t i = 0; i < labels.size() && i < want.size(); ++i)
    if (labels[i] != want[i]) std::printf("  label %zu: %s\n", i, ToUtf8(labels[i]).c_str());
  // Asked for out of order, the labels are the same.
  auto again = LoadOk(f);
  CHECK(again);
  if (!again) return;
  for (size_t i : {4, 0, 9, 2, 13, 11}) {
    auto l = again->Label(Paras(*again)[i]);
    CHECK((l ? l->text : u"-") == want[i]);
  }
  auto bullet = doc->Label(Paras(*doc)[11]);
  CHECK(bullet && bullet->bullet && bullet->suffix == "tab" && bullet->numFmt == "bullet");
}

TEST(Numbering_OverrideLevelsAndRestartLevels) {
  Fixture f;
  f.numbering =
      "<w:abstractNum w:abstractNumId=\"0\"><w:lvl w:ilvl=\"0\"><w:start w:val=\"1\"/><w:numFmt w:val=\"decimal\"/>"
      "<w:lvlText w:val=\"%1.\"/></w:lvl><w:lvl w:ilvl=\"1\"><w:start w:val=\"1\"/><w:numFmt w:val=\"lowerLetter\"/>"
      "<w:lvlText w:val=\"%2\"/></w:lvl><w:lvl w:ilvl=\"2\"><w:start w:val=\"1\"/><w:numFmt w:val=\"lowerRoman\"/>"
      "<w:lvlRestart w:val=\"1\"/><w:lvlText w:val=\"%3\"/></w:lvl></w:abstractNum>"
      "<w:abstractNum w:abstractNumId=\"1\"><w:lvl w:ilvl=\"0\"><w:start w:val=\"1\"/><w:numFmt w:val=\"decimal\"/>"
      "<w:lvlText w:val=\"%1.\"/></w:lvl></w:abstractNum>"
      "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>"
      "<w:num w:numId=\"2\"><w:abstractNumId w:val=\"1\"/><w:lvlOverride w:ilvl=\"0\"><w:lvl w:ilvl=\"0\">"
      "<w:start w:val=\"5\"/><w:numFmt w:val=\"upperLetter\"/><w:lvlText w:val=\"(%1)\"/></w:lvl></w:lvlOverride></w:num>";
  auto p = [](int num, int ilvl) {
    return "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"" + std::to_string(ilvl) + "\"/><w:numId w:val=\"" + std::to_string(num) +
           "\"/></w:numPr></w:pPr></w:p>";
  };
  // Level 2 restarts after level 0 only (its w:lvlRestart is 1), not after level 1.
  f.body = p(1, 0) + p(1, 2) + p(1, 1) + p(1, 2) + p(1, 0) + p(1, 2) + p(2, 0) + p(2, 0);
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  std::vector<std::u16string> labels;
  for (uint32_t para : Paras(*doc)) {
    auto l = doc->Label(para);
    labels.push_back(l ? l->text : u"-");
  }
  // The override's w:lvl gives the format and text, but its w:start is ignored (2.1.292).
  std::vector<std::u16string> want = {u"1.", u"i", u"a", u"ii", u"2.", u"i", u"(A)", u"(B)"};
  CHECK(labels == want);
}

// ---------- The reference registry ----------

TEST(Doc_RenamesReferencesInModelAndStore) {
  Fixture f;
  f.body = "<w:p><w:bookmarkStart w:id=\"1\" w:name=\"Intro\"/><w:r><w:t>x</w:t></w:r><w:bookmarkEnd w:id=\"1\"/>"
           "<w:hyperlink w:anchor=\"Intro\"><w:r><w:t>see</w:t></w:r></w:hyperlink>"
           "<x:keep><w:hyperlink w:anchor=\"Intro\"/></x:keep></w:p>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  size_t changed = doc->RenameReference(fw::pkg::RefKind::BookmarkName, "intro", "Start");
  CHECK(changed == 3);  // the bookmark, the link, and the link inside kept content (names ignore case)
  uint32_t p = FirstPara(*doc);
  CHECK(doc->Runs(p)[1].hyperlink.AttrOr(Name("anchor")) == "Start");
  bool kept = false;
  doc->Store().ForEachFragment([&](fw::pkg::NodeId, fw::pkg::Fragment& fr) {
    kept |= fr.xml.find("w:anchor=\"Start\"") != std::string::npos;
  });
  CHECK(kept);
  // Hooked into a registry, the registry's renames reach the model.
  fw::pkg::ReferenceRegistry registry = fw::pkg::ReferenceRegistry::Default();
  doc->AttachTo(registry);
  registry.Rename(fw::pkg::RefKind::BookmarkName, "Start", "Begin", pugi::xml_node(), nullptr);
  CHECK(doc->Runs(p)[1].hyperlink.AttrOr(Name("anchor")) == "Begin");
  doc.reset();
  registry.Rename(fw::pkg::RefKind::BookmarkName, "Begin", "Again", pugi::xml_node(), nullptr);  // no document: no crash
}

TEST(Doc_CommentsRepliesAndHeaders) {
  Fixture f;
  f.comments = "<w:comment w:id=\"1\" w:author=\"A\"><w:p w14:paraId=\"0000000A\"><w:r><w:t>q</w:t></w:r></w:p></w:comment>"
               "<w:comment w:id=\"2\" w:author=\"B\"><w:p w14:paraId=\"0000000B\"><w:r><w:t>a</w:t></w:r></w:p></w:comment>";
  f.commentsEx = "<w15:commentEx w15:paraId=\"0000000A\" w15:done=\"1\"/>"
                 "<w15:commentEx w15:paraId=\"0000000B\" w15:paraIdParent=\"0000000A\" w15:done=\"0\"/>";
  f.header = "<w:p><w:r><w:t>head</w:t></w:r></w:p>";
  f.body = "<w:p><w:commentRangeStart w:id=\"1\"/><w:r><w:t>text</w:t></w:r><w:commentRangeEnd w:id=\"1\"/>"
           "<w:r><w:commentReference w:id=\"1\"/></w:r></w:p>"
           "<w:p><w:pPr><w:sectPr><w:headerReference w:type=\"default\" r:id=\"rIdHeader\"/><w:titlePg/></w:sectPr></w:pPr></w:p>"
           "<w:sectPr><w:headerReference w:type=\"first\" r:id=\"rIdHeader\"/></w:sectPr>";
  auto doc = LoadOk(f);
  CHECK(doc);
  if (!doc) return;
  auto comments = doc->Comments();
  CHECK(comments.size() == 2);
  if (comments.size() != 2) return;
  CHECK(comments[0].done && comments[0].start.offset == 0 && comments[0].end.offset == 4 && comments[0].reference.offset == 4);
  CHECK(!comments[1].done && comments[1].parentId == "1");
  // The second section inherits the first's default header and names its own first-page one.
  const auto& secs = doc->Sections();
  CHECK(secs.size() == 2 && secs[0].headers[0] != kNone && secs[1].headers[0] == secs[0].headers[0] &&
        secs[1].headers[1] == secs[0].headers[0] && secs[0].headers[1] == kNone);
  CHECK(secs.size() == 2 && secs[0].headers[0] != kNone && doc->Stories()[secs[0].headers[0]].kind == StoryKind::Header);
  CHECK(RoundTrips(*doc));
}
