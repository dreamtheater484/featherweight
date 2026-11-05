#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "doc/edit/Editor.h"

// Pictures in a Word document (plan §6.2 "Pictures", milestone D3): insert from a file, the
// clipboard or a drop; resize, rotate and crop; in line with the text or floating, and where; alt
// text; reset size. Deleting is the editor's: the picture is one character (U+FFFC).
//
// A picture's DrawingML is kept as XML (the model keeps drawings as round-trip fragments, plan
// §6.3), so a change edits that XML in place: the attributes and elements it changes, nothing
// else. Unknown DrawingML stays as it was. Drawings this doesn't change (shapes, charts, groups,
// VML, anything in mc:AlternateContent) are refused with a plain message.
//
// Every change is one undo step of the editor, and undo puts back the XML, the drawing's handle
// and the package (a picture's part, relationship and content type) exactly. Pictures are read
// with fwdraw's limits (ImageLimits), as anything from outside is.
namespace fw::doc::edit {

// How a picture sits among the text.
enum class Wrap : uint8_t {
  Inline,        // in line with the text: a character
  Square,        // floating, text around its box
  Tight,         // floating, text close around it
  TopAndBottom,  // floating, text above and below
  Behind,        // floating, behind the text
  InFront,       // floating, in front of the text
};

// The part of a picture cut off each side, as a fraction of the picture (a:srcRect).
struct Crop {
  double left = 0, top = 0, right = 0, bottom = 0;
  bool operator==(const Crop&) const = default;
  bool Any() const { return left != 0 || top != 0 || right != 0 || bottom != 0; }
};

// What the picture tools show for a drawing.
struct PictureInfo {
  bool picture = false;    // a picture these tools change
  std::u16string why;      // otherwise, why not
  Wrap wrap = Wrap::Inline;
  Emu cx{0}, cy{0};        // its size (wp:extent)
  double rotation = 0;     // degrees, clockwise, [0, 360)
  bool flipH = false, flipV = false;
  Crop crop;
  bool lockAspect = true;  // resize keeps the shape unless Shift is held (a:picLocks noChangeAspect)
  std::u16string altText;  // wp:docPr descr
};

class Pictures {
 public:
  explicit Pictures(Editor& editor);

  // Whether the bytes are a picture Insert takes: PNG, JPEG, GIF, BMP, TIFF or SVG.
  static bool Accepts(const std::vector<uint8_t>& bytes);

  // Inserts the picture at `at`, in line with the text, at its own size, made smaller to fit
  // `maxWidth` (the text column; 0 for no limit). `name` is the file's name, for wp:docPr. A BMP is
  // stored as PNG; an SVG with a PNG copy for Word before 2016, as Word does. A picture identical
  // to one the document already has shares its part. One undo step.
  Result Insert(Pos at, const std::vector<uint8_t>& bytes, Emu maxWidth = Emu(0), std::u16string_view name = {});

  // ---- Changes to a drawing (an index into Document::Drawings()), each one undo step.
  PictureInfo Info(uint32_t drawing) const;
  // Its size (wp:extent, and the picture's own a:ext).
  Result Resize(uint32_t drawing, Emu cx, Emu cy);
  // Turns it to `degrees` clockwise (a:xfrm rot); the room it takes around it follows
  // (wp:effectExtent).
  Result Rotate(uint32_t drawing, double degrees);
  // Crops it (a:srcRect). The picture keeps its scale, so its size follows the crop.
  Result SetCrop(uint32_t drawing, const Crop& crop);
  // In line or floating, and how text wraps. A picture that starts floating keeps the place it
  // has: `x`, `y` are its top left on the page now (EMU), from the view.
  Result SetWrap(uint32_t drawing, Wrap wrap, Emu x = Emu(0), Emu y = Emu(0));
  // A floating picture to (x, y) on its page (EMU, top left), held to the page there. With `anchor`
  // (a paragraph start, where it was dropped) it moves there first, as Word moves the anchor.
  Result MoveFloating(uint32_t drawing, Emu x, Emu y, std::optional<Pos> anchor = {});
  // A picture in line to `to`, a place in the text.
  Result MoveInline(uint32_t drawing, Pos to);
  Result SetAltText(uint32_t drawing, std::u16string_view text);
  // Its own size: no crop, no rotation, 100 %.
  Result ResetSize(uint32_t drawing);

  // The drawing's own size in EMU (its picture at 100 %), or nothing when it can't be read.
  std::optional<std::pair<Emu, Emu>> NaturalSize(uint32_t drawing) const;

 private:
  Editor& editor_;
};


}  // namespace fw::doc::edit
