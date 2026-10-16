# Linux / other non-Apple UNIX desktop backend: X11 (Xlib) windows, cairo
# drawing, fontconfig + HarfBuzz + cairo-ft text, libpng/libjpeg image
# decoding, optional CUPS printing. Runs on Wayland desktops via XWayland.
#
# These system libraries are used ONLY inside this directory; the portable
# layers never see them. When the development packages are missing the
# portable libraries and tests still configure and build - only the `rivet`
# executable is skipped (CI and headless builders must not break).

find_package(PkgConfig QUIET)
if(NOT PKG_CONFIG_FOUND)
    message(STATUS "Rivet: pkg-config not found; skipping the Linux desktop executable")
    return()
endif()

pkg_check_modules(RIVET_LINUX_GUI QUIET IMPORTED_TARGET
    x11 cairo cairo-xlib cairo-ft cairo-pdf fontconfig freetype2 harfbuzz libpng)
pkg_check_modules(RIVET_LINUX_JPEG QUIET IMPORTED_TARGET libjpeg)
set(_rivet_jpeg_target "")
if(RIVET_LINUX_JPEG_FOUND)
    set(_rivet_jpeg_target PkgConfig::RIVET_LINUX_JPEG)
else()
    find_package(JPEG QUIET)
    if(JPEG_FOUND)
        set(_rivet_jpeg_target JPEG::JPEG)
    endif()
endif()

if(NOT RIVET_LINUX_GUI_FOUND OR _rivet_jpeg_target STREQUAL "")
    message(STATUS "Rivet: X11/cairo/fontconfig/freetype/harfbuzz/libpng/libjpeg development packages "
                   "not found; skipping the Linux desktop executable (see docs/BUILDING.md)")
    return()
endif()

add_library(rivet_platform_linux STATIC
    LinuxAlertService.cpp
    LinuxAppDataLocation.cpp
    LinuxClipboard.cpp
    LinuxExternalUrlOpener.cpp
    LinuxFileDialog.cpp
    LinuxImageDecoder.cpp
    LinuxMainThreadDispatcher.cpp
    LinuxMenuBar.cpp
    LinuxPaintContext.cpp
    LinuxSubprocess.cpp
    LinuxTextShaper.cpp
    X11Window.cpp
)

add_library(rivet::platform_linux ALIAS rivet_platform_linux)

# PUBLIC deps mirror real header exposure (platform/*.hpp, ui/Widget.hpp,
# ui/PaintContext.hpp, core); cairo appears in LinuxPaintContext.hpp.
# rivet::app is deliberately ABSENT: the backend never depends on the
# application layer; the executable is the composition root.
target_link_libraries(rivet_platform_linux
    PUBLIC rivet::platform rivet::ui rivet::core PkgConfig::RIVET_LINUX_GUI
    PRIVATE ${_rivet_jpeg_target} Threads::Threads
)

find_package(Cups QUIET)
if(Cups_FOUND OR CUPS_FOUND)
    target_sources(rivet_platform_linux PRIVATE LinuxPrintService.cpp)
    target_link_libraries(rivet_platform_linux PRIVATE Cups::Cups)
    target_compile_definitions(rivet_platform_linux PUBLIC RIVET_HAVE_CUPS=1)
    message(STATUS "Rivet: Linux printing via CUPS enabled")
else()
    target_compile_definitions(rivet_platform_linux PUBLIC RIVET_HAVE_CUPS=0)
    message(STATUS "Rivet: libcups not found; the Linux build has no printing (printService = null)")
endif()

rivet_set_target_warnings(rivet_platform_linux)
rivet_set_target_sanitizers(rivet_platform_linux)

add_executable(rivet main.cpp)
# Composition root: the platform backend plus the application layer;
# everything else arrives transitively through their PUBLIC interfaces.
target_link_libraries(rivet PRIVATE
    rivet_platform_linux
    rivet::app
)
if(RIVET_WITH_PDFIUM)
    target_link_libraries(rivet PRIVATE rivet::pdfium)
endif()
rivet_set_target_warnings(rivet)
rivet_set_target_sanitizers(rivet)
