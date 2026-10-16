// SPDX-License-Identifier: MPL-2.0
#pragma once

// PDFium implementation of "Save Reduced Copy" (ADR-0028): downsampling and
// JPEG re-encoding of a page's top-level images. INTERNAL to
// src/pdf/pdfium/ (the only place FPDF_* may appear).
//
// Gate discipline: nothing here acquires the PDFium call gate; callers hold
// it. Only the assembly's private working document is mutated.

#include "core/Error.hpp"
#include "pdf/ImageReduce.hpp"

#include <cstddef>
#include <functional>

#include "fpdfview.h"

namespace rivet::pdf::internal {

// Reduces the images of the loaded `page` of `document` (the assembly's
// working document) and, when at least one image was replaced, regenerates
// the page's content streams. Returns the number of images replaced.
//
// An image is replaced when ALL of the following hold:
//   - it is a top-level image object (images inside Form XObjects are left
//     alone) of at least kMinReducibleImageSide px per side, with more than
//     1 bit per pixel;
//   - it has no mask / soft mask / alpha (checked on the rendered image: a
//     translucent pixel inside the image keeps it untouched) and its data is
//     not shared with another image object of the page;
//   - it exceeds the target resolution (pixels / placed inches), or it is
//     not stored as a JPEG, and the re-encoded JPEG is smaller than the
//     stored stream (and, when the pixel size is unchanged, at most 3/4 of
//     it: no lossy re-encode for a marginal gain).
// The object's placement matrix is kept.
//
// `regenerationSafe` is called at most once, right before the first
// replacement on the page; false skips the whole page (rewriting its content
// streams could change it, ADR-0017). Encoder failures skip the image
// (never fail the save); PDFium failures while replacing do fail it.
core::Result<std::size_t> reduceImagesOnPage(FPDF_DOCUMENT document, FPDF_PAGE page,
                                             const PdfImageReduction& options,
                                             const std::function<bool()>& regenerationSafe);

} // namespace rivet::pdf::internal
