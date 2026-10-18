add_executable(rivet_net_tests
    TestAddress.cpp
    TestAsync.cpp
    TestClient.cpp
    TestConsent.cpp
    TestCurl.cpp
    TestPolicy.cpp
    TestUrl.cpp
)

add_test(NAME net COMMAND rivet_net_tests)

target_link_libraries(rivet_net_tests PRIVATE
    rivet::test_harness
    rivet::net
)

rivet_set_target_warnings(rivet_net_tests)
rivet_set_target_sanitizers(rivet_net_tests)
