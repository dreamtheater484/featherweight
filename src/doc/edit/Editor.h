#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "doc/model/Document.h"

// Editing a Document (plan §4.3 rule 4, milestone D2). Headless: no window, no layout.
//
// Every change is a transaction that replaces a run of sibling paragraphs (one paragraph when
// typing; two after a split; one after a join). The editor records each transaction's paragraphs
// before and after it, element subtrees and round-trip fragments included, so undo and redo put the
// document back exactly. Formatting commands, paste and replace are transactions too, grouped with
// BeginGroup/EndGroup into one undo step.
//
// Never damage a file (plan §3): an edit either keeps the document valid (fields, bookmarks,
// comments, content controls, tracked changes and kept unknown content split and join correctly)
// or is refused with a plain message and changes nothing.
//
// Positions are doc::Pos {para, offset}: a paragraph index (Document::Paragraphs()) and a UTF-16
// offset into its text, the same as layout::TextPos. Paragraph indices stay in document order: a
// transaction that adds or removes paragraphs renumbers the ones after it, and its Change says so.
namespace fw::doc::edit {

struct EditorState;  // Editor.cpp

// What a transaction, undo or redo changed. Paragraphs [first, first + removed) were replaced by
// [first, first + added); later paragraphs moved by added - removed. Layout lays out again from the
// page `first` is on.
struct Change {
  uint32_t story = kNone;
  uint32_t first = kNone;
  uint32_t removed = 0, added = 0;
  bool structure = false;  // fields, lists or sections may read differently after it
  int32_t Shift() const { return static_cast<int32_t>(added) - static_cast<int32_t>(removed); }
};

// The outcome of an edit. On failure nothing changed and `message` says why, in plain language.
struct Result {
  bool ok = true;
  std::u16string message;
  Pos caret;  // where the caret goes after the edit
  explicit operator bool() const { return ok; }
};

// A selection: the anchor stays where it started, the caret moves. Empty when they are equal.
struct Selection {
  Pos anchor, caret;
  bool Empty() const { return anchor == caret; }
};

// Properties to set or remove in a run's w:rPr or a paragraph's w:pPr, by element name as the
// schema spells it ("b", "sz", "color", "rFonts", "pStyle", "jc", "spacing", "numPr/numId").
// An item with no attributes and remove false sets an on/off property on (<w:b/>). Attributes
// replace the ones named and keep the others.
struct PropEdit {
  struct Item {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;  // {"val", "24"}, {"w14:paraId", ...}
    bool remove = false;
  };
  std::vector<Item> items;
  PropEdit& Set(std::string name, std::vector<std::pair<std::string, std::string>> attrs = {}) {
    items.push_back({std::move(name), std::move(attrs), false});
    return *this;
  }
  PropEdit& Remove(std::string name) {
    items.push_back({std::move(name), {}, true});
    return *this;
  }
};

// A change made to the model directly rather than as a run of paragraphs: formatting, styles and
// lists (TreeEdit.h). It has been applied when it's recorded, and undoes and redoes itself.
struct CustomStep {
  virtual ~CustomStep() = default;
  virtual void Undo() = 0;
  virtual void Redo() = 0;
  std::vector<uint32_t> parts;  // the parts it changes (indexes into Document::Parts())
  // The paragraphs whose layout it changes, all of one story (their neighbours are laid out again
  // too). kNone: anything may read differently, and the whole document is laid out again.
  uint32_t first = kNone, count = 0;
};

class Editor {
 public:
  explicit Editor(Document& doc);
  ~Editor();
  Editor(const Editor&) = delete;
  Editor& operator=(const Editor&) = delete;

  const Document& Doc() const { return doc_; }
  // False, with the reason, when the document is shown but not edited (Document::Editable(), a part
  // the writer can't reproduce).
  bool CanEdit(std::u16string* why = nullptr) const;

  // ---- Positions
  // -1, 0 or 1 by document order. Positions in different stories compare by story.
  int Compare(Pos a, Pos b) const;
  // The paragraph before or after `para` in its story's reading order (table cells included);
  // kNone at either end.
  uint32_t PrevPara(uint32_t para) const;
  uint32_t NextPara(uint32_t para) const;
  uint32_t ParaLength(uint32_t para) const { return static_cast<uint32_t>(doc_.Para(para).text.size()); }

  // ---- Transactions. Each is one undo step unless inside a group, and changes nothing on failure.
  // Inserts text at `at`. A tab becomes w:tab and U+000B a line break (w:br); paragraph breaks
  // ("\r", "\n", "\r\n") split the paragraph. The text takes the formatting of the text before it.
  Result InsertText(Pos at, std::u16string_view text);
  // Deletes [from, to). Both ends in one story; across paragraphs they join.
  Result Delete(Pos from, Pos to);
  // Typing over a selection: deletes it, then inserts.
  Result Replace(Pos from, Pos to, std::u16string_view text);
  // Splits the paragraph at `at` (Enter). The new paragraph after it gets the same properties.
  Result SplitParagraph(Pos at);
  // Joins `para` with the paragraph after it (Delete at the end, Backspace at the start of the next).
  Result JoinParagraphs(uint32_t para);
  // Sets run properties on the text in [from, to), splitting runs at the ends.
  Result SetRunProps(Pos from, Pos to, const PropEdit& edit);
  // Sets the paragraph mark's run properties (w:pPr/w:rPr) of each paragraph in [first, last].
  Result SetMarkProps(uint32_t first, uint32_t last, const PropEdit& edit);
  // Sets paragraph properties on each paragraph from `first` to `last` in document order.
  Result SetParaProps(uint32_t first, uint32_t last, const PropEdit& edit);
  // Inserts a drawing at `at` (one character, U+FFFC): a w:drawing holding `xml`, a self-contained
  // wp:inline or wp:anchor, in the run of the text before it. `info` is its handle as the reader
  // would make it (ReadDrawingFrame); its node, part and position are filled in here. The handle
  // stays in Document::Drawings() when the insert is undone, at no position. Pictures.h makes the
  // XML and adds the picture's part.
  Result InsertDrawing(Pos at, std::string xml, const Drawing& info);
  // Makes the text in [from, to), in one paragraph, a hyperlink to `target`: a web address (a
  // relationship of the part, kept if the link is undone, which is harmless) or "#bookmark".
  // Refused where the text is already a link, or in a field, a content control or a tracked change.
  Result SetLink(Pos from, Pos to, std::string_view target);

  // Groups the transactions until EndGroup into one undo step named `name` ("Bold", "Paste").
  // Groups nest; the outermost names the step.
  void BeginGroup(std::u16string name);
  void EndGroup();
  // Ends the group BeginGroup opened last, taking back what was done since, newest first, and
  // leaving no undo or redo step for it: a change made of several that fails part way.
  void CancelGroup();
  // Typing and deleting a character at a time coalesce into one step while they continue where the
  // last one ended. The view breaks the run when the caret moves on its own or time passes.
  void BreakTyping();

  // ---- Direct changes (TreeEdit.h). RecordStep adds an applied step to the history, as one undo
  // step or as part of the open group, rebuilds the model's indexes and tells the listeners that
  // every paragraph may look different. New elements take their node ids from NewNodeId, the
  // transactions' own counter.
  void RecordStep(std::u16string name, std::shared_ptr<CustomStep> step, Pos caret = {});
  pkg::NodeId NewNodeId();

  // ---- Undo and redo
  bool CanUndo() const;
  bool CanRedo() const;
  std::u16string UndoName() const;  // "Typing", "Bold", ...
  std::u16string RedoName() const;
  Result Undo();  // caret: where the undone change was
  Result Redo();

  // ---- Notifications and state
  using Listener = std::function<void(const Change&)>;
  // Called after every transaction, undo and redo, once per replaced paragraph run.
  void OnChange(Listener listener) { listeners_.push_back(std::move(listener)); }
  // Changed since opening, or since MarkSaved(). Undoing back to the saved state clears it.
  bool Modified() const;
  void MarkSaved();
  // The parts changed since opening (indexes into Document::Parts()); a save writes only these.
  std::vector<uint32_t> DirtyParts() const;

 private:
  Document& doc_;
  std::unique_ptr<EditorState> impl_;
  std::vector<Listener> listeners_;
};

// True when every character of `utf8` can be written in XML 1.0: valid UTF-8, without control
// characters (tab, line feed and carriage return aside), lone surrogates, U+FFFE or U+FFFF. A
// property value or link target with others would make the saved file unreadable.
bool XmlSafe(std::string_view utf8);

}  // namespace fw::doc::edit
