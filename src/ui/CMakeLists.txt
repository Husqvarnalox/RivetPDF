add_library(rivet_ui STATIC
    Animation.cpp
    Button.cpp
    IconButton.cpp
    Icons.cpp
    Theme.cpp
    OutlinePanel.cpp
    PageThumbnailList.cpp
    PdfViewport.cpp
    PopupMenu.cpp
    ScrollBar.cpp
    SegmentedControl.cpp
    SourceEditor.cpp
    Sidebar.cpp
    TabStrip.cpp
    TextBuffer.cpp
    TextArea.cpp
    TextField.cpp
    Toolbar.cpp
    Tooltip.cpp
    ZoomControl.cpp
)

add_library(rivet::ui ALIAS rivet_ui)

target_link_libraries(rivet_ui PUBLIC rivet::core rivet::render)

rivet_set_target_warnings(rivet_ui)
rivet_set_target_sanitizers(rivet_ui)
