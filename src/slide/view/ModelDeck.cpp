#include "slide/view/ModelDeck.h"

#include <algorithm>
#include <vector>

#include "core/Strings.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "slide/Presentation.h"
#include "slide/view/SlidePages.h"
#include "text/Fonts.h"

namespace fw::slide::view {

namespace {

// A shape's text, paragraphs separated by '\n' (line breaks too).
std::wstring BodyText(const TextBody& body) {
  std::wstring out;
  for (size_t p = 0; p < body.paragraphs.size(); ++p) {
    if (p) out += L'\n';
    for (const TextRun& r : body.paragraphs[p].runs) out += r.kind == TextRun::Kind::Break ? std::wstring(L"\n") : r.text;
  }
  return out;
}

class ModelDeck : public Deck {
 public:
  explicit ModelDeck(std::unique_ptr<Presentation> deck, size_t bytes) : deck_(std::move(deck)), bytes_(bytes) {}

  size_t SlideCount() const override { return deck_->SlideCount(); }
  draw::Size SlideSize() const override { return deck_->SlideSize(); }
  draw::Size NotesSize() const override { return deck_->NotesSize(); }
  bool Hidden(size_t i) const override {
    const Slide* s = Get(i);
    return s && s->hidden;
  }
  std::vector<TextBlock> Text(size_t i) const override {
    std::vector<TextBlock> out;
    if (const Slide* s = Get(i))
      for (const Shape& sh : s->shapes) AddText(sh, *s, sh.bounds, out);
    return out;
  }
  std::wstring Notes(size_t i) const override {
    const Slide* s = Get(i);
    return s ? s->NotesText() : std::wstring();
  }
  std::vector<SlideLink> Links(size_t i) const override {
    std::vector<SlideLink> out;
    if (const Slide* s = Get(i))
      for (const Shape& sh : s->shapes) AddLinks(sh, *s, sh.bounds, out);
    return out;
  }
  bool StartsAsShow() const override { return deck_->StartsInShow(); }
  void Paint(size_t i, draw::Painter& p) override;
  size_t MemoryUse() const override { return bytes_; }

 private:
  const Slide* Get(size_t i) const { return i < deck_->SlideCount() ? deck_->GetSlide(i) : nullptr; }
  // `box`: where the shape is on the slide (a group's child takes its top-level group's box).
  void AddText(const Shape& sh, const Slide& s, const draw::Rect& box, std::vector<TextBlock>& out) const;
  void AddLinks(const Shape& sh, const Slide& s, const draw::Rect& box, std::vector<SlideLink>& out) const;
  SlideLink LinkOf(const Hyperlink& h, const draw::Rect& box, size_t slide) const;

  std::unique_ptr<Presentation> deck_;
  size_t bytes_ = 0;
  std::unique_ptr<text::Fonts> fonts_;
};

void ModelDeck::AddText(const Shape& sh, const Slide& s, const draw::Rect& box, std::vector<TextBlock>& out) const {
  if (sh.kind == Shape::Kind::Group) {
    for (const Shape& c : sh.children) AddText(c, s, box, out);
    return;
  }
  if (sh.kind == Shape::Kind::Table) {
    if (const Table* t = deck_->GetTable(sh, s)) {
      std::wstring text;
      int row = -1;
      for (const TableCell& c : t->cells) {
        if (c.covered) continue;
        if (row >= 0) text += c.row != row ? L'\n' : L'\t';
        row = c.row;
        if (c.text) text += BodyText(*c.text);
      }
      if (!text.empty()) out.push_back({text, box});
    }
    return;
  }
  if (!sh.textNode) return;
  if (const TextBody* body = deck_->Text(sh, s); body && !body->Empty()) out.push_back({BodyText(*body), box});
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
    default: link.slide = -3; break;  // shown, does nothing in the tab (programs, macros, files)
  }
  if (link.tooltip.empty()) link.tooltip = link.url;
  return link;
}

void ModelDeck::AddLinks(const Shape& sh, const Slide& s, const draw::Rect& box, std::vector<SlideLink>& out) const {
  if (sh.click) out.push_back(LinkOf(sh.click, box, s.index));
  for (const Shape& c : sh.children) AddLinks(c, s, box, out);
  if (!sh.textNode) return;
  // Links on the text's runs: the shape's box, until the renderer tells where the runs are.
  if (const TextBody* body = deck_->Text(sh, s))
    for (const Paragraph& p : body->paragraphs)
      for (const TextRun& r : p.runs)
        if (r.props.click) {
          out.push_back(LinkOf(r.props.click, box, s.index));
          return;
        }
}

void ModelDeck::Paint(size_t i, draw::Painter& p) {
  const draw::Size size = deck_->SlideSize();
  p.FillRect({0, 0, size.width, size.height}, draw::Brush::Solid({1, 1, 1, 1}));
  if (!fonts_) fonts_ = std::make_unique<text::Fonts>();
  for (const TextBlock& b : Text(i)) {
    p.Save();
    p.ClipRect(b.bounds);
    DrawPlainText(*fonts_, p, b.text, {b.bounds.left + 7, b.bounds.top + 4, b.bounds.right - 7, b.bounds.bottom - 4}, 18,
                  L"Calibri", {0.1f, 0.1f, 0.1f, 1});
    p.Restore();
  }
}

}  // namespace

std::unique_ptr<Deck> OpenDeck(const std::wstring& path, std::wstring& error) {
  pkg::Status status;
  std::unique_ptr<pkg::Snapshot> snap = pkg::Snapshot::Open(path, {}, status);
  if (!snap) {
    error = L"The file couldn't be read.";
    return nullptr;
  }
  const size_t bytes = static_cast<size_t>(snap->Size());
  std::unique_ptr<pkg::Package> package = pkg::Package::Open(std::move(snap), pkg::Limits::Default(), status);
  std::unique_ptr<Presentation> deck = package && status.Ok() ? Read(std::move(package), status) : nullptr;
  if (!deck) {
    error = status.message.empty() ? L"This file isn't a presentation Featherweight can open, or it is damaged."
                                   : Utf8ToWide(status.message);
    return nullptr;
  }
  return std::make_unique<ModelDeck>(std::move(deck), bytes);
}

}  // namespace fw::slide::view
