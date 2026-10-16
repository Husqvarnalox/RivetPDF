add_executable(rivet_app_tests
    TestDocumentWorkspace.cpp
    TestShellWiring.cpp
    # ADR-0033 shell chrome (welcome, tool modes, sidebar, overflow menu).
    TestShellChrome.cpp
    TestWorkspaceLifecycle.cpp
    TestUrlPolicy.cpp
    # Saved signatures (ADR-0025).
    TestSignatureStore.cpp
    TestSignatureSheet.cpp
    TestMenuModel.cpp
    TestFormController.cpp
    # Redaction tool, marks and Apply Redactions (ADR-0026), fake backend.
    TestRedactionController.cpp
    # Recognize Text (ADR-0027) with a fake ITextRecognizer.
    TestOcrController.cpp
    TestAnnotationController.cpp
    TestAnnotationStress.cpp
    TestAnnotationInteraction.cpp
    TestAnnotationPainter.cpp
    TestContentInteraction.cpp
    TestContentController.cpp
    TestContentLayer.cpp
    TestContentBarController.cpp
    TestPageEditingController.cpp
    TestFileController.cpp
    TestMarkdownShell.cpp
    # Document comparison tab (ADR-0029).
    TestComparisonTab.cpp
    TestMarkdownPreview.cpp
    TestMarkdownSourcePane.cpp
    TestMarkdownLive.cpp
    TestMarkdownWysiwyg.cpp
    TestSearchRestart.cpp
    TestPrintCoordinator.cpp
    # Phase 6A Markdown -> PDF export: the portable exporter and the
    # Export as PDF / Markdown print controller (fake engine).
    TestMarkdownExport.cpp
    TestMarkdownExportController.cpp
    # Mermaid diagrams and math in the preview and the export (ADR-0032).
    TestMarkdownGraphics.cpp
    # Phase 5 end-to-end content editing over the real engine (PDFium-ON
    # bodies; early return without a backend).
    TestContentIntegration.cpp
    TestContentIntegrationText.cpp
    TestContentIntegrationImage.cpp
    TestContentIntegrationIdentity.cpp
    TestContentIntegrationLimits.cpp
    TestContentIntegrationStress.cpp
    TestContentIntegrationPrint.cpp
    # PDFium-ON round trips through the file lifecycle (early return
    # without a backend).
    TestFileRoundTripPdfium.cpp
    # PDFium-ON Markdown export reopened through the real engine (early
    # return without a backend).
    TestMarkdownExportPdfium.cpp
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
    "${CMAKE_SOURCE_DIR}/tests/ui" "${CMAKE_SOURCE_DIR}/tests/pdf" "${CMAKE_SOURCE_DIR}/tests/markdown")

# PDFium-ON bodies (TestFileRoundTripPdfium.cpp) run against the shared
# fixtures and early-return without a backend.
if(RIVET_WITH_PDFIUM)
    target_link_libraries(rivet_app_tests PRIVATE rivet::pdfium)
endif()
target_compile_definitions(rivet_app_tests PRIVATE
    RIVET_APP_PDF_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/pdf/fixtures"
)
