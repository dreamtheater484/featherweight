#include <set>
#include <string>

#include "TestHarness.h"
#include "core/FileKinds.h"
#include "core/Strings.h"

using namespace fw;

TEST(FileKinds_Table) {
  auto kinds = FileKinds();
  CHECK_EQ(kinds.size(), static_cast<size_t>(kFileKindCount));
  const wchar_t* progIds[] = {L"Featherweight.PDF", L"Featherweight.Document", L"Featherweight.Workbook",
                              L"Featherweight.Presentation"};
  for (int i = 0; i < kFileKindCount; ++i) {
    CHECK(kinds[i].kind == static_cast<FileKind>(i));
    CHECK(std::wstring(kinds[i].progId) == progIds[i]);
    CHECK_EQ(kinds[i].iconIndex, i);  // the app icon for PDFs, then the exe's file icons in order
    CHECK(!kinds[i].extensions.empty());
  }
  CHECK(std::wstring(Info(FileKind::Pdf).typeName) == L"PDF Document");
  CHECK(std::wstring(Info(FileKind::Document).extensions[0]) == L".docx");
  CHECK(std::wstring(Info(FileKind::Workbook).extensions[0]) == L".xlsx");
  CHECK(std::wstring(Info(FileKind::Presentation).extensions[0]) == L".pptx");
  // Every extension belongs to one kind only, and is lowercase with its dot.
  std::set<std::wstring> seen;
  for (const FileKindInfo& k : kinds) {
    for (auto list : {k.extensions, k.later}) {
      for (const wchar_t* ext : list) {
        CHECK(ext[0] == L'.');
        CHECK(ToLower(ext) == ext);
        CHECK(seen.insert(ext).second);
      }
    }
  }
}

TEST(FileKinds_KindOfPath) {
  CHECK(KindOfPath(L"C:\\Docs\\Lease.PDF") == FileKind::Pdf);
  CHECK(KindOfPath(L"C:\\Docs\\Report Q3.docx") == FileKind::Document);
  CHECK(KindOfPath(L"notes.TXT") == FileKind::Document);
  CHECK(KindOfPath(L"letter.rtf") == FileKind::Document);
  CHECK(KindOfPath(L"old.doc") == FileKind::Document);  // imported (D5)
  CHECK(KindOfPath(L"Budget.xlsm") == FileKind::Workbook);
  CHECK(KindOfPath(L"export.csv") == FileKind::Workbook);
  CHECK(KindOfPath(L"Deck.ppsx") == FileKind::Presentation);
  CHECK(KindOfPath(L"slides.odp") == FileKind::Presentation);
  CHECK(KindOfPath(L"photo.png") == FileKind::Unknown);
  CHECK(KindOfPath(L"C:\\Docs\\Lease.pdf.bak") == FileKind::Unknown);
  CHECK(KindOfPath(L"C:\\Docs\\pdf") == FileKind::Unknown);
  CHECK(KindOfPath(L"") == FileKind::Unknown);
}

TEST(FileKinds_ThePdfReaderAndTheDocumentEditorHaveShipped) {
  // Update this when a viewer or an editor ships: file associations and the start screen follow.
  CHECK(ShippedViewers() == (std::vector<FileKind>{FileKind::Pdf, FileKind::Document}));
  CHECK(EditorShipped(FileKind::Document) && AnyEditorShipped());
  CHECK(!EditorShipped(FileKind::Workbook) && !EditorShipped(FileKind::Presentation));
  CHECK(ViewerShipped(FileKind::Pdf));
  CHECK(ViewerShipped(FileKind::Document));
  CHECK(!ViewerShipped(FileKind::Unknown));
  CHECK(!EditorShipped(FileKind::Unknown));
  // What the viewers open: not the formats that come later.
  CHECK(Opens(L"C:\\Docs\\Lease.PDF"));
  CHECK(Opens(L"Report.docx") && Opens(L"Template.DOTM") && Opens(L"Notes.odt") && Opens(L"Form.OTT"));
  CHECK(!Opens(L"notes.txt") && !Opens(L"letter.rtf") && Opens(L"old.doc"));  // .doc is imported (D5)
  CHECK(!Opens(L"Budget.xlsx") && !Opens(L"photo.png"));
}

TEST(FileKinds_EditorsForTesting) {
  SetEditorShippedForTesting(FileKind::Workbook, true);
  CHECK(EditorShipped(FileKind::Workbook));
  CHECK(AnyEditorShipped());
  CHECK(!ViewerShipped(FileKind::Workbook));  // file associations don't follow the testing switch
  CHECK(ShippedViewers() == (std::vector<FileKind>{FileKind::Pdf, FileKind::Document}));
  SetEditorShippedForTesting(FileKind::Pdf, true);  // PDFs are read, not created
  CHECK(!EditorShipped(FileKind::Pdf));
  SetEditorShippedForTesting(FileKind::Workbook, false);
  CHECK(!EditorShipped(FileKind::Workbook));
  CHECK(EditorShipped(FileKind::Document));  // shipped, not only for testing
}
