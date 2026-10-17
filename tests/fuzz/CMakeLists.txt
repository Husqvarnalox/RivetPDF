# Fuzz harnesses for the Phase 8 attack surface (see docs/security/phase8-review.md).
#
#  * rivet_fuzz_replay (always built, needs RIVET_ENABLE_CRYPTO for the seed
#    builders): replays generated seed corpora and a bounded, deterministic
#    mutation loop per target. No libFuzzer needed: runs on GCC, MSVC, Apple
#    clang and under ASan/UBSan/TSan builds. One ctest per target.
#  * fuzz_<target> (only with -DRIVET_BUILD_FUZZERS=ON, clang): the libFuzzer
#    binaries. Typical run:
#      rivet_fuzz_replay --dump-corpus corpus/der der
#      fuzz_der -max_len=65536 -max_total_time=60 corpus/der

find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)

add_library(rivet_fuzz_targets STATIC FuzzTargets.cpp)
target_include_directories(rivet_fuzz_targets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rivet_fuzz_targets PUBLIC
    rivet::crypto rivet::timestamp rivet::revocation rivet::remote_signing
    rivet::pdf_sign rivet::pdf_encrypt rivet::net)
rivet_set_target_warnings(rivet_fuzz_targets)
rivet_set_target_sanitizers(rivet_fuzz_targets)

add_executable(rivet_fuzz_replay FuzzReplay.cpp FuzzSeeds.cpp)
target_include_directories(rivet_fuzz_replay PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/crypto
    ${CMAKE_SOURCE_DIR}/tests/crypto/revocation
    ${CMAKE_SOURCE_DIR}/tests/pdf/sign
    ${CMAKE_SOURCE_DIR}/tests/pdf/encrypt)
target_link_libraries(rivet_fuzz_replay PRIVATE rivet_fuzz_targets OpenSSL::Crypto)
rivet_set_target_warnings(rivet_fuzz_replay)
rivet_set_target_sanitizers(rivet_fuzz_replay)

set(RIVET_FUZZ_TARGET_NAMES der x509 cms timestamp revocation pdfsig pdfencrypt net json)
set(RIVET_FUZZ_FN_der fuzzDer)
set(RIVET_FUZZ_FN_x509 fuzzX509)
set(RIVET_FUZZ_FN_cms fuzzCms)
set(RIVET_FUZZ_FN_timestamp fuzzTimestamp)
set(RIVET_FUZZ_FN_revocation fuzzRevocation)
set(RIVET_FUZZ_FN_pdfsig fuzzPdfSignature)
set(RIVET_FUZZ_FN_pdfencrypt fuzzPdfEncrypt)
set(RIVET_FUZZ_FN_net fuzzNet)
set(RIVET_FUZZ_FN_json fuzzJson)

foreach(_name IN LISTS RIVET_FUZZ_TARGET_NAMES)
    add_test(NAME fuzz_replay_${_name} COMMAND rivet_fuzz_replay ${_name})
    set_tests_properties(fuzz_replay_${_name} PROPERTIES LABELS fuzz TIMEOUT 180)
endforeach()
# Longer deterministic runs, kept out of the default `ctest -LE slow` gate.
foreach(_name IN LISTS RIVET_FUZZ_TARGET_NAMES)
    add_test(NAME fuzz_replay_long_${_name} COMMAND rivet_fuzz_replay ${_name} 200000)
    set_tests_properties(fuzz_replay_long_${_name} PROPERTIES LABELS "fuzz;slow" TIMEOUT 1800)
endforeach()

if(RIVET_BUILD_FUZZERS)
    foreach(_name IN LISTS RIVET_FUZZ_TARGET_NAMES)
        add_executable(fuzz_${_name} FuzzEntry.cpp)
        target_compile_definitions(fuzz_${_name} PRIVATE RIVET_FUZZ_FN=${RIVET_FUZZ_FN_${_name}})
        target_compile_options(fuzz_${_name} PRIVATE -fsanitize=fuzzer)
        target_link_options(fuzz_${_name} PRIVATE -fsanitize=fuzzer)
        target_link_libraries(fuzz_${_name} PRIVATE rivet_fuzz_targets)
        rivet_set_target_warnings(fuzz_${_name})
        rivet_set_target_sanitizers(fuzz_${_name})
    endforeach()
endif()
