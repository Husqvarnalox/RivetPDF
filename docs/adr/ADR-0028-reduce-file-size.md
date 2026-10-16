# ADR-0028: Reduce file size

Status: Accepted

Date: 2026-10-16

## Decision

"Save Reduced Copy…" is a Save As variant with a preset (High quality:
200 dpi, JPEG q85; Medium: 150 dpi, q75; Small: 100 dpi, q60). At assembly
time, for every image object whose effective resolution (pixels / placed
size in inches) exceeds the preset dpi, or which is not already JPEG, the
backend decodes it, downsamples (box filter, portable code in `rivet_pdf`),
re-encodes as JPEG through the existing image-replacement path and keeps the
original placement matrix. Images with masks/alpha, 1-bit images and
images smaller than 64 px are left untouched. A non-incremental save
already drops unreferenced objects. The status bar reports old -> new size.
JPEG encoding uses the platform image codec (`IImageEncoder`: ImageIO,
WIC, libjpeg) - no new dependency.

## Implementation notes

- Menu: three items, "Save Reduced Copy (High / Medium / Small)...". It is an
  export: the tab, its path and its dirty state do not change, and the open
  document's own path is refused as a destination.
- Reduction runs inside the PDFium backend on top-level image objects of
  pages the ADR-0017 regeneration probe accepts (checked lazily, once per
  page). Masked or alpha images (detected from the rendered bitmap), 1-bit
  images, images under 64 px, images whose data is shared with another image
  object, JPEGs that need no resampling, and results not smaller than the
  stored stream are left untouched. Rotated placements use the matrix column
  lengths for the effective resolution; masks under rotation may be skipped
  conservatively.
- `pdf::IJpegEncoder` (portable) is adapted by `app::JpegEncoderAdapter` over
  `platform::IImageEncoder`: ImageIO (macOS), libjpeg (Linux). Windows
  provides none yet, so the commands are disabled there.
- The status bar shows the on-disk source size, the new size and the number
  of images re-encoded.
