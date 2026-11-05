#include "TestHarness.h"

#include <string>
#include <vector>

#include "PkgTestUtil.h"
#include "pkg/Check.h"
#include "pkg/Derived.h"
#include "pkg/Formula.h"
#include "pkg/References.h"
#include "pkg/RoundTrip.h"
#include "pkg/XmlDom.h"

using namespace fw::pkg;

namespace {

#define W_NS "http://schemas.openxmlformats.org/wordprocessingml/2006/main"
#define X_NS "http://schemas.openxmlformats.org/spreadsheetml/2006/main"
#define P_NS "http://schemas.openxmlformats.org/presentationml/2006/main"
#define R_NS "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
#define RELS_NS "http://schemas.openxmlformats.org/package/2006/relationships"
#define TYPES_NS "http://schemas.openxmlformats.org/package/2006/content-types"
#define REL_TYPE "http://schemas.openxmlformats.org/officeDocument/2006/relationships/"

const char* kWordOpen =
    "<w:document xmlns:w=\"" W_NS "\" xmlns:r=\"" R_NS "\" "
    "xmlns:w14=\"http://schemas.microsoft.com/office/word/2010/wordml\" "
    "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
    "xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\" "
    "xmlns:v=\"urn:schemas-microsoft-com:vml\" xmlns:o=\"urn:schemas-microsoft-com:office:office\" "
    "mc:Ignorable=\"w14\"><w:body>";
const char* kWordClose = "</w:body></w:document>";

size_t Count(const std::vector<FoundRef>& refs, RefKind kind) {
  size_t n = 0;
  for (const auto& r : refs) n += r.kind == kind;
  return n;
}

bool Has(const std::vector<std::string>& list, std::string_view needle) {
  for (const auto& s : list)
    if (s.find(needle) != std::string::npos) return true;
  return false;
}

using Parts = std::vector<std::pair<std::string, std::string>>;

std::string Rels(const std::string& items) { return "<Relationships xmlns=\"" RELS_NS "\">" + items + "</Relationships>"; }
std::string Rel(const char* id, const char* type, const char* target) {
  return std::string("<Relationship Id=\"") + id + "\" Type=\"" REL_TYPE + type + "\" Target=\"" + target + "\"/>";
}
std::string Types(const char* mainPart, const char* mainType, const std::string& more = {}) {
  return std::string("<Types xmlns=\"" TYPES_NS "\">"
                     "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                     "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
                     "<Default Extension=\"xlsx\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet\"/>"
                     "<Default Extension=\"vml\" ContentType=\"application/vnd.openxmlformats-officedocument.vmlDrawing\"/>"
                     "<Override PartName=\"") +
         mainPart + "\" ContentType=\"" + mainType + "\"/>" + more + "</Types>";
}

// A Word package: the body, the document's relationships and any other parts.
Parts Word(const std::string& body, const std::string& rels = {}, const Parts& extra = {}) {
  Parts p = {
      {"[Content_Types].xml",
       Types("/word/document.xml", "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml")},
      {"_rels/.rels", Rels(Rel("rId1", "officeDocument", "word/document.xml"))},
      {"word/document.xml", kWordOpen + body + kWordClose},
      {"word/_rels/document.xml.rels", Rels(rels)},
  };
  p.insert(p.end(), extra.begin(), extra.end());
  return p;
}

// An Excel package with one sheet, Data (sheetId 1).
Parts Excel(const std::string& sheetXml, const std::string& rels = {}, const Parts& extra = {}) {
  Parts p = {
      {"[Content_Types].xml",
       Types("/xl/workbook.xml", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml")},
      {"_rels/.rels", Rels(Rel("rId1", "officeDocument", "xl/workbook.xml"))},
      {"xl/workbook.xml", "<workbook xmlns=\"" X_NS "\" xmlns:r=\"" R_NS "\"><sheets>"
                          "<sheet name=\"Data\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>"},
      {"xl/_rels/workbook.xml.rels", Rels(Rel("rId1", "worksheet", "worksheets/sheet1.xml") + rels)},
      {"xl/worksheets/sheet1.xml", "<worksheet xmlns=\"" X_NS "\" xmlns:r=\"" R_NS "\">" + sheetXml + "</worksheet>"},
  };
  p.insert(p.end(), extra.begin(), extra.end());
  return p;
}

CheckReport CheckParts(const Parts& parts) {
  Status st;
  auto pkg = test::OpenBytes(test::MakeZip(parts), st);
  if (!pkg) {
    CheckReport rep;
    rep.errors.push_back("didn't open: " + st.message);
    return rep;
  }
  return CheckPackage(*pkg);
}

const char* kSampleBody =
    "<w:p w14:paraId=\"00000001\" w14:textId=\"77777777\">"
    "<w:bookmarkStart w:id=\"0\" w:name=\"Intro\"/><w:commentRangeStart w:id=\"3\"/><w:r><w:t>Hello</w:t></w:r>"
    "<w:commentRangeEnd w:id=\"3\"/><w:r><w:commentReference w:id=\"3\"/></w:r><w:bookmarkEnd w:id=\"0\"/>"
    "<w:r><w:fldChar w:fldCharType=\"begin\"/></w:r><w:r><w:instrText xml:space=\"preserve\"> REF Intro \\h </w:instrText></w:r>"
    "<w:r><w:fldChar w:fldCharType=\"separate\"/></w:r><w:r><w:t>Hello</w:t></w:r><w:r><w:fldChar w:fldCharType=\"end\"/></w:r>"
    "<w:hyperlink w:anchor=\"Intro\"><w:r><w:t>link</w:t></w:r></w:hyperlink>"
    "<w:r><w:footnoteReference w:id=\"2\"/></w:r><w:permStart w:id=\"9\"/><w:permEnd w:id=\"9\"/></w:p>"
    "<w:p w14:paraId=\"00000002\"><w:pPr><w:pStyle w:val=\"Heading1\"/><w:numPr><w:numId w:val=\"0\"/></w:numPr></w:pPr>"
    "<w:r><mc:AlternateContent><mc:Choice Requires=\"wps\"><w:drawing><wp:inline><wp:docPr id=\"7\" name=\"Pic\"/>"
    "</wp:inline></w:drawing></mc:Choice><mc:Fallback><w:pict><v:shapetype id=\"_x0000_t75\"/>"
    "<v:shape o:spid=\"_x0000_s1025\" type=\"#_x0000_t75\"/></w:pict></mc:Fallback></mc:AlternateContent></w:r></w:p>";

}  // namespace

// ---------- The registry ----------

TEST(References_FindEveryWordKind) {
  auto registry = ReferenceRegistry::Default();
  XmlDom dom;
  CHECK(dom.Parse(std::string(kWordOpen) + kSampleBody + kWordClose).Ok());
  auto refs = registry.Find(dom.Root());
  CHECK_EQ(Count(refs, RefKind::ParagraphId), size_t{2});
  CHECK_EQ(Count(refs, RefKind::TextId), size_t{1});
  CHECK_EQ(Count(refs, RefKind::BookmarkId), size_t{2});
  CHECK_EQ(Count(refs, RefKind::BookmarkName), size_t{3});  // the start, the REF field, the link
  CHECK_EQ(Count(refs, RefKind::CommentId), size_t{3});
  CHECK_EQ(Count(refs, RefKind::Field), size_t{3});
  CHECK_EQ(Count(refs, RefKind::FootnoteId), size_t{1});
  CHECK_EQ(Count(refs, RefKind::PermissionId), size_t{2});
  CHECK_EQ(Count(refs, RefKind::StyleId), size_t{1});
  CHECK_EQ(Count(refs, RefKind::NumberingId), size_t{0});  // numId 0 means no list
  CHECK_EQ(Count(refs, RefKind::DrawingId), size_t{1});
  CHECK_EQ(Count(refs, RefKind::VmlShapeId), size_t{1});
  CHECK_EQ(Count(refs, RefKind::VmlShapeType), size_t{2});
  for (const auto& r : refs) {
    if (r.kind == RefKind::DrawingId) CHECK(!r.alternate);  // the first branch
    if (r.kind == RefKind::VmlShapeType) CHECK(r.alternate);
    if (r.kind == RefKind::BookmarkName && !r.attr) CHECK(std::string(r.element.name()) == "w:instrText");
  }
  // Values come out of their syntax.
  std::vector<std::string> values;
  for (const auto& r : refs)
    if (r.kind == RefKind::BookmarkName || r.kind == RefKind::VmlShapeType)
      ForEachRefValue(*r.site, r.kind, r.Value(), [&](std::string_view v) { values.emplace_back(v); });
  CHECK(values == (std::vector<std::string>{"Intro", "Intro", "Intro", "_x0000_t75", "_x0000_t75"}));
}

TEST(References_ContentControlsAndPictureBullets) {
  auto registry = ReferenceRegistry::Default();
  XmlDom dom;
  CHECK(dom.Parse(std::string(kWordOpen) +
                  "<w:sdt><w:sdtPr><w:id w:val=\"-12\"/></w:sdtPr><w:sdtContent><w:p/></w:sdtContent></w:sdt>"
                  "<w:numPicBullet w:numPicBulletId=\"0\"/><w:lvl><w:lvlPicBulletId w:val=\"0\"/></w:lvl>"
                  "<w:p><w:bookmarkStart w:id=\"3\" w:name=\"x\"/><w:bookmarkEnd w:id=\"3\"/></w:p>" +
                  kWordClose)
            .Ok());
  auto refs = registry.Find(dom.Root());
  CHECK_EQ(Count(refs, RefKind::ContentControlId), size_t{1});  // w:id under w:sdtPr only, not w:bookmarkStart's
  CHECK_EQ(Count(refs, RefKind::PictureBulletId), size_t{2});
  IdRenewer renewer(registry);
  renewer.Take(RefKind::ContentControlId, "-12");
  auto renamed = renewer.Renew(dom.Root());
  CHECK(renamed.size() == 1 && renamed[0].kind == RefKind::ContentControlId && renamed[0].to == "0");
}

TEST(References_RenameEverySyntax) {
  auto registry = ReferenceRegistry::Default();
  XmlDom dom;
  CHECK(dom.Parse(std::string(kWordOpen) + kSampleBody + kWordClose).Ok());
  CHECK_EQ(registry.Rename(RefKind::BookmarkName, "intro", "Overview", dom.Root(), nullptr), size_t{3});
  std::string xml = dom.Serialize();
  CHECK(xml.find("w:name=\"Overview\"") != std::string::npos);
  CHECK(xml.find(" REF Overview \\h ") != std::string::npos);
  CHECK(xml.find("w:anchor=\"Overview\"") != std::string::npos);
  CHECK_EQ(registry.Rename(RefKind::VmlShapeType, "_x0000_t75", "_x0000_t76", dom.Root(), nullptr), size_t{2});
  CHECK(dom.Serialize().find("type=\"#_x0000_t76\"") != std::string::npos);
  CHECK_EQ(registry.Rename(RefKind::BookmarkId, "0", "4", dom.Root(), nullptr), size_t{2});
  CHECK_EQ(registry.Rename(RefKind::Field, "begin", "x", dom.Root(), nullptr), size_t{0});  // fields have no value

  // Sheet names in formulas, defined names and links, quoted as needed; cells aren't renamed.
  XmlDom book;
  CHECK(book.Parse("<workbook xmlns=\"" X_NS "\"><sheets><sheet name=\"Sales\" sheetId=\"1\"/></sheets>"
                   "<definedName name=\"N\">'Sales'!$A$1:$A$3</definedName><c r=\"A1\"><f>SUM(Sales!A1:A3)+Other!B1</f></c>"
                   "<hyperlink ref=\"A1\" location=\"sales!A1\"/></workbook>")
            .Ok());
  CHECK_EQ(registry.Rename(RefKind::SheetName, "Sales", "Q1 sales", book.Root(), nullptr), size_t{4});
  std::string out = book.Serialize();
  CHECK(out.find("name=\"Q1 sales\"") != std::string::npos);
  CHECK(out.find(">'Q1 sales'!$A$1:$A$3<") != std::string::npos);
  CHECK(out.find("SUM('Q1 sales'!A1:A3)+Other!B1") != std::string::npos);
  CHECK(out.find("location=\"'Q1 sales'!A1\"") != std::string::npos);
  CHECK_EQ(registry.Rename(RefKind::CellRange, "A1", "B1", book.Root(), nullptr), size_t{0});

  // Kept content: a fragment holding a bookmark is renamed too.
  RoundTripStore store;
  store.Add(1, {Slot::After, {}, "<w:bookmarkStart xmlns:w=\"" W_NS "\" w:id=\"8\" w:name=\"Kept\"/>"});
  CHECK_EQ(registry.Rename(RefKind::BookmarkName, "Kept", "Held", dom.Root(), &store), size_t{1});
  CHECK(store.FragmentsOf(1, Slot::After)[0]->xml.find("w:name=\"Held\"") != std::string::npos);
}

TEST(References_StrictNamespacesMatch) {
  auto registry = ReferenceRegistry::Default();
  XmlDom dom;
  CHECK(dom.Parse("<w:document xmlns:w=\"http://purl.oclc.org/ooxml/wordprocessingml/main\" "
                  "xmlns:r=\"http://purl.oclc.org/ooxml/officeDocument/relationships\"><w:body><w:p>"
                  "<w:bookmarkStart w:id=\"1\" w:name=\"S\"/><w:hyperlink r:id=\"rId3\"/></w:p></w:body></w:document>")
            .Ok());
  auto refs = registry.Find(dom.Root());
  CHECK_EQ(Count(refs, RefKind::BookmarkId), size_t{1});
  CHECK_EQ(Count(refs, RefKind::RelationshipId), size_t{1});
}

TEST(References_Validators) {
  CHECK(IsLongHexId("1A2B3C4D") && IsLongHexId("7fffffff") && !IsLongHexId("80000000") && !IsLongHexId("1A2B3C"));
  CHECK(IsUnsignedId("0") && IsUnsignedId("4294967295") && !IsUnsignedId("4294967296") && !IsUnsignedId("-1"));
  CHECK(IsSlideId("256") && IsSlideId("2147483647") && !IsSlideId("255") && !IsSlideId("2147483648"));
  CHECK(IsSlideMasterId("2147483648") && !IsSlideMasterId("2147483647"));
}

// ---------- Formulas ----------

TEST(Formula_SheetTokens) {
  auto names = [](std::string_view f) {
    std::vector<std::string> out;
    ForEachSheetToken(f, [&](const SheetToken& t) {
      out.push_back(t.first);
      if (!t.last.empty()) out.push_back(t.last);
    });
    return out;
  };
  CHECK(names("SUM(Sales!B2:B7)") == std::vector<std::string>{"Sales"});
  CHECK(names("'My sheet'!A1+'It''s'!B2") == (std::vector<std::string>{"My sheet", "It's"}));
  CHECK(names("SUM(Jan:Mar!A1)") == (std::vector<std::string>{"Jan", "Mar"}));
  CHECK(names("'Jan 1:Mar 1'!A1") == (std::vector<std::string>{"Jan 1", "Mar 1"}));
  CHECK(names("[1]Sheet1!A1+'[2]Other'!B1").empty());  // other workbooks
  CHECK(names("\"Sales!A1\"&#REF!").empty());          // a string and an error value
  CHECK(names("Données!A1") == std::vector<std::string>{"Données"});
  std::vector<std::string> cells;
  ForEachCellToken("SUM($A$1:B2)+LOG10(4)+Sales!C3", [&](std::string_view c) { cells.emplace_back(c); });
  CHECK(cells == (std::vector<std::string>{"$A$1", "B2", "C3"}));
}

TEST(Formula_RenameAndQuote) {
  CHECK(QuoteSheetName("Sales") == "Sales");
  CHECK(QuoteSheetName("Q1 sales") == "'Q1 sales'");
  CHECK(QuoteSheetName("2024") == "'2024'");
  CHECK(QuoteSheetName("A1") == "'A1'");
  CHECK(QuoteSheetName("R1C1") == "'R1C1'");
  CHECK(QuoteSheetName("It's") == "'It''s'");
  bool changed = false;
  CHECK(RenameSheetInFormula("Sales!A1+'sales'!B2+Other!C3", "SALES", "New", &changed) == "New!A1+New!B2+Other!C3");
  CHECK(changed);
  CHECK(RenameSheetInFormula("SUM(Jan:Mar!A1)", "Mar", "Q1 end") == "SUM('Jan:Q1 end'!A1)");
  CHECK(RenameSheetInFormula("\"Sales!A1\"", "Sales", "New", &changed) == "\"Sales!A1\"");
  CHECK(!changed);
}

TEST(Formula_CellRanges) {
  uint32_t col = 0, row = 0;
  CHECK(ParseCellRef("XFD1048576", col, row) && col == kMaxColumn && row == kMaxRow);
  CHECK(ParseCellRef("$b$3", col, row) && col == 2 && row == 3);
  CHECK(!ParseCellRef("XFE1", col, row) && !ParseCellRef("A0", col, row) && !ParseCellRef("A1048577", col, row));
  CHECK(IsCellRange("A1") && IsCellRange("A1:B2") && IsCellRange("$A:$C") && IsCellRange("1:3"));
  CHECK(!IsCellRange("A") && !IsCellRange("A1:") && !IsCellRange("A:1") && !IsCellRange("Sheet1!A1"));
  CHECK(CellName(1, 1) == "A1" && CellName(28, 5) == "AB5" && CellName(kMaxColumn, 2) == "XFD2");
}

// ---------- Extensions ----------

TEST(References_UnknownExtensionsBlockEdits) {
  auto registry = ReferenceRegistry::Default();
  CHECK(registry.ClassifyExtension("{78c0d931-6437-407d-a8ee-f0aad7539e65}") == ExtensionClass::Registry);
  CHECK(registry.ClassifyExtension("{C3380CC4-5D6E-409C-BE32-E72D297353CC}") == ExtensionClass::ReferenceFree);
  CHECK(registry.ClassifyExtension("{00000000-0000-0000-0000-000000000000}") == ExtensionClass::Unknown);
  XmlDom dom;
  CHECK(dom.Parse("<worksheet xmlns=\"" X_NS "\" xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\">"
                  "<a:ext cx=\"1\" cy=\"2\"/><extLst>"
                  "<ext uri=\"{78C0D931-6437-407D-A8EE-F0AAD7539E65}\"><x14 xmlns=\"urn:x14\">Sales!A1</x14></ext>"
                  "<ext uri=\"{11111111-2222-3333-4444-555555555555}\"><thing xmlns=\"urn:x99\" target=\"Sales!A1\" "
                  "id=\"rId10\"/></ext></extLst></worksheet>")
            .Ok());
  auto unknown = registry.UnknownExtensions(dom.Root());
  CHECK(unknown.size() == 1 && unknown[0] == "{11111111-2222-3333-4444-555555555555}");
  auto strand = [&](RefKind kind, std::vector<std::string> values) {
    return registry.WouldStrand(dom.Root(), {{kind, std::move(values)}});
  };
  auto renameSheet = strand(RefKind::SheetName, {"sales"});
  CHECK(renameSheet.size() == 1 && renameSheet[0].value == "sales" && renameSheet[0].kind == RefKind::SheetName);
  CHECK(strand(RefKind::RelationshipId, {"rId1"}).empty());  // rId10 isn't rId1
  CHECK_EQ(strand(RefKind::RelationshipId, {"rId10"}).size(), size_t{1});
  CHECK_EQ(strand(RefKind::CellRange, {}).size(), size_t{1});  // inserting a row shifts every cell
  CHECK(strand(RefKind::SlideId, {"256"}).empty());
}

// ---------- Pasting ----------

TEST(References_IdRenewerGivesPastedContentNewIds) {
  auto registry = ReferenceRegistry::Default();
  XmlDom target, pasted;
  CHECK(target.Parse(std::string(kWordOpen) +
                     "<w:p w14:paraId=\"0000000A\"><w:bookmarkStart w:id=\"0\" w:name=\"Intro\"/><w:bookmarkEnd w:id=\"0\"/>"
                     "<w:commentRangeStart w:id=\"0\"/><w:commentRangeEnd w:id=\"0\"/><w:r><w:drawing><wp:inline>"
                     "<wp:docPr id=\"1\" name=\"A\"/></wp:inline></w:drawing></w:r></w:p>" +
                     kWordClose)
            .Ok());
  const std::string pastedXml =
      std::string(kWordOpen) +
      "<w:p w14:paraId=\"0000000A\"><w:bookmarkStart w:id=\"0\" w:name=\"intro\"/><w:r><w:t>x</w:t></w:r>"
      "<w:bookmarkEnd w:id=\"0\"/><w:hyperlink w:anchor=\"Intro\"/><w:hyperlink w:anchor=\"Elsewhere\"/>"
      "<w:r><w:commentReference w:id=\"0\"/></w:r><w:r><w:drawing><wp:inline><wp:docPr id=\"1\" name=\"B\"/></wp:inline>"
      "</w:drawing></w:r></w:p><w:p w14:paraId=\"0000000B\"/>" +
      kWordClose;
  CHECK(pasted.Parse(pastedXml).Ok());
  IdRenewer renewer(registry);
  renewer.Take(target.Root());
  auto renamed = renewer.Renew(pasted.Root());
  CHECK_EQ(renamed.size(), size_t{5});  // paragraph id, bookmark id and name, comment, drawing
  std::string out = pasted.Serialize();
  CHECK(out.find("w14:paraId=\"0000000A\"") == std::string::npos);
  CHECK(out.find("w14:paraId=\"0000000B\"") != std::string::npos);  // no collision: kept
  CHECK(out.find("<w:bookmarkStart w:id=\"1\" w:name=\"intro_1\"/>") != std::string::npos);
  CHECK(out.find("<w:bookmarkEnd w:id=\"1\"/>") != std::string::npos);
  CHECK(out.find("w:anchor=\"intro_1\"") != std::string::npos);    // pointed at the pasted bookmark
  CHECK(out.find("w:anchor=\"Elsewhere\"") != std::string::npos);  // pointed at one that stays behind
  CHECK(out.find("<w:commentReference w:id=\"1\"/>") != std::string::npos);
  CHECK(out.find("<wp:docPr id=\"2\"") != std::string::npos);
  for (const auto& r : renamed)
    if (r.kind == RefKind::CommentId) CHECK(r.from == "0" && r.to == "1");  // the caller copies the comment
  // Pasting the same content again gives it yet other ids.
  XmlDom again;
  CHECK(again.Parse(pastedXml).Ok());
  renewer.Renew(again.Root());
  std::string second = again.Serialize();
  CHECK(second.find("w:name=\"intro_2\"") != std::string::npos && second.find("<wp:docPr id=\"3\"") != std::string::npos);
  CHECK(second.find("w14:paraId=\"0000000B\"") == std::string::npos);
}

// ---------- fwtool check ----------

TEST(Check_CleanWordPackagePasses) {
  auto rep = CheckParts(Word(kSampleBody, Rel("rId9", "comments", "comments.xml") + Rel("rId8", "footnotes", "footnotes.xml") +
                                              Rel("rId7", "styles", "styles.xml"),
                             {{"word/comments.xml", "<w:comments xmlns:w=\"" W_NS "\"><w:comment w:id=\"3\"><w:p/></w:comment></w:comments>"},
                              {"word/footnotes.xml", "<w:footnotes xmlns:w=\"" W_NS "\"><w:footnote w:id=\"2\"><w:p/></w:footnote></w:footnotes>"},
                              {"word/styles.xml", "<w:styles xmlns:w=\"" W_NS "\"><w:style w:styleId=\"Heading1\"/></w:styles>"}}));
  for (const auto& e : rep.errors) std::printf("    unexpected: %s\n", e.c_str());
  CHECK(rep.Ok());
  CHECK(rep.warnings.empty());
  CHECK_EQ(rep.pairs, size_t{4});  // a bookmark, a comment range, a permission range, a field
  CHECK(rep.references >= 6);
}

TEST(Check_RejectsBrokenWordContent) {
  auto errors = [](const std::string& body, const std::string& rels = {}, const Parts& extra = {}) {
    return CheckParts(Word(body, rels, extra)).errors;
  };
  CHECK(Has(errors("<w:p><w:hyperlink r:id=\"rId5\"/></w:p>"), "r:id=\"rId5\" doesn't match any of its relationships"));
  CHECK(Has(errors("<w:p w14:paraId=\"00000001\"/><w:p w14:paraId=\"00000001\"/>"), "paragraph id \"00000001\" is used twice"));
  CHECK(Has(errors("<w:p w14:paraId=\"80000000\"/>"), "paragraph id \"80000000\" isn't valid"));
  CHECK(Has(errors("<w:p><w:bookmarkStart w:id=\"5\" w:name=\"B\"/></w:p>"), "bookmark \"5\" starts but doesn't end"));
  CHECK(Has(errors("<w:p><w:bookmarkEnd w:id=\"5\"/></w:p>"), "bookmark \"5\" ends but doesn't start"));
  CHECK(Has(errors("<w:p><w:bookmarkEnd w:id=\"5\"/><w:bookmarkStart w:id=\"5\" w:name=\"B\"/></w:p>"),
            "bookmark \"5\" ends before it starts"));
  CHECK(Has(errors("<w:p><w:bookmarkStart w:id=\"1\" w:name=\"B\"/><w:bookmarkEnd w:id=\"1\"/>"
                   "<w:bookmarkStart w:id=\"2\" w:name=\"b\"/><w:bookmarkEnd w:id=\"2\"/></w:p>"),
            "bookmark \"B\" is used twice"));  // names compare without case
  CHECK(Has(errors("<w:p><w:r><w:fldChar w:fldCharType=\"separate\"/></w:r></w:p>"), "field separator is outside any field"));
  CHECK(Has(errors("<w:p><w:r><w:fldChar w:fldCharType=\"end\"/></w:r></w:p>"), "field ends but doesn't begin"));
  CHECK(Has(errors("<w:p><w:r><w:fldChar w:fldCharType=\"begin\"/><w:fldChar w:fldCharType=\"begin\"/></w:r></w:p>"),
            "2 fields begin but don't end"));
  CHECK(Has(errors("<w:p><w:r><w:fldChar w:fldCharType=\"begin\"/><w:fldChar w:fldCharType=\"separate\"/>"
                   "<w:fldChar w:fldCharType=\"separate\"/><w:fldChar w:fldCharType=\"end\"/></w:r></w:p>"),
            "field has two separators"));
  CHECK(Has(errors("<w:p><w:r><w:footnoteReference w:id=\"2\"/></w:r></w:p>"), "names footnote \"2\", which doesn't exist"));
  CHECK(Has(errors("<w:p><w:commentRangeStart w:id=\"4\"/><w:commentRangeEnd w:id=\"4\"/></w:p>"), "names comment \"4\""));
  // Drawing ids are unique across the document's parts.
  CHECK(Has(errors("<w:p><w:r><w:drawing><wp:inline><wp:docPr id=\"1\" name=\"A\"/></wp:inline></w:drawing></w:r></w:p>",
                   Rel("rId2", "header", "header1.xml"),
                   {{"word/header1.xml", std::string("<w:hdr xmlns:w=\"" W_NS "\" xmlns:wp=\"http://schemas.openxmlformats.org/"
                                                     "drawingml/2006/wordprocessingDrawing\"><w:p><w:r><w:drawing><wp:inline>"
                                                     "<wp:docPr id=\"1\" name=\"B\"/></wp:inline></w:drawing></w:r></w:p></w:hdr>")}}),
            "drawing object id \"1\" is used twice"));
}

// A field must begin, separate and end in the same hyperlink, custom XML element or simple field,
// and not separate or end inside a content control it didn't begin in: Word refuses or repairs
// such files, and the validator doesn't notice. Each case was opened in Word (D2's torture test,
// finding 6: a paragraph split inside a table of contents entry's PAGEREF field).
TEST(Check_FieldsStayInTheirContainers) {
  const std::string begin = "<w:r><w:fldChar w:fldCharType=\"begin\"/></w:r><w:r><w:instrText> PAGEREF B \\h </w:instrText></w:r>";
  const std::string sep = "<w:r><w:fldChar w:fldCharType=\"separate\"/></w:r><w:r><w:t>1</w:t></w:r>";
  const std::string end = "<w:r><w:fldChar w:fldCharType=\"end\"/></w:r>";
  auto link = [](const std::string& inner) { return "<w:hyperlink w:anchor=\"B\">" + inner + "</w:hyperlink>"; };
  auto sdt = [](const std::string& inner) { return "<w:sdt><w:sdtPr/><w:sdtContent>" + inner + "</w:sdtContent></w:sdt>"; };
  auto check = [](const std::string& body) {
    return CheckParts(Word("<w:p><w:bookmarkStart w:id=\"0\" w:name=\"B\"/><w:bookmarkEnd w:id=\"0\"/></w:p>" + body));
  };
  // Word opens these.
  CHECK(check("<w:p>" + link(begin + sep + end) + "</w:p>").Ok());
  CHECK(check("<w:p>" + begin + sep + "</w:p><w:p>" + end + "</w:p>").Ok());  // a table of contents spans paragraphs
  CHECK(check("<w:p>" + begin + sep + "</w:p><w:tbl><w:tr><w:tc><w:p>" + end + "</w:p></w:tc></w:tr></w:tbl>").Ok());
  CHECK(check("<w:p><w:ins w:id=\"7\" w:author=\"A\">" + begin + "</w:ins>" + sep + end + "</w:p>").Ok());
  CHECK(check("<w:p><w:fldSimple w:instr=\" PAGE \">" + begin + sep + end + "</w:fldSimple></w:p>").Ok());
  // Word refuses these: "Word experienced an error trying to open the file".
  CHECK(Has(check("<w:p>" + link(begin) + "</w:p><w:p>" + link(sep + end) + "</w:p>").errors,
            "a field begins inside a hyperlink but its separator is outside it"));
  CHECK(Has(check("<w:p>" + link(begin) + sep + end + "</w:p>").errors, "a field begins inside a hyperlink"));
  CHECK(Has(check("<w:p>" + begin + link(sep + end) + "</w:p>").errors,
            "a field's separator is inside a hyperlink the field doesn't begin in"));
  CHECK(Has(check("<w:p>" + link(begin + sep) + link(end) + "</w:p>").errors, "but its end is outside it"));
  CHECK(Has(check("<w:p><w:customXml w:element=\"e\">" + begin + "</w:customXml>" + sep + end + "</w:p>").errors,
            "a field begins inside a custom XML element"));
  CHECK(Has(check("<w:p>" + begin + sep + "</w:p><w:customXml w:element=\"b\"><w:p>" + end + "</w:p></w:customXml>").errors,
            "a field's end is inside a custom XML element"));
  CHECK(Has(check("<w:p><w:fldSimple w:instr=\" PAGE \">" + begin + "</w:fldSimple>" + sep + end + "</w:p>").errors,
            "a field begins inside a simple field"));
  // Word repairs these: a field that separates or ends inside a content control it didn't begin in.
  CHECK(Has(check("<w:p>" + begin + sdt(sep + end) + "</w:p>").errors, "is inside a content control the field doesn't begin in"));
  CHECK(Has(check("<w:p>" + begin + sep + "</w:p>" + sdt("<w:p>" + end + "</w:p>")).errors,
            "a field's end is inside a content control the field doesn't begin in"));
  // Leaving a content control Word tolerates: a warning.
  auto leaving = check("<w:p>" + sdt(begin) + sep + end + "</w:p>");
  CHECK(leaving.Ok());
  CHECK(Has(leaving.warnings, "a field begins inside a content control but its separator is outside it"));
}

// Tables as Word can open them: a row needs a cell, and a cell must end with a paragraph. Word
// refuses or repairs these, and the validator accepts them (opened in Word, D2's torture test).
TEST(Check_WordTablesWordCanOpen) {
  auto check = [](const std::string& rows) { return CheckParts(Word("<w:tbl><w:tblGrid><w:gridCol/></w:tblGrid>" + rows + "</w:tbl><w:p/>")); };
  auto cell = [](const std::string& inner) { return "<w:tr><w:tc><w:tcPr/>" + inner + "</w:tc></w:tr>"; };
  // Word opens these.
  CHECK(check("").Ok());  // a table without rows
  CHECK(check(cell("<w:p/>")).Ok());
  CHECK(check(cell("<w:p/><w:bookmarkStart w:id=\"1\" w:name=\"B\"/><w:bookmarkEnd w:id=\"1\"/>")).Ok());
  CHECK(check(cell("<w:sdt><w:sdtPr/><w:sdtContent><w:p/></w:sdtContent></w:sdt>")).Ok());
  CHECK(check(cell("<w:tbl><w:tr><w:tc><w:p/></w:tc></w:tr></w:tbl><w:p/>")).Ok());
  CHECK(check("<w:tr><w:sdt><w:sdtPr/><w:sdtContent><w:tc><w:p/></w:tc></w:sdtContent></w:sdt></w:tr>").Ok());
  // Word refuses or repairs these.
  CHECK(Has(check("<w:tr></w:tr>").errors, "a table row has no cells"));
  CHECK(Has(check(cell("")).errors, "a table cell has no paragraph"));
  CHECK(Has(check(cell("<w:tbl><w:tr><w:tc><w:p/></w:tc></w:tr></w:tbl>")).errors,
            "a table cell ends with a table instead of a paragraph"));
  CHECK(Has(check(cell("<w:sdt><w:sdtPr/><w:sdtContent><w:tbl><w:tr><w:tc><w:p/></w:tc></w:tr></w:tbl></w:sdtContent></w:sdt>")).errors,
            "a table cell ends with a table instead of a paragraph"));
}

TEST(Check_ScopesAlternatesAndWarnings) {
  // A glossary is a document of its own: its paragraph ids may repeat the main document's.
  const std::string glossary =
      "<w:glossaryDocument xmlns:w=\"" W_NS "\" xmlns:w14=\"http://schemas.microsoft.com/office/word/2010/wordml\">"
      "<w:docParts><w:docPart><w:docPartBody><w:p w14:paraId=\"00000001\"/></w:docPartBody></w:docPart></w:docParts>"
      "</w:glossaryDocument>";
  auto rep = CheckParts(Word("<w:p w14:paraId=\"00000001\"/>", Rel("rId3", "glossaryDocument", "glossary/document.xml"),
                             {{"word/glossary/document.xml", glossary}}));
  CHECK(rep.Ok());
  // The branches of an AlternateContent block repeat the same content.
  rep = CheckParts(Word("<w:p><w:r><mc:AlternateContent><mc:Choice Requires=\"wps\"><w:drawing><wp:inline>"
                        "<wp:docPr id=\"3\" name=\"A\"/></wp:inline></w:drawing><w:bookmarkStart w:id=\"1\" w:name=\"M\"/>"
                        "<w:bookmarkEnd w:id=\"1\"/></mc:Choice><mc:Fallback><w:drawing><wp:inline><wp:docPr id=\"3\" name=\"A\"/>"
                        "</wp:inline></w:drawing><w:bookmarkStart w:id=\"1\" w:name=\"M\"/><w:bookmarkEnd w:id=\"1\"/>"
                        "</mc:Fallback></mc:AlternateContent></w:r></w:p>"));
  CHECK(rep.Ok());
  // What Word tolerates is a warning.
  rep = CheckParts(Word("<w:p><w:pPr><w:pStyle w:val=\"Missing\"/></w:pPr><w:hyperlink w:anchor=\"Nowhere\"/></w:p>"));
  CHECK(rep.Ok());
  CHECK(Has(rep.warnings, "names style \"Missing\"") && Has(rep.warnings, "names bookmark \"Nowhere\""));
}

TEST(Check_ExcelCalcChainCellsAndNotes) {
  auto withChain = [](const std::string& chain) {
    return Excel("<sheetData><row r=\"1\"><c r=\"A1\"><v>1</v></c><c r=\"B1\"><f>A1*2</f><v>2</v></c></row></sheetData>",
                 Rel("rId2", "calcChain", "calcChain.xml"),
                 {{"xl/calcChain.xml", "<calcChain xmlns=\"" X_NS "\">" + chain + "</calcChain>"}});
  };
  auto rep = CheckParts(withChain("<c r=\"B1\" i=\"1\"/>"));
  CHECK(rep.Ok());
  CHECK_EQ(rep.derived, size_t{1});
  CHECK(Has(CheckParts(withChain("<c r=\"B1\" i=\"1\"/><c r=\"A1\"/>")).errors,
            "the calculation chain is stale: Data!A1 has no formula"));
  CHECK(Has(CheckParts(withChain("<c r=\"B1\" i=\"9\"/>")).errors, "names sheet id \"9\", which doesn't exist"));

  CHECK(Has(CheckParts(Excel("<sheetData/><mergeCells><mergeCell ref=\"A1:XFE2\"/></mergeCells>")).errors,
            "mergeCell ref=\"A1:XFE2\" isn't a cell reference"));
  CHECK(Has(CheckParts(Excel("<sheetData><row r=\"1\"><c r=\"A1\"><f>Nowhere!B2</f></c></row></sheetData>")).errors,
            "names sheet \"Nowhere\", which doesn't exist"));

  // Each comment needs a VML note on its cell (0-based row and column), and each note a comment.
  const std::string vml =
      "<xml xmlns:v=\"urn:schemas-microsoft-com:vml\" xmlns:x=\"urn:schemas-microsoft-com:office:excel\">"
      "<v:shape id=\"_x0000_s1025\"><x:ClientData ObjectType=\"Note\"><x:Row>1</x:Row><x:Column>0</x:Column>"
      "</x:ClientData></v:shape><v:shape id=\"_x0000_s1026\"><x:ClientData ObjectType=\"Note\"><x:Row>4</x:Row>"
      "<x:Column>2</x:Column></x:ClientData></v:shape></xml>";
  const std::string comments = "<comments xmlns=\"" X_NS "\"><commentList><comment ref=\"A2\"/><comment ref=\"B9\"/>"
                               "</commentList></comments>";
  Parts notes = Excel("<sheetData/><legacyDrawing r:id=\"rId2\"/>", {},
                      {{"xl/worksheets/_rels/sheet1.xml.rels",
                        Rels(Rel("rId1", "comments", "../comments1.xml") + Rel("rId2", "vmlDrawing", "../drawings/vml1.vml"))},
                       {"xl/comments1.xml", comments},
                       {"xl/drawings/vml1.vml", vml}});
  rep = CheckParts(notes);
  CHECK(rep.Ok());
  CHECK(Has(rep.warnings, "the comment on B9 has no note"));
  CHECK(Has(rep.warnings, "a note on C5 has no comment"));
}

TEST(Check_ChartCachesMatchTheirWorkbook) {
  auto chart = [](const char* cached, const char* externalData) {
    return std::string("<c:chartSpace xmlns:c=\"http://schemas.openxmlformats.org/drawingml/2006/chart\" xmlns:r=\"" R_NS
                       "\"><c:chart><c:ser><c:val><c:numRef><c:f>'Data'!$B$1:$B$2</c:f><c:numCache><c:ptCount val=\"2\"/>"
                       "<c:pt idx=\"0\"><c:v>2.0</c:v></c:pt><c:pt idx=\"1\"><c:v>") +
           cached + "</c:v></c:pt></c:numCache></c:numRef></c:val></c:ser></c:chart>" + externalData + "</c:chartSpace>";
  };
  const char* sheet = "<sheetData><row r=\"1\"><c r=\"B1\"><v>2</v></c></row><row r=\"2\"><c r=\"B2\"><v>5</v></c></row></sheetData>";
  // In a workbook, the chart's data is in the workbook itself.
  auto rep = CheckParts(Excel(sheet, {}, {{"xl/charts/chart1.xml", chart("5", "")}}));
  CHECK(rep.Ok() && rep.warnings.empty() && rep.derived == 2);
  rep = CheckParts(Excel(sheet, {}, {{"xl/charts/chart1.xml", chart("6", "")}}));
  CHECK(Has(rep.warnings, "the chart's cached value \"6\" for Data!B2 doesn't match the workbook (\"5\")"));

  // In a presentation, it is in the embedded workbook, which is checked as a package too.
  auto inner = test::MakeZip(Excel(sheet));
  std::string embedded(inner.begin(), inner.end());
  auto deck = [&](const char* cached) {
    return Parts{
        {"[Content_Types].xml",
         Types("/ppt/presentation.xml", "application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml")},
        {"_rels/.rels", Rels(Rel("rId1", "officeDocument", "ppt/presentation.xml"))},
        {"ppt/presentation.xml", "<p:presentation xmlns:p=\"" P_NS "\"/>"},
        {"ppt/charts/chart1.xml", chart(cached, "<c:externalData r:id=\"rId1\"/>")},
        {"ppt/charts/_rels/chart1.xml.rels", Rels(Rel("rId1", "package", "../embeddings/book.xlsx"))},
        {"ppt/embeddings/book.xlsx", embedded},
    };
  };
  rep = CheckParts(deck("5"));
  CHECK(rep.Ok() && rep.warnings.empty());
  CHECK(rep.embedded == 1 && rep.derived == 2);
  rep = CheckParts(deck("7"));
  CHECK(Has(rep.warnings, "doesn't match its embedded workbook"));
}

TEST(Check_PowerPointShapeIdsAndAnimations) {
  auto slide = [](const char* shapes, const char* timing) {
    return std::string("<p:sld xmlns:p=\"" P_NS "\"><p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/></p:nvGrpSpPr>") +
           shapes + "</p:spTree></p:cSld>" + timing + "</p:sld>";
  };
  auto deck = [&](const std::string& slide1, const std::string& slide2) {
    return Parts{
        {"[Content_Types].xml",
         Types("/ppt/presentation.xml", "application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml")},
        {"_rels/.rels", Rels(Rel("rId1", "officeDocument", "ppt/presentation.xml"))},
        {"ppt/presentation.xml", "<p:presentation xmlns:p=\"" P_NS "\" xmlns:r=\"" R_NS "\"><p:sldIdLst>"
                                 "<p:sldId id=\"256\" r:id=\"rId2\"/><p:sldId id=\"257\" r:id=\"rId3\"/></p:sldIdLst></p:presentation>"},
        {"ppt/_rels/presentation.xml.rels", Rels(Rel("rId2", "slide", "slides/slide1.xml") + Rel("rId3", "slide", "slides/slide2.xml"))},
        {"ppt/slides/slide1.xml", slide1},
        {"ppt/slides/slide2.xml", slide2},
    };
  };
  const char* shape = "<p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"A\"/></p:nvSpPr></p:sp>";
  const char* animate = "<p:timing><p:tnLst><p:par><p:spTgt spid=\"2\"/></p:par></p:tnLst></p:timing>";
  auto rep = CheckParts(deck(slide(shape, animate), slide(shape, "")));  // the same id on two slides is fine
  CHECK(rep.Ok());
  rep = CheckParts(deck(slide(shape, "<p:timing><p:spTgt spid=\"5\"/></p:timing>"), slide(shape, "")));
  CHECK(Has(rep.errors, "p:spTgt spid=\"5\" names shape id \"5\", which doesn't exist"));
  std::string twice = std::string(shape) + shape;
  CHECK(Has(CheckParts(deck(slide(twice.c_str(), ""), slide(shape, ""))).errors, "shape id \"2\" is used twice"));
}

TEST(Derived_ListCoversEveryFormat) {
  bool calcChain = false, charts = false, fields = false;
  for (const auto* d : DerivedDataFor(DocFormat::Excel)) {
    calcChain |= std::string_view(d->what).find("calculation chain") != std::string_view::npos && d->onSave == OnSave::Drop && d->checked;
    charts |= std::string_view(d->what).find("chart caches") != std::string_view::npos && d->onSave == OnSave::Regenerate;
  }
  for (const auto* d : DerivedDataFor(DocFormat::Word)) fields |= std::string_view(d->what).find("field results") != std::string_view::npos;
  CHECK(calcChain && charts && fields);
  CHECK(!DerivedDataFor(DocFormat::PowerPoint).empty());
  for (const auto* d : DerivedDataFor(DocFormat::Word)) CHECK(d->format == DocFormat::Any || d->format == DocFormat::Word);
}
