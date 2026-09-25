# FindPDFium.cmake
#
# Locates an explicitly provided PDFium build/installation and creates the
# imported target `PDFium::PDFium`.
#
# Rivet NEVER downloads PDFium. The caller must build or install PDFium
# locally (see docs/BUILDING_PDFIUM.md) and point Rivet at it:
#
#   cmake -S . -B build/pdfium -G Ninja -D RIVET_WITH_PDFIUM=ON \
#         -D RIVET_PDFIUM_ROOT=/absolute/path/to/pdfium-install
#
# RIVET_PDFIUM_ROOT may also be provided through the environment variable
# RIVET_PDFIUM_ROOT.
#
# Preferred layout inside RIVET_PDFIUM_ROOT (STATIC library):
#
#   <root>/include/fpdfview.h     (plus the other fpdf_*.h headers)
#   <root>/lib/libpdfium.a        (macOS/Linux, static)
#
# A static, complete-library PDFium is produced by GN args
#   is_component_build=false pdf_is_complete_lib=true
# which yields a single self-contained libpdfium.a (out/<dir>/libpdfium.a).
# Copy or symlink it into <root>/lib/ and the public headers (from
# pdfium/public/) into <root>/include/ to obtain the layout above, or point
# RIVET_PDFIUM_ROOT directly at a directory with include/ and lib/.
#
# Shared libraries are still accepted (find_library matches libpdfium.a,
# libpdfium.dylib / libpdfium.so for the name `pdfium`):
#   <root>/lib/libpdfium.dylib    (macOS)
#   <root>/lib/libpdfium.so       (Linux)
#   <root>/lib/pdfium.lib         (Windows, import library for pdfium.dll)
# Static is preferred: the search hints try <root>/lib before <root>, and the
# docs describe the static build as the recommended configuration.
#
# Result variables:
#   PDFium_FOUND        - PDFium was located
#   PDFIUM_INCLUDE_DIR  - directory containing fpdfview.h
#   PDFIUM_LIBRARY      - full path to the PDFium library
#
# Result targets:
#   PDFium::PDFium      - imported (UNKNOWN) library target

if(NOT PDFIUM_INCLUDE_DIR)
    if(RIVET_PDFIUM_ROOT AND EXISTS "${RIVET_PDFIUM_ROOT}")
        find_path(PDFIUM_INCLUDE_DIR
            NAMES fpdfview.h
            HINTS
                "${RIVET_PDFIUM_ROOT}/include"
                "${RIVET_PDFIUM_ROOT}"
            NO_DEFAULT_PATH)
        find_library(PDFIUM_LIBRARY
            NAMES pdfium libpdfium
            HINTS
                "${RIVET_PDFIUM_ROOT}/lib"
                "${RIVET_PDFIUM_ROOT}"
                "${RIVET_PDFIUM_ROOT}/out"
            NO_DEFAULT_PATH)
    endif()

    if(NOT PDFIUM_INCLUDE_DIR OR NOT PDFIUM_LIBRARY)
        # Allow standard system search paths as a last resort.
        find_path(PDFIUM_INCLUDE_DIR NAMES fpdfview.h)
        find_library(PDFIUM_LIBRARY NAMES pdfium libpdfium)
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(PDFium
    REQUIRED_VARS PDFIUM_LIBRARY PDFIUM_INCLUDE_DIR
    FAIL_MESSAGE "PDFium was not found. Set -D RIVET_PDFIUM_ROOT=<path> to a local PDFium build/install. See docs/BUILDING_PDFIUM.md.")

if(PDFIUM_FOUND AND NOT TARGET PDFium::PDFium)
    add_library(PDFium::PDFium UNKNOWN IMPORTED)
    set_target_properties(PDFium::PDFium PROPERTIES
        IMPORTED_LOCATION "${PDFIUM_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${PDFIUM_INCLUDE_DIR}")

    # A STATIC libpdfium.a is self-contained for PDFium's own code, but the
    # consumer must link the system libraries PDFium's sources use directly.
    # This set was determined empirically against a macOS static build
    # (pdf_is_complete_lib=true, revision 40ecb4f4): CoreFoundation,
    # CoreGraphics, CoreText and Security (the last one via partition_alloc).
    # Linking them for a shared dylib is harmless (it already carries its own
    # dependency list).
    if(APPLE)
        set_target_properties(PDFium::PDFium PROPERTIES
            INTERFACE_LINK_LIBRARIES
                "-framework CoreFoundation;-framework CoreGraphics;-framework CoreText;-framework Security")
    elseif(UNIX)
        # TODO(linux-backend): verify the exact set against a real Linux
        # static build; pthread is already provided by Rivet's Threads::Threads.
        set_target_properties(PDFium::PDFium PROPERTIES
            INTERFACE_LINK_LIBRARIES "dl")
    endif()
endif()

mark_as_advanced(PDFIUM_INCLUDE_DIR PDFIUM_LIBRARY)
