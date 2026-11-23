// Tables in slides (a:tbl): the grid, merged cells, and the table style (ppt/tableStyles.xml, or
// one of PowerPoint's built-in styles a deck names without defining) applied to every cell.
#include <algorithm>
#include <cctype>
#include <string>

#include "slide/Internal.h"

namespace fw::slide {

using namespace detail;

namespace {

constexpr int kMaxRows = 4096, kMaxColumns = 1024;
constexpr size_t kMaxCells = 200000;

// PowerPoint's built-in styles, which decks may name without defining them (python-pptx and other
// generators write only the id).
struct BuiltInStyle {
  unsigned char id[16];
  unsigned char text, color, color2;
};
#include "slide/TableStylesData.inc"

// The 16 bytes of a "{XXXXXXXX-XXXX-...}" id in the order written, or false.
bool IdBytes(std::string_view id, unsigned char (&out)[16]) {
  int n = 0;
  for (char ch : id) {
    int v = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
    if (v < 0) {
      if (ch == '{' || ch == '}' || ch == '-' || std::isspace(static_cast<unsigned char>(ch))) continue;
      return false;
    }
    if (n >= 32) return false;
    out[n / 2] = static_cast<unsigned char>(n % 2 ? out[n / 2] | v : v << 4);
    ++n;
  }
  return n == 32;
}

// A built-in style's a:tblStyle children: its template expanded, with its colors put in.
std::string BuiltInBody(const BuiltInStyle& style) {
  const unsigned char* at = kStyleText;
  for (int i = 0; i < style.text; ++i) at += std::char_traits<char>::length(reinterpret_cast<const char*>(at)) + 1;
  std::string out;
  std::vector<unsigned char> stack;
  for (; *at; ++at) {
    stack.push_back(*at);
    while (!stack.empty()) {
      unsigned char c = stack.back();
      stack.pop_back();
      if (c < 128) {
        if (c == '@' || c == '#') {
          out += kStyleColors[c == '@' ? style.color : style.color2];
        } else {
          out += static_cast<char>(c);
        }
      } else {
        stack.push_back(kStylePairs[c - 128][1]);
        stack.push_back(kStylePairs[c - 128][0]);
      }
    }
  }
  return out;
}

bool SameId(std::string_view a, std::string_view b) {
  auto trim = [](std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
  };
  a = trim(a);
  b = trim(b);
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  }
  return true;
}

// A fill element (or a:fillRef through the theme) resolved.
CellFill ReadCellFill(pugi::xml_node el, const draw::ColorContext& colors, const draw::Theme& theme) {
  CellFill f;
  if (!el) return f;
  draw::ColorContext ctx = colors;
  if (LocalNameOf(el) == "fillRef") {
    ctx.placeholder = draw::ReadColor(el, colors);
    el = theme.Fill(static_cast<int>(Number(el, "idx", 0)));
    if (!el) return f;
  }
  std::string_view k = LocalNameOf(el);
  if (k == "solidFill") {
    if (std::optional<draw::Color> c = draw::ReadColor(el, ctx)) {
      f.kind = CellFill::Kind::Solid;
      f.color = *c;
    }
  } else if (k == "gradFill" || k == "pattFill" || k == "blipFill") {
    f.kind = CellFill::Kind::Other;
    f.node = el;
    std::optional<draw::Color> c = k == "gradFill"   ? draw::ReadColor(A(A(el, "gsLst"), "gs"), ctx)
                                   : k == "pattFill" ? draw::ReadColor(A(el, "fgClr"), ctx)
                                                     : std::nullopt;
    if (c) f.color = *c;
  }
  return f;
}

// The fill element among `parent`'s children, or null.
pugi::xml_node FillChild(pugi::xml_node parent) {
  pugi::xml_node found;
  ForEachChild(parent, [&](pugi::xml_node c) {
    std::string_view k = LocalNameOf(c);
    if (k == "noFill" || k == "solidFill" || k == "gradFill" || k == "blipFill" || k == "pattFill" || k == "fillRef") {
      found = c;
    }
    return !found;
  });
  return found;
}

// An a:ln, or a:lnRef through the theme, resolved.
CellBorder ReadBorder(pugi::xml_node el, const draw::ColorContext& colors, const draw::Theme& theme) {
  CellBorder b;
  if (!el) return b;
  draw::ColorContext ctx = colors;
  if (LocalNameOf(el) == "lnRef") {
    ctx.placeholder = draw::ReadColor(el, colors);
    el = theme.Line(static_cast<int>(Number(el, "idx", 0)));
    if (!el) return b;
  } else {
    b.line = el;
  }
  b.width = std::clamp(Emu(el, "w", 12700), 0.f, 1584.f);
  b.dash = A(el, "prstDash").attribute("val").value();
  if (b.dash == "solid") b.dash.clear();
  pugi::xml_node fill = FillChild(el);
  CellFill f = ReadCellFill(fill, ctx, theme);
  b.visible = f.kind != CellFill::Kind::None;
  b.color = f.color;
  return b;
}

// A rectangle `w` x `h` points filled with `fill` (a copy), for fwdraw: what a cell's gradient,
// picture or pattern fill draws.
pugi::xml_node FillShape(pugi::xml_document& scratch, pugi::xml_node fill, float w, float h) {
  pugi::xml_node sp = scratch.append_child("p:sp");
  sp.append_attribute("xmlns:p") = std::string(kP).c_str();
  sp.append_attribute("xmlns:a") = std::string(kA).c_str();
  sp.append_attribute("xmlns:r") = std::string(kR).c_str();
  pugi::xml_node nv = sp.append_child("p:nvSpPr");
  pugi::xml_node c = nv.append_child("p:cNvPr");
  c.append_attribute("id") = "0";
  c.append_attribute("name") = "";
  nv.append_child("p:cNvSpPr");
  nv.append_child("p:nvPr");
  pugi::xml_node spPr = sp.append_child("p:spPr");
  pugi::xml_node xfrm = spPr.append_child("a:xfrm");
  pugi::xml_node off = xfrm.append_child("a:off");
  off.append_attribute("x") = "0";
  off.append_attribute("y") = "0";
  pugi::xml_node ext = xfrm.append_child("a:ext");
  ext.append_attribute("cx") = static_cast<long long>(std::max(1.f, w) * 12700.0 + 0.5);
  ext.append_attribute("cy") = static_cast<long long>(std::max(1.f, h) * 12700.0 + 0.5);
  pugi::xml_node geom = spPr.append_child("a:prstGeom");
  geom.append_attribute("prst") = "rect";
  geom.append_child("a:avLst");
  CopyInto(spPr, fill);
  spPr.append_child("a:ln").append_child("a:noFill");
  return sp;
}

}  // namespace

const Table* Presentation::GetTable(const Shape& shape, const SlideBase& shownOn) {
  if (shape.kind != Shape::Kind::Table || !shape.owner) return nullptr;
  std::lock_guard lock(mutex_);
  auto key = std::make_pair(static_cast<const void*>(&shape), static_cast<const void*>(&shownOn));
  if (auto it = tables_.find(key); it != tables_.end()) return it->second.get();
  std::unique_ptr<Table>& slot = tables_[key];
  pugi::xml_node tbl = A(A(A(shape.node, "graphic"), "graphicData"), "tbl");
  if (!tbl) return nullptr;
  auto t = std::make_unique<Table>();
  t->node = tbl;
  t->owner = shape.owner;
  const draw::Theme& theme = shownOn.theme ? *shownOn.theme : *draw::Theme::Default();
  t->colors.theme = &theme;
  t->colors.colorMap = &shownOn.colorMap;

  // The grid.
  ForEachChild(A(tbl, "tblGrid"), [&](pugi::xml_node col) {
    if (LocalNameOf(col) == "gridCol") t->columns.push_back(std::clamp(Emu(col, "w"), 0.f, 4032.f));
    return t->columns.size() < kMaxColumns;
  });
  std::vector<pugi::xml_node> rows;
  ForEachChild(tbl, [&](pugi::xml_node tr) {
    if (LocalNameOf(tr) != "tr") return true;
    rows.push_back(tr);
    t->rows.push_back(std::clamp(Emu(tr, "h"), 0.f, 4032.f));
    return rows.size() < kMaxRows && rows.size() * t->columns.size() < kMaxCells;
  });
  const int R = static_cast<int>(t->rows.size()), C = static_cast<int>(t->columns.size());

  // The style: the deck's, a built-in one, or one written in the table.
  pugi::xml_node tblPr = A(tbl, "tblPr");
  t->rtl = Bool(tblPr, "rtl", false);
  bool firstRow = Bool(tblPr, "firstRow", false), lastRow = Bool(tblPr, "lastRow", false);
  bool firstCol = Bool(tblPr, "firstCol", false), lastCol = Bool(tblPr, "lastCol", false);
  bool bandRow = Bool(tblPr, "bandRow", false), bandCol = Bool(tblPr, "bandCol", false);
  pugi::xml_node style = A(tblPr, "tableStyle");
  if (!style) {
    std::string_view id = A(tblPr, "tableStyleId").text().get();
    if (!id.empty()) {
      if (!tableStylesRead_) {
        tableStylesRead_ = true;
        std::string part = TargetOfType(RelationshipsOf(presentationPart_), "tableStyles");
        tableStyles_ = DomOf(*this, part, nullptr);
      }
      if (tableStyles_) {
        ForEachChild(tableStyles_->Root(), [&](pugi::xml_node s) {
          if (LocalNameOf(s) == "tblStyle" && SameId(s.attribute("styleId").value(), id)) style = s;
          return !style;
        });
      }
    }
    if (!style) {
      // A built-in style the deck names without defining it. PowerPoint 2021 (probed 2026-11-20)
      // draws a table without a style, or with an id it doesn't know, as a black 1 pt grid with no
      // fills: Table Grid's look.
      const BuiltInStyle* b = &kBuiltInStyles[kTableGridStyle];
      unsigned char bytes[16];
      if (IdBytes(id, bytes)) {
        for (const BuiltInStyle& candidate : kBuiltInStyles) {
          if (std::equal(std::begin(bytes), std::end(bytes), std::begin(candidate.id))) b = &candidate;
        }
      }
      if (!tableScratch_) tableScratch_ = std::make_unique<pugi::xml_document>();
      std::string xml = std::string("<a:tblStyle xmlns:a=\"") + std::string(kA) + "\">" + BuiltInBody(*b) + "</a:tblStyle>";
      pugi::xml_document doc;
      if (doc.load_buffer(xml.data(), xml.size())) style = tableScratch_->append_copy(doc.document_element());
    }
  }
  t->background = ReadCellFill(FillChild(A(style, "tblBg")), t->colors, theme);

  // Every grid cell, then the spans.
  t->cells.resize(static_cast<size_t>(R) * static_cast<size_t>(C));
  for (int r = 0; r < R; ++r) {
    int c = 0;
    ForEachChild(rows[r], [&](pugi::xml_node tc) {
      if (LocalNameOf(tc) != "tc") return true;
      TableCell& cell = t->cells[static_cast<size_t>(r) * C + c];
      cell.node = tc;
      cell.row = r;
      cell.column = c;
      cell.columnSpan = std::clamp(static_cast<int>(Number(tc, "gridSpan", 1)), 1, C - c);
      cell.rowSpan = std::clamp(static_cast<int>(Number(tc, "rowSpan", 1)), 1, R - r);
      cell.covered = Bool(tc, "hMerge", false) || Bool(tc, "vMerge", false);
      return ++c < C;
    });
    for (; c < C; ++c) {
      TableCell& cell = t->cells[static_cast<size_t>(r) * C + c];
      cell.row = r;
      cell.column = c;
    }
  }
  for (TableCell& cell : t->cells) {
    if (cell.covered || (cell.rowSpan == 1 && cell.columnSpan == 1)) continue;
    for (int r = cell.row; r < cell.row + cell.rowSpan; ++r) {
      for (int c = cell.column; c < cell.column + cell.columnSpan; ++c) {
        if (r != cell.row || c != cell.column) t->cells[static_cast<size_t>(r) * C + c].covered = true;
      }
    }
  }

  // The style's parts for each cell, lowest first, each with the region its outer borders bound.
  struct Region {
    pugi::xml_node part;
    int r0, r1, c0, c1;
  };
  auto partOf = [&](const char* name) { return A(style, name); };
  for (TableCell& cell : t->cells) {
    const int r = cell.row, c = cell.column;
    const int rEnd = r + cell.rowSpan - 1, cEnd = c + cell.columnSpan - 1;
    std::vector<Region> regions;
    regions.push_back({partOf("wholeTbl"), 0, R - 1, 0, C - 1});
    if (bandRow && !(firstRow && r == 0) && !(lastRow && r == R - 1)) {
      int i = r - (firstRow ? 1 : 0);
      regions.push_back({partOf(i % 2 == 0 ? "band1H" : "band2H"), r, r, 0, C - 1});
    }
    if (bandCol && !(firstCol && c == 0) && !(lastCol && c == C - 1)) {
      int i = c - (firstCol ? 1 : 0);
      regions.push_back({partOf(i % 2 == 0 ? "band1V" : "band2V"), 0, R - 1, c, c});
    }
    if (lastCol && cEnd == C - 1) regions.push_back({partOf("lastCol"), 0, R - 1, C - 1, C - 1});
    if (firstCol && c == 0) regions.push_back({partOf("firstCol"), 0, R - 1, 0, 0});
    if (lastRow && rEnd == R - 1) regions.push_back({partOf("lastRow"), R - 1, R - 1, 0, C - 1});
    if (firstRow && r == 0) regions.push_back({partOf("firstRow"), 0, 0, 0, C - 1});
    if (lastRow && lastCol && rEnd == R - 1 && cEnd == C - 1) regions.push_back({partOf("seCell"), R - 1, R - 1, C - 1, C - 1});
    if (lastRow && firstCol && rEnd == R - 1 && c == 0) regions.push_back({partOf("swCell"), R - 1, R - 1, 0, 0});
    if (firstRow && lastCol && r == 0 && cEnd == C - 1) regions.push_back({partOf("neCell"), 0, 0, C - 1, C - 1});
    if (firstRow && firstCol && r == 0 && c == 0) regions.push_back({partOf("nwCell"), 0, 0, 0, 0});

    std::vector<pugi::xml_node> textStyles;
    for (const Region& g : regions) {
      if (!g.part) continue;
      if (pugi::xml_node tx = A(g.part, "tcTxStyle")) textStyles.push_back(tx);
      pugi::xml_node tcStyle = A(g.part, "tcStyle");
      if (pugi::xml_node fill = A(tcStyle, "fill")) cell.fill = ReadCellFill(FillChild(fill), t->colors, theme);
      if (pugi::xml_node ref = A(tcStyle, "fillRef")) cell.fill = ReadCellFill(ref, t->colors, theme);
      pugi::xml_node bdr = A(tcStyle, "tcBdr");
      auto edge = [&](int which, const char* name) {
        pugi::xml_node e = A(bdr, name);
        if (!e) return;
        pugi::xml_node line = A(e, "ln");
        if (!line) line = A(e, "lnRef");
        cell.borders[which] = ReadBorder(line, t->colors, theme);
      };
      edge(TableCell::Left, c == g.c0 ? "left" : "insideV");
      edge(TableCell::Right, cEnd == g.c1 ? "right" : "insideV");
      edge(TableCell::Top, r == g.r0 ? "top" : "insideH");
      edge(TableCell::Bottom, rEnd == g.r1 ? "bottom" : "insideH");
      edge(TableCell::TopLeftToBottomRight, "tl2br");
      edge(TableCell::BottomLeftToTopRight, "tr2bl");
    }

    // The cell's own properties.
    pugi::xml_node tcPr = A(cell.node, "tcPr");
    cell.marginLeft = std::clamp(Emu(tcPr, "marL", 91440), 0.f, 4032.f);
    cell.marginRight = std::clamp(Emu(tcPr, "marR", 91440), 0.f, 4032.f);
    cell.marginTop = std::clamp(Emu(tcPr, "marT", 45720), 0.f, 4032.f);
    cell.marginBottom = std::clamp(Emu(tcPr, "marB", 45720), 0.f, 4032.f);
    std::string_view anchor = tcPr.attribute("anchor").value();
    cell.anchor = anchor == "ctr"    ? BodyProps::Anchor::Middle
                  : anchor == "b"    ? BodyProps::Anchor::Bottom
                  : anchor == "just" ? BodyProps::Anchor::Justified
                  : anchor == "dist" ? BodyProps::Anchor::Distributed
                                     : BodyProps::Anchor::Top;
    std::string_view vert = tcPr.attribute("vert").value();
    cell.vertical = vert == "vert"           ? BodyProps::Vertical::Vertical
                    : vert == "vert270"      ? BodyProps::Vertical::Vertical270
                    : vert == "wordArtVert"  ? BodyProps::Vertical::WordArtVertical
                    : vert == "eaVert"       ? BodyProps::Vertical::EastAsian
                    : vert == "mongolianVert" ? BodyProps::Vertical::Mongolian
                                              : BodyProps::Vertical::Horizontal;
    const char* lines[] = {"lnL", "lnR", "lnT", "lnB", "lnTlToBr", "lnBlToTr"};
    for (int i = 0; i < 6; ++i) {
      if (pugi::xml_node ln = A(tcPr, lines[i])) cell.borders[i] = ReadBorder(ln, t->colors, theme);
    }
    if (pugi::xml_node fill = FillChild(tcPr)) cell.fill = ReadCellFill(fill, t->colors, theme);

    cell.textStyles = textStyles;
    if (pugi::xml_node txBody = A(cell.node, "txBody")) {
      cellTexts_.push_back(ResolvePlainText(*this, txBody, *shape.owner, shownOn, textStyles));
      cell.text = cellTexts_.back().get();
    }
  }
  // Gradient, picture and pattern fills as rectangles fwdraw draws, at the cells' sizes in the grid.
  auto shapeFor = [&](CellFill& f, float w, float h) {
    if (f.kind != CellFill::Kind::Other || !f.node) return;
    if (!tableScratch_) tableScratch_ = std::make_unique<pugi::xml_document>();
    f.shape = FillShape(*tableScratch_, f.node, w, h);
  };
  float width = 0, height = 0;
  for (float w : t->columns) width += w;
  for (float h : t->rows) height += h;
  shapeFor(t->background, width, height);
  for (TableCell& cell : t->cells) {
    float w = 0, h = 0;
    for (int c = cell.column; c < std::min(C, cell.column + cell.columnSpan); ++c) w += t->columns[c];
    for (int r = cell.row; r < std::min(R, cell.row + cell.rowSpan); ++r) h += t->rows[r];
    shapeFor(cell.fill, w, h);
  }
  slot = std::move(t);
  return slot.get();
}

}  // namespace fw::slide
