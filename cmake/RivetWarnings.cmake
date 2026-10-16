# Rivet compiler warning configuration.
#
# rivet_set_target_warnings(<target>)
# Applies the shared Rivet warning set to a target. Warnings are errors-
#Candidates off by default: the build must stay clean, but CI/compiler
# differences should not break developer builds.

function(rivet_set_target_warnings target)
    if(MSVC)
        # The GCC/Clang set below defines the Rivet warning policy; it does
        # not include -Wconversion. MSVC's implicit-conversion family
        # (C4244/C4267/C4245/C4305/C4242/C4254) is therefore disabled for
        # parity, together with level-4 diagnostics that have no GCC/Clang
        # counterpart and fire on idiomatic code (C4127 constant condition,
        # C4324 alignas padding, C4702 unreachable code after inlining,
        # C4706 assignment in a parenthesized condition). C4265 is not
        # enabled: it also flags interfaces with a protected non-virtual
        # destructor, which -Wnon-virtual-dtor deliberately accepts.
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /wd4244
            /wd4267
            /wd4245
            /wd4305
            /wd4127
            /wd4324
            /wd4702
            /wd4706
            /w14263
            /w14287
            /w14296
            /w14311
            /w14545
            /w14546
            /w14547
            /w14549
            /w14555
            /w14640
            /w14826
            /w14905
            /w14906
            /w14928
        )
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wnon-virtual-dtor
            -Woverloaded-virtual
            -Wcast-align
            -Wunused
            -Wnull-dereference
            -Wdouble-promotion
            -Wformat=2
        )
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target} PRIVATE
                -Wno-unknown-warning-option
            )
        endif()
    endif()
endfunction()
