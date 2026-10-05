add_executable(rivet_markdown_tests
    TestMarkdownLayout.cpp
    TestMarkdownParser.cpp
    TestMarkdownSlug.cpp
    TestMarkdownSourceMap.cpp
)

add_test(NAME markdown COMMAND rivet_markdown_tests)

target_link_libraries(rivet_markdown_tests PRIVATE
    rivet::test_harness
    rivet::markdown
)
target_compile_definitions(rivet_markdown_tests PRIVATE
    RIVET_MD_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/markdown")

rivet_set_target_warnings(rivet_markdown_tests)
rivet_set_target_sanitizers(rivet_markdown_tests)

# Opt-in timing probe (RIVET_BUILD_PERF_PROBES), further gated at run time by
# RIVET_PERF_MARKDOWN=1. Prints timings only. Not registered with ctest.
if(RIVET_BUILD_PERF_PROBES)
    add_executable(rivet_perf_markdown PerfMarkdown.cpp)
    target_link_libraries(rivet_perf_markdown PRIVATE rivet::app)
    rivet_set_target_warnings(rivet_perf_markdown)
endif()
