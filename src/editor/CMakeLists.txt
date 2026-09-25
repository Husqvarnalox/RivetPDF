add_library(rivet_editor STATIC
    SelectionModel.cpp
    SelectionText.cpp
    TextPageCache.cpp
    TextSearchController.cpp
    TextService.cpp
    LinkService.cpp
    CommandStack.cpp
    DocumentRenderer.cpp
    DocumentSession.cpp
)

add_library(rivet::editor ALIAS rivet_editor)

target_link_libraries(rivet_editor PUBLIC rivet::render rivet::pdf rivet::core)

rivet_set_target_warnings(rivet_editor)
rivet_set_target_sanitizers(rivet_editor)
