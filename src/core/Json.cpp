#include "core/Json.h"

#include <cstdlib>

namespace fw {
namespace {

class Parser {
 public:
  explicit Parser(std::string_view s) : s_(s) {}

  bool ParseDocument(JsonValue& out) {
    if (!ParseValue(out, 0)) return false;
    SkipSpace();
    return pos_ == s_.size();
  }

 private:
  static constexpr int kMaxDepth = 64;

  void SkipSpace() {
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
  }

  bool Consume(char c) {
    SkipSpace();
    if (pos_ < s_.size() && s_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  bool Literal(std::string_view word) {
    if (s_.substr(pos_, word.size()) != word) return false;
    pos_ += word.size();
    return true;
  }

  bool ParseValue(JsonValue& v, int depth) {
    if (depth > kMaxDepth) return false;
    SkipSpace();
    if (pos_ >= s_.size()) return false;
    char c = s_[pos_];
    if (c == '{') return ParseObject(v, depth);
    if (c == '[') return ParseArray(v, depth);
    if (c == '"') {
      v.type = JsonValue::Type::String;
      return ParseString(v.string);
    }
    if (c == 't') {
      v.type = JsonValue::Type::Bool;
      v.boolean = true;
      return Literal("true");
    }
    if (c == 'f') {
      v.type = JsonValue::Type::Bool;
      v.boolean = false;
      return Literal("false");
    }
    if (c == 'n') {
      v.type = JsonValue::Type::Null;
      return Literal("null");
    }
    return ParseNumber(v);
  }

  bool ParseObject(JsonValue& v, int depth) {
    v.type = JsonValue::Type::Object;
    ++pos_;
    if (Consume('}')) return true;
    for (;;) {
      SkipSpace();
      std::string key;
      if (pos_ >= s_.size() || s_[pos_] != '"' || !ParseString(key)) return false;
      if (!Consume(':')) return false;
      JsonValue child;
      if (!ParseValue(child, depth + 1)) return false;
      v.object.emplace_back(std::move(key), std::move(child));
      if (Consume(',')) continue;
      return Consume('}');
    }
  }

  bool ParseArray(JsonValue& v, int depth) {
    v.type = JsonValue::Type::Array;
    ++pos_;
    if (Consume(']')) return true;
    for (;;) {
      JsonValue child;
      if (!ParseValue(child, depth + 1)) return false;
      v.array.push_back(std::move(child));
      if (Consume(',')) continue;
      return Consume(']');
    }
  }

  static void AppendUtf8(std::string& out, unsigned cp) {
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

  bool ParseHex4(unsigned& out) {
    if (pos_ + 4 > s_.size()) return false;
    out = 0;
    for (int i = 0; i < 4; ++i) {
      char c = s_[pos_++];
      out <<= 4;
      if (c >= '0' && c <= '9') out |= c - '0';
      else if (c >= 'a' && c <= 'f') out |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') out |= c - 'A' + 10;
      else return false;
    }
    return true;
  }

  bool ParseString(std::string& out) {
    ++pos_;  // opening quote
    while (pos_ < s_.size()) {
      char c = s_[pos_++];
      if (c == '"') return true;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (pos_ >= s_.size()) return false;
      char e = s_[pos_++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          unsigned cp;
          if (!ParseHex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
            pos_ += 2;
            unsigned lo;
            if (!ParseHex4(lo)) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          AppendUtf8(out, cp);
          break;
        }
        default: return false;
      }
    }
    return false;
  }

  bool ParseNumber(JsonValue& v) {
    size_t start = pos_;
    while (pos_ < s_.size()) {
      char c = s_[pos_];
      if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') ++pos_;
      else break;
    }
    if (pos_ == start) return false;
    std::string num(s_.substr(start, pos_ - start));
    char* end = nullptr;
    v.type = JsonValue::Type::Number;
    v.number = strtod(num.c_str(), &end);
    return end && *end == 0;
  }

  std::string_view s_;
  size_t pos_ = 0;
};

}  // namespace

const JsonValue* JsonValue::Find(std::string_view key) const {
  if (type != Type::Object) return nullptr;
  for (const auto& kv : object)
    if (kv.first == key) return &kv.second;
  return nullptr;
}

std::string JsonValue::StringOr(std::string_view key, std::string_view fallback) const {
  const JsonValue* v = Find(key);
  return (v && v->type == Type::String) ? v->string : std::string(fallback);
}

std::optional<JsonValue> ParseJson(std::string_view text) {
  JsonValue v;
  Parser p(text);
  if (!p.ParseDocument(v)) return std::nullopt;
  return v;
}

}  // namespace fw
