#pragma once

#include <windows.h>

#include <d2d1_1.h>

#include <memory>
#include <array>
#include <optional>
#include <string>

#include "TabContent.h"
#include "slide/view/TextSession.h"

struct ToolbarModel;
struct ToolItem;
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
  // The edit layer text goes through (null: read-only): find and replace use it too.
  TextModel* Model() const { return model_; }
  TextSession* Session() { return session_.get(); }
  // The shape whose text is edited on `slide`, else 0 (the slide's prompts leave it out).
  uint32_t EditingShape(size_t slide) const;
  // Starts editing `t`, the caret at a point (slide points) or at the end.
  bool Begin(const TextTarget& t, std::optional<draw::Point> at = std::nullopt);
  void End();
  // Editing the text of shape `shape` on `slide`, the caret at its end; a table: its first cell
  // (after inserting a text box or a table, p2-objects' Host::beginText).
  bool BeginTextEdit(size_t slide, uint32_t shape);
  // Typing with shape `shape` selected (no text being edited): its text, all of it selected so the
  // typing replaces it, as PowerPoint does. False when it has no text body to edit.
  bool TypeInto(size_t slide, uint32_t shape);
  // The block of table cells selected while a cell is edited (a drag across cells, Shift+click in
  // another cell of the table), as {first row, first column, last row, last column}; just the
  // edited cell when no block is selected; nothing when the text edited isn't a table cell.
  std::optional<std::array<int, 4>> CellRange() const;
  // The shape whose text Esc left, once (the tab keeps it selected), else 0.
  uint32_t TakeLeft();
  // Script ops (scripts/build-deck.ps1): edit:title|subtitle|body|body2 (the caret at the end of that
  // placeholder of `slide`), fontsize:<points> and fontcolor:<RRGGBB> (the selection, or the word at
  // the caret). False for ops that aren't text's; one that can't do its job says why on stderr.
  bool ScriptOp(const std::wstring& op, const std::wstring& arg, size_t slide);
  // F7: the next misspelled word after the caret (from the start of `slide` when no text is being
  // edited), through the slides' texts in find's order and round again, selected in its text;
  // `goTo` shows another slide first. False when spelling can't be checked or nothing is misspelled
  // (the tab is told why).
  bool NextMisspelling(size_t slide, const std::function<void(size_t)>& goTo);

  // ---- Input (slide points) ----
  // A click on the slide: true when it went into text (the tab then drags with MouseMove).
  bool MouseDown(size_t slide, draw::Point at, bool doubleClick, UINT mods);
  void MouseMove(draw::Point at);
  void MouseUp();  // the end of a click or drag in text: the format painter's look goes on
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
  // The text commands command search offers (labeled while text is edited).
  static std::vector<int> CommandIds();
  // A command of the text's (CommandIds, the font and size boxes) whatever is edited: Query names
  // it even with no text being edited.
  static bool Names(int cmd);
  CommandState Query(int cmd);
  bool Execute(int cmd);
  // The control for one of the text commands, as the ribbon shows it: Font and size
  // boxes, color split buttons, the buttons with their glyphs.
  ToolItem Tool(int cmd);
  void FillContextMenu(HMENU menu, size_t slide, draw::Point at);

 private:
  void Changed();          // repaint, status, errors
  void Edited(const edit::Changes& c);  // the editor's notification
  bool StartAt(size_t slide, const TextTarget& t);
  void Copy(bool cut);
  void OpenLinkPanel();  // Ctrl+K
  bool NextCell(bool back);  // Tab in a table
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
  uint32_t left_ = 0;  // TakeLeft's
  // A block of cells: from the edited cell (the anchor) to this one; none: the text selection.
  std::optional<std::pair<int, int>> cellTo_;
  bool CellUnder(draw::Point at, int& row, int& column) const;  // the edited table's cell there
  // The edited text's colors: the slide's theme and color map when editing began (StartAt).
  std::shared_ptr<const draw::Theme> theme_;
  draw::ColorMap colorMap_;
  wchar_t highSurrogate_ = 0;
  double lastDoubleClick_ = -1e9;
  draw::Point lastDoubleAt_;
  pugi::xml_document copiedLook_;  // Ctrl+Shift+C, and the format painter's
  bool painter_ = false;           // the format painter is up: the next MouseUp applies copiedLook_
  std::unique_ptr<fw::text::SpellChecker> checker_;
  std::unique_ptr<SlideSpelling> spelling_;
  std::wstring lastColorHex_ = L"C00000";
  std::vector<std::wstring> suggestions_;  // the context menu's
  std::optional<std::pair<TextPos, TextPos>> menuWord_;
  class LinkPanel;
  std::unique_ptr<LinkPanel> linkPanel_;
};

}  // namespace fw::slide::view
