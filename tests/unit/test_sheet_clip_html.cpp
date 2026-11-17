// Pasting from Excel, Word and the web (s2-plan "Clipboard"): the clipboard's HTML read as rows of
// cells, with Excel's values under the shown text, its classes' formats, spans as merged areas and
// paragraphs outside tables as rows of one cell.
#include <cstdio>
#include <string>

#include "TestHarness.h"
#include "sheet/view/ClipHtml.h"

using namespace fw::sheet;
using fw::sheet::view::HtmlCell;
using fw::sheet::view::HtmlGrid;
using fw::sheet::view::ReadClipHtml;

namespace {

const HtmlCell* At(const HtmlGrid& g, uint32_t row, uint32_t col) {
  for (const HtmlCell& c : g.cells)
    if (c.row == row && c.col == col) return &c;
  return nullptr;
}

std::wstring TextAt(const HtmlGrid& g, uint32_t row, uint32_t col) {
  const HtmlCell* c = At(g, row, col);
  return c ? c->text : L"<none>";
}

// The clipboard's CF_HTML: a header with offsets, then the HTML.
std::string Clip(const std::string& html) {
  const char* kHeader = "Version:0.9\r\nStartHTML:%010d\r\nEndHTML:%010d\r\nStartFragment:%010d\r\nEndFragment:%010d\r\n";
  char probe[200];
  const int headerLen = std::snprintf(probe, sizeof probe, kHeader, 0, 0, 0, 0);
  const size_t frag = html.find("<!--StartFragment-->"), fragEnd = html.find("<!--EndFragment-->");
  char head[200];
  std::snprintf(head, sizeof head, kHeader, headerLen, headerLen + static_cast<int>(html.size()),
                headerLen + static_cast<int>(frag == std::string::npos ? 0 : frag + 20),
                headerLen + static_cast<int>(fragEnd == std::string::npos ? html.size() : fragEnd));
  return head + html;
}

}  // namespace

TEST(SheetClipHtml_ExcelValuesFormatsAndSpans) {
  const std::string html =
      "<html xmlns:o=\"urn:schemas-microsoft-com:office:office\" xmlns:x=\"urn:schemas-microsoft-com:office:excel\">\n"
      "<head><style>\n<!--table\n\t{mso-displayed-decimal-separator:\"\\.\";}\n"
      "td {padding-top:1px; color:black; font-size:11.0pt; font-weight:400; font-family:Calibri, sans-serif;\n"
      "\tmso-number-format:General; text-align:general; white-space:nowrap;}\n"
      ".xl65 {mso-number-format:\"\\#\\,\\#\\#0\\.00\"; font-weight:700;}\n"
      ".xl66 {background:yellow; mso-pattern:black none; text-align:center; white-space:normal;}\n"
      ".xl67 {mso-number-format:\"\\0022$\\0022\\#\\,\\#\\#0\";}\n-->\n</style></head><body>\n"
      "<table border=0 cellpadding=0 cellspacing=0 width=128>\n<!--StartFragment-->\n"
      " <tr height=20>\n  <td height=20 class=xl65 align=right width=64 x:num=\"1234.5\">1,234.50</td>\n"
      "  <td class=xl66 width=64>Mid</td>\n </tr>\n"
      " <tr height=20>\n  <td height=20 x:str=\"007\">007</td>\n  <td x:bool=\"TRUE\">TRUE</td>\n </tr>\n"
      " <tr>\n  <td colspan=2 class=xl67 x:num=\"5\">$5</td>\n </tr>\n"
      " <tr><td>a<br style='mso-data-placement:same-cell'>b</td><td x:err=\"#DIV/0!\">#DIV/0!</td></tr>\n"
      "<!--EndFragment-->\n</table></body></html>";
  HtmlGrid g;
  CHECK(ReadClipHtml(Clip(html), g));
  CHECK(g.excel);
  CHECK_EQ(g.rows, 4u);
  CHECK_EQ(g.cols, 2u);
  const HtmlCell* a = At(g, 0, 0);
  CHECK(a && a->type == ValueType::Number && a->number == 1234.5 && a->numFmt == "#,##0.00" && a->bold);
  CHECK(a && a->size == 11 && a->font == "Calibri" && a->align == HAlign::Right && a->color == 0);
  const HtmlCell* b = At(g, 0, 1);
  CHECK(b && b->text == L"Mid" && b->fill == 0xFFFFFF00u && b->align == HAlign::Center && b->wrap && !b->bold);
  CHECK(b && b->type == ValueType::Empty && b->numFmt.empty());
  const HtmlCell* c = At(g, 1, 0);
  CHECK(c && c->type == ValueType::String && c->text == L"007");
  const HtmlCell* d = At(g, 1, 1);
  CHECK(d && d->type == ValueType::Bool && d->boolean);
  const HtmlCell* e = At(g, 2, 0);
  CHECK(e && e->type == ValueType::Number && e->number == 5 && e->numFmt == "\"$\"#,##0");
  CHECK(g.merges.size() == 1 && g.merges[0] == (Range{2, 0, 2, 1}));
  CHECK(TextAt(g, 3, 0) == L"a\nb");
  const HtmlCell* f = At(g, 3, 1);
  CHECK(f && f->type == ValueType::Error && f->error == "#DIV/0!");
}

TEST(SheetClipHtml_WebTableParagraphsAndLists) {
  const std::string html =
      "<html><head><title>Not this</title><style>p{color:red}</style></head><body><!--StartFragment-->"
      "<h2>Prices &amp; more</h2>"
      "<table><thead><tr><th>Name</th><th>Price</th></tr></thead>\n"
      "<tbody><tr><td rowspan=\"2\">Tea&nbsp;bag</td><td><b>1.5</b></td></tr>\n"
      "<tr><td style=\"color:#c00\">2</td></tr></tbody></table>"
      "<ul><li>one</li><li>two &lt;3</li></ul><!--EndFragment--></body></html>";
  HtmlGrid g;
  CHECK(ReadClipHtml(Clip(html), g));
  CHECK(!g.excel);
  CHECK(TextAt(g, 0, 0) == L"Prices & more");
  const HtmlCell* name = At(g, 1, 0);
  CHECK(name && name->text == L"Name" && name->bold);
  CHECK(TextAt(g, 1, 1) == L"Price");
  CHECK(TextAt(g, 2, 0) == L"Tea bag");
  const HtmlCell* price = At(g, 2, 1);
  CHECK(price && price->text == L"1.5" && price->bold && price->type == ValueType::Empty);
  // The second row's first column is the rowspan's: its cell goes beside it.
  const HtmlCell* two = At(g, 3, 1);
  CHECK(two && two->text == L"2" && two->color == 0xFFCC0000u && !two->bold);
  CHECK(!At(g, 3, 0));
  CHECK(g.merges.size() == 1 && g.merges[0] == (Range{2, 0, 3, 0}));
  CHECK(TextAt(g, 4, 0) == L"one");
  CHECK(TextAt(g, 5, 0) == L"two <3");
  CHECK_EQ(g.rows, 6u);
  // Web fonts aren't taken, only Excel's.
  CHECK(name && name->size == 0 && name->font.empty());
}

TEST(SheetClipHtml_WordParagraphsAndNothing) {
  HtmlGrid g;
  CHECK(ReadClipHtml(Clip("<html><body><!--StartFragment--><p class=MsoNormal>First<o:p></o:p></p>\r\n"
                          "<p class=MsoNormal>Second\r\n line</p><!--EndFragment--></body></html>"),
                     g));
  CHECK_EQ(g.rows, 2u);
  CHECK(TextAt(g, 0, 0) == L"First");
  CHECK(TextAt(g, 1, 0) == L"Second line");
  // A table with a nested one: the inner cells' text is the outer cell's.
  CHECK(ReadClipHtml("<table><tr><td>x<table><tr><td>in</td><td>ner</td></tr></table></td><td>y</td></tr></table>", g));
  CHECK(TextAt(g, 0, 0) == L"x in ner");
  CHECK(TextAt(g, 0, 1) == L"y");
  CHECK(!ReadClipHtml(Clip("<html><body><!--StartFragment--><!--EndFragment--></body></html>"), g));
  CHECK(!ReadClipHtml("", g));
}
