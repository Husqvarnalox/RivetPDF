# MSVC C runtime selection.
#
# A static PDFium built from Chromium's GN (is_component_build=false) uses the
# STATIC release CRT (/MT, tools/pdfium/args.common.gn). Mixing /MT objects
# with a /MD (or /MDd) Rivet fails at link time (LNK2038 RuntimeLibrary /
# _ITERATOR_DEBUG_LEVEL mismatch) or, worse, gives two heaps. Rivet therefore
# adopts the static CRT whenever it links PDFium on MSVC.
#
# RIVET_MSVC_STATIC_CRT: AUTO (default) = ON iff RIVET_WITH_PDFIUM,
#                        ON / OFF force it. OFF with a static PDFium will not link.
#
# The RELEASE static CRT (/MT) is used for every configuration, Debug
# included, because the PDFium archive is always a release build
# (is_debug=false); /MTd would not match it.
#
# Must be included before any target is created (CMP0091 NEW, which
# cmake_minimum_required(3.28) selects).
set(RIVET_MSVC_STATIC_CRT "AUTO" CACHE STRING "Use the static MSVC CRT (/MT): AUTO, ON or OFF")
set_property(CACHE RIVET_MSVC_STATIC_CRT PROPERTY STRINGS AUTO ON OFF)

if(MSVC)
    if(RIVET_MSVC_STATIC_CRT STREQUAL "ON"
       OR (RIVET_MSVC_STATIC_CRT STREQUAL "AUTO" AND RIVET_WITH_PDFIUM))
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
        message(STATUS "Rivet: MSVC static CRT (/MT) selected")
    endif()
endif()
