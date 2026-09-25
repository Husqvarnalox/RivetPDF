#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <unordered_set>

namespace rivet::core {

// A typed identifier. Distinct tags produce distinct, non-interchangeable
// types: a DocumentId can never be assigned to a PageId without an explicit
// conversion through value(). Value 0 is reserved as the invalid ID.
template <typename Tag, typename Underlying = std::uint64_t>
class StrongId {
public:
    constexpr StrongId() = default;

    static constexpr StrongId invalid() { return StrongId{}; }

    constexpr explicit StrongId(Underlying value) : value_(value) {}

    constexpr Underlying value() const { return value_; }

    constexpr explicit operator bool() const { return value_ != 0; }

    constexpr bool operator==(const StrongId&) const = default;
    constexpr auto operator<=>(const StrongId&) const = default;

private:
    Underlying value_ = 0;
};

struct DocumentIdTag;
struct PageIdTag;
struct ObjectIdTag;

using DocumentId = StrongId<DocumentIdTag>;
using PageId = StrongId<PageIdTag>;
using ObjectId = StrongId<ObjectIdTag>;

// Sequential ID source. Thread-safe usage is the caller's responsibility;
// each generator instance should be owned by the layer that mints the IDs
// (e.g. the document controller on the main thread).
template <typename Tag, typename Underlying = std::uint64_t>
class IdGenerator {
public:
    StrongId<Tag, Underlying> next() { return StrongId<Tag, Underlying>{++counter_}; }

    void reset() { counter_ = 0; }

private:
    Underlying counter_ = 0; // 0 is reserved for the invalid ID
};

} // namespace rivet::core

template <typename Tag, typename Underlying>
struct std::hash<rivet::core::StrongId<Tag, Underlying>> {
    std::size_t operator()(const rivet::core::StrongId<Tag, Underlying>& id) const noexcept {
        return std::hash<Underlying>{}(id.value());
    }
};
