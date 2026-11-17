// The workbook tab's review features (S3, s3-review): Protect Sheet and Protect Workbook, and how a
// protected sheet behaves in the grid. Links, notes and comments come here too. Every change goes
// through the edit API (sheet/edit/Protection.h has Excel's rules).
#include <algorithm>
#include <iterator>

#include "Commands.h"
#include "core/Strings.h"
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
    default: return false;
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
