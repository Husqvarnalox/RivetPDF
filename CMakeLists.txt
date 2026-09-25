cmake_minimum_required(VERSION 3.28)

project(rivet
    VERSION 0.1.0
    DESCRIPTION "Rivet - a fast, native, open-source PDF editor"
    LANGUAGES CXX)

# Objective-C++ exists only for the macOS platform backend; Windows/Linux
# builds must not require it. Non-Apple platforms configure the portable
# libraries and tests, but no desktop executable yet (src/platform/CMakeLists.txt).
if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 23)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(CMAKE_OBJCXX_EXTENSIONS OFF)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(RIVET_WITH_PDFIUM "Build with the PDFium-backed PDF engine" OFF)
option(RIVET_BUILD_TESTS "Build Rivet tests" ON)
option(RIVET_ENABLE_SANITIZERS "Build with AddressSanitizer and UBSan" OFF)

find_package(Threads REQUIRED)

if(RIVET_WITH_PDFIUM)
    find_package(PDFium QUIET)
    if(NOT PDFIUM_FOUND)
        message(FATAL_ERROR
            "RIVET_WITH_PDFIUM is ON but PDFium was not found.\n"
            "Rivet never downloads PDFium automatically. Build PDFium locally\n"
            "(see docs/BUILDING_PDFIUM.md) and provide it via:\n"
            "    -D RIVET_PDFIUM_ROOT=/absolute/path/to/pdfium-install\n"
            "Expected layout:\n"
            "    <root>/include/fpdfview.h   (plus fpdf_*.h headers)\n"
            "    <root>/lib/libpdfium.a      (macOS/Linux static; .dylib/.so also accepted)")
    endif()
    message(STATUS "Rivet: PDFium found: ${PDFIUM_LIBRARY}")
else()
    message(STATUS "Rivet: building WITHOUT PDFium (RIVET_WITH_PDFIUM=OFF); PDF features are disabled at runtime.")
endif()

include(RivetWarnings)
include(RivetSanitizers)

# Subsystems register themselves when their CMakeLists.txt exists. This lets
# the build tree grow incrementally during development; once all subsystems
# are present every directory is always added.
macro(rivet_add_subdirectory dir)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/CMakeLists.txt")
        add_subdirectory(${dir})
    else()
        message(STATUS "Rivet: subsystem '${dir}' not present yet, skipping")
    endif()
endmacro()

rivet_add_subdirectory(src/core)
rivet_add_subdirectory(src/render)
rivet_add_subdirectory(src/pdf)
rivet_add_subdirectory(src/editor)
rivet_add_subdirectory(src/ui)
rivet_add_subdirectory(src/platform)
rivet_add_subdirectory(src/app)

if(RIVET_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
