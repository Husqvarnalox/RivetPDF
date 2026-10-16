// SPDX-License-Identifier: MPL-2.0
#include "WinCommon.hpp"

#include <objbase.h>

#include <climits>

namespace rivet::platform::win {

std::wstring widen(std::string_view utf8) {
    if (utf8.empty() || utf8.size() > static_cast<std::size_t>(INT_MAX)) return {};
    const int length = static_cast<int>(utf8.size());
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), length, nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), length, out.data(), needed);
    return out;
}

std::string narrow(std::wstring_view utf16) {
    if (utf16.empty() || utf16.size() > static_cast<std::size_t>(INT_MAX)) return {};
    const int length = static_cast<int>(utf16.size());
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, utf16.data(), length, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, utf16.data(), length, out.data(), needed, nullptr, nullptr);
    return out;
}

ComScope::ComScope(DWORD model) {
    const HRESULT hr = ::CoInitializeEx(nullptr, model);
    balance_ = SUCCEEDED(hr); // S_OK or S_FALSE both need a CoUninitialize
}

ComScope::~ComScope() {
    if (balance_) ::CoUninitialize();
}

} // namespace rivet::platform::win
