#include "draw/Geometry.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iterator>
#include <mutex>
#include <string>

#include <miniz.h>

namespace draw {
namespace {

// The preset shapes: their names, where each one's statements start in the text, and the text,
// deflated (tools/make_preset_shapes.py).
struct PresetDef {
  const char* name;
  uint32_t offset, size;
};
#include "draw/PresetShapes.inc"

// The presets' text, inflated when the first one is drawn.
std::string_view PresetText() {
  static const std::string text = [] {
    std::string t(kPresetTextSize, 0);
    if (tinfl_decompress_mem_to_mem(t.data(), t.size(), kPresetData, sizeof(kPresetData), 0) != t.size()) t.clear();
    return t;
  }();
  return text;
}

constexpr double kPi = 3.14159265358979323846;
constexpr double kEmuPerPoint = 12700;
// Angles are in 1/60000 of a degree.
constexpr double kAngle = kPi / (180.0 * 60000.0);

// DrawingML's built-in guide names (§20.1.9.11), in the order Evaluate fills them in.
constexpr std::string_view kBuiltins[] = {
    "w",   "h",    "l",    "t",    "r",    "b",    "hc",   "vc",    "wd2",   "wd3",  "wd4",  "wd5",  "wd6",  "wd8",
    "wd10", "wd12", "wd32", "hd2",  "hd3",  "hd4",  "hd5",  "hd6",   "hd8",   "hd10", "hd12", "hd32", "ss",   "ls",
    "ssd2", "ssd4", "ssd6", "ssd8", "ssd16", "ssd32", "cd2", "cd4", "cd8", "3cd4", "3cd8", "5cd8", "7cd8", "cd3"};
constexpr int kBuiltinCount = static_cast<int>(std::size(kBuiltins));

// Caps for custom geometry from untrusted files.
constexpr size_t kMaxNames = 4096;
constexpr size_t kMaxPaths = 1024;
constexpr size_t kMaxCommands = 65536;

void FillBuiltins(double w, double h, double* v) {
  const double ss = std::min(w, h), ls = std::max(w, h);
  const double values[kBuiltinCount] = {
      w,      h,      0,      0,      w,      h,      w / 2,  h / 2,  w / 2,  w / 3,  w / 4,  w / 5,  w / 6,
      w / 8,  w / 10, w / 12, w / 32, h / 2,  h / 3,  h / 4,  h / 5,  h / 6,  h / 8,  h / 10, h / 12, h / 32,
      ss,     ls,     ss / 2, ss / 4, ss / 6, ss / 8, ss / 16, ss / 32, 10800000, 5400000, 2700000, 16200000,
      8100000, 13500000, 18900000, 7200000};
  std::copy(std::begin(values), std::end(values), v);
}

// Guides multiply squared distances (circularArrow reaches 1e20 EMU^4 at an inch), so they get
// a wide range; a product of two stays finite. Coordinates are clamped when they become points.
double Sane(double x) {
  if (!std::isfinite(x)) return 0;
  return std::clamp(x, -1e100, 1e100);
}

float ToPoints(double emu) { return static_cast<float>(std::clamp(emu / kEmuPerPoint, -1e6, 1e6)); }

std::vector<std::string_view> Tokens(std::string_view s) {
  std::vector<std::string_view> out;
  size_t i = 0;
  while (i < s.size() && out.size() < 16) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    size_t j = i;
    while (j < s.size() && s[j] != ' ' && s[j] != '\t' && s[j] != '\n' && s[j] != '\r') ++j;
    if (j > i) out.push_back(s.substr(i, j - i));
    i = j;
  }
  return out;
}

}  // namespace

Geometry::Operand Geometry::ParseOperand(std::string_view token) const {
  for (size_t i = names_.size(); i-- > 0;)
    if (names_[i] == token) return {0, kBuiltinCount + static_cast<int>(i)};
  for (int i = 0; i < kBuiltinCount; ++i)
    if (kBuiltins[i] == token) return {0, i};
  double d = 0;
  if (!token.empty() && token.front() == '+') token.remove_prefix(1);
  auto [end, ec] = std::from_chars(token.data(), token.data() + token.size(), d);
  if (ec != std::errc() || end != token.data() + token.size()) d = 0;
  return {Sane(d), -1};
}

Geometry::Formula Geometry::ParseFormula(std::string_view text) const {
  struct Name {
    std::string_view name;
    Op op;
  };
  static constexpr Name kOps[] = {{"val", Op::Val},   {"*/", Op::MulDiv}, {"+-", Op::AddSub}, {"+/", Op::AddDiv},
                                  {"?:", Op::IfElse}, {"abs", Op::Abs},   {"at2", Op::At2},   {"cat2", Op::Cat2},
                                  {"cos", Op::Cos},   {"max", Op::Max},   {"min", Op::Min},   {"mod", Op::Mod},
                                  {"pin", Op::Pin},   {"sat2", Op::Sat2}, {"sin", Op::Sin},   {"sqrt", Op::Sqrt},
                                  {"tan", Op::Tan}};
  Formula f;
  std::vector<std::string_view> t = Tokens(text);
  if (t.empty()) return f;
  size_t first = 1;
  auto op = std::find_if(std::begin(kOps), std::end(kOps), [&](const Name& n) { return n.name == t[0]; });
  if (op != std::end(kOps)) f.op = op->op;
  else first = 0;  // no operator: take the value as written
  Operand* ops[3] = {&f.a, &f.b, &f.c};
  for (size_t i = 0; i < 3 && first + i < t.size(); ++i) *ops[i] = ParseOperand(t[first + i]);
  return f;
}

void Geometry::Define(std::string_view name, std::string_view formula, bool adjust) {
  if (names_.size() >= kMaxNames) return;
  Formula f = ParseFormula(formula);
  f.adjust = adjust;
  f.var = kBuiltinCount + static_cast<int>(names_.size());
  names_.emplace_back(name);
  formulas_.push_back(f);
}

void Geometry::AddAdjust(std::string_view name, std::string_view formula) { Define(name, formula, true); }
void Geometry::AddGuide(std::string_view name, std::string_view formula) { Define(name, formula, false); }

void Geometry::BeginPath(double width, double height, PathFill fill, bool stroke) {
  if (paths_.size() >= kMaxPaths) return;
  PathDef p;
  p.width = std::isfinite(width) && width > 0 ? width : 0;
  p.height = std::isfinite(height) && height > 0 ? height : 0;
  p.fill = fill;
  p.stroke = stroke;
  paths_.push_back(std::move(p));
}

void Geometry::Add(Cmd cmd, std::initializer_list<std::string_view> args) {
  if (commands_ >= kMaxCommands) return;
  if (paths_.empty()) BeginPath(0, 0, PathFill::Normal, true);
  Command c{cmd, {}};
  int i = 0;
  for (std::string_view a : args) c.args[i++] = ParseOperand(a);
  paths_.back().commands.push_back(c);
  ++commands_;
}

void Geometry::MoveTo(std::string_view x, std::string_view y) { Add(Cmd::Move, {x, y}); }
void Geometry::LineTo(std::string_view x, std::string_view y) { Add(Cmd::Line, {x, y}); }
void Geometry::ArcTo(std::string_view wR, std::string_view hR, std::string_view stAng, std::string_view swAng) {
  Add(Cmd::Arc, {wR, hR, stAng, swAng});
}
void Geometry::QuadTo(std::string_view x1, std::string_view y1, std::string_view x2, std::string_view y2) {
  Add(Cmd::Quad, {x1, y1, x2, y2});
}
void Geometry::CubicTo(std::string_view x1, std::string_view y1, std::string_view x2, std::string_view y2,
                       std::string_view x3, std::string_view y3) {
  Add(Cmd::Cubic, {x1, y1, x2, y2, x3, y3});
}
void Geometry::Close() { Add(Cmd::Close, {}); }

void Geometry::SetTextRect(std::string_view l, std::string_view t, std::string_view r, std::string_view b) {
  rect_[0] = ParseOperand(l);
  rect_[1] = ParseOperand(t);
  rect_[2] = ParseOperand(r);
  rect_[3] = ParseOperand(b);
  hasRect_ = true;
}

// The statements tools/make_preset_shapes.py writes.
void Geometry::ReadCompact(std::string_view def) {
  while (!def.empty()) {
    size_t end = def.find(';');
    std::string_view stmt = def.substr(0, end);
    def = end == std::string_view::npos ? std::string_view() : def.substr(end + 1);
    if (stmt.size() < 1) continue;
    const char kind = stmt[0];
    std::string_view rest = stmt.size() > 2 ? stmt.substr(2) : std::string_view();
    if (kind == 'a' || kind == 'g') {
      size_t sp = rest.find(' ');
      Define(rest.substr(0, sp), sp == std::string_view::npos ? "" : rest.substr(sp + 1), kind == 'a');
      continue;
    }
    std::vector<std::string_view> t = Tokens(rest);
    t.resize(6);
    switch (kind) {
      case 'p': {
        static constexpr char kFills[] = "n0lLdD";
        const char* f = std::find(kFills, kFills + 6, t[2].empty() ? 'n' : t[2][0]);
        BeginPath(ParseOperand(t[0]).constant, ParseOperand(t[1]).constant,
                  static_cast<PathFill>(f < kFills + 6 ? f - kFills : 0), t[3] != "0");
        break;
      }
      case 'm': MoveTo(t[0], t[1]); break;
      case 'l': LineTo(t[0], t[1]); break;
      case 'A': ArcTo(t[0], t[1], t[2], t[3]); break;
      case 'q': QuadTo(t[0], t[1], t[2], t[3]); break;
      case 'c': CubicTo(t[0], t[1], t[2], t[3], t[4], t[5]); break;
      case 'z': Close(); break;
      case 'r': SetTextRect(t[0], t[1], t[2], t[3]); break;
      default: break;
    }
  }
}

std::shared_ptr<const Geometry> Geometry::Preset(std::string_view name) {
  static std::mutex mutex;
  static std::shared_ptr<const Geometry> cache[std::size(kPresets)];
  auto it = std::lower_bound(std::begin(kPresets), std::end(kPresets), name,
                             [](const PresetDef& p, std::string_view n) { return std::string_view(p.name) < n; });
  if (it == std::end(kPresets) || std::string_view(it->name) != name) return nullptr;
  std::lock_guard<std::mutex> lock(mutex);
  std::shared_ptr<const Geometry>& slot = cache[it - std::begin(kPresets)];
  if (!slot) {
    auto g = std::make_shared<Geometry>();
    const std::string_view text = PresetText();
    if (it->offset + it->size <= text.size()) g->ReadCompact(text.substr(it->offset, it->size));
    slot = std::move(g);
  }
  return slot;
}

std::vector<std::string_view> Geometry::PresetNames() {
  std::vector<std::string_view> names;
  for (const PresetDef& p : kPresets) names.emplace_back(p.name);
  return names;
}

Geometry::Outline Geometry::Evaluate(Size size, const std::vector<Adjust>& adjust) const {
  const double w = std::max(0.f, size.width) * kEmuPerPoint, h = std::max(0.f, size.height) * kEmuPerPoint;
  std::vector<double> v(kBuiltinCount + names_.size(), 0.0);
  FillBuiltins(w, h, v.data());
  auto val = [&](const Operand& o) { return o.var >= 0 && o.var < static_cast<int>(v.size()) ? v[o.var] : o.constant; };
  for (const Formula& f : formulas_) {
    if (f.adjust) {
      const std::string& name = names_[f.var - kBuiltinCount];
      auto given = std::find_if(adjust.begin(), adjust.end(), [&](const Adjust& a) { return a.name == name; });
      if (given != adjust.end()) {
        v[f.var] = Sane(given->value);
        continue;
      }
    }
    const double a = val(f.a), b = val(f.b), c = val(f.c);
    double r = 0;
    switch (f.op) {
      case Op::Val: r = a; break;
      case Op::MulDiv: r = c == 0 ? 0 : a * b / c; break;
      case Op::AddSub: r = a + b - c; break;
      case Op::AddDiv: r = c == 0 ? 0 : (a + b) / c; break;
      case Op::IfElse: r = a > 0 ? b : c; break;
      case Op::Abs: r = std::abs(a); break;
      case Op::At2: r = std::atan2(b, a) / kAngle; break;
      case Op::Cat2: r = a * std::cos(std::atan2(c, b)); break;
      case Op::Cos: r = a * std::cos(b * kAngle); break;
      case Op::Max: r = std::max(a, b); break;
      case Op::Min: r = std::min(a, b); break;
      case Op::Mod: r = std::sqrt(a * a + b * b + c * c); break;
      case Op::Pin: r = b < a ? a : b > c ? c : b; break;
      case Op::Sat2: r = a * std::sin(std::atan2(c, b)); break;
      case Op::Sin: r = a * std::sin(b * kAngle); break;
      case Op::Sqrt: r = std::sqrt(std::max(0.0, a)); break;
      case Op::Tan: r = a * std::tan(b * kAngle); break;
    }
    v[f.var] = Sane(r);
  }

  Outline out;
  for (const PathDef& def : paths_) {
    Figure fig;
    fig.path.fillRule = Path::FillRule::EvenOdd;
    fig.fill = def.fill;
    fig.stroke = def.stroke;
    const double sx = def.width > 0 ? w / def.width : 1, sy = def.height > 0 ? h / def.height : 1;
    auto pt = [&](const Operand& x, const Operand& y) {
      return Point{ToPoints(val(x) * sx), ToPoints(val(y) * sy)};
    };
    double cx = 0, cy = 0, startX = 0, startY = 0;  // current point and figure start, in path units
    for (const Command& c : def.commands) {
      switch (c.cmd) {
        case Cmd::Move:
          cx = startX = val(c.args[0]);
          cy = startY = val(c.args[1]);
          fig.path.MoveTo(pt(c.args[0], c.args[1]));
          break;
        case Cmd::Line:
          cx = val(c.args[0]);
          cy = val(c.args[1]);
          fig.path.LineTo(pt(c.args[0], c.args[1]));
          break;
        case Cmd::Quad:
          fig.path.QuadTo(pt(c.args[0], c.args[1]), pt(c.args[2], c.args[3]));
          cx = val(c.args[2]);
          cy = val(c.args[3]);
          break;
        case Cmd::Cubic:
          fig.path.CubicTo(pt(c.args[0], c.args[1]), pt(c.args[2], c.args[3]), pt(c.args[4], c.args[5]));
          cx = val(c.args[4]);
          cy = val(c.args[5]);
          break;
        case Cmd::Close:
          fig.path.Close();
          cx = startX;
          cy = startY;
          break;
        case Cmd::Arc: {
          // The current point is on the ellipse at the visual angle stAng; the arc sweeps swAng
          // (positive is clockwise). Emitted in pieces of at most 90° so each is unambiguous.
          const double wR = val(c.args[0]), hR = val(c.args[1]);
          double st = val(c.args[2]) * kAngle, sw = std::clamp(val(c.args[3]) * kAngle, -2 * kPi, 2 * kPi);
          auto param = [&](double angle) { return std::atan2(wR * std::sin(angle), hR * std::cos(angle)); };
          const double t0 = param(st);
          const double ox = cx - wR * std::cos(t0), oy = cy - hR * std::sin(t0);  // the center
          const int pieces = std::max(1, static_cast<int>(std::ceil(std::abs(sw) / (kPi / 2) - 1e-9)));
          for (int i = 1; i <= pieces; ++i) {
            const double t = param(st + sw * i / pieces);
            const double ex = ox + wR * std::cos(t), ey = oy + hR * std::sin(t);
            const Point end{ToPoints(ex * sx), ToPoints(ey * sy)};
            const float rx = ToPoints(std::abs(wR * sx)), ry = ToPoints(std::abs(hR * sy));
            if (rx < 1e-4f || ry < 1e-4f || sw == 0) fig.path.LineTo(end);
            else fig.path.ArcTo(end, rx, ry, 0, false, sw > 0);
            cx = ex;
            cy = ey;
          }
          break;
        }
      }
    }
    out.figures.push_back(std::move(fig));
  }
  if (hasRect_) {
    out.textRect = {ToPoints(val(rect_[0])), ToPoints(val(rect_[1])), ToPoints(val(rect_[2])), ToPoints(val(rect_[3]))};
  } else {
    out.textRect = {0, 0, ToPoints(w), ToPoints(h)};
  }
  return out;
}

}  // namespace draw
