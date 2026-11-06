// OpenDocument text (.odt) to WordprocessingML: the parts a .docx needs, written from the ODF
// content and styles, zipped. The .docx reader then opens it as any Word document, so everything
// downstream (layout, editing, saving) is the same code.
#include "doc/odf/Odt.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "pkg/Snapshot.h"
#include "pkg/XmlDom.h"
#include "pkg/Zip.h"

namespace fw::doc::odf {
namespace {

using pugi::xml_node;

constexpr std::string_view kDecl = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n";
constexpr std::string_view kNs =
    "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
    "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
    "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
    "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
    "xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"";
constexpr std::string_view kWml = "application/vnd.openxmlformats-officedocument.wordprocessingml.";
constexpr std::string_view kRel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
constexpr int kMaxDepth = 64;  // nested lists, tables, spans: deeper content is written flat

std::string Esc(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
  return out;
}

std::string_view Attr(xml_node n, const char* name) { return n.attribute(name).value(); }

// ODF lengths ("2.5cm", "0.5in", "12pt") in points.
std::optional<double> Points(std::string_view v) {
  size_t i = 0;
  while (i < v.size() && (std::isdigit(static_cast<unsigned char>(v[i])) || v[i] == '.' || v[i] == '-' || v[i] == '+')) ++i;
  if (i == 0) return std::nullopt;
  double n = 0;
  try {
    n = std::stod(std::string(v.substr(0, i)));
  } catch (...) {
    return std::nullopt;
  }
  if (!std::isfinite(n) || std::abs(n) > 1e6) return std::nullopt;
  const std::string_view unit = v.substr(i);
  if (unit == "pt") return n;
  if (unit == "cm") return n * 72 / 2.54;
  if (unit == "mm") return n * 72 / 25.4;
  if (unit == "in" || unit == "inch") return n * 72;
  if (unit == "pc") return n * 12;
  if (unit == "px") return n * 0.75;
  return std::nullopt;
}
std::optional<int> Twips(std::string_view v) {
  auto p = Points(v);
  if (!p) return std::nullopt;
  return static_cast<int>(std::lround(*p * 20));
}
int64_t Emu(std::string_view v, int64_t fallback) {
  auto p = Points(v);
  return p && *p > 0 ? static_cast<int64_t>(*p * 12700) : fallback;
}
// "#1f4e79" as "1F4E79"; empty for "transparent" or anything else.
std::string Color(std::string_view v) {
  if (v.size() != 7 || v[0] != '#') return {};
  std::string out;
  for (char c : v.substr(1)) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) return {};
    out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return out;
}
// An encoded ODF name as people read it: "_20_" is a space.
std::string Display(std::string_view name) {
  std::string out;
  for (size_t i = 0; i < name.size(); ++i) {
    if (name[i] == '_' && i + 3 < name.size() && name[i + 3] == '_' && std::isxdigit(static_cast<unsigned char>(name[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(name[i + 2]))) {
      const int c = std::stoi(std::string(name.substr(i + 1, 2)), nullptr, 16);
      if (c >= 0x20 && c < 0x7F) {
        out += static_cast<char>(c);
        i += 3;
        continue;
      }
    }
    out += name[i];
  }
  return out;
}

// A style name as Word's style id: letters and digits ("Text_20_body" is "Textbody").
std::string StyleId(std::string_view name) {
  if (name == "Standard") return "Normal";
  if (name.starts_with("Heading_20_") && name.size() == 12 && name[11] >= '1' && name[11] <= '9')
    return "Heading" + std::string(1, name[11]);
  std::string out;
  for (char c : Display(name))
    if (std::isalnum(static_cast<unsigned char>(c))) out += c;
  return out.empty() ? "Style" : out;
}
// ---------- Properties ----------

struct RunProps {
  std::string font, color, shade, lang, vert;
  int size = 0;  // half-points
  int b = -1, i = -1, caps = -1, smallCaps = -1, strike = -1, u = -1;  // -1: not set
  std::optional<int> spacing;  // fo:letter-spacing, twips ("normal" is 0)
  int scale = 0;               // style:text-scale, percent (0: not set)

  void Read(xml_node p, const std::unordered_map<std::string, std::string>& fonts) {
    if (!p) return;
    if (auto f = Attr(p, "style:font-name"); !f.empty()) {
      auto it = fonts.find(std::string(f));
      font = it != fonts.end() ? it->second : std::string(f);
    } else if (auto ff = Attr(p, "fo:font-family"); !ff.empty()) {
      font = std::string(ff);
    }
    font.erase(std::remove(font.begin(), font.end(), '\''), font.end());
    if (auto s = Points(Attr(p, "fo:font-size"))) size = std::clamp(static_cast<int>(std::lround(*s * 2)), 2, 3276);
    if (auto w = Attr(p, "fo:font-weight"); !w.empty()) b = w == "bold" || (std::isdigit(static_cast<unsigned char>(w[0])) && std::atoi(std::string(w).c_str()) >= 600);
    if (auto s = Attr(p, "fo:font-style"); !s.empty()) i = s == "italic" || s == "oblique";
    if (auto v = Attr(p, "fo:font-variant"); !v.empty()) smallCaps = v == "small-caps";
    if (auto t = Attr(p, "fo:text-transform"); !t.empty()) caps = t == "uppercase";
    if (auto s = Attr(p, "style:text-line-through-style"); !s.empty()) strike = s != "none";
    if (auto s = Attr(p, "style:text-underline-style"); !s.empty()) u = s == "none" ? 0 : Attr(p, "style:text-underline-type") == "double" ? 2 : 1;
    if (auto c = Attr(p, "fo:color"); !c.empty()) color = Color(c);
    if (auto ls = Attr(p, "fo:letter-spacing"); ls == "normal") spacing = 0;
    else if (auto v = Twips(ls)) spacing = std::clamp(*v, -31680, 31680);
    if (auto ts = Attr(p, "style:text-scale"); !ts.empty() && ts.back() == '%')
      scale = std::clamp(std::atoi(std::string(ts).c_str()), 1, 600);
    if (auto c = Attr(p, "fo:background-color"); !c.empty()) shade = Color(c);
    if (auto pos = Attr(p, "style:text-position"); !pos.empty())
      vert = pos.starts_with("super") || (pos[0] != '-' && pos[0] != '0' && std::isdigit(static_cast<unsigned char>(pos[0]))) ? "superscript"
             : pos.starts_with("sub") || pos[0] == '-' ? "subscript"
                                                         : "baseline";
    if (auto l = Attr(p, "fo:language"); !l.empty() && l != "zxx" && l.size() <= 8) {
      lang = std::string(l);
      if (auto c = Attr(p, "fo:country"); !c.empty() && c != "none" && c.size() <= 8) lang += "-" + std::string(c);
    }
  }
  void Over(const RunProps& o) {
    if (!o.font.empty()) font = o.font;
    if (!o.color.empty()) color = o.color;
    if (!o.shade.empty()) shade = o.shade;
    if (!o.lang.empty()) lang = o.lang;
    if (!o.vert.empty()) vert = o.vert;
    if (o.size) size = o.size;
    if (o.spacing) spacing = o.spacing;
    if (o.scale) scale = o.scale;
    for (auto [mine, theirs] : {std::pair{&b, o.b}, {&i, o.i}, {&caps, o.caps}, {&smallCaps, o.smallCaps}, {&strike, o.strike}, {&u, o.u}})
      if (theirs >= 0) *mine = theirs;
  }
  // w:rPr's children, in the schema's order.
  std::string Xml(std::string_view rStyle = {}) const {
    std::string s;
    if (!rStyle.empty()) s += "<w:rStyle w:val=\"" + Esc(rStyle) + "\"/>";
    if (!font.empty()) {
      const std::string f = Esc(font);
      s += "<w:rFonts w:ascii=\"" + f + "\" w:hAnsi=\"" + f + "\" w:cs=\"" + f + "\"/>";
    }
    auto flag = [&](int v, const char* name) {
      if (v == 1) s += std::string("<w:") + name + "/>";
      else if (v == 0) s += std::string("<w:") + name + " w:val=\"0\"/>";
    };
    flag(b, "b");
    flag(i, "i");
    flag(caps, "caps");
    flag(smallCaps, "smallCaps");
    flag(strike, "strike");
    if (!color.empty()) s += "<w:color w:val=\"" + color + "\"/>";
    if (spacing) s += "<w:spacing w:val=\"" + std::to_string(*spacing) + "\"/>";
    if (scale) s += "<w:w w:val=\"" + std::to_string(scale) + "\"/>";
    if (size) s += "<w:sz w:val=\"" + std::to_string(size) + "\"/><w:szCs w:val=\"" + std::to_string(size) + "\"/>";
    if (u >= 0) s += std::string("<w:u w:val=\"") + (u == 0 ? "none" : u == 2 ? "double" : "single") + "\"/>";
    if (!shade.empty()) s += "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"" + shade + "\"/>";
    if (!vert.empty()) s += "<w:vertAlign w:val=\"" + vert + "\"/>";
    if (!lang.empty()) s += "<w:lang w:val=\"" + Esc(lang) + "\"/>";
    return s;
  }
};

std::string Border(const char* side, std::string_view v);

struct ParaProps {
  std::string jc, shade, tabs;
  std::array<std::string, 4> border;     // ODF's borders: top, left, bottom, right
  std::array<std::optional<double>, 4> padding;  // points
  std::optional<int> before, after, left, right, first, line, outline;
  std::string lineRule;
  bool keepNext = false, pageBreak = false;

  void Read(xml_node p) {
    if (!p) return;
    if (auto a = Attr(p, "fo:text-align"); !a.empty())
      jc = a == "center" ? "center" : a == "justify" ? "both" : a == "end" || a == "right" ? "right" : "left";
    before = Twips(Attr(p, "fo:margin-top")).value_or(before.value_or(-1));
    after = Twips(Attr(p, "fo:margin-bottom")).value_or(after.value_or(-1));
    if (before && *before < 0) before.reset();
    if (after && *after < 0) after.reset();
    if (auto v = Twips(Attr(p, "fo:margin-left"))) left = *v;
    if (auto v = Twips(Attr(p, "fo:margin-right"))) right = *v;
    if (auto v = Twips(Attr(p, "fo:text-indent"))) first = *v;
    if (auto lh = Attr(p, "fo:line-height"); !lh.empty() && lh != "normal") {
      if (lh.back() == '%') {
        line = std::clamp(std::atoi(std::string(lh).c_str()) * 240 / 100, 24, 2400);
        lineRule = "auto";
      } else if (auto v = Twips(lh)) {
        line = *v;
        lineRule = "exact";
      }
    } else if (auto v = Twips(Attr(p, "style:line-height-at-least"))) {
      line = *v;
      lineRule = "atLeast";
    }
    if (auto c = Attr(p, "fo:background-color"); !c.empty()) shade = Color(c);
    static const char* const kBorder[] = {"fo:border-top", "fo:border-left", "fo:border-bottom", "fo:border-right"};
    static const char* const kPadding[] = {"fo:padding-top", "fo:padding-left", "fo:padding-bottom", "fo:padding-right"};
    for (size_t k = 0; k < 4; ++k) {
      if (auto b = Attr(p, kBorder[k]); !b.empty()) border[k] = std::string(b);
      else if (auto all = Attr(p, "fo:border"); !all.empty()) border[k] = std::string(all);
      if (auto v = Points(Attr(p, kPadding[k]))) padding[k] = *v;
      else if (auto all = Points(Attr(p, "fo:padding"))) padding[k] = *all;
    }
    if (Attr(p, "fo:keep-with-next") == "always") keepNext = true;
    if (Attr(p, "fo:break-before") == "page") pageBreak = true;
    std::string t;
    for (xml_node stop : p.child("style:tab-stops").children("style:tab-stop")) {
      auto pos = Twips(Attr(stop, "style:position"));
      if (!pos || *pos < 0 || *pos > 31680) continue;
      const std::string_view type = Attr(stop, "style:type");
      t += std::string("<w:tab w:val=\"") + (type == "center" ? "center" : type == "right" ? "right" : type == "char" ? "decimal" : "left") +
           "\" w:pos=\"" + std::to_string(*pos + left.value_or(0)) + "\"/>";
    }
    if (!t.empty()) tabs = "<w:tabs>" + t + "</w:tabs>";
  }
  // w:pPr's children, in the schema's order.
  std::string Xml(std::string_view pStyle, std::string_view numPr, std::string_view markRun) const {
    std::string s;
    if (!pStyle.empty()) s += "<w:pStyle w:val=\"" + Esc(pStyle) + "\"/>";
    if (keepNext) s += "<w:keepNext/>";
    if (pageBreak) s += "<w:pageBreakBefore/>";
    s += numPr;
    // Borders, each its padding from the text as w:space (whole points, at most 31).
    if (std::any_of(border.begin(), border.end(), [](const std::string& b) { return !b.empty(); })) {
      static const char* const kSide[] = {"top", "left", "bottom", "right"};
      std::string b;
      for (size_t k = 0; k < 4; ++k) {
        std::string one = Border(kSide[k], border[k]);
        const size_t at = one.find(" w:space=\"0\"");
        if (at != std::string::npos && padding[k])
          one.replace(at, 12, " w:space=\"" + std::to_string(std::clamp(static_cast<int>(std::lround(*padding[k])), 0, 31)) + "\"");
        b += one;
      }
      if (!b.empty()) s += "<w:pBdr>" + b + "</w:pBdr>";
    }
    if (!shade.empty()) s += "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"" + shade + "\"/>";
    s += tabs;
    if (before || after || line) {
      s += "<w:spacing";
      if (before) s += " w:before=\"" + std::to_string(std::min(*before, 31680)) + "\"";
      if (after) s += " w:after=\"" + std::to_string(std::min(*after, 31680)) + "\"";
      if (line) s += " w:line=\"" + std::to_string(*line) + "\" w:lineRule=\"" + lineRule + "\"";
      s += "/>";
    }
    if (left || right || first) {
      s += "<w:ind";
      if (left) s += " w:left=\"" + std::to_string(std::clamp(*left, -31680, 31680)) + "\"";
      if (right) s += " w:right=\"" + std::to_string(std::clamp(*right, -31680, 31680)) + "\"";
      if (first && *first >= 0) s += " w:firstLine=\"" + std::to_string(std::min(*first, 31680)) + "\"";
      if (first && *first < 0) s += " w:hanging=\"" + std::to_string(std::min(-*first, 31680)) + "\"";
      s += "/>";
    }
    if (!jc.empty()) s += "<w:jc w:val=\"" + jc + "\"/>";
    if (outline) s += "<w:outlineLvl w:val=\"" + std::to_string(*outline) + "\"/>";
    if (!markRun.empty()) s += "<w:rPr>" + std::string(markRun) + "</w:rPr>";
    return s;
  }
};

// ODF's borders ("0.5pt solid #000000") as WordprocessingML's.
std::string Border(const char* side, std::string_view v) {
  if (v.empty()) return {};
  if (v == "none") return std::string("<w:") + side + " w:val=\"nil\"/>";
  std::string width, style = "single", color = "auto";
  size_t at = 0;
  while (at < v.size()) {
    size_t end = v.find(' ', at);
    if (end == std::string_view::npos) end = v.size();
    const std::string_view word = v.substr(at, end - at);
    if (auto c = Color(word); !c.empty()) color = c;
    else if (Points(word)) width = std::string(word);
    else if (word == "double") style = "double";
    else if (word == "dashed") style = "dashed";
    else if (word == "dotted") style = "dotted";
    at = end + 1;
  }
  const int eighths = std::clamp(static_cast<int>(std::lround(Points(width).value_or(0.5) * 8)), 2, 96);
  return std::string("<w:") + side + " w:val=\"" + style + "\" w:sz=\"" + std::to_string(eighths) +
         "\" w:space=\"0\" w:color=\"" + color + "\"/>";
}

// ---------- The converter ----------

struct Style {
  std::string family, name, parent, list, master;
  xml_node node;
  int outline = 0;
};

struct OutPart {
  std::string name, xml;  // the part's path in the package and its content
  std::vector<std::pair<std::string, std::string>> rels;  // id -> relationship element
};

class Converter {
 public:
  Converter(const pkg::ZipReader& zip, const pkg::Limits& limits) : zip_(zip), limits_(limits) {}
  pkg::Status Run(std::vector<uint8_t>& docx);

 private:
  pkg::Status Load(const char* name, pkg::XmlDom& dom, bool required);
  void ReadStyles(xml_node root, int scope);
  const Style* Find(std::string_view family, std::string_view name) const;
  std::string Mapped(std::string_view family, std::string_view name) const;

  // Block content into `out`.
  void Blocks(xml_node parent, std::string& out, int depth);
  void Paragraph(xml_node p, std::string& out, int depth, std::string_view listStyle = {}, int level = -1);
  void List(xml_node list, std::string& out, int depth, std::string listStyle, int level);
  void Table(xml_node table, std::string& out, int depth);
  // Inline content of a paragraph into `out` (runs); text frames' paragraphs into `after`.
  struct Inline {
    RunProps props;
    std::string rStyle;
    bool link = false;
    bool space = true;  // the last character was white space (or the paragraph's start)
  };
  void Runs(xml_node parent, Inline& in, std::string& out, std::string& after, int depth);
  void Text(std::string_view text, Inline& in, std::string& out);
  void Picture(xml_node frame, std::string& out);
  void Note(xml_node note, const Inline& in, std::string& out, int depth);

  std::string Rel(std::string type, std::string target, bool external = false);
  int Num(std::string_view listStyle, bool restart);
  std::string NumPr(std::string_view listStyle, int level, bool restart);
  std::string SectPr();
  int TextWidth() const;  // the section's text width, twips
  // A paragraph that starts another page style: the section before it ends with the paragraph before.
  void SectionBreak(std::string& out, std::string_view master, std::string_view pageNumber);
  std::string StylesXml();
  std::string NumberingXml();

  const pkg::ZipReader& zip_;
  const pkg::Limits& limits_;
  pkg::XmlDom content_, styles_;
  std::unordered_map<std::string, std::string> fonts_;
  std::map<std::string, xml_node> faces_;  // each font's declaration, by its family name: the font table
  std::string FontTableXml() const;
  // Bookmarks' text, for references that show it (reference-format "text"): read before the body is written.
  void BookmarkTexts(xml_node parent, int depth);
  std::unordered_map<std::string, std::string> refText_, openText_;
  size_t refBudget_ = 4 << 20;  // the most text references may repeat, in bytes
  size_t refCollect_ = 16 << 20;  // the most bookmark text kept
  std::unordered_map<std::string, Style> named_, autos_[2];  // "family\nname"
  std::unordered_map<std::string, xml_node> listStyles_[2], pageLayouts_;
  xml_node outline_, defaults_, master_;  // master_: the page style of the section being written
  int pageStart_ = 0;                      // its first page's number, when it starts again (0: it doesn't)
  std::string* body_ = nullptr;            // the body's blocks, where sections end
  int scope_ = 0;  // 0: the body's automatic styles; 1: styles.xml's (headers and footers)

  std::vector<OutPart> parts_;
  size_t part_ = 0;  // the part being written (its relationships)
  std::unordered_map<std::string, std::string> media_;  // ODF path -> word/media/...
  std::vector<std::string> mediaTypes_;
  std::unordered_map<std::string, int> bookmarks_;  // name -> id while open, -1 once ended
  std::map<int, size_t> open_;                       // bookmarks started and not ended: id -> part
  // The ends of this part's bookmarks that never ended, at its end.
  void CloseBookmarks(std::string& out) {
    for (auto it = open_.begin(); it != open_.end();) {
      if (it->second != part_) {
        ++it;
        continue;
      }
      out += "<w:bookmarkEnd w:id=\"" + std::to_string(it->first) + "\"/>";
      it = open_.erase(it);
    }
  }
  int nextBookmark_ = 0, nextDocPr_ = 1, nextFootnote_ = 1, nextEndnote_ = 1;
  std::string footnotes_, endnotes_, notePrefix_;
  // Lists: an abstract numbering per ODF list style, a w:num per list that restarts.
  std::vector<std::string> abstracts_;
  std::unordered_map<std::string, int> abstractOf_, lastNum_;
  std::vector<std::pair<int, int>> nums_;  // abstract, and the number it starts again at (0: none)
  std::vector<int> starts_;                // each abstract's first level's start
  bool firstParagraph_ = true;
};

pkg::Status Converter::Load(const char* name, pkg::XmlDom& dom, bool required) {
  const pkg::ZipEntry* e = zip_.Find(name);
  if (!e) {
    return required ? pkg::Status::Fail(pkg::Status::Code::Damaged, "This isn't an OpenDocument text document.")
                    : pkg::Status{};
  }
  std::vector<uint8_t> bytes;
  if (pkg::Status st = zip_.Read(*e, bytes); !st.Ok()) return st;
  if (pkg::Status st = dom.Parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), limits_, name);
      !st.Ok())
    return st;
  // ODF's usual prefixes, whatever the file declares (pugixml keeps names as written).
  static const std::pair<std::string_view, std::string_view> kCanon[] = {
      {"urn:oasis:names:tc:opendocument:xmlns:office:1.0", "office"},
      {"urn:oasis:names:tc:opendocument:xmlns:style:1.0", "style"},
      {"urn:oasis:names:tc:opendocument:xmlns:text:1.0", "text"},
      {"urn:oasis:names:tc:opendocument:xmlns:table:1.0", "table"},
      {"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0", "draw"},
      {"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0", "fo"},
      {"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0", "svg"},
      {"http://www.w3.org/1999/xlink", "xlink"}};
  std::map<std::string, std::string> renames;
  for (pugi::xml_attribute a : dom.Root().attributes()) {
    const std::string_view n = a.name();
    if (!n.starts_with("xmlns:")) continue;
    for (const auto& [uri, prefix] : kCanon)
      if (uri == a.value() && n.substr(6) != prefix) renames[std::string(n.substr(6))] = std::string(prefix);
  }
  if (renames.empty()) return {};
  auto rename = [&](auto&& item) {
    const std::string_view n = item.name();
    const size_t colon = n.find(':');
    if (colon == std::string_view::npos || n.starts_with("xmlns")) return;
    if (auto it = renames.find(std::string(n.substr(0, colon))); it != renames.end())
      item.set_name((it->second + std::string(n.substr(colon))).c_str());
  };
  for (xml_node n = dom.Root(); n;) {
    if (n.type() == pugi::node_element) {
      rename(n);
      for (pugi::xml_attribute a : n.attributes()) rename(a);
    }
    if (n.first_child()) {
      n = n.first_child();
      continue;
    }
    while (n && !n.next_sibling() && n != dom.Root()) n = n.parent();
    if (!n || n == dom.Root()) break;
    n = n.next_sibling();
  }
  return {};
}

void Converter::ReadStyles(xml_node root, int scope) {
  for (xml_node f : root.child("office:font-face-decls").children("style:font-face"))
    if (auto fam = Attr(f, "svg:font-family"); !fam.empty()) {
      fonts_[f.attribute("style:name").value()] = std::string(fam);
      std::string name(fam);
      name.erase(std::remove(name.begin(), name.end(), '\''), name.end());
      if (!name.empty() && !faces_.count(name)) faces_[name] = f;
    }
  auto read = [&](xml_node group, bool automatic) {
    for (xml_node s : group.children()) {
      const std::string_view n = s.name();
      if (n == "style:style") {
        Style st{std::string(Attr(s, "style:family")), std::string(Attr(s, "style:name")),
                 std::string(Attr(s, "style:parent-style-name")), std::string(Attr(s, "style:list-style-name")),
                 std::string(Attr(s, "style:master-page-name")), s, std::atoi(Attr(s, "style:default-outline-level").data())};
        const std::string key = st.family + "\n" + st.name;
        (automatic ? autos_[scope] : named_)[key] = std::move(st);
      } else if (n == "style:default-style" && Attr(s, "style:family") == "paragraph") {
        defaults_ = s;
      } else if (n == "text:list-style") {
        listStyles_[automatic ? scope : 0][s.attribute("style:name").value()] = s;
        if (!automatic) listStyles_[1][s.attribute("style:name").value()] = s;
      } else if (n == "text:outline-style") {
        outline_ = s;
      } else if (n == "style:page-layout") {
        pageLayouts_[s.attribute("style:name").value()] = s;
      }
    }
  };
  read(root.child("office:styles"), false);
  read(root.child("office:automatic-styles"), true);
}

const Style* Converter::Find(std::string_view family, std::string_view name) const {
  const std::string key = std::string(family) + "\n" + std::string(name);
  if (auto it = autos_[scope_].find(key); it != autos_[scope_].end()) return &it->second;
  if (auto it = named_.find(key); it != named_.end()) return &it->second;
  return nullptr;
}

// A named style's Word id (a paragraph style's parent if the name is an automatic style's).
std::string Converter::Mapped(std::string_view family, std::string_view name) const {
  if (name.empty()) return {};
  const std::string key = std::string(family) + "\n" + std::string(name);
  if (named_.count(key)) return StyleId(name);
  if (auto it = autos_[scope_].find(key); it != autos_[scope_].end() && !it->second.parent.empty() &&
                                          named_.count(std::string(family) + "\n" + it->second.parent))
    return StyleId(it->second.parent);
  return {};
}

std::string Converter::Rel(std::string type, std::string target, bool external) {
  OutPart& p = parts_[part_];
  const std::string id = "rId" + std::to_string(p.rels.size() + 1);
  p.rels.emplace_back(id, "<Relationship Id=\"" + id + "\" Type=\"" + std::string(kRel) + type + "\" Target=\"" +
                              Esc(target) + "\"" + (external ? " TargetMode=\"External\"" : "") + "/>");
  return id;
}

// ---------- Lists ----------

int Converter::Num(std::string_view listStyle, bool restart) {
  const std::string key(listStyle);
  auto it = abstractOf_.find(key);
  if (it == abstractOf_.end()) {
    xml_node ls = key == "\noutline" ? outline_ : xml_node();
    if (!ls)
      if (auto f = listStyles_[scope_].find(key); f != listStyles_[scope_].end()) ls = f->second;
    if (!ls) return 0;
    std::string lvls;
    for (int lvl = 0; lvl < 9; ++lvl) {
      xml_node def;
      for (xml_node c : ls.children())
        if (std::atoi(Attr(c, "text:level").data()) == lvl + 1) def = c;
      const bool bullet = std::string_view(def.name()) == "text:list-level-style-bullet";
      std::string fmt = "decimal", text;
      if (bullet) {
        fmt = "bullet";
        text = Attr(def, "text:bullet-char");
        if (text.empty()) text = "\xE2\x80\xA2";
      } else {
        const std::string_view f = def ? Attr(def, "style:num-format") : std::string_view("1");
        fmt = f == "a" ? "lowerLetter" : f == "A" ? "upperLetter" : f == "i" ? "lowerRoman" : f == "I" ? "upperRoman" : f.empty() ? "none" : "decimal";
        const int shown = std::clamp(def ? std::atoi(Attr(def, "text:display-levels").data()) : 1, 1, lvl + 1);
        text = std::string(Attr(def, "style:num-prefix"));
        for (int k = lvl + 1 - shown; k <= lvl; ++k) text += "%" + std::to_string(k + 1) + (k < lvl ? "." : "");
        text += Attr(def, "style:num-suffix");
        if (fmt == "none") text.clear();
      }
      int left = 720 * (lvl + 1), hanging = 360;
      xml_node props = def.child("style:list-level-properties");
      if (xml_node la = props.child("style:list-level-label-alignment")) {
        left = Twips(Attr(la, "fo:margin-left")).value_or(left);
        hanging = -Twips(Attr(la, "fo:text-indent")).value_or(-hanging);
      } else if (props) {
        const int before = Twips(Attr(props, "text:space-before")).value_or(0);
        hanging = Twips(Attr(props, "text:min-label-width")).value_or(hanging);
        left = before + hanging;
      }
      const int start = std::clamp(def ? std::max(1, std::atoi(Attr(def, "text:start-value").data())) : 1, 0, 32767);
      lvls += "<w:lvl w:ilvl=\"" + std::to_string(lvl) + "\"><w:start w:val=\"" + std::to_string(start) +
              "\"/><w:numFmt w:val=\"" + fmt + "\"/><w:lvlText w:val=\"" + Esc(text) +
              "\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"" + std::to_string(std::clamp(left, 0, 31680)) +
              "\" w:hanging=\"" + std::to_string(std::clamp(hanging, 0, 31680)) + "\"/></w:pPr>";
      // The label's font: a bullet in Symbol or Wingdings (Word's own .odt files) needs it.
      RunProps label;
      label.Read(def.child("style:text-properties"), fonts_);
      if (!label.font.empty()) {
        const std::string f = Esc(label.font);
        lvls += "<w:rPr><w:rFonts w:ascii=\"" + f + "\" w:hAnsi=\"" + f + "\" w:cs=\"" + f + "\" w:hint=\"default\"/></w:rPr>";
      }
      lvls += "</w:lvl>";
    }
    it = abstractOf_.emplace(key, static_cast<int>(abstracts_.size())).first;
    xml_node first;
    for (xml_node c : ls.children())
      if (std::atoi(Attr(c, "text:level").data()) == 1) first = c;
    starts_.push_back(std::clamp(first ? std::max(1, std::atoi(Attr(first, "text:start-value").data())) : 1, 0, 32767));
    abstracts_.push_back("<w:abstractNum w:abstractNumId=\"" + std::to_string(abstracts_.size()) +
                         "\"><w:multiLevelType w:val=\"hybridMultilevel\"/>" + lvls + "</w:abstractNum>");
  }
  const auto last = lastNum_.find(key);
  if (!restart && last != lastNum_.end()) return last->second;
  // A list after the first of its style starts again (Word's w:num share their count otherwise).
  nums_.emplace_back(it->second, last != lastNum_.end() ? starts_[static_cast<size_t>(it->second)] : 0);
  return lastNum_[key] = static_cast<int>(nums_.size());
}

std::string Converter::NumPr(std::string_view listStyle, int level, bool restart) {
  const int num = Num(listStyle, restart);
  if (num == 0) return {};
  return "<w:numPr><w:ilvl w:val=\"" + std::to_string(std::clamp(level, 0, 8)) + "\"/><w:numId w:val=\"" +
         std::to_string(num) + "\"/></w:numPr>";
}

std::string Converter::NumberingXml() {
  std::string s = std::string(kDecl) + "<w:numbering " + std::string(kNs) + ">";
  for (const std::string& a : abstracts_) s += a;
  for (size_t i = 0; i < nums_.size(); ++i) {
    s += "<w:num w:numId=\"" + std::to_string(i + 1) + "\"><w:abstractNumId w:val=\"" + std::to_string(nums_[i].first) + "\"/>";
    if (nums_[i].second)
      s += "<w:lvlOverride w:ilvl=\"0\"><w:startOverride w:val=\"" + std::to_string(nums_[i].second) + "\"/></w:lvlOverride>";
    s += "</w:num>";
  }
  return s + "</w:numbering>";
}

// ---------- Blocks ----------

void Converter::Blocks(xml_node parent, std::string& out, int depth) {
  for (xml_node c : parent.children()) {
    const std::string_view n = c.name();
    if (n == "text:p" || n == "text:h") {
      Paragraph(c, out, depth);
    } else if (n == "text:list") {
      List(c, out, depth, std::string(Attr(c, "text:style-name")), 0);
    } else if (n == "table:table") {
      Table(c, out, depth);
    } else if (depth < kMaxDepth &&
               (n == "text:section" || n == "text:index-body" || n == "text:table-of-content" ||
                n == "text:alphabetical-index" || n == "text:illustration-index" || n == "text:table-index" ||
                n == "text:object-index" || n == "text:user-index" || n == "text:bibliography" ||
                n == "text:numbered-paragraph" || n == "text:list-header")) {
      Blocks(c, out, depth + 1);
    }
  }
}

void Converter::List(xml_node list, std::string& out, int depth, std::string listStyle, int level) {
  if (auto s = Attr(list, "text:style-name"); !s.empty()) listStyle = std::string(s);
  bool restart = level == 0 && Attr(list, "text:continue-numbering") != "true" && Attr(list, "text:continue-list").empty();
  for (xml_node item : list.children()) {
    const std::string_view n = item.name();
    if (n != "text:list-item" && n != "text:list-header") continue;
    bool first = true;
    for (xml_node c : item.children()) {
      const std::string_view cn = c.name();
      if (cn == "text:p" || cn == "text:h") {
        // The item's first paragraph has its label; a header's, and the ones after, none.
        Paragraph(c, out, depth, first && n == "text:list-item" ? std::string_view(listStyle) : std::string_view(), restart ? -2 - level : level);
        restart = false;
        first = false;
      } else if (cn == "text:list" && depth < kMaxDepth) {
        List(c, out, depth + 1, listStyle, level + 1);
      } else if (cn == "table:table") {
        Table(c, out, depth);
      }
    }
  }
}

void Converter::Paragraph(xml_node p, std::string& out, int depth, std::string_view listStyle, int level) {
  const bool restart = level <= -2;
  if (restart) level = -2 - level;
  ParaProps pp;
  RunProps mark;
  std::string pStyle;
  const std::string_view styleName = Attr(p, "text:style-name");
  const Style* st = Find("paragraph", styleName);
  if (st && autos_[scope_].count("paragraph\n" + std::string(styleName))) {
    pp.Read(st->node.child("style:paragraph-properties"));
    mark.Read(st->node.child("style:text-properties"), fonts_);
    if (!st->master.empty() && !firstParagraph_ && scope_ == 0) {
      if (&out == body_) SectionBreak(out, st->master, Attr(st->node.child("style:paragraph-properties"), "style:page-number"));
      else pp.pageBreak = true;  // in a table or a note: the page style can't change there
    }
  }
  pStyle = Mapped("paragraph", styleName);
  const bool heading = std::string_view(p.name()) == "text:h";
  const int outline = heading ? std::clamp(std::atoi(Attr(p, "text:outline-level").data()), 0, 9) : 0;
  if (heading && pStyle.empty() && outline > 0) pStyle = "Heading" + std::to_string(outline);
  if (heading && outline > 0) pp.outline = outline - 1;
  std::string numPr;
  if (!listStyle.empty()) numPr = NumPr(listStyle, level, restart);
  else if (heading && outline > 0 && outline_ && level < 0) {
    for (xml_node c : outline_.children())
      if (std::atoi(Attr(c, "text:level").data()) == outline && !Attr(c, "style:num-format").empty())
        numPr = NumPr("\noutline", outline - 1, false);
  }
  if (scope_ == 0) firstParagraph_ = false;

  Inline in;
  in.props = mark;
  std::string runs, after;
  if (!notePrefix_.empty()) {
    runs += notePrefix_;
    notePrefix_.clear();
  }
  Runs(p, in, runs, after, depth);
  const std::string ppr = pp.Xml(pStyle, numPr, mark.Xml());
  out += "<w:p>" + (ppr.empty() ? std::string() : "<w:pPr>" + ppr + "</w:pPr>") + runs + "</w:p>" + after;
}

void Converter::Table(xml_node table, std::string& out, int depth) {
  if (depth >= kMaxDepth) return;
  // The grid: each column's width, repeated as the file says.
  std::vector<int> widths;
  auto columns = [&](auto&& self, xml_node parent) -> void {
    for (xml_node c : parent.children()) {
      const std::string_view n = c.name();
      if (n == "table:table-column") {
        int w = 0;
        if (const Style* s = Find("table-column", Attr(c, "table:style-name")))
          w = Twips(Attr(s->node.child("style:table-column-properties"), "style:column-width")).value_or(0);
        const int repeat = std::clamp(std::atoi(Attr(c, "table:number-columns-repeated").data()), 1, 64);
        for (int k = 0; k < repeat && widths.size() < 64; ++k) widths.push_back(w);
      } else if (n == "table:table-columns" || n == "table:table-header-columns" || n == "table:table-column-group") {
        self(self, c);
      }
    }
  };
  columns(columns, table);
  std::vector<xml_node> rows;
  std::vector<bool> header;
  auto collect = [&](auto&& self, xml_node parent, bool head) -> void {
    for (xml_node c : parent.children()) {
      const std::string_view n = c.name();
      if (n == "table:table-row" && rows.size() < 10000) {
        rows.push_back(c);
        header.push_back(head);
      } else if (n == "table:table-header-rows" || n == "table:table-rows" || n == "table:table-row-group") {
        self(self, c, head || n == "table:table-header-rows");
      }
    }
  };
  collect(collect, table, false);
  size_t cols = widths.size();
  for (xml_node r : rows) {
    size_t n = 0;
    for (xml_node c : r.children())
      if (std::string_view(c.name()) == "table:table-cell" || std::string_view(c.name()) == "table:covered-table-cell")
        n += std::clamp(std::atoi(Attr(c, "table:number-columns-repeated").data()), 1, 64);
    cols = std::max(cols, std::min<size_t>(n, 64));
  }
  if (cols == 0 || rows.empty()) return;
  widths.resize(cols, 0);
  for (int& w : widths)
    if (w <= 0) w = 9000 / static_cast<int>(cols);

  std::string tblPr = "<w:tblW w:w=\"0\" w:type=\"auto\"/>";
  if (const Style* s = Find("table", Attr(table, "table:style-name"))) {
    xml_node tp = s->node.child("style:table-properties");
    // A width relative to the text's: Word fits the columns to it.
    if (auto rel = Attr(tp, "style:rel-width"); !rel.empty() && rel.back() == '%') {
      const int pct = std::clamp(std::atoi(std::string(rel).c_str()), 1, 100);
      tblPr = "<w:tblW w:w=\"" + std::to_string(pct * 50) + "\" w:type=\"pct\"/>";
      const int text = TextWidth();
      long long sum = 0;
      for (int w : widths) sum += w;
      if (&out == body_ && text > 0 && sum > 0)
        for (int& w : widths) w = static_cast<int>(std::max<long long>(1, static_cast<long long>(w) * text * pct / 100 / sum));
    }
    if (auto a = Attr(tp, "table:align"); a == "center" || a == "right")
      tblPr += std::string("<w:jc w:val=\"") + (a == "center" ? "center" : "right") + "\"/>";
    if (auto ind = Twips(Attr(tp, "fo:margin-left")); ind && *ind != 0 && Attr(tp, "table:align") != "center")
      tblPr += "<w:tblInd w:w=\"" + std::to_string(std::clamp(*ind, -31680, 31680)) + "\" w:type=\"dxa\"/>";
  }
  out += "<w:tbl><w:tblPr>" + tblPr + "<w:tblLayout w:type=\"fixed\"/></w:tblPr><w:tblGrid>";
  for (int w : widths) out += "<w:gridCol w:w=\"" + std::to_string(w) + "\"/>";
  out += "</w:tblGrid>";
  std::vector<std::pair<int, int>> below(cols, {0, 1});  // rows still merged down from above, and their span
  for (size_t r = 0; r < rows.size(); ++r) {
    out += "<w:tr>";
    {
      // The row: kept on one page, its height, a header that repeats.
      std::string trPr;
      if (const Style* rs = Find("table-row", Attr(rows[r], "table:style-name"))) {
        xml_node rp = rs->node.child("style:table-row-properties");
        if (Attr(rp, "fo:keep-together") == "always") trPr += "<w:cantSplit/>";
        if (auto h = Twips(Attr(rp, "style:row-height")); h && *h > 0)
          trPr += "<w:trHeight w:val=\"" + std::to_string(std::min(*h, 31680)) + "\" w:hRule=\"exact\"/>";
        else if (auto m = Twips(Attr(rp, "style:min-row-height")); m && *m > 0)
          trPr += "<w:trHeight w:val=\"" + std::to_string(std::min(*m, 31680)) + "\" w:hRule=\"atLeast\"/>";
      }
      if (header[r]) trPr += "<w:tblHeader/>";
      if (!trPr.empty()) out += "<w:trPr>" + trPr + "</w:trPr>";
    }
    size_t col = 0;
    bool any = false;
    for (xml_node c : rows[r].children()) {
      const std::string_view n = c.name();
      const bool covered = n == "table:covered-table-cell";
      if ((n != "table:table-cell" && !covered) || col >= cols) continue;
      const int repeat = std::clamp(std::atoi(Attr(c, "table:number-columns-repeated").data()), 1, 64);
      for (int k = 0; k < repeat && col < cols; ++k) {
        if (covered) {
          // Under a cell that spans rows: a cell that continues it; beside one that spans
          // columns: nothing (its w:gridSpan covers it).
          if (below[col].first > 0) {
            const int span = below[col].second;
            --below[col].first;
            out += "<w:tc><w:tcPr><w:tcW w:w=\"0\" w:type=\"auto\"/>" +
                   (span > 1 ? "<w:gridSpan w:val=\"" + std::to_string(span) + "\"/>" : std::string()) +
                   "<w:vMerge/></w:tcPr><w:p/></w:tc>";
            any = true;
            col += static_cast<size_t>(span);
            k += span - 1;
          } else {
            ++col;
          }
          continue;
        }
        const int span = static_cast<int>(std::clamp<size_t>(std::max(1, std::atoi(Attr(c, "table:number-columns-spanned").data())), 1, cols - col));
        const int down = std::clamp(std::atoi(Attr(c, "table:number-rows-spanned").data()), 1, 10000);
        std::string tcPr = "<w:tcW w:w=\"0\" w:type=\"auto\"/>";
        if (span > 1) tcPr += "<w:gridSpan w:val=\"" + std::to_string(span) + "\"/>";
        if (down > 1) {
          tcPr += "<w:vMerge w:val=\"restart\"/>";
          below[col] = {down - 1, span};
        }
        std::string borders, shade, valign, margins;
        if (const Style* s = Find("table-cell", Attr(c, "table:style-name"))) {
          xml_node cp = s->node.child("style:table-cell-properties");
          const std::string_view all = Attr(cp, "fo:border");
          for (auto [side, attr] : {std::pair{"top", "fo:border-top"}, {"left", "fo:border-left"}, {"bottom", "fo:border-bottom"}, {"right", "fo:border-right"}}) {
            const std::string_view v = Attr(cp, attr);
            borders += Border(side, v.empty() ? all : v);
          }
          if (auto bg = Color(Attr(cp, "fo:background-color")); !bg.empty())
            shade = "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"" + bg + "\"/>";
          // The cell's padding: its margins.
          static const std::pair<const char*, const char*> kPad[] = {
              {"top", "fo:padding-top"}, {"left", "fo:padding-left"}, {"bottom", "fo:padding-bottom"}, {"right", "fo:padding-right"}};
          for (auto [side, attr] : kPad) {
            std::optional<int> v = Twips(Attr(cp, attr));
            if (!v) v = Twips(Attr(cp, "fo:padding"));
            if (v) margins += std::string("<w:") + side + " w:w=\"" + std::to_string(std::clamp(*v, 0, 31680)) + "\" w:type=\"dxa\"/>";
          }
          if (!margins.empty()) margins = "<w:tcMar>" + margins + "</w:tcMar>";
          if (auto va = Attr(cp, "style:vertical-align"); va == "middle" || va == "bottom")
            valign = std::string("<w:vAlign w:val=\"") + (va == "middle" ? "center" : "bottom") + "\"/>";
        }
        if (!borders.empty()) tcPr += "<w:tcBorders>" + borders + "</w:tcBorders>";
        tcPr += shade + margins + valign;
        std::string inner;
        Blocks(c, inner, depth + 1);
        if (inner.size() < 7 || inner.compare(inner.size() - 6, 6, "</w:p>") != 0) inner += "<w:p/>";
        out += "<w:tc><w:tcPr>" + tcPr + "</w:tcPr>" + inner + "</w:tc>";
        any = true;
        col += static_cast<size_t>(span);
      }
    }
    // A row needs a cell; columns the file left out stay empty.
    while (col < cols) {
      out += "<w:tc><w:tcPr><w:tcW w:w=\"0\" w:type=\"auto\"/></w:tcPr><w:p/></w:tc>";
      ++col;
      any = true;
    }
    if (!any) out += "<w:tc><w:p/></w:tc>";
    out += "</w:tr>";
  }
  out += "</w:tbl>";
  // Two tables in a row would become one: a paragraph keeps them apart.
  if (table.next_sibling() && std::string_view(table.next_sibling().name()) == "table:table") out += "<w:p/>";
}

// ---------- Inline ----------

void Converter::Text(std::string_view text, Inline& in, std::string& out) {
  std::string t;
  for (char c : text) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (!in.space) t += ' ';
      in.space = true;
    } else {
      t += c;
      in.space = false;
    }
  }
  if (t.empty()) return;
  const std::string rpr = in.props.Xml(in.rStyle);
  out += "<w:r>" + (rpr.empty() ? std::string() : "<w:rPr>" + rpr + "</w:rPr>") + "<w:t xml:space=\"preserve\">" + Esc(t) +
         "</w:t></w:r>";
}

void Converter::BookmarkTexts(xml_node parent, int depth) {
  constexpr size_t kMost = 4096;  // a bookmark's text that a reference repeats, at most
  auto add = [&](std::string_view t) {
    for (auto& [name, text] : openText_)
      for (char ch : t) {
        if (text.size() >= kMost) break;
        const bool white = ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
        if (white && (text.empty() || text.back() == ' ')) continue;  // ODF's white space collapses
        if (refCollect_ == 0) return;
        --refCollect_;
        text += white ? ' ' : ch;
      }
  };
  for (xml_node c : parent.children()) {
    if (c.type() == pugi::node_pcdata || c.type() == pugi::node_cdata) {
      if (!openText_.empty()) add(c.value());
      continue;
    }
    if (c.type() != pugi::node_element) continue;
    const std::string_view n = c.name();
    const std::string name(Attr(c, "text:name"));
    if (n == "text:bookmark-start") {
      if (!name.empty() && !refText_.count(name) && openText_.size() < 64) openText_.emplace(name, std::string());
    } else if (n == "text:bookmark-end") {
      if (auto it = openText_.find(name); it != openText_.end()) {
        while (!it->second.empty() && it->second.back() == ' ') it->second.pop_back();
        refText_[name] = std::move(it->second);
        openText_.erase(it);
      }
    } else if (n == "text:s" || n == "text:tab") {
      if (!openText_.empty()) add(" ");
    } else if (n == "text:bookmark-ref" && !openText_.empty() &&
               (Attr(c, "text:reference-format") == "text" || Attr(c, "text:reference-format").empty()) &&
               refText_.count(std::string(Attr(c, "text:ref-name")))) {
      add(refText_[std::string(Attr(c, "text:ref-name"))]);  // a reference in the text: what it shows
    } else if (n == "text:note" || n == "office:annotation" || n == "text:tracked-changes" || n.starts_with("text:change")) {
      continue;
    } else if (depth < kMaxDepth) {
      if ((n == "text:p" || n == "text:h") && !openText_.empty()) add(" ");  // paragraphs: a space between
      BookmarkTexts(c, depth + 1);
    }
  }
}

void Converter::Runs(xml_node parent, Inline& in, std::string& out, std::string& after, int depth) {
  auto run = [&](std::string_view inner) {
    const std::string rpr = in.props.Xml(in.rStyle);
    out += "<w:r>" + (rpr.empty() ? std::string() : "<w:rPr>" + rpr + "</w:rPr>") + std::string(inner) + "</w:r>";
  };
  for (xml_node c : parent.children()) {
    if (c.type() == pugi::node_pcdata || c.type() == pugi::node_cdata) {
      Text(c.value(), in, out);
      continue;
    }
    if (c.type() != pugi::node_element) continue;
    const std::string_view n = c.name();
    if (n == "text:s") {
      const int count = std::clamp(std::atoi(Attr(c, "text:c").data()), 1, 1000);
      std::string spaces(static_cast<size_t>(count), ' ');
      const std::string rpr = in.props.Xml(in.rStyle);
      out += "<w:r>" + (rpr.empty() ? std::string() : "<w:rPr>" + rpr + "</w:rPr>") + "<w:t xml:space=\"preserve\">" + spaces + "</w:t></w:r>";
      in.space = false;
    } else if (n == "text:tab") {
      run("<w:tab/>");
      in.space = true;
    } else if (n == "text:line-break") {
      run("<w:br/>");
      in.space = true;
    } else if (n == "text:span" || n == "text:a" || n == "text:ruby-base") {
      if (depth >= kMaxDepth) continue;
      Inline inner = in;
      const std::string_view styleName = Attr(c, "text:style-name");
      if (const Style* s = Find("text", styleName)) {
        if (autos_[scope_].count("text\n" + std::string(styleName))) {
          RunProps p;
          p.Read(s->node.child("style:text-properties"), fonts_);
          inner.props.Over(p);
        }
        if (auto id = Mapped("text", styleName); !id.empty()) inner.rStyle = id;
      }
      if (n == "text:a" && !in.link) {
        inner.link = true;
        std::string_view href = Attr(c, "xlink:href");
        std::string runs;
        Runs(c, inner, runs, after, depth + 1);
        in.space = inner.space;
        if (href.starts_with("#")) {
          std::string anchor(href.substr(1));
          if (size_t bar = anchor.find('|'); bar != std::string::npos) anchor.resize(bar);
          out += "<w:hyperlink w:anchor=\"" + Esc(anchor) + "\" w:history=\"1\">" + runs + "</w:hyperlink>";
        } else if (!href.empty() && href.size() < 2048) {
          // A relationship's target is a URI: what can't be in one is percent-encoded.
          std::string uri;
          for (unsigned char ch : href) {
            if (ch <= 0x20 || ch >= 0x7F || std::string_view("\"<>\\^`{|}").find(static_cast<char>(ch)) != std::string_view::npos) {
              static const char kHex[] = "0123456789ABCDEF";
              uri += '%';
              uri += kHex[ch >> 4];
              uri += kHex[ch & 15];
            } else {
              uri += static_cast<char>(ch);
            }
          }
          out += "<w:hyperlink r:id=\"" + Rel("hyperlink", uri, true) + "\" w:history=\"1\">" + runs + "</w:hyperlink>";
        } else {
          out += runs;
        }
      } else {
        Runs(c, inner, out, after, depth + 1);
        in.space = inner.space;
      }
    } else if (n == "text:bookmark" || n == "text:bookmark-start" || n == "text:bookmark-end") {
      const std::string name(Attr(c, "text:name"));
      if (name.empty()) continue;
      if (n != "text:bookmark-end") {
        if (bookmarks_.count(name)) continue;
        const int id = nextBookmark_++;
        const bool point = n == "text:bookmark";
        bookmarks_[name] = point ? -1 : id;
        if (!point) open_[id] = part_;
        size_t cut = std::min<size_t>(name.size(), 40);  // Word's longest, on a character's start
        while (cut > 0 && cut < name.size() && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) --cut;
        out += "<w:bookmarkStart w:id=\"" + std::to_string(id) + "\" w:name=\"" + Esc(name.substr(0, cut)) + "\"/>";
        if (point) out += "<w:bookmarkEnd w:id=\"" + std::to_string(id) + "\"/>";
      } else if (auto it = bookmarks_.find(name); it != bookmarks_.end() && it->second >= 0) {
        // Its end in the part its start is in (a note's or a header's end elsewhere waits for
        // the end of the part: CloseBookmarks).
        if (auto o = open_.find(it->second); o != open_.end() && o->second == part_) {
          out += "<w:bookmarkEnd w:id=\"" + std::to_string(it->second) + "\"/>";
          open_.erase(o);
          it->second = -1;
        }
      }
    } else if (n == "text:note") {
      Note(c, in, out, depth);
    } else if (n == "draw:frame") {
      Picture(c, out);
      // A text frame's paragraphs follow the paragraph.
      for (xml_node k : c.children("draw:text-box"))
        if (depth < kMaxDepth) Blocks(k, after, depth + 1);
    } else if (n == "draw:a") {
      if (depth < kMaxDepth) Runs(c, in, out, after, depth + 1);  // a linked frame
    } else if (n.starts_with("draw:")) {
      // Shapes: their text, after the paragraph.
      if (depth < kMaxDepth) Blocks(c, after, depth + 1);
    } else if (n == "text:page-number" || n == "text:page-count") {
      std::string result(c.text().get());
      if (result.empty()) result = "1";
      const std::string rpr = in.props.Xml(in.rStyle);
      out += std::string("<w:fldSimple w:instr=\"") + (n == "text:page-number" ? " PAGE " : " NUMPAGES ") + "\"><w:r>" +
             (rpr.empty() ? std::string() : "<w:rPr>" + rpr + "</w:rPr>") + "<w:t>" + Esc(result) + "</w:t></w:r></w:fldSimple>";
      in.space = false;
    } else if (n == "text:bookmark-ref" && (Attr(c, "text:reference-format") == "text" || Attr(c, "text:reference-format").empty()) &&
               refText_.count(std::string(Attr(c, "text:ref-name"))) && !refText_[std::string(Attr(c, "text:ref-name"))].empty() &&
               Attr(c, "text:ref-name").find_first_of(" \"\\") == std::string_view::npos &&
               refText_[std::string(Attr(c, "text:ref-name"))].size() <= refBudget_) {
      // A reference to the bookmark's text shows that text (the file keeps what it showed when saved: Word
      // writes its numbered references so, and shows the text when it reads them back).
      std::string name(Attr(c, "text:ref-name"));
      const std::string& text = refText_[name];
      refBudget_ -= text.size();
      size_t cut = std::min<size_t>(name.size(), 40);  // as the bookmark's name was cut
      while (cut > 0 && cut < name.size() && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) --cut;
      name.resize(cut);
      const std::string rpr = in.props.Xml(in.rStyle);
      out += "<w:fldSimple w:instr=\" REF " + Esc(name) + " \\h \"><w:r>" + (rpr.empty() ? std::string() : "<w:rPr>" + rpr + "</w:rPr>") +
             "<w:t xml:space=\"preserve\">" + Esc(text) + "</w:t></w:r></w:fldSimple>";
      in.space = text.back() == ' ';
    } else if (n == "office:annotation" || n == "office:annotation-end" || n == "text:soft-page-break" ||
               n.starts_with("text:change") || n == "text:tracked-changes" || n == "text:ruby-text" ||
               n == "text:sequence-decls" || n == "text:variable-decls" || n == "text:user-field-decls") {
      continue;
    } else if (depth < kMaxDepth) {
      Runs(c, in, out, after, depth + 1);  // fields and the like: their text
    }
  }
}

void Converter::Note(xml_node note, const Inline& in, std::string& out, int depth) {
  const bool end = Attr(note, "text:note-class") == "endnote";
  xml_node body = note.child("text:note-body");
  if (!body || depth >= kMaxDepth) return;
  const int id = end ? nextEndnote_++ : nextFootnote_++;
  const char* kind = end ? "endnote" : "footnote";
  RunProps raised = in.props;
  raised.vert = "superscript";  // in its place among the run's properties (before w:lang)
  out += "<w:r><w:rPr>" + raised.Xml() + "</w:rPr><w:" + kind + "Reference w:id=\"" + std::to_string(id) + "\"/></w:r>";
  // The note's text, in its own part: its first paragraph starts with the number.
  const size_t was = part_;
  for (size_t i = 0; i < parts_.size(); ++i)
    if (parts_[i].name == (end ? "word/endnotes.xml" : "word/footnotes.xml")) part_ = i;
  if (part_ == was) {
    parts_.push_back({end ? "word/endnotes.xml" : "word/footnotes.xml", {}, {}});
    part_ = parts_.size() - 1;
  }
  notePrefix_ = std::string("<w:r><w:rPr><w:vertAlign w:val=\"superscript\"/></w:rPr><w:") + kind +
                "Ref/></w:r><w:r><w:t xml:space=\"preserve\"> </w:t></w:r>";
  const bool firstWas = firstParagraph_;
  std::string text;
  Blocks(body, text, depth + 1);
  CloseBookmarks(text);
  firstParagraph_ = firstWas;
  if (!notePrefix_.empty()) text = "<w:p>" + notePrefix_ + "</w:p>" + text;  // a note with no paragraph
  notePrefix_.clear();
  (end ? endnotes_ : footnotes_) += std::string("<w:") + kind + " w:id=\"" + std::to_string(id) + "\">" + text + "</w:" + kind + ">";
  part_ = was;
}

void Converter::Picture(xml_node frame, std::string& out) {
  // The first picture in a format Word reads (a frame may hold an SVG and a PNG of it).
  std::string data, ext;
  for (xml_node img : frame.children("draw:image")) {
    std::string href(Attr(img, "xlink:href"));
    if (href.starts_with("./")) href.erase(0, 2);
    const pkg::ZipEntry* e = href.empty() || href.find("://") != std::string::npos ? nullptr : zip_.Find(href);
    if (!e) continue;
    std::vector<uint8_t> bytes;
    if (!zip_.Read(*e, bytes).Ok() || bytes.size() < 8) continue;
    const std::string_view head(reinterpret_cast<const char*>(bytes.data()), std::min<size_t>(bytes.size(), 48));
    if (head.starts_with("\x89PNG")) ext = "png";
    else if (head.starts_with("\xFF\xD8")) ext = "jpeg";
    else if (head.starts_with("GIF8")) ext = "gif";
    else if (head.starts_with("BM")) ext = "bmp";
    else if (head.starts_with("II*") || head.starts_with("MM")) ext = "tiff";
    else if (head.starts_with(std::string_view("\xD7\xCD\xC6\x9A", 4))) ext = "wmf";
    else if (head.size() >= 44 && head.substr(40, 4) == " EMF") ext = "emf";
    else continue;
    auto it = media_.find(href);
    if (it == media_.end()) {
      it = media_.emplace(href, "media/image" + std::to_string(media_.size() + 1) + "." + ext).first;
      parts_.push_back({"word/" + it->second, std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), {}});
      if (std::find(mediaTypes_.begin(), mediaTypes_.end(), ext) == mediaTypes_.end()) mediaTypes_.push_back(ext);
    }
    // Pictures in headers and footers live in those parts' folders too: the target is relative.
    data = it->second;
    break;
  }
  if (data.empty()) return;
  const int64_t cx = std::clamp<int64_t>(Emu(Attr(frame, "svg:width"), 1828800), 12700, 20000000);
  const int64_t cy = std::clamp<int64_t>(Emu(Attr(frame, "svg:height"), 1371600), 12700, 20000000);
  const std::string rid = Rel("image", data);
  const int id = nextDocPr_++;
  std::string descr(frame.child("svg:desc").text().get());
  if (descr.empty()) descr = frame.child("svg:title").text().get();
  const std::string extent = "<wp:extent cx=\"" + std::to_string(cx) + "\" cy=\"" + std::to_string(cy) + "\"/>";
  // A frame anchored to the paragraph, a character or the page floats where its graphic style puts it.
  const std::string_view anchor = Attr(frame, "text:anchor-type");
  const bool floating = anchor == "paragraph" || anchor == "char" || anchor == "page";
  std::string open = "<wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\">" + extent, close = "</wp:inline>";
  if (floating) {
    xml_node gp;
    if (const Style* st = Find("graphic", Attr(frame, "draw:style-name"))) gp = st->node.child("style:graphic-properties");
    auto rel = [&](const char* attr, bool horizontal) -> std::string {
      const std::string_view v = Attr(gp, attr);
      if (v == "page" || anchor == "page") return "page";
      if (v == "page-content" || v == "page-start-margin" || v == "page-end-margin") return "margin";
      if (v == "char" || v == "line" || v == "baseline" || v == "text") return horizontal ? "character" : "line";
      return horizontal ? "column" : "paragraph";
    };
    auto pos = [&](bool horizontal) {
      const std::string_view p = Attr(gp, horizontal ? "style:horizontal-pos" : "style:vertical-pos");
      std::string align;
      if (horizontal) align = p == "left" ? "left" : p == "center" ? "center" : p == "right" ? "right" : p == "inside" ? "inside" : p == "outside" ? "outside" : "";
      else align = p == "top" ? "top" : p == "middle" ? "center" : p == "bottom" ? "bottom" : "";
      const char* tag = horizontal ? "positionH" : "positionV";
      std::string x = std::string("<wp:") + tag + " relativeFrom=\"" + rel(horizontal ? "style:horizontal-rel" : "style:vertical-rel", horizontal) + "\">";
      if (!align.empty()) x += "<wp:align>" + align + "</wp:align>";
      else {
        const std::optional<double> pt = Points(Attr(frame, horizontal ? "svg:x" : "svg:y"));
        x += "<wp:posOffset>" + std::to_string(std::clamp<int64_t>(pt ? std::llround(*pt * 12700) : 0, -20000000, 20000000)) + "</wp:posOffset>";
      }
      return x + "</wp:" + tag + ">";
    };
    const std::string_view wrap = Attr(gp, "style:wrap");
    const bool behind = wrap == "run-through" && Attr(gp, "style:run-through") == "background";
    std::string wrapXml = wrap == "none" ? "<wp:wrapTopAndBottom/>"
                          : wrap == "run-through" ? "<wp:wrapNone/>"
                          : std::string("<wp:wrapSquare wrapText=\"") + (wrap == "left" ? "left" : wrap == "right" ? "right" : wrap == "dynamic" ? "largest" : "bothSides") + "\"/>";
    auto dist = [&](const char* attr) { return std::to_string(std::clamp<int64_t>(Emu(Attr(gp, attr), 0), 0, 2000000)); };
    const int z = std::clamp(std::atoi(Attr(frame, "draw:z-index").data()), 0, 0x7FFFFFFF);
    open = "<wp:anchor distT=\"" + dist("fo:margin-top") + "\" distB=\"" + dist("fo:margin-bottom") + "\" distL=\"" + dist("fo:margin-left") +
           "\" distR=\"" + dist("fo:margin-right") + "\" simplePos=\"0\" relativeHeight=\"" + std::to_string(z) + "\" behindDoc=\"" +
           (behind ? "1" : "0") + "\" locked=\"0\" layoutInCell=\"1\" allowOverlap=\"1\"><wp:simplePos x=\"0\" y=\"0\"/>" + pos(true) + pos(false) +
           extent + wrapXml;
    close = "</wp:anchor>";
  }
  out += "<w:r><w:drawing>" + open + "<wp:docPr id=\"" +
         std::to_string(id) + "\" name=\"Picture " + std::to_string(id) + "\"" +
         (descr.empty() ? std::string() : " descr=\"" + Esc(descr) + "\"") +
         "/><a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"><pic:pic>"
         "<pic:nvPicPr><pic:cNvPr id=\"" + std::to_string(id) + "\" name=\"Picture " + std::to_string(id) +
         "\"/><pic:cNvPicPr/></pic:nvPicPr><pic:blipFill><a:blip r:embed=\"" + rid +
         "\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill><pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"" +
         std::to_string(cx) + "\" cy=\"" + std::to_string(cy) +
         "\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr></pic:pic></a:graphicData>"
         "</a:graphic>" + close + "</w:drawing></w:r>";
}

// ---------- Parts ----------

// The font table: what Word (and layout) substitute a font that isn't installed by, its family and pitch.
std::string Converter::FontTableXml() const {
  std::string s = std::string(kDecl) + "<w:fonts " + std::string(kNs) + ">";
  for (const auto& [name, f] : faces_) {
    s += "<w:font w:name=\"" + Esc(name) + "\">";
    // svg:panose-1 "2 15 5 2 2 2 4 3 2 4" as "020F0502020204030204"; all zeros says nothing.
    std::string panose;
    bool known = false;
    for (std::string_view v = Attr(f, "svg:panose-1"); !v.empty();) {
      const size_t sp = v.find(' ');
      const int n = std::atoi(std::string(v.substr(0, sp)).c_str());
      static const char kHex[] = "0123456789ABCDEF";
      panose += kHex[(n >> 4) & 15];
      panose += kHex[n & 15];
      known = known || n != 0;
      v = sp == std::string_view::npos ? std::string_view() : v.substr(sp + 1);
    }
    if (known && panose.size() == 20) s += "<w:panose1 w:val=\"" + panose + "\"/>";
    if (Attr(f, "style:font-charset") == "x-symbol") s += "<w:charset w:val=\"02\"/>";
    const std::string_view g = Attr(f, "style:font-family-generic");
    if (g == "roman" || g == "swiss" || g == "modern" || g == "script" || g == "decorative") s += "<w:family w:val=\"" + std::string(g) + "\"/>";
    if (const std::string_view p = Attr(f, "style:font-pitch"); p == "fixed" || p == "variable") s += "<w:pitch w:val=\"" + std::string(p) + "\"/>";
    s += "</w:font>";
  }
  return s + "</w:fonts>";
}

std::string Converter::StylesXml() {
  RunProps base;
  ParaProps basePara;
  if (defaults_) {
    base.Read(defaults_.child("style:text-properties"), fonts_);
    basePara.Read(defaults_.child("style:paragraph-properties"));
  }
  if (base.font.empty()) base.font = "Liberation Serif";
  if (!base.size) base.size = 24;
  std::string s = std::string(kDecl) + "<w:styles " + std::string(kNs) + "><w:docDefaults><w:rPrDefault><w:rPr>" +
                  base.Xml() + "</w:rPr></w:rPrDefault><w:pPrDefault><w:pPr>" + basePara.Xml({}, {}, {}) +
                  "</w:pPr></w:pPrDefault></w:docDefaults>";
  bool normal = false;
  std::unordered_map<std::string, bool> ids;
  std::vector<const Style*> order;
  for (const auto& [key, st] : named_) order.push_back(&st);
  std::sort(order.begin(), order.end(), [](const Style* a, const Style* b) { return a->family + a->name < b->family + b->name; });
  for (const Style* st : order) {
    const bool para = st->family == "paragraph";
    if (!para && st->family != "text") continue;
    const std::string id = StyleId(st->name);
    if (ids.count(id)) continue;  // two ODF names that read the same
    ids[id] = true;
    std::string display(Attr(st->node, "style:display-name"));
    if (display.empty()) display = Display(st->name);
    if (id == "Normal") display = "Normal", normal = true;
    if (id.starts_with("Heading") && id.size() == 8) display = "heading " + id.substr(7);
    s += std::string("<w:style w:type=\"") + (para ? "paragraph" : "character") + "\"" +
         (id == "Normal" ? " w:default=\"1\"" : "") + " w:styleId=\"" + Esc(id) + "\"><w:name w:val=\"" + Esc(display) + "\"/>";
    if (!st->parent.empty() && named_.count(st->family + "\n" + st->parent)) s += "<w:basedOn w:val=\"" + Esc(StyleId(st->parent)) + "\"/>";
    else if (para && id != "Normal") s += "<w:basedOn w:val=\"Normal\"/>";
    if (auto next = Attr(st->node, "style:next-style-name"); para && !next.empty() && named_.count("paragraph\n" + std::string(next)))
      s += "<w:next w:val=\"" + Esc(StyleId(next)) + "\"/>";
    s += "<w:qFormat/>";
    ParaProps pp;
    RunProps rp;
    pp.Read(st->node.child("style:paragraph-properties"));
    rp.Read(st->node.child("style:text-properties"), fonts_);
    if (para && st->outline > 0) pp.outline = std::clamp(st->outline, 1, 9) - 1;
    if (para) {
      const std::string ppr = pp.Xml({}, {}, {});
      if (!ppr.empty()) s += "<w:pPr>" + ppr + "</w:pPr>";
    }
    const std::string rpr = rp.Xml();
    if (!rpr.empty()) s += "<w:rPr>" + rpr + "</w:rPr>";
    s += "</w:style>";
  }
  if (!normal) s += "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/><w:qFormat/></w:style>";
  // Headings the text uses without a style of the file's.
  for (int level = 1; level <= 9; ++level) {
    const std::string id = "Heading" + std::to_string(level);
    if (ids.count(id)) continue;
    s += "<w:style w:type=\"paragraph\" w:styleId=\"" + id + "\"><w:name w:val=\"heading " + std::to_string(level) +
         "\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:qFormat/><w:pPr><w:keepNext/><w:spacing w:before=\"240\" "
         "w:after=\"120\"/><w:outlineLvl w:val=\"" + std::to_string(level - 1) + "\"/></w:pPr><w:rPr><w:b/><w:sz w:val=\"" +
         std::to_string(std::max(24, 36 - 4 * (level - 1))) + "\"/></w:rPr></w:style>";
  }
  return s + "</w:styles>";
}

void Converter::SectionBreak(std::string& out, std::string_view master, std::string_view pageNumber) {
  const std::string sect = SectPr();
  const size_t full = out.rfind("<w:p>"), empty = out.rfind("<w:p/>");
  const bool para = out.size() >= 6 && (out.compare(out.size() - 6, 6, "</w:p>") == 0 || out.compare(out.size() - 6, 6, "<w:p/>") == 0);
  if (!para) {
    out += "<w:p><w:pPr>" + sect + "</w:pPr></w:p>";  // after a table, a paragraph to hold it
  } else if (empty != std::string::npos && (full == std::string::npos || empty > full)) {
    out.replace(empty, 6, "<w:p><w:pPr>" + sect + "</w:pPr></w:p>");
  } else if (out.compare(full + 5, 7, "<w:pPr>") == 0) {
    out.insert(out.find("</w:pPr>", full), sect);  // after its w:rPr, as the schema orders them
  } else {
    out.insert(full + 5, "<w:pPr>" + sect + "</w:pPr>");
  }
  for (xml_node m : styles_.Root().child("office:master-styles").children("style:master-page"))
    if (Attr(m, "style:name") == master) master_ = m;
  pageStart_ = std::clamp(std::atoi(std::string(pageNumber).c_str()), 0, 32767);
}

int Converter::TextWidth() const {
  xml_node layout;
  if (master_)
    if (auto it = pageLayouts_.find(Attr(master_, "style:page-layout-name").data()); it != pageLayouts_.end()) layout = it->second;
  xml_node pl = layout.child("style:page-layout-properties");
  const int width = Twips(Attr(pl, "fo:page-width")).value_or(11906);
  return width - Twips(Attr(pl, "fo:margin-left")).value_or(1134) - Twips(Attr(pl, "fo:margin-right")).value_or(1134);
}

std::string Converter::SectPr() {
  std::string s = "<w:sectPr>";
  xml_node layout;
  if (master_)
    if (auto it = pageLayouts_.find(Attr(master_, "style:page-layout-name").data()); it != pageLayouts_.end()) layout = it->second;
  xml_node pl = layout.child("style:page-layout-properties");
  int width = Twips(Attr(pl, "fo:page-width")).value_or(11906), height = Twips(Attr(pl, "fo:page-height")).value_or(16838);
  width = std::clamp(width, 1440, 31680);
  height = std::clamp(height, 1440, 31680);
  auto margin = [&](const char* side, int fallback) { return std::clamp(Twips(Attr(pl, side)).value_or(fallback), 0, 15840); };
  int top = margin("fo:margin-top", 1134), bottom = margin("fo:margin-bottom", 1134);
  const int left = margin("fo:margin-left", 1134), right = margin("fo:margin-right", 1134);
  int header = top, footer = bottom;
  // ODF's margins reach the header and footer; Word's the text below and above them.
  auto extra = [&](const char* name) {
    xml_node hf = layout.child(name).child("style:header-footer-properties");
    if (!hf) return 0;
    return std::clamp(Twips(Attr(hf, "fo:min-height")).value_or(0) +
                          Twips(Attr(hf, name == std::string_view("style:header-style") ? "fo:margin-bottom" : "fo:margin-top")).value_or(0),
                      0, 7200);
  };
  bool first = false;
  for (const char* kind : {"header", "footer"}) {
    for (const char* variant : {"", "-first"}) {
      xml_node content = master_.child((std::string("style:") + kind + variant).c_str());
      if (!content || Attr(content, "style:display") == "false") continue;
      if (variant[0]) first = true;
      const bool head = kind == std::string_view("header");
      const std::string name = std::string("word/") + kind + std::to_string(parts_.size()) + ".xml";
      parts_.push_back({name, {}, {}});
      const size_t was = part_;
      part_ = parts_.size() - 1;
      scope_ = 1;
      std::string text;
      Blocks(content, text, 0);
      CloseBookmarks(text);
      if (text.empty() || text.compare(text.size() - 6, 6, "</w:p>") != 0) text += "<w:p/>";
      scope_ = 0;
      parts_[part_].xml = std::string(kDecl) + (head ? "<w:hdr " : "<w:ftr ") + std::string(kNs) + ">" + text + (head ? "</w:hdr>" : "</w:ftr>");
      part_ = was;
      const std::string rid = Rel(kind, name.substr(5));
      s += std::string("<w:") + kind + "Reference w:type=\"" + (variant[0] ? "first" : "default") + "\" r:id=\"" + rid + "\"/>";
    }
  }
  // Word reserves the header's and footer's height whether the page style has them or not (a page with only
  // a first-page footer, or none): it writes its header distance so.
  top += extra("style:header-style");
  bottom += extra("style:footer-style");
  s += "<w:pgSz w:w=\"" + std::to_string(width) + "\" w:h=\"" + std::to_string(height) + "\"" +
       (Attr(pl, "style:print-orientation") == "landscape" ? " w:orient=\"landscape\"" : "") + "/>";
  s += "<w:pgMar w:top=\"" + std::to_string(top) + "\" w:right=\"" + std::to_string(right) + "\" w:bottom=\"" +
       std::to_string(bottom) + "\" w:left=\"" + std::to_string(left) + "\" w:header=\"" + std::to_string(header) +
       "\" w:footer=\"" + std::to_string(footer) + "\" w:gutter=\"0\"/>";
  // The page numbers: their format (the page layout's) and where they start again.
  const std::string_view nf = Attr(pl, "style:num-format");
  const char* fmt = nf == "i" ? "lowerRoman" : nf == "I" ? "upperRoman" : nf == "a" ? "lowerLetter" : nf == "A" ? "upperLetter" : nullptr;
  if (fmt || pageStart_ > 0) {
    s += "<w:pgNumType";
    if (fmt) s += std::string(" w:fmt=\"") + fmt + "\"";
    if (pageStart_ > 0) s += " w:start=\"" + std::to_string(pageStart_) + "\"";
    s += "/>";
  }
  if (xml_node cols = pl.child("style:columns"); cols) {
    const int n = std::clamp(std::atoi(Attr(cols, "fo:column-count").data()), 1, 45);
    if (n > 1) s += "<w:cols w:num=\"" + std::to_string(n) + "\" w:space=\"" + std::to_string(Twips(Attr(cols, "fo:column-gap")).value_or(709)) + "\"/>";
  }
  if (first) s += "<w:titlePg/>";
  return s + "</w:sectPr>";
}

pkg::Status Converter::Run(std::vector<uint8_t>& docx) {
  if (const pkg::ZipEntry* m = zip_.Find("mimetype")) {
    std::vector<uint8_t> type;
    if (zip_.Read(*m, type).Ok()) {
      const std::string_view t(reinterpret_cast<const char*>(type.data()), type.size());
      if (!t.starts_with("application/vnd.oasis.opendocument.text"))
        return pkg::Status::Fail(pkg::Status::Code::Unsupported, "This OpenDocument file isn't a text document.");
    }
  }
  if (pkg::Status st = Load("content.xml", content_, true); !st.Ok()) return st;
  if (pkg::Status st = Load("styles.xml", styles_, false); !st.Ok()) return st;
  ReadStyles(styles_.Root(), 1);
  ReadStyles(content_.Root(), 0);
  xml_node text = content_.Root().child("office:body").child("office:text");
  if (!text) return pkg::Status::Fail(pkg::Status::Code::Unsupported, "This OpenDocument file isn't a text document.");

  // The page style: the one the first paragraph starts, or "Standard".
  xml_node masters = styles_.Root().child("office:master-styles");
  std::string masterName = "Standard";
  for (xml_node p : text.children()) {
    const std::string_view n = p.name();
    if (n != "text:p" && n != "text:h" && n != "table:table" && n != "text:list") continue;
    const char* attr = n == "table:table" ? "table:style-name" : "text:style-name";
    if (const Style* st = Find(n == "table:table" ? "table" : "paragraph", Attr(p, attr)); st && !st->master.empty())
      masterName = st->master;
    break;
  }
  for (xml_node m : masters.children("style:master-page"))
    if (!master_ || Attr(m, "style:name") == masterName) master_ = m;

  parts_.push_back({"word/document.xml", {}, {}});
  part_ = 0;
  BookmarkTexts(text, 0);
  openText_.clear();
  std::string body;
  body_ = &body;
  Blocks(text, body, 0);
  CloseBookmarks(body);
  const std::string sect = SectPr();
  parts_[0].xml = std::string(kDecl) + "<w:document " + std::string(kNs) + "><w:body>" + body + sect + "</w:body></w:document>";

  // The other parts.
  std::string overrides = "<Override PartName=\"/word/document.xml\" ContentType=\"" + std::string(kWml) + "document.main+xml\"/>";
  auto add = [&](std::string name, std::string type, std::string relType, std::string xml) {
    parts_[0].rels.emplace_back("", "");
    const std::string id = "rId" + std::to_string(parts_[0].rels.size());
    parts_[0].rels.back() = {id, "<Relationship Id=\"" + id + "\" Type=\"" + std::string(kRel) + relType + "\" Target=\"" + name.substr(5) + "\"/>"};
    overrides += "<Override PartName=\"/" + name + "\" ContentType=\"" + std::string(kWml) + type + "\"/>";
    parts_.push_back({std::move(name), std::move(xml), {}});
  };
  add("word/styles.xml", "styles+xml", "styles", StylesXml());
  if (!faces_.empty()) add("word/fontTable.xml", "fontTable+xml", "fontTable", FontTableXml());
  std::string settings = std::string(kDecl) + "<w:settings " + std::string(kNs) + "><w:defaultTabStop w:val=\"709\"/>";
  if (nextFootnote_ > 1) settings += "<w:footnotePr><w:footnote w:id=\"-1\"/><w:footnote w:id=\"0\"/></w:footnotePr>";
  if (nextEndnote_ > 1) settings += "<w:endnotePr><w:endnote w:id=\"-1\"/><w:endnote w:id=\"0\"/></w:endnotePr>";
  settings += "<w:compat><w:compatSetting w:name=\"compatibilityMode\" w:uri=\"http://schemas.microsoft.com/office/word\" "
              "w:val=\"15\"/></w:compat></w:settings>";
  add("word/settings.xml", "settings+xml", "settings", settings);
  if (!nums_.empty()) add("word/numbering.xml", "numbering+xml", "numbering", NumberingXml());
  for (size_t i = 1; i < parts_.size(); ++i) {
    OutPart& p = parts_[i];
    const bool foot = p.name == "word/footnotes.xml", endn = p.name == "word/endnotes.xml";
    if (!foot && !endn) {
      if (p.name.starts_with("word/header")) overrides += "<Override PartName=\"/" + p.name + "\" ContentType=\"" + std::string(kWml) + "header+xml\"/>";
      if (p.name.starts_with("word/footer")) overrides += "<Override PartName=\"/" + p.name + "\" ContentType=\"" + std::string(kWml) + "footer+xml\"/>";
      continue;
    }
    const char* kind = foot ? "footnote" : "endnote";
    const std::string sep = std::string("<w:") + kind + " w:type=\"separator\" w:id=\"-1\"><w:p><w:pPr><w:spacing w:after=\"0\" w:line=\"240\" "
                            "w:lineRule=\"auto\"/></w:pPr><w:r><w:separator/></w:r></w:p></w:" + kind + "><w:" + kind +
                            " w:type=\"continuationSeparator\" w:id=\"0\"><w:p><w:pPr><w:spacing w:after=\"0\" w:line=\"240\" "
                            "w:lineRule=\"auto\"/></w:pPr><w:r><w:continuationSeparator/></w:r></w:p></w:" + kind + ">";
    p.xml = std::string(kDecl) + "<w:" + kind + "s " + std::string(kNs) + ">" + sep + (foot ? footnotes_ : endnotes_) + "</w:" + kind + "s>";
    const std::string id = "rId" + std::to_string(parts_[0].rels.size() + 1);
    parts_[0].rels.emplace_back(id, "<Relationship Id=\"" + id + "\" Type=\"" + std::string(kRel) + kind + "s\" Target=\"" + p.name.substr(5) + "\"/>");
    overrides += "<Override PartName=\"/" + p.name + "\" ContentType=\"" + std::string(kWml) + kind + "s+xml\"/>";
  }
  std::string types = std::string(kDecl) +
                      "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                      "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                      "<Default Extension=\"xml\" ContentType=\"application/xml\"/>";
  for (const std::string& ext : mediaTypes_)
    types += "<Default Extension=\"" + ext + "\" ContentType=\"" +
             (ext == "wmf" ? "image/x-wmf" : ext == "emf" ? "image/x-emf" : "image/" + ext) + "\"/>";
  types += overrides + "</Types>";

  pkg::MemorySink sink;
  pkg::ZipWriter zip(sink);
  auto put = [&](std::string_view name, std::string_view text) {
    return zip.Add(name, std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
  };
  put("[Content_Types].xml", types);
  put("_rels/.rels", std::string(kDecl) +
                         "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                         "<Relationship Id=\"rId1\" Type=\"" + std::string(kRel) + "officeDocument\" Target=\"word/document.xml\"/>"
                         "</Relationships>");
  for (const OutPart& p : parts_) {
    if (pkg::Status st = put(p.name, p.xml); !st.Ok()) return st;
    if (p.rels.empty()) continue;
    std::string rels = std::string(kDecl) + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
    for (const auto& [id, rel] : p.rels) rels += rel;
    const size_t slash = p.name.rfind('/');
    put(p.name.substr(0, slash) + "/_rels/" + p.name.substr(slash + 1) + ".rels", rels + "</Relationships>");
  }
  if (pkg::Status st = zip.Finish(); !st.Ok()) return st;
  docx = std::move(sink.bytes);
  return {};
}

}  // namespace

pkg::Status OdtToDocx(std::span<const uint8_t> odt, std::vector<uint8_t>& docx, const pkg::Limits& limits) {
  auto snapshot = pkg::Snapshot::FromBytes(std::vector<uint8_t>(odt.begin(), odt.end()));
  pkg::ZipReader zip;
  if (pkg::Status st = zip.Open(*snapshot, limits); !st.Ok()) return st;
  Converter c(zip, limits);
  return c.Run(docx);
}

}  // namespace fw::doc::odf
