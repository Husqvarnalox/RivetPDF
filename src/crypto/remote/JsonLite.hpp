// SPDX-License-Identifier: MPL-2.0
#pragma once

// Small, strict JSON reader / writer plus base64 and form encoding for the
// remote-signing adapters. Bounded (size and nesting) because the input is a
// network response. Not a general-purpose JSON library: numbers are doubles,
// duplicate keys keep the first value, strings are UTF-8 (\u escapes decoded).

#include "crypto/Error.hpp"
#include "crypto/Types.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::crypto::remote {

inline constexpr std::size_t kMaxJsonBytes = std::size_t{4} << 20;
inline constexpr int kMaxJsonDepth = 8;

class JsonValue {
public:
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0;
    std::string text;
    std::vector<JsonValue> items;     // Array elements, or Object values
    std::vector<std::string> keys;    // Object keys, parallel to items

    bool isObject() const { return kind == Kind::Object; }
    bool isArray() const { return kind == Kind::Array; }
    // Object member lookup (first match), nullptr when absent or not an object.
    const JsonValue* find(std::string_view key) const;
    std::optional<std::string> stringAt(std::string_view key) const;
    std::optional<long long> intAt(std::string_view key) const;
    std::optional<bool> boolAt(std::string_view key) const;
};

// Errors: CryptoErrorCode::BadResponse (malformed / too large / too deep).
CryptoResult<JsonValue> parseJson(std::string_view text);

// Flat object writer; keys and values are escaped.
class JsonObjectWriter {
public:
    JsonObjectWriter& add(std::string_view key, std::string_view value);
    JsonObjectWriter& addInt(std::string_view key, long long value);
    JsonObjectWriter& addBool(std::string_view key, bool value);
    // `rawJson` must already be valid JSON (e.g. a nested object).
    JsonObjectWriter& addRaw(std::string_view key, std::string_view rawJson);
    std::string str() const { return "{" + body_ + "}"; }

private:
    void key(std::string_view k);
    std::string body_;
};

std::string jsonQuote(std::string_view text);

std::string base64Encode(ByteView data);
// Strict RFC 4648 (padding required, no whitespace); nullopt on malformed input.
std::optional<Bytes> base64Decode(std::string_view text);

// application/x-www-form-urlencoded value escaping.
std::string formEncode(std::string_view value);

} // namespace rivet::crypto::remote
