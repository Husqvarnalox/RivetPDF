# Wire the async sources into rivet_core. src/core/CMakeLists.txt does not
# add_subdirectory(async) yet, so the module registers itself from here; the
# module's global-property guard makes this idempotent once src/core wires it
# directly. Adding the subdirectory (rather than listing the .cpp files here)
# keeps rivet_core visible to target_sources per CMake's directory rules.
add_subdirectory("${CMAKE_SOURCE_DIR}/src/core/async" rivet_async_module)

add_executable(rivet_async_tests
    TestSerialExecutor.cpp
    TestTaskScheduler.cpp
)

add_test(NAME async COMMAND rivet_async_tests)

target_link_libraries(rivet_async_tests PRIVATE
    rivet::test_harness
    rivet::core
)

rivet_set_target_warnings(rivet_async_tests)
rivet_set_target_sanitizers(rivet_async_tests)
