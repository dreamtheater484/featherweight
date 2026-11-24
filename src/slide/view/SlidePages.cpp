#include "slide/view/SlidePages.h"

#include <algorithm>
#include <vector>

#include "text/Fonts.h"
#include "text/LineLayout.h"

namespace fw::slide::view {

namespace {

draw::GlyphRun ShownRun(const text::GlyphPiece& g, std::wstring_view text) {
  draw::GlyphRun r = g.Run();
  if (!g.hyphen && !g.leader && g.textStart < g.textEnd && g.textEnd <= text.size()) {
    r.text = reinterpret_cast<const char16_t*>(text.data() + g.textStart);
    r.textLength = g.textEnd - g.textStart;
  }
  return r;
}

const draw::Color kInk{0, 0, 0, 1};
const draw::Color kFrame{0.55f, 0.55f, 0.55f, 1};
const draw::Color kRule{0.62f, 0.62f, 0.62f, 1};

}  // namespace

float DrawPlainText(text::Fonts& fonts, draw::Painter& p, std::wstring_view text, const draw::Rect& box, float size,
                    const std::wstring& family, draw::Color color, bool rightAligned) {
  text::TextStyle style;
  style.family = family;
  style.size = size;
  const draw::Brush ink = draw::Brush::Solid(color);
  const float width = std::max(1.0f, box.Width());
  float y = box.top;
  size_t from = 0;
  while (from <= text.size()) {
    size_t end = text.find(L'\n', from);
    if (end == std::wstring_view::npos) end = text.size();
    std::wstring_view para = text.substr(from, end - from);
    if (!para.empty() && para.back() == L'\r') para.remove_suffix(1);
    text::Paragraph pg;
    pg.text = para;
    if (!para.empty()) pg.runs.push_back({static_cast<uint32_t>(para.size()), &style});
    pg.markStyle = &style;
    if (rightAligned) pg.format.alignment = text::Alignment::End;
    for (const text::Line& ln : text::Layout(fonts, pg, width)) {
      const float pitch = ln.ascent + ln.descent + ln.lineGap;
      if (y + ln.ascent + ln.descent > box.bottom + 0.5f) return y - box.top;
      const float baseline = y + ln.ascent;
      for (const text::GlyphPiece& g : ln.pieces)
        p.DrawGlyphs({box.left + g.OriginX(), baseline - g.rise}, ShownRun(g, para), ink);
      y += pitch;
    }
    if (end >= text.size()) break;
    from = end + 1;
  }
  return y - box.top;
}

void PaintPage(Deck& deck, const PrintPage& page, const PrintSettings& settings, text::Fonts& fonts,
               draw::Painter& p) {
  const draw::Size slide = deck.SlideSize();
  const bool framed = settings.what != PrintWhat::Slides && settings.frame;
  auto drawSlide = [&](size_t index, const draw::Rect& r) {
    p.Save();
    p.ClipRect(r);
    const float s = r.Width() / slide.width;
    p.Transform(draw::Matrix::Scale(s, s) * draw::Matrix::Translate(r.left, r.top));
    deck.Paint(index, p);
    p.Restore();
    if (framed) {
      draw::Pen pen;
      pen.brush = draw::Brush::Solid(kFrame);
      pen.width = 0.5f;
      p.StrokePath(draw::Path::FromRect(r), pen);
    }
  };
  bool ownNumber = settings.what != PrintWhat::Slides;
  for (const PageItem& it : page.items) {
    if (it.slide >= deck.SlideCount() || slide.width <= 0 || slide.height <= 0) continue;
    if (settings.what == PrintWhat::Notes) {
      // The deck's own notes page (its notes master's placeholders, the notes in its fonts, its
      // page number), fitted on the page, with the slide where its slide image goes.
      const draw::Size ns = deck.NotesSize();
      const draw::Rect fit = FitRect(ns, {0, 0, page.size.width, page.size.height});
      const float k = ns.width > 0 ? fit.Width() / ns.width : 1;
      p.Save();
      p.Transform(draw::Matrix::Scale(k, k) * draw::Matrix::Translate(fit.left, fit.top));
      draw::Rect box;
      const bool drawn = deck.PaintNotesPage(it.slide, p, box);
      if (drawn && box.Width() > 0 && box.Height() > 0) drawSlide(it.slide, FitRect(slide, box));
      p.Restore();
      if (drawn) {
        ownNumber = false;
        continue;
      }
    }
    drawSlide(it.slide, it.rect);
    if (settings.what == PrintWhat::Notes && it.notes.Width() > 0) {
      const std::wstring notes = deck.Notes(it.slide);
      if (!notes.empty()) DrawPlainText(fonts, p, notes, it.notes, 12, L"Calibri", kInk);
    } else if (settings.what == PrintWhat::Handouts && it.notes.Width() > 0) {
      // Three per page: lines to write on beside each slide, as PowerPoint prints them.
      draw::Pen pen;
      pen.brush = draw::Brush::Solid(kRule);
      pen.width = 0.5f;
      const float step = std::max(12.0f, it.notes.Height() / 8);
      for (float y = it.notes.top + step; y <= it.notes.bottom + 0.5f; y += step) {
        draw::Path line;
        line.MoveTo({it.notes.left, y});
        line.LineTo({it.notes.right, y});
        p.StrokePath(line, pen);
      }
    }
  }
  // Handouts and notes pages carry their page number in the bottom right corner.
  if (ownNumber && page.number > 0) {
    const float h = page.size.height, w = page.size.width;
    const draw::Rect box{w * 0.55f, h - h * 0.06f, w * 0.9f, h - h * 0.02f};
    DrawPlainText(fonts, p, std::to_wstring(page.number), box, 10, L"Calibri", kInk, true);
  }
}

}  // namespace fw::slide::view
