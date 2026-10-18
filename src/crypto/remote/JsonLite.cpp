// SPDX-License-Identifier: MPL-2.0
#include "crypto/remote/JsonLite.hpp"

#include <cstdlib>

namespace rivet::crypto::remote {

const JsonValue* JsonValue::find(std::string_view key) const {
    if (kind != Kind::Object) return nullptr;
    for (std::size_t i = 0; i < keys.size(); ++i)
        if (keys[i] == key) return &items[i];
    return nullptr;
}

std::optional<std::string> JsonValue::stringAt(std::string_view key) const {
    const JsonValue* v = find(key);
    if (!v || v->kind != Kind::String) return std::nullopt;
    return v->text;
}

std::optional<long long> JsonValue::intAt(std::string_view key) const {
    const JsonValue* v = find(key);
    if (!v || v->kind != Kind::Number || v->number < -9e15 || v->number > 9e15) return std::nullopt;
    return static_cast<long long>(v->number);
}

std::optional<bool> JsonValue::boolAt(std::string_view key) const {
    const JsonValue* v = find(key);
    if (!v || v->kind != Kind::Bool) return std::nullopt;
    return v->boolean;
}

namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    bool parseDocument(JsonValue& out) {
        skipWs();
        if (!value(out, 0)) return false;
        skipWs();
        return pos_ == s_.size();
    }

private:
    void skipWs() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
    }
    bool eat(char c) {
        if (pos_ < s_.size() && s_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }
    bool literal(std::string_view word) {
        if (s_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }

    static void appendUtf8(std::string& out, unsigned cp) {
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

    bool hex4(unsigned& out) {
        if (pos_ + 4 > s_.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool string(std::string& out) {
        if (!eat('"')) return false;
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= s_.size()) return false;
            const char e = s_[pos_++];
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
                unsigned cp = 0;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    unsigned lo = 0;
                    if (!literal("\\u") || !hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return false;
                }
                appendUtf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool number(JsonValue& out) {
        const std::size_t start = pos_;
        eat('-');
        if (pos_ >= s_.size()) return false;
        if (s_[pos_] == '0') {
            ++pos_;
        } else if (s_[pos_] >= '1' && s_[pos_] <= '9') {
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        } else {
            return false;
        }
        if (eat('.')) {
            const std::size_t d = pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
            if (pos_ == d) return false;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            const std::size_t d = pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
            if (pos_ == d) return false;
        }
        const std::string token(s_.substr(start, pos_ - start));
        out.kind = JsonValue::Kind::Number;
        out.number = std::strtod(token.c_str(), nullptr);
        return true;
    }

    bool value(JsonValue& out, int depth) {
        if (depth > kMaxJsonDepth || pos_ >= s_.size()) return false;
        const char c = s_[pos_];
        if (c == '{') {
            ++pos_;
            out.kind = JsonValue::Kind::Object;
            skipWs();
            if (eat('}')) return true;
            for (;;) {
                skipWs();
                std::string key;
                if (!string(key)) return false;
                skipWs();
                if (!eat(':')) return false;
                skipWs();
                JsonValue v;
                if (!value(v, depth + 1)) return false;
                bool duplicate = false;
                for (const auto& k : out.keys) duplicate = duplicate || k == key;
                if (!duplicate) {
                    out.keys.push_back(std::move(key));
                    out.items.push_back(std::move(v));
                }
                skipWs();
                if (eat(',')) continue;
                return eat('}');
            }
        }
        if (c == '[') {
            ++pos_;
            out.kind = JsonValue::Kind::Array;
            skipWs();
            if (eat(']')) return true;
            for (;;) {
                skipWs();
                JsonValue v;
                if (!value(v, depth + 1)) return false;
                out.items.push_back(std::move(v));
                skipWs();
                if (eat(',')) continue;
                return eat(']');
            }
        }
        if (c == '"') {
            out.kind = JsonValue::Kind::String;
            return string(out.text);
        }
        if (c == 't') {
            out.kind = JsonValue::Kind::Bool;
            out.boolean = true;
            return literal("true");
        }
        if (c == 'f') {
            out.kind = JsonValue::Kind::Bool;
            return literal("false");
        }
        if (c == 'n') return literal("null");
        return number(out);
    }

    std::string_view s_;
    std::size_t pos_ = 0;
};

constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

} // namespace

CryptoResult<JsonValue> parseJson(std::string_view text) {
    if (text.size() > kMaxJsonBytes) return fail(CryptoErrorCode::BadResponse, "JSON reply is too large");
    JsonValue out;
    Parser parser(text);
    if (!parser.parseDocument(out)) return fail(CryptoErrorCode::BadResponse, "malformed JSON reply");
    return out;
}

std::string jsonQuote(std::string_view text) {
    std::string out = "\"";
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                static constexpr char hex[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 15]);
            } else {
                out.push_back(ch);
            }
        }
    }
    out.push_back('"');
    return out;
}

void JsonObjectWriter::key(std::string_view k) {
    if (!body_.empty()) body_.push_back(',');
    body_ += jsonQuote(k);
    body_.push_back(':');
}
JsonObjectWriter& JsonObjectWriter::add(std::string_view k, std::string_view value) {
    key(k);
    body_ += jsonQuote(value);
    return *this;
}
JsonObjectWriter& JsonObjectWriter::addInt(std::string_view k, long long value) {
    key(k);
    body_ += std::to_string(value);
    return *this;
}
JsonObjectWriter& JsonObjectWriter::addBool(std::string_view k, bool value) {
    key(k);
    body_ += value ? "true" : "false";
    return *this;
}
JsonObjectWriter& JsonObjectWriter::addRaw(std::string_view k, std::string_view rawJson) {
    key(k);
    body_ += rawJson;
    return *this;
}

std::string base64Encode(ByteView data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        const unsigned v = (unsigned{data[i]} << 16) | (unsigned{data[i + 1]} << 8) | data[i + 2];
        out.push_back(kB64[v >> 18]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(kB64[(v >> 6) & 63]);
        out.push_back(kB64[v & 63]);
    }
    if (const std::size_t rest = data.size() - i; rest > 0) {
        unsigned v = unsigned{data[i]} << 16;
        if (rest == 2) v |= unsigned{data[i + 1]} << 8;
        out.push_back(kB64[v >> 18]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(rest == 2 ? kB64[(v >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

std::optional<Bytes> base64Decode(std::string_view text) {
    if (text.size() % 4 != 0) return std::nullopt;
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    Bytes out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = text[i + static_cast<std::size_t>(k)];
            if (c == '=') {
                // padding only in the last quartet's last two positions
                if (i + 4 != text.size() || k < 2) return std::nullopt;
                v[k] = 0;
                ++pad;
            } else {
                if (pad > 0) return std::nullopt;
                v[k] = val(c);
                if (v[k] < 0) return std::nullopt;
            }
        }
        const unsigned n = (static_cast<unsigned>(v[0]) << 18) | (static_cast<unsigned>(v[1]) << 12) |
                           (static_cast<unsigned>(v[2]) << 6) | static_cast<unsigned>(v[3]);
        out.push_back(static_cast<std::uint8_t>(n >> 16));
        if (pad < 2) out.push_back(static_cast<std::uint8_t>((n >> 8) & 0xFF));
        if (pad < 1) out.push_back(static_cast<std::uint8_t>(n & 0xFF));
    }
    return out;
}

std::string formEncode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const char ch : value) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out.push_back(ch);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

} // namespace rivet::crypto::remote
