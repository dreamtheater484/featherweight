#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "doc/edit/Editor.h"

// A document's watermark (plan §6.2 "Text watermark", milestone D4): text or a picture behind the
// text of every page, written as Word writes it: a VML shape in each header ("PowerPlusWaterMark"
// for text, "WordPictureWatermark" for a picture), centered on the margins, behind the text. A
// section's page types without a header get one (HeaderFooter.h). Each change is one undo step.
namespace fw::doc::edit {

struct TextWatermark {
  std::u16string text = u"DRAFT";
  std::string font = "Calibri";
  uint32_t color = 0xC0C0C0;  // 0xRRGGBB
  bool semitransparent = true;
  bool diagonal = true;
  // The text's width over its height in `font`, as the view measures it: the shape takes this
  // shape, so the text isn't stretched. 0: guessed from its length.
  double aspect = 0;
};

struct WatermarkInfo {
  enum class Kind : uint8_t { None, Text, Picture };
  Kind kind = Kind::None;
  std::u16string text;  // a text watermark's
  std::string font;
  uint32_t color = 0xC0C0C0;
  bool diagonal = false, semitransparent = false;
  bool washout = false;  // a picture's
};

// The watermark the document's headers show (the first one found).
WatermarkInfo FindWatermark(const Document& doc);
// A text watermark in every header, in place of any watermark there.
Result SetTextWatermark(Editor& editor, const TextWatermark& mark);
// A picture watermark (PNG, JPEG, GIF, BMP or TIFF), washed out as Word does unless `washout` is false.
Result SetPictureWatermark(Editor& editor, const std::vector<uint8_t>& bytes, bool washout = true);
// Takes away every watermark.
Result RemoveWatermark(Editor& editor);

}  // namespace fw::doc::edit
