#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doc/model/Document.h"
#include "pkg/Status.h"

// fwtool torture: what the random edits go through. The harness (Torture.cpp) generates edits from
// what the document model reads, and an EditTarget applies them with the editor (D2).
namespace fwtool::torture {

enum class EditKind : uint8_t { Type, Delete, Split, Join, Format, Undo, Redo, Replace, Group, Command, Paint, Save };

// One edit, as reproducers keep it: positions are paragraph indices and offsets in the model's
// paragraph text (Document.h) at the moment the edit is applied.
struct Edit {
  EditKind kind = EditKind::Type;
  fw::doc::Pos at, to;   // Type, Split: at. Join: at.para and the next. Delete, Replace, Format, Command, Paint: [at, to), across paragraphs too.
  fw::doc::Pos from;     // Paint: where the formatting is copied from
  std::u16string text;   // Type, Replace
  // Format (the editor's property edits): "bold", "italic", "underline", "strike", "size=<half-points>",
  // "color=<RRGGBB>", "highlight=<name>", "plain" (no bold, italic or underline),
  // "align=<left|center|right|both>", "style=<paragraph style id>". "mark:" before a run format
  // applies it to the paragraph marks.
  // Command (the formatting commands of doc/edit Format.h, Lists.h and Styles.h, as the Word tab
  // runs them): "bullets", "numbering", "multilevel", "nolist", "restart", "continue", "indent",
  // "outdent", "apply=<style name or id>", "modify=<style id>", "update", "toggle=<bold|italic|
  // underline|strike|super|sub>", "grow", "shrink", "grow1", "shrink1", "clearchars", "clearpara",
  // "clearall", "font=<name>", "spacing=<240ths>", "spacebefore", "border=<box|bottom|none>".
  std::string format;
  uint32_t count = 1;    // Undo, Redo: how many steps. Group: how many of the next edits make one undo step.
  std::string target;    // what it aims at, for people: "field PAGE", "bookmark 3", "table cell"
};

// One line per edit, and back: type 12:5 "abc" / delete 12:3 14:7 / replace 12:3 14:7 "abc" /
// split 12:5 / join 12 / format 12:3 12:9 bold / format 12:0 13:0 mark:bold / undo 2 / redo 1 /
// group 3 / command 12:0 14:3 "apply=heading 1" / paint 12:3 12:9 40:2 (from 40:2) / save, each
// optionally followed by "# target". A save in the middle is checked as the last one is, then the
// edits go on from it (Save, then undo back past it, then Save again).
std::string FormatEdit(const Edit& edit);
bool ParseEdit(std::string_view line, Edit& edit);

enum class Applied : uint8_t {
  Done,
  Refused,  // the editor didn't do it (blocked, or not possible there): fine, and counted
};

// What the torture test edits through. Each target owns its document.
class EditTarget {
 public:
  virtual ~EditTarget() = default;
  virtual const char* Name() const = 0;
  virtual const fw::doc::Document& Doc() const = 0;
  // Applies one edit, Undo and Redo included. `why` says why an edit was refused.
  virtual Applied Apply(const Edit& edit, std::string& why) = 0;
  // Saves as the app saves: the bytes of the file it would write.
  virtual fw::pkg::Status Save(std::vector<uint8_t>& out) = 0;
  // After a save that went to the file: undo back to here is clean (DocView::Save).
  virtual void Saved() {}
  std::vector<std::string> written;  // the parts the last Save wrote from the model
};

// Opens a read document for editing with the editor (doc/edit/Editor.h). Built without the
// editor, every edit is refused and Save writes every part the model owns, as read.
std::unique_ptr<EditTarget> OpenTarget(std::unique_ptr<fw::doc::Document> doc, fw::pkg::Status& status);

}  // namespace fwtool::torture
