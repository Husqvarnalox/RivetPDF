add_library(rivet_core STATIC
    Bitmap.cpp
    Error.cpp
    Log.cpp
    Time.cpp
    io/AtomicFileWriter.cpp
)

add_subdirectory(async)

add_library(rivet::core ALIAS rivet_core)

target_include_directories(rivet_core PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(rivet_core PUBLIC Threads::Threads)

rivet_set_target_warnings(rivet_core)
rivet_set_target_sanitizers(rivet_core)
