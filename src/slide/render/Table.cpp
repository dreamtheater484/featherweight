#include "slide/render/Table.h"

#include <algorithm>
#include <mutex>
#include <optional>

#include "core/Strings.h"
#include "draw/Xml.h"
#include "pkg/XmlDom.h"
#include "text/Fonts.h"

namespace fw::slide::render {

using namespace draw::xml;

namespace {

constexpr int kMaxRows = 1000, kMaxCols = 200;

// A border as a table draws it: a solid or dashed line, centered on the cell edge.
struct Border {
  bool set = false;  // a style or the cell said something (noFill included)
  std::optional<draw::Color> color;
  float width = 1;
  std::vector<float> dashes;
};

std::vector<float> Dashes(std::string_view v) {
  if (v == "dash" || v == "sysDash") return {3, 1};
  if (v == "dot" || v == "sysDot") return {1, 1};
  if (v == "lgDash") return {8, 3};
  if (v == "dashDot" || v == "sysDashDot") return {4, 3, 1, 3};
  if (v == "lgDashDot") return {8, 3, 1, 3};
  if (v == "lgDashDotDot" || v == "sysDashDotDot") return {8, 3, 1, 3, 1, 3};
  return {};
}

// a:ln (or a theme line through a:lnRef) into `b`.
void ReadLine(pugi::xml_node ln, const draw::ColorContext& colors, Border& b) {
  if (!ln) return;
  b.set = true;
  if (ln.attribute("w")) b.width = std::clamp(EmuToPt(Number(ln, "w", 12700)), 0.f, 100.f);
  ForEachChild(ln, [&](pugi::xml_node c) {
    std::string_view k = LocalNameOf(c);
    if (k == "noFill") b.color.reset();
    else if (k == "solidFill") b.color = draw::ReadColor(c, colors);
    else if (k == "gradFill") {
      if (pugi::xml_node gs = A(A(c, "gsLst"), "gs")) b.color = draw::ReadColor(gs, colors);
    } else if (k == "prstDash") b.dashes = Dashes(c.attribute("val").value());
    return true;
  });
}

// A border element of a table style (a:left, a:insideH…) or a cell's (a:lnL…): a:ln or a:lnRef.
void ReadBorder(pugi::xml_node e, const draw::ColorContext& colors, Border& b) {
  if (!e) return;
  if (pugi::xml_node ln = A(e, "ln")) {
    b = Border{};
    ReadLine(ln, colors, b);
  } else if (pugi::xml_node ref = A(e, "lnRef")) {
    b = Border{};
    b.set = true;
    draw::ColorContext c = colors;
    c.placeholder = draw::ReadColor(ref, colors);
    if (colors.theme) ReadLine(colors.theme->Line(static_cast<int>(Number(ref, "idx", 0))), c, b);
  } else if (LocalNameOf(e).starts_with("ln")) {
    b = Border{};
    ReadLine(e, colors, b);
  }
}

// A fill: solid colors (a gradient by its first stop), or none.
bool ReadFill(pugi::xml_node parent, const draw::ColorContext& colors, std::optional<draw::Color>& out) {
  bool said = false;
  ForEachChild(parent, [&](pugi::xml_node c) {
    if (NamespaceOf(c) != kA) return true;
    std::string_view k = LocalNameOf(c);
    if (k == "noFill") {
      out.reset();
      said = true;
    } else if (k == "solidFill") {
      out = draw::ReadColor(c, colors);
      said = true;
    } else if (k == "gradFill") {
      if (pugi::xml_node gs = A(A(c, "gsLst"), "gs")) out = draw::ReadColor(gs, colors);
      said = true;
    } else if (k == "pattFill") {
      out = draw::ReadColor(A(c, "bgClr"), colors);
      said = true;
    }
    return !said;
  });
  return said;
}

// a:fill or a:fillRef in a table style part.
void ReadStyleFill(pugi::xml_node tcStyle, const draw::ColorContext& colors, std::optional<draw::Color>& out) {
  if (pugi::xml_node f = A(tcStyle, "fill")) {
    ReadFill(f, colors, out);
  } else if (pugi::xml_node ref = A(tcStyle, "fillRef")) {
    draw::ColorContext c = colors;
    c.placeholder = draw::ReadColor(ref, colors);
    if (colors.theme) {
      std::optional<draw::Color> theme;
      pugi::xml_node fill = colors.theme->Fill(static_cast<int>(Number(ref, "idx", 0)));
      pugi::xml_node wrap = fill ? fill.parent() : pugi::xml_node();
      (void)wrap;
      if (fill && LocalNameOf(fill) == "solidFill") out = draw::ReadColor(fill, c);
      else if (fill && LocalNameOf(fill) == "gradFill") {
        if (pugi::xml_node gs = A(A(fill, "gsLst"), "gs")) out = draw::ReadColor(gs, c);
      } else if (fill && LocalNameOf(fill) == "noFill") out.reset();
      else out = c.placeholder;
    }
  }
}

struct CellLook {
  std::optional<draw::Color> fill;
  Border left, right, top, bottom, tl2br, tr2bl;
  std::optional<bool> bold, italic;
  std::optional<draw::Color> color;
  std::string face;
};

struct Cell {
  pugi::xml_node tc;
  int row = 0, col = 0, rows = 1, cols = 1;
  bool covered = false;  // merged into another cell
  CellLook look;
  draw::Rect rect;
  TextBlock text;
  draw::TextBox box;
};

}  // namespace

struct Table::Impl {
  std::vector<float> colX, rowY;  // edges
  std::vector<Cell> cells;
  std::optional<draw::Color> background;
};

Table::Table() : impl_(std::make_unique<Impl>()) {}
Table::~Table() = default;

std::unique_ptr<Table> Table::Read(pugi::xml_node tbl, draw::Size frame, pugi::xml_node tableStyles, const TextContext& textCtx,
                                   text::Fonts& fonts) {
  if (!tbl) return nullptr;
  std::unique_ptr<Table> t(new Table());
  Impl& m = *t->impl_;
  const draw::ColorContext& colors = textCtx.colors;
  // Columns and rows.
  std::vector<float> widths, heights;
  ForEachChild(A(tbl, "tblGrid"), [&](pugi::xml_node gc) {
    widths.push_back(std::clamp(EmuToPt(Number(gc, "w", 0)), 0.f, 10000.f));
    return widths.size() < kMaxCols;
  });
  std::vector<pugi::xml_node> rowNodes;
  ForEachChild(tbl, [&](pugi::xml_node tr) {
    if (LocalNameOf(tr) == "tr") {
      rowNodes.push_back(tr);
      heights.push_back(std::clamp(EmuToPt(Number(tr, "h", 0)), 0.f, 10000.f));
    }
    return rowNodes.size() < kMaxRows;
  });
  const int nRows = static_cast<int>(rowNodes.size()), nCols = static_cast<int>(widths.size());
  if (nRows == 0 || nCols == 0) return nullptr;

  // The style and the parts it turns on.
  pugi::xml_node tblPr = A(tbl, "tblPr");
  const std::string styleId = A(tblPr, "tableStyleId").text().get();
  pugi::xml_node style;
  const std::string wanted = styleId.empty() ? std::string(tableStyles.attribute("def").value()) : styleId;
  ForEachChild(tableStyles, [&](pugi::xml_node s) {
    if (std::string_view(s.attribute("styleId").value()) == wanted) style = s;
    return !style;
  });
  if (!style && !wanted.empty()) style = BuiltInTableStyle(wanted);
  if (!style && styleId.empty() && !tableStyles) style = pugi::xml_node();
  const bool firstRow = Bool(tblPr, "firstRow", false), lastRow = Bool(tblPr, "lastRow", false);
  const bool firstCol = Bool(tblPr, "firstCol", false), lastCol = Bool(tblPr, "lastCol", false);
  const bool bandRow = Bool(tblPr, "bandRow", false), bandCol = Bool(tblPr, "bandCol", false);
  ReadFill(tblPr, colors, m.background);
  if (!m.background) ReadStyleFill(A(style, "tblBg"), colors, m.background);

  // Cells, with merges.
  std::vector<std::vector<int>> grid(nRows, std::vector<int>(nCols, -1));
  for (int r = 0; r < nRows; ++r) {
    int c = 0;
    ForEachChild(rowNodes[r], [&](pugi::xml_node tc) {
      if (LocalNameOf(tc) != "tc" || c >= nCols) return c < nCols;
      Cell cell;
      cell.tc = tc;
      cell.row = r;
      cell.col = c;
      cell.cols = std::clamp(static_cast<int>(Number(tc, "gridSpan", 1)), 1, nCols - c);
      cell.rows = std::clamp(static_cast<int>(Number(tc, "rowSpan", 1)), 1, nRows - r);
      cell.covered = Bool(tc, "hMerge", false) || Bool(tc, "vMerge", false);
      grid[r][c] = static_cast<int>(m.cells.size());
      m.cells.push_back(std::move(cell));
      ++c;
      return true;
    });
  }

  // Each cell's look: the style's parts in order, then the cell's own properties.
  for (Cell& cell : m.cells) {
    CellLook& look = cell.look;
    const int r = cell.row, c = cell.col, r2 = cell.row + cell.rows - 1, c2 = cell.col + cell.cols - 1;
    auto apply = [&](std::string_view part, int top, int left, int bottom, int right) {
      pugi::xml_node p = A(style, part);
      if (!p) return;
      pugi::xml_node tcStyle = A(p, "tcStyle");
      ReadStyleFill(tcStyle, colors, look.fill);
      pugi::xml_node bdr = A(tcStyle, "tcBdr");
      ReadBorder(A(bdr, c == left ? "left" : "insideV"), colors, look.left);
      ReadBorder(A(bdr, c2 == right ? "right" : "insideV"), colors, look.right);
      ReadBorder(A(bdr, r == top ? "top" : "insideH"), colors, look.top);
      ReadBorder(A(bdr, r2 == bottom ? "bottom" : "insideH"), colors, look.bottom);
      ReadBorder(A(bdr, "tl2br"), colors, look.tl2br);
      ReadBorder(A(bdr, "tr2bl"), colors, look.tr2bl);
      pugi::xml_node tx = A(p, "tcTxStyle");
      if (!tx) return;
      if (pugi::xml_attribute b = tx.attribute("b"); b && std::string_view(b.value()) != "def") look.bold = std::string_view(b.value()) == "on";
      if (pugi::xml_attribute i = tx.attribute("i"); i && std::string_view(i.value()) != "def") look.italic = std::string_view(i.value()) == "on";
      if (pugi::xml_node font = A(tx, "fontRef")) {
        std::string_view idx = font.attribute("idx").value();
        look.face = idx == "major" ? "+mj-lt" : idx == "minor" ? "+mn-lt" : look.face;
        if (auto col = draw::ReadColor(font, colors)) look.color = col;
      } else if (pugi::xml_node f = A(tx, "font")) {
        if (pugi::xml_node latin = A(f, "latin")) look.face = latin.attribute("typeface").value();
      }
      ForEachChild(tx, [&](pugi::xml_node col) {
        std::string_view k = LocalNameOf(col);
        if (k != "font" && k != "fontRef" && k != "extLst")
          if (auto v = draw::ReadColorElement(col, colors)) look.color = v;
        return true;
      });
    };
    apply("wholeTbl", 0, 0, nRows - 1, nCols - 1);
    const int firstBody = firstRow ? 1 : 0, lastBody = lastRow ? nRows - 2 : nRows - 1;
    if (bandRow && r >= firstBody && r <= lastBody)
      apply((r - firstBody) % 2 == 0 ? "band1H" : "band2H", r, 0, r2, nCols - 1);
    const int firstBodyCol = firstCol ? 1 : 0, lastBodyCol = lastCol ? nCols - 2 : nCols - 1;
    if (bandCol && c >= firstBodyCol && c <= lastBodyCol)
      apply((c - firstBodyCol) % 2 == 0 ? "band1V" : "band2V", 0, c, nRows - 1, c2);
    if (lastCol && c2 == nCols - 1) apply("lastCol", 0, nCols - 1, nRows - 1, nCols - 1);
    if (firstCol && c == 0) apply("firstCol", 0, 0, nRows - 1, 0);
    if (lastRow && r2 == nRows - 1) apply("lastRow", nRows - 1, 0, nRows - 1, nCols - 1);
    if (firstRow && r == 0) apply("firstRow", 0, 0, 0, nCols - 1);
    if (firstRow && lastCol && r == 0 && c2 == nCols - 1) apply("neCell", 0, nCols - 1, 0, nCols - 1);
    if (firstRow && firstCol && r == 0 && c == 0) apply("nwCell", 0, 0, 0, 0);
    if (lastRow && lastCol && r2 == nRows - 1 && c2 == nCols - 1) apply("seCell", nRows - 1, nCols - 1, nRows - 1, nCols - 1);
    if (lastRow && firstCol && r2 == nRows - 1 && c == 0) apply("swCell", nRows - 1, 0, nRows - 1, 0);
    // The cell's own.
    pugi::xml_node tcPr = A(cell.tc, "tcPr");
    ReadFill(tcPr, colors, look.fill);
    ReadBorder(A(tcPr, "lnL"), colors, look.left);
    ReadBorder(A(tcPr, "lnR"), colors, look.right);
    ReadBorder(A(tcPr, "lnT"), colors, look.top);
    ReadBorder(A(tcPr, "lnB"), colors, look.bottom);
    ReadBorder(A(tcPr, "lnTlToBr"), colors, look.tl2br);
    ReadBorder(A(tcPr, "lnBlToTr"), colors, look.tr2bl);
  }

  // Text, laid out at the columns' widths; rows grow to fit it.
  m.colX.assign(nCols + 1, 0);
  for (int c = 0; c < nCols; ++c) m.colX[c + 1] = m.colX[c] + widths[c];
  for (Cell& cell : m.cells) {
    if (cell.covered) continue;
    pugi::xml_node tcPr = A(cell.tc, "tcPr");
    draw::TextBox& box = cell.box;
    box.content = A(cell.tc, "txBody");
    box.properties = A(box.content, "bodyPr");
    box.insets = {EmuToPt(Number(tcPr, "marL", 91440)), EmuToPt(Number(tcPr, "marT", 45720)), EmuToPt(Number(tcPr, "marR", 91440)),
                  EmuToPt(Number(tcPr, "marB", 45720))};
    std::string_view anchor = tcPr.attribute("anchor").value();
    box.anchor = anchor == "ctr" ? draw::TextBox::Anchor::Middle : anchor == "b" ? draw::TextBox::Anchor::Bottom : draw::TextBox::Anchor::Top;
    box.anchorCenter = Bool(tcPr, "anchorCtr", false);
    std::string_view vert = tcPr.attribute("vert").value();
    box.flow = vert == "vert" || vert == "eaVert" ? draw::TextBox::Flow::Vertical
               : vert == "vert270"               ? draw::TextBox::Flow::Vertical270
                                                 : draw::TextBox::Flow::Horizontal;
    const float w = m.colX[cell.col + cell.cols] - m.colX[cell.col];
    box.rect = {box.insets.left, box.insets.top, std::max(box.insets.left, w - box.insets.right), box.insets.top + 1};
    TextContext ctx = textCtx;
    ctx.bold = cell.look.bold;
    ctx.italic = cell.look.italic;
    ctx.color = cell.look.color;
    ctx.face = cell.look.face;
    cell.text.Build(fonts, box, ctx);
    if (cell.rows == 1 && box.flow == draw::TextBox::Flow::Horizontal)
      heights[cell.row] = std::max(heights[cell.row], cell.text.ContentHeight() + box.insets.top + box.insets.bottom);
  }
  m.rowY.assign(nRows + 1, 0);
  for (int r = 0; r < nRows; ++r) m.rowY[r + 1] = m.rowY[r] + heights[r];
  (void)frame;
  // Lay the text out again in the cells' final boxes, for anchoring.
  for (Cell& cell : m.cells) {
    cell.rect = {m.colX[cell.col], m.rowY[cell.row], m.colX[cell.col + cell.cols], m.rowY[cell.row + cell.rows]};
    if (cell.covered) continue;
    draw::TextBox& box = cell.box;
    box.rect = {cell.rect.left + box.insets.left, cell.rect.top + box.insets.top,
                std::max(cell.rect.left + box.insets.left, cell.rect.right - box.insets.right),
                std::max(cell.rect.top + box.insets.top, cell.rect.bottom - box.insets.bottom)};
    TextContext ctx = textCtx;
    ctx.bold = cell.look.bold;
    ctx.italic = cell.look.italic;
    ctx.color = cell.look.color;
    ctx.face = cell.look.face;
    cell.text.Build(fonts, box, ctx);
  }
  return t;
}

draw::Size Table::Extent() const { return {impl_->colX.back(), impl_->rowY.back()}; }

void Table::Draw(draw::Painter& p, DrawFilter* filter, uint32_t shape) const {
  const Impl& m = *impl_;
  const bool body = !filter || filter->ShapeBody(shape);
  if (body) {
    if (m.background) p.FillRect({0, 0, m.colX.back(), m.rowY.back()}, draw::Brush::Solid(*m.background));
    for (const Cell& c : m.cells)
      if (!c.covered && c.look.fill) p.FillRect(c.rect, draw::Brush::Solid(*c.look.fill));
  }
  int paragraph = 0;
  struct Counting : DrawFilter {
    DrawFilter* inner;
    int base = 0;
    bool Paragraph(uint32_t id, int index) override { return !inner || inner->Paragraph(id, base + index); }
  } counting;
  counting.inner = filter;
  for (const Cell& c : m.cells) {
    if (c.covered) continue;
    counting.base = paragraph;
    c.text.Draw(p, &counting, shape);
    paragraph += static_cast<int>(c.text.ParagraphBounds().size());
  }
  if (!body) return;
  auto line = [&](const Border& b, draw::Point a, draw::Point z) {
    if (!b.color || b.width <= 0) return;
    draw::Path path;
    path.MoveTo(a);
    path.LineTo(z);
    draw::Pen pen;
    pen.brush = draw::Brush::Solid(*b.color);
    pen.width = b.width;
    pen.dashes = b.dashes;
    p.StrokePath(path, pen);
  };
  for (const Cell& c : m.cells) {
    if (c.covered) continue;
    const draw::Rect& r = c.rect;
    line(c.look.top, {r.left, r.top}, {r.right, r.top});
    line(c.look.bottom, {r.left, r.bottom}, {r.right, r.bottom});
    line(c.look.left, {r.left, r.top}, {r.left, r.bottom});
    line(c.look.right, {r.right, r.top}, {r.right, r.bottom});
    line(c.look.tl2br, {r.left, r.top}, {r.right, r.bottom});
    line(c.look.tr2bl, {r.right, r.top}, {r.left, r.bottom});
  }
}

std::vector<draw::Rect> Table::ParagraphBounds() const {
  std::vector<draw::Rect> out;
  for (const Cell& c : impl_->cells) {
    if (c.covered) continue;
    for (const draw::Rect& r : c.text.ParagraphBounds()) out.push_back(r);
  }
  return out;
}

std::vector<TextBlock::Link> Table::Links() const {
  std::vector<TextBlock::Link> out;
  for (const Cell& c : impl_->cells)
    if (!c.covered)
      for (TextBlock::Link& l : c.text.Links()) out.push_back(std::move(l));
  return out;
}

std::wstring Table::PlainText() const {
  std::wstring out;
  int row = -1;
  for (const Cell& c : impl_->cells) {
    if (c.covered) continue;
    if (row >= 0) out += c.row != row ? L'\n' : L'\t';
    row = c.row;
    out += c.text.PlainText();
  }
  return out;
}

// ---- PowerPoint's built-in table styles ----

namespace {

// Medium Style 2, the style PowerPoint gives new tables, with `{c}` for its color.
constexpr const char* kMedium2 =
    "<a:tblStyle xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\">"
    "<a:wholeTbl><a:tcTxStyle><a:fontRef idx=\"minor\"><a:prstClr val=\"black\"/></a:fontRef><a:schemeClr val=\"dk1\"/></a:tcTxStyle>"
    "<a:tcStyle><a:tcBdr>"
    "<a:left><a:ln w=\"12700\" cmpd=\"sng\"><a:solidFill><a:schemeClr val=\"lt1\"/></a:solidFill></a:ln></a:left>"
    "<a:right><a:ln w=\"12700\" cmpd=\"sng\"><a:solidFill><a:schemeClr val=\"lt1\"/></a:solidFill></a:ln></a:right>"
    "<a:top><a:ln w=\"12700\" cmpd=\"sng\"><a:solidFill><a:schemeClr val=\"lt1\"/></a:solidFill></a:ln></a:top>"
    "<a:bottom><a:ln w=\"12700\" cmpd=\"sng\"><a:solidFill><a:schemeClr val=\"lt1\"/></a:solidFill></a:ln></a:bottom>"
    "<a:insideH><a:ln w=\"12700\" cmpd=\"sng\"><a:solidFill><a:schemeClr val=\"lt1\"/></a:solidFill></a:ln></a:insideH>"
    "<a:insideV><a:ln w=\"12700\" cmpd=\"sng\"><a:solidFill><a:schemeClr val=\"lt1\"/></a:solidFill></a:ln></a:insideV>"
    "</a:tcBdr><a:fill><a:solidFill><a:schemeClr val=\"{c}\"><a:tint val=\"20000\"/></a:schemeClr></a:solidFill></a:fill></a:tcStyle></a:wholeTbl>"
    "<a:band1H><a:tcStyle><a:tcBdr/><a:fill><a:solidFill><a:schemeClr val=\"{c}\"><a:tint val=\"40000\"/></a:schemeClr></a:solidFill></a:fill></a:tcStyle></a:band1H>"
    "<a:band2H><a:tcStyle><a:tcBdr/></a:tcStyle></a:band2H>"
    "<a:band1V><a:tcStyle><a:tcBdr/><a:fill><a:solidFill><a:schemeClr val=\"{c}\"><a:tint val=\"40000\"/></a:schemeClr></a:solidFill></a:fill></a:tcStyle></a:band1V>"
    "<a:band2V><a:tcStyle><a:tcBdr/></a:tcStyle></a:band2V>"
    "<a:lastCol><a:tcTxStyle b=\"on\"><a:fontRef idx=\"minor\"><a:prstClr val=\"black\"/></a:fontRef><a:schemeClr val=\"lt1\"/></a:tcTxStyle>"
    "<a:tcStyle><a:tcBdr/><a:fill><a:solidFill><a:schemeClr val=\"{c}\"/></a:solidFill></a:fill></a:tcStyle></a:lastCol>"
    "<a:firstCol><a:tcTxStyle b=\"on\"><a:fontRef idx=\"minor\"><a:prstClr val=\"black\"/></a:fontRef><a:schemeClr val=\"lt1\"/></a:tcTxStyle>"
    "<a:tcStyle><a:tcBdr/><a:fill><a:solidFill><a:schemeClr val=\"{c}\"/></a:solidFill></a:fill></a:tcStyle></a:firstCol>"
    "<a:lastRow><a:tcTxStyle b=\"on\"><a:fontRef idx=\"minor\"><a:prstClr val=\"black\"/></a:fontRef><a:schemeClr val=\"lt1\"/></a:tcTxStyle>"
    "<a:tcStyle><a:tcBdr><a:top><a:ln w=\"38100\" cmpd=\"sng\"><a:solidFill><a:schemeClr val=\"lt1\"/></a:solidFill></a:ln></a:top></a:tcBdr>"
    "<a:fill><a:solidFill><a:schemeClr val=\"{c}\"/></a:solidFill></a:fill></a:tcStyle></a:lastRow>"
    "<a:firstRow><a:tcTxStyle b=\"on\"><a:fontRef idx=\"minor\"><a:prstClr val=\"black\"/></a:fontRef><a:schemeClr val=\"lt1\"/></a:tcTxStyle>"
    "<a:tcStyle><a:tcBdr><a:bottom><a:ln w=\"38100\" cmpd=\"sng\"><a:solidFill><a:schemeClr val=\"lt1\"/></a:solidFill></a:ln></a:bottom></a:tcBdr>"
    "<a:fill><a:solidFill><a:schemeClr val=\"{c}\"/></a:solidFill></a:fill></a:tcStyle></a:firstRow></a:tblStyle>";

struct BuiltIn {
  const char* id;
  const char* templ;
  const char* color;
};

constexpr BuiltIn kBuiltIns[] = {
    {"{073A0DAA-6AF3-43AB-8588-CEC1D06C72B9}", kMedium2, "dk1"},
    {"{5C22544A-7EE6-4342-B048-85BDC9FD1C3A}", kMedium2, "accent1"},
    {"{21E4AEA4-8DFA-4A89-87EB-49C32662AFE8}", kMedium2, "accent2"},
    {"{F5AB1C69-6EDB-4FF4-983F-18BD219EF322}", kMedium2, "accent3"},
    {"{00A15C55-8517-42AA-B614-E9B94910E393}", kMedium2, "accent4"},
    {"{7DF18680-E054-41AD-8BC1-D1AEF772440D}", kMedium2, "accent5"},
    {"{93296810-A885-4BE3-A3E7-6D5BEEA58F35}", kMedium2, "accent6"},
};

}  // namespace

pugi::xml_node BuiltInTableStyle(std::string_view id) {
  static std::mutex mutex;
  static std::vector<std::pair<std::string, std::unique_ptr<fw::pkg::XmlDom>>> parsed;
  std::lock_guard<std::mutex> lock(mutex);
  for (const auto& [key, dom] : parsed)
    if (key == id) return dom ? dom->Root() : pugi::xml_node();
  std::unique_ptr<fw::pkg::XmlDom> dom;
  for (const BuiltIn& b : kBuiltIns) {
    if (id != b.id) continue;
    std::string xml = b.templ;
    for (size_t at = xml.find("{c}"); at != std::string::npos; at = xml.find("{c}", at)) xml.replace(at, 3, b.color);
    dom = std::make_unique<fw::pkg::XmlDom>();
    if (!dom->Parse(xml).Ok()) dom.reset();
    break;
  }
  pugi::xml_node root = dom ? dom->Root() : pugi::xml_node();
  parsed.emplace_back(std::string(id), std::move(dom));
  return root;
}

}  // namespace fw::slide::render
