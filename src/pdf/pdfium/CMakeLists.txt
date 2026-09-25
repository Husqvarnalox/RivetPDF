add_library(rivet_pdfium STATIC
    PdfSystem_pdfium.cpp
    PdfiumCallGate.cpp
    PdfiumEngine.cpp
    PdfiumAssembly.cpp
    PdfiumDocument.cpp
    PdfiumFileSource.cpp
    PdfiumTextPage.cpp
    PdfiumLibrary.cpp
)
add_library(rivet::pdfium ALIAS rivet_pdfium)
target_link_libraries(rivet_pdfium
    PUBLIC rivet::pdf
    PRIVATE PDFium::PDFium
)
rivet_set_target_warnings(rivet_pdfium)
rivet_set_target_sanitizers(rivet_pdfium)
