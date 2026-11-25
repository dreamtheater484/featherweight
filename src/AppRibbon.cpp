// The ribbon in the window (R1, plan §5.6): the active editor's ribbon in place of its toolbar,
// the File page, quick access in the title bar, KeyTips and Ctrl+F1, the ribbon's commands in
// command search, and the script steps its tests run.

#include <algorithm>
#include <cmath>
#include <cwctype>

#include "App.h"
#include "Commands.h"
#include "UserTemplates.h"
#include "Util.h"
#include "core/FileKinds.h"
#include "core/Lang.h"
#include "core/Strings.h"
#include "pdf/PdfText.h"
#include "pkg/Backups.h"
#include "ui/Icons.h"
#include "ui/Menu.h"
#include "word/DocView.h"

using namespace gfx;
using menu::AppendItem;
using menu::AppendSeparator;

namespace {

// Below this much room (DIPs of the screen's work area), the ribbon starts with only its tabs.
constexpr float kSmallScreenH = 720;
// A menu of a ribbon item's choices: ids from here (never commands).
constexpr int kChoiceFirst = 0xB000;

#if FW_TEST_SCREENS
// A script step's report on stderr, as UTF-8 (fwprintf's C locale turns "…" and "›" into "?").
void Report(const std::wstring& s) { fputs(fw::WideToUtf8(s).c_str(), stderr); }
#endif

// Menus take & as the next letter's underline: a name's own & is doubled.
std::wstring MenuText(const std::wstring& s) {
  std::wstring out;
  for (wchar_t c : s) {
    if (c == L'&') out += L'&';
    out += c;
  }
  return out;
}

#if FW_TEST_SCREENS
// Every command id in a menu and its submenus.
void MenuCommands(HMENU m, std::vector<int>& out) {
  const int n = GetMenuItemCount(m);
  for (int i = 0; i < n; ++i) {
    if (HMENU sub = GetSubMenu(m, i)) {
      MenuCommands(sub, out);
      continue;
    }
    const UINT id = GetMenuItemID(m, i);
    if (id != 0 && id != static_cast<UINT>(-1)) out.push_back(static_cast<int>(id));
  }
}

// Ids that stand for a list that changes from PC to PC (recent files, the user's templates):
// counted as their first, so the coverage baseline is the same everywhere.
int Canonical(int cmd) {
  if (cmd >= CmdRecent && cmd < CmdRecent + Settings::kMaxRecent) return CmdRecent;
  if (cmd >= CmdUserTemplate && cmd < CmdUserTemplatesFolder) return CmdUserTemplate;
  return cmd;
}
#endif

}  // namespace

void App::InitRibbon() {
  ribbon_.SetWindow(hwnd_);
  ribbon_.query = [this](int cmd) { return QueryCommand(cmd); };
  ribbon_.onCommand = [this](int cmd) { Execute(cmd); };
  ribbon_.onMenu = [this](const RibbonItem& item, const D2D1_RECT_F& anchor) { ShowRibbonMenu(item, anchor); };
  filePage_.onCommand = [this](int cmd) { Execute(cmd); };
  filePage_.onClose = [this] { CloseFilePage(); };
  // Only the tab names on a small screen, until the user chooses (Ctrl+F1, remembered).
  bool collapsed = settings_.ribbon == 1;
  if (settings_.ribbon < 0 && !snapshot_) {
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
    collapsed = (mi.rcWork.bottom - mi.rcWork.top) / ui_ < kSmallScreenH;
  }
  ribbon_.SetCollapsed(collapsed);
}

RibbonModel App::CurrentRibbon() {
  RibbonModel m;
  TabContent* v = Active();
  if (!v || !v->Ready() || presentation_) return m;
  v->BuildRibbon(m, ToolbarContext{settings_.invert, search_.IsOpen(), fullscreen_});
  // As on the toolbar: what this kind of tab doesn't offer goes, and groups and tabs left empty.
  for (RibbonTab& t : m.tabs) {
    for (RibbonGroup& g : t.groups)
      g.items.erase(std::remove_if(g.items.begin(), g.items.end(),
                                   [this](const RibbonItem& it) { return it.tool.cmd && !QueryCommand(it.tool.cmd).visible; }),
                    g.items.end());
    t.groups.erase(std::remove_if(t.groups.begin(), t.groups.end(), [](const RibbonGroup& g) { return g.items.empty(); }),
                   t.groups.end());
  }
  m.tabs.erase(std::remove_if(m.tabs.begin(), m.tabs.end(), [](const RibbonTab& t) { return t.groups.empty(); }), m.tabs.end());
  return m;
}

std::vector<TitleBar::QuickButton> App::QuickAccess(const RibbonModel& m) {
  std::vector<TitleBar::QuickButton> q;
  const std::pair<int, wchar_t> buttons[] = {{CmdSave, icon::Save}, {CmdUndo, icon::Undo}, {CmdRedo, icon::Redo}};
  for (const auto& [cmd, glyph] : buttons) {
    const CommandState s = QueryCommand(cmd);
    if (!s.visible) continue;
    TitleBar::QuickButton b{cmd, glyph, s.Tooltip(), s.enabled};
    // The tab's list for it (the actions Undo takes back), behind a ▾.
    for (const ToolItem& it : m.quick)
      if (it.cmd == cmd && !it.choices.empty()) b.list = true;
    q.push_back(b);
  }
  // The tab's own buttons after Redo.
  for (const ToolItem& it : m.quick) {
    if (!it.glyph || it.cmd == CmdSave || it.cmd == CmdUndo || it.cmd == CmdRedo) continue;
    const CommandState s = QueryCommand(it.cmd);
    if (s.visible) q.push_back({it.cmd, it.glyph, s.Tooltip(), s.enabled});
  }
  return q;
}

void App::ShowQuickList(int index) {
  const int cmd = title_.QuickCommand(index);
  for (const ToolItem& it : ribbon_.Model().quick)
    if (it.cmd == cmd && !it.choices.empty()) {
      RibbonItem item;
      item.tool = it;
      ShowRibbonMenu(item, title_.QuickRect(index));
      return;
    }
}

void App::ShowRibbonMenu(const RibbonItem& from, const D2D1_RECT_F& anchor) {
  TabContent* v = Active();
  if (!v || !v->Ready()) return;
  const RibbonItem item = from;  // a copy: the menu's loop paints, and every frame replaces the ribbon's model
  if (item.menu.empty() && item.tool.choices.empty()) {
    ShowMenu(item.tool.cmd);  // the tab's own (FillMenu), or the window's (zoom, page layout)
    return;
  }
  HMENU m = CreatePopupMenu();
  if (!item.menu.empty()) {
    bool sep = false;
    for (int cmd : item.menu) {
      if (!cmd) {
        sep = GetMenuItemCount(m) > 0;
        continue;
      }
      const CommandState s = QueryCommand(cmd);
      if (!s.visible || s.label.empty()) continue;
      if (sep) AppendSeparator(m);
      sep = false;
      AppendItem(m, static_cast<UINT>(cmd), MenuText(s.label) + (s.shortcut.empty() ? L"" : L"\t" + s.shortcut), s.enabled, s.checked);
    }
  } else {
    for (size_t i = 0; i < item.tool.choices.size(); ++i)
      AppendItem(m, static_cast<UINT>(kChoiceFirst + i), MenuText(item.tool.choices[i]), item.tool.enabled,
                 static_cast<int>(i) == item.tool.choice, true);
  }
  openMenu_ = item.tool.cmd;
  const int cmd = GetMenuItemCount(m) > 0 ? TrackMenu(m, anchor, false) : 0;
  openMenu_ = 0;
  DestroyMenu(m);
  if (cmd >= kChoiceFirst && cmd < kChoiceFirst + static_cast<int>(item.tool.choices.size())) {
    ToolValue value;
    value.index = cmd - kChoiceFirst;
    value.text = item.tool.choices[static_cast<size_t>(value.index)];
    if (item.tool.onChange) item.tool.onChange(value);
    InvalidateRect(hwnd_, nullptr, FALSE);
  } else if (cmd) {
    Execute(cmd);
  }
}

void App::ToggleRibbon() {
  if (!ribbonShown_) return;
  ribbon_.SetCollapsed(!ribbon_.Collapsed());
  settings_.ribbon = ribbon_.Collapsed() ? 1 : 0;
  if (!snapshot_ && !sandbox_) settings_.Save();
  Layout();
}

// ---------------------------------------------------------------------------
// The File page

void App::OpenFilePage() {
  if (!ribbonShown_ || presentation_) return;
  if (commands_.IsOpen()) CloseCommandSearch();
  ribbon_.ClosePopups();
  ribbon_.EndKeyTips();
  ribbon_.Blur();
  HideTooltip();
  // Lists from disk are read once, as the page opens.
  userTemplates_ = usertemplates::List(kUserTemplateCount);
  fileBackup_ = !LatestBackup().empty();
  filePage_.Open(ui::FilePage::kInfo);
  Layout();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::CloseFilePage() {
  if (!filePage_.IsOpen()) return;
  filePage_.Close();
  Layout();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

ui::FilePage::Model App::CurrentFilePage() {
  using FP = ui::FilePage;
  FP::Model m;
  TabContent* v = Active();
  const bool ready = v && v->Ready();
  if (ready) {
    m.title = v->Title();
    m.path = v->Path().empty() ? T(L"Not saved yet") : v->Path();
  }
  auto entry = [](int cmd, std::wstring label, wchar_t glyph = 0, std::wstring detail = {}) {
    FP::Entry e;
    e.cmd = cmd;
    e.label = std::move(label);
    e.glyph = glyph;
    e.detail = std::move(detail);
    return e;
  };
  auto heading = [](std::wstring label) {
    FP::Entry e;
    e.label = std::move(label);
    return e;
  };
  // A command as the window names it; `setting`: shown on or off.
  auto command = [&](int cmd, wchar_t glyph = 0, std::wstring detail = {}, bool setting = false) {
    const CommandState s = QueryCommand(cmd);
    FP::Entry e = entry(cmd, s.label, glyph, std::move(detail));
    e.enabled = s.enabled;
    e.checked = s.checked;
    e.toggle = setting;
    return e;
  };
  auto kindGlyph = [](fw::FileKind k) {
    return k == fw::FileKind::Workbook ? icon::Table : k == fw::FileKind::Presentation ? icon::Slideshow
                                       : k == fw::FileKind::Pdf                         ? icon::PDF
                                                                                        : icon::Document;
  };

  // New: a blank file of each kind, the built-in templates and the user's own.
  auto& fresh = m.entries[FP::kNew];
  for (const fw::FileKindInfo& k : fw::FileKinds())
    if (fw::EditorShipped(k.kind))
      fresh.push_back(entry(CmdNewFile + static_cast<int>(k.kind), fw::Phrase(k.kind, fw::KindPhrase::Blank), kindGlyph(k.kind),
                            k.kind == fw::FileKind::Document ? L"Ctrl+N" : L""));
  const bool docs = fw::EditorShipped(fw::FileKind::Document);
  if (docs || !userTemplates_.empty()) fresh.push_back(heading(T(L"Templates")));
  if (docs)
    for (int i = 0; i < EmptyState::kTemplateCount; ++i)
      fresh.push_back(entry(CmdNewTemplate + i, std::wstring(EmptyState::kTemplates[i]), icon::TextDocument));
  for (size_t i = 0; i < userTemplates_.size(); ++i) {
    std::wstring name = fw::FileNameOf(userTemplates_[i]);
    name = name.substr(0, name.find_last_of(L'.'));
    fresh.push_back(entry(CmdUserTemplate + static_cast<int>(i), name, kindGlyph(fw::KindOfPath(userTemplates_[i]))));
  }
  if (docs || fw::EditorShipped(fw::FileKind::Workbook) || fw::EditorShipped(fw::FileKind::Presentation))
    fresh.push_back(entry(CmdUserTemplatesFolder, T(L"More templates…"), icon::FolderOpen));

  // Open: browse, then the recent files.
  auto& open = m.entries[FP::kOpen];
  open.push_back(entry(CmdOpen, T(L"Browse…"), icon::FolderOpen, L"Ctrl+O"));
  if (!settings_.recent.empty() && settings_.showRecent) {
    open.push_back(heading(T(L"Recent")));
    for (int i = 0; i < static_cast<int>(settings_.recent.size()); ++i) {
      const std::wstring& path = settings_.recent[i].path;
      const size_t slash = path.find_last_of(L"\\/");
      open.push_back(entry(CmdRecent + i, fw::FileNameOf(path), kindGlyph(settings_.recent[i].Kind()),
                           slash == std::wstring::npos ? L"" : path.substr(0, slash)));
    }
    open.push_back(command(CmdClearRecent, icon::Delete));
  }

  // The editor's own entries go to their places.
  RibbonModel rm;
  if (ready && !presentation_) v->BuildRibbon(rm, ToolbarContext{settings_.invert, search_.IsOpen(), fullscreen_});
  auto own = [&](RibbonFilePlace place, std::vector<FP::Entry>& to, bool settings) {
    for (const RibbonFileItem& f : rm.file) {
      if (f.place != place || !f.cmd) continue;
      const CommandState s = QueryCommand(f.cmd);
      if (!s.visible) continue;
      FP::Entry e = entry(f.cmd, f.label.empty() ? s.label : f.label, f.glyph, f.description);
      e.enabled = s.enabled;
      e.checked = s.checked;
      e.radio = f.choice;
      // A setting is shown on or off; in Options, anything but "Reviewer name…" (which opens a box).
      e.toggle = f.setting || (settings && !f.choice && !e.label.empty() && e.label.back() != L'…');
      to.push_back(e);
    }
  };

  // Info: this file.
  auto& info = m.entries[FP::kInfo];
  if (ready && v->Kind() == TabKind::Document && !v->Path().empty()) {
    FP::Entry restore = entry(CmdRestoreOriginal, T(L"Restore original"), icon::Undo,
                              T(L"Open the file as it was before Featherweight first saved over it"));
    restore.enabled = fileBackup_;
    info.push_back(restore);
  }
  own(RibbonFilePlace::Info, info, false);

  // Save, Save as, Print and Close run at once.
  m.run[FP::kSave] = CmdSave;
  m.enabled[FP::kSave] = QueryCommand(CmdSave).enabled;
  m.run[FP::kSaveAs] = CmdSaveAs;
  m.enabled[FP::kSaveAs] = ready;
  // Print: the dialog at once, or a Print button over the tab's print settings (PowerPoint's layouts).
  auto& print = m.entries[FP::kPrint];
  own(RibbonFilePlace::Print, print, false);
  m.enabled[FP::kPrint] = QueryCommand(CmdPrint).enabled;
  if (print.empty()) {
    m.run[FP::kPrint] = CmdPrint;
  } else {
    print.insert(print.begin(), {command(CmdPrint, icon::Print, L"Ctrl+P"), heading(T(L"Settings"))});
  }
  m.run[FP::kClose] = CmdCloseTab;

  // Share (F1, f1-pdf): Windows' share sheet, with the file itself or a PDF of it.
  auto& shr = m.entries[FP::kShare];
  shr.push_back(command(CmdShareFile, icon::Share, T(L"The file itself, to mail or another app")));
  shr.push_back(command(CmdSharePdf, icon::PDF, T(L"A PDF of it, to mail or another app")));
  m.enabled[FP::kShare] = ready;

  // Export.
  auto& exp = m.entries[FP::kExport];
  exp.push_back(command(CmdExportPdf, icon::PDF, T(L"The pages as a PDF file, as they look")));
  own(RibbonFilePlace::Export, exp, false);
  // With nothing else to export (documents, workbooks), Export is Export PDF at once, as Print is
  // the dialog at once: File, Export, Enter, as ⋯, Export PDF, Enter was (the friction review, R1).
  if (exp.size() == 1) {
    m.run[FP::kExport] = CmdExportPdf;
    m.enabled[FP::kExport] = QueryCommand(CmdExportPdf).enabled;
  }

  // Options: the app's, then this kind of tab's.
  auto& opt = m.entries[FP::kOptions];
  opt.push_back(heading(T(L"App theme")));
  for (int cmd : {CmdThemeSystem, CmdThemeLight, CmdThemeDark}) {
    FP::Entry e = command(cmd);
    e.radio = true;
    e.label = cmd == CmdThemeSystem ? T(L"Use Windows setting") : cmd == CmdThemeLight ? T(L"Light") : T(L"Dark");
    opt.push_back(e);
  }
  // f1-lang: the interface's language.
  opt.push_back(heading(T(L"Language")));
  for (int cmd = CmdLanguageWindows; cmd < CmdLanguage0 + 9; ++cmd)
    if (QueryCommand(cmd).visible) {
      FP::Entry e = command(cmd);
      e.radio = true;
      opt.push_back(e);
    }
  opt.push_back(heading(T(L"Reading")));
  opt.push_back(command(CmdToggleInvert, 0, {}, true));
  opt.push_back(command(CmdToggleHighlight, 0, {}, true));
  opt.push_back(command(CmdShowRecentFiles, 0, {}, true));
  if (ready && v->Kind() == TabKind::Document) {
    opt.push_back(heading(T(L"Editing")));
    for (int cmd : {CmdToggleSpelling, CmdToggleAutoCorrect, CmdRuler}) opt.push_back(command(cmd, 0, {}, true));
  }
  const size_t before = opt.size();
  opt.push_back(heading(ready ? v->Kind() == TabKind::Workbook       ? T(L"Workbooks")
                                : v->Kind() == TabKind::Presentation ? T(L"Presentations")
                                                                     : T(L"Documents")
                              : T(L"This file")));
  own(RibbonFilePlace::Options, opt, true);
  if (opt.size() == before + 1) opt.pop_back();
  if (QueryCommand(CmdCheckUpdates).visible) {  // f1-ship: inside an MSIX package the Store updates the app
    opt.push_back(heading(T(L"Updates")));
    opt.push_back(command(CmdToggleAutoUpdate, 0, {}, true));
    opt.push_back(command(CmdUpdateBeta, 0, {}, true));
    opt.push_back(command(CmdCheckUpdates, icon::Download));
    if (QueryCommand(CmdRollBackUpdate).visible) opt.push_back(command(CmdRollBackUpdate, icon::Undo));
  }
  opt.push_back(heading(L"Featherweight"));
  if (QueryCommand(CmdScriptEngine).visible) opt.push_back(command(CmdScriptEngine, icon::Download));  // f1-ship
  if (QueryCommand(CmdMakeDefault).visible) opt.push_back(command(CmdMakeDefault, icon::PDF));
  if (fw::pkg::backups::Any(DocFile::Config().backups))
    opt.push_back(entry(CmdDeleteBackups, T(L"Delete backups"), icon::Delete, T(L"The originals kept for Restore original, in every folder")));
  // f1-trust: help and support, opened in the browser (the app sends nothing itself).
  opt.push_back(command(CmdHelp, icon::TextDocument));
  opt.push_back(command(CmdKnownIssues, icon::Warning));
  opt.push_back(command(CmdReportProblem, icon::Comment));
  opt.push_back(command(CmdThirdPartyNotices, icon::Document));
  opt.push_back(command(CmdAbout, icon::Info));
  return m;
}

// ---------------------------------------------------------------------------
// KeyTips

bool App::RibbonAltKey(UINT vk) {
  if (!ribbonShown_ || presentation_ || filePage_.IsOpen()) return false;
  if (!((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) || vk == 'Q') return false;
  if (vk == 'F') {  // Alt+F: the File page, with its letters
    OpenFilePage();
    filePage_.ShowKeyTips(true);
    return true;
  }
  ribbon_.extraTips.clear();
  for (int i = 0; i < title_.QuickCount(); ++i)
    ribbon_.extraTips.push_back({std::to_wstring(i + 1), title_.QuickRect(i), title_.QuickCommand(i)});
  // Alt+H: Home's letters at once; Alt+1: the first quick access button.
  return ribbon_.StartKeyTips(static_cast<wchar_t>(vk));
}

void App::RibbonAltReleased() {
  if (filePage_.IsOpen()) {
    filePage_.ShowKeyTips(!filePage_.KeyTipsShown());
  } else if (ribbon_.KeyTipsShown()) {
    ribbon_.EndKeyTips();
    if (ribbon_.PopupOpen()) ribbon_.ClosePopups();
  } else {
    ribbon_.extraTips.clear();
    for (int i = 0; i < title_.QuickCount(); ++i)
      ribbon_.extraTips.push_back({std::to_wstring(i + 1), title_.QuickRect(i), title_.QuickCommand(i)});
    Layout();  // the letters go where the ribbon is now
    ribbon_.StartKeyTips();
  }
  InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Coverage (test builds: FW_TEST_SCREENS)

#if FW_TEST_SCREENS
std::vector<int> App::ToolbarCommands() {
  std::vector<int> out;
  TabContent* v = Active();
  if (!v || !v->Ready()) return out;
  const ToolbarModel tm = CurrentToolbar();
  auto menuOf = [&](int which) {
    HMENU m = CreatePopupMenu();
    FillWindowMenu(which, m);
    MenuCommands(m, out);
    DestroyMenu(m);
  };
  for (const std::vector<ToolItem>* items : {&tm.left, &tm.right})
    for (const ToolItem& it : *items) {
      if (it.kind == ToolKind::Button && it.dropdown) menuOf(it.cmd);  // what its menu lists
      else if (it.cmd != CmdSaveMenu) out.push_back(it.cmd);
    }
  menuOf(CmdMoreMenu);  // ⋯
  for (int& c : out) c = Canonical(c);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  out.erase(std::remove_if(out.begin(), out.end(), [](int c) { return c <= 0 || c == CmdToolbarOverflow; }), out.end());
  return out;
}

std::vector<int> App::ReachableCommands(std::wstring* report) {
  std::vector<int> ribbon, file, context, search;
  TabContent* v = Active();
  // The ribbon: its items, their menus (the tab's FillMenu where the item has no list of its own)
  // and launchers, in every tab.
  for (const auto& [cmd, where] : ribbon_.Commands()) ribbon.push_back(cmd);
  const RibbonModel model = ribbon_.Model();  // a copy: a menu's FillMenu may lay the window out again
  for (const RibbonTab& t : model.tabs)
    for (const RibbonGroup& g : t.groups)
      for (const RibbonItem& it : g.items)
        if (it.menu.empty() && it.tool.choices.empty() &&
            ((it.tool.kind == ToolKind::Button && it.tool.dropdown) || it.tool.kind == ToolKind::SplitButton)) {
          HMENU m = CreatePopupMenu();
          FillWindowMenu(it.tool.cmd, m);
          MenuCommands(m, ribbon);
          DestroyMenu(m);
        }
  // The File page.
  if (ribbonShown_) {
    const ui::FilePage::Model fm = CurrentFilePage();
    for (int s = 0; s < ui::FilePage::kSectionCount; ++s) {
      if (fm.run[s]) file.push_back(fm.run[s]);
      for (const auto& e : fm.entries[s])
        if (e.cmd) file.push_back(e.cmd);
    }
  }
  // The right-click menu, in the middle of the page.
  if (v && v->Ready()) {
    const RECT doc = DocRect();
    HMENU m = CreatePopupMenu();
    v->FillContextMenu(m, (doc.left + doc.right) / 2, (doc.top + doc.bottom) / 2);
    MenuCommands(m, context);
    DestroyMenu(m);
  }
  for (const CommandEntry& e : SearchableCommands()) search.push_back(e.cmd);
  std::vector<int> all;
  for (const std::vector<int>* list : {&ribbon, &file, &context, &search})
    for (int c : *list) all.push_back(Canonical(c));
  std::sort(all.begin(), all.end());
  all.erase(std::unique(all.begin(), all.end()), all.end());
  if (report)
    *report = L"ribbon " + std::to_wstring(ribbon.size()) + L", File page " + std::to_wstring(file.size()) + L", right-click " +
              std::to_wstring(context.size()) + L", Alt+Q " + std::to_wstring(search.size());
  return all;
}
#endif

// ---------------------------------------------------------------------------
// Script steps (snapshots and tests)

// All of them only drive test snapshots and checks: test builds only (FW_TEST_SCREENS).
bool App::RibbonScriptOp(const std::wstring& op, const std::wstring& arg) {
#if !FW_TEST_SCREENS
  (void)op;
  (void)arg;
#else
  if (op == L"ribbon") {
    // ribbon:<tab name>, ribbon:collapse, ribbon:expand, ribbon:open (a collapsed ribbon's groups shown)
    if (arg == L"collapse" || arg == L"expand") {
      ribbon_.SetCollapsed(arg == L"collapse");
    } else if (arg == L"open") {
      ribbon_.OpenCollapsed();
    } else {
      ribbon_.Select(arg);
    }
    Layout();
    return true;
  }
  if (op == L"keytips") {
    // keytips, or keytips:HFF (Alt, then those letters)
    Layout();
    RibbonAltReleased();
    for (wchar_t ch : arg) {
      const UINT vk = static_cast<UINT>(towupper(ch));
      if (filePage_.IsOpen()) filePage_.OnKeyDown(vk, false);
      else ribbon_.KeyTipKey(vk);
      Layout();
    }
    return true;
  }
  if (op == L"filepage") {
    OpenFilePage();
    for (int s = 0; s < ui::FilePage::kSectionCount; ++s)
      if (fw::ToLower(ui::FilePage::SectionName(static_cast<ui::FilePage::Section>(s))) == fw::ToLower(arg)) {
        filePage_.Open(static_cast<ui::FilePage::Section>(s));
        Layout();
      }
    return true;
  }
  if (op == L"dumpribbon") {
    Layout();
    Report(ribbon_.Dump());
    return true;
  }
  if (op == L"dumpreach") {
    // The commands the toolbar and the ⋯ menu reach now, for a coverage baseline.
    for (int cmd : ToolbarCommands()) Report(L"reach " + std::to_wstring(cmd) + L" " + QueryCommand(cmd).label + L"\n");
    return true;
  }
  if (op == L"ribboncheck") {
    // ribboncheck[:<baseline>]: every item runs a command the tab or the window knows; every tab's
    // groups fit the window and its KeyTips are unique; every command of the baseline (what the
    // toolbar and the ⋯ menu reached) is reachable from the ribbon, the File page, the right-click
    // menu or command search.
    std::vector<std::wstring> problems;
    TabContent* v = Active();
    RibbonModel raw;
    if (v && v->Ready()) v->BuildRibbon(raw, ToolbarContext{settings_.invert, search_.IsOpen(), fullscreen_});
    if (raw.tabs.empty()) problems.push_back(L"the tab has no ribbon");
    int items = 0;
    auto named = [&](int cmd) { return cmd > 0 && !QueryCommand(cmd).label.empty(); };
    for (const RibbonTab& t : raw.tabs)
      for (const RibbonGroup& g : t.groups) {
        const std::wstring where = t.name + L" › " + g.name;
        if (g.items.empty()) problems.push_back(where + L": an empty group");
        if (g.launcher && !named(g.launcher)) problems.push_back(where + L": launcher " + std::to_wstring(g.launcher) + L" has no command");
        for (const RibbonItem& it : g.items) {
          ++items;
          const std::wstring what = where + L": item " + std::to_wstring(it.tool.cmd) + L" (" + it.label + L")";
          if (it.tool.kind == ToolKind::Custom ? !it.tool.control : !named(it.tool.cmd))
            problems.push_back(what + L" runs no command");
          for (int c : it.menu)
            if (c && !named(c)) problems.push_back(what + L": menu command " + std::to_wstring(c) + L" has no name");
          if (it.gallery) {
            if (it.tool.choices.empty() || !it.tool.onChange) problems.push_back(what + L": a gallery without choices");
            for (int c : it.gallery->menu)
              if (c && !named(c)) problems.push_back(what + L": gallery command " + std::to_wstring(c) + L" has no name");
          }
          if (!it.tool.glyph && it.size != RibbonSize::Medium && it.tool.kind == ToolKind::Button)
            problems.push_back(what + L": a button without an icon");
        }
      }
    // Each tab: its groups fit, its KeyTips are unique.
    const std::wstring was = ribbon_.Selected();
    std::vector<std::wstring> tabNames;  // Layout replaces the model: its tabs can't be walked across it
    for (const RibbonTab& t : ribbon_.Model().tabs) tabNames.push_back(t.name);
    for (const std::wstring& name : tabNames) {
      ribbon_.Select(name);
      Layout();
      for (const std::wstring& p : ribbon_.Problems()) problems.push_back(name + L": " + p);
    }
    ribbon_.Select(was);
    Layout();
    // The baseline.
    std::wstring report;
    const std::vector<int> reach = ReachableCommands(&report);
    int baseline = 0;
    if (!arg.empty()) {
      std::string data;
      if (!util::ReadFileBytes(arg, data, 1u << 20)) {
        problems.push_back(L"can't read the baseline " + arg);
      } else {
        for (const std::wstring& line : fw::Split(fw::Utf8ToWide(data), L'\n')) {
          int cmd = 0;
          const std::wstring t = fw::Trim(line);
          if (t.compare(0, 6, L"reach ") != 0 || !fw::ParseInt(t.substr(6, t.find(L' ', 6) - 6), cmd)) continue;
          ++baseline;
          if (!std::binary_search(reach.begin(), reach.end(), Canonical(cmd)))
            problems.push_back(L"not reachable: " + t.substr(6));
        }
      }
    }
    for (const std::wstring& p : problems) Report(L"ribboncheck: " + p + L"\n");
    Report(L"ribboncheck: " + std::to_wstring(items) + L" items in " + std::to_wstring(raw.tabs.size()) + L" tabs; " +
           std::to_wstring(baseline) + L" baseline commands; reachable " + report + L"; " + std::to_wstring(problems.size()) +
           L" problems\n");
    if (!problems.empty()) scriptFailed_ = true;
    return true;
  }
  if (op == L"clipcheck") {
    // clipcheck (F1, f1-lang): the window drawn with each ribbon tab, then each File page section;
    // every label that doesn't fit its box is a problem. With the pseudo-language (--lang qps) only the
    // interface's own texts count (in its brackets): a file's name or a cell's text may be cut.
    std::vector<gfx::ClippedText> clipped;
    std::vector<std::pair<std::wstring, std::wstring>> problems;  // where, what
    const bool pseudo = std::wstring_view(fw::lang::Current()) == L"qps";
    int surfaces = 0;
    auto look = [&](const std::wstring& where) {
      ++surfaces;
      clipped.clear();
      gfx::WatchClipping(&clipped);
      DrawSnapshotFrame();
      gfx::WatchClipping(nullptr);
      for (const gfx::ClippedText& c : clipped) {
        if (pseudo && c.text.find(L'·') == std::wstring::npos) continue;
        const std::wstring what = L"\"" + c.text + L"\" needs " + std::to_wstring(std::lround(c.need)) + L" px, has " +
                                  std::to_wstring(std::lround(c.room));
        if (std::none_of(problems.begin(), problems.end(), [&](const auto& p) { return p.second == what; }))
          problems.push_back({where, what});
      }
    };
    const std::wstring was = ribbon_.Selected();
    std::vector<std::wstring> tabNames;
    for (const RibbonTab& t : ribbon_.Model().tabs) tabNames.push_back(t.name);
    for (const std::wstring& name : tabNames) {
      ribbon_.Select(name);
      Layout();
      look(name);
    }
    ribbon_.Select(was);
    Layout();
    if (ribbonShown_ && !presentation_) {
      OpenFilePage();
      for (int s = 0; s < ui::FilePage::kSectionCount; ++s) {
        filePage_.Open(static_cast<ui::FilePage::Section>(s));
        Layout();
        look(std::wstring(T(L"File")) + L" › " + ui::FilePage::SectionName(static_cast<ui::FilePage::Section>(s)));
      }
      CloseFilePage();
    }
    for (const auto& [where, what] : problems) Report(L"clipcheck: " + where + L": " + what + L"\n");
    // The summary tools/strings/clipping.ps1 reads: the language actually in use, what was drawn, what didn't fit.
    Report(L"clipcheck: language " + std::wstring(fw::lang::Current()) + L", " + std::to_wstring(surfaces) + L" surfaces, " +
           std::to_wstring(problems.size()) + L" clipped\n");
    if (!problems.empty()) scriptFailed_ = true;
    return true;
  }
  if (op == L"ribbonbench") {
    // Switching each tab (the tab's model built, laid out and the ribbon drawn) and laying the
    // ribbon out again at 1366, 1024, 800 and 640 px: within one frame (16 ms), or the run fails.
    // The whole window's frame after a switch is reported too (the page is the tab's, not the
    // ribbon's; with --gpu as on screen). Each tab and width counts its best of five rounds, and
    // the thread's own CPU time is measured beside the clock (its cycles, at the rate the best of a
    // few short spins shows): while other programs keep every core busy (parallel builds, ctest -j)
    // the clock runs on as this thread waits, so a run fails only when both pass 16 ms.
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    auto now = [] {
      LARGE_INTEGER t;
      QueryPerformanceCounter(&t);
      return t.QuadPart;
    };
    const HANDLE thread = GetCurrentThread();
    auto cycles = [&] {
      ULONG64 c = 0;
      QueryThreadCycleTime(thread, &c);
      return c;
    };
    auto wallMs = [&](LONGLONG from, LONGLONG to) { return 1000.0 * static_cast<double>(to - from) / static_cast<double>(freq.QuadPart); };
    double cyclesPerMs = 0;
    for (int k = 0; k < 8; ++k) {  // 2 ms spins: the fastest rate is the one with no waiting in it
      const LONGLONG a = now();
      const ULONG64 c = cycles();
      LONGLONG b;
      do b = now();
      while (wallMs(a, b) < 2);
      cyclesPerMs = std::max(cyclesPerMs, static_cast<double>(cycles() - c) / wallMs(a, b));
    }
    struct Time {
      double wall = 1e9, cpu = 1e9;
    };
    LONGLONG t0 = 0;
    ULONG64 c0 = 0;
    double slowest = 0;
    auto start = [&] {
      t0 = now();
      c0 = cycles();
    };
    auto stop = [&](Time& best) {
      const double wall = wallMs(t0, now()), cpu = cyclesPerMs > 0 ? static_cast<double>(cycles() - c0) / cyclesPerMs : wall;
      best.wall = std::min(best.wall, wall);
      best.cpu = std::min(best.cpu, cpu);
      slowest = std::max(slowest, wall);
    };
    constexpr int kRounds = 5;
    const std::wstring was = ribbon_.Selected();
    std::vector<std::wstring> names;
    for (const RibbonTab& t : ribbon_.Model().tabs) names.push_back(t.name);
    const int widths[] = {1366, 1024, 800, 640};
    std::vector<Time> switches(names.size()), frames(names.size()), layouts(std::size(widths));
    for (int round = 0; round < kRounds; ++round)
      for (size_t n = 0; n < names.size(); ++n) {
        start();
        ribbon_.Select(names[n]);
        Layout();
        dc_->BeginDraw();
        canvas_.Begin(dc_.Get());
        ribbon_.Draw(canvas_);
        ribbon_.DrawPopup(canvas_);
        canvas_.End();
        dc_->EndDraw();
        stop(switches[n]);
        const double keep = slowest;  // the whole frame isn't the ribbon's: not its slowest round
        start();
        DrawSnapshotFrame();
        stop(frames[n]);
        slowest = keep;
      }
    const int top = TitleHeight();
    for (int round = 0; round < kRounds; ++round)
      for (size_t k = 0; k < std::size(widths); ++k) {
        start();
        ribbon_.Layout(RECT{0, top, widths[k], top + ToolbarHeight()}, ui_, CurrentRibbon());
        stop(layouts[k]);
      }
    ribbon_.Select(was);
    Layout();
    auto worst = [](const std::vector<Time>& v) {
      Time w{0, 0};
      for (const Time& t : v) {
        w.wall = std::max(w.wall, t.wall);
        w.cpu = std::max(w.cpu, t.cpu);
      }
      return w;
    };
    const Time sw = worst(switches), lw = worst(layouts), fw = worst(frames);
    fwprintf(stderr,
             L"ribbonbench: %d tabs, switch worst %.2f ms (CPU %.2f), relayout worst %.2f ms (CPU %.2f) (the whole frame "
             L"after a switch: %.2f ms; best of %d rounds each, slowest single round %.2f ms)\n",
             static_cast<int>(names.size()), sw.wall, sw.cpu, lw.wall, lw.cpu, fw.wall, kRounds, slowest);
    if ((sw.wall > 16 && sw.cpu > 16) || (lw.wall > 16 && lw.cpu > 16)) scriptFailed_ = true;
    return true;
  }
#endif
  return false;
}
