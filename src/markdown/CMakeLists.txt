# Portable Markdown core: model, parser (MD4C backed), plain-text extraction
# and the pure layout engine. Links only rivet_core: no PDF, editor, UI or
# platform code.
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/md4c ${CMAKE_BINARY_DIR}/third_party/md4c)

add_library(rivet_markdown STATIC
    Md4cMarkdownParser.cpp
    MarkdownPlainText.cpp
    MarkdownSlug.cpp
)

add_library(rivet::markdown ALIAS rivet_markdown)

target_include_directories(rivet_markdown PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(rivet_markdown PUBLIC rivet::core)
# md4c is an implementation detail: its headers are not exposed to consumers.
target_link_libraries(rivet_markdown PRIVATE rivet_md4c)

rivet_set_target_warnings(rivet_markdown)
rivet_set_target_sanitizers(rivet_markdown)
