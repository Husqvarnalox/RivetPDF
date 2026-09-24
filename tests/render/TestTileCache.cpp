#include "RivetTest.h"

#include "core/Bitmap.hpp"
#include "render/TileCache.hpp"
#include "render/RenderScaleKey.hpp"

#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

using rivet::core::Bitmap;
using rivet::core::DocumentId;
using rivet::core::PageId;
using rivet::render::RenderScaleKey;
using rivet::render::TileCache;
using rivet::render::TileKey;

namespace {

std::shared_ptr<const Bitmap> makeBitmap(std::uint32_t width, std::uint32_t height) {
    auto result = Bitmap::create(width, height);
    CHECK(result.has_value());
    return std::make_shared<const Bitmap>(std::move(*result));
}

TileKey makeTileKey(std::uint64_t document, std::uint64_t page, std::uint32_t tileX = 0,
                    std::uint32_t tileY = 0) {
    TileKey key;
    key.documentId = DocumentId{document};
    key.pageId = PageId{page};
    key.scale = RenderScaleKey::fromZoom(1.0);
    key.tileX = tileX;
    key.tileY = tileY;
    return key;
}

} // namespace

RIVET_TEST(tileCacheInsertLookup) {
    TileCache cache;
    const TileKey key = makeTileKey(1, 1);
    auto bitmap = makeBitmap(16, 1);
    CHECK(cache.put(key, 1, bitmap));
    CHECK_EQ(cache.count(), std::size_t{1});
    CHECK_EQ(cache.sizeBytes(), bitmap->sizeBytes());

    const std::shared_ptr<const Bitmap> fetched = cache.get(key, 1);
    CHECK(fetched != nullptr);
    CHECK_EQ(fetched, bitmap); // shared_ptr identity preserved
    CHECK_EQ(cache.get(makeTileKey(1, 2), 1), nullptr); // unknown key misses
    CHECK_EQ(cache.get(makeTileKey(1, 1, 3, 4), 1), nullptr);
}

RIVET_TEST(tileCacheMissOnEmpty) {
    TileCache cache;
    CHECK_EQ(cache.get(makeTileKey(1, 1), 1), nullptr);
    CHECK_EQ(cache.count(), std::size_t{0});
    CHECK_EQ(cache.sizeBytes(), std::size_t{0});
    cache.remove(makeTileKey(1, 1)); // no-op, no crash
}

RIVET_TEST(tileCachePromotion) {
    // A=64 B=128 C=256 bytes; budget 320 forces one eviction when C arrives.
    TileCache cache(320);
    const TileKey keyA = makeTileKey(1, 1);
    const TileKey keyB = makeTileKey(1, 2);
    const TileKey keyC = makeTileKey(1, 3);
    auto a = makeBitmap(16, 1);
    auto b = makeBitmap(32, 1);
    auto c = makeBitmap(64, 1);
    CHECK(cache.put(keyA, 1, a));
    CHECK(cache.put(keyB, 1, b));
    CHECK_EQ(cache.get(keyA, 1), a); // promote A; B is now least recently used
    CHECK(cache.put(keyC, 1, c));
    CHECK_EQ(cache.count(), std::size_t{2});
    CHECK_EQ(cache.get(keyB, 1), nullptr); // B was evicted, not A
    CHECK_EQ(cache.get(keyA, 1), a);
    CHECK_EQ(cache.get(keyC, 1), c);
    CHECK_EQ(cache.sizeBytes(), a->sizeBytes() + c->sizeBytes());
}

RIVET_TEST(tileCacheEvictionOrderExact) {
    // A=64 B=128 C=128 D=128 bytes; budget 400: inserting D evicts exactly A.
    TileCache cache(400);
    const TileKey keyA = makeTileKey(1, 1);
    const TileKey keyB = makeTileKey(1, 2);
    const TileKey keyC = makeTileKey(1, 3);
    const TileKey keyD = makeTileKey(1, 4);
    CHECK(cache.put(keyA, 1, makeBitmap(16, 1)));
    CHECK(cache.put(keyB, 1, makeBitmap(32, 1)));
    CHECK(cache.put(keyC, 1, makeBitmap(16, 2)));
    CHECK(cache.put(keyD, 1, makeBitmap(17, 1))); // 68 -> 128 byte stride
    CHECK_EQ(cache.count(), std::size_t{3});
    CHECK_EQ(cache.sizeBytes(), std::size_t{384});
    CHECK_EQ(cache.get(keyA, 1), nullptr);
    CHECK(cache.get(keyB, 1) != nullptr);
    CHECK(cache.get(keyC, 1) != nullptr);
    CHECK(cache.get(keyD, 1) != nullptr);
}

RIVET_TEST(tileCacheReplacementUpdatesAccounting) {
    TileCache cache;
    const TileKey key = makeTileKey(1, 1);
    auto small = makeBitmap(16, 1); // 64 bytes
    auto big = makeBitmap(64, 1);   // 256 bytes
    CHECK(cache.put(key, 1, small));
    CHECK_EQ(cache.sizeBytes(), small->sizeBytes());
    CHECK(cache.put(key, 1, big));
    CHECK_EQ(cache.count(), std::size_t{1});
    CHECK_EQ(cache.sizeBytes(), big->sizeBytes());
    CHECK_EQ(cache.get(key, 1), big);

    // Replacement across a revision updates the stamp too.
    CHECK(cache.put(key, 2, small));
    CHECK_EQ(cache.get(key, 2), small);
    CHECK_EQ(cache.sizeBytes(), small->sizeBytes());
    // The superseded revision is stale from the cache's point of view.
    CHECK_EQ(cache.get(key, 1), nullptr);
}

RIVET_TEST(tileCacheRejectsOversize) {
    auto big = makeBitmap(64, 1); // 256 bytes
    TileCache cache(big->sizeBytes() - 1);
    CHECK(!cache.put(makeTileKey(1, 1), 1, big));
    CHECK_EQ(cache.count(), std::size_t{0});
    CHECK_EQ(cache.sizeBytes(), std::size_t{0});

    // Exactly the budget fits.
    TileCache exact(big->sizeBytes());
    CHECK(exact.put(makeTileKey(1, 1), 1, big));
    CHECK_EQ(exact.sizeBytes(), big->sizeBytes());
}

RIVET_TEST(tileCacheRejectsNullAndInvalid) {
    TileCache cache;
    CHECK(!cache.put(makeTileKey(1, 1), 1, nullptr));
    auto invalid = std::make_shared<const Bitmap>(); // default-constructed: no pixels
    CHECK(!cache.put(makeTileKey(1, 1), 1, invalid));
    CHECK_EQ(cache.count(), std::size_t{0});
    CHECK_EQ(cache.sizeBytes(), std::size_t{0});
}

RIVET_TEST(tileCacheByteAccountingAfterMixedOps) {
    TileCache cache(1024);
    const TileKey keyA = makeTileKey(1, 1);
    const TileKey keyB = makeTileKey(1, 2);
    const TileKey keyC = makeTileKey(1, 3);
    auto a = makeBitmap(16, 1);  // 64 bytes
    auto b = makeBitmap(32, 1);  // 128 bytes
    auto c = makeBitmap(16, 3);  // 192 bytes
    CHECK(cache.put(keyA, 1, a));
    CHECK(cache.put(keyB, 1, b));
    CHECK(cache.put(keyC, 1, c));
    CHECK_EQ(cache.sizeBytes(), a->sizeBytes() + b->sizeBytes() + c->sizeBytes());

    cache.remove(keyB);
    CHECK_EQ(cache.count(), std::size_t{2});
    CHECK_EQ(cache.sizeBytes(), a->sizeBytes() + c->sizeBytes());

    auto replacement = makeBitmap(64, 1); // 256 bytes
    CHECK(cache.put(keyA, 1, replacement));
    CHECK_EQ(cache.count(), std::size_t{2});
    CHECK_EQ(cache.sizeBytes(), replacement->sizeBytes() + c->sizeBytes());

    cache.clear();
    CHECK_EQ(cache.count(), std::size_t{0});
    CHECK_EQ(cache.sizeBytes(), std::size_t{0});
    CHECK_EQ(cache.get(keyA, 1), nullptr);
    CHECK_EQ(cache.get(keyC, 1), nullptr);
}

RIVET_TEST(tileCacheSetMaxBytesEvicts) {
    TileCache cache;
    const TileKey keyA = makeTileKey(1, 1);
    const TileKey keyB = makeTileKey(1, 2);
    const TileKey keyC = makeTileKey(1, 3);
    auto unit = makeBitmap(16, 1); // 64 bytes
    CHECK(cache.put(keyA, 1, unit));
    CHECK(cache.put(keyB, 1, unit));
    CHECK(cache.put(keyC, 1, unit));
    CHECK_EQ(cache.sizeBytes(), 3 * unit->sizeBytes());

    cache.setMaxBytes(2 * unit->sizeBytes());
    CHECK_EQ(cache.count(), std::size_t{2});
    CHECK_EQ(cache.sizeBytes(), 2 * unit->sizeBytes());
    CHECK_EQ(cache.get(keyA, 1), nullptr); // A was least recently used
    CHECK(cache.get(keyB, 1) != nullptr);
    CHECK(cache.get(keyC, 1) != nullptr);

    cache.setMaxBytes(0);
    CHECK_EQ(cache.count(), std::size_t{0});
    CHECK_EQ(cache.sizeBytes(), std::size_t{0});
    CHECK(!cache.put(makeTileKey(9, 9), 1, unit)); // nothing fits an empty budget
}

RIVET_TEST(tileCacheStaleRevision) {
    TileCache cache;
    const TileKey key = makeTileKey(1, 1);
    auto bitmap = makeBitmap(16, 1);
    CHECK(cache.put(key, 5, bitmap));
    CHECK_EQ(cache.get(key, 5), bitmap);

    // A newer revision misses AND lazily drops the stale entry.
    CHECK_EQ(cache.get(key, 6), nullptr);
    CHECK_EQ(cache.count(), std::size_t{0});
    CHECK_EQ(cache.sizeBytes(), std::size_t{0});
    CHECK_EQ(cache.get(key, 5), nullptr); // already dropped

    // Re-populated at the current revision it hits again.
    CHECK(cache.put(key, 6, bitmap));
    CHECK_EQ(cache.get(key, 6), bitmap);
    CHECK_EQ(cache.count(), std::size_t{1});
}

RIVET_TEST(tileCacheConcurrentAccess) {
    // Threads work on disjoint key sets; the final accounting must be exact.
    TileCache cache(1u << 20);
    auto unit = makeBitmap(16, 1);
    constexpr int kThreads = 4;
    constexpr int kKeysPerThread = 200;
    constexpr int kRemovedPerThread = 67; // i % 3 == 0 -> removed

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&cache, t] {
            for (int i = 0; i < kKeysPerThread; ++i) {
                const TileKey key = makeTileKey(static_cast<std::uint64_t>(t + 1),
                                                static_cast<std::uint64_t>(i + 1));
                auto bitmap = makeBitmap(16, 1);
                cache.put(key, 1, bitmap);
                cache.get(key, 1);
                if (i % 3 == 0) cache.remove(key);
            }
        });
    }
    for (auto& thread : threads) thread.join();

    constexpr std::size_t kKept = static_cast<std::size_t>(kThreads) * (kKeysPerThread - kRemovedPerThread);
    CHECK_EQ(cache.count(), kKept);
    CHECK_EQ(cache.sizeBytes(), kKept * unit->sizeBytes());
}
