// SPDX-License-Identifier: MPL-2.0
// Interop of the post-save encryptor with PDFium (ADR-0046): a PDFium-saved
// document is encrypted and reopened through the real engine with the user
// and the owner password; page count, text and rendering must equal the
// unencrypted original. Matrix: {AES-256, AES-128} x {user only, user+owner,
// owner only} x {metadata encrypted, not encrypted}.

#include "RivetTest.h"

#include "PdfFixtures.hpp"

#include "core/Bitmap.hpp"
#include "core/Error.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/PdfAssembly.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfSystem.hpp"
#include "pdf/encrypt/PdfEncrypt.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace core = rivet::core;
namespace pe = rivet::pdf::encrypt;
using namespace rivet::pdf;

class MemorySink final : public IPdfByteSink {
public:
    core::Status write(const void* data, std::size_t size) override {
        bytes_.append(static_cast<const char*>(data), size);
        return core::ok();
    }
    const std::string& bytes() const { return bytes_; }

private:
    std::string bytes_;
};

fs::path tempPath() {
    static std::atomic<int> counter{0};
    return fs::temp_directory_path() / ("rivet-enc-" + std::to_string(counter.fetch_add(1)) + "-" +
                                        std::to_string(std::filesystem::hash_value(fs::temp_directory_path())) + ".pdf");
}

core::Result<std::unique_ptr<PdfDocument>> openBytes(PdfEngine& engine, const std::string& bytes, std::string_view password) {
    const fs::path path = tempPath();
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    auto opened = engine.openDocument(path, password);
    std::error_code ignored;
    fs::remove(path, ignored);
    return opened;
}

// PDFium's own save of every page of `source` (the input format of the encryptor).
std::string pdfiumSave(PdfEngine& engine, PdfDocument& source) {
    PdfAssemblyRequest request;
    request.mode = PdfAssemblyRequest::Mode::PreserveBase;
    request.base = &source;
    for (std::size_t i = 0; i < source.info().pageCount; ++i) {
        PdfAssemblyPage page;
        page.source = &source;
        page.sourcePageIndex = i;
        auto info = source.pageInfo(i);
        CHECK(info.has_value());
        if (info) page.view = info->view;
        request.pages.push_back(std::move(page));
    }
    MemorySink sink;
    const auto status = engine.assembleDocument(request, sink);
    CHECK(status.has_value());
    return sink.bytes();
}

struct Snapshot {
    std::size_t pages = 0;
    std::vector<std::string> texts;
    std::vector<core::Bitmap> bitmaps;
};

Snapshot snapshot(PdfDocument& doc) {
    Snapshot s;
    s.pages = doc.info().pageCount;
    for (std::size_t i = 0; i < s.pages; ++i) {
        auto text = doc.textPage(i);
        CHECK(text.has_value());
        s.texts.push_back(text ? (*text)->text() : std::string());
        auto info = doc.pageInfo(i);
        CHECK(info.has_value());
        if (!info) continue;
        auto bmp = doc.renderPage(i, core::Rect(0, 0, info->sizePoints.width, info->sizePoints.height), 1.0);
        CHECK(bmp.has_value());
        if (bmp) s.bitmaps.push_back(std::move(*bmp));
    }
    return s;
}

bool sameBitmap(const core::Bitmap& a, const core::Bitmap& b) {
    return a.width() == b.width() && a.height() == b.height() && a.stride() == b.stride() &&
           std::memcmp(a.data(), b.data(), a.sizeBytes()) == 0;
}

void expectSame(const Snapshot& expected, PdfDocument& doc, const char* what) {
    const Snapshot got = snapshot(doc);
    std::fprintf(stderr, "    %s: pages %zu/%zu\n", what, got.pages, expected.pages);
    CHECK_EQ(got.pages, expected.pages);
    CHECK_EQ(got.texts.size(), expected.texts.size());
    CHECK_EQ(got.bitmaps.size(), expected.bitmaps.size());
    for (std::size_t i = 0; i < expected.texts.size() && i < got.texts.size(); ++i) {
        if (got.texts[i] != expected.texts[i]) std::fprintf(stderr, "    %s: page %zu text differs\n", what, i);
        CHECK_EQ(got.texts[i], expected.texts[i]);
    }
    for (std::size_t i = 0; i < expected.bitmaps.size() && i < got.bitmaps.size(); ++i) {
        const bool same = sameBitmap(got.bitmaps[i], expected.bitmaps[i]);
        if (!same) std::fprintf(stderr, "    %s: page %zu render differs\n", what, i);
        CHECK(same);
    }
}

} // namespace

RIVET_TEST(pdfiumOpensEveryEncryptionVariantWithTheRightPasswords) {
    auto engine = createEngine();
    CHECK(engine != nullptr);
    if (!engine || !engine->isAvailable()) return;

    auto original = openBytes(*engine, rivet::test::pdffix::richPdf(), {});
    CHECK(original.has_value());
    if (!original) return;
    const std::string saved = pdfiumSave(*engine, **original);
    auto reopened = openBytes(*engine, saved, {});
    CHECK(reopened.has_value());
    if (!reopened) return;
    const Snapshot reference = snapshot(**reopened);
    CHECK_EQ(reference.pages, std::size_t{2});
    CHECK(reference.texts[0].find("Hello World") != std::string::npos);

    struct Passwords {
        const char* user;
        const char* owner;
    };
    const Passwords variants[] = {{"user-pw", ""}, {"user-pw", "owner-pw"}, {"", "owner-pw"}};
    for (const pe::Cipher cipher : {pe::Cipher::Aes256, pe::Cipher::Aes128}) {
        for (const Passwords& pw : variants) {
            for (const bool encryptMetadata : {true, false}) {
                pe::EncryptOptions o;
                o.cipher = cipher;
                o.userPassword = pw.user;
                o.ownerPassword = pw.owner;
                o.encryptMetadata = encryptMetadata;
                o.permissions.copy = false; // restrictions must not stop PDFium opening the file
                std::fprintf(stderr, "  variant: %s user='%s' owner='%s' metadata=%d\n",
                             cipher == pe::Cipher::Aes256 ? "AES-256" : "AES-128", pw.user, pw.owner, encryptMetadata);

                auto enc = pe::encryptPdf(std::vector<std::uint8_t>(saved.begin(), saved.end()), o);
                if (!enc) std::fprintf(stderr, "    encryptPdf: %s\n", core::describe(enc.error()).c_str());
                CHECK(enc.has_value());
                if (!enc) continue;
                const std::string bytes(enc->begin(), enc->end());

                const std::string user = pw.user;
                const std::string owner = pw.owner;
                // The password the user is expected to type: user (may be empty).
                auto withUser = openBytes(*engine, bytes, user);
                if (!withUser) std::fprintf(stderr, "    open with user password failed: %s\n", core::describe(withUser.error()).c_str());
                CHECK(withUser.has_value());
                if (withUser) {
                    // The engine reports "encrypted" from the password it was given, so only check it when one is set.
                    if (!user.empty()) CHECK((*withUser)->info().isEncrypted);
                    expectSame(reference, **withUser, "user");
                }
                if (!owner.empty()) {
                    auto withOwner = openBytes(*engine, bytes, owner);
                    if (!withOwner) std::fprintf(stderr, "    open with owner password failed: %s\n", core::describe(withOwner.error()).c_str());
                    CHECK(withOwner.has_value());
                    if (withOwner) expectSame(reference, **withOwner, "owner");
                }
                // Wrong password (and no password when a user password is set) is refused.
                auto wrong = openBytes(*engine, bytes, "definitely-wrong");
                CHECK(!wrong.has_value());
                if (!wrong) CHECK(wrong.error().code == core::ErrorCode::PasswordRequired);
                if (!user.empty()) {
                    auto none = openBytes(*engine, bytes, {});
                    CHECK(!none.has_value());
                    if (!none) CHECK(none.error().code == core::ErrorCode::PasswordRequired);
                    // The user password is not the owner password and vice versa for the hash check.
                    auto swapped = openBytes(*engine, bytes, user + "x");
                    CHECK(!swapped.has_value());
                }
            }
        }
    }
}

RIVET_TEST(pdfiumOpensUnicodeAes256Password) {
    auto engine = createEngine();
    CHECK(engine != nullptr);
    if (!engine || !engine->isAvailable()) return;
    auto original = openBytes(*engine, rivet::test::pdffix::richPdf(), {});
    CHECK(original.has_value());
    if (!original) return;
    const std::string saved = pdfiumSave(*engine, **original);

    pe::EncryptOptions o;
    o.userPassword = "p\xC3\xA4ssw\xC3\xB6rd-\xD0\xBF\xD0\xB0\xD1\x80\xD0\xBE\xD0\xBB\xD1\x8C";
    auto enc = pe::encryptPdf(std::vector<std::uint8_t>(saved.begin(), saved.end()), o);
    CHECK(enc.has_value());
    if (!enc) return;
    const std::string bytes(enc->begin(), enc->end());
    auto ok = openBytes(*engine, bytes, o.userPassword);
    if (!ok) std::fprintf(stderr, "    unicode password open failed: %s\n", core::describe(ok.error()).c_str());
    CHECK(ok.has_value());
    CHECK(!openBytes(*engine, bytes, "password-parol").has_value());
}

RIVET_TEST(encryptedOutputIsNeverReEncrypted) {
    // An encrypted document opened with its password and saved by PDFium keeps its encryption, so the
    // encryptor must refuse that output (no re-encryption) instead of corrupting it.
    auto engine = createEngine();
    CHECK(engine != nullptr);
    if (!engine || !engine->isAvailable()) return;
    auto original = openBytes(*engine, rivet::test::pdffix::richPdf(), {});
    CHECK(original.has_value());
    if (!original) return;
    const std::string saved = pdfiumSave(*engine, **original);
    pe::EncryptOptions o;
    o.userPassword = "pw";
    auto enc = pe::encryptPdf(std::vector<std::uint8_t>(saved.begin(), saved.end()), o);
    CHECK(enc.has_value());
    if (!enc) return;
    auto again = pe::encryptPdf(*enc, o);
    CHECK(!again.has_value());
    if (!again) CHECK(again.error().code == core::ErrorCode::Unsupported);
}

RIVET_TEST(everyPdfiumSavedFixtureSurvivesEncryption) {
    auto engine = createEngine();
    CHECK(engine != nullptr);
    if (!engine || !engine->isAvailable()) return;
    // RIVET_ENC_DUMP_DIR (optional): keep the PDFium-saved inputs there for external readers (poppler).
    const char* dump = std::getenv("RIVET_ENC_DUMP_DIR");

    std::size_t tested = 0, refused = 0;
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(RIVET_PDF_RENDER_FIXTURE_DIR))
        if (entry.path().extension() == ".pdf") files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    for (const fs::path& file : files) {
        auto source = engine->openDocument(file);
        if (!source) continue; // corrupted.pdf, password.pdf: not inputs of a plain save
        if ((*source)->info().pageCount == 0) continue;
        std::string saved;
        {
            PdfAssemblyRequest request;
            request.mode = PdfAssemblyRequest::Mode::PreserveBase;
            request.base = source->get();
            for (std::size_t i = 0; i < (*source)->info().pageCount; ++i) {
                PdfAssemblyPage page;
                page.source = source->get();
                page.sourcePageIndex = i;
                auto info = (*source)->pageInfo(i);
                if (info) page.view = info->view;
                request.pages.push_back(std::move(page));
            }
            MemorySink sink;
            if (!engine->assembleDocument(request, sink)) continue;
            saved = sink.bytes();
        }
        if (dump) {
            std::ofstream out(fs::path(dump) / (file.stem().string() + ".saved.pdf"), std::ios::binary | std::ios::trunc);
            out.write(saved.data(), static_cast<std::streamsize>(saved.size()));
        }
        auto plain = openBytes(*engine, saved, {});
        CHECK(plain.has_value());
        if (!plain) continue;
        const Snapshot reference = snapshot(**plain);

        for (const pe::Cipher cipher : {pe::Cipher::Aes256, pe::Cipher::Aes128}) {
            pe::EncryptOptions o;
            o.cipher = cipher;
            o.userPassword = "u";
            o.ownerPassword = "o";
            auto enc = pe::encryptPdf(std::vector<std::uint8_t>(saved.begin(), saved.end()), o);
            if (!enc) std::fprintf(stderr, "    %s: %s\n", file.filename().string().c_str(), core::describe(enc.error()).c_str());
            if (!enc && core::describe(enc.error()).find("Unsupported") != std::string::npos) {
                // PDFium preserved the source's object/xref streams: the constrained encryptor refuses by design.
                ++refused;
                break;
            }
            CHECK(enc.has_value());
            if (!enc) continue;
            const std::string bytes(enc->begin(), enc->end());
            auto withUser = openBytes(*engine, bytes, "u");
            CHECK(withUser.has_value());
            if (withUser) expectSame(reference, **withUser, file.filename().string().c_str());
        }
        ++tested;
    }
    std::fprintf(stderr, "    %zu fixtures encrypted and compared, %zu refused (object streams)\n", tested, refused);
    CHECK(tested >= 10);
}
