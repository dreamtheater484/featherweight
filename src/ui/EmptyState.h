#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "Settings.h"
#include "core/FileKinds.h"
#include "core/Lang.h"
#include "ui/Uia.h"
#include "ui/Widgets.h"

// The start page (plan §5.1; redone for 4.0 by p3-start). A header with the logo, the name and
// Open…; New, a card for each editor that has shipped, with the templates under the cards (the
// built-in ones and the user's own); and the recent files in a card, pinned ones first, with a
// filter once the list is long, unless the "Show recent files" setting is off. The column, the type
// and the spacing grow with the window and the DPI; the page scrolls when it doesn't fit. Tab moves
// through Open…, the cards, the templates, the filter and the list; the arrows move within them,
// Enter opens, Delete takes a file off the list, and the menu key (or a right-click) pins, removes
// or shows a file in its folder. Nothing runs while it waits: it draws only when something changes.
class EmptyState {
 public:
  struct Result {
    enum class Kind {
      None, Open, New, Template, UserTemplate, MoreTemplates, OpenRecent, RemoveRecent, PinRecent, RecentMenu,
      ShowRecent, HideRecent, FocusFilter
    } kind = Kind::None;
    // The recent file (an index into Settings::recent), the fw::FileKind to create, or the template:
    // built-in (kTemplates) or the user's own (an index into the list Layout was given).
    int index = -1;
    POINT at{};  // RecentMenu: where the menu opens, in client pixels
  };
  // The built-in templates, in fw::doc::docx::Template's order (after None).
  static constexpr int kTemplateCount = 3;
  static constexpr UiText kTemplates[kTemplateCount] = {NC_(L"Letter", L"template"), L"CV", N_(L"Report")};

  EmptyState();
  // `templates`: the user's own (usertemplates::List). `showRecent`: Settings::showRecent.
  void Layout(const RECT& area, float ui, const std::vector<RecentFile>& recent,
              const std::vector<std::wstring>& templates, bool showRecent);
  void Draw(gfx::Canvas& c, const std::vector<RecentFile>& recent, bool caretVisible);
  bool OnMouseMove(int x, int y);
  void OnMouseLeave();
  void OnMouseDown(int x, int y);
  Result OnMouseUp(int x, int y);
  Result OnRightClick(int x, int y);
  bool OnWheel(int delta);
  // Keys pressed without Ctrl or Alt. Returns false for keys the start page doesn't use.
  bool OnKeyDown(UINT vk, bool shift, Result& result);
  bool OverClickable(int x, int y) const;
  std::wstring TooltipAt(int x, int y, const std::vector<RecentFile>& recent) const;
  // UI Automation: Open…, the cards, the templates, the filter and the recent files. `act` gets
  // what a click would return; `focusFilter` gives the filter the keyboard.
  void Describe(uia::Tree& t, int parent, const std::vector<RecentFile>& recent,
                const std::function<void(Result)>& act, const std::function<void()>& focusFilter);

  // The recent files' filter. The window gives it the keyboard (App::FocusBox); its own scale is
  // the page's.
  TextBox& Filter() { return filter_; }
  bool OverFilter(int x, int y) const;
  float Scale() const { return s_; }
  // Tab or Shift+Tab in the filter: the keyboard goes on from it.
  void LeaveFilter(bool forward);
  // Down in the filter: the list.
  void FocusList();
  // The first file the filter leaves (what Enter in the filter opens), or -1.
  int FirstShown() const { return rows_.empty() ? -1 : rows_.front().recent; }

 private:
  enum class Part { Open, New, Template, UserTemplate, More, ShowHide };
  struct Item {
    Part part = Part::Open;
    int index = -1;  // the file kind, the template
    fw::FileKind kind = fw::FileKind::Unknown;
    wchar_t glyph = 0;
    std::wstring text, sub, keys, tip;  // keys: the shortcut it shows
    D2D1_RECT_F rc{};
    Anim hover, press;
  };
  struct Row {
    int recent = -1;  // index into Settings::recent
    D2D1_RECT_F rc{};
  };
  // Where the keyboard is. Filter: the window's text box has it (App::FocusBox).
  enum class Focus { None, Item, Filter, List };
  struct Stop {
    Focus focus;
    int item;
  };
  // The row's buttons, shown while it's hovered: 1 pin, 2 remove.
  enum { kNoButton = 0, kPinButton = 1, kRemoveButton = 2 };

  void BuildItems(const std::vector<std::wstring>& templates, bool showRecent, size_t recentCount);
  Result ItemResult(const Item& it) const;
  int ItemAt(int x, int y) const;
  int RowAt(int x, int y) const;
  D2D1_RECT_F ButtonRect(int row, int button) const;
  int ButtonAt(int row, int x, int y) const;
  bool InView(int x, int y) const;
  void FocusOn(Focus f, int item);
  void Select(int row);
  std::vector<Stop> Stops() const;
  // To the stop after `from` (-1: the first or the last). True when that's the filter.
  bool Go(const std::vector<Stop>& stops, int from, bool forward);
  // Tab: true when the next stop is the filter.
  bool MoveFocus(bool forward);
  void DrawItem(gfx::Canvas& c, const Item& it, bool focused) const;

  RECT area_{};
  float ui_ = 1, s_ = 1;  // s_: the page's scale, the DPI's (ui_) grown with the window
  std::vector<Item> items_;
  std::wstring itemsKey_;  // what items_ were made from
  D2D1_RECT_F logo_{}, title_{}, subtitle_{}, newLabel_{}, templatesLabel_{}, recentLabel_{}, recentCard_{}, note_{};
  std::wstring noteText_;  // in the recent card when it has no rows, or under the header when hidden
  bool showRecent_ = true;
  bool filterShown_ = false;
  TextBox filter_;
  std::vector<Row> rows_;
  int pinnedRows_ = 0;  // the first rows, which are pinned
  std::vector<Anim> rowHover_;
  int hoverItem_ = -1, pressedItem_ = -1;
  int hoverRow_ = -1, hoverButton_ = kNoButton;
  int pressedRow_ = -1, pressedButton_ = kNoButton;
  float scroll_ = 0, scrollMax_ = 0, contentH_ = 0;
  bool reveal_ = false;  // scroll the keyboard's place into view at the next layout

  // Keyboard focus. The selected file is remembered by path, so it stays put when the list changes.
  Focus focus_ = Focus::None;
  int focusItem_ = -1;
  bool focusVisible_ = false;
  int selected_ = -1;
  std::wstring selectedPath_;
  std::vector<std::wstring> paths_;  // the files the rows show, for keeping the selection
};
