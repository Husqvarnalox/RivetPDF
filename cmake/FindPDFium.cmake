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
# Expected layout inside RIVET_PDFIUM_ROOT:
#
#   <root>/include/fpdfview.h     (plus the other fpdf_*.h headers)
#   <root>/lib/libpdfium.dylib    (macOS)
#   <root>/lib/libpdfium.so       (Linux)
#   <root>/lib/pdfium.lib         (Windows, import library for pdfium.dll)
#
# The tree produced by `autoninja pdfium` inside a PDFium checkout (with
# is_component_build=false) contains out/<dir>/pdfium.dylib (or .so / .dll);
# copy or symlink the produced dylib into <root>/lib/ and the public headers
# (from pdfium/public/) into <root>/include/ to obtain the layout above, or
# point RIVET_PDFIUM_ROOT directly at a directory with include/ and lib/.
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
endif()

mark_as_advanced(PDFIUM_INCLUDE_DIR PDFIUM_LIBRARY)
