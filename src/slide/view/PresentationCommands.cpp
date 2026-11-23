// The presentation tab's keys, commands, toolbar and menus; find, copy, links and the slide show.

#include <windows.h>

#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Commands.h"
#include "Util.h"
#include "core/Layout.h"
#include "core/Strings.h"
#include "draw/D2DPainter.h"
#include "slide/show/SlideRenderer.h"
#include "slide/show/SlideShow.h"
#include "slide/view/PresentationText.h"
#include "slide/view/TextFind.h"
#include "slide/view/ObjectTool.h"
#include "slide/edit/Edit.h"
#include "slide/view/PresentationView.h"
#include "ui/Icons.h"
#include "ui/Menu.h"
#include "ui/StatusBar.h"
#include "ui/Toolbar.h"

using Microsoft::WRL::ComPtr;
using menu::AppendItem;
using menu::AppendSeparator;

namespace fw::slide::view {

namespace {

struct CommandName {
  int cmd;
  const wchar_t* label;
  const wchar_t* keys;
};

constexpr CommandName kNames[] = {
    {CmdPresentation, L"Present from the beginning", L"F5"},
    {CmdSlideShowFromStart, L"Present from the beginning", L"F5"},
    {CmdSlideShowFromCurrent, L"Present from the current slide", L"Shift+F5"},
    {CmdNextPage, L"Next slide", L"Page Down"},
    {CmdPrevPage, L"Previous slide", L"Page Up"},
    {CmdFirstPage, L"First slide", L"Home"},
    {CmdLastPage, L"Last slide", L"End"},
    {CmdZoomIn, L"Zoom in", L"Ctrl+="},
    {CmdZoomOut, L"Zoom out", L"Ctrl+−"},
    {CmdZoomFitPage, L"Fit slide to window", L"Ctrl+0"},
    {CmdZoomActual, L"Zoom to 100 %", L"Ctrl+1"},
    {CmdNotesPane, L"Notes", L""},
    {CmdCopy, L"Copy slide", L"Ctrl+C"},
    {CmdCopySlideText, L"Copy slide text", L""},
    {CmdCopySlidePicture, L"Copy slide as picture", L""},
    {CmdCopyNotes, L"Copy speaker notes", L""},
    {CmdOpenSlideLink, L"Open link", L""},
    {CmdPrintLayoutMenu, L"Print layout", L""},
    {CmdPrintSlides, L"Print and export: full page slides", L""},
    {CmdPrintHandouts1, L"Print and export: handouts, 1 slide per page", L""},
    {CmdPrintHandouts2, L"Print and export: handouts, 2 slides per page", L""},
    {CmdPrintHandouts3, L"Print and export: handouts, 3 slides per page", L""},
    {CmdPrintHandouts4, L"Print and export: handouts, 4 slides per page", L""},
    {CmdPrintHandouts6, L"Print and export: handouts, 6 slides per page", L""},
    {CmdPrintHandouts9, L"Print and export: handouts, 9 slides per page", L""},
    {CmdPrintNotes, L"Print and export: notes pages", L""},
    {CmdPrintHidden, L"Print hidden slides", L""},
    {CmdPrintFrame, L"Frame slides on handouts and notes pages", L""},
};

// The Print layout choices, in the order the dropdown lists them.
struct LayoutChoice {
  int cmd;
  PrintWhat what;
  int perPage;
  const wchar_t* label;
};
constexpr LayoutChoice kLayouts[] = {
    {CmdPrintSlides, PrintWhat::Slides, 0, L"Full page slides"},
    {CmdPrintNotes, PrintWhat::Notes, 0, L"Notes pages"},
    {CmdPrintHandouts1, PrintWhat::Handouts, 1, L"Handouts: 1 slide"},
    {CmdPrintHandouts2, PrintWhat::Handouts, 2, L"Handouts: 2 slides"},
    {CmdPrintHandouts3, PrintWhat::Handouts, 3, L"Handouts: 3 slides"},
    {CmdPrintHandouts4, PrintWhat::Handouts, 4, L"Handouts: 4 slides"},
    {CmdPrintHandouts6, PrintWhat::Handouts, 6, L"Handouts: 6 slides"},
    {CmdPrintHandouts9, PrintWhat::Handouts, 9, L"Handouts: 9 slides"},
};

int LayoutIndex(const PrintSettings& s) {
  for (size_t i = 0; i < std::size(kLayouts); ++i)
    if (kLayouts[i].what == s.what && (s.what != PrintWhat::Handouts || kLayouts[i].perPage == s.perPage))
      return static_cast<int>(i);
  return 0;
}

void PutClipboard(UINT format, const void* data, size_t size) {
  if (!size) return;
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, size);
  if (!h) return;
  if (void* p = GlobalLock(h)) {
    std::memcpy(p, data, size);
    GlobalUnlock(h);
    if (SetClipboardData(format, h)) return;
  }
  GlobalFree(h);
}

}  // namespace

int PresentationView::ShortcutFor(UINT vk, UINT mods) const {
  if (int cmd = text_->Shortcut(vk, mods)) return cmd;  // text being edited: its keys first
  // p2-objects: the objects' keys, and undo and redo wherever the keys are.
  const bool undoKey = (mods & kModCtrl) && (vk == 'Z' || vk == 'Y');
  if (const int c = Ready() && (!keysInThumbs_ || undoKey) ? Objects().Shortcut(vk, mods) : 0) return c;
  if (const int slides = SlideShortcut(vk, mods)) return slides;  // P2: Ctrl+M, Ctrl+D, the thumbnails' keys
  const bool ctrl = (mods & kModCtrl) != 0, shift = (mods & kModShift) != 0, alt = (mods & kModAlt) != 0;
  if (vk == VK_F5 && !ctrl && !alt) return shift ? CmdSlideShowFromCurrent : CmdSlideShowFromStart;
  if (ctrl && !alt) {
    switch (vk) {
      case VK_OEM_PLUS:
      case VK_ADD: return CmdZoomIn;
      case VK_OEM_MINUS:
      case VK_SUBTRACT: return CmdZoomOut;
      case '0':
      case VK_NUMPAD0: return CmdZoomFitPage;
      case '1': return CmdZoomActual;
    }
    return 0;
  }
  if (alt || shift) return 0;
  switch (vk) {
    case VK_NEXT: return CmdNextPage;
    case VK_PRIOR: return CmdPrevPage;
    case VK_HOME: return CmdFirstPage;
    case VK_END: return CmdLastPage;
  }
  return 0;
}

CommandState PresentationView::QueryCommand(int cmd) {
  if (text_->Owns(cmd)) return text_->Query(cmd);
  CommandState s;
  if (Ready() && ObjectsTake(cmd) && Objects().Query(cmd, s)) return s;  // p2-objects
  if (SlideCommandState(cmd, s)) return s;  // P2: the slides' commands
  const bool ready = Ready() && SlideCount() > 0;
  switch (cmd) {
    case CmdSave: s.enabled = Ready() && deck_->Editor() != nullptr; break;  // P2: PresentationSave.cpp
    case CmdPresentation:
    case CmdSlideShowFromStart:
    case CmdSlideShowFromCurrent:
    case CmdZoomIn:
    case CmdZoomOut:
    case CmdZoomFitPage:
    case CmdZoomActual:
    case CmdCopy:
    case CmdCopySlideText:
    case CmdCopySlidePicture: s.enabled = ready; break;
    case CmdNextPage:
    case CmdLastPage: s.enabled = ready && current_ + 1 < SlideCount(); break;
    case CmdPrevPage:
    case CmdFirstPage: s.enabled = ready && current_ > 0; break;
    case CmdNotesPane:
      s.enabled = Ready();
      s.checked = notesShown_;
      break;
    case CmdCopyNotes: s.enabled = ready && !deck_->Notes(current_).empty(); break;
    case CmdOpenSlideLink: s.enabled = ready; break;
    case CmdPrintLayoutMenu: s.enabled = ready; break;
    case CmdPrintHidden:
      s.enabled = ready;
      s.checked = print_.includeHidden;
      break;
    case CmdPrintFrame:
      s.enabled = ready;
      s.checked = print_.frame;
      break;
    default:
      for (const LayoutChoice& l : kLayouts)
        if (l.cmd == cmd) {
          s.enabled = ready;
          s.checked = LayoutIndex(print_) == static_cast<int>(&l - kLayouts);
        }
      if (cmd >= CmdZoomPreset && cmd < CmdZoomPreset + static_cast<int>(fw::ZoomPresets().size())) {  // r1-slide: View > Zoom
        const double z = fw::ZoomPresets()[static_cast<size_t>(cmd - CmdZoomPreset)];
        s.enabled = ready;
        s.checked = zoom_ > 0 && std::fabs(zoom_ - z) < 1e-6;
        s.label = fw::FormatPercent(z);
      }
      break;
  }
  for (const CommandName& n : kNames)
    if (n.cmd == cmd) {
      s.label = n.label;
      s.shortcut = n.keys;
    }
  if (cmd == CmdZoomFitPage) s.checked = zoom_ == 0;
  return s;
}

std::vector<int> PresentationView::Commands() {
  std::vector<int> out;
  for (const CommandName& n : kNames)
    if (n.cmd != CmdOpenSlideLink && n.cmd != CmdPrintLayoutMenu) out.push_back(n.cmd);
  for (int c : Objects().Commands()) out.push_back(c);  // p2-objects
  // P2: the slides' commands.
  for (int cmd : {CmdSlideNew, CmdSlideDuplicate, CmdSlideDelete, CmdSlideHide, CmdSlideMoveUp, CmdSlideMoveDown,
                  CmdSlideMoveFirst, CmdSlideMoveLast, CmdDesignPanel, CmdFormatBackground, CmdResetBackground,
                  CmdHeaderFooter, CmdSlideSize169, CmdSlideSize43})
    out.push_back(cmd);
  // r1-slide: the text commands (while text is edited) and the ribbon's themes and theme colors.
  for (int cmd : TextEditing::CommandIds())
    if (std::find(out.begin(), out.end(), cmd) == out.end()) out.push_back(cmd);
  for (size_t i = 0; i < BuiltInThemes().size(); ++i) out.push_back(CmdApplyTheme + static_cast<int>(i));
  for (size_t i = 0; i < ColorSchemes().size(); ++i) out.push_back(CmdApplyColors + static_cast<int>(i));
  return out;
}

bool PresentationView::Execute(int cmd) {
  if (!Ready()) return false;
  if (cmd == CmdSpelling && text_->Owns(cmd)) {  // p2-text: F7, maybe on another slide
    text_->NextMisspelling(current_, [this](size_t slide) { GoToSlide(slide); });
    return true;
  }
  if (text_->Owns(cmd)) return text_->Execute(cmd);
  if (ObjectsTake(cmd) && Objects().Execute(cmd)) return true;  // p2-objects
  if (SlideCommand(cmd)) return true;  // P2: the slides' commands
  const size_t n = SlideCount();
  switch (cmd) {
    case CmdPresentation:
    case CmdSlideShowFromStart: StartShow(false); return true;
    case CmdSlideShowFromCurrent: StartShow(true); return true;
    case CmdNextPage:
      if (current_ + 1 < n) GoToSlide(current_ + 1);
      return true;
    case CmdPrevPage:
      if (current_ > 0) GoToSlide(current_ - 1);
      return true;
    case CmdFirstPage: GoToSlide(0); return true;
    case CmdLastPage:
      if (n) GoToSlide(n - 1);
      return true;
    case CmdZoomIn:
    case CmdZoomOut: {
      const double now = zoom_ > 0 ? zoom_ : Scale() / (96.0 / 72.0 * ui_);
      const std::vector<double>& presets = fw::ZoomPresets();
      double next = now;
      if (cmd == CmdZoomIn) {
        next = presets.empty() ? now * 1.25 : presets.back();
        for (double z : presets)
          if (z > now * 1.001) {
            next = z;
            break;
          }
      } else {
        next = presets.empty() ? now / 1.25 : presets.front();
        for (auto it = presets.rbegin(); it != presets.rend(); ++it)
          if (*it < now * 0.999) {
            next = *it;
            break;
          }
      }
      SetZoom(next);
      return true;
    }
    case CmdZoomFitPage: SetZoom(0); return true;
    case CmdZoomActual: SetZoom(1); return true;
    case CmdNotesPane:
      notesShown_ = !notesShown_;
      LayoutParts();
      slideBitmap_.Reset();
      host_.ViewInvalidate();
      return true;
    case CmdCopy: CopySlide(true, true); return true;
    case CmdCopySlideText: CopySlide(true, false); return true;
    case CmdCopySlidePicture: CopySlide(false, true); return true;
    case CmdCopyNotes: CopyNotes(); return true;
    case CmdOpenSlideLink:
      if (const SlideLink* link = LinkAt(downX_, downY_)) FollowLink(*link);
      return true;
    case CmdPrintLayoutMenu: return false;  // the window shows FillMenu
    case CmdPrintHidden: print_.includeHidden = !print_.includeHidden; return true;
    case CmdPrintFrame: print_.frame = !print_.frame; return true;
    default:
      if (cmd >= CmdSlideShowMonitor && cmd < CmdSlideShowMonitor + 16) {
        showMonitor_ = cmd - CmdSlideShowMonitor;
        return true;
      }
      if (cmd >= CmdZoomPreset && cmd < CmdZoomPreset + static_cast<int>(fw::ZoomPresets().size())) {
        SetZoom(fw::ZoomPresets()[static_cast<size_t>(cmd - CmdZoomPreset)]);
        return true;
      }
      for (const LayoutChoice& l : kLayouts)
        if (l.cmd == cmd) {
          print_.what = l.what;
          if (l.what == PrintWhat::Handouts) print_.perPage = l.perPage;
          host_.ViewStateChanged();
          return true;
        }
      return false;
  }
}

void PresentationView::BuildToolbar(ToolbarModel& m, const ToolbarContext& ctx) {
  auto item = [](int cmd, wchar_t glyph, const std::wstring& tooltip = L"") {
    ToolItem t;
    t.cmd = cmd;
    t.glyph = glyph;
    t.tooltip = tooltip;
    return t;
  };
  ToolItem print = item(CmdPrint, icon::Print);
  // What Print and Export PDF lay out, as PowerPoint's print settings choose it.
  ToolItem layout;
  layout.kind = ToolKind::Dropdown;
  layout.cmd = CmdPrintLayoutMenu;
  layout.tooltip = L"Print layout: what Print and Export PDF put on each page";
  layout.width = 150;
  layout.priority = 6;
  for (const LayoutChoice& l : kLayouts) layout.choices.push_back(l.label);
  layout.choice = LayoutIndex(print_);
  layout.enabled = Ready() && SlideCount() > 0;
  layout.onChange = [this](const ToolValue& v) {
    if (v.index >= 0 && v.index < static_cast<int>(std::size(kLayouts))) Execute(kLayouts[v.index].cmd);
  };
  m.left = {print, layout};
  AddSlideTools(m);  // P2: ＋ Slide ▾ and Layout ▾ first
  text_->AddToolbar(m);  // the text being edited: font, size, emphasis, color, alignment, lists
  if (Ready()) Objects().AddTools(m);  // p2-objects: insert, arrange, format

  // ▶ Present ▾: from the beginning, or the list's choice.
  ToolItem present;
  present.kind = ToolKind::SplitButton;
  present.cmd = CmdPresentation;
  present.glyph = icon::Slideshow;
  present.tooltip = L"Present from the beginning (F5)";
  present.choices = {L"From the beginning\tF5", L"From the current slide\tShift+F5"};
  // With two monitors or more, where the show goes (p1-show's Monitors()).
  const std::vector<std::wstring> monitors = fw::slide::show::Monitors();
  if (monitors.size() >= 2)
    for (size_t i = 0; i < monitors.size(); ++i)
      present.choices.push_back((static_cast<int>(i) == showMonitor_ ? L"✓ Present on " : L"Present on ") + monitors[i]);
  present.enabled = Ready() && SlideCount() > 0;
  present.priority = 9;
  present.sepBefore = true;
  present.onChange = [this](const ToolValue& v) {
    if (v.index == 0) StartShow(false);
    else if (v.index == 1) StartShow(true);
    else if (v.index >= 2) showMonitor_ = v.index - 2;
  };
  ToolItem notes = item(CmdNotesPane, icon::TextDocument, L"Speaker notes");
  notes.toggle = true;
  notes.checked = notesShown_;
  notes.priority = 4;
  ToolItem find = item(CmdFind, icon::Search);
  find.checked = ctx.searchOpen;
  find.toggle = true;
  find.priority = 5;
  find.sepBefore = true;
  ToolItem full = item(CmdFullScreen, ctx.fullscreen ? icon::BackToWindow : icon::FullScreen);
  full.priority = 2;
  m.right = {present, notes, find, full};
}

void PresentationView::FillMenu(int which, HMENU menu) {
  if (which != CmdPrintLayoutMenu) return;
  for (const LayoutChoice& l : kLayouts) AppendItem(menu, l.cmd, l.label, true, LayoutIndex(print_) == &l - kLayouts, true);
  AppendSeparator(menu);
  AppendItem(menu, CmdPrintHidden, L"Print hidden slides", true, print_.includeHidden);
  AppendItem(menu, CmdPrintFrame, L"Frame slides", true, print_.frame);
}

void PresentationView::FillContextMenu(HMENU m, int x, int y) {
  if (!Ready()) return;
  downX_ = x;
  downY_ = y;
  if (text_->Editing() && !gfx::Contains(thumbs_, static_cast<float>(x), static_cast<float>(y))) {
    text_->FillContextMenu(m, current_, SlidePoint(x, y));
    Objects().TableContextMenu(m);  // p2-objects: a table cell's rows, columns, merge and split
    return;
  }
  // A right-click on a thumbnail is about that slide (and the others selected with it).
  if (const int t = ThumbAt(x, y); t >= 0 || gfx::Contains(thumbs_, static_cast<float>(x), static_cast<float>(y)))
    SlideContextMenu(m, t);  // P2: new, duplicate, delete, layout, hide
  else if (gfx::Contains(view_, static_cast<float>(x), static_cast<float>(y)))  // p2-objects
    Objects().ContextMenu(m, static_cast<float>(x), static_cast<float>(y));
  const bool any = SlideCount() > 0;
  if (LinkAt(x, y)) {
    AppendItem(m, CmdOpenSlideLink, L"Open link");
    AppendSeparator(m);
  }
  AppendItem(m, CmdCopy, L"Copy slide\tCtrl+C", any);
  AppendItem(m, CmdCopySlideText, L"Copy slide text", any);
  AppendItem(m, CmdCopySlidePicture, L"Copy slide as picture", any);
  AppendItem(m, CmdCopyNotes, L"Copy speaker notes", any && !deck_->Notes(current_).empty());
  AppendSeparator(m);
  AppendItem(m, CmdPresentation, L"Present from the beginning\tF5", any);
  AppendItem(m, CmdSlideShowFromCurrent, L"Present from this slide\tShift+F5", any);
  AppendSeparator(m);
  AppendItem(m, CmdZoomFitPage, L"Fit slide to window\tCtrl+0", any, zoom_ == 0);
  AppendItem(m, CmdNotesPane, L"Speaker notes", true, notesShown_);
}

void PresentationView::BuildStatusBar(StatusBarModel& m) {
  const size_t n = SlideCount();
  std::wstring slide = n ? L"Slide " + std::to_wstring(current_ + 1) + L" of " + std::to_wstring(n) : L"No slides";
  if (n && deck_->Hidden(current_)) slide += L" (hidden)";
  m.segments.push_back(slide);
  if (edit::Editor* ed = SlideEditor(); !ed || !ed->CanEdit()) m.segments.push_back(L"Read-only");
  m.zoom = n > 0;
  m.zoomValue = Scale() / (96.0 / 72.0 * ui_);
  m.onZoom = [this](double z) {
    SetZoom(z);
    host_.ViewInvalidate();
  };
}

// ---------------------------------------------------------------------------
// Find: the slides' text and the notes, slide by slide.

void PresentationView::StartSearch(const std::wstring& query, bool matchCase, bool wholeWords) {
  matches_.clear();
  match_ = -1;
  search_.query = query;  // p2-text: for Replace
  search_.matchCase = matchCase;
  search_.wholeWords = wholeWords;
  if (!Ready() || query.empty()) {
    host_.ViewInvalidate();
    return;
  }
  for (size_t s = 0; s < SlideCount(); ++s) {
    const std::vector<TextBlock> blocks = deck_->Text(s);
    for (size_t b = 0; b < blocks.size(); ++b)
      for (size_t at : FindAll(blocks[b].text, query, matchCase, wholeWords))
        matches_.push_back({s, false, b, at, query.size()});
    for (size_t at : FindAll(deck_->Notes(s), query, matchCase, wholeWords))
      matches_.push_back({s, true, 0, at, query.size()});
  }
  // The first match at or after the current slide.
  for (size_t i = 0; i < matches_.size(); ++i)
    if (matches_[i].slide >= current_) {
      match_ = static_cast<int>(i);
      break;
    }
  if (match_ < 0 && !matches_.empty()) match_ = 0;
  ShowMatch();
}

void PresentationView::ClearSearch() {
  matches_.clear();
  match_ = -1;
  host_.ViewInvalidate();
}

void PresentationView::SearchStep(bool forward) {
  if (matches_.empty()) return;
  const int n = static_cast<int>(matches_.size());
  match_ = match_ < 0 ? 0 : (match_ + (forward ? 1 : n - 1)) % n;
  ShowMatch();
}

void PresentationView::ShowMatch() {
  if (match_ < 0 || match_ >= static_cast<int>(matches_.size())) {
    host_.ViewInvalidate();
    return;
  }
  const Match& m = matches_[static_cast<size_t>(match_)];
  if (m.notes && !notesShown_) {
    notesShown_ = true;
    LayoutParts();
    slideBitmap_.Reset();
  }
  GoToSlide(m.slide);
  host_.ViewInvalidate();
}

std::wstring PresentationView::SelectedText() {
  if (match_ >= 0 && match_ < static_cast<int>(matches_.size())) {
    const Match& m = matches_[static_cast<size_t>(match_)];
    const std::wstring text = m.notes ? deck_->Notes(m.slide) : deck_->Text(m.slide)[m.block].text;
    return text.substr(m.at, m.length);
  }
  return {};
}

// ---------------------------------------------------------------------------
// Replace (p2-text): the matches found again in the text as the edit layer has it, the current one
// by its place among its slide's (or its notes') matches.

bool PresentationView::CanReplace() const { return text_->Model() != nullptr; }

bool PresentationView::ReplaceCurrent(const std::wstring& with) {
  TextModel* model = text_->Model();
  if (!model || match_ < 0 || match_ >= static_cast<int>(matches_.size())) return false;
  const Match cur = matches_[static_cast<size_t>(match_)];
  size_t rank = 0;
  for (int i = 0; i < match_; ++i)
    if (matches_[static_cast<size_t>(i)].slide == cur.slide && matches_[static_cast<size_t>(i)].notes == cur.notes) ++rank;
  std::vector<TextMatch> here;
  for (const TextMatch& m : fw::slide::view::FindAll(*model, search_.query, search_.matchCase, search_.wholeWords))
    if (m.target.slide == cur.slide && m.target.notes == cur.notes) here.push_back(m);
  if (rank >= here.size() || !ReplaceMatch(*model, here[rank], search_.query, search_.matchCase, with)) {
    host_.ViewMessage(BannerKind::Warning, L"That text can't be replaced here.");
    return false;
  }
  // Found again; the current match is the one after the replaced text.
  const auto search = search_;
  StartSearch(search.query, search.matchCase, search.wholeWords);
  size_t seen = 0;
  for (size_t i = 0; i < matches_.size(); ++i) {
    const Match& m = matches_[i];
    const bool same = m.slide == cur.slide && m.notes == cur.notes;
    if ((same && seen++ == rank) || m.slide > cur.slide || (m.slide == cur.slide && m.notes && !cur.notes)) {
      match_ = static_cast<int>(i);
      ShowMatch();
      return true;
    }
  }
  match_ = matches_.empty() ? -1 : 0;
  ShowMatch();
  return true;
}

int PresentationView::ReplaceAll(const std::wstring& with) {
  TextModel* model = text_->Model();
  if (!model || search_.query.empty()) return 0;
  const int n = fw::slide::view::ReplaceAll(*model, fw::slide::view::FindAll(*model, search_.query, search_.matchCase, search_.wholeWords), with);
  const auto search = search_;
  StartSearch(search.query, search.matchCase, search.wholeWords);
  return n;
}

// ---------------------------------------------------------------------------
// Copy

std::wstring PresentationView::SlideText(size_t slide) const {
  std::wstring out;
  for (const TextBlock& b : deck_->Text(slide)) {
    if (!out.empty()) out += L"\r\n";
    for (wchar_t ch : b.text) {
      if (ch == L'\n') out += L"\r\n";
      else out += ch;
    }
  }
  return out;
}

void PresentationView::CopySlide(bool text, bool picture) {
  if (!Ready() || SlideCount() == 0) return;
  const std::wstring words = SlideText(current_);
  // The picture: the slide at 1920 pixels across (PowerPoint copies 1280), as PNG and a bitmap.
  std::vector<uint8_t> png, dib;
  if (picture && !util::UsingPrivateClipboard()) {
    const draw::Size s = deck_->SlideSize();
    const UINT w = 1920, h = std::max(1u, static_cast<UINT>(std::lround(1920.0f * s.height / s.width)));
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<ID2D1DeviceContext> dc;
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (SUCCEEDED(gfx::WIC()->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)) &&
        SUCCEEDED(gfx::D2D()->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target)) && SUCCEEDED(target.As(&dc))) {
      const HRESULT drawn = draw::DrawFrame(dc.Get(), [&](bool fallback) {
        dc->Clear(D2D1::ColorF(1, 1, 1, 1));
        dc->SetTransform(D2D1::Matrix3x2F::Scale(w / s.width, h / s.height));
        draw::D2DPainter painter(dc.Get());
        painter.UsePdfLikeDrawing();
        painter.SetNoShadows(fallback);
        deck_->Paint(current_, painter);
      });
      if (SUCCEEDED(drawn)) {
        // PNG, through a memory stream.
        ComPtr<IStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) &&
            SUCCEEDED(gfx::WIC()->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
            SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
            SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
            SUCCEEDED(frame->WriteSource(bitmap.Get(), nullptr)) && SUCCEEDED(frame->Commit()) &&
            SUCCEEDED(encoder->Commit())) {
          STATSTG st{};
          stream->Stat(&st, STATFLAG_NONAME);
          png.resize(static_cast<size_t>(st.cbSize.QuadPart));
          LARGE_INTEGER zero{};
          stream->Seek(zero, STREAM_SEEK_SET, nullptr);
          ULONG read = 0;
          stream->Read(png.data(), static_cast<ULONG>(png.size()), &read);
          png.resize(read);
        }
        // A bottom-up 24-bit DIB for programs that don't take PNG (the slide is opaque).
        ComPtr<IWICBitmapLock> lock;
        const WICRect all{0, 0, static_cast<INT>(w), static_cast<INT>(h)};
        if (SUCCEEDED(bitmap->Lock(&all, WICBitmapLockRead, &lock))) {
          UINT stride = 0, size = 0;
          BYTE* px = nullptr;
          lock->GetStride(&stride);
          lock->GetDataPointer(&size, &px);
          const UINT rowBytes = (w * 3 + 3) & ~3u;
          BITMAPINFOHEADER hdr{sizeof(hdr), static_cast<LONG>(w), static_cast<LONG>(h), 1, 24, BI_RGB};
          dib.resize(sizeof(hdr) + static_cast<size_t>(rowBytes) * h);
          std::memcpy(dib.data(), &hdr, sizeof(hdr));
          for (UINT y = 0; y < h && px; ++y) {
            const BYTE* src = px + static_cast<size_t>(y) * stride;
            BYTE* dst = dib.data() + sizeof(hdr) + static_cast<size_t>(h - 1 - y) * rowBytes;
            for (UINT x = 0; x < w; ++x) std::memcpy(dst + x * 3, src + x * 4, 3);
          }
        }
      }
    }
  }
  if (util::UsingPrivateClipboard() || png.empty()) {
    if (text) util::SetClipboardText(words);
  } else if (OpenClipboard(host_.ViewHwnd())) {
    EmptyClipboard();
    static const UINT pngFormat = RegisterClipboardFormatW(L"PNG");
    PutClipboard(pngFormat, png.data(), png.size());
    PutClipboard(CF_DIB, dib.data(), dib.size());
    if (text && !words.empty()) PutClipboard(CF_UNICODETEXT, words.c_str(), (words.size() + 1) * sizeof(wchar_t));
    CloseClipboard();
  }
  host_.ViewMessage(BannerKind::Success, picture && text ? L"Copied the slide"
                                         : picture       ? L"Copied the slide as a picture"
                                                         : L"Copied the slide's text");
}

void PresentationView::CopyNotes() {
  if (!Ready() || SlideCount() == 0) return;
  std::wstring out;
  for (wchar_t ch : deck_->Notes(current_)) {
    if (ch == L'\n') out += L"\r\n";
    else out += ch;
  }
  if (out.empty()) return;
  util::SetClipboardText(out);
  host_.ViewMessage(BannerKind::Success, L"Copied the speaker notes");
}

// ---------------------------------------------------------------------------
// Links

const SlideLink* PresentationView::LinkAt(int x, int y) const {
  if (!Ready() || SlideCount() == 0 || !gfx::Contains(view_, static_cast<float>(x), static_cast<float>(y))) return nullptr;
  const D2D1_RECT_F rc = SlideRect();
  const float k = gfx::Width(rc) / std::max(1.0f, deck_->SlideSize().width);
  const float px = (static_cast<float>(x) - rc.left) / k, py = (static_cast<float>(y) - rc.top) / k;
  if (linksOf_ != current_) {
    links_ = deck_->Links(current_);
    linksOf_ = current_;
  }
  // The topmost first: the last drawn.
  for (auto it = links_.rbegin(); it != links_.rend(); ++it)
    if (px >= it->rect.left && px < it->rect.right && py >= it->rect.top && py < it->rect.bottom) return &*it;
  return nullptr;
}

void PresentationView::FollowLink(const SlideLink& link) {
  if (link.slide >= 0) {
    if (static_cast<size_t>(link.slide) < SlideCount()) GoToSlide(static_cast<size_t>(link.slide));
    else host_.ViewMessage(BannerKind::Warning, L"The slide this link points to isn't in the presentation.");
    return;
  }
  if (link.slide == -2) {  // the next slide
    if (current_ + 1 < SlideCount()) GoToSlide(current_ + 1);
    return;
  }
  if (!util::OpenExternalLink(link.url))
    host_.ViewMessage(BannerKind::Warning, L"This link was blocked because it doesn’t point to a website.");
}

// ---------------------------------------------------------------------------
// The slide show and the banner

void PresentationView::StartShow(bool fromCurrent) {
  if (!Ready() || SlideCount() == 0 || fw::slide::show::Running()) return;
  Presentation* model = deck_->Model();
  if (!model) return;
  // p1-show's renderer for the show: slides drawn by p1-render's SlideRender.
  if (!showRenderer_) showRenderer_ = fw::slide::show::MakeRenderer(*model);
  fw::slide::show::ShowOptions o;
  o.first = fromCurrent ? current_ : 0;
  o.owner = host_.ViewHwnd();
  o.monitor = showMonitor_ < static_cast<int>(fw::slide::show::Monitors().size()) ? showMonitor_ : -1;
  o.deckPath = path_;
  // The show ends on a slide: the tab shows it, as PowerPoint's normal view does.
  o.ended = [this](size_t slide) {
    showing_ = false;
    GoToSlide(slide);
    host_.ViewInvalidate();
  };
  o.openFile = [](const std::wstring&) { return false; };  // links to files aren't followed from the show in P1
  showing_ = fw::slide::show::Start(*model, *showRenderer_, o);
  if (!showing_) host_.ViewMessage(BannerKind::Warning, L"The slide show couldn't start.");
}

void PresentationView::ReadOnlyBanner() {
  // P2: only a deck the edit layer can't change says it's read-only, with the reason.
  edit::Editor* ed = SlideEditor();
  std::u16string why;
  const bool editable = ed && ed->CanEdit(&why);
  const bool macros = deck_ && deck_->HasMacros();
  if (editable) {
    if (macros) host_.ViewMessage(BannerKind::Info, L"This presentation's macros are kept but never run.");
    return;
  }
  std::wstring text = why.empty() ? std::wstring(L"This presentation is read-only.") : std::wstring(why.begin(), why.end());
  if (macros) text += L" Its macros are kept but never run.";
  host_.ViewMessage(BannerKind::Info, text);
}

}  // namespace fw::slide::view
