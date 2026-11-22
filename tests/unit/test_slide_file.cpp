// The presentation tab's file (src/slide/view/PresentationFile, P2 p2-model): saving in place on a
// worker thread with the original kept in the backups, and the recovery journal replayed.
#include <windows.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "PkgTestUtil.h"
#include "TestHarness.h"
#include "pkg/Package.h"
#include "pkg/Recovery.h"
#include "pkg/SafeFile.h"
#include "pkg/Snapshot.h"
#include "slide/Presentation.h"
#include "slide/edit/Edit.h"
#include "slide/edit/Journal.h"
#include "slide/view/PresentationFile.h"

using namespace fw::slide;

namespace {

struct Folder {
  std::wstring path = test::TempPath(L"slidefile");
  Folder() {
    CreateDirectoryW(path.c_str(), nullptr);
    PresentationFile::Config() = {path + L"\\Backups", path + L"\\Recovery", L"Tester", L"en-US", false};
  }
  ~Folder() {
    PresentationFile::RecoverySession().End();  // the next test's folder starts its own
    PresentationFile::Config() = {};
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  size_t Count(const std::wstring& sub) const {
    size_t n = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(path + L"\\" + sub, ec)) n += e.is_regular_file();
    return n;
  }
};

std::unique_ptr<Presentation> OpenFile(const std::wstring& path, fw::pkg::Snapshot::Stamp* stamp = nullptr) {
  fw::pkg::Status st;
  auto snap = fw::pkg::Snapshot::Open(path, {}, st);
  if (snap && stamp) *stamp = snap->GetStamp();
  auto pk = snap ? fw::pkg::Package::Open(std::move(snap), fw::pkg::Limits::Default(), st) : nullptr;
  return pk ? Read(std::move(pk), st) : nullptr;
}

std::wstring Generated(const wchar_t* name) {
  return (std::filesystem::path(__FILE__).parent_path().parent_path() / "office" / "generated" / name).wstring();
}

}  // namespace

TEST(SlideFileSavesInPlaceWithBackupAndJournal) {
  Folder folder;
  std::wstring file = folder.path + L"\\deck.pptx";
  CHECK(CopyFileW(Generated(L"layouts.pptx").c_str(), file.c_str(), FALSE));
  fw::pkg::Snapshot::Stamp stamp;
  auto deck = OpenFile(file, &stamp);
  CHECK(deck != nullptr);
  if (!deck) return;
  size_t count = deck->SlideCount();
  edit::Editor ed(*deck);
  PresentationFile pf;
  pf.Opened(file, stamp);
  ed.SetJournal(pf.Journal());
  ed.OnChange([&](const edit::Changes&) { pf.Changed(ed); });
  CHECK(ed.NewSlide(0, 0));
  CHECK(pf.Dirty());
  // The autosave: a journal in the recovery folder that brings the file as opened to the edits.
  pf.Autosave(ed);
  CHECK_EQ(folder.Count(L"Recovery") >= 1, true);
  // Saved in place: the original goes to the backups, the file has the new slide.
  std::wstring error;
  CHECK(pf.Save(ed, file, error));
  CHECK(!ed.Modified());
  CHECK(!pf.Dirty());
  CHECK_EQ(folder.Count(L"Backups"), size_t{1});
  auto again = OpenFile(file);
  CHECK(again != nullptr);
  CHECK_EQ(again->SlideCount(), count + 1);
}

// What App::RecoverDocuments does after a crash: the recovery file read back is a presentation's
// journal ("pptx"), and its records replayed on the deck as it was opened bring back every edit,
// across autosaves (each appends only what's new).
TEST(SlideFileJournalRecoversEdits) {
  Folder folder;
  std::wstring file = folder.path + L"\\deck.pptx";
  CHECK(CopyFileW(Generated(L"layouts.pptx").c_str(), file.c_str(), FALSE));
  fw::pkg::Snapshot::Stamp stamp;
  auto deck = OpenFile(file, &stamp);
  CHECK(deck != nullptr);
  if (!deck) return;
  const size_t count = deck->SlideCount();
  edit::Editor ed(*deck);
  PresentationFile pf;
  pf.Opened(file, stamp);
  ed.SetJournal(pf.Journal());
  ed.OnChange([&](const edit::Changes&) { pf.Changed(ed); });
  CHECK(ed.NewSlide(0, 0));
  pf.Autosave(ed);
  uint32_t box = 0;
  CHECK(ed.InsertTextBox(0, {72, 72, 360, 144}, L"Back after a crash", &box));
  pf.Autosave(ed);
  // The recovery folder holds one journal for the deck.
  fw::pkg::recovery::Entry entry;
  std::vector<uint8_t> bytes, records;
  size_t journals = 0;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(folder.path + L"\\Recovery", ec)) {
    fw::pkg::recovery::Entry en;
    std::vector<uint8_t> b, r;
    if (e.is_regular_file() && fw::pkg::recovery::ReadJournal(e.path().wstring(), en, b, r)) {
      ++journals;
      entry = en;
      bytes = std::move(b);
      records = std::move(r);
    }
  }
  CHECK_EQ(journals, size_t{1});
  CHECK(entry.journal && entry.meta == "pptx");
  CHECK(entry.originalPath == file);
  // The deck as opened, its edits made again as one step.
  fw::pkg::Status st;
  auto pk = fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(std::move(bytes)), fw::pkg::Limits::Default(), st);
  auto recovered = pk ? Read(std::move(pk), st) : nullptr;
  CHECK(recovered != nullptr);
  if (!recovered) return;
  CHECK_EQ(recovered->SlideCount(), count);
  edit::Editor red(*recovered);
  CHECK(edit::Replay(red, std::string_view(reinterpret_cast<const char*>(records.data()), records.size())));
  CHECK_EQ(recovered->SlideCount(), count + 1);
  CHECK(red.ShapeXml(0, box).find("Back after a crash") != std::string::npos);
  CHECK(red.Modified());
}
