# Rivet sanitizer configuration.
#
# rivet_set_target_sanitizers(<target>)
# When RIVET_ENABLE_SANITIZERS is ON, builds the target with
# AddressSanitizer + UndefinedBehaviorSanitizer.

function(rivet_set_target_sanitizers target)
    if(RIVET_BUILD_FUZZERS AND NOT MSVC)
        # libFuzzer build (tests/fuzz): every library is instrumented for
        # coverage, ASan, UBSan and float-cast-overflow (JSON number -> integer).
        target_compile_options(${target} PRIVATE
            -fsanitize=fuzzer-no-link,address,undefined,float-cast-overflow
            -fno-omit-frame-pointer
        )
        target_link_options(${target} PRIVATE
            -fsanitize=address,undefined,float-cast-overflow
        )
    elseif(RIVET_ENABLE_SANITIZERS AND NOT MSVC)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(${target} PRIVATE
            -fsanitize=address,undefined
        )
    elseif(RIVET_ENABLE_SANITIZERS AND MSVC)
        target_compile_options(${target} PRIVATE /fsanitize=address)
    endif()
endfunction()
