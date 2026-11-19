// Page setup (S3, s3-print): Editor::SetPageSetup and SetPageBreaks, saved as Excel 2021 writes a
// sheet set up in its Page Setup (measured: scratchpad page_probe.py), read back, undone, replayed.
#include "TestHarness.h"

#include <string>
#include <vector>

#include "SheetTestUtil.h"
#include "sheet/edit/Edit.h"
#include "sheet/edit/Journal.h"
#include "sheet/model/Workbook.h"
#include "sheet/numfmt/NumLocale.h"
#include "sheet/view/CellText.h"
#include "sheet/view/Grid.h"
#include "sheet/view/PrintLayout.h"
#include "sheet/xlsx/Xlsx.h"
#include "text/Fonts.h"

using namespace fw::sheet;
using edit::Editor;
using namespace sheet_test;

namespace {

std::vector<uint8_t> SavePage(Workbook& book, Editor& ed) {
  ed.BeginSave();
  xlsx::SavePlan plan;
  fw::pkg::Status st = xlsx::PrepareSave(book, ed, plan, book.Format());
  CHECK(st.Ok());
  fw::pkg::MemorySink sink;
  if (st.Ok()) st = xlsx::WriteSave(book, plan, sink);
  CHECK(st.Ok());
  if (st.Ok()) xlsx::CommitSave(book, ed, plan);
  else ed.EndSave();
  return sink.bytes;
}

std::string PartText(const std::vector<uint8_t>& zip, const std::string& part) {
  fw::pkg::Status st;
  auto pkg = test::OpenBytes(zip, st);
  std::vector<uint8_t> out;
  if (!pkg || !pkg->ReadPart(part, out).Ok()) return {};
  return test::Text(out);
}

bool Has(const std::string& text, const std::string& what) {
  if (text.find(what) != std::string::npos) return true;
  std::printf("    missing: %s\n", what.c_str());
  return false;
}

// Sheet1 as Excel's probe set it up.
void SetUp(PageSetup& p, HeaderFooter& h) {
  p.orientation = Orientation::Landscape;
  p.paperSize = 9;
  p.fitToWidth = 1;
  p.fitToHeight = 0;
  p.margins = {0.5, 0.4, 1, 0.9, 0.2, 0.25};
  p.horizontalCentered = p.verticalCentered = p.gridLines = p.headings = true;
  p.pageOrder = PageOrder::OverThenDown;
  p.useFirstPageNumber = true;
  p.firstPageNumber = 3;
  h.oddHeader = "&C&A";
  h.oddFooter = "&RPage &P of &N";
}

}  // namespace

TEST(sheet_page_setup_save_reopen_undo) {
  // The file's pageSetup keeps what the dialog doesn't set (the printer's resolution).
  auto parts = WorkbookParts({"<sheetData><row r=\"1\"><c r=\"A1\"><v>1</v></c></row></sheetData>"
                              "<pageMargins left=\"0.7\" right=\"0.7\" top=\"0.75\" bottom=\"0.75\" header=\"0.3\" footer=\"0.3\"/>"
                              "<pageSetup orientation=\"portrait\" horizontalDpi=\"360\" verticalDpi=\"360\"/>"});
  auto book = OpenZip(test::MakeZip(parts));
  if (!book) return;
  Editor ed(*book);
  PageSetup p = book->GetSheet(0).Page();
  HeaderFooter h;
  SetUp(p, h);
  CHECK(ed.SetPageSetup(0, p, h, true));
  CHECK(ed.UndoName() == u"Page Setup");
  CHECK(ed.SetPageSetup(0, p, h, true) && ed.UndoName() == u"Page Setup");  // the same again: no step
  std::vector<PageBreak> rows(1), cols(1);
  rows[0] = {10, 0, 16383, true, false};
  cols[0] = {3, 0, 1048575, true, false};
  CHECK(ed.SetPageBreaks(0, rows, cols));
  const std::string sheet = PartText(SavePage(*book, ed), "/xl/worksheets/sheet1.xml");
  CHECK(Has(sheet, "<sheetPr><pageSetUpPr fitToPage=\"1\"/></sheetPr>"));
  CHECK(Has(sheet, "<printOptions horizontalCentered=\"1\" verticalCentered=\"1\" headings=\"1\" gridLines=\"1\"/>"
                   "<pageMargins left=\"0.5\" right=\"0.4\" top=\"1\" bottom=\"0.9\" header=\"0.2\" footer=\"0.25\"/>"
                   "<pageSetup paperSize=\"9\" firstPageNumber=\"3\" fitToHeight=\"0\" pageOrder=\"overThenDown\" "
                   "orientation=\"landscape\" useFirstPageNumber=\"1\" horizontalDpi=\"360\" verticalDpi=\"360\"/>"
                   "<headerFooter><oddHeader>&amp;C&amp;A</oddHeader><oddFooter>&amp;RPage &amp;P of &amp;N</oddFooter></headerFooter>"
                   "<rowBreaks count=\"1\" manualBreakCount=\"1\"><brk id=\"10\" max=\"16383\" man=\"1\"/></rowBreaks>"
                   "<colBreaks count=\"1\" manualBreakCount=\"1\"><brk id=\"3\" max=\"1048575\" man=\"1\"/></colBreaks>"));
  // Read back as set.
  auto again = OpenZip(SavePage(*book, ed));
  if (again) {
    const Sheet& s = again->GetSheet(0);
    CHECK(s.Props().fitToPage && s.Page().orientation == Orientation::Landscape && s.Page().paperSize == 9);
    CHECK(s.Page().fitToHeight == 0 && s.Page().firstPageNumber == 3 && s.Page().margins.bottom == 0.9);
    CHECK(s.HeadersFooters().oddFooter == "&RPage &P of &N" && s.RowBreaks().size() == 1 && s.ColBreaks()[0].id == 3);
  }
  // Undone: as the file had it.
  CHECK(ed.Undo(2));
  const Sheet& s = book->GetSheet(0);
  CHECK(!s.Props().fitToPage && s.Page().orientation == Orientation::Portrait && s.RowBreaks().empty());
  const std::string before = PartText(SavePage(*book, ed), "/xl/worksheets/sheet1.xml");
  CHECK(Has(before, "<pageSetup orientation=\"portrait\" horizontalDpi=\"360\" verticalDpi=\"360\"/>"));
  CHECK(before.find("printOptions") == std::string::npos && before.find("headerFooter") == std::string::npos &&
        before.find("sheetPr") == std::string::npos && before.find("rowBreaks") == std::string::npos);
  // Out of Excel's range: refused.
  PageSetup bad = p;
  bad.scale = 5;
  CHECK(!ed.SetPageSetup(0, bad, h, false));
  CHECK(!ed.SetPageBreaks(0, {PageBreak{0}}, {}));
}

TEST(sheet_page_setup_new_sheet_and_journal) {
  auto parts = WorkbookParts({"<sheetData/>"});
  const std::vector<uint8_t> zip = test::MakeZip(parts);
  auto book = OpenZip(zip);
  if (!book) return;
  Editor ed(*book);
  edit::Journal journal;
  ed.SetJournal(&journal);
  // A sheet added since opening gets its page setup when saved.
  CHECK(ed.AddSheet(1));
  PageSetup p;
  HeaderFooter h;
  p.scale = 85;
  h.differentFirst = true;
  h.firstHeader = "&CFirst";
  CHECK(ed.SetPageSetup(1, p, h, false));
  CHECK(ed.SetPageBreaks(1, {PageBreak{5, 0, 16383, true, false}}, {}));
  // Replayed on the file as opened: the same.
  auto replayBook = OpenZip(zip);
  if (!replayBook) return;
  Editor replayed(*replayBook);
  size_t applied = 0;
  const edit::Result r = replayed.Replay(journal.All(), &applied);
  CHECK(r && applied >= 3);  // AddSheet records the active sheet too
  CHECK(replayBook->SheetCount() == 2 && replayBook->GetSheet(1).Page().scale == 85 &&
        replayBook->GetSheet(1).HeadersFooters().firstHeader == "&CFirst" && replayBook->GetSheet(1).RowBreaks().size() == 1);
  // Saved (after the replay: a save starts the journal afresh).
  const std::string sheet2 = PartText(SavePage(*book, ed), "/xl/worksheets/sheet2.xml");
  CHECK(Has(sheet2, "<pageSetup scale=\"85\"/><headerFooter differentFirst=\"1\"><firstHeader>&amp;CFirst</firstHeader></headerFooter>"
                    "<rowBreaks count=\"1\" manualBreakCount=\"1\"><brk id=\"5\" max=\"16383\" man=\"1\"/></rowBreaks>"));
}

// Printed as Excel 2021 printed the same setup (s3-print's page_run.ps1): Fit to 1 page wide by
// automatic keeps the manual row break, and page numbers start at the first page number.
TEST(sheet_page_fit_width_keeps_row_breaks) {
  std::string data = "<sheetData>";
  for (int r = 1; r <= 60; ++r)
    data += "<row r=\"" + std::to_string(r) + "\"><c r=\"A" + std::to_string(r) + "\"><v>" + std::to_string(r) +
            "</v></c><c r=\"P" + std::to_string(r) + "\"><v>1</v></c></row>";
  data += "</sheetData>";
  auto book = OpenZip(test::MakeZip(WorkbookParts({data})));
  if (!book) return;
  Editor ed(*book);
  PageSetup p;
  p.fitToWidth = 1;
  p.fitToHeight = 0;
  p.useFirstPageNumber = true;
  p.firstPageNumber = 3;
  CHECK(ed.SetPageSetup(0, p, HeaderFooter{}, true));
  CHECK(ed.SetPageBreaks(0, {PageBreak{10, 0, 16383, true, false}}, {}));
  static fw::text::Fonts fonts;
  view::FontMeasure measure(fonts);
  NumLocale locale;
  view::CellText text(*book, fonts, measure, locale);
  const Sheet& s = book->GetSheet(0);
  view::Grid grid;
  grid.Build(s, book->GetStyles(), measure);
  view::PrintLayout layout(*book, s, 0, grid, text, view::PrintInfo{});
  CHECK(layout.PageCount() == 2);  // one page wide (A:P), the row break kept
  if (layout.PageCount() == 2) {
    CHECK(layout.Page(0).body.lastRow == 9 && layout.Page(1).body.firstRow == 10);
    CHECK(layout.Page(0).body.lastCol == 15 && layout.Page(0).number == 3 && layout.Page(1).number == 4);
  }
  // Fit to 1 by 1: the break doesn't count, as in Excel.
  p.fitToHeight = 1;
  CHECK(ed.SetPageSetup(0, p, HeaderFooter{}, true));
  view::PrintLayout fitted(*book, s, 0, grid, text, view::PrintInfo{});
  CHECK(fitted.PageCount() == 1);
}

TEST(sheet_page_break_preview_saved) {
  auto book = OpenZip(test::MakeZip(WorkbookParts({"<sheetViews><sheetView workbookViewId=\"0\"/></sheetViews><sheetData/>"})));
  if (!book) return;
  Editor ed(*book);
  ed.SetViewMode(0, ViewMode::PageBreakPreview);
  const std::vector<uint8_t> zip = SavePage(*book, ed);
  CHECK(Has(PartText(zip, "/xl/worksheets/sheet1.xml"), "view=\"pageBreakPreview\""));
  auto again = OpenZip(zip);
  CHECK(again && again->GetSheet(0).Views()[0].view == ViewMode::PageBreakPreview);
  ed.SetViewMode(0, ViewMode::Normal);
  CHECK(PartText(SavePage(*book, ed), "/xl/worksheets/sheet1.xml").find("pageBreakPreview") == std::string::npos);
}
