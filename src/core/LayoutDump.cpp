#include "core/LayoutDump.h"

#include <charconv>
#include <cmath>

#include "core/Json.h"
#include "core/Strings.h"

namespace fw {
namespace {

constexpr std::string_view kFormat = "featherweight-layout";
constexpr std::string_view kStoryNames[] = {"body", "header", "footer", "footnote", "endnote", "cell", "textbox"};

// Points with at most two decimals, without trailing zeros, independent of the locale.
void AppendNumber(std::string& out, double v) {
  if (!std::isfinite(v)) v = 0;
  v = std::round(v * 100) / 100;
  if (v == 0) v = 0;  // no "-0"
  char buf[32];
  auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::fixed, 2);
  std::string_view s(buf, ec == std::errc() ? static_cast<size_t>(end - buf) : 0);
  while (!s.empty() && s.back() == '0') s.remove_suffix(1);
  if (!s.empty() && s.back() == '.') s.remove_suffix(1);
  out += s.empty() ? std::string_view("0") : s;
}

void AppendString(std::string& out, std::string_view utf8) {
  out += '"';
  for (char c : utf8) {
    unsigned char u = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (u < 0x20) {
      static const char kHex[] = "0123456789abcdef";
      out += "\\u00";
      out += kHex[u >> 4];
      out += kHex[u & 15];
    } else {
      out += c;
    }
  }
  out += '"';
}

double NumberOr(const JsonValue& v, std::string_view key, double fallback = 0) {
  const JsonValue* n = v.Find(key);
  return n && n->type == JsonValue::Type::Number ? n->number : fallback;
}

std::optional<LayoutDump> Fail(std::string* error, std::string message) {
  if (error) *error = std::move(message);
  return std::nullopt;
}

}  // namespace

std::string_view StoryName(LayoutStory story) { return kStoryNames[static_cast<int>(story)]; }

std::optional<LayoutStory> StoryFromName(std::string_view name) {
  for (int i = 0; i < static_cast<int>(std::size(kStoryNames)); ++i)
    if (kStoryNames[i] == name) return static_cast<LayoutStory>(i);
  return std::nullopt;
}

std::string WriteLayoutDump(const LayoutDump& dump) {
  std::string out = "{\n  \"format\": ";
  AppendString(out, kFormat);
  out += ",\n  \"version\": " + std::to_string(LayoutDump::kVersion) + ",\n  \"source\": ";
  AppendString(out, dump.source);
  out += ",\n  \"producer\": ";
  AppendString(out, dump.producer);
  out += ",\n  \"pages\": [";
  for (size_t p = 0; p < dump.pages.size(); ++p) {
    const LayoutPage& page = dump.pages[p];
    out += p ? ",\n    {\"width\": " : "\n    {\"width\": ";
    AppendNumber(out, page.width);
    out += ", \"height\": ";
    AppendNumber(out, page.height);
    out += ", \"lines\": [";
    for (size_t i = 0; i < page.lines.size(); ++i) {
      const LayoutLine& line = page.lines[i];
      out += i ? ",\n      {\"story\": " : "\n      {\"story\": ";
      AppendString(out, StoryName(line.story));
      out += ", \"x\": ";
      AppendNumber(out, line.x);
      out += ", \"baseline\": ";
      AppendNumber(out, line.baseline);
      out += ", \"width\": ";
      AppendNumber(out, line.width);
      out += ", \"size\": ";
      AppendNumber(out, line.size);
      out += ", \"text\": ";
      AppendString(out, WideToUtf8(line.text));
      out += '}';
    }
    out += page.lines.empty() ? "]}" : "\n    ]}";
  }
  out += dump.pages.empty() ? "]\n}\n" : "\n  ]\n}\n";
  return out;
}

std::optional<LayoutDump> ReadLayoutDump(std::string_view json, std::string* error) {
  auto root = ParseJson(json);
  if (!root || root->type != JsonValue::Type::Object) return Fail(error, "not JSON");
  if (root->StringOr("format") != kFormat) return Fail(error, "not a layout dump (no \"format\": \"featherweight-layout\")");
  double version = NumberOr(*root, "version");
  if (version < 1 || version > LayoutDump::kVersion)
    return Fail(error, "layout dump version " + std::to_string(static_cast<int>(version)) + "; this build reads up to " +
                           std::to_string(LayoutDump::kVersion));
  LayoutDump dump;
  dump.source = root->StringOr("source");
  dump.producer = root->StringOr("producer");
  const JsonValue* pages = root->Find("pages");
  if (!pages || pages->type != JsonValue::Type::Array) return Fail(error, "no \"pages\" array");
  for (const JsonValue& p : pages->array) {
    if (p.type != JsonValue::Type::Object) return Fail(error, "a page isn't an object");
    LayoutPage page;
    page.width = NumberOr(p, "width");
    page.height = NumberOr(p, "height");
    if (const JsonValue* lines = p.Find("lines"); lines && lines->type == JsonValue::Type::Array) {
      for (const JsonValue& l : lines->array) {
        const JsonValue* text = l.Find("text");
        if (!text || text->type != JsonValue::Type::String)
          return Fail(error, "a line on page " + std::to_string(dump.pages.size() + 1) + " has no text");
        auto story = StoryFromName(l.StringOr("story", "body"));
        if (!story)
          return Fail(error, "unknown story \"" + l.StringOr("story") + "\" on page " +
                                 std::to_string(dump.pages.size() + 1));
        LayoutLine line;
        line.story = *story;
        line.x = NumberOr(l, "x");
        line.baseline = NumberOr(l, "baseline");
        line.width = NumberOr(l, "width");
        line.size = NumberOr(l, "size");
        line.text = Utf8ToWide(text->string);
        page.lines.push_back(std::move(line));
      }
    }
    dump.pages.push_back(std::move(page));
  }
  return dump;
}

}  // namespace fw
