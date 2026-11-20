// The slide show's renderer (SlideRenderer.h) on p1-render's slides (slide/render/SlideRender.h): the
// show's items are the master's and layout's shapes as one item (they never animate), then the
// slide's own top-level shapes, bottom to top. p1-render filters by shape id; the show by item.
#include <algorithm>
#include <list>
#include <map>

#include "draw/D2DPainter.h"
#include "slide/Presentation.h"
#include "slide/render/SlideRender.h"
#include "slide/show/SlideRenderer.h"
#include "text/Fonts.h"

namespace fw::slide::show {
namespace {

constexpr size_t kKept = 6;  // prepared slides kept: the current, the next, and a few back

struct Prepared {
  std::unique_ptr<render::SlideRender> slide;
  std::vector<ItemInfo> items;
  std::map<uint32_t, size_t> itemOfId;  // the slide's top-level shapes
};

// The show's filter, asked by shape id.
class ById : public render::DrawFilter {
 public:
  ById(const Prepared& p, const show::DrawFilter& f) : p_(p), f_(f) {}
  bool Shape(uint32_t id) override {
    if (id == 0) return f_.Item(0);
    auto it = p_.itemOfId.find(id);
    return it == p_.itemOfId.end() || f_.Item(it->second);  // inside a group: the group decided
  }
  bool ShapeBody(uint32_t id) override {
    if (id == 0) return true;
    auto it = p_.itemOfId.find(id);
    return it == p_.itemOfId.end() || f_.Body(it->second);
  }
  bool Paragraph(uint32_t id, int index) override {
    if (id == 0) return true;
    auto it = p_.itemOfId.find(id);
    return it == p_.itemOfId.end() || f_.Paragraph(it->second, index);
  }
  bool Background() override { return f_.Background(); }

 private:
  const Prepared& p_;
  const show::DrawFilter& f_;
};

class DeckRenderer final : public SlideRenderer {
 public:
  explicit DeckRenderer(Presentation& deck) : deck_(deck) { render::UseDeckFonts(fonts_, deck_); }

  size_t SlideCount() const override { return deck_.SlideCount(); }
  draw::Size SlideSize() const override { return deck_.SlideSize(); }
  const std::vector<ItemInfo>& Items(size_t slide) override { return Get(slide).items; }

  draw::Rect ParagraphBox(size_t slide, size_t item, int paragraph) override {
    Prepared& p = Get(slide);
    if (item >= p.items.size()) return {};
    if (p.slide)
      for (const render::ParagraphInfo& pi : p.slide->Paragraphs())
        if (pi.shape == p.items[item].spid && pi.index == paragraph) return pi.bounds;
    return p.items[item].bounds;
  }

  void Draw(ID2D1DeviceContext* dc, size_t slide, const D2D1_MATRIX_3X2_F& toTarget, const DrawFilter* filter) override {
    Prepared& p = Get(slide);
    if (!p.slide) return;
    D2D1_MATRIX_3X2_F saved;
    dc->GetTransform(&saved);
    dc->SetTransform(toTarget);
    {
      draw::D2DPainter painter(dc, &cache_);
      draw::Size size = p.slide->Size();
      if (filter) {
        ById byId(p, *filter);
        p.slide->Draw(painter, {0, 0, size.width, size.height}, &byId);
      } else {
        p.slide->Draw(painter, {0, 0, size.width, size.height});
      }
    }
    dc->SetTransform(saved);
  }

  const Hyperlink* TextLinkAt(size_t slide, size_t item, draw::Point pt) override {
    Prepared& p = Get(slide);
    if (!p.slide || item >= p.items.size()) return nullptr;
    for (const render::LinkArea& a : p.slide->Links())
      if (a.shape == p.items[item].spid && pt.x >= a.bounds.left && pt.x <= a.bounds.right && pt.y >= a.bounds.top &&
          pt.y <= a.bounds.bottom && a.link)
        return &a.link;
    return nullptr;
  }

  void ReleaseDevice() override { cache_.Clear(); }

 private:
  Prepared& Get(size_t index) {
    auto found = std::find_if(kept_.begin(), kept_.end(), [&](const auto& e) { return e.first == index; });
    if (found != kept_.end()) {
      kept_.splice(kept_.begin(), kept_, found);  // most recent first
      return kept_.front().second;
    }
    if (kept_.size() >= kKept) kept_.pop_back();
    kept_.emplace_front(index, Prepared{});
    Prepared& p = kept_.front().second;
    const Slide* s = deck_.GetSlide(index);
    if (!s) return p;
    p.slide = render::SlideRender::Make(deck_, *s, fonts_);
    if (!p.slide) return p;
    // The master's and layout's shapes: one item, under everything.
    ItemInfo master;
    master.spid = 0;
    p.items.push_back(master);
    for (const render::ShapeInfo& si : p.slide->Shapes()) {
      if (si.fromMaster || !si.shape || si.id == 0) continue;
      bool topLevel = std::any_of(s->shapes.begin(), s->shapes.end(), [&](const Shape& sh) { return &sh == si.shape; });
      if (!topLevel || p.itemOfId.count(si.id)) continue;
      ItemInfo info;
      info.spid = si.id;
      info.bounds = si.bounds;
      // Shadows and glow reach past the box: room for them in the shape's sprite.
      float m = 6 + 0.06f * std::max(si.bounds.Width(), si.bounds.Height());
      info.ink = {si.bounds.left - m, si.bounds.top - m, si.bounds.right + m, si.bounds.bottom + m};
      info.click = si.shape->click ? &si.shape->click : nullptr;
      p.itemOfId[si.id] = p.items.size();
      p.items.push_back(info);
    }
    for (const render::ParagraphInfo& pi : p.slide->Paragraphs()) {
      auto it = p.itemOfId.find(pi.shape);
      if (it == p.itemOfId.end()) continue;
      ItemInfo& info = p.items[it->second];
      info.paragraphs = std::max(info.paragraphs, pi.index + 1);
      info.ink = {std::min(info.ink.left, pi.bounds.left), std::min(info.ink.top, pi.bounds.top),
                  std::max(info.ink.right, pi.bounds.right), std::max(info.ink.bottom, pi.bounds.bottom)};
    }
    return p;
  }

  Presentation& deck_;
  text::Fonts fonts_;
  std::list<std::pair<size_t, Prepared>> kept_;
  draw::D2DBitmapCache cache_;
};

}  // namespace

std::unique_ptr<SlideRenderer> MakeRenderer(Presentation& deck) { return std::make_unique<DeckRenderer>(deck); }

}  // namespace fw::slide::show
