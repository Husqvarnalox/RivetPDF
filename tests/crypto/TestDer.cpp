// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "crypto/Der.hpp"

#include <chrono>

using namespace rivet::crypto;
using namespace rivet::crypto::der;

RIVET_TEST(derOidRoundTrip) {
    for (const char* dotted : {"1.2.840.113549.1.7.2", "2.16.840.1.101.3.4.2.1", "1.2.643.100.3", "2.999.3"}) {
        auto enc = encodeOid(dotted);
        CHECK(enc.has_value());
        auto dec = decodeOid(*enc);
        CHECK(dec.has_value());
        if (dec) CHECK_EQ(*dec, std::string(dotted));
    }
    CHECK(!encodeOid("").has_value());
    CHECK(!encodeOid("1").has_value());
    CHECK(!encodeOid("3.1").has_value());
    CHECK(!encodeOid("1.40").has_value());
    CHECK(!encodeOid("1.a").has_value());
}

RIVET_TEST(derWriterReaderRoundTrip) {
    const Bytes big(70000, 0x5A);
    const Bytes e = sequence({integerFromInt(5), octetString(big), oidElement("1.2.3")});
    auto top = readSingle(e);
    CHECK(top.has_value());
    auto kids = children(*top);
    CHECK(kids.has_value());
    CHECK_EQ(kids->size(), std::size_t{3});
    CHECK_EQ((*kids)[1].content.size(), big.size());
}

RIVET_TEST(derIntegerEncodingIsMinimalAndPositive) {
    CHECK((integer(Bytes{0x00, 0x00, 0x7F}) == Bytes{0x02, 0x01, 0x7F}));
    CHECK((integer(Bytes{0x80}) == Bytes{0x02, 0x02, 0x00, 0x80}));
    CHECK((integer(Bytes{}) == Bytes{0x02, 0x01, 0x00}));
}

RIVET_TEST(derSetOfSorts) {
    const Bytes a = integerFromInt(9);
    const Bytes b = integerFromInt(1);
    const Bytes set = setOf({a, b});
    CHECK_EQ(set[0], kSet);
    CHECK_EQ(set[4], 1); // the encoding with the smaller content comes first
}

RIVET_TEST(derTimeRoundTrip) {
    using namespace std::chrono;
    const system_clock::time_point t(seconds(1'800'000'000));
    const Bytes enc = timeElement(t);
    auto el = readSingle(enc);
    CHECK(el.has_value());
    auto back = decodeTime(*el);
    CHECK(back.has_value());
    if (back) CHECK(*back == t);
    // Far future uses GeneralizedTime.
    const system_clock::time_point far(seconds(4'200'000'000LL));
    const Bytes enc2 = timeElement(far);
    auto el2 = readSingle(enc2);
    CHECK(el2.has_value());
    if (el2) CHECK_EQ(el2->tag, kGeneralizedTime);
}

RIVET_TEST(derRejectsTruncatedAndOversizedLengths) {
    CHECK(!readSingle(Bytes{}).has_value());
    CHECK(!readSingle(Bytes{0x30}).has_value());
    CHECK(!readSingle(Bytes{0x30, 0x05, 0x01}).has_value());            // length past the end
    CHECK(!readSingle(Bytes{0x04, 0x84, 0xFF, 0xFF, 0xFF, 0xFF}).has_value()); // 4 GiB claim
    const auto wide = readSingle(Bytes{0x04, 0x89, 1, 2, 3, 4, 5, 6, 7, 8, 9}); // 9-byte length field
    CHECK(!wide.has_value());
    if (!wide) CHECK_EQ(wide.error().code, CryptoErrorCode::LimitExceeded);
}

RIVET_TEST(derRejectsHighTagsEocAndPrimitiveIndefinite) {
    CHECK(!readSingle(Bytes{0x1F, 0x81, 0x01, 0x00}).has_value()); // high tag number form
    CHECK(!readSingle(Bytes{0x00, 0x00}).has_value());             // bare end-of-contents
    CHECK(!readSingle(Bytes{0x04, 0x80, 0x00, 0x00}).has_value()); // indefinite primitive
    CHECK(!readSingle(Bytes{0x30, 0x80, 0x02, 0x01, 0x05}).has_value()); // unterminated indefinite
    CHECK(readSingle(Bytes{0x30, 0x80, 0x02, 0x01, 0x05, 0x00, 0x00}).has_value()); // well-formed BER
    CHECK(!readSingle(Bytes{0x30, 0x03, 0x02, 0x01, 0x05, 0x00}).has_value());      // trailing byte
}

RIVET_TEST(derBoundsNestingDepth) {
    Bytes e = integerFromInt(1);
    for (int i = 0; i < 200; ++i) e = tlv(kSequence, e);
    const auto deep = readSingle(e);
    CHECK(!deep.has_value());
    if (!deep) CHECK_EQ(deep.error().code, CryptoErrorCode::LimitExceeded);

    Bytes ok = integerFromInt(1);
    for (int i = 0; i < 10; ++i) ok = tlv(kSequence, ok);
    CHECK(readSingle(ok).has_value());

    // Deep indefinite-length nesting is bounded too.
    Bytes ind;
    for (int i = 0; i < 200; ++i) {
        ind.push_back(0x30);
        ind.push_back(0x80);
    }
    CHECK(!readSingle(ind).has_value());
}

RIVET_TEST(derConstructedOctetStringIsConcatenated) {
    const Bytes ber{0x24, 0x80, 0x04, 0x02, 'a', 'b', 0x04, 0x01, 'c', 0x00, 0x00};
    auto el = readSingle(ber);
    CHECK(el.has_value());
    auto v = octetStringValue(*el);
    CHECK(v.has_value());
    if (v) CHECK((*v == Bytes{'a', 'b', 'c'}));
}
