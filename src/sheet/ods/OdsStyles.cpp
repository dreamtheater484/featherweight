#include "sheet/ods/OdsStyles.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "pkg/Odf.h"

namespace fw::sheet::ods::detail {
namespace {

using pugi::xml_node;
using pkg::odf::Escape;

std::string_view Attr(xml_node n, const char* name) { return n.attribute(name).value(); }

int IntAttr(xml_node n, const char* name, int def, int lo, int hi) {
  const char* v = n.attribute(name).value();
  if (!*v) return def;
  char* end = nullptr;
  const long x = std::strtol(v, &end, 10);
  return static_cast<int>(std::clamp<long>(x, lo, hi));
}

std::string Unquote(std::string_view s) {
  if (s.size() >= 2 && (s.front() == '\'' || s.front() == '"') && s.back() == s.front()) s = s.substr(1, s.size() - 2);
  return std::string(s);
}

// A border's width and line as Excel's style: Excel writes "thin", "2pt" (medium) and "thick".
Line BorderLine(std::string_view v) {
  Line line;
  const auto b = pkg::odf::ParseBorder(v);
  if (!b || b->none) return line;
  double w = b->width;
  if (v.find("thin") != std::string_view::npos) w = 0.75;
  else if (v.find("medium") != std::string_view::npos) w = 2;
  else if (v.find("thick") != std::string_view::npos) w = 3;
  const std::string& s = b->style;
  if (s == "double" || s == "double-thin") line.style = "double";
  else if (s == "dotted") line.style = w < 0.3 ? "hair" : "dotted";
  else if (s == "dashed" || s == "fine-dashed") line.style = w > 1.25 ? "mediumDashed" : "dashed";
  else if (s == "dash-dot" || s == "dot-dash") line.style = w > 1.25 ? "mediumDashDot" : "dashDot";
  else if (s == "dash-dot-dot" || s == "dot-dot-dash") line.style = w > 1.25 ? "mediumDashDotDot" : "dashDotDot";
  else line.style = w > 2.25 ? "thick" : w > 1.25 ? "medium" : "thin";
  line.color = b->color;
  return line;
}

// Excel's eight named colors for a number format's [Color], the nearest one.
std::string ColorName(std::string_view rgb) {
  static const std::pair<const char*, int> kNames[] = {{"Black", 0x000000}, {"White", 0xFFFFFF}, {"Red", 0xFF0000},
                                                       {"Green", 0x00FF00}, {"Blue", 0x0000FF}, {"Yellow", 0xFFFF00},
                                                       {"Magenta", 0xFF00FF}, {"Cyan", 0x00FFFF}};
  const int v = static_cast<int>(std::strtol(std::string(rgb).c_str(), nullptr, 16));
  const char* best = "Black";
  long bestD = -1;
  for (const auto& [name, c] : kNames) {
    const long dr = ((v >> 16) & 255) - ((c >> 16) & 255), dg = ((v >> 8) & 255) - ((c >> 8) & 255), db = (v & 255) - (c & 255);
    const long d = dr * dr + dg * dg + db * db;
    if (bestD < 0 || d < bestD) {
      bestD = d;
      best = name;
    }
  }
  return std::string("[") + best + "]";
}

enum class Kind { Number, Percentage, Currency, Date, Time, Boolean, Text };

Kind KindOf(xml_node style) {
  const std::string_view n = style.name();
  if (n == "number:percentage-style") return Kind::Percentage;
  if (n == "number:currency-style") return Kind::Currency;
  if (n == "number:date-style") return Kind::Date;
  if (n == "number:time-style") return Kind::Time;
  if (n == "number:boolean-style") return Kind::Boolean;
  if (n == "number:text-style") return Kind::Text;
  return Kind::Number;
}

// Literal text in a format code: bare where Excel shows the character as it is, otherwise
// escaped or quoted.
void Literal(std::string_view text, Kind kind, std::string& out) {
  const bool dates = kind == Kind::Date || kind == Kind::Time;
  std::string quoted;
  auto flush = [&] {
    if (quoted.empty()) return;
    if (quoted.size() == 1) out += "\\" + quoted;
    else out += "\"" + quoted + "\"";
    quoted.clear();
  };
  for (char c : text) {
    const bool bare = c == ' ' || c == '-' || c == '(' || c == ')' || c == '+' || c == '$' || c == '!' || c == '^' ||
                      c == '&' || c == '~' || c == '{' || c == '}' || c == '<' || c == '>' || c == '=' || c == '\'' ||
                      (dates && (c == '/' || c == ':')) || (c == '%' && kind == Kind::Percentage) ||
                      (c == ':' && kind != Kind::Text);
    if (bare) {
      flush();
      out += c;
    } else if (c == '"') {
      flush();
      out += "\\\"";
    } else {
      quoted += c;
    }
  }
  flush();
}

struct Condition {
  std::string op;
  double value = 0;
  bool ok = false;
};

Condition ParseCondition(std::string_view c) {
  Condition r;
  if (!c.starts_with("value()")) return r;
  c.remove_prefix(7);
  size_t i = 0;
  while (i < c.size() && (c[i] == '<' || c[i] == '>' || c[i] == '=' || c[i] == '!')) ++i;
  r.op = std::string(c.substr(0, i));
  if (r.op == "!=") r.op = "<>";
  try {
    r.value = std::stod(std::string(c.substr(i)));
  } catch (...) {
    return r;
  }
  r.ok = !r.op.empty();
  return r;
}

std::string Number(double v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.15g", v);
  return buf;
}

std::string Key(const Xf& x) {
  std::string k;
  auto add = [&](const std::string& s) { k += s; k += '\x1f'; };
  add(x.font); add(Number(x.size)); add(std::to_string(x.bold * 1 + x.italic * 2 + x.strike * 4 + x.wrap * 8 + x.shrink * 16 +
                                                      x.locked * 32 + x.hidden * 64 + x.diagUp * 128 + x.diagDown * 256));
  add(std::to_string(x.underline)); add(std::to_string(x.position)); add(x.color); add(x.fill);
  for (const Line* l : {&x.left, &x.right, &x.top, &x.bottom, &x.diagonal}) { add(l->style); add(l->color); }
  add(x.horizontal); add(x.vertical); add(std::to_string(x.indent)); add(std::to_string(x.rotation)); add(x.numFmt);
  return k;
}

}  // namespace

void Styles::Read(xml_node stylesRoot, xml_node contentRoot) {
  for (xml_node root : {stylesRoot, contentRoot}) {
    for (xml_node f : root.child("office:font-face-decls").children("style:font-face")) {
      std::string family = Unquote(Attr(f, "svg:font-family"));
      if (family.empty()) family = Attr(f, "style:name");
      faces_[std::string(Attr(f, "style:name"))] = family;
    }
    for (const char* group : {"office:styles", "office:automatic-styles"}) {
      for (xml_node s : root.child(group).children()) {
        const std::string_view n = s.name();
        const std::string name(Attr(s, "style:name"));
        if (n == "style:default-style" && Attr(s, "style:family") == "table-cell") defaultCell_ = s;
        if (name.empty()) continue;
        if (n == "style:style") {
          if (Attr(s, "style:family") == "table-cell") cell_[name] = s;
          else other_[std::string(Attr(s, "style:family")) + "\x1f" + name] = s;
        } else if (n.starts_with("number:") && n.ends_with("-style")) {
          data_[name] = s;
        }
      }
    }
  }
}

pugi::xml_node Styles::Find(std::string_view family, std::string_view name) const {
  auto it = other_.find(std::string(family) + "\x1f" + std::string(name));
  return it == other_.end() ? xml_node() : it->second;
}

void Styles::Apply(xml_node style, Xf& x) {
  if (xml_node tp = style.child("style:text-properties")) {
    if (auto v = Attr(tp, "style:font-name"); !v.empty()) {
      auto it = faces_.find(std::string(v));
      x.font = it != faces_.end() ? it->second : std::string(v);
    } else if (auto f = Attr(tp, "fo:font-family"); !f.empty()) {
      x.font = Unquote(f);
    }
    if (auto p = pkg::odf::Points(Attr(tp, "fo:font-size")); p && *p > 0 && *p <= 409) x.size = std::round(*p * 2) / 2;
    if (auto w = Attr(tp, "fo:font-weight"); !w.empty()) x.bold = w == "bold" || std::atoi(std::string(w).c_str()) >= 600;
    if (auto i = Attr(tp, "fo:font-style"); !i.empty()) x.italic = i == "italic" || i == "oblique";
    if (auto u = Attr(tp, "style:text-underline-style"); !u.empty())
      x.underline = u == "none" ? 0 : Attr(tp, "style:text-underline-type") == "double" ? 2 : 1;
    if (auto s = Attr(tp, "style:text-line-through-style"); !s.empty()) x.strike = s != "none";
    if (auto c = pkg::odf::Color(Attr(tp, "fo:color")); !c.empty()) x.color = c;
    if (Attr(tp, "style:use-window-font-color") == "true") x.color.clear();
    if (auto p = Attr(tp, "style:text-position"); !p.empty()) {
      x.position = p.starts_with("super") ? 1 : p.starts_with("sub") ? 2 : 0;
      if (!x.position && !p.starts_with("0")) x.position = p.starts_with("-") ? 2 : 1;
    }
  }
  if (xml_node cp = style.child("style:table-cell-properties")) {
    if (auto bg = Attr(cp, "fo:background-color"); !bg.empty()) x.fill = pkg::odf::Color(bg);
    if (auto b = Attr(cp, "fo:border"); !b.empty()) x.left = x.right = x.top = x.bottom = BorderLine(b);
    if (auto b = Attr(cp, "fo:border-left"); !b.empty()) x.left = BorderLine(b);
    if (auto b = Attr(cp, "fo:border-right"); !b.empty()) x.right = BorderLine(b);
    if (auto b = Attr(cp, "fo:border-top"); !b.empty()) x.top = BorderLine(b);
    if (auto b = Attr(cp, "fo:border-bottom"); !b.empty()) x.bottom = BorderLine(b);
    if (auto b = Attr(cp, "style:diagonal-tl-br"); !b.empty()) {
      Line l = BorderLine(b);
      x.diagDown = !l.style.empty();
      if (x.diagDown) x.diagonal = l;
    }
    if (auto b = Attr(cp, "style:diagonal-bl-tr"); !b.empty()) {
      Line l = BorderLine(b);
      x.diagUp = !l.style.empty();
      if (x.diagUp) x.diagonal = l;
    }
    if (auto v = Attr(cp, "style:vertical-align"); !v.empty())
      x.vertical = v == "top" ? "top" : v == "middle" ? "center" : v == "bottom" ? "bottom" : "";
    if (auto w = Attr(cp, "fo:wrap-option"); !w.empty()) x.wrap = w == "wrap";
    if (auto s = Attr(cp, "style:shrink-to-fit"); !s.empty()) x.shrink = s == "true";
    if (auto r = Attr(cp, "style:rotation-angle"); !r.empty()) {
      double a = std::fmod(std::atof(std::string(r).c_str()), 360.0);
      if (a < 0) a += 360;
      const int deg = static_cast<int>(std::lround(a));
      x.rotation = deg == 0 || deg == 360 ? 0 : deg <= 90 ? deg : deg >= 270 ? 90 + (360 - deg) : 0;
    }
    if (Attr(cp, "style:direction") == "ttb") x.rotation = 255;
    if (auto p = Attr(cp, "style:cell-protect"); !p.empty()) {
      x.locked = p.find("protected") != std::string_view::npos && p.find("none") == std::string_view::npos;
      x.hidden = p.find("hidden") != std::string_view::npos;
    }
    if (Attr(cp, "style:text-align-source") == "value-type") x.horizontal.clear();
  }
  if (xml_node pp = style.child("style:paragraph-properties")) {
    const bool fromValue = Attr(style.child("style:table-cell-properties"), "style:text-align-source") == "value-type";
    if (auto a = Attr(pp, "fo:text-align"); !a.empty() && !fromValue)
      x.horizontal = a == "start" || a == "left" ? "left" : a == "center" ? "center" : a == "end" || a == "right" ? "right"
                     : a == "justify" ? "justify" : "";
    if (auto m = pkg::odf::Points(Attr(pp, "fo:margin-left")); m && *m > 0)
      x.indent = std::clamp(static_cast<int>(std::lround(*m / 10)), 0, 250);
  }
  if (auto d = Attr(style, "style:data-style-name"); !d.empty()) {
    auto it = data_.find(std::string(d));
    x.numFmt = it == data_.end() ? std::string() : NumberCode(it->second);
  }
}

int Styles::Resolve(std::string_view name) {
  if (auto it = resolved_.find(std::string(name)); it != resolved_.end()) return it->second;
  // The chain: the default style, the parents (outermost first), the style.
  std::vector<xml_node> chain;
  std::string at(name.empty() ? "Default" : name);
  for (int depth = 0; depth < 16 && !at.empty(); ++depth) {
    auto it = cell_.find(at);
    if (it == cell_.end()) break;
    if (std::find(chain.begin(), chain.end(), it->second) != chain.end()) break;
    chain.push_back(it->second);
    at = Attr(it->second, "style:parent-style-name");
  }
  if (name.empty() || chain.empty() || Attr(chain.back(), "style:parent-style-name").empty()) {
    // A style without parent builds on "Default" (LibreOffice and Excel name it so), and that on
    // the default style.
    if (auto it = cell_.find("Default"); it != cell_.end() && (chain.empty() || chain.back() != it->second))
      chain.push_back(it->second);
  }
  Xf x;
  if (defaultCell_) Apply(defaultCell_, x);
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) Apply(*it, x);
  const std::string key = Key(x);
  int index;
  if (auto it = xfIndex_.find(key); it != xfIndex_.end()) {
    index = it->second;
  } else {
    if (xfs_.empty() && !name.empty()) Resolve("");  // the default style is cell format 0
    index = static_cast<int>(xfs_.size());
    if (auto again = xfIndex_.find(key); again != xfIndex_.end()) index = again->second;
    else {
      xfs_.push_back(x);
      xfIndex_[key] = index;
    }
  }
  resolved_[std::string(name)] = index;
  return index;
}

double Styles::ColumnChars(double points) {
  const double size = Default().size;
  const int digit = std::max(5, static_cast<int>(std::lround(size * 96 / 72 * 0.5)));
  const double px = std::round(points * 96 / 72);
  return std::floor(px / digit * 256) / 256;
}

// ---------- Number formats ----------

std::string Styles::Section(xml_node style, bool& text) {
  const Kind kind = KindOf(style);
  std::string out, color;
  const bool elapsed = Attr(style, "number:truncate-on-overflow") == "false";
  bool firstUnit = true;
  for (xml_node c : style.children()) {
    const std::string_view n = c.name();
    if (n == "style:text-properties") {
      if (auto rgb = pkg::odf::Color(Attr(c, "fo:color")); !rgb.empty()) color = ColorName(rgb);
    } else if (n == "number:number") {
      if (c.attribute("number:decimal-places").empty() && !c.child("number:embedded-text") && Attr(c, "number:grouping") != "true") {
        out += "General";
        continue;
      }
      const int dec = IntAttr(c, "number:decimal-places", 0, 0, 30);
      int minDec = IntAttr(c, "number:min-decimal-places", IntAttr(c, "loext:min-decimal-places", dec, 0, 30), 0, 30);
      minDec = std::min(minDec, dec);
      const int minInt = IntAttr(c, "number:min-integer-digits", 1, 0, 30);
      const bool grouping = Attr(c, "number:grouping") == "true";
      std::multimap<int, std::string> embedded;
      int maxPos = -1;
      for (xml_node e : c.children("number:embedded-text")) {
        const int pos = IntAttr(e, "number:position", 0, 0, 30);
        embedded.emplace(pos, e.text().get());
        maxPos = std::max(maxPos, pos);
      }
      int len = std::max({minInt, grouping ? 4 : 1, maxPos + 1});
      for (int k = len - 1; k >= 0; --k) {
        out += k < minInt ? '0' : '#';
        if (k == 3 && grouping) out += ',';
        auto [a, b] = embedded.equal_range(k);
        for (auto it = a; it != b; ++it) Literal(it->second, kind, out);
      }
      if (dec > 0) out += "." + std::string(minDec, '0') + std::string(dec - minDec, '#');
      const double factor = std::atof(std::string(Attr(c, "number:display-factor")).c_str());
      if (factor >= 999) out += factor >= 999999 ? ",," : ",";
    } else if (n == "number:scientific-number") {
      const int dec = IntAttr(c, "number:decimal-places", 2, 0, 30);
      const int minInt = IntAttr(c, "number:min-integer-digits", 1, 1, 30);
      out += std::string(minInt, '0');
      if (dec > 0) out += "." + std::string(dec, '0');
      out += "E+" + std::string(IntAttr(c, "number:min-exponent-digits", 2, 1, 5), '0');
    } else if (n == "number:fraction") {
      if (!c.attribute("number:min-integer-digits").empty()) {
        const int minInt = IntAttr(c, "number:min-integer-digits", 0, 0, 30);
        out += (minInt ? std::string(minInt, '0') : std::string("#")) + " ";
      }
      out += std::string(IntAttr(c, "number:min-numerator-digits", 1, 1, 5), '?') + "/";
      const int den = IntAttr(c, "number:denominator-value", 0, 0, 99999);
      out += den > 0 ? std::to_string(den) : std::string(IntAttr(c, "number:min-denominator-digits", 1, 1, 5), '?');
    } else if (n == "number:text") {
      Literal(c.text().get(), kind, out);
    } else if (n == "number:text-content") {
      out += '@';
      text = true;
    } else if (n == "number:currency-symbol") {
      const std::string sym = c.text().get();
      if (sym == "$") out += '$';
      else if (!sym.empty()) out += "\"" + sym + "\"";
    } else if (n == "number:fill-character") {
      std::string f = c.text().get();
      out += "*" + (f.empty() ? std::string(" ") : f.substr(0, f.size() > 1 && (f[0] & 0x80) ? f.size() : 1));
    } else if (n == "number:day") {
      out += Attr(c, "number:style") == "long" ? "dd" : "d";
    } else if (n == "number:month") {
      const bool lng = Attr(c, "number:style") == "long";
      out += Attr(c, "number:textual") == "true" ? (lng ? "mmmm" : "mmm") : (lng ? "mm" : "m");
    } else if (n == "number:year") {
      out += Attr(c, "number:style") == "long" ? "yyyy" : "yy";
    } else if (n == "number:day-of-week") {
      out += Attr(c, "number:style") == "long" ? "dddd" : "ddd";
    } else if (n == "number:hours" || n == "number:minutes" || n == "number:seconds") {
      const bool lng = Attr(c, "number:style") == "long";
      std::string unit = n == "number:hours" ? (lng ? "hh" : "h") : n == "number:minutes" ? (lng ? "mm" : "m") : (lng ? "ss" : "s");
      if (elapsed && firstUnit) unit = "[" + unit + "]";
      firstUnit = false;
      out += unit;
      if (n == "number:seconds")
        if (const int dec = IntAttr(c, "number:decimal-places", 0, 0, 3); dec > 0) out += "." + std::string(dec, '0');
    } else if (n == "number:am-pm") {
      out += "AM/PM";
    }
  }
  return color + out;  // empty: the section shows nothing
}

std::string Styles::NumberCode(xml_node style) {
  const std::string name(Attr(style, "style:name"));
  if (auto it = codes_.find(name); it != codes_.end()) return it->second;
  codes_[name] = std::string();  // a style mapping to itself
  std::string code;
  if (KindOf(style) == Kind::Boolean) {
    code = "\"TRUE\";\"TRUE\";\"FALSE\"";
  } else {
    bool text = false;
    const std::string main = Section(style, text);
    struct Map {
      Condition cond;
      std::string code;
    };
    std::vector<Map> maps;
    for (xml_node m : style.children("style:map")) {
      auto it = data_.find(std::string(Attr(m, "style:apply-style-name")));
      if (it == data_.end() || maps.size() == 3) continue;
      bool t = false;
      maps.push_back({ParseCondition(Attr(m, "style:condition")), Section(it->second, t)});
    }
    auto is = [&](size_t i, const char* op) { return i < maps.size() && maps[i].cond.ok && maps[i].cond.value == 0 && maps[i].cond.op == op; };
    if (maps.empty()) {
      code = main == "General" ? std::string() : main;
    } else if (!text && maps.size() == 1 && is(0, ">=")) {
      code = maps[0].code + ";" + main;
    } else if (!text && maps.size() == 2 && is(0, ">") && is(1, "<")) {
      code = maps[0].code + ";" + maps[1].code + ";" + main;
    } else if (maps.size() == 3 && is(0, ">") && is(1, "<") && is(2, "=")) {
      code = maps[0].code + ";" + maps[1].code + ";" + maps[2].code + ";" + main;
    } else if (text) {
      std::vector<std::string> s;
      for (const Map& m : maps) s.push_back(m.code);
      while (s.size() < 3) s.push_back(s.front());
      code = s[0] + ";" + s[1] + ";" + s[2] + ";" + main;
    } else {
      for (const Map& m : maps)
        code += (m.cond.ok ? "[" + m.cond.op + Number(m.cond.value) + "]" : std::string()) + m.code + ";";
      code += main;
    }
  }
  if (code == "General") code.clear();
  codes_[name] = code;
  return code;
}

// ---------- styles.xml ----------

std::string Styles::Xml() const {
  std::vector<std::string> fonts, fills = {"<fill><patternFill patternType=\"none\"/></fill>",
                                           "<fill><patternFill patternType=\"gray125\"/></fill>"},
                           borders = {"<border><left/><right/><top/><bottom/><diagonal/></border>"};
  std::map<std::string, int> fontIds, fillIds, borderIds, numFmtIds;
  static const std::pair<const char*, int> kBuiltin[] = {{"0", 1}, {"0.00", 2}, {"#,##0", 3}, {"#,##0.00", 4},
                                                         {"0%", 9}, {"0.00%", 10}, {"0.00E+00", 11}, {"@", 49}};
  for (const auto& [code, id] : kBuiltin) numFmtIds[code] = id;
  std::string numFmts;
  int nextFmt = 164, customFmts = 0;
  auto id = [](std::map<std::string, int>& ids, std::vector<std::string>& list, const std::string& xml) {
    auto [it, added] = ids.emplace(xml, static_cast<int>(list.size()));
    if (added) list.push_back(xml);
    return it->second;
  };
  auto line = [](const char* side, const Line& l) {
    if (l.style.empty()) return std::string("<") + side + "/>";
    std::string s = std::string("<") + side + " style=\"" + l.style + "\">";
    s += l.color.empty() ? "<color auto=\"1\"/>" : "<color rgb=\"FF" + l.color + "\"/>";
    return s + "</" + side + ">";
  };
  std::string xfs;
  for (const Xf& x : xfs_) {
    std::string font = "<font>";
    if (x.bold) font += "<b/>";
    if (x.italic) font += "<i/>";
    if (x.strike) font += "<strike/>";
    if (x.underline) font += x.underline == 2 ? "<u val=\"double\"/>" : "<u/>";
    if (x.position) font += x.position == 1 ? "<vertAlign val=\"superscript\"/>" : "<vertAlign val=\"subscript\"/>";
    font += "<sz val=\"" + Number(x.size) + "\"/>";
    if (!x.color.empty()) font += "<color rgb=\"FF" + x.color + "\"/>";
    font += "<name val=\"" + Escape(x.font) + "\"/><family val=\"2\"/></font>";
    const int fontId = id(fontIds, fonts, font);
    const int fillId = x.fill.empty() ? 0
                                      : id(fillIds, fills, "<fill><patternFill patternType=\"solid\"><fgColor rgb=\"FF" + x.fill +
                                                               "\"/><bgColor indexed=\"64\"/></patternFill></fill>");
    std::string border = "<border";
    if (x.diagUp) border += " diagonalUp=\"1\"";
    if (x.diagDown) border += " diagonalDown=\"1\"";
    border += ">" + line("left", x.left) + line("right", x.right) + line("top", x.top) + line("bottom", x.bottom) +
              line("diagonal", x.diagonal) + "</border>";
    const int borderId = border == borders[0] ? 0 : id(borderIds, borders, border);
    int fmtId = 0;
    if (!x.numFmt.empty()) {
      auto [it, added] = numFmtIds.emplace(x.numFmt, nextFmt);
      if (added) {
        numFmts += "<numFmt numFmtId=\"" + std::to_string(nextFmt) + "\" formatCode=\"" + Escape(x.numFmt) + "\"/>";
        ++nextFmt;
        ++customFmts;
      }
      fmtId = it->second;
    }
    std::string align;
    if (!x.horizontal.empty()) align += " horizontal=\"" + x.horizontal + "\"";
    if (!x.vertical.empty() && x.vertical != "bottom") align += " vertical=\"" + x.vertical + "\"";
    if (x.rotation) align += " textRotation=\"" + std::to_string(x.rotation) + "\"";
    if (x.wrap) align += " wrapText=\"1\"";
    if (x.indent) align += " indent=\"" + std::to_string(x.indent) + "\"";
    if (x.shrink) align += " shrinkToFit=\"1\"";
    xfs += "<xf numFmtId=\"" + std::to_string(fmtId) + "\" fontId=\"" + std::to_string(fontId) + "\" fillId=\"" +
           std::to_string(fillId) + "\" borderId=\"" + std::to_string(borderId) + "\" xfId=\"0\"";
    if (fmtId) xfs += " applyNumberFormat=\"1\"";
    if (fontId) xfs += " applyFont=\"1\"";
    if (fillId) xfs += " applyFill=\"1\"";
    if (borderId) xfs += " applyBorder=\"1\"";
    if (!align.empty()) xfs += " applyAlignment=\"1\"";
    const bool protection = !x.locked || x.hidden;
    if (protection) xfs += " applyProtection=\"1\"";
    if (align.empty() && !protection) {
      xfs += "/>";
      continue;
    }
    xfs += ">";
    if (!align.empty()) xfs += "<alignment" + align + "/>";
    if (protection) xfs += std::string("<protection") + (x.locked ? "" : " locked=\"0\"") + (x.hidden ? " hidden=\"1\"" : "") + "/>";
    xfs += "</xf>";
  }
  std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
                    "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">";
  if (customFmts) out += "<numFmts count=\"" + std::to_string(customFmts) + "\">" + numFmts + "</numFmts>";
  auto list = [&](const char* tag, const std::vector<std::string>& items) {
    out += std::string("<") + tag + " count=\"" + std::to_string(items.size()) + "\">";
    for (const std::string& s : items) out += s;
    out += std::string("</") + tag + ">";
  };
  list("fonts", fonts);
  list("fills", fills);
  list("borders", borders);
  out += "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>";
  out += "<cellXfs count=\"" + std::to_string(xfs_.size()) + "\">" + xfs + "</cellXfs>";
  out += "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>"
         "<dxfs count=\"0\"/><tableStyles count=\"0\" defaultTableStyle=\"TableStyleMedium2\" "
         "defaultPivotStyle=\"PivotStyleLight16\"/></styleSheet>";
  return out;
}

}  // namespace fw::sheet::ods::detail
