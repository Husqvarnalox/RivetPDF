# Rivet compiler warning configuration.
#
# rivet_set_target_warnings(<target>)
# Applies the shared Rivet warning set to a target. Warnings are errors-
#Candidates off by default: the build must stay clean, but CI/compiler
# differences should not break developer builds.

function(rivet_set_target_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /w14242
            /w14254
            /w14263
            /w14265
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
