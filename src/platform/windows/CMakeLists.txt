# Windows desktop backend: Win32 windows and menus, Direct2D drawing,
# DirectWrite text, WIC image decoding, common item dialogs, TaskDialog
# alerts, Win32 clipboard, ShellExecute URLs and GDI printing. Windows 10/11,
# x64 and ARM64; MSVC or clang/MinGW.
#
# These system APIs are used ONLY inside this directory (plus the plain
# Win32 file APIs of core/io/AtomicFileWriter); the portable layers never
# see them.

enable_language(RC)

add_library(rivet_platform_windows STATIC
    WinCommon.cpp
    WindowsAlertService.cpp
    WindowsClipboard.cpp
    WindowsExternalUrlOpener.cpp
    WindowsFileDialog.cpp
    WindowsImageDecoder.cpp
    WindowsMainThreadDispatcher.cpp
    WindowsMenuBar.cpp
    WindowsPaintContext.cpp
    WindowsPrintService.cpp
    WindowsTextShaper.cpp
    WindowsWindow.cpp
)

add_library(rivet::platform_windows ALIAS rivet_platform_windows)

# Windows 10 API level (GetDpiForWindow, AdjustWindowRectExForDpi, ...).
target_compile_definitions(rivet_platform_windows PUBLIC
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)

# PUBLIC deps mirror real header exposure (platform/*.hpp, ui/Widget.hpp,
# ui/PaintContext.hpp, core). rivet::app is deliberately ABSENT: the backend
# never depends on the application layer; the executable is the composition
# root.
target_link_libraries(rivet_platform_windows
    PUBLIC rivet::platform rivet::ui rivet::core
    PRIVATE d2d1 dwrite windowscodecs ole32 uuid comdlg32 shell32 user32 gdi32 dwmapi advapi32
            Threads::Threads
)

rivet_set_target_warnings(rivet_platform_windows)
rivet_set_target_sanitizers(rivet_platform_windows)

# Resources: application manifest (per-monitor-v2 DPI, common controls v6,
# UTF-8 active code page) and version info.
set(RIVET_WINDOWS_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/resources/rivet.manifest")
configure_file(resources/rivet.rc.in "${CMAKE_CURRENT_BINARY_DIR}/rivet.rc" @ONLY)

# A separate object library keeps the C++ warning options away from the
# resource compiler.
add_library(rivet_windows_resources OBJECT "${CMAKE_CURRENT_BINARY_DIR}/rivet.rc")
if(MSVC)
    target_compile_definitions(rivet_windows_resources PRIVATE RIVET_RC_NO_MANIFEST)
endif()

add_executable(rivet WIN32 main.cpp $<TARGET_OBJECTS:rivet_windows_resources>)
if(MSVC)
    # The MSVC linker merges and embeds .manifest sources itself.
    target_sources(rivet PRIVATE "${RIVET_WINDOWS_MANIFEST}")
endif()
# Composition root: the platform backend plus the application layer;
# everything else arrives transitively through their PUBLIC interfaces.
target_link_libraries(rivet PRIVATE
    rivet_platform_windows
    rivet::app
    ole32 shell32 user32
)
if(RIVET_WITH_PDFIUM)
    target_link_libraries(rivet PRIVATE rivet::pdfium)
endif()
rivet_set_target_warnings(rivet)
rivet_set_target_sanitizers(rivet)
