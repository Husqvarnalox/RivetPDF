# Provider tests. Each executable is its own ctest entry; backend-dependent
# tests skip (print "SKIP", pass) when the backend is absent.

# X.509 reader, display model, signature classification (backend-free).
add_executable(rivet_crypto_x509_tests
    TestX509Parse.cpp
    TestQualification.cpp
    TestCertificateDisplay.cpp
)
target_link_libraries(rivet_crypto_x509_tests PRIVATE rivet::test_harness rivet::crypto)
if(RIVET_ENABLE_CRYPTO)
    # Equivalence tests compare the backend-free reader with the OpenSSL provider.
    find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
    target_include_directories(rivet_crypto_x509_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(rivet_crypto_x509_tests PRIVATE OpenSSL::Crypto)
    target_compile_definitions(rivet_crypto_x509_tests PRIVATE RIVET_TEST_CRYPTO_OPENSSL=1)
endif()
rivet_set_target_warnings(rivet_crypto_x509_tests)
rivet_set_target_sanitizers(rivet_crypto_x509_tests)
add_test(NAME crypto_x509 COMMAND rivet_crypto_x509_tests)

# CryptoPro CSP.
add_executable(rivet_crypto_cryptopro_tests
    TestCryptoPro.cpp
)
target_link_libraries(rivet_crypto_cryptopro_tests PRIVATE rivet::test_harness rivet::crypto_providers)
rivet_set_target_warnings(rivet_crypto_cryptopro_tests)
rivet_set_target_sanitizers(rivet_crypto_cryptopro_tests)
add_test(NAME crypto_cryptopro COMMAND rivet_crypto_cryptopro_tests)

# PKCS#11 (mock module + optional SoftHSM / real modules).
add_executable(rivet_crypto_pkcs11_tests
    TestPkcs11.cpp
)
target_link_libraries(rivet_crypto_pkcs11_tests PRIVATE rivet::test_harness rivet::crypto_providers)
rivet_set_target_warnings(rivet_crypto_pkcs11_tests)
rivet_set_target_sanitizers(rivet_crypto_pkcs11_tests)
add_test(NAME crypto_pkcs11 COMMAND rivet_crypto_pkcs11_tests)

# Platform keystore.
add_executable(rivet_crypto_platform_tests
    TestPlatformStore.cpp
)
target_link_libraries(rivet_crypto_platform_tests PRIVATE rivet::test_harness rivet::crypto_providers)
if(RIVET_ENABLE_CRYPTO)
    # Keychain tests fabricate identities with OpenSSL and verify signatures with it.
    find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
    target_include_directories(rivet_crypto_platform_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(rivet_crypto_platform_tests PRIVATE OpenSSL::Crypto)
    target_compile_definitions(rivet_crypto_platform_tests PRIVATE RIVET_TEST_CRYPTO_OPENSSL=1)
endif()
rivet_set_target_warnings(rivet_crypto_platform_tests)
rivet_set_target_sanitizers(rivet_crypto_platform_tests)
add_test(NAME crypto_platform COMMAND rivet_crypto_platform_tests)
