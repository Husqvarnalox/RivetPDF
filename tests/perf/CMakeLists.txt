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

    # Content editing through the real engine (Phase 5).
    add_executable(rivet_perf_content_pdfium PerfContentPdfium.cpp)
    target_link_libraries(rivet_perf_content_pdfium PRIVATE rivet::editor rivet::pdfium)
    target_include_directories(rivet_perf_content_pdfium PRIVATE "${CMAKE_SOURCE_DIR}/tests/pdf")
    rivet_set_target_warnings(rivet_perf_content_pdfium)

    # Content editing over real PDFs supplied at run time (command line).
    add_executable(rivet_probe_content_corpus ProbeContentCorpus.cpp)
    target_link_libraries(rivet_probe_content_corpus PRIVATE rivet::editor rivet::pdfium)
    rivet_set_target_warnings(rivet_probe_content_corpus)
    rivet_set_target_sanitizers(rivet_probe_content_corpus)
endif()

# Content editing (Phase 5): PDFium-free, runs over the editor fakes. The
# harness provides main().
add_executable(rivet_perf_content PerfContent.cpp)
target_link_libraries(rivet_perf_content PRIVATE rivet::editor rivet::test_harness)
target_include_directories(rivet_perf_content PRIVATE "${CMAKE_SOURCE_DIR}/tests/editor")
rivet_set_target_warnings(rivet_perf_content)
