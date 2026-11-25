// Password-protected presentations (P3, p3-review; plan §8.2): an encrypted .pptx (agile or standard
// encryption, [MS-OFFCRYPTO]) opens behind the password card through pkg/OfficeCrypto, as documents
// and workbooks do; FileSession saves it encrypted with the same key. File > Info > Protect
// Presentation > Encrypt with Password sets or removes the password the next save writes.

#include <cstdio>

#include "Commands.h"
#include "PasswordCard.h"
#include "core/Lang.h"
#include "core/Strings.h"
#include "pkg/CompoundFile.h"
#include "pkg/OfficeCrypto.h"
#include "slide/view/PresentationComments.h"
#include "slide/view/PresentationFile.h"
#include "ui/Theme.h"

using namespace gfx;

namespace fw::slide::view {

namespace {

// Whether the file starts as a compound file does (an encrypted package, or a .ppt).
bool CompoundOnDisk(const std::wstring& path) {
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
  uint8_t head[8] = {};
  const size_t n = std::fread(head, 1, sizeof head, f);
  std::fclose(f);
  return n == sizeof head && pkg::CompoundFile::HasSignature(std::span<const uint8_t>(head, n));
}

}  // namespace

// Only an encrypted package is taken: another compound file (a .ppt) goes on to the importers.
bool PresentationView::TakeProtected() {
  if (newDeck_) return false;
  auto encrypted = [](std::span<const uint8_t> bytes) {
    if (!pkg::CompoundFile::HasSignature(bytes)) return false;
    auto cf = pkg::CompoundFile::Open(bytes);
    return cf && pkg::crypto::IsEncryptedPackage(*cf);
  };
  if (fromBytes_) {
    if (!encrypted(bytes_)) return false;
    protected_ = std::move(bytes_);  // a recovered protected deck: its autosave, encrypted
    bytes_.clear();
    return true;
  }
  if (!CompoundOnDisk(path_)) return false;
  pkg::Status st;
  std::unique_ptr<pkg::Snapshot> snap = pkg::Snapshot::Open(path_, {}, st);
  if (!snap || !encrypted(snap->Bytes())) return false;
  const auto bytes = snap->Bytes();
  protected_.assign(bytes.begin(), bytes.end());
  protectedStamp_ = snap->GetStamp();
  return true;
}

OpenResult PresentationView::Unlock(const std::wstring& password, std::vector<uint8_t>& plain, pkg::crypto::Key& key,
                                    std::wstring& error) {
  namespace crypto = pkg::crypto;
  const std::wstring name = Title();
  auto cf = pkg::CompoundFile::Open(protected_);
  if (!cf || !crypto::IsEncryptedPackage(*cf)) {
    error = name + T(L" couldn't be opened — it isn't a presentation Featherweight can open, or it is damaged.");
    return OpenResult::Failed;
  }
  if (password.empty()) {
    needPassword_ = true;
    return OpenResult::NeedPassword;
  }
  switch (crypto::Decrypt(*cf, password, plain, key)) {
    case crypto::Result::Ok:
      if (!plain.empty()) break;
      [[fallthrough]];  // an empty package
    case crypto::Result::Damaged:
      error = name + T(L" couldn't be opened — it seems to be damaged.");
      return OpenResult::Failed;
    case crypto::Result::WrongPassword:
      needPassword_ = true;
      return OpenResult::WrongPassword;
    case crypto::Result::Unsupported:
      error = name + T(L" couldn't be opened — it's protected in a way Featherweight doesn't support.");
      return OpenResult::Failed;
  }
  needPassword_ = false;
  return OpenResult::Ok;
}

PasswordCard& PresentationView::Card() {
  if (!card_) card_ = std::make_unique<PasswordCard>(host_, [this](const std::wstring& p) { SubmitPassword(p); });
  return *card_;
}

TextBox* PresentationView::PasswordBox() { return needPassword_ ? &Card().Box() : nullptr; }

void PresentationView::SubmitPassword(const std::wstring& password) {
  if (password.empty()) {
    Card().Wrong();
    host_.ViewInvalidate();
    return;
  }
  std::wstring error;
  const OpenResult r = Load(password, error);
  if (r == OpenResult::Ok) {
    Card().Accepted();
    host_.ViewPasswordAccepted(this);
  } else if (r == OpenResult::WrongPassword || r == OpenResult::NeedPassword) {
    Card().Wrong();
  } else {
    needPassword_ = true;  // the card stays, with why
    Card().Failed(error);
  }
  host_.ViewInvalidate();
}

// ---------------------------------------------------------------------------
// Encrypt with Password

bool PresentationView::ProtectCommandState(int cmd, CommandState& s) {
  if (cmd != CmdEncryptPassword) return false;
  s.label = T(L"Encrypt with password…");
  s.enabled = Ready() && SlideEditor() != nullptr;
  s.checked = file_ && file_->Protected();
  return true;
}

bool PresentationView::ProtectCommand(int cmd) {
  if (cmd != CmdEncryptPassword || !Ready() || !SlideEditor()) return false;
  if (!passwordPanel_) passwordPanel_ = std::make_unique<PasswordPanel>(*this);
  passwordPanel_->Open();
  host_.ViewShowPanel(passwordPanel_.get());
  return true;
}

bool PresentationView::SetFilePassword(const std::wstring& password) {
  if (!Ready() || !SlideEditor()) return false;
  // A new key for the password (empty: none); the tab is unsaved until the next save writes it so.
  File().Protect(password.empty() ? pkg::crypto::Key() : pkg::crypto::NewKey(password));
  File().ProtectionChanged();
  host_.ViewStateChanged();
  return true;
}

PresentationView::PasswordPanel::PasswordPanel(PresentationView& view) : v_(view) {
  for (Box* b : {&password_, &confirm_}) {
    b->SetPassword(true);
    b->autoComplete = false;
    b->onEnter = [this] { Encrypt(); };
  }
  password_.label = T(L"Password");
  confirm_.label = T(L"Reenter password");
  encrypt_.text = T(L"Encrypt");
  encrypt_.onInvoke = [this] { Encrypt(); };
  remove_.text = T(L"Remove password");
  remove_.onInvoke = [this] { Remove(); };
}

void PresentationView::PasswordPanel::Open() {
  password_.ReplaceText(L"");
  confirm_.ReplaceText(L"");
  note_ = v_.file_ && v_.file_->Protected() ? T(L"A password is needed to open this presentation.") : L"";
  focusWanted_ = true;
}

std::vector<Control*> PresentationView::PasswordPanel::Layout(const D2D1_RECT_F& body, float ui) {
  ui_ = ui;
  labels_.clear();
  std::vector<Control*> out;
  const float left = body.left + 16 * ui, right = body.right - 16 * ui, gap = 6 * ui, rowH = 30 * ui;
  float y = body.top + 10 * ui;
  labels_.push_back({T(L"Encrypt the contents of this file"), RectF(left, y, right, y + 16 * ui)});
  y += 22 * ui;
  for (Box* b : {&password_, &confirm_}) {
    labels_.push_back({b->label, RectF(left, y, right, y + 16 * ui)});
    y += 20 * ui;
    b->rc = RectF(left, y, right, y + rowH);
    out.push_back(b);
    y += rowH + 8 * ui;
  }
  const float mid = (left + right) / 2;
  encrypt_.rc = RectF(left, y, mid - gap / 2, y + rowH);
  remove_.rc = RectF(mid + gap / 2, y, right, y + rowH);
  remove_.enabled = v_.file_ && v_.file_->Protected();
  out.push_back(&encrypt_);
  out.push_back(&remove_);
  y += rowH + 10 * ui;
  noteRc_ = RectF(left, y, right, y + 40 * ui);
  y += 46 * ui;
  // PowerPoint's caution.
  labels_.push_back({T(L"Caution: if you lose or forget the password, it can't be recovered. Keep passwords and "
                     L"their file names in a safe place. Passwords are case-sensitive."),
                     RectF(left, y, right, y + 64 * ui)});
  return out;
}

void PresentationView::PasswordPanel::Draw(Canvas& c) {
  if (focusWanted_) {
    password_.TakeFocus();
    focusWanted_ = false;
  }
  const Palette& p = theme::P();
  for (const auto& [text, rc] : labels_) c.TextWrapped(text, rc, p.textSecondary, 12 * ui_, 4);
  if (!note_.empty()) c.TextWrapped(note_, noteRc_, p.text, 12 * ui_, 3);
}

void PresentationView::PasswordPanel::Encrypt() {
  const std::wstring pw = password_.Text();
  if (pw.empty()) {
    note_ = T(L"Type a password.");
  } else if (pw != confirm_.Text()) {
    note_ = T(L"The passwords you typed don't match.");  // PowerPoint's "Reenter password" check
  } else if (!v_.SlideEditor()) {
    note_ = T(L"This presentation can't be protected.");
  } else {
    v_.SetFilePassword(pw);  // saving writes it encrypted
    password_.ReplaceText(L"");
    confirm_.ReplaceText(L"");
    note_ = T(L"Encrypted: the next save needs this password to open the file.");
    v_.host_.ViewStateChanged();
  }
  v_.host_.ViewInvalidate();
}

void PresentationView::PasswordPanel::Remove() {
  if (!v_.file_ || !v_.file_->Protected()) return;
  v_.SetFilePassword(L"");
  note_ = T(L"The password is removed: the next save opens without one.");
  v_.host_.ViewInvalidate();
}

}  // namespace fw::slide::view
