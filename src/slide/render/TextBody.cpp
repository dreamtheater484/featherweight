#include "slide/render/TextBody.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <deque>

#include "core/Strings.h"
#include "draw/Shapes.h"
#include "draw/Xml.h"
#include "text/Fonts.h"
#include "text/LineLayout.h"

namespace fw::slide::render {

using namespace draw::xml;

namespace {

constexpr size_t kMaxChars = 200000;

bool IsSymbolFont(std::wstring_view f) {
  std::wstring l(f);
  for (wchar_t& c : l) c = static_cast<wchar_t>(std::towlower(c));
  return l == L"wingdings" || l == L"wingdings 2" || l == L"wingdings 3" || l == L"webdings" || l == L"symbol" ||
         l == L"marlett" || l == L"monotype sorts" || l == L"zapfdingbats" || l == L"mt extra";
}

std::wstring Roman(int n, bool upper) {
  static constexpr std::pair<int, const wchar_t*> kTable[] = {{1000, L"m"}, {900, L"cm"}, {500, L"d"}, {400, L"cd"},
                                                              {100, L"c"},  {90, L"xc"},  {50, L"l"},  {40, L"xl"},
                                                              {10, L"x"},   {9, L"ix"},   {5, L"v"},   {4, L"iv"},
                                                              {1, L"i"}};
  std::wstring out;
  n = std::clamp(n, 1, 3999);
  for (const auto& [v, s] : kTable)
    while (n >= v) {
      out += s;
      n -= v;
    }
  if (upper)
    for (wchar_t& c : out) c = static_cast<wchar_t>(std::towupper(c));
  return out;
}

// ST_TextAutonumberScheme: the common ones; the rest count in arabic numerals with a period.
std::wstring AutoNumber(std::string_view scheme, int n) {
  std::wstring body;
  if (scheme.starts_with("roman")) {
    body = Roman(n, scheme.starts_with("romanUc"));
  } else if (scheme.starts_with("alpha")) {
    n = std::max(n, 1);
    body.assign(static_cast<size_t>((n - 1) / 26 + 1), static_cast<wchar_t>((scheme.starts_with("alphaUc") ? L'A' : L'a') + (n - 1) % 26));
  } else if (scheme.starts_with("circleNum") && n >= 1 && n <= 20) {
    return std::wstring(1, static_cast<wchar_t>(0x2460 + n - 1));
  } else {
    body = std::to_wstring(n);
  }
  if (scheme.ends_with("ParenBoth")) return L"(" + body + L")";
  if (scheme.ends_with("ParenR")) return body + L")";
  if (scheme.ends_with("Period")) return body + L".";
  if (scheme.ends_with("Minus")) return L"- " + body + L" -";
  return body;
}

struct RunDraw {
  std::optional<draw::Color> color;  // none: not drawn (noFill)
  std::optional<draw::Color> outline;  // the glyphs' outline (a:ln), `outlineWidth` points wide
  float outlineWidth = 0;
  std::optional<draw::Color> highlight, underlineColor;
  uint8_t underline = 0;  // 0 none, 1 single, 2 double, 3 heavy
  uint8_t strike = 0;     // 0 none, 1 single, 2 double
  float ulPos = 0, ulThick = 0, stPos = 0, stThick = 0;
  int link = -1;
};

// Whether a hyperlink keeps its text's color (a14's hlinkClr val="tx").
bool LinkKeepsTextColor(pugi::xml_node run) {
  bool keep = false;
  ForEachChild(A(A(A(run, "rPr"), "hlinkClick"), "extLst"), [&](pugi::xml_node ext) {
    ForEachChild(ext, [&](pugi::xml_node c) {
      if (LocalNameOf(c) == "hlinkClr" && std::string_view(c.attribute("val").value()) == "tx") keep = true;
      return !keep;
    });
    return !keep;
  });
  return keep;
}

}  // namespace

struct TextBlock::Impl {
  struct Para {
    std::wstring text;
    std::deque<text::TextStyle> styles;
    text::TextStyle mark;
    std::vector<uint32_t> runStart;  // per run: its first offset in text
    std::vector<RunDraw> runs;
    std::vector<text::Line> lines;
    std::vector<draw::Point> origins;           // per line: where column coordinates' 0 is, and the baseline
    std::vector<std::pair<float, float>> rows;  // per line: top and bottom
    std::shared_ptr<const draw::Image> picture;  // a picture bullet, at its U+FFFC
    bool empty = true;
    bool bullet = false;  // the first run is the bullet and its tab
    uint32_t first = 0;   // where the paragraph's own text starts in `text` (after the bullet)
  };
  struct LinkRun {
    size_t para;
    uint32_t start, end;
    fw::slide::Hyperlink link;
  };
  std::vector<Para> paras;
  std::vector<LinkRun> links;
  draw::Matrix toBox;  // text coordinates → the box's
  float height = 0;
  draw::Rect rect;
};

TextBlock::TextBlock() : impl_(std::make_unique<Impl>()) {}
TextBlock::~TextBlock() = default;
TextBlock::TextBlock(TextBlock&&) noexcept = default;
TextBlock& TextBlock::operator=(TextBlock&&) noexcept = default;

void TextBlock::Build(text::Fonts& fonts, const draw::TextBox& box, const fw::slide::TextBody& body, const TextExtras& extras) {
  using fw::slide::Bullet;
  using fw::slide::RunProps;
  using fw::slide::TextAlign;
  Impl& m = *impl_;
  m = Impl{};
  m.rect = box.rect;
  const float fontScale = box.autofit == draw::TextBox::Autofit::ShrinkText ? box.fontScale : 1.f;
  const float lineReduction = box.autofit == draw::TextBox::Autofit::ShrinkText ? box.lineSpacingReduction : 0.f;
  const bool vertical = box.flow != draw::TextBox::Flow::Horizontal;
  const float boxW = std::max(0.f, vertical ? box.rect.Height() : box.rect.Width());
  const float boxH = std::max(0.f, vertical ? box.rect.Width() : box.rect.Height());
  const int columns = std::max(1, box.columns);
  const float colGap = columns > 1 ? box.columnSpacing : 0.f;
  const float colW = std::max(1.f, (boxW - colGap * (columns - 1)) / columns);
  const float layoutW = box.wrap ? colW : 100000.f;

  auto styleOf = [&](const RunProps& r) {
    text::TextStyle s;
    s.family = r.latin.empty() ? std::wstring(L"Calibri") : r.latin;
    s.eastAsiaFamily = r.eastAsian;
    s.complexFamily = r.complexScript;
    // PowerPoint rounds a shrunk size to whole points (32 pt at 55 % is drawn at 18).
    float size = fontScale < 1 ? std::max(1.f, std::round(r.size * fontScale)) : r.size;
    if (r.baseline != 0) {
      s.baselineShift = r.baseline * size;
      size *= 2.f / 3;
    }
    s.size = std::max(0.5f, size);
    s.bold = s.complexBold = r.bold;
    s.italic = s.complexItalic = r.italic;
    s.caps = r.caps == fw::slide::Caps::All ? text::Caps::All : r.caps == fw::slide::Caps::Small ? text::Caps::Small : text::Caps::None;
    s.letterSpacing = r.spacing;
    s.kerningMinSize = r.kern;
    if (!r.lang.empty()) s.language = Utf8ToWide(r.lang);
    return s;
  };
  auto runDraw = [&](const RunProps& r, const text::TextStyle& s) {
    RunDraw d;
    if (!r.noFill) d.color = r.color;
    if (r.outline && !A(r.outline, "noFill")) {
      d.outline = draw::ReadColor(A(r.outline, "solidFill"), extras.colors);
      d.outlineWidth = std::max(0.25f, EmuToPt(Number(r.outline, "w", 9525)) * (fontScale < 1 ? fontScale : 1.f));
    }
    d.highlight = r.highlight;
    d.underlineColor = r.underlineColor;
    using U = fw::slide::Underline;
    d.underline = r.underline == U::None ? 0 : r.underline == U::Double || r.underline == U::WavyDouble ? 2
                  : r.underline == U::Heavy || r.underline == U::DottedHeavy || r.underline == U::DashHeavy ||
                          r.underline == U::DashLongHeavy || r.underline == U::DotDashHeavy || r.underline == U::DotDotDashHeavy ||
                          r.underline == U::WavyHeavy
                      ? 3
                      : 1;
    d.strike = r.strike == fw::slide::Strike::Single ? 1 : r.strike == fw::slide::Strike::Double ? 2 : 0;
    if (d.underline || d.strike) {
      if (const text::Face* f = fonts.FaceFor(s.family, s.bold, s.italic)) {
        const text::FontMetrics fm = f->Metrics(s.size);
        d.ulPos = fm.underlinePosition;
        d.ulThick = std::max(fm.underlineThickness, 0.25f);
        d.stPos = fm.strikethroughPosition;
        d.stThick = std::max(fm.strikethroughThickness, 0.25f);
      } else {
        d.ulPos = s.size / 10;
        d.ulThick = d.stThick = s.size / 18;
        d.stPos = s.size / 3;
      }
      if (d.underline == 3) d.ulThick *= 2;
    }
    return d;
  };

  size_t chars = 0;
  float y = 0, maxBottom = 0;
  int column = 0;
  bool first = true;
  for (const fw::slide::Paragraph& src : body.paragraphs) {
    const fw::slide::ParagraphProps& pp = src.props;
    Impl::Para& para = m.paras.emplace_back();
    para.empty = src.runs.empty();
    para.mark = styleOf(src.endProps);
    std::vector<text::TextRun> runs;
    auto add = [&](const std::wstring& t, const RunProps& props, const fw::slide::Hyperlink* link) {
      para.runStart.push_back(static_cast<uint32_t>(para.text.size()));
      para.text += t;
      text::TextStyle& st = para.styles.emplace_back(styleOf(props));
      if (!t.empty() && t.front() == L'￼') {
        st.objectWidth = st.size;
        st.objectAscent = st.size * 0.8f;
        st.objectDescent = st.size * 0.2f;
      }
      para.runs.push_back(runDraw(props, st));
      if (link) {
        para.runs.back().link = static_cast<int>(m.links.size());
        m.links.push_back({m.paras.size() - 1, para.runStart.back(), static_cast<uint32_t>(para.text.size()), *link});
      }
      runs.push_back({static_cast<uint32_t>(t.size()), &st});
    };
    // The bullet: a character, a number or a picture, then a tab to the text. Its font, size and
    // color follow the first run unless the paragraph says otherwise.
    const bool bulletShown = !para.empty || static_cast<int>(m.paras.size() - 1) == extras.bulletEmptyParagraph;
    if (bulletShown && pp.bullet.kind != Bullet::Kind::None) {
      RunProps bp = para.empty ? src.endProps : src.runs.front().props;
      const Bullet& b = pp.bullet;
      if (b.color) {
        bp.color = *b.color;
        bp.noFill = false;
      }
      bp.size = b.sizePoints > 0 ? b.sizePoints : bp.size * b.sizePercent;
      bp.underline = fw::slide::Underline::None;
      bp.strike = fw::slide::Strike::None;
      bp.baseline = bp.spacing = 0;
      bp.highlight.reset();
      std::wstring t;
      if (b.kind == Bullet::Kind::Char) {
        if (!b.font.empty()) bp.latin = bp.eastAsian = bp.complexScript = b.font;
        t = b.character;
        if (IsSymbolFont(bp.latin))
          for (wchar_t& c : t)
            if (c >= 0x20 && c < 0x100) c = static_cast<wchar_t>(0xF000 + c);
      } else if (b.kind == Bullet::Kind::AutoNumber) {
        if (!b.font.empty()) bp.latin = b.font;
        t = AutoNumber(b.scheme, std::max(src.number, 1));
      } else if (b.kind == Bullet::Kind::Picture && extras.picture) {
        para.picture = extras.picture(b.picturePart, b.pictureRel);
        if (para.picture) t = L"￼";
      }
      if (!t.empty()) {
        add(t + L'\t', bp, nullptr);
        para.bullet = true;
        para.first = static_cast<uint32_t>(para.text.size());
      }
    }
    for (const fw::slide::TextRun& r : src.runs) {
      std::wstring t = r.kind == fw::slide::TextRun::Kind::Break ? std::wstring(L"\u000B") : r.text;
      // Line feeds in a run break lines as a:br does; other control characters show as spaces.
      for (wchar_t& c : t)
        if (c == L'\n' || c == L'\r') c = L'\u000B';
        else if (c < 0x20 && c != L'\t' && c != L'\u000B') c = L' ';
      if (t.empty() || chars >= kMaxChars) continue;
      chars += t.size();
      RunProps props = r.props;
      const fw::slide::Hyperlink* link = r.props.click ? &r.props.click : nullptr;
      if (link && !LinkKeepsTextColor(r.node)) {
        if (std::optional<draw::Color> c = draw::SchemeColor("hlink", extras.colors)) {
          props.color = *c;
          props.noFill = false;
        }
        if (props.underline == fw::slide::Underline::None) props.underline = fw::slide::Underline::Single;
      }
      add(t, props, link);
    }

    text::Paragraph tp;
    tp.text = para.text;
    tp.runs = runs;
    tp.markStyle = &para.mark;
    text::ParagraphFormat& f = tp.format;
    using text::Alignment;
    Alignment a = pp.align == TextAlign::Center                                      ? Alignment::Center
                  : pp.align == TextAlign::Right                                     ? Alignment::End
                  : pp.align == TextAlign::Justify || pp.align == TextAlign::JustifyLow ? Alignment::Justify
                  : pp.align == TextAlign::Distributed || pp.align == TextAlign::ThaiDistributed ? Alignment::Distribute
                                                                                     : Alignment::Start;
    // DrawingML's alignment is visual (left, right); fwtext's logical.
    if (pp.rtl && (a == Alignment::Start || a == Alignment::End)) a = a == Alignment::Start ? Alignment::End : Alignment::Start;
    f.rightToLeft = pp.rtl;
    f.alignment = box.wrap ? a : (pp.rtl ? Alignment::End : Alignment::Start);
    f.startIndent = pp.marginLeft;
    f.endIndent = pp.marginRight;
    f.firstLineIndent = pp.indent;
    for (const fw::slide::TabStop& t : pp.tabs) {
      text::TabStop ts;
      ts.position = t.position;
      ts.align = t.kind == fw::slide::TabStop::Kind::Center    ? text::TabAlign::Center
                 : t.kind == fw::slide::TabStop::Kind::Right   ? text::TabAlign::End
                 : t.kind == fw::slide::TabStop::Kind::Decimal ? text::TabAlign::Decimal
                                                               : text::TabAlign::Start;
      f.tabs.push_back(ts);
    }
    f.defaultTabStop = std::max(1.f, pp.defaultTab);
    f.squeezeSpaces = false;
    f.hangPunctuation = pp.hangingPunctuation;
    para.lines = text::Layout(fonts, tp, layoutW);

    // Vertical placement. Percent line spacing scales each line's height (its fonts' ascent,
    // descent and gap), less the stored autofit's reduction; paragraph spacing in percent is of
    // the first line's largest font.
    // The bullet (run 0) doesn't count: a bullet at 150 % leaves its line as high (PowerPoint's PDFs).
    const auto counts = [&](uint32_t run) { return !para.bullet || run != 0; };
    float largest = 0;
    if (!para.lines.empty())
      for (const text::GlyphPiece& g : para.lines.front().pieces)
        if (counts(g.run)) largest = std::max(largest, g.emSize);
    if (largest <= 0) largest = para.mark.size;
    auto spacing = [&](const fw::slide::Spacing& s) { return s.percent ? s.value * largest * 1.2f : s.value * fontScale; };
    if (!first) y += spacing(pp.spaceBefore);
    const float lineFactor = pp.lineSpacing.percent ? std::max(0.f, pp.lineSpacing.value - lineReduction) : 0.f;
    for (const text::Line& line : para.lines) {
      // PowerPoint's single line is 1.2 times the line's largest font size, whatever the font
      // (its PDFs: 21.6 pt for Calibri 18, whose own ascent and descent make 21.97), with the
      // baseline at the font's ascent's share of it.
      float size = 0;
      for (const text::GlyphPiece& g : line.pieces)
        if (counts(g.run)) size = std::max(size, g.emSize);
      for (const text::InlineObject& o : line.objects)
        if (counts(o.run)) size = std::max(size, o.ascent + o.descent);
      if (size <= 0) size = para.mark.size;
      const float natural = 1.2f * size;
      const float pitch = pp.lineSpacing.percent ? natural * lineFactor : pp.lineSpacing.value * fontScale;
      const float above = line.ascent + line.descent > 0 ? pitch * line.ascent / (line.ascent + line.descent) : 0;
      float shift = 0;
      if (!box.wrap) {
        // Lines that don't wrap are aligned in the box as wide as they are.
        const float left = pp.marginLeft, right = boxW - pp.marginRight;
        if (a == Alignment::Center) shift = (left + right) / 2 - (line.contentLeft + line.contentRight) / 2;
        else if (a == Alignment::End) shift = right - line.contentRight;
      }
      para.origins.push_back({shift, y + above});
      para.rows.emplace_back(y, y + pitch);
      y += pitch;
      maxBottom = std::max(maxBottom, y);
    }
    y += spacing(pp.spaceAfter);
    first = false;
  }
  // The block's height: down to the last line (the last paragraph's space after doesn't count).
  m.height = maxBottom;
  // Columns: lines go down a column as far as the box, then on to the next; text too long for
  // them all is shared out evenly, as PowerPoint does.
  if (columns > 1 && maxBottom > 0) {
    const float room = maxBottom > boxH * columns ? maxBottom / columns : boxH;
    float start = 0, height = 0;
    for (Impl::Para& p : m.paras)
      for (size_t i = 0; i < p.rows.size(); ++i) {
        if (column + 1 < columns && p.rows[i].second - start > room + 0.01f && p.rows[i].first > start) {
          ++column;
          start = p.rows[i].first;
        }
        p.origins[i].x += column * (colW + colGap);
        p.origins[i].y -= start;
        p.rows[i].first -= start;
        p.rows[i].second -= start;
        height = std::max(height, p.rows[i].second);
      }
    m.height = height;
  }

  // Anchoring, then text coordinates (x along the lines from the text area's top left) to the box's.
  float dy = 0;
  if (box.anchor == draw::TextBox::Anchor::Bottom) dy = boxH - m.height;
  else if (box.anchor != draw::TextBox::Anchor::Top) dy = (boxH - m.height) / 2;
  float dx = 0;
  if (box.anchorCenter) {
    float lo = 1e9f, hi = -1e9f;
    for (const Impl::Para& p : m.paras)
      for (size_t i = 0; i < p.lines.size(); ++i) {
        lo = std::min(lo, p.origins[i].x + p.lines[i].contentLeft);
        hi = std::max(hi, p.origins[i].x + p.lines[i].contentRight);
      }
    if (lo <= hi) dx = (boxW - (hi - lo)) / 2 - lo;
  }
  const draw::Rect& r = box.rect;
  draw::Matrix t = draw::Matrix::Translate(dx, dy);
  if (box.flow == draw::TextBox::Flow::Vertical270) t = t * draw::Matrix{0, -1, 1, 0, r.left, r.bottom};
  else if (vertical) t = t * draw::Matrix{0, 1, -1, 0, r.right, r.top};
  else t = t * draw::Matrix::Translate(r.left, r.top);
  if (box.rotation != 0) {
    const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    t = t * draw::Matrix::Translate(-cx, -cy) * draw::Matrix::Rotate(box.rotation) * draw::Matrix::Translate(cx, cy);
  }
  m.toBox = t;
}

void TextBlock::Draw(draw::Painter& p, DrawFilter* filter, uint32_t shape) const {
  const Impl& m = *impl_;
  if (m.paras.empty()) return;
  p.Save();
  // PowerPoint doesn't mirror text with its shape: a horizontal flip leaves it as it is, a
  // vertical one turns it upside down.
  const draw::Matrix ct = p.CurrentTransform();
  if (ct.m11 * ct.m22 - ct.m12 * ct.m21 < 0) {
    const float cx = (m.rect.left + m.rect.right) / 2;
    p.Transform({-1, 0, 0, 1, 2 * cx, 0});
  }
  p.Transform(m.toBox);
  const bool wantText = p.WantsText();
  for (size_t pi = 0; pi < m.paras.size(); ++pi) {
    const Impl::Para& para = m.paras[pi];
    if (filter && !filter->Paragraph(shape, static_cast<int>(pi))) continue;
    for (size_t li = 0; li < para.lines.size(); ++li) {
      const text::Line& line = para.lines[li];
      const draw::Point o = para.origins[li];
      for (size_t ri = 0; ri < para.runs.size(); ++ri) {
        if (!para.runs[ri].highlight) continue;
        const uint32_t s = para.runStart[ri];
        const uint32_t e = ri + 1 < para.runStart.size() ? para.runStart[ri + 1] : static_cast<uint32_t>(para.text.size());
        for (const auto& [x0, x1] : line.Ranges(s, e))
          p.FillRect({o.x + x0, para.rows[li].first, o.x + x1, para.rows[li].second}, draw::Brush::Solid(*para.runs[ri].highlight));
      }
      for (const text::GlyphPiece& g : line.pieces) {
        if (g.glyphs.empty() || g.run >= para.runs.size()) continue;
        const RunDraw& rd = para.runs[g.run];
        if (!rd.color && !rd.outline) continue;
        const draw::Brush brush = draw::Brush::Solid(rd.color ? *rd.color : *rd.outline);
        draw::GlyphRun run = g.Run();
        if (wantText && g.textEnd > g.textStart && g.textEnd <= para.text.size()) {
          run.text = reinterpret_cast<const char16_t*>(para.text.data() + g.textStart);
          run.textLength = g.textEnd - g.textStart;
        }
        const float baseline = o.y - g.rise;
        if (rd.color) p.DrawGlyphs({o.x + g.OriginX(), baseline}, run, brush);
        if (rd.outline) {
          // Outlined text: the glyphs' outlines stroked, over their fill.
          draw::Pen pen;
          pen.brush = draw::Brush::Solid(*rd.outline);
          pen.width = rd.outlineWidth;
          pen.join = draw::LineJoin::Round;
          p.Save();
          p.Transform(draw::Matrix::Translate(o.x + g.OriginX(), baseline));
          p.StrokePath(draw::GlyphRunOutline(run), pen);
          p.Restore();
        }
        if (rd.underline && !g.leader) {
          const draw::Brush ub = rd.underlineColor ? draw::Brush::Solid(*rd.underlineColor) : brush;
          const float top = o.y + rd.ulPos - rd.ulThick / 2;
          p.FillRect({o.x + g.x, top, o.x + g.x + g.width, top + rd.ulThick}, ub);
          if (rd.underline == 2) p.FillRect({o.x + g.x, top + 2 * rd.ulThick, o.x + g.x + g.width, top + 3 * rd.ulThick}, ub);
        }
        if (rd.strike) {
          const float top = baseline - rd.stPos - rd.stThick / 2;
          p.FillRect({o.x + g.x, top, o.x + g.x + g.width, top + rd.stThick}, brush);
          if (rd.strike == 2) p.FillRect({o.x + g.x, top - 2 * rd.stThick, o.x + g.x + g.width, top - rd.stThick}, brush);
        }
      }
      if (para.picture && para.bullet)
        for (const text::InlineObject& obj : line.objects) {
          if (obj.run != 0) continue;
          if (std::shared_ptr<const draw::Bitmap> px = para.picture->Pixels(64, 64))
            p.DrawBitmap(*px, {o.x + obj.x, o.y - obj.ascent, o.x + obj.x + obj.width, o.y + obj.descent});
        }
    }
  }
  p.Restore();
}

float TextBlock::ContentHeight() const { return impl_->height; }

namespace {
draw::Rect Transformed(const draw::Matrix& m, const draw::Rect& r) {
  const draw::Point pts[4] = {m.Apply({r.left, r.top}), m.Apply({r.right, r.top}), m.Apply({r.left, r.bottom}),
                              m.Apply({r.right, r.bottom})};
  draw::Rect out{pts[0].x, pts[0].y, pts[0].x, pts[0].y};
  for (const draw::Point& q : pts)
    out = {std::min(out.left, q.x), std::min(out.top, q.y), std::max(out.right, q.x), std::max(out.bottom, q.y)};
  return out;
}
}  // namespace

std::vector<draw::Rect> TextBlock::ParagraphBounds() const {
  std::vector<draw::Rect> out;
  for (const Impl::Para& p : impl_->paras) {
    draw::Rect r{};
    for (size_t i = 0; i < p.lines.size(); ++i) {
      const draw::Rect line{p.origins[i].x + p.lines[i].contentLeft, p.rows[i].first, p.origins[i].x + p.lines[i].contentRight,
                            p.rows[i].second};
      r = i == 0 ? line : draw::Rect{std::min(r.left, line.left), std::min(r.top, line.top), std::max(r.right, line.right),
                                     std::max(r.bottom, line.bottom)};
    }
    out.push_back(p.lines.empty() ? draw::Rect{} : Transformed(impl_->toBox, r));
  }
  return out;
}

std::vector<TextBlock::Link> TextBlock::Links() const {
  std::vector<Link> out;
  for (const Impl::LinkRun& l : impl_->links) {
    const Impl::Para& p = impl_->paras[l.para];
    for (size_t i = 0; i < p.lines.size(); ++i)
      for (const auto& [x0, x1] : p.lines[i].Ranges(l.start, l.end))
        out.push_back({Transformed(impl_->toBox, {p.origins[i].x + x0, p.rows[i].first, p.origins[i].x + x1, p.rows[i].second}), l.link});
  }
  return out;
}

// ---------------------------------------------------------------------------
// Editing (P2): carets, hit testing and selection over the laid-out lines.

draw::Matrix TextBlock::ToSlide(const draw::Matrix& boxToSlide) const {
  const Impl& m = *impl_;
  draw::Matrix t = m.toBox;
  if (boxToSlide.m11 * boxToSlide.m22 - boxToSlide.m12 * boxToSlide.m21 < 0) {
    const float cx = (m.rect.left + m.rect.right) / 2;
    t = t * draw::Matrix{-1, 0, 0, 1, 2 * cx, 0};
  }
  return t * boxToSlide;
}

size_t TextBlock::ParagraphCount() const { return impl_->paras.size(); }

uint32_t TextBlock::ParagraphLength(size_t para) const {
  if (para >= impl_->paras.size()) return 0;
  const Impl::Para& p = impl_->paras[para];
  return static_cast<uint32_t>(p.text.size()) - p.first;
}

namespace {

// The line of `lines` holding text offset `t`: at a boundary the next line, unless `lineEnd` and the
// line above wrapped there.
size_t LineOf(const std::vector<text::Line>& lines, uint32_t t, bool lineEnd) {
  if (lines.empty()) return 0;
  for (size_t i = 0; i < lines.size(); ++i) {
    const text::Line& l = lines[i];
    if (t < l.end) return i;
    if (t == l.end) {
      if (i + 1 == lines.size()) return i;
      if (lineEnd && l.ending == text::LineEnd::Wrap) return i;
    }
  }
  return lines.size() - 1;
}

}  // namespace

TextBlock::Caret TextBlock::CaretAt(size_t para, uint32_t offset, bool lineEnd) const {
  const Impl& m = *impl_;
  Caret c;
  if (m.paras.empty()) return c;
  para = std::min(para, m.paras.size() - 1);
  const Impl::Para& p = m.paras[para];
  const uint32_t t = std::min<uint32_t>(p.first + offset, static_cast<uint32_t>(p.text.size()));
  if (p.lines.empty()) return c;
  const size_t li = LineOf(p.lines, t, lineEnd);
  const text::Line& line = p.lines[li];
  c.x = p.origins[li].x + line.CaretX(std::clamp(t, line.start, line.end));
  c.top = p.rows[li].first;
  c.bottom = p.rows[li].second;
  return c;
}

namespace {

// The offset nearest x on a line, kept off the bullet and before a line break's character.
uint32_t OffsetOnLine(const text::Line& line, float x, uint32_t first, const std::wstring& text, bool& lineEnd) {
  uint32_t t = std::clamp(line.HitTest(x), line.start, line.end);
  lineEnd = false;
  if (t == line.end && line.end > line.start) {
    if (line.ending == text::LineEnd::LineBreak && line.end - 1 < text.size() && text[line.end - 1] == L'\u000B') t = line.end - 1;
    else if (line.ending == text::LineEnd::Wrap) lineEnd = true;
  }
  return std::max(t, first);
}

}  // namespace

TextBlock::Hit TextBlock::HitTest(draw::Point pt) const {
  const Impl& m = *impl_;
  Hit h;
  if (m.paras.empty()) return h;
  // The line whose row holds y; above the first, the first; below the last, the last; between
  // paragraphs (their spacing), the nearer.
  size_t bestPara = 0, bestLine = 0;
  float best = 1e30f;
  for (size_t pi = 0; pi < m.paras.size(); ++pi) {
    const Impl::Para& p = m.paras[pi];
    for (size_t li = 0; li < p.rows.size(); ++li) {
      // Columns: only lines of the column under x count, unless none does.
      const float left = p.origins[li].x + std::min(p.lines[li].left, p.lines[li].contentLeft);
      const float right = p.origins[li].x + std::max(p.lines[li].right, p.lines[li].contentRight);
      const float dx = pt.x < left ? left - pt.x : pt.x > right ? pt.x - right : 0;
      const float dy = pt.y < p.rows[li].first ? p.rows[li].first - pt.y : pt.y > p.rows[li].second ? pt.y - p.rows[li].second : 0;
      const float d = dy + dx * 4;  // across columns, x decides
      if (d < best) {
        best = d;
        bestPara = pi;
        bestLine = li;
      }
    }
  }
  const Impl::Para& p = m.paras[bestPara];
  h.para = bestPara;
  if (p.lines.empty()) return h;
  const uint32_t t = OffsetOnLine(p.lines[bestLine], pt.x - p.origins[bestLine].x, p.first, p.text, h.lineEnd);
  h.offset = t - p.first;
  return h;
}

std::pair<uint32_t, uint32_t> TextBlock::LineRange(size_t para, uint32_t offset, bool lineEnd) const {
  const Impl& m = *impl_;
  if (para >= m.paras.size()) return {0, 0};
  const Impl::Para& p = m.paras[para];
  if (p.lines.empty()) return {0, 0};
  const uint32_t t = std::min<uint32_t>(p.first + offset, static_cast<uint32_t>(p.text.size()));
  const text::Line& line = p.lines[LineOf(p.lines, t, lineEnd)];
  uint32_t end = line.end;
  if (line.ending == text::LineEnd::LineBreak && end > line.start && end - 1 < p.text.size() && p.text[end - 1] == L'\u000B') --end;
  const uint32_t start = std::max(line.start, p.first);
  return {start - p.first, std::max(end, start) - p.first};
}

bool TextBlock::Vertical(size_t para, uint32_t offset, bool lineEnd, int direction, float x, Hit& out) const {
  const Impl& m = *impl_;
  if (para >= m.paras.size() || m.paras[para].lines.empty()) return false;
  const Impl::Para& p = m.paras[para];
  const uint32_t t = std::min<uint32_t>(p.first + offset, static_cast<uint32_t>(p.text.size()));
  size_t pi = para, li = LineOf(p.lines, t, lineEnd);
  if (direction < 0) {
    if (li > 0) {
      --li;
    } else {
      do {
        if (pi == 0) return false;
        --pi;
      } while (m.paras[pi].lines.empty());
      li = m.paras[pi].lines.size() - 1;
    }
  } else {
    if (li + 1 < p.lines.size()) {
      ++li;
    } else {
      do {
        if (pi + 1 >= m.paras.size()) return false;
        ++pi;
      } while (m.paras[pi].lines.empty());
      li = 0;
    }
  }
  const Impl::Para& q = m.paras[pi];
  out.para = pi;
  out.offset = OffsetOnLine(q.lines[li], x - q.origins[li].x, q.first, q.text, out.lineEnd) - q.first;
  return true;
}

bool TextBlock::IsStop(size_t para, uint32_t offset) const {
  const Impl& m = *impl_;
  if (para >= m.paras.size()) return true;
  const Impl::Para& p = m.paras[para];
  const uint32_t t = p.first + offset;
  if (t >= p.text.size()) return true;
  for (const text::Line& l : p.lines)
    if (t >= l.start && t < l.end) return l.IsCaretStop(t);
  return true;
}

std::vector<draw::Rect> TextBlock::SelectionRects(size_t fromPara, uint32_t fromOffset, size_t toPara, uint32_t toOffset) const {
  const Impl& m = *impl_;
  std::vector<draw::Rect> out;
  for (size_t pi = fromPara; pi <= toPara && pi < m.paras.size(); ++pi) {
    const Impl::Para& p = m.paras[pi];
    const uint32_t len = static_cast<uint32_t>(p.text.size());
    const uint32_t s = pi == fromPara ? std::min(len, p.first + fromOffset) : p.first;
    const uint32_t e = pi == toPara ? std::min(len, p.first + toOffset) : len;
    for (size_t li = 0; li < p.lines.size(); ++li) {
      const text::Line& l = p.lines[li];
      const uint32_t a = std::max(s, l.start), b = std::min(e, l.end);
      const float top = p.rows[li].first, bottom = p.rows[li].second, ox = p.origins[li].x;
      if (a < b)
        for (const auto& [x0, x1] : l.Ranges(a, b)) out.push_back({ox + x0, top, ox + x1, bottom});
      // The paragraph's mark, when the selection goes on past it: a space's width.
      if (pi < toPara && li + 1 == p.lines.size()) {
        const float x = ox + l.CaretX(l.end);
        const float w = std::max(2.f, 0.25f * p.mark.size);
        out.push_back({x, top, x + w, bottom});
      }
    }
  }
  return out;
}

std::wstring TextBlock::PlainText() const {
  std::wstring out;
  for (const Impl::Para& p : impl_->paras) {
    if (!out.empty()) out += L'\n';
    const size_t from = p.bullet && p.runStart.size() > 1 ? p.runStart[1] : 0;
    for (size_t i = from; i < p.text.size(); ++i) out += p.text[i] == L'\u000B' ? L'\n' : p.text[i];
  }
  return out;
}

}  // namespace fw::slide::render
