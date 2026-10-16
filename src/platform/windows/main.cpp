// SPDX-License-Identifier: MPL-2.0
//
// Composition root of the Windows (Win32 + Direct2D) desktop shell: the
// counterpart of src/platform/macos/main.mm and src/platform/linux/main.cpp.
// It wires the platform services into app::createShell(), renders
// app::buildMenuBar() as a native HMENU, runs the message loop and tears
// everything down in the same order as macOS.

#include "app/MenuModel.hpp"
#include "app/ShellController.hpp"
#include "platform/AppLifecycle.hpp"
#include "platform/PlatformKit.hpp"
#include "ui/UiTypes.hpp"

#include "WindowsAlertService.hpp"
#include "WindowsAppDataLocation.hpp"
#include "WindowsClipboard.hpp"
#include "WindowsExternalUrlOpener.hpp"
#include "WindowsFileDialog.hpp"
#include "WindowsImageDecoder.hpp"
#include "WindowsMainThreadDispatcher.hpp"
#include "WindowsMenuBar.hpp"
#include "WindowsPrintService.hpp"
#include "WindowsTextShaper.hpp"
#include "WindowsWindow.hpp"

#include "WinCommon.hpp" // last: <windows.h> macros

#include <objbase.h>
#include <shellapi.h>

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

// app::buildMenuBar() -> the platform menu model. Item ids are flat indices
// into `commands` (the same scheme the macOS menu uses for item tags).
std::vector<rivet::platform::WinMenu> convertMenus(const std::vector<rivet::app::Menu>& model,
                                                   std::vector<rivet::app::AppCommand>& commands) {
    std::vector<rivet::platform::WinMenu> menus;
    for (const rivet::app::Menu& menu : model) {
        rivet::platform::WinMenu out;
        out.title = menu.title;
        for (const rivet::app::MenuItem& entry : menu.items) {
            rivet::platform::WinMenuItem item;
            // Windows names the quit command File > Exit.
            item.title = entry.role == rivet::app::MenuRole::Quit ? std::string("Exit") : entry.title;
            item.id = static_cast<int>(commands.size());
            item.separatorBefore = entry.separatorBefore;
            item.checkable = entry.checkable;
            if (entry.shortcut.has_value()) {
                item.shortcut = rivet::platform::WinMenuShortcut{entry.shortcut->key, entry.shortcut->primary,
                                                                 entry.shortcut->shift, entry.shortcut->alt};
            }
            commands.push_back(entry.command);
            out.items.push_back(std::move(item));
        }
        menus.push_back(std::move(out));
    }
    return menus;
}

// Command-line arguments as UTF-16 (independent of the ANSI code page).
std::vector<std::filesystem::path> documentArguments() {
    std::vector<std::filesystem::path> paths;
    int count = 0;
    LPWSTR* arguments = ::CommandLineToArgvW(::GetCommandLineW(), &count);
    if (arguments == nullptr) return paths;
    for (int i = 1; i < count; ++i) {
        if (arguments[i] != nullptr && arguments[i][0] != L'\0' && arguments[i][0] != L'-') {
            paths.emplace_back(arguments[i]);
        }
    }
    ::LocalFree(arguments);
    return paths;
}

} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int showCommand) {
    using namespace rivet;

    // The manifest declares per-monitor-v2 DPI awareness; this covers a
    // build whose manifest was stripped (ignored when already set).
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // STA for the shell's common dialogs, drag & drop and ShellExecute.
    const platform::win::ComScope com(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    platform::text::initialize();

    // Services must outlive the app shell and the message loop.
    const auto dispatcher = std::make_unique<platform::WindowsMainThreadDispatcher>();

    platform::WindowsWindow::Options windowOptions;
    windowOptions.title = "Rivet";
    windowOptions.logicalSize = core::Size{1280.0, 850.0};
    windowOptions.minLogicalSize = core::Size{940.0, 600.0};
    auto window = std::make_unique<platform::WindowsWindow>(windowOptions);
    if (!window->valid()) {
        ::MessageBoxW(nullptr, L"Rivet could not create its window.", L"Rivet", MB_ICONERROR | MB_OK);
        return 1;
    }
    HWND hwnd = window->hwnd();

    const auto alertService = std::make_unique<platform::WindowsAlertService>(hwnd, dispatcher.get());
    const auto fileDialog =
        std::make_unique<platform::WindowsFileDialog>(hwnd, dispatcher.get(), alertService.get());
    const auto imageDecoder = std::make_unique<platform::WindowsImageDecoder>();
    const auto appData = std::make_unique<platform::WindowsAppDataLocation>();
    const auto clipboard = std::make_unique<platform::WindowsClipboard>(hwnd);
    const auto urlOpener = std::make_unique<platform::WindowsExternalUrlOpener>();
    const auto printService = std::make_unique<platform::WindowsPrintService>(hwnd, dispatcher.get());
    const auto lifecycle = std::make_unique<platform::LifecycleHooks>();

    platform::ShellServices services;
    services.redrawSink = window->redrawSink();
    services.mainDispatcher = dispatcher.get();
    services.fileDialog = fileDialog.get();
    services.imageDecoder = imageDecoder.get();
    services.appData = appData.get();
    services.clipboard = clipboard.get();
    services.urlOpener = urlOpener.get();
    services.printService = printService.get();
    services.saveDialog = fileDialog.get();
    services.alerts = alertService.get();
    services.lifecycle = lifecycle.get();
    platform::WindowsWindow* const windowPtr = window.get();
    services.setWindowTitle = [windowPtr](const std::string& title) { windowPtr->setTitle(title); };
    services.setDocumentEdited = [windowPtr](bool edited) { windowPtr->setDocumentEdited(edited); };

    // Message-loop state; quit decisions land here.
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
    platform::WindowsMenuBar menuBar(convertMenus(app::buildMenuBar(), commands));
    {
        app::ShellController* shellPtr = shell.get();
        platform::WindowsMenuBar::Callbacks callbacks;
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

    window->show(showCommand);

    // Open-on-launch: every non-option argument (PDF or Markdown by
    // extension); failures surface in the shell's status label.
    for (const auto& path : documentArguments()) shell->openDocument(path);

    MSG message;
    while (running) {
        const BOOL got = ::GetMessageW(&message, nullptr, 0, 0);
        if (got == 0 || got == -1) break; // WM_QUIT or an invalid loop state
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }

    // Teardown, in main.mm's order: detach every borrower of the widget tree
    // and every handler that captures shell state, then destroy the shell
    // (workspace shutdown waits for background opens, the sessions drain
    // their worker streams, the scheduler joins). Main-thread tasks still
    // queued are dropped with the dispatcher, after the shell is gone.
    menuBar.setCallbacks({});
    window->setMenuBar(nullptr);
    window->setFileDropHandler(nullptr);
    window->setCloseHandler(nullptr);
    shell->setQuitHandler(nullptr);
    lifecycle->clearHandlers();
    window->setKeyHandler(nullptr);
    window->setRootWidget(nullptr);
    shell.reset();
    window.reset();
    platform::text::shutdown();
    return 0;
}
