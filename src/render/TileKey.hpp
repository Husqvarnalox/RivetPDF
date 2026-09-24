#pragma once

#include "core/StrongId.hpp"
#include "render/RenderScaleKey.hpp"

#include <cstdint>
#include <functional>

namespace rivet::render {

// Identity of a rendered tile: which document, which page, at which
// quantized scale, which cell of the tile grid.
struct TileKey {
    core::DocumentId documentId;
    core::PageId pageId;
    RenderScaleKey scale;
    std::uint32_t tileX = 0;
    std::uint32_t tileY = 0;

    constexpr bool operator==(const TileKey&) const = default;
    constexpr auto operator<=>(const TileKey&) const = default;
};

} // namespace rivet::render

template <>
struct std::hash<rivet::render::TileKey> {
    std::size_t operator()(const rivet::render::TileKey& key) const noexcept {
        std::size_t h = std::hash<std::uint64_t>{}(key.documentId.value());
        h ^= std::hash<std::uint64_t>{}(key.pageId.value()) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<std::uint32_t>{}(key.scale.value) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<std::uint32_t>{}(key.tileX) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<std::uint32_t>{}(key.tileY) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};
