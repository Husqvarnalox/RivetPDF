// SPDX-License-Identifier: MPL-2.0

#include "PdfiumWriting.h"

#include <cstring>
#include <optional>
#include <string>
#include <utility>

#include "fpdf_save.h" // FPDF_SaveAsCopy

namespace rivet::pdf::internal {
namespace {

core::Error internalError(std::string message) {
    return core::makeError(core::ErrorCode::Internal, std::move(message), "pdf");
}

// FPDF_FILEWRITE adapter forwarding to an IPdfByteSink. The first failure
// is recorded and every later block is refused, so nothing after a failed
// block ever reaches the sink.
struct SinkWriter final : FPDF_FILEWRITE {
    IPdfByteSink* sink = nullptr;
    std::optional<core::Error> error;
    bool threw = false;
    std::uint64_t bytesWritten = 0;
};

// Called from inside PDFium: never lets an exception escape.
int writeToSink(FPDF_FILEWRITE* self, const void* data, unsigned long size) noexcept {
    auto* writer = static_cast<SinkWriter*>(self);
    if (writer->error.has_value() || writer->threw) {
        return 0;
    }
    if (size == 0) {
        return 1;
    }
    try {
        core::Status status = writer->sink->write(data, static_cast<std::size_t>(size));
        if (!status.has_value()) {
            writer->error.emplace(std::move(status).error());
            return 0;
        }
    } catch (...) {
        writer->threw = true;
        return 0;
    }
    writer->bytesWritten += size;
    return 1;
}

} // namespace

core::Status saveDocumentToSink(FPDF_DOCUMENT document, IPdfByteSink& sink) {
    SinkWriter writer;
    writer.version = 1;
    writer.WriteBlock = &writeToSink;
    writer.sink = &sink;

    const bool saved = FPDF_SaveAsCopy(document, &writer, FPDF_NO_INCREMENTAL) != 0;
    // Checked BEFORE `saved`: the final flush's result is dropped by PDFium.
    if (writer.error.has_value()) {
        return std::unexpected(std::move(*writer.error));
    }
    if (writer.threw) {
        return std::unexpected(internalError("the output sink threw an exception"));
    }
    if (!saved || writer.bytesWritten == 0) {
        return std::unexpected(internalError("PDFium failed to serialize the document"));
    }
    return core::ok();
}

int readJpegBlock(void* param, unsigned long position, unsigned char* buffer, unsigned long size) {
    const auto* source = static_cast<const JpegSource*>(param);
    if (position > source->size || size > source->size - position) {
        return 0;
    }
    std::memcpy(buffer, source->data + position, size);
    return 1;
}

} // namespace rivet::pdf::internal
