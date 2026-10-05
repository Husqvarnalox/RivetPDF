// SPDX-License-Identifier: MPL-2.0
#include "app/MarkdownTabState.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <utility>

namespace rivet::app {

namespace {

std::string lowerExtension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    if (!ext.empty() && ext.front() == '.') ext.erase(ext.begin());
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return ext;
}

constexpr const char* kSubsystem = "app.markdown";

} // namespace

bool hasMarkdownExtension(const std::filesystem::path& path) {
    const std::string ext = lowerExtension(path);
    return std::ranges::any_of(kMarkdownExtensions, [&](std::string_view known) { return ext == known; });
}

DocumentKind documentKindForPath(const std::filesystem::path& path) {
    return hasMarkdownExtension(path) ? DocumentKind::Markdown : DocumentKind::Pdf;
}

bool isValidUtf8(std::string_view bytes) {
    const auto* p = reinterpret_cast<const unsigned char*>(bytes.data());
    const std::size_t n = bytes.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        if (c < 0x80) {
            ++i;
            continue;
        }
        std::size_t extra = 0;
        std::uint32_t minimum = 0;
        std::uint32_t code = 0;
        if (c >= 0xC2 && c <= 0xDF) {
            extra = 1;
            minimum = 0x80;
            code = c & 0x1Fu;
        } else if (c >= 0xE0 && c <= 0xEF) {
            extra = 2;
            minimum = 0x800;
            code = c & 0x0Fu;
        } else if (c >= 0xF0 && c <= 0xF4) {
            extra = 3;
            minimum = 0x10000;
            code = c & 0x07u;
        } else {
            return false; // continuation byte, overlong C0/C1, or F5..FF
        }
        if (i + extra >= n) return false;
        for (std::size_t k = 1; k <= extra; ++k) {
            const unsigned char cont = p[i + k];
            if ((cont & 0xC0u) != 0x80u) return false;
            code = (code << 6) | (cont & 0x3Fu);
        }
        if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return false;
        i += extra + 1;
    }
    return true;
}

core::Result<DecodedText> decodeMarkdownBytes(std::string_view bytes) {
    DecodedText out;
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        out.hadBom = true;
        bytes.remove_prefix(3);
    }
    if (!isValidUtf8(bytes)) {
        return std::unexpected(core::makeError(
            core::ErrorCode::InvalidDocument, "the file is not valid UTF-8 text (other encodings are not supported)",
            kSubsystem));
    }
    std::size_t crlf = 0;
    std::size_t bareLf = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] != '\n') continue;
        if (i > 0 && bytes[i - 1] == '\r') {
            ++crlf;
        } else {
            ++bareLf;
        }
    }
    out.lineEnding = crlf > bareLf ? LineEnding::CRLF : LineEnding::LF;
    out.mixedLineEndings = crlf > 0 && bareLf > 0;
    if (crlf == 0) {
        out.text.assign(bytes);
    } else {
        out.text.reserve(bytes.size() - crlf);
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            if (bytes[i] == '\r' && i + 1 < bytes.size() && bytes[i + 1] == '\n') continue;
            out.text.push_back(bytes[i]);
        }
    }
    return out;
}

std::string encodeMarkdownBytes(std::string_view text, LineEnding lineEnding, bool bom) {
    std::string out;
    out.reserve(text.size() + (bom ? 3 : 0) + (lineEnding == LineEnding::CRLF ? text.size() / 32 : 0));
    if (bom) out.append("\xEF\xBB\xBF");
    if (lineEnding == LineEnding::LF) {
        out.append(text);
        return out;
    }
    for (const char c : text) {
        if (c == '\n') out.push_back('\r');
        out.push_back(c);
    }
    return out;
}

namespace {
std::uint64_t nextInstanceId() {
    static std::atomic<std::uint64_t> counter{0};
    return ++counter;
}
} // namespace

MarkdownTabState::MarkdownTabState(std::filesystem::path path, DecodedText decoded)
    : instanceId_(nextInstanceId()),
      path_(std::move(path)),
      source_(std::move(decoded.text)),
      lineEnding_(decoded.lineEnding),
      hasBom_(decoded.hadBom),
      mixedLineEndings_(decoded.mixedLineEndings) {
    commands_.setOnChanged([this] { notifyChanged(); });
    savedStateId_ = commands_.stateId();
}

MarkdownTabState::~MarkdownTabState() = default;

core::Result<std::unique_ptr<MarkdownTabState>> MarkdownTabState::load(const std::filesystem::path& path,
                                                                       const std::function<bool()>& isCancelled) {
    const auto cancelled = [&] { return isCancelled != nullptr && isCancelled(); };
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (ec || !std::filesystem::exists(status)) {
        return std::unexpected(core::makeError(core::ErrorCode::NotFound, "the file does not exist", kSubsystem));
    }
    if (!std::filesystem::is_regular_file(status)) {
        return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument, "not a regular file", kSubsystem));
    }
    std::FILE* file = std::fopen(path.string().c_str(), "rb");
    if (file == nullptr) {
        const int err = errno;
        return std::unexpected(core::makeError(
            err == EACCES || err == EPERM ? core::ErrorCode::PermissionDenied : core::ErrorCode::Io,
            err == EACCES || err == EPERM ? "permission denied reading the file" : "could not open the file",
            kSubsystem));
    }
    std::string bytes;
    std::array<char, 1 << 16> chunk{};
    core::Status failure = core::ok();
    for (;;) {
        if (cancelled()) {
            failure = std::unexpected(core::makeError(core::ErrorCode::Cancelled, "open cancelled", kSubsystem));
            break;
        }
        const std::size_t got = std::fread(chunk.data(), 1, chunk.size(), file);
        if (got > 0) {
            if (bytes.size() + got > kMaxMarkdownFileBytes) {
                failure = std::unexpected(core::makeError(core::ErrorCode::InvalidArgument,
                                                          "the file is too large to open as Markdown", kSubsystem));
                break;
            }
            bytes.append(chunk.data(), got);
        }
        if (got < chunk.size()) {
            if (std::ferror(file) != 0) {
                failure = std::unexpected(core::makeError(core::ErrorCode::Io, "error reading the file", kSubsystem));
            }
            break;
        }
    }
    std::fclose(file);
    if (!failure.has_value()) return std::unexpected(failure.error());

    core::Result<DecodedText> decoded = decodeMarkdownBytes(bytes);
    if (!decoded.has_value()) return std::unexpected(decoded.error());
    return std::make_unique<MarkdownTabState>(path, std::move(*decoded));
}

void MarkdownTabState::setMode(MarkdownDisplayMode mode) {
    if (mode_ == mode) return;
    mode_ = mode;
    notifyChanged();
}

bool MarkdownTabState::execute(std::unique_ptr<editor::Command> command) {
    if (editingLocked_) return false;
    return commands_.execute(std::move(command));
}

bool MarkdownTabState::undo() { return !editingLocked_ && commands_.undo(); }
bool MarkdownTabState::redo() { return !editingLocked_ && commands_.redo(); }

bool MarkdownTabState::applyEdit(std::size_t offset, std::size_t removeLength, std::string_view insert) {
    if (offset > source_.size() || removeLength > source_.size() - offset) return false;
    source_.replace(offset, removeLength, insert);
    ++revision_;
    return true; // the command stack notifies after the command settles
}

void MarkdownTabState::markSaved() {
    savedStateId_ = commands_.stateId();
    savedRevision_ = revision_;
    notifyChanged();
}

TextEditCommand::TextEditCommand(MarkdownTabState& state, std::size_t offset, std::size_t removeLength,
                                 std::string insert, std::string_view name)
    : state_(state), offset_(offset), removeLength_(removeLength), insert_(std::move(insert)), name_(name) {}

bool TextEditCommand::execute() {
    const std::string& source = state_.source();
    if (offset_ > source.size() || removeLength_ > source.size() - offset_) return false;
    removed_ = source.substr(offset_, removeLength_);
    return state_.applyEdit(offset_, removeLength_, insert_);
}

bool TextEditCommand::undo() { return state_.applyEdit(offset_, insert_.size(), removed_); }

bool TextEditCommand::tryMerge(std::size_t offset, std::size_t removeLength, std::string_view insert) {
    const std::string& source = state_.source();
    if (offset > source.size() || removeLength > source.size() - offset) return false;
    const std::size_t insertEnd = offset_ + insert_.size();
    std::string newInsert;
    std::string newRemoved;
    std::size_t newOffset = offset_;
    if (offset >= offset_ && offset + removeLength <= insertEnd) { // inside / touching the inserted text
        newInsert = insert_.substr(0, offset - offset_);
        newInsert.append(insert);
        newInsert.append(insert_, offset + removeLength - offset_, std::string::npos);
        newRemoved = removed_;
    } else if (offset == insertEnd) { // directly after it, possibly deleting forward
        newInsert = insert_;
        newInsert.append(insert);
        newRemoved = removed_;
        newRemoved.append(source, offset, removeLength);
    } else if (offset + removeLength == offset_) { // directly before it (backspace)
        newInsert.assign(insert);
        newInsert.append(insert_);
        newRemoved.assign(source, offset, removeLength);
        newRemoved.append(removed_);
        newOffset = offset;
    } else {
        return false;
    }
    if (!state_.applyEdit(offset, removeLength, insert)) return false;
    offset_ = newOffset;
    insert_ = std::move(newInsert);
    removed_ = std::move(newRemoved);
    removeLength_ = removed_.size();
    state_.notifyEdited();
    return true;
}

} // namespace rivet::app
