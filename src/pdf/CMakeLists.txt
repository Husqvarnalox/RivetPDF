# Portable sources shared by both configurations.
set(RIVET_PDF_SOURCES
    BundledFonts.cpp
    PdfAnnotation.cpp
    PdfContent.cpp
    PdfPageGeometry.cpp
    PdfText.cpp
    PdfTextLayout.cpp
)

# The bundled fonts (third_party/fonts, ADR-0016) are compiled in at build
# time by cmake/EmbedFonts.cmake. The order is the PdfBundledFont order.
set(RIVET_BUNDLED_FONT_FILES
    Arimo-Regular.ttf
    Arimo-Bold.ttf
    Tinos-Regular.ttf
    Tinos-Bold.ttf
    Cousine-Regular.ttf
    Cousine-Bold.ttf
)
set(RIVET_FONT_DIR "${PROJECT_SOURCE_DIR}/third_party/fonts")
set(RIVET_BUNDLED_FONT_DATA_CPP "${CMAKE_CURRENT_BINARY_DIR}/BundledFontData.cpp")
set(RIVET_BUNDLED_FONT_DEPENDS "")
foreach(font IN LISTS RIVET_BUNDLED_FONT_FILES)
    list(APPEND RIVET_BUNDLED_FONT_DEPENDS "${RIVET_FONT_DIR}/${font}")
endforeach()
add_custom_command(
    OUTPUT "${RIVET_BUNDLED_FONT_DATA_CPP}"
    COMMAND ${CMAKE_COMMAND}
        "-DFONT_DIR=${RIVET_FONT_DIR}"
        "-DOUTPUT=${RIVET_BUNDLED_FONT_DATA_CPP}"
        "-DFONTS=${RIVET_BUNDLED_FONT_FILES}"
        -P "${PROJECT_SOURCE_DIR}/cmake/EmbedFonts.cmake"
    DEPENDS ${RIVET_BUNDLED_FONT_DEPENDS} "${PROJECT_SOURCE_DIR}/cmake/EmbedFonts.cmake"
    COMMENT "Embedding bundled fonts"
    VERBATIM)

if(RIVET_WITH_PDFIUM)
    # Interface (PdfEngine.hpp etc.) plus the Rivet-owned text model, page
    # geometry (views, box mapping), content model and bundled fonts, which
    # are engine-independent; the PDFium-specific implementation lives in
    # rivet_pdfium.
    add_library(rivet_pdf STATIC ${RIVET_PDF_SOURCES} "${RIVET_BUNDLED_FONT_DATA_CPP}")
    add_library(rivet::pdf ALIAS rivet_pdf)
    target_link_libraries(rivet_pdf PUBLIC rivet::core)
    rivet_set_target_warnings(rivet_pdf)
    rivet_set_target_sanitizers(rivet_pdf)
    add_subdirectory(pdfium)
else()
    add_library(rivet_pdf STATIC ${RIVET_PDF_SOURCES} PdfSystem_null.cpp "${RIVET_BUNDLED_FONT_DATA_CPP}")
    add_library(rivet::pdf ALIAS rivet_pdf)
    target_link_libraries(rivet_pdf PUBLIC rivet::core)
    rivet_set_target_warnings(rivet_pdf)
    rivet_set_target_sanitizers(rivet_pdf)
endif()
