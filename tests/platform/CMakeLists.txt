# macOS-only: the clipboard and print tests drive the AppKit-backed implementations.
if(APPLE)
    add_executable(rivet_platform_tests
        TestClipboard.cpp
        TestPrintService.mm
    )

    add_test(NAME platform COMMAND rivet_platform_tests)

    target_link_libraries(rivet_platform_tests PRIVATE
        rivet::test_harness
        rivet::platform_macos
    )
    target_compile_options(rivet_platform_tests PRIVATE -fobjc-arc)

    rivet_set_target_warnings(rivet_platform_tests)
    rivet_set_target_sanitizers(rivet_platform_tests)
endif()
