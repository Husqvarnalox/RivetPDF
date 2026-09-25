// SPDX-License-Identifier: MPL-2.0

#include "PdfiumFileSource.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <limits>
#include <new>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace rivet::pdf {

core::Result<std::shared_ptr<PdfiumFileSource>> PdfiumFileSource::open(const std::filesystem::path& path) {
    // Only the file path is ever reported, never document contents.
    int fd = -1;
    do {
        fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        const int error = errno;
        return std::unexpected(core::makeError(
            core::ErrorCode::Io,
            "could not open the file: " + path.string() + " (" +
                std::generic_category().message(error) + ")",
            "pdf"));
    }

    struct stat info {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        ::close(fd);
        return std::unexpected(core::makeError(core::ErrorCode::Io,
                                               "not a readable regular file: " + path.string(), "pdf"));
    }
    if (info.st_size <= 0) {
        ::close(fd);
        return std::unexpected(core::makeError(core::ErrorCode::InvalidDocument,
                                               "the file is empty: " + path.string(), "pdf"));
    }
    // FPDF_FILEACCESS::m_FileLen is an unsigned long (32-bit on some
    // platforms); refuse sizes it cannot describe.
    const auto size = static_cast<std::uint64_t>(info.st_size);
    if (size > std::numeric_limits<unsigned long>::max()) {
        ::close(fd);
        return std::unexpected(core::makeError(core::ErrorCode::Unsupported,
                                               "the file is too large: " + path.string(), "pdf"));
    }

    auto* source = new (std::nothrow) PdfiumFileSource(fd, size);
    if (source == nullptr) {
        ::close(fd);
        return std::unexpected(core::makeError(core::ErrorCode::OutOfMemory,
                                               "could not allocate the file source", "pdf"));
    }
    return std::shared_ptr<PdfiumFileSource>(source);
}

PdfiumFileSource::~PdfiumFileSource() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

FPDF_FILEACCESS PdfiumFileSource::fileAccess() const {
    FPDF_FILEACCESS access{};
    access.m_FileLen = static_cast<unsigned long>(size_);
    access.m_GetBlock = &PdfiumFileSource::readBlock;
    // PDFium's m_Param is non-const; readBlock only reads through it.
    access.m_Param = const_cast<PdfiumFileSource*>(this);
    return access;
}

int PdfiumFileSource::readBlock(void* param, unsigned long position, unsigned char* buffer,
                                unsigned long size) {
    const auto* source = static_cast<const PdfiumFileSource*>(param);
    if (source == nullptr || buffer == nullptr) {
        return 0;
    }
    // CPDFSDK_CustomAccess already bounds requests by m_FileLen; re-check so
    // this callback is safe on its own.
    const std::uint64_t begin = position;
    const std::uint64_t length = size;
    if (begin > source->size_ || length > source->size_ - begin) {
        return 0;
    }

    std::uint64_t done = 0;
    while (done < length) {
        const std::uint64_t remaining = length - done;
        const std::size_t chunk = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, std::numeric_limits<ssize_t>::max()));
        const ssize_t got = ::pread(source->fd_, buffer + done, chunk, static_cast<off_t>(begin + done));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 0;
        }
        if (got == 0) {
            return 0; // the file shrank underneath us: fail, never pad
        }
        done += static_cast<std::uint64_t>(got);
    }
    return 1;
}

} // namespace rivet::pdf
