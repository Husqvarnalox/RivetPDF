// SPDX-License-Identifier: MPL-2.0
#pragma once

// Test-only fixtures for the PDF signing engine: PDFs with a classic xref
// table or an xref stream (optionally with an object stream and a Flate +
// PNG-predictor xref stream) are generated here, never read from disk, plus
// signer identities fabricated with OpenSSL independently of the code under test.

#include "TestSupport.hpp"
#include "pdf/sign/PdfSigner.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace rivet::pdf::sign::testing {

using rivet::pdf::sign::Bytes;
using rivet::pdf::sign::ByteView;

inline Bytes toBytes(const std::string& s) { return Bytes(s.begin(), s.end()); }
inline std::string asString(const Bytes& b) { return std::string(b.begin(), b.end()); }

struct FixtureSpec {
    bool emptySigField = false;   // /Sig1 signature field on the page, listed in /AcroForm
    bool textField = false;       // text field /Name
    bool indirectAnnots = false;  // page /Annots is an indirect array
    bool textAnnot = false;       // an existing /Text annotation
    bool encrypted = false;       // trailer /Encrypt (the dictionary is a dummy; signing must refuse)
    std::size_t contentPadding = 0; // extra bytes in the page content stream
    bool xrefStream = false;
    bool objectStream = false;    // objects 1,2,3,5,6,7,8,9 live in an object stream (needs xrefStream)
    bool flateXref = false;       // xref stream stored as zlib (stored block) with PNG Up predictor
};

inline std::string zlibStored(const std::string& data) {
    std::string out;
    out.push_back(0x78);
    out.push_back(0x01);
    std::size_t pos = 0;
    do {
        const std::size_t n = std::min<std::size_t>(65535, data.size() - pos);
        const bool last = pos + n >= data.size();
        out.push_back(last ? 1 : 0);
        out.push_back(static_cast<char>(n & 0xFF));
        out.push_back(static_cast<char>(n >> 8));
        out.push_back(static_cast<char>(~n & 0xFF));
        out.push_back(static_cast<char>((~n >> 8) & 0xFF));
        out.append(data, pos, n);
        pos += n;
    } while (pos < data.size());
    std::uint32_t a = 1, b = 0;
    for (const char c : data) {
        a = (a + static_cast<std::uint8_t>(c)) % 65521;
        b = (b + a) % 65521;
    }
    const std::uint32_t adler = (b << 16) | a;
    for (int sh = 24; sh >= 0; sh -= 8) out.push_back(static_cast<char>((adler >> sh) & 0xFF));
    return out;
}

inline Bytes makeFixture(const FixtureSpec& s = {}) {
    std::map<std::uint32_t, std::string> dicts; // non-stream objects: body text
    std::string annots;
    auto addAnnot = [&](std::uint32_t n) { annots += std::to_string(n) + " 0 R "; };
    dicts[2] = "<< /Type /Pages /Kids [3 0 R] /Count 1 >>";
    std::string fields;
    if (s.emptySigField) {
        dicts[5] = "<< /Type /Annot /Subtype /Widget /FT /Sig /T (Sig1) /Rect [50 50 250 100] /P 3 0 R /F 4 >>";
        fields += "5 0 R ";
        addAnnot(5);
    }
    if (s.textField) {
        dicts[6] =
            "<< /Type /Annot /Subtype /Widget /FT /Tx /T (Name) /V () /Rect [50 120 250 150] /P 3 0 R /F 4 /DA (/Helv 0 Tf 0 g) >>";
        fields += "6 0 R ";
        addAnnot(6);
    }
    std::string catalog = "<< /Type /Catalog /Pages 2 0 R";
    if (s.emptySigField || s.textField) {
        dicts[7] = "<< /Fields [" + fields + "] /DA (/Helv 0 Tf 0 g) >>";
        catalog += " /AcroForm 7 0 R";
    }
    catalog += " >>";
    dicts[1] = catalog;
    if (s.textAnnot) {
        dicts[9] = "<< /Type /Annot /Subtype /Text /Rect [300 300 320 320] /Contents (note) /P 3 0 R >>";
        addAnnot(9);
    }
    std::string page = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << >>";
    if (s.indirectAnnots) {
        dicts[8] = "[" + annots + "]";
        page += " /Annots 8 0 R";
    } else if (!annots.empty()) {
        page += " /Annots [" + annots + "]";
    }
    page += " >>";
    dicts[3] = page;
    if (s.encrypted) dicts[10] = "<< /Filter /Standard /V 1 /R 2 /O (0123456789abcdef0123456789abcdef) /U (0123456789abcdef0123456789abcdef) /P -4 >>";
    dicts[11] = "<< /Producer (rivet-test) >>";

    std::string content = "BT /F1 12 Tf 72 720 Td (hello) Tj ET\n";
    if (s.contentPadding) {
        content.append(s.contentPadding, '%');
        content.back() = '\n';
    }
    const std::string stream4 = "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "endstream";

    const std::string header = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    const std::string id = "/ID [<0123456789ABCDEF0123456789ABCDEF> <0123456789ABCDEF0123456789ABCDEF>]";
    std::string out = header;

    if (!s.xrefStream) {
        std::map<std::uint32_t, std::string> all = dicts;
        all[4] = stream4;
        std::map<std::uint32_t, std::size_t> offsets;
        for (const auto& [n, body] : all) {
            offsets[n] = out.size();
            out += std::to_string(n) + " 0 obj\n" + body + "\nendobj\n";
        }
        const std::uint32_t maxNum = all.rbegin()->first;
        const std::size_t xrefPos = out.size();
        out += "xref\n0 " + std::to_string(maxNum + 1) + "\n";
        for (std::uint32_t n = 0; n <= maxNum; ++n) {
            char line[32];
            if (n == 0) std::snprintf(line, sizeof line, "0000000000 65535 f \n");
            else if (offsets.count(n)) std::snprintf(line, sizeof line, "%010zu 00000 n \n", offsets[n]);
            else std::snprintf(line, sizeof line, "0000000000 00000 f \n");
            out += line;
        }
        out += "trailer\n<< /Size " + std::to_string(maxNum + 1) + " /Root 1 0 R /Info 11 0 R " + id;
        if (s.encrypted) out += " /Encrypt 10 0 R";
        out += " >>\nstartxref\n" + std::to_string(xrefPos) + "\n%%EOF\n";
        return toBytes(out);
    }

    // xref stream
    std::map<std::uint32_t, std::pair<int, std::pair<std::size_t, std::uint32_t>>> entries; // type, (a, b)
    std::map<std::uint32_t, std::string> plain;
    plain[4] = stream4;
    constexpr std::uint32_t kObjStm = 12, kXref = 13;
    std::map<std::uint32_t, std::string> inStream;
    for (const auto& [n, body] : dicts) {
        if (s.objectStream && n != 10 && n != 11) inStream[n] = body;
        else plain[n] = body;
    }
    if (!inStream.empty()) {
        std::string index, bodies;
        std::uint32_t idx = 0;
        for (const auto& [n, body] : inStream) {
            index += std::to_string(n) + " " + std::to_string(bodies.size()) + " ";
            bodies += body + "\n";
            entries[n] = {2, {kObjStm, idx++}};
        }
        plain[kObjStm] = "<< /Type /ObjStm /N " + std::to_string(inStream.size()) + " /First " +
                         std::to_string(index.size()) + " /Length " + std::to_string(index.size() + bodies.size()) +
                         " >>\nstream\n" + index + bodies + "endstream";
    }
    for (const auto& [n, body] : plain) {
        entries[n] = {1, {out.size(), 0}};
        out += std::to_string(n) + " 0 obj\n" + body + "\nendobj\n";
    }
    const std::size_t xrefPos = out.size();
    entries[kXref] = {1, {xrefPos, 0}};
    std::string rows;
    std::string prevRow(7, '\0');
    for (std::uint32_t n = 0; n < kXref + 1; ++n) {
        std::string row(7, '\0');
        if (n == 0) {
            row[0] = 0;
            row[5] = static_cast<char>(0xFF);
            row[6] = static_cast<char>(0xFF);
        } else if (entries.count(n)) {
            const auto& e = entries[n];
            row[0] = static_cast<char>(e.first);
            for (int i = 0; i < 4; ++i) row[1 + i] = static_cast<char>((e.second.first >> (8 * (3 - i))) & 0xFF);
            row[5] = static_cast<char>((e.second.second >> 8) & 0xFF);
            row[6] = static_cast<char>(e.second.second & 0xFF);
        }
        if (s.flateXref) {
            rows.push_back(2); // PNG Up
            for (int i = 0; i < 7; ++i) rows.push_back(static_cast<char>(static_cast<std::uint8_t>(row[i]) - static_cast<std::uint8_t>(prevRow[i])));
            prevRow = row;
        } else {
            rows += row;
        }
    }
    std::string data = s.flateXref ? zlibStored(rows) : rows;
    out += std::to_string(kXref) + " 0 obj\n<< /Type /XRef /Size " + std::to_string(kXref + 1) +
           " /W [1 4 2] /Root 1 0 R /Info 11 0 R " + id;
    if (s.flateXref) out += " /Filter /FlateDecode /DecodeParms << /Predictor 12 /Columns 7 >>";
    if (s.encrypted) out += " /Encrypt 10 0 R";
    out += " /Length " + std::to_string(data.size()) + " >>\nstream\n" + data + "\nendstream\nendobj\n";
    out += "startxref\n" + std::to_string(xrefPos) + "\n%%EOF\n";
    return toBytes(out);
}

// Appends a classic-table revision (test helper for simulating later edits):
// `objects` are "n g obj" bodies (without the obj/endobj wrapper).
inline Bytes appendRevision(Bytes base, const std::vector<std::pair<std::uint32_t, std::string>>& objects,
                            const std::string& extraTrailer = {}) {
    auto file = PdfFile::open(base);
    const std::uint64_t prev = file->lastStartxref();
    const std::uint64_t size = file->nextObjectNumber();
    std::string root = "1 0 R";
    const auto trailer = file->mergedTrailer(); // by value: keep it alive while `r` is used
    if (const PdfObject* r = trailer.find("Root"); r && r->isRef())
        root = std::to_string(r->ref.num) + " 0 R";
    std::string out;
    if (base.back() != '\n') out += "\n";
    std::map<std::uint32_t, std::size_t> offsets;
    for (const auto& [n, body] : objects) {
        offsets[n] = base.size() + out.size();
        out += std::to_string(n) + " 0 obj\n" + body + "\nendobj\n";
    }
    const std::size_t xrefPos = base.size() + out.size();
    out += "xref\n";
    for (const auto& [n, off] : offsets) {
        char line[64];
        std::snprintf(line, sizeof line, "%u 1\n%010zu 00000 n \n", n, off);
        out += line;
    }
    std::uint64_t newSize = size;
    for (const auto& [n, off] : offsets) {
        (void)off;
        newSize = std::max<std::uint64_t>(newSize, n + 1ull);
    }
    out += "trailer\n<< /Size " + std::to_string(newSize) + " /Root " + root + " /Prev " + std::to_string(prev) + " " +
           extraTrailer + " >>\nstartxref\n" + std::to_string(xrefPos) + "\n%%EOF\n";
    base.insert(base.end(), out.begin(), out.end());
    return base;
}

struct Signer {
    std::shared_ptr<crypto::CryptoProvider> provider = crypto::makeDefaultProvider();
    crypto::testing::TestKey key;
    std::unique_ptr<crypto::SigningIdentity> identity;
    crypto::TrustAnchorSet anchors;

    explicit Signer(const std::string& cn = "Rivet Test Signer", const std::string& keyKind = "rsa2048") {
        crypto::testing::CertSpec spec;
        spec.cn = cn;
        spec.keyKind = keyKind;
        key = crypto::testing::makeCert(spec);
        identity = crypto::testing::makeIdentity(*provider, key);
        if (auto c = provider->parseCertificate(crypto::testing::certDer(key))) anchors.add(*c);
    }
};

inline VerifyOptions trusting(const Signer& s) {
    VerifyOptions o;
    o.anchors = &s.anchors;
    return o;
}

inline std::vector<PdfSignatureInfo> verifyAll(const Bytes& pdf, Signer& s, bool trust = false) {
    auto r = verifyPdfSignatures(pdf, *s.provider, trust ? trusting(s) : VerifyOptions{});
    if (!r) return {};
    return std::move(*r);
}

inline SignRequest basicRequest(SignatureProfile profile = SignatureProfile::PadesBaselineB) {
    SignRequest r;
    r.profile = profile;
    return r;
}

inline std::filesystem::path scratchDir(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / ("rivet-pdf-sign-" + name);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

// Runs a shell command, returns its exit status and the combined output.
inline int runCommand(const std::string& command, std::string& output) {
#if defined(_WIN32)
    (void)command;
    output.clear();
    return -1;
#else
    const auto outFile = std::filesystem::temp_directory_path() / "rivet-pdf-sign-cmd.txt";
    const int rc = std::system((command + " > '" + outFile.string() + "' 2>&1").c_str());
    std::ifstream in(outFile);
    output.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    std::filesystem::remove(outFile);
    return rc;
#endif
}

inline bool toolAvailable(const char* name) {
    std::string out;
    return runCommand(std::string("command -v ") + name, out) == 0;
}

} // namespace rivet::pdf::sign::testing
