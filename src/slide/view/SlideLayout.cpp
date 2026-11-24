#include "slide/view/SlideLayout.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwctype>

#include "core/Strings.h"

namespace fw::slide::view {

draw::Rect FitRect(draw::Size content, const draw::Rect& area, float maxScale) {
  const float w = std::max(0.0f, area.Width()), h = std::max(0.0f, area.Height());
  if (content.width <= 0 || content.height <= 0) return {area.left, area.top, area.left, area.top};
  const float s = std::min({w / content.width, h / content.height, maxScale});
  const float cw = content.width * s, ch = content.height * s;
  const float x = area.left + (w - cw) / 2, y = area.top + (h - ch) / 2;
  return {x, y, x + cw, y + ch};
}

// ---------------------------------------------------------------------------
// Thumbnails

namespace {
constexpr float kThumbPad = 12;     // DIPs: above the first row, between rows, below the last
constexpr float kThumbNumber = 28;  // DIPs: the number column
constexpr float kThumbRight = 14;   // DIPs: right of the picture (room for the scrollbar)
}  // namespace

std::vector<ThumbRow> ThumbRows(size_t count, draw::Size slide, const ThumbMetrics& m) {
  std::vector<ThumbRow> rows;
  rows.reserve(count);
  const float ui = m.ui > 0 ? m.ui : 1;
  const float left = kThumbNumber * ui, right = std::max(left + 8 * ui, m.width - kThumbRight * ui);
  const float w = right - left;
  const float h = slide.width > 0 ? std::round(w * slide.height / slide.width) : w * 0.5625f;
  float y = kThumbPad * ui;
  for (size_t i = 0; i < count; ++i) {
    ThumbRow r;
    r.slide = {left, y, right, y + h};
    r.number = {0, y, left - 6 * ui, y + 20 * ui};
    r.row = {0, y - kThumbPad * ui / 2, m.width, y + h + kThumbPad * ui / 2};
    rows.push_back(r);
    y += h + kThumbPad * ui;
  }
  return rows;
}

float ThumbListHeight(const std::vector<ThumbRow>& rows, float ui) {
  return rows.empty() ? 0 : rows.back().slide.bottom + kThumbPad * (ui > 0 ? ui : 1);
}

// ---------------------------------------------------------------------------
// Pages

bool ValidHandoutCount(int n) {
  for (int k : kHandoutCounts)
    if (k == n) return true;
  return false;
}

std::vector<PageItem> HandoutSlots(int perPage, draw::Size page, draw::Size slide) {
  if (!ValidHandoutCount(perPage)) perPage = 6;
  // PowerPoint's handout master: a header and date along the top, footer and page number along
  // the bottom, half an inch each, and the slides in the rest with a quarter inch or so between.
  const float side = page.width * 0.1f, top = page.height * 0.075f, bottom = page.height * 0.075f;
  const draw::Rect area{side, top, page.width - side, page.height - bottom};
  int cols = 1, rows = 1;
  switch (perPage) {
    case 1: cols = 1, rows = 1; break;
    case 2: cols = 1, rows = 2; break;
    case 3: cols = 1, rows = 3; break;
    case 4: cols = 2, rows = 2; break;
    case 6: cols = 2, rows = 3; break;
    case 9: cols = 3, rows = 3; break;
  }
  // 3 per page: the slides take the left part and lines for notes the right.
  const float slotsWidth = perPage == 3 ? area.Width() * 0.48f : area.Width();
  const float gapX = cols > 1 ? page.width * 0.04f : 0, gapY = rows > 1 ? page.height * 0.03f : 0;
  const float cellW = (slotsWidth - gapX * (cols - 1)) / cols;
  const float cellH = (area.Height() - gapY * (rows - 1)) / rows;
  std::vector<PageItem> out;
  for (int r = 0; r < rows; ++r)
    for (int c = 0; c < cols; ++c) {
      const float x = area.left + c * (cellW + gapX), y = area.top + r * (cellH + gapY);
      PageItem it;
      it.slide = out.size();
      it.rect = FitRect(slide, {x, y, x + cellW, y + cellH});
      if (perPage == 3) {
        const float lx = area.left + slotsWidth + page.width * 0.05f;
        it.notes = {lx, it.rect.top, area.right, it.rect.bottom};
      }
      out.push_back(it);
    }
  return out;
}

PageItem NotesSlots(draw::Size page, draw::Size slide) {
  // PowerPoint's default notes master on a 7.5 x 10 inch page: the slide image 0.75 in from the
  // left and 1.25 in from the top, 6 in wide; the notes 0.75 in from the left, 4.8 in from the top,
  // 6 in wide and 3.9 in tall. As fractions of the page, so other notes sizes scale.
  PageItem it;
  const draw::Rect image{page.width * 0.1f, page.height * 0.125f, page.width * 0.9f, page.height * 0.4625f};
  it.rect = FitRect(slide, image);
  it.notes = {page.width * 0.1f, page.height * 0.48f, page.width * 0.9f, page.height * 0.875f};
  return it;
}

std::vector<PrintPage> LayoutPages(const std::vector<size_t>& slides, draw::Size slideSize, draw::Size slidePaper,
                                   draw::Size paper, const PrintSettings& settings) {
  std::vector<PrintPage> pages;
  switch (settings.what) {
    case PrintWhat::Slides:
      for (size_t s : slides) {
        PrintPage p;
        p.size = slidePaper;
        p.number = static_cast<int>(pages.size()) + 1;
        p.items.push_back({s, FitRect(slideSize, {0, 0, slidePaper.width, slidePaper.height}), {}});
        pages.push_back(std::move(p));
      }
      break;
    case PrintWhat::Notes:
      for (size_t s : slides) {
        PrintPage p;
        p.size = paper;
        p.number = static_cast<int>(pages.size()) + 1;
        PageItem it = NotesSlots(paper, slideSize);
        it.slide = s;
        p.items.push_back(it);
        pages.push_back(std::move(p));
      }
      break;
    case PrintWhat::Handouts: {
      const std::vector<PageItem> slots = HandoutSlots(settings.perPage, paper, slideSize);
      for (size_t i = 0; i < slides.size(); i += slots.size()) {
        PrintPage p;
        p.size = paper;
        p.number = static_cast<int>(pages.size()) + 1;
        for (size_t k = 0; k < slots.size() && i + k < slides.size(); ++k) {
          PageItem it = slots[k];
          it.slide = slides[i + k];
          p.items.push_back(it);
        }
        pages.push_back(std::move(p));
      }
      break;
    }
  }
  return pages;
}

// ---------------------------------------------------------------------------
// Find

std::vector<size_t> FindAll(std::wstring_view text, std::wstring_view query, bool matchCase, bool wholeWords) {
  std::vector<size_t> out;
  if (query.empty() || text.size() < query.size()) return out;
  std::wstring t, q;
  if (matchCase) {
    t = text;
    q = query;
  } else {
    t = fw::ToLower(text);  // CharLowerBuff keeps the length, so offsets stay the text's
    q = fw::ToLower(query);
  }
  auto wordChar = [](wchar_t c) { return std::iswalnum(c) != 0 || c == L'_'; };
  for (size_t at = t.find(q); at != std::wstring::npos; at = t.find(q, at + 1)) {
    if (wholeWords) {
      if (at > 0 && wordChar(t[at - 1])) continue;
      if (at + q.size() < t.size() && wordChar(t[at + q.size()])) continue;
    }
    out.push_back(at);
    at += q.size() - 1;  // not overlapping
  }
  return out;
}

}  // namespace fw::slide::view
