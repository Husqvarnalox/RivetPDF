// SPDX-License-Identifier: MPL-2.0
// libFuzzer entry point. Compiled once per target with RIVET_FUZZ_FN set to the
// target function (see CMakeLists.txt); only built with RIVET_BUILD_FUZZERS=ON.
#include "FuzzTargets.hpp"

#ifndef RIVET_FUZZ_FN
#error "RIVET_FUZZ_FN must name the fuzz target function"
#endif

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    return rivet::fuzz::RIVET_FUZZ_FN(data, size);
}
