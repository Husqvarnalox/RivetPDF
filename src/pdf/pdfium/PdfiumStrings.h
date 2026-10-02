// SPDX-License-Identifier: MPL-2.0
#pragma once

// UTF-16LE <-> UTF-8 helpers for the PDFium string APIs (FPDF_WIDESTRING /
// FPDF_WCHAR buffers). INTERNAL to src/pdf/pdfium/.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::pdf::internal {

// Converts UTF-16LE bytes (as returned by FPDF_GetMetaText, FPDFAnnot_GetStringValue,
// ...) to UTF-8. Stops at the terminating NUL code unit, skips a leading BOM,
// decodes surrogate pairs and emits U+FFFD for malformed sequences.
std::string utf16leToUtf8(const std::uint8_t* bytes, std::size_t byteLength);

// UTF-8 -> UTF-16LE bytes INCLUDING the 2-byte NUL terminator (what
// FPDF_WIDESTRING expects). Malformed UTF-8 bytes become U+FFFD.
std::vector<std::uint8_t> utf8ToUtf16le(std::string_view text);

} // namespace rivet::pdf::internal
