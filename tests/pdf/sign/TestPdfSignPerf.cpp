// SPDX-License-Identifier: MPL-2.0
// Slow test: a synthetic 100+ MB PDF is signed and verified through the file API.

#include "RivetTest.h"

#include "SignTestSupport.hpp"

#include <chrono>
#include <fstream>

using namespace rivet::pdf::sign;
using namespace rivet::pdf::sign::testing;
namespace crypto = rivet::crypto;

RIVET_TEST(largeFileSignAndVerify) {
    Signer s;
    const auto dir = scratchDir("perf");
    const Bytes base = makeFixture({.contentPadding = std::size_t{110} * 1024 * 1024});
    CHECK(base.size() > std::size_t{100} * 1024 * 1024);
    const auto in = dir / "in.pdf";
    const auto out = dir / "out.pdf";
    {
        std::ofstream f(in, std::ios::binary);
        f.write(reinterpret_cast<const char*>(base.data()), static_cast<std::streamsize>(base.size()));
    }
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    auto signedPdf = signPdfFile(in, out, basicRequest(), *s.provider, *s.identity);
    const auto t1 = clock::now();
    CHECK(signedPdf.has_value());
    if (!signedPdf) return;
    Bytes written;
    {
        std::ifstream f(out, std::ios::binary);
        { char blk_[8192]; while (f.read(blk_, sizeof blk_) || f.gcount() > 0) written.insert(written.end(), blk_, blk_ + f.gcount()); }
    }
    const auto sigs = verifyAll(written, s, true);
    const auto t2 = clock::now();
    CHECK_EQ(sigs.size(), std::size_t{1});
    if (!sigs.empty()) CHECK(sigs[0].validation.state == crypto::SignatureState::ValidTrusted);
    CHECK(written.size() > base.size());
    const auto ms = [](auto a, auto b) { return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count(); };
    std::printf("    %zu MB: sign+file %lld ms, verify %lld ms\n", base.size() >> 20, static_cast<long long>(ms(t0, t1)),
                static_cast<long long>(ms(t1, t2)));
    CHECK(ms(t0, t2) < 120000);
}
