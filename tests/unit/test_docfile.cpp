// The file behind a document tab (D2 "Files"): Save keeps the original once in the backups,
// writes Word's owner file only while there are unsaved edits, refuses to replace a file another
// program changed or holds, and recovery autosaves go with a clean close.
#include <windows.h>

#include <string>
#include <vector>

#include "PkgTestUtil.h"
#include "TestHarness.h"
#include "core/Strings.h"
#include "doc/docx/Blank.h"
#include "doc/docx/Docx.h"
#include "doc/edit/Editor.h"
#include "pkg/Check.h"
#include "pkg/Backups.h"
#include "pkg/OwnerFile.h"
#include "word/DocFile.h"

namespace {

struct Folder {
  std::wstring path = test::TempPath(L"docfile");
  Folder() { CreateDirectoryW(path.c_str(), nullptr); }
  ~Folder() { Remove(path); }
  static void Remove(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        std::wstring name = fd.cFileName, p = dir + L"\\" + name;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
          Remove(p);
        } else {
          SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
          DeleteFileW(p.c_str());
        }
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
  }
  size_t Count(const std::wstring& sub, const wchar_t* pattern = L"*") const {
    size_t n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((path + L"\\" + sub + L"\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do n += !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
  }
};

std::vector<uint8_t> ReadAll(const std::wstring& path) {
  std::vector<uint8_t> out(1 << 20);
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  DWORD got = 0;
  if (h != INVALID_HANDLE_VALUE) {
    ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
    CloseHandle(h);
  }
  out.resize(got);
  return out;
}

std::unique_ptr<fw::doc::Document> Open(const std::wstring& path) {
  fw::pkg::Status st;
  auto snap = fw::pkg::Snapshot::Open(path, {}, st);
  auto pkg = snap ? fw::pkg::Package::Open(std::move(snap), fw::pkg::Limits::Default(), st) : nullptr;
  return pkg ? fw::doc::docx::Read(std::move(pkg), st) : nullptr;
}

uint32_t MainPart(const fw::doc::Document& doc) {
  return static_cast<uint32_t>(doc.FindPart(fw::doc::PartKind::Document) - doc.Parts().data());
}

void Configure(const Folder& f) {
  DocFile::Config() = {f.path + L"\\Backups", f.path + L"\\Recovery", L"Tester", L"en-US", false};
}

}  // namespace

TEST(DocFile_SaveKeepsTheOriginalOnceAndTellsWord) {
  Folder f;
  Configure(f);
  const std::wstring path = f.path + L"\\report.docx";
  const auto original = fw::doc::docx::BlankDocx({"en-US", false, "Someone", 0});
  CHECK(test::WriteFileBytes(path, original));
  auto doc = Open(path);
  CHECK(doc);
  if (!doc) return;
  {
    DocFile file;
    file.Opened(path, doc->Package().GetSnapshot().GetStamp());
    CHECK(file.InUseMessage().empty());
    CHECK(file.Title() == L"report.docx");
    CHECK(!file.Dirty());

    // The first edit tells Word users the file is in use; saving makes it clean again.
    file.Edited(MainPart(*doc));
    CHECK(file.Dirty());
    CHECK(fw::pkg::ReadOwner(path) == L"Tester");
    std::wstring error;
    CHECK(file.Save(*doc, path, error));
    CHECK(!file.Dirty());
    CHECK(fw::pkg::ReadOwner(path).empty());
    CHECK(ReadAll(path) != original);
    // The original, as it was opened, is in the backups.
    const std::wstring backup = fw::pkg::backups::Latest(DocFile::Config().backups, path);
    CHECK(!backup.empty() && ReadAll(backup) == original);
    // A second save replaces our own file: no second backup, and nothing to notice on disk.
    file.Edited(MainPart(*doc));
    CHECK(file.Save(*doc, path, error));
    CHECK_EQ(f.Count(L"Backups"), 1u);
    CHECK(file.CheckDisk() == DocFile::DiskChange::None);

    // Another program changes it: noticed once, and Save refuses to mix the two versions.
    Sleep(20);
    CHECK(test::WriteFileBytes(path, test::Bytes("someone else's version")));
    CHECK(file.CheckDisk() == DocFile::DiskChange::Changed);
    CHECK(file.CheckDisk() == DocFile::DiskChange::None);
    file.Edited(MainPart(*doc));
    CHECK(!file.Save(*doc, path, error));
    CHECK(error.find(L"changed by another program") != std::wstring::npos);
    CHECK(test::Text(ReadAll(path)) == "someone else's version");
    // Save As goes elsewhere, and the tab is that file from then on. The file it replaces is kept.
    const std::wstring copy = f.path + L"\\copy.docx";
    CHECK(test::WriteFileBytes(copy, test::Bytes("an older copy")));
    CHECK(file.Save(*doc, copy, error));
    CHECK(fw::EqualsNoCase(file.Path(), copy));
    CHECK_EQ(f.Count(L"Backups"), 2u);
    CHECK(test::Text(ReadAll(fw::pkg::backups::Latest(DocFile::Config().backups, copy))) == "an older copy");
    CHECK(fw::pkg::ReadOwner(path).empty());  // ours went with the edits
    auto reopened = Open(copy);
    CHECK(reopened && reopened->Paragraphs().size() == 1);
    // Deleted: noticed.
    DeleteFileW(copy.c_str());
    CHECK(file.CheckDisk() == DocFile::DiskChange::Missing);
  }
}

TEST(DocFile_InUseByWordGoesToSaveAs) {
  Folder f;
  Configure(f);
  const std::wstring path = f.path + L"\\Agreement.docx";
  CHECK(test::WriteFileBytes(path, fw::doc::docx::BlankDocx()));
  auto doc = Open(path);
  CHECK(doc);
  if (!doc) return;
  CHECK(fw::pkg::WriteOwner(path, L"Ann"));
  HANDLE word = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  {
    DocFile file;
    file.Opened(path, doc->Package().GetSnapshot().GetStamp());
    CHECK(file.InUseMessage().find(L"in use by Ann") != std::wstring::npos);
    file.Edited(MainPart(*doc));
    CHECK(fw::pkg::ReadOwner(path) == L"Ann");  // Word's owner file stays Word's
    std::wstring error;
    CHECK(!file.Save(*doc, path, error));
    CHECK(error.find(L"in use by Ann") != std::wstring::npos);
    CHECK(file.Save(*doc, f.path + L"\\Agreement (mine).docx", error));
  }
  CloseHandle(word);
  CHECK(fw::pkg::ReadOwner(path) == L"Ann");
}

TEST(DocFile_AutosaveGoesWithACleanClose) {
  Folder f;
  Configure(f);
  const std::wstring path = f.path + L"\\notes.docx";
  CHECK(test::WriteFileBytes(path, fw::doc::docx::BlankDocx()));
  auto doc = Open(path);
  CHECK(doc);
  if (!doc) return;
  {
    DocFile file;
    file.Opened(path, doc->Package().GetSnapshot().GetStamp());
    file.Autosave(*doc);  // nothing unsaved: nothing written
    CHECK_EQ(f.Count(L"Recovery", L"*.fwr"), 0u);
    file.Edited(MainPart(*doc));
    file.Autosave(*doc);
    CHECK_EQ(f.Count(L"Recovery", L"*.fwr"), 1u);
    // This session is running: its autosaves aren't offered to another.
    CHECK(fw::pkg::recovery::FindOrphans(DocFile::Config().recovery).empty());
  }
  CHECK_EQ(f.Count(L"Recovery", L"*.fwr"), 0u);
  DocFile::RecoverySession().End();
}

TEST(DocFile_UndoBackToTheSavedStateIsClean) {
  Folder f;
  Configure(f);
  const std::wstring path = f.path + L"\\letter.docx";
  CHECK(test::WriteFileBytes(path, fw::doc::docx::BlankDocx()));
  auto doc = Open(path);
  CHECK(doc);
  if (!doc) return;
  DocFile file;
  file.Opened(path, doc->Package().GetSnapshot().GetStamp());
  const uint32_t main = MainPart(*doc);
  file.Changed(true, {main});  // typing
  CHECK(file.Dirty());
  CHECK(fw::pkg::ReadOwner(path) == L"Tester");
  file.Autosave(*doc);
  CHECK_EQ(f.Count(L"Recovery", L"*.fwr"), 1u);
  file.Changed(false, {main});  // undone back to the file as opened
  CHECK(!file.Dirty());
  CHECK(fw::pkg::ReadOwner(path).empty());
  CHECK_EQ(f.Count(L"Recovery", L"*.fwr"), 0u);
  file.Changed(true, {main});  // redone
  CHECK(file.Dirty());
  std::wstring error;
  CHECK(file.Save(*doc, path, error));
  CHECK(!file.Dirty());
  DocFile::RecoverySession().End();
}

TEST(DocFile_TheEditorDrivesModifiedSaveAndUndo) {
  Folder f;
  Configure(f);
  const std::wstring path = f.path + L"\\memo.docx";
  CHECK(test::WriteFileBytes(path, fw::doc::docx::BlankDocx()));
  auto doc = Open(path);
  CHECK(doc);
  if (!doc) return;
  DocFile file;
  file.Opened(path, doc->Package().GetSnapshot().GetStamp());
  fw::doc::edit::Editor ed(*doc);
  ed.OnChange([&](const fw::doc::edit::Change&) { file.Changed(ed.Modified(), ed.DirtyParts()); });
  auto firstText = [](const std::wstring& p) {
    auto d = Open(p);
    return d ? d->Para(d->StoryParagraphs(d->MainStory()).front()).text : std::u16string(u"(unreadable)");
  };
  const uint32_t para = doc->StoryParagraphs(doc->MainStory()).front();

  CHECK(ed.InsertText({para, 0}, u"Hello"));
  CHECK(file.Dirty());
  CHECK(fw::pkg::ReadOwner(path) == L"Tester");
  std::wstring error;
  CHECK(file.Save(*doc, path, error));
  ed.MarkSaved();
  CHECK(firstText(path) == u"Hello");

  // Undone after the save: the model is back to the original, so the editor lists no changed
  // parts, but the file holds "Hello". Saving must write the model, not keep that version.
  CHECK(ed.Undo());
  CHECK(file.Dirty());
  CHECK(ed.DirtyParts().empty());
  CHECK(file.Save(*doc, path, error));
  ed.MarkSaved();
  CHECK(firstText(path).empty());

  // Redo, then undo back to what was saved: clean again, and our owner file goes.
  CHECK(ed.Redo());
  CHECK(file.Dirty());
  CHECK(fw::pkg::ReadOwner(path) == L"Tester");
  CHECK(ed.Undo());
  CHECK(!file.Dirty());
  CHECK(fw::pkg::ReadOwner(path).empty());
  auto saved = Open(path);
  CHECK(saved && fw::pkg::CheckPackage(saved->Package()).Ok());
}

TEST(DocFile_NewAndRecoveredDocumentsSuggestAName) {
  const std::string body =
      "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body>"
      "<w:p/><w:p><w:r><w:t xml:space=\"preserve\">  Quarterly report: Q3/Q4 </w:t></w:r></w:p></w:body></w:document>";
  auto bytes = test::MakeZip({
      {"[Content_Types].xml",
       "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"><Default Extension=\"rels\" "
       "ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/><Override PartName=\"/word/document.xml\" "
       "ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/></Types>"},
      {"_rels/.rels",
       "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" "
       "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" "
       "Target=\"word/document.xml\"/></Relationships>"},
      {"word/document.xml", body},
  });
  fw::pkg::Status st;
  auto doc = fw::doc::docx::Read(test::OpenBytes(bytes, st), st);
  CHECK(doc);
  if (!doc) return;
  DocFile created;
  created.New(3);
  CHECK(created.Title() == L"Document 3");
  CHECK(fw::FileNameOf(created.SuggestedPath(*doc)) == L"Quarterly report Q3 Q4.docx");
  auto blank = fw::doc::docx::Read(test::OpenBytes(fw::doc::docx::BlankDocx(), st), st);
  CHECK(blank && fw::FileNameOf(created.SuggestedPath(*blank)) == L"Document 3.docx");
  DocFile recovered;
  recovered.Copy(L"Plan (recovered).docx", L"D:\\Docs\\Plan.docx", true);
  CHECK(recovered.Dirty());
  CHECK(recovered.Title() == L"Plan (recovered).docx");
  CHECK(recovered.SuggestedPath(*doc) == L"D:\\Docs\\Plan.docx");
}
