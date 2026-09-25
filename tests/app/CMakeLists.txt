add_executable(rivet_app_tests
    TestDocumentWorkspace.cpp
    TestWorkspaceLifecycle.cpp
    TestUrlPolicy.cpp
)

add_test(NAME app COMMAND rivet_app_tests)

target_link_libraries(rivet_app_tests PRIVATE
    rivet::test_harness
    rivet::app
)

rivet_set_target_warnings(rivet_app_tests)
rivet_set_target_sanitizers(rivet_app_tests)
