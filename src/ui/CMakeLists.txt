add_library(rivet_ui STATIC
    Button.cpp
    OutlinePanel.cpp
    PageThumbnailList.cpp
    PdfViewport.cpp
    ScrollBar.cpp
    Sidebar.cpp
    TabStrip.cpp
    TextField.cpp
    Toolbar.cpp
)

add_library(rivet::ui ALIAS rivet_ui)

target_link_libraries(rivet_ui PUBLIC rivet::core rivet::render)

rivet_set_target_warnings(rivet_ui)
rivet_set_target_sanitizers(rivet_ui)
