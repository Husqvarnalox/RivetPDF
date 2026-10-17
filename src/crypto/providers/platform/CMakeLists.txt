# Platform keystores: macOS Security.framework (Keychain identities) and
# Windows CNG / certificate store. Linux has no platform store: the factory
# reports "unavailable" (use PKCS#11 or files there); nothing is faked.
target_sources(rivet_crypto_providers PRIVATE
    PlatformIdentityStore.cpp
)
if(APPLE)
    target_sources(rivet_crypto_providers PRIVATE MacKeychainStore_macos.mm)
    target_compile_definitions(rivet_crypto_providers PUBLIC RIVET_CRYPTO_PLATFORM_MACOS=1)
    target_link_libraries(rivet_crypto_providers PRIVATE "-framework Security" "-framework CoreFoundation")
elseif(WIN32)
    target_sources(rivet_crypto_providers PRIVATE CngStore_windows.cpp)
    target_compile_definitions(rivet_crypto_providers PUBLIC RIVET_CRYPTO_PLATFORM_WINDOWS=1)
    target_link_libraries(rivet_crypto_providers PRIVATE crypt32 ncrypt bcrypt)
endif()
