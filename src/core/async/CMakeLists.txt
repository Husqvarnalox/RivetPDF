# Async/threading subsystem: worker pool + per-stream serialization, compiled
# into the existing rivet_core target (no new library).
#
# This file is written to be includable more than once while the build tree is
# wired up incrementally (src/core/CMakeLists.txt does not add_subdirectory()
# it yet; tests/core/async registers it from there). The global-property guard
# makes repeated inclusion a no-op, so once src/core adds add_subdirectory(async)
# directly, everything keeps working unchanged.

get_property(rivet_async_sources_wired GLOBAL PROPERTY RIVET_ASYNC_CORE_SOURCES_WIRED)
if(rivet_async_sources_wired)
    return()
endif()
set_property(GLOBAL PROPERTY RIVET_ASYNC_CORE_SOURCES_WIRED ON)

target_sources(rivet_core PRIVATE
    SerialExecutor.cpp
    TaskScheduler.cpp
)
