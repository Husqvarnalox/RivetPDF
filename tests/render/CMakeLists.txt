add_executable(rivet_render_tests
    TestPageLayout.cpp
    TestPageTransform.cpp
    TestPhysicalRenderScaleKey.cpp
    TestRenderScaleKey.cpp
    TestTileCache.cpp
    TestZoomState.cpp
)

add_test(NAME render COMMAND rivet_render_tests)

target_link_libraries(rivet_render_tests PRIVATE
    rivet::test_harness
    rivet::render
)

rivet_set_target_warnings(rivet_render_tests)
rivet_set_target_sanitizers(rivet_render_tests)
