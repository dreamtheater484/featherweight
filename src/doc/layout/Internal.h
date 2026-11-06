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
  bool usesPage = false;                             // PAGE
  bool empty = false;                                // nothing visible: a hidden paragraph
  // The paragraphs before it that the view joins to it (their marks deleted): their text is laid out
  // first, after the label, and stands for this paragraph's start. They themselves are empty.
  std::vector<uint32_t> joined;

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
  std::vector<CellRect> cells;
  // Footnotes its lines refer to: the bottom of the line with the reference, and the note's story.
  std::vector<std::pair<float, uint32_t>> notes;
  float height = 0;
  bool usesPageCount = false;  // shows NUMPAGES or SECTIONPAGES
  bool usesPage = false;       // shows PAGE
  bool numbersNotes = false;   // has note references, numbered as they're laid out
  bool rtl = false;            // a note separator in a right-to-left paragraph
  // Has content placed relative to the page or its margins, or that text wraps around: where it
  // goes depends on where the stack goes (a footer is stacked before it's placed).
  bool placedOnPage = false;
  bool labels = false;  // shows list labels, which an edit before them may number again
  void Append(Stack&& other, float dx, float dy);
  void TakeFlags(const Stack& other);  // what `other`, shown in it, depends on
  void Offset(float dx, float dy);
};

// The lines of paragraphs laid out again after an edit (D2): a paragraph's text and the lines its
// breaker made, kept by paragraph version and column width, so the next keystroke breaks only the
// paragraph that changed (plan §4), whether the paragraph is in the body or stacked in a cell, a
// note or a header. Kept for the few pages laid out after each edit (Engine::LinesOf).
struct CachedPara {
  uint32_t version = 0;
  float width = 0;
  bool breaks = false;  // laid out with the body's page and column breaks (BuildText's pageBreaks)
  std::shared_ptr<ParaText> pt;
  text::LineBreaker::State start;
  struct Made {
    text::LineBreaker::State before, after;
    float left = 0, right = 0;
    text::Line line;
  };
  std::vector<Made> lines;
  uint32_t used = 0;  // the edit it was last used after
};

inline bool SameState(const text::LineBreaker::State& a, const text::LineBreaker::State& b) {
  return a.position == b.position && a.line == b.line && a.hyphenated == b.hyphenated && a.done == b.done;
}

// A paragraph's lines as its LineBreaker makes them: from the cache when they were made before,
// else from a breaker made when first needed.
class ParaLines {
 public:
  ParaLines(text::Fonts& fonts, std::shared_ptr<ParaText> pt, float width, std::shared_ptr<CachedPara> cache)
      : fonts_(fonts), pt_(std::move(pt)), width_(width), cache_(std::move(cache)) {
    if (cache_ && !cache_->lines.empty()) {
      state_ = cache_->start;
    } else {
      state_ = Breaker().Save();
      if (cache_) cache_->start = state_;
    }
  }
  bool Done() const { return state_.done; }
  text::LineBreaker::State Save() const { return state_; }
  void Restore(const text::LineBreaker::State& state) { state_ = state; }
  text::Line Next(float left, float right) {
    if (cache_)
      for (const CachedPara::Made& m : cache_->lines)
        if (SameState(m.before, state_) && m.left == left && m.right == right) {
          state_ = m.after;
          return m.line;
        }
    text::LineBreaker& b = Breaker();
    b.Restore(state_);
    text::Line line = b.Next(left, right);
    const text::LineBreaker::State after = b.Save();
    if (cache_) cache_->lines.push_back({state_, after, left, right, line});
    state_ = after;
    return line;
  }

 private:
  text::LineBreaker& Breaker() {
    if (!breaker_) breaker_ = std::make_unique<text::LineBreaker>(fonts_, pt_->fw, width_);
    return *breaker_;
  }
  text::Fonts& fonts_;
  std::shared_ptr<ParaText> pt_;
  float width_;
  std::shared_ptr<CachedPara> cache_;
  std::unique_ptr<text::LineBreaker> breaker_;
  text::LineBreaker::State state_;
};

}  // namespace fw::doc::layout
