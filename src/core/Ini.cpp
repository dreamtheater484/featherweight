#include "core/Ini.h"

#include "core/Strings.h"

namespace fw {

void Ini::Parse(std::string_view utf8) {
  sections_.clear();
  if (utf8.size() >= 3 && static_cast<unsigned char>(utf8[0]) == 0xEF && static_cast<unsigned char>(utf8[1]) == 0xBB &&
      static_cast<unsigned char>(utf8[2]) == 0xBF) {
    utf8.remove_prefix(3);
  }
  Section* current = &FindOrAdd(L"");
  size_t pos = 0;
  while (pos <= utf8.size()) {
    size_t end = utf8.find('\n', pos);
    if (end == std::string_view::npos) end = utf8.size();
    std::string line = Trim(utf8.substr(pos, end - pos));
    pos = end + 1;
    if (line.empty() || line[0] == ';' || line[0] == '#') continue;
    if (line.front() == '[' && line.back() == ']') {
      current = &FindOrAdd(Utf8ToWide(Trim(std::string_view(line).substr(1, line.size() - 2))));
      continue;
    }
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::wstring key = Utf8ToWide(Trim(std::string_view(line).substr(0, eq)));
    std::wstring value = Utf8ToWide(Trim(std::string_view(line).substr(eq + 1)));
    if (key.empty()) continue;
    bool replaced = false;
    for (auto& e : current->entries) {
      if (EqualsNoCase(e.first, key)) {
        e.second = value;
        replaced = true;
        break;
      }
    }
    if (!replaced) current->entries.emplace_back(std::move(key), std::move(value));
  }
}

std::string Ini::Serialize() const {
  std::string out;
  for (const auto& s : sections_) {
    if (s.entries.empty()) continue;
    if (!s.name.empty()) {
      if (!out.empty()) out += "\r\n";
      out += "[" + WideToUtf8(s.name) + "]\r\n";
    }
    for (const auto& e : s.entries) out += WideToUtf8(e.first) + "=" + WideToUtf8(e.second) + "\r\n";
  }
  return out;
}

const Ini::Section* Ini::Find(std::wstring_view name) const {
  for (const auto& s : sections_)
    if (EqualsNoCase(s.name, name)) return &s;
  return nullptr;
}

Ini::Section& Ini::FindOrAdd(std::wstring_view name) {
  for (auto& s : sections_)
    if (EqualsNoCase(s.name, name)) return s;
  sections_.push_back(Section{std::wstring(name), {}});
  return sections_.back();
}

std::wstring Ini::Get(std::wstring_view section, std::wstring_view key, std::wstring_view fallback) const {
  if (const Section* s = Find(section))
    for (const auto& e : s->entries)
      if (EqualsNoCase(e.first, key)) return e.second;
  return std::wstring(fallback);
}

bool Ini::Has(std::wstring_view section, std::wstring_view key) const {
  if (const Section* s = Find(section))
    for (const auto& e : s->entries)
      if (EqualsNoCase(e.first, key)) return true;
  return false;
}

int Ini::GetInt(std::wstring_view section, std::wstring_view key, int fallback) const {
  return static_cast<int>(GetInt64(section, key, fallback));
}

long long Ini::GetInt64(std::wstring_view section, std::wstring_view key, long long fallback) const {
  std::wstring v = Get(section, key);
  if (v.empty()) return fallback;
  wchar_t* end = nullptr;
  long long r = wcstoll(v.c_str(), &end, 10);
  return (end && *end == 0) ? r : fallback;
}

bool Ini::GetBool(std::wstring_view section, std::wstring_view key, bool fallback) const {
  std::wstring v = ToLower(Get(section, key));
  if (v == L"1" || v == L"true" || v == L"yes" || v == L"on") return true;
  if (v == L"0" || v == L"false" || v == L"no" || v == L"off") return false;
  return fallback;
}

void Ini::Set(std::wstring_view section, std::wstring_view key, std::wstring_view value) {
  Section& s = FindOrAdd(section);
  for (auto& e : s.entries) {
    if (EqualsNoCase(e.first, key)) {
      e.second = value;
      return;
    }
  }
  s.entries.emplace_back(std::wstring(key), std::wstring(value));
}

void Ini::SetInt(std::wstring_view section, std::wstring_view key, long long value) {
  Set(section, key, std::to_wstring(value));
}

void Ini::SetBool(std::wstring_view section, std::wstring_view key, bool value) {
  Set(section, key, value ? L"1" : L"0");
}

void Ini::Remove(std::wstring_view section, std::wstring_view key) {
  for (auto& s : sections_) {
    if (!EqualsNoCase(s.name, section)) continue;
    for (auto it = s.entries.begin(); it != s.entries.end(); ++it) {
      if (EqualsNoCase(it->first, key)) {
        s.entries.erase(it);
        return;
      }
    }
  }
}

std::vector<Ini::Entry> Ini::Entries(std::wstring_view section) const {
  if (const Section* s = Find(section)) return s->entries;
  return {};
}

void Ini::ReplaceSection(std::wstring_view section, const std::vector<Entry>& entries) {
  FindOrAdd(section).entries = entries;
}

}  // namespace fw
