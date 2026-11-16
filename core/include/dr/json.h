// Minimal, strict JSON value type, parser and serializer for Demo Recorder
// project files.
//
// Project files can come from other people, so the parser is defensive:
// bounded nesting depth, bounded input size, strict RFC 8259 grammar, and no
// exceptions escape `parse`. Integers that fit in int64 are kept exact (host
// timestamps in nanoseconds do not fit in a double's 53-bit mantissa).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace dr::json {

enum class Type { Null, Bool, Int, Double, String, Array, Object };

class Value;

using Array = std::vector<Value>;
// Objects keep insertion order so files round-trip with stable diffs.
using Object = std::vector<std::pair<std::string, Value>>;

class Value {
 public:
  Value() = default;
  Value(std::nullptr_t) {}
  Value(bool b) : v_(b) {}
  Value(int i) : v_(static_cast<int64_t>(i)) {}
  Value(int64_t i) : v_(i) {}
  Value(uint32_t i) : v_(static_cast<int64_t>(i)) {}
  Value(double d) : v_(d) {}
  Value(const char* s) : v_(std::string(s)) {}
  Value(std::string s) : v_(std::move(s)) {}
  Value(std::string_view s) : v_(std::string(s)) {}
  Value(Array arr) : v_(std::move(arr)) {}
  Value(Object obj) : v_(std::move(obj)) {}

  Type type() const { return static_cast<Type>(v_.index()); }
  bool is_null() const { return type() == Type::Null; }
  bool is_bool() const { return type() == Type::Bool; }
  bool is_int() const { return type() == Type::Int; }
  bool is_number() const { return type() == Type::Int || type() == Type::Double; }
  bool is_string() const { return type() == Type::String; }
  bool is_array() const { return type() == Type::Array; }
  bool is_object() const { return type() == Type::Object; }

  // Typed accessors. Each returns nullopt / nullptr on a type mismatch, so
  // callers validating untrusted files never hit undefined behavior.
  std::optional<bool> as_bool() const;
  std::optional<int64_t> as_int() const;      // Int, or Double with an exact integer value
  std::optional<double> as_double() const;    // Int or Double
  const std::string* as_string() const;
  const Array* as_array() const;
  Array* as_array();
  const Object* as_object() const;
  Object* as_object();

  // Object helpers. `find` returns nullptr when absent or not an object.
  const Value* find(std::string_view key) const;
  // Sets (or replaces) a key. Converts a null value into an empty object first.
  Value& set(std::string_view key, Value v);
  // Appends to an array. Converts a null value into an empty array first.
  Value& push_back(Value v);

  friend bool operator==(const Value& a, const Value& b) { return a.v_ == b.v_; }
  friend bool operator!=(const Value& a, const Value& b) { return !(a == b); }

 private:
  std::variant<std::nullptr_t, bool, int64_t, double, std::string, Array, Object> v_;
};

struct ParseOptions {
  size_t max_depth = 64;
  size_t max_input_bytes = size_t{512} * 1024 * 1024;
};

struct ParseError {
  size_t offset = 0;
  std::string message;
};

// Parses a complete JSON document. Trailing non-whitespace is an error.
std::optional<Value> parse(std::string_view text, ParseError* error = nullptr,
                           const ParseOptions& options = {});

// Serializes a value. indent < 0 produces compact output on one line.
std::string serialize(const Value& value, int indent = 2);

// Appends the JSON encoding of a string / number to `out`. Used by streaming
// writers that must not build a whole document in memory.
void append_string(std::string& out, std::string_view s);
void append_number(std::string& out, double d);
void append_number(std::string& out, int64_t i);

// Locale-independent number parsing shared with streaming readers.
std::optional<double> parse_double(std::string_view s);

}  // namespace dr::json
