// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Bitmap.hpp"
#include "core/async/AsyncScope.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "markdown/MarkdownLayout.hpp"
#include "platform/ImageDecoder.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace rivet::app {

// Why an image is not shown (drives the placeholder text).
enum class ImageBlockReason : std::uint8_t {
    None,
    Remote,     // a URL with a scheme (http, https, data, file, ...): never fetched
    TooLarge,   // file, side or decoded size over the limits, or the cache budget is spent
    Unreadable, // missing, not a regular file, not PNG/JPEG, decode failure
};

// Loads the local images of a Markdown document and serves their sizes to the
// layout (IImageSizeProvider) and their pixels to the painter.
//
// SECURITY: the document is untrusted. No image is ever fetched from the
// network: every URL with a scheme (and protocol-relative "//host") is a
// Remote block, never touched. Local files are only read as bounded,
// regular files and decoded by the platform decoder under dimension and
// decoded-byte limits (also checked from the header before pixel allocation);
// nothing about the content is logged. Failures degrade to a placeholder.
//
// Threading: imageInfo()/bitmap() and every callback run on the main thread.
// Reading and decoding happen on the TaskScheduler inside an AsyncScope token;
// results are posted back through the IMainThreadDispatcher and discarded when
// the store is gone (weak reference) or the entry was cleared. The destructor
// closes the scope, so no worker touches the decoder afterwards.
class MarkdownImageStore final : public markdown::IImageSizeProvider {
public:
    struct Limits {
        std::uint32_t maxSide = 8192;
        std::uint64_t maxDecodedBytes = 64ull * 1024 * 1024; // per image
        std::uint64_t maxFileBytes = 32ull * 1024 * 1024;
        std::uint64_t maxTotalDecodedBytes = 256ull * 1024 * 1024; // whole cache
    };

    struct Environment {
        core::IMainThreadDispatcher* dispatcher = nullptr;
        core::TaskScheduler* scheduler = nullptr;
        const platform::IImageDecoder* decoder = nullptr; // null: every local image is Unreadable
    };

    static std::shared_ptr<MarkdownImageStore> create(Environment environment) { return create(environment, Limits{}); }
    static std::shared_ptr<MarkdownImageStore> create(Environment environment, Limits limits);
    ~MarkdownImageStore();

    MarkdownImageStore(const MarkdownImageStore&) = delete;
    MarkdownImageStore& operator=(const MarkdownImageStore&) = delete;

    // Directory relative image paths resolve against (the .md file's folder).
    // Changing it keeps already loaded entries (they are keyed by full path).
    void setBaseDirectory(std::filesystem::path directory);

    // Fired (main thread) after an image finished loading or failed; the owner
    // relayouts. Coalescing is the owner's business.
    void setOnChanged(std::function<void()> onChanged) { onChanged_ = std::move(onChanged); }

    // Drops the cache and ignores results of loads still in flight.
    void clear();

    // IImageSizeProvider: Known (loaded), Blocked (remote/failed) or Unknown
    // (loading; the first query starts the load).
    markdown::ImageInfo imageInfo(std::string_view url) const override;
    // The pixels of a loaded image (null otherwise).
    std::shared_ptr<const core::Bitmap> bitmap(std::string_view url) const;
    ImageBlockReason blockReason(std::string_view url) const;

    // Resolves a document URL to the file it would load, or empty when it is
    // not a local path (remote scheme, empty, NUL byte). Exposed for tests.
    std::filesystem::path resolve(std::string_view url) const;

    std::size_t cachedBytes() const { return totalBytes_; }
    std::size_t entryCount() const { return entries_.size(); }

private:
    struct Entry {
        enum class State : std::uint8_t { Loading, Ready, Blocked } state = State::Loading;
        ImageBlockReason reason = ImageBlockReason::None;
        std::shared_ptr<const core::Bitmap> bitmap;
        std::uint64_t generation = 0;
    };
    struct LoadResult {
        std::shared_ptr<const core::Bitmap> bitmap;
        ImageBlockReason reason = ImageBlockReason::None;
    };

    MarkdownImageStore(Environment environment, Limits limits);
    Entry& entryFor(const std::string& key) const;
    void startLoad(const std::string& key) const;
    void complete(const std::string& key, std::uint64_t generation, LoadResult result);
    static LoadResult loadFile(const std::filesystem::path& path, const platform::IImageDecoder& decoder,
                               const Limits& limits);

    Environment env_;
    Limits limits_;
    std::filesystem::path baseDirectory_;
    std::function<void()> onChanged_;
    mutable std::unordered_map<std::string, Entry> entries_;
    mutable std::uint64_t totalBytes_ = 0;
    std::uint64_t generation_ = 1;
    mutable core::AsyncScope scope_;
    std::weak_ptr<MarkdownImageStore> self_;
};

} // namespace rivet::app
