#include "slide/view/ModelDeck.h"
#include "slide/view/NewDeck.h"
#include "slide/view/SlideOrder.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "core/Strings.h"
#include "pkg/CompoundFile.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "slide/ppt/Ppt.h"
#include "slide/Presentation.h"
#include "slide/odp/Odp.h"
#include "slide/render/SlideRender.h"
#include "text/Fonts.h"

#if __has_include("slide/edit/Edit.h")
#include "core/Lang.h"
#include "slide/edit/Edit.h"
#define FW_SLIDE_EDIT 1
#else
#define FW_SLIDE_EDIT 0
#endif

namespace fw::slide::view {

namespace {

// Slides prepared for drawing (shapes read, text laid out) kept at once; the farthest from the
// one asked for goes first, so paging through a long deck stays light.
constexpr size_t kKeptRenders = 24;

// One shape's text for find and copy, and where each of its paragraphs starts in it.
struct Block {
  TextBlock block;
  uint32_t shapeId = 0;
  bool table = false;
  std::vector<size_t> paragraphStarts;
};

class ModelDeck : public Deck {
 public:
  explicit ModelDeck(std::unique_ptr<Presentation> deck, size_t bytes) : deck_(std::move(deck)), bytes_(bytes) {
    renders_.resize(deck_->SlideCount());
    blocks_.resize(deck_->SlideCount());
    blocksRead_.assign(deck_->SlideCount(), false);
  }

  size_t SlideCount() const override { return deck_->SlideCount(); }
  draw::Size SlideSize() const override { return deck_->SlideSize(); }
  draw::Size NotesSize() const override { return deck_->NotesSize(); }
  bool Hidden(size_t i) const override {
    const Slide* s = Get(i);
    return s && s->hidden;
  }
  std::vector<TextBlock> Text(size_t i) const override {
    std::vector<TextBlock> out;
    for (const Block& b : Blocks(i)) out.push_back(b.block);
    return out;
  }
  std::wstring Notes(size_t i) const override {
    const Slide* s = Get(i);
    return s ? s->NotesText() : std::wstring();
  }
  std::vector<SlideLink> Links(size_t i) const override;
  bool StartsAsShow() const override { return deck_->StartsInShow(); }
  bool HasMacros() const override { return deck_->HasMacros(); }
  Presentation* Model() override { return deck_.get(); }
  edit::Editor* Editor() override;
  void Paint(size_t i, draw::Painter& p) override {
    if (render::SlideRender* r = Render(i)) {
      const draw::Size s = deck_->SlideSize();
      r->Draw(p, {0, 0, s.width, s.height});
    }
  }
  bool PaintNotesPage(size_t i, draw::Painter& p, draw::Rect& slideBox) override;
  std::vector<draw::Rect> TextRects(size_t slide, size_t block, size_t from, size_t length) override;
  size_t MemoryUse() const override { return bytes_; }
  render::SlideRender* Rendered(size_t i) override { return Render(i); }
  text::Fonts* LayoutFonts() override { return &Fonts(); }
  void Invalidate(size_t i) override {
    // Slides added or removed: everything prepared goes.
    if (deck_->SlideCount() != renders_.size()) {
      renders_.clear();
      blocks_.clear();
      blocksRead_.clear();
      renders_.resize(deck_->SlideCount());
      blocks_.resize(deck_->SlideCount());
      blocksRead_.assign(deck_->SlideCount(), false);
      return;
    }
    if (i >= renders_.size()) return;
    renders_[i].reset();
    blocks_[i].clear();
    blocksRead_[i] = false;
  }
  void PaintFiltered(size_t i, draw::Painter& p, render::DrawFilter* filter) override {
    if (render::SlideRender* r = Render(i)) {
      const draw::Size s = deck_->SlideSize();
      r->Draw(p, {0, 0, s.width, s.height}, filter);
    }
  }
  void PaintPrompts(size_t i, draw::Painter& p, uint32_t skip) override {
    if (render::SlideRender* r = Render(i)) {
      const draw::Size s = deck_->SlideSize();
      r->DrawPrompts(p, {0, 0, s.width, s.height}, Fonts(), skip);
    }
  }
  std::vector<LayoutChoice> LayoutsFor(size_t slide) const override;
  int LayoutOf(size_t slide) const override;
  void PaintLayout(size_t layout, draw::Painter& p) override;
  void Reorder(const std::vector<int>& oldIndexes) override {
    FollowSlides(renders_, oldIndexes);
    FollowSlides(blocks_, oldIndexes);
    std::vector<bool> read(oldIndexes.size(), false);
    for (size_t i = 0; i < oldIndexes.size(); ++i)
      if (oldIndexes[i] >= 0 && static_cast<size_t>(oldIndexes[i]) < blocksRead_.size()) read[i] = blocksRead_[static_cast<size_t>(oldIndexes[i])];
    blocksRead_ = std::move(read);
  }

 private:
  const Slide* Get(size_t i) const { return i < deck_->SlideCount() ? deck_->GetSlide(i) : nullptr; }
  text::Fonts& Fonts() const;
  render::SlideRender* Render(size_t i) const;
  const std::vector<Block>& Blocks(size_t i) const;
  void AddText(const Shape& sh, const Slide& s, const draw::Rect& box, std::vector<Block>& out) const;
  SlideLink LinkOf(const Hyperlink& h, const draw::Rect& box, size_t slide) const;

  std::unique_ptr<Presentation> deck_;
#if FW_SLIDE_EDIT
  std::unique_ptr<edit::Editor> editor_;  // made on first use; goes before the deck it edits
#endif
  size_t bytes_ = 0;
  mutable std::unique_ptr<text::Fonts> fonts_;
  mutable std::vector<std::unique_ptr<render::SlideRender>> renders_;
  mutable std::vector<std::vector<Block>> blocks_;
  mutable std::vector<bool> blocksRead_;
};

edit::Editor* ModelDeck::Editor() {
#if FW_SLIDE_EDIT
  if (!editor_) editor_ = std::make_unique<edit::Editor>(*deck_);
  return editor_.get();
#else
  return nullptr;
#endif
}

text::Fonts& ModelDeck::Fonts() const {
  if (!fonts_) {
    fonts_ = std::make_unique<text::Fonts>();
    render::UseDeckFonts(*fonts_, *deck_);  // the fonts the deck embeds, before anything is laid out
  }
  return *fonts_;
}

render::SlideRender* ModelDeck::Render(size_t i) const {
  if (i >= renders_.size()) return nullptr;
  if (!renders_[i]) {
    const Slide* s = Get(i);
    if (!s) return nullptr;
    size_t kept = 0, farthest = SIZE_MAX, distance = 0;
    for (size_t k = 0; k < renders_.size(); ++k)
      if (renders_[k]) {
        ++kept;
        const size_t d = k > i ? k - i : i - k;
        if (farthest == SIZE_MAX || d > distance) farthest = k, distance = d;
      }
    if (kept >= kKeptRenders && farthest != SIZE_MAX) renders_[farthest].reset();
    renders_[i] = render::SlideRender::Make(*deck_, *s, Fonts());
  }
  return renders_[i].get();
}

const std::vector<Block>& ModelDeck::Blocks(size_t i) const {
  static const std::vector<Block> none;
  if (i >= blocks_.size()) return none;
  if (!blocksRead_[i]) {
    blocksRead_[i] = true;
    if (const Slide* s = Get(i))
      for (const Shape& sh : s->shapes) AddText(sh, *s, sh.bounds, blocks_[i]);
  }
  return blocks_[i];
}

void ModelDeck::AddText(const Shape& sh, const Slide& s, const draw::Rect& box, std::vector<Block>& out) const {
  if (sh.kind == Shape::Kind::Group) {
    for (const Shape& c : sh.children) AddText(c, s, box, out);
    return;
  }
  Block b;
  b.shapeId = sh.id;
  b.block.bounds = box;
  if (sh.kind == Shape::Kind::Table) {
    const Table* t = deck_->GetTable(sh, s);
    if (!t) return;
    b.table = true;
    int row = -1;
    for (const TableCell& c : t->cells) {
      if (c.covered) continue;
      if (row >= 0) b.block.text += c.row != row ? L'\n' : L'\t';
      row = c.row;
      if (c.text)
        for (size_t p = 0; p < c.text->paragraphs.size(); ++p) {
          if (p) b.block.text += L' ';
          for (const TextRun& r : c.text->paragraphs[p].runs) b.block.text += r.kind == TextRun::Kind::Break ? L" " : r.text;
        }
    }
  } else {
    if (!sh.textNode) return;
    const TextBody* body = deck_->Text(sh, s);
    if (!body || body->Empty()) return;
    for (size_t p = 0; p < body->paragraphs.size(); ++p) {
      if (p) b.block.text += L'\n';
      b.paragraphStarts.push_back(b.block.text.size());
      for (const TextRun& r : body->paragraphs[p].runs)
        b.block.text += r.kind == TextRun::Kind::Break ? std::wstring(1, L' ') : r.text;
    }
  }
  if (b.block.text.empty()) return;
  // Copy and the notes pages want line breaks as such.
  std::replace(b.block.text.begin(), b.block.text.end(), L' ', L'\n');
  out.push_back(std::move(b));
}

std::vector<draw::Rect> ModelDeck::TextRects(size_t slide, size_t block, size_t from, size_t /*length*/) {
  // The paragraph holding the match, where the renderer laid it out; else the shape as drawn.
  const std::vector<Block>& blocks = Blocks(slide);
  render::SlideRender* r = Render(slide);
  if (block >= blocks.size() || !r) return {};
  const Block& b = blocks[block];
  if (!b.table && !b.paragraphStarts.empty()) {
    const int para = static_cast<int>(std::upper_bound(b.paragraphStarts.begin(), b.paragraphStarts.end(), from) -
                                      b.paragraphStarts.begin()) - 1;
    for (const render::ParagraphInfo& p : r->Paragraphs())
      if (p.shape == b.shapeId && p.index == para && p.bounds.Width() > 0) return {p.bounds};
  }
  for (const render::ShapeInfo& s : r->Shapes())
    if (s.id == b.shapeId && b.shapeId != 0) return {s.bounds};
  return {};
}

SlideLink ModelDeck::LinkOf(const Hyperlink& h, const draw::Rect& box, size_t slide) const {
  SlideLink link;
  link.rect = box;
  link.tooltip = Utf8ToWide(h.tooltip);
  const int n = static_cast<int>(deck_->SlideCount());
  switch (h.action) {
    case Hyperlink::Action::Url: link.url = Utf8ToWide(h.url); break;
    case Hyperlink::Action::Slide: link.slide = h.slide >= 0 ? h.slide : n; break;  // n: not in the deck
    case Hyperlink::Action::FirstSlide: link.slide = 0; break;
    case Hyperlink::Action::LastSlide: link.slide = std::max(0, n - 1); break;
    case Hyperlink::Action::NextSlide: link.slide = std::min(n - 1, static_cast<int>(slide) + 1); break;
    case Hyperlink::Action::PreviousSlide: link.slide = std::max(0, static_cast<int>(slide) - 1); break;
    default: link.slide = -3; break;  // programs, macros, files, shows: never started from the tab
  }
  if (link.tooltip.empty()) link.tooltip = link.url;
  return link;
}

std::vector<SlideLink> ModelDeck::Links(size_t i) const {
  std::vector<SlideLink> out;
  render::SlideRender* r = Render(i);
  if (!r) return out;
  // Shapes' own links first, then the runs' (on top: a run's link wins over its shape's).
  for (const render::ShapeInfo& s : r->Shapes())
    if (s.shape && s.shape->click) out.push_back(LinkOf(s.shape->click, s.bounds, i));
  for (const render::LinkArea& a : r->Links())
    if (a.link) out.push_back(LinkOf(a.link, a.bounds, i));
  return out;
}

bool ModelDeck::PaintNotesPage(size_t i, draw::Painter& p, draw::Rect& slideBox) {
  const Slide* s = Get(i);
  if (!s || !s->notes) return false;
  const NotesSlide& notes = *s->notes;
  // Where the slide's picture goes: the notes page's slide image placeholder, as it inherits it.
  const Shape* image = notes.FindPlaceholder(PlaceholderType::SlideImage);
  for (const Shape* ph = image; ph && slideBox.Width() <= 0; ph = ph->inherits) slideBox = ph->bounds;
  if (slideBox.Width() <= 0)
    if (const Master* m = deck_->NotesMaster())
      if (const Shape* ph = m->FindPlaceholder(PlaceholderType::SlideImage)) slideBox = ph->bounds;
  std::unique_ptr<render::SlideRender> page = render::SlideRender::Make(*deck_, notes, Fonts());
  if (!page) return false;
  const draw::Size ns = deck_->NotesSize();
  p.FillRect({0, 0, ns.width, ns.height}, draw::Brush::Solid({1, 1, 1, 1}));
  page->Draw(p, {0, 0, ns.width, ns.height});
  return true;
}

std::vector<Deck::LayoutChoice> ModelDeck::LayoutsFor(size_t slide) const {
  std::vector<LayoutChoice> out;
  const Slide* s = Get(slide);
  const Master* master = s ? s->master : nullptr;
  if (!master && !deck_->Masters().empty()) master = deck_->Masters().front().get();
  if (!master) return out;
  const auto& all = deck_->Layouts();
  for (const Layout* l : master->layouts)
    for (size_t i = 0; i < all.size(); ++i)
      if (all[i].get() == l) out.push_back({i, Utf8ToWide(l->name.empty() ? l->type : l->name)});
  return out;
}

int ModelDeck::LayoutOf(size_t slide) const {
  const Slide* s = Get(slide);
  if (!s || !s->layout) return -1;
  const auto& all = deck_->Layouts();
  for (size_t i = 0; i < all.size(); ++i)
    if (all[i].get() == s->layout) return static_cast<int>(i);
  return -1;
}

void ModelDeck::PaintLayout(size_t layout, draw::Painter& p) {
  const auto& all = deck_->Layouts();
  if (layout >= all.size()) return;
  const Layout& l = *all[layout];
  // The gallery leaves out the date, footer and slide number, as PowerPoint's does.
  struct Filter : render::DrawFilter {
    std::vector<uint32_t> skip;
    bool Shape(uint32_t id) override { return id == 0 || std::find(skip.begin(), skip.end(), id) == skip.end(); }
  } filter;
  for (const Shape& sh : l.shapes)
    if (sh.placeholder.type == PlaceholderType::Date || sh.placeholder.type == PlaceholderType::Footer ||
        sh.placeholder.type == PlaceholderType::SlideNumber)
      filter.skip.push_back(sh.id);
  const draw::Size size = deck_->SlideSize();
  if (std::unique_ptr<render::SlideRender> r = render::SlideRender::Make(*deck_, l, Fonts()))
    r->Draw(p, {0, 0, size.width, size.height}, &filter);
  // The placeholders' outlines, dotted, as PowerPoint's gallery shows where content goes.
  draw::Pen pen;
  pen.brush = draw::Brush::Solid({0.55f, 0.55f, 0.55f, 1});
  pen.width = std::max(1.0f, size.width / 160);
  pen.dashes = {2, 1.5f};
  for (const Shape& sh : l.shapes) {
    if (!sh.placeholder || sh.hidden || std::find(filter.skip.begin(), filter.skip.end(), sh.id) != filter.skip.end()) continue;
    draw::Rect b = sh.bounds;
    for (const Shape* up = sh.inherits; b.Width() <= 0 && up; up = up->inherits) b = up->bounds;
    if (b.Width() > 0 && b.Height() > 0) p.StrokePath(draw::Path::FromRect(b), pen);
  }
}

}  // namespace

std::unique_ptr<Deck> DeckOf(std::unique_ptr<pkg::Snapshot> snap, std::wstring& error);

std::unique_ptr<Deck> OpenDeck(const std::wstring& path, std::wstring& error) {
  pkg::Status status;
  std::unique_ptr<pkg::Snapshot> snap = pkg::Snapshot::Open(path, {}, status);
  if (!snap) {
    error = T(L"The file couldn't be read.");
    return nullptr;
  }
  return DeckOf(std::move(snap), error);
}

std::unique_ptr<Deck> OpenOpenDocument(const std::wstring& path, std::wstring& error) {
  pkg::Status status;
  std::unique_ptr<pkg::Snapshot> snap = pkg::Snapshot::Open(path, {}, status);
  if (!snap) {
    error = T(L"The file couldn't be read.");
    return nullptr;
  }
  std::vector<uint8_t> pptx;
  status = odp::OdpToPptx(snap->Bytes(), pptx);
  snap.reset();
  if (!status.Ok()) {
    error = status.code == pkg::Status::Code::TooLarge ? T(L"It's too large to open.")
                                                       : T(L"It seems to be damaged or isn't an OpenDocument presentation.");
    return nullptr;
  }
  return DeckOf(pkg::Snapshot::FromBytes(std::move(pptx)), error);
}

bool IsOpenDocument(const std::wstring& path) { return EndsWithNoCase(path, L".odp") || EndsWithNoCase(path, L".otp"); }

bool ReadLegacyPresentation(const std::wstring& path, std::wstring_view password, std::vector<uint8_t>& pptx,
                            std::wstring& error, bool& locked) {
  pptx.clear();
  error.clear();
  locked = false;
  pkg::Status status;
  std::unique_ptr<pkg::Snapshot> snap = pkg::Snapshot::Open(path, {}, status);
  if (!snap || !pkg::CompoundFile::HasSignature(snap->Bytes())) return false;
  auto cf = pkg::CompoundFile::Open(snap->Bytes());
  const ppt::StreamFn streams = [&cf](std::string_view name) { return cf->Stream(name); };
  if (!cf || !ppt::IsPresentation(streams)) return false;
  locked = ppt::NeedsPassword(streams);
  if (locked && password.empty()) return true;
  pptx = ppt::ToPptx(streams, status, password);
  if (pptx.empty() && locked && status.code == pkg::Status::Code::Rejected) return true;  // the wrong password
  if (pptx.empty())
    error = status.message.empty() ? T(L"This file isn't a presentation Featherweight can open, or it is damaged.")
                                   : Fmt(T(L"It couldn't be opened — {0}"), {Utf8ToWide(status.message)});
  return true;
}

std::unique_ptr<Deck> OpenDeckFromBytes(std::vector<uint8_t> bytes, std::wstring& error) {
  if (bytes.empty()) {
    error = T(L"The presentation couldn't be made.");
    return nullptr;
  }
  return DeckOf(pkg::Snapshot::FromBytes(std::move(bytes)), error);
}

std::unique_ptr<Deck> DeckOf(std::unique_ptr<pkg::Snapshot> snap, std::wstring& error) {
  pkg::Status status;
  const size_t bytes = static_cast<size_t>(snap->Size());
  std::unique_ptr<pkg::Package> package = pkg::Package::Open(std::move(snap), pkg::Limits::Default(), status);
  std::unique_ptr<Presentation> deck = package && status.Ok() ? Read(std::move(package), status) : nullptr;
  if (!deck) {
    error = status.message.empty() ? T(L"This file isn't a presentation Featherweight can open, or it is damaged.")
                                   : Utf8ToWide(status.message);
    return nullptr;
  }
  return std::make_unique<ModelDeck>(std::move(deck), bytes);
}

}  // namespace fw::slide::view
