# Generic PKCS#11 provider. The token vendor module is dlopen'ed at run time
# (path configured by the application), so there is no link-time dependency;
# RIVET_ENABLE_PKCS11=OFF compiles only the "unavailable" factory.
target_sources(rivet_crypto_providers PRIVATE
    Pkcs11IdentityStore.cpp
)
if(RIVET_ENABLE_PKCS11)
    target_sources(rivet_crypto_providers PRIVATE Pkcs11Module.cpp)
    target_compile_definitions(rivet_crypto_providers PUBLIC RIVET_CRYPTO_PKCS11=1)
    target_link_libraries(rivet_crypto_providers PRIVATE ${CMAKE_DL_LIBS})
endif()
