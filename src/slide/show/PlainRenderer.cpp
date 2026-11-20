// The slide show's own plain renderer (SlideRenderer.h): fwdraw for shapes, pictures and backgrounds,
// and one DirectWrite layout per paragraph for text. It stands in for p1-render's slide renderer.
#include <dwrite.h>

#include <algorithm>
#include <map>
#include <string>

#include "draw/D2DPainter.h"
#include "draw/Drawing.h"
#include "slide/Presentation.h"
#include "slide/show/SlideRenderer.h"
#include "ui/Gfx.h"

namespace fw::slide::show {
namespace {

D2D1_MATRIX_3X2_F ToD2D(const draw::Matrix& m) { return {m.m11, m.m12, m.m21, m.m22, m.dx, m.dy}; }

struct Para {
  ComPtr<IDWriteTextLayout> layout;
  D2D1_COLOR_F color{0, 0, 0, 1};
  draw::Rect box;  // the drawing's coordinates (its extent's top left is 0, 0)
};

struct Item {
  std::unique_ptr<draw::DrawingResources> resources;
  std::unique_ptr<draw::Drawing> drawing;
  draw::Matrix textTransform;  // the text box's coordinates to the drawing's
  std::vector<Para> paras;
};

struct SlideCache {
  std::unique_ptr<draw::DrawingResources> bgResources;
  std::unique_ptr<draw::Drawing> background;
  std::vector<Item> items;
  std::vector<ItemInfo> info;
};

std::wstring PlainText(const Paragraph& p) {
  std::wstring s;
  for (const TextRun& r : p.runs) s += r.kind == TextRun::Kind::Break ? std::wstring(L"\n") : r.text;
  return s;
}

class PlainRenderer final : public SlideRenderer {
 public:
  explicit PlainRenderer(Presentation& deck) : deck_(deck) {}

  size_t SlideCount() const override { return deck_.SlideCount(); }
  draw::Size SlideSize() const override { return deck_.SlideSize(); }

  const std::vector<ItemInfo>& Items(size_t slide) override { return Get(slide).info; }

  draw::Rect ParagraphBox(size_t slide, size_t item, int paragraph) override {
    SlideCache& c = Get(slide);
    if (item >= c.items.size()) return {};
    const Item& it = c.items[item];
    if (paragraph < 0 || paragraph >= static_cast<int>(it.paras.size())) return c.info[item].bounds;
    draw::Rect b = it.paras[paragraph].box;
    draw::Rect bounds = it.drawing ? it.drawing->Bounds() : draw::Rect{};
    draw::Point a = it.textTransform.Apply({b.left, b.top}), z = it.textTransform.Apply({b.right, b.bottom});
    return {bounds.left + std::min(a.x, z.x), bounds.top + std::min(a.y, z.y), bounds.left + std::max(a.x, z.x),
            bounds.top + std::max(a.y, z.y)};
  }

  void Draw(ID2D1DeviceContext* dc, size_t slide, const D2D1_MATRIX_3X2_F& toTarget, const DrawFilter* filter) override {
    SlideCache& c = Get(slide);
    D2D1_MATRIX_3X2_F saved;
    dc->GetTransform(&saved);
    dc->SetTransform(toTarget);
    if (c.background && (!filter || filter->Background())) {
      draw::D2DPainter painter(dc, &cache_);
      c.background->Draw(painter, c.background->Bounds());
    }
    for (size_t i = 0; i < c.items.size(); ++i) {
      if (filter && !filter->Item(i)) continue;
      const Item& it = c.items[i];
      if (!it.drawing) continue;
      draw::Rect bounds = it.drawing->Bounds();
      if (!filter || filter->Body(i)) {
        draw::D2DPainter painter(dc, &cache_);
        it.drawing->Draw(painter, bounds);
      }
      if (it.paras.empty()) continue;
      D2D1_MATRIX_3X2_F place = ToD2D(it.textTransform * draw::Matrix::Translate(bounds.left, bounds.top));
      dc->SetTransform(place * toTarget);
      for (size_t p = 0; p < it.paras.size(); ++p) {
        if (filter && !filter->Paragraph(i, static_cast<int>(p))) continue;
        const Para& para = it.paras[p];
        if (!para.layout) continue;
        ComPtr<ID2D1SolidColorBrush> brush;
        dc->CreateSolidColorBrush(para.color, &brush);
        if (brush) dc->DrawTextLayout({para.box.left, para.box.top}, para.layout.Get(), brush.Get());
      }
      dc->SetTransform(toTarget);
    }
    dc->SetTransform(saved);
  }

  const Hyperlink* TextLinkAt(size_t slide, size_t item, draw::Point pt) override {
    SlideCache& c = Get(slide);
    if (item >= c.items.size()) return nullptr;
    const Slide* s = deck_.GetSlide(slide);
    std::vector<DrawItem> list = deck_.DrawList(*s);
    if (item >= list.size()) return nullptr;
    const TextBody* text = deck_.Text(*list[item].shape, *s);
    if (!text) return nullptr;
    for (size_t p = 0; p < text->paragraphs.size() && p < c.items[item].paras.size(); ++p) {
      draw::Rect b = ParagraphBox(slide, item, static_cast<int>(p));
      if (pt.x < b.left || pt.x > b.right || pt.y < b.top || pt.y > b.bottom) continue;
      for (const TextRun& r : text->paragraphs[p].runs)
        if (r.props.click) return &r.props.click;
    }
    return nullptr;
  }

  void ReleaseDevice() override { cache_.Clear(); }

 private:
  SlideCache& Get(size_t index) {
    auto found = slides_.find(index);
    if (found != slides_.end()) return found->second;
    SlideCache& c = slides_[index];
    const Slide* s = deck_.GetSlide(index);
    if (!s) return c;
    if (s->background && s->backgroundOwner) {
      c.bgResources = deck_.Resources(*s->backgroundOwner, *s);
      c.background = draw::Drawing::FromShapeTree(s->background, *c.bgResources);
    }
    for (const DrawItem& d : deck_.DrawList(*s)) {
      Item it;
      ItemInfo info;
      it.resources = deck_.Resources(*d.owner, *s);
      it.drawing = draw::Drawing::FromShapeTree(d.shape->drawNode, *it.resources);
      if (d.owner == s) {
        info.spid = d.shape->id;
        info.click = d.shape->click ? &d.shape->click : nullptr;
      }
      if (it.drawing) {
        info.bounds = it.drawing->Bounds();
        draw::Rect ink = it.drawing->InkBounds();
        info.ink = {info.bounds.left + ink.left, info.bounds.top + ink.top, info.bounds.left + ink.right,
                    info.bounds.top + ink.bottom};
        for (const draw::TextBox& box : it.drawing->TextBoxes()) {
          const Shape* shape = d.owner->ShapeOfText(box.content);
          const TextBody* text = shape ? deck_.Text(*shape, *s) : nullptr;
          if (!text || text->Empty()) continue;
          it.textTransform = box.transform;
          Layout(*text, box, it.paras);
          break;  // one text box per shape
        }
        for (const Para& p : it.paras) {
          draw::Point a = it.textTransform.Apply({p.box.left, p.box.top});
          draw::Point z = it.textTransform.Apply({p.box.right, p.box.bottom});
          info.ink.left = std::min(info.ink.left, info.bounds.left + std::min(a.x, z.x));
          info.ink.top = std::min(info.ink.top, info.bounds.top + std::min(a.y, z.y));
          info.ink.right = std::max(info.ink.right, info.bounds.left + std::max(a.x, z.x));
          info.ink.bottom = std::max(info.ink.bottom, info.bounds.top + std::max(a.y, z.y));
        }
      }
      info.paragraphs = static_cast<int>(it.paras.size());
      c.items.push_back(std::move(it));
      c.info.push_back(info);
    }
    return c;
  }

  // Paragraphs one under another in the box, at their first run's font, size and color.
  void Layout(const TextBody& text, const draw::TextBox& box, std::vector<Para>& out) {
    float width = std::max(1.f, box.rect.Width());
    float y = 0;
    for (const Paragraph& p : text.paragraphs) {
      Para para;
      const RunProps& rp = p.runs.empty() ? p.endProps : p.runs.front().props;
      std::wstring face = rp.latin.empty() ? L"Calibri" : rp.latin;
      float size = std::max(1.f, rp.size * box.fontScale);
      ComPtr<IDWriteTextFormat> format;
      gfx::DWrite()->CreateTextFormat(face.c_str(), nullptr, rp.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                      rp.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, size, L"", &format);
      std::wstring s = PlainText(p);
      float indent = p.props.marginLeft;
      if (format) {
        format->SetTextAlignment(p.props.align == TextAlign::Center  ? DWRITE_TEXT_ALIGNMENT_CENTER
                                 : p.props.align == TextAlign::Right ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                                     : DWRITE_TEXT_ALIGNMENT_LEADING);
        if (!box.wrap) format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        gfx::DWrite()->CreateTextLayout(s.empty() ? L" " : s.c_str(), s.empty() ? 1 : static_cast<UINT32>(s.size()),
                                        format.Get(), std::max(1.f, width - indent), 1e5f, &para.layout);
      }
      float h = size * 1.2f;
      if (para.layout) {
        DWRITE_TEXT_METRICS m{};
        para.layout->GetMetrics(&m);
        h = m.height;
      }
      para.color = {rp.color.r, rp.color.g, rp.color.b, rp.noFill ? 0.f : rp.color.a};
      para.box = {box.rect.left + indent, y, box.rect.right, y + h};
      y += h;
      out.push_back(std::move(para));
    }
    // Anchored as the box says.
    float slack = box.rect.Height() - y;
    float shift = box.anchor == draw::TextBox::Anchor::Middle   ? slack / 2
                  : box.anchor == draw::TextBox::Anchor::Bottom ? slack
                                                                : 0;
    for (Para& p : out) {
      p.box.top += box.rect.top + shift;
      p.box.bottom += box.rect.top + shift;
    }
  }

  Presentation& deck_;
  std::map<size_t, SlideCache> slides_;
  draw::D2DBitmapCache cache_;
};

}  // namespace

std::unique_ptr<SlideRenderer> MakePlainRenderer(Presentation& deck) { return std::make_unique<PlainRenderer>(deck); }

}  // namespace fw::slide::show
