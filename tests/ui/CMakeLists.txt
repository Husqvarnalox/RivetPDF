add_executable(rivet_ui_tests
    TestButton.cpp
    TestPdfViewport.cpp
    TestScrollBar.cpp
    TestTabStrip.cpp
    TestTextField.cpp
    TestWidgetTree.cpp
)

add_test(NAME ui COMMAND rivet_ui_tests)

# rivet::ui propagates rivet::core and rivet::render (PUBLIC linkage); adding
# them again here would trip Apple ld's duplicate-library warning.
target_link_libraries(rivet_ui_tests PRIVATE
    rivet::test_harness
    rivet::ui
)

rivet_set_target_warnings(rivet_ui_tests)
rivet_set_target_sanitizers(rivet_ui_tests)
