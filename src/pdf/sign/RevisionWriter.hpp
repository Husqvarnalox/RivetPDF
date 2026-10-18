// SPDX-License-Identifier: MPL-2.0
#pragma once

// Appends one incremental revision (objects + cross-reference section +
// trailer) to an existing PDF without touching any existing byte (ADR-0037,
// ADR-0045). Used for revisions that carry no signature placeholder, such as
// the /DSS validation-data revision. The signer's own layout code
// (PdfSigner.cpp) writes the same structures around its /Contents hole.

#include "pdf/sign/PdfFile.hpp"

#include <string>
#include <vector>

namespace rivet::pdf::sign {

// One object of the new revision whose body is already serialized.
struct NewObject {
    std::uint32_t num = 0;
    std::uint32_t gen = 0;
    std::string head;       // serialized value; a stream object's dictionary
    std::string streamData; // raw stream bytes (stream == true)
    bool stream = false;
};

NewObject plainObject(std::uint32_t num, std::uint32_t gen, const PdfObject& value);
// Uncompressed stream with /Length (extra entries may be given; /Length is overwritten).
NewObject streamObject(std::uint32_t num, std::string data, PdfDict extra = {});

// Generation number of an in-use object (0 otherwise).
std::uint32_t generationOf(PdfFile& f, std::uint32_t num);

class RevisionWriter {
public:
    // Captures everything needed from `f` (previous startxref, trailer keys,
    // xref style, next object number); `f` may be destroyed afterwards.
    // Refuses encrypted documents and files whose trailer has no /Root.
    static SignResult<RevisionWriter> begin(PdfFile& f);

    SignResult<std::uint32_t> allocate();
    void add(NewObject object);
    bool empty() const { return objects_.empty(); }
    bool classicXref() const { return xrefNum_ == 0; }

    // Appends the revision to `pdf`, which must be the exact bytes `f` was opened on.
    SignOk appendTo(Bytes& pdf);

private:
    std::vector<NewObject> objects_;
    std::uint64_t next_ = 0;
    std::uint64_t prev_ = 0;
    bool streamXref_ = false;
    std::uint32_t xrefNum_ = 0;
    PdfDict trailerKeys_;
};

} // namespace rivet::pdf::sign
