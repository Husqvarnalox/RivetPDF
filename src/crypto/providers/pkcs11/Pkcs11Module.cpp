// SPDX-License-Identifier: MPL-2.0
#include "crypto/providers/pkcs11/Pkcs11Module.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace rivet::crypto::p11 {

namespace {

std::mutex& cacheMutex() {
    static std::mutex m;
    return m;
}

std::map<std::string, std::weak_ptr<Pkcs11Module>>& cache() {
    static std::map<std::string, std::weak_ptr<Pkcs11Module>> c;
    return c;
}

#if defined(_WIN32)
void* openLibrary(const std::filesystem::path& path, std::string& error) {
    HMODULE h = LoadLibraryW(path.c_str());
    if (!h) error = "LoadLibrary failed (error " + std::to_string(GetLastError()) + ")";
    return reinterpret_cast<void*>(h);
}
void* findSymbol(void* handle, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
}
void closeLibrary(void* handle) { FreeLibrary(static_cast<HMODULE>(handle)); }
#else
void* openLibrary(const std::filesystem::path& path, std::string& error) {
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* why = dlerror();
        error = why ? why : "dlopen failed";
    }
    return h;
}
void* findSymbol(void* handle, const char* name) { return dlsym(handle, name); }
void closeLibrary(void* handle) { dlclose(handle); }
#endif

} // namespace

std::string rvText(CK_RV rv) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "PKCS#11 error 0x%08lX", static_cast<unsigned long>(rv));
    return buf;
}

CryptoError mapRv(CK_RV rv, std::string_view what) {
    CryptoErrorCode code = CryptoErrorCode::Io;
    switch (rv) {
    case CKR_TOKEN_NOT_PRESENT:
    case CKR_DEVICE_REMOVED:
    case CKR_SLOT_ID_INVALID:
        code = CryptoErrorCode::DeviceRemoved;
        break;
    case CKR_SESSION_HANDLE_INVALID:
    case CKR_SESSION_CLOSED:
    case CKR_USER_NOT_LOGGED_IN:
        code = CryptoErrorCode::SessionExpired;
        break;
    case CKR_KEY_HANDLE_INVALID:
        code = CryptoErrorCode::NoPrivateKey;
        break;
    case CKR_MECHANISM_INVALID:
    case CKR_MECHANISM_PARAM_INVALID:
    case CKR_FUNCTION_NOT_SUPPORTED:
        code = CryptoErrorCode::UnsupportedAlgorithm;
        break;
    case CKR_PIN_LOCKED:
        code = CryptoErrorCode::PinLocked;
        break;
    case CKR_PIN_INCORRECT:
        code = CryptoErrorCode::WrongPassword;
        break;
    default:
        break;
    }
    std::string message(what);
    message += ": ";
    message += rvText(rv);
    return CryptoError{code, std::move(message)};
}

std::string trimField(const unsigned char* data, std::size_t size) {
    std::size_t end = size;
    while (end > 0 && (data[end - 1] == ' ' || data[end - 1] == 0)) --end;
    std::string out;
    out.reserve(end);
    for (std::size_t i = 0; i < end; ++i) {
        const unsigned char c = data[i];
        out.push_back(c < 0x20 ? ' ' : static_cast<char>(c)); // control characters never reach a UI / log
    }
    return out;
}

// ---- module ---------------------------------------------------------------

CryptoResult<std::shared_ptr<Pkcs11Module>> Pkcs11Module::acquire(const std::filesystem::path& path) {
    if (path.empty()) return fail(CryptoErrorCode::BackendUnavailable, "no PKCS#11 module path configured");
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return fail(CryptoErrorCode::BackendUnavailable, "PKCS#11 module not found: " + path.string());
    const std::string key = path.lexically_normal().string();

    std::lock_guard lock(cacheMutex());
    if (auto it = cache().find(key); it != cache().end()) {
        if (auto alive = it->second.lock()) return alive;
        cache().erase(it);
    }
    std::shared_ptr<Pkcs11Module> module(new Pkcs11Module());
    module->key_ = key;
    if (auto ok = module->init(path); !ok) return std::unexpected(ok.error());
    module->registered_ = true;
    cache()[key] = module;
    return module;
}

CryptoOk Pkcs11Module::init(const std::filesystem::path& path) {
    std::string error;
    handle_ = openLibrary(path, error);
    if (!handle_) return fail(CryptoErrorCode::BackendUnavailable, "cannot load PKCS#11 module " + path.string() + ": " + error);

    // The only symbol resolved by name; every other entry comes from the table.
    void* symbol = findSymbol(handle_, "C_GetFunctionList");
    if (!symbol) return fail(CryptoErrorCode::BackendUnavailable, "not a PKCS#11 module (no C_GetFunctionList): " + path.string());
    CK_FUNCTION_LIST* list = nullptr;
    const CK_RV listRv = reinterpret_cast<CK_C_GetFunctionList>(symbol)(&list);
    if (listRv != CKR_OK || !list)
        return fail(CryptoErrorCode::BackendUnavailable, "C_GetFunctionList failed: " + rvText(listRv));
    if (!list->C_Initialize) return fail(CryptoErrorCode::BackendUnavailable, "module has no C_Initialize");
    list_ = list;

    CK_C_INITIALIZE_ARGS args{};
    args.flags = CKF_OS_LOCKING_OK; // the module uses its own OS locking; Rivet serialises calls anyway
    const CK_RV initRv = list_->C_Initialize(&args);
    if (initRv == CKR_OK) {
        initialized_ = true;
    } else if (initRv != CKR_CRYPTOKI_ALREADY_INITIALIZED) {
        // Another component of the process owns the module: share it, never finalize it.
        return fail(CryptoErrorCode::BackendUnavailable, "C_Initialize failed: " + rvText(initRv));
    }
    return {};
}

Pkcs11Module::~Pkcs11Module() {
    // Finalize and unload under the cache mutex so that a concurrent acquire()
    // of the same path cannot observe a half-finalized library.
    std::unique_lock<std::mutex> lock(cacheMutex(), std::defer_lock);
    if (registered_) {
        lock.lock();
        if (auto it = cache().find(key_); it != cache().end() && it->second.expired()) cache().erase(it);
    }
    if (initialized_ && list_ && list_->C_Finalize) list_->C_Finalize(nullptr);
    list_ = nullptr;
    if (handle_) closeLibrary(handle_);
}

// ---- session --------------------------------------------------------------

Pkcs11Session::~Pkcs11Session() { close(); }

Pkcs11Session::Pkcs11Session(Pkcs11Session&& other) noexcept
    : module_(std::move(other.module_)), handle_(std::exchange(other.handle_, 0)) {}

Pkcs11Session& Pkcs11Session::operator=(Pkcs11Session&& other) noexcept {
    if (this != &other) {
        close();
        module_ = std::move(other.module_);
        handle_ = std::exchange(other.handle_, 0);
    }
    return *this;
}

void Pkcs11Session::close() {
    if (handle_ && module_) module_->call(&CK_FUNCTION_LIST::C_CloseSession, handle_); // result is irrelevant here
    handle_ = 0;
    module_.reset();
}

CK_RV Pkcs11Session::open(const std::shared_ptr<Pkcs11Module>& module, CK_SLOT_ID slot, Pkcs11Session& out) {
    out.close();
    CK_SESSION_HANDLE handle = 0;
    const CK_RV rv = module->call(&CK_FUNCTION_LIST::C_OpenSession, slot, CKF_SERIAL_SESSION, static_cast<void*>(nullptr),
                                  static_cast<void*>(nullptr), &handle);
    if (rv != CKR_OK) return rv;
    if (handle == 0) return CKR_GENERAL_ERROR;
    out.module_ = module;
    out.handle_ = handle;
    return CKR_OK;
}

CK_RV Pkcs11Session::find(std::vector<CK_ATTRIBUTE> templ, std::size_t max, std::vector<CK_OBJECT_HANDLE>& out) const {
    out.clear();
    CK_RV rv = module_->call(&CK_FUNCTION_LIST::C_FindObjectsInit, handle_, templ.data(),
                             static_cast<CK_ULONG>(templ.size()));
    if (rv != CKR_OK) return rv;
    CK_RV result = CKR_OK;
    while (out.size() < max) {
        CK_OBJECT_HANDLE batch[16];
        const CK_ULONG want = static_cast<CK_ULONG>(std::min<std::size_t>(16, max - out.size()));
        CK_ULONG got = 0;
        rv = module_->call(&CK_FUNCTION_LIST::C_FindObjects, handle_, batch, want, &got);
        if (rv != CKR_OK) {
            result = rv;
            break;
        }
        if (got == 0) break;
        if (got > want) got = want; // a misbehaving module must not overrun the batch
        out.insert(out.end(), batch, batch + got);
    }
    module_->call(&CK_FUNCTION_LIST::C_FindObjectsFinal, handle_);
    return result;
}

CK_RV Pkcs11Session::getBytes(CK_OBJECT_HANDLE object, CK_ATTRIBUTE_TYPE type, Bytes& out, std::size_t maxLen) const {
    out.clear();
    CK_ATTRIBUTE attribute{type, nullptr, 0};
    CK_RV rv = module_->call(&CK_FUNCTION_LIST::C_GetAttributeValue, handle_, object, &attribute, CK_ULONG{1});
    if (rv != CKR_OK) return rv;
    if (attribute.ulValueLen == CK_UNAVAILABLE_INFORMATION) return CKR_ATTRIBUTE_TYPE_INVALID;
    if (attribute.ulValueLen > maxLen) return CKR_BUFFER_TOO_SMALL;
    if (attribute.ulValueLen == 0) return CKR_OK;
    Bytes buffer(attribute.ulValueLen);
    const CK_ULONG expected = attribute.ulValueLen;
    attribute.pValue = buffer.data();
    rv = module_->call(&CK_FUNCTION_LIST::C_GetAttributeValue, handle_, object, &attribute, CK_ULONG{1});
    if (rv != CKR_OK) return rv;
    if (attribute.ulValueLen == CK_UNAVAILABLE_INFORMATION || attribute.ulValueLen > expected) return CKR_ATTRIBUTE_TYPE_INVALID;
    buffer.resize(attribute.ulValueLen);
    out = std::move(buffer);
    return CKR_OK;
}

CK_RV Pkcs11Session::getUlong(CK_OBJECT_HANDLE object, CK_ATTRIBUTE_TYPE type, CK_ULONG& out) const {
    CK_ULONG value = 0;
    CK_ATTRIBUTE attribute{type, &value, sizeof value};
    const CK_RV rv = module_->call(&CK_FUNCTION_LIST::C_GetAttributeValue, handle_, object, &attribute, CK_ULONG{1});
    if (rv != CKR_OK) return rv;
    if (attribute.ulValueLen != sizeof value) return CKR_ATTRIBUTE_TYPE_INVALID;
    out = value;
    return CKR_OK;
}

CK_RV Pkcs11Session::getBool(CK_OBJECT_HANDLE object, CK_ATTRIBUTE_TYPE type, bool& out) const {
    CK_BBOOL value = 0;
    CK_ATTRIBUTE attribute{type, &value, sizeof value};
    const CK_RV rv = module_->call(&CK_FUNCTION_LIST::C_GetAttributeValue, handle_, object, &attribute, CK_ULONG{1});
    if (rv != CKR_OK) return rv;
    if (attribute.ulValueLen != sizeof value) return CKR_ATTRIBUTE_TYPE_INVALID;
    out = value != 0;
    return CKR_OK;
}

} // namespace rivet::crypto::p11
