#include "slide/edit/TimingOps.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <set>

#include "core/Lang.h"
#include "slide/edit/EditInternal.h"

// Transitions and the main animation sequence (P3, p3-anim). PowerPoint 2021's own forms, read from
// the corpus (FeatherweightNotes\p3\p3-anim-state.md): a transition is wrapped in mc:AlternateContent
// (a p14 Choice with p14:dur) only when its duration isn't its spd's; the main sequence is click
// groups of time groups of effects, a time group starting where the one before it ends.
namespace fw::slide::edit {
namespace {

constexpr std::string_view kMc = pkg::mce::kNamespace;
constexpr const char* kPNs = "http://schemas.openxmlformats.org/presentationml/2006/main";

bool IsP(pugi::xml_node n, std::string_view local) { return n.type() == pugi::node_element && Is(n, kP, local); }
pugi::xml_node PChild(pugi::xml_node n, std::string_view local) { return DirectChild(n, kP, local); }

bool ParseMs(std::string_view s, double& v) {
  if (s.empty() || s.size() > 32) return false;
  std::string t(s);
  char* end = nullptr;
  v = std::strtod(t.c_str(), &end);
  return end && *end == 0 && std::isfinite(v);
}
std::string MsText(double v) { return std::to_string(std::llround(v)); }

template <class F>
void ForElements(pugi::xml_node n, F&& f) {
  for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element) f(c);
}

// A fragment written with the prefixes p (PresentationML) and, in transitions, mc and p14 declared
// where PowerPoint declares them; copied in with InsertCopy, which declares p where the slide binds
// it otherwise.
bool Fragment(const std::string& inner, pugi::xml_document& doc) {
  return ParseFragment("<w xmlns:p=\"" + std::string(kPNs) + "\">" + inner + "</w>", doc);
}

void SetAttr(pugi::xml_node n, const char* name, const std::string& v) {
  pugi::xml_attribute a = n.attribute(name);
  (a ? a : n.append_attribute(name)).set_value(v.c_str());
}

// ---------------------------------------------------------------------------------------------
// Transitions

struct TransOpt {
  UiText16 label;
  const char* dir;     // null: none
  const char* orient;  // split
  bool thruBlk;
};

constexpr TransOpt kFadeOpts[] = {{N_(u"Smoothly"), nullptr, nullptr, false}, {N_(u"Through Black"), nullptr, nullptr, true}};
constexpr TransOpt kPushOpts[] = {{N_(u"From Bottom"), "u", nullptr, false},
                                  {N_(u"From Left"), "r", nullptr, false},
                                  {N_(u"From Right"), "l", nullptr, false},
                                  {N_(u"From Top"), "d", nullptr, false}};
constexpr TransOpt kWipeOpts[] = {{N_(u"From Right"), "l", nullptr, false},
                                  {N_(u"From Top"), "d", nullptr, false},
                                  {N_(u"From Left"), "r", nullptr, false},
                                  {N_(u"From Bottom"), "u", nullptr, false}};
constexpr TransOpt kSplitOpts[] = {{N_(u"Vertical Out"), "out", "vert", false},
                                   {N_(u"Vertical In"), "in", "vert", false},
                                   {N_(u"Horizontal Out"), "out", "horz", false},
                                   {N_(u"Horizontal In"), "in", "horz", false}};
constexpr TransOpt kCoverOpts[] = {{N_(u"From Right"), "l", nullptr, false},       {N_(u"From Top"), "d", nullptr, false},
                                   {N_(u"From Left"), "r", nullptr, false},        {N_(u"From Bottom"), "u", nullptr, false},
                                   {N_(u"From Top-Right"), "ld", nullptr, false},  {N_(u"From Bottom-Right"), "lu", nullptr, false},
                                   {N_(u"From Top-Left"), "rd", nullptr, false},   {N_(u"From Bottom-Left"), "ru", nullptr, false}};
constexpr TransOpt kZoomOpts[] = {{N_(u"In"), "in", nullptr, false}, {N_(u"Out"), "out", nullptr, false}};

struct TransDef {
  TransitionEffect effect;
  const char* element;  // its element's local name
  UiText16 name;
  double ms;
  const TransOpt* opts;
  int count;
  const char* defaultDir;  // the schema's default dir
};

constexpr TransDef kTrans[] = {
    {TransitionEffect::None, "", N_(u"None"), 0, nullptr, 0, nullptr},
    {TransitionEffect::Cut, "cut", N_(u"Cut"), 0, nullptr, 0, nullptr},
    {TransitionEffect::Fade, "fade", N_(u"Fade"), 700, kFadeOpts, 2, nullptr},
    {TransitionEffect::Push, "push", N_(u"Push"), 1000, kPushOpts, 4, "l"},
    {TransitionEffect::Wipe, "wipe", N_(u"Wipe"), 1000, kWipeOpts, 4, "l"},
    {TransitionEffect::Split, "split", N_(u"Split"), 1500, kSplitOpts, 4, "out"},
    {TransitionEffect::Uncover, "pull", N_(u"Uncover"), 1000, kCoverOpts, 8, "l"},
    {TransitionEffect::Cover, "cover", N_(u"Cover"), 1000, kCoverOpts, 8, "l"},
    {TransitionEffect::Zoom, "zoom", N_(u"Zoom"), 500, kZoomOpts, 2, "out"},
    {TransitionEffect::Other, "", N_(u"Other"), 0, nullptr, 0, nullptr},
};

const TransDef& DefOf(TransitionEffect e) {
  for (const TransDef& d : kTrans)
    if (d.effect == e) return d;
  return kTrans[0];
}

// The spd a duration is written with, and that spd's own duration (when p14:dur isn't needed).
const char* SpdFor(double ms) {
  if (ms == 500) return "fast";
  if (ms == 750) return "med";
  if (ms == 1000) return "slow";
  return ms < 500 ? "fast" : ms < 750 ? "med" : "slow";
}
double SpdMs(std::string_view spd) { return spd == "slow" ? 1000 : spd == "med" ? 750 : 500; }

// A slide's transition: a p:transition, or an mc:AlternateContent whose branches hold them.
struct TransSlot {
  pugi::xml_node node;
  struct Branch {
    pugi::xml_node transition;
    bool choice = false;
  };
  std::vector<Branch> branches;
  pugi::xml_node primary;  // read for the info: the first Choice's, else the plain one, else the Fallback's
};

TransSlot FindTransition(pugi::xml_node root) {
  TransSlot s;
  for (pugi::xml_node c = root.first_child(); c && !s.node; c = c.next_sibling()) {
    if (IsP(c, "transition")) {
      s.node = c;
      s.branches.push_back({c, false});
    } else if (c.type() == pugi::node_element && Is(c, kMc, "AlternateContent")) {
      ForElements(c, [&](pugi::xml_node b) {
        bool choice = Is(b, kMc, "Choice");
        if (!choice && !Is(b, kMc, "Fallback")) return;
        if (pugi::xml_node t = PChild(b, "transition")) s.branches.push_back({t, choice});
      });
      if (!s.branches.empty()) s.node = c;
    }
  }
  for (const auto& b : s.branches)
    if (b.choice) {
      s.primary = b.transition;
      break;
    }
  if (!s.primary && !s.branches.empty()) s.primary = s.branches.front().transition;
  return s;
}

pugi::xml_node EffectElement(pugi::xml_node transition) {
  for (pugi::xml_node c = transition.first_child(); c; c = c.next_sibling()) {
    if (c.type() != pugi::node_element) continue;
    if (IsP(c, "sndAc") || IsP(c, "extLst")) continue;
    return c;
  }
  return {};
}

int OptionOf(const TransDef& d, pugi::xml_node el) {
  if (!d.count) return 0;
  std::string_view dir = el.attribute("dir").value(), orient = el.attribute("orient").value();
  if (dir.empty() && d.defaultDir) dir = d.defaultDir;
  if (orient.empty()) orient = "horz";
  bool thru = Bool(el.attribute("thruBlk").value(), false);
  for (int i = 0; i < d.count; ++i) {
    const TransOpt& o = d.opts[i];
    if (o.dir && dir != o.dir) continue;
    if (!o.dir && !el.attribute("dir").empty()) continue;
    if (o.orient && orient != o.orient) continue;
    if (o.thruBlk != thru) continue;
    return i;
  }
  return -1;
}

TransitionInfo ReadTransition(pugi::xml_node root) {
  TransitionInfo info;
  TransSlot s = FindTransition(root);
  if (!s.primary) return info;
  pugi::xml_node t = s.primary;
  info.advanceOnClick = Bool(t.attribute("advClick").value(), true);
  double v;
  if (ParseMs(t.attribute("advTm").value(), v) && v >= 0) info.advanceAfterMs = v;
  for (const auto& b : s.branches) info.sound = info.sound || PChild(b.transition, "sndAc");

  pugi::xml_node el = EffectElement(t);
  bool foreign = false;
  for (const auto& b : s.branches) {
    pugi::xml_node e = EffectElement(b.transition);
    if (e && NamespaceOf(e) != kP) foreign = true;
  }
  if (!el) {
    info.effect = foreign ? TransitionEffect::Other : TransitionEffect::None;
  } else {
    info.effect = TransitionEffect::Other;
    if (!foreign) {
      for (const TransDef& d : kTrans) {
        if (!*d.element || LocalNameOf(el) != d.element) continue;
        int option = OptionOf(d, el);
        if (option >= 0) {
          info.effect = d.effect;
          info.option = option;
        }
        break;
      }
    }
  }
  if (info.effect == TransitionEffect::Other) {
    for (const auto& b : s.branches) {
      pugi::xml_node e = EffectElement(b.transition);
      if (!e) continue;
      info.element = e.name();
      for (pugi::xml_attribute a = e.first_attribute(); a; a = a.next_attribute())
        info.element.append(" ").append(a.name()).append("=").append(a.value());
      break;
    }
  }
  if (info.effect == TransitionEffect::None || info.effect == TransitionEffect::Cut) return info;
  info.durationMs = SpdMs(t.attribute("spd").value());
  for (const auto& b : s.branches) {
    pugi::xml_attribute dur = FindAttribute(b.transition, kP14, "dur");
    if (b.choice && dur && ParseMs(dur.value(), v) && v >= 0) {
      info.durationMs = v;
      break;
    }
  }
  return info;
}

// Where a new transition goes: before the timing and the extension list.
pugi::xml_node TransitionPlace(pugi::xml_node root) {
  for (pugi::xml_node c = root.first_child(); c; c = c.next_sibling()) {
    if (c.type() != pugi::node_element) continue;
    if (IsP(c, "timing") || IsP(c, "extLst")) return c;
    if (Is(c, kMc, "AlternateContent") && c.find_node([](pugi::xml_node n) { return IsP(n, "timing"); })) return c;
  }
  return {};
}

struct TransTarget {
  TransitionEffect effect = TransitionEffect::None;
  int option = 0;
  double ms = 0;
  bool click = true;
  double after = -1;
};

// Appends `parts` to `x`: one call instead of a chain of string temporaries (much smaller code).
void Cat(std::string& x, std::initializer_list<std::string_view> parts) {
  for (std::string_view part : parts) x += part;
}

std::string TransitionXml(const TransTarget& t, bool choice) {
  const TransDef& d = DefOf(t.effect);
  const bool timed = t.effect != TransitionEffect::None && t.effect != TransitionEffect::Cut;
  std::string x = "<p:transition";
  if (timed) {
    const std::string_view spd = SpdFor(t.ms);
    if (spd != "fast") Cat(x, {" spd=\"", spd, "\""});
    if (choice) Cat(x, {" p14:dur=\"", MsText(t.ms), "\""});
  }
  if (!t.click) x += " advClick=\"0\"";
  if (t.after >= 0) Cat(x, {" advTm=\"", MsText(t.after), "\""});
  x += ">";
  if (*d.element) {
    Cat(x, {"<p:", d.element});
    if (d.count) {
      const TransOpt& o = d.opts[t.option];
      if (o.orient && std::string_view(o.orient) != "horz") Cat(x, {" orient=\"", o.orient, "\""});
      if (o.dir && (!d.defaultDir || std::string_view(o.dir) != d.defaultDir)) Cat(x, {" dir=\"", o.dir, "\""});
      if (o.thruBlk) x += " thruBlk=\"1\"";
    }
    x += "/>";
  }
  return x + "</p:transition>";
}

std::string Serialize(pugi::xml_node n) {
  struct Writer : pugi::xml_writer {
    std::string s;
    void write(const void* data, size_t size) override { s.append(static_cast<const char*>(data), size); }
  } w;
  n.print(w, "", pugi::format_raw);
  return w.s;
}

// The prefix bound to `ns` at `n`, declared on `n` itself as `fallback` when none is.
std::string DeclareHere(pugi::xml_node n, std::string_view ns, std::string_view fallback) {
  for (pugi::xml_node a = n; a && a.type() == pugi::node_element; a = a.parent()) {
    for (pugi::xml_attribute at = a.first_attribute(); at; at = at.next_attribute()) {
      std::string_view name = at.name();
      if (name.starts_with("xmlns:") && at.value() == ns) {
        std::string prefix(name.substr(6));
        auto bound = pkg::LookupNamespace(n, prefix);
        if (bound && *bound == ns) return prefix;
      }
    }
  }
  std::string prefix(fallback);
  for (int i = 2; pkg::LookupNamespace(n, prefix).has_value(); ++i) prefix = std::string(fallback) + std::to_string(i);
  n.append_attribute(("xmlns:" + prefix).c_str()) = std::string(ns).c_str();
  return prefix;
}

// The relationships a copied transition names (a sound's r:embed), made on the slide it lands on.
bool CopyRels(Editor::Impl& m, std::string_view from, std::string_view to, pugi::xml_node copy) {
  if (from == to) return true;
  const pkg::RelationshipSet* src = m.Rels(from);
  std::vector<std::pair<pugi::xml_attribute, std::string>> sites;
  for (const pkg::FoundRef& f : m.registry.Find(copy, pkg::RefKind::RelationshipId)) {
    if (!f.attr) return false;
    const pkg::Relationship* r = src ? src->Find(f.Value()) : nullptr;
    if (!r) return false;
    std::string id;
    if (const pkg::RelationshipSet* dst = m.Rels(to)) {
      for (const pkg::Relationship& d : dst->items)
        if (d.type == r->type && d.external == r->external && (r->external ? d.target == r->target : d.targetPart == r->targetPart))
          id = d.id;
    }
    if (id.empty()) id = m.AddRel(to, r->type, r->external ? r->target : r->targetPart, r->external);
    sites.emplace_back(f.attr, id);
  }
  for (auto& [a, v] : sites) pugi::xml_attribute(a).set_value(v.c_str());
  return true;
}

// The transition of the slide's layout, else of its master: what a slide without its own plays (as
// PowerPoint). `from` gets the part it's in.
pugi::xml_node LayoutTransition(Editor::Impl& m, std::string_view slidePart, std::string* from) {
  const std::string layout = TargetOfType(m.Rels(slidePart), "slideLayout");
  const std::string master = layout.empty() ? std::string() : TargetOfType(m.Rels(layout), "slideMaster");
  for (const std::string& p : {layout, master}) {
    pugi::xml_node root = p.empty() ? pugi::xml_node() : m.Root(p);
    if (pugi::xml_node n = root ? FindTransition(root).node : pugi::xml_node()) {
      if (from) *from = p;
      return n;
    }
  }
  return {};
}

// Declares in the copy `c` of `s` (from another part) the prefixes it uses that aren't bound where it
// landed; false when one is bound to another namespace there.
bool BindLike(pugi::xml_node c, pugi::xml_node s) {
  auto bind = [&](std::string_view qname) {
    const size_t colon = qname.find(':');
    if (colon == std::string_view::npos) return true;
    const std::string prefix(qname.substr(0, colon));
    if (prefix == "xmlns" || prefix == "xml") return true;
    const auto want = pkg::LookupNamespace(s, prefix);
    if (!want) return true;
    if (const auto have = pkg::LookupNamespace(c, prefix)) return *have == *want;
    c.append_attribute(("xmlns:" + prefix).c_str()) = std::string(*want).c_str();
    return true;
  };
  if (!bind(c.name())) return false;
  for (pugi::xml_attribute a = c.first_attribute(); a; a = a.next_attribute())
    if (!bind(a.name())) return false;
  for (pugi::xml_node cc = c.first_child(), sc = s.first_child(); cc && sc; cc = cc.next_sibling(), sc = sc.next_sibling())
    if (cc.type() == pugi::node_element && !BindLike(cc, sc)) return false;
  return true;
}

// A transition from part `from` copied into slide `root` (in `part`) before `before`, with the
// relationships and prefixes it needs; null when it can't be.
pugi::xml_node CopyTransition(Editor::Impl& m, std::string_view from, pugi::xml_node src, std::string_view part,
                              pugi::xml_node root, pugi::xml_node before) {
  pugi::xml_node copy = InsertCopy(root, src, before);
  if (BindLike(copy, src) && CopyRels(m, from, part, copy)) return copy;
  root.remove_child(copy);
  return {};
}

Result SetOneTransition(Editor::Impl& m, const std::string& part, const TransitionPatch& patch) {
  pugi::xml_node root = m.Root(part);
  if (!root) return Result::Fail(T(u"The slide can't be read."));
  std::string from;
  const bool layoutHas = static_cast<bool>(LayoutTransition(m, part, &from));
  if (layoutHas && !FindTransition(root).node) {
    // A slide playing its layout's transition gets its own copy first, as PowerPoint writes one when
    // it's changed.
    root = m.Edit(part);
    if (!CopyTransition(m, from, LayoutTransition(m, part, nullptr), part, root, TransitionPlace(root)))
      return Result::Fail(T(u"The layout's transition can't be copied to the slide."));
  }
  TransitionInfo now = ReadTransition(root);
  TransTarget t{now.effect, now.option, now.durationMs, now.advanceOnClick, now.advanceAfterMs};
  bool newEffect = patch.effect && *patch.effect != now.effect;
  if (patch.effect) t.effect = *patch.effect;
  const TransDef& d = DefOf(t.effect);
  if (newEffect) {
    t.option = 0;
    t.ms = d.ms;
  }
  if (patch.option) {
    if (t.effect == TransitionEffect::Other) return Result::Fail(T(u"This transition's options can't be changed in Featherweight."));
    if (*patch.option < 0 || *patch.option >= std::max(d.count, 1)) return Result::Fail(T(u"That effect option isn't available."));
    t.option = *patch.option;
  }
  bool timed = t.effect != TransitionEffect::None && t.effect != TransitionEffect::Cut;
  if (patch.durationMs && timed) t.ms = std::round(*patch.durationMs);
  if (patch.advanceOnClick) t.click = *patch.advanceOnClick;
  if (patch.advanceAfterMs) t.after = *patch.advanceAfterMs < 0 ? -1 : std::round(*patch.advanceAfterMs);

  root = m.Edit(part);
  TransSlot s = FindTransition(root);
  if (t.effect == TransitionEffect::Other) {
    // Kept: only its timing changes, in every branch (p14:dur in the Choices, as PowerPoint writes it).
    bool durChanged = patch.durationMs.has_value();
    const char* spd = SpdFor(t.ms);
    bool wrap = false;
    for (const auto& b : s.branches) {
      pugi::xml_node tr = b.transition;
      if (durChanged) {
        if (std::string_view(spd) == "fast") tr.remove_attribute("spd");
        else SetAttr(tr, "spd", spd);
        if (b.choice) {
          pugi::xml_attribute dur = FindAttribute(tr, kP14, "dur");
          if (!dur) {
            std::string p14 = DeclareHere(tr.parent(), kP14, "p14");
            dur = tr.append_attribute((p14 + ":dur").c_str());
          }
          dur.set_value(MsText(t.ms).c_str());
        } else if (s.branches.size() == 1 && t.ms != SpdMs(spd)) {
          wrap = true;
        }
      }
      if (t.click) tr.remove_attribute("advClick");
      else SetAttr(tr, "advClick", "0");
      if (t.after >= 0) SetAttr(tr, "advTm", MsText(t.after));
      else tr.remove_attribute("advTm");
    }
    if (wrap) {
      // A plain transition whose duration isn't its spd's: a p14 Choice with p14:dur and the plain one.
      pugi::xml_node plain = s.node;
      pugi::xml_document doc;
      std::string ac = "<mc:AlternateContent xmlns:mc=\"" + std::string(kMc) + "\"><mc:Choice xmlns:p14=\"" +
                       std::string(kP14) + "\" Requires=\"p14\"/><mc:Fallback/></mc:AlternateContent>";
      if (!Fragment(ac, doc)) return Result::Fail(T(u"The transition can't be written."));
      pugi::xml_node made = InsertCopy(root, doc.document_element().first_child(), plain);
      pugi::xml_node choice = made.first_child(), fallback = choice.next_sibling();
      pugi::xml_node c = InsertCopy(choice, plain);
      InsertCopy(fallback, plain);
      c.append_attribute("p14:dur") = MsText(t.ms).c_str();
      root.remove_child(plain);
    }
    return {};
  }

  // A transition Featherweight writes: made anew, its sound and extensions kept.
  pugi::xml_node snd = PChild(s.primary, "sndAc"), ext = PChild(s.primary, "extLst");
  pugi::xml_node place = s.node ? s.node : TransitionPlace(root);
  // None on a slide whose layout has a transition is an empty one of its own: without it, the
  // layout's would play.
  bool empty = t.effect == TransitionEffect::None && t.click && t.after < 0 && !snd && !ext && !layoutHas;
  if (!empty) {
    std::string x;
    if (timed && t.ms != SpdMs(SpdFor(t.ms))) {
      x = "<mc:AlternateContent xmlns:mc=\"" + std::string(kMc) + "\"><mc:Choice xmlns:p14=\"" + std::string(kP14) +
          "\" Requires=\"p14\">" + TransitionXml(t, true) + "</mc:Choice><mc:Fallback>" + TransitionXml(t, false) +
          "</mc:Fallback></mc:AlternateContent>";
    } else {
      x = TransitionXml(t, false);
    }
    pugi::xml_document doc;
    if (!Fragment(x, doc)) return Result::Fail(T(u"The transition can't be written."));
    pugi::xml_node made = InsertCopy(root, doc.document_element().first_child(), place);
    std::vector<pugi::xml_node> made2;
    if (IsP(made, "transition")) made2.push_back(made);
    ForElements(made, [&](pugi::xml_node b) {
      if (pugi::xml_node tr = PChild(b, "transition")) made2.push_back(tr);
    });
    for (pugi::xml_node tr : made2) {
      if (snd) InsertCopy(tr, snd);
      if (ext) InsertCopy(tr, ext);
    }
  }
  if (s.node) root.remove_child(s.node);
  return {};
}

// ---------------------------------------------------------------------------------------------
// Animations: the presets

struct AnimOpt {
  UiText16 label;
  int subtype;
  int presetId;  // 0: the animation's own
};

constexpr AnimOpt kNoOpts[] = {{u"", 0, 0}};
constexpr AnimOpt kFlyInOpts[] = {{N_(u"From Bottom"), 4, 0},    {N_(u"From Bottom-Left"), 12, 0}, {N_(u"From Left"), 8, 0},
                                  {N_(u"From Top-Left"), 9, 0},  {N_(u"From Top"), 1, 0},          {N_(u"From Top-Right"), 3, 0},
                                  {N_(u"From Right"), 2, 0},     {N_(u"From Bottom-Right"), 6, 0}};
constexpr AnimOpt kFlyOutOpts[] = {{N_(u"To Bottom"), 4, 0},   {N_(u"To Bottom-Left"), 12, 0}, {N_(u"To Left"), 8, 0},
                                   {N_(u"To Top-Left"), 9, 0}, {N_(u"To Top"), 1, 0},          {N_(u"To Top-Right"), 3, 0},
                                   {N_(u"To Right"), 2, 0},    {N_(u"To Bottom-Right"), 6, 0}};
constexpr AnimOpt kFloatInOpts[] = {{N_(u"Float Up"), 0, 42}, {N_(u"Float Down"), 0, 47}};
constexpr AnimOpt kFloatOutOpts[] = {{N_(u"Float Down"), 0, 47}, {N_(u"Float Up"), 0, 42}};
constexpr AnimOpt kSplitOpts2[] = {{N_(u"Horizontal In"), 26, 0}, {N_(u"Horizontal Out"), 42, 0}, {N_(u"Vertical In"), 21, 0}, {N_(u"Vertical Out"), 37, 0}};
constexpr AnimOpt kWipeInOpts[] = {{N_(u"From Bottom"), 4, 0}, {N_(u"From Left"), 8, 0}, {N_(u"From Right"), 2, 0}, {N_(u"From Top"), 1, 0}};
constexpr AnimOpt kWipeOutOpts[] = {{N_(u"To Bottom"), 4, 0}, {N_(u"To Left"), 8, 0}, {N_(u"To Right"), 2, 0}, {N_(u"To Top"), 1, 0}};
constexpr AnimOpt kZoomOpts2[] = {{N_(u"Object Center"), 16, 0}};
constexpr AnimOpt kZoomOutOpts[] = {{N_(u"Object Center"), 32, 0}};  // PowerPoint 2021 writes the exit as #53/32
constexpr AnimOpt kGrowOpts[] = {{N_(u"Both"), 0, 0}, {N_(u"Horizontal"), 0, 0}, {N_(u"Vertical"), 0, 0}};
constexpr AnimOpt kSpinOpts[] = {{N_(u"Clockwise"), 0, 0}, {N_(u"Counterclockwise"), 0, 0}};

struct AnimDef {
  Anim anim;
  AnimClass cls;
  const char* presetClass;
  int presetId;
  UiText16 name;
  double ms;
  int defaultOption;
  const AnimOpt* opts;
  int count;  // 0: no options (kNoOpts' one entry, subtype 0)
};

constexpr AnimDef kAnims[] = {
    {Anim::Appear, AnimClass::Entrance, "entr", 1, N_(u"Appear"), 0, 0, kNoOpts, 0},
    {Anim::Fade, AnimClass::Entrance, "entr", 10, N_(u"Fade"), 500, 0, kNoOpts, 0},
    {Anim::FlyIn, AnimClass::Entrance, "entr", 2, N_(u"Fly In"), 500, 0, kFlyInOpts, 8},
    {Anim::FloatIn, AnimClass::Entrance, "entr", 42, N_(u"Float In"), 1000, 0, kFloatInOpts, 2},
    {Anim::Split, AnimClass::Entrance, "entr", 16, N_(u"Split"), 500, 2, kSplitOpts2, 4},
    {Anim::Wipe, AnimClass::Entrance, "entr", 22, N_(u"Wipe"), 500, 0, kWipeInOpts, 4},
    {Anim::Zoom, AnimClass::Entrance, "entr", 53, N_(u"Zoom"), 500, 0, kZoomOpts2, 1},
    {Anim::GrowShrink, AnimClass::Emphasis, "emph", 6, u"Grow/Shrink", 2000, 0, kGrowOpts, 3},
    {Anim::Spin, AnimClass::Emphasis, "emph", 8, N_(u"Spin"), 2000, 0, kSpinOpts, 2},
    {Anim::Disappear, AnimClass::Exit, "exit", 1, N_(u"Disappear"), 0, 0, kNoOpts, 0},
    {Anim::FadeOut, AnimClass::Exit, "exit", 10, N_(u"Fade"), 500, 0, kNoOpts, 0},
    {Anim::FlyOut, AnimClass::Exit, "exit", 2, N_(u"Fly Out"), 500, 0, kFlyOutOpts, 8},
    {Anim::FloatOut, AnimClass::Exit, "exit", 47, N_(u"Float Out"), 1000, 0, kFloatOutOpts, 2},
    {Anim::SplitOut, AnimClass::Exit, "exit", 16, N_(u"Split"), 500, 2, kSplitOpts2, 4},
    {Anim::WipeOut, AnimClass::Exit, "exit", 22, N_(u"Wipe"), 500, 0, kWipeOutOpts, 4},
    {Anim::ZoomOut, AnimClass::Exit, "exit", 53, N_(u"Zoom"), 500, 0, kZoomOutOpts, 1},
};

const AnimDef* AnimDefOf(Anim a) {
  for (const AnimDef& d : kAnims)
    if (d.anim == a) return &d;
  return nullptr;
}

const AnimOpt& OptOf(const AnimDef& d, int option) { return d.count ? d.opts[option] : kNoOpts[0]; }
int PresetIdOf(const AnimDef& d, int option) {
  const AnimOpt& o = OptOf(d, option);
  return o.presetId ? o.presetId : d.presetId;
}

struct Ids {
  uint32_t next = 1;
  std::string operator()() { return std::to_string(next++); }
};

void Val(std::string& x, std::string_view v) {
  if (v == "0") x += "<p:fltVal val=\"0\"/>";
  else Cat(x, {"<p:strVal val=\"", v, "\"/>"});
}

void SetVis(std::string& x, Ids& id, std::string_view tgt, bool visible, double delay) {
  Cat(x, {"<p:set><p:cBhvr><p:cTn id=\"", id(), "\" dur=\"1\" fill=\"hold\"><p:stCondLst><p:cond delay=\"", MsText(delay),
          "\"/></p:stCondLst></p:cTn>", tgt,
          "<p:attrNameLst><p:attrName>style.visibility</p:attrName></p:attrNameLst></p:cBhvr><p:to><p:strVal val=\"",
          visible ? "visible" : "hidden", "\"/></p:to></p:set>"});
}

void Filter(std::string& x, Ids& id, std::string_view tgt, bool in, std::string_view filter, double dur) {
  Cat(x, {"<p:animEffect transition=\"", in ? "in" : "out", "\" filter=\"", filter, "\"><p:cBhvr><p:cTn id=\"", id(),
          "\" dur=\"", MsText(dur), "\"/>", tgt, "</p:cBhvr></p:animEffect>"});
}

void Prop(std::string& x, Ids& id, std::string_view tgt, const char* attr, std::string_view from, std::string_view to, double dur,
          bool base, bool hold) {
  Cat(x, {"<p:anim calcmode=\"lin\" valueType=\"num\"><p:cBhvr", base ? " additive=\"base\"" : "", "><p:cTn id=\"", id(),
          "\" dur=\"", MsText(dur), "\"", hold ? " fill=\"hold\"" : "", "/>", tgt, "<p:attrNameLst><p:attrName>", attr,
          "</p:attrName></p:attrNameLst></p:cBhvr><p:tavLst><p:tav tm=\"0\"><p:val>"});
  Val(x, from);
  x += "</p:val></p:tav><p:tav tm=\"100000\"><p:val>";
  Val(x, to);
  x += "</p:val></p:tav></p:tavLst></p:anim>";
}

const char* WipeFilter(int sub) { return sub == 1 ? "wipe(up)" : sub == 2 ? "wipe(right)" : sub == 8 ? "wipe(left)" : "wipe(down)"; }
const char* SplitFilter(int sub) {
  return sub == 21 ? "barn(inVertical)" : sub == 26 ? "barn(inHorizontal)" : sub == 37 ? "barn(outVertical)" : "barn(outHorizontal)";
}

// The behaviors of an animation, as PowerPoint writes them (the corpus' and PowerPoint 2021's).
std::string Behaviors(const AnimDef& d, int option, double dur, std::string_view tgt, Ids& id) {
  const int sub = OptOf(d, option).subtype;
  const bool down = PresetIdOf(d, option) == 47;  // Float: Down (47) or Up (42)
  std::string x;
  switch (d.anim) {
    case Anim::Appear:
      SetVis(x, id, tgt, true, 0);
      break;
    case Anim::Fade:
      SetVis(x, id, tgt, true, 0);
      Filter(x, id, tgt, true, "fade", dur);
      break;
    case Anim::FlyIn:
      SetVis(x, id, tgt, true, 0);
      Prop(x, id, tgt, "ppt_x", sub & 8 ? "0-#ppt_w/2" : sub & 2 ? "1+#ppt_w/2" : "#ppt_x", "#ppt_x", dur, true, true);
      Prop(x, id, tgt, "ppt_y", sub & 1 ? "0-#ppt_h/2" : sub & 4 ? "1+#ppt_h/2" : "#ppt_y", "#ppt_y", dur, true, true);
      break;
    case Anim::FloatIn:
      SetVis(x, id, tgt, true, 0);
      Filter(x, id, tgt, true, "fade", dur);
      Prop(x, id, tgt, "ppt_x", "#ppt_x", "#ppt_x", dur, false, true);
      Prop(x, id, tgt, "ppt_y", down ? "#ppt_y-.1" : "#ppt_y+.1", "#ppt_y", dur, false, true);
      break;
    case Anim::Split:
    case Anim::Wipe:
      SetVis(x, id, tgt, true, 0);
      Filter(x, id, tgt, true, d.anim == Anim::Split ? SplitFilter(sub) : WipeFilter(sub), dur);
      break;
    case Anim::Zoom:
      SetVis(x, id, tgt, true, 0);
      Prop(x, id, tgt, "ppt_w", "0", "#ppt_w", dur, false, true);
      Prop(x, id, tgt, "ppt_h", "0", "#ppt_h", dur, false, true);
      Filter(x, id, tgt, true, "fade", dur);
      break;
    case Anim::GrowShrink:
      Cat(x, {"<p:animScale><p:cBhvr><p:cTn id=\"", id(), "\" dur=\"", MsText(dur), "\" fill=\"hold\"/>", tgt,
              "</p:cBhvr><p:by x=\"", option == 2 ? "100000" : "150000", "\" y=\"", option == 1 ? "100000" : "150000",
              "\"/></p:animScale>"});
      break;
    case Anim::Spin:
      Cat(x, {"<p:animRot by=\"", option == 1 ? "-21600000" : "21600000", "\"><p:cBhvr><p:cTn id=\"", id(), "\" dur=\"",
              MsText(dur), "\" fill=\"hold\"/>", tgt, "<p:attrNameLst><p:attrName>r</p:attrName></p:attrNameLst></p:cBhvr></p:animRot>"});
      break;
    case Anim::Disappear:
      SetVis(x, id, tgt, false, 0);
      break;
    case Anim::FadeOut:
    case Anim::SplitOut:
    case Anim::WipeOut:
      Filter(x, id, tgt, false, d.anim == Anim::FadeOut ? "fade" : d.anim == Anim::SplitOut ? SplitFilter(sub) : WipeFilter(sub), dur);
      SetVis(x, id, tgt, false, dur - 1);
      break;
    case Anim::FlyOut:
      Prop(x, id, tgt, "ppt_x", "ppt_x", sub & 8 ? "0-ppt_w/2" : sub & 2 ? "1+ppt_w/2" : "ppt_x", dur, true, false);
      Prop(x, id, tgt, "ppt_y", "ppt_y", sub & 1 ? "0-ppt_h/2" : sub & 4 ? "1+ppt_h/2" : "ppt_y", dur, true, false);
      SetVis(x, id, tgt, false, dur - 1);
      break;
    case Anim::FloatOut:
      Filter(x, id, tgt, false, "fade", dur);
      Prop(x, id, tgt, "ppt_x", "ppt_x", "ppt_x", dur, false, false);
      Prop(x, id, tgt, "ppt_y", "ppt_y", down ? "ppt_y+.1" : "ppt_y-.1", dur, false, false);
      SetVis(x, id, tgt, false, dur - 1);
      break;
    case Anim::ZoomOut:
      Prop(x, id, tgt, "ppt_w", "ppt_w", "0", dur, false, false);
      Prop(x, id, tgt, "ppt_h", "ppt_h", "0", dur, false, false);
      Filter(x, id, tgt, false, "fade", dur);
      SetVis(x, id, tgt, false, dur - 1);
      break;
    default:
      break;
  }
  return x;
}

std::string TargetXml(uint32_t spid, int first, int last) {
  std::string x;
  Cat(x, {"<p:tgtEl><p:spTgt spid=\"", std::to_string(spid), "\""});
  if (first < 0) x += "/></p:tgtEl>";
  else Cat(x, {"><p:txEl><p:pRg st=\"", std::to_string(first), "\" end=\"", std::to_string(last), "\"/></p:txEl></p:spTgt></p:tgtEl>"});
  return x;
}

// ---------------------------------------------------------------------------------------------
// Animations: reading an effect

bool IsBehavior(pugi::xml_node n) {
  std::string_view k = LocalNameOf(n);
  return NamespaceOf(n) == kP && (k == "set" || k == "anim" || k == "animEffect" || k == "animScale" || k == "animRot" ||
                                  k == "animMotion" || k == "animClr");
}

double CondDelay(pugi::xml_node cTn) {
  double v;
  pugi::xml_node cond = PChild(PChild(cTn, "stCondLst"), "cond");
  return cond && ParseMs(cond.attribute("delay").value(), v) ? std::max(0.0, v) : 0;
}

double Repeat(pugi::xml_node cTn) {
  double v;
  double r = ParseMs(cTn.attribute("repeatCount").value(), v) ? std::clamp(v / 1000, 0.0, 1000.0) : 1;
  return r * (Bool(cTn.attribute("autoRev").value(), false) ? 2 : 1);
}

// How long a time node's children play (ms after it starts), as PowerPoint counts it: a 1 ms set
// counts 0 (Appear has no length).
double Length(pugi::xml_node cTn, int depth = 0) {
  if (depth > 32) return 0;
  double end = 0;
  bool seq = false;
  double at = 0;
  pugi::xml_node list = PChild(cTn, "childTnLst");
  seq = IsP(cTn.parent(), "seq");
  ForElements(list, [&](pugi::xml_node c) {
    double len = 0;
    if (IsBehavior(c)) {
      pugi::xml_node bc = PChild(PChild(c, "cBhvr"), "cTn");
      double dur = 0, v;
      if (ParseMs(bc.attribute("dur").value(), v) && v > 1) dur = v;
      len = CondDelay(bc) + dur * Repeat(bc);
    } else if (IsP(c, "par") || IsP(c, "seq")) {
      pugi::xml_node cc = PChild(c, "cTn");
      len = CondDelay(cc) + Length(cc, depth + 1) * Repeat(cc);
    }
    if (seq) {
      at += len;
      end = at;
    } else {
      end = std::max(end, len);
    }
  });
  return end;
}

struct Target {
  uint32_t spid = 0;
  int first = -1, last = -1;
  bool simple = false;  // a shape or a paragraph range of it, the same in every behavior
};

Target ReadTarget(pugi::xml_node sp) {
  Target t;
  double v;
  if (!ParseMs(sp.attribute("spid").value(), v) || v < 0 || v > 4294967295.0) return t;
  t.spid = static_cast<uint32_t>(v);
  t.simple = true;
  int kids = 0;
  ForElements(sp, [&](pugi::xml_node c) {
    ++kids;
    pugi::xml_node rg = PChild(c, "pRg");
    double a, b;
    if (IsP(c, "txEl") && rg && ParseMs(rg.attribute("st").value(), a) && ParseMs(rg.attribute("end").value(), b) && a >= 0 &&
        b >= a && b < 1e6) {
      t.first = static_cast<int>(a);
      t.last = static_cast<int>(b);
    } else {
      t.simple = false;
    }
  });
  if (kids > 1) t.simple = false;
  return t;
}

struct Classified {
  const AnimDef* def = nullptr;
  int option = 0;
  bool editable = false;
  Target target;
  AnimClass cls = AnimClass::Other;
};

AnimClass ClassFromAttr(std::string_view s) {
  if (s == "entr") return AnimClass::Entrance;
  if (s == "emph") return AnimClass::Emphasis;
  if (s == "exit") return AnimClass::Exit;
  if (s == "path") return AnimClass::Path;
  return AnimClass::Other;
}

Classified Classify(pugi::xml_node cTn) {
  Classified r;
  std::string_view cls = cTn.attribute("presetClass").value();
  r.cls = ClassFromAttr(cls);
  double id = Number(cTn.attribute("presetID").value(), -1), sub = Number(cTn.attribute("presetSubtype").value(), 0);
  // The target: every behavior's the same shape (or paragraph range).
  bool first = true, same = true;
  cTn.find_node([&](pugi::xml_node n) {
    if (!IsP(n, "spTgt")) return false;
    Target t = ReadTarget(n);
    if (first) r.target = t;
    else same = same && t.simple && t.spid == r.target.spid && t.first == r.target.first && t.last == r.target.last;
    first = false;
    return false;
  });
  // A sound's or another target's (no spTgt at all) is no shape.
  if (first) r.target.simple = false;
  for (const AnimDef& d : kAnims) {
    if (cls != d.presetClass) continue;
    for (int i = 0; i < std::max(d.count, 1); ++i) {
      const AnimOpt& o = OptOf(d, i);
      bool zoomOld = (d.anim == Anim::Zoom || d.anim == Anim::ZoomOut) && sub == 0;
      if (PresetIdOf(d, i) == id && (o.subtype == sub || zoomOld)) {
        r.def = &d;
        r.option = i;
        break;
      }
    }
    if (r.def) break;
  }
  if (!r.def) return r;
  // Options read from the behaviors.
  if (r.def->anim == Anim::GrowShrink) {
    pugi::xml_node by = cTn.find_node([](pugi::xml_node n) { return IsP(n, "by") && IsP(n.parent(), "animScale"); });
    double x = Number(by.attribute("x").value(), 0), y = Number(by.attribute("y").value(), 0);
    r.option = x != y && y == 100000 ? 1 : x != y && x == 100000 ? 2 : 0;
  } else if (r.def->anim == Anim::Spin) {
    pugi::xml_node rot = cTn.find_node([](pugi::xml_node n) { return IsP(n, "animRot"); });
    r.option = Number(rot.attribute("by").value(), 0) < 0 ? 1 : 0;
  }
  // Editable: plain behaviors with numeric times, one simple target.
  bool plain = r.target.simple && same;
  ForElements(PChild(cTn, "childTnLst"), [&](pugi::xml_node c) {
    if (!IsBehavior(c)) {
      plain = false;
      return;
    }
    pugi::xml_node bc = PChild(PChild(c, "cBhvr"), "cTn");
    double v;
    if (!bc || (bc.attribute("dur") && !ParseMs(bc.attribute("dur").value(), v))) plain = false;
    ForElements(PChild(bc, "stCondLst"), [&](pugi::xml_node cond) {
      if (!cond.attribute("evt").empty() || cond.first_child() || !ParseMs(cond.attribute("delay").value(), v)) plain = false;
    });
  });
  r.editable = plain && PChild(cTn, "childTnLst");
  return r;
}

// ---------------------------------------------------------------------------------------------
// Animations: the main sequence

constexpr const char16_t* kOdd = N_(u"The animations on this slide are stored in a way Featherweight can't change.");

struct Entry {
  pugi::xml_node par, cTn;
  AnimTrigger trigger = AnimTrigger::OnClick;
};

struct MainSeq {
  pugi::xml_node timing, tnLst, rootCTn, rootList, seq, list;
  std::vector<Entry> entries;
  std::vector<std::string> containerIds;  // the click and time groups' ids, reused when rebuilding
  bool ok = true;
};

bool Only(pugi::xml_node cTn, std::initializer_list<std::string_view> attrs) {
  for (pugi::xml_attribute a = cTn.first_attribute(); a; a = a.next_attribute())
    if (std::find(attrs.begin(), attrs.end(), std::string_view(a.name())) == attrs.end()) return false;
  return true;
}

// The two lists a group's p:cTn holds, and nothing else.
bool GroupShape(pugi::xml_node cTn) {
  int n = 0;
  bool ok = true;
  ForElements(cTn, [&](pugi::xml_node c) {
    ok = ok && ((n == 0 && IsP(c, "stCondLst")) || (n == 1 && IsP(c, "childTnLst")));
    ++n;
  });
  return ok && n == 2;
}

MainSeq ReadMainSeq(pugi::xml_node root) {
  MainSeq s;
  for (pugi::xml_node c = root.first_child(); c; c = c.next_sibling()) {
    if (c.type() == pugi::node_element && Is(c, kMc, "AlternateContent") &&
        c.find_node([](pugi::xml_node n) { return IsP(n, "timing"); })) {
      s.ok = false;
      return s;
    }
  }
  s.timing = PChild(root, "timing");
  if (!s.timing) return s;
  auto fail = [&] {
    s.ok = false;
    s.entries.clear();
    return s;
  };
  s.tnLst = PChild(s.timing, "tnLst");
  if (!s.tnLst) return fail();
  int count = 0;
  ForElements(s.tnLst, [&](pugi::xml_node) { ++count; });
  pugi::xml_node rootPar = PChild(s.tnLst, "par");
  s.rootCTn = PChild(rootPar, "cTn");
  if (count != 1 || std::string_view(s.rootCTn.attribute("nodeType").value()) != "tmRoot") return fail();
  s.rootList = PChild(s.rootCTn, "childTnLst");
  bool odd = false;
  ForElements(s.rootList, [&](pugi::xml_node c) {
    if (Is(c, kMc, "AlternateContent")) odd = true;
    if (IsP(c, "seq") && std::string_view(PChild(c, "cTn").attribute("nodeType").value()) == "mainSeq") {
      if (s.seq) odd = true;
      s.seq = c;
    }
  });
  if (odd) return fail();
  if (!s.seq) return s;
  s.list = PChild(PChild(s.seq, "cTn"), "childTnLst");
  if (!s.list) return fail();
  bool firstGroup = true;
  ForElements(s.list, [&](pugi::xml_node l1) {
    if (!s.ok) return;
    pugi::xml_node c1 = PChild(l1, "cTn");
    if (!IsP(l1, "par") || !c1 || !Only(c1, {"id", "fill", "nodeType"}) || !GroupShape(c1)) {
      s.ok = false;
      return;
    }
    // Waits for a click; the first group may also start with the slide.
    int indefinite = 0, begin = 0, other = 0;
    ForElements(PChild(c1, "stCondLst"), [&](pugi::xml_node cond) {
      std::string_view evt = cond.attribute("evt").value(), delay = cond.attribute("delay").value();
      if (IsP(cond, "cond") && delay == "indefinite" && evt.empty() && !cond.first_child()) ++indefinite;
      else if (IsP(cond, "cond") && evt == "onBegin" && firstGroup && PChild(cond, "tn")) ++begin;
      else ++other;
    });
    if (indefinite != 1 || begin > 1 || other) {
      s.ok = false;
      return;
    }
    bool autoStart = begin == 1;
    s.containerIds.emplace_back(c1.attribute("id").value());
    bool firstInClick = true;
    int groups = 0;
    ForElements(PChild(c1, "childTnLst"), [&](pugi::xml_node l2) {
      if (!s.ok) return;
      ++groups;
      pugi::xml_node c2 = PChild(l2, "cTn");
      if (!IsP(l2, "par") || !c2 || !Only(c2, {"id", "fill", "nodeType"}) || !GroupShape(c2)) {
        s.ok = false;
        return;
      }
      int conds = 0;
      double v;
      ForElements(PChild(c2, "stCondLst"), [&](pugi::xml_node cond) {
        ++conds;
        if (!IsP(cond, "cond") || !cond.attribute("evt").empty() || cond.first_child() ||
            !ParseMs(cond.attribute("delay").value(), v))
          s.ok = false;
      });
      if (conds != 1) s.ok = false;
      s.containerIds.emplace_back(c2.attribute("id").value());
      bool firstInTime = true;
      int effects = 0;
      ForElements(PChild(c2, "childTnLst"), [&](pugi::xml_node l3) {
        ++effects;
        pugi::xml_node c3 = PChild(l3, "cTn");
        if (!IsP(l3, "par") || !c3) {
          s.ok = false;
          return;
        }
        AnimTrigger t = AnimTrigger::WithPrevious;
        if (firstInClick) {
          if (autoStart)
            t = std::string_view(c3.attribute("nodeType").value()) == "afterEffect" ? AnimTrigger::AfterPrevious : AnimTrigger::WithPrevious;
          else
            t = AnimTrigger::OnClick;
        } else if (firstInTime) {
          t = AnimTrigger::AfterPrevious;
        }
        s.entries.push_back({l3, c3, t});
        firstInClick = firstInTime = false;
      });
      if (!effects) s.ok = false;
    });
    if (!groups) s.ok = false;
    firstGroup = false;
  });
  if (!s.ok) return fail();
  return s;
}

uint32_t MaxId(pugi::xml_node timing) {
  uint32_t max = 0;
  timing.find_node([&](pugi::xml_node n) {
    if (IsP(n, "cTn")) max = std::max(max, static_cast<uint32_t>(std::clamp(Number(n.attribute("id").value(), 0), 0.0, 4e9)));
    return false;
  });
  return max;
}

// The main sequence made when the slide has none (and the timing, when it has none either).
bool EnsureMainSeq(pugi::xml_node root, MainSeq& s) {
  if (s.seq) return true;
  static const std::string kSeqTail =
      "<p:prevCondLst><p:cond evt=\"onPrev\" delay=\"0\"><p:tgtEl><p:sldTgt/></p:tgtEl></p:cond></p:prevCondLst>"
      "<p:nextCondLst><p:cond evt=\"onNext\" delay=\"0\"><p:tgtEl><p:sldTgt/></p:tgtEl></p:cond></p:nextCondLst>";
  pugi::xml_document doc;
  if (!s.timing) {
    std::string x =
        "<p:timing><p:tnLst><p:par><p:cTn id=\"1\" dur=\"indefinite\" restart=\"never\" nodeType=\"tmRoot\"><p:childTnLst>"
        "<p:seq concurrent=\"1\" nextAc=\"seek\"><p:cTn id=\"2\" dur=\"indefinite\" nodeType=\"mainSeq\"><p:childTnLst/></p:cTn>" +
        kSeqTail + "</p:seq></p:childTnLst></p:cTn></p:par></p:tnLst></p:timing>";
    if (!Fragment(x, doc)) return false;
    InsertCopy(root, doc.document_element().first_child(), PChild(root, "extLst"));
  } else {
    if (!s.rootList) {
      std::string p = PrefixFor(s.rootCTn, kP, "p");
      std::string name = p.empty() ? "childTnLst" : p + ":childTnLst";
      pugi::xml_node sub = PChild(s.rootCTn, "subTnLst");
      if (sub) s.rootCTn.insert_child_before(name.c_str(), sub);
      else s.rootCTn.append_child(name.c_str());
    }
    std::string id = std::to_string(MaxId(s.timing) + 1);
    std::string x = "<p:seq concurrent=\"1\" nextAc=\"seek\"><p:cTn id=\"" + id +
                    "\" dur=\"indefinite\" nodeType=\"mainSeq\"><p:childTnLst/></p:cTn>" + kSeqTail + "</p:seq>";
    if (!Fragment(x, doc)) return false;
    pugi::xml_node list = PChild(s.rootCTn, "childTnLst");
    pugi::xml_node first;
    for (first = list.first_child(); first && first.type() != pugi::node_element; first = first.next_sibling()) {
    }
    InsertCopy(list, doc.document_element().first_child(), first);
  }
  s = ReadMainSeq(root);
  return s.ok && s.seq;
}

const char* NodeTypeOf(AnimTrigger t) {
  return t == AnimTrigger::OnClick ? "clickEffect" : t == AnimTrigger::WithPrevious ? "withEffect" : "afterEffect";
}

// The main sequence written again from `entries` (their effect nodes moved, not rewritten): click
// groups at each On Click, time groups at each After Previous, each time group starting where the
// one before it ends. No effects left: the sequence goes, and the timing with it when nothing else
// is in it.
void Rebuild(pugi::xml_node root, MainSeq& s, const std::vector<Entry>& entries) {
  if (entries.empty()) {
    s.rootList.remove_child(s.seq);
    bool any = false;
    ForElements(s.rootList, [&](pugi::xml_node) { any = true; });
    if (!any) root.remove_child(s.timing);
    return;
  }
  std::vector<pugi::xml_node> old;
  ForElements(s.list, [&](pugi::xml_node c) { old.push_back(c); });
  Ids ids{MaxId(s.timing) + 1};
  size_t pool = 0;
  auto nextId = [&] { return pool < s.containerIds.size() ? s.containerIds[pool++] : ids(); };
  std::string p = PrefixFor(s.list, kP, "p");
  auto q = [&](const char* local) { return p.empty() ? std::string(local) : p + ":" + local; };
  std::string mainId = PChild(s.seq, "cTn").attribute("id").value();

  auto group = [&](pugi::xml_node parent, const std::string& delay, bool begin) {
    pugi::xml_node par = parent.append_child(q("par").c_str());
    pugi::xml_node c = par.append_child(q("cTn").c_str());
    c.append_attribute("id") = nextId().c_str();
    c.append_attribute("fill") = "hold";
    pugi::xml_node st = c.append_child(q("stCondLst").c_str());
    st.append_child(q("cond").c_str()).append_attribute("delay") = delay.c_str();
    if (begin) {
      pugi::xml_node cond = st.append_child(q("cond").c_str());
      cond.append_attribute("evt") = "onBegin";
      cond.append_attribute("delay") = "0";
      cond.append_child(q("tn").c_str()).append_attribute("val") = mainId.c_str();
    }
    return c.append_child(q("childTnLst").c_str());
  };

  pugi::xml_node clickList, timeList;
  double timeStart = 0, timeEnd = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    const Entry& e = entries[i];
    if (i == 0 || e.trigger == AnimTrigger::OnClick) {
      clickList = group(s.list, "indefinite", i == 0 && e.trigger != AnimTrigger::OnClick);
      timeStart = timeEnd = 0;
      timeList = group(clickList, "0", false);
    } else if (e.trigger == AnimTrigger::AfterPrevious) {
      timeStart = timeEnd;
      timeList = group(clickList, MsText(timeStart), false);
    }
    timeList.append_move(e.par);
    timeEnd = std::max(timeEnd, timeStart + CondDelay(e.cTn) + Length(e.cTn) * Repeat(e.cTn));
    const char* type = NodeTypeOf(e.trigger);
    if (std::string_view(e.cTn.attribute("nodeType").value()) != type) SetAttr(e.cTn, "nodeType", type);
  }
  for (pugi::xml_node o : old) s.list.remove_child(o);
}

// ---- Builds (p:bldLst): which shapes build as a whole or by paragraph.

struct GroupRef {
  uint32_t spid;
  std::string grp;
  bool operator<(const GroupRef& o) const { return spid != o.spid ? spid < o.spid : grp < o.grp; }
};

std::set<GroupRef> EffectGroups(pugi::xml_node timing) {
  std::set<GroupRef> out;
  timing.find_node([&](pugi::xml_node n) {
    if (IsP(n, "cTn") && n.attribute("presetClass") && n.attribute("grpId")) {
      pugi::xml_node sp = n.find_node([](pugi::xml_node x) { return IsP(x, "spTgt"); });
      if (sp) out.insert({static_cast<uint32_t>(Number(sp.attribute("spid").value(), 0)), n.attribute("grpId").value()});
    }
    return false;
  });
  return out;
}

void DropBuilds(pugi::xml_node root, const std::set<GroupRef>& gone) {
  pugi::xml_node timing = PChild(root, "timing");
  pugi::xml_node bld = PChild(timing, "bldLst");
  if (!bld || gone.empty()) return;
  std::set<GroupRef> used = EffectGroups(timing);
  std::vector<pugi::xml_node> drop;
  ForElements(bld, [&](pugi::xml_node b) {
    GroupRef g{static_cast<uint32_t>(Number(b.attribute("spid").value(), 0)), b.attribute("grpId").value()};
    if (gone.count(g) && !used.count(g)) drop.push_back(b);
  });
  for (pugi::xml_node b : drop) bld.remove_child(b);
  bool any = false;
  ForElements(bld, [&](pugi::xml_node) { any = true; });
  if (!any) timing.remove_child(bld);
}

bool HasText(pugi::xml_node shape) {
  pugi::xml_node body = PChild(shape, "txBody");
  return body && body.find_node([](pugi::xml_node n) { return Is(n, kA, "t") && *n.text().get(); });
}

// ---- Calls

Result Check(const Editor::Impl& m, size_t slide, std::string& part) {
  part = m.SlidePart(slide);
  if (part.empty()) return Result::Fail(T(u"That slide isn't in the presentation."));
  if (!m.Root(part)) return Result::Fail(T(u"The slide can't be read."));
  return {};
}

Result CheckEffects(const std::vector<size_t>& effects, size_t count) {
  if (effects.empty()) return Result::Fail(T(u"No animations are selected."));
  std::set<size_t> seen;
  for (size_t e : effects) {
    if (e >= count) return Result::Fail(T(u"That animation isn't on the slide."));
    if (!seen.insert(e).second) return Result::Fail(T(u"An animation is selected twice."));
  }
  return {};
}

// Reads the main sequence for an edit: the slide's own copy to change.
Result EditSeq(Editor::Impl& m, const std::string& part, pugi::xml_node& root, MainSeq& s) {
  MainSeq look = ReadMainSeq(m.Root(part));
  if (!look.ok) return Result::Fail(kOdd);
  root = m.Edit(part);
  s = ReadMainSeq(root);
  if (!s.ok) return Result::Fail(kOdd);
  return {};
}

// The duration of an editable effect changed: every behavior's time scaled, a set at the end stays
// 1 ms before it.
void ScaleEffect(pugi::xml_node cTn, double from, double to) {
  if (from <= 0 || to <= 0) return;
  double f = to / from;
  ForElements(PChild(cTn, "childTnLst"), [&](pugi::xml_node c) {
    pugi::xml_node bc = PChild(PChild(c, "cBhvr"), "cTn");
    double dur = 0, v;
    bool hasDur = ParseMs(bc.attribute("dur").value(), dur);
    if (hasDur && dur > 1) SetAttr(bc, "dur", MsText(std::max(1.0, std::round(dur * f))));
    ForElements(PChild(bc, "stCondLst"), [&](pugi::xml_node cond) {
      if (!ParseMs(cond.attribute("delay").value(), v) || v <= 0) return;
      double nv = (!hasDur || dur <= 1) && std::llround(v) == std::llround(from) - 1 ? to - 1 : std::round(v * f);
      SetAttr(cond, "delay", MsText(std::max(0.0, nv)));
    });
  });
}

void SetDelay(pugi::xml_node cTn, double ms) {
  pugi::xml_node st = PChild(cTn, "stCondLst");
  std::string p = PrefixFor(cTn, kP, "p");
  auto q = [&](const char* local) { return p.empty() ? std::string(local) : p + ":" + local; };
  if (!st) {
    pugi::xml_node first;
    for (first = cTn.first_child(); first && first.type() != pugi::node_element; first = first.next_sibling()) {
    }
    st = first ? cTn.insert_child_before(q("stCondLst").c_str(), first) : cTn.append_child(q("stCondLst").c_str());
  }
  pugi::xml_node cond = PChild(st, "cond");
  if (!cond) cond = st.append_child(q("cond").c_str());
  SetAttr(cond, "delay", MsText(ms));
}

// The effect's behaviors replaced by `def`'s (option, duration), its preset attributes with them.
bool Regenerate(pugi::xml_node timing, pugi::xml_node cTn, const AnimDef& def, int option, double ms, const Target& t) {
  Ids ids{MaxId(timing) + 1};
  pugi::xml_document doc;
  if (!Fragment(Behaviors(def, option, ms, TargetXml(t.spid, t.first, t.last), ids), doc)) return false;
  pugi::xml_node list = PChild(cTn, "childTnLst");
  if (!list) return false;
  while (list.first_child()) list.remove_child(list.first_child());
  ForElements(doc.document_element(), [&](pugi::xml_node c) { InsertCopy(list, c); });
  SetAttr(cTn, "presetID", std::to_string(PresetIdOf(def, option)));
  SetAttr(cTn, "presetClass", def.presetClass);
  SetAttr(cTn, "presetSubtype", std::to_string(OptOf(def, option).subtype));
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Transitions

const char16_t* TransitionName(TransitionEffect effect) { return DefOf(effect).name; }

std::vector<std::u16string> TransitionOptions(TransitionEffect effect) {
  const TransDef& d = DefOf(effect);
  std::vector<std::u16string> out;
  for (int i = 0; i < d.count; ++i) out.emplace_back(d.opts[i].label);
  return out;
}

double DefaultTransitionMs(TransitionEffect effect) { return DefOf(effect).ms; }

TransitionInfo GetTransition(const Editor& ed, size_t slide) {
  Editor::Impl& m = const_cast<Editor&>(ed).Internals();
  std::string part = m.SlidePart(slide);
  pugi::xml_node root = part.empty() ? pugi::xml_node() : m.Root(part);
  if (!root) return {};
  std::string from;
  if (FindTransition(root).node || !LayoutTransition(m, part, &from)) return ReadTransition(root);
  TransitionInfo info = ReadTransition(m.Root(from));
  info.inherited = true;
  return info;
}

Result SetTransition(Editor& ed, const std::vector<size_t>& slides, const TransitionPatch& patch) {
  Call call(ed, T(u"Transition"));
  Editor::Impl& m = ed.Internals();
  if (Result r = m.CheckSlides(slides, m.SlideList().size()); !r) return call.End(r);
  if (patch.effect == TransitionEffect::Other) return call.End(Result::Fail(T(u"Pick a transition from the gallery.")));
  if (patch.durationMs && !(*patch.durationMs >= 10 && *patch.durationMs <= 59000))
    return call.End(Result::Fail(T(u"The duration must be between 0.01 and 59 seconds.")));
  if (patch.advanceAfterMs && *patch.advanceAfterMs > 86400000)
    return call.End(Result::Fail(T(u"A slide can advance after at most 24 hours.")));
  if (patch.advanceAfterMs && std::isnan(*patch.advanceAfterMs)) return call.End(Result::Fail(T(u"That time isn't a number.")));
  for (size_t i : std::set<size_t>(slides.begin(), slides.end())) {
    if (Result r = SetOneTransition(m, m.SlidePart(i), patch); !r) return call.End(r);
  }
  return call.End({});
}

Result ApplyTransitionToAll(Editor& ed, size_t from) {
  Call call(ed, T(u"Apply To All"));
  Editor::Impl& m = ed.Internals();
  std::vector<Editor::Impl::SlideEntry> list = m.SlideList();
  if (from >= list.size()) return call.End(Result::Fail(T(u"That slide isn't in the presentation.")));
  if (!m.Root(list[from].part)) return call.End(Result::Fail(T(u"The slide can't be read.")));
  // What the slide plays: its own transition, or its layout's.
  std::string srcPart;
  auto source = [&] {
    srcPart = list[from].part;
    pugi::xml_node own = FindTransition(m.Root(srcPart)).node;
    return own ? own : LayoutTransition(m, list[from].part, &srcPart);
  };
  pugi::xml_node node = source();
  const std::string fromXml = node ? Serialize(node) : std::string();
  const bool sound = node && node.find_node([](pugi::xml_node n) { return IsP(n, "sndAc"); });
  for (size_t i = 0; i < list.size(); ++i) {
    if (i == from) continue;
    pugi::xml_node look = m.Root(list[i].part);
    if (!look) continue;
    TransSlot here = FindTransition(look);
    const bool layoutHas = static_cast<bool>(LayoutTransition(m, list[i].part, nullptr));
    // The same already (a sound is copied anyway: its relationship may name another sound here), or
    // none to play and none playing.
    if (!sound && here.node && Serialize(here.node) == fromXml) continue;
    if (!node && !here.node && !layoutHas) continue;
    pugi::xml_node root = m.Edit(list[i].part);
    if (!root) continue;
    node = source();
    here = FindTransition(root);
    pugi::xml_node place = here.node ? here.node : TransitionPlace(root);
    if (node) {
      if (!CopyTransition(m, srcPart, node, list[i].part, root, place))
        return call.End(Result::Fail(T(u"The transition's sound can't be copied to the other slides.")));
    } else if (layoutHas) {
      // None over a layout's transition: an empty one of the slide's own.
      pugi::xml_document doc;
      if (!Fragment(TransitionXml(TransTarget{}, false), doc)) return call.End(Result::Fail(T(u"The transition can't be written.")));
      InsertCopy(root, doc.document_element().first_child(), place);
    }
    if (here.node) root.remove_child(here.node);
  }
  return call.End({});
}

// ---------------------------------------------------------------------------------------------
// Animations

AnimClass ClassOf(Anim anim) {
  const AnimDef* d = AnimDefOf(anim);
  return d ? d->cls : AnimClass::Other;
}

const char16_t* AnimName(Anim anim) {
  const AnimDef* d = AnimDefOf(anim);
  return d ? static_cast<const char16_t*>(d->name) : T(u"Custom");
}

std::vector<std::u16string> AnimOptions(Anim anim) {
  std::vector<std::u16string> out;
  if (const AnimDef* d = AnimDefOf(anim))
    for (int i = 0; i < d->count; ++i) out.emplace_back(d->opts[i].label);
  return out;
}

int DefaultAnimOption(Anim anim) {
  const AnimDef* d = AnimDefOf(anim);
  return d ? d->defaultOption : 0;
}

double DefaultAnimMs(Anim anim) {
  const AnimDef* d = AnimDefOf(anim);
  return d ? d->ms : 0;
}

std::vector<AnimEffect> MainSequence(const Editor& ed, size_t slide, std::u16string* whyNot) {
  if (whyNot) whyNot->clear();
  std::vector<AnimEffect> out;
  Editor::Impl& m = const_cast<Editor&>(ed).Internals();
  std::string part = m.SlidePart(slide);
  pugi::xml_node root = part.empty() ? pugi::xml_node() : m.Root(part);
  if (!root) return out;
  MainSeq s = ReadMainSeq(root);
  if (!s.ok) {
    if (whyNot) *whyNot = kOdd;
    return out;
  }
  for (const Entry& e : s.entries) {
    Classified c = Classify(e.cTn);
    AnimEffect a;
    a.shape = c.target.spid;
    a.paragraph = c.target.first;
    a.anim = c.def ? c.def->anim : Anim::Other;
    a.cls = c.cls;
    a.option = c.option;
    a.trigger = e.trigger;
    a.durationMs = Length(e.cTn);
    double v;
    const char* rc = e.cTn.attribute("repeatCount").value();
    a.repeat = std::string_view(rc) == "indefinite" ? 0 : ParseMs(rc, v) ? v / 1000 : 1;
    a.delayMs = CondDelay(e.cTn);
    a.editable = c.def && c.editable;
    a.presetId = static_cast<int>(Number(e.cTn.attribute("presetID").value(), 0));
    a.presetSubtype = static_cast<int>(Number(e.cTn.attribute("presetSubtype").value(), 0));
    out.push_back(a);
  }
  return out;
}

Result AddAnimation(Editor& ed, size_t slide, uint32_t shape, const AnimSpec& spec, int at, size_t* index) {
  Call call(ed, T(u"Add Animation"));
  Editor::Impl& m = ed.Internals();
  std::string part;
  if (Result r = Check(m, slide, part); !r) return call.End(r);
  const AnimDef* def = AnimDefOf(spec.anim);
  if (!def) return call.End(Result::Fail(T(u"Pick an animation from the gallery.")));
  int option = spec.option < 0 ? def->defaultOption : spec.option;
  if (option >= std::max(def->count, 1)) return call.End(Result::Fail(T(u"That effect option isn't available.")));
  double ms = spec.durationMs ? std::round(*spec.durationMs) : def->ms;
  if (def->ms > 0 && !(ms >= 10 && ms <= 59000)) return call.End(Result::Fail(T(u"The duration must be between 0.01 and 59 seconds.")));
  if (!(spec.delayMs >= 0 && spec.delayMs <= 59000)) return call.End(Result::Fail(T(u"The delay must be between 0 and 59 seconds.")));
  if (def->ms == 0) ms = 0;

  pugi::xml_node look = m.Root(part);
  pugi::xml_node el = Editor::Impl::FindShape(look, shape);
  if (!el) return call.End(Result::Fail(T(u"That shape isn't on the slide.")));
  pugi::xml_node parent = el.parent();
  while (parent && (Is(parent, kMc, "Choice") || Is(parent, kMc, "Fallback"))) parent = parent.parent().parent();
  if (!IsP(parent, "spTree")) return call.End(Result::Fail(T(u"A shape inside a group can't be animated on its own; animate the group.")));
  bool sp = IsP(el, "sp");
  // The paragraph ranges: each first-level paragraph with the deeper ones after it, empty ones left out.
  std::vector<std::pair<int, int>> ranges;
  if (spec.byParagraph) {
    if (!sp || !HasText(el)) return call.End(Result::Fail(T(u"Only a shape with text can be animated by paragraph.")));
    int i = 0;
    ForElements(PChild(el, "txBody"), [&](pugi::xml_node para) {
      if (!Is(para, kA, "p")) return;
      int lvl = static_cast<int>(Number(A(para, "pPr").attribute("lvl").value(), 0));
      bool text = static_cast<bool>(para.find_node([](pugi::xml_node n) { return Is(n, kA, "t") && *n.text().get(); }));
      if (lvl > 0 && !ranges.empty() && ranges.back().second == i - 1) ranges.back().second = i;
      else if (text) ranges.emplace_back(i, i);
      ++i;
    });
  }
  if (ranges.empty()) ranges.emplace_back(-1, -1);

  MainSeq s0 = ReadMainSeq(look);
  if (!s0.ok) return call.End(Result::Fail(kOdd));
  if (at > static_cast<int>(s0.entries.size())) return call.End(Result::Fail(T(u"That place isn't in the animation order.")));
  size_t pos = at < 0 ? s0.entries.size() : static_cast<size_t>(at);

  pugi::xml_node root = m.Edit(part);
  MainSeq s = ReadMainSeq(root);
  if (!EnsureMainSeq(root, s)) return call.End(Result::Fail(kOdd));
  el = Editor::Impl::FindShape(root, shape);

  // Builds: shapes have one per effect group (grpId), pictures and others none (as PowerPoint).
  std::string grp;
  if (sp) {
    int max = -1;
    s.timing.find_node([&](pugi::xml_node n) {
      bool effect = IsP(n, "cTn") && n.attribute("grpId");
      bool build = NamespaceOf(n) == kP && LocalNameOf(n).starts_with("bld") && n.attribute("grpId");
      if (!effect && !build) return false;
      uint32_t spid = 0;
      if (build) {
        spid = static_cast<uint32_t>(Number(n.attribute("spid").value(), 0));
      } else if (pugi::xml_node t = n.find_node([](pugi::xml_node x) { return IsP(x, "spTgt"); })) {
        spid = static_cast<uint32_t>(Number(t.attribute("spid").value(), 0));
      }
      if (spid == shape) max = std::max(max, static_cast<int>(Number(n.attribute("grpId").value(), 0)));
      return false;
    });
    grp = std::to_string(max + 1);
  }

  Ids ids{MaxId(s.timing) + 1};
  std::string x;
  for (size_t k = 0; k < ranges.size(); ++k) {
    AnimTrigger trigger = k == 0 ? spec.trigger : AnimTrigger::OnClick;
    std::string tgt = TargetXml(shape, ranges[k].first, ranges[k].second);
    std::string effectId = ids();
    Cat(x, {"<p:par><p:cTn id=\"", effectId, "\" presetID=\"", std::to_string(PresetIdOf(*def, option)), "\" presetClass=\"",
            def->presetClass, "\" presetSubtype=\"", std::to_string(OptOf(*def, option).subtype), "\" fill=\"hold\"",
            grp.empty() ? "" : " grpId=\"", grp, grp.empty() ? "" : "\"", " nodeType=\"", NodeTypeOf(trigger),
            "\"><p:stCondLst><p:cond delay=\"", MsText(spec.delayMs), "\"/></p:stCondLst><p:childTnLst>",
            Behaviors(*def, option, ms, tgt, ids), "</p:childTnLst></p:cTn></p:par>"});
  }
  pugi::xml_document doc;
  if (!Fragment(x, doc)) return call.End(Result::Fail(T(u"The animation can't be written.")));
  // New effects wait in the sequence's list until Rebuild moves them into their groups.
  std::vector<Entry> entries = s.entries;
  std::vector<Entry> added;
  size_t k = 0;
  ForElements(doc.document_element(), [&](pugi::xml_node c) {
    pugi::xml_node par = InsertCopy(s.list, c);
    added.push_back({par, PChild(par, "cTn"), k == 0 ? spec.trigger : AnimTrigger::OnClick});
    ++k;
  });
  entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(pos), added.begin(), added.end());
  // Rebuild removes the old groups: the new effects aren't in them.
  Rebuild(root, s, entries);

  if (sp) {
    pugi::xml_node bld = PChild(s.timing, "bldLst");
    std::string p = PrefixFor(s.timing, kP, "p");
    auto q = [&](const char* local) { return p.empty() ? std::string(local) : p + ":" + local; };
    if (!bld) {
      pugi::xml_node ext = PChild(s.timing, "extLst");
      bld = ext ? s.timing.insert_child_before(q("bldLst").c_str(), ext) : s.timing.append_child(q("bldLst").c_str());
    }
    pugi::xml_node b = bld.append_child(q("bldP").c_str());
    b.append_attribute("spid") = std::to_string(shape).c_str();
    b.append_attribute("grpId") = grp.c_str();
    if (spec.byParagraph) b.append_attribute("build") = "p";
    if (!HasText(el)) b.append_attribute("animBg") = "1";
  }
  if (index) *index = pos;
  return call.End({});
}

Result ChangeAnimation(Editor& ed, size_t slide, const std::vector<size_t>& effects, Anim anim) {
  Call call(ed, T(u"Change Animation"));
  Editor::Impl& m = ed.Internals();
  std::string part;
  if (Result r = Check(m, slide, part); !r) return call.End(r);
  const AnimDef* def = AnimDefOf(anim);
  if (!def) return call.End(Result::Fail(T(u"Pick an animation from the gallery.")));
  pugi::xml_node root;
  MainSeq s;
  if (Result r = EditSeq(m, part, root, s); !r) return call.End(r);
  if (Result r = CheckEffects(effects, s.entries.size()); !r) return call.End(r);
  for (size_t i : effects) {
    Classified c = Classify(s.entries[i].cTn);
    if (!c.target.simple) return call.End(Result::Fail(T(u"This animation can't be changed in Featherweight.")));
  }
  for (size_t i : effects) {
    Classified c = Classify(s.entries[i].cTn);
    if (!Regenerate(s.timing, s.entries[i].cTn, *def, def->defaultOption, def->ms, c.target))
      return call.End(Result::Fail(T(u"The animation can't be written.")));
  }
  Rebuild(root, s, s.entries);  // time groups after a changed length move
  return call.End({});
}

Result SetAnimationOption(Editor& ed, size_t slide, const std::vector<size_t>& effects, int option) {
  Call call(ed, T(u"Effect Options"));
  Editor::Impl& m = ed.Internals();
  std::string part;
  if (Result r = Check(m, slide, part); !r) return call.End(r);
  pugi::xml_node root;
  MainSeq s;
  if (Result r = EditSeq(m, part, root, s); !r) return call.End(r);
  if (Result r = CheckEffects(effects, s.entries.size()); !r) return call.End(r);
  for (size_t i : effects) {
    Classified c = Classify(s.entries[i].cTn);
    if (!c.def || !c.editable) return call.End(Result::Fail(T(u"This animation's options can't be changed in Featherweight.")));
    if (option < 0 || option >= std::max(c.def->count, 1)) return call.End(Result::Fail(T(u"That effect option isn't available.")));
  }
  for (size_t i : effects) {
    Classified c = Classify(s.entries[i].cTn);
    if (c.option == option) continue;
    double ms = Length(s.entries[i].cTn);
    if (ms <= 0) ms = c.def->ms;
    if (!Regenerate(s.timing, s.entries[i].cTn, *c.def, option, c.def->ms ? ms : 0, c.target))
      return call.End(Result::Fail(T(u"The animation can't be written.")));
  }
  return call.End({});
}

Result SetAnimationTiming(Editor& ed, size_t slide, const std::vector<size_t>& effects, const AnimTimingPatch& patch) {
  Call call(ed, T(u"Animation Timing"));
  Editor::Impl& m = ed.Internals();
  std::string part;
  if (Result r = Check(m, slide, part); !r) return call.End(r);
  if (patch.durationMs && !(*patch.durationMs >= 10 && *patch.durationMs <= 59000))
    return call.End(Result::Fail(T(u"The duration must be between 0.01 and 59 seconds.")));
  if (patch.delayMs && !(*patch.delayMs >= 0 && *patch.delayMs <= 59000))
    return call.End(Result::Fail(T(u"The delay must be between 0 and 59 seconds.")));
  pugi::xml_node root;
  MainSeq s;
  if (Result r = EditSeq(m, part, root, s); !r) return call.End(r);
  if (Result r = CheckEffects(effects, s.entries.size()); !r) return call.End(r);
  if (patch.durationMs) {
    for (size_t i : effects) {
      Classified c = Classify(s.entries[i].cTn);
      if (!c.def || !c.editable) return call.End(Result::Fail(T(u"This animation's duration can't be changed in Featherweight.")));
    }
  }
  for (size_t i : effects) {
    Entry& e = s.entries[i];
    if (patch.trigger) e.trigger = *patch.trigger;
    if (patch.delayMs) SetDelay(e.cTn, std::round(*patch.delayMs));
    if (patch.durationMs) {
      double len = Length(e.cTn);
      if (len > 0) ScaleEffect(e.cTn, len, std::round(*patch.durationMs));
    }
  }
  Rebuild(root, s, s.entries);
  return call.End({});
}

Result MoveAnimations(Editor& ed, size_t slide, const std::vector<size_t>& effects, size_t to) {
  Call call(ed, T(u"Reorder Animation"));
  Editor::Impl& m = ed.Internals();
  std::string part;
  if (Result r = Check(m, slide, part); !r) return call.End(r);
  pugi::xml_node root;
  MainSeq s;
  if (Result r = EditSeq(m, part, root, s); !r) return call.End(r);
  if (Result r = CheckEffects(effects, s.entries.size()); !r) return call.End(r);
  std::vector<size_t> moved(effects);
  std::sort(moved.begin(), moved.end());
  std::vector<Entry> rest, block;
  for (size_t i = 0; i < s.entries.size(); ++i)
    (std::binary_search(moved.begin(), moved.end(), i) ? block : rest).push_back(s.entries[i]);
  if (to > rest.size()) return call.End(Result::Fail(T(u"That place isn't in the animation order.")));
  rest.insert(rest.begin() + static_cast<std::ptrdiff_t>(to), block.begin(), block.end());
  Rebuild(root, s, rest);
  return call.End({});
}

Result RemoveAnimations(Editor& ed, size_t slide, const std::vector<size_t>& effects) {
  Call call(ed, T(u"Remove Animation"));
  Editor::Impl& m = ed.Internals();
  std::string part;
  if (Result r = Check(m, slide, part); !r) return call.End(r);
  pugi::xml_node root;
  MainSeq s;
  if (Result r = EditSeq(m, part, root, s); !r) return call.End(r);
  if (Result r = CheckEffects(effects, s.entries.size()); !r) return call.End(r);
  std::set<size_t> gone(effects.begin(), effects.end());
  std::set<GroupRef> groups;
  std::vector<Entry> rest;
  for (size_t i = 0; i < s.entries.size(); ++i) {
    if (!gone.count(i)) {
      rest.push_back(s.entries[i]);
      continue;
    }
    pugi::xml_node cTn = s.entries[i].cTn;
    if (cTn.attribute("grpId")) {
      Classified c = Classify(cTn);
      groups.insert({c.target.spid, cTn.attribute("grpId").value()});
    }
  }
  pugi::xml_node timing = s.timing;
  Rebuild(root, s, rest);
  if (PChild(root, "timing") == timing) DropBuilds(root, groups);
  return call.End({});
}

Result SetAnimationSequence(Editor& ed, size_t slide, size_t effect, bool byParagraph) {
  const std::vector<AnimEffect> seq = MainSequence(ed, slide);
  if (effect >= seq.size()) return Result::Fail(T(u"That animation isn't on the slide."));
  const AnimEffect a = seq[effect];
  if (!a.editable) return Result::Fail(T(u"This animation's options can't be changed in Featherweight."));
  auto same = [&](size_t k) { return seq[k].shape == a.shape && seq[k].paragraph >= 0 && seq[k].anim == a.anim; };
  size_t first = effect, last = effect;
  if (a.paragraph >= 0) {
    while (first > 0 && same(first - 1)) --first;
    while (last + 1 < seq.size() && same(last + 1)) ++last;
  }
  if ((a.paragraph >= 0) == byParagraph) return {};
  EditGroup group(ed, T(u"Effect Options"));
  AnimSpec s;
  s.anim = a.anim;
  s.option = a.option;
  s.trigger = seq[first].trigger;
  s.delayMs = seq[first].delayMs;
  s.byParagraph = byParagraph;
  if (DefaultAnimMs(a.anim) > 0) s.durationMs = seq[first].durationMs;
  // The new effects in front of the old ones, then the old ones removed; the group is cancelled
  // (nothing changed) when either is refused.
  if (Result r = AddAnimation(ed, slide, a.shape, s, static_cast<int>(first)); !r) return r;
  const size_t added = MainSequence(ed, slide).size() - seq.size();
  std::vector<size_t> old;
  for (size_t k = first; k <= last; ++k) old.push_back(k + added);
  Result r = RemoveAnimations(ed, slide, old);
  group.done = static_cast<bool>(r);
  return r;
}

}  // namespace fw::slide::edit
