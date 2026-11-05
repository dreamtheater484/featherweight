#include "doc/model/Document.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include "doc/model/Internal.h"
#include "doc/model/Utf.h"

namespace fw::doc {

using namespace names;

namespace {

// Pre-order successor of `id` inside the subtree of `top`, not descending into `id`'s children
// when `skipChildren`.
ElemId NextInTree(const Tree& t, ElemId id, ElemId top, bool skipChildren = false) {
  // A shared property set is walked through its first user only: its parent links lead there.
  if (!skipChildren && !(t.elems[id].flags & kSharedKids) && t.elems[id].first != kNoElem) return t.elems[id].first;
  while (id != top && id != kNoElem) {
    if (t.elems[id].next != kNoElem) return t.elems[id].next;
    id = t.elems[id].parent;
  }
  return kNoElem;
}

bool InTable(Node n) {
  for (Node p = n.Parent(); p; p = p.Parent())
    if (p.Is(kTc)) return true;
  return false;
}

}  // namespace

Document::Document() : caches_(std::make_unique<Caches>()), alive_(std::make_shared<bool>(true)) {}

Document::~Document() { *alive_ = false; }

const Part* Document::FindPart(PartKind kind) const {
  for (const auto& p : parts_)
    if (p.kind == kind) return &p;
  return nullptr;
}

bool Document::Editable() const {
  return std::all_of(parts_.begin(), parts_.end(), [](const Part& p) { return p.Editable(); });
}

uint32_t Document::NoteStory(StoryKind kind, std::string_view id) const {
  const auto& map = kind == StoryKind::Footnote ? caches_->footnoteStory
                    : kind == StoryKind::Endnote ? caches_->endnoteStory
                                                 : caches_->commentStory;
  auto it = map.find(id);
  return it == map.end() ? kNone : it->second;
}

std::vector<Block> Document::Blocks(uint32_t story) const {
  if (story >= stories_.size()) return {};
  return Blocks(Node(&tree_, stories_[story].root));
}

std::vector<Block> Document::Blocks(Node container) const {
  std::vector<Block> out;
  if (!container) return out;
  if (container.Is(kSdt)) container = container.Child(kSdtContent);
  for (Node c : container.Kids()) {
    if (c.Is(kP)) out.push_back({BlockKind::Paragraph, c, c.Raw().start});
    else if (c.Is(kTbl)) out.push_back({BlockKind::Table, c, kNone});
    else if (c.Is(kSdt)) out.push_back({BlockKind::ContentControl, c, kNone});
    else if (c.Is(kCustomXml)) out.push_back({BlockKind::CustomXml, c, kNone});
  }
  return out;
}

std::vector<uint32_t> Document::StoryParagraphs(uint32_t story) const {
  std::vector<uint32_t> out;
  if (story >= stories_.size()) return out;
  ElemId top = stories_[story].root;
  for (ElemId id = top; id != kNoElem;) {
    bool para = tree_.elems[id].def != kNoDef && Schema::Get().Elem(tree_.elems[id].def).name == kP;
    if (para) out.push_back(tree_.elems[id].start);
    id = NextInTree(tree_, id, top, para);
  }
  return out;
}

std::string_view Document::W14ParaId(uint32_t para) const {
  return ParaNode(para).AttrOr(Name("w14:paraId"));
}

uint32_t Document::ParagraphOf(Node node) const {
  for (Node n = node; n; n = n.Parent())
    if (n.Is(kP)) return n.Raw().start;
  return kNone;
}

// ---------- Runs and items ----------

namespace {

ItemKind ItemKindOf(Node n) {
  static const std::unordered_map<uint32_t, ItemKind> kinds = {
      {Name("tab"), ItemKind::Tab}, {Name("ptab"), ItemKind::PositionalTab}, {Name("cr"), ItemKind::LineBreak},
      {Name("noBreakHyphen"), ItemKind::NoBreakHyphen}, {Name("softHyphen"), ItemKind::SoftHyphen},
      {Name("sym"), ItemKind::Symbol}, {Name("footnoteReference"), ItemKind::FootnoteRef},
      {Name("endnoteReference"), ItemKind::EndnoteRef}, {Name("commentReference"), ItemKind::CommentRef},
      {Name("footnoteRef"), ItemKind::NoteMark}, {Name("endnoteRef"), ItemKind::NoteMark},
      {Name("annotationRef"), ItemKind::AnnotationMark}, {Name("separator"), ItemKind::Separator},
      {Name("continuationSeparator"), ItemKind::ContinuationSeparator}, {Name("pgNum"), ItemKind::PageNumber},
      {Name("dayShort"), ItemKind::DateText}, {Name("monthShort"), ItemKind::DateText},
      {Name("yearShort"), ItemKind::DateText}, {Name("dayLong"), ItemKind::DateText},
      {Name("monthLong"), ItemKind::DateText}, {Name("yearLong"), ItemKind::DateText},
      {Name("lastRenderedPageBreak"), ItemKind::RenderedPageBreak},
      {Name("bookmarkStart"), ItemKind::BookmarkStart}, {Name("bookmarkEnd"), ItemKind::BookmarkEnd},
      {Name("commentRangeStart"), ItemKind::CommentStart}, {Name("commentRangeEnd"), ItemKind::CommentEnd},
      {Name("moveFromRangeStart"), ItemKind::MoveFromStart}, {Name("moveFromRangeEnd"), ItemKind::MoveFromEnd},
      {Name("moveToRangeStart"), ItemKind::MoveToStart}, {Name("moveToRangeEnd"), ItemKind::MoveToEnd},
      {Name("permStart"), ItemKind::PermStart}, {Name("permEnd"), ItemKind::PermEnd},
      {Name("proofErr"), ItemKind::ProofError},
  };
  uint32_t name = n.NameHash();
  if (name == Name("br")) {
    std::string_view type = n.AttrOr(Name("type"));
    return type == "page" ? ItemKind::PageBreak : type == "column" ? ItemKind::ColumnBreak : ItemKind::LineBreak;
  }
  if (name == Name("fldChar")) {
    std::string_view type = n.AttrOr(Name("fldCharType"));
    return type == "begin" ? ItemKind::FieldBegin : type == "separate" ? ItemKind::FieldSeparator : ItemKind::FieldEnd;
  }
  if (n.GetRole() == Role::Object) return ItemKind::Drawing;
  auto it = kinds.find(name);
  return it == kinds.end() ? ItemKind::Other : it->second;
}

bool IsRangeMarker(Node n) {
  switch (ItemKindOf(n)) {
    case ItemKind::BookmarkStart: case ItemKind::BookmarkEnd: case ItemKind::CommentStart: case ItemKind::CommentEnd:
    case ItemKind::MoveFromStart: case ItemKind::MoveFromEnd: case ItemKind::MoveToStart: case ItemKind::MoveToEnd:
    case ItemKind::PermStart: case ItemKind::PermEnd: case ItemKind::ProofError:
      return true;
    default:
      return false;
  }
}

}  // namespace

std::vector<TextRun> Document::Runs(uint32_t para) const {
  std::vector<TextRun> out;
  if (para >= paras_.size()) return out;
  // The field state where the paragraph starts: the story's field characters up to it.
  uint32_t story = paras_[para].story;
  if (story != kNone) {
    if (caches_->fieldPass.size() < stories_.size()) caches_->fieldPass.resize(stories_.size());
    Caches::FieldPass& pass = caches_->fieldPass[story];
    if (pass.paras.empty()) pass.paras = StoryParagraphs(story);
    while (!pass.Passed(para) && pass.next < pass.paras.size()) {
      uint32_t pi = pass.paras[pass.next++];
      if (!pass.open.empty()) caches_->fieldStack[pi] = pass.open;
      ElemId top = paras_[pi].elem;
      for (ElemId id = top; id != kNoElem; id = NextInTree(tree_, id, top)) {
        if (tree_.elems[id].flags & kDisplayOnly) continue;
        if (Schema::Get().Elem(tree_.elems[id].def).name != Name("fldChar")) continue;
        std::string_view type = Node(&tree_, id).AttrOr(Name("fldCharType"));
        if (type == "begin") pass.open.push_back(1);
        else if (type == "separate" && !pass.open.empty()) pass.open.back() = 0;
        else if (type == "end" && !pass.open.empty()) pass.open.pop_back();
      }
    }
  }
  std::vector<uint8_t> fields;
  if (auto it = caches_->fieldStack.find(para); it != caches_->fieldStack.end()) fields = it->second;
  auto inCode = [&] { return std::any_of(fields.begin(), fields.end(), [](uint8_t c) { return c != 0; }); };

  // Depth first with the enclosing context.
  struct Ctx {
    ElemId elem;
    Node hyperlink, revision, control, simple;
  };
  std::vector<Ctx> stack{{paras_[para].elem, {}, {}, {}, {}}};
  const Schema& s = Schema::Get();
  std::vector<std::pair<ElemId, Ctx>> work{{paras_[para].elem, stack[0]}};
  while (!work.empty()) {
    auto [id, ctx] = work.back();
    work.pop_back();
    Node n(&tree_, id);
    const ElemDef& def = s.Elem(n.Raw().def);
    Role role = s.Type(def.type).role;
    if (role == Role::Run) {
      // Split where the field state changes; field characters themselves are hidden.
      TextRun cur;
      bool open = false;
      for (Node piece : n.Kids()) {
        if (piece.Is(kRPr)) continue;
        bool fldChar = piece.Is(Name("fldChar"));
        bool displayOnly = n.DisplayOnly() || piece.DisplayOnly();
        if (fldChar && !displayOnly) {
          std::string_view type = piece.AttrOr(Name("fldCharType"));
          if (type == "begin") fields.push_back(1);
          else if (type == "separate" && !fields.empty()) fields.back() = 0;
        }
        bool hidden = fldChar || inCode();
        uint8_t depth = static_cast<uint8_t>(std::min<size_t>(fields.size(), 255));
        if (!open || cur.fieldCode != hidden || cur.fieldDepth != depth || cur.displayOnly != displayOnly) {
          if (open && cur.end > cur.start) out.push_back(cur);
          cur = TextRun{};
          cur.start = piece.TextStart();
          cur.run = n;
          cur.hyperlink = ctx.hyperlink;
          cur.revision = ctx.revision;
          cur.contentControl = ctx.control;
          cur.simpleField = ctx.simple;
          cur.fieldCode = hidden;
          cur.fieldDepth = depth;
          cur.displayOnly = displayOnly;
          open = true;
        }
        cur.end = piece.TextStart() + piece.TextLength();
        if (fldChar && !displayOnly && piece.AttrOr(Name("fldCharType")) == "end" && !fields.empty()) fields.pop_back();
      }
      if (open && cur.end > cur.start) out.push_back(cur);
      continue;
    }
    Ctx inner = ctx;
    uint32_t name = def.name;
    if (name == Name("hyperlink")) inner.hyperlink = n;
    else if (name == Name("ins") || name == Name("del") || name == Name("moveFrom") || name == Name("moveTo")) inner.revision = n;
    else if (name == kSdt) inner.control = n;
    else if (name == Name("fldSimple")) inner.simple = n;
    std::vector<ElemId> kids;
    for (Node c : n.Kids())
      if (!c.Is(kPPr)) kids.push_back(c.Id());
    for (size_t i = kids.size(); i-- > 0;) work.emplace_back(kids[i], inner);
  }
  return out;
}

std::vector<InlineItem> Document::Items(uint32_t para) const {
  std::vector<InlineItem> out;
  if (para >= paras_.size()) return out;
  ElemId top = paras_[para].elem;
  std::unordered_map<ElemId, uint32_t> drawingOf;
  for (ElemId id = top; id != kNoElem;) {
    Node n(&tree_, id);
    bool skip = id != top && n.Is(kPPr);
    if (!skip && id != top) {
      Role role = n.GetRole();
      if (role == Role::Char || role == Role::Object || role == Role::Marker || IsRangeMarker(n)) {
        InlineItem item;
        item.kind = ItemKindOf(n);
        item.offset = n.TextStart();
        item.length = role == Role::Char || role == Role::Object ? 1 : 0;
        item.node = n;
        if (role == Role::Object) {
          if (drawingOf.empty())
            for (uint32_t d = 0; d < drawings_.size(); ++d)
              if (drawings_[d].pos.para == para) drawingOf[drawings_[d].node.Id()] = d;
          auto it = drawingOf.find(id);
          item.drawing = it == drawingOf.end() ? kNone : it->second;
        }
        out.push_back(item);
      }
    }
    id = NextInTree(tree_, id, top, skip);
  }
  return out;
}

Pos Document::PosOf(Node node) const {
  uint32_t para = ParagraphOf(node);
  if (para != kNone) return Pos{para, node.Is(kP) ? 0 : node.TextStart()};
  // Between blocks: the start of the next paragraph, else the end of the story's last one.
  ElemId top = node.Id();
  while (top != kNoElem && tree_.elems[top].parent != kNoElem) top = tree_.elems[top].parent;
  for (ElemId id = NextInTree(tree_, node.Id(), top, true); id != kNoElem; id = NextInTree(tree_, id, top)) {
    if (Schema::Get().Elem(tree_.elems[id].def).name == kP) return Pos{tree_.elems[id].start, 0};
  }
  for (Node n = node.Parent(); n; n = n.Parent()) {
    if (n.GetRole() != Role::Story) continue;
    for (uint32_t s = 0; s < stories_.size(); ++s) {
      if (stories_[s].root != n.Id()) continue;
      auto paras = StoryParagraphs(s);
      if (!paras.empty()) return Pos{paras.back(), static_cast<uint32_t>(paras_[paras.back()].text.size())};
    }
  }
  return Pos{};
}

// ---------- Tables ----------

std::vector<Twips> Document::TableGrid(Node table) const {
  std::vector<Twips> cols;
  for (Node c : table.Child(kTblGrid).Kids())
    if (c.Is(kGridCol)) cols.push_back(Twips(c.Twips(kW).value_or(0)));
  return cols;
}

namespace {

// Children of `container` named `name`, looking inside content controls and custom XML.
std::vector<Node> Gather(Node container, uint32_t name) {
  std::vector<Node> out;
  std::vector<Node> work;
  for (Node c : container.Kids()) work.push_back(c);
  std::reverse(work.begin(), work.end());
  while (!work.empty()) {
    Node n = work.back();
    work.pop_back();
    if (n.Is(name)) {
      out.push_back(n);
    } else if (n.Is(kSdt) || n.Is(kCustomXml)) {
      Node content = n.Is(kSdt) ? n.Child(kSdtContent) : n;
      std::vector<Node> kids;
      for (Node c : content.Kids()) kids.push_back(c);
      for (size_t i = kids.size(); i-- > 0;) work.push_back(kids[i]);
    }
  }
  return out;
}

Node Enclosing(Node n, uint32_t name) {
  for (Node p = n.Parent(); p; p = p.Parent())
    if (p.Is(name)) return p;
  return {};
}

int32_t SpanOf(Node tc) { return static_cast<int32_t>(std::max<int64_t>(1, tc.Path({kTcPr, Name("gridSpan")}).Int(kVal).value_or(1))); }

}  // namespace

std::vector<Node> Document::TableRows(Node table) const { return Gather(table, kTr); }
std::vector<Node> Document::RowCells(Node row) const { return Gather(row, kTc); }

int32_t Document::CellGridColumn(Node cell) const {
  Node row = Enclosing(cell, kTr);
  int32_t col = static_cast<int32_t>(row.Path({kTrPr, Name("gridBefore")}).Int(kVal).value_or(0));
  for (Node c : RowCells(row)) {
    if (c == cell) return col;
    col += SpanOf(c);
  }
  return col;
}

int32_t Document::CellRowSpan(Node cell) const {
  Node vm = cell.Path({kTcPr, Name("vMerge")});
  if (!vm || vm.AttrOr(kVal) != "restart") return 1;
  Node table = Enclosing(cell, kTbl);
  int32_t col = CellGridColumn(cell);
  auto rows = TableRows(table);
  Node row = Enclosing(cell, kTr);
  int32_t span = 1;
  bool after = false;
  for (Node r : rows) {
    if (r == row) {
      after = true;
      continue;
    }
    if (!after) continue;
    bool continues = false;
    for (Node c : RowCells(r)) {
      if (CellGridColumn(c) != col) continue;
      Node m = c.Path({kTcPr, Name("vMerge")});
      continues = m && m.AttrOr(kVal, "continue") == "continue";
      break;
    }
    if (!continues) break;
    ++span;
  }
  return span;
}

// ---------- Styles, theme ----------

const Style* Document::FindStyle(std::string_view styleId) const {
  auto it = caches_->styleIndex.find(styleId);
  return it == caches_->styleIndex.end() ? nullptr : &styles_[it->second];
}

const Style* Document::DefaultStyle(StyleType type) const {
  for (const auto& s : styles_)
    if (s.type == type && s.isDefault) return &s;
  return nullptr;
}

std::string_view Document::ThemeFont(std::string_view ref) const {
  // minorHAnsi, majorEastAsia, minorBidi, ...: a:latin for Ascii and HAnsi, a:ea for EastAsia, a:cs
  // for Bidi, or the a:font for the script themeFontLang names.
  if (!theme_.present) return {};
  const Theme::Fonts& f = ref.starts_with("major") ? theme_.major : theme_.minor;
  auto byScript = [&](std::string_view lang) -> std::string_view {
    static constexpr std::pair<std::string_view, std::string_view> kScripts[] = {
        {"ja", "Jpan"}, {"ko", "Hang"}, {"zh-CN", "Hans"}, {"zh-SG", "Hans"}, {"zh-TW", "Hant"}, {"zh-HK", "Hant"},
        {"zh-MO", "Hant"}, {"zh", "Hans"}, {"ar", "Arab"}, {"he", "Hebr"}, {"th", "Thai"}, {"fa", "Arab"},
        {"ur", "Arab"}, {"hi", "Deva"}, {"bn", "Beng"}, {"ta", "Taml"}, {"el", "Grek"}, {"ru", "Cyrl"},
    };
    for (auto [prefix, script] : kScripts) {
      if (!lang.starts_with(prefix)) continue;
      for (const auto& [sc, face] : f.scripts)
        if (sc == script) return face;
      break;
    }
    return {};
  };
  if (ref.ends_with("EastAsia")) {
    std::string_view face = byScript(settings_.themeFontLangEastAsia);
    return face.empty() ? std::string_view(f.eastAsia) : face;
  }
  if (ref.ends_with("Bidi")) {
    std::string_view face = byScript(settings_.themeFontLangBidi);
    return face.empty() ? std::string_view(f.cs) : face;
  }
  return f.latin;
}

namespace {

void RgbToHsl(uint32_t rgb, double& h, double& s, double& l) {
  double r = ((rgb >> 16) & 0xFF) / 255.0, g = ((rgb >> 8) & 0xFF) / 255.0, b = (rgb & 0xFF) / 255.0;
  double mx = std::max({r, g, b}), mn = std::min({r, g, b});
  l = (mx + mn) / 2;
  if (mx == mn) {
    h = s = 0;
    return;
  }
  double d = mx - mn;
  s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
  if (mx == r) h = (g - b) / d + (g < b ? 6 : 0);
  else if (mx == g) h = (b - r) / d + 2;
  else h = (r - g) / d + 4;
  h /= 6;
}

uint32_t HslToRgb(double h, double s, double l) {
  auto hue = [](double p, double q, double t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6) return p + (q - p) * 6 * t;
    if (t < 1.0 / 2) return q;
    if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
  };
  double r, g, b;
  if (s == 0) {
    r = g = b = l;
  } else {
    double q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
    r = hue(p, q, h + 1.0 / 3);
    g = hue(p, q, h);
    b = hue(p, q, h - 1.0 / 3);
  }
  // Truncated, as Word does (see ThemeColor).
  auto byte = [](double v) { return static_cast<uint32_t>(std::clamp(static_cast<int>(v * 255 + 1e-9), 0, 255)); };
  return (byte(r) << 16) | (byte(g) << 8) | byte(b);
}

}  // namespace

std::optional<uint32_t> Document::ThemeColor(std::string_view name, int tint, int shade) const {
  if (!theme_.present) return std::nullopt;
  // w:clrSchemeMapping maps text1/background1/... to theme slots ([MS-OI29500] 2.1.395: t1 is
  // dark1 and t2 dark2 when absent; bg1 and bg2 likewise light1 and light2).
  static constexpr std::pair<std::string_view, std::string_view> kMapped[] = {
      {"text1", "t1"}, {"dark1", "t1"}, {"background1", "bg1"}, {"light1", "bg1"},
      {"text2", "t2"}, {"dark2", "t2"}, {"background2", "bg2"}, {"light2", "bg2"},
  };
  std::string_view slot = name;
  Node mapping;
  if (const Part* settings = FindPart(PartKind::Settings)) mapping = RootOf(*settings).Child(Name("clrSchemeMapping"));
  for (auto [from, attr] : kMapped) {
    if (name != from) continue;
    std::string_view fallback = attr == "t1" ? "dark1" : attr == "t2" ? "dark2" : attr == "bg1" ? "light1" : "light2";
    slot = mapping ? mapping.AttrOr(Name(attr), fallback) : fallback;
  }
  static constexpr std::string_view kSlots[] = {"dark1", "light1", "dark2", "light2", "accent1", "accent2",
                                                "accent3", "accent4", "accent5", "accent6", "hyperlink",
                                                "followedHyperlink"};
  size_t i = 0;
  while (i < 12 && kSlots[i] != slot) ++i;
  if (i == 12) return std::nullopt;
  uint32_t rgb = theme_.colors[i];
  // [MS-OI29500] 2.1.72: Word applies tint and shade to the luminance in HSL, and the tint when
  // both are given. With the luminance and the channels truncated to bytes, this gives the spec's
  // examples and 11 of the 15 tints and shades in Word's Office theme palette exactly; the other
  // four are one off in one channel.
  if (tint >= 0 || shade >= 0) {
    double h, s, l;
    RgbToHsl(rgb, h, s, l);
    l = std::floor(l * 255 + 1e-9) / 255;
    if (tint >= 0) l = l * (tint / 255.0) + (1 - tint / 255.0);
    else l = l * (shade / 255.0);
    rgb = HslToRgb(h, s, l);
  }
  return rgb;
}

Node Document::NumberingLevel(std::string_view numId, int32_t ilvl) const {
  auto num = caches_->nums.find(numId);
  if (num == caches_->nums.end()) return {};
  Node n(&tree_, num->second);
  for (Node o : n.Kids()) {
    if (!o.Is(Name("lvlOverride")) || o.Int(Name("ilvl")).value_or(-1) != ilvl) continue;
    if (Node lvl = o.Child(Name("lvl"))) return lvl;
  }
  std::string_view abstractId = n.Child(Name("abstractNumId")).AttrOr(kVal);
  for (int hops = 0; hops < 4; ++hops) {
    auto it = caches_->abstractNums.find(abstractId);
    if (it == caches_->abstractNums.end()) return {};
    Node abs(&tree_, it->second);
    // w:numStyleLink: the definition lives in a numbering style's w:num.
    if (Node link = abs.Child(Name("numStyleLink")); link && !abs.Child(Name("lvl"))) {
      const Style* style = FindStyle(link.AttrOr(kVal));
      std::string_view linked = style ? style->node.Path({kPPr, Name("numPr"), Name("numId")}).AttrOr(kVal) : "";
      auto ln = caches_->nums.find(linked);
      if (ln == caches_->nums.end()) return {};
      abstractId = Node(&tree_, ln->second).Child(Name("abstractNumId")).AttrOr(kVal);
      continue;
    }
    for (Node lvl : abs.Kids())
      if (lvl.Is(Name("lvl")) && lvl.Int(Name("ilvl")).value_or(-1) == ilvl) return lvl;
    return {};
  }
  return {};
}

// ---------- Fields, comments, revisions ----------

std::u16string_view Field::Type() const {
  std::u16string_view c = code;
  while (!c.empty() && (c.front() == u' ' || c.front() == u'\t')) c.remove_prefix(1);
  size_t n = 0;
  while (n < c.size() && c[n] != u' ' && c[n] != u'\t' && c[n] != u'\\') ++n;
  return c.substr(0, n);
}

std::vector<Field> Document::Fields(uint32_t story) const {
  std::vector<Field> out;
  struct Open {
    uint32_t index;
    bool code;
    ElemId simple;  // the w:fldSimple, or kNoElem
  };
  std::vector<Open> open;
  const Schema& s = Schema::Get();
  for (uint32_t pi : StoryParagraphs(story)) {
    const std::u16string& text = paras_[pi].text;
    // Shown text belongs to the result of each open field from the innermost out, up to one whose
    // code it is part of (a field nested in another's code shows its result to that code only).
    auto addResult = [&](std::u16string_view piece) {
      for (size_t i = open.size(); i-- > 0 && !open[i].code;) out[open[i].index].result += piece;
    };
    addResult(u"\r");
    ElemId top = paras_[pi].elem;
    for (ElemId id = top; id != kNoElem;) {
      Node n(&tree_, id);
      if (n.DisplayOnly()) {
        id = NextInTree(tree_, id, top, true);
        continue;
      }
      // Close fldSimple fields whose subtree is done.
      while (!open.empty() && open.back().simple != kNoElem) {
        bool inside = false;
        for (Node a = n; a; a = a.Parent())
          if (a.Id() == open.back().simple) inside = true;
        if (inside) break;
        Field& f = out[open.back().index];
        Node simple(&tree_, open.back().simple);
        f.end = f.separator = Pos{pi, simple.TextStart() + simple.TextLength()};
        open.pop_back();
      }
      uint32_t name = s.Elem(n.Raw().def).name;
      Role role = n.GetRole();
      if (name == Name("fldChar")) {
        std::string_view type = n.AttrOr(Name("fldCharType"));
        Pos at{pi, n.TextStart()};
        if (type == "begin") {
          Field f;
          f.story = story;
          f.begin = at;
          f.beginNode = n;
          f.locked = n.OnOff(Name("fldLock")).value_or(false);
          f.dirty = n.OnOff(Name("dirty")).value_or(false);
          f.depth = static_cast<uint32_t>(open.size());
          f.parent = open.empty() ? kNone : open.back().index;
          f.complete = false;
          open.push_back({static_cast<uint32_t>(out.size()), true, kNoElem});
          out.push_back(std::move(f));
        } else if (type == "separate" && !open.empty() && open.back().simple == kNoElem) {
          out[open.back().index].separator = at;
          out[open.back().index].separatorNode = n;
          open.back().code = false;
        } else if (type == "end" && !open.empty() && open.back().simple == kNoElem) {
          Field& f = out[open.back().index];
          f.end = at;
          f.endNode = n;
          f.complete = true;
          open.pop_back();
        }
      } else if (name == Name("fldSimple")) {
        Field f;
        f.story = story;
        f.simple = true;
        f.begin = Pos{pi, n.TextStart()};
        f.beginNode = f.separatorNode = f.endNode = n;
        f.code = ToUtf16(n.AttrOr(Name("instr")));
        f.locked = n.OnOff(Name("fldLock")).value_or(false);
        f.dirty = n.OnOff(Name("dirty")).value_or(false);
        f.depth = static_cast<uint32_t>(open.size());
        f.parent = open.empty() ? kNone : open.back().index;
        open.push_back({static_cast<uint32_t>(out.size()), false, id});
        out.push_back(std::move(f));
      } else if (role == Role::Text || role == Role::Char || role == Role::Object) {
        std::u16string_view piece = std::u16string_view(text).substr(std::min<size_t>(n.TextStart(), text.size()), n.TextLength());
        if (!open.empty() && open.back().code) {
          if (name == Name("instrText") || name == Name("delInstrText")) out[open.back().index].code += piece;
        } else {
          addResult(piece);
        }
      }
      id = NextInTree(tree_, id, top, role == Role::Text || n.Is(kPPr));
    }
    while (!open.empty() && open.back().simple != kNoElem) {
      Field& f = out[open.back().index];
      Node simple(&tree_, open.back().simple);
      f.end = f.separator = Pos{pi, simple.TextStart() + simple.TextLength()};
      open.pop_back();
    }
  }
  return out;
}

std::vector<CommentInfo> Document::Comments() const {
  std::vector<CommentInfo> out;
  std::unordered_map<std::string_view, size_t> byId;
  // The comments in the comments part: one deleted (or added and undone) keeps its story, out of it.
  std::unordered_set<ElemId> linked;
  if (const Part* part = FindPart(PartKind::Comments))
    for (ElemId c = tree_.elems[part->root].first; c != kNoElem; c = tree_.elems[c].next) linked.insert(c);
  for (uint32_t s = 0; s < stories_.size(); ++s) {
    if (stories_[s].kind != StoryKind::Comment || !linked.count(stories_[s].root)) continue;
    Node root(&tree_, stories_[s].root);
    CommentInfo c;
    c.story = s;
    c.id = root.AttrOr(kId);
    c.author = root.AttrOr(Name("author"));
    c.initials = root.AttrOr(Name("initials"));
    c.date = root.AttrOr(Name("date"));
    auto paras = StoryParagraphs(s);
    if (!paras.empty()) c.paraId = W14ParaId(paras.back());
    byId[c.id] = out.size();
    out.push_back(c);
  }
  for (const auto& pair : pairs_) {
    if (pair.kind != pkg::RefKind::CommentId) continue;
    auto it = byId.find(pair.id);
    if (it == byId.end()) continue;
    out[it->second].start = pair.openPos;
    out[it->second].end = pair.closePos;
  }
  for (ElemId e = 0; e < tree_.elems.size(); ++e) {
    if (Schema::Get().Elem(tree_.elems[e].def).name != Name("commentReference")) continue;
    Node n(&tree_, e);
    if (n.DisplayOnly()) continue;
    auto it = byId.find(n.AttrOr(kId));
    if (it != byId.end()) out[it->second].reference = PosOf(n);
  }
  // Replies and resolved state, from commentsExtended (keyed by each comment's last paraId).
  std::unordered_map<std::string_view, std::string_view> idByPara;
  for (const auto& c : out)
    if (!c.paraId.empty()) idByPara[c.paraId] = c.id;
  for (auto& c : out) {
    for (const auto& ex : commentsEx_) {
      if (c.paraId.empty() || ex.paraId != c.paraId) continue;
      c.done = ex.done;
      if (auto it = idByPara.find(ex.parentParaId); it != idByPara.end()) c.parentId = std::string(it->second);
    }
  }
  return out;
}

std::vector<Revision> Document::Revisions(uint32_t story) const {
  std::vector<Revision> out;
  if (story >= stories_.size()) return out;
  const Schema& s = Schema::Get();
  ElemId top = stories_[story].root;
  for (ElemId id = top; id != kNoElem; id = NextInTree(tree_, id, top)) {
    Node n(&tree_, id);
    if (n.DisplayOnly()) continue;
    std::string_view local = s.Elem(n.Raw().def).local;
    std::string_view parent = n.Parent() ? n.Parent().LocalName() : std::string_view();
    std::string_view typeName = s.Type(n.Type()).name;
    std::optional<RevisionKind> kind;
    if (typeName == "InlineChange") {
      kind = local == "ins" ? RevisionKind::Insertion : local == "del" ? RevisionKind::Deletion
             : local == "moveFrom" ? RevisionKind::MoveFrom : RevisionKind::MoveTo;
    } else if (typeName == "Change" && parent == "rPr" && n.Parent().Parent().Is(kPPr)) {
      if (local == "ins" || local == "moveTo") kind = RevisionKind::MarkInsertion;
      else if (local == "del" || local == "moveFrom") kind = RevisionKind::MarkDeletion;
    } else if (typeName == "Change" && parent == "trPr") {
      kind = local == "ins" ? RevisionKind::RowInsertion : RevisionKind::RowDeletion;
    } else if (local == "cellIns") {
      kind = RevisionKind::CellInsertion;
    } else if (local == "cellDel") {
      kind = RevisionKind::CellDeletion;
    } else if (local == "cellMerge") {
      kind = RevisionKind::CellMerge;
    } else if (local == "rPrChange") {
      kind = RevisionKind::RunFormat;
    } else if (local == "pPrChange") {
      kind = RevisionKind::ParagraphFormat;
    } else if (local == "sectPrChange") {
      kind = RevisionKind::SectionFormat;
    } else if (local == "tblPrChange" || local == "tblPrExChange") {
      kind = RevisionKind::TableFormat;
    } else if (local == "trPrChange") {
      kind = RevisionKind::RowFormat;
    } else if (local == "tcPrChange") {
      kind = RevisionKind::CellFormat;
    } else if (local == "tblGridChange") {
      kind = RevisionKind::TableGrid;
    }
    if (!kind) continue;
    Revision r;
    r.kind = *kind;
    r.node = n;
    r.id = n.AttrOr(kId);
    r.author = n.AttrOr(Name("author"));
    r.date = n.AttrOr(Name("date"));
    r.story = story;
    if (r.kind == RevisionKind::Insertion || r.kind == RevisionKind::Deletion || r.kind == RevisionKind::MoveFrom ||
        r.kind == RevisionKind::MoveTo) {
      uint32_t para = ParagraphOf(n);
      r.start = Pos{para, n.TextStart()};
      r.end = Pos{para, n.TextStart() + n.TextLength()};
    } else if (r.kind == RevisionKind::RunFormat) {
      Node run = n.Parent().Parent();
      uint32_t para = ParagraphOf(run);
      r.start = Pos{para, run.TextStart()};
      r.end = Pos{para, run.TextStart() + run.TextLength()};
    } else {
      r.start = r.end = PosOf(n);
    }
    out.push_back(r);
  }
  return out;
}

// ---------- Drawings, relationships, links ----------

std::string Document::DrawingXml(const Drawing& d) const {
  if (!d.keptXml.empty()) return d.keptXml;
  std::string xml;
  if (!d.node) return xml;
  for (const pkg::Fragment* f : store_.FragmentsOf(d.node.Raw().node, pkg::Slot::Inside)) xml += f->xml;
  return xml;
}

const pkg::RelationshipSet* Document::RelationshipsOf(uint32_t part) const {
  return part < parts_.size() ? package_->RelationshipsOf(parts_[part].name) : nullptr;
}

pkg::Status Document::ReadTarget(uint32_t part, std::string_view relId, std::vector<uint8_t>& out) const {
  const pkg::RelationshipSet* rels = RelationshipsOf(part);
  const pkg::Relationship* r = rels ? rels->Find(relId) : nullptr;
  if (!r) return pkg::Status::Fail(pkg::Status::Code::NotFound, "A picture or object in this document is missing.");
  // External content is never fetched (plan §4.7).
  if (r->external) return pkg::Status::Fail(pkg::Status::Code::Unsupported, "This picture is linked to a file outside the document.");
  return package_->ReadPart(r->targetPart, out);
}

std::string Document::HyperlinkTarget(Node hyperlink) const {
  if (auto anchor = hyperlink.Attr(Name("anchor")); anchor && !hyperlink.Has(Name("r:id"))) return "#" + std::string(*anchor);
  auto rid = hyperlink.Attr(Name("r:id"));
  if (!rid) return {};
  // The part: the one whose tree holds the link, or for a text box, its drawing's part.
  ElemId top = hyperlink.Id();
  while (tree_.elems[top].parent != kNoElem) top = tree_.elems[top].parent;
  uint32_t part = kNone;
  for (uint32_t i = 0; i < parts_.size(); ++i)
    if (parts_[i].root == top) part = i;
  if (part == kNone)
    for (const auto& s : stories_)
      if (s.root == top) part = s.part;
  const pkg::RelationshipSet* rels = RelationshipsOf(part);
  const pkg::Relationship* r = rels ? rels->Find(*rid) : nullptr;
  if (!r) return {};
  std::string target = r->target;
  if (auto anchor = hyperlink.Attr(Name("anchor"))) target += "#" + std::string(*anchor);
  return target;
}

// ---------- The reference registry ----------

size_t Document::RenameInModel(pkg::RefKind kind, std::string_view from, std::string_view to) {
  size_t changed = 0;
  const Schema& s = Schema::Get();
  for (const auto& rule : registry_->Rules()) {
    if (rule.kind != kind) continue;
    if (rule.format != pkg::DocFormat::Word && rule.format != pkg::DocFormat::Any) continue;
    auto same = [&](std::string_view a, std::string_view b) {
      if (!rule.ignoreCase) return a == b;
      return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return (x >= 'A' && x <= 'Z' ? x + 32 : x) == (y >= 'A' && y <= 'Z' ? y + 32 : y);
             });
    };
    for (const auto& site : rule.sites) {
      if (site.attrLocal.empty() || site.syntax == pkg::RefSyntax::None) continue;  // element text: D2's editor
      uint32_t attrName = NameOf(NsFromUri(site.attrNs), site.attrLocal);
      Ns elemNs = NsFromUri(site.elementNs);
      for (Elem& e : tree_.elems) {
        const ElemDef& def = s.Elem(e.def);
        if (!site.elementLocal.empty() && (def.local != site.elementLocal || def.ns != elemNs)) continue;
        if (site.elementLocal.empty() && !site.elementNs.empty() && def.ns != elemNs) continue;
        if (!site.parentLocal.empty() &&
            (e.parent == kNoElem || s.Elem(tree_.elems[e.parent].def).local != site.parentLocal))
          continue;
        for (uint32_t i = 0; i < e.attrCount; ++i) {
          AttrVal& a = tree_.attrs[e.attrs + i];
          if (s.Attr(a.def).name != attrName) continue;
          std::string_view value = tree_.strings.Get(a.value);
          if (!site.skipValue.empty() && value == site.skipValue) continue;
          // Replace each value the site holds that names `from`.
          std::string renamed;
          size_t copied = 0;
          pkg::ForEachRefValue(site, kind, value, [&](std::string_view v) {
            if (!same(v, from)) return;
            size_t at = static_cast<size_t>(v.data() - value.data());
            renamed.append(value.substr(copied, at - copied));
            renamed.append(to);
            copied = at + v.size();
          });
          if (copied == 0 && renamed.empty()) continue;
          renamed.append(value.substr(copied));
          a.value = tree_.strings.Intern(renamed);
          ++changed;
        }
      }
    }
  }
  if (changed) {
    ResetCaches();
    BuildIndexes();
  }
  return changed;
}

size_t Document::RenameReference(pkg::RefKind kind, std::string_view from, std::string_view to) {
  std::string f(from), t(to);  // the views may point into the model, which the rename changes
  size_t changed = registry_->Rename(kind, f, t, pugi::xml_node(), &store_);
  if (!attached_) changed += RenameInModel(kind, f, t);
  return changed;
}

void Document::AttachTo(pkg::ReferenceRegistry& registry) {
  std::weak_ptr<bool> alive = alive_;
  Document* self = this;
  registry.AddHook([alive, self](pkg::RefKind kind, std::string_view from, std::string_view to) {
    auto a = alive.lock();
    if (a && *a) self->RenameInModel(kind, std::string(from), std::string(to));
  });
  if (&registry == registry_) attached_ = true;
}

size_t Document::ModelBytes() const {
  size_t n = tree_.BytesUsed();
  for (const auto& p : paras_) n += sizeof(Paragraph) + p.text.capacity() * sizeof(char16_t);
  n += stories_.capacity() * sizeof(Story) + drawings_.capacity() * sizeof(Drawing) + pairs_.capacity() * sizeof(PairInfo);
  n += styles_.capacity() * sizeof(Style);
  return n;
}

// ---------- Indexes built after reading ----------

void Document::BuildIndexes() {
  Caches& c = *caches_;
  const Schema& sc = Schema::Get();

  // Notes and comments by id (a note deleted with its reference has no root, D4).
  for (uint32_t s = 0; s < stories_.size(); ++s) {
    const Story& st = stories_[s];
    if (st.root == kNoElem) continue;
    if (st.kind == StoryKind::Footnote) c.footnoteStory.emplace(st.id, s);
    else if (st.kind == StoryKind::Endnote) c.endnoteStory.emplace(st.id, s);
    else if (st.kind == StoryKind::Comment) c.commentStory.emplace(st.id, s);
  }

  // Styles.
  styles_.clear();
  if (const Part* part = FindPart(PartKind::Styles)) {
    for (Node n : RootOf(*part).Kids()) {
      if (!n.Is(Name("style"))) continue;
      Style st;
      std::string_view kind = n.AttrOr(kType);
      st.type = kind == "character" ? StyleType::Character : kind == "table" ? StyleType::Table
                : kind == "numbering" ? StyleType::Numbering : StyleType::Paragraph;
      st.id = n.AttrOr(Name("styleId"));
      st.name = n.Child(Name("name")).AttrOr(kVal);
      st.node = n;
      st.isDefault = n.OnOff(Name("default")).value_or(false);
      styles_.push_back(st);
    }
  }
  for (uint32_t i = 0; i < styles_.size(); ++i) c.styleIndex.emplace(styles_[i].id, i);  // the first wins
  for (auto& st : styles_) {
    auto link = [&](uint32_t name) {
      auto it = c.styleIndex.find(st.node.Child(name).AttrOr(kVal));
      return st.node.Child(name) && it != c.styleIndex.end() ? it->second : kNone;
    };
    st.basedOn = link(Name("basedOn"));
    st.next = link(Name("next"));
    st.link = link(Name("link"));
  }

  // Numbering.
  if (const Part* part = FindPart(PartKind::Numbering)) {
    for (Node n : RootOf(*part).Kids()) {
      if (n.Is(Name("num"))) c.nums.emplace(n.AttrOr(Name("numId")), n.Id());
      else if (n.Is(Name("abstractNum"))) c.abstractNums.emplace(n.AttrOr(Name("abstractNumId")), n.Id());
    }
  }

  // Settings.
  settings_ = DocSettings{};
  if (const Part* part = FindPart(PartKind::Settings)) {
    Node root = RootOf(*part);
    DocSettings& s = settings_;
    auto on = [&](uint32_t name) { return root.Child(name) && root.Child(name).On(); };
    s.autoHyphenation = on(Name("autoHyphenation"));
    s.doNotHyphenateCaps = on(Name("doNotHyphenateCaps"));
    s.evenAndOddHeaders = on(Name("evenAndOddHeaders"));
    s.mirrorMargins = on(Name("mirrorMargins"));
    s.gutterAtTop = on(Name("gutterAtTop"));
    s.bordersDoNotSurroundHeader = on(Name("bordersDoNotSurroundHeader"));
    s.bordersDoNotSurroundFooter = on(Name("bordersDoNotSurroundFooter"));
    s.displayBackgroundShape = on(Name("displayBackgroundShape"));
    s.consecutiveHyphenLimit = static_cast<int32_t>(root.Child(Name("consecutiveHyphenLimit")).Int(kVal).value_or(0));
    s.hyphenationZone = Twips(root.Child(Name("hyphenationZone")).Twips(kVal).value_or(360));
    s.defaultTabStop = Twips(root.Child(Name("defaultTabStop")).Twips(kVal).value_or(720));
    s.characterSpacingControl = root.Child(Name("characterSpacingControl")).AttrOr(kVal);
    Node lang = root.Child(Name("themeFontLang"));
    s.themeFontLang = lang.AttrOr(kVal);
    s.themeFontLangEastAsia = lang.AttrOr(Name("eastAsia"));
    s.themeFontLangBidi = lang.AttrOr(Name("bidi"));
    s.decimalSymbol = root.Child(Name("decimalSymbol")).AttrOr(kVal);
    s.listSeparator = root.Child(Name("listSeparator")).AttrOr(kVal);
    s.compat = root.Child(Name("compat"));
    for (Node cs : s.compat.Kids())
      if (cs.Is(Name("compatSetting")) && cs.AttrOr(Name("name")) == "compatibilityMode")
        s.compatibilityMode = static_cast<int32_t>(cs.Int(kVal).value_or(0));
    for (auto [props, name, list, note] :
         {std::tuple{&s.footnotes, Name("footnotePr"), &s.footnoteSeparators, Name("footnote")},
          std::tuple{&s.endnotes, Name("endnotePr"), &s.endnoteSeparators, Name("endnote")}}) {
      Node pr = root.Child(name);
      props->pos = pr.Child(Name("pos")).AttrOr(kVal);
      props->numFmt = pr.Child(Name("numFmt")).AttrOr(kVal);
      props->start = static_cast<int32_t>(pr.Child(Name("numStart")).Int(kVal).value_or(1));
      props->restart = pr.Child(Name("numRestart")).AttrOr(kVal);
      for (Node n : pr.Kids())
        if (n.Is(note)) list->push_back(n.AttrOr(kId));
    }
  }

  // Fonts.
  fonts_.clear();
  if (const Part* part = FindPart(PartKind::FontTable)) {
    for (Node n : RootOf(*part).Kids()) {
      if (!n.Is(Name("font"))) continue;
      FontInfo f;
      f.name = n.AttrOr(Name("name"));
      f.altName = n.Child(Name("altName")).AttrOr(kVal);
      f.panose = n.Child(Name("panose1")).AttrOr(kVal);
      f.charset = n.Child(Name("charset")).AttrOr(kVal);
      f.family = n.Child(Name("family")).AttrOr(kVal);
      f.pitch = n.Child(Name("pitch")).AttrOr(kVal);
      f.notTrueType = n.Child(Name("notTrueType")) && n.Child(Name("notTrueType")).On();
      Node sig = n.Child(Name("sig"));
      const uint32_t kSig[] = {Name("usb0"), Name("usb1"), Name("usb2"), Name("usb3"), Name("csb0"), Name("csb1")};
      for (size_t i = 0; i < 6; ++i) f.sig[i] = sig.Hex(kSig[i]).value_or(0);
      for (auto [embed, name] : {std::pair{&f.regular, Name("embedRegular")}, std::pair{&f.bold, Name("embedBold")},
                                 std::pair{&f.italic, Name("embedItalic")},
                                 std::pair{&f.boldItalic, Name("embedBoldItalic")}}) {
        Node e = n.Child(name);
        embed->relId = e.AttrOr(Name("r:id"));
        embed->fontKey = e.AttrOr(Name("fontKey"));
        embed->subsetted = e.OnOff(Name("subsetted")).value_or(false);
      }
      fonts_.push_back(f);
    }
  }

  // Pairs, by the registry's rules: value pairs match by id within a part, fields by nesting.
  pairs_.clear();
  struct SiteOf {
    const pkg::RefRule* rule;
    const pkg::RefSite* site;
  };
  std::unordered_map<uint32_t, std::vector<SiteOf>> sites;
  for (const auto& rule : registry_->Rules()) {
    if (rule.format != pkg::DocFormat::Word && rule.format != pkg::DocFormat::Any) continue;
    for (const auto& site : rule.sites)
      if ((site.role & (pkg::kOpens | pkg::kCloses)) && !site.elementLocal.empty())
        sites[NameOf(NsFromUri(site.elementNs), site.elementLocal)].push_back({&rule, &site});
  }
  std::vector<ElemId> roots;
  for (const auto& part : parts_) roots.push_back(part.root);
  for (const auto& st : stories_)
    if (st.kind == StoryKind::TextBox) roots.push_back(st.root);
  for (ElemId root : roots) {
    std::unordered_map<std::string, size_t> openById;  // "kind:id" -> pair index
    std::vector<size_t> fieldStack;
    for (ElemId id = root; id != kNoElem; id = NextInTree(tree_, id, root)) {
      Node n(&tree_, id);
      if (n.DisplayOnly()) continue;
      auto it = sites.find(sc.Elem(n.Raw().def).name);
      if (it == sites.end()) continue;
      for (const auto& [rule, site] : it->second) {
        std::optional<std::string_view> value = site->attrLocal.empty() ? std::nullopt
                                                : n.Attr(NameOf(NsFromUri(site->attrNs), site->attrLocal));
        if (!site->whenValue.empty() && value != site->whenValue) continue;
        uint32_t story = kNone;
        uint32_t para = ParagraphOf(n);
        if (para != kNone) story = paras_[para].story;
        if (rule->nested) {
          if (site->role & pkg::kOpens) {
            PairInfo pi;
            pi.kind = rule->kind;
            pi.open = n;
            pi.story = story;
            pi.openPos = PosOf(n);
            fieldStack.push_back(pairs_.size());
            pairs_.push_back(pi);
          } else if ((site->role & pkg::kCloses) && !fieldStack.empty()) {
            PairInfo& pi = pairs_[fieldStack.back()];
            pi.close = n;
            pi.closePos = PosOf(n);
            fieldStack.pop_back();
          }
          break;
        }
        if (!value) continue;
        std::string key = std::to_string(static_cast<int>(rule->kind)) + ":" + std::string(*value);
        if (site->role & pkg::kOpens) {
          PairInfo pi;
          pi.kind = rule->kind;
          pi.id = *value;
          pi.open = n;
          pi.story = story;
          pi.openPos = PosOf(n);
          openById[key] = pairs_.size();
          pairs_.push_back(pi);
        } else if (auto o = openById.find(key); o != openById.end()) {
          PairInfo& pi = pairs_[o->second];
          pi.close = n;
          pi.closePos = PosOf(n);
          openById.erase(o);
        } else {
          PairInfo pi;  // an end without a start: reported as a pair with no opening
          pi.kind = rule->kind;
          pi.id = *value;
          pi.close = n;
          pi.story = story;
          pi.closePos = PosOf(n);
          pairs_.push_back(pi);
        }
        break;
      }
    }
  }

  BuildSections();
}

void Document::ResetCaches() {
  caches_ = std::make_unique<Caches>();
  ++cacheGeneration_;
}

void Document::BuildSections() {
  // Sections: body-level paragraphs whose properties hold a w:sectPr, then the body's own.
  sections_.clear();
  if (main_ != kNone) {
    std::vector<std::pair<Node, uint32_t>> found;
    for (uint32_t pi : StoryParagraphs(main_)) {
      Node sp = ParaNode(pi).Path({kPPr, kSectPr});
      if (sp && !InTable(ParaNode(pi)) && !ParaNode(pi).DisplayOnly()) found.emplace_back(sp, pi);
    }
    found.emplace_back(Node(&tree_, stories_[main_].root).Child(kSectPr), kNone);
    const Part& mainPart = parts_[stories_[main_].part];
    const pkg::RelationshipSet* rels = package_->RelationshipsOf(mainPart.name);
    std::array<uint32_t, 3> headers{kNone, kNone, kNone}, footers{kNone, kNone, kNone};
    for (auto [sp, endPara] : found) {
      Section sec;
      sec.sectPr = sp;
      sec.endPara = endPara;
      sec.props = ResolveSection(sp);
      for (Node ref : sp.Kids()) {
        bool header = ref.Is(Name("headerReference"));
        if (!header && !ref.Is(Name("footerReference"))) continue;
        std::string_view type = ref.AttrOr(kType, "default");
        size_t slot = type == "first" ? 1 : type == "even" ? 2 : 0;
        const pkg::Relationship* rel = rels ? rels->Find(ref.AttrOr(Name("r:id"))) : nullptr;
        uint32_t story = kNone;
        if (rel)
          for (uint32_t s = 0; s < stories_.size(); ++s)
            if ((stories_[s].kind == StoryKind::Header || stories_[s].kind == StoryKind::Footer) &&
                pkg::PartKey(parts_[stories_[s].part].name) == pkg::PartKey(rel->targetPart))
              story = s;
        (header ? headers : footers)[slot] = story;  // a reference replaces the inherited one
      }
      sec.headers = headers;
      sec.footers = footers;
      sections_.push_back(std::move(sec));
    }
  }
}

}  // namespace fw::doc
