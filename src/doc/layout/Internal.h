#pragma once

// Pagination internals (Layout.h), shared by the layout's source files.
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "doc/layout/Layout.h"
#include "text/Fonts.h"
#include "text/LineLayout.h"

namespace fw::doc::layout {

inline float Pt(Twips t) { return static_cast<float>(t.Value()) / 20.0f; }
inline float Pt(Emu e) { return static_cast<float>(e.Value()) / 12700.0f; }
inline float Pt(HalfPoints h) { return static_cast<float>(h.Value()) / 2.0f; }
inline float Pt(EighthPoints e) { return static_cast<float>(e.Value()) / 8.0f; }

// What fields that pagination computes show on the page being laid out.
struct PageFields {
  std::u16string page = u"1", pages, sectionPages;
};

// fwtext styles, interned for the document's lifetime: lines keep pointers to them.
class StylePool {
 public:
  const text::TextStyle* Intern(const text::TextStyle& style);
  // The font metrics of a style's main face, for underlines and strikethrough.
  const text::FontMetrics& Metrics(text::Fonts& fonts, const text::TextStyle* style);
  size_t Count() const { return pool_.size(); }

 private:
  std::deque<text::TextStyle> pool_;
  std::unordered_map<std::wstring, const text::TextStyle*> index_;
  std::unordered_map<const text::TextStyle*, text::FontMetrics> metrics_;
};

// A paragraph made ready for fwtext: its text as laid out (the list label first, field results
// that pagination computes filled in), its runs and how each is painted.
struct ParaText {
  uint32_t para = kNone;
  const ParaProps* props = nullptr;
  std::wstring text;
  std::vector<text::TextRun> runs;
  std::vector<RunPaint> paints;  // one per run
  text::Paragraph fw;            // views into text and runs
  uint32_t prefix = 0;           // label characters before the paragraph's own text
  // Where the layout text and the model text differ in length: [at, at + length) in the layout
  // text stands for [modelAt, modelAt + modelLength) in the model's.
  struct Subst {
    uint32_t at = 0, length = 0, modelAt = 0, modelLength = 0;
  };
  std::vector<Subst> subst;
  std::vector<std::pair<uint32_t, uint32_t>> notes;  // (layout offset, note story) for each reference
  std::vector<uint32_t> anchors;                     // floating drawings anchored in it
  std::vector<std::pair<uint32_t, uint32_t>> inlines;  // (layout offset, drawing) for inline drawings
  bool usesPageCount = false;                        // NUMPAGES or SECTIONPAGES
  bool empty = false;                                // nothing visible: a hidden paragraph

  uint32_t ModelOffset(uint32_t layoutOffset) const;
  // What [from, to) of the layout text shows: hidden text and markers left out, a hyphen added.
  std::u16string Shown(uint32_t from, uint32_t to, bool hyphenated) const;
  uint32_t LayoutOffset(uint32_t modelOffset) const;
};

class Engine;

// Lays out stacked content (a header, a cell, a note, a text box) in a frame `width` wide from y=0
// down, without page breaks: lines and boxes relative to the frame's top-left corner.
struct Stack {
  std::vector<LineBox> lines;
  std::vector<RunPaint> paints;
  std::vector<Fill> fills;
  std::vector<Edge> edges;
  std::vector<DrawingBox> drawings;
  float height = 0;
  bool usesPageCount = false;  // shows NUMPAGES or SECTIONPAGES
  void Append(Stack&& other, float dx, float dy);
  void Offset(float dx, float dy);
};

}  // namespace fw::doc::layout
