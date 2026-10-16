add_executable(rivet_ui_tests
    TestButton.cpp
    TestControls.cpp
    TestOutlinePanel.cpp
    TestPageThumbnailList.cpp
    TestPath.cpp
    TestPdfViewport.cpp
    TestScrollBar.cpp
    TestSourceEditor.cpp
    TestTabStrip.cpp
    TestTextArea.cpp
    TestTextBuffer.cpp
    TestTextField.cpp
    TestTheme.cpp
    TestNoHardcodedColors.cpp
    TestWidgetTree.cpp
)

# The color-literal scan reads the sources (ADR-0033).
target_compile_definitions(rivet_ui_tests PRIVATE RIVET_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

add_test(NAME ui COMMAND rivet_ui_tests)

# rivet::ui propagates rivet::core and rivet::render (PUBLIC linkage); adding
# them again here would trip Apple ld's duplicate-library warning.
target_link_libraries(rivet_ui_tests PRIVATE
    rivet::test_harness
    rivet::ui
)

rivet_set_target_warnings(rivet_ui_tests)
rivet_set_target_sanitizers(rivet_ui_tests)
