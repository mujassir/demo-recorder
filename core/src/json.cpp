#include "dr/json.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <system_error>

#if !(defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L)
#if defined(_WIN32)
#include <locale.h>
#else
#include <locale.h>
#if defined(__APPLE__)
#include <xlocale.h>
#endif
#endif
#endif

namespace dr::json {

// ---------------------------------------------------------------------------
// Value

std::optional<bool> Value::as_bool() const {
  if (auto* b = std::get_if<bool>(&v_)) return *b;
  return std::nullopt;
}

std::optional<int64_t> Value::as_int() const {
  if (auto* i = std::get_if<int64_t>(&v_)) return *i;
  if (auto* d = std::get_if<double>(&v_)) {
    // Only accept doubles that are exactly integral and in range.
    if (std::isfinite(*d) && std::floor(*d) == *d && *d >= -9.2233720368547758e18 &&
        *d < 9.2233720368547758e18) {
      return static_cast<int64_t>(*d);
    }
  }
  return std::nullopt;
}

std::optional<double> Value::as_double() const {
  if (auto* d = std::get_if<double>(&v_)) return *d;
  if (auto* i = std::get_if<int64_t>(&v_)) return static_cast<double>(*i);
  return std::nullopt;
}

const std::string* Value::as_string() const { return std::get_if<std::string>(&v_); }
const Array* Value::as_array() const { return std::get_if<Array>(&v_); }
Array* Value::as_array() { return std::get_if<Array>(&v_); }
const Object* Value::as_object() const { return std::get_if<Object>(&v_); }
Object* Value::as_object() { return std::get_if<Object>(&v_); }

const Value* Value::find(std::string_view key) const {
  const Object* o = as_object();
  if (!o) return nullptr;
  for (const auto& [k, v] : *o) {
    if (k == key) return &v;
  }
  return nullptr;
}

Value& Value::set(std::string_view key, Value v) {
  if (is_null()) v_ = Object{};
  Object& o = std::get<Object>(v_);
  for (auto& [k, existing] : o) {
    if (k == key) {
      existing = std::move(v);
      return existing;
    }
  }
  o.emplace_back(std::string(key), std::move(v));
  return o.back().second;
}

Value& Value::push_back(Value v) {
  if (is_null()) v_ = Array{};
  Array& a = std::get<Array>(v_);
  a.push_back(std::move(v));
  return a.back();
}

// ---------------------------------------------------------------------------
// Number parsing

std::optional<double> parse_double(std::string_view s) {
  if (s.empty()) return std::nullopt;
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
  double d = 0;
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), d);
  if (ec != std::errc() || ptr != s.data() + s.size()) return std::nullopt;
  return d;
#else
  // strtod is locale-dependent; use an explicit "C" locale.
  std::string tmp(s);
#if defined(_WIN32)
  static _locale_t c_locale = _create_locale(LC_NUMERIC, "C");
  char* end = nullptr;
  double d = _strtod_l(tmp.c_str(), &end, c_locale);
#else
  static locale_t c_locale = newlocale(LC_NUMERIC_MASK, "C", locale_t{});
  char* end = nullptr;
  double d = strtod_l(tmp.c_str(), &end, c_locale);
#endif
  if (end != tmp.c_str() + tmp.size()) return std::nullopt;
  return d;
#endif
}

// ---------------------------------------------------------------------------
// Parser

namespace {

class Parser {
 public:
  Parser(std::string_view text, const ParseOptions& options) : s_(text), opt_(options) {}

  std::optional<Value> run(ParseError* error) {
    skip_ws();
    Value v;
    if (!parse_value(v, 0)) return fail_out(error);
    skip_ws();
    if (pos_ != s_.size()) {
      set_error("unexpected trailing characters");
      return fail_out(error);
    }
    return v;
  }

 private:
  std::optional<Value> fail_out(ParseError* error) {
    if (error) *error = err_;
    return std::nullopt;
  }

  bool set_error(const char* msg) {
    if (err_.message.empty()) {
      err_.offset = pos_;
      err_.message = msg;
    }
    return false;
  }

  void skip_ws() {
    while (pos_ < s_.size()) {
      char c = s_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool consume_literal(std::string_view lit) {
    if (s_.substr(pos_, lit.size()) == lit) {
      pos_ += lit.size();
      return true;
    }
    return set_error("invalid literal");
  }

  bool parse_value(Value& out, size_t depth) {
    if (depth > opt_.max_depth) return set_error("nesting too deep");
    if (pos_ >= s_.size()) return set_error("unexpected end of input");
    switch (s_[pos_]) {
      case 'n':
        if (!consume_literal("null")) return false;
        out = Value(nullptr);
        return true;
      case 't':
        if (!consume_literal("true")) return false;
        out = Value(true);
        return true;
      case 'f':
        if (!consume_literal("false")) return false;
        out = Value(false);
        return true;
      case '"': {
        std::string str;
        if (!parse_string(str)) return false;
        out = Value(std::move(str));
        return true;
      }
      case '[':
        return parse_array(out, depth);
      case '{':
        return parse_object(out, depth);
      default:
        return parse_number(out);
    }
  }

  bool parse_array(Value& out, size_t depth) {
    ++pos_;  // '['
    Array arr;
    skip_ws();
    if (pos_ < s_.size() && s_[pos_] == ']') {
      ++pos_;
      out = Value(std::move(arr));
      return true;
    }
    for (;;) {
      skip_ws();
      Value item;
      if (!parse_value(item, depth + 1)) return false;
      arr.push_back(std::move(item));
      skip_ws();
      if (pos_ >= s_.size()) return set_error("unterminated array");
      if (s_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (s_[pos_] == ']') {
        ++pos_;
        out = Value(std::move(arr));
        return true;
      }
      return set_error("expected ',' or ']'");
    }
  }

  bool parse_object(Value& out, size_t depth) {
    ++pos_;  // '{'
    Object obj;
    skip_ws();
    if (pos_ < s_.size() && s_[pos_] == '}') {
      ++pos_;
      out = Value(std::move(obj));
      return true;
    }
    for (;;) {
      skip_ws();
      if (pos_ >= s_.size() || s_[pos_] != '"') return set_error("expected object key");
      std::string key;
      if (!parse_string(key)) return false;
      for (const auto& kv : obj) {
        if (kv.first == key) return set_error("duplicate object key");
      }
      skip_ws();
      if (pos_ >= s_.size() || s_[pos_] != ':') return set_error("expected ':'");
      ++pos_;
      skip_ws();
      Value v;
      if (!parse_value(v, depth + 1)) return false;
      obj.emplace_back(std::move(key), std::move(v));
      skip_ws();
      if (pos_ >= s_.size()) return set_error("unterminated object");
      if (s_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (s_[pos_] == '}') {
        ++pos_;
        out = Value(std::move(obj));
        return true;
      }
      return set_error("expected ',' or '}'");
    }
  }

  static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }

  bool parse_hex4(uint32_t& out) {
    if (pos_ + 4 > s_.size()) return set_error("truncated \\u escape");
    out = 0;
    for (int i = 0; i < 4; ++i) {
      int h = hex_value(s_[pos_ + i]);
      if (h < 0) return set_error("invalid \\u escape");
      out = (out << 4) | static_cast<uint32_t>(h);
    }
    pos_ += 4;
    return true;
  }

  static void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  // Validates one UTF-8 sequence starting at pos_ and copies it to out.
  bool copy_utf8(std::string& out) {
    auto b0 = static_cast<unsigned char>(s_[pos_]);
    size_t len;
    uint32_t cp;
    if (b0 < 0x80) {
      out.push_back(static_cast<char>(b0));
      ++pos_;
      return true;
    } else if ((b0 & 0xE0) == 0xC0) {
      len = 2;
      cp = b0 & 0x1F;
    } else if ((b0 & 0xF0) == 0xE0) {
      len = 3;
      cp = b0 & 0x0F;
    } else if ((b0 & 0xF8) == 0xF0) {
      len = 4;
      cp = b0 & 0x07;
    } else {
      return set_error("invalid UTF-8");
    }
    if (pos_ + len > s_.size()) return set_error("truncated UTF-8");
    for (size_t i = 1; i < len; ++i) {
      auto b = static_cast<unsigned char>(s_[pos_ + i]);
      if ((b & 0xC0) != 0x80) return set_error("invalid UTF-8");
      cp = (cp << 6) | (b & 0x3F);
    }
    // Reject overlong encodings, surrogates and out-of-range code points.
    static constexpr uint32_t kMin[5] = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < kMin[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
      return set_error("invalid UTF-8");
    }
    out.append(s_.data() + pos_, len);
    pos_ += len;
    return true;
  }

  bool parse_string(std::string& out) {
    ++pos_;  // opening quote
    for (;;) {
      if (pos_ >= s_.size()) return set_error("unterminated string");
      char c = s_[pos_];
      if (c == '"') {
        ++pos_;
        return true;
      }
      if (static_cast<unsigned char>(c) < 0x20) return set_error("control character in string");
      if (c != '\\') {
        if (!copy_utf8(out)) return false;
        continue;
      }
      ++pos_;
      if (pos_ >= s_.size()) return set_error("unterminated escape");
      char e = s_[pos_++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          uint32_t cp;
          if (!parse_hex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (pos_ + 2 > s_.size() || s_[pos_] != '\\' || s_[pos_ + 1] != 'u') {
              return set_error("unpaired surrogate");
            }
            pos_ += 2;
            uint32_t lo;
            if (!parse_hex4(lo)) return false;
            if (lo < 0xDC00 || lo > 0xDFFF) return set_error("unpaired surrogate");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return set_error("unpaired surrogate");
          }
          append_utf8(out, cp);
          break;
        }
        default:
          return set_error("invalid escape");
      }
    }
  }

  bool parse_number(Value& out) {
    size_t start = pos_;
    bool integral = true;
    if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
    if (pos_ >= s_.size()) return set_error("invalid number");
    if (s_[pos_] == '0') {
      ++pos_;
    } else if (s_[pos_] >= '1' && s_[pos_] <= '9') {
      while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
    } else {
      return set_error("invalid value");
    }
    if (pos_ < s_.size() && s_[pos_] == '.') {
      integral = false;
      ++pos_;
      size_t digits = pos_;
      while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
      if (pos_ == digits) return set_error("invalid number");
    }
    if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
      integral = false;
      ++pos_;
      if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
      size_t digits = pos_;
      while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
      if (pos_ == digits) return set_error("invalid number");
    }
    std::string_view text = s_.substr(start, pos_ - start);
    if (integral) {
      int64_t i = 0;
      auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), i);
      if (ec == std::errc() && ptr == text.data() + text.size()) {
        out = Value(i);
        return true;
      }
      // Out of int64 range: fall through to double.
    }
    auto d = parse_double(text);
    if (!d || !std::isfinite(*d)) return set_error("number out of range");
    out = Value(*d);
    return true;
  }

  std::string_view s_;
  ParseOptions opt_;
  size_t pos_ = 0;
  ParseError err_;
};

}  // namespace

std::optional<Value> parse(std::string_view text, ParseError* error, const ParseOptions& options) {
  if (text.size() > options.max_input_bytes) {
    if (error) *error = ParseError{0, "input too large"};
    return std::nullopt;
  }
  Parser p(text, options);
  return p.run(error);
}

// ---------------------------------------------------------------------------
// Serializer

void append_string(std::string& out, std::string_view s) {
  static constexpr char kHex[] = "0123456789abcdef";
  out.push_back('"');
  for (char c : s) {
    auto u = static_cast<unsigned char>(c);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (u < 0x20) {
          out += "\\u00";
          out.push_back(kHex[u >> 4]);
          out.push_back(kHex[u & 0xF]);
        } else {
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
}

void append_number(std::string& out, int64_t i) {
  char buf[24];
  auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), i);
  out.append(buf, ptr);
}

void append_number(std::string& out, double d) {
  if (!std::isfinite(d)) {
    // JSON has no NaN/Inf. Writers must not produce them; degrade to null.
    out += "null";
    return;
  }
  char buf[32];
  // Shortest representation that round-trips exactly.
  auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), d);
  std::string_view text(buf, static_cast<size_t>(ptr - buf));
  out.append(text);
  // Keep doubles recognizable as non-integers so they round-trip as Double.
  if (text.find_first_of(".eE") == std::string_view::npos) out += ".0";
}

namespace {

void newline(std::string& out, int indent, int level) {
  if (indent < 0) return;
  out.push_back('\n');
  out.append(static_cast<size_t>(indent) * static_cast<size_t>(level), ' ');
}

void write_value(std::string& out, const Value& v, int indent, int level) {
  switch (v.type()) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += *v.as_bool() ? "true" : "false"; break;
    case Type::Int: append_number(out, *v.as_int()); break;
    case Type::Double: append_number(out, *v.as_double()); break;
    case Type::String: append_string(out, *v.as_string()); break;
    case Type::Array: {
      const Array& a = *v.as_array();
      if (a.empty()) {
        out += "[]";
        break;
      }
      // Arrays of scalars stay on one line; it keeps tuple rows readable.
      bool scalars = true;
      for (const auto& item : a) {
        if (item.is_array() || item.is_object()) {
          scalars = false;
          break;
        }
      }
      out.push_back('[');
      for (size_t i = 0; i < a.size(); ++i) {
        if (i) out += scalars && indent >= 0 ? ", " : ",";
        if (!scalars) newline(out, indent, level + 1);
        write_value(out, a[i], indent, level + 1);
      }
      if (!scalars) newline(out, indent, level);
      out.push_back(']');
      break;
    }
    case Type::Object: {
      const Object& o = *v.as_object();
      if (o.empty()) {
        out += "{}";
        break;
      }
      out.push_back('{');
      for (size_t i = 0; i < o.size(); ++i) {
        if (i) out.push_back(',');
        newline(out, indent, level + 1);
        append_string(out, o[i].first);
        out += indent >= 0 ? ": " : ":";
        write_value(out, o[i].second, indent, level + 1);
      }
      newline(out, indent, level);
      out.push_back('}');
      break;
    }
  }
}

}  // namespace

std::string serialize(const Value& value, int indent) {
  std::string out;
  write_value(out, value, indent, 0);
  if (indent >= 0) out.push_back('\n');
  return out;
}

}  // namespace dr::json
