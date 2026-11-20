// The .pptx reader (plan §8, P1): the presentation part, masters, layouts and themes at once;
// slides and their notes on first use.
#include <algorithm>
#include <cstring>

#include "core/Strings.h"
#include "draw/Sfnt.h"
#include "slide/Internal.h"

namespace fw::slide {

using namespace detail;

namespace detail {

std::string TargetOfType(const pkg::RelationshipSet* rels, std::string_view kind) {
  if (!rels) return {};
  for (const pkg::Relationship& r : rels->items) {
    std::string_view type = r.type;
    if (!r.external && type.size() > kind.size() && type.ends_with(kind) && type[type.size() - kind.size() - 1] == '/') {
      return r.targetPart;
    }
  }
  return {};
}

std::string TargetOf(const pkg::RelationshipSet* rels, std::string_view id) {
  if (!rels || id.empty()) return {};
  const pkg::Relationship* r = rels->Find(id);
  return r && !r->external ? r->targetPart : std::string();
}

std::unique_ptr<pkg::XmlDom> ParsePart(const pkg::Package& package, std::string_view part, std::string* problem) {
  std::vector<uint8_t> bytes;
  if (part.empty()) return nullptr;
  pkg::Status st = package.ReadPart(part, bytes);
  if (st.Ok()) {
    auto dom = std::make_unique<pkg::XmlDom>();
    st = dom->Parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), package.GetLimits(),
                    part);
    if (st.Ok()) return dom;
  }
  if (problem) *problem = std::string(part) + ": " + st.message;
  return nullptr;
}

const Master* MasterOf(const SlideBase& part) {
  const SlideBase* p = &part;
  for (int i = 0; i < 4 && p; ++i, p = p->parent) {
    if (p->kind == PartKind::Master || p->kind == PartKind::NotesMaster || p->kind == PartKind::HandoutMaster) {
      return static_cast<const Master*>(p);
    }
  }
  return nullptr;
}

}  // namespace detail

SlideBase::~SlideBase() = default;

const Shape* SlideBase::FindShape(uint32_t id) const {
  auto it = byId_.find(id);
  return it == byId_.end() ? nullptr : it->second;
}

const Shape* SlideBase::ShapeOfText(pugi::xml_node content) const {
  if (!content) return nullptr;
  auto it = byText_.find(content.internal_object());
  return it == byText_.end() ? nullptr : it->second;
}

std::wstring Slide::NotesText() const {
  std::wstring out;
  if (!notes) return out;
  const Shape* body = notes->FindPlaceholder(PlaceholderType::Body);
  if (!body) return out;
  pugi::xml_node txBody = P(body->node, "txBody");
  bool first = true;
  ForEachChild(txBody, [&](pugi::xml_node p) {
    if (!Is(p, kA, "p")) return true;
    if (!first) out += L'\n';
    first = false;
    ForEachChild(p, [&](pugi::xml_node r) {
      std::string_view k = LocalNameOf(r);
      if (k == "br") {
        out += L'\v';
      } else if (k == "r" || k == "fld") {
        std::string_view t = A(r, "t").text().get();
        out += fw::Utf8ToWide(t);
      }
      return true;
    });
    return true;
  });
  return out;
}

const TableCell* Table::Cell(int row, int column) const {
  if (row < 0 || column < 0 || row >= static_cast<int>(rows.size()) || column >= static_cast<int>(columns.size())) {
    return nullptr;
  }
  size_t i = static_cast<size_t>(row) * columns.size() + static_cast<size_t>(column);
  return i < cells.size() ? &cells[i] : nullptr;
}

bool TextBody::Empty() const {
  for (const Paragraph& p : paragraphs) {
    for (const TextRun& r : p.runs) {
      if (r.kind != TextRun::Kind::Text || !r.text.empty()) return false;
    }
  }
  return true;
}

// ---- Loading

class Loader {
 public:
  static std::unique_ptr<Presentation> Read(std::unique_ptr<pkg::Package> package, pkg::Status& status);
  static void LoadSlide(Presentation& deck, size_t index);

 private:
  static bool InitPart(Presentation& deck, SlideBase& part, PartKind kind, const std::string& name);
  static Master* LoadMaster(Presentation& deck, const std::string& part, PartKind kind);
  static Layout* LoadLayout(Presentation& deck, const std::string& part, Master* master);
  static Layout* FindLayout(Presentation& deck, const std::string& part);
  static const NotesSlide* LoadNotes(Presentation& deck, const std::string& part, Slide& slide);
  static void Finish(Presentation& deck, SlideBase& part, pugi::xml_node spTree, draw::Size size);
  static void Index(SlideBase& part, Shape& s);
  static void ReadPresentation(Presentation& deck, pugi::xml_node root);
  static void ReadPresProps(Presentation& deck, const std::string& part);
};

namespace {

FileFormat FormatOf(std::string_view contentType) {
  if (contentType.find("presentationml.slideshow") != std::string_view::npos) return FileFormat::Ppsx;
  if (contentType.find("presentationml.template") != std::string_view::npos) return FileFormat::Potx;
  if (contentType.find("powerpoint.slideshow.macroEnabled") != std::string_view::npos) return FileFormat::Ppsm;
  if (contentType.find("powerpoint.template.macroEnabled") != std::string_view::npos) return FileFormat::Potm;
  if (contentType.find("powerpoint.presentation.macroEnabled") != std::string_view::npos) return FileFormat::Pptm;
  return FileFormat::Pptx;
}

// The direct child (or the mc:AlternateContent holding it) named p:local.
pugi::xml_node RawChild(pugi::xml_node parent, std::string_view local) {
  for (pugi::xml_node c = parent.first_child(); c; c = c.next_sibling()) {
    if (c.type() != pugi::node_element) continue;
    if (Is(c, kP, local)) return c;
    if (Is(c, fw::pkg::mce::kNamespace, "AlternateContent")) {
      for (pugi::xml_node b = c.first_child(); b; b = b.next_sibling()) {
        for (pugi::xml_node g = b.first_child(); g; g = g.next_sibling()) {
          if (Is(g, kP, local)) return c;
        }
      }
    }
  }
  return {};
}

uint32_t U32(pugi::xml_node n, const char* attr, uint32_t def = 0) {
  double d = Number(n, attr, def);
  return d >= 0 && d <= 4294967295.0 ? static_cast<uint32_t>(d) : def;
}

}  // namespace

bool Loader::InitPart(Presentation& deck, SlideBase& part, PartKind kind, const std::string& name) {
  part.kind = kind;
  part.part = name;
  part.relationships = deck.package_->RelationshipsOf(name);
  static const pkg::RelationshipSet kNone;
  if (!part.relationships) part.relationships = &kNone;
  std::string problem;
  part.dom_ = ParsePart(*deck.package_, name, &problem);
  if (!part.dom_) {
    deck.problems_.push_back(problem.empty() ? name + ": missing" : problem);
    return false;
  }
  part.root = part.dom_->Root();
  pugi::xml_node cSld = P(part.root, "cSld");
  part.name = cSld.attribute("name").value();
  part.showMasterShapes = Bool(part.root, "showMasterSp", true);
  return true;
}

void Loader::Index(SlideBase& part, Shape& s) {
  s.owner = &part;
  part.byId_.emplace(s.id, &s);
  if (s.textNode) part.byText_.emplace(s.textNode.internal_object(), &s);
  for (Shape& c : s.children) Index(part, c);
}

void Loader::Finish(Presentation& deck, SlideBase& part, pugi::xml_node spTree, draw::Size size) {
  ReadShapeTree(part, spTree, deck);
  for (Shape& s : part.shapes) Index(part, s);
  ResolveBackground(part, size);
}

Master* Loader::LoadMaster(Presentation& deck, const std::string& name, PartKind kind) {
  auto m = std::make_unique<Master>();
  if (!InitPart(deck, *m, kind, name)) {
    m->root = {};
  }
  // The theme, shared by parts that name the same one.
  std::string themePart = TargetOfType(m->relationships, "theme");
  if (!themePart.empty()) {
    std::string key = pkg::PartKey(themePart);
    auto it = deck.themes_.find(key);
    if (it == deck.themes_.end()) {
      std::vector<uint8_t> bytes;
      std::shared_ptr<const draw::Theme> theme;
      if (deck.package_->ReadPart(themePart, bytes).Ok()) {
        theme = draw::Theme::Parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
      }
      if (!theme) deck.problems_.push_back(themePart + ": not a theme Featherweight can read");
      it = deck.themes_.emplace(key, theme).first;
    }
    m->theme = it->second;
  }
  if (!m->theme) m->theme = draw::Theme::Default();
  if (pugi::xml_node map = P(m->root, "clrMap")) m->colorMap = draw::ColorMap::Read(map);
  if (kind == PartKind::Master) {
    pugi::xml_node styles = P(m->root, "txStyles");
    m->titleStyle = P(styles, "titleStyle");
    m->bodyStyle = P(styles, "bodyStyle");
    m->otherStyle = P(styles, "otherStyle");
  } else {
    m->bodyStyle = P(m->root, "notesStyle");
  }
  Master* raw = m.get();
  if (kind == PartKind::Master) {
    deck.masters_.push_back(std::move(m));
  } else if (kind == PartKind::NotesMaster) {
    deck.notesMaster_ = std::move(m);
  }
  Finish(deck, *raw, P(P(raw->root, "cSld"), "spTree"),
         kind == PartKind::Master ? deck.slideSize_ : deck.notesSize_);
  if (kind == PartKind::Master) {
    // Its layouts, in the order it lists them.
    ForEachChild(P(raw->root, "sldLayoutIdLst"), [&](pugi::xml_node id) {
      std::string layout = TargetOf(raw->relationships, RelId(id, "id"));
      if (!layout.empty() && !FindLayout(deck, layout)) LoadLayout(deck, layout, raw);
      return deck.layouts_.size() < 1000;
    });
  }
  return raw;
}

Layout* Loader::FindLayout(Presentation& deck, const std::string& part) {
  std::string key = pkg::PartKey(part);
  for (auto& l : deck.layouts_) {
    if (pkg::PartKey(l->part) == key) return l.get();
  }
  return nullptr;
}

Layout* Loader::LoadLayout(Presentation& deck, const std::string& name, Master* master) {
  auto l = std::make_unique<Layout>();
  InitPart(deck, *l, PartKind::Layout, name);
  if (!master) {
    // A layout no master lists: its own master, read now.
    std::string m = TargetOfType(l->relationships, "slideMaster");
    for (auto& existing : deck.masters_) {
      if (!m.empty() && pkg::PartKey(existing->part) == pkg::PartKey(m)) master = existing.get();
    }
    if (!master && !m.empty() && deck.masters_.size() < 100) master = LoadMaster(deck, m, PartKind::Master);
    if (!master && !deck.masters_.empty()) master = deck.masters_.front().get();
  }
  l->master = master;
  l->parent = master;
  l->type = l->root.attribute("type").value();
  if (l->type.empty() && l->root) l->type = "cust";
  l->theme = master ? master->theme : draw::Theme::Default();
  if (master) l->colorMap = master->colorMap;
  if (pugi::xml_node over = A(P(l->root, "clrMapOvr"), "overrideClrMapping")) l->colorMap = draw::ColorMap::Read(over);
  Layout* raw = l.get();
  deck.layouts_.push_back(std::move(l));
  if (master) master->layouts.push_back(raw);
  Finish(deck, *raw, P(P(raw->root, "cSld"), "spTree"), deck.slideSize_);
  return raw;
}

const NotesSlide* Loader::LoadNotes(Presentation& deck, const std::string& name, Slide& slide) {
  auto n = std::make_unique<NotesSlide>();
  if (!InitPart(deck, *n, PartKind::Notes, name)) return nullptr;
  if (!deck.notesMaster_) {
    std::string m = TargetOfType(n->relationships, "notesMaster");
    if (!m.empty()) LoadMaster(deck, m, PartKind::NotesMaster);
  }
  const Master* master = deck.notesMaster_.get();
  n->parent = master;
  n->theme = master && master->theme ? master->theme : slide.theme;
  n->colorMap = master ? master->colorMap : slide.colorMap;
  if (pugi::xml_node over = A(P(n->root, "clrMapOvr"), "overrideClrMapping")) n->colorMap = draw::ColorMap::Read(over);
  NotesSlide* raw = n.get();
  deck.notes_.push_back(std::move(n));
  Finish(deck, *raw, P(P(raw->root, "cSld"), "spTree"), deck.notesSize_);
  return raw;
}

void Loader::LoadSlide(Presentation& deck, size_t index) {
  Presentation::SlideRef& ref = deck.slideRefs_[index];
  auto s = std::make_unique<Slide>();
  s->id = ref.id;
  s->index = index;
  s->number = deck.firstSlideNumber_ + static_cast<int>(index);
  bool ok = InitPart(deck, *s, PartKind::Slide, ref.part);
  std::string layoutPart = TargetOfType(s->relationships, "slideLayout");
  Layout* layout = layoutPart.empty() ? nullptr : FindLayout(deck, layoutPart);
  if (!layout && !layoutPart.empty() && ok && deck.layouts_.size() < 1000) layout = LoadLayout(deck, layoutPart, nullptr);
  if (!layout && !deck.layouts_.empty()) layout = deck.layouts_.front().get();
  s->layout = layout;
  s->parent = layout;
  s->master = layout ? layout->master : (deck.masters_.empty() ? nullptr : deck.masters_.front().get());
  if (!s->parent) s->parent = s->master;
  s->theme = layout ? layout->theme : s->master ? s->master->theme : draw::Theme::Default();
  if (layout) {
    s->colorMap = layout->colorMap;
  } else if (s->master) {
    s->colorMap = s->master->colorMap;
  }
  if (ok) {
    if (pugi::xml_node over = A(P(s->root, "clrMapOvr"), "overrideClrMapping")) s->colorMap = draw::ColorMap::Read(over);
    s->hidden = !Bool(s->root, "show", true);
    s->transition = RawChild(s->root, "transition");
    s->timing = RawChild(s->root, "timing");
  }
  Slide* raw = s.get();
  ref.slide = std::move(s);
  Finish(deck, *raw, P(P(raw->root, "cSld"), "spTree"), deck.slideSize_);
  std::string notes = TargetOfType(raw->relationships, "notesSlide");
  if (!notes.empty()) raw->notes = LoadNotes(deck, notes, *raw);
}

void Loader::ReadPresProps(Presentation& deck, const std::string& part) {
  std::unique_ptr<pkg::XmlDom> dom = ParsePart(*deck.package_, part, nullptr);
  if (!dom) return;
  pugi::xml_node show = P(dom->Root(), "showPr");
  if (!show) return;
  ShowSettings& s = deck.show_;
  s.loop = Bool(show, "loop", false);
  s.narration = Bool(show, "showNarration", true);
  s.animations = Bool(show, "showAnimation", true);
  s.useTimings = Bool(show, "useTimings", true);
  s.kiosk = static_cast<bool>(P(show, "kiosk"));
  s.browse = static_cast<bool>(P(show, "browse"));
  if (pugi::xml_node range = P(show, "sldRg")) {
    int count = static_cast<int>(deck.slideRefs_.size());
    s.firstSlide = std::clamp(static_cast<int>(Number(range, "st", 1)) - 1, 0, std::max(count - 1, 0));
    s.lastSlide = std::clamp(static_cast<int>(Number(range, "end", count)) - 1, s.firstSlide, std::max(count - 1, 0));
  }
  if (pugi::xml_node custom = P(show, "custShow")) {
    s.hasCustomShow = true;
    s.customShow = U32(custom, "id");
  }
}

void Loader::ReadPresentation(Presentation& deck, pugi::xml_node root) {
  const pkg::RelationshipSet* rels = deck.package_->RelationshipsOf(deck.presentationPart_);
  if (pugi::xml_node sz = P(root, "sldSz")) {
    float w = Emu(sz, "cx", 9144000), h = Emu(sz, "cy", 6858000);
    // ST_SlideSizeCoordinate: 914400 to 51206400 EMU.
    if (w >= 72 && w <= 4032 && h >= 72 && h <= 4032) deck.slideSize_ = {w, h};
  }
  if (pugi::xml_node sz = P(root, "notesSz")) {
    float w = Emu(sz, "cx", 6858000), h = Emu(sz, "cy", 9144000);
    if (w >= 72 && w <= 4032 && h >= 72 && h <= 4032) deck.notesSize_ = {w, h};
  }
  deck.firstSlideNumber_ = std::clamp(static_cast<int>(Number(root, "firstSlideNum", 1)), -9999, 9999);
  deck.rtl_ = Bool(root, "rtl", false);
  deck.defaultTextStyle_ = P(root, "defaultTextStyle");

  // Slides, then masters (which read their layouts).
  ForEachChild(P(root, "sldIdLst"), [&](pugi::xml_node id) {
    if (!Is(id, kP, "sldId")) return true;
    std::string part = TargetOf(rels, RelId(id, "id"));
    if (part.empty()) {
      deck.problems_.push_back("A slide in the slide list has no part; it was left out.");
      return true;
    }
    Presentation::SlideRef ref;
    ref.id = U32(id, "id");
    ref.part = part;
    deck.slideRefs_.push_back(std::move(ref));
    return deck.slideRefs_.size() < 100000;
  });
  ForEachChild(P(root, "sldMasterIdLst"), [&](pugi::xml_node id) {
    std::string part = TargetOf(rels, RelId(id, "id"));
    if (!part.empty() && deck.masters_.size() < 100) LoadMaster(deck, part, PartKind::Master);
    return true;
  });
  if (deck.masters_.empty()) {
    std::string part = TargetOfType(rels, "slideMaster");
    if (!part.empty()) LoadMaster(deck, part, PartKind::Master);
  }

  // Custom shows and sections, as slide indexes.
  ForEachChild(P(root, "custShowLst"), [&](pugi::xml_node show) {
    CustomShow cs;
    cs.id = U32(show, "id");
    cs.name = show.attribute("name").value();
    ForEachChild(P(show, "sldLst"), [&](pugi::xml_node sld) {
      int i = deck.SlideIndexOfPart(TargetOf(rels, RelId(sld, "id")));
      if (i >= 0) cs.slides.push_back(static_cast<size_t>(i));
      return cs.slides.size() < 100000;
    });
    deck.customShows_.push_back(std::move(cs));
    return deck.customShows_.size() < 1000;
  });
  ForEachChild(P(root, "extLst"), [&](pugi::xml_node ext) {
    pugi::xml_node list = Child(ext, kP14, "sectionLst");
    if (!list) return true;
    ForEachChild(list, [&](pugi::xml_node sec) {
      Section s;
      s.name = sec.attribute("name").value();
      ForEachChild(Child(sec, kP14, "sldIdLst"), [&](pugi::xml_node id) {
        int i = deck.SlideIndexOfId(U32(id, "id"));
        if (i >= 0) s.slides.push_back(static_cast<size_t>(i));
        return s.slides.size() < 100000;
      });
      deck.sections_.push_back(std::move(s));
      return deck.sections_.size() < 10000;
    });
    return false;
  });

  // Embedded fonts: decoded on first use.
  ForEachChild(P(root, "embeddedFontLst"), [&](pugi::xml_node font) {
    std::wstring typeface = fw::Utf8ToWide(P(font, "font").attribute("typeface").value());
    std::array<std::string, 4> parts;
    const char* kinds[] = {"regular", "bold", "italic", "boldItalic"};
    for (int i = 0; i < 4; ++i) parts[i] = TargetOf(rels, RelId(P(font, kinds[i]), "id"));
    if (!typeface.empty()) deck.fontParts_.emplace_back(std::move(typeface), std::move(parts));
    return deck.fontParts_.size() < 256;
  });

  std::string props = TargetOfType(rels, "presProps");
  if (!props.empty()) ReadPresProps(deck, props);
  deck.hasMacros_ = !TargetOfType(rels, "vbaProject").empty();
}

std::unique_ptr<Presentation> Loader::Read(std::unique_ptr<pkg::Package> package, pkg::Status& status) {
  status = {};
  if (!package) {
    status = pkg::Status::Fail(pkg::Status::Code::Damaged, "The file isn't a presentation.");
    return nullptr;
  }
  std::unique_ptr<Presentation> deck(new Presentation());
  deck->package_ = std::move(package);
  deck->images_ = std::make_shared<draw::ImageCache>();
  const pkg::Package& pk = *deck->package_;
  std::string main = TargetOfType(pk.RelationshipsOf("/"), "officeDocument");
  std::string problem;
  deck->presentationDom_ = main.empty() ? nullptr : ParsePart(pk, main, &problem);
  pugi::xml_node root = deck->presentationDom_ ? deck->presentationDom_->Root() : pugi::xml_node();
  if (!root || !Is(root, kP, "presentation")) {
    if (!problem.empty()) {
      status = pkg::Status::Fail(pkg::Status::Code::Damaged,
                                 "The presentation can't be opened, because a part of it is damaged (" + problem + ").");
    } else {
      status = pkg::Status::Fail(pkg::Status::Code::Unsupported, "The file isn't a PowerPoint presentation.");
    }
    return nullptr;
  }
  deck->presentationPart_ = main;
  deck->format_ = FormatOf(pk.ContentTypeOf(main));
  ReadPresentation(*deck, root);
  return deck;
}

std::unique_ptr<Presentation> Read(std::unique_ptr<pkg::Package> package, pkg::Status& status) {
  return Loader::Read(std::move(package), status);
}

// ---- Presentation

Presentation::Presentation() = default;
Presentation::~Presentation() = default;

uint32_t Presentation::SlideId(size_t index) const { return index < slideRefs_.size() ? slideRefs_[index].id : 0; }

int Presentation::SlideIndexOfId(uint32_t id) const {
  for (size_t i = 0; i < slideRefs_.size(); ++i) {
    if (slideRefs_[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

int Presentation::SlideIndexOfPart(std::string_view part) const {
  if (part.empty()) return -1;
  std::string key = pkg::PartKey(part);
  for (size_t i = 0; i < slideRefs_.size(); ++i) {
    if (pkg::PartKey(slideRefs_[i].part) == key) return static_cast<int>(i);
  }
  return -1;
}

const Slide* Presentation::GetSlide(size_t index) {
  std::lock_guard lock(mutex_);
  if (index >= slideRefs_.size()) return nullptr;
  if (!slideRefs_[index].slide) Loader::LoadSlide(*this, index);
  return slideRefs_[index].slide.get();
}

void Presentation::LoadAll() {
  for (size_t i = 0; i < slideRefs_.size(); ++i) GetSlide(i);
}

std::vector<DrawItem> Presentation::DrawList(const SlideBase& slide) const {
  std::vector<DrawItem> items;
  auto add = [&](const SlideBase& part, bool placeholders) {
    for (const Shape& s : part.shapes) {
      if (placeholders || !s.placeholder) items.push_back({&s, &part});
    }
  };
  // The chain: the part, then its parents up to the master, each drawn only while every part
  // below it shows its master's shapes.
  std::vector<const SlideBase*> chain{&slide};
  while (chain.size() < 4 && chain.back()->parent && chain.back()->showMasterShapes) chain.push_back(chain.back()->parent);
  for (size_t i = chain.size(); i-- > 1;) add(*chain[i], false);
  add(slide, true);
  return items;
}

std::unique_ptr<draw::DrawingResources> Presentation::Resources(const SlideBase& owner, const SlideBase& shownOn) const {
  return std::make_unique<draw::PackageResources>(*package_, owner.part, shownOn.theme, shownOn.colorMap, images_);
}

const TextBody* Presentation::Text(const Shape& shape, const SlideBase& shownOn) {
  if (!shape.textNode || !shape.owner) return nullptr;
  std::lock_guard lock(mutex_);
  auto key = std::make_pair(static_cast<const void*>(&shape), static_cast<const void*>(&shownOn));
  auto it = texts_.find(key);
  if (it == texts_.end()) it = texts_.emplace(key, ResolveText(*this, shape, *shape.owner, shownOn)).first;
  return it->second.get();
}

const TextBody* Presentation::TextOf(pugi::xml_node txBody, const SlideBase& owner, const SlideBase& shownOn) {
  if (!txBody) return nullptr;
  if (const Shape* s = owner.ShapeOfText(txBody)) return Text(*s, shownOn);
  std::lock_guard lock(mutex_);
  auto key = std::make_pair(static_cast<const void*>(txBody.internal_object()), static_cast<const void*>(&shownOn));
  auto it = texts_.find(key);
  if (it == texts_.end()) {
    Shape standIn;
    standIn.node = standIn.drawNode = txBody.parent();
    standIn.textNode = txBody;
    standIn.owner = &owner;
    it = texts_.emplace(key, ResolveText(*this, standIn, owner, shownOn)).first;
  }
  return it->second.get();
}

const std::vector<EmbeddedFont>& Presentation::EmbeddedFonts() {
  std::lock_guard lock(mutex_);
  if (fontsDecoded_) return fonts_;
  fontsDecoded_ = true;
  // Embedded OpenType (.fntdata), each style that decodes.
  for (const auto& [typeface, parts] : fontParts_) {
    for (int i = 0; i < 4; ++i) {
      if (parts[i].empty()) continue;
      std::vector<uint8_t> bytes;
      if (!package_->ReadPart(parts[i], bytes).Ok()) continue;
      EmbeddedFont f;
      f.typeface = typeface;
      f.bold = i == 1 || i == 3;
      f.italic = i == 2 || i == 3;
      f.sfnt = draw::sfnt::DecodeEot(bytes);
      if (f.sfnt.empty()) {
        problems_.push_back(parts[i] + ": an embedded font Featherweight can't read");
        continue;
      }
      fonts_.push_back(std::move(f));
    }
  }
  return fonts_;
}

std::vector<std::string> Presentation::Problems() const {
  std::lock_guard lock(mutex_);
  return problems_;
}

}  // namespace fw::slide
