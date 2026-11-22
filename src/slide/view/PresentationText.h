#pragma once

#include <windows.h>

#include <d2d1_1.h>

#include <memory>
#include <optional>
#include <string>

#include "TabContent.h"
#include "slide/view/TextSession.h"

struct ToolbarModel;
namespace gfx {
class Canvas;
}
namespace fw::text {
class SpellChecker;
}

// Text editing in the presentation tab (P2, p2-text): the tab's hooks into TextSession. The tab
// owns one TextEditing and passes it clicks (in slide points), keys, characters and commands first
// while text is being edited; it draws the slide without the edited text (Filter) and then calls
// Draw, which draws the text live with its selection and caret. Without a TextModel (no edit layer
// yet) nothing here edits.
namespace fw::slide::edit {
class Editor;
struct Changes;
}  // namespace fw::slide::edit

namespace fw::slide::view {

class Deck;
class SlideSpelling;

class TextEditing {
 public:
  explicit TextEditing(ViewHost& host);
  ~TextEditing();

  // The deck and the edit layer (null: read-only).
  void SetModel(Deck* deck, TextModel* model);
  // The deck's editor (p2-model's edit::Editor; null: read-only): text edits go through it (an
  // EditTextModel), and its change notification keeps the edited text and the slide current
  // (undo, redo, other edits). Call again when the tab loads another deck.
  void Attach(Deck* deck, edit::Editor* editor);
  bool Editing() const { return session_ != nullptr; }
  TextSession* Session() { return session_.get(); }
  // The shape whose text is edited on `slide`, else 0 (the slide's prompts leave it out).
  uint32_t EditingShape(size_t slide) const;
  // Starts editing `t`, the caret at a point (slide points) or at the end.
  bool Begin(const TextTarget& t, std::optional<draw::Point> at = std::nullopt);
  void End();

  // ---- Input (slide points) ----
  // A click on the slide: true when it went into text (the tab then drags with MouseMove).
  bool MouseDown(size_t slide, draw::Point at, bool doubleClick, UINT mods);
  void MouseMove(draw::Point at);
  bool OverText(size_t slide, draw::Point at);  // the I-beam
  bool KeyDown(UINT vk, UINT mods);
  bool Char(wchar_t ch, UINT mods);

  // ---- Drawing ----
  // What the slide's bitmap leaves out while its text is edited (null when nothing).
  render::DrawFilter* Filter(size_t slide);
  // The text live, its selection and caret, over the slide drawn in `slideRect` (client pixels).
  void Draw(gfx::Canvas& c, ID2D1DeviceContext* dc, size_t slide, const D2D1_RECT_F& slideRect);
  // Squiggles under misspelled words on the slide (when spelling as you type is on).
  void DrawSpelling(gfx::Canvas& c, size_t slide, const D2D1_RECT_F& slideRect, bool on);
  std::optional<RECT> CaretRect(const D2D1_RECT_F& slideRect) const;
  // An edit changed the slide around the text (a row or an autofit shape grew): the tab draws the
  // slide again. Read once.
  bool TakeSlideChanged();

  // ---- The model changed (the edit layer's notification, after it read the slide again) ----
  void ModelChanged();

  // ---- Commands ----
  int Shortcut(UINT vk, UINT mods) const;
  bool Owns(int cmd) const;
  CommandState Query(int cmd);
  bool Execute(int cmd);
  void AddToolbar(ToolbarModel& m);
  void FillContextMenu(HMENU menu, size_t slide, draw::Point at);

 private:
  void Changed();          // repaint, status, errors
  void Edited(const edit::Changes& c);  // the editor's notification
  bool StartAt(size_t slide, const TextTarget& t);
  void Copy(bool cut);
  void OpenLinkPanel();  // Ctrl+K
  void Paste(bool textOnly);

  ViewHost& host_;
  Deck* deck_ = nullptr;
  TextModel* model_ = nullptr;
  edit::Editor* editor_ = nullptr;      // Attach's
  std::unique_ptr<TextModel> ownModel_;  // Attach's EditTextModel
  std::unique_ptr<TextSession> session_;
  struct Hide;
  std::unique_ptr<Hide> hide_;
  bool slideChanged_ = false;
  wchar_t highSurrogate_ = 0;
  double lastDoubleClick_ = -1e9;
  draw::Point lastDoubleAt_;
  pugi::xml_document copiedLook_;  // Ctrl+Shift+C
  std::unique_ptr<fw::text::SpellChecker> checker_;
  std::unique_ptr<SlideSpelling> spelling_;
  std::wstring lastColorHex_ = L"C00000";
  std::vector<std::wstring> suggestions_;  // the context menu's
  std::optional<std::pair<TextPos, TextPos>> menuWord_;
  class LinkPanel;
  std::unique_ptr<LinkPanel> linkPanel_;
};

}  // namespace fw::slide::view
