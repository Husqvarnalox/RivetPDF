add_executable(rivet_editor_tests
    TestSelectionModel.cpp
    TestTextPageCache.cpp
    TestTextPipeline.cpp
    TestSearchConcurrency.cpp
    TestSelectionText.cpp
    TestCommandStack.cpp
    TestDocumentRenderer.cpp
    TestDocumentSession.cpp
    TestPrintSpooler.cpp
    # Page model, page commands, page selection (portable, fakes/).
    TestPageModel.cpp
    TestPageSession.cpp
    # Page model -> assembly -> reload against PDFium fixtures: PDFium-ON
    # bodies only (returns early without a backend).
    TestPageModelPdfium.cpp
)

add_test(NAME editor COMMAND rivet_editor_tests)

target_link_libraries(rivet_editor_tests PRIVATE
    rivet::test_harness
    rivet::editor
)

# pdf::createEngine(): the PDFium engine when built in, otherwise the null
# engine that rivet::pdf already provides.
if(RIVET_WITH_PDFIUM)
    target_link_libraries(rivet_editor_tests PRIVATE rivet::pdfium)
endif()

target_include_directories(rivet_editor_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_definitions(rivet_editor_tests PRIVATE
    RIVET_EDITOR_PDF_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/pdf/fixtures"
)

rivet_set_target_warnings(rivet_editor_tests)
rivet_set_target_sanitizers(rivet_editor_tests)
