# OCSP / CRL / revocation-policy tests. TestRevocationOff.cpp needs no crypto
# backend; the rest fabricates a PKI and OpenSSL-produced responses and is only
# built with RIVET_ENABLE_CRYPTO.
add_executable(rivet_revocation_tests
    TestRevocationOff.cpp
)
target_link_libraries(rivet_revocation_tests PRIVATE rivet::test_harness rivet::revocation rivet::net)

if(RIVET_ENABLE_CRYPTO)
    find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
    target_sources(rivet_revocation_tests PRIVATE
        TestRevocation.cpp
        TestRevocationInterop.cpp
    )
    # TestSupport.hpp lives one directory up.
    target_include_directories(rivet_revocation_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(rivet_revocation_tests PRIVATE OpenSSL::Crypto)
    target_compile_definitions(rivet_revocation_tests PRIVATE RIVET_TEST_CRYPTO_OPENSSL=1)
endif()

rivet_set_target_warnings(rivet_revocation_tests)
rivet_set_target_sanitizers(rivet_revocation_tests)
add_test(NAME crypto_revocation COMMAND rivet_revocation_tests)
