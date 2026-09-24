add_library(rivet_editor STATIC
    CommandStack.cpp
    DocumentRenderer.cpp
    DocumentSession.cpp
)

add_library(rivet::editor ALIAS rivet_editor)

target_link_libraries(rivet_editor PUBLIC rivet::core rivet::render rivet::pdf)

rivet_set_target_warnings(rivet_editor)
rivet_set_target_sanitizers(rivet_editor)
