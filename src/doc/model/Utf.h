#pragma once

#include <string>
#include <string_view>

namespace fw::doc {

// UTF-8 <-> UTF-16 for paragraph text. Input that isn't valid UTF-8 becomes U+FFFD (XmlPull has
// already checked every part, so that only happens with bytes from elsewhere).
inline void AppendUtf16(std::u16string& out, std::string_view utf8) {
  for (size_t i = 0; i < utf8.size();) {
    auto b = static_cast<unsigned char>(utf8[i]);
    char32_t cp = 0xFFFD;
    size_t n = 1;
    if (b < 0x80) {
      cp = b;
    } else if ((b >> 5) == 0x6 && i + 1 < utf8.size()) {
      cp = ((b & 0x1F) << 6) | (utf8[i + 1] & 0x3F);
      n = 2;
    } else if ((b >> 4) == 0xE && i + 2 < utf8.size()) {
      cp = ((b & 0x0F) << 12) | ((utf8[i + 1] & 0x3F) << 6) | (utf8[i + 2] & 0x3F);
      n = 3;
    } else if ((b >> 3) == 0x1E && i + 3 < utf8.size()) {
      cp = ((b & 0x07) << 18) | ((utf8[i + 1] & 0x3F) << 12) | ((utf8[i + 2] & 0x3F) << 6) | (utf8[i + 3] & 0x3F);
      n = 4;
    }
    i += n;
    if (cp >= 0x10000 && cp <= 0x10FFFF) {
      cp -= 0x10000;
      out += static_cast<char16_t>(0xD800 + (cp >> 10));
      out += static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
    } else {
      out += static_cast<char16_t>(cp > 0x10FFFF ? 0xFFFD : cp);
    }
  }
}

inline void AppendUtf8(std::string& out, std::u16string_view utf16) {
  for (size_t i = 0; i < utf16.size(); ++i) {
    char32_t cp = utf16[i];
    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < utf16.size() && utf16[i + 1] >= 0xDC00 && utf16[i + 1] < 0xE000) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (utf16[i + 1] - 0xDC00);
      ++i;
    } else if (cp >= 0xD800 && cp < 0xE000) {
      cp = 0xFFFD;
    }
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
}

inline std::string ToUtf8(std::u16string_view utf16) {
  std::string out;
  AppendUtf8(out, utf16);
  return out;
}

inline std::u16string ToUtf16(std::string_view utf8) {
  std::u16string out;
  AppendUtf16(out, utf8);
  return out;
}

}  // namespace fw::doc
