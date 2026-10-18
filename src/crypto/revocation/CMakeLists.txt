# OCSP (RFC 6960) and CRL (RFC 5280) clients, revocation evidence model and
# policy. Parsing/verification use the rivet_crypto provider API; every fetch
# goes through rivet::net (online-validation gate, SSRF policy, size / time
# limits). rivet_crypto itself does not depend on rivet_net.

add_library(rivet_revocation STATIC
    Common.cpp
    CertExtensions.cpp
    Evidence.cpp
    Ocsp.cpp
    Crl.cpp
    RevocationChecker.cpp
    AiaFetcher.cpp
)
add_library(rivet::revocation ALIAS rivet_revocation)

target_include_directories(rivet_revocation PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(rivet_revocation PUBLIC rivet::crypto rivet::net rivet::core)

rivet_set_target_warnings(rivet_revocation)
rivet_set_target_sanitizers(rivet_revocation)
