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
#   <root>/lib/pdfium.lib         (Windows, STATIC lib, static CRT /MT)
#   <root>/PDFIUM_BUILD_INFO      (optional, written by tools/pdfium/*)
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
#   <root>/lib/pdfium.dll.lib     (Windows, import library for pdfium.dll)
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

# Provenance written by tools/pdfium/build-pdfium.{sh,ps1}: <root>/PDFIUM_BUILD_INFO.
# When present it is enforced, so a PDFium built with a JavaScript/XFA engine
# (or Skia) can never be linked by accident (ADR-0007, ADR-0036). Hand-built
# installs without the file are accepted unchecked.
set(PDFIUM_BUILD_INFO_FILE "")
if(PDFIUM_INCLUDE_DIR)
    foreach(_rivet_info "${RIVET_PDFIUM_ROOT}/PDFIUM_BUILD_INFO" "${PDFIUM_INCLUDE_DIR}/../PDFIUM_BUILD_INFO" "${PDFIUM_INCLUDE_DIR}/PDFIUM_BUILD_INFO")
        if(EXISTS "${_rivet_info}")
            set(PDFIUM_BUILD_INFO_FILE "${_rivet_info}")
            break()
        endif()
    endforeach()
endif()
if(PDFIUM_BUILD_INFO_FILE)
    file(STRINGS "${PDFIUM_BUILD_INFO_FILE}" _rivet_info_lines)
    foreach(_rivet_line IN LISTS _rivet_info_lines)
        if(_rivet_line MATCHES "^([a-z_0-9]+)=(.*)$")
            set(PDFIUM_INFO_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
        endif()
    endforeach()
    foreach(_rivet_flag pdf_enable_v8 pdf_enable_xfa pdf_use_skia)
        if(DEFINED PDFIUM_INFO_${_rivet_flag} AND NOT "${PDFIUM_INFO_${_rivet_flag}}" STREQUAL "false")
            message(FATAL_ERROR
                "PDFium at ${PDFIUM_INCLUDE_DIR} was built with ${_rivet_flag}=${PDFIUM_INFO_${_rivet_flag}}; "
                "Rivet requires false (docs/BUILDING_PDFIUM.md, ADR-0007).")
        endif()
    endforeach()
    message(STATUS "PDFium build info: revision ${PDFIUM_INFO_revision} "
                   "(${PDFIUM_INFO_target_os}/${PDFIUM_INFO_target_cpu})")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(PDFium
    REQUIRED_VARS PDFIUM_LIBRARY PDFIUM_INCLUDE_DIR
    FAIL_MESSAGE "PDFium was not found. Set -D RIVET_PDFIUM_ROOT=<path> to a local PDFium build/install. See docs/BUILDING_PDFIUM.md.")

if(PDFIUM_FOUND AND NOT TARGET PDFium::PDFium)
    add_library(PDFium::PDFium UNKNOWN IMPORTED)
    set_target_properties(PDFium::PDFium PROPERTIES
        IMPORTED_LOCATION "${PDFIUM_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${PDFIUM_INCLUDE_DIR}"
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${PDFIUM_INCLUDE_DIR}")

    # A STATIC PDFium is self-contained for PDFium's own code, but the consumer
    # must link the system libraries PDFium's sources use directly. Linking
    # them for a shared build is harmless (it already carries its own list).
    #
    # macOS: determined empirically against a real static build
    # (pdf_is_complete_lib=true, revision 40ecb4f4): CoreFoundation,
    # CoreGraphics, CoreText and Security (the last via partition_alloc).
    # Linux / Windows: the sets below are derived from PDFium's platform code
    # (fx_linux_impl/dlopen, partition_alloc, GDI/COM font + clipboard-free
    # win32 paths) and over-approximate on purpose; they have NOT yet been
    # verified against a real Linux/Windows link (see ADR-0036). If a link
    # reports an unresolved symbol, add its library here.
    if(APPLE)
        set_target_properties(PDFium::PDFium PROPERTIES
            INTERFACE_LINK_LIBRARIES
                "-framework CoreFoundation;-framework CoreGraphics;-framework CoreText;-framework Security")
    elseif(WIN32)
        set_target_properties(PDFium::PDFium PROPERTIES
            INTERFACE_LINK_LIBRARIES
                "advapi32;bcrypt;gdi32;ole32;oleaut32;shell32;user32;winmm")
    elseif(UNIX)
        # pthread comes from Rivet's Threads::Threads.
        set_target_properties(PDFium::PDFium PROPERTIES
            INTERFACE_LINK_LIBRARIES "dl;m")
    endif()
endif()

mark_as_advanced(PDFIUM_INCLUDE_DIR PDFIUM_LIBRARY)
