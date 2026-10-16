// SPDX-License-Identifier: MPL-2.0
#pragma once

// Shared Win32 plumbing for the Windows desktop backend: the one place that
// includes <windows.h> with Rivet's macro hygiene, a minimal COM smart
// pointer and UTF-8 <-> UTF-16 conversion. Only src/platform/windows
// includes this header.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
// COM basics (COINIT_*, CoTaskMemFree); also unlocks the COM-based parts
// of later headers such as PRINTDLGEX in <commdlg.h>.
#include <objbase.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace rivet::platform::win {

// UTF-8 -> UTF-16. Invalid sequences become U+FFFD.
std::wstring widen(std::string_view utf8);
// UTF-16 -> UTF-8. Unpaired surrogates become U+FFFD.
std::string narrow(std::wstring_view utf16);

// Minimal intrusive COM pointer (no WRL/ATL dependency).
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(std::nullptr_t) {}
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr& other) : ptr_(other.ptr_) {
        if (ptr_ != nullptr) ptr_->AddRef();
    }
    ComPtr(ComPtr&& other) noexcept : ptr_(std::exchange(other.ptr_, nullptr)) {}
    ComPtr& operator=(ComPtr other) noexcept {
        std::swap(ptr_, other.ptr_);
        return *this;
    }

    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    // Releases the current object and returns the slot for an out-param.
    T** put() {
        reset();
        return &ptr_;
    }
    void** putVoid() { return reinterpret_cast<void**>(put()); }

    void reset() {
        if (ptr_ != nullptr) std::exchange(ptr_, nullptr)->Release();
    }

    template <typename U>
    ComPtr<U> as(const IID& iid) const {
        ComPtr<U> result;
        if (ptr_ != nullptr) ptr_->QueryInterface(iid, result.putVoid());
        return result;
    }

private:
    T* ptr_ = nullptr;
};

// CoInitializeEx for the current scope; balances only a successful init.
// RPC_E_CHANGED_MODE (the thread already chose another apartment) is fine.
class ComScope {
public:
    explicit ComScope(DWORD model = COINIT_MULTITHREADED);
    ~ComScope();
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

private:
    bool balance_ = false;
};

} // namespace rivet::platform::win
