#include "slide/show/Timing.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>

#include "pkg/Mce.h"
#include "pkg/XmlDom.h"

namespace fw::slide::show {
namespace {

using fw::pkg::FindAttribute;
using fw::pkg::IsElement;
using fw::pkg::LocalNameOf;
using fw::pkg::NamespaceOf;

constexpr std::string_view kP = "http://schemas.openxmlformats.org/presentationml/2006/main";
constexpr std::string_view kP14 = "http://schemas.microsoft.com/office/powerpoint/2010/main";
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr int kMaxDepth = 48;           // time nodes nested deeper are left out
constexpr size_t kMaxBehaviors = 20000;  // per slide
constexpr size_t kMaxPathPoints = 4096;

bool IsP(pugi::xml_node n, std::string_view local) { return IsElement(n, kP, local); }

pugi::xml_node ChildP(pugi::xml_node n, std::string_view local) {
  for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element && IsP(c, local)) return c;
  return {};
}

std::string_view Attr(pugi::xml_node n, std::string_view local) {
  // Attributes of PresentationML elements have no namespace.
  for (pugi::xml_attribute a = n.first_attribute(); a; a = a.next_attribute()) {
    std::string_view name = a.name();
    if (name == local) return a.value();
  }
  return {};
}

bool ParseDouble(std::string_view s, double& out) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '+')) s.remove_prefix(1);
  while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
  if (s.empty()) return false;
  auto r = std::from_chars(s.data(), s.data() + s.size(), out);
  return r.ec == std::errc() && r.ptr == s.data() + s.size() && std::isfinite(out);
}

double Number(std::string_view s, double fallback) {
  double v;
  return ParseDouble(s, v) ? v : fallback;
}

bool Truthy(std::string_view s) { return s == "1" || s == "true" || s == "on"; }

Side ParseSide(std::string_view s, Side fallback) {
  if (s == "l") return Side::Left;
  if (s == "u") return Side::Up;
  if (s == "r") return Side::Right;
  if (s == "d") return Side::Down;
  if (s == "lu") return Side::LeftUp;
  if (s == "ru") return Side::RightUp;
  if (s == "ld") return Side::LeftDown;
  if (s == "rd") return Side::RightDown;
  return fallback;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Transitions

Transition ParseTransition(pugi::xml_node node) {
  Transition t;
  if (IsElement(node, fw::pkg::mce::kNamespace, "AlternateContent")) {
    // p14 gives the duration in ms (p14:dur) and PowerPoint 2010's transitions, which play as
    // fades; p15 (prstTrans) and p159 (morph) aren't read, so their Fallback is used.
    static const fw::pkg::mce::Understood understood{kP, kP14};
    pugi::xml_node branch = fw::pkg::mce::SelectBranch(node, understood);
    node = branch ? ChildP(branch, "transition") : pugi::xml_node();
  }
  if (!IsP(node, "transition")) return t;

  std::string_view spd = Attr(node, "spd");
  double ms = spd == "slow" ? 1000 : spd == "med" ? 750 : 500;
  if (pugi::xml_attribute dur = FindAttribute(node, kP14, "dur")) ms = Number(dur.value(), ms);
  t.durationMs = std::clamp(ms, 0.0, 60000.0);
  std::string_view click = Attr(node, "advClick");
  t.advanceOnClick = click.empty() || Truthy(click);
  double after;
  if (ParseDouble(Attr(node, "advTm"), after) && after >= 0) t.advanceAfterMs = std::min(after, 86400000.0);

  for (pugi::xml_node c = node.first_child(); c; c = c.next_sibling()) {
    if (c.type() != pugi::node_element) continue;
    std::string_view ns = NamespaceOf(c), name = LocalNameOf(c);
    if (ns == kP && (name == "sndAc" || name == "extLst")) continue;
    if (ns != kP) {
      if (ns == kP14 || ns.empty()) {
        t.kind = TransitionKind::Fade;
        t.played = false;
      }
      continue;
    }
    if (name == "fade") {
      t.kind = TransitionKind::Fade;
      t.throughBlack = Truthy(Attr(c, "thruBlk"));
    } else if (name == "push") {
      t.kind = TransitionKind::Push;
      t.side = ParseSide(Attr(c, "dir"), Side::Left);
    } else if (name == "wipe") {
      t.kind = TransitionKind::Wipe;
      t.side = ParseSide(Attr(c, "dir"), Side::Left);
    } else if (name == "split") {
      t.kind = TransitionKind::Split;
      t.vertical = Attr(c, "orient") == "vert";
      t.in = Attr(c, "dir") == "in";
    } else if (name == "cover" || name == "pull") {
      t.kind = name == "cover" ? TransitionKind::Cover : TransitionKind::Uncover;
      t.side = ParseSide(Attr(c, "dir"), Side::Left);
    } else if (name == "zoom") {
      t.kind = TransitionKind::Zoom;
      t.in = Attr(c, "dir") == "in";
    } else if (name == "cut") {
      t.kind = TransitionKind::None;
    } else {
      t.kind = TransitionKind::Fade;
      t.played = false;
    }
    break;
  }
  if (t.kind == TransitionKind::None) t.durationMs = 0;
  return t;
}

// ---------------------------------------------------------------------------------------------
// Formulas

namespace {

// Recursive descent over the formula grammar of ECMA-376 §19.5.84 (tav fmla) and the values of
// <p:strVal>: numbers, + - * / ^ %, unary minus, parentheses, the variables (#)ppt_x/y/w/h, $, pi
// and e, and the functions abs acos asin atan ceil cos cosh deg exp floor ln max min rad rand sin
// sinh sqrt tan tanh.
class FormulaParser {
 public:
  FormulaParser(std::string_view s, const TargetBox& box, double value) : s_(s), box_(box), value_(value) {}

  double Parse() {
    double v = Sum();
    Space();
    return pos_ == s_.size() && !failed_ ? v : kNaN;
  }

 private:
  void Space() {
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t')) ++pos_;
  }
  bool Eat(char c) {
    Space();
    if (pos_ < s_.size() && s_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }
  double Fail() {
    failed_ = true;
    return kNaN;
  }

  double Sum() {
    if (++depth_ > 64) return Fail();
    double v = Product();
    for (;;) {
      if (Eat('+')) v += Product();
      else if (Eat('-')) v -= Product();
      else break;
    }
    --depth_;
    return v;
  }
  double Product() {
    double v = Power();
    for (;;) {
      if (Eat('*')) v *= Power();
      else if (Eat('/')) {
        double d = Power();
        v = d == 0 ? 0 : v / d;
      } else if (Eat('%')) {
        double d = Power();
        v = d == 0 ? 0 : std::fmod(v, d);
      } else break;
    }
    return v;
  }
  double Power() {
    double v = Unary();
    if (Eat('^')) v = std::pow(v, Power());
    return v;
  }
  double Unary() {
    if (Eat('-')) return -Unary();
    if (Eat('+')) return Unary();
    return Primary();
  }
  double Primary() {
    if (failed_ || ++depth_ > 64) return Fail();
    Space();
    double v = kNaN;
    if (Eat('(')) {
      v = Sum();
      if (!Eat(')')) return Fail();
    } else if (pos_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '.')) {
      size_t start = pos_;
      while (pos_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '.')) ++pos_;
      if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E') && pos_ + 1 < s_.size() &&
          (std::isdigit(static_cast<unsigned char>(s_[pos_ + 1])) || s_[pos_ + 1] == '-' || s_[pos_ + 1] == '+')) {
        pos_ += 2;
        while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
      }
      if (!ParseDouble(s_.substr(start, pos_ - start), v)) return Fail();
    } else if (Eat('$')) {
      v = value_;
    } else {
      bool hash = Eat('#');
      size_t start = pos_;
      while (pos_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '_')) ++pos_;
      std::string_view id = s_.substr(start, pos_ - start);
      if (id.empty()) return Fail();
      if (id == "ppt_x") v = box_.x;
      else if (id == "ppt_y") v = box_.y;
      else if (id == "ppt_w") v = box_.w;
      else if (id == "ppt_h") v = box_.h;
      else if (hash) return Fail();
      else if (id == "pi") v = 3.14159265358979323846;
      else if (id == "e") v = 2.71828182845904523536;
      else v = Function(id);
    }
    --depth_;
    return v;
  }
  double Function(std::string_view id) {
    if (!Eat('(')) return Fail();
    double a = Sum(), b = 0;
    bool two = Eat(',');
    if (two) b = Sum();
    if (!Eat(')')) return Fail();
    if (id == "max") return two ? std::max(a, b) : a;
    if (id == "min") return two ? std::min(a, b) : a;
    if (two) return Fail();
    if (id == "abs") return std::fabs(a);
    if (id == "acos") return std::acos(std::clamp(a, -1.0, 1.0));
    if (id == "asin") return std::asin(std::clamp(a, -1.0, 1.0));
    if (id == "atan") return std::atan(a);
    if (id == "ceil") return std::ceil(a);
    if (id == "cos") return std::cos(a);
    if (id == "cosh") return std::cosh(std::clamp(a, -700.0, 700.0));
    if (id == "deg") return a * 180 / 3.14159265358979323846;
    if (id == "exp") return std::exp(std::clamp(a, -700.0, 700.0));
    if (id == "floor") return std::floor(a);
    if (id == "ln") return a > 0 ? std::log(a) : 0;
    if (id == "rad") return a * 3.14159265358979323846 / 180;
    if (id == "rand") return a * 0.5;  // deterministic: the middle of [0, a)
    if (id == "sin") return std::sin(a);
    if (id == "sinh") return std::sinh(std::clamp(a, -700.0, 700.0));
    if (id == "sqrt") return a > 0 ? std::sqrt(a) : 0;
    if (id == "tan") return std::tan(a);
    if (id == "tanh") return std::tanh(a);
    return Fail();
  }

  std::string_view s_;
  const TargetBox& box_;
  double value_;
  size_t pos_ = 0;
  int depth_ = 0;
  bool failed_ = false;
};

}  // namespace

double EvalFormula(std::string_view text, const TargetBox& box, double value) {
  if (text.empty() || text.size() > 4096) return kNaN;
  double v = FormulaParser(text, box, value).Parse();
  return std::isfinite(v) ? v : kNaN;
}

// ---------------------------------------------------------------------------------------------
// The timing tree

bool TargetKey::operator<(const TargetKey& o) const {
  if (spid != o.spid) return spid < o.spid;
  if (part != o.part) return part < o.part;
  if (part != Paragraphs) return false;
  if (first != o.first) return first < o.first;
  return last < o.last;
}

double Behavior::End() const {
  return begin + duration * std::max(0.0, repeat) * (autoReverse ? 2 : 1);
}

namespace {

// Which preset effects play as PowerPoint plays them; the others play as appear (entrance and exit)
// or not at all (emphasis), as plan §8.2 says. Preset ids as PowerPoint writes them (presetID).
bool PresetPlayed(EffectClass cls, int id) {
  switch (cls) {
    case EffectClass::Entrance:
    case EffectClass::Exit:
      // appear, fly, fade, split, wipe, zoom (basic), float (ascend/descend), zoom (faded)
      return id == 0 || id == 1 || id == 2 || id == 10 || id == 16 || id == 22 || id == 23 || id == 42 ||
             id == 47 || id == 53;
    case EffectClass::Emphasis:
      return id == 0 || id == 6 || id == 8;  // grow/shrink, spin
    case EffectClass::Path:
      return true;  // simplified (plan §8.2)
    default:
      return false;
  }
}

EffectClass ParseClass(std::string_view s) {
  if (s == "entr") return EffectClass::Entrance;
  if (s == "exit") return EffectClass::Exit;
  if (s == "emph") return EffectClass::Emphasis;
  if (s == "path") return EffectClass::Path;
  if (s == "verb") return EffectClass::Verb;
  if (s == "mediacall") return EffectClass::Media;
  return EffectClass::None;
}

Behavior::Attr ParseAttr(std::string_view s) {
  if (s == "style.visibility") return Behavior::Visibility;
  if (s == "style.opacity") return Behavior::Opacity;
  if (s == "ppt_x") return Behavior::X;
  if (s == "ppt_y") return Behavior::Y;
  if (s == "ppt_w") return Behavior::W;
  if (s == "ppt_h") return Behavior::H;
  if (s == "r" || s == "style.rotation" || s == "ppt_r") return Behavior::Rotation;
  return Behavior::Other;
}

// The delay of a node's start conditions: the first numeric delay of a condition with no event,
// or one that fires when the parent starts. -1 when it waits for a click ("indefinite").
double StartDelay(pugi::xml_node cTn, bool* indefinite = nullptr) {
  bool waits = false;
  double delay = 0;
  bool found = false;
  if (pugi::xml_node lst = ChildP(cTn, "stCondLst")) {
    for (pugi::xml_node c = lst.first_child(); c; c = c.next_sibling()) {
      if (!IsP(c, "cond")) continue;
      std::string_view d = Attr(c, "delay"), evt = Attr(c, "evt");
      if (d == "indefinite") {
        waits = true;
        continue;
      }
      if (!evt.empty() && evt != "onBegin" && evt != "begin") continue;
      double v;
      if (!found && ParseDouble(d.empty() ? "0" : d, v)) {
        delay = std::clamp(v, 0.0, 3600000.0);
        found = true;
      }
    }
  }
  if (indefinite) *indefinite = waits && !found;
  return delay;
}

// A cTn's simple duration in ms; 0 for "indefinite" or none.
double Duration(pugi::xml_node cTn) {
  double v;
  return ParseDouble(Attr(cTn, "dur"), v) ? std::clamp(v, 0.0, 3600000.0) : 0;
}

bool ParseTarget(pugi::xml_node cBhvr, TargetKey& key) {
  pugi::xml_node tgt = ChildP(cBhvr, "tgtEl");
  if (!tgt) return false;
  pugi::xml_node sp = ChildP(tgt, "spTgt");
  if (!sp) sp = ChildP(tgt, "inkTgt");
  if (!sp) return false;  // the slide or a sound
  double id;
  if (!ParseDouble(Attr(sp, "spid"), id) || id < 0 || id > 4294967295.0) return false;
  key = {};
  key.spid = static_cast<uint32_t>(id);
  if (ChildP(sp, "bg")) {
    key.part = TargetKey::Background;
  } else if (pugi::xml_node tx = ChildP(sp, "txEl")) {
    if (pugi::xml_node rg = ChildP(tx, "pRg")) {
      double st = Number(Attr(rg, "st"), 0), en = Number(Attr(rg, "end"), st);
      key.part = TargetKey::Paragraphs;
      key.first = static_cast<int>(std::clamp(st, 0.0, 1e6));
      key.last = std::max(key.first, static_cast<int>(std::clamp(en, 0.0, 1e6)));
    }
  }
  return true;
}

void ParseValue(pugi::xml_node val, std::string& out) {
  for (pugi::xml_node c = val.first_child(); c; c = c.next_sibling()) {
    if (IsP(c, "strVal") || IsP(c, "fltVal") || IsP(c, "intVal") || IsP(c, "boolVal")) {
      out = std::string(Attr(c, "val"));
      return;
    }
  }
}

// x and y of a <p:by>, <p:from> or <p:to> point (in 1000ths of a percent for animScale).
bool ParsePoint(pugi::xml_node n, double out[2], double scale) {
  if (!n) return false;
  out[0] = Number(Attr(n, "x"), 0) / scale;
  out[1] = Number(Attr(n, "y"), 0) / scale;
  return true;
}

// A motion path ("M 0 0 L 0.25 0.1 C ... E"), flattened into points relative to its start.
void ParsePath(std::string_view s, std::vector<std::pair<float, float>>& out) {
  out.clear();
  double cx = 0, cy = 0, sx = 0, sy = 0;
  char cmd = 'M';
  size_t i = 0;
  auto digit = [&](size_t k) { return k < s.size() && std::isdigit(static_cast<unsigned char>(s[k])); };
  auto skip = [&] {
    while (i < s.size() && (s[i] == ' ' || s[i] == ',' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
  };
  auto number = [&](double& v) {
    skip();
    size_t start = i;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
    while (digit(i) || (i < s.size() && s[i] == '.')) ++i;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E') &&
        (digit(i + 1) || (i + 1 < s.size() && (s[i + 1] == '-' || s[i + 1] == '+') && digit(i + 2)))) {
      i += 2;
      while (digit(i)) ++i;
    }
    return ParseDouble(s.substr(start, i - start), v) && std::fabs(v) < 1e4;
  };
  auto add = [&](double x, double y) {
    if (out.size() < kMaxPathPoints) out.emplace_back(static_cast<float>(x), static_cast<float>(y));
  };
  while (out.size() < kMaxPathPoints) {
    skip();
    if (i >= s.size()) break;
    char c = s[i];
    if (std::isalpha(static_cast<unsigned char>(c))) {
      ++i;
      if (c == 'E' || c == 'e') break;  // the path's end
      if (c == 'Z' || c == 'z') {
        cx = sx;
        cy = sy;
        add(cx, cy);
      } else {
        cmd = c;
      }
      continue;
    }
    bool rel = cmd >= 'a' && cmd <= 'z';
    char up = static_cast<char>(rel ? cmd - ('a' - 'A') : cmd);
    // A path that doesn't start with M starts where the target is.
    if (out.empty() && up != 'M') add(0, 0);
    if (up == 'M' || up == 'L') {
      double x, y;
      if (!number(x) || !number(y)) break;
      if (rel) x += cx, y += cy;
      if (up == 'M') sx = x, sy = y;
      cx = x;
      cy = y;
      add(x, y);
    } else if (up == 'C') {
      double p[6];
      bool ok = true;
      for (double& v : p) ok = ok && number(v);
      if (!ok) break;
      if (rel)
        for (int k = 0; k < 6; k += 2) p[k] += cx, p[k + 1] += cy;
      for (int k = 1; k <= 12; ++k) {
        double t = k / 12.0, u = 1 - t;
        add(u * u * u * cx + 3 * u * u * t * p[0] + 3 * u * t * t * p[2] + t * t * t * p[4],
            u * u * u * cy + 3 * u * u * t * p[1] + 3 * u * t * t * p[3] + t * t * t * p[5]);
      }
      cx = p[4];
      cy = p[5];
    } else {
      break;
    }
  }
}

struct Context {
  double begin = 0;  // ms after the step starts
  EffectClass cls = EffectClass::None;
  bool played = true;
  double repeat = 1;
  bool autoReverse = false;
  double accel = 0, decel = 0;  // the effect's own (motion paths' smooth start and end)
};

class Compiler {
 public:
  explicit Compiler(Timing& t) : t_(t) {}

  void Sequence(pugi::xml_node seq, Sequence& out) {
    pugi::xml_node cTn = ChildP(seq, "cTn");
    pugi::xml_node children = ChildP(cTn, "childTnLst");
    for (pugi::xml_node c = children.first_child(); c; c = c.next_sibling()) {
      if (!IsP(c, "par") && !IsP(c, "seq")) continue;
      pugi::xml_node group = ChildP(c, "cTn");
      bool clicks = false;
      double delay = StartDelay(group, &clicks);
      Step step;
      if (out.steps.empty() && !clicks) step.start = StepStart::WithPrevious;
      else if (!clicks) step.start = StepStart::AfterPrevious;
      step_ = &step;
      Context ctx;
      ctx.begin = clicks ? 0 : delay;
      Node(c, ctx, 0);
      for (const Behavior& b : step.behaviors) step.length = std::max(step.length, b.End());
      out.steps.push_back(std::move(step));
    }
  }

 private:
  void Node(pugi::xml_node node, Context ctx, int depth) {
    if (depth > kMaxDepth || !step_ || behaviors_ >= kMaxBehaviors) return;
    std::string_view name = LocalNameOf(node);
    if (NamespaceOf(node) != kP) return;
    if (name == "par" || name == "seq" || name == "excl") {
      pugi::xml_node cTn = ChildP(node, "cTn");
      if (!cTn) return;
      if (std::string_view cls = Attr(cTn, "presetClass"); !cls.empty()) {
        ctx.cls = ParseClass(cls);
        ctx.played = PresetPlayed(ctx.cls, static_cast<int>(Number(Attr(cTn, "presetID"), 0)));
        if (!ctx.played) ++t_.unsupportedEffects;
        Repeat(cTn, ctx);
      }
      bool waits = false;
      double delay = StartDelay(cTn, &waits);
      if (depth > 0) ctx.begin += delay;
      pugi::xml_node children = ChildP(cTn, "childTnLst");
      double at = ctx.begin;
      for (pugi::xml_node c = children.first_child(); c; c = c.next_sibling()) {
        if (c.type() != pugi::node_element) continue;
        size_t before = step_->behaviors.size();
        Context child = ctx;
        if (name == "seq") child.begin = at;
        Node(c, child, depth + 1);
        if (name == "seq")
          for (size_t i = before; i < step_->behaviors.size(); ++i) at = std::max(at, step_->behaviors[i].End());
      }
      return;
    }
    Behavior b;
    if (name == "set") b.kind = Behavior::Set;
    else if (name == "anim") b.kind = Behavior::Anim;
    else if (name == "animEffect") b.kind = Behavior::Effect;
    else if (name == "animScale") b.kind = Behavior::Scale;
    else if (name == "animRot") b.kind = Behavior::Rotate;
    else if (name == "animMotion") b.kind = Behavior::Motion;
    else return;  // audio, video, cmd, animClr: P3 or not played
    pugi::xml_node cBhvr = ChildP(node, "cBhvr");
    pugi::xml_node cTn = ChildP(cBhvr, "cTn");
    if (!cBhvr || !cTn || !ParseTarget(cBhvr, b.target)) return;
    b.effectClass = ctx.cls;
    if (pugi::xml_node names = ChildP(cBhvr, "attrNameLst"))
      if (pugi::xml_node first = ChildP(names, "attrName")) b.attr = ParseAttr(first.text().get());
    // An effect that isn't played as PowerPoint plays it appears (or disappears): only its
    // visibility is kept. Media and verbs aren't played at all.
    if (!ctx.played && !(b.kind == Behavior::Set && b.attr == Behavior::Visibility)) return;
    if (ctx.cls == EffectClass::Media || ctx.cls == EffectClass::Verb) return;
    b.begin = ctx.begin + StartDelay(cTn);
    b.duration = Duration(cTn);
    std::string_view rc = Attr(cTn, "repeatCount");
    b.repeat = rc.empty() || rc == "indefinite" ? 1 : std::clamp(Number(rc, 1000) / 1000, 0.0, 1000.0);
    b.repeat *= ctx.repeat;
    b.autoReverse = Truthy(Attr(cTn, "autoRev")) || ctx.autoReverse;
    // A behavior without its own easing takes its effect's: the behaviors of one effect run
    // together, so easing each is easing the effect.
    std::string_view accel = Attr(cTn, "accel"), decel = Attr(cTn, "decel");
    b.accel = std::clamp((accel.empty() ? ctx.accel : Number(accel, 0)) / 100000, 0.0, 1.0);
    b.decel = std::clamp((decel.empty() ? ctx.decel : Number(decel, 0)) / 100000, 0.0, 1.0);
    if (b.accel + b.decel > 1) {
      double s = b.accel + b.decel;
      b.accel /= s;
      b.decel /= s;
    }
    std::string_view fill = Attr(cTn, "fill");
    b.hold = fill != "remove";
    b.additive = Attr(cBhvr, "additive") == "sum";

    switch (b.kind) {
      case Behavior::Set: {
        std::string v;
        if (pugi::xml_node to = ChildP(node, "to")) ParseValue(to, v);
        b.keys.push_back({0, std::move(v), {}});
        b.discrete = true;
        break;
      }
      case Behavior::Anim: {
        std::string_view calc = Attr(node, "calcmode");
        b.discrete = calc == "discrete" || Attr(node, "valueType") == "str";
        if (pugi::xml_node tavs = ChildP(node, "tavLst")) {
          int count = 0;
          for (pugi::xml_node tav = tavs.first_child(); tav; tav = tav.next_sibling())
            if (IsP(tav, "tav")) ++count;
          int index = 0;
          for (pugi::xml_node tav = tavs.first_child(); tav && b.keys.size() < 256; tav = tav.next_sibling()) {
            if (!IsP(tav, "tav")) continue;
            Behavior::Key k;
            double tm;
            k.time = ParseDouble(Attr(tav, "tm"), tm) ? tm / 100000
                                                       : (count > 1 ? static_cast<double>(index) / (count - 1) : 0);
            k.time = std::clamp(k.time, 0.0, 1.0);
            k.formula = std::string(Attr(tav, "fmla"));
            if (pugi::xml_node val = ChildP(tav, "val")) ParseValue(val, k.value);
            b.keys.push_back(std::move(k));
            ++index;
          }
          std::stable_sort(b.keys.begin(), b.keys.end(),
                           [](const Behavior::Key& a, const Behavior::Key& c) { return a.time < c.time; });
        }
        if (b.keys.empty()) {
          std::string_view from = Attr(node, "from"), to = Attr(node, "to"), by = Attr(node, "by");
          if (!to.empty()) {
            b.keys.push_back({0, from.empty() ? std::string() : std::string(from), {}});
            b.keys.push_back({1, std::string(to), {}});
          } else if (!by.empty()) {
            b.additive = true;
            b.keys.push_back({0, "0", {}});
            b.keys.push_back({1, std::string(by), {}});
          } else {
            return;
          }
        }
        break;
      }
      case Behavior::Effect: {
        b.effectOut = Attr(node, "transition") == "out";
        std::string_view filter = Attr(node, "filter");
        std::string_view kind = filter.substr(0, filter.find('('));
        std::string_view arg;
        if (size_t open = filter.find('('); open != std::string_view::npos) {
          arg = filter.substr(open + 1);
          arg = arg.substr(0, arg.find(')'));
        }
        if (kind == "fade") {
          b.fade = true;
        } else if (kind == "wipe") {
          b.reveal = Reveal::Wipe;
          b.side = arg == "up" ? Side::Up : arg == "left" ? Side::Left : arg == "right" ? Side::Right : Side::Down;
        } else if (kind == "barn") {
          b.reveal = Reveal::Split;
          b.vertical = arg.find("Vertical") != std::string_view::npos;
          b.outward = arg.substr(0, 2) == "ou";
        } else {
          return;  // played as appear: the set beside it shows the shape
        }
        break;
      }
      case Behavior::Scale: {
        b.hasBy = ParsePoint(ChildP(node, "by"), b.by, 100000);
        b.hasFrom = ParsePoint(ChildP(node, "from"), b.from, 100000);
        b.hasTo = ParsePoint(ChildP(node, "to"), b.to, 100000);
        if (!b.hasBy && !b.hasTo) return;
        break;
      }
      case Behavior::Rotate: {
        double v;
        b.hasBy = ParseDouble(Attr(node, "by"), v);
        if (b.hasBy) b.by[0] = v / 60000;
        b.hasFrom = ParseDouble(Attr(node, "from"), v);
        if (b.hasFrom) b.from[0] = v / 60000;
        b.hasTo = ParseDouble(Attr(node, "to"), v);
        if (b.hasTo) b.to[0] = v / 60000;
        if (!b.hasBy && !b.hasTo) return;
        b.attr = Behavior::Rotation;
        break;
      }
      case Behavior::Motion: {
        ParsePath(Attr(node, "path"), b.path);
        if (b.path.size() < 2) {
          double by[2];
          if (!ParsePoint(ChildP(node, "by"), by, 1) && !ParsePoint(ChildP(node, "to"), by, 1)) return;
          b.path = {{0.f, 0.f}, {static_cast<float>(by[0]), static_cast<float>(by[1])}};
        }
        break;
      }
    }
    ++behaviors_;
    step_->behaviors.push_back(std::move(b));
  }

  void Repeat(pugi::xml_node cTn, Context& ctx) {
    std::string_view rc = Attr(cTn, "repeatCount");
    if (!rc.empty() && rc != "indefinite") ctx.repeat = std::clamp(Number(rc, 1000) / 1000, 0.0, 1000.0);
    ctx.autoReverse = Truthy(Attr(cTn, "autoRev"));
    ctx.accel = Number(Attr(cTn, "accel"), 0);
    ctx.decel = Number(Attr(cTn, "decel"), 0);
  }

  Timing& t_;
  Step* step_ = nullptr;
  size_t behaviors_ = 0;
};

bool Overlaps(const TargetKey& a, const TargetKey& b) {
  if (a.spid != b.spid) return false;
  if (a.part != TargetKey::Paragraphs || b.part != TargetKey::Paragraphs) return a.part == b.part;
  return a.first <= b.last && b.first <= a.last;
}

void AddUnique(std::vector<TargetKey>& v, const TargetKey& k) {
  if (std::find(v.begin(), v.end(), k) == v.end()) v.push_back(k);
}

}  // namespace

Timing ParseTiming(pugi::xml_node timing) {
  Timing t;
  if (!IsP(timing, "timing")) return t;
  Compiler compiler(t);
  pugi::xml_node root = ChildP(ChildP(timing, "tnLst"), "par");
  pugi::xml_node rootTn = ChildP(root, "cTn");
  bool haveMain = false;
  for (pugi::xml_node c = ChildP(rootTn, "childTnLst").first_child(); c; c = c.next_sibling()) {
    if (!IsP(c, "seq")) continue;
    pugi::xml_node cTn = ChildP(c, "cTn");
    std::string_view type = Attr(cTn, "nodeType");
    if (type == "mainSeq" || (type.empty() && !haveMain)) {
      if (haveMain) continue;
      haveMain = true;
      compiler.Sequence(c, t.main);
    } else if (type == "interactiveSeq") {
      Sequence s;
      if (pugi::xml_node lst = ChildP(cTn, "stCondLst"))
        for (pugi::xml_node cond = lst.first_child(); cond && !s.triggerSpid; cond = cond.next_sibling())
          if (pugi::xml_node sp = ChildP(ChildP(cond, "tgtEl"), "spTgt"))
            s.triggerSpid = static_cast<uint32_t>(std::clamp(Number(Attr(sp, "spid"), 0), 0.0, 4294967295.0));
      if (!s.triggerSpid || t.triggers.size() >= 256) continue;
      compiler.Sequence(c, s);
      t.triggers.push_back(std::move(s));
    }
  }

  // Every target, and those whose first effect is an entrance: they wait hidden.
  std::vector<TargetKey> seen;
  auto visit = [&](const Sequence& s) {
    for (const Step& step : s.steps) {
      for (const Behavior& b : step.behaviors) {
        AddUnique(t.targets, b.target);
        bool first = std::none_of(seen.begin(), seen.end(), [&](const TargetKey& k) { return Overlaps(k, b.target); });
        if (first && b.effectClass == EffectClass::Entrance) AddUnique(t.initiallyHidden, b.target);
        AddUnique(seen, b.target);
        if (b.target.part == TargetKey::Paragraphs &&
            std::find(t.byParagraph.begin(), t.byParagraph.end(), b.target.spid) == t.byParagraph.end())
          t.byParagraph.push_back(b.target.spid);
      }
    }
  };
  visit(t.main);
  for (const Sequence& s : t.triggers) visit(s);
  if (pugi::xml_node bld = ChildP(timing, "bldLst")) {
    for (pugi::xml_node p = bld.first_child(); p; p = p.next_sibling()) {
      if (!IsP(p, "bldP")) continue;
      std::string_view build = Attr(p, "build");
      double id;
      if ((build == "p" || build == "cust") && ParseDouble(Attr(p, "spid"), id) && id >= 0 && id <= 4294967295.0) {
        uint32_t spid = static_cast<uint32_t>(id);
        if (std::find(t.byParagraph.begin(), t.byParagraph.end(), spid) == t.byParagraph.end())
          t.byParagraph.push_back(spid);
      }
    }
  }
  std::sort(t.targets.begin(), t.targets.end());
  std::sort(t.byParagraph.begin(), t.byParagraph.end());
  return t;
}

// ---------------------------------------------------------------------------------------------
// The animator

namespace {

// SMIL's acceleration and deceleration (SMIL 3.0 §12.7.3), on a progress in [0, 1].
double Ease(double t, double a, double d) {
  if (a <= 0 && d <= 0) return t;
  double r = 1 / (1 - a / 2 - d / 2);
  if (t < a) return r * t * t / (2 * a);
  if (t <= 1 - d) return r * (t - a / 2);
  double td = 1 - t;
  return 1 - r * td * td / (2 * d);
}

double ValueOf(const Behavior::Key& k, const TargetBox& box) {
  double v = EvalFormula(k.value, box);
  if (!k.formula.empty() && !std::isnan(v)) v = EvalFormula(k.formula, box, v);
  return v;
}

std::pair<double, double> PathAt(const std::vector<std::pair<float, float>>& path, double p) {
  if (path.empty()) return {0, 0};
  double total = 0;
  for (size_t i = 1; i < path.size(); ++i)
    total += std::hypot(path[i].first - path[i - 1].first, path[i].second - path[i - 1].second);
  if (total <= 0) return {path.back().first, path.back().second};
  double want = std::clamp(p, 0.0, 1.0) * total;
  for (size_t i = 1; i < path.size(); ++i) {
    double len = std::hypot(path[i].first - path[i - 1].first, path[i].second - path[i - 1].second);
    if (want <= len && len > 0) {
      double f = want / len;
      return {path[i - 1].first + (path[i].first - path[i - 1].first) * f,
              path[i - 1].second + (path[i].second - path[i - 1].second) * f};
    }
    want -= len;
  }
  return {path.back().first, path.back().second};
}

}  // namespace

Pose* Animator::Find(const TargetKey& key) {
  for (auto& [k, p] : poses_)
    if (k == key) return &p;
  poses_.emplace_back(key, Pose{});
  return &poses_.back().second;
}

void Animator::Apply(const Behavior& b, double local, bool settle) {
  double total = b.End() - b.begin;
  double p;
  bool ended = settle || local >= total;
  if (!settle && local < 0) return;
  if (ended) {
    if (!b.hold) return;
    // The end of the last iteration: back at the start after an auto-reverse.
    double iterations = std::max(0.0, b.repeat);
    double frac = iterations - std::floor(iterations);
    p = b.autoReverse ? (frac > 0 ? 1 - frac : 0) : (frac > 0 ? frac : 1);
  } else if (b.duration <= 0) {
    p = 1;
  } else {
    double span = b.duration * (b.autoReverse ? 2 : 1);
    double t = std::fmod(local, span);
    if (b.autoReverse && t > b.duration) t = span - t;
    p = t / b.duration;
  }
  p = Ease(std::clamp(p, 0.0, 1.0), b.accel, b.decel);
  TargetBox box = box_ ? box_(ctx_, b.target) : TargetBox{};
  Pose& pose = *Find(b.target);

  switch (b.kind) {
    case Behavior::Set:
    case Behavior::Anim: {
      if (b.keys.empty()) return;
      if (b.attr == Behavior::Visibility) {
        const Behavior::Key* k = &b.keys.front();
        for (const auto& key : b.keys)
          if (key.time <= p) k = &key;
        pose.visible = k->value != "hidden";
        return;
      }
      double v;
      if (b.discrete || b.keys.size() == 1) {
        const Behavior::Key* k = &b.keys.front();
        for (const auto& key : b.keys)
          if (key.time <= p) k = &key;
        v = ValueOf(*k, box);
      } else {
        size_t i = 1;
        while (i + 1 < b.keys.size() && b.keys[i].time < p) ++i;
        const auto& a = b.keys[i - 1];
        const auto& c = b.keys[i];
        double va = a.value.empty() ? kNaN : ValueOf(a, box), vc = ValueOf(c, box);
        if (std::isnan(va)) {  // no "from": the target's own value
          switch (b.attr) {
            case Behavior::X: va = box.x + pose.dx; break;
            case Behavior::Y: va = box.y + pose.dy; break;
            case Behavior::W: va = box.w * pose.sx; break;
            case Behavior::H: va = box.h * pose.sy; break;
            case Behavior::Rotation: va = pose.rotation; break;
            case Behavior::Opacity: va = pose.opacity; break;
            default: va = 0;
          }
        }
        if (std::isnan(vc)) vc = va;
        double span = c.time - a.time;
        double f = span > 0 ? std::clamp((p - a.time) / span, 0.0, 1.0) : 1;
        v = va + (vc - va) * f;
      }
      if (std::isnan(v)) return;
      float fv = static_cast<float>(std::clamp(v, -1e4, 1e4));
      switch (b.attr) {
        case Behavior::X: pose.dx = b.additive ? pose.dx + fv : fv - static_cast<float>(box.x); break;
        case Behavior::Y: pose.dy = b.additive ? pose.dy + fv : fv - static_cast<float>(box.y); break;
        case Behavior::W:
          if (box.w > 0) pose.sx = b.additive ? pose.sx + fv / static_cast<float>(box.w) : fv / static_cast<float>(box.w);
          break;
        case Behavior::H:
          if (box.h > 0) pose.sy = b.additive ? pose.sy + fv / static_cast<float>(box.h) : fv / static_cast<float>(box.h);
          break;
        case Behavior::Rotation: pose.rotation = b.additive ? pose.rotation + fv : fv; break;
        case Behavior::Opacity: pose.opacity = std::clamp(fv, 0.f, 1.f); break;
        default: break;
      }
      return;
    }
    case Behavior::Effect: {
      // A filter only shows while it runs: after an entrance the whole target shows, and after an
      // exit the set beside it hides the target.
      if (ended) return;
      float shown = static_cast<float>(b.effectOut ? 1 - p : p);
      if (b.fade) pose.opacity *= shown;
      if (b.reveal != Reveal::None) {
        pose.reveal.kind = b.reveal;
        pose.reveal.side = b.side;
        pose.reveal.vertical = b.vertical;
        pose.reveal.outward = b.outward;
        pose.reveal.shown = shown;
      }
      return;
    }
    case Behavior::Scale: {
      for (int i = 0; i < 2; ++i) {
        float& s = i == 0 ? pose.sx : pose.sy;
        if (b.hasBy) s *= static_cast<float>(1 + (b.by[i] - 1) * p);
        else s = static_cast<float>((b.hasFrom ? b.from[i] : s) + (b.to[i] - (b.hasFrom ? b.from[i] : s)) * p);
        s = std::clamp(s, -1000.f, 1000.f);
      }
      return;
    }
    case Behavior::Rotate: {
      if (b.hasBy) pose.rotation += static_cast<float>(b.by[0] * p);
      else pose.rotation = static_cast<float>((b.hasFrom ? b.from[0] : pose.rotation) +
                                              (b.to[0] - (b.hasFrom ? b.from[0] : pose.rotation)) * p);
      pose.rotation = std::fmod(pose.rotation, 360000.f);
      return;
    }
    case Behavior::Motion: {
      auto [x, y] = PathAt(b.path, p);
      pose.dx += static_cast<float>(x);
      pose.dy += static_cast<float>(y);
      return;
    }
  }
}

void Animator::Settle(const Step& step) {
  std::swap(poses_, settled_);
  for (const Behavior& b : step.behaviors) Apply(b, 0, true);
  std::swap(poses_, settled_);
}

void Animator::StartStep(const Step* step, double now) { runs_.push_back({step, now}); }

void Animator::Start(double now, bool begin) {
  settled_.clear();
  history_.clear();
  historyNext_.clear();
  runs_.clear();
  mainNext_ = 0;
  triggerNext_.assign(timing_ ? timing_->triggers.size() : 0, 0);
  if (timing_) {
    for (const TargetKey& k : timing_->initiallyHidden) {
      Pose p;
      p.visible = false;
      settled_.emplace_back(k, p);
    }
  }
  if (begin) BeginAuto(now);
  else Update(now);
}

void Animator::BeginAuto(double now) {
  if (timing_ && mainNext_ == 0 && !timing_->main.steps.empty() && timing_->main.steps[0].start != StepStart::Click)
    StartStep(&timing_->main.steps[mainNext_++], now);
  Update(now);
}

void Animator::Finish() {
  // Steps that would start after these run at once too: the whole chain ends.
  for (;;) {
    for (const Run& r : runs_) Settle(*r.step);
    bool chained = !runs_.empty() && timing_ && mainNext_ < static_cast<int>(timing_->main.steps.size()) &&
                   timing_->main.steps[mainNext_].start != StepStart::Click &&
                   std::any_of(runs_.begin(), runs_.end(), [&](const Run& r) {
                     return r.step >= timing_->main.steps.data() &&
                            r.step < timing_->main.steps.data() + timing_->main.steps.size();
                   });
    runs_.clear();
    if (!chained) break;
    runs_.push_back({&timing_->main.steps[mainNext_++], 0});
  }
  poses_ = settled_;
  moving_.clear();
}

bool Animator::Click(double now) {
  if (!timing_) return false;
  if (!runs_.empty()) Finish();
  if (mainNext_ >= static_cast<int>(timing_->main.steps.size())) return false;
  history_.push_back(settled_);
  historyNext_.push_back(mainNext_);
  StartStep(&timing_->main.steps[mainNext_++], now);
  Update(now);
  return true;
}

bool Animator::Trigger(uint32_t spid, double now) {
  if (!timing_ || !spid) return false;
  for (size_t i = 0; i < timing_->triggers.size(); ++i) {
    const Sequence& s = timing_->triggers[i];
    if (s.triggerSpid != spid) continue;
    if (triggerNext_[i] >= static_cast<int>(s.steps.size())) return false;
    StartStep(&s.steps[triggerNext_[i]++], now);
    Update(now);
    return true;
  }
  return false;
}

bool Animator::Back() {
  if (history_.empty()) return false;
  runs_.clear();
  settled_ = std::move(history_.back());
  mainNext_ = historyNext_.back();
  history_.pop_back();
  historyNext_.pop_back();
  poses_ = settled_;
  moving_.clear();
  return true;
}

void Animator::FinishAll(double now) {
  Start(now);
  Finish();
  if (!timing_) return;
  while (mainNext_ < static_cast<int>(timing_->main.steps.size())) {
    history_.push_back(settled_);
    historyNext_.push_back(mainNext_);
    runs_.push_back({&timing_->main.steps[mainNext_++], now});
    Finish();
  }
}

void Animator::Update(double now) {
  if (!timing_) return;
  // Steps that end start the ones chained after them, at their end.
  for (bool again = true; again;) {
    again = false;
    for (size_t i = 0; i < runs_.size(); ++i) {
      Run r = runs_[i];
      if (now < r.start + r.step->length) continue;
      Settle(*r.step);
      runs_.erase(runs_.begin() + static_cast<std::ptrdiff_t>(i));
      const auto& steps = timing_->main.steps;
      bool isMain = r.step >= steps.data() && r.step < steps.data() + steps.size();
      if (isMain && mainNext_ < static_cast<int>(steps.size()) && steps[mainNext_].start != StepStart::Click)
        runs_.push_back({&steps[mainNext_++], r.start + r.step->length});
      again = true;
      break;
    }
  }
  Recompute(now);
}

void Animator::Recompute(double now) {
  poses_ = settled_;
  moving_.clear();
  for (const Run& r : runs_) {
    for (const Behavior& b : r.step->behaviors) {
      Apply(b, now - r.start - b.begin, false);
      if (std::find(moving_.begin(), moving_.end(), b.target) == moving_.end()) moving_.push_back(b.target);
    }
  }
}

bool Animator::Running(double /*now*/) const { return !runs_.empty(); }

bool Animator::HasNextStep() const {
  return timing_ && mainNext_ < static_cast<int>(timing_->main.steps.size());
}

Pose Animator::PoseOf(const TargetKey& key) const {
  for (const auto& [k, p] : poses_)
    if (k == key) return p;
  return {};
}

// ---------------------------------------------------------------------------------------------
// SlideOrder

SlideOrder::SlideOrder(size_t count, std::function<bool(size_t)> hidden, bool loop)
    : count_(count), hidden_(std::move(hidden)), loop_(loop) {}

size_t SlideOrder::First() const {
  if (inCustom_) return custom_.front();
  for (size_t i = 0; i < count_; ++i)
    if (!Hidden(i)) return i;
  return kEnd;
}

size_t SlideOrder::Next(size_t from) const {
  if (from == kEnd) return kEnd;
  if (inCustom_) {
    if (pos_ + 1 < custom_.size()) return custom_[pos_ + 1];
    return customLoop_ ? custom_.front() : return_;
  }
  for (size_t i = from + 1; i < count_; ++i)
    if (!Hidden(i)) return i;
  if (loop_)
    for (size_t i = 0; i <= from && i < count_; ++i)
      if (!Hidden(i)) return i;
  return kEnd;
}

size_t SlideOrder::Prev(size_t from) const {
  if (inCustom_) {
    if (from == kEnd) return custom_.back();
    return pos_ > 0 ? custom_[pos_ - 1] : return_;
  }
  size_t i = from == kEnd || from > count_ ? count_ : from;
  while (i > 0)
    if (!Hidden(--i)) return i;
  return kEnd;
}

bool SlideOrder::StartCustom(const std::vector<size_t>& slides, size_t returnTo, bool loop) {
  if (slides.empty()) return false;
  for (size_t i : slides)
    if (i >= count_) return false;
  custom_ = slides;
  inCustom_ = true;
  pos_ = 0;
  return_ = returnTo < count_ ? returnTo : kEnd;
  customLoop_ = loop;
  return true;
}

size_t SlideOrder::Advance(size_t from) {
  size_t next = Next(from);
  if (inCustom_ && from != kEnd && !customLoop_ && pos_ + 1 >= custom_.size()) inCustom_ = false;
  return next;
}

size_t SlideOrder::Back(size_t from) {
  size_t prev = Prev(from);
  if (inCustom_ && from != kEnd && pos_ == 0 && prev != kEnd) inCustom_ = false;
  return prev;
}

void SlideOrder::Moved(size_t index) {
  if (!inCustom_) return;
  if (custom_[pos_] == index) return;
  if (pos_ + 1 < custom_.size() && custom_[pos_ + 1] == index) {
    ++pos_;
    return;
  }
  if (customLoop_ && pos_ + 1 == custom_.size() && custom_.front() == index) {
    pos_ = 0;
    return;
  }
  if (pos_ > 0 && custom_[pos_ - 1] == index) {
    --pos_;
    return;
  }
  auto it = std::find(custom_.begin(), custom_.end(), index);
  if (it != custom_.end()) pos_ = static_cast<size_t>(it - custom_.begin());
  else inCustom_ = false;  // a jump out of it
}

}  // namespace fw::slide::show
