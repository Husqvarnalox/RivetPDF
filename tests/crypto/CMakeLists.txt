add_executable(rivet_crypto_tests
    TestAlgorithms.cpp
    TestAsync.cpp
    TestDer.cpp
    TestMalformedCms.cpp
    TestModuleTrust.cpp
    TestSecretBuffer.cpp
    TestTimeStampOff.cpp
)

if(RIVET_ENABLE_CRYPTO)
    find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
    target_sources(rivet_crypto_tests PRIVATE
        TestCertificates.cpp
        TestCms.cpp
        TestInterop.cpp
        TestPathBuilder.cpp
        TestPkcs12.cpp
        TestTimeStamp.cpp
    )
    target_link_libraries(rivet_crypto_tests PRIVATE OpenSSL::Crypto)
    # Interop tests find the CLI tool by path when available.
    target_compile_definitions(rivet_crypto_tests PRIVATE RIVET_TEST_CRYPTO_OPENSSL=1)
endif()

if(NOT WIN32)
    # Two trivial loadable modules (ids 1 and 2) for the module-loading TOCTOU tests.
    foreach(id a b)
        add_library(rivet_test_module_${id} MODULE test_modules/module_${id}.cpp)
        set_target_properties(rivet_test_module_${id} PROPERTIES PREFIX "" SUFFIX ".so"
            CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
        add_dependencies(rivet_crypto_tests rivet_test_module_${id})
    endforeach()
    target_compile_definitions(rivet_crypto_tests PRIVATE
        "RIVET_TEST_MODULE_A=\"$<TARGET_FILE:rivet_test_module_a>\""
        "RIVET_TEST_MODULE_B=\"$<TARGET_FILE:rivet_test_module_b>\"")
    target_link_libraries(rivet_crypto_tests PRIVATE ${CMAKE_DL_LIBS})
endif()

add_test(NAME crypto COMMAND rivet_crypto_tests)

target_link_libraries(rivet_crypto_tests PRIVATE
    rivet::test_harness
    rivet::timestamp
    rivet::net
)

rivet_set_target_warnings(rivet_crypto_tests)
rivet_set_target_sanitizers(rivet_crypto_tests)

add_subdirectory(providers)
add_subdirectory(revocation)
add_subdirectory(remote)
