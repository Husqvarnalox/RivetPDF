add_library(rivet_app STATIC
    DocumentWorkspace.cpp
    PasswordPromptController.cpp
    PrintCoordinator.cpp
    SearchBarController.cpp
    ShellController.cpp
    SidebarController.cpp
    StatusBarController.cpp
    TextInteractionController.cpp
    TextLabel.cpp
)

add_library(rivet::app ALIAS rivet_app)

target_link_libraries(rivet_app
    PUBLIC rivet::ui rivet::editor rivet::pdf rivet::platform rivet::core
)

rivet_set_target_warnings(rivet_app)
rivet_set_target_sanitizers(rivet_app)
