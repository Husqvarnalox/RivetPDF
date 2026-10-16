# ADR-0027: OCR text layer

Status: Accepted

Date: 2026-10-16

## Decision

1. `platform/TextRecognizer.hpp`: `ITextRecognizer::recognize(const
   core::Bitmap&, languages) -> Result<RecognizedPage>` (lines -> words
   with UTF-8 text, bitmap-space quads, confidence). Thread-safe, called on
   workers; cancellable through a stop token. Implementations: macOS
   Vision (`VNRecognizeTextRequest`, accurate level); Windows
   `Windows.Media.Ocr` via C++/WinRT where available; Linux: none
   (service null -> command disabled with a reason). No bundled OCR
   engine (no Tesseract dependency).
2. "Recognize Text" renders each target page at 300 dpi through the
   existing renderer, recognizes off the main thread with progress and
   cancel, then builds one content edit per page: invisible text (render
   mode 3) in the bundled Sans face, one text object per word, scaled
   horizontally to the word box. It is a normal undoable content edit,
   saved through the content pipeline.
3. Pages that already have extractable text are skipped unless the user
   chooses "All pages". Recognition never logs text.

## Implementation notes

- **Interface.** `platform/TextRecognizer.hpp` as decided;
  `ShellServices::textRecognizer` is nullable. macOS: `MacosTextRecognizer`
  (Vision, accurate level, language correction on). The requested languages
  are `en-US`, `ru-RU`, intersected with what the installed Vision revision
  supports (an empty intersection selects Vision's default). Each word is
  cut from the best candidate at whitespace and located with
  `boundingBoxForRange:`. Cancellation calls `-[VNRequest cancel]` from a
  `std::stop_callback`.
- **Windows: not implemented.** The service stays null there, so the menu
  items are disabled. Follow-up: a `Windows.Media.Ocr` recognizer needs
  C++/WinRT (not used by the Windows shell today) and the language packs the
  user has installed; it plugs in through the same interface.
- **Linux:** null by design.
- **Planning is pure.** `editor/OcrLayer` maps recognized boxes to runs:
  bitmap pixels -> display points (`ocrWordFromBitmap`) -> source user space
  through `userToDisplayMatrix(view).inverted()`, so /Rotate and a CropBox
  offset are handled by the one central mapping. Per word: font size = box
  height, baseline a fifth of the height above the bottom edge, horizontal
  scale = box width / natural advance in the bundled Sans face (limits
  0.01..100). Characters the face cannot write are dropped from a word; words
  left empty are skipped.
- **Edit model.** `PdfPageContentEdits::invisibleRuns` (+ `invisibleTag`,
  the origin tag of the created objects) is validated by `pdf::validate`
  (at most 8192 runs, 256 bytes each, no control characters, rigid
  placement) and applied by the same `applyContentEdits` for display and
  save, after text blocks and before redactions. The objects are
  `Created`-origin text objects in render mode 3, so they are read-only in
  the editor and never painted.
- **Command.** `editor::addInvisibleText` is one undoable edit per page and
  replaces an OCR layer Rivet added to that page earlier. `app::OcrController`
  (Content menu: "Recognize Text (Pages Without Text)" / "Recognize Text
  (All Pages)") renders at 300 dpi on a worker, recognizes there, and applies
  each page's edit on the main thread as it completes (progress in the
  status bar). Esc and closing the tab cancel; pages already done stay, each
  its own undo step. A page whose content is still being extracted is retried
  every 20 ms. Text is never logged or put in a message.
- **Limits.** "All pages" does not remove hidden text from other OCR tools,
  so it can duplicate it. Rotated scans (text not upright on the displayed
  page) are located by Vision's own orientation handling only; no deskew or
  page-orientation detection is done. Vision accuracy on real scans has not
  been measured (tests use a fake recognizer).
