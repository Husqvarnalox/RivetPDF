add_library(rivet_platform_macos STATIC
    MacosFileDialog.mm
    MacosMainThreadDispatcher.mm
    MacosPaintContext.mm
    RivetContentView.mm
)

add_library(rivet::platform_macos ALIAS rivet_platform_macos)

target_link_libraries(rivet_platform_macos
    PUBLIC rivet::platform rivet::ui rivet::core
    PRIVATE rivet::app
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
target_link_libraries(rivet PRIVATE
    rivet_platform_macos rivet::app rivet::ui rivet::editor rivet::pdf rivet::render rivet::core
)
if(RIVET_WITH_PDFIUM)
    target_link_libraries(rivet PRIVATE rivet::pdfium)
endif()
rivet_set_target_warnings(rivet)
rivet_set_target_sanitizers(rivet)
