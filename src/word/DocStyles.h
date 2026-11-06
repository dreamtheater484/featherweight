#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "TabContent.h"
#include "doc/edit/Editor.h"
#include "doc/edit/Format.h"
#include "doc/model/Props.h"
#include "ui/SidePanel.h"

// Tab stops and styles in the Word tab (D3 audit):
//  - the ruler's tab stop menu (a right-click on a stop): its kind and leader, clear it or all;
//  - the Tabs panel: the paragraph's stops, set at a position with a kind and a leader, or cleared;
//  - the Style panel: a style's font and paragraph settings changed directly (Modify style…), or a
//    new style made from the selection (New style…), with what the panel shows.
// Each change is one undo step of the tab's editor.
class DocStyles {
 public:
  struct Host {
    // The selection: where it started and where the caret is.
    std::function<std::pair<fw::doc::Pos, fw::doc::Pos>()> selection;
    // After a change: the pages laid out again, and a refusal or note shown when there's one.
    std::function<void(const std::wstring& message)> done;
    // Opens a side panel (null closes it), or closes it when it's open and `toggle`.
    std::function<void(PanelContent*, bool toggle)> showPanel;
  };

  DocStyles(fw::doc::edit::Editor& editor, Host host);
  ~DocStyles();
  DocStyles(const DocStyles&) = delete;
  DocStyles& operator=(const DocStyles&) = delete;

  static bool Owns(int cmd);
  static std::vector<int> CommandIds();
  CommandState Query(int cmd);
  void Execute(int cmd);
  // Whether the side panel shows one of these (to close it when the tab goes).
  bool ShowsIn(const PanelContent* shown) const;

  // The ruler's menu, opened on the tab stop `stop` (none: on the ruler beside the stops). The
  // stop's commands act on it.
  void FillTabMenu(HMENU menu, std::optional<fw::doc::TabStop> stop);

 private:
  class TabsPanel;
  class StylePanel;
  friend class TabsPanel;
  friend class StylePanel;

  const fw::doc::Document& Doc() const { return editor_.Doc(); }
  fw::doc::edit::Range Selection() const;
  // The tab stops of the caret's paragraph (not cleared ones).
  std::vector<fw::doc::TabStop> Stops() const;
  // Changes the selection's paragraphs' stops (edit::ChangeTabStops).
  void ChangeTabs(const std::vector<fw::Twips>& remove, const std::vector<fw::doc::TabStop>& add);
  void OpenStylePanel(bool create);

  fw::doc::edit::Editor& editor_;
  Host host_;
  std::optional<fw::doc::TabStop> menuTab_;
  std::unique_ptr<TabsPanel> tabs_;
  std::unique_ptr<StylePanel> style_;
};
