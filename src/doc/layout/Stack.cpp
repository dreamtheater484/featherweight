// Stacked layout (Engine.h): content laid out from the top of a frame down, without page breaks,
// for headers, footers, notes, table cells and text boxes; and the paragraph pieces the body flow
// shares with it (line heights, spacing, floats).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

#include "doc/layout/Engine.h"

namespace fw::doc::layout {

namespace {

constexpr float kMinWrapWidth = 18;  // narrower gaps beside a float get no text

float ParseVmlLength(std::string_view v, float fallback) {
  while (!v.empty() && v.front() == ' ') v.remove_prefix(1);
  if (v.empty()) return fallback;
  char* end = nullptr;
  std::string s(v);
  double n = std::strtod(s.c_str(), &end);
  std::string_view unit(end);
  while (!unit.empty() && unit.back() == ' ') unit.remove_suffix(1);
  if (unit == "pt") return static_cast<float>(n);
  if (unit == "in") return static_cast<float>(n * 72);
  if (unit == "cm") return static_cast<float>(n * 72 / 2.54);
  if (unit == "mm") return static_cast<float>(n * 72 / 25.4);
  if (unit == "px") return static_cast<float>(n * 0.75);
  if (unit.empty()) return static_cast<float>(n / 12700.0);  // EMU
  return fallback;
}

// A text box's text direction, in quarter turns clockwise: bodyPr vert="vert" or "eaVert" runs
// down (1), "vert270" up (3); VML's layout-flow:vertical runs down, or up with
// mso-layout-flow-alt:bottom-to-top. 0: horizontal.
uint8_t TextBoxTurn(const std::string& xml) {
  const size_t body = xml.find("bodyPr");
  if (body != std::string::npos) {
    const size_t end = xml.find('>', body);
    const size_t vert = xml.find(" vert=\"", body);
    if (vert != std::string::npos && vert < end) {
      const std::string_view v = std::string_view(xml).substr(vert + 7, xml.find('"', vert + 7) - (vert + 7));
      if (v == "vert" || v == "eaVert") return 1;
      if (v == "vert270") return 3;
    }
    return 0;
  }
  if (xml.find("layout-flow:vertical") != std::string::npos)
    return xml.find("bottom-to-top") != std::string::npos ? 3 : 1;
  return 0;
}

// A text box's insets (left, top, right, bottom), from bodyPr or the VML textbox's inset.
std::array<float, 4> Insets(const std::string& xml) {
  std::array<float, 4> in = {7.2f, 3.6f, 7.2f, 3.6f};
  auto attr = [&](size_t from, std::string_view name) -> std::optional<std::string_view> {
    size_t end = xml.find('>', from);
    std::string key = " " + std::string(name) + "=\"";
    size_t at = xml.find(key, from);
    if (at == std::string::npos || at > end) return std::nullopt;
    at += key.size();
    size_t close = xml.find('"', at);
    if (close == std::string::npos) return std::nullopt;
    return std::string_view(xml).substr(at, close - at);
  };
  size_t body = xml.find(":bodyPr");
  if (body != std::string::npos) {
    const char* names[] = {"lIns", "tIns", "rIns", "bIns"};
    for (int i = 0; i < 4; ++i)
      if (auto v = attr(body, names[i])) in[i] = static_cast<float>(std::atof(std::string(*v).c_str()) / 12700.0);
    return in;
  }
  size_t box = xml.find("<v:textbox");
  if (box != std::string::npos) {
    if (auto v = attr(box, "inset")) {
      std::string_view s = *v;
      for (int i = 0; i < 4 && !s.empty(); ++i) {
        size_t comma = s.find(',');
        std::string_view part = s.substr(0, comma);
        if (!part.empty()) in[i] = ParseVmlLength(part, in[i]);
        if (comma == std::string_view::npos) break;
        s.remove_prefix(comma + 1);
      }
    }
  }
  return in;
}

}  // namespace

void Stack::Offset(float dx, float dy) {
  for (LineBox& l : lines) {
    if (l.turn) {  // vertical text: its frame moves
      l.turnX += dx;
      l.turnY += dy;
    } else {
      l.x += dx;
      l.top += dy;
      l.baseline += dy;
    }
    if (l.clip.Width() > 0) {
      l.clip.left += dx;
      l.clip.right += dx;
      l.clip.top += dy;
      l.clip.bottom += dy;
    }
  }
  for (Fill& f : fills) f.rect = {f.rect.left + dx, f.rect.top + dy, f.rect.right + dx, f.rect.bottom + dy};
  for (auto& n : notes) n.first += dy;
  for (Edge& e : edges) {
    e.x1 += dx;
    e.x2 += dx;
    e.y1 += dy;
    e.y2 += dy;
  }
  for (DrawingBox& d : drawings) d.rect = {d.rect.left + dx, d.rect.top + dy, d.rect.right + dx, d.rect.bottom + dy};
  for (CellRect& c : cells) c.box = {c.box.left + dx, c.box.top + dy, c.box.right + dx, c.box.bottom + dy};
}

void Stack::Append(Stack&& other, float dx, float dy) {
  other.Offset(dx, dy);
  uint32_t base = static_cast<uint32_t>(paints.size());
  for (LineBox& l : other.lines) {
    l.paint += base;
    lines.push_back(std::move(l));
  }
  paints.insert(paints.end(), other.paints.begin(), other.paints.end());
  fills.insert(fills.end(), other.fills.begin(), other.fills.end());
  edges.insert(edges.end(), other.edges.begin(), other.edges.end());
  drawings.insert(drawings.end(), other.drawings.begin(), other.drawings.end());
  cells.insert(cells.end(), other.cells.begin(), other.cells.end());
  notes.insert(notes.end(), other.notes.begin(), other.notes.end());
  TakeFlags(other);
}

void Stack::TakeFlags(const Stack& other) {
  usesPageCount = usesPageCount || other.usesPageCount;
  usesPage = usesPage || other.usesPage;
  numbersNotes = numbersNotes || other.numbersNotes;
  placedOnPage = placedOnPage || other.placedOnPage;
  labels = labels || other.labels;
}

// The height of a line's text alone, without its inline objects: what proportional spacing adds
// to (as LibreOffice reproduces Word: a double-spaced line with a picture gets one more line of
// text height, not twice the picture).
float Engine::TextHeight(const ParaText& pt, const text::Line& line) {
  if (line.objects.empty()) return line.ascent + line.descent + line.lineGap;
  float top = 0, descent = 0;
  auto use = [&](const text::TextStyle* style) {
    if (!style) return;
    const text::FontMetrics& m = styles_.Metrics(fonts_, style);
    top = std::max(top, m.ascent + m.lineGap);
    descent = std::max(descent, m.descent);
  };
  for (const text::GlyphPiece& g : line.pieces)
    if (g.run < pt.runs.size()) use(pt.runs[g.run].style);
  if (line.ending == text::LineEnd::ParagraphEnd || line.pieces.empty()) use(pt.fw.markStyle);
  return top + descent;
}

float Engine::LineHeight(const ParaText& pt, const text::Line& line) {
  const ParaProps& p = *pt.props;
  const float single = line.ascent + line.descent + line.lineGap;
  switch (p.lineRule) {
    case LineRule::Exact: return std::abs(static_cast<float>(p.line)) / 20.0f;
    case LineRule::AtLeast: return std::max(single, static_cast<float>(p.line) / 20.0f);
    default: {
      if (p.line <= 0) return single;
      const float factor = static_cast<float>(p.line) / 240.0f;
      if (factor < 1) return single * factor;
      return single + (factor - 1) * TextHeight(pt, line);
    }
  }
}

float Engine::BaselineIn(const ParaText& pt, const text::Line& line, float height) const {
  // Proportional and at-least spacing add their space below the text: a double-spaced first line on
  // a page has its baseline where a single-spaced one would. An exact height puts the baseline 4/5
  // of the way down, cutting off what doesn't fit; so does proportional spacing under single.
  const ParaProps& p = *pt.props;
  const float single = line.ascent + line.descent + line.lineGap;
  if (p.lineRule == LineRule::Exact) return height * 0.8f;
  if (p.lineRule == LineRule::Auto && p.line > 0 && p.line < 240) return height * 0.8f;
  if (height >= single) return line.lineGap + line.ascent;
  return height - line.descent;
}

float Engine::Hang(const ParaText& pt, const text::Line& line, float height) const {
  const ParaProps& p = *pt.props;
  if (p.lineRule != LineRule::Auto || p.line <= 240) return 0;
  return std::max(0.0f, height - (line.ascent + line.descent + line.lineGap));
}

bool Engine::Contextual(const ParaProps& a, const ParaProps& b) const { return a.style == b.style; }

float Engine::SpaceBefore(const ParaProps& p, const Spacing& s) const {
  float before = p.spaceBeforeLines >= 0 ? static_cast<float>(p.spaceBeforeLines) * 12.0f / 100.0f : Pt(p.spaceBefore);
  if (p.spaceBeforeAuto) before = s.atTop && s.prev == nullptr ? 0.0f : 14.0f;
  if (s.prev && p.contextualSpacing && Contextual(*s.prev, p)) before = 0;
  float after = s.after;
  // HTML's automatic spacing leaves none between two items of one list, as between <li>s.
  const bool sameList = s.prev && !p.numId.empty() && s.prev->numId == p.numId;
  if (sameList && p.spaceBeforeAuto) before = 0;
  if (sameList && s.prev->spaceAfterAuto) after = 0;
  if (s.prev && s.prev->contextualSpacing && Contextual(*s.prev, p)) after = 0;
  // Between two paragraphs Word leaves the larger of the one's space after and the other's space
  // before, as HTML does, unless the document asks it not to use HTML paragraph spacing.
  return sumSpacing_ ? after + before : std::max(after, before);
}

namespace {
float BorderRoom(const Border& b) { return b.Present() ? static_cast<float>(b.spacePt) + Pt(b.width) : 0.0f; }
bool Bordered(const ParaBorders& b) {
  return b.top.Present() || b.bottom.Present() || b.start.Present() || b.end.Present() || b.between.Present();
}
}  // namespace

float Engine::BorderAbove(const ParaProps& p, const Spacing& s) const {
  if (s.prev && Bordered(p.borders) && s.prev->borders == p.borders && s.prev->indStart == p.indStart &&
      s.prev->indEnd == p.indEnd)
    return BorderRoom(p.borders.between);
  return BorderRoom(p.borders.top) + (s.prev ? BorderRoom(s.prev->borders.bottom) : 0.0f);
}

float Engine::BorderBelow(const Spacing& s) const { return s.prev ? BorderRoom(s.prev->borders.bottom) : 0.0f; }

float Engine::SpaceAfter(const ParaProps& p) const {
  if (p.spaceAfterAuto) return 14;
  return p.spaceAfterLines >= 0 ? static_cast<float>(p.spaceAfterLines) * 12.0f / 100.0f : Pt(p.spaceAfter);
}

Rect Engine::PlaceFloat(const Drawing& d, const Frame& frame, float paraTop, float columnLeft, float columnRight) const {
  const PageGeom& g = *frame.geom;
  float w = Pt(d.cx), h = Pt(d.cy);
  // Horizontal.
  std::string_view hr = d.hRelativeFrom;
  float l = columnLeft, r = columnRight;
  if (hr == "page") l = 0, r = g.width;
  else if (hr == "margin") l = g.left, r = g.width - g.right;
  else if (hr == "leftMargin" || hr == "insideMargin") l = 0, r = g.left;
  else if (hr == "rightMargin" || hr == "outsideMargin") l = g.width - g.right, r = g.width;
  float x = l;
  if (d.simplePos) x = Pt(d.simpleX);
  else if (!d.hAlign.empty()) {
    if (d.hAlign == "right" || d.hAlign == "outside") x = r - w;
    else if (d.hAlign == "center") x = (l + r - w) / 2;
  } else if (d.hOffset) {
    x = l + Pt(*d.hOffset);
  }
  // Vertical.
  std::string_view vr = d.vRelativeFrom;
  float t = paraTop, b = paraTop;
  if (vr == "page") t = 0, b = g.height;
  else if (vr == "margin") t = g.top, b = g.height - g.bottom;
  else if (vr == "topMargin" || vr == "insideMargin") t = 0, b = g.top;
  else if (vr == "bottomMargin" || vr == "outsideMargin") t = g.height - g.bottom, b = g.height;
  float y = t;
  if (d.simplePos) y = Pt(d.simpleY);
  else if (!d.vAlign.empty() && b > t) {
    if (d.vAlign == "bottom" || d.vAlign == "outside") y = b - h;
    else if (d.vAlign == "center") y = (t + b - h) / 2;
  } else if (d.vOffset) {
    y = t + Pt(*d.vOffset);
  }
  return {x, y, x + w, y + h};
}

Zone Engine::ZoneOf(const Drawing& d, const Rect& box) const {
  Zone z;
  z.wrap = d.wrap;
  z.rect = {box.left - Pt(d.distL), box.top - Pt(d.distT), box.right + Pt(d.distR), box.bottom + Pt(d.distB)};
  if (d.wrap == WrapType::TopAndBottom) z.rect = {box.left, box.top - Pt(d.distT), box.right, box.bottom + Pt(d.distB)};
  z.side = d.wrapText == "left" ? 1 : d.wrapText == "right" ? 2 : d.wrapText == "largest" ? 3 : 0;
  return z;
}

text::LineSpace Engine::SpaceBeside(const std::vector<Zone>& zones, float top, float height, float left, float right) const {
  text::LineSpace s{top, left, right};
  for (int guard = 0; guard < 64; ++guard) {
    float l = left, r = right, below = -1;
    bool moved = false;
    for (const Zone& z : zones) {
      if (z.wrap == WrapType::None || z.wrap == WrapType::Inline) continue;
      if (z.rect.top >= s.top + height || z.rect.bottom <= s.top) continue;
      if (z.rect.right <= l || z.rect.left >= r) continue;
      if (z.wrap == WrapType::TopAndBottom) {
        s.top = z.rect.bottom;
        moved = true;
        break;
      }
      float leftW = z.rect.left - l, rightW = r - z.rect.right;
      bool keepLeft = z.side == 1 || (z.side != 2 && leftW >= rightW);
      if (keepLeft) r = std::min(r, z.rect.left);
      else l = std::max(l, z.rect.right);
      below = below < 0 ? z.rect.bottom : std::min(below, z.rect.bottom);
    }
    if (moved) continue;
    if (r - l < kMinWrapWidth && below > s.top) {
      s.top = below;
      continue;
    }
    s.left = l;
    s.right = std::max(l, r);
    return s;
  }
  return s;
}

void Engine::AddDrawing(uint32_t drawing, const Rect& box, bool floating, const Frame& frame, Stack& out) {
  const Drawing& d = doc_.Drawings()[drawing];
  if (d.hidden) return;
  DrawingBox db;
  db.drawing = drawing;
  db.rect = box;
  db.floating = floating;
  db.behindText = d.behindDoc;
  db.z = d.relativeHeight;
  out.drawings.push_back(db);
  if (d.textBoxes.empty()) return;
  const std::string& xml = doc_.DrawingXml(d);
  std::array<float, 4> in = Insets(xml);
  Rect inner{box.left + in[0], box.top + in[1], box.right - in[2], box.bottom - in[3]};
  if (inner.Width() <= 0) return;
  // Vertical text runs along the box's height, in a frame turned about a corner of the box.
  const uint8_t turn = TextBoxTurn(xml);
  if (turn && inner.Height() <= 0) return;
  // One story per box; linked boxes continue in the same story.
  Frame tf = frame;
  tf.x = frame.x + inner.left;
  tf.y = frame.y + inner.top;
  tf.width = turn ? inner.Height() : inner.Width();
  if (frame.story == LineStory::Body || frame.story == LineStory::Cell) tf.story = LineStory::TextBox;
  tf.zones = nullptr;
  tf.inCell = false;
  Stack s = StackStory(d.textBoxes.front(), tf);
  // "Resize shape to fit text" (a:spAutoFit, VML mso-fit-shape-to-text) grows the box to its
  // text, as Word does when the text changes; any other box shows what fits, cut at its edge.
  const bool grows = xml.find("spAutoFit") != std::string::npos || xml.find("fit-shape-to-text:t") != std::string::npos;
  if (grows && !turn && s.height > inner.Height()) out.drawings.back().rect.bottom += s.height - inner.Height();
  for (LineBox& l : s.lines) {
    l.drawing = drawing;
    if (turn) {
      l.turn = turn;
      l.turnX = turn == 3 ? 0 : inner.Width();
      l.turnY = turn == 3 ? inner.Height() : 0;
      l.clip = {0, 0, inner.Width(), inner.Height()};
    } else {
      l.clip = {0, 0, inner.Width(), grows ? std::max(inner.Height(), s.height) : inner.Height() + in[3]};
    }
  }
  out.Append(std::move(s), inner.left, inner.top);
}

void Engine::AddLine(const ParaText& pt, text::Line&& line, float x, float top, float height, const Frame& frame,
                     uint32_t paintBase, Stack& out) {
  LineBox lb;
  lb.para = pt.para;
  lb.story = frame.story;
  lb.x = x;
  lb.top = top;
  lb.height = height;
  lb.baseline = top + BaselineIn(pt, line, height);
  lb.prefix = pt.prefix;
  lb.paint = paintBase;
  lb.text = pt.Shown(line.start, line.end, line.hyphenated);
  lb.line = std::move(line);
  for (const text::InlineObject& o : lb.line.objects) {
    auto it = std::find_if(pt.inlines.begin(), pt.inlines.end(), [&](const auto& p) { return p.first == o.offset; });
    if (it == pt.inlines.end()) continue;
    const Drawing& d = doc_.Drawings()[it->second];
    float bottom = lb.baseline - o.rise + o.descent;
    Rect box{x + o.x + Pt(d.effectL), bottom - o.ascent - o.descent + Pt(d.effectT), x + o.x + o.width - Pt(d.effectR),
             bottom - Pt(d.effectB)};
    AddDrawing(it->second, box, false, frame, out);
  }
  out.lines.push_back(std::move(lb));
}

Stack Engine::StackStory(uint32_t story, const Frame& frame) {
  Stack out;
  if (story == kNone || story >= doc_.Stories().size()) return out;
  Spacing spacing;
  std::vector<Zone> zones;
  Frame f = frame;
  if (!f.zones) f.zones = &zones;
  StackBlocks(doc_.Blocks(story), f, out, spacing);
  out.height += BorderBelow(spacing);
  return out;
}

void Engine::StackBlocks(const std::vector<Block>& blocks, const Frame& frame, Stack& out, Spacing& spacing) {
  for (size_t i = 0; i < blocks.size(); ++i) {
    const Block& b = blocks[i];
    if (!frame.inCell && b.kind != BlockKind::ContentControl && b.kind != BlockKind::CustomXml) {
      if (Placement place = PlacementOf(b)) {
        size_t n = 1;
        while (i + n < blocks.size() && JoinsPlaced(place, blocks[i + n])) ++n;
        std::vector<Block> group(blocks.begin() + static_cast<ptrdiff_t>(i), blocks.begin() + static_cast<ptrdiff_t>(i + n));
        Placed p = PlaceBlocks(group, place, frame, frame.y + out.height, frame.x, frame.x + frame.width);
        if (frame.zones && p.zone.wrap != WrapType::None) frame.zones->push_back(p.zone);
        out.Append(std::move(p.content), p.box.left - frame.x, p.box.top - frame.y);
        out.placedOnPage = true;
        i += n - 1;
        continue;
      }
    }
    switch (b.kind) {
      case BlockKind::Paragraph: StackParagraph(b.para, frame, out, spacing); break;
      case BlockKind::Table: StackTable(b.node, frame, out, spacing); break;
      default: StackBlocks(doc_.Blocks(b.node), frame, out, spacing); break;
    }
  }
}

Engine::Placement Engine::PlacementOf(const Block& b) const {
  Placement place;
  auto frameOf = [&](uint32_t para) -> std::optional<FrameProps> {
    const std::optional<FrameProps>& f = ParaOf(para).frame;
    if (!f || f->dropCap == "drop" || f->dropCap == "margin") return std::nullopt;  // drop caps stay in the text
    return f;
  };
  if (b.kind == BlockKind::Paragraph) {
    place.frame = frameOf(b.para);
  } else if (b.kind == BlockKind::Table) {
    TableProps t = doc_.ResolvedTable(b.node);
    if (t.floating) {
      place.table = t.floating;
    } else if (std::vector<Node> rows = doc_.TableRows(b.node); !rows.empty()) {
      std::vector<Node> cells = doc_.RowCells(rows.front());
      if (!cells.empty()) {
        for (const Block& c : doc_.Blocks(cells.front())) {
          if (c.kind != BlockKind::Paragraph) continue;
          place.frame = frameOf(c.para);
          break;
        }
      }
    }
  }
  return place;
}

bool Engine::JoinsPlaced(const Placement& place, const Block& b) const {
  if (!place.frame || (b.kind != BlockKind::Paragraph && b.kind != BlockKind::Table)) return false;
  Placement next = PlacementOf(b);
  return next.frame && !next.table && *next.frame == *place.frame;
}

Engine::Placed Engine::PlaceBlocks(const std::vector<Block>& group, const Placement& place, const Frame& frame,
                                   float anchorTop, float columnLeft, float columnRight) {
  const PageGeom& g = *frame.geom;
  const FrameProps* fp = place.frame ? &*place.frame : nullptr;
  const TableFloat* tf = place.table ? &*place.table : nullptr;
  // The range it's placed in: Word's defaults are the column across and the margins down.
  const std::string_view ha = fp ? fp->hAnchor : tf->horzAnchor;
  const std::string_view va = fp ? fp->vAnchor : tf->vertAnchor;
  float l = columnLeft, r = columnRight, t = g.top, b = g.height - g.bottom;
  if (ha == "page") l = 0, r = g.width;
  else if (ha == "margin") l = g.left, r = g.width - g.right;
  if (va == "page") t = 0, b = g.height;
  else if (va == "text") t = b = anchorTop;

  // Its content, at the frame's width; a frame without one is as wide as its widest line.
  auto stack = [&](float width) {
    Frame inner = frame;
    inner.x = l;
    inner.y = t;
    inner.width = width;
    inner.zones = nullptr;
    inner.inCell = true;  // nothing in it is placed again
    Stack s;
    Spacing spacing;
    StackBlocks(group, inner, s, spacing);
    s.height += BorderBelow(spacing);
    return s;
  };
  const bool autoWidth = fp && fp->w.Value() <= 0 && group.size() == 1 && group.front().kind == BlockKind::Paragraph;
  Placed out;
  out.content = stack(fp && fp->w.Value() > 0 ? Pt(fp->w) : columnRight - columnLeft);
  float w = fp && fp->w.Value() > 0 ? Pt(fp->w) : 0;
  if (w <= 0) {
    for (const LineBox& lb : out.content.lines) w = std::max(w, lb.x + lb.line.contentRight);
    for (const Edge& e : out.content.edges) w = std::max({w, e.x1, e.x2});
    for (const Fill& f : out.content.fills) w = std::max(w, f.rect.right);
    if (autoWidth) {
      float narrow = 0;
      for (const LineBox& lb : out.content.lines) narrow = std::max(narrow, lb.line.contentRight - lb.line.contentLeft);
      if (narrow > 0 && narrow + 0.5f < w) out.content = stack(w = narrow + 0.5f);
    }
  }
  float h = out.content.height;
  if (fp && fp->h.Value() > 0 && fp->hRule == "exact") h = Pt(fp->h);
  else if (fp && fp->h.Value() > 0 && fp->hRule == "atLeast") h = std::max(h, Pt(fp->h));

  const std::string_view xa = fp ? fp->xAlign : tf->xSpec;
  const std::string_view ya = fp ? fp->yAlign : tf->ySpec;
  float x = l + Pt(fp ? fp->x : tf->x), y = t + Pt(fp ? fp->y : tf->y);
  if (xa == "left" || xa == "inside") x = l;
  else if (xa == "right" || xa == "outside") x = r - w;
  else if (xa == "center") x = (l + r - w) / 2;
  if (ya == "top" || ya == "inside") y = t;
  else if (ya == "bottom" || ya == "outside") y = b - h;
  else if (ya == "center") y = (t + b - h) / 2;
  else if (ya == "inline") y = anchorTop;
  out.box = {x, y, x + w, y + h};

  // Text keeps the frame's distances from it, beside or (notBeside, none) above and below.
  const float dl = Pt(fp ? fp->hSpace : tf->leftFromText), dr = Pt(fp ? fp->hSpace : tf->rightFromText);
  const float dt = Pt(fp ? fp->vSpace : tf->topFromText), db = Pt(fp ? fp->vSpace : tf->bottomFromText);
  const bool beside = !(fp && (fp->wrap == "notBeside" || fp->wrap == "none"));
  out.zone.wrap = w <= 0 || h <= 0 ? WrapType::None : beside ? WrapType::Square : WrapType::TopAndBottom;
  out.zone.rect = beside ? Rect{x - dl, y - dt, x + w + dr, y + h + db} : Rect{x, y - dt, x + w, y + h + db};
  return out;
}

void Engine::StackParagraph(uint32_t para, const Frame& frame, Stack& out, Spacing& spacing) {
  // After an edit, a paragraph laid out again before comes from the cache.
  std::shared_ptr<CachedPara> cached = LinesOf(para, frame.width, false);
  std::shared_ptr<ParaText> text = cached ? cached->pt : nullptr;
  if (!text) {
    text = std::make_shared<ParaText>();
    BuildText(para, frame.fields ? *frame.fields : PageFields(), *text);
    cached = KeepLines(para, frame.width, false, text);
  }
  const ParaText& pt = *text;
  if (pt.empty) return;
  const ParaProps& p = *pt.props;
  if (pt.usesPageCount) out.usesPageCount = true;
  if (pt.usesPage) out.usesPage = true;
  if (!pt.notes.empty()) out.numbersNotes = true;
  if (pt.prefix > 0) out.labels = true;
  float y = out.height - spacing.after + SpaceBefore(p, spacing) + BorderAbove(p, spacing);
  if (spacing.atTop && spacing.prev == nullptr) y = out.height + SpaceBefore(p, spacing) + BorderAbove(p, spacing);
  const float paraTop = y;
  // Floats anchored here.
  for (uint32_t a : pt.anchors) {
    const Drawing& d = doc_.Drawings()[a];
    Rect box = PlaceFloat(d, frame, frame.y + paraTop, frame.x, frame.x + frame.width);
    if (frame.zones && !d.behindDoc && d.wrap != WrapType::None) {
      frame.zones->push_back(ZoneOf(d, box));
      out.placedOnPage = true;
    }
    if (d.vRelativeFrom == "page" || d.vRelativeFrom == "margin" || d.vRelativeFrom.ends_with("Margin")) out.placedOnPage = true;
    AddDrawing(a, {box.left - frame.x, box.top - frame.y, box.right - frame.x, box.bottom - frame.y}, true, frame, out);
  }
  uint32_t paintBase = static_cast<uint32_t>(out.paints.size());
  out.paints.insert(out.paints.end(), pt.paints.begin(), pt.paints.end());
  ParaLines breaker(fonts_, text, frame.width, std::move(cached));
  const bool wrap = frame.zones && !frame.zones->empty();
  float estimate = 12;
  while (!breaker.Done()) {
    text::LineSpace space{frame.y + y, frame.x, frame.x + frame.width};
    const text::LineBreaker::State state = breaker.Save();
    if (wrap) space = SpaceBeside(*frame.zones, frame.y + y, estimate, frame.x, frame.x + frame.width);
    text::Line line = breaker.Next(space.left - frame.x, space.right - frame.x);
    float h = LineHeight(pt, line);
    if (wrap && h > estimate) {
      text::LineSpace again = SpaceBeside(*frame.zones, frame.y + y, h, frame.x, frame.x + frame.width);
      if (again.left != space.left || again.right != space.right || again.top != space.top) {
        breaker.Restore(state);
        space = again;
        line = breaker.Next(space.left - frame.x, space.right - frame.x);
        h = LineHeight(pt, line);
      }
    }
    estimate = h;
    float top = space.top - frame.y;
    for (const auto& [offset, story] : pt.notes)
      if (offset >= line.start && offset < std::max(line.end, line.start + 1) &&
          doc_.Stories()[story].kind == StoryKind::Footnote)
        out.notes.push_back({top + h, story});
    AddLine(pt, std::move(line), 0, top, h, frame, paintBase, out);
    y = top + h;
  }
  if (p.shading.Present() && !p.shading.fill.automatic)
    out.fills.push_back({{Pt(p.indStart), paraTop, frame.width - Pt(p.indEnd), y}, p.shading.fill.rgb});
  float after = SpaceAfter(p);
  out.height = y + after;
  spacing.after = after;
  spacing.prev = &p;
  spacing.atTop = false;
}

void Engine::StackTable(Node tbl, const Frame& frame, Stack& out, Spacing& spacing) {
  TableBox t = MeasureTable(tbl, frame);
  float y = out.height;
  for (size_t r = 0; r < t.rows.size(); ++r) {
    const RowBox& row = t.rows[r];
    if (row.props.hidden) continue;
    EmitRow(t, r, t.left, y, row.height, nullptr, nullptr, out, r == 0, r + 1 == t.rows.size());
    y += row.height;
    for (const CellBox& c : row.cells) out.TakeFlags(c.content);
  }
  out.height = y;
  spacing.after = 0;
  spacing.prev = nullptr;
  spacing.atTop = false;
}

}  // namespace fw::doc::layout
