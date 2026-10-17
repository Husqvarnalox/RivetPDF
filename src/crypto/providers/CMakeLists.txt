# Rivet crypto providers: identity stores and signing backends that sit behind
# the rivet::crypto interfaces (IdentityStore, SigningIdentity, CryptoProvider).
#
#   cryptopro/  CryptoPro CSP (GOST R 34.10/34.11-2012), RIVET_ENABLE_CRYPTOPRO
#   pkcs11/     generic PKCS#11 tokens (Rutoken, JaCarta, SoftHSM), RIVET_ENABLE_PKCS11
#   platform/   macOS Security.framework / Windows CNG certificate store
#
# Every provider always compiles its factory; with its option OFF (or the
# backend not installed) the factory returns a store whose isAvailable() is
# false and whose operations fail with BackendUnavailable.

add_library(rivet_crypto_providers STATIC)
add_library(rivet::crypto_providers ALIAS rivet_crypto_providers)

target_include_directories(rivet_crypto_providers PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(rivet_crypto_providers PUBLIC rivet::crypto)

add_subdirectory(cryptopro)
add_subdirectory(pkcs11)
add_subdirectory(platform)

rivet_set_target_warnings(rivet_crypto_providers)
rivet_set_target_sanitizers(rivet_crypto_providers)
