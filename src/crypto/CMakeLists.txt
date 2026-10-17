# Rivet crypto layer: typed algorithms, Rivet-owned CMS model, provider
# interfaces and (optionally) the OpenSSL 3.x/4.x software provider.
#
# RIVET_ENABLE_CRYPTO=OFF (default): the OpenSSL provider is not built; the
# null provider keeps everything linking, and every operation reports
# BackendUnavailable. The CMS parser/encoder and the policy code are
# backend-free and always built.

option(RIVET_ENABLE_CRYPTO "Build the OpenSSL-backed crypto provider (signatures, PKCS#12)" OFF)

add_library(rivet_crypto STATIC
    Algorithms.cpp
    Certificate.cpp
    PathBuilder.cpp
    Cms.cpp
    Der.cpp
    Error.cpp
    NullProvider.cpp
    Providers.cpp
    RemoteIdentity.cpp
    SecretBuffer.cpp
    Validation.cpp
    X509Parse.cpp
    Qualification.cpp
    CertificateDisplay.cpp
    timestamp/TimeStampRequest.cpp
    timestamp/TimeStampToken.cpp
)
add_library(rivet::crypto ALIAS rivet_crypto)

target_include_directories(rivet_crypto PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(rivet_crypto PUBLIC rivet::core)

if(RIVET_ENABLE_CRYPTO)
    find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
    target_sources(rivet_crypto PRIVATE openssl/OpenSslProvider.cpp openssl/OpenSslSymmetric.cpp)
    target_compile_definitions(rivet_crypto PUBLIC RIVET_CRYPTO_OPENSSL=1)
    target_link_libraries(rivet_crypto PRIVATE OpenSSL::Crypto)
    message(STATUS "Rivet: crypto backend OpenSSL ${OPENSSL_VERSION}")
else()
    target_sources(rivet_crypto PRIVATE SymmetricNull.cpp)
    message(STATUS "Rivet: crypto backend OFF (null provider; RIVET_ENABLE_CRYPTO=OFF)")
endif()

# Optional provider backends (default OFF; see src/crypto/providers).
option(RIVET_ENABLE_CRYPTOPRO "Build the CryptoPro CSP provider (needs the CSP SDK headers; the library is dlopen'ed at run time)" OFF)
option(RIVET_ENABLE_PKCS11 "Build the PKCS#11 token provider (module is dlopen'ed at run time)" OFF)
add_subdirectory(providers)
# RFC 3161 client (needs rivet::net); the request/token code is part of rivet_crypto.
add_subdirectory(timestamp)
# OCSP / CRL clients, revocation evidence and policy (needs rivet::net).
add_subdirectory(revocation)
# Cloud / remote signing adapters (CryptoPro DSS, mock-verified only; needs rivet::net).
add_subdirectory(remote)

rivet_set_target_warnings(rivet_crypto)
rivet_set_target_sanitizers(rivet_crypto)

# Diagnostic tool: dumps a CMS SignedData file. Not shipped in the app.
add_executable(rivet-signature-inspect tools/SignatureInspectMain.cpp)
target_link_libraries(rivet-signature-inspect PRIVATE rivet::crypto)
rivet_set_target_warnings(rivet-signature-inspect)
rivet_set_target_sanitizers(rivet-signature-inspect)
