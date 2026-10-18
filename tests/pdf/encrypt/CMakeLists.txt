# PDF encryption tests (ADR-0046). Need the OpenSSL-backed crypto provider.
if(NOT RIVET_ENABLE_CRYPTO)
    return()
endif()

add_executable(rivet_pdf_encrypt_tests TestPdfEncrypt.cpp)
target_link_libraries(rivet_pdf_encrypt_tests PRIVATE rivet::test_harness rivet::pdf_encrypt)
rivet_set_target_warnings(rivet_pdf_encrypt_tests)
rivet_set_target_sanitizers(rivet_pdf_encrypt_tests)
add_test(NAME pdf_encrypt COMMAND rivet_pdf_encrypt_tests)

# Interop with the real PDFium engine: PDFium-ON builds only.
if(RIVET_WITH_PDFIUM)
    add_executable(rivet_pdf_encrypt_pdfium_tests TestPdfiumEncrypt.cpp)
    target_include_directories(rivet_pdf_encrypt_pdfium_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/pdf)
    target_link_libraries(rivet_pdf_encrypt_pdfium_tests PRIVATE rivet::test_harness rivet::pdf_encrypt rivet::pdfium)
    target_compile_definitions(rivet_pdf_encrypt_pdfium_tests PRIVATE
        RIVET_PDF_RENDER_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/pdf/fixtures")
    rivet_set_target_warnings(rivet_pdf_encrypt_pdfium_tests)
    rivet_set_target_sanitizers(rivet_pdf_encrypt_pdfium_tests)
    add_test(NAME pdf_encrypt_pdfium COMMAND rivet_pdf_encrypt_pdfium_tests)
endif()
