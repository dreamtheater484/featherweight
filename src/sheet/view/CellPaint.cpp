#include "sheet/view/CellPaint.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "sheet/condfmt/CondFormat.h"
#include "sheet/model/TableStyles.h"

namespace fw::sheet::view {

draw::Color ToColor(uint32_t argb) {
  return {((argb >> 16) & 0xFF) / 255.f, ((argb >> 8) & 0xFF) / 255.f, (argb & 0xFF) / 255.f, ((argb >> 24) & 0xFF) / 255.f};
}

namespace {

constexpr uint32_t kOverflowReach = 64;  // columns a text may run into beside the drawn ones

draw::Color Mix(uint32_t a, uint32_t b, float t) {
  const draw::Color x = ToColor(a), y = ToColor(b);
  return {x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, 1};
}

// How much of a pattern's foreground shows (Excel's 8x8 patterns), for drawing it as a tint.
float PatternShare(PatternType t) {
  switch (t) {
    case PatternType::Solid: return 1;
    case PatternType::DarkGray: return 0.75f;
    case PatternType::MediumGray: return 0.5f;
    case PatternType::LightGray: return 0.25f;
    case PatternType::Gray125: return 0.125f;
    case PatternType::Gray0625: return 0.0625f;
    case PatternType::DarkHorizontal:
    case PatternType::DarkVertical:
    case PatternType::DarkDown:
    case PatternType::DarkUp:
    case PatternType::DarkGrid:
    case PatternType::DarkTrellis: return 0.5f;
    default: return 0.25f;
  }
}

// A border style's lines: widths in Excel pixels and the dash pattern (on, off... in pixels).
struct LineKind {
  int width = 1;
  bool doubled = false;
  std::vector<int> dashes;
  int weight = 0;  // which wins where two meet: the heavier
};

// Where two cells' borders meet on one edge, Excel draws the heavier: thick over double over
// medium and its dashes, over thin and its dashes and dots, over hair.
LineKind KindOf(BorderStyle s) {
  switch (s) {
    case BorderStyle::Hair: return {1, false, {}, 0};
    case BorderStyle::Dotted: return {1, false, {1, 1}, 1};
    case BorderStyle::DashDotDot: return {1, false, {9, 3, 3, 3, 3, 3}, 2};
    case BorderStyle::DashDot: return {1, false, {9, 3, 3, 3}, 3};
    case BorderStyle::Dashed: return {1, false, {3, 1}, 4};
    case BorderStyle::Thin: return {1, false, {}, 5};
    case BorderStyle::MediumDashDotDot: return {2, false, {9, 3, 3, 3, 3, 3}, 6};
    case BorderStyle::SlantDashDot: return {2, false, {9, 1, 3, 1}, 7};
    case BorderStyle::MediumDashDot: return {2, false, {9, 3, 3, 3}, 8};
    case BorderStyle::MediumDashed: return {2, false, {9, 3}, 9};
    case BorderStyle::Medium: return {2, false, {}, 10};
    case BorderStyle::Double: return {1, true, {}, 11};
    case BorderStyle::Thick: return {3, false, {}, 12};
    default: return {0, false, {}, -1};
  }
}

constexpr float kPrintHair = 0.2f;  // points: printed gridlines and hair borders

struct Segment {
  float a0, a1;   // along the line
  float b;        // the boundary across it (the gridline's far edge)
  bool vertical;
  BorderStyle style;
  uint32_t color;
  uint64_t edge;  // the cell edge it is on: the same for a cell's bottom and the top of the one below
};

struct RowData {
  uint32_t row;
  uint32_t style = kNone;  // the row's format (customFormat), else kNone
  std::vector<CellInfo> cells;  // stored cells, by column
  std::vector<std::pair<uint32_t, uint32_t>> spans;  // text running across columns: first, last
  const CellInfo* Find(uint32_t col) const {
    auto it = std::lower_bound(cells.begin(), cells.end(), col, [](const CellInfo& c, uint32_t x) { return c.col < x; });
    return it != cells.end() && it->col == col ? &*it : nullptr;
  }
  bool SpanCrosses(uint32_t a, uint32_t b) const {  // the boundary between columns a and b = a + 1
    for (const auto& s : spans)
      if (s.first <= a && b <= s.second) return true;
    return false;
  }
};

class Painting {
 public:
  Painting(draw::Painter& p, const Sheet& sheet, CellText& text, const Grid& grid, const Geometry& g, const Range& area,
           const PaintOptions& o)
      : p_(p), sheet_(sheet), text_(text), grid_(grid), g_(g), area_(area), o_(o),
        styles_(text.Book().GetStyles()) {}

  void Run() {
    for (uint32_t r = area_.firstRow; r <= area_.lastRow && r < kMaxRows; ++r)
      if (!grid_.RowHidden(r)) rows_.push_back({r});
    for (uint32_t c = area_.firstCol; c <= area_.lastCol && c < kMaxCols; ++c)
      if (!grid_.ColHidden(c)) cols_.push_back(c);
    if (rows_.empty() || cols_.empty()) return;
    for (const Range& m : sheet_.Merges())
      if (m.lastRow >= area_.firstRow && m.firstRow <= area_.lastRow && m.lastCol >= area_.firstCol &&
          m.firstCol <= area_.lastCol)
        merges_.push_back(&m);
    reachFirst_ = area_.firstCol > kOverflowReach ? area_.firstCol - kOverflowReach : 0;
    reachLast_ = std::min(area_.lastCol + kOverflowReach, kMaxCols - 1);
    size_t at = 0;
    sheet_.ForEachCell(Range{rows_.front().row, reachFirst_, rows_.back().row, reachLast_}, [&](const CellInfo& c) {
      while (at < rows_.size() && rows_[at].row < c.row) ++at;
      if (at < rows_.size() && rows_[at].row == c.row) rows_[at].cells.push_back(c);
      return true;
    });
    for (RowData& rd : rows_) {
      const RowInfo info = sheet_.Row(rd.row);
      if (info.customFormat) rd.style = info.style;
    }
    colStyle_.resize(cols_.size());
    for (size_t i = 0; i < cols_.size(); ++i) colStyle_[i] = sheet_.Column(cols_[i]).style;

    PlaceText();
    if (o_.gridlines) Gridlines();
    Fills();
    BarsAndIcons();
    Borders();
    DrawText();
  }

 private:
  // ---- Tables and conditional formats
  struct Look {
    TableCellFormat table;  // under the cell's own format
    bool cf = false;        // a conditional format holds:
    condfmt::Style st;      // over it
  };
  bool Looks() const { return o_.rules || (o_.tables && !sheet_.Tables().empty()); }
  const Look& LookAt(uint32_t row, uint32_t col) {
    const uint64_t key = (uint64_t{row} << 15) | col;
    auto it = looks_.find(key);
    if (it != looks_.end()) return it->second;
    Look& lk = looks_[key];
    if (o_.tables && !sheet_.Tables().empty()) lk.table = TableFormatAt(styles_, sheet_, row, col);
    if (o_.rules) lk.cf = o_.rules->At(row, col, lk.st);
    return lk;
  }
  bool HideValue(uint32_t row, uint32_t col) {
    if (!o_.rules) return false;
    const Look& lk = LookAt(row, col);
    return lk.cf && (lk.st.set & condfmt::Style::kHideValue);
  }
  TextLook TextLookAt(uint32_t row, uint32_t col, const Xf& xf) {
    TextLook t;
    if (!Looks()) return t;
    const Look& lk = LookAt(row, col);
    if (!lk.table.Empty()) {
      // Bold and italic add to the cell's; the color shows where its font has the default's.
      const Font& f = styles_.FontOf(xf);
      if (lk.table.bold && !f.bold) t.bold = 1;
      if (lk.table.italic && !f.italic) t.italic = 1;
      const uint32_t def = styles_.Resolve(styles_.DefaultFont().color, 0xFF000000);
      if (lk.table.fontColor.IsSet() && (!f.color.IsSet() || styles_.Resolve(f.color, 0xFF000000) == def))
        t.fontColor = styles_.Resolve(lk.table.fontColor, 0xFF000000);
    }
    if (lk.cf) {
      const condfmt::Style& s = lk.st;
      if (s.set & condfmt::Style::kFontColor) {
        t.fontColor = s.fontColor;
        t.colorWins = true;
      }
      if (s.set & condfmt::Style::kBold) t.bold = s.bold ? 1 : 0;
      if (s.set & condfmt::Style::kItalic) t.italic = s.italic ? 1 : 0;
      if (s.set & condfmt::Style::kStrike) t.strike = s.strike ? 1 : 0;
      if (s.set & condfmt::Style::kUnderline) t.underline = static_cast<int8_t>(s.underline);
      if (s.set & condfmt::Style::kNumFmt) t.numFmt = s.numFmt;
    }
    return t;
  }
  // A cell's fill: a conditional format's, else its own, else its table's.
  void FillCell(const draw::Rect& r, uint32_t row, uint32_t col, const Fill& own) {
    const bool has = own.pattern != PatternType::None || own.gradient;
    if (Looks()) {
      const Look& lk = LookAt(row, col);
      if (lk.cf && (lk.st.set & condfmt::Style::kFill)) {
        const condfmt::Style& s = lk.st;
        if (s.gradient) FillRect(r, *s.gradient);
        else if (s.pattern == PatternType::Solid || (s.pattern == PatternType::None && s.fillColor))
          p_.FillRect(r, draw::Brush::Solid(ToColor(s.fillColor)));
        else if (s.pattern != PatternType::None)
          p_.FillRect(r, draw::Brush::Solid(Mix(s.fillColor ? s.fillColor : 0xFFFFFFFF, s.patternColor, PatternShare(s.pattern))));
        return;
      }
      if (!has && (lk.table.fill.pattern != PatternType::None || lk.table.fill.gradient)) {
        FillRect(r, lk.table.fill);
        return;
      }
    }
    if (has) FillRect(r, own);
  }
  // A cell's borders: its own, its table's where it has none, a conditional format's over both.
  Border BorderAt(uint32_t row, uint32_t col, const Border& own) {
    if (!Looks()) return own;
    const Look& lk = LookAt(row, col);
    Border b = own;
    const bool cf = lk.cf && (lk.st.set & condfmt::Style::kBorder);
    auto side = [&](BorderSide& s, const BorderSide& table, const condfmt::Style::Side& over) {
      if (s.style == BorderStyle::None && table.style != BorderStyle::None) s = table;
      if (cf && over.style != BorderStyle::None) {
        s.style = over.style;
        s.color = Color{Color::Kind::Rgb, over.color ? over.color : 0xFF000000, 0};
      }
    };
    side(b.left, lk.table.left, lk.st.left);
    side(b.right, lk.table.right, lk.st.right);
    side(b.top, lk.table.top, lk.st.top);
    side(b.bottom, lk.table.bottom, lk.st.bottom);
    return b;
  }

  // ---- Data bars and icons (conditional formats): over the fills, under the borders and text.
  void BarsAndIcons() {
    if (!o_.rules) return;
    for (const RowData& rd : rows_)
      for (uint32_t col : cols_) {
        if (MergeAt(rd.row, col)) continue;
        const Look& lk = LookAt(rd.row, col);
        if (!lk.cf) continue;
        const draw::Rect box = Inner(rd.row, col, rd.row, col);
        if (lk.st.set & condfmt::Style::kBar) DrawBar(box, lk.st.bar);
        if (lk.st.set & condfmt::Style::kIcon) DrawIcon(box, lk.st.iconSet, lk.st.icon);
      }
  }

  void DrawBar(const draw::Rect& box, const condfmt::DataBar& bar) {
    const float u = g_.px;
    const float l = box.left + 2 * u, r = box.right - 2 * u, t = box.top + 2 * u, b = box.bottom - u;
    if (r <= l || b <= t) return;
    const float w = r - l;
    auto at = [&](float f) { return bar.rightToLeft ? r - f * w : l + f * w; };
    float x0 = at(bar.from), x1 = at(bar.to);
    // The axis end: where the bar starts from.
    const float base = x0;
    if (x1 < x0) std::swap(x0, x1);
    if (x1 > x0) {
      const draw::Rect br{x0, t, x1, b};
      if (bar.gradient) {
        const float end = base == x0 ? x1 : x0;
        p_.FillRect(br, draw::Brush::Linear({base, t}, {end, t},
                                            {{0, ToColor(bar.fill)}, {1, Mix(bar.fill, 0xFFFFFFFF, 0.9f)}}));
      } else {
        p_.FillRect(br, draw::Brush::Solid(ToColor(bar.fill)));
      }
      if (bar.border) {
        const draw::Color c = ToColor(bar.border);
        const float L = std::max(g_.line * 0.75f, 0.5f);
        Rect(x0, t, x1, t + L, c);
        Rect(x0, b - L, x1, b, c);
        Rect(x0, t, x0 + L, b, c);
        Rect(x1 - L, t, x1, b, c);
      }
    }
    if (bar.axis >= 0) {
      // A dotted line between negative and positive values.
      const float x = at(bar.axis), L = std::max(g_.line * 0.75f, 0.5f);
      const draw::Color c = ToColor(bar.axisColor ? bar.axisColor : 0xFF000000);
      for (float y = box.top; y < box.bottom; y += 2 * u) Rect(x - L / 2, y, x + L / 2, std::min(y + u, box.bottom), c);
    }
  }

  // Excel's icons, drawn in a square: each set's shapes and colors, the lowest values' first. The
  // red, yellow, green, blue, dark gray and the rating's empty bars are the colors of the icons in
  // Excel's printed pages.
  void DrawIcon(const draw::Rect& box, condfmt::IconSet set, int i) {
    using S = condfmt::IconSet;
    constexpr uint32_t red = 0xFFD65532, yellow = 0xFFEAC282, green = 0xFF68A490, gray = 0xFF8C8C8C,
                       black = 0xFF505050, gold = 0xFFF0B323, blue = 0xFF4A7DB1, pink = 0xFFF09A9A;
    char shape = 'o';
    uint32_t color = gray;
    int level = 0;
    const uint32_t ryg[3] = {red, yellow, green};
    switch (set) {
      case S::Arrows3: shape = "v>^"[i % 3]; color = ryg[i % 3]; break;
      case S::ArrowsGray3: shape = "v>^"[i % 3]; break;
      case S::Arrows4: shape = "vn/^"[i % 4]; color = i == 0 ? red : i == 3 ? green : yellow; break;
      case S::ArrowsGray4: shape = "vn/^"[i % 4]; break;
      case S::Arrows5: shape = "vn>/^"[i % 5]; color = i == 0 ? red : i == 4 ? green : yellow; break;
      case S::ArrowsGray5: shape = "vn>/^"[i % 5]; break;
      case S::Flags3: shape = 'f'; color = ryg[i % 3]; break;
      case S::TrafficLights3:
      case S::TrafficLights3Rimmed: shape = 'o'; color = ryg[i % 3]; break;
      case S::Signs3: shape = "dAo"[i % 3]; color = ryg[i % 3]; break;
      case S::Symbols3: shape = "x!c"[i % 3]; color = ryg[i % 3]; break;
      case S::Symbols3Plain: shape = "XIC"[i % 3]; color = ryg[i % 3]; break;
      case S::Stars3: shape = '*'; color = gold; level = i; break;
      case S::Triangles3: shape = "V-A"[i % 3]; color = ryg[i % 3]; break;
      case S::RedToBlack4: shape = 'o'; color = i == 0 ? black : i == 1 ? gray : i == 2 ? pink : red; break;
      case S::TrafficLights4: shape = 'o'; color = i == 0 ? black : ryg[(i - 1) % 3]; break;
      case S::Rating4: shape = 'b'; color = blue; level = i + 1; break;
      case S::Rating5: shape = 'b'; color = blue; level = i; break;
      case S::Quarters5: shape = 'q'; color = black; level = i; break;
      case S::Boxes5: shape = 'B'; color = blue; level = i; break;
      default: return;
    }
    const float u = g_.px;
    const float sz = std::min(box.bottom - box.top - 2 * u, 15 * u);
    if (sz <= 2) return;
    const float x = g_.rtl ? box.right - 2 * u - sz : box.left + 2 * u, y = box.bottom - u - sz;
    const draw::Brush ink = draw::Brush::Solid(ToColor(color)), white = draw::Brush::Solid({1, 1, 1, 1});
    const draw::Brush light = draw::Brush::Solid(ToColor(0xFFD9D9D9));
    auto P = [&](float a, float b) { return draw::Point{x + a * sz, y + b * sz}; };
    auto poly = [&](std::initializer_list<std::pair<float, float>> pts, float deg = 0) {
      draw::Path path;
      const double rad = deg * 3.14159265358979 / 180, c = std::cos(rad), s = std::sin(rad);
      bool first = true;
      for (auto [a, b] : pts) {
        const float ra = static_cast<float>(0.5 + (a - 0.5) * c - (b - 0.5) * s);
        const float rb = static_cast<float>(0.5 + (a - 0.5) * s + (b - 0.5) * c);
        if (first) path.MoveTo(P(ra, rb));
        else path.LineTo(P(ra, rb));
        first = false;
      }
      path.Close();
      return path;
    };
    auto stroke = [&](std::initializer_list<std::pair<float, float>> pts, const draw::Brush& b) {
      draw::Path path;
      bool first = true;
      for (auto [a, c] : pts) {
        if (first) path.MoveTo(P(a, c));
        else path.LineTo(P(a, c));
        first = false;
      }
      draw::Pen pen;
      pen.brush = b;
      pen.width = sz * 0.13f;
      pen.cap = draw::LineCap::Round;
      p_.StrokePath(path, pen);
    };
    auto circle = [&](const draw::Brush& b) { p_.FillPath(draw::Path::Ellipse({x, y, x + sz, y + sz}), b); };
    auto arrow = [&](float deg) {
      p_.FillPath(poly({{0.5f, 0.05f}, {0.95f, 0.5f}, {0.68f, 0.5f}, {0.68f, 0.95f}, {0.32f, 0.95f}, {0.32f, 0.5f}, {0.05f, 0.5f}}, deg), ink);
    };
    auto check = [&](const draw::Brush& b) { stroke({{0.27f, 0.52f}, {0.44f, 0.7f}, {0.75f, 0.32f}}, b); };
    auto bang = [&](const draw::Brush& b) {
      p_.FillRect({x + 0.44f * sz, y + 0.18f * sz, x + 0.56f * sz, y + 0.6f * sz}, b);
      p_.FillRect({x + 0.44f * sz, y + 0.7f * sz, x + 0.56f * sz, y + 0.82f * sz}, b);
    };
    auto cross = [&](const draw::Brush& b) {
      stroke({{0.32f, 0.32f}, {0.68f, 0.68f}}, b);
      stroke({{0.68f, 0.32f}, {0.32f, 0.68f}}, b);
    };
    switch (shape) {
      case '^': arrow(0); break;
      case '/': arrow(45); break;
      case '>': arrow(90); break;
      case 'n': arrow(135); break;
      case 'v': arrow(180); break;
      case 'o': {
        // With a darker rim, a 24th of the icon.
        circle(draw::Brush::Solid(Mix(color, 0xFF000000, 0.32f)));
        const float r = sz / 24;
        p_.FillPath(draw::Path::Ellipse({x + r, y + r, x + sz - r, y + sz - r}), ink);
        break;
      }
      case 'f':
        p_.FillRect({x + 0.15f * sz, y + 0.05f * sz, x + 0.24f * sz, y + 0.95f * sz}, draw::Brush::Solid(ToColor(black)));
        p_.FillPath(poly({{0.24f, 0.07f}, {0.9f, 0.3f}, {0.24f, 0.53f}}), ink);
        break;
      case 'd': p_.FillPath(poly({{0.5f, 0.05f}, {0.95f, 0.5f}, {0.5f, 0.95f}, {0.05f, 0.5f}}), ink); break;
      case 'A': p_.FillPath(poly({{0.5f, 0.1f}, {0.95f, 0.88f}, {0.05f, 0.88f}}), ink); break;
      case 'V': p_.FillPath(poly({{0.05f, 0.12f}, {0.95f, 0.12f}, {0.5f, 0.9f}}), ink); break;
      case '-': p_.FillRect({x + 0.15f * sz, y + 0.4f * sz, x + 0.85f * sz, y + 0.6f * sz}, ink); break;
      case 'c': circle(ink); check(white); break;
      case '!': circle(ink); bang(white); break;
      case 'x': circle(ink); cross(white); break;
      case 'C': check(ink); break;
      case 'I': bang(ink); break;
      case 'X': cross(ink); break;
      case '*': {
        std::vector<std::pair<float, float>> pts;
        for (int k = 0; k < 10; ++k) {
          const double a = (k * 36 - 90) * 3.14159265358979 / 180, rr = k % 2 ? 0.2 : 0.48;
          pts.push_back({static_cast<float>(0.5 + rr * std::cos(a)), static_cast<float>(0.54 + rr * std::sin(a))});
        }
        draw::Path star;
        star.MoveTo(P(pts[0].first, pts[0].second));
        for (size_t k = 1; k < pts.size(); ++k) star.LineTo(P(pts[k].first, pts[k].second));
        star.Close();
        if (level >= 2) {
          p_.FillPath(star, ink);
        } else {
          p_.FillPath(star, light);
          if (level == 1) {
            p_.Save();
            p_.ClipRect({x, y, x + sz / 2, y + sz});
            p_.FillPath(star, ink);
            p_.Restore();
          }
        }
        break;
      }
      case 'b':
        for (int k = 0; k < 4; ++k) {
          const float h = 0.25f + 0.2f * k;
          p_.FillRect({x + (0.08f + 0.23f * k) * sz, y + (0.95f - h) * sz, x + (0.26f + 0.23f * k) * sz, y + 0.95f * sz},
                      k < level ? ink : draw::Brush::Solid(ToColor(0xFFB3B3B3)));
        }
        break;
      case 'q': {
        p_.FillPath(draw::Path::Ellipse({x, y, x + sz, y + sz}), ink);
        p_.FillPath(draw::Path::Ellipse({x + 0.1f * sz, y + 0.1f * sz, x + 0.9f * sz, y + 0.9f * sz}), white);
        if (level >= 4) {
          p_.FillPath(draw::Path::Ellipse({x + 0.1f * sz, y + 0.1f * sz, x + 0.9f * sz, y + 0.9f * sz}), ink);
        } else if (level > 0) {
          const double a = (level * 90 - 90) * 3.14159265358979 / 180;
          draw::Path pie;
          pie.MoveTo(P(0.5f, 0.5f));
          pie.LineTo(P(0.5f, 0.1f));
          pie.ArcTo(P(static_cast<float>(0.5 + 0.4 * std::cos(a)), static_cast<float>(0.5 + 0.4 * std::sin(a))), 0.4f * sz,
                    0.4f * sz, 0, level > 2, true);
          pie.Close();
          p_.FillPath(pie, ink);
        }
        break;
      }
      case 'B':
        for (int k = 0; k < 4; ++k) {
          const float bx = (k % 2) * 0.48f + 0.05f, by = (k / 2) * 0.48f + 0.05f;
          p_.FillRect({x + bx * sz, y + by * sz, x + (bx + 0.42f) * sz, y + (by + 0.42f) * sz}, k < level ? ink : light);
        }
        break;
      default: break;
    }
  }

  // ---- Helpers
  const Range* MergeAt(uint32_t r, uint32_t c) const {
    for (const Range* m : merges_)
      if (m->Contains(r, c)) return m;
    return nullptr;
  }
  uint32_t StyleAt(const RowData& rd, size_t colIndex) const {
    if (const CellInfo* c = rd.Find(cols_[colIndex])) return c->style;
    return rd.style != kNone ? rd.style : colStyle_[colIndex];
  }
  // A cell text can run into: no value and not merged.
  bool Empty(const RowData& rd, uint32_t c) const {
    if (c < reachFirst_ || c > reachLast_) return false;
    const CellInfo* x = rd.Find(c);
    if (x && x->value.type != ValueType::Empty) return false;
    return !MergeAt(rd.row, c);
  }
  // The cell's inner box: without its gridline (right in a left-to-right sheet, left otherwise).
  draw::Rect Inner(uint32_t r0, uint32_t c0, uint32_t r1, uint32_t c1) const {
    float l = std::min(g_.Left(c0), g_.Left(c1)), rgt = std::max(g_.Right(c0), g_.Right(c1));
    if (g_.rtl) l += g_.line;
    else rgt -= g_.line;
    return {l, g_.Top(r0), rgt, g_.Bottom(r1) - g_.line};
  }
  // The text's pads at the cell's left and right: the start's at the left in a left-to-right
  // sheet, at the right otherwise.
  float PadL() const { return g_.rtl ? g_.padEnd : g_.padStart; }
  float PadR() const { return g_.rtl ? g_.padStart : g_.padEnd; }
  draw::Rect Outer(uint32_t r0, uint32_t c0, uint32_t r1, uint32_t c1) const {
    return {std::min(g_.Left(c0), g_.Left(c1)), g_.Top(r0), std::max(g_.Right(c0), g_.Right(c1)), g_.Bottom(r1)};
  }
  uint32_t Next(uint32_t c, int visualDir) const {  // the column visually to the right (+1) or left (-1)
    const int step = g_.rtl ? -visualDir : visualDir;
    if (step < 0) return c == 0 ? kNone : c - 1;
    return c + 1 >= kMaxCols ? kNone : c + 1;
  }
  void Rect(float l, float t, float r, float b, const draw::Color& c) {
    if (r > l && b > t) p_.FillRect({l, t, r, b}, draw::Brush::Solid(c));
  }

  // ---- Text: where each cell's text goes, first, because it decides the gridlines it covers.
  struct Item {
    const CellText::Layout* layout;
    draw::Rect box;   // the cell (or merged area) the text is placed in
    draw::Rect clip;
    float x = 0;      // the text's left edge, painter coordinates (wrapped: the wrap box's)
    const Xf* xf;
    float rowDescent = 0;  // pixels: the row's largest font descent, which bottom-aligned text sits on
  };

  void PlaceText() {
    for (RowData& rd : rows_) {
      // Excel puts a row's bottom-aligned text on one baseline: above the descent of its largest
      // font (a 36 pt cell lifts its neighbours' text too).
      rowDescent_ = 0;
      for (const CellInfo& cell : rd.cells)
        if (cell.value.type != ValueType::Empty) rowDescent_ = std::max(rowDescent_, DescentPx(styles_.FontOf(styles_.CellXf(cell.style))));
      for (const CellInfo& cell : rd.cells) {
        if (cell.value.type == ValueType::Empty) continue;
        if (!o_.showZeros && cell.value.type == ValueType::Number && cell.value.number == 0) continue;
        if (MergeAt(cell.row, cell.col)) continue;  // drawn with its merged area
        if (grid_.ColHidden(cell.col)) continue;
        PlaceCell(rd, cell);
      }
    }
    rowDescent_ = 0;
    // Merged areas: the first cell's text in the whole area.
    for (const Range* m : merges_) {
      CellInfo cell;
      if (!sheet_.GetCell(m->firstRow, m->firstCol, cell) || cell.value.type == ValueType::Empty) continue;
      int screenPx = 0;
      for (uint32_t c = m->firstCol; c <= m->lastCol; ++c) screenPx += grid_.ColWidth(c);
      const int widthPx = static_cast<int>(std::lround(screenPx * g_.widthScale));
      if (widthPx == 0 || HideValue(m->firstRow, m->firstCol)) continue;
      const CellText::Layout& l =
          text_.Get(sheet_, cell, widthPx, TextLookAt(m->firstRow, m->firstCol, styles_.CellXf(cell.style)), screenPx);
      const Xf& xf = styles_.CellXf(cell.style);
      Item it{&l, Inner(m->firstRow, m->firstCol, m->lastRow, m->lastCol), {}, 0, &xf};
      it.clip = it.box;
      it.x = AlignX(l, xf, it.box);
      items_.push_back(it);
    }
  }

  // A font's descent in pixels, as bottom-aligned text sits on it: an accounting underline's
  // three pixels below it too.
  float DescentPx(const Font& f) const {
    const bool accounting = f.underline == Underline::SingleAccounting || f.underline == Underline::DoubleAccounting;
    return static_cast<float>(text_.Measure().Of(f, styles_.DefaultFont()).descent + (accounting ? 3 : 0));
  }

  float Indent(const Xf& xf) const {
    if (!xf.alignment.indent) return 0;
    const Font& base = styles_.DefaultFont();
    return xf.alignment.indent * 3 * text_.Measure().Of(styles_.FontOf(xf), base).space * g_.px;
  }

  // The text's left edge in `box` for its alignment (for one line; wrapped lines align inside).
  float AlignX(const CellText::Layout& l, const Xf& xf, const draw::Rect& box) const {
    const float w = l.width * g_.pt, indent = Indent(xf);
    if (l.wrapped) return box.left + PadL() + (l.align == HAlign::Right ? 0 : indent);
    switch (l.align) {
      case HAlign::Right: return box.right - PadR() - indent - w;
      case HAlign::Center:
      case HAlign::CenterContinuous: return (box.left + box.right - w) / 2;
      default: return box.left + PadL() + indent;
    }
  }

  void PlaceCell(RowData& rd, const CellInfo& cell) {
    const int widthPx = static_cast<int>(std::lround(grid_.ColWidth(cell.col) * g_.widthScale));
    if (HideValue(cell.row, cell.col)) return;
    const CellText::Layout& l =
        text_.Get(sheet_, cell, widthPx, TextLookAt(cell.row, cell.col, styles_.CellXf(cell.style)), grid_.ColWidth(cell.col));
    if (l.lines.empty()) return;
    const Xf& xf = styles_.CellXf(cell.style);
    Item it{&l, Inner(cell.row, cell.col, cell.row, cell.col), {}, 0, &xf, rowDescent_};
    it.clip = it.box;
    uint32_t first = cell.col, last = cell.col;
    const float need = l.width * g_.pt + g_.padStart + g_.padEnd + Indent(xf);
    const bool runs = !l.wrapped && !l.stacked && !l.shown.number && !l.shown.fill && xf.alignment.rotation == 0 &&
                      !xf.alignment.shrinkToFit && l.align != HAlign::Fill && l.align != HAlign::Justify &&
                      l.align != HAlign::Distributed;
    auto grow = [&](int dir, float want) {  // into empty neighbors, visually right (+1) or left (-1)
      float got = 0;
      uint32_t edge = dir > 0 ? (g_.rtl ? first : last) : (g_.rtl ? last : first);
      while (got < want) {
        const uint32_t n = Next(edge, dir);
        if (n == kNone || !Empty(rd, n)) break;
        edge = n;
        got += g_.Right(n) - g_.Left(n);
        first = std::min(first, n);
        last = std::max(last, n);
      }
    };
    const float have = it.box.right - it.box.left;
    if (l.align == HAlign::CenterContinuous) {
      // Across the empty cells after it that center across selection too.
      uint32_t c = cell.col;
      while (true) {
        const uint32_t n = Next(c, g_.rtl ? -1 : 1);
        if (n == kNone || !Empty(rd, n)) break;
        size_t ci = static_cast<size_t>(std::find(cols_.begin(), cols_.end(), n) - cols_.begin());
        const uint32_t style = ci < cols_.size() ? StyleAt(rd, ci) : 0;
        if (styles_.CellXf(style).alignment.horizontal != HAlign::CenterContinuous) break;
        c = n;
        last = std::max(last, n);
        first = std::min(first, n);
      }
    } else if (runs && need > have + g_.line) {
      if (l.align == HAlign::Left) grow(1, need - have);
      else if (l.align == HAlign::Right) grow(-1, need - have);
      else if (l.align == HAlign::Center) {
        const float half = (need - have) / 2;
        grow(1, half);
        grow(-1, half);
      }
    }
    if (first != cell.col || last != cell.col) {
      const draw::Rect span = Inner(cell.row, first, cell.row, last);
      it.clip = span;
      if (l.align == HAlign::CenterContinuous) it.box = span;
      rd.spans.push_back({first, last});
    }
    // Only what reaches the drawn columns.
    if (last < area_.firstCol || first > area_.lastCol) return;
    it.x = AlignX(l, xf, it.box);
    items_.push_back(it);
  }

  void DrawText() {
    for (const Item& it : items_) DrawItem(it);
  }

  void DrawItem(const Item& it) {
    const CellText::Layout& l = *it.layout;
    const Alignment& al = it.xf->alignment;
    const float s = g_.pt;
    // The text block, in points: the lines' pitch (Excel's row height for the font) and the
    // first line's ascent and descent as GDI measures them.
    const size_t n = l.lines.size();
    const float blockH = l.ascent + l.descent + (n > 1 ? (n - 1) * (l.wrapped || l.stacked ? l.pitch : l.pitch) : 0);
    float scale = 1;
    if (al.shrinkToFit && !l.wrapped) {
      const float avail = (it.box.right - it.box.left) - g_.padStart - g_.padEnd;
      if (l.width * s > avail && l.width > 0) scale = std::max(0.1f, avail / (l.width * s));
    }
    const float boxH = it.box.bottom - it.box.top;
    (void)boxH;
    p_.Save();
    p_.ClipRect(it.clip);
    const bool rotated = al.rotation != 0 && al.rotation != 255;
    if (rotated) {
      const double deg = al.rotation <= 90 ? al.rotation : -(al.rotation - 90.0);
      const double rad = deg * 3.14159265358979 / 180;
      const float w = l.width * s, h = blockH * s;
      const float bw = static_cast<float>(w * std::fabs(std::cos(rad)) + h * std::fabs(std::sin(rad)));
      const float bh = static_cast<float>(w * std::fabs(std::sin(rad)) + h * std::fabs(std::cos(rad)));
      float cx;
      switch (l.align) {
        case HAlign::Right: cx = it.box.right - PadR() - bw / 2; break;
        case HAlign::Center:
        case HAlign::CenterContinuous: cx = (it.box.left + it.box.right) / 2; break;
        default: cx = it.box.left + PadL() + bw / 2; break;
      }
      float cy;
      switch (al.vertical) {
        case VAlign::Top: cy = it.box.top + g_.px + bh / 2; break;
        case VAlign::Center: cy = (it.box.top + it.box.bottom) / 2; break;
        default: cy = it.box.bottom - g_.px - bh / 2; break;
      }
      p_.Transform(draw::Matrix::Translate(-l.width / 2, -(blockH / 2 - l.ascent)) * draw::Matrix::Rotate(static_cast<float>(-deg)) *
                   draw::Matrix::Scale(s, s) * draw::Matrix::Translate(cx, cy));
      DrawLines(l, 0, l.pitch);
      p_.Restore();
      return;
    }
    // Vertical placement in Excel's pixels, as the screen lays it out: the first line's ascent and
    // descent and the lines' pitch (printing scales rows, and these with them).
    const float a = l.ascent / 0.75f * scale, d = l.descent / 0.75f * scale, pitch = l.pitch / 0.75f * scale;
    const float rp = g_.rowPx;
    const float blockPx = a + d + (n > 1 ? (n - 1) * pitch : 0);
    float baseline;
    switch (al.vertical) {
      case VAlign::Top:
      case VAlign::Justify:
      case VAlign::Distributed: baseline = it.box.top + (1 + a) * rp; break;
      case VAlign::Center: baseline = it.box.top + ((boxH / rp - blockPx) / 2 + a) * rp; break;
      default: {
        const float own = d + DescentPx(styles_.FontOf(*it.xf)) * scale - l.descent / 0.75f * scale;
        const float dr = std::max(own, it.rowDescent);
        baseline = g_.descentPx > 0 ? it.box.bottom + g_.line - dr * g_.descentPx - (n > 1 ? (n - 1) * pitch * rp : 0)
                                    : it.box.bottom - (dr + (n > 1 ? (n - 1) * pitch : 0)) * rp;
        break;
      }
    }
    const float pitchText = pitch * rp / (s * scale);
    float x = it.x;
    if (scale != 1 && !l.wrapped) {
      const float w = l.width * s * scale;
      switch (l.align) {
        case HAlign::Right: x = it.box.right - PadR() - w; break;
        case HAlign::Center:
        case HAlign::CenterContinuous: x = (it.box.left + it.box.right - w) / 2; break;
        default: break;
      }
    }
    if (l.stacked) x = (it.box.left + it.box.right) / 2 - (l.lines.empty() ? 0 : (l.lines[0].right * s) / 2);
    if (l.align == HAlign::Fill && !l.wrapped && l.width > 0) {
      // The text repeated across the cell.
      const float w = l.width * s;
      const int times = std::clamp(static_cast<int>((it.box.right - it.box.left - g_.padStart - g_.padEnd) / w), 1, 256);
      for (int i = 0; i < times; ++i) {
        p_.Save();
        p_.Transform(draw::Matrix::Scale(s, s) * draw::Matrix::Translate(it.box.left + PadL() + i * w, baseline));
        DrawLines(l, 0, pitchText);
        p_.Restore();
      }
      p_.Restore();
      return;
    }
    p_.Transform(draw::Matrix::Scale(s * scale, s * scale) * draw::Matrix::Translate(x, baseline));
    DrawLines(l, l.wrapped || l.stacked ? 0 : 1, pitchText, (it.box.left + PadL() - x) / (s * scale),
              (it.box.right - x) / (s * scale));
    p_.Restore();
  }

  // The lines in text space: x in points from the text's left edge, y from the first baseline.
  // `accLeft` and `accRight`: the cell's text box, where an accounting underline runs (text space).
  void DrawLines(const CellText::Layout& l, int fromContent, float pitch, float accLeft = 0, float accRight = 0) {
    float y = 0;
    for (const text::Line& line : l.lines) {
      const float dx = fromContent ? -line.contentLeft : 0;
      for (const text::GlyphPiece& piece : line.pieces) {
        if (piece.glyphs.empty()) continue;
        const CellText::Run* run = piece.run < l.runs.size() ? &l.runs[piece.run] : nullptr;
        if (run && run->hidden) continue;
        const draw::Brush brush = draw::Brush::Solid(ToColor(run ? run->color : 0xFF000000));
        const draw::GlyphRun gr = ShownRun(piece, l.text);
        if (piece.scale != 1) {
          p_.Save();
          p_.Transform(draw::Matrix::Translate(-piece.OriginX(), 0) * draw::Matrix::Scale(piece.scale, 1) *
                       draw::Matrix::Translate(piece.OriginX() + dx, y - piece.rise));
          p_.DrawGlyphs({piece.OriginX(), 0}, gr, brush);
          p_.Restore();
        } else {
          p_.DrawGlyphs({piece.OriginX() + dx, y - piece.rise}, gr, brush);
        }
        if (run && run->underline) {
          // As Excel prints them: an accounting underline 1.7 pt lower and from the text's start to
          // the cell's end; a double one as two lines 0.6 pt thick, 0.6 pt above and below.
          float l0 = piece.x + dx, l1 = piece.x + dx + piece.width;
          float uy = y + run->underlinePos;
          if (run->accounting) {
            uy += 1.7f;
            if (accRight > accLeft) l1 = std::max(l1, accRight);
          }
          const float t = run->underlineThick;
          if (!run->doubleUnderline) {
            p_.FillRect({l0, uy, l1, uy + t}, brush);
          } else {
            const float t2 = t * 0.6f / 0.84f;
            p_.FillRect({l0, uy - t2, l1, uy}, brush);
            p_.FillRect({l0, uy + t2, l1, uy + 2 * t2}, brush);
          }
        }
        if (run && run->strike) {
          const float sy = y - run->strikePos;
          p_.FillRect({piece.x + dx, sy, piece.x + dx + piece.width, sy + run->strikeThick}, brush);
        }
      }
      y += pitch;
    }
  }

  // ---- Gridlines: under fills, not inside merged areas or text that runs across them.
  void Gridlines() {
    const draw::Color c = ToColor(o_.gridColor);
    const float L = g_.line;
    // The line's width, how far it starts before the edge and runs past it.
    const float w = g_.centered ? kPrintHair * g_.pt : L, before = g_.centered ? w / 2 : L, after = w - before;
    // Horizontal: each row's bottom.
    for (const RowData& rd : rows_) {
      const float y = g_.Bottom(rd.row) - before;
      float runStart = 0, runEnd = 0;
      bool open = false;
      for (size_t i = 0; i < cols_.size(); ++i) {
        const uint32_t col = cols_[i];
        const Range* m = MergeAt(rd.row, col);
        const bool skip = m && rd.row < m->lastRow;
        const float l = g_.Left(col) - (g_.centered ? before : g_.rtl ? 0 : L);
        const float r = g_.Right(col) + (g_.centered ? after : g_.rtl ? L : 0);
        if (skip) {
          if (open) Rect(runStart, y, runEnd, y + w, c);
          open = false;
          continue;
        }
        if (open && (std::fabs(l - runEnd) < 0.5f || std::fabs(r - runStart) < 0.5f)) {
          runStart = std::min(runStart, l);
          runEnd = std::max(runEnd, r);
        } else {
          if (open) Rect(runStart, y, runEnd, y + w, c);
          runStart = l;
          runEnd = r;
          open = true;
        }
      }
      if (open) Rect(runStart, y, runEnd, y + w, c);
    }
    // Vertical: each column's trailing edge.
    for (size_t i = 0; i < cols_.size(); ++i) {
      const uint32_t col = cols_[i];
      const float x = g_.centered ? (g_.rtl ? g_.Left(col) : g_.Right(col)) - before
                      : g_.rtl     ? g_.Left(col)
                                   : g_.Right(col) - L;
      float runTop = 0, runBottom = 0;
      bool open = false;
      for (const RowData& rd : rows_) {
        const Range* m = MergeAt(rd.row, col);
        const bool skip = (m && col < m->lastCol) || rd.SpanCrosses(col, col + 1);
        if (skip) {
          if (open) Rect(x, runTop, x + w, runBottom, c);
          open = false;
          continue;
        }
        const float t = g_.Top(rd.row) - before, b = g_.Bottom(rd.row) + after;
        if (open && std::fabs(t + w - runBottom) < 0.5f) {
          runBottom = b;
        } else {
          if (open) Rect(x, runTop, x + w, runBottom, c);
          runTop = t;
          runBottom = b;
          open = true;
        }
      }
      if (open) Rect(x, runTop, x + w, runBottom, c);
    }
  }

  // ---- Fills
  void FillRect(const draw::Rect& r, const Fill& f) {
    if (f.gradient && !f.stops.empty()) {
      std::vector<draw::GradientStop> stops;
      for (const GradientStop& s : f.stops)
        stops.push_back({static_cast<float>(s.position), ToColor(styles_.Resolve(s.color, 0xFFFFFFFF))});
      if (f.path) {
        const float cx = r.left + (r.right - r.left) * static_cast<float>((f.left + f.right) / 2);
        const float cy = r.top + (r.bottom - r.top) * static_cast<float>((f.top + f.bottom) / 2);
        p_.FillRect(r, draw::Brush::Radial({cx, cy}, (r.right - r.left), (r.bottom - r.top), stops));
        return;
      }
      const double rad = f.degree * 3.14159265358979 / 180;
      const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
      const float hw = (r.right - r.left) / 2, hh = (r.bottom - r.top) / 2;
      const float dx = static_cast<float>(std::cos(rad)), dy = static_cast<float>(std::sin(rad));
      const float reach = std::fabs(hw * dx) + std::fabs(hh * dy);
      p_.FillRect(r, draw::Brush::Linear({cx - dx * reach, cy - dy * reach}, {cx + dx * reach, cy + dy * reach}, stops));
      return;
    }
    if (f.pattern == PatternType::None) return;
    // Indexed 64 and 65 are the system's text and window colors: black and white.
    auto color = [&](const Color& c, uint32_t fallback) {
      if (c.kind == Color::Kind::Indexed && (c.value == 64 || c.value == 65)) {
        const uint32_t argb = c.value == 64 ? 0xFF000000u : 0xFFFFFFFFu;
        return c.tint != 0 ? ApplyTint(argb, c.tint) : argb;
      }
      return styles_.Resolve(c, fallback);
    };
    const uint32_t fg = color(f.fg, 0xFF000000);
    if (f.pattern == PatternType::Solid) {
      p_.FillRect(r, draw::Brush::Solid(ToColor(fg)));
      return;
    }
    const uint32_t bg = color(f.bg, 0xFFFFFFFF);
    p_.FillRect(r, draw::Brush::Solid(Mix(bg, fg, PatternShare(f.pattern))));
  }

  void Fills() {
    for (const RowData& rd : rows_) {
      for (size_t i = 0; i < cols_.size(); ++i) {
        const uint32_t col = cols_[i];
        if (MergeAt(rd.row, col)) continue;
        const Fill& f = styles_.FillOf(styles_.CellXf(StyleAt(rd, i)));
        if (f.pattern == PatternType::None && !f.gradient && !Looks()) continue;
        FillCell(Outer(rd.row, col, rd.row, col), rd.row, col, f);
      }
    }
    for (const Range* m : merges_) {
      const Fill& f = styles_.FillOf(styles_.CellXf(sheet_.StyleAt(m->firstRow, m->firstCol)));
      FillCell(Outer(m->firstRow, m->firstCol, m->lastRow, m->lastCol), m->firstRow, m->firstCol, f);
    }
  }

  // ---- Borders
  // Keys for cell edges: the top edge of (r, c) is the bottom of (r - 1, c); the start edge of
  // (r, c) is the end of (r, c - 1).
  static uint64_t HEdge(uint32_t row, uint32_t col) { return (uint64_t{row} << 15 | col) << 1; }
  static uint64_t VEdge(uint32_t row, uint32_t col) { return (uint64_t{row} << 15 | col) << 1 | 1; }

  void AddSide(std::vector<Segment>& out, const BorderSide& side, bool vertical, float a0, float a1, float b, uint64_t edge) {
    if (side.style == BorderStyle::None) return;
    out.push_back({a0, a1, b, vertical, side.style, styles_.Resolve(side.color, 0xFF000000), edge});
  }

  void Borders() {
    std::vector<Segment> segs;
    std::vector<std::pair<draw::Rect, std::pair<Border, uint32_t>>> diagonals;  // a copy: `b` is the loop's
    const float L = g_.centered ? 0 : g_.line;
    for (const RowData& rd : rows_) {
      for (size_t i = 0; i < cols_.size(); ++i) {
        const uint32_t col = cols_[i];
        const Border b = BorderAt(rd.row, col, styles_.BorderOf(styles_.CellXf(StyleAt(rd, i))));
        const Range* m = MergeAt(rd.row, col);
        const bool top = !m || rd.row == m->firstRow, bottom = !m || rd.row == m->lastRow;
        const bool first = !m || col == m->firstCol, last = !m || col == m->lastCol;
        const float l = g_.Left(col), r = g_.Right(col), t = g_.Top(rd.row), btm = g_.Bottom(rd.row);
        // The file's left is the start: the visual right in a right-to-left sheet.
        const float startX = g_.rtl ? r : l, endX = g_.rtl ? l : r;
        if (top) AddSide(segs, b.top, false, l - L, r, t, HEdge(rd.row, col));
        if (bottom) AddSide(segs, b.bottom, false, l - L, r, btm, HEdge(rd.row + 1, col));
        if (first) AddSide(segs, b.left, true, t - L, btm, startX, VEdge(rd.row, col));
        if (last) AddSide(segs, b.right, true, t - L, btm, endX, VEdge(rd.row, col + 1));
        if ((b.diagonalUp || b.diagonalDown) && b.diagonal.style != BorderStyle::None && !m)
          diagonals.push_back({{l, t, r, btm}, {b, styles_.Resolve(b.diagonal.color, 0xFF000000)}});
      }
    }
    // The cells just outside: their edges along the drawn ones (the row above's bottom border is
    // the first row's top).
    const uint32_t r0 = rows_.front().row, r1 = rows_.back().row, c0 = cols_.front(), c1 = cols_.back();
    if (r0 > 0) {
      const uint32_t above = r0 - 1;
      for (uint32_t col : cols_)
        AddSide(segs, styles_.BorderOf(styles_.CellXf(sheet_.StyleAt(above, col))).bottom, false, g_.Left(col) - L,
                g_.Right(col), g_.Top(r0), HEdge(r0, col));
    }
    if (r1 + 1 < kMaxRows)
      for (uint32_t col : cols_)
        AddSide(segs, styles_.BorderOf(styles_.CellXf(sheet_.StyleAt(r1 + 1, col))).top, false, g_.Left(col) - L,
                g_.Right(col), g_.Bottom(r1), HEdge(r1 + 1, col));
    for (const RowData& rd : rows_) {
      const float t = g_.Top(rd.row), btm = g_.Bottom(rd.row);
      if (c0 > 0)
        AddSide(segs, styles_.BorderOf(styles_.CellXf(sheet_.StyleAt(rd.row, c0 - 1))).right, true, t - L, btm,
                g_.rtl ? g_.Right(c0) : g_.Left(c0), VEdge(rd.row, c0));
      if (c1 + 1 < kMaxCols)
        AddSide(segs, styles_.BorderOf(styles_.CellXf(sheet_.StyleAt(rd.row, c1 + 1))).left, true, t - L, btm,
                g_.rtl ? g_.Left(c1) : g_.Right(c1), VEdge(rd.row, c1 + 1));
    }
    // One border an edge: the heavier, else the later cell's (below, after).
    std::stable_sort(segs.begin(), segs.end(), [](const Segment& a, const Segment& b) { return a.edge < b.edge; });
    std::vector<Segment> kept;
    for (const Segment& s : segs) {
      if (!kept.empty() && kept.back().edge == s.edge) {
        if (KindOf(s.style).weight >= KindOf(kept.back().style).weight) kept.back() = s;
        continue;
      }
      kept.push_back(s);
    }
    std::stable_sort(kept.begin(), kept.end(),
                     [](const Segment& a, const Segment& b) { return KindOf(a.style).weight < KindOf(b.style).weight; });
    for (const Segment& s : kept) DrawSegment(s);
    for (const auto& [r, d] : diagonals) {
      const LineKind k = KindOf(d.first.diagonal.style);
      draw::Pen pen;
      pen.brush = draw::Brush::Solid(ToColor(d.second));
      pen.width = g_.centered ? k.width * g_.pt : std::max(g_.line, k.width * g_.px);
      if (d.first.diagonalDown) {
        draw::Path path;
        path.MoveTo({r.left, r.top});
        path.LineTo({r.right, r.bottom});
        p_.StrokePath(path, pen);
      }
      if (d.first.diagonalUp) {
        draw::Path path;
        path.MoveTo({r.left, r.bottom});
        path.LineTo({r.right, r.top});
        p_.StrokePath(path, pen);
      }
    }
  }

  void DrawSegment(const Segment& s) {
    const LineKind k = KindOf(s.style);
    if (k.width <= 0) return;
    const float L = g_.line;
    const draw::Color c = ToColor(s.color);
    std::vector<std::pair<float, float>> bands;
    float a0 = s.a0, a1 = s.a1;
    std::vector<int> dashes = k.dashes;
    float unit = std::max(L, g_.px);
    if (g_.centered) {
      // Paper: centred on the edge, a point a pixel; hair a hairline, dots solid (as Excel prints).
      const float U = g_.pt;
      const float w = s.style == BorderStyle::Hair ? kPrintHair * U : (k.doubled ? 3 : k.width) * U;
      const float lo = s.b - w / 2;
      if (k.doubled) bands = {{lo, lo + U}, {lo + 2 * U, lo + 3 * U}};
      else bands = {{lo, lo + w}};
      a0 -= w / 2;
      a1 += w / 2;
      if (s.style == BorderStyle::Dotted) dashes.clear();
      unit = U;
    } else if (k.doubled) {
      // Across the line: centered on the gridline's pixel, which is the last of the cell before it.
      bands = {{s.b - 2 * L, s.b - L}, {s.b, s.b + L}};
    } else {
      const float lo = s.b - L * static_cast<float>((k.width + 1) / 2);
      bands = {{lo, lo + L * k.width}};
    }
    auto put = [&](float a0, float a1) {
      for (const auto& [b0, b1] : bands) {
        if (s.vertical) Rect(b0, a0, b1, a1, c);
        else Rect(a0, b0, a1, b1, c);
      }
    };
    if (dashes.empty()) {
      put(a0, a1);
      return;
    }
    float a = a0;
    size_t i = 0;
    while (a < a1) {
      const float len = dashes[i % dashes.size()] * unit;
      if (i % 2 == 0) put(a, std::min(a + len, a1));
      a += len;
      ++i;
    }
  }

  draw::Painter& p_;
  const Sheet& sheet_;
  CellText& text_;
  const Grid& grid_;
  const Geometry& g_;
  Range area_;
  const PaintOptions& o_;
  const Styles& styles_;
  float rowDescent_ = 0;  // PlaceText's current row's (Item::rowDescent)
  std::vector<RowData> rows_;
  std::vector<uint32_t> cols_;
  std::vector<uint32_t> colStyle_;
  std::vector<const Range*> merges_;
  std::vector<Item> items_;
  uint32_t reachFirst_ = 0, reachLast_ = 0;
  std::unordered_map<uint64_t, Look> looks_;
};

}  // namespace

void PaintCells(draw::Painter& p, const Sheet& sheet, CellText& text, const Grid& grid, const Geometry& g,
                const Range& area, const PaintOptions& options) {
  Painting(p, sheet, text, grid, g, area, options).Run();
}

}  // namespace fw::sheet::view
