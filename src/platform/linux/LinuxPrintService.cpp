// SPDX-License-Identifier: MPL-2.0
#include "LinuxPrintService.hpp"

#include "LinuxAlertService.hpp"
#include "LinuxPaintContext.hpp"

#include <cairo-pdf.h>
#include <cairo.h>
#include <cups/cups.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

namespace rivet::platform {

namespace {

constexpr const char* kSubsystem = "platform.linux.print";
constexpr std::uint32_t kJobMagic = 0x52505254; // "RPRT"

core::Error printError(core::ErrorCode code, std::string message) {
    return core::makeError(code, std::move(message), kSubsystem);
}

// The opaque PrintSettings::platformHandle of this backend.
struct CupsJob {
    std::uint32_t magic = kJobMagic;
    std::string printer;
    std::string instance;
};

cairo_status_t writeToFd(void* closure, const unsigned char* data, unsigned int length) {
    const int fd = *static_cast<int*>(closure);
    while (length > 0) {
        const ssize_t written = ::write(fd, data, length);
        if (written <= 0) return CAIRO_STATUS_WRITE_ERROR;
        data += written;
        length -= static_cast<unsigned int>(written);
    }
    return CAIRO_STATUS_SUCCESS;
}

// Reads one band file after validating its geometry and size (the same
// checks as the macOS backend).
core::Result<std::vector<unsigned char>> readBand(const PrintSpoolBand& band) {
    const std::size_t minStride = std::size_t{band.pixelWidth} * 4;
    if (band.pixelWidth == 0 || band.pixelHeight == 0 || band.stride < minStride ||
        band.stride > SIZE_MAX / band.pixelHeight || band.pixelWidth > 32767 || band.pixelHeight > 32767) {
        return std::unexpected(printError(core::ErrorCode::InvalidArgument, "invalid print band geometry"));
    }
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(band.file, error);
    const std::size_t expected = band.stride * band.pixelHeight;
    if (error || size != expected) {
        return std::unexpected(printError(core::ErrorCode::Io, "print spool file is missing or truncated: " +
                                                                   band.file.filename().string()));
    }
    std::vector<unsigned char> bytes(expected);
    std::ifstream in(band.file, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(expected))) {
        return std::unexpected(printError(core::ErrorCode::Io, "cannot read print spool file"));
    }
    return bytes;
}

class TempFile {
public:
    TempFile() {
        const char* dir = std::getenv("TMPDIR");
        std::string pattern = (dir != nullptr && dir[0] == '/' ? std::string(dir) : std::string("/tmp")) +
                              "/rivet-print-XXXXXX.pdf";
        path_.assign(pattern.begin(), pattern.end());
        path_.push_back('\0');
        fd_ = ::mkstemps(path_.data(), 4);
    }
    ~TempFile() {
        if (fd_ >= 0) ::close(fd_);
        if (fd_ != -1) ::unlink(path_.data()); // -1: mkstemps failed, nothing was created
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    int fd() const { return fd_; }
    const char* path() const { return path_.data(); }
    void closeFd() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -2; // closed, but the file still needs unlinking
    }

private:
    std::vector<char> path_;
    int fd_ = -1;
};

} // namespace

core::Result<PrintSettings> LinuxPrintService::choosePrintSettings(const PrintSetup& setup) {
    if (setup.pageCount == 0) {
        return std::unexpected(printError(core::ErrorCode::InvalidArgument, "the document has no pages to print"));
    }
    cups_dest_t* dests = nullptr;
    const int count = cupsGetDests(&dests);
    cups_dest_t* fallback = cupsGetDest(nullptr, nullptr, count, dests);
    if (fallback == nullptr || fallback->name == nullptr) {
        cupsFreeDests(count, dests);
        return std::unexpected(
            printError(core::ErrorCode::NotAvailable, "No default printer is configured (CUPS)."));
    }
    auto job = std::make_shared<CupsJob>();
    job->printer = fallback->name;
    if (fallback->instance != nullptr) job->instance = fallback->instance;
    cupsFreeDests(count, dests);

    if (alerts_ != nullptr) {
        const std::string pages = setup.pageCount == 1 ? std::string("1 page") : std::to_string(setup.pageCount) + " pages";
        if (!alerts_->confirm("Print \xE2\x80\x9C" + setup.jobTitle + "\xE2\x80\x9D?",
                              pages + " will be printed on \xE2\x80\x9C" + job->printer + "\xE2\x80\x9D.", "Print")) {
            return std::unexpected(printError(core::ErrorCode::Cancelled, "user dismissed the print dialog"));
        }
    }

    PrintSettings settings;
    settings.jobTitle = setup.jobTitle;
    settings.firstPage = 0;
    settings.lastPage = setup.pageCount - 1;
    settings.platformHandle = std::static_pointer_cast<void>(job);
    return settings;
}

core::Status LinuxPrintService::printSpool(const PrintSettings& settings, const PrintSpoolDescription& spool) {
    const auto* job = static_cast<const CupsJob*>(settings.platformHandle.get());
    if (job == nullptr || job->magic != kJobMagic || job->printer.empty()) {
        return std::unexpected(printError(core::ErrorCode::InvalidArgument, "print settings are not from CUPS"));
    }
    if (spool.pages.empty()) return std::unexpected(printError(core::ErrorCode::InvalidArgument, "nothing to print"));

    TempFile file;
    if (file.fd() < 0) return std::unexpected(printError(core::ErrorCode::Io, "cannot create a print spool file"));
    int fd = file.fd();

    const PrintSpoolPage& first = spool.pages.front();
    cairo_surface_t* pdf = cairo_pdf_surface_create_for_stream(writeToFd, &fd, first.displaySizePoints.width,
                                                               first.displaySizePoints.height);
    cairo_t* cr = cairo_create(pdf);
    core::Status status = core::ok();
    for (const PrintSpoolPage& page : spool.pages) {
        if (!(page.displaySizePoints.width > 0.0) || !(page.displaySizePoints.height > 0.0)) {
            status = std::unexpected(printError(core::ErrorCode::InvalidArgument, "invalid print page size"));
            break;
        }
        cairo_pdf_surface_set_size(pdf, page.displaySizePoints.width, page.displaySizePoints.height);
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_paint(cr);
        for (const PrintSpoolBand& band : page.bands) {
            auto bytes = readBand(band);
            if (!bytes) {
                status = std::unexpected(bytes.error());
                break;
            }
            cairo_surface_t* image =
                createSurfaceFromStraightBgra(bytes->data(), static_cast<int>(band.pixelWidth),
                                              static_cast<int>(band.pixelHeight), band.stride);
            if (image == nullptr) {
                status = std::unexpected(printError(core::ErrorCode::OutOfMemory, "cannot buffer a print band"));
                break;
            }
            cairo_save(cr);
            cairo_rectangle(cr, band.rectPoints.minX(), band.rectPoints.minY(), band.rectPoints.size.width,
                            band.rectPoints.size.height);
            cairo_clip(cr);
            cairo_translate(cr, band.rectPoints.minX(), band.rectPoints.minY());
            cairo_scale(cr, band.rectPoints.size.width / band.pixelWidth,
                        band.rectPoints.size.height / band.pixelHeight);
            cairo_set_source_surface(cr, image, 0.0, 0.0);
            cairo_paint(cr);
            cairo_restore(cr);
            cairo_surface_destroy(image);
        }
        if (!status) break;
        cairo_show_page(cr);
    }
    cairo_destroy(cr);
    cairo_surface_finish(pdf);
    const cairo_status_t pdfStatus = cairo_surface_status(pdf);
    cairo_surface_destroy(pdf);
    if (!status) return status;
    if (pdfStatus != CAIRO_STATUS_SUCCESS) {
        return std::unexpected(printError(core::ErrorCode::Io, "cannot write the print spool file"));
    }
    file.closeFd();

    cups_option_t* options = nullptr;
    int optionCount = 0;
    optionCount = cupsAddOption("fit-to-page", "true", optionCount, &options);
    const std::string title = settings.jobTitle.empty() ? std::string("Rivet") : settings.jobTitle;
    const int jobId = cupsPrintFile(job->printer.c_str(), file.path(), title.c_str(), optionCount, options);
    cupsFreeOptions(optionCount, options);
    if (jobId == 0) {
        const char* reason = cupsLastErrorString();
        return std::unexpected(printError(core::ErrorCode::Io, std::string("CUPS refused the print job: ") +
                                                                   (reason != nullptr ? reason : "unknown error")));
    }
    return core::ok();
}

} // namespace rivet::platform
