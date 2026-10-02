// SPDX-License-Identifier: MPL-2.0

#include "pdf/PdfContent.hpp"

#include "core/CheckedArithmetic.hpp"
#include "core/Error.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

namespace rivet::pdf {

namespace {

// Coordinates beyond this are not a page; they also keep later arithmetic
// (matrix products, effective sizes) far from overflow.
constexpr double kMaxMagnitude = 1.0e9;
// Tolerance of the rigid-placement test (cos/sin of a double angle).
constexpr double kRigidEpsilon = 1.0e-4;

core::Status invalid(std::string message) {
    return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument, std::move(message), "pdf"));
}

bool finiteBounded(double v) {
    return std::isfinite(v) && std::fabs(v) <= kMaxMagnitude;
}

bool finiteMatrix(const core::Matrix& m) {
    return finiteBounded(m.a) && finiteBounded(m.b) && finiteBounded(m.c) && finiteBounded(m.d) &&
           finiteBounded(m.tx) && finiteBounded(m.ty);
}

bool rigid(const core::Matrix& m) {
    return finiteMatrix(m) && std::fabs(m.a - m.d) <= kRigidEpsilon && std::fabs(m.b + m.c) <= kRigidEpsilon &&
           std::fabs(m.a * m.a + m.b * m.b - 1.0) <= kRigidEpsilon;
}

core::Status validateImage(const PdfImageData& image) {
    if (image.width == 0 || image.height == 0 || image.width > kMaxImageSide || image.height > kMaxImageSide) {
        return invalid("replacement image dimensions out of range");
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(image.width) * image.height;
    if (pixels > kMaxImagePixels) {
        return invalid("replacement image has too many pixels");
    }
    switch (image.format) {
    case PdfImageData::Format::Jpeg:
        if (image.bytes.size() < 4 || image.bytes.size() > kMaxImageEncodedBytes) {
            return invalid("replacement JPEG size out of range");
        }
        if (image.bytes[0] != 0xFF || image.bytes[1] != 0xD8) {
            return invalid("replacement JPEG lacks the SOI marker");
        }
        return core::ok();
    case PdfImageData::Format::Bgra: {
        std::size_t rowBytes = 0;
        if (!core::checkedMultiply(image.width, 4, rowBytes) || image.stride < rowBytes) {
            return invalid("replacement bitmap stride too small");
        }
        std::size_t rowsBeforeLast = 0;
        std::size_t needed = 0;
        if (!core::checkedMultiply(image.stride, static_cast<std::size_t>(image.height) - 1, rowsBeforeLast) ||
            !core::checkedAdd(rowsBeforeLast, rowBytes, needed)) {
            return invalid("replacement bitmap size overflows");
        }
        if (image.bytes.size() < needed) {
            return invalid("replacement bitmap data is shorter than its dimensions");
        }
        return core::ok();
    }
    }
    return invalid("unknown replacement image format");
}

} // namespace

core::Status validate(const PdfPageContentEdits& edits, std::size_t sourceObjectCount) {
    // --- Object edits -------------------------------------------------------
    std::set<std::uint32_t> editedObjects;
    bool haveLast = false;
    std::uint32_t last = 0;
    for (const PdfObjectEdit& edit : edits.objects) {
        if (edit.sourceIndex >= sourceObjectCount) return invalid("object edit index out of range");
        if (haveLast && edit.sourceIndex <= last) return invalid("object edits not sorted or not unique");
        haveLast = true;
        last = edit.sourceIndex;
        editedObjects.insert(edit.sourceIndex);
        if (edit.transform.has_value() && !finiteMatrix(*edit.transform)) {
            return invalid("object transform is not finite");
        }
        if (edit.replaceImage != nullptr) {
            if (edit.remove) return invalid("object is both removed and replaced");
            if (core::Status status = validateImage(*edit.replaceImage); !status) return status;
        }
    }

    // --- Text blocks --------------------------------------------------------
    if (edits.textBlocks.size() > kMaxTextBlocksPerPage) return invalid("too many text blocks");
    std::set<std::uint64_t> tags;
    std::set<std::uint32_t> claimed;
    for (const PdfTextBlockEdit& block : edits.textBlocks) {
        if (block.tag == 0) return invalid("text block tag must be non-zero");
        if (!tags.insert(block.tag).second) return invalid("duplicate text block tag");
        if (block.text.size() > kMaxTextBlockBytes) return invalid("text block too long");

        bool haveMember = false;
        std::uint32_t lastMember = 0;
        for (const std::uint32_t member : block.members) {
            if (member >= sourceObjectCount) return invalid("text block member out of range");
            if (haveMember && member <= lastMember) return invalid("text block members not sorted or not unique");
            haveMember = true;
            lastMember = member;
            if (!claimed.insert(member).second) return invalid("object claimed by two text blocks");
            if (editedObjects.count(member) != 0) {
                return invalid("text block member is also edited as an object");
            }
        }
        if (block.font.kind == PdfFontRef::Kind::FromObject && block.font.sourceIndex >= sourceObjectCount) {
            return invalid("text block font object out of range");
        }
        if (!std::isfinite(block.fontSize) || block.fontSize < kMinFontSize || block.fontSize > kMaxFontSize) {
            return invalid("text block font size out of range");
        }
        if (!std::isfinite(block.lineAdvance) || block.lineAdvance <= 0.0 || block.lineAdvance > kMaxMagnitude) {
            return invalid("text block line advance must be positive");
        }
        if (std::isnan(block.wrapWidth) || block.wrapWidth > kMaxMagnitude) {
            return invalid("text block wrap width invalid");
        }
        if (!std::isfinite(block.color.r) || !std::isfinite(block.color.g) || !std::isfinite(block.color.b)) {
            return invalid("text block color is not finite");
        }
        if (!rigid(block.placement)) return invalid("text block placement must be a rotation plus translation");
    }
    // Total text accepted per page is bounded by the counts above
    // (kMaxTextBlocksPerPage * kMaxTextBlockBytes).
    return core::ok();
}

} // namespace rivet::pdf
