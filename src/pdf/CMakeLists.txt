if(RIVET_WITH_PDFIUM)
    # Header-only interface target; implementation lives in rivet_pdfium.
    add_library(rivet_pdf INTERFACE)
    add_library(rivet::pdf ALIAS rivet_pdf)
    target_link_libraries(rivet_pdf INTERFACE rivet::core)
    add_subdirectory(pdfium)
else()
    add_library(rivet_pdf STATIC
        PdfSystem_null.cpp
    )
    add_library(rivet::pdf ALIAS rivet_pdf)
    target_link_libraries(rivet_pdf PUBLIC rivet::core)
    rivet_set_target_warnings(rivet_pdf)
    rivet_set_target_sanitizers(rivet_pdf)
endif()
