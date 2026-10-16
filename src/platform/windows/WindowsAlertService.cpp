// SPDX-License-Identifier: MPL-2.0
#include "WindowsAlertService.hpp"

#include "WinCommon.hpp"
#include "WindowsMainThreadDispatcher.hpp"

#include <commctrl.h>

#include <cstdint>
#include <string>
#include <vector>

namespace rivet::platform {

namespace {

constexpr int kFirst = 100;
constexpr int kSecond = 101;

struct DialogButton {
    int id = 0;
    std::wstring label;
};

struct DialogSpec {
    std::wstring instruction; // bold main line
    std::wstring content;     // explanatory text
    PCWSTR icon = nullptr;    // TD_WARNING_ICON, ...
    std::vector<DialogButton> buttons;
    bool cancelButton = false; // adds the standard Cancel (IDCANCEL, Esc)
    int defaultId = 0;
};

using TaskDialogIndirectFn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);

TaskDialogIndirectFn taskDialogIndirect() {
    // Resolved at run time: only common controls v6 (selected by the app
    // manifest) exports it, and a hard import would keep the executable
    // from loading without the manifest.
    static const TaskDialogIndirectFn fn = [] {
        HMODULE module = ::LoadLibraryW(L"comctl32.dll");
        return module != nullptr
                   ? reinterpret_cast<TaskDialogIndirectFn>(
                         reinterpret_cast<void*>(::GetProcAddress(module, "TaskDialogIndirect")))
                   : nullptr;
    }();
    return fn;
}

// Returns the pressed button id; IDCANCEL for Esc / the close button.
int runDialog(HWND owner, WindowsMainThreadDispatcher* dispatcher, const DialogSpec& spec) {
    const WindowsMainThreadDispatcher::ModalScope modal(dispatcher);
    if (const TaskDialogIndirectFn fn = taskDialogIndirect(); fn != nullptr) {
        std::vector<TASKDIALOG_BUTTON> buttons;
        for (const DialogButton& button : spec.buttons) buttons.push_back({button.id, button.label.c_str()});
        TASKDIALOGCONFIG config{};
        config.cbSize = sizeof config;
        config.hwndParent = owner;
        config.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | (spec.cancelButton ? TDF_ALLOW_DIALOG_CANCELLATION : 0);
        config.dwCommonButtons = spec.cancelButton ? TDCBF_CANCEL_BUTTON : 0;
        config.pszWindowTitle = L"Rivet";
        config.pszMainIcon = spec.icon;
        config.pszMainInstruction = spec.instruction.c_str();
        config.pszContent = spec.content.empty() ? nullptr : spec.content.c_str();
        config.cButtons = static_cast<UINT>(buttons.size());
        config.pButtons = buttons.empty() ? nullptr : buttons.data();
        config.nDefaultButton = spec.defaultId;
        int pressed = IDCANCEL;
        if (SUCCEEDED(fn(&config, &pressed, nullptr, nullptr))) return pressed;
    }

    // Fallback without common controls v6: a MessageBox whose Yes/No/OK
    // stand for the custom buttons in order.
    std::wstring text = spec.instruction;
    if (!spec.content.empty()) text += L"\n\n" + spec.content;
    UINT type = MB_TASKMODAL | MB_ICONWARNING;
    const std::size_t count = spec.buttons.size();
    if (count >= 2) {
        text += L"\n\nYes: " + spec.buttons[0].label + L"\nNo: " + spec.buttons[1].label;
        type |= spec.cancelButton ? MB_YESNOCANCEL : MB_YESNO;
    } else if (count == 1) {
        type |= spec.cancelButton ? MB_OKCANCEL : MB_OK;
    } else {
        type |= MB_OK;
    }
    const int answer = ::MessageBoxW(owner, text.c_str(), L"Rivet", type);
    switch (answer) {
    case IDYES:
    case IDOK: return count >= 1 ? spec.buttons[0].id : IDOK;
    case IDNO: return count >= 2 ? spec.buttons[1].id : IDCANCEL;
    default: return IDCANCEL;
    }
}

// ---- promptForText: in-memory dialog template -----------------------------

constexpr WORD kEditId = 1001;
constexpr WORD kMessageId = 1002;

struct PromptState {
    std::wstring title;
    std::wstring message;
    std::wstring text;
};

class TemplateBuilder {
public:
    void word(WORD value) { data_.push_back(value); }
    void dword(DWORD value) {
        word(static_cast<WORD>(value & 0xFFFF));
        word(static_cast<WORD>(value >> 16));
    }
    void string(const wchar_t* text) {
        for (; *text != L'\0'; ++text) word(static_cast<WORD>(*text));
        word(0);
    }
    void alignDword() {
        if (data_.size() % 2 != 0) word(0);
    }
    // DLGITEMTEMPLATE + class atom + title + no creation data.
    void item(DWORD style, short x, short y, short cx, short cy, WORD id, WORD classAtom, const wchar_t* text) {
        alignDword();
        dword(style | WS_CHILD | WS_VISIBLE);
        dword(0); // extended style
        word(static_cast<WORD>(x));
        word(static_cast<WORD>(y));
        word(static_cast<WORD>(cx));
        word(static_cast<WORD>(cy));
        word(id);
        word(0xFFFF);
        word(classAtom);
        string(text);
        word(0);
    }
    const DLGTEMPLATE* get() {
        alignDword();
        return reinterpret_cast<const DLGTEMPLATE*>(data_.data());
    }

private:
    std::vector<WORD> data_;
};

INT_PTR CALLBACK promptProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG: {
        auto* state = reinterpret_cast<PromptState*>(lParam);
        ::SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        ::SetWindowTextW(dialog, state->title.c_str());
        ::SetDlgItemTextW(dialog, kMessageId, state->message.c_str());
        HWND edit = ::GetDlgItem(dialog, kEditId);
        ::SetWindowTextW(edit, state->text.c_str());
        ::SendMessageW(edit, EM_SETSEL, 0, -1);
        ::SetFocus(edit);
        return FALSE; // focus set explicitly
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            auto* state = reinterpret_cast<PromptState*>(::GetWindowLongPtrW(dialog, DWLP_USER));
            HWND edit = ::GetDlgItem(dialog, kEditId);
            const int length = ::GetWindowTextLengthW(edit);
            std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
            const int copied = ::GetWindowTextW(edit, text.data(), length + 1);
            text.resize(static_cast<std::size_t>(copied > 0 ? copied : 0));
            if (state != nullptr) state->text = std::move(text);
            ::EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            ::EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;
    default: break;
    }
    return FALSE;
}

} // namespace

SaveChangesChoice WindowsAlertService::askSaveChanges(std::string_view documentTitle) {
    DialogSpec spec;
    spec.instruction = L"Do you want to save the changes made to the document “" + win::widen(documentTitle) +
                       L"”?";
    spec.content = L"Your changes will be lost if you don’t save them.";
    spec.icon = TD_WARNING_ICON;
    spec.buttons = {{kFirst, L"&Save"}, {kSecond, L"Do&n’t Save"}};
    spec.cancelButton = true;
    spec.defaultId = kFirst;
    switch (runDialog(owner_, dispatcher_, spec)) {
    case kFirst: return SaveChangesChoice::Save;
    case kSecond: return SaveChangesChoice::DontSave;
    default: return SaveChangesChoice::Cancel;
    }
}

ReviewChangesChoice WindowsAlertService::askReviewUnsavedChanges(std::size_t unsavedDocumentCount) {
    DialogSpec spec;
    spec.instruction = L"You have " + std::to_wstring(unsavedDocumentCount) +
                       L" documents with unsaved changes. Do you want to review these changes before quitting?";
    spec.content = L"If you don’t review your documents, all your changes will be lost.";
    spec.icon = TD_WARNING_ICON;
    spec.buttons = {{kFirst, L"&Review Changes…"}, {kSecond, L"&Discard Changes"}};
    spec.cancelButton = true;
    spec.defaultId = kFirst;
    switch (runDialog(owner_, dispatcher_, spec)) {
    case kFirst: return ReviewChangesChoice::Review;
    case kSecond: return ReviewChangesChoice::DiscardAll;
    default: return ReviewChangesChoice::Cancel;
    }
}

void WindowsAlertService::showError(std::string_view title, std::string_view message) {
    DialogSpec spec;
    spec.instruction = win::widen(title);
    spec.content = win::widen(message);
    spec.icon = TD_WARNING_ICON;
    spec.buttons = {{kFirst, L"OK"}};
    spec.defaultId = kFirst;
    (void)runDialog(owner_, dispatcher_, spec);
}

bool WindowsAlertService::confirm(std::string_view title, std::string_view message, std::string_view confirmLabel) {
    DialogSpec spec;
    spec.instruction = win::widen(title);
    spec.content = win::widen(message);
    spec.buttons = {{kFirst, win::widen(confirmLabel)}};
    spec.cancelButton = true;
    spec.defaultId = kFirst;
    return runDialog(owner_, dispatcher_, spec) == kFirst;
}

std::optional<std::string> WindowsAlertService::promptForText(std::string_view title, std::string_view message,
                                                              std::string_view defaultText) {
    PromptState state;
    state.title = title.empty() ? std::wstring(L"Rivet") : win::widen(title);
    state.message = win::widen(message);
    state.text = win::widen(defaultText);

    // Dialog units; the system scales them with the dialog font and DPI.
    TemplateBuilder builder;
    builder.dword(DS_SETFONT | DS_MODALFRAME | DS_CENTER | DS_SHELLFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    builder.dword(0); // extended style
    builder.word(4);  // item count
    builder.word(0);  // x
    builder.word(0);  // y
    builder.word(260);
    builder.word(86);
    builder.word(0);      // no menu
    builder.word(0);      // default dialog class
    builder.string(L""); // title set in WM_INITDIALOG
    builder.word(9);      // point size
    builder.string(L"Segoe UI");
    builder.item(SS_LEFT | SS_NOPREFIX, 7, 7, 246, 26, kMessageId, 0x0082, L"");
    builder.item(ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 7, 38, 246, 14, kEditId, 0x0081, L"");
    builder.item(BS_DEFPUSHBUTTON | WS_TABSTOP, 149, 62, 50, 14, IDOK, 0x0080, L"OK");
    builder.item(BS_PUSHBUTTON | WS_TABSTOP, 203, 62, 50, 14, IDCANCEL, 0x0080, L"Cancel");

    const WindowsMainThreadDispatcher::ModalScope modal(dispatcher_);
    const INT_PTR result = ::DialogBoxIndirectParamW(::GetModuleHandleW(nullptr), builder.get(), owner_, promptProc,
                                                     reinterpret_cast<LPARAM>(&state));
    if (result != IDOK) return std::nullopt;
    return win::narrow(state.text);
}

} // namespace rivet::platform
