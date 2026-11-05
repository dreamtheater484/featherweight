#include "TestHarness.h"

#include <algorithm>
#include <string>
#include <vector>

#include "DocTestUtil.h"
#include "doc/edit/Editor.h"
#include "doc/model/Utf.h"
#include "pkg/Check.h"

using namespace fw::doc;
using namespace doc_test;
using edit::Editor;
using edit::PropEdit;

#define REQUIRE(cond)   do { if (!(cond)) { test::Fail(__FILE__, __LINE__, #cond); return; } } while (0)

// Table editing (milestone D3): every change keeps the grid consistent with the cells, every row a
// cell and every cell a paragraph, writes what Word would, and undoes exactly; what can't be done
// safely is refused and changes nothing.

namespace {

Pos P(uint32_t para, uint32_t offset) { return Pos{para, offset}; }
std::vector<uint32_t> Main(const Document& doc) { return doc.StoryParagraphs(doc.MainStory()); }
std::string Text(const Document& doc, uint32_t para) { return ToUtf8(doc.Para(para).text); }

// <w:p> with this text.
std::string Para(const std::string& text) {
  return text.empty() ? "<w:p/>" : "<w:p><w:r><w:t xml:space=\"preserve\">" + text + "</w:t></w:r></w:p>";
}
// A table: rows of cells, each "text" or "text|tcPr inner XML"; columns 2000 twips wide.
std::string Table(const std::vector<std::vector<std::string>>& rows, int cols = -1, const std::string& tblPr = "") {
  if (cols < 0) cols = static_cast<int>(rows.front().size());
  std::string xml = "<w:tbl><w:tblPr><w:tblW w:w=\"0\" w:type=\"auto\"/>" + tblPr + "</w:tblPr><w:tblGrid>";
  for (int c = 0; c < cols; ++c) xml += "<w:gridCol w:w=\"2000\"/>";
  xml += "</w:tblGrid>";
  for (const auto& row : rows) {
    xml += "<w:tr>";
    for (const std::string& cell : row) {
      size_t bar = cell.find('|');
      std::string text = cell.substr(0, bar), pr = bar == std::string::npos ? "" : cell.substr(bar + 1);
      xml += "<w:tc>" + (pr.empty() ? "" : "<w:tcPr>" + pr + "</w:tcPr>") + Para(text) + "</w:tc>";
    }
    xml += "</w:tr>";
  }
  return xml + "</w:tbl>";
}

// The main part written now, canonical.
std::string Written(const Document& doc) { return Canon(MainXml(doc)); }

// Every table in a story's blocks (and in their cells) is whole: rows with cells, cells ending
// with a paragraph, a grid as wide as its widest row, and merges that continue a cell above.
bool GridOk(const Document& doc, Node container, int depth = 0) {
  bool ok = true;
  for (const Block& b : container ? doc.Blocks(container) : doc.Blocks(doc.MainStory())) {
    if (b.kind != BlockKind::Table) continue;
    const Node tbl = b.node;
    const auto rows = doc.TableRows(tbl);
    if (rows.empty()) {
      std::printf("  a table without rows\n");
      return false;
    }
    int32_t width = 0;
    std::vector<std::vector<std::pair<int32_t, CellProps>>> grid;
    for (Node tr : rows) {
      const RowProps rp = doc.ResolvedRow(tr);
      int32_t col = rp.gridBefore;
      grid.emplace_back();
      const auto cells = doc.RowCells(tr);
      if (cells.empty()) {
        std::printf("  a row without cells\n");
        ok = false;
      }
      for (Node tc : cells) {
        CellProps cp = doc.ResolvedCell(tc);
        grid.back().emplace_back(col, cp);
        col += std::max(1, cp.gridSpan);
        auto blocks = doc.Blocks(tc);
        if (blocks.empty() || blocks.back().kind != BlockKind::Paragraph) {
          std::printf("  a cell that doesn't end with a paragraph\n");
          ok = false;
        }
        ok = GridOk(doc, tc, depth + 1) && ok;
      }
      width = std::max(width, col + rp.gridAfter);
    }
    if (static_cast<int32_t>(doc.TableGrid(tbl).size()) != width) {
      std::printf("  a grid of %zu columns for rows %d wide\n", doc.TableGrid(tbl).size(), width);
      ok = false;
    }
    for (size_t r = 0; r < grid.size(); ++r)
      for (const auto& [col, cp] : grid[r]) {
        if (cp.vMerge != VMerge::Continue) continue;
        bool above = false;
        if (r > 0)
          for (const auto& [c2, p2] : grid[r - 1])
            above = above || (c2 == col && p2.gridSpan == cp.gridSpan && p2.vMerge != VMerge::None);
        if (!above) {
          std::printf("  row %zu: a merged cell that continues nothing\n", r);
          ok = false;
        }
      }
  }
  return ok;
}
bool GridOk(const Document& doc) { return GridOk(doc, Node()); }

// Saved and opened again, the package passes the checker; the model agrees with what it writes.
bool ChecksOut(Document& doc) {
  if (!docx::WriteAllParts(doc).Ok()) return false;
  fw::pkg::MemorySink sink;
  if (!doc.Package().Save(sink).Ok()) return false;
  fw::pkg::Status st;
  auto again = test::OpenBytes(std::move(sink.bytes), st);
  if (!again) return false;
  auto report = fw::pkg::CheckPackage(*again);
  for (const auto& e : report.errors) std::printf("  check: %s\n", e.c_str());
  return report.Ok();
}
bool Consistent(const Document& doc, const Fixture& f) {
  bool ok = true;
  for (uint32_t p = 0; p < doc.Paragraphs().size(); ++p) {
    Node n = doc.ParaNode(p);
    if (n.Raw().start != p || n.TextLength() != doc.Para(p).text.size()) {
      std::printf("  paragraph %u: its w:p says %u, length %u (text %zu)\n", p, n.Raw().start, n.TextLength(),
                  doc.Para(p).text.size());
      ok = false;
    }
  }
  Fixture g = f;
  g.body.clear();
  g.documentXml = MainXml(doc);
  auto back = LoadOk(g);
  if (!back) return false;
  auto a = Main(doc), b = Main(*back);
  if (a.size() != b.size()) {
    std::printf("  %zu paragraphs, %zu read back\n", a.size(), b.size());
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i)
    if (doc.Para(a[i]).text != back->Para(b[i]).text) {
      std::printf("  paragraph %zu: \"%s\", read back \"%s\"\n", i, Text(doc, a[i]).c_str(), Text(*back, b[i]).c_str());
      ok = false;
    }
  return ok && GridOk(*back);
}

// Undoes every step, checks the original came back, redoes them all and checks the edited
// document came back too.
bool UndoesAndRedoes(Editor& ed, const Document& doc, const std::string& before) {
  const std::string after = Written(doc);
  uint32_t n = 0;
  while (ed.CanUndo() && ed.Undo()) ++n;
  bool ok = SameXml(before, Written(doc), "undone");
  while (ed.CanRedo() && ed.Redo()) --n;
  ok = SameXml(after, Written(doc), "redone") && ok;
  return ok && n == 0;
}

// The texts of a table's cells, row by row: "a,b|c,d".
std::string Cells(const Document& doc, Node tbl) {
  std::string out;
  for (Node tr : doc.TableRows(tbl)) {
    if (!out.empty()) out += "|";
    bool first = true;
    for (Node tc : doc.RowCells(tr)) {
      if (!first) out += ",";
      first = false;
      std::string text;
      for (const Block& b : doc.Blocks(tc)) {
        if (b.kind != BlockKind::Paragraph) continue;
        if (!text.empty()) text += "/";
        text += Text(doc, b.para);
      }
      out += text;
    }
  }
  return out;
}
Node FirstTable(const Document& doc) {
  for (const Block& b : doc.Blocks(doc.MainStory()))
    if (b.kind == BlockKind::Table) return b.node;
  return Node();
}
// The paragraph index of a cell's first paragraph (row r, cell c of the first table).
uint32_t CellPara(const Document& doc, size_t r, size_t c) {
  Node tc = doc.RowCells(doc.TableRows(FirstTable(doc))[r])[c];
  for (const Block& b : doc.Blocks(tc))
    if (b.kind == BlockKind::Paragraph) return b.para;
  return kNone;
}
std::vector<int32_t> GridWidths(const Document& doc, Node tbl) {
  std::vector<int32_t> w;
  for (fw::Twips t : doc.TableGrid(tbl)) w.push_back(t.Value());
  return w;
}

}  // namespace

TEST(Tables_InsertTable) {
  Fixture f;
  f.body = Para("Hello world") + Para("Last");
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const std::string before = Written(*doc);
  Editor ed(*doc);
  // Inside a paragraph: it splits, and the table goes between the halves.
  auto r = ed.InsertTable(P(0, 5), 2, 3);
  REQUIRE(r);
  Node tbl = FirstTable(*doc);
  REQUIRE(tbl);
  CHECK_EQ(doc->TableRows(tbl).size(), 2u);
  CHECK_EQ(doc->RowCells(doc->TableRows(tbl)[0]).size(), 3u);
  CHECK_EQ(doc->TableGrid(tbl).size(), 3u);
  CHECK_EQ(Cells(*doc, tbl), std::string(",,|,,"));
  auto ps = Main(*doc);
  CHECK_EQ(Text(*doc, ps.front()), std::string("Hello"));
  CHECK_EQ(Text(*doc, ps[ps.size() - 2]), std::string(" world"));
  CHECK(r.caret == P(CellPara(*doc, 0, 0), 0));
  // Its columns share the page's text width (US Letter, 1-inch margins: 9360 twips).
  int32_t sum = 0;
  for (int32_t w : GridWidths(*doc, tbl)) sum += w;
  CHECK(sum > 9300 && sum <= 9360);
  CHECK(Consistent(*doc, f) && GridOk(*doc));
  // Typing in a cell, then undo and redo of both.
  REQUIRE(ed.InsertText(r.caret, u"x"));
  CHECK_EQ(Cells(*doc, FirstTable(*doc)), std::string("x,,|,,"));
  CHECK(UndoesAndRedoes(ed, *doc, before));
  CHECK(ChecksOut(*doc));
}

TEST(Tables_InsertTablePlaces) {
  Fixture f;
  f.styles = "<w:style w:type=\"table\" w:styleId=\"TableGrid\"><w:name w:val=\"Table Grid\"/></w:style>";
  f.body = Para("One") + Para("Two");
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const std::string before = Written(*doc);
  Editor ed(*doc);
  // At a paragraph's start: before it, with the document's Table Grid style.
  REQUIRE(ed.InsertTable(P(1, 0), 1, 1));
  auto blocks = doc->Blocks(doc->MainStory());
  REQUIRE(blocks.size() == 3);
  CHECK(blocks[0].kind == BlockKind::Paragraph && blocks[1].kind == BlockKind::Table && blocks[2].kind == BlockKind::Paragraph);
  CHECK(Written(*doc).find("TableGrid") != std::string::npos);
  CHECK(Written(*doc).find("tblBorders") == std::string::npos);
  // At the end of the last paragraph: after it, and an empty paragraph after the table.
  const uint32_t last = Main(*doc).back();
  REQUIRE(ed.InsertTable(P(last, 3), 1, 2));
  blocks = doc->Blocks(doc->MainStory());
  REQUIRE(blocks.size() == 5);
  CHECK(blocks[3].kind == BlockKind::Table && blocks[4].kind == BlockKind::Paragraph && doc->Para(blocks[4].para).text.empty());
  // Right after a table: an empty paragraph keeps the two apart.
  REQUIRE(ed.InsertTable(P(blocks[4].para, 0), 1, 1));
  blocks = doc->Blocks(doc->MainStory());
  for (size_t i = 1; i < blocks.size(); ++i)
    CHECK(!(blocks[i].kind == BlockKind::Table && blocks[i - 1].kind == BlockKind::Table));
  // In a cell: a nested table, as wide as the cell.
  Node t1 = blocks[1].node;
  uint32_t cellPara = doc->Blocks(doc->RowCells(doc->TableRows(t1)[0])[0]).front().para;
  REQUIRE(ed.InsertTable(P(cellPara, 0), 2, 2));
  CHECK(Consistent(*doc, f) && GridOk(*doc));
  CHECK(UndoesAndRedoes(ed, *doc, before));
  CHECK(ChecksOut(*doc));
  // Refused: no rows, too many columns.
  CHECK(!ed.InsertTable(P(0, 0), 0, 2));
  CHECK(!ed.InsertTable(P(0, 0), 1, 64));
}

TEST(Tables_TabGoesThroughTheCells) {
  Fixture f;
  f.body = Para("Before") +
           Table({{"a|<w:vMerge w:val=\"restart\"/>", "b", "c"}, {"|<w:vMerge/>", "e|<w:gridSpan w:val=\"2\"/>"}}) +
           Para("After");
  auto doc = LoadOk(f);
  REQUIRE(doc);
  Editor ed(*doc);
  const uint32_t a = CellPara(*doc, 0, 0);
  std::vector<std::string> seen;
  for (auto cell = ed.CellAt(P(a, 0)); cell; cell = ed.NextCell(cell->start)) seen.push_back(Text(*doc, cell->start.para));
  // The merged cell's continuation isn't a stop of its own.
  CHECK_EQ(seen.size(), 4u);
  CHECK(seen == std::vector<std::string>({"a", "b", "c", "e"}));
  auto e = ed.CellAt(P(CellPara(*doc, 1, 1), 0));
  REQUIRE(e);
  CHECK_EQ(e->row, 1u);
  CHECK_EQ(e->col, 1);
  CHECK_EQ(e->span, 2);
  auto back = ed.NextCell(e->start, true);
  REQUIRE(back);
  CHECK_EQ(Text(*doc, back->start.para), std::string("c"));
  auto top = ed.CellAt(P(a, 0));
  REQUIRE(top);
  CHECK_EQ(top->rows, 2u);
  CHECK(!ed.NextCell(P(a, 0), true));
  CHECK(!ed.CellAt(P(0, 0)));
  // Blocks: from "a" to "e" is the whole table; rows and columns grow to it too.
  auto block = ed.BlockAt(P(a, 0), e->start);
  REQUIRE(block);
  CHECK(block->row0 == 0 && block->row1 == 1 && block->col0 == 0 && block->col1 == 2);
  CHECK_EQ(block->cells.size(), 4u);
  auto row = ed.Grow(P(CellPara(*doc, 0, 1), 0), P(CellPara(*doc, 0, 1), 0), Editor::Extent::Rows);
  REQUIRE(row);
  auto rb = ed.BlockAt(row->first, row->second);
  REQUIRE(rb);
  CHECK(rb->col0 == 0 && rb->col1 == 2 && rb->row0 == 0 && rb->row1 == 1);  // "a" runs into row 2
}

TEST(Tables_InsertRowsAndColumns) {
  Fixture f;
  f.body = Table({{"a", "b"}, {"c", "d"}}) + Para("");
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const std::string before = Written(*doc);
  Editor ed(*doc);
  auto r = ed.InsertRows(P(CellPara(*doc, 0, 0), 0), P(CellPara(*doc, 0, 0), 0), true);
  REQUIRE(r);
  CHECK_EQ(Cells(*doc, FirstTable(*doc)), std::string("a,b|,|c,d"));
  CHECK(r.caret == P(CellPara(*doc, 1, 0), 0));
  // Two rows selected: two new rows above.
  REQUIRE(ed.InsertRows(P(CellPara(*doc, 1, 0), 0), P(CellPara(*doc, 2, 1), 0), false));
  CHECK_EQ(Cells(*doc, FirstTable(*doc)), std::string("a,b|,|,|,|c,d"));
  // A column to the right of the first, and one to the left of the first.
  REQUIRE(ed.InsertColumns(P(CellPara(*doc, 0, 0), 0), P(CellPara(*doc, 0, 0), 0), true));
  CHECK_EQ(Cells(*doc, FirstTable(*doc)), std::string("a,,b|,,|,,|,,|c,,d"));
  REQUIRE(ed.InsertColumns(P(CellPara(*doc, 0, 0), 0), P(CellPara(*doc, 0, 0), 0), false));
  CHECK_EQ(Cells(*doc, FirstTable(*doc)), std::string(",a,,b|,,,|,,,|,,,|,c,,d"));
  CHECK_EQ(doc->TableGrid(FirstTable(*doc)).size(), 4u);
  CHECK(Consistent(*doc, f) && GridOk(*doc));
  CHECK(UndoesAndRedoes(ed, *doc, before));
  CHECK(ChecksOut(*doc));
}

TEST(Tables_NewRowsAndColumnsAroundMergedCells) {
  // A cell across both columns, and a cell merged down two rows.
  Fixture f;
  f.body = Table({{"wide|<w:gridSpan w:val=\"2\"/>", "x"}, {"m|<w:vMerge w:val=\"restart\"/>", "b", "y"},
                  {"|<w:vMerge/>", "d", "z"}},
                 3) +
           Para("");
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const std::string before = Written(*doc);
  Editor ed(*doc);
  // A column between the first two: the wide cell covers it too.
  REQUIRE(ed.InsertColumns(P(CellPara(*doc, 1, 0), 0), P(CellPara(*doc, 1, 0), 0), true));
  Node tbl = FirstTable(*doc);
  CHECK_EQ(doc->TableGrid(tbl).size(), 4u);
  CHECK_EQ(doc->ResolvedCell(doc->RowCells(doc->TableRows(tbl)[0])[0]).gridSpan, 3);
  CHECK(GridOk(*doc));
  // A row inside the merged cell: the new row's cell continues the merge.
  REQUIRE(ed.InsertRows(P(CellPara(*doc, 1, 1), 0), P(CellPara(*doc, 1, 1), 0), true));
  tbl = FirstTable(*doc);
  CHECK(doc->ResolvedCell(doc->RowCells(doc->TableRows(tbl)[2])[0]).vMerge == VMerge::Continue);
  CHECK_EQ(doc->CellRowSpan(doc->RowCells(doc->TableRows(tbl)[1])[0]), 3);
  // Below the merge's last row: no merge.
  REQUIRE(ed.InsertRows(P(CellPara(*doc, 3, 1), 0), P(CellPara(*doc, 3, 1), 0), true));
  tbl = FirstTable(*doc);
  CHECK(doc->ResolvedCell(doc->RowCells(doc->TableRows(tbl)[4])[0]).vMerge == VMerge::None);
  CHECK(Consistent(*doc, f) && GridOk(*doc));
  CHECK(UndoesAndRedoes(ed, *doc, before));
  CHECK(ChecksOut(*doc));
}

TEST(Tables_DeleteRowsAndColumns) {
  Fixture f;
  f.body = Table({{"a", "b", "c"}, {"d", "e", "f"}, {"g|<w:gridSpan w:val=\"2\"/>", "i"}}) + Para("after");
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const std::string before = Written(*doc);
  Editor ed(*doc);
  auto r = ed.DeleteRows(P(CellPara(*doc, 1, 1), 0), P(CellPara(*doc, 1, 1), 0));
  REQUIRE(r);
  CHECK_EQ(Cells(*doc, FirstTable(*doc)), std::string("a,b,c|g,i"));
  CHECK_EQ(Text(*doc, r.caret.para), std::string("g"));
  // The middle column: the wide cell narrows.
  REQUIRE(ed.DeleteColumns(P(CellPara(*doc, 0, 1), 0), P(CellPara(*doc, 0, 1), 0)));
  Node tbl = FirstTable(*doc);
  CHECK_EQ(Cells(*doc, tbl), std::string("a,c|g,i"));
  CHECK_EQ(doc->TableGrid(tbl).size(), 2u);
  CHECK_EQ(doc->ResolvedCell(doc->RowCells(doc->TableRows(tbl)[1])[0]).gridSpan, 1);
  CHECK(Consistent(*doc, f) && GridOk(*doc));
  // Every row: the table goes, the paragraph after stays.
  REQUIRE(ed.DeleteRows(P(CellPara(*doc, 0, 0), 0), P(CellPara(*doc, 1, 1), 0)));
  CHECK(!FirstTable(*doc));
  CHECK_EQ(Text(*doc, Main(*doc).front()), std::string("after"));
  CHECK(Consistent(*doc, f));
  CHECK(UndoesAndRedoes(ed, *doc, before));
  CHECK(ChecksOut(*doc));
}

TEST(Tables_DeletingKeepsRangesAndAnchors) {
  // A bookmark from the second row to the paragraph after the table, and a comment in a cell.
  Fixture f;
  f.comments = "<w:comment w:id=\"0\" w:author=\"A\"><w:p><w:r><w:t>Note</w:t></w:r></w:p></w:comment>";
  f.body = "<w:tbl><w:tblPr><w:tblW w:w=\"0\" w:type=\"auto\"/></w:tblPr><w:tblGrid><w:gridCol w:w=\"2000\"/>"
           "<w:gridCol w:w=\"2000\"/></w:tblGrid>"
           "<w:tr><w:tc>" + Para("a") + "</w:tc><w:tc>" + Para("b") + "</w:tc></w:tr>"
           "<w:tr><w:tc><w:p><w:bookmarkStart w:id=\"5\" w:name=\"Mark\"/><w:r><w:t>c</w:t></w:r></w:p></w:tc><w:tc>" +
           Para("d") + "</w:tc></w:tr>"
           "<w:tr><w:tc><w:p><w:r><w:commentReference w:id=\"0\"/></w:r></w:p></w:tc><w:tc>" + Para("f") +
           "</w:tc></w:tr></w:tbl>"
           "<w:p><w:r><w:t>after</w:t></w:r><w:bookmarkEnd w:id=\"5\"/></w:p>";
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const std::string before = Written(*doc);
  Editor ed(*doc);
  // The comment's row can't go; nothing changes.
  auto refused = ed.DeleteRows(P(CellPara(*doc, 2, 1), 0), P(CellPara(*doc, 2, 1), 0));
  CHECK(!refused && !refused.message.empty());
  CHECK(!ed.CanUndo() && Written(*doc) == before);
  // The bookmark's row goes: its start moves to the next row, its end stays.
  REQUIRE(ed.DeleteRows(P(CellPara(*doc, 1, 0), 0), P(CellPara(*doc, 1, 0), 0)));
  const std::string now = MainXml(*doc);
  CHECK(now.find("bookmarkStart") != std::string::npos && now.find("bookmarkEnd") != std::string::npos);
  CHECK(now.find("bookmarkStart") < now.find("bookmarkEnd"));
  CHECK(Consistent(*doc, f) && GridOk(*doc));
  CHECK(UndoesAndRedoes(ed, *doc, before));
  CHECK(ChecksOut(*doc));
  // The whole table: refused while the comment is in it.
  CHECK(!ed.DeleteTable(P(CellPara(*doc, 0, 0), 0)));
}

TEST(Tables_MergeAndSplit) {
  Fixture f;
  f.body = Table({{"a", "b", "c"}, {"d", "", "f"}, {"g", "h", "i"}}) + Para("");
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const std::string before = Written(*doc);
  Editor ed(*doc);
  // a b / d _ : one cell, its text in reading order (the empty cell adds nothing).
  auto r = ed.MergeCells(P(CellPara(*doc, 0, 0), 0), P(CellPara(*doc, 1, 1), 0));
  REQUIRE(r);
  Node tbl = FirstTable(*doc);
  CHECK_EQ(Cells(*doc, tbl), std::string("a/b/d,c|,f|g,h,i"));
  Node merged = doc->RowCells(doc->TableRows(tbl)[0])[0];
  CHECK_EQ(doc->ResolvedCell(merged).gridSpan, 2);
  CHECK(doc->ResolvedCell(merged).vMerge == VMerge::Restart);
  CHECK_EQ(doc->CellRowSpan(merged), 2);
  CHECK(GridOk(*doc) && Consistent(*doc, f));
  // Split back into 2 x 2: the text stays in the first.
  REQUIRE(ed.SplitCell(r.caret, 2, 2));
  tbl = FirstTable(*doc);
  CHECK_EQ(Cells(*doc, tbl), std::string("a/b/d,,c|,,f|g,h,i"));
  CHECK_EQ(doc->TableGrid(tbl).size(), 3u);
  CHECK(GridOk(*doc) && Consistent(*doc, f));
  // A single cell into three columns: the grid gains two, the other rows' cells cover them.
  REQUIRE(ed.SplitCell(P(CellPara(*doc, 2, 2), 0), 1, 3));
  tbl = FirstTable(*doc);
  CHECK_EQ(doc->TableGrid(tbl).size(), 5u);
  CHECK_EQ(doc->RowCells(doc->TableRows(tbl)[2]).size(), 5u);
  CHECK_EQ(doc->ResolvedCell(doc->RowCells(doc->TableRows(tbl)[0])[2]).gridSpan, 3);
  CHECK(GridOk(*doc) && Consistent(*doc, f));
  // A cell into two rows: a new row, which the row's other cells run down over.
  REQUIRE(ed.SplitCell(P(CellPara(*doc, 2, 0), 0), 2, 1));
  tbl = FirstTable(*doc);
  CHECK_EQ(doc->TableRows(tbl).size(), 4u);
  CHECK(doc->ResolvedCell(doc->RowCells(doc->TableRows(tbl)[3])[1]).vMerge == VMerge::Continue);
  CHECK(doc->ResolvedCell(doc->RowCells(doc->TableRows(tbl)[3])[0]).vMerge == VMerge::None);
  CHECK(GridOk(*doc) && Consistent(*doc, f));
  CHECK(UndoesAndRedoes(ed, *doc, before));
  CHECK(ChecksOut(*doc));
  // Refused: one cell, and a merged cell into rows that don't divide it.
  CHECK(!ed.MergeCells(P(CellPara(*doc, 0, 1), 0), P(CellPara(*doc, 0, 1), 0)));
}

TEST(Tables_TextToTableAndBack) {
  Fixture f;
  f.body = Para("Name\tAge") + Para("Ann\t31") + Para("Bob") + Para("End");
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const std::string before = Written(*doc);
  Editor ed(*doc);
  auto r = ed.TextToTable(0, 2);
  REQUIRE(r);
  Node tbl = FirstTable(*doc);
  REQUIRE(tbl);
  CHECK_EQ(Cells(*doc, tbl), std::string("Name,Age|Ann,31|Bob,"));
  CHECK(r.caret == P(CellPara(*doc, 0, 0), 0));
  CHECK(Consistent(*doc, f) && GridOk(*doc));
  auto back = ed.TableToText(r.caret);
  REQUIRE(back);
  CHECK(!FirstTable(*doc));
  auto ps = Main(*doc);
  REQUIRE(ps.size() == 4);
  CHECK_EQ(Text(*doc, ps[0]), std::string("Name\tAge"));
  CHECK_EQ(Text(*doc, ps[1]), std::string("Ann\t31"));
  CHECK_EQ(Text(*doc, ps[2]), std::string("Bob\t"));
  CHECK(Consistent(*doc, f));
  CHECK(UndoesAndRedoes(ed, *doc, before));
  CHECK(ChecksOut(*doc));
}

TEST(Tables_SizesAndAutoFit) {
  Fixture f;
  f.body = Table({{"a", "b"}, {"c", "d"}}) + Para("");
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const std::string before = Written(*doc);
  Editor ed(*doc);
  const Pos a = P(CellPara(*doc, 0, 0), 0);
  REQUIRE(ed.SetColumnWidth(a, a, 3000));
  CHECK(GridWidths(*doc, FirstTable(*doc)) == std::vector<int32_t>({3000, 2000}));
  CHECK_EQ(doc->ResolvedCell(doc->RowCells(doc->TableRows(FirstTable(*doc))[1])[0]).width.value, 3000);  // the cells' widths follow
  // Dragging the border between them: the columns give and take.
  REQUIRE(ed.MoveColumnBorder(a, 1, -500));
  CHECK(GridWidths(*doc, FirstTable(*doc)) == std::vector<int32_t>({2500, 2500}));
  // The right edge: the last column alone; never narrower than its margins.
  REQUIRE(ed.MoveColumnBorder(a, 2, -10000));
  CHECK(GridWidths(*doc, FirstTable(*doc)) == std::vector<int32_t>({2500, 144}));
  REQUIRE(ed.SetRowHeight(a, a, 720, "exact"));
  CHECK(doc->ResolvedRow(doc->TableRows(FirstTable(*doc))[0]).height.Value() == 720);
  CHECK(doc->ResolvedRow(doc->TableRows(FirstTable(*doc))[0]).heightRule == "exact");
  // AutoFit to the window: the text width; fixed: the widths stay; contents: as measured.
  REQUIRE(ed.AutoFit(a, Editor::Fit::Window));
  int32_t sum = 0;
  for (int32_t w : GridWidths(*doc, FirstTable(*doc))) sum += w;
  CHECK_EQ(sum, 9360);
  CHECK(doc->ResolvedTable(FirstTable(*doc)).width.type == TableWidth::Type::Pct);
  REQUIRE(ed.AutoFit(a, Editor::Fit::Fixed));
  CHECK(doc->ResolvedTable(FirstTable(*doc)).fixedLayout);
  REQUIRE(ed.AutoFit(a, Editor::Fit::Contents, {1000, 1200}));
  CHECK(GridWidths(*doc, FirstTable(*doc)) == std::vector<int32_t>({1000, 1200}));
  CHECK(!doc->ResolvedTable(FirstTable(*doc)).fixedLayout);
  CHECK(!ed.AutoFit(a, Editor::Fit::Contents, {1000}));
  CHECK(Consistent(*doc, f) && GridOk(*doc));
  CHECK(UndoesAndRedoes(ed, *doc, before));
  CHECK(ChecksOut(*doc));
}

TEST(Tables_CellRowAndTableProperties) {
  Fixture f;
  f.styles = "<w:style w:type=\"table\" w:styleId=\"Plain\"><w:name w:val=\"Plain Table 1\"/></w:style>";
  f.body = Table({{"a", "b"}, {"c", "d"}}) + Para("");
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const std::string before = Written(*doc);
  Editor ed(*doc);
  const Pos a = P(CellPara(*doc, 0, 0), 0), d = P(CellPara(*doc, 1, 1), 0);
  REQUIRE(ed.SetCellProps(a, d, PropEdit().Set("shd", {{"val", "clear"}, {"color", "auto"}, {"fill", "FFF2CC"}})));
  REQUIRE(ed.SetCellProps(a, a, PropEdit().Set("vAlign", {{"val", "center"}})));
  REQUIRE(ed.SetCellProps(a, a, PropEdit().Set("tcBorders/bottom", {{"val", "double"}, {"sz", "6"}, {"color", "auto"}})));
  REQUIRE(ed.SetCellProps(a, a, PropEdit().Set("tcMar/top", {{"w", "100"}, {"type", "dxa"}})));
  REQUIRE(ed.SetRowProps(a, a, PropEdit().Set("tblHeader")));
  REQUIRE(ed.SetTableProps(a, PropEdit().Set("tblStyle", {{"val", "Plain"}})));
  REQUIRE(ed.SetTableProps(a, PropEdit().Set("jc", {{"val", "center"}})));
  Node tbl = FirstTable(*doc);
  CellProps cp = doc->ResolvedCell(doc->RowCells(doc->TableRows(tbl)[0])[0]);
  CHECK(cp.vAlign == "center");
  CHECK(cp.bottom.style == BorderStyle::Double);
  CHECK_EQ(cp.shading.fill.rgb & 0xFFFFFF, 0xFFF2CCu);
  CHECK(doc->ResolvedRow(doc->TableRows(tbl)[0]).header);
  CHECK(doc->ResolvedTable(tbl).style == "Plain");
  CHECK(Consistent(*doc, f) && GridOk(*doc));
  CHECK(UndoesAndRedoes(ed, *doc, before));
  CHECK(ChecksOut(*doc));
  // Values XML can't hold, and properties that don't exist, are refused.
  CHECK(!ed.SetCellProps(a, a, PropEdit().Set("vAlign", {{"val", "\x01"}})));
  CHECK(!ed.SetCellProps(a, a, PropEdit().Set("noSuchThing")));
}

TEST(Tables_ClearAndDelete) {
  Fixture f;
  f.body = Para("x") + Table({{"a", "b"}, {"c", "d"}}) + Table({{"t"}}) + Para("y");
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const std::string before = Written(*doc);
  Editor ed(*doc);
  REQUIRE(ed.ClearCells(P(CellPara(*doc, 0, 1), 0), P(CellPara(*doc, 1, 1), 0)));
  CHECK_EQ(Cells(*doc, FirstTable(*doc)), std::string("a,|c,"));
  // Between two tables: an empty paragraph takes its place, so they stay apart.
  REQUIRE(ed.DeleteTable(P(CellPara(*doc, 0, 0), 0)));
  auto blocks = doc->Blocks(doc->MainStory());
  REQUIRE(blocks.size() == 4);
  CHECK(blocks[1].kind == BlockKind::Paragraph && blocks[2].kind == BlockKind::Table);
  CHECK(Consistent(*doc, f) && GridOk(*doc));
  CHECK(UndoesAndRedoes(ed, *doc, before));
  CHECK(ChecksOut(*doc));
}

TEST(Tables_NestedTablesAreTheirOwn) {
  Fixture f;
  f.body = "<w:tbl><w:tblPr><w:tblW w:w=\"0\" w:type=\"auto\"/></w:tblPr><w:tblGrid><w:gridCol w:w=\"4000\"/>"
           "<w:gridCol w:w=\"4000\"/></w:tblGrid><w:tr><w:tc>" + Table({{"i", "j"}}) + Para("") + "</w:tc><w:tc>" +
           Para("o") + "</w:tc></w:tr></w:tbl>" + Para("");
  auto doc = LoadOk(f);
  REQUIRE(doc);
  const std::string before = Written(*doc);
  Editor ed(*doc);
  const uint32_t i = Main(*doc)[0];
  CHECK_EQ(Text(*doc, i), std::string("i"));
  REQUIRE(ed.InsertRows(P(i, 0), P(i, 0), true));
  REQUIRE(ed.DeleteColumns(P(i, 0), P(i, 0)));
  // The outer table is as it was: one row, two cells.
  Node outer = FirstTable(*doc);
  CHECK_EQ(doc->TableRows(outer).size(), 1u);
  CHECK_EQ(doc->RowCells(doc->TableRows(outer)[0]).size(), 2u);
  // Deleting the inner table: the cell keeps its paragraph.
  REQUIRE(ed.DeleteTable(P(Main(*doc)[0], 0)));
  CHECK_EQ(Cells(*doc, FirstTable(*doc)), std::string(",o"));
  CHECK(Consistent(*doc, f) && GridOk(*doc));
  CHECK(UndoesAndRedoes(ed, *doc, before));
  CHECK(ChecksOut(*doc));
}

TEST(Tables_RefusedShapesChangeNothing) {
  const std::string grid = "<w:tblGrid><w:gridCol w:w=\"2000\"/><w:gridCol w:w=\"2000\"/></w:tblGrid>";
  const std::vector<std::string> bodies = {
      // A row in a content control.
      "<w:tbl><w:tblPr/>" + grid + "<w:sdt><w:sdtContent><w:tr><w:tc>" + Para("a") + "</w:tc><w:tc>" + Para("b") +
          "</w:tc></w:tr></w:sdtContent></w:sdt></w:tbl>",
      // A tracked cell insertion.
      "<w:tbl><w:tblPr/>" + grid + "<w:tr><w:tc><w:tcPr><w:cellIns w:id=\"1\" w:author=\"A\"/></w:tcPr>" + Para("a") +
          "</w:tc><w:tc>" + Para("b") + "</w:tc></w:tr></w:tbl>",
      // An old-style horizontal merge.
      "<w:tbl><w:tblPr/>" + grid + "<w:tr><w:tc><w:tcPr><w:hMerge w:val=\"restart\"/></w:tcPr>" + Para("a") +
          "</w:tc><w:tc><w:tcPr><w:hMerge/></w:tcPr>" + Para("b") + "</w:tc></w:tr></w:tbl>",
  };
  for (const std::string& body : bodies) {
    Fixture f;
    f.body = body + Para("");
    auto doc = LoadOk(f);
    REQUIRE(doc);
    const std::string before = Written(*doc);
    Editor ed(*doc);
    const Pos a = P(Main(*doc)[0], 0);
    for (auto r : {ed.InsertRows(a, a, true), ed.InsertColumns(a, a, true), ed.DeleteRows(a, a), ed.MergeCells(a, P(Main(*doc)[1], 0)),
                   ed.SplitCell(a, 1, 2)}) {
      CHECK(!r);
      CHECK(!r.message.empty());
    }
    CHECK(!ed.CanUndo() && Written(*doc) == before);
    // Typing in its cells still works.
    CHECK(ed.InsertText(a, u"x"));
  }
}
