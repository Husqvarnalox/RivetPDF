// SPDX-License-Identifier: MPL-2.0
#include "crypto/Der.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>

namespace rivet::crypto::der {

namespace {

CryptoError malformed(std::string message) {
    return CryptoError{CryptoErrorCode::MalformedCms, std::move(message)};
}

} // namespace

CryptoResult<Element> readElement(ByteView in, int depth) {
    if (depth > kMaxDepth) return fail(CryptoErrorCode::LimitExceeded, "ASN.1 nesting too deep");
    if (in.size() > kMaxInputSize) return fail(CryptoErrorCode::LimitExceeded, "ASN.1 input too large");
    if (in.size() < 2) return std::unexpected(malformed("truncated element header"));

    Element el;
    el.tag = in[0];
    if (el.tag == 0) return std::unexpected(malformed("unexpected end-of-contents marker"));
    if ((el.tag & 0x1F) == 0x1F) return std::unexpected(malformed("high tag numbers are not supported"));
    el.constructed = (el.tag & 0x20) != 0;

    std::size_t pos = 2;
    std::size_t length = 0;
    const std::uint8_t first = in[1];
    if (first == 0x80) {
        if (!el.constructed) return std::unexpected(malformed("indefinite length on a primitive element"));
        el.indefinite = true;
    } else if (first < 0x80) {
        length = first;
    } else {
        const std::size_t count = first & 0x7F;
        if (count > 4) return fail(CryptoErrorCode::LimitExceeded, "ASN.1 length field too wide");
        if (in.size() < pos + count) return std::unexpected(malformed("truncated length field"));
        for (std::size_t i = 0; i < count; ++i) length = (length << 8) | in[pos + i];
        pos += count;
    }

    if (!el.indefinite) {
        if (length > in.size() - pos) return std::unexpected(malformed("element length exceeds the input"));
        el.content = in.subspan(pos, length);
        el.full = in.first(pos + length);
        if (el.constructed) {
            ByteView rest = el.content;
            while (!rest.empty()) {
                auto child = readElement(rest, depth + 1);
                if (!child) return std::unexpected(child.error());
                rest = rest.subspan(child->full.size());
            }
        }
        return el;
    }

    std::size_t p = pos;
    for (;;) {
        if (in.size() < p + 2) return std::unexpected(malformed("unterminated indefinite-length element"));
        if (in[p] == 0 && in[p + 1] == 0) break;
        auto child = readElement(in.subspan(p), depth + 1);
        if (!child) return std::unexpected(child.error());
        p += child->full.size();
    }
    el.content = in.subspan(pos, p - pos);
    el.full = in.first(p + 2);
    return el;
}

CryptoResult<Element> readSingle(ByteView in) {
    auto el = readElement(in);
    if (!el) return el;
    if (el->full.size() != in.size()) return std::unexpected(malformed("trailing data after the element"));
    return el;
}

CryptoResult<std::vector<Element>> children(const Element& parent) {
    if (!parent.constructed) return std::unexpected(malformed("expected a constructed element"));
    std::vector<Element> out;
    ByteView rest = parent.content;
    while (!rest.empty()) {
        if (out.size() >= kMaxChildren) return fail(CryptoErrorCode::LimitExceeded, "too many child elements");
        auto child = readElement(rest);
        if (!child) return std::unexpected(child.error());
        rest = rest.subspan(child->full.size());
        out.push_back(*child);
    }
    return out;
}

CryptoResult<std::string> decodeOid(ByteView content) {
    if (content.empty()) return std::unexpected(malformed("empty OBJECT IDENTIFIER"));
    std::string out;
    std::uint64_t value = 0;
    bool first = true;
    bool pending = false;
    for (std::uint8_t b : content) {
        if (value > (std::uint64_t{1} << 56)) return std::unexpected(malformed("OID arc too large"));
        value = (value << 7) | (b & 0x7F);
        pending = (b & 0x80) != 0;
        if (pending) continue;
        if (first) {
            const std::uint64_t a = value >= 80 ? 2 : value / 40;
            out += std::to_string(a) + "." + std::to_string(value - a * 40);
            first = false;
        } else {
            out += "." + std::to_string(value);
        }
        value = 0;
    }
    if (pending) return std::unexpected(malformed("truncated OBJECT IDENTIFIER"));
    return out;
}

CryptoResult<Bytes> encodeOid(std::string_view dotted) {
    std::vector<std::uint64_t> arcs;
    std::size_t pos = 0;
    while (pos <= dotted.size()) {
        const std::size_t dot = dotted.find('.', pos);
        const std::string_view part = dotted.substr(pos, dot == std::string_view::npos ? dot : dot - pos);
        std::uint64_t value = 0;
        const auto res = std::from_chars(part.data(), part.data() + part.size(), value);
        if (part.empty() || res.ec != std::errc{} || res.ptr != part.data() + part.size())
            return fail(CryptoErrorCode::InvalidArgument, "invalid OID string");
        arcs.push_back(value);
        if (dot == std::string_view::npos) break;
        pos = dot + 1;
    }
    if (arcs.size() < 2 || arcs[0] > 2 || (arcs[0] < 2 && arcs[1] >= 40))
        return fail(CryptoErrorCode::InvalidArgument, "invalid OID string");
    Bytes out;
    auto put = [&out](std::uint64_t v) {
        std::uint8_t tmp[10];
        int n = 0;
        do {
            tmp[n++] = static_cast<std::uint8_t>(v & 0x7F);
            v >>= 7;
        } while (v);
        for (int i = n - 1; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(tmp[i] | (i ? 0x80 : 0)));
    };
    put(arcs[0] * 40 + arcs[1]);
    for (std::size_t i = 2; i < arcs.size(); ++i) put(arcs[i]);
    return out;
}

Bytes normalizeInteger(ByteView content) {
    std::size_t i = 0;
    while (i < content.size() && content[i] == 0) ++i;
    return Bytes(content.begin() + static_cast<std::ptrdiff_t>(i), content.end());
}

CryptoResult<Bytes> octetStringValue(const Element& element) {
    if (element.tag == kOctetString) return Bytes(element.content.begin(), element.content.end());
    if (element.tag != (kOctetString | 0x20)) return std::unexpected(malformed("expected OCTET STRING"));
    // BER constructed form: concatenate primitive chunks (no nesting).
    auto kids = children(element);
    if (!kids) return std::unexpected(kids.error());
    Bytes out;
    for (const Element& k : *kids) {
        if (k.tag != kOctetString) return std::unexpected(malformed("nested constructed OCTET STRING"));
        out.insert(out.end(), k.content.begin(), k.content.end());
    }
    return out;
}

namespace {

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant).
std::int64_t daysFromCivil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civilFromDays(std::int64_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t yy = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int>(yy + (m <= 2));
}

} // namespace

CryptoResult<std::chrono::system_clock::time_point> decodeTime(const Element& element) {
    const std::string_view text(reinterpret_cast<const char*>(element.content.data()), element.content.size());
    std::size_t yearDigits = 0;
    if (element.tag == kUtcTime) yearDigits = 2;
    else if (element.tag == kGeneralizedTime) yearDigits = 4;
    else return std::unexpected(malformed("expected a time value"));
    // YY[YY]MMDDHHMMSSZ
    if (text.size() != yearDigits + 11 || text.back() != 'Z') return std::unexpected(malformed("unsupported time format"));
    auto num = [&](std::size_t off, std::size_t n) -> int {
        int v = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const char c = text[off + i];
            if (c < '0' || c > '9') return -1;
            v = v * 10 + (c - '0');
        }
        return v;
    };
    int year = num(0, yearDigits);
    const int mon = num(yearDigits, 2);
    const int day = num(yearDigits + 2, 2);
    const int hour = num(yearDigits + 4, 2);
    const int min = num(yearDigits + 6, 2);
    const int sec = num(yearDigits + 8, 2);
    if (year < 0 || mon < 1 || mon > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || min < 0 || min > 59 ||
        sec < 0 || sec > 60)
        return std::unexpected(malformed("invalid time value"));
    if (yearDigits == 2) year += year >= 50 ? 1900 : 2000;
    const std::int64_t days = daysFromCivil(year, static_cast<unsigned>(mon), static_cast<unsigned>(day));
    return std::chrono::system_clock::time_point(
        std::chrono::seconds(days * 86400 + hour * 3600 + min * 60 + sec));
}

// ---- writer ---------------------------------------------------------------

Bytes tlv(std::uint8_t tag, ByteView content) {
    Bytes out;
    out.reserve(content.size() + 6);
    out.push_back(tag);
    const std::size_t n = content.size();
    if (n < 0x80) {
        out.push_back(static_cast<std::uint8_t>(n));
    } else {
        std::uint8_t tmp[8];
        int count = 0;
        for (std::size_t v = n; v; v >>= 8) tmp[count++] = static_cast<std::uint8_t>(v & 0xFF);
        out.push_back(static_cast<std::uint8_t>(0x80 | count));
        for (int i = count - 1; i >= 0; --i) out.push_back(tmp[i]);
    }
    out.insert(out.end(), content.begin(), content.end());
    return out;
}

Bytes concat(std::initializer_list<ByteView> parts) {
    Bytes out;
    for (ByteView p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

Bytes concat(const std::vector<Bytes>& parts) {
    Bytes out;
    for (const Bytes& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

Bytes sequence(const std::vector<Bytes>& parts) { return tlv(kSequence, concat(parts)); }

Bytes setOf(std::vector<Bytes> members) {
    std::sort(members.begin(), members.end());
    return tlv(kSet, concat(members));
}

Bytes integer(ByteView magnitude) {
    Bytes body = normalizeInteger(magnitude);
    if (body.empty() || (body[0] & 0x80)) body.insert(body.begin(), 0);
    return tlv(kInteger, body);
}

Bytes integerFromInt(std::int64_t value) {
    Bytes mag;
    std::uint64_t v = static_cast<std::uint64_t>(value < 0 ? 0 : value);
    for (int i = 7; i >= 0; --i) mag.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    return integer(mag);
}

Bytes octetString(ByteView content) { return tlv(kOctetString, content); }

Bytes nullValue() { return Bytes{kNull, 0}; }

Bytes oidElement(std::string_view dotted) {
    auto body = encodeOid(dotted);
    return tlv(kOid, body ? ByteView(*body) : ByteView{});
}

Bytes timeElement(std::chrono::system_clock::time_point time) {
    const std::int64_t secs = std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
    std::int64_t days = secs / 86400;
    std::int64_t rem = secs % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    int y;
    unsigned m, d;
    civilFromDays(days, y, m, d);
    char buf[24];
    const bool utc = y >= 1950 && y <= 2049;
    int n = 0;
    if (utc)
        n = std::snprintf(buf, sizeof buf, "%02d%02u%02u%02d%02d%02dZ", y % 100, m, d, static_cast<int>(rem / 3600),
                          static_cast<int>((rem % 3600) / 60), static_cast<int>(rem % 60));
    else
        n = std::snprintf(buf, sizeof buf, "%04d%02u%02u%02d%02d%02dZ", y, m, d, static_cast<int>(rem / 3600),
                          static_cast<int>((rem % 3600) / 60), static_cast<int>(rem % 60));
    return tlv(utc ? kUtcTime : kGeneralizedTime, ByteView(reinterpret_cast<const std::uint8_t*>(buf), static_cast<std::size_t>(n)));
}

} // namespace rivet::crypto::der
