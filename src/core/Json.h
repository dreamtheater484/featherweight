#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fw {

// Just enough JSON to read GitHub release metadata.
struct JsonValue {
  enum class Type { Null, Bool, Number, String, Array, Object };
  Type type = Type::Null;
  bool boolean = false;
  double number = 0;
  std::string string;  // UTF-8
  std::vector<JsonValue> array;
  std::vector<std::pair<std::string, JsonValue>> object;

  const JsonValue* Find(std::string_view key) const;
  std::string StringOr(std::string_view key, std::string_view fallback = {}) const;
};

std::optional<JsonValue> ParseJson(std::string_view text);

}  // namespace fw
