add_executable(rivet_editor_tests
    TestCommandStack.cpp
    TestDocumentSession.cpp
)

add_test(NAME editor COMMAND rivet_editor_tests)

target_link_libraries(rivet_editor_tests PRIVATE
    rivet::test_harness
    rivet::editor
    rivet::core
    rivet::render
)

rivet_set_target_warnings(rivet_editor_tests)
rivet_set_target_sanitizers(rivet_editor_tests)
