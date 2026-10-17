# RFC 3161 time-stamp client. Parsing, verification and the CMS integration
# live in rivet_crypto (no networking there); only the HTTP exchange is here,
# and it goes through rivet::net exclusively (online-validation gate, SSRF
# policy, size / time limits).

add_library(rivet_timestamp STATIC
    TimeStampClient.cpp
)
add_library(rivet::timestamp ALIAS rivet_timestamp)

target_include_directories(rivet_timestamp PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(rivet_timestamp PUBLIC rivet::crypto rivet::net rivet::core)

rivet_set_target_warnings(rivet_timestamp)
rivet_set_target_sanitizers(rivet_timestamp)
