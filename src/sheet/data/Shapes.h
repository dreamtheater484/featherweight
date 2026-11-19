#pragma once

// Pictures and shapes (s3-data): the anchors Excel writes when it inserts a picture or one of the
// shapes Featherweight offers, and an object's formatting (fill, outline, text, alt text) read
// from its anchor and written back into it, everything else in it kept. Editor ops put them into
// the sheet's drawing (s3-charts' drawing API places them: drawing::Placed).

#include <cstdint>
#include <string>
#include <string_view>

namespace fw::sheet::data {

enum class ShapeKind : uint8_t { Rectangle, RoundedRectangle, Oval, Line, Arrow, TextBox };

// Excel's name for a new shape, before its number ("Rectangle 1"): Rectangle, "Rectangle: Rounded
// Corners", Oval, "Straight Connector", "Straight Arrow Connector", TextBox; and Picture.
const char* ShapeName(ShapeKind kind);

// A new shape's anchor as Excel 2021 inserts it (measured): a twoCellAnchor (moves and sizes with
// cells) with Excel's style references (accent 1), the text box's white fill and grey outline,
// lines and arrows as connectors. `id` is its cNvPr id, named "<ShapeName> <id - 1>"; `text` goes
// in the shape (not lines or arrows), paragraphs split at \n. Its place is all zeros: the caller
// places it (drawing::Placed). The anchor declares the xdr: and a: prefixes.
std::string ShapeAnchor(ShapeKind kind, uint32_t id, std::string_view text);

// A picture's anchor as Excel inserts it: a twoCellAnchor that moves but doesn't size with cells
// (editAs="oneCell"), the aspect ratio locked, the picture `relId` (r:embed) stretched over it, and
// the alt text (descr). Placed by the caller, as ShapeAnchor.
std::string PictureAnchor(uint32_t id, std::string_view relId, std::string_view altText);

// A picture's bytes as the package stores them: the media part's extension and content type, from
// the bytes themselves (PNG, JPEG, GIF, BMP); false for anything else.
bool PictureType(std::string_view bytes, std::string& extension, std::string& contentType);

// An object's formatting as Format Shape shows it.
struct ObjectFormat {
  enum class Kind : uint8_t { Shape, Connector, Picture, Other };
  enum class Paint : uint8_t { Automatic, None, Solid };  // Automatic: the shape style's
  Kind kind = Kind::Other;
  std::string name;  // cNvPr name ("Rectangle 1"); read only
  Paint fill = Paint::Automatic;
  uint32_t fillRgb = 0;  // 0xRRGGBB, for Solid (a theme color reads as its slot's value: unknown, 0)
  Paint line = Paint::Automatic;
  uint32_t lineRgb = 0;
  double lineWidth = 0;  // points; 0: as it is
  bool hasText = false;  // a shape that holds text (not connectors, pictures)
  std::string text;      // its paragraphs, joined with \n
  std::string altText;   // cNvPr descr
};
// What FormatObject changes: the rest of the anchor stays as it is.
enum : uint32_t { kFormatFill = 1u << 0, kFormatLine = 1u << 1, kFormatText = 1u << 2, kFormatAltText = 1u << 3 };

// The formatting of an object's anchor (one object's bytes, as Editor::AnchorXml gives them).
ObjectFormat ReadObjectFormat(std::string_view anchor);
// The anchor with `f`'s parts named by `what` written in, as Excel writes them (a solid fill or
// none in the shape's properties; the outline's fill and width; the text in paragraphs that keep
// the first one's paragraph and run properties; descr). Empty when the anchor doesn't read.
std::string FormatObject(std::string_view anchor, const ObjectFormat& f, uint32_t what);

}  // namespace fw::sheet::data
