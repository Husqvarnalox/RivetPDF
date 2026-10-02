// SPDX-License-Identifier: MPL-2.0

#include "RivetTest.h"

#include "core/Error.hpp"
#include "pdf/PdfContent.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

// PDFium-free tests of pdf::validate (PdfContent.hpp). Both build modes.

namespace {

namespace core = rivet::core;
using namespace rivet::pdf;

bool isInvalidArgument(const core::Status& status) {
    return !status.has_value() && status.error().code == core::ErrorCode::InvalidArgument;
}

PdfTextBlockEdit block(std::uint64_t tag, std::vector<std::uint32_t> members = {}) {
    PdfTextBlockEdit b;
    b.tag = tag;
    b.members = std::move(members);
    b.text = "hello";
    return b;
}

std::shared_ptr<const PdfImageData> bgra(std::uint32_t w, std::uint32_t h, std::uint32_t stride = 0) {
    auto image = std::make_shared<PdfImageData>();
    image->format = PdfImageData::Format::Bgra;
    image->width = w;
    image->height = h;
    image->stride = stride == 0 ? w * 4 : stride;
    image->bytes.assign(static_cast<std::size_t>(image->stride) * h, 0x80);
    return image;
}

} // namespace

RIVET_TEST(contentValidateEmptyAndBasics) {
    PdfPageContentEdits edits;
    CHECK(edits.empty());
    CHECK(validate(edits, 0).has_value());
    CHECK(validate(edits, 10).has_value());

    PdfObjectEdit move;
    move.sourceIndex = 3;
    move.transform = core::Matrix::translation(10, 20);
    edits.objects.push_back(move);
    CHECK(!edits.empty());
    CHECK(validate(edits, 4).has_value());
    CHECK(isInvalidArgument(validate(edits, 3))); // index out of range
}

RIVET_TEST(contentValidateObjectOrdering) {
    PdfPageContentEdits edits;
    PdfObjectEdit a;
    a.sourceIndex = 5;
    a.remove = true;
    PdfObjectEdit b;
    b.sourceIndex = 2;
    b.remove = true;
    edits.objects = {a, b};
    CHECK(isInvalidArgument(validate(edits, 10))); // unsorted
    edits.objects = {b, b};
    CHECK(isInvalidArgument(validate(edits, 10))); // duplicate
    edits.objects = {b, a};
    CHECK(validate(edits, 10).has_value());
}

RIVET_TEST(contentValidateTransformMustBeFinite) {
    PdfPageContentEdits edits;
    PdfObjectEdit e;
    e.sourceIndex = 0;
    e.transform = core::Matrix{1, 0, 0, 1, std::numeric_limits<double>::quiet_NaN(), 0};
    edits.objects.push_back(e);
    CHECK(isInvalidArgument(validate(edits, 1)));
    edits.objects[0].transform = core::Matrix{1, 0, 0, 1, std::numeric_limits<double>::infinity(), 0};
    CHECK(isInvalidArgument(validate(edits, 1)));
    edits.objects[0].transform = core::Matrix{1, 0, 0, 1, 1e12, 0};
    CHECK(isInvalidArgument(validate(edits, 1)));
}

RIVET_TEST(contentValidateImageData) {
    PdfPageContentEdits edits;
    PdfObjectEdit e;
    e.sourceIndex = 0;
    edits.objects.push_back(e);

    edits.objects[0].replaceImage = bgra(4, 4);
    CHECK(validate(edits, 1).has_value());

    edits.objects[0].replaceImage = bgra(4, 4, 20); // padded stride
    CHECK(validate(edits, 1).has_value());

    auto shortStride = std::make_shared<PdfImageData>(*bgra(4, 4));
    shortStride->stride = 8;
    edits.objects[0].replaceImage = shortStride;
    CHECK(isInvalidArgument(validate(edits, 1)));

    auto truncated = std::make_shared<PdfImageData>(*bgra(4, 4));
    truncated->bytes.resize(10);
    edits.objects[0].replaceImage = truncated;
    CHECK(isInvalidArgument(validate(edits, 1)));

    // Zero and oversize dimensions.
    auto zero = std::make_shared<PdfImageData>();
    zero->format = PdfImageData::Format::Bgra;
    edits.objects[0].replaceImage = zero;
    CHECK(isInvalidArgument(validate(edits, 1)));

    // Image bomb: within the per-side limit, over the pixel limit, and the
    // declared size is checked BEFORE any pixel data is required.
    auto bomb = std::make_shared<PdfImageData>();
    bomb->format = PdfImageData::Format::Bgra;
    bomb->width = kMaxImageSide;
    bomb->height = kMaxImageSide;
    bomb->stride = kMaxImageSide * 4;
    edits.objects[0].replaceImage = bomb;
    CHECK(isInvalidArgument(validate(edits, 1)));

    auto wide = std::make_shared<PdfImageData>();
    wide->format = PdfImageData::Format::Bgra;
    wide->width = kMaxImageSide + 1;
    wide->height = 1;
    wide->stride = (kMaxImageSide + 1) * 4;
    wide->bytes.resize(wide->stride);
    edits.objects[0].replaceImage = wide;
    CHECK(isInvalidArgument(validate(edits, 1)));

    // Removing and replacing the same object is contradictory.
    edits.objects[0].replaceImage = bgra(2, 2);
    edits.objects[0].remove = true;
    CHECK(isInvalidArgument(validate(edits, 1)));
}

RIVET_TEST(contentValidateJpegData) {
    PdfPageContentEdits edits;
    PdfObjectEdit e;
    e.sourceIndex = 0;
    auto jpeg = std::make_shared<PdfImageData>();
    jpeg->format = PdfImageData::Format::Jpeg;
    jpeg->width = 2;
    jpeg->height = 2;
    jpeg->bytes = {0xFF, 0xD8, 0xFF, 0xD9};
    e.replaceImage = jpeg;
    edits.objects.push_back(e);
    CHECK(validate(edits, 1).has_value());

    auto notJpeg = std::make_shared<PdfImageData>(*jpeg);
    notJpeg->bytes = {0x00, 0x01, 0x02, 0x03};
    edits.objects[0].replaceImage = notJpeg;
    CHECK(isInvalidArgument(validate(edits, 1)));

    auto empty = std::make_shared<PdfImageData>(*jpeg);
    empty->bytes.clear();
    edits.objects[0].replaceImage = empty;
    CHECK(isInvalidArgument(validate(edits, 1)));
}

RIVET_TEST(contentValidateTextBlocks) {
    PdfPageContentEdits edits;
    edits.textBlocks.push_back(block(1, {2, 4}));
    edits.textBlocks.push_back(block(2));
    CHECK(validate(edits, 6).has_value());

    // Tag rules.
    edits.textBlocks[1].tag = 0;
    CHECK(isInvalidArgument(validate(edits, 6)));
    edits.textBlocks[1].tag = 1;
    CHECK(isInvalidArgument(validate(edits, 6)));
    edits.textBlocks[1].tag = 2;

    // Members: sorted/unique/in range/not shared.
    edits.textBlocks[0].members = {4, 2};
    CHECK(isInvalidArgument(validate(edits, 6)));
    edits.textBlocks[0].members = {2, 2};
    CHECK(isInvalidArgument(validate(edits, 6)));
    edits.textBlocks[0].members = {2, 9};
    CHECK(isInvalidArgument(validate(edits, 6)));
    edits.textBlocks[0].members = {2, 4};
    edits.textBlocks[1].members = {4};
    CHECK(isInvalidArgument(validate(edits, 6)));
    edits.textBlocks[1].members = {5};
    CHECK(validate(edits, 6).has_value());

    // A member must not be removed or transformed by an object edit.
    PdfObjectEdit remove;
    remove.sourceIndex = 2;
    remove.remove = true;
    edits.objects = {remove};
    CHECK(isInvalidArgument(validate(edits, 6)));
    edits.objects[0].remove = false;
    edits.objects[0].transform = core::Matrix::translation(1, 1);
    CHECK(isInvalidArgument(validate(edits, 6)));
    edits.objects[0].sourceIndex = 3; // unrelated object: fine
    CHECK(validate(edits, 6).has_value());
}

RIVET_TEST(contentValidateTextBlockParameters) {
    auto check = [](auto&& mutate, bool expectValid) {
        PdfPageContentEdits edits;
        edits.textBlocks.push_back(block(7));
        mutate(edits.textBlocks[0]);
        CHECK_EQ(validate(edits, 3).has_value(), expectValid);
    };
    check([](PdfTextBlockEdit&) {}, true);
    check([](PdfTextBlockEdit& b) { b.fontSize = kMinFontSize; }, true);
    check([](PdfTextBlockEdit& b) { b.fontSize = kMaxFontSize; }, true);
    check([](PdfTextBlockEdit& b) { b.fontSize = kMinFontSize - 0.01; }, false);
    check([](PdfTextBlockEdit& b) { b.fontSize = kMaxFontSize + 0.01; }, false);
    check([](PdfTextBlockEdit& b) { b.fontSize = std::nan(""); }, false);
    check([](PdfTextBlockEdit& b) { b.lineAdvance = 0.0; }, false);
    check([](PdfTextBlockEdit& b) { b.lineAdvance = -1.0; }, false);
    check([](PdfTextBlockEdit& b) { b.lineAdvance = std::nan(""); }, false);
    check([](PdfTextBlockEdit& b) { b.wrapWidth = std::nan(""); }, false);
    check([](PdfTextBlockEdit& b) { b.wrapWidth = -5.0; }, true); // <= 0 = no wrapping
    check([](PdfTextBlockEdit& b) { b.color.r = std::nanf(""); }, false);
    check([](PdfTextBlockEdit& b) { b.text.assign(kMaxTextBlockBytes, 'x'); }, true);
    check([](PdfTextBlockEdit& b) { b.text.assign(kMaxTextBlockBytes + 1, 'x'); }, false);
    check([](PdfTextBlockEdit& b) { b.font.kind = PdfFontRef::Kind::FromObject; b.font.sourceIndex = 2; }, true);
    check([](PdfTextBlockEdit& b) { b.font.kind = PdfFontRef::Kind::FromObject; b.font.sourceIndex = 3; }, false);
}

RIVET_TEST(contentValidateRigidPlacement) {
    auto check = [](const core::Matrix& m, bool expectValid) {
        PdfPageContentEdits edits;
        edits.textBlocks.push_back(block(1));
        edits.textBlocks[0].placement = m;
        CHECK_EQ(validate(edits, 0).has_value(), expectValid);
    };
    check(core::Matrix::identity(), true);
    check(core::Matrix::translation(100, 200), true);
    check(core::Matrix::translation(100, 200) * core::Matrix::rotation(0.7), true);
    check(core::Matrix::rotation(3.14159265358979 / 2), true);
    check(core::Matrix::scaling(2, 2), false);
    check(core::Matrix::scaling(-1, 1), false);   // mirror
    check(core::Matrix{1, 0, 0.5, 1, 0, 0}, false); // shear
    check(core::Matrix{1, 0, 0, 1, std::nan(""), 0}, false);
    check(core::Matrix{0, 0, 0, 0, 0, 0}, false);
}

RIVET_TEST(contentValidateTooManyBlocks) {
    PdfPageContentEdits edits;
    for (std::size_t i = 0; i < kMaxTextBlocksPerPage; ++i) edits.textBlocks.push_back(block(i + 1));
    CHECK(validate(edits, 0).has_value());
    edits.textBlocks.push_back(block(kMaxTextBlocksPerPage + 1));
    CHECK(isInvalidArgument(validate(edits, 0)));
}
