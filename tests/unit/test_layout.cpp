// Pagination (src/doc/layout): documents built from XML, laid out with the PC's fonts. The checks
// are about where things go relative to each other and to the page, not exact positions, so they
// hold whatever the font metrics.
#include "TestHarness.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "PkgTestUtil.h"
#include "doc/docx/Docx.h"
#include "doc/edit/Clip.h"
#include "doc/edit/Editor.h"
#include "doc/edit/Page.h"
#include "doc/layout/Layout.h"
#include "text/Fonts.h"

// Stops the test when a check it depends on fails.
#define REQUIRE(cond)   do { if (!(cond)) { test::Fail(__FILE__, __LINE__, #cond); return; } } while (0)

using namespace fw::doc;
using fw::Twips;
using fw::doc::layout::Layout;
using fw::doc::layout::TextPos;
using fw::doc::layout::LineBox;
using fw::doc::layout::Page;
using fw::doc::layout::SubstituteFamily;

namespace {

constexpr const char* kW = "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"";
constexpr const char* kR = "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"";

// A Word document from its body and, optionally, its settings and footnotes.
struct Doc {
  std::unique_ptr<Document> doc;
  std::unique_ptr<Layout> layout;
  const Page& page(size_t i) const { return layout->GetPage(i); }
  size_t pages() const { return layout->PageCount(); }
};

fw::text::Fonts& Fonts() {
  static fw::text::Fonts fonts;
  return fonts;
}

Doc Make(const std::string& body, const std::string& settings = "", const std::string& footnotes = "", bool all = true,
         const std::string& fonts = "", const std::string& header = "") {
  const std::string wml = "application/vnd.openxmlformats-officedocument.wordprocessingml.";
  const std::string rel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
  const std::string head = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>";
  std::string types =
      "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
      "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
      "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
      "<Override PartName=\"/word/document.xml\" ContentType=\"" + wml + "document.main+xml\"/>";
  std::string rels = "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
  std::vector<std::pair<std::string, std::string>> entries;
  if (!settings.empty()) {
    types += "<Override PartName=\"/word/settings.xml\" ContentType=\"" + wml + "settings+xml\"/>";
    rels += "<Relationship Id=\"rId1\" Type=\"" + rel + "settings\" Target=\"settings.xml\"/>";
    entries.push_back({"word/settings.xml", head + "<w:settings " + kW + ">" + settings + "</w:settings>"});
  }
  if (!fonts.empty()) {
    types += "<Override PartName=\"/word/fontTable.xml\" ContentType=\"" + wml + "fontTable+xml\"/>";
    rels += "<Relationship Id=\"rId3\" Type=\"" + rel + "fontTable\" Target=\"fontTable.xml\"/>";
    entries.push_back({"word/fontTable.xml", head + "<w:fonts " + kW + ">" + fonts + "</w:fonts>"});
  }
  if (!header.empty()) {  // the sections refer to it as rIdH
    types += "<Override PartName=\"/word/header1.xml\" ContentType=\"" + wml + "header+xml\"/>";
    rels += "<Relationship Id=\"rIdH\" Type=\"" + rel + "header\" Target=\"header1.xml\"/>";
    entries.push_back({"word/header1.xml", head + "<w:hdr " + kW + ">" + header + "</w:hdr>"});
  }
  if (!footnotes.empty()) {
    types += "<Override PartName=\"/word/footnotes.xml\" ContentType=\"" + wml + "footnotes+xml\"/>";
    rels += "<Relationship Id=\"rId2\" Type=\"" + rel + "footnotes\" Target=\"footnotes.xml\"/>";
    entries.push_back({"word/footnotes.xml", head + "<w:footnotes " + kW + ">" + footnotes + "</w:footnotes>"});
  }
  types += "</Types>";
  rels += "</Relationships>";
  entries.insert(entries.begin(),
                 {{"[Content_Types].xml", types},
                  {"_rels/.rels", "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                                  "<Relationship Id=\"rId1\" Type=\"" + rel + "officeDocument\" Target=\"word/document.xml\"/>"
                                  "</Relationships>"},
                  {"word/document.xml", head + "<w:document " + kW + " " + kR + "><w:body>" + body + "</w:body></w:document>"},
                  {"word/_rels/document.xml.rels", rels}});
  Doc d;
  fw::pkg::Status st;
  auto pkg = test::OpenBytes(test::MakeZip(entries), st);
  if (pkg) d.doc = docx::Read(std::move(pkg), st);
  if (!d.doc) {
    std::printf("  load failed: %s\n", st.message.c_str());
    return d;
  }
  d.layout = std::make_unique<Layout>(*d.doc, Fonts());
  if (all) d.layout->LayOutAll();
  return d;
}

std::string P(const std::string& text, const std::string& pPr = "") {
  return "<w:p><w:pPr>" + pPr + "</w:pPr><w:r><w:t xml:space=\"preserve\">" + text + "</w:t></w:r></w:p>";
}

std::string Words(int n) {
  std::string s;
  for (int i = 0; i < n; ++i) s += "word" + std::to_string(i % 10) + " ";
  return s;
}

// A small page, so a few lines fill it: 200 x 200 pt, 20 pt margins.
std::string SmallPage(const std::string& extra = "") {
  return "<w:sectPr>" + extra +
         "<w:pgSz w:w=\"4000\" w:h=\"4000\"/><w:pgMar w:top=\"400\" w:right=\"400\" w:bottom=\"400\" w:left=\"400\" "
         "w:header=\"200\" w:footer=\"200\" w:gutter=\"0\"/></w:sectPr>";
}

const LineBox* Find(const Doc& d, const std::u16string& text, size_t* page = nullptr) {
  for (size_t p = 0; p < d.pages(); ++p)
    for (const LineBox& l : d.page(p).lines)
      if (l.text.find(text) != std::u16string::npos) {
        if (page) *page = p;
        return &l;
      }
  return nullptr;
}

}  // namespace

TEST(Layout_FillsPagesWithinTheMargins) {
  std::string body;
  for (int i = 0; i < 30; ++i) body += P("Line " + std::to_string(i));
  Doc d = Make(body + SmallPage());
  REQUIRE(d.layout);
  CHECK(d.pages() > 2);
  CHECK(d.layout->Done());
  for (size_t p = 0; p < d.pages(); ++p) {
    const Page& page = d.page(p);
    CHECK_NEAR(page.width, 200.0f, 0.01f);
    CHECK_EQ(page.number, static_cast<int32_t>(p + 1));
    float last = 0;
    for (const LineBox& l : page.lines) {
      CHECK(l.top >= 20 - 0.01f && l.top + l.height <= 180 + 0.01f);
      CHECK(l.top >= last);
      last = l.top;
    }
  }
}

TEST(Layout_PageAndSectionBreaks) {
  Doc d = Make(P("one") + "<w:p><w:r><w:br w:type=\"page\"/><w:t>two</w:t></w:r></w:p>" +
               "<w:p><w:pPr><w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/></w:sectPr></w:pPr><w:r><w:t>three</w:t></w:r></w:p>" +
               P("four") + "<w:sectPr><w:pgSz w:w=\"15840\" w:h=\"12240\" w:orient=\"landscape\"/></w:sectPr>");
  REQUIRE(d.layout);
  size_t p1 = 9, p2 = 9, p3 = 9, p4 = 9;
  CHECK(Find(d, u"one", &p1) && Find(d, u"two", &p2) && Find(d, u"three", &p3) && Find(d, u"four", &p4));
  CHECK_EQ(p1, 0u);
  CHECK_EQ(p2, 1u);
  CHECK_EQ(p3, 1u);
  CHECK_EQ(p4, 2u);
  CHECK(d.pages() == 3 && d.page(2).width > d.page(2).height);  // the landscape section
}

TEST(Layout_SpacingBetweenParagraphsIsTheLarger) {
  const std::string body = P("A", "<w:spacing w:after=\"240\"/>") + P("B", "<w:spacing w:before=\"480\"/>");
  Doc html = Make(body);
  Doc sum = Make(body, "<w:compat><w:doNotUseHTMLParagraphAutoSpacing/></w:compat>");
  REQUIRE(html.layout && sum.layout);
  const LineBox *a = Find(html, u"A"), *b = Find(html, u"B");
  const LineBox *a2 = Find(sum, u"A"), *b2 = Find(sum, u"B");
  REQUIRE(a && b && a2 && b2);
  CHECK_NEAR(b->top - (a->top + a->height), 24.0f, 0.05f);    // max(12, 24)
  CHECK_NEAR(b2->top - (a2->top + a2->height), 36.0f, 0.05f);  // 12 + 24
}

TEST(Layout_KeepWithNextAndWidowControl) {
  // Lines of filler, then a heading kept with its paragraph: both go to the next page together.
  std::string body;
  for (int i = 0; i < 40; ++i) body += P("filler " + std::to_string(i));
  Doc probe = Make(body + SmallPage());
  REQUIRE(probe.layout);
  // Fill the first page to one line short, then a heading with keepNext and a two-line paragraph.
  size_t perPage = 0;
  for (const LineBox& l : probe.page(0).lines)
    if (l.story == fw::LayoutStory::Body) ++perPage;
  std::string fill;
  for (size_t i = 0; i + 1 < perPage; ++i) fill += P("filler " + std::to_string(i));
  Doc d = Make(fill + P("Heading", "<w:keepNext/>") + P(Words(40)) + SmallPage());
  REQUIRE(d.layout);
  size_t heading = 9;
  CHECK(Find(d, u"Heading", &heading));
  CHECK_EQ(heading, 1u);
  // Widow control: a paragraph whose last line would go on alone takes one more line along.
  Doc w = Make(fill + P(Words(40)) + SmallPage());
  REQUIRE(w.layout);
  size_t onFirst = 0;
  for (const LineBox& l : w.page(0).lines)
    if (l.story == fw::LayoutStory::Body && l.text.find(u"word") != std::u16string::npos) ++onFirst;
  CHECK(onFirst != 1);  // never a lone first line at the foot of the page
  if (w.pages() > 1) {
    size_t onSecond = 0;
    for (const LineBox& l : w.page(1).lines)
      if (l.story == fw::LayoutStory::Body) ++onSecond;
    CHECK(onSecond != 1);  // nor a lone last line at the top of the next
  }
}

TEST(Layout_TableRowsSplitUnderRepeatedHeaders) {
  std::string rows = "<w:tr><w:trPr><w:tblHeader/></w:trPr><w:tc><w:p><w:r><w:t>Header</w:t></w:r></w:p></w:tc></w:tr>";
  for (int i = 0; i < 20; ++i) rows += "<w:tr><w:tc>" + P("Row " + std::to_string(i)) + "</w:tc></w:tr>";
  Doc d = Make("<w:tbl><w:tblPr><w:tblW w:w=\"0\" w:type=\"auto\"/></w:tblPr><w:tblGrid><w:gridCol w:w=\"3000\"/></w:tblGrid>" + rows +
               "</w:tbl>" + P("after") + SmallPage());
  REQUIRE(d.layout);
  CHECK(d.pages() > 1);
  for (size_t p = 0; p < d.pages(); ++p) {
    bool header = false;
    for (const LineBox& l : d.page(p).lines)
      if (l.text == u"Header") header = true;
    if (p + 1 < d.pages() || Find(d, u"Row 19")) CHECK(header || p == d.pages() - 1);
  }
}

TEST(Layout_FootnotesAtTheFootOfTheirPage) {
  const std::string notes =
      "<w:footnote w:type=\"separator\" w:id=\"-1\"><w:p><w:r><w:separator/></w:r></w:p></w:footnote>"
      "<w:footnote w:id=\"1\"><w:p><w:r><w:footnoteRef/></w:r><w:r><w:t xml:space=\"preserve\"> The note.</w:t></w:r></w:p></w:footnote>";
  Doc d = Make("<w:p><w:r><w:t>Text</w:t></w:r><w:r><w:rPr><w:vertAlign w:val=\"superscript\"/></w:rPr><w:footnoteReference w:id=\"1\"/></w:r></w:p>" +
                   P("More") + SmallPage(),
               "", notes);
  REQUIRE(d.layout);
  size_t textPage = 9, notePage = 9;
  const LineBox* text = Find(d, u"Text", &textPage);
  const LineBox* note = Find(d, u"The note.", &notePage);
  REQUIRE(text && note);
  CHECK_EQ(textPage, notePage);
  CHECK(note->story == fw::LayoutStory::Footnote);
  CHECK(note->top > text->top + 60);  // at the foot of the page, not under the text
  CHECK(text->text.find(u"1") != std::u16string::npos);  // the reference mark
}

TEST(Layout_FloatingTablesAndFramesLeaveTheFlow) {
  // A table positioned on the page doesn't push the paragraph after it down.
  Doc d = Make("<w:tbl><w:tblPr><w:tblpPr w:vertAnchor=\"page\" w:horzAnchor=\"page\" w:tblpX=\"3000\" w:tblpY=\"2800\"/>"
               "<w:tblW w:w=\"600\" w:type=\"dxa\"/></w:tblPr><w:tblGrid><w:gridCol w:w=\"600\"/></w:tblGrid>"
               "<w:tr><w:tc>" + P("float") + "</w:tc></w:tr></w:tbl>" + P("first") +
               "<w:p><w:pPr><w:framePr w:w=\"600\" w:hAnchor=\"page\" w:vAnchor=\"page\" w:x=\"200\" w:y=\"3200\"/></w:pPr>"
               "<w:r><w:t>framed</w:t></w:r></w:p>" + P("second") + SmallPage());
  REQUIRE(d.layout);
  const LineBox *flt = Find(d, u"float"), *first = Find(d, u"first"), *framed = Find(d, u"framed"), *second = Find(d, u"second");
  REQUIRE(flt && first && framed && second);
  CHECK(first->top < 30);                   // at the top of the text area
  CHECK(flt->top >= 140 && flt->x >= 150);  // where the table was placed
  CHECK(framed->top >= 160 - 0.5f);         // and the frame
  CHECK_NEAR(second->top, first->top + first->height, 0.5f);  // the frame took no room in the flow
}

TEST(Layout_WhatWordLeavesOut) {
  // A page break in a table cell adds nothing; a paragraph whose mark and text are deleted is
  // gone; so is the empty paragraph that only holds a section break.
  Doc d = Make("<w:tbl><w:tblGrid><w:gridCol w:w=\"3000\"/></w:tblGrid><w:tr><w:tc>"
               "<w:p><w:r><w:br w:type=\"page\"/><w:t>cell</w:t></w:r></w:p></w:tc></w:tr></w:tbl>" +
               P("after table") +
               "<w:p><w:pPr><w:rPr><w:del w:id=\"1\" w:author=\"a\"/></w:rPr></w:pPr><w:del w:id=\"2\" w:author=\"a\">"
               "<w:r><w:delText>deleted</w:delText></w:r></w:del></w:p>" +
               P("kept") + "<w:p><w:pPr>" + SmallPage() + "</w:pPr></w:p>" + P("next section") +
               SmallPage("<w:type w:val=\"continuous\"/>"));
  REQUIRE(d.layout);
  CHECK_EQ(d.pages(), 1u);
  const LineBox *cell = Find(d, u"cell"), *after = Find(d, u"after table"), *kept = Find(d, u"kept"), *next = Find(d, u"next section");
  REQUIRE(cell && after && kept && next);
  CHECK(cell->top < after->top && after->top - cell->top < cell->height * 1.6f);  // one line, not two
  CHECK(!Find(d, u"deleted"));
  CHECK_NEAR(kept->top, after->top + after->height, 0.5f);
  CHECK_NEAR(next->top, kept->top + kept->height, 0.5f);
}

TEST(Layout_HitTestAndCarets) {
  Doc d = Make(P("Hello world") + P(Words(30)));
  REQUIRE(d.layout);
  const LineBox* hello = Find(d, u"Hello");
  REQUIRE(hello);
  auto hit = d.layout->HitTest(0, hello->x + 1, hello->baseline - 2);
  CHECK(hit && hit->para == hello->para && hit->offset <= 1);
  auto end = d.layout->HitTest(0, 10000, hello->baseline - 2);
  CHECK(end && end->para == hello->para && end->offset == 11);
  auto caret = d.layout->CaretAt({hello->para, 6});
  CHECK(caret && caret->page == 0 && caret->x > hello->x && caret->top <= hello->baseline && caret->bottom >= hello->baseline);
  auto ranges = d.layout->Ranges({hello->para, 0}, {hello->para, 5});
  CHECK(ranges.size() == 1 && ranges[0].second.Width() > 0);
}

TEST(Layout_MergedCellsRunOnAcrossPages) {
  // A first column merged over twenty rows, holding forty lines: its text continues on the next
  // page with the rows, never below the text area.
  std::string merged;
  for (int i = 0; i < 40; ++i) merged += P("merged " + std::to_string(i));
  std::string rows;
  for (int i = 0; i < 20; ++i)
    rows += "<w:tr><w:tc><w:tcPr>" + std::string(i == 0 ? "<w:vMerge w:val=\"restart\"/>" : "<w:vMerge/>") + "</w:tcPr>" +
            (i == 0 ? merged : P("")) + "</w:tc><w:tc>" + P("row " + std::to_string(i)) + "</w:tc></w:tr>";
  Doc d = Make("<w:tbl><w:tblGrid><w:gridCol w:w=\"1500\"/><w:gridCol w:w=\"1500\"/></w:tblGrid>" + rows + "</w:tbl>" + P("after") +
               SmallPage());
  REQUIRE(d.layout);
  CHECK(d.pages() > 1);
  size_t first = 9, last = 9;
  CHECK(Find(d, u"merged 0", &first) && Find(d, u"merged 39", &last));
  CHECK(first == 0 && last > 0);
  for (size_t p = 0; p < d.pages(); ++p)
    for (const LineBox& l : d.page(p).lines) CHECK(l.top + l.height <= d.page(p).body.bottom + 0.5f);
}

TEST(Layout_RowsThatStartPagesOrStayTogether) {
  // A row whose first paragraph has a page break before starts a page.
  Doc b = Make(P("before") + "<w:tbl><w:tblGrid><w:gridCol w:w=\"3000\"/></w:tblGrid><w:tr><w:tc>" + P("one") +
               "</w:tc></w:tr><w:tr><w:tc>" + P("two", "<w:pageBreakBefore/>") + "</w:tc></w:tr></w:tbl>");
  REQUIRE(b.layout);
  size_t one = 9, two = 9;
  CHECK(Find(b, u"one", &one) && Find(b, u"two", &two));
  CHECK(one == 0 && two == 1);
  // Rows whose paragraphs keep with the next stay with it, and with the heading kept with them.
  std::string fill;
  for (int i = 0; i < 40; ++i) fill += P("filler " + std::to_string(i));
  Doc probe = Make(fill + SmallPage());
  REQUIRE(probe.layout);
  size_t perPage = 0;
  for (const LineBox& l : probe.page(0).lines)
    if (l.story == fw::LayoutStory::Body) ++perPage;
  std::string start;
  for (size_t i = 0; i + 3 < perPage; ++i) start += P("filler " + std::to_string(i));
  std::string rows;
  for (int i = 0; i < 4; ++i)
    rows += "<w:tr><w:tc>" + P("kept " + std::to_string(i), i < 3 ? "<w:keepNext/>" : "") + "</w:tc></w:tr>";
  Doc k = Make(start + P("Heading", "<w:keepNext/>") + "<w:tbl><w:tblGrid><w:gridCol w:w=\"3000\"/></w:tblGrid>" + rows +
               "</w:tbl>" + SmallPage());
  REQUIRE(k.layout);
  size_t heading = 9, k0 = 9, k3 = 9;
  CHECK(Find(k, u"Heading", &heading) && Find(k, u"kept 0", &k0) && Find(k, u"kept 3", &k3));
  CHECK(heading == 1 && k0 == 1 && k3 == 1);
}

TEST(Layout_VerticalTextInCells) {
  // A narrow cell with btLr text: its line runs up the cell, unwrapped, and the row is as tall as
  // the line is long; a click on it finds its text.
  Doc d = Make("<w:tbl><w:tblGrid><w:gridCol w:w=\"500\"/><w:gridCol w:w=\"3000\"/></w:tblGrid><w:tr><w:tc><w:tcPr>"
               "<w:textDirection w:val=\"btLr\"/></w:tcPr>" + P("Turned label text") + "</w:tc><w:tc>" + P("beside") +
               "</w:tc></w:tr></w:tbl>" + P("after"));
  REQUIRE(d.layout);
  const LineBox *turned = Find(d, u"Turned label text"), *after = Find(d, u"after");
  REQUIRE(turned && after);
  CHECK_EQ(turned->turn, 3);
  const float length = turned->line.contentRight - turned->line.contentLeft;
  CHECK(length > 40);
  CHECK(after->top > turned->turnY - 1);  // the row reaches down to where the line starts
  CHECK(after->top - turned->turnY < 20);
  // The line runs up from (turnX, turnY): its middle is above that point, inside the narrow cell.
  auto hit = d.layout->HitTest(0, turned->turnX + turned->baseline - 2, turned->turnY - length / 2);
  CHECK(hit && hit->para == turned->para);
  auto ranges = d.layout->Ranges({turned->para, 0}, {turned->para, 6});
  CHECK(ranges.size() == 1 && ranges[0].second.Height() > ranges[0].second.Width());
}

TEST(Layout_RangesStopAtTheirEnd) {
  // A range that ends exactly at the end of a line doesn't lay out the rest of the document.
  std::string body;
  for (int i = 0; i < 200; ++i) body += P(Words(30));
  Doc d = Make(body + SmallPage(), "", "", false);
  REQUIRE(d.layout);
  d.layout->LayOutPages(1);
  const size_t before = d.layout->PageCount();
  const LineBox& first = d.page(0).lines.front();
  auto ranges = d.layout->Ranges({first.para, 0}, {first.para, first.line.end - first.prefix});
  CHECK_EQ(ranges.size(), 1u);
  CHECK(d.layout->PageCount() <= before + 1);
  CHECK(!d.layout->Done());
}

TEST(Layout_EastAsianFontsGetEastAsianStandIns) {
  // Missing fonts from the font table: a fixed-pitch Japanese Mincho, a Korean font known by its
  // Korean name, a Chinese one by its code pages only, and a Latin fixed-pitch one.
  Doc d = Make(P("text"), "", "", true,
               "<w:font w:name=\"MS \xE6\x98\x8E\xE6\x9C\x9D\"><w:panose1 w:val=\"02020609040205080304\"/>"
               "<w:charset w:val=\"80\"/><w:family w:val=\"roman\"/><w:pitch w:val=\"fixed\"/></w:font>"
               "<w:font w:name=\"Unknown Hangul Font\"><w:charset w:val=\"81\"/><w:pitch w:val=\"variable\"/></w:font>"
               "<w:font w:name=\"Unknown Song\"><w:panose1 w:val=\"02010600030101010101\"/><w:charset w:val=\"00\"/>"
               "<w:sig w:usb0=\"00000003\" w:usb1=\"288F0000\" w:usb2=\"00000016\" w:usb3=\"00000000\" w:csb0=\"00040001\" w:csb1=\"00000000\"/></w:font>"
               "<w:font w:name=\"Unknown Typewriter\"><w:charset w:val=\"00\"/><w:pitch w:val=\"fixed\"/></w:font>");
  REQUIRE(d.doc);
  auto in = [](const std::wstring& s, std::initializer_list<const wchar_t*> names) {
    for (const wchar_t* n : names)
      if (s == n) return true;
    return false;
  };
  // Every Windows 10 and 11 has one of each script's last stand-ins.
  const std::wstring mincho = SubstituteFamily(*d.doc, L"MS \x660E\x671D");
  CHECK(in(mincho, {L"MS Mincho", L"Yu Mincho", L"MS Gothic", L"Yu Gothic"}));
  CHECK(in(SubstituteFamily(*d.doc, L"Unknown Hangul Font"), {L"Malgun Gothic", L"Gulim", L"Dotum"}));
  CHECK(in(SubstituteFamily(*d.doc, L"\xBC14\xD0D5"), {L"Batang", L"Malgun Gothic"}));  // not in the table
  CHECK(in(SubstituteFamily(*d.doc, L"Unknown Song"), {L"SimSun", L"Microsoft YaHei"}));
  CHECK(SubstituteFamily(*d.doc, L"Unknown Typewriter") == L"Courier New");
}

// ---- Laying out again after edits (D2)

namespace {

std::string Narrow(const std::u16string& s) {
  std::string out;
  for (char16_t c : s) out += c < 128 ? static_cast<char>(c) : '?';
  return out;
}

// The layout as text, page by page, to compare one laid out again after edits with one laid out
// from scratch.
std::vector<std::string> Signature(const Layout& l) {
  std::vector<std::string> pages;
  char buf[160];
  for (size_t p = 0; p < l.PageCount(); ++p) {
    const Page& pg = l.GetPage(p);
    std::snprintf(buf, sizeof buf, "page %zu: %.2fx%.2f number %d first %u.%u\n", p, pg.width, pg.height, pg.number,
                  pg.first.para, pg.first.offset);
    std::string s = buf;
    for (const LineBox& lb : pg.lines) {
      std::snprintf(buf, sizeof buf, "  %d p%u [%u,%u) x%.2f y%.2f h%.2f ", static_cast<int>(lb.story), lb.para,
                    lb.line.start, lb.line.end, lb.x, lb.top, lb.height);
      s += buf + Narrow(lb.text) + "\n";
    }
    for (const auto& dr : pg.drawings) {
      std::snprintf(buf, sizeof buf, "  drawing %u %.2f,%.2f\n", dr.drawing, dr.rect.left, dr.rect.top);
      s += buf;
    }
    pages.push_back(std::move(s));
  }
  return pages;
}

// Whether `l` is as a layout made from scratch would be; prints where not.
bool SameAsFresh(const Document& doc, const Layout& l, const char* when) {
  Layout fresh(doc, Fonts());
  fresh.LayOutAll();
  const std::vector<std::string> a = Signature(l), b = Signature(fresh);
  for (size_t p = 0; p < std::max(a.size(), b.size()); ++p) {
    if (p < a.size() && p < b.size() && a[p] == b[p]) continue;
    std::printf("  %s: page %zu differs (%zu pages, from scratch %zu)\n  incremental:\n%s  from scratch:\n%s", when, p,
                a.size(), b.size(), p < a.size() ? a[p].c_str() : "(none)\n", p < b.size() ? b[p].c_str() : "(none)\n");
    return false;
  }
  return true;
}

// A random edit of the kind typing makes: text in or out, Enter, joining, a paragraph property.
void RandomEdit(edit::Editor& ed, const Document& doc, std::mt19937& rng) {
  // Mostly the body; now and then a header or a note.
  const bool body = rng() % 8 != 0;
  std::vector<uint32_t> paras;
  for (uint32_t p = 0; p < doc.Paragraphs().size(); ++p)
    if ((doc.Para(p).story == doc.MainStory()) == body) paras.push_back(p);
  if (paras.empty()) return;
  const uint32_t para = paras[rng() % paras.size()];
  const auto len = static_cast<uint32_t>(doc.Para(para).text.size());
  const Pos at{para, len ? static_cast<uint32_t>(rng() % (len + 1)) : 0};
  static const char16_t* const kTexts[] = {u"x", u" ", u"ab", u"word ", u"several more words to wrap the line ",
                                           u"a long run of text that is long enough to take a line or two of the small "
                                           u"page by itself, and then some more of it "};
  const uint32_t section = edit::SectionOf(doc, para), sections = static_cast<uint32_t>(doc.Sections().size());
  switch (rng() % 13) {
    case 0: case 1: case 2: case 3: ed.InsertText(at, kTexts[rng() % 6]); break;
    case 4: case 5: {
      const uint32_t to = std::min(len, at.offset + 1 + static_cast<uint32_t>(rng() % 40));
      if (to > at.offset) ed.Delete(at, {para, to});
      break;
    }
    case 6: ed.SplitParagraph(at); break;
    case 7: ed.JoinParagraphs(para); break;
    case 8: {
      const uint32_t next = ed.NextPara(para);
      if (next != kNone && len > 0)
        ed.Delete({para, len / 2}, {next, std::min<uint32_t>(3, static_cast<uint32_t>(doc.Para(next).text.size()))});
      break;
    }
    case 9: {
      edit::PropEdit p;
      if (rng() % 2) p.Set("keepNext");
      else p.Remove("keepNext");
      ed.SetParaProps(para, para, p);
      break;
    }
    case 10: {  // page setup (D3): the paragraph's section, or the whole document
      const bool all = rng() % 4 == 0;
      edit::PageSetup p;
      switch (rng() % 4) {
        case 0: p.landscape = rng() % 2 == 0; break;
        case 1: p.left = Twips(200 + static_cast<int32_t>(rng() % 800)); p.right = Twips(200 + static_cast<int32_t>(rng() % 800)); break;
        case 2: p.top = Twips(200 + static_cast<int32_t>(rng() % 600)); p.bottom = Twips(200 + static_cast<int32_t>(rng() % 600)); break;
        default: p.width = Twips(3200 + static_cast<int32_t>(rng() % 2000)); break;
      }
      edit::SetPageSetup(ed, all ? 0 : section, all ? sections - 1 : section, p);
      break;
    }
    case 11: {
      switch (rng() % 3) {
        case 0: {
          edit::ColumnSetup c;
          c.count = 1 + static_cast<int32_t>(rng() % 3);
          c.space = Twips(200);
          c.line = rng() % 2 == 0;
          edit::SetColumns(ed, section, section, c);
          break;
        }
        case 1: edit::SetSectionStart(ed, section, static_cast<SectionStart>(rng() % 4)); break;
        default: {
          edit::Hyphenation h;
          h.automatic = rng() % 2 == 0;
          edit::SetHyphenation(ed, h);
          break;
        }
      }
      break;
    }
    default: {
      switch (rng() % 3) {
        case 0: edit::InsertSectionBreak(ed, at, static_cast<SectionStart>(rng() % 4)); break;
        case 1: if (sections > 1) edit::DeleteSectionBreak(ed, static_cast<uint32_t>(rng() % (sections - 1))); break;
        default: edit::InsertBreak(ed, at, rng() % 2 == 0); break;
      }
      break;
    }
  }
}

}  // namespace

TEST(Layout_CellsOnAPage) {
  // A 2 x 2 table: the first column's two cells merged, a table in the last cell. Every piece of a
  // cell is there, in its row and grid column, the inner table's after the cell it's in.
  const std::string inner = "<w:tbl><w:tblPr><w:tblW w:w=\"0\" w:type=\"auto\"/></w:tblPr><w:tblGrid><w:gridCol w:w=\"600\"/>"
                            "</w:tblGrid><w:tr><w:tc>" + P("In") + "</w:tc></w:tr></w:tbl>";
  const std::string table =
      "<w:tbl><w:tblPr><w:tblW w:w=\"0\" w:type=\"auto\"/></w:tblPr><w:tblGrid><w:gridCol w:w=\"1200\"/><w:gridCol w:w=\"1400\"/>"
      "</w:tblGrid><w:tr><w:tc><w:tcPr><w:vMerge w:val=\"restart\"/></w:tcPr>" + P("Merged") + "</w:tc><w:tc>" + P("B") +
      "</w:tc></w:tr><w:tr><w:tc><w:tcPr><w:vMerge/></w:tcPr><w:p/></w:tc><w:tc>" + inner + P("D") + "</w:tc></w:tr></w:tbl>";
  Doc d = Make(P("Before") + table + P("After") + SmallPage());
  REQUIRE(d.layout);
  const std::vector<fw::doc::layout::CellRect> cells = d.layout->Cells(0);
  REQUIRE(cells.size() == 5);
  const ElemId tbl = cells[0].tbl;
  CHECK(cells[0].row == 0 && cells[0].col == 0 && cells[1].row == 0 && cells[1].col == 1);
  CHECK(cells[2].row == 1 && cells[2].col == 0 && cells[3].row == 1 && cells[3].col == 1);
  for (size_t i = 0; i < 4; ++i) CHECK(cells[i].tbl == tbl && cells[i].box.Width() > 0 && cells[i].box.bottom > cells[i].box.top);
  // Rows share their edges; columns line up.
  CHECK(std::abs(cells[0].box.bottom - cells[2].box.top) < 0.01f && std::abs(cells[1].box.left - cells[3].box.left) < 0.01f);
  CHECK(std::abs(cells[0].box.right - cells[1].box.left) < 0.01f);
  // The inner table, inside the cell it's in.
  const fw::doc::layout::CellRect& in = cells[4];
  CHECK(in.tbl != tbl && in.row == 0 && in.col == 0);
  CHECK(in.box.left >= cells[3].box.left && in.box.right <= cells[3].box.right && in.box.top >= cells[3].box.top);
  CHECK(d.layout->Cells(99).empty());
}

TEST(Layout_APageSetupChangeLaysOutInIdleTime) {
  // A change of every section's margins: the first pages again at once, and the caret near the end
  // isn't laid out (so painting doesn't lay out the whole document) until the rest is.
  std::string body;
  for (int i = 0; i < 60; ++i) body += P(Words(25));
  Doc d = Make(body + SmallPage());
  REQUIRE(d.layout);
  const size_t pages = d.pages();
  REQUIRE(pages > 20);
  const std::vector<uint32_t> paras = d.doc->StoryParagraphs(d.doc->MainStory());
  const TextPos first{paras.front(), 0}, last{paras.back(), 3};
  CHECK(d.layout->LaidOut(first) && d.layout->LaidOut(last));
  edit::Editor ed(*d.doc);
  ed.OnChange([&](const edit::Change& c) { d.layout->Edited(c); });
  edit::PageSetup wider;
  wider.left = Twips(1600);
  REQUIRE(edit::SetPageSetup(ed, 0, 0, wider));
  CHECK(!d.layout->Done() && !d.layout->LaidOut(first) && !d.layout->LaidOut(last));
  d.layout->LayOutPages(2);
  CHECK(d.layout->LaidOut(first) && !d.layout->LaidOut(last));
  CHECK(!d.layout->Done());  // asking laid nothing out
  CHECK(d.pages() >= pages - 1);  // the old pages are shown meanwhile
  while (d.layout->Continue(5)) {
  }
  CHECK(d.layout->LaidOut(last) && d.pages() > pages);
  CHECK(SameAsFresh(*d.doc, *d.layout, "page setup"));
}

TEST(Layout_EditsLayOutAgainAsFromScratch) {
  // Paragraphs of every length, kept-together headings, a table that splits, footnotes and a
  // section break, on small pages; then random edits, laid out again a bit at a time.
  const std::string notes =
      "<w:footnote w:type=\"separator\" w:id=\"-1\"><w:p><w:r><w:separator/></w:r></w:p></w:footnote>"
      "<w:footnote w:id=\"1\"><w:p><w:r><w:footnoteRef/></w:r><w:r><w:t xml:space=\"preserve\"> A note.</w:t></w:r></w:p></w:footnote>"
      "<w:footnote w:id=\"2\"><w:p><w:r><w:footnoteRef/></w:r><w:r><w:t xml:space=\"preserve\"> Another, longer note that "
      "takes two lines at least.</w:t></w:r></w:p></w:footnote>";
  auto noteRef = [](int id) {
    return "<w:p><w:r><w:t xml:space=\"preserve\">Noted </w:t></w:r><w:r><w:footnoteReference w:id=\"" +
           std::to_string(id) + "\"/></w:r><w:r><w:t xml:space=\"preserve\"> " + Words(12) + "</w:t></w:r></w:p>";
  };
  // A table with a repeated header row and a table in one of its cells.
  const std::string inner = "<w:tbl><w:tblPr><w:tblW w:w=\"0\" w:type=\"auto\"/></w:tblPr><w:tblGrid><w:gridCol w:w=\"700\"/>"
                            "</w:tblGrid><w:tr><w:tc>" + P(Words(5)) + "</w:tc></w:tr></w:tbl>";
  std::string rows;
  for (int i = 0; i < 8; ++i)
    rows += std::string("<w:tr>") + (i == 0 ? "<w:trPr><w:tblHeader/></w:trPr>" : "") + "<w:tc>" +
            P("Cell " + std::to_string(i) + " " + Words(i * 3)) + "</w:tc><w:tc>" + (i == 4 ? inner : "") + P(Words(4)) +
            "</w:tc></w:tr>";
  const std::string table = "<w:tbl><w:tblPr><w:tblW w:w=\"0\" w:type=\"auto\"/></w:tblPr><w:tblGrid><w:gridCol w:w=\"1500\"/>"
                            "<w:gridCol w:w=\"1500\"/></w:tblGrid>" + rows + "</w:tbl>";
  // Merged cells running on across rows, and a header showing the page and the page count.
  const std::string merged =
      "<w:tbl><w:tblPr><w:tblW w:w=\"0\" w:type=\"auto\"/></w:tblPr><w:tblGrid><w:gridCol w:w=\"1500\"/><w:gridCol w:w=\"1500\"/>"
      "</w:tblGrid><w:tr><w:tc><w:tcPr><w:vMerge w:val=\"restart\"/></w:tcPr>" + P(Words(30)) + "</w:tc><w:tc>" + P("a") +
      "</w:tc></w:tr><w:tr><w:tc><w:tcPr><w:vMerge/></w:tcPr><w:p/></w:tc><w:tc>" + P("b") + "</w:tc></w:tr></w:tbl>";
  auto field = [](const char* code) {
    return std::string("<w:r><w:fldChar w:fldCharType=\"begin\"/></w:r><w:r><w:instrText xml:space=\"preserve\"> ") + code +
           " </w:instrText></w:r><w:r><w:fldChar w:fldCharType=\"separate\"/></w:r><w:r><w:t>1</w:t></w:r>"
           "<w:r><w:fldChar w:fldCharType=\"end\"/></w:r>";
  };
  const std::string header = "<w:p><w:r><w:t xml:space=\"preserve\">Page </w:t></w:r>" + field("PAGE") +
                             "<w:r><w:t xml:space=\"preserve\"> of </w:t></w:r>" + field("NUMPAGES") + "</w:p>";
  const std::string hdr = "<w:headerReference w:type=\"default\" r:id=\"rIdH\"/>";
  std::string body;
  for (int i = 0; i < 36; ++i) {
    if (i % 9 == 0) body += P("Heading " + std::to_string(i), "<w:keepNext/>");
    body += P(Words((i * 7) % 23 + 1), i % 5 == 0 ? "<w:widowControl/>" : i % 7 == 3 ? "<w:keepLines/>" : "");
    if (i == 6) body += noteRef(1);
    if (i == 10) body += "<w:p><w:r><w:t>Before a break</w:t></w:r><w:r><w:br w:type=\"page\"/></w:r><w:r><w:t>after</w:t></w:r></w:p>";
    if (i == 14) body += table;
    if (i == 17) body += P("Starts a page", "<w:pageBreakBefore/>") + merged;
    if (i == 20) body += P("End of section one", "<w:sectPr>" + hdr + "<w:type w:val=\"nextPage\"/><w:pgSz w:w=\"4000\" w:h=\"4000\"/>"
                                                 "<w:pgMar w:top=\"400\" w:right=\"400\" w:bottom=\"400\" w:left=\"400\" "
                                                 "w:header=\"200\" w:footer=\"200\" w:gutter=\"0\"/></w:sectPr>");
    if (i == 27) body += noteRef(2);
    if (i == 30) body += P("Two columns from here", "<w:sectPr>" + hdr + "<w:type w:val=\"continuous\"/><w:pgSz w:w=\"4000\" w:h=\"4000\"/>"
                                                    "<w:pgMar w:top=\"400\" w:right=\"400\" w:bottom=\"400\" w:left=\"400\" "
                                                    "w:header=\"200\" w:footer=\"200\" w:gutter=\"0\"/></w:sectPr>");
  }
  for (unsigned seed : {1u, 2u, 3u, 4u}) {
    Doc d = Make(body + SmallPage(hdr + "<w:type w:val=\"continuous\"/><w:cols w:num=\"2\" w:space=\"200\"/>"), "", notes,
                 true, "", header);
    REQUIRE(d.layout);
    REQUIRE(d.pages() > 8);
    edit::Editor ed(*d.doc);
    ed.OnChange([&](const edit::Change& c) { d.layout->Edited(c); });
    std::mt19937 rng(seed);
    for (int step = 0; step < 120; ++step) {
      RandomEdit(ed, *d.doc, rng);
      switch (rng() % 4) {
        case 0: d.layout->LayOutPages(1 + rng() % 6); break;
        case 1: d.layout->Continue(0.2); break;
        case 2: break;
        default: d.layout->LayOutAll(); break;
      }
      // The pages shown, old ones too, hit-test to positions that exist.
      for (size_t p = 0; p < d.pages(); ++p)
        if (auto pos = d.layout->HitTest(p, 100, 100)) {
          CHECK(pos->para < d.doc->Paragraphs().size());
          CHECK(pos->offset <= d.doc->Para(pos->para).text.size());
        }
      if (step % 8 == 7) {
        d.layout->LayOutAll();
        char when[64];
        std::snprintf(when, sizeof when, "seed %u, step %d", seed, step);
        REQUIRE(SameAsFresh(*d.doc, *d.layout, when));
      }
    }
  }
}

TEST(Layout_TypingLaysOutAPageOrTwo) {
  // A keystroke on the first page of a long document: the old pages after the first page that
  // ends as it did come back as they were, so laying out three pages finishes the document.
  std::string body;
  for (int i = 0; i < 200; ++i) body += P(Words(i % 17 + 3));
  Doc d = Make(body + SmallPage());
  REQUIRE(d.layout && d.pages() > 40);
  const size_t pages = d.pages();
  edit::Editor ed(*d.doc);
  ed.OnChange([&](const edit::Change& c) { d.layout->Edited(c); });
  for (int i = 0; i < 5; ++i) {
    REQUIRE(ed.InsertText({0, 2}, u"x"));
    CHECK(!d.layout->Done());
    CHECK_EQ(d.pages(), pages);  // the old pages are still shown
    d.layout->LayOutPages(3);
    CHECK(d.layout->Done());
  }
  CHECK(SameAsFresh(*d.doc, *d.layout, "after typing"));
  // The caret and a selection across the change, on the pages laid out again.
  const auto caret = d.layout->CaretAt({0, 7});
  REQUIRE(caret);
  CHECK_EQ(caret->page, 0u);
  CHECK(!d.layout->Ranges({0, 0}, {1, 3}).empty());
  // Enter at the start pushes everything down a line: laid out again to the end, in idle time.
  REQUIRE(ed.SplitParagraph({0, 0}));
  d.layout->LayOutPages(1);
  CHECK_EQ(d.pages(), pages);
  // Until then the old pages show what they did, under the paragraphs' new indices.
  const Page& old = d.page(20);
  const LineBox* line = nullptr;
  for (const LineBox& l : old.lines)
    if (l.story == fw::LayoutStory::Body && !l.text.empty() && l.line.start == 0) {
      line = &l;
      break;
    }
  REQUIRE(line);
  CHECK(d.doc->Para(line->para).text.compare(0, line->text.size(), line->text) == 0);
  const auto hit = d.layout->HitTest(20, line->x + line->line.left + 1, line->top + line->height / 2);
  REQUIRE(hit);
  CHECK_EQ(hit->para, line->para);
  d.layout->LayOutAll();
  CHECK(SameAsFresh(*d.doc, *d.layout, "after Enter"));
}

TEST(Layout_TableEditsLayOutAgainAsFromScratch) {
  // Table editing (D3) on a table that runs over several small pages, between paragraphs: each
  // change of shape is laid out again as a fresh layout lays it out, a bit at a time or at once.
  std::string rows;
  for (int i = 0; i < 12; ++i)
    rows += std::string("<w:tr>") + (i == 0 ? "<w:trPr><w:tblHeader/></w:trPr>" : "") + "<w:tc>" + P("Cell " + std::to_string(i)) +
            "</w:tc><w:tc>" + P(Words(i % 5 + 1)) + "</w:tc><w:tc>" + P(Words(2)) + "</w:tc></w:tr>";
  const std::string table = "<w:tbl><w:tblPr><w:tblW w:w=\"0\" w:type=\"auto\"/></w:tblPr><w:tblGrid><w:gridCol w:w=\"1000\"/>"
                            "<w:gridCol w:w=\"1000\"/><w:gridCol w:w=\"1000\"/></w:tblGrid>" + rows + "</w:tbl>";
  std::string body;
  for (int i = 0; i < 8; ++i) body += P(Words(i * 3 + 2));
  body += table;
  for (int i = 0; i < 8; ++i) body += P(Words(i * 2 + 1));
  Doc d = Make(body + SmallPage());
  REQUIRE(d.layout);
  edit::Editor ed(*d.doc);
  ed.OnChange([&](const edit::Change& c) { d.layout->Edited(c); });
  auto cell = [&](size_t r, size_t c) {
    Node tbl;
    for (const Block& b : d.doc->Blocks(d.doc->MainStory()))
      if (b.kind == BlockKind::Table) tbl = b.node;
    Node tc = d.doc->RowCells(d.doc->TableRows(tbl)[r])[c];
    return Pos{d.doc->Blocks(tc).front().para, 0};
  };
  int step = 0;
  auto check = [&](bool ok, const char* what) {
    REQUIRE(ok);
    if (step++ % 2) d.layout->LayOutPages(2);
    d.layout->LayOutAll();
    REQUIRE(SameAsFresh(*d.doc, *d.layout, what));
  };
  check(bool(ed.InsertRows(cell(3, 1), cell(4, 1), true)), "rows inserted");
  check(bool(ed.InsertColumns(cell(2, 0), cell(2, 0), true)), "a column inserted");
  check(bool(ed.MergeCells(cell(5, 0), cell(7, 1))), "cells merged");
  check(bool(ed.SplitCell(cell(1, 2), 2, 2)), "a cell split");
  check(bool(ed.SetColumnWidth(cell(0, 0), cell(0, 0), 600)), "a column narrowed");
  check(bool(ed.DeleteRows(cell(9, 0), cell(10, 0))), "rows deleted");
  check(bool(ed.DeleteColumns(cell(0, 1), cell(0, 1))), "a column deleted");
  check(bool(ed.SetRowHeight(cell(2, 0), cell(2, 0), 900, "exact")), "a row's height");
  check(bool(ed.SetRowProps(cell(0, 0), cell(1, 0), edit::PropEdit().Set("tblHeader"))), "two header rows");
  check(bool(ed.SetRowProps(cell(0, 0), cell(1, 0), edit::PropEdit().Remove("tblHeader"))), "no header row");
  check(bool(ed.SetCellBorders(cell(1, 0), cell(3, 1), edit::Editor::CellBorders::All, "single", 12)), "borders");
  check(bool(ed.SetCellProps(cell(4, 0), cell(4, 0), edit::PropEdit().Set("shd", {{"val", "clear"}, {"fill", "FFFF00"}}))),
        "shading");
  check(bool(ed.SetTableProps(cell(0, 0), edit::PropEdit().Set("jc", {{"val", "center"}}))), "centered");
  check(bool(ed.AutoFit(cell(0, 0), edit::Editor::Fit::Window)), "fitted to the window");
  {
    // Copy and paste: cells over cells, and a new table in the text before.
    auto clip = edit::CopyCells(ed, cell(1, 0), cell(2, 1));
    REQUIRE(clip);
    check(bool(edit::PasteClip(ed, cell(6, 0), cell(6, 0), *clip)), "cells pasted over cells");
    check(bool(edit::PasteClip(ed, Pos{1, 2}, Pos{1, 2}, *clip)), "a table pasted");
    check(bool(ed.Undo()), "the pasted table undone");
  }
  check(bool(ed.InsertTable(Pos{0, 3}, 3, 2)), "a table inserted");
  check(bool(ed.Undo()) && bool(ed.Undo()) && bool(ed.Undo()), "undone");
  check(bool(ed.Redo()), "redone");
  check(bool(ed.DeleteTable(cell(0, 0))), "the table deleted");
}

TEST(Layout_CellsNumberedAsTheEditorsGrid) {
  // The Word tab fills selected cells from Layout::Cells, matching the editor's block by row and
  // grid column (D3): both number them alike. A cell over two columns, a row past gridBefore, a
  // merged cell whose second piece has its own w:tc.
  const std::string table =
      "<w:tbl><w:tblPr><w:tblW w:w=\"0\" w:type=\"auto\"/></w:tblPr><w:tblGrid><w:gridCol w:w=\"1200\"/>"
      "<w:gridCol w:w=\"1200\"/><w:gridCol w:w=\"1200\"/></w:tblGrid>"
      "<w:tr><w:tc>" + P("A") + "</w:tc><w:tc><w:tcPr><w:gridSpan w:val=\"2\"/></w:tcPr>" + P("BC") + "</w:tc></w:tr>"
      "<w:tr><w:trPr><w:gridBefore w:val=\"1\"/></w:trPr><w:tc><w:tcPr><w:vMerge w:val=\"restart\"/></w:tcPr>" + P("E") +
      "</w:tc><w:tc>" + P("F") + "</w:tc></w:tr>"
      "<w:tr><w:tc>" + P("G") + "</w:tc><w:tc><w:tcPr><w:vMerge/></w:tcPr><w:p/></w:tc><w:tc>" + P("I") + "</w:tc></w:tr></w:tbl>";
  Doc d = Make(P("Before") + table + P("After") + SmallPage());
  REQUIRE(d.layout);
  edit::Editor ed(*d.doc);
  const std::vector<fw::doc::layout::CellRect> cells = d.layout->Cells(0);
  const auto infos = ed.TableCells(Pos{1, 0});
  REQUIRE(infos.size() == 6);
  for (const auto& info : infos) {
    size_t found = 0;
    for (const auto& c : cells)
      if (c.tc == info.tc.Id())
        found += c.tbl == info.tbl.Id() && c.row == info.row && c.col == info.col && c.span == info.span;
    CHECK_EQ(found, 1u);
  }
  // The merged cell's second piece: the row below, the same column.
  const auto e = ed.CellAt(Pos{3, 0});  // "E"
  REQUIRE(e && e->rows == 2 && e->col == 1);
  size_t pieces = 0;
  for (const auto& c : cells) pieces += c.tbl == e->tbl.Id() && c.col == 1 && (c.row == 1 || c.row == 2);
  CHECK_EQ(pieces, 2u);
}
