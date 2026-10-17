// SPDX-License-Identifier: MPL-2.0
#include "crypto/providers/cryptopro/CryptoProApi.hpp"

#include <dlfcn.h>

#include <vector>

namespace rivet::crypto::cryptopro_detail {

namespace {

#if defined(__APPLE__)
constexpr const char* kExt = ".dylib";
#else
constexpr const char* kExt = ".so";
#endif

void* openLibrary(const std::string& dir, const char* base) {
    return dlopen((dir + "/" + base + kExt).c_str(), RTLD_NOW | RTLD_LOCAL);
}

template <typename Fn>
bool bind(void* handle, const char* symbol, Fn& out) {
    out = reinterpret_cast<Fn>(dlsym(handle, symbol));
    return out != nullptr;
}

} // namespace

CspApi::~CspApi() {
    // CSP libraries keep process-wide state; unloading them is not safe.
    // The handles are intentionally leaked for the process lifetime.
}

std::shared_ptr<const CspApi> CspApi::load(const std::string& libraryDirectory, std::string& reason) {
    std::vector<std::string> dirs;
    if (!libraryDirectory.empty()) {
        // A relative directory would be resolved against the working directory (library planting).
        if (libraryDirectory.front() != '/') {
            reason = "CryptoProOptions::libraryDirectory must be an absolute path";
            return nullptr;
        }
        dirs.push_back(libraryDirectory);
    } else {
        for (const char* d : {"/opt/cprocsp/lib", "/opt/cprocsp/lib/aarch64", "/opt/cprocsp/lib/amd64",
                              "/opt/cprocsp/lib/ia32", "/opt/cprocsp/lib/arm64"}) {
            dirs.emplace_back(d);
        }
    }
    auto api = std::make_shared<CspApi>();
    for (const auto& dir : dirs) {
        api->handle10 = openLibrary(dir, "libcapi10");
        if (!api->handle10) continue;
        api->handle20 = openLibrary(dir, "libcapi20");
        if (api->handle20) break;
        api->handle10 = nullptr;
    }
    if (!api->handle10 || !api->handle20) {
        reason = "CryptoPro CSP libraries (libcapi10/libcapi20) not found; install the CSP or set CryptoProOptions::libraryDirectory";
        return nullptr;
    }
    bool ok = true;
    ok &= bind(api->handle10, "CryptAcquireContextA", api->acquireContext);
    ok &= bind(api->handle10, "CryptReleaseContext", api->releaseContext);
    ok &= bind(api->handle10, "CryptGetProvParam", api->getProvParam);
    ok &= bind(api->handle10, "CryptSetProvParam", api->setProvParam);
    ok &= bind(api->handle10, "CryptCreateHash", api->createHash);
    ok &= bind(api->handle10, "CryptHashData", api->hashData);
    ok &= bind(api->handle10, "CryptGetHashParam", api->getHashParam);
    ok &= bind(api->handle10, "CryptSetHashParam", api->setHashParam);
    ok &= bind(api->handle10, "CryptDestroyHash", api->destroyHash);
    ok &= bind(api->handle10, "CryptSignHashA", api->signHash);
    ok &= bind(api->handle10, "CryptVerifySignatureA", api->verifySignature);
    ok &= bind(api->handle10, "CryptGetUserKey", api->getUserKey);
    ok &= bind(api->handle10, "CryptDestroyKey", api->destroyKey);
    ok &= bind(api->handle10, "CryptGenRandom", api->genRandom);
    ok &= bind(api->handle10, "GetLastError", api->getLastError);
    ok &= bind(api->handle20, "CertOpenSystemStoreA", api->openSystemStore);
    ok &= bind(api->handle20, "CertCloseStore", api->closeStore);
    ok &= bind(api->handle20, "CertEnumCertificatesInStore", api->enumCertificates);
    ok &= bind(api->handle20, "CertGetCertificateContextProperty", api->getCertificateProperty);
    ok &= bind(api->handle20, "CertCreateCertificateContext", api->createCertificateContext);
    ok &= bind(api->handle20, "CertFreeCertificateContext", api->freeCertificateContext);
    ok &= bind(api->handle20, "CryptImportPublicKeyInfo", api->importPublicKeyInfo);
    if (!ok) {
        reason = "CryptoPro CSP libraries are present but lack a required entry point";
        return nullptr;
    }
    return api;
}

} // namespace rivet::crypto::cryptopro_detail
