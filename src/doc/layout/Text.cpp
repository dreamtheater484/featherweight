// From the model's paragraphs to fwtext's (Internal.h): resolved properties to text styles and
// paragraph formats, the list label, field results that pagination computes, note reference
// marks, inline objects, and what the view hides (field codes, deleted text, markers).
#include <windows.h>

#include <algorithm>
#include <cwctype>

#include "core/Strings.h"
#include "doc/model/Utf.h"
#include "doc/layout/Engine.h"

namespace fw::doc::layout {

namespace {

std::wstring Wide(std::string_view s) { return s.empty() ? std::wstring() : Utf8ToWide(s); }

std::wstring StyleKey(const text::TextStyle& s) {
  std::wstring k;
  k.reserve(64 + s.family.size());
  k += s.family;
  k += L'|';
  k += s.eastAsiaFamily;
  k += L'|';
  k += s.complexFamily;
  k += L'|';
  k += s.language;
  k += L'|';
  k += s.eastAsiaLanguage;
  k += L'|';
  k += s.complexLanguage;
  k += L'|';
  const float nums[] = {s.size, s.complexSize, s.letterSpacing, s.scale, s.baselineShift, s.kerningMinSize,
                        s.objectWidth, s.objectAscent, s.objectDescent};
  k.append(reinterpret_cast<const wchar_t*>(nums), sizeof nums / sizeof(wchar_t));
  const wchar_t flags[] = {static_cast<wchar_t>(int(s.bold) | s.italic << 1 | s.complexBold << 2 | s.complexItalic << 3 |
                                                s.ligatures << 4 | s.hidden << 5 | s.rightToLeft << 6 |
                                                s.complexScript << 7 | s.eastAsiaHint << 8),
                           static_cast<wchar_t>(static_cast<int>(s.caps) | static_cast<int>(s.verticalAlign) << 4)};
  k.append(flags, 2);
  return k;
}

bool Deleted(Node revision) { return revision && (revision.Is(Name("del")) || revision.Is(Name("moveFrom"))); }

text::Alignment AlignmentOf(Justification jc) {
  switch (jc) {
    case Justification::Center: return text::Alignment::Center;
    case Justification::End: return text::Alignment::End;
    case Justification::Both:
    case Justification::LowKashida:
    case Justification::MediumKashida:
    case Justification::HighKashida:
    case Justification::ThaiDistribute: return text::Alignment::Justify;
    case Justification::Distribute: return text::Alignment::Distribute;
    default: return text::Alignment::Start;
  }
}

text::TabLeader LeaderOf(TabLeader l) {
  switch (l) {
    case TabLeader::Dot: return text::TabLeader::Dot;
    case TabLeader::Hyphen: return text::TabLeader::Hyphen;
    case TabLeader::Underscore: return text::TabLeader::Underscore;
    case TabLeader::Heavy: return text::TabLeader::Heavy;
    case TabLeader::MiddleDot: return text::TabLeader::MiddleDot;
    default: return text::TabLeader::None;
  }
}

// The field types whose results pagination computes, and the number format a switch asks for.
enum class PageField { None, Page, NumPages, SectionPages };
PageField PageFieldOf(std::u16string_view type) {
  if (type == u"PAGE") return PageField::Page;
  if (type == u"NUMPAGES") return PageField::NumPages;
  if (type == u"SECTIONPAGES") return PageField::SectionPages;
  return PageField::None;
}
std::string_view FormatSwitch(std::u16string_view code) {
  size_t at = code.find(u"\\*");
  if (at == std::u16string_view::npos) return {};
  std::u16string_view rest = code.substr(at + 2);
  while (!rest.empty() && rest.front() == u' ') rest.remove_prefix(1);
  auto starts = [&](std::u16string_view w) { return rest.substr(0, w.size()) == w; };
  if (starts(u"roman")) return "lowerRoman";
  if (starts(u"ROMAN")) return "upperRoman";
  if (starts(u"alphabetic")) return "lowerLetter";
  if (starts(u"ALPHABETIC")) return "upperLetter";
  return {};
}

// DATE and TIME, which Word updates when it opens a document (plan §6.2). The other date fields
// (CREATEDATE, SAVEDATE, PRINTDATE) keep their results.
bool IsDateField(std::u16string_view type) { return type == u"DATE" || type == u"TIME"; }

// The \@ switch's date-time picture: quoted ("MMMM d, yyyy") or one word (dd.MM.yy).
std::wstring DatePicture(std::u16string_view code) {
  const size_t at = code.find(u"\\@");
  if (at == std::u16string_view::npos) return {};
  std::u16string_view rest = code.substr(at + 2);
  while (!rest.empty() && rest.front() == u' ') rest.remove_prefix(1);
  if (rest.empty()) return {};
  if (rest.front() == u'"') {
    const size_t end = rest.find(u'"', 1);
    rest = rest.substr(1, end == std::u16string_view::npos ? std::u16string_view::npos : end - 1);
  } else {
    rest = rest.substr(0, rest.find(u' '));
  }
  return std::wstring(rest.begin(), rest.end());
}

// `t` as Word shows a DATE or TIME field: by the picture's codes (M MM MMM MMMM, d dd ddd dddd, yy
// yyyy, h hh H HH, m mm, s ss, AM/PM am/pm A/P a/p, 'literal text'), with month and day names in
// the field's language; with no picture, the language's short date (DATE) or its time (TIME).
std::u16string FormatDateTime(const SYSTEMTIME& t, const std::wstring& picture, bool time, const wchar_t* locale) {
  std::wstring out;
  auto names = [&](const wchar_t* code) {  // names from Windows, in the language
    wchar_t b[128];
    const int n = GetDateFormatEx(locale, 0, &t, code, b, 128, nullptr);
    if (n > 1) out.append(b, static_cast<size_t>(n - 1));
  };
  auto num = [&](int v, bool two) {
    wchar_t b[16];
    swprintf_s(b, two ? L"%02d" : L"%d", v);
    out += b;
  };
  if (picture.empty()) {
    wchar_t b[128];
    const int n = time ? GetTimeFormatEx(locale, TIME_NOSECONDS, &t, nullptr, b, 128)
                       : GetDateFormatEx(locale, DATE_SHORTDATE, &t, nullptr, b, 128, nullptr);
    if (n > 1) out.assign(b, static_cast<size_t>(n - 1));
    return std::u16string(out.begin(), out.end());
  }
  auto designator = [&](bool pm) {
    wchar_t b[32] = {};
    std::wstring d = GetLocaleInfoEx(locale, pm ? LOCALE_S2359 : LOCALE_S1159, b, 32) > 1 ? std::wstring(b) : std::wstring();
    return d.empty() ? std::wstring(pm ? L"PM" : L"AM") : d;
  };
  const std::wstring& p = picture;
  for (size_t i = 0; i < p.size();) {
    const wchar_t c = p[i];
    if (c == L'\'') {  // text as it is
      const size_t end = p.find(L'\'', i + 1);
      out += p.substr(i + 1, (end == std::wstring::npos ? p.size() : end) - i - 1);
      i = end == std::wstring::npos ? p.size() : end + 1;
      continue;
    }
    auto at = [&](std::wstring_view w) { return _wcsnicmp(p.c_str() + i, w.data(), w.size()) == 0 && p.size() - i >= w.size(); };
    if (at(L"AM/PM") || at(L"A/P")) {
      std::wstring d = designator(t.wHour >= 12);
      const bool lower = c == L'a';
      if (at(L"A/P")) d = d.substr(0, 1);
      for (wchar_t& ch : d) ch = lower ? static_cast<wchar_t>(std::towlower(ch)) : static_cast<wchar_t>(std::towupper(ch));
      out += d;
      i += at(L"A/P") ? 3 : 5;
      continue;
    }
    size_t n = 1;
    while (i + n < p.size() && p[i + n] == c) ++n;
    switch (c) {
      case L'M':
        if (n <= 2) num(t.wMonth, n == 2);
        else names(n == 3 ? L"MMM" : L"MMMM");
        break;
      case L'd':
      case L'D':
        if (n <= 2) num(t.wDay, n == 2);
        else names(n == 3 ? L"ddd" : L"dddd");
        break;
      case L'y':
      case L'Y':
        if (n <= 2) num(t.wYear % 100, true);
        else num(t.wYear, false);
        break;
      case L'h': num(t.wHour % 12 == 0 ? 12 : t.wHour % 12, n >= 2); break;
      case L'H': num(t.wHour, n >= 2); break;
      case L'm': num(t.wMinute, n >= 2); break;
      case L's':
      case L'S': num(t.wSecond, n >= 2); break;
      default: out.append(n, c); break;
    }
    i += n;
  }
  return std::u16string(out.begin(), out.end());
}

}  // namespace

std::u16string DateFieldResult(std::u16string_view code, int64_t now, std::string_view lang) {
  const size_t s = code.find_first_not_of(u' ');
  if (s == std::u16string_view::npos || now == 0) return {};
  const size_t e = code.find(u' ', s);
  const std::u16string_view type = code.substr(s, e == std::u16string_view::npos ? std::u16string_view::npos : e - s);
  if (!IsDateField(type)) return {};
  FILETIME ft;
  ft.dwLowDateTime = static_cast<DWORD>(static_cast<uint64_t>(now) & 0xFFFFFFFFu);
  ft.dwHighDateTime = static_cast<DWORD>(static_cast<uint64_t>(now) >> 32);
  SYSTEMTIME utc, local;
  if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return {};
  std::wstring locale(lang.begin(), lang.end());
  const wchar_t* name = !locale.empty() && IsValidLocaleName(locale.c_str()) ? locale.c_str() : LOCALE_NAME_USER_DEFAULT;
  return FormatDateTime(local, DatePicture(code), type == u"TIME", name);
}

const text::TextStyle* StylePool::Intern(const text::TextStyle& style) {
  std::wstring key = StyleKey(style);
  auto it = index_.find(key);
  if (it != index_.end()) return it->second;
  const text::TextStyle* s = &pool_.emplace_back(style);
  index_.emplace(std::move(key), s);
  return s;
}

const text::FontMetrics& StylePool::Metrics(text::Fonts& fonts, const text::TextStyle* style) {
  auto it = metrics_.find(style);
  if (it != metrics_.end()) return it->second;
  text::FontMetrics m;
  const std::wstring& family = fonts.Resolve(style->family);
  if (const text::Face* face = fonts.Get(family, style->bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                         style->italic))
    m = face->Metrics(style->size);
  return metrics_.emplace(style, m).first->second;
}

uint32_t ParaText::ModelOffset(uint32_t at) const {
  if (at < prefix) return 0;
  int64_t delta = -static_cast<int64_t>(prefix);
  for (const Subst& s : subst) {
    if (at < s.at) break;
    if (at < s.at + s.length) return s.modelAt;
    delta = static_cast<int64_t>(s.modelAt + s.modelLength) - static_cast<int64_t>(s.at + s.length);
  }
  return static_cast<uint32_t>(std::max<int64_t>(0, static_cast<int64_t>(at) + delta));
}

std::u16string ParaText::Shown(uint32_t from, uint32_t to, bool hyphenated) const {
  std::u16string s;
  uint32_t at = 0;
  size_t r = 0;
  while (r < runs.size() && at + runs[r].length <= from) at += runs[r++].length;
  for (uint32_t i = from; i < to && i < text.size(); ++i) {
    while (r < runs.size() && i >= at + runs[r].length) at += runs[r++].length;
    if (r < runs.size() && runs[r].style->hidden) continue;
    char16_t c = static_cast<char16_t>(text[i]);
    switch (c) {
      case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0xAD: case 0x200B: case 0x2028: case 0xFFFC: continue;
      case 0x2011: c = u'-'; break;
      case 0xA0: c = u' '; break;
      default: break;
    }
    if (c < 0x20 && c != u'\t') continue;
    s += c;
  }
  while (!s.empty() && s.back() == u' ') s.pop_back();
  if (hyphenated) s += u'-';
  return s;
}

uint32_t ParaText::LayoutOffset(uint32_t model) const {
  int64_t delta = prefix;
  for (const Subst& s : subst) {
    if (model < s.modelAt) break;
    if (model < s.modelAt + s.modelLength) return s.at;
    delta = static_cast<int64_t>(s.at + s.length) - static_cast<int64_t>(s.modelAt + s.modelLength);
  }
  return static_cast<uint32_t>(static_cast<int64_t>(model) + delta);
}

text::TextStyle Engine::StyleOf(const RunProps& r, bool hidden) const {
  text::TextStyle s;
  s.family = Wide(!r.fonts.ascii.empty() ? r.fonts.ascii : r.fonts.hAnsi);
  s.eastAsiaFamily = Wide(r.fonts.eastAsia);
  s.complexFamily = Wide(r.fonts.cs);
  s.size = Pt(r.size);
  s.complexSize = Pt(r.sizeCs);
  s.bold = r.bold;
  s.italic = r.italic;
  s.complexBold = r.boldCs;
  s.complexItalic = r.italicCs;
  s.letterSpacing = Pt(r.spacing);
  s.scale = r.scale > 0 && r.scale != 100 ? static_cast<float>(r.scale) / 100.0f : 1.0f;
  s.baselineShift = Pt(r.position);
  s.kerningMinSize = r.kern.Value() > 0 ? Pt(r.kern) : 0;
  s.caps = r.caps ? text::Caps::All : r.smallCaps ? text::Caps::Small : text::Caps::None;
  s.verticalAlign = r.vertAlign == VertAlign::Superscript ? text::VerticalAlign::Superscript
                    : r.vertAlign == VertAlign::Subscript ? text::VerticalAlign::Subscript
                                                          : text::VerticalAlign::Baseline;
  s.hidden = hidden || r.specVanish || (r.vanish && !options_.showHiddenText);
  s.rightToLeft = r.rtl;
  s.complexScript = r.complexScript;
  s.eastAsiaHint = r.fonts.hint == FontHint::EastAsia;
  s.language = Wide(r.lang);
  s.eastAsiaLanguage = Wide(r.langEastAsia);
  s.complexLanguage = Wide(r.langBidi);
  return s;
}

RunPaint Engine::PaintOf(const RunProps& r, uint32_t modelRun) const {
  RunPaint p;
  p.color = r.color.automatic ? 0 : r.color.rgb;
  if (r.color.automatic && r.shading.Present() && !r.shading.fill.automatic) {
    uint32_t f = r.shading.fill.rgb;
    int luma = (((f >> 16) & 255) * 299 + ((f >> 8) & 255) * 587 + (f & 255) * 114) / 1000;
    if (luma < 96) p.color = 0xFFFFFF;
  }
  p.underline = r.underline;
  p.underlineColor = r.underlineColor.automatic ? p.color : r.underlineColor.rgb;
  p.strike = r.strike;
  p.doubleStrike = r.doubleStrike;
  if (r.highlight.present) {
    p.highlight = true;
    p.highlightColor = r.highlight.rgb;
  } else if (r.shading.Present() && !r.shading.fill.automatic) {
    p.highlight = true;
    p.highlightColor = r.shading.fill.rgb;
  }
  // Hidden text shown with the formatting marks: dotted underneath, as Word shows it.
  if (r.vanish && options_.showHiddenText && options_.formattingMarks && p.underline == Underline::None) {
    p.underline = Underline::Dotted;
    p.underlineColor = 0x7F7F7F;
  }
  p.modelRun = modelRun;
  return p;
}

text::ParagraphFormat Engine::FormatOf(const ParaProps& p) const {
  const DocSettings& set = doc_.Settings();
  text::ParagraphFormat f;
  f.alignment = AlignmentOf(p.jc);
  f.rightToLeft = p.bidi;
  f.startIndent = Pt(p.indStart);
  f.endIndent = Pt(p.indEnd);
  f.firstLineIndent = Pt(p.firstLine);
  for (const TabStop& t : p.tabs) {
    text::TabStop s;
    s.position = Pt(t.pos);
    s.leader = LeaderOf(t.leader);
    switch (t.align) {
      case TabAlign::Center: s.align = text::TabAlign::Center; break;
      case TabAlign::End: s.align = text::TabAlign::End; break;
      case TabAlign::Decimal: s.align = text::TabAlign::Decimal; break;
      case TabAlign::Bar: s.align = text::TabAlign::Bar; break;
      case TabAlign::Clear: continue;
      default: s.align = text::TabAlign::Start; break;
    }
    f.tabs.push_back(s);
  }
  float defaultTab = Pt(set.defaultTabStop);
  f.defaultTabStop = defaultTab > 0 ? defaultTab : 36;
  f.hyphenation.automatic = set.autoHyphenation && !p.suppressAutoHyphens;
  f.hyphenation.zone = compatMode_ >= 15 ? 0 : Pt(set.hyphenationZone);
  f.hyphenation.consecutiveLimit = set.consecutiveHyphenLimit;
  f.hyphenation.caps = !set.doNotHyphenateCaps;
  f.expandLineBreaks = !set.Compat(Name("doNotExpandShiftReturn"));
  f.squeezeSpaces = compatMode_ >= 15;
  if (!set.decimalSymbol.empty()) f.decimalSeparator = static_cast<wchar_t>(set.decimalSymbol.front());
  return f;
}

void Engine::BuildText(uint32_t para, const PageFields& fields, ParaText& out, bool pageBreaks, bool numberNotes) {
  out = ParaText();
  out.para = para;
  const Paragraph& model = doc_.Para(para);
  const ParaProps& props = doc_.ResolvedPara(para);
  out.props = &props;
  const std::u16string& t = model.text;
  out.text.reserve(t.size() + 8);

  auto emit = [&](std::u16string_view s, const text::TextStyle* style, const RunPaint& paint) {
    if (s.empty()) return;
    out.text.append(reinterpret_cast<const wchar_t*>(s.data()), s.size());
    if (!out.runs.empty() && out.runs.back().style == style && out.paints.back().modelRun == paint.modelRun) {
      out.runs.back().length += static_cast<uint32_t>(s.size());
      return;
    }
    out.runs.push_back({static_cast<uint32_t>(s.size()), style});
    out.paints.push_back(paint);
    RunPaint& p = out.paints.back();
    if (p.underline != Underline::None || p.strike || p.doubleStrike) {
      const text::FontMetrics& m = styles_.Metrics(fonts_, style);
      p.underlineOffset = m.underlinePosition;
      p.underlineThickness = m.underlineThickness;
      p.strikeOffset = m.strikethroughPosition;
      p.strikeThickness = m.strikethroughThickness;
    }
  };
  auto subst = [&](uint32_t modelAt, uint32_t modelLength, std::u16string_view value, const text::TextStyle* style,
                   const RunPaint& paint) {
    out.subst.push_back({static_cast<uint32_t>(out.text.size()), static_cast<uint32_t>(value.size()), modelAt, modelLength});
    emit(value, style, paint);
  };

  // The list label and what follows it.
  const RunProps& markProps = doc_.ResolvedMark(para);
  if (std::optional<ListLabel> label = doc_.Label(para)) {
    std::u16string s = label->text;
    if (label->suffix.empty() || label->suffix == "tab") s += u'\t';
    else if (label->suffix == "space") s += u' ';
    if (!s.empty() && !(markProps.vanish && !options_.showHiddenText)) {
      text::TextStyle ls = StyleOf(label->props, false);
      emit(s, styles_.Intern(ls), PaintOf(label->props, kNone));
      out.prefix = static_cast<uint32_t>(out.text.size());
    }
  }

  // Field results pagination computes: [from, to) in the model text, replaced.
  struct Replace {
    uint32_t from, to;
    std::u16string value;
  };
  std::vector<Replace> replaces;
  if (fieldsCount_[model.story] > 0) {
    for (const Field& f : StoryFields(model.story)) {
      if (f.separator.para != para || f.end.para != para || !f.complete) continue;
      PageField kind = PageFieldOf(f.Type());
      if (kind == PageField::None) {
        if (options_.now == 0 || !IsDateField(f.Type())) continue;
        // In the language of the result's first character.
        std::string_view lang;
        for (const TextRun& r : doc_.Runs(para))
          if (r.run && r.start <= f.separator.offset + 1 && f.separator.offset + 1 < r.end) lang = doc_.ResolvedRun(para, r.run).lang;
        std::u16string value = DateFieldResult(f.code, options_.now, lang);
        if (!value.empty()) replaces.push_back({f.separator.offset + 1, f.end.offset, std::move(value)});
        continue;
      }
      std::u16string value = kind == PageField::Page       ? fields.page
                             : kind == PageField::NumPages ? fields.pages
                                                           : fields.sectionPages;
      if (kind != PageField::Page) out.usesPageCount = true;
      else out.usesPage = true;
      if (value.empty()) continue;  // not known yet: the last result
      if (std::string_view fmt = FormatSwitch(f.code); !fmt.empty()) {
        int64_t n = 0;
        for (char16_t c : value)
          if (c >= u'0' && c <= u'9') n = n * 10 + (c - u'0');
        value = FormatNumber(n, fmt);
      }
      replaces.push_back({f.separator.offset + 1, f.end.offset, std::move(value)});
    }
  }

  std::vector<InlineItem> items = doc_.Items(para);
  size_t item = 0;
  const std::vector<TextRun> runs = doc_.Runs(para);
  const StoryKind storyKind = doc_.Stories()[model.story].kind;
  bool visible = false;
  uint32_t covered = 0;  // model text emitted so far

  for (uint32_t r = 0; r < runs.size(); ++r) {
    const TextRun& run = runs[r];
    if (run.start < covered) continue;  // overlapping runs don't happen; be safe
    const RunProps& rp = doc_.ResolvedRun(para, run.run);
    bool hidden = run.fieldCode || Deleted(run.revision);
    const text::TextStyle* style = styles_.Intern(StyleOf(rp, hidden));
    const text::TextStyle* hiddenStyle = style->hidden ? style : nullptr;
    auto hide = [&]() {
      if (!hiddenStyle) {
        text::TextStyle h = *style;
        h.hidden = true;
        hiddenStyle = styles_.Intern(h);
      }
      return hiddenStyle;
    };
    RunPaint paint = PaintOf(rp, r);
    // fldSimple PAGE / NUMPAGES: the run's text is the result.
    if (run.simpleField && !hidden) {
      std::u16string code = ToUtf16(run.simpleField.AttrOr(Name("instr")));
      size_t s = code.find_first_not_of(u' ');
      size_t e = s == std::u16string::npos ? s : code.find(u' ', s);
      PageField kind = s == std::u16string::npos ? PageField::None : PageFieldOf(std::u16string_view(code).substr(s, e - s));
      if (kind == PageField::None && options_.now != 0) {
        if (std::u16string value = DateFieldResult(code, options_.now, rp.lang); !value.empty()) {
          uint32_t end = run.end;
          while (r + 1 < runs.size() && runs[r + 1].simpleField == run.simpleField) end = runs[++r].end;
          subst(run.start, end - run.start, value, style, paint);
          visible = true;
          covered = end;
          continue;
        }
      }
      if (kind != PageField::None) {
        if (kind != PageField::Page) out.usesPageCount = true;
        else out.usesPage = true;
        const std::u16string& value = kind == PageField::Page       ? fields.page
                                      : kind == PageField::NumPages ? fields.pages
                                                                    : fields.sectionPages;
        if (!value.empty()) {
          uint32_t end = run.end;
          while (r + 1 < runs.size() && runs[r + 1].simpleField == run.simpleField) end = runs[++r].end;
          subst(run.start, end - run.start, value, style, paint);
          visible = true;
          covered = end;
          continue;
        }
      }
    }
    uint32_t i = std::max(run.start, covered);
    while (i < run.end) {
      // A computed field result starting here replaces the old one.
      auto rep = std::find_if(replaces.begin(), replaces.end(), [&](const Replace& x) { return x.from == i; });
      if (rep != replaces.end() && !style->hidden) {
        uint32_t to = std::max(rep->to, i);
        subst(i, to - i, rep->value, style, paint);
        visible = true;
        i = to;
        rep->from = UINT32_MAX;
        if (i >= run.end) break;
        continue;
      }
      char16_t c = t[i];
      // The item for this character; markers without text before it (w:lastRenderedPageBreak,
      // bookmarks) don't count.
      while (item < items.size() && (items[item].offset < i || (items[item].offset == i && items[item].length == 0))) ++item;
      const InlineItem* it = item < items.size() && items[item].offset == i ? &items[item] : nullptr;
      switch (c) {
        case 0x13: case 0x14: case 0x15: case 0x05: case 0x03: case 0x04:
          emit(std::u16string_view(&t[i], 1), hide(), paint);
          ++i;
          continue;
        case 0x0C: case 0x0E:
          if (pageBreaks) break;
          emit(std::u16string_view(&t[i], 1), hide(), paint);
          ++i;
          continue;
        case 0x02: {
          std::u16string mark;
          if (it && (it->kind == ItemKind::FootnoteRef || it->kind == ItemKind::EndnoteRef)) {
            StoryKind k = it->kind == ItemKind::FootnoteRef ? StoryKind::Footnote : StoryKind::Endnote;
            uint32_t story = doc_.NoteStory(k, it->node.AttrOr(Name("id")));
            bool custom = it->node.OnOff(Name("customMarkFollows")).value_or(false);
            if (story != kNone && !custom) {
              mark = numberNotes ? NoteReference(story) : NoteMark(story);
              out.notes.push_back({static_cast<uint32_t>(out.text.size()), story});
            } else if (story != kNone) {
              if (numberNotes) NoteReference(story);
              out.notes.push_back({static_cast<uint32_t>(out.text.size()), story});
            }
          } else if (it && it->kind == ItemKind::NoteMark &&
                     (storyKind == StoryKind::Footnote || storyKind == StoryKind::Endnote)) {
            mark = NoteMark(model.story);
          }
          if (!mark.empty() && !style->hidden) {
            subst(i, 1, mark, style, paint);
            visible = true;
          } else {
            emit(std::u16string_view(&t[i], 1), hide(), paint);
          }
          ++i;
          continue;
        }
        case 0xFFFC: {
          if (it && it->kind == ItemKind::Drawing && it->drawing != kNone) {
            const Drawing& d = doc_.Drawings()[it->drawing];
            if (d.anchored && !style->hidden) {
              out.anchors.push_back(it->drawing);
              emit(std::u16string_view(&t[i], 1), hide(), paint);
            } else if (!d.hidden && !style->hidden) {
              text::TextStyle os = *style;
              os.objectWidth = Pt(Emu(d.cx.Value() + d.effectL.Value() + d.effectR.Value()));
              os.objectAscent = Pt(Emu(d.cy.Value() + d.effectT.Value() + d.effectB.Value()));
              os.objectDescent = 0;
              if (d.kind == DrawingKind::Equation) {
                os.objectWidth = EquationWidth(it->drawing, style->size);
                os.objectAscent = style->size * 0.9f;
                os.objectDescent = style->size * 0.25f;
              }
              out.inlines.push_back({static_cast<uint32_t>(out.text.size()), it->drawing});
              emit(std::u16string_view(&t[i], 1), styles_.Intern(os), paint);
              visible = true;
            } else {
              emit(std::u16string_view(&t[i], 1), hide(), paint);
            }
          } else if (it && it->kind == ItemKind::PageNumber && !style->hidden) {
            subst(i, 1, fields.page, style, paint);
            out.usesPage = true;
            visible = true;
          } else {
            emit(std::u16string_view(&t[i], 1), hide(), paint);
          }
          ++i;
          continue;
        }
        default: break;
      }
      // Plain text up to the next special character, run end or replacement.
      uint32_t j = i + 1;
      while (j < run.end) {
        char16_t d = t[j];
        if (d == 0x13 || d == 0x14 || d == 0x15 || d == 0x05 || d == 0x03 || d == 0x04 || d == 0x02 || d == 0xFFFC) break;
        if ((d == 0x0C || d == 0x0E) && !pageBreaks) break;
        ++j;
      }
      for (const Replace& x : replaces)
        if (x.from > i && x.from < j) j = x.from;
      if (it && it->kind == ItemKind::Symbol && it->node) {
        // w:sym: its own font.
        std::string_view font = it->node.AttrOr(Name("font"));
        if (!font.empty()) {
          text::TextStyle ss = *style;
          ss.family = Wide(font);
          ss.eastAsiaFamily.clear();
          ss.complexFamily.clear();
          emit(std::u16string_view(&t[i], 1), styles_.Intern(ss), paint);
          if (!style->hidden) visible = true;
          ++i;
          continue;
        }
      }
      emit(std::u16string_view(&t[i], j - i), style, paint);
      if (!style->hidden) visible = true;
      i = j;
    }
    covered = std::max(covered, run.end);
  }
  // Text no run covers (there shouldn't be any): hidden, so offsets still match.
  if (covered < t.size()) {
    text::TextStyle h = StyleOf(markProps, true);
    emit(std::u16string_view(t).substr(covered), styles_.Intern(h), RunPaint());
  }

  // A deleted paragraph mark joins the paragraph to the next in Word's final view, so one with
  // nothing of its own left to show is gone.
  const Node markRPr = doc_.ParaNode(para).Child(Name("pPr")).Child(Name("rPr"));
  const bool markDeleted = markRPr && (markRPr.Child(Name("del")) || markRPr.Child(Name("moveFrom")));
  const bool markHidden = (markProps.vanish && !options_.showHiddenText) || markProps.specVanish || markDeleted;
  out.empty = markHidden && !visible && (out.prefix == 0 || markDeleted);  // a deleted mark takes its label along
  text::TextStyle mark = StyleOf(markProps, false);
  out.fw.markStyle = styles_.Intern(mark);
  out.fw.text = out.text;
  out.fw.runs = out.runs;
  out.fw.format = FormatOf(props);
}

std::u16string Engine::NoteReference(uint32_t story) {
  auto it = noteNumbers_.find(story);
  if (it != noteNumbers_.end()) return it->second;
  const bool foot = doc_.Stories()[story].kind == StoryKind::Footnote;
  const NoteProps& np = foot ? doc_.Settings().footnotes : doc_.Settings().endnotes;
  int32_t n = (foot ? footnoteCount_++ : endnoteCount_++) + std::max(1, np.start);
  std::string_view fmt = !np.numFmt.empty() ? np.numFmt : foot ? "decimal" : "lowerRoman";
  noteOrder_[story] = n;
  noteSeq_.push_back(story);
  return noteNumbers_.emplace(story, FormatNumber(n, fmt)).first->second;
}

std::u16string Engine::NoteMark(uint32_t story) const {
  auto it = noteNumbers_.find(story);
  if (it == noteNumbers_.end()) return u"";
  return it->second;
}

float Engine::EquationWidth(uint32_t drawing, float size) {
  std::string xml = doc_.DrawingXml(doc_.Drawings()[drawing]);
  size_t chars = 0;
  for (size_t at = xml.find("<m:t"); at != std::string::npos; at = xml.find("<m:t", at + 1)) {
    size_t open = xml.find('>', at);
    if (open == std::string::npos) break;
    if (xml[open - 1] == '/') continue;
    size_t close = xml.find("</m:t>", open);
    if (close == std::string::npos) break;
    for (size_t k = open + 1; k < close; ++k)
      if ((static_cast<unsigned char>(xml[k]) & 0xC0) != 0x80) ++chars;
  }
  return static_cast<float>(chars) * size * 0.5f;
}

}  // namespace fw::doc::layout
