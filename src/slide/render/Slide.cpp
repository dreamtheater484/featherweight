#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>

#include "draw/Shapes.h"
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

// A family the PC doesn't have, as PowerPoint replaces it: fwtext's table (Aptos, Helvetica...),
// Windows' FontSubstitutes, a typewriter or code face in Courier New, an installed family named
// among its words ("Adobe Garamond Pro" in Garamond, PowerPoint's PDF), everything else in Calibri
// (p1-render's corpus survey: Whitney, Circular Std, Futura PT, Source Sans Pro, Monaco,
// Liberation Serif, Open Sans SemiBold all drawn in Calibri, whatever the theme's fonts).
std::wstring PresentationSubstitute(std::wstring_view family, text::Fonts& fonts) {
  if (family.empty() || family.size() > 64) return {};
  if (std::wstring_view known = text::Fonts::BuiltInSubstitute(family); !known.empty()) return std::wstring(known);
  const std::wstring name(family);
  wchar_t value[128] = {};
  DWORD bytes = sizeof value;
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontSubstitutes", name.c_str(),
                   RRF_RT_REG_SZ, nullptr, value, &bytes) == ERROR_SUCCESS && value[0]) {
    std::wstring s = value;
    if (size_t comma = s.find(L','); comma != std::wstring::npos) s.resize(comma);  // "Arial,0": a charset
    return s;
  }
  std::wstring lower = name;
  for (wchar_t& c : lower) c = static_cast<wchar_t>(std::towlower(c));
  for (std::wstring_view word : {L"mono", L"courier", L"consol", L"typewriter", L"code"})
    if (lower.find(word) != std::wstring::npos) return L"Courier New";
  // The longest run of whole words that is an installed family.
  std::vector<std::wstring_view> words;
  for (size_t at = 0; at < family.size();) {
    const size_t end = std::min(family.find(L' ', at), family.size());
    if (end > at) words.push_back(family.substr(at, end - at));
    at = end + 1;
  }
  std::wstring best;
  for (size_t i = 0; i < words.size(); ++i)
    for (size_t j = i + 1; j <= words.size() && j - i < words.size(); ++j) {
      const std::wstring run(words[i].data(), words[j - 1].data() + words[j - 1].size() - words[i].data());
      if (run.size() > best.size() && fonts.Installed(run)) best = run;
    }
  return best.empty() ? L"Calibri" : best;
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
    std::vector<const fw::slide::TextBody*> bodies;  // what each was laid out from (null: none)
    std::unique_ptr<TableRender> table;
    bool backgroundFill = false;  // p:sp useBgFill
    draw::Rect frame;  // a table's graphic frame, slide points
  };
  std::vector<Item> items;
  // Empty placeholders, for the editor's prompts: laid out on first use.
  struct Prompt {
    size_t item = 0, box = 0;
    uint32_t id = 0;
    std::unique_ptr<fw::slide::TextBody> body;
    std::unique_ptr<TextBlock> block;
  };
  std::vector<Prompt> prompts;
  TextExtras extras;
};

namespace {

// What an empty placeholder says in the editor, by its type (PowerPoint's words).
std::wstring PromptText(fw::slide::PlaceholderType t, bool notes) {
  using T = fw::slide::PlaceholderType;
  if (notes) return t == T::Body ? L"Click to add notes" : L"";
  switch (t) {
    case T::Title:
    case T::CenteredTitle: return L"Click to add title";
    case T::Subtitle: return L"Click to add subtitle";
    case T::Body:
    case T::Object: return L"Click to add text";
    case T::Picture: return L"Click icon to add picture";
    case T::Table: return L"Click icon to add table";
    case T::Chart: return L"Click icon to add chart";
    case T::Media: return L"Click icon to add media";
    case T::Diagram:
    case T::ClipArt: return L"Click icon to add a graphic";
    default: return L"";
  }
}

}  // namespace

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
    impl.extras = extras;
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
    item.backgroundFill = draw::xml::Bool(s.node, "useBgFill", false);
    if (!item.drawing || item.drawing->IsHidden()) return;
    for (const draw::TextBox& box : item.drawing->TextBoxes()) {
      // The model's shapes' text, and SmartArt's (a text box's, with its style's font reference).
      const fw::slide::TextBody* body = box.content ? deck.TextOf(box.content, *di.owner, slide) : nullptr;
      TextBlock& block = item.texts.emplace_back();
      item.bodies.push_back(body);
      if (body && !body->Empty()) block.Build(fonts, box, *body, extras);
      else if (body && !item.inherited && s.placeholder && box.content == s.textNode) Prompt(s, *body, item.texts.size() - 1);
    }
    impl.items.push_back(std::move(item));
  }

  // An empty placeholder's prompt, in the look its text would have: the layout's own words when it
  // has a custom prompt, else PowerPoint's for its type.
  void Prompt(const fw::slide::Shape& s, const fw::slide::TextBody& body, size_t box) {
    const bool notes = slide.kind == fw::slide::PartKind::Notes;
    auto prompt = std::make_unique<fw::slide::TextBody>();
    prompt->body = body.body;
    const fw::slide::Shape* custom = s.inherits;
    while (custom && !custom->placeholder.customPrompt) custom = custom->inherits;
    const fw::slide::TextBody* own = custom ? deck.Text(*custom, slide) : nullptr;
    if (own && !own->Empty()) {
      prompt->paragraphs = own->paragraphs;
    } else {
      const std::wstring words = PromptText(s.placeholder.type, notes);
      if (words.empty()) return;
      fw::slide::Paragraph p = body.paragraphs.empty() ? fw::slide::Paragraph{} : body.paragraphs.front();
      fw::slide::TextRun run;
      run.props = p.endProps;
      run.text = words;
      p.runs = {run};
      p.number = 1;
      prompt->paragraphs.push_back(std::move(p));
    }
    impl.prompts.push_back({impl.items.size(), box, s.id, std::move(prompt), nullptr});
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
  const draw::Drawing* background = !impl_->items.empty() && impl_->items.front().background ? impl_->items.front().drawing.get() : nullptr;
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
    // A shape filled with the slide's background (useBgFill): the background, inside its outline.
    if (item.backgroundFill && background && (!filter || filter->Shape(item.inherited ? 0 : item.id))) {
      draw::Path outline;
      const draw::Rect b = item.drawing->Bounds();
      for (const draw::Path::Polygon& poly : item.drawing->WrapOutline()) {
        for (size_t i = 0; i < poly.points.size(); ++i) {
          const draw::Point q{poly.points[i].x + b.left, poly.points[i].y + b.top};
          if (i == 0) outline.MoveTo(q);
          else outline.LineTo(q);
        }
        outline.Close();
      }
      p.Save();
      p.ClipPath(outline);
      background->Draw(p, background->Bounds());
      p.Restore();
    }
    Hooks hooks(item, filter);
    item.drawing->Draw(p, item.drawing->Bounds(), hooks);
  }
  p.Restore();
}

bool SlideRender::FindText(uint32_t shape, int row, int column, TextFrame& out) const {
  if (shape == 0) return false;
  for (const Impl::Item& item : impl_->items) {
    if (item.inherited || item.background) continue;
    if (item.table) {
      if (item.id != shape || row < 0) continue;
      out.toSlide = draw::Matrix::Translate(item.frame.left, item.frame.top);
      return item.table->CellFrame(row, column, out.box, out.firstParagraph, out.paragraphs, out.contentHeight);
    }
    if (row >= 0) continue;
    const std::vector<draw::TextBox>& boxes = item.drawing->TextBoxes();
    const draw::Rect b = item.drawing->Bounds();
    for (size_t t = 0; t < boxes.size() && t < item.texts.size(); ++t) {
      const uint32_t id = boxes[t].shape ? boxes[t].shape : item.id;
      if (id != shape) continue;
      out.box = boxes[t];
      out.toSlide = boxes[t].transform * draw::Matrix::Translate(b.left, b.top);
      out.firstParagraph = 0;
      out.paragraphs = static_cast<int>(item.texts[t].ParagraphCount());
      out.contentHeight = item.texts[t].ContentHeight();
      return true;
    }
  }
  return false;
}

std::vector<SlideRender::TextOnSlide> SlideRender::Texts() const {
  std::vector<TextOnSlide> out;
  for (const Impl::Item& item : impl_->items) {
    if (item.inherited || item.background || item.table || !item.drawing) continue;
    const std::vector<draw::TextBox>& boxes = item.drawing->TextBoxes();
    const draw::Rect b = item.drawing->Bounds();
    for (size_t t = 0; t < boxes.size() && t < item.texts.size() && t < item.bodies.size(); ++t) {
      if (!item.bodies[t] || item.bodies[t]->Empty()) continue;
      TextOnSlide x;
      x.shape = boxes[t].shape ? boxes[t].shape : item.id;
      x.block = &item.texts[t];
      x.body = item.bodies[t];
      x.toSlide = item.texts[t].ToSlide(boxes[t].transform * draw::Matrix::Translate(b.left, b.top));
      out.push_back(x);
    }
  }
  return out;
}

bool SlideRender::CellAt(uint32_t shape, draw::Point at, int& row, int& column) const {
  for (const Impl::Item& item : impl_->items)
    if (item.table && !item.inherited && item.id == shape)
      return item.table->CellAt({at.x - item.frame.left, at.y - item.frame.top}, row, column);
  return false;
}

bool SlideRender::IsEmptyPlaceholder(uint32_t shape) const {
  for (const Impl::Prompt& p : impl_->prompts)
    if (p.id == shape) return true;
  return false;
}

void SlideRender::DrawPrompts(draw::Painter& p, const draw::Rect& frame, text::Fonts& fonts, uint32_t skip) {
  if (impl_->prompts.empty() || size_.width <= 0 || size_.height <= 0) return;
  p.Save();
  p.Transform(draw::Matrix::Scale(frame.Width() / size_.width, frame.Height() / size_.height) *
              draw::Matrix::Translate(frame.left, frame.top));
  for (Impl::Prompt& pr : impl_->prompts) {
    if (pr.id == skip || pr.item >= impl_->items.size()) continue;
    const Impl::Item& item = impl_->items[pr.item];
    const std::vector<draw::TextBox>& boxes = item.drawing->TextBoxes();
    if (pr.box >= boxes.size()) continue;
    if (!pr.block) {
      pr.block = std::make_unique<TextBlock>();
      pr.block->Build(fonts, boxes[pr.box], *pr.body, impl_->extras);
    }
    const draw::Rect b = item.drawing->Bounds();
    p.Save();
    p.Transform(boxes[pr.box].transform * draw::Matrix::Translate(b.left, b.top));
    // The placeholder's outline, dashed, as the editor marks an empty one.
    const draw::Rect r{boxes[pr.box].rect.left - boxes[pr.box].insets.left, boxes[pr.box].rect.top - boxes[pr.box].insets.top,
                       boxes[pr.box].rect.right + boxes[pr.box].insets.right,
                       boxes[pr.box].rect.bottom + boxes[pr.box].insets.bottom};
    draw::Path outline;
    outline.MoveTo({r.left, r.top});
    outline.LineTo({r.right, r.top});
    outline.LineTo({r.right, r.bottom});
    outline.LineTo({r.left, r.bottom});
    outline.Close();
    draw::Pen pen;
    pen.brush = draw::Brush::Solid({0.55f, 0.55f, 0.55f, 1});
    pen.width = 0.75f;
    pen.dashes = {4, 3};
    p.StrokePath(outline, pen);
    pr.block->Draw(p, nullptr, pr.id);
    p.Restore();
  }
  p.Restore();
}

namespace {

// Where an sfnt font's table `tag` starts, from its table directory; 0 if it has none.
size_t TableAt(const std::vector<uint8_t>& sfnt, const char (&tag)[5]) {
  if (sfnt.size() < 12) return 0;
  const size_t count = static_cast<size_t>(sfnt[4]) << 8 | sfnt[5];
  for (size_t i = 0; i < count && 12 + 16 * i + 16 <= sfnt.size(); ++i) {
    const uint8_t* e = sfnt.data() + 12 + 16 * i;
    if (std::memcmp(e, tag, 4) == 0) return static_cast<size_t>(e[8]) << 24 | e[9] << 16 | e[10] << 8 | e[11];
  }
  return 0;
}

}  // namespace

void UseDeckFonts(text::Fonts& fonts, fw::slide::Presentation& deck, bool directWrite) {
  std::vector<std::vector<uint8_t>> files;
  std::vector<std::wstring> names;
  for (const fw::slide::EmbeddedFont& f : deck.EmbeddedFonts()) {
    std::vector<uint8_t> sfnt = f.sfnt;
    if (TableAt(sfnt, "CFF ") > 0) {
      // CFF outlines under TrueType's version tag (cpb deck: RijksoverheidSans): DirectWrite looks
      // for glyf outlines and draws nothing; tagged as the OpenType font it is, it draws. Its lines
      // keep the heights they had (Windows' metrics, which PowerPoint's PDF matches), so its
      // USE_TYPO_METRICS bit, which DirectWrite honours only for the OpenType tag, goes.
      static constexpr uint8_t kTrueType[] = {0, 1, 0, 0};
      if (std::memcmp(sfnt.data(), kTrueType, 4) == 0) {
        std::memcpy(sfnt.data(), "OTTO", 4);
        if (const size_t os2 = TableAt(sfnt, "OS/2"); os2 > 0 && os2 + 64 <= sfnt.size()) sfnt[os2 + 63] &= ~0x80;
      }
      // One DirectWrite still can't outline would leave its text blank on screen: drawn in the
      // font's substitute instead, noted once in the debugger's log.
      if (directWrite && !draw::OutlinesInDirectWrite(sfnt)) {
        OutputDebugStringW((L"Featherweight: embedded font " + f.typeface +
                            L" has no outlines DirectWrite draws; its text is drawn in a substitute\n").c_str());
        continue;
      }
    }
    files.push_back(std::move(sfnt));
    names.push_back(f.typeface);
  }
  if (!files.empty()) fonts.UseDocumentFonts(files, names);
  fonts.SetSubstitute([&fonts](std::wstring_view family) { return PresentationSubstitute(family, fonts); });
  fonts.SetGdiFamilies(true);
  fonts.SetPowerPointFallback(true);
}

}  // namespace fw::slide::render
