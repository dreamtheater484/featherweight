// fwtool torture for decks (P2's "never damage a file" gate, docs/p2-plan.md). Each deck is first
// saved with no edits: the save must write no part, and the saved file must have the original's parts
// in the original's order, byte for byte (the save-without-edits check; scripts\deck-torture.ps1 then
// has PowerPoint open it and compares its PDF with the original's). Then each deck gets rounds of
// seeded random edits through the edit API, aimed at what breaks decks: new, duplicated, deleted,
// moved and hidden slides, layout changes, typing in placeholders, text boxes and shapes, notes,
// moving, resizing, rotating, deleting, grouping, ungrouping and reordering shapes, formatting, undo
// and redo. After each round the deck is saved, and the saved file must open, pass fwtool check with
// no errors the original didn't have, raise no new model problems, read back as the edited deck, and
// keep every part the save didn't write byte for byte. Undoing every edit must give the deck as
// opened, and redoing them the deck as edited. A failure is shrunk to the fewest edits that still
// fail and kept as a reproducer. scripts\deck-torture.ps1 runs the validator and the PowerPoint open
// check on the saved copies.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>

#include "Common.h"
#include "DeckTorture.h"
#include "core/Strings.h"
#include "pkg/Check.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "pkg/Zip.h"
// p2-model's edit API and writer (P2); without them every edit is refused.
#if __has_include("slide/edit/Edit.h") && __has_include("slide/edit/Save.h")
#include "slide/edit/Edit.h"
#include "slide/edit/Save.h"
#define FW_SLIDE_EDIT 1
#else
#define FW_SLIDE_EDIT 0
#endif

namespace fwtool {
namespace decktorture {
namespace {

namespace fs = std::filesystem;
namespace sl = fw::slide;

constexpr struct {
  Op op;
  const char* name;
} kOps[] = {
    {Op::NewSlide, "new-slide"}, {Op::Duplicate, "duplicate"}, {Op::DeleteSlide, "delete-slide"},
    {Op::MoveSlide, "move-slide"}, {Op::Hide, "hide"},          {Op::Layout, "layout"},
    {Op::Type, "type"},          {Op::Notes, "notes"},          {Op::Move, "move"},
    {Op::Resize, "resize"},      {Op::Rotate, "rotate"},        {Op::DeleteShape, "delete"},
    {Op::Group, "group"},        {Op::Ungroup, "ungroup"},      {Op::Order, "order"},
    {Op::Format, "format"},      {Op::Undo, "undo"},            {Op::Redo, "redo"},
    {Op::Save, "save"},
};

const char* OpName(Op op) {
  for (const auto& o : kOps)
    if (o.op == op) return o.name;
  return "?";
}

std::string Quote(std::string_view text) {
  std::string out = "\"";
  for (char c : text) {
    if (c == '"' || c == '\\') out += '\\', out += c;
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else out += c;
  }
  return out + "\"";
}

// Reads a quoted string at the start of `s` and moves past it.
bool Unquote(std::string_view& s, std::string& out) {
  if (s.empty() || s.front() != '"') return false;
  out.clear();
  for (size_t i = 1; i < s.size(); ++i) {
    char c = s[i];
    if (c == '\\' && i + 1 < s.size()) {
      char n = s[++i];
      out += n == 'n' ? '\n' : n == 'r' ? '\r' : n == 't' ? '\t' : n;
    } else if (c == '"') {
      s.remove_prefix(i + 1);
      return true;
    } else {
      out += c;
    }
  }
  return false;
}

std::string_view NextWord(std::string_view& s) {
  while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  size_t end = s.find(' ');
  std::string_view w = s.substr(0, end);
  s.remove_prefix(end == std::string_view::npos ? s.size() : end);
  while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  return w;
}

std::string Num(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.6g", v);
  return buf;
}

bool ToInt(std::string_view s, int& v) {
  if (s.empty()) return false;
  char* end = nullptr;
  std::string t(s);
  long n = std::strtol(t.c_str(), &end, 10);
  if (*end) return false;
  v = static_cast<int>(n);
  return true;
}

bool ToDouble(std::string_view s, double& v) {
  if (s.empty()) return false;
  char* end = nullptr;
  std::string t(s);
  v = std::strtod(t.c_str(), &end);
  return !*end;
}

// "2#5" (slide 2, shape 5) or "2#5,7,9".
bool ParseShapeRef(std::string_view s, int& slide, std::vector<uint32_t>& ids) {
  size_t hash = s.find('#');
  if (hash == std::string_view::npos || !ToInt(s.substr(0, hash), slide)) return false;
  ids.clear();
  for (std::string_view rest = s.substr(hash + 1); !rest.empty();) {
    size_t comma = rest.find(',');
    int id = 0;
    if (!ToInt(rest.substr(0, comma), id) || id < 0) return false;
    ids.push_back(static_cast<uint32_t>(id));
    rest.remove_prefix(comma == std::string_view::npos ? rest.size() : comma + 1);
  }
  return !ids.empty();
}

bool IsShapeOp(Op op) {
  return op == Op::Type || op == Op::Move || op == Op::Resize || op == Op::Rotate || op == Op::DeleteShape ||
         op == Op::Ungroup || op == Op::Order || op == Op::Format;
}

}  // namespace

std::string FormatEdit(const Edit& e) {
  std::string s = OpName(e.op);
  const std::string at = std::to_string(e.slide) + "#" + std::to_string(e.shape);
  switch (e.op) {
    case Op::NewSlide: s += " " + std::to_string(e.slide) + " L" + std::to_string(e.count); break;
    case Op::Duplicate:
    case Op::DeleteSlide: s += " " + std::to_string(e.slide); break;
    case Op::MoveSlide: s += " " + std::to_string(e.slide) + " " + std::to_string(e.to); break;
    case Op::Hide: s += " " + std::to_string(e.slide) + " " + e.arg; break;
    case Op::Layout: s += " " + std::to_string(e.slide) + " L" + std::to_string(e.count); break;
    case Op::Type: s += " " + at + " " + e.arg + " " + Quote(e.text); break;
    case Op::Notes: s += " " + std::to_string(e.slide) + " " + Quote(e.text); break;
    case Op::Move:
    case Op::Resize: s += " " + at + " " + Num(e.dx) + " " + Num(e.dy); break;
    case Op::Rotate: s += " " + at + " " + Num(e.dx); break;
    case Op::DeleteShape:
    case Op::Ungroup: s += " " + at; break;
    case Op::Group: {
      s += " " + std::to_string(e.slide) + "#";
      for (size_t i = 0; i < e.shapes.size(); ++i) s += (i ? "," : "") + std::to_string(e.shapes[i]);
      break;
    }
    case Op::Order:
    case Op::Format: s += " " + at + " " + e.arg; break;
    case Op::Undo:
    case Op::Redo: s += " " + std::to_string(e.count); break;
    case Op::Save: break;
  }
  if (!e.target.empty()) s += " # " + e.target;
  return s;
}

bool ParseEdit(std::string_view line, Edit& e) {
  e = Edit{};
  // The target comment: the first " # " outside quoted text (it may hold quotes of its own).
  for (size_t i = 0, quoted = 0; i < line.size(); ++i) {
    if (quoted && line[i] == '\\') ++i;
    else if (line[i] == '"') quoted = !quoted;
    else if (!quoted && line.substr(i).starts_with(" # ")) {
      e.target = std::string(line.substr(i + 3));
      line = line.substr(0, i);
      break;
    }
  }
  std::string_view s = line;
  std::string_view name = NextWord(s);
  bool found = false;
  for (const auto& o : kOps)
    if (name == o.name) e.op = o.op, found = true;
  if (!found) return false;
  auto layoutNumber = [&](std::string_view w) {
    int n = 0;
    if (w.size() < 2 || w.front() != 'L' || !ToInt(w.substr(1), n) || n < 0) return false;
    e.count = static_cast<uint32_t>(n);
    return true;
  };
  std::vector<uint32_t> ids;
  switch (e.op) {
    case Op::NewSlide:
    case Op::Layout: return ToInt(NextWord(s), e.slide) && layoutNumber(NextWord(s)) && s.empty();
    case Op::Duplicate:
    case Op::DeleteSlide: return ToInt(NextWord(s), e.slide) && s.empty();
    case Op::MoveSlide: return ToInt(NextWord(s), e.slide) && ToInt(NextWord(s), e.to) && s.empty();
    case Op::Hide: {
      if (!ToInt(NextWord(s), e.slide)) return false;
      e.arg = std::string(NextWord(s));
      return (e.arg == "on" || e.arg == "off") && s.empty();
    }
    case Op::Notes: return ToInt(NextWord(s), e.slide) && Unquote(s, e.text) && s.empty();
    case Op::Group: {
      if (!ParseShapeRef(NextWord(s), e.slide, e.shapes)) return false;
      return e.shapes.size() >= 2 && s.empty();
    }
    case Op::Undo:
    case Op::Redo: {
      int n = 0;
      if (!ToInt(NextWord(s), n) || n < 1) return false;
      e.count = static_cast<uint32_t>(n);
      return s.empty();
    }
    case Op::Save: return s.empty();
    default: break;
  }
  if (!IsShapeOp(e.op) || !ParseShapeRef(NextWord(s), e.slide, ids) || ids.size() != 1) return false;
  e.shape = ids[0];
  switch (e.op) {
    case Op::Type: {
      e.arg = std::string(NextWord(s));
      if (e.arg != "start" && e.arg != "end" && e.arg != "replace" && e.arg != "para") return false;
      return Unquote(s, e.text) && s.empty();
    }
    case Op::Move:
    case Op::Resize: return ToDouble(NextWord(s), e.dx) && ToDouble(NextWord(s), e.dy) && s.empty();
    case Op::Rotate: return ToDouble(NextWord(s), e.dx) && s.empty();
    case Op::DeleteShape:
    case Op::Ungroup: return s.empty();
    case Op::Order: {
      e.arg = std::string(NextWord(s));
      return (e.arg == "front" || e.arg == "back" || e.arg == "forward" || e.arg == "backward") && s.empty();
    }
    case Op::Format: {
      e.arg = std::string(NextWord(s));
      return !e.arg.empty() && s.empty();
    }
    default: return false;
  }
}

namespace {

// ---- Targets

// Without p2-model's edit API in the build: every edit is refused, and Save writes the package as
// it was read (the package's own save: every part copied raw).
class ReadOnlyTarget : public DeckTarget {
 public:
  explicit ReadOnlyTarget(std::unique_ptr<sl::Presentation> deck) : deck_(std::move(deck)) {}
  const char* Name() const override { return "none: the edit API isn't in this build, every edit refused"; }
  sl::Presentation& Deck() override { return *deck_; }
  Applied Apply(const Edit& e, std::string& why) override {
    if (e.op == Op::Undo || e.op == Op::Redo) {
      why = "nothing to " + std::string(OpName(e.op));
      return Applied::Refused;
    }
    why = "the edit API isn't in this build";
    return Applied::Refused;
  }
  fw::pkg::Status Save(std::vector<uint8_t>& out) override {
    written.clear();
    fw::pkg::MemorySink sink;
    fw::pkg::Status st = deck_->GetPackage().Save(sink);
    if (st.Ok()) out = std::move(sink.bytes);
    return st;
  }
  int UndoAll() override { return 0; }

 private:
  std::unique_ptr<sl::Presentation> deck_;
};

#if FW_SLIDE_EDIT
namespace ed = fw::slide::edit;

std::string Utf8(const std::u16string& s) { return fw::WideToUtf8(std::wstring(s.begin(), s.end())); }

// Through p2-model's edit API (slide/edit/Edit.h) and its writer (Save.h), as the tab's commands do.
class EditTarget : public DeckTarget {
 public:
  explicit EditTarget(std::unique_ptr<sl::Presentation> deck) : deck_(std::move(deck)), editor_(*deck_) {}
  const char* Name() const override { return "the edit API (slide/edit/Edit.h)"; }
  sl::Presentation& Deck() override { return *deck_; }

  Applied Apply(const Edit& e, std::string& why) override {
    ed::Result r = Run(e, why);
    if (!why.empty()) return Applied::Refused;  // refused here, before the editor was asked
    if (!r) {
      why = Utf8(r.message);
      return Applied::Refused;
    }
    return Applied::Done;
  }

  fw::pkg::Status Save(std::vector<uint8_t>& out) override {
    written.clear();
    return ed::SaveToBytes(editor_, out, &written);
  }
  int UndoAll() override {
    const size_t steps = editor_.UndoNames().size();
    if (steps && !editor_.Undo(steps)) return -1;
    return static_cast<int>(steps);
  }
  bool RedoSteps(int steps) override { return steps <= 0 || static_cast<bool>(editor_.Redo(static_cast<size_t>(steps))); }
  bool CanUndo() const override { return editor_.CanUndo(); }

 private:
  ed::Result Run(const Edit& e, std::string& why) {
    const size_t slide = static_cast<size_t>(std::max(e.slide, 0));
    const std::wstring text = fw::Utf8ToWide(e.text);
    switch (e.op) {
      case Op::NewSlide: return editor_.NewSlide(static_cast<size_t>(e.slide + 1), e.count);
      case Op::Duplicate: return editor_.DuplicateSlides({slide});
      case Op::DeleteSlide: return editor_.DeleteSlides({slide});
      case Op::MoveSlide: return editor_.MoveSlides({slide}, static_cast<size_t>(e.to));
      case Op::Hide: return editor_.HideSlides({slide}, e.arg == "on");
      case Op::Layout: return editor_.SetLayout({slide}, e.count);
      case Op::Notes: return editor_.SetNotesText(slide, text);
      case Op::Undo:
      case Op::Redo: {
        const size_t have = e.op == Op::Undo ? editor_.UndoNames().size() : editor_.RedoNames().size();
        if (!have) {
          why = std::string("nothing to ") + OpName(e.op);
          return {};
        }
        const size_t steps = std::min<size_t>(e.count, have);
        return e.op == Op::Undo ? editor_.Undo(steps) : editor_.Redo(steps);
      }
      case Op::Type: {
        const ed::TextTarget t{slide, e.shape};
        const size_t n = editor_.ParagraphCount(t);
        if (n == 0) return editor_.SetText(t, text);  // a shape without text yet: typing gives it some
        const ed::TextPos end{n - 1, editor_.ParagraphText(t, n - 1).size()};
        if (e.arg == "start") return editor_.InsertText(t, {0, 0}, text);
        if (e.arg == "replace") return editor_.ReplaceText(t, {{0, 0}, end}, text);
        if (e.arg == "para") return editor_.InsertText(t, end, L"\n" + text);
        return editor_.InsertText(t, end, text);
      }
      case Op::Move:
      case Op::Resize:
      case Op::Rotate: {
        const sl::Shape* s = slide < deck_->SlideCount() ? deck_->GetSlide(slide)->FindShape(e.shape) : nullptr;
        if (!s) {
          why = "no shape " + std::to_string(e.shape) + " on slide " + std::to_string(slide);
          return {};
        }
        ed::Transform t{s->bounds, s->rotation, s->flipH, s->flipV};
        if (e.op == Op::Move) {
          t.bounds.left += static_cast<float>(e.dx), t.bounds.right += static_cast<float>(e.dx);
          t.bounds.top += static_cast<float>(e.dy), t.bounds.bottom += static_cast<float>(e.dy);
        } else if (e.op == Op::Resize) {
          t.bounds.right = t.bounds.left + s->bounds.Width() * static_cast<float>(e.dx);
          t.bounds.bottom = t.bounds.top + s->bounds.Height() * static_cast<float>(e.dy);
        } else {
          t.rotation = static_cast<float>(e.dx);
        }
        return editor_.SetTransform(slide, e.shape, t);
      }
      case Op::DeleteShape: return editor_.DeleteShapes(slide, {e.shape});
      case Op::Group: return editor_.Group(slide, e.shapes);
      case Op::Ungroup: return editor_.Ungroup(slide, e.shape);
      case Op::Order: {
        const ed::ZOrder z = e.arg == "front" ? ed::ZOrder::BringToFront : e.arg == "back" ? ed::ZOrder::SendToBack
                             : e.arg == "forward" ? ed::ZOrder::BringForward : ed::ZOrder::SendBackward;
        return editor_.Arrange(slide, {e.shape}, z);
      }
      case Op::Format: {
        constexpr const char* kA = "http://schemas.openxmlformats.org/drawingml/2006/main";
        auto value = [&](size_t prefix) { return e.arg.substr(prefix); };
        if (e.arg.starts_with("fill=") || e.arg.starts_with("line=")) {
          const std::string fill = "<a:solidFill xmlns:a=\"" + std::string(kA) + "\"><a:srgbClr val=\"" + value(5) + "\"/></a:solidFill>";
          ed::ShapePatch p;
          if (e.arg.starts_with("fill=")) p.fill = fill;
          else p.line = "<a:ln xmlns:a=\"" + std::string(kA) + "\" w=\"25400\"><a:solidFill><a:srgbClr val=\"" + value(5) +
                        "\"/></a:solidFill></a:ln>";
          return editor_.SetShapeProps(slide, {e.shape}, p);
        }
        // The text ones over all the shape's text.
        const ed::TextTarget t{slide, e.shape};
        const size_t n = editor_.ParagraphCount(t);
        if (n == 0) {
          why = "the shape has no text to format";
          return {};
        }
        ed::RunPatch p;
        if (e.arg == "bold") p.bold = true;
        else if (e.arg == "italic") p.italic = true;
        else if (e.arg.starts_with("size=")) p.size = static_cast<float>(std::atof(value(5).c_str()));
        else if (e.arg.starts_with("color=")) p.color = ed::ColorSpec::Rgb(static_cast<uint32_t>(std::strtoul(value(6).c_str(), nullptr, 16)));
        else {
          why = "unknown format " + e.arg;
          return {};
        }
        return editor_.SetRunProps(t, {{0, 0}, {n - 1, editor_.ParagraphText(t, n - 1).size()}}, p);
      }
      case Op::Save: return {};
    }
    return {};
  }

  std::unique_ptr<sl::Presentation> deck_;
  ed::Editor editor_;  // after deck_: it goes first
};
#endif

// ---- Random edits

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
  uint64_t Next() {  // splitmix64
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  uint32_t Below(uint32_t n) { return n ? static_cast<uint32_t>(Next() % n) : 0; }
  bool Chance(uint32_t percent) { return Below(100) < percent; }
  double Between(double a, double b) { return a + (b - a) * static_cast<double>(Next() % 1000001) / 1000000.0; }
  template <typename T, size_t N>
  const T& Pick(const T (&items)[N]) { return items[Below(N)]; }
};

uint64_t RoundSeed(uint64_t seed, const std::string& name, uint32_t round) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : name) h = (h ^ c) * 1099511628211ull;
  Rng mix(seed ^ h ^ (static_cast<uint64_t>(round) << 32));
  return mix.Next();
}

// What's typed: plain words, the characters XML escapes, other scripts, an emoji (a surrogate pair),
// a tab, spaces at the ends, and long text that makes autofit and wrapping work.
constexpr const char* kTyped[] = {
    "Hello",
    "Quarterly results",
    " and more ",
    "<b>&amp; \"quotes\" 'apostrophes' & ampersands</b>",
    "na\xC3\xAFve caf\xC3\xA9 \xE2\x80\x94 \xE2\x82\xAC" "12,50",
    "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE3\x83\x86\xE3\x82\xAD\xE3\x82\xB9\xE3\x83\x88",
    "\xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9",
    "smile \xF0\x9F\x99\x82 ok",
    "tab\there",
    "1.",
    "A much longer line of text that wraps over several lines in a title or a text box and makes autofit shrink "
    "the text when it no longer fits in the shape it was typed into, as PowerPoint does",
    "x",
};

constexpr const char* kColors[] = {"C00000", "0070C0", "00B050", "FFC000", "7030A0", "000000", "FFFFFF"};

struct ShapeInfo {
  const sl::Shape* shape = nullptr;
  bool topLevel = false;
};

void CollectShapes(const std::vector<sl::Shape>& shapes, bool top, std::vector<ShapeInfo>& out) {
  for (const sl::Shape& s : shapes) {
    out.push_back({&s, top});
    if (s.kind == sl::Shape::Kind::Group) CollectShapes(s.children, false, out);
  }
}

std::string KindName(sl::Shape::Kind k) {
  switch (k) {
    case sl::Shape::Kind::Shape: return "shape";
    case sl::Shape::Kind::Picture: return "picture";
    case sl::Shape::Kind::Connector: return "connector";
    case sl::Shape::Kind::Group: return "group";
    case sl::Shape::Kind::Table: return "table";
    case sl::Shape::Kind::Chart: return "chart";
    case sl::Shape::Kind::Diagram: return "SmartArt";
    case sl::Shape::Kind::Ole: return "embedded object";
    case sl::Shape::Kind::Graphic: return "graphic frame";
    case sl::Shape::Kind::ContentPart: return "ink";
  }
  return "?";
}

std::string Describe(const sl::Shape& s) {
  if (s.placeholder) return "a placeholder";
  if (s.textBox) return "a text box";
  if (s.media) return "a video or audio";
  std::string k = KindName(s.kind);
  return (k[0] == 'a' || k[0] == 'e' || k[0] == 'i' ? "an " : "a ") + k;
}

class Generator {
 public:
  explicit Generator(uint64_t seed) : rng_(seed) {}

  Edit Next(sl::Presentation& deck) {
    for (int tries = 0; tries < 50; ++tries) {
      Edit e;
      if (Make(deck, e)) return e;
    }
    Edit e;
    e.op = Op::Undo;
    return e;
  }

 private:
  bool Make(sl::Presentation& deck, Edit& e) {
    // Weights: typing and the shape edits most, the slide operations often, undo and redo now and then.
    static constexpr struct {
      Op op;
      uint32_t weight;
    } kWeights[] = {
        {Op::Type, 20}, {Op::Format, 9}, {Op::Move, 9}, {Op::Resize, 6}, {Op::Rotate, 3}, {Op::DeleteShape, 6},
        {Op::Group, 4}, {Op::Ungroup, 2}, {Op::Order, 3}, {Op::NewSlide, 5}, {Op::Duplicate, 6},
        {Op::DeleteSlide, 5}, {Op::MoveSlide, 6}, {Op::Hide, 2}, {Op::Layout, 4}, {Op::Notes, 3},
        {Op::Undo, 4}, {Op::Redo, 2}, {Op::Save, 1},
    };
    uint32_t total = 0;
    for (const auto& w : kWeights) total += w.weight;
    uint32_t pick = rng_.Below(total);
    for (const auto& w : kWeights) {
      if (pick < w.weight) {
        e.op = w.op;
        break;
      }
      pick -= w.weight;
    }
    const int slides = static_cast<int>(deck.SlideCount());
    const uint32_t layouts = static_cast<uint32_t>(deck.Layouts().size());
    switch (e.op) {
      case Op::Undo: e.count = 1 + rng_.Below(3); return true;
      case Op::Redo: e.count = 1 + rng_.Below(2); return true;
      case Op::Save: return true;
      case Op::NewSlide:
      case Op::Layout: {
        if (!layouts || (e.op == Op::Layout && !slides)) return false;
        e.slide = e.op == Op::NewSlide ? static_cast<int>(rng_.Below(static_cast<uint32_t>(slides + 1))) - 1
                                       : static_cast<int>(rng_.Below(static_cast<uint32_t>(slides)));
        // Mostly a layout of the slide's own master, as the layout gallery shows first.
        e.count = rng_.Below(layouts);
        if (e.slide >= 0 && rng_.Chance(80)) {
          const sl::Slide* s = deck.GetSlide(static_cast<size_t>(e.slide));
          if (s->master && !s->master->layouts.empty()) {
            const sl::Layout* l = s->master->layouts[rng_.Below(static_cast<uint32_t>(s->master->layouts.size()))];
            for (uint32_t i = 0; i < layouts; ++i)
              if (deck.Layouts()[i].get() == l) e.count = i;
          }
        }
        e.target = "the layout \"" + deck.Layouts()[e.count]->name + "\"";
        return true;
      }
      case Op::Duplicate:
      case Op::Hide:
      case Op::Notes:
      case Op::DeleteSlide: {
        if (!slides) return false;
        // Deleting the last slide leaves a deck without slides, which PowerPoint opens; not too often.
        if (e.op == Op::DeleteSlide && slides == 1 && !rng_.Chance(20)) return false;
        e.slide = static_cast<int>(rng_.Below(static_cast<uint32_t>(slides)));
        if (e.op == Op::Hide) e.arg = deck.GetSlide(static_cast<size_t>(e.slide))->hidden ? "off" : "on";
        if (e.op == Op::Notes) {
          e.text = rng_.Chance(15) ? std::string() : kTyped[rng_.Below(static_cast<uint32_t>(std::size(kTyped)))];
          e.target = deck.GetSlide(static_cast<size_t>(e.slide))->notes ? "notes" : "a slide without notes";
        }
        return true;
      }
      case Op::MoveSlide: {
        if (slides < 2) return false;
        e.slide = static_cast<int>(rng_.Below(static_cast<uint32_t>(slides)));
        do e.to = static_cast<int>(rng_.Below(static_cast<uint32_t>(slides)));
        while (e.to == e.slide);
        if (!deck.Sections().empty()) e.target = "a deck with sections";
        return true;
      }
      default: break;
    }
    // Shape edits: a slide with shapes of its own.
    if (!slides) return false;
    e.slide = static_cast<int>(rng_.Below(static_cast<uint32_t>(slides)));
    const sl::Slide* slide = deck.GetSlide(static_cast<size_t>(e.slide));
    std::vector<ShapeInfo> all;
    CollectShapes(slide->shapes, true, all);
    if (all.empty()) return false;
    if (e.op == Op::Group) {
      std::vector<const sl::Shape*> top;
      for (const ShapeInfo& i : all)
        // Placeholders can't be grouped in PowerPoint: tried now and then (the edit API must refuse).
        if (i.topLevel && (!i.shape->placeholder || rng_.Chance(10))) top.push_back(i.shape);
      if (top.size() < 2) return false;
      const uint32_t n = 2 + rng_.Below(static_cast<uint32_t>(std::min<size_t>(top.size(), 4) - 1));
      for (uint32_t k = 0; k < n; ++k) {
        const sl::Shape* s = top[rng_.Below(static_cast<uint32_t>(top.size()))];
        if (std::find(e.shapes.begin(), e.shapes.end(), s->id) == e.shapes.end()) e.shapes.push_back(s->id);
      }
      if (e.shapes.size() < 2) return false;
      e.target = std::to_string(e.shapes.size()) + " shapes";
      return true;
    }
    std::vector<const sl::Shape*> candidates;
    for (const ShapeInfo& i : all) {
      const sl::Shape& s = *i.shape;
      switch (e.op) {
        case Op::Type:
          // Text goes into shapes (placeholders, text boxes); a shape without a text body sometimes.
          if (s.kind == sl::Shape::Kind::Shape && (s.textNode || rng_.Chance(30))) candidates.push_back(&s);
          break;
        case Op::Ungroup:
          if (s.kind == sl::Shape::Kind::Group) candidates.push_back(&s);
          break;
        case Op::Order:
          if (i.topLevel) candidates.push_back(&s);
          break;
        default:
          // Moving shapes inside groups now and then; mostly the slide's own.
          if (i.topLevel || rng_.Chance(25)) candidates.push_back(&s);
          break;
      }
    }
    if (candidates.empty()) return false;
    const sl::Shape& s = *candidates[rng_.Below(static_cast<uint32_t>(candidates.size()))];
    e.shape = s.id;
    e.target = Describe(s);
    switch (e.op) {
      case Op::Type: {
        static constexpr const char* kWhere[] = {"end", "end", "start", "replace", "para"};
        e.arg = rng_.Pick(kWhere);
        e.text = kTyped[rng_.Below(static_cast<uint32_t>(std::size(kTyped)))];
        return true;
      }
      case Op::Move:
        e.dx = std::round(rng_.Between(-120, 120) * 4) / 4;
        e.dy = std::round(rng_.Between(-90, 90) * 4) / 4;
        return true;
      case Op::Resize:
        e.dx = std::round(rng_.Between(0.5, 1.6) * 100) / 100;
        e.dy = rng_.Chance(50) ? e.dx : std::round(rng_.Between(0.5, 1.6) * 100) / 100;
        return true;
      case Op::Rotate: {
        static constexpr double kAngles[] = {0, 15, 45, 90, 180, 270, 330};
        e.dx = rng_.Pick(kAngles);
        return true;
      }
      case Op::Order: {
        static constexpr const char* kOrder[] = {"front", "back", "forward", "backward"};
        e.arg = rng_.Pick(kOrder);
        return true;
      }
      case Op::Format: {
        const std::string color = rng_.Pick(kColors);
        switch (rng_.Below(6)) {
          case 0: e.arg = "fill=" + color; break;
          case 1: e.arg = "line=" + color; break;
          case 2: e.arg = "bold"; break;
          case 3: e.arg = "italic"; break;
          case 4: e.arg = "size=" + std::to_string(10 + 2 * rng_.Below(20)); break;
          default: e.arg = "color=" + color; break;
        }
        return true;
      }
      default: return true;  // DeleteShape, Ungroup
    }
  }

  Rng rng_;
};

// ---- The deck, one fact per line (undo and read-back checks)

long long Emu(float points) { return std::llround(static_cast<double>(points) * 12700.0); }

std::string Hex6(const draw::Color& c) {
  auto ch = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.f, 1.f) * 255)); };
  char buf[16];
  std::snprintf(buf, sizeof buf, "%02X%02X%02X", ch(c.r), ch(c.g), ch(c.b));
  return buf;
}

std::string TextLine(const sl::TextBody& body) {
  std::string s;
  for (size_t p = 0; p < body.paragraphs.size(); ++p) {
    if (p) s += " | ";
    for (const sl::TextRun& r : body.paragraphs[p].runs) {
      const sl::RunProps& f = r.props;
      s += "[";
      if (f.bold) s += "b ";
      if (f.italic) s += "i ";
      s += std::to_string(std::lround(f.size * 100)) + " " + Hex6(f.color) + "]";
      if (r.kind == sl::TextRun::Kind::Break) s += "\\v";
      else if (r.kind == sl::TextRun::Kind::Field) s += "{" + r.fieldType + ":" + fw::WideToUtf8(r.text) + "}";
      else s += fw::WideToUtf8(r.text);
    }
  }
  return s;
}

void ShapeLines(sl::Presentation& deck, const sl::Slide& slide, const std::vector<sl::Shape>& shapes, const std::string& indent,
                std::vector<std::string>& out) {
  for (const sl::Shape& s : shapes) {
    std::string line = indent + "shape " + std::to_string(s.id) + " " + KindName(s.kind);
    if (s.placeholder)
      line += " ph=" + std::to_string(static_cast<int>(s.placeholder.type)) +
              (s.placeholder.hasIndex ? "/" + std::to_string(s.placeholder.index) : "");
    line += " \"" + s.name + "\" at " + std::to_string(Emu(s.bounds.left)) + "," + std::to_string(Emu(s.bounds.top)) + " " +
            std::to_string(Emu(s.bounds.Width())) + "x" + std::to_string(Emu(s.bounds.Height()));
    if (s.rotation) line += " rot " + std::to_string(std::lround(s.rotation * 60000));
    if (s.flipH) line += " flipH";
    if (s.flipV) line += " flipV";
    if (s.hidden) line += " hidden";
    out.push_back(line);
    if (const sl::TextBody* t = deck.Text(s, slide)) out.push_back(indent + "  text " + TextLine(*t));
    if (s.kind == sl::Shape::Kind::Group) ShapeLines(deck, slide, s.children, indent + "  ", out);
  }
}

std::vector<std::string> Digest(sl::Presentation& deck) {
  std::vector<std::string> out;
  deck.LoadAll();
  const draw::Size size = deck.SlideSize();
  out.push_back("size " + std::to_string(Emu(size.width)) + "x" + std::to_string(Emu(size.height)) + ", first number " +
                std::to_string(deck.FirstSlideNumber()) + ", " + std::to_string(deck.SlideCount()) + " slides");
  for (size_t i = 0; i < deck.SlideCount(); ++i) {
    const sl::Slide* s = deck.GetSlide(i);
    std::string layout = "?";
    for (size_t l = 0; l < deck.Layouts().size(); ++l)
      if (deck.Layouts()[l].get() == s->layout) layout = "L" + std::to_string(l) + " \"" + s->layout->name + "\"";
    out.push_back("slide " + std::to_string(i) + " id " + std::to_string(s->id) + " layout " + layout +
                  (s->hidden ? " hidden" : "") + (s->showMasterShapes ? "" : " no-master-shapes"));
    ShapeLines(deck, *s, s->shapes, "  ", out);
    if (s->notes) out.push_back("  notes " + fw::WideToUtf8(s->NotesText()));
  }
  for (const sl::Section& sec : deck.Sections()) {
    std::string line = "section \"" + sec.name + "\":";
    for (size_t i : sec.slides) line += " " + std::to_string(i);
    out.push_back(line);
  }
  for (const sl::CustomShow& show : deck.CustomShows()) {
    std::string line = "custom show " + std::to_string(show.id) + " \"" + show.name + "\":";
    for (size_t i : show.slides) line += " " + std::to_string(i);
    out.push_back(line);
  }
  return out;
}

std::string FirstDifference(const std::vector<std::string>& a, const std::vector<std::string>& b) {
  size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i)
    if (a[i] != b[i]) return "edited: " + a[i] + " | saved: " + b[i];
  if (a.size() != b.size()) return a.size() > b.size() ? "edited has more: " + a[n] : "saved has more: " + b[n];
  return {};
}

std::string Joined(const std::vector<std::string>& lines) {
  std::string s;
  for (const std::string& l : lines) s += l + "\n";
  return s;
}

std::string Hex(const fw::pkg::Sha256& h) {
  static const char* digits = "0123456789abcdef";
  std::string s;
  for (uint8_t b : h) s += digits[b >> 4], s += digits[b & 15];
  return s;
}

std::vector<uint8_t> ReadBytes(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

void WriteBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream(path, std::ios::binary | std::ios::trunc)
      .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::string SafeName(const fs::path& file) {
  std::string name = Display(fs::path(file).replace_extension());
  for (char& c : name)
    if (c == '/' || c == '\\' || c == ':' || c == ' ') c = '_';
  while (!name.empty() && (name.front() == '.' || name.front() == '_')) name.erase(name.begin());
  return name;
}

std::string JsonText(std::string_view s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\', out += c;
    else if (static_cast<unsigned char>(c) < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\u%04x", c);
      out += buf;
    } else out += c;
  }
  return out + "\"";
}

std::string AbsoluteUtf8(const fs::path& p) { return fw::WideToUtf8(fs::absolute(p).generic_wstring()); }

// ---- One round: open, edit, save, check.

struct Failure {
  std::string category;  // open, edit, undo, redo, save, reopen, check, model, readback, untouched, unchanged
  std::string message;
};

struct Round {
  std::optional<Failure> failure;
  uint32_t done = 0, refused = 0;
  std::vector<Edit> applied;
  std::vector<uint8_t> saved;
  std::vector<std::string> written;
  std::string edited, readBack;  // the digests, when they differ
  bool identical = false;        // with no edits: the saved file is the original, byte for byte
};

// What the original holds that a saved copy is measured against.
struct Original {
  std::unique_ptr<fw::pkg::Package> pkg;
  std::vector<std::string> damaged, checkErrors, problems;
  std::vector<uint8_t> bytes;
};

bool Listed(const std::vector<std::string>& list, const std::string& part) {
  return std::find(list.begin(), list.end(), part) != list.end() ||
         (part.starts_with('/') && std::find(list.begin(), list.end(), part.substr(1)) != list.end());
}

// A save, measured against the original: the parts it didn't write or leave out are the original's,
// byte for byte; no new check errors or model problems; it reads back as the deck as edited.
std::optional<Failure> CheckSave(Original& orig, DeckTarget& target, Round& r) {
  std::vector<uint8_t> bytes;
  if (fw::pkg::Status st = target.Save(bytes); !st.Ok()) return Failure{"save", st.message};
  r.written = target.written;
  r.saved = bytes;
  fw::pkg::Status st;
  auto again = fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(std::move(bytes)), orig.pkg->GetLimits(), st);
  std::vector<std::string> damaged;
  if (again) st = again->Verify(&damaged);
  if (!again || !st.Ok()) return Failure{"reopen", "the saved copy doesn't open: " + st.message};
  if (damaged != orig.damaged) return Failure{"reopen", "the saved copy has other damaged parts than the original"};
  for (const std::string& e : fw::pkg::CheckPackage(*again).errors)
    if (!Listed(orig.checkErrors, e)) return Failure{"check", e};
  // Every part the save didn't write is the original's, byte for byte.
  std::vector<uint8_t> a, b;
  for (const auto& part : orig.pkg->Parts()) {
    if (Listed(r.written, part.name) || Listed(orig.damaged, part.name)) continue;
    if (!again->ReadPart(part.name, b).Ok()) return Failure{"untouched", part.name + " is missing from the saved copy"};
    if (!orig.pkg->ReadPart(part.name, a).Ok() || a != b) return Failure{"untouched", part.name + " changed"};
  }
  for (const auto& part : again->Parts()) {
    const bool known = std::any_of(orig.pkg->Parts().begin(), orig.pkg->Parts().end(),
                                   [&](const auto& p) { return p.name == part.name; });
    if (!known && !Listed(r.written, part.name)) return Failure{"untouched", part.name + " is new, but the save didn't write it"};
  }
  // Saved without edits: nothing written, the same parts in the same order, the same zip records.
  if (r.done == 0) {
    if (!r.written.empty()) return Failure{"unchanged", "saving without edits wrote " + r.written.front()};
    const auto& before = orig.pkg->Zip().Entries();
    const auto& after = again->Zip().Entries();
    if (before.size() != after.size()) return Failure{"unchanged", "saving without edits changed the number of zip entries"};
    for (size_t i = 0; i < before.size(); ++i) {
      if (before[i].name != after[i].name) return Failure{"unchanged", "saving without edits moved " + before[i].name};
      if (before[i].crc32 != after[i].crc32 || before[i].uncompressedSize != after[i].uncompressedSize)
        return Failure{"unchanged", "saving without edits changed " + before[i].name};
    }
    r.identical = r.saved == orig.bytes;
  }
  auto deck = sl::Read(std::move(again), st);
  if (!deck) return Failure{"reopen", "the model doesn't read the saved copy: " + st.message};
  std::vector<std::string> readBack = Digest(*deck);
  for (const std::string& p : deck->Problems())
    if (!Listed(orig.problems, p)) return Failure{"model", p};
  std::vector<std::string> edited = Digest(target.Deck());
  if (std::string diff = FirstDifference(edited, readBack); !diff.empty()) {
    r.edited = Joined(edited), r.readBack = Joined(readBack);
    return Failure{"readback", diff};
  }
  // Every save is measured against the file as opened: the writer copies untouched parts from it and
  // lists what it wrote or left out since then (Save.h), not since the last save.
  return std::nullopt;
}

// `replay`: apply these edits instead of generating `edits` of them. `current`: the edits so far,
// rewritten before each one, so that a crash leaves them behind.
Round RunRound(const fs::path& file, uint64_t seed, uint32_t edits, const std::vector<Edit>* replay, const fs::path& current) {
  Round r;
  Original orig;
  fw::pkg::Status st;
  orig.bytes = ReadBytes(file);
  orig.pkg = OpenPackage(file, st);
  if (orig.pkg) st = orig.pkg->Verify(&orig.damaged);
  if (!orig.pkg || !st.Ok()) {
    r.failure = Failure{"open", st.message};
    return r;
  }
  orig.checkErrors = fw::pkg::CheckPackage(*orig.pkg).errors;
  auto pkg = OpenPackage(file, st);
  auto deck = pkg ? sl::Read(std::move(pkg), st) : nullptr;
  if (!deck) {
    r.failure = Failure{"open", st.message};
    return r;
  }
  deck->LoadAll();
  orig.problems = deck->Problems();
  auto target = OpenDeckTarget(std::move(deck), st);
  if (!target) {
    r.failure = Failure{"open", st.message};
    return r;
  }
  std::vector<std::string> undoBase = Digest(target->Deck());  // where undoing everything leads
  Generator gen(seed);
  const size_t count = replay ? replay->size() : edits;
  for (size_t i = 0; i < count; ++i) {
    Edit e = replay ? (*replay)[i] : gen.Next(target->Deck());
    r.applied.push_back(e);
    if (!current.empty()) {
      std::ofstream cur(current, std::ios::trunc);
      cur << "file: " << AbsoluteUtf8(file) << "\nseed: " << seed << "\n";
      for (const Edit& a : r.applied) cur << FormatEdit(a) << "\n";
    }
    if (e.op == Op::Save) {
      if ((r.failure = CheckSave(orig, *target, r))) return r;
      if (!target->CanUndo()) undoBase = Digest(target->Deck());
      ++r.done;
      continue;
    }
    std::string why;
    try {
      if (target->Apply(e, why) == Applied::Done) ++r.done;
      else ++r.refused;
    } catch (const std::exception& ex) {
      r.failure = Failure{"edit", FormatEdit(e) + ": " + ex.what()};
      return r;
    }
  }
  // Undo everything: the deck as opened; redo it all: the deck as edited. (Up to the editor's 100
  // steps: a round with more doesn't get back to the start.)
  const std::vector<std::string> edited = Digest(target->Deck());
  if (r.done > 0 && r.done < 90) {
    const int steps = target->UndoAll();
    if (steps >= 0) {
      if (std::string diff = FirstDifference(undoBase, Digest(target->Deck())); !diff.empty()) {
        r.failure = Failure{"undo", "after undoing " + std::to_string(steps) + " steps: " + diff};
        return r;
      }
      if (!target->RedoSteps(steps)) {
        r.failure = Failure{"redo", "redoing " + std::to_string(steps) + " steps failed"};
        return r;
      }
      if (std::string diff = FirstDifference(edited, Digest(target->Deck())); !diff.empty()) {
        r.failure = Failure{"redo", "after redoing " + std::to_string(steps) + " steps: " + diff};
        return r;
      }
    }
  }
  r.failure = CheckSave(orig, *target, r);
  return r;
}

// Shrinks the edits to the fewest that still fail the same way (delta debugging, bounded).
std::vector<Edit> Minimize(const fs::path& file, uint64_t seed, std::vector<Edit> edits, const std::string& category) {
  auto fails = [&](const std::vector<Edit>& try_) {
    Round r = RunRound(file, seed, 0, &try_, {});
    return r.failure && r.failure->category == category;
  };
  size_t chunks = 2;
  for (int tries = 0; edits.size() > 1 && tries < 200;) {
    size_t size = (edits.size() + chunks - 1) / chunks;
    bool removed = false;
    for (size_t start = 0; start < edits.size() && tries < 200; start += size, ++tries) {
      std::vector<Edit> rest(edits.begin(), edits.begin() + start);
      rest.insert(rest.end(), edits.begin() + std::min(edits.size(), start + size), edits.end());
      if (fails(rest)) {
        edits = std::move(rest);
        chunks = std::max<size_t>(chunks - 1, 2);
        removed = true;
        break;
      }
    }
    if (!removed) {
      if (size == 1) break;
      chunks = std::min(chunks * 2, edits.size());
    }
  }
  return edits;
}

void WriteEdits(const fs::path& path, const fs::path& file, uint64_t seed, uint32_t round, const std::string& failure,
                const std::vector<Edit>& edits) {
  std::string sha = "unknown";
  if (auto bytes = ReadBytes(file); !bytes.empty()) sha = Hex(fw::pkg::HashBytes(bytes));
  std::ofstream txt(path, std::ios::trunc);
  txt << "# fwtool torture reproducer (deck): fwtool torture --replay " << fw::WideToUtf8(path.generic_wstring()) << "\n";
  txt << "file: " << AbsoluteUtf8(file) << "\n";
  txt << "sha256: " << sha << "\nseed: " << seed << "\nround: " << round << "\nfailure: " << failure << "\n";
  txt << "edits: " << edits.size() << "\n";
  for (const Edit& e : edits) txt << FormatEdit(e) << "\n";
}

// A reproducer names its file by the path it had where it was made; elsewhere, by the longest
// tail of that path that exists from the current folder.
fs::path FindReproFile(const fs::path& recorded) {
  std::error_code ec;
  if (fs::exists(recorded, ec)) return recorded;
  std::vector<fs::path> parts(recorded.relative_path().begin(), recorded.relative_path().end());
  for (size_t first = 1; first < parts.size(); ++first) {
    fs::path tail;
    for (size_t i = first; i < parts.size(); ++i) tail /= parts[i];
    if (fs::exists(tail, ec)) return tail;
  }
  return recorded;
}

bool ReadReproducer(const fs::path& repro, fs::path& file, uint64_t& seed, std::string& sha, std::vector<Edit>* edits,
                    std::string& error) {
  std::ifstream in(repro);
  if (!in) {
    error = "can't read " + Display(repro);
    return false;
  }
  std::string line;
  for (bool first = true; std::getline(in, line); first = false) {
    if (first && line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3);  // a UTF-8 BOM (PowerShell 5.1 writes one)
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    if (line.starts_with("file: ")) file = fs::path(fw::Utf8ToWide(line.substr(6)));
    else if (line.starts_with("seed: ")) seed = std::strtoull(line.c_str() + 6, nullptr, 10);
    else if (line.starts_with("sha256: ")) sha = line.substr(8);
    else if (line.starts_with("round: ") || line.starts_with("failure: ") || line.starts_with("edits: ")) continue;
    else if (edits) {
      Edit e;
      if (!ParseEdit(line, e)) {
        error = "can't read the edit \"" + line + "\"";
        return false;
      }
      edits->push_back(std::move(e));
    }
  }
  return true;
}

}  // namespace

std::unique_ptr<DeckTarget> OpenDeckTarget(std::unique_ptr<sl::Presentation> deck, fw::pkg::Status& status) {
  if (!deck) {
    status = fw::pkg::Status::Fail(fw::pkg::Status::Code::Unsupported, "no presentation");
    return nullptr;
  }
#if FW_SLIDE_EDIT
  return std::make_unique<EditTarget>(std::move(deck));
#else
  return std::make_unique<ReadOnlyTarget>(std::move(deck));
#endif
}

}  // namespace decktorture

using namespace decktorture;

bool IsDeckFile(const std::filesystem::path& file) {
  const std::wstring ext = fw::ToLower(file.extension().wstring());
  for (const wchar_t* known : {L".pptx", L".pptm", L".potx", L".potm", L".ppsx", L".ppsm"})
    if (ext == known) return true;
  return false;
}

bool IsDeckReproducer(const std::filesystem::path& repro) {
  fs::path file;
  uint64_t seed = 0;
  std::string sha, error;
  return ReadReproducer(repro, file, seed, sha, nullptr, error) && IsDeckFile(file);
}

int ReplayDeck(const std::filesystem::path& repro, const std::filesystem::path& outDir) {
  fs::path file;
  uint64_t seed = 0;
  std::string sha, error;
  std::vector<Edit> edits;
  if (!ReadReproducer(repro, file, seed, sha, &edits, error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 2;
  }
  file = FindReproFile(file);
  if (auto bytes = ReadBytes(file); !bytes.empty() && !sha.empty() && sha != "unknown" && Hex(fw::pkg::HashBytes(bytes)) != sha)
    std::fprintf(stderr, "Note: %s isn't the file this reproducer was made from (its SHA-256 differs).\n", Display(file).c_str());
  Round r = RunRound(file, seed, 0, &edits, {});
  if (!r.saved.empty()) {
    fs::path made = (outDir.empty() ? repro.parent_path() : outDir) / repro.stem();
    std::error_code ec;
    if (!outDir.empty()) fs::create_directories(outDir, ec);
    fs::path saved = made;
    saved += "-replay" + file.extension().string();
    WriteBytes(saved, r.saved);
    if (!r.edited.empty()) {
      fs::path a = made, b = made;
      a += "-replay-edited.txt", b += "-replay-readback.txt";
      WriteBytes(a, std::vector<uint8_t>(r.edited.begin(), r.edited.end()));
      WriteBytes(b, std::vector<uint8_t>(r.readBack.begin(), r.readBack.end()));
    }
  }
  std::string written;
  for (const std::string& part : r.written) written += (written.empty() ? "" : ", ") + part;
  if (r.failure)
    std::printf("[FAIL] %s: %zu edits (%u done, %u refused): %s: %s\n", Display(file).c_str(), edits.size(), r.done,
                r.refused, r.failure->category.c_str(), r.failure->message.c_str());
  else
    std::printf("[ ok ] %s: %zu edits (%u done, %u refused)\n", Display(file).c_str(), edits.size(), r.done, r.refused);
  std::printf("       parts written: %s\n", written.empty() ? "none" : written.c_str());
  return r.failure ? 1 : 0;
}

int RunDeckTorture(const Options& options, const std::vector<std::filesystem::path>& files, bool appendPairs) {
  const uint64_t baseSeed = options.seed ? options.seed : 1;
  const uint32_t edits = static_cast<uint32_t>(options.edits), rounds = options.rounds;
  const fs::path out = options.outDir.empty() ? fs::path("build") / "torture" : options.outDir;
  std::error_code ec;
  for (const char* dir : {"saved", "unchanged", "repro"}) fs::create_directories(out / dir, ec);
  // original, saved copy: for the validator and the PowerPoint open check (scripts\deck-torture.ps1)
  std::ofstream pairs(out / "pairs.tsv", appendPairs ? std::ios::app : std::ios::trunc);
  std::string json = "{\n  \"kind\": \"decks\", \"seed\": " + std::to_string(baseSeed) + ", \"edits\": " +
                     std::to_string(edits) + ", \"rounds\": " + std::to_string(rounds) + ",\n  \"files\": [";
  int failedFiles = 0, unreadFiles = 0, identicalFiles = 0;
  uint64_t totalDone = 0, totalRefused = 0;
  std::string targetName;
  for (size_t fi = 0; fi < files.size(); ++fi) {
    const fs::path& file = files[fi];
    const std::string name = Display(file);
    std::string fileJson = "\n    {\"file\": " + JsonText(AbsoluteUtf8(file)) + ", \"failures\": [";
    uint32_t done = 0, refused = 0, failures = 0;
    bool unread = false, identical = false;
    std::string note, savedPath, unchangedPath;
    auto fail = [&](uint32_t round, uint64_t seed, const Failure& f, const std::vector<Edit>& applied) {
      std::vector<Edit> kept = applied.empty() ? applied : Minimize(file, seed, applied, f.category);
      const fs::path repro = out / "repro" / (SafeName(file) + "-round" + std::to_string(round) + ".txt");
      WriteEdits(repro, file, seed, round, f.category + ": " + f.message, kept);
      std::printf("[FAIL] %s: %s: %s: %s\n       reproducer (%zu of %zu edits): %s\n", name.c_str(),
                  applied.empty() ? "saved without edits" : ("round " + std::to_string(round)).c_str(), f.category.c_str(),
                  f.message.c_str(), kept.size(), applied.size(), Display(repro).c_str());
      fileJson += std::string(failures ? "," : "") + "\n      {\"round\": " + std::to_string(round) + ", \"seed\": " +
                  std::to_string(seed) + ", \"category\": " + JsonText(f.category) + ", \"message\": " + JsonText(f.message) +
                  ", \"edits\": " + std::to_string(kept.size()) + ", \"reproducer\": " + JsonText(AbsoluteUtf8(repro)) + "}";
      ++failures;
    };
    // Saved without edits first (round 0 of the reproducers, no edits): nothing written, every part
    // as it was; the copy goes to <out>\unchanged for PowerPoint and the PDF comparison.
    {
      const std::vector<Edit> none;
      Round base = RunRound(file, 0, 0, &none, {});
      if (base.failure && base.failure->category == "open") {
        unread = true;
        note = base.failure->message;
      } else {
        if (!base.saved.empty()) {
          const fs::path u = out / "unchanged" / (SafeName(file) + file.extension().string());
          WriteBytes(u, base.saved);
          unchangedPath = AbsoluteUtf8(u);
        }
        identical = base.identical;
        if (base.failure) fail(0, 0, *base.failure, {});
      }
    }
    for (uint32_t round = 0; round < rounds && !unread && edits > 0; ++round) {
      const uint64_t seed = RoundSeed(baseSeed, fw::WideToUtf8(file.filename().wstring()), round);
      Round r = RunRound(file, seed, edits, nullptr, out / "current-edits.txt");
      done += r.done, refused += r.refused;
      if (r.failure && r.failure->category == "open") {
        unread = true;
        note = r.failure->message;
        break;
      }
      if (targetName.empty()) {
        fw::pkg::Status st;
        if (auto pkg = OpenPackage(file, st))
          if (auto deck = fw::slide::Read(std::move(pkg), st)) targetName = OpenDeckTarget(std::move(deck), st)->Name();
      }
      if (!r.saved.empty() && savedPath.empty()) {
        const fs::path saved = out / "saved" / (SafeName(file) + file.extension().string());
        WriteBytes(saved, r.saved);
        // Its edits, so that what the validator or PowerPoint finds in it can be replayed.
        WriteEdits(out / "saved" / (SafeName(file) + ".edits.txt"), file, seed, round,
                   r.failure ? r.failure->category + ": " + r.failure->message : "none", r.applied);
        savedPath = AbsoluteUtf8(saved);
        pairs << AbsoluteUtf8(file) << "\t" << savedPath << "\n";
      }
      if (r.failure) fail(round, seed, *r.failure, r.applied);
    }
    totalDone += done, totalRefused += refused;
    if (identical) ++identicalFiles;
    if (unread) {
      ++unreadFiles;
      std::printf("[skip] %s: %s\n", name.c_str(), note.c_str());
    } else if (!failures) {
      std::printf("[ ok ] %s: saved unchanged%s; %u rounds, %u edits done, %u refused\n", name.c_str(),
                  identical ? " (the same bytes)" : " (the same parts)", edits ? rounds : 0, done, refused);
    } else {
      ++failedFiles;
    }
    fileJson += std::string(failures ? "\n    " : "") + "], \"done\": " + std::to_string(done) + ", \"refused\": " +
                std::to_string(refused) + ", \"skipped\": " + (unread ? JsonText(note) : "null") +
                ", \"identical\": " + (identical ? "true" : "false") +
                ", \"unchanged\": " + (unchangedPath.empty() ? "null" : JsonText(unchangedPath)) +
                ", \"saved\": " + (savedPath.empty() ? "null" : JsonText(savedPath)) + "}";
    json += fileJson + (fi + 1 < files.size() ? "," : "");
  }
  json += "\n  ]\n}\n";
  fs::path jsonPath = options.jsonOut;
  if (!jsonPath.empty() && appendPairs) jsonPath.replace_filename(jsonPath.stem().wstring() + L"-decks.json");
  if (!jsonPath.empty()) std::ofstream(jsonPath, std::ios::trunc) << json;
  fs::remove(out / "current-edits.txt", ec);
  std::printf("\n%zu decks, %d failed, %d not read by the model; %d saved without edits byte for byte; %llu edits done, "
              "%llu refused (edits through: %s)\n",
              files.size(), failedFiles, unreadFiles, identicalFiles, static_cast<unsigned long long>(totalDone),
              static_cast<unsigned long long>(totalRefused), targetName.empty() ? "?" : targetName.c_str());
  return failedFiles == 0 ? 0 : 1;
}

}  // namespace fwtool
