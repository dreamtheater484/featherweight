#include "sheet/view/ClipHtml.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>

#include "core/Strings.h"

namespace fw::sheet::view {

namespace {

char Low(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = Low(c);
  return out;
}

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f'; }

std::string_view Trim(std::string_view s) {
  while (!s.empty() && IsSpace(s.front())) s.remove_prefix(1);
  while (!s.empty() && IsSpace(s.back())) s.remove_suffix(1);
  return s;
}

bool StartsNoCase(std::string_view s, size_t at, std::string_view with) {
  if (at + with.size() > s.size()) return false;
  for (size_t i = 0; i < with.size(); ++i)
    if (Low(s[at + i]) != with[i]) return false;
  return true;
}

size_t FindNoCase(std::string_view s, std::string_view what, size_t from) {
  for (size_t i = from; i + what.size() <= s.size(); ++i)
    if (StartsNoCase(s, i, what)) return i;
  return std::string_view::npos;
}

int Hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  c = Low(c);
  return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

void AppendUtf8(std::string& out, uint32_t cp) {
  if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// HTML's character references: the common names and every number.
std::string Decode(std::string_view s) {
  static const std::pair<const char*, uint32_t> kNames[] = {
      {"amp", '&'},      {"lt", '<'},       {"gt", '>'},        {"quot", '"'},     {"apos", '\''},
      {"nbsp", 0xA0},    {"copy", 0xA9},    {"reg", 0xAE},      {"euro", 0x20AC},  {"mdash", 0x2014},
      {"ndash", 0x2013}, {"hellip", 0x2026}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C},
      {"rdquo", 0x201D}, {"bull", 0x2022},  {"middot", 0xB7},   {"times", 0xD7},   {"divide", 0xF7},
      {"deg", 0xB0},     {"plusmn", 0xB1},  {"pound", 0xA3},    {"yen", 0xA5},     {"cent", 0xA2},
      {"sect", 0xA7},    {"para", 0xB6},    {"laquo", 0xAB},    {"raquo", 0xBB},   {"trade", 0x2122}};
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '&') {
      out += s[i];
      continue;
    }
    const size_t semi = s.find(';', i + 1);
    if (semi == std::string_view::npos || semi - i > 10) {
      out += '&';
      continue;
    }
    const std::string_view name = s.substr(i + 1, semi - i - 1);
    uint32_t cp = 0;
    bool known = false;
    if (!name.empty() && name[0] == '#') {
      const bool hex = name.size() > 1 && Low(name[1]) == 'x';
      for (size_t j = hex ? 2 : 1; j < name.size(); ++j) {
        const int d = hex ? Hex(name[j]) : (name[j] >= '0' && name[j] <= '9' ? name[j] - '0' : -1);
        if (d < 0 || cp > 0x10FFFF) {
          cp = 0;
          break;
        }
        cp = cp * (hex ? 16 : 10) + static_cast<uint32_t>(d);
        known = true;
      }
    } else {
      const std::string low = Lower(name);
      for (const auto& [n, c] : kNames)
        if (low == n) {
          cp = c;
          known = true;
        }
    }
    if (!known) {
      out += '&';
      continue;
    }
    AppendUtf8(out, cp);
    i = semi;
  }
  return out;
}

// CSS: a value's quotes and escapes ("0\.00" is 0.00; \0022 is a quote, as Excel writes them).
std::string CssValue(std::string_view v) {
  v = Trim(v);
  if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i] != '\\' || i + 1 >= v.size()) {
      out += v[i];
      continue;
    }
    // Excel's hex escapes have four digits (\0022); one other character is itself.
    if (i + 4 < v.size() && Hex(v[i + 1]) >= 0 && Hex(v[i + 2]) >= 0 && Hex(v[i + 3]) >= 0 && Hex(v[i + 4]) >= 0) {
      uint32_t cp = 0;
      for (size_t j = 1; j <= 4; ++j) cp = cp * 16 + static_cast<uint32_t>(Hex(v[i + j]));
      AppendUtf8(out, cp);
      i += 4;
      if (i + 1 < v.size() && v[i + 1] == ' ') ++i;
    } else {
      out += v[++i];
    }
  }
  return out;
}

using Decls = std::vector<std::pair<std::string, std::string>>;

Decls ParseDecls(std::string_view s) {
  Decls out;
  size_t i = 0;
  while (i < s.size()) {
    size_t end = i;
    char quote = 0;
    for (; end < s.size(); ++end) {
      const char c = s[end];
      if (quote) {
        if (c == '\\') ++end;
        else if (c == quote) quote = 0;
      } else if (c == '"' || c == '\'') {
        quote = c;
      } else if (c == ';') {
        break;
      }
    }
    const std::string_view decl = s.substr(i, std::min(end, s.size()) - i);
    const size_t colon = decl.find(':');
    if (colon != std::string_view::npos) {
      const std::string prop = Lower(Trim(decl.substr(0, colon)));
      if (!prop.empty()) out.emplace_back(prop, std::string(decl.substr(colon + 1)));
    }
    i = end + 1;
  }
  return out;
}

uint32_t ParseColor(std::string_view raw) {
  const std::string v = Lower(Trim(raw));
  auto rgb = [](uint32_t r, uint32_t g, uint32_t b) { return 0xFF000000u | (r << 16) | (g << 8) | b; };
  if (!v.empty() && v[0] == '#') {
    uint32_t x = 0;
    size_t n = 0;
    for (size_t i = 1; i < v.size() && Hex(v[i]) >= 0; ++i, ++n) x = x * 16 + static_cast<uint32_t>(Hex(v[i]));
    if (n == 6) return 0xFF000000u | x;
    if (n == 3) return rgb(((x >> 8) & 15) * 17, ((x >> 4) & 15) * 17, (x & 15) * 17);
    return 0;
  }
  if (v.rfind("rgb", 0) == 0) {
    const size_t open = v.find('(');
    if (open == std::string::npos) return 0;
    uint32_t c[3] = {};
    const char* p = v.c_str() + open + 1;
    for (int k = 0; k < 3; ++k) {
      char* end = nullptr;
      const long n = std::strtol(p, &end, 10);
      if (end == p) return 0;
      c[k] = static_cast<uint32_t>(std::clamp(n, 0L, 255L));
      p = end;
      while (*p == ' ' || *p == ',') ++p;
    }
    return rgb(c[0], c[1], c[2]);
  }
  static const std::pair<const char*, uint32_t> kNamed[] = {
      {"black", 0x000000},  {"white", 0xFFFFFF}, {"red", 0xFF0000},    {"green", 0x008000},  {"blue", 0x0000FF},
      {"yellow", 0xFFFF00}, {"gray", 0x808080},  {"grey", 0x808080},   {"silver", 0xC0C0C0}, {"maroon", 0x800000},
      {"navy", 0x000080},   {"purple", 0x800080}, {"teal", 0x008080},  {"olive", 0x808000},  {"lime", 0x00FF00},
      {"aqua", 0x00FFFF},   {"fuchsia", 0xFF00FF}, {"orange", 0xFFA500}};
  const std::string word = v.substr(0, v.find(' '));
  for (const auto& [name, c] : kNamed)
    if (word == name) return 0xFF000000u | c;
  return 0;  // windowtext, transparent, none, auto, and the rest
}

// A look as CSS and tags give it; -1 for not given.
struct Look {
  std::string numFmt;
  int bold = -1, italic = -1, underline = -1, strike = -1, wrap = -1;
  uint32_t color = 0, fill = 0;
  double size = 0;
  std::string font;
  int align = -1;  // HAlign
};

void Apply(const Decls& decls, Look& l) {
  for (const auto& [prop, raw] : decls) {
    const std::string v = Lower(Trim(raw));
    if (prop == "mso-number-format") {
      l.numFmt = CssValue(raw);
    } else if (prop == "font-weight") {
      l.bold = v == "bold" || v == "bolder" || std::atoi(v.c_str()) >= 600;
    } else if (prop == "font-style") {
      l.italic = v == "italic" || v == "oblique";
    } else if (prop == "text-decoration" || prop == "text-decoration-line") {
      if (v.find("underline") != std::string::npos) l.underline = 1;
      if (v.find("line-through") != std::string::npos) l.strike = 1;
      if (v == "none") l.underline = l.strike = 0;
    } else if (prop == "text-underline-style") {
      l.underline = v != "none";
    } else if (prop == "text-line-through") {
      l.strike = v != "none";
    } else if (prop == "color") {
      l.color = ParseColor(v);
    } else if (prop == "background" || prop == "background-color") {
      l.fill = ParseColor(v);
    } else if (prop == "font-size") {
      const double n = std::atof(v.c_str());
      if (n > 0) l.size = v.find("px") != std::string::npos ? n * 0.75 : v.find("pt") != std::string::npos ? n : 0;
    } else if (prop == "font-family") {
      std::string f = CssValue(raw.substr(0, raw.find(',')));
      l.font = std::string(Trim(f));
    } else if (prop == "text-align") {
      l.align = v == "left" ? static_cast<int>(HAlign::Left)
                : v == "center" ? static_cast<int>(HAlign::Center)
                : v == "right" ? static_cast<int>(HAlign::Right)
                : v == "justify" ? static_cast<int>(HAlign::Justify)
                                 : static_cast<int>(HAlign::General);
    } else if (prop == "white-space") {
      l.wrap = v == "normal" || v == "pre-wrap";
    }
  }
}

void Merge(const Look& over, Look& l) {
  if (!over.numFmt.empty()) l.numFmt = over.numFmt;
  if (over.bold >= 0) l.bold = over.bold;
  if (over.italic >= 0) l.italic = over.italic;
  if (over.underline >= 0) l.underline = over.underline;
  if (over.strike >= 0) l.strike = over.strike;
  if (over.wrap >= 0) l.wrap = over.wrap;
  if (over.color) l.color = over.color;
  if (over.fill) l.fill = over.fill;
  if (over.size > 0) l.size = over.size;
  if (!over.font.empty()) l.font = over.font;
  if (over.align >= 0) l.align = over.align;
}

// Excel's names for its built-in formats in mso-number-format.
std::string FormatCode(const std::string& f) {
  const std::string low = Lower(f);
  if (low == "general") return {};
  if (low == "percent") return "0.00%";
  if (low == "fixed") return "0.00";
  if (low == "standard") return "#,##0.00";
  if (low == "scientific") return "0.00E+00";
  if (low == "short date") return "m/d/yyyy";
  if (low == "medium date") return "d-mmm-yy";
  if (low == "long date") return "dddd, mmmm d, yyyy";
  if (low == "short time") return "h:mm";
  if (low == "medium time") return "h:mm AM/PM";
  if (low == "long time") return "h:mm:ss";
  return f;
}

struct Tag {
  std::string name;  // lower case; empty for text
  bool closing = false, selfClosing = false;
  std::vector<std::pair<std::string, std::string>> attrs;  // lower-case names, decoded values
  bool has(std::string_view n) const {
    for (const auto& a : attrs)
      if (a.first == n) return true;
    return false;
  }
  std::string get(std::string_view n) const {
    for (const auto& a : attrs)
      if (a.first == n) return a.second;
    return {};
  }
};

// One tag from `<` at `i`; `i` ends after `>`.
Tag ReadTag(std::string_view s, size_t& i) {
  Tag t;
  size_t j = i + 1;
  if (j < s.size() && s[j] == '/') {
    t.closing = true;
    ++j;
  }
  const size_t n0 = j;
  while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == ':' || s[j] == '-')) ++j;
  t.name = Lower(s.substr(n0, j - n0));
  while (j < s.size() && s[j] != '>') {
    while (j < s.size() && (IsSpace(s[j]) || s[j] == '/')) {
      if (s[j] == '/') t.selfClosing = true;
      ++j;
    }
    if (j >= s.size() || s[j] == '>') break;
    t.selfClosing = false;
    const size_t a0 = j;
    while (j < s.size() && !IsSpace(s[j]) && s[j] != '=' && s[j] != '>' && s[j] != '/') ++j;
    std::string name = Lower(s.substr(a0, j - a0));
    while (j < s.size() && IsSpace(s[j])) ++j;
    std::string value;
    if (j < s.size() && s[j] == '=') {
      ++j;
      while (j < s.size() && IsSpace(s[j])) ++j;
      if (j < s.size() && (s[j] == '"' || s[j] == '\'')) {
        const char q = s[j++];
        const size_t v0 = j;
        while (j < s.size() && s[j] != q) ++j;
        value = Decode(s.substr(v0, j - v0));
        if (j < s.size()) ++j;
      } else {
        const size_t v0 = j;
        while (j < s.size() && !IsSpace(s[j]) && s[j] != '>') ++j;
        value = Decode(s.substr(v0, j - v0));
      }
    }
    if (!name.empty()) t.attrs.emplace_back(std::move(name), std::move(value));
    if (j == a0) ++j;  // never stuck
  }
  i = j < s.size() ? j + 1 : s.size();
  return t;
}

bool IsBlock(const std::string& n) {
  static const char* kBlocks[] = {"p",  "div", "h1", "h2", "h3", "h4", "h5", "h6", "li", "ul",
                                  "ol", "pre", "blockquote", "dt", "dd", "section", "article", "header", "footer"};
  for (const char* b : kBlocks)
    if (n == b) return true;
  return false;
}

}  // namespace

bool ReadClipHtml(std::string_view cf, HtmlGrid& out, size_t maxCells) {
  out = HtmlGrid{};
  // CF_HTML: the HTML starts at StartHTML (or at its first tag).
  std::string_view html = cf;
  if (const size_t at = cf.find("StartHTML:"); at != std::string_view::npos && at < 200) {
    const long start = std::strtol(std::string(cf.substr(at + 10, 12)).c_str(), nullptr, 10);
    if (start > 0 && static_cast<size_t>(start) < cf.size()) html = cf.substr(static_cast<size_t>(start));
    else if (const size_t lt = cf.find('<'); lt != std::string_view::npos) html = cf.substr(lt);
  }
  out.excel = html.find("urn:schemas-microsoft-com:office:excel") != std::string_view::npos ||
              FindNoCase(html, "content=excel.sheet", 0) != std::string_view::npos;

  std::map<std::string, Look> classes;
  Look tdLook;
  // The cells, with their text in UTF-8 while it is read.
  struct Open {
    HtmlCell cell;
    std::string text;
    Look look;
    bool textValue = false;  // x:str without a value: the shown text is the string
  };
  std::vector<Open> cells;
  std::string para;  // text outside tables, up to the next block
  int tableDepth = 0;
  bool inCell = false, skipHead = false, pre = false;
  uint32_t row = 0;           // the next row to fill
  uint32_t tableRow = 0;      // the row the open table row fills
  bool rowOpen = false;
  uint32_t col = 0;
  std::map<uint32_t, uint32_t> spanned;  // column -> last row a rowspan above covers
  std::vector<std::pair<std::string, Look>> inline_;  // open inline tags and what they set

  auto effective = [&](const Look& base) {
    Look l = base;
    for (const auto& [name, look] : inline_) Merge(look, l);
    return l;
  };
  auto flushPara = [&]() {
    std::string_view t = Trim(para);
    if (!t.empty()) {
      Open o;
      o.cell.row = row++;
      o.cell.col = 0;
      o.text = std::string(t);
      cells.push_back(std::move(o));
    }
    para.clear();
  };
  auto closeCell = [&]() { inCell = false; };
  auto appendText = [&](std::string_view t) {
    if (inCell && !cells.empty()) {
      Open& o = cells.back();
      o.text += t;
      if (!Trim(t).empty()) {
        // Formatting inside the cell (<b>, a styled <span>) is the cell's.
        const Look l = effective(Look{});
        if (l.bold > 0) o.look.bold = 1;
        if (l.italic > 0) o.look.italic = 1;
        if (l.underline > 0) o.look.underline = 1;
        if (l.strike > 0) o.look.strike = 1;
        if (l.color) o.look.color = l.color;
      }
    } else if (tableDepth == 0) {
      para += t;
    }
  };

  size_t i = 0;
  while (i < html.size()) {
    if (cells.size() > maxCells) return false;
    if (html[i] != '<') {
      size_t end = html.find('<', i);
      if (end == std::string_view::npos) end = html.size();
      if (!skipHead) {
        std::string t = Decode(html.substr(i, end - i));
        if (!pre) {
          // Runs of white space are one space.
          std::string c;
          bool space = false;
          for (char ch : t) {
            if (IsSpace(ch)) {
              space = true;
              continue;
            }
            if (space) c += ' ';
            space = false;
            c += ch;
          }
          if (space) c += ' ';
          t = std::move(c);
        }
        appendText(t);
      }
      i = end;
      continue;
    }
    if (html.compare(i, 4, "<!--") == 0) {
      const size_t end = html.find("-->", i + 4);
      i = end == std::string_view::npos ? html.size() : end + 3;
      continue;
    }
    if (i + 1 < html.size() && (html[i + 1] == '!' || html[i + 1] == '?')) {
      const size_t end = html.find('>', i);
      i = end == std::string_view::npos ? html.size() : end + 1;
      continue;
    }
    const Tag t = ReadTag(html, i);
    const std::string& n = t.name;
    if (n.empty()) continue;
    if (!t.closing && (n == "style" || n == "script" || n == "title")) {
      const size_t end = FindNoCase(html, "</" + n, i);
      const std::string_view body = html.substr(i, (end == std::string_view::npos ? html.size() : end) - i);
      i = end == std::string_view::npos ? html.size() : html.find('>', end) + 1;
      if (i == 0) i = html.size();
      if (n != "style") continue;
      // Rules: Excel's classes (.xl65) and its default for every cell (td).
      size_t k = 0;
      while (k < body.size()) {
        const size_t open = body.find('{', k);
        if (open == std::string_view::npos) break;
        const size_t close = body.find('}', open);
        if (close == std::string_view::npos) break;
        std::string sel = std::string(body.substr(k, open - k));
        if (const size_t c = sel.rfind("-->"); c != std::string::npos) sel = sel.substr(c + 3);
        if (const size_t c = sel.rfind("<!--"); c != std::string::npos) sel = sel.substr(c + 4);
        const Decls decls = ParseDecls(body.substr(open + 1, close - open - 1));
        size_t s0 = 0;
        while (s0 <= sel.size()) {
          size_t s1 = sel.find(',', s0);
          if (s1 == std::string::npos) s1 = sel.size();
          const std::string one = Lower(Trim(std::string_view(sel).substr(s0, s1 - s0)));
          if (one == "td") Apply(decls, tdLook);
          else if (!one.empty() && one[0] == '.' && one.find_first_of(" >:") == std::string::npos) Apply(decls, classes[one.substr(1)]);
          s0 = s1 + 1;
        }
        k = close + 1;
      }
      continue;
    }
    if (n == "head") {
      skipHead = !t.closing;
      continue;
    }
    if (n == "body") {
      skipHead = false;
      continue;
    }
    if (n == "pre") pre = !t.closing;
    if (n == "table") {
      if (!t.closing) {
        if (tableDepth == 0) {
          flushPara();
          spanned.clear();
          // Featherweight's own copy says where it came from (its formulas are written for there).
          if (const std::string from = t.get("data-fw-from"); !from.empty() && !out.hasFrom && cells.empty()) {
            const size_t comma = from.find(',');
            if (comma != std::string::npos) {
              out.hasFrom = true;
              out.from = CellAddr{static_cast<uint32_t>(std::strtoul(from.c_str(), nullptr, 10)),
                                  static_cast<uint32_t>(std::strtoul(from.c_str() + comma + 1, nullptr, 10))};
            }
          }
        } else {
          appendText(" ");
        }
        ++tableDepth;
      } else if (tableDepth > 0) {
        --tableDepth;
        if (tableDepth == 0) {
          closeCell();
          if (rowOpen) row = std::max(row, tableRow + 1);
          rowOpen = false;
          // Rows that rowspans reach below the last one.
          for (const auto& [c, last] : spanned) row = std::max(row, last + 1);
        }
      }
      continue;
    }
    if (tableDepth > 1 && (n == "tr" || n == "td" || n == "th")) {
      if (!t.closing) appendText(" ");  // an inner table's cells: its text in the outer cell
      continue;
    }
    if (tableDepth == 1 && n == "tr") {
      closeCell();
      if (!t.closing) {
        if (rowOpen) row = std::max(row, tableRow + 1);
        tableRow = row;
        rowOpen = true;
        col = 0;
      } else if (rowOpen) {
        row = std::max(row, tableRow + 1);
        rowOpen = false;
      }
      continue;
    }
    if (tableDepth == 1 && (n == "td" || n == "th")) {
      closeCell();
      if (t.closing) continue;
      if (!rowOpen) {  // a cell outside any <tr>
        tableRow = row;
        rowOpen = true;
        col = 0;
      }
      for (auto it = spanned.find(col); it != spanned.end() && it->second >= tableRow; it = spanned.find(col)) ++col;
      const uint32_t cs = static_cast<uint32_t>(std::clamp(std::atoi(t.get("colspan").c_str()), 1, 16384));
      const uint32_t rs = static_cast<uint32_t>(std::clamp(std::atoi(t.get("rowspan").c_str()), 1, 65536));
      Open o;
      o.cell.row = tableRow;
      o.cell.col = col;
      o.look = tdLook;
      if (n == "th") o.look.bold = 1;  // as browsers show a header cell
      // Its class, then its own style and attributes.
      std::string cls = Lower(t.get("class"));
      for (size_t s0 = 0; s0 < cls.size();) {
        size_t s1 = cls.find(' ', s0);
        if (s1 == std::string::npos) s1 = cls.size();
        if (auto it = classes.find(cls.substr(s0, s1 - s0)); it != classes.end()) Merge(it->second, o.look);
        s0 = s1 + 1;
      }
      if (t.has("align")) {
        Look a;
        Apply(ParseDecls("text-align:" + t.get("align")), a);
        Merge(a, o.look);
      }
      if (t.has("bgcolor")) o.look.fill = ParseColor(t.get("bgcolor"));
      if (t.has("style")) {
        Look s;
        Apply(ParseDecls(t.get("style")), s);
        Merge(s, o.look);
      }
      // Excel's value under the shown text.
      if (t.has("x:num") && !t.get("x:num").empty()) {
        char* end = nullptr;
        const std::string v = t.get("x:num");
        const double d = std::strtod(v.c_str(), &end);
        if (end && *end == 0) {
          o.cell.type = ValueType::Number;
          o.cell.number = d;
        }
      } else if (t.has("x:str")) {
        // Text, even where it reads as a number; its value, or else the text shown.
        o.textValue = t.get("x:str").empty();
        o.cell.type = ValueType::String;
        o.text = t.get("x:str");
        if (!o.textValue) o.cell.text = Utf8ToWide(o.text), o.text.clear();
      } else if (t.has("x:bool")) {
        o.cell.type = ValueType::Bool;
        o.cell.boolean = Lower(t.get("x:bool")) == "true";
      } else if (t.has("x:err")) {
        o.cell.type = ValueType::Error;
        o.cell.error = t.get("x:err");
      }
      if (t.has("data-fw-f")) o.cell.formula = t.get("data-fw-f");
      if (cs > 1 || rs > 1)
        out.merges.push_back(Range{tableRow, col, tableRow + rs - 1, col + cs - 1});
      if (rs > 1)
        for (uint32_t c = col; c < col + cs; ++c) spanned[c] = std::max(spanned[c], tableRow + rs - 1);
      col += cs;
      cells.push_back(std::move(o));
      inCell = true;
      continue;
    }
    if (n == "br") {
      if (inCell) cells.back().text += '\n';
      else if (tableDepth == 0) flushPara();
      continue;
    }
    if (IsBlock(n)) {
      if (inCell) {
        if (t.closing || (!cells.back().text.empty() && cells.back().text.back() != '\n'))
          if (!Trim(cells.back().text).empty()) cells.back().text += '\n';
      } else if (tableDepth == 0) {
        flushPara();
      }
      continue;
    }
    // Inline formatting: <b>, <i>, <u>, <s>, <font color>, <span style>.
    if (n == "b" || n == "strong" || n == "i" || n == "em" || n == "u" || n == "s" || n == "strike" || n == "del" ||
        n == "font" || n == "span") {
      if (t.closing) {
        for (size_t k = inline_.size(); k-- > 0;)
          if (inline_[k].first == n) {
            inline_.erase(inline_.begin() + static_cast<std::ptrdiff_t>(k));
            break;
          }
        continue;
      }
      if (t.selfClosing) continue;
      Look l;
      if (n == "b" || n == "strong") l.bold = 1;
      if (n == "i" || n == "em") l.italic = 1;
      if (n == "u") l.underline = 1;
      if (n == "s" || n == "strike" || n == "del") l.strike = 1;
      if (t.has("color")) l.color = ParseColor(t.get("color"));
      if (t.has("class"))
        if (auto it = classes.find(Lower(t.get("class"))); it != classes.end()) Merge(it->second, l);
      if (t.has("style")) Apply(ParseDecls(t.get("style")), l);
      if (inline_.size() < 64) inline_.emplace_back(n, l);
      continue;
    }
  }
  flushPara();

  // Each cell's text: trimmed, and in the cell's format.
  for (Open& o : cells) {
    std::string& s = o.text;
    // Spaces around line breaks go, and the ends' blanks; spaces from text on both sides of a
    // tag are one, as a browser shows them.
    std::string c;
    for (size_t k = 0; k < s.size(); ++k) {
      if (s[k] == ' ' && ((k + 1 < s.size() && s[k + 1] == '\n') || (!c.empty() && (c.back() == '\n' || c.back() == ' '))))
        continue;
      c += s[k];
    }
    while (!c.empty() && (c.back() == '\n' || c.back() == ' ')) c.pop_back();
    size_t lead = 0;
    while (lead < c.size() && (c[lead] == '\n' || c[lead] == ' ')) ++lead;
    c.erase(0, lead);
    // No-break spaces read as spaces, as Excel pastes them.
    std::string plain;
    for (size_t k = 0; k < c.size(); ++k) {
      if (static_cast<unsigned char>(c[k]) == 0xC2 && k + 1 < c.size() && static_cast<unsigned char>(c[k + 1]) == 0xA0) {
        plain += ' ';
        ++k;
      } else {
        plain += c[k];
      }
    }
    HtmlCell cell = std::move(o.cell);
    // x:str="value": the value is the text, whatever is shown.
    if (!(cell.type == ValueType::String && !o.textValue)) cell.text = Utf8ToWide(plain);
    const Look& l = o.look;
    cell.numFmt = FormatCode(l.numFmt);
    cell.bold = l.bold > 0;
    cell.italic = l.italic > 0;
    cell.underline = l.underline > 0;
    cell.strike = l.strike > 0;
    // Black text and a white fill are what a cell has anyway.
    cell.color = l.color == 0xFF000000u ? 0 : l.color;
    cell.fill = l.fill == 0xFFFFFFFFu ? 0 : l.fill;
    cell.align = l.align >= 0 ? static_cast<HAlign>(l.align) : HAlign::General;
    if (out.excel) {
      // Excel's own copy: its fonts and wrapping too.
      cell.size = l.size;
      cell.font = l.font;
      cell.wrap = l.wrap > 0;
    }
    out.rows = std::max(out.rows, cell.row + 1);
    out.cols = std::max(out.cols, cell.col + 1);
    out.cells.push_back(std::move(cell));
  }
  for (const Range& m : out.merges) {
    out.rows = std::max(out.rows, m.lastRow + 1);
    out.cols = std::max(out.cols, m.lastCol + 1);
  }
  std::stable_sort(out.cells.begin(), out.cells.end(),
                   [](const HtmlCell& a, const HtmlCell& b) { return a.row != b.row ? a.row < b.row : a.col < b.col; });
  bool any = false;
  for (const HtmlCell& c : out.cells) any = any || !c.text.empty() || c.type != ValueType::Empty;
  return any && out.cells.size() <= maxCells;
}

}  // namespace fw::sheet::view
