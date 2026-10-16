// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Bitmap.hpp"
#include "core/Error.hpp"
#include "core/geometry/Point.hpp"

#include <array>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace rivet::platform {

// One recognized word. `quad` is the word's box in the PIXELS of the
// recognized bitmap (origin top-left, y-down), vertices in reading order of
// the text as it appears in the bitmap: top-left, top-right, bottom-right,
// bottom-left (so a slightly rotated line keeps its angle). `text` is UTF-8
// without whitespace.
struct RecognizedWord {
    std::string text;
    std::array<core::Point, 4> quad{};
    float confidence = 0.0F; // 0..1; 1 when the engine reports none
};

struct RecognizedLine {
    std::vector<RecognizedWord> words; // reading order
    float confidence = 0.0F;
};

struct RecognizedPage {
    std::vector<RecognizedLine> lines; // reading order
};

// Optical character recognition (ADR-0027), implemented per platform (Vision
// on macOS; no engine on Linux and Windows yet, where the service pointer in
// ShellServices stays null). No OCR engine is bundled.
//
// Contract:
//   - Thread-safe and re-entrant: called on worker threads, never on the main
//     thread. It must not call back into the PDF layer.
//   - `languages` are BCP-47 tags in priority order (for example "en-US",
//     "ru-RU"); tags the engine does not support are ignored, and an empty
//     list (or none supported) selects the engine's default.
//   - Cancellation: when `stop` is requested the call returns
//     ErrorCode::Cancelled as soon as the engine lets it; a result computed
//     after the request is discarded.
//   - Failures are ErrorCode::Internal / Unsupported / InvalidArgument. No
//     exceptions escape; recognized text is NEVER logged or put in an error
//     message.
class ITextRecognizer {
public:
    virtual ~ITextRecognizer() = default;

    virtual core::Result<RecognizedPage> recognize(const core::Bitmap& bitmap,
                                                   std::span<const std::string> languages,
                                                   std::stop_token stop) const = 0;
};

} // namespace rivet::platform
