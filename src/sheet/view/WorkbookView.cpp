#include "sheet/view/WorkbookView.h"

#include <algorithm>
#include <cmath>

#include "core/Strings.h"
#include "draw/D2DPainter.h"
#include "pkg/CompoundFile.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "sheet/csv/Csv.h"
#include "sheet/numfmt/NumLocale.h"
#include "sheet/view/WorkbookFilter.h"
#include "sheet/view/WorkbookFormat.h"
#include "sheet/view/WorkbookPanels.h"
#include "sheet/xlsx/Xlsx.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

using namespace gfx;

namespace fw::sheet::view {

namespace {

// Cells of one pane on screen: whole device pixels at the zoom, mirrored in a right-to-left sheet.
class ScreenGeometry : public Geometry {
 public:
  ScreenGeometry(const Grid& grid, float left, float top, double sx, double sy, double k, bool rtlSheet, float mirror)
      : grid_(grid), left_(left), top_(top), gridX_(sx), gridY_(sy), k_(k), mirror_(mirror) {
    rtl = rtlSheet;
    pt = static_cast<float>(k * 4 / 3);
    px = static_cast<float>(k);
    line = std::max(1.f, std::floor(static_cast<float>(k) + 0.01f));
    rowPx = static_cast<float>(k);
    padStart = padEnd = static_cast<float>(2 * k);
  }
  float X(uint32_t c) const { return left_ + static_cast<float>(std::round((grid_.ColLeft(c) - gridX_) * k_)); }
  float Left(uint32_t c) const override { return rtl ? mirror_ - X(c + 1) : X(c); }
  float Right(uint32_t c) const override { return rtl ? mirror_ - X(c) : X(c + 1); }
  float Top(uint32_t r) const override { return top_ + static_cast<float>(std::round((grid_.RowTop(r) - gridY_) * k_)); }
  float Bottom(uint32_t r) const override { return Top(r + 1); }

 private:
  const Grid& grid_;
  float left_, top_;
  double gridX_, gridY_, k_;
  float mirror_;
};

std::wstring OpenError(const std::wstring& name, const pkg::Status& st) {
  using Code = pkg::Status::Code;
  switch (st.code) {
    case Code::IoError: return name + L" couldn't be opened — the file couldn't be read.";
    case Code::TooLarge: return name + L" couldn't be opened — it goes beyond the limits Featherweight sets for safety.";
    case Code::Rejected: return name + L" couldn't be opened — it contains something Featherweight refuses for safety.";
    case Code::Unsupported: return name + L" couldn't be opened — it uses something Featherweight doesn't support yet.";
    default: return name + L" couldn't be opened — it seems to be damaged or isn't an Excel workbook.";
  }
}

D2D1_RECT_F Clip(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
  return {std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
}

}  // namespace

WorkbookView::WorkbookView(ViewHost& host, std::wstring path)
    : PagedView(host), path_(std::move(path)), file_(std::make_unique<WorkbookFile>()) {
  zoomMode_ = fw::ZoomMode::FitPage;
}

WorkbookView::~WorkbookView() = default;

std::unique_ptr<WorkbookView> WorkbookView::NewWorkbook(ViewHost& host, int number) {
  auto view = std::make_unique<WorkbookView>(host, std::wstring());
  view->newBook_ = true;
  view->file_->New(number);
  return view;
}

std::unique_ptr<WorkbookView> WorkbookView::FromBytes(ViewHost& host, std::vector<uint8_t> bytes, std::wstring title,
                                                      std::wstring suggested, bool unsaved) {
  auto view = std::make_unique<WorkbookView>(host, std::wstring());
  view->bytes_ = std::move(bytes);
  view->fromBytes_ = true;
  view->file_->Copy(std::move(title), std::move(suggested), unsaved);
  return view;
}

std::wstring WorkbookView::Title() const { return path_.empty() ? file_->Title() : FileNameOf(path_); }

// ---------------------------------------------------------------------------
// Loading

OpenResult WorkbookView::Load(const std::wstring&, std::wstring& error) {
  const std::wstring name = Title();
  pkg::Status st;
  if (newBook_) {
    std::unique_ptr<Workbook> book = xlsx::NewWorkbook(st);
    if (!book) {
      error = L"A new workbook couldn't be made.";
      return OpenResult::Failed;
    }
    return Opened(std::move(book), error);
  }
  // A recovered workbook is read from its autosave, named as its tab is.
  std::unique_ptr<pkg::Snapshot> snapshot =
      fromBytes_ ? pkg::Snapshot::FromBytes(std::move(bytes_)) : pkg::Snapshot::Open(path_, {}, st);
  const std::wstring& named = fromBytes_ ? name : path_;
  if (EndsWithNoCase(named, L".csv") || EndsWithNoCase(named, L".tsv")) {
    // A text file of values: one sheet, named after the file as Excel names it.
    if (!snapshot) {
      error = OpenError(name, st);
      return OpenResult::Failed;
    }
    const auto bytes = snapshot->Bytes();
    csv::Options o;
    o.locale = &UserLocale();
    o.tsv = EndsWithNoCase(named, L".tsv");
    auto csvBook = std::make_unique<Workbook>();
    std::wstring sheetName = name.substr(0, name.rfind(L'.'));
    if (sheetName.size() > 31) sheetName.resize(31);
    csv::Source source;
    csv::Read(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), WideToUtf8(sheetName), o,
              *csvBook, source);
    // Saving reads the file again to write it its own way (WorkbookFile).
    if (!fromBytes_) file_->Opened(path_, snapshot->GetStamp());
    file_->SetCsvOptions(o);
    snapshot.reset();  // the model has the values: the file's copy goes before the grid is built
    return Opened(std::move(csvBook), error);
  }
  if (snapshot && pkg::CompoundFile::HasSignature(snapshot->Bytes())) {
    error = name + L" couldn't be opened — it is protected with a password, which workbooks don't support yet.";
    return OpenResult::Failed;
  }
  auto package = snapshot ? pkg::Package::Open(std::move(snapshot), pkg::Limits::Default(), st) : nullptr;
  std::unique_ptr<Workbook> book = package ? xlsx::Read(std::move(package), st) : nullptr;
  if (!book) {
    error = OpenError(name, st);
    return OpenResult::Failed;
  }
  recalc_ = true;  // a workbook: at idle, recalculate what Excel would on opening
  if (!fromBytes_ && newNumber_ > 0) {
    // A template: a new workbook from it, saved with Save As.
    file_->OpenedTemplate(path_, newNumber_);
    path_.clear();
  } else if (!fromBytes_) {
    file_->Opened(path_, book->GetPackage()->GetSnapshot().GetStamp());
  }
  const OpenResult r = Opened(std::move(book), error);
  // Someone else has it open in Excel: changes can only be saved as a copy.
  if (r == OpenResult::Ok)
    if (std::wstring inUse = file_->InUseMessage(); !inUse.empty()) host_.ViewMessage(BannerKind::Warning, inUse);
  return r;
}

OpenResult WorkbookView::Opened(std::unique_ptr<Workbook> book, std::wstring& error) {
  const std::wstring name = Title();
  book_ = std::move(book);
  fonts_ = std::make_unique<text::Fonts>();
  measure_ = std::make_unique<FontMeasure>(*fonts_);
  text_ = std::make_unique<CellText>(*book_, *fonts_, *measure_, UserLocale());
  states_.assign(book_->SheetCount(), PerSheet{});
  sheetOrder_.clear();
  for (size_t i = 0; i < book_->SheetCount(); ++i) sheetOrder_.push_back(&book_->GetSheet(i));
  objects_.clear();
  objects_.resize(book_->SheetCount());
  rules_.clear();
  rules_.resize(book_->SheetCount());
  rulesRead_.assign(book_->SheetCount(), false);
  size_t first = book_->ActiveSheet();
  if (first >= book_->SheetCount() || !SheetShown(first)) first = NextShownSheet(book_->SheetCount() - 1, 1);
  if (book_->SheetCount() == 0 || first >= book_->SheetCount() || !SheetShown(first)) {
    book_.reset();
    error = name + L" couldn't be opened — it has no sheet to show.";
    return OpenResult::Failed;
  }
  sheet_ = first;
  ShowSheet(first);
  return OpenResult::Ok;
}

bool WorkbookView::SheetShown(size_t i) const {
  if (!book_ || i >= book_->SheetCount()) return false;
  const Sheet& s = book_->GetSheet(i);
  return s.State() == SheetState::Visible && (s.Kind() == SheetKind::Worksheet || s.Kind() == SheetKind::Chartsheet);
}

size_t WorkbookView::NextShownSheet(size_t from, int dir) const {
  const size_t n = book_ ? book_->SheetCount() : 0;
  for (size_t step = 1; step <= n; ++step) {
    const size_t i = (from + n + (dir > 0 ? step : n - step)) % n;
    if (SheetShown(i)) return i;
  }
  return from;
}

const fw::sheet::SheetView* WorkbookView::ViewOf() const {
  const auto& views = Current().Views();
  return views.empty() ? nullptr : &views.front();
}

void WorkbookView::ShowSheet(size_t index) {
  if (!book_ || index >= book_->SheetCount()) return;
  if (shown_ && sheet_ < states_.size()) {
    // Keep where the sheet being left was.
    PerSheet& was = states_[sheet_];
    was.active = active_;
    was.end = end_;
    was.sx = gridX_;
    was.sy = gridY_;
    was.zoom = zoom_;
  }
  shown_ = true;
  if (preview_) {
    preview_ = false;
    print_.reset();
  }
  sheet_ = index;
  Sheet& sheet = book_->GetSheet(index);
  if (!sheet.Loaded()) book_->LoadSheet(index);
  text_->Clear();
  grid_.Build(sheet, book_->GetStyles(), *measure_);
  BuildGroups();
  if (states_[index].groupsKept) {
    // As expanded and collapsed when the sheet was last shown.
    rowGroups_ = states_[index].rowGroups;
    colGroups_ = states_[index].colGroups;
    ApplyGroups(true);
    ApplyGroups(false);
  }
  if (index < objects_.size() && !objects_[index]) objects_[index] = SheetObjects::Load(*book_, index);
  note_.reset();
  PlaceObjects();
  LoadView();
  if (ChartSheet()) headers_ = false;  // the chart fills the window
  totals_ = Totals{};
  if (!query_.empty()) StartSearch(query_, matchCase_, wholeWords_);
  LayoutParts();
  ClampScroll();
  host_.ViewStateChanged();
  host_.ViewInvalidate();
}

// The screen's columns and rows for the objects layer: points at 100 % from A1.
class ScreenSheetLayout : public SheetLayout {
 public:
  explicit ScreenSheetLayout(const Grid& grid) : grid_(grid) {}
  double ColumnLeft(uint32_t col) const override { return grid_.ColLeft(col) * kPtPerPx; }
  double RowTop(uint32_t row) const override { return grid_.RowTop(row) * kPtPerPx; }

 private:
  const Grid& grid_;
};

// The formula engine on the workbook, made when first needed (the open recalculation reads the
// formulas a slice at a time; conditional formats only need the names it starts with).
calc::WorkbookCalc& WorkbookView::Calc() {
  if (!calc_) {
    calc_ = std::make_unique<calc::WorkbookCalc>(*book_, &text_->Locale());
    // A save reads the sheets on a worker thread: nothing is recalculated into them meanwhile.
    calc_->HoldWhile([this] { return edit_ && edit_->Saving(); });
  }
  return *calc_;
}

condfmt::SheetRules* WorkbookView::RulesFor(size_t index) {
  if (!book_ || index >= rules_.size()) return nullptr;
  if (!rulesRead_[index]) {
    rulesRead_[index] = true;
    Sheet& sheet = book_->GetSheet(index);
    if (!sheet.Loaded()) book_->LoadSheet(index);
    if (sheet.KeptXml().find("conditionalFormatting") != std::string::npos) {
      condfmt::Options o;
      o.locale = &text_->Locale();
      o.calc = &Calc();
      rules_[index] = std::make_unique<condfmt::SheetRules>(*book_, index, o);
      if (rules_[index]->Empty()) rules_[index].reset();
    }
  }
  return rules_[index].get();
}

void WorkbookView::PlaceObjects() {
  if (SheetObjects* o = Objects()) o->Place(ScreenSheetLayout(grid_));
}

void WorkbookView::LoadView() {
  const Sheet& sheet = Current();
  PerSheet& st = states_[sheet_];
  const fw::sheet::SheetView* v = ViewOf();
  fw::sheet::SheetView def;
  if (!v) v = &def;
  gridlines_ = v->showGridLines;
  headers_ = v->showRowColHeaders;
  showZeros_ = v->showZeros;
  rtl_ = v->rightToLeft;
  gridColor_ = 0xFFD9D9D9;
  if (!v->defaultGridColor) {
    const auto& palette = book_->GetStyles().Palette();
    if (v->colorId < palette.size() && v->colorId != 64) gridColor_ = 0xFF000000u | palette[v->colorId];
  }
  zoom_ = std::clamp(v->zoomScale ? v->zoomScale : 100u, 10u, 400u) / 100.0;
  if (st.zoom > 0) zoom_ = st.zoom;

  frozenRow0_ = frozenRow1_ = frozenCol0_ = frozenCol1_ = 0;
  CellAddr scrollFrom = v->topLeft;
  if (v->paneState == PaneState::Frozen || v->paneState == PaneState::FrozenSplit) {
    const uint32_t xs = static_cast<uint32_t>(std::max(0.0, v->xSplit));
    const uint32_t ys = static_cast<uint32_t>(std::max(0.0, v->ySplit));
    if (ys > 0) {
      frozenRow0_ = v->topLeft.row;
      frozenRow1_ = std::min(kMaxRows - 1, v->topLeft.row + ys);
    }
    if (xs > 0) {
      frozenCol0_ = v->topLeft.col;
      frozenCol1_ = std::min(kMaxCols - 1, v->topLeft.col + xs);
    }
    scrollFrom = v->paneTopLeft;
    if (ys > 0) scrollFrom.row = std::max(scrollFrom.row, frozenRow1_);
    else scrollFrom.row = v->topLeft.row;
    if (xs > 0) scrollFrom.col = std::max(scrollFrom.col, frozenCol1_);
    else scrollFrom.col = v->topLeft.col;
  }
  gridX_ = static_cast<double>(grid_.ColLeft(scrollFrom.col));
  gridY_ = static_cast<double>(grid_.RowTop(scrollFrom.row));
  if (st.sx >= 0) gridX_ = st.sx, gridY_ = st.sy;

  // The selection: the file's for the active pane, else A1.
  active_ = end_ = {0, 0};
  if (st.sx >= 0) {
    active_ = st.active;
    end_ = st.end;
  } else {
    const Selection* sel = nullptr;
    for (const Selection& s : v->selections)
      if (s.pane == v->activePane) sel = &s;
    if (!sel && !v->selections.empty()) sel = &v->selections.front();
    if (sel) {
      active_ = end_ = sel->active;
      for (const Range& r : sel->ranges) {
        if (!r.Contains(active_.row, active_.col)) continue;
        end_ = {r.firstRow == active_.row ? r.lastRow : r.firstRow, r.firstCol == active_.col ? r.lastCol : r.firstCol};
        break;
      }
    }
  }
  (void)sheet;
}

// ---------------------------------------------------------------------------
// Layout

void WorkbookView::SetViewport(const RECT& rc, float ui) {
  full_ = rc;
  PagedView::SetViewport(rc, ui);
  if (Ready()) {
    LayoutParts();
    ClampScroll();
  }
}

void WorkbookView::LayoutParts() {
  const float u = ui_;
  const D2D1_RECT_F all = RectF(full_);
  const float fb = std::round(32 * u), tb = std::round(30 * u);
  formulaBar_ = {all.left, all.top, all.right, all.top + fb};
  nameBox_ = {all.left + 8 * u, all.top + 4 * u, all.left + 8 * u + std::round(96 * u), all.top + fb - 4 * u};
  tabs_ = {all.left, all.bottom - tb, all.right, all.bottom};
  grid_rc_ = {all.left, formulaBar_.bottom, all.right, tabs_.top};

  const double k = K();
  const int rowLevels = OutlineLevels(true), colLevels = OutlineLevels(false);
  const float rowGutter = rowLevels ? std::round((rowLevels + 1) * 14 * u + 4 * u) : 0;
  const float colGutter = colLevels ? std::round((colLevels + 1) * 14 * u + 4 * u) : 0;
  float headH = 0, headW = 0;
  if (headers_) {
    headH = std::max(std::round(static_cast<float>(grid_.DefaultRowPx() * k)), std::round(14 * u));
    // Wide enough for the numbers of the rows that may show.
    const double rows = (grid_rc_.bottom - grid_rc_.top) / std::max(1.0, grid_.DefaultRowPx() * k);
    const uint32_t lastRow = std::min<uint32_t>(kMaxRows, grid_.RowAt(static_cast<int64_t>(gridY_)) + static_cast<uint32_t>(rows) + 2);
    const int digits = std::max(3, static_cast<int>(std::to_string(lastRow).size()));
    headW = std::round(static_cast<float>((digits * grid_.MaxDigit() + 10) * k));
  }
  // Left to right; mirrored for right-to-left sheets below.
  float left = grid_rc_.left, top = grid_rc_.top;
  colGutter_ = {left + rowGutter, top, grid_rc_.right, top + colGutter};
  rowGutter_ = {left, top + colGutter, left + rowGutter, grid_rc_.bottom};
  corner_ = {left + rowGutter, top + colGutter, left + rowGutter + headW, top + colGutter + headH};
  colHeader_ = {corner_.right, corner_.top, grid_rc_.right, corner_.bottom};
  rowHeader_ = {corner_.left, corner_.bottom, corner_.right, grid_rc_.bottom};
  cells_ = {corner_.right, corner_.bottom, grid_rc_.right, grid_rc_.bottom};
  if (rtl_) {
    auto flip = [&](D2D1_RECT_F& r) {
      const float l = grid_rc_.left + grid_rc_.right - r.right, rr = grid_rc_.left + grid_rc_.right - r.left;
      r.left = l;
      r.right = rr;
    };
    flip(colGutter_);
    flip(rowGutter_);
    flip(corner_);
    flip(colHeader_);
    flip(rowHeader_);
    flip(cells_);
  }
}

int WorkbookView::FrozenWidthPx() const {
  if (frozenCol1_ <= frozenCol0_) return 0;
  return static_cast<int>(std::round((grid_.ColLeft(frozenCol1_) - grid_.ColLeft(frozenCol0_)) * K()));
}

int WorkbookView::FrozenHeightPx() const {
  if (frozenRow1_ <= frozenRow0_) return 0;
  return static_cast<int>(std::round((grid_.RowTop(frozenRow1_) - grid_.RowTop(frozenRow0_)) * K()));
}

std::vector<WorkbookView::Pane> WorkbookView::Panes() const {
  std::vector<Pane> out;
  // In left-to-right coordinates of the cells area (mirrored when drawn in a right-to-left sheet).
  const LONG l = static_cast<LONG>(cells_.left), t = static_cast<LONG>(cells_.top);
  const LONG r = static_cast<LONG>(cells_.right), b = static_cast<LONG>(cells_.bottom);
  const LONG fw = std::min<LONG>(FrozenWidthPx(), r - l), fh = std::min<LONG>(FrozenHeightPx(), b - t);
  const double fx = static_cast<double>(grid_.ColLeft(frozenCol0_)), fy = static_cast<double>(grid_.RowTop(frozenRow0_));
  if (fw > 0 && fh > 0) out.push_back({{l, t, l + fw, t + fh}, frozenRow0_, frozenCol0_, frozenRow1_, frozenCol1_, fx, fy});
  if (fh > 0) out.push_back({{l + fw, t, r, t + fh}, frozenRow0_, frozenCol1_, frozenRow1_, kMaxCols, gridX_, fy, true, false});
  if (fw > 0) out.push_back({{l, t + fh, l + fw, b}, frozenRow1_, frozenCol0_, kMaxRows, frozenCol1_, fx, gridY_, false, true});
  out.push_back({{l + fw, t + fh, r, b}, frozenRow1_, frozenCol1_, kMaxRows, kMaxCols, gridX_, gridY_, true, true});
  return out;
}

float WorkbookView::PaneX(const Pane& p, uint32_t col) const {
  return static_cast<float>(p.rc.left + std::round((grid_.ColLeft(col) - p.sx) * K()));
}

float WorkbookView::PaneY(const Pane& p, uint32_t row) const {
  return static_cast<float>(p.rc.top + std::round((grid_.RowTop(row) - p.sy) * K()));
}

D2D1_RECT_F WorkbookView::CellRect(const Pane& p, uint32_t r0, uint32_t c0, uint32_t r1, uint32_t c1) const {
  float l = PaneX(p, c0), r = PaneX(p, c1 + 1);
  if (rtl_) {
    const float a = Mirror(r), b = Mirror(l);
    l = a;
    r = b;
  }
  return {l, PaneY(p, r0), r, PaneY(p, r1 + 1)};
}

void WorkbookView::ClampScroll() {
  if (!Ready()) return;
  const double k = K();
  const double minX = static_cast<double>(grid_.ColLeft(frozenCol1_)), minY = static_cast<double>(grid_.RowTop(frozenRow1_));
  const double w = std::max(0.0, (cells_.right - cells_.left - FrozenWidthPx()) / k);
  const double h = std::max(0.0, (cells_.bottom - cells_.top - FrozenHeightPx()) / k);
  gridX_ = std::clamp(gridX_, minX, std::max(minX, static_cast<double>(grid_.Width()) - w));
  gridY_ = std::clamp(gridY_, minY, std::max(minY, static_cast<double>(grid_.Height()) - h));
}

void WorkbookView::ScrollTo(double sx, double sy) {
  gridX_ = sx;
  gridY_ = sy;
  ClampScroll();
  LayoutParts();  // the row headers may need another digit
  host_.ViewInvalidate();
}

void WorkbookView::ScrollIntoView(CellAddr cell) {
  const double k = K();
  const double w = (cells_.right - cells_.left - FrozenWidthPx()) / k;
  const double h = (cells_.bottom - cells_.top - FrozenHeightPx()) / k;
  double sx = gridX_, sy = gridY_;
  if (cell.row >= frozenRow1_) {
    const double top = static_cast<double>(grid_.RowTop(cell.row)), bottom = static_cast<double>(grid_.RowTop(cell.row + 1));
    if (top < sy) sy = top;
    else if (bottom > sy + h) sy = std::min(top, bottom - h);
  }
  if (cell.col >= frozenCol1_) {
    const double left = static_cast<double>(grid_.ColLeft(cell.col)), right = static_cast<double>(grid_.ColLeft(cell.col + 1));
    if (left < sx) sx = left;
    else if (right > sx + w) sx = std::min(left, right - w);
  }
  if (sx != gridX_ || sy != gridY_) ScrollTo(sx, sy);
}

std::optional<CellAddr> WorkbookView::CellAt(int x, int y) const {
  if (!Ready()) return std::nullopt;
  const float fx = Mirror(static_cast<float>(x));  // into left-to-right coordinates
  for (const Pane& p : Panes()) {
    if (fx < p.rc.left || fx >= p.rc.right || y < p.rc.top || y >= p.rc.bottom) continue;
    const double k = K();
    uint32_t c = grid_.ColAt(static_cast<int64_t>(std::floor(p.sx + (fx - p.rc.left) / k)));
    uint32_t r = grid_.RowAt(static_cast<int64_t>(std::floor(p.sy + (y - p.rc.top) / k)));
    c = std::clamp(c, p.col0, std::max(p.col0, std::min(p.col1, kMaxCols) - 1));
    r = std::clamp(r, p.row0, std::max(p.row0, std::min(p.row1, kMaxRows) - 1));
    return CellAddr{r, c};
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Painting

void WorkbookView::Draw(Canvas& c, ID2D1DeviceContext* dc) {
  const Palette& p = theme::P();
  const D2D1_RECT_F all = RectF(full_);
  c.PushClip(all);
  if (preview_) {
    DrawPreview(c, dc);
    c.PopClip();
    return;
  }
  c.Fill(all, p.chrome);
  if (Ready() && ChartSheet()) {
    c.Fill(cells_, Rgb(0xFFFFFF));
    {
      draw::D2DPainter painter(dc);
      painter.ClipRect({cells_.left, cells_.top, cells_.right, cells_.bottom});
      Objects()->DrawChartSheet(painter, {cells_.left, cells_.top, cells_.right, cells_.bottom});
    }
    DrawFormulaBar(c);
    DrawSheetTabs(c);
  } else if (Ready()) {
    DrawGrid(c, dc);
    DrawStale(c);
    DrawFillHandle(c);
    DrawFilterButtons(c);
    DrawValidation(c);
    DrawSizeGuide(c);
    DrawTabDrop(c);
    DrawNote(dc);
    DrawEditor(c);
    DrawHeaders(c);
    DrawOutline(c);
    DrawScrollbars(c);
    DrawFormulaBar(c);
    DrawSheetTabs(c);
    if (renaming_ >= 0) DrawEditBox(c, tabs_);
    DrawFormulaHelp(c);
  }
  c.PopClip();
}

void WorkbookView::DrawGrid(Canvas& c, ID2D1DeviceContext* dc) {
  c.Fill(cells_, Rgb(0xFFFFFF));
  const Sheet& sheet = Current();
  PaintOptions o;
  o.gridlines = gridlines_;
  o.gridColor = gridColor_;
  o.showZeros = showZeros_;
  o.rules = RulesFor(sheet_);
  const double k = K();
  const float mirror = cells_.left + cells_.right;
  for (const Pane& pane : Panes()) {
    if (pane.rc.right <= pane.rc.left || pane.rc.bottom <= pane.rc.top) continue;
    const float l = static_cast<float>(pane.rc.left), t = static_cast<float>(pane.rc.top);
    const ScreenGeometry g(grid_, l, t, pane.sx, pane.sy, k, rtl_, mirror);
    // The cells it shows.
    Range area;
    area.firstRow = std::max(pane.row0, grid_.RowAt(static_cast<int64_t>(pane.sy)));
    area.lastRow = std::min(std::min(pane.row1, kMaxRows) - 1,
                            grid_.RowAt(static_cast<int64_t>(pane.sy + (pane.rc.bottom - pane.rc.top) / k)));
    area.firstCol = std::max(pane.col0, grid_.ColAt(static_cast<int64_t>(pane.sx)));
    area.lastCol = std::min(std::min(pane.col1, kMaxCols) - 1,
                            grid_.ColAt(static_cast<int64_t>(pane.sx + (pane.rc.right - pane.rc.left) / k)));
    if (area.lastRow < area.firstRow || area.lastCol < area.firstCol) continue;
    D2D1_RECT_F rc = RectF(pane.rc);
    if (rtl_) {
      const float a = mirror - rc.right, b = mirror - rc.left;
      rc.left = a;
      rc.right = b;
    }
    {
      draw::D2DPainter painter(dc);
      painter.ClipRect({rc.left, rc.top, rc.right, rc.bottom});
      PaintCells(painter, sheet, *text_, grid_, g, area, o);
      // Pictures, shapes, charts and the notes' marks, from sheet points to the pane. A sheet read
      // right to left mirrors where they go (about the pane's mirror line), not what they show.
      if (SheetObjects* objects = Objects()) {
        const float s = static_cast<float>(k / kPtPerPx);
        const float tx = l - static_cast<float>(pane.sx * k);
        painter.Save();
        painter.Transform(draw::Matrix::Scale(s, s) * draw::Matrix::Translate(tx, t - static_cast<float>(pane.sy * k)));
        const float x0 = static_cast<float>(pane.sx * kPtPerPx), y0 = static_cast<float>(pane.sy * kPtPerPx);
        SheetObjects::DrawOptions d;
        if (rtl_) d.mirrorX = (mirror - 2 * tx) / s;
        objects->Draw(painter, {x0, y0, x0 + (rc.right - rc.left) / s, y0 + (rc.bottom - rc.top) / s}, d);
        painter.Restore();
      }
    }
    c.PushClip(rc);
    DrawSelection(c, pane);
    DrawRefBorders(c, pane);
    DrawClipBorder(c, pane);
    c.PopClip();
  }
  // The frozen panes' edges.
  const D2D1_COLOR_F edge = Rgb(0x9A9A9A);
  if (const int fw = FrozenWidthPx(); fw > 0) {
    const float x = Mirror(cells_.left + fw) - (rtl_ ? 0 : 1);
    c.Fill({x, cells_.top, x + 1, cells_.bottom}, edge);
  }
  if (const int fh = FrozenHeightPx(); fh > 0) c.Fill({cells_.left, cells_.top + fh - 1, cells_.right, cells_.top + fh}, edge);
}

bool WorkbookView::Stale(uint32_t row, uint32_t col) const {
  return calc_ && calc_->engine().StaleCount() > 0 &&
         calc_->engine().Stale(static_cast<int>(sheet_), static_cast<int32_t>(row), static_cast<int32_t>(col));
}

void WorkbookView::DrawStale(Canvas& c) {
  if (!calc_ || calc_->engine().StaleCount() == 0) return;
  const double k = K();
  const float u = static_cast<float>(std::max(1.0, std::floor(k + 0.01)));
  const D2D1_COLOR_F gray = Rgb(0x808080);
  for (const Pane& pane : Panes()) {
    if (pane.rc.right <= pane.rc.left || pane.rc.bottom <= pane.rc.top) continue;
    Range area;
    area.firstRow = std::max(pane.row0, grid_.RowAt(static_cast<int64_t>(pane.sy)));
    area.lastRow = std::min(std::min(pane.row1, kMaxRows) - 1,
                            grid_.RowAt(static_cast<int64_t>(pane.sy + (pane.rc.bottom - pane.rc.top) / k)));
    area.firstCol = std::max(pane.col0, grid_.ColAt(static_cast<int64_t>(pane.sx)));
    area.lastCol = std::min(std::min(pane.col1, kMaxCols) - 1,
                            grid_.ColAt(static_cast<int64_t>(pane.sx + (pane.rc.right - pane.rc.left) / k)));
    if (area.firstRow > area.lastRow || area.firstCol > area.lastCol) continue;
    c.PushClip(RectF(pane.rc));
    Current().ForEachCell(area, [&](const CellInfo& cell) {
      if (!cell.hasFormula || !Stale(cell.row, cell.col)) return true;
      const D2D1_RECT_F r = CellRect(pane, cell.row, cell.col, cell.row, cell.col);
      // A small triangle in the corner where the text starts.
      for (int i = 0; i < 5; ++i) {
        const float y = r.top + 1 + i * u, w = (5 - i) * u;
        c.Fill(rtl_ ? D2D1::RectF(r.right - 1 - w, y, r.right - 1, y + u) : D2D1::RectF(r.left + 1, y, r.left + 1 + w, y + u), gray);
      }
      return true;
    });
    c.PopClip();
  }
}

void WorkbookView::DrawNote(ID2D1DeviceContext* dc) {
  SheetObjects* objects = Objects();
  if (!note_ || !objects) return;
  const CellNote* note = objects->NoteAt(note_->row, note_->col);
  if (!note) return;
  // Beside the cell (or its merged area), at its top, in the pane that shows it.
  Range at = Range::Cell(note_->row, note_->col);
  if (const Range* m = Current().MergeAt(note_->row, note_->col)) at = *m;
  for (const Pane& pane : Panes()) {
    if (at.firstRow < pane.row0 || at.firstRow >= pane.row1 || at.lastCol < pane.col0 || at.firstCol >= pane.col1) continue;
    const D2D1_RECT_F cell = CellRect(pane, at.firstRow, at.firstCol, at.lastRow, at.lastCol);
    const float s = static_cast<float>(K() / kPtPerPx);
    const draw::Size size = objects->NoteSize(*note);
    float x = (rtl_ ? cell.left - 12 * s - size.width * s : cell.right + 12 * s) / s, y = cell.top / s;
    // Kept inside the cells.
    x = std::clamp(x, cells_.left / s, std::max(cells_.left / s, cells_.right / s - size.width));
    y = std::clamp(y, cells_.top / s, std::max(cells_.top / s, cells_.bottom / s - size.height));
    draw::D2DPainter painter(dc);
    painter.ClipRect({cells_.left, cells_.top, cells_.right, cells_.bottom});
    painter.Transform(draw::Matrix::Scale(s, s));
    objects->DrawNote(painter, *note, {x, y, x + size.width, y + size.height});
    return;
  }
}

void WorkbookView::DrawSelection(Canvas& c, const Pane& pane) {
  const Palette& p = theme::P();
  const Range sel = Selected();
  // Only the part in this pane.
  const uint32_t r0 = std::max(sel.firstRow, pane.row0), r1 = std::min(sel.lastRow, std::min(pane.row1, kMaxRows) - 1);
  const uint32_t c0 = std::max(sel.firstCol, pane.col0), c1 = std::min(sel.lastCol, std::min(pane.col1, kMaxCols) - 1);
  if (r0 > r1 || c0 > c1) return;
  const D2D1_RECT_F all = CellRect(pane, r0, c0, r1, c1);
  const bool single = sel.firstRow == sel.lastRow && sel.firstCol == sel.lastCol;
  const Range* m = Current().MergeAt(active_.row, active_.col);
  const bool oneCell = single || (m && *m == sel);
  if (!oneCell) {
    // The range is shaded, the active cell not.
    const D2D1_COLOR_F shade = Alpha(p.accent, 0.14f);
    Range act = m ? *m : Range::Cell(active_.row, active_.col);
    const D2D1_RECT_F a = CellRect(pane, act.firstRow, act.firstCol, act.lastRow, act.lastCol);
    c.Fill({all.left, all.top, all.right, a.top}, shade);
    c.Fill({all.left, a.bottom, all.right, all.bottom}, shade);
    c.Fill({all.left, std::max(all.top, a.top), a.left, std::min(all.bottom, a.bottom)}, shade);
    c.Fill({a.right, std::max(all.top, a.top), all.right, std::min(all.bottom, a.bottom)}, shade);
  }
  // A border around it all, only on the edges that are the selection's own.
  const D2D1_RECT_F full = CellRect(pane, sel.firstRow, sel.firstCol, sel.lastRow, sel.lastCol);
  const float w = std::max(2.f, std::round(2 * ui_));
  const D2D1_COLOR_F ink = p.accent;
  const bool rtl = rtl_;
  auto edge = [&](bool show, D2D1_RECT_F r) {
    if (show) c.Fill(r, ink);
  };
  const bool leftEdge = rtl ? sel.lastCol <= c1 : sel.firstCol >= c0;
  const bool rightEdge = rtl ? sel.firstCol >= c0 : sel.lastCol <= c1;
  edge(sel.firstRow >= r0, {full.left - w / 2, full.top - w / 2, full.right + w / 2, full.top + w / 2});
  edge(sel.lastRow <= r1, {full.left - w / 2, full.bottom - w / 2 - 1, full.right + w / 2, full.bottom + w / 2 - 1});
  edge(leftEdge, {full.left - w / 2, full.top - w / 2, full.left + w / 2, full.bottom + w / 2});
  edge(rightEdge, {full.right - w / 2 - 1, full.top - w / 2, full.right + w / 2 - 1, full.bottom + w / 2});
}

void WorkbookView::DrawHeaders(Canvas& c) {
  if (!headers_) return;
  const Palette& p = theme::P();
  const D2D1_COLOR_F bg = p.dark ? Mix(p.chrome, Rgb(0x000000), 0.15f) : Rgb(0xF3F3F3);
  const D2D1_COLOR_F line = p.dark ? Alpha(p.chromeBorder, 0.9f) : Rgb(0xD0D0D0);
  const D2D1_COLOR_F ink = p.dark ? p.textSecondary : Rgb(0x444444);
  const D2D1_COLOR_F selBg = Alpha(p.accent, 0.18f);
  const Range sel = Selected();
  const double k = K();
  const float size = static_cast<float>(std::clamp(11.0 * 4 / 3 * k * 0.92, 6.0, 40.0));
  c.Fill(colHeader_, bg);
  c.Fill(rowHeader_, bg);
  c.Fill(corner_, bg);
  // The corner's triangle selects everything.
  c.Line(corner_.right - 1, corner_.top, corner_.right - 1, corner_.bottom, line, 1);
  c.Line(corner_.left, corner_.bottom - 1, corner_.right, corner_.bottom - 1, line, 1);
  const bool allCols = sel.firstRow == 0 && sel.lastRow == kMaxRows - 1;
  const bool allRows = sel.firstCol == 0 && sel.lastCol == kMaxCols - 1;
  for (const Pane& pane : Panes()) {
    // Column letters above the panes in the top row of panes; row numbers left of the left ones.
    if (pane.rc.top == static_cast<LONG>(cells_.top)) {
      D2D1_RECT_F clip = {Mirror(static_cast<float>(rtl_ ? pane.rc.right : pane.rc.left)), colHeader_.top,
                          Mirror(static_cast<float>(rtl_ ? pane.rc.left : pane.rc.right)), colHeader_.bottom};
      c.PushClip(clip);
      const uint32_t first = std::max(pane.col0, grid_.ColAt(static_cast<int64_t>(pane.sx)));
      const uint32_t last = std::min(std::min(pane.col1, kMaxCols) - 1,
                                     grid_.ColAt(static_cast<int64_t>(pane.sx + (pane.rc.right - pane.rc.left) / k)));
      for (uint32_t col = first; col <= last; ++col) {
        if (grid_.ColHidden(col)) continue;
        const D2D1_RECT_F r = CellRect(pane, 0, col, 0, col);
        const D2D1_RECT_F h = {r.left, colHeader_.top, r.right, colHeader_.bottom};
        const bool on = col >= sel.firstCol && col <= sel.lastCol;
        if (on) {
          c.Fill(h, selBg);
          if (allCols) c.Fill(h, Alpha(p.accent, 0.25f));
          c.Fill({h.left, h.bottom - 2 * ui_, h.right, h.bottom}, p.accent);
        }
        c.Fill({rtl_ ? h.left : h.right - 1, h.top, rtl_ ? h.left + 1 : h.right, h.bottom}, line);
        c.Text(Utf8ToWide(ColumnName(col)), h, on ? p.accentText : ink, size, DWRITE_FONT_WEIGHT_REGULAR, Align::Center);
      }
      c.PopClip();
    }
    if (pane.rc.left == static_cast<LONG>(cells_.left)) {
      D2D1_RECT_F clip = {rowHeader_.left, static_cast<float>(pane.rc.top), rowHeader_.right, static_cast<float>(pane.rc.bottom)};
      c.PushClip(clip);
      const uint32_t first = std::max(pane.row0, grid_.RowAt(static_cast<int64_t>(pane.sy)));
      const uint32_t last = std::min(std::min(pane.row1, kMaxRows) - 1,
                                     grid_.RowAt(static_cast<int64_t>(pane.sy + (pane.rc.bottom - pane.rc.top) / k)));
      for (uint32_t row = first; row <= last; ++row) {
        if (grid_.RowHidden(row)) continue;
        const float t = PaneY(pane, row), b = PaneY(pane, row + 1);
        const D2D1_RECT_F h = {rowHeader_.left, t, rowHeader_.right, b};
        const bool on = row >= sel.firstRow && row <= sel.lastRow;
        if (on) {
          c.Fill(h, selBg);
          if (allRows) c.Fill(h, Alpha(p.accent, 0.25f));
          const float x = rtl_ ? h.left : h.right - 2 * ui_;
          c.Fill({x, h.top, x + 2 * ui_, h.bottom}, p.accent);
        }
        c.Fill({h.left, b - 1, h.right, b}, line);
        c.Text(std::to_wstring(row + 1), h, on ? p.accentText : ink, size, DWRITE_FONT_WEIGHT_REGULAR, Align::Center);
      }
      c.PopClip();
    }
  }
  // Their edges along the cells.
  c.Fill({colHeader_.left, colHeader_.bottom - 1, colHeader_.right, colHeader_.bottom}, line);
  const float x = rtl_ ? rowHeader_.left : rowHeader_.right - 1;
  c.Fill({x, rowHeader_.top, x + 1, rowHeader_.bottom}, line);
}

void WorkbookView::DrawFormulaBar(Canvas& c) {
  const Palette& p = theme::P();
  const float u = ui_;
  c.Fill(formulaBar_, p.chrome);
  c.Fill({formulaBar_.left, formulaBar_.bottom - 1, formulaBar_.right, formulaBar_.bottom}, p.chromeBorder);
  c.FillRound(nameBox_, 4 * u, p.inputBg);
  c.StrokeRound(nameBox_, 4 * u, p.inputBorder, 1);
  const float size = FormulaBarSize();
  c.Text(ActiveName(), Inflate(nameBox_, -8 * u, 0), p.text, size);
  const D2D1_RECT_F fx = {nameBox_.right + 8 * u, formulaBar_.top, nameBox_.right + 36 * u, formulaBar_.bottom};
  c.Text(L"fx", fx, p.textSecondary, size, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center);
  c.Fill({fx.right + 4 * u, formulaBar_.top + 6 * u, fx.right + 5 * u, formulaBar_.bottom - 6 * u}, p.chromeBorder);
  DrawBarText(c);
}

D2D1_RECT_F WorkbookView::FormulaBox() const {
  const float u = ui_;
  return {nameBox_.right + 48 * u, formulaBar_.top, formulaBar_.right - 12 * u, formulaBar_.bottom};
}

std::vector<WorkbookView::TabRect> WorkbookView::TabRects() const {
  std::vector<TabRect> out;
  if (!book_) return out;
  const float u = ui_;
  const float size = 12.5f * u;
  Canvas measure;  // measuring needs no device
  float x = tabs_.left + 64 * u - tabScroll_;
  for (size_t i = 0; i < book_->SheetCount(); ++i) {
    if (!SheetShown(i)) continue;
    const std::wstring name = Utf8ToWide(book_->GetSheet(i).Name());
    const DWRITE_FONT_WEIGHT weight = i == sheet_ ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_REGULAR;
    const float w = std::max(48 * u, std::ceil(measure.MeasureText(name, size, weight)) + 28 * u);
    out.push_back({i, {x, tabs_.top + 1, x + w, tabs_.bottom - 3 * u}});
    x += w;
  }
  return out;
}

void WorkbookView::DrawSheetTabs(Canvas& c) {
  const Palette& p = theme::P();
  const float u = ui_;
  c.Fill(tabs_, p.chrome);
  c.Fill({tabs_.left, tabs_.top, tabs_.right, tabs_.top + 1}, p.chromeBorder);
  // ◂ ▸ go to the previous and next sheet.
  const D2D1_RECT_F prev = {tabs_.left + 6 * u, tabs_.top, tabs_.left + 30 * u, tabs_.bottom};
  const D2D1_RECT_F next = {prev.right, tabs_.top, prev.right + 24 * u, tabs_.bottom};
  c.Icon(icon::ChevronLeft, prev, p.textSecondary, 10 * u);
  c.Icon(icon::ChevronRight, next, p.textSecondary, 10 * u);
  const float size = 12.5f * u;
  c.PushClip({tabs_.left + 60 * u, tabs_.top, tabs_.right, tabs_.bottom});
  for (const TabRect& t : TabRects()) {
    const Sheet& s = book_->GetSheet(t.sheet);
    const bool active = t.sheet == sheet_;
    const DWRITE_FONT_WEIGHT weight = active ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_REGULAR;
    const D2D1_RECT_F& r = t.rc;
    if (active) {
      c.Fill(r, p.dark ? Mix(p.chrome, Rgb(0xFFFFFF), 0.08f) : Rgb(0xFFFFFF));
      c.Fill({r.left + 8 * u, r.bottom - 2 * u, r.right - 8 * u, r.bottom}, p.accent);
    } else if (static_cast<int>(t.sheet) == hoverTab_) {
      c.Fill(r, p.hover);
    }
    if (s.Props().tabColor.IsSet()) {
      const uint32_t argb = book_->GetStyles().Resolve(s.Props().tabColor, 0xFF808080);
      c.Fill({r.left + 2 * u, r.bottom - (active ? 5 : 4) * u, r.right - 2 * u, r.bottom - (active ? 3 : 0) * u},
             Rgb(argb & 0xFFFFFF));
    }
    c.Text(Utf8ToWide(s.Name()), r, active ? p.accentText : p.text, size, weight, Align::Center);
    c.Fill({r.right - 1, r.top + 6 * u, r.right, r.bottom - 6 * u}, p.chromeBorder);
  }
  c.PopClip();
}

bool WorkbookView::Bars(Bar& v, Bar& h) const {
  v = h = Bar{};
  if (!Ready()) return false;
  const double k = K();
  const float u = ui_;
  const float thick = 6 * u, inset = 3 * u, track = 14 * u;
  const double viewH = (cells_.bottom - cells_.top - FrozenHeightPx()) / k;
  const double viewW = (cells_.right - cells_.left - FrozenWidthPx()) / k;
  const auto used = Current().UsedRange();
  // Pictures and charts past the cells count too.
  double objRight = 0, objBottom = 0;
  if (SheetObjects* o = Objects())
    for (const SheetObjects::Object& ob : o->Objects()) {
      objRight = std::max(objRight, static_cast<double>(ob.rect.right) / kPtPerPx);
      objBottom = std::max(objBottom, static_cast<double>(ob.rect.bottom) / kPtPerPx);
    }
  v.min = static_cast<double>(grid_.RowTop(frozenRow1_));
  h.min = static_cast<double>(grid_.ColLeft(frozenCol1_));
  v.view = viewH;
  h.view = viewW;
  // The sheet as far as it's used, half a screen more, or as far as it's been scrolled.
  v.end = std::max(gridY_ + viewH,
                   std::max(objBottom, static_cast<double>(grid_.RowTop(used ? used->lastRow + 1 : 0))) + viewH / 2);
  h.end = std::max(gridX_ + viewW,
                   std::max(objRight, static_cast<double>(grid_.ColLeft(used ? used->lastCol + 1 : 0))) + viewW / 2);
  if (v.end - v.min > viewH + 1) {
    const float top = cells_.top + FrozenHeightPx(), bottom = cells_.bottom - track;
    const float x = rtl_ ? cells_.left : cells_.right - track;
    v.track = {x, top, x + track, bottom};
    const float len = std::max(24 * u, static_cast<float>((bottom - top) * viewH / (v.end - v.min)));
    const float pos = static_cast<float>((gridY_ - v.min) / std::max(1.0, v.end - v.min - viewH)) * (bottom - top - len);
    v.thumb = {x, top + pos, x + track, top + pos + len};
    v.shown = true;
  }
  if (h.end - h.min > viewW + 1) {
    const float left = cells_.left + (rtl_ ? track : FrozenWidthPx()), right = cells_.right - (rtl_ ? FrozenWidthPx() : track);
    h.track = {left, cells_.bottom - track, right, cells_.bottom};
    const float len = std::max(24 * u, static_cast<float>((right - left) * viewW / (h.end - h.min)));
    float pos = static_cast<float>((gridX_ - h.min) / std::max(1.0, h.end - h.min - viewW)) * (right - left - len);
    if (rtl_) pos = right - left - len - pos;
    h.thumb = {left + pos, h.track.top, left + pos + len, h.track.bottom};
    h.shown = true;
  }
  (void)thick;
  (void)inset;
  return v.shown || h.shown;
}

void WorkbookView::DrawScrollbars(Canvas& c) {
  const Palette& p = theme::P();
  Bar v, h;
  if (!Bars(v, h)) return;
  const float u = ui_;
  auto draw = [&](const Bar& b, bool vertical, bool hot) {
    if (!b.shown) return;
    const float thick = (hot ? 8.f : 5.f) * u, inset = 3 * u;
    D2D1_RECT_F r = vertical ? RectF(b.track.right - inset - thick, b.thumb.top + 2 * u, b.track.right - inset, b.thumb.bottom - 2 * u)
                             : RectF(b.thumb.left + 2 * u, b.track.bottom - inset - thick, b.thumb.right - 2 * u, b.track.bottom - inset);
    if (vertical && rtl_) r = RectF(b.track.left + inset, r.top, b.track.left + inset + thick, r.bottom);
    if (hot) c.FillRound(Inflate(b.track, -1.5f * u, -1.5f * u), 6 * u, Alpha(p.scrollTrack, 0.8f));
    c.FillRound(r, thick / 2, hot ? p.scrollThumbHover : p.scrollThumb);
  };
  const bool hotV = mouse_ == Mouse::ThumbV || gfx::Contains(v.track, static_cast<float>(hover_.x), static_cast<float>(hover_.y));
  const bool hotH = mouse_ == Mouse::ThumbH || gfx::Contains(h.track, static_cast<float>(hover_.x), static_cast<float>(hover_.y));
  draw(v, true, hotV);
  draw(h, false, hotH);
}

}  // namespace fw::sheet::view
