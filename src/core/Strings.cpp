#include "core/Strings.h"

#include <windows.h>

#include <cmath>
#include <cwctype>

namespace fw {

std::wstring Utf8ToWide(std::string_view s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

std::string WideToUtf8(std::wstring_view s) {
  if (s.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
  return out;
}

std::wstring ToLower(std::wstring_view s) {
  std::wstring out(s);
  if (!out.empty()) CharLowerBuffW(out.data(), static_cast<DWORD>(out.size()));
  return out;
}

std::wstring Trim(std::wstring_view s) {
  size_t a = 0, b = s.size();
  while (a < b && iswspace(s[a])) ++a;
  while (b > a && iswspace(s[b - 1])) --b;
  return std::wstring(s.substr(a, b - a));
}

std::string Trim(std::string_view s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
  return std::string(s.substr(a, b - a));
}

std::vector<std::wstring> Split(std::wstring_view s, wchar_t sep) {
  std::vector<std::wstring> parts;
  size_t start = 0;
  for (;;) {
    size_t pos = s.find(sep, start);
    if (pos == std::wstring_view::npos) {
      parts.emplace_back(s.substr(start));
      break;
    }
    parts.emplace_back(s.substr(start, pos - start));
    start = pos + 1;
  }
  return parts;
}

bool EqualsNoCase(std::wstring_view a, std::wstring_view b) {
  return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
         CSTR_EQUAL;
}

bool EndsWithNoCase(std::wstring_view s, std::wstring_view suffix) {
  return s.size() >= suffix.size() && EqualsNoCase(s.substr(s.size() - suffix.size()), suffix);
}

std::wstring FileNameOf(std::wstring_view path) {
  size_t pos = path.find_last_of(L"\\/");
  return std::wstring(pos == std::wstring_view::npos ? path : path.substr(pos + 1));
}

std::wstring DirOf(std::wstring_view path) {
  size_t pos = path.find_last_of(L"\\/");
  return pos == std::wstring_view::npos ? std::wstring() : std::wstring(path.substr(0, pos));
}

uint64_t Fnv1a64(std::wstring_view s) {
  uint64_t h = 1469598103934665603ull;
  for (wchar_t c : s) {
    h ^= static_cast<uint16_t>(c) & 0xFF;
    h *= 1099511628211ull;
    h ^= static_cast<uint16_t>(c) >> 8;
    h *= 1099511628211ull;
  }
  return h;
}

std::wstring Hex64(uint64_t v) {
  wchar_t buf[17];
  static const wchar_t* digits = L"0123456789abcdef";
  for (int i = 15; i >= 0; --i) {
    buf[i] = digits[v & 0xF];
    v >>= 4;
  }
  buf[16] = 0;
  return buf;
}

std::wstring FormatPercent(double factor) {
  double pct = factor * 100.0;
  if (pct < 10.0) return std::to_wstring(static_cast<int>(std::lround(pct * 10)) / 10) + L"%";
  return std::to_wstring(static_cast<int>(std::lround(pct))) + L"%";
}

std::wstring RelativeTime(int64_t secondsAgo) {
  if (secondsAgo < 60) return L"Just now";
  int64_t minutes = secondsAgo / 60;
  if (minutes < 60) return minutes == 1 ? L"1 minute ago" : std::to_wstring(minutes) + L" minutes ago";
  int64_t hours = minutes / 60;
  if (hours < 24) return hours == 1 ? L"1 hour ago" : std::to_wstring(hours) + L" hours ago";
  int64_t days = hours / 24;
  if (days == 1) return L"Yesterday";
  if (days < 7) return std::to_wstring(days) + L" days ago";
  int64_t weeks = days / 7;
  if (days < 31) return weeks == 1 ? L"Last week" : std::to_wstring(weeks) + L" weeks ago";
  int64_t months = days / 30;
  if (days < 365) return months <= 1 ? L"Last month" : std::to_wstring(months) + L" months ago";
  int64_t years = days / 365;
  return years == 1 ? L"Last year" : std::to_wstring(years) + L" years ago";
}

bool ParseInt(std::wstring_view s, int& out) {
  std::wstring t = Trim(s);
  if (t.empty() || t.size() > 9) return false;
  int v = 0;
  for (wchar_t c : t) {
    if (c < L'0' || c > L'9') return false;
    v = v * 10 + (c - L'0');
  }
  out = v;
  return true;
}

}  // namespace fw
