add_executable(rivet_crypto_tests
    TestAlgorithms.cpp
    TestAsync.cpp
    TestDer.cpp
    TestMalformedCms.cpp
    TestSecretBuffer.cpp
)

if(RIVET_ENABLE_CRYPTO)
    find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
    target_sources(rivet_crypto_tests PRIVATE
        TestCertificates.cpp
        TestCms.cpp
        TestInterop.cpp
        TestPkcs12.cpp
    )
    target_link_libraries(rivet_crypto_tests PRIVATE OpenSSL::Crypto)
    # Interop tests find the CLI tool by path when available.
    target_compile_definitions(rivet_crypto_tests PRIVATE RIVET_TEST_CRYPTO_OPENSSL=1)
endif()

add_test(NAME crypto COMMAND rivet_crypto_tests)

target_link_libraries(rivet_crypto_tests PRIVATE
    rivet::test_harness
    rivet::crypto
)

rivet_set_target_warnings(rivet_crypto_tests)
rivet_set_target_sanitizers(rivet_crypto_tests)
