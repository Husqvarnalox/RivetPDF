add_executable(rivet_pdf_tests
    TestPdfSystem.cpp
)

add_test(NAME pdf COMMAND rivet_pdf_tests)

target_link_libraries(rivet_pdf_tests PRIVATE rivet::test_harness rivet::pdf rivet::core)

# In PDFium builds rivet::pdf is interface-only and createEngine() lives in
# rivet_pdfium, so the test binary must link that as well (the null build gets
# createEngine() from rivet::pdf itself).
if(RIVET_WITH_PDFIUM)
    target_link_libraries(rivet_pdf_tests PRIVATE rivet::pdfium)
endif()

# Fixture paths are resolved at build time so the test binary works from any
# working directory (ctest runs it from the build tree).
target_compile_definitions(rivet_pdf_tests PRIVATE
    RIVET_PDF_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures"
)

rivet_set_target_warnings(rivet_pdf_tests)
rivet_set_target_sanitizers(rivet_pdf_tests)
