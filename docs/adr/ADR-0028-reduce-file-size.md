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
