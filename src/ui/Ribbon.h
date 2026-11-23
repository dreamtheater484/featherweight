#pragma once

#include <windows.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "TabContent.h"  // CommandState
#include "ui/Controls.h"
#include "ui/RibbonModel.h"
#include "ui/Uia.h"

namespace ui {

// The editors' ribbon (plan §5.6, decision 27): the File tab and the active tab's own tabs, their
// groups with large, medium and small buttons, boxes, galleries and ↘ dialog launchers, as Office
// places them. It shows a RibbonModel (ui/RibbonModel.h) and runs the same commands as the
// shortcuts, the right-click menus and command search.
//
// As the window narrows, groups shrink a step at a time (large buttons to medium, medium to small,
// galleries to fewer columns and then a button) and at last fold into one button that opens the
// group in a popup. Ctrl+F1 leaves only the tab names; a tab then opens its groups over the page.
// Alt shows KeyTips: a tab's letter, then an item's, runs it. Buttons the ribbon draws itself; the
// other kinds are the toolbar's controls (Toolbar::ConfigureControl).
class Ribbon {
 public:
  static constexpr float kTabsH = 30;  // DIPs: the tab names
  static constexpr float kBodyH = 94;  // DIPs: the groups, with their names

  Ribbon();
  Ribbon(const Ribbon&) = delete;
  Ribbon& operator=(const Ribbon&) = delete;
  void SetWindow(HWND hwnd) { group_.hwnd = hwnd; }

  // The window's command table: names, shortcuts and state.
  std::function<CommandState(int)> query;
  // Runs a command: a button, a launcher, a split button's main part, File (CmdFilePage), the
  // collapse button (CmdRibbonCollapse).
  std::function<void(int)> onCommand;
  // Opens a menu button's menu under `anchor`: `item.menu`'s commands, the tab's FillMenu, or the
  // item's choices (calling its onChange).
  std::function<void(const RibbonItem& item, const D2D1_RECT_F& anchor)> onMenu;

  // Only the tab names (Ctrl+F1).
  bool Collapsed() const { return collapsed_; }
  void SetCollapsed(bool collapsed);
  // Collapsed: the selected tab's groups shown over the page, as a click on the tab does.
  void OpenCollapsed() { overlay_ = collapsed_; }
  // Its height in pixels at `ui`: the tab names, and the groups unless collapsed.
  static int HeightFor(bool collapsed, float ui);
  int Height() const { return HeightFor(collapsed_, ui_); }

  // Shows `m` (the active tab's ribbon) in `rc`. The model is kept until the next Layout; its
  // callbacks belong to the tab, so Forget comes before the tab goes.
  void Layout(const RECT& rc, float ui, RibbonModel m);
  void Draw(gfx::Canvas& c);
  // Drawn above everything: the groups over the page when collapsed, a folded group's popup, an
  // open gallery, the controls' lists and the KeyTips.
  void DrawPopup(gfx::Canvas& c);
  const RibbonModel& Model() const { return model_; }
  bool Empty() const { return model_.tabs.empty(); }

  bool Hit(int x, int y) const;       // the ribbon or one of its popups
  bool HitPopup(int x, int y) const;  // one of its popups (over the page)
  bool PopupOpen() const;
  void ClosePopups();

  bool OnMouseMove(int x, int y);
  void OnMouseLeave();
  void OnMouseDown(int x, int y, bool dbl, bool shift);
  void OnMouseUp(int x, int y);
  bool OnWheel(int delta, int x, int y);
  std::wstring TooltipAt(int x, int y) const;
  bool OverText(int x, int y) const;
  // Where the item running `cmd` shows (for its menu), or an empty rectangle.
  D2D1_RECT_F ItemRect(int cmd) const;

  // The keyboard on one of the ribbon's boxes or lists (after a click or a KeyTip).
  bool HasKeyFocus() const { return group_.Focused() != nullptr; }
  // Keys go to the ribbon: a focused box, or an open gallery's grid.
  bool WantsKeys() const { return group_.Focused() != nullptr || gridGroup_ >= 0; }
  void OnKeyDown(UINT vk, bool ctrl, bool shift, bool alt);
  void OnChar(wchar_t ch) { group_.OnChar(ch); }
  void Blur() { group_.Blur(); }
  // Lets go of the tab's controls (ToolKind::Custom) and callbacks, before the tab goes.
  void Forget();

  // The selected tab, by name.
  const std::wstring& Selected() const { return selected_; }
  void Select(const std::wstring& name);

  // KeyTips. `extra` are letters for the window's own buttons (quick access: 1, 2, 3).
  struct ExtraTip {
    std::wstring keys;
    D2D1_RECT_F at;
    int cmd;
  };
  std::vector<ExtraTip> extraTips;
  bool KeyTipsShown() const { return tips_ != Tips::None; }
  // Alt pressed and let go: the tabs' letters. With `first` (Alt+H): that letter typed at once;
  // false when it's no tab's (or quick access's), and nothing shows.
  bool StartKeyTips(wchar_t first = 0);
  void EndKeyTips();
  // A key while they show: true when it was used. Letters pick; Esc goes back a level.
  bool KeyTipKey(UINT vk);

  // UI Automation: the tabs and the selected tab's items, under `parent`.
  void Describe(uia::Tree& t, int parent);

  // For tests and the coverage check: each tab, group and item with its place, size and KeyTip,
  // as the last layout had them ("tab Home H", "group Font 0", "item 1000 Bold small 1 x,y,w,h").
  std::wstring Dump();
  // Every command the model reaches, with where ("Home › Font"): items, their menus, launchers.
  std::vector<std::pair<int, std::wstring>> Commands() const;
  // For the checks: groups that don't fit the window (beyond its right edge), and KeyTips that are
  // the same as another or the start of one, in the selected tab.
  std::vector<std::wstring> Problems() const;
  // The time the last layout took, in ms (the 16 ms budget).
  double LastLayoutMs() const { return layoutMs_; }

 private:
  enum class PartKind {
    Tab, File, Item, ItemArrow, Control, Launcher, GroupButton, GalleryChoice, GalleryUp, GalleryDown,
    GalleryMore, Collapse, GridChoice, GridMenu
  };
  enum class Layer { Body, Overlay, Popup };  // the body, the groups over the page (collapsed), a folded group or a grid
  struct Part {
    PartKind kind;
    D2D1_RECT_F rc;
    int group = -1, item = -1, index = -1;  // index: a tab, a choice, a grid menu row
    RibbonSize size = RibbonSize::Small;
    Layer layer = Layer::Body;
    Control* control = nullptr;
  };
  enum class Tips { None, Tabs, Items, Group };

  const RibbonTab* Tab() const;
  RibbonItem* ItemAt(int g, int i);
  const RibbonItem* ItemAt(int g, int i) const;
  CommandState Query(int cmd) const;
  std::wstring LabelOf(const RibbonItem& it) const;
  float TextW(const std::wstring& s, float dip, bool semibold = false) const;

  // Layout of one group at a stage (0: as given ... 3: galleries as buttons), from `x` at body
  // top `y`; returns its width. Places its parts when `place`.
  float FlowGroup(int g, int stage, float x, float y, bool place, Layer layer);
  RibbonSize SizeAt(const RibbonItem& it, int stage) const;
  int GalleryColumns(const RibbonItem& it, int stage) const;
  float ItemWidth(const RibbonItem& it, RibbonSize size, int galleryColumns) const;
  float GroupButtonWidth(int g) const;
  void PlaceTabs();
  void PlaceBody(float top, Layer layer);
  void PlaceGroupPopup();
  void PlaceGrid();
  void AssignKeyTips();

  void DrawPart(gfx::Canvas& c, const Part& p, size_t index);
  void DrawBodyFrame(gfx::Canvas& c, float top, Layer layer);
  void DrawGrid(gfx::Canvas& c);
  void DrawKeyTips(gfx::Canvas& c);
  int PartAt(float x, float y) const;
  // What a click or a KeyTip on part `i` does.
  void Activate(size_t i, bool byKeyboard);
  void OpenGroupPopup(int g);
  void OpenGrid(int g, int i, bool byKeyboard);
  void Pick(int g, int i, int choice);
  std::wstring TipOf(const Part& p) const;

  RibbonModel model_;
  RECT rc_{};
  float ui_ = 1;
  bool collapsed_ = false;
  bool overlay_ = false;  // collapsed, and a tab's groups shown over the page
  std::wstring selected_, lastNormal_, lastShow_;
  int tab_ = -1;
  std::vector<int> stage_;  // per group of the selected tab
  std::vector<D2D1_RECT_F> groupRc_;
  float bodyTop_ = 0;
  D2D1_RECT_F overlayRc_{};
  int popupGroup_ = -1;
  D2D1_RECT_F popupRc_{};
  // The open gallery grid.
  int gridGroup_ = -1, gridItem_ = -1, gridHot_ = -1, gridScroll_ = 0, gridCols_ = 1, gridRows_ = 0;
  D2D1_RECT_F gridRc_{};
  std::map<int, int> galleryFirst_;  // a strip's first choice, by command

  std::vector<Part> parts_;
  int hot_ = -1, pressed_ = -1;

  ControlGroup group_;  // before the controls, so it outlives them
  std::map<int, std::unique_ptr<Control>> controls_;
  std::map<int, ToolKind> kinds_;
  std::vector<Control*> shown_;

  // KeyTips: each part's letters, the tabs', and what's typed so far.
  Tips tips_ = Tips::None;
  std::wstring typed_;
  std::map<size_t, std::wstring> partTips_;
  std::vector<std::wstring> tabTips_;
  std::map<std::pair<int, int>, std::wstring> itemTips_;  // (group, item), and (group, -1) for a folded group,
                                                          // (group, -2) for its launcher

  mutable std::map<std::wstring, float> widths_;  // DIPs, at widthsUi_
  mutable float widthsUi_ = 0;
  double layoutMs_ = 0;
};

}  // namespace ui
