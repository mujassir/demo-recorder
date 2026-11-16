#include "dr/json.h"
#include "test.h"

using namespace dr;

TEST(json_parses_scalars) {
  CHECK(json::parse("null")->is_null());
  CHECK(*json::parse("true")->as_bool() == true);
  CHECK(*json::parse(" false ")->as_bool() == false);
  CHECK(*json::parse("-42")->as_int() == -42);
  CHECK(*json::parse("1.5")->as_double() == 1.5);
  CHECK(*json::parse("1e3")->as_double() == 1000.0);
  CHECK(*json::parse("\"hi\"")->as_string() == "hi");
}

TEST(json_keeps_large_integers_exact) {
  // Host time in ns after ~200 days of uptime; does not fit in a double.
  auto v = json::parse("17280000000000123");
  REQUIRE(v && v->is_int());
  CHECK(*v->as_int() == 17280000000000123LL);
  CHECK(json::serialize(*v, -1) == "17280000000000123");
}

TEST(json_out_of_range_integer_becomes_double) {
  auto v = json::parse("99999999999999999999");
  REQUIRE(v);
  CHECK(v->type() == json::Type::Double);
}

TEST(json_string_escapes_and_unicode) {
  auto v = json::parse(R"("a\"b\\c\n\u00e9\ud83d\ude00")");
  REQUIRE(v);
  CHECK(*v->as_string() == "a\"b\\c\n\xC3\xA9\xF0\x9F\x98\x80");
  std::string out = json::serialize(*v, -1);
  auto back = json::parse(out);
  REQUIRE(back);
  CHECK(*back == *v);
}

TEST(json_rejects_malformed_input) {
  const char* bad[] = {
      "",          "{",         "[1,]",          "{\"a\":1,}", "01",        "1.",
      ".5",        "+1",        "NaN",           "[1 2]",      "\"\\x\"",   "\"\x01\"",
      "{\"a\" 1}", "tru",       "[1]x",          "\"\\ud800\"", "\"\xC0\xAF\"",
      "\"\xED\xA0\x80\"", "{\"a\":1,\"a\":2}", "1e",
  };
  for (const char* s : bad) {
    json::ParseError err;
    bool failed = !json::parse(s, &err);
    if (!failed) std::fprintf(stderr, "    accepted: %s\n", s);
    CHECK(failed);
    CHECK(!failed || !err.message.empty());
  }
}

TEST(json_depth_limit) {
  std::string deep(200, '[');
  deep += std::string(200, ']');
  CHECK(!json::parse(deep));
  json::ParseOptions opts;
  opts.max_depth = 300;
  CHECK(json::parse(deep, nullptr, opts).has_value());
}

TEST(json_round_trip_preserves_order_and_types) {
  json::Value doc;
  doc.set("z", 1);
  doc.set("a", 2.0);
  doc.set("m", json::Array{json::Value(1), json::Value("x"), json::Value(nullptr)});
  doc.set("o", json::Object{{"k", json::Value(true)}});
  std::string text = json::serialize(doc);
  auto back = json::parse(text);
  REQUIRE(back);
  CHECK(*back == doc);
  CHECK(text.find("\"z\"") < text.find("\"a\""));
  CHECK(back->find("a")->type() == json::Type::Double);
}

TEST(json_doubles_round_trip_exactly) {
  double values[] = {0.1, 1.0 / 3.0, 1e-300, 123456.789, -0.0, 5e20};
  for (double d : values) {
    std::string s;
    json::append_number(s, d);
    auto v = json::parse(s);
    REQUIRE(v);
    CHECK(*v->as_double() == d);
  }
}
