// Slide numbers, footer and date (Insert > Header & Footer; P2, p2-slides; plan §8.2): the date,
// footer and slide number placeholders made on slides from their layout's, holding a field (the
// slide number, a date that updates) or the text, or removed, as PowerPoint writes them.

#include <cstdio>
#include <cstdlib>
#include <random>

#include "core/Lang.h"
#include "slide/edit/EditInternal.h"
#include "slide/edit/ShapeXml.h"
#include "slide/edit/SlideCopy.h"

namespace fw::slide::edit {

namespace {

enum class Kind { Date, Footer, Number };
constexpr Kind kKinds[] = {Kind::Date, Kind::Footer, Kind::Number};

const char* TypeOf(Kind k) { return k == Kind::Date ? "dt" : k == Kind::Footer ? "ftr" : "sldNum"; }

// The top-level shape of a part whose placeholder has p:ph type `type`; null when none.
pugi::xml_node PlaceholderShape(pugi::xml_node root, std::string_view type) {
  pugi::xml_node tree = P(P(root, "cSld"), "spTree");
  for (pugi::xml_node c = tree.first_child(); c; c = c.next_sibling()) {
    if (c.type() != pugi::node_element) continue;
    pugi::xml_node ph = PlaceholderOf(c);
    if (ph && type == ph.attribute("type").value()) return c;
  }
  return {};
}

uint32_t ShapeId(pugi::xml_node shape) {
  for (pugi::xml_node nv = shape.first_child(); nv; nv = nv.next_sibling())
    if (LocalNameOf(nv).starts_with("nv"))
      if (pugi::xml_node c = P(nv, "cNvPr")) return static_cast<uint32_t>(std::strtoul(c.attribute("id").value(), nullptr, 10));
  return 0;
}

// The first element under `n` (depth first) with an `attr` attribute whose local name is one of `names`.
pugi::xml_node FindWith(pugi::xml_node n, std::initializer_list<std::string_view> names, const char* attr) {
  for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling()) {
    if (c.type() != pugi::node_element) continue;
    for (std::string_view k : names)
      if (LocalNameOf(c) == k && *c.attribute(attr).value()) return c;
    if (pugi::xml_node f = FindWith(c, names, attr)) return f;
  }
  return {};
}

// A field id: the layout placeholder's (PowerPoint copies it), else a new one.
std::string FieldId(pugi::xml_node layoutShape) {
  if (pugi::xml_node f = FindWith(layoutShape, {"fld"}, "id")) return f.attribute("id").value();
  static std::mt19937_64 rng{std::random_device{}()};
  const uint64_t hi = rng(), lo = rng();
  char buf[48];
  std::snprintf(buf, sizeof buf, "{%08X-%04X-4%03X-%04X-%012llX}", static_cast<unsigned>(hi >> 32),
                static_cast<unsigned>((hi >> 16) & 0xFFFF), static_cast<unsigned>(hi & 0xFFF),
                static_cast<unsigned>(0x8000 | ((lo >> 48) & 0x3FFF)), static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFull));
  return buf;
}

// The language of the shape's first run, else en-US.
std::string LangOf(pugi::xml_node shape) {
  if (pugi::xml_node r = FindWith(shape, {"rPr", "endParaRPr"}, "lang")) return r.attribute("lang").value();
  return "en-US";
}

// The placeholder's text: one paragraph holding the field or the text.
void FillBody(pugi::xml_node shape, Kind kind, const HeaderFooter& hf, const std::string& fieldId) {
  pugi::xml_node body = P(shape, "txBody");
  if (!body) return;
  const std::string lang = LangOf(shape);
  const std::string a = PrefixFor(body, kA, "a");
  for (pugi::xml_node p = body.first_child(); p;) {
    pugi::xml_node next = p.next_sibling();
    if (Is(p, kA, "p")) body.remove_child(p);
    p = next;
  }
  pugi::xml_node para = body.append_child((a + ":p").c_str());
  auto rPr = [&](pugi::xml_node run) { run.append_child((a + ":rPr").c_str()).append_attribute("lang") = lang.c_str(); };
  auto field = [&](const std::string& type, const std::wstring& shown) {
    pugi::xml_node f = para.append_child((a + ":fld").c_str());
    f.append_attribute("id") = fieldId.c_str();
    f.append_attribute("type") = type.c_str();
    rPr(f);
    f.append_child((a + ":t").c_str()).text() = Utf8(shown).c_str();
  };
  // The text as runs: only what XML can hold (XmlText); a line break ('\v' as typing makes it, or a
  // line feed) is an a:br between runs, as InsertText writes it (fuzz_pptx finding 4).
  auto run = [&](const std::wstring& text) {
    const std::wstring clean = XmlText(text, true);
    size_t from = 0;
    for (size_t i = 0; i <= clean.size(); ++i) {
      const bool brk = i < clean.size() && (clean[i] == L'\v' || clean[i] == L'\n' || clean[i] == L'\r');
      if (i < clean.size() && !brk) continue;
      if (i > from) {
        pugi::xml_node r = para.append_child((a + ":r").c_str());
        rPr(r);
        r.append_child((a + ":t").c_str()).text() = Utf8(clean.substr(from, i - from)).c_str();
      }
      if (brk) {
        if (clean[i] == L'\r' && i + 1 < clean.size() && clean[i + 1] == L'\n') ++i;
        rPr(para.append_child((a + ":br").c_str()));
      }
      from = i + 1;
    }
  };
  switch (kind) {
    case Kind::Number: field("slidenum", L"‹#›"); break;  // ‹#›: numbered when shown
    case Kind::Footer: run(hf.footerText); break;
    case Kind::Date:
      if (hf.dateFixed) {
        run(hf.dateText);
      } else {
        const std::string type = hf.dateFormat.starts_with("datetime") ? hf.dateFormat : "datetime1";
        field(type, DateFieldText(type, lang));
      }
      break;
  }
  para.append_child((a + ":endParaRPr").c_str()).append_attribute("lang") = lang.c_str();
}

bool TitleLayout(pugi::xml_node layoutRoot) {
  if (std::string_view(layoutRoot.attribute("type").value()) == "title") return true;
  return static_cast<bool>(PlaceholderShape(layoutRoot, "ctrTitle"));
}

}  // namespace

Result Editor::SetHeaderFooter(const std::vector<size_t>& slides, const HeaderFooter& hf) {
  Call call(*this, T(u"Header and Footer"));
  Impl& m = *impl_;
  const std::vector<Impl::SlideEntry> list = m.SlideList();
  std::vector<size_t> targets = slides;
  if (targets.empty())
    for (size_t i = 0; i < list.size(); ++i) targets.push_back(i);
  if (Result r = m.CheckSlides(targets, list.size()); !r) return call.End(r);
  for (size_t i : targets) {
    const std::string& part = list[i].part;
    const pugi::xml_node layoutRoot = m.Root(TargetOfType(m.Rels(part), "slideLayout"));
    const bool title = TitleLayout(layoutRoot);
    for (Kind kind : kKinds) {
      const bool want = (kind == Kind::Date ? hf.date : kind == Kind::Footer ? hf.footer : hf.slideNumber) &&
                        !(title && hf.notOnTitle);
      const pugi::xml_node have = PlaceholderShape(m.Root(part), TypeOf(kind));
      if (!want) {
        if (!have) continue;
        pugi::xml_node root = m.Edit(part);
        pugi::xml_node shape = PlaceholderShape(root, TypeOf(kind));
        const uint32_t id = ShapeId(shape);
        shape.parent().remove_child(shape);
        DropShapeRefs(m, root, {id});  // its effects and build go with it, as deleting it does
        m.Touched(part, id);
        continue;
      }
      // Only where the layout has the placeholder, as in PowerPoint (it has nowhere else to go).
      const pugi::xml_node layoutShape = PlaceholderShape(layoutRoot, TypeOf(kind));
      if (!layoutShape) continue;
      pugi::xml_node root = m.Edit(part);
      if (!root) return call.End(Result::Fail(T(u"That slide can't be changed.")));
      pugi::xml_node shape = PlaceholderShape(root, TypeOf(kind));
      if (!shape) {
        pugi::xml_document doc;
        if (!ParseFragment(EmptyPlaceholderXml(PlaceholderOf(layoutShape), Impl::NextShapeId(root)), doc))
          return call.End(Result::Fail(T(u"The placeholder couldn't be made.")));
        shape = InsertCopy(P(P(root, "cSld"), "spTree"), doc.document_element());
      }
      FillBody(shape, kind, hf, FieldId(layoutShape));
      m.Touched(part, ShapeId(shape));
    }
  }
  return call.End({});
}

}  // namespace fw::slide::edit
