# Remote (cloud) signing: JSON / base64 helpers (backend-free) and the CryptoPro
# DSS adapter against an in-process MOCK service (needs the OpenSSL provider for
# the test CA, the mock service's key and PAdES verification). No real service
# is ever contacted.
add_executable(rivet_crypto_remote_tests TestJsonLite.cpp)
target_link_libraries(rivet_crypto_remote_tests PRIVATE rivet::test_harness rivet::remote_signing)

if(RIVET_ENABLE_CRYPTO)
    find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
    target_sources(rivet_crypto_remote_tests PRIVATE TestRemoteSigning.cpp)
    target_include_directories(rivet_crypto_remote_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/tests/crypto ${CMAKE_SOURCE_DIR}/tests/pdf/sign)
    target_link_libraries(rivet_crypto_remote_tests PRIVATE rivet::pdf_sign OpenSSL::Crypto)
endif()

rivet_set_target_warnings(rivet_crypto_remote_tests)
rivet_set_target_sanitizers(rivet_crypto_remote_tests)
add_test(NAME crypto_remote COMMAND rivet_crypto_remote_tests)
