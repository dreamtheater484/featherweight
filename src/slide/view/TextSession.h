#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "draw/Painter.h"
#include "slide/render/SlideRender.h"
#include "slide/render/TextBody.h"
#include "slide/view/TextDoc.h"
#include "slide/view/TextModel.h"

namespace fw::text {
class Fonts;
}

// Typing and selecting in one text body on a slide (P2, p2-text; plan §8.2): the caret, the
// selection, the keys that move them, the mouse, typing (one undo step per burst), and autofit
// after each edit. Headless: the tab draws what it says (TextBlock, the caret and selection in
// slide points) and passes it keys and clicks.
//
// Each edit changes the working copy (TextDoc), lays the body out again (nothing else on the
// slide), and puts the copy into the model through TextModel::SetText.
namespace fw::slide::view {

using TextFrame = render::SlideRender::TextFrame;

class TextSession {
 public:
  TextSession(TextModel& model, TextTarget target, text::Fonts& fonts);
  ~TextSession();

  // Reads the body and lays it out where `frame` says. False when the target has no text there.
  bool Start(const TextFrame& frame, const render::TextExtras& extras);
  // The text's place changed (the slide was laid out again): lay out in the new one.
  void SetFrame(const TextFrame& frame);
  // The model changed under the session (undo, redo, another edit): read it again, keeping the
  // caret where it can be.
  void ModelChanged();

  const TextTarget& Target() const { return target_; }
  TextDoc& Doc() { return *doc_; }
  const TextDoc& Doc() const { return *doc_; }
  const render::TextBlock& Block() const { return block_; }
  const TextFrame& Frame() const { return frame_; }
  // Text coordinates -> slide points.
  draw::Matrix TextToSlide() const;
  // Set when an edit changed what the rest of the slide looks like (a table row or an autofit shape
  // grew): the tab draws the slide again and calls SetFrame.
  bool TakeGeometryChanged();
  // Why the last edit didn't happen (the edit layer refused it), once.
  std::wstring TakeError();
  // True while the session's own edit goes into the model: the tab's change notification then
  // needn't call ModelChanged.
  bool Committing() const { return committing_; }

  // ---- Caret and selection ----
  TextPos Caret() const { return caret_; }
  TextPos Anchor() const { return anchor_; }
  bool HasSelection() const { return caret_ != anchor_; }
  TextPos SelFrom() const { return caret_ < anchor_ ? caret_ : anchor_; }
  TextPos SelTo() const { return caret_ < anchor_ ? anchor_ : caret_; }
  void Select(TextPos anchor, TextPos caret);
  void SelectAll();
  enum class Move { Left, Right, WordLeft, WordRight, Up, Down, LineStart, LineEnd, ParaUp, ParaDown, Start, End };
  void MoveCaret(Move m, bool extend);
  // The word around a position (double-click), the paragraph (triple-click).
  void WordAt(TextPos p, TextPos& from, TextPos& to) const;

  // ---- Mouse, slide points ----
  // Whether a point is in the text's box (where a click edits text).
  bool Contains(draw::Point slide) const;
  // A click: 1 places the caret, 2 selects a word, 3 the paragraph; `extend` (Shift) extends.
  void PointerDown(draw::Point slide, int clicks, bool extend);
  void PointerDrag(draw::Point slide);
  TextPos PosAt(draw::Point slide) const;

  // ---- Editing. Each returns whether the text changed. ----
  bool Type(std::wstring_view text);               // replaces the selection; typing coalesces
  bool Backspace(bool word);
  bool DeleteForward(bool word);
  bool Enter();                                    // a new paragraph
  bool LineBreak();                                // Shift+Enter
  // Tab: at the start of a paragraph (or over several) the level goes down a step (Shift: up), as
  // PowerPoint's lists do; elsewhere a tab character.
  bool Tab(bool shift);
  // Replaces the selection with text (paste as text), one undo step named `step`.
  bool ReplaceSelection(std::wstring_view text, std::u16string_view step);
  // Replaces the selection with paragraphs (pasting Featherweight's own copy).
  bool ReplaceWithParagraphs(const std::vector<pugi::xml_node>& paragraphs, std::u16string_view step);
  // Character formatting over the selection, or with none, the word at the caret (as PowerPoint
  // does) or what's typed next. `edit` gets each a:rPr and what that text looks like now.
  bool FormatRuns(std::u16string_view step,
                  const std::function<void(TextDoc&, pugi::xml_node rPr, const RunProps& now)>& edit);
  // What the text at a position looks like (the run typing there joins, else the mark).
  RunProps LookAt(TextPos p) const;
  // Paragraph formatting over the paragraphs the selection touches.
  bool FormatParagraphs(std::u16string_view step, const std::function<void(TextDoc&, pugi::xml_node pPr)>& edit);
  // Any other edit on the working copy, then committed as one step; `edit` returns whether it changed.
  bool Edit(std::u16string_view step, const std::function<bool(TextDoc&)>& edit);
  // What typing takes next: the props set with no selection (Ctrl+B with the caret alone), else
  // null (the text there).
  pugi::xml_node PendingProps() const { return pending_ ? pending_.first_child() : pugi::xml_node(); }

  // ---- What the tab draws ----
  struct CaretLine {
    draw::Point top, bottom;  // slide points
  };
  CaretLine CaretGeometry() const;
  // The selection, as quadrilaterals in slide points (rotated text gives rotated ones).
  std::vector<std::array<draw::Point, 4>> SelectionQuads() const;
  // The last edit's layout time, ms (the keystroke budget, plan §11).
  double LastLayoutMs() const { return layoutMs_; }

 private:
  void Relayout();
  void TakeFrame(const TextFrame& frame);
  // After an edit: autofit, layout, the model. `coalesce` folds typing into one undo step.
  bool Commit(std::u16string_view step, bool coalesce);
  void Fit();                      // autofit after an edit (shrink text, or resize the shape)
  void SetCaret(TextPos p, bool extend, bool lineEnd = false);
  TextPos Step(TextPos p, bool forward) const;
  TextPos StepWord(TextPos p, bool forward) const;
  void EndTyping() { typing_ = false; }

  TextModel& model_;
  TextTarget target_;
  text::Fonts& fonts_;
  std::unique_ptr<TextDoc> doc_;
  TextFrame frame_;
  pugi::xml_document props_;  // frame_'s bodyPr, copied (frame_.box.properties points here)
  render::TextExtras extras_;
  render::TextBlock block_;
  float fontScale_ = 1, lineReduction_ = 0;  // autofit in effect (the file's until the text changes)
  TextPos caret_, anchor_;
  bool lineEnd_ = false;        // the caret shows at the end of a wrapped line
  std::optional<float> goalX_;  // Up and Down keep to this x
  bool typing_ = false;         // the last edit was typing: the next folds into its undo step
  bool geometryChanged_ = false;
  bool committing_ = false;
  std::wstring error_;
  pugi::xml_document pending_;  // props for what's typed next (its one child), or empty
  TextPos pendingAt_;
  int clickMode_ = 1;           // dragging after a double-click extends by words
  TextPos clickFrom_, clickTo_;
  double layoutMs_ = 0;
};

// The text body under a point of a slide (slide points), topmost first: a shape with text, an empty
// placeholder, or a table cell. Shapes from the layout and master don't count.
std::optional<TextTarget> TextTargetAt(render::SlideRender& slide, size_t index, bool notes, draw::Point point);

}  // namespace fw::slide::view
