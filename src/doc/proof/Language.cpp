#include "doc/proof/Language.h"

#include <windows.h>

#include "core/Strings.h"

namespace fw::doc::proof {

namespace {

std::wstring Wide(std::string_view s) { return s.empty() ? std::wstring() : fw::Utf8ToWide(std::string(s)); }

const std::wstring& PcLanguage() {
  static const std::wstring tag = [] {
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
    return GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0 ? std::wstring(name) : std::wstring(L"en-US");
  }();
  return tag;
}

// The run holding offset `at` of `para` (the one before it at the paragraph's end).
Node RunAt(const Document& doc, uint32_t para, uint32_t at, char16_t& c) {
  const std::u16string& text = doc.Para(para).text;
  if (text.empty()) {
    c = u' ';
    return {};
  }
  if (at >= text.size()) at = static_cast<uint32_t>(text.size()) - 1;
  c = text[at];
  Node found;
  for (const TextRun& r : doc.Runs(para)) {
    if (at >= r.start && at < r.end) return r.run;
    if (r.start <= at) found = r.run;
  }
  return found;
}

}  // namespace

bool IsEastAsian(char32_t c) {
  return (c >= 0x1100 && c <= 0x11FF) ||   // Hangul Jamo
         (c >= 0x2E80 && c <= 0x9FFF) ||   // CJK radicals, punctuation, kana, Han
         (c >= 0xA960 && c <= 0xA97F) ||   // Hangul Jamo extended
         (c >= 0xAC00 && c <= 0xD7FF) ||   // Hangul syllables
         (c >= 0xF900 && c <= 0xFAFF) ||   // CJK compatibility ideographs
         (c >= 0xFE30 && c <= 0xFE4F) ||   // CJK compatibility forms
         (c >= 0xFF00 && c <= 0xFFEF) ||   // full-width and half-width forms
         (c >= 0x20000 && c <= 0x3FFFF);   // supplementary Han
}

std::wstring LanguageOf(const Document& doc, uint32_t para, Node run, char16_t c) {
  const RunProps& p = run ? doc.ResolvedRun(para, run) : doc.ResolvedMark(para);
  const DocSettings& s = doc.Settings();
  std::string_view tag;
  if (p.rtl || p.complexScript) tag = !p.langBidi.empty() ? p.langBidi : s.themeFontLangBidi;
  else if (IsEastAsian(c)) tag = !p.langEastAsia.empty() ? p.langEastAsia : s.themeFontLangEastAsia;
  else tag = !p.lang.empty() ? p.lang : s.themeFontLang;
  return tag.empty() ? PcLanguage() : Wide(tag);
}

std::wstring LanguageAt(const Document& doc, Pos pos) {
  if (pos.para == kNone || pos.para >= doc.Paragraphs().size()) return PcLanguage();
  char16_t c = u' ';
  Node run = RunAt(doc, pos.para, pos.offset, c);
  return LanguageOf(doc, pos.para, run, c);
}

std::optional<std::wstring> LanguageOfRange(const Document& doc, Pos from, Pos to) {
  if (from.para == kNone || from.para >= doc.Paragraphs().size()) return PcLanguage();
  if (to.para < from.para || (to.para == from.para && to.offset < from.offset)) std::swap(from, to);
  if (from == to) return LanguageAt(doc, from);
  std::optional<std::wstring> lang;
  const uint32_t story = doc.Para(from.para).story;
  for (uint32_t p = from.para; p <= to.para && p < doc.Paragraphs().size(); ++p) {
    if (doc.Para(p).story != story) continue;
    const std::u16string& text = doc.Para(p).text;
    const uint32_t a = p == from.para ? from.offset : 0;
    const uint32_t b = p == to.para ? to.offset : static_cast<uint32_t>(text.size());
    for (const TextRun& r : doc.Runs(p)) {
      if (r.end <= a || r.start >= b || r.fieldCode) continue;
      // The run's language for its first letter (or its first character).
      uint32_t at = std::max(a, r.start);
      for (uint32_t i = at; i < std::min(b, r.end); ++i)
        if (iswalpha(text[i])) {
          at = i;
          break;
        }
      std::wstring l = LanguageOf(doc, p, r.run, text[at]);
      if (lang && *lang != l) return std::nullopt;
      lang = std::move(l);
    }
  }
  return lang ? lang : LanguageAt(doc, from);
}

std::wstring LanguageName(std::wstring_view tag) {
  wchar_t name[256] = {};
  const std::wstring t(tag);
  if (GetLocaleInfoEx(t.c_str(), LOCALE_SLOCALIZEDDISPLAYNAME, name, 256) > 0) return name;
  return t;
}

edit::Result SetLanguage(edit::Editor& editor, Pos from, Pos to, std::wstring_view tag) {
  editor.BeginGroup(u"Language");
  edit::Result r = editor.SetRunProps(from, to, edit::PropEdit().Set("lang", {{"val", fw::WideToUtf8(std::wstring(tag))}}));
  editor.EndGroup();
  return r;
}

}  // namespace fw::doc::proof
