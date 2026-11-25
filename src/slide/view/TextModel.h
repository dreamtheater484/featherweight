#pragma once

#include <pugixml.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "slide/Text.h"

namespace fw::slide {
class Presentation;
struct SlideBase;
struct Shape;
}

// What text editing needs from the presentation and its edit layer (P2): where a text body is, its
// text as the model has it, how a working copy of it resolves, and the one call that puts an edited
// copy back. The tab gives an implementation over p2-model's edit API (src/slide/edit); tests give
// their own. Text editing changes the model only through SetText (plan §4.3 rule 4).
namespace fw::slide::view {

// A text body: a shape's on a slide or its notes page, or a table cell's.
struct TextTarget {
  size_t slide = 0;
  bool notes = false;           // the slide's notes page
  uint32_t shape = 0;           // cNvPr id in that part
  int row = -1, column = -1;    // a table cell; -1 otherwise
  bool Cell() const { return row >= 0 && column >= 0; }
  friend bool operator==(const TextTarget&, const TextTarget&) = default;
};

class TextModel {
 public:
  virtual ~TextModel() = default;

  // The part the target is in (null when it's gone: the slide was deleted).
  virtual const SlideBase* Part(const TextTarget& t) = 0;
  // The text body as the model has it now (null when the shape has none yet: an empty placeholder
  // without one).
  virtual pugi::xml_node Body(const TextTarget& t) = 0;
  // A working copy of it, resolved as the model would resolve the body (Presentation::ResolveTextBody,
  // ResolveCellText).
  virtual std::unique_ptr<TextBody> Resolve(const TextTarget& t, pugi::xml_node txBody) = 0;

  // Puts an edited copy into the model as one undo step named `step`; `coalesce` folds it into the
  // previous step when that was this target's too (typing). False, with a reason, when refused.
  virtual bool SetText(const TextTarget& t, pugi::xml_node txBody, std::u16string_view step, bool coalesce,
                       std::wstring& error) = 0;
  // Several edits as one undo step (Replace All, autofit's resize with its text).
  virtual void BeginGroup(std::u16string_view step) = 0;
  virtual void EndGroup() = 0;
  // Autofit's "resize shape to fit text": the shape's height, points. False when it can't.
  virtual bool SetShapeHeight(const TextTarget& t, float height) {
    (void)t;
    (void)height;
    return false;
  }
  // A hyperlink's relationship in the target's part: a web or mail address (external), or a slide
  // (its index). The relationship id, or empty when it can't be made.
  virtual std::string AddLink(const TextTarget& t, const std::wstring& url, int slide) {
    (void)t;
    (void)url;
    (void)slide;
    return {};
  }
  virtual Presentation* Deck() = 0;
};

}  // namespace fw::slide::view
