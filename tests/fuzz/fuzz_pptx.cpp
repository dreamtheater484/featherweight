// Fuzz target: the presentation model (src/slide, P1): the .pptx reader with every slide, its
// layout, master and notes, and the inheritance the renderer asks for: each shape's drawNode read
// by fwdraw, the text boxes Drawing finds mapped back to their shapes, every text resolved for the
// slide it shows on, tables with their styles, backgrounds, hyperlinks and the embedded fonts.
// Then (P2, p2-corpus) edits a hash of the input picks, made through the edit API as the app makes
// them (slides, layouts, notes, typing and text formats, shapes moved, grouped, formatted, inserted,
// copied and pasted, tables, headers and footers, the slide size, backgrounds, groups of edits, undo
// and redo): a refused edit changes nothing; the edited model passes the reader's checks; the save
// writes well-formed parts, copies every other part byte for byte, opens again (without package
// errors when the input had none) and reads back as the same deck; the crash-recovery journal
// replayed on the deck read again gives the same deck and saves the same parts; undoing everything
// gives the deck as read, redoing everything the deck as edited.
// Seeds: tests/fuzz/seeds/pptx (the generated decks and a deck with every structure the model
// reads, as text; python tests/fuzz/make_pptx_seeds.py) and tests/fuzz/seeds/package.
// FUZZ_PPTX_STEPS=n makes only the first n edits (the same ones); FUZZ_PPTX_TRACE prints each edit.
//
// FUZZ_PPTX_OPEN=1 adds the checks of findings reported to their owner and not fixed yet (the
// replay test in CI runs without them; the edits made are the same). Remove each once it's fixed:
//   1. (p2-model, 2026-11-22) Typing or deleting text that's refused right after typing in the same
//      place takes that typing back (Call reopens the typing step, then cancels all of it).
//      tests/office/presentations/template.potx, FUZZ_PPTX_STEPS=6.
//   2. (p2-model, 2026-11-22) Text with characters XML doesn't allow (U+0001, a lone surrogate,
//      U+FFFE) is taken and saved: the part doesn't parse any more. Here they're typed only then.
//      tests/office/presentations/charts.pptx, FUZZ_PPTX_STEPS=23 (InsertTextBox "a\u0001z").
//   3. (p2-model, 2026-11-22) After the journal's replay, the save writes the parts its "back to the
//      file's own" records name ([Content_Types].xml, rels) anew instead of copying them: the same
//      content, other bytes. Without FUZZ_PPTX_OPEN the replay's save is compared canonicalized.
//      tests/office/presentations/bullets.pptx, FUZZ_PPTX_STEPS=1.
//
// An input is a zip, or parts as text as fuzz_check reads them: each part starts on a line
// "@<part name>", and the rest of the input up to the next such line is its content.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <algorithm>
#include <set>

#include "draw/Drawing.h"
#include "pkg/Canonical.h"
#include "pkg/Check.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "pkg/Zip.h"
#include "slide/Presentation.h"
#include "slide/edit/Edit.h"
#include "slide/edit/Journal.h"
#include "slide/edit/Save.h"

using namespace fw::pkg;
using namespace fw::slide;

namespace {

Limits FuzzLimits() {
  Limits limits;
  limits.maxEntrySize = 16u << 20;
  limits.maxTotalSize = 64u << 20;
  limits.maxEntries = 4096;
  limits.maxStringLength = 1u << 20;
  return limits;
}

std::vector<uint8_t> ZipFromText(std::string_view text) {
  MemorySink sink;
  ZipWriter writer(sink);
  size_t pos = text.starts_with('@') ? 0 : text.find("\n@");
  while (pos != std::string_view::npos) {
    if (text[pos] == '\n') ++pos;
    size_t eol = text.find('\n', pos);
    if (eol == std::string_view::npos) break;
    std::string_view name = text.substr(pos + 1, eol - pos - 1);
    if (!name.empty() && name.back() == '\r') name.remove_suffix(1);
    size_t next = text.find("\n@", eol);
    std::string_view body = text.substr(eol + 1, (next == std::string_view::npos ? text.size() : next) - eol - 1);
    auto bytes = reinterpret_cast<const uint8_t*>(body.data());
    writer.Add(name, std::span<const uint8_t>(bytes, body.size()), nullptr, ZipWriter::Compression::Store);
    pos = next;
  }
  writer.Finish();
  return std::move(sink.bytes);
}

void CheckAt(bool ok, int line) {
  if (!ok) {
    std::fprintf(stderr, "fuzz_pptx.cpp:%d: check failed\n", line);
    std::abort();
  }
}
#define Check(ok) CheckAt((ok), __LINE__)

bool Finite(float v) { return std::isfinite(v); }
bool Finite(const draw::Rect& r) { return Finite(r.left) && Finite(r.top) && Finite(r.right) && Finite(r.bottom); }
bool Finite(const draw::Color& c) {
  return Finite(c.r) && Finite(c.g) && Finite(c.b) && Finite(c.a) && c.a >= 0 && c.a <= 1;
}

void CheckLink(const Presentation& deck, const Hyperlink& h) {
  if (h.action == Hyperlink::Action::Slide) Check(h.slide >= -1 && h.slide < static_cast<int>(deck.SlideCount()));
}

void CheckText(const Presentation& deck, const TextBody& t) {
  Check(t.body.columns >= 1 && t.body.columns <= 16);
  Check(t.body.fontScale > 0 && t.body.fontScale <= 1);
  Check(Finite(t.body.left) && Finite(t.body.top) && Finite(t.body.right) && Finite(t.body.bottom));
  for (const Paragraph& p : t.paragraphs) {
    Check(p.props.level >= 0 && p.props.level <= 8);
    Check(p.number >= 0);
    Check(Finite(p.props.marginLeft) && Finite(p.props.indent) && Finite(p.props.lineSpacing.value));
    Check(p.props.tabs.size() <= 64);
    Check(p.endProps.size >= 1 && p.endProps.size <= 4000);
    for (const TextRun& r : p.runs) {
      Check(r.props.size >= 1 && r.props.size <= 4000);
      Check(Finite(r.props.baseline) && Finite(r.props.spacing) && Finite(r.props.kern));
      Check(Finite(r.props.color));
      Check(r.node);
      CheckLink(deck, r.props.click);
      CheckLink(deck, r.props.hover);
    }
  }
}

void CheckShape(Presentation& deck, const Shape& s, const SlideBase& owner, const SlideBase& shownOn, int depth) {
  Check(depth <= 40);
  Check(s.owner == &owner);
  Check(s.node && s.drawNode);
  Check(Finite(s.bounds) && Finite(s.rotation));
  Check(owner.FindShape(s.id) != nullptr);
  CheckLink(deck, s.click);
  CheckLink(deck, s.hover);
  if (s.textNode) {
    Check(owner.ShapeOfText(s.textNode) == &s);
    const TextBody* t = deck.Text(s, shownOn);
    Check(t != nullptr);
    CheckText(deck, *t);
    Check(deck.Text(s, shownOn) == t);  // cached
  }
  if (const Table* t = deck.GetTable(s, shownOn)) {
    Check(t->cells.size() == t->rows.size() * t->columns.size());
    for (const TableCell& c : t->cells) {
      Check(c.row >= 0 && c.column >= 0 && c.rowSpan >= 1 && c.columnSpan >= 1);
      Check(c.row + c.rowSpan <= static_cast<int>(t->rows.size()));
      Check(c.column + c.columnSpan <= static_cast<int>(t->columns.size()));
      Check(t->Cell(c.row, c.column) == &c);
      Check(Finite(c.fill.color));
      for (const CellBorder& b : c.borders) Check(Finite(b.width) && b.width >= 0 && Finite(b.color));
      if (c.text) CheckText(deck, *c.text);
    }
  }
  for (const Shape& c : s.children) CheckShape(deck, c, owner, shownOn, depth + 1);
}

// fwdraw reads each drawNode, and every text box it finds belongs to a shape of the part.
void DrawPart(Presentation& deck, const SlideBase& shownOn) {
  if (shownOn.background) {
    Check(shownOn.backgroundOwner != nullptr);
    auto res = deck.Resources(*shownOn.backgroundOwner, shownOn);
    draw::Drawing::FromShapeTree(shownOn.background, *res);
  }
  for (const DrawItem& item : deck.DrawList(shownOn)) {
    Check(item.shape && item.owner);
    CheckShape(deck, *item.shape, *item.owner, shownOn, 0);
    auto res = deck.Resources(*item.owner, shownOn);
    auto d = draw::Drawing::FromShapeTree(item.shape->drawNode, *res);
    if (!d) continue;
    Check(Finite(d->Bounds()));
    for (const draw::TextBox& box : d->TextBoxes()) {
      const Shape* t = item.owner->ShapeOfText(box.content);
      if (t) Check(deck.Text(*t, shownOn) != nullptr);
    }
  }
}

// Everything the renderer and the show ask of the model, for every slide, layout and master.
void CheckDeck(Presentation& deck) {
  Check(deck.SlideSize().width >= 72 && deck.SlideSize().height >= 72);
  deck.LoadAll();
  for (size_t i = 0; i < deck.SlideCount(); ++i) {
    const Slide* s = deck.GetSlide(i);
    Check(s != nullptr && s->index == i && s->theme);
    Check(deck.SlideIndexOfId(s->id) >= 0);
    DrawPart(deck, *s);
    if (s->notes) DrawPart(deck, *s->notes);
    s->NotesText();
  }
  for (const auto& l : deck.Layouts()) DrawPart(deck, *l);
  for (const auto& m : deck.Masters()) DrawPart(deck, *m);
  for (const Section& sec : deck.Sections())
    for (size_t i : sec.slides) Check(i < deck.SlideCount());
  for (const CustomShow& cs : deck.CustomShows())
    for (size_t i : cs.slides) Check(i < deck.SlideCount());
  for (const EmbeddedFont& f : deck.EmbeddedFonts()) Check(!f.sfnt.empty());
  deck.Problems();
}

// ---- Edits (P2): a program a hash of the input picks, made through the edit API as the app makes
// them, undo and redo among them.

struct Rng {
  uint64_t s;
  uint64_t Next() {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  uint32_t Below(size_t n) { return n ? static_cast<uint32_t>(Next() % n) : 0; }
  bool Chance(uint32_t percent) { return Below(100) < percent; }
  float Between(float a, float b) { return a + (b - a) * static_cast<float>(Below(10001)) / 10000.f; }
};

uint64_t HashOf(const uint8_t* data, size_t size) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < size; ++i) h = (h ^ data[i]) * 1099511628211ull;
  return h;
}

std::string Esc(std::wstring_view w) {
  std::string s;
  for (wchar_t c : w) {
    if (c >= 0x20 && c < 0x7F) {
      s += static_cast<char>(c);
    } else {
      char buf[16];
      std::snprintf(buf, sizeof buf, "\\u%04X", static_cast<unsigned>(c));
      s += buf;
    }
  }
  return s;
}

long long Emu(float points) { return std::llround(static_cast<double>(points) * 12700.0); }

void DigestText(const TextBody& t, std::string& out) {
  for (const Paragraph& p : t.paragraphs) {
    out += "|";
    for (const TextRun& r : p.runs) {
      out += "[" + std::string(r.props.bold ? "b" : "") + (r.props.italic ? "i" : "") + std::to_string(std::lround(r.props.size * 100)) + "]";
      out += r.kind == TextRun::Kind::Break ? std::string("\\v") : Esc(r.text);
    }
  }
}

void DigestShapes(Presentation& deck, const Slide& slide, const std::vector<Shape>& shapes, size_t depth, std::string& out) {
  for (const Shape& s : shapes) {
    out += std::string(depth * 2 + 2, ' ') + std::to_string(s.id) + " k" + std::to_string(static_cast<int>(s.kind)) + " \"" + s.name +
           "\" " + std::to_string(Emu(s.bounds.left)) + "," + std::to_string(Emu(s.bounds.top)) + " " +
           std::to_string(Emu(s.bounds.Width())) + "x" + std::to_string(Emu(s.bounds.Height())) + " r" +
           std::to_string(std::lround(s.rotation * 60000)) + (s.flipH ? " fh" : "") + (s.flipV ? " fv" : "") +
           (s.hidden ? " hidden" : "") + "\n";
    if (const TextBody* t = deck.Text(s, slide)) {
      out += std::string(depth * 2 + 4, ' ') + "text ";
      DigestText(*t, out);
      out += "\n";
    }
    if (const Table* t = deck.GetTable(s, slide)) {
      out += std::string(depth * 2 + 4, ' ') + "table " + std::to_string(t->rows.size()) + "x" + std::to_string(t->columns.size());
      for (const TableCell& c : t->cells) {
        out += " (" + std::to_string(c.row) + "," + std::to_string(c.column) + " " + std::to_string(c.rowSpan) + "x" +
               std::to_string(c.columnSpan);
        if (c.text) DigestText(*c.text, out);
        out += ")";
      }
      out += "\n";
    }
    DigestShapes(deck, slide, s.children, depth + 1, out);
  }
}

// The deck, as text: what undoing everything, the journal and reading the save back must give.
std::string Digest(Presentation& deck) {
  deck.LoadAll();
  std::string out = "size " + std::to_string(Emu(deck.SlideSize().width)) + "x" + std::to_string(Emu(deck.SlideSize().height)) +
                    ", first number " + std::to_string(deck.FirstSlideNumber()) + "\n";
  for (size_t i = 0; i < deck.SlideCount(); ++i) {
    const Slide* s = deck.GetSlide(i);
    int layout = -1;
    for (size_t l = 0; l < deck.Layouts().size(); ++l)
      if (deck.Layouts()[l].get() == s->layout) layout = static_cast<int>(l);
    out += "slide " + std::to_string(i) + " id " + std::to_string(s->id) + " layout " + std::to_string(layout) +
           (s->hidden ? " hidden" : "") + (s->showMasterShapes ? "" : " no-master-shapes") + "\n";
    DigestShapes(deck, *s, s->shapes, 0, out);
    if (s->notes) out += "  notes " + Esc(s->NotesText()) + "\n";
  }
  for (const Section& sec : deck.Sections()) {
    out += "section \"" + sec.name + "\":";
    for (size_t i : sec.slides) out += " " + std::to_string(i);
    out += "\n";
  }
  for (const CustomShow& show : deck.CustomShows()) {
    out += "custom show " + std::to_string(show.id) + " \"" + show.name + "\":";
    for (size_t i : show.slides) out += " " + std::to_string(i);
    out += "\n";
  }
  return out;
}

void PrintDifference(const char* what, const std::string& a, const std::string& b) {
  size_t at = 0;
  while (at < a.size() && at < b.size() && a[at] == b[at]) ++at;
  const size_t from = a.rfind('\n', at) == std::string::npos ? 0 : a.rfind('\n', at) + 1;
  std::fprintf(stderr, "%s:\n  %s\n  %s\n", what, a.substr(from, 200).c_str(), b.substr(from, 200).c_str());
}

void Collect(const std::vector<Shape>& shapes, std::vector<const Shape*>& out) {
  for (const Shape& s : shapes) {
    out.push_back(&s);
    Collect(s.children, out);
  }
}

// What's typed: words, XML's characters, other scripts, an emoji, paragraph and line breaks, a
// tab, nothing, and (rarely) what only a paste brings: a control character, a lone surrogate.
const wchar_t* const kTexts[] = {L"Hello", L"<&>\"'", L"na\u00EFve \u20AC", L"\u65E5\u672C\u8A9E", L"\u0627\u0644\u0639\u0631\u0628\u064A\u0629",
                                 L"\U0001F642", L"one\ntwo", L"line\vbreak", L"tab\there", L"", L" ",
                                 L"A longer line that wraps over several lines and makes autofit shrink the text in its shape"};
const wchar_t* const kHostile[] = {L"a\x01z", L"\xD800", L"\xDC00x", L"\xFFFE", L"\r\n"};
const char* const kPresets[] = {"rect", "ellipse", "rightArrow", "star5", "roundRect", "line", "notAPreset"};
constexpr const char* kA = "http://schemas.openxmlformats.org/drawingml/2006/main";
constexpr const char* kP = "http://schemas.openxmlformats.org/presentationml/2006/main";
// A 1x1 PNG.
const uint8_t kPng[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00,
                        0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00,
                        0x00, 0x00, 0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00, 0x05, 0x00, 0x01,
                        0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

std::string SolidFill(Rng& rng) {
  char rgb[8];
  std::snprintf(rgb, sizeof rgb, "%06X", static_cast<unsigned>(rng.Below(0x1000000)));
  return std::string("<a:solidFill xmlns:a=\"") + kA + "\"><a:srgbClr val=\"" + rgb + "\"/></a:solidFill>";
}

edit::ColorSpec AnyColor(Rng& rng) {
  switch (rng.Below(4)) {
    case 0: return edit::ColorSpec::Rgb(rng.Below(0x1000000));
    case 1: return edit::ColorSpec::Scheme(rng.Chance(50) ? "accent1" : "tx1", rng.Chance(50) ? 75000 : 0);
    case 2: return edit::ColorSpec{};  // inherit
    default: {
      edit::ColorSpec c;
      c.kind = edit::ColorSpec::Kind::None;
      return c;
    }
  }
}

// Makes one edit (or none, when it's refused: then nothing may change).
// The fuzzer's state between edits.
struct State {
  int groups = 0;       // groups open (BeginGroup)
  bool typing = false;  // the last edit typed or deleted text (the next may continue its step)
  bool open = false;    // FUZZ_PPTX_OPEN: the checks of reported findings not fixed yet (see the top)
  bool trace = false;
};

void EditOnce(edit::Editor& ed, Presentation& deck, Rng& rng, State& state, uint32_t step) {
  int& groups = state.groups;
  const bool afterTyping = state.typing;
  state.typing = false;
  deck.LoadAll();
  const size_t count = deck.SlideCount();
  const size_t slide = count == 0 || rng.Chance(4) ? count + rng.Below(2) : rng.Below(count);
  std::vector<const Shape*> all, top, texts, tables, groupsOnSlide, pictures;
  if (slide < count) {
    const Slide* s = deck.GetSlide(slide);
    for (const Shape& x : s->shapes) top.push_back(&x);
    Collect(s->shapes, all);
    for (const Shape* x : all) {
      if (x->textNode) texts.push_back(x);
      if (deck.GetTable(*x, *s)) tables.push_back(x);
      if (x->kind == Shape::Kind::Group) groupsOnSlide.push_back(x);
      if (x->kind == Shape::Kind::Picture) pictures.push_back(x);
    }
  }
  auto from = [&](const std::vector<const Shape*>& v) -> const Shape* { return v.empty() ? nullptr : v[rng.Below(v.size())]; };
  auto idOf = [&](const std::vector<const Shape*>& v) -> uint32_t {
    const Shape* s = rng.Chance(5) ? nullptr : from(v.empty() ? all : v);
    return s ? s->id : 1 + rng.Below(5000);
  };
  std::wstring typed;  // the last text made (the trace shows it)
  auto text = [&]() -> std::wstring {
    const bool hostile = rng.Chance(3);
    const uint32_t i = rng.Below(hostile ? std::size(kHostile) : std::size(kTexts));
    // Characters XML doesn't allow are refused or dropped only with FUZZ_PPTX_OPEN (finding 2).
    return typed = hostile && state.open ? kHostile[i] : kTexts[i % std::size(kTexts)];
  };
  auto rect = [&]() {
    const draw::Size size = deck.SlideSize();
    const float x = rng.Between(-50, size.width), y = rng.Between(-50, size.height);
    return draw::Rect{x, y, x + rng.Between(0, size.width / 2), y + rng.Between(0, size.height / 2)};
  };
  auto target = [&]() {
    edit::TextTarget t{slide, idOf(texts)};
    if (const Shape* table = rng.Chance(20) ? from(tables) : nullptr) {
      const Table* tb = deck.GetTable(*table, *deck.GetSlide(slide));
      t.shape = table->id;
      t.row = static_cast<int>(rng.Below(tb->rows.size() + 1));
      t.column = static_cast<int>(rng.Below(tb->columns.size() + 1));
    } else if (rng.Chance(10) && slide < count) {
      t.shape = ed.NotesShape(slide);
      t.notes = true;
    }
    return t;
  };
  auto pos = [&](const edit::TextTarget& t) {
    const size_t n = ed.ParagraphCount(t);
    edit::TextPos p{rng.Below(n + (rng.Chance(5) ? 1 : 0)), 0};
    if (p.paragraph < n) p.offset = rng.Below(ed.ParagraphText(t, p.paragraph).size() + (rng.Chance(5) ? 2 : 1));
    return p;
  };
  auto range = [&](const edit::TextTarget& t) {
    edit::TextRange r{pos(t), pos(t)};
    if (r.to < r.from) std::swap(r.from, r.to);
    return r;
  };
  auto link = [&]() -> std::optional<edit::LinkTarget> {
    if (rng.Chance(25)) return std::nullopt;
    edit::LinkTarget l;
    l.kind = static_cast<edit::LinkTarget::Kind>(rng.Below(7));
    l.url = rng.Chance(50) ? "https://example.com/a?b=1&c=<2>" : "mailto:someone@example.com";
    l.slide = rng.Below(count + 1);
    l.tooltip = "tip & \"more\"";
    return l;
  };

  const std::string before = Digest(deck);
  const size_t undoSteps = ed.UndoNames().size();
  const uint32_t op = rng.Below(41);
  edit::Result r;
  const char* name = "?";
  switch (op) {
    case 0: name = "NewSlide"; r = ed.NewSlide(rng.Below(count + 2), rng.Below(deck.Layouts().size() + 1)); break;
    case 1: {
      name = "DuplicateSlides";
      std::set<size_t> pick{slide};
      if (rng.Chance(30)) pick.insert(rng.Below(count + 1));
      r = ed.DuplicateSlides(std::vector<size_t>(pick.begin(), pick.end()));
      break;
    }
    case 2: name = "DeleteSlides"; r = ed.DeleteSlides({slide}); break;
    case 3: name = "MoveSlides"; r = ed.MoveSlides({slide}, rng.Below(count + 1)); break;
    case 4: name = "HideSlides"; r = ed.HideSlides({slide}, rng.Chance(50)); break;
    case 5: name = "SetLayout"; r = ed.SetLayout({slide}, rng.Below(deck.Layouts().size() + 1)); break;
    case 6: name = "SetNotesText"; r = ed.SetNotesText(slide, text()); break;
    case 7:
    case 8:
    case 9: {
      name = "InsertText";
      const edit::TextTarget t = target();
      r = ed.InsertText(t, pos(t), text());
      state.typing = r.ok;
      break;
    }
    case 10: {
      name = "DeleteText";
      const edit::TextTarget t = target();
      r = ed.DeleteText(t, range(t));
      state.typing = r.ok;
      break;
    }
    case 11: {
      name = "ReplaceText";
      const edit::TextTarget t = target();
      r = ed.ReplaceText(t, range(t), text());
      break;
    }
    case 12: name = "SetText"; r = ed.SetText(target(), text()); break;
    case 13: {
      name = "SetRunProps";
      const edit::TextTarget t = target();
      edit::RunPatch p;
      switch (rng.Below(9)) {
        case 0: p.bold = rng.Chance(50); break;
        case 1: p.italic = rng.Chance(50); break;
        case 2: p.underline = static_cast<Underline>(rng.Below(18)); break;
        case 3: p.size = rng.Chance(10) ? rng.Between(-5, 5000) : rng.Between(1, 96); break;
        case 4: p.color = AnyColor(rng); break;
        case 5: p.highlight = AnyColor(rng); break;
        case 6: p.baseline = rng.Chance(50) ? 0.3f : -0.25f; break;
        case 7: p.latin = rng.Chance(50) ? L"Consolas" : L"+mn-lt"; break;
        default: p.caps = static_cast<Caps>(rng.Below(3)); break;
      }
      r = ed.SetRunProps(t, range(t), p);
      break;
    }
    case 14: {
      name = "SetParagraphProps";
      const edit::TextTarget t = target();
      const size_t n = ed.ParagraphCount(t);
      const size_t first = rng.Below(n + 1), last = first + rng.Below(3);
      edit::ParagraphPatch p;
      switch (rng.Below(6)) {
        case 0: p.align = static_cast<TextAlign>(rng.Below(7)); break;
        case 1: p.level = static_cast<int>(rng.Below(11)) - 1; break;
        case 2: {
          edit::BulletSpec b;
          b.kind = static_cast<edit::BulletSpec::Kind>(rng.Below(4));
          b.startAt = static_cast<int>(rng.Below(40000)) - 2;
          p.bullet = b;
          break;
        }
        case 3: p.lineSpacing = Spacing{rng.Chance(50), rng.Between(0, 3)}; break;
        case 4: p.marginLeft = rng.Between(-20, 200), p.indent = rng.Between(-50, 50); break;
        default: p.rtl = rng.Chance(50); break;
      }
      r = ed.SetParagraphProps(t, first, last, p);
      break;
    }
    case 15: {
      name = "SetBodyProps";
      edit::BodyPatch p;
      switch (rng.Below(6)) {
        case 0: p.autofit = static_cast<BodyProps::Autofit>(rng.Below(3)); break;
        case 1: p.anchor = static_cast<BodyProps::Anchor>(rng.Below(5)); break;
        case 2: p.wrap = rng.Chance(50); break;
        case 3: p.insets = std::array<float, 4>{rng.Between(0, 30), rng.Between(0, 30), rng.Between(0, 30), rng.Between(0, 30)}; break;
        case 4: p.columns = static_cast<int>(rng.Below(18)); break;
        default: p.vertical = static_cast<BodyProps::Vertical>(rng.Below(7)); break;
      }
      r = ed.SetBodyProps(target(), p);
      break;
    }
    case 16: {
      name = "SetTransform";
      const Shape* s = from(all);
      edit::Transform t{s ? s->bounds : rect(), s ? s->rotation : 0, s && s->flipH, s && s->flipV};
      const uint32_t id = s && !rng.Chance(5) ? s->id : 1 + rng.Below(5000);
      switch (rng.Below(4)) {
        case 0: {
          const float dx = rng.Between(-200, 200), dy = rng.Between(-200, 200);
          t.bounds = {t.bounds.left + dx, t.bounds.top + dy, t.bounds.right + dx, t.bounds.bottom + dy};
          break;
        }
        case 1:
          t.bounds.right = t.bounds.left + t.bounds.Width() * rng.Between(-0.5f, 3);
          t.bounds.bottom = t.bounds.top + t.bounds.Height() * rng.Between(0, 3);
          break;
        case 2: t.rotation = rng.Between(-720, 720); break;
        default: t.flipH = !t.flipH, t.flipV = rng.Chance(50) ? !t.flipV : t.flipV; break;
      }
      r = ed.SetTransform(slide, id, t);
      break;
    }
    case 17: {
      name = "DeleteShapes";
      std::vector<uint32_t> ids{idOf(all)};
      if (rng.Chance(30)) ids.push_back(idOf(all));
      r = ed.DeleteShapes(slide, ids);
      break;
    }
    case 18: {
      name = "Group";
      std::set<uint32_t> ids;
      for (uint32_t i = 0, n = 2 + rng.Below(2); i < n; ++i) ids.insert(idOf(rng.Chance(80) ? top : all));
      r = ed.Group(slide, std::vector<uint32_t>(ids.begin(), ids.end()));
      break;
    }
    case 19: name = "Ungroup"; r = ed.Ungroup(slide, idOf(groupsOnSlide)); break;
    case 20: name = "Arrange"; r = ed.Arrange(slide, {idOf(all)}, static_cast<edit::ZOrder>(rng.Below(4))); break;
    case 21: {
      name = "SetShapeProps";
      edit::ShapePatch p;
      switch (rng.Below(7)) {
        case 0: p.fill = rng.Chance(80) ? SolidFill(rng) : std::string(); break;
        case 1:
          p.line = std::string("<a:ln xmlns:a=\"") + kA + "\" w=\"" + std::to_string(rng.Below(200000)) + "\">" +
                   "<a:solidFill><a:srgbClr val=\"C00000\"/></a:solidFill></a:ln>";
          break;
        case 2: p.effects = std::string("<a:effectLst xmlns:a=\"") + kA + "\"><a:outerShdw blurRad=\"50800\" dist=\"38100\"><a:prstClr val=\"black\"/></a:outerShdw></a:effectLst>"; break;
        case 3: p.geometry = std::string("<a:prstGeom xmlns:a=\"") + kA + "\" prst=\"" + kPresets[rng.Below(std::size(kPresets))] + "\"><a:avLst/></a:prstGeom>"; break;
        case 4: p.name = text(); break;
        case 5: p.altText = text(); break;
        default: p.hidden = rng.Chance(50); break;
      }
      r = ed.SetShapeProps(slide, {idOf(all)}, p);
      break;
    }
    case 22: name = "InsertTextBox"; r = ed.InsertTextBox(slide, rect(), text()); break;
    case 23: name = "InsertAutoShape"; r = ed.InsertAutoShape(slide, kPresets[rng.Below(std::size(kPresets))], rect()); break;
    case 24: name = "InsertTable"; r = ed.InsertTable(slide, static_cast<int>(rng.Below(6)), static_cast<int>(rng.Below(6)), rect()); break;
    case 25: {
      const uint32_t id = idOf(tables);
      const int a = static_cast<int>(rng.Below(6)) - 1, b = static_cast<int>(rng.Below(6)) - 1;
      const int c = static_cast<int>(rng.Below(6)), d = static_cast<int>(rng.Below(4));
      switch (rng.Below(9)) {
        case 0: name = "InsertTableRows"; r = ed.InsertTableRows(slide, id, a, d); break;
        case 1: name = "DeleteTableRows"; r = ed.DeleteTableRows(slide, id, a, d); break;
        case 2: name = "InsertTableColumns"; r = ed.InsertTableColumns(slide, id, a, d); break;
        case 3: name = "DeleteTableColumns"; r = ed.DeleteTableColumns(slide, id, a, d); break;
        case 4: name = "MergeTableCells"; r = ed.MergeTableCells(slide, id, a, b, a + d, b + c); break;
        case 5: name = "SplitTableCell"; r = ed.SplitTableCell(slide, id, a, b); break;
        case 6: name = "SetTableColumnWidth"; r = ed.SetTableColumnWidth(slide, id, a, rng.Between(-10, 400)); break;
        case 7: name = "SetTableRowHeight"; r = ed.SetTableRowHeight(slide, id, a, rng.Between(-10, 400)); break;
        default: {
          name = "SetCellProps";
          edit::CellPatch p;
          p.fill = rng.Chance(70) ? SolidFill(rng) : std::string();
          if (rng.Chance(50)) p.anchor = static_cast<BodyProps::Anchor>(rng.Below(5));
          r = ed.SetCellProps(slide, id, a, b, a + d, b + c, p);
          break;
        }
      }
      break;
    }
    case 26: {
      name = "CopyShapes+PasteShapes";
      std::vector<uint32_t> ids{idOf(all)};
      if (rng.Chance(30)) ids.push_back(idOf(all));
      const edit::ShapeClip clip = ed.CopyShapes(slide, ids);
      r = ed.PasteShapes(rng.Chance(70) ? slide : rng.Below(count + 1), clip);
      break;
    }
    case 27: {
      name = "SetHeaderFooter";
      edit::HeaderFooter hf;
      hf.slideNumber = rng.Chance(50);
      hf.footer = rng.Chance(50);
      hf.footerText = text();
      hf.date = rng.Chance(40);
      hf.dateFixed = rng.Chance(50);
      hf.dateText = L"22-11-2026";
      hf.notOnTitle = rng.Chance(30);
      r = ed.SetHeaderFooter(rng.Chance(50) ? std::vector<size_t>{} : std::vector<size_t>{slide}, hf);
      break;
    }
    case 28: {
      name = "SetSlideSize";
      const draw::Size sizes[] = {{960, 540}, {720, 540}, {rng.Between(-10, 4000), rng.Between(0, 4000)}};
      r = ed.SetSlideSize(sizes[rng.Below(3)], static_cast<edit::Scale>(rng.Below(3)));
      break;
    }
    case 29: {
      name = "SetBackground";
      const std::string bg = std::string("<p:bg xmlns:p=\"") + kP + "\" xmlns:a=\"" + kA + "\"><p:bgPr>" + SolidFill(rng) +
                             "<a:effectLst/></p:bgPr></p:bg>";
      r = ed.SetBackground({slide}, rng.Chance(80) ? bg : std::string());
      break;
    }
    case 30: name = "Undo"; r = ed.Undo(1 + rng.Below(3)); break;
    case 31: name = "Redo"; r = ed.Redo(1 + rng.Below(3)); break;
    case 32: name = "EndTyping"; ed.EndTyping(); break;
    case 33:
      name = "BeginGroup";
      if (groups < 3) ed.BeginGroup(u"Group"), ++groups;
      break;
    case 34:
      name = rng.Chance(30) ? "CancelGroup" : "EndGroup";
      if (groups > 0) {
        name[0] == 'C' ? ed.CancelGroup() : ed.EndGroup();
        --groups;
      }
      break;
    case 35: name = "MoveShape"; r = ed.MoveShape(slide, idOf(all), rng.Chance(50) ? 0 : idOf(groupsOnSlide), static_cast<int>(rng.Below(6)) - 1); break;
    case 36: {
      name = "SetCrop";
      edit::Crop c{rng.Between(-0.2f, 0.6f), rng.Between(-0.2f, 0.6f), rng.Between(-0.2f, 0.6f), rng.Between(-0.2f, 0.6f)};
      r = ed.SetCrop(slide, idOf(pictures), c);
      break;
    }
    case 37:
      if (rng.Chance(50)) {
        name = "SetShapeLink";
        r = ed.SetShapeLink(slide, idOf(all), link());
      } else {
        name = "SetTextLink";
        const edit::TextTarget t = target();
        r = ed.SetTextLink(t, range(t), link());
      }
      break;
    case 38: {
      name = "SetMasterBackground";
      const std::string bg = std::string("<p:bg xmlns:p=\"") + kP + "\" xmlns:a=\"" + kA + "\"><p:bgPr>" + SolidFill(rng) +
                             "<a:effectLst/></p:bgPr></p:bg>";
      r = ed.SetMasterBackground(rng.Below(deck.Masters().size() + 1), bg);
      break;
    }
    case 39: name = "InsertPicture"; r = ed.InsertPicture(slide, std::vector<uint8_t>(std::begin(kPng), std::end(kPng)), rect(), text()); break;
    default: {
      name = "SetTextBodyXml";
      const std::string xml = std::string("<p:txBody xmlns:p=\"") + kP + "\" xmlns:a=\"" + kA +
                              "\"><a:bodyPr/><a:lstStyle/><a:p><a:r><a:rPr lang=\"en-US\" b=\"1\"/><a:t>Pasted &amp; more</a:t></a:r></a:p></p:txBody>";
      r = ed.SetTextBodyXml(target(), xml);
      break;
    }
  }
  if (state.trace) {
    std::string why;
    for (char16_t c : r.message) why += c < 0x80 ? static_cast<char>(c) : '?';
    const std::string shown = typed.empty() ? std::string() : " \"" + Esc(typed) + "\"";
    std::fprintf(stderr, "edit %u: %s on slide %zu%s: %s\n", step, name, slide, shown.c_str(), r.ok ? "done" : ("refused: " + why).c_str());
  }
  // A refused edit changes nothing. Typing or deleting text refused right after typing takes that
  // typing back (finding 1): checked only with FUZZ_PPTX_OPEN.
  const bool typingEdit = std::string_view(name) == "InsertText" || std::string_view(name) == "DeleteText";
  if (!r.ok && (state.open || !(afterTyping && typingEdit))) {
    const std::string after = Digest(deck);
    if (after != before) PrintDifference((std::string("refused ") + name + " changed the deck").c_str(), before, after);
    Check(after == before);
    Check(ed.UndoNames().size() == undoSteps);
  }
}

// The parts of a saved file, by name: their bytes as text.
std::vector<std::pair<std::string, std::string>> PartsOf(const std::vector<uint8_t>& zip, const Limits& limits) {
  Status st;
  auto pkg = Package::Open(Snapshot::FromBytes(std::vector<uint8_t>(zip)), limits, st);
  Check(pkg != nullptr);
  std::vector<std::pair<std::string, std::string>> out;
  std::vector<uint8_t> bytes;
  for (const auto& part : pkg->Parts()) {
    Check(pkg->ReadPart(part.name, bytes).Ok());
    out.push_back({part.name, std::string(bytes.begin(), bytes.end())});
  }
  return out;
}

void EditAndSave(const std::vector<uint8_t>& zip, const Limits& limits, uint64_t seed) {
  auto read = [&] {
    Status st;
    auto pkg = Package::Open(Snapshot::FromBytes(std::vector<uint8_t>(zip)), limits, st);
    return pkg ? Read(std::move(pkg), st) : nullptr;
  };
  std::unique_ptr<Presentation> deck = read(), again = read(), fresh = read();
  if (!deck || !again || !fresh) return;
  const std::string asRead = Digest(*fresh);
  edit::Editor ed(*deck);
  if (!ed.CanEdit()) return;
  edit::Journal journal;
  ed.SetJournal(&journal);
  Rng rng{seed};
  State state;
  state.open = std::getenv("FUZZ_PPTX_OPEN") != nullptr;
  state.trace = std::getenv("FUZZ_PPTX_TRACE") != nullptr;
  uint32_t steps = 8 + rng.Below(24);
  if (const char* most = std::getenv("FUZZ_PPTX_STEPS")) steps = std::min<uint32_t>(steps, static_cast<uint32_t>(std::atoi(most)));
  for (uint32_t i = 0; i < steps; ++i) EditOnce(ed, *deck, rng, state, i);
  for (; state.groups > 0; --state.groups) ed.EndGroup();
  CheckDeck(*deck);  // the edited model is one the renderer can draw
  const std::string edited = Digest(*deck);

  const std::string records = journal.All(ed);  // before saving: the save starts a new journal

  // Saved: the parts it wrote are well-formed XML, every other part is the file's own byte for byte,
  // and the file opens, without package errors when the input had none, and reads back as the deck
  // edited.
  const size_t history = ed.UndoNames().size();
  std::vector<uint8_t> saved;
  std::vector<std::string> written;
  const bool ok = edit::SaveToBytes(ed, saved, &written).Ok();
  std::vector<std::pair<std::string, std::string>> parts;
  if (ok) {
    parts = PartsOf(saved, limits);
    Status st;
    auto original = Package::Open(Snapshot::FromBytes(std::vector<uint8_t>(zip)), limits, st);
    Check(original != nullptr);
    const std::set<std::string> anew(written.begin(), written.end());
    std::vector<uint8_t> mine;
    for (const auto& [name, bytes] : parts) {
      if (anew.count(name)) {
        std::string canonical;
        if (name.ends_with(".xml") || name.ends_with(".rels")) {
          const Status parsed = CanonicalizeXml(bytes, canonical, limits, name);
          if (!parsed.Ok()) std::fprintf(stderr, "the save wrote %s, which isn't well-formed: %s\n", name.c_str(), parsed.message.c_str());
          Check(parsed.Ok());
        }
        continue;
      }
      const bool had = original->ReadPart(name, mine).Ok();
      if (!had || std::string(mine.begin(), mine.end()) != bytes) std::fprintf(stderr, "%s changed but isn't written anew\n", name.c_str());
      Check(had && std::string(mine.begin(), mine.end()) == bytes);
    }
    auto pkg = Package::Open(Snapshot::FromBytes(std::vector<uint8_t>(saved)), limits, st);
    Check(pkg != nullptr);
    const bool clean = CheckPackage(*original).Ok();
    if (clean) {
      CheckReport report = CheckPackage(*pkg);
      for (const std::string& e : report.errors) std::fprintf(stderr, "saved file: %s\n", e.c_str());
      Check(report.Ok());
    }
    std::unique_ptr<Presentation> back = Read(std::move(pkg), st);
    if (!back) std::fprintf(stderr, "the save doesn't read: %s\n", st.message.c_str());
    Check(back != nullptr);
    if (clean) {
      const std::string readBack = Digest(*back);
      if (readBack != edited) PrintDifference("the save reads back as another deck (edited, read back)", edited, readBack);
      Check(readBack == edited);
    }
  }

  // The journal makes every edit again on the deck read again: the same deck, and its save the same
  // parts (byte for byte only with FUZZ_PPTX_OPEN: finding 3).
  edit::Editor replayed(*again);
  size_t applied = 0;
  edit::Result r = edit::Replay(replayed, records, &applied);
  if (!r) {
    std::string why;
    for (char16_t c : r.message) why += c < 0x80 ? static_cast<char>(c) : '?';
    std::fprintf(stderr, "replay stopped after %zu records: %s\n", applied, why.c_str());
  }
  Check(r.ok);
  const std::string remade = Digest(*again);
  if (edited != remade) PrintDifference("the journal remade another deck (edited, replayed)", edited, remade);
  Check(edited == remade);
  std::vector<uint8_t> savedAgain;
  Check(ok == edit::SaveToBytes(replayed, savedAgain).Ok());
  if (ok) {
    const auto partsAgain = PartsOf(savedAgain, limits);
    const bool strict = state.open;
    Check(parts.size() == partsAgain.size());
    for (size_t i = 0; i < parts.size(); ++i) {
      Check(parts[i].first == partsAgain[i].first);
      std::string a = parts[i].second, b = partsAgain[i].second;
      if (a != b && !strict && (parts[i].first.ends_with(".xml") || parts[i].first.ends_with(".rels"))) {
        std::string ca, cb;
        if (CanonicalizeXml(a, ca, limits, parts[i].first).Ok() && CanonicalizeXml(b, cb, limits, parts[i].first).Ok()) a = ca, b = cb;
      }
      if (a != b) PrintDifference(("the replay saves " + parts[i].first + " otherwise (edited, replayed)").c_str(), a, b);
      Check(a == b);
    }
  }

  // Undoing everything gives the deck as read; redoing everything the deck as edited (unless the
  // save ended the history).
  if (ed.UndoNames().size() != history || history == 0) return;
  Check(ed.Undo(history).ok);
  const std::string undone = Digest(*deck);
  if (undone != asRead) PrintDifference("undoing everything gives another deck (as read, undone)", asRead, undone);
  Check(undone == asRead);
  Check(ed.Redo(ed.RedoNames().size()).ok);
  const std::string redone = Digest(*deck);
  if (redone != edited) PrintDifference("redoing everything gives another deck (edited, redone)", edited, redone);
  Check(redone == edited);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const Limits limits = FuzzLimits();
  std::string_view input(reinterpret_cast<const char*>(data), size);
  std::vector<uint8_t> zip = input.starts_with("PK\x03\x04") ? std::vector<uint8_t>(data, data + size) : ZipFromText(input);
  const std::vector<uint8_t> input0 = zip;
  Status st;
  auto pkg = Package::Open(Snapshot::FromBytes(std::move(zip)), limits, st);
  if (!pkg) return 0;
  std::unique_ptr<Presentation> deck = Read(std::move(pkg), st);
  if (!deck) {
    Check(!st.Ok() && !st.message.empty());
    return 0;
  }
  CheckDeck(*deck);
  deck.reset();
  EditAndSave(input0, limits, HashOf(data, size));
  return 0;
}
