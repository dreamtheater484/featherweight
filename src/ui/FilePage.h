#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "ui/Gfx.h"
#include "ui/Uia.h"

namespace ui {

// The File page (plan §5.6): Office's File tab, over the ribbon and the page. A rail on the left
// (New, Open, Info, Save, Save as, Print, Share, Export, Close, Options) and the chosen section's
// entries on the right. The window fills it from its own commands and the editor's
// (RibbonModel::file); every entry runs a command, as the ☰ and ⋯ menus did.
class FilePage {
 public:
  // Share: F1, f1-pdf (Windows' share sheet), where Word has it.
  enum Section { kNew, kOpen, kInfo, kSave, kSaveAs, kPrint, kShare, kExport, kClose, kOptions, kSectionCount };
  struct Entry {
    int cmd = 0;  // 0: a heading
    std::wstring label;
    std::wstring detail;  // a line under it: a recent file's folder, what a command does
    wchar_t glyph = 0;
    bool enabled = true;
    bool checked = false;
    bool toggle = false;  // a setting shown on or off; the page stays open when it changes
    bool radio = false;   // one of a set (the app theme); stays open too
  };
  struct Model {
    std::wstring title;  // the file's name
    std::wstring path;   // and where it is
    std::vector<Entry> entries[kSectionCount];
    // The sections that run a command at once instead of showing entries (Save, Print, Close).
    int run[kSectionCount] = {};
    bool enabled[kSectionCount] = {true, true, true, true, true, true, true, true, true, true};
  };

  bool IsOpen() const { return open_; }
  void Open(Section s = kInfo);
  void Close() { open_ = false; }
  Section Current() const { return section_; }

  void Layout(const RECT& rc, float ui, Model m);
  void Draw(gfx::Canvas& c);
  bool Hit(int x, int y) const;

  bool OnMouseMove(int x, int y);
  void OnMouseLeave() { hot_ = -1; }
  void OnMouseDown(int x, int y);
  void OnMouseUp(int x, int y);
  bool OnWheel(int delta, int x, int y);
  // Up and Down, Tab between the rail and the entries, Enter, Esc; letters while KeyTips show.
  bool OnKeyDown(UINT vk, bool shift);
  std::wstring TooltipAt(int x, int y) const;
  void ShowKeyTips(bool show) { tips_ = show; }
  bool KeyTipsShown() const { return tips_; }
  void Describe(uia::Tree& t, int parent);

  // Runs an entry's command. A setting's (toggle, radio) keeps the page open; anything else closes
  // it first (onClose).
  std::function<void(int cmd)> onCommand;
  std::function<void()> onClose;

  static const wchar_t* SectionName(Section s);

 private:
  struct Hot {
    int section = -1;  // a rail entry
    int entry = -1;    // or an entry of the current section
    bool back = false;
    D2D1_RECT_F rc{};
  };
  int HotAt(int x, int y) const;
  void Activate(int i);
  void Choose(Section s);

  bool open_ = false;
  bool tips_ = false;
  Section section_ = kInfo;
  Model model_;
  RECT rc_{};
  float ui_ = 1;
  float scroll_ = 0, contentH_ = 0;
  D2D1_RECT_F rail_{}, content_{};
  std::vector<Hot> hots_;
  int hot_ = -1, pressed_ = -1;
  int focus_ = -1;  // the keyboard's place in hots_
};

}  // namespace ui
