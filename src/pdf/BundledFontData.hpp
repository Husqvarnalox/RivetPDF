// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// Internal: the font bytes compiled in by cmake/EmbedFonts.cmake. The index
// is the PdfBundledFont enumerator value (Sans/Serif/Mono x Regular/Bold);
// the generated translation unit is BundledFontData.cpp in the build tree.
// Use bundledFontData() (BundledFonts.hpp) instead.

namespace rivet::pdf {

std::span<const std::uint8_t> embeddedFontData(std::size_t index);

} // namespace rivet::pdf
