// SPDX-License-Identifier: MPL-2.0
#pragma once

// PRIVATE: run-time loaded PKCS#11 module, RAII session and error mapping.
// Only the PKCS#11 provider sources include this header.

#include "crypto/Error.hpp"
#include "crypto/Types.hpp"
#include "crypto/providers/pkcs11/Pkcs11Abi.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::crypto::p11 {

// "PKCS#11 error 0x00000030" (a number, never a secret).
std::string rvText(CK_RV rv);
// Typed mapping used for every failure that has no more specific meaning.
CryptoError mapRv(CK_RV rv, std::string_view what);
// Fixed-width, blank-padded PKCS#11 text field -> trimmed std::string.
std::string trimField(const unsigned char* data, std::size_t size);

class Pkcs11Module {
public:
    // Loads (dlopen / LoadLibrary), resolves C_GetFunctionList (the only
    // symbol looked up by name) and calls C_Initialize. One instance per
    // library path is shared process-wide while somebody holds it, so
    // C_Initialize / C_Finalize are paired exactly once.
    //
    // Before loading, the file must pass crypto::checkModuleFile (absolute, regular
    // file after symlink resolution, owned by root / the current user, not
    // group/world-writable, parent directory not world-writable). allowInsecurePath
    // is the explicit development override (see Pkcs11Options).
    static CryptoResult<std::shared_ptr<Pkcs11Module>> acquire(const std::filesystem::path& path,
                                                               bool allowInsecurePath = false);

    ~Pkcs11Module();
    Pkcs11Module(const Pkcs11Module&) = delete;
    Pkcs11Module& operator=(const Pkcs11Module&) = delete;

    // Calls one function-list entry. The per-module gate serialises calls
    // into the module (many vendor modules are not thread safe in practice
    // even with OS locking) and is held for the duration of this single call
    // only: never across a PinPrompt or any other callback. A NULL entry
    // yields CKR_FUNCTION_NOT_SUPPORTED, never a null call.
    template <typename Fn, typename... Args>
    CK_RV call(Fn CK_FUNCTION_LIST::*entry, Args... args) const {
        Fn function = list_ ? list_->*entry : nullptr;
        if (!function) return CKR_FUNCTION_NOT_SUPPORTED;
        std::lock_guard lock(gate_);
        return function(args...);
    }

    template <typename Fn>
    bool has(Fn CK_FUNCTION_LIST::*entry) const {
        return list_ && (list_->*entry) != nullptr;
    }

private:
    Pkcs11Module() = default;
    CryptoOk init(const std::filesystem::path& path);

    void* handle_ = nullptr;
    CK_FUNCTION_LIST* list_ = nullptr;
    bool initialized_ = false; // this instance called C_Initialize successfully
    bool registered_ = false;  // present in the process-wide cache
    std::string key_;
    mutable std::mutex gate_;
};

// One PKCS#11 session, closed on destruction. Not thread safe by itself.
class Pkcs11Session {
public:
    Pkcs11Session() = default;
    ~Pkcs11Session();
    Pkcs11Session(Pkcs11Session&& other) noexcept;
    Pkcs11Session& operator=(Pkcs11Session&& other) noexcept;
    Pkcs11Session(const Pkcs11Session&) = delete;
    Pkcs11Session& operator=(const Pkcs11Session&) = delete;

    // Opens a read-only serial session on `slot`.
    static CK_RV open(const std::shared_ptr<Pkcs11Module>& module, CK_SLOT_ID slot, Pkcs11Session& out);

    bool valid() const { return handle_ != 0; }
    CK_SESSION_HANDLE handle() const { return handle_; }
    const std::shared_ptr<Pkcs11Module>& module() const { return module_; }

    // Up to `max` object handles matching the template.
    CK_RV find(std::vector<CK_ATTRIBUTE> templ, std::size_t max, std::vector<CK_OBJECT_HANDLE>& out) const;
    // Attribute readers; CKR_ATTRIBUTE_TYPE_INVALID when the value is
    // unavailable, CKR_BUFFER_TOO_SMALL when it exceeds `maxLen`.
    CK_RV getBytes(CK_OBJECT_HANDLE object, CK_ATTRIBUTE_TYPE type, Bytes& out, std::size_t maxLen) const;
    CK_RV getUlong(CK_OBJECT_HANDLE object, CK_ATTRIBUTE_TYPE type, CK_ULONG& out) const;
    CK_RV getBool(CK_OBJECT_HANDLE object, CK_ATTRIBUTE_TYPE type, bool& out) const;

    void close();

private:
    std::shared_ptr<Pkcs11Module> module_;
    CK_SESSION_HANDLE handle_ = 0;
};

} // namespace rivet::crypto::p11
