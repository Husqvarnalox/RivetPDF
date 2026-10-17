# CryptoPro CSP provider (CAPILite / CryptoAPI 2.0 for Unix and Windows).
#
# Built only with RIVET_ENABLE_CRYPTOPRO=ON, which needs the CryptoPro CSP SDK
# *headers* (never vendored, proprietary): RIVET_CRYPTOPRO_ROOT, default
# /opt/cprocsp on Unix. The CSP *libraries* are not linked: they are loaded
# with dlopen / LoadLibrary at run time, so one Rivet binary works with or
# without an installed CSP (ADR-0040).
target_sources(rivet_crypto_providers PRIVATE
    CryptoProIdentityStore.cpp
)

if(RIVET_ENABLE_CRYPTOPRO)
    set(RIVET_CRYPTOPRO_ROOT "/opt/cprocsp" CACHE PATH "CryptoPro CSP installation root (SDK headers under include/)")
    if(NOT EXISTS "${RIVET_CRYPTOPRO_ROOT}/include/cpcsp/WinCryptEx.h")
        message(FATAL_ERROR "RIVET_ENABLE_CRYPTOPRO=ON but ${RIVET_CRYPTOPRO_ROOT}/include/cpcsp/WinCryptEx.h was not found; set RIVET_CRYPTOPRO_ROOT")
    endif()
    target_sources(rivet_crypto_providers PRIVATE
        CryptoProApi.cpp
        CryptoProBackend.cpp
    )
    target_compile_definitions(rivet_crypto_providers PUBLIC RIVET_CRYPTO_CRYPTOPRO=1)
    # SDK headers stay private to the backend translation units.
    set_source_files_properties(CryptoProApi.cpp CryptoProBackend.cpp PROPERTIES
        COMPILE_FLAGS "-I${RIVET_CRYPTOPRO_ROOT}/include -I${RIVET_CRYPTOPRO_ROOT}/include/cpcsp"
        COMPILE_DEFINITIONS "UNIX;SIZEOF_VOID_P=${CMAKE_SIZEOF_VOID_P}")
    target_link_libraries(rivet_crypto_providers PRIVATE ${CMAKE_DL_LIBS})
    message(STATUS "Rivet: CryptoPro CSP provider ON (headers: ${RIVET_CRYPTOPRO_ROOT}/include)")
else()
    message(STATUS "Rivet: CryptoPro CSP provider OFF (RIVET_ENABLE_CRYPTOPRO=OFF)")
endif()
