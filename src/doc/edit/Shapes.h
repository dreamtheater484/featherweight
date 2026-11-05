#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "doc/edit/Editor.h"
#include "doc/edit/Pictures.h"

// Shapes and text boxes in a Word document (plan §6.2 "Text boxes and basic shapes", milestone
// D4): insert a text box, rectangle, rounded rectangle, oval, line, arrow or callout; resize, move
// and turn it; how text wraps around it; its fill and outline; text in it. Deleting is the
// editor's: a shape is one character (U+FFFC).
//
// Word writes a shape as an mc:AlternateContent block: the DrawingML shape (wps) for Word 2010 and
// later, and a VML copy for older Word. A change edits the DrawingML in place, as pictures do, and
// writes the VML copy again from it (plan §6.3 "Editing"): rectangles, rounded rectangles, ovals,
// lines and text boxes get a new one; other shapes, whose VML Featherweight doesn't write, then
// have none. A shape's text is a text box story the editor edits, written into both copies when the
// document is saved (docx::Writer).
//
// Every change is one undo step that puts the XML and the drawing's handle back exactly. A text box
// story made for a new shape (or for "Add text") stays in the model when that's undone, showing
// nowhere.
namespace fw::doc::edit {

enum class ShapeKind : uint8_t { TextBox, Rectangle, RoundedRectangle, Oval, Line, Arrow, Callout };

// What the shape tools show for a drawing.
struct ShapeInfo {
  bool shape = false;       // a shape these tools change
  std::u16string why;       // otherwise, why not
  std::string preset;       // its a:prstGeom ("rect", "ellipse", "line", ...); empty for others
  bool line = false;        // a line or connector: an outline, no fill and no text
  bool text = false;        // it has text (a text box story)
  Wrap wrap = Wrap::InFront;
  Emu cx{0}, cy{0};         // its size (wp:extent)
  double rotation = 0;      // degrees, clockwise, [0, 360)
  // Its fill and outline as plain colors (0xRRGGBB); nullopt when they're none or set another way
  // (a theme's style, a gradient).
  std::optional<uint32_t> fill, outline;
  bool noFill = false, noOutline = false;  // set to none
  double outlineWidth = 0;  // points; 0 when not set
};

class Shapes {
 public:
  explicit Shapes(Editor& editor);

  // A new shape, floating in front of the text with its top left at (x, y) on the page (EMU),
  // anchored at the start of paragraph `para`, `cx` x `cy` large (a line goes from its top left to
  // its bottom right). A text box and a callout have an empty paragraph to type in. One undo step;
  // the caret is the shape's character, so the view selects it.
  Result Insert(uint32_t para, ShapeKind kind, Emu x, Emu y, Emu cx, Emu cy);

  // ---- Changes to a drawing (an index into Document::Drawings()), each one undo step.
  ShapeInfo Info(uint32_t drawing) const;
  Result Resize(uint32_t drawing, Emu cx, Emu cy);
  Result Rotate(uint32_t drawing, double degrees);
  // A floating shape to (x, y) on the page it's anchored on (EMU, top left).
  Result Move(uint32_t drawing, Emu x, Emu y);
  // In line or floating, and how text wraps; a shape that starts floating stays where it is: `x`,
  // `y` are its top left on the page now (EMU), from the view.
  Result SetWrap(uint32_t drawing, Wrap wrap, Emu x = Emu(0), Emu y = Emu(0));
  // A plain fill color (0xRRGGBB), or none.
  Result SetFill(uint32_t drawing, std::optional<uint32_t> rgb);
  // A plain outline color, or none; `points` its width (0: as it is).
  Result SetOutline(uint32_t drawing, std::optional<uint32_t> rgb, double points = 0);
  // Gives a shape without text an empty paragraph to type in (Word's "Add Text").
  Result AddText(uint32_t drawing);

  // The story of the shape's text, kNone when it has none.
  uint32_t TextStory(uint32_t drawing) const;

 private:
  Editor& editor_;
};

}  // namespace fw::doc::edit
