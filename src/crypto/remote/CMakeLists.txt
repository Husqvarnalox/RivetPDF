# Cloud / remote signing (ADR-0047). The generic abstraction (RemoteSigningProvider,
# RemoteSigningIdentity, RemoteIdentityStore) lives in rivet_crypto; this library
# holds the network-facing adapters. Everything goes through rivet::net
# (consent gate, SSRF policy, https-only for credentials, size / time limits).
#
# The CryptoPro DSS adapter is built and tested against an in-process MOCK
# service only. It has never talked to a real DSS (ADR-0047, "Assumptions").

add_library(rivet_remote_signing STATIC
    JsonLite.cpp
    CryptoProDss.cpp
)
add_library(rivet::remote_signing ALIAS rivet_remote_signing)

target_include_directories(rivet_remote_signing PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(rivet_remote_signing PUBLIC rivet::crypto rivet::net rivet::core)

rivet_set_target_warnings(rivet_remote_signing)
rivet_set_target_sanitizers(rivet_remote_signing)
