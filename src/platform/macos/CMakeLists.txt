add_library(rivet_platform_macos STATIC
    MacosFileDialog.mm
    MacosMainThreadDispatcher.mm
    MacosPaintContext.mm
    RivetContentView.mm
)

add_library(rivet::platform_macos ALIAS rivet_platform_macos)

# PUBLIC deps mirror real header exposure:
#   - rivet::platform  via MacosFileDialog.h -> platform/PlatformKit.hpp
#   - rivet::ui        via RivetContentView.h -> ui/Widget.hpp,
#                        MacosPaintContext.h -> ui/PaintContext.hpp, ui/UiTypes.hpp
#   - rivet::core      via MacosMainThreadDispatcher.h -> core/async/IMainThreadDispatcher.hpp,
#                        MacosPaintContext.h -> core/Bitmap.hpp, core/geometry/*.hpp
# rivet::app is deliberately ABSENT: the platform backend must never depend on
# the application layer. The direction is app -> platform; the executable is
# the composition root and may depend on both.
target_link_libraries(rivet_platform_macos
    PUBLIC rivet::platform rivet::ui rivet::core
)

target_compile_options(rivet_platform_macos PRIVATE -fobjc-arc)

find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
find_library(CORETEXT_FRAMEWORK CoreText REQUIRED)
target_link_libraries(rivet_platform_macos PUBLIC "${COCOA_FRAMEWORK}" "${CORETEXT_FRAMEWORK}")
find_library(UTI_FRAMEWORK UniformTypeIdentifiers)
if(UTI_FRAMEWORK)
    target_link_libraries(rivet_platform_macos PUBLIC "${UTI_FRAMEWORK}")
endif()

rivet_set_target_warnings(rivet_platform_macos)
rivet_set_target_sanitizers(rivet_platform_macos)

add_executable(rivet main.mm)
# main.mm is ObjC++ like the rest of the backend and shares the ARC setting.
target_compile_options(rivet PRIVATE -fobjc-arc)
# The executable is the composition root: it links the platform backend and
# the application layer only. Everything else (ui/editor/pdf/render/core/
# platform) arrives transitively through their PUBLIC interfaces. main.mm
# (app/ShellController.hpp etc.) gets its include paths from rivet::app.
target_link_libraries(rivet PRIVATE
    rivet_platform_macos
    rivet::app
)
if(RIVET_WITH_PDFIUM)
    target_link_libraries(rivet PRIVATE rivet::pdfium)
endif()
rivet_set_target_warnings(rivet)
rivet_set_target_sanitizers(rivet)
