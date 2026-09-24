add_library(rivet_test_harness STATIC
    harness/TestMain.cpp
)

add_library(rivet::test_harness ALIAS rivet_test_harness)

target_include_directories(rivet_test_harness PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/harness)

rivet_set_target_warnings(rivet_test_harness)
rivet_set_target_sanitizers(rivet_test_harness)

# Test modules register their own subdirectories as they become available.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/core/CMakeLists.txt")
    add_subdirectory(core)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/render/CMakeLists.txt")
    add_subdirectory(render)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/pdf/CMakeLists.txt")
    add_subdirectory(pdf)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/editor/CMakeLists.txt")
    add_subdirectory(editor)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ui/CMakeLists.txt")
    add_subdirectory(ui)
endif()
