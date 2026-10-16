// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Error.hpp"
#include "pdf/PdfAnnotation.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rivet::app {

// The user's saved visual signatures (ADR-0025), kept in the per-user
// application-data directory (platform::IAppDataLocation) as ONE small
// versioned binary file, `signatures.bin`:
//
//   "RVSG" | u32 version (1) | u32 count (<= kMaxEntries) | entries | u32 FNV-1a
//   entry: u8 source (0 ink, 1 text, 2 image), then
//     ink:   f64 aspect, f32 inkWidth, u32 strokes, per stroke u32 points + f32 x, f32 y each
//     text:  u32 bytes + UTF-8
//     image: u32 bytes + the encoded PNG/JPEG as chosen (decoded on use)
//   (all integers little-endian; the checksum covers everything before it)
//
// Every count and length is capped (kMaxEntries, pdf::kMaxSignature*,
// kMaxImageBytes) BEFORE anything is allocated; a file that is truncated,
// over a cap, of another version or with a bad checksum is CORRUPT and
// ignored as a whole (the next save replaces it). Writes are atomic
// (core::AtomicFileWriter) and owner-only where the platform allows.
// Signature data is personal: it is never logged, and errors never contain
// it.
//
// Main thread only (plain synchronous file I/O of a small file).
class SignatureStore {
public:
    static constexpr std::size_t kMaxEntries = 8;
    static constexpr std::size_t kMaxImageBytes = 2u * 1024u * 1024u;
    static constexpr std::uint32_t kVersion = 1;
    static constexpr const char* kFileName = "signatures.bin";

    struct Entry {
        pdf::PdfSignatureSource source = pdf::PdfSignatureSource::Ink;
        // Ink (see pdf::PdfSignatureAppearance).
        std::vector<std::vector<pdf::PdfPoint>> strokes;
        double aspect = 1.0;
        float inkWidth = 0.04F;
        // Text.
        std::string text;
        // Image: the encoded file bytes.
        std::vector<std::uint8_t> image;

        bool operator==(const Entry&) const = default;
    };

    enum class LoadResult : std::uint8_t {
        Loaded,     // entries replaced by the file's
        Missing,    // no file yet: no entries
        Corrupt,    // unreadable content: ignored, no entries
        Unreadable, // I/O error: no entries
    };

    // `file` = the store file; an empty path keeps the store in memory only
    // (no app-data location on this platform).
    explicit SignatureStore(std::filesystem::path file);

    const std::filesystem::path& file() const { return file_; }
    bool persistent() const { return !file_.empty(); }

    LoadResult load();
    // Atomically replaces the file with the current entries (creates the
    // directory). No-op for an in-memory store.
    core::Status save() const;

    const std::vector<Entry>& entries() const { return entries_; }
    // Inserts `entry` first (most recent); the oldest one beyond kMaxEntries
    // is dropped. An identical existing entry moves to the front instead of
    // being duplicated. False (nothing changed) for an invalid entry.
    bool add(Entry entry);
    bool remove(std::size_t index);

    // Within the caps and drawable (ink/text through pdf::isValidSignature;
    // image: non-empty and <= kMaxImageBytes).
    static bool isValidEntry(const Entry& entry);
    // Ink/Text entries as an appearance (nullopt for Image entries, which
    // need the platform decoder, and for invalid entries).
    static std::optional<pdf::PdfSignatureAppearance> appearance(const Entry& entry);
    // An Ink/Text entry from an appearance (nullopt for Image).
    static std::optional<Entry> fromAppearance(const pdf::PdfSignatureAppearance& appearance);

    static std::vector<std::uint8_t> serialize(const std::vector<Entry>& entries);
    static std::optional<std::vector<Entry>> parse(std::span<const std::uint8_t> bytes);

private:
    std::filesystem::path file_;
    std::vector<Entry> entries_;
};

} // namespace rivet::app
