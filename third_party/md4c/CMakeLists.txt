# MD4C (https://github.com/mity/md4c), vendored verbatim: md4c.c / md4c.h /
# LICENSE.md. MIT licensed. Version: see VENDORED.txt.
#
# Built as a C static library with warnings relaxed (third-party code is not
# held to the Rivet warning set). Only src/markdown/Md4cMarkdownParser.cpp
# may include md4c.h.
enable_language(C)

add_library(rivet_md4c STATIC md4c.c)
target_include_directories(rivet_md4c PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(rivet_md4c PROPERTIES C_STANDARD 99 C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(rivet_md4c PRIVATE /W0)
else()
    target_compile_options(rivet_md4c PRIVATE -w)
endif()
rivet_set_target_sanitizers(rivet_md4c)
