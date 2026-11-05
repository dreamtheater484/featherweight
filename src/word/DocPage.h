#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "TabContent.h"
#include "doc/edit/Editor.h"
#include "doc/edit/Page.h"
#include "ui/SidePanel.h"
#include "ui/Toolbar.h"

// Page layout in the Word tab (plan §6.2 "Page layout", milestone D3).
//  - The Page setup panel (the side panel, from ⋯ or the Layout menu): paper size, orientation,
//    margins, columns, how the section starts, for the sections the selection is in or the whole
//    document; and the document's hyphenation. Each change applies at once, as one undo step.
//  - The toolbar's Layout menu: orientation, Word's margin presets, one to three columns, page
//    setup, and automatic hyphenation.
//  - Breaks: page (Ctrl+Enter), column (Ctrl+Shift+Enter) and section breaks, in Insert ▾ and
//    command search; Delete section break joins the caret's section to the next.
// A change to the page lays out the pages shown at once and the rest in idle time.
class DocPage {
 public:
  struct Host {
    // The selection: where it started and where the caret is.
    std::function<std::pair<fw::doc::Pos, fw::doc::Pos>()> selection;
    // After an edit that moves the caret (a break): the caret where it says, or the refusal shown.
    std::function<bool(const fw::doc::edit::Result&)> apply;
    // After a change of the page: the pages shown are laid out now, the rest later; the caret
    // stays, or with `caret` goes where the result says. Or the refusal shown: false.
    std::function<bool(const fw::doc::edit::Result&, bool caret)> keep;
    // Opens a side panel (null closes it), or closes it when it's open and `toggle`.
    std::function<void(PanelContent*, bool toggle)> showPanel;
    // The section of the header or footer being edited, or kNone in the body.
    std::function<uint32_t()> headerSection;
  };

  DocPage(fw::doc::edit::Editor& editor, Host host);
  ~DocPage();
  DocPage(const DocPage&) = delete;
  DocPage& operator=(const DocPage&) = delete;

  static bool Owns(int cmd);
  static std::vector<int> CommandIds();
  // Ctrl+Enter and Ctrl+Shift+Enter, or 0.
  static int Shortcut(UINT vk, UINT mods);
  // The breaks Insert ▾ offers, after Picture….
  static const std::vector<int>& InsertCommands();

  CommandState Query(int cmd);
  void Execute(int cmd);
  // The Layout menu, after the formatting groups.
  void AddToolbar(ToolbarModel& m);
  // Whether the side panel shows this one's (to close it when the tab goes).
  bool ShowsIn(const PanelContent* shown) const;

 private:
  class Panel;
  friend class Panel;

  const fw::doc::Document& Doc() const { return editor_.Doc(); }
  // The sections the selection is in (the caret's in a header or a note: the last one).
  std::pair<uint32_t, uint32_t> Sections() const;
  void Margins(int32_t top, int32_t bottom, int32_t left, int32_t right);
  void SectionBreak(fw::doc::SectionStart start);
  void Break(bool column);

  fw::doc::edit::Editor& editor_;
  Host host_;
  std::unique_ptr<Panel> panel_;
};
