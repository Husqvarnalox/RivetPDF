#include "RivetTest.h"

#include "core/StrongId.hpp"

#include <cstdint>
#include <type_traits>
#include <unordered_set>

using namespace rivet::core;

static_assert(!std::is_same_v<DocumentId, PageId>, "DocumentId and PageId must be distinct types");
static_assert(!std::is_same_v<PageId, ObjectId>, "PageId and ObjectId must be distinct types");
static_assert(!std::is_same_v<DocumentId, ObjectId>, "DocumentId and ObjectId must be distinct types");

RIVET_TEST(strongIdsStartInvalid) {
    CHECK(!DocumentId{}.operator bool());
    CHECK(!PageId::invalid().operator bool());
    CHECK_EQ(DocumentId{}.value(), std::uint64_t{0});
}

RIVET_TEST(strongIdsWrapRawValuesExplicitly) {
    const DocumentId id{std::uint64_t{7}};
    CHECK(id.operator bool());
    CHECK_EQ(id.value(), std::uint64_t{7});
}

RIVET_TEST(strongIdsAreDistinctTypes) {
    const DocumentId d{1};
    const PageId p{1};
    // Same underlying value, different types: equality between them does not
    // even compile, so verify the type identity instead.
    CHECK((std::is_same_v<std::remove_const_t<decltype(d)>, DocumentId>));
    CHECK((std::is_same_v<std::remove_const_t<decltype(p)>, PageId>));
}

RIVET_TEST(strongIdsCompareAndHash) {
    const DocumentId a{1};
    const DocumentId b{2};
    CHECK(a != b);
    CHECK(a < b);
    CHECK_EQ(DocumentId{5}, DocumentId{5});

    std::unordered_set<DocumentId> set;
    set.insert(a);
    set.insert(b);
    set.insert(DocumentId{1});
    CHECK_EQ(set.size(), std::size_t{2});
    CHECK(set.count(DocumentId{2}) == 1);
}

RIVET_TEST(idGeneratorProducesSequentialNonZeroIds) {
    IdGenerator<DocumentId> gen;
    const auto first = gen.next();
    const auto second = gen.next();
    const auto third = gen.next();
    CHECK(first.operator bool());
    CHECK_EQ(first.value(), std::uint64_t{1});
    CHECK_EQ(second.value(), std::uint64_t{2});
    CHECK_EQ(third.value(), std::uint64_t{3});

    gen.reset();
    CHECK_EQ(gen.next().value(), std::uint64_t{1});
}
