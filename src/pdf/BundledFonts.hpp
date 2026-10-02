// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "pdf/PdfContent.hpp"

#include <cstdint>
#include <span>
#include <string_view>

// The fonts Rivet ships for new and substituted text (ADR-0016): subset
// Arimo / Tinos / Cousine (Apache-2.0), compiled into rivet_pdf from
// third_party/fonts at build time.

namespace rivet::pdf {

// The TrueType program of `font` (never empty, static storage).
std::span<const std::uint8_t> bundledFontData(PdfBundledFont font);

// Human-readable family + style, e.g. "Arimo Bold" (static storage).
std::string_view bundledFontDisplayName(PdfBundledFont font);

// Whether the bundled subset contains a glyph for `codepoint` (the subset's
// character set, ADR-0016). Line breaks and spaces are always accepted.
bool bundledFontCovers(PdfBundledFont font, char32_t codepoint);

// The fallback face for an existing font (ADR-0016): Mono for monospace,
// Serif for serif, else Sans; Bold when bold.
PdfBundledFont fallbackFor(const PdfFontInfo& original);

} // namespace rivet::pdf
