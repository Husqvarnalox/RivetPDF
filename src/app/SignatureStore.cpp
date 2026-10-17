// SPDX-License-Identifier: MPL-2.0
#include "app/SignatureStore.hpp"

#include "core/io/AtomicFileWriter.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <system_error>
#include <utility>

namespace rivet::app {

namespace {

constexpr char kMagic[4] = {'R', 'V', 'S', 'G'};
// The largest well-formed file: 8 image entries at the cap plus framing and
// (much smaller) ink entries; anything bigger is not read at all.
constexpr std::size_t kMaxFileBytes = SignatureStore::kMaxEntries * (SignatureStore::kMaxImageBytes + 128u * 1024u);

std::uint32_t fnv1a(std::span<const std::uint8_t> bytes) {
    std::uint32_t hash = 2166136261u;
    for (const std::uint8_t b : bytes) {
        hash ^= b;
        hash *= 16777619u;
    }
    return hash;
}

class Writer {
public:
    void u8(std::uint8_t v) { out_.push_back(v); }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
    }
    void u64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) out_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
    }
    void f32(float v) { u32(std::bit_cast<std::uint32_t>(v)); }
    void f64(double v) { u64(std::bit_cast<std::uint64_t>(v)); }
    void bytes(std::span<const std::uint8_t> data) { out_.insert(out_.end(), data.begin(), data.end()); }
    std::vector<std::uint8_t>& out() { return out_; }

private:
    std::vector<std::uint8_t> out_;
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data) : data_(data) {}
    bool ok() const { return ok_; }
    std::size_t remaining() const { return data_.size() - at_; }

    std::uint8_t u8() {
        if (!need(1)) return 0;
        return data_[at_++];
    }
    std::uint32_t u32() {
        if (!need(4)) return 0;
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(data_[at_ + static_cast<std::size_t>(i)]) << (8 * i);
        at_ += 4;
        return v;
    }
    std::uint64_t u64() {
        if (!need(8)) return 0;
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(data_[at_ + static_cast<std::size_t>(i)]) << (8 * i);
        at_ += 8;
        return v;
    }
    float f32() { return std::bit_cast<float>(u32()); }
    double f64() { return std::bit_cast<double>(u64()); }
    std::span<const std::uint8_t> bytes(std::size_t n) {
        if (!need(n)) return {};
        const auto out = data_.subspan(at_, n);
        at_ += n;
        return out;
    }
    void fail() { ok_ = false; }

private:
    bool need(std::size_t n) {
        if (!ok_ || n > remaining()) {
            ok_ = false;
            return false;
        }
        return true;
    }

    std::span<const std::uint8_t> data_;
    std::size_t at_ = 0;
    bool ok_ = true;
};

std::optional<SignatureStore::Entry> readEntry(Reader& r) {
    SignatureStore::Entry entry;
    const std::uint8_t source = r.u8();
    switch (source) {
    case 0: {
        entry.source = pdf::PdfSignatureSource::Ink;
        entry.aspect = r.f64();
        entry.inkWidth = r.f32();
        const std::uint32_t strokes = r.u32();
        if (!r.ok() || strokes == 0 || strokes > pdf::kMaxSignatureStrokes) return std::nullopt;
        std::size_t total = 0;
        for (std::uint32_t s = 0; s < strokes; ++s) {
            const std::uint32_t points = r.u32();
            total += points;
            if (!r.ok() || points == 0 || total > pdf::kMaxSignaturePoints || r.remaining() / 8 < points) {
                return std::nullopt;
            }
            std::vector<pdf::PdfPoint> stroke;
            stroke.reserve(points);
            for (std::uint32_t p = 0; p < points; ++p) {
                const float x = r.f32();
                const float y = r.f32();
                stroke.push_back(pdf::PdfPoint{static_cast<double>(x), static_cast<double>(y)});
            }
            entry.strokes.push_back(std::move(stroke));
        }
        break;
    }
    case 1: {
        entry.source = pdf::PdfSignatureSource::Text;
        const std::uint32_t length = r.u32();
        if (!r.ok() || length == 0 || length > pdf::kMaxSignatureTextBytes) return std::nullopt;
        const auto text = r.bytes(length);
        entry.text.assign(text.begin(), text.end());
        break;
    }
    case 2: {
        entry.source = pdf::PdfSignatureSource::Image;
        const std::uint32_t length = r.u32();
        if (!r.ok() || length == 0 || length > SignatureStore::kMaxImageBytes) return std::nullopt;
        const auto image = r.bytes(length);
        entry.image.assign(image.begin(), image.end());
        break;
    }
    default:
        return std::nullopt;
    }
    if (!r.ok() || !SignatureStore::isValidEntry(entry)) return std::nullopt;
    return entry;
}

} // namespace

SignatureStore::SignatureStore(std::filesystem::path file) : file_(std::move(file)) {}

bool SignatureStore::isValidEntry(const Entry& entry) {
    switch (entry.source) {
    case pdf::PdfSignatureSource::Ink:
    case pdf::PdfSignatureSource::Text: {
        const auto ap = appearance(entry);
        return ap.has_value();
    }
    case pdf::PdfSignatureSource::Image:
        return !entry.image.empty() && entry.image.size() <= kMaxImageBytes;
    }
    return false;
}

std::optional<pdf::PdfSignatureAppearance> SignatureStore::appearance(const Entry& entry) {
    pdf::PdfSignatureAppearance out;
    out.source = entry.source;
    switch (entry.source) {
    case pdf::PdfSignatureSource::Ink:
        out.strokes = entry.strokes;
        out.aspect = entry.aspect;
        out.inkWidth = entry.inkWidth;
        break;
    case pdf::PdfSignatureSource::Text:
        out.text = entry.text;
        break;
    case pdf::PdfSignatureSource::Image:
        return std::nullopt;
    }
    if (!pdf::isValidSignature(out)) return std::nullopt;
    return out;
}

std::optional<SignatureStore::Entry> SignatureStore::fromAppearance(const pdf::PdfSignatureAppearance& appearance) {
    if (!pdf::isValidSignature(appearance)) return std::nullopt;
    Entry entry;
    entry.source = appearance.source;
    switch (appearance.source) {
    case pdf::PdfSignatureSource::Ink:
        entry.aspect = appearance.aspect;
        entry.inkWidth = appearance.inkWidth;
        // Stored as f32: round now so a saved entry equals its reloaded copy.
        for (const auto& stroke : appearance.strokes) {
            std::vector<pdf::PdfPoint> rounded;
            rounded.reserve(stroke.size());
            for (const pdf::PdfPoint& p : stroke) {
                rounded.push_back(pdf::PdfPoint{static_cast<double>(static_cast<float>(p.x)),
                                                static_cast<double>(static_cast<float>(p.y))});
            }
            entry.strokes.push_back(std::move(rounded));
        }
        break;
    case pdf::PdfSignatureSource::Text:
        entry.text = appearance.text;
        break;
    case pdf::PdfSignatureSource::Image:
        return std::nullopt;
    }
    return entry;
}

std::vector<std::uint8_t> SignatureStore::serialize(const std::vector<Entry>& entries) {
    Writer w;
    for (const char c : kMagic) w.u8(static_cast<std::uint8_t>(c));
    w.u32(kVersion);
    const std::size_t count = std::min(entries.size(), kMaxEntries);
    w.u32(static_cast<std::uint32_t>(count));
    for (std::size_t i = 0; i < count; ++i) {
        const Entry& e = entries[i];
        switch (e.source) {
        case pdf::PdfSignatureSource::Ink:
            w.u8(0);
            w.f64(e.aspect);
            w.f32(e.inkWidth);
            w.u32(static_cast<std::uint32_t>(e.strokes.size()));
            for (const auto& stroke : e.strokes) {
                w.u32(static_cast<std::uint32_t>(stroke.size()));
                for (const pdf::PdfPoint& p : stroke) {
                    w.f32(static_cast<float>(p.x));
                    w.f32(static_cast<float>(p.y));
                }
            }
            break;
        case pdf::PdfSignatureSource::Text:
            w.u8(1);
            w.u32(static_cast<std::uint32_t>(e.text.size()));
            w.bytes(std::span(reinterpret_cast<const std::uint8_t*>(e.text.data()), e.text.size()));
            break;
        case pdf::PdfSignatureSource::Image:
            w.u8(2);
            w.u32(static_cast<std::uint32_t>(e.image.size()));
            w.bytes(e.image);
            break;
        }
    }
    const std::uint32_t checksum = fnv1a(w.out());
    w.u32(checksum);
    return std::move(w.out());
}

std::optional<std::vector<SignatureStore::Entry>> SignatureStore::parse(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 16 || bytes.size() > kMaxFileBytes) return std::nullopt;
    const std::span<const std::uint8_t> body = bytes.first(bytes.size() - 4);
    Reader tail(bytes.subspan(bytes.size() - 4));
    if (fnv1a(body) != tail.u32()) return std::nullopt;
    Reader r(body);
    for (const char c : kMagic) {
        if (r.u8() != static_cast<std::uint8_t>(c)) return std::nullopt;
    }
    if (r.u32() != kVersion) return std::nullopt;
    const std::uint32_t count = r.u32();
    if (!r.ok() || count > kMaxEntries) return std::nullopt;
    std::vector<Entry> entries;
    entries.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        auto entry = readEntry(r);
        if (!entry) return std::nullopt;
        entries.push_back(std::move(*entry));
    }
    if (!r.ok() || r.remaining() != 0) return std::nullopt;
    return entries;
}

SignatureStore::LoadResult SignatureStore::load() {
    entries_.clear();
    if (file_.empty()) return LoadResult::Missing;
    std::error_code ec;
    const bool exists = std::filesystem::exists(file_, ec);
    if (ec) return LoadResult::Unreadable;
    if (!exists) return LoadResult::Missing;
    const auto size = std::filesystem::file_size(file_, ec);
    if (ec) return LoadResult::Unreadable;
    if (size > kMaxFileBytes) return LoadResult::Corrupt;
    std::ifstream in(file_, std::ios::binary);
    if (!in) return LoadResult::Unreadable;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (size > 0 && !in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
        return LoadResult::Unreadable;
    }
    auto parsed = parse(bytes);
    if (!parsed) return LoadResult::Corrupt;
    entries_ = std::move(*parsed);
    return LoadResult::Loaded;
}

core::Status SignatureStore::save() const {
    if (file_.empty()) return core::ok();
    std::error_code ec;
    const std::filesystem::path directory = file_.parent_path();
    if (!directory.empty() && !std::filesystem::exists(directory, ec)) {
        std::filesystem::create_directories(directory, ec);
        if (ec) {
            return std::unexpected(core::makeError(core::ErrorCode::Io,
                                                   "could not create the application-data folder", "app"));
        }
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, ec); // best effort
    }
    const std::vector<std::uint8_t> bytes = serialize(entries_);
    auto writer = core::io::AtomicFileWriter::begin(file_);
    if (!writer.has_value()) return std::unexpected(writer.error());
    if (auto written = (*writer)->write(bytes.data(), bytes.size()); !written.has_value()) return written;
    if (auto committed = (*writer)->commit(); !committed.has_value()) return committed;
    // Personal data: owner-only where the platform has POSIX permissions.
    std::filesystem::permissions(file_, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec);
    return core::ok();
}

bool SignatureStore::add(Entry entry) {
    if (!isValidEntry(entry)) return false;
    // The file stores points as f32: keep the in-memory copy identical to a
    // reloaded one.
    for (auto& stroke : entry.strokes) {
        for (pdf::PdfPoint& p : stroke) {
            p.x = static_cast<double>(static_cast<float>(p.x));
            p.y = static_cast<double>(static_cast<float>(p.y));
        }
    }
    const auto same = std::find(entries_.begin(), entries_.end(), entry);
    if (same != entries_.end()) entries_.erase(same);
    entries_.insert(entries_.begin(), std::move(entry));
    if (entries_.size() > kMaxEntries) entries_.resize(kMaxEntries);
    return true;
}

bool SignatureStore::remove(std::size_t index) {
    if (index >= entries_.size()) return false;
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

} // namespace rivet::app
