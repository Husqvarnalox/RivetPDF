# Changelog

## [0.1.0] - First public preview

Initial public release. macOS is the only desktop shell; portable core layers also build on Linux (GCC).

- PDF viewer on PDFium: tabs, thumbnails, outline, search, text selection and copy, printing, tile-based rendering with a bounded cache.
- Page editing: reorder, rotate, delete, duplicate, crop, insert/merge, extract, split, with undo/redo.
- Background Save / Save As with atomic file replacement.
- Annotations: highlight, underline, strikeout, notes, ink, shapes, stamps.
- Content editing: select, move, resize and delete page objects; retype existing text in place; add text; replace images.
- Markdown documents: Rendered, Source (live preview) and Split views, source-text editing with undo/redo and background save.
- Custom retained-mode UI toolkit; AppKit/CoreGraphics/CoreText confined to the macOS platform layer.

Not implemented: Markdown WYSIWYG/export/print, forms, signatures, OCR, redaction, compression, encryption, Windows/Linux shells. See the README for known limitations.
