#pragma once

#include <windows.h>
// (WIN32_LEAN_AND_MEAN leaves out the COM declarations UI Automation needs.)
#include <ole2.h>
#include <uiautomation.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ui/Gfx.h"

struct Button;
class TextBox;

// UI Automation for the custom-drawn window, so screen readers and test tools see every tab,
// button, box and list as an element with a role, a name, a state and the actions it supports.
//
// The window describes what it shows as a tree of plain `Node`s. Providers find their element by
// key in the latest tree, so nothing dangles when the UI rebuilds itself, and comparing one tree
// with the next tells which events to raise. Everything runs on the UI thread.
namespace uia {

// Stays the same for an element across rebuilds: made from its parent's key and its own id.
using Key = uint64_t;

struct Node {
  Key key = 0;
  int parent = -1;
  std::vector<int> children;

  CONTROLTYPEID type = UIA_PaneControlTypeId;
  std::wstring name;
  std::wstring accelerator;  // "Ctrl+O"
  std::wstring help;
  std::wstring status;       // ItemStatus: what the element shows that isn't its name
  std::wstring automationId;
  D2D1_RECT_F rc{};          // client pixels; empty for none
  bool enabled = true;
  bool focusable = false;
  bool focused = false;
  bool offscreen = false;
  bool password = false;
  bool live = false;         // a live region: its text is read out when it changes (banners)
  int layer = 0;             // drawn above lower layers (1: floating panels, 2: open lists)
  int orientation = -1;      // OrientationType, or -1 for none
  int positionInSet = 0, sizeOfSet = 0;  // 1-based, for items in a list

  // Patterns. A pattern is supported when its action is set (or, for state, when it isn't -1).
  std::function<void()> invoke;
  int toggle = -1;  // ToggleState
  std::function<void()> doToggle;
  int expand = -1;  // ExpandCollapseState
  std::function<void(bool expand)> doExpand;
  bool hasValue = false;
  std::wstring value;
  std::function<void(const std::wstring&)> setValue;  // read-only when empty
  bool hasRange = false;
  double rangeMin = 0, rangeMax = 0, rangeValue = 0, rangeSmall = 1, rangeLarge = 10;
  std::function<void(double)> setRange;
  bool selectionContainer = false;
  bool selectionRequired = false;
  bool selectable = false;  // SelectionItem; its container is the nearest selection container above it
  bool selected = false;
  std::function<void()> select;
  bool scrollable = false;
  double scrollPercent = UIA_ScrollPatternNoScroll, viewPercent = 100;  // vertical
  std::function<void(double percent)> setScroll;
  std::function<void()> scrollIntoView;
  std::function<void()> focus;
};

class Tree {
 public:
  Tree();
  // Adds a child of `parent`; `id` tells it apart from its siblings. Returns its index.
  // Node references are invalidated by the next Add.
  int Add(int parent, int64_t id, CONTROLTYPEID type, std::wstring name, D2D1_RECT_F rc = {});
  Node& operator[](int i) { return nodes_[i]; }
  const Node& operator[](int i) const { return nodes_[i]; }
  int Size() const { return static_cast<int>(nodes_.size()); }
  int Find(Key key) const;
  // The element with keyboard focus, or -1.
  int Focused() const;
  // The deepest element at a client point; later siblings are on top.
  int HitTest(float x, float y) const;
  // The selection container `i` belongs to, or -1.
  int ContainerOf(int i) const;
  bool HasFocusBelow(int i) const;
  void Clear();

 private:
  std::vector<Node> nodes_;
  std::unordered_map<Key, int> index_;
};

// "Open (Ctrl+O)" -> {"Open", "Ctrl+O"}. Tooltips carry their shortcut in parentheses.
std::pair<std::wstring, std::wstring> SplitShortcut(const std::wstring& tooltip);

// Elements for the shared widgets. Names come from the button's text, else its tooltip.
int AddButton(Tree& t, int parent, int64_t id, const Button& b, std::function<void()> invoke);
// A text box. `focus` gives it the keyboard. Setting its value types the text and applies it
// the way pressing Enter would (or as typing does, for boxes that act as you type).
int AddTextBox(Tree& t, int parent, int64_t id, TextBox& box, const std::wstring& name, std::function<void()> focus);

struct State;
class Element;

// The window's UI Automation root. The window owns one and forwards WM_GETOBJECT to it.
class Root {
 public:
  // Posted to the window to run actions UI Automation asked for (see Post).
  static constexpr UINT kRunMessage = WM_APP + 0x5541;

  Root();
  ~Root();
  // `describe` fills the tree below node 0 (the window itself).
  void Attach(HWND hwnd, std::function<void(Tree&)> describe);
  void Detach();
  LRESULT OnGetObject(WPARAM wp, LPARAM lp);
  // Called after every window message: the UI may have changed. Raises events when anyone listens.
  void AfterMessage(UINT msg);
  void RunPosted();

  // For providers (UI thread only).
  const Tree& Current();
  HWND Hwnd() const { return hwnd_; }
  Element* RootElement() const { return root_; }
  const std::shared_ptr<State>& Shared() const { return state_; }
  // Invokes an element after the current UI Automation call returns: invoking can open menus and
  // dialogs, which must not run inside a cross-process call. The element is looked up again then.
  void PostInvoke(Key key);
  // Runs an action now, then raises the events it caused.
  void Run(std::function<void()> action);

 private:
  struct Seen {
    std::wstring name, value, status;
    double range = 0, scroll = 0;
    int toggle = -1, expand = -1;
    bool selected = false, enabled = true, offscreen = false, live = false;
    uint64_t children = 0;
  };
  void Sync();
  void RaiseEvents(const Tree& t);

  HWND hwnd_ = nullptr;
  std::function<void(Tree&)> describe_;
  Tree tree_;
  bool stale_ = true;
  bool building_ = false;
  std::shared_ptr<State> state_;
  Element* root_ = nullptr;
  std::vector<Key> posted_;
  // What clients were last told.
  std::unordered_map<Key, Seen> seen_;
  Key seenFocus_ = 0;
  bool haveSeen_ = false;
};

}  // namespace uia
