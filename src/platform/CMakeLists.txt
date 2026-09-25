add_library(rivet_platform INTERFACE)

add_library(rivet::platform ALIAS rivet_platform)

target_include_directories(rivet_platform INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(rivet_platform INTERFACE rivet::core)

# Desktop backends are selected by target platform, not by directory
# existence. Unsupported platforms still configure the portable libraries
# and tests above; they simply get no desktop executable until a backend
# is implemented (no fake/stub implementations).
if(APPLE)
    add_subdirectory(macos)
endif()

# TODO(backend): Windows backend. Uncomment when src/platform/windows exists.
# if(WIN32)
#     add_subdirectory(windows)
# endif()

# TODO(backend): Linux/other UNIX backend. Uncomment when src/platform/unix exists.
# if(UNIX AND NOT APPLE)
#     add_subdirectory(unix)
# endif()
