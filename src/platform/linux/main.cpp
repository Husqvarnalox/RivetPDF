// SPDX-License-Identifier: MPL-2.0
//
// Composition root of the Linux (X11) desktop shell: the counterpart of
// src/platform/macos/main.mm. It wires the platform services into
// app::createShell(), renders app::buildMenuBar() as the in-window menu bar,
// runs the event loop and tears everything down in the same order as macOS.

#include "LinuxAlertService.hpp"
#include "LinuxAppDataLocation.hpp"
#include "LinuxAppearance.hpp"
#include "LinuxClipboard.hpp"
#include "LinuxExternalUrlOpener.hpp"
#include "LinuxFileDialog.hpp"
#include "LinuxImageDecoder.hpp"
#include "LinuxImageEncoder.hpp"
#include "LinuxMainThreadDispatcher.hpp"
#include "LinuxMenuBar.hpp"
#include "X11Window.hpp"
#if RIVET_HAVE_CUPS
#include "LinuxPrintService.hpp"
#endif

#include "app/MenuModel.hpp"
#include "app/QaScript.hpp"
#include "app/ShellController.hpp"
#include "platform/AppLifecycle.hpp"
#include "platform/PlatformKit.hpp"
#include "ui/UiTypes.hpp"

#include <clocale>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

// app::buildMenuBar() -> the platform menu strip. Item ids are flat indices
// into `commands` (the same scheme the macOS menu uses for item tags).
std::vector<rivet::platform::MenuBarMenu> convertMenus(const std::vector<rivet::app::Menu>& model,
                                                       std::vector<rivet::app::AppCommand>& commands) {
    std::vector<rivet::platform::MenuBarMenu> menus;
    for (const rivet::app::Menu& menu : model) {
        rivet::platform::MenuBarMenu out;
        out.title = menu.title;
        for (const rivet::app::MenuItem& entry : menu.items) {
            rivet::platform::MenuBarItem item;
            item.title = entry.title;
            item.id = static_cast<int>(commands.size());
            item.separatorBefore = entry.separatorBefore;
            item.checkable = entry.checkable;
            if (entry.shortcut.has_value()) {
                item.shortcut = rivet::platform::MenuBarShortcut{entry.shortcut->key, entry.shortcut->primary,
                                                                 entry.shortcut->shift, entry.shortcut->alt};
            }
            commands.push_back(entry.command);
            out.items.push_back(std::move(item));
        }
        menus.push_back(std::move(out));
    }
    return menus;
}

} // namespace

int main(int argc, char** argv) {
    // Only LC_CTYPE: the input method needs the locale's character set, while
    // LC_NUMERIC must stay "C" (PDF serialization formats numbers).
    std::setlocale(LC_CTYPE, "");

    using namespace rivet;

    // Services must outlive the app shell and the event loop.
    const auto dispatcher = std::make_unique<platform::LinuxMainThreadDispatcher>();
    std::string error;
    const auto connection = platform::X11Connection::open(*dispatcher, error);
    if (!connection) {
        std::fprintf(stderr, "rivet: %s\n", error.c_str());
        return 1;
    }

    platform::X11Window::Options windowOptions;
    windowOptions.title = "Rivet";
    windowOptions.logicalSize = core::Size{1280.0, 850.0};
    windowOptions.minLogicalSize = core::Size{940.0, 600.0};
    auto window = std::make_unique<platform::X11Window>(*connection, windowOptions);

    const auto alertService = std::make_unique<platform::LinuxAlertService>(*connection, window.get());
    const auto fileDialog = std::make_unique<platform::LinuxFileDialog>(*connection, alertService.get());
    fileDialog->setOwner(window.get());
    const auto imageDecoder = std::make_unique<platform::LinuxImageDecoder>();
    const auto imageEncoder = std::make_unique<platform::LinuxImageEncoder>();
    const auto appData = std::make_unique<platform::LinuxAppDataLocation>();
    const auto appearance = std::make_unique<platform::LinuxAppearance>(connection->display());
    const auto clipboard = std::make_unique<platform::LinuxClipboard>(*connection);
    const auto urlOpener = std::make_unique<platform::LinuxExternalUrlOpener>();
#if RIVET_HAVE_CUPS
    const auto printService = std::make_unique<platform::LinuxPrintService>(alertService.get());
#endif
    const auto lifecycle = std::make_unique<platform::LifecycleHooks>();

    platform::ShellServices services;
    services.redrawSink = window->redrawSink();
    services.mainDispatcher = dispatcher.get();
    services.fileDialog = fileDialog.get();
    services.imageDecoder = imageDecoder.get();
    services.imageEncoder = imageEncoder.get();
    services.appData = appData.get();
    services.appearance = appearance.get();
    services.clipboard = clipboard.get();
    services.urlOpener = urlOpener.get();
#if RIVET_HAVE_CUPS
    services.printService = printService.get();
#endif
    // Without zenity/kdialog there is no save panel: Save As is unavailable
    // (the open calls still report why).
    services.saveDialog = fileDialog->available() ? fileDialog.get() : nullptr;
    services.alerts = alertService.get();
    services.lifecycle = lifecycle.get();
    platform::X11Window* const windowPtr = window.get();
    services.setWindowTitle = [windowPtr](const std::string& title) { windowPtr->setTitle(title); };
    services.setDocumentEdited = [windowPtr](bool edited) { windowPtr->setDocumentEdited(edited); };
    services.captureWindowPng = [windowPtr](const std::filesystem::path& path) { return windowPtr->writePng(path); };
    services.resizeWindow = [windowPtr](double width, double height) { windowPtr->resizeContent(width, height); };

    // Scripted QA run (RIVET_QA_SCRIPT): non-interactive dialogs and a
    // script runner; inert (null) otherwise. Outlives the shell.
    std::unique_ptr<app::QaSession> qa = app::QaSession::fromEnvironment();
    if (qa != nullptr) qa->installDoubles(services);

    // Event-loop state; quit decisions land here.
    bool running = true;
    const auto requestQuit = [&running, hooks = lifecycle.get()] {
        // Same protocol as applicationShouldTerminate: a synchronous answer
        // or a later asyncReply (main thread) after asynchronous saves.
        const auto decision = hooks->dispatchQuitRequest([&running](bool proceed) {
            if (proceed) running = false;
        });
        if (decision == platform::LifecycleHooks::QuitDecision::Proceed) running = false;
    };

    // The shell owns the widget tree; the window only borrows the root.
    auto shell = app::createShell(services);

    std::vector<app::AppCommand> commands;
    platform::LinuxMenuBar menuBar(convertMenus(app::buildMenuBar(), commands), {});
    {
        app::ShellController* shellPtr = shell.get();
        platform::LinuxMenuBar::Callbacks callbacks;
        callbacks.isEnabled = [shellPtr, &commands](int id) {
            return id >= 0 && static_cast<std::size_t>(id) < commands.size() &&
                   shellPtr->canPerform(commands[static_cast<std::size_t>(id)]);
        };
        callbacks.isChecked = [shellPtr, &commands](int id) -> std::optional<bool> {
            if (id < 0 || static_cast<std::size_t>(id) >= commands.size()) return std::nullopt;
            return shellPtr->isChecked(commands[static_cast<std::size_t>(id)]);
        };
        callbacks.activate = [shellPtr, &commands](int id) {
            if (id >= 0 && static_cast<std::size_t>(id) < commands.size()) {
                shellPtr->perform(commands[static_cast<std::size_t>(id)]);
            }
        };
        menuBar.setCallbacks(std::move(callbacks));
    }
    shell->setQuitHandler(requestQuit);

    window->setMenuBar(&menuBar);
    window->setRootWidget(&shell->rootWidget());
    window->setKeyHandler([shellPtr = shell.get()](const ui::KeyEvent& event) {
        return shellPtr->handleKeyEvent(event);
    });
    window->setFileDropHandler([shellPtr = shell.get()](const std::vector<std::filesystem::path>& paths) {
        for (const auto& path : paths) shellPtr->openDocument(path);
    });
    window->setCloseHandler([&running, hooks = lifecycle.get()] {
        // windowShouldClose: -> the app's unsaved-changes prompt. Rivet has
        // one window, so an accepted close ends the app.
        if (hooks->dispatchCloseRequest()) running = false;
    });
    connection->setSignalQuitHandler(requestQuit);
    connection->installSignalHandlers();

    window->show();

    // Open-on-launch: every non-option argument (PDF or Markdown by
    // extension); failures surface in the shell's status label.
    for (int i = 1; i < argc; ++i) {
        if (argv[i] != nullptr && argv[i][0] != '-') shell->openDocument(std::filesystem::path(argv[i]));
    }

    if (qa != nullptr) {
        // The script ends the loop itself (no quit prompts).
        qa->start(*shell, services, [&running](int) { running = false; });
    }

    while (running) connection->pump(platform::X11Connection::Mode::Normal, nullptr, -1, -1);

    // Teardown, in main.mm's order: detach every borrower of the widget tree
    // and every handler that captures shell state, then destroy the shell
    // (workspace shutdown waits for background opens, the sessions drain
    // their worker streams, the scheduler joins). Main-thread tasks still
    // queued are dropped with the dispatcher, after the shell is gone.
    menuBar.setCallbacks({});
    window->setMenuBar(nullptr);
    window->setFileDropHandler(nullptr);
    window->setCloseHandler(nullptr);
    connection->setSignalQuitHandler(nullptr);
    shell->setQuitHandler(nullptr);
    lifecycle->clearHandlers();
    window->setKeyHandler(nullptr);
    window->setRootWidget(nullptr);
    shell.reset();
    window.reset();
    return qa != nullptr ? qa->exitCode() : 0;
}
