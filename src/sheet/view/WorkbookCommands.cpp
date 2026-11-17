// The workbook tab's commands, toolbar, status bar, copy, find, formula bar and links.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Commands.h"
#include "Util.h"
#include "ui/Anim.h"
#include "core/Layout.h"
#include "core/Strings.h"
#include "sheet/view/WorkbookView.h"
#include "ui/Icons.h"
#include "ui/Menu.h"
#include "ui/StatusBar.h"
#include "ui/Toolbar.h"
#include "ui/Uia.h"

namespace fw::sheet::view {

namespace {

struct CommandName {
  int cmd;
  const wchar_t* label;
  const wchar_t* keys;
};

constexpr CommandName kCommandNames[] = {
    {CmdCopy, L"Copy", L"Ctrl+C"},
    {CmdSelectAll, L"Select all", L"Ctrl+A"},
    {CmdPrintPreview, L"Print preview", L"Ctrl+F2"},
    {CmdNextSheet, L"Next sheet", L"Ctrl+Page Down"},
    {CmdPrevSheet, L"Previous sheet", L"Ctrl+Page Up"},
    {CmdZoomIn, L"Zoom in", L"Ctrl+="},
    {CmdZoomOut, L"Zoom out", L"Ctrl+−"},
    {CmdZoomActual, L"Zoom to 100 %", L"Ctrl+0"},
};

// The zoom steps Ctrl+= and Ctrl+− go through.
constexpr double kZoomSteps[] = {0.1, 0.25, 0.5, 0.75, 0.85, 1, 1.15, 1.25, 1.5, 1.75, 2, 2.5, 3, 4};

std::wstring Lower(std::wstring_view s) {
  std::wstring out(s);
  if (!out.empty()) CharLowerBuffW(out.data(), static_cast<DWORD>(out.size()));
  return out;
}

bool WordChar(wchar_t c) { return IsCharAlphaNumericW(c) || c == L'_'; }

bool Contains(const std::wstring& hay, const std::wstring& needle, bool whole) {
  size_t at = 0;
  while ((at = hay.find(needle, at)) != std::wstring::npos) {
    if (!whole) return true;
    const bool before = at == 0 || !WordChar(hay[at - 1]);
    const bool after = at + needle.size() >= hay.size() || !WordChar(hay[at + needle.size()]);
    if (before && after) return true;
    ++at;
  }
  return false;
}

void HtmlEscape(std::string& out, std::string_view s) {
  for (char ch : s) {
    switch (ch) {
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '&': out += "&amp;"; break;
      case '"': out += "&quot;"; break;
      case '\n': out += "<br>"; break;
      default: out += ch;
    }
  }
}

std::string CssColor(uint32_t argb) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "#%06X", static_cast<unsigned>(argb & 0xFFFFFF));
  return buf;
}

// Moves the relative references of a formula written for one cell to another (a shared formula's
// followers): A1 references not marked absolute ($), outside strings and quoted sheet names.
std::string ShiftFormula(std::string_view f, int64_t dr, int64_t dc) {
  if (dr == 0 && dc == 0) return std::string(f);
  std::string out;
  size_t i = 0;
  auto isIdent = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '\\'; };
  while (i < f.size()) {
    const char c = f[i];
    if (c == '"' || c == '\'') {  // a string or a quoted sheet name
      const size_t start = i++;
      while (i < f.size()) {
        if (f[i] == c && i + 1 < f.size() && f[i + 1] == c) i += 2;
        else if (f[i] == c) {
          ++i;
          break;
        } else ++i;
      }
      out.append(f.substr(start, i - start));
      continue;
    }
    if (c == '[') {  // structured references and external workbooks: as they are
      const size_t end = f.find(']', i);
      const size_t stop = end == std::string_view::npos ? f.size() : end + 1;
      out.append(f.substr(i, stop - i));
      i = stop;
      continue;
    }
    // A reference: [$]letters[$]digits, not part of a longer name or a function call.
    if ((c == '$' || std::isalpha(static_cast<unsigned char>(c))) && (i == 0 || !isIdent(f[i - 1]))) {
      size_t j = i;
      const bool colAbs = f[j] == '$';
      if (colAbs) ++j;
      const size_t ls = j;
      while (j < f.size() && std::isalpha(static_cast<unsigned char>(f[j])) && j - ls < 3) ++j;
      const size_t le = j;
      const bool rowAbs = j < f.size() && f[j] == '$';
      if (rowAbs) ++j;
      const size_t ds = j;
      while (j < f.size() && std::isdigit(static_cast<unsigned char>(f[j]))) ++j;
      const size_t de = j;
      const bool isRef = le > ls && de > ds && (j >= f.size() || (!isIdent(f[j]) && f[j] != '(' && f[j] != '!'));
      if (isRef) {
        std::string letters(f.substr(ls, le - ls));
        for (char& ch : letters) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        int64_t col = 0;
        for (char ch : letters) col = col * 26 + (ch - 'A' + 1);
        int64_t row = std::atoll(std::string(f.substr(ds, de - ds)).c_str());
        if (col >= 1 && col <= kMaxCols && row >= 1 && row <= kMaxRows) {
          if (!colAbs) col += dc;
          if (!rowAbs) row += dr;
          if (col < 1 || col > kMaxCols || row < 1 || row > kMaxRows) {
            out += "#REF!";
          } else {
            if (colAbs) out += '$';
            out += ColumnName(static_cast<uint32_t>(col - 1));
            if (rowAbs) out += '$';
            out += std::to_string(row);
          }
          i = j;
          continue;
        }
      }
      // A name or function: copied as it is.
      size_t k = i + (colAbs ? 1 : 0);
      while (k < f.size() && isIdent(f[k])) ++k;
      if (k == i) k = i + 1;
      out.append(f.substr(i, k - i));
      i = k;
      continue;
    }
    out += c;
    ++i;
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Commands

int WorkbookView::ShortcutFor(UINT vk, UINT mods) const {
  const bool ctrl = (mods & kModCtrl) != 0, shift = (mods & kModShift) != 0, alt = (mods & kModAlt) != 0;
  if (ctrl && !alt && !shift) {
    switch (vk) {
      case VK_NEXT: return CmdNextSheet;
      case VK_PRIOR: return CmdPrevSheet;
      case VK_OEM_PLUS: return CmdZoomIn;
      case '0':
      case VK_NUMPAD0: return CmdZoomActual;
      case VK_F2: return CmdPrintPreview;
      // Undo and redo the workbook's edits (while a cell is typed in, the keys are the entry's).
      case 'Z': return editor_.Active() ? 0 : CmdUndo;
      case 'Y': return editor_.Active() ? 0 : CmdRedo;
      case 'G': return editor_.Active() ? 0 : CmdGoTo;
    }
  }
  // Excel's keys for rows, columns, cells and sheets (not while a cell is typed in).
  if (!editor_.Active() && !preview_ && !alt) {
    if (ctrl && ((shift && vk == VK_OEM_PLUS) || vk == VK_ADD)) return CmdInsertCells;
    if (ctrl && !shift && (vk == VK_OEM_MINUS || vk == VK_SUBTRACT)) return CmdDeleteCells;
    if (!ctrl && shift && vk == VK_F11) return CmdInsertSheet;
    if (!ctrl && !shift && vk == VK_F5) return CmdGoTo;
  }
  if (preview_ && vk == VK_ESCAPE) return CmdPrintPreview;
  return 0;
}

CommandState WorkbookView::QueryCommand(int cmd) {
  CommandState s;
  if (preview_ && (cmd == CmdNextPage || cmd == CmdPrevPage || cmd == CmdFirstPage || cmd == CmdLastPage ||
                   cmd == CmdZoomAuto || cmd == CmdZoomFitPage || cmd == CmdZoomFitWidth || cmd == CmdZoomActual ||
                   cmd == CmdViewSingle || cmd == CmdViewContinuous || cmd == CmdViewFacing ||
                   (cmd >= CmdZoomPreset && cmd < CmdZoomPreset + 50)))
    return PagedView::QueryCommand(cmd);
  if (GridCommandState(cmd, s)) return s;
  const bool ready = Ready();
  size_t shown = 0;
  if (ready)
    for (size_t i = 0; i < book_->SheetCount(); ++i) shown += SheetShown(i);
  switch (cmd) {
    case CmdCopy:
    case CmdSelectAll:
    case CmdPrint:
    case CmdZoomIn:
    case CmdZoomOut:
    case CmdZoomActual: s.enabled = ready; break;
    case CmdPrintPreview:
      s.enabled = ready;
      s.checked = preview_;
      break;
    case CmdNextSheet:
    case CmdPrevSheet: s.enabled = ready && shown > 1; break;
    case CmdSave: s.enabled = false; break;
    case CmdUndo: s.enabled = ready && edit_ && edit_->CanUndo(); break;
    case CmdRedo: s.enabled = ready && edit_ && edit_->CanRedo(); break;
    case CmdRotateLeft:
    case CmdRotateRight:
    case CmdToggleCover:
    case CmdHighlight:
    case CmdSelectAllOnPage:
    case CmdViewSingle:
    case CmdViewContinuous:
    case CmdViewFacing:
    case CmdGoToPage: s.visible = false; break;
    default:
      if (cmd >= CmdSheetPick && cmd <= CmdSheetPickLast && ready) {
        const size_t i = static_cast<size_t>(cmd - CmdSheetPick);
        s.enabled = SheetShown(i);
        s.checked = i == sheet_;
        if (i < book_->SheetCount()) s.label = Utf8ToWide(book_->GetSheet(i).Name());
      } else if (cmd >= CmdZoomPreset && cmd < CmdZoomPreset + 50) {
        const auto& presets = fw::ZoomPresets();
        const size_t i = static_cast<size_t>(cmd - CmdZoomPreset);
        s.enabled = ready;
        s.checked = i < presets.size() && std::abs(zoom_ - presets[i]) < 0.005;
      }
      break;
  }
  for (const CommandName& n : kCommandNames)
    if (n.cmd == cmd) {
      s.label = n.label;
      s.shortcut = n.keys;
    }
  return s;
}

std::vector<int> WorkbookView::Commands() {
  std::vector<int> ids;
  for (const CommandName& n : kCommandNames) ids.push_back(n.cmd);
  return ids;
}

bool WorkbookView::Execute(int cmd) {
  if (!Ready()) return false;
  if (preview_ && cmd != CmdPrintPreview && cmd != CmdCopy && cmd != CmdNextSheet && cmd != CmdPrevSheet &&
      !(cmd >= CmdSheetPick && cmd <= CmdSheetPickLast))
    return PagedView::Execute(cmd);
  if (cmd >= CmdSheetPick && cmd <= CmdSheetPickLast) {
    const size_t i = static_cast<size_t>(cmd - CmdSheetPick);
    if (SheetShown(i)) ShowSheet(i);
    return true;
  }
  if (cmd >= CmdZoomPreset && cmd < CmdZoomPreset + 50) {
    const auto& presets = fw::ZoomPresets();
    const size_t i = static_cast<size_t>(cmd - CmdZoomPreset);
    if (i < presets.size()) SetZoom100(presets[i]);
    return true;
  }
  if (GridCommand(cmd)) return true;
  switch (cmd) {
    case CmdUndo:
    case CmdRedo:
      if (editor_.Active()) {
        editor_.End();
        host_.ViewStateChanged();
      } else if (edit_) {
        EditRefused(cmd == CmdUndo ? edit_->Undo() : edit_->Redo());
      }
      return true;
    case CmdCopy: Copy(); return true;
    case CmdSelectAll: Select({0, 0}, {kMaxRows - 1, kMaxCols - 1}, false); return true;
    case CmdPrintPreview: TogglePreview(); return true;
    case CmdNextSheet:
    case CmdPrevSheet: {
      const bool wasPreview = preview_;
      ShowSheet(NextShownSheet(sheet_, cmd == CmdNextSheet ? 1 : -1));
      if (wasPreview) TogglePreview();
      return true;
    }
    case CmdZoomIn:
    case CmdZoomOut: {
      double z = zoom_;
      if (cmd == CmdZoomIn) {
        for (double step : kZoomSteps)
          if (step > zoom_ + 0.001) {
            z = step;
            break;
          }
      } else {
        for (double step : kZoomSteps)
          if (step < zoom_ - 0.001) z = step;
      }
      SetZoom100(z);
      return true;
    }
    case CmdZoomActual: SetZoom100(1); return true;
    case CmdRotateLeft:
    case CmdRotateRight:
    case CmdToggleCover: return true;
  }
  return false;
}

void WorkbookView::SetZoom100(double zoom) {
  zoom_ = std::clamp(zoom, 0.1, 4.0);
  if (std::abs(zoom_ - std::round(zoom_ * 100) / 100) < 0.004) zoom_ = std::round(zoom_ * 100) / 100;
  text_->Clear();
  LayoutParts();
  ClampScroll();
  host_.ViewStateChanged();
  host_.ViewInvalidate();
}

void WorkbookView::BuildToolbar(ToolbarModel& m, const ToolbarContext& ctx) {
  auto item = [](int cmd, wchar_t glyph, const std::wstring& tooltip = L"") {
    ToolItem t;
    t.cmd = cmd;
    t.glyph = glyph;
    t.tooltip = tooltip;
    return t;
  };
  ToolItem print = item(CmdPrint, icon::Print);
  ToolItem preview = item(CmdPrintPreview, icon::Page, L"Print preview (Ctrl+F2)");
  preview.toggle = true;
  preview.checked = preview_;
  m.left = {print, preview};
  m.pager = preview_;
  if (preview_) {
    m.page = currentPage_;
    m.pageCount = PageCount();
    m.zoomText = fw::FormatPercent(Zoom());
  }
  ToolItem find = item(CmdFind, icon::Search);
  find.checked = ctx.searchOpen;
  find.toggle = true;
  find.priority = 5;
  find.sepBefore = true;
  ToolItem full = item(CmdFullScreen, ctx.fullscreen ? icon::BackToWindow : icon::FullScreen);
  full.priority = 2;
  m.right = {find, full};
}

void WorkbookView::FillMenu(int which, HMENU menu) {
  if (preview_ || which != CmdZoomMenu) {
    PagedView::FillMenu(which, menu);
    return;
  }
  const auto& presets = fw::ZoomPresets();
  for (int i = 0; i < static_cast<int>(presets.size()); ++i)
    menu::AppendItem(menu, CmdZoomPreset + i, fw::FormatPercent(presets[i]), true,
                     QueryCommand(CmdZoomPreset + i).checked, true);
}

// ---------------------------------------------------------------------------
// Status bar: the selection's sum, average and count

void WorkbookView::UpdateStatus() {
  const Range sel = Selected();
  if (totals_.range == sel && totals_.sheet == sheet_) return;
  totals_ = Totals{};
  totals_.range = sel;
  totals_.sheet = sheet_;
  bool styled = false;
  Current().ForEachCell(sel, [&](const CellInfo& c) {
    if (c.value.type == ValueType::Empty) return true;
    ++totals_.count;
    if (c.value.type == ValueType::Number) {
      totals_.sum += c.value.number;
      ++totals_.numbers;
      if (!styled) {
        totals_.style = c.style;
        styled = true;
      }
    }
    return true;
  });
}

void WorkbookView::BuildStatusBar(StatusBarModel& m) {
  if (!Ready()) return;
  UpdateStatus();
  if (Stale(active_.row, active_.col))
    m.segments.push_back(L"Value possibly out of date: it uses a function Featherweight can't calculate yet");
  if (totals_.count >= 2) {
    // Excel's order: average, count, sum; in the format of the first number.
    auto show = [&](double v) {
      const Xf& xf = book_->GetStyles().CellXf(totals_.style);
      const NumberFormat& f = text_->FormatOf(xf.numFmt);
      Formatted out;
      FormatContext ctx;
      ctx.locale = &text_->Locale();
      ctx.date1904 = book_->Date1904();
      f.Format(v, ctx, out);
      if (out.overflow || out.text.empty() || f.IsGeneral()) return FormatGeneral(v, 15, text_->Locale());
      return out.text;
    };
    if (totals_.numbers > 0) m.segments.push_back(L"Average: " + show(totals_.sum / static_cast<double>(totals_.numbers)));
    m.segments.push_back(L"Count: " + std::to_wstring(totals_.count));
    if (totals_.numbers > 0) m.segments.push_back(L"Sum: " + show(totals_.sum));
  }
  m.zoom = true;
  m.zoomValue = zoom_;
  m.onZoom = [this](double z) { SetZoom100(z); };
}

// ---------------------------------------------------------------------------
// Copy: plain text and HTML (a table), as other programs paste Excel's

void WorkbookView::Copy() {
  if (!Ready()) return;
  Range sel = Selected();
  // Whole rows and columns: as far as the cells in use.
  if (auto used = Current().UsedRange()) {
    sel.lastRow = std::min(sel.lastRow, used->lastRow);
    sel.lastCol = std::min(sel.lastCol, used->lastCol);
  }
  if (sel.lastRow < sel.firstRow || sel.lastCol < sel.firstCol) return;
  const uint64_t cells = static_cast<uint64_t>(sel.lastRow - sel.firstRow + 1) * (sel.lastCol - sel.firstCol + 1);
  if (cells > 2000000) {
    host_.ViewMessage(BannerKind::Info, L"That's too many cells to copy at once. Select fewer and copy again.");
    return;
  }
  const Sheet& s = Current();
  const Styles& st = book_->GetStyles();
  std::wstring text;
  std::string html = "<table border=0 cellpadding=0 cellspacing=0 style='border-collapse:collapse'>";
  const bool withHtml = cells <= 100000;
  std::vector<CellInfo> row;
  for (uint32_t r = sel.firstRow; r <= sel.lastRow; ++r) {
    row.clear();
    s.ForEachCell(Range{r, sel.firstCol, r, sel.lastCol}, [&](const CellInfo& c) {
      row.push_back(c);
      return true;
    });
    if (withHtml) html += "<tr>";
    size_t at = 0;
    for (uint32_t c = sel.firstCol; c <= sel.lastCol; ++c) {
      const CellInfo* cell = at < row.size() && row[at].col == c ? &row[at++] : nullptr;
      std::wstring t = cell ? text_->Text(s, *cell) : std::wstring();
      if (c > sel.firstCol) text += L'\t';
      // Excel quotes a value with a line break, tab or quote in it.
      if (t.find_first_of(L"\t\n\"") != std::wstring::npos) {
        std::wstring q = L"\"";
        for (wchar_t ch : t) q += ch == L'"' ? std::wstring(L"\"\"") : std::wstring(1, ch);
        text += q + L"\"";
      } else {
        text += t;
      }
      if (!withHtml) continue;
      const Xf& xf = st.CellXf(cell ? cell->style : s.StyleAt(r, c));
      const Font& f = st.FontOf(xf);
      std::string css;
      if (f.bold) css += "font-weight:bold;";
      if (f.italic) css += "font-style:italic;";
      if (f.underline != Underline::None) css += "text-decoration:underline;";
      if (!f.name.empty()) css += "font-family:\"" + f.name + "\";";
      if (f.size > 0) css += "font-size:" + std::to_string(static_cast<int>(f.size)) + "pt;";
      if (f.color.IsSet()) css += "color:" + CssColor(st.Resolve(f.color, 0xFF000000)) + ";";
      const Fill& fill = st.FillOf(xf);
      if (fill.pattern == PatternType::Solid) css += "background:" + CssColor(st.Resolve(fill.fg, 0xFFFFFFFF)) + ";";
      switch (xf.alignment.horizontal) {
        case HAlign::Center: css += "text-align:center;"; break;
        case HAlign::Right: css += "text-align:right;"; break;
        case HAlign::General:
          if (cell && cell->value.type == ValueType::Number) css += "text-align:right;";
          break;
        default: break;
      }
      html += css.empty() ? "<td>" : "<td style='" + css + "'>";
      HtmlEscape(html, WideToUtf8(t));
      html += "</td>";
    }
    text += L"\r\n";
    if (withHtml) html += "</tr>";
  }
  html += "</table>";
  std::string cfHtml;
  if (withHtml) {
    const std::string head = "<html><body>\r\n<!--StartFragment-->";
    const std::string tail = "<!--EndFragment-->\r\n</body></html>";
    const char* const kHeader = "Version:0.9\r\nStartHTML:%010zu\r\nEndHTML:%010zu\r\nStartFragment:%010zu\r\nEndFragment:%010zu\r\n";
    char probe[160];
    const size_t headerSize = static_cast<size_t>(std::snprintf(probe, sizeof probe, kHeader, size_t{0}, size_t{0}, size_t{0}, size_t{0}));
    const size_t startFrag = headerSize + head.size(), endFrag = startFrag + html.size(), endHtml = endFrag + tail.size();
    char header[160];
    std::snprintf(header, sizeof header, kHeader, headerSize, endHtml, startFrag, endFrag);
    cfHtml = header + head + html + tail;
  }
  util::SetClipboardText(text, {}, cfHtml);
}

std::wstring WorkbookView::SelectedText() {
  if (!Ready()) return {};
  CellInfo cell;
  if (!Current().GetCell(active_.row, active_.col, cell)) return {};
  return text_->Text(Current(), cell);
}

// ---------------------------------------------------------------------------
// Find: the shown text of every cell in the sheet, row by row, in idle time

void WorkbookView::StartSearch(const std::wstring& query, bool matchCase, bool wholeWords) {
  query_ = query;
  matchCase_ = matchCase;
  wholeWords_ = wholeWords;
  matches_.clear();
  matchIndex_ = -1;
  searchDone_ = true;
  if (!Ready() || query.empty()) {
    host_.ViewInvalidate();
    return;
  }
  searchArea_ = Current().UsedRange();
  if (!searchArea_) return;
  searchRow_ = searchArea_->firstRow;
  searchDone_ = false;
  SearchIdle(anim::Now() + 30);  // small sheets are done at once
  host_.ViewInvalidate();
}

void WorkbookView::SearchIdle(double deadlineMs) {
  if (searchDone_ || !searchArea_) return;
  const Sheet& s = Current();
  const std::wstring needle = matchCase_ ? query_ : Lower(query_);
  while (searchRow_ <= searchArea_->lastRow) {
    const uint32_t last = std::min(searchArea_->lastRow, searchRow_ + 255);
    s.ForEachCell(Range{searchRow_, searchArea_->firstCol, last, searchArea_->lastCol}, [&](const CellInfo& c) {
      if (c.value.type == ValueType::Empty) return true;
      std::wstring t = text_->Text(s, c);
      if (!matchCase_) t = Lower(t);
      if (Contains(t, needle, wholeWords_) && matches_.size() < 1000000)
        matches_.push_back({static_cast<uint32_t>(sheet_), c.row, c.col});
      return true;
    });
    searchRow_ = last + 1;
    if (anim::Now() > deadlineMs) break;
  }
  if (searchRow_ > searchArea_->lastRow) searchDone_ = true;
  if (matchIndex_ < 0 && !matches_.empty()) {
    // The first match from the active cell on.
    matchIndex_ = 0;
    for (size_t i = 0; i < matches_.size(); ++i) {
      const Match& m = matches_[i];
      if (m.row > active_.row || (m.row == active_.row && m.col >= active_.col)) {
        matchIndex_ = static_cast<int>(i);
        break;
      }
    }
    ShowMatch();
  }
  host_.ViewStateChanged();
}

void WorkbookView::ClearSearch() {
  query_.clear();
  matches_.clear();
  matchIndex_ = -1;
  searchDone_ = true;
  host_.ViewInvalidate();
}

void WorkbookView::SearchStep(bool forward) {
  if (matches_.empty()) return;
  // The next match after (or before) the active cell, around the end.
  auto after = [&](const Match& m) { return m.row > active_.row || (m.row == active_.row && m.col > active_.col); };
  auto before = [&](const Match& m) { return m.row < active_.row || (m.row == active_.row && m.col < active_.col); };
  int next = -1;
  if (forward) {
    for (size_t i = 0; i < matches_.size(); ++i)
      if (after(matches_[i])) {
        next = static_cast<int>(i);
        break;
      }
    if (next < 0) next = 0;
  } else {
    for (size_t i = matches_.size(); i-- > 0;)
      if (before(matches_[i])) {
        next = static_cast<int>(i);
        break;
      }
    if (next < 0) next = static_cast<int>(matches_.size()) - 1;
  }
  matchIndex_ = next;
  ShowMatch();
}

void WorkbookView::ShowMatch() {
  if (matchIndex_ < 0 || matchIndex_ >= static_cast<int>(matches_.size())) return;
  const Match& m = matches_[matchIndex_];
  if (preview_) TogglePreview();
  Select({m.row, m.col}, {m.row, m.col});
}

// ---------------------------------------------------------------------------
// Formula bar

std::wstring WorkbookView::ActiveName() const { return Ready() ? Utf8ToWide(CellName(active_.row, active_.col)) : L""; }

std::wstring WorkbookView::FormulaBarText() const {
  if (!Ready()) return {};
  const Sheet& s = Current();
  CellInfo cell;
  if (!s.GetCell(active_.row, active_.col, cell)) return {};
  if (cell.hasFormula) {
    FormulaInfo f;
    if (s.GetFormula(active_.row, active_.col, f)) {
      std::string text = ShiftFormula(f.text, static_cast<int64_t>(active_.row) - f.origin.row,
                                      static_cast<int64_t>(active_.col) - f.origin.col);
      for (const char* prefix : {"_xlfn._xlws.", "_xlfn.", "_xlws."}) {
        for (size_t at; (at = text.find(prefix)) != std::string::npos;) text.erase(at, std::strlen(prefix));
      }
      // In the region's form (en-NL: =SUM(1;2,5)), as Excel's formula bar shows it.
      const std::wstring w = L"=" + Utf8ToWide(calc::ToLocal(text, Region()));
      return f.kind == FormulaKind::Array ? L"{" + w + L"}" : w;
    }
  }
  const Xf& xf = book_->GetStyles().CellXf(cell.style);
  switch (cell.value.type) {
    case ValueType::Empty: return {};
    case ValueType::String: return (xf.quotePrefix ? L"'" : L"") + Utf8ToWide(cell.value.text);
    case ValueType::Bool: return cell.value.boolean ? L"TRUE" : L"FALSE";
    case ValueType::Error: return Utf8ToWide(ErrorText(cell.value.error));
    case ValueType::Number: break;
  }
  // Numbers as Excel's formula bar shows them: dates and times in the region's short form,
  // percentages as percentages, anything else in full (15 digits).
  const double v = cell.value.number;
  const NumberFormat& f = text_->FormatOf(xf.numFmt);
  FormatContext ctx;
  ctx.locale = &text_->Locale();
  ctx.date1904 = book_->Date1904();
  Formatted out;
  if (f.IsDateTime()) {
    const bool date = v >= 1, time = std::fabs(v - std::floor(v)) > 1e-9;
    std::wstring r;
    if (date || !time) {
      NumberFormat::Builtin(14, text_->Locale()).Format(v, ctx, out);
      r = out.text;
    }
    if (time) {
      NumberFormat::Builtin(21, text_->Locale()).Format(v, ctx, out);
      r += (r.empty() ? L"" : L" ") + out.text;
    }
    return r;
  }
  const std::string_view code = book_->GetStyles().FormatCode(xf.numFmt);
  const bool percent = code.find('%') != std::string_view::npos || xf.numFmt == 9 || xf.numFmt == 10;
  wchar_t buf[64];
  std::swprintf(buf, 64, L"%.15g", percent ? v * 100 : v);
  std::wstring r = buf;
  if (const size_t e = r.find(L'e'); e != std::wstring::npos) {
    r[e] = L'E';
    // 1E+20 rather than 1E+020.
    size_t d = e + 2;
    while (d + 1 < r.size() && r[d] == L'0') r.erase(d, 1);
  }
  if (const size_t dot = r.find(L'.'); dot != std::wstring::npos) r.replace(dot, 1, text_->Locale().decimal);
  return percent ? r + L"%" : r;
}

// ---------------------------------------------------------------------------
// Links

const Hyperlink* WorkbookView::LinkAt(uint32_t r, uint32_t c) const {
  for (const Hyperlink& h : Current().Hyperlinks())
    if (h.ref.Contains(r, c)) return &h;
  return nullptr;
}

void WorkbookView::FollowLink(const Hyperlink& link) {
  if (!link.location.empty() && link.target.empty()) {
    // A place in the workbook: Sheet2!B3, 'My sheet'!A1:C4, or a defined name.
    std::string where = link.location;
    std::string sheetName;
    if (const size_t bang = where.rfind('!'); bang != std::string::npos) {
      sheetName = where.substr(0, bang);
      where = where.substr(bang + 1);
      if (sheetName.size() >= 2 && sheetName.front() == '\'' && sheetName.back() == '\'') {
        sheetName = sheetName.substr(1, sheetName.size() - 2);
        for (size_t at; (at = sheetName.find("''")) != std::string::npos;) sheetName.erase(at, 1);
      }
    } else if (const DefinedName* n = book_->FindName(where, static_cast<int>(sheet_))) {
      std::string f = n->formula;
      const size_t at = f.rfind('!');
      if (at != std::string::npos) {
        sheetName = f.substr(0, at);
        if (sheetName.size() >= 2 && sheetName.front() == '\'') sheetName = sheetName.substr(1, sheetName.size() - 2);
        where = f.substr(at + 1);
      }
    }
    Range r;
    const int target = sheetName.empty() ? static_cast<int>(sheet_) : book_->FindSheet(sheetName);
    if (target < 0 || !SheetShown(static_cast<size_t>(target)) || !ParseRange(where, r)) {
      host_.ViewMessage(BannerKind::Warning, L"The place this link points to isn't in the workbook.");
      return;
    }
    if (static_cast<size_t>(target) != sheet_) ShowSheet(static_cast<size_t>(target));
    Select({r.firstRow, r.firstCol}, {r.lastRow, r.lastCol});
    return;
  }
  std::wstring url = Utf8ToWide(link.target);
  if (!link.location.empty()) url += L"#" + Utf8ToWide(link.location);
  if (!util::OpenExternalLink(url))
    host_.ViewMessage(BannerKind::Warning, L"This link was blocked because it doesn’t point to a website.");
}

// ---------------------------------------------------------------------------
// File and state

bool WorkbookView::Save(const std::wstring& path, std::wstring& error) {
  // Nothing is changed in a workbook yet: saving elsewhere is a copy of the file as it was opened.
  if (!Ready()) return false;
  if (EqualsNoCase(path, path_)) return true;
  const auto bytes = book_->GetPackage()->GetSnapshot().Bytes();
  if (!util::WriteFileAtomic(path, std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()))) {
    error = L"The workbook couldn't be saved there.";
    return false;
  }
  return true;
}

SaveFormat WorkbookView::SaveAsFormat() const {
  if (book_ && book_->Format() == FileFormat::Xlsm) return {L"Excel macro-enabled workbook", L"*.xlsm", L"xlsm"};
  return {L"Excel workbook", L"*.xlsx", L"xlsx"};
}

FileState WorkbookView::State() const {
  FileState s;
  s.page = static_cast<int>(sheet_);
  s.zoomMode = fw::ZoomMode::Custom;
  s.zoom = zoom_;
  return s;
}

void WorkbookView::ApplyState(const FileState& s) {
  // Only a remembered state: the file's own active sheet otherwise.
  if (!Ready() || s.lastUsed == 0) return;
  if (s.page >= 0 && static_cast<size_t>(s.page) != sheet_ && SheetShown(static_cast<size_t>(s.page)))
    ShowSheet(static_cast<size_t>(s.page));
}

void WorkbookView::DescribeUia(uia::Tree& t, int parent) {
  if (!Ready()) return;
  const int grid = t.Add(parent, 1, UIA_DataGridControlTypeId, Utf8ToWide(Current().Name()), cells_);
  t[grid].hasValue = true;
  t[grid].value = ActiveName() + L" " + SelectedText();
  t[grid].focusable = true;
  const int tabs = t.Add(parent, 2, UIA_TabControlTypeId, L"Sheets", tabs_);
  t[tabs].selectionContainer = true;
  int i = 0;
  for (const TabRect& r : TabRects()) {
    const size_t sheet = r.sheet;
    const int item = t.Add(tabs, static_cast<int64_t>(100 + sheet), UIA_TabItemControlTypeId,
                           Utf8ToWide(book_->GetSheet(sheet).Name()), r.rc);
    t[item].selectable = true;
    t[item].selected = sheet == sheet_;
    t[item].select = [this, sheet] { ShowSheet(sheet); };
    t[item].positionInSet = ++i;
  }
}

// ---------------------------------------------------------------------------
// Idle work: the other sheets, and find

bool WorkbookView::HasIdleWork() const {
  if (!Ready()) return false;
  if (!searchDone_) return true;
  for (size_t i = loadNext_; i < book_->SheetCount(); ++i)
    if (!book_->GetSheet(i).Loaded()) return true;
  return recalc_ && (!calc_ || calc_->Opening());  // then nothing: no idle CPU
}

bool WorkbookView::IdleWork(ID2D1DeviceContext*, double deadlineMs) {
  if (!Ready()) return false;
  if (!searchDone_) {
    SearchIdle(deadlineMs);
    return true;
  }
  // One sheet a turn: the others are read once the shown one is on screen.
  while (loadNext_ < book_->SheetCount() && book_->GetSheet(loadNext_).Loaded()) ++loadNext_;
  if (loadNext_ < book_->SheetCount()) {
    book_->LoadSheet(loadNext_++);
    return false;
  }
  // Then what Excel recalculates when it opens the file, a slice at a time (plan §7.3 "Cached
  // values"); the cached values show until then.
  if (!recalc_ || (calc_ && !calc_->Opening())) return false;
  calc::WorkbookCalc& calc = Calc();
  const uint64_t before = calc.Changes();
  const bool done = calc.OpenStep(std::max(1.0, deadlineMs - anim::Now()));
  const bool changed = calc.Changes() != before;
  if (!changed && !done) return false;
  if (changed) {
    text_->Clear();  // keyed by cell: it wouldn't see the new values
    totals_ = Totals{};
    host_.ViewStateChanged();
  }
  // Conditional formats see the new values (and, once done, sheets read after they were worked out).
  for (auto& r : rules_)
    if (r) r->Changed();
  return true;
}

void WorkbookView::OnDeactivated() {
  if (text_) text_->Clear();
}

}  // namespace fw::sheet::view
