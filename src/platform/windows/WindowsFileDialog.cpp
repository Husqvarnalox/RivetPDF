// SPDX-License-Identifier: MPL-2.0
#include "WindowsFileDialog.hpp"

#include "WinCommon.hpp"
#include "WindowsAlertService.hpp"
#include "WindowsMainThreadDispatcher.hpp"

#include <objbase.h>
#include <shobjidl.h>

#include <algorithm>
#include <cwctype>

namespace rivet::platform {

namespace {

constexpr const char* kSubsystem = "platform.windows.dialog";

core::Error dialogError(core::ErrorCode code, const char* message) {
    return core::makeError(code, message, kSubsystem);
}

std::wstring lowerWide(std::wstring text) {
    for (wchar_t& c : text) c = static_cast<wchar_t>(std::towlower(c));
    return text;
}

std::filesystem::path itemPath(IShellItem* item) {
    PWSTR raw = nullptr;
    if (item == nullptr || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || raw == nullptr) return {};
    std::filesystem::path path(raw);
    ::CoTaskMemFree(raw);
    return path;
}

std::wstring describeExtension(const std::string& extension) {
    if (extension == "pdf") return L"PDF document";
    if (extension == "md" || extension == "markdown" || extension == "mdown") return L"Markdown document";
    if (extension == "png") return L"PNG image";
    if (extension == "jpg" || extension == "jpeg") return L"JPEG image";
    std::wstring upper = win::widen(extension);
    for (wchar_t& c : upper) c = static_cast<wchar_t>(std::towupper(c));
    return upper + L" file";
}

} // namespace

core::Result<std::vector<std::filesystem::path>> WindowsFileDialog::runOpen(const std::string& title,
                                                                            const std::string& prompt,
                                                                            const std::vector<Filter>& filters,
                                                                            bool multiple) {
    win::ComPtr<IFileOpenDialog> dialog;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IFileOpenDialog),
                                  dialog.putVoid()))) {
        return std::unexpected(dialogError(core::ErrorCode::NotAvailable, "the open dialog is unavailable"));
    }

    // COMDLG_FILTERSPEC points into these strings: keep them alive.
    std::vector<std::wstring> specs;
    specs.reserve(filters.size());
    for (const Filter& filter : filters) {
        std::wstring joined;
        for (const std::wstring& pattern : filter.patterns) {
            if (!joined.empty()) joined += L';';
            joined += pattern;
        }
        specs.push_back(std::move(joined));
    }
    std::vector<COMDLG_FILTERSPEC> filterSpecs;
    for (std::size_t i = 0; i < filters.size(); ++i) filterSpecs.push_back({filters[i].name.c_str(), specs[i].c_str()});
    if (!filterSpecs.empty()) dialog->SetFileTypes(static_cast<UINT>(filterSpecs.size()), filterSpecs.data());

    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
    if (multiple) options |= FOS_ALLOWMULTISELECT;
    dialog->SetOptions(options);
    if (!title.empty()) dialog->SetTitle(win::widen(title).c_str());
    if (!prompt.empty()) dialog->SetOkButtonLabel(win::widen(prompt).c_str());

    HRESULT shown = E_FAIL;
    {
        const WindowsMainThreadDispatcher::ModalScope modal(dispatcher_);
        shown = dialog->Show(owner_);
    }
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return std::unexpected(dialogError(core::ErrorCode::Cancelled, "user dismissed the open dialog"));
    }
    if (FAILED(shown)) return std::unexpected(dialogError(core::ErrorCode::Internal, "the open dialog failed"));

    std::vector<std::filesystem::path> paths;
    win::ComPtr<IShellItemArray> results;
    if (SUCCEEDED(dialog->GetResults(results.put())) && results) {
        DWORD count = 0;
        results->GetCount(&count);
        for (DWORD i = 0; i < count; ++i) {
            win::ComPtr<IShellItem> item;
            if (SUCCEEDED(results->GetItemAt(i, item.put()))) {
                std::filesystem::path path = itemPath(item.get());
                if (!path.empty()) paths.push_back(std::move(path));
            }
        }
    }
    if (paths.empty()) {
        return std::unexpected(dialogError(core::ErrorCode::NotFound, "the chosen item is not a file on disk"));
    }
    return paths;
}

core::Result<std::filesystem::path> WindowsFileDialog::openPdf() {
    auto paths = runOpen("Open PDF", "Open", {{L"PDF documents", {L"*.pdf"}}}, false);
    if (!paths) return std::unexpected(paths.error());
    return paths->front();
}

core::Result<std::filesystem::path> WindowsFileDialog::openDocument() {
    auto paths = runOpen("Open", "Open",
                         {{L"PDF and Markdown documents", {L"*.pdf", L"*.md", L"*.markdown", L"*.mdown"}},
                          {L"PDF documents", {L"*.pdf"}},
                          {L"Markdown documents", {L"*.md", L"*.markdown", L"*.mdown"}}},
                         false);
    if (!paths) return std::unexpected(paths.error());
    return paths->front();
}

core::Result<std::vector<std::filesystem::path>> WindowsFileDialog::openPdfs(const OpenOptions& options) {
    return runOpen(options.title, options.prompt, {{L"PDF documents", {L"*.pdf"}}}, options.allowMultiple);
}

core::Result<std::filesystem::path> WindowsFileDialog::openImage() {
    auto paths = runOpen("Choose Image", "Choose", {{L"Images (PNG, JPEG)", {L"*.png", L"*.jpg", L"*.jpeg"}}}, false);
    if (!paths) return std::unexpected(paths.error());
    return paths->front();
}

std::optional<std::filesystem::path> WindowsFileDialog::runSavePanel(const ISaveDialog::Options& options) {
    std::vector<std::string> extensions = options.allowedExtensions;
    if (extensions.empty()) extensions.push_back("pdf");

    win::ComPtr<IFileSaveDialog> dialog;
    if (FAILED(::CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IFileSaveDialog),
                                  dialog.putVoid()))) {
        return std::nullopt;
    }

    std::vector<std::wstring> names;
    std::vector<std::wstring> patterns;
    for (const std::string& extension : extensions) {
        names.push_back(describeExtension(extension) + L" (*." + win::widen(extension) + L")");
        patterns.push_back(L"*." + win::widen(extension));
    }
    std::vector<COMDLG_FILTERSPEC> specs;
    for (std::size_t i = 0; i < extensions.size(); ++i) specs.push_back({names[i].c_str(), patterns[i].c_str()});
    dialog->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(win::widen(extensions.front()).c_str());

    FILEOPENDIALOGOPTIONS flags = 0;
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST);
    if (!options.title.empty()) dialog->SetTitle(win::widen(options.title).c_str());
    if (!options.prompt.empty()) dialog->SetOkButtonLabel(win::widen(options.prompt).c_str());
    if (!options.suggestedName.empty()) dialog->SetFileName(win::widen(options.suggestedName).c_str());
    if (!options.initialDirectory.empty()) {
        win::ComPtr<IShellItem> folder;
        if (SUCCEEDED(::SHCreateItemFromParsingName(options.initialDirectory.c_str(), nullptr, __uuidof(IShellItem),
                                                    folder.putVoid()))) {
            dialog->SetFolder(folder.get());
        }
    }

    HRESULT shown = E_FAIL;
    {
        const WindowsMainThreadDispatcher::ModalScope modal(dispatcher_);
        shown = dialog->Show(owner_);
    }
    if (FAILED(shown)) return std::nullopt; // cancelled (or failed: nothing chosen)

    win::ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.put()))) return std::nullopt;
    std::filesystem::path path = itemPath(item.get());
    if (path.empty()) return std::nullopt;

    // The dialog appends the default extension to bare names; a name typed
    // with another extension gets the default one appended here.
    const std::wstring extension = lowerWide(path.extension().wstring());
    const bool allowed = std::any_of(extensions.begin(), extensions.end(), [&](const std::string& candidate) {
        return extension == L"." + lowerWide(win::widen(candidate));
    });
    if (!allowed) {
        path += L"." + win::widen(extensions.front());
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            if (alerts_ == nullptr) return std::nullopt;
            const std::string name = win::narrow(path.filename().wstring());
            if (!alerts_->confirm("\xE2\x80\x9C" + name + "\xE2\x80\x9D already exists. Do you want to replace it?",
                                  "A file with the same name already exists. Replacing it will overwrite its "
                                  "current contents.",
                                  "Replace")) {
                return std::nullopt;
            }
        }
    }
    return path;
}

} // namespace rivet::platform
