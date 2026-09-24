#pragma once

#include "core/Error.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rivet::core {

enum class PixelFormat : std::uint8_t {
    // 32 bits per pixel, 8 bits per channel, byte order B,G,R,A,
    // premultiplied alpha. Matches the native PDFium raster output format.
    BGRA8888Premultiplied,
};

inline constexpr std::uint32_t kMaxBitmapDimension = 1u << 20; // 1M pixels per axis
inline constexpr std::size_t kMaxBitmapBytes = std::size_t{512} << 20; // 512 MiB

// Rounds value up to the next multiple of alignment (alignment must be a
// power of two).
std::size_t roundUpToAlignment(std::size_t value, std::size_t alignment);

// Rivet-owned raster surface. Move-only. Stride is explicit and may exceed
// width * bytesPerPixel; all allocation arithmetic is overflow-checked and
// bounded, because document-driven dimensions are untrusted input.
class Bitmap {
public:
    Bitmap() = default;

    // Creates a zero-filled bitmap. bytesPerRow == 0 selects
    // roundUpToAlignment(width * bytesPerPixel, 64).
    // Returns an error instead of throwing for impossible dimensions or
    // allocation failure.
    static Result<Bitmap> create(std::uint32_t width, std::uint32_t height, std::size_t bytesPerRow = 0);

    Bitmap(Bitmap&&) noexcept = default;
    Bitmap& operator=(Bitmap&&) noexcept = default;
    Bitmap(const Bitmap&) = delete;
    Bitmap& operator=(const Bitmap&) = delete;

    bool isValid() const { return !pixels_.empty(); }

    std::uint32_t width() const { return width_; }
    std::uint32_t height() const { return height_; }
    std::size_t stride() const { return stride_; }
    std::size_t bytesPerPixel() const;
    PixelFormat format() const { return PixelFormat::BGRA8888Premultiplied; }

    // Bytes actually in use: stride * height (excluding any allocator slack).
    std::size_t sizeBytes() const { return stride_ * static_cast<std::size_t>(height_); }

    std::byte* data() { return pixels_.data(); }
    const std::byte* data() const { return pixels_.data(); }

    // Zero-fills the pixel storage.
    void clear();

private:
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::size_t stride_ = 0;
    std::vector<std::byte> pixels_;
};

} // namespace rivet::core
