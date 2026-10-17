# PDF signing engine (ADR-0037, ADR-0038): incremental signer, bounded
# structure reader and signature verifier. Depends only on rivet_core and
# rivet_crypto; it never touches PDFium and never rebuilds the document.

add_library(rivet_pdf_sign STATIC
    PdfFile.cpp
    PdfInflate.cpp
    PdfObject.cpp
    PdfSignatureVerifier.cpp
    PdfSigner.cpp
    SignError.cpp
)
add_library(rivet::pdf_sign ALIAS rivet_pdf_sign)

target_include_directories(rivet_pdf_sign PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(rivet_pdf_sign PUBLIC rivet::core rivet::crypto)

rivet_set_target_warnings(rivet_pdf_sign)
rivet_set_target_sanitizers(rivet_pdf_sign)

# Diagnostic tool: lists the signatures of a PDF with their validation status.
add_executable(rivet-pdf-signature-inspect tools/PdfSignatureInspectMain.cpp)
target_link_libraries(rivet-pdf-signature-inspect PRIVATE rivet::pdf_sign)
rivet_set_target_warnings(rivet-pdf-signature-inspect)
rivet_set_target_sanitizers(rivet-pdf-signature-inspect)

# Dev/test tool: signs a PDF with a PKCS#12 identity and creates throwaway TEST
# identities. The password comes from RIVET_P12_PASSWORD or stdin, never argv.
add_executable(rivet-pdf-sign tools/PdfSignMain.cpp)
target_link_libraries(rivet-pdf-sign PRIVATE rivet::pdf_sign rivet::timestamp rivet::net)
rivet_set_target_warnings(rivet-pdf-sign)
rivet_set_target_sanitizers(rivet-pdf-sign)
