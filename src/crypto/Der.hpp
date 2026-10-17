// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "crypto/Error.hpp"
#include "crypto/Types.hpp"

#include <chrono>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

// Strict, bounded ASN.1 DER/BER reader and a small DER writer used by the
// Rivet-owned CMS model. The reader never allocates for element contents
// (spans into the input), rejects high-tag-number form, length fields wider
// than 4 bytes, lengths past the end of input, stray end-of-contents markers
// and nesting deeper than kMaxDepth. Indefinite lengths are accepted only
// for constructed elements (BER output of other CMS producers).
namespace rivet::crypto::der {

inline constexpr int kMaxDepth = 24;
inline constexpr std::size_t kMaxInputSize = std::size_t{1} << 30; // 1 GiB hard cap
inline constexpr std::size_t kMaxChildren = std::size_t{1} << 20;

inline constexpr std::uint8_t kInteger = 0x02;
inline constexpr std::uint8_t kBitString = 0x03;
inline constexpr std::uint8_t kOctetString = 0x04;
inline constexpr std::uint8_t kNull = 0x05;
inline constexpr std::uint8_t kOid = 0x06;
inline constexpr std::uint8_t kUtf8String = 0x0C;
inline constexpr std::uint8_t kUtcTime = 0x17;
inline constexpr std::uint8_t kGeneralizedTime = 0x18;
inline constexpr std::uint8_t kSequence = 0x30;
inline constexpr std::uint8_t kSet = 0x31;

constexpr std::uint8_t contextTag(unsigned number, bool constructed) {
    return static_cast<std::uint8_t>(0x80u | (constructed ? 0x20u : 0u) | number);
}

struct Element {
    std::uint8_t tag = 0;
    bool constructed = false;
    bool indefinite = false;
    ByteView full;    // header + content (+ end-of-contents when indefinite)
    ByteView content; // content octets only
};

// Reads the first element of `in` (and validates its whole subtree).
CryptoResult<Element> readElement(ByteView in, int depth = 0);
// Reads `in` as exactly one element with no trailing bytes.
CryptoResult<Element> readSingle(ByteView in);
// Child elements of a constructed element's content.
CryptoResult<std::vector<Element>> children(const Element& parent);

CryptoResult<std::string> decodeOid(ByteView content);
CryptoResult<Bytes> encodeOid(std::string_view dotted);
// Integer content with leading zero octets removed (zero -> empty).
Bytes normalizeInteger(ByteView content);
// Concatenated BER primitive/constructed OCTET STRING content (bounded depth 2).
CryptoResult<Bytes> octetStringValue(const Element& element);
// UTCTime / GeneralizedTime content -> UTC time point.
CryptoResult<std::chrono::system_clock::time_point> decodeTime(const Element& element);

// ---- writer -------------------------------------------------------------

Bytes tlv(std::uint8_t tag, ByteView content);
Bytes concat(std::initializer_list<ByteView> parts);
Bytes concat(const std::vector<Bytes>& parts);
Bytes sequence(const std::vector<Bytes>& parts);
// SET OF: members sorted by encoding, as DER requires.
Bytes setOf(std::vector<Bytes> members);
Bytes integer(ByteView magnitudeBigEndian);
Bytes integerFromInt(std::int64_t value);
Bytes octetString(ByteView content);
Bytes nullValue();
Bytes oidElement(std::string_view dotted);
Bytes timeElement(std::chrono::system_clock::time_point time);

} // namespace rivet::crypto::der
