add_executable(rivet_platform_tests
    TestClipboard.cpp
)

add_test(NAME platform COMMAND rivet_platform_tests)

# macOS-only: the clipboard implementation lives in the AppKit backend.
if(APPLE)
    target_link_libraries(rivet_platform_tests PRIVATE
        rivet::test_harness
        rivet::platform_macos
    )
    target_compile_options(rivet_platform_tests PRIVATE -fobjc-arc)
endif()

rivet_set_target_warnings(rivet_platform_tests)
rivet_set_target_sanitizers(rivet_platform_tests)
