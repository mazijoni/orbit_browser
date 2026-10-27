#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orbit {

// A small, strict JSON reader (RFC 8259) for files Orbit imports from other
// browsers (Chromium's Bookmarks and Local State, Firefox's logins.json).
// Strings are UTF-8 (\u escapes, surrogate pairs included); nesting is capped.
struct JsonValue {
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };
  Type type = Type::kNull;
  bool boolean = false;
  double number = 0.0;
  std::string string;
  std::vector<JsonValue> array;
  std::vector<std::pair<std::string, JsonValue>> object;

  // A member of an object (null for anything else or a missing key).
  const JsonValue* Get(std::string_view key) const;
  // The member's string, or empty.
  const std::string& Str(std::string_view key) const;
  bool is_object() const { return type == Type::kObject; }
  bool is_array() const { return type == Type::kArray; }
  bool is_string() const { return type == Type::kString; }
};

std::optional<JsonValue> ParseJson(std::string_view text, int max_depth = 96);

// |text| as a JSON string literal, quotes included (control characters and
// quotes escaped; UTF-8 passes through).
std::string JsonQuote(std::string_view text);
// |value| written back as JSON (numbers in their shortest round-trip form).
std::string ToJson(const JsonValue& value);

}  // namespace orbit
