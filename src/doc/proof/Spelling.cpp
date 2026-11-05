#include "doc/proof/Spelling.h"

#include <algorithm>
#include <chrono>

#include "doc/proof/Language.h"
#include "pkg/XmlPull.h"

namespace fw::doc::proof {

namespace {

using Clock = std::chrono::steady_clock;

constexpr const char* kW = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";

// Text of a deletion (w:del, w:moveFrom): not shown, so not checked.
bool Deleted(Node revision) { return revision && (revision.Is(Name("del")) || revision.Is(Name("moveFrom"))); }

// One stretch of a paragraph in one language, as the checker reads it, with each character's
// offset in the paragraph's text.
struct Segment {
  std::wstring lang;  // the installed language that checks it; empty: not checked
  std::wstring text;
  std::vector<uint32_t> map;
};

// Characters inside words that the checker shouldn't see (soft hyphen, zero-width joiners), and
// characters that stand for objects or breaks, which end a word.
bool Dropped(char16_t c) { return c == 0x00AD || c == 0x200C || c == 0x200D || c == 0x2060 || c == 0xFEFF; }
char16_t Shown(char16_t c) {
  switch (c) {
    case 0x2011: return u'-';  // non-breaking hyphen
    case 0x00A0: case 0x202F: return u' ';
    case u'\t': case 0x0B: case 0x0C: case 0x0E: case 0x0D: return u' ';
    case 0x02: case 0x05: case 0x13: case 0x14: case 0x15: case 0xFFFC: return u' ';  // notes, comments, fields, objects
    default: return c < 0x20 ? u' ' : c;
  }
}

}  // namespace

Spelling::Spelling(const Document& doc, text::SpellChecker& checker) : doc_(doc), checker_(checker) {
  paras_.resize(doc.Paragraphs().size());
  for (uint32_t p = 0; p < paras_.size(); ++p) {
    const uint32_t story = doc.Para(p).story;
    if (story < doc.Stories().size() && doc.Stories()[story].kind == StoryKind::Comment) paras_[p].checked = true;
    else ++pending_;
  }
}

void Spelling::Prefer(uint32_t para) {
  if (para < paras_.size()) next_ = para;
}

bool Spelling::Continue(double ms) {
  if (pending_ == 0 || paras_.empty()) return false;
  const auto until = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(ms));
  bool changed = false;
  do {
    // The next unchecked paragraph from the cursor on, wrapping around.
    for (size_t n = 0; n < paras_.size() && paras_[next_].checked; ++n) next_ = (next_ + 1) % paras_.size();
    if (paras_[next_].checked) break;
    CheckPara(next_);
    changed |= !paras_[next_].errors.empty();
    next_ = (next_ + 1) % static_cast<uint32_t>(paras_.size());
  } while (pending_ > 0 && Clock::now() < until);
  if (changed || pending_ == 0) ++revision_;
  return pending_ > 0;
}

uint16_t Spelling::LangIndex(const std::wstring& tag) {
  for (size_t i = 0; i < langs_.size(); ++i)
    if (langs_[i] == tag) return static_cast<uint16_t>(i);
  langs_.push_back(tag);
  return static_cast<uint16_t>(langs_.size() - 1);
}

void Spelling::CheckPara(uint32_t para) {
  ParaState& state = paras_[para];
  state.errors.clear();
  if (!state.checked) {
    state.checked = true;
    --pending_;
  }
  const std::u16string& text = doc_.Para(para).text;
  if (text.empty()) return;

  // Split the paragraph into stretches by language; text that isn't checked ends a stretch.
  std::vector<Segment> segments;
  auto add = [&](const std::wstring& lang, char16_t c, uint32_t at) {
    if (segments.empty() || segments.back().lang != lang) segments.push_back({lang, {}, {}});
    segments.back().text.push_back(static_cast<wchar_t>(c));
    segments.back().map.push_back(at);
  };
  auto gap = [&](uint32_t at) {
    if (!segments.empty()) {
      segments.back().text.push_back(L' ');
      segments.back().map.push_back(at);
    }
  };
  for (const TextRun& r : doc_.Runs(para)) {
    if (r.fieldCode || Deleted(r.revision)) continue;
    const RunProps* props = r.run ? &doc_.ResolvedRun(para, r.run) : nullptr;
    if (props && (props->vanish || props->specVanish)) continue;
    if (props && props->noProof) {
      gap(r.start);
      continue;
    }
    // The run's language for Latin and for East Asian text, worked out once each.
    std::wstring latin, eastAsian;
    for (uint32_t i = r.start; i < r.end && i < text.size(); ++i) {
      const char16_t c = text[i];
      if (Dropped(c)) continue;
      const char16_t shown = Shown(c);
      const bool ea = IsEastAsian(c);
      std::wstring& lang = ea ? eastAsian : latin;
      if (lang.empty()) lang = checker_.Resolve(LanguageOf(doc_, para, r.run, c));
      add(lang, shown, i);
    }
  }
  for (const Segment& s : segments) {
    if (s.lang.empty() || s.text.find_first_not_of(L' ') == std::wstring::npos) continue;
    for (const auto& e : checker_.Check(s.lang, s.text)) {
      if (e.start + e.length > s.map.size() || e.length == 0) continue;
      Misspelling m;
      m.from = s.map[e.start];
      m.to = s.map[e.start + e.length - 1] + 1;
      m.action = e.action;
      m.lang = LangIndex(s.lang);
      m.replacement = e.replacement;
      state.errors.push_back(std::move(m));
    }
  }
  std::sort(state.errors.begin(), state.errors.end(), [](const Misspelling& a, const Misspelling& b) { return a.from < b.from; });
}

const std::vector<Misspelling>* Spelling::Errors(uint32_t para) const {
  return para < paras_.size() && paras_[para].checked ? &paras_[para].errors : nullptr;
}

const Misspelling* Spelling::At(Pos pos) const {
  const auto* errors = Errors(pos.para);
  if (!errors) return nullptr;
  for (const Misspelling& m : *errors)
    if (pos.offset >= m.from && pos.offset <= m.to) return &m;
  return nullptr;
}

std::optional<std::pair<uint32_t, Misspelling>> Spelling::Next(Pos pos) const {
  if (paras_.empty()) return std::nullopt;
  const uint32_t start = pos.para < paras_.size() ? pos.para : 0;
  for (size_t n = 0; n <= paras_.size(); ++n) {
    const uint32_t p = static_cast<uint32_t>((start + n) % paras_.size());
    const ParaState& s = paras_[p];
    for (const Misspelling& m : s.errors)
      if (n > 0 || m.from >= pos.offset) return std::make_pair(p, m);
  }
  return std::nullopt;
}

std::wstring Spelling::Word(uint32_t para, const Misspelling& m) const {
  std::wstring out;
  if (para >= doc_.Paragraphs().size()) return out;
  const std::u16string& text = doc_.Para(para).text;
  for (uint32_t i = m.from; i < m.to && i < text.size(); ++i)
    if (!Dropped(text[i])) out.push_back(static_cast<wchar_t>(text[i]));
  return out;
}

size_t Spelling::Count() const {
  size_t n = 0;
  for (const ParaState& s : paras_) n += s.errors.size();
  return n;
}

void Spelling::Changed(const edit::Change& change) {
  if (change.first == kNone || change.first > paras_.size()) return;
  const uint32_t removed = std::min<uint32_t>(change.removed, static_cast<uint32_t>(paras_.size()) - change.first);
  for (uint32_t i = 0; i < removed; ++i)
    if (!paras_[change.first + i].checked) --pending_;
  paras_.erase(paras_.begin() + change.first, paras_.begin() + change.first + removed);
  paras_.insert(paras_.begin() + change.first, change.added, ParaState{});
  pending_ += change.added;
  if (next_ >= paras_.size() || next_ > change.first) next_ = change.first;
  ++revision_;
}

void Spelling::Recheck(std::wstring_view word) {
  auto lower = [](std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
  };
  const std::wstring w = lower(std::wstring(word));
  for (uint32_t p = 0; p < paras_.size(); ++p) {
    ParaState& s = paras_[p];
    if (!s.checked) continue;
    for (const Misspelling& m : s.errors)
      if (lower(Word(p, m)) == w) {
        s.checked = false;
        ++pending_;
        break;
      }
  }
  ++revision_;
}

bool Spelling::HiddenByDocument(const Document& doc) {
  const Part* settings = doc.FindPart(PartKind::Settings);
  if (!settings) return false;
  std::vector<uint8_t> bytes;
  if (!doc.Package().ReadPart(settings->name, bytes).Ok()) return false;
  pkg::XmlPull pull(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  for (auto e = pull.Next(); e != pkg::XmlPull::Event::EndDocument && e != pkg::XmlPull::Event::Error; e = pull.Next()) {
    if (e != pkg::XmlPull::Event::StartElement) continue;
    if (pull.LocalName() != "hideSpellingErrors" || pull.NamespaceUri() != kW) continue;
    const auto* val = pull.FindAttribute(kW, "val");
    return !val || (val->value != "false" && val->value != "0" && val->value != "off");
  }
  return false;
}

}  // namespace fw::doc::proof
