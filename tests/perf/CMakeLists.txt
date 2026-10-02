# Opt-in perf probes (RIVET_BUILD_PERF_PROBES). Not registered with ctest.
if(RIVET_WITH_PDFIUM)
    add_executable(rivet_perf_page_editing PerfPageEditing.cpp)
    target_link_libraries(rivet_perf_page_editing PRIVATE rivet::editor rivet::pdfium)
    rivet_set_target_warnings(rivet_perf_page_editing)

    add_executable(rivet_perf_annotations PerfAnnotations.cpp)
    target_link_libraries(rivet_perf_annotations PRIVATE rivet::editor rivet::pdfium)
    target_compile_definitions(rivet_perf_annotations PRIVATE
        RIVET_PERF_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/pdf/fixtures")
    rivet_set_target_warnings(rivet_perf_annotations)
endif()

# Content editing (Phase 5): PDFium-free, runs over the editor fakes. The
# harness provides main().
add_executable(rivet_perf_content PerfContent.cpp)
target_link_libraries(rivet_perf_content PRIVATE rivet::editor rivet::test_harness)
target_include_directories(rivet_perf_content PRIVATE "${CMAKE_SOURCE_DIR}/tests/editor")
rivet_set_target_warnings(rivet_perf_content)
