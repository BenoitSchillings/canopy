// Tests for core/json.h
#include "core/json.h"
#include <cmath>
#include <cstdio>
#include <string>

using canopy::Json;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { auto _a = (a); auto _b = (b); if (_a == _b) g_pass++; else { g_fail++; std::printf("FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b); } } while (0)

static void test_scalars() {
    std::string err;
    auto j = Json::parse("null", &err);
    CHECK(j && j->is_null());
    j = Json::parse(" true ");
    CHECK(j && j->is_bool() && j->as_bool());
    j = Json::parse("false");
    CHECK(j && j->is_bool() && !j->as_bool());
    j = Json::parse("42");
    CHECK(j && j->is_number() && j->as_int() == 42);
    j = Json::parse("-0");
    CHECK(j && j->is_number() && j->as_double() == 0.0);
    j = Json::parse("3.25e2");
    CHECK(j && std::fabs(j->as_double() - 325.0) < 1e-9);
    j = Json::parse("1E-2");
    CHECK(j && std::fabs(j->as_double() - 0.01) < 1e-12);
    j = Json::parse("\"hi\"");
    CHECK(j && j->is_string() && j->as_string() == "hi");
}

static void test_strings() {
    auto j = Json::parse(R"("a\"b\\c\/d\b\f\n\r\t")");
    CHECK(j && j->as_string() == std::string("a\"b\\c/d\b\f\n\r\t"));
    // \u escapes: BMP, surrogate pair, raw UTF-8 passthrough
    j = Json::parse(R"("\u00e9\u20ac")");
    CHECK(j && j->as_string() == "\xC3\xA9\xE2\x82\xAC");
    j = Json::parse(R"("\ud83d\ude00")");
    CHECK(j && j->as_string() == "\xF0\x9F\x98\x80");   // U+1F600
    j = Json::parse("\"caf\xC3\xA9\"");
    CHECK(j && j->as_string() == "caf\xC3\xA9");
    // Lone surrogate -> U+FFFD rather than failure
    j = Json::parse(R"("\ud83d")");
    CHECK(j && j->as_string() == "\xEF\xBF\xBD");
    // Serialization escapes
    Json s(std::string("tab\there \"q\" back\\slash \x01 nl\n \xC3\xA9"));
    CHECK_EQ(s.dump(), std::string(R"("tab\there \"q\" back\\slash \u0001 nl\n )") + "\xC3\xA9\"");
    // Round-trip of every control char
    std::string all;
    for (int c = 0; c < 0x20; c++) all.push_back((char)c);
    all.push_back((char)0x7f);
    auto rt = Json::parse(Json(all).dump());
    CHECK(rt && rt->as_string() == all);
}

static void test_containers() {
    std::string err;
    auto j = Json::parse(R"( { "b": [1, 2, {"c": null}], "a": "x", "d": {} , "e": [] } )", &err);
    CHECK(j && err.empty());
    CHECK(j->is_object());
    CHECK_EQ(j->size(), (size_t)4);
    CHECK((*j)["b"].is_array());
    CHECK_EQ((*j)["b"].size(), (size_t)3);
    CHECK_EQ((*j)["b"][1].as_int(), (int64_t)2);
    CHECK((*j)["b"][2]["c"].is_null());
    CHECK((*j)["b"][7].is_null());          // out of range -> static null
    CHECK((*j)["missing"].is_null());       // missing key -> static null
    CHECK((*j)["missing"]["deeper"].is_null());
    CHECK(j->has("a") && !j->has("zz"));
    // Deterministic key order (sorted)
    CHECK_EQ(j->dump(), std::string(R"({"a":"x","b":[1,2,{"c":null}],"d":{},"e":[]})"));
    // Indented output
    Json small = Json::object();
    small["k"] = Json(1);
    small["arr"] = Json::array();
    small["arr"].push_back(Json(true));
    CHECK_EQ(small.dump(2), std::string("{\n  \"arr\": [\n    true\n  ],\n  \"k\": 1\n}"));
    // Mutating access converts null to container
    Json m;
    m["x"]["y"] = Json(3);
    CHECK(m.is_object() && m["x"]["y"].as_int() == 3);
    Json a;
    a.push_back(Json("q"));
    a[2] = Json(5);
    CHECK(a.is_array() && a.size() == 3 && a[1].is_null() && a[2].as_int() == 5);
    // Duplicate keys: last wins
    j = Json::parse(R"({"a":1,"a":2})");
    CHECK(j && (*j)["a"].as_int() == 2);
}

static void test_numbers() {
    CHECK_EQ(Json(0).dump(), std::string("0"));
    CHECK_EQ(Json(-17).dump(), std::string("-17"));
    CHECK_EQ(Json(1.0).dump(), std::string("1"));
    CHECK_EQ(Json(1.5).dump(), std::string("1.5"));
    CHECK_EQ(Json(0.1).dump(), std::string("0.10000000000000001"));
    CHECK_EQ(Json((int64_t)9007199254740991LL).dump(), std::string("9007199254740991"));
    CHECK_EQ(Json((int64_t)-9007199254740991LL).dump(), std::string("-9007199254740991"));
    CHECK_EQ(Json(1e300).dump(), std::string("1.0000000000000001e+300"));
    CHECK_EQ(Json(std::nan("")).dump(), std::string("null"));
    // Integer round trip across a wide range
    int64_t vals[] = {0, 1, -1, 123456789, -987654321, 4294967296LL, 1LL << 40, (1LL << 53) - 1, 1700000000};
    for (int64_t v : vals) {
        auto j = Json::parse(Json(v).dump());
        CHECK(j && j->as_int() == v);
    }
    // Double round trip
    double ds[] = {0.1, 3.141592653589793, -2.5e-7, 6.02214076e23, 1.0 / 3.0};
    for (double d : ds) {
        auto j = Json::parse(Json(d).dump());
        CHECK(j && j->as_double() == d);
    }
    // uint64 beyond 2^53 prints as %.17g (not exact, but parseable)
    auto big = Json::parse(Json((uint64_t)18446744073709551615ULL).dump());
    CHECK(big && big->is_number());
}

static void test_errors() {
    const char* bad[] = {
        "", "   ", "{", "[1,", "[1,]", "{\"a\":}", "{\"a\" 1}", "{a:1}", "tru", "nul", "01", "1.", ".5", "1e",
        "\"abc", "\"\\x\"", "\"\\u12\"", "\"a\nb\"", "[1] x", "{\"a\":1,}", "-", "+1", "[1 2]", "\"\\ud83d\\u12\"",
    };
    for (const char* b : bad) {
        std::string err;
        auto j = Json::parse(b, &err);
        if (j) std::printf("  unexpectedly parsed: %s\n", b);
        CHECK(!j);
        CHECK(!err.empty());
    }
    std::string err;
    Json::parse("{\n  \"a\": tru\n}", &err);
    CHECK(err.find("line 2") != std::string::npos);
    CHECK(err.find("offset 8") != std::string::npos);
    // Deep nesting is rejected rather than crashing
    std::string deep(100000, '[');
    CHECK(!Json::parse(deep, &err));
}

static void test_accessors_defaults() {
    Json s("str");
    CHECK_EQ(s.as_int(7), (int64_t)7);
    CHECK_EQ(s.as_double(2.5), 2.5);
    CHECK(s.as_bool(true));
    CHECK(s.as_array().empty());
    CHECK(s.as_object().empty());
    CHECK_EQ(Json(5).as_string("dflt"), std::string("dflt"));
    CHECK_EQ(Json(5).as_string(), std::string(""));
    const Json n;
    CHECK(n[(size_t)0].is_null());
    CHECK(n["k"].is_null());
    CHECK_EQ(n.size(), (size_t)0);
}

static void test_roundtrip_big() {
    Json doc = Json::object();
    for (int i = 0; i < 200; i++) {
        Json item = Json::object();
        item["id"] = Json(i);
        item["name"] = Json("item \xE2\x9C\x93 " + std::to_string(i));
        item["ratio"] = Json(i / 7.0);
        item["tags"] = Json::array();
        item["tags"].push_back(Json("a"));
        item["tags"].push_back(Json(nullptr));
        item["tags"].push_back(Json(i % 2 == 0));
        doc["items"].push_back(std::move(item));
    }
    std::string compact = doc.dump();
    std::string pretty = doc.dump(4);
    auto a = Json::parse(compact);
    auto b = Json::parse(pretty);
    CHECK(a && b);
    CHECK_EQ(a->dump(), compact);
    CHECK_EQ(b->dump(), compact);
    CHECK_EQ((*a)["items"][199]["name"].as_string(), std::string("item \xE2\x9C\x93 199"));
}

int main() {
    test_scalars();
    test_strings();
    test_containers();
    test_numbers();
    test_errors();
    test_accessors_defaults();
    test_roundtrip_big();
    std::printf("test_json: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
