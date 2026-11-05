// The shell around the tabs: the side panel, the status bar and command search, and the names
// the window gives its own commands for command search and the toolbar.

#include <algorithm>
#include <cmath>

#include "App.h"
#include "Commands.h"
#include "FileAssoc.h"

using namespace gfx;

namespace {

// The window's own commands, with the names and keys command search shows. A tab may name one
// itself when its keys differ.
struct AppCommand {
  int cmd;
  const wchar_t* label;
  const wchar_t* keys;
};

constexpr AppCommand kAppCommands[] = {
    {CmdOpen, L"Open", L"Ctrl+O"},
    {CmdSave, L"Save", L"Ctrl+S"},
    {CmdSaveAs, L"Save as…", L"Ctrl+Shift+S"},
    {CmdPrint, L"Print", L"Ctrl+P"},
    {CmdFind, L"Find", L"Ctrl+F"},
    {CmdFindNext, L"Find next", L"F3"},
    {CmdFindPrev, L"Find previous", L"Shift+F3"},
    {CmdCloseTab, L"Close tab", L"Ctrl+W"},
    {CmdNextTab, L"Next tab", L"Ctrl+Tab"},
    {CmdPrevTab, L"Previous tab", L"Ctrl+Shift+Tab"},
    {CmdFullScreen, L"Full screen", L"F11"},
    {CmdPresentation, L"Present", L""},
    {CmdToggleInvert, L"Night reading: dark pages", L""},
    {CmdThemeSystem, L"App theme: use Windows setting", L""},
    {CmdThemeLight, L"App theme: light", L""},
    {CmdThemeDark, L"App theme: dark", L""},
    {CmdToggleHighlight, L"Highlight form fields", L""},
    {CmdToggleSpelling, L"Check spelling as you type", L""},
    {CmdToggleAutoCorrect, L"AutoCorrect as you type", L""},
    {CmdClearRecent, L"Clear recent files", L""},
    {CmdToggleAutoUpdate, L"Update automatically", L""},
    {CmdCheckUpdates, L"Check for updates now", L""},
    {CmdMakeDefault, L"Make Featherweight the default PDF app", L""},
    {CmdAbout, L"About Featherweight", L""},
    {CmdExit, L"Exit", L"Alt+F4"},
};

const AppCommand* FindAppCommand(int cmd) {
  for (const AppCommand& c : kAppCommands)
    if (c.cmd == cmd) return &c;
  return nullptr;
}

}  // namespace

void App::InitShell() {
  panel_.SetWindow(hwnd_);
  status_.SetWindow(hwnd_);
  commands_.SetWindow(hwnd_);
  commands_.onEscape = [this] { CloseCommandSearch(); };
  commands_.onRun = [this](int cmd) {
    CloseCommandSearch();
    Execute(cmd);
  };
}

// ---------------------------------------------------------------------------
// Commands

CommandState App::QueryCommand(int cmd) {
  TabContent* v = Active();
  const bool ready = v && v->Ready();
  CommandState s = ready ? v->QueryCommand(cmd) : CommandState{};
  const AppCommand* own = FindAppCommand(cmd);
  if (!own) return s;
  if (s.label.empty()) {
    s.label = own->label;
    s.shortcut = own->keys;
  }
  const bool tabEnabled = s.enabled;
  s.visible = s.enabled = true;
  switch (cmd) {
    case CmdSave: s.enabled = ready && tabEnabled; break;
    case CmdSaveAs: s.enabled = ready; break;
    case CmdPrint: s.enabled = ready && v->CanPrint(); break;
    case CmdFind:
    case CmdFindNext:
    case CmdFindPrev: s.enabled = ready && !presentation_; break;
    case CmdCloseTab: s.enabled = active_ >= 0; break;
    case CmdNextTab:
    case CmdPrevTab: s.enabled = tabs_.size() > 1; break;
    case CmdFullScreen: s.label = fullscreen_ ? L"Exit full screen" : L"Full screen"; break;
    case CmdPresentation: s.enabled = ready && v->Paged(); break;
    case CmdToggleInvert: s.checked = settings_.invert; break;
    case CmdThemeSystem: s.checked = settings_.theme == ThemeSetting::System; break;
    case CmdThemeLight: s.checked = settings_.theme == ThemeSetting::Light; break;
    case CmdThemeDark: s.checked = settings_.theme == ThemeSetting::Dark; break;
    case CmdToggleHighlight: s.checked = settings_.highlightFields; break;
    case CmdToggleSpelling:
    case CmdToggleAutoCorrect:
      s.checked = cmd == CmdToggleSpelling ? settings_.spelling : settings_.autoCorrect;
      s.visible = ready && v->Kind() == TabKind::Document;
      break;
    case CmdToggleAutoUpdate: s.checked = settings_.autoUpdate; break;
    case CmdClearRecent: s.enabled = !settings_.recent.empty(); break;
    case CmdMakeDefault: s.visible = !fileassoc::IsDefault(); break;
    default: break;
  }
  return s;
}

void App::NameToolbarItems(ToolbarModel& m) {
  for (std::vector<ToolItem>* items : {&m.left, &m.right}) {
    items->erase(std::remove_if(items->begin(), items->end(),
                                [this](const ToolItem& it) { return it.cmd && !QueryCommand(it.cmd).visible; }),
                 items->end());
    for (ToolItem& it : *items)
      if (it.tooltip.empty()) it.tooltip = QueryCommand(it.cmd).Tooltip();
  }
}

std::vector<CommandEntry> App::SearchableCommands() {
  TabContent* v = Active();
  std::vector<int> ids;
  if (v && v->Ready()) ids = v->Commands();
  for (const AppCommand& c : kAppCommands)
    if (std::find(ids.begin(), ids.end(), c.cmd) == ids.end()) ids.push_back(c.cmd);
  std::vector<CommandEntry> out;
  for (int id : ids) {
    CommandState s = QueryCommand(id);
    if (s.visible && !s.label.empty()) out.push_back({id, s.label, s.shortcut, s.enabled, s.checked});
  }
  return out;
}

// ---------------------------------------------------------------------------
// Command search

void App::OpenCommandSearch() {
  if (presentation_) return;
  if (!commands_.IsOpen()) {
    commandsReturn_ = CurrentKeyArea();
    HideTooltip();
    commands_.Open(SearchableCommands());
  }
  FocusBox(&commands_.Box());
  commands_.Box().SelectAll();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::CloseCommandSearch() {
  const KeyArea back = commandsReturn_;
  commandsReturn_ = KeyArea::Doc;
  commands_.Close();
  if (focusBox_ == &commands_.Box()) FocusBox(nullptr);
  if (back != KeyArea::Doc) MoveKeyTo(back, true, true);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Side panel and status bar

void App::ViewShowPanel(PanelContent* content) {
  if (!content) {
    ClosePanel();
    return;
  }
  const bool hadKeys = panel_.HasKeyFocus();
  panel_.Show(content);
  if (hadKeys) panel_.FocusIn(true, false);
  KeyFocusMoved();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::ClosePanel() {
  if (!panel_.IsOpen()) return;
  const bool hadKeys = panel_.HasKeyFocus();
  const KeyArea back = panelReturn_;
  panelReturn_ = KeyArea::Doc;
  panel_.Show(nullptr);
  if (hadKeys) MoveKeyTo(back == KeyArea::Status ? KeyArea::Status : KeyArea::Doc, true, true);
  KeyFocusMoved();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

bool App::StatusBarShown() const {
  if (presentation_) return false;
  TabContent* v = Active();
  return v && v->Ready() && v->HasStatusBar();
}

int App::StatusBarHeight() const { return static_cast<int>(std::lround(StatusBar::kHeight * ui_)); }

StatusBarModel App::CurrentStatusBar() {
  StatusBarModel m;
  if (StatusBarShown()) Active()->BuildStatusBar(m);
  return m;
}

void App::LayoutShell() {
  RECT rc;
  GetClientRect(hwnd_, &rc);
  const int titleH = TitleHeight();
  const int top = titleH + (fullscreen_ || presentation_ ? 0 : ToolbarHeight());
  const int statusH = StatusBarShown() ? StatusBarHeight() : 0;
  if (statusH) {
    status_.Layout(RECT{0, rc.bottom - statusH, rc.right, rc.bottom}, ui_, CurrentStatusBar());
  } else if (status_.HasKeyFocus()) {
    status_.Blur();
    KeyFocusMoved();
  }
  panel_.Layout(RECT{0, top, rc.right, rc.bottom - statusH}, ui_);
  commands_.Layout(RECT{0, titleH, rc.right, titleH + ToolbarHeight()}, rc, ui_);
}

void App::DrawShell() {
  if (presentation_) return;
  if (StatusBarShown()) status_.Draw(canvas_);
  panel_.Draw(canvas_, windowActive_);
  panel_.DrawPopup(canvas_);
}

// ---------------------------------------------------------------------------
// Keyboard

App::KeyArea App::CurrentKeyArea() const {
  if (panel_.HasKeyFocus()) return KeyArea::Panel;
  if (status_.HasKeyFocus()) return KeyArea::Status;
  return KeyArea::Doc;
}

bool App::MoveKeyTo(KeyArea area, bool forward, bool resume) {
  const KeyArea from = CurrentKeyArea();
  if (area == from && area != KeyArea::Doc) return true;
  switch (area) {
    case KeyArea::Panel:
      if (!panel_.FocusIn(forward, resume)) return false;
      panelReturn_ = from;
      status_.Blur();
      break;
    case KeyArea::Status:
      if (!StatusBarShown() || !status_.FocusIn()) return false;
      panel_.Blur();
      break;
    case KeyArea::Doc:
      panel_.Blur();
      status_.Blur();
      break;
  }
  FocusBox(nullptr);
  toolbar_.Blur();
  KeyFocusMoved();
  InvalidateRect(hwnd_, nullptr, FALSE);
  return true;
}

bool App::ShellKeyDown(UINT vk, UINT mods) {
  const bool ctrl = (mods & kModCtrl) != 0, shift = (mods & kModShift) != 0, alt = (mods & kModAlt) != 0;
  if (vk == VK_F6 && !ctrl && !alt) {
    // The document, the side panel and the status bar in turn. Without them F6 is the tab's.
    std::vector<KeyArea> areas{KeyArea::Doc};
    if (panel_.IsOpen()) areas.push_back(KeyArea::Panel);
    if (StatusBarShown() && status_.CanFocus()) areas.push_back(KeyArea::Status);
    if (areas.size() < 2) return false;
    const int n = static_cast<int>(areas.size());
    const int at = static_cast<int>(std::find(areas.begin(), areas.end(), CurrentKeyArea()) - areas.begin());
    for (int k = 1; k < n; ++k) {
      const KeyArea next = areas[((at + (shift ? -k : k)) % n + n) % n];
      if (MoveKeyTo(next, !shift, true)) break;
    }
    return true;
  }
  if (!panel_.HasKeyFocus() && !status_.HasKeyFocus()) return false;

  // The window's shortcuts still work, except the clipboard's: those are for the panel's boxes.
  TabContent* v = Active();
  int cmd = AppShortcut(vk, mods);
  if (!cmd) cmd = v ? v->ShortcutFor(vk, mods) : PdfView::Shortcut(vk, mods);
  if (cmd && !(ctrl && (vk == 'A' || vk == 'C' || vk == 'V' || vk == 'X' || vk == 'Z'))) {
    Execute(cmd);
    return true;
  }
  const bool inPanel = panel_.HasKeyFocus();
  const bool used = inPanel ? panel_.OnKeyDown(vk, ctrl, shift, alt) : status_.OnKeyDown(vk, ctrl, shift);
  if (!used && vk == VK_ESCAPE) {
    if (inPanel) ClosePanel();
    else MoveKeyTo(KeyArea::Doc, true, true);
  }
  InvalidateRect(hwnd_, nullptr, FALSE);
  return true;
}

bool App::ShellChar(wchar_t ch) {
  if (panel_.HasKeyFocus()) {
    if (panel_.OnChar(ch)) InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
  }
  return status_.HasKeyFocus();
}

bool App::TabIntoPanel(UINT vk, UINT mods) {
  if (vk != VK_TAB || (mods & (kModCtrl | kModAlt)) || !panel_.IsOpen()) return false;
  return MoveKeyTo(KeyArea::Panel, (mods & kModShift) == 0, false);
}

// ---------------------------------------------------------------------------
// Pointer

App::Region App::ShellRegionAt(int x, int y) const {
  if (presentation_) return Region::None;
  if (panel_.Hit(x, y)) return Region::Panel;
  if (StatusBarShown() && status_.Hit(x, y)) return Region::Status;
  return Region::None;
}

void App::ShellMouseDown(Region r, int x, int y, int button, bool dbl, bool shift) {
  if (r != Region::Panel) panel_.Blur();
  if (r != Region::Status) status_.Blur();
  if (button != 0) return;
  switch (r) {
    case Region::Panel:
      if (!panel_.HasKeyFocus()) panelReturn_ = KeyArea::Doc;
      panel_.OnMouseDown(x, y, dbl, shift);
      break;
    case Region::Status: status_.OnMouseDown(x, y); break;
    case Region::Commands: commands_.OnMouseDown(x, y, shift, dbl); break;
    default: break;
  }
}

bool App::ShellMouseMove(Region r, int x, int y) {
  switch (r) {
    case Region::Panel: return panel_.OnMouseMove(x, y);
    case Region::Status: return status_.OnMouseMove(x, y);
    case Region::Commands:
      if (capture_ == Region::Commands && GetKeyState(VK_LBUTTON) < 0) commands_.OnMouseDrag(x, y);
      return commands_.OnMouseMove(x, y);
    default: return false;
  }
}

void App::ShellMouseUp(Region r, int x, int y, int button) {
  if (button != 0) return;
  switch (r) {
    case Region::Panel:
      if (panel_.OnMouseUp(x, y)) ClosePanel();
      break;
    case Region::Status: status_.OnMouseUp(x, y); break;
    case Region::Commands: commands_.OnMouseUp(x, y); break;
    default: break;
  }
}

void App::ShellMouseLeave(Region r) {
  switch (r) {
    case Region::Panel: panel_.OnMouseLeave(); break;
    case Region::Status: status_.OnMouseLeave(); break;
    case Region::Commands: commands_.OnMouseLeave(); break;
    default: break;
  }
}

bool App::ShellWheel(int delta, int x, int y) {
  if (commands_.OnWheel(delta, x, y)) return true;
  switch (ShellRegionAt(x, y)) {
    case Region::Panel: panel_.OnWheel(delta, x, y); return true;
    case Region::Status: status_.OnWheel(delta, x, y); return true;
    default: return false;
  }
}

HCURSOR App::ShellCursor(Region r, int x, int y) const {
  static HCURSOR arrow = LoadCursor(nullptr, IDC_ARROW), ibeam = LoadCursor(nullptr, IDC_IBEAM);
  switch (r) {
    case Region::Panel: return panel_.OverText(x, y) ? ibeam : arrow;
    case Region::Status: return arrow;
    case Region::Commands: return commands_.HitBox(x, y) ? ibeam : arrow;
    default: return nullptr;
  }
}

std::wstring App::ShellTooltip(Region r, int x, int y) const {
  switch (r) {
    case Region::Panel: return panel_.TooltipAt(x, y);
    case Region::Status: return status_.TooltipAt(x, y);
    default: return {};
  }
}

// ---------------------------------------------------------------------------
// UI Automation

void App::DescribeShell(uia::Tree& t) {
  if (presentation_) return;
  // An element of the panel or the status bar takes the keyboard from everything else, as a
  // click there does.
  auto takeFocus = [&](int first, KeyArea area) {
    for (int k = first; k < t.Size(); ++k) {
      if (!t[k].focus) continue;
      t[k].focus = [this, area, f = std::move(t[k].focus)] {
        const KeyArea from = CurrentKeyArea();
        FocusBox(nullptr);
        toolbar_.Blur();
        if (area != KeyArea::Panel) panel_.Blur();
        if (area != KeyArea::Status) status_.Blur();
        f();
        if (area == KeyArea::Panel && from != KeyArea::Panel) panelReturn_ = from;
        KeyFocusMoved();
      };
    }
  };
  int first = t.Size();
  panel_.Describe(t, 0, [this] { ClosePanel(); });
  takeFocus(first, KeyArea::Panel);
  if (StatusBarShown()) {
    first = t.Size();
    status_.Describe(t, 0, [this] { status_.FocusIn(); });
    takeFocus(first, KeyArea::Status);
  }
  commands_.Describe(t, 0, [this] { FocusBox(&commands_.Box()); });
}
