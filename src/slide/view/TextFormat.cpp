#include "slide/view/TextFormat.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/Lang.h"
#include "core/Strings.h"
#include "doc/edit/Case.h"
#include "draw/Xml.h"

namespace fw::slide::view {

using namespace draw::xml;

namespace {

template <class T>
struct Acc {
  bool any = false, mixed = false;
  T value{};
  void Add(const T& v) {
    if (!any) {
      any = true;
      value = v;
    } else if (!(value == v)) {
      mixed = true;
    }
  }
  std::optional<T> Get() const { return any && !mixed ? std::optional<T>(value) : std::nullopt; }
};

std::string Hex(const draw::Color& c) {
  char buf[8];
  auto v = [](float f) { return static_cast<unsigned>(std::lround(std::clamp(f, 0.f, 1.f) * 255)); };
  std::snprintf(buf, sizeof buf, "%02X%02X%02X", v(c.r), v(c.g), v(c.b));
  return buf;
}

void Set(pugi::xml_node n, const char* name, const std::string& value) {
  pugi::xml_attribute a = n.attribute(name);
  if (!a) a = n.append_attribute(name);
  a.set_value(value.c_str());
}

// What a:rPr set for typing says over what's there (b, i, u, strike, sz, baseline, typeface, color).
void Overlay(RunProps& r, pugi::xml_node rPr) {
  if (!rPr) return;
  if (pugi::xml_attribute a = rPr.attribute("b")) r.bold = Bool(a.value(), r.bold);
  if (pugi::xml_attribute a = rPr.attribute("i")) r.italic = Bool(a.value(), r.italic);
  if (pugi::xml_attribute a = rPr.attribute("u")) r.underline = std::string_view(a.value()) == "none" ? fw::slide::Underline::None : fw::slide::Underline::Single;
  if (pugi::xml_attribute a = rPr.attribute("strike"))
    r.strike = std::string_view(a.value()) == "noStrike" ? Strike::None : Strike::Single;
  if (pugi::xml_attribute a = rPr.attribute("sz")) r.size = static_cast<float>(Number(a.value(), r.size * 100) / 100);
  if (pugi::xml_attribute a = rPr.attribute("baseline")) r.baseline = static_cast<float>(Number(a.value(), 0) / 100000);
  if (pugi::xml_node l = A(rPr, "latin")) r.latin = Utf8ToWide(l.attribute("typeface").value());
  if (pugi::xml_node c = A(A(rPr, "solidFill"), "srgbClr")) {
    const unsigned v = static_cast<unsigned>(std::strtoul(c.attribute("val").value(), nullptr, 16));
    r.color = {((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f, 1};
  }
}

}  // namespace

TextLook LookOf(const TextSession& s) {
  TextLook look;
  Acc<bool> bold, italic, underline, strike, link;
  Acc<float> size;
  Acc<std::wstring> font;
  Acc<uint32_t> color;
  auto add = [&](const RunProps& r) {
    bold.Add(r.bold);
    italic.Add(r.italic);
    underline.Add(r.underline != fw::slide::Underline::None);
    strike.Add(r.strike != Strike::None);
    size.Add(r.size);
    font.Add(r.latin);
    color.Add(static_cast<uint32_t>(std::strtoul(Hex(r.color).c_str(), nullptr, 16)));
    link.Add(static_cast<bool>(r.click));
  };
  const std::vector<Paragraph>& paras = s.Doc().Resolved().paragraphs;
  const TextPos from = s.SelFrom(), to = s.SelTo();
  RunProps first;
  if (!s.HasSelection()) {
    first = s.LookAt(s.Caret());
    Overlay(first, s.PendingProps());
    add(first);
  } else {
    bool seen = false;
    for (uint32_t p = from.para; p <= to.para && p < paras.size(); ++p) {
      uint32_t at = 0;
      const uint32_t a = p == from.para ? from.offset : 0, b = p == to.para ? to.offset : UINT32_MAX;
      for (const TextRun& r : paras[p].runs) {
        const uint32_t len = r.kind == TextRun::Kind::Break ? 1 : static_cast<uint32_t>(r.text.size());
        if (at < b && at + len > a && r.kind != TextRun::Kind::Break) {
          if (!seen) first = r.props;
          seen = true;
          add(r.props);
        }
        at += len;
      }
    }
    if (!seen) {
      first = s.LookAt(from);
      add(first);
    }
  }
  look.bold = bold.Get();
  look.italic = italic.Get();
  look.underline = underline.Get();
  look.strike = strike.Get();
  look.size = size.Get();
  look.font = font.Get();
  if (color.Get()) look.color = first.color;
  look.baseline = first.baseline > 0 ? 1 : first.baseline < 0 ? -1 : 0;
  look.link = link.any && link.value;
  if (from.para < paras.size()) {
    const ParagraphProps& pp = paras[from.para].props;
    Acc<int> align;
    for (uint32_t p = from.para; p <= to.para && p < paras.size(); ++p) align.Add(static_cast<int>(paras[p].props.align));
    if (align.Get()) look.align = static_cast<TextAlign>(*align.Get());
    look.lineSpacing = pp.lineSpacing.percent ? pp.lineSpacing.value : 0;
    look.bullets = pp.bullet.kind == Bullet::Kind::Char || pp.bullet.kind == Bullet::Kind::Picture;
    look.numbering = pp.bullet.kind == Bullet::Kind::AutoNumber;
    look.level = pp.level;
  }
  return look;
}

namespace format {

namespace {

bool Runs(TextSession& s, std::u16string_view step, const std::function<void(TextDoc&, pugi::xml_node, const RunProps&)>& edit) {
  return s.FormatRuns(step, edit);
}

constexpr float kSizes[] = {8, 9, 10, 10.5f, 11, 12, 14, 16, 18, 20, 24, 28, 32, 36, 40, 44, 48, 54, 60, 66, 72, 80, 88, 96};

std::string SizeValue(float points) { return std::to_string(std::lround(std::clamp(points, 1.f, 4000.f) * 100)); }

}  // namespace

bool Bold(TextSession& s, bool on) {
  return Runs(s, ::T(u"Bold"), [on](TextDoc&, pugi::xml_node r, const RunProps&) { Set(r, "b", on ? "1" : "0"); });
}

bool Italic(TextSession& s, bool on) {
  return Runs(s, ::T(u"Italic"), [on](TextDoc&, pugi::xml_node r, const RunProps&) { Set(r, "i", on ? "1" : "0"); });
}

bool Underline(TextSession& s, bool on) {
  return Runs(s, ::T(u"Underline"), [on](TextDoc&, pugi::xml_node r, const RunProps&) { Set(r, "u", on ? "sng" : "none"); });
}

bool Strike(TextSession& s, bool on) {
  return Runs(s, ::T(u"Strikethrough"),
              [on](TextDoc&, pugi::xml_node r, const RunProps&) { Set(r, "strike", on ? "sngStrike" : "noStrike"); });
}

bool Baseline(TextSession& s, int which) {
  return Runs(s, which > 0 ? ::T(u"Superscript") : which < 0 ? ::T(u"Subscript") : ::T(u"Baseline"), [which](TextDoc&, pugi::xml_node r, const RunProps&) {
    if (which == 0) r.remove_attribute("baseline");
    else Set(r, "baseline", which > 0 ? "30000" : "-25000");
  });
}

bool Size(TextSession& s, float points) {
  if (!(points >= 1 && points <= 4000)) return false;
  return Runs(s, ::T(u"Font Size"), [points](TextDoc&, pugi::xml_node r, const RunProps&) { Set(r, "sz", SizeValue(points)); });
}

bool Grow(TextSession& s, bool grow, bool step) {
  return Runs(s, grow ? ::T(u"Increase Font Size") : ::T(u"Decrease Font Size"), [grow, step](TextDoc&, pugi::xml_node r, const RunProps& now) {
    float next = now.size;
    if (step) {
      next = std::clamp(now.size + (grow ? 1.f : -1.f), 1.f, 4000.f);
    } else if (grow) {
      next = now.size >= kSizes[std::size(kSizes) - 1] ? now.size + 10 : now.size;
      for (float k : kSizes)
        if (k > now.size + 0.01f) {
          next = k;
          break;
        }
    } else {
      next = now.size > kSizes[std::size(kSizes) - 1] ? std::max(kSizes[std::size(kSizes) - 1], now.size - 10) : now.size;
      for (size_t i = std::size(kSizes); i-- > 0;)
        if (kSizes[i] < now.size - 0.01f) {
          next = kSizes[i];
          break;
        }
      if (now.size <= kSizes[0]) next = std::max(1.f, now.size - 1);
    }
    Set(r, "sz", SizeValue(next));
  });
}

bool Font(TextSession& s, const std::wstring& typeface) {
  if (typeface.empty()) return false;
  const std::string face = WideToUtf8(typeface);
  return Runs(s, ::T(u"Font"), [&face](TextDoc& d, pugi::xml_node r, const RunProps&) {
    // The face's own description (panose, charset) belongs to the old one.
    if (pugi::xml_node old = d.RunChild(r, "latin", false)) r.remove_child(old);
    d.RunChild(r, "latin", true).append_attribute("typeface") = face.c_str();
  });
}

bool Color(TextSession& s, const std::optional<draw::Color>& color) {
  return Runs(s, ::T(u"Font Color"), [&color](TextDoc& d, pugi::xml_node r, const RunProps&) {
    TextDoc::RemoveFills(r);
    if (!color) return;
    pugi::xml_node fill = d.RunChild(r, "solidFill", true);
    fill.append_child((d.Prefix() + "srgbClr").c_str()).append_attribute("val") = Hex(*color).c_str();
  });
}

bool Highlight(TextSession& s, const std::optional<draw::Color>& color) {
  return Runs(s, ::T(u"Highlight"), [&color](TextDoc& d, pugi::xml_node r, const RunProps&) {
    if (pugi::xml_node old = d.RunChild(r, "highlight", false)) r.remove_child(old);
    if (!color) return;
    d.RunChild(r, "highlight", true).append_child((d.Prefix() + "srgbClr").c_str()).append_attribute("val") = Hex(*color).c_str();
  });
}

bool ClearCharacter(TextSession& s) {
  return Runs(s, ::T(u"Clear Formatting"), [](TextDoc&, pugi::xml_node r, const RunProps&) {
    for (pugi::xml_attribute a = r.first_attribute(); a;) {
      pugi::xml_attribute next = a.next_attribute();
      const std::string_view k = a.name();
      if (k != "lang" && k != "altLang" && k != "dirty") r.remove_attribute(a);
      a = next;
    }
    for (pugi::xml_node c = r.first_child(); c;) {
      pugi::xml_node next = c.next_sibling();
      const std::string_view k = LocalNameOf(c);
      if (k != "hlinkClick" && k != "hlinkMouseOver") r.remove_child(c);
      c = next;
    }
  });
}

bool ChangeCase(TextSession& s) {
  TextPos from = s.SelFrom(), to = s.SelTo();
  if (!s.HasSelection()) s.WordAt(s.Caret(), from, to);
  if (from == to) return false;
  const std::wstring all = s.Doc().Text(from, to);
  const fw::doc::edit::TextCase c = fw::doc::edit::NextCase(std::u16string(all.begin(), all.end()));
  const TextPos caret = s.Caret(), anchor = s.Anchor();
  const bool ok = s.Edit(::T(u"Change Case"), [&](TextDoc& d) {
    bool changed = false;
    for (uint32_t p = from.para; p <= to.para; ++p) {
      const uint32_t a = p == from.para ? from.offset : 0, b = p == to.para ? to.offset : d.Length(p);
      if (a >= b) continue;
      const std::wstring t = d.Text(p).substr(a, b - a);
      const std::u16string out = fw::doc::edit::InCase(std::u16string(t.begin(), t.end()), c, a == 0);
      if (out.size() != t.size()) continue;
      changed |= d.ReplaceChars({p, a}, {p, b}, std::wstring(out.begin(), out.end()));
    }
    return changed;
  });
  if (ok) s.Select(anchor, caret);
  return ok;
}

bool ApplyLook(TextSession& s, pugi::xml_node look) {
  if (!look) return false;
  return Runs(s, ::T(u"Paste Formatting"), [look](TextDoc&, pugi::xml_node r, const RunProps&) {
    // The look comes over; the text keeps its links.
    std::vector<pugi::xml_node> keep;
    for (pugi::xml_node c = r.first_child(); c; c = c.next_sibling())
      if (LocalNameOf(c) == "hlinkClick" || LocalNameOf(c) == "hlinkMouseOver") keep.push_back(c);
    pugi::xml_document hold;
    for (pugi::xml_node k : keep) hold.append_copy(k);
    while (r.first_attribute()) r.remove_attribute(r.first_attribute());
    while (r.first_child()) r.remove_child(r.first_child());
    for (pugi::xml_attribute a : look.attributes())
      if (std::string_view(a.name()) != "err") r.append_attribute(a.name()) = a.value();
    for (pugi::xml_node c : look.children())
      if (LocalNameOf(c) != "hlinkClick" && LocalNameOf(c) != "hlinkMouseOver") r.append_copy(c);
    // Links go before rtl and extLst.
    for (pugi::xml_node k : hold.children()) {
      pugi::xml_node before;
      for (pugi::xml_node c = r.first_child(); c && !before; c = c.next_sibling())
        if (LocalNameOf(c) == "rtl" || LocalNameOf(c) == "extLst") before = c;
      if (before) r.insert_copy_before(k, before);
      else r.append_copy(k);
    }
  });
}

void CopyLook(const TextSession& s, pugi::xml_document& into) {
  into.reset();
  pugi::xml_node out = into.append_child("a:rPr");
  TextPos at = s.HasSelection() ? s.SelFrom() : s.Caret();
  if (s.HasSelection() && at.offset < s.Doc().Length(at.para)) ++at.offset;
  pugi::xml_node from = s.PendingProps() ? s.PendingProps() : s.Doc().PropsAt(at);
  if (!from) return;
  for (pugi::xml_attribute a : from.attributes())
    if (std::string_view(a.name()) != "err") out.append_attribute(a.name()) = a.value();
  for (pugi::xml_node c : from.children())
    if (LocalNameOf(c) != "hlinkClick" && LocalNameOf(c) != "hlinkMouseOver") out.append_copy(c);
}

// ---------------------------------------------------------------------------
// Paragraphs

bool Align(TextSession& s, TextAlign align) {
  const char* v = align == TextAlign::Center    ? "ctr"
                  : align == TextAlign::Right   ? "r"
                  : align == TextAlign::Justify ? "just"
                  : align == TextAlign::Distributed ? "dist"
                                                    : "l";
  return s.FormatParagraphs(::T(u"Alignment"), [v](TextDoc&, pugi::xml_node p) { Set(p, "algn", v); });
}

namespace {

void Spacing(TextDoc& d, pugi::xml_node pPr, const char* which, bool percent, float value) {
  pugi::xml_node sp = d.ParaChild(pPr, which, true);
  while (sp.first_child()) sp.remove_child(sp.first_child());
  pugi::xml_node v = sp.append_child((d.Prefix() + (percent ? "spcPct" : "spcPts")).c_str());
  v.append_attribute("val") = std::to_string(std::lround(value * (percent ? 100000 : 100))).c_str();
}

void ClearBulletKinds(TextDoc& d, pugi::xml_node pPr) {
  for (const char* k : {"buNone", "buAutoNum", "buChar", "buBlip", "buFont", "buFontTx"})
    if (pugi::xml_node n = d.ParaChild(pPr, k, false)) pPr.remove_child(n);
}

bool List(TextSession& s, bool on, bool numbers) {
  const uint32_t first = s.SelFrom().para, last = s.SelTo().para;
  return s.Edit(numbers ? ::T(u"Numbering") : ::T(u"Bullets"), [&](TextDoc& d) {
    if (!on) {
      // PowerPoint's way: no bullet, and no hanging indent left for it.
      d.EditParagraphs(first, last, [&](pugi::xml_node pPr) {
        ClearBulletKinds(d, pPr);
        Set(pPr, "marL", "0");
        Set(pPr, "indent", "0");
        d.ParaChild(pPr, "buNone", true);
      });
      return true;
    }
    // What the paragraph's level gives (the master's text styles) where it has a bullet of its
    // own; else one of PowerPoint's, with its hanging indent.
    d.EditParagraphs(first, last, [&](pugi::xml_node pPr) {
      ClearBulletKinds(d, pPr);
      if (std::string_view(pPr.attribute("marL").value()) == "0" && std::string_view(pPr.attribute("indent").value()) == "0") {
        pPr.remove_attribute("marL");
        pPr.remove_attribute("indent");
      }
    });
    for (uint32_t p = first; p <= last && p < d.Resolved().paragraphs.size(); ++p) {
      const ParagraphProps& pp = d.Resolved().paragraphs[p].props;
      const bool has = numbers ? false : pp.bullet.kind == Bullet::Kind::Char || pp.bullet.kind == Bullet::Kind::Picture;
      if (has) continue;
      const bool hanging = pp.indent < 0;
      const int level = pp.level;
      d.EditParagraphs(p, p, [&](pugi::xml_node pPr) {
        if (numbers) {
          d.ParaChild(pPr, "buFont", true).append_attribute("typeface") = "+mj-lt";
          d.ParaChild(pPr, "buAutoNum", true).append_attribute("type") = "arabicPeriod";
        } else {
          d.ParaChild(pPr, "buFont", true).append_attribute("typeface") = "Arial";
          d.ParaChild(pPr, "buChar", true).append_attribute("char") = "\xE2\x80\xA2";  // •
        }
        if (!hanging) {
          const long step = numbers ? 342900 : 285750;
          Set(pPr, "marL", std::to_string(step * (level + 1)));
          Set(pPr, "indent", std::to_string(-step));
        }
      });
    }
    return true;
  });
}

}  // namespace

bool LineSpacing(TextSession& s, float multiple) {
  return s.FormatParagraphs(::T(u"Line Spacing"), [multiple](TextDoc& d, pugi::xml_node p) { Spacing(d, p, "lnSpc", true, multiple); });
}

bool SpaceBefore(TextSession& s, float points) {
  return s.FormatParagraphs(::T(u"Paragraph Spacing"), [points](TextDoc& d, pugi::xml_node p) { Spacing(d, p, "spcBef", false, points); });
}

bool SpaceAfter(TextSession& s, float points) {
  return s.FormatParagraphs(::T(u"Paragraph Spacing"), [points](TextDoc& d, pugi::xml_node p) { Spacing(d, p, "spcAft", false, points); });
}

bool Bullets(TextSession& s, bool on) { return List(s, on, false); }
bool Numbering(TextSession& s, bool on) { return List(s, on, true); }

bool Level(TextSession& s, bool deeper) {
  const uint32_t first = s.SelFrom().para, last = s.SelTo().para;
  return s.Edit(deeper ? ::T(u"Demote") : ::T(u"Promote"), [&](TextDoc& d) {
    bool changed = false;
    for (uint32_t p = first; p <= last && p < d.Resolved().paragraphs.size(); ++p) {
      const int level = d.Resolved().paragraphs[p].props.level;
      const int next = std::clamp(level + (deeper ? 1 : -1), 0, 8);
      if (next == level) continue;
      changed = true;
      d.EditParagraphs(p, p, [&](pugi::xml_node pPr) {
        pPr.remove_attribute("marL");
        pPr.remove_attribute("indent");
        if (next == 0) pPr.remove_attribute("lvl");
        else Set(pPr, "lvl", std::to_string(next));
      });
    }
    return changed;
  });
}

// ---------------------------------------------------------------------------
// Links

bool LinkAt(const TextSession& s, TextPos& from, TextPos& to, pugi::xml_node& hlink) {
  const TextDoc& d = s.Doc();
  const std::vector<Paragraph>& paras = d.Resolved().paragraphs;
  auto linkOf = [](const TextRun& r) { return A(A(r.node, "rPr"), "hlinkClick"); };
  if (s.HasSelection()) {
    const TextPos a = s.SelFrom(), b = s.SelTo();
    for (uint32_t p = a.para; p <= b.para && p < paras.size(); ++p) {
      uint32_t at = 0;
      for (const TextRun& r : paras[p].runs) {
        const uint32_t len = r.kind == TextRun::Kind::Break ? 1 : static_cast<uint32_t>(r.text.size());
        const uint32_t lo = p == a.para ? a.offset : 0, hi = p == b.para ? b.offset : UINT32_MAX;
        if (at < hi && at + len > lo)
          if (pugi::xml_node h = linkOf(r)) {
            hlink = h;
            from = a;
            to = b;
            return true;
          }
        at += len;
      }
    }
    return false;
  }
  const TextPos c = s.Caret();
  if (c.para >= paras.size()) return false;
  // The run around the caret (or just before it) with a link, and its neighbours with the same one.
  const std::vector<TextRun>& runs = paras[c.para].runs;
  std::vector<uint32_t> starts;
  uint32_t at = 0;
  for (const TextRun& r : runs) {
    starts.push_back(at);
    at += r.kind == TextRun::Kind::Break ? 1 : static_cast<uint32_t>(r.text.size());
  }
  starts.push_back(at);
  for (size_t i = 0; i < runs.size(); ++i) {
    if (!(starts[i] <= c.offset && c.offset <= starts[i + 1])) continue;
    pugi::xml_node h = linkOf(runs[i]);
    if (!h) continue;
    const std::string_view id = RelId(h, "id");
    size_t lo = i, hi = i;
    while (lo > 0 && linkOf(runs[lo - 1]) && RelId(linkOf(runs[lo - 1]), "id") == id) --lo;
    while (hi + 1 < runs.size() && linkOf(runs[hi + 1]) && RelId(linkOf(runs[hi + 1]), "id") == id) ++hi;
    hlink = h;
    from = {c.para, starts[lo]};
    to = {c.para, starts[hi + 1]};
    return true;
  }
  return false;
}

bool SetLink(TextSession& s, const std::string& rel, bool slideJump, const std::wstring& tooltip) {
  TextPos from = s.SelFrom(), to = s.SelTo();
  pugi::xml_node existing;
  if (!s.HasSelection()) {
    if (!LinkAt(s, from, to, existing)) return false;
    s.Select(from, to);
  }
  const std::string tip = WideToUtf8(tooltip);
  return s.FormatRuns(rel.empty() ? ::T(u"Remove Hyperlink") : ::T(u"Hyperlink"), [&](TextDoc& d, pugi::xml_node r, const RunProps&) {
    if (pugi::xml_node old = d.RunChild(r, "hlinkClick", false)) r.remove_child(old);
    if (rel.empty()) return;
    pugi::xml_node h = d.RunChild(r, "hlinkClick", true);
    h.append_attribute((d.RelPrefix() + "id").c_str()) = rel.c_str();
    if (slideJump) h.append_attribute("action") = "ppaction://hlinksldjump";
    if (!tip.empty()) h.append_attribute("tooltip") = tip.c_str();
  });
}

}  // namespace format
}  // namespace fw::slide::view
