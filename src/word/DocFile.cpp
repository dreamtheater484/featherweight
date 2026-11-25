#include "word/DocFile.h"

#include <windows.h>
#include <shlobj.h>

#include <vector>

#include "core/Lang.h"
#include "core/Strings.h"
#include "doc/docx/Save.h"
#include "doc/model/Document.h"

namespace pkg = fw::pkg;

std::wstring DocFile::SuggestedPath(const fw::doc::Document& doc) const {
  if (GetOrigin() == Origin::File) return Path();
  if (GetOrigin() == Origin::Copy) return SuggestedBase();
  // As Word does: the first words of the document, without what a file name can't hold.
  std::wstring name;
  for (uint32_t p : doc.StoryParagraphs(doc.MainStory())) {
    for (char16_t c : doc.Para(p).text) {
      if (name.size() >= 40) break;
      const bool ok = c > 0x20 && c != 0xFFFC && !(c >= 0xE000 && c <= 0xF8FF) && !wcschr(L"\\/:*?\"<>|", c);
      if (ok) name += static_cast<wchar_t>(c);
      else if (!name.empty() && name.back() != L' ') name += L' ';  // one space for what can't be there
    }
    name = fw::Trim(name);
    if (!name.empty()) break;
  }
  if (const size_t space = name.rfind(L' '); name.size() >= 40 && space != std::wstring::npos && space > 10)
    name.erase(space);  // a whole word
  while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) name.pop_back();
  std::wstring dir;
  PWSTR docs = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
    dir = std::wstring(docs) + L"\\";
    CoTaskMemFree(docs);
  }
  return dir + (name.empty() ? Title() : name) + L".docx";
}

bool DocFile::Save(fw::doc::Document& doc, const std::wstring& path, std::wstring& error) {
  SaveJob job;
  if (!BeginSave(path, job, error)) return false;
  fw::doc::docx::SaveOptions options;
  options.changedParts.assign(ChangedParts().begin(), ChangedParts().end());
  options.edited = Dirty() || GetOrigin() == Origin::New;
  options.author = fw::WideToUtf8(Config().user);
  std::vector<uint8_t> bytes;
  pkg::Status st = fw::doc::docx::Serialize(doc, options, bytes);
  if (!st.Ok()) {
    error = job.name + T(L" couldn't be saved. ") + fw::Utf8ToWide(st.message);
    return false;
  }
  pkg::SaveResult saved = WriteSave(job, std::move(bytes), &doc.Package().GetSnapshot());
  if (!saved.Ok()) {
    error = fw::Utf8ToWide(saved.message);
    return false;
  }
  SaveDone(job, saved);
  return true;
}

void DocFile::Autosave(fw::doc::Document& doc) {
  if (!AutosaveDue()) return;
  fw::doc::docx::SaveOptions options;
  options.changedParts.assign(ChangedParts().begin(), ChangedParts().end());
  options.edited = true;
  options.author = fw::WideToUtf8(Config().user);
  std::vector<uint8_t> bytes;
  if (!fw::doc::docx::Serialize(doc, options, bytes).Ok()) return;
  WriteAutosave(std::move(bytes));
}
