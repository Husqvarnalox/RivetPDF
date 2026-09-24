add_library(rivet_render STATIC
    PageLayout.cpp
    PageTransform.cpp
    TileCache.cpp
    ZoomState.cpp
)

add_library(rivet::render ALIAS rivet_render)

target_link_libraries(rivet_render PUBLIC rivet::core)

rivet_set_target_warnings(rivet_render)
rivet_set_target_sanitizers(rivet_render)
