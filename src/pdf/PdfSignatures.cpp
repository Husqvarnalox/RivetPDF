// SPDX-License-Identifier: MPL-2.0

#include "pdf/PdfSignatures.hpp"

namespace rivet::pdf {

PdfByteRangeCoverage analyzeByteRange(std::span<const std::int64_t> pairs, std::uint64_t fileSize) {
    PdfByteRangeCoverage out;
    if (pairs.empty() || pairs.size() % 2 != 0 || pairs.size() > kMaxSignatureByteRangeInts) return out;
    std::uint64_t previousEnd = 0;
    std::size_t holes = 0;
    for (std::size_t i = 0; i < pairs.size(); i += 2) {
        if (pairs[i] < 0 || pairs[i + 1] < 0) return out;
        const auto offset = static_cast<std::uint64_t>(pairs[i]);
        const auto length = static_cast<std::uint64_t>(pairs[i + 1]);
        if (offset < previousEnd || length > fileSize || offset > fileSize - length) return out;
        if (i == 0 ? offset != 0 : offset > previousEnd) ++holes;
        previousEnd = offset + length;
    }
    out.valid = true;
    out.signedEnd = previousEnd;
    out.modifiedAfterSigning = previousEnd < fileSize;
    // A leading gap (first offset != 0) counts as a hole too, so "exactly one
    // hole" means: from byte 0, one gap (the /Contents string), to the end.
    out.coversWholeDocument = pairs[0] == 0 && holes == 1 && previousEnd == fileSize;
    return out;
}

std::string signatureStatusText(const PdfSignatureInfo& info) {
    std::string text = "Signature present (not verified)";
    if (!info.coverage.valid) {
        text += " - byte range is malformed";
    } else if (info.coverage.modifiedAfterSigning) {
        text += " - document changed after signing";
    } else if (info.coverage.coversWholeDocument) {
        text += " - covers the whole document";
    } else {
        text += " - covers part of the document";
    }
    return text;
}

} // namespace rivet::pdf
