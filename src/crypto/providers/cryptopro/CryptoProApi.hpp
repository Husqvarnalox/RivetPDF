// SPDX-License-Identifier: MPL-2.0
#pragma once

// Run-time bound CryptoPro CSP API (CAPI10 + CAPI20 "CAPILite"). Private to
// this directory: it includes the proprietary SDK headers, which are never
// vendored and never reach a public Rivet header.

#include <cstdint>
#include <memory>
#include <string>

#include "cpcsp/WinCryptEx.h"

namespace rivet::crypto::cryptopro_detail {

struct CspApi {
    // CAPI10 (libcapi10)
    decltype(&CryptAcquireContextA) acquireContext = nullptr;
    decltype(&CryptReleaseContext) releaseContext = nullptr;
    decltype(&CryptGetProvParam) getProvParam = nullptr;
    decltype(&CryptSetProvParam) setProvParam = nullptr;
    decltype(&CryptCreateHash) createHash = nullptr;
    decltype(&CryptHashData) hashData = nullptr;
    decltype(&CryptGetHashParam) getHashParam = nullptr;
    decltype(&CryptSetHashParam) setHashParam = nullptr;
    decltype(&CryptDestroyHash) destroyHash = nullptr;
    decltype(&CryptSignHashA) signHash = nullptr;
    decltype(&CryptVerifySignatureA) verifySignature = nullptr;
    decltype(&CryptGetUserKey) getUserKey = nullptr;
    decltype(&CryptDestroyKey) destroyKey = nullptr;
    decltype(&CryptGenRandom) genRandom = nullptr;
    DWORD (*getLastError)() = nullptr; // exported by libcapi10 on Unix
    // CAPI20 (libcapi20)
    decltype(&CertOpenSystemStoreA) openSystemStore = nullptr;
    decltype(&CertCloseStore) closeStore = nullptr;
    decltype(&CertEnumCertificatesInStore) enumCertificates = nullptr;
    decltype(&CertGetCertificateContextProperty) getCertificateProperty = nullptr;
    decltype(&CertCreateCertificateContext) createCertificateContext = nullptr;
    decltype(&CertFreeCertificateContext) freeCertificateContext = nullptr;
    decltype(&CryptImportPublicKeyInfo) importPublicKeyInfo = nullptr;

    void* handle10 = nullptr;
    void* handle20 = nullptr;

    ~CspApi();
    CspApi() = default;
    CspApi(const CspApi&) = delete;
    CspApi& operator=(const CspApi&) = delete;

    // Probes `libraryDirectory` (or the standard /opt/cprocsp/lib locations)
    // and resolves every entry point. Null + `reason` on failure.
    // Each library file must pass crypto::checkModuleFile before it is dlopen'ed unless
    // allowInsecure (development override).
    static std::shared_ptr<const CspApi> load(const std::string& libraryDirectory, std::string& reason,
                                              bool allowInsecure = false);
};

} // namespace rivet::crypto::cryptopro_detail
