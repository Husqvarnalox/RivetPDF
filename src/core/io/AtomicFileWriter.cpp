// SPDX-License-Identifier: MPL-2.0
#include "core/io/AtomicFileWriter.hpp"

#include "core/Log.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <format>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#if defined(__APPLE__)
#include <copyfile.h>
#include <stdio.h> // renamex_np, RENAME_EXCL
#endif

namespace rivet::core::io {

namespace {

constexpr std::string_view kSubsystem = "core.io";

Error makeIoError(ErrorCode code, std::string message) {
    return makeError(code, std::move(message), std::string(kSubsystem));
}

// Maps an errno from `operation` to a Rivet error with a user-presentable
// message. Paths are deliberately not included (privacy; the UI knows the
// file it was saving).
Error errnoError(int err, std::string_view operation) {
    switch (err) {
        case EACCES:
        case EPERM:
            return makeIoError(ErrorCode::PermissionDenied,
                               std::format("{}: permission denied", operation));
        case EROFS:
            return makeIoError(ErrorCode::PermissionDenied,
                               std::format("{}: the volume is read-only", operation));
        case ENOSPC:
            return makeIoError(ErrorCode::DiskFull,
                               std::format("{}: not enough free space on the volume", operation));
#if defined(EDQUOT)
        case EDQUOT:
            return makeIoError(ErrorCode::DiskFull,
                               std::format("{}: disk quota exceeded", operation));
#endif
        case ENOENT:
        case ENOTDIR:
            return makeIoError(ErrorCode::NotFound,
                               std::format("{}: the destination folder does not exist", operation));
        case EISDIR:
            return makeIoError(ErrorCode::InvalidArgument,
                               std::format("{}: the destination is a folder", operation));
        case ENAMETOOLONG:
            return makeIoError(ErrorCode::InvalidArgument,
                               std::format("{}: the file name is too long", operation));
        case EEXIST:
            return makeIoError(ErrorCode::AlreadyExists,
                               std::format("{}: a file with that name already exists", operation));
        case ELOOP:
            return makeIoError(ErrorCode::InvalidArgument,
                               std::format("{}: too many levels of symbolic links", operation));
        case ECANCELED:
            return makeIoError(ErrorCode::Cancelled, std::format("{}: cancelled", operation));
        default:
            return makeIoError(ErrorCode::Io, std::format("{}: {}", operation,
                                                          std::generic_category().message(err)));
    }
}

int injected(const AtomicWriteOptions& options, AtomicWriteFault point) {
    return options.faultInjector ? options.faultInjector(point) : 0;
}

bool cancelled(const AtomicWriteOptions& options) {
    return options.cancelFlag != nullptr && options.cancelFlag->load(std::memory_order_acquire);
}

Error cancelledError() {
    return makeIoError(ErrorCode::Cancelled, "save was cancelled");
}

std::string randomSuffix() {
    static std::atomic<std::uint64_t> counter{0};
    std::random_device device;
    const std::uint64_t value = (static_cast<std::uint64_t>(device()) << 32U) ^ device() ^
                                counter.fetch_add(0x9E3779B97F4A7C15ULL, std::memory_order_relaxed);
    return std::format("{:012x}", value & 0xFFFFFFFFFFFFULL);
}

#if !defined(_WIN32)

// Follows a chain of symlinks at `path` to the final (possibly nonexistent)
// target. errno-style failure code on error.
Result<std::filesystem::path> resolveSymlinks(std::filesystem::path path) {
    for (int hop = 0; hop < 32; ++hop) {
        struct stat st{};
        if (::lstat(path.c_str(), &st) != 0) {
            const int err = errno;
            if (err == ENOENT) return path; // new file (or dangling link target)
            return std::unexpected(errnoError(err, "cannot inspect the destination"));
        }
        if (!S_ISLNK(st.st_mode)) return path;
        std::error_code ec;
        const std::filesystem::path target = std::filesystem::read_symlink(path, ec);
        if (ec) return std::unexpected(errnoError(ec.value(), "cannot read the symbolic link"));
        path = target.is_absolute() ? target : path.parent_path() / target;
        path = path.lexically_normal();
    }
    return std::unexpected(errnoError(ELOOP, "cannot resolve the destination"));
}

// Leading part of `name`, at most `maxBytes` bytes, cut at a UTF-8 boundary
// (APFS rejects names that are not valid UTF-8).
std::string utf8Prefix(const std::string& name, std::size_t maxBytes) {
    if (name.size() <= maxBytes) return name;
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(name[cut]) & 0xC0U) == 0x80U) --cut;
    return name.substr(0, cut);
}

void closeQuietly(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

// Full data + metadata flush. On Apple fsync() does not flush the drive's
// write cache; F_FULLFSYNC does (falls back to fsync where unsupported).
int fullSync(int fd) {
#if defined(__APPLE__)
    if (::fcntl(fd, F_FULLFSYNC) == 0) return 0;
#endif
    while (::fsync(fd) != 0) {
        if (errno != EINTR) return -1;
    }
    return 0;
}

#endif // !_WIN32

} // namespace

#if defined(_WIN32)

namespace {

// Maps a Win32 error code from `operation` to a Rivet error (no paths in the
// message, like errnoError).
Error win32Error(DWORD err, std::string_view operation) {
    switch (err) {
        case ERROR_ACCESS_DENIED:
            return makeIoError(ErrorCode::PermissionDenied, std::format("{}: permission denied", operation));
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
        case ERROR_USER_MAPPED_FILE:
            return makeIoError(ErrorCode::PermissionDenied,
                               std::format("{}: the file is in use by another program", operation));
        case ERROR_WRITE_PROTECT:
            return makeIoError(ErrorCode::PermissionDenied, std::format("{}: the volume is read-only", operation));
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
            return makeIoError(ErrorCode::DiskFull,
                               std::format("{}: not enough free space on the volume", operation));
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_BAD_NETPATH:
            return makeIoError(ErrorCode::NotFound,
                               std::format("{}: the destination folder does not exist", operation));
        case ERROR_FILENAME_EXCED_RANGE:
        case ERROR_INVALID_NAME:
            return makeIoError(ErrorCode::InvalidArgument,
                               std::format("{}: the file name is too long or invalid", operation));
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:
            return makeIoError(ErrorCode::AlreadyExists,
                               std::format("{}: a file with that name already exists", operation));
        case ERROR_CANCELLED:
            return makeIoError(ErrorCode::Cancelled, std::format("{}: cancelled", operation));
        default:
            return makeIoError(ErrorCode::Io, std::format("{}: {}", operation,
                                                          std::system_category().message(static_cast<int>(err))));
    }
}

// Follows symlinks/junctions at `path` (up to 32 hops) to the final, possibly
// nonexistent, target.
Result<std::filesystem::path> resolveSymlinksWin(std::filesystem::path path) {
    for (int hop = 0; hop < 32; ++hop) {
        std::error_code ec;
        const std::filesystem::file_status status = std::filesystem::symlink_status(path, ec);
        if (ec || status.type() == std::filesystem::file_type::not_found) return path;
        if (status.type() != std::filesystem::file_type::symlink) return path;
        const std::filesystem::path target = std::filesystem::read_symlink(path, ec);
        if (ec) return path; // not a resolvable link (e.g. an app-exec link): replace the entry itself
        path = target.is_absolute() ? target : path.parent_path() / target;
        path = path.lexically_normal();
    }
    return std::unexpected(errnoError(ELOOP, "cannot resolve the destination"));
}

// Leading part of a UTF-16 file name, at most `maxUnits` code units, never
// splitting a surrogate pair.
std::wstring widePrefix(const std::wstring& name, std::size_t maxUnits) {
    if (name.size() <= maxUnits) return name;
    std::size_t cut = maxUnits;
    if (cut > 0 && name[cut - 1] >= 0xD800 && name[cut - 1] <= 0xDBFF) --cut;
    return name.substr(0, cut);
}

void closeHandleQuietly(void*& handle) {
    if (handle != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(handle));
        handle = nullptr;
    }
}

} // namespace

Result<std::unique_ptr<AtomicFileWriter>> AtomicFileWriter::begin(const std::filesystem::path& destination,
                                                                  AtomicWriteOptions options) {
    if (destination.empty() || !destination.has_filename()) {
        return std::unexpected(makeIoError(ErrorCode::InvalidArgument, "the destination file name is empty"));
    }
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(destination, ec);
    if (ec) return std::unexpected(makeIoError(ErrorCode::InvalidArgument, "cannot resolve the destination"));

    auto resolved = resolveSymlinksWin(absolute.lexically_normal());
    if (!resolved) return std::unexpected(resolved.error());

    std::unique_ptr<AtomicFileWriter> writer(new AtomicFileWriter());
    writer->destination_ = std::move(*resolved);
    writer->options_ = std::move(options);

    const std::filesystem::path& dest = writer->destination_;
    std::filesystem::path directory = dest.parent_path();
    if (directory.empty()) directory = ".";
    const std::wstring fileName = dest.filename().wstring();
    if (fileName.empty() || fileName == L"." || fileName == L"..") {
        return std::unexpected(makeIoError(ErrorCode::InvalidArgument, "the destination file name is invalid"));
    }

    const DWORD dirAttributes = ::GetFileAttributesW(directory.c_str());
    if (dirAttributes == INVALID_FILE_ATTRIBUTES) return std::unexpected(win32Error(::GetLastError(), "cannot save"));
    if ((dirAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return std::unexpected(errnoError(ENOTDIR, "cannot save"));
    if (fileName.size() > 255) return std::unexpected(errnoError(ENAMETOOLONG, "cannot save"));

    const DWORD destAttributes = ::GetFileAttributesW(dest.c_str());
    if (destAttributes != INVALID_FILE_ATTRIBUTES) {
        if ((destAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return std::unexpected(errnoError(EISDIR, "cannot save"));
        }
        if (!writer->options_.overwriteExisting) return std::unexpected(errnoError(EEXIST, "cannot save"));
        if ((destAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
            return std::unexpected(
                makeIoError(ErrorCode::PermissionDenied, "cannot save: the file is read-only or locked"));
        }
        // Probe write access (ACLs) without modifying the file.
        HANDLE probe = ::CreateFileW(dest.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (probe == INVALID_HANDLE_VALUE) {
            const DWORD err = ::GetLastError();
            if (err == ERROR_ACCESS_DENIED) {
                return std::unexpected(
                    makeIoError(ErrorCode::PermissionDenied, "cannot save: the file is read-only or locked"));
            }
            return std::unexpected(win32Error(err, "cannot save"));
        }
        ::CloseHandle(probe);
        writer->destinationExisted_ = true;
    } else {
        const DWORD err = ::GetLastError();
        if (err != ERROR_FILE_NOT_FOUND) return std::unexpected(win32Error(err, "cannot save"));
    }

    const std::wstring base = widePrefix(fileName, 32);
    HANDLE handle = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 16; ++attempt) {
        const std::string suffix = randomSuffix();
        std::filesystem::path candidate =
            directory / (L"." + base + L".rivet-" + std::wstring(suffix.begin(), suffix.end()) + L".tmp");
        handle = ::CreateFileW(candidate.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            writer->tempPath_ = std::move(candidate);
            break;
        }
        const DWORD err = ::GetLastError();
        if (err != ERROR_FILE_EXISTS && err != ERROR_ALREADY_EXISTS) {
            return std::unexpected(win32Error(err, "cannot create the file"));
        }
    }
    if (handle == INVALID_HANDLE_VALUE) return std::unexpected(errnoError(EEXIST, "cannot create a temporary file"));
    writer->handle_ = handle;
    return writer;
}

AtomicFileWriter::~AtomicFileWriter() { abort(); }

Status AtomicFileWriter::fail(Error error) {
    closeHandleQuietly(handle_);
    if (!tempPath_.empty()) ::DeleteFileW(tempPath_.c_str());
    state_ = State::Failed;
    error_ = error;
    return std::unexpected(std::move(error));
}

void AtomicFileWriter::abort() {
    if (state_ != State::Open) return;
    closeHandleQuietly(handle_);
    if (!tempPath_.empty()) ::DeleteFileW(tempPath_.c_str());
    state_ = State::Aborted;
}

Status AtomicFileWriter::write(const void* data, std::size_t size) {
    switch (state_) {
        case State::Open: break;
        case State::Failed: return std::unexpected(error_);
        case State::Committed: return std::unexpected(makeIoError(ErrorCode::InvalidArgument, "write after commit"));
        case State::Aborted: return std::unexpected(makeIoError(ErrorCode::Cancelled, "write after abort"));
    }
    if (cancelled(options_)) return fail(cancelledError());
    if (size == 0) return ok();
    if (data == nullptr) return fail(makeIoError(ErrorCode::InvalidArgument, "write of a null buffer"));

    const auto* bytes = static_cast<const unsigned char*>(data);
    std::size_t remaining = size;
    while (remaining > 0) {
        if (const int fault = injected(options_, AtomicWriteFault::Write); fault != 0) {
            return fail(errnoError(fault, "cannot write the file"));
        }
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, 1U << 30));
        DWORD written = 0;
        if (!::WriteFile(static_cast<HANDLE>(handle_), bytes, chunk, &written, nullptr)) {
            return fail(win32Error(::GetLastError(), "cannot write the file"));
        }
        if (written == 0) return fail(errnoError(EIO, "cannot write the file"));
        bytes += written;
        remaining -= written;
        bytesWritten_ += written;
    }
    return ok();
}

Status AtomicFileWriter::commit() {
    switch (state_) {
        case State::Open: break;
        case State::Failed: return std::unexpected(error_);
        case State::Committed: return std::unexpected(makeIoError(ErrorCode::InvalidArgument, "already committed"));
        case State::Aborted: return std::unexpected(makeIoError(ErrorCode::Cancelled, "commit after abort"));
    }
    if (cancelled(options_)) return fail(cancelledError());

    if (const int fault = injected(options_, AtomicWriteFault::Sync); fault != 0) {
        return fail(errnoError(fault, "cannot flush the file to disk"));
    }
    HANDLE handle = static_cast<HANDLE>(handle_);
    if (!::FlushFileBuffers(handle)) return fail(win32Error(::GetLastError(), "cannot flush the file to disk"));

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(handle, &size)) return fail(win32Error(::GetLastError(), "cannot verify the file"));
    if (static_cast<std::uint64_t>(size.QuadPart) != bytesWritten_) {
        return fail(makeIoError(ErrorCode::Io, std::format("cannot verify the file: size is {} bytes, expected {}",
                                                           static_cast<long long>(size.QuadPart), bytesWritten_)));
    }
    closeHandleQuietly(handle_);

    if (cancelled(options_)) return fail(cancelledError());
    if (const int fault = injected(options_, AtomicWriteFault::Rename); fault != 0) {
        return fail(errnoError(fault, "cannot replace the file"));
    }

    bool replaced = false;
    DWORD err = ERROR_SUCCESS;
    if (options_.overwriteExisting && destinationExisted_) {
        // Keeps the destination's ACLs, attributes and streams.
        replaced = ::ReplaceFileW(destination_.c_str(), tempPath_.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS,
                                  nullptr, nullptr) != FALSE;
        if (!replaced) {
            err = ::GetLastError();
            // Sharing/permission failures are final; anything else (e.g. a
            // file system without ReplaceFile support) falls back to a move.
            if (err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED && err != ERROR_LOCK_VIOLATION &&
                err != ERROR_USER_MAPPED_FILE) {
                replaced = ::MoveFileExW(tempPath_.c_str(), destination_.c_str(),
                                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
                if (!replaced) err = ::GetLastError();
            }
        }
    } else {
        // Without REPLACE_EXISTING the move fails atomically when the
        // destination appeared meanwhile (overwriteExisting == false).
        const DWORD flags = MOVEFILE_WRITE_THROUGH | (options_.overwriteExisting ? MOVEFILE_REPLACE_EXISTING : 0U);
        replaced = ::MoveFileExW(tempPath_.c_str(), destination_.c_str(), flags) != FALSE;
        if (!replaced) err = ::GetLastError();
    }
    if (!replaced) return fail(win32Error(err, "cannot replace the file"));
    state_ = State::Committed;

    // NTFS has no directory fsync; MOVEFILE_WRITE_THROUGH / ReplaceFileW
    // already flushed the rename. The fault point is kept for parity.
    if (const int syncError = injected(options_, AtomicWriteFault::DirSync); syncError != 0) {
        log::warning(std::format("atomic save: directory fsync failed after replace: {}",
                                 std::generic_category().message(syncError)));
    }
    return ok();
}

#else

Result<std::unique_ptr<AtomicFileWriter>> AtomicFileWriter::begin(
    const std::filesystem::path& destination, AtomicWriteOptions options) {
    if (destination.empty() || !destination.has_filename()) {
        return std::unexpected(
            makeIoError(ErrorCode::InvalidArgument, "the destination file name is empty"));
    }
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(destination, ec);
    if (ec) return std::unexpected(errnoError(ec.value(), "cannot resolve the destination"));

    auto resolved = resolveSymlinks(absolute.lexically_normal());
    if (!resolved) return std::unexpected(resolved.error());

    std::unique_ptr<AtomicFileWriter> writer(new AtomicFileWriter());
    writer->destination_ = std::move(*resolved);
    writer->options_ = std::move(options);

    const std::filesystem::path& dest = writer->destination_;
    std::filesystem::path directory = dest.parent_path();
    if (directory.empty()) directory = ".";
    const std::string fileName = dest.filename().string();
    if (fileName.empty() || fileName == "." || fileName == "..") {
        return std::unexpected(
            makeIoError(ErrorCode::InvalidArgument, "the destination file name is invalid"));
    }

    // Destination directory must exist and be a directory.
    struct stat dirStat{};
    if (::stat(directory.c_str(), &dirStat) != 0) {
        return std::unexpected(errnoError(errno, "cannot save"));
    }
    if (!S_ISDIR(dirStat.st_mode)) {
        return std::unexpected(errnoError(ENOTDIR, "cannot save"));
    }

    long nameMax = ::pathconf(directory.c_str(), _PC_NAME_MAX);
    if (nameMax <= 0) nameMax = 255;
    if (fileName.size() > static_cast<std::size_t>(nameMax)) {
        return std::unexpected(errnoError(ENAMETOOLONG, "cannot save"));
    }

    // Existing destination: must be a writable regular file.
    struct stat destStat{};
    if (::stat(dest.c_str(), &destStat) == 0) {
        if (S_ISDIR(destStat.st_mode)) return std::unexpected(errnoError(EISDIR, "cannot save"));
        if (!S_ISREG(destStat.st_mode)) {
            return std::unexpected(makeIoError(ErrorCode::InvalidArgument,
                                               "cannot save: the destination is not a regular file"));
        }
        if (!writer->options_.overwriteExisting) {
            return std::unexpected(errnoError(EEXIST, "cannot save"));
        }
        if (::access(dest.c_str(), W_OK) != 0) {
            const int err = errno;
            if (err == EROFS) return std::unexpected(errnoError(EROFS, "cannot save"));
            return std::unexpected(makeIoError(ErrorCode::PermissionDenied,
                                               "cannot save: the file is read-only or locked"));
        }
        writer->destinationExisted_ = true;
        writer->finalMode_ = static_cast<unsigned>(destStat.st_mode) & 07777U;
        writer->ownerUid_ = static_cast<long long>(destStat.st_uid);
        writer->ownerGid_ = static_cast<long long>(destStat.st_gid);
    } else if (errno != ENOENT) {
        return std::unexpected(errnoError(errno, "cannot save"));
    }

    // Unique hidden temp next to the destination. Created with 0666 so the
    // kernel applies the umask (observed via fstat: race-free, unlike
    // umask(2) which is process-global), then immediately narrowed to 0600
    // before any byte is written.
    const std::string base = utf8Prefix(fileName, 32);
    int fd = -1;
    for (int attempt = 0; attempt < 16; ++attempt) {
        std::filesystem::path candidate =
            directory / std::format(".{}.rivet-{}.tmp", base, randomSuffix());
        fd = ::open(candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (fd >= 0) {
            writer->tempPath_ = std::move(candidate);
            break;
        }
        if (errno != EEXIST) return std::unexpected(errnoError(errno, "cannot create the file"));
    }
    if (fd < 0) return std::unexpected(errnoError(EEXIST, "cannot create a temporary file"));
    writer->fd_ = fd;

    struct stat tempStat{};
    if (::fstat(fd, &tempStat) != 0) {
        const int err = errno;
        (void)writer->fail(errnoError(err, "cannot create the file"));
        return std::unexpected(writer->error_);
    }
    if (!writer->destinationExisted_) {
        writer->finalMode_ = 0644U & static_cast<unsigned>(tempStat.st_mode);
    }
    if (::fchmod(fd, 0600) != 0) {
        const int err = errno;
        (void)writer->fail(errnoError(err, "cannot create the file"));
        return std::unexpected(writer->error_);
    }
    return writer;
}

AtomicFileWriter::~AtomicFileWriter() { abort(); }

Status AtomicFileWriter::fail(Error error) {
    closeQuietly(fd_);
    if (!tempPath_.empty()) ::unlink(tempPath_.c_str());
    state_ = State::Failed;
    error_ = error;
    return std::unexpected(std::move(error));
}

void AtomicFileWriter::abort() {
    if (state_ != State::Open) return;
    closeQuietly(fd_);
    if (!tempPath_.empty()) ::unlink(tempPath_.c_str());
    state_ = State::Aborted;
}

Status AtomicFileWriter::write(const void* data, std::size_t size) {
    switch (state_) {
        case State::Open: break;
        case State::Failed: return std::unexpected(error_);
        case State::Committed:
            return std::unexpected(makeIoError(ErrorCode::InvalidArgument, "write after commit"));
        case State::Aborted:
            return std::unexpected(makeIoError(ErrorCode::Cancelled, "write after abort"));
    }
    if (cancelled(options_)) return fail(cancelledError());
    if (size == 0) return ok();
    if (data == nullptr) {
        return fail(makeIoError(ErrorCode::InvalidArgument, "write of a null buffer"));
    }

    const auto* bytes = static_cast<const unsigned char*>(data);
    std::size_t remaining = size;
    while (remaining > 0) {
        if (const int fault = injected(options_, AtomicWriteFault::Write); fault != 0) {
            return fail(errnoError(fault, "cannot write the file"));
        }
        const ssize_t written = ::write(fd_, bytes, remaining);
        if (written < 0) {
            if (errno == EINTR) continue;
            return fail(errnoError(errno, "cannot write the file"));
        }
        if (written == 0) return fail(errnoError(EIO, "cannot write the file"));
        bytes += written;
        remaining -= static_cast<std::size_t>(written);
        bytesWritten_ += static_cast<std::uint64_t>(written);
    }
    return ok();
}

Status AtomicFileWriter::commit() {
    switch (state_) {
        case State::Open: break;
        case State::Failed: return std::unexpected(error_);
        case State::Committed:
            return std::unexpected(makeIoError(ErrorCode::InvalidArgument, "already committed"));
        case State::Aborted:
            return std::unexpected(makeIoError(ErrorCode::Cancelled, "commit after abort"));
    }
    if (cancelled(options_)) return fail(cancelledError());

#if defined(__APPLE__)
    // Best effort: carry the old file's extended attributes (Finder tags,
    // etc.) over to the replacement. Failure never fails the save.
    if (destinationExisted_) {
        const int source = ::open(destination_.c_str(), O_RDONLY | O_CLOEXEC);
        if (source >= 0) {
            (void)::fcopyfile(source, fd_, nullptr, COPYFILE_XATTR);
            ::close(source);
        }
    }
#endif

    if (::fchmod(fd_, static_cast<mode_t>(finalMode_)) != 0) {
        return fail(errnoError(errno, "cannot set file permissions"));
    }
    if (destinationExisted_) {
        // Keep the original owner/group where permitted (e.g. saving a file
        // owned by another group member). Not permitted -> stays ours.
        // glibc marks fchown warn_unused_result; a (void) cast does not
        // silence GCC, so consume the result explicitly.
        if (::fchown(fd_, static_cast<uid_t>(ownerUid_), static_cast<gid_t>(ownerGid_)) != 0) {
            // best effort: keep our ownership
        }
    }

    if (const int fault = injected(options_, AtomicWriteFault::Sync); fault != 0) {
        return fail(errnoError(fault, "cannot flush the file to disk"));
    }
    if (fullSync(fd_) != 0) return fail(errnoError(errno, "cannot flush the file to disk"));

    struct stat st{};
    if (::fstat(fd_, &st) != 0) return fail(errnoError(errno, "cannot verify the file"));
    if (static_cast<std::uint64_t>(st.st_size) != bytesWritten_) {
        return fail(makeIoError(ErrorCode::Io, std::format("cannot verify the file: size is {} bytes, "
                                                           "expected {}",
                                                           static_cast<long long>(st.st_size),
                                                           bytesWritten_)));
    }
    const int fd = fd_;
    fd_ = -1;
    if (::close(fd) != 0 && errno != EINTR) {
        return fail(errnoError(errno, "cannot close the file"));
    }

    if (cancelled(options_)) return fail(cancelledError());
    if (const int fault = injected(options_, AtomicWriteFault::Rename); fault != 0) {
        return fail(errnoError(fault, "cannot replace the file"));
    }

    int renamed = 0;
    if (options_.overwriteExisting) {
        renamed = ::rename(tempPath_.c_str(), destination_.c_str());
    } else {
#if defined(__APPLE__)
        renamed = ::renamex_np(tempPath_.c_str(), destination_.c_str(), RENAME_EXCL);
#else
        // Best effort without renameat2: re-check right before the rename.
        struct stat existing{};
        if (::lstat(destination_.c_str(), &existing) == 0) {
            errno = EEXIST;
            renamed = -1;
        } else {
            renamed = ::rename(tempPath_.c_str(), destination_.c_str());
        }
#endif
    }
    if (renamed != 0) return fail(errnoError(errno, "cannot replace the file"));
    state_ = State::Committed;

    // Make the new directory entry durable. The replace already happened and
    // cannot be undone, so a failure here is only reported in the log.
    int syncError = injected(options_, AtomicWriteFault::DirSync);
    if (syncError == 0) {
        std::filesystem::path directory = destination_.parent_path();
        if (directory.empty()) directory = ".";
        const int dirFd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dirFd < 0) {
            syncError = errno;
        } else {
            if (fullSync(dirFd) != 0) syncError = errno;
            ::close(dirFd);
        }
    }
    if (syncError != 0 && syncError != EINVAL && syncError != ENOTSUP) {
        log::warning(std::format("atomic save: directory fsync failed after replace: {}",
                                 std::generic_category().message(syncError)));
    }
    return ok();
}

#endif // _WIN32

Status writeFileAtomically(const std::filesystem::path& destination,
                           const std::function<Status(IByteSink&)>& produce,
                           AtomicWriteOptions options) {
    auto writer = AtomicFileWriter::begin(destination, std::move(options));
    if (!writer) return std::unexpected(writer.error());
    if (produce) {
        if (Status produced = produce(**writer); !produced) {
            (*writer)->abort();
            return produced;
        }
    }
    return (*writer)->commit();
}

} // namespace rivet::core::io
