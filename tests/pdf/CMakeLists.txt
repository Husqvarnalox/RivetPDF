add_executable(rivet_pdf_tests
    TestPdfSystem.cpp
    TestPdfiumCallGate.cpp
    # Rendering-contract tests against the geometry fixtures in
    # tests/pdf/fixtures/ (orientation, tiling, error paths). Bodies skip
    # gracefully when the engine is unavailable.
    TestPdfiumRender.cpp
    # Compiled directly into the test binary rather than linked from
    # rivet_pdfium, so the gate tests run with RIVET_WITH_PDFIUM=OFF too. The
    # gate translation unit has no FPDF_* dependency. In PDFium-ON builds the
    # linker resolves globalPdfiumCallGate() from this object and never pulls
    # the duplicate archive member from rivet_pdfium.
    "${CMAKE_SOURCE_DIR}/src/pdf/pdfium/PdfiumCallGate.cpp"
)

add_test(NAME pdf COMMAND rivet_pdf_tests)

# rivet::core arrives transitively via rivet::pdf in both build modes
# (static null backend, or interface + rivet::pdfium); listing it directly
# too makes the macOS linker see librivet_core.a twice.
target_link_libraries(rivet_pdf_tests PRIVATE rivet::test_harness rivet::pdf)

# Header path for the directly-compiled gate sources.
target_include_directories(rivet_pdf_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/pdf/pdfium")

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
    RIVET_PDF_RENDER_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/pdf/fixtures"
)

rivet_set_target_warnings(rivet_pdf_tests)
rivet_set_target_sanitizers(rivet_pdf_tests)
