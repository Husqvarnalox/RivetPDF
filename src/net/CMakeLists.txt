# Rivet network layer: URL parser, network policy (SSRF guard), consent gate,
# redirect-driving client and transports. Nothing in the app touches the
# network except through NetworkClient.
#
# RIVET_ENABLE_NETWORK=OFF (default): the libcurl transport is not built;
# makeDefaultTransport() returns a transport that reports BackendUnavailable.
# Everything else (parser, policy, client, mock transport) is backend-free.

option(RIVET_ENABLE_NETWORK "Build the libcurl HTTP transport" OFF)

add_library(rivet_net STATIC
    Client.cpp
    Consent.cpp
    DefaultTransport.cpp
    Error.cpp
    Http.cpp
    Ip.cpp
    MockTransport.cpp
    Policy.cpp
    Url.cpp
)
add_library(rivet::net ALIAS rivet_net)

target_include_directories(rivet_net PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(rivet_net PUBLIC rivet::core)

if(RIVET_ENABLE_NETWORK)
    find_package(CURL REQUIRED)
    target_sources(rivet_net PRIVATE curl/CurlTransport.cpp)
    target_compile_definitions(rivet_net PUBLIC RIVET_NET_CURL=1)
    target_link_libraries(rivet_net PRIVATE CURL::libcurl)
    message(STATUS "Rivet: network backend libcurl ${CURL_VERSION_STRING}")
else()
    message(STATUS "Rivet: network backend OFF (RIVET_ENABLE_NETWORK=OFF)")
endif()

rivet_set_target_warnings(rivet_net)
rivet_set_target_sanitizers(rivet_net)
