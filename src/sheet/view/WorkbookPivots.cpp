// Pivot tables in the grid (S4 s4-pivot): Insert > PivotTable (its panel), Refresh (Alt+F5) and
// Refresh All (Ctrl+Alt+F5), through the edit layer (Editor::InsertPivot, RefreshPivots), so each
// is one undo step.
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <string_view>
#include <vector>

#include "Commands.h"
#include "core/Strings.h"
#include "pkg/Package.h"
#include "sheet/edit/Edit.h"
#include "sheet/view/WorkbookFormat.h"
#include "sheet/view/WorkbookView.h"
#include "ui/Gfx.h"
#include "ui/Theme.h"

using namespace gfx;

namespace fw::sheet::view {

namespace {

// What a field does in the new pivot, in the panel's list's order.
enum Role : int { kNone, kRows, kColumns, kFilters, kSum, kCount, kAverage };
constexpr const wchar_t* kRoles[] = {L"—", L"Rows", L"Columns", L"Filters", L"Sum", L"Count", L"Average"};
constexpr size_t kMaxFields = 14;  // the panel's list; a wider source keeps its first ones
constexpr const wchar_t* kNewSheet = L"New worksheet";  // the place: a new sheet, as Excel's default

std::wstring Trim(std::wstring s) {
  while (!s.empty() && (s.back() == L' ' || s.back() == L'=')) s.pop_back();
  size_t at = 0;
  while (at < s.size() && (s[at] == L' ' || s[at] == L'=')) ++at;
  return s.substr(at);
}

// "Sheet 2!A1:F9", "'My sheet'!B3" or "A1:F9" (on `current`): the sheet and the range.
bool ParsePlace(const Workbook& book, size_t current, const std::wstring& text, int& sheet, Range& r) {
  std::string s = WideToUtf8(Trim(text));
  sheet = static_cast<int>(current);
  if (size_t bang = s.rfind('!'); bang != std::string::npos) {
    std::string name = s.substr(0, bang);
    if (name.size() >= 2 && name.front() == '\'' && name.back() == '\'') name = name.substr(1, name.size() - 2);
    sheet = book.FindSheet(name);
    s = s.substr(bang + 1);
    if (sheet < 0) return false;
  }
  return ParseRange(s, r);
}

}  // namespace

class WorkbookView::PivotPanel : public PanelContent {
 public:
  explicit PivotPanel(WorkbookView& view) : v_(view) {
    source_.autoComplete = place_.autoComplete = false;
    source_.onCommit = [this](const std::wstring&, int) { ReadFields(); };
    create_.text = L"Create PivotTable";
    create_.onInvoke = [this] { Create(); };
    for (Dropdown& d : roles_) {
      for (const wchar_t* r : kRoles) d.items.push_back(r);
      d.selected = kNone;
    }
  }

  std::wstring Title() const override { return L"Create PivotTable"; }

  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override {
    body_ = body;
    ui_ = ui;
    Place();
    std::vector<Control*> out{&source_, &place_};
    for (size_t i = 0; i < names_.size(); ++i) out.push_back(&roles_[i]);
    out.push_back(&create_);
    return out;
  }

  void Draw(Canvas& c) override {
    for (const auto& [text, rc] : labels_) c.Text(text, rc, theme::P().textSecondary, 12 * ui_);
  }

  // The source: the table at the active cell, else the selection or the region around the cell;
  // the place: a new worksheet, as in Excel, or (listed) two columns right of it, at its first row.
  void Open() {
    const int t = v_.TableIndexAt(v_.active_);
    Range r = v_.DataRange();
    if (t >= 0) {
      const Table& table = v_.Current().Tables()[static_cast<size_t>(t)];
      source_.SetText(Utf8ToWide(table.name));
      r = table.ref;
    } else {
      source_.SetText(Utf8ToWide(RangeName(r)));
    }
    place_.items = {kNewSheet, Utf8ToWide(CellName(r.firstRow, std::min<uint32_t>(kMaxCols - 1, r.lastCol + 2)))};
    place_.SetText(kNewSheet);
    ReadFields();
  }

 private:
  // The source's header row: one line per field.
  void ReadFields() {
    names_.clear();
    int sheet = 0;
    Range r;
    if (Source(sheet, r)) {
      const Sheet& s = v_.book_->GetSheet(static_cast<size_t>(sheet));
      for (uint32_t c = r.firstCol; c <= r.lastCol && names_.size() < kMaxFields; ++c) {
        Value v = s.GetValue(r.firstRow, c);
        std::wstring name;
        if (v.type == ValueType::String) {
          name = Utf8ToWide(v.text);
        } else if (v.type == ValueType::Number) {
          wchar_t buf[40];
          std::swprintf(buf, 40, L"%.15g", v.number);
          name = buf;
        }
        names_.push_back(name.empty() ? L"(no name)" : name);
      }
    }
    for (Dropdown& d : roles_) d.selected = kNone;
    Place();
  }

  // The source as typed: a table's name, or cells (on this sheet unless named).
  bool Source(int& sheet, Range& r, std::string* table = nullptr) {
    const std::string text = WideToUtf8(Trim(source_.Text()));
    size_t at = 0;
    if (const Table* t = v_.book_->FindTable(text, &at)) {
      sheet = static_cast<int>(at);
      r = t->ref;
      if (table) *table = t->name;
      return true;
    }
    return ParsePlace(*v_.book_, v_.sheet_, source_.Text(), sheet, r) && v_.book_->LoadSheet(static_cast<size_t>(sheet)).Ok();
  }

  void Create() {
    edit::Editor::NewPivotTable p;
    Range r, at;
    if (!Source(p.sourceSheet, r, &p.table)) {
      v_.host_.ViewMessage(BannerKind::Warning, L"Type the data's cells, such as A1:D20, or a table's name.");
      return;
    }
    p.source = r;
    const bool newSheet = _wcsicmp(Trim(place_.Text()).c_str(), kNewSheet) == 0;
    if (!newSheet) {
      if (!ParsePlace(*v_.book_, v_.sheet_, place_.Text(), p.sheet, at)) {
        v_.host_.ViewMessage(BannerKind::Warning, L"Type the cell for the PivotTable, such as H1 or Sheet2!A3, or choose New worksheet.");
        return;
      }
      p.at = {at.firstRow, at.firstCol};
    }
    for (size_t i = 0; i < names_.size(); ++i) {
      const uint32_t c = static_cast<uint32_t>(i);
      switch (roles_[i].selected) {
        case kRows: p.rows.push_back(c); break;
        case kColumns: p.cols.push_back(c); break;
        case kFilters: p.filters.push_back(c); break;
        case kSum: p.values.push_back({c, 0}); break;
        case kCount: p.values.push_back({c, 1}); break;
        case kAverage: p.values.push_back({c, 2}); break;
        default: break;
      }
    }
    if (!newSheet) {
      if (v_.EditRefused(v_.Ed().InsertPivot(p))) return;
    } else {
      // A new sheet before the active one, as Excel adds it, with the pivot at A3 (its filters
      // above it, from A1): one undo step with the pivot.
      const int sheet = static_cast<int>(v_.sheet_);
      {
        edit::EditGroup group(v_.Ed(), u"PivotTable");
        if (v_.EditRefused(v_.Ed().AddSheet(sheet))) return;
        if (p.sourceSheet >= sheet) ++p.sourceSheet;
        p.sheet = sheet;
        p.at = p.filters.empty() ? CellAddr{2, 0} : CellAddr{0, 0};
        if (v_.EditRefused(v_.Ed().InsertPivot(p))) return;
        group.done = true;
      }
      v_.ShowSheet(static_cast<size_t>(sheet));
    }
    v_.host_.ViewShowPanel(nullptr);
    v_.host_.ViewFocusPage();
  }

  void Place() {
    const float u = ui_, left = body_.left + 16 * u, right = body_.right - 16 * u, gap = 8 * u;
    const float half = (right - left - gap) / 2;
    float y = body_.top + 10 * u;
    labels_.clear();
    auto label = [&](std::wstring text, D2D1_RECT_F rc) { labels_.push_back({std::move(text), rc}); };
    label(L"Where is the data? (cells or a table)", RectF(left, y, right, y + 16 * u));
    y += 20 * u;
    source_.rc = RectF(left, y, right, y + 30 * u);
    y += 40 * u;
    label(L"Put the PivotTable at", RectF(left, y, right, y + 16 * u));
    y += 20 * u;
    place_.rc = RectF(left, y, right, y + 30 * u);
    y += 42 * u;
    if (!names_.empty()) {
      label(L"Fields: rows, columns, filters and values", RectF(left, y, right, y + 16 * u));
      y += 22 * u;
    }
    for (size_t i = 0; i < kMaxFields; ++i) {
      roles_[i].visible = i < names_.size();
      if (i >= names_.size()) continue;
      label(names_[i], RectF(left, y + 7 * u, left + half, y + 23 * u));
      roles_[i].rc = RectF(left + half + gap, y, right, y + 30 * u);
      y += 36 * u;
    }
    y += 6 * u;
    create_.rc = RectF(left, y, left + half, y + 30 * u);
    v_.host_.ViewInvalidate();
  }

  WorkbookView& v_;
  D2D1_RECT_F body_{};
  float ui_ = 1;
  ComboBox source_, place_;
  Dropdown roles_[kMaxFields];
  PanelButton create_;
  std::vector<std::wstring> names_;
  std::vector<std::pair<std::wstring, D2D1_RECT_F>> labels_;
};

void WorkbookView::ShowPivotPanel() {
  if (!Ready() || preview_ || ChartSheet()) return;
  if (!pivotPanel_) pivotPanel_ = std::make_shared<PivotPanel>(*this);
  static_cast<PivotPanel*>(pivotPanel_.get())->Open();
  host_.ViewShowPanel(pivotPanel_.get());
}

bool WorkbookView::PivotsCommandState(int cmd, CommandState& s) {
  const bool cells = Ready() && !preview_ && !ChartSheet() && !Current().PartName().empty();
  switch (cmd) {
    case CmdPivotTable:
      s.label = L"PivotTable…";
      s.enabled = cells;
      return true;
    case CmdPivotRefresh:
      s.label = L"Refresh PivotTable";
      s.shortcut = L"Alt+F5";
      s.enabled = cells;
      return true;
    case CmdPivotRefreshAll:
      s.label = L"Refresh all PivotTables";
      s.shortcut = L"Ctrl+Alt+F5";
      s.enabled = Ready() && !preview_;
      return true;
  }
  return false;
}

bool WorkbookView::PivotsCommand(int cmd) {
  switch (cmd) {
    case CmdPivotTable: ShowPivotPanel(); return true;
    case CmdPivotRefresh:
      if (Ready() && !preview_)
        EditRefused(Ed().RefreshPivots(edit::Editor::PivotRefresh::AtCell, static_cast<int>(sheet_), active_));
      return true;
    case CmdPivotRefreshAll:
      if (Ready() && !preview_) EditRefused(Ed().RefreshPivots(edit::Editor::PivotRefresh::All));
      return true;
  }
  return false;
}

const std::vector<pivot::Look>* WorkbookView::PivotLooksFor(size_t sheet) {
  if (!book_ || sheet >= book_->SheetCount() || !book_->GetPackage()) return nullptr;
  if (pivotLooksRead_.size() != book_->SheetCount()) {
    pivotLooksRead_.assign(book_->SheetCount(), false);
    pivotLooks_.assign(book_->SheetCount(), {});
  }
  if (!pivotLooksRead_[sheet]) {
    pivotLooks_[sheet] = pivot::SheetLooks(*book_, sheet);
    pivotLooksRead_[sheet] = true;
  }
  return pivotLooks_[sheet].empty() ? nullptr : &pivotLooks_[sheet];
}

void WorkbookView::RefreshPivotsOnOpen() {
  const pkg::Package* package = book_ ? book_->GetPackage() : nullptr;
  if (!package) return;
  // Only a workbook with such a cache gets an editor here (most have no pivots at all).
  bool asks = false;
  for (const pkg::Part& part : package->Parts()) {
    if (part.deleted || !package->ContentTypeOf(part.name).ends_with(".pivotCacheDefinition+xml")) continue;
    std::vector<uint8_t> bytes;
    if (!package->ReadPart(part.name, bytes).Ok()) continue;
    const std::string_view xml(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (xml.find("refreshOnLoad=\"1\"") != std::string_view::npos || xml.find("refreshOnLoad=\"true\"") != std::string_view::npos) {
      asks = true;
      break;
    }
  }
  // A pivot Featherweight keeps but doesn't refresh keeps its values, as the file has them.
  if (asks) (void)Ed().RefreshPivots(edit::Editor::PivotRefresh::OnLoad);
}

}  // namespace fw::sheet::view
