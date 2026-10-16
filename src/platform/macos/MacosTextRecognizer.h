// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/TextRecognizer.hpp"

namespace rivet::platform {

// Vision-backed ITextRecognizer (VNRecognizeTextRequest, accurate level,
// language correction on). Stateless: every call creates its own request and
// handler, so recognize() is safe to call concurrently from worker threads.
class MacosTextRecognizer final : public ITextRecognizer {
public:
    core::Result<RecognizedPage> recognize(const core::Bitmap& bitmap, std::span<const std::string> languages,
                                           std::stop_token stop) const override;
};

} // namespace rivet::platform
