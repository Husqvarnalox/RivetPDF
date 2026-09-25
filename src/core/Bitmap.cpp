#include "core/Bitmap.hpp"

#include <algorithm>
#include <new>

namespace rivet::core {

std::size_t Bitmap::bytesPerPixel() const {
    switch (format()) {
        case PixelFormat::BGRA8888Straight: return 4;
    }
    return 4;
}

Result<Bitmap> Bitmap::create(std::uint32_t width, std::uint32_t height, std::size_t bytesPerRow) {
    if (width == 0 || height == 0) {
        return std::unexpected(makeError(ErrorCode::InvalidArgument,
                                         "Bitmap dimensions must be non-zero", "core"));
    }
    if (width > kMaxBitmapDimension || height > kMaxBitmapDimension) {
        return std::unexpected(makeError(ErrorCode::InvalidArgument,
                                         "Bitmap dimensions exceed the supported maximum", "core"));
    }

    const std::size_t bpp = 4; // BGRA8888Straight

    std::size_t stride = bytesPerRow;
    if (stride == 0) {
        std::size_t rawStride = 0;
        if (!checkedMultiply(static_cast<std::size_t>(width), bpp, rawStride)) {
            return std::unexpected(makeError(ErrorCode::InvalidArgument,
                                             "Bitmap row size overflows", "core"));
        }
        stride = roundUpToAlignment(rawStride, 64);
    }

    std::size_t totalBytes = 0;
    if (!checkedMultiply(stride, static_cast<std::size_t>(height), totalBytes)) {
        return std::unexpected(makeError(ErrorCode::InvalidArgument,
                                         "Bitmap size overflows", "core"));
    }
    if (totalBytes > kMaxBitmapBytes) {
        return std::unexpected(makeError(ErrorCode::InvalidArgument,
                                         "Bitmap size exceeds the supported maximum", "core"));
    }

    Bitmap bitmap;
    bitmap.width_ = width;
    bitmap.height_ = height;
    bitmap.stride_ = stride;

    bitmap.pixels_.resize(totalBytes); // zero-filled
    return bitmap;
}

void Bitmap::clear() {
    std::fill(pixels_.begin(), pixels_.end(), std::byte{0});
}

} // namespace rivet::core
