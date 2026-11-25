#include "FileSession.h"

#include <windows.h>

#include "Util.h"
#include "core/Lang.h"
#include "core/Strings.h"
#include "pkg/Backups.h"

namespace pkg = fw::pkg;

FileSession::Setup& FileSession::Config() {
  static Setup setup;
  return setup;
}

pkg::recovery::Session& FileSession::RecoverySession() {
  static pkg::recovery::Session session;
  return session;
}

FileSession::~FileSession() { Clean(); }

void FileSession::Opened(const std::wstring& path, const pkg::Snapshot::Stamp& stamp) {
  origin_ = Origin::File;
  path_ = path;
  stamp_ = stamp;
  // Word or Excel, or someone on another PC, has it open: Save will go to Save As, and our owner
  // file must not replace theirs.
  inUse_ = pkg::CheckInUse(path_, Config().user, owner_);
}

void FileSession::New(int number) {
  origin_ = Origin::New;
  number_ = number;
  backedUp_ = true;  // nothing to back up
}

void FileSession::Copy(std::wstring title, std::wstring suggested, bool dirty) {
  origin_ = Origin::Copy;
  title_ = std::move(title);
  suggested_ = std::move(suggested);
  backedUp_ = true;
  dirty_ = dirty;
  if (dirty) ++generation_;
}

std::wstring FileSession::Title() const {
  switch (origin_) {
    case Origin::File: return fw::FileNameOf(path_);
    case Origin::New: return Fmt(newName_.c_str(), {std::to_wstring(number_)});
    case Origin::Copy: return title_;
  }
  return {};
}

void FileSession::WriteOwnerFile() {
  if (origin_ == Origin::File && !inUse_.locked && !Config().user.empty())
    ownerWritten_ = pkg::WriteOwner(path_, Config().user, owner_);
}

void FileSession::Edited(uint32_t part) {
  changed_.insert(part);
  ++generation_;
  if (dirty_) return;
  dirty_ = true;
  WriteOwnerFile();
}

void FileSession::Changed(bool modified, const std::vector<uint32_t>& parts) {
  changed_ = std::set<uint32_t>(parts.begin(), parts.end());  // exact, under undo too
  ++generation_;
  if (modified) {
    // Modified with no part listed is an undo past a save: the model is back to the original,
    // but the file isn't.
    if (!dirty_) {
      dirty_ = true;
      WriteOwnerFile();
    }
    return;
  }
  if (!dirty_ || origin_ == Origin::Copy) return;  // a recovered document stays unsaved
  dirty_ = false;  // back where it was opened or saved
  Clean();
}

bool FileSession::BeginSave(const std::wstring& path, SaveJob& job, std::wstring& error) {
  job = SaveJob{};
  job.target = util::FullPath(path);
  job.name = fw::FileNameOf(job.target);
  const bool sameFile = origin_ == Origin::File && fw::EqualsNoCase(job.target, path_);
  if (sameFile) {
    // Two versions are never mixed (plan §3): the file must be as we last read or wrote it.
    if (pkg::CheckDisk(path_, stamp_) != pkg::Snapshot::DiskState::Unchanged) {
      error = Fmt(T(L"{0} was changed by another program after you opened it."), {job.name});
      return false;
    }
    inUse_ = pkg::CheckInUse(path_, Config().user, owner_);
    if (inUse_.locked) {
      error = inUse_.user.empty() ? Fmt(T(L"{0} is open in another program, so it can't be replaced."), {job.name})
                                  : Fmt(T(L"{0} is in use by {1}, so it can't be replaced."), {job.name, inUse_.user});
      return false;
    }
  }
  // The first time Featherweight replaces a file, the original is kept for 30 days (§13); Save As
  // over another file (a recovered document over its original, say) keeps that one too.
  job.backups = Config().backups;
  job.opened = path_;
  job.keepOriginal = sameFile && !backedUp_ && !job.backups.empty();
  job.keepExisting = !sameFile && !job.backups.empty() && GetFileAttributesW(job.target.c_str()) != INVALID_FILE_ATTRIBUTES;
  job.key = Protected() ? &key_ : nullptr;
  return true;
}

pkg::SaveResult FileSession::WriteSave(const SaveJob& job, std::vector<uint8_t> bytes, const pkg::Snapshot* opened) {
  if (job.key) {  // with the password it was opened with, as Word does
    bytes = pkg::crypto::Encrypt(bytes, *job.key);
    if (bytes.empty()) {
      pkg::SaveResult failed;
      failed.failure = pkg::SaveFailure::Other;
      failed.message = fw::WideToUtf8(Fmt(T(L"{0} couldn't be saved — Windows couldn't encrypt it."), {job.name}));
      return failed;
    }
  }
  // The original is the snapshot, which BeginSave found identical to the file. A protected
  // document's snapshot is the decrypted package, so the file itself is kept, still encrypted.
  if (job.keepOriginal) {
    std::vector<uint8_t> original;
    std::string file;
    if (job.key || !opened) {
      if (util::ReadFileBytes(job.opened, file, 1u << 30)) original.assign(file.begin(), file.end());
    } else {
      original.resize(static_cast<size_t>(opened->Size()));
      if (!opened->ReadAt(0, original.data(), original.size())) original.clear();
    }
    if (!original.empty()) pkg::backups::Keep(job.backups, job.opened, original);
  } else if (job.keepExisting) {
    std::string existing;
    if (util::ReadFileBytes(job.target, existing, 1u << 30))
      pkg::backups::Keep(job.backups, job.target, std::vector<uint8_t>(existing.begin(), existing.end()));
  }
  return pkg::SafeWrite(job.target, bytes);
}

void FileSession::SaveDone(const SaveJob& job, const pkg::SaveResult& saved) {
  if (ownerWritten_) pkg::RemoveOwner(path_, Config().user, owner_);  // clean now, or a file we left (Save As)
  ownerWritten_ = false;
  origin_ = Origin::File;
  path_ = job.target;
  stamp_ = saved.stamp;
  backedUp_ = true;  // later saves replace our own file
  inUse_ = {};
  dirty_ = false;
  changed_.clear();  // the document keeps what was written
  Clean();
}

bool FileSession::AutosaveDue() const { return dirty_ && generation_ != autosaved_ && !Config().recovery.empty(); }

void FileSession::WriteRecovery(std::span<const uint8_t> bytes) {
  pkg::recovery::Session& session = RecoverySession();
  if (!session.Started() && !session.Start(Config().recovery)) return;
  if (autosave_.empty()) autosave_ = session.NewFile();
  const std::wstring where = origin_ == Origin::File ? path_ : suggested_;
  journal_ = false;  // replaced by a whole autosave
  if (!bytes.empty() && pkg::recovery::Write(autosave_, where, Title(), bytes)) autosaved_ = generation_;
}

bool FileSession::WriteJournal(std::span<const uint8_t> opened, std::string_view meta, std::string_view all,
                               std::string_view unwritten) {
  if (!AutosaveDue() || Protected()) return false;
  pkg::recovery::Session& session = RecoverySession();
  if (!session.Started() && !session.Start(Config().recovery)) return false;
  if (autosave_.empty()) {
    autosave_ = session.NewFile();
    journal_ = false;
  }
  auto bytes = [](std::string_view s) { return std::span(reinterpret_cast<const uint8_t*>(s.data()), s.size()); };
  bool ok;
  if (!journal_) {
    const std::wstring where = origin_ == Origin::File ? path_ : suggested_;
    ok = pkg::recovery::StartJournal(autosave_, where, Title(), opened, meta) &&
         pkg::recovery::AppendJournal(autosave_, bytes(all));
  } else {
    ok = pkg::recovery::AppendJournal(autosave_, bytes(unwritten));
  }
  journal_ = ok;  // a piece that failed may be cut short in the file: the next one starts it again
  if (ok) autosaved_ = generation_;
  return ok;
}

void FileSession::WriteAutosave(std::vector<uint8_t> document) {
  if (!AutosaveDue()) return;
  if (Protected()) document = pkg::crypto::Encrypt(document, key_);  // never in clear, even for this Windows user
  WriteRecovery(document);
}

void FileSession::AutosaveProtected(std::span<const uint8_t> file) {
  if (!AutosaveDue()) return;
  WriteRecovery(file);
}

std::wstring FileSession::InUseMessage() const {
  if (!inUse_.locked) return {};
  const std::wstring name = Title();
  return (inUse_.user.empty() ? Fmt(T(L"{0} is open in another program."), {name})
                              : Fmt(T(L"{0} is in use by {1}."), {name, inUse_.user})) +
         T(L" Changes you make here can be saved as a copy.");
}

FileSession::DiskChange FileSession::CheckDisk() {
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

void FileSession::Clean() {
  if (ownerWritten_) pkg::RemoveOwner(path_, Config().user, owner_);
  ownerWritten_ = false;
  if (!autosave_.empty()) pkg::recovery::Remove(autosave_);
  autosave_.clear();
  journal_ = false;
  autosaved_ = generation_;
}
