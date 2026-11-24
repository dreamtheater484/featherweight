// Custom shapes of an OpenDocument presentation as DrawingML geometry (P3, p3-files). An enhanced
// geometry is a path in a viewBox whose coordinates are equations ("?f3", "$0", "left / 2"):
// they're evaluated once, for the shape as it is, and the path is written with plain numbers (the
// shape can still be resized: a:path scales with it). Arcs become cubic Béziers.

#include "slide/odp/OdpGeometry.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_map>
#include <vector>

#include "pkg/Odf.h"

namespace fw::slide::odp {
namespace {

using pugi::xml_node;

constexpr double kPi = 3.14159265358979323846;
constexpr size_t kMaxEquations = 4096;
constexpr int kMaxNesting = 64;          // parentheses and references within one equation
constexpr size_t kMaxSegments = 20000;   // path commands written for one shape

bool Space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ','; }
bool NameChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// A number at the start of `s` (digits, a point, an exponent), and how many characters it took.
std::optional<double> Number(std::string_view s, size_t& used) {
  size_t i = 0;
  if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
  const size_t digits = i;
  while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) ++i;
  if (i == digits) return std::nullopt;
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    size_t j = i + 1;
    if (j < s.size() && (s[j] == '-' || s[j] == '+')) ++j;
    if (j < s.size() && s[j] >= '0' && s[j] <= '9') {
      i = j;
      while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    }
  }
  double v = 0;
  try {
    v = std::stod(std::string(s.substr(0, i)));
  } catch (...) {
    return std::nullopt;
  }
  used = i;
  return std::isfinite(v) ? std::optional<double>(v) : std::nullopt;
}

double Finite(double v) { return std::isfinite(v) ? std::clamp(v, -1e12, 1e12) : 0; }

// The equations of one enhanced geometry, evaluated for one shape.
class Equations {
 public:
  Equations(xml_node geometry, double left, double top, double width, double height, int64_t w, int64_t h)
      : left_(left), top_(top), width_(width), height_(height), w_(static_cast<double>(w)), h_(static_cast<double>(h)) {
    for (xml_node e = geometry.child("draw:equation"); e && formulas_.size() < kMaxEquations; e = e.next_sibling("draw:equation")) {
      names_.emplace(e.attribute("draw:name").value(), formulas_.size());
      formulas_.push_back(e.attribute("draw:formula").value());
    }
    std::string_view mods = geometry.attribute("draw:modifiers").value();
    while (!mods.empty() && modifiers_.size() < 64) {
      while (!mods.empty() && Space(mods.front())) mods.remove_prefix(1);
      size_t used = 0;
      const auto v = Number(mods, used);
      if (!v) break;
      modifiers_.push_back(*v);
      mods.remove_prefix(used);
    }
    values_.assign(formulas_.size(), 0);
    state_.assign(formulas_.size(), 0);
    // In order: an equation mostly names earlier ones, so few references nest.
    for (size_t i = 0; i < formulas_.size(); ++i) Equation(i, 0);
  }

  // A path, text area or handle parameter: a number, "?name" or "$n".
  double Value(std::string_view token) {
    if (token.empty()) return 0;
    if (token[0] == '?') return Reference(token.substr(1), 0);
    if (token[0] == '$') return Modifier(token.substr(1));
    size_t used = 0;
    const auto v = Number(token, used);
    return v ? *v : Constant(token);
  }

 private:
  double Reference(std::string_view name, int depth) {
    auto it = names_.find(name);
    return it == names_.end() ? 0 : Equation(it->second, depth + 1);
  }

  double Modifier(std::string_view digits) {
    size_t used = 0;
    const auto v = Number(digits, used);
    if (!v || *v < 0) return 0;
    const size_t i = static_cast<size_t>(*v);
    return i < modifiers_.size() ? modifiers_[i] : 0;
  }

  double Constant(std::string_view name) const {
    if (name == "pi") return kPi;
    if (name == "left") return left_;
    if (name == "top") return top_;
    if (name == "right") return left_ + width_;
    if (name == "bottom") return top_ + height_;
    if (name == "width") return width_;
    if (name == "height") return height_;
    if (name == "logwidth") return w_ / 360;  // 1/100 mm
    if (name == "logheight") return h_ / 360;
    if (name == "xstretch" || name == "ystretch" || name == "hasstroke" || name == "hasfill") return 1;
    return 0;
  }

  double Equation(size_t i, int depth) {
    if (state_[i] == 2) return values_[i];
    if (state_[i] == 1 || depth > kMaxNesting) return 0;  // a cycle, or nested too deep
    state_[i] = 1;
    Cursor c{formulas_[i], 0, depth};
    values_[i] = Finite(Sum(c));
    state_[i] = 2;
    return values_[i];
  }

  struct Cursor {
    std::string_view s;
    size_t at;
    int depth;
    char Peek() {
      while (at < s.size() && (s[at] == ' ' || s[at] == '\t')) ++at;
      return at < s.size() ? s[at] : '\0';
    }
  };

  double Sum(Cursor& c) {
    double v = Product(c);
    for (char op = c.Peek(); op == '+' || op == '-'; op = c.Peek()) {
      ++c.at;
      const double r = Product(c);
      v = op == '+' ? v + r : v - r;
    }
    return v;
  }

  double Product(Cursor& c) {
    double v = Unary(c);
    for (char op = c.Peek(); op == '*' || op == '/'; op = c.Peek()) {
      ++c.at;
      const double r = Unary(c);
      v = op == '*' ? v * r : (r == 0 ? 0 : v / r);
    }
    return v;
  }

  double Unary(Cursor& c) {
    if (++c.depth > kMaxNesting) return 0;
    double v = 0;
    const char ch = c.Peek();
    if (ch == '-') {
      ++c.at;
      v = -Unary(c);
    } else if (ch == '+') {
      ++c.at;
      v = Unary(c);
    } else {
      v = Primary(c);
    }
    --c.depth;
    return Finite(v);
  }

  double Primary(Cursor& c) {
    const char ch = c.Peek();
    if (ch == '(') {
      ++c.at;
      const double v = Sum(c);
      if (c.Peek() == ')') ++c.at;
      return v;
    }
    if (ch == '?' || ch == '$') {
      const size_t start = ++c.at;
      while (c.at < c.s.size() && NameChar(c.s[c.at])) ++c.at;
      const std::string_view name = c.s.substr(start, c.at - start);
      return ch == '?' ? Reference(name, c.depth) : Modifier(name);
    }
    if ((ch >= '0' && ch <= '9') || ch == '.') {
      size_t used = 0;
      const auto v = Number(c.s.substr(c.at), used);
      c.at += v ? used : 1;
      return v ? *v : 0;
    }
    if (!NameChar(ch)) {
      if (ch) ++c.at;  // something else: skipped
      return 0;
    }
    const size_t start = c.at;
    while (c.at < c.s.size() && NameChar(c.s[c.at])) ++c.at;
    const std::string_view name = c.s.substr(start, c.at - start);
    if (c.Peek() != '(') return Constant(name);
    ++c.at;
    double args[3] = {0, 0, 0};
    int n = 0;
    for (;;) {
      const double v = Sum(c);
      if (n < 3) args[n++] = v;
      const char next = c.Peek();
      if (next == ',') {
        ++c.at;
        continue;
      }
      if (next == ')') ++c.at;
      break;
    }
    const double x = args[0], y = args[1];
    if (name == "abs") return std::abs(x);
    if (name == "sqrt") return x > 0 ? std::sqrt(x) : 0;
    if (name == "sin") return std::sin(x);
    if (name == "cos") return std::cos(x);
    if (name == "tan") return std::tan(x);
    if (name == "atan") return std::atan(x);
    if (name == "atan2") return std::atan2(x, y);
    if (name == "min") return std::min(x, y);
    if (name == "max") return std::max(x, y);
    if (name == "if") return x > 0 ? y : args[2];
    return 0;
  }

  double left_, top_, width_, height_, w_, h_;
  std::vector<std::string_view> formulas_;
  std::unordered_map<std::string_view, size_t> names_;
  std::vector<double> values_;
  std::vector<uint8_t> state_;  // 0 not yet, 1 being evaluated, 2 done
  std::vector<double> modifiers_;
};

// a:path elements for a pathLst, in a viewBox's units.
class PathWriter {
 public:
  PathWriter(double left, double top, double width, double height)
      : left_(left), top_(top), w_(std::max<int64_t>(1, std::llround(Finite(width)))), h_(std::max<int64_t>(1, std::llround(Finite(height)))) {}

  void NoFill() { noFill_ = true; }
  void NoStroke() { noStroke_ = true; }
  bool Full() const { return segments_ > kMaxSegments; }
  bool HasPoint() const { return started_; }

  void Move(double x, double y) {
    Cmd("moveTo", {x, y});
    px_ = sx_ = x, py_ = sy_ = y;
    started_ = true;
  }
  void Line(double x, double y) {
    if (!started_) return Move(x, y);
    if (x == px_ && y == py_) return;  // to where it is (PowerPoint's empty arc before a real one)
    Cmd("lnTo", {x, y});
    px_ = x, py_ = y;
  }
  void Quad(double x1, double y1, double x, double y) {
    if (!started_) Move(px_, py_);
    Cmd("quadBezTo", {x1, y1, x, y});
    px_ = x, py_ = y;
  }
  void Cubic(double x1, double y1, double x2, double y2, double x, double y) {
    if (!started_) Move(px_, py_);
    Cmd("cubicBezTo", {x1, y1, x2, y2, x, y});
    px_ = x, py_ = y;
  }
  void Close() {
    if (!started_) return;
    cur_ += "<a:close/>";
    px_ = sx_, py_ = sy_;
  }
  // An elliptic arc: centre, radii, start angle and sweep in radians, y down (a positive sweep
  // turns clockwise on screen). `line`: a line to its start first, else a new subpath there.
  void Arc(double cx, double cy, double rx, double ry, double start, double sweep, bool line) {
    const double x0 = cx + rx * std::cos(start), y0 = cy + ry * std::sin(start);
    if (line && started_) Line(x0, y0);
    else Move(x0, y0);
    sweep = std::clamp(sweep, -2 * kPi, 2 * kPi);
    const int n = std::max(1, static_cast<int>(std::ceil(std::abs(sweep) / (kPi / 2) - 1e-9)));
    const double t = sweep / n, k = 4.0 / 3.0 * std::tan(t / 4);
    double a = start;
    for (int i = 0; i < n; ++i) {
      const double b = a + t;
      const double ax = cx + rx * std::cos(a), ay = cy + ry * std::sin(a);
      const double bx = cx + rx * std::cos(b), by = cy + ry * std::sin(b);
      Cubic(ax - k * rx * std::sin(a), ay + k * ry * std::cos(a), bx + k * rx * std::sin(b), by - k * ry * std::cos(b), bx, by);
      a = b;
    }
  }
  double X() const { return px_; }
  double Y() const { return py_; }

  // Ends the path (ODF's "N"): what F and S said applies to it alone.
  void End() {
    if (!cur_.empty()) {
      out_ += "<a:path w=\"" + std::to_string(w_) + "\" h=\"" + std::to_string(h_) + "\"";
      if (noFill_) out_ += " fill=\"none\"";
      if (noStroke_) out_ += " stroke=\"0\"";
      out_ += ">" + cur_ + "</a:path>";
    }
    cur_.clear();
    noFill_ = noStroke_ = started_ = false;
  }
  const std::string& Paths() const { return out_; }

 private:
  void Cmd(const char* name, std::initializer_list<double> xy) {
    ++segments_;
    cur_ += "<a:";
    cur_ += name;
    cur_ += ">";
    for (auto it = xy.begin(); it != xy.end(); it += 2) {
      cur_ += "<a:pt x=\"" + std::to_string(std::llround(Finite(*it - left_))) + "\" y=\"" +
              std::to_string(std::llround(Finite(*(it + 1) - top_))) + "\"/>";
    }
    cur_ += "</a:";
    cur_ += name;
    cur_ += ">";
  }

  double left_, top_;
  int64_t w_, h_;
  std::string out_, cur_;
  bool noFill_ = false, noStroke_ = false, started_ = false;
  double px_ = 0, py_ = 0, sx_ = 0, sy_ = 0;
  size_t segments_ = 0;
};

// The path's tokens: a command letter, or a parameter ("12.5", "?f3", "$0").
class Tokens {
 public:
  explicit Tokens(std::string_view s) : s_(s) {}
  // The next token; empty at the end.
  std::string_view Next() {
    while (at_ < s_.size() && Space(s_[at_])) ++at_;
    if (at_ >= s_.size()) return {};
    const size_t start = at_;
    const char c = s_[at_];
    if (c >= 'A' && c <= 'Z') {
      ++at_;
    } else if (c == '?' || c == '$') {
      ++at_;
      while (at_ < s_.size() && NameChar(s_[at_])) ++at_;
    } else {
      size_t used = 0;
      if (Number(s_.substr(at_), used)) at_ += used;
      else ++at_;  // not a number: one character, read as 0
    }
    return s_.substr(start, at_ - start);
  }
  static bool Command(std::string_view t) { return t.size() == 1 && t[0] >= 'A' && t[0] <= 'Z'; }

 private:
  std::string_view s_;
  size_t at_ = 0;
};

std::string Rect(int64_t l, int64_t t, int64_t r, int64_t b) {
  return "<a:rect l=\"" + std::to_string(l) + "\" t=\"" + std::to_string(t) + "\" r=\"" + std::to_string(r) + "\" b=\"" +
         std::to_string(b) + "\"/>";
}

std::string Geometry(const std::string& rect, const std::string& paths) {
  return "<a:custGeom><a:avLst/><a:gdLst/><a:ahLst/><a:cxnLst/>" + rect + "<a:pathLst>" + paths + "</a:pathLst></a:custGeom>";
}

bool ViewBox(std::string_view text, double box[4]) {
  for (int i = 0; i < 4; ++i) {
    while (!text.empty() && Space(text.front())) text.remove_prefix(1);
    size_t used = 0;
    const auto v = Number(text, used);
    if (!v) return false;
    box[i] = *v;
    text.remove_prefix(used);
  }
  return box[2] > 0 && box[3] > 0;
}

}  // namespace

std::string CustomGeometry(xml_node geometry, int64_t w, int64_t h) {
  double vb[4];
  if (!ViewBox(geometry.attribute("svg:viewBox").value(), vb)) vb[0] = vb[1] = 0, vb[2] = vb[3] = 21600;
  std::string_view path = geometry.attribute("draw:enhanced-path").value();
  if (path.empty()) return {};
  Equations eq(geometry, vb[0], vb[1], vb[2], vb[3], w, h);
  PathWriter out(vb[0], vb[1], vb[2], vb[3]);
  Tokens tokens(path);
  std::string_view t = tokens.Next();
  char cmd = 'M';
  int quadrant = 0;  // X and Y alternate
  while (!t.empty() && !out.Full()) {
    if (Tokens::Command(t)) {
      cmd = t[0];
      if (cmd == 'X') quadrant = 0;
      if (cmd == 'Y') quadrant = 1;
      t = tokens.Next();
      switch (cmd) {
        case 'Z': out.Close(); continue;
        case 'N': out.End(); continue;
        case 'F': out.NoFill(); continue;
        case 'S': out.NoStroke(); continue;
        default: break;
      }
    }
    // Parameters for `cmd`, as many groups as there are.
    const int count = cmd == 'C' ? 6 : (cmd == 'Q' ? 4 : (cmd == 'A' || cmd == 'B' || cmd == 'W' || cmd == 'V') ? 8
                                                       : (cmd == 'T' || cmd == 'U') ? 6 : 2);
    double p[8];
    int got = 0;
    while (got < count && !t.empty() && !Tokens::Command(t)) {
      p[got++] = eq.Value(t);
      t = tokens.Next();
    }
    if (got < count) continue;  // a short group: dropped
    switch (cmd) {
      case 'M':
        out.Move(p[0], p[1]);
        cmd = 'L';  // more pairs are lines
        break;
      case 'L': out.Line(p[0], p[1]); break;
      case 'C': out.Cubic(p[0], p[1], p[2], p[3], p[4], p[5]); break;
      case 'Q': out.Quad(p[0], p[1], p[2], p[3]); break;
      case 'A': case 'B': case 'W': case 'V': {
        // The ellipse in the rectangle (p0, p1)-(p2, p3), from the direction of (p4, p5) to that of
        // (p6, p7): A and B counter-clockwise, W and V clockwise (on screen).
        const double cx = (p[0] + p[2]) / 2, cy = (p[1] + p[3]) / 2;
        const double rx = std::abs(p[2] - p[0]) / 2, ry = std::abs(p[3] - p[1]) / 2;
        const bool line = cmd == 'A' || cmd == 'W';
        if (rx < 1e-9 && ry < 1e-9) {
          if (line) out.Line(cx, cy);
          else out.Move(cx, cy);
          break;
        }
        const double a0 = std::atan2((p[5] - cy) * rx, (p[4] - cx) * ry);
        const double a1 = std::atan2((p[7] - cy) * rx, (p[6] - cx) * ry);
        double sweep = a1 - a0;
        if (cmd == 'W' || cmd == 'V') {
          if (sweep < 0) sweep += 2 * kPi;
        } else if (sweep > 0) {
          sweep -= 2 * kPi;
        }
        out.Arc(cx, cy, rx, ry, a0, sweep, line);
        break;
      }
      case 'T': case 'U': {
        // Centre, radii, start and end angles in degrees (counter-clockwise).
        const double a0 = -p[4] * kPi / 180;
        double sweep = -(p[5] - p[4]) * kPi / 180;
        if (sweep == 0) sweep = -2 * kPi;
        out.Arc(p[0], p[1], std::abs(p[2]), std::abs(p[3]), a0, sweep, cmd == 'T');
        break;
      }
      case 'X': case 'Y': {
        // A quarter ellipse to (p0, p1), leaving horizontally (X) or vertically (Y); they alternate.
        constexpr double k = 0.5522847498;
        const double x0 = out.X(), y0 = out.Y(), dx = p[0] - x0, dy = p[1] - y0;
        if (quadrant == 0) out.Cubic(x0 + k * dx, y0, p[0], p[1] - k * dy, p[0], p[1]);
        else out.Cubic(x0, y0 + k * dy, p[0] - k * dx, p[1], p[0], p[1]);
        quadrant ^= 1;
        break;
      }
      default: break;  // an unknown command: its parameters are dropped
    }
  }
  out.End();
  if (out.Paths().empty()) return {};

  // The text rectangle: the first text area, in the shape's EMU.
  std::string rect;
  Tokens area(geometry.attribute("draw:text-areas").value());
  double r[4];
  int got = 0;
  for (std::string_view a = area.Next(); !a.empty() && got < 4; a = area.Next()) r[got++] = eq.Value(a);
  if (got == 4) {
    auto ex = [&](double x) { return std::llround(Finite((x - vb[0]) * static_cast<double>(w) / vb[2])); };
    auto ey = [&](double y) { return std::llround(Finite((y - vb[1]) * static_cast<double>(h) / vb[3])); };
    rect = Rect(ex(std::min(r[0], r[2])), ey(std::min(r[1], r[3])), ex(std::max(r[0], r[2])), ey(std::max(r[1], r[3])));
  }
  return Geometry(rect, out.Paths());
}

std::string PolygonGeometry(std::string_view points, std::string_view viewBox, bool closed) {
  double vb[4];
  if (!ViewBox(viewBox, vb)) return {};
  PathWriter out(vb[0], vb[1], vb[2], vb[3]);
  if (!closed) out.NoFill();
  Tokens t(points);
  for (;;) {
    std::string_view x = t.Next(), y = t.Next();
    if (y.empty() || out.Full()) break;
    size_t used = 0;
    const double px = Number(x, used).value_or(0), py = Number(y, used).value_or(0);
    if (out.HasPoint()) out.Line(px, py);
    else out.Move(px, py);
  }
  if (closed) out.Close();
  out.End();
  return out.Paths().empty() ? std::string() : Geometry({}, out.Paths());
}

bool Transform::Read(std::string_view text) {
  *this = Transform();
  bool any = false;
  while (true) {
    while (!text.empty() && Space(text.front())) text.remove_prefix(1);
    const size_t open = text.find('(');
    if (text.empty() || open == std::string_view::npos) break;
    std::string_view name = text.substr(0, open);
    while (!name.empty() && Space(name.back())) name.remove_suffix(1);
    const size_t close = text.find(')', open);
    if (close == std::string_view::npos) return false;
    std::string_view args = text.substr(open + 1, close - open - 1);
    text.remove_prefix(close + 1);
    // The arguments: numbers, or lengths (as EMU).
    double v[6] = {0, 0, 0, 0, 0, 0};
    int n = 0;
    while (n < 6) {
      while (!args.empty() && Space(args.front())) args.remove_prefix(1);
      if (args.empty()) break;
      size_t end = 0;
      while (end < args.size() && !Space(args[end])) ++end;
      const std::string_view word = args.substr(0, end);
      args.remove_prefix(end);
      size_t used = 0;
      const auto num = Number(word, used);
      if (!num) return false;
      if (used < word.size()) {
        const auto pt = pkg::odf::Points(word);
        v[n++] = pt ? *pt * 12700 : 0;
      } else {
        v[n++] = *num;
      }
    }
    Transform op;
    if (name == "rotate" && n >= 1) {
      op.a = std::cos(v[0]), op.b = std::sin(v[0]), op.c = -std::sin(v[0]), op.d = std::cos(v[0]);
    } else if (name == "translate" && n >= 1) {
      op.e = v[0], op.f = n > 1 ? v[1] : 0;
    } else if (name == "scale" && n >= 1) {
      op.a = v[0], op.d = n > 1 ? v[1] : v[0];
    } else if (name == "matrix" && n == 6) {
      op.a = v[0], op.c = v[1], op.b = v[2], op.d = v[3], op.e = v[4], op.f = v[5];
    } else {
      continue;  // skewX, skewY: not kept
    }
    any = true;
    const Transform t = *this;
    a = op.a * t.a + op.b * t.c, b = op.a * t.b + op.b * t.d, e = op.a * t.e + op.b * t.f + op.e;
    c = op.c * t.a + op.d * t.c, d = op.c * t.b + op.d * t.d, f = op.c * t.e + op.d * t.f + op.f;
  }
  a = Finite(a), b = Finite(b), c = Finite(c), d = Finite(d), e = Finite(e), f = Finite(f);
  return any;
}

double Transform::Degrees() const {
  double deg = -std::atan2(b, a) * 180 / kPi;
  if (!std::isfinite(deg)) return 0;
  deg = std::fmod(deg, 360.0);
  if (deg < 0) deg += 360;
  return deg >= 360 ? 0 : deg;
}

}  // namespace fw::slide::odp
