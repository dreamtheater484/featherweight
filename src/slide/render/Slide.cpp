#include <algorithm>
#include <cmath>

#include "draw/Xml.h"
#include "slide/render/SlideRender.h"
#include "slide/render/Table.h"
#include "slide/render/TextBody.h"
#include "text/Fonts.h"

namespace fw::slide::render {

namespace {

draw::Rect Bounds(const draw::Matrix& m, draw::Size size) {
  const draw::Point pts[4] = {m.Apply({0, 0}), m.Apply({size.width, 0}), m.Apply({0, size.height}),
                              m.Apply({size.width, size.height})};
  draw::Rect r{pts[0].x, pts[0].y, pts[0].x, pts[0].y};
  for (const draw::Point& q : pts)
    r = {std::min(r.left, q.x), std::min(r.top, q.y), std::max(r.right, q.x), std::max(r.bottom, q.y)};
  return r;
}

draw::Rect Transformed(const draw::Matrix& m, const draw::Rect& r) {
  return Bounds(draw::Matrix::Translate(r.left, r.top) * m, {r.Width(), r.Height()});
}

}  // namespace

struct SlideRender::Impl {
  struct Item {
    uint32_t id = 0;
    bool inherited = false, background = false;
    const fw::slide::Shape* shape = nullptr;
    // Kept with the drawing: SmartArt's text boxes point into the parts it read.
    std::unique_ptr<draw::DrawingResources> resources;
    std::unique_ptr<draw::Drawing> drawing;
    std::vector<TextBlock> texts;  // one per drawing text box
    std::unique_ptr<TableRender> table;
    draw::Rect frame;  // a table's graphic frame, slide points
  };
  std::vector<Item> items;
};

class Hooks : public draw::DrawHooks {
 public:
  Hooks(const SlideRender::Impl::Item& item, DrawFilter* filter) : item_(item), filter_(filter) {}
  // Shapes without an id of their own (SmartArt's) go with their graphic frame.
  bool DrawShape(uint32_t id) override { return !filter_ || (id == 0 && !item_.inherited) || filter_->Shape(Id(id)); }
  bool DrawBody(uint32_t id) override { return !filter_ || (id == 0 && !item_.inherited) || filter_->ShapeBody(Id(id)); }
  void DrawText(draw::Painter& painter, size_t index) override {
    if (index >= item_.texts.size()) return;
    const uint32_t id = item_.drawing->TextBoxes()[index].shape;
    item_.texts[index].Draw(painter, filter_, id == 0 ? Id(item_.id) : Id(id));
  }
  uint32_t Id(uint32_t id) const { return item_.inherited ? 0 : id; }

 private:
  const SlideRender::Impl::Item& item_;
  DrawFilter* filter_;
};

struct Builder {
  fw::slide::Presentation& deck;
  const fw::slide::SlideBase& slide;
  text::Fonts& fonts;
  SlideRender& out;
  SlideRender::Impl& impl;
  TextExtras extras;

  Builder(fw::slide::Presentation& d, const fw::slide::SlideBase& s, text::Fonts& f, SlideRender& o)
      : deck(d), slide(s), fonts(f), out(o), impl(*o.impl_) {}

  // The part a picture bullet's relationship belongs to: the slide, its layout or its master.
  const fw::slide::SlideBase* PartNamed(const std::string& name) const {
    for (const fw::slide::SlideBase* p = &slide; p; p = p->parent)
      if (p->part == name) return p;
    return nullptr;
  }

  void Run() {
    const bool notes = slide.kind == fw::slide::PartKind::Notes || slide.kind == fw::slide::PartKind::NotesMaster;
    out.size_ = notes ? deck.NotesSize() : deck.SlideSize();
    extras.colors.theme = slide.theme ? slide.theme.get() : draw::Theme::Default().get();
    extras.colors.colorMap = &slide.colorMap;
    extras.picture = [this](const std::string& part, const std::string& rel) -> std::shared_ptr<const draw::Image> {
      const fw::slide::SlideBase* owner = PartNamed(part);
      if (!owner) return nullptr;
      return deck.Resources(*owner, slide)->GetImage(rel);
    };
    if (slide.background && slide.backgroundOwner) {
      SlideRender::Impl::Item item;
      item.background = item.inherited = true;
      item.resources = deck.Resources(*slide.backgroundOwner, slide);
      item.drawing = draw::Drawing::FromShapeTree(slide.background, *item.resources);
      if (item.drawing) impl.items.push_back(std::move(item));
    }
    for (const fw::slide::DrawItem& di : deck.DrawList(slide)) Shape(di);
    Finish();
  }

  void Shape(const fw::slide::DrawItem& di) {
    const fw::slide::Shape& s = *di.shape;
    using K = fw::slide::Shape::Kind;
    if (s.hidden || s.kind == K::ContentPart || s.placeholder.type == fw::slide::PlaceholderType::SlideImage) return;
    SlideRender::Impl::Item item;
    if (s.kind == K::Table) {
      const fw::slide::Table* t = deck.GetTable(s, slide);
      if (!t) return;
      item.inherited = di.owner != &slide;
      item.id = s.id;
      item.shape = &s;
      item.table = std::make_unique<TableRender>(*t, fonts, extras);
      item.frame = s.bounds;
      impl.items.push_back(std::move(item));
      return;
    }
    item.inherited = di.owner != &slide;
    item.id = s.id;
    item.shape = &s;
    item.resources = deck.Resources(*di.owner, slide);
    item.drawing = draw::Drawing::FromShapeTree(s.drawNode, *item.resources);
    if (!item.drawing || item.drawing->IsHidden()) return;
    for (const draw::TextBox& box : item.drawing->TextBoxes()) {
      // The model's shapes' text, and SmartArt's (a text box's, with its style's font reference).
      const fw::slide::TextBody* body = box.content ? deck.TextOf(box.content, *di.owner, slide) : nullptr;
      TextBlock& block = item.texts.emplace_back();
      if (body && !body->Empty()) block.Build(fonts, box, *body, extras);
    }
    impl.items.push_back(std::move(item));
  }

  void Finish() {
    for (const SlideRender::Impl::Item& item : impl.items) {
      if (item.background) continue;
      if (item.table) {
        ShapeInfo info;
        info.id = item.inherited ? 0 : item.id;
        info.shape = item.shape;
        info.size = item.table->Extent();
        info.transform = draw::Matrix::Translate(item.frame.left, item.frame.top);
        info.bounds = Bounds(info.transform, info.size);
        info.fromMaster = item.inherited;
        out.shapes_.push_back(info);
        int i = 0;
        for (const draw::Rect& r : item.table->ParagraphBounds()) out.paragraphs_.push_back({info.id, i++, Transformed(info.transform, r)});
        for (const TextBlock::Link& l : item.table->Links()) out.links_.push_back({info.id, Transformed(info.transform, l.bounds), l.link});
        if (!item.inherited) Append(item.table->PlainText());
        continue;
      }
      const draw::Rect b = item.drawing->Bounds();
      const draw::Matrix place = draw::Matrix::Translate(b.left, b.top);
      for (const draw::Drawing::Part& p : item.drawing->Parts()) {
        if (p.id == 0 && !item.inherited) continue;  // SmartArt's own shapes
        ShapeInfo info;
        info.id = item.inherited ? 0 : p.id;
        info.shape = p.id == item.id ? item.shape : item.shape->owner ? item.shape->owner->FindShape(p.id) : nullptr;
        if (item.inherited && info.shape == nullptr) info.shape = item.shape;
        info.size = p.size;
        info.transform = p.transform * place;
        info.bounds = Bounds(info.transform, p.size);
        info.group = p.group;
        info.fromMaster = item.inherited;
        out.shapes_.push_back(info);
      }
      const std::vector<draw::TextBox>& boxes = item.drawing->TextBoxes();
      for (size_t t = 0; t < boxes.size() && t < item.texts.size(); ++t) {
        const draw::Matrix m = boxes[t].transform * place;
        const uint32_t id = item.inherited ? 0 : boxes[t].shape ? boxes[t].shape : item.id;
        int i = 0;
        for (const draw::Rect& r : item.texts[t].ParagraphBounds()) out.paragraphs_.push_back({id, i++, Transformed(m, r)});
        for (const TextBlock::Link& l : item.texts[t].Links()) out.links_.push_back({id, Transformed(m, l.bounds), l.link});
        if (!item.inherited) Append(item.texts[t].PlainText());
      }
    }
  }

  void Append(const std::wstring& t) {
    if (t.empty()) return;
    if (!out.text_.empty()) out.text_ += L'\n';
    out.text_ += t;
  }
};

SlideRender::SlideRender() : impl_(std::make_unique<Impl>()) {}
SlideRender::~SlideRender() = default;

std::unique_ptr<SlideRender> SlideRender::Make(fw::slide::Presentation& deck, const fw::slide::SlideBase& slide,
                                               text::Fonts& fonts) {
  std::unique_ptr<SlideRender> s(new SlideRender());
  Builder(deck, slide, fonts, *s).Run();
  return s;
}

void SlideRender::Draw(draw::Painter& p, const draw::Rect& frame, DrawFilter* filter) const {
  if (size_.width <= 0 || size_.height <= 0) return;
  p.Save();
  p.Transform(draw::Matrix::Scale(frame.Width() / size_.width, frame.Height() / size_.height) *
              draw::Matrix::Translate(frame.left, frame.top));
  // Nothing shows outside the slide, as in PowerPoint's show and PDF; no background is white.
  p.ClipRect({0, 0, size_.width, size_.height});
  if (impl_->items.empty() || !impl_->items.front().background)
    if (!filter || filter->Background()) p.FillRect({0, 0, size_.width, size_.height}, draw::Brush::Solid({1, 1, 1, 1}));
  for (const Impl::Item& item : impl_->items) {
    if (item.background) {
      if (!filter || filter->Background()) item.drawing->Draw(p, item.drawing->Bounds());
      continue;
    }
    if (item.table) {
      const uint32_t id = item.inherited ? 0 : item.id;
      if (filter && !filter->Shape(id)) continue;
      p.Save();
      p.Transform(draw::Matrix::Translate(item.frame.left, item.frame.top));
      item.table->Draw(p, filter, id);
      p.Restore();
      continue;
    }
    Hooks hooks(item, filter);
    item.drawing->Draw(p, item.drawing->Bounds(), hooks);
  }
  p.Restore();
}

void UseDeckFonts(text::Fonts& fonts, fw::slide::Presentation& deck) {
  std::vector<std::vector<uint8_t>> files;
  std::vector<std::wstring> names;
  for (const fw::slide::EmbeddedFont& f : deck.EmbeddedFonts()) {
    files.push_back(f.sfnt);
    names.push_back(f.typeface);
  }
  if (!files.empty()) fonts.UseDocumentFonts(files, names);
}

}  // namespace fw::slide::render
