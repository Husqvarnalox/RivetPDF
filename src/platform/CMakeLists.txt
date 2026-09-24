add_library(rivet_platform INTERFACE)

add_library(rivet::platform ALIAS rivet_platform)

target_include_directories(rivet_platform INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(rivet_platform INTERFACE rivet::core)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/macos/CMakeLists.txt")
    add_subdirectory(macos)
endif()
