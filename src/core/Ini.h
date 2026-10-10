#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fw {

// A small INI document that keeps section and key order, so files stay readable.
// Keys are case-insensitive; values are stored as-is (UTF-8 on disk).
class Ini {
 public:
  using Entry = std::pair<std::wstring, std::wstring>;

  void Parse(std::string_view utf8);
  std::string Serialize() const;

  std::wstring Get(std::wstring_view section, std::wstring_view key, std::wstring_view fallback = L"") const;
  int GetInt(std::wstring_view section, std::wstring_view key, int fallback) const;
  long long GetInt64(std::wstring_view section, std::wstring_view key, long long fallback) const;
  bool GetBool(std::wstring_view section, std::wstring_view key, bool fallback) const;
  bool Has(std::wstring_view section, std::wstring_view key) const;

  void Set(std::wstring_view section, std::wstring_view key, std::wstring_view value);
  void SetInt(std::wstring_view section, std::wstring_view key, long long value);
  void SetBool(std::wstring_view section, std::wstring_view key, bool value);
  void Remove(std::wstring_view section, std::wstring_view key);

  std::vector<Entry> Entries(std::wstring_view section) const;
  void ReplaceSection(std::wstring_view section, const std::vector<Entry>& entries);

 private:
  struct Section {
    std::wstring name;
    std::vector<Entry> entries;
  };
  const Section* Find(std::wstring_view name) const;
  Section& FindOrAdd(std::wstring_view name);

  std::vector<Section> sections_;
};

}  // namespace fw
