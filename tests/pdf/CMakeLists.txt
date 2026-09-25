add_executable(rivet_pdf_tests
    TestPdfSystem.cpp
    TestPdfiumCallGate.cpp
    # Rendering-contract tests against the geometry fixtures in
    # tests/pdf/fixtures/ (orientation, tiling, error paths). Bodies skip
    # gracefully when the engine is unavailable.
    TestPdfiumRender.cpp
    # PDFium-free text model tests (folding, search, hit testing): run in
    # both build modes.
    TestPdfText.cpp
    # Text-extraction tests against the text fixtures: PDFium-ON bodies only
    # (same skip-when-unavailable pattern).
    TestPdfiumText.cpp
    # Document-navigation tests (outline, page labels, links): PDFium-ON
    # bodies only.
    TestPdfNavigation.cpp
    # Engine-independent page-view geometry: runs in both build modes.
    TestPdfPageGeometry.cpp
    # View-aware reads and document assembly round trips (page editing
    # foundation): PDFium-ON bodies only.
    TestPdfPageEditing.cpp
    # Compiled directly into the test binary rather than linked from
    # rivet_pdfium, so the gate tests run with RIVET_WITH_PDFIUM=OFF too. The
    # gate translation unit has no FPDF_* dependency. In PDFium-ON builds the
    # linker resolves globalPdfiumCallGate() from this object and never pulls
    # the duplicate archive member from rivet_pdfium.
    "${CMAKE_SOURCE_DIR}/src/pdf/pdfium/PdfiumCallGate.cpp"
)

add_test(NAME pdf COMMAND rivet_pdf_tests)

target_link_libraries(rivet_pdf_tests PRIVATE rivet::test_harness)

# Header path for the directly-compiled gate sources.
target_include_directories(rivet_pdf_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/pdf/pdfium")

# rivet::core arrives transitively via rivet::pdf in both build modes. In
# PDFium builds rivet::pdf also arrives via rivet::pdfium (which links it
# PUBLIC), so listing rivet::pdf here as well would put librivet_pdf.a on the
# link line twice and the macOS linker warns about duplicate libraries.
if(RIVET_WITH_PDFIUM)
    target_link_libraries(rivet_pdf_tests PRIVATE rivet::pdfium)
else()
    target_link_libraries(rivet_pdf_tests PRIVATE rivet::pdf)
endif()

# Fixture paths are resolved at build time so the test binary works from any
# working directory (ctest runs it from the build tree).
target_compile_definitions(rivet_pdf_tests PRIVATE
    RIVET_PDF_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures"
    RIVET_PDF_RENDER_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/pdf/fixtures"
    RIVET_PDF_TEXT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/pdf/fixtures"
)

rivet_set_target_warnings(rivet_pdf_tests)
rivet_set_target_sanitizers(rivet_pdf_tests)
