// SPDX-License-Identifier: MPL-2.0
#pragma once

// Bounded zlib/DEFLATE decoder (RFC 1950/1951) and PNG-predictor reversal
// for the two stream kinds the signing engine must read: cross-reference
// streams and object streams. Dependency-free by design; output is capped.

#include "pdf/sign/PdfObject.hpp"

namespace rivet::pdf::sign {

// Decodes a zlib stream. Fails with LimitExceeded when the output would pass
// `maxOutput`, MalformedPdf on any structural error. The Adler-32 trailer is
// not verified (the data is untrusted either way; structure is what matters).
SignResult<Bytes> inflateZlib(ByteView input, std::size_t maxOutput);

// Reverses a PNG predictor (Predictor >= 10) with 8-bit components.
SignResult<Bytes> undoPngPredictor(ByteView data, unsigned columns, unsigned colors);

} // namespace rivet::pdf::sign
