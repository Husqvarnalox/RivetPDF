// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "app/SignatureStore.hpp"

#include "TestProcess.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

// The saved-signature store (ADR-0025): round trip through the file, caps,
// corruption handling and the entry <-> appearance conversion.

using namespace rivet;
using app::SignatureStore;
namespace fs = std::filesystem;

namespace {

class TempDir {
public:
    TempDir() {
        static int counter = 0;
        path_ = fs::temp_directory_path() /
                ("rivet-sigstore-" + std::to_string(rivet::test::processId()) + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(counter++));
    }
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

SignatureStore::Entry inkEntry(double shift = 0.0) {
    SignatureStore::Entry e;
    e.source = pdf::PdfSignatureSource::Ink;
    e.aspect = 2.5;
    e.inkWidth = 0.05F;
    e.strokes = {{{0.125 + shift, 0.25}, {0.5, 0.75}, {2.25, 0.5}}, {{1.0, 0.5}}};
    return e;
}

SignatureStore::Entry textEntry(std::string text) {
    SignatureStore::Entry e;
    e.source = pdf::PdfSignatureSource::Text;
    e.text = std::move(text);
    return e;
}

SignatureStore::Entry imageEntry(std::size_t bytes) {
    SignatureStore::Entry e;
    e.source = pdf::PdfSignatureSource::Image;
    e.image.assign(bytes, 0x5A);
    return e;
}

std::vector<std::uint8_t> readAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeAll(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

} // namespace

RIVET_TEST(signatureStoreRoundTripThroughTheFile) {
    TempDir dir;
    const fs::path file = dir.path() / "nested" / SignatureStore::kFileName;
    SignatureStore store(file);
    CHECK(store.load() == SignatureStore::LoadResult::Missing);
    CHECK(store.entries().empty());
    CHECK(store.add(inkEntry()));
    CHECK(store.add(textEntry("Jane Q. Doe \xC3\xA9")));
    CHECK(store.add(imageEntry(1000)));
    CHECK(store.save().has_value()); // creates the directory
    CHECK(fs::exists(file));
#ifndef _WIN32
    const auto perms = fs::status(file).permissions();
    CHECK((perms & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
#endif

    SignatureStore reloaded(file);
    CHECK(reloaded.load() == SignatureStore::LoadResult::Loaded);
    CHECK_EQ(reloaded.entries().size(), std::size_t{3});
    CHECK(reloaded.entries() == store.entries());
    // Most recent first.
    CHECK(reloaded.entries()[0].source == pdf::PdfSignatureSource::Image);
    CHECK(reloaded.entries()[2].source == pdf::PdfSignatureSource::Ink);

    // Entries convert to appearances (images need the platform decoder).
    const auto ink = SignatureStore::appearance(reloaded.entries()[2]);
    CHECK(ink.has_value());
    if (ink) CHECK(ink->strokes.size() == 2);
    CHECK(!SignatureStore::appearance(reloaded.entries()[0]).has_value());
    const auto back = SignatureStore::fromAppearance(*ink);
    CHECK(back.has_value());
    if (back) CHECK(*back == reloaded.entries()[2]);
}

RIVET_TEST(signatureStoreLimits) {
    SignatureStore store({}); // in memory
    CHECK(!store.persistent());
    for (int i = 0; i < 10; ++i) CHECK(store.add(inkEntry(static_cast<double>(i) / 64.0)));
    CHECK_EQ(store.entries().size(), SignatureStore::kMaxEntries);
    CHECK(store.entries()[0] == inkEntry(9.0 / 64.0)); // newest first, oldest dropped
    // Re-adding an existing entry moves it to the front without duplicating.
    CHECK(store.add(inkEntry(5.0 / 64.0)));
    CHECK_EQ(store.entries().size(), SignatureStore::kMaxEntries);
    CHECK(store.entries()[0] == inkEntry(5.0 / 64.0));
    CHECK(store.save().has_value()); // no-op in memory

    CHECK(!store.add(imageEntry(SignatureStore::kMaxImageBytes + 1)));
    CHECK(store.add(imageEntry(SignatureStore::kMaxImageBytes)));
    CHECK(!store.add(imageEntry(0)));
    CHECK(!store.add(textEntry("")));
    CHECK(!store.add(textEntry(std::string(pdf::kMaxSignatureTextBytes + 1, 'x'))));
    SignatureStore::Entry tooMany = inkEntry();
    tooMany.strokes.assign(pdf::kMaxSignatureStrokes + 1, std::vector<pdf::PdfPoint>{{0.5, 0.5}});
    CHECK(!store.add(tooMany));
    SignatureStore::Entry nan = inkEntry();
    nan.strokes[0][0].x = std::nan("");
    CHECK(!store.add(nan));

    CHECK(store.remove(0));
    CHECK(!store.remove(100));
}

RIVET_TEST(signatureStoreIgnoresCorruptFiles) {
    TempDir dir;
    fs::create_directories(dir.path());
    const fs::path file = dir.path() / SignatureStore::kFileName;
    SignatureStore store(file);
    CHECK(store.add(inkEntry()));
    CHECK(store.add(textEntry("Ok")));
    CHECK(store.save().has_value());
    const std::vector<std::uint8_t> good = readAll(file);
    CHECK(SignatureStore::parse(good).has_value());

    const auto expectCorrupt = [&](const std::vector<std::uint8_t>& bytes) {
        writeAll(file, bytes);
        SignatureStore reloaded(file);
        CHECK(reloaded.load() == SignatureStore::LoadResult::Corrupt);
        CHECK(reloaded.entries().empty());
    };
    // Every truncation.
    for (std::size_t n = 0; n < good.size(); n += 7) {
        expectCorrupt(std::vector<std::uint8_t>(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n)));
    }
    // A flipped byte anywhere (the checksum catches it).
    for (std::size_t i = 0; i < good.size(); i += 5) {
        std::vector<std::uint8_t> bad = good;
        bad[i] ^= 0x40;
        expectCorrupt(bad);
    }
    // Another version, an absurd count, trailing bytes - even with a fixed-up checksum.
    const auto withChecksum = [](std::vector<std::uint8_t> body) {
        std::uint32_t hash = 2166136261u;
        for (const std::uint8_t b : body) {
            hash ^= b;
            hash *= 16777619u;
        }
        for (int i = 0; i < 4; ++i) body.push_back(static_cast<std::uint8_t>((hash >> (8 * i)) & 0xFFu));
        return body;
    };
    std::vector<std::uint8_t> body(good.begin(), good.end() - 4);
    CHECK(body.size() > 35);
    std::vector<std::uint8_t> version = body;
    version[4] = 2;
    expectCorrupt(withChecksum(version));
    std::vector<std::uint8_t> count = body;
    count[8] = 200;
    expectCorrupt(withChecksum(count));
    std::vector<std::uint8_t> trailing = body;
    trailing.push_back(0);
    expectCorrupt(withChecksum(trailing));
    std::vector<std::uint8_t> hugeStroke = body;
    // Entry 0 is the text "Ok" (12..18); entry 1 (ink): source at 19, aspect
    // f64 at 20, inkWidth f32 at 28, stroke count u32 at 32.
    hugeStroke[34] = 0xFF;
    hugeStroke[35] = 0xFF;
    expectCorrupt(withChecksum(hugeStroke));

    // The next save replaces the corrupt file.
    SignatureStore fresh(file);
    CHECK(fresh.load() == SignatureStore::LoadResult::Corrupt);
    CHECK(fresh.add(textEntry("Again")));
    CHECK(fresh.save().has_value());
    SignatureStore again(file);
    CHECK(again.load() == SignatureStore::LoadResult::Loaded);
    CHECK_EQ(again.entries().size(), std::size_t{1});
}
