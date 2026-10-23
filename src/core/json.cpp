#include "core/json.h"

#include <cstdlib>

namespace orbit {
namespace {

class Parser {
 public:
  Parser(std::string_view text, int max_depth) : s_(text), max_depth_(max_depth) {}

  std::optional<JsonValue> Run() {
    // A UTF-8 byte order mark is tolerated.
    if (s_.size() >= 3 && static_cast<unsigned char>(s_[0]) == 0xEF &&
        static_cast<unsigned char>(s_[1]) == 0xBB && static_cast<unsigned char>(s_[2]) == 0xBF) {
      i_ = 3;
    }
    JsonValue v;
    if (!Value(v, 0)) return std::nullopt;
    Space();
    if (i_ != s_.size()) return std::nullopt;
    return v;
  }

 private:
  void Space() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
  }
  bool Literal(std::string_view word) {
    if (s_.substr(i_, word.size()) != word) return false;
    i_ += word.size();
    return true;
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
  bool Hex4(unsigned& out) {
    if (i_ + 4 > s_.size()) return false;
    out = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = s_[i_++];
      out <<= 4;
      if (c >= '0' && c <= '9') out |= static_cast<unsigned>(c - '0');
      else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned>(c - 'A' + 10);
      else return false;
    }
    return true;
  }
  bool String(std::string& out) {
    if (i_ >= s_.size() || s_[i_] != '"') return false;
    ++i_;
    while (i_ < s_.size()) {
      const char c = s_[i_++];
      if (c == '"') return true;
      if (static_cast<unsigned char>(c) < 0x20) return false;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (i_ >= s_.size()) return false;
      const char e = s_[i_++];
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
          unsigned cp = 0;
          if (!Hex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            unsigned low = 0;
            if (i_ + 2 <= s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
              i_ += 2;
              if (!Hex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
              cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            } else {
              cp = 0xFFFD;  // a lone surrogate
            }
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;
          }
          AppendUtf8(out, cp);
          break;
        }
        default:
          return false;
      }
    }
    return false;
  }
  bool Number(double& out) {
    const size_t start = i_;
    if (i_ < s_.size() && s_[i_] == '-') ++i_;
    if (i_ >= s_.size() || s_[i_] < '0' || s_[i_] > '9') return false;
    while (i_ < s_.size() && ((s_[i_] >= '0' && s_[i_] <= '9') || s_[i_] == '.' || s_[i_] == 'e' ||
                              s_[i_] == 'E' || s_[i_] == '+' || s_[i_] == '-')) {
      ++i_;
    }
    const std::string text(s_.substr(start, i_ - start));
    char* end = nullptr;
    out = std::strtod(text.c_str(), &end);
    return end && *end == '\0';
  }
  bool Value(JsonValue& v, int depth) {
    if (depth > max_depth_) return false;
    Space();
    if (i_ >= s_.size()) return false;
    const char c = s_[i_];
    if (c == '{') {
      ++i_;
      v.type = JsonValue::Type::kObject;
      Space();
      if (i_ < s_.size() && s_[i_] == '}') {
        ++i_;
        return true;
      }
      while (true) {
        Space();
        std::string key;
        if (!String(key)) return false;
        Space();
        if (i_ >= s_.size() || s_[i_++] != ':') return false;
        JsonValue member;
        if (!Value(member, depth + 1)) return false;
        v.object.emplace_back(std::move(key), std::move(member));
        Space();
        if (i_ >= s_.size()) return false;
        if (s_[i_] == ',') {
          ++i_;
          continue;
        }
        if (s_[i_] == '}') {
          ++i_;
          return true;
        }
        return false;
      }
    }
    if (c == '[') {
      ++i_;
      v.type = JsonValue::Type::kArray;
      Space();
      if (i_ < s_.size() && s_[i_] == ']') {
        ++i_;
        return true;
      }
      while (true) {
        JsonValue item;
        if (!Value(item, depth + 1)) return false;
        v.array.push_back(std::move(item));
        Space();
        if (i_ >= s_.size()) return false;
        if (s_[i_] == ',') {
          ++i_;
          continue;
        }
        if (s_[i_] == ']') {
          ++i_;
          return true;
        }
        return false;
      }
    }
    if (c == '"') {
      v.type = JsonValue::Type::kString;
      return String(v.string);
    }
    if (Literal("true")) {
      v.type = JsonValue::Type::kBool;
      v.boolean = true;
      return true;
    }
    if (Literal("false")) {
      v.type = JsonValue::Type::kBool;
      return true;
    }
    if (Literal("null")) return true;
    v.type = JsonValue::Type::kNumber;
    return Number(v.number);
  }

  std::string_view s_;
  size_t i_ = 0;
  int max_depth_;
};

const std::string kEmpty;

}  // namespace

const JsonValue* JsonValue::Get(std::string_view key) const {
  if (type != Type::kObject) return nullptr;
  for (const auto& [k, v] : object) {
    if (k == key) return &v;
  }
  return nullptr;
}

const std::string& JsonValue::Str(std::string_view key) const {
  const JsonValue* v = Get(key);
  return v && v->type == Type::kString ? v->string : kEmpty;
}

std::optional<JsonValue> ParseJson(std::string_view text, int max_depth) {
  return Parser(text, max_depth).Run();
}

}  // namespace orbit
