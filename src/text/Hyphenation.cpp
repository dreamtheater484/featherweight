#include "text/Hyphenation.h"

#include <windows.h>

#include <miniz.h>

#include <algorithm>
#include <cwctype>
#include <memory>
#include <mutex>
#include <tuple>

#include "text/HyphenationData.h"

namespace fw::text {
namespace {

// The shipped pattern sets. `matches` are the language tags each covers, lower case: a tag
// matches when it equals one or starts with one followed by "-". British English comes before
// the other English tags. The minimums are hyph-utf8's for typesetting, except Spanish's left
// minimum: Word keeps at least three letters before a Spanish hyphen ("me-diante" stays whole).
struct Language {
  const wchar_t* tag;
  const char* blob;
  const wchar_t* matches[12];
  int leftMin, rightMin;
  bool latin1;  // the file is ISO 8859-1, not UTF-8
};
constexpr Language kLanguages[] = {
    {L"en-GB", "en-gb", {L"en-gb", L"en-au", L"en-nz", L"en-ie", L"en-za", L"en-in", L"en-sg", L"en-hk", L"en-mt", L"en-my", L"en-150"}, 2, 3, false},
    {L"en-US", "en-us", {L"en"}, 2, 3, false},
    {L"nl", "nl", {L"nl"}, 2, 2, true},
    {L"de", "de", {L"de"}, 2, 2, false},
    {L"fr", "fr", {L"fr"}, 2, 2, false},
    {L"es", "es", {L"es"}, 3, 2, false},
    {L"it", "it", {L"it"}, 2, 2, false},
    {L"pt", "pt", {L"pt"}, 2, 3, false},
    {L"da", "da", {L"da"}, 2, 2, false},
    {L"sv", "sv", {L"sv"}, 2, 2, false},
    {L"nb", "nb", {L"nb", L"nn", L"no"}, 2, 2, false},
    {L"fi", "fi", {L"fi"}, 2, 2, false},
};
constexpr size_t kLanguageCount = sizeof(kLanguages) / sizeof(kLanguages[0]);

bool Matches(const Language& lang, const std::wstring& tag) {
  for (const wchar_t* m : lang.matches) {
    if (!m) break;
    size_t n = wcslen(m);
    if (tag.compare(0, n, m) == 0 && (tag.size() == n || tag[n] == L'-')) return true;
  }
  return false;
}

std::wstring Lower(std::wstring_view s) {
  std::wstring out(s);
  if (!out.empty()) {
    LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.data(), static_cast<int>(s.size()), out.data(),
                  static_cast<int>(out.size()), nullptr, nullptr, 0);
  }
  return out;
}

std::unique_ptr<Hyphenator> Load(const Language& lang) {
  const detail::PatternBlob* blob = nullptr;
  for (size_t i = 0; i < detail::kPatternBlobCount; ++i) {
    if (strcmp(detail::kPatternBlobs[i].id, lang.blob) == 0) blob = &detail::kPatternBlobs[i];
  }
  if (!blob) return nullptr;
  std::string raw(blob->rawSize, '\0');
  size_t got = tinfl_decompress_mem_to_mem(raw.data(), raw.size(), blob->data, blob->size, 0);
  if (got != blob->rawSize) return nullptr;
  std::wstring text;
  if (lang.latin1) {
    text.assign(raw.begin(), raw.end());
    for (size_t i = 0; i < raw.size(); ++i) text[i] = static_cast<wchar_t>(static_cast<unsigned char>(raw[i]));
  } else {
    int n = MultiByteToWideChar(CP_UTF8, 0, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
    text.resize(static_cast<size_t>(n));
    MultiByteToWideChar(CP_UTF8, 0, raw.data(), static_cast<int>(raw.size()), text.data(), n);
  }
  return std::make_unique<Hyphenator>(lang.tag, text, lang.leftMin, lang.rightMin);
}

// A line that is a directive or an encoding name, not a pattern: capitals, digits and punctuation
// only ("ISO8859-1", "LEFTHYPHENMIN 2").
bool IsDirective(std::wstring_view line) {
  int capitals = 0;
  for (wchar_t c : line) {
    if (c >= L'A' && c <= L'Z') ++capitals;
    else if (!(c >= L'0' && c <= L'9') && c != L' ' && c != L'-' && c != L'_') return false;
  }
  return capitals >= 2;
}

// The two-letter languages of the hyphenators in Office's PROOF folders, from their file names
// (MSHY7EN.DLL, LTSHYPH_EN.LEX): "en", "es", "fr". Empty when Office isn't installed.
std::vector<std::wstring> OfficeHyphenators() {
  std::vector<std::wstring> dirs;
  wchar_t buf[MAX_PATH];
  DWORD size = sizeof(buf);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Office\\ClickToRun\\Configuration", L"InstallationPath",
                   RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
    dirs.push_back(std::wstring(buf) + L"\\root\\Office16\\PROOF");
  for (const wchar_t* var : {L"ProgramFiles", L"ProgramFiles(x86)", L"CommonProgramFiles", L"CommonProgramFiles(x86)"}) {
    DWORD n = GetEnvironmentVariableW(var, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) continue;
    const bool common = var[0] == L'C';
    dirs.push_back(std::wstring(buf, n) + (common ? L"\\Microsoft Shared\\PROOF" : L"\\Microsoft Office\\root\\Office16\\PROOF"));
  }
  std::vector<std::wstring> codes;
  for (const std::wstring& dir : dirs) {
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) continue;
    do {
      const std::wstring name = Lower(f.cFileName);
      size_t at = 0;
      if (name.rfind(L"mshy", 0) == 0) {
        at = 4;
        while (at < name.size() && name[at] >= L'0' && name[at] <= L'9') ++at;
      } else if (name.rfind(L"ltshyph_", 0) == 0) {
        at = 8;
      } else {
        continue;
      }
      const std::wstring code = name.substr(at, 2);
      if (code.size() == 2 && iswalpha(code[0]) && iswalpha(code[1]) &&
          std::find(codes.begin(), codes.end(), code) == codes.end())
        codes.push_back(code);
    } while (FindNextFileW(h, &f));
    FindClose(h);
  }
  return codes;
}

}  // namespace

bool Hyphenator::WordHyphenates(std::wstring_view language) {
  static const std::vector<std::wstring> office = OfficeHyphenators();
  if (office.empty()) return true;
  const std::wstring two = Lower(language.substr(0, 2));
  auto norwegian = [](const std::wstring& c) { return c == L"nb" || c == L"nn" || c == L"no"; };
  for (const std::wstring& c : office)
    if (c == two || (norwegian(c) && norwegian(two))) return true;
  return false;
}

Hyphenator::Hyphenator(std::wstring tag, std::wstring_view patterns, int leftMin, int rightMin)
    : tag_(std::move(tag)), leftMin_(leftMin), rightMin_(rightMin) {
  std::vector<std::tuple<std::wstring, std::string>> parsed;
  size_t pos = 0;
  while (pos < patterns.size()) {
    size_t eol = patterns.find_first_of(L"\r\n", pos);
    if (eol == std::wstring_view::npos) eol = patterns.size();
    std::wstring_view line = patterns.substr(pos, eol - pos);
    pos = eol + 1;
    if (line.empty() || line[0] == L'%' || IsDirective(line)) continue;
    size_t t = 0;
    while (t < line.size()) {
      size_t end = line.find_first_of(L" \t", t);
      if (end == std::wstring_view::npos) end = line.size();
      std::wstring_view token = line.substr(t, end - t);
      t = end + 1;
      if (token.empty() || token[0] == L'%') break;
      bool pattern = token.find_first_of(L"0123456789") != std::wstring_view::npos;
      std::wstring key;
      std::string values;
      if (pattern) {
        char pending = 0;
        for (wchar_t c : token) {
          if (c >= L'0' && c <= L'9') {
            pending = static_cast<char>(c - L'0');
          } else {
            values.push_back(pending);
            pending = 0;
            key.push_back(c);
          }
        }
        values.push_back(pending);
        if (!key.empty() && key.size() < 0xFFFF) parsed.emplace_back(Lower(key), std::move(values));
      } else {
        std::vector<uint32_t> points;
        for (wchar_t c : token) {
          if (c == L'-') points.push_back(static_cast<uint32_t>(key.size()));
          else key.push_back(c);
        }
        exceptions_.emplace_back(Lower(key), std::move(points));
      }
    }
  }
  std::stable_sort(parsed.begin(), parsed.end(),
                   [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });
  for (size_t i = 0; i < parsed.size(); ++i) {
    // A pattern repeated later replaces the earlier one.
    if (i + 1 < parsed.size() && std::get<0>(parsed[i]) == std::get<0>(parsed[i + 1])) continue;
    const auto& [key, values] = parsed[i];
    entries_.push_back({static_cast<uint32_t>(keys_.size()), static_cast<uint32_t>(values_.size()),
                        static_cast<uint16_t>(key.size())});
    keys_ += key;
    values_ += values;
    longest_ = std::max(longest_, key.size());
  }
  std::sort(exceptions_.begin(), exceptions_.end());
}

const Hyphenator::Entry* Hyphenator::Find(std::wstring_view key) const {
  auto it = std::lower_bound(entries_.begin(), entries_.end(), key,
                             [this](const Entry& e, std::wstring_view k) { return Key(e) < k; });
  return it != entries_.end() && Key(*it) == key ? &*it : nullptr;
}

std::vector<uint32_t> Hyphenator::Points(std::wstring_view word, int leftMin, int rightMin) const {
  if (leftMin <= 0) leftMin = leftMin_;
  if (rightMin <= 0) rightMin = rightMin_;
  std::vector<uint32_t> points;
  const int n = static_cast<int>(word.size());
  if (n < leftMin + rightMin) return points;
  std::wstring lower = Lower(word);
  auto keep = [&](uint32_t p) { return static_cast<int>(p) >= leftMin && n - static_cast<int>(p) >= rightMin; };

  auto ex = std::lower_bound(exceptions_.begin(), exceptions_.end(), lower,
                             [](const auto& e, const std::wstring& w) { return e.first < w; });
  if (ex != exceptions_.end() && ex->first == lower) {
    for (uint32_t p : ex->second)
      if (keep(p)) points.push_back(p);
    return points;
  }

  std::wstring w = L"." + lower + L".";
  std::vector<char> values(w.size() + 1, 0);
  for (size_t i = 0; i < w.size(); ++i) {
    size_t longest = std::min(longest_, w.size() - i);
    for (size_t len = 1; len <= longest; ++len) {
      const Entry* e = Find(std::wstring_view(w).substr(i, len));
      if (!e) continue;
      for (size_t k = 0; k <= len; ++k) values[i + k] = std::max(values[i + k], values_[e->values + k]);
    }
  }
  // A hyphen between word[p - 1] and word[p] is between w[p] and w[p + 1].
  for (int p = leftMin; p <= n - rightMin; ++p) {
    if (values[static_cast<size_t>(p) + 1] & 1) points.push_back(static_cast<uint32_t>(p));
  }
  return points;
}

const Hyphenator* Hyphenator::For(std::wstring_view language) {
  static std::mutex mutex;
  static std::unique_ptr<Hyphenator> loaded[kLanguageCount];
  static bool tried[kLanguageCount] = {};
  std::wstring tag = Lower(language);
  for (auto& c : tag)
    if (c == L'_') c = L'-';
  for (size_t i = 0; i < kLanguageCount; ++i) {
    if (!Matches(kLanguages[i], tag)) continue;
    std::lock_guard<std::mutex> lock(mutex);
    if (!tried[i]) {
      tried[i] = true;
      loaded[i] = Load(kLanguages[i]);
    }
    return loaded[i].get();
  }
  return nullptr;
}

std::vector<std::wstring> Hyphenator::Languages() {
  std::vector<std::wstring> tags;
  for (const auto& lang : kLanguages) tags.emplace_back(lang.tag);
  return tags;
}

}  // namespace fw::text
