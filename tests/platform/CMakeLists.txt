# Portable: header-only fakes of the platform shell services for app-layer
# tests, and the lifecycle plumbing tests that need no native backend.
add_library(rivet_platform_fakes INTERFACE)
add_library(rivet::platform_fakes ALIAS rivet_platform_fakes)
target_include_directories(rivet_platform_fakes INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rivet_platform_fakes INTERFACE rivet::platform)

add_executable(rivet_platform_portable_tests
    TestLifecycleHooks.cpp
)

add_test(NAME platform_portable COMMAND rivet_platform_portable_tests)

target_link_libraries(rivet_platform_portable_tests PRIVATE
    rivet::test_harness
    rivet::platform_fakes
)

rivet_set_target_warnings(rivet_platform_portable_tests)
rivet_set_target_sanitizers(rivet_platform_portable_tests)

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
