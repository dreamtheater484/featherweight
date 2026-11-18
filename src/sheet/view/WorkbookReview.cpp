// The workbook tab's review features (S3, s3-review): Protect Sheet and Protect Workbook, and how a
// protected sheet behaves in the grid. Links, notes and comments come here too. Every change goes
// through the edit API (sheet/edit/Protection.h has Excel's rules).
#include <algorithm>
#include <iterator>

#include "Commands.h"
#include "core/Strings.h"
#include "pkg/Formula.h"
#include "sheet/edit/Protection.h"
#include "sheet/view/WorkbookFormat.h"
#include "sheet/view/WorkbookPanels.h"
#include "sheet/view/WorkbookView.h"
#include "ui/Menu.h"
#include "ui/Theme.h"

using namespace gfx;

namespace fw::sheet::view {

namespace {

// Excel's words (Excel 2021).
constexpr wchar_t kWrongPassword[] =
    L"The password you supplied is not correct. Verify that the CAPS LOCK key is off and be sure to use the correct "
    L"capitalization.";
constexpr wchar_t kNotIdentical[] = L"Confirmation password is not identical.";

// Protect Sheet's list, in the order of Excel's dialog: what everyone may still do. Each is a
// SheetProtection flag that protects when set.
struct Permission {
  const wchar_t* label;
  bool edit::SheetProtection::*flag;
};
constexpr Permission kPermissions[] = {
    {L"Select locked cells", &edit::SheetProtection::selectLockedCells},
    {L"Select unlocked cells", &edit::SheetProtection::selectUnlockedCells},
    {L"Format cells", &edit::SheetProtection::formatCells},
    {L"Format columns", &edit::SheetProtection::formatColumns},
    {L"Format rows", &edit::SheetProtection::formatRows},
    {L"Insert columns", &edit::SheetProtection::insertColumns},
    {L"Insert rows", &edit::SheetProtection::insertRows},
    {L"Insert links", &edit::SheetProtection::insertHyperlinks},
    {L"Delete columns", &edit::SheetProtection::deleteColumns},
    {L"Delete rows", &edit::SheetProtection::deleteRows},
    {L"Sort", &edit::SheetProtection::sort},
    {L"Use AutoFilter", &edit::SheetProtection::autoFilter},
    {L"Use PivotTables", &edit::SheetProtection::pivotTables},
    {L"Edit objects", &edit::SheetProtection::objects},
    {L"Edit scenarios", &edit::SheetProtection::scenarios},
};
constexpr size_t kPermissionCount = std::size(kPermissions);

}  // namespace

// Protect Sheet, Unprotect Sheet (asking for the password), Protect Workbook and Unprotect Workbook.
class WorkbookView::ProtectPanel : public PanelContent {
 public:
  enum Mode { kProtectSheet, kUnprotectSheet, kProtectBook, kUnprotectBook };

  explicit ProtectPanel(WorkbookView& view) : v_(view) {
    for (FocusComboBox* box : {&password_, &confirm_}) {
      box->SetPassword(true);
      box->autoComplete = false;
      box->onEnter = [this] { Apply(); };
    }
    password_.label = L"Password";
    confirm_.label = L"Confirm password";
    for (size_t i = 0; i < kPermissionCount; ++i) {
      allow_[i].toggle = true;
      allow_[i].text = kPermissions[i].label;
      allow_[i].textSize = 12;
      allow_[i].onInvoke = [this, i] { allow_[i].checked = !allow_[i].checked; v_.host_.ViewInvalidate(); };
    }
    structure_.toggle = true;
    structure_.text = L"Structure";
    structure_.onInvoke = [this] { structure_.checked = !structure_.checked; v_.host_.ViewInvalidate(); };
    apply_.onInvoke = [this] { Apply(); };
  }

  std::wstring Title() const override {
    switch (mode_) {
      case kProtectSheet: return L"Protect sheet";
      case kUnprotectSheet: return L"Unprotect sheet";
      case kProtectBook: return L"Protect workbook";
      case kUnprotectBook: return L"Unprotect workbook";
    }
    return {};
  }

  void Open(Mode mode) {
    mode_ = mode;
    note_.clear();
    password_.SetText(L"");
    confirm_.SetText(L"");
    // Excel's dialog: selecting cells allowed, everything else not.
    const edit::SheetProtection defaults;
    for (size_t i = 0; i < kPermissionCount; ++i) allow_[i].checked = !(defaults.*kPermissions[i].flag);
    structure_.checked = true;
    apply_.text = mode == kProtectSheet || mode == kProtectBook ? L"Protect" : L"Unprotect";
    focusWanted_ = true;
  }

  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override {
    ui_ = ui;
    const float left = body.left + 16 * ui, right = body.right - 16 * ui;
    float y = body.top + 12 * ui;
    labels_.clear();
    std::vector<Control*> out;
    auto box = [&](FocusComboBox& b, const wchar_t* label) {
      labels_.push_back({label, RectF(left, y, right, y + 16 * ui)});
      b.rc = RectF(left, y + 18 * ui, right, y + 48 * ui);
      y += 56 * ui;
      out.push_back(&b);
    };
    const bool protect = mode_ == kProtectSheet || mode_ == kProtectBook;
    box(password_, protect ? L"Password (optional)" : L"Password");
    if (protect) box(confirm_, L"Confirm the password");
    if (mode_ == kProtectSheet) {
      labels_.push_back({L"Everyone may:", RectF(left, y, right, y + 16 * ui)});
      y += 20 * ui;
      const float gap = 4 * ui, w = (right - left - gap) / 2, h = 26 * ui;
      for (size_t i = 0; i < kPermissionCount; ++i) {
        const float x = left + (i % 2) * (w + gap), top = y + (i / 2) * (h + gap);
        allow_[i].rc = RectF(x, top, x + w, top + h);
        out.push_back(&allow_[i]);
      }
      y += ((kPermissionCount + 1) / 2) * (h + gap) + 6 * ui;
    } else if (mode_ == kProtectBook) {
      labels_.push_back({L"Protect the workbook's:", RectF(left, y, right, y + 18 * ui)});
      structure_.rc = RectF(left, y + 22 * ui, left + 140 * ui, y + 52 * ui);
      out.push_back(&structure_);
      y += 64 * ui;
    }
    apply_.rc = RectF(left, y, left + 120 * ui, y + 32 * ui);
    out.push_back(&apply_);
    noteRc_ = RectF(left, apply_.rc.bottom + 12 * ui, right, apply_.rc.bottom + 92 * ui);
    return out;
  }

  void Draw(Canvas& c) override {
    const Palette& p = theme::P();
    for (const auto& [text, rc] : labels_) c.Text(text, rc, p.textSecondary, 12 * ui_);
    std::wstring help = note_;
    if (help.empty())
      help = mode_ == kProtectSheet ? L"Locked cells can't be changed (Format cells > Protect). Without a password, "
                                      L"anyone can unprotect the sheet."
             : mode_ == kProtectBook ? L"Sheets can't be added, deleted, moved, renamed or hidden."
                                     : L"";
    c.TextWrapped(help, noteRc_, note_.empty() ? p.textSecondary : p.text, 12 * ui_, 4);
    if (focusWanted_) {
      password_.TakeFocus();
      focusWanted_ = false;
    }
  }

 private:
  void Fail(const wchar_t* why) {
    note_ = why;
    v_.host_.ViewInvalidate();
  }

  void Apply() {
    const int sheet = static_cast<int>(v_.sheet_);
    const std::wstring& pw = password_.Text();
    edit::Result r;
    switch (mode_) {
      case kProtectSheet: {
        if (pw != confirm_.Text()) return Fail(kNotIdentical);
        edit::SheetProtection p;
        for (size_t i = 0; i < kPermissionCount; ++i) p.*kPermissions[i].flag = !allow_[i].checked;
        // Cells that can't be selected when unlocked ones can't be (Excel's dialog does the same).
        if (p.selectUnlockedCells) p.selectLockedCells = true;
        p.password = edit::ProtectionPassword::Make(pw);
        r = v_.Ed().SetSheetProtection(sheet, p);
        break;
      }
      case kUnprotectSheet: {
        const auto p = edit::SheetProtectionOf(v_.Current());
        if (p && !p->password.Matches(pw)) return Fail(kWrongPassword);
        r = v_.UnprotectSheet();
        break;
      }
      case kProtectBook: {
        if (pw != confirm_.Text()) return Fail(kNotIdentical);
        if (!structure_.checked) return Fail(L"Choose what to protect.");
        edit::WorkbookProtection w;
        if (auto now = edit::WorkbookProtectionOf(*v_.book_)) w.other = now->other;  // the revisions password
        w.password = edit::ProtectionPassword::Make(pw);
        r = v_.Ed().SetWorkbookProtection(w);
        break;
      }
      case kUnprotectBook: {
        const auto w = edit::WorkbookProtectionOf(*v_.book_);
        if (w && !w->password.Matches(pw)) return Fail(kWrongPassword);
        r = v_.Ed().SetWorkbookProtection(std::nullopt);
        break;
      }
    }
    if (v_.EditRefused(r)) return;
    password_.SetText(L"");
    confirm_.SetText(L"");
    v_.host_.ViewShowPanel(nullptr);
    v_.host_.ViewFocusPage();
    v_.host_.ViewStateChanged();
    v_.host_.ViewInvalidate();
  }

  WorkbookView& v_;
  Mode mode_ = kProtectSheet;
  FocusComboBox password_, confirm_;
  PanelButton allow_[kPermissionCount];
  PanelButton structure_, apply_;
  std::vector<std::pair<std::wstring, D2D1_RECT_F>> labels_;
  std::wstring note_;
  D2D1_RECT_F noteRc_{};
  float ui_ = 1;
  bool focusWanted_ = false;
};

// Insert link (Ctrl+K) and Edit link: a web address, file or e-mail address, or a place in this
// workbook, and the text the cell shows.
class WorkbookView::LinkPanel : public PanelContent {
 public:
  explicit LinkPanel(WorkbookView& view) : v_(view) {
    for (FocusComboBox* box : {&address_, &place_, &text_}) {
      box->autoComplete = false;
      box->onEnter = [this] { Apply(); };
    }
    place_.autoComplete = true;
    address_.label = L"Address";
    address_.tooltip = L"A web address (https://...), an e-mail address or a file";
    place_.label = L"Place in this workbook";
    text_.label = L"Text to display";
    apply_.onInvoke = [this] { Apply(); };
    remove_.text = L"Remove link";
    remove_.onInvoke = [this] {
      if (!v_.EditRefused(v_.Ed().SetHyperlink(static_cast<int>(v_.sheet_), v_.Selected(), std::nullopt))) Close();
    };
  }

  std::wstring Title() const override { return editing_ ? L"Edit link" : L"Insert link"; }

  void Open() {
    note_.clear();
    const Hyperlink* link = v_.LinkAt(v_.active_.row, v_.active_.col);
    editing_ = link != nullptr;
    old_ = link ? *link : Hyperlink{};
    address_.SetText(Utf8ToWide(old_.target));
    place_.SetText(Utf8ToWide(old_.location));
    text_.SetText(Utf8ToWide(old_.display));
    std::vector<std::wstring> places;
    for (size_t i = 0; i < v_.book_->SheetCount(); ++i)
      if (v_.SheetShown(i)) places.push_back(Utf8ToWide(pkg::QuoteSheetName(v_.book_->GetSheet(i).Name())) + L"!A1");
    for (const DefinedName& n : v_.book_->Names())
      if (!n.hidden && n.name.rfind("_xlnm.", 0) != 0) places.push_back(Utf8ToWide(n.name));
    place_.items = std::move(places);
    apply_.text = editing_ ? L"Update" : L"Insert";
    focusWanted_ = true;
  }

  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override {
    ui_ = ui;
    const float left = body.left + 16 * ui, right = body.right - 16 * ui;
    float y = body.top + 12 * ui;
    labels_.clear();
    for (FocusComboBox* b : {&address_, &place_, &text_}) {
      labels_.push_back({b->label, RectF(left, y, right, y + 16 * ui)});
      b->rc = RectF(left, y + 18 * ui, right, y + 48 * ui);
      y += 56 * ui;
    }
    apply_.rc = RectF(left, y, left + 110 * ui, y + 32 * ui);
    remove_.rc = RectF(apply_.rc.right + 8 * ui, y, apply_.rc.right + 8 * ui + 130 * ui, y + 32 * ui);
    remove_.visible = editing_;
    noteRc_ = RectF(left, y + 44 * ui, right, y + 124 * ui);
    std::vector<Control*> out = {&address_, &place_, &text_, &apply_};
    if (editing_) out.push_back(&remove_);
    return out;
  }

  void Draw(Canvas& c) override {
    const Palette& p = theme::P();
    for (const auto& [text, rc] : labels_) c.Text(text, rc, p.textSecondary, 12 * ui_);
    c.TextWrapped(note_.empty() ? L"An address, or a place in this workbook (a cell or a name). Clicking the cell opens "
                                  L"web and e-mail addresses."
                                : note_,
                  noteRc_, note_.empty() ? p.textSecondary : p.text, 12 * ui_, 4);
    if (focusWanted_) {
      address_.TakeFocus();
      focusWanted_ = false;
    }
  }

 private:
  void Close() {
    v_.host_.ViewShowPanel(nullptr);
    v_.host_.ViewFocusPage();
    v_.host_.ViewInvalidate();
  }

  void Apply() {
    const std::wstring addr = Trim(address_.Text()), place = Trim(place_.Text());
    if (addr.empty() && place.empty()) {
      note_ = L"Type an address, or choose a place in this workbook.";
      v_.host_.ViewInvalidate();
      return;
    }
    Hyperlink h;
    if (!addr.empty()) {
      std::wstring t = addr;
      // As Excel takes them: an e-mail address gets mailto:, a www. address http://.
      if (t.find(L"://") == std::wstring::npos && t.rfind(L"mailto:", 0) != 0) {
        if (t.find(L'@') != std::wstring::npos && t.find_first_of(L"\\/") == std::wstring::npos) t = L"mailto:" + t;
        else if (t.rfind(L"www.", 0) == 0) t = L"http://" + t;
      }
      h.target = WideToUtf8(t);
      if (h.target == old_.target) h.relId = old_.relId;
    } else {
      h.location = WideToUtf8(place[0] == L'#' ? place.substr(1) : place);
    }
    const int sheet = static_cast<int>(v_.sheet_);
    const CellAddr at = v_.active_;
    CellInfo cell;
    const bool empty = !v_.Current().GetCell(at.row, at.col, cell) || cell.value.type == ValueType::Empty;
    std::wstring shown = text_.Text();
    if (shown.empty() && empty) shown = addr.empty() ? place : addr;
    h.display = WideToUtf8(shown);
    edit::EditGroup group(v_.Ed(), u"Insert Hyperlink");
    // An empty cell shows the text to display, as in Excel.
    if (empty && !shown.empty() && v_.EditRefused(v_.Ed().SetCell(sheet, at.row, at.col, edit::CellInput::Text(h.display))))
      return;
    if (v_.EditRefused(v_.Ed().SetHyperlink(sheet, v_.Selected(), h))) return;
    // Excel's Hyperlink look: the theme's link color, underlined (where formatting is allowed).
    if (!editing_ && edit::ProtectionRefusal(*v_.book_, sheet, edit::ProtectAct::FormatCells).empty())
      v_.ChangeFont(u"Insert Hyperlink", [](Font& f) {
        f.color = Color{Color::Kind::Theme, 10};
        f.underline = Underline::Single;
      });
    group.done = true;
    Close();
  }

  WorkbookView& v_;
  bool editing_ = false;
  Hyperlink old_;
  FocusComboBox address_, place_, text_;
  PanelButton apply_, remove_;
  std::vector<std::pair<std::wstring, D2D1_RECT_F>> labels_;
  std::wstring note_;
  D2D1_RECT_F noteRc_{};
  float ui_ = 1;
  bool focusWanted_ = false;
};

namespace {

// The note's author: Windows' user name, as the plan says (not anything the user didn't type).
std::string NoteAuthor() {
  wchar_t name[257];
  DWORD n = static_cast<DWORD>(std::size(name));
  return GetUserNameW(name, &n) && n > 1 ? WideToUtf8(std::wstring(name, n - 1)) : std::string();
}

// The text box holds one line: a note's line breaks show as pilcrows.
std::wstring ToBox(std::wstring s) {
  std::replace(s.begin(), s.end(), L'\n', L'¶');
  return s;
}
std::wstring FromBox(std::wstring s) {
  std::replace(s.begin(), s.end(), L'¶', L'\n');
  return s;
}

}  // namespace

// New Note and Edit Note (Shift+F2): the note's text, shown or not; Delete.
class WorkbookView::NotePanel : public PanelContent {
 public:
  explicit NotePanel(WorkbookView& view) : v_(view) {
    text_.autoComplete = false;
    text_.label = L"Note";
    text_.onEnter = [this] { Apply(); };
    shown_.toggle = true;
    shown_.text = L"Keep shown";
    shown_.onInvoke = [this] { shown_.checked = !shown_.checked; v_.host_.ViewInvalidate(); };
    save_.text = L"Save";
    save_.onInvoke = [this] { Apply(); };
    delete_.text = L"Delete note";
    delete_.onInvoke = [this] {
      if (!v_.EditRefused(v_.Ed().SetNote(static_cast<int>(v_.sheet_), cell_, std::nullopt))) Close();
    };
  }
  std::wstring Title() const override { return editing_ ? L"Edit note" : L"New note"; }

  void Open() {
    cell_ = v_.active_;
    std::wstring text;
    const CellNote* note = v_.NoteHere(&text);
    editing_ = note != nullptr;
    const std::wstring reviewer = v_.host_.ViewReviewer();
    author_ = note ? WideToUtf8(note->author) : reviewer.empty() ? NoteAuthor() : WideToUtf8(reviewer);
    text_.SetText(ToBox(text));
    shown_.checked = note && note->shown;
    focusWanted_ = true;
  }

  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override {
    ui_ = ui;
    const float left = body.left + 16 * ui, right = body.right - 16 * ui;
    labelRc_ = RectF(left, body.top + 12 * ui, right, body.top + 28 * ui);
    text_.rc = RectF(left, body.top + 30 * ui, right, body.top + 62 * ui);
    shown_.rc = RectF(left, body.top + 72 * ui, left + 130 * ui, body.top + 100 * ui);
    save_.rc = RectF(left, body.top + 112 * ui, left + 100 * ui, body.top + 144 * ui);
    delete_.rc = RectF(save_.rc.right + 8 * ui, save_.rc.top, save_.rc.right + 138 * ui, save_.rc.bottom);
    noteRc_ = RectF(left, save_.rc.bottom + 12 * ui, right, save_.rc.bottom + 72 * ui);
    std::vector<Control*> out = {&text_, &shown_, &save_};
    if (editing_) out.push_back(&delete_);
    delete_.visible = editing_;
    return out;
  }

  void Draw(Canvas& c) override {
    const Palette& p = theme::P();
    const std::wstring by = author_.empty() ? std::wstring() : L" by " + Utf8ToWide(author_);
    c.Text(L"Note on " + Utf8ToWide(CellName(cell_.row, cell_.col)) + by, labelRc_, p.textSecondary, 12 * ui_);
    c.TextWrapped(L"Enter saves. A ¶ starts a new line in the note.", noteRc_, p.textSecondary, 12 * ui_, 3);
    if (focusWanted_) {
      text_.TakeFocus();
      focusWanted_ = false;
    }
  }

 private:
  void Close() {
    v_.host_.ViewShowPanel(nullptr);
    v_.host_.ViewFocusPage();
    v_.host_.ViewInvalidate();
  }
  void Apply() {
    edit::Editor::Note n{author_, WideToUtf8(FromBox(text_.Text())), shown_.checked};
    if (!v_.EditRefused(v_.Ed().SetNote(static_cast<int>(v_.sheet_), cell_, n))) Close();
  }

  WorkbookView& v_;
  CellAddr cell_{};
  bool editing_ = false;
  std::string author_;
  FocusComboBox text_;
  PanelButton shown_, save_, delete_;
  D2D1_RECT_F labelRc_{}, noteRc_{};
  float ui_ = 1;
  bool focusWanted_ = false;
};

// New Comment, a thread's replies (Excel 365's threaded comments): post a comment or a reply,
// pick one to edit or delete (the first deletes the thread), resolve or reopen.
class WorkbookView::CommentPanel : public PanelContent {
 public:
  explicit CommentPanel(WorkbookView& view) : v_(view) {
    pick_.autoComplete = false;
    pick_.label = L"Comment";
    pick_.onCommit = [this](const std::wstring&, int i) { Pick(i); };
    text_.autoComplete = false;
    text_.onEnter = [this] { Post(); };
    post_.onInvoke = [this] { Post(); };
    resolve_.onInvoke = [this] {
      if (!thread_) return;
      edit::Editor::Thread t = *thread_;
      t.resolved = !t.resolved;
      Set(t);
    };
    delete_.onInvoke = [this] {
      if (!thread_) return;
      if (editing_ <= 0) {
        Set(std::nullopt);
      } else {
        edit::Editor::Thread t = *thread_;
        t.comments.erase(t.comments.begin() + editing_);
        Set(t);
      }
    };
  }
  std::wstring Title() const override { return thread_ ? L"Comments" : L"New comment"; }

  void Open() {
    cell_ = v_.active_;
    // The name Word's comments use (set in the menu), else Windows' user name.
    const std::wstring reviewer = v_.host_.ViewReviewer();
    author_ = reviewer.empty() ? NoteAuthor() : WideToUtf8(reviewer);
    Reload(-1);
  }

  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override {
    ui_ = ui;
    const float left = body.left + 16 * ui, right = body.right - 16 * ui;
    labelRc_ = RectF(left, body.top + 12 * ui, right, body.top + 28 * ui);
    threadRc_ = RectF(left, body.top + 32 * ui, right, body.top + 112 * ui);
    float y = thread_ ? threadRc_.bottom + 8 * ui : body.top + 32 * ui;
    pick_.rc = RectF(left, y, right, y + 32 * ui);
    if (thread_) y = pick_.rc.bottom + 10 * ui;
    text_.rc = RectF(left, y, right, y + 32 * ui);
    post_.rc = RectF(left, text_.rc.bottom + 12 * ui, left + 72 * ui, text_.rc.bottom + 44 * ui);
    resolve_.rc = RectF(post_.rc.right + 8 * ui, post_.rc.top, post_.rc.right + 92 * ui, post_.rc.bottom);
    delete_.rc = RectF(resolve_.rc.right + 8 * ui, post_.rc.top, std::min(right, resolve_.rc.right + 124 * ui), post_.rc.bottom);
    noteRc_ = RectF(left, post_.rc.bottom + 12 * ui, right, post_.rc.bottom + 60 * ui);
    std::vector<Control*> out = {&text_, &post_};
    if (thread_) out = {&pick_, &text_, &post_, &resolve_, &delete_};
    pick_.visible = resolve_.visible = delete_.visible = thread_.has_value();
    return out;
  }

  void Draw(Canvas& c) override {
    const Palette& p = theme::P();
    std::wstring head = L"Comment on " + Utf8ToWide(CellName(cell_.row, cell_.col));
    if (thread_ && thread_->resolved) head += L" · resolved";
    c.Text(head, labelRc_, p.textSecondary, 12 * ui_);
    if (thread_) {
      // The latest comments that fit, each "Name: text".
      std::wstring all;
      for (const auto& m : thread_->comments)
        all += (all.empty() ? L"" : L"\n") + Utf8ToWide(m.author.empty() ? m.text : m.author + ": " + m.text);
      c.TextWrapped(all, threadRc_, p.text, 12 * ui_, 5);
    }
    c.TextWrapped(L"Enter posts. A ¶ starts a new line.", noteRc_, p.textSecondary, 12 * ui_, 2);
    if (focusWanted_) {
      text_.TakeFocus();
      focusWanted_ = false;
    }
  }

 private:
  // The thread as the file has it now; `edit` the comment being edited (-1: a reply or a new one).
  void Reload(int edit) {
    thread_ = v_.Ed().ThreadAt(static_cast<int>(v_.sheet_), cell_);
    pick_.items.clear();
    if (thread_) {
      pick_.items.push_back(L"Reply");
      for (size_t i = 0; i < thread_->comments.size(); ++i) {
        const auto& m = thread_->comments[i];
        std::wstring t = std::to_wstring(i + 1) + L". " + Utf8ToWide(m.author) + L": " + ToBox(Utf8ToWide(m.text));
        if (t.size() > 60) t = t.substr(0, 59) + L"…";
        pick_.items.push_back(std::move(t));
      }
    }
    Pick(edit < 0 ? 0 : edit + 1);
  }
  void Pick(int i) {
    if (!thread_ || i < 0 || static_cast<size_t>(i) > thread_->comments.size()) i = 0;
    editing_ = i - 1;
    pick_.ReplaceText(thread_ ? pick_.items[static_cast<size_t>(i)] : std::wstring());
    text_.label = !thread_ ? L"Comment" : editing_ < 0 ? L"Reply" : L"Edit";
    text_.ReplaceText(editing_ < 0 ? std::wstring() : ToBox(Utf8ToWide(thread_->comments[static_cast<size_t>(editing_)].text)));
    post_.text = editing_ < 0 ? L"Post" : L"Save";
    resolve_.text = thread_ && thread_->resolved ? L"Reopen" : L"Resolve";
    delete_.text = editing_ <= 0 ? L"Delete thread" : L"Delete reply";
    focusWanted_ = true;
    v_.host_.ViewInvalidate();
  }
  void Close() {
    v_.host_.ViewShowPanel(nullptr);
    v_.host_.ViewFocusPage();
    v_.host_.ViewInvalidate();
  }
  void Set(const std::optional<edit::Editor::Thread>& t) {
    if (v_.EditRefused(v_.Ed().SetThread(static_cast<int>(v_.sheet_), cell_, t))) return;
    if (!t) return Close();
    Reload(-1);
  }
  void Post() {
    const std::string text = WideToUtf8(FromBox(text_.Text()));
    if (text.empty()) return;
    edit::Editor::Thread t = thread_.value_or(edit::Editor::Thread{});
    if (editing_ >= 0) {
      t.comments[static_cast<size_t>(editing_)].text = text;
    } else {
      t.comments.push_back({author_, text, {}, {}});
      t.resolved = false;  // a reply reopens a resolved thread, as in Excel
    }
    Set(t);
  }

  WorkbookView& v_;
  CellAddr cell_{};
  std::string author_;
  std::optional<edit::Editor::Thread> thread_;
  int editing_ = -1;
  FocusComboBox pick_, text_;
  PanelButton post_, resolve_, delete_;
  D2D1_RECT_F labelRc_{}, threadRc_{}, noteRc_{};
  float ui_ = 1;
  bool focusWanted_ = false;
};

bool WorkbookView::ThreadHere() const {
  const SheetObjects* o = Objects();
  const CellNote* n = o ? o->NoteAt(active_.row, active_.col) : nullptr;
  return n && n->threaded;
}

const CellNote* WorkbookView::NoteHere(std::wstring* text) const {
  const SheetObjects* o = Objects();
  const CellNote* n = o ? o->NoteAt(active_.row, active_.col) : nullptr;
  if (!n || n->threaded) return nullptr;
  if (text) {
    *text = n->text;
    // Excel's New Note starts with "Author:" on its own line.
    const std::wstring head = n->author + L":\n";
    if (!n->author.empty() && text->rfind(head, 0) == 0) text->erase(0, head.size());
  }
  return n;
}

// ---------------------------------------------------------------------------
// Commands

edit::Result WorkbookView::UnprotectSheet() {
  const auto p = edit::SheetProtectionOf(Current());
  // Excel keeps the selection settings in the file (an element that protects nothing).
  std::optional<edit::SheetProtection> keep;
  if (p && (p->selectLockedCells || p->selectUnlockedCells)) {
    keep = edit::SheetProtection{};
    keep->sheet = keep->objects = keep->scenarios = false;
    keep->selectLockedCells = p->selectLockedCells;
    keep->selectUnlockedCells = p->selectUnlockedCells;
  }
  return Ed().SetSheetProtection(static_cast<int>(sheet_), keep);
}

bool WorkbookView::ReviewCommandState(int cmd, CommandState& s) {
  const bool grid = Ready() && !preview_;
  switch (cmd) {
    case CmdProtectSheet:
      s.label = grid && edit::SheetProtected(Current()) ? L"Unprotect sheet" : L"Protect sheet";
      s.enabled = grid && !ChartSheet();
      return true;
    case CmdProtectWorkbook:
      s.label = L"Protect workbook";
      s.checked = Ready() && edit::StructureProtected(*book_);
      s.enabled = grid;
      return true;
    case CmdInsertLink:
      s.label = grid && !ChartSheet() && LinkAt(active_.row, active_.col) ? L"Edit link" : L"Insert link";
      s.shortcut = L"Ctrl+K";
      s.enabled = grid && !ChartSheet();
      return true;
    case CmdRemoveLink: {
      bool any = false;
      if (grid && !ChartSheet())
        for (const Hyperlink& h : Current().Hyperlinks()) {
          const Range sel = Selected();
          any = any || (sel.firstRow <= h.ref.lastRow && h.ref.firstRow <= sel.lastRow && sel.firstCol <= h.ref.lastCol &&
                        h.ref.firstCol <= sel.lastCol);
        }
      s.label = L"Remove link";
      s.enabled = any;
      return true;
    }
    case CmdNewNote:
      s.label = grid && !ChartSheet() && NoteHere() ? L"Edit note" : L"New note";
      s.shortcut = L"Shift+F2";
      s.enabled = grid && !ChartSheet() && !ThreadHere();  // a cell has a note or a thread
      return true;
    case CmdNewComment:
      s.label = grid && !ChartSheet() && ThreadHere() ? L"Reply to comment" : L"New comment";
      s.shortcut = L"Ctrl+Alt+M";
      s.enabled = grid && !ChartSheet() && !NoteHere();
      return true;
    case CmdDeleteThread:
      s.label = L"Delete comment";
      s.enabled = grid && !ChartSheet() && ThreadHere();
      return true;
    case CmdDeleteNote:
      s.label = L"Delete note";
      s.enabled = grid && !ChartSheet() && NoteHere();
      return true;
    case CmdShowNote: {
      const CellNote* n = grid && !ChartSheet() ? NoteHere() : nullptr;
      s.label = n && n->shown ? L"Hide note" : L"Show note";
      s.enabled = n != nullptr;
      return true;
    }
    case CmdShowAllNotes: {
      bool any = false;
      if (const SheetObjects* o = grid && !ChartSheet() ? Objects() : nullptr)
        for (const CellNote& n : o->Notes()) any = any || !n.threaded;
      s.label = L"Show all notes";
      s.enabled = any;
      return true;
    }
    default: return false;
  }
}

void WorkbookView::AddReviewCellItems(HMENU m) {
  for (int cmd : {CmdInsertLink, CmdRemoveLink, CmdNewComment, CmdDeleteThread, CmdNewNote, CmdDeleteNote, CmdShowNote}) {
    CommandState s;
    ReviewCommandState(cmd, s);
    if ((cmd == CmdRemoveLink || cmd == CmdDeleteNote || cmd == CmdShowNote || cmd == CmdDeleteThread) && !s.enabled)
      continue;
    std::wstring text = s.label;
    if (cmd == CmdInsertLink) text += L"…\tCtrl+K";
    if (cmd == CmdNewNote) text += L"…\tShift+F2";
    if (cmd == CmdNewComment) text += L"…\tCtrl+Alt+M";
    menu::AppendItem(m, static_cast<UINT>(cmd), text, s.enabled);
  }
}

bool WorkbookView::ReviewCommand(int cmd) {
  auto open = [&](ProtectPanel::Mode mode) {
    if (!protectPanel_) protectPanel_ = std::make_unique<ProtectPanel>(*this);
    auto* panel = static_cast<ProtectPanel*>(protectPanel_.get());
    panel->Open(mode);
    host_.ViewShowPanel(panel);
  };
  switch (cmd) {
    case CmdProtectSheet: {
      const auto p = edit::SheetProtectionOf(Current());
      if (!p || !p->sheet) open(ProtectPanel::kProtectSheet);
      else if (p->password.Set()) open(ProtectPanel::kUnprotectSheet);
      else if (!EditRefused(UnprotectSheet())) host_.ViewStateChanged();  // no password: at once, as Excel
      return true;
    }
    case CmdProtectWorkbook: {
      const auto w = edit::WorkbookProtectionOf(*book_);
      if (!w || !w->lockStructure) open(ProtectPanel::kProtectBook);
      else if (w->password.Set()) open(ProtectPanel::kUnprotectBook);
      else if (!EditRefused(Ed().SetWorkbookProtection(std::nullopt))) host_.ViewStateChanged();
      return true;
    }
    case CmdInsertLink: {
      if (!linkPanel_) linkPanel_ = std::make_unique<LinkPanel>(*this);
      auto* panel = static_cast<LinkPanel*>(linkPanel_.get());
      panel->Open();
      host_.ViewShowPanel(panel);
      return true;
    }
    case CmdRemoveLink: EditRefused(Ed().SetHyperlink(static_cast<int>(sheet_), Selected(), std::nullopt)); return true;
    case CmdNewNote: {
      if (ChartSheet()) return true;
      if (ThreadHere()) return ReviewCommand(CmdNewComment);  // Shift+F2 on a thread: its replies
      if (!notePanel_) notePanel_ = std::make_unique<NotePanel>(*this);
      auto* panel = static_cast<NotePanel*>(notePanel_.get());
      panel->Open();
      host_.ViewShowPanel(panel);
      return true;
    }
    case CmdDeleteNote: EditRefused(Ed().SetNote(static_cast<int>(sheet_), active_, std::nullopt)); return true;
    case CmdNewComment: {
      if (ChartSheet() || NoteHere()) return true;
      if (!commentPanel_) commentPanel_ = std::make_unique<CommentPanel>(*this);
      auto* panel = static_cast<CommentPanel*>(commentPanel_.get());
      panel->Open();
      host_.ViewShowPanel(panel);
      return true;
    }
    case CmdDeleteThread:
      if (ThreadHere()) EditRefused(Ed().SetThread(static_cast<int>(sheet_), active_, std::nullopt));
      return true;
    case CmdShowNote:
    case CmdShowAllNotes: {
      // Each note again with its text, shown or hidden (all shown unless every one is).
      const SheetObjects* o = Objects();
      if (!o) return true;
      std::vector<CellNote> notes;
      bool allShown = true;
      for (const CellNote& n : o->Notes())
        if (!n.threaded && (cmd == CmdShowAllNotes || (n.row == active_.row && n.col == active_.col))) {
          notes.push_back(n);
          allShown = allShown && n.shown;
        }
      edit::EditGroup group(Ed(), cmd == CmdShowNote ? u"Show Note" : u"Show All Notes");
      for (const CellNote& n : notes) {
        std::wstring text = n.text;
        const std::wstring head = n.author + L":\n";
        if (!n.author.empty() && text.rfind(head, 0) == 0) text.erase(0, head.size());
        const edit::Editor::Note note{WideToUtf8(n.author), WideToUtf8(text), cmd == CmdShowNote ? !n.shown : !allShown};
        if (EditRefused(Ed().SetNote(static_cast<int>(sheet_), CellAddr{n.row, n.col}, note))) return true;
      }
      group.done = true;
      return true;
    }
    default: return false;
  }
}

void WorkbookView::AddReviewTabItems(HMENU m) {
  for (int cmd : {CmdProtectSheet, CmdProtectWorkbook}) {
    CommandState s;
    ReviewCommandState(cmd, s);
    menu::AppendItem(m, static_cast<UINT>(cmd), s.label + L"…", s.enabled, s.checked);
  }
}

// ---------------------------------------------------------------------------
// A protected sheet in the grid

bool WorkbookView::LockedForTyping() {
  if (!Ready() || ChartSheet()) return false;
  const Sheet& s = Current();
  if (!edit::SheetProtected(s) || !edit::CellLocked(s, book_->GetStyles(), active_.row, active_.col)) return false;
  const std::u16string why = edit::LockedCellMessage();
  host_.ViewMessage(BannerKind::Warning, std::wstring(why.begin(), why.end()));
  return true;
}

bool WorkbookView::FormulaBarHidden() const {
  if (!Ready() || ChartSheet()) return false;
  const Sheet& s = Current();
  return edit::SheetProtected(s) && edit::FormulaHidden(s, book_->GetStyles(), active_.row, active_.col);
}

bool WorkbookView::SelectionBlocked(CellAddr at) const {
  if (!Ready() || ChartSheet()) return false;
  const Sheet& s = Current();
  const auto p = edit::SheetProtectionOf(s);
  if (!p || !p->sheet) return false;
  if (p->selectUnlockedCells) return true;  // nothing can be selected
  return p->selectLockedCells && edit::CellLocked(s, book_->GetStyles(), at.row, at.col);
}

bool WorkbookView::ProtectedMove(int dr, int dc) {
  if (!Ready() || ChartSheet()) return false;
  const Sheet& s = Current();
  const auto p = edit::SheetProtectionOf(s);
  if (!p || !p->sheet || !(p->selectLockedCells || p->selectUnlockedCells)) return false;
  if (p->selectUnlockedCells) return true;  // the cursor stays
  // The next unlocked cell that way, or nowhere.
  int64_t r = active_.row, c = active_.col;
  for (int steps = 0; steps < 100000; ++steps) {
    r += dr;
    c += dc;
    if (r < 0 || c < 0 || r >= kMaxRows || c >= kMaxCols) break;
    const CellAddr at{static_cast<uint32_t>(r), static_cast<uint32_t>(c)};
    if (!edit::CellLocked(s, book_->GetStyles(), at.row, at.col)) {
      Select(at, at);
      break;
    }
  }
  return true;
}

bool WorkbookView::TabToUnlocked(bool back) {
  if (!Ready() || ChartSheet()) return false;
  const Sheet& s = Current();
  if (!edit::SheetProtected(s)) return false;
  // Excel's Tab on a protected sheet: the next unlocked cell, row by row, round the used area.
  Range area = s.UsedRange().value_or(Range{0, 0, 0, 0});
  area.firstRow = std::min(area.firstRow, active_.row);
  area.firstCol = std::min(area.firstCol, active_.col);
  area.lastRow = std::max(area.lastRow, active_.row);
  area.lastCol = std::max(area.lastCol, active_.col);
  const uint64_t w = uint64_t{area.lastCol} - area.firstCol + 1, h = uint64_t{area.lastRow} - area.firstRow + 1;
  const uint64_t n = w * h, start = (uint64_t{active_.row} - area.firstRow) * w + (active_.col - area.firstCol);
  for (uint64_t step = 1; step <= std::min<uint64_t>(n, 2000000); ++step) {
    const uint64_t i = back ? (start + n - step % n) % n : (start + step) % n;
    const CellAddr at{area.firstRow + static_cast<uint32_t>(i / w), area.firstCol + static_cast<uint32_t>(i % w)};
    if (!edit::CellLocked(s, book_->GetStyles(), at.row, at.col)) {
      Select(at, at);
      return true;
    }
  }
  return true;  // none: the cursor stays
}

}  // namespace fw::sheet::view
