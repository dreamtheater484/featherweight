#include "doc/edit/Export.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "doc/edit/Clip.h"
#include "doc/edit/Page.h"
#include "doc/model/Utf.h"

namespace fw::doc::edit {
namespace {

constexpr char16_t kPageBreak = 0x0C, kColumnBreak = 0x0E;

// The fields a file keeps as fields, so that each page shows its own number.
const char* KeptField(const Field& f) {
  const std::u16string_view type = f.Type();
  auto is = [&](std::u16string_view name) {
    if (type.size() != name.size()) return false;
    for (size_t i = 0; i < type.size(); ++i) {
      char16_t c = type[i];
      if (c >= u'a' && c <= u'z') c = static_cast<char16_t>(c - u'a' + u'A');
      if (c != name[i]) return false;
    }
    return true;
  };
  if (is(u"PAGE")) return "PAGE";
  if (is(u"NUMPAGES")) return "NUMPAGES";
  if (is(u"SECTIONPAGES")) return "SECTIONPAGES";
  return nullptr;
}

// A kept field's result, or a page or column break, in a paragraph's text.
struct Mark {
  uint32_t from = 0, to = 0;
  const char* field = nullptr;
  char16_t brk = 0;
};

// The paragraph's runs again, with what copying leaves out: cut where a kept field's result starts
// and ends, and with its page and column breaks.
void Mend(const Document& doc, uint32_t p, const std::vector<const Field*>& fields, ClipPara& cp) {
  const std::u16string& s = doc.Para(p).text;
  std::vector<Mark> marks;
  for (const Field* f : fields) {
    Mark m;
    m.from = f->simple ? f->begin.offset : f->separator.offset;
    m.to = f->end.offset;
    m.field = KeptField(*f);
    if (m.from < m.to) marks.push_back(m);
  }
  const size_t fieldMarks = marks.size();
  for (uint32_t i = 0; i < s.size(); ++i) {
    if (s[i] != kPageBreak && s[i] != kColumnBreak) continue;
    bool inField = false;
    for (size_t k = 0; k < fieldMarks; ++k) inField = inField || (marks[k].from <= i && i < marks[k].to);
    if (!inField) marks.push_back({i, i + 1, nullptr, s[i]});
  }
  if (marks.empty()) return;
  std::sort(marks.begin(), marks.end(), [](const Mark& a, const Mark& b) { return a.from < b.from; });
  std::vector<ClipRun> runs;
  auto add = [&](uint32_t from, uint32_t to, const char* field) {
    if (from >= to) return;
    Clip piece = CopyClip(doc, {p, from}, {p, to});
    if (piece.paras.empty()) return;
    for (ClipRun& r : piece.paras.front().runs) {
      if (field) r.field = field;
      if (!runs.empty() && runs.back().SameLook(r)) runs.back().text += r.text;
      else runs.push_back(std::move(r));
    }
  };
  uint32_t at = 0;
  for (const Mark& m : marks) {
    if (m.from < at) continue;  // fields that overlap: the first one is kept
    add(at, m.from, nullptr);
    if (m.brk) {
      // In the look of the text before it, outside any link or field.
      if (runs.empty() || !runs.back().field.empty() || !runs.back().link.empty()) {
        ClipRun r = runs.empty() ? ClipRun{} : runs.back();
        r.text.clear();
        r.field.clear();
        r.link.clear();
        runs.push_back(std::move(r));
      }
      runs.back().text += m.brk;
    } else {
      add(m.from, m.to, m.field);
    }
    at = m.to;
  }
  add(at, static_cast<uint32_t>(s.size()), nullptr);
  cp.runs = std::move(runs);
}

// A bullet drawn from a symbol font (Symbol's U+F0B7, Wingdings' U+F0A7) as the Unicode character
// it shows, for readers without the font: a file's list label is plain text.
void PlainLabel(std::u16string& label) {
  for (char16_t& c : label) {
    if (c < 0xF020 || c > 0xF0FF) continue;
    switch (c) {
      case 0xF0A7: c = 0x25AA; break;  // a small square
      case 0xF0D8: c = 0x27A2; break;  // an arrowhead
      case 0xF076: c = 0x2756; break;  // four diamonds
      case 0xF0FC: c = 0x2714; break;  // a check mark
      default: c = 0x2022;             // a bullet, Symbol's and the rest
    }
  }
}

// Paragraphs [first, last] of one story as copying gives them, with what a file keeps besides:
// page breaks before paragraphs, page and column breaks, and `fields` of the story that are kept.
Clip StoryClip(const Document& doc, uint32_t first, uint32_t last, const std::vector<Field>& fields) {
  if (first == kNone || last == kNone || last < first) return {};
  Clip clip = CopyClip(doc, {first, 0}, {last, static_cast<uint32_t>(doc.Para(last).text.size())});
  std::unordered_map<uint32_t, std::vector<const Field*>> kept;  // by paragraph: outermost, in one paragraph
  for (const Field& f : fields)
    if (f.depth == 0 && f.complete && KeptField(f) && f.end.para >= first && f.end.para <= last &&
        (f.simple ? f.begin.para : f.separator.para) == f.end.para)
      kept[f.end.para].push_back(&f);
  static const std::vector<const Field*> kNoFields;
  // CopyClip gives each paragraph of the story in [first, last] in order.
  const uint32_t story = doc.Para(first).story;
  size_t k = 0;
  for (uint32_t p = first; p <= last && k < clip.paras.size(); ++p) {
    if (doc.Para(p).story != story) continue;
    ClipPara& cp = clip.paras[k++];
    cp.pageBreakBefore = doc.ResolvedPara(p).pageBreakBefore;
    PlainLabel(cp.label);
    const std::u16string& s = doc.Para(p).text;
    auto it = kept.find(p);
    if (it != kept.end() || s.find(kPageBreak) != std::u16string::npos || s.find(kColumnBreak) != std::u16string::npos)
      Mend(doc, p, it != kept.end() ? it->second : kNoFields, cp);
    // The list label as text, in the look of the text after it: Word reads RTF's {\listtext} only
    // with RTF list tables, which these files don't have, and would leave it out.
    if (!cp.label.empty()) {
      ClipRun label;
      if (!cp.runs.empty()) {
        const ClipRun& look = cp.runs.front();
        label.bold = look.bold, label.italic = look.italic, label.underline = look.underline;
        label.strike = look.strike, label.halfPoints = look.halfPoints, label.color = look.color, label.font = look.font;
      }
      label.text = cp.label + u"\t";
      if (!cp.runs.empty() && cp.runs.front().SameLook(label)) cp.runs.front().text.insert(0, label.text);
      else cp.runs.insert(cp.runs.begin(), std::move(label));
      cp.label.clear();
    }
  }
  return clip;
}

std::string Num(int32_t v) { return std::to_string(v); }

// Margins are distances: a negative top or bottom one (text kept clear of the header) is the same.
std::string Measure(const char* word, decltype(SectionProps::pageWidth) t) { return word + Num(std::abs(t.Value())); }

std::string SectionWords(const SectionProps& sp) {
  std::string out = "\\sectd";
  switch (sp.start) {
    case SectionStart::Continuous: out += "\\sbknone"; break;
    case SectionStart::NextColumn: out += "\\sbkcol"; break;
    case SectionStart::EvenPage: out += "\\sbkeven"; break;
    case SectionStart::OddPage: out += "\\sbkodd"; break;
    case SectionStart::NextPage: break;  // RTF's default
  }
  out += Measure("\\pgwsxn", sp.pageWidth) + Measure("\\pghsxn", sp.pageHeight);
  out += Measure("\\marglsxn", sp.marginStart) + Measure("\\margrsxn", sp.marginEnd);
  out += Measure("\\margtsxn", sp.marginTop) + Measure("\\margbsxn", sp.marginBottom);
  if (sp.gutter.Value()) out += Measure("\\guttersxn", sp.gutter);
  out += Measure("\\headery", sp.headerDistance) + Measure("\\footery", sp.footerDistance);
  if (sp.landscape) out += "\\lndscpsxn";
  if (sp.titlePage) out += "\\titlepg";
  if (sp.columns.count > 1) {
    out += "\\cols" + Num(sp.columns.count) + Measure("\\colsx", sp.columns.space);
    if (sp.columns.separator) out += "\\linebetcol";
  }
  if (sp.pageNumberStart) out += "\\pgnrestart\\pgnstarts" + Num(*sp.pageNumberStart);
  if (sp.pageNumberFormat == "lowerRoman") out += "\\pgnlcrm";
  else if (sp.pageNumberFormat == "upperRoman") out += "\\pgnucrm";
  else if (sp.pageNumberFormat == "lowerLetter") out += "\\pgnlcltr";
  else if (sp.pageNumberFormat == "upperLetter") out += "\\pgnucltr";
  return out;
}

// The main story's paragraphs by section: [first, last] for each.
std::vector<std::pair<uint32_t, uint32_t>> SectionRanges(const Document& doc) {
  std::vector<std::pair<uint32_t, uint32_t>> out;
  for (uint32_t s = 0; s < doc.Sections().size(); ++s) out.push_back(SectionParagraphs(doc, s));
  if (out.empty() && doc.MainStory() != kNone) {
    const std::vector<uint32_t> paras = doc.StoryParagraphs(doc.MainStory());
    if (!paras.empty()) out.push_back({paras.front(), paras.back()});
  }
  return out;
}

}  // namespace

std::string RtfFile(const Document& doc) {
  RtfWriter writer;
  std::string body;
  const std::vector<Section>& sections = doc.Sections();
  const DocSettings& settings = doc.Settings();
  // The document's own page: the first section's, for readers that only read these.
  if (!sections.empty()) {
    const SectionProps& sp = sections.front().props;
    body += Measure("\\paperw", sp.pageWidth) + Measure("\\paperh", sp.pageHeight) + Measure("\\margl", sp.marginStart) +
            Measure("\\margr", sp.marginEnd) + Measure("\\margt", sp.marginTop) + Measure("\\margb", sp.marginBottom);
    if (sp.landscape) body += "\\landscape";
  }
  if (settings.evenAndOddHeaders) body += "\\facingp";
  if (settings.mirrorMargins) body += "\\margmirror";
  body += "\r\n";

  auto part = [&](const char* word, uint32_t story) {
    if (story == kNone) return;
    const std::vector<uint32_t> paras = doc.StoryParagraphs(story);
    if (paras.empty()) return;
    body += "{\\";
    body += word;
    body += ' ';
    writer.Paragraphs(body, StoryClip(doc, paras.front(), paras.back(), doc.Fields(story)));
    body += "\\par}\r\n";
  };
  const std::vector<Field> fields = doc.MainStory() == kNone ? std::vector<Field>{} : doc.Fields(doc.MainStory());
  const auto ranges = SectionRanges(doc);
  for (size_t s = 0; s < ranges.size(); ++s) {
    if (s < sections.size()) {
      const Section& sec = sections[s];
      body += SectionWords(sec.props) + "\r\n";
      // Word's types: default (odd pages when even pages have their own), even, and first page.
      const bool facing = settings.evenAndOddHeaders;
      constexpr auto kDefault = static_cast<size_t>(HdrFtrType::Default), kEven = static_cast<size_t>(HdrFtrType::Even),
                     kFirst = static_cast<size_t>(HdrFtrType::First);
      part(facing ? "headerr" : "header", sec.headers[kDefault]);
      part(facing ? "footerr" : "footer", sec.footers[kDefault]);
      if (facing) {
        part("headerl", sec.headers[kEven]);
        part("footerl", sec.footers[kEven]);
      }
      if (sec.props.titlePage) {
        part("headerf", sec.headers[kFirst]);
        part("footerf", sec.footers[kFirst]);
      }
    }
    writer.Paragraphs(body, StoryClip(doc, ranges[s].first, ranges[s].second, fields));
    if (s + 1 < ranges.size()) body += "\\sect\r\n";
  }
  return writer.File(body);
}

std::string TextFile(const Document& doc, TextEncoding encoding) {
  std::u16string text;
  const std::vector<Section>& sections = doc.Sections();
  const auto ranges = SectionRanges(doc);
  for (size_t s = 0; s < ranges.size(); ++s) {
    // A section that starts a page starts with a form feed, as a page break does.
    if (s && s < sections.size() && sections[s].props.start != SectionStart::Continuous &&
        sections[s].props.start != SectionStart::NextColumn)
      text += kPageBreak;
    for (char16_t c : StoryClip(doc, ranges[s].first, ranges[s].second, {}).PlainText()) {
      if (c == kColumnBreak) text += u"\r\n";
      else text += c;
    }
    text += u"\r\n";
  }
  std::string out;
  if (encoding == TextEncoding::Utf8) {
    out = "\xEF\xBB\xBF";
    AppendUtf8(out, text);
  } else {
    out.reserve(2 + text.size() * 2);
    out = "\xFF\xFE";
    for (char16_t c : text) {
      out += static_cast<char>(c & 0xFF);
      out += static_cast<char>(c >> 8);
    }
  }
  return out;
}

}  // namespace fw::doc::edit
