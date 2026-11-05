#include "word/DocFile.h"

#include <windows.h>
#include <shlobj.h>

#include <vector>

#include "Util.h"
#include "core/Strings.h"
#include "doc/docx/Save.h"
#include "doc/model/Document.h"
#include "pkg/Backups.h"
#include "pkg/SafeFile.h"

namespace pkg = fw::pkg;

DocFile::Setup& DocFile::Config() {
  static Setup setup;
  return setup;
}

pkg::recovery::Session& DocFile::RecoverySession() {
  static pkg::recovery::Session session;
  return session;
}

DocFile::~DocFile() { Clean(); }

void DocFile::Opened(const std::wstring& path, const pkg::Snapshot::Stamp& stamp) {
  origin_ = Origin::File;
  path_ = path;
  stamp_ = stamp;
  // Word, or someone on another PC, has it open: Save will go to Save As, and our owner file
  // must not replace theirs.
  inUse_ = pkg::CheckInUse(path_, Config().user);
}

void DocFile::New(int number) {
  origin_ = Origin::New;
  number_ = number;
  backedUp_ = true;  // nothing to back up
}

void DocFile::Copy(std::wstring title, std::wstring suggested, bool dirty) {
  origin_ = Origin::Copy;
  title_ = std::move(title);
  suggested_ = std::move(suggested);
  backedUp_ = true;
  dirty_ = dirty;
  if (dirty) ++generation_;
}

std::wstring DocFile::Title() const {
  switch (origin_) {
    case Origin::File: return fw::FileNameOf(path_);
    case Origin::New: return L"Document " + std::to_wstring(number_);
    case Origin::Copy: return title_;
  }
  return {};
}

std::wstring DocFile::SuggestedPath(const fw::doc::Document& doc) const {
  if (origin_ == Origin::File) return path_;
  if (origin_ == Origin::Copy) return suggested_;
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

void DocFile::Edited(uint32_t part) {
  changed_.insert(part);
  ++generation_;
  if (dirty_) return;
  dirty_ = true;
  if (origin_ == Origin::File && !inUse_.locked && !Config().user.empty())
    ownerWritten_ = pkg::WriteOwner(path_, Config().user);
}

bool DocFile::Save(fw::doc::Document& doc, const std::wstring& path, std::wstring& error) {
  const std::wstring target = util::FullPath(path);
  const std::wstring name = fw::FileNameOf(target);
  const bool sameFile = origin_ == Origin::File && fw::EqualsNoCase(target, path_);
  if (sameFile) {
    // Two versions are never mixed (plan §3): the file must be as we last read or wrote it.
    if (pkg::CheckDisk(path_, stamp_) != pkg::Snapshot::DiskState::Unchanged) {
      error = name + L" was changed by another program after you opened it.";
      return false;
    }
    inUse_ = pkg::CheckInUse(path_, Config().user);
    if (inUse_.locked) {
      error = inUse_.user.empty() ? name + L" is open in another program, so it can't be replaced."
                                  : name + L" is in use by " + inUse_.user + L", so it can't be replaced.";
      return false;
    }
  }

  fw::doc::docx::SaveOptions options;
  options.changedParts.assign(changed_.begin(), changed_.end());
  options.edited = dirty_ || origin_ == Origin::New;
  options.author = fw::WideToUtf8(Config().user);
  std::vector<uint8_t> bytes;
  pkg::Status st = fw::doc::docx::Serialize(doc, options, bytes);
  if (!st.Ok()) {
    error = name + L" couldn't be saved. " + fw::Utf8ToWide(st.message);
    return false;
  }
  // The first time Featherweight replaces a file, the original is kept for 30 days (§13). It is
  // the snapshot, which CheckDisk just found identical to the file.
  if (sameFile && !backedUp_ && !Config().backups.empty()) {
    const pkg::Snapshot& snapshot = doc.Package().GetSnapshot();
    std::vector<uint8_t> original(static_cast<size_t>(snapshot.Size()));
    if (snapshot.ReadAt(0, original.data(), original.size()))
      backedUp_ = !pkg::backups::Keep(Config().backups, path_, original).empty();
  }
  pkg::SaveResult saved = pkg::SafeWrite(target, bytes);
  if (!saved.Ok()) {
    error = fw::Utf8ToWide(saved.message);
    return false;
  }
  if (ownerWritten_) pkg::RemoveOwner(path_, Config().user);  // clean now, or a file we left (Save As)
  ownerWritten_ = false;
  origin_ = Origin::File;
  path_ = target;
  stamp_ = saved.stamp;
  backedUp_ = true;  // later saves replace our own file
  inUse_ = {};
  dirty_ = false;
  changed_.clear();  // the package keeps what was written
  Clean();
  return true;
}

void DocFile::Autosave(fw::doc::Document& doc) {
  if (!dirty_ || generation_ == autosaved_ || Config().recovery.empty()) return;
  pkg::recovery::Session& session = RecoverySession();
  if (!session.Started() && !session.Start(Config().recovery)) return;
  if (autosave_.empty()) autosave_ = session.NewFile();
  fw::doc::docx::SaveOptions options;
  options.changedParts.assign(changed_.begin(), changed_.end());
  options.edited = true;
  options.author = fw::WideToUtf8(Config().user);
  std::vector<uint8_t> bytes;
  const std::wstring where = origin_ == Origin::File ? path_ : suggested_;
  if (fw::doc::docx::Serialize(doc, options, bytes).Ok() && pkg::recovery::Write(autosave_, where, Title(), bytes))
    autosaved_ = generation_;
}

std::wstring DocFile::InUseMessage() const {
  if (!inUse_.locked) return {};
  const std::wstring name = Title();
  return (inUse_.user.empty() ? name + L" is open in another program." : name + L" is in use by " + inUse_.user + L".") +
         L" Changes you make here can be saved as a copy.";
}

DocFile::DiskChange DocFile::CheckDisk() {
  if (origin_ != Origin::File || path_.empty() || pkg::SameSizeAndTime(path_, stamp_)) return DiskChange::None;
  WIN32_FILE_ATTRIBUTE_DATA fa{};
  const bool exists = GetFileAttributesExW(path_.c_str(), GetFileExInfoStandard, &fa) != FALSE;
  const uint64_t size = exists ? (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow : 0;
  const int64_t time = exists ? static_cast<int64_t>((static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) |
                                                     fa.ftLastWriteTime.dwLowDateTime)
                              : -1;
  if (size == notifiedSize_ && time == notifiedTime_) return DiskChange::None;  // told already
  const pkg::Snapshot::DiskState state = pkg::CheckDisk(path_, stamp_);
  if (state == pkg::Snapshot::DiskState::Unchanged) {
    pkg::StampOf(path_, stamp_.hash, stamp_);  // only its time changed: the content is ours
    return DiskChange::None;
  }
  notifiedSize_ = size;
  notifiedTime_ = time;
  return state == pkg::Snapshot::DiskState::Missing ? DiskChange::Missing : DiskChange::Changed;
}

void DocFile::Clean() {
  if (ownerWritten_) pkg::RemoveOwner(path_, Config().user);
  ownerWritten_ = false;
  if (!autosave_.empty()) pkg::recovery::Remove(autosave_);
  autosave_.clear();
  autosaved_ = generation_;
}
