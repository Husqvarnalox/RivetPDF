// SPDX-License-Identifier: MPL-2.0
#include "WindowsPrintService.hpp"

#include "WinCommon.hpp"
#include "WindowsMainThreadDispatcher.hpp"

#include <commdlg.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace rivet::platform {

namespace {

constexpr const char* kSubsystem = "platform.windows.print";
constexpr std::uint32_t kJobMagic = 0x57505254; // "WPRT"

core::Error printError(core::ErrorCode code, std::string message) {
    return core::makeError(code, std::move(message), kSubsystem);
}

// The opaque PrintSettings::platformHandle of this backend.
struct WinPrintJob {
    std::uint32_t magic = kJobMagic;
    HDC dc = nullptr;
    HGLOBAL devMode = nullptr;
    HGLOBAL devNames = nullptr;

    WinPrintJob() = default;
    WinPrintJob(const WinPrintJob&) = delete;
    WinPrintJob& operator=(const WinPrintJob&) = delete;
    ~WinPrintJob() {
        if (dc != nullptr) ::DeleteDC(dc);
        if (devMode != nullptr) ::GlobalFree(devMode);
        if (devNames != nullptr) ::GlobalFree(devNames);
    }
};

// Reads one band file after validating its geometry and size (the same
// checks as the macOS and Linux backends).
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
                                                                   win::narrow(band.file.filename().wstring())));
    }
    std::vector<unsigned char> bytes(expected);
    std::ifstream in(band.file, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(expected))) {
        return std::unexpected(printError(core::ErrorCode::Io, "cannot read print spool file"));
    }
    return bytes;
}

// Straight-alpha BGRA rows -> opaque 32-bit BGRX rows composited over white
// (paper), tightly packed as StretchDIBits expects.
std::vector<unsigned char> overWhite(const std::vector<unsigned char>& bytes, const PrintSpoolBand& band) {
    const std::size_t rowBytes = std::size_t{band.pixelWidth} * 4;
    std::vector<unsigned char> out(rowBytes * band.pixelHeight);
    for (std::uint32_t y = 0; y < band.pixelHeight; ++y) {
        const unsigned char* src = bytes.data() + static_cast<std::size_t>(y) * band.stride;
        unsigned char* dst = out.data() + static_cast<std::size_t>(y) * rowBytes;
        for (std::uint32_t x = 0; x < band.pixelWidth; ++x, src += 4, dst += 4) {
            const unsigned a = src[3];
            const unsigned inverse = 255u - a;
            dst[0] = static_cast<unsigned char>((src[0] * a + 255u * inverse + 127u) / 255u);
            dst[1] = static_cast<unsigned char>((src[1] * a + 255u * inverse + 127u) / 255u);
            dst[2] = static_cast<unsigned char>((src[2] * a + 255u * inverse + 127u) / 255u);
            dst[3] = 0;
        }
    }
    return out;
}

} // namespace

core::Result<PrintSettings> WindowsPrintService::choosePrintSettings(const PrintSetup& setup) {
    if (setup.pageCount == 0) {
        return std::unexpected(printError(core::ErrorCode::InvalidArgument, "the document has no pages to print"));
    }
    auto job = std::make_shared<WinPrintJob>();

    // Default printer first, to preset the orientation from the first page.
    PRINTDLGEXW defaults{};
    defaults.lStructSize = sizeof defaults;
    defaults.hwndOwner = owner_;
    defaults.Flags = PD_RETURNDEFAULT;
    defaults.nStartPage = START_PAGE_GENERAL;
    if (SUCCEEDED(::PrintDlgExW(&defaults))) {
        job->devMode = defaults.hDevMode;
        job->devNames = defaults.hDevNames;
    }
    if (job->devMode == nullptr) {
        return std::unexpected(printError(core::ErrorCode::NotAvailable, "No printer is installed."));
    }
    if (auto* devMode = static_cast<DEVMODEW*>(::GlobalLock(job->devMode)); devMode != nullptr) {
        const bool landscape = setup.firstPageSizePoints.width > setup.firstPageSizePoints.height;
        devMode->dmFields |= DM_ORIENTATION;
        devMode->dmOrientation = static_cast<short>(landscape ? DMORIENT_LANDSCAPE : DMORIENT_PORTRAIT);
        ::GlobalUnlock(job->devMode);
    }

    const auto lastPage = static_cast<DWORD>(std::min<std::size_t>(setup.pageCount, 0xFFFFFFFEu));
    PRINTPAGERANGE range{1, lastPage};
    PRINTDLGEXW dialog{};
    dialog.lStructSize = sizeof dialog;
    dialog.hwndOwner = owner_;
    dialog.hDevMode = job->devMode;
    dialog.hDevNames = job->devNames;
    dialog.Flags = PD_RETURNDC | PD_USEDEVMODECOPIESANDCOLLATE | PD_NOSELECTION | PD_NOCURRENTPAGE;
    dialog.nPageRanges = 1;
    dialog.nMaxPageRanges = 1;
    dialog.lpPageRanges = &range;
    dialog.nMinPage = 1;
    dialog.nMaxPage = lastPage;
    dialog.nCopies = 1;
    dialog.nStartPage = START_PAGE_GENERAL;
    HRESULT shown = E_FAIL;
    {
        const WindowsMainThreadDispatcher::ModalScope modal(dispatcher_);
        shown = ::PrintDlgExW(&dialog);
    }
    // The dialog may have replaced the handles (another printer).
    job->devMode = dialog.hDevMode;
    job->devNames = dialog.hDevNames;
    job->dc = dialog.hDC;
    if (FAILED(shown)) return std::unexpected(printError(core::ErrorCode::Internal, "the print dialog failed"));
    if (dialog.dwResultAction != PD_RESULT_PRINT) {
        return std::unexpected(printError(core::ErrorCode::Cancelled, "user dismissed the print dialog"));
    }
    if (job->dc == nullptr) {
        return std::unexpected(printError(core::ErrorCode::NotAvailable, "the printer could not be opened"));
    }

    PrintSettings settings;
    settings.jobTitle = setup.jobTitle;
    settings.firstPage = 0;
    settings.lastPage = setup.pageCount - 1;
    if ((dialog.Flags & PD_PAGENUMS) != 0 && dialog.nPageRanges >= 1) {
        const DWORD from = std::max<DWORD>(1, std::min(range.nFromPage, range.nToPage));
        const DWORD to = std::max(range.nFromPage, range.nToPage);
        settings.firstPage = std::min<std::size_t>(from - 1, setup.pageCount - 1);
        settings.lastPage = std::clamp<std::size_t>(to - 1, settings.firstPage, setup.pageCount - 1);
    }
    settings.platformHandle = std::static_pointer_cast<void>(job);
    return settings;
}

core::Status WindowsPrintService::printSpool(const PrintSettings& settings, const PrintSpoolDescription& spool) {
    const auto* job = static_cast<const WinPrintJob*>(settings.platformHandle.get());
    if (job == nullptr || job->magic != kJobMagic || job->dc == nullptr) {
        return std::unexpected(printError(core::ErrorCode::InvalidArgument, "print settings are not from Windows"));
    }
    if (spool.pages.empty()) return std::unexpected(printError(core::ErrorCode::InvalidArgument, "nothing to print"));
    HDC dc = job->dc;

    // Printer drivers (e.g. "Print to PDF") may show their own modal UI.
    const WindowsMainThreadDispatcher::ModalScope modal(dispatcher_);

    const std::wstring title = win::widen(settings.jobTitle.empty() ? std::string("Rivet") : settings.jobTitle);
    DOCINFOW document{};
    document.cbSize = sizeof document;
    document.lpszDocName = title.c_str();
    ::SetLastError(ERROR_SUCCESS);
    if (::StartDocW(dc, &document) <= 0) {
        if (::GetLastError() == ERROR_CANCELLED) {
            return std::unexpected(printError(core::ErrorCode::Cancelled, "the print job was cancelled"));
        }
        return std::unexpected(printError(core::ErrorCode::Io, "the printer refused the print job"));
    }

    const double printableWidth = ::GetDeviceCaps(dc, HORZRES);
    const double printableHeight = ::GetDeviceCaps(dc, VERTRES);
    core::Status status = core::ok();
    for (const PrintSpoolPage& page : spool.pages) {
        if (!(page.displaySizePoints.width > 0.0) || !(page.displaySizePoints.height > 0.0)) {
            status = std::unexpected(printError(core::ErrorCode::InvalidArgument, "invalid print page size"));
            break;
        }
        if (::StartPage(dc) <= 0) {
            status = std::unexpected(printError(core::ErrorCode::Io, "the printer refused a page"));
            break;
        }
        // Fit the page into the printable area, centered.
        const double scale = std::min(printableWidth / page.displaySizePoints.width,
                                      printableHeight / page.displaySizePoints.height);
        const double offsetX = (printableWidth - page.displaySizePoints.width * scale) / 2.0;
        const double offsetY = (printableHeight - page.displaySizePoints.height * scale) / 2.0;
        ::SetStretchBltMode(dc, HALFTONE);
        ::SetBrushOrgEx(dc, 0, 0, nullptr);
        for (const PrintSpoolBand& band : page.bands) {
            auto bytes = readBand(band);
            if (!bytes) {
                status = std::unexpected(bytes.error());
                break;
            }
            const std::vector<unsigned char> pixels = overWhite(*bytes, band);
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof info.bmiHeader;
            info.bmiHeader.biWidth = static_cast<LONG>(band.pixelWidth);
            info.bmiHeader.biHeight = -static_cast<LONG>(band.pixelHeight); // top-down rows
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            // Shared edges round identically, so bands tile without gaps.
            const auto x0 = static_cast<int>(std::lround(offsetX + band.rectPoints.minX() * scale));
            const auto x1 = static_cast<int>(std::lround(offsetX + band.rectPoints.maxX() * scale));
            const auto y0 = static_cast<int>(std::lround(offsetY + band.rectPoints.minY() * scale));
            const auto y1 = static_cast<int>(std::lround(offsetY + band.rectPoints.maxY() * scale));
            if (x1 <= x0 || y1 <= y0) continue;
            if (::StretchDIBits(dc, x0, y0, x1 - x0, y1 - y0, 0, 0, static_cast<int>(band.pixelWidth),
                                static_cast<int>(band.pixelHeight), pixels.data(), &info, DIB_RGB_COLORS,
                                SRCCOPY) == 0) {
                status = std::unexpected(printError(core::ErrorCode::Io, "cannot send a page band to the printer"));
                break;
            }
        }
        if (!status) break;
        if (::EndPage(dc) <= 0) {
            status = std::unexpected(printError(core::ErrorCode::Io, "the printer refused a page"));
            break;
        }
    }
    if (!status) {
        ::AbortDoc(dc);
        return status;
    }
    if (::EndDoc(dc) <= 0) return std::unexpected(printError(core::ErrorCode::Io, "the print job failed"));
    return core::ok();
}

} // namespace rivet::platform
