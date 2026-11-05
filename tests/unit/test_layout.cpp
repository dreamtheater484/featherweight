// Pagination (src/doc/layout): documents built from XML, laid out with the PC's fonts. The checks
// are about where things go relative to each other and to the page, not exact positions, so they
// hold whatever the font metrics.
#include "TestHarness.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "PkgTestUtil.h"
#include "doc/docx/Docx.h"
#include "doc/layout/Layout.h"
#include "text/Fonts.h"

// Stops the test when a check it depends on fails.
#define REQUIRE(cond)   do { if (!(cond)) { test::Fail(__FILE__, __LINE__, #cond); return; } } while (0)

using namespace fw::doc;
using fw::doc::layout::Layout;
using fw::doc::layout::LineBox;
using fw::doc::layout::Page;

namespace {

constexpr const char* kW = "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"";

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

Doc Make(const std::string& body, const std::string& settings = "", const std::string& footnotes = "", bool all = true) {
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
                  {"word/document.xml", head + "<w:document " + kW + "><w:body>" + body + "</w:body></w:document>"},
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
