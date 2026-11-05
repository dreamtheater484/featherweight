#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "doc/edit/Editor.h"
#include "doc/edit/Format.h"
#include "doc/model/DateFormat.h"

namespace fw::doc::edit {

// Content controls and legacy form fields (plan §6.2 "Advanced", milestone D4), filled in as
// Word does: text typed over a placeholder, check boxes ticked, items and dates picked, and
// forms protection. Each change is one undo step of the editor.

enum class ControlKind : uint8_t { RichText, PlainText, CheckBox, DropDown, ComboBox, Date, Picture, Other };

struct ListItem {
  std::u16string text;  // w:displayText (the value when there's none)
  std::string value;    // w:value
};

// A content control (w:sdt) as filling it in needs it.
struct ContentControl {
  ElemId sdt = kNoElem;
  ControlKind kind = ControlKind::RichText;
  bool block = false;            // around paragraphs, not inside one
  Range content;                 // its text: inside one paragraph, or its first to last paragraph
  bool placeholder = false;      // its text is its placeholder (w:showingPlcHdr)
  bool contentLocked = false;    // w:lock: its text can't change
  bool controlLocked = false;    // w:lock: it can't be deleted
  bool bound = false;            // w:dataBinding: Word fills it from data in the package
  std::u16string title, tag;     // w:alias, w:tag
  // A check box (w14:checkbox): whether it's ticked, and the symbols for each state.
  bool checked = false;
  char16_t checkedSymbol = u'☒', uncheckedSymbol = u'☐';
  // A drop-down list or combo box.
  std::vector<ListItem> items;
  // A date: its picture (w:dateFormat), language (w:lid) and date (w:fullDate).
  std::u16string dateFormat;
  std::string dateLang;
  std::optional<DateTime> date;

  // Text can be typed in it: rich and plain text, combo boxes and dates (when not locked or bound).
  bool Typable() const;
  // It can be filled in at all: those, and check boxes and drop-down lists.
  bool Fillable() const;
};

// The content controls of a story, in document order (outer ones before those inside them).
std::vector<ContentControl> ContentControls(const Document& doc, uint32_t story);
// The innermost content control whose text holds `at` (its ends included).
std::optional<ContentControl> ControlAt(const Document& doc, Pos at);

// Legacy form fields (FORMTEXT, FORMCHECKBOX, FORMDROPDOWN): a field whose w:fldChar holds w:ffData.
enum class FormFieldKind : uint8_t { Text, CheckBox, DropDown };

struct FormField {
  uint32_t story = kNone;
  FormFieldKind kind = FormFieldKind::Text;
  ElemId begin = kNoElem;  // the w:fldChar that begins it, which holds the w:ffData
  Pos start, end;          // its first character (the begin) and the one after its end
  Range result;            // its result text (empty at the end when it has none)
  bool hasResult = false;  // it has a separator
  bool enabled = true;
  std::u16string name;
  bool checked = false;                // a check box: w:checked, else w:default
  std::vector<std::u16string> entries;  // a drop-down: w:listEntry
  int selected = 0;                    // its w:result, else w:default
  int maxLength = 0;                   // a text field: 0 for no limit
};

std::vector<FormField> FormFields(const Document& doc, uint32_t story);
// The form field whose result holds `at` (its ends included), or that `at` is in.
std::optional<FormField> FormFieldAt(const Document& doc, Pos at);

// Forms protection (w:documentProtection w:edit="forms", enforced): only content controls and
// form fields can be filled in. The editor refuses every other change (Editor, TreeEdit).
bool FormsProtected(const Document& doc);
// Whether [from, to] is text that can be typed in or deleted under forms protection: inside one
// content control that takes typing, or the result of one enabled text form field. `why` says
// what it is otherwise.
// With `typing` false (the editor's own check), check boxes and lists count too: their text
// changes as they're filled in.
bool InFillIn(const Document& doc, Pos from, Pos to, std::u16string* why = nullptr, bool typing = true);
// The next (or previous) control or field to fill in after `at`, as Tab goes in a protected form:
// the range to select. Wraps around; nullopt when there's none.
std::optional<Range> NextFillIn(const Document& doc, Pos at, bool back);

// ---- Filling in. The result's caret is after the new text.
// Text in place of a control's (its placeholder, the item it shows): the control's own formatting,
// without the placeholder style; the control no longer shows its placeholder.
Result FillControl(Editor& ed, const ContentControl& c, std::u16string_view text);
// Its text deleted down to nothing: the placeholder comes back, as in Word.
Result ShowPlaceholder(Editor& ed, const ContentControl& c);
Result SetChecked(Editor& ed, const ContentControl& c, bool checked);
Result PickItem(Editor& ed, const ContentControl& c, size_t index);
Result PickDate(Editor& ed, const ContentControl& c, const DateTime& date);
// The placeholder Word shows in an empty control of this kind ("Choose an item.").
std::u16string_view DefaultPlaceholder(ControlKind kind);

Result SetFormCheckBox(Editor& ed, const FormField& f, bool checked);
Result PickFormEntry(Editor& ed, const FormField& f, size_t index);

// Inserts a content control at `at` (one paragraph of a story), showing its placeholder, as Word's
// Developer tab does: rich text, plain text, check box, drop-down list (with `items`), or date.
Result InsertControl(Editor& ed, Pos at, ControlKind kind, const std::vector<std::u16string>& items = {});

}  // namespace fw::doc::edit
