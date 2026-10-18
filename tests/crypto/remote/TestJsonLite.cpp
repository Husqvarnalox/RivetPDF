// SPDX-License-Identifier: MPL-2.0
// JSON / base64 / form-encoding helpers of the remote-signing adapters.

#include "RivetTest.h"

#include "crypto/remote/JsonLite.hpp"

using namespace rivet::crypto;
using namespace rivet::crypto::remote;

RIVET_TEST(json_parses_nested_values) {
    auto v = parseJson(R"({"a":"x\ny\u00e9\ud83d\ude00","n":-12.5e1,"b":true,"z":null,"l":[1,"two",{"k":false}],"a":"dup"})");
    CHECK(v.has_value());
    if (!v) return;
    CHECK(v->isObject());
    CHECK_EQ(v->stringAt("a").value_or(""), std::string("x\ny\xC3\xA9\xF0\x9F\x98\x80")); // first duplicate wins
    CHECK_EQ(v->intAt("n").value_or(0), -125);
    CHECK_EQ(v->boolAt("b").value_or(false), true);
    const JsonValue* l = v->find("l");
    CHECK(l != nullptr && l->isArray() && l->items.size() == 3);
    if (l && l->items.size() == 3) CHECK_EQ(l->items[2].boolAt("k").value_or(true), false);
    CHECK(v->find("missing") == nullptr);
    CHECK(!v->stringAt("n").has_value());
}

RIVET_TEST(json_rejects_malformed_input) {
    for (const char* bad : {"", "{", "{\"a\":}", "[1,]", "{\"a\" 1}", "\"unterminated", "{\"a\":1} x", "01", "1.", "\"\\ud800\"",
                            "\"\\q\"", "tru", "{\"a\":\"\x01\"}"}) {
        auto r = parseJson(bad);
        if (r) std::fprintf(stderr, "    accepted: %s\n", bad);
        CHECK(!r.has_value());
        if (!r) CHECK(r.error().code == CryptoErrorCode::BadResponse);
    }
}

RIVET_TEST(json_bounds_depth_and_size) {
    std::string deep;
    for (int i = 0; i < kMaxJsonDepth + 3; ++i) deep += "[";
    for (int i = 0; i < kMaxJsonDepth + 3; ++i) deep += "]";
    CHECK(!parseJson(deep).has_value());
    CHECK(parseJson("[[[[[[[[1]]]]]]]]").has_value()); // depth 8
    CHECK(!parseJson(std::string(kMaxJsonBytes + 1, ' ')).has_value());
}

RIVET_TEST(json_writer_escapes) {
    const std::string s = JsonObjectWriter().add("k\"", "v\\\n\x01").addInt("n", 7).addBool("b", true).addRaw("o", "{}").str();
    auto back = parseJson(s);
    CHECK(back.has_value());
    if (!back) return;
    CHECK_EQ(back->stringAt("k\"").value_or(""), std::string("v\\\n\x01"));
    CHECK_EQ(back->intAt("n").value_or(0), 7);
    CHECK(back->find("o") != nullptr);
}

RIVET_TEST(base64_rfc4648_vectors) {
    const struct {
        const char* plain;
        const char* enc;
    } vectors[] = {{"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"}, {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"}};
    for (const auto& v : vectors) {
        const std::string plain = v.plain;
        CHECK_EQ(base64Encode(viewOf(plain)), std::string(v.enc));
        auto dec = base64Decode(v.enc);
        CHECK(dec.has_value());
        if (dec) CHECK(std::string(dec->begin(), dec->end()) == plain);
    }
    for (const char* bad : {"Zg=", "Zg", "Z===", "Zm9v\n", "Zm=v", "Zm9*", "=Zm9"}) {
        if (base64Decode(bad)) std::fprintf(stderr, "    accepted: %s\n", bad);
        CHECK(!base64Decode(bad).has_value());
    }
}

RIVET_TEST(form_encoding_escapes_reserved) {
    CHECK_EQ(formEncode("a b&c=d/é~-_."), std::string("a%20b%26c%3Dd%2F%C3%A9~-_."));
}
