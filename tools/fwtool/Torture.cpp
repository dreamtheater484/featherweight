// fwtool torture: the "open, edit and save" never-damage test (plan §10), D2's exit gate. Each Word
// document gets rounds of seeded random edits through the editor, aimed at what breaks files:
// deletes across fields, bookmarks, comments, content controls, tables and tracked changes,
// paragraph splits and joins, formatting, typing, undo and redo. After each round the document is
// saved, the saved file must open, pass fwtool check (no errors the original didn't have) and read
// back into the model as the edited document. A failure is shrunk to the fewest edits that still
// fail and kept as a reproducer. scripts\torture.ps1 then runs the validator and Word on the saved
// copies.
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>

#include "Common.h"
#include "Torture.h"
#include "core/Strings.h"
#include "doc/docx/Docx.h"
#include "doc/model/Schema.h"
#include "doc/model/Utf.h"
#include "pkg/Check.h"
#include "pkg/Snapshot.h"

// The editor arrives with D2 (doc/edit/Editor.h); until then the harness runs without it.
#if __has_include("doc/edit/Editor.h")
#include "doc/edit/Editor.h"
#define FW_TORTURE_EDITOR 1
#else
#define FW_TORTURE_EDITOR 0
#endif

namespace fwtool {

namespace torture {

namespace {

std::string PosText(fw::doc::Pos p) { return std::to_string(p.para) + ":" + std::to_string(p.offset); }

bool ParsePos(std::string_view s, fw::doc::Pos& p) {
  size_t colon = s.find(':');
  if (colon == std::string_view::npos) return false;
  p.para = static_cast<uint32_t>(std::strtoul(std::string(s.substr(0, colon)).c_str(), nullptr, 10));
  p.offset = static_cast<uint32_t>(std::strtoul(std::string(s.substr(colon + 1)).c_str(), nullptr, 10));
  return true;
}

// Typed text in a reproducer: UTF-8 in quotes, with \t, \", \\ and \uXXXX for other control characters.
std::string Quote(std::u16string_view text) {
  std::string out = "\"";
  for (char16_t c : text) {
    if (c == u'"') out += "\\\"";
    else if (c == u'\\') out += "\\\\";
    else if (c == u'\t') out += "\\t";
    else if (c < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\u%04X", static_cast<unsigned>(c));
      out += buf;
    } else fw::doc::AppendUtf8(out, std::u16string_view(&c, 1));
  }
  return out + "\"";
}

bool Unquote(std::string_view s, std::u16string& out) {
  if (s.size() < 2 || s.front() != '"' || s.back() != '"') return false;
  std::string utf8;
  for (size_t i = 1; i + 1 < s.size(); ++i) {
    if (s[i] != '\\' || i + 2 >= s.size()) {
      utf8 += s[i];
      continue;
    }
    char e = s[++i];
    if (e == 't') utf8 += '\t';
    else if (e == 'u' && i + 4 < s.size()) {
      char16_t c = static_cast<char16_t>(std::strtoul(std::string(s.substr(i + 1, 4)).c_str(), nullptr, 16));
      fw::doc::AppendUtf8(utf8, std::u16string_view(&c, 1));
      i += 4;
    } else utf8 += e;
  }
  out = fw::doc::ToUtf16(utf8);
  return true;
}

}  // namespace

std::string FormatEdit(const Edit& e) {
  std::string line;
  switch (e.kind) {
    case EditKind::Type: line = "type " + PosText(e.at) + " " + Quote(e.text); break;
    case EditKind::Delete: line = "delete " + PosText(e.at) + " " + PosText(e.to); break;
    case EditKind::Split: line = "split " + PosText(e.at); break;
    case EditKind::Join: line = "join " + std::to_string(e.at.para); break;
    case EditKind::Format: line = "format " + PosText(e.at) + " " + PosText(e.to) + " " + e.format; break;
    case EditKind::Undo: line = "undo " + std::to_string(e.count); break;
    case EditKind::Redo: line = "redo " + std::to_string(e.count); break;
  }
  if (!e.target.empty()) line += "  # " + e.target;
  return line;
}

bool ParseEdit(std::string_view line, Edit& e) {
  e = Edit{};
  // The comment after the edit names its target; quoted text may contain '#'.
  size_t hash = std::string_view::npos;
  bool quoted = false;
  for (size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '\\' && quoted) ++i;
    else if (line[i] == '"') quoted = !quoted;
    else if (line[i] == '#' && !quoted) {
      hash = i;
      break;
    }
  }
  if (hash != std::string_view::npos) {
    std::string_view t = line.substr(hash + 1);
    while (!t.empty() && t.front() == ' ') t.remove_prefix(1);
    e.target = std::string(t);
    line = line.substr(0, hash);
  }
  while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) line.remove_suffix(1);
  auto word = [&]() {
    while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
    size_t end = line.front() == '"' ? line.size() : std::min(line.find(' '), line.size());
    std::string_view w = line.substr(0, end);
    line.remove_prefix(end);
    return w;
  };
  if (line.empty()) return false;
  std::string_view verb = word();
  if (verb == "type") {
    e.kind = EditKind::Type;
    return ParsePos(word(), e.at) && Unquote(word(), e.text);
  }
  if (verb == "delete" || verb == "format") {
    e.kind = verb == "delete" ? EditKind::Delete : EditKind::Format;
    if (!ParsePos(word(), e.at) || !ParsePos(word(), e.to)) return false;
    if (e.kind == EditKind::Format) e.format = std::string(word());
    return e.kind == EditKind::Delete || !e.format.empty();
  }
  if (verb == "split") {
    e.kind = EditKind::Split;
    return ParsePos(word(), e.at);
  }
  if (verb == "join") {
    e.kind = EditKind::Join;
    e.at.para = static_cast<uint32_t>(std::strtoul(std::string(word()).c_str(), nullptr, 10));
    return true;
  }
  if (verb == "undo" || verb == "redo") {
    e.kind = verb == "undo" ? EditKind::Undo : EditKind::Redo;
    e.count = static_cast<uint32_t>(std::strtoul(std::string(word()).c_str(), nullptr, 10));
    return e.count > 0;
  }
  return false;
}

namespace {

// Saves as the app does: the given parts written from the model into the package, then the package.
fw::pkg::Status SaveParts(fw::doc::Document& doc, const std::vector<uint32_t>& parts, std::vector<uint8_t>& out) {
  fw::pkg::Status st;
  for (uint32_t part : parts) {
    std::string xml;
    if (!(st = fw::doc::docx::WritePart(doc, part, xml)).Ok()) return st;
    doc.Package().SetPart(doc.Parts()[part].name, std::vector<uint8_t>(xml.begin(), xml.end()));
  }
  fw::pkg::MemorySink sink;
  if (!(st = doc.Package().Save(sink)).Ok()) return st;
  out = std::move(sink.bytes);
  return st;
}

#if FW_TORTURE_EDITOR
// The editor (D2). Edits whose positions don't exist in the document (from a shrunk or replayed
// list) are refused here, so that the editor only sees edits a person could make.
class EditorTarget : public EditTarget {
 public:
  explicit EditorTarget(std::unique_ptr<fw::doc::Document> doc) : doc_(std::move(doc)), editor_(*doc_) {}
  const char* Name() const override { return "the editor (doc/edit/Editor.h)"; }
  const fw::doc::Document& Doc() const override { return *doc_; }

  Applied Apply(const Edit& e, std::string& why) override {
    namespace ed = fw::doc::edit;
    auto exists = [&](fw::doc::Pos p) { return p.para < doc_->Paragraphs().size() && p.offset <= editor_.ParaLength(p.para); };
    auto refuse = [&](const std::string& reason) {
      why = reason;
      return Applied::Refused;
    };
    ed::Result r;
    switch (e.kind) {
      case EditKind::Type:
        if (!exists(e.at)) return refuse("no such position");
        r = editor_.InsertText(e.at, e.text);
        break;
      case EditKind::Delete:
        if (!exists(e.at) || !exists(e.to) || editor_.Compare(e.at, e.to) > 0) return refuse("no such range");
        r = editor_.Delete(e.at, e.to);
        break;
      case EditKind::Split:
        if (!exists(e.at)) return refuse("no such position");
        r = editor_.SplitParagraph(e.at);
        break;
      case EditKind::Join:
        if (e.at.para >= doc_->Paragraphs().size()) return refuse("no such paragraph");
        r = editor_.JoinParagraphs(e.at.para);
        break;
      case EditKind::Format: {
        if (!exists(e.at) || !exists(e.to) || editor_.Compare(e.at, e.to) > 0) return refuse("no such range");
        size_t eq = e.format.find('=');
        std::string name = e.format.substr(0, eq), value = eq == std::string::npos ? "" : e.format.substr(eq + 1);
        bool needsValue = name == "align" || name == "style" || name == "size" || name == "color" || name == "highlight";
        if (needsValue == value.empty()) return refuse("format " + name + (needsValue ? " needs =value" : " takes no value"));
        ed::PropEdit props;
        if (name == "align") {
          r = editor_.SetParaProps(e.at.para, e.to.para, props.Set("jc", {{"val", value}}));
        } else if (name == "style") {
          r = editor_.SetParaProps(e.at.para, e.to.para, props.Set("pStyle", {{"val", value}}));
        } else {
          if (name == "bold") props.Set("b");
          else if (name == "italic") props.Set("i");
          else if (name == "strike") props.Set("strike");
          else if (name == "underline") props.Set("u", {{"val", "single"}});
          else if (name == "size") props.Set("sz", {{"val", value}});
          else if (name == "color") props.Set("color", {{"val", value}});
          else if (name == "highlight") props.Set("highlight", {{"val", value}});
          else if (name == "plain") props.Remove("b").Remove("i").Remove("u");
          else return refuse("unknown format");
          r = editor_.SetRunProps(e.at, e.to, props);
        }
        break;
      }
      case EditKind::Undo:
      case EditKind::Redo:
        for (uint32_t i = 0; i < e.count; ++i) {
          bool undo = e.kind == EditKind::Undo;
          if (undo ? !editor_.CanUndo() : !editor_.CanRedo()) return i ? Applied::Done : refuse("nothing to undo or redo");
          if (!(r = undo ? editor_.Undo() : editor_.Redo())) break;
        }
        break;
    }
    if (!r) {
      why = fw::doc::ToUtf8(r.message);
      return Applied::Refused;
    }
    return Applied::Done;
  }

  fw::pkg::Status Save(std::vector<uint8_t>& out) override { return SaveParts(*doc_, editor_.DirtyParts(), out); }

 private:
  std::unique_ptr<fw::doc::Document> doc_;
  fw::doc::edit::Editor editor_;
};
#endif

// Without the editor: the document as read, nothing applied, every part the model owns written.
class ModelOnlyTarget : public EditTarget {
 public:
  explicit ModelOnlyTarget(std::unique_ptr<fw::doc::Document> doc) : doc_(std::move(doc)) {}
  const char* Name() const override { return "model only (no editor yet)"; }
  const fw::doc::Document& Doc() const override { return *doc_; }
  Applied Apply(const Edit&, std::string& why) override {
    why = "no editor";
    return Applied::Refused;
  }
  fw::pkg::Status Save(std::vector<uint8_t>& out) override {
    std::vector<uint32_t> all;
    for (uint32_t i = 0; i < doc_->Parts().size(); ++i)
      if (doc_->Parts()[i].writable) all.push_back(i);
    return SaveParts(*doc_, all, out);
  }

 private:
  std::unique_ptr<fw::doc::Document> doc_;
};

}  // namespace

std::unique_ptr<EditTarget> OpenTarget(std::unique_ptr<fw::doc::Document> doc, fw::pkg::Status& status) {
  status = {};
#if FW_TORTURE_EDITOR
  return std::make_unique<EditorTarget>(std::move(doc));
#else
  return std::make_unique<ModelOnlyTarget>(std::move(doc));
#endif
}

}  // namespace torture

namespace {

using namespace fw::doc;
using torture::Applied;
using torture::Edit;
using torture::EditKind;
using torture::EditTarget;
namespace fs = std::filesystem;

// SplitMix64: the same draws on every compiler and machine, so a seed always means the same edits.
struct Rng {
  uint64_t state;
  explicit Rng(uint64_t seed) : state(seed) {}
  uint64_t Next() {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  uint32_t Below(uint32_t n) { return n ? static_cast<uint32_t>(Next() % n) : 0; }
  uint32_t Between(uint32_t lo, uint32_t hi) { return hi <= lo ? lo : lo + Below(hi - lo + 1); }
  bool Percent(uint32_t p) { return Below(100) < p; }
  template <class T> const T& Pick(const std::vector<T>& v) { return v[Below(static_cast<uint32_t>(v.size()))]; }
};

// Each file and round gets its own seed from the run's seed and the file's name.
uint64_t RoundSeed(uint64_t seed, const std::string& name, uint32_t round) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : name) h = (h ^ c) * 1099511628211ull;
  Rng mix(seed ^ h ^ (static_cast<uint64_t>(round) << 32));
  return mix.Next();
}

// A stretch of a story the edits aim at: a field, a bookmark, a comment, a content control...
struct Span {
  std::string what;
  uint32_t story = kNone;
  Pos a, b;  // b is just past its end
};

class Generator {
 public:
  Generator(const Document& doc, Rng& rng) : doc_(doc), rng_(rng) {}

  // The next edit for the document as it is now. `undone` is how many edits can be redone.
  Edit Next(uint32_t applied, uint32_t undone) {
    Index();
    Edit e;
    if (stories_.empty()) {
      e.kind = EditKind::Undo;
      return e;
    }
    uint32_t roll = rng_.Below(100);
    if (roll < 4 && applied) {
      e.kind = EditKind::Undo;
      e.count = rng_.Between(1, std::min<uint32_t>(applied, 4));
    } else if (roll < 7 && undone) {
      e.kind = EditKind::Redo;
      e.count = rng_.Between(1, std::min<uint32_t>(undone, 4));
    } else if (roll < 40) {
      Structural(e);
    } else if (roll < 62) {
      e.kind = EditKind::Type;
      e.at = RandomPos(RandomPara());
      e.text = RandomText();
    } else if (roll < 74) {
      PlainDelete(e);
    } else if (roll < 82) {
      e.kind = EditKind::Split;
      e.at = RandomPos(RandomPara());
    } else if (roll < 88) {
      Join(e);
    } else {
      Formatting(e);
    }
    return e;
  }

 private:
  // The editable stories and the order of their paragraphs.
  void Index() {
    stories_.clear();
    order_.clear();
    paras_.clear();
    for (uint32_t s = 0; s < doc_.Stories().size(); ++s) {
      const Story& story = doc_.Stories()[s];
      if (story.part >= doc_.Parts().size() || !doc_.Parts()[story.part].Editable()) continue;
      std::vector<uint32_t> ps = doc_.StoryParagraphs(s);
      std::vector<uint32_t> keep;
      for (uint32_t p : ps)
        if (!doc_.ParaNode(p).DisplayOnly()) keep.push_back(p);
      if (keep.empty()) continue;
      for (size_t i = 0; i < keep.size(); ++i) order_[keep[i]] = {s, static_cast<uint32_t>(i)};
      stories_.push_back(s);
      paras_[s] = std::move(keep);
    }
  }
  uint32_t Len(uint32_t para) const { return static_cast<uint32_t>(doc_.Para(para).text.size()); }
  // Mostly the main text; the other stories now and then.
  uint32_t RandomStory() {
    uint32_t main = doc_.MainStory();
    if (paras_.contains(main) && (stories_.size() == 1 || rng_.Percent(75))) return main;
    return rng_.Pick(stories_);
  }
  uint32_t RandomPara() { return rng_.Pick(paras_[RandomStory()]); }
  Pos RandomPos(uint32_t para) { return Pos{para, rng_.Between(0, Len(para))}; }
  // A position up to `reach` paragraphs after p in its story, at or after p.
  Pos Later(Pos p, uint32_t reach) {
    auto [story, index] = order_[p.para];
    const auto& ps = paras_[story];
    uint32_t to = std::min<uint32_t>(index + rng_.Between(0, reach), static_cast<uint32_t>(ps.size()) - 1);
    if (to == index) return Pos{p.para, rng_.Between(p.offset, Len(p.para))};
    return RandomPos(ps[to]);
  }
  bool Known(Pos p) const { return p.para != kNone && order_.contains(p.para) && p.offset <= Len(p.para); }
  bool Before(Pos a, Pos b) const {
    auto ia = order_.at(a.para), ib = order_.at(b.para);
    return ia.second != ib.second ? ia.second < ib.second : a.offset < b.offset;
  }

  std::u16string RandomText() {
    static const std::vector<std::u16string> pieces = {
        u"a", u"e", u"the ", u"word ", u"Featherweight", u"  ", u" ", u".", u", ", u"12", u"é", u"ß", u"ü", u"ñ",
        u"ç", u"€", u"—", u"“quoted”", u"日本", u"\t", u"o"};
    std::u16string t;
    for (uint32_t n = rng_.Between(1, 4); n; --n) t += rng_.Pick(pieces);
    return t;
  }

  void PlainDelete(Edit& e) {
    e.kind = EditKind::Delete;
    e.at = RandomPos(RandomPara());
    e.to = Later(e.at, rng_.Percent(70) ? 0 : 3);
    if (e.to == e.at && Len(e.at.para) > e.at.offset) e.to.offset = e.at.offset + 1;
  }

  // Joining two paragraphs is deleting the first one's mark.
  void Join(Edit& e) {
    uint32_t story = RandomStory();
    const auto& ps = paras_[story];
    if (ps.size() < 2) return PlainDelete(e);
    uint32_t i = rng_.Below(static_cast<uint32_t>(ps.size()) - 1);
    if (rng_.Percent(50)) {
      e.kind = EditKind::Join;
      e.at = Pos{ps[i], 0};
      return;
    }
    e.kind = EditKind::Delete;
    e.at = Pos{ps[i], Len(ps[i])};
    e.to = Pos{ps[i + 1], 0};
    e.target = "paragraph join";
  }

  void Formatting(Edit& e) {
    static const std::vector<std::string> run = {"bold", "italic", "underline", "strike", "size=16", "size=28",
                                                 "color=C00000", "color=1F4E79", "highlight=yellow", "plain"};
    static const std::vector<std::string> para = {"align=center", "align=right", "align=both", "align=left"};
    e.kind = EditKind::Format;
    e.at = RandomPos(RandomPara());
    e.to = Later(e.at, rng_.Percent(80) ? 0 : 2);
    if (rng_.Percent(80)) {
      e.format = rng_.Pick(run);
    } else if (rng_.Percent(50) && !doc_.Styles().empty()) {
      std::vector<std::string> styles;
      for (const Style& s : doc_.Styles())
        if (s.type == StyleType::Paragraph && !s.id.empty()) styles.emplace_back(s.id);
      e.format = styles.empty() ? rng_.Pick(para) : "style=" + rng_.Pick(styles);
    } else {
      e.format = rng_.Pick(para);
    }
  }

  // Everything the edits can aim at in the document as it is now.
  std::vector<Span> Spans() {
    std::vector<Span> spans;
    auto add = [&](std::string what, uint32_t story, Pos a, Pos b) {
      if (!Known(a) || !Known(b) || order_[a.para].first != order_[b.para].first) return;
      if (Before(b, a)) std::swap(a, b);
      spans.push_back({std::move(what), story, a, b});
    };
    for (uint32_t s : stories_) {
      for (const Field& f : doc_.Fields(s))
        if (f.complete) add("field " + ToUtf8(f.Type()), s, f.begin, Pos{f.end.para, f.end.offset + 1});
      for (const Revision& r : doc_.Revisions(s))
        if (r.kind <= RevisionKind::MoveTo) add("tracked change", s, r.start, r.end);
      // Inline content controls: the text of their runs; block ones: their paragraphs.
      for (uint32_t p : paras_[s]) {
        Node cc;
        uint32_t start = 0, end = 0;
        for (const TextRun& r : doc_.Runs(p)) {
          if (r.contentControl && r.contentControl == cc) {
            end = r.end;
            continue;
          }
          if (cc) add("content control", s, Pos{p, start}, Pos{p, end});
          cc = r.contentControl, start = r.start, end = r.end;
        }
        if (cc) add("content control", s, Pos{p, start}, Pos{p, end});
      }
      for (const Block& b : doc_.Blocks(s)) {
        if (b.kind == BlockKind::ContentControl) {
          std::vector<Block> inner = doc_.Blocks(b.node);
          uint32_t first = kNone, last = kNone;
          for (const Block& i : inner) {
            if (i.kind != BlockKind::Paragraph) continue;
            if (first == kNone) first = i.para;
            last = i.para;
          }
          if (first != kNone) add("block content control", s, Pos{first, 0}, Pos{last, Len(last)});
        } else if (b.kind == BlockKind::Table) {
          std::vector<uint32_t> cellParas;
          for (Node tr : doc_.TableRows(b.node))
            for (Node tc : doc_.RowCells(tr))
              for (const Block& i : doc_.Blocks(tc))
                if (i.kind == BlockKind::Paragraph) cellParas.push_back(i.para);
          if (cellParas.empty()) continue;
          add("table", s, Pos{cellParas.front(), 0}, Pos{cellParas.back(), Len(cellParas.back())});
          uint32_t c = rng_.Below(static_cast<uint32_t>(cellParas.size()));
          add("table cell", s, Pos{cellParas[c], 0}, Pos{cellParas[c], Len(cellParas[c])});
        }
      }
    }
    for (const PairInfo& pair : doc_.Pairs()) {
      const char* what = pair.kind == fw::pkg::RefKind::BookmarkId         ? "bookmark"
                         : pair.kind == fw::pkg::RefKind::CommentId        ? "comment range"
                         : pair.kind == fw::pkg::RefKind::PermissionId     ? "permission range"
                         : pair.kind == fw::pkg::RefKind::MoveRangeId      ? "move range"
                         : pair.kind == fw::pkg::RefKind::CustomXmlRangeId ? "custom XML range"
                                                                           : nullptr;
      if (what) add(std::string(what) + " " + std::string(pair.id), pair.story, pair.openPos, pair.closePos);
    }
    for (const CommentInfo& c : doc_.Comments())
      if (c.reference.para != kNone)
        add("comment " + std::string(c.id) + " reference", c.story, c.reference, Pos{c.reference.para, c.reference.offset + 1});
    return spans;
  }

  // Deletes across one end of a structure, around it or inside it; or types or splits inside it.
  void Structural(Edit& e) {
    std::vector<Span> spans = Spans();
    if (spans.empty()) return PlainDelete(e);
    const Span& s = rng_.Pick(spans);
    e.target = s.what;
    auto before = [&] { return Pos{s.a.para, rng_.Between(0, s.a.offset)}; };
    auto after = [&] { return Pos{s.b.para, rng_.Between(std::min(s.b.offset, Len(s.b.para)), Len(s.b.para))}; };
    auto inside = [&] {
      if (s.a.para == s.b.para) return Pos{s.a.para, rng_.Between(s.a.offset, s.b.offset)};
      return rng_.Percent(50) ? Pos{s.a.para, rng_.Between(s.a.offset, Len(s.a.para))} : Pos{s.b.para, rng_.Between(0, s.b.offset)};
    };
    switch (rng_.Below(6)) {
      case 0: e.kind = EditKind::Delete, e.at = before(), e.to = inside(); break;
      case 1: e.kind = EditKind::Delete, e.at = inside(), e.to = after(); break;
      case 2: e.kind = EditKind::Delete, e.at = before(), e.to = after(); break;
      case 3: e.kind = EditKind::Delete, e.at = inside(), e.to = inside(); break;
      case 4: e.kind = EditKind::Type, e.at = inside(), e.text = RandomText(); break;
      default: e.kind = EditKind::Split, e.at = inside(); break;
    }
    if (e.kind == EditKind::Delete && Before(e.to, e.at)) std::swap(e.at, e.to);
  }

  const Document& doc_;
  Rng& rng_;
  std::vector<uint32_t> stories_;
  std::unordered_map<uint32_t, std::vector<uint32_t>> paras_;               // story -> paragraphs
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> order_;       // paragraph -> story, index
};

// What a document says, for comparing the edited model with the saved file read back: each story's
// paragraphs with their style, text, and where the formatting changes (so run splits don't matter).
std::string Digest(const Document& doc) {
  static const char* kinds[] = {"main", "header", "footer", "footnote", "endnote", "comment", "text box"};
  // A text box counts while its drawing is still in a story that counts: one whose drawing was
  // deleted is gone from the file, though the edited model may keep its story.
  std::vector<bool> live(doc.Stories().size());
  for (uint32_t s = 0; s < live.size(); ++s) live[s] = doc.Stories()[s].kind != StoryKind::TextBox;
  for (bool grew = true; grew;) {
    grew = false;
    for (uint32_t s = 0; s < live.size(); ++s) {
      if (!live[s]) continue;
      for (uint32_t p : doc.StoryParagraphs(s))
        for (const InlineItem& item : doc.Items(p)) {
          if (item.kind != ItemKind::Drawing || item.drawing >= doc.Drawings().size()) continue;
          for (uint32_t box : doc.Drawings()[item.drawing].textBoxes)
            if (box < live.size() && !live[box]) live[box] = grew = true;
        }
    }
  }
  std::string main;
  std::vector<std::string> stories;
  uint32_t textBoxes = 0;
  for (uint32_t s = 0; s < doc.Stories().size(); ++s) {
    if (!live[s]) continue;
    const Story& story = doc.Stories()[s];
    std::string out = std::string("[") + kinds[static_cast<int>(story.kind)] + " " +
                      (story.kind == StoryKind::TextBox ? std::to_string(textBoxes++) : story.id) + "]\n";
    for (uint32_t p : doc.StoryParagraphs(s)) {
      const ParaProps& pp = doc.ResolvedPara(p);
      out += std::string(pp.style) + " " + std::to_string(static_cast<int>(pp.jc)) + " | ";
      std::string last;
      for (const TextRun& r : doc.Runs(p)) {
        const RunProps& rp = doc.ResolvedRun(p, r.run);
        std::string sig = std::string(rp.bold ? "b" : "") + (rp.italic ? "i" : "") + (rp.strike ? "s" : "") +
                          "u" + std::to_string(static_cast<int>(rp.underline)) + "z" + std::to_string(rp.size.Value());
        if (sig != last) out += "{" + std::to_string(r.start) + ":" + sig + "}";
        last = sig;
      }
      out += " " + ToUtf8(doc.Para(p).text) + "\n";
    }
    if (s == doc.MainStory()) main = std::move(out);
    else stories.push_back(std::move(out));
  }
  std::sort(stories.begin(), stories.end());  // after the main story, by kind and id
  for (const auto& s : stories) main += s;
  return main;
}

// The first line where two digests differ, both sides.
std::string FirstDifference(const std::string& a, const std::string& b) {
  size_t i = 0, line = 0;
  while (i < a.size() && i < b.size() && a[i] == b[i]) {
    if (a[i] == '\n') line = i + 1;
    ++i;
  }
  auto lineAt = [](const std::string& s, size_t from) {
    size_t end = s.find('\n', from);
    std::string l = s.substr(from, end == std::string::npos ? std::string::npos : end - from);
    return l.size() > 160 ? l.substr(0, 160) + "..." : l;
  };
  // The story it's in: the last "[kind id]" line at or before it.
  std::string story = "main";
  for (size_t at = line;; at = a.rfind('\n', at - 2) + 1) {
    if (a.compare(at, 1, "[") == 0) {
      story = lineAt(a, at);
      break;
    }
    if (at < 2) break;
  }
  return "in " + story + ": edited \"" + lineAt(a, line) + "\", read back \"" + lineAt(b, line) + "\"";
}

// Runs fn, catching crashes (access violations and the like) and C++ exceptions alike: a crash in
// the editor is a finding with a reproducer, not the end of the run.
using Thunk = void (*)(void*);
DWORD Guard(Thunk fn, void* context) {
  __try {
    fn(context);
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return GetExceptionCode();
  }
}
template <class F> DWORD Guarded(F& fn) {
  return Guard([](void* c) { (*static_cast<F*>(c))(); }, &fn);
}
std::string CrashText(DWORD code) {
  char buf[64];
  std::snprintf(buf, sizeof buf, code == 0xE06D7363 ? "a C++ exception" : "exception 0x%08lX", code);
  return buf;
}

struct Settings {
  uint64_t seed = 1;
  uint32_t edits = 40, rounds = 3;
  fs::path out;
  std::chrono::seconds minimizeBudget{90};
};

struct Failure {
  std::string category;  // crash, save, open, check, reopen, differs
  std::string message;
};

struct RoundResult {
  std::optional<Failure> failure;
  std::vector<Edit> applied;  // every edit tried, in order (refused ones too: they're part of the replay)
  uint32_t done = 0, refused = 0;
  std::vector<uint8_t> saved;
};

std::string Hex(const fw::pkg::Sha256& h) {
  static const char* digits = "0123456789abcdef";
  std::string s;
  for (uint8_t b : h) s += digits[b >> 4], s += digits[b & 15];
  return s;
}

// One round: open, apply the edits (generated, or replayed from `replay`), save and check.
RoundResult RunRound(const fs::path& file, uint64_t seed, uint32_t edits, const std::vector<Edit>* replay,
                     const fs::path& log) {
  RoundResult r;
  auto fail = [&](std::string category, std::string message) {
    if (!r.failure) r.failure = Failure{std::move(category), std::move(message)};
  };
  fw::pkg::Status st;
  auto pkg = OpenPackage(file, st);
  std::unique_ptr<Document> doc = pkg ? docx::Read(std::move(pkg), st) : nullptr;
  if (!doc) {
    fail("open", "the model refuses the original: " + st.message);
    return r;
  }
  std::vector<std::string> known = fw::pkg::CheckPackage(doc->Package()).errors;
  std::unique_ptr<EditTarget> target = torture::OpenTarget(std::move(doc), st);
  if (!target) {
    fail("open", "the editor refuses it: " + st.message);
    return r;
  }
  // The edits go to the log before they're applied, so that a crash that ends fwtool still leaves them.
  std::ofstream logFile;
  if (!log.empty()) logFile.open(log, std::ios::trunc);
  Rng rng(seed);
  Generator gen(target->Doc(), rng);
  uint32_t undoable = 0, undone = 0;
  size_t count = replay ? replay->size() : edits;
  for (size_t i = 0; i < count && !r.failure; ++i) {
    Edit e;
    DWORD crash = 0;
    if (replay) {
      e = (*replay)[i];
    } else {
      auto next = [&] { e = gen.Next(undoable, undone); };
      if ((crash = Guarded(next)) != 0) {
        fail("crash", "generating an edit: " + CrashText(crash));
        break;
      }
    }
    if (logFile) logFile << torture::FormatEdit(e) << std::endl;
    r.applied.push_back(e);
    Applied applied = Applied::Refused;
    std::string why;
    auto apply = [&] { applied = target->Apply(e, why); };
    if ((crash = Guarded(apply)) != 0) {
      fail("crash", CrashText(crash) + " in: " + torture::FormatEdit(e));
      break;
    }
    if (applied == Applied::Refused) {
      ++r.refused;
      continue;
    }
    ++r.done;
    if (e.kind == EditKind::Undo) undone += e.count, undoable -= std::min(undoable, e.count);
    else if (e.kind == EditKind::Redo) undoable += e.count, undone -= std::min(undone, e.count);
    else ++undoable, undone = 0;
  }
  if (r.failure) return r;

  std::string before;
  auto save = [&] {
    before = Digest(target->Doc());
    st = target->Save(r.saved);
  };
  if (DWORD crash = Guarded(save)) return fail("crash", CrashText(crash) + " while saving"), r;
  if (!st.Ok()) return fail("save", st.message), r;

  // The saved file opens, is consistent, and reads back as the edited document.
  auto again = fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(r.saved), fw::pkg::Limits::Default(), st);
  if (again) st = again->Verify();
  if (!st.Ok()) return fail("open", "the saved file doesn't open: " + st.message), r;
  for (const std::string& error : fw::pkg::CheckPackage(*again).errors)
    if (std::find(known.begin(), known.end(), error) == known.end()) return fail("check", error), r;
  std::unique_ptr<Document> reread;
  auto reopen = [&] { reread = docx::Read(std::move(again), st); };
  if (DWORD crash = Guarded(reopen)) return fail("crash", CrashText(crash) + " reading the saved file"), r;
  if (!reread) return fail("reopen", st.message), r;
  std::string after;
  auto digest = [&] { after = Digest(*reread); };
  if (DWORD crash = Guarded(digest)) return fail("crash", CrashText(crash) + " reading the saved file's text"), r;
  if (before != after) fail("differs", FirstDifference(before, after));
  return r;
}

// Delta debugging (Zeller's ddmin): the fewest edits that still fail the same way, within a time budget.
std::vector<Edit> Minimize(std::vector<Edit> edits, const std::function<bool(const std::vector<Edit>&)>& fails,
                           std::chrono::seconds budget) {
  auto deadline = std::chrono::steady_clock::now() + budget;
  size_t n = 2;
  while (edits.size() >= 2 && std::chrono::steady_clock::now() < deadline) {
    size_t chunk = (edits.size() + n - 1) / n;
    bool reduced = false;
    for (size_t start = 0; start < edits.size() && std::chrono::steady_clock::now() < deadline; start += chunk) {
      // Without this chunk.
      std::vector<Edit> rest(edits.begin(), edits.begin() + start);
      rest.insert(rest.end(), edits.begin() + std::min(start + chunk, edits.size()), edits.end());
      if (!rest.empty() && fails(rest)) {
        edits = std::move(rest);
        n = std::max<size_t>(n - 1, 2);
        reduced = true;
        break;
      }
    }
    if (!reduced) {
      if (n >= edits.size()) break;
      n = std::min(n * 2, edits.size());
    }
  }
  return edits;
}

// A file's name for what the run writes about it: its path, flattened, without the extension.
std::string SafeName(const fs::path& file) {
  std::string name = Display(fs::path(file).replace_extension());
  for (char& c : name)
    if (c == '/' || c == '\\' || c == ':' || c == ' ') c = '_';
  while (!name.empty() && (name.front() == '.' || name.front() == '_')) name.erase(name.begin());
  return name;
}

void WriteBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::string JsonText(std::string_view s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\', out += c;
    else if (static_cast<unsigned char>(c) < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
      out += buf;
    } else out += c;
  }
  return out + "\"";
}

// A failure, shrunk and kept: the edits as text (fwtool torture --replay reads them back) and the
// saved file they make.
std::string KeepReproducer(const fs::path& file, uint64_t seed, uint32_t round, const RoundResult& failed,
                           const Settings& set, size_t& kept) {
  const Failure& f = *failed.failure;
  auto same = [&](const std::vector<Edit>& edits) {
    RoundResult r = RunRound(file, seed, 0, &edits, {});
    return r.failure && r.failure->category == f.category;
  };
  std::vector<Edit> small = Minimize(failed.applied, same, set.minimizeBudget);
  RoundResult last = RunRound(file, seed, 0, &small, {});
  if (!last.failure) {  // shrinking lost it (a flaky failure): keep them all
    last = failed;
    small = failed.applied;
  }
  kept = small.size();
  char stem[64];
  std::snprintf(stem, sizeof stem, "-seed%016llx", static_cast<unsigned long long>(seed));
  fs::path base = set.out / "repro" / (SafeName(file) + stem);
  fs::path txtPath = base, docxPath = base;  // names keep their dots: extensions are added, not replaced
  txtPath += ".txt";
  docxPath += ".docx";
  std::error_code ec;
  fs::create_directories(base.parent_path(), ec);
  std::vector<uint8_t> original;
  std::string sha = "unknown";
  if (std::ifstream in{file, std::ios::binary}) {
    original.assign(std::istreambuf_iterator<char>(in), {});
    sha = Hex(fw::pkg::HashBytes(original));
  }
  std::ofstream txt(txtPath, std::ios::trunc);
  txt << "# fwtool torture reproducer: fwtool torture --replay " << fw::WideToUtf8(txtPath.generic_wstring()) << "\n";
  txt << "file: " << fw::WideToUtf8(fs::absolute(file).generic_wstring()) << "\n";
  txt << "sha256: " << sha << "\n";
  txt << "seed: " << seed << "\nround: " << round << "\n";
  txt << "failure: " << last.failure->category << ": " << last.failure->message << "\n";
  txt << "edits: " << small.size() << " of " << failed.applied.size() << "\n";
  for (const Edit& e : small) txt << torture::FormatEdit(e) << "\n";
  if (!last.saved.empty()) WriteBytes(docxPath, last.saved);
  return fw::WideToUtf8(txtPath.generic_wstring());
}

bool IsWordFile(const fs::path& file) {
  std::wstring ext = fw::ToLower(file.extension().wstring());
  return ext == L".docx" || ext == L".docm" || ext == L".dotx" || ext == L".dotm";
}

// A reproducer names its file by the path it had where it was made. Elsewhere (another checkout),
// the same file is found by the longest tail of that path that exists from the current folder.
fs::path FindReproFile(const fs::path& recorded) {
  std::error_code ec;
  if (fs::exists(recorded, ec)) return recorded;
  const fs::path relative = recorded.relative_path();
  std::vector<fs::path> parts(relative.begin(), relative.end());
  for (size_t first = 1; first < parts.size(); ++first) {
    fs::path tail;
    for (size_t i = first; i < parts.size(); ++i) tail /= parts[i];
    if (fs::exists(tail, ec)) return tail;
  }
  return recorded;
}

// Replays a reproducer. The file it makes goes next to it (<name>-replay.docx), or into --out.
int Replay(const fs::path& repro, const fs::path& outDir) {
  std::ifstream in(repro);
  if (!in) {
    std::fprintf(stderr, "Can't read %s\n", Display(repro).c_str());
    return 2;
  }
  fs::path file;
  uint64_t seed = 0;
  std::string sha;
  std::vector<Edit> edits;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    if (line.starts_with("file: ")) file = fs::path(fw::Utf8ToWide(line.substr(6)));
    else if (line.starts_with("seed: ")) seed = std::strtoull(line.c_str() + 6, nullptr, 10);
    else if (line.starts_with("sha256: ")) sha = line.substr(8);
    else if (line.starts_with("round: ") || line.starts_with("failure: ") || line.starts_with("edits: ")) continue;
    else {
      Edit e;
      if (!torture::ParseEdit(line, e)) {
        std::fprintf(stderr, "Can't read the edit \"%s\"\n", line.c_str());
        return 2;
      }
      edits.push_back(std::move(e));
    }
  }
  file = FindReproFile(file);
  if (std::ifstream original{file, std::ios::binary}; original && !sha.empty() && sha != "unknown") {
    std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(original), {});
    if (Hex(fw::pkg::HashBytes(bytes)) != sha)
      std::fprintf(stderr, "Note: %s isn't the file this reproducer was made from (its SHA-256 differs).\n",
                   Display(file).c_str());
  }
  RoundResult r = RunRound(file, seed, 0, &edits, {});
  if (!r.saved.empty()) {
    fs::path made = outDir.empty() ? repro.parent_path() / repro.stem() : outDir / repro.stem();
    made += "-replay.docx";
    if (!outDir.empty()) {
      std::error_code ec;
      fs::create_directories(outDir, ec);
    }
    WriteBytes(made, r.saved);
  }
  if (r.failure) {
    std::printf("[FAIL] %s: %zu edits (%u done, %u refused): %s: %s\n", Display(file).c_str(), edits.size(), r.done,
                r.refused, r.failure->category.c_str(), r.failure->message.c_str());
    return 1;
  }
  std::printf("[ ok ] %s: %zu edits (%u done, %u refused)\n", Display(file).c_str(), edits.size(), r.done, r.refused);
  return 0;
}

}  // namespace

int RunTorture(const Options& options) {
  Settings set;
  set.seed = options.seed;
  set.edits = options.edits;
  set.rounds = options.rounds;
  if (options.replay) return Replay(options.inputs.front(), options.outDir);
  set.out = options.outDir.empty() ? fs::path("build") / "torture" : options.outDir;

  std::vector<fs::path> files;
  for (const auto& f : CollectFiles(options.inputs))
    if (IsWordFile(f)) files.push_back(f);
  if (files.empty()) {
    std::fprintf(stderr, "No Word documents found.\n");
    return 1;
  }
  std::error_code ec;
  fs::create_directories(set.out / "saved", ec);
  std::ofstream pairs(set.out / "pairs.tsv", std::ios::trunc);  // original, saved copy: for the validator and Word
  std::string json = "{\n  \"seed\": " + std::to_string(set.seed) + ", \"edits\": " + std::to_string(set.edits) +
                     ", \"rounds\": " + std::to_string(set.rounds) + ",\n  \"files\": [";
  int failedFiles = 0, refusedFiles = 0;
  uint64_t totalDone = 0, totalRefused = 0;
  const char* targetName = nullptr;
  for (size_t fi = 0; fi < files.size(); ++fi) {
    const fs::path& file = files[fi];
    std::string name = Display(file);
    std::string fileJson = "\n    {\"file\": " + JsonText(fw::WideToUtf8(fs::absolute(file).generic_wstring())) + ", \"failures\": [";
    uint32_t done = 0, refused = 0, failures = 0;
    bool unreadable = false;
    std::string note, savedPath;
    for (uint32_t round = 0; round < set.rounds; ++round) {
      uint64_t seed = RoundSeed(set.seed, fw::WideToUtf8(file.filename().wstring()), round);
      RoundResult r = RunRound(file, seed, set.edits, nullptr, set.out / "current-edits.txt");
      done += r.done, refused += r.refused;
      if (r.failure && r.failure->category == "open" && r.applied.empty()) {
        unreadable = true;  // the model doesn't read the original: D1's, not an editing failure
        note = r.failure->message;
        break;
      }
      if (!r.saved.empty() && savedPath.empty()) {
        fs::path saved = set.out / "saved" / (SafeName(file) + ".docx");
        WriteBytes(saved, r.saved);
        savedPath = fw::WideToUtf8(fs::absolute(saved).generic_wstring());
        pairs << fw::WideToUtf8(fs::absolute(file).generic_wstring()) << "\t" << savedPath << "\n";
      }
      if (!r.failure) continue;
      size_t kept = 0;
      std::string repro = KeepReproducer(file, seed, round, r, set, kept);
      std::printf("[FAIL] %s: round %u: %s: %s\n       reproducer (%zu of %zu edits): %s\n", name.c_str(), round,
                  r.failure->category.c_str(), r.failure->message.c_str(), kept, r.applied.size(), repro.c_str());
      fileJson += std::string(failures ? "," : "") + "\n      {\"round\": " + std::to_string(round) + ", \"seed\": " +
                  std::to_string(seed) + ", \"category\": " + JsonText(r.failure->category) + ", \"message\": " +
                  JsonText(r.failure->message) + ", \"edits\": " + std::to_string(kept) + ", \"reproducer\": " +
                  JsonText(repro) + "}";
      ++failures;
    }
    totalDone += done, totalRefused += refused;
    if (unreadable) {
      ++refusedFiles;
      std::printf("[skip] %s: %s\n", name.c_str(), note.c_str());
    } else if (!failures) {
      std::printf("[ ok ] %s: %u rounds, %u edits done, %u refused\n", name.c_str(), set.rounds, done, refused);
    } else {
      ++failedFiles;
    }
    fileJson += std::string(failures ? "\n    " : "") + "], \"done\": " + std::to_string(done) + ", \"refused\": " +
                std::to_string(refused) + ", \"skipped\": " + (unreadable ? JsonText(note) : "null") +
                ", \"saved\": " + (savedPath.empty() ? "null" : JsonText(savedPath)) + "}";
    json += fileJson + (fi + 1 < files.size() ? "," : "");
    if (!targetName) {
      fw::pkg::Status st;
      auto pkg = OpenPackage(file, st);
      if (auto doc = pkg ? docx::Read(std::move(pkg), st) : nullptr) targetName = torture::OpenTarget(std::move(doc), st)->Name();
    }
  }
  json += "\n  ]\n}\n";
  if (!options.jsonOut.empty()) std::ofstream(options.jsonOut, std::ios::trunc) << json;
  fs::remove(set.out / "current-edits.txt", ec);
  std::printf("\n%zu documents, %d failed, %d not read by the model; %llu edits done, %llu refused (editor: %s)\n",
              files.size(), failedFiles, refusedFiles, static_cast<unsigned long long>(totalDone),
              static_cast<unsigned long long>(totalRefused), targetName ? targetName : "?");
  return failedFiles == 0 ? 0 : 1;
}

}  // namespace fwtool
