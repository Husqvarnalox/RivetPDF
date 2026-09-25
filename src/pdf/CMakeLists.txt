if(RIVET_WITH_PDFIUM)
    # Interface (PdfEngine.hpp etc.) plus the Rivet-owned text model, which is
    # engine-independent; the PDFium-specific implementation lives in
    # rivet_pdfium.
    add_library(rivet_pdf STATIC
        PdfText.cpp
    )
    add_library(rivet::pdf ALIAS rivet_pdf)
    target_link_libraries(rivet_pdf PUBLIC rivet::core)
    rivet_set_target_warnings(rivet_pdf)
    rivet_set_target_sanitizers(rivet_pdf)
    add_subdirectory(pdfium)
else()
    add_library(rivet_pdf STATIC
        PdfSystem_null.cpp
        PdfText.cpp
    )
    add_library(rivet::pdf ALIAS rivet_pdf)
    target_link_libraries(rivet_pdf PUBLIC rivet::core)
    rivet_set_target_warnings(rivet_pdf)
    rivet_set_target_sanitizers(rivet_pdf)
endif()
