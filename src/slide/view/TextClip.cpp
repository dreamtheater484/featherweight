#include "slide/view/TextClip.h"

#include <objbase.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <sstream>

#include "core/Strings.h"
#include "doc/edit/Clip.h"
#include "draw/Xml.h"
#include "slide/Presentation.h"

namespace fw::slide::view {

using namespace draw::xml;
namespace dedit = fw::doc::edit;

namespace {

constexpr std::string_view kMark = "<!--fw-slide-text:";

// ---- Base64, for Featherweight's own copy inside an HTML comment.

constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Encode(std::string_view in) {
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  for (size_t i = 0; i < in.size(); i += 3) {
    uint32_t v = static_cast<uint8_t>(in[i]) << 16;
    if (i + 1 < in.size()) v |= static_cast<uint8_t>(in[i + 1]) << 8;
    if (i + 2 < in.size()) v |= static_cast<uint8_t>(in[i + 2]);
    out += kB64[(v >> 18) & 63];
    out += kB64[(v >> 12) & 63];
    out += i + 1 < in.size() ? kB64[(v >> 6) & 63] : '=';
    out += i + 2 < in.size() ? kB64[v & 63] : '=';
  }
  return out;
}

std::string Decode(std::string_view in) {
  std::string out;
  uint32_t v = 0;
  int bits = 0;
  for (char c : in) {
    const char* p = c ? std::strchr(kB64, c) : nullptr;
    if (!p) {
      if (c == '=') break;
      continue;
    }
    v = (v << 6) | static_cast<uint32_t>(p - kB64);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += static_cast<char>((v >> bits) & 0xFF);
    }
  }
  return out;
}

std::string Xml(pugi::xml_node n) {
  std::ostringstream os;
  n.print(os, "", pugi::format_raw);
  return os.str();
}

uint32_t Rgb(const draw::Color& c) {
  auto v = [](float f) { return static_cast<uint32_t>(std::lround(std::clamp(f, 0.f, 1.f) * 255)); };
  return (v(c.r) << 16) | (v(c.g) << 8) | v(c.b);
}

std::string NewFieldId() {
  GUID g{};
  if (FAILED(CoCreateGuid(&g))) return "{00000000-0000-0000-0000-000000000000}";
  char buf[48];
  std::snprintf(buf, sizeof buf, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g.Data1, g.Data2, g.Data3,
                g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
  return buf;
}

// The selection as the documents' clip: what readers of RTF and HTML see.
dedit::Clip ClipOf(const TextSession& s) {
  dedit::Clip clip;
  const TextPos from = s.SelFrom(), to = s.SelTo();
  const std::vector<Paragraph>& paras = s.Doc().Resolved().paragraphs;
  for (uint32_t p = from.para; p <= to.para && p < paras.size(); ++p) {
    const Paragraph& para = paras[p];
    dedit::ClipPara& cp = clip.paras.emplace_back();
    const uint32_t a = p == from.para ? from.offset : 0, b = p == to.para ? to.offset : UINT32_MAX;
    uint32_t at = 0;
    for (const TextRun& r : para.runs) {
      const std::wstring text = r.kind == TextRun::Kind::Break ? std::wstring(L"\u000B") : r.text;
      const uint32_t len = static_cast<uint32_t>(text.size());
      const uint32_t lo = std::max(a, at), hi = std::min(b, at + len);
      if (lo < hi) {
        dedit::ClipRun run;
        const std::wstring piece = text.substr(lo - at, hi - lo);
        run.text.assign(piece.begin(), piece.end());
        run.bold = r.props.bold;
        run.italic = r.props.italic;
        run.underline = r.props.underline != fw::slide::Underline::None;
        run.strike = r.props.strike != Strike::None;
        run.vertAlign = r.props.baseline > 0   ? fw::doc::VertAlign::Superscript
                        : r.props.baseline < 0 ? fw::doc::VertAlign::Subscript
                                               : fw::doc::VertAlign::Baseline;
        run.halfPoints = static_cast<int32_t>(std::lround(r.props.size * 2));
        if (!r.props.noFill) run.color = Rgb(r.props.color);
        run.font = WideToUtf8(r.props.latin);
        if (r.props.click.action == Hyperlink::Action::Url) run.link = r.props.click.url;
        if (!cp.runs.empty() && cp.runs.back().SameLook(run)) cp.runs.back().text += run.text;
        else cp.runs.push_back(std::move(run));
      }
      at += len;
    }
    const ParagraphProps& pp = para.props;
    cp.align = pp.align == TextAlign::Center ? dedit::ClipAlign::Center
               : pp.align == TextAlign::Right ? dedit::ClipAlign::Right
               : pp.align == TextAlign::Justify || pp.align == TextAlign::JustifyLow ? dedit::ClipAlign::Justify
                                                                                    : dedit::ClipAlign::Left;
    if (a == 0 && !para.runs.empty()) {
      if (pp.bullet.kind == Bullet::Kind::Char) cp.label.assign(pp.bullet.character.begin(), pp.bullet.character.end());
      else if (pp.bullet.kind == Bullet::Kind::AutoNumber) {
        const std::wstring n = std::to_wstring(std::max(para.number, 1)) + L".";
        cp.label.assign(n.begin(), n.end());
      }
    }
    cp.indent = static_cast<int32_t>(std::lround(pp.marginLeft * 20));
    cp.firstLine = static_cast<int32_t>(std::lround(pp.indent * 20));
    if (!pp.spaceBefore.percent) cp.spaceBefore = static_cast<int32_t>(std::lround(pp.spaceBefore.value * 20));
    if (!pp.spaceAfter.percent) cp.spaceAfter = static_cast<int32_t>(std::lround(pp.spaceAfter.value * 20));
  }
  // A selection ending at a paragraph's start ends with that paragraph's mark: no empty one after.
  return clip;
}

// Featherweight's own copy: the paragraphs as DrawingML, each link's address or slide written on it
// (relationship ids mean nothing in another part).
std::string OwnCopy(const TextSession& s) {
  pugi::xml_document doc;
  pugi::xml_node root = doc.append_child("fw-slide-text");
  root.append_attribute("xmlns:a") = std::string(kA).c_str();
  root.append_attribute("xmlns:r") = std::string(kR).c_str();
  s.Doc().CopyParagraphs(s.SelFrom(), s.SelTo(), root);
  // Links: the resolved run with the same relationship id says where each goes.
  std::function<void(pugi::xml_node)> walk = [&](pugi::xml_node n) {
    for (pugi::xml_node c : n.children()) {
      if (c.type() != pugi::node_element) continue;
      const std::string_view k = LocalNameOf(c);
      if (k == "hlinkClick" || k == "hlinkMouseOver") {
        const std::string id(RelId(c, "id"));
        // Find a resolved link with this id in the body.
        for (const Paragraph& p : s.Doc().Resolved().paragraphs)
          for (const TextRun& r : p.runs) {
            pugi::xml_node h = A(A(r.node, "rPr"), k == "hlinkClick" ? "hlinkClick" : "hlinkMouseOver");
            if (!h || RelId(h, "id") != id) continue;
            const Hyperlink& link = k == "hlinkClick" ? r.props.click : r.props.hover;
            if (link.action == Hyperlink::Action::Url) c.append_attribute("fw-url") = link.url.c_str();
            if (link.action == Hyperlink::Action::Slide) c.append_attribute("fw-slide") = link.slide;
          }
        for (pugi::xml_attribute at = c.first_attribute(); at;) {
          pugi::xml_attribute next = at.next_attribute();
          if (LocalNameOf(at) == "id") c.remove_attribute(at);
          at = next;
        }
        continue;
      }
      walk(c);
    }
  };
  walk(root);
  return Xml(root);
}

// The paragraphs to paste: Featherweight's own copy, its links made again in this part.
std::vector<pugi::xml_node> OwnParagraphs(pugi::xml_document& doc, std::string_view html, TextSession& s, TextModel& model,
                                          bool& found) {
  std::vector<pugi::xml_node> out;
  found = false;
  const size_t at = html.find(kMark);
  if (at == std::string_view::npos) return out;
  const size_t end = html.find("-->", at);
  if (end == std::string_view::npos) return out;
  const std::string xml = Decode(html.substr(at + kMark.size(), end - at - kMark.size()));
  if (!doc.load_buffer(xml.data(), xml.size(), pugi::parse_default | pugi::parse_ws_pcdata)) return out;
  pugi::xml_node root = doc.first_child();
  const std::string r = s.Doc().RelPrefix();
  std::function<void(pugi::xml_node)> walk = [&](pugi::xml_node n) {
    for (pugi::xml_node c = n.first_child(); c;) {
      pugi::xml_node next = c.next_sibling();
      if (c.type() == pugi::node_element) {
        const std::string_view k = LocalNameOf(c);
        if (k == "hlinkClick" || k == "hlinkMouseOver") {
          const std::string url = c.attribute("fw-url").value();
          const int slide = c.attribute("fw-slide") ? c.attribute("fw-slide").as_int(-1) : -1;
          c.remove_attribute("fw-url");
          c.remove_attribute("fw-slide");
          const std::string id = !url.empty() ? model.AddLink(s.Target(), Utf8ToWide(url), -1)
                                 : slide >= 0  ? model.AddLink(s.Target(), L"", slide)
                                               : std::string();
          if (id.empty()) n.remove_child(c);
          else c.append_attribute((r + "id").c_str()) = id.c_str();
        } else if (k == "fld") {
          // A field pasted is a new field.
          if (pugi::xml_attribute idAttr = c.attribute("id")) idAttr.set_value(NewFieldId().c_str());
          walk(c);
        } else {
          walk(c);
        }
      }
      c = next;
    }
  };
  walk(root);
  for (pugi::xml_node p = root.first_child(); p; p = p.next_sibling())
    if (p.type() == pugi::node_element && LocalNameOf(p) == "p") out.push_back(p);
  found = true;
  return out;
}

// A clip from elsewhere as paragraphs in the look of where it goes: its emphasis and web links kept.
std::vector<pugi::xml_node> ClipParagraphs(pugi::xml_document& doc, const dedit::Clip& clip, TextSession& s, TextModel& model) {
  std::vector<pugi::xml_node> out;
  pugi::xml_node root = doc.append_child("fw-slide-text");
  root.append_attribute("xmlns:a") = std::string(kA).c_str();
  root.append_attribute("xmlns:r") = std::string(kR).c_str();
  const TextDoc& d = s.Doc();
  const TextPos at = s.SelFrom();
  pugi::xml_node here = d.PropsAt({at.para, at.offset + (s.HasSelection() && at.offset < d.Length(at.para) ? 1u : 0u)});
  pugi::xml_node pPr;
  for (const Paragraph& p : d.Resolved().paragraphs)
    if (&p - d.Resolved().paragraphs.data() == static_cast<ptrdiff_t>(at.para)) pPr = A(p.node, "pPr");
  const std::string r = s.Doc().RelPrefix();
  for (const dedit::ClipPara& cp : clip.paras) {
    pugi::xml_node p = root.append_child("a:p");
    if (pPr) p.append_copy(pPr).set_name("a:pPr");
    for (const dedit::ClipRun& run : cp.runs) {
      // Line breaks inside a run come out as a:br.
      std::wstring text(run.text.begin(), run.text.end());
      size_t i = 0;
      while (i <= text.size()) {
        size_t j = text.find(L'\u000B', i);
        if (j == std::wstring::npos) j = text.size();
        auto props = [&](pugi::xml_node into) {
          pugi::xml_node rPr = into.append_child("a:rPr");
          if (here)
            for (pugi::xml_attribute a : here.attributes())
              if (std::string_view(a.name()) != "err") rPr.append_attribute(a.name()) = a.value();
          auto set = [&](const char* name, const char* v) {
            if (pugi::xml_attribute a = rPr.attribute(name)) a.set_value(v);
            else rPr.append_attribute(name) = v;
          };
          if (run.bold) set("b", "1");
          if (run.italic) set("i", "1");
          if (run.underline) set("u", "sng");
          if (run.strike) set("strike", "sngStrike");
          if (run.vertAlign == fw::doc::VertAlign::Superscript) set("baseline", "30000");
          if (run.vertAlign == fw::doc::VertAlign::Subscript) set("baseline", "-25000");
          if (here)
            for (pugi::xml_node c : here.children())
              if (LocalNameOf(c) != "hlinkClick" && LocalNameOf(c) != "hlinkMouseOver") rPr.append_copy(c);
          const bool web = run.link.starts_with("http:") || run.link.starts_with("https:") || run.link.starts_with("mailto:") ||
                           run.link.starts_with("ftp:");
          if (web) {
            const std::string id = model.AddLink(s.Target(), Utf8ToWide(run.link), -1);
            if (!id.empty()) {
              pugi::xml_node h = rPr.append_child("a:hlinkClick");
              h.append_attribute((r + "id").c_str()) = id.c_str();
            }
          }
        };
        if (j > i) {
          pugi::xml_node rn = p.append_child("a:r");
          props(rn);
          rn.append_child("a:t").text().set(WideToUtf8(text.substr(i, j - i)).c_str());
        }
        if (j >= text.size()) break;
        props(p.append_child("a:br"));
        i = j + 1;
      }
    }
    out.push_back(p);
  }
  return out;
}

}  // namespace

TextClipboard CopySelection(TextSession& s, TextModel& model) {
  (void)model;
  TextClipboard out;
  if (!s.HasSelection()) return out;
  const dedit::Clip clip = ClipOf(s);
  const std::u16string plain = clip.PlainText();
  out.text.assign(plain.begin(), plain.end());
  out.rtf = dedit::ToRtf(clip);
  std::string html = dedit::ToHtml(clip);
  // Featherweight's own copy goes in the fragment; the header's offsets move with it.
  const std::string own = std::string(kMark) + Encode(OwnCopy(s)) + "-->";
  const size_t frag = html.find("<!--StartFragment-->");
  if (frag != std::string::npos) {
    html.insert(frag + 20, own);
    for (const char* key : {"EndHTML:", "EndFragment:"}) {
      const size_t k = html.find(key);
      if (k == std::string::npos) continue;
      const size_t v = k + std::strlen(key);
      const size_t n = std::strtoull(html.c_str() + v, nullptr, 10) + own.size();
      char buf[16];
      std::snprintf(buf, sizeof buf, "%010zu", n);
      html.replace(v, 10, buf);
    }
  }
  out.html = std::move(html);
  return out;
}

bool Paste(TextSession& s, TextModel& model, const std::string& html, const std::string& rtf, const std::wstring& text, bool textOnly) {
  if (!textOnly) {
    pugi::xml_document doc;
    bool own = false;
    model.BeginGroup(u"Paste");
    std::vector<pugi::xml_node> paras = OwnParagraphs(doc, html, s, model, own);
    if (!own) {
      std::optional<dedit::Clip> clip;
      if (!rtf.empty()) clip = dedit::FromRtf(rtf);
      if ((!clip || clip->Empty()) && !html.empty()) clip = dedit::FromHtml(html);
      if (clip && !clip->Empty()) paras = ClipParagraphs(doc, *clip, s, model);
    }
    bool ok = false;
    if (!paras.empty()) ok = s.ReplaceWithParagraphs(paras, u"Paste");
    model.EndGroup();
    if (ok || !paras.empty()) return ok;
  }
  if (text.empty()) return false;
  return s.ReplaceSelection(text, u"Paste");
}

}  // namespace fw::slide::view
