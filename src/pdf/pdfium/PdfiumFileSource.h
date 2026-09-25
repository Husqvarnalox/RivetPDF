// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Error.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>

// PDFium public headers. Allowed here only: this file lives inside
// src/pdf/pdfium/, the single directory where FPDF_* types may appear.
#include "fpdfview.h"

namespace rivet::pdf {

// A Rivet-owned, read-only, shared view of one PDF file's bytes, fed to
// PDFium through FPDF_LoadCustomDocument.
//
// The source holds an OPEN file descriptor (plus the size observed at open
// time) for its whole lifetime. Every FPDF_DOCUMENT loaded from it - the
// live PdfiumDocument and any working copy an assembly reopens later - reads
// through that descriptor, so they all see the SAME bytes even if the path
// on disk is atomically replaced (POSIX rename semantics keep the open
// inode alive; a save that writes a temp file and renames it over the
// original therefore never pulls the rug from under the live document).
// Truncating the file IN PLACE would still be visible; reads past the
// observed size or short reads fail cleanly instead of returning garbage.
//
// POSIX-only (open/fstat/pread): a Windows port needs a ReadFile-based
// equivalent. Shared via std::shared_ptr; the owner of every FPDF_DOCUMENT
// loaded from a source must keep the source alive until that document is
// closed (PDFium calls back into it lazily while parsing).
//
// Threading: readBlock is only ever called by PDFium, i.e. under the global
// PDFium call gate; pread has no shared file offset, so even concurrent
// reads would be safe.
class PdfiumFileSource {
public:
    // Opens `path` read-only. Io for unopenable/non-regular files,
    // InvalidDocument for an empty file (nothing PDFium could parse).
    static core::Result<std::shared_ptr<PdfiumFileSource>> open(const std::filesystem::path& path);

    ~PdfiumFileSource();

    PdfiumFileSource(const PdfiumFileSource&) = delete;
    PdfiumFileSource& operator=(const PdfiumFileSource&) = delete;

    std::uint64_t size() const { return size_; }

    // A file-access descriptor for FPDF_LoadCustomDocument whose m_Param
    // points at this source. PDFium copies the struct; the source itself
    // must outlive the loaded document.
    FPDF_FILEACCESS fileAccess() const;

private:
    PdfiumFileSource(int fd, std::uint64_t size) : fd_(fd), size_(size) {}

    // FPDF_FILEACCESS::m_GetBlock. Returns non-zero on success; never throws
    // (it is called from inside PDFium).
    static int readBlock(void* param, unsigned long position, unsigned char* buffer, unsigned long size);

    int fd_ = -1;
    std::uint64_t size_ = 0;
};

} // namespace rivet::pdf
