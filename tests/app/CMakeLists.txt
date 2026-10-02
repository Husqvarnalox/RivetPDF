add_executable(rivet_app_tests
    TestDocumentWorkspace.cpp
    TestShellWiring.cpp
    TestWorkspaceLifecycle.cpp
    TestUrlPolicy.cpp
    TestAnnotationInteraction.cpp
    TestAnnotationPainter.cpp
    TestPageEditingController.cpp
    TestFileController.cpp
    # PDFium-ON round trips through the file lifecycle (early return
    # without a backend).
    TestFileRoundTripPdfium.cpp
)

add_test(NAME app COMMAND rivet_app_tests)

target_link_libraries(rivet_app_tests PRIVATE
    rivet::test_harness
    rivet::app
)

rivet_set_target_warnings(rivet_app_tests)
rivet_set_target_sanitizers(rivet_app_tests)

# TestPageEditingController/TestFileController share the editor-layer fakes
# (FakePageDocument, FakeWritableEngine).
target_include_directories(rivet_app_tests PRIVATE "${CMAKE_SOURCE_DIR}/tests/editor"
    "${CMAKE_SOURCE_DIR}/tests/ui")

# PDFium-ON bodies (TestFileRoundTripPdfium.cpp) run against the shared
# fixtures and early-return without a backend.
if(RIVET_WITH_PDFIUM)
    target_link_libraries(rivet_app_tests PRIVATE rivet::pdfium)
endif()
target_compile_definitions(rivet_app_tests PRIVATE
    RIVET_APP_PDF_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/pdf/fixtures"
)
