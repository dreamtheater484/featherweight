#include "slide/view/TextSpell.h"

#include <windows.h>

#include <algorithm>

#include "core/Strings.h"
#include "doc/proof/Language.h"
#include "draw/Xml.h"

namespace fw::slide::view {

namespace {

constexpr size_t kCached = 512;  // bodies

struct Segment {
  std::wstring lang;
  std::wstring text;
  std::vector<uint32_t> map;  // per character: its offset in the paragraph
};

std::wstring UserLanguage() {
  wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
  return GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0 ? std::wstring(name) : std::wstring(L"en-US");
}

}  // namespace

const std::vector<SpellError>& SlideSpelling::Check(const TextBody& body) {
  // The key: each run's language and text (fields and unchecked runs marked), paragraph by paragraph.
  std::wstring key;
  for (const Paragraph& p : body.paragraphs) {
    for (const TextRun& r : p.runs) {
      key += Utf8ToWide(r.props.lang);
      key += L'\x1';
      const bool skip = r.kind == TextRun::Kind::Field ||
                        draw::xml::Bool(draw::xml::A(r.node, "rPr"), "noProof", false);
      key += skip ? L'\x2' : L'\x3';
      key += r.kind == TextRun::Kind::Break ? std::wstring(L"\u000B") : r.text;
    }
    key += L'\x4';
  }
  if (auto it = cache_.find(key); it != cache_.end()) return it->second;

  std::vector<SpellError> errors;
  static const std::wstring user = UserLanguage();
  for (size_t pi = 0; pi < body.paragraphs.size(); ++pi) {
    const Paragraph& p = body.paragraphs[pi];
    std::vector<Segment> segments;
    uint32_t at = 0;
    auto gap = [&](uint32_t offset) {
      if (!segments.empty()) {
        segments.back().text += L' ';
        segments.back().map.push_back(offset);
      }
    };
    for (const TextRun& r : p.runs) {
      const std::wstring text = r.kind == TextRun::Kind::Break ? std::wstring(L" ") : r.text;
      const bool skip = r.kind == TextRun::Kind::Field || draw::xml::Bool(draw::xml::A(r.node, "rPr"), "noProof", false);
      if (skip) {
        gap(at);
      } else {
        // The run's language for Latin text; East Asian text isn't checked (as the documents').
        const std::wstring lang = checker_.Resolve(r.props.lang.empty() ? user : Utf8ToWide(r.props.lang));
        for (size_t i = 0; i < text.size(); ++i) {
          const wchar_t c = text[i];
          if (fw::doc::proof::IsEastAsian(c) || lang.empty()) {
            gap(at + static_cast<uint32_t>(i));
            continue;
          }
          if (segments.empty() || segments.back().lang != lang) segments.push_back({lang, {}, {}});
          segments.back().text += c;
          segments.back().map.push_back(at + static_cast<uint32_t>(i));
        }
      }
      at += static_cast<uint32_t>(text.size());
    }
    for (const Segment& s : segments) {
      if (s.lang.empty() || s.text.find_first_not_of(L' ') == std::wstring::npos) continue;
      for (const text::SpellChecker::Error& e : checker_.Check(s.lang, s.text)) {
        if (e.length == 0 || e.start + e.length > s.map.size()) continue;
        SpellError out;
        out.para = static_cast<uint32_t>(pi);
        out.from = s.map[e.start];
        out.to = s.map[e.start + e.length - 1] + 1;
        out.lang = s.lang;
        out.action = e.action;
        out.replacement = e.replacement;
        errors.push_back(std::move(out));
      }
    }
  }
  std::sort(errors.begin(), errors.end(), [](const SpellError& a, const SpellError& b) {
    return a.para != b.para ? a.para < b.para : a.from < b.from;
  });
  if (cache_.size() >= kCached && !order_.empty()) {
    cache_.erase(order_.front());
    order_.pop_front();
  }
  order_.push_back(key);
  return cache_.emplace(key, std::move(errors)).first->second;
}

}  // namespace fw::slide::view
