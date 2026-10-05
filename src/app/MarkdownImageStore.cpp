// SPDX-License-Identifier: MPL-2.0
#include "app/MarkdownImageStore.hpp"

#include "app/MarkdownLinks.hpp"

#include <cstring>
#include <optional>
#include <fstream>
#include <system_error>
#include <vector>

namespace rivet::app {

namespace {

constexpr std::size_t kMaxInFlight = 4096; // queued/in-flight loads per store (a bound on a hostile document)

} // namespace

std::shared_ptr<MarkdownImageStore> MarkdownImageStore::create(Environment environment, Limits limits) {
    std::shared_ptr<MarkdownImageStore> store(new MarkdownImageStore(environment, limits));
    store->self_ = store;
    return store;
}

MarkdownImageStore::MarkdownImageStore(Environment environment, Limits limits)
    : env_(environment), limits_(limits) {}

MarkdownImageStore::~MarkdownImageStore() { scope_.closeAndWait(); }

void MarkdownImageStore::setBaseDirectory(std::filesystem::path directory) {
    baseDirectory_ = std::move(directory);
}

void MarkdownImageStore::clear() {
    entries_.clear();
    totalBytes_ = 0;
    ++generation_;
}

namespace {

// The URL minus fragment/query, percent-decoded. Empty for an unusable URL.
std::string decodedLocalText(std::string_view url) {
    if (url.empty() || url.find('\0') != std::string_view::npos) return {};
    std::string text = percentDecode(url);
    if (const std::size_t cut = text.find_first_of("?#"); cut != std::string::npos) text.erase(cut);
    if (text.find('\0') != std::string::npos) return {};
    return text;
}

// A path that would reach a network location: a doubled leading separator (UNC), a UNC / device root
// name, or a network mount point. Evaluated on decoded text, never on the raw URL.
bool isRemotePath(const std::string& text, const std::filesystem::path& path) {
    if (text.rfind("\\\\", 0) == 0 || text.rfind("//", 0) == 0) return true;
    const std::string root = path.root_name().string();
    if (!root.empty()) {
        const bool driveLetter = root.size() == 2 && root[1] == ':' &&
                                 ((root[0] >= 'A' && root[0] <= 'Z') || (root[0] >= 'a' && root[0] <= 'z'));
        if (!driveLetter) return true;
    }
    const std::string normal = path.lexically_normal().generic_string();
    return normal.rfind("/net/", 0) == 0 || normal.rfind("/Network/", 0) == 0;
}

} // namespace

std::filesystem::path MarkdownImageStore::resolve(std::string_view url) const {
    if (hasUriScheme(url)) return {};
    const std::string text = decodedLocalText(url);
    if (text.empty()) return {};
    std::filesystem::path path{text};
    if (isRemotePath(text, path)) return {};
    if (path.is_relative()) {
        if (baseDirectory_.empty()) return {};
        path = baseDirectory_ / path;
        if (isRemotePath(path.generic_string(), path)) return {};
    }
    return path.lexically_normal();
}

MarkdownImageStore::Entry& MarkdownImageStore::entryFor(const std::string& key) const {
    return entries_[key];
}

markdown::ImageInfo MarkdownImageStore::imageInfo(std::string_view url) const {
    markdown::ImageInfo info;
    const std::filesystem::path path = resolve(url);
    if (path.empty()) {
        info.state = markdown::ImageState::Blocked;
        return info;
    }
    const std::string key = path.string();
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        if (entries_.size() >= kMaxInFlight) {
            info.state = markdown::ImageState::Blocked;
            return info;
        }
        startLoad(key);
        return info; // Unknown
    }
    const Entry& entry = it->second;
    switch (entry.state) {
    case Entry::State::Loading: break;
    case Entry::State::Blocked: info.state = markdown::ImageState::Blocked; break;
    case Entry::State::Ready:
        info.state = markdown::ImageState::Known;
        info.width = static_cast<double>(entry.bitmap->width());
        info.height = static_cast<double>(entry.bitmap->height());
        break;
    }
    return info;
}

std::shared_ptr<const core::Bitmap> MarkdownImageStore::bitmap(std::string_view url) const {
    const std::filesystem::path path = resolve(url);
    if (path.empty()) return nullptr;
    const auto it = entries_.find(path.string());
    return it != entries_.end() && it->second.state == Entry::State::Ready ? it->second.bitmap : nullptr;
}

ImageBlockReason MarkdownImageStore::blockReason(std::string_view url) const {
    const std::filesystem::path path = resolve(url);
    if (path.empty()) {
        const std::string text = decodedLocalText(url);
        return (hasUriScheme(url) || (!text.empty() && isRemotePath(text, std::filesystem::path{text})))
                   ? ImageBlockReason::Remote
                   : ImageBlockReason::Unreadable;
    }
    const auto it = entries_.find(path.string());
    if (it == entries_.end() || it->second.state != Entry::State::Blocked) return ImageBlockReason::None;
    return it->second.reason;
}

void MarkdownImageStore::startLoad(const std::string& key) const {
    Entry& entry = entryFor(key);
    entry.generation = generation_;
    const auto block = [&](ImageBlockReason reason) {
        entry.state = Entry::State::Blocked;
        entry.reason = reason;
    };
    if (env_.decoder == nullptr || env_.scheduler == nullptr || env_.dispatcher == nullptr) {
        block(ImageBlockReason::Unreadable);
        return;
    }
    std::optional<core::AsyncScope::Token> token = scope_.enter();
    if (!token) {
        block(ImageBlockReason::Unreadable);
        return;
    }
    const std::weak_ptr<MarkdownImageStore> weak = self_;
    const platform::IImageDecoder* decoder = env_.decoder;
    core::IMainThreadDispatcher* dispatcher = env_.dispatcher;
    const Limits limits = limits_;
    const std::uint64_t generation = generation_;
    const auto shared = std::make_shared<core::AsyncScope::Token>(std::move(*token));
    env_.scheduler->post([token = shared, weak, decoder, dispatcher, limits, generation, key] {
        LoadResult result;
        if (token->cancelled()) {
            result.reason = ImageBlockReason::Unreadable;
        } else {
            result = loadFile(std::filesystem::path{key}, *decoder, limits);
        }
        dispatcher->post([weak, key, generation, result = std::move(result)]() mutable {
            if (const std::shared_ptr<MarkdownImageStore> self = weak.lock()) {
                self->complete(key, generation, std::move(result));
            }
        });
    });
}

void MarkdownImageStore::complete(const std::string& key, std::uint64_t generation, LoadResult result) {
    const auto it = entries_.find(key);
    if (it == entries_.end() || it->second.generation != generation ||
        it->second.state != Entry::State::Loading) {
        return; // cleared or superseded
    }
    Entry& entry = it->second;
    if (result.bitmap != nullptr) {
        const std::uint64_t bytes = result.bitmap->sizeBytes();
        if (totalBytes_ + bytes > limits_.maxTotalDecodedBytes) {
            entry.state = Entry::State::Blocked;
            entry.reason = ImageBlockReason::TooLarge;
        } else {
            totalBytes_ += bytes;
            entry.state = Entry::State::Ready;
            entry.bitmap = std::move(result.bitmap);
        }
    } else {
        entry.state = Entry::State::Blocked;
        entry.reason = result.reason == ImageBlockReason::None ? ImageBlockReason::Unreadable : result.reason;
    }
    if (onChanged_) onChanged_();
}

// Worker: bounded read of a regular file, then the platform decode.
MarkdownImageStore::LoadResult MarkdownImageStore::loadFile(const std::filesystem::path& path,
                                                            const platform::IImageDecoder& decoder,
                                                            const Limits& limits) {
    LoadResult result;
    result.reason = ImageBlockReason::Unreadable;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) return result;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) return result;
    if (size == 0) return result;
    if (size > limits.maxFileBytes) {
        result.reason = ImageBlockReason::TooLarge;
        return result;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return result;
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (static_cast<std::uintmax_t>(in.gcount()) != size) return result;
    }
    platform::IImageDecoder::BgraLimits bgra;
    bgra.maxSide = limits.maxSide;
    bgra.maxDecodedBytes = limits.maxDecodedBytes;
    auto decoded = decoder.decodeBgra(bytes, bgra);
    if (!decoded) {
        result.reason = decoded.error().code == core::ErrorCode::InvalidArgument ? ImageBlockReason::TooLarge
                                                                                 : ImageBlockReason::Unreadable;
        return result;
    }
    const pdf::PdfImageData& img = *decoded;
    if (img.format != pdf::PdfImageData::Format::Bgra || img.width == 0 || img.height == 0 ||
        img.width > limits.maxSide || img.height > limits.maxSide ||
        static_cast<std::uint64_t>(img.width) * img.height * 4u > limits.maxDecodedBytes ||
        img.stride < static_cast<std::uint64_t>(img.width) * 4u ||
        img.bytes.size() < static_cast<std::uint64_t>(img.stride) * img.height) {
        result.reason = ImageBlockReason::TooLarge;
        return result;
    }
    auto bitmap = core::Bitmap::create(img.width, img.height);
    if (!bitmap) return result;
    for (std::uint32_t row = 0; row < img.height; ++row) {
        std::memcpy(bitmap->data() + static_cast<std::size_t>(row) * bitmap->stride(),
                    img.bytes.data() + static_cast<std::size_t>(row) * img.stride,
                    static_cast<std::size_t>(img.width) * 4u);
    }
    result.bitmap = std::make_shared<const core::Bitmap>(std::move(*bitmap));
    result.reason = ImageBlockReason::None;
    return result;
}

} // namespace rivet::app
