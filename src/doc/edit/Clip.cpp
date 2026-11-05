#include "doc/edit/Clip.h"

#include <windows.h>  // MultiByteToWideChar, for RTF's code pages

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

#include "doc/model/Utf.h"

namespace fw::doc::edit {
namespace {

// ---------------------------------------------------------------------------
// Small helpers

// Characters in paragraph text that aren't shown (field marks, note and annotation marks,
// drawings, soft hyphens), and page and column breaks, which copying leaves out.
bool Hidden(char16_t c) {
  return c == 0x13 || c == 0x14 || c == 0x15 || c == 0x02 || c == 0x03 || c == 0x04 || c == 0x05 || c == 0xFFFC ||
         c == 0x00AD || c == 0x0C || c == 0x0E;
}

bool Deleted(Node revision) { return revision && (revision.Is(Name("del")) || revision.Is(Name("moveFrom"))); }

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool SameNoCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (Lower(a[i]) != Lower(b[i])) return false;
  return true;
}

std::string LowerAscii(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = Lower(c);
  return out;
}

std::string_view TrimView(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
  return s;
}

void TrimEnd(std::u16string& s) {
  while (!s.empty() && (s.back() == u' ' || s.back() == u'\t' || s.back() == 0xA0)) s.pop_back();
}

std::string Hex6(uint32_t rgb) {
  char b[8];
  std::snprintf(b, sizeof b, "%06X", rgb & 0xFFFFFF);
  return b;
}

ClipAlign AlignOf(Justification jc) {
  switch (jc) {
    case Justification::Center: return ClipAlign::Center;
    case Justification::End: return ClipAlign::Right;
    case Justification::Both:
    case Justification::Distribute:
    case Justification::LowKashida:
    case Justification::MediumKashida:
    case Justification::HighKashida:
    case Justification::ThaiDistribute: return ClipAlign::Justify;
    default: return ClipAlign::Left;
  }
}

ClipRun LookOf(const RunProps& r) {
  ClipRun c;
  c.bold = r.bold;
  c.italic = r.italic;
  c.underline = r.underline != Underline::None;
  c.strike = r.strike || r.doubleStrike;
  c.vertAlign = r.vertAlign;
  c.halfPoints = static_cast<int32_t>(r.size.Value());
  if (!r.color.automatic) c.color = r.color.rgb;
  c.font = std::string(r.fonts.ascii);
  c.charStyle = std::string(r.charStyle);
  return c;
}

// Black and automatic look the same on a page, and pasted text says black all the time.
bool Automatic(const std::optional<uint32_t>& c) { return !c || (*c & 0xFFFFFF) == 0; }
bool Automatic(const Color& c) { return c.automatic || (c.rgb & 0xFFFFFF) == 0; }

// The text as pasting inserts it: no paragraph breaks or other control characters inside a run.
std::u16string Clean(std::u16string_view s) {
  std::u16string out;
  out.reserve(s.size());
  for (char16_t c : s) {
    if (c == u'\r' || c == u'\n') c = u' ';
    if (c < 0x20 && c != u'\t' && c != 0x0B) continue;
    out += c;
  }
  return out;
}

// A link worth keeping from the clipboard: a web address (http, https, mailto, ftp) or, from
// Featherweight's own copies, "#bookmark". Anything else (javascript:, file:, data:) is dropped.
std::string SafeLink(std::string_view href, bool anchors) {
  href = TrimView(href);
  const std::string low = LowerAscii(href.substr(0, 8));
  for (const char* scheme : {"http://", "https://", "mailto:", "ftp://"})
    if (low.rfind(scheme, 0) == 0 && href.size() > std::strlen(scheme)) return std::string(href);
  if (anchors && href.size() > 1 && href[0] == '#') return std::string(href);
  return {};
}
bool WebLink(std::string_view link) { return !link.empty() && link[0] != '#'; }

// ---------------------------------------------------------------------------
// Pasting

// The style a pasted paragraph gets: the clip's when this document has it, a heading style for a
// heading, else the default paragraph style.
std::string ParaStyleFor(const Document& doc, const ClipPara& cp) {
  if (!cp.style.empty())
    if (const Style* s = doc.FindStyle(cp.style); s && s->type == StyleType::Paragraph) return cp.style;
  if (cp.heading > 0) {
    const std::string name = "heading " + std::to_string(cp.heading);
    for (const Style& s : doc.Styles())
      if (s.type == StyleType::Paragraph && SameNoCase(s.name, name)) return std::string(s.id);
  }
  const Style* d = doc.DefaultStyle(StyleType::Paragraph);
  return d ? std::string(d->id) : std::string();
}

std::string CharStyleFor(const Document& doc, const ClipRun& run) {
  if (run.charStyle.empty()) return {};
  const Style* s = doc.FindStyle(run.charStyle);
  return s && s->type == StyleType::Character && !s->isDefault ? run.charStyle : std::string();
}

// The names a pasted run's look is made of: the direct formatting pasting replaces.
bool LookProp(std::string_view name) {
  static const char* const kNames[] = {"rStyle", "b", "bCs", "i", "iCs", "u", "strike", "dstrike",
                                       "vertAlign", "sz", "szCs", "color", "rFonts"};
  for (const char* n : kNames)
    if (name == n) return true;
  return false;
}

bool LooksRight(const RunProps& r, const ClipRun& want, std::string_view charStyle) {
  if (r.bold != want.bold || r.italic != want.italic || (r.underline != Underline::None) != want.underline) return false;
  if ((r.strike || r.doubleStrike) != want.strike || r.vertAlign != want.vertAlign) return false;
  if (want.halfPoints > 0 && r.size.Value() != want.halfPoints) return false;
  if (Automatic(want.color) != Automatic(r.color)) return false;
  if (!Automatic(want.color) && (r.color.rgb & 0xFFFFFF) != (*want.color & 0xFFFFFF)) return false;
  if (!want.font.empty() && !SameNoCase(r.fonts.ascii, want.font)) return false;
  return r.charStyle == charStyle;
}

// Gives the text in [from, to) (one paragraph) the run's look, setting only what differs from
// what its paragraph and character styles give it.
Result FormatRun(Editor& ed, Pos from, Pos to, const ClipRun& want) {
  const Document& doc = ed.Doc();
  const std::string charStyle = CharStyleFor(doc, want);
  auto over = [&](auto&& fn) {
    for (const TextRun& t : doc.Runs(from.para))
      if (t.run && t.end > from.offset && t.start < to.offset) fn(t);
  };
  bool right = true;
  std::vector<std::string> present;
  over([&](const TextRun& t) {
    right = right && LooksRight(doc.ResolvedRun(from.para, t.run), want, charStyle);
    if (Node pr = t.run.Child(Name("rPr")))
      for (Node k : pr.Kids()) {
        std::string name(k.LocalName());
        if (LookProp(name) && std::find(present.begin(), present.end(), name) == present.end()) present.push_back(name);
      }
  });
  Result ok;
  ok.caret = to;
  if (right) return ok;

  // The look the styles give, with the direct formatting the text took from before it gone.
  PropEdit clear;
  for (const std::string& name : present) clear.Remove(name);
  if (!charStyle.empty()) clear.Set("rStyle", {{"val", charStyle}});
  if (!clear.items.empty())
    if (Result r = ed.SetRunProps(from, to, clear); !r) return r;
  const RunProps* base = nullptr;
  over([&](const TextRun& t) {
    if (!base) base = &doc.ResolvedRun(from.para, t.run);
  });
  if (!base) return ok;

  using Attrs = std::vector<std::pair<std::string, std::string>>;
  auto onOff = [](bool on) { return on ? Attrs{} : Attrs{{"val", "0"}}; };
  PropEdit set;
  if (base->bold != want.bold) set.Set("b", onOff(want.bold));
  if (base->italic != want.italic) set.Set("i", onOff(want.italic));
  if ((base->underline != Underline::None) != want.underline) set.Set("u", {{"val", want.underline ? "single" : "none"}});
  if (base->strike != want.strike) set.Set("strike", onOff(want.strike));
  if (base->doubleStrike && !want.strike) set.Set("dstrike", {{"val", "0"}});
  if (base->vertAlign != want.vertAlign)
    set.Set("vertAlign", {{"val", want.vertAlign == VertAlign::Superscript ? "superscript"
                                  : want.vertAlign == VertAlign::Subscript ? "subscript"
                                                                           : "baseline"}});
  if (want.halfPoints > 0 && base->size.Value() != want.halfPoints) {
    set.Set("sz", {{"val", std::to_string(want.halfPoints)}});
    set.Set("szCs", {{"val", std::to_string(want.halfPoints)}});
  }
  if (Automatic(want.color) != Automatic(base->color) ||
      (!Automatic(want.color) && (base->color.rgb & 0xFFFFFF) != (*want.color & 0xFFFFFF)))
    set.Set("color", {{"val", Automatic(want.color) ? "auto" : Hex6(*want.color)}});
  if (!want.font.empty() && !SameNoCase(base->fonts.ascii, want.font))
    set.Set("rFonts", {{"ascii", want.font}, {"hAnsi", want.font}, {"cs", want.font}});
  if (set.items.empty()) return ok;
  if (Result r = ed.SetRunProps(from, to, set); !r) return r;
  return ok;
}

// Gives a pasted paragraph the clip paragraph's style, alignment, indents, spacing and list.
Result FormatPara(Editor& ed, uint32_t para, const ClipPara& cp, bool numbered) {
  const Document& doc = ed.Doc();
  const std::string style = ParaStyleFor(doc, cp);
  auto right = [&](const ParaProps& pp) {
    if (pp.style != style || AlignOf(pp.jc) != cp.align) return false;
    if (cp.indent && pp.indStart.Value() != *cp.indent) return false;
    if (cp.firstLine && pp.firstLine.Value() != *cp.firstLine) return false;
    if (cp.spaceBefore && pp.spaceBefore.Value() != *cp.spaceBefore) return false;
    if (cp.spaceAfter && pp.spaceAfter.Value() != *cp.spaceAfter) return false;
    if (numbered && (pp.numId != cp.numId || pp.ilvl != cp.ilvl)) return false;
    return true;
  };
  Result ok;
  if (right(doc.ResolvedPara(para))) return ok;

  // The style, without the direct formatting the paragraph had.
  PropEdit clear;
  bool styled = false;
  if (Node pPr = doc.ParaNode(para).Child(Name("pPr")))
    for (Node k : pPr.Kids()) {
      std::string_view n = k.LocalName();
      if (n == "pStyle") styled = true;
      if (n == "jc" || n == "ind" || n == "spacing" || n == "numPr") clear.Remove(std::string(n));
    }
  const Style* def = doc.DefaultStyle(StyleType::Paragraph);
  if (def && def->id == style) {
    if (styled) clear.Remove("pStyle");
  } else if (!style.empty()) {
    clear.Set("pStyle", {{"val", style}});
  }
  if (numbered) {
    clear.Set("numPr/ilvl", {{"val", std::to_string(cp.ilvl)}});
    clear.Set("numPr/numId", {{"val", cp.numId}});
  }
  if (!clear.items.empty())
    if (Result r = ed.SetParaProps(para, para, clear); !r) return r;

  const ParaProps& base = doc.ResolvedPara(para);
  PropEdit set;
  if (AlignOf(base.jc) != cp.align) {
    static const char* const kJc[] = {"left", "center", "right", "both"};
    set.Set("jc", {{"val", kJc[static_cast<int>(cp.align)]}});
  }
  std::vector<std::pair<std::string, std::string>> ind, spacing;
  if (cp.indent && base.indStart.Value() != *cp.indent) ind.push_back({"left", std::to_string(*cp.indent)});
  if (cp.firstLine && base.firstLine.Value() != *cp.firstLine) {
    if (*cp.firstLine >= 0) ind.push_back({"firstLine", std::to_string(*cp.firstLine)});
    else ind.push_back({"hanging", std::to_string(-*cp.firstLine)});
  }
  if (cp.spaceBefore && base.spaceBefore.Value() != *cp.spaceBefore) spacing.push_back({"before", std::to_string(*cp.spaceBefore)});
  if (cp.spaceAfter && base.spaceAfter.Value() != *cp.spaceAfter) spacing.push_back({"after", std::to_string(*cp.spaceAfter)});
  if (!ind.empty()) set.Set("ind", ind);
  if (!spacing.empty()) set.Set("spacing", spacing);
  if (set.items.empty()) return ok;
  return ed.SetParaProps(para, para, set);
}

}  // namespace

// ---------------------------------------------------------------------------
// The clip

bool Clip::Empty() const {
  for (const ClipPara& p : paras)
    if (!p.runs.empty() || !p.label.empty()) return false;
  return paras.size() <= 1;
}

std::u16string Clip::PlainText() const {
  std::u16string out;
  for (size_t i = 0; i < paras.size(); ++i) {
    if (i) out += u"\r\n";
    if (!paras[i].label.empty()) out += paras[i].label + u"\t";
    for (const ClipRun& r : paras[i].runs)
      for (char16_t c : r.text) {
        if (c == 0x0B) out += u"\r\n";
        else out += c;
      }
  }
  return out;
}

Clip CopyClip(const Document& doc, Pos from, Pos to, uint64_t source) {
  Clip clip;
  clip.source = source;
  if (to.para < from.para || (to.para == from.para && to.offset < from.offset)) std::swap(from, to);
  const auto& paras = doc.Paragraphs();
  if (from.para >= paras.size()) return clip;
  const uint32_t story = doc.Para(from.para).story;
  for (uint32_t p = from.para; p <= to.para && p < paras.size(); ++p) {
    if (doc.Para(p).story != story) continue;
    ClipPara cp;
    const ParaProps& pp = doc.ResolvedPara(p);
    cp.align = AlignOf(pp.jc);
    cp.heading = pp.outlineLevel >= 0 && pp.outlineLevel < 9 ? pp.outlineLevel + 1 : 0;
    cp.indent = static_cast<int32_t>(pp.indStart.Value());
    cp.firstLine = static_cast<int32_t>(pp.firstLine.Value());
    cp.spaceBefore = static_cast<int32_t>(pp.spaceBefore.Value());
    cp.spaceAfter = static_cast<int32_t>(pp.spaceAfter.Value());
    cp.style = std::string(pp.style);
    cp.numId = std::string(pp.numId);
    cp.ilvl = pp.ilvl;
    const uint32_t a = p == from.para ? from.offset : 0;
    const uint32_t b = p == to.para ? to.offset : UINT32_MAX;
    if (a == 0)
      if (auto label = doc.Label(p)) cp.label = label->text;
    const std::u16string& s = doc.Para(p).text;
    for (const TextRun& run : doc.Runs(p)) {
      if (run.fieldCode || Deleted(run.revision)) continue;
      const RunProps& rp = run.run ? doc.ResolvedRun(p, run.run) : doc.ResolvedMark(p);
      if (rp.vanish) continue;
      ClipRun cr = LookOf(rp);
      if (run.hyperlink) cr.link = doc.HyperlinkTarget(run.hyperlink);
      for (uint32_t i = std::max(run.start, a); i < run.end && i < b && i < s.size(); ++i)
        if (!Hidden(s[i])) cr.text += s[i];
      cr.text = Clean(cr.text);
      if (cr.text.empty()) continue;
      if (!cp.runs.empty() && cp.runs.back().SameLook(cr)) cp.runs.back().text += cr.text;
      else cp.runs.push_back(std::move(cr));
    }
    clip.paras.push_back(std::move(cp));
  }
  return clip;
}

Result PasteClip(Editor& ed, Pos from, Pos to, const Clip& clip, uint64_t target) {
  const Document& doc = ed.Doc();
  const bool same = clip.source != 0 && clip.source == target;
  if (ed.Compare(from, to) > 0) std::swap(from, to);
  ed.BeginGroup(u"Paste");
  bool changed = false;
  auto fail = [&](Result r) {
    ed.EndGroup();
    if (changed) ed.Undo();
    r.ok = false;
    return r;
  };
  Pos caret = from;
  if (!(from == to)) {
    Result r = ed.Delete(from, to);
    if (!r) return fail(r);
    changed = true;
    caret = r.caret;
  }
  // Web links become hyperlinks over the runs that share them, once their text is in. Where the
  // editor can't make one (inside a field or a content control) the text stays plain.
  std::string link;
  Pos linkFrom = caret;
  auto endLink = [&]() {
    if (!link.empty() && linkFrom.para == caret.para && linkFrom.offset < caret.offset)
      ed.SetLink(linkFrom, caret, link);
    link.clear();
  };
  auto insert = [&](const ClipRun& run) {
    const std::u16string text = Clean(run.text);
    Result r;
    r.caret = caret;
    if (text.empty()) return r;
    const std::string want = WebLink(run.link) ? run.link : std::string();
    if (want != link) {
      endLink();
      link = want;
      linkFrom = caret;
    }
    r = ed.InsertText(caret, text);
    if (!r) return r;
    changed = true;
    const Pos start = caret;
    caret = r.caret;
    return FormatRun(ed, start, caret, run);
  };
  for (size_t i = 0; i < clip.paras.size(); ++i) {
    const ClipPara& cp = clip.paras[i];
    const bool numbered = same && !cp.numId.empty() && doc.NumberingLevel(cp.numId, cp.ilvl);
    // A paragraph with its mark: the mark first, so that its text is formatted against the style
    // it ends up with. The text goes at the end of the first half; the second half is the rest.
    Pos next{kNone, 0};
    if (i + 1 < clip.paras.size()) {
      Result r = ed.SplitParagraph(caret);
      if (!r) return fail(r);
      changed = true;
      next = r.caret;
      if (Result f = FormatPara(ed, caret.para, cp, numbered); !f) return fail(f);
    }
    if (!numbered && !cp.label.empty()) {
      ClipRun label = cp.runs.empty() ? ClipRun{} : cp.runs.front();
      label.text = cp.label + u"\t";
      label.link.clear();
      if (Result r = insert(label); !r) return fail(r);
    }
    for (const ClipRun& run : cp.runs)
      if (Result r = insert(run); !r) return fail(r);
    endLink();
    if (next.para != kNone) caret = next;
  }
  ed.EndGroup();
  Result ok;
  ok.caret = caret;
  return ok;
}

// ---------------------------------------------------------------------------
// RTF out

namespace {

void RtfText(std::string& out, std::u16string_view s) {
  for (char16_t c : s) {
    if (c == u'\\' || c == u'{' || c == u'}') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c == u'\t') {
      out += "\\tab ";
    } else if (c == 0x0B) {
      out += "\\line ";
    } else if (c == 0xA0) {
      out += "\\~";
    } else if (c == 0x2011) {
      out += "\\_";
    } else if (c >= 0x20 && c < 0x80) {
      out += static_cast<char>(c);
    } else if (c >= 0x80) {
      out += "\\u" + std::to_string(static_cast<int16_t>(c)) + "?";
    }
  }
}

}  // namespace

std::string ToRtf(const Clip& clip) {
  std::vector<std::string> fonts;
  std::vector<uint32_t> colors;
  auto fontIndex = [&](const std::string& f) {
    auto it = std::find(fonts.begin(), fonts.end(), f);
    if (it != fonts.end()) return static_cast<int>(it - fonts.begin());
    fonts.push_back(f);
    return static_cast<int>(fonts.size() - 1);
  };
  auto colorIndex = [&](uint32_t c) {
    auto it = std::find(colors.begin(), colors.end(), c);
    if (it != colors.end()) return static_cast<int>(it - colors.begin()) + 1;
    colors.push_back(c);
    return static_cast<int>(colors.size());
  };
  std::string body;
  for (size_t i = 0; i < clip.paras.size(); ++i) {
    const ClipPara& p = clip.paras[i];
    body += "\\pard\\plain";
    static const char* const kAlign[] = {"\\ql", "\\qc", "\\qr", "\\qj"};
    body += kAlign[static_cast<int>(p.align)];
    if (p.indent && *p.indent) body += "\\li" + std::to_string(*p.indent);
    if (p.firstLine && *p.firstLine) body += "\\fi" + std::to_string(*p.firstLine);
    if (p.spaceBefore && *p.spaceBefore) body += "\\sb" + std::to_string(*p.spaceBefore);
    if (p.spaceAfter && *p.spaceAfter) body += "\\sa" + std::to_string(*p.spaceAfter);
    if (p.heading > 0) body += "\\outlinelevel" + std::to_string(p.heading - 1);
    body += ' ';
    auto run = [&](const ClipRun& r, std::u16string_view text) {
      if (!r.link.empty()) {
        // A link is Word's HYPERLINK field around the run: HYPERLINK "address" or \l "bookmark".
        const bool anchor = r.link[0] == '#';
        std::string target;
        for (char c : anchor ? r.link.substr(1) : r.link) {
          if (c == '"') {
            target += "%22";
            continue;
          }
          if (c == '\\' || c == '{' || c == '}') target += '\\';
          target += c;
        }
        body += std::string("{\\field{\\*\\fldinst HYPERLINK ") + (anchor ? "\\\\l " : "") + "\"" + target + "\"}{\\fldrslt ";
      }
      body += '{';
      if (!r.font.empty()) body += "\\f" + std::to_string(fontIndex(r.font));
      if (r.halfPoints > 0) body += "\\fs" + std::to_string(r.halfPoints);
      if (r.bold) body += "\\b";
      if (r.italic) body += "\\i";
      if (r.underline) body += "\\ul";
      if (r.strike) body += "\\strike";
      if (r.vertAlign == VertAlign::Superscript) body += "\\super";
      if (r.vertAlign == VertAlign::Subscript) body += "\\sub";
      if (!Automatic(r.color)) body += "\\cf" + std::to_string(colorIndex(*r.color));
      body += ' ';
      RtfText(body, text);
      body += '}';
      if (!r.link.empty()) body += "}}";
    };
    if (!p.label.empty()) {
      body += "{\\listtext ";
      RtfText(body, p.label + u"\t");
      body += '}';
    }
    for (const ClipRun& r : p.runs) run(r, r.text);
    if (i + 1 < clip.paras.size()) body += "\\par\r\n";
  }
  if (fonts.empty()) fonts.push_back("Calibri");
  std::string out = "{\\rtf1\\ansi\\ansicpg1252\\deff0\\uc1{\\fonttbl";
  for (size_t i = 0; i < fonts.size(); ++i) {
    out += "{\\f" + std::to_string(i) + "\\fnil ";
    std::u16string name;
    AppendUtf16(name, fonts[i]);
    RtfText(out, name);
    out += ";}";
  }
  out += "}{\\colortbl;";
  for (uint32_t c : colors)
    out += "\\red" + std::to_string((c >> 16) & 0xFF) + "\\green" + std::to_string((c >> 8) & 0xFF) + "\\blue" +
           std::to_string(c & 0xFF) + ";";
  out += "}\r\n" + body + "}";
  return out;
}

// ---------------------------------------------------------------------------
// RTF in

namespace {

uint32_t CodePageOfCharset(int charset) {
  switch (charset) {
    case 77: return 10000;
    case 128: return 932;
    case 129: return 949;
    case 134: return 936;
    case 136: return 950;
    case 161: return 1253;
    case 162: return 1254;
    case 163: return 1258;
    case 177: return 1255;
    case 178: return 1256;
    case 186: return 1257;
    case 204: return 1251;
    case 222: return 874;
    case 238: return 1250;
    default: return 0;  // the document's \ansicpg
  }
}

// Nesting the readers follow (RTF groups, HTML elements); deeper is pasted without its formatting.
constexpr size_t kMaxDepth = 256;

// Builds a clip a character at a time, for both readers.
struct Builder {
  Clip clip;
  ClipPara cur;
  ClipRun look;
  bool label = false;  // text goes to the list label

  void Text(std::u16string_view t) {
    if (t.empty()) return;
    if (label) {
      cur.label += t;
      return;
    }
    if (cur.runs.empty() || !cur.runs.back().SameLook(look)) {
      ClipRun r = look;
      r.text.clear();
      cur.runs.push_back(std::move(r));
    }
    cur.runs.back().text += t;
  }
  bool HasContent() const { return !cur.runs.empty() || !cur.label.empty(); }
  static void CleanLabel(std::u16string& label) {
    TrimEnd(label);
    if (label == u"\u00B7" || label == u"\uF0B7") label = u"\u2022";
  }
  void EndPara() {
    CleanLabel(cur.label);
    clip.paras.push_back(std::move(cur));
    cur = ClipPara{};
  }
  Clip Finish() {
    CleanLabel(cur.label);
    clip.paras.push_back(std::move(cur));
    for (ClipPara& p : clip.paras)
      p.runs.erase(std::remove_if(p.runs.begin(), p.runs.end(), [](const ClipRun& r) { return r.text.empty(); }),
                   p.runs.end());
    return std::move(clip);
  }
};

class RtfReader {
 public:
  explicit RtfReader(std::string_view s) : s_(s) {}

  std::optional<Clip> Read() {
    if (s_.substr(0, 5) != "{\\rtf") return std::nullopt;
    Plain();
    size_t i = 0;
    while (i < s_.size()) {
      const char c = s_[i];
      // Groups nested deeper than any real document are skipped whole: pasted RTF is untrusted.
      if (overflow_ > 0) {
        if (c == '{') ++overflow_;
        else if (c == '}') --overflow_;
        else if (c == '\\') ++i;  // \{ and \} aren't groups
        ++i;
        continue;
      }
      if (c == '{' && stack_.size() >= kMaxDepth) {
        overflow_ = 1;
        ++i;
        continue;
      }
      if (c == '{') {
        Flush();
        stack_.push_back(st_);
        skip_ = 0;
        ++i;
      } else if (c == '}') {
        Flush();
        EndGroup();
        skip_ = 0;
        ++i;
        if (stack_.empty()) break;
        st_ = stack_.back();
        stack_.pop_back();
        b_.look = st_.look;
        b_.label = st_.dest == Dest::Label;
      } else if (c == '\\') {
        i = Control(i + 1);
      } else if (c == '\r' || c == '\n') {
        ++i;
      } else {
        if (skip_ > 0) --skip_;
        else Byte(static_cast<uint8_t>(c));
        ++i;
      }
    }
    Flush();
    return b_.Finish();
  }

 private:
  enum class Dest : uint8_t { Text, Skip, Fonts, Colors, Styles, Label, FieldCode };
  struct State {
    ClipRun look;
    Dest dest = Dest::Text;
    int font = -1;
    int uc = 1;
    bool hidden = false;
  };
  struct Font {
    std::string name;
    int charset = 0;
  };

  void Plain() {
    st_.look = ClipRun{};
    st_.look.halfPoints = 24;
    st_.font = deff_;
    st_.hidden = false;
    if (auto it = fonts_.find(deff_); it != fonts_.end()) st_.look.font = it->second.name;
    b_.look = st_.look;
  }
  void Pard() {
    ClipPara p;
    p.runs = std::move(b_.cur.runs);
    p.label = std::move(b_.cur.label);
    b_.cur = std::move(p);
  }

  size_t Control(size_t i) {
    if (i >= s_.size()) return i;
    const char d = s_[i];
    if ((d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z')) {
      size_t j = i;
      while (j < s_.size() && ((s_[j] >= 'a' && s_[j] <= 'z') || (s_[j] >= 'A' && s_[j] <= 'Z'))) ++j;
      std::string_view word = s_.substr(i, j - i);
      bool has = false, neg = false;
      long long param = 0;
      if (j < s_.size() && s_[j] == '-') neg = true, ++j;
      while (j < s_.size() && s_[j] >= '0' && s_[j] <= '9') {
        has = true;
        param = std::min<long long>(param * 10 + (s_[j] - '0'), 1LL << 31);
        ++j;
      }
      if (neg) param = -param;
      if (j < s_.size() && s_[j] == ' ') ++j;
      if (word == "bin") return j + static_cast<size_t>(std::max<long long>(0, param));
      Flush();
      Word(word, has, static_cast<int>(param));
      return j;
    }
    if (d == '\'') {
      auto hex = [](char h) {
        const char l = Lower(h);
        return h >= '0' && h <= '9' ? h - '0' : l >= 'a' && l <= 'f' ? l - 'a' + 10 : -1;
      };
      const int hi = i + 1 < s_.size() ? hex(s_[i + 1]) : -1;
      const int lo = i + 2 < s_.size() ? hex(s_[i + 2]) : -1;
      if (hi >= 0 && lo >= 0) {
        if (skip_ > 0) --skip_;
        else Byte(static_cast<uint8_t>(hi * 16 + lo));
      }
      return i + 3;
    }
    Flush();
    switch (d) {
      case '\\':
      case '{':
      case '}': Char(static_cast<char16_t>(d)); break;
      case '~': Char(0xA0); break;
      case '_': Char(0x2011); break;
      case '*': st_.dest = Dest::Skip; break;
      case '\r':
      case '\n': Word("par", false, 0); break;
      default: break;
    }
    return i + 1;
  }

  void Word(std::string_view w, bool has, int n) {
    const bool on = !has || n != 0;
    const bool body = st_.dest == Dest::Text;
    ClipRun& L = st_.look;
    // Destinations.
    static const char* const kSkip[] = {
        "info", "pict", "object", "header", "headerl", "headerr", "headerf", "footer", "footerl", "footerr", "footerf",
        "footnote", "annotation", "xe", "tc", "themedata", "colorschememapping", "latentstyles", "datastore",
        "listtable", "listoverridetable", "rsidtbl", "generator", "shpinst", "nonshppict", "pgdsctbl", "revtbl",
        "xmlnstbl", "filetbl", "fchars", "lchars", "pntxta", "pntxtb", "docvar", "userprops", "defchp", "defpap",
        "background", "protusertbl", "txe", "atnid", "atnauthor", "template", "falt", "panose", "mmathPr"};
    for (const char* k : kSkip)
      if (w == k) {
        st_.dest = Dest::Skip;
        return;
      }
    if (w == "fldinst") {
      st_.dest = Dest::FieldCode;
      fieldCode_.clear();
      return;
    }
    if (w == "fldrslt") {
      // A HYPERLINK field's result is the link's text.
      st_.dest = Dest::Text;
      L.link = LinkOfField(fieldCode_);
      b_.look = L;
      return;
    }
    if (w == "fonttbl") st_.dest = Dest::Fonts;
    else if (w == "colortbl") st_.dest = Dest::Colors, colorHas_ = false, red_ = green_ = blue_ = 0;
    else if (w == "stylesheet") st_.dest = Dest::Styles;
    else if (w == "listtext" || w == "pntext") st_.dest = Dest::Label, b_.label = true;
    // The document.
    else if (w == "ansicpg") codePage_ = static_cast<uint32_t>(std::max(0, n));
    else if (w == "deff") deff_ = n;
    else if (w == "uc") st_.uc = std::max(0, n);
    else if (w == "u") {
      Char(static_cast<char16_t>(n < 0 ? n + 65536 : n));
      skip_ = st_.uc;
    }
    // Fonts, colors and styles.
    else if (w == "f") {
      if (st_.dest == Dest::Fonts) entryFont_ = n;
      st_.font = n;
      if (auto it = fonts_.find(n); it != fonts_.end()) L.font = it->second.name;
    } else if (w == "fcharset") entryCharset_ = n;
    else if (w == "red") red_ = n, colorHas_ = true;
    else if (w == "green") green_ = n, colorHas_ = true;
    else if (w == "blue") blue_ = n, colorHas_ = true;
    else if (w == "s") {
      if (st_.dest == Dest::Styles) entryStyle_ = n;
      else if (auto it = styles_.find(n); body && it != styles_.end()) b_.cur.heading = HeadingOf(it->second);
    }
    // Characters.
    else if (w == "plain") Plain();
    else if (w == "b") L.bold = on;
    else if (w == "i") L.italic = on;
    else if (w == "strike" || w == "striked") L.strike = on;
    else if (w == "ulnone") L.underline = false;
    else if (w.substr(0, 2) == "ul" && w != "ulc") L.underline = on;
    else if (w == "super") L.vertAlign = VertAlign::Superscript;
    else if (w == "sub") L.vertAlign = VertAlign::Subscript;
    else if (w == "nosupersub") L.vertAlign = VertAlign::Baseline;
    else if (w == "fs") L.halfPoints = n > 0 ? n : 24;
    else if (w == "cf") L.color = n >= 0 && n < static_cast<int>(colors_.size()) ? colors_[static_cast<size_t>(n)] : std::nullopt;
    else if (w == "v") st_.hidden = on;
    // Paragraphs (the style sheet's paragraph properties are the styles').
    else if (!body) {
    } else if (w == "pard") Pard();
    else if (w == "ql") b_.cur.align = ClipAlign::Left;
    else if (w == "qc") b_.cur.align = ClipAlign::Center;
    else if (w == "qr") b_.cur.align = ClipAlign::Right;
    else if (w == "qj" || w == "qd") b_.cur.align = ClipAlign::Justify;
    else if (w == "li") b_.cur.indent = n;
    else if (w == "fi") b_.cur.firstLine = n;
    else if (w == "sb") b_.cur.spaceBefore = n;
    else if (w == "sa") b_.cur.spaceAfter = n;
    else if (w == "outlinelevel") b_.cur.heading = n >= 0 && n < 9 ? n + 1 : 0;
    // Breaks and special characters.
    else if (w == "par" || w == "sect") Par();
    else if (w == "row") {
      if (st_.dest == Dest::Text && !b_.cur.runs.empty() && !b_.cur.runs.back().text.empty() &&
          b_.cur.runs.back().text.back() == u'\t')
        b_.cur.runs.back().text.pop_back();
      Par();
    } else if (w == "cell" || w == "nestcell" || w == "tab") Char(u'\t');
    else if (w == "line") Char(0x0B);
    else if (w == "emdash") Char(0x2014);
    else if (w == "endash") Char(0x2013);
    else if (w == "bullet") Char(0x2022);
    else if (w == "lquote") Char(0x2018);
    else if (w == "rquote") Char(0x2019);
    else if (w == "ldblquote") Char(0x201C);
    else if (w == "rdblquote") Char(0x201D);
    else if (w == "emspace") Char(0x2003);
    else if (w == "enspace") Char(0x2002);
    else if (w == "qmspace") Char(0x2005);
    b_.look = L;
  }

  // HYPERLINK "address" or HYPERLINK \l "bookmark", with other switches around them.
  static std::string LinkOfField(std::string_view code) {
    code = TrimView(code);
    if (code.size() < 9 || !SameNoCase(code.substr(0, 9), "HYPERLINK")) return {};
    code.remove_prefix(9);
    bool anchor = false, skipNext = false;
    while (!code.empty()) {
      code = TrimView(code);
      if (code.empty()) break;
      std::string token;
      if (code[0] == '"') {
        size_t end = code.find('"', 1);
        if (end == std::string_view::npos) end = code.size();
        token = std::string(code.substr(1, end - 1));
        code.remove_prefix(std::min(code.size(), end + 1));
      } else {
        size_t end = code.find_first_of(" \t");
        token = std::string(code.substr(0, end));
        code.remove_prefix(end == std::string_view::npos ? code.size() : end);
        if (token == "\\l") {
          anchor = true;
          continue;
        }
        if (!token.empty() && token[0] == '\\') {
          skipNext = true;  // \o "tooltip", \t "frame": their argument isn't the address
          continue;
        }
      }
      if (skipNext) {
        skipNext = false;
        continue;
      }
      return SafeLink(anchor ? "#" + token : token, true);
    }
    return {};
  }

  static int HeadingOf(const std::string& style) {
    std::string s = LowerAscii(style);  // "heading 1", or with aliases: "heading 1,h1"
    if (s.size() >= 9 && s.substr(0, 8) == "heading " && s[8] >= '1' && s[8] <= '9' && (s.size() == 9 || s[9] == ','))
      return s[8] - '0';
    return 0;
  }

  void Par() {
    if (st_.dest != Dest::Text) return;
    b_.EndPara();
    // The next paragraph keeps the paragraph properties until \pard.
    const ClipPara& done = b_.clip.paras.back();
    b_.cur.align = done.align;
    b_.cur.indent = done.indent;
    b_.cur.firstLine = done.firstLine;
    b_.cur.spaceBefore = done.spaceBefore;
    b_.cur.spaceAfter = done.spaceAfter;
    b_.cur.heading = done.heading;
  }

  void Byte(uint8_t c) {
    if (st_.dest != Dest::Skip) bytes_ += static_cast<char>(c);
  }

  void Flush() {
    if (bytes_.empty()) return;
    std::string bytes;
    bytes.swap(bytes_);
    int charset = 0;
    if (auto it = fonts_.find(st_.dest == Dest::Fonts ? entryFont_ : st_.font); it != fonts_.end())
      charset = it->second.charset;
    if (st_.dest == Dest::Fonts) charset = entryCharset_;
    std::u16string text;
    if (charset == 2) {  // symbol fonts: their private-use code points
      for (char c : bytes) text += static_cast<char16_t>(0xF000 + static_cast<uint8_t>(c));
    } else {
      uint32_t cp = CodePageOfCharset(charset);
      if (cp == 0) cp = codePage_ ? codePage_ : 1252;
      const int n = MultiByteToWideChar(cp, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
      if (n > 0) {
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(cp, 0, bytes.data(), static_cast<int>(bytes.size()), w.data(), n);
        text.assign(w.begin(), w.end());
      } else {
        for (char c : bytes) text += static_cast<char16_t>(static_cast<uint8_t>(c));
      }
    }
    Emit(text);
  }

  void Char(char16_t c) {
    Flush();
    Emit(std::u16string_view(&c, 1));
  }

  void Emit(std::u16string_view t) {
    switch (st_.dest) {
      case Dest::Skip: return;
      case Dest::Fonts:
      case Dest::Styles:
        for (char16_t c : t) {
          if (c == u';') EndEntry();
          else AppendUtf8(entry_, std::u16string_view(&c, 1));
        }
        return;
      case Dest::Colors:
        for (char16_t c : t)
          if (c == u';') {
            colors_.push_back(colorHas_ ? std::optional<uint32_t>((static_cast<uint32_t>(red_ & 0xFF) << 16) |
                                                                  (static_cast<uint32_t>(green_ & 0xFF) << 8) |
                                                                  static_cast<uint32_t>(blue_ & 0xFF))
                                        : std::nullopt);
            colorHas_ = false;
            red_ = green_ = blue_ = 0;
          }
        return;
      case Dest::Label:
      case Dest::Text:
        if (st_.hidden) return;
        b_.Text(t);
        return;
      case Dest::FieldCode: AppendUtf8(fieldCode_, t); return;
    }
  }

  void EndEntry() {
    std::string name(TrimView(entry_));
    entry_.clear();
    if (st_.dest == Dest::Fonts && entryFont_ >= 0) {
      fonts_[entryFont_] = {name, entryCharset_};
    } else if (st_.dest == Dest::Styles) {
      styles_[entryStyle_] = name;
      entryStyle_ = 0;
    }
  }

  void EndGroup() {
    if ((st_.dest == Dest::Fonts || st_.dest == Dest::Styles) && !entry_.empty()) EndEntry();
    if (st_.dest == Dest::Fonts && !stack_.empty() && stack_.back().dest != Dest::Fonts) {
      // The font table is done: plain text is in the default font.
      if (auto it = fonts_.find(deff_); it != fonts_.end()) stack_.back().look.font = it->second.name;
    }
    if (st_.dest == Dest::Fonts) entryFont_ = -1, entryCharset_ = 0;
  }

  std::string_view s_;
  Builder b_;
  State st_;
  std::vector<State> stack_;
  std::string bytes_, entry_, fieldCode_;
  int skip_ = 0;
  size_t overflow_ = 0;  // groups open beyond kMaxDepth
  uint32_t codePage_ = 1252;
  int deff_ = 0;
  std::map<int, Font> fonts_;
  std::map<int, std::string> styles_;
  std::vector<std::optional<uint32_t>> colors_;  // \cfN is colors_[N]; entry 0 is automatic
  bool colorHas_ = false;
  int red_ = 0, green_ = 0, blue_ = 0;
  int entryFont_ = -1, entryCharset_ = 0, entryStyle_ = 0;
};

}  // namespace

std::optional<Clip> FromRtf(std::string_view rtf) {
  RtfReader r(rtf);
  return r.Read();
}

// ---------------------------------------------------------------------------
// HTML out

namespace {

void HtmlText(std::string& out, std::u16string_view s) {
  for (size_t i = 0; i < s.size(); ++i) {
    const char16_t c = s[i];
    switch (c) {
      case u'&': out += "&amp;"; break;
      case u'<': out += "&lt;"; break;
      case u'>': out += "&gt;"; break;
      case u'"': out += "&quot;"; break;
      case 0xA0: out += "&nbsp;"; break;
      case 0x0B: out += "<br>"; break;
      default: {
        // A surrogate pair goes as one character.
        const size_t n = c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() ? 2 : 1;
        AppendUtf8(out, s.substr(i, n));
        i += n - 1;
        break;
      }
    }
  }
}

std::string Points(int32_t twips) {
  char b[32];
  std::snprintf(b, sizeof b, "%gpt", twips / 20.0);
  return b;
}

void Attr(std::string& out, const char* name, std::string_view value) {
  out += ' ';
  out += name;
  out += "=\"";
  for (char c : value) {
    if (c == '"') out += "&quot;";
    else if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else out += c;
  }
  out += '"';
}

}  // namespace

std::string ToHtml(const Clip& clip) {
  std::string frag;
  frag += "<div";
  if (clip.source) {
    char b[24];
    std::snprintf(b, sizeof b, "%llx", static_cast<unsigned long long>(clip.source));
    Attr(frag, "data-fw-src", b);
  }
  size_t count = clip.paras.size();
  // The empty paragraph after a final paragraph mark is where pasting continues, not a paragraph.
  const bool mark = count > 1 && clip.paras.back().runs.empty() && clip.paras.back().label.empty();
  if (mark) {
    --count;
    Attr(frag, "data-fw-mark", "1");
  }
  frag += ">";
  for (size_t i = 0; i < count; ++i) {
    const ClipPara& p = clip.paras[i];
    const std::string tag = p.heading >= 1 && p.heading <= 6 ? "h" + std::to_string(p.heading) : "p";
    std::string css = "margin:0;white-space:pre-wrap";
    static const char* const kAlign[] = {"left", "center", "right", "justify"};
    if (p.align != ClipAlign::Left) css += std::string(";text-align:") + kAlign[static_cast<int>(p.align)];
    if (p.spaceBefore && *p.spaceBefore) css += ";margin-top:" + Points(*p.spaceBefore);
    if (p.spaceAfter && *p.spaceAfter) css += ";margin-bottom:" + Points(*p.spaceAfter);
    if (p.indent && *p.indent) css += ";margin-left:" + Points(*p.indent);
    if (p.firstLine && *p.firstLine) css += ";text-indent:" + Points(*p.firstLine);
    frag += "<" + tag;
    Attr(frag, "style", css);
    if (!p.style.empty()) Attr(frag, "data-fw-style", p.style);
    if (!p.numId.empty()) {
      Attr(frag, "data-fw-num", p.numId);
      Attr(frag, "data-fw-ilvl", std::to_string(p.ilvl));
    }
    frag += ">";
    auto span = [&](const ClipRun& r, std::u16string_view text, bool label) {
      std::string rc;
      rc += r.bold ? "font-weight:bold" : "font-weight:normal";
      rc += r.italic ? ";font-style:italic" : ";font-style:normal";
      if (r.underline || r.strike)
        rc += std::string(";text-decoration:") + (r.underline ? "underline" : "") + (r.underline && r.strike ? " " : "") +
              (r.strike ? "line-through" : "");
      if (r.vertAlign == VertAlign::Superscript) rc += ";vertical-align:super";
      if (r.vertAlign == VertAlign::Subscript) rc += ";vertical-align:sub";
      if (r.halfPoints > 0) {
        char b[24];
        std::snprintf(b, sizeof b, ";font-size:%gpt", r.halfPoints / 2.0);
        rc += b;
      }
      if (!Automatic(r.color)) rc += ";color:#" + Hex6(*r.color);
      if (!r.font.empty()) rc += ";font-family:'" + r.font + "'";
      if (!r.link.empty()) {
        frag += "<a";
        Attr(frag, "href", r.link);
        frag += ">";
      }
      frag += "<span";
      Attr(frag, "style", rc);
      if (!r.charStyle.empty()) Attr(frag, "data-fw-cstyle", r.charStyle);
      if (label) Attr(frag, "data-fw-label", "1");
      frag += ">";
      HtmlText(frag, text);
      frag += "</span>";
      if (!r.link.empty()) frag += "</a>";
    };
    if (!p.label.empty()) span(p.runs.empty() ? ClipRun{} : p.runs.front(), p.label + u"\t", true);
    for (const ClipRun& r : p.runs) span(r, r.text, false);
    if (p.runs.empty() && p.label.empty()) frag += "<br>";
    frag += "</" + tag + ">";
  }
  frag += "</div>";

  // CF_HTML: a header with byte offsets of the HTML and of the fragment in it.
  const std::string head = "<html><body>\r\n<!--StartFragment-->";
  const std::string tail = "<!--EndFragment-->\r\n</body></html>";
  const char* const kHeader = "Version:0.9\r\nStartHTML:%010zu\r\nEndHTML:%010zu\r\nStartFragment:%010zu\r\nEndFragment:%010zu\r\n";
  char probe[160];
  const size_t headerSize = static_cast<size_t>(std::snprintf(probe, sizeof probe, kHeader, size_t{0}, size_t{0}, size_t{0}, size_t{0}));
  const size_t startHtml = headerSize, startFrag = startHtml + head.size(), endFrag = startFrag + frag.size(),
               endHtml = endFrag + tail.size();
  char header[160];
  std::snprintf(header, sizeof header, kHeader, startHtml, endHtml, startFrag, endFrag);
  return header + head + frag + tail;
}

// ---------------------------------------------------------------------------
// HTML in

namespace {

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f'; }

std::optional<uint32_t> CssColor(std::string_view v) {
  v = TrimView(v);
  std::string s = LowerAscii(v);
  auto hex = [](char h) { return h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : -1; };
  if (!s.empty() && s[0] == '#') {
    uint32_t c = 0;
    if (s.size() == 4) {
      for (size_t i = 1; i < 4; ++i) {
        int d = hex(s[i]);
        if (d < 0) return std::nullopt;
        c = (c << 8) | static_cast<uint32_t>(d * 17);
      }
      return c;
    }
    if (s.size() == 7) {
      for (size_t i = 1; i < 7; ++i) {
        int d = hex(s[i]);
        if (d < 0) return std::nullopt;
        c = (c << 4) | static_cast<uint32_t>(d);
      }
      return c;
    }
    return std::nullopt;
  }
  if (s.substr(0, 4) == "rgb(" || s.substr(0, 5) == "rgba(") {
    const size_t open = s.find('(');
    int vals[4] = {0, 0, 0, 255};
    int k = 0;
    double alpha = 1;
    std::string_view rest = std::string_view(s).substr(open + 1);
    while (k < 4 && !rest.empty()) {
      size_t end = rest.find_first_of(",)/ ");
      std::string_view num = rest.substr(0, end);
      if (!num.empty()) {
        if (k == 3) alpha = std::atof(std::string(num).c_str());
        else vals[k] = std::clamp(static_cast<int>(std::lround(std::atof(std::string(num).c_str()))), 0, 255);
        ++k;
      }
      if (end == std::string_view::npos) break;
      rest = rest.substr(end + 1);
    }
    if (k < 3 || alpha <= 0) return std::nullopt;
    return (static_cast<uint32_t>(vals[0]) << 16) | (static_cast<uint32_t>(vals[1]) << 8) | static_cast<uint32_t>(vals[2]);
  }
  static const std::pair<const char*, uint32_t> kNamed[] = {
      {"black", 0x000000}, {"white", 0xFFFFFF}, {"red", 0xFF0000},    {"green", 0x008000}, {"blue", 0x0000FF},
      {"gray", 0x808080},  {"grey", 0x808080},  {"yellow", 0xFFFF00}, {"orange", 0xFFA500}, {"purple", 0x800080},
      {"navy", 0x000080},  {"maroon", 0x800000}, {"teal", 0x008080},  {"silver", 0xC0C0C0}, {"olive", 0x808000},
      {"lime", 0x00FF00},  {"aqua", 0x00FFFF},  {"fuchsia", 0xFF00FF}, {"darkblue", 0x00008B}, {"darkred", 0x8B0000},
      {"darkgreen", 0x006400}, {"windowtext", 0x000000}};
  for (const auto& [name, rgb] : kNamed)
    if (s == name) return rgb;
  return std::nullopt;
}

// A CSS length in points; `em` is the current font size in points.
std::optional<double> CssPoints(std::string_view v, double em) {
  v = TrimView(v);
  if (v.empty()) return std::nullopt;
  std::string s = LowerAscii(v);
  char* end = nullptr;
  const double n = std::strtod(s.c_str(), &end);
  if (end == s.c_str()) return std::nullopt;
  std::string_view unit = TrimView(std::string_view(end));
  if (unit.empty()) return n == 0 ? std::optional<double>(0) : std::nullopt;
  if (unit == "pt") return n;
  if (unit == "px") return n * 0.75;
  if (unit == "in") return n * 72;
  if (unit == "cm") return n * 72 / 2.54;
  if (unit == "mm") return n * 72 / 25.4;
  if (unit == "pc") return n * 12;
  if (unit == "em" || unit == "rem") return n * em;
  if (unit == "%") return n * em / 100;
  return std::nullopt;
}

std::string FirstFamily(std::string_view v) {
  while (!v.empty()) {
    size_t comma = v.find(',');
    std::string_view f = TrimView(v.substr(0, comma));
    if (f.size() >= 2 && (f.front() == '"' || f.front() == '\'')) f = f.substr(1, f.size() - 2);
    f = TrimView(f);
    std::string low = LowerAscii(f);
    if (low == "serif") return "Times New Roman";
    if (low == "sans-serif") return "Arial";
    if (low == "monospace") return "Courier New";
    if (low == "system-ui" || low == "ui-sans-serif" || low == "blinkmacsystemfont" || low == "-apple-system") return "Segoe UI";
    if (!f.empty() && f[0] != '-' && low != "inherit" && low != "initial" && low.substr(0, 3) != "var") return std::string(f);
    if (comma == std::string_view::npos) break;
    v = v.substr(comma + 1);
  }
  return {};
}

struct Frame {
  std::string name;
  ClipRun look;
  ClipAlign align = ClipAlign::Left;  // inherited, as CSS's text-align is
  bool pre = false, skip = false, label = false, block = false;
  int listKind = 0;  // 1 for ul, 2 for ol
  int counter = 0;   // ol: the next number; tr: cells so far
};

class HtmlReader {
 public:
  explicit HtmlReader(std::string_view s) : s_(s) {}

  Clip Read() {
    size_t begin = 0, end = s_.size();
    size_t fragBegin = SIZE_MAX, fragEnd = SIZE_MAX;
    if (s_.substr(0, 8) == "Version:") {
      auto number = [&](std::string_view key) -> size_t {
        size_t at = s_.find(key);
        if (at == std::string_view::npos || at > 400) return SIZE_MAX;
        at += key.size();
        size_t n = 0;
        bool any = false;
        while (at < s_.size() && s_[at] >= '0' && s_[at] <= '9') n = n * 10 + static_cast<size_t>(s_[at++] - '0'), any = true;
        return any ? n : SIZE_MAX;
      };
      const size_t sh = number("StartHTML:"), eh = number("EndHTML:");
      fragBegin = number("StartFragment:");
      fragEnd = number("EndFragment:");
      if (sh != SIZE_MAX && sh < s_.size()) begin = sh;
      else begin = s_.find('<');
      if (eh != SIZE_MAX && eh <= s_.size() && eh > begin) end = eh;
      if (begin == std::string_view::npos) begin = 0;
      if (fragBegin > s_.size() || fragEnd > s_.size() || fragEnd < fragBegin) fragBegin = fragEnd = SIZE_MAX;
    }
    // Text is taken only inside the fragment: between its offsets, or its comments.
    const bool marked = s_.find("<!--StartFragment") != std::string_view::npos;
    emit_ = fragBegin == SIZE_MAX && !marked;
    Frame root;
    stack_.push_back(root);
    size_t i = begin;
    while (i < end) {
      if (fragBegin != SIZE_MAX) emit_ = i >= fragBegin && i < fragEnd;
      if (s_[i] == '<') {
        i = Tag(i, end);
      } else {
        size_t j = s_.find('<', i);
        if (j == std::string_view::npos || j > end) j = end;
        if (fragBegin != SIZE_MAX && i < fragBegin && j > fragBegin) j = fragBegin;
        if (fragBegin != SIZE_MAX && i < fragEnd && j > fragEnd) j = fragEnd;
        Text(s_.substr(i, j - i));
        i = j;
      }
    }
    Finish();
    return std::move(b_.clip);
  }

  bool Own() const { return own_; }

 private:
  Frame& Top() { return stack_.back(); }
  const Frame& Top() const { return stack_.back(); }

  size_t Tag(size_t i, size_t end) {
    if (s_.compare(i, 4, "<!--") == 0) {
      size_t close = s_.find("-->", i + 4);
      if (close == std::string_view::npos) close = end;
      std::string_view body = TrimView(s_.substr(i + 4, close - i - 4));
      if (body == "StartFragment") emit_ = true;
      else if (body == "EndFragment") emit_ = false;
      return std::min(end, close + 3);
    }
    size_t close = s_.find('>', i);
    if (close == std::string_view::npos || close >= end) return end;
    std::string_view tag = s_.substr(i + 1, close - i - 1);
    if (tag.empty() || tag[0] == '!' || tag[0] == '?') return close + 1;
    const bool closing = tag[0] == '/';
    if (closing) tag.remove_prefix(1);
    size_t n = 0;
    while (n < tag.size() && !IsSpace(tag[n]) && tag[n] != '/') ++n;
    const std::string name = LowerAscii(tag.substr(0, n));
    if (closing) EndTag(name);
    else StartTag(name, tag.substr(n), !tag.empty() && tag.back() == '/');
    return close + 1;
  }

  static bool IsBlock(std::string_view n) {
    static const char* const kBlocks[] = {"p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "li", "ul", "ol", "table",
                                          "tr", "blockquote", "pre", "section", "article", "header", "footer", "nav",
                                          "aside", "dl", "dt", "dd", "figure", "figcaption", "address", "main",
                                          "center", "hr", "form", "fieldset", "tbody", "thead", "tfoot", "caption"};
    for (const char* b : kBlocks)
      if (n == b) return true;
    return false;
  }
  static bool IsVoid(std::string_view n) {
    static const char* const kVoid[] = {"br", "img", "hr", "meta", "link", "input", "wbr", "col", "area", "base", "param",
                                        "source", "track", "embed"};
    for (const char* v : kVoid)
      if (n == v) return true;
    return false;
  }
  static bool IsSkipped(std::string_view n) {
    static const char* const kSkip[] = {"head", "style", "script", "title", "template", "xml", "noscript", "svg",
                                        "math", "object", "iframe", "select", "textarea", "button"};
    for (const char* v : kSkip)
      if (n == v) return true;
    return false;
  }

  // Attributes: name -> value, lowercased names.
  static std::vector<std::pair<std::string, std::string>> Attrs(std::string_view a) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t i = 0;
    while (i < a.size()) {
      while (i < a.size() && (IsSpace(a[i]) || a[i] == '/')) ++i;
      size_t n = i;
      while (n < a.size() && !IsSpace(a[n]) && a[n] != '=' && a[n] != '/') ++n;
      if (n == i) break;
      std::string name = LowerAscii(a.substr(i, n - i));
      i = n;
      while (i < a.size() && IsSpace(a[i])) ++i;
      std::string value;
      if (i < a.size() && a[i] == '=') {
        ++i;
        while (i < a.size() && IsSpace(a[i])) ++i;
        if (i < a.size() && (a[i] == '"' || a[i] == '\'')) {
          const char q = a[i++];
          size_t e = a.find(q, i);
          if (e == std::string_view::npos) e = a.size();
          value = std::string(a.substr(i, e - i));
          i = e + 1;
        } else {
          size_t e = i;
          while (e < a.size() && !IsSpace(a[e])) ++e;
          value = std::string(a.substr(i, e - i));
          i = e;
        }
      }
      out.push_back({std::move(name), Decode(value)});
    }
    return out;
  }

  void StartTag(const std::string& name, std::string_view attrText, bool selfClosing) {
    if (name == "br") {
      if (emit_ && !Top().skip) Emit(u"\v");
      return;
    }
    // Paragraphs end where a block starts; list items and cells end where the next one starts.
    if (IsBlock(name) && Top().name == "p") EndTag("p");
    if (name == "li") CloseTo("li", {"ul", "ol"});
    if (name == "td" || name == "th") CloseTo(name == "td" ? "td" : "th", {"tr", "table"});
    if (name == "tr") CloseTo("tr", {"table", "tbody", "thead", "tfoot"});
    if (IsVoid(name) || selfClosing) {
      return;
    }
    Frame f = Top();
    f.name = name;
    f.block = IsBlock(name);
    f.listKind = 0;
    f.counter = 0;
    if (IsSkipped(name)) f.skip = true;
    const auto attrs = Attrs(attrText);
    ClipPara props;  // a block's paragraph properties
    props.align = f.align;
    ClipRun& L = f.look;
    if (name == "b" || name == "strong" || name == "th") L.bold = true;
    if (name == "i" || name == "em" || name == "cite" || name == "var" || name == "dfn") L.italic = true;
    if (name == "u" || name == "ins") L.underline = true;
    if (name == "s" || name == "strike" || name == "del") L.strike = true;
    if (name == "sup") L.vertAlign = VertAlign::Superscript;
    if (name == "sub") L.vertAlign = VertAlign::Subscript;
    if (name == "pre" || name == "code" || name == "tt" || name == "kbd" || name == "samp") L.font = "Courier New";
    if (name == "pre") f.pre = true;
    if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
      props.heading = name[1] - '0';
      L.bold = true;
    }
    if (name == "ul") f.listKind = 1;
    if (name == "ol") f.listKind = 2, f.counter = 1;
    if (name == "center") props.align = ClipAlign::Center;
    for (const auto& [an, av] : attrs) {
      if (an == "style") Css(av, f, props);
      else if (an == "align") props.align = AlignNamed(av, props.align);
      else if (an == "color" && name == "font") L.color = CssColor(av);
      else if (an == "face" && name == "font") L.font = FirstFamily(av);
      else if (an == "size" && name == "font") {
        static const int kSizes[] = {0, 15, 20, 24, 27, 36, 48, 72};  // <font size=1..7>, in half-points
        int n = std::atoi(av.c_str());
        if (n >= 1 && n <= 7) L.halfPoints = kSizes[n];
      } else if (an == "data-fw-src") {
        b_.clip.source = std::strtoull(av.c_str(), nullptr, 16);
        own_ = true;
      } else if (an == "data-fw-mark") mark_ = true;
      else if (an == "data-fw-style") props.style = av;
      else if (an == "data-fw-num") props.numId = av;
      else if (an == "data-fw-ilvl") props.ilvl = std::atoi(av.c_str());
      else if (an == "data-fw-cstyle") L.charStyle = av;
      else if (an == "data-fw-label") f.label = true;
      else if (an == "href" && name == "a") L.link = SafeLink(av, own_);
      else if (an == "hidden") f.skip = true;
    }
    f.align = props.align;
    if (f.block) {
      if (emit_ && !f.skip) Break();
      if (!b_.HasContent()) {
        // The paragraph takes the innermost block's properties.
        std::vector<ClipRun> runs = std::move(b_.cur.runs);
        std::u16string label = std::move(b_.cur.label);
        b_.cur = props;
        b_.cur.runs = std::move(runs);
        b_.cur.label = std::move(label);
      }
    }
    if (name == "li" && emit_ && !f.skip) {
      // The list's label, as text before the item.
      for (size_t k = stack_.size(); k-- > 0;) {
        if (stack_[k].listKind == 1) {
          b_.cur.label = u"\u2022";
          break;
        }
        if (stack_[k].listKind == 2) {
          b_.cur.label = ToUtf16(std::to_string(stack_[k].counter++) + ".");
          break;
        }
      }
    }
    if ((name == "td" || name == "th") && emit_ && !f.skip) {
      for (size_t k = stack_.size(); k-- > 0;)
        if (stack_[k].name == "tr") {
          if (stack_[k].counter++ > 0) Emit(u"\t");
          break;
        }
    }
    // Pasted HTML is untrusted: elements nested deeper than any real page add no formatting.
    if (stack_.size() < kMaxDepth) stack_.push_back(std::move(f));
  }

  void CloseTo(const std::string& name, std::initializer_list<const char*> stops) {
    for (size_t k = stack_.size(); k-- > 1;) {
      for (const char* s : stops)
        if (stack_[k].name == s) return;
      if (stack_[k].name == name) {
        EndTag(name);
        return;
      }
    }
  }

  void EndTag(const std::string& name) {
    size_t k = stack_.size();
    while (k-- > 1)
      if (stack_[k].name == name) break;
    if (k == 0) return;
    bool block = false;
    while (stack_.size() > k) {
      block = block || stack_.back().block;
      stack_.pop_back();
    }
    if (block && emit_ && !Top().skip) Break();
    b_.look = Top().look;
    b_.label = Top().label;
  }

  static ClipAlign AlignNamed(std::string_view v, ClipAlign fallback) {
    std::string s = LowerAscii(TrimView(v));
    if (s == "left" || s == "start") return ClipAlign::Left;
    if (s == "center" || s == "middle") return ClipAlign::Center;
    if (s == "right" || s == "end") return ClipAlign::Right;
    if (s == "justify") return ClipAlign::Justify;
    return fallback;
  }

  void Css(std::string_view style, Frame& f, ClipPara& props) {
    ClipRun& L = f.look;
    while (!style.empty()) {
      size_t semi = style.find(';');
      std::string_view decl = style.substr(0, semi);
      style = semi == std::string_view::npos ? std::string_view() : style.substr(semi + 1);
      size_t colon = decl.find(':');
      if (colon == std::string_view::npos) continue;
      const std::string prop = LowerAscii(TrimView(decl.substr(0, colon)));
      std::string_view value = TrimView(decl.substr(colon + 1));
      if (value.size() > 10 && SameNoCase(value.substr(value.size() - 10), "!important"))
        value = TrimView(value.substr(0, value.size() - 10));
      const std::string v = LowerAscii(value);
      const double em = L.halfPoints > 0 ? L.halfPoints / 2.0 : 12.0;
      auto twips = [&](std::string_view x) -> std::optional<int32_t> {
        if (auto pt = CssPoints(x, em)) return static_cast<int32_t>(std::lround(*pt * 20));
        return std::nullopt;
      };
      if (prop == "font-weight") {
        if (v == "bold" || v == "bolder") L.bold = true;
        else if (v == "normal" || v == "lighter") L.bold = false;
        else if (int w = std::atoi(v.c_str()); w > 0) L.bold = w >= 600;
      } else if (prop == "font-style") {
        L.italic = v == "italic" || v == "oblique";
      } else if (prop == "text-decoration" || prop == "text-decoration-line") {
        if (v.find("none") != std::string::npos) L.underline = L.strike = false;
        if (v.find("underline") != std::string::npos) L.underline = true;
        if (v.find("line-through") != std::string::npos) L.strike = true;
      } else if (prop == "vertical-align") {
        if (v == "super") L.vertAlign = VertAlign::Superscript;
        else if (v == "sub") L.vertAlign = VertAlign::Subscript;
        else if (v == "baseline") L.vertAlign = VertAlign::Baseline;
      } else if (prop == "font-size") {
        static const std::pair<const char*, double> kNamed[] = {{"xx-small", 7},  {"x-small", 7.5}, {"small", 10},
                                                                {"medium", 12},   {"large", 13.5},  {"x-large", 18},
                                                                {"xx-large", 24}, {"xxx-large", 36}};
        std::optional<double> pt = CssPoints(v, em);
        for (const auto& [kw, size] : kNamed)
          if (v == kw) pt = size;
        if (pt && *pt > 0 && *pt < 1000) L.halfPoints = static_cast<int32_t>(std::lround(*pt * 2));
      } else if (prop == "color") {
        if (v != "inherit" && v != "initial" && v != "currentcolor") L.color = CssColor(v);
      } else if (prop == "font-family") {
        if (std::string fam = FirstFamily(value); !fam.empty()) L.font = fam;
      } else if (prop == "font") {
        // The shorthand, only for Word's list label spacers ("7.0pt Times New Roman"): ignored.
      } else if (prop == "text-align") {
        props.align = AlignNamed(v, props.align);
      } else if (prop == "margin-left") {
        props.indent = twips(v);
      } else if (prop == "text-indent") {
        props.firstLine = twips(v);
      } else if (prop == "margin-top") {
        props.spaceBefore = twips(v);
      } else if (prop == "margin-bottom") {
        props.spaceAfter = twips(v);
      } else if (prop == "margin") {
        std::vector<std::string> parts;
        size_t a = 0;
        while (a < v.size()) {
          while (a < v.size() && IsSpace(v[a])) ++a;
          size_t e = a;
          while (e < v.size() && !IsSpace(v[e])) ++e;
          if (e > a) parts.push_back(v.substr(a, e - a));
          a = e;
        }
        if (!parts.empty()) {
          props.spaceBefore = twips(parts[0]);
          props.spaceAfter = twips(parts.size() >= 3 ? parts[2] : parts[0]);
          props.indent = twips(parts.size() == 4 ? parts[3] : parts.size() >= 2 ? parts[1] : parts[0]);
        }
      } else if (prop == "white-space") {
        f.pre = v == "pre" || v == "pre-wrap" || v == "break-spaces" || v == "pre-line";
      } else if (prop == "display") {
        if (v == "none") f.skip = true;
      } else if (prop == "mso-list") {
        if (v == "ignore") f.label = true;
      }
    }
  }

  void Text(std::string_view raw) {
    if (!emit_ || Top().skip || raw.empty()) return;
    std::u16string t;
    AppendUtf16(t, Decode(raw));
    if (!Top().pre) {
      std::u16string c;
      for (char16_t ch : t) {
        if (ch == u' ' || ch == u'\t' || ch == u'\r' || ch == u'\n' || ch == u'\f') {
          if (LastIsBreakOrStart() && c.empty()) continue;
          if (!c.empty() && c.back() == u' ') continue;
          c += u' ';
        } else {
          c += ch;
        }
      }
      t = std::move(c);
    } else {
      // Preformatted: line feeds are line breaks.
      std::u16string c;
      for (size_t k = 0; k < t.size(); ++k) {
        if (t[k] == u'\r') continue;
        c += t[k] == u'\n' ? char16_t(0x0B) : t[k];
      }
      t = std::move(c);
    }
    Emit(t);
  }

  bool LastIsBreakOrStart() const {
    if (Top().label) return b_.cur.label.empty() || b_.cur.label.back() == u' ';
    if (b_.cur.runs.empty()) return true;
    const std::u16string& s = b_.cur.runs.back().text;
    return s.empty() || s.back() == u' ' || s.back() == 0x0B || s.back() == u'\t';
  }

  void Emit(std::u16string_view t) {
    b_.look = Top().look;
    b_.label = Top().label;
    b_.Text(t);
  }

  // A block starts or ends: the paragraph so far ends, if it has anything in it.
  void Break() {
    if (!b_.HasContent()) return;
    CleanEnd(b_.cur);
    b_.EndPara();
    lastFromBlock_ = true;
  }

  static void CleanEnd(ClipPara& p) {
    // Trailing spaces and one trailing line break (a block's last <br>) aren't text.
    while (!p.runs.empty()) {
      std::u16string& s = p.runs.back().text;
      while (!s.empty() && s.back() == u' ') s.pop_back();
      if (!s.empty() && s.back() == 0x0B) s.pop_back();
      while (!s.empty() && s.back() == u' ') s.pop_back();
      if (!s.empty()) break;
      p.runs.pop_back();
    }
    // Word's empty paragraphs hold a no-break space.
    if (p.runs.size() == 1 && p.runs[0].text == u"\u00A0") p.runs.clear();
    Builder::CleanLabel(p.label);
  }

  void Finish() {
    if (b_.HasContent()) {
      CleanEnd(b_.cur);
      lastFromBlock_ = false;
    }
    // A last block's end isn't a paragraph mark, unless Featherweight copied one.
    Clip c = b_.Finish();
    if (lastFromBlock_ && !mark_ && c.paras.size() > 1 && c.paras.back().runs.empty() && c.paras.back().label.empty())
      c.paras.pop_back();
    b_.clip = std::move(c);
  }

  static std::string Decode(std::string_view s) {
    if (s.find('&') == std::string_view::npos) return std::string(s);
    static const std::pair<const char*, char32_t> kNamed[] = {
        {"amp", '&'},       {"lt", '<'},        {"gt", '>'},        {"quot", '"'},      {"apos", '\''},
        {"nbsp", 0xA0},     {"copy", 0xA9},     {"reg", 0xAE},      {"trade", 0x2122},  {"hellip", 0x2026},
        {"mdash", 0x2014},  {"ndash", 0x2013},  {"lsquo", 0x2018},  {"rsquo", 0x2019},  {"ldquo", 0x201C},
        {"rdquo", 0x201D},  {"bull", 0x2022},   {"euro", 0x20AC},   {"middot", 0xB7},   {"laquo", 0xAB},
        {"raquo", 0xBB},    {"deg", 0xB0},      {"times", 0xD7},    {"shy", 0xAD},      {"pound", 0xA3},
        {"sect", 0xA7},     {"para", 0xB6},     {"plusmn", 0xB1},   {"frac12", 0xBD},   {"eacute", 0xE9},
        {"egrave", 0xE8},   {"euml", 0xEB},     {"ouml", 0xF6},     {"uuml", 0xFC},     {"auml", 0xE4},
        {"iuml", 0xEF},     {"ccedil", 0xE7},   {"szlig", 0xDF},    {"ensp", 0x2002},   {"emsp", 0x2003},
        {"thinsp", 0x2009}, {"zwj", 0x200D},    {"zwnj", 0x200C}};
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] != '&') {
        out += s[i];
        continue;
      }
      size_t semi = s.find(';', i);
      if (semi == std::string_view::npos || semi - i > 12) {
        out += '&';
        continue;
      }
      std::string_view ent = s.substr(i + 1, semi - i - 1);
      char32_t cp = 0;
      if (!ent.empty() && ent[0] == '#') {
        if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) cp = static_cast<char32_t>(std::strtoul(std::string(ent.substr(2)).c_str(), nullptr, 16));
        else cp = static_cast<char32_t>(std::strtoul(std::string(ent.substr(1)).c_str(), nullptr, 10));
      } else {
        for (const auto& [name, c] : kNamed)
          if (ent == name) cp = c;
      }
      if (cp == 0 || cp > 0x10FFFF) {
        out += '&';
        continue;
      }
      std::u16string u;
      if (cp >= 0x10000) {
        u += static_cast<char16_t>(0xD800 + ((cp - 0x10000) >> 10));
        u += static_cast<char16_t>(0xDC00 + ((cp - 0x10000) & 0x3FF));
      } else {
        u += static_cast<char16_t>(cp);
      }
      AppendUtf8(out, u);
      i = semi;
    }
    return out;
  }

  std::string_view s_;
  Builder b_;
  std::vector<Frame> stack_;
  bool emit_ = false, own_ = false, mark_ = false, lastFromBlock_ = false;
};

}  // namespace

std::optional<Clip> FromHtml(std::string_view html) {
  if (html.find('<') == std::string_view::npos) return std::nullopt;
  HtmlReader r(html);
  return r.Read();
}

bool IsOwnHtml(std::string_view html) { return html.find("data-fw-src=") != std::string_view::npos; }

}  // namespace fw::doc::edit
