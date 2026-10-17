if(NOT RIVET_ENABLE_CRYPTO)
    return()
endif()

find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)

add_executable(rivet_pdf_sign_tests
    TestPdfSign.cpp
    TestPdfSignHostile.cpp
    TestPdfSignInterop.cpp
)
add_executable(rivet_pdf_sign_perf_tests TestPdfSignPerf.cpp)

foreach(_target IN ITEMS rivet_pdf_sign_tests rivet_pdf_sign_perf_tests)
    target_include_directories(${_target} PRIVATE ${CMAKE_SOURCE_DIR}/tests/crypto)
    target_link_libraries(${_target} PRIVATE rivet::test_harness rivet::pdf_sign OpenSSL::Crypto)
    rivet_set_target_warnings(${_target})
    rivet_set_target_sanitizers(${_target})
endforeach()

add_test(NAME pdf_sign COMMAND rivet_pdf_sign_tests)
add_test(NAME pdf_sign_perf COMMAND rivet_pdf_sign_perf_tests)
set_tests_properties(pdf_sign_perf PROPERTIES LABELS slow TIMEOUT 300)
