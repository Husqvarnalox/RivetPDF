// SPDX-License-Identifier: MPL-2.0
#include "WindowsClipboard.hpp"

#include "WinCommon.hpp"

#include <cstring>

namespace rivet::platform {

namespace {

// Another process may hold the clipboard briefly; retry for ~100 ms.
bool openClipboard(HWND owner) {
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (::OpenClipboard(owner)) return true;
        ::Sleep(10);
    }
    return false;
}

core::Error clipboardError(const char* message) {
    return core::makeError(core::ErrorCode::NotAvailable, message, "platform.windows.clipboard");
}

} // namespace

core::Status WindowsClipboard::setText(const std::string& text) {
    std::string crlf;
    crlf.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) crlf += '\r';
        crlf += text[i];
    }
    const std::wstring wide = win::widen(crlf);
    if (wide.empty() && !text.empty()) {
        return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument, "text is not valid UTF-8",
                                               "platform.windows.clipboard"));
    }

    const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) return std::unexpected(clipboardError("cannot allocate clipboard memory"));
    void* locked = ::GlobalLock(memory);
    if (locked == nullptr) {
        ::GlobalFree(memory);
        return std::unexpected(clipboardError("cannot allocate clipboard memory"));
    }
    std::memcpy(locked, wide.c_str(), bytes);
    ::GlobalUnlock(memory);

    if (!openClipboard(owner_)) {
        ::GlobalFree(memory);
        return std::unexpected(clipboardError("the clipboard is in use by another program"));
    }
    ::EmptyClipboard();
    const bool stored = ::SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
    ::CloseClipboard();
    if (!stored) {
        ::GlobalFree(memory); // ownership passes to the system only on success
        return std::unexpected(clipboardError("cannot write to the clipboard"));
    }
    return core::ok();
}

std::string WindowsClipboard::text() const {
    if (!::IsClipboardFormatAvailable(CF_UNICODETEXT)) return {};
    if (!openClipboard(owner_)) return {};
    std::wstring wide;
    if (HANDLE data = ::GetClipboardData(CF_UNICODETEXT); data != nullptr) {
        if (const auto* chars = static_cast<const wchar_t*>(::GlobalLock(data)); chars != nullptr) {
            // Bounded by the allocation: the text need not be terminated.
            const std::size_t capacity = ::GlobalSize(data) / sizeof(wchar_t);
            std::size_t length = 0;
            while (length < capacity && chars[length] != L'\0') ++length;
            wide.assign(chars, length);
            ::GlobalUnlock(data);
        }
    }
    ::CloseClipboard();

    std::string utf8 = win::narrow(wide);
    std::string out;
    out.reserve(utf8.size());
    for (std::size_t i = 0; i < utf8.size(); ++i) {
        if (utf8[i] == '\r' && i + 1 < utf8.size() && utf8[i + 1] == '\n') continue;
        out += utf8[i];
    }
    return out;
}

} // namespace rivet::platform
