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
