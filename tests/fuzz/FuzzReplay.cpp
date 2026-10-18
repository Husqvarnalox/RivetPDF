// SPDX-License-Identifier: MPL-2.0
// Deterministic corpus replay + bounded mutation runner. It needs no libFuzzer,
// so it runs in every build (GCC, MSVC, sanitizer builds) as a ctest:
//
//   rivet_fuzz_replay <target> [iterations]     replay the seeds, then mutate
//   rivet_fuzz_replay --dump-corpus <dir> <target>   write the seeds as files
//   rivet_fuzz_replay --list
//
// Iterations default to RIVET_FUZZ_ITERATIONS or a per-target value. The
// mutation stream is a fixed-seed PRNG, so a failure reproduces exactly.
#include "FuzzTargets.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>

namespace rivet::fuzz {

std::span<const Target> allTargets() {
    static const Target targets[] = {
        {"der", fuzzDer, seedsDer},
        {"x509", fuzzX509, seedsX509},
        {"cms", fuzzCms, seedsCms},
        {"timestamp", fuzzTimestamp, seedsTimestamp},
        {"revocation", fuzzRevocation, seedsRevocation},
        {"pdfsig", fuzzPdfSignature, seedsPdfSignature},
        {"pdfencrypt", fuzzPdfEncrypt, seedsPdfEncrypt},
        {"net", fuzzNet, seedsNet},
        {"json", fuzzJson, seedsJson},
    };
    return targets;
}

} // namespace rivet::fuzz

namespace {

using rivet::fuzz::Bytes;

struct Rng {
    std::uint64_t s = 0x9E3779B97F4A7C15ull;
    std::uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    std::size_t below(std::size_t n) { return n ? static_cast<std::size_t>(next() % n) : 0; }
};

constexpr std::uint8_t kInteresting[] = {0x00, 0x01, 0x7F, 0x80, 0x81, 0xFF, 0x30, 0x31, 0x04, 0x06, 0x02, 0x05, 0xA0, 0x84, 0x82};

Bytes mutate(const Bytes& seed, const std::vector<Bytes>& pool, Rng& rng, std::size_t maxLen) {
    Bytes b = seed;
    const int rounds = 1 + static_cast<int>(rng.below(4));
    for (int r = 0; r < rounds; ++r) {
        switch (rng.below(9)) {
        case 0:
            if (!b.empty()) b[rng.below(b.size())] ^= static_cast<std::uint8_t>(1u << rng.below(8));
            break;
        case 1:
            if (!b.empty()) b[rng.below(b.size())] = static_cast<std::uint8_t>(rng.next());
            break;
        case 2:
            if (!b.empty()) b[rng.below(b.size())] = kInteresting[rng.below(sizeof kInteresting)];
            break;
        case 3: // insert random bytes
            b.insert(b.begin() + static_cast<std::ptrdiff_t>(rng.below(b.size() + 1)), 1 + rng.below(4),
                     static_cast<std::uint8_t>(rng.next()));
            break;
        case 4: // delete a range
            if (!b.empty()) {
                const std::size_t at = rng.below(b.size());
                b.erase(b.begin() + static_cast<std::ptrdiff_t>(at),
                        b.begin() + static_cast<std::ptrdiff_t>(std::min(b.size(), at + 1 + rng.below(16))));
            }
            break;
        case 5: // truncate
            if (!b.empty()) b.resize(rng.below(b.size()));
            break;
        case 6: // duplicate a chunk
            if (!b.empty()) {
                const std::size_t at = rng.below(b.size());
                const std::size_t n = std::min<std::size_t>(b.size() - at, 1 + rng.below(32));
                const Bytes chunk(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + n));
                b.insert(b.begin() + static_cast<std::ptrdiff_t>(rng.below(b.size() + 1)), chunk.begin(), chunk.end());
            }
            break;
        case 7: // splice from another seed
            if (!pool.empty()) {
                const Bytes& o = pool[rng.below(pool.size())];
                if (!o.empty()) {
                    const std::size_t at = rng.below(o.size());
                    const std::size_t n = std::min<std::size_t>(o.size() - at, 1 + rng.below(64));
                    b.insert(b.begin() + static_cast<std::ptrdiff_t>(rng.below(b.size() + 1)),
                             o.begin() + static_cast<std::ptrdiff_t>(at), o.begin() + static_cast<std::ptrdiff_t>(at + n));
                }
            }
            break;
        default: // overwrite a 2-4 byte window with a boundary length
            if (b.size() >= 4) {
                const std::size_t at = rng.below(b.size() - 3);
                static constexpr std::uint8_t kLens[][4] = {{0xFF, 0xFF, 0xFF, 0xFF}, {0x7F, 0xFF, 0xFF, 0xFF}, {0x00, 0x00, 0x00, 0x00}, {0x80, 0x00, 0x00, 0x00}, {0x84, 0x7F, 0xFF, 0xFF}};
                std::memcpy(&b[at], kLens[rng.below(5)], 4);
            }
            break;
        }
    }
    if (b.size() > maxLen) b.resize(maxLen);
    return b;
}

std::size_t defaultIterations(std::string_view target) {
    if (target == "cms" || target == "timestamp" || target == "revocation") return 1500;
    if (target == "pdfsig") return 400;
    if (target == "pdfencrypt") return 1500;
    return 6000;
}

int runOne(const rivet::fuzz::Target& t, const Bytes& input, const char* phase, std::size_t index) {
    try {
        t.run(input.data(), input.size());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[%s] %s #%zu (%zu bytes): uncaught exception: %s\n", t.name.data(), phase, index, input.size(), e.what());
    } catch (...) {
        std::fprintf(stderr, "[%s] %s #%zu (%zu bytes): uncaught non-standard exception\n", t.name.data(), phase, index, input.size());
    }
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    using namespace rivet::fuzz;
    if (argc >= 2 && std::strcmp(argv[1], "--list") == 0) {
        for (const Target& t : allTargets()) std::printf("%s\n", t.name.data());
        return 0;
    }
    const bool dump = argc >= 4 && std::strcmp(argv[1], "--dump-corpus") == 0;
    const char* name = dump ? argv[3] : argc >= 2 ? argv[1] : nullptr;
    if (!name) {
        std::fprintf(stderr, "usage: %s <target> [iterations] | --dump-corpus <dir> <target> | --list\n", argv[0]);
        return 2;
    }
    const Target* target = nullptr;
    for (const Target& t : allTargets())
        if (t.name == name) target = &t;
    if (!target) {
        std::fprintf(stderr, "unknown target: %s\n", name);
        return 2;
    }

    const std::vector<Bytes> seeds = target->seeds();
    if (seeds.empty()) {
        std::fprintf(stderr, "[%s] no seeds were produced\n", name);
        return 1;
    }
    if (dump) {
        std::filesystem::create_directories(argv[2]);
        for (std::size_t i = 0; i < seeds.size(); ++i) {
            std::ofstream f(std::filesystem::path(argv[2]) / (std::string(name) + "-" + std::to_string(i)), std::ios::binary);
            f.write(reinterpret_cast<const char*>(seeds[i].data()), static_cast<std::streamsize>(seeds[i].size()));
        }
        std::printf("[%s] wrote %zu seeds to %s\n", name, seeds.size(), argv[2]);
        return 0;
    }

    std::size_t iterations = defaultIterations(target->name);
    if (const char* env = std::getenv("RIVET_FUZZ_ITERATIONS")) iterations = static_cast<std::size_t>(std::strtoull(env, nullptr, 10));
    if (argc >= 3) iterations = static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10));

    const auto begin = std::chrono::steady_clock::now();
    int failures = 0;
    for (std::size_t i = 0; i < seeds.size(); ++i) failures += runOne(*target, seeds[i], "seed", i);
    if (failures) return 1;

    Rng rng;
    std::size_t maxLen = 1 << 16;
    for (const Bytes& s : seeds) maxLen = std::max(maxLen, s.size() + 256);
    for (std::size_t i = 0; i < iterations; ++i) {
        const Bytes input = mutate(seeds[i % seeds.size()], seeds, rng, maxLen);
        failures += runOne(*target, input, "mutation", i);
        if (failures) {
            std::fprintf(stderr, "[%s] stopping after the first failure\n", name);
            return 1;
        }
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    std::printf("[%s] %zu seeds + %zu mutations ok in %.1fs\n", name, seeds.size(), iterations, secs);
    return 0;
}
