# PDF encryption (ADR-0046): post-save encryptor for the constrained byte
# output of a PDFium save. Never touches PDFium; crypto primitives come from
# rivet_crypto. With RIVET_ENABLE_CRYPTO=OFF the library still builds and
# encryptPdf() reports NotAvailable (the null crypto provider).

add_library(rivet_pdf_encrypt STATIC
    PdfEncrypt.cpp
    StandardHandler.cpp
)
add_library(rivet::pdf_encrypt ALIAS rivet_pdf_encrypt)

target_include_directories(rivet_pdf_encrypt PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(rivet_pdf_encrypt PUBLIC rivet::core rivet::crypto)

rivet_set_target_warnings(rivet_pdf_encrypt)
rivet_set_target_sanitizers(rivet_pdf_encrypt)

# Dev/test tool: encrypts a PDF (the passwords come from the environment or
# stdin, never from argv).
add_executable(rivet-pdf-encrypt tools/PdfEncryptMain.cpp)
target_link_libraries(rivet-pdf-encrypt PRIVATE rivet::pdf_encrypt)
rivet_set_target_warnings(rivet-pdf-encrypt)
rivet_set_target_sanitizers(rivet-pdf-encrypt)
