add_executable(rivet_core_tests
    TestAtomicFileWriter.cpp
    TestBitmap.cpp
    TestCheckedArithmetic.cpp
    TestError.cpp
    TestGeometry.cpp
    TestStrongId.cpp
)

add_test(NAME core COMMAND rivet_core_tests)

target_link_libraries(rivet_core_tests PRIVATE
    rivet::test_harness
    rivet::core
)

rivet_set_target_warnings(rivet_core_tests)
rivet_set_target_sanitizers(rivet_core_tests)

# Threading tests live in their own target, added by the async module.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/async/CMakeLists.txt")
    add_subdirectory(async)
endif()
