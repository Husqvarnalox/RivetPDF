// SPDX-License-Identifier: MPL-2.0
#pragma once

// Shared helpers of the PDF encryption tests: a tiny classic-xref PDF builder
// shaped like PDFium's output and a password-based decryptor that follows the
// standard (used to check what the encryptor wrote, independent of any reader).

#include "crypto/Symmetric.hpp"
#include "pdf/encrypt/PdfEncrypt.hpp"
#include "pdf/encrypt/StandardHandler.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace rivet::test::enc {

namespace pe = rivet::pdf::encrypt;

inline std::string toStr(const std::vector<std::uint8_t>& b) { return std::string(b.begin(), b.end()); }
inline std::vector<std::uint8_t> toBytes(const std::string& s) { return std::vector<std::uint8_t>(s.begin(), s.end()); }

// bodies[i] is object i+1 (without the "n 0 obj" wrapper). `trailerExtra` is
// appended inside the trailer dictionary.
inline std::string buildPdf(const std::vector<std::string>& bodies, const std::string& trailerExtra = {},
                            const std::string& header = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n") {
    std::string out = header;
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < bodies.size(); ++i) {
        offsets.push_back(out.size());
        out += std::to_string(i + 1) + " 0 obj\n" + bodies[i] + "\nendobj\n";
    }
    const std::size_t xref = out.size();
    out += "xref\n0 " + std::to_string(bodies.size() + 1) + "\n0000000000 65535 f \n";
    for (const std::size_t offset : offsets) {
        char line[32];
        std::snprintf(line, sizeof line, "%010zu 00000 n \n", offset);
        out += line;
    }
    out += "trailer\n<< /Size " + std::to_string(bodies.size() + 1) + " /Root 1 0 R " + trailerExtra +
           " >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return out;
}

inline std::string stream(const std::string& dictEntries, const std::string& data) {
    return "<< " + dictEntries + " /Length " + std::to_string(data.size()) + " >>\nstream\n" + data + "\nendstream";
}

constexpr const char* kBody = "BT /F1 12 Tf 72 700 Td (Secret body text) Tj ET";
constexpr const char* kXmp = "<?xpacket begin?><x:xmpmeta>SECRET-XMP-MARKER</x:xmpmeta><?xpacket end?>";

// Catalog(1) Pages(2) Page(3) Content(4, indirect /Length 7) Info(5) Metadata(6) Length(7).
inline std::string samplePdf(const std::string& trailerExtra = "/Info 5 0 R") {
    const std::string body = kBody;
    return buildPdf(
        {"<< /Type /Catalog /Pages 2 0 R /Metadata 6 0 R >>", "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
         "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Contents 4 0 R >>",
         "<< /Length 7 0 R >>\nstream\n" + body + "\nendstream",
         "<< /Title (Caf\\351 \\(draft\\) \\101\\\nB) /Author <FEFF0041004C> /Odd <4> >>",
         stream("/Type /Metadata /Subtype /XML", kXmp), std::to_string(body.size())},
        trailerExtra);
}

inline std::string hexAfter(const std::string& s, const std::string& key, std::size_t from = 0) {
    const std::size_t k = s.find(key + " <", from);
    if (k == std::string::npos) return {};
    const std::size_t b = k + key.size() + 2;
    const std::size_t e = s.find('>', b);
    std::string raw = s.substr(b, e - b);
    std::string out;
    for (std::size_t i = 0; i + 1 < raw.size(); i += 2) out += static_cast<char>(std::stoi(raw.substr(i, 2), nullptr, 16));
    return out;
}

inline crypto::ByteView view(const std::string& s) { return crypto::viewOf(s); }

// File key of an AES-256 (R6) output opened with the user or owner password;
// empty when the password does not validate.
inline std::string r6FileKey(const std::string& pdf, const std::string& password, bool owner) {
    const std::string u = hexAfter(pdf, "/U");
    const std::string o = hexAfter(pdf, "/O");
    const std::string ue = hexAfter(pdf, "/UE");
    const std::string oe = hexAfter(pdf, "/OE");
    auto pw = pe::prepareR6Password(password);
    if (!pw || u.size() != 48 || o.size() != 48) return {};
    const std::string& entry = owner ? o : u;
    const std::string extra = owner ? u : std::string();
    auto check = pe::hashR6(*pw, view(entry.substr(32, 8)), view(extra));
    if (!check || toStr(*check) != entry.substr(0, 32)) return {};
    auto key = pe::hashR6(*pw, view(entry.substr(40, 8)), view(extra));
    if (!key) return {};
    const std::vector<std::uint8_t> zeroIv(16, 0);
    auto fileKey = crypto::aesDecrypt(crypto::AesMode::Cbc, *key, zeroIv, view(owner ? oe : ue), false);
    return fileKey ? toStr(*fileKey) : std::string();
}

inline std::string decryptWith(const std::string& key, const std::string& ivAndCipher) {
    if (ivAndCipher.size() < 32) return "<short>";
    auto plain = crypto::aesDecrypt(crypto::AesMode::Cbc, view(key), view(ivAndCipher.substr(0, 16)),
                                    view(ivAndCipher.substr(16)), true);
    return plain ? toStr(*plain) : std::string("<bad padding>");
}

// Raw (still encrypted) data of the stream of object `num` in `pdf`.
inline std::string rawStream(const std::string& pdf, int num) {
    const std::size_t o = pdf.find("\n" + std::to_string(num) + " 0 obj");
    if (o == std::string::npos) return {};
    std::size_t b = pdf.find("stream\n", o);
    if (b == std::string::npos) return {};
    b += 7;
    const std::size_t e = pdf.find("\nendstream", b);
    return pdf.substr(b, e - b);
}

} // namespace rivet::test::enc
