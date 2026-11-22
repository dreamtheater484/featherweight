#pragma once

#include <optional>
#include <string>

#include "draw/Painter.h"
#include "slide/Text.h"
#include "slide/view/TextSession.h"

// Formatting text on slides (P2, p2-text; plan §8.2 "Text"): what fwtext lays out, set as PowerPoint
// writes it (a:rPr and a:pPr), each one undo step. Character formatting goes over the selection, the
// word at the caret, or what's typed next; paragraph formatting over the paragraphs the selection
// touches. Bullets and numbering keep to the levels of the master's text styles.
namespace fw::slide::view {

// What the selection looks like, for the toolbar: an empty optional where it's mixed.
struct TextLook {
  std::optional<bool> bold, italic, underline, strike;
  int baseline = 0;  // 1 superscript, -1 subscript (the first character's)
  std::optional<float> size;          // points, as written (before autofit's scale)
  std::optional<std::wstring> font;   // the Latin typeface
  std::optional<draw::Color> color;
  std::optional<TextAlign> align;
  float lineSpacing = 1;              // the first paragraph's, a multiple (0 when in points)
  bool bullets = false, numbering = false;  // the first paragraph's
  int level = 0;
  bool link = false;                  // a hyperlink in the selection
};
TextLook LookOf(const TextSession& s);

namespace format {

// Character formatting.
bool Bold(TextSession& s, bool on);
bool Italic(TextSession& s, bool on);
bool Underline(TextSession& s, bool on);
bool Strike(TextSession& s, bool on);
bool Baseline(TextSession& s, int which);  // 1 superscript, -1 subscript, 0 neither
bool Size(TextSession& s, float points);
// Ctrl+Shift+> and Ctrl+Shift+<: each run to the next size of PowerPoint's list; `step` (Ctrl+],
// Ctrl+[): a point at a time.
bool Grow(TextSession& s, bool grow, bool step);
bool Font(TextSession& s, const std::wstring& typeface);
bool Color(TextSession& s, const std::optional<draw::Color>& color);      // none: the style's
bool Highlight(TextSession& s, const std::optional<draw::Color>& color);  // none: no highlight
// Ctrl+Space: the text's own formatting goes (its language and links stay).
bool ClearCharacter(TextSession& s);
// Shift+F3: the next case in Word's cycle (lower, UPPER, Capitalize Each Word).
bool ChangeCase(TextSession& s);
// Ctrl+Shift+V after Ctrl+Shift+C: the copied look (an a:rPr) onto the selection.
bool ApplyLook(TextSession& s, pugi::xml_node rPr);
// The look at the caret (or the selection's start), as an a:rPr into `into`, for Ctrl+Shift+C.
void CopyLook(const TextSession& s, pugi::xml_document& into);

// Paragraph formatting.
bool Align(TextSession& s, TextAlign align);
bool LineSpacing(TextSession& s, float multiple);
bool SpaceBefore(TextSession& s, float points);
bool SpaceAfter(TextSession& s, float points);
bool Bullets(TextSession& s, bool on);
bool Numbering(TextSession& s, bool on);
// Increase or decrease the list level (Alt+Shift+Right/Left, the toolbar's indent buttons).
bool Level(TextSession& s, bool deeper);

// Links (Ctrl+K): `rel` is the relationship's id in the part (TextModel::AddLink); `slideJump`
// marks a link to a slide of the deck. Empty `rel`: the link goes. Over the selection, or the
// link's whole run at the caret.
bool SetLink(TextSession& s, const std::string& rel, bool slideJump, const std::wstring& tooltip);
// The link at the caret or over the selection: its run's range, and its hlinkClick.
bool LinkAt(const TextSession& s, TextPos& from, TextPos& to, pugi::xml_node& hlinkClick);

}  // namespace format
}  // namespace fw::slide::view
