// SPDX-License-Identifier: MPL-2.0
#pragma once

// Bounded reader of the PDF file structure the signing engine needs
// (ADR-0037): header, startxref, the cross-reference chain (tables, xref
// streams, hybrid /XRefStm), indirect objects (including objects inside
// object streams), the page tree and the AcroForm field tree. It reads raw
// bytes only, never rebuilds the document, and treats every byte as hostile.
//
// A "view" is the document as of a cross-reference section: view(i) sees
// sections i..end of the chain (section 0 is the newest revision). The
// modification analysis uses this to compare a signed revision with later
// ones.

#include "pdf/sign/PdfObject.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace rivet::pdf::sign {

inline constexpr std::size_t kMaxXrefSections = 8192;
inline constexpr std::size_t kMaxXrefEntries = std::size_t{1} << 24;
inline constexpr std::size_t kMaxObjectStreamBytes = std::size_t{256} << 20;
inline constexpr std::size_t kMaxFieldNodes = 200000;
inline constexpr std::uint64_t kMaxSignableFileSize = 0x7FFFFFFFull; // ByteRange numbers must fit int32 for all readers

struct XrefEntry {
    enum class Type : std::uint8_t { Free, InUse, Compressed };
    Type type = Type::Free;
    std::uint64_t a = 0; // InUse: file offset; Compressed: object stream number
    std::uint32_t b = 0; // InUse: generation;  Compressed: index in the stream
};

struct XrefSection {
    std::uint64_t offset = 0;
    bool isStream = false;
    bool hybrid = false; // a classic table that also carries /XRefStm
    std::uint32_t streamObjNum = 0;
    std::map<std::uint32_t, XrefEntry> entries; // table entries first, then /XRefStm entries not already present
    PdfDict trailer;                            // trailer dictionary or xref-stream dictionary
    std::uint64_t eofEnd = 0;                   // offset right after this revision's "%%EOF"; 0 when not found
};

struct PageRef {
    PdfRef ref;
    PdfDict dict;
};

struct FieldInfo {
    std::optional<PdfRef> ref; // absent for a direct field dictionary
    PdfDict dict;
    std::string fullName;      // UTF-8, '.'-joined partial names
    std::string fieldType;     // inherited /FT without the slash ("Sig", "Tx", ...), empty if unknown
    std::vector<PdfRef> widgets; // /Kids entries that are widgets of this terminal field (empty: merged widget)
    std::uint32_t flags = 0;   // inherited /Ff
};

class PdfFile {
public:
    // Parses header, startxref and the whole xref chain.
    static SignResult<PdfFile> open(ByteView data);

    ByteView data() const { return data_; }
    std::size_t sectionCount() const { return sections_.size(); }
    const XrefSection& section(std::size_t i) const { return sections_[i]; }
    std::uint64_t lastStartxref() const { return lastStartxref_; }
    // "1.7" from the %PDF- header (empty when unreadable).
    const std::string& headerVersion() const { return headerVersion_; }

    // Trailer entries as seen from `from` (first section that has the key wins).
    PdfDict mergedTrailer(std::size_t from = 0) const;
    bool isEncrypted() const;
    // Highest object number defined in the view + 1, at least the trailer /Size.
    std::uint64_t nextObjectNumber(std::size_t from = 0) const;

    std::optional<XrefEntry> lookup(std::uint32_t num, std::size_t from = 0) const;

    // Value of an object (the dictionary of a stream object); nullopt when it
    // is free or undefined in the view.
    SignResult<std::optional<PdfObject>> loadObject(std::uint32_t num, std::size_t from = 0);
    // Follows references (bounded); Null when anything fails.
    PdfObject resolve(const PdfObject& object, std::size_t from = 0);
    // Raw framing of an in-use, non-compressed stream object.
    SignResult<IndirectObject> loadIndirect(std::uint32_t num, std::size_t from = 0);
    // Decodes a stream with no filter or FlateDecode (+ PNG predictor).
    SignResult<Bytes> decodeStream(const IndirectObject& stream, std::size_t maxOutput);

    // Catalog dictionary and its reference.
    SignResult<std::pair<PdfRef, PdfDict>> catalog(std::size_t from = 0);
    // Zero-based leaf page of the page tree.
    SignResult<PageRef> findPage(std::size_t index, std::size_t from = 0);
    // Terminal fields of /AcroForm /Fields. Empty when there is no AcroForm.
    SignResult<std::vector<FieldInfo>> fields(std::size_t from = 0);

private:
    struct ObjectStream {
        Bytes data;
        std::vector<std::pair<std::uint32_t, std::uint64_t>> index; // object number, offset relative to First
        std::uint64_t first = 0;
    };

    SignResult<std::shared_ptr<ObjectStream>> loadObjectStream(std::uint32_t stmNum, std::size_t from);
    SignResult<XrefSection> parseSection(std::uint64_t offset);
    SignResult<void> parseTable(XrefSection& s, std::uint64_t offset);
    SignResult<void> parseStreamSection(XrefSection& s, std::uint64_t offset);
    void mergeXrefStream(XrefSection& into, const XrefSection& stream);

    ByteView data_;
    std::vector<XrefSection> sections_;
    std::uint64_t lastStartxref_ = 0;
    std::string headerVersion_;
    std::map<std::uint64_t, std::shared_ptr<ObjectStream>> objStreamCache_;
    int resolveDepth_ = 0;
};

// Decodes a PDF text string (UTF-16BE with BOM, UTF-8 with BOM, else
// PDFDocEncoding approximated as Latin-1) to UTF-8. Never fails.
std::string textStringToUtf8(std::string_view bytes);

// Convenience accessors on a dictionary (all tolerate wrong types).
std::optional<std::int64_t> dictInt(const PdfDict& d, std::string_view key);

} // namespace rivet::pdf::sign
